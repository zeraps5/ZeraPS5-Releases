/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host renderer: executes GPU work with the host graphics API and presents
 * VideoOut buffers in the window. One implementation today: Vulkan. */
#ifndef PS5C_RENDERER_H
#define PS5C_RENDERER_H

#include "base/base.h"
#include "gpu/gpu.h"
#include "mem/gmem.h"
#include "pal/pal.h"

typedef struct renderer renderer;

/* NULL when no usable host GPU is found (callers fall back to the software presenter). */
renderer *renderer_create(pal_window *window, gmem *mem);
void renderer_destroy(renderer *r);

/* Persistent shader cache (vk_shader_cache.c): the game's user directory
 * (user/<game folder>), set before renderer_create; and a save of what is not
 * written yet, before the process exits without renderer_destroy. */
void renderer_set_cache_dir(const char *game_user_dir);
void renderer_save_caches(renderer *r);

/* The host GPUs, in the order the settings number them ([video] placa takes
 * a name or a number; auto picks a dedicated one). Returns how many (at most
 * max are stored), 0 without Vulkan. */
typedef struct renderer_gpu {
    char name[256];
    bool discrete;
} renderer_gpu;
uint32_t renderer_list_gpus(renderer_gpu *out, uint32_t max);

/* Guest image formats for presentation */
typedef struct renderer_surface {
    guest_addr addr;
    uint32_t width, height;
    uint32_t pitch;         /* in pixels */
    uint64_t pixel_format;  /* VideoOut pixel format */
    uint32_t tiling;        /* VideoOut tiling mode */
} renderer_surface;

/* Shows `s` in the window (letterboxed). Called by VideoOut at flip time. */
void renderer_present(renderer *r, const renderer_surface *s);

/* The system splash screen (the package's sce_sys/pic1.dds, BC7): shown in
 * place of the game's frames from boot until the game hides it. */
bool renderer_set_splash(renderer *r, const void *bc7, size_t size, uint32_t width, uint32_t height, bool srgb);
void renderer_hide_splash(renderer *r);

/* A picture (top-down BGRA, width x height) presented now, letterboxed, in place of the game's
 * frames and the splash screen until renderer_hide_picture (the intro video). Every picture
 * has the size of the first. */
bool renderer_show_picture(renderer *r, const void *bgra, uint32_t width, uint32_t height);
void renderer_hide_picture(renderer *r);

/* GPU work, called by the command processor with its register state */
void renderer_draw(renderer *r, const gpu *g, const gpu_draw *d);
void renderer_dispatch(renderer *r, const gpu *g, uint32_t x, uint32_t y, uint32_t z);
/* The command processor wrote guest memory (depth metadata clears are seen here). */
void renderer_mem_written(renderer *r, guest_addr addr, uint64_t size);
/* Makes GPU results for [addr, addr + size) visible in guest memory (waits for the work that holds them). */
void renderer_sync_mem(renderer *r, guest_addr addr, uint64_t size);
/* End of pipe: true when the renderer holds the release until the work ahead of it completes (gpu_callbacks). */
bool renderer_release(renderer *r, gpu *g, const gpu_release *rel);
/* A guest command buffer starts (the CPU may have written memory since the last one). */
void renderer_submit_begin(renderer *r);
/* The command processor has no more work queued: held releases complete. */
void renderer_gpu_idle(renderer *r);
/* Explicit guest suspension: submit and finish all preceding host work. */
void renderer_wait_idle(renderer *r);
/* Guest SuspendPoint, after its commands were recorded: guest memory they read
 * is already copied, so only results still owed to guest memory (writebacks,
 * held labels) make it wait for the host GPU. */
void renderer_suspend_point(renderer *r);
/* The command processor writes guest memory now: held releases complete first. */
void renderer_cp_write(renderer *r);
/* Opt-in binding feedback; disabled renderers return false without changing memory. */
bool renderer_lod_stats(renderer *r,void *out,uint32_t bytes,uint32_t flags);
bool renderer_sample_counter(renderer *r,uint64_t *value);
/* A notification (a trophy) in the corner of the presented image for `ms` milliseconds. */
void renderer_notify(renderer *r, const char *text, uint32_t ms);

#endif
