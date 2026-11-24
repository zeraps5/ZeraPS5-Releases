/* SPDX-License-Identifier: GPL-2.0-or-later */
/* v_movrels/v_movreld: VGPR indexing by M0, in range, out of range and under a partial EXEC. */
#include "gpu/shader/shader.h"
#include "gpu/shader/spv.h"
#include "gpu/vk/vk_internal.h"
#include "gpu/gpu_regs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static uint32_t mov(unsigned dst,unsigned src){return 0x7e000200|(dst<<17)|src;}
static uint32_t relative(unsigned op,unsigned dst,unsigned src){return 0x7e000000|(dst<<17)|(op<<9)|(256+src);}
static unsigned program(uint32_t *c, bool indexed){
    unsigned n=0;
    for(unsigned k=0;k<64;k++) {
        c[n++]=mov(8+k,255);c[n++]=100+k;
        c[n++]=mov(128+k,255);c[n++]=1000+k;
        c[n++]=mov(192+k,255);c[n++]=2000+k;
    }
    for(unsigned reg=72;reg<128;reg++){c[n++]=mov(reg,255);c[n++]=0x40000000u+reg;}
    c[n++]=mov(5,128);c[n++]=mov(6,128);c[n++]=mov(7,128);
    c[n++]=0xbefc0300; /* m0 = s0 */
    for (unsigned i=0;i<(indexed?20u:1u);i++)
        c[n++]=0x7e088708; /* v4 = v[8 + m0] (v_movrels_b32) */
    c[n++]=0x36060006; /* v3 = v0 & s6 */
    c[n++]=0x7da40680; /* exec = v3 == 0: the even lanes when s6 is 1 */
    c[n++]=0x7e108500; /* v[8 + m0] = v0 (v_movreld_b32) */
    c[n++]=relative(0x44,192,128); /* v[192+m0] = v[128+m0], under the same EXEC */
    c[n++]=relative(0x43,5,255); /* one-register source bank */
    c[n++]=relative(0x43,6,253); /* three-register source bank */
    c[n++]=relative(0x42,255,0); /* one-register destination bank */
    c[n++]=0xbefe04c1; /* exec = -1 */
    const uint32_t regs[]={8,68,4,192,252};
    for(unsigned k=0;k<5;k++) {
        c[n++]=0xe0782000|(k*16);c[n++]=0x80040000|(regs[k]<<8); /* 80-byte record per lane */
    }
    c[n++]=0xbf810000;return n;
}
static int cpu_test(bool indexed){
    uint32_t code[1024],n=program(code,indexed),*spv;size_t words;sh_program *p=calloc(1,sizeof(*p));CHECK(p);
    CHECK(sh_analyze(code,n,0,p));
    sh_stage_info info={.stage=SH_STAGE_CS,.local_size={64,1,1},.cs_tid_components=1};
    sh_site_shape shapes[SH_MAX_SITES]={0};CHECK(sh_translate(p,&info,shapes,0,&spv,&words));
    unsigned branches=0;
    for(size_t i=5;i<words;){size_t count=spv[i]>>16;CHECK(count && count<=words-i);branches+=(spv[i]&65535)==SpvOpBranchConditional;i+=count;}
    if (indexed) CHECK(branches < 64); /* large shaders use a bounded indexed bank */
    else CHECK(branches>=2*125); /* smaller shaders retain independent VGPRs */
    free(spv);free(p);return 0;
}
static int gpu_test(bool indexed){
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr a;CHECK(gmem_map(&m,0,4*GUEST_PAGE_SIZE,GPROT_RW,0,"movrel",&a)==PS5C_OK);
    pal_window *w=NULL;CHECK(pal_window_open("ZeraPS5 movrel regression",320,180,&w)==PS5C_OK);renderer *r=renderer_create(w,&m);CHECK(r);
    gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    uint32_t code[1024],n=program(code,indexed);CHECK(gmem_write(&m,a,code,n*4));
    g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(a>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(a>>40);
    g.regs[mmCOMPUTE_NUM_THREAD_X]=64;g.regs[mmCOMPUTE_NUM_THREAD_Y]=1;g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    g.regs[mmCOMPUTE_PGM_RSRC2]=24u<<1;
    guest_addr regs=a+0x4000;uint32_t *q=gmem_host(&m,regs,64*80);CHECK(q);
    uint32_t *d=u+16;d[0]=(uint32_t)regs;d[1]=(uint32_t)(regs>>32)|(80u<<16);d[2]=64;d[3]=0x16204;
    static const uint32_t index[]={0,1,3,63,64,70,120,184,245,247,248,255,UINT32_MAX,2};
    for(unsigned k=0;k<PS5C_ARRAY_COUNT(index);k++){
        bool partial=k+1==PS5C_ARRAY_COUNT(index);
        uint64_t mask=partial?0x5555555555555555ull:UINT64_MAX;
        u[0]=index[k];u[6]=partial;
        memset(q,0xcd,64*80);
        renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,regs,64*80);
        for(unsigned lane=0;lane<64;lane++){
            bool active=((mask>>lane)&1)!=0;uint32_t *row=q+lane*20;
            uint32_t state[256]={0};
            for(unsigned reg=0;reg<64;reg++) {state[8+reg]=100+reg;state[128+reg]=1000+reg;state[192+reg]=2000+reg;}
            for(unsigned reg=72;reg<128;reg++)state[reg]=0x40000000u+reg;
            uint32_t idx=index[k],want=state[idx<248?8+idx:8];
            if(active && idx<248)state[8+idx]=lane;
            if(active && idx<64)state[192+idx]=state[128+idx];
            uint32_t read255=active?state[255]:0,read253=active?state[253+(idx<3?idx:0)]:0;
            if(active && !idx)state[255]=lane;
            if(row[8]!=want)fprintf(stderr,"case %u lane %u read %u expected %u\n",k,lane,row[8],want);
            CHECK(row[8]==want);
            for(unsigned j=0;j<4;j++){
                CHECK(row[j]==state[8+j]);
                CHECK(row[4+j]==state[68+j]);
                CHECK(row[12+j]==state[192+j]);
                CHECK(row[16+j]==state[252+j]);
            }
            CHECK(row[9]==read255);
            CHECK(row[10]==read253);
            CHECK(row[11]==0);
        }
    }
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(w);gmem_free(&m);pal_shutdown();
    puts("movrel: full 256-register file and short banks, combined read/write, register255, invalid indices and partial EXEC verified");return 0;
}
int main(void){
    CHECK(cpu_test(false)==0);
    CHECK(cpu_test(true)==0);
    if (getenv("PS5C_TEST_GPU")) {
        CHECK(gpu_test(false)==0);
        CHECK(gpu_test(true)==0);
    }
    return 0;
}
