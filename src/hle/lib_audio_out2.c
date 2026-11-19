/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceAudioOut2 (the context/port audio API Unreal Engine uses): a context
 * consumes one pushed grain every num_grains / 48 kHz, so the game's audio
 * thread keeps its real pace. At each push, the grain every port of the
 * context points at (its PCM attribute) goes to the host mixer. */
#include <string.h>
#include <stdlib.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/audio_mix.h"
#include "hle/hle.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

#define AO2_ERR_INVALID_PARAM ((int32_t)0x80268001)
#define AO2_ERR_NOT_READY ((int32_t)0x80268008)
#define AO2_ERR_INVALID_CONTEXT ((int32_t)0x80268009)

#define MAX_CONTEXTS 16
#define MAX_PORTS 64
#define ATTRIBUTE_PCM 0

typedef struct ao2_context {
    uint64_t handle; /* 0 = free */
    uint32_t queue_depth;
    uint32_t queued;
    uint32_t grains; /* samples per grain */
    uint64_t grain_ns;
    uint64_t last_ns; /* when the last queued grain was accounted for */
} ao2_context;

/* data_format: bits 0-6 sample type (0 float, 1 S16), bit 7 standard channel
 * order, bits 8-15 channels (0 = 2). Types: 0 main, 1 BGM, 2 voice, 3 pad
 * speaker, 4 personal, 5 aux, 6 vibration; 0x1xx are object ports. */
typedef struct ao2_port {
    uint64_t handle; /* 0 = free */
    uint64_t ctx;
    uint32_t channels;
    bool is_float;
    guest_addr pcm; /* the grain to play at the next push */
    int stream;     /* audio_mix stream, -1 if not heard */
} ao2_port;

static struct {
    pal_mutex *lock;
    ao2_context ctx[MAX_CONTEXTS];
    ao2_port ports[MAX_PORTS];
    uint64_t next_handle;
} g_ao2;

/* SceAudioOut2ContextParam: max_ports, max_object_ports, guarantee_object_ports,
 * queue_depth, num_grains, flags, reserved[10] (all u32) */
typedef struct ao2_context_param {
    uint32_t max_ports, max_object_ports, guarantee_object_ports, queue_depth, num_grains, flags;
    uint32_t reserved[10];
} ao2_context_param;

static uint64_t next_handle(void) {
    pal_mutex_lock(g_ao2.lock);
    uint64_t h = ++g_ao2.next_handle;
    pal_mutex_unlock(g_ao2.lock);
    return h;
}

/* Caller holds the lock. Retires the grains the "device" played since last_ns. */
static void drain(ao2_context *x) {
    uint64_t now = pal_time_ns();
    if (!x->queued) {
        x->last_ns = now;
        return;
    }
    uint64_t played = (now - x->last_ns) / x->grain_ns;
    if (played >= x->queued) {
        x->queued = 0;
        x->last_ns = now;
    } else {
        x->queued -= (uint32_t)played;
        x->last_ns += played * x->grain_ns;
    }
}

static ao2_context *find(uint64_t handle) {
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        if (handle && g_ao2.ctx[i].handle == handle) return &g_ao2.ctx[i];
    }
    return NULL;
}

static void ok(hle_call *c) { hle_ret_s32(c, 0); }

static void context_reset_param(hle_call *c) {
    ao2_context_param *p = (ao2_context_param *)hle_mem(c, hle_arg_ptr(c, 0), sizeof(ao2_context_param));
    if (!p) {
        hle_ret_s32(c, AO2_ERR_INVALID_PARAM);
        return;
    }
    memset(p, 0, sizeof(*p));
    p->max_ports = 256;
    p->max_object_ports = 256;
    p->queue_depth = 4;
    p->num_grains = 512;
    p->flags = 1;
    hle_ret_s32(c, 0);
}

/* int sceAudioOut2ContextQueryMemory(const param *p, size_t *size) */
static void context_query_memory(hle_call *c) {
    const ao2_context_param *p = (const ao2_context_param *)hle_mem(c, hle_arg_ptr(c, 0), sizeof(ao2_context_param));
    uint64_t *size = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 1), 8);
    if (!p || !size) {
        hle_ret_s32(c, AO2_ERR_INVALID_PARAM);
        return;
    }
    *size = 0x10000u + (uint64_t)(p->queue_depth ? p->queue_depth : 4) * 0x590u;
    hle_ret_s32(c, 0);
}

/* int sceAudioOut2ContextCreate(const param *p, void *memory, size_t size, handle *out) */
static void context_create(hle_call *c) {
    const ao2_context_param *p = (const ao2_context_param *)hle_mem(c, hle_arg_ptr(c, 0), sizeof(ao2_context_param));
    uint64_t *out = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 3), 8);
    if (!p || !out) {
        hle_ret_s32(c, AO2_ERR_INVALID_PARAM);
        return;
    }
    uint64_t h = next_handle();
    pal_mutex_lock(g_ao2.lock);
    ao2_context *x = NULL;
    for (int i = 0; i < MAX_CONTEXTS && !x; i++) {
        if (!g_ao2.ctx[i].handle) x = &g_ao2.ctx[i];
    }
    if (x) {
        uint32_t grains = p->num_grains ? p->num_grains : 512;
        x->handle = h;
        x->grains = grains;
        x->queue_depth = p->queue_depth ? p->queue_depth : 4;
        x->queued = 0;
        x->grain_ns = (uint64_t)grains * 1000000000u / 48000u;
        x->last_ns = pal_time_ns();
    }
    pal_mutex_unlock(g_ao2.lock);
    if (!x) {
        hle_ret_s32(c, AO2_ERR_INVALID_PARAM);
        return;
    }
    *out = h;
    if (getenv("PS5C_LOG_AUDIO"))
        LOG_I(KERNEL, "audioout2 context %llu: %u grains, depth %u", (unsigned long long)h,
              p->num_grains, p->queue_depth);
    hle_ret_s32(c, 0);
}

static void context_destroy(hle_call *c) {
    pal_mutex_lock(g_ao2.lock);
    ao2_context *x = find(hle_arg(c, 0));
    if (x) {
        for (int i = 0; i < MAX_PORTS; i++) {
            ao2_port *pt = &g_ao2.ports[i];
            if (!pt->handle || pt->ctx != x->handle) continue;
            audio_mix_close(pt->stream);
            memset(pt, 0, sizeof(*pt));
        }
        x->handle = 0;
    }
    pal_mutex_unlock(g_ao2.lock);
    hle_ret_s32(c, x ? 0 : AO2_ERR_INVALID_CONTEXT);
}

static void context_advance(hle_call *c) {
    pal_mutex_lock(g_ao2.lock);
    ao2_context *x = find(hle_arg(c, 0));
    if (x) drain(x);
    pal_mutex_unlock(g_ao2.lock);
    hle_ret_s32(c, x ? 0 : AO2_ERR_INVALID_CONTEXT);
}

/* The grain of every port of the context goes to the mixer (lock held). */
static void play_grain(hle_call *c, const ao2_context *x) {
    for (int i = 0; i < MAX_PORTS; i++) {
        const ao2_port *pt = &g_ao2.ports[i];
        if (!pt->handle || pt->ctx != x->handle || pt->stream < 0 || !pt->pcm) continue;
        uint64_t bytes = (uint64_t)x->grains * pt->channels * (pt->is_float ? 4 : 2);
        const void *buf = hle_mem(c, pt->pcm, bytes);
        if (buf) audio_mix_write(pt->stream, buf, x->grains, pt->channels, pt->is_float, 1.0f, 1.0f);
    }
}

/* Frames the most loaded heard port of the context has queued on the host,
 * -1 without a heard port (lock held). */
static int64_t heard_level(const ao2_context *x) {
    int64_t level = -1;
    if (!audio_mix_active()) return -1;
    for (int i = 0; i < MAX_PORTS; i++) {
        const ao2_port *pt = &g_ao2.ports[i];
        if (!pt->handle || pt->ctx != x->handle || pt->stream < 0) continue;
        int64_t q = audio_mix_queued(pt->stream);
        if (q > level) level = q;
    }
    return level;
}

/* int sceAudioOut2ContextPush(handle ctx, uint32_t blocking): queues one grain;
 * a full queue blocks (or fails with NOT_READY) until the device plays one.
 * With heard ports, the host device's queue is the context's queue. */
static void context_push(hle_call *c) {
    bool blocking = hle_arg_u32(c, 1) != 0;
    for (;;) {
        pal_mutex_lock(g_ao2.lock);
        ao2_context *hx = find(hle_arg(c, 0));
        int64_t level = hx ? heard_level(hx) : -1;
        if (hx && level >= 0) {
            if (level < AUDIO_QUEUE_TARGET) { /* room: play it */
                play_grain(c, hx);
                pal_mutex_unlock(g_ao2.lock);
                hle_ret_s32(c, 0);
                return;
            }
            pal_mutex_unlock(g_ao2.lock);
            if (!blocking) {
                hle_ret_s32(c, AO2_ERR_NOT_READY);
                return;
            }
            pal_sleep_ns(1000000);
            continue;
        }
        pal_mutex_unlock(g_ao2.lock);
        pal_mutex_lock(g_ao2.lock);
        ao2_context *x = find(hle_arg(c, 0));
        if (!x) {
            pal_mutex_unlock(g_ao2.lock);
            hle_ret_s32(c, AO2_ERR_INVALID_CONTEXT);
            return;
        }
        drain(x);
        if (x->queued < x->queue_depth) {
            if (!x->queued) x->last_ns = pal_time_ns();
            x->queued++;
            play_grain(c, x);
            pal_mutex_unlock(g_ao2.lock);
            hle_ret_s32(c, 0);
            return;
        }
        uint64_t grain = x->grain_ns, wait = x->last_ns + grain - pal_time_ns();
        pal_mutex_unlock(g_ao2.lock);
        if (!blocking) {
            hle_ret_s32(c, AO2_ERR_NOT_READY);
            return;
        }
        pal_sleep_ns(wait < grain ? wait : grain);
    }
}

/* int sceAudioOut2ContextGetQueueLevel(handle ctx, uint32_t *level, uint32_t *available) */
static void context_get_queue_level(hle_call *c) {
    uint32_t *level = (uint32_t *)hle_mem(c, hle_arg_ptr(c, 1), 4);
    uint32_t *avail = (uint32_t *)hle_mem(c, hle_arg_ptr(c, 2), 4);
    pal_mutex_lock(g_ao2.lock);
    ao2_context *x = find(hle_arg(c, 0));
    if (x) {
        drain(x);
        uint32_t q = x->queued;
        int64_t heard = heard_level(x);
        if (heard >= 0) q = (uint32_t)((heard + x->grains - 1) / x->grains);
        if (q > x->queue_depth) q = x->queue_depth;
        if (level) *level = q;
        if (avail) *avail = x->queue_depth - q;
    }
    pal_mutex_unlock(g_ao2.lock);
    hle_ret_s32(c, x ? 0 : AO2_ERR_INVALID_CONTEXT);
}

/* Handles for ports and users: any distinct non-zero value. */
static void create_handle(hle_call *c, int out_arg) {
    uint64_t *out = (uint64_t *)hle_mem(c, hle_arg_ptr(c, out_arg), 8);
    if (!out) {
        hle_ret_s32(c, AO2_ERR_INVALID_PARAM);
        return;
    }
    *out = next_handle();
    hle_ret_s32(c, 0);
}

/* int sceAudioOut2PortCreate(handle ctx, const SceAudioOut2PortParam *p, handle *out):
 * param { u16 port_type; u16 pad; u32 data_format; u32 sampling_freq; ... } */
static void port_create(hle_call *c) {
    const uint8_t *prm = (const uint8_t *)hle_mem(c, hle_arg_ptr(c, 1), 16);
    uint64_t *out = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 2), 8);
    if (!prm || !out) {
        hle_ret_s32(c, AO2_ERR_INVALID_PARAM);
        return;
    }
    uint16_t type;
    uint32_t format;
    memcpy(&type, prm, 2);
    memcpy(&format, prm + 4, 4);
    uint64_t h = next_handle();
    uint32_t channels = (format >> 8) & 0xFF;
    bool heard = (type & 0xFF00) == 0 && (type & 0xFF) != 3 && (type & 0xFF) != 6 && (format & 0x7F) <= 1;
    pal_mutex_lock(g_ao2.lock);
    ao2_port *pt = NULL;
    uint32_t accepted_channels = 0;
    int stream = -1;
    for (int i = 0; i < MAX_PORTS && !pt; i++) {
        if (!g_ao2.ports[i].handle) pt = &g_ao2.ports[i];
    }
    if (pt) {
        memset(pt, 0, sizeof(*pt));
        pt->handle = h;
        pt->ctx = hle_arg(c, 0);
        pt->channels = channels ? (channels > 16 ? 16 : channels) : 2;
        pt->is_float = (format & 0x7F) == 0;
        pt->stream = heard ? audio_mix_open() : -1;
        accepted_channels = pt->channels;
        stream = pt->stream;
    }
    pal_mutex_unlock(g_ao2.lock);
    *out = h; /* ports past the table still get a handle: they are only not heard */
    static unsigned port_trace;
    if (getenv("PS5C_LOG_AUDIO") && __atomic_fetch_add(&port_trace, 1, __ATOMIC_RELAXED) < 16) {
        uint32_t frequency;
        memcpy(&frequency, prm + 8, 4);
        LOG_I(KERNEL, "audioout2 port %llu: type %04x format %08x, %u Hz, %u channels, stream %d",
              (unsigned long long)h, type, format, frequency, accepted_channels, stream);
    }
    hle_ret_s32(c, 0);
}

static void port_destroy(hle_call *c) {
    pal_mutex_lock(g_ao2.lock);
    for (int i = 0; i < MAX_PORTS; i++) {
        ao2_port *pt = &g_ao2.ports[i];
        if (!pt->handle || pt->handle != hle_arg(c, 0)) continue;
        audio_mix_close(pt->stream);
        memset(pt, 0, sizeof(*pt));
    }
    pal_mutex_unlock(g_ao2.lock);
    hle_ret_s32(c, 0);
}

/* int sceAudioOut2PortSetAttributes(handle port, const SceAudioOut2Attribute *a, uint32_t num):
 * attribute { u32 id; s32 reserved; const void *value; size_t size }; PCM: value -> { const void *data } */
static void port_set_attributes(hle_call *c) {
    uint32_t num = hle_arg_u32(c, 2);
    const uint8_t *attrs = num ? (const uint8_t *)hle_mem(c, hle_arg_ptr(c, 1), (uint64_t)num * 24) : NULL;
    if (num && !attrs) {
        hle_ret_s32(c, AO2_ERR_INVALID_PARAM);
        return;
    }
    for (uint32_t i = 0; i < num; i++) {
        uint32_t id;
        uint64_t value, size;
        memcpy(&id, attrs + i * 24, 4);
        memcpy(&value, attrs + i * 24 + 8, 8);
        memcpy(&size, attrs + i * 24 + 16, 8);
        if (id != ATTRIBUTE_PCM || !value || size < 8) continue;
        uint64_t data = 0;
        if (!gmem_read_u64(c->mem, value, &data)) continue;
        pal_mutex_lock(g_ao2.lock);
        for (int k = 0; k < MAX_PORTS; k++) {
            if (g_ao2.ports[k].handle && g_ao2.ports[k].handle == hle_arg(c, 0)) g_ao2.ports[k].pcm = data;
        }
        pal_mutex_unlock(g_ao2.lock);
    }
    hle_ret_s32(c, 0);
}
/* int sceAudioOut2UserCreate(uint32_t user_id, handle *out) */
static void user_create(hle_call *c) { create_handle(c, 1); }

/* int sceAudioOut2PortGetState(handle port, SceAudioOut2PortState *state):
 * u16 output, u8 channels, u8 pad, s16 volume, u16 reroute, u32 flags, ... (64 bytes) */
static void port_get_state(hle_call *c) {
    uint8_t *s = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 1), 64);
    if (!s) {
        hle_ret_s32(c, AO2_ERR_INVALID_PARAM);
        return;
    }
    memset(s, 0, 64);
    uint16_t output = 1;
    int16_t volume = 127;
    memcpy(s, &output, 2);
    /* Report the PCM layout accepted at creation. A constant stereo state
     * makes middleware rebuild an eight-channel sink with a different stride. */
    s[2] = 2;
    pal_mutex_lock(g_ao2.lock);
    for (int i = 0; i < MAX_PORTS; i++) {
        if (g_ao2.ports[i].handle && g_ao2.ports[i].handle == hle_arg(c, 0)) {
            s[2] = (uint8_t)g_ao2.ports[i].channels;
            break;
        }
    }
    pal_mutex_unlock(g_ao2.lock);
    memcpy(s + 4, &volume, 2);
    hle_ret_s32(c, 0);
}

/* int sceAudioOut2GetSystemState(SceAudioOut2SystemState *state) (64 bytes) */
static void get_system_state(hle_call *c) {
    uint8_t *s = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 64);
    if (s) memset(s, 0, 64);
    hle_ret_s32(c, s ? 0 : AO2_ERR_INVALID_PARAM);
}

/* int sceAudioOut2GetSpeakerInfo(SceAudioOut2SpeakerInfo *info, uint32_t output).
 * Middleware reads output type at +0, speaker mask at +4, headphone flags at
 * +8 and 64 bytes of extended information at +16. Returning success without
 * writing this structure exposes the caller's stack as a changing device
 * configuration, repeatedly destroying and reopening its audio sinks.
 * The host mixer currently exposes one conventional stereo speaker output. */
static void get_speaker_info(hle_call *c) {
    uint8_t *info = hle_mem(c, hle_arg_ptr(c, 0), 80);
    if (!info) {
        hle_ret_s32(c, AO2_ERR_INVALID_PARAM);
        return;
    }
    memset(info, 0, 80);
    uint32_t speakers = 3; /* front left and front right; no headphone/3D mode */
    memcpy(info + 4, &speakers, sizeof(speakers));
    hle_ret_s32(c, 0);
}

#define L "libSceAudioOut2"

/* Speaker arrays (ambisonics decoding to the room's speakers): the memory
 * size is generous and the rest is logged and succeeds (GTA V asserts on a
 * zero size) */
static void speaker_array_memory_size(hle_call *c) { hle_ret_u64(c, 0x10000); }

static void speaker_array_call(hle_call *c) {
    static uint32_t logged;
    if (logged++ < 8)
        LOG_I(HLE, "audioout2 speaker array call (0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx)",
              (unsigned long long)hle_arg(c, 0), (unsigned long long)hle_arg(c, 1), (unsigned long long)hle_arg(c, 2),
              (unsigned long long)hle_arg(c, 3), (unsigned long long)hle_arg(c, 4), (unsigned long long)hle_arg(c, 5));
    hle_ret_s32(c, 0);
}

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(L, "sceAudioOut2Initialize", ok),
    HLE_FUNC_ENTRY(L, "sceAudioOut2ContextResetParam", context_reset_param),
    HLE_FUNC_ENTRY(L, "sceAudioOut2ContextQueryMemory", context_query_memory),
    HLE_FUNC_ENTRY(L, "sceAudioOut2ContextCreate", context_create),
    HLE_FUNC_ENTRY(L, "sceAudioOut2ContextDestroy", context_destroy),
    HLE_FUNC_ENTRY(L, "sceAudioOut2ContextSetAttributes", ok),
    HLE_FUNC_ENTRY(L, "sceAudioOut2ContextAdvance", context_advance),
    HLE_FUNC_ENTRY(L, "sceAudioOut2ContextPush", context_push),
    HLE_FUNC_ENTRY(L, "sceAudioOut2ContextGetQueueLevel", context_get_queue_level),
    HLE_FUNC_ENTRY(L, "sceAudioOut2PortCreate", port_create),
    HLE_FUNC_ENTRY(L, "sceAudioOut2PortDestroy", port_destroy),
    HLE_FUNC_ENTRY(L, "sceAudioOut2PortSetAttributes", port_set_attributes),
    HLE_FUNC_ENTRY(L, "sceAudioOut2PortGetState", port_get_state),
    HLE_FUNC_ENTRY(L, "sceAudioOut2UserCreate", user_create),
    HLE_FUNC_ENTRY(L, "sceAudioOut2UserDestroy", ok),
    HLE_FUNC_ENTRY(L, "sceAudioOut2GetSystemState", get_system_state),
    HLE_FUNC_ENTRY(L, "sceAudioOut2MasteringInit", ok),
    HLE_FUNC_ENTRY(L, "sceAudioOut2MasteringSetParam", ok),
    HLE_FUNC_ENTRY(L, "sceAudioOut2MasteringTerm", ok),
    HLE_FUNC_ENTRY(L, "sceAudioOut2Set3DLatency", ok), /* the mixer has no 3D audio latency to match */
    HLE_FUNC_ENTRY(L, "sceAudioOut2GetSpeakerArrayMemorySize", speaker_array_memory_size),
    HLE_FUNC_ENTRY(L, "sceAudioOut2SpeakerArrayCreate", speaker_array_call),
    HLE_FUNC_ENTRY(L, "sceAudioOut2SpeakerArrayDestroy", ok),
    HLE_FUNC_ENTRY(L, "sceAudioOut2GetSpeakerArrayAmbisonicsCoefficients", speaker_array_call),
    HLE_FUNC_ENTRY(L, "sceAudioOut2GetSpeakerInfo", get_speaker_info),
};

void hle_register_audio_out2(void) {
    if (!g_ao2.lock && pal_mutex_create(&g_ao2.lock) != PS5C_OK) {
        LOG_E(HLE, "cannot create the AudioOut2 lock");
        return;
    }
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}
