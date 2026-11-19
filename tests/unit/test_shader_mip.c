/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Uniform mip copy: reject lane/flow-dependent addresses, select the actual
 * descriptor level. PS5C_TEST_GPU=1 verifies stores and other levels on Vulkan. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/gpu_regs.h"
#include "gpu/renderer.h"
#include "gpu/shader/shader.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static const uint32_t store[] = {
    0x7e000280, 0x7e020280, 0x7e040208, /* v0=0, v1=0, v2=s8 (mip) */
    0x7e080281, 0x7e0a0282, 0x7e0c0283, 0x7e0e0284, /* uint texel 1,2,3,4 */
    0xf0240f08, 0x00000400, /* image_store_mip v[4:7], v[0:2], s[0:7] */
    0xbf810000
};
static const uint32_t load[] = {
    0x7e000280, 0x7e020280, 0x7e040208,
    0xf0040f08, 0x00000400, /* image_load_mip v[4:7], v[0:2], s[0:7] */
    0xbf8c3f70, /* s_waitcnt vmcnt(0) */
    0xe0780000, 0x80030400, /* buffer_store_dwordx4 v[4:7], off, s[12:15], 0 */
    0xbf810000
};

static int gpu_test(void) {
    CHECK(pal_init() == PS5C_OK);
    gmem m;
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 8 * GUEST_PAGE_SIZE, GPROT_RW, 0, "mip fixture", &a) == PS5C_OK);
    CHECK(gmem_write(&m, a, store, sizeof(store)));
    CHECK(gmem_write(&m, a + 0x100, load, sizeof(load)));
    uint8_t *pixels = gmem_host(&m, a + 0x2000, 8192);
    uint32_t *output = gmem_host(&m, a + 0x1000, 16);
    CHECK(pixels && output);
    memset(pixels, 0, 8192);
    /* GFX10 linear layout: 256-byte rows, 4x4 then 8x8 then 16x16. */
    const uint64_t offsets[] = {12 * 256, 4 * 256, 0};
    for (uint32_t level = 0; level < 3; level++) {
        uint64_t off = offsets[level];
        for (uint32_t c = 0; c < 4; c++) pixels[off + c] = (uint8_t)(100 + level * 10 + c);
    }
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 synthetic mip regression", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m);
    CHECK(r);
    gpu g;
    CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    g.regs[mmCOMPUTE_PGM_RSRC2] = 16u << 1;
    g.regs[mmCOMPUTE_NUM_THREAD_X] = g.regs[mmCOMPUTE_NUM_THREAD_Y] = g.regs[mmCOMPUTE_NUM_THREAD_Z] = 1;
    uint32_t *u = g.regs + mmCOMPUTE_USER_DATA_0;
    guest_addr image = a + 0x2000;
    u[0] = (uint32_t)(image >> 8);
    u[1] = (uint32_t)(image >> 40) | (60u << 20) | (3u << 30); /* RGBA8 uint */
    u[2] = 3u | (15u << 14); /* 16x16 */
    u[3] = (9u << 28) | (2u << 16) | 0xfacu;
    u[12] = (uint32_t)(a + 0x1000);
    u[13] = (uint32_t)((a + 0x1000) >> 32);
    u[14] = 16;
    u[15] = 0x00016204;
    u[8] = 1;
    g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(a >> 8);
    g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)(a >> 40);
    renderer_dispatch(r, &g, 1, 1, 1);
    g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)((a + 0x100) >> 8);
    for (uint32_t level = 0; level < 3; level++) {
        u[8] = level;
        renderer_submit_begin(r);
        renderer_dispatch(r, &g, 1, 1, 1);
        renderer_sync_mem(r, a + 0x1000, 16);
        for (uint32_t c = 0; c < 4; c++) {
            uint32_t expected = level == 1 ? c + 1 : 100 + level * 10 + c;
            if (output[c] != expected) fprintf(stderr, "mip %u component %u: %u expected %u\n", level, c, output[c], expected);
            CHECK(output[c] == expected);
        }
    }
    /* The address is relative to BASE_LEVEL, including on the same pipeline. */
    u[3] |= 1u << 12;
    u[8] = 1;
    renderer_submit_begin(r);
    renderer_dispatch(r, &g, 1, 1, 1);
    renderer_sync_mem(r, a + 0x1000, 16);
    for (uint32_t c = 0; c < 4; c++) CHECK(output[c] == 120 + c);
    renderer_destroy(r);
    free(g.regs);
    pal_mutex_destroy(g.lock);
    pal_window_destroy(window);
    gmem_free(&m);
    pal_shutdown();
    puts("mip GPU: level 1 written, levels 0 and 2 preserved");
    return 0;
}

int main(void) {
    sh_program *p = calloc(1, sizeof(*p));
    CHECK(p);
    sh_stage_info info = {.stage = SH_STAGE_CS, .cs_tgid_sgpr = 16, .local_size = {1, 1, 1}};
    uint32_t source, literal;
    CHECK(sh_analyze(store, PS5C_ARRAY_COUNT(store), 0x100000, p));
    CHECK(sh_image_mip_source(p, &info, 0, &source, &literal) && source == 8);
    sh_site_shape shape = {.img_type = 9, .img_kind = 1};
    uint32_t *spv = NULL;
    size_t words;
    CHECK(sh_translate(p, &info, &shape, 0, &spv, &words));
    bool write = false;
    for (size_t i = 5; i < words; i += spv[i] >> 16) write |= (spv[i] & 0xffff) == 99; /* OpImageWrite */
    CHECK(write);
    if (getenv("PS5C_TEST_SPIRV")) {
        char path[512];
        snprintf(path, sizeof(path), "%s/mip-store.spv", getenv("PS5C_TEST_SPIRV"));
        FILE *f = fopen(path, "wb");
        CHECK(f && fwrite(spv, 4, words, f) == words && fclose(f) == 0);
    }
    free(spv);
    uint32_t mutated[PS5C_ARRAY_COUNT(store)];
    memcpy(mutated, store, sizeof(store));
    mutated[2] = 0x7e040282; /* literal mip 2 */
    CHECK(sh_analyze(mutated, PS5C_ARRAY_COUNT(mutated), 0x100000, p));
    CHECK(sh_image_mip_source(p, &info, 0, &source, &literal) && source == RDNA2_OP_LITERAL && literal == 2);
    mutated[2] = 0x7e040210; /* workgroup id is not a user SGPR */
    CHECK(sh_analyze(mutated, PS5C_ARRAY_COUNT(mutated), 0x100000, p));
    CHECK(!sh_image_mip_source(p, &info, 0, &source, &literal));
    uint32_t scalar_load[PS5C_ARRAY_COUNT(store) + 2];
    scalar_load[0] = 0xf4000200; scalar_load[1] = 0xfa000000; /* s_load_dword s8 */
    memcpy(scalar_load + 2, store, sizeof(store));
    CHECK(sh_analyze(scalar_load, PS5C_ARRAY_COUNT(scalar_load), 0x100000, p));
    CHECK(!sh_image_mip_source(p, &info, 1, &source, &literal));
    uint32_t image_clobber[] = {0x7e040208, 0xf0040f08, 0x00000000, /* writes v[0:3], including LOD */
                                0xf0240f08, 0x00000400, 0xbf810000};
    CHECK(sh_analyze(image_clobber, PS5C_ARRAY_COUNT(image_clobber), 0x100000, p));
    CHECK(!sh_image_mip_source(p, &info, 1, &source, &literal));
    uint32_t nsa[] = {0x7e0c0208, 0xf0240f0a, 0x00000405, 0x00000604, 0xbf810000};
    CHECK(sh_analyze(nsa, PS5C_ARRAY_COUNT(nsa), 0x100000, p));
    CHECK(sh_image_mip_source(p, &info, 0, &source, &literal) && source == 8);
    mutated[2] = 0x7e040300; /* v_mov_b32 v2, v0: lane-dependent */
    CHECK(sh_analyze(mutated, PS5C_ARRAY_COUNT(mutated), 0x100000, p));
    CHECK(!sh_image_mip_source(p, &info, 0, &source, &literal));
    memcpy(mutated, store, sizeof(store));
    mutated[3] = 0x4a040100; /* v_add_nc_u32 v2, v0, v0 clobbers the LOD */
    CHECK(sh_analyze(mutated, PS5C_ARRAY_COUNT(mutated), 0x100000, p));
    CHECK(!sh_image_mip_source(p, &info, 0, &source, &literal));
    memcpy(mutated, store, sizeof(store));
    mutated[0] = 0xbf820000; /* branch, even to next instruction, is not proven */
    CHECK(sh_analyze(mutated, PS5C_ARRAY_COUNT(mutated), 0x100000, p));
    CHECK(!sh_image_mip_source(p, &info, 0, &source, &literal));
    /* With branches: a constant mip written on the path every storing lane takes (mip-chain shaders) */
    uint32_t chain[] = {0x7e000280, 0x7e020280, 0x7da41080, /* v_cmpx_eq_u32 0, v8 */
                        0xbf880003,                         /* s_cbranch_execz past the store */
                        0x7e040281,                         /* v_mov_b32 v2, 1 */
                        0xf0240f08, 0x00000400, 0xbf810000};
    CHECK(sh_analyze(chain, PS5C_ARRAY_COUNT(chain), 0x100000, p));
    CHECK(p->has_flow);
    CHECK(sh_image_mip_source(p, &info, 0, &source, &literal) && source == RDNA2_OP_LITERAL && literal == 1);
    CHECK(sh_translate(p, &info, &shape, 0, &spv, &words));
    free(spv);
    uint32_t skipped[] = {0x7e000280, 0x7e020280, 0x7e040280, /* v2 = 0 */
                          0xbf880000 | 1,                     /* s_cbranch_execz over the mov, onto the store */
                          0x7e040281, 0xf0240f08, 0x00000400, 0xbf810000};
    CHECK(sh_analyze(skipped, PS5C_ARRAY_COUNT(skipped), 0x100000, p));
    CHECK(!sh_image_mip_source(p, &info, 0, &source, &literal));
    uint32_t widened[] = {0xbf880000, 0x7e000280, 0x7e020280, 0x7e040281,
                          0xbefe04c1, /* s_mov_b64 exec, -1: lanes that skipped the mov */
                          0xf0240f08, 0x00000400, 0xbf810000};
    CHECK(sh_analyze(widened, PS5C_ARRAY_COUNT(widened), 0x100000, p));
    CHECK(!sh_image_mip_source(p, &info, 0, &source, &literal));
    uint32_t looped[] = {0x7e000280, 0x7e020280, 0x7e040281, 0xf0240f08, 0x00000400,
                         0xbf89fffb, /* s_cbranch_execnz back to the mov */
                         0xbf810000};
    CHECK(sh_analyze(looped, PS5C_ARRAY_COUNT(looped), 0x100000, p));
    CHECK(sh_image_mip_source(p, &info, 0, &source, &literal) && literal == 1); /* the loop re-runs the mov first */
    uint32_t changed[] = {0x7e000280, 0x7e020280, 0x7e040281, 0xf0240f08, 0x00000400,
                          0x4a040481, /* v_add_nc_u32 v2, 1, v2 */
                          0xbf89fffc, /* s_cbranch_execnz back to the store */
                          0xbf810000};
    CHECK(sh_analyze(changed, PS5C_ARRAY_COUNT(changed), 0x100000, p));
    CHECK(!sh_image_mip_source(p, &info, 0, &source, &literal));
    uint32_t desc[8] = {0, 0, 3u | (15u << 14), (9u << 28) | (4u << 16) | (1u << 12)}, selected[8];
    CHECK(sh_image_mip_desc(desc, 2, selected));
    CHECK(((selected[3] >> 12) & 15) == 3 && ((selected[3] >> 16) & 15) == 4);
    CHECK(!sh_image_mip_desc(desc, 4, selected));
    CHECK(!sh_image_mip_desc(desc, UINT32_MAX, selected));
    desc[3] = (9u << 28) | (15u << 16);
    CHECK(!sh_image_mip_desc(desc, 5, selected)); /* 16x16 has five levels */
    desc[3] = (13u << 28) | (4u << 16);
    CHECK(!sh_image_mip_desc(desc, 0, selected)); /* arrays need a different coordinate proof */
    free(p);
    if (getenv("PS5C_TEST_GPU")) CHECK(gpu_test() == 0);
    puts("shader mip: ok");
    return 0;
}
