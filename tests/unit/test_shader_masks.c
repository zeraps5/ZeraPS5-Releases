/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Register reuse must not change a comparison/carry's lane mask. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/gpu_regs.h"
#include "gpu/renderer.h"
#include "gpu/shader/shader.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static const uint32_t cases[][16] = {
    { /* Control: 32 < local invocation ID, without an earlier constant. */
        0xbf800000, 0x7d0200a0, 0xbefe046a, 0x7e020281,
        0xe0702000, 0x80030100, 0xbf810000
    },
    { /* VCC first contains a plain wave mask, then a vector comparison. */
        0xbeea0482, 0x7d0200a0, 0xbefe046a, 0x7e020281,
        0xe0702000, 0x80030100, 0xbf810000
    },
    { /* Wave32 comparison writes s4 through SDWA SDST. */
        0xbe840382, 0x7d0200f9, 0x068684a0, 0xbefe0304, 0x7e020281,
        0xe0702000, 0x80030100, 0xbf810000
    },
    { /* An unsigned addition replaces s4 with its carry mask. */
        0xbe840382, 0xd70f0402, 0x00020000, 0xbefe0304, 0x7e020281,
        0xe0702000, 0x80030100, 0xbf810000
    },
    { /* Discarding carry to NULL must leave EXEC unchanged (wave64). */
        0xd70f7d02, 0x00020000, 0x7e020281,
        0xe0702000, 0x80030100, 0xbf810000
    },
    { /* A full scalar mask AND a per-lane comparison, then VCC -> EXEC. */
        0xbe800381, 0xbe810381, 0x7e040280, 0x7d840480,
        0x87ea6a00, 0xbefe046a, 0x7e020281,
        0xe0702000, 0x80030100, 0xbf810000
    },
    { /* Keep the lane representation through AND to s4 and MOV to s6. */
        0xbe800381, 0xbe810381, 0x7e040280, 0x7d840480,
        0x87846a00, 0xbe860404, 0xbefe0406, 0x7e020281,
        0xe0702000, 0x80030100, 0xbf810000
    },
    { /* The plain mask can be either operand of the AND. */
        0xbe800381, 0xbe810381, 0x7e040280, 0x7d840480,
        0x8784006a, 0xbefe0404, 0x7e020281,
        0xe0702000, 0x80030100, 0xbf810000
    },
    { /* OR adds the constant's own lanes to the comparison's lanes. */
        0xbe800381, 0xbe810381, 0x7d0200a0,
        0x88ea6a00, 0xbefe046a, 0x7e020281,
        0xe0702000, 0x80030100, 0xbf810000
    },
    { /* Ordinary scalar bitwise arithmetic still produces the full value. */
        0xbe800381, 0xbe810381, 0xbe820381, 0xbe830381,
        0x87840200, 0x7e020204,
        0xe0702000, 0x80030100, 0xbf810000
    },
    { /* The Sonic reduction layout: 32x1x6, select x=0 in z=0 only. */
        0xbe800381, 0xbe810381, 0x7d840480,
        0x87ea6a00, 0xbefe046a, 0x7e020281,
        0xd7460003, 0x04010b02,
        0xe0702000, 0x80030103, 0xbf810000
    }
};
static const uint32_t lengths[] = {7, 7, 8, 8, 6, 10, 11, 10, 9, 9, 11};

static int pseudoflag_gpu_test(void) {
    CHECK(getenv("PS5C_CS_WAVE_FROM_PACKET"));
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,GUEST_PAGE_SIZE,GPROT_RW,0,"pseudoflags",&a)==PS5C_OK);
    uint32_t *output=gmem_host(&m,a+0x1000,64*4);CHECK(output);
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 pseudoflag regression",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_NUM_THREAD_X]=64;g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    g.regs[mmCOMPUTE_PGM_RSRC2]=24u<<1;
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    u[12]=(uint32_t)(a+0x1000);u[13]=(uint32_t)((a+0x1000)>>32)|(4u<<16);u[14]=64;u[15]=0x16204;
    const uint32_t highs[]={0,1,0x80000000};bool matched=true;
    for(unsigned wave=0;wave<2;wave++)for(unsigned mask=0;mask<2;mask++) {
        unsigned lo=mask?RDNA2_OP_EXEC_LO:RDNA2_OP_VCC_LO;
        unsigned pseudo=mask?RDNA2_OP_EXECZ:RDNA2_OP_VCCZ;
        uint32_t code[]={0xbe800300u|(lo<<16)|20,0xbe800300u|((lo+1)<<16)|21,
            0xbe960300u|pseudo,0xbefe04c1, /* capture, then restore full EXEC */
            0x7e020216,0xe0702000,0x80030100,0xbf810000};
        guest_addr pc=a+(wave*2+mask)*0x100;
        CHECK(gmem_write(&m,pc,code,sizeof(code)));
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
        g.regs[mmCOMPUTE_DISPATCH_INITIATOR]=1|(wave<<15);
        for(unsigned low=0;low<2;low++)for(unsigned h=0;h<PS5C_ARRAY_COUNT(highs);h++) {
            u[20]=low;u[21]=highs[h];for(unsigned lane=0;lane<64;lane++)output[lane]=0xdeadbeef;
            renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,a+0x1000,64*4);
            uint32_t expected=(low|(wave?0:highs[h]))==0;
            for(unsigned lane=0;lane<64;lane++) {
                if(output[lane]!=expected)fprintf(stderr,"pseudo %u wave%u low%u high%08x lane%u: %u expected%u\n",pseudo,wave?32:64,low,highs[h],lane,output[lane],expected);
                if(output[lane]!=expected)matched=false;
            }
        }
    }
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&m);pal_shutdown();
    CHECK(matched);puts("GPU pseudoflags: wave32/64, LOW/HIGH variations and restored EXEC passed");return 0;
}

static int gpu_test(void) {
    CHECK(pal_init() == PS5C_OK);
    gmem m;
    gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "lane masks", &a) == PS5C_OK);
    uint32_t *output = gmem_host(&m, a + 0x1000, 192 * 4); CHECK(output);
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 mask regression", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m); CHECK(r);
    gpu g; CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    g.regs[mmCOMPUTE_PGM_RSRC2] = 16u << 1;
    g.regs[mmCOMPUTE_NUM_THREAD_X] = 64;
    g.regs[mmCOMPUTE_NUM_THREAD_Y] = g.regs[mmCOMPUTE_NUM_THREAD_Z] = 1;
    uint32_t *u = g.regs + mmCOMPUTE_USER_DATA_0;
    u[0] = 0xffffffff;
    u[12] = (uint32_t)(a + 0x1000);
    u[13] = (uint32_t)((a + 0x1000) >> 32) | (4u << 16);
    u[14] = 192; u[15] = 0x00016204;
    for (unsigned k = 0; k < PS5C_ARRAY_COUNT(cases); k++) {
        CHECK(gmem_write(&m, a + k * 0x100, cases[k], lengths[k] * 4));
        guest_addr pc = a + k * 0x100;
        g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(pc >> 8);
        g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)(pc >> 40);
        uint32_t lanes = k == 10 ? 192 : 64;
        g.regs[mmCOMPUTE_NUM_THREAD_X] = k == 10 ? 32 : 64;
        g.regs[mmCOMPUTE_NUM_THREAD_Z] = k == 10 ? 6 : 1;
        g.regs[mmCOMPUTE_PGM_RSRC2] = (16u << 1) | (k == 10 ? 2u << 11 : 0);
        memset(output, 0, 192 * 4);
        renderer_submit_begin(r); renderer_dispatch(r, &g, 1, 1, 1);
        renderer_sync_mem(r, a + 0x1000, 192 * 4);
        for (unsigned lane = 0; lane < lanes; lane++) {
            uint32_t expected = k == 10 ? lane == 0 : k == 9 ? 1 : k == 8 ? (lane == 0 || lane >= 32) : k >= 5 ? (lane == 0 || lane == 32) : k == 4 ? 1 : k == 3 ? lane != 0 : lane > 32;
            if (output[lane] != expected)
                fprintf(stderr, "case %u lane %u: %u expected %u\n", k, lane, output[lane], expected);
            CHECK(output[lane] == expected);
        }
    }
    renderer_destroy(r); free(g.regs); pal_mutex_destroy(g.lock);
    pal_window_destroy(window); gmem_free(&m); pal_shutdown();
    puts("GPU masks: comparison/carry reuse and scalar masks, including 32x1x6");
    return 0;
}

int main(void) {
    if(getenv("PS5C_TEST_PSEUDOFLAGS"))return pseudoflag_gpu_test();
    sh_program *p = calloc(1, sizeof(*p)); CHECK(p);
    for (unsigned k = 0; k < PS5C_ARRAY_COUNT(cases); k++) {
        CHECK(sh_analyze(cases[k], lengths[k], 0x100000, p));
        sh_site_shape shape = {0};
        sh_stage_info info = {.stage = SH_STAGE_CS, .cs_tid_components = 1, .local_size = {64, 1, 1}};
        if (k == 10) { info.cs_tid_components = 3; info.local_size[0] = 32; info.local_size[2] = 6; }
        uint32_t *spv = NULL; size_t words = 0;
        CHECK(sh_translate(p, &info, &shape, 0, &spv, &words)); free(spv);
    }
    free(p);
    if (getenv("PS5C_TEST_GPU")) CHECK(gpu_test() == 0);
    puts("shader masks: ok"); return 0;
}
