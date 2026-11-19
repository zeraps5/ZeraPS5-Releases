/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Internal operation ids: the translator and the scalar evaluator switch on
 * these instead of comparing mnemonics. sh_op_of() maps a decoded
 * instruction to its id (SH_OP_UNKNOWN when not implemented). */
#ifndef PS5C_SHADER_OPS_H
#define PS5C_SHADER_OPS_H

#include "gpu/rdna2.h"

#define SH_OPS(X)                                                                                                      \
    X(UNKNOWN, "")                                                                                                     \
    /* scalar ALU */                                                                                                   \
    X(S_MOV_B32, "s_mov_b32") X(S_MOV_B64, "s_mov_b64") X(S_CMOV_B32, "s_cmov_b32") X(S_MOVK_I32, "s_movk_i32")      \
    X(S_WQM_B32, "s_wqm_b32") X(S_WQM_B64, "s_wqm_b64") X(S_NOT_B32, "s_not_b32") X(S_NOT_B64, "s_not_b64") X(S_BREV_B32, "s_brev_b32")                                   \
    X(S_BCNT1_I32_B32, "s_bcnt1_i32_b32") X(S_BCNT1_I32_B64, "s_bcnt1_i32_b64") X(S_FF1_I32_B32, "s_ff1_i32_b32")      \
    X(S_FF1_I32_B64, "s_ff1_i32_b64") X(S_FLBIT_I32_B32, "s_flbit_i32_b32") X(S_FLBIT_I32_B64, "s_flbit_i32_b64")                                         \
    X(S_BITREPLICATE_B64_B32, "s_bitreplicate_b64_b32") X(S_SEXT_I32_I8, "s_sext_i32_i8")          \
    X(S_SEXT_I32_I16, "s_sext_i32_i16") X(S_ABS_I32, "s_abs_i32") X(S_GETPC_B64, "s_getpc_b64")                        \
    X(S_ADD_U32, "s_add_u32") X(S_SUB_U32, "s_sub_u32") X(S_ADD_I32, "s_add_i32") X(S_SUB_I32, "s_sub_i32")            \
    X(S_ADDC_U32, "s_addc_u32") X(S_SUBB_U32, "s_subb_u32") X(S_ADDK_I32, "s_addk_i32") X(S_MULK_I32, "s_mulk_i32")    \
    X(S_MIN_I32, "s_min_i32") X(S_MIN_U32, "s_min_u32") X(S_MAX_I32, "s_max_i32") X(S_MAX_U32, "s_max_u32")            \
    X(S_AND_B32, "s_and_b32") X(S_AND_B64, "s_and_b64") X(S_OR_B32, "s_or_b32") X(S_OR_B64, "s_or_b64")                \
    X(S_XOR_B32, "s_xor_b32") X(S_XOR_B64, "s_xor_b64") X(S_ANDN2_B32, "s_andn2_b32") X(S_ANDN2_B64, "s_andn2_b64")    \
    X(S_ORN2_B32, "s_orn2_b32") X(S_ORN2_B64, "s_orn2_b64") X(S_NAND_B32, "s_nand_b32") X(S_NOR_B32, "s_nor_b32")      \
    X(S_XNOR_B32, "s_xnor_b32") X(S_NAND_B64, "s_nand_b64") X(S_NOR_B64, "s_nor_b64") X(S_XNOR_B64, "s_xnor_b64") \
    X(S_LSHL_B32, "s_lshl_b32") X(S_LSHL_B64, "s_lshl_b64") X(S_LSHR_B32, "s_lshr_b32")    \
    X(S_LSHR_B64, "s_lshr_b64") X(S_ASHR_I32, "s_ashr_i32") X(S_BFM_B32, "s_bfm_b32") X(S_MUL_I32, "s_mul_i32")        \
    X(S_MUL_HI_U32, "s_mul_hi_u32") X(S_MUL_HI_I32, "s_mul_hi_i32") X(S_BFE_U32, "s_bfe_u32")                          \
    X(S_BFE_I32, "s_bfe_i32") X(S_BFE_U64, "s_bfe_u64") X(S_CSELECT_B32, "s_cselect_b32")                              \
    X(S_CSELECT_B64, "s_cselect_b64") X(S_ABSDIFF_I32, "s_absdiff_i32") X(S_LSHL1_ADD_U32, "s_lshl1_add_u32")          \
    X(S_LSHL2_ADD_U32, "s_lshl2_add_u32") X(S_LSHL3_ADD_U32, "s_lshl3_add_u32") X(S_LSHL4_ADD_U32, "s_lshl4_add_u32")  \
    X(S_PACK_LL_B32_B16, "s_pack_ll_b32_b16") X(S_PACK_LH_B32_B16, "s_pack_lh_b32_b16")                                \
    X(S_PACK_HH_B32_B16, "s_pack_hh_b32_b16")                                                                          \
    X(S_AND_SAVEEXEC_B32, "s_and_saveexec_b32") X(S_AND_SAVEEXEC_B64, "s_and_saveexec_b64")                            \
    X(S_OR_SAVEEXEC_B32, "s_or_saveexec_b32") X(S_OR_SAVEEXEC_B64, "s_or_saveexec_b64")                                \
    X(S_XOR_SAVEEXEC_B32, "s_xor_saveexec_b32") X(S_XOR_SAVEEXEC_B64, "s_xor_saveexec_b64")                            \
    X(S_ANDN2_SAVEEXEC_B32, "s_andn2_saveexec_b32") X(S_ANDN2_SAVEEXEC_B64, "s_andn2_saveexec_b64")                    \
    X(S_ORN2_SAVEEXEC_B32, "s_orn2_saveexec_b32") X(S_ORN2_SAVEEXEC_B64, "s_orn2_saveexec_b64")                        \
    X(S_NAND_SAVEEXEC_B32, "s_nand_saveexec_b32") X(S_NAND_SAVEEXEC_B64, "s_nand_saveexec_b64")                        \
    X(S_NOR_SAVEEXEC_B32, "s_nor_saveexec_b32") X(S_NOR_SAVEEXEC_B64, "s_nor_saveexec_b64")                            \
    X(S_XNOR_SAVEEXEC_B32, "s_xnor_saveexec_b32") X(S_XNOR_SAVEEXEC_B64, "s_xnor_saveexec_b64")                        \
    X(S_ANDN1_SAVEEXEC_B32, "s_andn1_saveexec_b32") X(S_ANDN1_SAVEEXEC_B64, "s_andn1_saveexec_b64")                    \
    X(S_ORN1_SAVEEXEC_B32, "s_orn1_saveexec_b32") X(S_ORN1_SAVEEXEC_B64, "s_orn1_saveexec_b64")                        \
    X(S_BITSET0_B32, "s_bitset0_b32") X(S_BITSET1_B32, "s_bitset1_b32")                                                \
    X(S_SETREG_B32, "s_setreg_b32") X(S_SETREG_IMM32_B32, "s_setreg_imm32_b32") X(S_GETREG_B32, "s_getreg_b32")        \
    X(S_CMP_EQ_I32, "s_cmp_eq_i32") X(S_CMP_LG_I32, "s_cmp_lg_i32") X(S_CMP_GT_I32, "s_cmp_gt_i32")                    \
    X(S_CMP_GE_I32, "s_cmp_ge_i32") X(S_CMP_LT_I32, "s_cmp_lt_i32") X(S_CMP_LE_I32, "s_cmp_le_i32")                    \
    X(S_CMP_EQ_U32, "s_cmp_eq_u32") X(S_CMP_LG_U32, "s_cmp_lg_u32") X(S_CMP_GT_U32, "s_cmp_gt_u32")                    \
    X(S_CMP_GE_U32, "s_cmp_ge_u32") X(S_CMP_LT_U32, "s_cmp_lt_u32") X(S_CMP_LE_U32, "s_cmp_le_u32")                    \
    X(S_CMP_EQ_U64, "s_cmp_eq_u64") X(S_CMP_LG_U64, "s_cmp_lg_u64") X(S_BITCMP0_B32, "s_bitcmp0_b32")                  \
    X(S_BITCMP1_B32, "s_bitcmp1_b32") X(S_CMPK_EQ_I32, "s_cmpk_eq_i32") X(S_CMPK_LG_I32, "s_cmpk_lg_i32")              \
    X(S_CMPK_EQ_U32, "s_cmpk_eq_u32") X(S_CMPK_LG_U32, "s_cmpk_lg_u32")                                                \
    X(S_CMPK_GT_I32, "s_cmpk_gt_i32") X(S_CMPK_GE_I32, "s_cmpk_ge_i32") X(S_CMPK_LT_I32, "s_cmpk_lt_i32")              \
    X(S_CMPK_LE_I32, "s_cmpk_le_i32") X(S_CMPK_GT_U32, "s_cmpk_gt_u32") X(S_CMPK_GE_U32, "s_cmpk_ge_u32")              \
    X(S_CMPK_LT_U32, "s_cmpk_lt_u32") X(S_CMPK_LE_U32, "s_cmpk_le_u32") X(S_QUADMASK_B32, "s_quadmask_b32")          \
    X(S_QUADMASK_B64, "s_quadmask_b64")                                                \
    /* program flow */                                                                                                 \
    X(S_SWAPPC_B64, "s_swappc_b64")                                                                                     \
    X(S_NOP, "s_nop") X(S_ENDPGM, "s_endpgm") X(S_BRANCH, "s_branch") X(S_CBRANCH_SCC0, "s_cbranch_scc0")              \
    X(S_CBRANCH_SCC1, "s_cbranch_scc1") X(S_CBRANCH_VCCZ, "s_cbranch_vccz") X(S_CBRANCH_VCCNZ, "s_cbranch_vccnz")      \
    X(S_CBRANCH_EXECZ, "s_cbranch_execz") X(S_CBRANCH_EXECNZ, "s_cbranch_execnz") X(S_WAITCNT, "s_waitcnt")            \
    X(S_WAITCNT_VSCNT, "s_waitcnt_vscnt") X(S_WAITCNT_VMCNT, "s_waitcnt_vmcnt")                                        \
    X(S_WAITCNT_LGKMCNT, "s_waitcnt_lgkmcnt") X(S_WAITCNT_EXPCNT, "s_waitcnt_expcnt")                                  \
    X(S_WAITCNT_DEPCTR, "s_waitcnt_depctr") X(S_BARRIER, "s_barrier") X(S_SENDMSG, "s_sendmsg")                        \
    X(S_INST_PREFETCH, "s_inst_prefetch") X(S_CLAUSE, "s_clause") X(S_SETPRIO, "s_setprio") X(S_SLEEP, "s_sleep") X(S_TRAP, "s_trap")      \
    X(S_CODE_END, "s_code_end") X(S_ICACHE_INV, "s_icache_inv") X(S_DENORM_MODE, "s_denorm_mode")                      \
    X(S_ROUND_MODE, "s_round_mode") X(S_TTRACEDATA, "s_ttracedata") X(S_DCACHE_INV, "s_dcache_inv")                    \
    X(S_GL1_INV, "s_gl1_inv") X(BUFFER_GL0_INV, "buffer_gl0_inv") X(BUFFER_GL1_INV, "buffer_gl1_inv")                  \
    /* scalar memory */                                                                                                \
    X(S_LOAD_DWORD, "s_load_dword") X(S_LOAD_DWORDX2, "s_load_dwordx2") X(S_LOAD_DWORDX4, "s_load_dwordx4")            \
    X(S_LOAD_DWORDX8, "s_load_dwordx8") X(S_LOAD_DWORDX16, "s_load_dwordx16")                                          \
    X(S_BUFFER_LOAD_DWORD, "s_buffer_load_dword") X(S_BUFFER_LOAD_DWORDX2, "s_buffer_load_dwordx2")                    \
    X(S_BUFFER_LOAD_DWORDX4, "s_buffer_load_dwordx4") X(S_BUFFER_LOAD_DWORDX8, "s_buffer_load_dwordx8")                \
    X(S_BUFFER_LOAD_DWORDX16, "s_buffer_load_dwordx16")                                                                \
    /* vector ALU */                                                                                                   \
    X(V_NOP, "v_nop") X(V_MOV_B32, "v_mov_b32") X(V_READFIRSTLANE_B32, "v_readfirstlane_b32")                          \
    X(V_CNDMASK_B32, "v_cndmask_b32") X(V_ADD_F32, "v_add_f32") X(V_SUB_F32, "v_sub_f32")                              \
    X(V_SUBREV_F32, "v_subrev_f32") X(V_MUL_F32, "v_mul_f32") X(V_MUL_LEGACY_F32, "v_mul_legacy_f32")                  \
    X(V_MIN_F32, "v_min_f32") X(V_MAX_F32, "v_max_f32") X(V_MAC_F32, "v_mac_f32") X(V_FMAC_F32, "v_fmac_f32")          \
    X(V_MAD_F32, "v_mad_f32") X(V_FMA_F32, "v_fma_f32") X(V_MADMK_F32, "v_madmk_f32") X(V_MADAK_F32, "v_madak_f32")    \
    X(V_FMAMK_F32, "v_fmamk_f32") X(V_FMAAK_F32, "v_fmaak_f32") X(V_MIN3_F32, "v_min3_f32")                            \
    X(V_MAX3_F32, "v_max3_f32") X(V_MED3_F32, "v_med3_f32") X(V_MUL_I32_I24, "v_mul_i32_i24")                          \
    X(V_MUL_U32_U24, "v_mul_u32_u24") X(V_MUL_LO_U32, "v_mul_lo_u32") X(V_MUL_HI_U32, "v_mul_hi_u32") X(V_MUL_HI_I32, "v_mul_hi_i32")                  \
    X(V_MAD_U32_U24, "v_mad_u32_u24") X(V_MAD_I32_I24, "v_mad_i32_i24") X(V_CVT_OFF_F32_I4, "v_cvt_off_f32_i4")        \
    X(V_READLANE_B32, "v_readlane_b32") X(V_MOVRELS_B32, "v_movrels_b32") X(V_MOVRELD_B32, "v_movreld_b32") X(V_MOVRELSD_B32, "v_movrelsd_b32") X(V_WRITELANE_B32, "v_writelane_b32")                                          \
    X(V_MIN_I32, "v_min_i32") X(V_MAX_I32, "v_max_i32") X(V_MIN_U32, "v_min_u32") X(V_MAX_U32, "v_max_u32")            \
    X(V_MIN3_I32, "v_min3_i32") X(V_MAX3_I32, "v_max3_i32") X(V_MIN3_U32, "v_min3_u32") X(V_MAX3_U32, "v_max3_u32")    \
    X(V_MED3_I32, "v_med3_i32") X(V_MED3_U32, "v_med3_u32") X(V_LSHRREV_B32, "v_lshrrev_b32")                          \
    X(V_ASHRREV_I32, "v_ashrrev_i32") X(V_LSHLREV_B32, "v_lshlrev_b32") X(V_AND_B32, "v_and_b32")                      \
    X(V_OR_B32, "v_or_b32") X(V_XOR_B32, "v_xor_b32") X(V_XNOR_B32, "v_xnor_b32") X(V_NOT_B32, "v_not_b32")            \
    X(V_BFREV_B32, "v_bfrev_b32") X(V_ADD_NC_U32, "v_add_nc_u32") X(V_SUB_NC_U32, "v_sub_nc_u32")                      \
    X(V_SUBREV_NC_U32, "v_subrev_nc_u32") X(V_ADD_CO_U32, "v_add_co_u32") X(V_SUB_CO_U32, "v_sub_co_u32")              \
    X(V_SUBREV_CO_U32, "v_subrev_co_u32") X(V_ADD_CO_CI_U32, "v_add_co_ci_u32") X(V_SUB_CO_CI_U32, "v_sub_co_ci_u32") X(V_SUBREV_CO_CI_U32, "v_subrev_co_ci_u32") X(V_ADD3_U32, "v_add3_u32") X(V_SAD_U32, "v_sad_u32")            \
    X(V_LSHL_ADD_U32, "v_lshl_add_u32") X(V_ADD_LSHL_U32, "v_add_lshl_u32") X(V_LSHL_OR_B32, "v_lshl_or_b32")          \
    X(V_AND_OR_B32, "v_and_or_b32") X(V_OR3_B32, "v_or3_b32") X(V_XOR3_B32, "v_xor3_b32") X(V_BFE_U32, "v_bfe_u32")    \
    X(V_BFE_I32, "v_bfe_i32") X(V_BFI_B32, "v_bfi_b32") X(V_BFM_B32, "v_bfm_b32") X(V_BCNT_U32_B32, "v_bcnt_u32_b32")  \
    X(V_MBCNT_LO_U32_B32, "v_mbcnt_lo_u32_b32") X(V_MBCNT_HI_U32_B32, "v_mbcnt_hi_u32_b32")                            \
    X(V_ALIGNBIT_B32, "v_alignbit_b32") X(V_PERM_B32, "v_perm_b32") X(V_FFBH_U32, "v_ffbh_u32")                        \
    X(V_FFBL_B32, "v_ffbl_b32") X(V_CVT_F32_I32, "v_cvt_f32_i32") X(V_CVT_F32_U32, "v_cvt_f32_u32")                    \
    X(V_CVT_U32_F32, "v_cvt_u32_f32") X(V_CVT_I32_F32, "v_cvt_i32_f32") X(V_CVT_F16_F32, "v_cvt_f16_f32")              \
    X(V_CVT_F32_F16, "v_cvt_f32_f16") X(V_CVT_FLR_I32_F32, "v_cvt_flr_i32_f32") X(V_CVT_RPI_I32_F32, "v_cvt_rpi_i32_f32") \
    X(V_CVT_F32_UBYTE0, "v_cvt_f32_ubyte0") X(V_CVT_F32_UBYTE1, "v_cvt_f32_ubyte1")                                    \
    X(V_CVT_F32_UBYTE2, "v_cvt_f32_ubyte2") X(V_CVT_F32_UBYTE3, "v_cvt_f32_ubyte3")                                    \
    X(V_CVT_PKRTZ_F16_F32, "v_cvt_pkrtz_f16_f32") X(V_CVT_PKNORM_U16_F32, "v_cvt_pknorm_u16_f32")                      \
    X(V_CVT_PKNORM_I16_F32, "v_cvt_pknorm_i16_f32") X(V_CVT_PK_U8_F32, "v_cvt_pk_u8_f32")                              \
    X(V_PACK_B32_F16, "v_pack_b32_f16") X(V_FRACT_F32, "v_fract_f32") X(V_TRUNC_F32, "v_trunc_f32")                    \
    X(V_CEIL_F32, "v_ceil_f32") X(V_RNDNE_F32, "v_rndne_f32") X(V_FLOOR_F32, "v_floor_f32") X(V_EXP_F32, "v_exp_f32")  \
    X(V_LOG_F32, "v_log_f32") X(V_RCP_F32, "v_rcp_f32") X(V_RCP_IFLAG_F32, "v_rcp_iflag_f32") X(V_RSQ_F32, "v_rsq_f32") \
    X(V_SQRT_F32, "v_sqrt_f32") X(V_SIN_F32, "v_sin_f32") X(V_COS_F32, "v_cos_f32") X(V_LDEXP_F32, "v_ldexp_f32")      \
    X(V_FREXP_MANT_F32, "v_frexp_mant_f32") X(V_FREXP_EXP_I32_F32, "v_frexp_exp_i32_f32")                              \
    X(V_CUBEID_F32, "v_cubeid_f32") X(V_CUBESC_F32, "v_cubesc_f32") X(V_CUBETC_F32, "v_cubetc_f32")                    \
    X(V_CUBEMA_F32, "v_cubema_f32") X(V_INTERP_P1_F32, "v_interp_p1_f32") X(V_INTERP_P2_F32, "v_interp_p2_f32")        \
    X(V_INTERP_MOV_F32, "v_interp_mov_f32") X(V_ADD_F16, "v_add_f16") X(V_MUL_F16, "v_mul_f16")                        \
    X(V_FMA_F16, "v_fma_f16") X(V_PK_ADD_F16, "v_pk_add_f16") X(V_PK_MUL_F16, "v_pk_mul_f16")                          \
    X(V_PK_FMA_F16, "v_pk_fma_f16") X(V_PK_FMAC_F16, "v_pk_fmac_f16") X(V_FMAMK_F16, "v_fmamk_f16")                   \
    X(V_FMAAK_F16, "v_fmaak_f16")                                                                                    \
    /* 16-bit */                                                                                                       \
    X(V_SUB_F16, "v_sub_f16") X(V_SUBREV_F16, "v_subrev_f16") X(V_MIN_F16, "v_min_f16") X(V_MAX_F16, "v_max_f16")      \
    X(V_MAD_F16, "v_mad_f16") X(V_FMAC_F16, "v_fmac_f16") X(V_LSHLREV_B16, "v_lshlrev_b16")                            \
    X(V_LSHRREV_B16, "v_lshrrev_b16") X(V_ASHRREV_I16, "v_ashrrev_i16") X(V_ADD_U16, "v_add_u16")                      \
    X(V_SUB_U16, "v_sub_u16") X(V_MUL_LO_U16, "v_mul_lo_u16") X(V_CVT_I16_F16, "v_cvt_i16_f16")                        \
    X(V_CVT_U16_F16, "v_cvt_u16_f16") X(V_CVT_F16_I16, "v_cvt_f16_i16") X(V_CVT_F16_U16, "v_cvt_f16_u16")              \
    X(V_CVT_PK_U16_U32, "v_cvt_pk_u16_u32") X(V_CVT_PK_I16_I32, "v_cvt_pk_i16_i32")                                    \
    X(V_PK_MIN_F16, "v_pk_min_f16") X(V_PK_MAX_F16, "v_pk_max_f16") X(V_PK_ADD_U16, "v_pk_add_u16")                    \
    X(V_PK_SUB_U16, "v_pk_sub_u16") X(V_PK_ADD_I16, "v_pk_add_i16") X(V_PK_SUB_I16, "v_pk_sub_i16")                    \
    X(V_PK_MUL_LO_U16, "v_pk_mul_lo_u16") X(V_PK_MAD_U16, "v_pk_mad_u16") X(V_PK_MAD_I16, "v_pk_mad_i16")              \
    X(V_PK_LSHLREV_B16, "v_pk_lshlrev_b16") X(V_PK_LSHRREV_B16, "v_pk_lshrrev_b16")                                    \
    X(V_PK_ASHRREV_I16, "v_pk_ashrrev_i16") X(V_PK_MAX_U16, "v_pk_max_u16") X(V_PK_MIN_U16, "v_pk_min_u16")            \
    X(V_PK_MAX_I16, "v_pk_max_i16") X(V_PK_MIN_I16, "v_pk_min_i16")                                                    \
    X(V_FMA_MIX_F32, "v_fma_mix_f32") X(V_FMA_MIXLO_F16, "v_fma_mixlo_f16") X(V_FMA_MIXHI_F16, "v_fma_mixhi_f16")      \
    /* lanes, 64-bit and misc */                                                                                       \
    X(V_PERMLANEX16_B32, "v_permlanex16_b32") X(V_PERMLANE16_B32, "v_permlane16_b32") X(V_XAD_U32, "v_xad_u32")        \
    X(V_LSHLREV_B64, "v_lshlrev_b64") X(V_LSHRREV_B64, "v_lshrrev_b64") X(V_ASHRREV_I64, "v_ashrrev_i64")              \
    X(V_MAD_U64_U32, "v_mad_u64_u32") X(V_MAD_I64_I32, "v_mad_i64_i32") X(V_ALIGNBYTE_B32, "v_alignbyte_b32")          \
    X(S_BFM_B64, "s_bfm_b64")                                                                                          \
    X(V_RSQ_F16, "v_rsq_f16") X(V_SQRT_F16, "v_sqrt_f16") X(V_RCP_F16, "v_rcp_f16") X(V_SIN_F16, "v_sin_f16")          \
    X(V_COS_F16, "v_cos_f16") X(V_EXP_F16, "v_exp_f16") X(V_LOG_F16, "v_log_f16") X(V_FLOOR_F16, "v_floor_f16")        \
    X(V_CEIL_F16, "v_ceil_f16") X(V_TRUNC_F16, "v_trunc_f16") X(V_RNDNE_F16, "v_rndne_f16") X(V_FRACT_F16, "v_fract_f16") \
    X(V_LDEXP_F16, "v_ldexp_f16") X(V_MED3_F16, "v_med3_f16") X(V_MIN3_F16, "v_min3_f16") X(V_MAX3_F16, "v_max3_f16") X(V_ADD_NC_U16, "v_add_nc_u16") X(V_SUB_NC_U16, "v_sub_nc_u16") X(V_ADD_NC_I16, "v_add_nc_i16") X(V_SUB_NC_I16, "v_sub_nc_i16")                      \
    X(V_MAX_U16, "v_max_u16") X(V_MIN_U16, "v_min_u16") X(V_MAX_I16, "v_max_i16") X(V_MIN_I16, "v_min_i16")            \
    /* completing the ISA: scalar, vector and memory forms games rarely use */                                         \
    X(S_ASHR_I64, "s_ashr_i64") X(S_BFE_I64, "s_bfe_i64") X(S_CMOVK_I32, "s_cmovk_i32")                                \
    X(S_BCNT0_I32_B32, "s_bcnt0_i32_b32") X(S_BCNT0_I32_B64, "s_bcnt0_i32_b64") X(S_FF0_I32_B32, "s_ff0_i32_b32")      \
    X(S_FF0_I32_B64, "s_ff0_i32_b64") X(S_FLBIT_I32, "s_flbit_i32") X(S_FLBIT_I32_I64, "s_flbit_i32_i64")              \
    X(S_BREV_B64, "s_brev_b64") X(S_BITSET0_B64, "s_bitset0_b64") X(S_BITSET1_B64, "s_bitset1_b64")                    \
    X(S_CMOV_B64, "s_cmov_b64") X(S_BITCMP0_B64, "s_bitcmp0_b64") X(S_BITCMP1_B64, "s_bitcmp1_b64")                    \
    X(S_ANDN1_WREXEC_B32, "s_andn1_wrexec_b32") X(S_ANDN1_WREXEC_B64, "s_andn1_wrexec_b64") X(S_ANDN2_WREXEC_B32, "s_andn2_wrexec_b32")\
    X(S_ANDN2_WREXEC_B64, "s_andn2_wrexec_b64") X(S_MEMTIME, "s_memtime") X(S_MEMREALTIME, "s_memrealtime")            \
    X(V_MUL_HI_I32_I24, "v_mul_hi_i32_i24") X(V_MUL_HI_U32_U24, "v_mul_hi_u32_u24") X(V_DOT2C_F32_F16, "v_dot2c_f32_f16")\
    X(V_DOT4C_I32_I8, "v_dot4c_i32_i8") X(V_MAC_LEGACY_F32, "v_mac_legacy_f32") X(V_MAD_LEGACY_F32, "v_mad_legacy_f32")\
    X(V_MULLIT_F32, "v_mullit_f32") X(V_FFBH_I32, "v_ffbh_i32") X(V_SWAP_B32, "v_swap_b32")                            \
    X(V_CVT_NORM_I16_F16, "v_cvt_norm_i16_f16") X(V_CVT_NORM_U16_F16, "v_cvt_norm_u16_f16") X(V_FREXP_EXP_I16_F16, "v_frexp_exp_i16_f16")\
    X(V_FREXP_MANT_F16, "v_frexp_mant_f16") X(V_SAT_PK_U8_I16, "v_sat_pk_u8_i16") X(V_ADD_NC_I32, "v_add_nc_i32")      \
    X(V_SUB_NC_I32, "v_sub_nc_i32") X(V_CVT_PKNORM_I16_F16, "v_cvt_pknorm_i16_f16") X(V_CVT_PKNORM_U16_F16, "v_cvt_pknorm_u16_f16")\
    X(V_DIV_FIXUP_F16, "v_div_fixup_f16") X(V_DIV_FIXUP_F32, "v_div_fixup_f32") X(V_DIV_FMAS_F32, "v_div_fmas_f32")    \
    X(V_DIV_SCALE_F32, "v_div_scale_f32") X(V_LERP_U8, "v_lerp_u8") X(V_MAD_I16, "v_mad_i16")                          \
    X(V_MAD_U16, "v_mad_u16") X(V_MAD_I32_I16, "v_mad_i32_i16") X(V_MAD_U32_U16, "v_mad_u32_u16")                      \
    X(V_MAX3_I16, "v_max3_i16") X(V_MAX3_U16, "v_max3_u16") X(V_MIN3_I16, "v_min3_i16")                                \
    X(V_MIN3_U16, "v_min3_u16") X(V_MED3_I16, "v_med3_i16") X(V_MED3_U16, "v_med3_u16")                                \
    X(V_MSAD_U8, "v_msad_u8") X(V_SAD_U8, "v_sad_u8") X(V_SAD_HI_U8, "v_sad_hi_u8")                                    \
    X(V_SAD_U16, "v_sad_u16") X(V_DOT2_F32_F16, "v_dot2_f32_f16") X(V_DOT2_I32_I16, "v_dot2_i32_i16")                  \
    X(V_DOT2_U32_U16, "v_dot2_u32_u16") X(V_DOT4_I32_I8, "v_dot4_i32_i8") X(V_DOT4_U32_U8, "v_dot4_u32_u8")            \
    X(V_DOT8_I32_I4, "v_dot8_i32_i4") X(V_DOT8_U32_U4, "v_dot8_u32_u4")                                                \
    X(V_CVT_F64_F32, "v_cvt_f64_f32") X(V_CVT_F32_F64, "v_cvt_f32_f64") X(V_CVT_F64_I32, "v_cvt_f64_i32")              \
    X(V_CVT_F64_U32, "v_cvt_f64_u32") X(V_CVT_I32_F64, "v_cvt_i32_f64") X(V_CVT_U32_F64, "v_cvt_u32_f64")              \
    X(V_TRUNC_F64, "v_trunc_f64") X(V_FLOOR_F64, "v_floor_f64") X(V_CEIL_F64, "v_ceil_f64")                            \
    X(V_RNDNE_F64, "v_rndne_f64") X(V_FRACT_F64, "v_fract_f64") X(V_FREXP_MANT_F64, "v_frexp_mant_f64")                \
    X(V_FREXP_EXP_I32_F64, "v_frexp_exp_i32_f64") X(V_LDEXP_F64, "v_ldexp_f64") X(V_MAX_F64, "v_max_f64")              \
    X(V_MIN_F64, "v_min_f64") X(V_ADD_F64, "v_add_f64") X(V_MUL_F64, "v_mul_f64")                                      \
    X(V_FMA_F64, "v_fma_f64") X(V_RCP_F64, "v_rcp_f64") X(V_RSQ_F64, "v_rsq_f64")                                      \
    X(V_SQRT_F64, "v_sqrt_f64") X(V_DIV_SCALE_F64, "v_div_scale_f64") X(V_DIV_FMAS_F64, "v_div_fmas_f64")              \
    X(V_DIV_FIXUP_F64, "v_div_fixup_f64")                                                                              \
    X(BUFFER_LOAD_SBYTE, "buffer_load_sbyte") X(BUFFER_LOAD_SSHORT, "buffer_load_sshort") X(BUFFER_LOAD_UBYTE_D16, "buffer_load_ubyte_d16")\
    X(BUFFER_LOAD_UBYTE_D16_HI, "buffer_load_ubyte_d16_hi") X(BUFFER_LOAD_SBYTE_D16, "buffer_load_sbyte_d16") X(BUFFER_LOAD_SBYTE_D16_HI, "buffer_load_sbyte_d16_hi")\
    X(BUFFER_LOAD_SHORT_D16, "buffer_load_short_d16") X(BUFFER_LOAD_SHORT_D16_HI, "buffer_load_short_d16_hi") X(BUFFER_STORE_BYTE, "buffer_store_byte")\
    X(BUFFER_STORE_BYTE_D16_HI, "buffer_store_byte_d16_hi") X(BUFFER_STORE_SHORT, "buffer_store_short") X(BUFFER_STORE_SHORT_D16_HI, "buffer_store_short_d16_hi")\
    /* vector compares: kind from the name tail */                                                                     \
    X(V_CMP, "v_cmp_") X(V_CMPX, "v_cmpx_")                                                                            \
    /* vector memory */                                                                                                \
    X(BUFFER_LOAD_FORMAT, "buffer_load_format_") X(BUFFER_STORE_FORMAT, "buffer_store_format_")                        \
    X(TBUFFER_LOAD_FORMAT, "tbuffer_load_format_") X(TBUFFER_STORE_FORMAT, "tbuffer_store_format_")                    \
    X(BUFFER_LOAD_DWORD, "buffer_load_dword") X(BUFFER_STORE_DWORD, "buffer_store_dword")                              \
    X(BUFFER_LOAD_UBYTE, "buffer_load_ubyte") X(BUFFER_LOAD_USHORT, "buffer_load_ushort")                              \
    X(IMAGE_SAMPLE, "image_sample") X(IMAGE_LOAD, "image_load") X(IMAGE_STORE, "image_store")                          \
    X(IMAGE_GET_RESINFO, "image_get_resinfo") X(IMAGE_GET_LOD, "image_get_lod") X(IMAGE_GATHER4, "image_gather4") X(IMAGE_ATOMIC, "image_atomic_")                                         \
    X(BUFFER_ATOMIC, "buffer_atomic_") X(GLOBAL_LOAD, "global_load_") X(GLOBAL_STORE, "global_store_") X(GLOBAL_ATOMIC, "global_atomic_") X(SCRATCH_LOAD, "scratch_load_") X(SCRATCH_STORE, "scratch_store_")                                                                                 \
    /* local data share: kind from the hardware opcode */                                                             \
    X(DS, "ds_")                                                                                                       \
    X(IMAGE_BVH, "image_bvh_intersect_ray") X(IMAGE_BVH64, "image_bvh64_intersect_ray")                              \
    X(EXP, "exp")

typedef enum sh_op {
#define X(id, name) SH_OP_##id,
    SH_OPS(X)
#undef X
    SH_OP_COUNT
} sh_op;

sh_op sh_op_of(const rdna2_inst *in);

#endif
