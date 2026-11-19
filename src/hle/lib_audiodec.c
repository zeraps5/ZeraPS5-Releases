/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceAudiodec: one compressed access unit at a time (ATRAC9 superframe, MP3 frame,
 * AAC frame) to interleaved PCM in the game's memory (Dragon Ball Sparking Zero,
 * Commandos Origins import it). Structures, checks and results as AnyPS5's
 * libSceAudiodec (GPL-2.0) and SharpEmu's CodecExports.cs (GPL-2.0) agree on; the
 * decoding is FFmpeg's, as in lib_ajm.c.
 *
 * Control (32 bytes): param, bsi info, au info, pcm item (pointers).
 * Au info / pcm item (24 bytes): u32 size (24), +8 address, +16 u32 bytes (in: what is
 * there; out: consumed / written).
 * Param: u32 size, s32 word size (0 24-bit in 32, 1 16-bit, 2 float), then per codec:
 *   ATRAC9 (12): u8 config[4]          MP3 (8)
 *   AAC (24, 28 or 32): u32 config (1 ADTS, 2 raw), u32 sampling index, u32 max channels,
 *   u32 HE-AAC, [u32 non-delay output], [u32 surround interleave order].
 * Bsi info: ATRAC9 (36): u32 size, channels, bitrate, rate, superframe bytes, frames per
 *   superframe, next frame bytes, frame samples, s32 result.
 *   MP3 (20): u32 size, u32 header, u8 crc, mode, mode extension, copyright, original,
 *   emphasis, 2 reserved, s32 result.
 *   AAC (20): u32 size, rate, channels, HE-AAC, s32 result. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "hle/hle.h"
#include "pal/pal.h"

#ifdef PS5C_HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#endif

void hle_register_table(const hle_export *table, size_t count);

#define E_API_FAIL ((int32_t)0x807F0000)
#define E_TYPE ((int32_t)0x807F0001)
#define E_ARG ((int32_t)0x807F0002)
#define E_PARAM_SIZE ((int32_t)0x807F0004)
#define E_BSI_SIZE ((int32_t)0x807F0005)
#define E_AU_INFO_SIZE ((int32_t)0x807F0006)
#define E_PCM_ITEM_SIZE ((int32_t)0x807F0007)
#define E_CTRL_PTR ((int32_t)0x807F0008)
#define E_PARAM_PTR ((int32_t)0x807F0009)
#define E_BSI_PTR ((int32_t)0x807F000A)
#define E_AU_INFO_PTR ((int32_t)0x807F000B)
#define E_PCM_ITEM_PTR ((int32_t)0x807F000C)
#define E_AU_PTR ((int32_t)0x807F000D)
#define E_PCM_PTR ((int32_t)0x807F000E)
#define E_HANDLE ((int32_t)0x807F000F)
#define E_WORD ((int32_t)0x807F0010)
#define E_AU_SIZE ((int32_t)0x807F0011)
#define E_PCM_SIZE ((int32_t)0x807F0012)
#define E_AAC_FREQ ((int32_t)0x807F0300)
#define E_AAC_HEAAC ((int32_t)0x807F0302)
#define E_AAC_CONFIG ((int32_t)0x807F0303)
#define E_AAC_CHANNELS ((int32_t)0x807F0304)
#define E_AT9_CONFIG ((int32_t)0x807F1000)

enum { TYPE_AT9 = 1, TYPE_MP3 = 2, TYPE_AAC = 3 };
enum { WORD_S32 = 0, WORD_S16 = 1, WORD_FLOAT = 2 };
#define AAC_ADTS 1
#define AAC_RAW 2
#define AAC_RESULT_DECODE_ERROR (-4)
#define AAC_RESULT_INSUFFICIENT_DATA (-5)
#define MAX_DECODERS 64
#define MAX_AU_BYTES (16u << 20)

typedef struct ctrl {
    guest_addr param, bsi, au_info, pcm_item;
    uint32_t param_size, bsi_size;
    int32_t word;
    guest_addr au, pcm;
    uint32_t au_size, pcm_size;
} ctrl;

typedef struct decoder {
    bool used;
    uint32_t type;
    int32_t word;
    /* ATRAC9 */
    uint8_t at9_config[4];
    uint32_t channels, rate, superframe_bytes, frames_per_superframe, frame_samples;
    /* AAC */
    bool adts;
    uint32_t aac_index, aac_channels;
#ifdef PS5C_HAVE_FFMPEG
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
#endif
} decoder;

static pal_mutex *g_lock;
static uint32_t g_init[4];
static decoder g_dec[MAX_DECODERS];

static const uint32_t k_aac_rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
static const uint32_t k_at9_rates[16] = {11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
                                         44100, 48000, 64000, 88200, 96000, 128000, 176400, 192000};
static const uint8_t k_at9_frame_log2[16] = {6, 6, 7, 7, 7, 8, 8, 8, 6, 6, 7, 7, 7, 8, 8, 8};
static const uint8_t k_at9_channels[8] = {1, 2, 2, 6, 8, 4, 1, 2}; /* 6 and 7: vibration layouts */

static void lock(void) {
    if (!g_lock && pal_mutex_create(&g_lock) != PS5C_OK) pal_exit_process(3);
    pal_mutex_lock(g_lock);
}

static bool valid_type(uint32_t t) { return t == TYPE_AT9 || t == TYPE_MP3 || t == TYPE_AAC; }

static uint32_t word_bytes(int32_t word) { return word == WORD_S16 ? 2u : 4u; }

/* The control and the sizes its structures declare; buffers too when decoding. */
static int32_t read_ctrl(hle_call *c, guest_addr at, uint32_t type, bool decode, ctrl *out) {
    memset(out, 0, sizeof(*out));
    uint64_t p[4];
    if (!at || !gmem_read(c->mem, at, p, sizeof(p))) return E_CTRL_PTR;
    out->param = p[0], out->bsi = p[1], out->au_info = p[2], out->pcm_item = p[3];
    uint32_t head[2];
    if (!out->param || !gmem_read(c->mem, out->param, head, sizeof(head))) return E_PARAM_PTR;
    out->param_size = head[0];
    out->word = (int32_t)head[1];
    if (!out->bsi || !gmem_read(c->mem, out->bsi, &out->bsi_size, 4)) return E_BSI_PTR;
    switch (type) {
    case TYPE_AT9:
        if (out->param_size != 12) return E_PARAM_SIZE;
        if (out->bsi_size != 36) return E_BSI_SIZE;
        break;
    case TYPE_MP3:
        if (out->param_size != 8) return E_PARAM_SIZE;
        if (out->bsi_size != 20) return E_BSI_SIZE;
        break;
    default:
        if (out->param_size != 24 && out->param_size != 28 && out->param_size != 32) return E_PARAM_SIZE;
        if (out->bsi_size != 20) return E_BSI_SIZE;
        break;
    }
    if (out->word < WORD_S32 || out->word > WORD_FLOAT || (out->word == WORD_FLOAT && type == TYPE_MP3)) return E_WORD;
    if (!decode) return 0;
    uint8_t item[24];
    if (!out->au_info || !gmem_read(c->mem, out->au_info, item, sizeof(item))) return E_AU_INFO_PTR;
    if (*(uint32_t *)item != 24) return E_AU_INFO_SIZE;
    memcpy(&out->au, item + 8, 8);
    memcpy(&out->au_size, item + 16, 4);
    if (!out->pcm_item || !gmem_read(c->mem, out->pcm_item, item, sizeof(item))) return E_PCM_ITEM_PTR;
    if (*(uint32_t *)item != 24) return E_PCM_ITEM_SIZE;
    memcpy(&out->pcm, item + 8, 8);
    memcpy(&out->pcm_size, item + 16, 4);
    if (!out->au) return E_AU_PTR;
    if (!out->pcm) return E_PCM_PTR;
    if (!out->au_size || out->au_size > MAX_AU_BYTES) return E_AU_SIZE;
    if (!out->pcm_size) return E_PCM_SIZE;
    return 0;
}

/* What the decode consumed and wrote, back into the au info and pcm item. */
static void put_sizes(hle_call *c, const ctrl *k, uint32_t consumed, uint32_t written) {
    gmem_write(c->mem, k->au_info + 16, &consumed, 4);
    gmem_write(c->mem, k->pcm_item + 16, &written, 4);
}

static void put_at9_info(hle_call *c, guest_addr at, const decoder *d, int32_t result) {
    uint32_t superframe_samples = d->frame_samples * d->frames_per_superframe;
    uint32_t info[9] = {36, d->channels,
                        superframe_samples ? (uint32_t)((uint64_t)d->superframe_bytes * 8 * d->rate / superframe_samples) : 0,
                        d->rate, d->superframe_bytes, d->frames_per_superframe, d->superframe_bytes, d->frame_samples,
                        (uint32_t)result};
    gmem_write(c->mem, at, info, sizeof(info));
}

static void put_aac_info(hle_call *c, guest_addr at, uint32_t rate, uint32_t channels, uint32_t he, int32_t result) {
    uint32_t info[5] = {20, rate, channels, he, (uint32_t)result};
    gmem_write(c->mem, at, info, sizeof(info));
}

/* MPEG audio layer III header: the frame's length, or 0 when the 4 bytes are not one. */
static uint32_t mp3_frame_bytes(uint32_t word) {
    uint32_t version = word >> 19 & 3, layer = word >> 17 & 3, bitrate_idx = word >> 12 & 15, rate_idx = word >> 10 & 3;
    if ((word & 0xFFE00000u) != 0xFFE00000u || version == 1 || layer != 1 || !bitrate_idx || bitrate_idx == 15 || rate_idx == 3)
        return 0;
    static const uint32_t rates[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};
    static const uint32_t mpeg1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const uint32_t mpeg2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    bool v1 = version == 3;
    uint32_t kbps = (v1 ? mpeg1 : mpeg2)[bitrate_idx];
    return (v1 ? 144u : 72u) * kbps * 1000 / rates[version][rate_idx] + (word >> 9 & 1);
}

static void put_mp3_info(hle_call *c, guest_addr at, uint32_t word, int32_t result) {
    uint8_t info[20] = {0};
    uint32_t size = 20;
    memcpy(info, &size, 4);
    memcpy(info + 4, &word, 4);
    info[8] = (word >> 16 & 1) == 0;  /* crc */
    info[9] = (uint8_t)(word >> 6 & 3);  /* mode */
    info[10] = (uint8_t)(word >> 4 & 3); /* mode extension */
    info[11] = (uint8_t)(word >> 3 & 1); /* copyright */
    info[12] = (uint8_t)(word >> 2 & 1); /* original */
    info[13] = (uint8_t)(word & 3);      /* emphasis */
    memcpy(info + 16, &result, 4);
    gmem_write(c->mem, at, info, sizeof(info));
}

#ifdef PS5C_HAVE_FFMPEG

static void codec_close(decoder *d) {
    avcodec_free_context(&d->ctx);
    av_packet_free(&d->pkt);
    av_frame_free(&d->frame);
}

static bool codec_open(decoder *d, enum AVCodecID id, const uint8_t *extra, int extra_size, int block_align) {
    codec_close(d);
    const AVCodec *codec = avcodec_find_decoder(id);
    d->ctx = codec ? avcodec_alloc_context3(codec) : NULL;
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (!d->ctx || !d->pkt || !d->frame) {
        codec_close(d);
        return false;
    }
    if (extra_size) {
        d->ctx->extradata = (uint8_t *)av_mallocz((size_t)extra_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!d->ctx->extradata) {
            codec_close(d);
            return false;
        }
        memcpy(d->ctx->extradata, extra, (size_t)extra_size);
        d->ctx->extradata_size = extra_size;
    }
    d->ctx->block_align = block_align;
    if (avcodec_open2(d->ctx, codec, NULL) < 0) {
        LOG_W(HLE, "audiodec: cannot open the %s decoder", codec->name);
        codec_close(d);
        return false;
    }
    return true;
}

/* AudioSpecificConfig of an AAC-LC stream without SBR signalling (raw access units). */
static int aac_config(uint8_t out[2], uint32_t index, uint32_t channels) {
    uint32_t v = 2u << 11 | (index & 15) << 7 | (channels & 15) << 3;
    out[0] = (uint8_t)(v >> 8);
    out[1] = (uint8_t)v;
    return 2;
}

static bool open_decoder(decoder *d) {
    if (d->type == TYPE_AT9) {
        uint8_t cfg[4];
        memcpy(cfg, d->at9_config, 4);
        uint32_t layout = (cfg[1] >> 1) & 7;
        if (layout >= 6) cfg[1] = (uint8_t)(cfg[1] - (6u << 1)); /* vibration: decoded as mono / stereo */
        uint8_t extra[12] = {2, 0, 0, 0, cfg[0], cfg[1], cfg[2], cfg[3], 0, 0, 0, 0};
        return codec_open(d, AV_CODEC_ID_ATRAC9, extra, 12, (int)d->superframe_bytes);
    }
    if (d->type == TYPE_MP3) return codec_open(d, AV_CODEC_ID_MP3, NULL, 0, 0);
    if (d->adts) return codec_open(d, AV_CODEC_ID_AAC, NULL, 0, 0);
    uint8_t asc[2]; /* mono or stereo, as AnyPS5 tries first (most streams) */
    return codec_open(d, AV_CODEC_ID_AAC, asc, aac_config(asc, d->aac_index, d->aac_channels < 2 ? 1 : 2), 0);
}

static float sample_at(const AVFrame *f, int ch, int i) {
    int chs = f->ch_layout.nb_channels;
    switch (f->format) {
    case AV_SAMPLE_FMT_FLTP: return ((const float *)f->extended_data[ch])[i];
    case AV_SAMPLE_FMT_FLT: return ((const float *)f->extended_data[0])[i * chs + ch];
    case AV_SAMPLE_FMT_S16P: return ((const int16_t *)f->extended_data[ch])[i] / 32768.0f;
    case AV_SAMPLE_FMT_S16: return ((const int16_t *)f->extended_data[0])[i * chs + ch] / 32768.0f;
    case AV_SAMPLE_FMT_S32P: return (float)((const int32_t *)f->extended_data[ch])[i] / 2147483648.0f;
    case AV_SAMPLE_FMT_S32: return (float)((const int32_t *)f->extended_data[0])[i * chs + ch] / 2147483648.0f;
    default: return 0.0f;
    }
}

static void put_sample(uint8_t *out, float v, int32_t word) {
    if (word == WORD_FLOAT) {
        memcpy(out, &v, 4);
        return;
    }
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    if (word == WORD_S16) {
        int16_t s = (int16_t)lrintf(v * 32767.0f);
        memcpy(out, &s, 2);
    } else {
        int32_t s = (int32_t)lrint((double)v * 2147483647.0);
        memcpy(out, &s, 4);
    }
}

typedef struct decoded {
    uint32_t written, channels, rate;
    bool he;
    int status; /* 0 ok, 1 bad data, 2 no room */
} decoded;

/* One packet into PCM at out (room bytes). */
static decoded decode_packet(decoder *d, const uint8_t *data, uint32_t size, uint8_t *out, uint32_t room) {
    decoded r = {0};
    if (!d->ctx) { r.status = 1; return r; }
    d->pkt->data = (uint8_t *)data;
    d->pkt->size = (int)size;
    if (avcodec_send_packet(d->ctx, d->pkt) < 0) r.status = 1;
    uint32_t bytes = word_bytes(d->word);
    while (!r.status && avcodec_receive_frame(d->ctx, d->frame) == 0) {
        int chs = d->frame->ch_layout.nb_channels, n = d->frame->nb_samples;
        if (d->type == TYPE_AT9 && (uint32_t)chs > d->channels) chs = (int)d->channels;
        uint64_t need = (uint64_t)n * (uint32_t)chs * bytes;
        if (r.written + need > room) {
            r.status = 2;
        } else {
            for (int i = 0; i < n; i++)
                for (int ch = 0; ch < chs; ch++, r.written += bytes) put_sample(out + r.written, sample_at(d->frame, ch, i), d->word);
        }
        r.channels = (uint32_t)chs;
        r.rate = (uint32_t)d->frame->sample_rate;
        r.he = d->ctx->profile == AV_PROFILE_AAC_HE || d->ctx->profile == AV_PROFILE_AAC_HE_V2;
        av_frame_unref(d->frame);
    }
    d->pkt->data = NULL;
    d->pkt->size = 0;
    return r;
}

#else

static void codec_close(decoder *d) { (void)d; }
static bool open_decoder(decoder *d) { (void)d; return true; }

#endif

static decoder *find(int32_t handle) {
    if (handle <= 0 || handle > MAX_DECODERS || !g_dec[handle - 1].used) return NULL;
    return &g_dec[handle - 1];
}

/* int sceAudiodecInitLibrary(uint32_t codec_type) */
static void init_library(hle_call *c) {
    uint32_t t = hle_arg_u32(c, 0);
    if (!valid_type(t)) { hle_ret_s32(c, E_TYPE); return; }
    lock();
    g_init[t]++;
    pal_mutex_unlock(g_lock);
    hle_ret_s32(c, 0);
}

/* int sceAudiodecTermLibrary(uint32_t codec_type) */
static void term_library(hle_call *c) {
    uint32_t t = hle_arg_u32(c, 0);
    if (!valid_type(t)) { hle_ret_s32(c, E_TYPE); return; }
    lock();
    if (g_init[t]) g_init[t]--;
    pal_mutex_unlock(g_lock);
    hle_ret_s32(c, 0);
}

/* Fills a new decoder from the param; bsi info written. */
static int32_t setup(hle_call *c, const ctrl *k, decoder *d) {
    if (d->type == TYPE_AT9) {
        if (!gmem_read(c->mem, k->param + 8, d->at9_config, 4)) return E_PARAM_PTR;
        const uint8_t *cfg = d->at9_config;
        if (cfg[0] != 0xFE) return E_AT9_CONFIG;
        uint32_t bits = (uint32_t)cfg[1] << 16 | (uint32_t)cfg[2] << 8 | cfg[3];
        uint32_t rate_idx = bits >> 20, layout = (bits >> 17) & 7;
        uint32_t frame_bytes = ((bits >> 5) & 0x7FF) + 1, superframe_idx = (bits >> 3) & 3;
        d->rate = k_at9_rates[rate_idx];
        d->channels = k_at9_channels[layout];
        d->frames_per_superframe = 1u << superframe_idx;
        d->superframe_bytes = frame_bytes << superframe_idx;
        d->frame_samples = 1u << k_at9_frame_log2[rate_idx];
        if (!open_decoder(d)) return E_AT9_CONFIG;
        put_at9_info(c, k->bsi, d, 0);
        return 0;
    }
    if (d->type == TYPE_MP3) return open_decoder(d) ? 0 : E_API_FAIL;
    uint32_t p[8] = {0};
    if (!gmem_read(c->mem, k->param, p, k->param_size)) return E_PARAM_PTR;
    uint32_t config = p[2], index = p[3], channels = p[4], he = p[5];
    if (k->param_size >= 28 && p[6] > 1) return E_ARG;
    if (config != AAC_ADTS && config != AAC_RAW) return E_AAC_CONFIG;
    uint32_t rate = index < 13 ? k_aac_rates[index] : 0;
    if (config == AAC_RAW && !rate) return E_AAC_FREQ;
    if (channels != 1 && channels != 2 && channels != 6 && channels != 8) return E_AAC_CHANNELS;
    if (he > 1 || (he && channels > 2)) return E_AAC_HEAAC;
    d->adts = config == AAC_ADTS;
    d->aac_index = index;
    d->aac_channels = channels;
    d->channels = channels;
    d->rate = rate;
    if (!open_decoder(d)) return E_API_FAIL;
    put_aac_info(c, k->bsi, rate, channels, 0, 0);
    return 0;
}

/* int sceAudiodecCreateDecoder(ctrl *, uint32_t codec_type): a handle (1..64) or an error */
static void create_decoder(hle_call *c) {
    uint32_t t = hle_arg_u32(c, 1);
    if (!valid_type(t)) { hle_ret_s32(c, E_TYPE); return; }
    ctrl k;
    int32_t e = read_ctrl(c, hle_arg_ptr(c, 0), t, false, &k);
    if (e) { hle_ret_s32(c, e); return; }
    lock();
    int32_t ret = E_API_FAIL;
    if (!g_init[t]) ret = E_ARG;
    for (uint32_t i = 0; g_init[t] && i < MAX_DECODERS; i++) {
        if (g_dec[i].used) continue;
        decoder *d = &g_dec[i];
        memset(d, 0, sizeof(*d));
        d->type = t;
        d->word = k.word;
        e = setup(c, &k, d);
        if (e) {
            codec_close(d);
            memset(d, 0, sizeof(*d));
            ret = e;
        } else {
            d->used = true;
            ret = (int32_t)i + 1;
        }
        break;
    }
    pal_mutex_unlock(g_lock);
    static unsigned logged;
    if (logged++ < 8) LOG_I(HLE, "sceAudiodecCreateDecoder: type %u, word %d -> 0x%x", t, k.word, (unsigned)ret);
    hle_ret_s32(c, ret);
}

/* int sceAudiodecDeleteDecoder(int32_t handle) */
static void delete_decoder(hle_call *c) {
    lock();
    decoder *d = find(hle_arg_s32(c, 0));
    if (d) {
        codec_close(d);
        memset(d, 0, sizeof(*d));
    }
    pal_mutex_unlock(g_lock);
    hle_ret_s32(c, d ? 0 : E_HANDLE);
}

/* int sceAudiodecClearContext(int32_t handle) */
static void clear_context(hle_call *c) {
    lock();
    decoder *d = find(hle_arg_s32(c, 0));
#ifdef PS5C_HAVE_FFMPEG
    if (d && d->ctx) avcodec_flush_buffers(d->ctx);
#endif
    pal_mutex_unlock(g_lock);
    hle_ret_s32(c, d ? 0 : E_HANDLE);
}

/* int sceAudiodecDecode(int32_t handle, ctrl *) */
static void decode(hle_call *c) {
    lock();
    decoder *d = find(hle_arg_s32(c, 0));
    ctrl k;
    int32_t e = d ? read_ctrl(c, hle_arg_ptr(c, 1), d->type, true, &k) : E_HANDLE;
    if (e) {
        pal_mutex_unlock(g_lock);
        hle_ret_s32(c, e);
        return;
    }
    uint8_t *in = (uint8_t *)malloc((size_t)k.au_size + 64), *out = (uint8_t *)malloc(k.pcm_size);
    if (!in || !out || !gmem_read(c->mem, k.au, in, k.au_size)) {
        free(in);
        free(out);
        pal_mutex_unlock(g_lock);
        hle_ret_s32(c, !in || !out ? E_API_FAIL : E_AU_PTR);
        return;
    }
    memset(in + k.au_size, 0, 64);
    uint32_t consumed = 0, written = 0, mp3_word = 0;
    int32_t ret = 0;
#ifdef PS5C_HAVE_FFMPEG
    decoded r = {0};
    if (d->type == TYPE_AT9) {
        if (k.au_size < d->superframe_bytes) r.status = 1;
        else r = decode_packet(d, in, d->superframe_bytes, out, k.pcm_size), consumed = d->superframe_bytes;
    } else if (d->type == TYPE_MP3) {
        mp3_word = k.au_size >= 4 ? (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3] : 0;
        uint32_t frame = mp3_frame_bytes(mp3_word);
        if (!frame || frame > k.au_size) r.status = 1;
        else r = decode_packet(d, in, frame, out, k.pcm_size), consumed = frame;
    } else {
        uint32_t size = k.au_size;
        if (d->adts) { /* one ADTS frame */
            if (size < 7 || in[0] != 0xFF || (in[1] & 0xF6) != 0xF0) size = 0;
            else {
                uint32_t frame = (uint32_t)(in[3] & 3) << 11 | (uint32_t)in[4] << 3 | in[5] >> 5;
                size = frame >= 7 && frame <= k.au_size ? frame : 0;
            }
        }
        if (!size) r.status = 1;
        else r = decode_packet(d, in, size, out, k.pcm_size), consumed = size;
        put_aac_info(c, k.bsi, r.rate ? r.rate : d->rate, r.channels ? r.channels : d->channels, r.he,
                     r.status == 1 ? AAC_RESULT_DECODE_ERROR : 0);
    }
    written = r.written;
    if (r.status == 2) ret = E_PCM_SIZE;
    else if (r.status == 1) {
        consumed = written = 0;
        ret = E_API_FAIL;
        if (d->ctx) avcodec_flush_buffers(d->ctx);
    }
    static unsigned bad;
    if (ret && bad++ < 8)
        LOG_W(HLE, "sceAudiodecDecode: type %u, %u bytes in, %u bytes of room: 0x%x", d->type, k.au_size, k.pcm_size, (unsigned)ret);
#else
    /* no decoder in this build: one frame of silence */
    consumed = k.au_size;
    written = k.pcm_size;
    memset(out, 0, written);
#endif
    if (written && !gmem_write(c->mem, k.pcm, out, written)) written = 0, ret = E_PCM_PTR;
    if (ret != E_PCM_SIZE) put_sizes(c, &k, consumed, written);
    if (!ret && d->type == TYPE_AT9) put_at9_info(c, k.bsi, d, 0);
    if (!ret && d->type == TYPE_MP3) put_mp3_info(c, k.bsi, mp3_word, 0);
    free(in);
    free(out);
    pal_mutex_unlock(g_lock);
    hle_ret_s32(c, ret);
}

#define L "libSceAudiodec"
static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(L, "sceAudiodecInitLibrary", init_library),
    HLE_FUNC_ENTRY(L, "sceAudiodecTermLibrary", term_library),
    HLE_FUNC_ENTRY(L, "sceAudiodecCreateDecoder", create_decoder),
    HLE_FUNC_ENTRY(L, "sceAudiodecDeleteDecoder", delete_decoder),
    HLE_FUNC_ENTRY(L, "sceAudiodecClearContext", clear_context),
    HLE_FUNC_ENTRY(L, "sceAudiodecDecode", decode),
};
#undef L

void hle_register_audiodec(void) {
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}
