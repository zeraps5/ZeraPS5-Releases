/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceAjm: the audio decoders games feed compressed sound through (ATRAC9,
 * MPEG-4 AAC, MP3, Opus), on top of FFmpeg. A batch is a buffer the game
 * fills with jobs and then starts; here each job runs when it is added (its
 * result structure is written right away) and only reserves its room in the
 * batch, so starting and waiting just hand out batch ids.
 * Result structures (sidebands): result {s32 result, s32 internal}, stream
 * {s32 consumed, s32 written, u64 total samples}, multiple frames {u32
 * frames, u32 reserved}, format {channels, channel mask, rate, encoding,
 * bitrate, reserved}, gapless {u32 total samples, u16 skip, u16 skipped}.
 * Instance flags: channels in bits 3-6, sample encoding (0 S16, 1 S32,
 * 2 float) in bits 7-9, codec-specific ones from bit 32. */
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/hle.h"
#include "hle/ajm_resample.h"
#include "pal/pal.h"

#ifdef PS5C_HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#endif

void hle_register_table(const hle_export *table, size_t count);

#define AJM_ERR_INVALID_CONTEXT ((int32_t)0x80930002)
#define AJM_ERR_INVALID_INSTANCE ((int32_t)0x80930003)
#define AJM_ERR_INVALID_PARAMETER ((int32_t)0x80930005)
#define AJM_ERR_CODEC_NOT_SUPPORTED ((int32_t)0x80930008)
#define AJM_ERR_JOB_CREATION ((int32_t)0x80930012)

/* result bits of a job */
#define RES_NOT_INITIALIZED 0x1
#define RES_INVALID_DATA 0x2
#define RES_INVALID_PARAMETER 0x4
#define RES_PARTIAL_INPUT 0x8
#define RES_NOT_ENOUGH_ROOM 0x10
#define RES_CODEC_ERROR 0x40000000
#define RES_FATAL ((int32_t)0x80000000)

#define CODEC_MP3 0
#define CODEC_AT9 1
#define CODEC_AAC 2
#define CODEC_NEW_ABI 14
#define CODEC_OPUS 24

#define ENC_S16 0
#define ENC_S32 1
#define ENC_FLOAT 2

/* room each job takes in the batch buffer */
#define JOB_CONTROL_SIZE 48
#define JOB_RUN_SIZE 64

#define MAX_INSTANCES 512
#define AT9_PARSE_RIFF_HEADER (1ull << 32)
#define AT9_CACHE_FRAMES 16 /* decoded ATRAC9 frames waiting to be handed out */

typedef struct gapless_state {
    uint32_t total; /* samples still to output (0 with limit: the end) */
    uint16_t skip, skipped;
} gapless_state;

typedef struct job_result {
    int32_t result, internal;
    uint64_t consumed, written; /* bytes */
    uint32_t frames;
} job_result;

typedef struct ajm_instance {
    bool used;
    uint32_t id, codec, max_channels, encoding;
    uint64_t flags;
    bool initialized;
    uint32_t channels, rate;
    uint64_t total_samples;
    ajm_resampler resampler;
    gapless_state g_init, g_cur;
    bool has_limit;
#ifdef PS5C_HAVE_FFMPEG
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
#endif
    /* ATRAC9 */
    uint8_t at9_config[4];
    uint32_t at9_frame_bytes, at9_frames_per_superframe, at9_frame_samples, at9_frame_in_superframe;
    uint8_t *at9_sf;    /* the superframe being gathered */
    uint32_t at9_sf_have;
    bool at9_direct;    /* the current superframe was decoded straight from the input */
    uint32_t at9_rejected; /* superframes FFmpeg refused, played as silence */
    uint8_t *at9_cache; /* AT9_CACHE_FRAMES decoded frames */
    uint32_t at9_cache_head, at9_cache_count;
    /* AAC */
    uint32_t aac_config, aac_freq_index;
    /* MP3 */
    uint32_t mp3_header;
    /* Opus */
    uint32_t opus_frames_per_packet;
} ajm_instance;

static struct {
    pal_mutex *lock;
    ajm_instance inst[MAX_INSTANCES];
    uint32_t next_slot, next_batch;
    guest_addr error_text;
} g_ajm;

static ajm_instance *find(uint32_t id) {
    for (uint32_t i = 0; i < MAX_INSTANCES; i++)
        if (g_ajm.inst[i].used && g_ajm.inst[i].id == id) return &g_ajm.inst[i];
    return NULL;
}

static uint32_t bytes_per_sample(uint32_t enc) { return enc == ENC_S16 ? 2 : 4; }

static uint32_t channel_mask(uint32_t channels) {
    static const uint32_t masks[9] = {0, 0x4, 0x3, 0x7, 0x33, 0x607, 0x60f, 0x70f, 0x63f};
    return channels <= 8 ? masks[channels] : 0;
}

/* ---- Gapless decoding: skip the leading samples, stop after the total ---- */

static void gapless_reset(ajm_instance *in) {
    in->g_cur = in->g_init;
    in->g_cur.skipped = 0;
}

static bool gapless_end(const ajm_instance *in) { return in->has_limit && in->g_cur.total == 0; }

/* Of `samples` decoded, how many to drop in front and how many to keep */
static void gapless_take(ajm_instance *in, uint32_t samples, uint32_t *skip, uint32_t *keep) {
    *skip = in->g_cur.skip < samples ? in->g_cur.skip : samples;
    *keep = samples - *skip;
    if (in->has_limit && *keep > in->g_cur.total) *keep = in->g_cur.total;
    in->g_cur.skip = (uint16_t)(in->g_cur.skip - *skip);
    if (in->has_limit) in->g_cur.total -= *keep;
    uint32_t skipped = in->g_cur.skipped + (samples - *keep);
    in->g_cur.skipped = (uint16_t)(skipped > 0xFFFF ? 0xFFFF : skipped);
}

#ifdef PS5C_HAVE_FFMPEG

/* ---- FFmpeg decoders ---- */

static void codec_close(ajm_instance *in) {
    ajm_resample_free(&in->resampler);
    free(in->at9_sf);
    free(in->at9_cache);
    in->at9_sf = in->at9_cache = NULL;
    in->at9_sf_have = in->at9_cache_head = in->at9_cache_count = 0;
    avcodec_free_context(&in->ctx);
    av_packet_free(&in->pkt);
    av_frame_free(&in->frame);
}

static bool codec_open(ajm_instance *in, enum AVCodecID id, const uint8_t *extradata, int extradata_size,
                       int block_align, int channels, int rate) {
    codec_close(in);
    const AVCodec *codec = avcodec_find_decoder(id);
    in->ctx = codec ? avcodec_alloc_context3(codec) : NULL;
    in->pkt = av_packet_alloc();
    in->frame = av_frame_alloc();
    if (!in->ctx || !in->pkt || !in->frame) {
        codec_close(in);
        return false;
    }
    if (extradata_size) {
        in->ctx->extradata = (uint8_t *)av_mallocz((size_t)extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!in->ctx->extradata) {
            codec_close(in);
            return false;
        }
        memcpy(in->ctx->extradata, extradata, (size_t)extradata_size);
        in->ctx->extradata_size = extradata_size;
    }
    in->ctx->block_align = block_align;
    if (channels) av_channel_layout_default(&in->ctx->ch_layout, channels);
    if (rate) in->ctx->sample_rate = rate;
    if (avcodec_open2(in->ctx, codec, NULL) < 0) {
        LOG_W(HLE, "ajm: cannot open the %s decoder", codec->name);
        codec_close(in);
        return false;
    }
    return true;
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

/* Frame samples [first, first + count) interleaved in the instance's encoding */
static void write_pcm(const ajm_instance *in, const AVFrame *f, uint32_t first, uint32_t count, uint8_t *out) {
    uint32_t chs = in->channels;
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t c = 0; c < chs; c++) {
            float v = c < (uint32_t)f->ch_layout.nb_channels ? sample_at(f, (int)c, (int)(first + i)) : 0.0f;
            if (in->encoding == ENC_FLOAT) {
                memcpy(out, &v, 4);
                out += 4;
                continue;
            }
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            if (in->encoding == ENC_S16) {
                int16_t s = (int16_t)(v * 32767.0f);
                memcpy(out, &s, 2);
                out += 2;
            } else {
                int32_t s = (int32_t)((double)v * 2147483647.0);
                memcpy(out, &s, 4);
                out += 4;
            }
        }
    }
}

static bool emit_resampled(ajm_instance *in, const uint8_t *pcm, uint32_t count,
                           uint8_t *out, uint64_t out_size, job_result *r) {
    if (!ajm_resample_push(&in->resampler, pcm, count, in->channels, in->encoding)) {
        r->result = RES_CODEC_ERROR | RES_FATAL;
        return false;
    }
    r->written += ajm_resample_produce(&in->resampler, out ? out + r->written : NULL,
                                       out_size - r->written, in->encoding);
    if (ajm_resample_ready(&in->resampler)) r->result |= RES_NOT_ENOUGH_ROOM;
    return true;
}

/* One packet through the decoder into out (at *written, room up to out_size):
 * false with r->result set on errors */
static bool decode_packet(ajm_instance *in, const uint8_t *data, uint32_t size, uint8_t *out, uint64_t out_size,
                          job_result *r) {
    if (av_new_packet(in->pkt, (int)size) < 0) {
        r->result = RES_CODEC_ERROR | RES_FATAL;
        return false;
    }
    memcpy(in->pkt->data, data, size);
    int ret = avcodec_send_packet(in->ctx, in->pkt);
    av_packet_unref(in->pkt);
    if (ret < 0) {
        r->result = RES_CODEC_ERROR | RES_INVALID_DATA;
        r->internal = ret;
        return false;
    }
    while ((ret = avcodec_receive_frame(in->ctx, in->frame)) == 0) {
        AVFrame *f = in->frame;
        if (!in->channels) {
            in->channels = (uint32_t)f->ch_layout.nb_channels;
            if (in->max_channels && in->channels > in->max_channels) in->channels = in->max_channels;
        }
        if (f->sample_rate > 0) in->rate = (uint32_t)f->sample_rate;
        uint32_t skip, keep;
        gapless_take(in, (uint32_t)f->nb_samples, &skip, &keep);
        uint64_t bytes = (uint64_t)keep * in->channels * bytes_per_sample(in->encoding);
        if (ajm_resample_active(&in->resampler)) {
            uint8_t *pcm = bytes ? malloc((size_t)bytes) : NULL;
            if (bytes && !pcm) { r->result = RES_CODEC_ERROR | RES_FATAL; av_frame_unref(f); return false; }
            if (keep) write_pcm(in, f, skip, keep, pcm);
            bool ok = emit_resampled(in, pcm, keep, out, out_size, r);
            free(pcm);
            in->total_samples += keep;
            av_frame_unref(f);
            if (!ok) return false;
            continue;
        }
        if (r->written + bytes > out_size) {
            r->result |= RES_NOT_ENOUGH_ROOM;
            keep = (uint32_t)((out_size - r->written) / ((uint64_t)in->channels * bytes_per_sample(in->encoding)));
            bytes = (uint64_t)keep * in->channels * bytes_per_sample(in->encoding);
        }
        if (keep && out) write_pcm(in, f, skip, keep, out + r->written);
        r->written += bytes;
        in->total_samples += keep;
        av_frame_unref(f);
    }
    r->frames++;
    return true;
}

/* ---- Per-codec framing ---- */

static const uint32_t k_at9_rates[16] = {11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
                                         44100, 48000, 64000, 88200, 96000, 128000, 176400, 192000};
static const uint8_t k_at9_frame_log2[16] = {6, 6, 7, 7, 7, 8, 8, 8, 6, 6, 7, 7, 7, 8, 8, 8};
static const uint8_t k_at9_channels[8] = {1, 2, 2, 6, 8, 4, 1, 2}; /* 6 and 7: vibration layouts */

static int32_t at9_configure(ajm_instance *in, const uint8_t *config) {
    uint8_t cfg[4];
    memcpy(cfg, config, 4);
    if (cfg[0] != 0xFE) return RES_CODEC_ERROR | RES_INVALID_DATA;
    uint32_t bits = (uint32_t)cfg[1] << 16 | (uint32_t)cfg[2] << 8 | cfg[3];
    uint32_t rate_idx = bits >> 20, layout = (bits >> 17) & 7, frame_bytes = ((bits >> 5) & 0x7FF) + 1;
    uint32_t superframe_idx = (bits >> 3) & 3;
    if (layout >= 6) cfg[1] = (uint8_t)(cfg[1] - (6u << 1)); /* vibration: decoded as mono / stereo */
    uint8_t extra[12] = {2, 0, 0, 0, cfg[0], cfg[1], cfg[2], cfg[3], 0, 0, 0, 0};
    memcpy(in->at9_config, config, 4);
    in->at9_frame_bytes = frame_bytes;
    in->at9_frames_per_superframe = 1u << superframe_idx;
    in->at9_frame_samples = 1u << k_at9_frame_log2[rate_idx];
    in->at9_frame_in_superframe = 0;
    in->rate = k_at9_rates[rate_idx];
    in->channels = k_at9_channels[layout];
    if (in->max_channels && in->channels > in->max_channels) in->channels = in->max_channels;
    /* packets of whole superframes (see at9_decode) */
    if (!codec_open(in, AV_CODEC_ID_ATRAC9, extra, 12, (int)(frame_bytes * in->at9_frames_per_superframe), 0, 0))
        return RES_CODEC_ERROR | RES_FATAL;
    in->at9_sf = (uint8_t *)malloc((size_t)frame_bytes * in->at9_frames_per_superframe);
    in->at9_cache = (uint8_t *)malloc((size_t)AT9_CACHE_FRAMES * in->at9_frame_samples * in->channels * 4);
    if (!in->at9_sf || !in->at9_cache) {
        codec_close(in);
        return RES_CODEC_ERROR | RES_FATAL;
    }
    in->initialized = true;
    return 0;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* A RIFF/WAVE AT9 header in front of the data: its format configures the
 * decoder, its fact chunk the gapless range. Bytes of header, 0 if partial. */
static uint64_t at9_riff_header(ajm_instance *in, const uint8_t *p, uint64_t size, job_result *r) {
    static const uint8_t guid[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
                                     0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};
    if (size < 12 || memcmp(p + 8, "WAVE", 4)) {
        r->result = RES_INVALID_DATA;
        return 0;
    }
    for (uint64_t off = 12; off + 8 <= size;) {
        uint32_t len = le32(p + off + 4);
        uint64_t payload = off + 8;
        if (!memcmp(p + off, "data", 4)) return payload;
        if (payload + len > size) break;
        if (!memcmp(p + off, "fmt ", 4) && len >= 48 && !memcmp(p + payload + 24, guid, 16)) {
            int32_t e = at9_configure(in, p + payload + 44);
            if (e) {
                r->result = e;
                return 0;
            }
        } else if (!memcmp(p + off, "fact", 4) && len >= 12) {
            in->g_init.total = le32(p + payload);
            uint32_t skip = le32(p + payload + 4);
            in->g_init.skip = (uint16_t)(skip > 0xFFFF ? 0xFFFF : skip);
            in->has_limit = in->g_init.total != 0 && in->g_init.total != 0xFFFFFFFFu;
            gapless_reset(in);
        }
        off = payload + len + (len & 1);
    }
    r->result = RES_PARTIAL_INPUT;
    return 0;
}

/* MP3 frame length from its header, 0 if it is not one */
static uint32_t mp3_frame_bytes(const uint8_t *p, uint64_t size) {
    static const uint16_t kbps1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const uint16_t kbps2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const uint32_t rates[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};
    if (size < 4) return 0;
    uint32_t h = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    uint32_t ver = (h >> 19) & 3, layer = (h >> 17) & 3, br = (h >> 12) & 15, sr = (h >> 10) & 3, pad = (h >> 9) & 1;
    if ((h & 0xFFE00000u) != 0xFFE00000u || ver == 1 || layer != 1 || br == 0 || br == 15 || sr == 3) return 0;
    uint32_t rate = rates[ver][sr], kbps = ver == 3 ? kbps1[br] : kbps2[br];
    return (ver == 3 ? 144000u : 72000u) * kbps / rate + pad;
}

/* The next packet of the codec's bitstream at p: its bytes to feed and to consume */
static bool next_packet(ajm_instance *in, const uint8_t *p, uint64_t size, uint32_t *feed_off, uint32_t *feed,
                        uint32_t *consume, job_result *r) {
    *feed_off = 0;
    switch (in->codec) {
    case CODEC_AT9:
        if (size < in->at9_frame_bytes) {
            r->result = RES_PARTIAL_INPUT;
            return false;
        }
        *feed = *consume = in->at9_frame_bytes;
        return true;
    case CODEC_MP3: {
        uint32_t n = mp3_frame_bytes(p, size);
        if (!n) {
            r->result = size < 4 ? RES_PARTIAL_INPUT : RES_INVALID_DATA;
            return false;
        }
        if (n > size) {
            r->result = RES_PARTIAL_INPUT;
            return false;
        }
        in->mp3_header = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        *feed = *consume = n;
        return true;
    }
    case CODEC_AAC:
        if (in->aac_config == 2) { /* raw: the whole input is one access unit */
            *feed = *consume = (uint32_t)size;
            return size > 0;
        }
        if (size < 7 || p[0] != 0xFF || (p[1] & 0xF6) != 0xF0) {
            r->result = size < 7 ? RES_PARTIAL_INPUT : RES_INVALID_DATA;
            return false;
        }
        *feed = ((uint32_t)(p[3] & 3) << 11) | ((uint32_t)p[4] << 3) | (p[5] >> 5);
        if (*feed < 7 || *feed > size) {
            r->result = RES_PARTIAL_INPUT;
            return false;
        }
        *consume = *feed;
        return true;
    case CODEC_OPUS: { /* each packet behind its 16-bit little-endian length */
        if (size < 2) {
            r->result = RES_PARTIAL_INPUT;
            return false;
        }
        uint32_t n = (uint32_t)p[0] | (uint32_t)p[1] << 8;
        if (!n || n + 2 > size) {
            r->result = n ? RES_PARTIAL_INPUT : (RES_CODEC_ERROR | RES_INVALID_DATA);
            return false;
        }
        uint32_t frames = (p[2] & 3) == 0 ? 1 : (p[2] & 3) == 3 ? (n > 1 ? p[3] & 0x3F : 0) : 2;
        in->opus_frames_per_packet = frames;
        *feed_off = 2;
        *feed = n;
        *consume = n + 2;
        return true;
    }
    default: r->result = RES_INVALID_PARAMETER; return false;
    }
}

/* ---- ATRAC9: frames depend on the earlier ones of their superframe, so the
 * decoder takes whole superframes while jobs still go frame by frame. A
 * superframe wholly in the input is decoded at its first frame (no delay);
 * otherwise its frames are gathered as they come and its samples come out
 * once it is complete. Decoded frames wait in a FIFO. */

static uint32_t at9_frame_pcm_bytes(const ajm_instance *in) {
    return in->at9_frame_samples * in->channels * bytes_per_sample(in->encoding);
}

static bool at9_decode_superframe(ajm_instance *in, const uint8_t *sf, job_result *r) {
    uint32_t sf_size = in->at9_frame_bytes * in->at9_frames_per_superframe;
    if (av_new_packet(in->pkt, (int)sf_size) < 0) {
        r->result = RES_CODEC_ERROR | RES_FATAL;
        return false;
    }
    memcpy(in->pkt->data, sf, sf_size);
    int ret = avcodec_send_packet(in->ctx, in->pkt);
    av_packet_unref(in->pkt);
    if (ret >= 0) ret = avcodec_receive_frame(in->ctx, in->frame);
    if (ret == AVERROR_INVALIDDATA) {
        /* FFmpeg's ATRAC9 decoder refuses some superframes the console plays (R-Type Delta HD:
         * SELECT_00001.wav, frame 34, bytes as in the bank); the game takes a codec error as
         * fatal. Its samples become silence and the decoder starts over at the next one. */
        if (getenv("PS5C_AJM_TRACE"))
            LOG_I(HLE, "ajm 0x%x: refused superframe %u after %llu samples", in->id, in->at9_rejected, (unsigned long long)in->total_samples);
        if (!in->at9_rejected++)
            LOG_W(HLE, "ajm 0x%x: FFmpeg refused an ATRAC9 superframe; its samples are silence", in->id);
        avcodec_flush_buffers(in->ctx);
        uint32_t bytes = at9_frame_pcm_bytes(in);
        for (uint32_t k = 0; k < in->at9_frames_per_superframe && in->at9_cache_count < AT9_CACHE_FRAMES; k++) {
            uint32_t slot = (in->at9_cache_head + in->at9_cache_count) % AT9_CACHE_FRAMES;
            memset(in->at9_cache + (size_t)slot * bytes, 0, bytes);
            in->at9_cache_count++;
        }
        return true;
    }
    if (ret < 0) {
        r->result = RES_CODEC_ERROR | RES_INVALID_DATA;
        r->internal = ret;
        return false;
    }
    AVFrame *f = in->frame;
    uint32_t fs = in->at9_frame_samples, bytes = at9_frame_pcm_bytes(in);
    for (uint32_t k = 0; k < in->at9_frames_per_superframe && (k + 1) * fs <= (uint32_t)f->nb_samples; k++) {
        if (in->at9_cache_count == AT9_CACHE_FRAMES) break;
        uint32_t slot = (in->at9_cache_head + in->at9_cache_count) % AT9_CACHE_FRAMES;
        write_pcm(in, f, k * fs, fs, in->at9_cache + (size_t)slot * bytes);
        in->at9_cache_count++;
    }
    av_frame_unref(f);
    return true;
}

static void at9_decode(ajm_instance *in, const uint8_t *data, uint64_t size, uint64_t off, uint8_t *out,
                       uint64_t out_size, bool multiple, job_result *r) {
    uint32_t fb = in->at9_frame_bytes, sf_size = fb * in->at9_frames_per_superframe;
    uint32_t fs = in->at9_frame_samples, frame_pcm = at9_frame_pcm_bytes(in);
    uint32_t sample_bytes = in->channels * bytes_per_sample(in->encoding);
    while (!gapless_end(in)) {
        if (size - off < fb) {
            if (!r->frames) r->result = RES_PARTIAL_INPUT;
            break;
        }
        /* room for this frame's samples (after the gapless trim) */
        uint32_t skip = in->g_cur.skip < fs ? in->g_cur.skip : fs, keep = fs - skip;
        if (in->has_limit && keep > in->g_cur.total) keep = in->g_cur.total;
        bool resampling = ajm_resample_active(&in->resampler);
        if (r->written + (resampling ? sample_bytes : (uint64_t)keep * sample_bytes) > out_size) {
            r->result |= RES_NOT_ENOUGH_ROOM;
            break;
        }
        if (in->at9_frame_in_superframe == 0)
            in->at9_direct = size - off >= sf_size && !in->at9_cache_count && !in->at9_sf_have &&
                             at9_decode_superframe(in, data + off, r);
        if (r->result) break;
        if (!in->at9_direct) {
            memcpy(in->at9_sf + in->at9_sf_have, data + off, fb);
            in->at9_sf_have += fb;
            if (in->at9_sf_have == sf_size) {
                in->at9_sf_have = 0;
                if (!at9_decode_superframe(in, in->at9_sf, r)) break;
            }
        }
        off += fb;
        r->consumed = off;
        r->frames++;
        in->at9_frame_in_superframe = (in->at9_frame_in_superframe + 1) % in->at9_frames_per_superframe;
        if (in->at9_cache_count) {
            const uint8_t *pcm = in->at9_cache + (size_t)in->at9_cache_head * frame_pcm;
            gapless_take(in, fs, &skip, &keep);
            if (resampling) {
                if (!emit_resampled(in, pcm + (size_t)skip * sample_bytes, keep, out, out_size, r)) return;
            } else {
                if (keep && out) memcpy(out + r->written, pcm + (size_t)skip * sample_bytes, (size_t)keep * sample_bytes);
                r->written += (uint64_t)keep * sample_bytes;
            }
            in->total_samples += keep;
            in->at9_cache_head = (in->at9_cache_head + 1) % AT9_CACHE_FRAMES;
            in->at9_cache_count--;
        }
        if (r->result) break;
        if (!multiple) break;
    }
}

static void decode(ajm_instance *in, const uint8_t *data, uint64_t size, uint8_t *out, uint64_t out_size,
                   bool multiple, job_result *r) {
    if (in->initialized && ajm_resample_active(&in->resampler)) {
        r->written = ajm_resample_produce(&in->resampler, out, out_size, in->encoding);
        if (ajm_resample_ready(&in->resampler) ||
            (size && out_size - r->written < in->channels * bytes_per_sample(in->encoding))) {
            r->result = RES_NOT_ENOUGH_ROOM;
            return;
        }
    }
    if (!data || !size) {
        if (!r->written) r->result = RES_PARTIAL_INPUT;
        return;
    }
    uint64_t off = 0;
    if (in->codec == CODEC_AT9 && (in->flags & AT9_PARSE_RIFF_HEADER) && size >= 12 && !memcmp(data, "RIFF", 4)) {
        off = at9_riff_header(in, data, size, r);
        r->consumed = off;
        if (!off || off >= size) return;
    }
    if (!in->initialized || !in->ctx) {
        r->result = RES_NOT_INITIALIZED;
        return;
    }
    if (in->codec == CODEC_AT9) {
        at9_decode(in, data, size, off, out, out_size, multiple, r);
        if ((r->result & ~(RES_PARTIAL_INPUT | RES_NOT_ENOUGH_ROOM)) && getenv("PS5C_AJM_TRACE"))
            LOG_I(HLE, "ajm 0x%x: AT9 decode 0x%x (internal %d): size %llu consumed %llu written %llu frames %u, total %llu, sf have %u, frame %u/%u, gapless total %u skip %u",
                  in->id, r->result, r->internal, (unsigned long long)size, (unsigned long long)r->consumed,
                  (unsigned long long)r->written, r->frames, (unsigned long long)in->total_samples, in->at9_sf_have,
                  in->at9_frame_in_superframe, in->at9_frames_per_superframe, in->g_cur.total, in->g_cur.skip);
        return;
    }
    while (off < size && !gapless_end(in)) {
        if (ajm_resample_active(&in->resampler) &&
            out_size - r->written < in->channels * bytes_per_sample(in->encoding)) {
            r->result |= RES_NOT_ENOUGH_ROOM;
            break;
        }
        uint32_t feed_off, feed, consume;
        if (!next_packet(in, data + off, size - off, &feed_off, &feed, &consume, r)) {
            if (r->frames && r->result == RES_PARTIAL_INPUT) r->result = 0; /* what fits was decoded */
            break;
        }
        if (!decode_packet(in, data + off + feed_off, feed, out, out_size, r)) break;
        off += consume;
        r->consumed = off;
        if (r->result || !multiple) break;
    }
}

static int32_t initialize(ajm_instance *in, const uint8_t *params, uint64_t size) {
    in->g_init = (gapless_state){0, 0, 0};
    in->has_limit = false;
    gapless_reset(in);
    in->total_samples = 0;
    in->initialized = false;
    switch (in->codec) {
    case CODEC_AT9:
        if (!params || size < 4) return RES_INVALID_PARAMETER;
        return at9_configure(in, params);
    case CODEC_AAC: {
        if (!params || size < 8) return RES_INVALID_PARAMETER;
        in->aac_config = le32(params);
        in->aac_freq_index = le32(params + 4);
        if (in->aac_config < 1 || in->aac_config > 3 || (in->aac_config == 2 && in->aac_freq_index > 11))
            return RES_INVALID_PARAMETER;
        uint32_t ch = in->max_channels ? in->max_channels : 2;
        /* raw access units need the AudioSpecificConfig: AAC LC, rate index, channels */
        uint16_t asc = (uint16_t)(2u << 11 | in->aac_freq_index << 7 | (ch > 7 ? 7 : ch) << 3);
        uint8_t extra[2] = {(uint8_t)(asc >> 8), (uint8_t)asc};
        in->channels = 0;
        if (!codec_open(in, AV_CODEC_ID_AAC, extra, in->aac_config == 2 ? 2 : 0, 0, 0, 0)) return RES_CODEC_ERROR | RES_FATAL;
        break;
    }
    case CODEC_MP3:
        in->channels = 0;
        if (!codec_open(in, AV_CODEC_ID_MP3, NULL, 0, 0, 0, 0)) return RES_CODEC_ERROR | RES_FATAL;
        break;
    case CODEC_OPUS: {
        if (!params || size < 8) return RES_INVALID_PARAMETER;
        uint32_t ch = le32(params), rate = le32(params + 4);
        if (!ch || ch > 2 || rate != 48000) return RES_INVALID_PARAMETER;
        in->channels = ch;
        in->rate = rate;
        if (!codec_open(in, AV_CODEC_ID_OPUS, NULL, 0, 0, (int)ch, (int)rate)) return RES_CODEC_ERROR | RES_FATAL;
        break;
    }
    default: return RES_INVALID_PARAMETER;
    }
    in->initialized = true;
    return 0;
}

static void clear_context(ajm_instance *in) {
    ajm_resample_clear(&in->resampler);
    if (in->ctx) avcodec_flush_buffers(in->ctx);
    in->total_samples = 0;
    in->at9_frame_in_superframe = in->at9_sf_have = in->at9_cache_count = 0;
    gapless_reset(in);
}

#else /* no FFmpeg: every job reports a codec error */

static void codec_close(ajm_instance *in) { ajm_resample_free(&in->resampler); }
static void decode(ajm_instance *in, const uint8_t *data, uint64_t size, uint8_t *out, uint64_t out_size,
                   bool multiple, job_result *r) {
    (void)in, (void)data, (void)size, (void)out, (void)out_size, (void)multiple;
    r->result = RES_CODEC_ERROR | RES_FATAL;
}
static int32_t initialize(ajm_instance *in, const uint8_t *params, uint64_t size) {
    (void)in, (void)params, (void)size;
    return RES_CODEC_ERROR | RES_FATAL;
}
static void clear_context(ajm_instance *in) { gapless_reset(in); ajm_resample_clear(&in->resampler); }

#endif

static bool codec_supported(uint32_t codec) {
#ifdef PS5C_HAVE_FFMPEG
    return codec == CODEC_MP3 || codec == CODEC_AT9 || codec == CODEC_AAC || codec == CODEC_OPUS;
#else
    (void)codec;
    return false;
#endif
}

/* ---- Context, modules, instances ---- */

/* int sceAjmInitialize(int64_t reserved, SceAjmContextId *out) */
static void ajm_initialize(hle_call *c) {
    uint32_t ctx = 1;
    gmem_write(c->mem, hle_arg_ptr(c, 1), &ctx, 4);
    hle_ret_s32(c, 0);
}

static void ajm_ok(hle_call *c) { hle_ret_s32(c, 0); }

/* int sceAjmDecMp3ParseFrame(const u8 *stream, u32 size, int parseOfl, frame *):
 * frame { u64 frameSize; u32 channels, samplesPerChannel, bitrate, sampleRate, encoderDelay,
 * numFrames, totalSamples, oflType }. With parseOfl, a Xing/Info tag gives the frame count and
 * its LAME extension the encoder delay and total samples; a VBRI tag the delay. Layout and
 * rules as in the AnyPS5 project's Ajm (its MP3 header parser). */
static void ajm_mp3_parse_frame(hle_call *c) {
    uint32_t size = hle_arg_u32(c, 1);
    int parse_ofl = hle_arg_s32(c, 2);
    const uint8_t *s = hle_arg_ptr(c, 0) && size >= 4 ? (const uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), size) : NULL;
    uint8_t *f = hle_arg_ptr(c, 3) ? (uint8_t *)hle_mem(c, hle_arg_ptr(c, 3), 40) : NULL;
    if (!s || !f) { hle_ret_s32(c, AJM_ERR_INVALID_PARAMETER); return; }
    static const uint32_t rates[4][4] = {{11025, 12000, 8000, 0}, {0}, {22050, 24000, 16000, 0}, {44100, 48000, 32000, 0}};
    static const uint16_t kbps[4][16] = {{0, 8, 16, 24, 32, 40, 48, 56, 64},
                                         {0},
                                         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
                                         {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}};
    uint32_t h = (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | s[3];
    uint32_t version = (h >> 19) & 3, layer = (h >> 17) & 3;
    uint32_t rate = rates[version][(h >> 10) & 3], bitrate = kbps[version][(h >> 12) & 15] * 1000u;
    if ((h >> 21) != 0x7FF || !layer || !rate || !bitrate || (parse_ofl && layer != 1)) {
        hle_ret_s32(c, AJM_ERR_INVALID_PARAMETER);
        return;
    }
    bool mpeg1 = version == 3, mono = ((h >> 6) & 3) == 3;
    uint64_t frame_size = (mpeg1 ? 144u : 72u) * bitrate / rate + ((h >> 9) & 1);
    uint32_t v[8] = {mono ? 1u : 2u, mpeg1 ? 1152u : 576u, bitrate, rate, 0, 0, 0, 0};
    if (parse_ofl) {
        size_t end = size < frame_size ? size : (size_t)frame_size;
        size_t at = mpeg1 ? (mono ? 21 : 36) : (mono ? 13 : 21);
        if (at + 8 <= end && (!memcmp(s + at, "Xing", 4) || !memcmp(s + at, "Info", 4))) {
            uint8_t flags = s[at + 7];
            size_t field = at + 8;
            uint32_t frames = 0;
            if ((flags & 1) && field + 4 <= end) {
                frames = (uint32_t)s[field] << 24 | (uint32_t)s[field + 1] << 16 | (uint32_t)s[field + 2] << 8 | s[field + 3];
                v[5] = frames;
            }
            field += (flags & 1 ? 4 : 0) + (flags & 2 ? 4 : 0) + (flags & 4 ? 100 : 0) + (flags & 8 ? 4 : 0);
            if (field + 0x18 <= end && !memcmp(s + field, "LAME", 4)) {
                uint32_t delay = (uint32_t)s[field + 0x15] << 4 | s[field + 0x16] >> 4;
                uint32_t padding = ((uint32_t)s[field + 0x16] & 15) << 8 | s[field + 0x17];
                if ((flags & 1) && frames) {
                    uint64_t samples = (uint64_t)frames * v[1], trim = (uint64_t)delay + padding;
                    v[6] = samples > trim ? (uint32_t)PS5C_MIN(samples - trim, UINT32_MAX) : 0;
                }
                v[4] = v[1] + delay + 529;
                v[7] = 1; /* LAME */
            }
        } else if (end >= 62 && !memcmp(s + 36, "VBRI", 4)) {
            v[4] = (uint32_t)s[42] << 8 | s[43];
            v[7] = 2; /* VBRI */
        }
    }
    memcpy(f, &frame_size, 8);
    memcpy(f + 8, v, 32);
    hle_ret_s32(c, 0);
}

/* int sceAjmModuleRegister(ctx, codec, int64_t reserved) */
static void ajm_module_register(hle_call *c) {
    uint32_t codec = hle_arg_u32(c, 1);
    /* 14: a decoder of the newer ABI the console registers (KytyPS5 lists it as supported);
     * Wolverine's sound engine (Wwise) gives up on all sound when it is refused. Instances of
     * it are still refused: no decoder is known for it. */
    bool ok = codec_supported(codec) || codec == CODEC_NEW_ABI;
    LOG_I(HLE, "sceAjmModuleRegister(codec %u)%s", codec, ok ? "" : ": not supported");
    hle_ret_s32(c, ok ? 0 : AJM_ERR_CODEC_NOT_SUPPORTED);
}

/* int sceAjmInstanceCreate(ctx, codec, uint64_t flags, SceAjmInstanceId *out) */
static void ajm_instance_create(hle_call *c) {
    uint32_t codec = hle_arg_u32(c, 1);
    uint64_t flags = hle_arg(c, 2);
    guest_addr out = hle_arg_ptr(c, 3);
    if (!out) {
        hle_ret_s32(c, AJM_ERR_INVALID_PARAMETER);
        return;
    }
    if (!codec_supported(codec)) {
        hle_ret_s32(c, AJM_ERR_CODEC_NOT_SUPPORTED);
        return;
    }
    pal_mutex_lock(g_ajm.lock);
    ajm_instance *in = NULL;
    for (uint32_t k = 0; k < MAX_INSTANCES && !in; k++) {
        uint32_t i = (g_ajm.next_slot + k) % MAX_INSTANCES;
        if (!g_ajm.inst[i].used) in = &g_ajm.inst[i];
    }
    if (in) {
        memset(in, 0, sizeof(*in));
        in->used = true;
        uint32_t slot = (uint32_t)(in - g_ajm.inst);
        g_ajm.next_slot = slot + 1;
        in->id = codec << 14 | (slot + 1);
        in->codec = codec;
        in->flags = flags;
        in->max_channels = (uint32_t)(flags >> 3) & 0xF;
        in->encoding = (uint32_t)(flags >> 7) & 7;
        if (in->encoding > ENC_FLOAT) in->encoding = ENC_S16;
        in->rate = 48000;
        in->resampler.ratio = 1.0f;
        ajm_resample_clear(&in->resampler);
        /* MP3 streams describe themselves: its decoder opens right away */
        if (codec == CODEC_MP3) initialize(in, NULL, 0);
        gmem_write(c->mem, out, &in->id, 4);
    }
    pal_mutex_unlock(g_ajm.lock);
    LOG_D(HLE, "sceAjmInstanceCreate(codec %u, flags 0x%llx) -> 0x%x", codec, (unsigned long long)flags, in ? in->id : 0);
    hle_ret_s32(c, in ? 0 : AJM_ERR_JOB_CREATION);
}

static void ajm_instance_destroy(hle_call *c) {
    pal_mutex_lock(g_ajm.lock);
    ajm_instance *in = find(hle_arg_u32(c, 1));
    if (in) {
        codec_close(in);
        memset(in, 0, sizeof(*in));
    }
    pal_mutex_unlock(g_ajm.lock);
    hle_ret_s32(c, in ? 0 : AJM_ERR_INVALID_INSTANCE);
}

/* ---- Batches ---- */

/* SceAjmBatchInfo: buffer, offset, size, last good job, its return address (u64 each) */
static void ajm_batch_initialize(hle_call *c) {
    guest_addr buf = hle_arg_ptr(c, 0), info = hle_arg_ptr(c, 2);
    if (!buf || !info) {
        hle_ret_s32(c, AJM_ERR_INVALID_PARAMETER);
        return;
    }
    uint64_t v[5] = {buf, 0, hle_arg(c, 1), 0, 0};
    gmem_write(c->mem, info, v, sizeof(v));
    hle_ret_s32(c, 0);
}

/* Takes a job's room in the batch: 0 or AJM_ERR_JOB_CREATION when full */
static int32_t reserve_job(hle_call *c, guest_addr info, uint64_t size) {
    uint64_t v[5];
    if (!info || !gmem_read(c->mem, info, v, sizeof(v))) return AJM_ERR_INVALID_PARAMETER;
    if (size > v[2] || v[1] > v[2] - size) return AJM_ERR_JOB_CREATION;
    v[3] = v[0] + v[1];
    v[4] = 0;
    v[1] += size;
    gmem_write(c->mem, info, v, sizeof(v));
    return 0;
}

static void write_result(hle_call *c, guest_addr at, const job_result *r, bool stream, bool mframe,
                         const ajm_instance *in) {
    if (!at) return;
    uint8_t buf[32];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &r->result, 4);
    memcpy(buf + 4, &r->internal, 4);
    size_t n = 8;
    if (stream) {
        int32_t consumed = (int32_t)(r->consumed > 0x7FFFFFFF ? 0x7FFFFFFF : r->consumed);
        int32_t written = (int32_t)(r->written > 0x7FFFFFFF ? 0x7FFFFFFF : r->written);
        uint64_t total = in ? in->total_samples : 0;
        memcpy(buf + 8, &consumed, 4);
        memcpy(buf + 12, &written, 4);
        memcpy(buf + 16, &total, 8);
        n = 24;
    }
    if (mframe) {
        memcpy(buf + 24, &r->frames, 4);
        n = 32;
    }
    gmem_write(c->mem, at, buf, n);
}

/* int sceAjmBatchJobInitialize(info, instance, const void *params, size_t size, result *) */
static void ajm_job_initialize(hle_call *c) {
    job_result r = {0};
    pal_mutex_lock(g_ajm.lock);
    ajm_instance *in = find(hle_arg_u32(c, 1));
    uint64_t size = hle_arg(c, 3);
    const uint8_t *params = size ? (const uint8_t *)gmem_host(c->mem, hle_arg_ptr(c, 2), size) : NULL;
    r.result = in ? initialize(in, params, size) : RES_INVALID_PARAMETER;
    if (in)
        LOG_D(HLE, "ajm: instance 0x%x initialized: %u ch %u Hz%s", in->id, in->channels, in->rate,
              r.result ? " (failed)" : "");
    pal_mutex_unlock(g_ajm.lock);
    write_result(c, hle_arg_ptr(c, 4), &r, false, false, NULL);
    hle_ret_s32(c, reserve_job(c, hle_arg_ptr(c, 0), JOB_CONTROL_SIZE));
}

/* int sceAjmBatchJobControl(info, instance, uint64_t flags, const void *in, size_t in_size, void *out, size_t out_size):
 * the generic control job the specific ones are made of. flags: bits 13-15 reset, initialize, resample;
 * bits 45-47 sideband gapless, format, stream. The input holds (gapless flag) the gapless state to set
 * {u32 total, u16 skip, u16}, then the codec's initialize parameters (R-Type Delta HD: 25 0c 00 00 40 00 00 00
 * fe 10 05 e0 00 00 00 00); the output gets the result, then the gapless state. */
static void control_job(hle_call *c) {
    job_result r = {0};
    uint64_t flags = hle_arg(c, 2), in_size = hle_arg(c, 4), out_size = hle_arg(c, 6);
    bool reset = flags >> 13 & 1, init = flags >> 14 & 1, gapless = flags >> 45 & 1;
    const uint8_t *params = in_size ? (const uint8_t *)gmem_host(c->mem, hle_arg_ptr(c, 3), in_size) : NULL;
    uint8_t buf[16];
    memset(buf, 0, sizeof(buf));
    pal_mutex_lock(g_ajm.lock);
    ajm_instance *in = find(hle_arg_u32(c, 1));
    if (!in || (in_size && !params)) {
        r.result = RES_INVALID_PARAMETER;
    } else {
        uint64_t skip = gapless && in_size >= 8 ? 8 : 0;
        if (reset) clear_context(in);
        if (init) r.result = initialize(in, params ? params + skip : NULL, in_size - skip);
        if (skip && !r.result) {
            const uint8_t *g = params;
            memcpy(&in->g_init.total, g, 4);
            memcpy(&in->g_init.skip, g + 4, 2);
            in->has_limit = in->g_init.total != 0 && in->g_init.total != 0xFFFFFFFFu;
            gapless_reset(in);
        }
        memcpy(buf + 8, &in->g_cur.total, 4);
        memcpy(buf + 12, &in->g_cur.skip, 2);
        memcpy(buf + 14, &in->g_cur.skipped, 2);
    }
    LOG_D(HLE, "sceAjmBatchJobControl(0x%x, flags 0x%llx, in %llu, out %llu) -> 0x%x", hle_arg_u32(c, 1),
          (unsigned long long)flags, (unsigned long long)in_size, (unsigned long long)out_size, r.result);
    pal_mutex_unlock(g_ajm.lock);
    memcpy(buf, &r.result, 4);
    memcpy(buf + 4, &r.internal, 4);
    if (hle_arg_ptr(c, 5) && out_size)
        gmem_write(c->mem, hle_arg_ptr(c, 5), buf, out_size < sizeof(buf) ? out_size : sizeof(buf));
}

static void ajm_job_control(hle_call *c) {
    control_job(c);
    hle_ret_s32(c, reserve_job(c, hle_arg_ptr(c, 0), JOB_CONTROL_SIZE));
}

/* int sceAjmBatchJobClearContext(info, instance, result *) */
static void ajm_job_clear_context(hle_call *c) {
    job_result r = {0};
    pal_mutex_lock(g_ajm.lock);
    ajm_instance *in = find(hle_arg_u32(c, 1));
    if (in) clear_context(in);
    else r.result = RES_INVALID_PARAMETER;
    pal_mutex_unlock(g_ajm.lock);
    write_result(c, hle_arg_ptr(c, 2), &r, false, false, NULL);
    hle_ret_s32(c, reserve_job(c, hle_arg_ptr(c, 0), JOB_CONTROL_SIZE));
}

static void run_decode(hle_call *c, uint32_t id, guest_addr in_addr, uint64_t in_size, guest_addr out_addr,
                       uint64_t out_size, bool multiple, job_result *r) {
    const uint8_t *src = in_size ? (const uint8_t *)gmem_host(c->mem, in_addr, in_size) : NULL;
    uint8_t *dst = out_size ? (uint8_t *)gmem_host(c->mem, out_addr, out_size) : NULL;
    ajm_instance *in = find(id);
    if (!in) r->result = RES_INVALID_PARAMETER;
    else if (in_size && !src) r->result = RES_INVALID_PARAMETER;
    else decode(in, src, in_size, dst, dst ? out_size : 0, multiple, r);
}

/* int sceAjmBatchJobDecode(info, instance, in, in_size, out, out_size, result *):
 * as many frames as fit; the result has result, stream and frame count */
static void ajm_job_decode(hle_call *c) {
    job_result r = {0};
    pal_mutex_lock(g_ajm.lock);
    uint32_t id = hle_arg_u32(c, 1);
    run_decode(c, id, hle_arg_ptr(c, 2), hle_arg(c, 3), hle_arg_ptr(c, 4), hle_arg(c, 5), true, &r);
    write_result(c, hle_arg_ptr(c, 6), &r, true, true, find(id));
    pal_mutex_unlock(g_ajm.lock);
    hle_ret_s32(c, reserve_job(c, hle_arg_ptr(c, 0), JOB_RUN_SIZE));
}

/* int sceAjmBatchJobDecodeSingle(...): one frame; result and stream */
static void ajm_job_decode_single(hle_call *c) {
    job_result r = {0};
    pal_mutex_lock(g_ajm.lock);
    uint32_t id = hle_arg_u32(c, 1);
    run_decode(c, id, hle_arg_ptr(c, 2), hle_arg(c, 3), hle_arg_ptr(c, 4), hle_arg(c, 5), false, &r);
    write_result(c, hle_arg_ptr(c, 6), &r, true, false, find(id));
    pal_mutex_unlock(g_ajm.lock);
    hle_ret_s32(c, reserve_job(c, hle_arg_ptr(c, 0), JOB_RUN_SIZE));
}

/* int sceAjmBatchJobDecodeSplit(info, instance, SceAjmBuffer *in, size_t n_in,
 * SceAjmBuffer *out, size_t n_out, result *): scattered buffers {addr, size} */
static void ajm_job_decode_split(hle_call *c) {
    job_result r = {0};
    guest_addr in_list = hle_arg_ptr(c, 2), out_list = hle_arg_ptr(c, 4);
    uint64_t n_in = hle_arg(c, 3), n_out = hle_arg(c, 5);
    uint64_t in_total = 0, out_total = 0, b[2];
    for (uint64_t i = 0; i < n_in && i < 64 && gmem_read(c->mem, in_list + i * 16, b, 16); i++) in_total += b[1];
    for (uint64_t i = 0; i < n_out && i < 64 && gmem_read(c->mem, out_list + i * 16, b, 16); i++) out_total += b[1];
    uint8_t *src = (uint8_t *)malloc(in_total ? in_total : 1), *dst = (uint8_t *)calloc(1, out_total ? out_total : 1);
    if (src && dst) {
        uint64_t off = 0;
        for (uint64_t i = 0; i < n_in && i < 64 && gmem_read(c->mem, in_list + i * 16, b, 16); off += b[1], i++)
            gmem_read(c->mem, b[0], src + off, b[1]);
        pal_mutex_lock(g_ajm.lock);
        uint32_t id = hle_arg_u32(c, 1);
        ajm_instance *in = find(id);
        if (in) decode(in, src, in_total, dst, out_total, true, &r);
        else r.result = RES_INVALID_PARAMETER;
        write_result(c, hle_arg_ptr(c, 6), &r, true, true, in);
        pal_mutex_unlock(g_ajm.lock);
        off = 0;
        for (uint64_t i = 0; i < n_out && i < 64 && off < r.written && gmem_read(c->mem, out_list + i * 16, b, 16); i++) {
            uint64_t n = r.written - off < b[1] ? r.written - off : b[1];
            gmem_write(c->mem, b[0], dst + off, n);
            off += b[1];
        }
    }
    free(src);
    free(dst);
    hle_ret_s32(c, reserve_job(c, hle_arg_ptr(c, 0), 16 * (n_in + n_out) + 32));
}

/* RunSplit differs from DecodeSplit: flags is GPR2, counts/buffers move
 * right, and count/output/size occupy three stack arguments. Flags/ABI:
 * KytyPS5 AjmBatchJobRunSplit; confirmed at the t1x d00306 call site.
 * Decode the real scattered payload; never return success with an untouched
 * stream sideband. Keep optional unsupported reports explicit. */
static void ajm_job_run_split(hle_call *c) {
    const uint64_t multiple=1ull<<12,stream=1ull<<47,limit=64ull<<20;
    uint64_t flags=hle_arg(c,2),ni=hle_arg(c,4),no=hle_arg(c,6),cap=hle_arg(c,8);
    guest_addr ip=hle_arg_ptr(c,3),op=hle_arg_ptr(c,5),result=hle_arg_ptr(c,7);
    uint64_t input[64][2],output[64][2],in_bytes=0,out_bytes=0;
    uint64_t result_size=8+((flags&stream)?16:0)+((flags&multiple)?8:0);
    if((flags&~(multiple|stream)) || ni>64 || no>64 ||
       (ni && !gmem_read(c->mem,ip,input,ni*16)) ||
       (no && !gmem_read(c->mem,op,output,no*16)) ||
       (cap && (!result || cap<result_size || !gmem_is_mapped(c->mem,result,result_size)))) {
        hle_ret_s32(c,AJM_ERR_INVALID_PARAMETER);return;
    }
    for(uint64_t i=0;i<ni;i++) {
        if(input[i][1]>limit-in_bytes || (input[i][1] && !gmem_is_mapped(c->mem,input[i][0],input[i][1]))) {
            hle_ret_s32(c,AJM_ERR_INVALID_PARAMETER);return;
        }
        in_bytes+=input[i][1];
    }
    for(uint64_t i=0;i<no;i++) {
        if(output[i][1]>limit-out_bytes || (output[i][1] && !gmem_is_mapped(c->mem,output[i][0],output[i][1]))) {
            hle_ret_s32(c,AJM_ERR_INVALID_PARAMETER);return;
        }
        out_bytes+=output[i][1];
    }
    uint8_t *src=malloc((size_t)(in_bytes?in_bytes:1)),*dst=malloc((size_t)(out_bytes?out_bytes:1));
    if(!src || !dst){free(src);free(dst);hle_ret_s32(c,AJM_ERR_INVALID_PARAMETER);return;}
    uint64_t offset=0;bool valid=true;
    for(uint64_t i=0;i<ni && valid;i++) {
        valid=!input[i][1] || gmem_read(c->mem,input[i][0],src+offset,input[i][1]);offset+=input[i][1];
    }
    int32_t error=valid?reserve_job(c,hle_arg_ptr(c,0),16*(ni+no)+32):AJM_ERR_INVALID_PARAMETER;
    if(error){free(src);free(dst);hle_ret_s32(c,error);return;}
    job_result r={0};uint64_t samples=0;
    pal_mutex_lock(g_ajm.lock);
    ajm_instance *in=find(hle_arg_u32(c,1));
    if(in){decode(in,src,in_bytes,dst,out_bytes,(flags&multiple)!=0,&r);samples=in->total_samples;}
    else r.result=RES_INVALID_PARAMETER;
    if(in && in->codec==CODEC_AT9 && getenv("PS5C_AJM_TRACE")) {
        char line[512];int n=snprintf(line,sizeof(line),"ajm 0x%x runsplit: %llu in (",in->id,(unsigned long long)ni);
        for(uint64_t i=0;i<ni && n<400;i++) n+=snprintf(line+n,sizeof(line)-n," 0x%llx+%llu",(unsigned long long)input[i][0],(unsigned long long)input[i][1]);
        n+=snprintf(line+n,sizeof(line)-n," ) %llu out -> 0x%x consumed %llu written %llu frames %u; heads",(unsigned long long)out_bytes,r.result,(unsigned long long)r.consumed,(unsigned long long)r.written,r.frames);
        for(uint64_t o=0;o+4<=in_bytes && n<500;o+=in->at9_frame_bytes?in->at9_frame_bytes:64) n+=snprintf(line+n,sizeof(line)-n," %02x%02x",src[o],src[o+1]);
        LOG_I(HLE,"%s",line);
    }
    pal_mutex_unlock(g_ajm.lock);
    offset=0;
    for(uint64_t i=0;i<no && offset<r.written;i++) {
        uint64_t bytes=r.written-offset<output[i][1]?r.written-offset:output[i][1];
        if(bytes && !gmem_write(c->mem,output[i][0],dst+offset,bytes))r.result|=RES_INVALID_PARAMETER;
        offset+=bytes;
    }
    uint8_t side[32]={0};offset=8;
    memcpy(side,&r.result,4);memcpy(side+4,&r.internal,4);
    if(flags&stream) {
        int32_t consumed=(int32_t)r.consumed,written=(int32_t)r.written;
        memcpy(side+offset,&consumed,4);memcpy(side+offset+4,&written,4);memcpy(side+offset+8,&samples,8);offset+=16;
    }
    if(flags&multiple)memcpy(side+offset,&r.frames,4);
    if(cap && !gmem_write(c->mem,result,side,result_size))error=AJM_ERR_INVALID_PARAMETER;
    free(src);free(dst);hle_ret_s32(c,error);
}

/* (BatchInfo *, float interval, Statistics *): the float consumes XMM0,
 * so Statistics is GPR1. No dedicated AJM hardware exists in the host path;
 * finish the result and report zero dedicated engine/memory usage. */
static void ajm_job_statistics(hle_call *c) {
    uint8_t statistics[48]={0};guest_addr result=hle_arg_ptr(c,1);
    if(!result || !gmem_is_mapped(c->mem,result,sizeof(statistics))) {
        hle_ret_s32(c,AJM_ERR_INVALID_PARAMETER);return;
    }
    int32_t error=reserve_job(c,hle_arg_ptr(c,0),88);
    if(!error && !gmem_write(c->mem,result,statistics,sizeof(statistics)))error=AJM_ERR_INVALID_PARAMETER;
    hle_ret_s32(c,error);
}

/* int sceAjmBatchJobGetCodecInfo(info, instance, void *result, size_t size): result, then the codec's info */
static void ajm_job_get_codec_info(hle_call *c) {
    guest_addr at = hle_arg_ptr(c, 2);
    uint64_t size = hle_arg(c, 3);
    uint8_t buf[32];
    memset(buf, 0, sizeof(buf));
    pal_mutex_lock(g_ajm.lock);
    ajm_instance *in = find(hle_arg_u32(c, 1));
    if (!in) {
        int32_t e = RES_INVALID_PARAMETER;
        memcpy(buf, &e, 4);
    } else if (in->codec == CODEC_AT9) {
        uint32_t v[4] = {in->at9_frame_bytes * in->at9_frames_per_superframe, in->at9_frames_per_superframe,
                         in->at9_frame_bytes, in->at9_frame_samples};
        memcpy(buf + 8, v, sizeof(v));
    } else if (in->codec == CODEC_MP3) {
        uint32_t h = in->mp3_header;
        memcpy(buf + 8, &h, 4);
        buf[12] = (uint8_t)(!((h >> 16) & 1)); /* CRC present */
        buf[13] = (uint8_t)((h >> 6) & 3);
        buf[14] = (uint8_t)((h >> 4) & 3);
        buf[15] = (uint8_t)((h >> 3) & 1);
        buf[16] = (uint8_t)((h >> 2) & 1);
        buf[17] = (uint8_t)(h & 3);
    } else if (in->codec == CODEC_OPUS) {
        memcpy(buf + 8, &in->opus_frames_per_packet, 4);
    }
    pal_mutex_unlock(g_ajm.lock);
    if (at && size) gmem_write(c->mem, at, buf, size < sizeof(buf) ? size : sizeof(buf));
    hle_ret_s32(c, reserve_job(c, hle_arg_ptr(c, 0), JOB_RUN_SIZE));
}

/* int sceAjmBatchJobSetGaplessDecode(info, instance, const gapless *, int reset, result *) */
static void ajm_job_set_gapless(hle_call *c) {
    job_result r = {0};
    uint8_t g[8];
    pal_mutex_lock(g_ajm.lock);
    ajm_instance *in = find(hle_arg_u32(c, 1));
    if (!in || !gmem_read(c->mem, hle_arg_ptr(c, 2), g, 8)) {
        r.result = RES_INVALID_PARAMETER;
    } else {
        memcpy(&in->g_init.total, g, 4);
        memcpy(&in->g_init.skip, g + 4, 2);
        in->has_limit = in->g_init.total != 0 && in->g_init.total != 0xFFFFFFFFu;
        bool fresh = !in->g_cur.total && !in->g_cur.skip && !in->g_cur.skipped;
        if (hle_arg_s32(c, 3) || fresh) gapless_reset(in);
    }
    pal_mutex_unlock(g_ajm.lock);
    write_result(c, hle_arg_ptr(c, 4), &r, false, false, NULL);
    hle_ret_s32(c, reserve_job(c, hle_arg_ptr(c, 0), JOB_CONTROL_SIZE));
}

/* int sceAjmBatchJobGetGaplessDecode(info, instance, result *): result, gapless */
static void ajm_job_get_gapless(hle_call *c) {
    uint8_t buf[16];
    memset(buf, 0, sizeof(buf));
    pal_mutex_lock(g_ajm.lock);
    ajm_instance *in = find(hle_arg_u32(c, 1));
    if (in) {
        memcpy(buf + 8, &in->g_cur.total, 4);
        memcpy(buf + 12, &in->g_cur.skip, 2);
        memcpy(buf + 14, &in->g_cur.skipped, 2);
    }
    pal_mutex_unlock(g_ajm.lock);
    if (hle_arg_ptr(c, 2)) gmem_write(c->mem, hle_arg_ptr(c, 2), buf, sizeof(buf));
    hle_ret_s32(c, reserve_job(c, hle_arg_ptr(c, 0), JOB_RUN_SIZE));
}

/* int sceAjmBatchJobGetInfo(info, instance, result *): result, format */
static void ajm_job_get_info(hle_call *c) {
    uint32_t v[8] = {0};
    pal_mutex_lock(g_ajm.lock);
    ajm_instance *in = find(hle_arg_u32(c, 1));
    if (in) {
        v[2] = in->channels;
        v[3] = channel_mask(in->channels);
        v[4] = in->rate;
        v[5] = in->encoding;
    } else {
        v[0] = RES_INVALID_PARAMETER;
    }
    pal_mutex_unlock(g_ajm.lock);
    if (hle_arg_ptr(c, 2)) gmem_write(c->mem, hle_arg_ptr(c, 2), v, sizeof(v));
    hle_ret_s32(c, reserve_job(c, hle_arg_ptr(c, 0), JOB_RUN_SIZE));
}

/* SysV keeps float arguments in XMM0/1, independently of the integer
 * registers: flags is GPR2 and result is GPR3 in both variants.
 * Constant-ratio Ex contract: AnyPS5 e8209307. Ramps are unsupported. */
static void set_resample(hle_call *c, bool extended) {
    float ratio, change = 0;
    memcpy(&ratio, &c->a->xmm[0][0], sizeof(ratio));
    if (extended) memcpy(&change, &c->a->xmm[1][0], sizeof(change));
    if (!isfinite(ratio) || ratio <= 0 || change != 0) {
        hle_ret_s32(c, AJM_ERR_INVALID_PARAMETER);
        return;
    }
    int32_t error = reserve_job(c, hle_arg_ptr(c, 0), 64);
    if (error) { hle_ret_s32(c, error); return; }
    job_result r = {0};
    pal_mutex_lock(g_ajm.lock);
    ajm_instance *in = find(hle_arg_u32(c, 1));
    if (in) in->resampler.ratio = ratio;
    else r.result = RES_INVALID_PARAMETER;
    write_result(c, hle_arg_ptr(c, 3), &r, false, false, NULL);
    pal_mutex_unlock(g_ajm.lock);
    hle_ret_s32(c, 0);
}
static void ajm_job_set_resample(hle_call *c) { set_resample(c, false); }
static void ajm_job_set_resample_ex(hle_call *c) { set_resample(c, true); }

/* int sceAjmBatchJobGetResampleInfo(info, instance, result *): result, {f32 ratio, s32 samples, reserved[8]} */
static void ajm_job_get_resample_info(hle_call *c) {
    int32_t error = reserve_job(c, hle_arg_ptr(c, 0), JOB_RUN_SIZE);
    if (error) { hle_ret_s32(c, error); return; }
    uint8_t buf[48];
    memset(buf, 0, sizeof(buf));
    pal_mutex_lock(g_ajm.lock);
    ajm_instance *in = find(hle_arg_u32(c, 1));
    if (in) {
        size_t held = ajm_resample_held(&in->resampler);
        if (!ajm_resample_active(&in->resampler)) held += (size_t)in->at9_cache_count * in->at9_frame_samples;
        int32_t samples = (int32_t)(held > INT32_MAX ? INT32_MAX : held);
        memcpy(buf + 8, &in->resampler.ratio, 4);
        memcpy(buf + 12, &samples, 4);
    } else {
        int32_t result = RES_INVALID_PARAMETER;
        memcpy(buf, &result, 4);
    }
    if (hle_arg_ptr(c, 2)) gmem_write(c->mem, hle_arg_ptr(c, 2), buf, sizeof(buf));
    pal_mutex_unlock(g_ajm.lock);
    hle_ret_s32(c, 0);
}

/* int sceAjmBatchStart(ctx, info, int priority, SceAjmBatchError *error, SceAjmBatchId *out) */
static void ajm_batch_start(hle_call *c) {
    uint8_t zero[32] = {0};
    if (hle_arg_ptr(c, 3)) gmem_write(c->mem, hle_arg_ptr(c, 3), zero, sizeof(zero));
    pal_mutex_lock(g_ajm.lock);
    uint32_t id = ++g_ajm.next_batch;
    pal_mutex_unlock(g_ajm.lock);
    if (hle_arg_ptr(c, 4)) gmem_write(c->mem, hle_arg_ptr(c, 4), &id, 4);
    hle_ret_s32(c, 0);
}

/* int sceAjmBatchWait(ctx, batch, uint32_t timeout, SceAjmBatchError *error): jobs already ran */
static void ajm_batch_wait(hle_call *c) {
    uint8_t zero[32] = {0};
    if (hle_arg_ptr(c, 3)) gmem_write(c->mem, hle_arg_ptr(c, 3), zero, sizeof(zero));
    hle_ret_s32(c, 0);
}

/* int sceAjmBatchErrorDump(info, SceAjmBatchError *error) */
static void ajm_batch_error_dump(hle_call *c) {
    uint8_t zero[32] = {0};
    if (hle_arg_ptr(c, 1)) gmem_write(c->mem, hle_arg_ptr(c, 1), zero, sizeof(zero));
    hle_ret_s32(c, 0);
}

/* const char *sceAjmStrError(int error) */
static void ajm_str_error(hle_call *c) {
    pal_mutex_lock(g_ajm.lock);
    if (!g_ajm.error_text && gmem_map(c->mem, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "ajm strings", &g_ajm.error_text) == PS5C_OK)
        gmem_write(c->mem, g_ajm.error_text, "AJM error", 10);
    pal_mutex_unlock(g_ajm.lock);
    hle_ret_u64(c, g_ajm.error_text);
}

/* int sceAjmDecAt9ParseConfigData(const uint8_t config[4], info *): channels,
 * rate, frame samples, superframe samples, superframe bytes */
static void ajm_at9_parse_config(hle_call *c) {
    uint8_t cfg[4];
    if (!gmem_read(c->mem, hle_arg_ptr(c, 0), cfg, 4) || cfg[0] != 0xFE || !hle_arg_ptr(c, 1)) {
        hle_ret_s32(c, AJM_ERR_INVALID_PARAMETER);
        return;
    }
    static const uint32_t rates[16] = {11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
                                       44100, 48000, 64000, 88200, 96000, 128000, 176400, 192000};
    static const uint8_t log2s[16] = {6, 6, 7, 7, 7, 8, 8, 8, 6, 6, 7, 7, 7, 8, 8, 8};
    static const uint8_t chans[8] = {1, 2, 2, 6, 8, 4, 1, 2};
    uint32_t bits = (uint32_t)cfg[1] << 16 | (uint32_t)cfg[2] << 8 | cfg[3];
    uint32_t rate_idx = bits >> 20, frames = 1u << ((bits >> 3) & 3), frame_samples = 1u << log2s[rate_idx];
    uint32_t v[5] = {chans[(bits >> 17) & 7], rates[rate_idx], frame_samples, frame_samples * frames,
                     (((bits >> 5) & 0x7FF) + 1) * frames};
    gmem_write(c->mem, hle_arg_ptr(c, 1), v, sizeof(v));
    hle_ret_s32(c, 0);
}

/* ---- Jobs written into a raw batch buffer (PS4 FMOD: Tearaway Unfolded) ----
 * As with the BatchInfo calls, a job runs when it is written. The game steps through the
 * buffer by itself (0x40 bytes a control job, 0x50 a run job) and only submits it to
 * sceAjmBatchStartBuffer, which then has nothing left to do. Flags as in shadPS4's AjmJobFlags. */
#define JOB_FLAG_CODEC_INFO (1ull << 11)
#define JOB_FLAG_MULTIPLE (1ull << 12)
#define JOB_FLAG_GAPLESS (1ull << 45)
#define JOB_FLAG_FORMAT (1ull << 46)
#define JOB_FLAG_STREAM (1ull << 47)
#define JOB_BUFFER_CONTROL 0x40
#define JOB_BUFFER_RUN 0x50

/* void *sceAjmBatchJobControlBufferRa(void *buffer, instance, flags, const void *in, size_t in_size,
 * void *out, size_t out_size, void *return_address): sceAjmBatchJobControl's arguments */
static void ajm_job_control_buffer(hle_call *c) {
    guest_addr buf = hle_arg_ptr(c, 0);
    control_job(c);
    hle_ret_u64(c, buf ? buf + JOB_BUFFER_CONTROL : 0);
}

/* The codec's information after a job's result (sceAjmBatchJobGetCodecInfo): up to 24 bytes */
static void codec_info(const ajm_instance *in, uint8_t *dst) {
    if (in->codec == CODEC_AT9) {
        uint32_t v[4] = {in->at9_frame_bytes * in->at9_frames_per_superframe, in->at9_frames_per_superframe,
                         in->at9_frame_bytes, in->at9_frame_samples};
        memcpy(dst, v, sizeof(v));
    } else if (in->codec == CODEC_MP3) {
        uint32_t h = in->mp3_header;
        memcpy(dst, &h, 4);
        dst[4] = (uint8_t)(!((h >> 16) & 1));
        dst[5] = (uint8_t)((h >> 6) & 3);
        dst[6] = (uint8_t)((h >> 4) & 3);
        dst[7] = (uint8_t)((h >> 3) & 1);
        dst[8] = (uint8_t)((h >> 2) & 1);
        dst[9] = (uint8_t)(h & 3);
    } else if (in->codec == CODEC_OPUS) {
        memcpy(dst, &in->opus_frames_per_packet, 4);
    }
}

/* void *sceAjmBatchJobRunBufferRa(void *buffer, instance, flags, const void *in, size_t in_size, void *out,
 * size_t out_size, void *sideband, size_t sideband_size, void *return_address). The sideband holds the
 * result, then by flag: stream {consumed, written, u64 samples}, format {channels, mask, rate, encoding,
 * 8 reserved bytes}, gapless {u32 total, u16 skip, u16 skipped}, frames {u32, u32}, codec information. */
static void ajm_job_run_buffer(hle_call *c) {
    guest_addr buf = hle_arg_ptr(c, 0), in_addr = hle_arg_ptr(c, 3), out_addr = hle_arg_ptr(c, 5), sb = hle_arg_ptr(c, 7);
    uint64_t flags = hle_arg(c, 2), in_size = hle_arg(c, 4), out_size = hle_arg(c, 6), sb_size = hle_arg(c, 8);
    uint32_t id = hle_arg_u32(c, 1);
    job_result r = {0};
    uint8_t side[96];
    memset(side, 0, sizeof(side));
    size_t n = 8;
    pal_mutex_lock(g_ajm.lock);
    ajm_instance *in = find(id);
    if (!in) r.result = RES_INVALID_PARAMETER;
    else if (in_size || out_size) run_decode(c, id, in_addr, in_size, out_addr, out_size, (flags & JOB_FLAG_MULTIPLE) != 0, &r);
    if (flags & JOB_FLAG_STREAM) {
        int32_t consumed = (int32_t)(r.consumed > 0x7FFFFFFF ? 0x7FFFFFFF : r.consumed);
        int32_t written = (int32_t)(r.written > 0x7FFFFFFF ? 0x7FFFFFFF : r.written);
        uint64_t total = in ? in->total_samples : 0;
        memcpy(side + n, &consumed, 4);
        memcpy(side + n + 4, &written, 4);
        memcpy(side + n + 8, &total, 8);
        n += 16;
    }
    if (flags & JOB_FLAG_FORMAT) {
        uint32_t v[6] = {0};
        if (in) v[0] = in->channels, v[1] = channel_mask(in->channels), v[2] = in->rate, v[3] = in->encoding;
        memcpy(side + n, v, sizeof(v));
        n += 24;
    }
    if (flags & JOB_FLAG_GAPLESS) {
        if (in) {
            memcpy(side + n, &in->g_cur.total, 4);
            memcpy(side + n + 4, &in->g_cur.skip, 2);
            memcpy(side + n + 6, &in->g_cur.skipped, 2);
        }
        n += 8;
    }
    if (flags & JOB_FLAG_MULTIPLE) {
        memcpy(side + n, &r.frames, 4);
        n += 8;
    }
    if (flags & JOB_FLAG_CODEC_INFO) {
        if (in) codec_info(in, side + n);
        n += 24;
    }
    pal_mutex_unlock(g_ajm.lock);
    memcpy(side, &r.result, 4);
    memcpy(side + 4, &r.internal, 4);
    if (sb && sb_size) gmem_write(c->mem, sb, side, sb_size < n ? sb_size : n);
    static uint32_t logged;
    if (logged++ < 8)
        LOG_I(HLE, "sceAjmBatchJobRunBufferRa(0x%x, flags 0x%llx, in %llu, out %llu, sideband %llu) -> 0x%x, consumed %llu written %llu",
              id, (unsigned long long)flags, (unsigned long long)in_size, (unsigned long long)out_size,
              (unsigned long long)sb_size, (unsigned)r.result, (unsigned long long)r.consumed, (unsigned long long)r.written);
    hle_ret_u64(c, buf ? buf + JOB_BUFFER_RUN : 0);
}

/* void *sceAjmBatchJobInlineBuffer(void *buffer, const void *data, size_t size, void **data_in_batch):
 * the data copied into the batch after an 8-byte header */
static void ajm_job_inline_buffer(hle_call *c) {
    guest_addr buf = hle_arg_ptr(c, 0), data = hle_arg_ptr(c, 1), out = hle_arg_ptr(c, 3);
    uint64_t size = hle_arg(c, 2);
    const void *src = size ? gmem_host(c->mem, data, size) : NULL;
    if (!buf || (size && !src)) {
        hle_ret_u64(c, 0);
        return;
    }
    if (size) gmem_write(c->mem, buf + 8, src, size);
    guest_addr at = buf + 8;
    if (out) gmem_write(c->mem, out, &at, 8);
    hle_ret_u64(c, buf + 8 + ((size + 7) & ~7ull));
}

/* int sceAjmBatchStartBuffer(ctx, const void *batch, size_t size, int priority, SceAjmBatchError *error,
 * SceAjmBatchId *out): the jobs already ran */
static void ajm_batch_start_buffer(hle_call *c) {
    uint8_t zero[32] = {0};
    if (hle_arg_ptr(c, 4)) gmem_write(c->mem, hle_arg_ptr(c, 4), zero, sizeof(zero));
    pal_mutex_lock(g_ajm.lock);
    uint32_t id = ++g_ajm.next_batch;
    pal_mutex_unlock(g_ajm.lock);
    if (hle_arg_ptr(c, 5)) gmem_write(c->mem, hle_arg_ptr(c, 5), &id, 4);
    hle_ret_s32(c, 0);
}

#define A "libSceAjm"

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(A, "sceAjmInitialize", ajm_initialize),
    HLE_FUNC_ENTRY(A, "sceAjmFinalize", ajm_ok),
    HLE_FUNC_ENTRY(A, "sceAjmDecMp3ParseFrame", ajm_mp3_parse_frame),
    HLE_FUNC_ENTRY(A, "sceAjmModuleRegister", ajm_module_register),
    HLE_FUNC_ENTRY(A, "sceAjmModuleUnregister", ajm_ok),
    HLE_FUNC_ENTRY(A, "sceAjmMemoryRegister", ajm_ok),
    HLE_FUNC_ENTRY(A, "sceAjmMemoryUnregister", ajm_ok),
    HLE_FUNC_ENTRY(A, "sceAjmInstanceCreate", ajm_instance_create),
    HLE_FUNC_ENTRY(A, "sceAjmInstanceDestroy", ajm_instance_destroy),
    HLE_FUNC_ENTRY(A, "sceAjmBatchInitialize", ajm_batch_initialize),
    HLE_FUNC_ENTRY(A, "sceAjmBatchJobInitialize", ajm_job_initialize),
    HLE_FUNC_ENTRY(A, "sceAjmBatchJobClearContext", ajm_job_clear_context),
    HLE_FUNC_ENTRY(A, "sceAjmBatchJobControl", ajm_job_control),
    HLE_FUNC_ENTRY(A, "sceAjmBatchJobDecode", ajm_job_decode),
    HLE_FUNC_ENTRY(A, "sceAjmBatchJobDecodeSingle", ajm_job_decode_single),
    HLE_FUNC_ENTRY(A, "sceAjmBatchJobDecodeSplit", ajm_job_decode_split),
    HLE_FUNC_ENTRY(A, "sceAjmBatchJobRunSplit", ajm_job_run_split),
    HLE_FUNC_ENTRY(A, "sceAjmBatchJobGetStatistics", ajm_job_statistics),
    HLE_FUNC_ENTRY(A, "sceAjmBatchJobGetCodecInfo", ajm_job_get_codec_info),
    HLE_FUNC_ENTRY(A, "sceAjmBatchJobSetGaplessDecode", ajm_job_set_gapless),
    HLE_FUNC_ENTRY(A, "sceAjmBatchJobGetGaplessDecode", ajm_job_get_gapless),
    HLE_FUNC_ENTRY(A, "sceAjmBatchJobGetInfo", ajm_job_get_info),
    HLE_FUNC_ENTRY(A, "sceAjmBatchJobSetResampleParameters", ajm_job_set_resample),
    HLE_FUNC_ENTRY(A, "sceAjmBatchJobSetResampleParametersEx", ajm_job_set_resample_ex),
    HLE_FUNC_ENTRY(A, "sceAjmBatchJobGetResampleInfo", ajm_job_get_resample_info),
    HLE_FUNC_ENTRY(A, "sceAjmBatchJobControlBufferRa", ajm_job_control_buffer),
    HLE_FUNC_ENTRY(A, "sceAjmBatchJobRunBufferRa", ajm_job_run_buffer),
    HLE_FUNC_ENTRY(A, "sceAjmBatchJobInlineBuffer", ajm_job_inline_buffer),
    HLE_FUNC_ENTRY(A, "sceAjmBatchStart", ajm_batch_start),
    HLE_FUNC_ENTRY(A, "sceAjmBatchStartBuffer", ajm_batch_start_buffer),
    HLE_FUNC_ENTRY(A, "sceAjmBatchWait", ajm_batch_wait),
    HLE_FUNC_ENTRY(A, "sceAjmBatchCancel", ajm_ok),
    HLE_FUNC_ENTRY(A, "sceAjmBatchErrorDump", ajm_batch_error_dump),
    HLE_FUNC_ENTRY(A, "sceAjmStrError", ajm_str_error),
    HLE_FUNC_ENTRY(A, "sceAjmDecAt9ParseConfigData", ajm_at9_parse_config),
};

void hle_register_ajm(void) {
    if (!g_ajm.lock && pal_mutex_create(&g_ajm.lock) != PS5C_OK) {
        LOG_E(HLE, "cannot create the AJM lock");
        return;
    }
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}
