/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pal_vdec through libavcodec: access units as games hand them to
 * libSceVideodec2 (H.264 / HEVC Annex B, VP9 frames), pictures out as NV12
 * with 256-aligned rows. Timestamps and the game's attached value ride with
 * each packet (AV_CODEC_FLAG_COPY_OPAQUE) through B-frame reordering. */
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libswscale/swscale.h>

#include "base/log.h"
#include "pal/pal.h"

typedef struct au_meta {
    uint64_t pts, dts, attached;
} au_meta;

struct pal_vdec {
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
    struct SwsContext *sws;
};

pal_vdec *pal_vdec_open_ex(pal_vdec_codec codec,bool apply_cropping) {
    enum AVCodecID id = codec == PAL_VDEC_H264   ? AV_CODEC_ID_H264
                        : codec == PAL_VDEC_HEVC ? AV_CODEC_ID_HEVC
                                                 : AV_CODEC_ID_VP9;
    const AVCodec *dec = avcodec_find_decoder(id);
    if (!dec) {
        LOG_W(VIDEO, "video decoder: FFmpeg has no %s decoder", avcodec_get_name(id));
        return NULL;
    }
    pal_vdec *d = (pal_vdec *)calloc(1, sizeof(pal_vdec));
    if (!d)
        return NULL;
    d->ctx = avcodec_alloc_context3(dec);
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (d->ctx) {
        d->ctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
        d->ctx->apply_cropping=apply_cropping;
    }
    if (!d->ctx || !d->pkt || !d->frame || avcodec_open2(d->ctx, dec, NULL) < 0) {
        pal_vdec_close(d);
        return NULL;
    }
    return d;
}

pal_vdec *pal_vdec_open(pal_vdec_codec codec) {return pal_vdec_open_ex(codec,true);}

/* The frame just received into `out` as NV12. */
static int take_frame(pal_vdec *d, uint8_t *out, uint64_t cap, pal_vdec_picture *pic) {
    AVFrame *f = d->frame;
    if (f->width <= 0 || f->height <= 0)
        return -1;
    uint32_t w = (uint32_t)f->width, h = (uint32_t)f->height, pitch = (w + 255u) & ~255u,
             ch = (h + 1) / 2;
    uint64_t need = (uint64_t)pitch * h + (uint64_t)pitch * ch;
    if (need > cap)
        return -2;
    uint8_t *uv = out + (size_t)pitch * h;
    if (f->format == AV_PIX_FMT_NV12) {
        for (uint32_t y = 0; y < h; y++)
            memcpy(out + (size_t)y * pitch, f->data[0] + (ptrdiff_t)y * f->linesize[0], w);
        for (uint32_t y = 0; y < ch; y++)
            memcpy(uv + (size_t)y * pitch, f->data[1] + (ptrdiff_t)y * f->linesize[1], w);
    } else if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P) {
        for (uint32_t y = 0; y < h; y++)
            memcpy(out + (size_t)y * pitch, f->data[0] + (ptrdiff_t)y * f->linesize[0], w);
        for (uint32_t y = 0; y < ch; y++) {
            const uint8_t *cb = f->data[1] + (ptrdiff_t)y * f->linesize[1],
                          *cr = f->data[2] + (ptrdiff_t)y * f->linesize[2];
            uint8_t *o = uv + (size_t)y * pitch;
            for (uint32_t x = 0; x < (w + 1) / 2; x++)
                o[2 * x] = cb[x], o[2 * x + 1] = cr[x];
        }
    } else { /* 10-bit and other layouts */
        d->sws = sws_getCachedContext(d->sws, f->width, f->height, (enum AVPixelFormat)f->format,
                                      f->width, f->height, AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, NULL,
                                      NULL, NULL);
        uint8_t *planes[4] = {out, uv, NULL, NULL};
        int strides[4] = {(int)pitch, (int)pitch, 0, 0};
        if (!d->sws || sws_scale(d->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height,
                                 planes, strides) != f->height)
            return -1;
    }
    memset(pic, 0, sizeof(*pic));
    pic->width = w, pic->height = h, pic->pitch = pitch;
    pic->crop_left = (uint32_t)f->crop_left, pic->crop_right = (uint32_t)f->crop_right;
    pic->crop_top = (uint32_t)f->crop_top, pic->crop_bottom = (uint32_t)f->crop_bottom;
    if (f->opaque_ref && f->opaque_ref->size >= sizeof(au_meta)) {
        au_meta m;
        memcpy(&m, f->opaque_ref->data, sizeof(m));
        pic->pts = m.pts, pic->dts = m.dts, pic->attached = m.attached;
    } else {
        pic->pts = f->pts == AV_NOPTS_VALUE ? ~0ull : (uint64_t)f->pts;
        pic->dts = f->pkt_dts == AV_NOPTS_VALUE ? ~0ull : (uint64_t)f->pkt_dts;
    }
    pic->profile = d->ctx->profile > 0 ? (uint32_t)d->ctx->profile : 0;
    pic->level = d->ctx->level > 0 ? (uint32_t)d->ctx->level : 0;
    pic->sar_width = (uint16_t)(f->sample_aspect_ratio.num > 0 && f->sample_aspect_ratio.num < 65536
                                    ? f->sample_aspect_ratio.num
                                    : 0);
    pic->sar_height =
        (uint16_t)(f->sample_aspect_ratio.den > 0 && f->sample_aspect_ratio.den < 65536
                       ? f->sample_aspect_ratio.den
                       : 0);
    pic->full_range = f->color_range == AVCOL_RANGE_JPEG;
    pic->primaries = (uint8_t)f->color_primaries, pic->transfer = (uint8_t)f->color_trc,
    pic->matrix = (uint8_t)f->colorspace;
    pic->key = (f->flags & AV_FRAME_FLAG_KEY) != 0;
    pic->corrupt = (f->flags & AV_FRAME_FLAG_CORRUPT) != 0;
    return 1;
}

static int receive(pal_vdec *d, uint8_t *out, uint64_t cap, pal_vdec_picture *pic) {
    int r = avcodec_receive_frame(d->ctx, d->frame);
    if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
        return 0;
    if (r < 0)
        return -1;
    r = take_frame(d, out, cap, pic);
    av_frame_unref(d->frame);
    return r;
}

int pal_vdec_decode(pal_vdec *d, const void *au, uint64_t size, uint64_t pts, uint64_t dts,
                    uint64_t attached, uint8_t *out, uint64_t cap, pal_vdec_picture *pic) {
    if (size > 0x7FFFFFFF || av_new_packet(d->pkt, (int)size) < 0)
        return -1;
    memcpy(d->pkt->data, au, (size_t)size);
    d->pkt->pts = pts > (uint64_t)INT64_MAX ? AV_NOPTS_VALUE : (int64_t)pts;
    d->pkt->dts = dts > (uint64_t)INT64_MAX ? AV_NOPTS_VALUE : (int64_t)dts;
    d->pkt->opaque_ref = av_buffer_alloc(sizeof(au_meta));
    if (d->pkt->opaque_ref) {
        au_meta m = {pts, dts, attached};
        memcpy(d->pkt->opaque_ref->data, &m, sizeof(m));
    }
    int got = 0, r = avcodec_send_packet(d->ctx, d->pkt);
    if (r ==
        AVERROR(EAGAIN)) { /* the decoder is full: hand out a picture, then the packet goes in */
        got = receive(d, out, cap, pic);
        if (got <= 0) {
            av_packet_unref(d->pkt);
            return got < 0 ? got : -1;
        }
        r = avcodec_send_packet(d->ctx, d->pkt);
    }
    av_packet_unref(d->pkt);
    if (r < 0) {
        static uint32_t logged;
        if (logged++ < 8)
            LOG_W(VIDEO, "video decoder: access unit of %llu bytes rejected (%d)",
                  (unsigned long long)size, r);
        return got ? got : -1;
    }
    return got ? got : receive(d, out, cap, pic);
}

int pal_vdec_flush(pal_vdec *d, uint8_t *out, uint64_t cap, pal_vdec_picture *pic) {
    return receive(d, out, cap, pic);
}

int pal_vdec_drain(pal_vdec *d,uint8_t *out,uint64_t cap,pal_vdec_picture *pic) {
    int r=avcodec_send_packet(d->ctx,NULL);
    if(r<0 && r!=AVERROR_EOF && r!=AVERROR(EAGAIN))return -1;
    return receive(d,out,cap,pic);
}

void pal_vdec_reset(pal_vdec *d) {
    avcodec_flush_buffers(d->ctx);
}

void pal_vdec_close(pal_vdec *d) {
    if (!d)
        return;
    sws_freeContext(d->sws);
    av_frame_free(&d->frame);
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->ctx);
    free(d);
}
