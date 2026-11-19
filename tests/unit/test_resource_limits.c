/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/vk/vk_resource_limits.h"
#include "test.h"

static void test_invalid_skyrim_extent(void) {
    /* Extent produced by Skyrim's failing descriptor evaluation. */
    VkPhysicalDeviceLimits limits = {.maxImageDimension2D = 16384,
                                    .maxImageDimension3D = 2048,
                                    .maxImageArrayLayers = 2048};
    CHECK(!vk_image_extent_valid(&limits, 54737, 5265, 1, 0));
    CHECK(vk_image_extent_valid(&limits, 3840, 2160, 1, 0));
    CHECK(vk_image_extent_valid(&limits, 2048, 2048, 256, 0));
}

static void test_device_limits_and_3d(void) {
    VkPhysicalDeviceLimits limits = {.maxImageDimension2D = 4096,
                                    .maxImageDimension3D = 256,
                                    .maxImageArrayLayers = 64};
    CHECK(vk_image_extent_valid(&limits, 4096, 4096, 64, 0));
    CHECK(!vk_image_extent_valid(&limits, 4097, 1, 1, 0));
    CHECK(!vk_image_extent_valid(&limits, 1, 4097, 1, 0));
    CHECK(!vk_image_extent_valid(&limits, 1, 1, 65, 0));
    CHECK(!vk_image_extent_valid(&limits, 0, 1, 1, 0));
    CHECK(!vk_image_extent_valid(&limits, 1, 0, 1, 0));
    CHECK(!vk_image_extent_valid(&limits, 1, 1, 0, 0));
    CHECK(vk_image_extent_valid(&limits, 256, 256, 1, 256));
    CHECK(!vk_image_extent_valid(&limits, 257, 1, 1, 1));
    CHECK(!vk_image_extent_valid(&limits, 1, 257, 1, 1));
    CHECK(!vk_image_extent_valid(&limits, 1, 1, 1, 257));
    CHECK(!vk_image_extent_valid(&limits, 1, 1, 2, 1));
}

static void test_unsupported_format_combination(void) {
    /* R8G8_SRGB has no optimal image support on the test GTX 1050 Ti.
     * A plausible extent is not permission to call vkCreateImage. */
    VkImageCreateInfo request = {.extent = {30721, 29, 1}, .mipLevels = 1,
                                .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT};
    VkImageFormatProperties supported = {.maxExtent = {32768, 32768, 1},
                                        .maxMipLevels = 16, .maxArrayLayers = 2048,
                                        .sampleCounts = VK_SAMPLE_COUNT_1_BIT};
    CHECK(vk_image_format_valid(VK_SUCCESS, &request, &supported));
    CHECK(!vk_image_format_valid(VK_ERROR_FORMAT_NOT_SUPPORTED, &request, &supported));
    CHECK(!vk_image_format_valid(VK_ERROR_OUT_OF_HOST_MEMORY, &request, &supported));
    supported = (VkImageFormatProperties){0};
    CHECK(!vk_image_format_valid(VK_SUCCESS, &request, &supported));
}

static void test_format_specific_limits(void) {
    VkImageCreateInfo request = {.extent = {1024, 512, 1}, .mipLevels = 1,
                                .arrayLayers = 2, .samples = VK_SAMPLE_COUNT_1_BIT};
    VkImageFormatProperties supported = {.maxExtent = {1024, 1024, 1},
                                        .maxMipLevels = 1, .maxArrayLayers = 2,
                                        .sampleCounts = VK_SAMPLE_COUNT_1_BIT};
    CHECK(vk_image_format_valid(VK_SUCCESS, &request, &supported));
    request.extent.width++;
    CHECK(!vk_image_format_valid(VK_SUCCESS, &request, &supported));
    request.extent.width--;
    request.arrayLayers++;
    CHECK(!vk_image_format_valid(VK_SUCCESS, &request, &supported));
    request.arrayLayers--;
    request.mipLevels++;
    CHECK(!vk_image_format_valid(VK_SUCCESS, &request, &supported));
    request.mipLevels--;
    request.samples = VK_SAMPLE_COUNT_4_BIT;
    CHECK(!vk_image_format_valid(VK_SUCCESS, &request, &supported));
}

static void test_depth_clear_stage_selection(void) {
    CHECK(vk_depth_stencil_clear_only(1, 0xcc0010, 0));
    CHECK(vk_depth_stencil_clear_only(1, 0xcc0000, 0xF));
    CHECK(!vk_depth_stencil_clear_only(0, 0xcc0010, 0));
    CHECK(!vk_depth_stencil_clear_only(1, 0xcc0010, 0xF)); /* simultaneous color draw retains PS */
    CHECK(vk_depth_stencil_clear_only(2, 0xcc0010, 0)); /* stencil clear also ignores stale PS */
    CHECK(!vk_depth_stencil_clear_only(4, 0xcc0010, 0)); /* neither clear bit */
}

static VkPhysicalDeviceLimits descriptor_limits(void) {
    return (VkPhysicalDeviceLimits){.maxColorAttachments = 8, .maxPerStageResources = 200,
        .maxPerStageDescriptorStorageBuffers = 200, .maxPerStageDescriptorStorageImages = 64,
        .maxPerStageDescriptorSampledImages = 128, .maxPerStageDescriptorSamplers = 128,
        .maxDescriptorSetStorageBuffers = 512, .maxDescriptorSetStorageImages = 128,
        .maxDescriptorSetSampledImages = 256, .maxDescriptorSetSamplers = 256,
        .maxFragmentCombinedOutputResources = 200};
}

static void test_gta_descriptor_budget(void) {
    VkPhysicalDeviceLimits limits = descriptor_limits();
    uint8_t types[138], stages[138];
    for (uint32_t i = 0; i < 138; i++) {
        /* uniforms + 105 scalar buffers + 32 images from the scene shader */
        types[i] = i < 106 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        stages[i] = 1;
    }
    CHECK(!vk_pipeline_descriptor_error(&limits, types, stages, 138, 4));
    limits.maxPerStageResources = 141; /* descriptors + four framebuffer color attachments */
    CHECK(!strcmp(vk_pipeline_descriptor_error(&limits, types, stages, 138, 4), "maxPerStageResources"));
    limits.maxPerStageResources = 142;
    CHECK(!vk_pipeline_descriptor_error(&limits, types, stages, 138, 4));
    limits.maxPerStageDescriptorStorageBuffers = 105; /* the uniforms binding also consumes one */
    CHECK(!strcmp(vk_pipeline_descriptor_error(&limits, types, stages, 138, 4), "maxPerStageDescriptorStorageBuffers"));
}

static void test_descriptor_stage_and_pipeline_limits(void) {
    VkPhysicalDeviceLimits limits = descriptor_limits();
    uint8_t types[200], stages[200];
    for (uint32_t i = 0; i < 200; i++) {
        types[i] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        stages[i] = i < 100 ? 0 : 1;
    }
    limits.maxPerStageDescriptorStorageBuffers = 100;
    limits.maxDescriptorSetStorageBuffers = 200;
    CHECK(!vk_pipeline_descriptor_error(&limits, types, stages, 200, 0));
    limits.maxDescriptorSetStorageBuffers = 199;
    CHECK(!strcmp(vk_pipeline_descriptor_error(&limits, types, stages, 200, 0), "maxDescriptorSetStorageBuffers"));
    limits.maxDescriptorSetStorageBuffers = 200;
    stages[99] = 1;
    CHECK(!strcmp(vk_pipeline_descriptor_error(&limits, types, stages, 200, 0), "maxPerStageDescriptorStorageBuffers"));
}

static void test_combined_samplers_and_fragment_outputs(void) {
    VkPhysicalDeviceLimits limits = descriptor_limits();
    const uint8_t types[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    const uint8_t stages[] = {1, 1, 1, 1};
    limits.maxPerStageResources = 8; /* four descriptors + four color attachments, not nine */
    limits.maxFragmentCombinedOutputResources = 7; /* two buffers + image + four colors */
    CHECK(!vk_pipeline_descriptor_error(&limits, types, stages, 4, 4));
    limits.maxFragmentCombinedOutputResources = 6;
    CHECK(!strcmp(vk_pipeline_descriptor_error(&limits, types, stages, 4, 4), "maxFragmentCombinedOutputResources"));
    limits.maxFragmentCombinedOutputResources = 7;
    limits.maxPerStageDescriptorSamplers = 0;
    CHECK(!strcmp(vk_pipeline_descriptor_error(&limits, types, stages, 4, 4), "maxPerStageDescriptorSamplers"));
    limits.maxPerStageDescriptorSamplers = 1;
    limits.maxPerStageDescriptorSampledImages = 0;
    CHECK(!strcmp(vk_pipeline_descriptor_error(&limits, types, stages, 4, 4), "maxPerStageDescriptorSampledImages"));
    limits.maxPerStageDescriptorSampledImages = 1;
    limits.maxDescriptorSetSamplers = 0;
    CHECK(!strcmp(vk_pipeline_descriptor_error(&limits, types, stages, 4, 4), "maxDescriptorSetSamplers"));
    limits.maxDescriptorSetSamplers = 1;
    limits.maxDescriptorSetSampledImages = 0;
    CHECK(!strcmp(vk_pipeline_descriptor_error(&limits, types, stages, 4, 4), "maxDescriptorSetSampledImages"));
    limits.maxDescriptorSetSampledImages = 1;
    limits.maxPerStageDescriptorStorageImages = 0;
    CHECK(!strcmp(vk_pipeline_descriptor_error(&limits, types, stages, 4, 4), "maxPerStageDescriptorStorageImages"));
    limits.maxPerStageDescriptorStorageImages = 1;
    limits.maxDescriptorSetStorageImages = 0;
    CHECK(!strcmp(vk_pipeline_descriptor_error(&limits, types, stages, 4, 4), "maxDescriptorSetStorageImages"));
}

int main(void) {
    const test_case tests[] = {TEST_CASE(test_invalid_skyrim_extent),
                               TEST_CASE(test_device_limits_and_3d),
                               TEST_CASE(test_unsupported_format_combination),
                               TEST_CASE(test_format_specific_limits),
                               TEST_CASE(test_depth_clear_stage_selection),
                               TEST_CASE(test_gta_descriptor_budget),
                               TEST_CASE(test_descriptor_stage_and_pipeline_limits),
                               TEST_CASE(test_combined_samplers_and_fragment_outputs)};
    return test_run_all("resource_limits", tests, sizeof(tests) / sizeof(tests[0]));
}
