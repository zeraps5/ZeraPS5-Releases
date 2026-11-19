/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceNgs2: the voice mixer many games play their sound through. A system
 * holds racks of voices; sampler voices play waveform blocks from guest
 * memory (16-bit or float PCM, ATRAC9 through FFmpeg), resampled to the
 * system rate; voices patch their ports into submixer and mastering voices
 * (with a volume and an optional matrix), and SystemRender mixes one grain
 * into the game's buffers through the mastering voices. Reverb voices run an
 * I3DL2 reverb (early reflections and a feedback delay network); sampler
 * low-pass filters are applied. Envelopes and user effects pass the sound
 * through untouched.
 *
 * Handles are addresses inside the buffers the game gives (or allocates
 * through its callbacks) for each system and rack, as on the console: a rack
 * takes RACK_HEADER bytes plus VOICE_STRIDE per voice, and a voice handle is
 * its slot. The state lives here, on the host.
 *
 * Voice parameters (VoiceControl): a list of {u16 size, s16 next, u32 id}
 * headers with their payload; id >> 16 is the rack type (0 for the common
 * ones: 1 matrix levels, 2 port volume, 3 port matrix, 4 port delay, 5 patch,
 * 6 event, 7 callback). Events: 1 play, 2 stop, 4/8 kill, 16 pause, 32 resume. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/hle.h"
#include "pal/pal.h"

#ifdef PS5C_HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#endif

void hle_register_table(const hle_export *table, size_t count);

#define NGS2_ERR_INVALID_OUT_ADDRESS ((int32_t)0x804A8010)
#define NGS2_ERR_INVALID_OUT_SIZE ((int32_t)0x804A8011)
#define NGS2_ERR_INVALID_OPTION ((int32_t)0x804A8110)
#define NGS2_ERR_INVALID_SYSTEM_HANDLE ((int32_t)0x804A8201)
#define NGS2_ERR_INVALID_RACK_HANDLE ((int32_t)0x804A8202)
#define NGS2_ERR_INVALID_VOICE_HANDLE ((int32_t)0x804A8203)
#define NGS2_ERR_INVALID_RACK_ID ((int32_t)0x804A8220)
#define NGS2_ERR_INVALID_VOICE_INDEX ((int32_t)0x804A8221)
#define NGS2_ERR_INVALID_WAVEFORM_DATA ((int32_t)0x804A8430)
#define NGS2_ERR_INVALID_WAVEFORM_FORMAT ((int32_t)0x804A8431)
#define NGS2_ERR_UNKNOWN_WAVEFORM_FORMAT ((int32_t)0x804A8432)
#define NGS2_ERR_FAIL ((int32_t)0x804A8001)

#define WAVE_PCM16 0x12
#define WAVE_FLOAT 0x18
#define WAVE_ATRAC9 0x40

#define RACK_SAMPLER 0x1000
#define RACK_SUBMIXER 0x2000
#define RACK_REVERB 0x2001
#define RACK_MASTERING 0x3000
#define RACK_CUSTOM_SAMPLER 0x4001
#define RACK_CUSTOM_SUBMIXER 0x4002
#define RACK_CUSTOM_MASTERING 0x4003

/* voice states (also their state flags: bit 0 in use, 1 playing, 2 paused, 3 stopping) */
#define ST_EMPTY 0
#define ST_PLAYING 3
#define ST_PAUSED 5
#define ST_STOPPED 0xB

#define MAX_SYSTEMS 16
#define MAX_RACKS 128
#define MAX_BLOCKS 32
#define MAX_CHANNELS 8
#define MAX_PORTS 16
#define MAX_MATRICES 16
#define MAX_GRAIN 1024
#define RACK_HEADER 0x1000
#define VOICE_STRIDE 0x100
#define MAX_CALLBACKS 256
#define CALLBACK_INFO_SIZE 56

typedef struct allocator {
    guest_addr alloc, free, user;
} allocator;

typedef struct ngs_system {
    bool used;
    guest_addr handle, buffer;
    uint64_t buffer_size;
    allocator alloc;
    uint32_t grain, max_grain, rate;
    uint64_t renders;
    char name[64];
    guest_addr scratch; /* guest page for callback infos */
} ngs_system;

typedef struct ngs_block {
    guest_addr data;
    uint64_t size;
    uint32_t repeats, skip, samples;
    uint64_t user;
    uint32_t cursor;      /* PCM: samples played; ATRAC9: samples produced */
    uint64_t byte_cursor; /* ATRAC9: bytes consumed */
    uint32_t repeated;
} ngs_block;

typedef struct ngs_port {
    guest_addr dest;
    float volume;
    int32_t matrix;
} ngs_port;

typedef struct ngs_rack ngs_rack;

/* A sampler filter (SceNgs2SamplerFilterParam): an RBJ biquad per channel */
#define MAX_FILTERS 8
typedef struct ngs_filter {
    bool enabled;
    uint64_t bypass; /* channel mask */
    double b0, b1, b2, a1, a2;
    double h[MAX_CHANNELS][4]; /* x1, x2, y1, y2 */
} ngs_filter;

/* The I3DL2 reverb of a reverb voice */
#define REV_LINES 8
#define REV_PRE_SECONDS 0.5
typedef struct ngs_reverb {
    float wet, dry;
    float early_gain, late_gain, in_coef, in_state;
    uint32_t early_delay, late_delay;
    float *pre;
    uint32_t pre_len, pre_pos;
    float *line[REV_LINES];
    uint32_t len[REV_LINES], pos[REV_LINES];
    float fb[REV_LINES], damp[REV_LINES], lp[REV_LINES];
    uint32_t rate;
    bool audible;  /* the wet path has any level */
    uint32_t tail; /* grains of tail left after the input went silent */
} ngs_reverb;

typedef struct ngs_voice {
    ngs_rack *rack;
    uint32_t index;
    uint32_t state, state_flags;
    uint32_t channels, wave_type, wave_rate, at9_config;
    guest_addr callback, callback_data;
    uint32_t callback_flags;
    ngs_block blocks[MAX_BLOCKS];
    uint32_t block_count;
    bool accepts_blocks;
    ngs_port ports[MAX_PORTS];
    float *matrices[MAX_MATRICES]; /* levels[dst * src_channels + src] */
    uint32_t matrix_levels[MAX_MATRICES];
    float out_matrix[MAX_CHANNELS * MAX_CHANNELS]; /* mastering: levels[src * out + dst] */
    uint32_t out_matrix_levels, output_id;
    bool band_gains;          /* mastering: full-band and LFE gains set (else 1) */
    float fbw_gain, lfe_gain;
    double phase, step, ratio;
    float cur[MAX_CHANNELS], next[MAX_CHANNELS];
    bool primed, starved;
    float samples[MAX_CHANNELS * MAX_GRAIN]; /* planar, one grain */
    bool has_samples, rendered, rendering;
    uint64_t decoded_samples, decoded_bytes;
    guest_addr last_data;
#ifdef PS5C_HAVE_FFMPEG
    AVCodecContext *at9;
    AVPacket *pkt;
    AVFrame *frame;
#endif
    uint32_t at9_sf_size, at9_sf_samples;
    uint8_t *at9_in; /* a superframe gathered across blocks */
    uint32_t at9_in_have;
    float *at9_pcm; /* the decoded superframe, interleaved */
    uint32_t at9_pcm_count, at9_pcm_pos;
    ngs_filter filters[MAX_FILTERS];
    ngs_reverb *reverb;
} ngs_voice;

struct ngs_rack {
    bool used;
    guest_addr handle, buffer;
    uint64_t buffer_size;
    allocator alloc;
    ngs_system *sys;
    uint32_t id, max_voices, max_ports, max_matrices, state_size;
    ngs_voice *voices;
};

typedef struct pending_callback {
    guest_addr fn;
    uint8_t info[CALLBACK_INFO_SIZE];
} pending_callback;

static struct {
    pal_mutex *lock;
    ngs_system systems[MAX_SYSTEMS];
    ngs_rack racks[MAX_RACKS];
    pending_callback callbacks[MAX_CALLBACKS];
    uint32_t callback_count;
    guest_addr alloc_scratch; /* a ContextBufferInfo for the allocator callbacks */
    uint32_t next_uid;
} g_ngs;

/* ---- Lookups ---- */

static ngs_system *find_system(guest_addr h) {
    for (uint32_t i = 0; h && i < MAX_SYSTEMS; i++)
        if (g_ngs.systems[i].used && g_ngs.systems[i].handle == h) return &g_ngs.systems[i];
    return NULL;
}

static ngs_rack *find_rack(guest_addr h) {
    for (uint32_t i = 0; h && i < MAX_RACKS; i++)
        if (g_ngs.racks[i].used && g_ngs.racks[i].handle == h) return &g_ngs.racks[i];
    return NULL;
}

static guest_addr voice_handle(const ngs_voice *v) {
    return v->rack->handle + RACK_HEADER + (guest_addr)v->index * VOICE_STRIDE;
}

static ngs_voice *find_voice(guest_addr h) {
    for (uint32_t i = 0; h && i < MAX_RACKS; i++) {
        ngs_rack *r = &g_ngs.racks[i];
        if (!r->used || h < r->handle + RACK_HEADER) continue;
        uint64_t off = h - r->handle - RACK_HEADER;
        if (off % VOICE_STRIDE == 0 && off / VOICE_STRIDE < r->max_voices) return &r->voices[off / VOICE_STRIDE];
    }
    return NULL;
}

/* ---- Guest allocator callbacks: int handler(SceNgs2ContextBufferInfo *) ----
 * ContextBufferInfo: host buffer, its size, reserved[5], user data (64 bytes) */

static bool guest_alloc(hle_call *c, const allocator *a, uint64_t size, guest_addr *out) {
    if (!g_ngs.alloc_scratch &&
        gmem_map(c->mem, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "ngs2 scratch", &g_ngs.alloc_scratch) != PS5C_OK)
        return false;
    uint64_t info[8] = {0, size, 0, 0, 0, 0, 0, a->user};
    gmem_write(c->mem, g_ngs.alloc_scratch, info, sizeof(info));
    uint64_t rax = 0;
    if (!runtime_call_nested(c->rt, a->alloc, g_ngs.alloc_scratch, 0, 0, &rax) || (int32_t)rax < 0) return false;
    gmem_read(c->mem, g_ngs.alloc_scratch, info, sizeof(info));
    *out = info[0];
    return info[0] != 0;
}

static void guest_free(hle_call *c, const allocator *a, guest_addr buffer, uint64_t size) {
    if (!a->free || !g_ngs.alloc_scratch) return;
    uint64_t info[8] = {buffer, size, 0, 0, 0, 0, 0, a->user};
    gmem_write(c->mem, g_ngs.alloc_scratch, info, sizeof(info));
    uint64_t rax;
    runtime_call_nested(c->rt, a->free, g_ngs.alloc_scratch, 0, 0, &rax);
}

static void read_allocator(hle_call *c, guest_addr at, allocator *a) {
    uint64_t v[3] = {0};
    gmem_read(c->mem, at, v, sizeof(v));
    a->alloc = v[0];
    a->free = v[1];
    a->user = v[2];
}

/* ---- Systems ----
 * SceNgs2SystemOption (144 bytes): size, name[64], scheduler options[4],
 * then u32 flags +0x68, max grain +0x6c, grain +0x70, rate +0x74, max voice channels +0x78 */

typedef struct system_option {
    uint32_t grain, max_grain, rate;
    char name[64];
} system_option;

static void read_system_option(hle_call *c, guest_addr at, system_option *o) {
    o->grain = 256;
    o->max_grain = 512;
    o->rate = 48000;
    memset(o->name, 0, sizeof(o->name));
    uint8_t buf[144];
    if (!at || !gmem_read(c->mem, at, buf, sizeof(buf))) return;
    uint32_t v[3];
    memcpy(v, buf + 0x6c, 12);
    if (v[0]) o->max_grain = v[0];
    if (v[1]) o->grain = v[1];
    if (v[2]) o->rate = v[2];
    memcpy(o->name, buf + 8, 63);
    if (o->grain > MAX_GRAIN) o->grain = MAX_GRAIN;
}

static void ngs_system_reset_option(hle_call *c) {
    guest_addr o = hle_arg_ptr(c, 0);
    uint8_t buf[144];
    memset(buf, 0, sizeof(buf));
    uint64_t size = sizeof(buf);
    uint32_t v[3] = {512, 256, 48000};
    memcpy(buf, &size, 8);
    memcpy(buf + 0x6c, v, 12);
    hle_ret_s32(c, o && gmem_write(c->mem, o, buf, sizeof(buf)) ? 0 : NGS2_ERR_INVALID_OUT_ADDRESS);
}

/* int sceNgs2SystemQueryBufferSize(const option *, SceNgs2ContextBufferInfo *out) */
static void ngs_system_query_buffer_size(hle_call *c) {
    uint64_t info[8] = {0, 0x1000, 0, 0, 0, 0, 0, 0};
    guest_addr out = hle_arg_ptr(c, 1);
    hle_ret_s32(c, out && gmem_write(c->mem, out, info, sizeof(info)) ? 0 : NGS2_ERR_INVALID_OUT_ADDRESS);
}

static ngs_system *system_new(hle_call *c, guest_addr option, guest_addr buffer, uint64_t size) {
    ngs_system *s = NULL;
    for (uint32_t i = 0; i < MAX_SYSTEMS && !s; i++)
        if (!g_ngs.systems[i].used) s = &g_ngs.systems[i];
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    system_option o;
    read_system_option(c, option, &o);
    s->used = true;
    s->handle = s->buffer = buffer;
    s->buffer_size = size;
    s->grain = o.grain;
    s->max_grain = o.max_grain;
    s->rate = o.rate;
    memcpy(s->name, o.name, sizeof(s->name));
    if (gmem_map(c->mem, 0, PS5C_ALIGN_UP((uint64_t)MAX_CALLBACKS * CALLBACK_INFO_SIZE, GUEST_PAGE_SIZE), GPROT_RW, 0,
                 "ngs2 callbacks", &s->scratch) != PS5C_OK)
        s->scratch = 0;
    LOG_I(HLE, "ngs2: system '%s' %u Hz, grain %u", s->name, s->rate, s->grain);
    return s;
}

/* int sceNgs2SystemCreate(const option *, const SceNgs2ContextBufferInfo *, SceNgs2Handle *out) */
static void ngs_system_create(hle_call *c) {
    uint64_t info[2] = {0};
    guest_addr out = hle_arg_ptr(c, 2);
    if (!out || !gmem_read(c->mem, hle_arg_ptr(c, 1), info, sizeof(info)) || !info[0]) {
        hle_ret_s32(c, NGS2_ERR_INVALID_OUT_ADDRESS);
        return;
    }
    pal_mutex_lock(g_ngs.lock);
    ngs_system *s = system_new(c, hle_arg_ptr(c, 0), info[0], info[1]);
    pal_mutex_unlock(g_ngs.lock);
    if (s) gmem_write_u64(c->mem, out, s->handle);
    hle_ret_s32(c, s ? 0 : NGS2_ERR_FAIL);
}

/* int sceNgs2SystemCreateWithAllocator(const option *, const SceNgs2BufferAllocator *, SceNgs2Handle *out) */
static void ngs_system_create_with_allocator(hle_call *c) {
    allocator a;
    read_allocator(c, hle_arg_ptr(c, 1), &a);
    guest_addr out = hle_arg_ptr(c, 2), buffer = 0;
    if (!out || !a.alloc || !guest_alloc(c, &a, 0x1000, &buffer)) {
        hle_ret_s32(c, NGS2_ERR_FAIL);
        return;
    }
    pal_mutex_lock(g_ngs.lock);
    ngs_system *s = system_new(c, hle_arg_ptr(c, 0), buffer, 0x1000);
    if (s) s->alloc = a;
    pal_mutex_unlock(g_ngs.lock);
    if (s) gmem_write_u64(c->mem, out, s->handle);
    hle_ret_s32(c, s ? 0 : NGS2_ERR_FAIL);
}

static void voice_release(ngs_voice *v);

/* int sceNgs2SystemDestroy(SceNgs2Handle, SceNgs2ContextBufferInfo *out) */
static void ngs_system_destroy(hle_call *c) {
    pal_mutex_lock(g_ngs.lock);
    ngs_system *s = find_system(hle_arg(c, 0));
    ngs_system copy;
    if (s) {
        for (uint32_t i = 0; i < MAX_RACKS; i++) {
            ngs_rack *r = &g_ngs.racks[i];
            if (!r->used || r->sys != s) continue;
            for (uint32_t k = 0; k < r->max_voices; k++) voice_release(&r->voices[k]);
            free(r->voices);
            memset(r, 0, sizeof(*r));
        }
        copy = *s;
        if (s->scratch)
            gmem_unmap(c->mem, s->scratch, PS5C_ALIGN_UP((uint64_t)MAX_CALLBACKS * CALLBACK_INFO_SIZE, GUEST_PAGE_SIZE));
        memset(s, 0, sizeof(*s));
    }
    pal_mutex_unlock(g_ngs.lock);
    if (!s) {
        hle_ret_s32(c, NGS2_ERR_INVALID_SYSTEM_HANDLE);
        return;
    }
    if (copy.alloc.alloc) guest_free(c, &copy.alloc, copy.buffer, copy.buffer_size);
    else if (hle_arg_ptr(c, 1)) {
        uint64_t info[8] = {copy.buffer, copy.buffer_size, 0, 0, 0, 0, 0, 0};
        gmem_write(c->mem, hle_arg_ptr(c, 1), info, sizeof(info));
    }
    hle_ret_s32(c, 0);
}

static void ngs_system_set_grain_samples(hle_call *c) {
    pal_mutex_lock(g_ngs.lock);
    ngs_system *s = find_system(hle_arg(c, 0));
    uint32_t n = hle_arg_u32(c, 1);
    if (s && n) s->grain = n > MAX_GRAIN ? MAX_GRAIN : n;
    pal_mutex_unlock(g_ngs.lock);
    hle_ret_s32(c, s ? 0 : NGS2_ERR_INVALID_SYSTEM_HANDLE);
}

/* int sceNgs2SystemGetInfo(SceNgs2Handle, SceNgs2SystemInfo *out, size_t size): 184 bytes */
static void ngs_system_get_info(hle_call *c) {
    guest_addr out = hle_arg_ptr(c, 1);
    if (!out) {
        hle_ret_s32(c, NGS2_ERR_INVALID_OUT_ADDRESS);
        return;
    }
    uint8_t buf[184];
    memset(buf, 0, sizeof(buf));
    pal_mutex_lock(g_ngs.lock);
    ngs_system *s = find_system(hle_arg(c, 0));
    if (s) {
        uint32_t racks = 0;
        for (uint32_t i = 0; i < MAX_RACKS; i++) racks += g_ngs.racks[i].used && g_ngs.racks[i].sys == s;
        memcpy(buf, s->name, 64);
        memcpy(buf + 64, &s->handle, 8);
        memcpy(buf + 72, &s->buffer, 8);
        memcpy(buf + 80, &s->buffer_size, 8);
        uint32_t v[5] = {(uint32_t)(s - g_ngs.systems) + 1, 64, s->max_grain, 1, racks};
        memcpy(buf + 136, v, sizeof(v));
        memcpy(buf + 168, &s->renders, 8);
        memcpy(buf + 176, &s->rate, 4);
        memcpy(buf + 180, &s->grain, 4);
    }
    pal_mutex_unlock(g_ngs.lock);
    if (s) gmem_write(c->mem, out, buf, hle_arg(c, 2) < sizeof(buf) ? hle_arg(c, 2) : sizeof(buf));
    hle_ret_s32(c, s ? 0 : NGS2_ERR_INVALID_SYSTEM_HANDLE);
}

/* ---- Racks ----
 * SceNgs2RackOption (176 bytes): size, name[64], u32 flags +0x48, max grain
 * +0x4c, max voices +0x50, max input delay blocks +0x54, max matrices +0x58,
 * max ports +0x5c, ...; each rack type's option follows it (a custom rack's
 * state size is at +0xb0). */

static bool rack_id_valid(uint32_t id) {
    return id == RACK_SAMPLER || id == RACK_SUBMIXER || id == RACK_REVERB || id == RACK_MASTERING ||
           id == RACK_CUSTOM_SAMPLER || id == RACK_CUSTOM_SUBMIXER || id == RACK_CUSTOM_MASTERING;
}

static bool is_sampler(uint32_t id) { return id == RACK_SAMPLER || id == RACK_CUSTOM_SAMPLER; }

typedef struct rack_option {
    uint32_t max_voices, max_ports, max_matrices, state_size;
} rack_option;

static void read_rack_option(hle_call *c, uint32_t id, guest_addr at, rack_option *o) {
    bool voices = is_sampler(id);
    o->max_voices = voices ? 256 : 1;
    o->max_ports = id == RACK_MASTERING ? 0 : 8;
    o->max_matrices = id == RACK_MASTERING ? 0 : 1;
    o->state_size = 0;
    uint32_t v[4];
    if (at && gmem_read(c->mem, at + 0x50, v, 16)) {
        o->max_voices = v[0];
        o->max_matrices = v[2];
        o->max_ports = v[3];
    }
    if (at && (id & 0xF000) == 0x4000) gmem_read(c->mem, at + 0xb0, &o->state_size, 4);
    if (o->max_voices > 4096) o->max_voices = 4096;
    if (o->max_ports > MAX_PORTS) o->max_ports = MAX_PORTS;
    if (o->max_matrices > MAX_MATRICES) o->max_matrices = MAX_MATRICES;
}

static uint64_t rack_buffer_size(uint32_t voices) { return RACK_HEADER + (uint64_t)voices * VOICE_STRIDE; }

/* int sceNgs2RackQueryBufferSize(uint32_t rack_id, const option *, SceNgs2ContextBufferInfo *out) */
static void ngs_rack_query_buffer_size(hle_call *c) {
    uint32_t id = hle_arg_u32(c, 0);
    guest_addr out = hle_arg_ptr(c, 2);
    if (!rack_id_valid(id)) {
        hle_ret_s32(c, NGS2_ERR_INVALID_RACK_ID);
        return;
    }
    rack_option o;
    read_rack_option(c, id, hle_arg_ptr(c, 1), &o);
    uint64_t info[8] = {0, rack_buffer_size(o.max_voices), 0, 0, 0, 0, 0, 0};
    hle_ret_s32(c, out && gmem_write(c->mem, out, info, sizeof(info)) ? 0 : NGS2_ERR_INVALID_OUT_ADDRESS);
}

static ngs_rack *rack_new(hle_call *c, ngs_system *s, uint32_t id, guest_addr option, guest_addr buffer, uint64_t size) {
    ngs_rack *r = NULL;
    for (uint32_t i = 0; i < MAX_RACKS && !r; i++)
        if (!g_ngs.racks[i].used) r = &g_ngs.racks[i];
    if (!r) return NULL;
    rack_option o;
    read_rack_option(c, id, option, &o);
    memset(r, 0, sizeof(*r));
    r->voices = (ngs_voice *)calloc(o.max_voices ? o.max_voices : 1, sizeof(ngs_voice));
    if (!r->voices) return NULL;
    r->used = true;
    r->handle = r->buffer = buffer;
    r->buffer_size = size;
    r->sys = s;
    r->id = id;
    r->max_voices = o.max_voices;
    r->max_ports = o.max_ports;
    r->max_matrices = o.max_matrices;
    r->state_size = o.state_size;
    for (uint32_t i = 0; i < r->max_voices; i++) {
        r->voices[i].rack = r;
        r->voices[i].index = i;
        r->voices[i].ratio = 1.0;
        for (uint32_t p = 0; p < MAX_PORTS; p++) r->voices[i].ports[p] = (ngs_port){0, 1.0f, -1};
    }
    LOG_I(HLE, "ngs2: rack 0x%x at 0x%llx with %u voices, %u ports, %u matrices", id, (unsigned long long)r->handle,
          r->max_voices, r->max_ports, r->max_matrices);
    return r;
}

/* int sceNgs2RackCreate(system, rack_id, const option *, const SceNgs2ContextBufferInfo *, SceNgs2Handle *out) */
static void ngs_rack_create(hle_call *c) {
    uint32_t id = hle_arg_u32(c, 1);
    uint64_t info[2] = {0};
    guest_addr out = hle_arg_ptr(c, 4);
    if (!rack_id_valid(id)) {
        hle_ret_s32(c, NGS2_ERR_INVALID_RACK_ID);
        return;
    }
    if (!out || !gmem_read(c->mem, hle_arg_ptr(c, 3), info, sizeof(info)) || !info[0]) {
        hle_ret_s32(c, NGS2_ERR_INVALID_OUT_ADDRESS);
        return;
    }
    pal_mutex_lock(g_ngs.lock);
    ngs_system *s = find_system(hle_arg(c, 0));
    ngs_rack *r = s ? rack_new(c, s, id, hle_arg_ptr(c, 2), info[0], info[1]) : NULL;
    pal_mutex_unlock(g_ngs.lock);
    if (r) gmem_write_u64(c->mem, out, r->handle);
    hle_ret_s32(c, r ? 0 : s ? NGS2_ERR_FAIL : NGS2_ERR_INVALID_SYSTEM_HANDLE);
}

/* int sceNgs2RackCreateWithAllocator(system, rack_id, const option *, const allocator *, SceNgs2Handle *out) */
static void ngs_rack_create_with_allocator(hle_call *c) {
    uint32_t id = hle_arg_u32(c, 1);
    guest_addr out = hle_arg_ptr(c, 4), buffer = 0;
    if (!rack_id_valid(id)) {
        hle_ret_s32(c, NGS2_ERR_INVALID_RACK_ID);
        return;
    }
    allocator a;
    read_allocator(c, hle_arg_ptr(c, 3), &a);
    rack_option o;
    read_rack_option(c, id, hle_arg_ptr(c, 2), &o);
    uint64_t size = rack_buffer_size(o.max_voices);
    if (!out || !a.alloc || !guest_alloc(c, &a, size, &buffer)) {
        hle_ret_s32(c, NGS2_ERR_FAIL);
        return;
    }
    pal_mutex_lock(g_ngs.lock);
    ngs_system *s = find_system(hle_arg(c, 0));
    ngs_rack *r = s ? rack_new(c, s, id, hle_arg_ptr(c, 2), buffer, size) : NULL;
    if (r) r->alloc = a;
    pal_mutex_unlock(g_ngs.lock);
    if (r) gmem_write_u64(c->mem, out, r->handle);
    hle_ret_s32(c, r ? 0 : s ? NGS2_ERR_FAIL : NGS2_ERR_INVALID_SYSTEM_HANDLE);
}

/* int sceNgs2RackDestroy(SceNgs2Handle, SceNgs2ContextBufferInfo *out) */
static void ngs_rack_destroy(hle_call *c) {
    pal_mutex_lock(g_ngs.lock);
    ngs_rack *r = find_rack(hle_arg(c, 0));
    ngs_rack copy;
    if (r) {
        for (uint32_t k = 0; k < r->max_voices; k++) voice_release(&r->voices[k]);
        free(r->voices);
        copy = *r;
        /* other voices patched into this rack's lose their destination */
        for (uint32_t i = 0; i < MAX_RACKS; i++) {
            ngs_rack *o = &g_ngs.racks[i];
            for (uint32_t k = 0; o->used && o != r && k < o->max_voices; k++)
                for (uint32_t p = 0; p < MAX_PORTS; p++) {
                    guest_addr d = o->voices[k].ports[p].dest;
                    if (d >= r->handle && d < r->handle + rack_buffer_size(r->max_voices)) o->voices[k].ports[p].dest = 0;
                }
        }
        memset(r, 0, sizeof(*r));
    }
    pal_mutex_unlock(g_ngs.lock);
    if (!r) {
        hle_ret_s32(c, NGS2_ERR_INVALID_RACK_HANDLE);
        return;
    }
    if (copy.alloc.alloc) guest_free(c, &copy.alloc, copy.buffer, copy.buffer_size);
    else if (hle_arg_ptr(c, 1)) {
        uint64_t info[8] = {copy.buffer, copy.buffer_size, 0, 0, 0, 0, 0, 0};
        gmem_write(c->mem, hle_arg_ptr(c, 1), info, sizeof(info));
    }
    hle_ret_s32(c, 0);
}

/* int sceNgs2RackGetVoiceHandle(SceNgs2Handle rack, uint32_t voice, SceNgs2Handle *out) */
static void ngs_rack_get_voice_handle(hle_call *c) {
    pal_mutex_lock(g_ngs.lock);
    ngs_rack *r = find_rack(hle_arg(c, 0));
    uint32_t i = hle_arg_u32(c, 1);
    guest_addr h = r && i < r->max_voices ? voice_handle(&r->voices[i]) : 0;
    pal_mutex_unlock(g_ngs.lock);
    if (getenv("PS5C_LOG_NGS2"))
        LOG_I(HLE, "ngs2: voice handle of rack 0x%llx voice %u -> 0x%llx (to 0x%llx)", (unsigned long long)hle_arg(c, 0), i,
              (unsigned long long)h, (unsigned long long)hle_arg_ptr(c, 2));
    if (h) gmem_write_u64(c->mem, hle_arg_ptr(c, 2), h);
    hle_ret_s32(c, h ? 0 : r ? NGS2_ERR_INVALID_VOICE_INDEX : NGS2_ERR_INVALID_RACK_HANDLE);
}

static void ngs_ok(hle_call *c) { hle_ret_s32(c, 0); }

/* int sceNgs2RackResetOption(uint32_t rack_id, option *out): the header part */
static void ngs_rack_reset_option(hle_call *c) {
    uint32_t id = hle_arg_u32(c, 0);
    guest_addr out = hle_arg_ptr(c, 1);
    if (!rack_id_valid(id) || !out) {
        hle_ret_s32(c, !out ? NGS2_ERR_INVALID_OUT_ADDRESS : NGS2_ERR_INVALID_RACK_ID);
        return;
    }
    uint8_t buf[176];
    memset(buf, 0, sizeof(buf));
    uint64_t size = sizeof(buf);
    uint32_t v[6] = {0, 512, is_sampler(id) ? 256u : 1u, id == RACK_MASTERING ? 1u : 0u, id == RACK_MASTERING ? 0u : 1u,
                     id == RACK_MASTERING ? 0u : 8u};
    memcpy(buf, &size, 8);
    memcpy(buf + 0x48, v, sizeof(v));
    gmem_write(c->mem, out, buf, sizeof(buf));
    hle_ret_s32(c, 0);
}

/* ---- Sampler playback ---- */

static void voice_release(ngs_voice *v) {
#ifdef PS5C_HAVE_FFMPEG
    avcodec_free_context(&v->at9);
    av_packet_free(&v->pkt);
    av_frame_free(&v->frame);
#endif
    free(v->at9_in);
    free(v->at9_pcm);
    if (v->reverb) {
        free(v->reverb->pre);
        for (uint32_t k = 0; k < REV_LINES; k++) free(v->reverb->line[k]);
        free(v->reverb);
        v->reverb = NULL;
    }
    v->at9_in = NULL;
    v->at9_pcm = NULL;
    for (uint32_t m = 0; m < MAX_MATRICES; m++) {
        free(v->matrices[m]);
        v->matrices[m] = NULL;
    }
}

static void queue_callback(ngs_voice *v, const ngs_block *b, uint32_t flag) {
    if (!v->callback || !(v->callback_flags & flag) || g_ngs.callback_count >= MAX_CALLBACKS) return;
    /* SceNgs2VoiceCallbackInfo: callback data, voice, u32 flag, reserved,
     * block user data, block data, u32 size, repeats, attributes, reserved */
    pending_callback *p = &g_ngs.callbacks[g_ngs.callback_count++];
    memset(p->info, 0, sizeof(p->info));
    uint64_t q[2] = {v->callback_data, voice_handle(v)};
    uint32_t size = (uint32_t)b->size, repeated = b->repeated;
    memcpy(p->info, q, 16);
    memcpy(p->info + 16, &flag, 4);
    memcpy(p->info + 24, &b->user, 8);
    memcpy(p->info + 32, &b->data, 8);
    memcpy(p->info + 40, &size, 4);
    memcpy(p->info + 44, &repeated, 4);
    p->fn = v->callback;
}

static void clear_blocks(ngs_voice *v) {
    v->block_count = 0;
    v->at9_in_have = v->at9_pcm_count = v->at9_pcm_pos = 0;
    v->primed = false;
    v->phase = 0;
}

/* The front block is done: repeat it or drop it */
static void finish_block(ngs_voice *v) {
    ngs_block *b = &v->blocks[0];
    v->last_data = b->data + b->size;
    if (b->repeats) {
        if (b->repeats != 0xFFFFFFFFu) b->repeats--;
        b->repeated++;
        b->cursor = 0;
        b->byte_cursor = 0;
        queue_callback(v, b, 2);
        return;
    }
    ngs_block done = *b;
    memmove(&v->blocks[0], &v->blocks[1], (size_t)(v->block_count - 1) * sizeof(ngs_block));
    v->block_count--;
    queue_callback(v, &done, 1);
}

#ifdef PS5C_HAVE_FFMPEG
static bool at9_setup(ngs_voice *v) {
    uint8_t cfg[4] = {(uint8_t)(v->at9_config >> 24), (uint8_t)(v->at9_config >> 16), (uint8_t)(v->at9_config >> 8),
                      (uint8_t)v->at9_config};
    if (cfg[0] != 0xFE) return false;
    uint32_t bits = (uint32_t)cfg[1] << 16 | (uint32_t)cfg[2] << 8 | cfg[3];
    static const uint8_t log2s[16] = {6, 6, 7, 7, 7, 8, 8, 8, 6, 6, 7, 7, 7, 8, 8, 8};
    uint32_t frames = 1u << ((bits >> 3) & 3);
    if (((bits >> 17) & 7) >= 6) cfg[1] = (uint8_t)(cfg[1] - (6u << 1));
    v->at9_sf_size = (((bits >> 5) & 0x7FF) + 1) * frames;
    v->at9_sf_samples = (1u << log2s[bits >> 20]) * frames;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_ATRAC9);
    v->at9 = codec ? avcodec_alloc_context3(codec) : NULL;
    v->pkt = av_packet_alloc();
    v->frame = av_frame_alloc();
    v->at9_in = (uint8_t *)malloc(v->at9_sf_size);
    v->at9_pcm = (float *)malloc((size_t)v->at9_sf_samples * MAX_CHANNELS * sizeof(float));
    if (!v->at9 || !v->pkt || !v->frame || !v->at9_in || !v->at9_pcm) return false;
    uint8_t extra[12] = {2, 0, 0, 0, cfg[0], cfg[1], cfg[2], cfg[3], 0, 0, 0, 0};
    v->at9->extradata = (uint8_t *)av_mallocz(12 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!v->at9->extradata) return false;
    memcpy(v->at9->extradata, extra, 12);
    v->at9->extradata_size = 12;
    v->at9->block_align = (int)v->at9_sf_size;
    return avcodec_open2(v->at9, codec, NULL) >= 0;
}

static bool at9_decode(ngs_voice *v, const uint8_t *sf) {
    if (av_new_packet(v->pkt, (int)v->at9_sf_size) < 0) return false;
    memcpy(v->pkt->data, sf, v->at9_sf_size);
    int ret = avcodec_send_packet(v->at9, v->pkt);
    av_packet_unref(v->pkt);
    if (ret < 0 || avcodec_receive_frame(v->at9, v->frame) < 0) return false;
    AVFrame *f = v->frame;
    uint32_t chs = v->channels, n = (uint32_t)f->nb_samples;
    if (n > v->at9_sf_samples) n = v->at9_sf_samples;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t ch = 0; ch < chs; ch++)
            v->at9_pcm[i * chs + ch] =
                ch < (uint32_t)f->ch_layout.nb_channels && f->format == AV_SAMPLE_FMT_FLTP ? ((const float *)f->extended_data[ch])[i] : 0.0f;
    av_frame_unref(f);
    v->at9_pcm_count = n;
    v->at9_pcm_pos = 0;
    return true;
}
#endif

/* The next source sample frame of a sampler voice (channels floats); false when out of data */
static bool pull_frame(hle_call *c, ngs_voice *v, float *out) {
    for (int guard = 0; guard < 64; guard++) {
        if (!v->block_count) return false;
        ngs_block *b = &v->blocks[0];
        if (v->wave_type == WAVE_ATRAC9) {
#ifdef PS5C_HAVE_FFMPEG
            if (b->samples && b->cursor >= b->samples) {
                v->at9_pcm_count = v->at9_pcm_pos = 0;
                finish_block(v);
                continue;
            }
            if (v->at9_pcm_pos < v->at9_pcm_count) {
                const float *s = v->at9_pcm + (size_t)v->at9_pcm_pos++ * v->channels;
                if (b->cursor < b->skip) { /* the block's leading samples are skipped */
                    b->cursor++;
                    if (b->samples) b->samples++;
                    continue;
                }
                memcpy(out, s, v->channels * sizeof(float));
                b->cursor++;
                v->decoded_samples++;
                return true;
            }
            /* decode the next superframe (gathered across blocks when split) */
            uint64_t left = b->size - b->byte_cursor;
            uint32_t need = v->at9_sf_size - v->at9_in_have;
            uint32_t take = left < need ? (uint32_t)left : need;
            if (take) {
                const uint8_t *src = (const uint8_t *)gmem_host(c->mem, b->data + b->byte_cursor, take);
                if (!src) return false;
                memcpy(v->at9_in + v->at9_in_have, src, take);
                v->at9_in_have += take;
                b->byte_cursor += take;
                v->decoded_bytes += take;
            }
            if (v->at9_in_have == v->at9_sf_size) {
                v->at9_in_have = 0;
                if (!at9_decode(v, v->at9_in)) return false;
                continue;
            }
            finish_block(v); /* the data ran out before the samples did */
            continue;
#else
            return false;
#endif
        }
        if (b->cursor >= b->samples) {
            finish_block(v);
            continue;
        }
        uint32_t bytes = v->wave_type == WAVE_FLOAT ? 4 : 2;
        uint64_t at = b->data + ((uint64_t)b->skip + b->cursor) * v->channels * bytes;
        uint8_t raw[MAX_CHANNELS * 4];
        if (!gmem_read(c->mem, at, raw, (uint64_t)v->channels * bytes)) return false;
        for (uint32_t ch = 0; ch < v->channels; ch++) {
            if (bytes == 4) memcpy(&out[ch], raw + ch * 4, 4);
            else {
                int16_t s;
                memcpy(&s, raw + ch * 2, 2);
                out[ch] = s / 32768.0f;
            }
        }
        b->cursor++;
        v->decoded_samples++;
        v->decoded_bytes += (uint64_t)v->channels * bytes;
        return true;
    }
    return false;
}

/* One grain of a sampler voice, linearly resampled to the system rate */
static void render_sampler(hle_call *c, ngs_voice *v, uint32_t grain, uint32_t rate) {
    if (!v->channels || !v->wave_type) return;
    double step = (double)v->wave_rate * v->ratio / (double)rate;
    uint32_t chs = v->channels;
    if (!v->primed) {
        if (!pull_frame(c, v, v->cur)) {
            v->starved = true;
            goto end;
        }
        if (!pull_frame(c, v, v->next)) memcpy(v->next, v->cur, chs * sizeof(float));
        v->primed = true;
        v->phase = 0;
    }
    for (uint32_t i = 0; i < grain; i++) {
        for (uint32_t ch = 0; ch < chs; ch++)
            v->samples[ch * grain + i] = v->cur[ch] + (v->next[ch] - v->cur[ch]) * (float)v->phase;
        v->has_samples = true;
        v->phase += step;
        while (v->phase >= 1.0) {
            v->phase -= 1.0;
            memcpy(v->cur, v->next, chs * sizeof(float));
            if (!pull_frame(c, v, v->next)) {
                v->primed = false;
                v->starved = true;
                goto end;
            }
        }
    }
    v->starved = false;
end:
    /* a voice that ran out of data and takes no more blocks is done */
    if (v->starved && !v->block_count && !v->accepts_blocks) v->state = ST_EMPTY;
}

/* ---- Effects ---- */

/* The PS5 filter param (header size 0x38): u32 index, location, u64 channel
 * mask (the channels it skips), u32 type, float frequency, q, level and two more
 * coefficients. Types: 1 low-pass, 2 high-pass, 3 band-pass (peak gain q),
 * 4 band-pass (0 dB peak), 5 notch, 6 low shelf, 7 high shelf, 8 peaking (the
 * usual biquad forms, level the linear gain of 6-8), 9 one-pole low-pass,
 * 10 one-pole high-pass, 11 the five coefficients as given. Layout and types as
 * AnyPS5 measured the PS5 module (its PR 13787); the console evaluates them in
 * float blocks of four frames, this is a plain double direct form. Location 0
 * and 1 filter every port, 2 + k port k only: here the voice's output. */
static void setup_filter_ps5(ngs_voice *v, const uint32_t *u, uint32_t rate) {
    if (u[0] >= MAX_FILTERS) return;
    ngs_filter *f = &v->filters[u[0]];
    uint32_t type = u[4];
    if (type == 0 || type > 11 || !rate) {
        memset(f, 0, sizeof(*f));
        return;
    }
    float k[5];
    memcpy(k, &u[5], sizeof(k));
    double b0, b1, b2, a0 = 1.0, a1 = 0.0, a2 = 0.0;
    if (type == 11) {
        b0 = k[0], b1 = k[1], b2 = k[2], a1 = -k[3], a2 = -k[4];
    } else {
        double fc = isfinite(k[0]) ? k[0] : 0.0, q = isfinite(k[1]) ? k[1] : 0.707, level = k[2];
        fc = fc < 0.0001 * rate ? 0.0001 * rate : fc > 0.499 * rate ? 0.499 * rate : fc;
        q = q < 0.0001 ? 0.0001 : q > 32.0 ? 32.0 : q;
        if (!(level > 0.0)) level = 0.0001;
        double w = 2.0 * 3.14159265358979 * fc / rate, cw = cos(w), sw = sin(w), alpha = sw / (2.0 * q);
        double A = sqrt(isfinite(level) ? level : 1.0), sa = 2.0 * sqrt(A) * alpha, t = tan(w * 0.5);
        switch (type) {
        case 1: b0 = b2 = (1.0 - cw) * 0.5, b1 = 1.0 - cw, a0 = 1.0 + alpha, a1 = -2.0 * cw, a2 = 1.0 - alpha; break;
        case 2: b0 = b2 = (1.0 + cw) * 0.5, b1 = -(1.0 + cw), a0 = 1.0 + alpha, a1 = -2.0 * cw, a2 = 1.0 - alpha; break;
        case 3: b0 = sw * 0.5, b1 = 0.0, b2 = -sw * 0.5, a0 = 1.0 + alpha, a1 = -2.0 * cw, a2 = 1.0 - alpha; break;
        case 4: b0 = alpha, b1 = 0.0, b2 = -alpha, a0 = 1.0 + alpha, a1 = -2.0 * cw, a2 = 1.0 - alpha; break;
        case 5: b0 = b2 = 1.0, b1 = -2.0 * cw, a0 = 1.0 + alpha, a1 = -2.0 * cw, a2 = 1.0 - alpha; break;
        case 6:
            b0 = A * ((A + 1) - (A - 1) * cw + sa), b1 = 2 * A * ((A - 1) - (A + 1) * cw), b2 = A * ((A + 1) - (A - 1) * cw - sa);
            a0 = (A + 1) + (A - 1) * cw + sa, a1 = -2 * ((A - 1) + (A + 1) * cw), a2 = (A + 1) + (A - 1) * cw - sa;
            break;
        case 7:
            b0 = A * ((A + 1) + (A - 1) * cw + sa), b1 = -2 * A * ((A - 1) + (A + 1) * cw), b2 = A * ((A + 1) + (A - 1) * cw - sa);
            a0 = (A + 1) - (A - 1) * cw + sa, a1 = 2 * ((A - 1) - (A + 1) * cw), a2 = (A + 1) - (A - 1) * cw - sa;
            break;
        case 8: b0 = 1 + alpha * A, b1 = -2 * cw, b2 = 1 - alpha * A, a0 = 1 + alpha / A, a1 = -2 * cw, a2 = 1 - alpha / A; break;
        case 9: b0 = b1 = t / (1.0 + t), b2 = 0.0, a1 = -(1.0 - t) / (1.0 + t); break;
        default: b0 = 1.0 / (1.0 + t), b1 = -b0, b2 = 0.0, a1 = -(1.0 - t) / (1.0 + t); break;
        }
    }
    if (u[1] > 2) {
        static bool warned;
        if (!warned) LOG_W(HLE, "ngs2: filter of port %u applied to the voice's output", u[1] - 2);
        warned = true;
    }
    bool was = f->enabled;
    f->enabled = true;
    f->bypass = (uint64_t)u[2] | (uint64_t)u[3] << 32;
    f->b0 = b0 / a0, f->b1 = b1 / a0, f->b2 = b2 / a0, f->a1 = a1 / a0, f->a2 = a2 / a0;
    if (!was) memset(f->h, 0, sizeof(f->h));
}

/* The PS4 layout: u32 index, location, type, reserved, u64 channel mask (the
 * channels it skips), float frequency, q, level. Type 1 is the low-pass filter
 * (the one games are seen to use); others pass through. */
static void setup_filter(ngs_voice *v, const uint32_t *u, uint32_t rate) {
    if (u[0] >= MAX_FILTERS) return;
    ngs_filter *f = &v->filters[u[0]];
    memset(f, 0, sizeof(*f));
    float freq, q, level;
    memcpy(&freq, &u[6], 4);
    memcpy(&q, &u[7], 4);
    memcpy(&level, &u[8], 4);
    if (u[2] == 0) return;
    if (u[2] != 1 || !isfinite(freq) || freq < 0.0f || !isfinite(q) || q <= 0.0f || !isfinite(level) || level < 0.0f ||
        !rate) {
        static uint32_t warned;
        if (warned != u[2]) LOG_W(HLE, "ngs2: sampler filter type %u (location %u) not supported", u[2], u[1]);
        warned = u[2];
        return;
    }
    f->enabled = true;
    f->bypass = (uint64_t)u[4] | (uint64_t)u[5] << 32;
    if (freq == 0.0f || freq >= rate * 0.5f) {
        f->b0 = freq == 0.0f ? 0.0 : level;
        return;
    }
    double w = 2.0 * 3.14159265358979 * freq / rate, cw = cos(w), alpha = sin(w) / (2.0 * q), a0 = 1.0 / (1.0 + alpha);
    f->b0 = (1.0 - cw) * 0.5 * a0 * level;
    f->b1 = 2.0 * f->b0;
    f->b2 = f->b0;
    f->a1 = -2.0 * cw * a0;
    f->a2 = (1.0 - alpha) * a0;
}

static void apply_filters(ngs_voice *v, uint32_t grain) {
    for (uint32_t k = 0; k < MAX_FILTERS; k++) {
        ngs_filter *f = &v->filters[k];
        if (!f->enabled) continue;
        for (uint32_t ch = 0; ch < v->channels; ch++) {
            if (f->bypass >> ch & 1) continue;
            double *h = f->h[ch];
            float *s = v->samples + (size_t)ch * grain;
            for (uint32_t i = 0; i < grain; i++) {
                double x = s[i], y = f->b0 * x + f->b1 * h[0] + f->b2 * h[1] - f->a1 * h[2] - f->a2 * h[3];
                if (fabs(y) < 1e-20) y = 0.0;
                h[1] = h[0];
                h[0] = x;
                h[3] = h[2];
                h[2] = y;
                s[i] = (float)y;
            }
        }
    }
}

static float millibels(int32_t mb) { return mb <= -10000 ? 0.0f : powf(10.0f, (float)mb / 2000.0f); }

/* One-pole low-pass coefficient whose gain at w (radians per sample) is g */
static float lowpass_coef(float g, float w) {
    if (g >= 0.9999f) return 0.0f;
    if (g < 0.001f) g = 0.001f;
    float cw = cosf(w), a = (1.0f - g * cw - sqrtf(fmaxf(0.0f, 2.0f * g * (1.0f - cw) - g * g * (1.0f - cw * cw)))) /
                            (1.0f - g);
    return a < 0.0f ? 0.0f : a > 0.99f ? 0.99f : a;
}

/* SceNgs2ReverbI3DL2Param: float wet, dry; s32 room, room HF; u32 reflection
 * pattern; float decay time, decay HF ratio; s32 reflections; float
 * reflections delay; s32 reverb; float reverb delay, diffusion, density, HF
 * reference (levels in millibels, times in seconds) */
static void setup_reverb(ngs_voice *v, const uint32_t *u, uint32_t rate) {
    if (!rate) return;
    ngs_reverb *r = v->reverb;
    if (!r) {
        r = (ngs_reverb *)calloc(1, sizeof(*r));
        if (!r) return;
        static const uint32_t base[REV_LINES] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        bool ok = true;
        r->rate = rate;
        r->pre_len = (uint32_t)(REV_PRE_SECONDS * rate) + 1;
        r->pre = (float *)calloc(r->pre_len, sizeof(float));
        ok = r->pre != NULL;
        for (uint32_t k = 0; k < REV_LINES && ok; k++) {
            r->len[k] = base[k] * rate / 44100;
            r->line[k] = (float *)calloc(r->len[k], sizeof(float));
            ok = r->line[k] != NULL;
        }
        v->reverb = r;
        if (!ok) {
            voice_release(v);
            return;
        }
    }
    float f[14];
    memcpy(f, u, sizeof(f));
    int32_t room = (int32_t)u[2], room_hf = (int32_t)u[3], reflections = (int32_t)u[7], reverb = (int32_t)u[9];
    float decay = f[5] > 0.1f ? f[5] : 0.1f, hf_ratio = f[6] > 0.1f ? f[6] : 0.1f;
    float refl_delay = f[8] < 0.0f ? 0.0f : f[8] > 0.3f ? 0.3f : f[8];
    float rev_delay = f[10] < 0.0f ? 0.0f : f[10] > 0.1f ? 0.1f : f[10];
    float hf_ref = f[13] > 20.0f ? f[13] : 5000.0f;
    if (decay > 20.0f) decay = 20.0f;
    r->wet = isfinite(f[0]) ? f[0] : 0.0f;
    r->dry = isfinite(f[1]) ? f[1] : 0.0f;
    float g_room = millibels(room);
    r->early_gain = g_room * millibels(reflections);
    r->late_gain = g_room * millibels(reverb) * 0.35f; /* the delay network sums eight lines */
    float w = 2.0f * 3.14159265f * hf_ref / (float)rate;
    r->in_coef = lowpass_coef(millibels(room_hf > 0 ? 0 : room_hf), w);
    r->early_delay = (uint32_t)(refl_delay * rate);
    r->late_delay = (uint32_t)((refl_delay + rev_delay) * rate);
    for (uint32_t k = 0; k < REV_LINES; k++) {
        float secs = (float)r->len[k] / (float)rate;
        r->fb[k] = powf(10.0f, -3.0f * secs / decay);
        float fb_hf = powf(10.0f, -3.0f * secs / (decay * (hf_ratio < 1.0f ? hf_ratio : 1.0f)));
        r->damp[k] = lowpass_coef(fb_hf / r->fb[k], w);
    }
    r->audible = r->wet != 0.0f && (r->early_gain > 0.0f || r->late_gain > 0.0f);
    if (getenv("PS5C_LOG_NGS2"))
        LOG_I(HLE, "ngs2: reverb wet %.2f dry %.2f room %d/%d decay %.2f s (HF x%.2f) reflections %d (%.3f s) reverb %d (%.3f s)",
              (double)r->wet, (double)r->dry, room, room_hf, (double)decay, (double)hf_ratio, reflections,
              (double)refl_delay, reverb, (double)rev_delay);
}

/* The voice's summed input (planar, in v->samples) becomes dry * input + wet * reverb */
static void apply_reverb(ngs_voice *v, uint32_t grain, bool has_input) {
    ngs_reverb *r = v->reverb;
    uint32_t chs = v->channels;
    if (!r || !chs) return;
    if (!r->audible) { /* the dry path alone */
        for (uint32_t k = 0; has_input && r->dry != 1.0f && k < chs * grain; k++) v->samples[k] *= r->dry;
        return;
    }
    if (has_input) r->tail = r->rate * 8 / (grain ? grain : 1); /* up to 8 s of tail */
    else if (!r->tail) return;
    else r->tail--;
    float mono_scale = 1.0f / (float)chs;
    for (uint32_t i = 0; i < grain; i++) {
        float x = 0.0f;
        for (uint32_t ch = 0; ch < chs; ch++) x += v->samples[ch * grain + i];
        x *= mono_scale;
        r->in_state = x + r->in_coef * (r->in_state - x);
        r->pre[r->pre_pos] = r->in_state;
        uint32_t e = (r->pre_pos + r->pre_len - r->early_delay) % r->pre_len;
        uint32_t l = (r->pre_pos + r->pre_len - r->late_delay) % r->pre_len;
        float early = r->pre[e] * r->early_gain, late_in = r->pre[l] * r->late_gain;
        r->pre_pos = (r->pre_pos + 1) % r->pre_len;
        float o[REV_LINES];
        for (uint32_t k = 0; k < REV_LINES; k++) {
            float y = r->line[k][r->pos[k]];
            r->lp[k] = y + r->damp[k] * (r->lp[k] - y);
            o[k] = r->lp[k] * r->fb[k];
        }
        /* an 8x8 Hadamard mix (scaled to stay lossless) feeds the lines back */
        float a[REV_LINES];
        for (uint32_t k = 0; k < REV_LINES; k++) a[k] = o[k];
        for (uint32_t span = 1; span < REV_LINES; span *= 2)
            for (uint32_t k = 0; k < REV_LINES; k += span * 2)
                for (uint32_t j = k; j < k + span; j++) {
                    float p = a[j], q = a[j + span];
                    a[j] = p + q;
                    a[j + span] = p - q;
                }
        for (uint32_t k = 0; k < REV_LINES; k++) {
            float in = a[k] * 0.35355339f + late_in;
            r->line[k][r->pos[k]] = fabsf(in) < 1e-20f ? 0.0f : in;
            r->pos[k] = (r->pos[k] + 1) % r->len[k];
        }
        for (uint32_t ch = 0; ch < chs; ch++) {
            float wet = early + o[ch % REV_LINES] - 0.5f * o[(ch + 3) % REV_LINES];
            float *s = &v->samples[ch * grain + i];
            *s = r->dry * *s + r->wet * wet;
        }
    }
    v->has_samples = true;
}

/* ---- Mixing graph ---- */

typedef struct render_ctx {
    hle_call *c;
    ngs_system *sys;
    uint32_t grain;
    ngs_voice **voices; /* every voice of the system that may sound */
    uint32_t count;
} render_ctx;

/* The default mix of a port without matrix levels: mono and stereo convert into
 * each other at -3 dB, mono into more channels feeds the front center, other
 * channels map one to one */
static float default_level(uint32_t src_channels, uint32_t s, uint32_t dst_channels, uint32_t d) {
    if ((src_channels == 1 && dst_channels == 2) || (src_channels == 2 && dst_channels == 1)) return 0.70710678f;
    if (src_channels == 1 && dst_channels > 2) return d == 2 ? 1.0f : 0.0f;
    return s == d ? 1.0f : 0.0f;
}

static void render_voice(render_ctx *x, ngs_voice *v) {
    if (v->rendered || v->rendering) return;
    v->rendering = true;
    uint32_t grain = x->grain;
    v->has_samples = false;
    if (v->channels) memset(v->samples, 0, sizeof(float) * v->channels * grain);
    if (v->state == ST_PLAYING || !is_sampler(v->rack->id)) {
        if (is_sampler(v->rack->id)) {
            if (v->state == ST_PLAYING) render_sampler(x->c, v, grain, x->sys->rate);
            if (v->has_samples) apply_filters(v, grain);
        } else {
            /* submixers and mastering voices sum what is patched into them */
            guest_addr me = voice_handle(v);
            for (uint32_t k = 0; k < x->count && v->channels; k++) {
                ngs_voice *src = x->voices[k];
                for (uint32_t p = 0; p < src->rack->max_ports; p++) {
                    const ngs_port *port = &src->ports[p];
                    if (port->dest != me || port->volume == 0.0f) continue;
                    render_voice(x, src);
                    if (!src->has_samples) continue;
                    const float *m = port->matrix >= 0 && port->matrix < MAX_MATRICES ? src->matrices[port->matrix] : NULL;
                    uint32_t mn = m ? src->matrix_levels[port->matrix] : 0;
                    if (!mn) m = NULL; /* a matrix without levels mixes as the default one */
                    for (uint32_t d = 0; d < v->channels; d++) {
                        for (uint32_t s = 0; s < src->channels; s++) {
                            float level;
                            if (m) level = d * src->channels + s < mn ? m[d * src->channels + s] : 0.0f;
                            else level = default_level(src->channels, s, v->channels, d);
                            level *= port->volume;
                            if (level == 0.0f) continue;
                            const float *in = src->samples + (size_t)s * grain;
                            float *outp = v->samples + (size_t)d * grain;
                            for (uint32_t i = 0; i < grain; i++) outp[i] += in[i] * level;
                        }
                    }
                    v->has_samples = true;
                }
            }
            if (v->reverb) apply_reverb(v, grain, v->has_samples);
        }
    }
    v->rendering = false;
    v->rendered = true;
}

/* int sceNgs2SystemRender(SceNgs2Handle, const SceNgs2RenderBufferInfo *buffers, uint32_t count):
 * each {void *buffer, size_t size, u32 waveform type, u32 channels} (24 bytes) */
static void ngs_system_render(hle_call *c) {
    guest_addr list = hle_arg_ptr(c, 1);
    uint32_t count = hle_arg_u32(c, 2);
    if (!list || !count || count > 16) {
        hle_ret_s32(c, NGS2_ERR_INVALID_OUT_ADDRESS);
        return;
    }
    struct {
        uint64_t buffer, size;
        uint32_t type, channels;
    } out[16];
    for (uint32_t i = 0; i < count; i++) {
        gmem_read(c->mem, list + i * 24, &out[i], 24);
        if (out[i].buffer && out[i].size) {
            void *p = gmem_host(c->mem, out[i].buffer, out[i].size);
            if (p) memset(p, 0, out[i].size);
        }
    }
    pal_mutex_lock(g_ngs.lock);
    ngs_system *s = find_system(hle_arg(c, 0));
    if (!s) {
        pal_mutex_unlock(g_ngs.lock);
        hle_ret_s32(c, NGS2_ERR_INVALID_SYSTEM_HANDLE);
        return;
    }
    uint32_t grain = s->grain;
    /* the voices that may sound this grain */
    static ngs_voice **voices;
    static uint32_t cap;
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_RACKS; i++) {
        ngs_rack *r = &g_ngs.racks[i];
        if (!r->used || r->sys != s) continue;
        for (uint32_t k = 0; k < r->max_voices; k++) {
            ngs_voice *v = &r->voices[k];
            v->rendered = false;
            if (is_sampler(r->id) && v->state != ST_PLAYING) continue;
            if (!v->channels) continue;
            if (n == cap) {
                uint32_t grown = cap ? cap * 2 : 256;
                ngs_voice **p = (ngs_voice **)realloc(voices, grown * sizeof(*p));
                if (!p) break;
                voices = p;
                cap = grown;
            }
            voices[n++] = v;
        }
    }
    render_ctx x = {c, s, grain, voices, n};
    static float mix[16][MAX_GRAIN * MAX_CHANNELS];
    bool used[16] = {false};
    for (uint32_t k = 0; k < n; k++) {
        ngs_voice *v = voices[k];
        render_voice(&x, v);
        if (v->rack->id != RACK_MASTERING || !v->has_samples || v->output_id >= count) continue;
        uint32_t oc = out[v->output_id].channels ? out[v->output_id].channels : 2;
        if (oc > MAX_CHANNELS) oc = MAX_CHANNELS;
        float *m = mix[v->output_id];
        if (!used[v->output_id]) memset(m, 0, sizeof(float) * grain * oc);
        used[v->output_id] = true;
        for (uint32_t d = 0; d < oc; d++) {
            for (uint32_t src = 0; src < v->channels; src++) {
                float level = v->out_matrix_levels ? (src * oc + d < v->out_matrix_levels ? v->out_matrix[src * oc + d] : 0.0f)
                                                   : (src == d ? 1.0f : 0.0f);
                if (v->band_gains) level *= d == 3 ? v->lfe_gain : v->fbw_gain; /* channel 3 is the LFE */
                if (level == 0.0f) continue;
                for (uint32_t i = 0; i < grain; i++) m[i * oc + d] += v->samples[src * grain + i] * level;
            }
        }
    }
    /* stopped voices end with this grain (no release envelope) */
    for (uint32_t i = 0; i < MAX_RACKS; i++) {
        ngs_rack *r = &g_ngs.racks[i];
        for (uint32_t k = 0; r->used && r->sys == s && k < r->max_voices; k++) {
            ngs_voice *v = &r->voices[k];
            if (v->state == ST_STOPPED) {
                v->state = ST_EMPTY;
                clear_blocks(v);
            }
            v->state_flags = v->state;
        }
    }
    s->renders++;
    /* callbacks go to the guest after the lock is released */
    uint32_t ncb = g_ngs.callback_count;
    pending_callback cbs[MAX_CALLBACKS];
    memcpy(cbs, g_ngs.callbacks, sizeof(pending_callback) * ncb);
    g_ngs.callback_count = 0;
    guest_addr scratch = s->scratch;
    pal_mutex_unlock(g_ngs.lock);

    for (uint32_t i = 0; i < count; i++) {
        if (!used[i] || !out[i].buffer) continue;
        uint32_t oc = out[i].channels ? out[i].channels : 2;
        if (oc > MAX_CHANNELS) oc = MAX_CHANNELS;
        uint64_t samples = (uint64_t)grain * oc;
        /* PS5C_NGS2_DUMP=<file>: the first minute of output 0 as raw 16-bit PCM (debugging aid) */
        static FILE *dump;
        static uint64_t dumped;
        const char *path = getenv("PS5C_NGS2_DUMP");
        if (i == 0 && path && dumped < 48000ull * 60 * oc * 2) {
            if (!dump) dump = fopen(path, "wb");
            for (uint64_t k = 0; dump && k < samples; k++) {
                float f = mix[i][k];
                int16_t v16 = (int16_t)lrintf((f > 1.0f ? 1.0f : f < -1.0f ? -1.0f : f) * 32767.0f);
                fwrite(&v16, 2, 1, dump);
            }
            if (dump) fflush(dump);
            dumped += samples * 2;
        }
        if (out[i].type == WAVE_FLOAT) {
            if (samples * 4 <= out[i].size) gmem_write(c->mem, out[i].buffer, mix[i], samples * 4);
        } else {
            int16_t *pcm = (int16_t *)gmem_host(c->mem, out[i].buffer, samples * 2);
            if (!pcm || samples * 2 > out[i].size) continue;
            for (uint64_t k = 0; k < samples; k++) {
                float f = mix[i][k];
                f = f > 1.0f ? 1.0f : f < -1.0f ? -1.0f : f;
                pcm[k] = (int16_t)lrintf(f * 32767.0f);
            }
        }
    }
    for (uint32_t i = 0; i < ncb && scratch; i++) {
        guest_addr info = scratch + (guest_addr)i * CALLBACK_INFO_SIZE;
        gmem_write(c->mem, info, cbs[i].info, CALLBACK_INFO_SIZE);
        uint64_t rax;
        runtime_call_nested(c->rt, cbs[i].fn, info, 0, 0, &rax);
    }
    hle_ret_s32(c, 0);
}

/* ---- Voice control ---- */

static void set_event(ngs_voice *v, uint32_t id) {
    switch (id) {
    case 1:
        if (v->state == ST_EMPTY || v->state == ST_STOPPED) {
            v->state = ST_PLAYING;
            v->state_flags |= 1;
        }
        break;
    case 2:
        if (v->state == ST_PLAYING || v->state == ST_PAUSED) v->state = ST_STOPPED;
        break;
    case 4:
    case 8:
        v->state = ST_EMPTY;
        clear_blocks(v);
        break;
    case 16:
        if (v->state == ST_PLAYING) v->state = ST_PAUSED;
        break;
    case 32:
        if (v->state == ST_PAUSED) v->state = ST_PLAYING;
        break;
    default: break;
    }
}

static void set_matrix(hle_call *c, ngs_voice *v, uint32_t index, guest_addr levels, uint32_t count) {
    if (index >= MAX_MATRICES || count > 256) return;
    float *m = (float *)realloc(v->matrices[index], (count ? count : 1) * sizeof(float));
    if (!m) return;
    v->matrices[index] = m;
    v->matrix_levels[index] = gmem_read(c->mem, levels, m, (uint64_t)count * 4) ? count : 0;
}

static void setup_sampler(ngs_voice *v, const uint32_t *fmt) {
    voice_release(v);
    clear_blocks(v);
    v->state = ST_EMPTY;
    v->wave_type = fmt[0];
    v->channels = fmt[1] > MAX_CHANNELS ? MAX_CHANNELS : fmt[1];
    v->wave_rate = fmt[2];
    v->at9_config = fmt[3];
    v->ratio = 1.0;
    v->accepts_blocks = true;
    v->decoded_samples = v->decoded_bytes = 0;
    for (uint32_t p = 0; p < MAX_PORTS; p++) v->ports[p] = (ngs_port){0, 1.0f, -1};
    if (v->wave_type == WAVE_ATRAC9) {
#ifdef PS5C_HAVE_FFMPEG
        if (!at9_setup(v)) {
            LOG_W(HLE, "ngs2: cannot decode the ATRAC9 waveform (config 0x%08x)", v->at9_config);
            v->wave_type = 0;
        }
#else
        v->wave_type = 0;
#endif
    } else if (v->wave_type && v->wave_type != WAVE_PCM16 && v->wave_type != WAVE_FLOAT) {
        static uint32_t warned;
        if (warned != v->wave_type) LOG_W(HLE, "ngs2: waveform type 0x%x not supported", v->wave_type);
        warned = v->wave_type;
        v->wave_type = 0;
    }
}

/* Sampler blocks: {u8 *data +8, u32 flags +16, u32 count +20, SceNgs2WaveformBlock *blocks +24};
 * a block (40 bytes): data offset, data size, u32 repeats, skip, samples, reserved, user data.
 * Flags: 1 more blocks will follow, 2 data only (ATRAC9 streaming), 4 replace the queued ones. */
static void add_blocks(hle_call *c, ngs_voice *v, guest_addr param) {
    uint64_t data = 0, list = 0;
    uint32_t fc[2] = {0};
    gmem_read_u64(c->mem, param + 8, &data);
    gmem_read(c->mem, param + 16, fc, 8);
    gmem_read_u64(c->mem, param + 24, &list);
    if (fc[0] & 4) clear_blocks(v);
    v->accepts_blocks = (fc[0] & 1) != 0;
    for (uint32_t i = 0; i < fc[1] && i < 64; i++) {
        uint8_t raw[40];
        if (!gmem_read(c->mem, list + i * 40, raw, sizeof(raw))) break;
        ngs_block b;
        memset(&b, 0, sizeof(b));
        uint64_t off;
        memcpy(&off, raw, 8);
        memcpy(&b.size, raw + 8, 8);
        memcpy(&b.repeats, raw + 16, 4);
        memcpy(&b.skip, raw + 20, 4);
        memcpy(&b.samples, raw + 24, 4);
        memcpy(&b.user, raw + 32, 8);
        b.data = data + off;
        static int log_blocks = -1;
        if (log_blocks < 0) log_blocks = getenv("PS5C_LOG_NGS2") != NULL;
        if (log_blocks)
            LOG_I(HLE, "ngs2: voice %u block: data 0x%llx size %llu repeats %u skip %u samples %u flags 0x%x", v->index,
                  (unsigned long long)b.data, (unsigned long long)b.size, b.repeats, b.skip, b.samples, fc[0]);
        if (fc[0] & 2) b.repeats = b.skip = b.samples = 0;
        if (!(fc[0] & 2) && !b.samples && !b.size) continue;
        if (v->wave_type != WAVE_ATRAC9 && !b.samples) continue;
        if (v->block_count == MAX_BLOCKS) break;
        v->blocks[v->block_count++] = b;
    }
}

/* int sceNgs2VoiceControl(SceNgs2Handle voice, const SceNgs2VoiceParamHead *list) */
static void ngs_voice_control(hle_call *c) {
    guest_addr p = hle_arg_ptr(c, 1);
    pal_mutex_lock(g_ngs.lock);
    ngs_voice *v = find_voice(hle_arg(c, 0));
    for (int guard = 0; v && p && guard < 256; guard++) {
        uint8_t h[8];
        if (!gmem_read(c->mem, p, h, sizeof(h))) break;
        uint16_t size;
        int16_t next;
        uint32_t id, u[24];
        memcpy(&size, h, 2);
        memcpy(&next, h + 2, 2);
        memcpy(&id, h + 4, 4);
        memset(u, 0, sizeof(u));
        if (size > 8) gmem_read(c->mem, p + 8, u, (uint64_t)size - 8 < sizeof(u) ? (uint64_t)size - 8 : sizeof(u));
        uint32_t rack = id >> 16, cid = id & 0xFFFF;
        static int log_params = -1;
        if (log_params < 0) log_params = getenv("PS5C_LOG_NGS2") != NULL;
        if (log_params)
            LOG_I(HLE, "ngs2: voice %u of rack 0x%x: param 0x%08x size %u: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
                  v->index, v->rack->id, id, size, u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9]);
        if (rack == 0) {
            uint64_t ptr = (uint64_t)u[2] | (uint64_t)u[3] << 32;
            switch (cid & 0x7FFF) {
            case 1: set_matrix(c, v, u[0], ptr, u[1]); break;                      /* matrix id, count, levels */
            case 2: if (u[0] < MAX_PORTS) memcpy(&v->ports[u[0]].volume, &u[1], 4); break; /* port, level */
            case 3: if (u[0] < MAX_PORTS) v->ports[u[0]].matrix = (int32_t)u[1]; break;   /* port, matrix */
            case 5: /* port, input, destination voice (a rack stands for its first voice: Dead Cells) */
                if (u[0] < MAX_PORTS) {
                    ngs_rack *dr = find_rack(ptr);
                    v->ports[u[0]].dest = dr && dr->max_voices ? voice_handle(&dr->voices[0]) : ptr;
                }
                break;
            case 6: set_event(v, u[0]); break;
            case 7: /* callback, its data, flags */
                v->callback = (uint64_t)u[0] | (uint64_t)u[1] << 32;
                v->callback_data = (uint64_t)u[2] | (uint64_t)u[3] << 32;
                v->callback_flags = u[4];
                break;
            default: break;
            }
        } else if (rack == RACK_SAMPLER || rack == RACK_CUSTOM_SAMPLER) {
            if (cid == 0) setup_sampler(v, u);
            else if (cid == 1) add_blocks(c, v, p);
            else if (cid == 4) {
                for (uint32_t i = 0; i < v->block_count; i++)
                    if (v->blocks[i].repeats) {
                        v->blocks[i].repeats = 0;
                        break;
                    }
            } else if (cid == 0xa && size >= 0x38) setup_filter_ps5(v, u, v->rack->sys->rate);
            else if (cid == 0xa) setup_filter(v, u, v->rack->sys->rate);
            else if (cid == 5) {
                float r;
                memcpy(&r, &u[0], 4);
                v->ratio = isfinite(r) ? (r < 0.0f ? 0.0 : r > 4.0f ? 4.0 : r) : 1.0;
            }
        } else if (id == 0x40001300) {
            /* the custom filter module: u64 channel mask, the type and five values
             * as in the sampler's PS5 filter; one filter over the voice's output */
            uint32_t f[10] = {0, 1, u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7]};
            setup_filter_ps5(v, f, v->rack->sys->rate);
        } else if (rack == RACK_SUBMIXER || rack == RACK_CUSTOM_SUBMIXER) {
            if (cid == 0) v->channels = u[0] > MAX_CHANNELS ? MAX_CHANNELS : u[0];
        } else if (rack == RACK_REVERB) { /* setup {input channels, output channels, flags}; I3DL2 */
            uint32_t n = u[1] ? u[1] : u[0];
            if (cid == 0) v->channels = n > MAX_CHANNELS ? MAX_CHANNELS : n;
            else if (cid == 1) setup_reverb(v, u, v->rack->sys->rate);
        } else if (rack == RACK_MASTERING || rack == RACK_CUSTOM_MASTERING) {
            if (cid == 0) v->channels = u[0] > MAX_CHANNELS ? MAX_CHANNELS : u[0];
            else if (cid == 1) { /* the output matrix: matrix id, count, levels */
                uint64_t ptr = (uint64_t)u[2] | (uint64_t)u[3] << 32;
                uint32_t n = u[1] > MAX_CHANNELS * MAX_CHANNELS ? MAX_CHANNELS * MAX_CHANNELS : u[1];
                v->out_matrix_levels = gmem_read(c->mem, ptr, v->out_matrix, (uint64_t)n * 4) ? n : 0;
            } else if (cid == 4) { /* full-band and LFE gains */
                float g[2];
                memcpy(g, u, sizeof(g));
                for (uint32_t i = 0; i < 2; i++) g[i] = isfinite(g[i]) ? (g[i] < 0.0f ? 0.0f : g[i] > 4.0f ? 4.0f : g[i]) : 1.0f;
                v->fbw_gain = g[0];
                v->lfe_gain = g[1];
                v->band_gains = true;
            } else if (cid == 5) v->output_id = u[0];
        }
        if (!next) break;
        p += (int64_t)next;
    }
    pal_mutex_unlock(g_ngs.lock);
    hle_ret_s32(c, v ? 0 : NGS2_ERR_INVALID_VOICE_HANDLE);
}

/* int sceNgs2VoiceRunCommands(SceNgs2Handle voice, const command *, size_t count):
 * {u32 id (index in bits 24-31), u8 flags, u8 type, u16 count, 8-byte value} */
static void ngs_voice_run_commands(hle_call *c) {
    guest_addr list = hle_arg_ptr(c, 1);
    uint64_t n = hle_arg(c, 2);
    pal_mutex_lock(g_ngs.lock);
    ngs_voice *v = find_voice(hle_arg(c, 0));
    for (uint64_t i = 0; v && i < n && i < 1024; i++) {
        uint8_t cmd[16];
        if (!gmem_read(c->mem, list + i * 16, cmd, 16)) break;
        uint32_t id, u;
        uint16_t count;
        uint64_t ptr;
        memcpy(&id, cmd, 4);
        memcpy(&count, cmd + 6, 2);
        memcpy(&u, cmd + 8, 4);
        memcpy(&ptr, cmd + 8, 8);
        uint32_t index = id >> 24;
        static int log_cmds = -1;
        if (log_cmds < 0) log_cmds = getenv("PS5C_LOG_NGS2") != NULL;
        if (log_cmds)
            LOG_I(HLE, "ngs2: voice %u of rack 0x%x: command 0x%08x type %u count %u value 0x%llx", v->index, v->rack->id,
                  id, cmd[5], count, (unsigned long long)ptr);
        switch (id & 0xFFFFFF) {
        case 2: set_event(v, u); break;
        case 5: set_matrix(c, v, index, ptr, count); break;
        case 6: if (index < MAX_PORTS) memcpy(&v->ports[index].volume, &u, 4); break;
        case 7: if (index < MAX_PORTS) v->ports[index].matrix = (int32_t)u; break;
        default: break;
        }
    }
    pal_mutex_unlock(g_ngs.lock);
    hle_ret_s32(c, v ? 0 : NGS2_ERR_INVALID_VOICE_HANDLE);
}

/* int sceNgs2VoiceGetState(SceNgs2Handle voice, SceNgs2VoiceState *out, size_t size):
 * {u32 state flags, s32 error}, then the rack type's fields */
static void ngs_voice_get_state(hle_call *c) {
    guest_addr out = hle_arg_ptr(c, 1);
    uint64_t size = hle_arg(c, 2);
    uint8_t buf[256];
    if (!out || size < 8) {
        hle_ret_s32(c, NGS2_ERR_INVALID_OUT_ADDRESS);
        return;
    }
    if (size > sizeof(buf)) size = sizeof(buf);
    memset(buf, 0, sizeof(buf));
    pal_mutex_lock(g_ngs.lock);
    ngs_voice *v = find_voice(hle_arg(c, 0));
    if (v) {
        memcpy(buf, &v->state_flags, 4);
        guest_addr wave = v->block_count ? v->blocks[0].data : v->last_data;
        uint64_t user = v->block_count ? v->blocks[0].user : 0;
        if (v->rack->id == RACK_SAMPLER) { /* envelope, peak, reserved, decoded samples, bytes, user data, waveform */
            float one = 1.0f;
            memcpy(buf + 8, &one, 4);
            memcpy(buf + 24, &v->decoded_samples, 8);
            memcpy(buf + 32, &v->decoded_bytes, 8);
            memcpy(buf + 40, &user, 8);
            memcpy(buf + 48, &wave, 8);
        } else if (v->rack->id == RACK_CUSTOM_SAMPLER) { /* waveform, decoded samples, bytes, user data */
            memcpy(buf + 8, &wave, 8);
            memcpy(buf + 16, &v->decoded_samples, 8);
            memcpy(buf + 24, &v->decoded_bytes, 8);
            memcpy(buf + 32, &user, 8);
        }
    }
    pal_mutex_unlock(g_ngs.lock);
    if (v) gmem_write(c->mem, out, buf, size);
    hle_ret_s32(c, v ? 0 : NGS2_ERR_INVALID_VOICE_HANDLE);
}

static void ngs_voice_get_state_flags(hle_call *c) {
    pal_mutex_lock(g_ngs.lock);
    ngs_voice *v = find_voice(hle_arg(c, 0));
    uint32_t flags = v ? v->state_flags : 0;
    pal_mutex_unlock(g_ngs.lock);
    if (v) gmem_write(c->mem, hle_arg_ptr(c, 1), &flags, 4);
    hle_ret_s32(c, v ? 0 : NGS2_ERR_INVALID_VOICE_HANDLE);
}

/* ---- Waveform helpers ---- */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* int sceNgs2ParseWaveformData(const void *data, size_t size, SceNgs2WaveformInfo *out):
 * a RIFF/WAVE file (ATRAC9 or 16-bit PCM). Info (232 bytes): format {type,
 * channels, rate, config, frame offset, frame margin}, data offset, data size,
 * loop begin, loop end, samples, unit size, unit samples, units per frame,
 * frame size, frame samples, delay samples, block count, blocks[4] (40 each) */
static void ngs_parse_waveform_data(hle_call *c) {
    guest_addr at = hle_arg_ptr(c, 0), out = hle_arg_ptr(c, 2);
    uint64_t size = hle_arg(c, 1);
    if (!out) {
        hle_ret_s32(c, NGS2_ERR_INVALID_OUT_ADDRESS);
        return;
    }
    const uint8_t *p = size >= 12 ? (const uint8_t *)gmem_host(c->mem, at, size) : NULL;
    uint8_t info[232];
    memset(info, 0, sizeof(info));
    if (!p || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) {
        gmem_write(c->mem, out, info, sizeof(info));
        hle_ret_s32(c, p ? NGS2_ERR_UNKNOWN_WAVEFORM_FORMAT : NGS2_ERR_INVALID_WAVEFORM_DATA);
        return;
    }
    const uint8_t *fmt = NULL, *fact = NULL, *smpl = NULL;
    uint32_t fmt_size = 0, data_off = 0, data_size = 0;
    for (uint64_t off = 12; off + 8 <= size;) {
        uint32_t len = rd32(p + off + 4);
        uint64_t payload = off + 8;
        if (!memcmp(p + off, "fmt ", 4)) {
            fmt = p + payload;
            fmt_size = len;
        } else if (!memcmp(p + off, "fact", 4)) fact = p + payload;
        else if (!memcmp(p + off, "smpl", 4)) smpl = p + payload;
        else if (!memcmp(p + off, "data", 4)) {
            data_off = (uint32_t)payload;
            data_size = len;
            break; /* the data may extend past what was given */
        }
        off = payload + len + (len & 1);
    }
    static const uint8_t at9_guid[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
                                         0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};
    if (!fmt || !data_off || fmt_size < 16 || (fmt + 16 > p + size)) {
        gmem_write(c->mem, out, info, sizeof(info));
        hle_ret_s32(c, NGS2_ERR_INVALID_WAVEFORM_FORMAT);
        return;
    }
    uint32_t tag = rd16(fmt), channels = rd16(fmt + 2), rate = rd32(fmt + 4), block_align = rd16(fmt + 12);
    uint32_t v[12];
    memset(v, 0, sizeof(v));
    uint32_t type, config = 0, samples, unit_size, unit_samples, units = 1, delay = 0, skip = 0;
    if (tag == 0xFFFE && fmt_size >= 52 && !memcmp(fmt + 24, at9_guid, 16)) {
        type = WAVE_ATRAC9;
        config = (uint32_t)fmt[44] << 24 | (uint32_t)fmt[45] << 16 | (uint32_t)fmt[46] << 8 | fmt[47];
        uint32_t bits = config & 0xFFFFFF;
        static const uint8_t log2s[16] = {6, 6, 7, 7, 7, 8, 8, 8, 6, 6, 7, 7, 7, 8, 8, 8};
        units = 1u << ((bits >> 3) & 3);
        unit_size = ((bits >> 5) & 0x7FF) + 1;
        unit_samples = 1u << log2s[bits >> 20];
        samples = fact ? rd32(fact) : 0;
        delay = fact ? rd32(fact + 4) : 0;
        skip = fact ? rd32(fact + 8) : 0;
    } else if ((tag == 1 || tag == 0xFFFE) && rd16(fmt + 14) == 16) {
        type = WAVE_PCM16;
        unit_size = block_align;
        unit_samples = 1;
        samples = block_align ? data_size / block_align : 0;
    } else {
        gmem_write(c->mem, out, info, sizeof(info));
        hle_ret_s32(c, NGS2_ERR_UNKNOWN_WAVEFORM_FORMAT);
        return;
    }
    uint32_t format[6] = {type, channels, rate, config, 0, 0};
    memcpy(info, format, sizeof(format));
    uint32_t loop_begin = 0, loop_end = 0;
    if (smpl && rd32(smpl + 28) >= 1) { /* the first loop of a sampler chunk */
        loop_begin = rd32(smpl + 36 + 8);
        loop_end = rd32(smpl + 36 + 12);
    }
    v[0] = data_off;
    v[1] = data_size;
    v[2] = loop_begin;
    v[3] = loop_end;
    v[4] = samples;
    v[5] = unit_size;
    v[6] = unit_samples;
    v[7] = units;
    v[8] = unit_size * units;
    v[9] = unit_samples * units;
    v[10] = delay;
    v[11] = 1;
    memcpy(info + 24, v, sizeof(v));
    uint64_t block_off = data_off, block_size = data_size;
    uint32_t b[4] = {0, skip, samples, 0};
    memcpy(info + 72, &block_off, 8);
    memcpy(info + 80, &block_size, 8);
    memcpy(info + 88, b, sizeof(b));
    gmem_write(c->mem, out, info, sizeof(info));
    hle_ret_s32(c, 0);
}

/* int sceNgs2CalcWaveformBlock(const format *, uint32_t sample_pos, uint32_t samples, block *out) */
static void ngs_calc_waveform_block(hle_call *c) {
    uint32_t fmt[6];
    guest_addr out = hle_arg_ptr(c, 3);
    uint32_t pos = hle_arg_u32(c, 1), n = hle_arg_u32(c, 2);
    uint8_t b[40];
    memset(b, 0, sizeof(b));
    if (!out || !gmem_read(c->mem, hle_arg_ptr(c, 0), fmt, sizeof(fmt)) || !fmt[1] || fmt[1] > 8) {
        hle_ret_s32(c, NGS2_ERR_INVALID_WAVEFORM_FORMAT);
        return;
    }
    uint64_t off, size;
    uint32_t skip = 0;
    if (fmt[0] == WAVE_ATRAC9) {
        uint32_t bits = fmt[3] & 0xFFFFFF;
        static const uint8_t log2s[16] = {6, 6, 7, 7, 7, 8, 8, 8, 6, 6, 7, 7, 7, 8, 8, 8};
        uint32_t frames = 1u << ((bits >> 3) & 3);
        uint64_t sf_samples = (uint64_t)(1u << log2s[bits >> 20]) * frames, sf_size = (((bits >> 5) & 0x7FF) + 1) * frames;
        skip = (uint32_t)(pos % sf_samples);
        off = pos / sf_samples * sf_size;
        size = n ? (skip + n + sf_samples - 1) / sf_samples * sf_size : 0;
    } else if (fmt[0] == WAVE_PCM16 || fmt[0] == WAVE_FLOAT) {
        uint32_t bytes = fmt[0] == WAVE_FLOAT ? 4 : 2;
        off = (uint64_t)pos * fmt[1] * bytes;
        size = (uint64_t)n * fmt[1] * bytes;
    } else {
        hle_ret_s32(c, NGS2_ERR_UNKNOWN_WAVEFORM_FORMAT);
        return;
    }
    memcpy(b, &off, 8);
    memcpy(b + 8, &size, 8);
    memcpy(b + 20, &skip, 4);
    memcpy(b + 24, &n, 4);
    gmem_write(c->mem, out, b, sizeof(b));
    hle_ret_s32(c, 0);
}

/* ---- Panning and geometry (a centered, level-preserving approximation) ---- */

/* int sceNgs2PanInit(SceNgs2PanWork *work, const float *angles, float unit_angle, uint32_t speakers):
 * work {float angles[8], float unit angle, u32 speakers} (unit_angle arrives in xmm0) */
static void ngs_pan_init(hle_call *c) {
    guest_addr work = hle_arg_ptr(c, 0), angles = hle_arg_ptr(c, 1);
    uint32_t speakers = hle_arg_u32(c, 2);
    float buf[10];
    memset(buf, 0, sizeof(buf));
    if (speakers > 8) speakers = 8;
    if (angles) gmem_read(c->mem, angles, buf, speakers * 4);
    memcpy(&buf[8], &c->a->xmm[0][0], 4);
    memcpy(&buf[9], &speakers, 4);
    hle_ret_s32(c, work && gmem_write(c->mem, work, buf, sizeof(buf)) ? 0 : NGS2_ERR_INVALID_OUT_ADDRESS);
}

/* int sceNgs2PanGetVolumeMatrix(work, const SceNgs2PanParam *params, uint32_t count,
 * uint32_t matrix_format, float *out): per source channel, a level for each
 * speaker; stereo gets a constant-power pan from the angle */
static void ngs_pan_get_volume_matrix(hle_call *c) {
    guest_addr work = hle_arg_ptr(c, 0), params = hle_arg_ptr(c, 1), out = hle_arg_ptr(c, 4);
    uint32_t n = hle_arg_u32(c, 2), format = hle_arg_u32(c, 3);
    uint32_t speakers = format == 0 ? 2 : format > 8 ? 8 : format;
    float wk[10] = {0};
    gmem_read(c->mem, work, wk, sizeof(wk));
    float unit = wk[8] > 0.0f ? wk[8] : 1.0f;
    for (uint32_t i = 0; out && i < n && i < 64; i++) {
        float p[4] = {0, 0, 1, 0}; /* angle, distance, full-band level, LFE level */
        gmem_read(c->mem, params + i * 16, p, sizeof(p));
        float level[8] = {0};
        float a = p[0] / unit * 3.14159265f / 180.0f; /* the angle in degrees, in units of unit_angle */
        float pan = sinf(a);
        pan = pan < -1.0f ? -1.0f : pan > 1.0f ? 1.0f : pan;
        float l = cosf((pan + 1.0f) * 0.78539816f), r = sinf((pan + 1.0f) * 0.78539816f);
        if (speakers == 1) level[0] = p[2];
        else {
            level[0] = l * p[2];
            level[1] = r * p[2];
        }
        gmem_write(c->mem, out + (guest_addr)i * speakers * 4, level, speakers * 4);
    }
    hle_ret_s32(c, 0);
}

/* SceNgs2GeomListenerParam: position, front, up, velocity, sound speed (as the console resets it) */
static void ngs_geom_reset_listener_param(hle_call *c) {
    float p[15];
    memset(p, 0, sizeof(p));
    p[5] = 1.0f;  /* front z */
    p[7] = 1.0f;  /* up y */
    p[12] = 343.0f;
    guest_addr out = hle_arg_ptr(c, 0);
    hle_ret_s32(c, out && gmem_write(c->mem, out, p, sizeof(p)) ? 0 : NGS2_ERR_INVALID_OUT_ADDRESS);
}

/* SceNgs2GeomSourceParam (112 bytes) with the console's defaults */
static void ngs_geom_reset_source_param(hle_call *c) {
    float p[28];
    memset(p, 0, sizeof(p));
    p[8] = 1.0f;                                    /* direction z */
    p[9] = 1.0f, p[10] = 360.0f, p[11] = 1.0f, p[12] = 360.0f; /* cone */
    p[14] = 1000000.0f, p[15] = 1.0f, p[16] = 1.0f; /* rolloff: max distance, factor, reference */
    p[17] = 1.0f, p[18] = 1.0f, p[19] = 1.0f, p[20] = 1.0f; /* doppler, full-band, LFE, max level */
    uint32_t two = 2;
    memcpy(&p[23], &two, 4);
    memcpy(&p[24], &two, 4);
    guest_addr out = hle_arg_ptr(c, 0);
    hle_ret_s32(c, out && gmem_write(c->mem, out, p, sizeof(p)) ? 0 : NGS2_ERR_INVALID_OUT_ADDRESS);
}

/* int sceNgs2GeomCalcListener(const listener param *, SceNgs2GeomListenerWork *out, uint32_t flags) */
static void ngs_geom_calc_listener(hle_call *c) {
    float w[24];
    memset(w, 0, sizeof(w));
    for (int i = 0; i < 4; i++) w[i * 5] = 1.0f;
    float lp[13] = {0};
    gmem_read(c->mem, hle_arg_ptr(c, 0), lp, sizeof(lp));
    memcpy(&w[16], &lp[9], 12);
    w[19] = lp[12] > 0.0f ? lp[12] : 343.0f;
    guest_addr out = hle_arg_ptr(c, 1);
    hle_ret_s32(c, out && gmem_write(c->mem, out, w, sizeof(w)) ? 0 : NGS2_ERR_INVALID_OUT_ADDRESS);
}

/* int sceNgs2GeomApply(listener work, source param, SceNgs2GeomAttribute *out, flags):
 * pitch 1 and the source's level on the diagonal of the 8x8 matrix */
static void ngs_geom_apply(hle_call *c) {
    float src[28] = {0};
    gmem_read(c->mem, hle_arg_ptr(c, 1), src, sizeof(src));
    float a[1 + 64 + 8 + 4];
    memset(a, 0, sizeof(a));
    a[0] = 1.0f;
    uint32_t fmt;
    memcpy(&fmt, &src[24], 4);
    uint32_t chs = fmt == 0 ? 2 : fmt > 8 ? 8 : fmt;
    float level = src[20] > 0.0f ? src[20] : 1.0f;
    for (uint32_t ch = 0; ch < chs; ch++) a[1 + ch * 8 + ch] = level;
    guest_addr out = hle_arg_ptr(c, 2);
    hle_ret_s32(c, out && gmem_write(c->mem, out, a, sizeof(a)) ? 0 : NGS2_ERR_INVALID_OUT_ADDRESS);
}

#define N "libSceNgs2"

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(N, "sceNgs2SystemResetOption", ngs_system_reset_option),
    HLE_FUNC_ENTRY(N, "sceNgs2SystemQueryBufferSize", ngs_system_query_buffer_size),
    HLE_FUNC_ENTRY(N, "sceNgs2SystemCreate", ngs_system_create),
    HLE_FUNC_ENTRY(N, "sceNgs2SystemCreateWithAllocator", ngs_system_create_with_allocator),
    HLE_FUNC_ENTRY(N, "sceNgs2SystemDestroy", ngs_system_destroy),
    HLE_FUNC_ENTRY(N, "sceNgs2SystemSetGrainSamples", ngs_system_set_grain_samples),
    HLE_FUNC_ENTRY(N, "sceNgs2SystemGetInfo", ngs_system_get_info),
    HLE_FUNC_ENTRY(N, "sceNgs2SystemRender", ngs_system_render),
    HLE_FUNC_ENTRY(N, "sceNgs2SystemLock", ngs_ok),
    HLE_FUNC_ENTRY(N, "sceNgs2SystemUnlock", ngs_ok),
    HLE_FUNC_ENTRY(N, "sceNgs2RackResetOption", ngs_rack_reset_option),
    HLE_FUNC_ENTRY(N, "sceNgs2RackQueryBufferSize", ngs_rack_query_buffer_size),
    HLE_FUNC_ENTRY(N, "sceNgs2RackCreate", ngs_rack_create),
    HLE_FUNC_ENTRY(N, "sceNgs2RackCreateWithAllocator", ngs_rack_create_with_allocator),
    HLE_FUNC_ENTRY(N, "sceNgs2RackDestroy", ngs_rack_destroy),
    HLE_FUNC_ENTRY(N, "sceNgs2RackGetVoiceHandle", ngs_rack_get_voice_handle),
    HLE_FUNC_ENTRY(N, "sceNgs2RackLock", ngs_ok),
    HLE_FUNC_ENTRY(N, "sceNgs2RackUnlock", ngs_ok),
    HLE_FUNC_ENTRY(N, "sceNgs2VoiceControl", ngs_voice_control),
    HLE_FUNC_ENTRY(N, "sceNgs2VoiceRunCommands", ngs_voice_run_commands),
    HLE_FUNC_ENTRY(N, "sceNgs2VoiceGetState", ngs_voice_get_state),
    HLE_FUNC_ENTRY(N, "sceNgs2VoiceGetStateFlags", ngs_voice_get_state_flags),
    HLE_FUNC_ENTRY(N, "sceNgs2ParseWaveformData", ngs_parse_waveform_data),
    HLE_FUNC_ENTRY(N, "sceNgs2CalcWaveformBlock", ngs_calc_waveform_block),
    HLE_FUNC_ENTRY(N, "sceNgs2PanInit", ngs_pan_init),
    HLE_FUNC_ENTRY(N, "sceNgs2PanGetVolumeMatrix", ngs_pan_get_volume_matrix),
    HLE_FUNC_ENTRY(N, "sceNgs2GeomResetListenerParam", ngs_geom_reset_listener_param),
    HLE_FUNC_ENTRY(N, "sceNgs2GeomResetSourceParam", ngs_geom_reset_source_param),
    HLE_FUNC_ENTRY(N, "sceNgs2GeomCalcListener", ngs_geom_calc_listener),
    HLE_FUNC_ENTRY(N, "sceNgs2GeomApply", ngs_geom_apply),
};

void hle_register_ngs2(void) {
    if (!g_ngs.lock && pal_mutex_create(&g_ngs.lock) != PS5C_OK) {
        LOG_E(HLE, "cannot create the NGS2 lock");
        return;
    }
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}
