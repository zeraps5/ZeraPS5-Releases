/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Own synthetic shaders: 128 overlapping fragments perform a non-atomic RMW.
 * The final count must retain every update through the native pixel interlock. */
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
static const uint32_t ps[]={
    0x8104ef80, /* s_add_i32 s4, 0, pops_exiting_wave_id */
    0x7e040280,0x7e060280, /* coordinates v2:3 = 0 */
    0xf0000108,0x00000402, /* image_load v4 */
    0x4a080881, /* v_add_nc_u32 v4, 1, v4 */
    0xf0200108,0x00000402, /* image_store v4 */
    0x7e0002f2,0x7e020280,0x7e040280,0x7e0602f2,
    0xf800100f,0x03020100,0xf800181f,0x03020100,0xbf810000
};
static int cpu_test(void){
    sh_program *p=calloc(1,sizeof(*p));CHECK(p && sh_analyze(ps,sizeof(ps)/4,0,p));
    CHECK(p->insts[0].src[1]==239);
    CHECK(p->site_count==2 && p->sites[0].pops_load && !p->sites[0].store);
    sh_stage_info info={.stage=SH_STAGE_PS};
    sh_site_shape shapes[SH_MAX_SITES]={0};
    for(unsigned i=0;i<p->site_count;i++) shapes[i].img_type=9,shapes[i].img_kind=1;
    shapes[0].ordered_load=1;
    sh_eval *eval=calloc(1,sizeof(*eval));CHECK(eval);
    eval->site[0].valid=eval->site[1].valid=true;
    eval->site[0].desc[0]=eval->site[1].desc[0]=0x12345;
    sh_site_shape shape;sh_site_shape_of(p,eval,0,&shape);CHECK(shape.ordered_load);
    eval->site[1].desc[0]++;sh_site_shape_of(p,eval,0,&shape);CHECK(!shape.ordered_load);
    eval->site[1].desc[0]--;eval->site[1].desc_unknown=true;
    sh_site_shape_of(p,eval,0,&shape);CHECK(!shape.ordered_load);
    free(eval);
    uint32_t *spv=NULL;size_t words=0;
    sh_set_pixel_interlock(false);CHECK(!sh_translate(p,&info,shapes,0,&spv,&words));
    sh_set_pixel_interlock(true);CHECK(sh_translate(p,&info,shapes,0,&spv,&words));
    unsigned begin=0,end=0,ordered=0;size_t end_at=0;
    for(size_t i=5;i<words;i+=spv[i]>>16){
        CHECK(spv[i]>>16);unsigned op=spv[i]&65535;
        begin+=op==SpvOpBeginInvocationInterlockEXT;
        if(op==SpvOpEndInvocationInterlockEXT) end++,end_at=i;
        if(op==SpvOpExecutionMode && spv[i+2]==SpvExecutionModePixelInterlockOrderedEXT) ordered++;
        if(op==SpvOpKill || op==SpvOpReturn) CHECK(end_at && end_at<i);
    }
    CHECK(begin==1 && end==1 && ordered==1);
    if(getenv("PS5C_TEST_SPIRV")){
        char path[512];snprintf(path,sizeof(path),"%s/pixel-interlock.spv",getenv("PS5C_TEST_SPIRV"));
        FILE *f=fopen(path,"wb");CHECK(f && fwrite(spv,4,words,f)==words && !fclose(f));
    }
    vk_shader_cache_device(false,false,false,false);uint64_t key=vk_shader_cache_key(p,&info,shapes,0);
    vk_shader_cache_device(false,false,false,true);CHECK(key!=vk_shader_cache_key(p,&info,shapes,0));
    free(spv);
    info.stage=SH_STAGE_CS;CHECK(!sh_translate(p,&info,shapes,0,&spv,&words));
    /* A different descriptor is an independent texture, not the RMW UAV. */
    uint32_t independent[sizeof(ps)/4];memcpy(independent,ps,sizeof(ps));independent[4]|=0x00020000;
    CHECK(sh_analyze(independent,sizeof(independent)/4,0,p));
    CHECK(p->sites[0].pops_load && !p->sites[0].store && p->sites[1].store);
    sh_set_pixel_interlock(false);free(p);
    puts("POPS: feature gate, ordered region, end before discard/return and cache separation verified");return 0;
}
static void desc(uint32_t *u,guest_addr a){
    memset(u,0,8*4);u[0]=(uint32_t)(a>>8);u[1]=(uint32_t)(a>>40)|(20u<<20);u[3]=(9u<<28)|0xfac;
}
static int gpu_test(void){
    static const uint32_t vs[]={0x36000a81,0x2c020a81,0x7e000d00,0x7e020d01,
        0x100000ff,0x42800000,0x100202ff,0x42800000,0x060000ff,0xc1000000,
        0x060202ff,0xc1000000,0x7e0402ff,0x3e800000,0x7e0602f2,0xf80008cf,0x03020100,0xbf810000};
    static const uint32_t load[]={0x7e040280,0x7e060280,0xf0000108,0x00000402,
        0xe0780000,0x80030400,0xbf810000};
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"pixel interlock",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,vs,sizeof(vs)) && gmem_write(&m,a+0x100,ps,sizeof(ps)) && gmem_write(&m,a+0x200,load,sizeof(load)));
    pal_window *w=NULL;CHECK(pal_window_open("ZeraPS5 ordered pixel regression",160,90,&w)==PS5C_OK);
    renderer *r=renderer_create(w,&m);CHECK(r);
    cleanup_renderer=r;cleanup_window=w;
    if(!r->pixel_interlock){puts("SKIP GPU: native pixel interlock disabled or unsupported");renderer_destroy(r);pal_window_destroy(w);gmem_free(&m);pal_shutdown();return 0;}
    gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    guest_addr color=a+0x4000,counter=a+0x8000,out=a+0xc000;
    g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(a>>8);g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(a>>40);
    g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x100)>>8);g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)((a+0x100)>>40);
    g.regs[mmSPI_SHADER_PGM_RSRC2_PS]=8u<<1;
    g.regs[mmCB_COLOR_CONTROL]=0x10;g.regs[mmCB_TARGET_MASK]=0x2f;
    g.regs[mmCB_SHADER_MASK]=0x1f;
    g.regs[mmCB_COLOR0_BASE]=(uint32_t)(color>>8);g.regs[mmCB_COLOR0_INFO]=10u<<2;
    /* MRT1 exports R to an R32 resource, while its target mask enables only
     * the absent G channel. It must not become an attachment of the UAV. */
    g.regs[mmCB_COLOR0_BASE+GPU_CB_STRIDE]=(uint32_t)(counter>>8);
    g.regs[mmCB_COLOR0_INFO+GPU_CB_STRIDE]=(4u<<2)|(4u<<8);
    g.regs[mmSPI_SHADER_COL_FORMAT]=0x99;g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_TRILIST;
    desc(g.regs+mmSPI_SHADER_USER_DATA_PS_0,counter);
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;desc(u,counter);
    u[12]=(uint32_t)out;u[13]=(uint32_t)(out>>32)|(16u<<16);u[14]=1;u[15]=0x16204;
    g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x200)>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)((a+0x200)>>40);
    g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    g.regs[mmCOMPUTE_PGM_RSRC2]=16u<<1;
    gpu_draw draw={.count=3,.instances=64};
    for(unsigned round=0;round<3;round++){
        renderer_submit_begin(r);vk_begin(r);
        vk_image *img=vk_image_get(r,counter,1,1,VK_FORMAT_R32_UINT);CHECK(img);
        vk_barrier(r->cmd,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue cv={.uint32={0}};VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(r->cmd,img->image,img->layout,&cv,1,&range);img->gpu_written=true;img->written_seq=++r->write_seq;
        renderer_draw(r,&g,&draw);renderer_draw(r,&g,&draw);
        renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,16);
        uint32_t *p=gmem_host(&m,out,16);CHECK(p);
        fprintf(stderr,"pixel interlock round %u: %u / 128 updates retained\n",round,p[0]);CHECK(p[0]==128);
    }
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(w);gmem_free(&m);pal_shutdown();
    puts("GPU: 3 pairs of consecutive draws, 128 overlapping fragments per pair, non-atomic image RMW verified");return 0;
}
int main(void){CHECK(cpu_test()==0);return getenv("PS5C_TEST_GPU")?gpu_test():0;}
