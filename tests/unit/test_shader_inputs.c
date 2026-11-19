/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pixel input routing: parameter aliases and the four hardware defaults. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/shader/shader.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static uint32_t variables(const uint32_t *spv, size_t words, uint32_t storage) {
    uint32_t n = 0;
    for (size_t i = 5; i < words; i += spv[i] >> 16)
        if ((spv[i] & 0xFFFF) == 59 && spv[i + 3] == storage) n++;
    return n;
}

static bool depth_interface(const uint32_t *spv, size_t words, bool expected) {
    unsigned depth = 0, replacing = 0, stores = 0;
    uint32_t depth_id = 0;
    for (size_t i = 5; i < words; i += spv[i] >> 16) {
        /* OpDecorate BuiltIn FragDepth and OpExecutionMode DepthReplacing. */
        if ((spv[i] & 0xffff) == 71 && (spv[i] >> 16) == 4 && spv[i + 2] == 11 && spv[i + 3] == 22) {
            depth++; depth_id = spv[i + 1];
        }
        if ((spv[i] & 0xffff) == 16 && spv[i + 2] == 12) replacing++;
    }
    for (size_t i = 5; i < words; i += spv[i] >> 16)
        if ((spv[i] & 0xffff) == 62 && spv[i + 1] == depth_id) stores++;
    return depth == (unsigned)expected && replacing == (unsigned)expected && stores == (unsigned)expected;
}

/* The interpolation-only program bitcasts each scalar default from a float
 * constant to the guest VGPR. Check the actual four channel values. */
static bool defaults(const uint32_t *spv, size_t words, uint32_t def) {
    uint32_t n = 0;
    for (size_t i = 5; i < words; i += spv[i] >> 16) {
        if ((spv[i] & 0xFFFF) != 124) continue; /* OpBitcast */
        for (size_t j = 5; j < words; j += spv[j] >> 16) {
            if ((spv[j] & 0xFFFF) != 43 || spv[j + 2] != spv[i + 3]) continue;
            uint32_t one = n == 3 ? def & 1u : (def >> 1) & 1u;
            if (n >= 4 || spv[j + 3] != (one ? 0x3F800000u : 0)) return false;
            n++;
        }
    }
    return n == 4;
}

static bool translate(sh_program *p, sh_stage_info *info, const uint32_t *code, uint32_t count,
                      uint32_t **spv, size_t *words) {
    static unsigned n;
    *spv = NULL;
    if (!sh_analyze(code, count, 0x100000, p) || !sh_translate(p, info, NULL, 0, spv, words)) return false;
    const char *dir = getenv("PS5C_TEST_SPIRV");
    if (dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/inputs%u.spv", dir, n++);
        FILE *f = fopen(path, "wb");
        if (!f) return false;
        bool ok = fwrite(*spv, 4, *words, f) == *words;
        if (fclose(f)) ok = false;
        if (!ok) return false;
    }
    return true;
}

int main(void) {
    sh_program *p = calloc(1, sizeof(*p));
    sh_stage_info info = {.stage = SH_STAGE_PS};
    uint32_t *spv;
    size_t words;
    CHECK(p);
    /* v_interp_p2_f32 v0, v0, attr0.x; same with attr1.x. */
    const uint32_t aliases[] = {0xC8010000u, 0xC8050400u, 0xBF810000u};
    CHECK(translate(p, &info, aliases, 3, &spv, &words));
    CHECK(variables(spv, words, 1) == 1); /* one Input, not two at Location 0 */
    free(spv);
    info.ps_attr_location[1] = 7;
    CHECK(translate(p, &info, aliases, 3, &spv, &words));
    CHECK(variables(spv, words, 1) == 2);
    free(spv);
    info.ps_attr_location[1] = 0;
    info.ps_flat_mask = 3;
    CHECK(translate(p, &info, aliases, 3, &spv, &words));
    CHECK(variables(spv, words, 1) == 1);
    free(spv);
    info.ps_flat_mask = 1;
    CHECK(!translate(p, &info, aliases, 3, &spv, &words));
    free(spv);
    sh_program *vs = calloc(1, sizeof(*vs)); CHECK(vs);
    sh_stage_info vs_info = {.stage = SH_STAGE_VS};
    vs->param_mask = 3; /* location 1 is exported even though the PS does not read it */
    CHECK(sh_link_graphics_inputs(vs, p, &vs_info, &info));
    CHECK(vs_info.vs_param_alias_mask == 1 && vs_info.vs_param_alias[0] == 2);
    CHECK(info.ps_attr_location[0] == 2 && info.ps_attr_location[1] == 0);
    CHECK(translate(p, &info, aliases, 3, &spv, &words));
    CHECK(variables(spv, words, 1) == 2); free(spv);
    /* Full interfaces must fail without changing either stage's routing. */
    info.ps_attr_location[0] = info.ps_attr_location[1] = 0;
    vs->param_mask = UINT32_MAX;
    sh_stage_info saved_vs = vs_info, saved_ps = info;
    CHECK(!sh_link_graphics_inputs(vs, p, &vs_info, &info));
    CHECK(!memcmp(&saved_vs, &vs_info, sizeof(vs_info)) && !memcmp(&saved_ps, &info, sizeof(info)));
    /* A DEFAULT flat attribute consumes no interface location. Reset aliases
     * left by an earlier draw using the same VS. */
    info.ps_attr_location[0] = 0x20;
    CHECK(sh_link_graphics_inputs(vs, p, &vs_info, &info) && !vs_info.vs_param_alias_mask);
    free(vs);
    info.ps_flat_mask = 0;
    /* attr0.xyzw to v0..3. */
    const uint32_t constant[] = {0xC8010000u, 0xC8050100u, 0xC8090200u, 0xC80D0300u, 0xBF810000u};
    for (uint32_t def = 0; def < 4; def++) {
        info.ps_attr_location[0] = sh_ps_input_pack(0x20u | (def << 8));
        CHECK(info.ps_attr_location[0] == (0x20u | (def << 6)));
        CHECK(translate(p, &info, constant, 5, &spv, &words));
        CHECK(variables(spv, words, 1) == 0 && defaults(spv, words, def));
        free(spv);
    }
    CHECK(sh_ps_input_pack(31u | (1u << 10)) == 31u);
    /* MRTZ X exports depth; Y/Z alone must retain rasterized depth. */
    memset(&info, 0, sizeof(info)); info.stage = SH_STAGE_PS;
    for (uint32_t mask = 1; mask < 8; mask++) {
        uint32_t mrtz[] = {0x7e0002f0, 0x7e020281, 0x7e0402c1,
                           0xf8001880u | mask, 0x00020100, 0xbf810000};
        CHECK(translate(p, &info, mrtz, PS5C_ARRAY_COUNT(mrtz), &spv, &words));
        CHECK(depth_interface(spv, words, (mask & 1) != 0));
        free(spv);
    }
    for (uint32_t mask = 0; mask < 16; mask++) {
        uint32_t lo = 0xfffefffeu | (mask & 1u) | ((mask & 2u) << 15);
        uint32_t hi = 0xfffefffeu | ((mask & 4u) >> 2) | ((mask & 8u) << 13);
        CHECK(sh_ps_quad_disabled(lo, hi) == (mask ^ 15u));
    }
    const uint32_t solid[] = {0x7e0002f2,0x7e020280,0x7e040280,0x7e0602f2,
                              0xf800180f,0x03020100,0xbf810000};
    for (uint32_t helpers = 0; helpers < 2; helpers++) {
        info.ps_quad_disable = 13u | (helpers ? SH_PS_QUAD_DEMOTE : 0);
        CHECK(translate(p, &info, solid, PS5C_ARRAY_COUNT(solid), &spv, &words));
        unsigned frag_coord = 0, demote = 0, early_tests = 0;
        for (size_t i = 5; i < words; i += spv[i] >> 16) {
            uint32_t op = spv[i] & 0xffffu;
            frag_coord += op == 71 && (spv[i] >> 16) == 4 && spv[i+2] == 11 && spv[i+3] == 15;
            demote += op == 5380;
            early_tests += op == 16 && spv[i+2] == 9;
        }
        CHECK(frag_coord == 1 && demote == helpers && !early_tests);
        free(spv);
    }
    free(p);
    puts("shader inputs: ok");
    return 0;
}
