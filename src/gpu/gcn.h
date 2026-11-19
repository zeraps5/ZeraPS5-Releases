/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GCN (GFX7, the PS4's GPU) shader code re-encoded as RDNA2 (GFX10.3) code, so that PS4
 * shaders go through the translator of PS5 ones.
 *
 * Most of the two instruction sets share their opcodes and operand codes; what differs is
 * re-encoded: SMRD becomes SMEM (byte offsets), VOP3 moves its opcode and clamp bits, a few
 * VOP2 opcodes changed or became VOP3 only (v_cndmask_b32, the carry-out adds, the
 * non-reversed shifts), s_waitcnt moved its counters, MIMG gets a dimension and MTBUF a
 * unified format. Branch offsets follow the new instruction sizes. */
#ifndef PS5C_GCN_H
#define PS5C_GCN_H

#include "base/base.h"

typedef struct gcn_error {
    uint32_t pos;  /* dword of the GCN instruction (in the fetch shader when `fetch`) */
    uint32_t word; /* its first dword */
    bool fetch;
    const char *why;
} gcn_error;

/* Dwords of a fetch shader, up to and including its closing s_setpc_b64; 0 if none in avail. */
uint32_t gcn_fetch_size(const uint32_t *code, uint32_t avail);

/* Re-encodes code[0..avail) up to and including its s_endpgm into out[0..cap). A s_swappc_b64
 * calls the fetch shader: with `fetch` (fetch_words from gcn_fetch_size), its body is inlined
 * there, without its s_setpc_b64. Returns the RDNA2 dword count, 0 on failure (err set). */
uint32_t gcn_to_rdna2(const uint32_t *code, uint32_t avail, const uint32_t *fetch, uint32_t fetch_words,
                      uint32_t *out, uint32_t cap, gcn_error *err);

/* A GFX7 buffer descriptor (V#) rewritten as a GFX10 one: the unified format, OOB_SELECT. */
void gcn_buffer_desc_to_rdna2(uint32_t v[4]);

/* A GFX7 image descriptor (T#) rewritten as a GFX10 one (size, format, levels, array range).
 * Linear: word 7 keeps the GCN tiling index for detiling. Samplers (S#) need no change. */
void gcn_image_desc_to_rdna2(uint32_t t[8]);

#endif
