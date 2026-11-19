/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Conservative prefix bounds for straight-line vertex buffer reads.
 * Unknown values are UINT32_MAX. This only reduces copies; it does not
 * change shader descriptors, addressing, or the pipeline key. The scalar
 * offset comes from the evaluation when it knows it (fetch shaders compute
 * it from a format word in memory), else from the instructions. */
#ifndef PS5C_VK_VERTEX_RANGES_H
#define PS5C_VK_VERTEX_RANGES_H

#include "gpu/shader/shader.h"
#include "gpu/shader/shader_ops.h"
#include <string.h>

/* The shader sees the index plus GE_INDX_OFFSET. Without a minimum index,
 * negative offsets may wrap: retain the full descriptor in that case. */
static inline uint32_t vr_vertex_max(uint32_t index_max, int32_t base_vertex) {
    if (base_vertex < 0 || index_max > UINT32_MAX - (uint32_t)base_vertex) return UINT32_MAX;
    return index_max + (uint32_t)base_vertex;
}

/* Indirect/relative destinations and loops need a CFG analysis (the flags come
 * from sh_program_flags). Forward branches are fine: every path runs a subset of
 * the instructions in program order, so a register no earlier instruction writes
 * still holds its input, and a write found scanning back from a use reaches it
 * when no branch lands in between (vr_joins_between). No Man's Sky's NGG vertex
 * shaders branch around their primitive export and copied 8 MiB per draw. */
static bool vr_straight_line(const sh_program *p) {
    if (p->has_stores || p->back_branch) return false;
    for (uint32_t i = 0; p->has_flow && i < p->count; i++) {
        const char *n = p->insts[i].name;
        if (n && (strstr(n, "setpc") || strstr(n, "swappc") || strstr(n, "movrel"))) return false;
    }
    return true;
}

/* Renderer-owned metadata only. The owner must invalidate before reanalyzing
 * or freeing ANY program, including fused programs: pointer reuse is not an
 * identity guarantee. Dynamic SGPRs, indices and buffer contents are not cached. */
typedef struct vr_flow_cache {
    struct { const sh_program *program; bool straight; } entries[64];
} vr_flow_cache;

static inline void vr_flow_invalidate(vr_flow_cache *cache) {
    memset(cache, 0, sizeof(*cache));
}

static inline bool vr_flow_cached(vr_flow_cache *cache, const sh_program *p) {
    size_t slot = ((uintptr_t)p >> 4) % 64;
    if (cache->entries[slot].program != p) {
        cache->entries[slot].straight = vr_straight_line(p);
        cache->entries[slot].program = p;
    }
    return cache->entries[slot].straight;
}

/* A branch target in (def, use]: another path reaches the use without the write at def. */
static bool vr_joins_between(const sh_program *p, uint32_t def, uint32_t use) {
    for (uint32_t j = 0; p->has_flow && j < p->count; j++)
        if (p->target[j] < p->count && p->target[j] > def && p->target[j] <= use) return true;
    return false;
}

static uint32_t vr_bound(const sh_program *p, uint32_t before, uint32_t reg,
                         const sh_stage_info *info, uint32_t vertex_max,
                         uint32_t instance_max, uint32_t depth) {
    if (depth > 16) return UINT32_MAX;
    if (reg == RDNA2_OP_EXEC_LO || reg == RDNA2_OP_EXEC_HI || reg == RDNA2_OP_M0 || reg == RDNA2_OP_NULL ||
        reg == RDNA2_OP_SCC || reg == RDNA2_OP_VCCZ || reg == RDNA2_OP_EXECZ) return UINT32_MAX;
    uint32_t literal;
    if (reg >= 128 && reg < 256) {
        /* Literal operands need their instruction's literal, handled below. */
        return reg != RDNA2_OP_LITERAL && sh_inline_const(reg, 0, &literal) ? literal : UINT32_MAX;
    }
    for (uint32_t i = before; i-- > 0;) {
        const rdna2_inst *in = &p->insts[i];
        sh_op op = (sh_op)p->ops[i];
        if (in->enc == RDNA2_ENC_SMEM) {
            /* Overestimating the overwritten span is safe. */
            if (reg < 128 && reg >= in->dst && reg < in->dst + 16u) return UINT32_MAX;
            continue;
        }
        if (in->enc == RDNA2_ENC_MUBUF || in->enc == RDNA2_ENC_MTBUF ||
            in->enc == RDNA2_ENC_MIMG || in->enc == RDNA2_ENC_FLAT || in->enc == RDNA2_ENC_DS) {
            /* the loads' own width when the name gives it (sh_program_flags), else a wide bound */
            if (reg >= 256 && reg - 256 >= in->vdata && reg - 256 < in->vdata + (uint32_t)p->vmem_width[i]) return UINT32_MAX;
            continue;
        }
        bool scalar = in->enc == RDNA2_ENC_SOP1 || in->enc == RDNA2_ENC_SOP2 || in->enc == RDNA2_ENC_SOPK;
        bool vector = in->enc == RDNA2_ENC_VOP1 || in->enc == RDNA2_ENC_VOP2 ||
                      in->enc == RDNA2_ENC_VOP3 || in->enc == RDNA2_ENC_VOP3P || in->enc == RDNA2_ENC_VINTRP;
        if (reg < 128 && vector && (reg == in->sdst || reg == in->sdst + 1u)) return UINT32_MAX;
        if (reg == RDNA2_OP_VCC_LO || reg == RDNA2_OP_VCC_HI ||
            reg == RDNA2_OP_EXEC_LO || reg == RDNA2_OP_EXEC_HI || reg == RDNA2_OP_M0) {
            /* Vector compares, saveexec and carry instructions have implicit writes (s_cmp only writes SCC). */
            if (in->enc == RDNA2_ENC_VOPC || (vector && (reg == in->sdst || (p->iflags[i] & SH_IF_CARRY)))) return UINT32_MAX;
        }
        if (!scalar && !vector) continue;
        uint32_t width = op == SH_OP_V_CMP || op == SH_OP_V_CMPX || (p->iflags[i] & SH_IF_64) ? 2 : 1;
        if (reg < in->dst || reg >= in->dst + width) continue;
        if (vr_joins_between(p, i, before)) return UINT32_MAX;
        if (reg != in->dst || in->abs || in->neg || in->clamp || in->omod || in->opsel ||
            in->dpp_sdwa) return UINT32_MAX;
        switch (op) {
        case SH_OP_S_MOV_B32: case SH_OP_V_MOV_B32: case SH_OP_S_MOVK_I32:
        case SH_OP_V_CNDMASK_B32: case SH_OP_S_CSELECT_B32:
        case SH_OP_S_AND_B32: case SH_OP_V_AND_B32:
        case SH_OP_S_BFE_U32: case SH_OP_S_LSHL_B32: case SH_OP_S_LSHR_B32: break;
        default: return UINT32_MAX;
        }
        if (reg >= 256) {
            /* Vector writes preserve inactive lanes. Every lane active at
             * the consuming instruction must have been active at this write. */
            for (uint32_t k = i + 1; k < before; k++) {
                const rdna2_inst *later = &p->insts[k];
                if (later->dst == RDNA2_OP_EXEC_LO || later->dst == RDNA2_OP_EXEC_HI ||
                    p->ops[k] == SH_OP_V_CMPX || (p->iflags[k] & SH_IF_SAVEEXEC))
                    return UINT32_MAX;
            }
        }
        uint32_t a = in->src[0] == RDNA2_OP_LITERAL ? in->literal :
            vr_bound(p, i, in->src[0], info, vertex_max, instance_max, depth + 1);
        uint32_t b = in->src[1] == RDNA2_OP_LITERAL ? in->literal :
            vr_bound(p, i, in->src[1], info, vertex_max, instance_max, depth + 1);
        uint32_t c;
        bool b_const = sh_inline_const(in->src[1], in->literal, &c);
        switch (op) {
        case SH_OP_S_MOV_B32: case SH_OP_V_MOV_B32: return a;
        case SH_OP_S_MOVK_I32: return (uint32_t)in->simm16;
        case SH_OP_V_CNDMASK_B32: case SH_OP_S_CSELECT_B32: return a > b ? a : b;
        case SH_OP_S_AND_B32: case SH_OP_V_AND_B32: return a < b ? a : b;
        case SH_OP_S_BFE_U32: {
            if (!b_const) return UINT32_MAX;
            uint32_t w = (c >> 16) & 0x7F, shift = c & 31;
            if (!w) return 0;
            if (w > 32 - shift) return UINT32_MAX;
            return w == 32 ? UINT32_MAX : (1u << w) - 1;
        }
        case SH_OP_S_LSHL_B32:
            if (b_const && a <= (UINT32_MAX >> (c & 31))) return a << (c & 31);
            return UINT32_MAX;
        case SH_OP_S_LSHR_B32:
            return b_const ? a >> (c & 31) : UINT32_MAX;
        default: return UINT32_MAX;
        }
    }
    if (reg == 256u + info->vertex_id_vgpr) return vertex_max;
    if (reg == 256u + info->instance_id_vgpr) return instance_max;
    return UINT32_MAX;
}

static uint64_t vr_site_prefix(const sh_program *p, uint32_t site, const sh_stage_info *info, const sh_site_value *v,
                               uint32_t vertex_max, uint32_t instance_max, uint32_t stride) {
    const rdna2_inst *in = &p->insts[p->sites[site].inst];
    sh_op op = (sh_op)p->ops[p->sites[site].inst];
    if (p->sites[site].store || !stride || !in->idxen || in->offen || in->offset < 0 ||
        (in->enc != RDNA2_ENC_MUBUF && in->enc != RDNA2_ENC_MTBUF) ||
        (op != SH_OP_BUFFER_LOAD_FORMAT && op != SH_OP_TBUFFER_LOAD_FORMAT &&
         op != SH_OP_BUFFER_LOAD_DWORD && op != SH_OP_BUFFER_LOAD_UBYTE && op != SH_OP_BUFFER_LOAD_USHORT))
        return UINT64_MAX;
    uint32_t idx = vr_bound(p, p->sites[site].inst, 256u + in->vaddr, info, vertex_max, instance_max, 0);
    uint32_t soff = v && v->soffset_known ? v->soffset
                  : in->soffset == RDNA2_OP_LITERAL ? in->literal
                  : vr_bound(p, p->sites[site].inst, in->soffset, info, vertex_max, instance_max, 0);
    if (idx == UINT32_MAX || soff == UINT32_MAX) return UINT64_MAX;
    /* All supported loads consume <=16 bytes. Keep 64 to include dword
     * rounding and padding without relying on a specific vertex format. */
    uint64_t end = (uint64_t)idx * stride + soff + (uint32_t)in->offset + 64;
    return end > UINT32_MAX ? UINT64_MAX : end;
}
#endif
