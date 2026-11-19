/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pal_media: each movie goes to the first decoder that opens it (FFmpeg,
 * then Media Foundation on Windows); without either, movies are skipped. */
#include <stdlib.h>

#include "pal/pal.h"
#include "pal/pal_exfat.h"
#include "pal/pal_media_backends.h"

typedef enum { BACKEND_FFMPEG, BACKEND_MF } backend;

struct pal_media {
    backend kind;
    void *impl;
};

static pal_media *wrap(backend kind, void *impl) {
    if (!impl) return NULL;
    pal_media *m = (pal_media *)malloc(sizeof(pal_media));
    if (!m) {
#ifdef PS5C_HAVE_FFMPEG
        if (kind == BACKEND_FFMPEG) ff_media_close((ff_media *)impl);
#endif
#ifdef _WIN32
        if (kind == BACKEND_MF) mf_media_close((mf_media *)impl);
#endif
        return NULL;
    }
    m->kind = kind;
    m->impl = impl;
    return m;
}

pal_media *pal_media_open_file(const char *path) {
    pal_media *m = NULL;
#ifdef PS5C_HAVE_FFMPEG
    m = wrap(BACKEND_FFMPEG, ff_media_open_file(path));
#endif
#ifdef _WIN32
    if (!m && exfat_path(path)) {
        pal_path_info pi;
        uint8_t *data = NULL; uint64_t size = 0;
        if (pal_path_stat(path, &pi) == PS5C_OK && pi.size <= (256u << 20) &&
            pal_file_read_all(path, &data, &size) == PS5C_OK) {
            m = wrap(BACKEND_MF, mf_media_open_memory(data, size));
            free(data);
        }
    } else if (!m) m = wrap(BACKEND_MF, mf_media_open_file(path));
#endif
    (void)path;
    return m;
}

pal_media *pal_media_open_memory(const void *data, uint64_t size) {
    pal_media *m = NULL;
#ifdef PS5C_HAVE_FFMPEG
    m = wrap(BACKEND_FFMPEG, ff_media_open_memory(data, size));
#endif
#ifdef _WIN32
    if (!m) m = wrap(BACKEND_MF, mf_media_open_memory(data, size));
#endif
    (void)data;
    (void)size;
    return m;
}

#if defined(PS5C_HAVE_FFMPEG) && defined(_WIN32)
#define DISPATCH(call_ff, call_mf) return m->kind == BACKEND_FFMPEG ? call_ff : call_mf
#elif defined(PS5C_HAVE_FFMPEG)
#define DISPATCH(call_ff, call_mf) return call_ff
#elif defined(_WIN32)
#define DISPATCH(call_ff, call_mf) return call_mf
#else
#define DISPATCH(call_ff, call_mf) return 0
#endif

void pal_media_get_info(pal_media *m, pal_media_info *out) {
#ifdef PS5C_HAVE_FFMPEG
    if (m->kind == BACKEND_FFMPEG) ff_media_get_info((ff_media *)m->impl, out);
#endif
#ifdef _WIN32
    if (m->kind == BACKEND_MF) mf_media_get_info((mf_media *)m->impl, out);
#endif
    (void)m;
    (void)out;
}

int pal_media_next_video(pal_media *m, uint8_t *nv12, uint64_t *time_us) {
    DISPATCH(ff_media_next_video((ff_media *)m->impl, nv12, time_us), mf_media_next_video((mf_media *)m->impl, nv12, time_us));
}

int pal_media_next_audio(pal_media *m, int16_t *pcm, uint32_t max_frames, uint64_t *time_us) {
    DISPATCH(ff_media_next_audio((ff_media *)m->impl, pcm, max_frames, time_us),
             mf_media_next_audio((mf_media *)m->impl, pcm, max_frames, time_us));
}

bool pal_media_rewind(pal_media *m) {
    DISPATCH(ff_media_rewind((ff_media *)m->impl), mf_media_rewind((mf_media *)m->impl));
}

void pal_media_close(pal_media *m) {
    if (!m) return;
#ifdef PS5C_HAVE_FFMPEG
    if (m->kind == BACKEND_FFMPEG) ff_media_close((ff_media *)m->impl);
#endif
#ifdef _WIN32
    if (m->kind == BACKEND_MF) mf_media_close((mf_media *)m->impl);
#endif
    free(m);
}

#ifndef PS5C_HAVE_FFMPEG
/* Elementary-stream video decoding needs FFmpeg: without it games see no decoder. */
pal_vdec *pal_vdec_open(pal_vdec_codec codec) {
    (void)codec;
    return NULL;
}
pal_vdec *pal_vdec_open_ex(pal_vdec_codec codec,bool apply_cropping) {
    (void)codec;(void)apply_cropping;return NULL;
}
int pal_vdec_decode(pal_vdec *d, const void *au, uint64_t size, uint64_t pts, uint64_t dts, uint64_t attached,
                    uint8_t *out, uint64_t cap, pal_vdec_picture *pic) {
    (void)d, (void)au, (void)size, (void)pts, (void)dts, (void)attached, (void)out, (void)cap, (void)pic;
    return -1;
}
int pal_vdec_drain(pal_vdec *d,uint8_t *out,uint64_t cap,pal_vdec_picture *pic) {
    (void)d;(void)out;(void)cap;(void)pic;return 0;
}
int pal_vdec_flush(pal_vdec *d, uint8_t *out, uint64_t cap, pal_vdec_picture *pic) {
    (void)d, (void)out, (void)cap, (void)pic;
    return 0;
}
void pal_vdec_reset(pal_vdec *d) { (void)d; }
void pal_vdec_close(pal_vdec *d) { (void)d; }
#endif
