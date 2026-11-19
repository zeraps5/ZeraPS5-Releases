#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generates src/gpu/rdna2_ops.inc: PS5 GPU opcode -> mnemonic tables.

Every candidate mnemonic below is assembled with clang's AMDGPU assembler
(tools/amdgpu_asm.py) and its opcode read back from the encoding, so the
tables hold only what the reference assembler confirms. The PS5 GPU runs the
GFX10.1 ISA (gfx1013: keeps v_mac_f32/v_mad_f32); opcodes gfx1013 does not
define are filled in from gfx1030.

    python tools/gen_rdna2_ops.py > src/gpu/rdna2_ops.inc
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import amdgpu_asm  # noqa: E402
from amdgpu_asm import assemble  # noqa: E402

SOP2 = """s_add_u32 s_sub_u32 s_add_i32 s_sub_i32 s_addc_u32 s_subb_u32 s_min_i32 s_min_u32 s_max_i32 s_max_u32
s_cselect_b32 s_cselect_b64 s_and_b32 s_and_b64 s_or_b32 s_or_b64 s_xor_b32 s_xor_b64 s_andn2_b32 s_andn2_b64
s_orn2_b32 s_orn2_b64 s_nand_b32 s_nand_b64 s_nor_b32 s_nor_b64 s_xnor_b32 s_xnor_b64 s_lshl_b32 s_lshl_b64
s_lshr_b32 s_lshr_b64 s_ashr_i32 s_ashr_i64 s_bfm_b32 s_bfm_b64 s_mul_i32 s_bfe_u32 s_bfe_i32 s_bfe_u64 s_bfe_i64
s_absdiff_i32 s_lshl1_add_u32 s_lshl2_add_u32 s_lshl3_add_u32 s_lshl4_add_u32 s_pack_ll_b32_b16
s_pack_lh_b32_b16 s_pack_hh_b32_b16 s_mul_hi_u32 s_mul_hi_i32"""

SOPK = """s_movk_i32 s_version s_cmovk_i32 s_cmpk_eq_i32 s_cmpk_lg_i32 s_cmpk_gt_i32 s_cmpk_ge_i32 s_cmpk_lt_i32
s_cmpk_le_i32 s_cmpk_eq_u32 s_cmpk_lg_u32 s_cmpk_gt_u32 s_cmpk_ge_u32 s_cmpk_lt_u32 s_cmpk_le_u32 s_addk_i32
s_mulk_i32 s_getreg_b32 s_setreg_b32 s_setreg_imm32_b32 s_call_b64 s_waitcnt_vscnt s_waitcnt_vmcnt
s_waitcnt_expcnt s_waitcnt_lgkmcnt s_subvector_loop_begin s_subvector_loop_end"""

SOP1 = """s_mov_b32 s_mov_b64 s_cmov_b32 s_cmov_b64 s_not_b32 s_not_b64 s_wqm_b32 s_wqm_b64 s_brev_b32 s_brev_b64
s_bcnt0_i32_b32 s_bcnt0_i32_b64 s_bcnt1_i32_b32 s_bcnt1_i32_b64 s_ff0_i32_b32 s_ff0_i32_b64 s_ff1_i32_b32
s_ff1_i32_b64 s_flbit_i32_b32 s_flbit_i32_b64 s_flbit_i32 s_flbit_i32_i64 s_sext_i32_i8 s_sext_i32_i16
s_bitset0_b32 s_bitset0_b64 s_bitset1_b32 s_bitset1_b64 s_getpc_b64 s_setpc_b64 s_swappc_b64 s_rfe_b64
s_and_saveexec_b64 s_or_saveexec_b64 s_xor_saveexec_b64 s_andn2_saveexec_b64 s_orn2_saveexec_b64
s_nand_saveexec_b64 s_nor_saveexec_b64 s_xnor_saveexec_b64 s_quadmask_b32 s_quadmask_b64 s_movrels_b32
s_movrels_b64 s_movreld_b32 s_movreld_b64 s_abs_i32 s_andn1_saveexec_b64 s_orn1_saveexec_b64
s_andn1_wrexec_b64 s_andn2_wrexec_b64 s_bitreplicate_b64_b32 s_and_saveexec_b32 s_or_saveexec_b32
s_xor_saveexec_b32 s_andn2_saveexec_b32 s_orn2_saveexec_b32 s_nand_saveexec_b32 s_nor_saveexec_b32
s_xnor_saveexec_b32 s_andn1_saveexec_b32 s_orn1_saveexec_b32 s_andn1_wrexec_b32 s_andn2_wrexec_b32
s_movrelsd_2_b32"""

SOPC = """s_cmp_eq_i32 s_cmp_lg_i32 s_cmp_gt_i32 s_cmp_ge_i32 s_cmp_lt_i32 s_cmp_le_i32 s_cmp_eq_u32 s_cmp_lg_u32
s_cmp_gt_u32 s_cmp_ge_u32 s_cmp_lt_u32 s_cmp_le_u32 s_bitcmp0_b32 s_bitcmp1_b32 s_bitcmp0_b64 s_bitcmp1_b64
s_cmp_eq_u64 s_cmp_lg_u64"""

SOPP = """s_nop s_endpgm s_branch s_wakeup s_cbranch_scc0 s_cbranch_scc1 s_cbranch_vccz s_cbranch_vccnz
s_cbranch_execz s_cbranch_execnz s_barrier s_setkill s_waitcnt s_sethalt s_sleep s_setprio s_sendmsg
s_sendmsghalt s_trap s_icache_inv s_incperflevel s_decperflevel s_ttracedata s_cbranch_cdbgsys
s_cbranch_cdbguser s_cbranch_cdbgsys_or_user s_cbranch_cdbgsys_and_user s_endpgm_saved s_endpgm_ordered_ps_done
s_code_end s_inst_prefetch s_clause s_wait_idle s_waitcnt_depctr s_round_mode s_denorm_mode s_ttracedata_imm"""

SMEM = """s_load_dword s_load_dwordx2 s_load_dwordx4 s_load_dwordx8 s_load_dwordx16 s_buffer_load_dword
s_buffer_load_dwordx2 s_buffer_load_dwordx4 s_buffer_load_dwordx8 s_buffer_load_dwordx16 s_gl1_inv s_dcache_inv
s_memtime s_memrealtime s_atc_probe s_atc_probe_buffer"""

VOP2 = """v_cndmask_b32 v_dot2c_f32_f16 v_mac_legacy_f32 v_add_f32 v_sub_f32 v_subrev_f32 v_fmac_legacy_f32 v_mul_legacy_f32
v_mul_f32 v_mul_i32_i24 v_mul_hi_i32_i24 v_mul_u32_u24 v_mul_hi_u32_u24 v_dot4c_i32_i8 v_min_f32 v_max_f32
v_min_i32 v_max_i32 v_min_u32 v_max_u32 v_lshrrev_b32 v_ashrrev_i32 v_lshlrev_b32 v_and_b32 v_or_b32 v_xor_b32
v_xnor_b32 v_mac_f32 v_madmk_f32 v_madak_f32 v_add_nc_u32 v_sub_nc_u32 v_subrev_nc_u32 v_add_co_ci_u32
v_sub_co_ci_u32 v_subrev_co_ci_u32 v_fmac_f32 v_fmamk_f32 v_fmaak_f32 v_cvt_pkrtz_f16_f32 v_add_f16 v_sub_f16
v_subrev_f16 v_mul_f16 v_fmac_f16 v_fmamk_f16 v_fmaak_f16 v_max_f16 v_min_f16 v_ldexp_f16 v_pk_fmac_f16"""

VOP1 = """v_nop v_mov_b32 v_readfirstlane_b32 v_cvt_i32_f64 v_cvt_f64_i32 v_cvt_f32_i32 v_cvt_f32_u32 v_cvt_u32_f32
v_cvt_i32_f32 v_cvt_f16_f32 v_cvt_f32_f16 v_cvt_rpi_i32_f32 v_cvt_flr_i32_f32 v_cvt_off_f32_i4 v_cvt_f32_f64
v_cvt_f64_f32 v_cvt_f32_ubyte0 v_cvt_f32_ubyte1 v_cvt_f32_ubyte2 v_cvt_f32_ubyte3 v_cvt_u32_f64 v_cvt_f64_u32
v_trunc_f64 v_ceil_f64 v_rndne_f64 v_floor_f64 v_pipeflush v_fract_f32 v_trunc_f32 v_ceil_f32 v_rndne_f32
v_floor_f32 v_exp_f32 v_log_f32 v_rcp_f32 v_rcp_iflag_f32 v_rsq_f32 v_rcp_f64 v_rsq_f64 v_sqrt_f32 v_sqrt_f64
v_sin_f32 v_cos_f32 v_not_b32 v_bfrev_b32 v_ffbh_u32 v_ffbl_b32 v_ffbh_i32 v_frexp_exp_i32_f64
v_frexp_mant_f64 v_fract_f64 v_frexp_exp_i32_f32 v_frexp_mant_f32 v_clrexcp v_movreld_b32 v_movrels_b32
v_movrelsd_b32 v_movrelsd_2_b32 v_cvt_f16_u16 v_cvt_f16_i16 v_cvt_u16_f16 v_cvt_i16_f16 v_rcp_f16 v_sqrt_f16
v_rsq_f16 v_log_f16 v_exp_f16 v_frexp_mant_f16 v_frexp_exp_i16_f16 v_floor_f16 v_ceil_f16 v_trunc_f16
v_rndne_f16 v_fract_f16 v_sin_f16 v_cos_f16 v_sat_pk_u8_i16 v_cvt_norm_i16_f16 v_cvt_norm_u16_f16 v_swap_b32
v_swaprel_b32"""

VOPC_TYPES = ["f16", "f32", "f64", "i16", "i32", "i64", "u16", "u32", "u64"]
VOPC_F = ["f", "lt", "eq", "le", "gt", "lg", "ge", "o", "u", "nge", "nlg", "ngt", "nle", "neq", "nlt", "tru"]
VOPC_I = ["f", "lt", "eq", "le", "gt", "ne", "ge", "t"]

VOP3_ONLY = """v_mad_legacy_f32 v_mad_f32 v_mad_i32_i24 v_mad_u32_u24 v_cubeid_f32 v_cubesc_f32 v_cubetc_f32 v_cubema_f32
v_bfe_u32 v_bfe_i32 v_bfi_b32 v_fma_f32 v_fma_f64 v_lerp_u8 v_alignbit_b32 v_alignbyte_b32 v_mullit_f32
v_min3_f32 v_min3_i32 v_min3_u32 v_max3_f32 v_max3_i32 v_max3_u32 v_med3_f32 v_med3_i32 v_med3_u32 v_sad_u8
v_sad_hi_u8 v_sad_u16 v_sad_u32 v_cvt_pk_u8_f32 v_div_fixup_f32 v_div_fixup_f64 v_add_f64 v_mul_f64 v_min_f64
v_max_f64 v_ldexp_f64 v_mul_lo_u32 v_mul_hi_u32 v_mul_hi_i32 v_div_scale_f32 v_div_scale_f64 v_div_fmas_f32
v_div_fmas_f64 v_msad_u8 v_qsad_pk_u16_u8 v_mqsad_pk_u16_u8 v_trig_preop_f64 v_mqsad_u32_u8 v_mad_u64_u32
v_mad_i64_i32 v_xor3_b32 v_lshlrev_b64 v_lshrrev_b64 v_ashrrev_i64 v_add_nc_u16 v_sub_nc_u16 v_mul_lo_u16
v_lshrrev_b16 v_ashrrev_i16 v_max_u16 v_max_i16 v_min_u16 v_min_i16 v_add_nc_i16 v_sub_nc_i16
v_add_co_u32 v_sub_co_u32 v_subrev_co_u32 v_pack_b32_f16 v_cvt_pknorm_i16_f16 v_cvt_pknorm_u16_f16
v_lshlrev_b16 v_mad_u16 v_interp_p1ll_f16 v_interp_p1lv_f16 v_perm_b32 v_xad_u32 v_lshl_add_u32
v_add_lshl_u32 v_fma_f16 v_min3_f16 v_min3_i16 v_min3_u16 v_max3_f16 v_max3_i16 v_max3_u16 v_med3_f16
v_med3_i16 v_med3_u16 v_interp_p2_f16 v_mad_i16 v_div_fixup_f16 v_readlane_b32 v_writelane_b32 v_ldexp_f32
v_bfm_b32 v_bcnt_u32_b32 v_mbcnt_lo_u32_b32 v_mbcnt_hi_u32_b32 v_cvt_pknorm_i16_f32 v_cvt_pknorm_u16_f32
v_cvt_pk_u16_u32 v_cvt_pk_i16_i32 v_add3_u32 v_lshl_or_b32 v_and_or_b32 v_or3_b32 v_mad_u32_u16
v_mad_i32_i16 v_sub_nc_i32 v_add_nc_i32 v_permlane16_b32 v_permlanex16_b32 v_interp_p1_f32 v_interp_p2_f32
v_interp_mov_f32 v_cvt_pkaccum_u8_f32"""

VOP3P = """v_pk_mad_i16 v_pk_mul_lo_u16 v_pk_add_i16 v_pk_sub_i16 v_pk_lshlrev_b16 v_pk_lshrrev_b16
v_pk_ashrrev_i16 v_pk_max_i16 v_pk_min_i16 v_pk_mad_u16 v_pk_add_u16 v_pk_sub_u16 v_pk_max_u16 v_pk_min_u16
v_pk_fma_f16 v_pk_add_f16 v_pk_mul_f16 v_pk_min_f16 v_pk_max_f16 v_fma_mix_f32 v_fma_mixlo_f16
v_fma_mixhi_f16 v_dot2_f32_f16 v_dot2_i32_i16 v_dot2_u32_u16 v_dot4_i32_i8 v_dot4_u32_u8 v_dot8_i32_i4
v_dot8_u32_u4"""

DS = """ds_add_u32 ds_sub_u32 ds_rsub_u32 ds_inc_u32 ds_dec_u32 ds_min_i32 ds_max_i32 ds_min_u32 ds_max_u32 ds_and_b32
ds_or_b32 ds_xor_b32 ds_mskor_b32 ds_write_b32 ds_write2_b32 ds_write2st64_b32 ds_cmpst_b32 ds_min_f32 ds_max_f32
ds_nop ds_add_f32 ds_write_b8 ds_write_b16 ds_add_rtn_u32 ds_sub_rtn_u32 ds_rsub_rtn_u32 ds_inc_rtn_u32
ds_dec_rtn_u32 ds_min_rtn_i32 ds_max_rtn_i32 ds_min_rtn_u32 ds_max_rtn_u32 ds_and_rtn_b32 ds_or_rtn_b32
ds_xor_rtn_b32 ds_mskor_rtn_b32 ds_wrxchg_rtn_b32 ds_wrxchg2_rtn_b32 ds_wrxchg2st64_rtn_b32 ds_cmpst_rtn_b32
ds_min_rtn_f32 ds_max_rtn_f32 ds_wrap_rtn_b32 ds_add_rtn_f32 ds_read_b32 ds_read2_b32 ds_read2st64_b32
ds_read_i8 ds_read_u8 ds_read_i16 ds_read_u16 ds_swizzle_b32 ds_permute_b32 ds_bpermute_b32 ds_add_u64
ds_sub_u64 ds_write_b64 ds_write2_b64 ds_write2st64_b64 ds_read_b64 ds_read2_b64 ds_read2st64_b64 ds_write_b96 ds_write_b128 ds_read_b96
ds_read_b128 ds_gws_init ds_gws_barrier ds_append ds_consume ds_ordered_count ds_read_u8_d16 ds_read_u8_d16_hi
ds_read_i8_d16 ds_read_i8_d16_hi ds_read_u16_d16 ds_read_u16_d16_hi ds_write_b8_d16_hi ds_write_b16_d16_hi"""

MUBUF = """buffer_load_format_x buffer_load_format_xy buffer_load_format_xyz buffer_load_format_xyzw
buffer_store_format_x buffer_store_format_xy buffer_store_format_xyz buffer_store_format_xyzw buffer_load_ubyte
buffer_load_sbyte buffer_load_ushort buffer_load_sshort buffer_load_dword buffer_load_dwordx2
buffer_load_dwordx4 buffer_load_dwordx3 buffer_store_byte buffer_store_byte_d16_hi buffer_store_short
buffer_store_short_d16_hi buffer_store_dword buffer_store_dwordx2 buffer_store_dwordx4 buffer_store_dwordx3
buffer_load_ubyte_d16 buffer_load_ubyte_d16_hi buffer_load_sbyte_d16 buffer_load_sbyte_d16_hi
buffer_load_short_d16 buffer_load_short_d16_hi buffer_load_format_d16_hi_x buffer_store_format_d16_hi_x
buffer_atomic_swap buffer_atomic_cmpswap buffer_atomic_add buffer_atomic_sub buffer_atomic_smin
buffer_atomic_umin buffer_atomic_smax buffer_atomic_umax buffer_atomic_and buffer_atomic_or buffer_atomic_xor
buffer_atomic_inc buffer_atomic_dec buffer_atomic_fcmpswap buffer_atomic_fmin buffer_atomic_fmax
buffer_atomic_swap_x2 buffer_atomic_cmpswap_x2 buffer_atomic_add_x2 buffer_atomic_sub_x2
buffer_atomic_smin_x2 buffer_atomic_umin_x2 buffer_atomic_smax_x2 buffer_atomic_umax_x2 buffer_atomic_and_x2
buffer_atomic_or_x2 buffer_atomic_xor_x2 buffer_atomic_inc_x2 buffer_atomic_dec_x2 buffer_atomic_fcmpswap_x2
buffer_atomic_fmin_x2 buffer_atomic_fmax_x2
buffer_gl0_inv buffer_gl1_inv buffer_load_format_d16_x buffer_load_format_d16_xy buffer_load_format_d16_xyz
buffer_load_format_d16_xyzw buffer_store_format_d16_x buffer_store_format_d16_xy buffer_store_format_d16_xyz
buffer_store_format_d16_xyzw"""

MTBUF = """tbuffer_load_format_x tbuffer_load_format_xy tbuffer_load_format_xyz tbuffer_load_format_xyzw
tbuffer_store_format_x tbuffer_store_format_xy tbuffer_store_format_xyz tbuffer_store_format_xyzw
tbuffer_load_format_d16_x tbuffer_load_format_d16_xy tbuffer_load_format_d16_xyz tbuffer_load_format_d16_xyzw
tbuffer_store_format_d16_x tbuffer_store_format_d16_xy tbuffer_store_format_d16_xyz tbuffer_store_format_d16_xyzw"""

MIMG = """image_load image_load_mip image_load_pck image_load_pck_sgn image_load_mip_pck image_load_mip_pck_sgn
image_store image_store_mip image_store_pck image_store_mip_pck image_get_resinfo image_atomic_swap
image_atomic_cmpswap image_atomic_add image_atomic_sub image_atomic_smin image_atomic_umin image_atomic_smax
image_atomic_umax image_atomic_and image_atomic_or image_atomic_xor image_atomic_inc image_atomic_dec
image_sample image_sample_cl image_sample_d image_sample_d_cl image_sample_l image_sample_b image_sample_b_cl
image_sample_lz image_sample_c image_sample_c_cl image_sample_c_d image_sample_c_d_cl image_sample_c_l
image_sample_c_b image_sample_c_b_cl image_sample_c_lz image_sample_o image_sample_cl_o image_sample_d_o
image_sample_d_cl_o image_sample_l_o image_sample_b_o image_sample_b_cl_o image_sample_lz_o image_sample_c_o
image_sample_c_cl_o image_sample_c_d_o image_sample_c_d_cl_o image_sample_c_l_o image_sample_c_b_o
image_sample_c_b_cl_o image_sample_c_lz_o image_gather4 image_gather4_cl image_gather4_l image_gather4_b
image_gather4_b_cl image_gather4_lz image_gather4_c image_gather4_c_cl image_gather4_c_l image_gather4_c_b
image_gather4_c_b_cl image_gather4_c_lz image_gather4_o image_gather4_cl_o image_gather4_l_o
image_gather4_b_o image_gather4_b_cl_o image_gather4_lz_o image_gather4_c_o image_gather4_c_cl_o
image_gather4_c_l_o image_gather4_c_b_o image_gather4_c_b_cl_o image_gather4_c_lz_o image_get_lod
image_sample_d_g16 image_sample_d_cl_g16 image_sample_c_d_g16 image_sample_c_d_cl_g16 image_sample_d_o_g16
image_sample_d_cl_o_g16 image_sample_c_d_o_g16 image_sample_c_d_cl_o_g16 image_gather4h image_msaa_load
image_atomic_fcmpswap image_atomic_fmin image_atomic_fmax"""

GLOBAL = """global_load_ubyte global_load_sbyte global_load_ushort global_load_sshort global_load_dword
global_load_dwordx2 global_load_dwordx4 global_load_dwordx3 global_store_byte global_store_byte_d16_hi
global_store_short global_store_short_d16_hi global_store_dword global_store_dwordx2 global_store_dwordx4
global_store_dwordx3 global_load_ubyte_d16 global_load_ubyte_d16_hi global_load_sbyte_d16
global_load_sbyte_d16_hi global_load_short_d16 global_load_short_d16_hi global_atomic_swap
global_atomic_cmpswap global_atomic_add global_atomic_sub global_atomic_smin global_atomic_umin
global_atomic_smax global_atomic_umax global_atomic_and global_atomic_or global_atomic_xor global_atomic_inc
global_atomic_dec global_atomic_fcmpswap global_atomic_fmin global_atomic_fmax global_atomic_swap_x2
global_atomic_cmpswap_x2 global_atomic_add_x2 global_atomic_sub_x2 global_atomic_smin_x2
global_atomic_umin_x2 global_atomic_smax_x2 global_atomic_umax_x2 global_atomic_and_x2 global_atomic_or_x2
global_atomic_xor_x2 global_atomic_inc_x2 global_atomic_dec_x2 global_load_dword_addtid
global_store_dword_addtid"""


def words(s):
    return s.split()


def is64(name):
    return name.endswith("_b64") or name.endswith("_u64") or name.endswith("_i64") or "saveexec_b64" in name \
        or "wrexec_b64" in name or name in ("s_getpc_b64", "s_setpc_b64", "s_swappc_b64", "s_rfe_b64")


def probe(cands):
    """cands: list of (name, [asm variants]) -> {name: dwords of first variant that assembles}"""
    lines, owners = [], []
    for name, variants in cands:
        for v in variants:
            lines.append(v)
            owners.append(name)
    res = assemble(lines)
    found = {}
    for name, line, w in zip(owners, lines, res):
        if w is not None and name not in found:
            found[name] = w
            CORPUS.append((line, w))
    return found


CORPUS = []  # (text, words) of every instruction that assembled


def sreg(n, wide):
    return "s[%d:%d]" % (n, n + 1) if wide else "s%d" % n


def sop2():
    c = []
    for n in words(SOP2):
        v = ["%s s0, s1, s2" % n, "%s s[0:1], s[2:3], s[4:5]" % n, "%s s[0:1], s[2:3], s4" % n,
             "%s s[0:1], s2, s3" % n]
        c.append((n, v))
    return {n: (w[0] >> 23) & 0x7F for n, w in probe(c).items()}


def sopk():
    c = [(n, ["%s s0, 0x10" % n, "%s 0x10" % n, "%s s[0:1], 0x10" % n, "%s s0, hwreg(HW_REG_MODE)" % n,
              "%s hwreg(HW_REG_MODE), s0" % n, "%s hwreg(HW_REG_MODE), 0x1" % n, "%s null, 0x10" % n,
              "%s 0x10, 0x10" % n]) for n in words(SOPK)]
    return {n: (w[0] >> 23) & 0x1F for n, w in probe(c).items()}


def sop1():
    c = []
    for n in words(SOP1):
        c.append((n, ["%s s0, s1" % n, "%s s[0:1], s[2:3]" % n, "%s s0, s[2:3]" % n, "%s s[0:1], s2" % n,
                      "%s s[0:1]" % n, "%s s0" % n]))
    return {n: (w[0] >> 8) & 0xFF for n, w in probe(c).items()}


def sopc():
    c = [(n, ["%s s0, s1" % n, "%s s[0:1], s[2:3]" % n, "%s s[0:1], s2" % n]) for n in words(SOPC)]
    return {n: (w[0] >> 16) & 0x7F for n, w in probe(c).items()}


def sopp():
    c = [(n, ["%s" % n, "%s 0" % n, "%s 1" % n, "%s 0x1" % n, "%s sendmsg(MSG_INTERRUPT)" % n]) for n in words(SOPP)]
    return {n: (w[0] >> 16) & 0x7F for n, w in probe(c).items()}


def smem():
    c = []
    for n in words(SMEM):
        v = []
        for k, dst in (("x16", "s[0:15]"), ("x8", "s[0:7]"), ("x4", "s[0:3]"), ("x2", "s[0:1]"), ("", "s0")):
            if n.endswith("dword" + k):
                base = "s[4:7]" if "buffer" in n else "s[4:5]"
                v.append("%s %s, %s, 0x10" % (n, dst, base))
        v += ["%s" % n, "%s s[0:1]" % n, "%s 7, s[4:5], 0x10" % n, "%s 7, s[4:7], 0x10" % n]
        c.append((n, v))
    return {n: (w[0] >> 18) & 0xFF for n, w in probe(c).items()}


def vop2():
    c = [(n, ["%s_e32 v0, v1, v2" % n, "%s_e32 v0, vcc_lo, v1, v2, vcc_lo" % n, "%s_e32 v0, v1, v2, vcc_lo" % n,
              "%s v0, v1, 0x40, v2" % n, "%s v0, v1, v2, 0x40" % n, "%s_e32 v0, vcc_lo, v1, v2" % n])
         for n in words(VOP2)]
    res = {}
    for n, w in probe(c).items():
        if (w[0] >> 31) == 0:
            res[n] = (w[0] >> 25) & 0x3F
    return res


def vop1():
    c = [(n, ["%s_e32 v0, v1" % n, "%s_e32 v[0:1], v[2:3]" % n, "%s_e32 v0, v[2:3]" % n,
              "%s_e32 v[0:1], v2" % n, "%s_e32" % n, "%s s0, v1" % n, "%s" % n]) for n in words(VOP1)]
    res = {}
    for n, w in probe(c).items():
        if (w[0] >> 25) == 0x3F:
            res[n] = (w[0] >> 9) & 0xFF
    return res


def vopc():
    c = []
    for t in VOPC_TYPES:
        conds = VOPC_F if t.startswith("f") else VOPC_I
        wide = t.endswith("64")
        a = "v[2:3], v[4:5]" if wide else "v1, v2"
        for pre in ("v_cmp_", "v_cmpx_"):
            for cond in conds:
                n = "%s%s_%s" % (pre, cond, t)
                c.append((n, ["%s_e32 vcc_lo, %s" % (n, a), "%s_e32 %s" % (n, a)]))
        for pre in ("v_cmp_class_", "v_cmpx_class_"):
            if t in ("f16", "f32", "f64"):
                n = pre + t
                c.append((n, ["%s_e32 vcc_lo, %s" % (n, "v[2:3], v4" if wide else "v1, v2"),
                              "%s_e32 %s" % (n, "v[2:3], v4" if wide else "v1, v2")]))
    res = {}
    for n, w in probe(c).items():
        if (w[0] >> 25) == 0x3E:
            res[n] = (w[0] >> 17) & 0xFF
    return res


def vop3(extra_names):
    c = []
    for n in words(VOP3_ONLY) + extra_names:
        v = ["%s v0, v1, v2, v3" % n, "%s v0, v1, v2" % n, "%s v0, v1" % n, "%s v[0:1], v[2:3], v[4:5], v[6:7]" % n,
             "%s v[0:1], v[2:3], v[4:5]" % n, "%s v[0:1], v2, v[4:5]" % n, "%s v[0:1], s[0:1], v2, v3, v[4:5]" % n,
             "%s v0, s0, v1, v2" % n, "%s v0, vcc_lo, v1, v2" % n, "%s v0, s0, v1, v2, s1" % n,
             "%s v0, vcc_lo, v1, v2, vcc_lo" % n, "%s v0, v1, attr0.x" % n, "%s v0, v1, attr0.x high" % n,
             "%s v0, p10, attr0.x" % n, "%s s0, v1, s2" % n, "%s v0, s1, s2" % n,
             "%s v[0:1], v[2:3], v4" % n, "%s v0, v1, v2, s0" % n, "%s_e64 v0, v1, v2" % n, "%s_e64 v0, v1" % n,
             "%s v0, v1, s0, s1" % n, "%s v0, v1, v2, s[0:1]" % n, "%s v0, v1, v2, vcc_lo" % n,
             "%s v[0:1], s0, v2, v3, v[4:5]" % n, "%s v[0:1], vcc_lo, v[2:3], v[4:5], v[6:7]" % n,
             "%s v0, v1, attr0.x, v2" % n, "%s v0, v1, attr0.x, v2 high" % n, "%s v0, v1, v2, v3, v4" % n]
        c.append((n, v))
    res = {}
    for n, w in probe(c).items():
        if (w[0] >> 26) == 0x35:
            res[n] = (w[0] >> 16) & 0x3FF
    return res


def vop3p():
    c = [(n, ["%s v0, v1, v2, v3" % n, "%s v0, v1, v2" % n]) for n in words(VOP3P)]
    res = {}
    for n, w in probe(c).items():
        if (w[0] >> 24) == 0xCC:
            res[n] = (w[0] >> 16) & 0x7F
    return res


def ds():
    c = [(n, ["%s v0, v1" % n, "%s v1, v2" % n, "%s v0, v1, v2" % n, "%s v1, v2, v3" % n,
              "%s v0, v1, v2, v3" % n, "%s v[0:1], v2" % n, "%s v2, v[0:1]" % n, "%s v[0:1], v2, v[4:5]" % n,
              "%s v[0:3], v2" % n, "%s v2, v[4:7]" % n, "%s v[0:2], v2" % n, "%s v2, v[4:6]" % n,
              "%s v2, v[4:5], v[6:7]" % n, "%s v[0:3], v2 offset1:1" % n, "%s v2, v4, v5 offset1:1" % n,
              "%s v0, v1, v2, v3 offset1:1" % n, "%s v0" % n, "%s v0 offset:0" % n, "%s" % n,
              "%s v0, v2 offset:4" % n, "%s v[0:1], v2 offset1:1" % n, "%s v0 gds" % n, "%s v0, v1 gds" % n,
              "%s offset:4 gds" % n]) for n in words(DS)]
    return {n: (w[0] >> 18) & 0xFF for n, w in probe(c).items() if (w[0] >> 26) == 0x36}


def mubuf():
    c = []
    for n in words(MUBUF):
        v = []
        for d in ("v1", "v[0:1]", "v[0:2]", "v[0:3]"):
            v.append("%s %s, v4, s[4:7], 0 offen" % (n, d))
        v += ["%s" % n]
        c.append((n, v))
    res = {}
    for n, w in probe(c).items():
        if (w[0] >> 26) == 0x38:
            res[n] = ((w[0] >> 18) & 0x7F) | (((w[0] >> 25) & 1) << 7)
    return res


def mtbuf():
    c = []
    for n in words(MTBUF):
        v = ["%s %s, v4, s[4:7], 0 format:[BUF_FMT_32_FLOAT] offen" % (n, d) for d in ("v1", "v[0:1]", "v[0:2]", "v[0:3]")]
        c.append((n, v))
    res = {}
    for n, w in probe(c).items():
        if (w[0] >> 26) == 0x3A:
            res[n] = ((w[0] >> 16) & 7) | (((w[1] >> 21) & 1) << 3)
    return res


def mimg():
    c = []
    for n in words(MIMG):
        v = []
        for d in ("v[0:3]", "v0", "v[0:1]"):
            for a in ("v[4:5]", "v[4:7]", "v[4:6]", "v4", "v[4:8]", "v[4:9]", "v[4:11]"):
                v.append("%s %s, %s, s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D" % (n, d, a))
                v.append("%s %s, %s, s[8:15] dmask:0xf dim:SQ_RSRC_IMG_2D" % (n, d, a))
                v.append("%s %s, %s, s[8:15] dmask:0x1 dim:SQ_RSRC_IMG_2D" % (n, d, a))
                v.append("%s %s, %s, s[8:15] dmask:0x1 dim:SQ_RSRC_IMG_2D glc" % (n, d, a))
                v.append("%s %s, %s, s[8:15], s[16:19] dmask:0x1 dim:SQ_RSRC_IMG_2D" % (n, d, a))
                v.append("%s %s, %s, s[8:15] dmask:0x3 dim:SQ_RSRC_IMG_2D glc" % (n, d, a))
        c.append((n, v))
    res = {}
    for n, w in probe(c).items():
        if (w[0] >> 26) == 0x3C:
            res[n] = ((w[0] >> 18) & 0x7F) | (((w[0] >> 25) & 1) << 7)
    return res


def glob():
    c = []
    for n in words(GLOBAL):
        v = ["%s %s, v[4:5], off" % (n, d) for d in ("v1", "v[0:1]", "v[0:2]", "v[0:3]")]
        v += ["%s v[4:5], %s, off" % (n, d) for d in ("v1", "v[0:1]", "v[0:2]", "v[0:3]")]
        v += ["%s v1, v[4:5], %s, off glc" % (n, d) for d in ("v2", "v[2:3]")]
        v += ["%s v[0:1], v[4:5], v[2:3], off glc" % n, "%s v[0:1], v[4:5], v[6:9], off glc" % n,
              "%s v1, v[4:5], v[2:3], off glc" % n, "%s v1, off" % n, "%s v1, s[0:1]" % n]
        c.append((n, v))
    res = {}
    for n, w in probe(c).items():
        if (w[0] >> 26) == 0x37:
            res[n] = (w[0] >> 18) & 0x7F
    return res


def emit_table(name, size, ops):
    table = [None] * size
    for n, op in ops.items():
        if op < size and table[op] is None:
            table[op] = n
    print("static const char *const %s[%d] = {" % (name, size))
    for op, n in enumerate(table):
        if n:
            print('    [0x%x] = "%s",' % (op, n))
    print("};\n")
    return table


TABLES = [("k_sop2", 128, "sop2"), ("k_sopk", 32, "sopk"), ("k_sop1", 256, "sop1"), ("k_sopc", 128, "sopc"),
          ("k_sopp", 128, "sopp"), ("k_smem", 256, "smem"), ("k_vop2", 64, "vop2"), ("k_vop1", 256, "vop1"),
          ("k_vopc", 256, "vopc"), ("k_vop3", 1024, "vop3"), ("k_vop3p", 128, "vop3p"), ("k_ds", 256, "ds"),
          ("k_mubuf", 256, "mubuf"), ("k_mtbuf", 16, "mtbuf"), ("k_mimg", 256, "mimg"), ("k_global", 128, "glob")]


def probe_all():
    res = {}
    v2, v1, vc = vop2(), vop1(), vopc()
    for name, size, fn in TABLES:
        if fn == "vop3":
            res[name] = vop3(list(v2) + list(v1) + list(vc))
        elif fn in ("vop2", "vop1", "vopc"):
            res[name] = {"vop2": v2, "vop1": v1, "vopc": vc}[fn]
        else:
            res[name] = globals()[fn]()
    return res


def main():
    amdgpu_asm.TARGET = "gfx1013"
    base = probe_all()
    amdgpu_asm.TARGET = "gfx1030"
    extra = probe_all()
    print("/* Generated by tools/gen_rdna2_ops.py from clang's gfx1013 and gfx1030 assemblers. Do not edit. */")
    print("/* clang-format off */")
    print()
    for name, size, _ in TABLES:
        ops = dict(base[name])
        taken = set(ops.values())
        for n, op in extra[name].items():
            if op not in taken and n not in ops:
                ops[n] = op
                taken.add(op)
        emit_table(name, size, ops)


if __name__ == "__main__":
    main()
