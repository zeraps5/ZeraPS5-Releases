/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_SHADER_BARRIER_H
#define PS5C_SHADER_BARRIER_H
#include <string.h>
#include "gpu/shader/shader.h"
#include "gpu/shader/shader_ops.h"

/* A deliberately narrow convergence proof for structured, forward-only CS.
 * Scalar registers in the lane interpreter can contain lane-dependent values;
 * calling every SCC branch uniform would deadlock a workgroup. Only compare
 * immutable values established before the first branch. Branches inside an
 * LDS phase may diverge provided they reconverge before the barrier. */
static bool sh_barrier_branch(unsigned op) {
    return op == SH_OP_S_BRANCH || op == SH_OP_S_CBRANCH_SCC0 || op == SH_OP_S_CBRANCH_SCC1 ||
           op == SH_OP_S_CBRANCH_VCCZ || op == SH_OP_S_CBRANCH_VCCNZ ||
           op == SH_OP_S_CBRANCH_EXECZ || op == SH_OP_S_CBRANCH_EXECNZ;
}
/* A completed LDS batch must be ordered across lanes too: host compilers may
 * otherwise move a following LDS write ahead of a neighbour's read. Preserve
 * the guest wave's wait with a subgroup barrier, not a CPU/queue wait. Partial
 * counters do not complete a batch. Independent ALU and partial waits between
 * the LDS access and its completed wait must not discard the dependency. */
static bool sh_barrier_lgkm_done(unsigned op, const rdna2_inst *in) {
    unsigned imm = (unsigned)in->simm16;
    return (op == SH_OP_S_WAITCNT && !(imm & 0x3f00)) ||
           (op == SH_OP_S_WAITCNT_LGKMCNT && imm == 0);
}
static bool sh_barrier_lds_wait(const sh_program *p, unsigned i) {
    if (!sh_barrier_lgkm_done(p->ops[i], &p->insts[i])) return false;
    while (i) {
        --i;
        if (p->ops[i] == SH_OP_S_BARRIER || sh_barrier_lgkm_done(p->ops[i], &p->insts[i])) return false;
        if (p->insts[i].enc == RDNA2_ENC_DS && !p->insts[i].gds) return true;
    }
    return false;
}
static unsigned sh_barrier_smem_width(unsigned op) {
    switch (op) {
    case SH_OP_S_LOAD_DWORDX2: case SH_OP_S_BUFFER_LOAD_DWORDX2: return 2;
    case SH_OP_S_LOAD_DWORDX4: case SH_OP_S_BUFFER_LOAD_DWORDX4: return 4;
    case SH_OP_S_LOAD_DWORDX8: case SH_OP_S_BUFFER_LOAD_DWORDX8: return 8;
    case SH_OP_S_LOAD_DWORDX16: case SH_OP_S_BUFFER_LOAD_DWORDX16: return 16;
    default: return 1;
    }
}
static bool sh_barrier_const(unsigned src) {
    uint32_t value;
    return src == RDNA2_OP_NULL || sh_inline_const(src, 0, &value);
}
static bool sh_barrier_uniform_src(const bool uniform[106], unsigned src) {
    return src < 106 ? uniform[src] : sh_barrier_const(src);
}
static unsigned sh_barrier_dst_width(const sh_program *p, unsigned i) {
    const rdna2_inst *in = &p->insts[i];
    if (in->enc == RDNA2_ENC_SMEM) return sh_barrier_smem_width(p->ops[i]);
    if (in->enc == RDNA2_ENC_VOP3 && in->valu < 0x100) return 2; /* e64 compare mask in dst */
    if (in->enc == RDNA2_ENC_SOP1 || in->enc == RDNA2_ENC_SOP2 ||
        in->enc == RDNA2_ENC_SOPK || in->enc == RDNA2_ENC_VOP1 ||
        p->ops[i] == SH_OP_V_READFIRSTLANE_B32 || p->ops[i] == SH_OP_V_READLANE_B32)
        return (p->iflags[i] & SH_IF_64) ? 2 : 1;
    return 0;
}
static void sh_barrier_kill_mask(bool uniform[106], const sh_program *p, unsigned i) {
    const rdna2_inst *in = &p->insts[i];
    unsigned dst = RDNA2_OP_NULL, op = p->ops[i];
    if (in->enc == RDNA2_ENC_VOP3 && ((p->iflags[i] & SH_IF_CARRY) ||
        op == SH_OP_V_MAD_U64_U32 || op == SH_OP_V_MAD_I64_I32 ||
        op == SH_OP_V_DIV_SCALE_F32 || op == SH_OP_V_DIV_SCALE_F64)) dst = in->sdst;
    if (in->enc == RDNA2_ENC_VOPC && in->src[0] == RDNA2_OP_SDWA && (in->dpp_sdwa & (1u << 15)))
        dst = (in->dpp_sdwa >> 8) & 127;
    if (dst < 106) {
        uniform[dst] = false;
        if (dst + 1 < 106) uniform[dst + 1] = false;
    }
}
static bool sh_barrier_cmp32(unsigned op) {
    switch (op) {
    case SH_OP_S_CMP_EQ_I32: case SH_OP_S_CMP_LG_I32: case SH_OP_S_CMP_GT_I32:
    case SH_OP_S_CMP_GE_I32: case SH_OP_S_CMP_LT_I32: case SH_OP_S_CMP_LE_I32:
    case SH_OP_S_CMP_EQ_U32: case SH_OP_S_CMP_LG_U32: case SH_OP_S_CMP_GT_U32:
    case SH_OP_S_CMP_GE_U32: case SH_OP_S_CMP_LT_U32: case SH_OP_S_CMP_LE_U32: return true;
    default: return false;
    }
}
static bool sh_barrier_scc_uniform(const sh_program *p, unsigned branch, const bool immutable[106]) {
    /* Only instructions known not to change SCC may separate its compare. */
    unsigned at = branch;
    while (branch) {
        unsigned i = --branch, op = p->ops[i];
        if (op == SH_OP_S_MOV_B32 || op == SH_OP_S_MOV_B64 ||
            op == SH_OP_S_WAITCNT || op == SH_OP_S_NOP) continue;
        const rdna2_inst *in = &p->insts[i];
        /* A lane must not jump past the compare and reuse an older SCC. */
        for (unsigned j = 0; j < i; j++)
            if (sh_barrier_branch(p->ops[j]) && p->target[j] > i && p->target[j] <= at) return false;
        return sh_barrier_cmp32(op) && sh_barrier_uniform_src(immutable, in->src[0]) &&
               sh_barrier_uniform_src(immutable, in->src[1]);
    }
    return false;
}
static bool sh_barrier_dominates(const sh_program *p, unsigned def, unsigned use) {
    for (unsigned i = 0; i < def; i++)
        if (sh_barrier_branch(p->ops[i]) && p->target[i] > def && p->target[i] <= use) return false;
    return true;
}
static bool sh_barrier_writes_s(const sh_program *p, unsigned i, unsigned reg) {
    const rdna2_inst *in = &p->insts[i];
    unsigned width = sh_barrier_dst_width(p, i);
    if (reg >= in->dst && reg - in->dst < width) return true;
    if (in->enc == RDNA2_ENC_VOPC) {
        unsigned dst = p->ops[i] == SH_OP_V_CMPX ? RDNA2_OP_EXEC_LO : RDNA2_OP_VCC_LO;
        if (in->src[0] == RDNA2_OP_SDWA && (in->dpp_sdwa & (1u << 15))) dst = (in->dpp_sdwa >> 8) & 127;
        if (reg >= dst && reg - dst < 2) return true;
    }
    if (in->enc == RDNA2_ENC_VOP3 && p->ops[i] == SH_OP_V_CMPX &&
        reg >= RDNA2_OP_EXEC_LO) return true;
    if (in->enc == RDNA2_ENC_VOP3 && ((p->iflags[i] & SH_IF_CARRY) ||
        p->ops[i] == SH_OP_V_MAD_U64_U32 || p->ops[i] == SH_OP_V_MAD_I64_I32 ||
        p->ops[i] == SH_OP_V_DIV_SCALE_F32 || p->ops[i] == SH_OP_V_DIV_SCALE_F64))
        return reg >= in->sdst && reg - in->sdst < 2;
    return false;
}
/* Local scalar def/use proof for the broadcast idiom below. Unlike immutable
 * prefix values, this also permits a temporary in VCC_LO overwritten by SMEM. */
static bool sh_barrier_scalar_at(const sh_program *p, const sh_site_shape *shapes,
                                 unsigned use, unsigned reg, unsigned depth) {
    if (sh_barrier_const(reg)) return true;
    if (reg >= SH_SGPRS || depth > 12) return false;
    for (unsigned j = use; j;) {
        unsigned i = --j;
        if (!sh_barrier_writes_s(p, i, reg)) continue;
        if (!sh_barrier_dominates(p, i, use)) return false;
        const rdna2_inst *in = &p->insts[i]; unsigned op = p->ops[i];
        if (in->enc == RDNA2_ENC_SMEM && p->site_of[i] >= 0) {
            unsigned site = (unsigned)p->site_of[i];
            if (p->sites[site].kind == SH_SITE_SMEM && !shapes[site].address_map) return true;
            if (p->sites[site].kind != SH_SITE_SBUFFER) return false;
            for (unsigned s = 0; s < p->site_count; s++)
                if (p->sites[s].store && p->sites[s].kind != SH_SITE_IMAGE) return false;
            for (unsigned k = 0; k < 4; k++)
                if (!sh_barrier_scalar_at(p, shapes, i, in->sbase + k, depth + 1)) return false;
            return sh_barrier_scalar_at(p, shapes, i, in->soffset, depth + 1);
        }
        if (op == SH_OP_S_MOVK_I32) return true;
        if (op == SH_OP_S_MOV_B32 || op == SH_OP_S_MOV_B64) {
            unsigned src = in->src[0];
            if (!sh_barrier_const(src)) src += reg - in->dst;
            return sh_barrier_scalar_at(p, shapes, i, src, depth + 1);
        }
        if (op == SH_OP_S_ADD_U32 || op == SH_OP_S_ADD_I32 || op == SH_OP_S_SUB_U32 || op == SH_OP_S_SUB_I32 ||
            op == SH_OP_S_MUL_I32 || op == SH_OP_S_AND_B32 || op == SH_OP_S_OR_B32 ||
            op == SH_OP_S_XOR_B32 || op == SH_OP_S_LSHL_B32 || op == SH_OP_S_LSHR_B32)
            return sh_barrier_scalar_at(p, shapes, i, in->src[0], depth + 1) &&
                   sh_barrier_scalar_at(p, shapes, i, in->src[1], depth + 1);
        return false;
    }
    return reg < RDNA2_OP_VCC_LO; /* ordinary initial SGPRs are workgroup inputs */
}
static bool sh_barrier_broadcast_branch(const sh_program *p, const sh_site_shape *shapes,
                                         const bool *safe, unsigned branch) {
    if ((p->ops[branch] != SH_OP_S_CBRANCH_VCCZ && p->ops[branch] != SH_OP_S_CBRANCH_VCCNZ) || branch < 5)
        return false;
    unsigned cmp = branch - 1, wait = cmp - 1, read = wait - 1, addr = read - 1, barrier = addr - 1;
    const rdna2_inst *c = &p->insts[cmp], *rd = &p->insts[read], *a = &p->insts[addr];
    /* A single constant-address LDS broadcast immediately after a proven
     * workgroup barrier; no intervening writes can race this shared value. */
    if (p->ops[barrier] != SH_OP_S_BARRIER || !safe[barrier] ||
        !sh_barrier_dominates(p, barrier, branch) ||
        p->ops[addr] != SH_OP_V_MOV_B32 || !sh_barrier_const(a->src[0]) ||
        rd->enc != RDNA2_ENC_DS || rd->op != 0x36 || rd->gds ||
        a->dst != RDNA2_OP_VGPR + rd->vaddr || !sh_barrier_lgkm_done(p->ops[wait], &p->insts[wait]) ||
        p->ops[cmp] != SH_OP_V_CMP || c->enc != RDNA2_ENC_VOPC || (p->iflags[cmp] & SH_IF_64) ||
        c->src[0] == RDNA2_OP_SDWA || c->src[0] == RDNA2_OP_DPP16) return false;
    unsigned src = c->src[0] == rd->dst ? c->src[1] : c->src[1] == rd->dst ? c->src[0] : UINT32_MAX;
    if (!sh_barrier_scalar_at(p, shapes, cmp, src, 0)) return false;
    /* A compare also depends on EXEC. All lanes must be active, not just the
     * subset that happened to read the same LDS address. */
    for (unsigned exec = RDNA2_OP_EXEC_LO; exec <= RDNA2_OP_EXEC_HI; exec++) {
        for (unsigned i = cmp; i;) {
            --i;
            if (!sh_barrier_writes_s(p, i, exec)) continue;
            if (p->ops[i] != SH_OP_S_MOV_B64 || p->insts[i].src[0] != 193 ||
                !sh_barrier_dominates(p, i, cmp)) return false;
            break;
        }
    }
    return true; /* initial EXEC enables every compute invocation */
}
static bool sh_barrier_prefix_scc(const sh_program *p, const sh_site_shape *shapes, unsigned branch) {
    unsigned use = branch;
    while (branch) {
        unsigned i = --branch, op = p->ops[i];
        if (op == SH_OP_S_MOV_B32 || op == SH_OP_S_MOV_B64 ||
            op == SH_OP_S_WAITCNT || op == SH_OP_S_NOP) continue;
        return sh_barrier_cmp32(op) && sh_barrier_dominates(p, i, use) &&
               sh_barrier_scalar_at(p, shapes, i, p->insts[i].src[0], 0) &&
               sh_barrier_scalar_at(p, shapes, i, p->insts[i].src[1], 0);
    }
    return false;
}
/* A later loop does not invalidate a barrier in a forward-only prefix.
 * Reject every edge that could re-enter that prefix: reaching this barrier
 * once is different from proving convergence on every loop iteration. */
static bool sh_barrier_prefix(const sh_program *p, const sh_site_shape *shapes, unsigned b, bool uniform_inputs) {
    bool suffix_flow = false;
    for (unsigned i = 0; i < p->count; i++) {
        unsigned op = p->ops[i];
        if ((p->iflags[i] & SH_IF_FLOW) && !sh_barrier_branch(op) &&
            op != SH_OP_S_ENDPGM && op != SH_OP_S_CODE_END) return false;
        if (sh_barrier_branch(op)) {
            if (p->target[i] <= i && p->target[i] <= b) return false;
            if (i < b && op == SH_OP_S_BRANCH) return false;
            if (i > b && (op == SH_OP_S_BRANCH || p->target[i] <= i)) suffix_flow = true;
            if (i < b && p->target[i] > b &&
                (!uniform_inputs || !(op == SH_OP_S_CBRANCH_SCC0 || op == SH_OP_S_CBRANCH_SCC1) ||
                 !sh_barrier_dominates(p, i, b) || !sh_barrier_prefix_scc(p, shapes, i))) return false;
        }
        if ((op == SH_OP_S_ENDPGM || op == SH_OP_S_CODE_END) && i + 1 != p->count) {
            if (i < b) return false;
            suffix_flow = true;
        }
    }
    return suffix_flow;
}
/* Exact first-barrier proof: every prefix edge moves forward within the
 * prefix, so both arms of an if/else reach the barrier exactly once. A later
 * edge must never re-enter it. The structurizer independently checks that
 * these edges can be emitted as legal selections. No claim of uniform SCC. */
static bool sh_barrier_join_prefix(const sh_program *p,unsigned barrier) {
    if(barrier>=p->count || p->ops[barrier]!=SH_OP_S_BARRIER)return false;
    for(unsigned i=0;i<p->count;i++) {
        unsigned op=p->ops[i];
        if((p->iflags[i]&SH_IF_FLOW) && !sh_barrier_branch(op) &&
           op!=SH_OP_S_ENDPGM && op!=SH_OP_S_CODE_END && op!=SH_OP_S_NOP)return false;
        if(i<barrier && (op==SH_OP_S_BARRIER || op==SH_OP_S_ENDPGM || op==SH_OP_S_CODE_END))return false;
        if(sh_barrier_branch(op)) {
            if(i<barrier && (p->target[i]<=i || p->target[i]>barrier))return false;
            if(i>barrier && p->target[i]<=barrier)return false;
        }
    }
    return true;
}
static void sh_barrier_forward_convergence(const sh_program *p, const sh_site_shape *shapes, bool *safe) {
    memset(safe, 0, p->count * sizeof(*safe));
    unsigned prefix = p->count;
    for (unsigned i = 0; i < p->count; i++) {
        unsigned op = p->ops[i];
        /* Unconditional jumps can be an if/else join; a crossing-edge test
         * alone is insufficient for them. Loops/indirect flow need a CFG proof. */
        if (op == SH_OP_S_BRANCH || (sh_barrier_branch(op) && p->target[i] <= i) ||
            ((p->iflags[i] & SH_IF_FLOW) && !sh_barrier_branch(op)) ||
            ((op == SH_OP_S_ENDPGM || op == SH_OP_S_CODE_END) && i + 1 != p->count)) return;
        if (sh_barrier_branch(op) && prefix == p->count) prefix = i;
    }
    bool uniform[106];
    memset(uniform, 1, sizeof(uniform)); /* initial ordinary SGPRs are workgroup inputs */
    for (unsigned i = 0; i < prefix; i++) {
        const rdna2_inst *in = &p->insts[i];
        unsigned op = p->ops[i], width = sh_barrier_dst_width(p, i);
        bool value = false;
        if (in->enc == RDNA2_ENC_SMEM && p->site_of[i] >= 0) {
            unsigned site = (unsigned)p->site_of[i];
            /* Snapshot SMEM is identical for all lanes. Read-only scalar
             * buffer loads also agree when their address and bound agree. */
            if (p->sites[site].kind == SH_SITE_SMEM && !shapes[site].address_map) value = true;
            else if (p->sites[site].kind == SH_SITE_SBUFFER) {
                value = sh_barrier_uniform_src(uniform, in->soffset);
                for (unsigned k = 0; k < 4; k++)
                    value &= in->sbase + k < 106 && uniform[in->sbase + k];
                for (unsigned s = 0; s < p->site_count; s++)
                    if (p->sites[s].store && p->sites[s].kind != SH_SITE_IMAGE) value = false;
            }
        } else if (op == SH_OP_S_MOV_B32 || op == SH_OP_S_MOV_B64) {
            value = sh_barrier_uniform_src(uniform, in->src[0]);
            if (width == 2 && in->src[0] < 106)
                value &= in->src[0] + 1 < 106 && uniform[in->src[0] + 1];
        } else if (op == SH_OP_S_MOVK_I32) value = true;
        else if (op == SH_OP_S_ADD_U32 || op == SH_OP_S_ADD_I32 || op == SH_OP_S_SUB_U32 ||
                 op == SH_OP_S_SUB_I32 || op == SH_OP_S_MUL_I32 || op == SH_OP_S_AND_B32 ||
                 op == SH_OP_S_OR_B32 || op == SH_OP_S_XOR_B32 || op == SH_OP_S_LSHL_B32 ||
                 op == SH_OP_S_LSHR_B32)
            value = sh_barrier_uniform_src(uniform, in->src[0]) && sh_barrier_uniform_src(uniform, in->src[1]);
        for (unsigned k = 0; k < width && in->dst + k < 106; k++) uniform[in->dst + k] = value;
        sh_barrier_kill_mask(uniform, p, i);
    }
    /* A later write, including a lane-to-scalar transfer, invalidates the
     * immutable-prefix proof. Width matters: s_load_dwordx8 s[24:31] is not s36. */
    for (unsigned i = prefix; i < p->count; i++) {
        const rdna2_inst *in = &p->insts[i];
        unsigned width = sh_barrier_dst_width(p, i);
        for (unsigned k = 0; k < width && in->dst + k < 106; k++) uniform[in->dst + k] = false;
        sh_barrier_kill_mask(uniform, p, i);
    }
    for (unsigned b = 0; b < p->count; b++) if (p->ops[b] == SH_OP_S_BARRIER) {
        bool converged = true;
        for (unsigned i = 0; i < b && converged; i++) if (sh_barrier_branch(p->ops[i]) && p->target[i] > b)
            converged = sh_barrier_dominates(p, i, b) &&
                       (((p->ops[i] == SH_OP_S_CBRANCH_SCC0 || p->ops[i] == SH_OP_S_CBRANCH_SCC1) &&
                         sh_barrier_scc_uniform(p, i, uniform)) ||
                        sh_barrier_broadcast_branch(p, shapes, safe, i));
        safe[b] = converged;
    }
}
static void sh_barrier_convergence(const sh_program *p, const sh_site_shape *shapes, bool *safe) {
    sh_barrier_forward_convergence(p, shapes, safe);
    for (unsigned b = 0; b < p->count; b++)
        if (p->ops[b] == SH_OP_S_BARRIER && !safe[b]) safe[b] = sh_barrier_prefix(p, shapes, b, true);
}
#endif
