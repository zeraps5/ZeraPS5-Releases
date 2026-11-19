/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Guest sound streams mixed onto the host output (pal_audio, 48 kHz stereo). */
#ifndef PS5C_HLE_AUDIO_MIX_H
#define PS5C_HLE_AUDIO_MIX_H

#include <stdbool.h>
#include <stdint.h>

/* A new stream: its id, or -1 when every stream is in use. The first one
 * starts the host output. */
int audio_mix_open(void);
void audio_mix_close(int id);

/* Appends `frames` frames of `channels` interleaved samples at 48 kHz (S16 or
 * float); more than two channels fold into stereo. */
void audio_mix_write(int id, const void *buf, uint32_t frames, uint32_t channels, bool is_float, float vol_l,
                     float vol_r);

/* Frames producers keep queued ahead of the host device (43 ms). */
#define AUDIO_QUEUE_TARGET 2048u

/* Frames a stream holds that the host has not played yet. */
uint32_t audio_mix_queued(int id);
/* The host output runs: producers pace themselves on audio_mix_queued. */
bool audio_mix_active(void);

#endif
