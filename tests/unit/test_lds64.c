/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/shader/shader.h"
#include "gpu/shader/spv.h"
#include "gpu/vk/vk_internal.h"
#include "gpu/vk/vk_shader_cache.h"
#include "gpu/gpu_regs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static renderer *cleanup_renderer;
static pal_window *cleanup_window;
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);if(cleanup_renderer)renderer_destroy(cleanup_renderer);if(cleanup_window)pal_window_destroy(cleanup_window);return 1;}}while(0)
static uint32_t vop2(unsigned op,unsigned dst,unsigned src0,unsigned vsrc1){return (op<<25)|(dst<<17)|(vsrc1<<9)|src0;}
static unsigned program(uint32_t *code,unsigned offset,bool inactive){
    unsigned n=0;
    code[n++]=vop2(0x1a,2,131,0); /* v2 = tid * 8 */
    code[n++]=0x7e0c0280;code[n++]=0x7e0e0280; /* v6:7 = 0 */
    code[n++]=0xd9340000;code[n++]=0x00000602; /* ds_write_b64 v2,v6:7 */
    code[n++]=0xbf8a0000; /* s_barrier */
    code[n++]=0x7e020281; /* v1 = 1 */
    code[n++]=vop2(0x1a,4,256,1); /* v4 = 1 << (tid & 31) */
    code[n++]=0x7e0a0204; /* v5 = workgroup X (s4) */
    code[n++]=vop2(0x1a,5,261,1); /* v5 = 1 << group X */
    code[n++]=0x7e040280; /* v2 = 0 */
    if(inactive)code[n++]=0xbefe0480;
    code[n++]=0xd9280000|offset;code[n++]=0x00000402;
    if(inactive)code[n++]=0xbefe04c1;
    code[n++]=0xbf8a0000;
    code[n++]=0xd9d80000|(offset<512?offset:0);code[n++]=0x08000002; /* ds_read_b64 v8:9,v2 */
    /* Interoperate with ordinary u32 access to the same shared storage. */
    code[n++]=0xd8d80000|(offset<512?offset:0);code[n++]=0x0a000002; /* ds_read_b32 v10,v2 */
    code[n++]=0x7e060204; /* v3 = workgroup X */
    code[n++]=vop2(0x1a,3,134,3); /* group * 64 */
    code[n++]=vop2(0x25,3,256,3); /* + tid */
    code[n++]=0x7e160300; /* v11 = tid */
    code[n++]=0xe0782000;code[n++]=0x80000803;
    code[n++]=0xbf810000;return n;
}
static int cpu_test(void){
    uint32_t code[48],n=program(code,8,false);rdna2_inst in;
    CHECK(rdna2_decode((uint32_t[]){0xd9280008,0x00000402},2,&in));
    CHECK(in.op==0x4a && !strcmp(in.name,"ds_or_b64") && in.offset==8 && in.vdata==4);
    CHECK(rdna2_decode((uint32_t[]){0xd93c0400,0x00060402},2,&in));
    CHECK(in.op==0x4f && !strcmp(in.name,"ds_write2st64_b64") && in.offset==0x400);
    CHECK(rdna2_decode((uint32_t[]){0xd9e00400,0x08000002},2,&in));
    CHECK(in.op==0x78 && !strcmp(in.name,"ds_read2st64_b64"));
    sh_program *p=calloc(1,sizeof(*p));CHECK(p && sh_analyze(code,n,0,p));
    sh_stage_info info={.stage=SH_STAGE_CS,.local_size={64,1,1},.cs_tid_components=1,
                        .cs_tgid_sgpr=4,.cs_tgid_enable=1,.lds_dwords=128};
    sh_site_shape shapes[SH_MAX_SITES]={0};uint32_t *spv=NULL;size_t words=0;
    sh_set_lds_atomic64(false);CHECK(!sh_translate(p,&info,shapes,0,&spv,&words));
    sh_set_lds_atomic64(true);CHECK(sh_translate(p,&info,shapes,0,&spv,&words));
    CHECK(spv[1]==0x00010400);unsigned aliased=0,atomic_or=0;
    for(size_t i=5;i<words;i+=spv[i]>>16){
        CHECK(spv[i]>>16);
        if((spv[i]&65535)==SpvOpDecorate && spv[i+2]==SpvDecorationAliased)aliased++;
        if((spv[i]&65535)==SpvOpAtomicOr)atomic_or++;
    }
    CHECK(aliased==2 && atomic_or==1);
    if(getenv("PS5C_TEST_SPIRV")){
        char path[512];snprintf(path,sizeof(path),"%s/lds64.spv",getenv("PS5C_TEST_SPIRV"));
        FILE *f=fopen(path,"wb");CHECK(f && fwrite(spv,4,words,f)==words && !fclose(f));
    }
    vk_shader_cache_device(false,false,false,false);uint64_t before=vk_shader_cache_key(p,&info,shapes,0);
    vk_shader_cache_device(false,false,true,false);CHECK(before!=vk_shader_cache_key(p,&info,shapes,0));
    sh_set_lds_atomic64(false);free(spv);free(p);
    puts("DS_OR_B64 decode, capability gate, aliased storage, atomic instruction and cache: ok");return 0;
}
static int gpu_test(void){
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,4*GUEST_PAGE_SIZE,GPROT_RW,0,"LDS64 fixtures",&a)==PS5C_OK);
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 LDS64 regression",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);cleanup_renderer=r;cleanup_window=window;
    gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    guest_addr out=a+0x4000;uint32_t *output=gmem_host(&m,out,2048);CHECK(output);
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    u[0]=(uint32_t)out;u[1]=(uint32_t)(out>>32)|(16u<<16);u[2]=128;u[3]=0x16204;
    g.regs[mmCOMPUTE_NUM_THREAD_X]=64;g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    unsigned cases=0;
    for(unsigned large=0;large<2;large++)for(unsigned variant=0;variant<5;variant++){
        uint32_t blocks=large?64:1; /* 32 KiB aliased twice must count once. */
        unsigned offset=variant==1?8:variant==2?504:variant==4?blocks*512:0;
        bool inactive=variant==3;
        g.regs[mmCOMPUTE_PGM_RSRC2]=(4u<<1)|(1u<<7)|(blocks<<15);
        uint32_t code[48],n=program(code,offset,inactive);guest_addr pc=a+variant*0x100;
        CHECK(gmem_write(&m,pc,code,n*4));g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
        memset(output,0xcd,2048);renderer_submit_begin(r);renderer_dispatch(r,&g,2,1,1);renderer_sync_mem(r,out,2048);
        bool ignored=inactive || variant==4;
        for(unsigned group=0;group<2;group++)for(unsigned tid=0;tid<64;tid++){
            unsigned record=group*64+tid;uint32_t lo=ignored?0:UINT32_MAX,hi=ignored?0:1u<<group;
            if(output[record*4]!=lo || output[record*4+1]!=hi)
                fprintf(stderr,"LDS case %u:%u group %u tid %u: %08x:%08x expected %08x:%08x\n",large,variant,group,tid,
                    output[record*4+1],output[record*4],hi,lo);
            CHECK(output[record*4]==lo && output[record*4+1]==hi && output[record*4+2]==lo && output[record*4+3]==tid);
        }
        cases++;
    }
    printf("LDS64 GPU: %u cases, 128 lanes each; both halves, u32 alias, groups, offsets, EXEC and bounds verified\n",cases);
    {
        /* Two qwords separated by 4*64*8 bytes, preserving both halves. */
        const uint32_t code[]={
            0x34040083, /* v2 = tid * 8 */
            0x7e080300, 0x7e0a0204, /* v4=tid, v5=group */
            0x3a0c00ff,0x76543210, /* v6=tid XOR payload */
            0x7e0e02ff,0xaabbccdd,
            0xd93c0400,0x00060402, /* ds_write2st64_b64 v2,v[4:5],v[6:7] */
            0xbf8a0000,
            0xd9e00400,0x08000002, /* ds_read2st64_b64 v[8:11],v2 */
            0xbf8cc07f,
            0x7e060204,0x34060686,0x4a060700, /* index=group*64+tid */
            0xe0782000,0x80000803,0xbf810000
        };
        guest_addr pc=a+0x1000;
        CHECK(gmem_write(&m,pc,code,sizeof(code)));
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
        g.regs[mmCOMPUTE_PGM_RSRC2]=(4u<<1)|(1u<<7)|(6u<<15);
        memset(output,0xcd,2048);renderer_submit_begin(r);renderer_dispatch(r,&g,2,1,1);renderer_sync_mem(r,out,2048);
        for(unsigned i=0;i<128;i++) {
            uint32_t tid=i&63,expected[4]={tid,i/64,tid^0x76543210,0xaabbccdd};
            for(unsigned k=0;k<4;k++) {
                if(output[i*4+k]!=expected[k])fprintf(stderr,"LDS stride64 lane %u component %u: %08x expected %08x\n",i,k,output[i*4+k],expected[k]);
                CHECK(output[i*4+k]==expected[k]);
            }
        }
        puts("LDS stride64 GPU: read/write2st64_b64, 128 lanes, separate groups and both qword halves verified");
    }
    renderer_destroy(r);cleanup_renderer=NULL;free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);cleanup_window=NULL;gmem_free(&m);pal_shutdown();return 0;
}
int main(void){CHECK(cpu_test()==0);return getenv("PS5C_TEST_GPU")?gpu_test():0;}
