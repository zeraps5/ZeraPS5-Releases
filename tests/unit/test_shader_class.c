/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Execute classification on raw payloads: sNaNs must never be quieted first. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/gpu_regs.h"
#include "gpu/renderer.h"
#include "gpu/shader/shader.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)

static const uint32_t opcode[]={0x8f,0x88,0xa8};
/* Independent IEEE encodings, ordered by the ISA's ten class bits. */
static const uint64_t payload[3][10]={
    {0x7c01,0x7e01,0xfc00,0xbc00,0x8001,0x8000,0,1,0x3c00,0x7c00},
    {0x7f800001,0x7fc00001,0xff800000,0xbf800000,0x80000001,0x80000000,0,1,0x3f800000,0x7f800000},
    {0x7ff0000000000001ull,0x7ff8000000000001ull,0xfff0000000000000ull,0xbff0000000000000ull,
     0x8000000000000001ull,0x8000000000000000ull,0,1,0x3ff0000000000000ull,0x7ff0000000000000ull}
};
static void program(uint32_t code[9],unsigned type) {
    const uint32_t base[]={0x7e020204,0x7e040205,0x7e080281,0,
        0x02060880,0xe0702000,0x80030300,0xbf810000,0xbf800000};
    memcpy(code,base,sizeof(base));
    code[3]=0x7c000000u|(opcode[type]<<17)|257u; /* compare raw v1[:2], class mask v0 */
}
static int execute(void) {
    CHECK(pal_init()==PS5C_OK);
    gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,GUEST_PAGE_SIZE,GPROT_RW,0,"float classes",&a)==PS5C_OK);
    uint32_t *out=gmem_host(&m,a+0x1000,64*4);CHECK(out);
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 float class regression",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_PGM_RSRC2]=16u<<1;
    g.regs[mmCOMPUTE_NUM_THREAD_X]=64;g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    u[12]=(uint32_t)(a+0x1000);u[13]=(uint32_t)((a+0x1000)>>32)|(4u<<16);u[14]=64;u[15]=0x16204;
    bool matched=true;
    for(unsigned type=0;type<3;type++) {
        uint32_t code[9];program(code,type);guest_addr pc=a+type*0x100;
        CHECK(gmem_write(&m,pc,code,sizeof(code)));
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
        for(unsigned wave=0;wave<2;wave++)for(unsigned sign=0;sign<2;sign++)for(unsigned cl=0;cl<10;cl++) {
            uint64_t bits=payload[type][cl];
            if(sign && cl<2)bits|=type==0?0x8000ull:type==1?0x80000000ull:0x8000000000000000ull;
            u[4]=(uint32_t)bits;u[5]=(uint32_t)(bits>>32);
            g.regs[mmCOMPUTE_DISPATCH_INITIATOR]=1|(wave<<15);
            for(unsigned lane=0;lane<64;lane++)out[lane]=0xdeadbeef;
            renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,a+0x1000,64*4);
            for(unsigned lane=0;lane<64;lane++) {
                uint32_t want=(lane>>cl)&1u;
                if(out[lane]!=want) {
                    fprintf(stderr,"f%u wave%u class%u sign%u lane%u: %u expected %u\n",type==0?16:type==1?32:64,wave?32:64,cl,sign,lane,out[lane],want);
                    matched=false;break;
                }
            }
        }
    }
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&m);pal_shutdown();
    CHECK(matched);puts("GPU float classes: all ten classes, signed NaN payloads, f16/f32/f64, wave32/64 passed");return 0;
}
int main(void) {
    sh_program *p=calloc(1,sizeof(*p));CHECK(p);
    for(unsigned type=0;type<3;type++) {
        uint32_t code[9];program(code,type);CHECK(sh_analyze(code,9,0x100000,p));
        sh_site_shape shape={0};sh_stage_info info={.stage=SH_STAGE_CS,.cs_tid_components=1,.local_size={64,1,1}};
        uint32_t *spv=NULL;size_t words=0;CHECK(sh_translate(p,&info,&shape,0,&spv,&words));free(spv);
    }
    free(p);if(getenv("PS5C_TEST_GPU"))CHECK(execute()==0);puts("shader class: ok");return 0;
}
