/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/shader/shader.h"
#include "gpu/gpu_regs.h"
#include "gpu/renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)

static uint32_t values[2]={0x12345678,0x87654321};
static const uint32_t leaf[]={0xbe941f00,0xf400058a,0xfa000000,0xf40005c8,0xfa000000,0xbefd210e};
static const guest_addr fn=0x11d039d400ull, data=0x1123450000ull;
static void *read_data(void *u,guest_addr a,uint64_t n) {
    (void)u;
    if(a>=fn && a-fn<=sizeof(leaf) && n<=sizeof(leaf)-(a-fn))return (char*)leaf+(a-fn);
    if(a>=data && a-data<=sizeof(values) && n<=sizeof(values)-(a-data))return (char*)values+(a-data);
    return NULL;
}

static int cpu_test(void) {
    /* Overlapping target/link pair: target must be read BEFORE writing the
     * return address. Unresolved calls must never execute as scalar no-ops. */
    const uint32_t code[]={0xbe8e210e,0xbf810000};
    sh_program *p=calloc(1,sizeof(*p));
    sh_eval *e=calloc(1,sizeof(*e));
    CHECK(p && e);
    p->back_branch=true;
    CHECK(sh_analyze(code,2,0x900000000ull,p) && !p->back_branch);
    e->sgpr_init[14]=0x12345000;e->sgpr_init[15]=0x11;
    CHECK(!sh_evaluate(p,NULL,e));
    CHECK(e->call_pending && e->call_known && e->call_inst==0);
    CHECK(e->call_target==0x1112345000ull);
    /* Group 0 is only a representative: its target cannot be frozen for
     * every workgroup of a dispatch. */
    e->group_sgpr=14;e->group_sgprs=1;
    CHECK(!sh_evaluate(p,NULL,e));
    CHECK(e->call_pending && !e->call_known && !e->call_target);
    const uint32_t end[]={0xbf810000};
    CHECK(sh_analyze(end,1,0,p));
    CHECK(sh_evaluate(p,NULL,e));
    CHECK(!e->call_pending && !e->call_known && !e->call_target);
    const uint32_t twice[]={0xbe900308,0xbe910309,0xbe8e0400,0xbe8e210e,
        0xbe90030a,0xbe91030b,0xbe8e0400,0xbe8e210e,0xbf810000};
    CHECK(sh_analyze(twice,9,0x900000000ull,p));
    e->group_sgprs=0;e->sgpr_init[0]=(uint32_t)fn;e->sgpr_init[1]=(uint32_t)(fn>>32);
    e->sgpr_init[8]=(uint32_t)data;e->sgpr_init[9]=(uint32_t)(data>>32);
    e->sgpr_init[10]=(uint32_t)data+4;e->sgpr_init[11]=(uint32_t)(data>>32);
    e->host=read_data;
    CHECK(!sh_evaluate(p,NULL,e) && e->call_inst==3 && e->call_target==fn);
    CHECK(sh_append_function(p,leaf,6,fn,3));
    CHECK(!sh_evaluate(p,NULL,e) && e->call_inst==7 && e->call_target==fn);
    CHECK(sh_append_function(p,leaf,6,fn,7));
    CHECK(p->function_count==3 && p->site_count==4 && sh_evaluate(p,NULL,e));
    CHECK(e->smem_data[e->site[0].smem_offset]==leaf[1]); /* original GETPC address */
    CHECK(e->smem_data[e->site[1].smem_offset]==values[0]);
    CHECK(e->smem_data[e->site[2].smem_offset]==leaf[1]);
    CHECK(e->smem_data[e->site[3].smem_offset]==values[1]); /* independent argument/site */
    uint64_t hash=p->hash;
    CHECK(!sh_append_function(p,leaf,6,fn,7) && p->hash==hash);
    CHECK(!sh_append_function(p,leaf,5,fn+0x100,7) && p->hash==hash); /* missing return */
    const uint32_t escape[]={0xbf82007f,0xbefd210e};
    CHECK(!sh_append_function(p,escape,2,fn+0x100,7) && p->hash==hash);
    sh_stage_info info={.stage=SH_STAGE_CS,.local_size={1,1,1}};
    sh_site_shape *shapes=calloc(SH_MAX_SITES,sizeof(*shapes));
    uint32_t *spv=NULL;size_t words=0;
    CHECK(shapes && sh_translate(p,&info,shapes,0,&spv,&words) && words>5);
    free(spv);
    p->wave32=true;info.local_size[0]=32;info.cs_wave_mode=2;info.mesh_indexed=SH_LEAF_CALL_INLINE;
    CHECK(sh_translate(p,&info,shapes,0,&spv,&words) && words>5);
    free(spv);free(shapes);
    /* A nested call saves/restores the outer return pair in ordinary SGPRs.
     * Each context still owns its own SMEM values. */
    const uint32_t outer[]={0xbe98040e,0xbe8e0402,0xbe8e210e,0xbe8e0418,0xbefd210e};
    const uint32_t inner[]={0xf4000588,0xfa000000,0xbefd210e};
    CHECK(sh_analyze(twice,9,0x900000000ull,p));
    e->sgpr_init[2]=(uint32_t)(fn+0x100);e->sgpr_init[3]=(uint32_t)((fn+0x100)>>32);
    for(unsigned n=0;n<4;n++) {
        CHECK(!sh_evaluate(p,NULL,e) && e->call_pending && e->call_known);
        CHECK(sh_append_function(p,e->call_target==fn?outer:inner,
            e->call_target==fn?5:3,e->call_target,e->call_inst));
    }
    CHECK(sh_evaluate(p,NULL,e) && p->function_count==5 && p->site_count==2);
    CHECK(e->smem_data[e->site[0].smem_offset]==values[0]);
    CHECK(e->smem_data[e->site[1].smem_offset]==values[1]);
    CHECK(!sh_append_function(p,inner,3,fn+0x200,UINT32_MAX));
    /* Clobbered return links refuse execution instead of silently dropping
     * the call or turning the target into a host pointer. */
    const uint32_t bad_return[]={0xbe8e0480,0xbefd210e};
    CHECK(sh_analyze(twice,9,0x900000000ull,p));
    CHECK(sh_append_function(p,bad_return,2,fn,3));
    CHECK(!sh_evaluate(p,NULL,e) && e->call_pending);
    sh_eval_free(e);free(e);free(p);
    return 0;
}

static int gpu_test(void) {
    CHECK(pal_init()==PS5C_OK);
    gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr a;CHECK(gmem_map(&m,0,0x20000,GPROT_RW,0,"call fixture",&a)==PS5C_OK);
    const uint32_t body[]={0xf4000588,0xfa000000, /* s22 = *s16 */
        0x7e020216, /* v1 = s22 */
        0xd8360000,0x00000100, /* GDS[v0] = v1 */
        0xbefd210e};
    const uint32_t main_code[]={0x7e000280,0xbe900308,0xbe910309,0xbe8e0400,0xbe8e210e,
        0x7e000284,0xbe90030a,0xbe91030b,0xbe8e0400,0xbe8e210e,0xbf810000};
    CHECK(gmem_write(&m,a,main_code,sizeof(main_code)) && gmem_write(&m,a+0x100,body,sizeof(body)));
    const uint32_t nested[]={0xbe98040e,0xbe8e0402,0xbe8e210e,0xbe8e0418,0xbefd210e};
    CHECK(gmem_write(&m,a+0x300,nested,sizeof(nested)));
    CHECK(gmem_write(&m,a+0x200,values,sizeof(values)));
    sh_gds_base=a+0x10000;
    uint32_t *out=gmem_host(&m,sh_gds_base,SH_GDS_SIZE);CHECK(out);memset(out,0,SH_GDS_SIZE);
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 scalar calls regression",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    if(getenv("PS5C_SHADER_LEAF_INLINE")) {
        g.regs[mmCOMPUTE_NUM_THREAD_X]=32;
        g.regs[mmCOMPUTE_DISPATCH_INITIATOR]=1|(1u<<15);
    }
    g.regs[mmCOMPUTE_PGM_RSRC2]=16u<<1;
    g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(a>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(a>>40);
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    u[0]=(uint32_t)(a+0x100);u[1]=(uint32_t)((a+0x100)>>32);
    u[8]=(uint32_t)(a+0x200);u[9]=(uint32_t)((a+0x200)>>32);
    u[10]=(uint32_t)(a+0x204);u[11]=(uint32_t)((a+0x204)>>32);
    for(unsigned pass=0;pass<2;pass++) {
        guest_addr entry=a+(pass?0x300:0x100);
        u[0]=(uint32_t)entry;u[1]=(uint32_t)(entry>>32);
        u[2]=(uint32_t)(a+0x100);u[3]=(uint32_t)((a+0x100)>>32);
        out[0]=out[1]=0;
        renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);
        renderer_sync_mem(r,sh_gds_base,SH_GDS_SIZE);
        fprintf(stderr,"GPU call results (nested=%u): %08x %08x\n",pass,out[0],out[1]);
        CHECK(out[0]==values[0] && out[1]==values[1]);
        const uint32_t changed[]={0x31415926,0x27182818};
        CHECK(gmem_write(&m,a+0x200,changed,sizeof(changed)));out[0]=out[1]=0;
        renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,sh_gds_base,SH_GDS_SIZE);
        CHECK(out[0]==changed[0] && out[1]==changed[1]);
        CHECK(gmem_write(&m,a+0x200,values,sizeof(values)));
    }
    if(getenv("PS5C_CS_WAVE_FROM_PACKET")) {
        /* A scalar VCC_HI temporary in a callee does not change the physical
         * wave mode. The packet requests wave64; only the high VCC word is
         * nonzero, so VCCZ must be false even after function linking. */
        const uint32_t caller[]={0x7e000280,0xbe8e0400,0xbe8e210e,0xbf810000};
        const uint32_t callee[]={0xbe960380,0xbeeb0381,0xbeea0414,0xbf860001,
            0xbe960381,0x7e020216,0xd8360000,0x00000100,0xbefd210e};
        guest_addr pc=a+0x800,entry=a+0x900;
        CHECK(gmem_write(&m,pc,caller,sizeof(caller)) && gmem_write(&m,entry,callee,sizeof(callee)));
        u[2]=(uint32_t)entry;u[3]=(uint32_t)(entry>>32);u[20]=0;u[21]=1;
        g.regs[mmCOMPUTE_PGM_RSRC2]=24u<<1;
        g.regs[mmCOMPUTE_NUM_THREAD_X]=64; /* complete native waves, not singleton fallback */
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
        for(unsigned nested_call=0;nested_call<2;nested_call++)for(unsigned wave32=0;wave32<2;wave32++) {
            guest_addr target=nested_call?a+0x300:entry;
            u[0]=(uint32_t)target;u[1]=(uint32_t)(target>>32);
            g.regs[mmCOMPUTE_DISPATCH_INITIATOR]=1|(wave32<<15);
            out[0]=0xdeadbeef;
            renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,sh_gds_base,SH_GDS_SIZE);
            fprintf(stderr,"GPU authoritative wave%u call result (nested=%u): %u\n",wave32?32:64,nested_call,out[0]);
            CHECK(out[0]==(wave32?0u:1u));
        }
    }
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&m);pal_shutdown();
    return 0;
}

int main(void) {int rc=cpu_test();if(rc)return rc;return getenv("PS5C_TEST_GPU")?gpu_test():0;}
