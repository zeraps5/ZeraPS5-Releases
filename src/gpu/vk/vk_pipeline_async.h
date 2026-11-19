/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pipelines built in the background (vk_pipeline_async.c): the pipelines a
 * game used in earlier sessions are built by worker threads while it starts,
 * and each pipeline is built once per session, by content. */
#ifndef PS5C_VK_PIPELINE_ASYNC_H
#define PS5C_VK_PIPELINE_ASYNC_H

#include "gpu/shader/shader.h"
#include "gpu/vk/vk_internal.h"

typedef struct vk_built_pipeline {
    VkPipeline pipe;
    VkPipelineLayout layout;
    VkDescriptorSetLayout dsl;
    bool update_after_bind; /* dsl needs an update-after-bind pool (past the ordinary limits) */
    bool push; /* dsl is a push-descriptor layout: no set is allocated */
} vk_built_pipeline;

/* vk_pipeline.c: a pipeline from a recipe (the inputs vk_pipeline.c puts in
 * one), callable from any thread. The SPIR-V of stage i is words[i] when given,
 * else the shader cache's. */
size_t vk_pipeline_recipe_size(void);
/* Admission cost without copying SPIR-V; false if a required stage is absent. */
bool vk_pipeline_prewarm_size(const void *recipe, uint64_t *bytes);
bool vk_pipeline_build(renderer *r, const void *recipe, const uint32_t *const words[2], const size_t n[2],
                       vk_built_pipeline *out);
/* Binding types of a stage, as a recipe lists them (types: SH_MAX_SITES + 1 bytes). */
uint32_t vk_pipeline_binding_types(const sh_program *p, const sh_site_shape *shapes, uint8_t *types);
/* An earlier translator's recipe with its shaders' new keys (vk_shader_cache_migrated);
 * false when it cannot be carried over. */
bool vk_pipeline_recipe_migrate(void *recipe);

/* After vk_shader_cache_open: loads the game's pipeline list and starts the
 * workers (unless PS5C_PIPELINES_ASYNC=0 or the shader cache is off). */
void vk_pipeline_async_start(renderer *r);
/* Before vk_pipeline_shutdown: true after all workers joined and pipelines were
 * destroyed. False after the bounded wait: the renderer/device/cache must remain
 * alive, and a new session must not overwrite this one. A later call may drain. */
bool vk_pipeline_async_stop(renderer *r);
bool vk_pipeline_async_on(void);

/* The pipeline of a recipe (known by a hash of it): built earlier (by a worker
 * or another draw), or handed to the workers and waited for up to the budget.
 * *shared: it stays owned here (vk_pipeline_async_stop destroys it); false
 * when the table could not take it, and the caller owns it. *pending: false
 * was returned because the build goes on in the background (ask again later).
 * must_complete bypasses the wait budget: a compute dispatch cannot be dropped
 * merely because its pipeline is still building. */
bool vk_pipeline_async_get(renderer *r, const void *recipe, const uint32_t *const words[2], const size_t n[2],
                           vk_built_pipeline *out, bool *shared, bool *pending, bool must_complete);

/* Statistics, also with PS5C_PIPELINES_ASYNC=0: time a draw or dispatch waited
 * for its pipeline, and each present (pauses logged every 10 s). */
void vk_pipeline_async_note_wait(uint64_t ns, bool built_here);
/* Builds not finished: of the earlier sessions' list (*done of *total) and the
 * ones draws asked for (*draws, queued or in the driver). False when none. */
bool vk_pipeline_async_progress(uint32_t *done, uint32_t *total, uint32_t *draws);
void vk_pipeline_async_frame(void);

#endif
