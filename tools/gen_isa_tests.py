#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generates tests/unit/test_shader_isa_cases.inc: one small compute program per
instruction the translator gained when the ISA was completed, with the result
every lane must store (computed here, from the instruction's definition).

Each program sets v1..v3 and s20..s22 to the case's inputs, v4 and s24 to the
initial destination, runs the instruction(s), and stores v[4:5] (scalar cases:
s[24:25]) at the lane's index. Encodings come from clang's AMDGPU assembler
(tools/amdgpu_asm.py; gfx1030 for the dot products gfx1013 lacks).

    python tools/gen_isa_tests.py > tests/unit/test_shader_isa_cases.inc
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import amdgpu_asm  # noqa: E402

M32 = 0xFFFFFFFF


def f32(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def tof(u):
    return struct.unpack("<f", struct.pack("<I", u & M32))[0]


def h(x):
    return struct.unpack("<H", struct.pack("<e", x))[0]


def tohf(u):
    return struct.unpack("<e", struct.pack("<H", u & 0xFFFF))[0]


def hh(lo, hi):
    return h(lo) | (h(hi) << 16)


def sx(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


def fields(v, bits):
    return [(v >> o) & ((1 << bits) - 1) for o in range(0, 32, bits)]


def r16(v, init):  # a 16-bit result in the low half, the high half kept
    return (init & 0xFFFF0000) | (v & 0xFFFF)


CASES = []  # (name, lines, A, B, C, init, expected (lo, hi), scalar, target, check)


def case(name, lines, a=0, b=0, c=0, init=0, lo=0, hi=0, scalar=False, target="gfx1013", check=True):
    CASES.append((name, lines if isinstance(lines, list) else [lines], a, b, c, init, (lo & M32, hi & M32), scalar,
                  target, check))


def s64(lo, hi):
    return (hi << 32) | lo


# ---- scalar ----
A, B = 0x12345678, 0x87654321
v = sx(s64(A, B), 64) >> 8
case("s_ashr_i64", "s_ashr_i64 s[24:25], s[20:21], s22", A, B, 8, lo=v & M32, hi=(v >> 32) & M32, scalar=True)
off, w = 28, 12
v = sx(s64(A, B) >> off, w)
case("s_bfe_i64", "s_bfe_i64 s[24:25], s[20:21], s22", A, B, (w << 16) | off, lo=v & M32, hi=(v >> 32) & M32, scalar=True)
case("s_cmovk_i32", ["s_cmp_eq_u32 s20, s20", "s_cmovk_i32 s24, 0x8123"], 5, init=7, lo=0xFFFF8123, scalar=True)
case("s_cmov_b64", ["s_cmp_eq_u32 s20, s20", "s_cmov_b64 s[24:25], s[20:21]"], A, B, lo=A, hi=B, scalar=True)
case("s_bcnt0_i32_b32", "s_bcnt0_i32_b32 s24, s20", 0x0000F00F, lo=24, scalar=True)
case("s_bcnt0_i32_b64", "s_bcnt0_i32_b64 s24, s[20:21]", 0xFFFFFFFF, 0x0000000F, lo=28, scalar=True)
case("s_ff0_i32_b32", "s_ff0_i32_b32 s24, s20", 0x0000FFFF, lo=16, scalar=True)
case("s_ff0_i32_b64", "s_ff0_i32_b64 s24, s[20:21]", 0xFFFFFFFF, 0x7, lo=35, scalar=True)
case("s_flbit_i32", "s_flbit_i32 s24, s20", 0xFFF00000, lo=12, scalar=True)
case("s_flbit_i32_i64", "s_flbit_i32_i64 s24, s[20:21]", 0x0FFFFFFF, 0xFFFFFFFF, lo=32, scalar=True)
rv = int("{:064b}".format(s64(A, B))[::-1], 2)
case("s_brev_b64", "s_brev_b64 s[24:25], s[20:21]", A, B, lo=rv & M32, hi=rv >> 32, scalar=True)
case("s_bitset0_b64", ["s_mov_b64 s[24:25], -1", "s_bitset0_b64 s[24:25], s22"], 0, 0, 40, lo=M32, hi=M32 & ~(1 << 8),
     scalar=True)
case("s_bitset1_b64", ["s_mov_b64 s[24:25], 0", "s_bitset1_b64 s[24:25], s22"], 0, 0, 37, lo=0, hi=1 << 5, scalar=True)
case("s_bitcmp1_b64", ["s_bitcmp1_b64 s[20:21], s22", "s_cselect_b32 s24, 1, 2"], 0, 0x10, 36, lo=1, scalar=True)
case("s_bitcmp0_b64", ["s_bitcmp0_b64 s[20:21], s22", "s_cselect_b32 s24, 1, 2"], 0, 0x10, 36, lo=2, scalar=True)
case("s_memtime", "s_memtime s[24:25]", init=5, lo=0x10000, hi=0, scalar=True)  # the first read: one 2^16 tick step
case("hints", ["s_sethalt 0", "s_setkill 0", "s_incperflevel 1", "s_decperflevel 1", "s_ttracedata_imm 3",
               "s_version 0x1", "v_clrexcp", "v_pipeflush", "s_mov_b32 s24, 7"], lo=7, scalar=True)

# ---- vector ----
a, b = 0x00800001, 0x007FFFFF
case("v_mul_hi_i32_i24", "v_mul_hi_i32_i24_e32 v4, v1, v2", a, b,
     lo=((sx(a, 24) * sx(b, 24)) >> 32) & M32)
case("v_mul_hi_u32_u24", "v_mul_hi_u32_u24_e32 v4, v1, v2", 0xFFFFFFFF, 0x00ABCDEF,
     lo=(0xFFFFFF * 0xABCDEF) >> 32)
case("v_dot2c_f32_f16", "v_dot2c_f32_f16_e32 v4, v1, v2", hh(2.0, 3.0), hh(0.5, 4.0), init=f32(1.0), lo=f32(14.0),
     target="gfx1030")
x, y = [1, -2, 3, -4], [5, 6, -7, 8]
pk = lambda vals, bits: sum((q & ((1 << bits) - 1)) << (bits * i) for i, q in enumerate(vals))
case("v_dot4c_i32_i8", "v_dot4c_i32_i8_e32 v4, v1, v2", pk(x, 8), pk(y, 8), init=10,
     lo=(sum(p * q for p, q in zip(x, y)) + 10) & M32, target="gfx1030")
case("v_mac_legacy_f32", "v_mac_legacy_f32_e32 v4, v1, v2", 0, 0x7F800000, init=f32(2.0), lo=f32(2.0))
case("v_mad_legacy_f32", "v_mad_legacy_f32 v4, v1, v2, v3", 0, 0x7FC00000, f32(1.5), lo=f32(1.5))
case("v_mullit_f32", "v_mullit_f32 v4, v1, v2, v3", f32(2.0), f32(3.0), f32(1.0), lo=f32(6.0))
case("v_mullit_f32_limit", "v_mullit_f32 v4, v1, v2, v3", f32(2.0), f32(3.0), f32(0.0), lo=0xFF7FFFFF)
case("v_ffbh_i32", "v_ffbh_i32_e32 v4, v1", 0xFFF00000, lo=12)
case("v_swap_b32", ["v_swap_b32 v4, v1", "v_mov_b32_e32 v5, v1"], 9, init=5, lo=9, hi=5)
case("v_cvt_norm_i16_f16", "v_cvt_norm_i16_f16_e32 v4, v1", h(-0.5), init=0xAAAA0000, lo=0xAAAAC000)
case("v_cvt_norm_u16_f16", "v_cvt_norm_u16_f16_e32 v4, v1", h(0.25), lo=0x4000)
case("v_frexp_exp_i16_f16", "v_frexp_exp_i16_f16_e32 v4, v1", h(10.0), init=0x55550000, lo=0x55550004)
case("v_frexp_mant_f16", "v_frexp_mant_f16_e32 v4, v1", h(10.0), lo=h(0.625))
case("v_sat_pk_u8_i16", "v_sat_pk_u8_i16_e32 v4, v1", 0x012CFFFB, lo=0xFF00)
case("v_add_nc_i32_clamp", "v_add_nc_i32 v4, v1, v2 clamp", 0x7FFFFFF0, 0x100, lo=0x7FFFFFFF)
case("v_sub_nc_i32", "v_sub_nc_i32 v4, v1, v2", 5, 7, lo=(5 - 7) & M32)
case("v_cvt_pknorm_i16_f16", "v_cvt_pknorm_i16_f16 v4, v1, v2", h(1.0), h(-1.0), lo=0x80017FFF)
case("v_cvt_pknorm_u16_f16", "v_cvt_pknorm_u16_f16 v4, v1, v2", h(0.5), h(1.0), lo=0xFFFF8000)
case("v_div_scale_f32", "v_div_scale_f32 v4, vcc_lo, v1, v2, v1", f32(3.0), f32(7.0), lo=f32(3.0))
case("v_div_fmas_f32", "v_div_fmas_f32 v4, v1, v2, v3", f32(1.5), f32(2.0), f32(0.25), lo=f32(3.25))
case("v_div_fixup_f32", "v_div_fixup_f32 v4, v1, v2, v3", f32(2.5), f32(2.0), f32(5.0), lo=f32(2.5))
case("v_div_fixup_f32_zero", "v_div_fixup_f32 v4, v1, v2, v3", f32(0.0), f32(0.0), f32(-1.0), lo=0xFF800000)
case("v_div_fixup_f16", "v_div_fixup_f16 v4, v1, v2, v3", 0, h(2.0), h(3.0), init=0x77770000, lo=0x77770000 | h(1.5))
la, lb, lc = 0x01FF8040, 0x03018041, 0x01000100
lerp = sum((((p + q + (r & 1)) >> 1) & 0xFF) << (8 * i) for i, (p, q, r) in
           enumerate(zip(fields(la, 8), fields(lb, 8), fields(lc, 8))))
case("v_lerp_u8", "v_lerp_u8 v4, v1, v2, v3", la, lb, lc, lo=lerp)
sa, sb = 0x10FF0380, 0x2001FF00
sad8 = sum(abs(p - q) for p, q in zip(fields(sa, 8), fields(sb, 8)))
case("v_sad_u8", "v_sad_u8 v4, v1, v2, v3", sa, sb, 1000, lo=sad8 + 1000)
case("v_sad_hi_u8", "v_sad_hi_u8 v4, v1, v2, v3", sa, sb, 1000, lo=(sad8 << 16) + 1000)
case("v_sad_u16", "v_sad_u16 v4, v1, v2, v3", sa, sb, 7, lo=sum(abs(p - q) for p, q in zip(fields(sa, 16), fields(sb, 16))) + 7)
case("v_msad_u8", "v_msad_u8 v4, v1, v2, v3", sa, sb, 3,
     lo=sum(abs(p - q) for p, q in zip(fields(sa, 8), fields(sb, 8)) if q) + 3)
case("v_mad_i16", "v_mad_i16 v4, v1, v2, v3", (-3) & 0xFFFF, 5, 7, init=0x12340000, lo=r16(-8, 0x12340000))
case("v_mad_u16", "v_mad_u16 v4, v1, v2, v3", 300, 300, 5, lo=(300 * 300 + 5) & 0xFFFF)
case("v_max3_i16", "v_max3_i16 v4, v1, v2, v3", (-3) & 0xFFFF, (-9) & 0xFFFF, (-5) & 0xFFFF, lo=(-3) & 0xFFFF)
case("v_min3_u16", "v_min3_u16 v4, v1, v2, v3", 9, 0xFFF0, 4, lo=4)
case("v_med3_i16", "v_med3_i16 v4, v1, v2, v3", (-3) & 0xFFFF, 8, (-9) & 0xFFFF, lo=(-3) & 0xFFFF)
case("v_med3_u16", "v_med3_u16 v4, v1, v2, v3", 9, 0xFFF0, 4, lo=9)
case("v_max3_u16", "v_max3_u16 v4, v1, v2, v3", 9, 0xFFF0, 4, lo=0xFFF0)
case("v_min3_i16", "v_min3_i16 v4, v1, v2, v3", (-3) & 0xFFFF, 8, (-9) & 0xFFFF, lo=(-9) & 0xFFFF)
case("v_mad_i32_i16", "v_mad_i32_i16 v4, v1, v2, v3", 0x7777FFFD, 5, 1000, lo=985)
case("v_mad_u32_u16", "v_mad_u32_u16 v4, v1, v2, v3", 0x7777FFFF, 0xFFFF, 1, lo=0xFFFF * 0xFFFF + 1)
case("v_dot2_f32_f16", "v_dot2_f32_f16 v4, v1, v2, v3", hh(2.0, 3.0), hh(0.5, 4.0), f32(1.0), lo=f32(14.0),
     target="gfx1030")
for name, bits, sg in (("v_dot2_i32_i16", 16, True), ("v_dot2_u32_u16", 16, False), ("v_dot4_i32_i8", 8, True),
                       ("v_dot4_u32_u8", 8, False), ("v_dot8_i32_i4", 4, True), ("v_dot8_u32_u4", 4, False)):
    n = 32 // bits
    xs = [(-1) ** i * (i + 1) for i in range(n)] if sg else [i + 2 for i in range(n)]
    ys = [3 - i for i in range(n)] if sg else [i + 1 for i in range(n)]
    xs = [max(min(q, (1 << (bits - 1)) - 1), -(1 << (bits - 1))) if sg else q & ((1 << bits) - 1) for q in xs]
    ys = [max(min(q, (1 << (bits - 1)) - 1), -(1 << (bits - 1))) if sg else q & ((1 << bits) - 1) for q in ys]
    case(name, "%s v4, v1, v2, v3" % name, pk(xs, bits), pk(ys, bits), 100,
         lo=(sum(p * q for p, q in zip(xs, ys)) + 100) & M32, target="gfx1030")
case("v_dot4_i32_i8_clamp", "v_dot4_i32_i8 v4, v1, v2, v3 clamp", pk([127] * 4, 8), pk([127] * 4, 8), 0x7FFFFFF0,
     lo=0x7FFFFFFF, target="gfx1030")




# ---- VOP3 16-bit op_sel: which half each source reads, and the half the result goes to.
# clang drops op_sel[3] (the destination half) on these ops; Unreal's code has it: raw words. ----
A16, B16, INIT = 0x0007FFF0, 0x0003000A, 0x11112222


def hi_put(v, init):  # result in the high half, the low half kept
    return ((v & 0xFFFF) << 16) | (init & 0xFFFF)


case("v_max_i16_opsel_dst_hi", [".long 0xd70a4004", ".long 0x00020501"], A16, B16, init=INIT,
     lo=hi_put(max(sx(A16 & 0xFFFF, 16), sx(B16 & 0xFFFF, 16)), INIT))
case("v_max_i16_opsel_src1_hi_dst_hi", [".long 0xd70a5004", ".long 0x00020501"], A16, B16, init=INIT,
     lo=hi_put(max(sx(A16 & 0xFFFF, 16), sx(B16 >> 16, 16)), INIT))
case("v_min_i16_opsel_src1_hi", "v_min_i16 v4, v1, v2 op_sel:[0,1,0,0]", A16, B16, init=INIT,
     lo=r16(min(sx(A16 & 0xFFFF, 16), sx(B16 >> 16, 16)), INIT))
case("v_add_nc_u16_opsel_src1_hi", "v_add_nc_u16 v4, v1, v2 op_sel:[0,1,0,0]", A16, B16, init=INIT,
     lo=r16((A16 + (B16 >> 16)) & 0xFFFF, INIT))
case("v_add_nc_u16_opsel_hi_hi", [".long 0xd7035004", ".long 0x00020501"], A16, B16, init=INIT,
     lo=hi_put((A16 + (B16 >> 16)) & 0xFFFF, INIT))
case("v_lshrrev_b16_opsel_dst_hi", [".long 0xd7074004", ".long 0x00020281"], A16, init=INIT,
     lo=hi_put((A16 & 0xFFFF) >> 1, INIT))
case("v_pk_min_i16_opsel_hi", "v_pk_min_i16 v4, v1, v2 op_sel_hi:[0,1]", A16, B16, init=INIT,
     lo=(min(sx(A16 & 0xFFFF, 16), sx(B16 & 0xFFFF, 16)) & 0xFFFF) |
        ((min(sx(A16 & 0xFFFF, 16), sx(B16 >> 16, 16)) & 0xFFFF) << 16))


# ---- SDWA word selects as in Unreal's upscaler: raw words (mov preserve, sext convert, word store) ----
case("sdwa_mov_word1_to_word0_preserve", [".long 0x7e0802f9", ".long 0x00051401"], 0x0007FFF0, init=0x11112222,
     lo=0x11110007)
case("sdwa_cvt_f32_i32_sext_word1", [".long 0x7e080af9", ".long 0x000d0601"], 0xFFF00010, lo=f32(-16.0))
case("sdwa_cvt_i32_f32_dst_word1_preserve", [".long 0x7e0810f9", ".long 0x00061501"], f32(-3.0), init=0x11112222,
     lo=0xFFFD2222)

case("sdwa_mov_inline_minus1", [".long 0x7e0802f9", ".long 0x008616c1"], 0x12345678, init=0x11112222, lo=0xFFFFFFFF)
case("v_sub_nc_i16_s_src_dst_hi", [".long 0xd70e5004", ".long 0x00020414"], 0x0007FFF0, 0x0003000A, init=0x11112222,
     lo=0xFFED2222)
case("v_pk_lshrrev_b16_opsel", "v_pk_lshrrev_b16 v4, 15, v1 op_sel:[0,1,0] op_sel_hi:[0,0,1]", 0x80010010, lo=0x00000001)
case("v_pk_sub_i16_opsel", "v_pk_sub_i16 v4, 0, v1 op_sel:[0,1,0] op_sel_hi:[0,0,1]", 0x00010000, lo=0x0000FFFF)

# every lane of a wave64 program: mbcnt_lo of all ones is min(lane, 32), mbcnt_hi adds the rest (lane)
case("v_mbcnt_lo_lane_index", ["v_mbcnt_lo_u32_b32 v4, -1, 0", "v_and_b32_e32 v6, 63, v0", "v_min_u32_e32 v6, 32, v6",
                               "v_sub_nc_u32_e32 v4, v4, v6"], lo=0)
case("v_mbcnt_hi_lane_index", ["v_mbcnt_lo_u32_b32 v4, -1, 0", "v_mbcnt_hi_u32_b32 v4, -1, v4", "v_and_b32_e32 v6, 63, v0",
                               "v_sub_nc_u32_e32 v4, v4, v6"], lo=0)

# a comparison mask (lanes with id % 32 < 16): each lane's place among them, min(id % 32, 16)
case("v_mbcnt_lo_vcc_compaction", ["v_and_b32_e32 v7, 31, v0", "v_cmp_gt_u32_e32 vcc_lo, 16, v7",
                                   "v_mbcnt_lo_u32_b32 v4, vcc_lo, 0", "v_min_u32_e32 v6, 16, v7",
                                   "v_sub_nc_u32_e32 v4, v4, v6"], lo=0)

# RADV scratch: a V# built in the shader with ADD_TID_ENABLE (word3 bit 23) gives
# each lane its own slice, so every lane reads back the index it stored
case("buffer_scratch_add_tid", ["s_mov_b32 s28, 0", "s_mov_b32 s29, 0", "s_mov_b32 s30, -1", "s_mov_b32 s31, 0x31e16000",
                                "v_and_b32_e32 v6, 63, v0", "buffer_store_dword v6, off, s[28:31], 0 offset:8",
                                "s_waitcnt vmcnt(0)", "buffer_load_dword v4, off, s[28:31], 0 offset:8",
                                "s_waitcnt vmcnt(0)", "v_sub_nc_u32_e32 v4, v4, v6"], lo=0)


# ---- v_min / v_max: -0 orders below +0 (the host's NMin/NMax may return either zero) ----
NZ, PZ = 0x80000000, 0x00000000
case("v_min_f32_signed_zero", "v_min_f32 v4, v1, v2", PZ, NZ, lo=NZ)
case("v_max_f32_signed_zero", "v_max_f32 v4, v1, v2", NZ, PZ, lo=PZ)
case("v_min_f16_signed_zero", "v_min_f16 v4, v1, v2", 0x0000, 0x8000, init=0, lo=0x8000)
case("v_max_f16_signed_zero", "v_max_f16 v4, v1, v2", 0x8000, 0x0000, init=0, lo=0x0000)


# ---- packed / mixed f16 as Unreal's TSR uses them (exact values: fused == unfused here) ----
PA, PB, PC, PI = hh(1.5, 2.5), hh(2.0, -0.5), hh(0.25, 3.0), hh(1.0, -2.0)
case("v_pk_fmac_f16", "v_pk_fmac_f16_e32 v4, v1, v2", PA, PB, init=PI, lo=hh(4.0, -3.25))
case("v_pk_fma_f16_src0_lo_both", "v_pk_fma_f16 v4, v1, v2, v3 op_sel_hi:[0,1,1]", PA, PB, PC, lo=hh(3.25, 2.25))
case("v_pk_fma_f16_inline_const", "v_pk_fma_f16 v4, 2.0, v1, v2 op_sel_hi:[0,1,1]", PA, PB, lo=hh(5.0, 4.5))
case("v_pk_fma_f16_clamp_src2_lo", "v_pk_fma_f16 v4, v1, v2, v3 op_sel_hi:[1,1,0] clamp", PA, PB, PC, lo=hh(1.0, 0.0))
case("v_pk_fma_f16_opsel_neg_hi", "v_pk_fma_f16 v4, v1, v2, v3 op_sel:[1,0,0] op_sel_hi:[1,1,0] neg_hi:[0,0,1]",
     PA, PB, PC, lo=hh(5.25, -1.5))
case("v_pk_min_f16", "v_pk_min_f16 v4, v1, v2", PA, PB, lo=hh(1.5, -0.5))
case("v_pk_max_f16", "v_pk_max_f16 v4, v1, v2", PA, PB, lo=hh(2.0, 2.5))
case("v_pk_mul_f16_opsel_src0_hi", "v_pk_mul_f16 v4, v1, v2 op_sel:[1,0,0]", PA, PB, lo=hh(5.0, -1.25))
case("v_min3_f16_opsel_hi_sources", "v_min3_f16 v4, v1, v2, v3 op_sel:[1,1,1,0]", PA, PB, PC, init=PI,
     lo=r16(h(-0.5), PI))
case("v_med3_f16", "v_med3_f16 v4, v1, v2, v3", PA, PB, PC, init=PI, lo=r16(h(1.5), PI))
case("v_fma_mixlo_f16_f32_sources", "v_fma_mixlo_f16 v4, v1, v2, v3 op_sel_hi:[0,0,0]",
     f32(1.5), f32(2.0), f32(0.25), init=PI, lo=r16(h(3.25), PI))
case("v_fma_mixhi_f16_half_src1_hi", "v_fma_mixhi_f16 v4, v1, v2, v3 op_sel:[0,1,0] op_sel_hi:[0,1,0]",
     f32(1.5), hh(0.0, 4.0), f32(0.25), init=PI, lo=(PI & 0xFFFF) | (h(6.25) << 16))
case("v_fma_mixlo_f16_neg_clamp", "v_fma_mixlo_f16 v4, v1, v2, v3 op_sel_hi:[0,0,0] clamp",
     f32(1.5), f32(2.0), f32(-4.0), init=PI, lo=r16(h(0.0), PI))

case("v_max3_f16_opsel_all_hi", [".long 0xd7547804", ".long 0x040e0501"], PA, PB, PC, init=PI,
     lo=(PI & 0xFFFF) | (h(3.0) << 16))  # clang drops op_sel[3] (dst hi): raw words
case("v_med3_f16_dst_hi", [".long 0xd7574004", ".long 0x040e0501"], PA, PB, PC, init=PI, lo=(PI & 0xFFFF) | (h(1.5) << 16))
case("v_min3_f16_src2_hi_dst_hi", [".long 0xd7516004", ".long 0x040e0501"], PA, PB, PC, init=PI,
     lo=(PI & 0xFFFF) | (h(1.5) << 16))

# SDWA f16 VOP1 into the high half, low half preserved (TSR's packed reciprocals: lo then hi)
case("sdwa_rcp_f16_word1_preserve", [".long 0x7e08a8f9", ".long 0x00051501"], hh(2.0, 4.0), init=PI,
     lo=(PI & 0xFFFF) | (h(0.25) << 16))
case("rcp_f16_pair_lo_then_hi", ["v_rcp_f16_e32 v4, v1", ".long 0x7e08a8f9", ".long 0x00051501"], hh(2.0, 4.0), init=PI,
     lo=hh(0.5, 0.25))

# v_fma_mix_f32 as TSR sums its history weights: f32 inline constant, f16 halves selected by op_sel
case("v_fma_mix_f32_const_lo_hi", "v_fma_mix_f32 v4, 1.0, v1, v2 op_sel:[0,0,1] op_sel_hi:[0,1,1]",
     hh(1.5, 9.0), hh(9.0, 2.25), lo=f32(1.0 * 1.5 + 2.25))
case("v_fma_mix_f32_const_hi_f32", "v_fma_mix_f32 v4, 1.0, v1, v2 op_sel:[0,1,0] op_sel_hi:[0,1,0]",
     hh(9.0, 3.5), f32(0.5), lo=f32(3.5 + 0.5))
case("v_fma_mix_f32_all_f16", "v_fma_mix_f32 v4, v1, v2, v3 op_sel_hi:[1,1,1]",
     hh(2.0, 9.0), hh(1.25, 9.0), hh(0.5, 9.0), lo=f32(2.0 * 1.25 + 0.5))

# SDWA v_cvt_f16_f32 into word 1, word 0 preserved (TSR packs its history normalization this way)
case("sdwa_cvt_f16_f32_word1_preserve", [".long 0x7e0814f9", ".long 0x00061501"], f32(1.0), init=hh(6.125, 7.0),
     lo=(h(6.125)) | (h(1.0) << 16))

case("v_pack_b32_f16_opsel_src0_hi", "v_pack_b32_f16 v4, v1, v2 op_sel:[1,0,0]", hh(6.125, 1.0), hh(2.0, 9.0),
     lo=hh(1.0, 2.0))
case("v_pack_b32_f16_opsel_both_hi", "v_pack_b32_f16 v4, v1, v2 op_sel:[1,1,0]", hh(6.125, 1.0), hh(2.0, 9.0),
     lo=hh(1.0, 9.0))
case("v_pack_b32_f16_neg", "v_pack_b32_f16 v4, -v1, |v2|", hh(6.125, 1.0), hh(-2.0, 9.0), lo=hh(-6.125, 2.0))

# ---- f64 (exact on the integer path; arithmetic through f32, exact for these values) ----
def d(x):
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def dcase(name, lines, x=0.0, res=None, lo=None, hi=None, extra=()):
    """x in v[1:2]; extra doubles in v[6:7], v[8:9]; the result in v[4:5]"""
    bits = d(x)
    pre = []
    for k, y in enumerate(extra):
        yb = d(y)
        pre += ["v_mov_b32_e32 v%d, 0x%x" % (6 + 2 * k, yb & M32), "v_mov_b32_e32 v%d, 0x%x" % (7 + 2 * k, yb >> 32)]
    if res is not None:
        lo, hi = d(res) & M32, d(res) >> 32
    case(name, pre + (lines if isinstance(lines, list) else [lines]), bits & M32, bits >> 32, 0, lo=lo, hi=hi)


case("v_cvt_f64_f32", "v_cvt_f64_f32_e32 v[4:5], v1", f32(1.5), lo=d(1.5) & M32, hi=d(1.5) >> 32)
case("v_cvt_f64_f32_denormal", "v_cvt_f64_f32_e32 v[4:5], v1", 1, lo=d(2.0 ** -149) & M32, hi=d(2.0 ** -149) >> 32)
dcase("v_cvt_f32_f64", "v_cvt_f32_f64_e32 v4, v[1:2]", 3.14159265358979, lo=f32(3.14159265358979), hi=0)
case("v_cvt_f64_i32", "v_cvt_f64_i32_e32 v[4:5], v1", (-7) & M32, lo=d(-7.0) & M32, hi=d(-7.0) >> 32)
case("v_cvt_f64_u32", "v_cvt_f64_u32_e32 v[4:5], v1", 0xFFFFFFFF, lo=d(4294967295.0) & M32, hi=d(4294967295.0) >> 32)
dcase("v_cvt_i32_f64", "v_cvt_i32_f64_e32 v4, v[1:2]", -123.75, lo=(-123) & M32, hi=0)
dcase("v_cvt_u32_f64_sat", "v_cvt_u32_f64_e32 v4, v[1:2]", 1e12, lo=0xFFFFFFFF, hi=0)
dcase("v_floor_f64", "v_floor_f64_e32 v[4:5], v[1:2]", -2.5, res=-3.0)
dcase("v_ceil_f64_neg_zero", "v_ceil_f64_e32 v[4:5], v[1:2]", -0.5, lo=0, hi=0x80000000)
dcase("v_trunc_f64", "v_trunc_f64_e32 v[4:5], v[1:2]", 1234.999, res=1234.0)
dcase("v_rndne_f64_even", "v_rndne_f64_e32 v[4:5], v[1:2]", 2.5, res=2.0)
dcase("v_rndne_f64_odd", "v_rndne_f64_e32 v[4:5], v[1:2]", 3.5, res=4.0)
dcase("v_fract_f64_neg", "v_fract_f64_e32 v[4:5], v[1:2]", -1.25, res=0.75)
dcase("v_fract_f64", "v_fract_f64_e32 v[4:5], v[1:2]", 5.75, res=0.75)
dcase("v_frexp_mant_f64", "v_frexp_mant_f64_e32 v[4:5], v[1:2]", 12.0, res=0.75)
dcase("v_frexp_exp_i32_f64", "v_frexp_exp_i32_f64_e32 v4, v[1:2]", 12.0, lo=4, hi=0)
dcase("v_ldexp_f64", ["v_mov_b32_e32 v3, 10", "v_ldexp_f64 v[4:5], v[1:2], v3"], 1.5, res=1536.0)
dcase("v_max_f64", "v_max_f64 v[4:5], v[1:2], v[6:7]", -1.5, res=2.25, extra=(2.25,))
dcase("v_min_f64", "v_min_f64 v[4:5], v[1:2], v[6:7]", -1.5, res=-1.5, extra=(2.25,))
dcase("v_add_f64", "v_add_f64 v[4:5], v[1:2], v[6:7]", 1.5, res=3.75, extra=(2.25,))
dcase("v_mul_f64", "v_mul_f64 v[4:5], v[1:2], v[6:7]", 1.5, res=-3.0, extra=(-2.0,))
dcase("v_fma_f64", "v_fma_f64 v[4:5], v[1:2], v[6:7], v[8:9]", 2.0, res=7.0, extra=(3.0, 1.0))
dcase("v_sqrt_f64", "v_sqrt_f64_e32 v[4:5], v[1:2]", 16.0, res=4.0)
dcase("v_rcp_f64", "v_rcp_f64_e32 v[4:5], v[1:2]", 4.0, res=0.25)
dcase("v_rsq_f64", "v_rsq_f64_e32 v[4:5], v[1:2]", 4.0, res=0.5)
dcase("v_div_fixup_f64", "v_div_fixup_f64 v[4:5], v[1:2], v[6:7], v[8:9]", 2.5, res=2.5, extra=(4.0, 10.0))

# ---- memory: s[8:11] is a raw buffer over 1024 bytes holding PATTERN (s[8:9] its address) ----
PATTERN = [(i * 37 + 0x80) & 0xFF for i in range(1024)]
MEM = {}  # name -> (offset, expected bytes after the dispatch)


def word(off):
    return PATTERN[off] | (PATTERN[off + 1] << 8) | (PATTERN[off + 2] << 16) | (PATTERN[off + 3] << 24)


for name, off, bits, sgn in (("buffer_load_sbyte", 5, 8, True), ("buffer_load_sshort", 6, 16, True)):
    val = PATTERN[off] | (PATTERN[off + 1] << 8 if bits == 16 else 0)
    case(name, "%s v4, off, s[8:11], 0 offset:%d" % (name, off), lo=sx(val, bits) & M32)
for name, off, bits, sgn, hi in (("buffer_load_ubyte_d16", 9, 8, False, False), ("buffer_load_ubyte_d16_hi", 9, 8, False, True),
                                 ("buffer_load_sbyte_d16", 9, 8, True, False), ("buffer_load_sbyte_d16_hi", 9, 8, True, True),
                                 ("buffer_load_short_d16", 10, 16, False, False),
                                 ("buffer_load_short_d16_hi", 10, 16, False, True)):
    val = PATTERN[off] | (PATTERN[off + 1] << 8 if bits == 16 else 0)
    val = (sx(val, bits) if sgn else val) & 0xFFFF
    init = 0x55AA33CC
    case(name, "%s v4, off, s[8:11], 0 offset:%d" % (name, off), init=init,
         lo=(init & 0xFFFF) | (val << 16) if hi else (init & 0xFFFF0000) | val)
# stores: each lane writes its byte / short next to the others' (v6 = offset + lane * size)
for name, base, size, hi in (("buffer_store_byte", 0x100, 1, False), ("buffer_store_byte_d16_hi", 0x140, 1, True),
                             ("buffer_store_short", 0x180, 2, False), ("buffer_store_short_d16_hi", 0x200, 2, True)):
    lines = ["v_mul_u32_u24_e32 v6, %d, v0" % size, "v_add_nc_u32_e32 v6, 0x%x, v6" % base,
             "v_mul_u32_u24_e32 v1, 0x10001, v0", "v_add_nc_u32_e32 v1, 0xa100b0, v1",
             "%s v1, v6, s[8:11], 0 offen" % name]
    exp = []
    for lane in range(64):
        v = (lane * 0x10001 + 0x00A100B0) & M32
        if hi:
            v >>= 16
        exp += [(v >> (8 * k)) & 0xFF for k in range(size)]
    case(name, lines, check=False)
    MEM[name] = (base, exp)
# global atomics, every lane on one dword (s[8:9] the base, v6 the offset)
g = 0x2C0
lines = ["v_mov_b32_e32 v6, 0x%x" % g, "v_mov_b32_e32 v2, 1", "global_atomic_add v4, v6, v2, s[8:9] glc"]
case("global_atomic_add", lines, check=False)
MEM["global_atomic_add"] = (g, list(((word(g) + 64) & M32).to_bytes(4, "little")))
lines = ["v_mov_b32_e32 v6, 0x%x" % g, "v_add_nc_u32_e32 v2, 0x7fffff00, v0", "global_atomic_umax v6, v2, s[8:9]"]
case("global_atomic_umax", lines, check=False)
MEM["global_atomic_umax"] = (g, list(max(word(g), 0x7FFFFF00 + 63).to_bytes(4, "little")))
lines = ["v_mov_b32_e32 v6, 0x%x" % g, "v_mov_b32_e32 v2, 0x1234", "v_mov_b32_e32 v3, 0x%x" % word(g),
         "global_atomic_cmpswap v4, v6, v[2:3], s[8:9] glc", "v_cmp_eq_u32_e32 vcc_lo, 0x%x, v4" % word(g),
         "v_cndmask_b32_e64 v4, 0, 1, vcc_lo"]
case("global_atomic_cmpswap", lines, check=False)
MEM["global_atomic_cmpswap"] = (g, list((0x1234).to_bytes(4, "little")))


def program(lines, a, b, c, init, scalar):
    pre = ["v_mov_b32_e32 v4, 0x%x" % init, "v_mov_b32_e32 v5, 0", "v_mov_b32_e32 v1, 0x%x" % a,
           "v_mov_b32_e32 v2, 0x%x" % b, "v_mov_b32_e32 v3, 0x%x" % c, "s_mov_b32 s20, 0x%x" % a,
           "s_mov_b32 s21, 0x%x" % b, "s_mov_b32 s22, 0x%x" % c, "s_mov_b32 s24, 0x%x" % init, "s_mov_b32 s25, 0"]
    post = ["v_mov_b32_e32 v4, s24", "v_mov_b32_e32 v5, s25"] if scalar else []
    post += ["buffer_store_dwordx2 v[4:5], v0, s[12:15], 0 idxen", "s_endpgm"]
    return pre + lines + post


def main():
    out = []
    for name, lines, a, b, c, init, exp, scalar, target, check in CASES:
        amdgpu_asm.TARGET = target
        src = program(lines, a, b, c, init, scalar)
        words = amdgpu_asm.assemble(src)
        bad = [line for line, w in zip(src, words) if w is None]
        if bad:
            sys.exit("%s: does not assemble: %s" % (name, bad))
        code = [w for ws in words for w in ws]
        out.append((name, "; ".join(lines), code, exp, check))
    print("/* Generated by tools/gen_isa_tests.py. Do not edit. */")
    print("/* clang-format off */")
    print("#ifdef ISA_PATTERN")
    print("static const uint8_t k_pattern[%d] = {%s};" % (len(PATTERN), ", ".join(str(x) for x in PATTERN)))
    print("#else")
    for name, text, code, exp, check in out:
        mem = MEM.get(name)
        memtext = "0x%x, %d, {%s}" % (mem[0], len(mem[1]), ", ".join(str(x) for x in mem[1])) if mem else "0, 0, {0}"
        print("{\"%s\", \"%s\", %d, {%s}, {0x%08xu, 0x%08xu}, %d, %s}," %
              (name, text, len(code), ", ".join("0x%08xu" % w for w in code), exp[0], exp[1], 1 if check else 0,
               memtext))
    print("#endif")


if __name__ == "__main__" and len(sys.argv) == 1:
    main()


# ---- coverage: every opcode of the decoder's tables, in one form clang assembles ----
ENC_OK = {
    "sop2": lambda w: w >> 30 == 2 and w >> 28 != 0xB, "sopk": lambda w: w >> 28 == 0xB,
    "sop1": lambda w: w >> 23 == 0x17D, "sopc": lambda w: w >> 23 == 0x17E, "sopp": lambda w: w >> 23 == 0x17F,
    "smem": lambda w: w >> 26 == 0x3D, "vop2": lambda w: w >> 31 == 0, "vop1": lambda w: w >> 25 == 0x3F,
    "vopc": lambda w: w >> 25 == 0x3E, "vop3": lambda w: w >> 26 == 0x35, "vop3p": lambda w: w >> 24 == 0xCC,
    "ds": lambda w: w >> 26 == 0x36, "mubuf": lambda w: w >> 26 == 0x38, "mtbuf": lambda w: w >> 26 == 0x3A,
    "mimg": lambda w: w >> 26 == 0x3C, "global": lambda w: w >> 26 == 0x37 and (w >> 14) & 3 == 2,
}


def coverage():
    import re
    import gen_rdna2_ops
    for target in ("gfx1013", "gfx1030"):
        amdgpu_asm.TARGET = target
        gen_rdna2_ops.probe_all()
    inc = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "gpu", "rdna2_ops.inc")).read()
    print("/* Generated by tools/gen_isa_tests.py --coverage. Do not edit. */")
    print("/* clang-format off */")
    for m in re.finditer(r"k_(\w+)\[\d+\] = \{(.*?)\n\};", inc, re.S):
        table = m.group(1)
        for name in sorted(set(re.findall(r'= "([^"]+)"', m.group(2)))):
            words = None
            for line, w in gen_rdna2_ops.CORPUS:
                mn = re.sub(r"_e(32|64)$", "", line.split()[0])
                if mn == name and ENC_OK[table](w[0]):
                    words = w
                    break
            if words is None and "_cd" in name:  # coarse derivatives: try the address sizes
                for a in ("v[4:9]", "v[4:10]", "v[4:11]", "v[4:12]"):
                    words = amdgpu_asm.assemble(["%s v[0:3], %s, s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D" % (name, a)])[0]
                    if words:
                        break
            if name.startswith("image_bvh"):
                words = amdgpu_asm.assemble(["%s v[4:7], v[0:%d], s[16:19]" % (name, 11 if "64" in name else 10)])[0]
            if name == "image_get_lod":  # both results (the lod forms read two components)
                words = amdgpu_asm.assemble(["image_get_lod v[0:1], v[4:5], s[8:15], s[16:19] dmask:0x3 dim:SQ_RSRC_IMG_2D"])[0]
            if table == "mimg" and name.startswith("image_atomic_"):  # one dword (two for the compare-swaps)
                cmp = "cmpswap" in name
                words = amdgpu_asm.assemble(["%s %s, v[4:5], s[8:15] dmask:0x%d dim:SQ_RSRC_IMG_2D glc" %
                                             (name, "v[0:1]" if cmp else "v0", 3 if cmp else 1)])[0]
            if words is None and table == "mimg" and name.endswith("_a"):
                # PS5 opcodes (the 0x80 bit: bit 0 of the first word) of a known sample form
                base = name[:-2]
                for line, w in gen_rdna2_ops.CORPUS:
                    if line.split()[0] == base and ENC_OK[table](w[0]):
                        words = [w[0] | 1] + list(w[1:])
                        break
            if words is None and name in ("ds_write_addtid_b32", "ds_read_addtid_b32"):
                op = 0xB0 if name.startswith("ds_write") else 0xB1  # data v1 / destination v0, no address
                words = [0xD8000000 | (op << 18), 0x00000100 if op == 0xB0 else 0]
            if words is None:
                print('{"%s", "%s", 0, {0}},' % (table, name))
            else:
                print('{"%s", "%s", %d, {%s}},' % (table, name, len(words), ", ".join("0x%08xu" % x for x in words)))


if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "--coverage":
    coverage()
