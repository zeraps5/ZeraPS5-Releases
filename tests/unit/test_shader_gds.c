/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GDS (global data share) through DS instructions with the gds bit: per-lane
 * writes, atomic adds without and with the old value. PS5C_TEST_GPU=1 runs it
 * on the host Vulkan device and checks the GDS memory afterwards. */
#include "gpu/gpu_regs.h"
#include "gpu/renderer.h"
#include "gpu/shader/shader.h"
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

static const uint32_t code[] = {
    0xbefc0380,             /* s_mov_b32 m0, 0 */
    0x34020082,             /* v_lshlrev_b32 v1, 2, v0: lane * 4 */
    0x7e0602ff, 0x00000100, /* v_mov_b32 v3, 0x100 */
    0x7e0a0281,             /* v_mov_b32 v5, 1 */
    0xd8360000, 0x00000001, /* ds_write_b32 v1, v0 gds: GDS[lane] = lane */
    0xd8020000, 0x00000003, /* ds_add_u32 v3, v0 gds: GDS[64] += lane */
    0xd8820004, 0x04000503, /* ds_add_rtn_u32 v4, v3, v5 offset:4 gds: v4 = GDS[65]++ */
    0xd8360200, 0x00000401, /* ds_write_b32 v1, v4 offset:0x200 gds: GDS[128 + lane] = v4 */
    0xbf810000,             /* s_endpgm */
};

static int gpu_test(void) {
    CHECK(pal_init() == PS5C_OK);
    gmem m;
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 0x20000, GPROT_RW, 0, "GDS", &a) == PS5C_OK);
    sh_gds_base = a + 0x10000;
    uint32_t *gds = gmem_host(&m, sh_gds_base, SH_GDS_SIZE);
    CHECK(gds);
    memset(gds, 0, SH_GDS_SIZE);
    CHECK(gmem_write(&m, a, code, sizeof(code)));
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 GDS regression", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m);
    CHECK(r);
    gpu g;
    CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    g.regs[mmCOMPUTE_PGM_RSRC2] = (16u << 1) | (1u << 11);
    g.regs[mmCOMPUTE_NUM_THREAD_X] = 64;
    g.regs[mmCOMPUTE_NUM_THREAD_Y] = g.regs[mmCOMPUTE_NUM_THREAD_Z] = 1;
    g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(a >> 8);
    g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)(a >> 40);
    renderer_submit_begin(r);
    renderer_dispatch(r, &g, 1, 1, 1);
    /* No RELEASE_MEM/label and no explicit CPU invalidation. Once the CP
     * runs out of work, guest CPU polling must see completed GPU stores. */
    renderer_gpu_idle(r);
    uint64_t seen = 0;
    for (unsigned i = 0; i < 64; i++) {
        if (gds[i] != i) fprintf(stderr, "GDS[%u] = %u\n", i, gds[i]);
        CHECK(gds[i] == i);
        CHECK(gds[128 + i] < 64 && !(seen >> gds[128 + i] & 1));
        seen |= 1ull << gds[128 + i];
    }
    if (gds[64] != 2016 || gds[65] != 64) fprintf(stderr, "GDS[64] = %u, GDS[65] = %u\n", gds[64], gds[65]);
    CHECK(gds[64] == 2016 && gds[65] == 64);
    gds[64]=0xa5a5a5a5u;
    renderer_gpu_idle(r);
    CHECK(gds[64]==0xa5a5a5a5u); /* an idle hint must not replay old writebacks */
    renderer_destroy(r);
    free(g.regs);
    pal_mutex_destroy(g.lock);
    pal_window_destroy(window);
    gmem_free(&m);
    pal_shutdown();
    puts("GDS GPU: writes, atomic adds and their old values verified");
    return 0;
}

int main(void) {
    sh_program *p = calloc(1, sizeof(*p));
    CHECK(p);
    sh_stage_info info = {
        .stage = SH_STAGE_CS, .cs_tid_components = 1, .cs_tgid_sgpr = 16, .local_size = {64, 1, 1}};
    sh_site_shape shape[SH_MAX_SITES] = {0};
    uint32_t *spv = NULL;
    size_t words = 0;
    CHECK(sh_analyze(code, PS5C_ARRAY_COUNT(code), 0x100000, p));
    CHECK(sh_translate(p, &info, shape, 0, &spv, &words));
    free(spv);
    free(p);
    if (getenv("PS5C_TEST_GPU"))
        CHECK(gpu_test() == 0);
    puts("GDS translation: ok");
    return 0;
}
