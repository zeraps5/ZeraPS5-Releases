/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VideoOut services for other HLE modules (the GPU flips through here). */
#ifndef PS5C_VIDEO_OUT_H
#define PS5C_VIDEO_OUT_H

#include "base/base.h"

/* Queues a flip of buffer `index` (-1 = blank) on port `handle`; performed at
 * the next vblank. Returns 0 or an SCE VideoOut error. */
int32_t video_out_submit_flip(int32_t handle, int32_t index, int32_t mode, int64_t arg);

/* The same for a flip the GPU performs: while the queue is full it waits (at most timeout_ns) for a vblank to
 * free a slot, as the hardware stalls the command processor, instead of dropping the frame. */
int32_t video_out_submit_flip_wait(int32_t handle, int32_t index, int32_t mode, int64_t arg, uint64_t timeout_ns);

/* Waits (at most timeout_ns) until buffer `index` of port `handle` is neither queued for a flip nor being
 * presented: the GPU may then draw into it. */
void video_out_wait_buffer(int32_t handle, int32_t index, uint64_t timeout_ns);

/* The host renderer, once the game opened its display (NULL before or without one). */
struct renderer *video_out_renderer(void);

/* Buffers flipped so far (debugging aids pace themselves by it). */
uint64_t video_out_flip_count(void);

/* Names the emulator in the game window's title ("ZeraPS5 0.1.0 build 77"). */
void video_out_set_caption(const char *emulator);

/* Opens the game window and plays the intro video (data/intro.mp4) in it, before the game runs;
 * [geral] abertura=0 or PS5C_NO_INTRO turn it off. */
struct ps5_runtime;
void video_out_intro(struct ps5_runtime *rt);

/* Ends the system splash screen (sceSystemServiceHideSplashScreen). */
void video_out_hide_splash(void);

#endif
