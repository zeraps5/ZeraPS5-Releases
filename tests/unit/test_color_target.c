/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/vk/vk_color_target.h"
#include "test.h"
#include "gpu/gpu_regs.h"
#include "gpu/vk/vk_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t float_bits(float value) { uint32_t bits; memcpy(&bits, &value, 4); return bits; }

static void test_3d_slice_gpu(void) {
    if (!getenv("PS5C_TEST_GPU")) return;
    const uint32_t vs[] = {
        0x36000a81, /* v_and_b32 v0, 1, v5 (vertex id) */
        0x2c020a81, /* v_lshrrev_b32 v1, 1, v5 */
        0x36020281, /* v_and_b32 v1, 1, v1 */
        0x7e000d00, 0x7e020d01, /* convert coordinates to float */
        0x420000f6, 0xbf800000, 0x420202f6, 0xbf800000, /* 4*x-1, 4*y-1 */
        0x7e040280, 0x7e0602f2, /* z=0, w=1 */
        0xf80008cf, 0x03020100, 0xbf810000
    };
    const uint32_t ps[] = {0x7e000200, 0x7e020201, 0x7e040202, 0x7e0602f2,
                           0xf800080f, 0x03020100, 0xbf810000};
    CHECK(pal_init() == PS5C_OK);
    gmem memory; gmem_config config = gmem_default_config(); config.identity = false;
    CHECK(gmem_init(&memory, &config) == PS5C_OK);
    guest_addr address;
    CHECK(gmem_map(&memory, 0, 4 * GUEST_PAGE_SIZE, GPROT_RW, 0, "3D target fixture", &address) == PS5C_OK);
    CHECK(gmem_write(&memory, address, vs, sizeof(vs)));
    CHECK(gmem_write(&memory, address + 0x100, ps, sizeof(ps)));
    pal_window *window = NULL;
    CHECK(pal_window_open("3D color slice regression", 320, 180, &window) == PS5C_OK);
    renderer *render = renderer_create(window, &memory); CHECK(render);
    vk_image *initial = vk_image_get_3d(render, address + 0x2000, 4, 4, 3, VK_FORMAT_R32G32B32A32_SFLOAT);
    CHECK(initial);
    VkCommandBuffer clear_command = vk_begin(render);
    vk_barrier(clear_command, initial, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue clear = {{0, 0, 0, 0}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(clear_command, initial->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    gpu state; CHECK(gpu_init(&state, &memory, NULL) == PS5C_OK);
    state.regs[mmSPI_SHADER_PGM_LO_ES] = (uint32_t)(address >> 8);
    state.regs[mmSPI_SHADER_PGM_HI_ES] = (uint32_t)(address >> 40);
    state.regs[mmSPI_SHADER_PGM_LO_PS] = (uint32_t)((address + 0x100) >> 8);
    state.regs[mmSPI_SHADER_PGM_HI_PS] = (uint32_t)(address >> 40);
    state.regs[mmSPI_SHADER_PGM_RSRC2_PS] = 3 << 1;
    state.regs[mmCB_COLOR0_BASE] = (uint32_t)((address + 0x2000) >> 8);
    state.regs[mmCB_COLOR0_INFO] = (14 << 2) | (7 << 8); /* RGBA32 float */
    state.regs[mmCB_COLOR0_ATTRIB2] = (3 << 14) | 3; /* 4x4 */
    state.regs[mmCB_COLOR0_ATTRIB3] = (2 << 24) | 2; /* 3D, three slices */
    state.regs[mmCB_COLOR_CONTROL] = 0x10;
    state.regs[mmCB_TARGET_MASK] = 15;
    state.regs[mmVGT_PRIMITIVE_TYPE] = 4;
    state.regs[mmPA_CL_VTE_CNTL] = 15;
    state.regs[mmPA_CL_VPORT_XSCALE] = state.regs[mmPA_CL_VPORT_XOFFSET] = float_bits(2);
    state.regs[mmPA_CL_VPORT_YSCALE] = state.regs[mmPA_CL_VPORT_YOFFSET] = float_bits(2);
    gpu_draw draw = {.count = 3, .instances = 1};
    for (uint32_t slice = 1; slice < 3; slice++) {
        renderer_submit_begin(render);
        state.regs[mmCB_COLOR0_VIEW] = slice | (slice << 13);
        state.regs[mmSPI_SHADER_USER_DATA_PS_0] = float_bits(slice == 1 ? 1 : 0);
        state.regs[mmSPI_SHADER_USER_DATA_PS_0 + 1] = float_bits(slice == 2 ? 1 : 0);
        renderer_draw(render, &state, &draw);
    }
    renderer_wait_idle(render);
    vk_image *image = vk_image_find_3d(render, address + 0x2000, 4, 4, 3, VK_FORMAT_R32G32B32A32_SFLOAT);
    CHECK(image && image->gpu_written);
    vk_buffer result; CHECK(vk_buffer_create(render, 4 * 4 * 3 * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &result));
    VkCommandBuffer command = vk_begin(render);
    vk_barrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = {0}; copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1; copy.imageExtent = (VkExtent3D){4, 4, 3};
    vkCmdCopyImageToBuffer(command, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, result.buffer, 1, &copy);
    vk_flush(render); CHECK(vkQueueWaitIdle(render->queue) == VK_SUCCESS);
    const float *pixels = result.mapped;
    for (uint32_t slice = 0; slice < 3; slice++)
        for (uint32_t pixel = 0; pixel < 16; pixel++) {
            const float *rgba = pixels + (slice * 16 + pixel) * 4;
            CHECK(rgba[0] == (slice == 1 ? 1.0f : 0.0f));
            CHECK(rgba[1] == (slice == 2 ? 1.0f : 0.0f));
            CHECK(rgba[2] == 0.0f);
        }
    vk_buffer_destroy(render, &result); renderer_destroy(render);
    free(state.regs); pal_mutex_destroy(state.lock); pal_window_destroy(window);
    gmem_free(&memory); pal_shutdown();
}

/* A COLOR_10_11_11 float target keeps its three channels apart (Dragon Ball Sparking Zero's
 * HDR scene and bloom are this format). */
static float uf_to_float(uint32_t v, uint32_t mbits) {
    uint32_t e = (v >> mbits) & 31, m = v & ((1u << mbits) - 1);
    if (!e) return (float)m / (float)(1u << (mbits + 14));
    float f = 1.0f + (float)m / (float)(1u << mbits);
    for (int i = 15; i < (int)e; i++) f *= 2.0f;
    for (int i = (int)e; i < 15; i++) f *= 0.5f;
    return f;
}
static void test_r11g11b10_gpu(void) {
    if (!getenv("PS5C_TEST_GPU")) return;
    const uint32_t vs[] = {
        0x36000a81, 0x2c020a81, 0x36020281, 0x7e000d00, 0x7e020d01,
        0x420000f6, 0xbf800000, 0x420202f6, 0xbf800000, 0x7e040280, 0x7e0602f2,
        0xf80008cf, 0x03020100, 0xbf810000
    };
    const uint32_t ps[] = {0x7e000200, 0x7e020201, 0x7e040202, 0x7e0602f2,
                           0xf800080f, 0x03020100, 0xbf810000};
    CHECK(pal_init() == PS5C_OK);
    gmem memory; gmem_config config = gmem_default_config(); config.identity = false;
    CHECK(gmem_init(&memory, &config) == PS5C_OK);
    guest_addr address;
    CHECK(gmem_map(&memory, 0, 4 * GUEST_PAGE_SIZE, GPROT_RW, 0, "R11G11B10 target fixture", &address) == PS5C_OK);
    CHECK(gmem_write(&memory, address, vs, sizeof(vs)));
    CHECK(gmem_write(&memory, address + 0x100, ps, sizeof(ps)));
    pal_window *window = NULL;
    CHECK(pal_window_open("R11G11B10 target regression", 320, 180, &window) == PS5C_OK);
    renderer *render = renderer_create(window, &memory); CHECK(render);
    gpu state; CHECK(gpu_init(&state, &memory, NULL) == PS5C_OK);
    state.regs[mmSPI_SHADER_PGM_LO_ES] = (uint32_t)(address >> 8);
    state.regs[mmSPI_SHADER_PGM_HI_ES] = (uint32_t)(address >> 40);
    state.regs[mmSPI_SHADER_PGM_LO_PS] = (uint32_t)((address + 0x100) >> 8);
    state.regs[mmSPI_SHADER_PGM_HI_PS] = (uint32_t)(address >> 40);
    state.regs[mmSPI_SHADER_PGM_RSRC2_PS] = 3 << 1;
    state.regs[mmCB_COLOR0_BASE] = (uint32_t)((address + 0x2000) >> 8);
    state.regs[mmCB_COLOR0_INFO] = (6 << 2) | (7 << 8); /* COLOR_10_11_11, float */
    state.regs[mmCB_COLOR0_ATTRIB2] = (3 << 14) | 3;    /* 4x4 */
    state.regs[mmCB_COLOR_CONTROL] = 0x10;
    state.regs[mmCB_TARGET_MASK] = 15;
    state.regs[mmVGT_PRIMITIVE_TYPE] = 4;
    state.regs[mmPA_CL_VTE_CNTL] = 15;
    state.regs[mmPA_CL_VPORT_XSCALE] = state.regs[mmPA_CL_VPORT_XOFFSET] = float_bits(2);
    state.regs[mmPA_CL_VPORT_YSCALE] = state.regs[mmPA_CL_VPORT_YOFFSET] = float_bits(2);
    state.regs[mmSPI_SHADER_USER_DATA_PS_0] = float_bits(1.0f);
    state.regs[mmSPI_SHADER_USER_DATA_PS_0 + 1] = float_bits(0.5f);
    state.regs[mmSPI_SHADER_USER_DATA_PS_0 + 2] = float_bits(0.25f);
    gpu_draw draw = {.count = 3, .instances = 1};
    renderer_submit_begin(render);
    renderer_draw(render, &state, &draw);
    renderer_wait_idle(render);
    vk_image *image = vk_image_find(render, address + 0x2000, 4, 4, VK_FORMAT_B10G11R11_UFLOAT_PACK32);
    CHECK(image && image->gpu_written);
    vk_buffer result; CHECK(vk_buffer_create(render, 4 * 4 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &result));
    VkCommandBuffer command = vk_begin(render);
    vk_barrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = {0}; copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1; copy.imageExtent = (VkExtent3D){4, 4, 1};
    vkCmdCopyImageToBuffer(command, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, result.buffer, 1, &copy);
    vk_flush(render); CHECK(vkQueueWaitIdle(render->queue) == VK_SUCCESS);
    const uint32_t *pixels = result.mapped;
    uint32_t p = pixels[0];
    float r = uf_to_float(p & 0x7FF, 6), g = uf_to_float((p >> 11) & 0x7FF, 6), b = uf_to_float(p >> 22, 5);
    printf("R11G11B10 target: %08x -> %g %g %g\n", p, r, g, b);
    CHECK(r == 1.0f && g == 0.5f && b == 0.25f);
    vk_buffer_destroy(render, &result); renderer_destroy(render);
    free(state.regs); pal_mutex_destroy(state.lock); pal_window_destroy(window);
    gmem_free(&memory); pal_shutdown();
}

static void test_reflection_pyramid(void) {
    const uint32_t views[] = {0, 0x04000000, 0x08000000, 0x0c000000, 0x10000000};
    for (uint32_t i = 0; i < 5; i++) {
        CHECK(vk_color_mip(views[i]) == i);
        CHECK(vk_color_mip_size(1024, vk_color_mip(views[i])) == (1024u >> i));
        CHECK(vk_color_mip_size(512, vk_color_mip(views[i])) == (512u >> i));
    }
}
static void test_small_and_array_views(void) {
    CHECK(vk_color_mip(0xc3ffffff) == 0); /* layer/other fields */
    CHECK(vk_color_mip(0xffffffff) == 15);
    CHECK(vk_color_mip_size(101, 2) == 25);
    CHECK(vk_color_mip_size(3, 3) == 1);
    CHECK(vk_color_mip_size(1, 15) == 1);
}
int main(void) {
    const test_case tests[] = {TEST_CASE(test_reflection_pyramid), TEST_CASE(test_small_and_array_views), TEST_CASE(test_3d_slice_gpu), TEST_CASE(test_r11g11b10_gpu)};
    return test_run_all("color_target", tests, sizeof(tests) / sizeof(tests[0]));
}
