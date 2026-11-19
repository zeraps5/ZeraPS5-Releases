/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceVideodec2: the game demuxes its movies itself (Unreal's Bink/Electra
 * players, CRI Sofdec, ...) and hands access units here; pictures go to the
 * game's frame buffers as NV12 with 256-aligned rows (pal_vdec). The compute
 * queue and decoder memory the game allocates are not used: decoding runs on
 * the host, synchronously in Decode. Structure layouts as KytyPS5's
 * libVideoDec2.cpp (GPL-2.0) documents them. */
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/hle.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

#define VDEC2_ERR_API_FAIL ((int32_t)0x811D0100)
#define VDEC2_ERR_STRUCT_SIZE ((int32_t)0x811D0101)
#define VDEC2_ERR_ARGUMENT_POINTER ((int32_t)0x811D0102)
#define VDEC2_ERR_DECODER_INSTANCE ((int32_t)0x811D0103)
#define VDEC2_ERR_MEMORY_SIZE ((int32_t)0x811D0104)
#define VDEC2_ERR_MEMORY_POINTER ((int32_t)0x811D0105)
#define VDEC2_ERR_FRAME_BUFFER_SIZE ((int32_t)0x811D0106)
#define VDEC2_ERR_FRAME_BUFFER_POINTER ((int32_t)0x811D0107)
#define VDEC2_ERR_ACCESS_UNIT_SIZE ((int32_t)0x811D010D)
#define VDEC2_ERR_ACCESS_UNIT_POINTER ((int32_t)0x811D010E)
#define VDEC2_ERR_OUTPUT_INFO ((int32_t)0x811D010F)
#define VDEC2_ERR_COMPUTE_QUEUE ((int32_t)0x811D0110)
#define VDEC2_ERR_CONFIG_INFO ((int32_t)0x811D0200)
#define VDEC2_ERR_COMPUTE_PIPE_ID ((int32_t)0x811D0201)
#define VDEC2_ERR_COMPUTE_QUEUE_ID ((int32_t)0x811D0202)
#define VDEC2_ERR_RESOURCE_TYPE ((int32_t)0x811D0203)
#define VDEC2_ERR_CODEC_TYPE ((int32_t)0x811D0204)
#define VDEC2_ERR_INPUT_QUEUE_DEPTH ((int32_t)0x811D0206)
#define VDEC2_ERR_DPB_FRAME_COUNT ((int32_t)0x811D0209)
#define VDEC2_ERR_FRAME_WIDTH_HEIGHT ((int32_t)0x811D020A)
#define VDEC2_ERR_ACCESS_UNIT ((int32_t)0x811D0301)
#define VDEC2_ERR_OVERSIZE_DECODE ((int32_t)0x811D0302)

#define CODEC_AVC 1u
#define CODEC_HEVC 974921u
#define CODEC_VP9 2382845u
#define MIN_MEMORY (16ull << 20)
/* Memory asked for the compute queue: nothing runs on it here, and games carve it from fixed
 * heaps (Sonic Origins' CRI movie heap of ~24 MB died with 16 MiB: its own buffers no longer fit) */
#define COMPUTE_MEMORY (1ull << 20)

/* Structure sizes (thisSize) and field offsets */
#define SZ_COMPUTE_MEMORY 24 /* thisSize, cpuGpuMemorySize, cpuGpuMemory */
#define SZ_COMPUTE_CONFIG 16 /* thisSize, u16 pipe, u16 queue, bool check, u8 r0, u16 r1 */
#define SZ_DECODER_CONFIG 72
#define SZ_DECODER_MEMORY 72
#define SZ_INPUT 48        /* thisSize, auData, auSize, pts, dts, attachedData */
#define SZ_OUTPUT 56       /* 48 before frameFormat / framePitchInBytes */
#define SZ_FRAME_BUFFER 32 /* thisSize, frameBuffer, frameBufferSize, bool isAccepted */
#define SZ_AVC_PICTURE 120 /* 104 in older SDKs */

#define MAX_DECODERS 8
#define MAX_PICTURES 64
#define HANDLE_BASE 0x7D2000ull /* decoder handles: HANDLE_BASE + index * 0x100 */

typedef struct vdec2 {
    bool used;
    pal_vdec *dec;
    uint32_t codec;
    int32_t max_w, max_h;
    uint64_t decodes, pictures;
} vdec2;

/* What each picture handed out was, for GetPictureInfo (keyed by frame buffer) */
typedef struct picture_entry {
    guest_addr buffer;
    uint32_t codec;
    pal_vdec_picture pic;
} picture_entry;

static vdec2 g_dec[MAX_DECODERS];
static picture_entry g_pics[MAX_PICTURES];
static uint32_t g_pic_next;
static pal_mutex *g_lock;

static vdec2 *decoder_of(uint64_t handle) {
    if (handle < HANDLE_BASE || (handle - HANDLE_BASE) % 0x100)
        return NULL;
    uint64_t i = (handle - HANDLE_BASE) / 0x100;
    return i < MAX_DECODERS && g_dec[i].used ? &g_dec[i] : NULL;
}

static bool rd64(hle_call *c, guest_addr a, uint64_t *v) {
    return gmem_read_u64(c->mem, a, v);
}
static bool rd32(hle_call *c, guest_addr a, uint32_t *v) {
    return gmem_read(c->mem, a, v, 4);
}
static void wr64(hle_call *c, guest_addr a, uint64_t v) {
    gmem_write_u64(c->mem, a, v);
}
static void wr32(hle_call *c, guest_addr a, uint32_t v) {
    gmem_write(c->mem, a, &v, 4);
}
static void wr8(hle_call *c, guest_addr a, uint8_t v) {
    gmem_write(c->mem, a, &v, 1);
}

static bool codec_known(uint32_t codec) {
    return codec == CODEC_AVC || codec == CODEC_HEVC || codec == CODEC_VP9;
}

/* SceVideodec2DecoderConfigInfo: checks shared by QueryDecoderMemoryInfo and CreateDecoder */
static int32_t check_config(hle_call *c, guest_addr cfg, bool need_queue) {
    uint8_t b[SZ_DECODER_CONFIG];
    if (!gmem_read(c->mem, cfg, b, sizeof(b)))
        return VDEC2_ERR_ARGUMENT_POINTER;
    uint32_t resource, codec, depth;
    int32_t w, h, dpb;
    uint64_t queue;
    memcpy(&resource, b + 8, 4), memcpy(&codec, b + 12, 4), memcpy(&w, b + 24, 4),
        memcpy(&h, b + 28, 4);
    memcpy(&dpb, b + 32, 4), memcpy(&depth, b + 36, 4), memcpy(&queue, b + 40, 8);
    if (resource != 1)
        return VDEC2_ERR_RESOURCE_TYPE;
    if (!codec_known(codec))
        return VDEC2_ERR_CODEC_TYPE;
    if (b[62] || b[63])
        return VDEC2_ERR_CONFIG_INFO;
    if (!depth)
        return VDEC2_ERR_INPUT_QUEUE_DEPTH;
    if (dpb < -1 || dpb == 0)
        return VDEC2_ERR_DPB_FRAME_COUNT;
    if (w < -1 || h < -1 || !w || !h)
        return VDEC2_ERR_FRAME_WIDTH_HEIGHT;
    if (need_queue && !queue)
        return VDEC2_ERR_COMPUTE_QUEUE;
    return 0;
}

/* NV12 frame size for the largest picture the configuration allows */
static uint64_t frame_size(int32_t w, int32_t h) {
    uint64_t pw = (uint64_t)((w > 0 ? w : 3840) + 255) & ~255ull, ph = (uint64_t)(h > 0 ? h : 2160);
    uint64_t n = pw * ph + pw * ((ph + 1) / 2);
    return n > MIN_MEMORY ? n : MIN_MEMORY;
}

/* int sceVideodec2QueryComputeMemoryInfo(SceVideodec2ComputeMemoryInfo *info) */
static void query_compute_memory_info(hle_call *c) {
    guest_addr info = hle_arg_ptr(c, 0);
    uint64_t size;
    if (!info || !rd64(c, info, &size)) {
        hle_ret_s32(c, VDEC2_ERR_ARGUMENT_POINTER);
        return;
    }
    if (size != SZ_COMPUTE_MEMORY) {
        hle_ret_s32(c, VDEC2_ERR_STRUCT_SIZE);
        return;
    }
    wr64(c, info + 8, COMPUTE_MEMORY);
    wr64(c, info + 16, 0);
    hle_ret_s32(c, 0);
}

/* int sceVideodec2AllocateComputeQueue(const ComputeConfigInfo *, const ComputeMemoryInfo *,
 * ComputeQueue *out) */
static void allocate_compute_queue(hle_call *c) {
    guest_addr cfg = hle_arg_ptr(c, 0), mem = hle_arg_ptr(c, 1), out = hle_arg_ptr(c, 2);
    uint8_t cb[SZ_COMPUTE_CONFIG];
    uint64_t msize = 0, mptr = 0, mthis = 0;
    if (!cfg || !mem || !out || !gmem_read(c->mem, cfg, cb, sizeof(cb)) || !rd64(c, mem, &mthis)) {
        hle_ret_s32(c, VDEC2_ERR_ARGUMENT_POINTER);
        return;
    }
    uint64_t cthis;
    uint16_t pipe, queue, r1;
    memcpy(&cthis, cb, 8), memcpy(&pipe, cb + 8, 2), memcpy(&queue, cb + 10, 2),
        memcpy(&r1, cb + 14, 2);
    if (cthis != SZ_COMPUTE_CONFIG || mthis != SZ_COMPUTE_MEMORY) {
        hle_ret_s32(c, VDEC2_ERR_STRUCT_SIZE);
        return;
    }
    if (cb[13] || r1) {
        hle_ret_s32(c, VDEC2_ERR_CONFIG_INFO);
        return;
    }
    if (pipe > 4) {
        hle_ret_s32(c, VDEC2_ERR_COMPUTE_PIPE_ID);
        return;
    }
    if (queue > 7) {
        hle_ret_s32(c, VDEC2_ERR_COMPUTE_QUEUE_ID);
        return;
    }
    rd64(c, mem + 8, &msize), rd64(c, mem + 16, &mptr);
    if (msize < COMPUTE_MEMORY) {
        hle_ret_s32(c, VDEC2_ERR_MEMORY_SIZE);
        return;
    }
    if (!mptr) {
        hle_ret_s32(c, VDEC2_ERR_MEMORY_POINTER);
        return;
    }
    wr64(c, out, mptr); /* the queue is its memory: nothing runs on it */
    hle_ret_s32(c, 0);
}

static void release_compute_queue(hle_call *c) {
    hle_ret_s32(c, hle_arg(c, 0) ? 0 : VDEC2_ERR_COMPUTE_QUEUE_ID);
}

/* int sceVideodec2QueryDecoderMemoryInfo(const DecoderConfigInfo *, DecoderMemoryInfo *) */
static void query_decoder_memory_info(hle_call *c) {
    guest_addr cfg = hle_arg_ptr(c, 0), mem = hle_arg_ptr(c, 1);
    uint64_t cthis = 0, mthis = 0;
    if (!cfg || !mem || !rd64(c, cfg, &cthis) || !rd64(c, mem, &mthis)) {
        hle_ret_s32(c, VDEC2_ERR_ARGUMENT_POINTER);
        return;
    }
    if (cthis != SZ_DECODER_CONFIG || mthis != SZ_DECODER_MEMORY) {
        hle_ret_s32(c, VDEC2_ERR_STRUCT_SIZE);
        return;
    }
    int32_t r = check_config(c, cfg, false);
    if (r) {
        hle_ret_s32(c, r);
        return;
    }
    int32_t w = 0, h = 0;
    rd32(c, cfg + 24, (uint32_t *)&w), rd32(c, cfg + 28, (uint32_t *)&h);
    uint64_t fb = frame_size(w, h);
    wr64(c, mem + 8, MIN_MEMORY), wr64(c, mem + 16, 0);  /* cpu */
    wr64(c, mem + 24, MIN_MEMORY), wr64(c, mem + 32, 0); /* gpu */
    wr64(c, mem + 40, MIN_MEMORY), wr64(c, mem + 48, 0); /* cpu+gpu */
    wr64(c, mem + 56, fb);                               /* maxFrameBufferSize */
    wr32(c, mem + 64, 0x100), wr32(c, mem + 68, 0);      /* frameBufferAlignment */
    hle_ret_s32(c, 0);
}

/* int sceVideodec2CreateDecoder(const DecoderConfigInfo *, const DecoderMemoryInfo *, Decoder *out)
 */
static void create_decoder(hle_call *c) {
    guest_addr cfg = hle_arg_ptr(c, 0), mem = hle_arg_ptr(c, 1), out = hle_arg_ptr(c, 2);
    uint64_t cthis = 0, mthis = 0;
    if (!cfg || !mem || !out || !rd64(c, cfg, &cthis) || !rd64(c, mem, &mthis)) {
        hle_ret_s32(c, VDEC2_ERR_ARGUMENT_POINTER);
        return;
    }
    if (cthis != SZ_DECODER_CONFIG || mthis != SZ_DECODER_MEMORY) {
        hle_ret_s32(c, VDEC2_ERR_STRUCT_SIZE);
        return;
    }
    int32_t r = check_config(c, cfg, true);
    if (r) {
        hle_ret_s32(c, r);
        return;
    }
    uint64_t m[7] = {0};
    for (int k = 0; k < 7; k++)
        rd64(c, mem + 8 + 8 * k, &m[k]);
    if (m[0] < MIN_MEMORY || m[2] < MIN_MEMORY || m[4] < MIN_MEMORY || m[6] < MIN_MEMORY) {
        hle_ret_s32(c, VDEC2_ERR_MEMORY_SIZE);
        return;
    }
    if (!m[1] || !m[3] || !m[5]) {
        hle_ret_s32(c, VDEC2_ERR_MEMORY_POINTER);
        return;
    }
    uint32_t codec = 0;
    int32_t w = 0, h = 0;
    rd32(c, cfg + 12, &codec), rd32(c, cfg + 24, (uint32_t *)&w), rd32(c, cfg + 28, (uint32_t *)&h);
    pal_vdec *dec = pal_vdec_open(codec == CODEC_AVC    ? PAL_VDEC_H264
                                  : codec == CODEC_HEVC ? PAL_VDEC_HEVC
                                                        : PAL_VDEC_VP9);
    if (!dec) {
        LOG_W(VIDEO, "videodec2: no host decoder for codec %u", codec);
        {
            hle_ret_s32(c, VDEC2_ERR_API_FAIL);
            return;
        }
    }
    pal_mutex_lock(g_lock);
    int slot = -1;
    for (int i = 0; i < MAX_DECODERS && slot < 0; i++)
        if (!g_dec[i].used)
            slot = i;
    if (slot >= 0) {
        memset(&g_dec[slot], 0, sizeof(g_dec[slot]));
        g_dec[slot].used = true, g_dec[slot].dec = dec, g_dec[slot].codec = codec;
        g_dec[slot].max_w = w, g_dec[slot].max_h = h;
    }
    pal_mutex_unlock(g_lock);
    if (slot < 0) {
        pal_vdec_close(dec);
        {
            hle_ret_s32(c, VDEC2_ERR_API_FAIL);
            return;
        }
    }
    wr64(c, out, HANDLE_BASE + (uint64_t)slot * 0x100);
    LOG_I(VIDEO, "videodec2: decoder %d, codec %s, up to %dx%d", slot,
          codec == CODEC_AVC    ? "H.264"
          : codec == CODEC_HEVC ? "HEVC"
                                : "VP9",
          w, h);
    hle_ret_s32(c, 0);
}

static void delete_decoder(hle_call *c) {
    pal_mutex_lock(g_lock);
    vdec2 *d = decoder_of(hle_arg(c, 0));
    pal_vdec *dec = d ? d->dec : NULL;
    if (d) {
        LOG_I(VIDEO, "videodec2: decoder %d closed after %llu access units, %llu pictures",
              (int)(d - g_dec), (unsigned long long)d->decodes, (unsigned long long)d->pictures);
        d->used = false;
    }
    pal_mutex_unlock(g_lock);
    pal_vdec_close(dec);
    hle_ret_s32(c, d ? 0 : VDEC2_ERR_DECODER_INSTANCE);
}

/* SceVideodec2OutputInfo before a decode: no picture, the frame buffer echoed */
static void output_empty(hle_call *c, guest_addr out, uint64_t out_size, uint32_t codec,
                         guest_addr fb, uint64_t fb_size) {
    uint8_t z[SZ_OUTPUT - 8] = {0};
    gmem_write(c->mem, out + 8, z, (size_t)out_size - 8);
    wr32(c, out + 12, codec);
    wr64(c, out + 32, fb), wr64(c, out + 40, fb_size);
}

static void output_picture(hle_call *c, vdec2 *d, guest_addr out, uint64_t out_size, guest_addr fb,
                           uint64_t fb_size, const pal_vdec_picture *pic) {
    wr8(c, out + 8, 1);            /* isValid */
    wr8(c, out + 9, pic->corrupt); /* isErrorFrame */
    wr8(c, out + 10, 1);           /* pictureCount */
    wr32(c, out + 12, d->codec);
    wr32(c, out + 16, pic->width), wr32(c, out + 20, pic->pitch), wr32(c, out + 24, pic->height);
    wr64(c, out + 32, fb), wr64(c, out + 40, fb_size);
    if (out_size == SZ_OUTPUT)
        wr32(c, out + 48, 0), wr32(c, out + 52, pic->pitch); /* frameFormat NV12, pitch in bytes */
    pal_mutex_lock(g_lock);
    picture_entry *e = NULL;
    for (uint32_t i = 0; i < MAX_PICTURES && !e; i++)
        if (g_pics[i].buffer == fb)
            e = &g_pics[i];
    if (!e)
        e = &g_pics[g_pic_next++ % MAX_PICTURES];
    e->buffer = fb, e->codec = d->codec, e->pic = *pic;
    d->pictures++;
    pal_mutex_unlock(g_lock);
}

/* The frame buffer and output structures both calls take; 0 or an error */
static int32_t take_buffers(hle_call *c, guest_addr fbs, guest_addr out, guest_addr *fb,
                            uint64_t *fb_size, uint64_t *out_size) {
    uint64_t fthis = 0;
    if (!fbs || !out || !rd64(c, fbs, &fthis) || !rd64(c, out, out_size))
        return VDEC2_ERR_ARGUMENT_POINTER;
    if (fthis != SZ_FRAME_BUFFER || (*out_size != SZ_OUTPUT && *out_size != SZ_OUTPUT - 8))
        return VDEC2_ERR_STRUCT_SIZE;
    rd64(c, fbs + 8, fb), rd64(c, fbs + 16, fb_size);
    if (!*fb_size)
        return VDEC2_ERR_FRAME_BUFFER_SIZE;
    if (!*fb)
        return VDEC2_ERR_FRAME_BUFFER_POINTER;
    return 0;
}

static int32_t picture_result(hle_call *c, vdec2 *d, int got, guest_addr fbs, guest_addr out,
                              uint64_t out_size, guest_addr fb, uint64_t fb_size,
                              const pal_vdec_picture *pic) {
    if (got == 1) {
        if ((d->max_w > 0 && (int32_t)pic->width > d->max_w) ||
            (d->max_h > 0 && (int32_t)pic->height > d->max_h))
            return VDEC2_ERR_OVERSIZE_DECODE;
        wr8(c, fbs + 24, 1); /* isAccepted */
        output_picture(c, d, out, out_size, fb, fb_size, pic);
        return 0;
    }
    return got == -2 ? VDEC2_ERR_FRAME_BUFFER_SIZE : got < 0 ? VDEC2_ERR_ACCESS_UNIT : 0;
}

/* int sceVideodec2Decode(Decoder, const InputData *, FrameBuffer *, OutputInfo *) */
static void decode(hle_call *c) {
    vdec2 *d = decoder_of(hle_arg(c, 0));
    if (!d) {
        hle_ret_s32(c, VDEC2_ERR_DECODER_INSTANCE);
        return;
    }
    guest_addr in = hle_arg_ptr(c, 1), fbs = hle_arg_ptr(c, 2), out = hle_arg_ptr(c, 3), fb = 0;
    uint64_t ithis = 0, fb_size = 0, out_size = 0, au = 0, au_size = 0, pts = 0, dts = 0, att = 0;
    if (!in || !rd64(c, in, &ithis)) {
        hle_ret_s32(c, VDEC2_ERR_ARGUMENT_POINTER);
        return;
    }
    int32_t r = take_buffers(c, fbs, out, &fb, &fb_size, &out_size);
    if (r) {
        hle_ret_s32(c, r);
        return;
    }
    if (ithis != SZ_INPUT) {
        hle_ret_s32(c, VDEC2_ERR_STRUCT_SIZE);
        return;
    }
    rd64(c, in + 8, &au), rd64(c, in + 16, &au_size), rd64(c, in + 24, &pts),
        rd64(c, in + 32, &dts), rd64(c, in + 40, &att);
    if (!au_size) {
        hle_ret_s32(c, VDEC2_ERR_ACCESS_UNIT_SIZE);
        return;
    }
    if (!au) {
        hle_ret_s32(c, VDEC2_ERR_ACCESS_UNIT_POINTER);
        return;
    }
    const void *data = gmem_host(c->mem, au, au_size);
    uint8_t *dst = (uint8_t *)gmem_host(c->mem, fb, fb_size);
    if (!data) {
        hle_ret_s32(c, VDEC2_ERR_ACCESS_UNIT_POINTER);
        return;
    }
    if (!dst) {
        hle_ret_s32(c, VDEC2_ERR_FRAME_BUFFER_POINTER);
        return;
    }
    wr8(c, fbs + 24, 0);
    output_empty(c, out, out_size, d->codec, fb, fb_size);
    pal_vdec_picture pic;
    int got = pal_vdec_decode(d->dec, data, au_size, pts, dts, att, dst, fb_size, &pic);
    d->decodes++;
    if (d->decodes == 1)
        LOG_I(VIDEO, "videodec2: first access unit, %llu bytes", (unsigned long long)au_size);
    hle_ret_s32(c, picture_result(c, d, got, fbs, out, out_size, fb, fb_size, &pic));
}

/* int sceVideodec2Flush(Decoder, FrameBuffer *, OutputInfo *): a picture still held */
static void flush(hle_call *c) {
    vdec2 *d = decoder_of(hle_arg(c, 0));
    if (!d) {
        hle_ret_s32(c, VDEC2_ERR_DECODER_INSTANCE);
        return;
    }
    guest_addr fbs = hle_arg_ptr(c, 1), out = hle_arg_ptr(c, 2), fb = 0;
    uint64_t fb_size = 0, out_size = 0;
    int32_t r = take_buffers(c, fbs, out, &fb, &fb_size, &out_size);
    if (r) {
        hle_ret_s32(c, r);
        return;
    }
    uint8_t *dst = (uint8_t *)gmem_host(c->mem, fb, fb_size);
    if (!dst) {
        hle_ret_s32(c, VDEC2_ERR_FRAME_BUFFER_POINTER);
        return;
    }
    wr8(c, fbs + 24, 0);
    output_empty(c, out, out_size, d->codec, fb, fb_size);
    pal_vdec_picture pic;
    int got = pal_vdec_flush(d->dec, dst, fb_size, &pic);
    hle_ret_s32(c, picture_result(c, d, got < 0 ? 0 : got, fbs, out, out_size, fb, fb_size, &pic));
}

static void reset(hle_call *c) {
    vdec2 *d = decoder_of(hle_arg(c, 0));
    if (!d) {
        hle_ret_s32(c, VDEC2_ERR_DECODER_INSTANCE);
        return;
    }
    pal_vdec_reset(d->dec);
    hle_ret_s32(c, 0);
}

/* The common head of the picture info structures: thisSize, isValid, pts, dts, attachedData */
static int32_t fill_common(hle_call *c, guest_addr dst, const picture_entry *e) {
    uint64_t size = 0;
    rd64(c, dst, &size);
    if (size < 40 || size > 256)
        return VDEC2_ERR_STRUCT_SIZE;
    uint8_t z[256] = {0};
    gmem_write(c->mem, dst + 8, z, (size_t)size - 8);
    if (e)
        wr8(c, dst + 8, 1), wr64(c, dst + 16, e->pic.pts), wr64(c, dst + 24, e->pic.dts),
            wr64(c, dst + 32, e->pic.attached);
    return 0;
}

/* int sceVideodec2GetPictureInfo(const OutputInfo *, void *first, void *second) */
static void get_picture_info(hle_call *c) {
    guest_addr out = hle_arg_ptr(c, 0), first = hle_arg_ptr(c, 1), second = hle_arg_ptr(c, 2),
               fb = 0;
    uint64_t out_size = 0;
    if (!out || !first || !rd64(c, out, &out_size)) {
        hle_ret_s32(c, VDEC2_ERR_ARGUMENT_POINTER);
        return;
    }
    if (out_size != SZ_OUTPUT && out_size != SZ_OUTPUT - 8) {
        hle_ret_s32(c, VDEC2_ERR_STRUCT_SIZE);
        return;
    }
    uint8_t valid = 0, count = 0;
    uint32_t codec = 0;
    gmem_read(c->mem, out + 8, &valid, 1), gmem_read(c->mem, out + 10, &count, 1);
    rd32(c, out + 12, &codec), rd64(c, out + 32, &fb);
    if (!valid || !count || !fb) {
        hle_ret_s32(c, VDEC2_ERR_OUTPUT_INFO);
        return;
    }
    picture_entry e;
    bool found = false;
    pal_mutex_lock(g_lock);
    for (uint32_t i = 0; i < MAX_PICTURES && !found; i++)
        if (g_pics[i].buffer == fb)
            e = g_pics[i], found = true;
    pal_mutex_unlock(g_lock);
    if (!found || e.codec != codec) {
        hle_ret_s32(c, VDEC2_ERR_OUTPUT_INFO);
        return;
    }
    if (codec == CODEC_AVC) {
        uint64_t size = 0;
        rd64(c, first, &size);
        if (size != SZ_AVC_PICTURE && (size | 16) != SZ_AVC_PICTURE) {
            hle_ret_s32(c, VDEC2_ERR_STRUCT_SIZE);
            return;
        }
        uint8_t p[SZ_AVC_PICTURE] = {0};
        const pal_vdec_picture *s = &e.pic;
        uint32_t wm = (s->width + 15) / 16 - 1, hm = (s->height + 15) / 16 - 1;
        memcpy(p, &size, 8);
        p[8] = 1;
        memcpy(p + 16, &s->pts, 8), memcpy(p + 24, &s->dts, 8), memcpy(p + 32, &s->attached, 8);
        p[40] = s->key, p[41] = (uint8_t)s->profile, p[42] = (uint8_t)s->level;
        memcpy(p + 44, &wm, 4), memcpy(p + 48, &hm, 4);
        p[52] = 1; /* frame_mbs_only */
        p[53] = s->crop_left || s->crop_right || s->crop_top || s->crop_bottom;
        memcpy(p + 56, &s->crop_left, 4), memcpy(p + 60, &s->crop_right, 4);
        memcpy(p + 64, &s->crop_top, 4), memcpy(p + 68, &s->crop_bottom, 4);
        p[72] = s->sar_width && s->sar_height, p[73] = p[72] ? 255 : 0;
        memcpy(p + 74, &s->sar_width, 2), memcpy(p + 76, &s->sar_height, 2);
        p[78] = 1, p[79] = 5, p[80] = s->full_range; /* video signal type: unspecified format */
        p[81] = s->primaries || s->transfer || s->matrix;
        p[82] = s->primaries, p[83] = s->transfer, p[84] = s->matrix;
        gmem_write(c->mem, first, p, (size_t)size);
    } else {
        int32_t r = fill_common(c, first, &e);
        if (r) {
            hle_ret_s32(c, r);
            return;
        }
    }
    if (second) {
        int32_t r = fill_common(c, second, NULL);
        if (r) {
            hle_ret_s32(c, r);
            return;
        }
    }
    hle_ret_s32(c, 0);
}

static void ok(hle_call *c) {
    hle_ret_s32(c, 0);
}

#define L "libSceVideodec2"

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(L, "sceVideodec2QueryComputeMemoryInfo", query_compute_memory_info),
    HLE_FUNC_ENTRY(L, "sceVideodec2AllocateComputeQueue", allocate_compute_queue),
    HLE_FUNC_ENTRY(L, "sceVideodec2ReleaseComputeQueue", release_compute_queue),
    HLE_FUNC_ENTRY(L, "sceVideodec2QueryDecoderMemoryInfo", query_decoder_memory_info),
    HLE_FUNC_ENTRY(L, "sceVideodec2CreateDecoder", create_decoder),
    HLE_FUNC_ENTRY(L, "sceVideodec2DeleteDecoder", delete_decoder),
    HLE_FUNC_ENTRY(L, "sceVideodec2Decode", decode),
    HLE_FUNC_ENTRY(L, "sceVideodec2Flush", flush),
    HLE_FUNC_ENTRY(L, "sceVideodec2Reset", reset),
    HLE_FUNC_ENTRY(L, "sceVideodec2GetPictureInfo", get_picture_info),
    HLE_FUNC_ENTRY(L, "sceVideodec2GetAvcPictureInfo", get_picture_info),
    HLE_FUNC_ENTRY(L, "sceVideodec2InitLibrary", ok),
    HLE_FUNC_ENTRY(L, "sceVideodec2TermLibrary", ok),
};

void hle_register_videodec2(void) {
    if (!g_lock && pal_mutex_create(&g_lock) != PS5C_OK) {
        LOG_E(HLE, "cannot create the Videodec2 lock");
        return;
    }
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}
