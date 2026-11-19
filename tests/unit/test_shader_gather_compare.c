/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Synthetic comparison gather: four distinct texels, eight comparison funcs,
 * signed offsets, A16 coordinates and D16 results. Optional Vulkan execution. */
#include "gpu/gpu_regs.h"
#include "gpu/renderer.h"
#include "gpu/shader/shader.h"
#include "gpu/shader/shader_ops.h"
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

static uint32_t program(uint32_t code[24], int offset, bool a16, bool d16, bool nsa, bool lz) {
    uint32_t n = 0, v = 0;
    if (offset) {
        code[n++] = 0x7e0002ff;
        code[n++] = offset > 0 ? 0x3f01 : 0x013f;
        v++;
    }
    code[n++] = 0x7e0002ff | (v << 17);
    code[n++] = 0x3f100000;
    v++; /* ref=9/16 */
    if (a16) {
        code[n++] = 0x7e0002ff | (v << 17);
        code[n++] = 0x38003800;
    } else {
        code[n++] = 0x7e0002f0 | (v << 17);
        code[n++] = 0x7e0002f0 | ((v + 1) << 17);
    }
    code[n++] = (offset ? 0xf17c0108 : 0xf13c0108) - (lz ? 0 : 0x1c0000);
    code[n++] = 0x00400400 | (a16 ? 0x40000000u : 0) | (d16 ? 0x80000000u : 0);
    if (nsa) {
        code[n - 2] |= 2;
        code[n++] = 0x00000201;
    } /* v0 then v1,v2 */
    code[n++] = 0xbf8c3f70;
    code[n++] = d16 ? 0xe0740000 : 0xe0780000;
    code[n++] = 0x80030400;
    code[n++] = 0xbf810000;
    return n;
}
static bool compare(unsigned func, unsigned texel) {
    switch (func) {
    case 0: return false;
    case 1: return 9 < texel;
    case 2: return 9 == texel;
    case 3: return 9 <= texel;
    case 4: return 9 > texel;
    case 5: return 9 != texel;
    case 6: return 9 >= texel;
    default: return true;
    }
}

/* Comparison sampling must keep the array slice. Skyrim selects two shadow
 * cascades with image_sample_c_lz; forcing Z=0 changes the shadow test. */
static uint32_t array_sample(uint32_t code[32], unsigned layer, bool lz, bool nsa) {
    uint32_t n=0;
    for (unsigned v=0;v<4;v++) {
        code[n++]=0x7e0002ff | (v<<17);
        code[n++]=v==3 ? (layer ? 0x3f800000 : 0) : v==0 ? 0x3f100000 : 0x3f000000;
    }
    code[n++]=(lz ? 0xf0bc0128 : 0xf0a00128) | (nsa ? 2u : 0u);
    code[n++]=0x00400400;
    if(nsa)code[n++]=0x00030201;
    code[n++]=0xbf8c3f70;
    code[n++]=0xe0780000;code[n++]=0x80030400;
    code[n++]=0xbf810000;
    return n;
}

static int array_gpu_cases(renderer *r,gpu *g,gmem *m,guest_addr a) {
    float *low=gmem_host(m,a+0x2000,4),*high=gmem_host(m,a+0x2100,4);
    uint32_t *output=gmem_host(m,a+0x1000,16),*u=g->regs+mmCOMPUTE_USER_DATA_0;
    CHECK(low && high && output);
    *low=0.25f;*high=0.75f;
    u[1]=(uint32_t)((a+0x2000)>>40)|(22u<<20);
    u[2]=0;u[3]=(13u<<28)|0x924;u[4]=1;
    unsigned cases=0;
    for(unsigned variant=0;variant<2;variant++)
        for(unsigned nsa=0;nsa<2;nsa++)
            for(unsigned layer=0;layer<2;layer++)
                for(unsigned f=0;f<8;f++) {
                    uint32_t code[32],n=array_sample(code,layer,variant!=0,nsa!=0);
                    guest_addr pc=a+0x14000+cases*0x100;
                    CHECK(gmem_write(m,pc,code,n*4));
                    g->regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);
                    g->regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);u[8]=f<<12;
                    output[0]=0xdeadbeef;
                    renderer_submit_begin(r);renderer_dispatch(r,g,1,1,1);
                    renderer_sync_mem(r,a+0x1000,16);
                    /* Same comparison as compare(), using ref 9/16 vs 4/16
                     * or 12/16; these cases have no equal operand. */
                    uint32_t expected=compare(f,layer?12:4)?0x3f800000:0;
                    if(output[0]!=expected)fprintf(stderr,"array sample variant %u nsa %u layer %u func %u: %08x expected %08x\n",variant,nsa,layer,f,output[0],expected);
                    CHECK(output[0]==expected);cases++;
                }
    printf("array comparison GPU: %u cases verified\n",cases);
    return 0;
}
/* Linear shadow filtering compares the four depths BEFORE interpolating.
 * Comparing an interpolated depth incorrectly returns only zero or one. */
static int filtered_array_cases(renderer *r,gpu *g,gmem *m,guest_addr a) {
    uint32_t *u=g->regs+mmCOMPUTE_USER_DATA_0,*output=gmem_host(m,a+0x1000,16);
    const float depths[2][4]={{0,.25f,.75f,1},{1,.75f,.25f,0}};
    const float xy[3][2]={{.5f,.5f},{.375f,.625f},{-.125f,.375f}};
    const float weights[3][4]={{.25f,.25f,.25f,.25f},{.1875f,.0625f,.5625f,.1875f},{.1875f,.5625f,.0625f,.1875f}};
    for(unsigned l=0;l<2;l++)for(unsigned y=0;y<2;y++)for(unsigned x=0;x<2;x++)
        CHECK(gmem_write(m,a+0x3000+l*512+y*256+x*4,&depths[l][y*2+x],4));
    u[0]=(uint32_t)((a+0x3000)>>8);
    u[1]=(uint32_t)((a+0x3000)>>40)|(22u<<20)|(1u<<30);
    u[2]=1u<<14;u[3]=(13u<<28)|0x924;u[4]=1;
    u[10]=(1u<<20)|(1u<<22); /* linear min AND mag, repeat addressing */
    unsigned cases=0;
    for(unsigned lz=0;lz<2;lz++)for(unsigned nsa=0;nsa<2;nsa++)
      for(unsigned layer=0;layer<2;layer++)for(unsigned uv=0;uv<3;uv++)for(unsigned f=0;f<8;f++) {
        uint32_t code[32],n=array_sample(code,layer,lz!=0,nsa!=0);
        memcpy(&code[3],&xy[uv][0],4);memcpy(&code[5],&xy[uv][1],4);
        guest_addr pc=a+0x20000+cases*0x100;
        CHECK(gmem_write(m,pc,code,n*4));
        g->regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);
        g->regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);u[8]=f<<12;
        output[0]=0xdeadbeef;
        renderer_submit_begin(r);renderer_dispatch(r,g,1,1,1);
        renderer_sync_mem(r,a+0x1000,16);
        float expected=0,actual;memcpy(&actual,output,4);
        for(unsigned k=0;k<4;k++)if(compare(f,(unsigned)(depths[layer][k]*16)))expected+=weights[uv][k];
        if(actual!=expected)fprintf(stderr,"filtered sample lz %u nsa %u layer %u uv %u func %u: %g expected %g\n",lz,nsa,layer,uv,f,actual,expected);
        CHECK(actual==expected);cases++;
      }
    /* Rebind only sampler state at one shader address: the pipeline/translation
     * cache must not reuse the comparison that belongs to the other filter. */
    uint32_t code[32],n=array_sample(code,0,true,false);
    guest_addr pc=a+0x30000;CHECK(gmem_write(m,pc,code,n*4));
    g->regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);
    g->regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);u[8]=1u<<12;
    for(unsigned phase=0;phase<3;phase++) {
        u[10]=phase==1 ? (1u<<20)|(1u<<22) : 0;
        renderer_submit_begin(r);renderer_dispatch(r,g,1,1,1);
        renderer_sync_mem(r,a+0x1000,16);
        float actual,expected=phase==1?.5f:1.f;memcpy(&actual,output,4);
        CHECK(actual==expected);
    }
    printf("filtered comparison GPU: %u cases and 3 sampler rebindings verified\n",cases);return 0;
}
static int gpu_test(void) {
    CHECK(pal_init() == PS5C_OK);
    gmem m;
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 16 * GUEST_PAGE_SIZE, GPROT_RW, 0, "gather compare", &a) == PS5C_OK);
    uint32_t *output = gmem_host(&m, a + 0x1000, 16);
    uint8_t *pixels = gmem_host(&m, a + 0x2000, 4096);
    CHECK(output && pixels);
    memset(pixels, 0, 4096);
    for (unsigned y = 0; y < 4; y++)
        for (unsigned x = 0; x < 4; x++) {
            float f = (float)(y * 4 + x) / 16;
            memcpy(pixels + y * 256 + x * 4, &f, 4);
        }
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 gather compare regression", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m);
    CHECK(r);
    gpu g;
    CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    g.regs[mmCOMPUTE_PGM_RSRC2] = 16u << 1;
    g.regs[mmCOMPUTE_NUM_THREAD_X] = g.regs[mmCOMPUTE_NUM_THREAD_Y] =
        g.regs[mmCOMPUTE_NUM_THREAD_Z] = 1;
    uint32_t *u = g.regs + mmCOMPUTE_USER_DATA_0;
    guest_addr image = a + 0x2000;
    u[0] = (uint32_t)(image >> 8);
    u[1] = (uint32_t)(image >> 40) | (22u << 20) | (3u << 30);
    u[2] = 3u << 14;
    u[3] = (9u << 28) | 0x924; /* 4x4 R32 float, linear, R */
    u[12] = (uint32_t)(a + 0x1000);
    u[13] = (uint32_t)((a + 0x1000) >> 32);
    u[14] = 16;
    u[15] = 0x00016204;
    unsigned cases = 0;
    for (unsigned variant = 0; variant < 2; variant++)
        for (unsigned layout = 0; layout < 5; layout++)
            for (int offset = -1; offset <= 1; offset++)
                for (unsigned f = 0; f < 8; f++) {
                    bool a16 = layout == 1 || layout == 3, d16 = layout == 2 || layout == 3,
                         nsa = layout == 4;
                    if (nsa && offset)
                        continue;
                    uint32_t code[24], n = program(code, offset, a16, d16, nsa, variant != 0);
                    guest_addr pc = a + 0x4000 + cases * 0x100;
                    CHECK(gmem_write(&m, pc, code, n * 4));
                    g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(pc >> 8);
                    g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)(pc >> 40);
                    u[8] = f << 12;
                    for (unsigned i = 0; i < 4; i++)
                        output[i] = 0xdeadbeef;
                    renderer_submit_begin(r);
                    renderer_dispatch(r, &g, 1, 1, 1);
                    renderer_sync_mem(r, a + 0x1000, 16);
                    /* lower-left, lower-right, upper-right, upper-left */
                    int x = 1 + (offset > 0   ? 1
                                 : offset < 0 ? -1
                                              : 0),
                        y = 1 + (offset > 0   ? -1
                                 : offset < 0 ? 1
                                              : 0);
                    unsigned texels[4] = {(y + 1) * 4 + x, (y + 1) * 4 + x + 1, y * 4 + x + 1,
                                          y * 4 + x};
                    uint32_t expected[4];
                    for (unsigned i = 0; i < 4; i++)
                        expected[i] = compare(f, texels[i]) ? (d16 ? 0x3c00 : 0x3f800000) : 0;
                    if (d16) {
                        expected[0] |= expected[1] << 16;
                        expected[1] = expected[2] | (expected[3] << 16);
                        expected[2] = expected[3] = 0xdeadbeef;
                    }
                    for (unsigned i = 0; i < 4; i++) {
                        if (output[i] != expected[i])
                            fprintf(stderr,
                                    "layout %u offset %d func %u out %u: %08x expected %08x\n",
                                    layout, offset, f, i, output[i], expected[i]);
                        CHECK(output[i] == expected[i]);
                    }
                    cases++;
                }
    int array_result=array_gpu_cases(r,&g,&m,a);
    if(!array_result)array_result=filtered_array_cases(r,&g,&m,a);
    renderer_destroy(r);
    free(g.regs);
    pal_mutex_destroy(g.lock);
    pal_window_destroy(window);
    gmem_free(&m);
    pal_shutdown();
    printf("gather compare GPU: %u cases verified\n", cases);
    return array_result;
}
int main(void) {
    sh_program *p = calloc(1, sizeof(*p));
    CHECK(p);
    sh_stage_info info = {.stage = SH_STAGE_CS, .cs_tgid_sgpr = 16, .local_size = {1, 1, 1}};
    sh_site_shape shapes[2] = {{.img_type = 9}};
    uint32_t code[24], n = program(code, 0, false, false, true, true), *spv = NULL;
    size_t words;
    CHECK(sh_analyze(code, n, 0x100000, p));
    CHECK(sh_translate(p, &info, shapes, 0, &spv, &words));
    free(spv);
    for (unsigned f = 0; f < 8; f++) {
        n = program(code, 1, false, false, false, true);
        shapes[0].cmp_func = (uint8_t)f;
        CHECK(sh_analyze(code, n, 0x100000, p));
        CHECK(sh_translate(p, &info, shapes, 0, &spv, &words));
        const char *dir = getenv("PS5C_TEST_SPIRV");
        if (dir) {
            char path[512];
            snprintf(path, sizeof(path), "%s/gather_compare_%u.spv", dir, f);
            FILE *out = fopen(path, "wb");
            CHECK(out && fwrite(spv, 4, words, out) == words && fclose(out) == 0);
        }
        free(spv);
    }
    shapes[0].img_kind = 1;
    spv = NULL;
    CHECK(!sh_translate(p, &info, shapes, 0, &spv, &words));
    shapes[0].img_kind = 2;
    CHECK(!sh_translate(p, &info, shapes, 0, &spv, &words));
    shapes[0].img_kind = 0;
    shapes[0].img_type = 13;
    CHECK(!sh_translate(p, &info, shapes, 0, &spv, &words));
    shapes[0].img_type = 9;
    unsigned image_inst = 0;
    while (image_inst < p->count && p->ops[image_inst] != SH_OP_IMAGE_GATHER4)
        image_inst++;
    CHECK(image_inst < p->count);
    rdna2_inst original = p->insts[image_inst];
    p->insts[image_inst].dmask = 3;
    CHECK(!sh_translate(p, &info, shapes, 0, &spv, &words));
    p->insts[image_inst] = original;
    p->insts[image_inst].tfe = 1;
    CHECK(!sh_translate(p, &info, shapes, 0, &spv, &words));
    p->insts[image_inst] = original;
    p->insts[image_inst].unorm = 1;
    CHECK(!sh_translate(p, &info, shapes, 0, &spv, &words));
    free(p);
    if (getenv("PS5C_TEST_GPU"))
        CHECK(gpu_test() == 0);
    puts("gather compare translation: ok");
    return 0;
}
