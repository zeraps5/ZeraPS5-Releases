/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/rdna2.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "base/log.h"

#include "gpu/rdna2_ops.inc"

/* VOP3 opcode ranges holding the VOPC, VOP2 and VOP1 opcodes */
#define VOP3_FROM_VOPC 0x000u
#define VOP3_FROM_VOP2 0x100u
#define VOP3_FROM_VOP1 0x180u

static const char *table_name(const char *const *table, size_t size, uint32_t op) {
    return op < size ? table[op] : NULL;
}
#define NAME(t, op) table_name(t, PS5C_ARRAY_COUNT(t), op)

/* Name of a VALU opcode in VOP3 numbering, whatever encoding it came in. */
static const char *valu_name(uint32_t valu) {
    if (valu < VOP3_FROM_VOP2) {
        const char *n = NAME(k_vopc, valu);
        if (n) return n;
    } else if (valu < VOP3_FROM_VOP1) {
        const char *n = NAME(k_vop2, valu - VOP3_FROM_VOP2);
        if (n) return n;
    } else if (valu < VOP3_FROM_VOP1 + 0x80) {
        const char *n = NAME(k_vop1, valu - VOP3_FROM_VOP1);
        if (n) return n;
    }
    return NAME(k_vop3, valu);
}

/* VOP3 opcodes whose dword 0 holds a scalar destination (VOP3B). */
static bool vop3b(uint32_t valu) {
    switch (valu) {
    case 0x128: /* v_add_co_ci_u32 */
    case 0x129: /* v_sub_co_ci_u32 */
    case 0x12A: /* v_subrev_co_ci_u32 */
    case 0x16D: /* v_div_scale_f32 */
    case 0x16E: /* v_div_scale_f64 */
    case 0x176: /* v_mad_u64_u32 */
    case 0x177: /* v_mad_i64_i32 */
    case 0x30F: /* v_add_co_u32 */
    case 0x310: /* v_sub_co_u32 */
    case 0x319: /* v_subrev_co_u32 */
        return true;
    default: return false;
    }
}

int rdna2_valu_opcode(const char *name) {
    for (uint32_t op = 0; op < PS5C_ARRAY_COUNT(k_vop3); op++) {
        const char *n = valu_name(op);
        if (n && !strcmp(n, name)) return (int)op;
    }
    return -1;
}

bool rdna2_valu_vop3b(uint32_t valu) { return vop3b(valu); }

const char *rdna2_op_name(rdna2_enc enc, uint32_t op) {
    switch (enc) {
    case RDNA2_ENC_SOP2: return NAME(k_sop2, op);
    case RDNA2_ENC_SOPK: return NAME(k_sopk, op);
    case RDNA2_ENC_SOP1: return NAME(k_sop1, op);
    case RDNA2_ENC_SOPC: return NAME(k_sopc, op);
    case RDNA2_ENC_SOPP: return NAME(k_sopp, op);
    case RDNA2_ENC_SMEM: return NAME(k_smem, op);
    case RDNA2_ENC_VOP3: return valu_name(op);
    case RDNA2_ENC_DS: return NAME(k_ds, op);
    case RDNA2_ENC_MUBUF: return NAME(k_mubuf, op);
    case RDNA2_ENC_MIMG: return NAME(k_mimg, op);
    default: return NULL;
    }
}

/* Adds the literal dword when a source asks for it. */
static bool take_literal(const uint32_t *code, uint32_t avail, rdna2_inst *in, uint32_t nsrc) {
    for (uint32_t i = 0; i < nsrc; i++) {
        if (in->src[i] == RDNA2_OP_LITERAL) {
            if (in->words >= avail) return false;
            in->literal = code[in->words++];
            return true;
        }
    }
    return true;
}

/* VOP1/VOP2/VOPC src0 may select an SDWA or DPP control dword. */
static bool take_dpp_sdwa(const uint32_t *code, uint32_t avail, rdna2_inst *in) {
    uint16_t s = in->src[0];
    if (s == RDNA2_OP_SDWA || s == RDNA2_OP_DPP16 || s == RDNA2_OP_DPP8 || s == RDNA2_OP_DPP8FI) {
        if (in->words >= avail) return false;
        in->dpp_sdwa = code[in->words++];
    }
    return true;
}

bool rdna2_decode(const uint32_t *code, uint32_t avail, rdna2_inst *in) {
    memset(in, 0, sizeof(*in));
    if (avail == 0) return false;
    uint32_t w0 = code[0], w1 = avail > 1 ? code[1] : 0;
    in->words = 1;
    in->src[0] = in->src[1] = in->src[2] = RDNA2_OP_NULL;

    if ((w0 >> 30) == 2) { /* scalar ALU and SMEM share the 10 prefix */
        uint32_t top9 = w0 >> 23;
        if (top9 == 0x17D) { /* SOP1 */
            in->enc = RDNA2_ENC_SOP1;
            in->op = (w0 >> 8) & 0xFF;
            in->dst = (w0 >> 16) & 0x7F;
            in->src[0] = w0 & 0xFF;
            in->name = NAME(k_sop1, in->op);
            if (!take_literal(code, avail, in, 1)) return false;
        } else if (top9 == 0x17E) { /* SOPC */
            in->enc = RDNA2_ENC_SOPC;
            in->op = (w0 >> 16) & 0x7F;
            in->src[0] = w0 & 0xFF;
            in->src[1] = (w0 >> 8) & 0xFF;
            in->name = NAME(k_sopc, in->op);
            if (!take_literal(code, avail, in, 2)) return false;
        } else if (top9 == 0x17F) { /* SOPP */
            in->enc = RDNA2_ENC_SOPP;
            in->op = (w0 >> 16) & 0x7F;
            in->simm16 = (int16_t)(w0 & 0xFFFF);
            in->name = NAME(k_sopp, in->op);
        } else if ((w0 >> 28) == 0xB) { /* SOPK */
            in->enc = RDNA2_ENC_SOPK;
            in->op = (w0 >> 23) & 0x1F;
            in->dst = (w0 >> 16) & 0x7F;
            in->simm16 = (int16_t)(w0 & 0xFFFF);
            in->name = NAME(k_sopk, in->op);
            if (in->op == 0x15) { /* s_setreg_imm32_b32: 32-bit immediate follows */
                if (avail < 2) return false;
                in->literal = w1;
                in->words = 2;
            }
        } else { /* SOP2 */
            in->enc = RDNA2_ENC_SOP2;
            in->op = (w0 >> 23) & 0x7F;
            in->dst = (w0 >> 16) & 0x7F;
            in->src[0] = w0 & 0xFF;
            in->src[1] = (w0 >> 8) & 0xFF;
            in->name = NAME(k_sop2, in->op);
            if (!take_literal(code, avail, in, 2)) return false;
        }
        return in->name != NULL;
    }

    if ((w0 >> 31) == 0) { /* VOP2 / VOP1 / VOPC */
        uint32_t top7 = w0 >> 25;
        in->src[0] = w0 & 0x1FF;
        if (top7 == 0x3F) {
            in->enc = RDNA2_ENC_VOP1;
            in->op = (w0 >> 9) & 0xFF;
            in->dst = RDNA2_OP_VGPR + ((w0 >> 17) & 0xFF);
            in->valu = (uint16_t)(VOP3_FROM_VOP1 + in->op);
            in->name = NAME(k_vop1, in->op);
        } else if (top7 == 0x3E) {
            in->enc = RDNA2_ENC_VOPC;
            in->op = (w0 >> 17) & 0xFF;
            in->src[1] = RDNA2_OP_VGPR + ((w0 >> 9) & 0xFF);
            in->dst = RDNA2_OP_VCC_LO;
            in->valu = (uint16_t)(VOP3_FROM_VOPC + in->op);
            in->name = NAME(k_vopc, in->op);
        } else {
            in->enc = RDNA2_ENC_VOP2;
            in->op = (w0 >> 25) & 0x3F;
            in->src[1] = RDNA2_OP_VGPR + ((w0 >> 9) & 0xFF);
            in->dst = RDNA2_OP_VGPR + ((w0 >> 17) & 0xFF);
            in->valu = (uint16_t)(VOP3_FROM_VOP2 + in->op);
            in->name = NAME(k_vop2, in->op);
        }
        if (!take_dpp_sdwa(code, avail, in)) return false;
        if (in->enc == RDNA2_ENC_VOP2 && in->name && (strstr(in->name, "mk_") || strstr(in->name, "ak_"))) {
            /* madmk/madak/fmamk/fmaak (and f16 forms) carry a literal constant */
            if (in->words >= avail) return false;
            in->literal = code[in->words++];
            return true;
        }
        if (!take_literal(code, avail, in, 1)) return false;
        return in->name != NULL;
    }

    if (avail < 2) return false;
    uint32_t top6 = w0 >> 26;
    in->words = 2;
    switch (top6) {
    case 0x3D: /* SMEM */
        in->enc = RDNA2_ENC_SMEM;
        in->op = (w0 >> 18) & 0xFF;
        in->sbase = (uint16_t)((w0 & 0x3F) << 1);
        in->dst = (w0 >> 6) & 0x7F;
        in->dlc = (w0 >> 14) & 1;
        in->glc = (w0 >> 16) & 1;
        in->offset = ((int32_t)(w1 << 11)) >> 11; /* signed 21 bits */
        in->soffset = (w1 >> 25) & 0x7F;
        in->name = NAME(k_smem, in->op);
        return in->name != NULL;
    case 0x35: /* VOP3 */
        in->enc = RDNA2_ENC_VOP3;
        in->op = (w0 >> 16) & 0x3FF;
        in->valu = in->op;
        in->src[0] = w1 & 0x1FF;
        in->src[1] = (w1 >> 9) & 0x1FF;
        in->src[2] = (w1 >> 18) & 0x1FF;
        in->omod = (w1 >> 27) & 3;
        in->neg = (w1 >> 29) & 7;
        in->clamp = (w0 >> 15) & 1;
        if (in->op < VOP3_FROM_VOP2 || vop3b(in->op)) {
            /* VOPC in VOP3 form writes an SGPR; VOP3B has a carry SGPR */
            in->sdst = (w0 >> 8) & 0x7F;
            if (in->op < VOP3_FROM_VOP2) {
                in->dst = w0 & 0xFF; /* scalar destination */
                in->abs = (w0 >> 8) & 7;
                in->sdst = 0;
            } else {
                in->dst = RDNA2_OP_VGPR + (w0 & 0xFF);
            }
        } else {
            in->dst = RDNA2_OP_VGPR + (w0 & 0xFF);
            in->abs = (w0 >> 8) & 7;
            in->opsel = (w0 >> 11) & 0xF;
        }
        in->name = valu_name(in->op);
        if (!take_literal(code, avail, in, 3)) return false;
        return in->name != NULL;
    case 0x33: /* VOP3P: 0xCC prefix */
        if ((w0 >> 24) != 0xCC) return false;
        in->enc = RDNA2_ENC_VOP3P;
        in->op = (w0 >> 16) & 0x7F;
        in->dst = RDNA2_OP_VGPR + (w0 & 0xFF);
        in->neg_hi = (w0 >> 8) & 7;
        in->opsel = (w0 >> 11) & 7;
        in->opsel_hi = (uint8_t)(((w1 >> 27) & 3) | (((w0 >> 14) & 1) << 2));
        in->clamp = (w0 >> 15) & 1;
        in->src[0] = w1 & 0x1FF;
        in->src[1] = (w1 >> 9) & 0x1FF;
        in->src[2] = (w1 >> 18) & 0x1FF;
        in->neg = (w1 >> 29) & 7;
        in->name = NAME(k_vop3p, in->op);
        if (!take_literal(code, avail, in, 3)) return false;
        return in->name != NULL;
    case 0x32: /* VINTRP (single dword) */
        in->enc = RDNA2_ENC_VINTRP;
        in->words = 1;
        in->op = (w0 >> 16) & 3;
        in->src[0] = RDNA2_OP_VGPR + (w0 & 0xFF);
        in->attr_chan = (w0 >> 8) & 3;
        in->attr = (w0 >> 10) & 0x3F;
        in->dst = RDNA2_OP_VGPR + ((w0 >> 18) & 0xFF);
        in->name = in->op == 0 ? "v_interp_p1_f32" : in->op == 1 ? "v_interp_p2_f32" : in->op == 2 ? "v_interp_mov_f32" : NULL;
        return in->name != NULL;
    case 0x36: /* DS */
        in->enc = RDNA2_ENC_DS;
        in->op = (w0 >> 18) & 0xFF;
        in->offset = (int32_t)(w0 & 0xFFFF); /* offset0 | offset1 << 8 */
        in->gds = (w0 >> 17) & 1;
        in->vaddr = w1 & 0xFF;
        in->vdata = (w1 >> 8) & 0xFF;
        in->src[1] = (w1 >> 16) & 0xFF; /* data1 */
        in->dst = RDNA2_OP_VGPR + ((w1 >> 24) & 0xFF);
        in->name = NAME(k_ds, in->op);
        return in->name != NULL;
    case 0x38: /* MUBUF */
    case 0x3A: /* MTBUF */
        in->enc = top6 == 0x38 ? RDNA2_ENC_MUBUF : RDNA2_ENC_MTBUF;
        in->offset = (int32_t)(w0 & 0xFFF);
        in->offen = (w0 >> 12) & 1;
        in->idxen = (w0 >> 13) & 1;
        in->glc = (w0 >> 14) & 1;
        in->dlc = (w0 >> 15) & 1;
        in->vaddr = w1 & 0xFF;
        in->vdata = (w1 >> 8) & 0xFF;
        in->srsrc = (uint16_t)(((w1 >> 16) & 0x1F) << 2);
        in->slc = (w1 >> 22) & 1;
        in->tfe = (w1 >> 23) & 1;
        in->soffset = (w1 >> 24) & 0xFF;
        if (top6 == 0x38) {
            in->op = (uint16_t)(((w0 >> 18) & 0x7F) | (((w0 >> 25) & 1) << 7));
            in->name = NAME(k_mubuf, in->op);
        } else {
            in->op = (uint16_t)(((w0 >> 16) & 7) | (((w1 >> 21) & 1) << 3));
            in->format = (w0 >> 19) & 0x7F;
            in->name = NAME(k_mtbuf, in->op);
        }
        return in->name != NULL;
    case 0x3C: { /* MIMG */
        in->enc = RDNA2_ENC_MIMG;
        uint32_t nsa = (w0 >> 1) & 3;
        in->dim = (w0 >> 3) & 7;
        in->dlc = (w0 >> 7) & 1;
        in->dmask = (w0 >> 8) & 0xF;
        in->unorm = (w0 >> 12) & 1;
        in->glc = (w0 >> 13) & 1;
        in->r128 = (w0 >> 15) & 1;
        in->tfe = (w0 >> 16) & 1;
        in->lwe = (w0 >> 17) & 1;
        in->op = (uint16_t)(((w0 >> 18) & 0x7F) | ((w0 & 1) << 7));
        in->slc = (w0 >> 25) & 1;
        in->vaddr = w1 & 0xFF;
        in->vdata = (w1 >> 8) & 0xFF;
        in->srsrc = (uint16_t)(((w1 >> 16) & 0x1F) << 2);
        in->ssamp = (uint16_t)(((w1 >> 21) & 0x1F) << 2);
        in->a16 = (w1 >> 30) & 1;
        in->d16 = (w1 >> 31) & 1;
        if (avail < 2 + nsa) return false;
        for (uint32_t i = 0; i < nsa * 4; i++) in->nsa_addr[i] = (uint8_t)(code[2 + i / 4] >> ((i % 4) * 8));
        in->nsa_count = (uint8_t)(nsa * 4);
        in->words = (uint8_t)(2 + nsa);
        in->name = NAME(k_mimg, in->op);
        return in->name != NULL;
    }
    case 0x3E: /* EXP */
        in->enc = RDNA2_ENC_EXP;
        in->exp_en = w0 & 0xF;
        in->exp_target = (w0 >> 4) & 0x3F;
        in->exp_compr = (w0 >> 10) & 1;
        in->exp_done = (w0 >> 11) & 1;
        in->exp_vm = (w0 >> 12) & 1;
        for (int i = 0; i < 4; i++) in->exp_src[i] = (uint8_t)(w1 >> (i * 8));
        in->name = "exp";
        return true;
    case 0x37: /* FLAT / GLOBAL / SCRATCH */
        in->enc = RDNA2_ENC_FLAT;
        in->offset = ((int32_t)(w0 << 20)) >> 20; /* signed 12 bits */
        in->dlc = (w0 >> 12) & 1;
        in->seg = (w0 >> 14) & 3;
        in->glc = (w0 >> 16) & 1;
        in->slc = (w0 >> 17) & 1;
        in->op = (w0 >> 18) & 0x7F;
        in->vaddr = w1 & 0xFF;
        in->vdata = (w1 >> 8) & 0xFF;
        in->sbase = (w1 >> 16) & 0x7F; /* saddr */
        in->dst = RDNA2_OP_VGPR + ((w1 >> 24) & 0xFF);
        if (in->seg == 0) {
            /* Preserve the generic segment: FLAT may address global, LDS or
             * private memory. Same opcode names as GLOBAL, unsigned 11-bit offset. */
            in->sbase = RDNA2_OP_NULL;
            in->offset = (int32_t)(w0 & 0x7FF);
        }
        in->name = in->seg == 2 || in->seg == 0 ? NAME(k_global, in->op) : in->seg == 1 ? NAME(k_scratch, in->op) : NULL;
        return in->name != NULL;
    default: return false;
    }
}

/* ---------------------------------------------------------------------------
 * Disassembly
 * ------------------------------------------------------------------------- */

typedef struct outbuf {
    char *p;
    size_t left;
    bool ok;
} outbuf;

static void put(outbuf *o, const char *fmt, ...) PS5C_PRINTF_FMT(2, 3);
static void put(outbuf *o, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->p, o->left, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->left) {
        o->ok = false;
        o->left = 0;
        return;
    }
    o->p += n;
    o->left -= (size_t)n;
}

/* Operand width in dwords from the type suffixes of the mnemonic:
 * v_cvt_f64_i32 -> dst 2, src 1. `which` 0 = dst, 1.. = sources. */
static int type_width(const char *name, int which) {
    int widths[4], count = 0;
    for (const char *p = name; *p && count < 4; p++) {
        if (p[0] == '_' && (p[1] == 'f' || p[1] == 'i' || p[1] == 'u' || p[1] == 'b') && p[2] >= '0' && p[2] <= '9') {
            int bits = 0;
            const char *q = p + 2;
            while (*q >= '0' && *q <= '9') bits = bits * 10 + (*q++ - '0');
            if ((bits == 16 || bits == 32 || bits == 64) && (*q == '_' || *q == 0)) widths[count++] = bits == 64 ? 2 : 1;
        }
    }
    if (count == 0) return 1;
    if (which == 0) return widths[0];
    return widths[count - 1];
}

/* Width of operand `which` (0 dst, 1..3 = src0..2), with the exceptions to
 * the naming rule. */
static int operand_width(const rdna2_inst *in, int which) {
    const char *n = in->name;
    if (strstr(n, "_cmp") && which == 0) return 1; /* wave32 mask */
    if (strstr(n, "saveexec_b64") || strstr(n, "wrexec_b64")) return 2;
    if (strstr(n, "saveexec_b32") || strstr(n, "wrexec_b32")) return 1;
    if (!strcmp(n, "v_lshlrev_b64") || !strcmp(n, "v_lshrrev_b64") || !strcmp(n, "v_ashrrev_i64")) {
        return which == 1 ? 1 : 2;
    }
    if (!strcmp(n, "s_lshl_b64") || !strcmp(n, "s_lshr_b64") || !strcmp(n, "s_ashr_i64") || !strcmp(n, "s_bfe_u64") ||
        !strcmp(n, "s_bfe_i64")) {
        return which == 2 ? 1 : 2;
    }
    if (!strcmp(n, "s_bfm_b64")) return which == 0 ? 2 : 1;
    if (!strcmp(n, "s_bitcmp0_b64") || !strcmp(n, "s_bitcmp1_b64")) return which == 2 ? 1 : 2;
    if (!strcmp(n, "s_bitset0_b64") || !strcmp(n, "s_bitset1_b64")) return which == 0 ? 2 : 1;
    if (!strcmp(n, "v_ldexp_f64") || !strcmp(n, "v_trig_preop_f64")) return which == 2 ? 1 : 2;
    if (!strcmp(n, "v_cmp_class_f64") || !strcmp(n, "v_cmpx_class_f64")) return which == 2 ? 1 : (which == 0 ? 1 : 2);
    if (!strcmp(n, "v_mad_u64_u32") || !strcmp(n, "v_mad_i64_i32")) return which == 0 || which == 3 ? 2 : 1;
    if (!strcmp(n, "v_div_scale_f64")) return 2;
    if (!strcmp(n, "v_readlane_b32") || !strcmp(n, "v_writelane_b32")) return 1;
    return type_width(n, which);
}

static void put_reg(outbuf *o, char prefix, uint32_t n, int width) {
    if (width == 1) put(o, "%c%u", prefix, n);
    else put(o, "%c[%u:%u]", prefix, n, n + (uint32_t)width - 1);
}

static void put_operand(outbuf *o, const rdna2_inst *in, uint32_t code, int width) {
    static const char *const fconst[] = {"0.5", "-0.5", "1.0", "-1.0", "2.0", "-2.0", "4.0", "-4.0", "0.15915494"};
    if (code >= RDNA2_OP_VGPR) {
        put_reg(o, 'v', code - RDNA2_OP_VGPR, width);
    } else if (code <= 105) {
        put_reg(o, 's', code, width);
    } else if (code == RDNA2_OP_VCC_LO) {
        put(o, "%s", width == 2 ? "vcc" : "vcc_lo");
    } else if (code == RDNA2_OP_VCC_HI) {
        put(o, "vcc_hi");
    } else if (code >= 108 && code <= 123) {
        if (width == 1) put(o, "ttmp%u", code - 108);
        else put(o, "ttmp[%u:%u]", code - 108, code - 108 + (uint32_t)width - 1);
    } else if (code == RDNA2_OP_M0) {
        put(o, "m0");
    } else if (code == RDNA2_OP_NULL) {
        put(o, "null");
    } else if (code == RDNA2_OP_EXEC_LO) {
        put(o, "%s", width == 2 ? "exec" : "exec_lo");
    } else if (code == RDNA2_OP_EXEC_HI) {
        put(o, "exec_hi");
    } else if (code >= 128 && code <= 192) {
        put(o, "%u", code - 128);
    } else if (code >= 193 && code <= 208) {
        put(o, "%d", -(int)(code - 192));
    } else if (code >= 240 && code <= 248) {
        put(o, "%s", fconst[code - 240]);
    } else if (code == RDNA2_OP_VCCZ) {
        put(o, "src_vccz");
    } else if (code == RDNA2_OP_EXECZ) {
        put(o, "src_execz");
    } else if (code == RDNA2_OP_SCC) {
        put(o, "src_scc");
    } else if (code == RDNA2_OP_LITERAL) {
        put(o, "0x%x", in->literal);
    } else if (code >= 235 && code <= 239) {
        static const char *const special[] = {"src_shared_base", "src_shared_limit", "src_private_base",
                                              "src_private_limit", "src_pops_exiting_wave_id"};
        put(o, "%s", special[code - 235]);
    } else {
        o->ok = false;
    }
}

static void put_src_mods(outbuf *o, const rdna2_inst *in, int i, int width) {
    bool neg = (in->neg >> i) & 1, abs = (in->abs >> i) & 1;
    if (neg) put(o, "-");
    if (abs) put(o, "|");
    put_operand(o, in, in->src[i], width);
    if (abs) put(o, "|");
}

/* Number of VALU sources from the operand fields actually used. */
static int valu_src_count(const rdna2_inst *in) {
    const char *n = in->name;
    if (in->enc == RDNA2_ENC_VOP1) return 1;
    if (in->enc == RDNA2_ENC_VOP2 || in->enc == RDNA2_ENC_VOPC) return 2;
    if (in->valu >= VOP3_FROM_VOP1 && in->valu < VOP3_FROM_VOP1 + 0x80) return strcmp(n, "v_nop") ? 1 : 0;
    if (in->valu < VOP3_FROM_VOPC + 0x100) return 2;
    if (in->valu >= VOP3_FROM_VOP2 && in->valu < VOP3_FROM_VOP2 + 0x40) {
        /* VOP2 in VOP3 form: cndmask and the carry-in forms read a third (mask) source */
        if (!strcmp(n, "v_cndmask_b32") || strstr(n, "_co_ci_") || strstr(n, "fmac") || strstr(n, "mac_")) {
            return strstr(n, "fmac") || strstr(n, "mac_") ? 2 : 3;
        }
        return 2;
    }
    return in->src[2] != 0 || strstr(n, "fma") || strstr(n, "mad") || strstr(n, "3_") || strstr(n, "bfe") ||
                   strstr(n, "bfi") || strstr(n, "lerp") || strstr(n, "align") || strstr(n, "sad") ||
                   strstr(n, "fixup") || strstr(n, "fmas") || strstr(n, "div_scale") || strstr(n, "perm") ||
                   strstr(n, "and_or") || strstr(n, "lshl_or") || strstr(n, "lshl_add") || strstr(n, "add_lshl") ||
                   strstr(n, "xad") || strstr(n, "cube") || strstr(n, "permlane") || strstr(n, "dot")
               ? 3
               : 2;
}

static void disasm_valu(outbuf *o, const rdna2_inst *in) {
    const char *n = in->name;
    bool e64 = in->enc == RDNA2_ENC_VOP3;
    bool is_vop3_only = e64 && in->valu >= 0x140 && !(in->valu >= VOP3_FROM_VOP1 && in->valu < VOP3_FROM_VOP1 + 0x80);
    put(o, "%s%s", n, is_vop3_only ? "" : e64 ? "_e64" : "_e32");
    int nsrc = valu_src_count(in);
    bool cmp = in->valu < VOP3_FROM_VOP2;
    bool cmpx = cmp && !strncmp(n, "v_cmpx", 6);
    bool first = true;
    /* destination */
    if (!(cmpx && !e64) && strcmp(n, "v_nop") != 0) {
        if (cmp) {
            if (e64) {
                if (cmpx) {
                    /* v_cmpx e64 writes only EXEC: no destination operand */
                } else {
                    put(o, " ");
                    put_operand(o, in, in->dst, 1);
                    first = false;
                }
            } else {
                put(o, " vcc_lo");
                first = false;
            }
        } else {
            put(o, " ");
            put_operand(o, in, in->dst, operand_width(in, 0));
            first = false;
        }
    }
    /* carry-out forms */
    bool carry_out = strstr(n, "_co_u32") || strstr(n, "_co_ci_") || !strcmp(n, "v_div_scale_f32") ||
                     !strcmp(n, "v_div_scale_f64") || !strcmp(n, "v_mad_u64_u32") || !strcmp(n, "v_mad_i64_i32");
    if (carry_out) {
        if (e64) {
            put(o, ", ");
            put_operand(o, in, in->sdst, 1);
        } else {
            put(o, ", vcc_lo");
        }
    }
    for (int i = 0; i < nsrc; i++) {
        put(o, "%s", first ? " " : ", ");
        first = false;
        if (i == 2 && !e64 && (!strcmp(n, "v_cndmask_b32") || strstr(n, "_co_ci_"))) {
            put(o, "vcc_lo");
            continue;
        }
        if (in->enc == RDNA2_ENC_VOP2 && i == 1 && strstr(n, "mk_")) {
            /* madmk/fmamk: dst = src0 * K + src1: K comes second */
            put(o, "0x%x, ", in->literal);
        }
        put_src_mods(o, in, i, operand_width(in, i + 1));
    }
    if (in->enc == RDNA2_ENC_VOP2 && strstr(n, "ak_")) put(o, ", 0x%x", in->literal);
    if (!e64 && !strcmp(n, "v_cndmask_b32") && nsrc == 2) put(o, ", vcc_lo");
    if (in->enc == RDNA2_ENC_VOP2 && strstr(n, "_co_ci_")) put(o, ", vcc_lo");
    if (e64) {
        if (in->opsel) {
            put(o, " op_sel:[%u,%u,%u,%u]", in->opsel & 1, (in->opsel >> 1) & 1, (in->opsel >> 2) & 1,
                (in->opsel >> 3) & 1);
        }
        if (in->clamp) put(o, " clamp");
        if (in->omod) put(o, " %s", in->omod == 1 ? "mul:2" : in->omod == 2 ? "mul:4" : "div:2");
    }
    if (in->dpp_sdwa) o->ok = false; /* SDWA/DPP printing not supported yet */
}

static void disasm_scalar(outbuf *o, const rdna2_inst *in) {
    const char *n = in->name;
    put(o, "%s", n);
    switch (in->enc) {
    case RDNA2_ENC_SOP2:
        put(o, " ");
        put_operand(o, in, in->dst, operand_width(in, 0));
        put(o, ", ");
        put_operand(o, in, in->src[0], operand_width(in, 1));
        put(o, ", ");
        put_operand(o, in, in->src[1], operand_width(in, 2));
        break;
    case RDNA2_ENC_SOP1: {
        bool no_src = !strcmp(n, "s_getpc_b64");
        bool no_dst = !strcmp(n, "s_setpc_b64") || !strcmp(n, "s_rfe_b64");
        if (!no_dst) {
            put(o, " ");
            put_operand(o, in, in->dst, operand_width(in, 0));
        }
        if (!no_src) {
            put(o, "%s", no_dst ? " " : ", ");
            put_operand(o, in, in->src[0], operand_width(in, 1));
        }
        break;
    }
    case RDNA2_ENC_SOPC:
        put(o, " ");
        put_operand(o, in, in->src[0], operand_width(in, 1));
        put(o, ", ");
        put_operand(o, in, in->src[1], operand_width(in, 2));
        break;
    case RDNA2_ENC_SOPK:
        if (!strcmp(n, "s_version") || !strcmp(n, "s_waitcnt_vscnt") || !strcmp(n, "s_waitcnt_vmcnt") ||
            !strcmp(n, "s_waitcnt_expcnt") || !strcmp(n, "s_waitcnt_lgkmcnt")) {
            if (strcmp(n, "s_version") != 0) {
                put(o, " ");
                put_operand(o, in, in->dst, 1);
                put(o, ",");
            }
            put(o, " 0x%x", (uint16_t)in->simm16);
        } else if (!strcmp(n, "s_setreg_imm32_b32")) {
            put(o, " hwreg(%u, %u, %u), 0x%x", in->simm16 & 0x3F, (in->simm16 >> 6) & 0x1F,
                ((in->simm16 >> 11) & 0x1F) + 1, in->literal);
        } else if (!strcmp(n, "s_getreg_b32")) {
            put(o, " ");
            put_operand(o, in, in->dst, 1);
            put(o, ", hwreg(%u, %u, %u)", in->simm16 & 0x3F, (in->simm16 >> 6) & 0x1F, ((in->simm16 >> 11) & 0x1F) + 1);
        } else if (!strcmp(n, "s_setreg_b32")) {
            put(o, " hwreg(%u, %u, %u), ", in->simm16 & 0x3F, (in->simm16 >> 6) & 0x1F, ((in->simm16 >> 11) & 0x1F) + 1);
            put_operand(o, in, in->dst, 1);
        } else {
            put(o, " ");
            put_operand(o, in, in->dst, strstr(n, "call_b64") ? 2 : 1);
            put(o, ", 0x%x", (uint16_t)in->simm16);
        }
        break;
    case RDNA2_ENC_SOPP:
        if (!strcmp(n, "s_endpgm") || !strcmp(n, "s_barrier") || !strcmp(n, "s_wakeup") || !strcmp(n, "s_icache_inv") ||
            !strcmp(n, "s_ttracedata") || !strcmp(n, "s_endpgm_saved") || !strcmp(n, "s_endpgm_ordered_ps_done") ||
            !strcmp(n, "s_code_end") || !strcmp(n, "s_wait_idle")) {
            if (in->simm16) put(o, " 0x%x", (uint16_t)in->simm16);
        } else {
            put(o, " 0x%x", (uint16_t)in->simm16);
        }
        break;
    default: o->ok = false; break;
    }
}

static void disasm_smem(outbuf *o, const rdna2_inst *in) {
    const char *n = in->name;
    put(o, "%s", n);
    int width = strstr(n, "x16") ? 16 : strstr(n, "x8") ? 8 : strstr(n, "x4") ? 4 : strstr(n, "x2") ? 2 : 1;
    bool buffer = strstr(n, "buffer") != NULL;
    if (!strcmp(n, "s_memtime") || !strcmp(n, "s_memrealtime")) {
        put(o, " ");
        put_reg(o, 's', in->dst, 2);
        return;
    }
    if (!strcmp(n, "s_gl1_inv") || !strcmp(n, "s_dcache_inv")) return;
    put(o, " ");
    put_operand(o, in, in->dst, width);
    put(o, ", ");
    put_reg(o, 's', in->sbase, buffer ? 4 : 2);
    if (in->soffset != RDNA2_OP_NULL) {
        put(o, ", ");
        put_operand(o, in, in->soffset, 1);
        if (in->offset) put(o, " offset:0x%x", (uint32_t)in->offset & 0x1FFFFF);
    } else {
        put(o, ", 0x%x", (uint32_t)in->offset & 0x1FFFFF);
    }
    if (in->glc) put(o, " glc");
    if (in->dlc) put(o, " dlc");
}

static const char *const k_dims[8] = {"SQ_RSRC_IMG_1D",      "SQ_RSRC_IMG_2D",       "SQ_RSRC_IMG_3D",
                                      "SQ_RSRC_IMG_CUBE",    "SQ_RSRC_IMG_1D_ARRAY", "SQ_RSRC_IMG_2D_ARRAY",
                                      "SQ_RSRC_IMG_2D_MSAA", "SQ_RSRC_IMG_2D_MSAA_ARRAY"};

static int popcount4(uint32_t v) { return (int)((v & 1) + ((v >> 1) & 1) + ((v >> 2) & 1) + ((v >> 3) & 1)); }

static void disasm_mem(outbuf *o, const rdna2_inst *in) {
    const char *n = in->name;
    put(o, "%s", n);
    if (in->enc == RDNA2_ENC_MUBUF || in->enc == RDNA2_ENC_MTBUF) {
        if (!strcmp(n, "buffer_gl0_inv") || !strcmp(n, "buffer_gl1_inv")) return;
        int w = strstr(n, "xyzw") || strstr(n, "x4") ? 4 : strstr(n, "xyz") || strstr(n, "x3") ? 3 :
                strstr(n, "xy") || strstr(n, "x2") ? 2 : 1;
        if (strstr(n, "d16")) w = (w + 1) / 2;
        if (strstr(n, "cmpswap")) w = 2;
        if (in->tfe) w++;
        put(o, " ");
        put_reg(o, 'v', in->vdata, w);
        put(o, ", ");
        if (in->offen || in->idxen) put_reg(o, 'v', in->vaddr, in->offen && in->idxen ? 2 : 1);
        else put(o, "off");
        put(o, ", ");
        put_reg(o, 's', in->srsrc, 4);
        put(o, ", ");
        put_operand(o, in, in->soffset, 1);
        if (in->enc == RDNA2_ENC_MTBUF) put(o, " format:%u", in->format);
        if (in->idxen) put(o, " idxen");
        if (in->offen) put(o, " offen");
        if (in->offset) put(o, " offset:%d", in->offset);
    } else if (in->enc == RDNA2_ENC_MIMG) {
        int w = popcount4(in->dmask);
        if (strstr(n, "gather4")) w = 4;
        if (w == 0) w = 1;
        if (in->d16) w = (w + 1) / 2;
        if (in->tfe || in->lwe) w++;
        put(o, " ");
        put_reg(o, 'v', in->vdata, w);
        put(o, ", ");
        if (in->nsa_count) {
            /* address registers: vaddr then the NSA list, until a zero byte past the needed count */
            put(o, "[v%u", in->vaddr);
            for (int i = 0; i < in->nsa_count && in->nsa_addr[i]; i++) put(o, ", v%u", in->nsa_addr[i]);
            put(o, "]");
            o->ok = false; /* the address count cannot be recovered from the encoding alone */
        } else {
            put(o, "v%u", in->vaddr); /* width unknown: printed as one register */
            o->ok = false;
        }
        put(o, ", ");
        put_reg(o, 's', in->srsrc, in->r128 ? 4 : 8);
        if (strstr(n, "sample") || strstr(n, "gather") || strstr(n, "get_lod")) {
            put(o, ", ");
            put_reg(o, 's', in->ssamp, 4);
        }
        put(o, " dmask:0x%x dim:%s", in->dmask, k_dims[in->dim]);
        if (in->unorm) put(o, " unorm");
        if (in->a16) put(o, " a16");
        if (in->d16) put(o, " d16");
    } else if (in->enc == RDNA2_ENC_FLAT) {
        bool store = strstr(n, "store") != NULL, atomic = strstr(n, "atomic") != NULL;
        int w = strstr(n, "x4") ? 4 : strstr(n, "x3") ? 3 : strstr(n, "x2") ? 2 : 1;
        put(o, " ");
        if (!store) {
            put_reg(o, 'v', in->dst - RDNA2_OP_VGPR, w);
            put(o, ", ");
        }
        put_reg(o, 'v', in->vaddr, in->sbase == RDNA2_OP_NULL ? 2 : 1);
        if (store || atomic) {
            put(o, ", ");
            put_reg(o, 'v', in->vdata, w);
        }
        put(o, ", ");
        if (in->sbase == RDNA2_OP_NULL) put(o, "off");
        else put_reg(o, 's', in->sbase, 2);
        if (in->offset) put(o, " offset:%d", in->offset);
    } else if (in->enc == RDNA2_ENC_DS) {
        if (in->op == 0x35) {
            put(o, " v%u, v%u offset:%d", in->dst - RDNA2_OP_VGPR, in->vaddr, in->offset);
        } else {
            o->ok = false; /* operand layout differs per opcode: not printed exactly yet */
            put(o, " v%u, v%u offset:%d", in->vaddr, in->vdata, in->offset);
        }
    }
    if (in->glc) put(o, " glc");
    if (in->slc) put(o, " slc");
    if (in->dlc) put(o, " dlc");
    if (in->enc != RDNA2_ENC_MIMG && in->tfe) put(o, " tfe");
    if (in->enc == RDNA2_ENC_MIMG && in->tfe) put(o, " tfe");
    if (in->enc == RDNA2_ENC_MIMG && in->lwe) put(o, " lwe");
    if (in->enc == RDNA2_ENC_MIMG && in->r128) put(o, " r128");
}

static void disasm_exp(outbuf *o, const rdna2_inst *in) {
    uint32_t t = in->exp_target;
    if (t <= 7) put(o, "exp mrt%u", t);
    else if (t == 8) put(o, "exp mrtz");
    else if (t == 9) put(o, "exp null");
    else if (t >= 12 && t <= 16) put(o, "exp pos%u", t - 12);
    else if (t == 20) put(o, "exp prim");
    else if (t >= 32 && t <= 63) put(o, "exp param%u", t - 32);
    else {
        o->ok = false;
        put(o, "exp target%u", t);
    }
    for (int i = 0; i < 4; i++) {
        /* compr: two packed registers, enabled by bit pairs; LLVM prints each twice */
        bool on = in->exp_compr ? (in->exp_en >> (i / 2 * 2)) & 3 : (in->exp_en >> i) & 1;
        uint32_t reg = in->exp_compr ? in->exp_src[i / 2] : in->exp_src[i];
        if (on) put(o, "%s v%u", i ? "," : "", reg);
        else put(o, "%s off", i ? "," : "");
    }
    if (in->exp_done) put(o, " done");
    if (in->exp_compr) put(o, " compr");
    if (in->exp_vm) put(o, " vm");
}

bool rdna2_disasm(const rdna2_inst *in, char *buf, size_t size) {
    outbuf o = {buf, size, true};
    if (size) buf[0] = 0;
    if (!in->name) return false;
    switch (in->enc) {
    case RDNA2_ENC_SOP2:
    case RDNA2_ENC_SOP1:
    case RDNA2_ENC_SOPC:
    case RDNA2_ENC_SOPK:
    case RDNA2_ENC_SOPP: disasm_scalar(&o, in); break;
    case RDNA2_ENC_SMEM: disasm_smem(&o, in); break;
    case RDNA2_ENC_VOP1:
    case RDNA2_ENC_VOP2:
    case RDNA2_ENC_VOPC:
    case RDNA2_ENC_VOP3: disasm_valu(&o, in); break;
    case RDNA2_ENC_VOP3P:
        put(&o, "%s ", in->name);
        put_operand(&o, in, in->dst, 1);
        for (int i = 0; i < 3; i++) {
            if (i == 2 && in->src[2] == 0) break;
            put(&o, ", ");
            if ((in->neg >> i) & 1) put(&o, "-");
            put_operand(&o, in, in->src[i], 1);
        }
        if (in->opsel) put(&o, " op_sel:[%u,%u,%u]", in->opsel & 1, (in->opsel >> 1) & 1, (in->opsel >> 2) & 1);
        if (in->opsel_hi != 7) {
            put(&o, " op_sel_hi:[%u,%u,%u]", in->opsel_hi & 1, (in->opsel_hi >> 1) & 1, (in->opsel_hi >> 2) & 1);
        }
        if (in->neg_hi) put(&o, " neg_hi:[%u,%u,%u]", in->neg_hi & 1, (in->neg_hi >> 1) & 1, (in->neg_hi >> 2) & 1);
        if (in->clamp) put(&o, " clamp");
        break;
    case RDNA2_ENC_VINTRP:
        put(&o, "%s v%u, ", in->name, in->dst - RDNA2_OP_VGPR);
        if (in->op == 2) put(&o, "%s", (in->src[0] & 3) == 0 ? "p10" : (in->src[0] & 3) == 1 ? "p20" : "p0");
        else put(&o, "v%u", in->src[0] - RDNA2_OP_VGPR);
        put(&o, ", attr%u.%c", in->attr, "xyzw"[in->attr_chan]);
        break;
    case RDNA2_ENC_EXP: disasm_exp(&o, in); break;
    default: disasm_mem(&o, in); break;
    }
    return o.ok;
}
