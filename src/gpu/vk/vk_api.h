/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Vulkan entry points, loaded at run time from the system loader
 * (vulkan-1.dll / libvulkan.so.1). Nothing links against Vulkan: a host
 * without it still runs the emulator, with the software presenter. */
#ifndef PS5C_VK_API_H
#define PS5C_VK_API_H

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include "gpu/vk/vk_mesh_compat.h"
#include "gpu/vk/vk_min_lod_compat.h"

#include "base/base.h"

#define VK_GLOBAL_FUNCS(X)                                                                                             \
    X(vkCreateInstance)                                                                                                \
    X(vkEnumerateInstanceExtensionProperties)                                                                          \
    X(vkEnumerateInstanceLayerProperties)

#define VK_INSTANCE_FUNCS(X)                                                                                           \
    X(vkDestroyInstance)                                                                                               \
    X(vkEnumeratePhysicalDevices)                                                                                      \
    X(vkGetPhysicalDeviceProperties)                                                                                   \
    X(vkGetPhysicalDeviceProperties2)                                                                                  \
    X(vkGetPhysicalDeviceFeatures)                                                                                     \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                                        \
    X(vkGetPhysicalDeviceMemoryProperties)                                                                             \
    X(vkGetPhysicalDeviceFormatProperties)                                                                             \
    X(vkGetPhysicalDeviceImageFormatProperties)                                                                        \
    X(vkEnumerateDeviceExtensionProperties)                                                                            \
    X(vkCreateDevice)                                                                                                  \
    X(vkGetDeviceProcAddr)                                                                                             \
    X(vkDestroySurfaceKHR)                                                                                             \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)                                                                            \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                                       \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                                            \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define VK_DEVICE_FUNCS(X)                                                                                             \
    X(vkDestroyDevice)                                                                                                 \
    X(vkGetDeviceQueue)                                                                                                \
    X(vkDeviceWaitIdle)                                                                                                \
    X(vkQueueSubmit)                                                                                                   \
    X(vkQueueBindSparse)                                                                                               \
    X(vkQueueWaitIdle)                                                                                                 \
    X(vkQueuePresentKHR)                                                                                               \
    X(vkCreateSwapchainKHR)                                                                                            \
    X(vkDestroySwapchainKHR)                                                                                           \
    X(vkGetSwapchainImagesKHR)                                                                                         \
    X(vkAcquireNextImageKHR)                                                                                           \
    X(vkCreateCommandPool)                                                                                             \
    X(vkDestroyCommandPool)                                                                                            \
    X(vkAllocateCommandBuffers)                                                                                        \
    X(vkFreeCommandBuffers)                                                                                            \
    X(vkBeginCommandBuffer)                                                                                            \
    X(vkEndCommandBuffer)                                                                                              \
    X(vkResetCommandBuffer)                                                                                            \
    X(vkCreateFence)                                                                                                   \
    X(vkDestroyFence)                                                                                                  \
    X(vkWaitForFences)                                                                                                 \
    X(vkResetFences)                                                                                                   \
    X(vkGetFenceStatus)                                                                                                \
    X(vkCreateSemaphore)                                                                                               \
    X(vkDestroySemaphore)                                                                                              \
    X(vkCreateImage)                                                                                                   \
    X(vkDestroyImage)                                                                                                  \
    X(vkGetImageMemoryRequirements)                                                                                    \
    X(vkGetImageSparseMemoryRequirements)                                                                              \
    X(vkBindImageMemory)                                                                                               \
    X(vkCreateBuffer)                                                                                                  \
    X(vkDestroyBuffer)                                                                                                 \
    X(vkGetBufferMemoryRequirements)                                                                                   \
    X(vkBindBufferMemory)                                                                                              \
    X(vkAllocateMemory)                                                                                                \
    X(vkFreeMemory)                                                                                                    \
    X(vkMapMemory)                                                                                                     \
    X(vkUnmapMemory)                                                                                                   \
    X(vkCreateImageView)                                                                                               \
    X(vkDestroyImageView)                                                                                              \
    X(vkCreateSampler)                                                                                                 \
    X(vkDestroySampler)                                                                                                \
    X(vkCreateShaderModule)                                                                                            \
    X(vkDestroyShaderModule)                                                                                           \
    X(vkCreatePipelineLayout)                                                                                          \
    X(vkDestroyPipelineLayout)                                                                                         \
    X(vkCreateGraphicsPipelines)                                                                                       \
    X(vkCreateComputePipelines)                                                                                        \
    X(vkDestroyPipeline)                                                                                               \
    X(vkCreatePipelineCache)                                                                                           \
    X(vkGetPipelineCacheData)                                                                                          \
    X(vkDestroyPipelineCache)                                                                                          \
    X(vkCreateDescriptorSetLayout)                                                                                     \
    X(vkDestroyDescriptorSetLayout)                                                                                    \
    X(vkCreateDescriptorPool)                                                                                          \
    X(vkDestroyDescriptorPool)                                                                                         \
    X(vkResetDescriptorPool)                                                                                           \
    X(vkAllocateDescriptorSets)                                                                                        \
    X(vkUpdateDescriptorSets)                                                                                          \
    X(vkCreateRenderPass)                                                                                              \
    X(vkDestroyRenderPass)                                                                                             \
    X(vkCreateFramebuffer)                                                                                             \
    X(vkDestroyFramebuffer)                                                                                            \
    X(vkCmdPipelineBarrier)                                                                                            \
    X(vkCmdCopyBuffer)                                                                                                 \
    X(vkCmdCopyBufferToImage)                                                                                          \
    X(vkCmdCopyImageToBuffer)                                                                                          \
    X(vkCmdCopyImage)                                                                                                  \
    X(vkCmdBlitImage)                                                                                                  \
    X(vkCmdClearAttachments)                                                                                           \
    X(vkCmdClearDepthStencilImage)                                                                                     \
    X(vkCmdClearColorImage)                                                                                            \
    X(vkCmdFillBuffer)                                                                                                 \
    X(vkCmdBeginRenderPass)                                                                                            \
    X(vkCmdEndRenderPass)                                                                                              \
    X(vkCmdBindPipeline)                                                                                               \
    X(vkCmdBindDescriptorSets)                                                                                         \
    X(vkCmdBindVertexBuffers)                                                                                          \
    X(vkCmdBindIndexBuffer)                                                                                            \
    X(vkCmdPushConstants)                                                                                              \
    X(vkCmdSetViewport)                                                                                                \
    X(vkCmdSetScissor)                                                                                                 \
    X(vkCmdSetDepthBias)                                                                                               \
    X(vkCmdSetBlendConstants)                                                                                          \
    X(vkCmdSetStencilReference)                                                                                        \
    X(vkCmdSetStencilCompareMask)                                                                                      \
    X(vkCmdSetStencilWriteMask)                                                                                        \
    X(vkCmdDraw)                                                                                                       \
    X(vkCmdDrawIndexed)                                                                                                \
    X(vkCmdDrawIndirect) \
    X(vkCmdDispatch)                                                                                                   \
    X(vkCmdDispatchBase)                                                                                               \
    X(vkCreateQueryPool)                                                                                               \
    X(vkDestroyQueryPool)                                                                                              \
    X(vkGetQueryPoolResults)                                                                                           \
    X(vkCmdResetQueryPool)                                                                                                 X(vkCmdBeginQuery)                                                                                                     X(vkCmdEndQuery)                                                                                                   \
    X(vkCmdWriteTimestamp)

#define VK_DECLARE(name) extern PFN_##name name;
VK_GLOBAL_FUNCS(VK_DECLARE)
VK_INSTANCE_FUNCS(VK_DECLARE)
VK_DEVICE_FUNCS(VK_DECLARE)
#undef VK_DECLARE

/* Loads the Vulkan loader and its global functions. False if Vulkan is absent. */
bool vk_api_load(void);
bool vk_api_load_instance(VkInstance instance);
bool vk_api_load_device(VkDevice device);
PFN_vkVoidFunction vk_api_instance_proc(VkInstance instance, const char *name);
/* The loader's instance version (vkEnumerateInstanceVersion); 1.0 when it has none. */
uint32_t vk_api_instance_version(void);

#endif
