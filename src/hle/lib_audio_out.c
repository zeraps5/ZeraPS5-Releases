/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceAudioOut (and libSceAudioIn). Ports accept buffers at the rate a real
 * device consumes them, so the game's mixer threads keep their real timing.
 * Each buffer is converted to stereo float into the port's queue; the host
 * audio thread (pal_audio) mixes the queues of the ports that reach the
 * speakers. The microphone records silence. */
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/audio_mix.h"
#include "hle/hle.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

#define AO_ERR_NOT_INIT ((int32_t)0x8026000F)
#define AO_ERR_INVALID_PORT ((int32_t)0x80260003)
#define AO_ERR_INVALID_SIZE ((int32_t)0x80260006)
#define AO_ERR_INVALID_FREQ ((int32_t)0x80260008)
#define AO_ERR_INVALID_FORMAT ((int32_t)0x80260007)
#define AO_ERR_PORT_FULL ((int32_t)0x80260005)
#define AO_ERR_OUT_OF_MEMORY ((int32_t)0x8026000D)

#define MAX_PORTS 16
#define PORT_HANDLE_BASE 0x100

#define PORT_TYPE_PADSPK 4 /* the controller's speaker: not mixed */

typedef struct ao_port {
    bool open;
    int32_t type;
    uint32_t samples; /* per buffer */
    uint32_t freq;
    uint32_t format;
    uint64_t period_ns;
    uint64_t next_ns; /* when the device finishes the queued buffers */
    uint64_t buffers;
    float volume[2]; /* left, right: 1.0 = 0 dB */
    int stream;      /* audio_mix stream, -1 if none */
} ao_port;

static struct {
    pal_mutex *lock;
    bool init;
    ao_port ports[MAX_PORTS];
} g_ao;

/* Formats: 0 S16 mono, 1 S16 stereo, 2 S16 8ch, 3 float mono, 4 float stereo,
 * 5 float 8ch, 6 S16 8ch (standard order), 7 float 8ch (standard order). */
static uint32_t format_channels(uint32_t f) { return f == 0 || f == 3 ? 1 : f == 1 || f == 4 ? 2 : 8; }
static bool format_float(uint32_t f) { return f == 3 || f == 4 || f == 5 || f == 7; }
static uint64_t buffer_bytes(const ao_port *p) {
    return (uint64_t)p->samples * format_channels(p->format) * (format_float(p->format) ? 4 : 2);
}

/* One buffer to the host mixer. */
static void enqueue(ao_port *p, const uint8_t *buf) {
    if (p->type == PORT_TYPE_PADSPK) return;
    audio_mix_write(p->stream, buf, p->samples, format_channels(p->format), format_float(p->format), p->volume[0],
                    p->volume[1]);
}

static void ao_init(hle_call *c) {
    if (!g_ao.lock && pal_mutex_create(&g_ao.lock) != PS5C_OK) {
        hle_ret_s32(c, AO_ERR_OUT_OF_MEMORY);
        return;
    }
    g_ao.init = true;
    hle_ret_s32(c, 0);
}

/* int sceAudioOutOpen(SceUserServiceUserId user, int type, int index, uint32_t len, uint32_t freq, uint32_t param) */
static void ao_open(hle_call *c) {
    int32_t type = hle_arg_s32(c, 1);
    uint32_t len = hle_arg_u32(c, 3), freq = hle_arg_u32(c, 4), param = hle_arg_u32(c, 5);
    if (!g_ao.init) {
        hle_ret_s32(c, AO_ERR_NOT_INIT);
        return;
    }
    if (len == 0 || len > 2048 || len % 256) {
        hle_ret_s32(c, AO_ERR_INVALID_SIZE);
        return;
    }
    if (freq != 48000) {
        hle_ret_s32(c, AO_ERR_INVALID_FREQ);
        return;
    }
    if ((param & 0xFF) > 7) {
        hle_ret_s32(c, AO_ERR_INVALID_FORMAT);
        return;
    }
    pal_mutex_lock(g_ao.lock);
    int32_t handle = AO_ERR_PORT_FULL;
    for (int i = 0; i < MAX_PORTS; i++) {
        ao_port *p = &g_ao.ports[i];
        if (p->open) continue;
        memset(p, 0, sizeof(*p));
        p->open = true;
        p->type = type;
        p->samples = len;
        p->freq = freq;
        p->format = param & 0xFF;
        p->period_ns = (uint64_t)len * 1000000000ull / freq;
        p->volume[0] = p->volume[1] = 1.0f;
        p->stream = type == PORT_TYPE_PADSPK ? -1 : audio_mix_open();
        handle = PORT_HANDLE_BASE + i;
        break;
    }
    pal_mutex_unlock(g_ao.lock);
    LOG_I(KERNEL, "audio port %d: type %d, %u samples at %u Hz, format %u", handle, type, len, freq, param & 0xFF);
    hle_ret_s32(c, handle);
}

static ao_port *port_arg(hle_call *c) {
    int32_t h = hle_arg_s32(c, 0) - PORT_HANDLE_BASE;
    if (!g_ao.init || h < 0 || h >= MAX_PORTS || !g_ao.ports[h].open) return NULL;
    return &g_ao.ports[h];
}

static void ao_close(hle_call *c) {
    ao_port *p = port_arg(c);
    if (!p) {
        hle_ret_s32(c, AO_ERR_INVALID_PORT);
        return;
    }
    pal_mutex_lock(g_ao.lock);
    p->open = false;
    audio_mix_close(p->stream);
    p->stream = -1;
    pal_mutex_unlock(g_ao.lock);
    hle_ret_s32(c, 0);
}

/* Queues one buffer: returns once the device has room, as the real call
 * does (one buffer in flight). A NULL buffer waits for the queue to drain.
 * With host sound, the host device's consumption paces the port (its clock
 * and ours drift apart); otherwise our clock does. */
static void wait_port(ao_port *p, bool queue) {
    if (p->stream >= 0 && audio_mix_active()) {
        uint32_t keep = queue ? AUDIO_QUEUE_TARGET : 0; /* about 43 ms ahead of the device */
        for (int i = 0; i < 500 && audio_mix_queued(p->stream) > keep; i++) pal_sleep_ns(1000000);
        return;
    }
    pal_mutex_lock(g_ao.lock);
    uint64_t now = pal_time_ns();
    if (p->next_ns < now) p->next_ns = now; /* underrun: restart the clock */
    uint64_t wake = p->next_ns;
    if (queue) {
        p->next_ns += p->period_ns;
        p->buffers++;
    }
    pal_mutex_unlock(g_ao.lock);
    if (wake > now) pal_sleep_ns(wake - now);
}

/* int sceAudioOutOutput(int handle, const void *ptr) */
static void ao_output(hle_call *c) {
    ao_port *p = port_arg(c);
    if (!p) {
        hle_ret_s32(c, AO_ERR_INVALID_PORT);
        return;
    }
    guest_addr ptr = hle_arg_ptr(c, 1);
    if (ptr) {
        const uint8_t *buf = (const uint8_t *)hle_mem(c, ptr, buffer_bytes(p));
        if (buf) enqueue(p, buf);
    }
    wait_port(p, ptr != 0);
    hle_ret_s32(c, 0);
}

/* int sceAudioOutOutputs(SceAudioOutOutputParam *param, uint32_t num): {int32 handle; pad; const void *ptr} */
static void ao_outputs(hle_call *c) {
    uint32_t num = hle_arg_u32(c, 1);
    const uint8_t *params = (const uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), (uint64_t)num * 16);
    if (!params || num == 0) {
        hle_ret_s32(c, (int32_t)0x80260002); /* invalid pointer */
        return;
    }
    /* every port gets its buffer; all advance together: pace on the first */
    for (uint32_t i = 0; i < num; i++) {
        int32_t ph;
        uint64_t pp;
        memcpy(&ph, params + i * 16, 4);
        memcpy(&pp, params + i * 16 + 8, 8);
        ph -= PORT_HANDLE_BASE;
        if (ph < 0 || ph >= MAX_PORTS || !g_ao.ports[ph].open || !pp) continue;
        const uint8_t *buf = (const uint8_t *)hle_mem(c, pp, buffer_bytes(&g_ao.ports[ph]));
        if (buf) enqueue(&g_ao.ports[ph], buf);
    }
    int32_t h;
    uint64_t ptr;
    memcpy(&h, params, 4);
    memcpy(&ptr, params + 8, 8);
    h -= PORT_HANDLE_BASE;
    if (h < 0 || h >= MAX_PORTS || !g_ao.ports[h].open) {
        hle_ret_s32(c, AO_ERR_INVALID_PORT);
        return;
    }
    wait_port(&g_ao.ports[h], ptr != 0);
    hle_ret_s32(c, 0);
}

/* int sceAudioOutSetVolume(int handle, int32_t flag, int32_t *vol): bit n of
 * flag selects channel n; 32768 = 0 dB */
static void ao_set_volume(hle_call *c) {
    ao_port *p = port_arg(c);
    if (!p) {
        hle_ret_s32(c, AO_ERR_INVALID_PORT);
        return;
    }
    uint32_t flag = hle_arg_u32(c, 1);
    const int32_t *vol = (const int32_t *)hle_mem(c, hle_arg_ptr(c, 2), 8 * 4);
    if (vol) {
        pal_mutex_lock(g_ao.lock);
        if (flag & 1) p->volume[0] = (float)vol[0] / 32768.0f;
        if (flag & 2) p->volume[1] = (float)vol[1] / 32768.0f;
        if (format_channels(p->format) == 1 && (flag & 1)) p->volume[1] = p->volume[0];
        pal_mutex_unlock(g_ao.lock);
    }
    hle_ret_s32(c, 0);
}

/* int sceAudioOutGetPortState(int handle, SceAudioOutPortState *state): 32 bytes */
static void ao_get_port_state(hle_call *c) {
    ao_port *p = port_arg(c);
    uint8_t *st = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 1), 32);
    if (!p || !st) {
        hle_ret_s32(c, p ? (int32_t)0x80260002 : AO_ERR_INVALID_PORT);
        return;
    }
    memset(st, 0, 32);
    uint16_t output = 1;                         /* primary output */
    uint8_t channels = p->format == 0 || p->format == 3 ? 1 : p->format == 1 || p->format == 4 ? 2 : 8;
    memcpy(st, &output, 2);
    st[2] = channels;
    hle_ret_s32(c, 0);
}

/* int sceAudioOutGetLastOutputTime(int handle, uint64_t *time): when the port last
 * output, in microseconds of the process clock (now: output is continuous) */
static void ao_get_last_output_time(hle_call *c) {
    ao_port *p = port_arg(c);
    if (!p || !hle_arg_ptr(c, 1)) {
        hle_ret_s32(c, p ? (int32_t)0x80260002 : AO_ERR_INVALID_PORT);
        return;
    }
    uint64_t us = pal_time_ns() / 1000u;
    hle_ret_s32(c, gmem_write(c->mem, hle_arg_ptr(c, 1), &us, 8) ? 0 : (int32_t)0x80260002);
}

static void ao_accept(hle_call *c) { hle_ret_s32(c, 0); }

/* int sceAudioOutGetSystemState({float loudness; u8 reserved[28]} *): all zero (AnyPS5) */
static void ao_get_system_state(hle_call *c) {
    void *s = hle_arg_ptr(c, 0) ? hle_mem(c, hle_arg_ptr(c, 0), 32) : NULL;
    if (s) memset(s, 0, 32);
    hle_ret_s32(c, s ? 0 : (int32_t)0x80260004);
}

/* int sceAudioOutSetMixLevelPadSpk(int handle, uint32_t level): level 0..32768 of a pad
 * speaker port (those ports are not mixed here); error codes as in AnyPS5 */
static void ao_set_mix_level_pad_spk(hle_call *c) {
    ao_port *p = port_arg(c);
    hle_ret_s32(c, !p                               ? AO_ERR_INVALID_PORT
                   : hle_arg_u32(c, 1) > 32768      ? (int32_t)0x80260014 /* invalid volume */
                   : p->type != PORT_TYPE_PADSPK    ? (int32_t)0x8026000A /* invalid port type */
                                                    : 0);
}

/* ---- libSceAudioIn: a microphone that records silence at the real rate ---- */

#define AI_HANDLE_BASE 0x200
#define AI_MAX_PORTS 4

static struct {
    bool open;
    uint32_t samples, channels;
    uint64_t period_ns, next_ns;
} g_ai[AI_MAX_PORTS];

/* int sceAudioInOpen(SceUserServiceUserId user, uint32_t type, uint32_t index, uint32_t len, uint32_t freq, uint32_t param) */
static void ai_open(hle_call *c) {
    uint32_t len = hle_arg_u32(c, 3), freq = hle_arg_u32(c, 4), param = hle_arg_u32(c, 5);
    if (!len || len > 4096 || !freq) {
        hle_ret_s32(c, (int32_t)0x80260105); /* invalid parameter */
        return;
    }
    for (int i = 0; i < AI_MAX_PORTS; i++) {
        if (g_ai[i].open) continue;
        g_ai[i].open = true;
        g_ai[i].samples = len;
        g_ai[i].channels = (param & 0xFF) == 2 ? 2 : 1; /* S16 mono / stereo */
        g_ai[i].period_ns = (uint64_t)len * 1000000000ull / freq;
        g_ai[i].next_ns = 0;
        hle_ret_s32(c, AI_HANDLE_BASE + i);
        return;
    }
    hle_ret_s32(c, (int32_t)0x80260109); /* port full */
}

/* int sceAudioInAsyncOpen(user, type, index, len, freq, param): the same port, for
 * 128-sample periods at 48 kHz only (checks as in AnyPS5) */
static void ai_async_open(hle_call *c) {
    if (hle_arg_u32(c, 4) != 48000) { hle_ret_s32(c, (int32_t)0x80260103); return; } /* invalid freq */
    if (hle_arg_u32(c, 3) != 128) { hle_ret_s32(c, (int32_t)0x80260102); return; }   /* invalid size */
    ai_open(c);
}

/* int sceAudioInHqOpen(...): 128-sample periods at 48 kHz, stereo (param 2) only */
static void ai_hq_open(hle_call *c) {
    if (hle_arg_u32(c, 5) != 2) { hle_ret_s32(c, (int32_t)0x80260105); return; } /* invalid param */
    ai_async_open(c);
}

static int ai_index(hle_call *c) {
    int h = hle_arg_s32(c, 0) - AI_HANDLE_BASE;
    return h >= 0 && h < AI_MAX_PORTS && g_ai[h].open ? h : -1;
}

/* int sceAudioInInput(int handle, void *dest): blocks one period, returns the samples read */
static void ai_input(hle_call *c) {
    int h = ai_index(c);
    if (h < 0) {
        hle_ret_s32(c, (int32_t)0x80260101); /* invalid handle */
        return;
    }
    uint64_t bytes = (uint64_t)g_ai[h].samples * g_ai[h].channels * 2;
    void *dst = hle_mem(c, hle_arg_ptr(c, 1), bytes);
    if (dst) memset(dst, 0, bytes);
    uint64_t now = pal_time_ns();
    if (g_ai[h].next_ns < now) g_ai[h].next_ns = now;
    uint64_t wake = g_ai[h].next_ns;
    g_ai[h].next_ns += g_ai[h].period_ns;
    if (wake > now) pal_sleep_ns(wake - now);
    hle_ret_s32(c, (int32_t)g_ai[h].samples);
}

static void ai_close(hle_call *c) {
    int h = ai_index(c);
    if (h >= 0) g_ai[h].open = false;
    hle_ret_s32(c, h >= 0 ? 0 : (int32_t)0x80260101);
}

/* int sceAudioInGetSilentState(handle): 0, the input (always silence) is not muted */
static void ai_silent_state(hle_call *c) { hle_ret_s32(c, ai_index(c) >= 0 ? 0 : (int32_t)0x80260101); }

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY("libSceAudioIn", "sceAudioInOpen", ai_open),
    HLE_FUNC_ENTRY("libSceAudioIn", "sceAudioInAsyncOpen", ai_async_open),
    HLE_FUNC_ENTRY("libSceAudioIn", "sceAudioInHqOpen", ai_hq_open),
    HLE_FUNC_ENTRY("libSceAudioOut", "sceAudioOutGetSystemState", ao_get_system_state),
    HLE_FUNC_ENTRY("libSceAudioIn", "sceAudioInGetSilentState", ai_silent_state),
    HLE_FUNC_ENTRY("libSceAudioIn", "sceAudioInInput", ai_input),
    HLE_FUNC_ENTRY("libSceAudioIn", "sceAudioInClose", ai_close),
    HLE_FUNC_ENTRY("libSceAudioOut", "sceAudioOutInit", ao_init),
    HLE_FUNC_ENTRY("libSceAudioOut", "sceAudioOutOpen", ao_open),
    HLE_FUNC_ENTRY("libSceAudioOut", "sceAudioOutClose", ao_close),
    HLE_FUNC_ENTRY("libSceAudioOut", "sceAudioOutOutput", ao_output),
    HLE_FUNC_ENTRY("libSceAudioOut", "sceAudioOutOutputs", ao_outputs),
    HLE_FUNC_ENTRY("libSceAudioOut", "sceAudioOutSetVolume", ao_set_volume),
    HLE_FUNC_ENTRY("libSceAudioOut", "sceAudioOutSetMixLevelPadSpk", ao_set_mix_level_pad_spk),
    HLE_FUNC_ENTRY("libSceAudioOut", "sceAudioOutGetPortState", ao_get_port_state),
    HLE_FUNC_ENTRY("libSceAudioOut", "sceAudioOutGetLastOutputTime", ao_get_last_output_time),
    HLE_FUNC_ENTRY("libSceAudioOut", "sceAudioOutSetAllMute", ao_accept),
};

void hle_register_audio_out(void) { hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports)); }
