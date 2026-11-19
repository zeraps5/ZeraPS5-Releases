/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RESINFO must describe guest mips despite single-level host images. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/gpu_regs.h"
#include "gpu/renderer.h"
#include "gpu/shader/shader.h"
#include "gpu/shader/shader_ops.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static const uint32_t code[] = {
    0x7e000208, /* v0 = s8 (requested LOD, changes on the same pipeline) */
    0xf0380f08, 0x00000400, /* image_get_resinfo v[4:7], v0, s[0:7] */
    0xbf8c3f70,
    0xe0780000, 0x80030400, /* buffer_store_dwordx4 v[4:7], s[12:15] */
    0xbf810000
};
static int gpu_test(void) {
    CHECK(pal_init() == PS5C_OK);
    gmem m;
    gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 8 * GUEST_PAGE_SIZE, GPROT_RW, 0, "resinfo fixture", &a) == PS5C_OK);
    CHECK(gmem_write(&m, a, code, sizeof(code)));
    uint32_t *output = gmem_host(&m, a + 0x1000, 16); CHECK(output);
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 RESINFO regression", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m); CHECK(r);
    gpu g; CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    g.regs[mmCOMPUTE_PGM_RSRC2] = 16u << 1;
    g.regs[mmCOMPUTE_NUM_THREAD_X] = g.regs[mmCOMPUTE_NUM_THREAD_Y] = g.regs[mmCOMPUTE_NUM_THREAD_Z] = 1;
    g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(a >> 8);
    g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)(a >> 40);
    uint32_t *u = g.regs + mmCOMPUTE_USER_DATA_0;
    guest_addr image = a + 0x2000;
    u[0] = (uint32_t)(image >> 8);
    u[1] = (uint32_t)(image >> 40) | (60u << 20) | (3u << 30);
    u[2] = 3u | (7u << 14); /* 16x8 */
    u[3] = (9u << 28) | (4u << 16) | 0xfacu;
    u[12] = (uint32_t)(a + 0x1000); u[13] = (uint32_t)((a + 0x1000) >> 32);
    u[14] = 16; u[15] = 0x00016204;
    const uint32_t expected[][4] = {{16,8,1,5}, {8,4,1,5}, {4,2,1,5}, {2,1,1,5}, {1,1,1,5}};
    for (uint32_t lod = 0; lod < 5; lod++) {
        u[8] = lod; memset(output, 0xcc, 16);
        renderer_submit_begin(r); renderer_dispatch(r, &g, 1, 1, 1);
        renderer_sync_mem(r, a + 0x1000, 16);
        for (unsigned c=0;c<4;c++) {
            if (output[c] != expected[lod][c]) fprintf(stderr,"LOD %u component %u: %u expected %u\n",lod,c,output[c],expected[lod][c]);
            CHECK(output[c] == expected[lod][c]);
        }
    }
    /* BASE_LEVEL is relative and the view's level count excludes earlier levels. */
    u[3] |= 1u << 12; u[8] = 1;
    renderer_submit_begin(r); renderer_dispatch(r, &g, 1, 1, 1);
    renderer_sync_mem(r, a + 0x1000, 16);
    CHECK(output[0] == 4 && output[1] == 2 && output[2] == 1 && output[3] == 4);
    /* Change dimensions without recompiling/rekeying by size. */
    u[1] &= ~(3u << 30); u[2] = 1u | (4u << 14); /* 5x5 */
    u[3] = (9u << 28) | (2u << 16) | 0xfacu; u[8] = 1;
    renderer_submit_begin(r); renderer_dispatch(r, &g, 1, 1, 1);
    renderer_sync_mem(r, a + 0x1000, 16);
    CHECK(output[0] == 2 && output[1] == 2 && output[3] == 3);
    renderer_destroy(r); free(g.regs); pal_mutex_destroy(g.lock);
    pal_window_destroy(window); gmem_free(&m); pal_shutdown();
    puts("resinfo GPU: five mips, base level and live dimensions verified");
    return 0;
}
int main(void) {
    sh_program *p = calloc(1, sizeof(*p)); CHECK(p);
    CHECK(sh_analyze(code, PS5C_ARRAY_COUNT(code), 0x100000, p));
    CHECK(p->ops[1] == SH_OP_IMAGE_GET_RESINFO);
    sh_stage_info info = {.stage=SH_STAGE_CS, .cs_tgid_sgpr=16, .local_size={1,1,1}};
    sh_site_shape shapes[2] = {{.img_type=9, .img_kind=1}, {0}};
    uint32_t *spv = NULL; size_t words;
    CHECK(sh_translate(p, &info, shapes, 0, &spv, &words));
    for (size_t i=5; i<words; i+=spv[i]>>16) CHECK((spv[i]&0xffff) != 103); /* no host QuerySizeLod */
    free(spv); free(p);
    if (getenv("PS5C_TEST_GPU")) CHECK(gpu_test() == 0);
    puts("resinfo: ok"); return 0;
}
