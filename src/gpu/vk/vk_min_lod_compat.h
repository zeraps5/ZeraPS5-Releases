/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Extension ABI declarations for build hosts with older Vulkan headers.
 * Values/layout: Khronos Vulkan registry, VK_EXT_image_view_min_lod. */
#ifndef PS5C_VK_MIN_LOD_COMPAT_H
#define PS5C_VK_MIN_LOD_COMPAT_H
#ifndef VK_EXT_image_view_min_lod
#define VK_EXT_image_view_min_lod 1
#define VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME "VK_EXT_image_view_min_lod"
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT ((VkStructureType)1000391000)
#define VK_STRUCTURE_TYPE_IMAGE_VIEW_MIN_LOD_CREATE_INFO_EXT ((VkStructureType)1000391001)
typedef struct VkPhysicalDeviceImageViewMinLodFeaturesEXT {
    VkStructureType sType;
    void *pNext;
    VkBool32 minLod;
} VkPhysicalDeviceImageViewMinLodFeaturesEXT;
typedef struct VkImageViewMinLodCreateInfoEXT {
    VkStructureType sType;
    const void *pNext;
    float minLod;
} VkImageViewMinLodCreateInfoEXT;
#endif
#endif
