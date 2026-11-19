/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Movie decoding through FFmpeg (libavformat demuxes, libavcodec decodes,
 * libswscale converts pictures to NV12, libswresample sound to 16-bit PCM of
 * at most two channels). One demuxer feeds both streams: packets read for
 * one stream while looking for the other wait in a small per-stream queue. */
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "base/log.h"
#include "pal/pal_exfat.h"
#include "pal/pal_media_backends.h"

typedef struct packet_queue {
    AVPacket **items;
    size_t head, count, cap;
} packet_queue;

typedef struct stream_dec {
    int index; /* -1: none */
    AVCodecContext *ctx;
    packet_queue queue;
    bool flushed; /* the end-of-stream flush packet was sent */
    bool ended;
    int64_t start; /* first timestamp, in the stream's time base */
    AVRational time_base;
} stream_dec;

struct ff_media {
    AVFormatContext *fmt;
    AVIOContext *io;
    pal_file *file; /* virtual image file: streamed through PAL callbacks */
    uint8_t *mem; /* movies given as memory */
    uint64_t mem_size, mem_pos;
    stream_dec video, audio;
    bool demux_end;
    AVPacket *pkt;
    AVFrame *frame;
    struct SwsContext *sws;
    SwrContext *swr;
    pal_media_info info;
    int16_t *pending; /* converted PCM not handed out yet */
    uint32_t pending_frames, pending_pos, pending_cap;
    uint64_t pending_time_us;
};

/* ---- Packet queues ---- */

static bool queue_push(packet_queue *q, AVPacket *p) {
    if (q->count == q->cap) {
        size_t cap = q->cap ? q->cap * 2 : 64;
        AVPacket **items = (AVPacket **)malloc(cap * sizeof(AVPacket *));
        if (!items) return false;
        for (size_t i = 0; i < q->count; i++) items[i] = q->items[(q->head + i) % q->cap];
        free(q->items);
        q->items = items;
        q->cap = cap;
        q->head = 0;
    }
    q->items[(q->head + q->count++) % q->cap] = p;
    return true;
}

static AVPacket *queue_pop(packet_queue *q) {
    if (!q->count) return NULL;
    AVPacket *p = q->items[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;
    return p;
}

static void queue_clear(packet_queue *q) {
    AVPacket *p;
    while ((p = queue_pop(q)) != NULL) av_packet_free(&p);
}

/* The next packet of `s` (NULL at the end of the file) */
static AVPacket *next_packet(ff_media *m, stream_dec *s) {
    AVPacket *p = queue_pop(&s->queue);
    if (p) return p;
    while (!m->demux_end) {
        if (av_read_frame(m->fmt, m->pkt) < 0) {
            m->demux_end = true;
            break;
        }
        stream_dec *owner = m->pkt->stream_index == m->video.index   ? &m->video
                            : m->pkt->stream_index == m->audio.index ? &m->audio
                                                                     : NULL;
        if (!owner) {
            av_packet_unref(m->pkt);
            continue;
        }
        AVPacket *copy = av_packet_clone(m->pkt);
        av_packet_unref(m->pkt);
        if (!copy) return NULL;
        if (owner == s) return copy;
        /* the other stream's: kept for it, unless it is not being read (a
         * movie whose sound nobody takes must not pile up) */
        if (owner->queue.count > 4096 || !queue_push(&owner->queue, copy)) av_packet_free(&copy);
    }
    return NULL;
}

/* The next decoded frame of `s` in m->frame: 1, 0 at the end, -1 on error */
static int decode_frame(ff_media *m, stream_dec *s) {
    if (s->index < 0 || s->ended) return 0;
    for (;;) {
        int r = avcodec_receive_frame(s->ctx, m->frame);
        if (r == 0) return 1;
        if (r == AVERROR_EOF) {
            s->ended = true;
            return 0;
        }
        if (r != AVERROR(EAGAIN)) return -1;
        AVPacket *p = next_packet(m, s);
        if (!p) {
            if (s->flushed) {
                s->ended = true;
                return 0;
            }
            s->flushed = true;
            avcodec_send_packet(s->ctx, NULL);
            continue;
        }
        r = avcodec_send_packet(s->ctx, p);
        av_packet_free(&p);
        if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_INVALIDDATA) return -1;
    }
}

static uint64_t frame_time_us(const stream_dec *s, const AVFrame *f) {
    int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    if (ts == AV_NOPTS_VALUE) return 0;
    if (s->start != AV_NOPTS_VALUE) ts -= s->start;
    return ts > 0 ? (uint64_t)av_rescale_q(ts, s->time_base, (AVRational){1, 1000000}) : 0;
}

/* ---- Opening ---- */

static bool open_stream(ff_media *m, stream_dec *s, enum AVMediaType type) {
    s->index = -1;
    const AVCodec *codec = NULL;
    int i = av_find_best_stream(m->fmt, type, -1, -1, &codec, 0);
    if (i < 0 || !codec) return false;
    AVStream *st = m->fmt->streams[i];
    s->ctx = avcodec_alloc_context3(codec);
    if (!s->ctx || avcodec_parameters_to_context(s->ctx, st->codecpar) < 0) return false;
    s->ctx->thread_count = type == AVMEDIA_TYPE_VIDEO ? 4 : 1;
    if (avcodec_open2(s->ctx, codec, NULL) < 0) {
        avcodec_free_context(&s->ctx);
        return false;
    }
    s->index = i;
    s->time_base = st->time_base;
    s->start = st->start_time;
    return true;
}

static bool setup_audio(ff_media *m) {
    AVCodecContext *c = m->audio.ctx;
    AVChannelLayout out_layout;
    int channels = c->ch_layout.nb_channels >= 2 ? 2 : 1;
    av_channel_layout_default(&out_layout, channels);
    int rate = c->sample_rate > 0 ? c->sample_rate : 48000;
    if (swr_alloc_set_opts2(&m->swr, &out_layout, AV_SAMPLE_FMT_S16, rate, &c->ch_layout, c->sample_fmt, rate, 0, NULL) < 0 ||
        swr_init(m->swr) < 0)
        return false;
    m->info.channels = (uint32_t)channels;
    m->info.sample_rate = (uint32_t)rate;
    return true;
}

static ff_media *finish_open(ff_media *m) {
    m->pkt = av_packet_alloc();
    m->frame = av_frame_alloc();
    if (!m->pkt || !m->frame || avformat_find_stream_info(m->fmt, NULL) < 0) {
        ff_media_close(m);
        return NULL;
    }
    if (open_stream(m, &m->video, AVMEDIA_TYPE_VIDEO)) {
        m->info.width = (uint32_t)m->video.ctx->width;
        m->info.height = (uint32_t)m->video.ctx->height;
        AVRational fr = m->fmt->streams[m->video.index]->avg_frame_rate;
        if (fr.num > 0 && fr.den > 0) m->info.frame_rate = av_q2d(fr);
    }
    if (open_stream(m, &m->audio, AVMEDIA_TYPE_AUDIO) && !setup_audio(m)) {
        avcodec_free_context(&m->audio.ctx);
        m->audio.index = -1;
    }
    if (m->fmt->duration > 0) m->info.duration_us = (uint64_t)m->fmt->duration * 1000000u / AV_TIME_BASE;
    if (!m->info.width && !m->info.channels) {
        ff_media_close(m);
        return NULL;
    }
    return m;
}

static int image_read(void *opaque, uint8_t *buf, int size) {
    ff_media *m = (ff_media *)opaque;
    uint64_t got = 0;
    if (size <= 0 || pal_file_read(m->file, buf, (uint64_t)size, &got) != PS5C_OK) return AVERROR(EIO);
    return got ? (int)got : AVERROR_EOF;
}
static int64_t image_seek(void *opaque, int64_t off, int whence) {
    ff_media *m = (ff_media *)opaque;
    uint64_t pos = 0;
    if (whence == AVSEEK_SIZE) return pal_file_size(m->file, &pos) == PS5C_OK ? (int64_t)pos : -1;
    int w = whence & ~AVSEEK_FORCE;
    if (w < SEEK_SET || w > SEEK_END) return -1;
    return pal_file_seek(m->file, off, w, &pos) == PS5C_OK ? (int64_t)pos : -1;
}
ff_media *ff_media_open_file(const char *path) {
    ff_media *m = (ff_media *)calloc(1, sizeof(ff_media));
    if (!m) return NULL;
    m->video.index = m->audio.index = -1;
    bool image = exfat_path(path);
    if (image) {
        unsigned char *buf = (unsigned char *)av_malloc(1 << 16);
        m->fmt = avformat_alloc_context();
        if (!buf || !m->fmt || pal_file_open(path, PAL_FILE_READ, &m->file) != PS5C_OK) {
            av_free(buf); ff_media_close(m); return NULL;
        }
        m->io = avio_alloc_context(buf, 1 << 16, 0, m, image_read, NULL, image_seek);
        if (!m->io) { av_free(buf); ff_media_close(m); return NULL; }
        m->fmt->pb = m->io;
        m->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    }
    if (avformat_open_input(&m->fmt, image ? NULL : path, NULL, NULL) < 0) {
        LOG_W(HLE, "movies: FFmpeg cannot open %s", path);
        ff_media_close(m);
        return NULL;
    }
    return finish_open(m);
}

static int mem_read(void *opaque, uint8_t *buf, int size) {
    ff_media *m = (ff_media *)opaque;
    uint64_t left = m->mem_size - m->mem_pos;
    if (!left) return AVERROR_EOF;
    int n = (uint64_t)size < left ? size : (int)left;
    memcpy(buf, m->mem + m->mem_pos, (size_t)n);
    m->mem_pos += (uint64_t)n;
    return n;
}

static int64_t mem_seek(void *opaque, int64_t offset, int whence) {
    ff_media *m = (ff_media *)opaque;
    if (whence == AVSEEK_SIZE) return (int64_t)m->mem_size;
    int64_t base = (whence & 3) == SEEK_CUR ? (int64_t)m->mem_pos : (whence & 3) == SEEK_END ? (int64_t)m->mem_size : 0;
    int64_t pos = base + offset;
    if (pos < 0 || (uint64_t)pos > m->mem_size) return -1;
    m->mem_pos = (uint64_t)pos;
    return pos;
}

ff_media *ff_media_open_memory(const void *data, uint64_t size) {
    ff_media *m = (ff_media *)calloc(1, sizeof(ff_media));
    if (!m) return NULL;
    m->video.index = m->audio.index = -1;
    m->mem = (uint8_t *)malloc(size ? size : 1);
    unsigned char *buf = (unsigned char *)av_malloc(1 << 16);
    m->fmt = avformat_alloc_context();
    if (!m->mem || !buf || !m->fmt) {
        av_free(buf);
        ff_media_close(m);
        return NULL;
    }
    memcpy(m->mem, data, size);
    m->mem_size = size;
    m->io = avio_alloc_context(buf, 1 << 16, 0, m, mem_read, NULL, mem_seek);
    if (!m->io) {
        av_free(buf);
        ff_media_close(m);
        return NULL;
    }
    m->fmt->pb = m->io;
    if (avformat_open_input(&m->fmt, NULL, NULL, NULL) < 0) {
        LOG_W(HLE, "movies: FFmpeg cannot decode a %llu-byte movie", (unsigned long long)size);
        m->fmt = NULL; /* freed by avformat_open_input on failure */
        ff_media_close(m);
        return NULL;
    }
    return finish_open(m);
}

void ff_media_get_info(ff_media *m, pal_media_info *out) { *out = m->info; }

/* ---- Decoding ---- */

int ff_media_next_video(ff_media *m, uint8_t *nv12, uint64_t *time_us) {
    int r = decode_frame(m, &m->video);
    if (r <= 0) return r;
    AVFrame *f = m->frame;
    int w = (int)m->info.width, h = (int)m->info.height;
    m->sws = sws_getCachedContext(m->sws, f->width, f->height, (enum AVPixelFormat)f->format, w, h, AV_PIX_FMT_NV12,
                                  SWS_BILINEAR, NULL, NULL, NULL);
    if (!m->sws) {
        av_frame_unref(f);
        return -1;
    }
    uint8_t *planes[4] = {nv12, nv12 + (size_t)w * h, NULL, NULL};
    int strides[4] = {w, w, 0, 0};
    sws_scale(m->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, planes, strides);
    *time_us = frame_time_us(&m->video, f);
    av_frame_unref(f);
    return 1;
}

int ff_media_next_audio(ff_media *m, int16_t *pcm, uint32_t max_frames, uint64_t *time_us) {
    uint32_t ch = m->info.channels;
    if (!ch) return 0;
    while (m->pending_pos >= m->pending_frames) {
        int r = decode_frame(m, &m->audio);
        if (r <= 0) return r;
        AVFrame *f = m->frame;
        int out_max = swr_get_out_samples(m->swr, f->nb_samples);
        if (out_max > (int)m->pending_cap) {
            int16_t *grown = (int16_t *)realloc(m->pending, (size_t)out_max * ch * 2);
            if (!grown) {
                av_frame_unref(f);
                return -1;
            }
            m->pending = grown;
            m->pending_cap = (uint32_t)out_max;
        }
        uint8_t *out[1] = {(uint8_t *)m->pending};
        int got = swr_convert(m->swr, out, out_max, (const uint8_t **)f->extended_data, f->nb_samples);
        m->pending_time_us = frame_time_us(&m->audio, f);
        av_frame_unref(f);
        m->pending_frames = got > 0 ? (uint32_t)got : 0;
        m->pending_pos = 0;
    }
    uint32_t n = m->pending_frames - m->pending_pos;
    if (n > max_frames) n = max_frames;
    memcpy(pcm, m->pending + (size_t)m->pending_pos * ch, (size_t)n * ch * 2);
    *time_us = m->pending_time_us + (uint64_t)m->pending_pos * 1000000u / m->info.sample_rate;
    m->pending_pos += n;
    return (int)n;
}

bool ff_media_rewind(ff_media *m) {
    if (av_seek_frame(m->fmt, -1, 0, AVSEEK_FLAG_BACKWARD) < 0) return false;
    stream_dec *streams[2] = {&m->video, &m->audio};
    for (int i = 0; i < 2; i++) {
        queue_clear(&streams[i]->queue);
        if (streams[i]->ctx) avcodec_flush_buffers(streams[i]->ctx);
        streams[i]->flushed = streams[i]->ended = false;
    }
    m->demux_end = false;
    m->pending_frames = m->pending_pos = 0;
    return true;
}

void ff_media_close(ff_media *m) {
    if (!m) return;
    queue_clear(&m->video.queue);
    queue_clear(&m->audio.queue);
    free(m->video.queue.items);
    free(m->audio.queue.items);
    avcodec_free_context(&m->video.ctx);
    avcodec_free_context(&m->audio.ctx);
    if (m->fmt) avformat_close_input(&m->fmt);
    if (m->io) {
        av_freep(&m->io->buffer);
        avio_context_free(&m->io);
    }
    sws_freeContext(m->sws);
    swr_free(&m->swr);
    av_packet_free(&m->pkt);
    av_frame_free(&m->frame);
    if (m->file) pal_file_close(m->file);
    free(m->mem);
    free(m->pending);
    free(m);
}
