/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Unknown runtime NUM_RECORDS, but a statically bounded global invocation
 * index. Exercise the actual GPU and publication, including CPU edits outside
 * the prefix and later readers requesting the complete original allocation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/gpu_regs.h"
#include "gpu/vk/vk_internal.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);fflush(stderr);_Exit(1);}}while(0)
int main(void) {
    /* Read count through VMEM (not evaluated on the CPU), transfer the address
     * and count to SGPRs, and store one dword per invocation at stride 20. */
    static const uint32_t code[]={
        0xd7460000,0x04010c0e, /* v0 = (s14 << 6) + local id */
        0xe0300000,0x80020400, /* v4 = count buffer s[8:11] */
        0xbf8c3f70,0x7e020204,0x7e040205, /* v1=s4, v2=s5 */
        0x7e060207,0x7e0a020d, /* v3=s7, v5=s13 (value) */
        0x7e000501,0x7e020502,0x7e040504,0x7e060503, /* s0..3=v1,v2,v4,v3 */
        0xe0702010,0x80000500,0xbf810000};
    CHECK(pal_init()==PS5C_OK);gmem mem;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&mem,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&mem,0,40u<<20,GPROT_RW,0,"buffer prefix GPU fixture",&a)==PS5C_OK);
    CHECK(gmem_write(&mem,a,code,sizeof(code)));
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 bounded buffer",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&mem);CHECK(r);gpu g;CHECK(gpu_init(&g,&mem,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(a>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(a>>40);
    g.regs[mmCOMPUTE_PGM_RSRC2]=(14u<<1)|(1u<<7);
    g.regs[mmCOMPUTE_NUM_THREAD_X]=64;g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    guest_addr output=a+0x4000,count=a+0x1000,label=a+(39u<<20);
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    u[4]=(uint32_t)output;u[5]=(uint32_t)(output>>32)|(20u<<16);u[6]=0;u[7]=0x16204;
    u[8]=(uint32_t)count;u[9]=(uint32_t)(count>>32)|(4u<<16);u[10]=1;u[11]=0x16204;
    u[13]=0x12345678;uint32_t records=128;CHECK(gmem_write(&mem,count,&records,4));
    uint8_t *bytes=gmem_host(&mem,output,32u<<20);CHECK(bytes);memset(bytes,0x5a,32u<<20);
    CHECK(gmem_write_u64(&mem,label,0));vk_begin(r);renderer_dispatch(r,&g,2,1,1);
    bytes[100000]=0x83; /* CPU write outside the proved GPU range */
    gpu_release release={.addr=label,.data=77,.data_sel=1};CHECK(renderer_release(r,&g,&release));
    renderer_sync_mem(r,output,32u<<20);
    uint32_t seen;CHECK(gmem_read(&mem,label,&seen,4) && seen==77);
    for(uint32_t i=0;i<(32u<<20);i++) {
        uint8_t want=i==100000?0x83:0x5a;
        if(i>=16 && (i-16)/20<128 && (i-16)%20<4)want=(uint8_t)(u[13]>>(((i-16)%20)*8));
        CHECK(bytes[i]==want);
    }
    uint64_t draws,dispatches;vk_pipeline_counts(r,&draws,&dispatches);CHECK(dispatches==1);
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&mem);pal_shutdown();
    puts("bounded buffer: unknown runtime count, scalar lane destinations, strided writes, full readback and CPU edits passed");return 0;
}
