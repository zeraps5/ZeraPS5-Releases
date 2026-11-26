/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Branch shapes that become SPIR-V selections instead of the dispatch loop
 * (a switch on a program counter, slow on GPUs). */
#include <stdio.h>
#include <stdlib.h>
#include "gpu/shader/shader.h"
#include "base/base.h"
#include "gpu/shader/spv.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

/* The Mortuary Assistant's menu PS (1886 instructions) in miniature:
 * if (scc) { the arms of a chain all jump to one end; s_endpgm } else { other code; s_endpgm } */
static const uint32_t chain[] = {
    0xbf840009, /*  0: s_cbranch_scc0 10 */
    0xbf860005, /*  1:   s_cbranch_vccz 7 */
    0xbf860002, /*  2:     s_cbranch_vccz 5 */
    0x7e000281, /*  3:       v_mov_b32 v0, 1 */
    0xbf820003, /*  4:       s_branch 8 (past the else of 1, not of 2) */
    0x7e000282, /*  5:     v_mov_b32 v0, 2 */
    0xbf820001, /*  6:     s_branch 8 */
    0x7e000283, /*  7:   v_mov_b32 v0, 3 */
    0x7e020280, /*  8: v_mov_b32 v1, 0 */
    0xbf810000, /*  9: s_endpgm */
    0x7e000284, /* 10: v_mov_b32 v0, 4 */
    0xbf810000, /* 11: s_endpgm */
};

/* PSPHD's pixel shaders: an alpha test deep in the code discards by jumping to the end. */
static const uint32_t discard[] = {
    0xbf840005, /*  0: s_cbranch_scc0 6 (the end) */
    0xbf840002, /*  1: s_cbranch_scc0 4 */
    0x7e000281, /*  2:   v_mov_b32 v0, 1 */
    0xbf840002, /*  3:   s_cbranch_scc0 6 (the end) */
    0x7e020280, /*  4: v_mov_b32 v1, 0 */
    0xbf810000, /*  5: s_endpgm */
    0xbf9f0000, /*  6: s_code_end */
};

/* The same in game: the jumps go to an exit block after the s_endpgm (exec restored, a
 * null export), which then ends the program too. */
static const uint32_t exit_block[] = {
    0xbf840004, /*  0: s_cbranch_scc0 5 (the exit block) */
    0x7e000281, /*  1: v_mov_b32 v0, 1 */
    0xbf840002, /*  2: s_cbranch_scc0 5 */
    0x7e020280, /*  3: v_mov_b32 v1, 0 */
    0xbf810000, /*  4: s_endpgm */
    0x7e000285, /*  5: v_mov_b32 v0, 5 */
    0xbf810000, /*  6: s_endpgm */
};

/* With one lane per invocation the saved EXEC mask is cleared in one pass. */
static const uint32_t waterfall[] = {
    0x7e040281, /* v_mov_b32 v2, 1 */
    0xbe94047e, /* s_mov_b64 s[20:21], exec */
    0x7e000502, /* v_readfirstlane_b32 s0, v2 */
    0x7da40400, /* v_cmpx_eq_u32 s0, v2 */
    0x7e060282, /* v_mov_b32 v3, 2 */
    0x8a947e14, /* s_andn2_b64 s[20:21], s[20:21], exec */
    0xbefe0414, /* s_mov_b64 exec, s[20:21] */
    0xbf85fffa, /* s_cbranch_scc1 back to v_readfirstlane */
    0xbf810000, /* s_endpgm */
};

static void option(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
    ps5c_env_reset();
}

static int counts(const uint32_t *code, uint32_t n, unsigned *sw, unsigned *sel, unsigned *loops) {
    sh_program *p = calloc(1, sizeof(*p));
    CHECK(p && sh_analyze(code, n, 0, p) && p->count == n);
    sh_stage_info info = {.stage = SH_STAGE_CS, .local_size = {64, 1, 1}};
    sh_site_shape shapes[SH_MAX_SITES] = {0};
    uint32_t *spv = NULL;
    size_t words = 0;
    CHECK(sh_translate(p, &info, shapes, 0, &spv, &words));
    *sw = *sel = *loops = 0;
    for (size_t i = 5; i < words; i += spv[i] >> 16) {
        CHECK(spv[i] >> 16);
        *sw += (spv[i] & 65535) == SpvOpSwitch;
        *sel += (spv[i] & 65535) == SpvOpSelectionMerge;
        *loops += (spv[i] & 65535) == SpvOpLoopMerge;
    }
    free(spv);
    free(p);
    return 0;
}

int main(void) {
    unsigned sw, sel, loops;
    option("PS5C_NO_STRUCTURED", NULL);
    CHECK(!counts(chain, PS5C_ARRAY_COUNT(chain), &sw, &sel, &loops));
    CHECK(sw == 0 && sel == 3 && loops == 0); /* scc, vccz, vccz */
    CHECK(!counts(discard, PS5C_ARRAY_COUNT(discard), &sw, &sel, &loops));
    CHECK(sw == 0 && loops == 1); /* the program runs as a loop once; its break ends it */
    CHECK(!counts(exit_block, PS5C_ARRAY_COUNT(exit_block), &sw, &sel, &loops));
    CHECK(sw == 0 && loops == 1);
    CHECK(!counts(waterfall, PS5C_ARRAY_COUNT(waterfall), &sw, &sel, &loops));
    CHECK(sw == 0 && loops == 0);
    uint32_t aliased[PS5C_ARRAY_COUNT(waterfall)];
    for (size_t i = 0; i < PS5C_ARRAY_COUNT(waterfall); i++) aliased[i] = waterfall[i];
    aliased[2] = 0x7e280502; /* v_readfirstlane_b32 s20, v2 overwrites the saved mask */
    aliased[3] = 0x7da40414; /* v_cmpx_eq_u32 s20, v2 */
    CHECK(!counts(aliased, PS5C_ARRAY_COUNT(aliased), &sw, &sel, &loops));
    CHECK(loops == 1);
    option("PS5C_NO_STRUCTURED", "1");
    CHECK(!counts(chain, PS5C_ARRAY_COUNT(chain), &sw, &sel, &loops));
    CHECK(sw == 1); /* the dispatch loop it used to be */
    puts("shader_structured: ok");
    return 0;
}
