/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceAvPlayer: the system movie player (MP4 with H.264 and AAC). A host
 * thread per player decodes ahead (pal_media) into small queues; the game
 * pulls pictures and sound as their time comes: GetVideoData hands out the
 * newest picture due by the playback clock, GetAudioData the next PCM block.
 * Pictures are NV12 with 256-byte rows and a 16-aligned height (the crop offsets
 * give the visible size), in guest buffers the player maps for itself.
 * [video] filmes=0 (or PS5C_NO_MOVIES) turns it off: sceAvPlayerInit then
 * fails and games skip their movies, as before this player existed. */
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "core/settings.h"
#include "gpu/gpu_watch.h"
#include "hle/hle.h"
#include "hle/avplayer_timing.h"
#include "hle/avplayer_picture.h"
#include "loader/module_set.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);
int kernel_fs_resolve(hle_call *c, const char *guest, char *host, size_t host_size);

#define AVP_ERR_INVALID_PARAMS ((int32_t)0x806A0001)
#define AVP_ERR_OPERATION_FAILED ((int32_t)0x806A0002)

/* SceAvPlayerEvents */
#define EV_STATE_STOP 0x01
#define EV_STATE_READY 0x02
#define EV_STATE_PLAY 0x03
#define EV_STATE_PAUSE 0x04

#define MAX_PLAYERS 8
#define VIDEO_QUEUE 3
#define AUDIO_QUEUE 32
#define AUDIO_CHUNK 1024      /* frames per GetAudioData block */
#define AUDIO_LEAD_US 100000  /* sound is handed out this far ahead of the clock */
#define GUEST_BUFFERS 4
#define HANDLE_BASE 0x5A000   /* handles: HANDLE_BASE + index * 0x100 */

/* A PS4 game (its executable imports libSceGnmDriver, the PS4 graphics driver; PS4 and PS5
 * executables both carry the "ORBI" SceProcParam): it builds the picture textures with PS4
 * pitch rules */
static bool ps4_title(hle_call *c) {
    static int known = -1;
    if (known < 0) {
        const loaded_module *m = c->rt && c->rt->ms ? module_set_main(c->rt->ms) : NULL;
        known = 0;
        for (uint32_t i = 0; m && i < m->img.import_lib_count; i++)
            if (m->img.import_libs[i].name && !strcmp(m->img.import_libs[i].name, "libSceGnmDriver")) known = 1;
        LOG_I(HLE, "AvPlayer: %s picture pitch", known ? "PS4 (64-byte)" : "PS5 (256-byte)");
    }
    return known == 1;
}

typedef struct avp_picture {
    uint8_t *nv12; /* width * height * 3 / 2, packed */
    uint64_t ts_us;  /* on the playback clock, which runs on across loops */
    uint64_t pos_us; /* in the file: what the game is told (it restarts at each loop) */
} avp_picture;

typedef struct avp_sound {
    int16_t pcm[AUDIO_CHUNK * 2];
    uint32_t frames;
    uint64_t ts_us, pos_us;
} avp_sound;

typedef struct av_player {
    bool used;
    /* guest callbacks: SceAvPlayerFileReplacement and SceAvPlayerEventReplacement */
    guest_addr file_obj, file_open, file_close, file_read, file_size;
    guest_addr event_obj, event_cb;
    bool auto_start;

    pal_media *media;
    pal_media_info info;
    uint32_t pitch, plane_h; /* GFX10 linear row pitch and padded plane height */

    pal_thread *thread;
    pal_mutex *lock;
    pal_cond *cond;
    bool quit, rewind;
    uint64_t skip_us; /* after a jump: pictures and sound before this file time are dropped */
    avp_picture vq[VIDEO_QUEUE];
    uint32_t vq_head, vq_count;
    avp_sound aq[AUDIO_QUEUE];
    uint32_t aq_head, aq_count;
    bool video_done, audio_done; /* the decoder reached the end (and is not looping) */

    bool has_source, playing, paused, looping, ended, stop_sent;
    uint64_t start_ns, paused_us, loop_base_us, last_ts_us;
    uint64_t shown_base_us; /* loop_base_us of the last picture handed out (CurrentTime) */
    int32_t pending[8];     /* state events not yet delivered (send_event), under g_avp.lock */
    uint32_t pending_count;
    uint64_t pending_ns;    /* when the oldest of them was queued */
    guest_addr vbuf, abuf; /* GUEST_BUFFERS pictures / sound blocks each */
    uint64_t vbuf_size, abuf_size;
    uint32_t vbuf_next, abuf_next;
} av_player;

static struct {
    pal_mutex *lock;
    pal_cond *events; /* wakes the event thread */
    bool event_thread;
    bool trace; /* PS5C_AVPLAYER_TRACE, initialized before guest threads */
    av_player players[MAX_PLAYERS];
} g_avp;

static av_player *get(uint64_t handle) {
    if (handle < HANDLE_BASE || (handle - HANDLE_BASE) % 0x100) return NULL;
    uint64_t i = (handle - HANDLE_BASE) / 0x100;
    return i < MAX_PLAYERS && g_avp.players[i].used ? &g_avp.players[i] : NULL;
}

static uint64_t clock_us(av_player *p) {
    if (!p->playing || p->paused) return p->paused_us;
    return (pal_time_ns() - p->start_ns) / 1000;
}

/* An event to the game's callback. The console's player sends them from its own
 * thread, after the call that caused them has returned; here they wait for the
 * game's next call to the player (Unity ignored a state change reported while its
 * own call was still running, and kept the movie paused), or for the event thread
 * once they are EVENT_DELAY_NS old (Dying Light waits for READY in a sleep loop
 * without calling the player). The public distribution delivers callbacks during the call by default.
 * PS5C_AVP_ASYNC_EVENTS=1 selects deferred/threaded delivery; an explicit
 * PS5C_AVP_SYNC_EVENTS override keeps synchronous delivery. */
#define EVENT_DELAY_NS 2000000ull

static void send_event(hle_call *c, av_player *p, int32_t event) {
    if (!p->event_cb) return;
    static int sync = -1;
    if (sync < 0) {
        const char *async = getenv("PS5C_AVP_ASYNC_EVENTS");
        sync = !async || strcmp(async, "1") || getenv("PS5C_AVP_SYNC_EVENTS") != NULL;
    }
    pal_mutex_lock(g_avp.lock);
    if (!sync && p->pending_count < PS5C_ARRAY_COUNT(p->pending)) {
        if (!p->pending_count) p->pending_ns = pal_time_ns();
        p->pending[p->pending_count++] = event;
        if (g_avp.events) pal_cond_signal(g_avp.events);
        pal_mutex_unlock(g_avp.lock);
        return;
    }
    pal_mutex_unlock(g_avp.lock);
    uint64_t rax;
    runtime_call_nested4(c->rt, p->event_cb, p->event_obj, (uint64_t)(uint32_t)event, 0, 0, &rax);
}

/* The next waiting event of p (0 if none), removed from its queue. Caller holds g_avp.lock. */
static int32_t pop_event(av_player *p, guest_addr *cb, guest_addr *obj) {
    if (!p->used || !p->pending_count || !p->event_cb) return 0;
    int32_t event = p->pending[0];
    p->pending_count--;
    memmove(p->pending, p->pending + 1, p->pending_count * sizeof(p->pending[0]));
    *cb = p->event_cb;
    *obj = p->event_obj;
    return event;
}

/* The player of a handle, after delivering the events waiting for this call */
static av_player *get_delivering(hle_call *c, uint64_t handle) {
    av_player *p = get(handle);
    while (p) {
        guest_addr cb = 0, obj = 0;
        pal_mutex_lock(g_avp.lock);
        int32_t event = pop_event(p, &cb, &obj);
        pal_mutex_unlock(g_avp.lock);
        if (!event) break;
        uint64_t rax;
        runtime_call_nested4(c->rt, cb, obj, (uint64_t)(uint32_t)event, 0, 0, &rax);
    }
    return p;
}

/* Guest thread (its entry is this HLE function) delivering the events that no
 * call to the player picked up in time. It runs until the process ends. */
static void avp_event_thread(hle_call *c) {
    pal_mutex_lock(g_avp.lock);
    for (;;) {
        uint64_t now = pal_time_ns(), wait = 0;
        av_player *due = NULL;
        for (uint32_t i = 0; i < MAX_PLAYERS && !due; i++) {
            av_player *p = &g_avp.players[i];
            if (!p->used || !p->pending_count || !p->event_cb) continue;
            uint64_t at = p->pending_ns + EVENT_DELAY_NS;
            if (now >= at) due = p;
            else if (!wait || at - now < wait) wait = at - now;
        }
        if (!due) {
            if (wait) pal_cond_wait_ns(g_avp.events, g_avp.lock, wait);
            else pal_cond_wait(g_avp.events, g_avp.lock);
            continue;
        }
        guest_addr cb = 0, obj = 0;
        int32_t event = pop_event(due, &cb, &obj);
        pal_mutex_unlock(g_avp.lock);
        if (g_avp.trace) LOG_I(HLE, "avplayer event %d from the event thread", event);
        uint64_t rax;
        if (!runtime_call_nested4(c->rt, cb, obj, (uint64_t)(uint32_t)event, 0, 0, &rax)) return;
        pal_mutex_lock(g_avp.lock);
    }
}

/* Starts the event thread once, from the guest thread creating the first player */
static void start_event_thread(hle_call *c) {
    pal_mutex_lock(g_avp.lock);
    bool start = !g_avp.event_thread && g_avp.events;
    g_avp.event_thread = true;
    pal_mutex_unlock(g_avp.lock);
    if (!start) return;
    guest_addr entry = runtime_resolve_symbol(c->rt, "__ps5c_avplayer_event_thread");
    guest_thread *t = NULL;
    if (!entry || runtime_start_thread(c->rt, entry, 0, 0, 0x40000, true, "SceAvPlayerEvent", &t) != 0) {
        LOG_W(HLE, "AvPlayer: no event thread (events wait for the next player call)");
        return;
    }
    runtime_release_thread(c->rt, t);
}

/* ---- Decoder thread ---- */

/* Caller holds p->lock. Keep a due picture available while freeing slots
 * for the decoder to catch up, even when GetVideoData is called infrequently. */
static void discard_late_video(av_player *p,uint64_t now) {
    uint64_t timestamps[VIDEO_QUEUE];
    for(uint32_t i=0;i<VIDEO_QUEUE;i++)timestamps[i]=p->vq[i].ts_us;
    uint32_t discard=avp_late_video_frames(timestamps,VIDEO_QUEUE,p->vq_head,p->vq_count,now);
    p->vq_head=(p->vq_head+discard)%VIDEO_QUEUE;p->vq_count-=discard;
}

static uint64_t frame_us(const av_player *p) {
    return p->info.frame_rate > 0 ? (uint64_t)(1e6 / p->info.frame_rate) : 33333;
}

static int32_t decode_main(void *arg) {
    av_player *p = (av_player *)arg;
    uint32_t w = p->info.width;
    for (;;) {
        pal_mutex_lock(p->lock);
        for (;;) {
            if (p->quit) {
                pal_mutex_unlock(p->lock);
                return 0;
            }
            if (p->rewind) break;
            discard_late_video(p,clock_us(p));
            bool want_video = !p->video_done && p->vq_count < VIDEO_QUEUE;
            bool want_audio = !p->audio_done && p->aq_count < AUDIO_QUEUE;
            bool at_end = (p->video_done || !w) && (p->audio_done || !p->info.channels);
            if (want_video || want_audio || (at_end && p->looping)) break;
            /* The clock can make queued pictures late without a consumer
             * call. Recheck it instead of waiting indefinitely on a full queue. */
            pal_cond_wait_ns(p->cond, p->lock, 5000000);
        }
        bool rewind = p->rewind || ((p->video_done || !w) && (p->audio_done || !p->info.channels) && p->looping);
        bool want_video = !p->video_done && p->vq_count < VIDEO_QUEUE;
        bool want_audio = !p->audio_done && p->aq_count < AUDIO_QUEUE;
        avp_picture *pic = want_video ? &p->vq[(p->vq_head + p->vq_count) % VIDEO_QUEUE] : NULL;
        avp_sound *snd = want_audio ? &p->aq[(p->aq_head + p->aq_count) % AUDIO_QUEUE] : NULL;
        pal_mutex_unlock(p->lock);

        if (rewind) {
            /* loop: the next pass continues the timeline after this one */
            bool ok = pal_media_rewind(p->media);
            pal_mutex_lock(p->lock);
            if (!p->rewind) {
                p->loop_base_us = p->last_ts_us + frame_us(p);
                p->skip_us = 0;
            }
            p->rewind = false;
            p->video_done = !ok || !w;
            p->audio_done = !ok || !p->info.channels;
            pal_mutex_unlock(p->lock);
            continue;
        }
        int vr = 0, ar = 0;
        uint64_t vts = 0, ats = 0;
        if (pic) vr = pal_media_next_video(p->media, pic->nv12, &vts);
        if (snd) ar = pal_media_next_audio(p->media, snd->pcm, AUDIO_CHUNK, &ats);
        pal_mutex_lock(p->lock);
        /* a jump while decoding: this picture and sound belong to the old position */
        if (p->rewind) pic = NULL, snd = NULL;
        /* still before the time jumped to: decode on without queueing */
        if (pic && vr > 0 && vts + frame_us(p) <= p->skip_us) pic = NULL;
        if (snd && ar > 0 && ats + (uint64_t)ar * 1000000 / (p->info.sample_rate ? p->info.sample_rate : 48000) <= p->skip_us)
            snd = NULL;
        if (pic) {
            if (g_avp.trace)
                LOG_I(HLE, "avplayer decode: result %d, timestamp %llu us, queue %u", vr,
                      (unsigned long long)vts, p->vq_count);
            if (vr > 0) {
                pic->ts_us = p->loop_base_us + vts;
                pic->pos_us = vts;
                p->last_ts_us = pic->ts_us;
                p->vq_count++;
            } else {
                p->video_done = true;
            }
        }
        if (snd) {
            if (ar > 0) {
                snd->frames = (uint32_t)ar;
                snd->ts_us = p->loop_base_us + ats;
                snd->pos_us = ats;
                p->aq_count++;
            } else {
                p->audio_done = true;
            }
        }
        pal_mutex_unlock(p->lock);
    }
}

/* ---- Setup ---- */

static void player_free(hle_call *c, av_player *p) {
    if (p->thread) {
        pal_mutex_lock(p->lock);
        p->quit = true;
        pal_cond_broadcast(p->cond);
        pal_mutex_unlock(p->lock);
        pal_thread_join(p->thread, NULL);
    }
    for (uint32_t i = 0; i < VIDEO_QUEUE; i++) free(p->vq[i].nv12);
    pal_media_close(p->media);
    if (p->cond) pal_cond_destroy(p->cond);
    if (p->lock) pal_mutex_destroy(p->lock);
    if (p->vbuf) gmem_unmap(c->mem, p->vbuf, p->vbuf_size * GUEST_BUFFERS);
    if (p->abuf) gmem_unmap(c->mem, p->abuf, p->abuf_size * GUEST_BUFFERS);
    pal_mutex_lock(g_avp.lock);
    memset(p, 0, sizeof(*p));
    pal_mutex_unlock(g_avp.lock);
}

static bool movies_enabled(void) {
    return !getenv("PS5C_NO_MOVIES") && settings_get_int("video", "filmes", 1) != 0;
}

/* Common part of Init and InitEx: the replacement callbacks at `mem`, `file` and `event` */
static uint64_t player_create(hle_call *c, guest_addr file, guest_addr event, bool auto_start) {
    if (!movies_enabled()) return 0;
    pal_mutex_lock(g_avp.lock);
    av_player *p = NULL;
    uint32_t i = 0;
    for (; i < MAX_PLAYERS && !p; i++)
        if (!g_avp.players[i].used) p = &g_avp.players[i];
    if (p) {
        memset(p, 0, sizeof(*p));
        p->used = true;
    }
    pal_mutex_unlock(g_avp.lock);
    if (!p) return 0;
    uint64_t f[5] = {0}, e[2] = {0};
    gmem_read(c->mem, file, f, sizeof(f));
    gmem_read(c->mem, event, e, sizeof(e));
    p->file_obj = f[0];
    p->file_open = f[1];
    p->file_close = f[2];
    p->file_read = f[3];
    p->file_size = f[4];
    p->event_obj = e[0];
    p->event_cb = e[1];
    /* without an event callback nobody hears READY to call Start: the player starts by
     * itself (Jurassic Park Classic Games Collection; as AnyPS5 and rps5e do) */
    p->auto_start = auto_start || !p->event_cb;
    if (p->event_cb) start_event_thread(c);
    if (g_avp.trace)
        LOG_I(HLE, "avplayer setup: auto_start %u, file object 0x%llx, event callback 0x%llx object 0x%llx",
              (unsigned)p->auto_start, (unsigned long long)p->file_obj, (unsigned long long)p->event_cb,
              (unsigned long long)p->event_obj);
    return HANDLE_BASE + (uint64_t)(i - 1) * 0x100;
}

/* SceAvPlayerHandle sceAvPlayerInit(SceAvPlayerInitData *data): memory
 * replacement at +0x00, file replacement at +0x28, event replacement at +0x50,
 * auto_start at +0x6c */
static void avp_init(hle_call *c) {
    guest_addr d = hle_arg_ptr(c, 0);
    uint8_t auto_start = 0;
    if (!d || !gmem_read(c->mem, d + 0x6c, &auto_start, 1)) {
        hle_ret_u64(c, 0);
        return;
    }
    uint64_t h = player_create(c, d + 0x28, d + 0x50, auto_start != 0);
    LOG_I(HLE, "sceAvPlayerInit -> 0x%llx", (unsigned long long)h);
    hle_ret_u64(c, h);
}

/* int sceAvPlayerInitEx(SceAvPlayerInitDataEx *data, SceAvPlayerHandle *out):
 * this_size at +0x00, memory replacement at +0x08, file at +0x30, event at
 * +0x58, auto_start at +0xa8 */
static void avp_init_ex(hle_call *c) {
    guest_addr d = hle_arg_ptr(c, 0), out = hle_arg_ptr(c, 1);
    uint8_t auto_start = 0;
    if (!d || !out || !gmem_read(c->mem, d + 0xa8, &auto_start, 1)) {
        hle_ret_s32(c, AVP_ERR_INVALID_PARAMS);
        return;
    }
    uint64_t h = player_create(c, d + 0x30, d + 0x58, auto_start != 0);
    LOG_I(HLE, "sceAvPlayerInitEx -> 0x%llx", (unsigned long long)h);
    gmem_write_u64(c->mem, out, h);
    hle_ret_s32(c, h ? 0 : AVP_ERR_OPERATION_FAILED);
}

/* The whole movie through the game's file callbacks (they must run on a guest
 * thread, so it is read here, up front) */
static pal_media *open_through_callbacks(hle_call *c, av_player *p, guest_addr path) {
    uint64_t r = 0;
    if (!runtime_call_nested(c->rt, p->file_open, p->file_obj, path, 0, &r) || (int32_t)r < 0) return NULL;
    uint64_t size = 0;
    runtime_call_nested(c->rt, p->file_size, p->file_obj, 0, 0, &size);
    pal_media *m = NULL;
    uint8_t *data = size && size < (1ull << 31) ? (uint8_t *)malloc(size) : NULL;
    guest_addr chunk = 0;
    const uint32_t chunk_size = 1u << 20;
    if (data && gmem_map(c->mem, 0, chunk_size, GPROT_RW, 0, "avplayer read", &chunk) == PS5C_OK) {
        uint64_t pos = 0;
        while (pos < size) {
            uint32_t n = (uint32_t)(size - pos < chunk_size ? size - pos : chunk_size);
            uint64_t got = 0;
            if (!runtime_call_nested4(c->rt, p->file_read, p->file_obj, chunk, pos, n, &got) || (int32_t)got <= 0) break;
            gmem_read(c->mem, chunk, data + pos, (uint32_t)got);
            pos += (uint32_t)got;
        }
        /* PS5C_AVPLAYER_DUMP=<file>: the movie as the game's callbacks gave it (debugging aid) */
        const char *dump = getenv("PS5C_AVPLAYER_DUMP");
        pal_file *df;
        if (dump && pos == size && pal_file_open(dump, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE, &df) == PS5C_OK) {
            pal_file_write(df, data, size, NULL);
            pal_file_close(df);
        }
        if (pos == size) m = pal_media_open_memory(data, size);
        gmem_unmap(c->mem, chunk, chunk_size);
    }
    free(data);
    runtime_call_nested(c->rt, p->file_close, p->file_obj, 0, 0, &r);
    return m;
}

static int32_t add_source(hle_call *c, av_player *p, guest_addr path_addr) {
    char path[512], host[1024];
    if (!p || !path_addr || !hle_read_str(c, path_addr, path, sizeof(path))) return AVP_ERR_INVALID_PARAMS;
    if (p->has_source) return AVP_ERR_OPERATION_FAILED;
    pal_media *m = NULL;
    if (p->file_open && p->file_read && p->file_size) m = open_through_callbacks(c, p, path_addr);
    else if (kernel_fs_resolve(c, path, host, sizeof(host)) == 0) m = pal_media_open_file(host);
    if (!m) {
        LOG_W(HLE, "sceAvPlayerAddSource: cannot play %s", path);
        return AVP_ERR_OPERATION_FAILED;
    }
    p->media = m;
    pal_media_get_info(m, &p->info);
    p->pitch = ps4_title(c) ? avp_picture_pitch_ps4(p->info.width) : avp_picture_pitch(p->info.width);
    p->plane_h = PS5C_ALIGN_UP(p->info.height, 16);
    LOG_I(HLE, "movie %s: %ux%u %.2f fps, audio %u ch %u Hz, %.1f s", path, p->info.width, p->info.height,
          p->info.frame_rate, p->info.channels, p->info.sample_rate, (double)p->info.duration_us / 1e6);
    bool ok = pal_mutex_create(&p->lock) == PS5C_OK && pal_cond_create(&p->cond) == PS5C_OK;
    for (uint32_t i = 0; ok && i < VIDEO_QUEUE && p->info.width; i++)
        ok = (p->vq[i].nv12 = (uint8_t *)malloc((size_t)p->info.width * p->info.height * 3 / 2)) != NULL;
    p->vbuf_size = PS5C_ALIGN_UP((uint64_t)p->pitch * p->plane_h * 3 / 2, GUEST_PAGE_SIZE);
    p->abuf_size = PS5C_ALIGN_UP((uint64_t)AUDIO_CHUNK * 4, GUEST_PAGE_SIZE);
    if (ok && p->info.width)
        ok = gmem_map(c->mem, 0, p->vbuf_size * GUEST_BUFFERS, GPROT_RW, 0, "avplayer pictures", &p->vbuf) == PS5C_OK;
    if (ok && p->info.channels)
        ok = gmem_map(c->mem, 0, p->abuf_size * GUEST_BUFFERS, GPROT_RW, 0, "avplayer sound", &p->abuf) == PS5C_OK;
    if (ok && p->vbuf) {
        /* the padding below and right of the picture stays black */
        for (uint32_t b = 0; b < GUEST_BUFFERS; b++) {
            uint8_t *dst = (uint8_t *)gmem_host(c->mem, p->vbuf + b * p->vbuf_size, p->vbuf_size);
            if (!dst) continue;
            memset(dst, 16, (size_t)p->pitch * p->plane_h);
            memset(dst + (size_t)p->pitch * p->plane_h, 128, (size_t)p->pitch * p->plane_h / 2);
        }
    }
    p->video_done = !p->info.width;
    p->audio_done = !p->info.channels;
    if (ok) ok = pal_thread_create(&p->thread, decode_main, p, "avplayer", 0) == PS5C_OK;
    if (!ok) {
        LOG_W(HLE, "sceAvPlayerAddSource: out of memory for %s", path);
        return AVP_ERR_OPERATION_FAILED;
    }
    p->has_source = true;
    send_event(c, p, EV_STATE_READY);
    if (p->auto_start) {
        p->playing = true;
        p->start_ns = pal_time_ns();
        send_event(c, p, EV_STATE_PLAY);
    }
    return 0;
}

/* int sceAvPlayerAddSource(SceAvPlayerHandle h, const char *path) */
static void avp_add_source(hle_call *c) {
    hle_ret_s32(c, add_source(c, get(hle_arg(c, 0)), hle_arg_ptr(c, 1)));
}

/* int sceAvPlayerAddSourceEx(h, SceAvPlayerUriType type, SceAvPlayerSourceDetails *d):
 * the URI's name pointer is at +0 */
static void avp_add_source_ex(hle_call *c) {
    uint64_t name = 0;
    guest_addr d = hle_arg_ptr(c, 2);
    if (!d || !gmem_read_u64(c->mem, d, &name)) {
        hle_ret_s32(c, AVP_ERR_INVALID_PARAMS);
        return;
    }
    hle_ret_s32(c, add_source(c, get(hle_arg(c, 0)), name));
}

/* ---- Playback control ---- */

static void avp_start(hle_call *c) {
    av_player *p = get_delivering(c, hle_arg(c, 0));
    if (!p || !p->has_source) {
        hle_ret_s32(c, AVP_ERR_OPERATION_FAILED);
        return;
    }
    if (!p->playing) {
        p->playing = true;
        p->paused = false;
        p->start_ns = pal_time_ns() - p->paused_us * 1000;
        send_event(c, p, EV_STATE_PLAY);
    }
    hle_ret_s32(c, 0);
}

static void finish(hle_call *c, av_player *p) {
    p->ended = true;
    if (p->playing) p->paused_us = clock_us(p);
    p->playing = false;
    if (!p->stop_sent) {
        p->stop_sent = true;
        send_event(c, p, EV_STATE_STOP);
    }
}

static void avp_stop(hle_call *c) {
    av_player *p = get_delivering(c, hle_arg(c, 0));
    if (p && p->has_source) finish(c, p);
    hle_ret_s32(c, p ? 0 : AVP_ERR_INVALID_PARAMS);
}

static void avp_pause(hle_call *c) {
    av_player *p = get_delivering(c, hle_arg(c, 0));
    if (p && p->playing && !p->paused) {
        p->paused_us = clock_us(p);
        p->paused = true;
        send_event(c, p, EV_STATE_PAUSE);
    }
    hle_ret_s32(c, p ? 0 : AVP_ERR_INVALID_PARAMS);
}

static void avp_resume(hle_call *c) {
    av_player *p = get_delivering(c, hle_arg(c, 0));
    if (p && p->playing && p->paused) {
        p->start_ns = pal_time_ns() - p->paused_us * 1000;
        p->paused = false;
        send_event(c, p, EV_STATE_PLAY);
    }
    hle_ret_s32(c, p ? 0 : AVP_ERR_INVALID_PARAMS);
}

/* int sceAvPlayerSetLooping(h, bool) */
static void avp_set_looping(hle_call *c) {
    av_player *p = get_delivering(c, hle_arg(c, 0));
    if (p) {
        if (p->lock) pal_mutex_lock(p->lock);
        p->looping = (hle_arg(c, 1) & 0xFF) != 0;
        if (p->cond) pal_cond_broadcast(p->cond);
        if (p->lock) pal_mutex_unlock(p->lock);
    }
    hle_ret_s32(c, p ? 0 : AVP_ERR_INVALID_PARAMS);
}

/* int sceAvPlayerJumpToTime(h, uint64_t ms): the decoder restarts from the
 * beginning and drops what comes before `ms` (both backends can rewind; none seeks) */
static void avp_jump_to_time(hle_call *c) {
    av_player *p = get_delivering(c, hle_arg(c, 0));
    if (!p || !p->has_source) {
        hle_ret_s32(c, AVP_ERR_INVALID_PARAMS);
        return;
    }
    uint64_t to_us = hle_arg(c, 1) * 1000;
    if (p->info.duration_us && to_us > p->info.duration_us) to_us = p->info.duration_us;
    if (g_avp.trace) LOG_I(HLE, "avplayer jump to %llu ms", (unsigned long long)(to_us / 1000));
    pal_mutex_lock(p->lock);
    p->rewind = true;
    p->skip_us = to_us;
    p->vq_count = p->aq_count = 0;
    p->loop_base_us = p->last_ts_us = p->shown_base_us = 0;
    p->video_done = !p->info.width;
    p->audio_done = !p->info.channels;
    pal_cond_broadcast(p->cond);
    pal_mutex_unlock(p->lock);
    p->paused_us = to_us;
    p->start_ns = pal_time_ns() - to_us * 1000;
    p->ended = p->stop_sent = false;
    hle_ret_s32(c, 0);
}

/* bool sceAvPlayerIsActive(h): false once the movie ended or stopped */
static void avp_is_active(hle_call *c) {
    av_player *p = get_delivering(c, hle_arg(c, 0));
    hle_ret_u32(c, p && p->has_source && !p->ended);
}

/* uint64_t sceAvPlayerCurrentTime(h): milliseconds */
static void avp_current_time(hle_call *c) {
    av_player *p = get_delivering(c, hle_arg(c, 0));
    /* the position in the file: Unity sees a loop when it goes back */
    uint64_t now = p ? clock_us(p) : 0;
    hle_ret_u64(c, p ? (now > p->shown_base_us ? now - p->shown_base_us : 0) / 1000 : 0);
}

/* ---- Streams: 0 = video, 1 = audio (when present) ---- */

static uint32_t stream_count(const av_player *p) { return (p->info.width ? 1u : 0u) + (p->info.channels ? 1u : 0u); }

/* 0 video, 1 audio, -1 none */
static int stream_type(const av_player *p, uint32_t index) {
    if (index == 0) return p->info.width ? 0 : p->info.channels ? 1 : -1;
    return index == 1 && p->info.width && p->info.channels ? 1 : -1;
}

static void avp_stream_count(hle_call *c) {
    av_player *p = get_delivering(c, hle_arg(c, 0));
    hle_ret_s32(c, p && p->has_source ? (int32_t)stream_count(p) : AVP_ERR_OPERATION_FAILED);
}

/* SceAvPlayerStreamInfo(Ex): type u32 at +0, details at +8 (16 or 80 bytes),
 * then the duration (u64, ms). The structure ends there: Astro Bot passes
 * 0x20 bytes on its stack (a start time after it smashed the canary). */
static void stream_info(hle_call *c, bool ex) {
    av_player *p = get_delivering(c, hle_arg(c, 0));
    guest_addr out = hle_arg_ptr(c, 2);
    int type = p && p->has_source ? stream_type(p, hle_arg_u32(c, 1)) : -1;
    if (type < 0 || !out) {
        hle_ret_s32(c, AVP_ERR_INVALID_PARAMS);
        return;
    }
    uint8_t info[0x68];
    memset(info, 0, sizeof(info));
    /* PS5 stream types: 0 = unknown, 1 = video, 2 = audio. Unity's
     * PS5Player branches on 1/2; reporting 0 made it ignore the video.
     * PS4 ones (SceAvPlayerStreamType): 0 = video, 1 = audio, 2 = timed text.
     * Tearaway Unfolded took its video for sound and never started its intro. */
    uint32_t t = ps4_title(c) ? (uint32_t)type : (uint32_t)type + 1;
    memcpy(info, &t, 4);
    if (type == 0) {
        uint32_t wh[2] = {p->info.width, p->info.height};
        float aspect = p->info.height ? (float)p->info.width / (float)p->info.height : 1.0f;
        memcpy(info + 8, wh, 8);
        memcpy(info + 16, &aspect, 4);
        memcpy(info + 20, "und", 4);
        if (ex) {
            uint32_t fps = (uint32_t)(p->info.frame_rate + 0.5);
            memcpy(info + 24, &fps, 4);
            /* the frame rate as a double (+0x40 of the structure): Unity divides by it to see a
             * movie's end coming; 0 made every movie end at its first picture (PPSA03706 paused) */
            double rate = p->info.frame_rate;
            memcpy(info + 0x38, &rate, 8);
        }
    } else {
        uint16_t ch = (uint16_t)p->info.channels;
        uint32_t rate = p->info.sample_rate, size = AUDIO_CHUNK * p->info.channels * 2;
        memcpy(info + 8, &ch, 2);
        memcpy(info + 12, &rate, 4);
        memcpy(info + 16, &size, 4);
        memcpy(info + 20, "und", 4);
    }
    /* the Ex structure starts with its own size (0x68, set by the caller): the fields follow it */
    uint32_t tail = ex ? 0x58 : 0x18;
    uint64_t duration_ms = p->info.duration_us / 1000;
    memcpy(info + tail, &duration_ms, 8);
    gmem_write(c->mem, out + (ex ? 8 : 0), info, tail + 8);
    hle_ret_s32(c, 0);
}

static void avp_get_stream_info(hle_call *c) { stream_info(c, false); }
static void avp_get_stream_info_ex(hle_call *c) { stream_info(c, true); }

static void avp_ok(hle_call *c) { hle_ret_s32(c, 0); }

/* ---- Pictures and sound ---- */

/* The newest picture due by the clock into the next guest buffer: its time (µs) */
static bool take_picture(hle_call *c, av_player *p, guest_addr *data, uint64_t *ts_us) {
    if (!p || !p->has_source || !p->playing || !p->vbuf) return false;
    uint64_t now = clock_us(p);
    pal_mutex_lock(p->lock);
    /* sound nobody takes must not hold the decoder back */
    while (p->aq_count && p->aq[p->aq_head].ts_us + 1000000 < now) {
        p->aq_head = (p->aq_head + 1) % AUDIO_QUEUE;
        p->aq_count--;
    }
    int ready = -1;
    discard_late_video(p,now);
    while (p->vq_count && p->vq[p->vq_head].ts_us <= now) {
        ready = (int)p->vq_head;
        break;
    }
    bool ended = !p->vq_count && p->video_done;
    bool got = false;
    if (ready >= 0) {
        avp_picture *pic = &p->vq[ready];
        guest_addr dst_addr = p->vbuf + p->vbuf_next * p->vbuf_size;
        uint8_t *dst = (uint8_t *)gmem_host(c->mem, dst_addr, p->vbuf_size);
        uint32_t w = p->info.width, h = p->info.height;
        /* not a guest instruction: the GPU's write tracking would keep the texture of the
         * buffer's previous picture (Tearaway Unfolded's intro showed only the first, black) */
        if (dst) gpu_watch_prepare_write(dst_addr, p->vbuf_size);
        if (dst) avp_picture_pack(dst, pic->nv12, w, h, p->pitch, p->plane_h);
        *data = dst_addr;
        *ts_us = pic->pos_us;
        p->shown_base_us = pic->ts_us - pic->pos_us;
        p->vbuf_next = (p->vbuf_next + 1) % GUEST_BUFFERS;
        p->vq_head = (p->vq_head + 1) % VIDEO_QUEUE;
        p->vq_count--;
        got = dst != NULL;
        pal_cond_broadcast(p->cond);
    }
    pal_mutex_unlock(p->lock);
    if (ended && !p->looping) finish(c, p);
    return got;
}

/* bool sceAvPlayerGetVideoData(h, SceAvPlayerFrameInfo *info): pData at +0,
 * timestamp (ms) at +0x10, width/height/aspect/language at +0x18 */
static void avp_get_video_data(hle_call *c) {
    av_player *p = get_delivering(c, hle_arg(c, 0));
    guest_addr out = hle_arg_ptr(c, 1), data = 0;
    uint64_t ts = 0;
    if (!out || !take_picture(c, p, &data, &ts)) {
        hle_ret_u32(c, 0);
        return;
    }
    uint8_t info[0x28];
    memset(info, 0, sizeof(info));
    uint64_t ms = ts / 1000;
    uint32_t wh[2] = {PS5C_ALIGN_UP(p->info.width, 16), p->plane_h};
    float aspect = (float)p->info.width / (float)p->info.height;
    memcpy(info, &data, 8);
    memcpy(info + 0x10, &ms, 8);
    memcpy(info + 0x18, wh, 8);
    memcpy(info + 0x20, &aspect, 4);
    memcpy(info + 0x24, "und", 4);
    gmem_write(c->mem, out, info, sizeof(info));
    if (g_avp.trace)
        LOG_I(HLE, "avplayer frame: data 0x%llx timestamp %llu us, %ux%u pitch %u", (unsigned long long)data,
              (unsigned long long)ts, p->pitch, p->plane_h, p->pitch);
    hle_ret_u32(c, 1);
}

/* bool sceAvPlayerGetVideoDataEx(h, SceAvPlayerFrameInfoEx *info): as above
 * with frame rate, crop offsets (left, right, top, bottom), pitch, bit depths.
 * Width and height are the picture's (16-aligned, the crops relative to them),
 * never the row pitch: games that size their texture from the width and sample
 * all of it (Cloudberry Kingdom, PS4) otherwise show the padding as a black
 * column. A linear texture of that width has the same 256-byte pitch anyway. */
static void avp_get_video_data_ex(hle_call *c) {
    av_player *p = get_delivering(c, hle_arg(c, 0));
    guest_addr out = hle_arg_ptr(c, 1), data = 0;
    uint64_t ts = 0;
    if (!out || !take_picture(c, p, &data, &ts)) {
        static uint32_t misses;
        ++misses;
        if (g_avp.trace && (misses & (misses - 1)) == 0)
            LOG_I(HLE, "avplayer frame ex: none (%u times; playing %d paused %d, clock %llu us, queue %u)", misses,
                  p ? p->playing : -1, p ? p->paused : -1, p ? (unsigned long long)clock_us(p) : 0ull, p ? p->vq_count : 0);
        hle_ret_u32(c, 0);
        return;
    }
    uint8_t info[0x68];
    memset(info, 0, sizeof(info));
    uint64_t ms = ts / 1000;
    uint32_t aw = PS5C_ALIGN_UP(p->info.width, 16);
    uint32_t v[10] = {aw, p->plane_h, 0, 0, (uint32_t)(p->info.frame_rate + 0.5), 0, aw - p->info.width,
                      0, p->plane_h - p->info.height, p->pitch};
    float aspect = (float)p->info.width / (float)p->info.height;
    memcpy(&v[2], &aspect, 4);
    memcpy(&v[3], "und", 4);
    memcpy(info, &data, 8);
    memcpy(info + 0x10, &ms, 8);
    memcpy(info + 0x18, v, sizeof(v));
    info[0x40] = 8; /* luma bit depth */
    info[0x41] = 8; /* chroma bit depth */
    gmem_write(c->mem, out, info, sizeof(info));
    if (g_avp.trace)
        LOG_I(HLE, "avplayer frame ex: data 0x%llx timestamp %llu us, %ux%u pitch %u", (unsigned long long)data,
              (unsigned long long)ts, p->pitch, p->plane_h, p->pitch);
    hle_ret_u32(c, 1);
}

/* bool sceAvPlayerGetAudioData(h, SceAvPlayerFrameInfo *info): 16-bit PCM;
 * channels (u16) at +0x18, sample rate at +0x1c, size in bytes at +0x20 */
static void avp_get_audio_data(hle_call *c) {
    av_player *p = get_delivering(c, hle_arg(c, 0));
    guest_addr out = hle_arg_ptr(c, 1);
    /* paused too: the sound up to the paused position (plus the usual lead) still comes out.
     * Unity starts a movie, pauses it at its first picture and plays it once its sound
     * thread has had the first block; a pause before that block left PPSA03706 waiting. */
    if (!p || !out || !p->has_source || !p->playing || !p->abuf) {
        hle_ret_u32(c, 0);
        return;
    }
    uint64_t now = clock_us(p);
    pal_mutex_lock(p->lock);
    bool got = false;
    guest_addr dst = p->abuf + p->abuf_next * p->abuf_size;
    uint32_t bytes = 0;
    uint64_t ts = 0;
    if (p->aq_count && p->aq[p->aq_head].ts_us <= now + AUDIO_LEAD_US) {
        avp_sound *s = &p->aq[p->aq_head];
        bytes = s->frames * p->info.channels * 2;
        got = gmem_write(c->mem, dst, s->pcm, bytes);
        ts = s->pos_us;
        p->aq_head = (p->aq_head + 1) % AUDIO_QUEUE;
        p->aq_count--;
        p->abuf_next = (p->abuf_next + 1) % GUEST_BUFFERS;
        pal_cond_broadcast(p->cond);
    }
    bool ended = !p->aq_count && p->audio_done && !p->info.width;
    pal_mutex_unlock(p->lock);
    if (ended && !p->looping) finish(c, p);
    if (!got) {
        hle_ret_u32(c, 0);
        return;
    }
    uint8_t info[0x28];
    memset(info, 0, sizeof(info));
    uint64_t ms = ts / 1000;
    uint16_t ch = (uint16_t)p->info.channels;
    uint32_t rate = p->info.sample_rate;
    memcpy(info, &dst, 8);
    memcpy(info + 0x10, &ms, 8);
    memcpy(info + 0x18, &ch, 2);
    memcpy(info + 0x1c, &rate, 4);
    memcpy(info + 0x20, &bytes, 4);
    memcpy(info + 0x24, "und", 4);
    gmem_write(c->mem, out, info, sizeof(info));
    hle_ret_u32(c, 1);
}

static void avp_close(hle_call *c) {
    av_player *p = get(hle_arg(c, 0));
    if (!p) {
        hle_ret_s32(c, AVP_ERR_INVALID_PARAMS);
        return;
    }
    player_free(c, p);
    hle_ret_s32(c, 0);
}

#define L "libSceAvPlayer"

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(L, "sceAvPlayerInit", avp_init),
    HLE_FUNC_ENTRY(L, "sceAvPlayerInitEx", avp_init_ex),
    HLE_FUNC_ENTRY(L, "sceAvPlayerPostInit", avp_ok),
    HLE_FUNC_ENTRY(L, "sceAvPlayerAddSource", avp_add_source),
    HLE_FUNC_ENTRY(L, "sceAvPlayerAddSourceEx", avp_add_source_ex),
    HLE_FUNC_ENTRY(L, "sceAvPlayerStart", avp_start),
    HLE_FUNC_ENTRY(L, "sceAvPlayerStop", avp_stop),
    HLE_FUNC_ENTRY(L, "sceAvPlayerPause", avp_pause),
    HLE_FUNC_ENTRY(L, "sceAvPlayerResume", avp_resume),
    HLE_FUNC_ENTRY(L, "sceAvPlayerSetLooping", avp_set_looping),
    HLE_FUNC_ENTRY(L, "sceAvPlayerJumpToTime", avp_jump_to_time),
    HLE_FUNC_ENTRY(L, "sceAvPlayerIsActive", avp_is_active),
    HLE_FUNC_ENTRY(L, "sceAvPlayerCurrentTime", avp_current_time),
    HLE_FUNC_ENTRY(L, "sceAvPlayerStreamCount", avp_stream_count),
    HLE_FUNC_ENTRY(L, "sceAvPlayerGetStreamInfo", avp_get_stream_info),
    HLE_FUNC_ENTRY(L, "sceAvPlayerGetStreamInfoEx", avp_get_stream_info_ex),
    HLE_FUNC_ENTRY(L, "sceAvPlayerEnableStream", avp_ok),
    HLE_FUNC_ENTRY(L, "sceAvPlayerDisableStream", avp_ok),
    HLE_FUNC_ENTRY(L, "sceAvPlayerChangeStream", avp_ok),
    HLE_FUNC_ENTRY(L, "sceAvPlayerSetAvSyncMode", avp_ok),
    HLE_FUNC_ENTRY(L, "sceAvPlayerSetTrickSpeed", avp_ok),
    HLE_FUNC_ENTRY(L, "sceAvPlayerSetLogCallback", avp_ok),
    HLE_FUNC_ENTRY(L, "sceAvPlayerSetAvailableBandwidth", avp_ok),
    HLE_FUNC_ENTRY(L, "sceAvPlayerGetVideoData", avp_get_video_data),
    HLE_FUNC_ENTRY(L, "sceAvPlayerGetVideoDataEx", avp_get_video_data_ex),
    HLE_FUNC_ENTRY(L, "sceAvPlayerGetAudioData", avp_get_audio_data),
    HLE_FUNC_ENTRY(L, "sceAvPlayerClose", avp_close),
    HLE_FUNC_ENTRY(L, "__ps5c_avplayer_event_thread", avp_event_thread),
};

void hle_register_av_player(void) {
    g_avp.trace = getenv("PS5C_AVPLAYER_TRACE") != NULL;
    if (!g_avp.lock && pal_mutex_create(&g_avp.lock) != PS5C_OK) {
        LOG_E(HLE, "cannot create the AvPlayer lock");
        return;
    }
    if (!g_avp.events && pal_cond_create(&g_avp.events) != PS5C_OK) g_avp.events = NULL;
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}
