/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_SHADER_DYNAMIC_H
#define PS5C_SHADER_DYNAMIC_H
#include "gpu/shader/shader.h"

#define SH_DYNAMIC_MAX_CHOICES 2048u
#define SH_DYNAMIC_MAX_TABLE_BYTES (2u << 20)
#define SH_DYNAMIC_MAX_RANGES 64u

typedef struct sh_dynamic_range {
    guest_addr base;
    uint32_t size, offset; /* packed byte offset in the read-only binding */
} sh_dynamic_range;
typedef struct sh_dynamic_span {
    guest_addr base;
    uint64_t size;
    uint8_t *snapshot; /* scalar inputs/table: immutable for this dispatch */
    bool store;
    sh_dynamic_range *ranges; /* actual readonly descriptor choices, never holes */
    uint32_t range_count, packed_size;
} sh_dynamic_span;

typedef struct sh_dynamic_plan {
    sh_dynamic_span spans[SH_MAX_SITES];
    sh_site_shape shapes[SH_MAX_SITES];
    sh_site_value images[SH_MAX_SITES]; /* image/sampler proven invariant over all object choices */
    uint32_t *uniforms, words;
    uint32_t choices;
    char reason[192];
} sh_dynamic_plan;

/* Restricted, checked finite-table lowering. Caller pins mappings and finishes
 * earlier GPU writes first. On rejection nothing may be submitted with this plan.
 * Does not infer resources by scanning descriptor-looking bytes. */
bool sh_dynamic_plan_build(const sh_program *p, const sh_eval *initial, const sh_stage_info *info,
                           gmem *mem, sh_dynamic_plan *out);
void sh_dynamic_plan_free(sh_dynamic_plan *plan);
#endif
