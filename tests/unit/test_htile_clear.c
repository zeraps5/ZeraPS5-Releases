/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RDNA2 DB_Z_INFO.TILE_SURFACE_ENABLE is bit 29; bit 31 alone must not enable HTILE. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/gpu_regs.h"
#include "gpu/shader/shader.h"
#include "gpu/vk/vk_internal.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)

/* Degenerate triangle: registers the targets without changing their pixels. */
static const uint32_t vs[] = {0x7e000280,0x7e020280,0x7e040280,0x7e0602f2,0xf80008cf,0x03020100,0xbf810000};
static const uint32_t ps[] = {0x7e0002f2,0x7e020280,0x7e040280,0x7e0602f2,0xf800180f,0x03020100,0xbf810000};
/* A metadata maintenance pass that reads/writes unchanged words is not a
 * new whole-depth clear. A write-only fill of the same zeros is a clear. */
static const uint32_t copy_meta[] = {0xe0380000,0x80000000,0xbf8c3f70,0xe0780000,0x80000000,0xbf810000};
static const uint32_t fill_meta[] = {0x7e000280,0x7e020280,0x7e040280,0x7e060280,0xe0780000,0x80000000,0xbf810000};

static void paint_depth(renderer *r, vk_image *z, float value) {
    VkCommandBuffer cmd=vk_begin(r);
    vk_barrier(cmd,z,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearDepthStencilValue clear={value,0};
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
    vkCmdClearDepthStencilImage(cmd,z->image,z->layout,&clear,1,&range);
    z->gpu_written=true; z->written_seq=++r->write_seq;
    renderer_wait_idle(r);
}
static bool depth_is(renderer *r, vk_image *z, vk_buffer *out, float value) {
    VkCommandBuffer cmd=vk_begin(r);
    vk_barrier(cmd,z,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy={0}; copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    copy.imageSubresource.layerCount=1; copy.imageExtent=(VkExtent3D){16,16,1};
    vkCmdCopyImageToBuffer(cmd,z->image,z->layout,out->buffer,1,&copy);
    renderer_wait_idle(r);
    const float *pixels=out->mapped;
    for (unsigned i=0;i<256;i++) if (pixels[i]!=value) {
        fprintf(stderr,"depth texel %u: %g, expected %g\n",i,pixels[i],value); return false;
    }
    return true;
}
static int run_gpu(void) {
    CHECK(pal_init()==PS5C_OK);
    gmem m; gmem_config cfg=gmem_default_config(); cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr a; CHECK(gmem_map(&m,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"HTILE",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,vs,sizeof(vs)) && gmem_write(&m,a+0x100,ps,sizeof(ps)) &&
          gmem_write(&m,a+0x200,copy_meta,sizeof(copy_meta)) && gmem_write(&m,a+0x300,fill_meta,sizeof(fill_meta)));
    pal_window *window=NULL; CHECK(pal_window_open("ZeraPS5 HTILE clear regression",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m); CHECK(r);
    gpu g; CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(a>>8);
    g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(a>>40);
    g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x100)>>8);
    g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)((a+0x100)>>40);
    g.regs[mmCB_COLOR_CONTROL]=0x10; g.regs[mmCB_TARGET_MASK]=15;
    g.regs[mmCB_COLOR0_BASE]=(uint32_t)((a+0x4000)>>8);
    g.regs[mmCB_COLOR0_INFO]=10u<<2;
    g.regs[mmCB_COLOR0_ATTRIB2]=(15u<<14)|15;
    g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_TRILIST;
    g.regs[mmDB_DEPTH_CONTROL]=2u|(7u<<4);
    g.regs[mmDB_DEPTH_SIZE_XY]=(15u<<16)|15;
    g.regs[mmDB_DEPTH_CLEAR]=0x3e800000; /* 0.25 */
    gpu_draw draw={.count=3,.instances=1};
    vk_buffer out; CHECK(vk_buffer_create(r,1024,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&out));
    const uint32_t modes[]={1u<<29,1u<<31,(1u<<29)|(1u<<31),0};
    for (unsigned k=0;k<4;k++) {
        guest_addr za=a+0x8000+k*0x1000, meta=a+0xc000+k*0x100;
        uint32_t *metadata=gmem_host(&m,meta,16); CHECK(metadata);
        for (unsigned j=0;j<4;j++) metadata[j]=0xffffffff;
        g.regs[mmDB_Z_INFO]=3u|modes[k];
        g.regs[mmDB_Z_READ_BASE]=g.regs[mmDB_Z_WRITE_BASE]=(uint32_t)(za>>8);
        g.regs[mmDB_HTILE_DATA_BASE]=(uint32_t)(meta>>8);
        renderer_draw(r,&g,&draw); renderer_wait_idle(r);
        vk_image *z=vk_image_find(r,za,16,16,VK_FORMAT_D32_SFLOAT); CHECK(z);
        paint_depth(r,z,0.75f);
        memset(metadata,0,16); renderer_mem_written(r,meta,16);
        renderer_draw(r,&g,&draw); renderer_wait_idle(r);
        float expected=(modes[k]&(1u<<29))?0.25f:0.75f;
        fprintf(stderr,"HTILE Z_INFO=%08x: expected %g\n",3u|modes[k],expected);
        CHECK(depth_is(r,z,&out,expected));
        /* A later draw cannot consume the same clear again. */
        paint_depth(r,z,0.5f);
        renderer_draw(r,&g,&draw); renderer_wait_idle(r);
        CHECK(depth_is(r,z,&out,0.5f));
        /* Metadata marked expanded must preserve the new depth. */
        for (unsigned j=0;j<4;j++) metadata[j]=0xffffffff;
        renderer_mem_written(r,meta,16); renderer_draw(r,&g,&draw); renderer_wait_idle(r);
        CHECK(depth_is(r,z,&out,0.5f));
        /* A read-modify-write maintenance shader may make no stores at all
         * (GTA's HTILE expander). Unchanged metadata must not wipe new depth. */
        memset(metadata,0,16); renderer_mem_written(r,meta,16);
        renderer_draw(r,&g,&draw); renderer_wait_idle(r);
        paint_depth(r,z,0.5f);
        g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(a>>40);
        g.regs[mmCOMPUTE_PGM_RSRC2]=4u<<1;
        g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
        uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
        u[0]=(uint32_t)meta; u[1]=(uint32_t)(meta>>32); u[2]=16; u[3]=0x00016204;
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x200)>>8);
        renderer_submit_begin(r); renderer_dispatch(r,&g,1,1,1);
        renderer_draw(r,&g,&draw); renderer_wait_idle(r);
        CHECK(depth_is(r,z,&out,0.5f));
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x300)>>8);
        renderer_submit_begin(r); renderer_dispatch(r,&g,1,1,1);
        renderer_draw(r,&g,&draw); renderer_wait_idle(r);
        CHECK(depth_is(r,z,&out,(modes[k]&(1u<<29))?0.25f:0.5f));
    }
    vk_buffer_destroy(r,&out); renderer_destroy(r); free(g.regs); pal_mutex_destroy(g.lock);
    pal_window_destroy(window); gmem_free(&m); pal_shutdown();
    puts("HTILE GPU: bit29, bit31 independence, single consumption and expanded metadata passed");
    return 0;
}
int main(void) {
    sh_program *p=calloc(1,sizeof(*p)); CHECK(p);
    CHECK(sh_analyze(vs,PS5C_ARRAY_COUNT(vs),0x100000,p));
    CHECK(sh_analyze(ps,PS5C_ARRAY_COUNT(ps),0x100100,p));
    CHECK(sh_analyze(copy_meta,PS5C_ARRAY_COUNT(copy_meta),0x100200,p) && p->site_count==2);
    CHECK(sh_analyze(fill_meta,PS5C_ARRAY_COUNT(fill_meta),0x100300,p) && p->site_count==1);
    free(p);
    return getenv("PS5C_TEST_GPU")?run_gpu():0;
}
