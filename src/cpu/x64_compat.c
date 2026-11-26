/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu/x64_compat.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#endif
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#include "base/log.h"
#include "core/settings.h"

/* Generated helpers use RCX/RDX and Win64 shadow space on every platform. */
#if defined(__GNUC__) && defined(__x86_64__)
#define COMPAT_ABI __attribute__((ms_abi))
#else
#define COMPAT_ABI
#endif

/* ---------------------------------------------------------------------------
 * Decoder
 * ------------------------------------------------------------------------- */

enum { F_M = 1, F_I8 = 2, F_IZ = 4, F_I16 = 8, F_BAD = 16, F_I32 = 32, F_MOFFS = 64, F_I64 = 128 };

static uint8_t g_one[256], g_two[256];
static bool g_tables;

static void set_range(uint8_t *t, int lo, int hi, uint8_t f) {
    for (int i = lo; i <= hi; i++) t[i] = f;
}

static void build_tables(void) {
    uint8_t *o = g_one;
    for (int row = 0; row < 8; row++) { /* 00-3F: ALU groups */
        int b = row * 8;
        set_range(o, b, b + 3, F_M);
        o[b + 4] = F_I8;
        o[b + 5] = F_IZ;
        o[b + 6] = o[b + 7] = F_BAD; /* push/pop seg, daa...; 26/2E/36/3E are prefixes (handled first) */
    }
    o[0x0F] = 0; /* escape (handled first) */
    set_range(o, 0x40, 0x5F, 0);
    o[0x60] = o[0x61] = o[0x62] = F_BAD;
    o[0x63] = F_M;
    o[0x68] = F_IZ;
    o[0x69] = F_M | F_IZ;
    o[0x6A] = F_I8;
    o[0x6B] = F_M | F_I8;
    set_range(o, 0x6C, 0x6F, 0);
    set_range(o, 0x70, 0x7F, F_I8);
    o[0x80] = F_M | F_I8;
    o[0x81] = F_M | F_IZ;
    o[0x82] = F_BAD;
    o[0x83] = F_M | F_I8;
    set_range(o, 0x84, 0x8F, F_M);
    set_range(o, 0x90, 0x9F, 0);
    o[0x9A] = F_BAD;
    set_range(o, 0xA0, 0xA3, F_MOFFS);
    set_range(o, 0xA4, 0xAF, 0);
    o[0xA8] = F_I8;
    o[0xA9] = F_IZ;
    set_range(o, 0xB0, 0xB7, F_I8);
    set_range(o, 0xB8, 0xBF, F_IZ | F_I64);
    o[0xC0] = o[0xC1] = F_M | F_I8;
    o[0xC2] = F_I16;
    o[0xC3] = 0;
    o[0xC6] = F_M | F_I8;
    o[0xC7] = F_M | F_IZ;
    o[0xC8] = F_I16 | F_I8;
    o[0xC9] = 0;
    o[0xCA] = F_I16;
    o[0xCB] = o[0xCC] = 0;
    o[0xCD] = F_I8;
    o[0xCE] = F_BAD;
    o[0xCF] = 0;
    set_range(o, 0xD0, 0xD3, F_M);
    o[0xD4] = o[0xD5] = o[0xD6] = F_BAD;
    o[0xD7] = 0;
    set_range(o, 0xD8, 0xDF, F_M);
    set_range(o, 0xE0, 0xE7, F_I8);
    o[0xE8] = o[0xE9] = F_I32;
    o[0xEA] = F_BAD;
    o[0xEB] = F_I8;
    set_range(o, 0xEC, 0xEF, 0);
    o[0xF1] = o[0xF4] = o[0xF5] = 0;
    o[0xF6] = o[0xF7] = F_M; /* + immediate for /0 and /1 */
    set_range(o, 0xF8, 0xFD, 0);
    o[0xFE] = o[0xFF] = F_M;

    uint8_t *t = g_two;
    set_range(t, 0x00, 0xFF, F_M);
    o = t;
    o[0x04] = o[0x0A] = o[0x0C] = F_BAD;
    set_range(o, 0x05, 0x09, 0);
    o[0x0B] = o[0x0E] = 0;
    o[0x0F] = F_M | F_I8;
    set_range(o, 0x24, 0x27, F_BAD);
    set_range(o, 0x30, 0x37, 0);
    o[0x39] = F_BAD;
    set_range(o, 0x3B, 0x3F, F_BAD);
    set_range(o, 0x70, 0x73, F_M | F_I8);
    o[0x77] = 0;
    o[0x7A] = o[0x7B] = F_BAD;
    set_range(o, 0x80, 0x8F, F_I32);
    o[0xA0] = o[0xA1] = o[0xA2] = 0;
    o[0xA4] = o[0xAC] = o[0xBA] = F_M | F_I8;
    o[0xA6] = o[0xA7] = F_BAD;
    o[0xA8] = o[0xA9] = o[0xAA] = 0;
    o[0xC2] = o[0xC4] = o[0xC5] = o[0xC6] = F_M | F_I8;
    set_range(o, 0xC8, 0xCF, 0);
    g_tables = true;
}

/* ModRM / SIB / displacement at p[i]; returns the new index or 0 on overflow */
static size_t decode_modrm(const uint8_t *p, size_t avail, size_t i, x64_insn *in, uint8_t rex_r, uint8_t rex_x,
                           uint8_t rex_b) {
    if (i >= avail) return 0;
    uint8_t m = p[i++];
    in->has_modrm = true;
    in->mod = m >> 6;
    in->reg = (uint8_t)(((m >> 3) & 7) | (rex_r ? 8 : 0));
    uint8_t rm = m & 7;
    in->index = 0xFF;
    if (in->mod == 3) {
        in->rm = (uint8_t)(rm | (rex_b ? 8 : 0));
        return i;
    }
    if (rm == 4) {
        if (i >= avail) return 0;
        uint8_t s = p[i++];
        in->has_sib = true;
        in->scale = s >> 6;
        uint8_t idx = (uint8_t)(((s >> 3) & 7) | (rex_x ? 8 : 0));
        in->index = idx == 4 ? 0xFF : idx;
        uint8_t base = s & 7;
        if (base == 5 && in->mod == 0) {
            in->no_base = true;
            if (i + 4 > avail) return 0;
            memcpy(&in->disp, p + i, 4);
            return i + 4;
        }
        in->base = (uint8_t)(base | (rex_b ? 8 : 0));
    } else if (rm == 5 && in->mod == 0) {
        in->rip_rel = true;
        in->disp_off = (uint8_t)i;
        if (i + 4 > avail) return 0;
        memcpy(&in->disp, p + i, 4);
        return i + 4;
    } else {
        in->base = (uint8_t)(rm | (rex_b ? 8 : 0));
    }
    if (in->mod == 1) {
        if (i + 1 > avail) return 0;
        in->disp = (int8_t)p[i];
        return i + 1;
    }
    if (in->mod == 2) {
        if (i + 4 > avail) return 0;
        memcpy(&in->disp, p + i, 4);
        return i + 4;
    }
    return i;
}

static bool take_imm(const uint8_t *p, size_t avail, size_t *i, uint32_t n, x64_insn *in) {
    if (*i + n > avail) return false;
    uint64_t v = 0;
    memcpy(&v, p + *i, n);
    if (!in->imm_len) in->imm = v; /* the first immediate (ENTER's second is not needed) */
    in->imm_len = (uint8_t)(in->imm_len + n);
    *i += n;
    return true;
}

bool x64_decode(const uint8_t *p, size_t avail, x64_insn *in) {
    if (!g_tables) build_tables();
    memset(in, 0, sizeof(*in));
    size_t i = 0;
    for (;; i++) { /* legacy prefixes */
        if (i >= avail || i >= 14) return false;
        uint8_t b = p[i];
        if (b == 0x66) in->p66 = true;
        else if (b == 0x67) in->p67 = true;
        else if (b == 0xF0) in->lock = true;
        else if (b == 0xF2 || b == 0xF3) in->rep = b;
        else if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65) in->seg = b;
        else break;
    }
    if ((p[i] & 0xF0) == 0x40) {
        in->rex = p[i++];
        if (i >= avail) return false;
    }
    uint8_t rex_w = in->rex & 8, rex_r = in->rex & 4, rex_x = in->rex & 2, rex_b = in->rex & 1;
    uint8_t b = p[i];
    if (b == 0xC4 || b == 0xC5) { /* VEX (always in 64-bit mode) */
        if (in->rex || in->p66 || in->rep || in->lock) return false;
        in->vex = true;
        uint8_t r, x = 1, bb = 1, w = 0, map, vl;
        if (b == 0xC5) {
            if (i + 2 > avail) return false;
            uint8_t v = p[i + 1];
            r = v >> 7;
            vl = v;
            map = 1;
            i += 2;
        } else {
            if (i + 3 > avail) return false;
            uint8_t v1 = p[i + 1], v2 = p[i + 2];
            r = v1 >> 7;
            x = (v1 >> 6) & 1;
            bb = (v1 >> 5) & 1;
            map = v1 & 0x1F;
            w = v2 >> 7;
            vl = v2;
            i += 3;
        }
        if (map < 1 || map > 3) return false;
        in->vvvv = (uint8_t)((~vl >> 3) & 15);
        in->vex_l = (vl >> 2) & 1;
        in->pp = vl & 3;
        in->vex_w = w;
        in->map = map;
        if (i >= avail) return false;
        in->op = p[i++];
        in->rex = (uint8_t)(0x40 | (w ? 8 : 0) | (r ? 0 : 4) | (x ? 0 : 2) | (bb ? 0 : 1));
        if (!(map == 1 && in->op == 0x77)) {
            i = decode_modrm(p, avail, i, in, !r, !x, !bb);
            if (!i) return false;
        }
        bool imm = map == 3 || (map == 1 && ((in->op >= 0x70 && in->op <= 0x73) || in->op == 0xC2 ||
                                             (in->op >= 0xC4 && in->op <= 0xC6)));
        if (imm && !take_imm(p, avail, &i, 1, in)) return false;
        in->len = (uint8_t)i;
        return i <= 15;
    }
    if (b == 0x62 || b == 0x8F) { /* EVEX / XOP: no Zen 2 code uses them */
        if (b == 0x62) return false;
    }
    uint8_t flags;
    if (b == 0x0F) {
        if (++i >= avail) return false;
        uint8_t b2 = p[i++];
        if (b2 == 0x38 || b2 == 0x3A) {
            if (i >= avail) return false;
            in->map = b2 == 0x38 ? 2 : 3;
            in->op = p[i++];
            flags = in->map == 3 ? (F_M | F_I8) : F_M;
        } else {
            in->map = 1;
            in->op = b2;
            flags = g_two[b2];
            /* SSE4a EXTRQ / INSERTQ immediate forms */
            if (b2 == 0x78 && (in->p66 || in->rep == 0xF2)) flags = F_M | F_I16;
        }
    } else {
        in->op = b;
        i++;
        flags = g_one[b];
    }
    if (flags & F_BAD) return false;
    if (flags & F_M) {
        i = decode_modrm(p, avail, i, in, rex_r, rex_x, rex_b);
        if (!i) return false;
    }
    if (in->map == 0 && (b == 0xF6 || b == 0xF7) && (in->reg & 7) <= 1) flags |= b == 0xF6 ? F_I8 : F_IZ;
    if (flags & F_MOFFS) {
        if (!take_imm(p, avail, &i, in->p67 ? 4 : 8, in)) return false;
    }
    if (flags & F_I16 && !take_imm(p, avail, &i, 2, in)) return false;
    if (flags & F_I32 && !take_imm(p, avail, &i, 4, in)) return false;
    if (flags & F_IZ) {
        uint32_t n = (flags & F_I64) && rex_w ? 8 : (in->p66 && !rex_w) ? 2 : 4;
        if (!take_imm(p, avail, &i, n, in)) return false;
    }
    if (flags & F_I8 && !take_imm(p, avail, &i, 1, in)) return false;
    in->len = (uint8_t)i;
    return i <= 15;
}

/* ---------------------------------------------------------------------------
 * What we emulate
 * ------------------------------------------------------------------------- */

static bool is_fma(const x64_insn *in) {
    if (!in->vex || in->map != 2 || in->pp != 1) return false;
    uint8_t lo = in->op & 15, hi = in->op >> 4;
    return (hi == 9 || hi == 0xA || hi == 0xB) && lo >= 6;
}

/* AVX2: integer operations on 256 bits, and the new forms (broadcasts,
 * permutes, variable shifts, dword blends, masked moves). Most of the 256-bit
 * integer operations work on each 128-bit half alike: the AVX (128-bit) form
 * of the same instruction runs twice (AVX2_LANE); the others run in C. */
enum { AVX2_NONE, AVX2_LANE, AVX2_C };

static int avx2_kind(const x64_insn *in) {
    if (!in->vex) return AVX2_NONE;
    uint8_t op = in->op;
    if (in->map == 1) {
        if (!in->vex_l) return AVX2_NONE;
        if (in->pp == 1) {
            if ((op >= 0x60 && op <= 0x6D) || (op >= 0x70 && op <= 0x76) || (op >= 0xD1 && op <= 0xD5) ||
                (op >= 0xD8 && op <= 0xDF) || (op >= 0xE0 && op <= 0xE5) || (op >= 0xE8 && op <= 0xEF) ||
                (op >= 0xF1 && op <= 0xF6) || (op >= 0xF8 && op <= 0xFE))
                return AVX2_LANE;
            if (op == 0xD7 && in->mod == 3 && in->reg != 4) return AVX2_C; /* VPMOVMSKB r32, ymm */
            return AVX2_NONE;
        }
        return (in->pp == 2 || in->pp == 3) && op == 0x70 ? AVX2_LANE : AVX2_NONE; /* VPSHUFHW / VPSHUFLW */
    }
    if (in->pp != 1) return AVX2_NONE;
    if (in->map == 2) {
        switch (op) {
        case 0x58: case 0x59: case 0x78: case 0x79: return AVX2_C;        /* VPBROADCASTD/Q/B/W */
        case 0x18: case 0x19: return in->mod == 3 ? AVX2_C : AVX2_NONE;   /* VBROADCASTSS/SD from a register */
        case 0x5A: return in->vex_l && in->mod != 3 ? AVX2_C : AVX2_NONE; /* VBROADCASTI128 */
        case 0x16: case 0x36: return in->vex_l ? AVX2_C : AVX2_NONE;      /* VPERMPS / VPERMD */
        case 0x45: case 0x46: case 0x47: return AVX2_C;                   /* VPSRLV / VPSRAV / VPSLLV */
        case 0x8C: case 0x8E: return in->mod != 3 ? AVX2_C : AVX2_NONE;   /* VPMASKMOV */
        case 0x90: case 0x91: case 0x92: case 0x93:                       /* gathers (VSIB) */
            return in->mod != 3 && in->has_sib ? AVX2_C : AVX2_NONE;
        }
        if (!in->vex_l) return AVX2_NONE;
        if (op <= 0x0B || (op >= 0x1C && op <= 0x1E) || op == 0x28 || op == 0x29 || op == 0x2B || op == 0x37 ||
            (op >= 0x38 && op <= 0x40))
            return AVX2_LANE;
        if (op == 0x2A && in->mod != 3) return AVX2_LANE; /* VMOVNTDQA */
        if ((op >= 0x20 && op <= 0x25) || (op >= 0x30 && op <= 0x35)) return AVX2_C; /* VPMOVSX / VPMOVZX */
        return AVX2_NONE;
    }
    if (in->map == 3) {
        switch (op) {
        case 0x00: case 0x01: return in->vex_l && in->vex_w ? AVX2_C : AVX2_NONE; /* VPERMQ / VPERMPD */
        case 0x02: return AVX2_C;                                                  /* VPBLENDD */
        case 0x38: case 0x39: case 0x46: return in->vex_l ? AVX2_C : AVX2_NONE;    /* VINSERTI128 / VEXTRACTI128 / VPERM2I128 */
        case 0x0E: case 0x0F: case 0x42: case 0x4C: return in->vex_l ? AVX2_LANE : AVX2_NONE;
        }
    }
    return AVX2_NONE;
}

/* lane forms: which operands they use */
static bool lane_ndd(const x64_insn *in) { return in->map == 1 && in->op >= 0x71 && in->op <= 0x73; } /* dest = vvvv */
static bool lane_no_vvvv(const x64_insn *in) {
    return (in->map == 1 && in->op == 0x70) || (in->map == 2 && ((in->op >= 0x1C && in->op <= 0x1E) || in->op == 0x2A));
}
static bool lane_count(const x64_insn *in) { /* shift by an xmm count: the same count for both halves */
    uint8_t op = in->op;
    return in->map == 1 && (op == 0xD1 || op == 0xD2 || op == 0xD3 || op == 0xE1 || op == 0xE2 || op == 0xF1 ||
                            op == 0xF2 || op == 0xF3);
}

uint32_t x64_compat_set(const x64_insn *in) {
    if (!in->vex && in->map == 1 &&
        ((in->op == 0x31 && in->len == 2) ||
         (in->op == 0x01 && in->len == 3 && in->mod == 3 && in->reg == 7 && in->rm == 1))) {
        return X64_SET_TSC;
    }
    if (in->seg == 0x64 || in->seg == 0x65) return 0; /* thread-pointer forms: not seen, not handled */
    if (in->vex) {
        if (avx2_kind(in)) return X64_SET_AVX2;
        if (is_fma(in)) return X64_SET_FMA;
        if (in->vex_l) return 0;
        bool dst_rsp = in->reg == 4;
        if (in->map == 2) {
            switch (in->op) {
            case 0xF2: /* ANDN */
                return in->pp == 0 && !dst_rsp ? X64_SET_BMI1 : 0;
            case 0xF3: /* BLSR /1, BLSMSK /2, BLSI /3: the destination is vvvv */
                return in->pp == 0 && (in->reg & 7) >= 1 && (in->reg & 7) <= 3 && in->vvvv != 4 ? X64_SET_BMI1 : 0;
            case 0xF5: /* BZHI, PEXT, PDEP */
                return in->pp != 1 && !dst_rsp ? X64_SET_BMI2 : 0;
            case 0xF6: /* MULX */
                return in->pp == 3 && !dst_rsp && in->vvvv != 4 ? X64_SET_BMI2 : 0;
            case 0xF7: /* BEXTR (BMI1), SHLX / SARX / SHRX */
                if (dst_rsp) return 0;
                return in->pp == 0 ? X64_SET_BMI1 : X64_SET_BMI2;
            }
        } else if (in->map == 3 && in->op == 0xF0 && in->pp == 3 && !dst_rsp) {
            return X64_SET_BMI2; /* RORX */
        }
        return 0;
    }
    if (in->lock) return 0;
    if (in->map == 1 && in->rep == 0xF3 && (in->op == 0xBC || in->op == 0xBD) && in->reg != 4)
        return in->op == 0xBD ? X64_SET_LZCNT : X64_SET_BMI1;
    if (in->map == 2 && (in->op == 0xF0 || in->op == 0xF1) && !in->rep && in->mod != 3 &&
        !(in->op == 0xF0 && in->reg == 4))
        return X64_SET_MOVBE;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Emulation
 * ------------------------------------------------------------------------- */

enum { FL_CF = 1u << 0, FL_PF = 1u << 2, FL_AF = 1u << 4, FL_ZF = 1u << 6, FL_SF = 1u << 7, FL_OF = 1u << 11 };

typedef struct cpu_state {
    uint64_t *gpr;
    uint64_t *rflags;
    uint8_t (*ymm)[32]; /* NULL: only the low 128 bits (xmm) are known */
} cpu_state;

__extension__ typedef unsigned __int128 u128;

static uint64_t mask_of(int bits) { return bits == 64 ? ~0ull : (1ull << bits) - 1; }

static void write_gpr(uint64_t *r, uint64_t v, int bits) {
    if (bits == 64) *r = v;
    else if (bits == 32) *r = (uint32_t)v;
    else *r = (*r & ~0xFFFFull) | (v & 0xFFFF);
}

static uint64_t ea(const x64_insn *in, uint64_t pc, const uint64_t *gpr) {
    uint64_t a;
    if (in->rip_rel) {
        a = pc + in->len + (uint64_t)(int64_t)in->disp;
    } else {
        a = in->no_base ? 0 : gpr[in->base];
        if (in->index != 0xFF) a += gpr[in->index] << in->scale;
        a += (uint64_t)(int64_t)in->disp;
    }
    return in->p67 ? (uint32_t)a : a;
}

static uint64_t read_rm(const x64_insn *in, uint64_t pc, const uint64_t *gpr, int bits) {
    if (in->mod == 3) return gpr[in->rm] & mask_of(bits);
    uint64_t v = 0;
    memcpy(&v, (const void *)(uintptr_t)ea(in, pc, gpr), (size_t)(bits / 8));
    return v;
}

static void set_flags(uint64_t *rf, uint32_t clear, uint32_t set) { *rf = (*rf & ~(uint64_t)clear) | set; }

static uint32_t zf_sf(uint64_t v, int bits) {
    uint32_t f = 0;
    if (!(v & mask_of(bits))) f |= FL_ZF;
    if ((v >> (bits - 1)) & 1) f |= FL_SF;
    return f;
}

static uint64_t pdep(uint64_t src, uint64_t mask) {
    uint64_t r = 0;
    for (uint64_t bit = 1; mask; bit <<= 1) {
        uint64_t low = mask & -mask;
        if (src & bit) r |= low;
        mask &= mask - 1;
    }
    return r;
}

static uint64_t pext(uint64_t src, uint64_t mask) {
    uint64_t r = 0;
    for (uint64_t bit = 1; mask; bit <<= 1) {
        uint64_t low = mask & -mask;
        if (src & low) r |= bit;
        mask &= mask - 1;
    }
    return r;
}

static bool exec_gpr(const x64_insn *in, uint64_t pc, cpu_state *s) {
    uint64_t *g = s->gpr;
    if (!in->vex && in->map == 1) { /* TZCNT / LZCNT */
        int bits = (in->rex & 8) ? 64 : in->p66 ? 16 : 32;
        uint64_t v = read_rm(in, pc, g, bits), n;
        if (in->op == 0xBC) n = v ? (uint64_t)__builtin_ctzll(v) : (uint64_t)bits;
        else n = v ? (uint64_t)(__builtin_clzll(v) - (64 - bits)) : (uint64_t)bits;
        write_gpr(&g[in->reg], n, bits);
        set_flags(s->rflags, FL_CF | FL_ZF, (v ? 0 : FL_CF) | (n ? 0 : FL_ZF));
        return true;
    }
    if (!in->vex && in->map == 2) { /* MOVBE */
        int bits = (in->rex & 8) ? 64 : in->p66 ? 16 : 32;
        void *m = (void *)(uintptr_t)ea(in, pc, g);
        if (in->op == 0xF0) {
            uint64_t v = 0;
            memcpy(&v, m, (size_t)(bits / 8));
            v = bits == 64 ? __builtin_bswap64(v) : bits == 32 ? __builtin_bswap32((uint32_t)v)
                                                                : __builtin_bswap16((uint16_t)v);
            write_gpr(&g[in->reg], v, bits);
        } else {
            uint64_t v = g[in->reg];
            v = bits == 64 ? __builtin_bswap64(v) : bits == 32 ? __builtin_bswap32((uint32_t)v)
                                                                : __builtin_bswap16((uint16_t)v);
            memcpy(m, &v, (size_t)(bits / 8));
        }
        return true;
    }
    int bits = in->vex_w ? 64 : 32;
    uint64_t mask = mask_of(bits);
    if (in->map == 3) { /* RORX */
        uint64_t v = read_rm(in, pc, g, bits);
        unsigned c = (unsigned)(in->imm & (unsigned)(bits - 1));
        uint64_t r = c ? ((v >> c) | (v << (bits - c))) & mask : v;
        write_gpr(&g[in->reg], r, bits);
        return true;
    }
    switch (in->op) {
    case 0xF2: { /* ANDN reg = ~vvvv & rm */
        uint64_t r = ~g[in->vvvv] & read_rm(in, pc, g, bits) & mask;
        write_gpr(&g[in->reg], r, bits);
        set_flags(s->rflags, FL_ZF | FL_SF | FL_CF | FL_OF, zf_sf(r, bits));
        return true;
    }
    case 0xF3: { /* BLSR / BLSMSK / BLSI vvvv = f(rm) */
        uint64_t v = read_rm(in, pc, g, bits), r;
        uint32_t f;
        switch (in->reg & 7) {
        case 1: r = (v - 1) & v & mask; f = zf_sf(r, bits) | (v ? 0 : FL_CF); break;
        case 2: r = ((v - 1) ^ v) & mask; f = (zf_sf(r, bits) & ~FL_ZF) | (v ? 0 : FL_CF); break;
        default: r = (0 - v) & v & mask; f = zf_sf(r, bits) | (v ? FL_CF : 0); break;
        }
        write_gpr(&g[in->vvvv], r, bits);
        set_flags(s->rflags, FL_ZF | FL_SF | FL_CF | FL_OF, f);
        return true;
    }
    case 0xF5: {
        uint64_t a = read_rm(in, pc, g, bits);
        if (in->pp == 0) { /* BZHI reg = rm with bits from vvvv[7:0] up cleared */
            unsigned n = (unsigned)(g[in->vvvv] & 0xFF);
            uint64_t r = n < (unsigned)bits ? a & ((1ull << n) - 1) : a;
            write_gpr(&g[in->reg], r, bits);
            set_flags(s->rflags, FL_ZF | FL_SF | FL_CF | FL_OF, zf_sf(r, bits) | (n > (unsigned)bits - 1 ? FL_CF : 0));
        } else { /* PEXT (F3) / PDEP (F2): reg = f(vvvv, mask rm) */
            uint64_t src = g[in->vvvv] & mask;
            write_gpr(&g[in->reg], in->pp == 2 ? pext(src, a) : pdep(src, a), bits);
        }
        return true;
    }
    case 0xF6: { /* MULX reg:vvvv = rdx * rm (high half last: it wins when both are the same) */
        uint64_t a = read_rm(in, pc, g, bits);
        if (bits == 64) {
            u128 p = (u128)g[2] * a;
            write_gpr(&g[in->vvvv], (uint64_t)p, 64);
            write_gpr(&g[in->reg], (uint64_t)(p >> 64), 64);
        } else {
            uint64_t p = (uint64_t)(uint32_t)g[2] * (uint32_t)a;
            write_gpr(&g[in->vvvv], (uint32_t)p, 32);
            write_gpr(&g[in->reg], p >> 32, 32);
        }
        return true;
    }
    case 0xF7: {
        uint64_t a = read_rm(in, pc, g, bits), c = g[in->vvvv];
        uint64_t r;
        if (in->pp == 0) { /* BEXTR */
            unsigned start = (unsigned)(c & 0xFF), len = (unsigned)((c >> 8) & 0xFF);
            r = start < (unsigned)bits ? a >> start : 0;
            if (len < 64) r &= (1ull << len) - 1;
            r &= mask;
            write_gpr(&g[in->reg], r, bits);
            set_flags(s->rflags, FL_ZF | FL_CF | FL_OF, r ? 0 : FL_ZF);
            return true;
        }
        unsigned n = (unsigned)(c & (unsigned)(bits - 1));
        if (in->pp == 1) r = (a << n) & mask; /* SHLX */
        else if (in->pp == 3) r = (a & mask) >> n; /* SHRX */
        else r = (uint64_t)((bits == 64 ? (int64_t)a : (int64_t)(int32_t)a) >> n) & mask; /* SARX */
        write_gpr(&g[in->reg], r, bits);
        return true;
    }
    }
    return false;
}

/* vector register access: ymm when known, else the xmm part only */
static uint8_t *vreg(cpu_state *s, unsigned r) { return s->ymm[r & 15]; }

static void exec_fma(const x64_insn *in, uint64_t pc, cpu_state *s) {
    unsigned lo = in->op & 15, order = in->op >> 4; /* 9: 132, A: 213, B: 231 */
    bool dbl = in->vex_w, scalar = (lo & 1) && lo >= 9;
    unsigned esize = dbl ? 8 : 4, vbytes = in->vex_l ? 32 : 16;
    unsigned n = scalar ? 1 : vbytes / esize;
    uint8_t a[32], b[32], c[32], r[32];
    memcpy(a, vreg(s, in->reg), 32);
    memcpy(b, vreg(s, in->vvvv), 32);
    if (in->mod == 3) memcpy(c, vreg(s, in->rm), 32);
    else memcpy(c, (const void *)(uintptr_t)ea(in, pc, s->gpr), scalar ? esize : vbytes);
    memcpy(r, a, 32);
    for (unsigned i = 0; i < n; i++) {
        /* x*y op z for each operand order */
        const uint8_t *x = order == 9 ? a : b, *y = order == 0xA ? a : c, *z = order == 9 ? b : order == 0xA ? c : a;
        bool neg_mul = lo >= 0xC, sub;
        if (lo == 6) sub = (i & 1) == 0;       /* fmaddsub: even lanes subtract */
        else if (lo == 7) sub = (i & 1) != 0;  /* fmsubadd: odd lanes subtract */
        else sub = lo == 0xA || lo == 0xB || lo == 0xE || lo == 0xF;
        if (dbl) {
            double xv, yv, zv;
            memcpy(&xv, x + i * 8, 8);
            memcpy(&yv, y + i * 8, 8);
            memcpy(&zv, z + i * 8, 8);
            double v = fma(neg_mul ? -xv : xv, yv, sub ? -zv : zv);
            memcpy(r + i * 8, &v, 8);
        } else {
            float xv, yv, zv;
            memcpy(&xv, x + i * 4, 4);
            memcpy(&yv, y + i * 4, 4);
            memcpy(&zv, z + i * 4, 4);
            float v = fmaf(neg_mul ? -xv : xv, yv, sub ? -zv : zv);
            memcpy(r + i * 4, &v, 4);
        }
    }
    if (!in->vex_l) memset(r + 16, 0, 16); /* VEX.128 clears the upper half */
    memcpy(vreg(s, in->reg), r, 32);
}

/* the r/m operand: a register's bytes or `bytes` from memory, into a 32-byte buffer */
static void read_vrm(const x64_insn *in, uint64_t pc, cpu_state *s, uint8_t *out, unsigned bytes) {
    memset(out, 0, 32);
    if (in->mod == 3) memcpy(out, s->ymm[in->rm], 32);
    else memcpy(out, (const void *)(uintptr_t)ea(in, pc, s->gpr), bytes);
}

static void exec_avx2_c(const x64_insn *in, uint64_t pc, cpu_state *s) {
    uint8_t src[32], r[32];
    unsigned vbytes = in->vex_l ? 32 : 16;
    uint8_t op = in->op;
    memset(r, 0, 32);
    if (in->map == 1) { /* VPMOVMSKB r32, ymm */
        const uint8_t *v = s->ymm[in->rm];
        uint32_t m = 0;
        for (int i = 0; i < 32; i++) m |= (uint32_t)(v[i] >> 7) << i;
        s->gpr[in->reg] = m;
        return;
    }
    if (in->map == 2) {
        switch (op) {
        case 0x58: case 0x59: case 0x78: case 0x79: case 0x18: case 0x19: { /* broadcasts */
            unsigned es = op == 0x78 ? 1 : op == 0x79 ? 2 : (op == 0x58 || op == 0x18) ? 4 : 8;
            read_vrm(in, pc, s, src, es);
            for (unsigned i = 0; i < vbytes; i += es) memcpy(r + i, src, es);
            break;
        }
        case 0x5A: /* VBROADCASTI128 */
            read_vrm(in, pc, s, src, 16);
            memcpy(r, src, 16);
            memcpy(r + 16, src, 16);
            break;
        case 0x16: case 0x36: { /* VPERMPS / VPERMD: dwords of r/m picked by the indexes in vvvv */
            read_vrm(in, pc, s, src, 32);
            const uint8_t *idx = s->ymm[in->vvvv];
            for (int i = 0; i < 8; i++) memcpy(r + i * 4, src + (idx[i * 4] & 7) * 4, 4);
            break;
        }
        case 0x45: case 0x46: case 0x47: { /* VPSRLV / VPSRAV / VPSLLV: vvvv shifted by r/m, per element */
            bool q = in->vex_w && op != 0x46;
            read_vrm(in, pc, s, src, vbytes);
            const uint8_t *v = s->ymm[in->vvvv];
            for (unsigned i = 0; i < vbytes; i += q ? 8 : 4) {
                if (q) {
                    uint64_t x, n;
                    memcpy(&x, v + i, 8);
                    memcpy(&n, src + i, 8);
                    x = n > 63 ? 0 : op == 0x47 ? x << n : x >> n;
                    memcpy(r + i, &x, 8);
                } else {
                    uint32_t x, n;
                    memcpy(&x, v + i, 4);
                    memcpy(&n, src + i, 4);
                    if (op == 0x46) x = (uint32_t)((int32_t)x >> (n > 31 ? 31 : n));
                    else x = n > 31 ? 0 : op == 0x47 ? x << n : x >> n;
                    memcpy(r + i, &x, 4);
                }
            }
            break;
        }
        case 0x8C: case 0x8E: { /* VPMASKMOVD/Q: elements whose mask (vvvv) has the top bit set */
            unsigned es = in->vex_w ? 8 : 4;
            uint8_t *m = (uint8_t *)(uintptr_t)ea(in, pc, s->gpr);
            const uint8_t *mask = s->ymm[in->vvvv];
            for (unsigned i = 0; i < vbytes; i += es) {
                if (!(mask[i + es - 1] & 0x80)) continue;
                if (op == 0x8C) memcpy(r + i, m + i, es);
                else memcpy(m + i, s->ymm[in->reg] + i, es);
            }
            if (op == 0x8E) return;
            break;
        }
        case 0x90: case 0x91: case 0x92: case 0x93: { /* VPGATHERD/Q, VGATHERD/QPS/PD: elements whose mask (vvvv)
                                                       * has the top bit set are loaded; the mask is cleared */
            unsigned es = in->vex_w ? 8 : 4, is = (op & 1) ? 8 : 4;
            unsigned n = vbytes / (es > is ? es : is);
            const uint8_t *idx = s->ymm[in->index == 0xFF ? 4 : in->index]; /* VSIB: index 4 is xmm4 */
            uint8_t *mask = s->ymm[in->vvvv];
            uint64_t b = (in->no_base ? 0 : s->gpr[in->base]) + (uint64_t)(int64_t)in->disp;
            memcpy(r, s->ymm[in->reg], n * es);
            for (unsigned i = 0; i < n; i++) {
                if (!(mask[i * es + es - 1] & 0x80)) continue;
                int64_t x;
                if (is == 8) memcpy(&x, idx + i * 8, 8);
                else {
                    int32_t d;
                    memcpy(&d, idx + i * 4, 4);
                    x = d;
                }
                uint64_t a = b + ((uint64_t)x << in->scale);
                if (in->p67) a = (uint32_t)a;
                memcpy(r + i * es, (const void *)(uintptr_t)a, es);
            }
            memset(mask, 0, 32);
            break;
        }
        default: { /* VPMOVSX (0x20-0x25) / VPMOVZX (0x30-0x35), 256-bit */
            static const uint8_t from[6] = {1, 1, 1, 2, 2, 4}, to[6] = {2, 4, 8, 4, 8, 8};
            unsigned k = op & 0x0F, fs = from[k], ts = to[k], n = 32 / ts;
            bool sx = op < 0x30;
            read_vrm(in, pc, s, src, n * fs);
            for (unsigned i = 0; i < n; i++) {
                uint64_t v = 0;
                memcpy(&v, src + i * fs, fs);
                if (sx && (v >> (fs * 8 - 1)) & 1) v |= ~0ull << (fs * 8);
                memcpy(r + i * ts, &v, ts);
            }
            break;
        }
        }
        memcpy(s->ymm[in->reg], r, 32);
        return;
    }
    /* map 3 */
    uint8_t imm = (uint8_t)in->imm;
    switch (op) {
    case 0x00: case 0x01: /* VPERMQ / VPERMPD */
        read_vrm(in, pc, s, src, 32);
        for (int i = 0; i < 4; i++) memcpy(r + i * 8, src + ((imm >> (2 * i)) & 3) * 8, 8);
        break;
    case 0x02: { /* VPBLENDD */
        read_vrm(in, pc, s, src, vbytes);
        const uint8_t *v = s->ymm[in->vvvv];
        for (unsigned i = 0; i < vbytes / 4; i++) memcpy(r + i * 4, ((imm >> i) & 1) ? src + i * 4 : v + i * 4, 4);
        break;
    }
    case 0x38: /* VINSERTI128 */
        read_vrm(in, pc, s, src, 16);
        memcpy(r, s->ymm[in->vvvv], 32);
        memcpy(r + (imm & 1) * 16, src, 16);
        break;
    case 0x39: { /* VEXTRACTI128 r/m <- half of reg */
        const uint8_t *half = s->ymm[in->reg] + (imm & 1) * 16;
        if (in->mod == 3) {
            memcpy(r, half, 16);
            memcpy(s->ymm[in->rm], r, 32);
        } else {
            memcpy((void *)(uintptr_t)ea(in, pc, s->gpr), half, 16);
        }
        return;
    }
    default: { /* 0x46 VPERM2I128 */
        uint8_t a[32];
        memcpy(a, s->ymm[in->vvvv], 32);
        read_vrm(in, pc, s, src, 32);
        for (int h = 0; h < 2; h++) {
            unsigned sel = (imm >> (4 * h)) & 3;
            if ((imm >> (4 * h + 3)) & 1) continue;
            memcpy(r + h * 16, (sel < 2 ? a : src) + (sel & 1) * 16, 16);
        }
        break;
    }
    }
    memcpy(s->ymm[in->reg], r, 32);
}

typedef void (COMPAT_ABI *lane_kernel)(uint8_t (*ymm)[32], const void *mem);

bool x64_guest_tsc_enabled(void) {
    static int mode;
    int value = __atomic_load_n(&mode, __ATOMIC_RELAXED);
    if (!value) {
        const char *env = getenv("PS5C_GUEST_TSC");
        value = env && !strcmp(env, "1") ? 1 : -1;
        __atomic_store_n(&mode, value, __ATOMIC_RELAXED);
        if (value > 0) LOG_I(CPU, "guest TSC: %llu Hz", X64_GUEST_TSC_HZ);
    }
    return value > 0;
}

static uint64_t guest_tsc_ticks(void) {
    uint64_t ns = pal_time_ns();
    /* 1.6 ticks per nanosecond, without overflowing the multiplication. */
    return (ns / 5) * 8 + (ns % 5) * 8 / 5;
}

uint64_t x64_guest_tsc_read(void) {
    return x64_guest_tsc_enabled() ? guest_tsc_ticks() : __rdtsc();
}

static bool exec_insn(const x64_insn *in, uint64_t pc, cpu_state *s, const void *kernel) {
    uint32_t set = x64_compat_set(in);
    if (set == X64_SET_TSC) {
        if (in->op == 0x01) {
            unsigned aux = 0;
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
            unsigned a, b, c, d;
            if (__get_cpuid(0x80000001, &a, &b, &c, &d) && (d & (1u << 27)))
                (void)__rdtscp(&aux);
#else
            (void)__rdtscp(&aux);
#endif
            s->gpr[1] = aux;
        }
        uint64_t ticks = guest_tsc_ticks();
        s->gpr[0] = (uint32_t)ticks;
        s->gpr[2] = (uint32_t)(ticks >> 32);
        return true;
    }
    if (set == X64_SET_AVX2) {
        if (!s->ymm) return false;
        if (avx2_kind(in) == AVX2_C) {
            exec_avx2_c(in, pc, s);
            return true;
        }
        if (!kernel) return false;
        lane_kernel k;
        memcpy(&k, &kernel, sizeof(k));
        k(s->ymm, in->mod == 3 ? NULL : (const void *)(uintptr_t)ea(in, pc, s->gpr));
        return true;
    }
    if (set == X64_SET_FMA) {
        if (!s->ymm) return false;
        exec_fma(in, pc, s);
        return true;
    }
    if (set & (X64_SET_BMI1 | X64_SET_BMI2 | X64_SET_LZCNT | X64_SET_MOVBE)) return exec_gpr(in, pc, s);
    return false;
}

/* ---------------------------------------------------------------------------
 * Host features
 * ------------------------------------------------------------------------- */

uint32_t x64_compat_missing(void) {
    static int cached = -1;
    if (cached >= 0) return (uint32_t)cached;
    uint32_t have = 0;
#if defined(__GNUC__) && defined(__x86_64__)
    unsigned a, b, c, d;
    if (__get_cpuid(1, &a, &b, &c, &d)) {
        if (c & (1u << 12)) have |= X64_SET_FMA;
        if (c & (1u << 22)) have |= X64_SET_MOVBE;
    }
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) {
        if (b & (1u << 3)) have |= X64_SET_BMI1;
        if (b & (1u << 5)) have |= X64_SET_AVX2;
        if (b & (1u << 8)) have |= X64_SET_BMI2;
    }
    if (__get_cpuid(0x80000001, &a, &b, &c, &d) && (c & (1u << 5))) have |= X64_SET_LZCNT;
#endif
    uint32_t all = X64_SET_BMI1 | X64_SET_BMI2 | X64_SET_LZCNT | X64_SET_MOVBE | X64_SET_FMA | X64_SET_AVX2;
    uint32_t missing = all & ~have;
    /* [geral] cpu_antiga: 0 when the host needs it, 1 always (as an Ivy Bridge), 2 never;
     * PS5C_CPU_COMPAT=ivb / off wins */
    const char *e = getenv("PS5C_CPU_COMPAT");
    int64_t mode = settings_get_int("geral", "cpu_antiga", 0);
    if (e && !strcmp(e, "off")) mode = 2;
    else if (e && !strcmp(e, "ivb")) mode = 1;
    if (mode == 2) missing = 0;
    else if (mode == 1) missing = all;
    if (missing) LOG_I(CPU, "host CPU lacks some PS5 instruction sets: replacing%s%s%s%s%s%s",
                       missing & X64_SET_BMI1 ? " BMI1" : "", missing & X64_SET_BMI2 ? " BMI2" : "",
                       missing & X64_SET_LZCNT ? " LZCNT" : "", missing & X64_SET_MOVBE ? " MOVBE" : "",
                       missing & X64_SET_FMA ? " FMA3" : "", missing & X64_SET_AVX2 ? " AVX2" : "");
    cached = (int)missing;
    return missing;
}

/* ---------------------------------------------------------------------------
 * Patching
 * ------------------------------------------------------------------------- */

typedef struct compat_ctx {
    uint8_t ymm[16][32]; /* 0 */
    uint64_t gpr[16];    /* 512 */
    uint64_t rflags;     /* 640 */
    uint64_t ret;        /* 648: return address into the stub */
    uint64_t pad[2];
} compat_ctx;
PS5C_STATIC_ASSERT(sizeof(compat_ctx) == 672, "compat_ctx size");
#define CTX_YMM 0
#define CTX_GPR 512
#define CTX_FLAGS 640
#define CTX_RET 648

typedef struct compat_site {
    uint64_t pc;
    x64_insn in;
    void *kernel; /* AVX2 lane forms */
    uint8_t orig[15];
} compat_site;

#define MAX_AREAS 512

typedef struct ret_entry {
    uint64_t ret; /* return address of a call to the common code */
    const compat_site *site;
} ret_entry;

typedef struct compat_area {
    uint64_t lo, hi; /* the stub code */
    uint32_t nrets, ntraps;
    ret_entry *rets;         /* ascending */
    compat_site *trap_sites; /* sorted by pc */
    compat_site *sites;      /* storage of the sites */
} compat_area;

static compat_area g_areas[MAX_AREAS];
static volatile uint32_t g_nareas;
static uint64_t g_stub_calls, g_trap_calls;

/* a line at every 2^22 runs of either kind (how hot the replacements are) */
static void count_run(uint64_t *counter) {
    uint64_t n = __atomic_add_fetch(counter, 1, __ATOMIC_RELAXED);
    if (!(n & ((1u << 22) - 1)))
        LOG_I(CPU, "cpu compat: %llu stub runs, %llu fault runs",
              (unsigned long long)__atomic_load_n(&g_stub_calls, __ATOMIC_RELAXED),
              (unsigned long long)__atomic_load_n(&g_trap_calls, __ATOMIC_RELAXED));
}

static void COMPAT_ABI stub_entry(compat_ctx *c) {
    uint32_t n = __atomic_load_n(&g_nareas, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n; i++) {
        compat_area *a = &g_areas[i];
        if (c->ret < a->lo || c->ret >= a->hi) continue;
        uint32_t lo = 0, hi = a->nrets;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (a->rets[mid].ret < c->ret) lo = mid + 1;
            else hi = mid;
        }
        if (lo >= a->nrets || a->rets[lo].ret != c->ret) break;
        const compat_site *s = a->rets[lo].site;
        cpu_state st = {c->gpr, &c->rflags, c->ymm};
        if (!exec_insn(&s->in, s->pc, &st, s->kernel))
            LOG_E(CPU, "cpu compat: could not run the instruction at 0x%llx", (unsigned long long)s->pc);
        count_run(&g_stub_calls);
        return;
    }
    LOG_E(CPU, "cpu compat: unknown stub call from 0x%llx", (unsigned long long)c->ret);
}


typedef struct emitter {
    uint8_t *p, *end;
} emitter;

static void em(emitter *e, const void *b, size_t n) {
    if (e->p + n <= e->end) memcpy(e->p, b, n);
    e->p += n;
}
static void em1(emitter *e, uint8_t b) { em(e, &b, 1); }
static void em32(emitter *e, uint32_t v) { em(e, &v, 4); }

/* op with modrm [rsp + disp32] (or [rbx + disp32] when rbx_base) */
static void em_mem(emitter *e, uint8_t rex, const uint8_t *op, size_t n, int reg, int32_t disp, bool rbx_base) {
    if (rex | (reg >= 8 ? 4 : 0)) em1(e, (uint8_t)(0x40 | rex | (reg >= 8 ? 4 : 0)));
    em(e, op, n);
    if (rbx_base) {
        em1(e, (uint8_t)(0x83 | ((reg & 7) << 3)));
    } else {
        em1(e, (uint8_t)(0x84 | ((reg & 7) << 3)));
        em1(e, 0x24);
    }
    em32(e, (uint32_t)disp);
}
static void mov_store(emitter *e, int32_t disp, int reg) { static const uint8_t op = 0x89; em_mem(e, 8, &op, 1, reg, disp, false); }
static void mov_load(emitter *e, int reg, int32_t disp) { static const uint8_t op = 0x8B; em_mem(e, 8, &op, 1, reg, disp, false); }
static void mov_load_rbx(emitter *e, int reg, int32_t disp) { static const uint8_t op = 0x8B; em_mem(e, 8, &op, 1, reg, disp, true); }
static void mov_store_rbx(emitter *e, int32_t disp, int reg) { static const uint8_t op = 0x89; em_mem(e, 8, &op, 1, reg, disp, true); }
/* vmovdqu [rsp+disp], ymm (store) / ymm, [rsp+disp] (load): VEX2 F3 0F 7F / 6F, L=1 */
static void vmovdqu(emitter *e, bool store, int ymm, int32_t disp) {
    em1(e, 0xC5);
    em1(e, (uint8_t)((ymm >= 8 ? 0x00 : 0x80) | 0x7E));
    em1(e, store ? 0x7F : 0x6F);
    em1(e, (uint8_t)(0x84 | ((ymm & 7) << 3)));
    em1(e, 0x24);
    em32(e, (uint32_t)disp);
}

static bool emit_common(uint8_t *buf, size_t size) {
    emitter e = {buf, buf + size};
    static const uint8_t head[] = {0x9C, 0x53, 0x48, 0x89, 0xE3, 0x48, 0x83, 0xE4, 0xE0, 0x48, 0x81, 0xEC};
    em(&e, head, sizeof(head)); /* pushfq; push rbx; mov rbx,rsp; and rsp,-32; sub rsp, imm32 */
    em32(&e, (uint32_t)sizeof(compat_ctx));
    for (int r = 0; r < 16; r++)
        if (r != 3 && r != 4) mov_store(&e, CTX_GPR + r * 8, r);
    mov_load_rbx(&e, 0, 0); /* the guest's rbx */
    mov_store(&e, CTX_GPR + 3 * 8, 0);
    static const uint8_t lea_rax[] = {0x48, 0x8D, 0x83}; /* lea rax, [rbx + 152]: the guest's rsp */
    em(&e, lea_rax, 3);
    em32(&e, 152);
    mov_store(&e, CTX_GPR + 4 * 8, 0);
    mov_load_rbx(&e, 0, 8);
    mov_store(&e, CTX_FLAGS, 0);
    mov_load_rbx(&e, 0, 16);
    mov_store(&e, CTX_RET, 0);
    for (int y = 0; y < 16; y++) vmovdqu(&e, true, y, CTX_YMM + y * 32);
    static const uint8_t call[] = {0x48, 0x89, 0xE1, 0x48, 0x83, 0xEC, 0x20, 0x48, 0xB8}; /* mov rcx,rsp; sub rsp,32; mov rax, */
    em(&e, call, sizeof(call));
    void (COMPAT_ABI *fn)(compat_ctx *) = stub_entry;
    uint64_t target;
    memcpy(&target, &fn, 8);
    em(&e, &target, 8);
    static const uint8_t call_rax[] = {0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x20}; /* call rax; add rsp,32 */
    em(&e, call_rax, sizeof(call_rax));
    for (int y = 0; y < 16; y++) vmovdqu(&e, false, y, CTX_YMM + y * 32);
    mov_load(&e, 0, CTX_FLAGS);
    mov_store_rbx(&e, 8, 0);
    mov_load(&e, 0, CTX_GPR + 3 * 8);
    mov_store_rbx(&e, 0, 0);
    for (int r = 0; r < 16; r++)
        if (r != 3 && r != 4) mov_load(&e, r, CTX_GPR + r * 8);
    static const uint8_t tail[] = {0x48, 0x89, 0xDC, 0x5B, 0x9D, 0xC3}; /* mov rsp,rbx; pop rbx; popfq; ret */
    em(&e, tail, sizeof(tail));
    return e.p <= e.end;
}

#define KERNEL_SIZE 160u

/* vmovdqu xmmN, [rcx + disp32] / vmovdqu [rcx + disp32], xmmN (N < 8) */
static void lane_move(emitter *e, bool store, int x, int32_t disp) {
    em1(e, 0xC5);
    em1(e, 0xFA);
    em1(e, store ? 0x7F : 0x6F);
    em1(e, (uint8_t)(0x81 | (x << 3)));
    em32(e, (uint32_t)disp);
}

/* A 256-bit lane instruction as two runs of its 128-bit form on xmm0-3:
 * xmm0 the destination (xmm1 for the shifts by an immediate, whose
 * destination is vvvv), xmm1 vvvv, xmm2 r/m (or [rdx], the address the C
 * side computed), xmm3 the fourth register of VPBLENDVB.
 * Called as kernel(ymm registers, memory operand). */
static bool emit_kernel(uint8_t *buf, const x64_insn *in) {
    emitter e = {buf, buf + KERNEL_SIZE};
    bool ndd = lane_ndd(in), uses_v = !ndd && !lane_no_vvvv(in), count = lane_count(in);
    bool is4 = in->map == 3 && in->op == 0x4C;
    int dest_x = ndd ? 1 : 0, dest_r = ndd ? in->vvvv : in->reg;
    if (count && in->mod == 3) lane_move(&e, false, 2, (int32_t)(in->rm * 32));
    for (int lane = 0; lane < 2; lane++) {
        int32_t off = lane * 16;
        if (uses_v) lane_move(&e, false, 1, (int32_t)(in->vvvv * 32) + off);
        if (in->mod == 3 && !count) lane_move(&e, false, 2, (int32_t)(in->rm * 32) + off);
        if (is4) lane_move(&e, false, 3, (int32_t)(((in->imm >> 4) & 15) * 32) + off);
        em1(&e, 0xC4);
        em1(&e, (uint8_t)(0xE0 | in->map));
        uint8_t vv = (uint8_t)(uses_v || ndd ? 1 : 0);
        em1(&e, (uint8_t)((in->vex_w ? 0x80 : 0) | ((~vv & 15) << 3) | in->pp));
        em1(&e, in->op);
        uint8_t reg = (uint8_t)(ndd ? (in->reg & 7) : 0);
        if (in->mod == 3) {
            em1(&e, (uint8_t)(0xC0 | (reg << 3) | 2));
        } else if (lane == 0 || count) {
            em1(&e, (uint8_t)((reg << 3) | 2)); /* [rdx] */
        } else {
            em1(&e, (uint8_t)(0x40 | (reg << 3) | 2)); /* [rdx + 16] */
            em1(&e, 16);
        }
        if (in->imm_len) {
            uint8_t imm = (uint8_t)in->imm;
            if (is4) imm = (uint8_t)(0x30 | (imm & 15));
            else if (in->map == 3 && in->op == 0x42 && lane) imm = (uint8_t)(imm >> 3); /* VMPSADBW: bits 5:3 */
            em1(&e, imm);
        }
        lane_move(&e, true, dest_x, (int32_t)(dest_r * 32) + off);
    }
    em1(&e, 0xC3);
    return e.p <= e.end;
}

/* PS5C_CPU_COMPAT_SELFTEST (on a host that has the instructions): each
 * register form of a replaced vector instruction runs natively and through
 * its replacement on the same random registers; differences are logged. */
static uint8_t *g_test_code;
static bool g_trap_all;

static void vmov_rm(emitter *e, bool store, int y, int base, int32_t disp) { /* vmovdqu ymm, [base+disp32] */
    em1(e, 0xC5);
    em1(e, (uint8_t)((y >= 8 ? 0x00 : 0x80) | 0x7E));
    em1(e, store ? 0x7F : 0x6F);
    em1(e, (uint8_t)(0x80 | ((y & 7) << 3) | base));
    em32(e, (uint32_t)disp);
}

static void selftest(const compat_site *s) {
    static uint32_t tested, failed;
    bool gather = s->in.vex && s->in.map == 2 && s->in.op >= 0x90 && s->in.op <= 0x93;
    if ((s->in.mod != 3 && !gather) || (s->in.map == 1 && s->in.op == 0xD7) || tested >= 200000) return;
    if (gather && (s->in.no_base || s->in.base == 4)) return;
    static uint8_t gbuf[1u << 20];
    uint64_t gbase = (uint64_t)(uintptr_t)gbuf + (1u << 19) - (uint64_t)(int64_t)s->in.disp;
    if (gather && !gbuf[1]) for (uint32_t i = 0; i < sizeof(gbuf); i++) gbuf[i] = (uint8_t)(i * 131 + (i >> 8));
    uint32_t set = x64_compat_set(&s->in);
    if (set != X64_SET_AVX2 && set != X64_SET_FMA) return;
    if (!g_test_code) {
        void *mem = NULL;
        if (pal_vm_reserve(NULL, 65536, false, &mem) || pal_vm_commit(mem, 65536, GPROT_RW | GPROT_EXEC)) return;
        g_test_code = (uint8_t *)mem;
    }
    /* native: r8 = save area for ymm6-15, rcx = input, rdx = output */
    emitter e = {g_test_code, g_test_code + 4096};
    for (int y = 6; y < 16; y++) { /* vmovdqu [r8 + y*32], ymm_y: VEX3 with B */
        em1(&e, 0xC4);
        em1(&e, (uint8_t)((y >= 8 ? 0x00 : 0x80) | 0x40 | 0x01));
        em1(&e, 0x7E);
        em1(&e, 0x7F);
        em1(&e, (uint8_t)(0x80 | ((y & 7) << 3) | 0));
        em32(&e, (uint32_t)(y * 32));
    }
    for (int y = 0; y < 16; y++) vmov_rm(&e, false, y, 1, y * 32);
    if (gather) { /* push base; push rdx; push r8; mov base, gbase (base may be callee-saved) */
        if (s->in.base >= 8) em1(&e, 0x41);
        em1(&e, (uint8_t)(0x50 + (s->in.base & 7)));
        em1(&e, 0x52);
        em1(&e, 0x41);
        em1(&e, 0x50);
        em1(&e, (uint8_t)(0x48 | (s->in.base >= 8 ? 1 : 0)));
        em1(&e, (uint8_t)(0xB8 + (s->in.base & 7)));
        for (int i = 0; i < 8; i++) em1(&e, (uint8_t)(gbase >> (8 * i)));
    }
    em(&e, s->orig, s->in.len);
    if (gather) { /* pop r8; pop rdx; pop base */
        em1(&e, 0x41);
        em1(&e, 0x58);
        em1(&e, 0x5A);
        if (s->in.base >= 8) em1(&e, 0x41);
        em1(&e, (uint8_t)(0x58 + (s->in.base & 7)));
    }
    for (int y = 0; y < 16; y++) vmov_rm(&e, true, y, 2, y * 32);
    for (int y = 6; y < 16; y++) {
        em1(&e, 0xC4);
        em1(&e, (uint8_t)((y >= 8 ? 0x00 : 0x80) | 0x40 | 0x01));
        em1(&e, 0x7E);
        em1(&e, 0x6F);
        em1(&e, (uint8_t)(0x80 | ((y & 7) << 3) | 0));
        em32(&e, (uint32_t)(y * 32));
    }
    em1(&e, 0xC5); em1(&e, 0xF8); em1(&e, 0x77); /* vzeroupper */
    em1(&e, 0xC3);
    static uint8_t in[16][32], native[16][32], emu[16][32], save[16][32];
    static uint64_t seed = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 16 * 32; i++) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        ((uint8_t *)in)[i] = (uint8_t)(seed >> 56);
    }
    if (gather) { /* indexes within the buffer */
        uint8_t *ix = in[s->in.index == 0xFF ? 4 : s->in.index];
        unsigned is = (s->in.op & 1) ? 8 : 4;
        for (unsigned i = 0; i < 32; i++) ix[i] = i % is ? 0 : (uint8_t)(ix[i] & 63);
    }
    void (COMPAT_ABI *fn)(void *, void *, void *);
    void *code = g_test_code;
    memcpy(&fn, &code, sizeof(fn));
    fn(in, native, save);
    memcpy(emu, in, sizeof(emu));
    uint64_t gpr[16] = {0}, rflags = 0;
    if (gather) gpr[s->in.base] = gbase;
    cpu_state st = {gpr, &rflags, emu};
    exec_insn(&s->in, s->pc, &st, s->kernel);
    tested++;
    if (memcmp(native, emu, sizeof(emu))) {
        if (failed++ < 40) {
            char hex[64] = {0};
            for (int i = 0; i < s->in.len && i < 15; i++) snprintf(hex + i * 3, 4, "%02X ", s->orig[i]);
            int r = 0;
            while (r < 16 && !memcmp(native[r], emu[r], 32)) r++;
            LOG_E(CPU, "cpu compat selftest: %s(map %u op %02X pp %u L %u) differs in ymm%d", hex, s->in.map, s->in.op,
                  s->in.pp, s->in.vex_l, r);
        }
    }
    if (tested % 5000 == 0) LOG_I(CPU, "cpu compat selftest: %u tested, %u differ", tested, failed);
}

static bool rel32_ok(uint64_t from_next, uint64_t to) {
    int64_t d = (int64_t)(to - from_next);
    return d >= INT32_MIN && d <= INT32_MAX;
}

static int cmp_site(const void *a, const void *b) {
    uint64_t x = ((const compat_site *)a)->pc, y = ((const compat_site *)b)->pc;
    return x < y ? -1 : x > y;
}

/* PS5C_CPU_COMPAT_SKIP: AVX2 forms left alone (debugging aid): "lane", "c", or
 * map.opcode pairs in hex ("2.58,3.39") */
static bool avx2_skipped(const x64_insn *in) {
    static const char *skip;
    static int init;
    if (!init) {
        skip = getenv("PS5C_CPU_COMPAT_SKIP");
        init = 1;
    }
    if (!skip) return false;
    int kind = avx2_kind(in);
    if ((strstr(skip, "lane") && kind == AVX2_LANE) || (strstr(skip, "c") && kind == AVX2_C)) return true;
    if (strstr(skip, "short") && in->len < 5) return true;
    if (strstr(skip, "long") && in->len >= 5) return true;
    if (strstr(skip, "mem") && in->mod != 3) return true;
    if (strstr(skip, "regs") && in->mod == 3) return true;
    char key[16];
    snprintf(key, sizeof(key), "%u.%x", in->map, in->op);
    for (const char *p = strstr(skip, key); p; p = strstr(p + 1, key)) {
        size_t n = strlen(key);
        if ((p == skip || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return true;
    }
    return false;
}

/* Relative branches: true with the target for jcc / jmp / call rel8|rel32 */
static bool rel_branch(const x64_insn *in, uint64_t pc, uint64_t *target) {
    bool rel = (in->map == 0 && ((in->op >= 0x70 && in->op <= 0x7F) || in->op == 0xEB || in->op == 0xE9 ||
                                 in->op == 0xE8 || (in->op >= 0xE0 && in->op <= 0xE3))) ||
               (in->map == 1 && in->op >= 0x80 && in->op <= 0x8F);
    if (!rel || in->vex) return false;
    int64_t d = in->imm_len == 1 ? (int8_t)in->imm : (int32_t)in->imm;
    *target = pc + in->len + (uint64_t)d;
    return true;
}

/* Control transfer other than a relative jcc / jmp (those are moved, re-encoded) */
static bool other_control(const x64_insn *in) {
    if (in->vex) return false;
    if (in->map == 0) {
        uint8_t op = in->op;
        if (op == 0xE8 || (op >= 0xE0 && op <= 0xE3) || op == 0xC2 || op == 0xC3 || op == 0xCA || op == 0xCB ||
            op == 0xCC || op == 0xCD || op == 0xCF || op == 0xF1 || op == 0xF4)
            return true;
        if (op == 0xFF && ((in->reg & 7) >= 2 && (in->reg & 7) <= 5)) return true;
    }
    if (in->map == 1 && (in->op == 0x05 || in->op == 0x0B || in->op == 0x07 || in->op == 0x34 || in->op == 0x35))
        return true;
    return false;
}

#define SHORT_STUB_MAX 112u
#define LONG_STUB_MAX 32u

static uint64_t emit_call_item(emitter *e, uint8_t *common) {
    static const uint8_t lea_down[] = {0x48, 0x8D, 0x64, 0x24, 0x80}; /* lea rsp, [rsp-128]: the red zone */
    em(e, lea_down, 5);
    em1(e, 0xE8);
    em32(e, (uint32_t)(int32_t)(common - (e->p + 4)));
    uint64_t ret = (uint64_t)(uintptr_t)e->p;
    static const uint8_t lea_up[] = {0x48, 0x8D, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00};
    em(e, lea_up, 8);
    return ret;
}

/* An instruction moved into a stub as is, or re-encoded (RIP-relative
 * operand, relative branch); false when it cannot be. */
static bool emit_moved(emitter *e, const uint8_t *bytes, const x64_insn *in, uint64_t pc) {
    uint64_t target;
    if (rel_branch(in, pc, &target)) {
        if (in->map == 0 && in->op == 0xE8) return false;
        if (in->map == 0 && (in->op == 0xEB || in->op == 0xE9)) {
            em1(e, 0xE9);
        } else if (in->map == 0 && in->op >= 0x70 && in->op <= 0x7F) {
            em1(e, 0x0F);
            em1(e, (uint8_t)(0x80 + (in->op - 0x70)));
        } else if (in->map == 1) {
            em1(e, 0x0F);
            em1(e, in->op);
        } else {
            return false;
        }
        int64_t d = (int64_t)(target - ((uint64_t)(uintptr_t)e->p + 4));
        if (d < INT32_MIN || d > INT32_MAX) return false;
        em32(e, (uint32_t)(int32_t)d);
        return true;
    }
    if (other_control(in)) return false;
    uint8_t *at = e->p;
    em(e, bytes, in->len);
    if (in->rip_rel && e->p <= e->end) {
        int64_t d = (int64_t)in->disp + (int64_t)(pc - (uint64_t)(uintptr_t)at);
        if (d < INT32_MIN || d > INT32_MAX) return false;
        int32_t nd = (int32_t)d;
        memcpy(at + in->disp_off, &nd, 4);
    }
    return true;
}

static void set_bit(uint8_t *bits, uint64_t i) { bits[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static bool get_bit(const uint8_t *bits, uint64_t i) { return (bits[i >> 3] >> (i & 7)) & 1; }

static int cmp_ret(const void *a, const void *b) {
    uint64_t x = ((const ret_entry *)a)->ret, y = ((const ret_entry *)b)->ret;
    return x < y ? -1 : x > y;
}

void x64_compat_patch(gmem *mem, const char *module, guest_addr code, uint64_t size, guest_addr near_hint) {
    uint32_t missing = x64_compat_missing();
    if (x64_guest_tsc_enabled()) missing |= X64_SET_TSC;
    if (!missing || !size) return;
    g_trap_all = getenv("PS5C_CPU_COMPAT_TRAPALL") != NULL;
    uint8_t *p = (uint8_t *)gmem_host(mem, code, size);
    if (!p) return;
    uint64_t base = (uint64_t)(uintptr_t)p;
    /* the sites and the targets of relative branches, by linear sweep (code
     * segments hold no data in PS5 modules) */
    uint8_t *targets = (uint8_t *)calloc((size_t)(size / 8 + 1), 1);
    compat_site *sites = NULL;
    uint32_t count = 0, cap = 0, per_set[7] = {0};
    for (uint64_t off = 0; targets && off < size;) {
        x64_insn in;
        if (!x64_decode(p + off, (size_t)PS5C_MIN(size - off, 15), &in)) {
            off++;
            continue;
        }
        uint64_t t;
        if (rel_branch(&in, base + off, &t) && t >= base && t < base + size) set_bit(targets, t - base);
        uint32_t set = x64_compat_set(&in);
        if (set == X64_SET_AVX2 && avx2_skipped(&in)) set = 0;
        if (set & missing) {
            if (count == cap) {
                cap = cap ? cap * 2 : 1024;
                compat_site *n = (compat_site *)realloc(sites, cap * sizeof(*sites));
                if (!n) break;
                sites = n;
            }
            memset(&sites[count], 0, sizeof(sites[count]));
            sites[count].pc = base + off;
            sites[count].in = in;
            memcpy(sites[count].orig, p + off, in.len);
            count++;
            per_set[__builtin_ctz(set)]++;
        }
        off += in.len;
    }
    /* PS5C_CPU_COMPAT_SITES=<file>: each scanned segment and the offsets of its sites (to compare with a
     * disassembler) */
    const char *dump = getenv("PS5C_CPU_COMPAT_SITES");
    FILE *df = dump ? fopen(dump, "a") : NULL;
    if (df) {
        fprintf(df, "segment\t%s\t%llx\n", module, (unsigned long long)size);
        for (uint32_t i = 0; i < count; i++)
            fprintf(df, "site\t%llx\n", (unsigned long long)(sites[i].pc - base));
        fclose(df);
    }
    if (!count) {
        free(sites);
        free(targets);
        return;
    }
    uint32_t nkernels = 0;
    for (uint32_t i = 0; i < count; i++) nkernels += avx2_kind(&sites[i].in) == AVX2_LANE;
    ret_entry *rets = (ret_entry *)malloc((count + 1) * sizeof(ret_entry));
    compat_site *trap_sites = (compat_site *)malloc((count + 1) * sizeof(compat_site));
    bool *done = (bool *)calloc(count, 1);
    uint32_t ai = __atomic_load_n(&g_nareas, __ATOMIC_ACQUIRE);
    guest_addr area = 0;
    uint64_t code_bytes = (uint64_t)count * SHORT_STUB_MAX;
    uint64_t area_size = PS5C_ALIGN_UP(4096 + code_bytes + (uint64_t)nkernels * KERNEL_SIZE, GUEST_PAGE_SIZE);
    if (!rets || !trap_sites || !done || !targets || ai >= MAX_AREAS ||
        gmem_map(mem, PS5C_ALIGN_UP(near_hint, 0x10000), area_size, GPROT_RW | GPROT_EXEC, 0, "cpu compat stubs",
                 &area) != PS5C_OK) {
        LOG_E(CPU, "%s: %u instructions the host lacks, no room to replace them", module, count);
        free(sites);
        free(rets);
        free(trap_sites);
        free(done);
        free(targets);
        return;
    }
    uint8_t *ab = (uint8_t *)gmem_host(mem, area, area_size);
    memset(ab, 0xCC, (size_t)area_size);
    uint8_t *common = ab; /* one copy per area: always within reach of its stubs */
    if (!emit_common(common, 4096)) {
        LOG_E(CPU, "cpu compat: common code does not fit");
        return;
    }
    uint8_t *cp = ab + 4096, *code_end = cp + code_bytes;
    uint8_t *kernels = code_end;
    uint32_t nrets = 0, ti = 0, ki = 0, bad = 0, moved = 0, nstubs = 0;
    for (uint32_t i = 0; i < count; i++) {
        compat_site *s = &sites[i];
        if (avx2_kind(&s->in) == AVX2_LANE) {
            uint8_t *k = kernels + (uint64_t)ki++ * KERNEL_SIZE;
            if (emit_kernel(k, &s->in)) s->kernel = k;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        if (done[i]) continue;
        compat_site *s = &sites[i];
        uint8_t *site = (uint8_t *)(uintptr_t)s->pc;
        if (avx2_kind(&s->in) == AVX2_LANE && !s->kernel) {
            bad++;
            continue; /* left as is */
        }
        done[i] = true;
        /* the stub: this instruction, then (when shorter than the 5-byte jmp)
         * the ones after it, until 5 bytes are covered */
        emitter e = {cp, code_end};
        uint8_t *stub = cp;
        uint32_t first_ret = nrets;
        rets[nrets].ret = emit_call_item(&e, common);
        rets[nrets++].site = s;
        uint64_t cur = s->pc + s->in.len, end = base + size;
        bool ok = !g_trap_all || x64_compat_set(&s->in) != X64_SET_AVX2;
        uint32_t j = i + 1, nmoved = 0, marked[8], nmarked = 0;
        while (ok && cur - s->pc < 5) {
            if (cur >= end || get_bit(targets, cur - base)) {
                ok = false;
                break;
            }
            while (j < count && sites[j].pc < cur) j++;
            if (j < count && sites[j].pc == cur && !done[j] &&
                !(avx2_kind(&sites[j].in) == AVX2_LANE && !sites[j].kernel)) {
                rets[nrets].ret = emit_call_item(&e, common);
                rets[nrets++].site = &sites[j];
                done[j] = true;
                if (nmarked < 8) marked[nmarked++] = j;
                cur += sites[j].in.len;
                continue;
            }
            x64_insn in;
            if (!x64_decode((const uint8_t *)(uintptr_t)cur, (size_t)PS5C_MIN(end - cur, 15), &in) ||
                !emit_moved(&e, (const uint8_t *)(uintptr_t)cur, &in, cur)) {
                ok = false;
                break;
            }
            nmoved++;
            cur += in.len;
        }
        if (ok) {
            em1(&e, 0xE9);
            em32(&e, (uint32_t)(int32_t)((int64_t)cur - (int64_t)((uint64_t)(uintptr_t)e.p + 4)));
            ok = e.p <= e.end && rel32_ok(s->pc + 5, (uint64_t)(uintptr_t)stub);
        }
        if (!ok) { /* UD2: the fault handler runs it (Windows writes the fault's context over the red zone:
                    * a last resort) */
            nrets = first_ret;
            for (uint32_t k = 0; k < nmarked; k++) done[marked[k]] = false;
            memset(stub, 0xCC, (size_t)(e.p - stub < SHORT_STUB_MAX ? e.p - stub : SHORT_STUB_MAX));
            trap_sites[ti++] = *s;
            site[0] = 0x0F;
            site[1] = 0x0B;
            continue;
        }
        cp = (uint8_t *)PS5C_ALIGN_UP((uintptr_t)e.p, 16);
        site[0] = 0xE9;
        int32_t rel = (int32_t)(stub - (site + 5));
        memcpy(site + 1, &rel, 4);
        for (uint64_t k = 5; k < cur - s->pc; k++) site[k] = 0xCC;
        nstubs++;
        moved += nmoved;
    }
    if (getenv("PS5C_CPU_COMPAT_SELFTEST"))
        for (uint32_t i = 0; i < count; i++) selftest(&sites[i]);
    qsort(trap_sites, ti, sizeof(compat_site), cmp_site);
    qsort(rets, nrets, sizeof(ret_entry), cmp_ret);
    compat_area *a = &g_areas[ai];
    a->lo = (uint64_t)(uintptr_t)ab;
    a->hi = (uint64_t)(uintptr_t)code_end;
    a->rets = rets;
    a->nrets = nrets;
    a->trap_sites = trap_sites;
    a->ntraps = ti;
    a->sites = sites;
    __atomic_store_n(&g_nareas, ai + 1, __ATOMIC_RELEASE);
    pal_icache_flush(p, size);
    pal_icache_flush(ab, (size_t)area_size);
    free(done);
    free(targets);
    LOG_I(CPU, "%s: replaced %u instructions (BMI1 %u, BMI2 %u, LZCNT %u, MOVBE %u, FMA3 %u, AVX2 %u, TSC %u): %u stubs (%u "
               "instructions moved along), %u by faults",
          module, count - bad, per_set[0], per_set[1], per_set[2], per_set[3], per_set[4], per_set[5],
          per_set[6], nstubs, moved, ti);
}

static const compat_site *find_trap(uint64_t pc) {
    uint32_t n = __atomic_load_n(&g_nareas, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n; i++) {
        const compat_area *a = &g_areas[i];
        uint32_t lo = 0, hi = a->ntraps;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (a->trap_sites[mid].pc < pc) lo = mid + 1;
            else hi = mid;
        }
        if (lo < a->ntraps && a->trap_sites[lo].pc == pc) return &a->trap_sites[lo];
    }
    return NULL;
}

/* CLZERO (0F 01 FC, AMD only): zeroes the 64-byte line holding [rax]. The PS5's Zen 2 has it, Intel hosts
 * fault. The usual loop `clzero; add rax,0x40; cmp rax,rcx; jb` is run whole in one go. */
static bool clzero_fault(pal_fault_info *info) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)info->pc;
    if (p[0] != 0x0F || p[1] != 0x01 || p[2] != 0xFC) return false;
    static const uint8_t loop[] = {0x48, 0x83, 0xC0, 0x40, 0x48, 0x39, 0xC8, 0x72, 0xF4};
    static bool logged;
    if (!logged) {
        logged = true;
        LOG_I(CPU, "clzero emulated (first at %llx)", (unsigned long long)info->pc);
    }
    uint64_t rax = info->gpr[0], rcx = info->gpr[1];
    if (memcmp(p + 3, loop, sizeof(loop)) == 0) {
        do {
            memset((void *)(uintptr_t)(rax & ~63ull), 0, 64);
            rax += 0x40;
        } while (rax < rcx);
        info->gpr[0] = rax;
        /* flags of the last cmp rax,rcx (rax >= rcx): CF=0, ZF on equality, SF/OF/PF from the difference */
        uint64_t d = rax - rcx, f = info->rflags & ~0x8D5ull;
        if (d == 0) f |= 0x40;
        if (d >> 63) f |= 0x80;
        if ((((rax ^ rcx) & (rax ^ d)) >> 63) & 1) f |= 0x800;
        if (!(__builtin_popcount((unsigned)(d & 0xFF)) & 1)) f |= 0x4;
        if (((rax ^ rcx ^ d) >> 4) & 1) f |= 0x10;
        info->rflags = f;
        info->pc += 3 + sizeof(loop);
        return true;
    }
    memset((void *)(uintptr_t)(rax & ~63ull), 0, 64);
    info->pc += 3;
    return true;
}

bool x64_compat_fault(pal_fault_info *info) {
    if (info->kind != PAL_FAULT_ILLEGAL_INSTRUCTION) return false;
    if (clzero_fault(info)) return true;
    const compat_site *s = find_trap(info->pc);
    x64_insn local;
    const x64_insn *in;
    if (s) {
        in = &s->in;
    } else { /* not found by the scan (an instruction the sweep did not reach) */
        if (!x64_decode((const uint8_t *)(uintptr_t)info->pc, 15, &local) || !x64_compat_set(&local)) return false;
        if (x64_compat_set(&local) == X64_SET_TSC && !x64_guest_tsc_enabled()) return false;
        in = &local;
    }
    uint64_t gpr[16];
    memcpy(gpr, info->gpr, sizeof(gpr));
    uint8_t ymm[16][32];
    for (int i = 0; i < 16; i++) {
        memcpy(ymm[i], info->xmm[i], 16);
        memcpy(ymm[i] + 16, info->ymm_hi[i], 16);
    }
    cpu_state st = {gpr, &info->rflags, info->has_ymm_hi ? ymm : NULL};
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("PS5C_CPU_COMPAT_DEBUG") != NULL;
    uint8_t before[16][32];
    if (dbg) memcpy(before, ymm, sizeof(before));
    if (!exec_insn(in, info->pc, &st, s ? s->kernel : NULL)) return false;
    if (dbg && s && in->vex) {
        static uint32_t shown;
        if (shown++ < 12) {
            char hex[64] = {0};
            for (int i = 0; i < in->len; i++) snprintf(hex + i * 3, 4, "%02X ", s->orig[i]);
            int r = in->reg;
            LOG_I(CPU, "trap %llx %s has_hi %d: ymm%d %016llx%016llx%016llx%016llx -> %016llx%016llx%016llx%016llx",
                  (unsigned long long)info->pc, hex, info->has_ymm_hi, r, *(unsigned long long *)(before[r] + 24),
                  *(unsigned long long *)(before[r] + 16), *(unsigned long long *)(before[r] + 8),
                  *(unsigned long long *)before[r], *(unsigned long long *)(ymm[r] + 24), *(unsigned long long *)(ymm[r] + 16),
                  *(unsigned long long *)(ymm[r] + 8), *(unsigned long long *)ymm[r]);
        }
    }
    for (int i = 0; i < 16; i++) {
        if (i != 4) info->gpr[i] = gpr[i];
        memcpy(info->xmm[i], ymm[i], 16);
        memcpy(info->ymm_hi[i], ymm[i] + 16, 16);
    }
    info->pc += in->len;
    count_run(&g_trap_calls);
    return true;
}
