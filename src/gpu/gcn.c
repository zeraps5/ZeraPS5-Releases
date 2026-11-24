/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/gcn.h"

#include <stdlib.h>
#include <string.h>

#include "gpu/rdna2.h"

#define LIT 255u     /* literal operand code (both ISAs) */
#define VCC 106u     /* vcc_lo / vcc */
#define SNULL 125u   /* RDNA2 null SGPR */
#define MAX_WORDS 0x4000u

/* ---- GCN instruction sizes ---- */

typedef enum gcn_enc {
    G_UNKNOWN, G_SOP2, G_SOPK, G_SOP1, G_SOPC, G_SOPP, G_SMRD, G_VOP2, G_VOP1, G_VOPC, G_VOP3, G_VINTRP,
    G_DS, G_MUBUF, G_MTBUF, G_MIMG, G_EXP, G_FLAT,
} gcn_enc;

static gcn_enc enc_of(uint32_t w) {
    switch (w >> 23) {
    case 0x17D: return G_SOP1;
    case 0x17E: return G_SOPC;
    case 0x17F: return G_SOPP;
    default: break;
    }
    if (w >> 28 == 0xB) return G_SOPK;
    if (w >> 30 == 2) return G_SOP2;
    if (w >> 27 == 0x18) return G_SMRD;
    switch (w >> 26) {
    case 0x32: return G_VINTRP;
    case 0x34: return G_VOP3;
    case 0x36: return G_DS;
    case 0x37: return G_FLAT;
    case 0x38: return G_MUBUF;
    case 0x3A: return G_MTBUF;
    case 0x3C: return G_MIMG;
    case 0x3E: return G_EXP;
    default: break;
    }
    if (w >> 25 == 0x3F) return G_VOP1;
    if (w >> 25 == 0x3E) return G_VOPC;
    if (w >> 31 == 0) return G_VOP2;
    return G_UNKNOWN;
}

/* Size in dwords of the GCN instruction at code (0: unknown or truncated) */
static uint32_t gcn_size(const uint32_t *code, uint32_t avail) {
    if (!avail) return 0;
    uint32_t w = code[0], n;
    switch (enc_of(w)) {
    case G_SOP2:
    case G_SOPC: n = 1 + ((w & 0xFF) == LIT || ((w >> 8) & 0xFF) == LIT); break;
    case G_SOP1: n = 1 + ((w & 0xFF) == LIT); break;
    case G_SOPK: n = 1 + (((w >> 23) & 0x1F) == 21); break; /* s_setreg_imm32_b32 */
    case G_SOPP:
    case G_VINTRP: n = 1; break;
    case G_SMRD: n = 1 + (!((w >> 8) & 1) && (w & 0xFF) == LIT); break;
    case G_VOP1:
    case G_VOPC: n = 1 + ((w & 0x1FF) == LIT); break;
    case G_VOP2: {
        uint32_t op = (w >> 25) & 0x3F;
        n = 1 + ((w & 0x1FF) == LIT || op == 32 || op == 33); /* v_madmk_f32 / v_madak_f32 carry a constant */
        break;
    }
    case G_UNKNOWN: return 0;
    default: n = 2; break;
    }
    return n <= avail ? n : 0;
}

uint32_t gcn_fetch_size(const uint32_t *code, uint32_t avail) {
    for (uint32_t i = 0; i < avail && i < MAX_WORDS;) {
        if (enc_of(code[i]) == G_SOP1 && ((code[i] >> 8) & 0xFF) == 32) return i + 1; /* s_setpc_b64 */
        if (code[i] == 0xBF810000u) return 0;                                       /* s_endpgm */
        uint32_t n = gcn_size(code + i, avail - i);
        if (!n) return 0;
        i += n;
    }
    return 0;
}

/* ---- VALU opcodes: GCN numbering -> names of the RDNA2 instructions that do the same ---- */

/* GCN VOP2 opcodes. NULL: none in RDNA2 */
static const char *const k_vop2[50] = {
    "v_cndmask_b32", NULL, NULL, "v_add_f32", "v_sub_f32", "v_subrev_f32", "v_mac_legacy_f32", "v_mul_legacy_f32",
    "v_mul_f32", "v_mul_i32_i24", "v_mul_hi_i32_i24", "v_mul_u32_u24", "v_mul_hi_u32_u24", NULL, NULL, "v_min_f32",
    "v_max_f32", "v_min_i32", "v_max_i32", "v_min_u32", "v_max_u32", "v_lshrrev_b32", "v_lshrrev_b32",
    "v_ashrrev_i32", "v_ashrrev_i32", "v_lshlrev_b32", "v_lshlrev_b32", "v_and_b32", "v_or_b32", "v_xor_b32",
    "v_bfm_b32", "v_mac_f32", "v_madmk_f32", "v_madak_f32", "v_bcnt_u32_b32", "v_mbcnt_lo_u32_b32",
    "v_mbcnt_hi_u32_b32", "v_add_co_u32", "v_sub_co_u32", "v_subrev_co_u32", "v_add_co_ci_u32", "v_sub_co_ci_u32",
    "v_subrev_co_ci_u32", "v_ldexp_f32", NULL, "v_cvt_pknorm_i16_f32", "v_cvt_pknorm_u16_f32",
    "v_cvt_pkrtz_f16_f32", "v_cvt_pk_u16_u32", "v_cvt_pk_i16_i32",
};
/* v_lshr_b32, v_ashr_i32, v_lshl_b32: the reversed RDNA2 forms, sources swapped */
static bool vop2_swapped(uint32_t op) { return op == 21 || op == 23 || op == 25; }
/* v_add_i32, v_sub_i32, v_subrev_i32 write their carry to vcc: VOP3B in RDNA2 */
static bool vop2_carry_out(uint32_t op) { return op >= 37 && op <= 39; }

/* GCN VOP3-only opcodes 0x140..0x177 */
static const char *const k_vop3[56] = {
    "v_mad_legacy_f32", "v_mad_f32", "v_mad_i32_i24", "v_mad_u32_u24", "v_cubeid_f32", "v_cubesc_f32",
    "v_cubetc_f32", "v_cubema_f32", "v_bfe_u32", "v_bfe_i32", "v_bfi_b32", "v_fma_f32", "v_fma_f64", "v_lerp_u8",
    "v_alignbit_b32", "v_alignbyte_b32", NULL, "v_min3_f32", "v_min3_i32", "v_min3_u32", "v_max3_f32",
    "v_max3_i32", "v_max3_u32", "v_med3_f32", "v_med3_i32", "v_med3_u32", "v_sad_u8", "v_sad_hi_u8",
    "v_sad_u16", "v_sad_u32", "v_cvt_pk_u8_f32", "v_div_fixup_f32", "v_div_fixup_f64", "v_lshlrev_b64",
    "v_lshrrev_b64", "v_ashrrev_i64", "v_add_f64", "v_mul_f64", "v_min_f64", "v_max_f64", "v_ldexp_f64",
    "v_mul_lo_u32", "v_mul_hi_u32", "v_mul_lo_u32" /* v_mul_lo_i32: the same low bits */, "v_mul_hi_i32",
    "v_div_scale_f32", "v_div_scale_f64", "v_div_fmas_f32", "v_div_fmas_f64", "v_msad_u8", "v_qsad_pk_u16_u8",
    "v_mqsad_pk_u16_u8", "v_trig_preop_f64", "v_mqsad_u32_u8", "v_mad_u64_u32", "v_mad_i64_i32",
};
static bool vop3_swapped(uint32_t op) { return op >= 0x161 && op <= 0x163; } /* 64-bit shifts */
static bool gcn_vop3b(uint32_t op) {
    return (op >= 0x125 && op <= 0x12A) || op == 0x16D || op == 0x16E || op == 0x176 || op == 0x177;
}

/* RDNA2 VALU opcode (VOP3 numbering) of a GCN one in VOP3 numbering; -1 if none */
static int valu_target(uint32_t op, bool *swap) {
    *swap = false;
    if (op < 0x100) { /* VOPC: same opcodes, but for the v_cmps_* ones RDNA2 lacks */
        if (op >= 0x40 && op < 0x80) return -1;
        return rdna2_op_name(RDNA2_ENC_VOP3, op) ? (int)op : -1;
    }
    if (op < 0x140) {
        uint32_t v = op - 0x100;
        if (v >= PS5C_ARRAY_COUNT(k_vop2) || !k_vop2[v]) return -1;
        *swap = vop2_swapped(v);
        return rdna2_valu_opcode(k_vop2[v]);
    }
    if (op < 0x180) {
        uint32_t v = op - 0x140;
        if (v >= PS5C_ARRAY_COUNT(k_vop3) || !k_vop3[v]) return -1;
        *swap = vop3_swapped(op);
        return rdna2_valu_opcode(k_vop3[v]);
    }
    if (op - 0x180 >= 0x47) return -1; /* RDNA2 VOP1 opcodes past GCN's are others */
    return rdna2_op_name(RDNA2_ENC_VOP3, op) ? (int)op : -1;
}

typedef struct out_inst {
    uint32_t w[4];
    uint32_t n;
} out_inst;

static void put(out_inst *o, uint32_t w) { o->w[o->n++] = w; }

/* RDNA2 VOP3 (VOP3A, or VOP3B when the opcode has a scalar destination) */
static void put_vop3(out_inst *o, uint32_t t, uint32_t vdst, uint32_t sdst_or_abs, bool clamp, uint32_t s0, uint32_t s1,
                     uint32_t s2, uint32_t omod, uint32_t neg) {
    put(o, (0x35u << 26) | (t << 16) | ((uint32_t)clamp << 15) | ((sdst_or_abs & 0x7F) << 8) | (vdst & 0xFF));
    put(o, (s0 & 0x1FF) | ((s1 & 0x1FF) << 9) | ((s2 & 0x1FF) << 18) | ((omod & 3) << 27) | ((neg & 7) << 29));
}

static uint32_t swap_bits01(uint32_t m) { return (m & 4) | ((m & 1) << 1) | ((m >> 1) & 1); }

static const char *conv_valu(const uint32_t *c, gcn_enc e, out_inst *o) {
    uint32_t w = c[0];
    bool swap;
    if (e == G_VOP1 || e == G_VOPC || e == G_VOP2) {
        uint32_t src0 = w & 0x1FF, lit = src0 == LIT ? c[1] : 0;
        if (e == G_VOP1) {
            uint32_t op = (w >> 9) & 0xFF;
            if (valu_target(0x180 + op, &swap) < 0) return "VOP1 opcode RDNA2 lacks";
            put(o, w); /* same layout and opcode */
            if (src0 == LIT) put(o, lit);
            return NULL;
        }
        if (e == G_VOPC) {
            uint32_t op = (w >> 17) & 0xFF;
            if (valu_target(op, &swap) < 0) return "VOPC opcode RDNA2 lacks";
            put(o, w);
            if (src0 == LIT) put(o, lit);
            return NULL;
        }
        uint32_t op = (w >> 25) & 0x3F, vsrc1 = 256 + ((w >> 9) & 0xFF), vdst = (w >> 17) & 0xFF;
        int t = valu_target(0x100 + op, &swap);
        if (t < 0) return "VOP2 opcode RDNA2 lacks";
        if (op == 32 || op == 33) lit = c[1]; /* v_madmk_f32 / v_madak_f32: the constant follows */
        if (t >= 0x100 && t < 0x140 && !swap && !vop2_carry_out(op)) {
            put(o, ((uint32_t)(t - 0x100) << 25) | (w & 0x01FFFFFFu));
            if (src0 == LIT || op == 32 || op == 33) put(o, lit);
            return NULL;
        }
        uint32_t s0 = src0, s1 = vsrc1;
        if (swap) { s0 = vsrc1; s1 = src0; }
        put_vop3(o, (uint32_t)t, vdst, vop2_carry_out(op) ? VCC : 0, false, s0, s1, 0, 0, 0);
        if (src0 == LIT) put(o, lit);
        return NULL;
    }
    /* VOP3 */
    uint32_t w1 = c[1], op = (w >> 17) & 0x1FF, vdst = w & 0xFF;
    int t = valu_target(op, &swap);
    if (t < 0) return "VOP3 opcode RDNA2 lacks";
    uint32_t s0 = w1 & 0x1FF, s1 = (w1 >> 9) & 0x1FF, s2 = (w1 >> 18) & 0x1FF, omod = (w1 >> 27) & 3, neg = (w1 >> 29) & 7;
    bool b = gcn_vop3b(op);
    uint32_t abs = b ? 0 : (w >> 8) & 7, sdst = b ? (w >> 8) & 0x7F : 0;
    bool clamp = !b && ((w >> 11) & 1);
    if (swap) {
        uint32_t x = s0;
        s0 = s1;
        s1 = x;
        abs = swap_bits01(abs);
        neg = swap_bits01(neg);
    }
    if (rdna2_valu_vop3b((uint32_t)t) != b) return "VOP3 form differs";
    put_vop3(o, (uint32_t)t, vdst, b ? sdst : abs, clamp, s0, s1, s2, omod, neg);
    return NULL;
}

/* GCN dfmt/nfmt -> RDNA2 unified buffer format (0: none) */
static uint32_t buffer_format(uint32_t dfmt, uint32_t nfmt) {
    /* first format of each data format (unorm, snorm, uscaled, sscaled, uint, sint follow), its float */
    static const uint8_t base[15] = {0, 1, 7, 14, 0, 23, 30, 37, 44, 50, 56, 0, 65, 0, 0};
    static const uint8_t flt[15] = {0, 0, 13, 0, 22, 29, 36, 43, 0, 0, 0, 64, 71, 74, 77};
    if (dfmt >= 15) return 0;
    if (dfmt == 4 || dfmt == 11 || dfmt == 13 || dfmt == 14) { /* 32-bit channels: uint, sint, float */
        static const uint8_t first[15] = {0, 0, 0, 0, 20, 0, 0, 0, 0, 0, 0, 62, 0, 72, 75};
        return nfmt == 4 ? first[dfmt] : nfmt == 5 ? first[dfmt] + 1u : nfmt == 7 ? flt[dfmt] : 0;
    }
    if (nfmt == 7) return flt[dfmt];
    return base[dfmt] && nfmt <= 5 ? base[dfmt] + nfmt : 0;
}

/* One instruction (not a branch, not s_swappc). NULL or why it cannot be re-encoded. */
static const char *conv_one(const uint32_t *c, uint32_t n, out_inst *o) {
    uint32_t w = c[0];
    gcn_enc e = enc_of(w);
    o->n = 0;
    switch (e) {
    case G_SOP2: {
        uint32_t op = (w >> 23) & 0x7F;
        if (op > 42 || !rdna2_op_name(RDNA2_ENC_SOP2, op)) return "SOP2 opcode";
        break;
    }
    case G_SOPK: {
        uint32_t op = (w >> 23) & 0x1F;
        if (op == 1 || op == 17 || op > 21 || !rdna2_op_name(RDNA2_ENC_SOPK, op)) return "SOPK opcode";
        break;
    }
    case G_SOP1: {
        uint32_t op = (w >> 8) & 0xFF;
        if (op > 0x34 || op == 0x32 || op == 0x33 || op == 0x20 || !rdna2_op_name(RDNA2_ENC_SOP1, op))
            return op == 0x20 ? "s_setpc_b64" : "SOP1 opcode";
        break;
    }
    case G_SOPC:
        if (((w >> 16) & 0x7F) > 15) return "SOPC opcode";
        break;
    case G_SOPP: {
        uint32_t op = (w >> 16) & 0x7F;
        if (op > 26 || !rdna2_op_name(RDNA2_ENC_SOPP, op)) return "SOPP opcode";
        if (op == 12) { /* s_waitcnt: vmcnt [3:0] (+[15:14] in RDNA2), expcnt [6:4], lgkmcnt [12:8] -> [13:8] */
            uint32_t vm = w & 0xF, exp = (w >> 4) & 7, lgkm = (w >> 8) & 0x1F;
            put(o, 0xBF8C0000u | vm | (exp << 4) | ((lgkm >= 15 ? 0x3Fu : lgkm) << 8) | (vm == 15 ? 3u << 14 : 0));
            return NULL;
        }
        break;
    }
    case G_SMRD: {
        uint32_t op = (w >> 22) & 0x1F, sdst = (w >> 15) & 0x7F, sbase = (w >> 9) & 0x3F, imm = (w >> 8) & 1,
                 off = w & 0xFF;
        uint32_t t = op;
        if (op == 0x1E) t = 0x24;                   /* s_memtime */
        else if (op == 0x1F || op == 0x1D) t = 0x20; /* s_dcache_inv(_vol) */
        else if (op > 12 || (op > 4 && op < 8)) return "SMRD opcode";
        int64_t bytes = 0;
        uint32_t soffset = SNULL;
        if (imm) bytes = (int64_t)off * 4;
        else if (off == LIT) bytes = (int64_t)c[1] * 4;
        else soffset = off;
        if (bytes >= (1 << 20)) return "SMRD offset";
        put(o, (0x3Du << 26) | (t << 18) | (sdst << 6) | sbase);
        put(o, ((uint32_t)bytes & 0x1FFFFF) | (soffset << 25));
        return NULL;
    }
    case G_VOP1:
    case G_VOP2:
    case G_VOPC:
    case G_VOP3: return conv_valu(c, e, o);
    case G_VINTRP:
    case G_EXP: break; /* same encodings */
    case G_DS:
        if (!rdna2_op_name(RDNA2_ENC_DS, (w >> 18) & 0xFF)) return "DS opcode";
        break;
    case G_MUBUF: {
        uint32_t op = (w >> 18) & 0x7F;
        if ((w >> 15) & 1) return "MUBUF addr64";
        if ((w >> 16) & 1) return "MUBUF lds";
        /* loads and stores, and the atomics (0x30..0x40, x2 0x50..0x60): numbered as on RDNA2 */
        bool atomic = (op >= 0x30 && op <= 0x40) || (op >= 0x50 && op <= 0x60);
        if ((op > 0x1F && !atomic) || !rdna2_op_name(RDNA2_ENC_MUBUF, op)) return "MUBUF opcode";
        put(o, w & ~(1u << 25));
        put(o, c[1] & ~(1u << 21));
        return NULL;
    }
    case G_MTBUF: {
        if ((w >> 15) & 1) return "MTBUF addr64";
        uint32_t fmt = buffer_format((w >> 19) & 0xF, (w >> 23) & 7);
        if (!fmt) return "MTBUF format";
        put(o, (w & 0x0007FFFFu & ~(1u << 15)) | (fmt << 19) | (0x3Au << 26));
        put(o, c[1] & ~(1u << 21));
        return NULL;
    }
    case G_MIMG: {
        uint32_t op = (w >> 18) & 0x7F;
        if (!rdna2_op_name(RDNA2_ENC_MIMG, op)) return "MIMG opcode";
        bool da = (w >> 14) & 1;
        /* the dimension is the resource's on GCN; 2D (array) until resources tell */
        put(o, (w & ~((1u << 14) | 0xFFu)) | ((da ? 5u : 1u) << 3));
        put(o, c[1] & 0x03FFFFFFu);
        return NULL;
    }
    default: return e == G_FLAT ? "FLAT" : "unknown encoding";
    }
    for (uint32_t i = 0; i < n; i++) put(o, c[i]);
    return NULL;
}

static bool is_branch(uint32_t w) {
    if (enc_of(w) != G_SOPP) return false;
    uint32_t op = (w >> 16) & 0x7F;
    return op == 2 || (op >= 4 && op <= 9) || (op >= 23 && op <= 26);
}

typedef struct emitter {
    uint32_t *out, cap, n;
    gcn_error *err;
    const uint32_t *emit; /* replaces s_sendmsg GS emit (geometry shader passes); NULL: kept */
    uint32_t emit_words;
} emitter;

/* s_sendmsg MSG_GS with OP emit (2) or emit-cut (3) */
static bool is_gs_emit(uint32_t w) {
    return (w & 0xFFFF0000u) == 0xBF900000u && (w & 0xFu) == 2u && ((w >> 4) & 3u) >= 2u;
}

static bool fail(emitter *em, uint32_t pos, uint32_t word, bool fetch, const char *why) {
    if (em->err) *em->err = (gcn_error){pos, word, fetch, why};
    return false;
}

/* One program: until s_endpgm (included), or s_setpc_b64 (excluded) in a fetch shader */
static bool emit_program(emitter *em, const uint32_t *code, uint32_t avail, bool is_fetch, const uint32_t *fetch,
                         uint32_t fetch_words) {
    if (avail > MAX_WORDS) avail = MAX_WORDS;
    uint32_t *map = (uint32_t *)malloc(((size_t)avail + 1) * sizeof(uint32_t));
    uint32_t (*fix)[2] = (uint32_t(*)[2])malloc(((size_t)avail + 1) * sizeof(*fix));
    bool ok = map && fix;
    uint32_t nfix = 0, pos = 0, start = em->n;
    uint32_t far = 0; /* furthest forward branch target: code after an early s_endpgm is still reachable */
    if (map) memset(map, 0xFF, ((size_t)avail + 1) * sizeof(uint32_t));
    while (ok) {
        if (pos >= avail) {
            ok = fail(em, pos, 0, is_fetch, is_fetch ? "no s_setpc_b64" : "no s_endpgm");
            break;
        }
        const uint32_t *c = code + pos;
        uint32_t n = gcn_size(c, avail - pos);
        if (!n) {
            ok = fail(em, pos, c[0], is_fetch, "unknown or truncated instruction");
            break;
        }
        map[pos] = em->n - start;
        bool setpc = enc_of(c[0]) == G_SOP1 && ((c[0] >> 8) & 0xFF) == 32;
        if (is_fetch && setpc) break;
        if (enc_of(c[0]) == G_SOP1 && ((c[0] >> 8) & 0xFF) == 33) { /* s_swappc_b64: the fetch shader */
            if (!fetch || is_fetch) {
                ok = fail(em, pos, c[0], is_fetch, fetch ? "call in a fetch shader" : "call without a fetch shader");
                break;
            }
            if (!(ok = emit_program(em, fetch, fetch_words, true, NULL, 0))) break;
            pos += n;
            continue;
        }
        if (em->emit && is_gs_emit(c[0])) {
            if (em->n + em->emit_words > em->cap) {
                ok = fail(em, pos, c[0], is_fetch, "output full");
                break;
            }
            memcpy(em->out + em->n, em->emit, em->emit_words * 4);
            em->n += em->emit_words;
            pos += n;
            continue;
        }
        out_inst o;
        if (is_branch(c[0])) {
            o.n = 1;
            o.w[0] = c[0] & 0xFFFF0000u;
            fix[nfix][0] = em->n;
            fix[nfix][1] = (uint32_t)((int32_t)pos + 1 + (int16_t)(c[0] & 0xFFFF));
            if (fix[nfix][1] < avail && fix[nfix][1] > far) far = fix[nfix][1];
            nfix++;
        } else {
            const char *why = conv_one(c, n, &o);
            if (why) {
                ok = fail(em, pos, c[0], is_fetch, why);
                break;
            }
        }
        if (em->n + o.n > em->cap) {
            ok = fail(em, pos, c[0], is_fetch, "output full");
            break;
        }
        memcpy(em->out + em->n, o.w, o.n * 4);
        em->n += o.n;
        pos += n;
        if (!is_fetch && c[0] == 0xBF810000u && pos > far) break; /* s_endpgm no branch jumps past */
    }
    for (uint32_t i = 0; ok && i < nfix; i++) {
        uint32_t target = fix[i][1];
        if (target > pos || map[target] == 0xFFFFFFFFu) {
            ok = fail(em, 0, em->out[fix[i][0]], is_fetch, "branch target");
            break;
        }
        int32_t rel = (int32_t)(start + map[target]) - (int32_t)(fix[i][0] + 1);
        em->out[fix[i][0]] |= (uint32_t)rel & 0xFFFF;
    }
    free(map);
    free(fix);
    return ok;
}

uint32_t gcn_to_rdna2(const uint32_t *code, uint32_t avail, const uint32_t *fetch, uint32_t fetch_words,
                      uint32_t *out, uint32_t cap, gcn_error *err) {
    emitter em = {out, cap, 0, err, NULL, 0};
    return emit_program(&em, code, avail, false, fetch, fetch_words) ? em.n : 0;
}

uint32_t gcn_to_rdna2_gs(const uint32_t *code, uint32_t avail, const uint32_t *fetch, uint32_t fetch_words,
                         const uint32_t *emit, uint32_t emit_words, uint32_t *out, uint32_t cap, gcn_error *err) {
    emitter em = {out, cap, 0, err, emit, emit_words};
    return emit_program(&em, code, avail, false, fetch, fetch_words) ? em.n : 0;
}

/* GFX7 image data / number format as the GFX10 unified format; 0 if it has none */
static uint32_t image_format(uint32_t dfmt, uint32_t nfmt) {
    if (nfmt == 9) { /* SRGB */
        switch (dfmt) {
        case 1: return 128;  /* 8 */
        case 3: return 129;  /* 8_8 */
        case 10: return 130; /* 8_8_8_8 */
        default: break;
        }
    }
    if (dfmt >= 35 && dfmt <= 41) { /* BC1..BC7: unorm / srgb (BC4, BC5: snorm; BC6: signed float) */
        static const uint8_t first[7] = {169, 171, 173, 175, 177, 179, 181};
        bool alt = dfmt == 38 || dfmt == 39 || dfmt == 40 ? nfmt == 1 || nfmt == 5 : nfmt == 9;
        return first[dfmt - 35] + (alt ? 1u : 0u);
    }
    switch (dfmt) { /* packed 16-bit and 8-bit formats, unorm */
    case 16: return 133; /* 5_6_5 */
    case 17: return 134; /* 1_5_5_5 */
    case 18: return 135; /* 5_5_5_1 */
    case 19: return 136; /* 4_4_4_4 */
    case 0x39: return 137; /* 4_4 */
    default: break;
    }
    return buffer_format(dfmt, nfmt);
}

void gcn_buffer_desc_to_rdna2(uint32_t v[4]) {
    uint32_t w3 = v[3], stride = (v[1] >> 16) & 0x3FFF;
    uint32_t fmt = buffer_format((w3 >> 15) & 0xF, (w3 >> 12) & 7);
    /* OOB_SELECT: structured buffers check the index, raw ones the whole access */
    uint32_t oob = stride ? 0u : 3u;
    v[3] = (w3 & 0xFFFu) | (fmt << 12) | (w3 & (0xFu << 21)) | (1u << 24) | (oob << 28) | (w3 & 0xC0000000u);
}

void gcn_image_desc_to_rdna2(uint32_t t[8]) {
    /* BASE_ADDRESS is 38 bits (word 0 and word 1 [5:0]); [7:6] are MTYPE_L2 cache bits */
    uint32_t base_hi = t[1] & 0x3F, min_lod = (t[1] >> 8) & 0xFFF;
    uint32_t fmt = image_format((t[1] >> 20) & 0x3F, (t[1] >> 26) & 0xF);
    uint32_t w1 = t[2] & 0x3FFF, h1 = (t[2] >> 14) & 0x3FFF;
    uint32_t type = t[3] >> 28, levels = t[3] & 0xFFFFFu; /* dst_sel, BASE_LEVEL, LAST_LEVEL */
    uint32_t depth = t[4] & 0x1FFF, base_array = t[5] & 0x1FFF, last_array = (t[5] >> 13) & 0x1FFF;
    uint32_t tiling = (t[3] >> 20) & 0x1F, pow2 = (t[3] >> 25) & 1, pitch = (t[4] >> 13) & 0x3FFF;
    t[1] = base_hi | (min_lod << 8) | (fmt << 20) | ((w1 & 3) << 30);
    t[2] = (w1 >> 2) | (h1 << 14) | (1u << 31);
    /* SW_MODE: linear; the GCN tiling index (kept in word 7) is not detiled yet */
    t[3] = levels | (type << 28);
    t[4] = (type == 10 ? depth : last_array) | (base_array << 16);
    t[5] = ((t[3] >> 16) & 0xF) << 4; /* MAX_MIP */
    t[6] = 0;
    t[7] = tiling | (pow2 << 5) | (pitch << 6); /* GCN tiling index, POW2_PAD, PITCH - 1 */
}
