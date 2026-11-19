/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/vk/vk_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool clear_and_read(renderer *r,vk_image *write,vk_image *read,vk_buffer *out) {
    vk_begin(r);
    vk_barrier(r->cmd,write,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue color={{0.125f,0.25f,0.5f,1.0f}};
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(r->cmd,write->image,write->layout,&color,1,&range);
    write->gpu_written=true;write->written_seq=++r->write_seq;
    vk_barrier(r->cmd,read,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy={0};copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount=1;copy.imageExtent=(VkExtent3D){4,4,1};
    vkCmdCopyImageToBuffer(r->cmd,read->image,read->layout,out->buffer,1,&copy);
    vk_flush(r);
    if(vkQueueWaitIdle(r->queue)!=VK_SUCCESS)return false;
    const uint16_t *v=out->mapped;const uint16_t expected[]={0x3000,0x3400,0x3800,0x3c00};
    for(unsigned i=0;i<64;i++)if(v[i]!=expected[i&3])return false;
    return true;
}

int main(void) {
    if(!pal_ph_supported()){puts("SKIP: shared placeholders unavailable");return 0;}
    if(gmem_set_shared_area_base(0x1000000000ull)!=PS5C_OK || gmem_reserve_early()!=PS5C_OK || pal_init()!=PS5C_OK)return 1;
    gmem m;gmem_config cfg=gmem_default_config();
    if(gmem_init(&m,&cfg)!=PS5C_OK)return 1;
    const guest_addr a=0x3001000000ull,b=0x3002000000ull,c=0x3003000000ull;
    const uint64_t physical=0x120000,other=0x220000;
    guest_addr out=0;bool shared=false,correct=m.ph_start==0x1000000000ull;
    const guest_addr addresses[]={a,b,c};
    for(unsigned i=0;correct && i<3;i++)correct=gmem_map_direct(&m,addresses[i],4*GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,
        GPROT_RW,GMAP_FIXED|GMAP_REPLACE,i==2?other:physical,"low image alias",&out,&shared)==PS5C_OK && shared && out==addresses[i];
    renderer *meta=calloc(1,sizeof(*meta));if(!meta){gmem_free(&m);return 1;}meta->mem=&m;
    if(correct) {
        correct=vk_guest_access_begin(meta) && vk_phys_of(meta,a)==physical && vk_phys_of(meta,b)==physical &&
            vk_phys_of(meta,b+GUEST_PAGE_SIZE+8)==physical+GUEST_PAGE_SIZE+8 && vk_phys_of(meta,c)==other &&
            vk_phys_of(meta,0x3004000000ull)==VK_NO_PHYS;
        vk_guest_access_end(meta);
    }
    free(meta);
    if(!correct)fprintf(stderr,"low shared view lost its physical identity below GUEST_PH_BASE\n");
    renderer *r=NULL;pal_window *window=NULL;vk_buffer buffer={0};
    if(correct && getenv("PS5C_TEST_GPU")) {
        correct=pal_window_open("ZeraPS5 low physical aliases",320,180,&window)==PS5C_OK && (r=renderer_create(window,&m));
        if(correct) {
            correct=vk_guest_access_begin(r);vk_begin(r);
            vk_image *first=vk_image_get(r,a,4,4,VK_FORMAT_R16G16B16A16_SFLOAT);
            vk_image *alias=vk_image_get(r,b,4,4,VK_FORMAT_R16G16B16A16_SFLOAT);
            vk_image *different=vk_image_get(r,c,4,4,VK_FORMAT_R16G16B16A16_SFLOAT);
            correct=correct && first && alias==first && different && different!=first && r->image_count==2;
            if(correct)correct=vk_buffer_create(r,128,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&buffer) && clear_and_read(r,first,alias,&buffer);
            vk_guest_access_end(r);
            /* Remap the original cached virtual address. The old contents
             * still belong to its other alias; the new physical page is C. */
            if(correct)correct=gmem_map_direct(&m,a,4*GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,GPROT_RW,
                GMAP_FIXED|GMAP_REPLACE,other,"remapped image",&out,&shared)==PS5C_OK && shared;
            if(correct) {
                correct=vk_guest_access_begin(r) && vk_phys_of(r,a)==other &&
                    vk_image_get(r,a,4,4,VK_FORMAT_R16G16B16A16_SFLOAT)==different &&
                    vk_image_get(r,b,4,4,VK_FORMAT_R16G16B16A16_SFLOAT)==first && r->image_count==2;
                if(correct)correct=clear_and_read(r,first,first,&buffer);
                vk_guest_access_end(r);
            }
        }
    }
    if(r){vk_buffer_destroy(r,&buffer);renderer_destroy(r);}
    if(window)pal_window_destroy(window);
    gmem_free(&m);pal_shutdown();
    if(!correct){fputs("physical image alias/remap regression failed\n",stderr);return 1;}
    puts("low physical aliases: identity, offset, distinct pages, shared HDR pixels and remap isolation verified");
    return 0;
}
