/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_VK_RESOURCE_LIMITS_H
#define PS5C_VK_RESOURCE_LIMITS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

/* The recipe uses storage buffers, storage images and combined samplers,
 * one descriptor per binding, with stages 0=VS, 1=PS, 2=CS. A combined
 * sampler counts as both sampler and sampled image, but only one resource.
 * Limits apply to declared stage visibility, not just the instructions used.
 * NULL means valid; otherwise return the exceeded limit for the diagnostic. */
static inline const char *vk_pipeline_descriptor_error(const VkPhysicalDeviceLimits *limits,
                                                        const uint8_t *types, const uint8_t *stages,
                                                        uint32_t count, uint32_t color_attachments) {
    uint32_t used[3][3] = {{0}}, total[3] = {0}; /* buffers, storage images, combined samplers */
    if (color_attachments > limits->maxColorAttachments) return "maxColorAttachments";
    for (uint32_t i = 0; i < count; i++) {
        if (stages[i] >= 3) return "invalid descriptor stage";
        uint32_t kind;
        switch (types[i]) {
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: kind = 0; break;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: kind = 1; break;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: kind = 2; break;
        default: return "unsupported descriptor type";
        }
        used[stages[i]][kind]++;
        total[kind]++;
    }
    for (uint32_t s = 0; s < 3; s++) {
        if (used[s][0] > limits->maxPerStageDescriptorStorageBuffers) return "maxPerStageDescriptorStorageBuffers";
        if (used[s][1] > limits->maxPerStageDescriptorStorageImages) return "maxPerStageDescriptorStorageImages";
        if (used[s][2] > limits->maxPerStageDescriptorSamplers) return "maxPerStageDescriptorSamplers";
        if (used[s][2] > limits->maxPerStageDescriptorSampledImages) return "maxPerStageDescriptorSampledImages";
        uint64_t resources = (uint64_t)used[s][0] + used[s][1] + used[s][2];
        if (s == 1) resources += color_attachments;
        if (resources > limits->maxPerStageResources) return "maxPerStageResources";
    }
    if ((uint64_t)used[1][0] + used[1][1] + color_attachments > limits->maxFragmentCombinedOutputResources)
        return "maxFragmentCombinedOutputResources";
    if (total[0] > limits->maxDescriptorSetStorageBuffers) return "maxDescriptorSetStorageBuffers";
    if (total[1] > limits->maxDescriptorSetStorageImages) return "maxDescriptorSetStorageImages";
    if (total[2] > limits->maxDescriptorSetSamplers) return "maxDescriptorSetSamplers";
    if (total[2] > limits->maxDescriptorSetSampledImages) return "maxDescriptorSetSampledImages";
    return NULL;
}

/* DB_SHADER_CONTROL: Z/stencil exports [2:0], kill/coverage/mask [8:6],
 * EXEC_ON_HIER_FAIL/NOOP [10:9]. Those require the guest pixel shader even
 * without color writes. Z_ORDER and ALPHA_TO_MASK_DISABLE alone do not. */
static inline bool vk_opaque_depth_only(uint32_t shader_control, uint32_t color_control, uint32_t target_mask) {
    bool color_off = ((color_control >> 4) & 7u) == 0 || target_mask == 0;
    return color_off && !(shader_control & 0x7C7u);
}

/* A depth/stencil-clear draw without enabled color outputs consumes clear registers,
 * not the stale pixel shader/resource state left by the preceding pass. */
static inline bool vk_depth_stencil_clear_only(uint32_t render_control, uint32_t color_control, uint32_t target_mask) {
    return (render_control & 3u) && (!target_mask || ((color_control >> 4) & 7u) == 0);
}

/* depth==0 is the renderer's 2D/array representation. Reject unsupported
 * extents before creating objects or evicting a valid cached image. */
static inline bool vk_image_extent_valid(const VkPhysicalDeviceLimits *limits,
                                         uint32_t width, uint32_t height,
                                         uint32_t layers, uint32_t depth) {
    if (!width || !height || !layers) return false;
    if (depth)
        return layers == 1 && width <= limits->maxImageDimension3D &&
               height <= limits->maxImageDimension3D && depth <= limits->maxImageDimension3D;
    return width <= limits->maxImageDimension2D && height <= limits->maxImageDimension2D &&
           layers <= limits->maxImageArrayLayers;
}

/* The device-wide limits alone do not establish support for this particular
 * combination of format, type, tiling, usage and flags. */
static inline bool vk_image_format_valid(VkResult result, const VkImageCreateInfo *request,
                                         const VkImageFormatProperties *limits) {
    if (result != VK_SUCCESS) return false;
    return request->extent.width && request->extent.height && request->extent.depth &&
           request->extent.width <= limits->maxExtent.width &&
           request->extent.height <= limits->maxExtent.height &&
           request->extent.depth <= limits->maxExtent.depth &&
           request->mipLevels && request->mipLevels <= limits->maxMipLevels &&
           request->arrayLayers && request->arrayLayers <= limits->maxArrayLayers &&
           request->samples && !(request->samples & (request->samples - 1)) &&
           (request->samples & limits->sampleCounts) != 0;
}
#endif
