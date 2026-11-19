/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_VK_DEPTH_POLICY_H
#define PS5C_VK_DEPTH_POLICY_H

#include "gpu/shader/shader.h"
#include <math.h>
#include <string.h>

/* RADV's masked HTILE fill preserves stencil while replacing every depth
 * field. Recognize the complete straight-line load/AND/OR/store, not merely
 * a write to metadata: conditional expansion and stencil-only updates must
 * never erase already rendered depth. The caller restricts this Z/S layout
 * to a surface whose HTILE also carries stencil. */
static inline bool vk_htile_masked_depth_clear(const sh_program *p, const sh_eval *e, const sh_stage_info *info) {
    if (!p || !e || !info || p->count != 10 || info->cs_tgid_sgpr > 123 ||
        info->local_size[0] != 64 || info->local_size[1] != 1 || info->local_size[2] != 1 ||
        info->cs_tgid_enable != 1 || info->cs_tid_components != 1) return false;
    const rdna2_inst *in = p->insts, *ld = in + 2, *st = in + 8;
    uint32_t user = info->cs_tgid_sgpr;
    if (strcmp(in[0].name, "v_lshl_add_u32") || in[0].dst != 256 ||
        in[0].src[0] != user || in[0].src[1] != 134 || in[0].src[2] != 256 ||
        strncmp(in[1].name, "v_lshlrev_b32", 13) || in[1].dst != 256 ||
        in[1].src[0] != 132 || in[1].src[1] != 256 ||
        strcmp(ld->name, "global_load_dwordx4") || ld->seg != 2 || ld->vaddr || ld->offset ||
        ld->sbase + 1u >= user || strcmp(in[3].name, "s_waitcnt") ||
        strcmp(st->name, "global_store_dwordx4") || st->seg != 2 || st->vaddr || st->offset ||
        st->sbase != ld->sbase || strcmp(in[9].name, "s_endpgm")) return false;
    uint32_t mask_reg = in[4].src[0], value_reg = in[4].src[2];
    if (mask_reg >= user || value_reg >= user) return false;
    /* ZMASK is in [3:0], ZDELTA/ZBASE in [31:12]. The intervening
     * stencil, reserved and VRS bits can be preserved. */
    uint32_t preserve = e->sgpr_init[mask_reg], value = e->sgpr_init[value_reg];
    if ((preserve & ~value & ~0xff0u) || (value & 15u)) return false;
    for (uint32_t k = 0; k < 4; k++) {
        const rdna2_inst *op = in + 4 + k;
        if (strcmp(op->name, "v_and_or_b32") || op->dst != st->vdata + 256u + k ||
            op->src[0] != mask_reg || op->src[1] != ld->dst + k || op->src[2] != value_reg ||
            op->abs || op->neg || op->opsel || op->omod || op->clamp || op->dpp_sdwa) return false;
    }
    for (uint32_t k = 0; k < 2; k++)
        if (in[k].abs || in[k].neg || in[k].opsel || in[k].omod || in[k].clamp || in[k].dpp_sdwa) return false;
    return true;
}

/* Conservatively recognize a constant post-divide plane. */
static inline bool vk_vertex_depth_is_constant(const sh_program *vs, const sh_program *ps) {
    if (!vs || !ps || (ps->mrt_mask & (1u << 8))) return false;
    bool position = false;
    for (uint32_t i = 0; i < vs->count; i++) {
        const rdna2_inst *in = &vs->insts[i];
        if (in->enc != RDNA2_ENC_EXP || in->exp_target != 12) continue;
        if (in->exp_compr || (in->exp_en & 12u) != 12u || in->exp_src[2] != in->exp_src[3]) return false;
        position = true;
    }
    return position;
}

/* D32's unit at depth 1 is 2^-23. Move an inward-biased far plane by the
 * same amount in clip space instead of letting fixed-function bias derive
 * its exponent from triangles crossing W=0. An inward move preserves the
 * original clipping domain. Other planes/formats and fragment depth keep
 * the ordinary rasterizer path. The slope of Z/W=1 is exactly zero. */
static inline bool vk_far_plane_bias(const sh_program *vs, const sh_program *ps,
                                     float scale, float offset, float constant, float clamp, float *delta) {
    if (scale != 1.0f || offset != 0.0f || constant > 0.0f || !isfinite(constant) || !isfinite(clamp) ||
        !vk_vertex_depth_is_constant(vs, ps)) return false;
    float d = constant * 0x1p-23f;
    if (clamp > 0.0f && d > clamp) d = clamp;
    if (clamp < 0.0f && d < clamp) d = clamp;
    if (d < -1.0f) return false;
    *delta = d;
    return true;
}

#endif
