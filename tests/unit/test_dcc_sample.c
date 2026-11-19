/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A DCC clear must be visible to image reads before the next color draw. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/vk/vk_api.h"
#include "gpu/vk/vk_internal.h"
#include "gpu/vk/vk_dcc_metadata.h"
#include "gpu/gpu_regs.h"
#include "gpu/shader/shader.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

/* Synthetic programs only. The degenerate triangle registers a color target. */
static const uint32_t vs[] = {
    0x7e000280, 0x7e020280, 0x7e040280, 0x7e0602f2,
    0xf80008cf, 0x03020100, 0xbf810000
};
static const uint32_t ps[] = {
    0x7e0002f2, 0x7e020280, 0x7e040280, 0x7e0602f2,
    0xf800180f, 0x03020100, 0xbf810000
};
static const uint32_t load[] = {
    0x7e000280, 0x7e020280,
    0xf0000f08, 0x00000400, /* image_load v[4:7], v[0:1], s[0:7] */
    0xbf8c3f70, 0xe0780000, 0x80030400, 0xbf810000
};
static const uint32_t clear_metadata[] = {
    0x7e000280, 0xe0700000, 0x80000000, 0xbf810000 /* v0=0; buffer_store_dword v0, s[0:3] */
};

static int gpu_test(void) {
    CHECK(pal_init() == PS5C_OK);
    gmem m; gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 8 * GUEST_PAGE_SIZE, GPROT_RW, 0, "DCC sample", &a) == PS5C_OK);
    CHECK(gmem_write(&m, a, vs, sizeof(vs)) && gmem_write(&m, a + 0x100, ps, sizeof(ps)) &&
          gmem_write(&m, a + 0x200, load, sizeof(load)) &&
          gmem_write(&m, a + 0x300, clear_metadata, sizeof(clear_metadata)));
    guest_addr image = a + 0x4000, meta = a + 0x8000, out = a + 0x1000;
    uint32_t *output = gmem_host(&m, out, 16); CHECK(output);
    uint8_t *metadata = gmem_host(&m, meta, 256); CHECK(metadata);
    memset(metadata, 0xff, 256); /* initially not a clear */
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 DCC sampling regression", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m); CHECK(r);
    gpu g; CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    g.regs[mmSPI_SHADER_PGM_LO_ES] = (uint32_t)(a >> 8);
    g.regs[mmSPI_SHADER_PGM_HI_ES] = (uint32_t)(a >> 40);
    g.regs[mmSPI_SHADER_PGM_LO_PS] = (uint32_t)((a + 0x100) >> 8);
    g.regs[mmSPI_SHADER_PGM_HI_PS] = (uint32_t)((a + 0x100) >> 40);
    g.regs[mmCB_COLOR_CONTROL] = 0x10;
    g.regs[mmCB_TARGET_MASK] = 15;
    g.regs[mmCB_COLOR0_BASE] = (uint32_t)(image >> 8);
    g.regs[mmCB_COLOR0_INFO] = (1u << 28) | (10u << 2); /* RGBA8 UNORM + DCC */
    g.regs[mmCB_COLOR0_ATTRIB2] = (15u << 14) | 15;
    g.regs[mmCB_COLOR0_DCC_BASE] = (uint32_t)(meta >> 8);
    g.regs[mmCB_COLOR0_DCC_BASE_EXT] = (uint32_t)(meta >> 40);
    g.regs[mmVGT_PRIMITIVE_TYPE] = GPU_PRIM_TRILIST;
    gpu_draw draw = {.count = 3, .instances = 1};
    renderer_draw(r, &g, &draw);
    renderer_wait_idle(r);
    vk_image *img = vk_image_find(r, image, 16, 16, VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(img && img->gpu_written);
    VkCommandBuffer cmd = vk_begin(r);
    vk_barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue red = {{1, 0, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &all);
    renderer_wait_idle(r);
    g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)((a + 0x200) >> 8);
    g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)((a + 0x200) >> 40);
    g.regs[mmCOMPUTE_PGM_RSRC2] = 16u << 1;
    g.regs[mmCOMPUTE_NUM_THREAD_X] = g.regs[mmCOMPUTE_NUM_THREAD_Y] = g.regs[mmCOMPUTE_NUM_THREAD_Z] = 1;
    uint32_t *u = g.regs + mmCOMPUTE_USER_DATA_0;
    u[0] = (uint32_t)(image >> 8); u[1] = (uint32_t)(image >> 40) | (56u << 20) | (3u << 30);
    u[2] = 3u | (15u << 14); u[3] = (9u << 28) | 0xfacu;
    u[12] = (uint32_t)out; u[13] = (uint32_t)(out >> 32); u[14] = 16; u[15] = 0x00016204;
    renderer_submit_begin(r); renderer_dispatch(r, &g, 1, 1, 1); renderer_sync_mem(r, out, 16);
    CHECK(output[0] == 0x3f800000 && output[1] == 0 && output[2] == 0 && output[3] == 0x3f800000);
    /* Four metadata bytes cover this 1024-byte surface. The first tile is
     * uncompressed, the other three carry a zero clear. A majority must
     * not clear the entire cached image and destroy its valid first texel.
     * This checks preservation only, not partial DCC decoding. */
    memset(metadata, 0, 256); metadata[0] = 0xff;
    renderer_mem_written(r, meta, 4);
    renderer_submit_begin(r); renderer_dispatch(r, &g, 1, 1, 1); renderer_sync_mem(r, out, 16);
    fprintf(stderr, "DCC mixed preserved texel: %08x %08x %08x %08x (expected red)\n",
            output[0], output[1], output[2], output[3]);
    CHECK(output[0] == 0x3f800000 && output[1] == 0 && output[2] == 0 && output[3] == 0x3f800000);
    /* DMA clears DCC; no further draw registers or clears this target. */
    memset(metadata, 0, 256);
    renderer_mem_written(r, meta, 256);
    memset(output, 0xcc, 16);
    renderer_submit_begin(r); renderer_dispatch(r, &g, 1, 1, 1); renderer_sync_mem(r, out, 16);
    fprintf(stderr, "DCC sampled texel: %08x %08x %08x %08x (expected zero)\n", output[0], output[1], output[2], output[3]);
    for (unsigned i = 0; i < 4; i++) CHECK(output[i] == 0);
    /* The clear is consumed once: a later GPU write stays visible. */
    cmd = vk_begin(r);
    vk_barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdClearColorImage(cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &all);
    renderer_wait_idle(r);
    renderer_submit_begin(r); renderer_dispatch(r, &g, 1, 1, 1); renderer_sync_mem(r, out, 16);
    CHECK(output[0] == 0x3f800000 && output[3] == 0x3f800000);
    memset(metadata, 0xff, 256); renderer_mem_written(r, meta, 256);
    renderer_submit_begin(r); renderer_dispatch(r, &g, 1, 1, 1); renderer_sync_mem(r, out, 16);
    CHECK(output[0] == 0x3f800000 && output[3] == 0x3f800000); /* not a clear */
    memset(metadata, 0x40, 256); renderer_mem_written(r, meta, 256);
    renderer_submit_begin(r); renderer_dispatch(r, &g, 1, 1, 1); renderer_sync_mem(r, out, 16);
    CHECK(output[0] == 0 && output[1] == 0 && output[2] == 0 && output[3] == 0x3f800000);
    /* Compute clears metadata in the same batch. Its pending buffer results
     * must land before reading the codes and retrying resource bindings. */
    cmd = vk_begin(r);
    vk_barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdClearColorImage(cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &all);
    memset(metadata, 0xff, 256);
    uint32_t saved[16]; memcpy(saved, u, sizeof(saved));
    u[0] = (uint32_t)meta; u[1] = (uint32_t)(meta >> 32); u[2] = 4; u[3] = 0x00016204;
    g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)((a + 0x300) >> 8);
    renderer_submit_begin(r); renderer_dispatch(r, &g, 1, 1, 1);
    memcpy(u, saved, sizeof(saved));
    g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)((a + 0x200) >> 8);
    renderer_submit_begin(r); renderer_dispatch(r, &g, 1, 1, 1); renderer_sync_mem(r, out, 16);
    for (unsigned i = 0; i < 4; i++) CHECK(output[i] == 0);
    puts("DCC sampling GPU: mixed preservation, DMA, compute pending, non-clear and single consumption passed");
    renderer_destroy(r); free(g.regs); pal_mutex_destroy(g.lock);
    pal_window_destroy(window); gmem_free(&m); pal_shutdown();
    return 0;
}
int main(void) {
    /* A rare unlike byte, including one skipped by the old 4096-sample
     * policy, must prevent a whole-image clear. Exercise unaligned input
     * and the short tail as well as large metadata intervals. */
    uint8_t metadata[16387];
    static const uint8_t codes[] = {0x00, 0x40, 0x80, 0xc0, 0x20};
    static const size_t changed[] = {0, 1, 7, 8, 4095, 4096, 8191, 16385, 16386};
    CHECK(vk_dcc_uniform_clear(NULL, 0) == -1);
    CHECK(vk_dcc_uniform_clear(metadata, 0) == -1);
    for (unsigned c = 0; c < sizeof(codes); c++) {
        memset(metadata, codes[c], sizeof(metadata));
        CHECK(vk_dcc_uniform_clear(metadata, sizeof(metadata)) == codes[c]);
        CHECK(vk_dcc_uniform_clear(metadata + 1, sizeof(metadata) - 1) == codes[c]);
        for (size_t k = 0; k < PS5C_ARRAY_COUNT(changed); k++) {
            size_t i = changed[k];
            metadata[i] = 0xff;
            CHECK(vk_dcc_uniform_clear(metadata, sizeof(metadata)) == -1);
            metadata[i] = codes[c];
        }
    }
    memset(metadata, 0xff, sizeof(metadata));
    CHECK(vk_dcc_uniform_clear(metadata, sizeof(metadata)) == -1);
    memset(metadata, 0x17, sizeof(metadata));
    CHECK(vk_dcc_uniform_clear(metadata, sizeof(metadata)) == -1);
    sh_program *p = calloc(1, sizeof(*p)); CHECK(p);
    CHECK(sh_analyze(vs, PS5C_ARRAY_COUNT(vs), 0x100000, p));
    CHECK(sh_analyze(ps, PS5C_ARRAY_COUNT(ps), 0x100100, p) && p->mrt_mask == 1);
    CHECK(sh_analyze(load, PS5C_ARRAY_COUNT(load), 0x100200, p) && p->site_count == 2);
    CHECK(sh_analyze(clear_metadata, PS5C_ARRAY_COUNT(clear_metadata), 0x100300, p));
    free(p);
    if (getenv("PS5C_TEST_GPU")) return gpu_test();
    puts("DCC sample: native regression available with PS5C_TEST_GPU=1");
    return 0;
}
