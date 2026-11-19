/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "base/base.h"
#include "gpu/shader/shader.h"
#include "gpu/shader/shader_ops.h"
#include "gpu/shader/shader_aperture.h"
#include "gpu/gpu_regs.h"
#include "gpu/renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static void enable(const char *v){
#ifdef _WIN32
    _putenv_s("PS5C_APERTURE_SOURCES",v); ps5c_env_reset();
#else
    setenv("PS5C_APERTURE_SOURCES",v,1); ps5c_env_reset();
#endif
}
static void flat_enable(const char *v){
#ifdef _WIN32
    _putenv_s("PS5C_FLAT_APERTURES",v); ps5c_env_reset();
#else
    setenv("PS5C_FLAT_APERTURES",v,1); ps5c_env_reset();
#endif
}
static unsigned flat_code(uint32_t *c){
    unsigned n=0;
#define E(x) c[n++]=(x)
#define MOV(v,s) E(0x7e000200u|((v)<<17)|(s))
#define FLAT(seg,op,vaddr,vdata,dst,offset) do { E(0xdc000000u|((seg)<<14)|((op)<<18)|(offset)); E(((dst)<<24)|0x007d0000u|((vdata)<<8)|(vaddr)); } while(0)
    E(0xbe8204eb); /* s[2:3] = shared base */
    E(0xbe8404ed); /* s[4:5] = private base */
    E(0x34020082); /* v1 = tid * 4 */
    MOV(2,3); /* high shared */
    E(0x4a0600ff);E(100); /* v3 = tid + 100 */
    FLAT(0,0x1c,1,3,0,0); /* LDS[tid] = tid + 100 */
    E(0xbf8a0000); /* Workgroup barrier, then read another lane's slot */
    E(0x3a140081); /* v10 = tid ^ 1 */
    E(0x34141482); /* v10 <<= 2 */
    E(0xd8d80000);E(0x0400000a); /* ds_read_b32 v4, v10 */
    E(0xbf8cc07f);
    MOV(1,192);MOV(2,5); /* private address64 */
    FLAT(0,0x1c,1,3,0,16); /* private[80] = tid + 100 */
    FLAT(1,0xc,1,0,5,16); /* explicit scratch aliases it */
    E(0x4a0600ff);E(200);
    FLAT(1,0x1c,1,3,0,16);
    FLAT(0,0xc,1,0,6,16); /* generic private reads explicit scratch */
    MOV(10,8);MOV(11,9); /* actual global address from user data */
    FLAT(0,0xc,10,0,7,0);
    FLAT(0,0x1e,1,4,0,32); /* private dwordx4 round trip */
    MOV(4,128);MOV(5,128);MOV(6,128);MOV(7,128);
    FLAT(0,0xe,1,0,4,32);
    FLAT(0,0x1f,1,4,0,48); /* and dwordx3 */
    MOV(4,128);MOV(5,128);MOV(6,128);
    FLAT(0,0xf,1,0,4,48);
    E(0xe0782000);E(0x80030400); /* buffer_store_dwordx4 v[4:7], v0, s[12:15] */
    MOV(10,255);E(512);MOV(11,3);
    FLAT(0,0xc,10,0,8,0); /* beyond allocated LDS, must not alias last dword */
    MOV(10,255);E(4096);MOV(11,5);
    FLAT(0,0xc,10,0,9,0); /* beyond private allocation */
    MOV(10,132);MOV(11,3);
    FLAT(0,0xd,10,0,10,0); /* address VGPRs overlap the two load destinations */
    E(0xe0782010);E(0x80030800);
    E(0xbf810000);
#undef FLAT
#undef MOV
#undef E
    return n;
}
static void code_for(uint32_t code[9],unsigned source){
    const uint32_t c[]={0xbe800300,0xbe820400,0x7e000200,0x7e020202,0x7e040203,
        0x7e060280,0xe0780000,0x80030000,0xbf810000};
    memcpy(code,c,sizeof(c));code[0]|=source;code[1]|=source;
}
static int cpu_test(void){
    sh_program *p=calloc(1,sizeof(*p));sh_eval *e=calloc(1,sizeof(*e));
    sh_site_shape *s=calloc(SH_MAX_SITES,sizeof(*s));CHECK(p && e && s);
    e->vsrc=calloc(SH_MAX_INSTS,sizeof(*e->vsrc));e->vsrc_state=calloc(SH_MAX_INSTS,sizeof(*e->vsrc_state));
    CHECK(e->vsrc && e->vsrc_state);
    for(unsigned operand=235;operand<=238;operand++){
        uint32_t code[9];code_for(code,operand);CHECK(sh_analyze(code,9,0,p));
        uint64_t expected;CHECK(sh_aperture_value(operand,&expected));
        enable("0");uint32_t *spv=NULL;size_t n=0;
        CHECK(!sh_evaluate(p,NULL,e) && !sh_translate(p,&(sh_stage_info){.stage=SH_STAGE_CS,.local_size={1,1,1}},s,0,&spv,&n));
        enable("1");CHECK(sh_evaluate(p,NULL,e));
        CHECK(e->vsrc_state[2][0]==1 && e->vsrc[2][0]==(uint32_t)expected);
        CHECK(e->vsrc_state[3][0]==1 && e->vsrc[3][0]==(uint32_t)expected);
        CHECK(e->vsrc_state[4][0]==1 && e->vsrc[4][0]==(uint32_t)(expected>>32));
        CHECK(sh_translate(p,&(sh_stage_info){.stage=SH_STAGE_CS,.local_size={1,1,1}},s,0,&spv,&n));free(spv);
        /* Never enable unrouted flat accesses merely by enabling constants. */
        p->insts[2].enc=RDNA2_ENC_FLAT;p->insts[2].seg=0;sh_program_flags(p);
        CHECK(!sh_aperture_allowed(p));
        uint32_t generic[11];memcpy(generic,code,8*4);
        generic[8]=0xdc200000;generic[9]=0x007d0001;generic[10]=0xbf810000;
        CHECK(sh_analyze(generic,11,0,p));
        CHECK((p->aperture_flags&5)==5 && !sh_aperture_allowed(p));
    }
    uint32_t flat[128];unsigned size=flat_code(flat);
    CHECK(sh_analyze(flat,size,0,p));CHECK((p->aperture_flags&5)==5);
    flat_enable("0");CHECK(!sh_aperture_allowed(p));
    flat_enable("1");CHECK(sh_aperture_allowed(p));
    uint32_t *spv=NULL;size_t words=0;
    sh_stage_info info={.stage=SH_STAGE_CS,.local_size={32,1,1},.lds_dwords=32};
    CHECK(sh_translate(p,&info,s,0,&spv,&words));free(spv);
    info.stage=SH_STAGE_VS;CHECK(!sh_translate(p,&info,s,0,&spv,&words));
    for(uint32_t i=0;i<p->count;i++)if(p->insts[i].enc==RDNA2_ENC_FLAT && p->insts[i].seg==0){
        uint16_t old=p->ops[i];p->ops[i]=SH_OP_GLOBAL_ATOMIC;
        CHECK(!sh_aperture_allowed(p));p->ops[i]=old;break;
    }
    for(uint32_t i=0;i<p->count;i++)if(p->insts[i].enc==RDNA2_ENC_FLAT && p->insts[i].seg==0){
        const char *old=p->insts[i].name;p->insts[i].name="global_load_dword_addtid";
        CHECK(!sh_aperture_allowed(p));p->insts[i].name=old;break;
    }
    enable("0");CHECK(!sh_aperture_allowed(p));enable("1");
    free(s);free(e->vsrc);free(e->vsrc_state);sh_eval_free(e);free(e);free(p);return 0;
}
static int gpu_test(void){
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,0x10000,GPROT_RW,0,"aperture query fixture",&a)==PS5C_OK);
    pal_window *w=NULL;CHECK(pal_window_open("ZeraPS5 aperture query regression",320,180,&w)==PS5C_OK);
    renderer *r=renderer_create(w,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    g.regs[mmCOMPUTE_PGM_RSRC2]=16u<<1;
    guest_addr out_addr=a+0x4000;uint32_t *out=gmem_host(&m,out_addr,16);CHECK(out);
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    u[12]=(uint32_t)out_addr;u[13]=(uint32_t)(out_addr>>32);u[14]=16;
    for(unsigned operand=235;operand<=238;operand++){
        uint32_t code[9];code_for(code,operand);guest_addr addr=a+(operand-235)*0x100;
        CHECK(gmem_write(&m,addr,code,sizeof(code)));memset(out,0x55,16);
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(addr>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(addr>>40);
        renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out_addr,16);
        uint64_t expected;CHECK(sh_aperture_value(operand,&expected));
        fprintf(stderr,"aperture %u: 32=%08x 64=%08x_%08x\n",operand,out[0],out[2],out[1]);
        CHECK(out[0]==(uint32_t)expected && out[1]==(uint32_t)expected && out[2]==(uint32_t)(expected>>32));
    }
    uint32_t flat[128];unsigned size=flat_code(flat);
    guest_addr addr=a+0x1000,input=a+0x6000;
    CHECK(gmem_write(&m,addr,flat,size*4));
    uint32_t expected=0x12345678;CHECK(gmem_write(&m,input,&expected,4));
    out=gmem_host(&m,out_addr,32*32);CHECK(out);memset(out,0x55,32*32);
    u[8]=(uint32_t)input;u[9]=(uint32_t)(input>>32);
    u[13]=(uint32_t)(out_addr>>32)|(32u<<16);u[14]=32;
    g.regs[mmCOMPUTE_NUM_THREAD_X]=32;
    g.regs[mmCOMPUTE_PGM_RSRC2]=(16u<<1)|(1u<<15); /* 512 bytes LDS */
    g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(addr>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(addr>>40);
    if(getenv("PS5C_TEST_FLAT_DISABLED"))flat_enable("0");
    renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out_addr,32*32);
    for(unsigned lane=0;lane<32;lane++){
        unsigned k=lane*8;
        fprintf(stderr,"flat lane%u: %u %u %u %08x; %u %u %u %u\n",lane,out[k],out[k+1],out[k+2],out[k+3],out[k+4],out[k+5],out[k+6],out[k+7]);
        CHECK(out[k]==(lane^1)+100 && out[k+1]==lane+100 && out[k+2]==lane+200 && out[k+3]==expected);
        CHECK(out[k+4]==0 && out[k+5]==0 && out[k+6]==101 && out[k+7]==102);
    }
    /* One invocation, real generic global store to the same binding read by FLAT. */
    const uint32_t write_global[]={0xbe8204eb,0x7e140208,0x7e160209,0x7e1802ff,0xabcdef01,
        0xdc700000,0x007d0c0a,0xbf810000};
    addr=a+0x1800;CHECK(gmem_write(&m,addr,write_global,sizeof(write_global)));
    g.regs[mmCOMPUTE_NUM_THREAD_X]=1;
    g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(addr>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(addr>>40);
    renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,input,4);
    CHECK(gmem_read(&m,input,&expected,4) && expected==0xabcdef01);
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(w);gmem_free(&m);pal_shutdown();return 0;
}
int main(void){int rc=cpu_test();if(rc)return rc;return getenv("PS5C_TEST_GPU")?gpu_test():0;}
