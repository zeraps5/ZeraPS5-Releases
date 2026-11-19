/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/gpu_regs.h"
#include "gpu/vk/vk_internal.h"
#include "gpu/vk/vk_image_formats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A render target created with swapped CB channels is subsequently accessed
 * by a canonical T# storage descriptor. The view must follow the descriptor,
 * preserving bytes rather than converting R/B. */
int main(void) {
    VkFormat formats[VK_IMAGE_ALIAS_CAPACITY];
    unsigned count=vk_image_alias_formats(VK_FORMAT_A2R10G10B10_UNORM_PACK32,4,formats);
    bool correct=vk_image_descriptor_view_format(VK_FORMAT_A2R10G10B10_UNORM_PACK32,
        VK_FORMAT_A2B10G10R10_UNORM_PACK32,true,0,formats,count)==VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    if(!correct)fputs("RGB10 storage descriptor lost its R/B byte order\n",stderr);
    if(!getenv("PS5C_TEST_GPU"))return correct?0:1;
    if(pal_init()!=PS5C_OK)return 1;
    gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    if(gmem_init(&m,&cfg)!=PS5C_OK){pal_shutdown();return 1;}
    guest_addr a;pal_window *window=NULL;renderer *r=NULL;gpu g={0};vk_buffer raw={0};
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);correct=false;goto cleanup;}}while(0)
    REQUIRE(gmem_map(&m,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"RGB10 storage views",&a)==PS5C_OK);
    /* v0/v1 are the local x/y indices; v2 is the array layer. */
    const uint32_t code[]={0x7e040280,0x7e0802ff,0x3f800000,0x7e0a02ff,0,
        0x7e0c02ff,0,0x7e0e02ff,0x3f800000,
        0xf0200f08,0x00000400,0xbf810000};
    REQUIRE(gmem_write(&m,a,code,sizeof(code)));
    REQUIRE(pal_window_open("ZeraPS5 RGB10 storage views",320,180,&window)==PS5C_OK);
    REQUIRE((r=renderer_create(window,&m))!=NULL);
    REQUIRE(gpu_init(&g,&m,NULL)==PS5C_OK);
    REQUIRE(vk_buffer_create(r,4*3*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&raw));
    g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(a>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(a>>40);
    g.regs[mmCOMPUTE_PGM_RSRC2]=(8u<<1)|(1u<<11);
    g.regs[mmCOMPUTE_NUM_THREAD_X]=4;g.regs[mmCOMPUTE_NUM_THREAD_Y]=3;g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    for(unsigned swapped=0;swapped<2;swapped++)for(unsigned array=0;array<2;array++) {
        guest_addr address=a+0x4000+(swapped*2+array)*0x1000;
        uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;memset(u,0,8*4);
        u[0]=(uint32_t)(address>>8);u[1]=(uint32_t)(address>>40)|(50u<<20)|(3u<<30);
        u[2]=2u<<14;u[3]=((array?13u:9u)<<28)|0xfac;
        renderer_submit_begin(r);vk_begin(r);
        VkFormat base=swapped?VK_FORMAT_A2R10G10B10_UNORM_PACK32:VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        vk_image *img=vk_image_get(r,address,4,3,base);REQUIRE(img);
        vk_barrier(r->cmd,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue zero={{0,0,0,0}};VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(r->cmd,img->image,img->layout,&zero,1,&range);
        img->gpu_written=true;img->written_seq=++r->write_seq;
        renderer_dispatch(r,&g,1,1,1);renderer_gpu_idle(r);
        img=vk_image_find(r,address,4,3,base);REQUIRE(img);
        vk_begin(r);vk_barrier(r->cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copy.imageExtent=(VkExtent3D){4,3,1};
        vkCmdCopyImageToBuffer(r->cmd,img->image,img->layout,raw.buffer,1,&copy);renderer_wait_idle(r);
        const uint32_t expected=0xc00003ffu;
        unsigned bad=0;for(unsigned i=0;i<12;i++)bad+=((uint32_t *)raw.mapped)[i]!=expected;
        fprintf(stderr,"RGB10 base %u %s: %u wrong pixels, first %08x expected %08x\n",
            base,array?"array":"2D",bad,((uint32_t *)raw.mapped)[0],expected);correct&=!bad;
    }
cleanup:
    if(r){vk_buffer_destroy(r,&raw);renderer_destroy(r);}
    free(g.regs);if(g.lock)pal_mutex_destroy(g.lock);
    if(window)pal_window_destroy(window);
    gmem_free(&m);pal_shutdown();return correct?0:1;
}
