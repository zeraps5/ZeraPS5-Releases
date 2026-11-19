/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual held-label helpers, shared physical views and real GPU publication. */
#include "../../src/gpu/vk/vk_pipeline.c"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);fflush(stderr);_Exit(1);}}while(0)
static int alias_test(gmem *m,guest_addr a,guest_addr b,guest_addr c,guest_addr split) {
    CHECK(!vk_guest_ranges_disjoint(m,a,4,b,4));
    CHECK(!vk_guest_ranges_disjoint(m,a+GUEST_PAGE_SIZE+8,4,b+GUEST_PAGE_SIZE+8,4));
    CHECK(vk_guest_ranges_disjoint(m,a+15,1,b+16,1));CHECK(vk_guest_ranges_disjoint(m,a,4,c,4));
    CHECK(!vk_guest_ranges_disjoint(m,a,4,split,8*GUEST_PAGE_SIZE)); /* alias in second VMA */
    CHECK(!vk_guest_ranges_disjoint(m,a,4,c+8*GUEST_PAGE_SIZE,4)); /* unmapped */
    CHECK(!vk_guest_ranges_disjoint(m,a,4,UINT64_MAX-1,8));CHECK(vk_guest_ranges_disjoint(m,a,0,b,4));
    renderer *r=calloc(1,sizeof(*r));CHECK(r);r->mem=m;vk_pipelines *p=calloc(1,sizeof(*p));CHECK(p);r->pipes=p;
    p->held_count=1;p->held[0].rel=(gpu_release){.addr=b+8,.data_sel=1,.data=77};p->wb_count=1;p->wb[0].addr=a;p->wb[0].size=4;
    CHECK(!held_patchable(r,p,b,16));p->wb[0].addr=b;CHECK(!held_patchable(r,p,b,16));
    p->wb[0].addr=c;CHECK(held_patchable(r,p,b,16));
    uint8_t bytes[16];CHECK(gmem_read(m,b,bytes,sizeof(bytes)));held_patch(p,bytes,b,sizeof(bytes));
    uint32_t label;memcpy(&label,bytes+8,4);CHECK(label==77);
    p->held[0].rel.data_sel=3;CHECK(!held_patchable(r,p,b,16));
    p->held[0].rel.data_sel=1;p->wb[0].addr=c+8*GUEST_PAGE_SIZE;CHECK(!held_patchable(r,p,b,16));
    free(p);free(r);puts("held labels: physical aliases, split views, disjoint spans, gaps and overflow passed");return 0;
}
static int gpu_test(gmem *m,guest_addr a,guest_addr b,guest_addr code,guest_addr out) {
    static const uint32_t writer[]={0x7e020208,0xe0700000,0x80000100,0xbf810000};
    static const uint32_t reader[]={0xe0380000,0x80010000,0xbf8c3f70,0xe0780000,0x80000000,0xbf810000};
    CHECK(gmem_write(m,code,writer,sizeof(writer)) && gmem_write(m,code+0x100,reader,sizeof(reader)));
    uint32_t input[4]={0x11112222,0x33334444,0,0x55556666};CHECK(gmem_write(m,a,input,sizeof(input)));
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 held label alias",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,m);CHECK(r);gpu g;CHECK(gpu_init(&g,m,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(code>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(code>>40);g.regs[mmCOMPUTE_PGM_RSRC2]=16u<<1;
    g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;u[0]=(uint32_t)a;u[1]=(uint32_t)(a>>32);u[2]=4;u[3]=0x16204;u[8]=0x12345678;
    renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);
    gpu_release release={.addr=b+8,.data=77,.data_sel=1};CHECK(renderer_release(r,&g,&release));
    uint32_t seen;CHECK(gmem_read(m,b,&seen,4) && seen==input[0]);CHECK(gmem_read(m,b+8,&seen,4) && seen==0);
    g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((code+0x100)>>8);u[0]=(uint32_t)out;u[1]=(uint32_t)(out>>32);u[2]=16;
    u[4]=(uint32_t)b;u[5]=(uint32_t)(b>>32);u[6]=16;u[7]=0x16204;
    renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,16);
    uint32_t actual[4];CHECK(gmem_read(m,out,actual,sizeof(actual)));
    bool correct=actual[0]==0x12345678 && actual[1]==input[1] && actual[2]==77 && actual[3]==input[3];
    if(!correct)fprintf(stderr,"alias result: %08x %08x %08x %08x\n",actual[0],actual[1],actual[2],actual[3]);
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);
    CHECK(correct);
    puts("held labels GPU: alias reader received completed GPU write and ordered label; untouched words preserved");return 0;
}
int main(void) {
    CHECK(pal_init()==PS5C_OK);
    if(!pal_ph_supported()){puts("held labels: shared mappings unavailable, skipped");pal_shutdown();return 0;}
    gmem m;gmem_config cfg=gmem_default_config();CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr a=m.ph_start,b=a+4*GUEST_PAGE_SIZE,c=b+4*GUEST_PAGE_SIZE,split=c+16*GUEST_PAGE_SIZE,at=0;bool shared=false;
    guest_addr views[]={a,b,c,split,split+4*GUEST_PAGE_SIZE};uint64_t phys[]={0,0,8*GUEST_PAGE_SIZE,16*GUEST_PAGE_SIZE,0};
    for(unsigned i=0;i<5;i++)CHECK(gmem_map_direct(&m,views[i],4*GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,GPROT_RW,GMAP_FIXED,phys[i],"alias fixture",&at,&shared)==PS5C_OK && shared);
    guest_addr code;CHECK(gmem_map(&m,0,4*GUEST_PAGE_SIZE,GPROT_RW,0,"alias code/output",&code)==PS5C_OK);
    CHECK(gmem_pin_mappings(&m));CHECK(alias_test(&m,a,b,c,split)==0);gmem_unpin_mappings(&m);
    if(getenv("PS5C_TEST_GPU"))CHECK(gpu_test(&m,a,b,code,code+GUEST_PAGE_SIZE)==0);
    gmem_free(&m);pal_shutdown();return 0;
}
