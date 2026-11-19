/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Guest stencil state for one face, as Vulkan can hold it. The guest has two
 * values (STENCILTESTVAL for the compare, STENCILOPVAL for REPLACE_OP; ONES
 * writes 0xFF); Vulkan has one reference for both. When a replacement writes a
 * value other than the test value, the reference stays the test value if the
 * compare guarantees what the stencil holds wherever that op runs (EQUAL for
 * pass/depth-fail ops, NOTEQUAL for fail ops) and every written bit is
 * compared: writing `value` there is inverting the bits where it differs from
 * the test value (an INVERT op with that write mask). The approach follows
 * AnyPS5's analysis (PR 7725); the code is ours. */
#ifndef PS5C_VK_STENCIL_H
#define PS5C_VK_STENCIL_H
#include <stdbool.h>
#include <stdint.h>

typedef struct vk_stencil_face {
    uint32_t ref, cmp, wr;
} vk_stencil_face;

#define VK_STENCIL_GUEST_INVERT 7u /* DB_STENCIL_CONTROL op code */

/* Exact logical-op lowering (AnyPS5 #9913 / KytyPS5 d4f8338 analysis).
 * Only KEEP and logic ops sharing one effective write mask are accepted.
 * Failure leaves both outputs untouched. Caller excludes shader exports. */
static inline bool vk_stencil_logic_state(uint32_t refmask, uint32_t func,
                                          uint32_t *ops, vk_stencil_face *f) {
    uint32_t test = refmask & 255, cmp = (refmask >> 8) & 255;
    uint32_t wr = (refmask >> 16) & 255, value = refmask >> 24;
    uint32_t mapped = 0, mask = 0, ref = test;
    bool logic = false, have_mask = false;
    for (unsigned i = 0; i < 3; i++) {
        uint32_t op = (*ops >> (4 * i)) & 15, bits, host;
        if (!op) continue;
        switch (op) {
        case 10: bits = ~value & wr; host = 1; break; /* AND -> ZERO */
        case 11: bits = value & wr; host = 3; ref |= bits; break; /* OR -> REPLACE */
        case 12: bits = value & wr; host = 7; break; /* XOR -> INVERT */
        case 15: bits = ~value & wr; host = 7; break; /* XNOR -> INVERT */
        default: return false;
        }
        logic = true;
        if (!bits) continue; /* no bits change: KEEP */
        if (have_mask && mask != bits) return false;
        have_mask = true;
        mask = bits;
        mapped |= host << (4 * i);
    }
    if (!logic || (func != 0 && func != 7 && ((ref ^ test) & cmp))) return false;
    *ops = mapped;
    *f = (vk_stencil_face){ref, cmp, mask};
    return true;
}

/* refmask: DB_STENCILREFMASK(_BF) (STENCILTESTVAL [7:0], STENCILMASK [15:8],
 * STENCILWRITEMASK [23:16], STENCILOPVAL [31:24]); func: STENCILFUNC (Vulkan's
 * order: 0 NEVER .. 7 ALWAYS); ops: fail [3:0], zpass [7:4], zfail [11:8], rewritten
 * in place when a replacement becomes an inversion. */
static inline void vk_stencil_face_state(uint32_t refmask, uint32_t func, uint32_t *ops, vk_stencil_face *f) {
    uint32_t test = refmask & 0xFF, cmp = (refmask >> 8) & 0xFF, wr = (refmask >> 16) & 0xFF, opval = refmask >> 24;
    f->ref = test;
    f->cmp = cmp;
    f->wr = wr;
    int32_t value[3];
    bool any = false;
    for (int i = 0; i < 3; i++) {
        uint32_t op = (*ops >> (4 * i)) & 15;
        value[i] = op == 4 ? (int32_t)opval : op == 2 ? 0xFF : op == 3 ? (int32_t)test : -1;
        any |= value[i] >= 0 && op != 3;
    }
    if (!any) return;
    if (func == 0 || func == 7) { /* the compare needs no reference: it is the replacement's value */
        for (int i = 0; i < 3; i++)
            if (value[i] >= 0) {
                f->ref = (uint32_t)value[i];
                break;
            }
        return;
    }
    /* the compare needs the test value: replacements that differ from it become inversions,
     * and then every other op of the face must be one too (they share the write mask) */
    uint32_t mask = 0, need = 0, other = 0;
    bool ok = true;
    for (int i = 0; i < 3; i++) {
        uint32_t op = (*ops >> (4 * i)) & 15;
        if (!op) continue;
        uint32_t diff = value[i] >= 0 ? ((uint32_t)value[i] ^ test) & wr : 0;
        if (!diff) {
            other++;
            continue;
        }
        bool pinned = (func == 2 && i > 0) || (func == 5 && i == 0); /* EQUAL: pass/zfail; NOTEQUAL: fail */
        if (!pinned || (wr & ~cmp) || (mask && mask != diff)) ok = false;
        mask = diff;
        need |= 1u << i;
    }
    if (!need || !ok || other) return; /* as close as one reference allows: the test value */
    for (int i = 0; i < 3; i++)
        if ((need >> i) & 1) *ops = (*ops & ~(15u << (4 * i))) | (VK_STENCIL_GUEST_INVERT << (4 * i));
    f->wr = mask;
}

#endif
