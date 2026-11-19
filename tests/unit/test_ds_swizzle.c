/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real lane values, distinct ADDR/DATA0, all four modes and inactive sources. */
#include "gpu/gpu_regs.h"
#include "gpu/renderer.h"
#include "gpu/shader/shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 2;}}while(0)
static const uint16_t offsets[]={0x001f,0x041f,0x801b,0xc038,0xc438,0xc020,0xe000,0xe010,0xe01f};
static const unsigned reverse5[]={0,16,8,24,4,20,12,28,2,18,10,26,6,22,14,30,1,17,9,25,5,21,13,29,3,19,11,27,7,23,15,31};
static const unsigned reverse4[]={0,8,4,12,2,10,6,14,1,9,5,13,3,11,7,15};
static unsigned source_lane(unsigned mode,unsigned lane) {
    switch(mode) {
    case 0:return lane;
    case 1:return lane^1;
    case 2:return lane^3;
    case 3:return (lane&~7u)|((lane+1)&7);
    case 4:return (lane&~7u)|((lane+7)&7);
    case 5:return (lane&~31u)|((lane+1)&31);
    case 6:return (lane&~31u)|reverse5[lane&31];
    case 7:return (lane&~15u)|reverse4[lane&15];
    default:return lane;
    }
}
static bool enabled(unsigned mask,unsigned lane) {return mask==0 || (mask==1?!(lane&1):lane<48);}
static unsigned program(uint32_t code[32],unsigned mode,unsigned source,unsigned mask) {
    unsigned n=0;
    code[n++]=0x7e140300; /* v10 = original local thread ID (store index) */
    code[n++]=0x7e100d00; /* v8 = float(v0) */
    code[n++]=0x061010f2; /* v8 = 1.0 + v8 */
    code[n++]=source?0x7e000200:0x7e000308; /* v0 = sentinel or v8 */
    code[n++]=0x7e040200; /* v2 = sentinel: inactive destinations keep it */
    if(mask==1) {
        code[n++]=0x36081481; /* v4 = v10 & 1 */
        code[n++]=0x7d840880; /* VCC = v4 == 0 */
        code[n++]=0xbefe046a;
    } else if(mask==2) {
        code[n++]=0x7e0802b0; /* v4 = 48 */
        code[n++]=0x7d82090a; /* VCC = v10 < v4 */
        code[n++]=0xbefe046a;
    }
    code[n++]=0xd8d40000u|offsets[mode];
    code[n++]=0x02000000u|source; /* VDST2, ADDR source, reserved DATA0=0 */
    code[n++]=0xbefe04c1; /* all lanes store their result */
    code[n++]=0xe0702000;code[n++]=0x8003020a; /* store v2 indexed by v10 */
    code[n++]=0xbf810000;return n;
}
static int cpu_test(void) {
    uint32_t code[]={0xd8d4c038,0x02000008};rdna2_inst in;char text[256];
    CHECK(rdna2_decode(code,2,&in));
    CHECK(in.vaddr==8 && in.vdata==0 && in.dst==RDNA2_OP_VGPR+2);
    CHECK(rdna2_disasm(&in,text,sizeof(text)) && strstr(text,"v2, v8"));
    puts("DS_SWIZZLE decoder: destination and ADDR source verified");return 0;
}
static int gpu_test(void) {
    CHECK(pal_init()==PS5C_OK);
    gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr a;CHECK(gmem_map(&m,0,4*GUEST_PAGE_SIZE,GPROT_RW,0,"DS swizzle fixture",&a)==PS5C_OK);
    uint32_t *output=gmem_host(&m,a+0x4000,64*4);CHECK(output);
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 DS swizzle regression",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);
    gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_PGM_RSRC2]=(16u<<1)|(1u<<11);
    g.regs[mmCOMPUTE_NUM_THREAD_X]=64;g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    bool native32=getenv("PS5C_TEST_SWIZZLE_NATIVE32")!=NULL;
    g.regs[mmCOMPUTE_DISPATCH_INITIATOR]=native32?0x8000u:0;
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;u[0]=0xdeadbeef;
    u[12]=(uint32_t)(a+0x4000);u[13]=(uint32_t)((a+0x4000)>>32)|(4u<<16);u[14]=64;u[15]=0x16204;
    unsigned cases=0,failures=0,failed_cases=0;
    for(unsigned mode=0;mode<PS5C_ARRAY_COUNT(offsets);mode++)for(unsigned source=0;source<=8;source+=8)for(unsigned mask=0;mask<3;mask++) {
        uint32_t code[32];unsigned count=program(code,mode,source,mask);guest_addr pc=a+cases*0x100;
        CHECK(gmem_write(&m,pc,code,count*4));
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
        for(unsigned lane=0;lane<64;lane++)output[lane]=0xabababab;
        renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,a+0x4000,64*4);
        unsigned bad=0;
        for(unsigned lane=0;lane<64;lane++) {
            unsigned from=source_lane(mode,lane);float value=(float)from+1;uint32_t expected;
            memcpy(&expected,&value,4);
            if(!enabled(mask,from))expected=0;
            if(!enabled(mask,lane))expected=0xdeadbeef;
            if(output[lane]!=expected) {
                if(!bad)fprintf(stderr,"DS offset %04x source%u mask%u lane%u: %08x expected %08x\n",offsets[mode],source,mask,lane,output[lane],expected);
                bad++;
            }
        }
        failures+=bad;failed_cases+=bad!=0;cases++;
    }
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&m);pal_shutdown();
    printf("DS_SWIZZLE GPU native32=%u: %u cases x64 lanes, %u mismatches in %u cases\n",native32,cases,failures,failed_cases);
    return failures?1:0;
}
int main(void) {return getenv("PS5C_TEST_GPU")?gpu_test():cpu_test();}
