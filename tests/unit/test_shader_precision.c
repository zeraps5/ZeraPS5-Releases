/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exact arithmetic must distinguish one-rounding FMA from two-rounding MAD.
 * Operands come from user SGPRs at dispatch, not compiler-folded constants. */
#include "base/base.h"
#include "gpu/gpu_regs.h"
#include "gpu/renderer.h"
#include "gpu/shader/shader.h"
#include "gpu/shader/spv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"precision line %d: %s\n",__LINE__,#x);return 1;}} while(0)

static unsigned program(uint32_t *code,unsigned kind) {
    const uint32_t prefix[]={0x7e000200,0x7e020201,0x7e040202};
    memcpy(code,prefix,sizeof(prefix));unsigned n=3;
    if(kind==2) code[n++]=(0x2bu<<25)|(2u<<17)|(1u<<9)|256u; /* FMAC v2,v0,v1 */
    else {code[n++]=kind?0xd5410002:0xd54b0002;code[n++]=256u|(257u<<9)|(258u<<18);}
    code[n++]=0xe0700000;code[n++]=0x80030200;code[n++]=0xbf810000;
    return n;
}

static int translation_test(void) {
    sh_program *p=calloc(1,sizeof(*p));CHECK(p);
    sh_stage_info info={.stage=SH_STAGE_CS,.local_size={1,1,1}};
    sh_site_shape shapes[4]={{0}};
    for(unsigned kind=0;kind<3;kind++) {
        uint32_t code[12],*spv=NULL;size_t words=0;unsigned n=program(code,kind);
        CHECK(sh_analyze(code,n,0x100000,p));
        CHECK(sh_translate(p,&info,shapes,0,&spv,&words));
        unsigned fma=0;
        for(size_t i=5;i<words;i+=spv[i]>>16) {
            unsigned op=spv[i]&65535,wc=spv[i]>>16;CHECK(wc && i+wc<=words);
            if(op!=SpvOpExtInst || wc<5 || spv[i+4]!=GLSLstd450Fma)continue;
            fma++;unsigned decorations=0;
            for(size_t j=5;j<words;j+=spv[j]>>16)
                if((spv[j]&65535)==SpvOpDecorate && spv[j+1]==spv[i+2] && spv[j+2]==SpvDecorationNoContraction)decorations++;
            CHECK(decorations==1);
        }
        CHECK(fma==(kind==1?0:1));
        const char *dir=getenv("PS5C_TEST_SPIRV");
        if(dir){char path[512];snprintf(path,sizeof(path),"%s/precision_%u.spv",dir,kind);
            FILE*f=fopen(path,"wb");CHECK(f);CHECK(fwrite(spv,4,words,f)==words);CHECK(fclose(f)==0);}
        free(spv);
    }
    for(unsigned mask=0;mask<2;mask++) {
        p->param_mask=mask?3:0;uint32_t *spv=NULL;size_t words=0;
        sh_stage_info mesh={.stage=SH_STAGE_MESH,.mesh_compute=1,.mesh_max_vertices=3,.mesh_max_primitives=1};
        CHECK(sh_mesh_replay(p,&mesh,&spv,&words));unsigned invariant=0;
        for(size_t i=5;i<words;i+=spv[i]>>16)
            if((spv[i]&65535)==SpvOpMemberDecorate && spv[i+2]==0 && spv[i+3]==SpvDecorationInvariant)invariant++;
        CHECK(invariant==1);
        const char*dir=getenv("PS5C_TEST_SPIRV");
        if(dir){char path[512];snprintf(path,sizeof(path),"%s/replay_precision_%u.spv",dir,mask);
            FILE*f=fopen(path,"wb");CHECK(f);CHECK(fwrite(spv,4,words,f)==words);CHECK(fclose(f)==0);}
        free(spv);
    }
    free(p);return 0;
}

static int gpu_test(void) {
    static const uint32_t cases[][4]={
        {0x3f800001,0x3f7ffffe,0xbf800000,0xa8800000}, /* -2^-46; MAD rounds to0 */
        {0xbf800001,0x3f7ffffe,0x3f800000,0x28800000}, /* +2^-46 */
        {0x45800800,0x45800800,0xcb801000,0x3f800000}, /* 4097^2-16785408 =1 */
    };
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr page;
    CHECK(gmem_map(&m,0,4*GUEST_PAGE_SIZE,GPROT_RW,0,"FMA precision",&page)==PS5C_OK);
    uint32_t*out=gmem_host(&m,page+0x8000,4);CHECK(out);
    pal_window*window=NULL;CHECK(pal_window_open("ZeraPS5 exact FMA regression",320,180,&window)==PS5C_OK);
    renderer*r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_PGM_RSRC2]=16u<<1;
    g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    uint32_t*u=g.regs+mmCOMPUTE_USER_DATA_0;
    u[12]=(uint32_t)(page+0x8000);u[13]=(uint32_t)((page+0x8000)>>32);u[14]=4;u[15]=0x16204;
    for(unsigned kind=0;kind<3;kind++)for(unsigned sample=0;sample<3;sample++) {
        uint32_t code[12];unsigned n=program(code,kind);guest_addr pc=page+kind*0x100;
        CHECK(gmem_write(&m,pc,code,n*4));
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
        u[0]=cases[sample][0];u[1]=cases[sample][1];u[2]=cases[sample][2];*out=0xdeadbeef;
        renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,page+0x8000,4);
        uint32_t wanted=kind==1?0:cases[sample][3];
        if(*out!=wanted)fprintf(stderr,"kind%u sample%u: %08x expected %08x\n",kind,sample,*out,wanted);
        CHECK(*out==wanted);
    }
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&m);pal_shutdown();
    puts("exact FMA GPU:9 results distinguish FMA/FMAC from two-rounding MAD");return 0;
}

int main(void) {
    CHECK(_putenv_s("PS5C_SHADER_EXACT","2")==0); ps5c_env_reset();
    CHECK(translation_test()==0);
    if(getenv("PS5C_TEST_GPU"))CHECK(gpu_test()==0);
    puts("exact shader arithmetic contract verified");return 0;
}
