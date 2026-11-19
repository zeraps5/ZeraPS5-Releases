/* SPDX-License-Identifier: GPL-2.0-or-later */
/* EXECZ escaping an arm must preserve scalar effects; terminal masked stores
 * still use the fast structured path. Synthetic programs, no game assets. */
#include <stdio.h>
#include <stdlib.h>
#include "gpu/gpu_regs.h"
#include "gpu/shader/shader.h"
#include "gpu/shader/spv.h"
#include "gpu/vk/vk_internal.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);fflush(stderr);_Exit(1);}}while(0)
static const uint32_t restore[]={0xbf068008,0xbf840006,0x7daa0100,0xbf880005,
    0xbefe04c1,0x7e020209,0xe0700000,0x80000100,0xbe8a0380,0xbefe04c1,0xbf810000};
static const uint32_t scalar[]={0xbf068008,0xbf840004,0x7daa0100,0xbf880003,
    0xbe8a0381,0xbf800000,0xbf800000,0xbefe04c1,0x7e02020a,0xe0700000,0x80000100,0xbf810000};
static const uint32_t safe[]={0xbf068008,0xbf840006,0x7daa0100,0xbf880005,
    0x7e020209,0xbf8cc07f,0xe0700000,0x80000100,0xbf800000,0xbefe04c1,0xbf810000};
static int lowering_test(const uint32_t *code,uint32_t count,bool dispatcher) {
    sh_program *p=calloc(1,sizeof(*p));CHECK(p && sh_analyze(code,count,0,p));
    sh_stage_info info={.stage=SH_STAGE_CS,.local_size={1,1,1},.cs_tid_components=1};
    sh_site_shape *shapes=calloc(p->site_count,sizeof(*shapes));uint32_t *spv=NULL;size_t words=0;
    CHECK(shapes && sh_translate(p,&info,shapes,0,&spv,&words));unsigned switches=0;
    for(size_t i=5;i<words;i+=spv[i]>>16){CHECK(spv[i]>>16);switches+=(spv[i]&65535)==SpvOpSwitch;}
    if(!getenv("PS5C_NO_STRUCTURED"))CHECK((switches!=0)==dispatcher);
    free(spv);free(shapes);free(p);return 0;
}
static int gpu_test(void) {
    gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr a;CHECK(gmem_map(&m,0,4*GUEST_PAGE_SIZE,GPROT_RW,0,"EXECZ fixture",&a)==PS5C_OK);
    const uint32_t *programs[]={restore,scalar,safe};uint32_t counts[]={PS5C_ARRAY_COUNT(restore),PS5C_ARRAY_COUNT(scalar),PS5C_ARRAY_COUNT(safe)};
    for(unsigned i=0;i<3;i++)CHECK(gmem_write(&m,a+i*0x100,programs[i],counts[i]*4));
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 EXECZ regression",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_PGM_RSRC2]=16u<<1;g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(a>>40);
    g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;guest_addr out=a+GUEST_PAGE_SIZE;
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;u[0]=(uint32_t)out;u[1]=(uint32_t)(out>>32);u[2]=16;u[3]=0x16204;u[9]=0x12345678;u[10]=7;
    for(unsigned threads=1;threads<=64;threads*=64)for(unsigned outer=0;outer<2;outer++)for(unsigned p=0;p<3;p++) {
        g.regs[mmCOMPUTE_NUM_THREAD_X]=threads;u[8]=outer;g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+p*0x100)>>8);
        uint32_t old=0x5a5a5a5a,value=0;CHECK(gmem_write(&m,out,&old,4));
        renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,4);CHECK(gmem_read(&m,out,&value,4));
        uint32_t expected=p==1?7:old;
        if(value!=expected)fprintf(stderr,"program %u threads %u outer %u: %08x expected %08x\n",p,threads,outer,value,expected);
        CHECK(value==expected);
    }
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&m);
    puts("EXECZ GPU: EXEC restore, scalar effects and safe masked stores preserved in 1/64-thread groups");return 0;
}
int main(void) {
    CHECK(pal_init()==PS5C_OK);CHECK(lowering_test(restore,PS5C_ARRAY_COUNT(restore),true)==0);
    CHECK(lowering_test(scalar,PS5C_ARRAY_COUNT(scalar),true)==0);CHECK(lowering_test(safe,PS5C_ARRAY_COUNT(safe),false)==0);
    if(getenv("PS5C_TEST_GPU"))CHECK(gpu_test()==0);
    pal_shutdown();return 0;
}
