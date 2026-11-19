/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/vk/vk_internal.h"
#include "gpu/vk/vk_sparse_plan.h"
#include "base/log.h"

/* A fresh image only: no in-place rebind or interaction with older commands.
 * Fence completion explicitly orders the binding before its first submit.
 * Unsupported aspects/metadata/usage retain the existing dense path. */
bool vk_sparse_image_create(renderer *r,const VkImageCreateInfo *info,uint32_t first,
                            VkImage *image,VkDeviceMemory *memory,VkDeviceSize *bytes) {
    if(!r->sparse_mips || !first || first>=info->mipLevels || info->imageType!=VK_IMAGE_TYPE_2D ||
       info->arrayLayers!=1 || info->samples!=VK_SAMPLE_COUNT_1_BIT ||
       info->format<VK_FORMAT_BC1_RGB_UNORM_BLOCK || info->format>VK_FORMAT_BC7_SRGB_BLOCK ||
       (info->usage & ~(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT))) return false;
    VkImageCreateInfo ci=*info;
    ci.flags|=VK_IMAGE_CREATE_SPARSE_BINDING_BIT|VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
    VkImageFormatProperties props;
    if(vkGetPhysicalDeviceImageFormatProperties(r->phys,ci.format,ci.imageType,ci.tiling,ci.usage,ci.flags,&props)!=VK_SUCCESS)
        return false;
    if(ci.extent.width>props.maxExtent.width || ci.extent.height>props.maxExtent.height ||
       ci.extent.depth>props.maxExtent.depth || ci.mipLevels>props.maxMipLevels ||
       ci.arrayLayers>props.maxArrayLayers || !(props.sampleCounts&ci.samples)) return false;
    VkImage img=VK_NULL_HANDLE;VkDeviceMemory mem=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;
    if(vkCreateImage(r->device,&ci,NULL,&img)!=VK_SUCCESS) return false;
    VkMemoryRequirements full;vkGetImageMemoryRequirements(r->device,img,&full);
    uint32_t count=0;vkGetImageSparseMemoryRequirements(r->device,img,&count,NULL);
    if(count!=1) goto fallback;
    VkSparseImageMemoryRequirements req;vkGetImageSparseMemoryRequirements(r->device,img,&count,&req);
    vk_sparse_plan plan;
    if(count!=1 || !vk_sparse_plan_build(ci.extent.width,ci.extent.height,ci.mipLevels,first,&full,&req,&plan)) goto fallback;
    VkMemoryRequirements actual=full;actual.size=plan.bytes;
    if(!vk_alloc_memory(r,&actual,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,&mem)) goto fallback;
    for(uint32_t k=0;k<plan.tile_count;k++)plan.tiles[k].memory=mem;
    plan.tail.memory=mem;
    VkSparseImageMemoryBindInfo tiles={.image=img,.bindCount=plan.tile_count,.pBinds=plan.tiles};
    VkSparseImageOpaqueMemoryBindInfo tail={.image=img,.bindCount=1,.pBinds=&plan.tail};
    VkBindSparseInfo bind={.sType=VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
        .imageBindCount=plan.tile_count?1u:0u,.pImageBinds=plan.tile_count?&tiles:NULL,
        .imageOpaqueBindCount=plan.has_tail?1u:0u,.pImageOpaqueBinds=plan.has_tail?&tail:NULL};
    VkFenceCreateInfo fc={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if(vkCreateFence(r->device,&fc,NULL,&fence)!=VK_SUCCESS) goto fallback;
    VkResult result=vkQueueBindSparse(r->queue,1,&bind,fence);
    vk_check_work(r,result,"vkQueueBindSparse");
    vk_check_work(r,vkWaitForFences(r->device,1,&fence,VK_TRUE,UINT64_MAX),"vkWaitForFences (sparse binding)");
    vkDestroyFence(r->device,fence,NULL);
    *image=img;*memory=mem;*bytes=plan.bytes;
    static uint32_t logged;
    if(logged++<32)LOG_I(GPU,"sparse mip allocation %ux%u fmt%d first%u tail%u: %llu / %llu bytes",
        ci.extent.width,ci.extent.height,(int)ci.format,first,req.imageMipTailFirstLod,
        (unsigned long long)plan.bytes,(unsigned long long)full.size);
    return true;
fallback:
    if(fence)vkDestroyFence(r->device,fence,NULL);
    vkDestroyImage(r->device,img,NULL);
    if(mem)vkFreeMemory(r->device,mem,NULL);
    return false;
}
