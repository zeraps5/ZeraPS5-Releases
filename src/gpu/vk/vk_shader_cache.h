/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Persistent shader cache (vk_shader_cache.c): SPIR-V translations and the
 * driver's pipeline cache, kept per game between runs. */
#ifndef PS5C_VK_SHADER_CACHE_H
#define PS5C_VK_SHADER_CACHE_H

#include "gpu/shader/shader.h"
#include "gpu/vk/vk_internal.h"

/* vk_renderer.c: the device's translation variant (Inf/NaN preserving code) */
void vk_shader_cache_device(bool preserve_inf_nan, bool buffer_atomic64, bool lds_atomic64, bool pixel_interlock);
/* Loads the game's cache files once the device exists; closes them (saving the pipeline cache). */
void vk_shader_cache_open(renderer *r);
void vk_shader_cache_close(renderer *r);
/* For vkCreate*Pipelines (VK_NULL_HANDLE when the cache is off). */
VkPipelineCache vk_shader_cache_pipelines(void);
/* Around every vkCreate*Pipelines with that cache: the save waits for them
 * (Intel's driver crashed reading the cache while a pipeline was created) */
void vk_shader_cache_use_begin(void);
void vk_shader_cache_use_end(void);

/* Key of a translation: every input sh_translate's output depends on. */
uint64_t vk_shader_cache_key(const sh_program *p, const sh_stage_info *info, const sh_site_shape *shapes,
                             uint32_t binding_base);
/* A stored translation (malloc'ed, the caller frees it), or false. Lookup does
 * the same without counting it in the hit/miss statistics (pipeline builds). */
bool vk_shader_cache_find(uint64_t key, uint32_t **spirv, size_t *words);
bool vk_shader_cache_lookup(uint64_t key, uint32_t **spirv, size_t *words);
/* Source bytes for prewarm admission; zero if no usable cached translation. */
uint64_t vk_shader_cache_size(uint64_t key);
typedef struct vk_shader_cache_usage {
    uint64_t resident_bytes, resident_limit, indexed_entries;
} vk_shader_cache_usage;
/* Diagnostic counters, excluding caller-owned copies and driver allocations. */
vk_shader_cache_usage vk_shader_cache_memory(void);
/* <user dir>/cache, or NULL when the cache is off; the game's user directory ("" if unknown). */
const char *vk_shader_cache_dir(void);
const char *vk_shader_cache_game_dir(void);
void vk_shader_cache_store(uint64_t key, const uint32_t *spirv, size_t words);

/* Time spent translating (a miss) and creating a pipeline (driver compile). */
void vk_shader_cache_note_translate(uint64_t ns);
void vk_shader_cache_note_pipeline(uint64_t ns);
/* At each present: saves the pipeline cache once new pipelines have settled. */
void vk_shader_cache_tick(void);

#endif
