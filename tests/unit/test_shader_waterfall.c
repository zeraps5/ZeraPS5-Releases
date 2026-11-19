/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Synthetic wave grouping loop: every active lane must be visited exactly once. */
#include "gpu/shader/shader.h"
#include "gpu/shader/spv.h"
#include "gpu/vk/vk_internal.h"
#include "gpu/gpu_regs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static uint32_t mov(unsigned dst,unsigned src){return 0x7e000200|(dst<<17)|src;}
static unsigned program(uint32_t *c){
    unsigned n=0;
    c[n++]=0xd7460000;c[n++]=0x04010701; /* flat = y*8+x */
    c[n++]=0xd7460008;c[n++]=0x04010c1c; /* v8 = group.x*64 + flat */
    c[n++]=0x4a0400ff;c[n++]=100; /* v2 = flat+100 */
    c[n++]=(0x16u<<25)|(1u<<17)|22; /* key = flat >> s22 */
    c[n++]=mov(9,129);c[n++]=(0x1au<<25)|(9u<<17)|(9u<<9)|256; /* bit = 1 << flat */
    c[n++]=0xbefe0414; /* exec = s[20:21] */
    c[n++]=0xbe84047e; /* s[4:5] = exec */
    c[n++]=0xbe8c0380; /* s12 = 0 */
    unsigned empty=n;c[n++]=0;
    unsigned loop=n;
    c[n++]=0xbe861404; /* s6 = ff1(s[4:5]) */
    c[n++]=0xd7600007;c[n++]=(6u<<9)|257; /* s7 = readlane(v1,s6) */
    c[n++]=0x7d8402f9;c[n++]=0x06868807; /* s[8:9] = key == s7 */
    c[n++]=0xbe8a2408; /* s10:11 = exec; exec &= s8:9 */
    c[n++]=mov(4,7);c[n++]=mov(5,6);c[n++]=mov(6,12);c[n++]=mov(7,256);
    c[n++]=0xe0782000;c[n++]=0x80000408; /* output indexed by v8 */
    /* Reduce selected lanes with DPP, combine both 32-lane halves, isolate
     * the first active lane using 64-bit negation, and issue one atomic OR. */
    c[n++]=0xbeea287e; /* save exec, enable all lanes */
    c[n++]=0x02061280; /* v3 = selected ? bit(v9) : 0 */
    for(unsigned shift=1;shift<=8;shift*=2){
        c[n++]=0x380606fa;c[n++]=0xff010003|((0x110+shift)<<8);
    }
    c[n++]=0xd778100c;c[n++]=0x03058303; /* v12 = other row's last lane */
    c[n++]=0x38061903;
    c[n++]=0xbefe046a;
    c[n++]=0xd760000e;c[n++]=(159u<<9)|259;
    c[n++]=0xd760000f;c[n++]=(191u<<9)|259;
    c[n++]=0x880d0f0e;
    c[n++]=0x80ea6a80;c[n++]=0x82eb6b80;c[n++]=0x87fe7e6a;
    c[n++]=mov(10,13);c[n++]=mov(11,7);
    c[n++]=0xd746000b;c[n++]=0x042d0c1c;
    c[n++]=0xe0e82000;c[n++]=0x80060a0b;
    c[n++]=0x800c810c; /* s12 += 1 */
    c[n++]=0x8a840804; /* s4:5 &= ~s8:9; scc = mask!=0 */
    c[n++]=0xbefe040a; /* restore exec without modifying scc */
    c[n++]=0; /* backward branch, patched below */
    c[n-1]=0xbf850000|((loop-n)&0xffff);
    c[empty]=0xbf880000|((n-empty-1)&0xffff);
    c[n++]=0xd760000e;c[n++]=(191u<<9)|258; /* readlane(v2,63) */
    c[n++]=0xd760000f;c[n++]=(159u<<9)|258; /* readlane(v2,31) */
    c[n++]=mov(4,14);c[n++]=mov(5,15);c[n++]=mov(6,12);c[n++]=mov(7,256);
    c[n++]=0xe0782000;c[n++]=0x80040408; /* second output s16:19 */
    c[n++]=0xbf810000;return n;
}
static unsigned read_program(uint32_t *c){
    unsigned n=0;
    c[n++]=0xd7460000;c[n++]=0x04010701;
    c[n++]=0xd7460008;c[n++]=0x04010c1c;
    c[n++]=0x4a0400ff;c[n++]=100;
    c[n++]=0xbefe0414;c[n++]=0xbe8c0380;
    c[n++]=0xd760000e;c[n++]=(191u<<9)|258;
    c[n++]=0xd760000f;c[n++]=(159u<<9)|258;
    c[n++]=mov(4,14);c[n++]=mov(5,15);c[n++]=mov(6,12);c[n++]=mov(7,256);
    c[n++]=0xe0782000;c[n++]=0x80040408;
    c[n++]=0xbf810000;return n;
}
static int cpu_test(void){
    uint32_t code[128],n=program(code),*spv;size_t words;sh_program *p=calloc(1,sizeof(*p));CHECK(p);
    CHECK(sh_analyze(code,n,0,p));
    sh_stage_info info={.stage=SH_STAGE_CS,.local_size={8,8,1},.cs_tid_components=3,.cs_tgid_sgpr=28,.cs_tgid_enable=1};
    sh_site_shape shapes[SH_MAX_SITES]={0};CHECK(sh_translate(p,&info,shapes,0,&spv,&words));
    unsigned barriers=0;
    for(size_t i=5;i<words;i+=spv[i]>>16){CHECK(spv[i]>>16);barriers+=(spv[i]&65535)==SpvOpControlBarrier;}
    CHECK(barriers>0);
    if(getenv("PS5C_TEST_SPIRV")){char path[512];snprintf(path,sizeof(path),"%s/waterfall.spv",getenv("PS5C_TEST_SPIRV"));FILE *f=fopen(path,"wb");CHECK(f && fwrite(spv,4,words,f)==words && !fclose(f));}
    free(spv);
    /* Cross-wave barriers are only legal when the entire workgroup is the
     * one emulated wave. Partial/multiple waves keep the existing path. */
    for(unsigned size=32;size<=128;size*=4){
        info.local_size[0]=size;info.local_size[1]=1;
        CHECK(sh_translate(p,&info,shapes,0,&spv,&words));barriers=0;
        for(size_t i=5;i<words;i+=spv[i]>>16)barriers+=(spv[i]&65535)==SpvOpControlBarrier;
        CHECK(!barriers);free(spv);
    }
    free(p);return 0;
}
static int gpu_test(void){
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr a;CHECK(gmem_map(&m,0,4*GUEST_PAGE_SIZE,GPROT_RW,0,"wave64 waterfall",&a)==PS5C_OK);
    pal_window *w=NULL;CHECK(pal_window_open("ZeraPS5 wave64 regression",320,180,&w)==PS5C_OK);renderer *r=renderer_create(w,&m);CHECK(r);
    gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    uint32_t code[128],n=program(code);CHECK(gmem_write(&m,a,code,n*4));
    g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(a>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(a>>40);
    g.regs[mmCOMPUTE_NUM_THREAD_X]=8;g.regs[mmCOMPUTE_NUM_THREAD_Y]=8;g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    g.regs[mmCOMPUTE_PGM_RSRC2]=(28u<<1)|(1u<<7)|(2u<<11);
    guest_addr out=a+0x4000,out2=a+0x5000;uint32_t *p=gmem_host(&m,out,2048),*q=gmem_host(&m,out2,2048);CHECK(p && q);
    guest_addr counters=a+0x6000;uint32_t *count=gmem_host(&m,counters,512);CHECK(count);
    u[24]=(uint32_t)counters;u[25]=(uint32_t)(counters>>32)|(4u<<16);u[26]=128;u[27]=0x16204;
    for(unsigned j=0;j<2;j++){uint32_t *d=u+(j?16:0);guest_addr dest=j?out2:out;d[0]=(uint32_t)dest;d[1]=(uint32_t)(dest>>32)|(16u<<16);d[2]=128;d[3]=0x16204;}
    for(unsigned k=0;k<8;k++){
        if(k==7){n=read_program(code);CHECK(gmem_write(&m,a+0x400,code,n*4));g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x400)>>8);}
        unsigned shift=k<4?(unsigned[]){0,1,3,6}[k]:3;
        uint64_t mask=k==4?0xffffffff00000000ull:k==5?0x8000000180000001ull:k==6?0:UINT64_MAX;
        u[20]=(uint32_t)mask;u[21]=(uint32_t)(mask>>32);u[22]=shift;
        memset(p,0xcd,2048);memset(q,0xcd,2048);memset(count,0,512);
        renderer_submit_begin(r);renderer_dispatch(r,&g,2,1,1);renderer_sync_mem(r,out,2048);renderer_sync_mem(r,out2,2048);renderer_sync_mem(r,counters,512);
        unsigned iterations=0;bool keys[64]={0};for(unsigned lane=0;lane<64;lane++)if((mask>>lane)&1)keys[lane>>shift]=true;
        for(unsigned i=0;i<64;i++)iterations+=keys[i];
        for(unsigned group=0;group<2;group++)for(unsigned lane=0;lane<64;lane++){
            unsigned at=4*(group*64+lane);
            if(!((mask>>lane)&1)){CHECK(p[at]==0xcdcdcdcd && q[at]==0xcdcdcdcd);continue;}
            unsigned first=0,rank=0,key=lane>>shift;while(first<64 && (!((mask>>first)&1) || (first>>shift)!=key))first++;
            for(unsigned i=0;i<key;i++)rank+=keys[i];
            if(k<7 && (p[at]!=key || p[at+1]!=first || p[at+2]!=rank || p[at+3]!=lane))
                fprintf(stderr,"case %u group %u lane %u got %u,%u,%u,%u expected %u,%u,%u,%u\n",k,group,lane,p[at],p[at+1],p[at+2],p[at+3],key,first,rank,lane);
            if(k<7)CHECK(p[at]==key && p[at+1]==first && p[at+2]==rank && p[at+3]==lane);
            else CHECK(p[at]==0xcdcdcdcd);
            CHECK(q[at]==163 && q[at+1]==131 && q[at+2]==(k<7?iterations:0) && q[at+3]==lane);
        }
        for(unsigned group=0;group<2;group++)for(unsigned key=0;key<64;key++){
            uint32_t expected=0;
            for(unsigned lane=0;lane<64 && k<7;lane++)if(((mask>>lane)&1) && (lane>>shift)==key)expected|=1u<<(lane&31);
            if(count[group*64+key]!=expected)fprintf(stderr,"atomic case %u group %u key %u got %08x expected %08x\n",k,group,key,count[group*64+key],expected);
            CHECK(count[group*64+key]==expected);
        }
    }
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(w);gmem_free(&m);pal_shutdown();
    puts("wave64 waterfall: 8 cases, 2 independent groups, all 64 lanes, partial EXEC and readlane31/63 verified");return 0;
}
/* One selected lane acquires work from a global counter, then READLANE must
 * broadcast that result to both native32 halves. LDS is used in the same
 * wave without S_BARRIER, as permitted by the guest's lockstep execution.
 * Straight-line synthetic code deliberately avoids an unbounded GPU hang. */
static int lds_atomic_readlane_test(void) {
    CHECK(pal_init()==PS5C_OK);
    gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr a;CHECK(gmem_map(&m,0,2*GUEST_PAGE_SIZE,GPROT_RW,0,"LDS atomic broadcast",&a)==PS5C_OK);
    pal_window *w=NULL;CHECK(pal_window_open("ZeraPS5 LDS broadcast regression",320,180,&w)==PS5C_OK);
    renderer *r=renderer_create(w,&m);CHECK(r);
    gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    const uint32_t code[]={
        0x34020082,                         /* v1 = tid * 4 */
        0x4a0400ff,100,                     /* v2 = tid + 100 */
        0xd8340000,0x00000201,              /* LDS[tid] = v2 */
        0xbf8cc07f,
        0x7e06020c,                         /* v3 = group X (s12) */
        0xbefe0404,                         /* EXEC = s[4:5] (one chosen lane) */
        0x7e0402c0,                         /* v2 = 64 */
        0xe0c86000,0x80020203,              /* atomic_add v2, v3, s[8:11], glc idxen */
        0xbefe04c1,0xbf8c3f70,              /* restore EXEC, wait for the atomic result */
        0xd760000e,0x00000d02,              /* s14 = readlane(v2, s6) */
        0x7e08020e,                         /* v4 = s14 */
        0x3a0200a0,0x34020282,              /* v1 = (tid xor 32) * 4 */
        0xd8d80000,0x05000001,0xbf8cc07f,   /* v5 = LDS[partner] */
        0x7e0c0300,0x7e0e020c,              /* v6 = tid, v7 = group X */
        0x7e10020c,0x34101086,0x4a101100,   /* v8 = group X * 64 + tid */
        0xe0782000,0x80000408,0xbf810000
    };
    CHECK(gmem_write(&m,a,code,sizeof(code)));
    uint32_t literal_code[PS5C_ARRAY_COUNT(code)];memcpy(literal_code,code,sizeof(code));
    literal_code[7]=0xbefe0481; /* EXEC = literal 1, not an input SGPR mask */
    CHECK(gmem_write(&m,a+0x200,literal_code,sizeof(literal_code)));
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    guest_addr dest=a+0x4000,counters=a+0x6000;
    uint32_t *out=gmem_host(&m,dest,128*16+8),*count=gmem_host(&m,counters,12);CHECK(out && count);
    u[0]=(uint32_t)dest;u[1]=(uint32_t)(dest>>32)|(16u<<16);u[2]=128;u[3]=0x16204;
    u[8]=(uint32_t)counters;u[9]=(uint32_t)(counters>>32)|(4u<<16);u[10]=2;u[11]=0x16204;
    g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(a>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(a>>40);
    g.regs[mmCOMPUTE_NUM_THREAD_X]=64;g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    g.regs[mmCOMPUTE_PGM_RSRC2]=(12u<<1)|(1u<<7)|(1u<<15);
    g.regs[mmCOMPUTE_DISPATCH_INITIATOR]=0x41;
    bool all_correct=true;
    for(unsigned pass=0;pass<5;pass++) {
        unsigned lane=(unsigned[]){0,31,32,63,0}[pass];uint64_t mask=1ull<<lane;
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+(pass==4?0x200:0))>>8);
        u[4]=(uint32_t)mask;u[5]=(uint32_t)(mask>>32);u[6]=lane;
        for(unsigned i=0;i<128*4+2;i++)out[i]=0xdeadbeef;
        count[0]=1000;count[1]=2000;count[2]=0xdeadbeef;
        renderer_submit_begin(r);renderer_dispatch(r,&g,2,1,1);
        renderer_sync_mem(r,dest,128*16+8);renderer_sync_mem(r,counters,12);
        bool correct=true;
        for(unsigned i=0;i<128;i++) {
            unsigned group=i/64,tid=i%64;
            uint32_t expected[4]={1000u*(group+1),(tid^32u)+100u,tid,group};
            for(unsigned k=0;k<4;k++)if(out[4*i+k]!=expected[k]) {
                if(correct)fprintf(stderr,"LDS readlane%u group%u tid%u component%u: %u expected %u\n",
                    lane,group,tid,k,out[4*i+k],expected[k]);
                correct=false;
            }
        }
        correct=correct && count[0]==1064 && count[1]==2064 && count[2]==0xdeadbeef &&
            out[128*4]==0xdeadbeef && out[128*4+1]==0xdeadbeef;
        if(!correct) {
            fprintf(stderr,"atomic counters: %u,%u (expected 1064,2064), guard %08x\n",count[0],count[1],count[2]);
            all_correct=false;break;
        }
    }
    /* Guest LDS can fit exactly while the translator's cross-half scratch
     * would exceed the device limit. Reject before creating that pipeline. */
    uint32_t shared_limit=r->props.limits.maxComputeSharedMemorySize;
    if(all_correct && shared_limit<=65536 && shared_limit%512==0) {
        g.regs[mmCOMPUTE_PGM_RSRC2]=(12u<<1)|(1u<<7)|((shared_limit/512)<<15);
        for(unsigned i=0;i<128*4+2;i++)out[i]=0xdeadbeef;
        count[0]=1000;count[1]=2000;count[2]=0xdeadbeef;
        renderer_submit_begin(r);renderer_dispatch(r,&g,2,1,1);
        renderer_sync_mem(r,dest,128*16+8);renderer_sync_mem(r,counters,12);
        for(unsigned i=0;i<128*4+2;i++)all_correct &= out[i]==0xdeadbeef;
        all_correct &= count[0]==1000 && count[1]==2000 && count[2]==0xdeadbeef;
        if(!all_correct)fprintf(stderr,"LDS scratch limit: rejected dispatch modified its buffers\n");
    }
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(w);gmem_free(&m);pal_shutdown();
    if(all_correct)puts("LDS atomic READLANE: SGPR/literal EXEC, lanes 0/31/32/63, two groups, both halves, guards and shared-memory limit verified");
    return all_correct?0:1;
}
int main(void){
    CHECK(cpu_test()==0);
    if(!getenv("PS5C_TEST_GPU"))return 0;
    CHECK(gpu_test()==0);
    return lds_atomic_readlane_test();
}
