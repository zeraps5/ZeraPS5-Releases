/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Synthetic DPP8 programs: permutations, float addition and inactive sources;
 * DPP16 row shifts and quad_perm over inactive sources (fetch_inactive, bound_ctrl).
 * PS5C_TEST_GPU=1 checks all 64 lanes on the host Vulkan device. */
#include "gpu/gpu_regs.h"
#include "gpu/renderer.h"
#include "gpu/shader/shader.h"
#include "gpu/shader/spv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

typedef struct fixture {
    uint8_t lanes[8];
    bool fi, masked, add, compare;
    uint16_t dpp16; /* DPP16 dpp_ctrl instead of the DPP8 lanes, 0 if none */
    bool bc;
} fixture;
static const fixture cases[] = {
    {{0, 1, 2, 3, 4, 5, 6, 7}, 0, 0, 0, 0, 0, 0}, {{7, 6, 5, 4, 3, 2, 1, 0}, 0, 0, 0, 0, 0, 0},
    {{0, 0, 0, 0, 0, 0, 0, 0}, 0, 0, 0, 0, 0, 0}, {{3, 0, 7, 1, 6, 2, 5, 4}, 1, 0, 0, 0, 0, 0},
    {{7, 6, 5, 4, 3, 2, 1, 0}, 0, 1, 0, 0, 0, 0}, {{7, 6, 5, 4, 3, 2, 1, 0}, 1, 1, 0, 0, 0, 0},
    {{1, 0, 3, 2, 5, 4, 7, 6}, 0, 0, 1, 0, 0, 0}, {{1, 0, 3, 2, 5, 4, 7, 6}, 0, 1, 1, 0, 0, 0},
    {{1, 0, 3, 2, 5, 4, 7, 6}, 1, 1, 1, 0, 0, 0}, {{0, 1, 2, 3, 4, 5, 6, 7}, 0, 0, 0, 1, 0, 0},
    {{7, 6, 5, 4, 3, 2, 1, 0}, 1, 0, 0, 1, 0, 0},
    /* DPP16 over even lanes: row_shr:1 reads odd (inactive) lanes, row_shr:2 even ones */
    {{0}, 0, 1, 0, 0, 0x111, 0}, {{0}, 1, 1, 0, 0, 0x111, 0}, {{0}, 0, 1, 0, 0, 0x111, 1},
    {{0}, 0, 1, 0, 0, 0x112, 0}, {{0}, 0, 0, 0, 0, 0x111, 0}, {{0}, 0, 0, 0, 0, 0x111, 1},
    {{0}, 0, 1, 0, 0, 0xB1, 0}, {{0}, 1, 1, 0, 0, 0xB1, 0}, {{0}, 0, 1, 0, 0, 0xB1, 1},
};
/* The lane a DPP16 lane reads, -1 outside its row */
static int dpp16_lane(uint32_t ctrl, unsigned i) {
    if (ctrl <= 0xFF) return (int)((i & ~3u) + ((ctrl >> 2 * (i & 3)) & 3));
    unsigned n = ctrl & 15; /* row_shr */
    return (i & 15) >= n ? (int)(i - n) : -1;
}
static uint32_t program(uint32_t code[20], const fixture *c) {
    uint32_t n = 0, control = 1;
    for (uint32_t i = 0; i < 8; i++)
        control |= (uint32_t)c->lanes[i] << (8 + 3 * i);
    code[n++] = c->add ? 0x7e020d00 : 0x7e020300; /* v1 = float(v0) or v0 */
    code[n++] = 0x7e040200;                       /* v2=s0: inactive destinations retain sentinel */
    code[n++] = 0x7e060201;                       /* v3=s1: float addend */
    if (c->masked) {
        code[n++] = 0x36080081; /* v_and_b32 v4, 1, v0 */
        code[n++] = 0x7d840880; /* v_cmp_eq_u32 vcc, 0, v4 */
        code[n++] = 0xbefe046a; /* s_mov_b64 exec, vcc: only even lanes */
    }
    uint32_t source = c->dpp16 ? RDNA2_OP_DPP16 : c->fi ? RDNA2_OP_DPP8FI : RDNA2_OP_DPP8;
    code[n++] = (c->compare ? 0x7d840000 : c->add ? 0x06040600 : 0x7e040200) | source;
    if (c->dpp16) control = 1 | (uint32_t)c->dpp16 << 8 | (uint32_t)c->fi << 18 | (uint32_t)c->bc << 19 | 0xFFu << 24;
    code[n++] = control;
    if (c->compare)
        code[n++] = 0x7e04026a; /* v2=vcc (this lane's compare result) */
    code[n++] = 0xbefe04c1;     /* s_mov_b64 exec, -1: all lanes store */
    code[n++] = 0xe0702000;
    code[n++] = 0x80030200; /* store v2, indexed by v0 */
    code[n++] = 0xbf810000;
    return n;
}
static int gpu_test(void) {
    CHECK(pal_init() == PS5C_OK);
    gmem m;
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 4 * GUEST_PAGE_SIZE, GPROT_RW, 0, "DPP8", &a) == PS5C_OK);
    uint32_t *output = gmem_host(&m, a + 0x2000, 64 * 4);
    CHECK(output);
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 DPP8 regression", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m);
    CHECK(r);
    gpu g;
    CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    g.regs[mmCOMPUTE_PGM_RSRC2] = (16u << 1) | (1u << 11);
    g.regs[mmCOMPUTE_NUM_THREAD_X] = 64;
    g.regs[mmCOMPUTE_NUM_THREAD_Y] = g.regs[mmCOMPUTE_NUM_THREAD_Z] = 1;
    /* The native32 run also sets CS_WAVE_FROM_PACKET and CS_NATIVE32 in the
     * harness, so these same inactive-source fixtures exercise raw EXEC masks. */
    g.regs[mmCOMPUTE_DISPATCH_INITIATOR] = getenv("PS5C_TEST_DPP_NATIVE32") ? 0x8000u : 0;
    uint32_t *u = g.regs + mmCOMPUTE_USER_DATA_0;
    u[0] = 0xdeadbeef;
    u[1] = 0x3f800000;
    u[12] = (uint32_t)(a + 0x2000);
    u[13] = (uint32_t)((a + 0x2000) >> 32) | (4u << 16);
    u[14] = 64;
    u[15] = 0x00016204;
    for (unsigned k = 0; k < PS5C_ARRAY_COUNT(cases); k++) {
        uint32_t code[20], n = program(code, &cases[k]);
        guest_addr pc = a + k * 0x100;
        CHECK(gmem_write(&m, pc, code, n * 4));
        g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(pc >> 8);
        g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)(pc >> 40);
        for (unsigned i = 0; i < 64; i++)
            output[i] = 0xabababab;
        renderer_submit_begin(r);
        renderer_dispatch(r, &g, 1, 1, 1);
        renderer_sync_mem(r, a + 0x2000, 64 * 4);
        for (unsigned i = 0; i < 64; i++) {
            unsigned j = (i & ~7u) + cases[k].lanes[i & 7];
            uint32_t expected = j;
            if (cases[k].masked && !cases[k].fi && (j & 1))
                expected = 0;
            if (cases[k].dpp16) { /* an invalid source reads 0 with bound_ctrl, else the lane keeps v2 */
                int s = dpp16_lane(cases[k].dpp16, i);
                bool valid = s >= 0 && !(cases[k].masked && !cases[k].fi && (s & 1));
                expected = valid ? (uint32_t)s : cases[k].bc ? 0 : 0xdeadbeef;
            }
            if (cases[k].compare) {
                expected = j == i;
                if (getenv("PS5C_TEST_DPP_NATIVE32") || getenv("PS5C_TEST_DPP_PAIRED64")) {
                    /* v_mov from VCC reads the whole 32-lane mask in this
                     * native32 or paired64 mode, rather than the fallback's
                     * per-invocation predicate. These cases repeat per32. */
                    expected = 0;
                    for (unsigned lane = 0; lane < 32; lane++) {
                        unsigned source = (lane & ~7u) + cases[k].lanes[lane & 7];
                        if (source == lane) expected |= 1u << lane;
                    }
                }
            }
            if (cases[k].add) {
                float value = (float)expected + 1;
                memcpy(&expected, &value, 4);
            }
            if (cases[k].masked && (i & 1))
                expected = 0xdeadbeef;
            if (output[i] != expected)
                fprintf(stderr, "case %u lane %u: %08x expected %08x\n", k, i, output[i], expected);
            CHECK(output[i] == expected);
        }
    }
    renderer_destroy(r);
    free(g.regs);
    pal_mutex_destroy(g.lock);
    pal_window_destroy(window);
    gmem_free(&m);
    pal_shutdown();
    puts("DPP8/DPP16 GPU: 20 cases x 64 lanes verified");
    return 0;
}
/* Quad-only shaders must declare every subgroup operation they actually use.
 * The larger fixtures also use row shuffles, hiding an undeclared shuffle in
 * the inactive-source check of a quad permutation. */
static int quad_only_modules(void) {
    sh_program *p = calloc(1, sizeof(*p));
    CHECK(p);
    for (unsigned pixel = 0; pixel < 2; pixel++) for (unsigned fi = 0; fi < 2; fi++) {
        uint32_t code[9] = {0x7e0002ff, 0x3e800000,
                           0x7e020200 | RDNA2_OP_DPP16,
                           0xff00b100u | (fi << 18)};
        unsigned n = 4;
        if (pixel) { code[n++] = 0xf800080f; code[n++] = 0x01010101; }
        code[n++] = 0xbf810000;
        sh_stage_info info = {.stage = pixel ? SH_STAGE_PS : SH_STAGE_CS,
                              .local_size = {32, 1, 1}};
        uint32_t *spv = NULL; size_t words = 0;
        CHECK(sh_analyze(code, n, 0x100000, p));
        CHECK(sh_translate(p, &info, NULL, 0, &spv, &words));
        bool shuffle = false, shuffle_cap = false, quad = false, quad_cap = false;
        for (size_t i = 5; i < words; i += spv[i] >> 16) {
            CHECK((spv[i] >> 16) && i + (spv[i] >> 16) <= words);
            uint32_t op = spv[i] & 0xffff;
            shuffle |= op == SpvOpGroupNonUniformShuffle;
            quad |= op == SpvOpGroupNonUniformQuadBroadcast;
            if (op == SpvOpCapability) {
                shuffle_cap |= spv[i+1] == SpvCapabilityGroupNonUniformShuffle;
                quad_cap |= spv[i+1] == SpvCapabilityGroupNonUniformQuad;
            }
        }
        const char *dir = getenv("PS5C_TEST_SPIRV");
        if (dir) {
            char path[512];
            snprintf(path, sizeof(path), "%s/quad_only_%s_fi%u.spv", dir, pixel ? "ps" : "cs", fi);
            FILE *f = fopen(path, "wb"); CHECK(f);
            CHECK(fwrite(spv, 4, words, f) == words); CHECK(fclose(f) == 0);
        }
        if (shuffle && !shuffle_cap)
            fprintf(stderr, "quad-only %s fi%u uses an undeclared subgroup shuffle\n", pixel ? "PS" : "CS", fi);
        free(spv);
        CHECK((!shuffle || shuffle_cap) && quad && quad_cap);
    }
    free(p);
    return 0;
}
int main(void) {
    CHECK(quad_only_modules() == 0);
    sh_program *p = calloc(1, sizeof(*p));
    CHECK(p);
    sh_stage_info info = {
        .stage = SH_STAGE_CS, .cs_tid_components = 1, .cs_tgid_sgpr = 16, .local_size = {64, 1, 1}};
    sh_site_shape shape = {0};
    for (unsigned k = 0; k < PS5C_ARRAY_COUNT(cases); k++) {
        uint32_t code[20], n = program(code, &cases[k]), *spv = NULL;
        size_t words = 0;
        CHECK(sh_analyze(code, n, 0x100000, p));
        CHECK(sh_translate(p, &info, &shape, 0, &spv, &words));
        unsigned shuffles = 0;
        for (size_t i = 5; i < words; i += spv[i] >> 16)
            shuffles += (spv[i] & 0xffff) == 345;
        CHECK(shuffles > 0 || cases[k].dpp16);
        const char *dir = getenv("PS5C_TEST_SPIRV");
        if (dir) {
            char path[512];
            snprintf(path, sizeof(path), "%s/dpp8_%u.spv", dir, k);
            FILE *f = fopen(path, "wb");
            CHECK(f);
            CHECK(fwrite(spv, 4, words, f) == words);
            CHECK(fclose(f) == 0);
        }
        free(spv);
    }
    free(p);
    if (getenv("PS5C_TEST_GPU"))
        CHECK(gpu_test() == 0);
    puts("DPP8 translation: ok");
    return 0;
}
