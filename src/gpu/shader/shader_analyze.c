/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shader analysis (decode, sites) and the per-draw scalar evaluator. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "gpu/shader/shader.h"
#include "gpu/shader/shader_aperture.h"
#include "gpu/shader/shader_ops.h"

/* ---- Operation ids ---- */

static const char *const k_op_names[SH_OP_COUNT] = {
#define X(id, name) name,
    SH_OPS(X)
#undef X
};

/* Entries matched as prefixes (the name tail carries the variant). */
static bool is_prefix_op(uint32_t op) {
    return op == SH_OP_V_CMP || op == SH_OP_V_CMPX || op == SH_OP_BUFFER_LOAD_FORMAT ||
           op == SH_OP_BUFFER_STORE_FORMAT || op == SH_OP_TBUFFER_LOAD_FORMAT || op == SH_OP_TBUFFER_STORE_FORMAT ||
           op == SH_OP_BUFFER_LOAD_DWORD || op == SH_OP_BUFFER_STORE_DWORD || op == SH_OP_IMAGE_SAMPLE ||
           op == SH_OP_IMAGE_LOAD || op == SH_OP_IMAGE_STORE || op == SH_OP_DS || op == SH_OP_IMAGE_GATHER4 ||
           op == SH_OP_BUFFER_ATOMIC || op == SH_OP_GLOBAL_LOAD || op == SH_OP_GLOBAL_STORE || op == SH_OP_IMAGE_ATOMIC ||
           op == SH_OP_GLOBAL_ATOMIC || op == SH_OP_SCRATCH_LOAD || op == SH_OP_SCRATCH_STORE;
}

/* Instructions that act as another: debugger and performance hints are no-ops,
 * program ends with extra state are ends, s_sendmsghalt sends its message */
static const struct {
    const char *name;
    sh_op op;
} k_aliases[] = {
    {"s_endpgm_saved", SH_OP_S_ENDPGM}, {"s_endpgm_ordered_ps_done", SH_OP_S_ENDPGM}, {"s_sendmsghalt", SH_OP_S_SENDMSG},
    {"s_sethalt", SH_OP_S_NOP}, {"s_setkill", SH_OP_S_NOP}, {"s_incperflevel", SH_OP_S_NOP}, {"s_decperflevel", SH_OP_S_NOP},
    {"s_ttracedata_imm", SH_OP_S_NOP}, {"s_wait_idle", SH_OP_S_NOP}, {"s_wakeup", SH_OP_S_NOP}, {"s_version", SH_OP_S_NOP},
    {"s_cbranch_cdbgsys", SH_OP_S_NOP}, {"s_cbranch_cdbguser", SH_OP_S_NOP}, /* no debugger: never taken */
    {"s_cbranch_cdbgsys_or_user", SH_OP_S_NOP}, {"s_cbranch_cdbgsys_and_user", SH_OP_S_NOP},
    {"s_atc_probe", SH_OP_S_NOP}, {"s_atc_probe_buffer", SH_OP_S_NOP}, {"v_clrexcp", SH_OP_V_NOP}, {"v_pipeflush", SH_OP_V_NOP},
};

static uint32_t hash_str32(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h ? h : 1u;
}

sh_op sh_op_of(const rdna2_inst *in) {
    if (!in->name) return SH_OP_UNKNOWN;
    enum { OP_HT_SIZE = 2048 };
    PS5C_STATIC_ASSERT(SH_OP_COUNT + PS5C_ARRAY_COUNT(k_aliases) < OP_HT_SIZE / 2, "op hash table load factor");
    static _Thread_local struct {
        const char *name;
        uint32_t h;
        sh_op op;
    } k_ht[OP_HT_SIZE];
    static _Thread_local struct {
        const char *name;
        uint8_t len;
        sh_op op;
    } k_pfx[32];
    static _Thread_local uint32_t k_pfx_count;
    static _Thread_local int init;
    if (!init) {
        for (size_t i = 0; i < PS5C_ARRAY_COUNT(k_aliases); i++) {
            uint32_t h = hash_str32(k_aliases[i].name);
            uint32_t idx = h & (OP_HT_SIZE - 1);
            while (k_ht[idx].name) idx = (idx + 1) & (OP_HT_SIZE - 1);
            k_ht[idx].name = k_aliases[i].name;
            k_ht[idx].h = h;
            k_ht[idx].op = k_aliases[i].op;
        }
        for (uint32_t i = 1; i < SH_OP_COUNT; i++) {
            uint32_t h = hash_str32(k_op_names[i]);
            uint32_t idx = h & (OP_HT_SIZE - 1);
            while (k_ht[idx].name) idx = (idx + 1) & (OP_HT_SIZE - 1);
            k_ht[idx].name = k_op_names[i];
            k_ht[idx].h = h;
            k_ht[idx].op = (sh_op)i;
            if (is_prefix_op(i) && k_pfx_count < PS5C_ARRAY_COUNT(k_pfx)) {
                k_pfx[k_pfx_count].name = k_op_names[i];
                k_pfx[k_pfx_count].len = (uint8_t)strlen(k_op_names[i]);
                k_pfx[k_pfx_count].op = (sh_op)i;
                k_pfx_count++;
            }
        }
        init = 1;
    }
    uint32_t h = hash_str32(in->name);
    for (uint32_t idx = h & (OP_HT_SIZE - 1); k_ht[idx].name; idx = (idx + 1) & (OP_HT_SIZE - 1)) {
        if (k_ht[idx].h == h && !strcmp(in->name, k_ht[idx].name)) return k_ht[idx].op;
    }
    for (uint32_t i = 0; i < k_pfx_count; i++) {
        if (!strncmp(in->name, k_pfx[i].name, k_pfx[i].len)) return k_pfx[i].op;
    }
    return SH_OP_UNKNOWN;
}

/* ---- Constants and formats ---- */

bool sh_inline_const(uint32_t code, uint32_t literal, uint32_t *out) {
    static const uint32_t k_float[9] = {0x3F000000u, 0xBF000000u, 0x3F800000u, 0xBF800000u, 0x40000000u,
                                        0xC0000000u, 0x40800000u, 0xC0800000u, 0x3E22F983u};
    if (code >= 128 && code <= 192) *out = code - 128;
    else if (code >= 193 && code <= 208) *out = (uint32_t)(-(int32_t)(code - 192));
    else if (code >= 240 && code <= 248) *out = k_float[code - 240];
    else if (code == RDNA2_OP_LITERAL) *out = literal;
    else return false;
    return true;
}

bool sh_buffer_format(uint32_t fmt, sh_buf_format *out) {
    static const uint8_t k_kinds7[7] = {0, 1, 2, 3, 4, 5, 6};
    memset(out, 0, sizeof(*out));
    uint8_t b[4] = {0};
    uint32_t comps = 0, kind = 0;
    if (fmt >= 1 && fmt <= 6) comps = 1, b[0] = 8, kind = k_kinds7[fmt - 1];
    else if (fmt >= 7 && fmt <= 13) comps = 1, b[0] = 16, kind = k_kinds7[fmt - 7];
    else if (fmt >= 14 && fmt <= 19) comps = 2, b[0] = b[1] = 8, kind = k_kinds7[fmt - 14];
    else if (fmt >= 20 && fmt <= 22) comps = 1, b[0] = 32, kind = (uint32_t)(4 + fmt - 20);
    else if (fmt >= 23 && fmt <= 29) comps = 2, b[0] = b[1] = 16, kind = k_kinds7[fmt - 23];
    else if (fmt >= 30 && fmt <= 36) comps = 3, b[0] = 11, b[1] = 11, b[2] = 10, kind = k_kinds7[fmt - 30];
    else if (fmt >= 37 && fmt <= 43) comps = 3, b[0] = 10, b[1] = 11, b[2] = 11, kind = k_kinds7[fmt - 37];
    else if (fmt >= 44 && fmt <= 49) comps = 4, b[0] = 2, b[1] = b[2] = b[3] = 10, kind = k_kinds7[fmt - 44];
    else if (fmt >= 50 && fmt <= 55) comps = 4, b[0] = b[1] = b[2] = 10, b[3] = 2, kind = k_kinds7[fmt - 50];
    else if (fmt >= 56 && fmt <= 61) comps = 4, b[0] = b[1] = b[2] = b[3] = 8, kind = k_kinds7[fmt - 56];
    else if (fmt >= 62 && fmt <= 64) comps = 2, b[0] = b[1] = 32, kind = (uint32_t)(4 + fmt - 62);
    else if (fmt >= 65 && fmt <= 71) comps = 4, b[0] = b[1] = b[2] = b[3] = 16, kind = k_kinds7[fmt - 65];
    else if (fmt >= 72 && fmt <= 74) comps = 3, b[0] = b[1] = b[2] = 32, kind = (uint32_t)(4 + fmt - 72);
    else if (fmt >= 75 && fmt <= 77) comps = 4, b[0] = b[1] = b[2] = b[3] = 32, kind = (uint32_t)(4 + fmt - 75);
    else return false;
    out->comps = (uint8_t)comps;
    memcpy(out->bits, b, 4);
    out->kind = (uint8_t)kind;
    return true;
}

/* ---- Analysis ---- */

static uint64_t fnv1a(const void *data, size_t size) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < size; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static bool is_branch(uint32_t op) {
    return op == SH_OP_S_BRANCH || op == SH_OP_S_CBRANCH_SCC0 || op == SH_OP_S_CBRANCH_SCC1 ||
           op == SH_OP_S_CBRANCH_VCCZ || op == SH_OP_S_CBRANCH_VCCNZ || op == SH_OP_S_CBRANCH_EXECZ ||
           op == SH_OP_S_CBRANCH_EXECNZ;
}

static bool is_smem(uint32_t op) { return op >= SH_OP_S_LOAD_DWORD && op <= SH_OP_S_BUFFER_LOAD_DWORDX16; }

/* Instructions the scalar evaluation acts on: all but vector ALU (except
 * compares and lane reads), interpolation, LDS and exports. */
static bool scalar_relevant(const rdna2_inst *in, uint32_t op) {
    switch (in->enc) {
    case RDNA2_ENC_VOP1: return op == SH_OP_V_READFIRSTLANE_B32;
    case RDNA2_ENC_VOP3: return in->valu < 0x100 || op == SH_OP_V_READLANE_B32 || op == SH_OP_V_WRITELANE_B32;
    case RDNA2_ENC_VOP2:
    case RDNA2_ENC_VOP3P:
    case RDNA2_ENC_DS: return in->gds; /* GDS: a memory site */
    case RDNA2_ENC_VINTRP:
    case RDNA2_ENC_EXP: return false;
    default: return true;
    }
}

guest_addr sh_gds_base;
bool sh_flat_apertures(const sh_program *p) {
    const char *setting=getenv("PS5C_FLAT_APERTURES");
    return (p->aperture_flags&5)==5 && setting && !strcmp(setting,"1");
}
bool sh_aperture_allowed(const sh_program *p) {
    if(!(p->aperture_flags&1))return true;
    const char *setting=getenv("PS5C_APERTURE_SOURCES");
    if((p->aperture_flags&2) || !setting || strcmp(setting,"1"))return false;
    if(!(p->aperture_flags&4))return true;
    if(!sh_flat_apertures(p))return false;
    for(uint32_t i=0;i<p->count;i++) {
        const rdna2_inst *in=&p->insts[i];
        if(in->enc!=RDNA2_ENC_FLAT || in->seg!=0)continue;
        if((p->ops[i]!=SH_OP_GLOBAL_LOAD && p->ops[i]!=SH_OP_GLOBAL_STORE) ||
           !in->name || !strstr(in->name,"dword") || strstr(in->name,"addtid") || in->vaddr>=255)return false;
    }
    return true;
}

static uint32_t smem_dwords(uint32_t op);

static inline void mark_sgprs64(uint64_t *mask, uint32_t base, uint32_t count) {
    for (uint32_t k = 0; k < count && base + k < 64; k++) *mask |= 1ULL << (base + k);
}

/* sh_program.eval_reloc_sgprs: every other operand that names one of them,
 * read or written, at any width, disqualifies it. Only the fields an encoding
 * has: the decoder leaves the others 0 (s0). */
static uint64_t reloc_sgprs(const sh_program *p, uint64_t eval_mask) {
    uint64_t bases = 0, other = 0;
    for (uint32_t i = 0; i < p->count; i++) {
        const rdna2_inst *in = &p->insts[i];
        const char *n = in->name ? in->name : "";
        uint32_t op = p->ops[i];
        if (op == SH_OP_S_SWAPPC_B64 || strstr(n, "movrel")) return 0;
        if (in->enc != RDNA2_ENC_DS)
            for (int k = 0; k < 3; k++) mark_sgprs64(&other, in->src[k], 2);
        switch (in->enc) {
        case RDNA2_ENC_SMEM:
            if (is_smem(op)) {
                bool buffer = op >= SH_OP_S_BUFFER_LOAD_DWORD;
                mark_sgprs64(&bases, in->sbase, 1);
                mark_sgprs64(&other, in->sbase + 1u, buffer ? 3 : 1);
                mark_sgprs64(&other, in->dst, smem_dwords(op));
                if (in->soffset != RDNA2_OP_NULL) mark_sgprs64(&other, in->soffset, 1);
            } else if (op == SH_OP_S_MEMTIME || op == SH_OP_S_MEMREALTIME) {
                mark_sgprs64(&other, in->dst, 2);
            } else if (strncmp(n, "s_dcache", 8) && strncmp(n, "s_gl1", 5)) {
                return 0; /* scalar stores and atomics */
            }
            break;
        case RDNA2_ENC_MUBUF: case RDNA2_ENC_MTBUF:
            mark_sgprs64(&other, in->srsrc, 4);
            if (in->soffset != RDNA2_OP_NULL) mark_sgprs64(&other, in->soffset, 1);
            break;
        case RDNA2_ENC_MIMG:
            mark_sgprs64(&other, in->srsrc, 8);
            mark_sgprs64(&other, in->ssamp, 4);
            break;
        case RDNA2_ENC_FLAT:
            if (in->sbase != RDNA2_OP_NULL) mark_sgprs64(&other, in->sbase, 2);
            break;
        case RDNA2_ENC_DS: case RDNA2_ENC_VINTRP: case RDNA2_ENC_EXP: case RDNA2_ENC_SOPP: case RDNA2_ENC_SOPC: break;
        case RDNA2_ENC_SOP1: case RDNA2_ENC_SOP2: case RDNA2_ENC_SOPK:
            mark_sgprs64(&other, in->dst, 2);
            break;
        case RDNA2_ENC_VOP1: case RDNA2_ENC_VOP2: case RDNA2_ENC_VOP3: case RDNA2_ENC_VOP3P: case RDNA2_ENC_VOPC:
            mark_sgprs64(&other, in->dst, 2); /* a VGPR (256 on) unless a scalar result */
            if (in->sdst || strstr(n, "_co_") || strstr(n, "div_scale") || strstr(n, "mad_u64") || strstr(n, "mad_i64"))
                mark_sgprs64(&other, in->sdst, 2);
            if (in->src[0] == RDNA2_OP_SDWA) {
                if ((in->dpp_sdwa >> 23) & 1) mark_sgprs64(&other, in->dpp_sdwa & 0xFF, 1);
                if ((in->dpp_sdwa >> 15) & 1) mark_sgprs64(&other, (in->dpp_sdwa >> 8) & 0x7F, 2);
                if (in->src[1] >= RDNA2_OP_VGPR && (in->dpp_sdwa >> 31)) mark_sgprs64(&other, in->src[1] - RDNA2_OP_VGPR, 1);
            }
            break;
        default: return 0; /* an encoding without known operands */
        }
    }
    return bases & ~other & eval_mask;
}

static inline void mark_eval_sgprs(uint64_t *mask, uint32_t base, uint32_t count) {
    for (uint32_t k = 0; k < count && base + k < 48; k++)
        *mask |= 1ULL << (base + k);
}

void sh_program_flags(sh_program *p) {
    static const struct { const char *tail; uint8_t w; } k_tails[] = {
        {"_xyzw", 4}, {"_xyz", 3}, {"_xy", 2}, {"_x", 1}, {"dwordx4", 4}, {"dwordx3", 3}, {"dwordx2", 2},
        {"dword", 1}, {"ubyte", 1}, {"sbyte", 1}, {"ushort", 1}, {"sshort", 1}};
    p->has_flow = false;
    p->eval_forward_branches = 0;
    p->aperture_flags&=4; /* raw FLAT marker from decoding, before seg is normalized */
    bool has_swappc = false, has_global = false;
    for (uint32_t i = 0; i < p->count; i++) {
        uint32_t op = p->ops[i];
        if (op == SH_OP_GLOBAL_LOAD || op == SH_OP_GLOBAL_STORE || op == SH_OP_GLOBAL_ATOMIC) {
            has_global = true;
            break;
        }
    }
    uint64_t sgpr_mask = 0;
    for (uint32_t i = 0; i < p->count; i++) {
        const rdna2_inst *in = &p->insts[i];
        const char *n = in->name ? in->name : "";
        uint32_t op = p->ops[i];
        for(unsigned s=0;s<3;s++) if(in->src[s]>=235 && in->src[s]<=238) {
            p->aperture_flags|=1;
            if(op!=SH_OP_S_MOV_B32 && op!=SH_OP_S_MOV_B64)p->aperture_flags|=2;
        }
        if(op!=SH_OP_S_BRANCH && is_branch(op) && in->simm16>0)p->eval_forward_branches++;
        if(in->enc==RDNA2_ENC_FLAT && in->seg==0)p->aperture_flags|=4;
        uint8_t f = 0;
        if (strstr(n, "b64") || strstr(n, "i64") || strstr(n, "u64")) f |= SH_IF_64;
        if (strstr(n, "co_")) f |= SH_IF_CARRY;
        if (strstr(n, "saveexec")) f |= SH_IF_SAVEEXEC;
        if (strstr(n, "branch") || strstr(n, "setpc") || strstr(n, "swappc") || strstr(n, "movrel") ||
            in->enc == RDNA2_ENC_UNKNOWN)
            f |= SH_IF_FLOW;
        p->iflags[i] = f;
        p->has_flow |= (f & SH_IF_FLOW) != 0;
        has_swappc |= op == SH_OP_S_SWAPPC_B64;
        if (strstr(n, "movrel")) sgpr_mask = 0xFFFFFFFFFFFFULL;
        if (in->enc == RDNA2_ENC_SOP1 || in->enc == RDNA2_ENC_SOP2 || in->enc == RDNA2_ENC_SOPK ||
            in->enc == RDNA2_ENC_SOPC || in->enc == RDNA2_ENC_SOPP) {
            mark_eval_sgprs(&sgpr_mask, in->src[0], 2);
            mark_eval_sgprs(&sgpr_mask, in->src[1], 2);
            if ((in->enc == RDNA2_ENC_SOPK && op != SH_OP_S_MOVK_I32 && op != SH_OP_S_GETREG_B32 && op != SH_OP_S_SETREG_IMM32_B32) ||
                op == SH_OP_S_BITSET0_B32 || op == SH_OP_S_BITSET1_B32 || op == SH_OP_S_BITSET0_B64 || op == SH_OP_S_BITSET1_B64 ||
                op == SH_OP_S_CMOV_B32 || op == SH_OP_S_CMOV_B64)
                mark_eval_sgprs(&sgpr_mask, in->dst, 2);
        } else if (is_smem(op)) {
            mark_eval_sgprs(&sgpr_mask, in->sbase, op >= SH_OP_S_BUFFER_LOAD_DWORD ? 4 : 2);
            mark_eval_sgprs(&sgpr_mask, in->soffset, 1);
        } else if (in->enc == RDNA2_ENC_MUBUF || in->enc == RDNA2_ENC_MTBUF) {
            mark_eval_sgprs(&sgpr_mask, in->srsrc, 4);
            mark_eval_sgprs(&sgpr_mask, in->soffset, 1);
        } else if (in->enc == RDNA2_ENC_MIMG) {
            mark_eval_sgprs(&sgpr_mask, in->srsrc, 8);
            mark_eval_sgprs(&sgpr_mask, in->ssamp, 4);
        } else if (in->enc == RDNA2_ENC_FLAT) {
            mark_eval_sgprs(&sgpr_mask, in->sbase, 2);
        } else if ((in->enc == RDNA2_ENC_VOP1 || in->enc == RDNA2_ENC_VOP2 || in->enc == RDNA2_ENC_VOP3) &&
                   (has_global || op == SH_OP_V_WRITELANE_B32 || op == SH_OP_V_READLANE_B32 || op == SH_OP_V_MOV_B32 ||
                    (p->lane_reads && (op == SH_OP_V_OR_B32 || op == SH_OP_V_AND_B32 || op == SH_OP_V_XOR_B32 ||
                                       op == SH_OP_V_ADD_NC_U32 || op == SH_OP_V_SUB_NC_U32 || op == SH_OP_V_SUBREV_NC_U32 ||
                                       op == SH_OP_V_LSHLREV_B32 || op == SH_OP_V_LSHRREV_B32)))) {
            for (int k = 0; k < 3; k++) mark_eval_sgprs(&sgpr_mask, in->src[k], 1);
            if (in->src[0] == RDNA2_OP_SDWA) {
                if ((in->dpp_sdwa >> 23) & 1) mark_eval_sgprs(&sgpr_mask, in->dpp_sdwa & 0xFF, 1);
                if (in->enc != RDNA2_ENC_VOP1 && in->src[1] >= RDNA2_OP_VGPR && (in->dpp_sdwa >> 31))
                    mark_eval_sgprs(&sgpr_mask, in->src[1] - RDNA2_OP_VGPR, 1);
            }
        }
        uint8_t w = op == SH_OP_IMAGE_BVH || op == SH_OP_IMAGE_BVH64 ? 4 : 16;
        if ((in->enc == RDNA2_ENC_MUBUF || in->enc == RDNA2_ENC_MTBUF || in->enc == RDNA2_ENC_FLAT) &&
            !strstr(n, "atomic") && !strstr(n, "d16")) {
            size_t len = strlen(n);
            for (size_t k = 0; k < PS5C_ARRAY_COUNT(k_tails) && w == 16; k++) {
                size_t t = strlen(k_tails[k].tail);
                if (len >= t && !strcmp(n + len - t, k_tails[k].tail)) w = (uint8_t)(k_tails[k].w + (in->tfe ? 1 : 0));
            }
        }
        p->vmem_width[i] = w;
    }
    p->eval_sgpr_lo = (uint32_t)sgpr_mask;
    p->eval_sgpr_hi = (uint16_t)(sgpr_mask >> 32);
    p->eval_reloc_sgprs = reloc_sgprs(p, sgpr_mask);
    if (p->count) {
        p->iflags[0] |= SH_IF_ANALYZED | (has_swappc ? SH_IF_HAS_SWAPPC : 0);
        for (uint32_t j = 0; j < p->count; j++) {
            if (is_branch(p->ops[j]) && p->target[j] < p->count && p->next_scalar[p->target[j]] < p->count)
                p->iflags[p->next_scalar[p->target[j]]] |= SH_IF_JOIN;
        }
    }
}

/* Whether `in` may write SGPR `r` or `r + 1` (conservative: SMEM loads count as 16 dwords) */
static bool writes_pair(const rdna2_inst *in, uint32_t r) {
    uint32_t d = in->dst, n = 2;
    switch (in->enc) {
    case RDNA2_ENC_SOP1:
    case RDNA2_ENC_SOP2:
    case RDNA2_ENC_SOPK: break;
    case RDNA2_ENC_SMEM: n = 16; break;
    case RDNA2_ENC_VOP1:
    case RDNA2_ENC_VOP2:
    case RDNA2_ENC_VOPC:
    case RDNA2_ENC_VOP3:
    case RDNA2_ENC_VOP3P:
        /* VOP3B: carry / div_scale masks in sdst (compares write dst) */
        if (in->enc == RDNA2_ENC_VOP3 && in->name && (strstr(in->name, "_co_") || strstr(in->name, "div_scale")) &&
            in->sdst + 1u >= r && in->sdst <= r + 1)
            return true;
        break;
    default: return false; /* SOPC/SOPP: SCC only; memory, LDS, exports: VGPRs */
    }
    return d < RDNA2_OP_VGPR && d + n > r && d <= r + 1;
}

/* s_setpc_b64 to a target the code computes from its own PC:
 *   s_getpc_b64 s[n:n+1]; s_add_u32 / s_sub_u32 sn, sn, K; s_addc_u32 / s_subb_u32 sn+1, sn+1, K'; s_setpc_b64 s[n:n+1]
 * (other instructions in between leave the pair alone). The target's dword
 * offset in the program, or -1 when it is not known at translation. */
static int64_t static_setpc(const sh_program *p, uint32_t i, uint32_t max_dwords, uint32_t *getpc) {
    const rdna2_inst *in = &p->insts[i];
    uint32_t r = in->src[0], g;
    if (r >= RDNA2_OP_VCC_LO || (r & 1)) return -1;
    for (g = i; g-- > 0 && i - g <= 32;) {
        const rdna2_inst *q = &p->insts[g];
        const char *n = q->name ? q->name : "";
        if (!strcmp(n, "s_getpc_b64") && q->dst == r) break;
        if (q->enc == RDNA2_ENC_SOPP && (strstr(n, "branch") || p->ops[g] == SH_OP_S_ENDPGM)) return -1;
        if (strstr(n, "setpc") || strstr(n, "swappc") || strstr(n, "movrel")) return -1;
    }
    if (g >= i || i - g > 32) return -1;
    uint64_t v = p->base + (uint64_t)(p->pc[g] + p->insts[g].words) * 4;
    bool carry = false, carry_known = false;
    for (uint32_t k = g + 1; k < i; k++) {
        const rdna2_inst *q = &p->insts[k];
        const char *n = q->name ? q->name : "";
        uint32_t c, lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
        bool salu = q->enc == RDNA2_ENC_SOP1 || q->enc == RDNA2_ENC_SOP2 || q->enc == RDNA2_ENC_SOPK || q->enc == RDNA2_ENC_SOPC;
        if (strstr(n, "movrel")) return -1;
        if (q->enc == RDNA2_ENC_SOP2 && (q->dst == r || q->dst == r + 1)) {
            /* the pair register is one source, a constant the other */
            uint32_t self = q->dst, other;
            if (q->src[0] == self) other = q->src[1];
            else if (q->src[1] == self && strstr(n, "add")) other = q->src[0];
            else return -1;
            if (!sh_inline_const(other, q->literal, &c)) return -1;
            if (!strcmp(n, "s_add_u32") && self == r) {
                carry = (uint64_t)lo + c > 0xFFFFFFFFull, lo += c;
            } else if (!strcmp(n, "s_sub_u32") && self == r) {
                carry = c > lo, lo -= c;
            } else if (!strcmp(n, "s_addc_u32") && self == r + 1 && carry_known) {
                hi += c + (carry ? 1u : 0u);
            } else if (!strcmp(n, "s_subb_u32") && self == r + 1 && carry_known) {
                hi -= c + (carry ? 1u : 0u);
            } else return -1;
            carry_known = self == r;
            v = (uint64_t)hi << 32 | lo;
            continue;
        }
        if (q->enc == RDNA2_ENC_SOPP && (strstr(n, "branch") || p->ops[k] == SH_OP_S_ENDPGM)) return -1;
        if (writes_pair(q, r)) return -1;
        if (salu) carry_known = false; /* SCC may change */
    }
    if (v < p->base || ((v - p->base) & 3) || (v - p->base) / 4 >= max_dwords) return -1;
    *getpc = g;
    return (int64_t)((v - p->base) / 4);
}

uint32_t sh_front_dwords(const uint32_t *code, uint32_t max_dwords) {
    rdna2_inst in;
    for (uint32_t pos = 0; pos < max_dwords;) {
        if (!rdna2_decode(code + pos, max_dwords - pos, &in)) return 0;
        /* s_setpc_b64 s[6:7], or s_swappc_b64 null, s[6:7] (the return address discarded: the same jump) */
        if (code[pos] == 0xBE802006u || code[pos] == 0xBEFD2106u) return pos + 1;
        if (code[pos] == 0xBF810000u || code[pos] == 0xBF9F0000u) return 0; /* ENDPGM / CODE_END */
        pos += in.words;
    }
    return 0;
}

static bool analyze(const uint32_t *code, uint32_t max_dwords, guest_addr base, sh_program *p, bool callee) {
    memset(p, 0, offsetof(sh_program, insts));
    p->base = base;
    p->code = code;
    p->count = 0;
    p->site_count = 0;
    p->param_mask = p->mrt_mask = p->attr_mask = 0;
    p->has_stores = false;
    p->wave32 = false;
    p->back_branch = false;
    uint32_t pos = 0, last_target = 0, bindings = 0;
    bool terminated = false;
    uint32_t jumps[16][2], jump_count = 0; /* resolved s_setpc_b64: its s_getpc_b64 and itself */
    while (pos < max_dwords) {
        if (p->count == SH_MAX_INSTS) {
            uint32_t end = pos;
            while (end < max_dwords && code[end] != 0xBF810000u) end++; /* s_endpgm */
            LOG_E(GPU, "shader 0x%llx: too long (over %u instructions; s_endpgm at dword %u of %u)",
                  (unsigned long long)base, SH_MAX_INSTS, end, max_dwords);
            return false;
        }
        rdna2_inst *in = &p->insts[p->count];
        if (!rdna2_decode(code + pos, max_dwords - pos, in)) {
            LOG_E(GPU, "shader 0x%llx: cannot decode at +0x%x (%08x)", (unsigned long long)base, pos * 4, code[pos]);
            return false;
        }
        if(in->enc==RDNA2_ENC_FLAT && ((code[pos]>>14)&3)==0)p->aperture_flags|=4;
        sh_op op = sh_op_of(in);
        uint32_t i = p->count++;
        p->ops[i] = (uint16_t)op;
        p->pc[i] = pos;
        p->site_of[i] = -1;
        pos += in->words;
        if (op == SH_OP_UNKNOWN && in->name && !strcmp(in->name, "s_setpc_b64")) {
            uint32_t g = 0;
            int64_t t = static_setpc(p, i, max_dwords, &g);
            if (t < 0 || jump_count == PS5C_ARRAY_COUNT(jumps)) {
                LOG_E(GPU, "shader 0x%llx: s_setpc_b64 s[%u:%u] at +0x%x: jump target not known at translation%s",
                      (unsigned long long)base, in->src[0], in->src[0] + 1u, p->pc[i] * 4,
                      in->src[0] == 6 ? " (first half of a merged program?)" : "");
                return false;
            }
            /* a jump to a known place: from here on an s_branch there */
            in->enc = RDNA2_ENC_SOPP;
            in->name = "s_branch";
            in->dst = in->src[0] = 0;
            in->simm16 = (int32_t)(t - (int64_t)pos);
            p->ops[i] = (uint16_t)(op = SH_OP_S_BRANCH);
            jumps[jump_count][0] = g, jumps[jump_count++][1] = i;
        }
        if (op == SH_OP_UNKNOWN) {
            LOG_E(GPU, "shader 0x%llx: '%s' not supported (+0x%x)", (unsigned long long)base, in->name, p->pc[i] * 4);
            return false;
        }
        if ((in->enc == RDNA2_ENC_SOP1 || in->enc == RDNA2_ENC_SOP2 || in->enc == RDNA2_ENC_SOPK) && in->name) {
            size_t nl = strlen(in->name);
            bool b32 = (nl > 4 && !strcmp(in->name + nl - 4, "_b32")) || strstr(in->name, "_u32") || strstr(in->name, "_i32");
            /* a 32-bit scalar result in VCC_HI or EXEC_LO (s_and_saveexec_b32 and the like) */
            if ((in->dst == 107 && b32) || (in->dst == 126 && nl > 4 && !strcmp(in->name + nl - 4, "_b32")))
                p->wave32 = true;
        }
        if (is_branch(op)) {
            uint32_t target = (uint32_t)((int32_t)pos + in->simm16);
            if (target > last_target) last_target = target;
        }
        bool vmem = in->enc == RDNA2_ENC_MUBUF || in->enc == RDNA2_ENC_MTBUF || in->enc == RDNA2_ENC_MIMG ||
                    (in->enc == RDNA2_ENC_FLAT && in->seg != 1); /* scratch: lane-private, no binding */
        bool gds = in->enc == RDNA2_ENC_DS && in->gds;
        if ((is_smem(op) || vmem || gds) && op != SH_OP_BUFFER_GL0_INV && op != SH_OP_BUFFER_GL1_INV) {
            if (p->site_count == SH_MAX_SITES) {
                LOG_E(GPU, "shader 0x%llx: too many memory instructions (over %u)", (unsigned long long)base, SH_MAX_SITES);
                return false;
            }
            sh_site *s = &p->sites[p->site_count];
            s->pops_load=false;
            s->inst = i;
            s->kind = is_smem(op) ? (op >= SH_OP_S_BUFFER_LOAD_DWORD ? SH_SITE_SBUFFER : SH_SITE_SMEM)
                                : in->enc == RDNA2_ENC_MIMG ? SH_SITE_IMAGE : SH_SITE_BUFFER;
            if (op == SH_OP_IMAGE_BVH || op == SH_OP_IMAGE_BVH64) s->kind = SH_SITE_BVH;
            s->store = op == SH_OP_BUFFER_STORE_FORMAT || op == SH_OP_TBUFFER_STORE_FORMAT ||
                       op == SH_OP_BUFFER_STORE_DWORD || op == SH_OP_IMAGE_STORE || op == SH_OP_BUFFER_ATOMIC ||
                       op == SH_OP_IMAGE_ATOMIC || op == SH_OP_GLOBAL_ATOMIC || op == SH_OP_BUFFER_STORE_BYTE ||
                       op == SH_OP_BUFFER_STORE_BYTE_D16_HI || op == SH_OP_BUFFER_STORE_SHORT ||
                       op == SH_OP_BUFFER_STORE_SHORT_D16_HI || op == SH_OP_GLOBAL_STORE || gds;
            /* every site but SMEM is one descriptor binding: keep within the device's per-stage resources
             * (Sonic's materials reach ~150; Intel also takes more than its 64 advertised samplers) */
            if (s->kind != SH_SITE_SMEM && ++bindings > SH_MAX_BINDING_SITES) {
                LOG_E(GPU, "shader 0x%llx: too many memory instructions (over %u bindings)", (unsigned long long)base,
                      SH_MAX_BINDING_SITES);
                return false;
            }
            if (s->store) p->has_stores = true;
            p->site_of[i] = (int16_t)p->site_count++;
        }
        if (op == SH_OP_EXP) {
            uint32_t t = in->exp_target;
            if (t <= 8) p->mrt_mask |= 1u << t;
            else if (t >= 32 && t < 64) p->param_mask |= 1u << (t - 32);
        }
        if (in->enc == RDNA2_ENC_VINTRP) p->attr_mask |= 1u << in->attr;
        if (callee && (op == SH_OP_S_ENDPGM || op == SH_OP_S_CODE_END)) return false;
        if ((callee && op == SH_OP_S_SWAPPC_B64 && in->dst == RDNA2_OP_NULL && pos > last_target) ||
            (op == SH_OP_S_ENDPGM && pos > last_target) || op == SH_OP_S_CODE_END) {
            terminated = true;
            break;
        }
    }
    if (!terminated) {
        LOG_E(GPU, "shader 0x%llx: code window ended without a complete program/function (%u dwords)",
              (unsigned long long)base, max_dwords);
        return false;
    }
    if (callee && (!p->count || p->ops[p->count-1] != SH_OP_S_SWAPPC_B64 ||
                   p->insts[p->count-1].dst != RDNA2_OP_NULL)) return false;
    p->dwords = pos;
    p->function_count = 1;
    p->functions[0] = (sh_function){base,0,p->count,0,pos,UINT32_MAX};
    /* POPS protects read/modify/write image access. Texture fetches may keep
     * stale data even inside the native interlock, so plain integer-coordinate
     * loads use coherent storage-image reads. Keep store separate: these sites
     * do not actually write. Mip/sample operations retain their own semantics. */
    bool pops=false;
    for(uint32_t i=0;i<p->count;i++) {
        const rdna2_inst *q=&p->insts[i];
        if(q->enc==RDNA2_ENC_SOP2 && (q->src[0]==239 || q->src[1]==239)) pops=true;
    }
    if(pops) for(uint32_t s=0;s<p->site_count;s++) {
        const rdna2_inst *q=&p->insts[p->sites[s].inst];
        if(p->ops[p->sites[s].inst]!=SH_OP_IMAGE_LOAD || !q->name || strcmp(q->name,"image_load")) continue;
        p->sites[s].pops_load=true;
    }
    p->hash = fnv1a(code, (size_t)pos * 4);
    p->next_scalar[p->count] = (uint16_t)p->count;
    p->lane_reads = false;
    for (uint32_t i = p->count; i-- > 0;) {
        p->next_scalar[i] = scalar_relevant(&p->insts[i], p->ops[i]) ? (uint16_t)i : p->next_scalar[i + 1];
        if ((p->ops[i] == SH_OP_V_READFIRSTLANE_B32 || p->ops[i] == SH_OP_V_READLANE_B32) &&
            p->insts[i].src[0] >= RDNA2_OP_VGPR) p->lane_reads = true;
        p->target[i] = (uint16_t)p->count;
        if (!is_branch(p->ops[i])) continue;
        uint32_t target = (uint32_t)((int32_t)(p->pc[i] + p->insts[i].words) + p->insts[i].simm16), t;
        uint32_t lo = 0, hi = p->count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (p->pc[mid] < target) lo = mid + 1;
            else hi = mid;
        }
        t = (lo < p->count && p->pc[lo] == target) ? lo : p->count;
        p->target[i] = (uint16_t)t;
        if (t <= i) p->back_branch = true;
        for (uint32_t k = 0; k < jump_count; k++) {
            if (t <= jumps[k][0] || t > jumps[k][1] || (i >= jumps[k][0] && i <= jumps[k][1])) continue;
            /* entered between the s_getpc_b64 and the s_setpc_b64: the pair may hold another address */
            LOG_E(GPU, "shader 0x%llx: s_setpc_b64 at +0x%x: jump target not known at translation (branch into its computation)",
                  (unsigned long long)base, p->pc[jumps[k][1]] * 4);
            return false;
        }
    }
    sh_program_flags(p);
    return true;
}

bool sh_analyze(const uint32_t *code, uint32_t n, guest_addr base, sh_program *p) {
    return analyze(code,n,base,p,false);
}

guest_addr sh_inst_address(const sh_program *p, uint32_t i) {
    for (uint32_t f=0; f<p->function_count; f++) {
        const sh_function *q=&p->functions[f];
        if (i>=q->first && i<q->first+q->count)
            return q->base+(uint64_t)(p->pc[i]-q->pc_start)*4;
    }
    return p->base+(uint64_t)p->pc[i]*4;
}

bool sh_pc_uniform(const sh_program *p) {
    static int off = -1;
    if (off < 0) off = getenv("PS5C_PC_CONSTANT") != NULL;
    if (off || p->function_count > 1 || (p->function_count == 1 && p->functions[0].base != p->base)) return false;
    for (uint32_t i = 0; i < p->count; i++)
        if (p->ops[i] == SH_OP_S_SWAPPC_B64) return false; /* s_setpc_b64 is not translated */
    return true;
}

uint32_t sh_call_destination(const sh_program *p, guest_addr addr, uint32_t caller) {
    for (uint32_t f=1; f<p->function_count; f++)
        if (addr==p->functions[f].base && caller==p->functions[f].caller) return p->functions[f].first;
    return p->count;
}

bool sh_append_function(sh_program *p, const uint32_t *code, uint32_t n, guest_addr base, uint32_t caller) {
    if (!base || (base&3) || p->function_count>=SH_MAX_FUNCTIONS) return false;
    if (!p->count || caller>=p->count-1 || p->ops[caller]!=SH_OP_S_SWAPPC_B64 || p->insts[caller].dst==RDNA2_OP_NULL) return false;
    for (uint32_t f=1; f<p->function_count; f++)
        if (p->functions[f].base==base && p->functions[f].caller==caller) return false;
    sh_program *q=malloc(sizeof(*q));
    if (!q) return false;
    bool ok=analyze(code,n,base,q,true);
    uint32_t at=p->count, site=p->site_count;
    if (!ok || q->count>SH_MAX_INSTS-at || q->site_count>SH_MAX_SITES-site) { free(q);return false; }
    /* No branch may escape its function, including to the end sentinel. */
    for (uint32_t i=0;i<q->count;i++)
        if (is_branch(q->ops[i]) && q->target[i]>=q->count) { free(q);return false; }
    const sh_function *last=&p->functions[p->function_count-1];
    uint32_t pc=last->pc_start+last->dwords;
    for (uint32_t i=0;i<q->count;i++) {
        p->insts[at+i]=q->insts[i];p->ops[at+i]=q->ops[i];p->pc[at+i]=q->pc[i]+pc;
        p->site_of[at+i]=q->site_of[i]<0?-1:(int16_t)(q->site_of[i]+site);
        p->target[at+i]=(uint16_t)(q->target[i]+at);
    }
    for (uint32_t s=0;s<q->site_count;s++) { p->sites[site+s]=q->sites[s];p->sites[site+s].inst+=at; }
    /* Each call context owns its resource sites. Sharing a function body
     * would overwrite descriptors when two calls pass different arguments. */
    p->functions[p->function_count++]=(sh_function){base,at,q->count,pc,q->dwords,caller};
    p->count+=q->count;p->site_count+=q->site_count;
    p->has_stores|=q->has_stores;p->wave32|=q->wave32;p->lane_reads|=q->lane_reads;
    p->aperture_flags|=q->aperture_flags&4;
    p->param_mask|=q->param_mask;p->mrt_mask|=q->mrt_mask;p->attr_mask|=q->attr_mask;p->back_branch|=q->back_branch;
    p->hash=(p->hash^q->hash)*0x100000001b3ull;
    p->hash=(p->hash^base)*0x100000001b3ull;
    p->hash=(p->hash^caller)*0x100000001b3ull;
    p->next_scalar[p->count]=(uint16_t)p->count;
    for (uint32_t i=p->count;i-- >0;)
        p->next_scalar[i]=scalar_relevant(&p->insts[i],p->ops[i])?(uint16_t)i:p->next_scalar[i+1];
    sh_program_flags(p);
    free(q);return true;
}

/* ---- Scalar evaluator ---- */

/* Scalar state. `unk` marks values that depend on vector results (compare
 * masks, v_readfirstlane), which the evaluator does not know. */
typedef struct eval_state {
    uint32_t s[SH_SGPRS];
    bool wave32;
    uint32_t call_depth;
    uint32_t call_resume[SH_MAX_FUNCTIONS];
    guest_addr call_entries[SH_MAX_FUNCTIONS], call_returns[SH_MAX_FUNCTIONS];
    bool scc;
    bool unk[SH_SGPRS];
    bool scc_unk;
    /* values that depend on a deferred S_BUFFER_LOAD (sh_eval.defer), a subset of the
     * unknown ones: SGPRs, SCC and (through vector copies) VGPRs */
    bool dfr[SH_SGPRS];
    bool scc_dfr;
    uint32_t vdfr[8];
    bool fork; /* a path taken at a branch the fall-through path skipped: fills sites only */
    /* SGPRs spilled to VGPR lanes (v_writelane) and read back (v_readlane): Unreal's
     * large shaders keep descriptors there; unknown lanes read as before (unknown) */
    uint32_t lane_count;
    struct {
        uint8_t vgpr, lane;
        bool unk, dfr;
        uint32_t v;
    } lanes[64];
    /* VGPRs holding one value in every lane, copied from scalar values (the
     * compiler keeps descriptor words there across loops and v_readfirstlane
     * reads them back); any other write forgets them */
    uint32_t uniform_count;
    uint32_t group_bits[SH_SGPRS / 32]; /* SGPRs still holding a workgroup id */
    struct {
        uint16_t vgpr;
        uint32_t v;
    } uniform[16];
} eval_state;
_Static_assert(SH_SGPRS % 32 == 0, "eval_state_hash reads the SGPRs and their marks as groups of four 64-bit words");

static const void *eval_host(const sh_eval *e, gmem *mem, guest_addr addr, uint64_t size) {
    return e->host ? e->host(e->host_user, addr, size) : gmem_host(mem, addr, size);
}

static uint32_t rd32(const eval_state *st, const rdna2_inst *in, uint32_t code) {
    uint32_t v = 0;
    uint64_t aperture;
    if(sh_aperture_value(code,&aperture))return (uint32_t)aperture;
    if (code < SH_SGPRS) return st->s[code];
    if (code == RDNA2_OP_SCC) return st->scc;
    if (code == RDNA2_OP_EXECZ) return (st->s[126] | (st->wave32?0:st->s[127])) == 0;
    if (code == RDNA2_OP_VCCZ) return (st->s[106] | (st->wave32?0:st->s[107])) == 0;
    sh_inline_const(code, in->literal, &v);
    return v;
}

static uint64_t rd64(const eval_state *st, const rdna2_inst *in, uint32_t code) {
    uint64_t aperture;
    if(sh_aperture_value(code,&aperture))return aperture;
    if (code < SH_SGPRS - 1) return (uint64_t)st->s[code] | ((uint64_t)st->s[code + 1] << 32);
    uint32_t v = 0;
    sh_inline_const(code, in->literal, &v);
    if (code >= 193 && code <= 208) return (uint64_t)(int64_t)(int32_t)v; /* negative integers extend */
    return v;
}

static void wr32(eval_state *st, uint32_t code, uint32_t v) {
    if (code < SH_SGPRS && code != RDNA2_OP_NULL) st->s[code] = v, st->group_bits[code / 32] &= ~(1u << (code % 32));
}

static void wr64(eval_state *st, uint32_t code, uint64_t v) {
    wr32(st, code, (uint32_t)v);
    wr32(st, code + 1, (uint32_t)(v >> 32));
}

static uint32_t smem_dwords(uint32_t op) {
    switch (op) {
    case SH_OP_S_LOAD_DWORDX2:
    case SH_OP_S_BUFFER_LOAD_DWORDX2: return 2;
    case SH_OP_S_LOAD_DWORDX4:
    case SH_OP_S_BUFFER_LOAD_DWORDX4: return 4;
    case SH_OP_S_LOAD_DWORDX8:
    case SH_OP_S_BUFFER_LOAD_DWORDX8: return 8;
    case SH_OP_S_LOAD_DWORDX16:
    case SH_OP_S_BUFFER_LOAD_DWORDX16: return 16;
    default: return 1;
    }
}

/* Whether instruction i may write VGPR v (overestimated where unsure). */
static bool mip_writes_vgpr(const sh_program *p, uint32_t i, uint32_t v) {
    const rdna2_inst *in = &p->insts[i];
    uint32_t op = p->ops[i], d = in->dst, n;
    if (in->enc == RDNA2_ENC_SOP1 || in->enc == RDNA2_ENC_SOP2 || in->enc == RDNA2_ENC_SOPK ||
        in->enc == RDNA2_ENC_SOPC || in->enc == RDNA2_ENC_SOPP || in->enc == RDNA2_ENC_SMEM ||
        in->enc == RDNA2_ENC_VOPC || in->enc == RDNA2_ENC_EXP || (in->enc == RDNA2_ENC_VOP3 && in->valu < 0x100) ||
        op == SH_OP_V_READLANE_B32 || op == SH_OP_V_READFIRSTLANE_B32) return false;
    if (in->enc == RDNA2_ENC_MUBUF || in->enc == RDNA2_ENC_MTBUF || in->enc == RDNA2_ENC_MIMG ||
        in->enc == RDNA2_ENC_FLAT) {
        int s = p->site_of[i];
        if (s >= 0 && p->sites[s].store && (!in->name || !strstr(in->name, "atomic"))) return false;
        d = RDNA2_OP_VGPR + in->vdata;
        n = p->vmem_width[i] + 1; /* tfe/lwe status included */
    } else if (in->enc == RDNA2_ENC_DS) {
        n = 16; /* LDS result widths overestimated */
    } else {
        n = (p->iflags[i] & SH_IF_64) ? 2 : 1;
    }
    return d >= RDNA2_OP_VGPR && v + RDNA2_OP_VGPR >= d && v + RDNA2_OP_VGPR - d < n;
}

static bool mip_branch(uint32_t op) { return op >= SH_OP_S_BRANCH && op <= SH_OP_S_CBRANCH_EXECNZ; }

/* With branches: the mip register's last write before the image instruction
 * is v_mov_b32 of a constant, every lane reaching the instruction ran it (no
 * branch lands between them except forward from between, EXEC only narrowed
 * by v_cmpx after it). The mip-chain shaders of Demon's Souls write
 * levels 1 and 2 this way after branching on the lane's position. */
static bool mip_flow_literal(const sh_program *p, uint32_t end, uint32_t mip, uint32_t *literal) {
    for (uint32_t i = 0; i < p->count; i++) {
        const char *n = p->insts[i].name;
        if (n && (!strncmp(n, "s_setpc", 7) || !strncmp(n, "s_swappc", 8) || !strncmp(n, "v_movrel", 8) ||
                  !strncmp(n, "s_movrel", 8) || !strncmp(n, "s_set_gpr_idx", 13))) return false;
    }
    uint32_t j = end;
    while (j-- > 0)
        if (mip_writes_vgpr(p, j, mip)) break;
    if (j >= end) return false;
    const rdna2_inst *mov = &p->insts[j];
    uint32_t v;
    if (p->ops[j] != SH_OP_V_MOV_B32 || mov->enc != RDNA2_ENC_VOP1 || mov->abs || mov->neg || mov->opsel ||
        mov->omod || mov->clamp || mov->dst != RDNA2_OP_VGPR + mip || !sh_inline_const(mov->src[0], mov->literal, &v))
        return false;
    for (uint32_t i = 0; i < p->count; i++) {
        if (!mip_branch(p->ops[i])) continue;
        /* Into (write, instruction]: only forward from inside it. From before,
         * it skips the write; from after (a loop), the register may have changed. */
        uint32_t t = p->target[i];
        if (t > j && t <= end && !(i > j && i < t)) return false;
    }
    for (uint32_t i = j + 1; i < end; i++) {
        const rdna2_inst *in = &p->insts[i];
        if (p->ops[i] == SH_OP_V_CMPX) continue;
        bool salu = in->enc == RDNA2_ENC_SOP1 || in->enc == RDNA2_ENC_SOP2 || in->enc == RDNA2_ENC_SOPK;
        uint32_t w = salu ? ((p->iflags[i] & SH_IF_64) ? 2 : 1) : 0;
        if (salu && in->dst <= RDNA2_OP_EXEC_LO + 1 && in->dst + w > RDNA2_OP_EXEC_LO) return false;
        if (salu && in->name && (strstr(in->name, "saveexec") || strstr(in->name, "wrexec"))) return false;
        if ((in->enc == RDNA2_ENC_VOP3 && (p->iflags[i] & SH_IF_CARRY) && in->sdst <= RDNA2_OP_EXEC_LO + 1 &&
             in->sdst + 2 > RDNA2_OP_EXEC_LO) ||
            ((p->ops[i] == SH_OP_V_READLANE_B32 || p->ops[i] == SH_OP_V_READFIRSTLANE_B32) &&
             (in->dst & 127) >= RDNA2_OP_EXEC_LO && (in->dst & 127) <= RDNA2_OP_EXEC_LO + 1))
            return false;
    }
    *literal = v;
    return true;
}

bool sh_image_mip_source(const sh_program *p, const sh_stage_info *info, uint32_t site,
                         uint32_t *source, uint32_t *literal) {
    if (info->stage != SH_STAGE_CS || site >= p->site_count ||
        p->sites[site].kind != SH_SITE_IMAGE) return false;
    uint32_t end = p->sites[site].inst;
    const rdna2_inst *image = &p->insts[end];
    if ((!image->name || (strcmp(image->name, "image_load_mip") && strcmp(image->name, "image_store_mip"))) ||
        image->dim != 1 || image->a16 || image->r128 || image->tfe || image->lwe ||
        (image->nsa_count && image->nsa_count < 2)) return false;
    uint32_t mip = image->nsa_count ? image->nsa_addr[1] : image->vaddr + 2;
    if (mip >= 256) return false;
    if (p->has_flow) {
        if (!mip_flow_literal(p, end, mip, literal)) return false;
        *source = RDNA2_OP_LITERAL;
        return true;
    }
    bool changed[SH_SGPRS] = {0}, known[256] = {0};
    uint32_t src[256] = {0}, val[256] = {0};
    for (uint32_t i = 0; i < end; i++) {
        const rdna2_inst *in = &p->insts[i];
        uint32_t op = p->ops[i];
        if (is_smem(op)) {
            uint32_t count = smem_dwords(op);
            for (uint32_t k = 0; k < count && in->dst + k < SH_SGPRS; k++) changed[in->dst + k] = true;
        } else if (op == SH_OP_S_INST_PREFETCH || op == SH_OP_S_WAITCNT || op == SH_OP_S_NOP ||
                   op == SH_OP_S_CLAUSE || op == SH_OP_V_NOP) {
            /* No data register writes. */
        } else if (op == SH_OP_V_MOV_B32 && in->enc == RDNA2_ENC_VOP1 &&
                   !in->abs && !in->neg && !in->opsel && !in->omod && !in->clamp) {
            if (in->dst < RDNA2_OP_VGPR || in->dst >= RDNA2_OP_VGPR + 256) return false;
            uint32_t d = in->dst - RDNA2_OP_VGPR, s = in->src[0], v;
            known[d] = false;
            if (s < info->cs_tgid_sgpr && s < SH_SGPRS && !changed[s]) {
                known[d] = true;
                src[d] = s;
            } else if (sh_inline_const(s, in->literal, &v)) {
                known[d] = true;
                src[d] = RDNA2_OP_LITERAL;
                val[d] = v;
            }
        } else if (op == SH_OP_V_ADD_NC_U32 || op == SH_OP_V_LSHL_ADD_U32) {
            if (in->dst < RDNA2_OP_VGPR || in->dst >= RDNA2_OP_VGPR + 256) return false;
            known[in->dst - RDNA2_OP_VGPR] = false;
        } else if (op == SH_OP_IMAGE_LOAD && in->name &&
                   (!strcmp(in->name, "image_load") || !strcmp(in->name, "image_load_mip")) && !in->d16 && !in->tfe) {
            uint32_t n = 0;
            for (uint32_t k = 0; k < 4; k++) n += (in->dmask >> k) & 1;
            for (uint32_t k = 0; k < n && in->vdata + k < 256; k++) known[in->vdata + k] = false;
        } else if (op == SH_OP_IMAGE_STORE) {
            /* No VGPR result. */
        } else {
            /* Unknown writes, masks, branches and wider destinations cannot
             * silently preserve a proof about a VGPR. */
            return false;
        }
    }
    if (!known[mip]) return false;
    *source = src[mip];
    *literal = val[mip];
    return true;
}

bool sh_image_mip_desc(const uint32_t desc[8], uint32_t lod, uint32_t out[8]) {
    uint32_t base = (desc[3] >> 12) & 15, last = (desc[3] >> 16) & 15;
    if ((desc[3] >> 28) != 9 || base > last || lod > last - base) return false;
    uint32_t w = ((desc[1] >> 30) | ((desc[2] & 0x3FFF) << 2)) + 1;
    uint32_t h = ((desc[2] >> 14) & 0xFFFF) + 1, size = w > h ? w : h, max_level = 0;
    while (size > 1) { size >>= 1; max_level++; }
    if (base + lod > max_level) return false; /* avoid aliased repeated 1x1 levels */
    memcpy(out, desc, 8 * sizeof(*out));
    out[3] = (out[3] & ~(15u << 12)) | ((base + lod) << 12);
    return true;
}

static bool grow(sh_eval *e, uint32_t words) {
    if (e->smem_words + words <= e->smem_cap) return true;
    uint32_t cap = e->smem_cap ? e->smem_cap : 8192;
    while (cap < e->smem_words + words) cap *= 2;
    uint32_t *d = (uint32_t *)realloc(e->smem_data, (size_t)cap * 4);
    if (!d) return false;
    memset(d + e->smem_cap, 0, (size_t)(cap - e->smem_cap) * 4);
    e->smem_data = d;
    e->smem_cap = cap;
    return true;
}

/* A deferred evaluation (sh_eval.defer) follows the fall-through side of a forward branch whose
 * condition came from a deferred load, where the GPU may take the other side. Every forward
 * branch forks (the other side fills its sites too, first path wins), so the guess only matters
 * when the paths disagree about a site: g_dfr_branch marks the guess, g_fork_conflict a forked
 * path that found a site with other descriptor words, another scalar base or other loaded data.
 * sh_evaluate sets sh_eval.guessed from both at its end. PS5C_EVAL_GUESS_BRANCHES=1: every
 * deferred branch counts as a guess, as before. */
static _Thread_local bool g_dfr_branch, g_fork_conflict;

static void eval_smem(const sh_program *p, uint32_t i, gmem *mem, eval_state *st, sh_eval *e) {
    const rdna2_inst *in = &p->insts[i];
    uint32_t n = smem_dwords(p->ops[i]);
    bool buffer = p->ops[i] >= SH_OP_S_BUFFER_LOAD_DWORD;
    uint32_t sb = in->sbase;
    /* A load replaces the destination's previous dependency. Snapshot the
     * source marks first: the destination may overlap its own descriptor.
     * An unknown offset/base still produces unknown values, even though the
     * CPU can read a representative snapshot at the placeholder address. */
    bool unknown = false;
    for (uint32_t k = 0; k < (buffer ? 4u : 2u) && sb + k < SH_SGPRS; k++)
        unknown |= st->unk[sb + k];
    if (in->soffset < SH_SGPRS) unknown |= st->unk[in->soffset];
    guest_addr base = (guest_addr)st->s[sb] | ((guest_addr)(st->s[sb + 1] & 0xFFFF) << 32);
    uint64_t off = (uint64_t)(int64_t)in->offset;
    if (in->soffset != RDNA2_OP_NULL) off += st->s[in->soffset & 0x7F];
    off &= ~3ull;
    uint64_t limit = UINT64_MAX;
    if (buffer) {
        uint32_t stride = (st->s[sb + 1] >> 16) & 0x3FFF, records = st->s[sb + 2];
        limit = stride ? (uint64_t)records * stride : records;
    }
    int site = p->site_of[i];
    bool record = !(st->fork && site >= 0 && e->site[site].valid);
    /* The translated instruction reads its site's last snapshot through
     * SH_U_SITE, even when scalar control flow visits it repeatedly. Reuse
     * that site's allocation; older snapshots have no remaining readers.
     * Still perform the guest read and scalar register updates on every visit. */
    static int no_compact = -1;
    if(no_compact<0)no_compact=getenv("PS5C_NO_COMPACT_SMEM")!=NULL;
    bool reuse = !no_compact && record && site >= 0 && e->site[site].valid;
    uint32_t at = reuse ? e->site[site].smem_offset : e->smem_words;
    if (record && !reuse && !grow(e, n)) return;
    uint32_t *dst = record ? e->smem_data + at : NULL;
    uint32_t desc[4] = {0};
    if (buffer) memcpy(desc, st->s + sb, sizeof(desc));
    /* one lookup for the whole load (the memory lock is shared with the game's
     * threads); dword by dword only across the end of a mapping */
    bool address_dfr = false;
    for (uint32_t k = 0; k < (buffer ? 4u : 2u) && sb + k < SH_SGPRS; k++) address_dfr |= st->dfr[sb + k];
    /* S_BUFFER_LOAD: the translation reads the bound buffer (all of NUM_RECORDS) at the offset the
     * shader computes, so a deferred offset alone is no guess; its values stay deferred */
    if (address_dfr || !buffer) e->guessed |= address_dfr || (in->soffset < SH_SGPRS && st->dfr[in->soffset]);
    if (in->soffset < SH_SGPRS) address_dfr |= st->dfr[in->soffset];
    /* an offset or base from a deferred load: its values are deferred anyway, so memory the GPU
     * still writes is not waited for just to read a stand-in (KAGE: ~5 ms per evaluation) */
    bool later = buffer && (!unknown || address_dfr) && e->defer && e->defer(e->host_user, base + off, (uint64_t)n * 4);
    if (later) e->deferred = unknown = true; /* the GPU still writes it: values unknown here */
    e->host_sbase = (uint8_t)sb;
    const uint32_t *all = !later && off + (uint64_t)n * 4 <= limit ? (const uint32_t *)eval_host(e, mem, base + off, (uint64_t)n * 4) : NULL;
    for (uint32_t k = 0; k < n; k++) {
        uint64_t o = off + (uint64_t)k * 4;
        uint32_t v = 0;
        if (later) {
        } else if (all) {
            v = all[k];
        } else if (o < limit) {
            const uint32_t *src = (const uint32_t *)eval_host(e, mem, base + o, 4);
            if (src) v = *src;
        }
        if (dst) dst[k] = v;
        if (!record && site >= 0 && !buffer && !later && e->site[site].smem_offset + k < e->smem_words &&
            e->smem_data[e->site[site].smem_offset + k] != v)
            g_fork_conflict = true; /* another path loads other data at this site */
        wr32(st, in->dst + k, v);
        if (in->dst + k < SH_SGPRS && in->dst + k != RDNA2_OP_NULL)
            st->unk[in->dst + k] = unknown, st->dfr[in->dst + k] = later || address_dfr;
    }
    e->host_sbase = 0xFF; /* later reads (descriptor or global loads) are not relative to this base */
    if (!record) {
        if (site >= 0 && buffer && memcmp(e->site[site].desc, desc, sizeof(desc))) g_fork_conflict = true;
        return;
    }
    if (site >= 0) {
        e->site[site].valid = true;
        if (buffer) memcpy(e->site[site].desc, desc, sizeof(desc));
        e->site[site].smem_offset = at;
        e->smem_data[SH_U_SITE + (uint32_t)site] = at;
    }
    if(!reuse)e->smem_words += n;
}

/* The SGPR a VGPR was last computed from before instruction `i` (v_mov or
 * v_add with a scalar operand): how 64-bit pointers reach global accesses. */
static bool vgpr_scalar_source(const sh_program *p, uint32_t i, uint32_t vgpr, uint32_t *sgpr) {
    while (i-- > 0) {
        const rdna2_inst *q = &p->insts[i];
        if (q->enc != RDNA2_ENC_VOP1 && q->enc != RDNA2_ENC_VOP2 && q->enc != RDNA2_ENC_VOP3) continue;
        if (q->dst != RDNA2_OP_VGPR + vgpr) continue;
        for (int k = 0; k < 2; k++) {
            uint32_t src = q->src[k];
            /* SDWA's control word selects scalar sources; the ordinary
             * operand field still looks like a VGPR (or the SDWA marker).
             * Match the operand decoding used by the SPIR-V translator. */
            if (q->src[0] == RDNA2_OP_SDWA) {
                if (k == 0)
                    src = (q->dpp_sdwa & 0xFF) + (((q->dpp_sdwa >> 23) & 1) ? 0 : RDNA2_OP_VGPR);
                else if (q->enc != RDNA2_ENC_VOP1 && src >= RDNA2_OP_VGPR && (q->dpp_sdwa >> 31))
                    src -= RDNA2_OP_VGPR;
            }
            if (src < RDNA2_OP_VCC_LO) {
                *sgpr = src;
                return true;
            }
        }
        return false;
    }
    return false;
}

/* GLOBAL accesses: a buffer site over the memory after the scalar base
 * (saddr, or the SGPR pair the 64-bit address VGPRs came from), up to the
 * end of its mapping. The uniforms entry of the site holds the base low bits. */
#define SH_GLOBAL_WINDOW (4u << 20)

static void eval_global_site(const sh_program *p, uint32_t i, const eval_state *st, gmem *mem, sh_eval *e) {
    const rdna2_inst *in = &p->insts[i];
    int site = p->site_of[i];
    if (site < 0) return;
    if (st->fork && e->site[site].valid) {
        uint32_t lo = in->sbase, hi = in->sbase + 1u;
        if ((in->sbase != RDNA2_OP_NULL ||
             (vgpr_scalar_source(p, i, in->vaddr, &lo) && vgpr_scalar_source(p, i, in->vaddr + 1u, &hi))) &&
            lo < SH_SGPRS && hi < SH_SGPRS && (st->s[lo] != e->site[site].desc[0] || (st->s[hi] & 0xFFFF) != e->site[site].desc[1]))
            g_fork_conflict = true;
        return;
    }
    sh_site_value *v = &e->site[site];
    memset(v->desc, 0, sizeof(v->desc));
    guest_addr base = 0;
    if (in->sbase != RDNA2_OP_NULL) {
        base = (guest_addr)st->s[in->sbase] | ((guest_addr)st->s[in->sbase + 1] << 32);
        e->guessed |= st->dfr[in->sbase] || st->dfr[in->sbase + 1];
    } else {
        uint32_t lo, hi;
        if (!vgpr_scalar_source(p, i, in->vaddr, &lo) || !vgpr_scalar_source(p, i, in->vaddr + 1u, &hi)) return;
        base = (guest_addr)st->s[lo] | ((guest_addr)st->s[hi] << 32);
        e->guessed |= st->dfr[lo] || st->dfr[hi];
    }
    gmem_vma copy, *vma = base && gmem_query_vma(mem, base, false, &copy) ? &copy : NULL; /* copied under the lock */
    if (!vma || vma->reserved || base >= vma->start + vma->size) return;
    uint64_t size = vma->start + vma->size - base;
    if (size > SH_GLOBAL_WINDOW) size = SH_GLOBAL_WINDOW;
    v->valid = true;
    v->desc[0] = (uint32_t)base;
    v->desc[1] = (uint32_t)(base >> 32) & 0xFFFF;
    v->desc[2] = (uint32_t)size;
    e->smem_data[SH_U_SITE + (uint32_t)site] = (uint32_t)base;
}

/* All GDS sites alias one canonical buffer, including reads before a write
 * in a loop. M0 is added by the shader; different values must not create
 * independent snapshots of the same physical counter. Sites conservatively
 * share a writeback; unchanged read-only contents are not copied back. */
static void eval_gds_site(const sh_program *p, uint32_t i, const eval_state *st, sh_eval *e) {
    int site = p->site_of[i];
    if (site < 0 || !sh_gds_base || (st->fork && e->site[site].valid)) return;
    sh_site_value *v = &e->site[site];
    guest_addr base = sh_gds_base;
    memset(v->desc, 0, sizeof(v->desc));
    v->valid = true;
    v->desc[0] = (uint32_t)base;
    v->desc[1] = (uint32_t)(base >> 32) & 0xFFFF;
    v->desc[2] = SH_GDS_SIZE;
}

static void eval_vmem_site(const sh_program *p, uint32_t i, const eval_state *st, sh_eval *e) {
    const rdna2_inst *in = &p->insts[i];
    int site = p->site_of[i];
    if (site < 0) return;
    sh_site_value *v = &e->site[site];
    if (p->sites[site].kind == SH_SITE_BVH) {
        uint32_t desc[4] = {0};
        bool unknown = in->srsrc + 3 >= SH_SGPRS;
        for (uint32_t k = 0; k < 4 && in->srsrc + k < SH_SGPRS; k++) {
            desc[k] = st->s[in->srsrc + k]; unknown |= st->unk[in->srsrc + k];
            e->guessed |= st->dfr[in->srsrc + k];
        }
        if (v->valid) {
            g_fork_conflict |= memcmp(v->desc, desc, sizeof(desc)) != 0;
            v->desc_unknown |= unknown || memcmp(v->desc, desc, sizeof(desc)) != 0;
            return;
        }
        uint32_t at = e->smem_words;
        if (!grow(e, 4)) return;
        memcpy(e->smem_data + at, desc, sizeof(desc));
        e->smem_words += 4;
        e->smem_data[SH_U_SITE + (uint32_t)site] = at;
        memcpy(v->desc, desc, sizeof(desc));
        v->desc_unknown = unknown; v->valid = true;
        return;
    }
    bool buffer = p->sites[site].kind == SH_SITE_BUFFER;
    uint32_t soff = buffer && in->soffset != RDNA2_OP_NULL ? rd32(st, in, in->soffset) : 0;
    bool soff_known = buffer && !(in->soffset < SH_SGPRS && st->unk[in->soffset]);
    bool desc_unknown = false, records_unknown = false;
    for (uint32_t k = 0; buffer && k < 4 && in->srsrc + k < SH_SGPRS; k++) {
        if (k == 2) records_unknown = st->unk[in->srsrc + k];
        else desc_unknown |= st->unk[in->srsrc + k];
    }
    static int log_unknown = -1; /* PS5C_LOG_UNKNOWN_DESC: which SGPRs (debugging aid) */
    if (log_unknown < 0) log_unknown = getenv("PS5C_LOG_UNKNOWN_DESC") != NULL;
    if ((desc_unknown || records_unknown) && log_unknown) {
        static uint32_t logged;
        if (logged++ < 32)
            LOG_I(GPU, "shader 0x%llx +0x%x: descriptor s[%u:%u] unknown %d%d%d%d%s", (unsigned long long)p->base, p->pc[i] * 4,
                  in->srsrc, in->srsrc + 3, st->unk[in->srsrc], st->unk[in->srsrc + 1], st->unk[in->srsrc + 2],
                  st->unk[in->srsrc + 3], st->fork ? " (fork)" : "");
    }
    /* descriptor words or the offset from a deferred load (sh_eval.guessed): a V# has 4 words,
     * a T# 8 (4 with r128) */
    uint32_t desc_words = p->sites[site].kind == SH_SITE_IMAGE ? (in->r128 ? 4u : 8u) : 4u;
    for (uint32_t k = 0; k < desc_words || (p->sites[site].kind == SH_SITE_IMAGE && k < 4); k++)
        e->guessed |= (k < desc_words && in->srsrc + k < SH_SGPRS && st->dfr[in->srsrc + k]) ||
                      (k < 4 && p->sites[site].kind == SH_SITE_IMAGE && in->ssamp + k < SH_SGPRS && st->dfr[in->ssamp + k]);
    /* only buffer instructions have an SOFFSET (an image instruction's field reads 0, s0) */
    if (in->enc == RDNA2_ENC_MUBUF || in->enc == RDNA2_ENC_MTBUF)
        e->guessed |= in->soffset < SH_SGPRS && st->dfr[in->soffset];
    if (st->fork && v->valid) { /* another path: the offset stays known only if it agrees */
        uint32_t n = p->sites[site].kind == SH_SITE_IMAGE ? (in->r128 ? 4u : 8u) : 4u;
        for (uint32_t k = 0; k < n && in->srsrc + k < SH_SGPRS; k++) g_fork_conflict |= v->desc[k] != st->s[in->srsrc + k];
        for (uint32_t k = 0; p->sites[site].kind == SH_SITE_IMAGE && k < 4 && in->ssamp + k < SH_SGPRS; k++)
            g_fork_conflict |= v->sampler[k] != st->s[in->ssamp + k];
        if (!soff_known || soff != v->soffset) v->soffset_known = false;
        v->desc_unknown |= desc_unknown;
        v->records_unknown |= records_unknown;
        return;
    }
    v->desc_unknown = desc_unknown;
    v->records_unknown = records_unknown;
    v->valid = true;
    v->soffset = soff;
    v->soffset_known = soff_known;
    uint32_t n = p->sites[site].kind == SH_SITE_IMAGE ? (in->r128 ? 4u : 8u) : 4u;
    memset(v->desc, 0, sizeof(v->desc));
    for (uint32_t k = 0; k < n && in->srsrc + k < SH_SGPRS; k++) v->desc[k] = st->s[in->srsrc + k];
    if (p->sites[site].kind == SH_SITE_IMAGE) {
        for (uint32_t k = 0; k < 4 && in->ssamp + k < SH_SGPRS; k++) v->sampler[k] = st->s[in->ssamp + k];
    }
}

/* Executes one scalar ALU instruction. Returns false if it is not scalar. */
static bool eval_salu(const sh_program *p, uint32_t i, eval_state *st) {
    const rdna2_inst *in = &p->insts[i];
    uint32_t op = p->ops[i];
    uint32_t a = 0, b = 0;
    uint64_t a64 = 0, b64 = 0;
    if (in->enc == RDNA2_ENC_SOP2 || in->enc == RDNA2_ENC_SOPC) {
        a = rd32(st, in, in->src[0]);
        b = rd32(st, in, in->src[1]);
        if (p->iflags[i] & SH_IF_64) {
            a64 = rd64(st, in, in->src[0]);
            b64 = rd64(st, in, in->src[1]);
        }
    } else if (in->enc == RDNA2_ENC_SOP1) {
        a = rd32(st, in, in->src[0]);
        if (p->iflags[i] & SH_IF_64) a64 = rd64(st, in, in->src[0]);
    } else if (in->enc == RDNA2_ENC_SOPK) {
        a = st->s[in->dst & 0x7F];
        b = (uint32_t)in->simm16;
        if (op >= SH_OP_S_CMPK_EQ_U32 && op <= SH_OP_S_CMPK_LE_U32 && op != SH_OP_S_CMPK_GT_I32 &&
            op != SH_OP_S_CMPK_GE_I32 && op != SH_OP_S_CMPK_LT_I32 && op != SH_OP_S_CMPK_LE_I32)
            b &= 0xFFFF; /* unsigned compares zero-extend the immediate */
    } else {
        return false;
    }
    uint32_t d = in->dst;
    uint64_t exec = rd64(st, in, RDNA2_OP_EXEC_LO);
    switch (op) {
    case SH_OP_S_WQM_B32:
    case SH_OP_S_WQM_B64: {
        /* RDNA2 SOP1: activate a whole quad when any of its four bits is set,
         * and publish SCC from the resulting mask. This is a wave mask in the
         * CPU evaluator, unlike the per-invocation bit used by SPIR-V. */
        uint64_t src=op==SH_OP_S_WQM_B32?a:a64,result=0;
        unsigned bits=op==SH_OP_S_WQM_B32?32:64;
        for(unsigned bit=0;bit<bits;bit+=4)if((src>>bit)&15)result|=15ull<<bit;
        if(bits==32)wr32(st,d,(uint32_t)result);else wr64(st,d,result);
        st->scc=result!=0;break;
    }
    case SH_OP_S_MOV_B32: wr32(st, d, a); break;
    case SH_OP_S_MOV_B64: wr64(st, d, a64); break;
    case SH_OP_S_CMOV_B32: if (st->scc) wr32(st, d, a); break;
    case SH_OP_S_MOVK_I32: wr32(st, d, (uint32_t)in->simm16); break;
    case SH_OP_S_NOT_B32: wr32(st, d, ~a); st->scc = ~a != 0; break;
    case SH_OP_S_NOT_B64: wr64(st, d, ~a64); st->scc = ~a64 != 0; break;
    case SH_OP_S_BREV_B32: {
        uint32_t r = 0;
        for (int k = 0; k < 32; k++) r |= ((a >> k) & 1u) << (31 - k);
        wr32(st, d, r);
        break;
    }
    case SH_OP_S_BCNT1_I32_B32: {
        uint32_t r = (uint32_t)__builtin_popcount(a);
        wr32(st, d, r); st->scc = r != 0; break;
    }
    case SH_OP_S_BCNT1_I32_B64: {
        uint32_t r = (uint32_t)__builtin_popcountll(a64);
        wr32(st, d, r); st->scc = r != 0; break;
    }
    case SH_OP_S_FF1_I32_B32: wr32(st, d, a ? (uint32_t)__builtin_ctz(a) : 0xFFFFFFFFu); break;
    case SH_OP_S_FF1_I32_B64: wr32(st, d, a64 ? (uint32_t)__builtin_ctzll(a64) : 0xFFFFFFFFu); break;
    case SH_OP_S_FLBIT_I32_B32: wr32(st, d, a ? (uint32_t)__builtin_clz(a) : 0xFFFFFFFFu); break;
    case SH_OP_S_FLBIT_I32_B64: wr32(st, d, a64 ? (uint32_t)__builtin_clzll(a64) : 0xFFFFFFFFu); break;
    case SH_OP_S_BITREPLICATE_B64_B32: {
        uint64_t r = 0;
        for (int k = 0; k < 32; k++) r |= (uint64_t)((a >> k) & 1) * (3ull << (2 * k));
        wr64(st, d, r);
        break;
    }
    case SH_OP_S_SEXT_I32_I8: wr32(st, d, (uint32_t)(int32_t)(int8_t)a); break;
    case SH_OP_S_SEXT_I32_I16: wr32(st, d, (uint32_t)(int32_t)(int16_t)a); break;
    case SH_OP_S_ABS_I32: {
        uint32_t r = (int32_t)a < 0 ? (uint32_t)(-(int32_t)a) : a;
        wr32(st, d, r); st->scc = r != 0; break;
    }
    case SH_OP_S_GETPC_B64: wr64(st, d, sh_inst_address(p,i) + (uint64_t)in->words * 4); break;
    case SH_OP_S_ADD_U32: { uint64_t r = (uint64_t)a + b; wr32(st, d, (uint32_t)r); st->scc = r >> 32; break; }
    case SH_OP_S_SUB_U32: wr32(st, d, a - b); st->scc = b > a; break;
    case SH_OP_S_ADD_I32: {
        uint32_t r = a + b;
        wr32(st, d, r); st->scc = ((~(a ^ b)) & (a ^ r)) >> 31; break;
    }
    case SH_OP_S_SUB_I32: {
        uint32_t r = a - b;
        wr32(st, d, r); st->scc = ((a ^ b) & (a ^ r)) >> 31; break;
    }
    case SH_OP_S_ADDC_U32: {
        uint64_t r = (uint64_t)a + b + st->scc;
        wr32(st, d, (uint32_t)r); st->scc = r >> 32; break;
    }
    case SH_OP_S_SUBB_U32: {
        uint64_t r = (uint64_t)a - b - st->scc;
        wr32(st, d, (uint32_t)r); st->scc = (uint64_t)b + st->scc > a; break;
    }
    case SH_OP_S_ADDK_I32: wr32(st, d, a + b); break;
    case SH_OP_S_MULK_I32: wr32(st, d, a * b); break;
    case SH_OP_S_MIN_I32: st->scc = (int32_t)a < (int32_t)b; wr32(st, d, st->scc ? a : b); break;
    case SH_OP_S_MIN_U32: st->scc = a < b; wr32(st, d, st->scc ? a : b); break;
    case SH_OP_S_MAX_I32: st->scc = (int32_t)a > (int32_t)b; wr32(st, d, st->scc ? a : b); break;
    case SH_OP_S_MAX_U32: st->scc = a > b; wr32(st, d, st->scc ? a : b); break;
#define LOGIC32(OPID, EXPR) case OPID: { uint32_t r = (EXPR); wr32(st, d, r); st->scc = r != 0; break; }
#define LOGIC64(OPID, EXPR) case OPID: { uint64_t r = (EXPR); wr64(st, d, r); st->scc = r != 0; break; }
    LOGIC32(SH_OP_S_AND_B32, a & b)
    LOGIC32(SH_OP_S_OR_B32, a | b)
    LOGIC32(SH_OP_S_XOR_B32, a ^ b)
    LOGIC32(SH_OP_S_ANDN2_B32, a & ~b)
    LOGIC32(SH_OP_S_ORN2_B32, a | ~b)
    LOGIC32(SH_OP_S_NAND_B32, ~(a & b))
    LOGIC32(SH_OP_S_NOR_B32, ~(a | b))
    LOGIC32(SH_OP_S_XNOR_B32, ~(a ^ b))
    LOGIC32(SH_OP_S_LSHL_B32, a << (b & 31))
    LOGIC32(SH_OP_S_LSHR_B32, a >> (b & 31))
    LOGIC32(SH_OP_S_ASHR_I32, (uint32_t)((int32_t)a >> (b & 31)))
    LOGIC32(SH_OP_S_BFE_U32, ((b >> 16) & 0x7F) ? (a >> (b & 31)) & (uint32_t)((1ull << ((b >> 16) & 0x7F)) - 1) : 0)
    LOGIC32(SH_OP_S_LSHL1_ADD_U32, (a << 1) + b)
    LOGIC32(SH_OP_S_LSHL2_ADD_U32, (a << 2) + b)
    LOGIC32(SH_OP_S_LSHL3_ADD_U32, (a << 3) + b)
    LOGIC32(SH_OP_S_LSHL4_ADD_U32, (a << 4) + b)
    LOGIC64(SH_OP_S_AND_B64, a64 & b64)
    LOGIC64(SH_OP_S_OR_B64, a64 | b64)
    LOGIC64(SH_OP_S_XOR_B64, a64 ^ b64)
    LOGIC64(SH_OP_S_ANDN2_B64, a64 & ~b64)
    LOGIC64(SH_OP_S_ORN2_B64, a64 | ~b64)
    LOGIC64(SH_OP_S_NAND_B64, ~(a64 & b64))
    LOGIC64(SH_OP_S_NOR_B64, ~(a64 | b64))
    LOGIC64(SH_OP_S_XNOR_B64, ~(a64 ^ b64))
    LOGIC64(SH_OP_S_LSHL_B64, a64 << (b & 63))
    LOGIC64(SH_OP_S_LSHR_B64, a64 >> (b & 63))
    LOGIC64(SH_OP_S_BFE_U64, ((b >> 16) & 0x7F) ? (a64 >> (b & 63)) & (((b >> 16) & 0x7F) >= 64 ? ~0ull : ((1ull << ((b >> 16) & 0x7F)) - 1)) : 0)
#undef LOGIC32
#undef LOGIC64
    case SH_OP_S_BFE_I32: {
        uint32_t o = b & 31, w = (b >> 16) & 0x7F;
        if (w > 32 - o) w = 32 - o;
        int32_t r = w ? (int32_t)(a << (32 - o - w)) >> (32 - w) : 0;
        wr32(st, d, (uint32_t)r); st->scc = r != 0; break;
    }
    case SH_OP_S_BFM_B32: wr32(st, d, (uint32_t)(((1ull << (a & 31)) - 1) << (b & 31))); break;
    case SH_OP_S_MUL_I32: wr32(st, d, a * b); break;
    case SH_OP_S_MUL_HI_U32: wr32(st, d, (uint32_t)(((uint64_t)a * b) >> 32)); break;
    case SH_OP_S_MUL_HI_I32: wr32(st, d, (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 32)); break;
    case SH_OP_S_CSELECT_B32: wr32(st, d, st->scc ? a : b); break;
    case SH_OP_S_CSELECT_B64: wr64(st, d, st->scc ? a64 : b64); break;
    case SH_OP_S_ABSDIFF_I32: {
        int32_t r = (int32_t)a - (int32_t)b;
        if (r < 0) r = -r;
        wr32(st, d, (uint32_t)r); st->scc = r != 0; break;
    }
    case SH_OP_S_PACK_LL_B32_B16: wr32(st, d, (a & 0xFFFF) | (b << 16)); break;
    case SH_OP_S_PACK_LH_B32_B16: wr32(st, d, (a & 0xFFFF) | (b & 0xFFFF0000u)); break;
    case SH_OP_S_PACK_HH_B32_B16: wr32(st, d, (a >> 16) | (b & 0xFFFF0000u)); break;
    case SH_OP_S_BITSET0_B32: wr32(st, d, rd32(st, in, d) & ~(1u << (a & 31))); break;
    case SH_OP_S_BITSET1_B32: wr32(st, d, rd32(st, in, d) | (1u << (a & 31))); break;
    case SH_OP_S_SETREG_B32:
    case SH_OP_S_SETREG_IMM32_B32: break; /* mode registers: no effect here */
    case SH_OP_S_GETREG_B32: wr32(st, d, 0); break;
    case SH_OP_S_ORN2_SAVEEXEC_B64: case SH_OP_S_NAND_SAVEEXEC_B64: case SH_OP_S_NOR_SAVEEXEC_B64:
    case SH_OP_S_XNOR_SAVEEXEC_B64: case SH_OP_S_ANDN1_SAVEEXEC_B64: case SH_OP_S_ORN1_SAVEEXEC_B64: {
        wr64(st, d, exec);
        uint64_t r = op == SH_OP_S_ORN2_SAVEEXEC_B64 ? a64 | ~exec : op == SH_OP_S_NAND_SAVEEXEC_B64 ? ~(a64 & exec)
                   : op == SH_OP_S_NOR_SAVEEXEC_B64 ? ~(a64 | exec) : op == SH_OP_S_XNOR_SAVEEXEC_B64 ? ~(a64 ^ exec)
                   : op == SH_OP_S_ANDN1_SAVEEXEC_B64 ? ~a64 & exec : ~a64 | exec;
        wr64(st, RDNA2_OP_EXEC_LO, r);
        st->scc = r != 0;
        break;
    }
    case SH_OP_S_ORN2_SAVEEXEC_B32: case SH_OP_S_NAND_SAVEEXEC_B32: case SH_OP_S_NOR_SAVEEXEC_B32:
    case SH_OP_S_XNOR_SAVEEXEC_B32: case SH_OP_S_ANDN1_SAVEEXEC_B32: case SH_OP_S_ORN1_SAVEEXEC_B32: {
        uint32_t e32 = (uint32_t)exec;
        wr32(st, d, e32);
        uint32_t r = op == SH_OP_S_ORN2_SAVEEXEC_B32 ? a | ~e32 : op == SH_OP_S_NAND_SAVEEXEC_B32 ? ~(a & e32)
                   : op == SH_OP_S_NOR_SAVEEXEC_B32 ? ~(a | e32) : op == SH_OP_S_XNOR_SAVEEXEC_B32 ? ~(a ^ e32)
                   : op == SH_OP_S_ANDN1_SAVEEXEC_B32 ? ~a & e32 : ~a | e32;
        wr32(st, RDNA2_OP_EXEC_LO, r);
        st->scc = r != 0;
        break;
    }
    case SH_OP_S_AND_SAVEEXEC_B64:
    case SH_OP_S_OR_SAVEEXEC_B64:
    case SH_OP_S_XOR_SAVEEXEC_B64:
    case SH_OP_S_ANDN2_SAVEEXEC_B64: {
        wr64(st, d, exec);
        uint64_t r = op == SH_OP_S_AND_SAVEEXEC_B64 ? a64 & exec : op == SH_OP_S_OR_SAVEEXEC_B64 ? a64 | exec
                   : op == SH_OP_S_XOR_SAVEEXEC_B64 ? a64 ^ exec : a64 & ~exec;
        wr64(st, RDNA2_OP_EXEC_LO, r);
        st->scc = r != 0;
        break;
    }
    case SH_OP_S_AND_SAVEEXEC_B32:
    case SH_OP_S_OR_SAVEEXEC_B32:
    case SH_OP_S_XOR_SAVEEXEC_B32:
    case SH_OP_S_ANDN2_SAVEEXEC_B32: {
        uint32_t e32 = (uint32_t)exec;
        wr32(st, d, e32);
        uint32_t r = op == SH_OP_S_AND_SAVEEXEC_B32 ? a & e32 : op == SH_OP_S_OR_SAVEEXEC_B32 ? a | e32
                   : op == SH_OP_S_XOR_SAVEEXEC_B32 ? a ^ e32 : a & ~e32;
        wr32(st, RDNA2_OP_EXEC_LO, r);
        st->scc = r != 0;
        break;
    }
    case SH_OP_S_CMP_EQ_I32: case SH_OP_S_CMP_EQ_U32: case SH_OP_S_CMPK_EQ_I32: case SH_OP_S_CMPK_EQ_U32:
        st->scc = a == b; break;
    case SH_OP_S_CMP_LG_I32: case SH_OP_S_CMP_LG_U32: case SH_OP_S_CMPK_LG_I32: case SH_OP_S_CMPK_LG_U32:
        st->scc = a != b; break;
    case SH_OP_S_CMP_GT_I32: case SH_OP_S_CMPK_GT_I32: st->scc = (int32_t)a > (int32_t)b; break;
    case SH_OP_S_CMP_GE_I32: case SH_OP_S_CMPK_GE_I32: st->scc = (int32_t)a >= (int32_t)b; break;
    case SH_OP_S_CMP_LT_I32: case SH_OP_S_CMPK_LT_I32: st->scc = (int32_t)a < (int32_t)b; break;
    case SH_OP_S_CMP_LE_I32: case SH_OP_S_CMPK_LE_I32: st->scc = (int32_t)a <= (int32_t)b; break;
    case SH_OP_S_CMP_GT_U32: case SH_OP_S_CMPK_GT_U32: st->scc = a > b; break;
    case SH_OP_S_CMP_GE_U32: case SH_OP_S_CMPK_GE_U32: st->scc = a >= b; break;
    case SH_OP_S_CMP_LT_U32: case SH_OP_S_CMPK_LT_U32: st->scc = a < b; break;
    case SH_OP_S_CMP_LE_U32: case SH_OP_S_CMPK_LE_U32: st->scc = a <= b; break;
    case SH_OP_S_QUADMASK_B32:
    case SH_OP_S_QUADMASK_B64: {
        uint64_t v = op == SH_OP_S_QUADMASK_B32 ? a : a64, r = 0;
        for (uint32_t q = 0; q < 16; q++)
            if ((v >> (4 * q)) & 0xF) r |= 1ull << q;
        if (op == SH_OP_S_QUADMASK_B32) wr32(st, d, (uint32_t)r);
        else wr64(st, d, r);
        st->scc = r != 0;
        break;
    }
    case SH_OP_S_CMP_EQ_U64: st->scc = a64 == b64; break;
    case SH_OP_S_CMP_LG_U64: st->scc = a64 != b64; break;
    case SH_OP_S_BITCMP0_B32: st->scc = ((a >> (b & 31)) & 1) == 0; break;
    case SH_OP_S_BITCMP1_B32: st->scc = ((a >> (b & 31)) & 1) != 0; break;
    case SH_OP_S_BITCMP0_B64: st->scc = ((a64 >> (b & 63)) & 1) == 0; break;
    case SH_OP_S_BITCMP1_B64: st->scc = ((a64 >> (b & 63)) & 1) != 0; break;
    case SH_OP_S_ASHR_I64: {
        uint64_t r = (uint64_t)((int64_t)a64 >> (b & 63));
        wr64(st, d, r); st->scc = r != 0; break;
    }
    case SH_OP_S_BFE_I64: {
        uint32_t off = b & 63, w = (b >> 16) & 0x7F;
        if (w > 64 - off) w = 64 - off;
        uint64_t r = w ? (uint64_t)((int64_t)(a64 << (64 - off - w)) >> (64 - w)) : 0;
        wr64(st, d, r); st->scc = r != 0; break;
    }
    case SH_OP_S_CMOVK_I32: if (st->scc) wr32(st, d, b); break;
    case SH_OP_S_CMOV_B64: if (st->scc) wr64(st, d, a64); break;
    case SH_OP_S_BCNT0_I32_B32: { uint32_t r = (uint32_t)__builtin_popcount(~a); wr32(st, d, r); st->scc = r != 0; break; }
    case SH_OP_S_BCNT0_I32_B64: { uint32_t r = (uint32_t)__builtin_popcountll(~a64); wr32(st, d, r); st->scc = r != 0; break; }
    case SH_OP_S_FF0_I32_B32: wr32(st, d, ~a ? (uint32_t)__builtin_ctz(~a) : 0xFFFFFFFFu); break;
    case SH_OP_S_FF0_I32_B64: wr32(st, d, ~a64 ? (uint32_t)__builtin_ctzll(~a64) : 0xFFFFFFFFu); break;
    case SH_OP_S_FLBIT_I32: {
        uint32_t x = a ^ (uint32_t)((int32_t)a >> 31);
        wr32(st, d, x ? (uint32_t)__builtin_clz(x) : 0xFFFFFFFFu);
        break;
    }
    case SH_OP_S_FLBIT_I32_I64: {
        uint64_t x = a64 ^ (uint64_t)((int64_t)a64 >> 63);
        wr32(st, d, x ? (uint32_t)__builtin_clzll(x) : 0xFFFFFFFFu);
        break;
    }
    case SH_OP_S_BREV_B64: {
        uint64_t r = 0;
        for (int k = 0; k < 64; k++) r |= ((a64 >> k) & 1u) << (63 - k);
        wr64(st, d, r);
        break;
    }
    case SH_OP_S_BITSET0_B64: wr64(st, d, rd64(st, in, d) & ~(1ull << (a & 63))); break;
    case SH_OP_S_BITSET1_B64: wr64(st, d, rd64(st, in, d) | (1ull << (a & 63))); break;
    case SH_OP_S_ANDN1_WREXEC_B64: case SH_OP_S_ANDN2_WREXEC_B64: {
        uint64_t r = op == SH_OP_S_ANDN1_WREXEC_B64 ? ~a64 & exec : a64 & ~exec;
        wr64(st, RDNA2_OP_EXEC_LO, r);
        wr64(st, d, r);
        st->scc = r != 0;
        break;
    }
    case SH_OP_S_ANDN1_WREXEC_B32: case SH_OP_S_ANDN2_WREXEC_B32: {
        uint32_t e32 = (uint32_t)exec, r = op == SH_OP_S_ANDN1_WREXEC_B32 ? ~a & e32 : a & ~e32;
        wr32(st, RDNA2_OP_EXEC_LO, r);
        wr32(st, d, r);
        st->scc = r != 0;
        break;
    }
    default: return false;
    }
    return true;
}

static bool operand_unknown(const eval_state *st, uint32_t code, bool wide) {
    /* NULL reads 0: compares that write it (an SDST of NULL) mark it, and it is also the
     * source field of instructions without one (SOP1's src1, SOPK) */
    if (code == RDNA2_OP_NULL) return false;
    if (code < SH_SGPRS) return st->unk[code] || (wide && code + 1 < SH_SGPRS && code + 1 != RDNA2_OP_NULL && st->unk[code + 1]);
    if (code == RDNA2_OP_SCC) return st->scc_unk;
    if (code == RDNA2_OP_EXECZ) return st->unk[126] || (!st->wave32 && st->unk[127]);
    if (code == RDNA2_OP_VCCZ) return st->unk[106] || (!st->wave32 && st->unk[107]);
    return false;
}

/* The same for the deferred marks (eval_state.dfr), VGPR operands included */
static bool vdfr_get(const eval_state *st, uint32_t v) {
    if(v>=256)return false;
    return ((st->vdfr[v>>5]>>(v&31))&1)!=0;
}
static void vdfr_set(eval_state *st, uint32_t v, bool d) {
    if (v >= 256) return;
    if (d) st->vdfr[v >> 5] |= 1u << (v & 31);
    else st->vdfr[v >> 5] &= ~(1u << (v & 31));
}
static bool operand_dfr(const eval_state *st, uint32_t code, bool wide) {
    if (code == RDNA2_OP_NULL) return false;
    if (code < SH_SGPRS) return st->dfr[code] || (wide && code + 1 < SH_SGPRS && code + 1 != RDNA2_OP_NULL && st->dfr[code + 1]);
    if (code == RDNA2_OP_SCC) return st->scc_dfr;
    if (code == RDNA2_OP_EXECZ) return st->dfr[126] || (!st->wave32 && st->dfr[127]);
    if (code == RDNA2_OP_VCCZ) return st->dfr[106] || (!st->wave32 && st->dfr[107]);
    if (code >= RDNA2_OP_VGPR && code < RDNA2_OP_VGPR + 256)
        return vdfr_get(st, code - RDNA2_OP_VGPR) || (wide && vdfr_get(st, code - RDNA2_OP_VGPR + 1));
    return false;
}

static bool has_prefix(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

/* Whether vector instruction j reads a deferred value (overestimated: SDWA/DPP
 * selectors count as an SGPR and a VGPR; EXEC, M0 and the implicit VCC count) */
static bool vector_dfr(const sh_program *p, uint32_t j, const eval_state *st) {
    const rdna2_inst *in = &p->insts[j];
    bool wide = (p->iflags[j] & SH_IF_64) != 0;
    bool d = st->dfr[RDNA2_OP_EXEC_LO] || st->dfr[RDNA2_OP_EXEC_LO + 1] || st->dfr[124];
    for (uint32_t k = 0; k < 3; k++) {
        uint32_t code = in->src[k];
        if (code == RDNA2_OP_SDWA || code == RDNA2_OP_DPP16 || code == RDNA2_OP_DPP8 || code == RDNA2_OP_DPP8FI) {
            uint32_t sel = in->dpp_sdwa & 0xFF;
            d |= vdfr_get(st, sel) || st->dfr[sel & 0x7F];
        } else {
            d |= operand_dfr(st, code, wide);
        }
    }
    if(p->ops[j]==SH_OP_V_MOVRELS_B32 || p->ops[j]==SH_OP_V_MOVRELSD_B32) {
        uint32_t base=in->src[0]>=RDNA2_OP_VGPR?in->src[0]-RDNA2_OP_VGPR:256;
        if(st->unk[124] || base>=256 || st->s[124]>=256 || base>=256-st->s[124]) {
            for(unsigned k=0;k<8;k++)d|=st->vdfr[k]!=0;
        } else d|=vdfr_get(st,base+st->s[124]);
    }
    if (in->src[0] == RDNA2_OP_SDWA && in->src[1] >= RDNA2_OP_VGPR) d |= st->dfr[(in->src[1] - RDNA2_OP_VGPR) & 0x7F];
    const char *name = in->name ? in->name : "";
    if (in->enc == RDNA2_ENC_VOP2 && (has_prefix(name, "v_cndmask") || strstr(name, "_co_ci")))
        d |= st->dfr[RDNA2_OP_VCC_LO] || st->dfr[RDNA2_OP_VCC_LO + 1];
    return d;
}

/* A vector ALU result is deferred when an operand is */
static void dfr_vector(const sh_program *p, uint32_t j, eval_state *st) {
    const rdna2_inst *in = &p->insts[j];
    if(p->ops[j]==SH_OP_V_NOP)return;
    if (in->enc != RDNA2_ENC_VOP1 && in->enc != RDNA2_ENC_VOP2 && in->enc != RDNA2_ENC_VOP3 &&
        in->enc != RDNA2_ENC_VOP3P && in->enc != RDNA2_ENC_VINTRP)
        return; /* memory results are vector data the evaluation never knows anyway */
    if (in->dst < RDNA2_OP_VGPR || in->dst >= RDNA2_OP_VGPR + 256) return;
    bool d = vector_dfr(p, j, st);
    if(p->ops[j]==SH_OP_V_MOVRELD_B32 || p->ops[j]==SH_OP_V_MOVRELSD_B32) {
        uint32_t first=in->dst-RDNA2_OP_VGPR;
        if(st->unk[124] || st->s[124]>=256 || first>=256-st->s[124]) {
            if(d)for(unsigned k=0;k<8;k++)st->vdfr[k]=UINT32_MAX;
            return;
        }
        first+=st->s[124];vdfr_set(st,first,d|vdfr_get(st,first));return;
    }
    if(p->ops[j]==SH_OP_V_SWAP_B32 && in->src[0]>=RDNA2_OP_VGPR && in->src[0]<RDNA2_OP_VGPR+256) {
        uint32_t dst=in->dst-RDNA2_OP_VGPR,src=in->src[0]-RDNA2_OP_VGPR;
        d|=vdfr_get(st,dst)|vdfr_get(st,src);
        vdfr_set(st,dst,d);vdfr_set(st,src,d);return;
    }
    bool full=!st->unk[RDNA2_OP_EXEC_LO] && st->s[RDNA2_OP_EXEC_LO]==UINT32_MAX &&
        (st->wave32 || (!st->unk[RDNA2_OP_EXEC_HI] && st->s[RDNA2_OP_EXEC_HI]==UINT32_MAX));
    bool preserve=!full || in->src[0]==RDNA2_OP_SDWA || in->src[0]==RDNA2_OP_DPP8 ||
        in->src[0]==RDNA2_OP_DPP8FI || in->src[0]==RDNA2_OP_DPP16 || in->d16 ||
        (in->name && strstr(in->name,"16"));
    uint32_t dst=in->dst-RDNA2_OP_VGPR;
    vdfr_set(st,dst,d|(preserve && vdfr_get(st,dst)));
    if(p->iflags[j]&SH_IF_64)vdfr_set(st,dst+1,d|(preserve && vdfr_get(st,dst+1)));
}

/* Scalar instructions that leave SCC as it is. */
static bool keeps_scc(const char *name) {
    static const char *const k[] = {"s_mov", "s_cmov", "s_cselect", "s_movk", "s_cmovk", "s_getpc", "s_setpc",
                                    "s_swappc", "s_mul_", "s_mulk", "s_bfm", "s_pack", "s_sext", "s_ff0", "s_ff1",
                                    "s_flbit", "s_brev", "s_bitset", "s_getreg", "s_setreg", "s_movrel", "s_version"};
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        if (has_prefix(name, k[i])) return true;
    return false;
}

enum { OPF_KNOWN = 1, OPF_READS_SCC = 2, OPF_SAVEEXEC = 4, OPF_WIDE = 8, OPF_KEEPS_SCC = 16, OPF_WREXEC = 32, OPF_CMPK = 64 };

/* Per-opcode properties of scalar instructions, from their names (computed once). */
static uint8_t op_flags(uint32_t op, const char *name) {
    static uint8_t cache[SH_OP_COUNT];
    if (op >= SH_OP_COUNT) return 0;
    if (cache[op]) return cache[op];
    uint8_t f = OPF_KNOWN;
    if (has_prefix(name, "s_cselect") || has_prefix(name, "s_cmov") || has_prefix(name, "s_addc") ||
        has_prefix(name, "s_subb"))
        f |= OPF_READS_SCC;
    if (strstr(name, "saveexec")) f |= OPF_SAVEEXEC;
    if (strstr(name, "wrexec")) f |= OPF_WREXEC;
    if (strstr(name, "_b64") || strstr(name, "_u64") || strstr(name, "_i64")) f |= OPF_WIDE;
    if (keeps_scc(name)) f |= OPF_KEEPS_SCC;
    if (has_prefix(name, "s_cmpk")) f |= OPF_CMPK;
    cache[op] = f;
    return f;
}

/* Runs one scalar ALU instruction and propagates the unknown marks. */
static bool eval_salu_tracked(const sh_program *p, uint32_t i, eval_state *st) {
    const rdna2_inst *in = &p->insts[i];
    uint32_t op = p->ops[i];
    const char *name = in->name ? in->name : "";
    uint8_t f = op_flags(op, name);
    bool wide = (f & OPF_WIDE) != 0, src0_wide = wide, src1_wide = wide;
    /* Result width and source width are not interchangeable. Counting/searching
     * a 64-bit input writes one SGPR; shifts and bitfield controls read one. */
    switch (op) {
    case SH_OP_S_BCNT0_I32_B64: case SH_OP_S_BCNT1_I32_B64:
    case SH_OP_S_FF0_I32_B64: case SH_OP_S_FF1_I32_B64:
    case SH_OP_S_FLBIT_I32_B64: case SH_OP_S_FLBIT_I32_I64:
        wide = false; break;
    case SH_OP_S_LSHL_B64: case SH_OP_S_LSHR_B64: case SH_OP_S_ASHR_I64:
    case SH_OP_S_BFE_U64: case SH_OP_S_BFE_I64:
    case SH_OP_S_BITCMP0_B64: case SH_OP_S_BITCMP1_B64:
        src1_wide = false; break;
    case SH_OP_S_BFM_B64:
        src0_wide = src1_wide = false; break;
    case SH_OP_S_BITREPLICATE_B64_B32:
    case SH_OP_S_BITSET0_B64: case SH_OP_S_BITSET1_B64:
        src0_wide = false; break;
    default: break;
    }
    bool u = false;
    if (in->enc == RDNA2_ENC_SOP1) {
        if (op != SH_OP_S_GETPC_B64) u = operand_unknown(st, in->src[0], src0_wide);
    } else if (in->enc == RDNA2_ENC_SOP2 || in->enc == RDNA2_ENC_SOPC) {
        u = operand_unknown(st, in->src[0], src0_wide) || operand_unknown(st, in->src[1], src1_wide);
        if ((op == SH_OP_S_CSELECT_B32 || op == SH_OP_S_CSELECT_B64) && !st->scc_unk)
            u = operand_unknown(st, in->src[st->scc ? 0 : 1], wide);
    } else if (in->enc == RDNA2_ENC_SOPK) {
        /* Immediate moves/getreg have no SGPR input. Arithmetic and compares
         * read the old destination, rather than the unused src[] fields. */
        if (op != SH_OP_S_MOVK_I32 && op != SH_OP_S_CMOVK_I32 && op != SH_OP_S_GETREG_B32 &&
            op != SH_OP_S_SETREG_IMM32_B32)
            u = operand_unknown(st, in->dst, false);
    }
    if (op == SH_OP_S_BITSET0_B32 || op == SH_OP_S_BITSET1_B32 ||
        op == SH_OP_S_BITSET0_B64 || op == SH_OP_S_BITSET1_B64)
        u |= operand_unknown(st, in->dst, wide);
    /* the deferred marks follow the same operands */
    bool d = false, scalar = in->enc == RDNA2_ENC_SOP1 || in->enc == RDNA2_ENC_SOP2 ||
                             in->enc == RDNA2_ENC_SOPC || in->enc == RDNA2_ENC_SOPK;
    if (in->enc == RDNA2_ENC_SOP1) {
        if (op != SH_OP_S_GETPC_B64) d = operand_dfr(st, in->src[0], src0_wide);
    } else if (in->enc == RDNA2_ENC_SOP2 || in->enc == RDNA2_ENC_SOPC) {
        d = operand_dfr(st, in->src[0], src0_wide) || operand_dfr(st, in->src[1], src1_wide);
        if ((op == SH_OP_S_CSELECT_B32 || op == SH_OP_S_CSELECT_B64) && !st->scc_unk)
            d = operand_dfr(st, in->src[st->scc ? 0 : 1], wide);
    } else if (in->enc == RDNA2_ENC_SOPK) {
        if (op != SH_OP_S_MOVK_I32 && op != SH_OP_S_CMOVK_I32 && op != SH_OP_S_GETREG_B32 &&
            op != SH_OP_S_SETREG_IMM32_B32)
            d = operand_dfr(st, in->dst, false);
    } else {
        d = vector_dfr(p, i, st); /* a vector instruction with a scalar result */
    }
    if (op == SH_OP_S_BITSET0_B32 || op == SH_OP_S_BITSET1_B32 ||
        op == SH_OP_S_BITSET0_B64 || op == SH_OP_S_BITSET1_B64)
        d |= operand_dfr(st, in->dst, wide);
    bool cmov = op == SH_OP_S_CMOV_B32 || op == SH_OP_S_CMOV_B64 || op == SH_OP_S_CMOVK_I32;
    bool writes_dst = !cmov || st->scc || st->scc_unk;
    if (cmov && st->scc_unk) u |= operand_unknown(st, in->dst, wide), d |= operand_dfr(st, in->dst, wide) || st->scc_dfr;
    if (f & OPF_READS_SCC) u |= st->scc_unk, d |= st->scc_dfr;
    bool saveexec = (f & OPF_SAVEEXEC) != 0;
    bool writes_exec = saveexec || (f & OPF_WREXEC) != 0;
    bool exec_unknown = operand_unknown(st, RDNA2_OP_EXEC_LO, src0_wide);
    bool exec_dfr = operand_dfr(st, RDNA2_OP_EXEC_LO, src0_wide);
    if (writes_exec) u |= exec_unknown, d |= exec_dfr;
    if (!eval_salu(p, i, st)) return false;
    if (writes_dst && in->enc != RDNA2_ENC_SOPC && in->dst < SH_SGPRS && !(f & OPF_CMPK) &&
        op != SH_OP_S_SETREG_B32 && op != SH_OP_S_SETREG_IMM32_B32) {
        st->unk[in->dst] = saveexec ? exec_unknown : u;
        st->dfr[in->dst] = saveexec ? exec_dfr : d;
        if (wide && in->dst + 1 < SH_SGPRS)
            st->unk[in->dst + 1] = saveexec ? exec_unknown : u, st->dfr[in->dst + 1] = saveexec ? exec_dfr : d;
    }
    if (writes_exec) {
        st->unk[RDNA2_OP_EXEC_LO] = u, st->dfr[RDNA2_OP_EXEC_LO] = d;
        if (wide) st->unk[RDNA2_OP_EXEC_LO + 1] = u, st->dfr[RDNA2_OP_EXEC_LO + 1] = d;
    }
    if (!(f & OPF_KEEPS_SCC)) st->scc_unk = u, st->scc_dfr = d;
    if (!scalar) dfr_vector(p, i, st); /* its vector result */
    return true;
}

/* Paths the evaluation also takes: at a forward branch whose condition
 * depends on vector values (per-draw data the scalar code cannot know), the
 * fall-through path runs first and the taken one after it, so resources used
 * only on one side (text or image modes of an uber shader) are found too. */
/* Large materials have more than 32 forward arms. Reaching the storage bound
 * must fail evaluation, never silently omit the remaining resource paths. */
#define MAX_FORKS 512

typedef struct eval_fork {
    uint32_t at;
    eval_state st;
} eval_fork;

/* SGPR sources of the vector instructions in [from, to): the scalar state does
 * not change across them (sh_store_range reads these values). */
static void record_vsrc(const sh_program *p, sh_eval *e, const eval_state *st, uint32_t from, uint32_t to) {
    for (uint32_t j = from; j < to && j < p->count; j++) {
        const rdna2_inst *in = &p->insts[j];
        if (in->enc != RDNA2_ENC_VOP1 && in->enc != RDNA2_ENC_VOP2 && in->enc != RDNA2_ENC_VOP3) continue;
        for (uint32_t k = 0; k < 3; k++) {
            uint32_t s = in->src[k];
            if (s >= SH_SGPRS) continue;
            uint32_t v = st->s[s];
            uint8_t *state = &e->vsrc_state[j][k];
            if (st->unk[s] || ((st->group_bits[s / 32] >> (s % 32)) & 1)) *state = 2;
            else if (*state == 0) *state = 1, e->vsrc[j][k] = v;
            else if (*state == 1 && e->vsrc[j][k] != v) *state = 2;
        }
    }
}

/* The scalar state a path brings to a branch target, without the fork flag. */
static bool uniform_get(const eval_state *st, uint32_t vgpr, uint32_t *v) {
    for (uint32_t k = 0; k < st->uniform_count; k++)
        if (st->uniform[k].vgpr == vgpr) return *v = st->uniform[k].v, true;
    return false;
}

static void uniform_forget(eval_state *st, uint32_t first, uint32_t count) {
    for (uint32_t k = 0; k < st->uniform_count;) {
        if (st->uniform[k].vgpr - first < count) st->uniform[k] = st->uniform[--st->uniform_count];
        else k++;
    }
}

/* A VALU source operand that is the same in every lane: known SGPRs,
 * constants and uniform VGPRs. */
static bool uniform_operand(const eval_state *st, const rdna2_inst *in, uint32_t code, uint32_t *v) {
    if (code >= RDNA2_OP_VGPR) return uniform_get(st, code - RDNA2_OP_VGPR, v);
    if (code < SH_SGPRS)
        return !st->unk[code] && !((st->group_bits[code / 32] >> (code % 32)) & 1) && (*v = st->s[code], true);
    if (code == RDNA2_OP_SDWA || code == RDNA2_OP_DPP16 || code == RDNA2_OP_DPP8 || code == RDNA2_OP_DPP8FI ||
        code == RDNA2_OP_LDS_DIRECT || (code == RDNA2_OP_SCC && st->scc_unk))
        return false;
    *v = rd32(st, in, code);
    return true;
}

/* Only a proved full uniform DWORD overwrite frees captured slots. Other
 * possible writes mark them unknown while preserving pending dependencies;
 * stable compaction keeps survivor order. */
static void lanes_written_range(eval_state *st,uint32_t first,uint32_t count,uint64_t active,bool unknown,bool deferred,bool full_value) {
    uint32_t kept=0;
    for(uint32_t k=0;k<st->lane_count;k++) {
        bool hit=(uint32_t)(st->lanes[k].vgpr-first)<count &&
            (unknown || ((active>>st->lanes[k].lane)&1));
        if(hit && !unknown && full_value)continue;
        if(hit){st->lanes[k].unk=true;st->lanes[k].dfr|=deferred;if(st->lanes[k].dfr)vdfr_set(st,st->lanes[k].vgpr,true);}
        st->lanes[kept++]=st->lanes[k];
    }
    st->lane_count=kept;
}
static void lanes_written(const sh_program *p,uint32_t j,eval_state *st,bool full_value) {
    if(!st->lane_count)return;
    const rdna2_inst *in=&p->insts[j];uint32_t op=p->ops[j],first=256,count=0;
    if(op==SH_OP_V_WRITELANE_B32 || op==SH_OP_V_READLANE_B32 || op==SH_OP_V_READFIRSTLANE_B32 ||
       in->enc==RDNA2_ENC_EXP || in->enc==RDNA2_ENC_VOPC ||
       (in->enc==RDNA2_ENC_VOP3 && in->valu<0x100))return;
    bool unknown=st->unk[RDNA2_OP_EXEC_LO] || (!st->wave32 && st->unk[RDNA2_OP_EXEC_HI]);
    uint64_t active=st->s[RDNA2_OP_EXEC_LO]|(st->wave32?0:(uint64_t)st->s[RDNA2_OP_EXEC_HI]<<32);
    if(!unknown && !active)return;
    if(op==SH_OP_BUFFER_STORE_DWORD || op==SH_OP_TBUFFER_STORE_FORMAT || op==SH_OP_IMAGE_STORE ||
       op==SH_OP_GLOBAL_STORE || op==SH_OP_SCRATCH_STORE || op==SH_OP_V_NOP)return;
    if(op==SH_OP_V_MOV_B32 && in->enc==RDNA2_ENC_VOP1 && in->src[0]==in->dst &&
       !in->abs && !in->neg && !in->clamp && !in->omod && !in->opsel && !in->opsel_hi && !in->neg_hi)return;
    bool deferred=vector_dfr(p,j,st);
    if(op==SH_OP_V_SWAP_B32 && in->dst>=RDNA2_OP_VGPR)deferred|=vdfr_get(st,in->dst-RDNA2_OP_VGPR);
    if(op==SH_OP_V_SWAP_B32) {
        if(in->dst>=RDNA2_OP_VGPR && in->dst<RDNA2_OP_VGPR+256)
            lanes_written_range(st,in->dst-RDNA2_OP_VGPR,1,active,unknown,deferred,false);
        if(in->src[0]>=RDNA2_OP_VGPR && in->src[0]<RDNA2_OP_VGPR+256)
            lanes_written_range(st,in->src[0]-RDNA2_OP_VGPR,1,active,unknown,deferred,false);
        return;
    }
    if(op==SH_OP_V_MOVRELD_B32 || op==SH_OP_V_MOVRELSD_B32) {
        if(st->unk[124] || in->dst<RDNA2_OP_VGPR || st->s[124]>=256 || (uint32_t)(in->dst-RDNA2_OP_VGPR)>=256-st->s[124]) {
            first=0;count=256;
        } else {first=in->dst-RDNA2_OP_VGPR+st->s[124];count=1;}
    } else if(in->enc==RDNA2_ENC_MUBUF || in->enc==RDNA2_ENC_MTBUF || in->enc==RDNA2_ENC_MIMG || in->enc==RDNA2_ENC_FLAT) {
        bool atomic=op==SH_OP_BUFFER_ATOMIC || op==SH_OP_IMAGE_ATOMIC || op==SH_OP_GLOBAL_ATOMIC;
        int site=p->site_of[j];
        if((atomic && !in->glc) || (!atomic && site>=0 && (uint32_t)site<p->site_count && p->sites[site].store))return;
        first=in->enc==RDNA2_ENC_FLAT?(in->dst>=RDNA2_OP_VGPR?in->dst-RDNA2_OP_VGPR:256):in->vdata;
        count=p->vmem_width[j]?p->vmem_width[j]:16; /* width already includes TFE */
    } else if(in->enc==RDNA2_ENC_DS) {
        const char *name=in->name?in->name:"";
        bool returns=strstr(name,"read") || strstr(name,"rtn") || strstr(name,"swizzle") || strstr(name,"permute") ||
            strstr(name,"append") || strstr(name,"consume") || strstr(name,"ordered_count");
        if(!returns && (strstr(name,"write") || strstr(name,"nop") || strstr(name,"gws") ||
           strstr(name,"add") || strstr(name,"sub") || strstr(name,"min") || strstr(name,"max") ||
           strstr(name,"and") || strstr(name,"xor") || strstr(name,"or_") || strstr(name,"inc") ||
           strstr(name,"dec") || strstr(name,"cmpst")))return;
        if(in->dst>=RDNA2_OP_VGPR && in->dst<RDNA2_OP_VGPR+256) {
            first=in->dst-RDNA2_OP_VGPR;count=16;
            if(strstr(name,"b128"))count=4;
            else if(strstr(name,"b96"))count=3;
            else if(strstr(name,"b64"))count=strstr(name,"read2")?4:2;
            else if(returns)count=strstr(name,"read2")?2:1;
        }
    } else if(in->dst>=RDNA2_OP_VGPR && in->dst<RDNA2_OP_VGPR+256) {
        first=in->dst-RDNA2_OP_VGPR;count=(p->iflags[j]&SH_IF_64)?2:1;
    }
    if(first<256 && count)lanes_written_range(st,first,PS5C_MIN(count,256-first),active,unknown,deferred,full_value);
}

/* The VGPRs instruction j writes, for the uniform table: a vector ALU result
 * and, for memory, everything from vdata on (overestimated). */
static void uniform_written(const sh_program *p, uint32_t j, eval_state *st) {
    const rdna2_inst *in = &p->insts[j];
    uint32_t op = p->ops[j];
    if(op==SH_OP_V_NOP)return;
    if (op == SH_OP_V_READFIRSTLANE_B32 || op == SH_OP_V_READLANE_B32) return; /* an SGPR, in the VDST field */
    if(op==SH_OP_V_SWAP_B32) {
        if(in->dst>=RDNA2_OP_VGPR)uniform_forget(st,in->dst-RDNA2_OP_VGPR,1);
        if(in->src[0]>=RDNA2_OP_VGPR)uniform_forget(st,in->src[0]-RDNA2_OP_VGPR,1);
        return;
    }
    if (op == SH_OP_V_MOVRELD_B32 || op == SH_OP_V_MOVRELSD_B32) { /* v[dst + m0] */
        if (st->unk[124]) st->uniform_count = 0;
        else if (in->dst >= RDNA2_OP_VGPR) uniform_forget(st, in->dst - RDNA2_OP_VGPR + st->s[124], 1);
        return;
    }
    switch (in->enc) {
    case RDNA2_ENC_MUBUF:
    case RDNA2_ENC_MTBUF:
    case RDNA2_ENC_MIMG:
    case RDNA2_ENC_FLAT:
        uniform_forget(st, in->vdata, p->vmem_width[j] ? p->vmem_width[j] + 1u : 16u);
        if (in->dst >= RDNA2_OP_VGPR) uniform_forget(st, in->dst - RDNA2_OP_VGPR, 16);
        return;
    case RDNA2_ENC_DS:
        if (in->dst >= RDNA2_OP_VGPR) uniform_forget(st, in->dst - RDNA2_OP_VGPR, 16);
        return;
    case RDNA2_ENC_EXP:
    case RDNA2_ENC_VOPC:
        return;
    default:
        if (in->dst >= RDNA2_OP_VGPR && in->dst < RDNA2_OP_VGPR + 256)
            uniform_forget(st, in->dst - RDNA2_OP_VGPR, (p->iflags[j] & SH_IF_64) ? 2 : 1);
        return;
    }
}

/* Vector instruction j, which the scalar evaluation otherwise skips: a copy or
 * simple integer operation of uniform operands, done by every lane, keeps its
 * result (SDWA only on whole dwords, without modifiers). */
static void eval_uniform(const sh_program *p, uint32_t j, eval_state *st) {
    const rdna2_inst *in = &p->insts[j];
    uint32_t op = p->ops[j], a = 0, b = 0, v;
    bool full = !st->unk[RDNA2_OP_EXEC_LO] && st->s[RDNA2_OP_EXEC_LO] == 0xFFFFFFFFu &&
                (p->wave32 || (!st->unk[RDNA2_OP_EXEC_HI] && st->s[RDNA2_OP_EXEC_HI] == 0xFFFFFFFFu));
    bool known = full && (in->enc == RDNA2_ENC_VOP1 || in->enc == RDNA2_ENC_VOP2) && in->dst >= RDNA2_OP_VGPR &&
                 in->dst < RDNA2_OP_VGPR + 256;
    uint32_t src0 = in->src[0], src1 = in->src[1];
    if (known && src0 == RDNA2_OP_SDWA) {
        uint32_t d = in->dpp_sdwa;
        /* dst/src0 selects whole dwords; no clamp, omod, sext, neg or abs */
        bool dword = ((d >> 8) & 7) == 6 && ((d >> 16) & 7) == 6 && !((d >> 13) & 7) && !((d >> 19) & 7);
        if (in->enc == RDNA2_ENC_VOP2) dword = dword && ((d >> 24) & 7) == 6 && !((d >> 27) & 7);
        known = dword;
        src0 = (d & 0xFF) + ((d >> 23) & 1 ? 0 : RDNA2_OP_VGPR);
        if (in->enc == RDNA2_ENC_VOP2 && src1 >= RDNA2_OP_VGPR && (d >> 31)) src1 -= RDNA2_OP_VGPR;
    }
    known = known && uniform_operand(st, in, src0, &a) && (in->enc == RDNA2_ENC_VOP1 || uniform_operand(st, in, src1, &b));
    switch (op) {
    case SH_OP_V_MOV_B32: v = a; break;
    case SH_OP_V_OR_B32: v = a | b; break;
    case SH_OP_V_AND_B32: v = a & b; break;
    case SH_OP_V_XOR_B32: v = a ^ b; break;
    case SH_OP_V_ADD_NC_U32: v = a + b; break;
    case SH_OP_V_SUB_NC_U32: v = a - b; break;
    case SH_OP_V_SUBREV_NC_U32: v = b - a; break;
    case SH_OP_V_LSHLREV_B32: v = b << (a & 31); break;
    case SH_OP_V_LSHRREV_B32: v = b >> (a & 31); break;
    default: known = false; v = 0; break;
    }
    known=known && !vector_dfr(p,j,st);
    lanes_written(p,j,st,known);
    uniform_written(p, j, st);
    if (!known || st->uniform_count == PS5C_ARRAY_COUNT(st->uniform)) return;
    st->uniform[st->uniform_count].vgpr = (uint16_t)(in->dst - RDNA2_OP_VGPR);
    st->uniform[st->uniform_count++].v = v;
}

static uint64_t eval_state_hash(const eval_state *st) {
    uint64_t h = 0xCBF29CE484222325ull;
    h=(h^st->call_depth)*0x100000001b3ull;
    for (uint32_t k=0;k<st->call_depth;k++) {
        h=(h^st->call_entries[k])*0x100000001b3ull;
        h=(h^st->call_returns[k])*0x100000001b3ull;
        h=(h^st->call_resume[k])*0x100000001b3ull;
    }
    /* The SGPRs and their marks as 64-bit words in four independent streams: one
     * multiply chain over 128 SGPRs was most of an evaluation at its joins
     * (PSPHD: ~100 joins a draw). */
    uint64_t a[4] = {h, h ^ 0x9E3779B97F4A7C15ull, h ^ 0xC2B2AE3D27D4EB4Full, h ^ 0x165667B19E3779F9ull};
    const struct { const void *p; size_t words; } part[3] = {
        {st->s, sizeof(st->s) / 8}, {st->unk, sizeof(st->unk) / 8}, {st->dfr, sizeof(st->dfr) / 8}};
    for (uint32_t k = 0; k < 3; k++) {
        const unsigned char *b = (const unsigned char *)part[k].p;
        for (size_t i = 0; i < part[k].words; i += 4) {
            uint64_t w[4];
            memcpy(w, b + i * 8, sizeof(w));
            a[0] = (a[0] ^ w[0]) * 0x100000001B3ull;
            a[1] = (a[1] ^ w[1]) * 0xFF51AFD7ED558CCDull;
            a[2] = (a[2] ^ w[2]) * 0xC4CEB9FE1A85EC53ull;
            a[3] = (a[3] ^ w[3]) * 0x9E3779B97F4A7C15ull;
        }
    }
    for (uint32_t k = 0; k < 4; k++) {
        uint64_t x = a[k];
        x ^= x >> 33; x *= 0xFF51AFD7ED558CCDull; x ^= x >> 33;
        h = (h ^ x) * 0x100000001B3ull;
    }
    h = (h ^ (st->scc * 4u + st->scc_unk + st->scc_dfr * 2u)) * 0x100000001B3ull;
    for (uint32_t k = 0; k < 8; k++) h = (h ^ st->vdfr[k]) * 0x100000001B3ull;
    h = (h ^ st->lane_count) * 0x100000001B3ull;
    for (uint32_t k = 0; k < st->lane_count && k < 64; k++)
        h = (h ^ ((uint64_t)st->lanes[k].v << 17 ^ st->lanes[k].vgpr << 9 ^ st->lanes[k].lane << 2 ^ st->lanes[k].dfr << 1 ^ st->lanes[k].unk)) *
            0x100000001B3ull;
    for (uint32_t k = 0; k < st->uniform_count; k++)
        h = (h ^ ((uint64_t)st->uniform[k].v << 16 ^ st->uniform[k].vgpr)) * 0x100000001B3ull;
    return h | 1;
}

bool sh_evaluate(const sh_program *p, gmem *mem, sh_eval *e) {
    if(!sh_aperture_allowed(p)) {
        e->call_pending=e->call_known=e->deferred=e->guessed=false;
        return false;
    }
    /* Keep the common small case in TLS. A 512-state static TLS array would
     * be instantiated by every guest host thread, including non-render threads.
     * Larger, analyzed working sets get bounded call-local storage instead. */
    static _Thread_local eval_fork small_forks[32];
    eval_fork *forks=small_forks;bool result=false;
    static _Thread_local bool forked[SH_MAX_INSTS]; /* branches already forked from */
    /* Branch targets and the states paths reached them with: a forked path that
     * arrives with a state an earlier path already brought there would repeat
     * that path from here on (same sites, same values), so it ends. Large
     * material shaders fork at most once per branch and each fork ran to
     * the end (No Man's Sky: ~1.3 ms per draw for 7700 instructions).
     * PS5C_EVAL_NO_JOIN=1: every fork runs to the end, as before. */
    static _Thread_local bool join[SH_MAX_INSTS];
    static _Thread_local uint64_t join_seen[SH_MAX_INSTS][2];
    static _Thread_local uint32_t join_epoch[SH_MAX_INSTS];
    static _Thread_local uint32_t cur_join_epoch;
    static int no_join = -1;
    if (no_join < 0) no_join = getenv("PS5C_EVAL_NO_JOIN") != NULL;
    uint32_t fork_count = 0, fork_next = 0;
    bool analyzed = p->count && (p->iflags[0] & SH_IF_ANALYZED) != 0;
    uint32_t forward=analyzed?p->eval_forward_branches:0;
    if(!analyzed)for(uint32_t j=0;j<p->count;j++)
        if(p->ops[j]!=SH_OP_S_BRANCH && is_branch(p->ops[j]) && p->insts[j].simm16>0)forward++;
    uint32_t fork_capacity=forward>MAX_FORKS?MAX_FORKS:forward;
    if(fork_capacity<PS5C_ARRAY_COUNT(small_forks))fork_capacity=PS5C_ARRAY_COUNT(small_forks);
    if(fork_capacity>PS5C_ARRAY_COUNT(small_forks)) {
        forks=malloc((size_t)fork_capacity*sizeof(*forks));
        if(!forks)return false;
    }
    bool has_flow = !analyzed || p->has_flow;
    if (has_flow) {
        memset(forked, 0, sizeof(bool) * p->count);
        if (!no_join) {
            if (++cur_join_epoch == 0) {
                memset(join_epoch, 0, sizeof(join_epoch));
                cur_join_epoch = 1;
            }
            if (!analyzed) {
                memset(join, 0, sizeof(bool) * p->count);
                for (uint32_t j = 0; j < p->count; j++)
                    if (is_branch(p->ops[j]) && p->target[j] < p->count && p->next_scalar[p->target[j]] < p->count)
                        join[p->next_scalar[p->target[j]]] = true; /* where a path there resumes */
            }
        }
    }

    eval_state st;
    st.call_depth=0;
    st.wave32=p->wave32;
    memcpy(st.s, e->sgpr_init, sizeof(st.s));
    st.scc = false;
    memset(st.unk, 0, sizeof(st.unk));
    st.scc_unk = false;
    memset(st.dfr, 0, sizeof(st.dfr));
    st.scc_dfr = false;
    memset(st.vdfr, 0, sizeof(st.vdfr));
    st.fork = false;
    st.lane_count = 0;
    st.uniform_count = 0;
    memset(st.group_bits, 0, sizeof(st.group_bits));
    for (uint32_t k = e->group_sgpr; k < (uint32_t)e->group_sgpr + e->group_sgprs && k < SH_SGPRS; k++)
        st.group_bits[k / 32] |= 1u << (k % 32);
    bool calls = analyzed ? (p->iflags[0] & SH_IF_HAS_SWAPPC) != 0 : false;
    if (!analyzed) for (uint32_t j=0;j<p->count;j++) calls|=p->ops[j]==SH_OP_S_SWAPPC_B64;
    /* Do not specialize an indirect function table from group zero. */
    if (calls) for (uint32_t k=e->group_sgpr;k<(uint32_t)e->group_sgpr+e->group_sgprs && k<SH_SGPRS;k++)
        st.unk[k]=true;
    /* Only sites declared by this program are observable. Small shaders must
     * not clear the capacity reserved for large material programs per draw. */
    memset(e->site, 0, sizeof(e->site[0]) * p->site_count);
    e->deferred = e->guessed = false;
    g_dfr_branch = g_fork_conflict = false;
    e->call_pending = e->call_known = false;
    e->call_target = 0;
    e->smem_words = 0;
    if (!grow(e, SH_U_DATA)) goto eval_done;
    memcpy(e->smem_data, e->sgpr_init, sizeof(e->sgpr_init));
    uint32_t clear_sites = p->site_count > e->site_dirty ? p->site_count : e->site_dirty;
    if (clear_sites > SH_MAX_SITES) clear_sites = SH_MAX_SITES;
    if (clear_sites) memset(e->smem_data + SH_U_SITE, 0, (size_t)clear_sites * 4);
    memset(e->smem_data + SH_U_VPORT, 0, (SH_U_DATA - SH_U_VPORT) * 4);
    e->smem_data[SH_U_PC] = (uint32_t)p->base;
    e->smem_data[SH_U_PC + 1] = (uint32_t)(p->base >> 32);
    e->site_dirty = p->site_count;
    e->smem_words = SH_U_DATA;

    uint32_t i = 0, steps = 0;
    /* times each backward branch was taken: loops that end when their lanes run
     * out (v_cmpx + s_cbranch_execz) never end here, where all lanes stay on */
    static _Thread_local uint16_t loops[SH_MAX_INSTS];
    static _Thread_local bool vector_exit[SH_MAX_INSTS];
    static _Thread_local bool bounded_loop[SH_MAX_INSTS];
    if (has_flow) {
        memset(vector_exit, 0, sizeof(bool) * p->count);
        if (!analyzed || p->back_branch) {
            memset(loops, 0, sizeof(uint16_t) * p->count);
            memset(bounded_loop, 0, sizeof(bool) * p->count);
        }
    }
    if (e->vsrc) memset(e->vsrc_state, 0, sizeof(uint8_t) * 3 * p->count);
    for (;;) {
        for (;;) {
            uint32_t at = i;
            i = p->next_scalar[i];
            if (e->vsrc) record_vsrc(p, e, &st, at, i);
            for (uint32_t j = at; p->lane_reads && j < i && j < p->count; j++) eval_uniform(p, j, &st);
            for (uint32_t j = at; e->deferred && j < i && j < p->count; j++) dfr_vector(p, j, &st);
            if (!(i < p->count && steps++ < 1000000)) break;
            if (has_flow && !no_join && (analyzed ? (p->iflags[i] & SH_IF_JOIN) != 0 : join[i])) {
                if (join_epoch[i] != cur_join_epoch) {
                    join_epoch[i] = cur_join_epoch;
                    join_seen[i][0] = join_seen[i][1] = 0;
                }
                uint64_t h = eval_state_hash(&st);
                if (st.fork && (join_seen[i][0] == h || join_seen[i][1] == h)) break;
                if (join_seen[i][0] != h) join_seen[i][1] = join_seen[i][0], join_seen[i][0] = h;
            }
            const rdna2_inst *in = &p->insts[i];
            uint32_t op = p->ops[i];
            uint32_t next = i + 1;
            if (op == SH_OP_S_ENDPGM || op == SH_OP_S_CODE_END) {
                if(st.call_depth)goto eval_done;
                break;
            }
            if (p->lane_reads) {lanes_written(p,i,&st,false);uniform_written(p, i, &st);}
            if (op == SH_OP_S_SWAPPC_B64) {
                e->call_pending = true;
                e->call_inst = i;
                uint32_t src = in->src[0];
                e->call_known = src <= 104 && !(src & 1u) &&
                    (in->dst==RDNA2_OP_NULL || (in->dst <= 104 && !(in->dst & 1u))) &&
                    !st.unk[src] && !st.unk[src + 1] && !st.dfr[src] && !st.dfr[src + 1] &&
                    !(st.group_bits[src / 32] & (3u << (src % 32)));
                if (e->call_known) e->call_target = rd64(&st, in, src);
                if (!e->call_known) goto eval_done;
                uint32_t dest;
                /* Capture the target before the possibly overlapping link. */
                if (in->dst!=RDNA2_OP_NULL) {
                    dest=sh_call_destination(p,e->call_target,i);
                    if(dest>=p->count)goto eval_done;
                    e->call_pending=false;
                    if (st.call_depth==SH_MAX_FUNCTIONS || e->call_target==p->base) goto eval_done;
                    for (uint32_t k=0;k<st.call_depth;k++) if (st.call_entries[k]==e->call_target) goto eval_done;
                    st.call_entries[st.call_depth]=e->call_target;
                    st.call_resume[st.call_depth]=i+1;
                    st.call_returns[st.call_depth++]=sh_inst_address(p,i)+(uint64_t)in->words*4;
                    wr64(&st,in->dst,sh_inst_address(p,i)+(uint64_t)in->words*4);
                    st.unk[in->dst]=st.unk[in->dst+1]=false;
                    st.dfr[in->dst]=st.dfr[in->dst+1]=false;
                } else {
                    if (!st.call_depth || st.call_returns[st.call_depth-1]!=e->call_target) goto eval_done;
                    dest=st.call_resume[--st.call_depth];
                    e->call_pending=false;
                }
                next=dest;
            } else if (is_branch(op)) {
                bool unknown = false, taken;
                switch (op) {
                case SH_OP_S_BRANCH: taken = true; break;
                case SH_OP_S_CBRANCH_SCC0: taken = !st.scc, unknown = st.scc_unk; break;
                case SH_OP_S_CBRANCH_SCC1: taken = st.scc, unknown = st.scc_unk; break;
                case SH_OP_S_CBRANCH_VCCZ:
                case SH_OP_S_CBRANCH_VCCNZ: { /* wave32: VCC_HI may hold a scalar temporary */
                    bool zero = p->wave32 ? !st.s[RDNA2_OP_VCC_LO] : !(st.s[RDNA2_OP_VCC_LO] | st.s[RDNA2_OP_VCC_LO + 1]);
                    unknown = st.unk[RDNA2_OP_VCC_LO] || (!p->wave32 && st.unk[RDNA2_OP_VCC_LO + 1]);
                    taken = op == SH_OP_S_CBRANCH_VCCZ ? zero : !zero;
                    break;
                }
                default: { /* EXECZ / EXECNZ */
                    bool zero = p->wave32 ? !st.s[RDNA2_OP_EXEC_LO] : !(st.s[RDNA2_OP_EXEC_LO] | st.s[RDNA2_OP_EXEC_LO + 1]);
                    unknown = st.unk[RDNA2_OP_EXEC_LO] || (!p->wave32 && st.unk[RDNA2_OP_EXEC_LO + 1]);
                    taken = op == SH_OP_S_CBRANCH_EXECZ ? zero : !zero;
                    break;
                }
                }
                if (unknown) {
                    /* a deferred value decides it: the dispatch may take the other path. Forward:
                     * both sides run here (g_dfr_branch); backward (a loop): a guess */
                    bool dfr_cond;
                    if (op == SH_OP_S_CBRANCH_SCC0 || op == SH_OP_S_CBRANCH_SCC1) dfr_cond = st.scc_dfr;
                    else if (op == SH_OP_S_CBRANCH_VCCZ || op == SH_OP_S_CBRANCH_VCCNZ)
                        dfr_cond = st.dfr[RDNA2_OP_VCC_LO] || (!p->wave32 && st.dfr[RDNA2_OP_VCC_LO + 1]);
                    else dfr_cond = st.dfr[RDNA2_OP_EXEC_LO] || (!p->wave32 && st.dfr[RDNA2_OP_EXEC_LO + 1]);
                    static int guess_all = -1;
                    if (guess_all < 0) { const char *g = getenv("PS5C_EVAL_GUESS_BRANCHES"); guess_all = g && !strcmp(g, "1"); }
                    if (in->simm16 > 0 && !guess_all) g_dfr_branch |= dfr_cond;
                    else e->guessed |= dfr_cond;
                    /* A loop can exit on an unknown VCC/SCC while EXEC remains
                     * known. Repeating its representative CPU path 1024 times
                     * cannot resolve that condition. Mark only backedges that
                     * cross this test AND precede its exit target; an unknown
                     * if wholly inside a known scalar loop is not a loop exit.
                     * This bounds resource discovery, not GPU execution. */
                    if (in->simm16 > 0 && !vector_exit[i]) {
                        vector_exit[i] = true;
                        for (uint32_t j = i + 1; j < p->target[i] && j < p->count; j++)
                            if (is_branch(p->ops[j]) && p->insts[j].simm16 < 0 && p->target[j] <= i)
                                bounded_loop[j] = true;
                    }
                    /* conditions on vector results (alpha kill, divergent control flow):
                     * the fall-through path, where some lane stays active; the other
                     * side of a forward branch later */
                    if (in->simm16 > 0 && !forked[i]) {
                        if (fork_count == fork_capacity) goto eval_done;
                        forked[i] = true;
                        forks[fork_count].at = p->target[i];
                        forks[fork_count].st = st;
                        forks[fork_count].st.fork = true;
                        fork_count++;
                    }
                    taken = false;
                } else if (op != SH_OP_S_BRANCH && in->simm16 > 0 && !forked[i]) {
                    if (fork_count == fork_capacity) goto eval_done;
                    /* a known condition can still differ when the shader runs (constants the
                     * CPU writes after the draw is recorded): the other side fills the
                     * descriptors of its sites too, so they are not bound as black stand-ins */
                    forked[i] = true;
                    forks[fork_count].at = taken ? i + 1 : p->target[i];
                    forks[fork_count].st = st;
                    forks[fork_count].st.fork = true;
                    fork_count++;
                }
                if (taken && in->simm16 < 0) {
                    /* Lane-dependent loops: two backedges find their resources. */
                    uint16_t limit = st.unk[126] || (!p->wave32 && st.unk[127]) || bounded_loop[i] ? 2 : 1024;
                    if (loops[i] >= limit) taken = false;
                    else loops[i]++;
                }
                if (taken) next = p->target[i];
            } else if (op == SH_OP_S_MEMTIME || op == SH_OP_S_MEMREALTIME) {
                wr64(&st, in->dst, 0);
                if (in->dst + 1u < SH_SGPRS) st.unk[in->dst] = st.unk[in->dst + 1] = true, st.dfr[in->dst] = st.dfr[in->dst + 1] = false;
            } else if (is_smem(op)) {
                eval_smem(p, i, mem, &st, e);
            } else if (in->enc == RDNA2_ENC_MUBUF || in->enc == RDNA2_ENC_MTBUF || in->enc == RDNA2_ENC_MIMG) {
                eval_vmem_site(p, i, &st, e);
            } else if (in->enc == RDNA2_ENC_FLAT && in->seg != 1) {
                eval_global_site(p, i, &st, mem, e);
            } else if (in->enc == RDNA2_ENC_DS) {
                eval_gds_site(p, i, &st, e);
            } else if (in->enc == RDNA2_ENC_VOPC || (in->enc == RDNA2_ENC_VOP3 && in->valu < 0x100)) {
                /* vector compare: result mask unknown, assume all lanes true. An SDWA
                 * compare with SD set writes its SDST instead of VCC. */
                uint32_t d = op == SH_OP_V_CMPX ? RDNA2_OP_EXEC_LO : in->dst;
                if (in->enc == RDNA2_ENC_VOPC && in->src[0] == RDNA2_OP_SDWA && op != SH_OP_V_CMPX &&
                    ((in->dpp_sdwa >> 15) & 1))
                    d = (in->dpp_sdwa >> 8) & 0x7F;
                bool dv = vector_dfr(p, i, &st);
                if (p->wave32) { /* the low half only: VCC_HI may hold a scalar temporary */
                    if (op != SH_OP_V_CMPX) wr32(&st, d, st.s[RDNA2_OP_EXEC_LO]);
                    if (d < SH_SGPRS) st.unk[d] = true, st.dfr[d] = dv;
                } else {
                    if (op == SH_OP_V_CMPX) wr64(&st, RDNA2_OP_EXEC_LO, rd64(&st, in, RDNA2_OP_EXEC_LO));
                    else wr64(&st, d, rd64(&st, in, RDNA2_OP_EXEC_LO));
                    if (d < SH_SGPRS - 1) st.unk[d] = st.unk[d + 1] = true, st.dfr[d] = st.dfr[d + 1] = dv;
                }
            } else if (op == SH_OP_V_WRITELANE_B32) {
                uint32_t n = (in->dst - RDNA2_OP_VGPR) & 0xFF, lane = rd32(&st, in, in->src[1]) & (st.wave32 ? 31 : 63), k;
                bool index_unknown=operand_unknown(&st,in->src[1],false);
                bool unk = operand_unknown(&st,in->src[0],false) || index_unknown;
                /* The per-VGPR summary also reaches copies with no captured
                 * lane at their destination, including capture overflow. */
                bool lane_dfr=operand_dfr(&st,in->src[0],false) || operand_dfr(&st,in->src[1],false);
                if(lane_dfr)vdfr_set(&st,n,true);
                if(index_unknown) {
                    for(uint32_t t=0;t<st.lane_count;t++)if(st.lanes[t].vgpr==n) {
                        st.lanes[t].unk=true;st.lanes[t].dfr|=lane_dfr;
                    }
                }
                if(!index_unknown) {
                    for (k = 0; k < st.lane_count && !(st.lanes[k].vgpr == n && st.lanes[k].lane == lane); k++) {}
                    if (k == st.lane_count && k < 64) st.lane_count++;
                    if (k < 64) {
                        st.lanes[k].vgpr = (uint8_t)n, st.lanes[k].lane = (uint8_t)lane;
                        st.lanes[k].v = rd32(&st, in, in->src[0]);
                        st.lanes[k].unk = unk;
                        st.lanes[k].dfr = lane_dfr;
                    }
                }
            } else if (op == SH_OP_V_READFIRSTLANE_B32 || op == SH_OP_V_READLANE_B32) {
                uint32_t d = in->dst & 0x7F, v = 0;
                bool unk = true, dv = operand_dfr(&st, in->src[0], false) || operand_dfr(&st, in->src[1], false) ||
                                    st.dfr[RDNA2_OP_EXEC_LO] || st.dfr[RDNA2_OP_EXEC_LO + 1];
                if (op == SH_OP_V_READLANE_B32 && in->src[0] >= RDNA2_OP_VGPR &&
                    !operand_unknown(&st,in->src[1],false)) {
                    uint32_t n = (in->src[0] - RDNA2_OP_VGPR) & 0xFF, lane = rd32(&st, in, in->src[1]) & (st.wave32 ? 31 : 63);
                    for (uint32_t k = 0; k < st.lane_count; k++)
                        if (st.lanes[k].vgpr == n && st.lanes[k].lane == lane)
                            v = st.lanes[k].v, unk = st.lanes[k].unk, dv |= st.lanes[k].dfr;
                }
                if (unk && in->src[0] >= RDNA2_OP_VGPR && in->src[0] < RDNA2_OP_VGPR + 256 &&
                    uniform_get(&st, in->src[0] - RDNA2_OP_VGPR, &v))
                    unk = false;
                wr32(&st, d, v);
                if (d < SH_SGPRS) st.unk[d] = unk, st.dfr[d] = unk && dv;
            } else {
                eval_salu_tracked(p, i, &st);
            }
            { /* PS5C_LOG_UNKNOWN_DESC=<shader address>: s0..s3 marks through 0x7f0..0x850 (debugging aid) */
                static int have = -1;
                static uint64_t want;
                static uint32_t logged;
                if (have < 0) {
                    have = getenv("PS5C_LOG_UNKNOWN_DESC") != NULL;
                    if (have) want = strtoull(getenv("PS5C_LOG_UNKNOWN_DESC"), NULL, 16);
                }
                if (have && p->base == want && p->pc[i] * 4 >= 0x7f0 && p->pc[i] * 4 <= 0x850 && logged < 80) {
                    logged++;
                    char text[128];
                    if (!rdna2_disasm(in, text, sizeof(text))) snprintf(text, sizeof(text), "%s", in->name ? in->name : "?");
                    LOG_I(GPU, "eval +0x%x %-40s s0-3 %08x %08x %08x %08x unk %d%d%d%d scc_unk %d", p->pc[i] * 4, text,
                          st.s[0], st.s[1], st.s[2], st.s[3], st.unk[0], st.unk[1], st.unk[2], st.unk[3], st.scc_unk);
                }
            }
            i = next;
        }
        if (steps >= 1000000 || fork_next == fork_count) break;
        i = forks[fork_next].at;
        st = forks[fork_next].st;
        fork_next++;
    }
    if (steps >= 1000000) {
        static int warned;
        if (warned++ < 4) {
            LOG_W(GPU, "shader 0x%llx: scalar evaluation hit the step limit", (unsigned long long)p->base);
            for (uint32_t j = 0; getenv("PS5C_LOG_SHADERS") && j < p->count; j++) { /* its scalar control flow */
                if (p->insts[j].enc > RDNA2_ENC_SMEM && p->insts[j].enc != RDNA2_ENC_VOPC) continue;
                char text[128];
                rdna2_disasm(&p->insts[j], text, sizeof(text));
                LOG_W(GPU, "  +0x%x %s  scc %d%s", p->pc[j] * 4, text, st.scc, st.scc_unk ? "?" : "");
            }
        }
    }
    /* A joined fork may end inside a callee: its identical continuation has
     * already been evaluated. Only a real END with an active call is invalid. */
    result=!calls || steps<1000000;
eval_done:
    e->guessed |= g_dfr_branch && g_fork_conflict;
    if(forks!=small_forks)free(forks);
    return result;
}

/* ---- Store ranges (compute) ---- */

typedef struct vrange {
    uint64_t lo, hi; /* within 32 bits when known */
    bool known;
} vrange;

static vrange vrange_top(void) {
    vrange r = {0, 0xFFFFFFFFull, false};
    return r;
}
static vrange vrange_of(uint64_t lo, uint64_t hi) {
    if (lo > hi || hi > 0xFFFFFFFFull) return vrange_top();
    vrange r = {lo, hi, true};
    return r;
}
static vrange vrange_join(vrange a, vrange b) {
    if (!a.known || !b.known) return vrange_top();
    return vrange_of(a.lo < b.lo ? a.lo : b.lo, a.hi > b.hi ? a.hi : b.hi);
}

/* Some instruction before `j` writes SGPR `reg` (conservative). */
static bool sgpr_written_before(const sh_program *p, uint32_t j, uint32_t reg) {
    for (uint32_t i = 0; i < j; i++) {
        const rdna2_inst *in = &p->insts[i];
        if ((p->ops[i] == SH_OP_V_READFIRSTLANE_B32 || p->ops[i] == SH_OP_V_READLANE_B32) &&
            reg == (in->dst & 0x7f)) return true;
        uint32_t w = in->enc == RDNA2_ENC_SMEM ? 16 : (p->iflags[i] & SH_IF_64) ? 2 : 1;
        bool scalar = in->enc == RDNA2_ENC_SOP1 || in->enc == RDNA2_ENC_SOP2 || in->enc == RDNA2_ENC_SOPK ||
                      in->enc == RDNA2_ENC_SMEM;
        if (scalar && reg >= in->dst && reg < in->dst + w) return true;
        if ((in->enc == RDNA2_ENC_VOP3 || in->enc == RDNA2_ENC_VOPC) && (reg == in->sdst || reg == in->sdst + 1u)) return true;
        if (in->dst < SH_SGPRS && reg >= in->dst && reg < in->dst + w && in->enc != RDNA2_ENC_SOPP) return true;
        if (in->enc == RDNA2_ENC_VOPC && (reg == RDNA2_OP_VCC_LO || reg == RDNA2_OP_VCC_LO + 1)) return true;
    }
    return false;
}

static vrange vrange_operand(const sh_program *p, const sh_eval *e, const sh_stage_info *info, const uint32_t groups[3],
                             const vrange *v, uint32_t j, uint32_t k) {
    const rdna2_inst *in = &p->insts[j];
    uint32_t src = in->src[k], c;
    if (src >= RDNA2_OP_VGPR) return v[(src - RDNA2_OP_VGPR) & 0xFF];
    if (src < SH_SGPRS) {
        uint32_t tg = info->cs_tgid_sgpr;
        for (uint32_t d = 0; d < 3; d++) { /* workgroup ids: one SGPR per enabled dimension */
            if (!((info->cs_tgid_enable >> d) & 1)) continue;
            if (src == tg && !sgpr_written_before(p, j, src)) return vrange_of(0, groups[d] ? groups[d] - 1 : 0);
            tg++;
        }
        return e->vsrc_state[j][k] == 1 ? vrange_of(e->vsrc[j][k], e->vsrc[j][k]) : vrange_top();
    }
    if (src == RDNA2_OP_LITERAL) return vrange_of(in->literal, in->literal);
    if (src >= 128 && src <= 208 && sh_inline_const(src, in->literal, &c)) return vrange_of(c, c); /* integers */
    return vrange_top();
}

static uint32_t f32_to_u32(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    if (!(f > 0.0f)) return 0; /* NaN and negatives */
    if (f >= 4294967296.0f) return 0xFFFFFFFFu;
    return (uint32_t)f;
}

/* The VGPR an instruction writes, its new range in *out; false for instructions that write no VGPR. */
static bool vrange_step(const sh_program *p, const sh_eval *e, const sh_stage_info *info, const uint32_t groups[3],
                        const vrange *v, uint32_t j, uint32_t *dst, vrange *out) {
    const rdna2_inst *in = &p->insts[j];
    /* The decoder uses VDST's encoding for these scalar destinations. Reading
     * a lane into s0 must not erase the independently bounded vector v0. */
    if (p->ops[j] == SH_OP_V_READFIRSTLANE_B32 || p->ops[j] == SH_OP_V_READLANE_B32) return false;
    if (in->enc != RDNA2_ENC_VOP1 && in->enc != RDNA2_ENC_VOP2 && in->enc != RDNA2_ENC_VOP3 &&
        in->enc != RDNA2_ENC_VOP3P && in->enc != RDNA2_ENC_VINTRP)
        return false;
    if (in->dst < RDNA2_OP_VGPR) return false; /* compares and readlanes write SGPRs */
    *dst = (in->dst - RDNA2_OP_VGPR) & 0xFF;
    *out = vrange_top();
    if (in->abs || in->neg || in->clamp || in->omod || in->opsel || in->src[0] == RDNA2_OP_SDWA ||
        in->src[0] == RDNA2_OP_DPP16 || in->enc == RDNA2_ENC_VOP3P || in->enc == RDNA2_ENC_VINTRP)
        return true;
    vrange a = vrange_operand(p, e, info, groups, v, j, 0), b = vrange_operand(p, e, info, groups, v, j, 1);
    vrange c = in->enc == RDNA2_ENC_VOP3 ? vrange_operand(p, e, info, groups, v, j, 2) : vrange_top();
    switch (p->ops[j]) {
    case SH_OP_V_MOV_B32: *out = a; break;
    case SH_OP_V_ADD_NC_U32:
    case SH_OP_V_ADD_CO_U32:
        if (a.known && b.known) *out = vrange_of(a.lo + b.lo, a.hi + b.hi);
        break;
    case SH_OP_V_SUB_NC_U32:
        if (a.known && b.known && a.lo >= b.hi) *out = vrange_of(a.lo - b.hi, a.hi - b.lo);
        break;
    case SH_OP_V_SUBREV_NC_U32:
        if (a.known && b.known && b.lo >= a.hi) *out = vrange_of(b.lo - a.hi, b.hi - a.lo);
        break;
    case SH_OP_V_LSHLREV_B32: /* src0 is the shift */
        if (a.known && b.known && a.lo == a.hi) *out = vrange_of(b.lo << (a.lo & 31), b.hi << (a.lo & 31));
        break;
    case SH_OP_V_LSHRREV_B32:
        if (a.known && b.known && a.lo == a.hi) *out = vrange_of(b.lo >> (a.lo & 31), b.hi >> (a.lo & 31));
        break;
    case SH_OP_V_LSHL_ADD_U32: /* (a << b) + c */
        if (a.known && b.known && c.known && b.lo == b.hi)
            *out = vrange_of((a.lo << (b.lo & 31)) + c.lo, (a.hi << (b.lo & 31)) + c.hi);
        break;
    case SH_OP_V_ADD_LSHL_U32: /* (a + b) << c */
        if (a.known && b.known && c.known && c.lo == c.hi)
            *out = vrange_of((a.lo + b.lo) << (c.lo & 31), (a.hi + b.hi) << (c.lo & 31));
        break;
    case SH_OP_V_MUL_U32_U24:
        if (a.known && b.known && a.hi < (1u << 24) && b.hi < (1u << 24)) *out = vrange_of(a.lo * b.lo, a.hi * b.hi);
        break;
    case SH_OP_V_MAD_U32_U24:
        if (a.known && b.known && c.known && a.hi < (1u << 24) && b.hi < (1u << 24))
            *out = vrange_of(a.lo * b.lo + c.lo, a.hi * b.hi + c.hi);
        break;
    case SH_OP_V_AND_B32: /* x & m <= min(x, m) */
        if (a.known && b.known) *out = vrange_of(0, a.hi < b.hi ? a.hi : b.hi);
        break;
    case SH_OP_V_CVT_U32_F32:
        if (a.known && a.lo == a.hi) *out = vrange_of(f32_to_u32((uint32_t)a.lo), f32_to_u32((uint32_t)a.lo));
        break;
    case SH_OP_V_CNDMASK_B32: *out = vrange_join(a, b); break;
    default: break;
    }
    return true;
}

/* EXEC written by something other than v_cmpx or s_and_saveexec (which only narrow it). */
static bool exec_may_widen(const sh_program *p, uint32_t j) {
    const rdna2_inst *in = &p->insts[j];
    if (p->ops[j] == SH_OP_V_CMPX) return false;
    if (in->name && (!strcmp(in->name, "s_and_saveexec_b64") || !strcmp(in->name, "s_and_saveexec_b32"))) return false;
    bool scalar = in->enc == RDNA2_ENC_SOP1 || in->enc == RDNA2_ENC_SOP2 || in->enc == RDNA2_ENC_SOPK;
    uint32_t w = (p->iflags[j] & SH_IF_64) ? 2 : 1;
    return scalar && RDNA2_OP_EXEC_LO >= in->dst && RDNA2_OP_EXEC_LO < in->dst + w;
}

#define MAX_JOINS 8

bool sh_store_range(const sh_program *p, const sh_eval *e, const sh_stage_info *info, const uint32_t groups[3],
                    uint32_t site, uint64_t *lo, uint64_t *hi) {
    if (!e->vsrc || site >= p->site_count || info->stage != SH_STAGE_CS) return false;
    const sh_site_value *sv = &e->site[site];
    uint32_t s = p->sites[site].inst;
    const rdna2_inst *in = &p->insts[s];
    if ((in->enc != RDNA2_ENC_MUBUF && in->enc != RDNA2_ENC_MTBUF) || !sv->valid || !sv->soffset_known) return false;
    if ((sv->desc[3] >> 23) & 1) return false; /* ADD_TID_ENABLE: swizzled addressing */
    uint32_t stride = (sv->desc[1] >> 16) & 0x3FFF;
    if (in->idxen && !stride) return false;
    static _Thread_local vrange v[256];
    static _Thread_local struct {
        uint32_t at;
        vrange v[256];
    } joins[MAX_JOINS];
    uint32_t njoins = 0;
    bool predicated = false;
    for (uint32_t k = 0; k < 256; k++) v[k] = vrange_top();
    for (uint32_t d = 0; d < info->cs_tid_components && d < 3; d++) /* local ids */
        v[d] = vrange_of(0, info->local_size[d] ? info->local_size[d] - 1 : 0);
    for (uint32_t j = 0; j < s; j++) {
        for (uint32_t k = 0; k < njoins; k++) { /* a forward branch lands here */
            if (joins[k].at != j) continue;
            for (uint32_t r = 0; r < 256; r++) v[r] = vrange_join(v[r], joins[k].v[r]);
            joins[k--] = joins[--njoins];
        }
        uint32_t op = p->ops[j];
        if (p->iflags[j] & SH_IF_FLOW) {
            if (op == SH_OP_S_BRANCH || p->target[j] <= j || !strstr(p->insts[j].name, "cbranch")) return false;
            if (p->target[j] <= s) { /* the instructions it skips may not run */
                if (njoins == MAX_JOINS) return false;
                joins[njoins].at = p->target[j];
                memcpy(joins[njoins].v, v, sizeof(v));
                njoins++;
            }
            continue;
        }
        /* Once EXEC can change, a vector assignment may leave inactive lanes
         * untouched. Retain their old interval, including after EXEC widens.
         * This also permits the common save/compare/restore-EXEC sequence. */
        if (exec_may_widen(p, j) || op == SH_OP_V_CMPX) predicated = true;
        if (op == SH_OP_V_MOVRELD_B32 || op == SH_OP_V_MOVRELSD_B32 ||
            op == SH_OP_V_SWAP_B32 || op == SH_OP_S_SWAPPC_B64) return false;
        if (p->insts[j].enc == RDNA2_ENC_MUBUF || p->insts[j].enc == RDNA2_ENC_MTBUF ||
            p->insts[j].enc == RDNA2_ENC_MIMG || p->insts[j].enc == RDNA2_ENC_FLAT || p->insts[j].enc == RDNA2_ENC_DS) {
            /* loads write VGPRs from vdata on */
            const rdna2_inst *m = &p->insts[j];
            for (uint32_t k = 0; k < p->vmem_width[j] && m->vdata + k < 256; k++) v[m->vdata + k] = vrange_top();
            continue;
        }
        uint32_t d;
        vrange r;
        if (vrange_step(p, e, info, groups, v, j, &d, &r)) v[d] = predicated ? vrange_join(v[d], r) : r;
    }
    if (njoins) { /* a branch past the end of its region into the store itself */
        for (uint32_t k = 0; k < njoins; k++) {
            if (joins[k].at != s) return false;
            for (uint32_t r = 0; r < 256; r++) v[r] = vrange_join(v[r], joins[k].v[r]);
        }
    }
    vrange idx = in->idxen ? v[in->vaddr & 0xFF] : vrange_of(0, 0);
    vrange off = in->offen ? v[(in->vaddr + (in->idxen ? 1u : 0u)) & 0xFF] : vrange_of(0, 0);
    if (!idx.known || !off.known || in->offset < 0) return false;
    uint64_t bytes = p->vmem_width[s] < 16 ? (uint64_t)p->vmem_width[s] * 4 : 64;
    uint64_t base = (uint64_t)sv->soffset + (uint32_t)in->offset;
    *lo = idx.lo * stride + off.lo + base;
    *hi = idx.hi * stride + off.hi + base + bytes;
    return true;
}

void sh_eval_free(sh_eval *e) {
    free(e->smem_data);
    e->smem_data = NULL;
    e->site_dirty = e->smem_words = e->smem_cap = 0;
}

void sh_site_shape_of(const sh_program *p, const sh_eval *e, uint32_t site, sh_site_shape *out) {
    memset(out, 0, sizeof(*out));
    const sh_site_value *v = &e->site[site];
    const rdna2_inst *in = &p->insts[p->sites[site].inst];
    /* Only what the translation of this instruction reads: descriptor fields it
     * ignores stay zero, so the shape (part of the shader and pipeline cache
     * keys) does not change with them. */
    const char *name = in->name ? in->name : "";
    if (p->sites[site].kind == SH_SITE_BUFFER) {
        if (in->idxen) out->buf_stride = (v->desc[1] >> 16) & 0x3FFF; /* without an index it multiplies 0 */
        if (in->enc == RDNA2_ENC_MTBUF || strstr(name, "_format")) { /* typed accesses */
            out->buf_format = in->enc == RDNA2_ENC_MTBUF ? in->format : (uint8_t)((v->desc[3] >> 12) & 0x7F);
            for (int k = 0; k < 4; k++) out->dst_sel[k] = (uint8_t)((v->desc[3] >> (3 * k)) & 7);
        }
    } else if (p->sites[site].kind == SH_SITE_IMAGE) {
        /* SGPR numbers are reused for unrelated descriptors in large materials.
         * Only evaluated descriptor equality proves that this load uses the UAV.
         * The flag is a shape/cache input, so rebinding a texture changes the
         * translation and descriptor layout together. */
        if(p->sites[site].pops_load && v->valid && !v->desc_unknown)
            for(uint32_t w=0;w<p->site_count;w++)
                if(p->sites[w].kind==SH_SITE_IMAGE && p->sites[w].store && e->site[w].valid &&
                   !e->site[w].desc_unknown && !memcmp(v->desc,e->site[w].desc,sizeof(v->desc)))
                    out->ordered_load=1;
        uint32_t fmt = (v->desc[1] >> 20) & 0x1FF;
        sh_buf_format f;
        size_t len = strlen(name);
        out->img_kind = sh_buffer_format(fmt, &f) ? (f.kind == 4 ? 1 : f.kind == 5 ? 2 : 0) : 0;
        out->img_components = f.comps;
        out->img_type = (uint8_t)(v->desc[3] >> 28);
        if (strstr(name, "_c_") || (len >= 2 && !strcmp(name + len - 2, "_c"))) { /* depth-compare variants */
            out->cmp_func = (uint8_t)((v->sampler[0] >> 12) & 7);
            out->cmp_filter = (uint8_t)((((v->sampler[2] >> 20) & 3) == 1 ? 1 : 0) |
                                       (((v->sampler[2] >> 22) & 3) == 1 ? 2 : 0));
        }
        for (int k = 0; k < 4; k++) out->dst_sel[k] = (uint8_t)((v->desc[3] >> (3 * k)) & 7);
        if (sh_site_storage(&p->sites[site],out) && fmt == SH_IMG_PACKED_R11G11B10 && p->ops[p->sites[site].inst] != SH_OP_IMAGE_ATOMIC)
            out->buf_format = SH_IMG_PACKED_R11G11B10;
        if (p->ops[p->sites[site].inst] == SH_OP_IMAGE_ATOMIC && (fmt == 20 || fmt == 22) &&
            (!strcmp(name,"image_atomic_fmin") || !strcmp(name,"image_atomic_fmax")))
            out->buf_format = SH_IMG_ATOMIC_FLOAT32;
        /* Atomics operate on a single 32-bit element, without format
         * conversion (RDNA2 ISA, image atomics). UMIN/UMAX choose unsigned bits,
         * even when ordinary loads of this T# interpret those bits as float. */
        if (p->ops[p->sites[site].inst] == SH_OP_IMAGE_ATOMIC && fmt == 22 &&
            (!strcmp(name,"image_atomic_umin") || !strcmp(name,"image_atomic_umax"))) {
            out->img_kind=1;
            out->buf_format=SH_IMG_ATOMIC_UINT32;
        }
        /* An unvisited or entirely null T# still needs a well-typed dummy
         * declaration. Never infer a live/unknown descriptor's numeric kind. */
        bool null_desc=true;
        for(size_t k=0;k<PS5C_ARRAY_COUNT(v->desc);k++)null_desc&=v->desc[k]==0;
        /* Width/type belong to the instruction. A disabled branch may name
         * a 32-bit dummy T#: it still needs a legal 64-bit image declaration.
         * The backend validates the bound texel width separately. */
        if(p->ops[p->sites[site].inst]==SH_OP_IMAGE_ATOMIC && in->dmask==3 &&
           (!strcmp(name,"image_atomic_umin") || !strcmp(name,"image_atomic_umax"))) {
            out->img_kind=1;out->buf_format=SH_IMG_ATOMIC_UINT64;
        }
        if (p->ops[p->sites[site].inst] == SH_OP_IMAGE_ATOMIC && (!v->valid || null_desc) && !v->desc_unknown) {
            static const char *const uint_ops[]={"swap","cmpswap","add","sub","umin","umax",
                "and","or","xor","inc","dec"};
            const char *tail=!strncmp(name,"image_atomic_",13)?name+13:"";
            for(size_t k=0;k<PS5C_ARRAY_COUNT(uint_ops);k++)
                if(!strcmp(tail,uint_ops[k]))out->img_kind=1;
            if(!strcmp(tail,"smin") || !strcmp(tail,"smax"))out->img_kind=2;
        }
    }
}
