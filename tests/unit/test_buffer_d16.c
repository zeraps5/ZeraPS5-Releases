/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Formatted D16 loads pack components; stores unpack them before conversion. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/gpu_regs.h"
#include "gpu/renderer.h"
#include "gpu/shader/shader.h"
#define CHECK(x) do { if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;} } while(0)
static unsigned program(uint32_t *code,unsigned n,bool high) {
    unsigned at=0;
    for(unsigned k=0;k<4;k++){code[at++]=0x7e0002ffu|((4+k)<<17);code[at++]=0x11223344u+k*0x11111111u;}
    code[at++]=(high?0xe0982000u:0xe2002000u+((n-1)<<18));code[at++]=0x80000400u;
    code[at++]=0xbf8c3f70u;
    code[at++]=0xe0782000u;code[at++]=0x80030400u; /* raw v4:v7 -> s12 descriptor */
    code[at++]=(high?0xe09c2000u:0xe2102000u+((n-1)<<18));code[at++]=0x80020400u;
    code[at++]=0xbf810000u;return at;
}
static int execute(void) {
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,GUEST_PAGE_SIZE,GPROT_RW,0,"D16 buffers",&a)==PS5C_OK);
    uint32_t *out=gmem_host(&m,a+0x1000,64*16),*src=gmem_host(&m,a+0x2000,64*4),*dst=gmem_host(&m,a+0x3000,64*4);CHECK(out&&src&&dst);
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 D16 buffer regression",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_PGM_RSRC2]=16u<<1;g.regs[mmCOMPUTE_NUM_THREAD_X]=64;
    g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    u[0]=(uint32_t)(a+0x2000);u[1]=(uint32_t)((a+0x2000)>>32)|(4u<<16);u[2]=64;
    u[8]=(uint32_t)(a+0x3000);u[9]=(uint32_t)((a+0x3000)>>32)|(4u<<16);u[10]=64;
    u[12]=(uint32_t)(a+0x1000);u[13]=(uint32_t)((a+0x1000)>>32)|(16u<<16);u[14]=64;u[15]=0x16204;
    bool matched=true;
    for(unsigned kind=0;kind<3;kind++)for(unsigned form=0;form<5;form++) {
        unsigned n=form==4?1:form+1;bool high=form==4;
        uint32_t code[20];unsigned words=program(code,n,high);guest_addr pc=a+(kind*5+form)*0x100;
        CHECK(gmem_write(&m,pc,code,words*4));
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
        u[3]=u[11]=0xfacu|((kind==0?60u:kind==1?56u:61u)<<12);
        uint32_t pixel=kind==0?0x78563412u:kind==1?0xff00ff00u:0x007fff80u;
        const uint32_t half[3][4]={{0x12,0x34,0x56,0x78},{0,0x3c00,0,0x3c00},{0xff80,0xffff,0x7f,0}};
        for(unsigned lane=0;lane<64;lane++){src[lane]=pixel;dst[lane]=0xaabbccddu;}
        memset(out,0xee,64*16);
        renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);
        renderer_sync_mem(r,a+0x1000,64*16);renderer_sync_mem(r,a+0x3000,64*4);
        for(unsigned lane=0;lane<64;lane++) {
            uint32_t want[4]={0x11223344,0x22334455,0x33445566,0x44556677};
            for(unsigned k=0;k<n;k++){unsigned reg=k/2,shift=high?16:(k&1u)*16;want[reg]=(want[reg]&~(0xffffu<<shift))|(half[kind][k]<<shift);}
            uint32_t mask=n==4?UINT32_MAX:(1u<<(8*n))-1;
            uint32_t stored=(0xaabbccddu&~mask)|(pixel&mask);
            if(memcmp(out+lane*4,want,sizeof(want))||dst[lane]!=stored) {
                fprintf(stderr,"kind%u form%u lane%u: %08x %08x %08x %08x, stored %08x expected %08x\n",kind,form,lane,out[lane*4],out[lane*4+1],out[lane*4+2],out[lane*4+3],dst[lane],stored);
                matched=false;break;
            }
        }
    }
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&m);pal_shutdown();
    CHECK(matched);puts("GPU D16: UINT/SINT/UNORM, 1-4 channels and high-half preservation passed");return 0;
}
int main(void) {
    sh_program *p=calloc(1,sizeof(*p));CHECK(p);
    for(unsigned form=0;form<5;form++) {
        uint32_t code[20];unsigned words=program(code,form==4?1:form+1,form==4);CHECK(sh_analyze(code,words,0x100000,p));
        sh_site_shape shapes[3]={{.buf_format=60,.dst_sel={4,5,6,7}},{0},{.buf_format=60,.dst_sel={4,5,6,7}}};
        sh_stage_info info={.stage=SH_STAGE_CS,.cs_tid_components=1,.local_size={64,1,1}};
        uint32_t *spv=NULL;size_t count=0;CHECK(sh_translate(p,&info,shapes,0,&spv,&count));free(spv);
    }
    free(p);if(getenv("PS5C_TEST_GPU"))CHECK(execute()==0);puts("buffer D16: ok");return 0;
}
