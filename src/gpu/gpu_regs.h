/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RDNA2 (GFX10.3) register dword offsets used by the command processor.
 * Offsets are absolute dword addresses: SET_SH_REG and friends add their
 * block base (SH 0x2C00, context 0xA000, uconfig 0xC000). Only registers
 * the renderer reads are listed. */
#ifndef PS5C_GPU_REGS_H
#define PS5C_GPU_REGS_H

#define GPU_REG_SH_BASE 0x2C00u
#define GPU_REG_CONTEXT_BASE 0xA000u
#define GPU_REG_UCONFIG_BASE 0xC000u
#define GPU_REG_COUNT 0x10000u

/* ---- SH: shader programs and user data ---- */
#define mmSPI_SHADER_PGM_LO_PS 0x2C08u
#define mmSPI_SHADER_PGM_HI_PS 0x2C09u
#define mmSPI_SHADER_PGM_RSRC1_PS 0x2C0Au
#define mmSPI_SHADER_PGM_RSRC2_PS 0x2C0Bu
#define mmSPI_SHADER_USER_DATA_PS_0 0x2C0Cu
#define mmSPI_SHADER_PGM_RSRC1_GS 0x2C8Au
#define mmSPI_SHADER_PGM_RSRC2_GS 0x2C8Bu
#define mmSPI_SHADER_USER_DATA_GS_0 0x2C8Cu
#define mmSPI_SHADER_USER_DATA_ADDR_LO_GS 0x2C82u /* byte address, delivered to GS s0:s1 */
#define mmSPI_SHADER_USER_DATA_ADDR_HI_GS 0x2C83u
#define mmSPI_SHADER_PGM_LO_ES 0x2CC8u /* merged ES+GS (NGG) program */
#define mmSPI_SHADER_PGM_LO_GS 0x2C88u /* its second half when the first ends in s_setpc_b64 s[6:7] */
#define mmSPI_SHADER_PGM_HI_ES 0x2CC9u
#define GPU_USER_DATA_REGS 32
/* PS4 (GCN) hardware VS stage */
#define GPU_GCN_PGM_LO_VS 0x2C48u
#define GPU_GCN_RSRC1_VS 0x2C4Au
#define GPU_GCN_RSRC2_VS 0x2C4Bu
#define GPU_GCN_USER_DATA_VS_0 0x2C4Cu

#define mmCOMPUTE_NUM_THREAD_X 0x2E07u
#define mmCOMPUTE_DISPATCH_INITIATOR 0x2E00u
#define mmCOMPUTE_NUM_THREAD_Y 0x2E08u
#define mmCOMPUTE_NUM_THREAD_Z 0x2E09u
#define mmCOMPUTE_PGM_LO 0x2E0Cu
#define mmCOMPUTE_PGM_HI 0x2E0Du
#define mmCOMPUTE_PGM_RSRC1 0x2E12u
#define mmCOMPUTE_PGM_RSRC2 0x2E13u
#define mmCOMPUTE_USER_DATA_0 0x2E40u
#define GPU_COMPUTE_USER_DATA_REGS 16

/* ---- Context: depth, viewport, render targets ---- */
#define mmDB_RENDER_CONTROL 0xA000u
#define mmDB_DEPTH_VIEW 0xA002u
#define mmDB_DEPTH_SIZE_XY 0xA007u
#define mmDB_STENCIL_CLEAR 0xA00Au
#define mmDB_DEPTH_CLEAR 0xA00Bu
#define mmDB_HTILE_DATA_BASE 0xA005u
#define mmDB_Z_INFO 0xA010u
#define GPU_DB_Z_INFO_TILE_SURFACE_ENABLE (1u << 29)
#define mmDB_STENCIL_INFO 0xA011u
#define mmDB_STENCIL_READ_BASE 0xA013u
#define mmDB_STENCIL_WRITE_BASE 0xA015u
#define mmDB_STENCIL_CONTROL 0xA10Bu
#define mmDB_STENCILREFMASK 0xA10Cu
#define mmDB_STENCILREFMASK_BF 0xA10Du
#define mmDB_Z_READ_BASE 0xA012u
#define mmDB_Z_WRITE_BASE 0xA014u
#define mmPA_SC_WINDOW_SCISSOR_TL 0xA081u
#define mmPA_SC_WINDOW_SCISSOR_BR 0xA082u
#define mmCB_TARGET_MASK 0xA08Eu
#define mmCB_SHADER_MASK 0xA08Fu
#define mmPA_SC_GENERIC_SCISSOR_TL 0xA090u
#define mmPA_SC_GENERIC_SCISSOR_BR 0xA091u
#define mmPA_SC_VPORT_SCISSOR_0_TL 0xA094u
#define mmPA_SC_VPORT_SCISSOR_0_BR 0xA095u
#define mmPA_SC_SCREEN_SCISSOR_TL 0xA00Cu
#define mmPA_SC_SCREEN_SCISSOR_BR 0xA00Du
#define mmPA_SC_MODE_CNTL_0 0xA292u
#define mmPA_SC_AA_MASK_X0Y0_X1Y0 0xA30Eu
#define mmPA_SC_AA_MASK_X0Y1_X1Y1 0xA30Fu
#define mmPA_SC_VPORT_ZMIN_0 0xA0B4u
#define mmPA_SC_VPORT_ZMAX_0 0xA0B5u
#define mmPA_CL_VPORT_XSCALE 0xA10Fu
#define mmCB_BLEND_RED 0xA105u
#define mmCB_BLEND_GREEN 0xA106u
#define mmCB_BLEND_BLUE 0xA107u
#define mmCB_BLEND_ALPHA 0xA108u
#define mmPA_CL_VPORT_XOFFSET 0xA110u
#define mmPA_CL_VPORT_YSCALE 0xA111u
#define mmPA_CL_VPORT_YOFFSET 0xA112u
#define mmPA_CL_VPORT_ZSCALE 0xA113u
#define mmPA_CL_VPORT_ZOFFSET 0xA114u
#define mmSPI_PS_INPUT_CNTL_0 0xA191u
#define mmSPI_VS_OUT_CONFIG 0xA1B1u
#define mmSPI_PS_INPUT_ENA 0xA1B3u
#define mmSPI_PS_INPUT_ADDR 0xA1B4u
#define mmSPI_SHADER_POS_FORMAT 0xA1C3u
#define mmSPI_SHADER_Z_FORMAT 0xA1C4u
#define mmSPI_SHADER_COL_FORMAT 0xA1C5u
#define mmCB_BLEND0_CONTROL 0xA1E0u
#define mmVGT_MULTI_PRIM_IB_RESET_INDX 0xA103u
#define mmDB_DEPTH_CONTROL 0xA200u
#define mmCB_COLOR_CONTROL 0xA202u
#define mmDB_SHADER_CONTROL 0xA203u
#define mmPA_CL_CLIP_CNTL 0xA204u
#define mmPA_SU_SC_MODE_CNTL 0xA205u
#define mmPA_CL_VTE_CNTL 0xA206u
#define mmCB_COLOR0_BASE 0xA318u
#define mmCB_COLOR0_VIEW 0xA31Bu
#define mmCB_COLOR0_INFO 0xA31Cu
#define mmCB_COLOR0_ATTRIB 0xA31Du
#define mmCB_COLOR0_DCC_CONTROL 0xA31Eu
#define mmCB_COLOR0_CMASK 0xA31Fu
#define mmCB_COLOR0_CLEAR_WORD0 0xA323u
#define mmCB_COLOR0_CLEAR_WORD1 0xA324u
#define mmCB_COLOR0_DCC_BASE 0xA325u
#define mmCB_COLOR0_CMASK_BASE_EXT 0xA398u
#define mmCB_COLOR0_DCC_BASE_EXT 0xA3A8u
#define mmCB_COLOR0_BASE_EXT 0xA390u
#define mmCB_COLOR0_ATTRIB2 0xA3B0u
#define mmCB_COLOR0_ATTRIB3 0xA3B8u
#define GPU_CB_STRIDE 0xFu /* CB_COLORn_* registers repeat every 15 dwords */
#define GPU_MAX_RENDER_TARGETS 8

/* ---- UConfig ---- */
#define mmVGT_PRIMITIVE_TYPE 0xC242u
#define mmVGT_INDEX_TYPE 0xC243u
#define mmGE_INDX_OFFSET 0xC24Au
#define mmGE_CNTL 0xC25Bu
#define mmGE_MAX_OUTPUT_PER_SUBGROUP 0xA1FFu
#define mmVGT_GS_OUT_PRIM_TYPE 0xA29Bu
#define mmVGT_GS_MAX_VERT_OUT 0xA2CEu
#define mmGE_NGG_SUBGRP_CNTL 0xA2D3u
#define mmVGT_SHADER_STAGES_EN 0xA2D5u

/* VGT_PRIMITIVE_TYPE values */
enum {
    GPU_PRIM_POINTLIST = 1,
    GPU_PRIM_LINELIST = 2,
    GPU_PRIM_LINESTRIP = 3,
    GPU_PRIM_TRILIST = 4,
    GPU_PRIM_TRIFAN = 5,
    GPU_PRIM_TRISTRIP = 6,
    GPU_PRIM_RECTLIST = 0x11,
    GPU_PRIM_QUADLIST = 0x13,
};

#endif
