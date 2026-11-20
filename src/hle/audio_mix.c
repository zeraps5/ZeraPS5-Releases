/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Guest sound streams (AudioOut ports, AudioOut2 ports) each queue stereo
 * float frames; the host audio thread sums the queues. */
#include "hle/audio_mix.h"

#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "core/settings.h"
#include "pal/pal.h"

#define MAX_STREAMS 32
#define QUEUE_FRAMES 8192      /* per stream: 170 ms */
#define QUEUE_KEEP_FRAMES 6144 /* more queued than this (a producer not pacing): the oldest are dropped */
#define STALL_NS 500000000ull /* a host device that has not asked for sound this long is not playing */
#define PRIME_FRAMES 1024      /* a stream plays once this much is queued (21 ms), again after running dry, */
                               /* or once its producer stops adding (it waits for room: its queue is smaller) */

typedef struct stream {
    bool used;
    bool primed;  /* playing; false until PRIME_FRAMES are queued */
    float *queue; /* stereo frames, a ring */
    uint32_t head, count;
    uint32_t seen; /* count at the last host callback, while not playing */
} stream;

static struct {
    pal_mutex *lock;
    bool host_tried, host;
    bool stalled;      /* the host device stopped asking for sound: our clock paces the streams */
    uint64_t last_ns;  /* the last host callback */
    uint32_t underruns; /* since the last PS5C_LOG_AUDIO line */
    stream streams[MAX_STREAMS];
} g_mix;

/* Host audio thread: sums the queued frames of every stream. */
static void mix(void *user, float *out, uint32_t frames) {
    (void)user;
    memset(out, 0, (size_t)frames * 2 * sizeof(float));
    __atomic_store_n(&g_mix.last_ns, pal_time_ns(), __ATOMIC_RELAXED);
    pal_mutex_lock(g_mix.lock);
    for (int i = 0; i < MAX_STREAMS; i++) {
        stream *s = &g_mix.streams[i];
        if (!s->used) continue;
        if (!s->primed && s->count < PRIME_FRAMES && (!s->count || s->count != s->seen)) {
            /* Sonic Frontiers' AudioOut2 context queues two grains of 256 frames, then waits for the device */
            s->seen = s->count;
            continue;
        }
        s->primed = true;
        uint32_t n = s->count < frames ? s->count : frames;
        if (n < frames) g_mix.underruns++; /* a gap in the sound: an audible click */
        for (uint32_t k = 0; k < n; k++) {
            uint32_t at = (s->head + k) % QUEUE_FRAMES;
            out[2 * k] += s->queue[2 * at];
            out[2 * k + 1] += s->queue[2 * at + 1];
        }
        s->head = (s->head + n) % QUEUE_FRAMES;
        s->count -= n;
        if (!s->count) s->primed = false; /* ran dry: buffer again before resuming */
    }
    pal_mutex_unlock(g_mix.lock);
    float peak = 0.0f;
    for (uint32_t k = 0; k < frames * 2; k++) {
        out[k] = out[k] > 1.0f ? 1.0f : out[k] < -1.0f ? -1.0f : out[k];
        float a = out[k] < 0 ? -out[k] : out[k];
        if (a > peak) peak = a;
    }
    /* PS5C_NO_AUDIO=1: the host plays silence (the streams keep their pace, testing aid) */
    static int muted = -1;
    if (muted < 0) muted = getenv("PS5C_NO_AUDIO") && strcmp(getenv("PS5C_NO_AUDIO"), "0");
    if (muted) memset(out, 0, sizeof(float) * frames * 2);
    /* PS5C_LOG_AUDIO: the loudest sample of every 5 seconds (debugging aid) */
    static int logging = -1;
    static float window_peak;
    static uint64_t window_frames;
    if (logging < 0) logging = getenv("PS5C_LOG_AUDIO") != NULL;
    if (logging) {
        if (peak > window_peak) window_peak = peak;
        window_frames += frames;
        if (window_frames >= 5 * 48000) {
            LOG_I(KERNEL, "audio: peak %.3f over 5 s, %u underruns", (double)window_peak, g_mix.underruns);
            g_mix.underruns = 0;
            window_peak = 0.0f;
            window_frames = 0;
        }
    }
}

/* Opening the host device can take long (waveOut ~1 s on an idle PC; Demon's
 * Souls' first port waited ~130 s behind its 13 spinning job threads, the main
 * thread stuck in sceAudioOut2PortCreate): it opens on a thread of its own, and
 * the streams go by our clock until it plays. */
static int32_t host_start(void *arg) {
    (void)arg;
    __atomic_store_n(&g_mix.last_ns, pal_time_ns(), __ATOMIC_RELAXED);
    if (pal_audio_start(mix, NULL) == PS5C_OK) {
        __atomic_store_n(&g_mix.last_ns, pal_time_ns(), __ATOMIC_RELAXED);
        __atomic_store_n(&g_mix.host, true, __ATOMIC_RELEASE);
    } else {
        LOG_W(KERNEL, "no host sound output: audio is silent");
    }
    return 0;
}

int audio_mix_open(void) {
    if (!g_mix.lock && pal_mutex_create(&g_mix.lock) != PS5C_OK) return -1;
    float *q = (float *)calloc(QUEUE_FRAMES * 2, sizeof(float));
    if (!q) return -1;
    int id = -1;
    pal_mutex_lock(g_mix.lock);
    for (int i = 0; i < MAX_STREAMS && id < 0; i++) {
        if (g_mix.streams[i].used) continue;
        g_mix.streams[i].used = true;
        g_mix.streams[i].queue = q;
        g_mix.streams[i].head = g_mix.streams[i].count = g_mix.streams[i].seen = 0;
        g_mix.streams[i].primed = false;
        id = i;
    }
    bool start = id >= 0 && !g_mix.host_tried;
    if (start) g_mix.host_tried = true;
    pal_mutex_unlock(g_mix.lock);
    if (id < 0) free(q);
    if (start) {
        /* [geral] som=0: no host sound (a PC without a sound device, or one that never plays) */
        if (!settings_get_int("geral", "som", 1)) {
            LOG_I(KERNEL, "sound off ([geral] som=0)");
        } else {
            static pal_thread *opener; /* runs once; never joined */
            if (pal_thread_create(&opener, host_start, NULL, "audio open", 0) != PS5C_OK) host_start(NULL);
        }
    }
    return id;
}

uint32_t audio_mix_queued(int id) {
    if (id < 0 || id >= MAX_STREAMS || !g_mix.lock) return 0;
    pal_mutex_lock(g_mix.lock);
    uint32_t n = g_mix.streams[id].used ? g_mix.streams[id].count : 0;
    pal_mutex_unlock(g_mix.lock);
    return n;
}

/* A host device that stops asking for sound (no output device, a driver that
 * never plays) would block every port waiting for room: its streams then go
 * by our clock, as without host sound. */
bool audio_mix_active(void) {
    if (!__atomic_load_n(&g_mix.host, __ATOMIC_ACQUIRE)) return false;
    /* Read the callback stamp before the clock: a callback between those
     * reads must not turn unsigned subtraction into a false stall. Multiple
     * guest audio threads can query this transition concurrently. */
    uint64_t last = __atomic_load_n(&g_mix.last_ns, __ATOMIC_RELAXED);
    bool stalled = pal_time_ns() - last > STALL_NS;
    if (stalled != __atomic_exchange_n(&g_mix.stalled, stalled, __ATOMIC_RELAXED)) {
        if (stalled) LOG_W(KERNEL, "host sound device is not playing: going on without it");
        else LOG_I(KERNEL, "host sound device is playing again");
    }
    return !stalled;
}

void audio_mix_close(int id) {
    if (id < 0 || id >= MAX_STREAMS || !g_mix.lock) return;
    pal_mutex_lock(g_mix.lock);
    stream *s = &g_mix.streams[id];
    free(s->queue);
    memset(s, 0, sizeof(*s));
    pal_mutex_unlock(g_mix.lock);
}

static float sample(const uint8_t *buf, bool is_float, uint64_t at) {
    if (is_float) {
        float v;
        memcpy(&v, buf + at * 4, 4);
        return v;
    }
    int16_t v;
    memcpy(&v, buf + at * 2, 2);
    return (float)v / 32768.0f;
}

void audio_mix_write(int id, const void *buf, uint32_t frames, uint32_t channels, bool is_float, float vol_l,
                     float vol_r) {
    if (id < 0 || id >= MAX_STREAMS || !buf || !channels || !g_mix.lock) return;
    const uint8_t *b = (const uint8_t *)buf;
    pal_mutex_lock(g_mix.lock);
    stream *s = &g_mix.streams[id];
    for (uint32_t i = 0; s->used && i < frames; i++) {
        uint64_t f = (uint64_t)i * channels;
        float l, r;
        if (channels == 1) {
            l = r = sample(b, is_float, f);
        } else if (channels < 8) {
            l = sample(b, is_float, f);
            r = sample(b, is_float, f + 1);
        } else { /* front pair, center (LFE left out), then the side/back pairs and any height pairs */
            float ctr = sample(b, is_float, f + 2) * 0.7071f;
            l = sample(b, is_float, f) + ctr;
            r = sample(b, is_float, f + 1) + ctr;
            for (uint32_t ch = 4; ch + 1 < channels; ch += 2) {
                l += 0.5f * sample(b, is_float, f + ch);
                r += 0.5f * sample(b, is_float, f + ch + 1);
            }
        }
        if (s->count == QUEUE_FRAMES) { /* full: the oldest frame goes */
            s->head = (s->head + 1) % QUEUE_FRAMES;
            s->count--;
        }
        uint32_t at = (s->head + s->count) % QUEUE_FRAMES;
        s->queue[2 * at] = l * vol_l;
        s->queue[2 * at + 1] = r * vol_r;
        s->count++;
    }
    /* the game runs ahead of the device (host clock drift): keep the latency bounded */
    while (s->count > QUEUE_KEEP_FRAMES) {
        s->head = (s->head + 1) % QUEUE_FRAMES;
        s->count--;
    }
    pal_mutex_unlock(g_mix.lock);
}
