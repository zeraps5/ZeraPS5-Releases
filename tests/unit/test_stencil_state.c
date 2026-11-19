/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Guest stencil face -> Vulkan reference/masks/ops (vk_stencil.h), checked by
 * simulating the guest's two-value semantics against the Vulkan result. */
#include <stdio.h>

#include "gpu/vk/vk_stencil.h"

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int cmp(uint32_t func, uint32_t ref, uint32_t s) {
    switch (func) {
    case 0: return 0;
    case 1: return ref < s;
    case 2: return ref == s;
    case 3: return ref <= s;
    case 4: return ref > s;
    case 5: return ref != s;
    case 6: return ref >= s;
    default: return 1;
    }
}

/* the guest: test value for the compare, op value for REPLACE_OP */
static uint32_t guest(uint32_t s, uint32_t refmask, uint32_t func, uint32_t op) {
    uint32_t test = refmask & 0xFF, m = (refmask >> 8) & 0xFF, wr = (refmask >> 16) & 0xFF, opval = refmask >> 24;
    uint32_t v = s;
    switch (op) {
    case 1: v = 0; break;
    case 2: v = 0xFF; break;
    case 3: v = test; break;
    case 4: v = opval; break;
    case 7: v = ~s & 0xFF; break;
    case 10: v = s & opval; break;
    case 11: v = s | opval; break;
    case 12: v = s ^ opval; break;
    case 15: v = ~(s ^ opval) & 255; break;
    }
    (void)m; (void)func;
    return (s & ~wr) | (v & wr);
}

/* Vulkan with one reference */
static uint32_t host(uint32_t s, const vk_stencil_face *f, uint32_t op) {
    uint32_t v = s;
    switch (op) {
    case 1: v = 0; break;
    case 2: case 3: case 4: v = f->ref; break;
    case 7: v = ~s & 0xFF; break;
    }
    return (s & ~f->wr) | (v & f->wr);
}

/* every stencil value: pass op only (fail/zfail KEEP) */
static int same(uint32_t refmask, uint32_t func, uint32_t pass_op) {
    uint32_t ops = pass_op << 4;
    vk_stencil_face f;
    vk_stencil_face_state(refmask, func, &ops, &f);
    uint32_t hop = (ops >> 4) & 15;
    for (uint32_t s = 0; s < 256; s++) {
        uint32_t m = (refmask >> 8) & 0xFF;
        int gp = cmp(func, refmask & m & 0xFF, s & m), hp = cmp(func, f.ref & f.cmp, s & f.cmp);
        if (gp != hp) return 0;
        if (gp && guest(s, refmask, func, pass_op) != host(s, &f, hop)) return 0;
    }
    return 1;
}

static int logic_tests(void) {
    const uint32_t codes[] = {10, 11, 12, 15}, masks[] = {0, 0x35, 0x80, 255};
    unsigned checked = 0, refused = 0;
    for (unsigned c = 0; c < 4; c++) for (unsigned m = 0; m < 4; m++)
    for (uint32_t val = 0; val < 256; val++) for (uint32_t func = 0; func < 8; func++)
    for (unsigned slot = 0; slot < 3; slot++) {
        uint32_t rm = (val << 24) | (masks[m] << 16) | (0xA5u << 8) | (val ^ 0xA5);
        uint32_t before = codes[c] << (4 * slot), ops = before;
        vk_stencil_face f = {0x1234,0x5678,0x9ABC};
        if (!vk_stencil_logic_state(rm, func, &ops, &f)) {
            CHECK(ops == before && f.ref == 0x1234 && f.cmp == 0x5678 && f.wr == 0x9ABC);
            refused++; continue;
        }
        for (uint32_t s = 0; s < 256; s++) {
            int pass = cmp(func, (rm & 255) & 0xA5, s & 0xA5);
            CHECK(pass == cmp(func, f.ref & f.cmp, s & f.cmp));
            for (unsigned depth = 0; depth < 2; depth++) {
                unsigned chosen = !pass ? 0 : depth ? 1 : 2;
                CHECK(guest(s,rm,func,(before >> (4*chosen)) & 15) == host(s,&f,(ops >> (4*chosen)) & 15));
            }
        }
        checked++;
    }
    CHECK(checked && refused);
    uint32_t ops = 10 | (12u << 4); vk_stencil_face f = {0};
    CHECK(!vk_stencil_logic_state(0x55FFFFFF,7,&ops,&f)); /* incompatible masks */
    CHECK(ops == (10 | (12u << 4)));
    ops = 12 | (12u << 4) | (12u << 8);
    CHECK(vk_stencil_logic_state(0x55FFFFFF,7,&ops,&f));
    CHECK(ops == 0x777 && f.wr == 0x55);
    ops = 13u << 4; CHECK(!vk_stencil_logic_state(0x55FFFFFF,7,&ops,&f));
    ops = 14u << 4; CHECK(!vk_stencil_logic_state(0x55FFFFFF,7,&ops,&f));
    printf("stencil logic: %u states checked over all 256 stored values, %u refused\n", checked, refused);
    return 0;
}

int main(void) {
    CHECK(logic_tests() == 0);
    /* Ghost of Tsushima's case: EQUAL 0xFF, write 0x40 with REPLACE_OP */
    CHECK(same(0x40FFFFFFu, 2, 4));
    /* ALWAYS: the reference becomes the op value */
    CHECK(same(0x2AFFFF04u, 7, 4));
    /* REPLACE_TEST and ONES under EQUAL */
    CHECK(same(0x00FFFF2Au, 2, 3));
    CHECK(same(0x00FFFF2Au, 2, 2));
    /* write mask narrower than the compare mask */
    CHECK(same(0x400FFFFFu, 2, 4));
    /* written bits the compare does not see: not expressible, the test value stays the reference */
    uint32_t o2 = 4u << 4;
    vk_stencil_face g;
    vk_stencil_face_state(0x40FF0FFFu, 2, &o2, &g);
    CHECK(g.ref == 0xFF && ((o2 >> 4) & 15) == 4);
    /* DBZ's base pass: ALWAYS, REPLACE_TEST 0x80 under write mask 0xF4 */
    CHECK(same(0x01F4FF80u, 7, 3));
    /* NOTEQUAL keeps the test value as the reference for a pass replace it cannot pin */
    uint32_t ops = 4u << 4;
    vk_stencil_face f;
    vk_stencil_face_state(0x40FFFFFFu, 5, &ops, &f);
    CHECK(f.ref == 0xFF && ((ops >> 4) & 15) == 4);
    printf("stencil state: ok\n");
    return 0;
}
