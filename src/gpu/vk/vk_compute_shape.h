/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_VK_COMPUTE_SHAPE_H
#define PS5C_VK_COMPUTE_SHAPE_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

/* Same workgroup and same linear invocation order, with legal host axes.
 * No workgroup splitting: barriers and LDS remain shared by the same threads. */
static inline bool vk_compute_shape(const uint32_t guest[3], const VkPhysicalDeviceLimits *limits,
                                    uint32_t host[3]) {
    memset(host,0,3*sizeof(*host));
    uint64_t total=(uint64_t)guest[0]*guest[1]*guest[2];
    if(!total || total>limits->maxComputeWorkGroupInvocations) return false;
    if(guest[0]<=limits->maxComputeWorkGroupSize[0] && guest[1]<=limits->maxComputeWorkGroupSize[1] &&
       guest[2]<=limits->maxComputeWorkGroupSize[2]) return true;
    uint32_t maxx=total<limits->maxComputeWorkGroupSize[0]?(uint32_t)total:limits->maxComputeWorkGroupSize[0];
    for(uint32_t x=maxx;x;x--) {
        if(total%x) continue;
        uint32_t rest=(uint32_t)total/x,maxy=rest<limits->maxComputeWorkGroupSize[1]?rest:limits->maxComputeWorkGroupSize[1];
        for(uint32_t y=maxy;y;y--)
            if(rest%y==0 && rest/y<=limits->maxComputeWorkGroupSize[2]) {
                host[0]=x;host[1]=y;host[2]=rest/y;return true;
            }
    }
    return false;
}
#endif
