/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Independent synthetic regression for AnyPS5 0bea402a's RDNA2 semantics.
 * Expectations are fixed bit patterns, not computed by the candidate helper. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/gpu_regs.h"
#include "gpu/renderer.h"
#include "gpu/shader/shader.h"
#include "gpu/shader/shader_corpus.h"
#include "gpu/vk/vk_shader_cache.h"
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1; } } while(0)
enum { ADD,MUL,FMAC,FMA,PKADD,PKMUL,PKFMA,PKFMAC,CONVERT,U16,RCP,EXP,LOG,MIXLO,MIXHI };
typedef struct sample { unsigned kind; uint32_t a,b,c,off,on; bool nan; } sample;
static const sample samples[]={
    {ADD,0x7bff,0x7bff,0x35550000,0x35557c00,0x35557bff,false},
    {ADD,0xfbff,0xfbff,0x35550000,0x3555fc00,0x3555fbff,false},
    {MUL,0x7bff,0x4000,0x35550000,0x35557c00,0x35557bff,false},
    {MUL,0xfbff,0x4000,0x35550000,0x3555fc00,0x3555fbff,false},
    {MUL,0x7c00,0x4000,0x35550000,0x35557c00,0x35557c00,false},
    {MUL,0xfc00,0x4000,0x35550000,0x3555fc00,0x3555fc00,false},
    {MUL,0x7e11,0x4000,0x35550000,0,0,true},
    {MUL,0,0x7c00,0x35550000,0,0,true},
    {FMAC,0x7bff,0x4000,0x35550000,0x35557c00,0x35557bff,false},
    {FMA,0x7bff,0x4000,0x35550000,0x35557c00,0x35557bff,false},
    {FMA,0x3c00,0x4000,0x35557c00,0x35557c00,0x35557c00,false},
    {PKADD,0x7bff,0x7bff,0,0x7c007c00,0x7bff7bff,false},
    {PKMUL,0xfbff,0x4000,0,0xfc00fc00,0xfbfffbff,false},
    {PKFMA,0x7bff,0x4000,0,0x7c007c00,0x7bff7bff,false},
    {PKFMAC,0x7bff7bff,0x40004000,0,0x7c007c00,0x7bff7bff,false},
    {PKMUL,0x7c00,0x4000,0,0x7c007c00,0x7c007c00,false},
    {CONVERT,0x477ff000,0,0,0x7c00,0x7bff,false}, /* 65520: RNE boundary */
    {CONVERT,0xc77ff000,0,0,0xfc00,0xfbff,false},
    {CONVERT,0x477fefff,0,0,0x7bff,0x7bff,false},
    {CONVERT,0x7f800000,0,0,0x7c00,0x7c00,false},
    {CONVERT,0xff800000,0,0,0xfc00,0xfc00,false},
    {CONVERT,0x7fc01234,0,0,0,0,true},
    {U16,65535,0,0x35550000,0x35557c00,0x35557bff,false},
    {RCP,0,0,0x35550000,0x35557c00,0x35557bff,false},
    {RCP,0x8000,0,0x35550000,0x3555fc00,0x3555fbff,false},
    {EXP,0x4c00,0,0x35550000,0x35557c00,0x35557bff,false}, /* exp2(16) */
    {EXP,0x7c00,0,0x35550000,0x35557c00,0x35557c00,false},
    {LOG,0,0,0x35550000,0x3555fc00,0x3555fbff,false},
    {MIXLO,0x47800000,0x3f800000,0,0x7c00,0x7bff,false},
    {MIXHI,0xc7800000,0x3f800000,0,0xfc000000,0xfbff0000,false},
};
static size_t program(unsigned kind,uint32_t code[12]) {
    uint32_t prefix[]={0x7e000200,0x7e020201,0x7e040202};
    memcpy(code,prefix,sizeof(prefix));size_t n=3;
    if(kind==FMA) {
        code[n++]=0xd74b0002;code[n++]=256u|(257u<<9)|(258u<<18);
    } else if(kind==PKADD||kind==PKMUL||kind==PKFMA||kind==MIXLO||kind==MIXHI) {
        unsigned op=kind==PKADD?0xf:kind==PKMUL?0x10:kind==PKFMA?0xe:kind==MIXLO?0x21:0x22;
        code[n++]=0xcc000002u|(op<<16);code[n++]=256u|(257u<<9)|(258u<<18);
    } else if(kind==CONVERT||kind==U16||kind==RCP||kind==EXP||kind==LOG) {
        unsigned op=kind==CONVERT?0xa:kind==U16?0x50:kind==RCP?0x54:kind==EXP?0x58:0x57;
        code[n++]=0x7e040100u|(op<<9);
    } else {
        unsigned op=kind==ADD?0x32:kind==MUL?0x35:kind==PKFMAC?0x3c:0x36;
        code[n++]=(op<<25)|(2u<<17)|(1u<<9)|256u;
    }
    code[n++]=0xe0700000;code[n++]=0x80030200;code[n++]=0xbf810000;
    return n;
}
static int translation_tests(void) {
    sh_program *p=calloc(1,sizeof(*p));CHECK(p);
    sh_stage_info info={.stage=SH_STAGE_CS,.local_size={1,1,1}};
    sh_site_shape shape={0};
    CHECK(sh_fp16_overflow_bit(SH_STAGE_CS,1u<<26)==1);
    CHECK(sh_fp16_overflow_bit(SH_STAGE_PS,1u<<29)==1);
    CHECK(sh_fp16_overflow_bit(SH_STAGE_VS,1u<<31)==1);
    CHECK(sh_fp16_overflow_bit(SH_STAGE_PS,1u<<26)==0);
    for(unsigned kind=0;kind<=MIXHI;kind++) {
        uint32_t code[12];size_t n=program(kind,code);
        CHECK(sh_analyze(code,(uint32_t)n,0x100000,p));
        uint64_t keys[2];
        for(unsigned mode=0;mode<2;mode++) {
            info.fp16_overflow=mode;uint32_t *spv=NULL;size_t words=0;
            keys[mode]=vk_shader_cache_key(p,&info,&shape,0);
            CHECK(sh_translate(p,&info,&shape,0,&spv,&words));
            const char *dir=getenv("PS5C_TEST_SPIRV");
            if(dir) { char path[512];snprintf(path,sizeof(path),"%s/fp16_%u_%u.spv",dir,kind,mode);
                FILE *f=fopen(path,"wb");CHECK(f);CHECK(fwrite(spv,4,words,f)==words);CHECK(fclose(f)==0); }
            free(spv);
        }
        CHECK(keys[0]!=keys[1]); /* mode is part of the persistent cache identity */
    }
    free(p);return 0;
}
static int gpu_test(void) {
    CHECK(pal_init()==PS5C_OK);
    gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr page;
    CHECK(gmem_map(&m,0,4*GUEST_PAGE_SIZE,GPROT_RW,0,"fp16 overflow",&page)==PS5C_OK);
    uint32_t *out=gmem_host(&m,page+0x8000,4);CHECK(out);
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 FP16 overflow regression",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_PGM_RSRC2]=16u<<1;
    g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    u[12]=(uint32_t)(page+0x8000);u[13]=(uint32_t)((page+0x8000)>>32);u[14]=4;u[15]=0x16204;
    unsigned failures=0;
    for(unsigned mode=0;mode<2;mode++)for(unsigned k=0;k<PS5C_ARRAY_COUNT(samples);k++) {
        const sample *s=&samples[k];uint32_t code[12];size_t n=program(s->kind,code);
        guest_addr pc=page+k*0x100;CHECK(gmem_write(&m,pc,code,n*4));
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
        g.regs[mmCOMPUTE_PGM_RSRC1]=mode<<26;u[0]=s->a;u[1]=s->b;u[2]=s->c;
        *out=0xdeadbeef;renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,page+0x8000,4);
        uint32_t wanted=mode?s->on:s->off;
        bool good=s->nan?((*out&0x7fffu)>0x7c00u && (*out>>16)==(s->c>>16)):(*out==wanted);
        if(!good) { failures++;fprintf(stderr,"FP16 sample %u kind %u mode %u: %08x expected %08x%s\n",k,s->kind,mode,*out,wanted,s->nan?" (NaN)":""); }
    }
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&m);pal_shutdown();
    CHECK(failures==0);printf("FP16 GPU: %zu results, mode alternation and cache isolation verified\n",2*PS5C_ARRAY_COUNT(samples));return 0;
}
int main(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "--gpu-only")) CHECK(translation_tests()==0);
    if(getenv("PS5C_TEST_GPU"))CHECK(gpu_test()==0);
    puts("FP16 overflow translation/cache: ok");return 0;
}
