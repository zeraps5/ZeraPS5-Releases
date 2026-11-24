/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/shader/shader_dynamic.h"
#include "pal/pal.h"
#include "gpu/renderer.h"
#include "gpu/gpu_regs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static LONG CALLBACK trace_exception(EXCEPTION_POINTERS *ep) {
    if(ep->ExceptionRecord->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION)return EXCEPTION_CONTINUE_SEARCH;
    static LONG entered;if(InterlockedExchange(&entered,1))return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT ctx=*ep->ContextRecord;
    fprintf(stderr,"native test access violation at %llx, address %llx\n",(unsigned long long)ctx.Rip,
            (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    for(unsigned k=0;k<12 && ctx.Rip;k++) {
        HMODULE mod=NULL;char path[MAX_PATH]={0};
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (const char*)(uintptr_t)ctx.Rip,&mod);
        GetModuleFileNameA(mod,path,sizeof(path));
        fprintf(stderr,"frame %u: %s + %llx\n",k,path,(unsigned long long)(ctx.Rip-(uintptr_t)mod));
        DWORD64 image=0,frame=0;void *data=NULL;
        PRUNTIME_FUNCTION fn=RtlLookupFunctionEntry(ctx.Rip,&image,NULL);
        if(fn)RtlVirtualUnwind(UNW_FLAG_NHANDLER,image,ctx.Rip,fn,&ctx,&data,&frame,NULL);
        else {ctx.Rip=*(DWORD64*)(uintptr_t)ctx.Rsp;ctx.Rsp+=8;}
    }
    fflush(stderr);return EXCEPTION_CONTINUE_SEARCH;
}
#endif
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static const sh_stage_info test_info={.stage=SH_STAGE_CS,.cs_tgid_sgpr=16,.cs_tgid_enable=1};
static bool count_memory_prepares(void *context) {
    uint32_t *calls = context;
    (*calls)++;
    return true;
}
static void desc(uint32_t *d,guest_addr a,uint32_t stride,uint32_t n) {
    d[0]=(uint32_t)a;d[1]=(uint32_t)(a>>32)|(stride<<16);d[2]=n;d[3]=0x00016204;
}
static int null_image_case(gmem *mem,guest_addr base,uint32_t *data,sh_program *p,sh_eval *e,
                           sh_dynamic_plan *plan,renderer *r,gpu *g,bool sample) {
    uint32_t code[29]={0xd7460010,0x04010c10,
        0x7e020210,0x34020290,0x2c040290,0x7e120502,0x930ac009,
        0xf4280100,0x14000000,0xf4080600,0xfa000010,0xf42c0800,0x14000020,
        0x7e060280,0xf0380f08,0x00680403,0xb01e0002,0xe0302000,0x80010200,
        0x4a040504,0xe0702000,0x80060210,0x809e811e,0xbf07801e,0xbf85fff8,0xbf810000};
    uint32_t words=26;
    if(sample) {
        memmove(code+17,code+14,(words-14)*4);words+=3;
        code[14]=0x7e0802ff;code[15]=0x3f600000; /* Y=0.875 overlaps destination v4 */
        code[16]=0x7e0a0280; /* defined LOD=0 in v5 */
        code[17]=0xf0900108; /* image_sample_l, X=v3, Y=v4, LOD=v5, result=v4 */
    }
    guest_addr image=base+0x3c00;
    memset(data+0x3c00/4,0,1024);
    for(uint32_t y=0;y<4;y++)memset((uint8_t*)data+0x3c00+y*256,y*85,4);
    for(uint32_t c=0;c<2;c++) {
        uint32_t *row=data+0x100/4+c*16;desc(row,base+0x1000+c*0x400,4+c*4,64);
        memset(row+8,0,32);row[8]=(uint32_t)(image>>8);
        row[9]=(uint32_t)(image>>40)|(1u<<20)|(3u<<30);row[10]=3u<<14;row[11]=(9u<<28)|0xfac;
    }
    desc(data+0x110/4,base+0x2800,4,192);desc(e->sgpr_init,base+0x100,64,2);
    CHECK(sh_analyze(code,words,base,p) && sh_evaluate(p,mem,e));
    bool ready=sh_dynamic_plan_build(p,e,&test_info,mem,plan);
    if(!ready)fprintf(stderr,"null image plan: %s\n",plan->reason);
    CHECK(ready && plan->shapes[3].address_map);sh_dynamic_plan_free(plan);
    if(r) {
        CHECK(gmem_write(mem,base+0xe00,code,words*4));
        g->regs[mmCOMPUTE_PGM_LO]=(uint32_t)((base+0xe00)>>8);
        desc(g->regs+mmCOMPUTE_USER_DATA_0,base+0x100,64,2);
        uint32_t expected[GUEST_PAGE_SIZE/4];memcpy(expected,data,sizeof(expected));
        for(uint32_t c=0;c<3;c++)for(uint32_t lane=0;lane<64;lane++)
            expected[(0x2800+c*256+lane*4)/4]=c<2?data[(0x1000+c*0x400+lane*(4+c*4))/4]+(sample?0x3f800000u:4u):0;
        renderer_dispatch(r,g,3,1,1);renderer_sync_mem(r,base,GUEST_PAGE_SIZE);
        for(uint32_t k=0;k<PS5C_ARRAY_COUNT(expected);k++) {
            if(data[k]!=expected[k])fprintf(stderr,"null image word %x: %08x expected %08x\n",k,data[k],expected[k]);
            CHECK(data[k]==expected[k]);
        }
        puts(sample?"finite image sampling: 128 texel results and 64 invalid choices; overlapping input/output VGPR preserved":
                    "finite image guard: GPU verified 128 valid and 64 out-of-table selections with an independent output");
    }
    return 0;
}
static int multivma_case(gmem *mem,guest_addr base,uint32_t *data,sh_program *p,sh_eval *e,
                          sh_dynamic_plan *plan,renderer *r,gpu *g,bool wide_gap) {
    static const uint32_t code[]={0x7e020210,0x34020290,0x2c040290,0x7e120502,0x930aa009,
        0xf4280100,0x14000000,0xf4280500,0x14000010,0xb01e0002,
        0xe0302000,0x80010200,0xe0702000,0x80050200,0x809e811e,0xbf07801e,0xbf85fff9,0xbf810000};
    /* gpu_init reserves GDS after the first page; keep these inputs beyond it. */
    guest_addr inputs[2]={base+8*GUEST_PAGE_SIZE,base+(wide_gap?0x09000000:10*GUEST_PAGE_SIZE)},out;
    uint32_t *pages[2];
    for(uint32_t c=0;c<2;c++) {
        if(mem->shm) {
            bool shared=false;
            CHECK(gmem_map_direct(mem,inputs[c],GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,GPROT_RW,GMAP_FIXED,
                                  c*GUEST_PAGE_SIZE,"separate shared input",&out,&shared)==PS5C_OK && shared && out==inputs[c]);
        } else CHECK(gmem_map(mem,inputs[c],GUEST_PAGE_SIZE,GPROT_RW,GMAP_FIXED,"separate input",&out)==PS5C_OK && out==inputs[c]);
        pages[c]=gmem_host(mem,inputs[c],GUEST_PAGE_SIZE);CHECK(pages[c]);
        for(uint32_t k=0;k<GUEST_PAGE_SIZE/4;k++)pages[c][k]=0x900000+c*10000+k;
        desc(data+0x100/4+c*8,inputs[c]+0x100,4+c*4,64);
        desc(data+0x100/4+c*8+4,base+0x2000+c*0x400,4,64);
    }
    desc(e->sgpr_init,base+0x100,32,2);
    CHECK(sh_analyze(code,PS5C_ARRAY_COUNT(code),base,p) && sh_evaluate(p,mem,e));
    CHECK(sh_dynamic_plan_build(p,e,&test_info,mem,plan));
    CHECK(plan->spans[2].base==inputs[0]+0x100 && plan->spans[2].size==inputs[1]-inputs[0]+512);
    CHECK(plan->shapes[2].address_map==(wide_gap?2:1));
    if(wide_gap)CHECK(plan->shapes[2].buf_stride==2 && plan->spans[2].packed_size==768);
    sh_dynamic_plan_free(plan);
    if(r) {
        CHECK(gmem_write(mem,base+0xf00,code,sizeof(code)));
        g->regs[mmCOMPUTE_PGM_LO]=(uint32_t)((base+0xf00)>>8);desc(g->regs+mmCOMPUTE_USER_DATA_0,base+0x100,32,2);
        uint32_t expected[GUEST_PAGE_SIZE/4];memcpy(expected,data,sizeof(expected));
        for(uint32_t c=0;c<2;c++)for(uint32_t lane=0;lane<64;lane++)
            expected[(0x2000+c*0x400+lane*4)/4]=pages[c][(0x100+lane*(4+c*4))/4];
        renderer_dispatch(r,g,2,1,1);renderer_sync_mem(r,base,GUEST_PAGE_SIZE);
        for(uint32_t k=0;k<PS5C_ARRAY_COUNT(expected);k++) {
            if(data[k]!=expected[k])fprintf(stderr,"multi VMA word %x: %08x expected %08x\n",k,data[k],expected[k]);
            CHECK(data[k]==expected[k]);
        }
        desc(data+0x100/4+4,inputs[0]+0x100,4,64); /* a frozen copy may not alias an imported store */
        for(uint32_t k=0;k<64;k++)data[0x2400/4+k]=0xdeadbeef;
        memcpy(expected,data,sizeof(expected));renderer_dispatch(r,g,2,1,1);renderer_sync_mem(r,base,GUEST_PAGE_SIZE);
        CHECK(!memcmp(expected,data,sizeof(expected)));
        if(mem->shm) {
            guest_addr alias=base+12*GUEST_PAGE_SIZE;bool shared=false;
            CHECK(gmem_map_direct(mem,alias,GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,GPROT_RW,GMAP_FIXED,0,
                                  "input physical alias",&out,&shared)==PS5C_OK && shared && out==alias);
            desc(data+0x100/4+4,alias+0x100,4,64);
            memcpy(expected,data,sizeof(expected));renderer_dispatch(r,g,2,1,1);renderer_sync_mem(r,base,GUEST_PAGE_SIZE);
            CHECK(!memcmp(expected,data,sizeof(expected)));
            CHECK(gmem_unmap(mem,alias,GUEST_PAGE_SIZE)==PS5C_OK);
            puts("finite multi-VMA dispatch: physical alias store rejected, independent output sentinel unchanged");
        }
        for(uint32_t c=0;c<2;c++)for(uint32_t k=0;k<GUEST_PAGE_SIZE/4;k++)CHECK(pages[c][k]==0x900000+c*10000+k);
        printf("finite multi-VMA dispatch: GPU verified 128 choices across %llu MiB gap; copied-input store alias rejected\n",
               (unsigned long long)((inputs[1]-inputs[0])>>20));
    }
    CHECK(gmem_unmap(mem,inputs[1],GUEST_PAGE_SIZE)==PS5C_OK);
    CHECK(!sh_dynamic_plan_build(p,e,&test_info,mem,plan));
    CHECK(gmem_unmap(mem,inputs[0],GUEST_PAGE_SIZE)==PS5C_OK);
    return 0;
}
static int large_table_case(gmem *mem,guest_addr base,uint32_t *data,sh_program *p,sh_eval *e,
                           sh_dynamic_plan *plan,renderer *r,gpu *g,uint32_t variant) {
    const bool same_vma=variant==1,wide_table=variant==2;
    const uint32_t choices=wide_table?1094:180,stride=wide_table?592:32;
    uint32_t code[19]={0x7e020210,0x34020290,0x2c040290,0x7e120502,0x930aa009,
        0xf4280100,0x14000000,0xf4280500,0x14000010,0xb01e0002,
        0xe0302000,0x80010200,0xe0702000,0x80050200,0x809e811e,0xbf07801e,0xbf85fff9,0xbf810000};
    uint32_t words=18;
    if(wide_table){memmove(code+6,code+5,13*4);code[4]=0x930aff09;code[5]=stride;words++;}
    guest_addr input[2]={base+0x01000000,base+0x0a000000},output=base+0x0b000000,actual;
    const uint32_t input_size=(wide_table?16:4)*GUEST_PAGE_SIZE,chunk=same_vma?512:256;
    const uint32_t output_size=(wide_table?20:4)*GUEST_PAGE_SIZE;
    guest_addr table=wide_table?base+0x0c000000:base+0x100;
    uint32_t *rows=data+0x100/4;
    if(wide_table) {
        CHECK(gmem_map(mem,table,1u<<20,GPROT_RW,GMAP_FIXED,"large record table",&actual)==PS5C_OK);
        rows=gmem_host(mem,table,1u<<20);CHECK(rows);
    }
    uint64_t pool_size=input[1]-input[0]+input_size;
    uint32_t *src[2];
    if(same_vma)CHECK(gmem_map(mem,input[0],pool_size,GPROT_RW,GMAP_FIXED,"distant pools in one VMA",&actual)==PS5C_OK);
    for(unsigned k=0;k<2;k++) {
        if(!same_vma && mem->shm) {
            bool shared=false;
            CHECK(gmem_map_direct(mem,input[k],input_size,input_size,GPROT_RW,GMAP_FIXED,
                                   k*input_size,"large table geometry",&actual,&shared)==PS5C_OK && shared && actual==input[k]);
        } else if(!same_vma)CHECK(gmem_map(mem,input[k],input_size,GPROT_RW,GMAP_FIXED,"large table geometry",&actual)==PS5C_OK);
        src[k]=gmem_host(mem,input[k],input_size);CHECK(src[k]);
    }
    CHECK(gmem_map(mem,output,output_size,GPROT_RW,GMAP_FIXED,"large table output",&actual)==PS5C_OK);
    uint32_t *dst=gmem_host(mem,output,output_size);CHECK(dst);
    for(uint32_t c=0;c<choices;c++) {
        guest_addr addr=input[c&1]+(c/2)*chunk;
        desc(rows+c*(stride/4),addr,4,64);desc(rows+c*(stride/4)+4,output+c*256,4,64);
        for(uint32_t lane=0;lane<64;lane++)src[c&1][(c/2)*(chunk/4)+lane]=0x800000+c*1000+lane;
    }
    desc(e->sgpr_init,table,stride,choices);
    CHECK(sh_analyze(code,words,base,p) && sh_evaluate(p,mem,e));
    CHECK(sh_dynamic_plan_build(p,e,&test_info,mem,plan) && plan->choices==choices);
    CHECK(plan->shapes[2].address_map==2 && plan->shapes[2].buf_stride==(same_vma?64:2));
    CHECK(plan->spans[2].packed_size==choices*256+(same_vma?(choices-64)*256:0));
    sh_dynamic_plan_free(plan);
    /* The record cap remains enforced before dereferencing an oversized table. */
    e->sgpr_init[2]=SH_DYNAMIC_MAX_CHOICES+1;
    CHECK(!sh_dynamic_plan_build(p,e,&test_info,mem,plan));
    CHECK(strstr(plan->reason,"table size"));e->sgpr_init[2]=choices;
    if(r) {
        CHECK(gmem_write(mem,base+0x3e00,code,words*4));
        g->regs[mmCOMPUTE_PGM_LO]=(uint32_t)((base+0x3e00)>>8);
        desc(g->regs+mmCOMPUTE_USER_DATA_0,table,stride,choices);
        memset(dst,0xcd,output_size);
        renderer_submit_begin(r);renderer_dispatch(r,g,choices,1,1);renderer_sync_mem(r,output,output_size);
        for(uint32_t c=0;c<choices;c++)for(uint32_t lane=0;lane<64;lane++)CHECK(dst[c*64+lane]==0x800000+c*1000+lane);
        for(uint32_t k=choices*64;k<output_size/4;k++)CHECK(dst[k]==0xcdcdcdcd);
        printf("large table: %u lane results for %u objects, stride %u, %s and untouched output neighbors verified\n",
               choices*64,choices,stride,same_vma?"64 compact ranges in one VMA":"two compact ranges");
    }
    if(same_vma)CHECK(gmem_unmap(mem,input[0],pool_size)==PS5C_OK);
    else for(unsigned k=0;k<2;k++)CHECK(gmem_unmap(mem,input[k],input_size)==PS5C_OK);
    CHECK(gmem_unmap(mem,output,output_size)==PS5C_OK);
    if(wide_table)CHECK(gmem_unmap(mem,table,1u<<20)==PS5C_OK);
    return 0;
}

static int offset_case(gmem *mem,guest_addr base,uint32_t *data,sh_program *p,sh_eval *e,
                       sh_dynamic_plan *plan,renderer *r,gpu *g) {
    uint32_t code[]={0x7e020210,0x34020290,0x2c040290,0x7e120502,0x930aa009,
        0xf4280100,0x14000000,0xf4280500,0x14000010,0x36020083,0x34020282,
        0xb01e0002,0xe0303008,0x80010200,0xe0702000,0x80050200,0x809e811e,
        0xbf07801e,0xbf85fff9,0xbf810000};
    for(uint32_t c=0;c<2;c++) {
        desc(data+0x100/4+c*8,base+0x1000+c*0x400,4+c*4,64);
        desc(data+0x100/4+c*8+4,base+0x2000+c*0x400,4,64);
    }
    desc(e->sgpr_init,base+0x100,32,2);
    CHECK(sh_analyze(code,PS5C_ARRAY_COUNT(code),base,p) && sh_evaluate(p,mem,e));
    bool ready=sh_dynamic_plan_build(p,e,&test_info,mem,plan);
    if(!ready)fprintf(stderr,"bounded offset plan: %s\n",plan->reason);
    CHECK(ready && plan->spans[2].size==0x610);sh_dynamic_plan_free(plan);
    code[9]=0x2c02009e; /* right shift also bounds an arbitrary local ID before scaling */
    CHECK(sh_analyze(code,PS5C_ARRAY_COUNT(code),base,p) && sh_dynamic_plan_build(p,e,&test_info,mem,plan));
    CHECK(plan->spans[2].size==0x610);sh_dynamic_plan_free(plan);
    code[9]=0x7e020210; /* a workgroup SGPR is runtime data, even if its snapshot is zero */
    CHECK(sh_analyze(code,PS5C_ARRAY_COUNT(code),base,p) && !sh_dynamic_plan_build(p,e,&test_info,mem,plan));
    CHECK(strstr(plan->reason,"offset has no finite bound"));code[9]=0x36020083;
    if(r) {
        CHECK(gmem_write(mem,base+0xd00,code,sizeof(code)));
        g->regs[mmCOMPUTE_PGM_LO]=(uint32_t)((base+0xd00)>>8);desc(g->regs+mmCOMPUTE_USER_DATA_0,base+0x100,32,2);
        for(uint32_t c=0;c<2;c++)for(uint32_t lane=0;lane<64;lane++)
            data[(0x1000+c*0x400+lane*(4+c*4)+8+(lane&3)*4)/4]=0x70000+c*100+lane;
        uint32_t expected[GUEST_PAGE_SIZE/4];memcpy(expected,data,sizeof(expected));
        for(uint32_t c=0;c<2;c++)for(uint32_t lane=0;lane<64;lane++)
            expected[(0x2000+c*0x400+lane*4)/4]=data[(0x1000+c*0x400+lane*(4+c*4)+8+(lane&3)*4)/4];
        renderer_dispatch(r,g,2,1,1);renderer_sync_mem(r,base,GUEST_PAGE_SIZE);
        for(uint32_t i=0;i<PS5C_ARRAY_COUNT(expected);i++) {
            if(data[i]!=expected[i])fprintf(stderr,"bounded offset word %x: %08x expected %08x\n",i,data[i],expected[i]);
            CHECK(data[i]==expected[i]);
        }
        puts("finite offset dispatch: GPU verified 128 indexed choices with bounded per-lane byte offsets");
    }
    return 0;
}
static int object_case(gmem *mem,guest_addr base,uint32_t *data,sh_program *p,sh_eval *e,
                       sh_dynamic_plan *plan,renderer *r,gpu *g) {
    static const uint32_t object_code[]={0x7e020210,0x34020290,0x2c040290,0x7e120502,0x930aa009,
        0xf4240100,0x14000000,0xf4080802,0xfa000000,0xf4280500,0x14000010,0xb01e0002,
        0xe0302000,0x80080200,0xe0702000,0x80050200,0x809e811e,0xbf07801e,0xbf85fff9,0xbf810000};
    for(uint32_t c=0;c<2;c++) {
        guest_addr object=base+0x800+c*0x400;memcpy(data+0x100/4+c*8,&object,8);
        desc(data+(0x800+c*0x400)/4,base+0x1000+c*0x400,4+c*4,64);
        desc(data+0x100/4+c*8+4,base+0x2000+c*0x400,4,64);
    }
    desc(e->sgpr_init,base+0x100,32,2);
    CHECK(sh_analyze(object_code,PS5C_ARRAY_COUNT(object_code),base,p) && sh_evaluate(p,mem,e));
    bool ready=sh_dynamic_plan_build(p,e,&test_info,mem,plan);if(!ready)fprintf(stderr,"object plan: %s\n",plan->reason);
    CHECK(ready && plan->spans[1].snapshot && plan->spans[1].size==0x410);
    CHECK(plan->spans[3].base==base+0x1000 && plan->spans[3].size==0x600);
    sh_dynamic_plan_free(plan);
    desc(data+0x100/4+4,base+0x800,4,64);
    CHECK(!sh_dynamic_plan_build(p,e,&test_info,mem,plan) && strstr(plan->reason,"aliases immutable"));
    desc(data+0x100/4+4,base+0x2000,4,64);
    if(r) {
        CHECK(gmem_write(mem,base+0x700,object_code,sizeof(object_code)));
        g->regs[mmCOMPUTE_PGM_LO]=(uint32_t)((base+0x700)>>8);desc(g->regs+mmCOMPUTE_USER_DATA_0,base+0x100,32,2);
        uint32_t expected[GUEST_PAGE_SIZE/4];memcpy(expected,data,sizeof(expected));
        for(uint32_t c=0;c<2;c++)for(uint32_t lane=0;lane<64;lane++)
            expected[(0x2000+c*0x400+lane*4)/4]=data[(0x1000+c*0x400+lane*(4+c*4))/4];
        renderer_dispatch(r,g,2,1,1);renderer_sync_mem(r,base,GUEST_PAGE_SIZE);
        for(uint32_t i=0;i<PS5C_ARRAY_COUNT(expected);i++) {
            if(data[i]!=expected[i])fprintf(stderr,"object word %x: %08x expected %08x\n",i,data[i],expected[i]);
            CHECK(data[i]==expected[i]);
        }
        puts("finite object dispatch: GPU verified 128 descriptors loaded through selected scalar pointers");
    }
    return 0;
}
static int image_case(gmem *mem,guest_addr base,uint32_t *data,sh_program *p,sh_eval *e,
                      sh_dynamic_plan *plan,renderer *r,gpu *g) {
    static const uint32_t image_code[]={0x7e020210,0x34020290,0x2c040290,0x7e120502,0x930ac009,
        0xf4280100,0x14000000,0xf4280500,0x14000010,0xf42c0800,0x14000020,
        0x7e060280,0xf0380f08,0x00680403,0xb01e0002,0xe0302000,0x80010200,
        0x4a040504,0xe0702000,0x80050200,0x809e811e,0xbf07801e,0xbf85fff8,0xbf810000};
    guest_addr image=base+0x3c00;
    for(uint32_t c=0;c<2;c++) {
        uint32_t *row=data+0x100/4+c*16;
        desc(row,base+0x1000+c*0x400,4+c*4,64);desc(row+4,base+0x2000+c*0x400,4,64);
        memset(row+8,0,32);row[8]=(uint32_t)(image>>8);
        row[9]=(uint32_t)(image>>40)|(1u<<20)|(3u<<30);row[10]=3u<<14;row[11]=(9u<<28)|0xfac;
    }
    memset(data+0x3c00/4,255,1024);desc(e->sgpr_init,base+0x100,64,2);
    CHECK(sh_analyze(image_code,PS5C_ARRAY_COUNT(image_code),base,p) && sh_evaluate(p,mem,e));
    bool ready=sh_dynamic_plan_build(p,e,&test_info,mem,plan);if(!ready)fprintf(stderr,"image plan: %s\n",plan->reason);
    CHECK(ready && plan->images[3].valid && plan->images[3].desc[0]==(uint32_t)(image>>8));
    sh_dynamic_plan_free(plan);
    data[0x100/4+16+10]^=1u<<14; /* a different image shape must not reuse a single snapshot */
    CHECK(!sh_dynamic_plan_build(p,e,&test_info,mem,plan) && strstr(plan->reason,"different images"));
    data[0x100/4+16+10]^=1u<<14;
    if(r) {
        CHECK(gmem_write(mem,base+0x900,image_code,sizeof(image_code)));
        g->regs[mmCOMPUTE_PGM_LO]=(uint32_t)((base+0x900)>>8);
        desc(g->regs+mmCOMPUTE_USER_DATA_0,base+0x100,64,2);
        uint32_t expected[GUEST_PAGE_SIZE/4];memcpy(expected,data,sizeof(expected));
        for(uint32_t c=0;c<2;c++)for(uint32_t lane=0;lane<64;lane++)
            expected[(0x2000+c*0x400+lane*4)/4]=data[(0x1000+c*0x400+lane*(4+c*4))/4]+4;
        renderer_dispatch(r,g,2,1,1);renderer_sync_mem(r,base,GUEST_PAGE_SIZE);
        for(uint32_t i=0;i<PS5C_ARRAY_COUNT(expected);i++) {
            if(data[i]!=expected[i])fprintf(stderr,"image word %x: %08x expected %08x\n",i,data[i],expected[i]);
            CHECK(data[i]==expected[i]);
        }
        puts("finite image dispatch: GPU verified 128 table choices with image dimensions and buffer results");
    }
    return 0;
}
int main(void) {
#ifdef _WIN32
    if(getenv("PS5C_TEST_EXCEPTION_TRACE"))AddVectoredExceptionHandler(1,trace_exception);
#endif
    uint32_t code[]={0x7e020210,0x34020290,0x2c040290,0x7e120502,0x930aa009,
        0xf4280100,0x14000000,0xf4280500,0x14000010,0xb01e0002,
        0xe0302000,0x80010200,0xe0702000,0x80050200,0x809e811e,0xbf07801e,0xbf85fff9,0xbf810000};
    CHECK(pal_init()==PS5C_OK);
    gmem mem;gmem_config cfg={.start=0x200000000ull,.size=0x12000000,.identity=false};
    if(getenv("PS5C_TEST_SHARED_GPU")) {
        CHECK(pal_ph_supported());
        cfg=(gmem_config){.start=GUEST_PH_BASE-4*GUEST_PAGE_SIZE,.size=0x12000000,
                          .identity=true,.alloc_hint=GUEST_PH_BASE};
    }
    CHECK(gmem_init(&mem,&cfg)==PS5C_OK);guest_addr base;
    CHECK(gmem_map(&mem,0,GUEST_PAGE_SIZE,GPROT_RW,0,"finite table",&base)==PS5C_OK);
    uint32_t *data=gmem_host(&mem,base,GUEST_PAGE_SIZE);CHECK(data);
    for(uint32_t i=0;i<2;i++) {
        desc(data+0x100/4+i*8,base+0x1000+i*0x400,4+i*4,64);
        desc(data+0x100/4+i*8+4,base+0x2000+i*0x400,4,64);
    }
    sh_program *p=calloc(1,sizeof(*p));sh_dynamic_plan *plan=calloc(1,sizeof(*plan));
    sh_eval *e=calloc(1,sizeof(*e));CHECK(p && plan && e);
    desc(e->sgpr_init,base+0x100,32,2);e->sgpr_init[126]=e->sgpr_init[127]=0xffffffff;
    e->group_sgpr=16;e->group_sgprs=1; /* the workgroup id, as the renderer sets it */
    CHECK(sh_analyze(code,PS5C_ARRAY_COUNT(code),base,p) && p->back_branch);
    CHECK(sh_evaluate(p,&mem,e) && e->site[2].desc_unknown);
    uint32_t prepares=0;
    bool ok=sh_dynamic_plan_build_with_prepare(p,e,&test_info,&mem,plan,count_memory_prepares,&prepares);
    if(!ok)fprintf(stderr,"plan: %s\n",plan->reason);
    CHECK(ok && plan->choices==2 && prepares==1);
    CHECK(plan->spans[0].snapshot && plan->spans[0].size==64);
    CHECK(plan->spans[2].base==base+0x1000 && plan->spans[2].size==0x600);
    CHECK(plan->spans[3].base==base+0x2000 && plan->spans[3].size==0x500 && plan->spans[3].store);
    CHECK(plan->shapes[2].address_map && plan->shapes[3].address_map);
    sh_dynamic_plan_free(plan);
    {
        uint32_t saved_words=e->smem_words;
        const uint32_t oversized[]={UINT32_MAX,UINT32_MAX-276*p->site_count+1};
        for(unsigned k=0;k<PS5C_ARRAY_COUNT(oversized);k++) {
            e->smem_words=oversized[k];
            CHECK(!sh_dynamic_plan_build(p,e,&test_info,&mem,plan));
            CHECK(strstr(plan->reason,"uniform word capacity overflow") && !plan->uniforms && !plan->words);
        }
        e->smem_words=saved_words;
    }
    code[10]|=8; /* last valid index may read beyond the declared record allocation */
    CHECK(sh_analyze(code,PS5C_ARRAY_COUNT(code),base,p) && sh_dynamic_plan_build(p,e,&test_info,&mem,plan));
    CHECK(plan->spans[2].size==0x604);
    sh_dynamic_plan_free(plan);code[10]&=~8u;
    code[10]|=0x1000; /* idxen + unbounded per-lane byte offset */
    CHECK(sh_analyze(code,PS5C_ARRAY_COUNT(code),base,p) && !sh_dynamic_plan_build(p,e,&test_info,&mem,plan));
    CHECK(strstr(plan->reason,"offset has no finite bound"));code[10]&=~0x1000u;
    code[5]=0xf4900100;code[6]=0; /* a timestamp result is not a load from the initial SBASE */
    CHECK(sh_analyze(code,PS5C_ARRAY_COUNT(code),base,p) && !sh_dynamic_plan_build(p,e,&test_info,&mem,plan));
    code[5]=0xf4280100;code[6]=0x14000000;
    code[4]=0x930aa109; /* product 33 instead of declared record stride 32 */
    CHECK(sh_analyze(code,PS5C_ARRAY_COUNT(code),base,p) && !sh_dynamic_plan_build(p,e,&test_info,&mem,plan));
    code[4]=0x930aa009;code[2]=0x2c040280; /* unbounded shift source */
    CHECK(sh_analyze(code,PS5C_ARRAY_COUNT(code),base,p) && !sh_dynamic_plan_build(p,e,&test_info,&mem,plan));
    code[2]=0x2c040290;
    code[16]=0xbf85fff2; /* another iteration reaches scalarization after overwriting its vector source */
    CHECK(sh_analyze(code,PS5C_ARRAY_COUNT(code),base,p));
    CHECK(!sh_dynamic_plan_build_with_prepare(p,e,&test_info,&mem,plan,count_memory_prepares,&prepares));
    CHECK(prepares==1 && strstr(plan->reason,"ambiguous vector index source"));
    code[16]=0xbf85fff9;
    desc(data+0x100/4+4,base+0x100,4,64); /* store would change the immutable table */
    CHECK(sh_analyze(code,PS5C_ARRAY_COUNT(code),base,p) && !sh_dynamic_plan_build(p,e,&test_info,&mem,plan));
    CHECK(strstr(plan->reason,"aliases immutable")!=NULL);
    desc(data+0x100/4+4,base+0x2000,4,64);
    /* A checked scalar loop reads fields through each selected object pointer.
     * The extent must include all five iterations, not the evaluator's two
     * representative passes through a vector-dependent loop. */
    {
        uint32_t saved[16];memcpy(saved,data+0x100/4,sizeof(saved));
        const uint32_t scalar_loop[]={0x7e020210,0x34020290,0x2c040290,0x7e120502,0x930aa009,
            0xf4240100,0x14000000,0xf4000342,0xfa000264,0xbe940404,0xbe8c0380,
            0xbf040d0c,0xbf800000,0x8588807e,0xbeea0408,0xbefe0408,0xbf860006,
            0x988eff0c,0x00000064,0x810c810c,0xf408060a,0x1c000000,0xbf82fff4,0xbf810000};
        for(uint32_t c=0;c<2;c++) {
            guest_addr object=base+0x800+c*0x400;
            memcpy(data+0x100/4+c*8,&object,8);data[(0x800+c*0x400+0x264)/4]=3+c*2;
        }
        CHECK(sh_analyze(scalar_loop,PS5C_ARRAY_COUNT(scalar_loop),base,p));
        CHECK(sh_evaluate(p,&mem,e));
        bool ready=sh_dynamic_plan_build(p,e,&test_info,&mem,plan);if(!ready)fprintf(stderr,"scalar plan: %s\n",plan->reason);
        CHECK(ready && plan->spans[2].base==base+0x800 && plan->spans[2].size==0x4b4);
        CHECK(plan->spans[2].snapshot && plan->shapes[2].address_map);
        sh_dynamic_plan_free(plan);
        data[(0xc00+0x264)/4]=2048;
        CHECK(!sh_dynamic_plan_build(p,e,&test_info,&mem,plan));
        data[(0xc00+0x264)/4]=5;
        memcpy(data+0x100/4,saved,sizeof(saved));
    }
    if(getenv("PS5C_TEST_GPU")) {
        pal_window *window=NULL;
        CHECK(pal_window_open("ZeraPS5 finite-table dispatch",320,180,&window)==PS5C_OK);
        renderer *r=renderer_create(window,&mem);CHECK(r);
        gpu g;CHECK(gpu_init(&g,&mem,NULL)==PS5C_OK);
        CHECK(gmem_write(&mem,base+0x300,code,sizeof(code)));
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((base+0x300)>>8);
        g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)((base+0x300)>>40);
        g.regs[mmCOMPUTE_PGM_RSRC2]=(16u<<1)|(1u<<7);
        g.regs[mmCOMPUTE_NUM_THREAD_X]=64;g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
        desc(g.regs+mmCOMPUTE_USER_DATA_0,base+0x100,32,2);
        uint32_t expected[GUEST_PAGE_SIZE/4];
        for(uint32_t round=0;round<3;round++) {
            uint32_t extra=round==2?8:0;
            if(extra) {
                code[10]|=extra;
                CHECK(gmem_write(&mem,base+0xb00,code,sizeof(code)));
                g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((base+0xb00)>>8);
            }
            for(uint32_t c=0;c<2;c++)for(uint32_t lane=0;lane<64;lane++)
                data[(0x1000+c*0x400+lane*(4+c*4)+extra)/4]=round*10000+c*100+lane+1;
            memcpy(expected,data,sizeof(expected));
            for(uint32_t c=0;c<2;c++)for(uint32_t lane=0;lane<64;lane++)
                expected[(0x2000+c*0x400+lane*4)/4]=data[(0x1000+c*0x400+lane*(4+c*4)+extra)/4];
            renderer_dispatch(r,&g,2,1,1);
            renderer_sync_mem(r,base,GUEST_PAGE_SIZE);
            for(uint32_t i=0;i<PS5C_ARRAY_COUNT(expected);i++) {
                if(data[i]!=expected[i])fprintf(stderr,"round %u word %x: %08x expected %08x\n",round,i,data[i],expected[i]);
                CHECK(data[i]==expected[i]);
            }
        }
        code[10]&=~8u;
        code[2]=0x2c040280; /* a new program with an unbounded table index must stay rejected */
        CHECK(gmem_write(&mem,base+0x500,code,sizeof(code)));
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((base+0x500)>>8);
        memcpy(expected,data,sizeof(expected));
        renderer_dispatch(r,&g,2,1,1);renderer_sync_mem(r,base,GUEST_PAGE_SIZE);
        CHECK(!memcmp(expected,data,sizeof(expected)));
        CHECK(object_case(&mem,base,data,p,e,plan,r,&g)==0);
        CHECK(image_case(&mem,base,data,p,e,plan,r,&g)==0);
        CHECK(offset_case(&mem,base,data,p,e,plan,r,&g)==0);
        CHECK(null_image_case(&mem,base,data,p,e,plan,r,&g,false)==0);
        CHECK(null_image_case(&mem,base,data,p,e,plan,r,&g,true)==0);
        CHECK(multivma_case(&mem,base,data,p,e,plan,r,&g,false)==0);
        CHECK(multivma_case(&mem,base,data,p,e,plan,r,&g,true)==0);
        for(uint32_t v=0;v<3;v++)CHECK(large_table_case(&mem,base,data,p,e,plan,r,&g,v)==0);
        renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);
        puts("finite table dispatch: GPU verified 384 choices/stores, including last-record offsets; unsafe index still skipped");
    }
    CHECK(object_case(&mem,base,data,p,e,plan,NULL,NULL)==0);
    CHECK(image_case(&mem,base,data,p,e,plan,NULL,NULL)==0);
    CHECK(offset_case(&mem,base,data,p,e,plan,NULL,NULL)==0);
    CHECK(null_image_case(&mem,base,data,p,e,plan,NULL,NULL,false)==0);
    CHECK(null_image_case(&mem,base,data,p,e,plan,NULL,NULL,true)==0);
    CHECK(multivma_case(&mem,base,data,p,e,plan,NULL,NULL,false)==0);
    CHECK(multivma_case(&mem,base,data,p,e,plan,NULL,NULL,true)==0);
    for(uint32_t v=0;v<3;v++)CHECK(large_table_case(&mem,base,data,p,e,plan,NULL,NULL,v)==0);
    sh_dynamic_plan_free(plan);sh_eval_free(e);free(e);free(plan);free(p);
    gmem_free(&mem);pal_shutdown();puts("finite table plan: complete descriptor choices and negative provenance/bounds/alias cases verified");
    return 0;
}
