/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The movie decoders behind pal_media (pal_media.c picks one per movie):
 * FFmpeg when the build has it (PS5C_HAVE_FFMPEG), then Media Foundation on
 * Windows. Each mirrors the pal_media calls. */
#ifndef PS5C_PAL_MEDIA_BACKENDS_H
#define PS5C_PAL_MEDIA_BACKENDS_H

#include "pal/pal.h"

typedef struct ff_media ff_media;
ff_media *ff_media_open_file(const char *path);
ff_media *ff_media_open_memory(const void *data, uint64_t size);
void ff_media_get_info(ff_media *m, pal_media_info *out);
int ff_media_next_video(ff_media *m, uint8_t *nv12, uint64_t *time_us);
int ff_media_next_audio(ff_media *m, int16_t *pcm, uint32_t max_frames, uint64_t *time_us);
bool ff_media_rewind(ff_media *m);
void ff_media_close(ff_media *m);

typedef struct mf_media mf_media;
mf_media *mf_media_open_file(const char *path);
mf_media *mf_media_open_memory(const void *data, uint64_t size);
void mf_media_get_info(mf_media *m, pal_media_info *out);
int mf_media_next_video(mf_media *m, uint8_t *nv12, uint64_t *time_us);
int mf_media_next_audio(mf_media *m, int16_t *pcm, uint32_t max_frames, uint64_t *time_us);
bool mf_media_rewind(mf_media *m);
void mf_media_close(mf_media *m);

#endif
