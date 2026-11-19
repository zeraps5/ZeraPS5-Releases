/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RDNA2 (GFX10.3) shader instruction decoder and disassembler.
 *
 * rdna2_decode() splits one instruction into its fields; rdna2_disasm()
 * prints it in LLVM assembler syntax, so that tools/amdgpu_asm.py can
 * assemble the text back and check the decoder bit for bit. */
#ifndef PS5C_RDNA2_H
#define PS5C_RDNA2_H

#include "base/base.h"

typedef enum rdna2_enc {
    RDNA2_ENC_UNKNOWN,
    RDNA2_ENC_SOP2,
    RDNA2_ENC_SOPK,
    RDNA2_ENC_SOP1,
    RDNA2_ENC_SOPC,
    RDNA2_ENC_SOPP,
    RDNA2_ENC_SMEM,
    RDNA2_ENC_VOP2,
    RDNA2_ENC_VOP1,
    RDNA2_ENC_VOPC,
    RDNA2_ENC_VOP3,
    RDNA2_ENC_VOP3P,
    RDNA2_ENC_VINTRP,
    RDNA2_ENC_DS,
    RDNA2_ENC_MUBUF,
    RDNA2_ENC_MTBUF,
    RDNA2_ENC_MIMG,
    RDNA2_ENC_EXP,
    RDNA2_ENC_FLAT, /* FLAT, GLOBAL and SCRATCH (seg) */
} rdna2_enc;

/* Operand codes (9 bits, as in VOP3 src fields; VGPRs are 256 + n) */
enum {
    RDNA2_OP_VCC_LO = 106,
    RDNA2_OP_VCC_HI = 107,
    RDNA2_OP_M0 = 124,
    RDNA2_OP_NULL = 125,
    RDNA2_OP_EXEC_LO = 126,
    RDNA2_OP_EXEC_HI = 127,
    RDNA2_OP_ZERO = 128,
    RDNA2_OP_DPP8 = 233,
    RDNA2_OP_DPP8FI = 234,
    RDNA2_OP_SDWA = 249,
    RDNA2_OP_DPP16 = 250,
    RDNA2_OP_VCCZ = 251,
    RDNA2_OP_EXECZ = 252,
    RDNA2_OP_SCC = 253,
    RDNA2_OP_LDS_DIRECT = 254,
    RDNA2_OP_LITERAL = 255,
    RDNA2_OP_VGPR = 256,
};

typedef struct rdna2_inst {
    rdna2_enc enc;
    uint16_t op;     /* opcode within the encoding */
    uint16_t valu;   /* VOP1/VOP2/VOPC/VOP3: opcode in VOP3 numbering */
    uint8_t words;   /* instruction size in dwords, literal included */
    const char *name;

    /* Scalar and vector ALU */
    uint16_t dst;     /* operand code (VGPR dst as 256 + n) */
    uint16_t sdst;    /* VOP3B / VOPC-e64 scalar destination */
    uint16_t src[3];  /* operand codes */
    uint32_t literal; /* when a source is RDNA2_OP_LITERAL */
    int32_t simm16;   /* SOPK / SOPP */
    uint8_t abs, neg, neg_hi, opsel, opsel_hi, omod;
    bool clamp;
    uint32_t dpp_sdwa; /* raw DPP / SDWA dword (src0 == SDWA/DPP) */

    /* Memory */
    uint16_t vaddr, vdata, sbase, srsrc, ssamp, soffset; /* register numbers (SGPR base or VGPR) */
    int32_t offset;
    uint8_t dmask, dim, format, seg;
    uint8_t nsa_count;     /* MIMG non-sequential addresses beyond vaddr */
    uint8_t nsa_addr[12];
    bool glc, slc, dlc, offen, idxen, unorm, a16, d16, tfe, lwe, r128, gds, soffset_imm;

    /* Export */
    uint8_t exp_target, exp_en;
    bool exp_compr, exp_done, exp_vm;
    uint8_t exp_src[4];

    /* VINTRP */
    uint8_t attr, attr_chan;
} rdna2_inst;

/* Decodes the instruction at code[0..avail). False if it is truncated or unknown. */
bool rdna2_decode(const uint32_t *code, uint32_t avail, rdna2_inst *out);
/* Prints `in` in LLVM syntax. Returns false when it cannot be printed exactly. */
bool rdna2_disasm(const rdna2_inst *in, char *buf, size_t size);

/* VALU opcode (VOP3 numbering: VOPC 0x000, VOP2 0x100, VOP1 0x180, VOP3 only) of an instruction name, or -1 */
int rdna2_valu_opcode(const char *name);
/* VOP3 opcodes whose dword 0 holds a scalar destination (VOP3B) */
bool rdna2_valu_vop3b(uint32_t valu);
/* Name of opcode `op` in an encoding (VOP3: VALU numbering), NULL if none */
const char *rdna2_op_name(rdna2_enc enc, uint32_t op);

#endif
