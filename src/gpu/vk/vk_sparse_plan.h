/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_VK_SPARSE_PLAN_H
#define PS5C_VK_SPARSE_PLAN_H
#include <string.h>
#include "gpu/vk/vk_api.h"

typedef struct vk_sparse_plan {
    VkSparseImageMemoryBind tiles[16];
    uint32_t tile_count;
    VkSparseMemoryBind tail;
    bool has_tail;
    VkDeviceSize bytes;
} vk_sparse_plan;

/* One color aspect/layer, complete mip levels from first onward. Memory
 * requirements come from the sparse image itself, not a dense surrogate.
 * Whole boundary tiles count even when a mip ends inside the granularity. */
static inline bool vk_sparse_plan_build(uint32_t width,uint32_t height,uint32_t levels,uint32_t first,
        const VkMemoryRequirements *mem,const VkSparseImageMemoryRequirements *req,vk_sparse_plan *out) {
    memset(out,0,sizeof(*out));
    VkExtent3D gran=req->formatProperties.imageGranularity;
    uint64_t align=mem->alignment;
    if(!width || !height || !levels || levels>16 || !first || first>=levels ||
       !align || (align&(align-1)) || !gran.width || !gran.height || gran.depth!=1 ||
       req->formatProperties.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT) return false;
    uint32_t stop=req->imageMipTailFirstLod<levels?req->imageMipTailFirstLod:levels;
    uint64_t at=0;
    for(uint32_t lv=first;lv<stop;lv++) {
        uint32_t w=width>>lv?width>>lv:1,h=height>>lv?height>>lv:1;
        uint64_t x=((uint64_t)w+gran.width-1)/gran.width,y=((uint64_t)h+gran.height-1)/gran.height;
        if(x>UINT64_MAX/y || x*y>UINT64_MAX/align || at>UINT64_MAX-x*y*align) return false;
        VkSparseImageMemoryBind *b=&out->tiles[out->tile_count++];
        b->subresource=(VkImageSubresource){VK_IMAGE_ASPECT_COLOR_BIT,lv,0};
        b->extent=(VkExtent3D){w,h,1};b->memoryOffset=at;
        at+=x*y*align;
    }
    if(req->imageMipTailFirstLod<levels && req->imageMipTailSize) {
        uint64_t size=req->imageMipTailSize;
        if(req->imageMipTailOffset>mem->size || size>mem->size-req->imageMipTailOffset ||
           size>UINT64_MAX-(align-1)) return false;
        uint64_t alloc=PS5C_ALIGN_UP(size,align);
        if(at>UINT64_MAX-alloc) return false;
        out->has_tail=true;
        out->tail=(VkSparseMemoryBind){.resourceOffset=req->imageMipTailOffset,.size=size,.memoryOffset=at};
        at+=alloc;
    }
    if(!at || at>=mem->size) return false; /* no physical memory saving: use dense allocation */
    out->bytes=at;return true;
}
#endif
