/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_VK_IMAGE_FORMATS_H
#define PS5C_VK_IMAGE_FORMATS_H
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

static inline bool vk_image_rgba8_family(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UINT ||
           f == VK_FORMAT_R8G8B8A8_SNORM || f == VK_FORMAT_R8G8B8A8_SINT || f == VK_FORMAT_R8G8B8A8_SRGB ||
           f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
}

static inline bool vk_image_rg8_family(VkFormat f) {
    return f == VK_FORMAT_R8G8_UNORM || f == VK_FORMAT_R8G8_SNORM ||
           f == VK_FORMAT_R8G8_UINT || f == VK_FORMAT_R8G8_SINT || f == VK_FORMAT_R8G8_SRGB;
}

static inline bool vk_image_rgb10_family(VkFormat f) {
    return f == VK_FORMAT_A2R10G10B10_UNORM_PACK32 || f == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
}

#define VK_IMAGE_ALIAS_CAPACITY 12
/* Same-size color formats allowed by the image's mutable-format list. */
static inline uint32_t vk_image_alias_formats(VkFormat f, uint32_t bytes, VkFormat out[VK_IMAGE_ALIAS_CAPACITY]) {
    uint32_t n = 0;
    out[n++] = f;
    static const VkFormat k_by_size[5][3] = {
        {VK_FORMAT_R8_UINT, VK_FORMAT_R8_UNORM, VK_FORMAT_UNDEFINED},
        {VK_FORMAT_R16_UINT, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_UNORM},
        {VK_FORMAT_R32_UINT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_SINT},
        {VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R16G16B16A16_UINT},
        {VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_UNDEFINED},
    };
    uint32_t row = bytes == 1 ? 0 : bytes == 2 ? 1 : bytes == 4 ? 2 : bytes == 8 ? 3 : bytes == 16 ? 4 : 5;
    for (uint32_t i = 0; row < 5 && i < 3; i++)
        if (k_by_size[row][i] != VK_FORMAT_UNDEFINED && k_by_size[row][i] != f) out[n++] = k_by_size[row][i];
    /* A BGRA render target can be read as raw RGBA bytes by a T# descriptor.
     * Include sRGB too: the descriptor controls the sampling conversion. */
    static const VkFormat k_rgba8[5] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT,
                                       VK_FORMAT_R8G8B8A8_SINT,
                                       VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_SRGB};
    for (uint32_t i = 0; vk_image_rgba8_family(f) && i < 5 && n < VK_IMAGE_ALIAS_CAPACITY; i++)
        if (k_rgba8[i] != f) out[n++] = k_rgba8[i];
    /* NV12 copy shaders store UV as integers, then sample the same bytes as
     * normalized R/G channels. These are compatible 16-bit image formats. */
    static const VkFormat k_rg8[5] = {VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM,
                                     VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8_SRGB};
    for (uint32_t i = 0; vk_image_rg8_family(f) && i < 5 && n < VK_IMAGE_ALIAS_CAPACITY; i++)
        if (k_rg8[i] != f) out[n++] = k_rg8[i];
    /* A CB target may use swapped channels while its T# writer uses canonical
     * RGB10. A2R10 itself need not support storage; its A2B10 view does. */
    if (vk_image_rgb10_family(f))
        out[n++] = f == VK_FORMAT_A2R10G10B10_UNORM_PACK32 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32
                                                       : VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    return n;
}

static inline uint32_t vk_image_numeric_kind(VkFormat f) {
    switch(f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R64_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: return 1;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_SINT: return 2;
    default: return 0;
    }
}

static inline VkFormat vk_image_descriptor_view_format(VkFormat image, VkFormat requested,
                                                        bool store, uint32_t kind,
                                                        const VkFormat *allowed, uint32_t count) {
    if (requested == image || requested == VK_FORMAT_UNDEFINED) return image;
    bool listed = false;
    for (uint32_t i = 0; i < count; i++) listed |= allowed[i] == requested;
    if (!listed) return image;
    /* Read the descriptor's byte order and number representation first.
     * Shader DST_SEL still performs the guest's channel selection afterwards. */
    if ((vk_image_rgba8_family(image) && vk_image_rgba8_family(requested)) ||
        (vk_image_rg8_family(image) && vk_image_rg8_family(requested)) ||
        (vk_image_rgb10_family(image) && vk_image_rgb10_family(requested))) return requested;
    return store || kind != vk_image_numeric_kind(image) ? requested : image;
}
#endif
