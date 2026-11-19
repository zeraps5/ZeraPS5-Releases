/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "base/base.h"
#include <stdlib.h>
#include <string.h>
#include "gpu/vk/vk_api.h"
#include "gpu/vk/vk_image_formats.h"
#include "gpu/vk/vk_internal.h"
#include "gpu/vk/vk_resource_limits.h"
#include "gpu/vk/vk_depth_policy.h"
#include "gpu/vk/vk_spirv_interface.h"
#include "gpu/vk/vk_texture_tiling.h"
#include "gpu/gpu_regs.h"
#include "test.h"

/* Graphics fixtures request ROP3 COPY explicitly. ROP3=0 means CLEAR once
 * color logic operations are supported, even when blending is disabled. */

static void test_cached_spirv_locations(void) {
    /* Failure shape: PS requires location 1, VS exports 0. Decorations precede
     * variables; a PS OUTPUT is not a fragment INPUT. */
    uint32_t code[] = {0x07230203,0x10300,0,8,0,
        (4u<<16)|SpvOpDecorate,2,SpvDecorationLocation,0,
        (4u<<16)|SpvOpDecorate,3,SpvDecorationLocation,1,
        (4u<<16)|SpvOpVariable,1,2,SpvStorageClassOutput,
        (4u<<16)|SpvOpVariable,1,3,SpvStorageClassInput};
    uint32_t output = 0, input = 0;
    CHECK(vk_spirv_locations(code,PS5C_ARRAY_COUNT(code),SpvStorageClassOutput,&output) && output==1);
    CHECK(vk_spirv_locations(code,PS5C_ARRAY_COUNT(code),SpvStorageClassInput,&input) && input==2);
    CHECK((input & ~output)==2);
    code[12]=0;
    CHECK(vk_spirv_locations(code,PS5C_ARRAY_COUNT(code),SpvStorageClassInput,&input) && input==1);
    CHECK(!(input & ~output));
    code[5]=0; CHECK(!vk_spirv_locations(code,PS5C_ARRAY_COUNT(code),SpvStorageClassInput,&input));
    code[5]=(4u<<16)|SpvOpDecorate;
    code[6]=8; CHECK(!vk_spirv_locations(code,PS5C_ARRAY_COUNT(code),SpvStorageClassInput,&input));
    code[6]=2;
    CHECK(!vk_spirv_locations(code,PS5C_ARRAY_COUNT(code)-1,SpvStorageClassInput,&input));
}

static VkFormat view(VkFormat image, VkFormat requested, bool store, uint32_t kind) {
    VkFormat allowed[VK_IMAGE_ALIAS_CAPACITY];
    uint32_t n = vk_image_alias_formats(image, 4, allowed);
    return vk_image_descriptor_view_format(image, requested, store, kind, allowed, n);
}

static void test_constant_depth_bias_policy(void) {
    sh_program *vs=calloc(1,sizeof(*vs)),*ps=calloc(1,sizeof(*ps));CHECK(vs && ps);
    CHECK(!vk_vertex_depth_is_constant(vs,ps));
    vs->count=1;rdna2_inst *in=&vs->insts[0];
    in->enc=RDNA2_ENC_EXP;in->exp_target=12;in->exp_en=15;in->exp_src[2]=in->exp_src[3]=7;
    CHECK(vk_vertex_depth_is_constant(vs,ps));
    in->exp_src[2]=6;CHECK(!vk_vertex_depth_is_constant(vs,ps));in->exp_src[2]=7;
    in->exp_en=11;CHECK(!vk_vertex_depth_is_constant(vs,ps));in->exp_en=15;
    in->exp_compr=true;CHECK(!vk_vertex_depth_is_constant(vs,ps));in->exp_compr=false;
    in->exp_target=0;CHECK(!vk_vertex_depth_is_constant(vs,ps));in->exp_target=12;
    vs->count=2;vs->insts[1]=*in;CHECK(vk_vertex_depth_is_constant(vs,ps));
    vs->count=2;vs->insts[1]=*in;vs->insts[1].exp_src[2]=6;
    CHECK(!vk_vertex_depth_is_constant(vs,ps));vs->count=1;
    float delta=0;
    CHECK(vk_far_plane_bias(vs,ps,1,0,-9,0,&delta) && delta==-9.f*0x1p-23f);
    CHECK(!vk_far_plane_bias(vs,ps,1,0,9,0,&delta));
    CHECK(!vk_far_plane_bias(vs,ps,.5f,0,-9,0,&delta));
    CHECK(!vk_far_plane_bias(vs,ps,1,.1f,-9,0,&delta));
    CHECK(!vk_far_plane_bias(vs,ps,1,0,-INFINITY,0,&delta));
    CHECK(vk_far_plane_bias(vs,ps,1,0,-9,-0x1p-24f,&delta) && delta==-0x1p-24f);
    ps->mrt_mask=1u<<8;CHECK(!vk_vertex_depth_is_constant(vs,ps));
    CHECK(!vk_far_plane_bias(vs,ps,1,0,-9,0,&delta));
    free(vs);free(ps);
}

static unsigned tracked_barriers;
static void test_opaque_depth_policy(void) {
    CHECK(vk_opaque_depth_only(0,0,15));
    CHECK(vk_opaque_depth_only(0,0x10,0));
    CHECK(vk_opaque_depth_only(0x830,0,15)); /* Z order and alpha-to-mask disable */
    CHECK(!vk_opaque_depth_only(0,0x10,15));
    for(unsigned bit=0;bit<=10;bit++)if((1u<<bit)&0x7C7u)
        CHECK(!vk_opaque_depth_only(1u<<bit,0,0));
}
static VkPipelineStageFlags tracked_src,tracked_dst;
static VkImageMemoryBarrier tracked_image;
static VKAPI_ATTR void VKAPI_CALL capture_image_barrier(VkCommandBuffer cmd,VkPipelineStageFlags src,
    VkPipelineStageFlags dst,VkDependencyFlags flags,uint32_t memories,const VkMemoryBarrier *memory,
    uint32_t buffers,const VkBufferMemoryBarrier *buffer,uint32_t images,const VkImageMemoryBarrier *image) {
    (void)cmd;(void)flags;(void)memories;(void)memory;(void)buffers;(void)buffer;
    tracked_barriers++;tracked_src=src;tracked_dst=dst;
    if(images==1)tracked_image=*image;
}
static void test_reads_before_image_overwrite(void) {
    /* A later write must wait for EVERY preceding reader, including those
     * that needed no layout transition. No extra barrier between readers. */
    vk_image image={0};
    image.layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image.access=VK_ACCESS_SHADER_READ_BIT;
    image.stage=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    image.aspect=VK_IMAGE_ASPECT_COLOR_BIT;
    PFN_vkCmdPipelineBarrier saved=vkCmdPipelineBarrier;
    vkCmdPipelineBarrier=capture_image_barrier;
    tracked_barriers=0;
    vk_barrier(VK_NULL_HANDLE,&image,image.layout,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vk_barrier(VK_NULL_HANDLE,&image,image.layout,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
    unsigned reader_barriers=tracked_barriers;
    vk_barrier(VK_NULL_HANDLE,&image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdPipelineBarrier=saved;
    CHECK(reader_barriers==0 && tracked_barriers==1);
    CHECK((tracked_src&(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_VERTEX_SHADER_BIT))==
          (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_VERTEX_SHADER_BIT));
    CHECK(tracked_dst==VK_PIPELINE_STAGE_TRANSFER_BIT);
    CHECK(tracked_image.srcAccessMask==VK_ACCESS_SHADER_READ_BIT && tracked_image.dstAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT);
    CHECK(tracked_image.oldLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && tracked_image.newLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}

static void test_gta_bgra_bytes_before_swizzle(void) {
    /* A BGRA target with shader RGB=(32,128,224) stores bytes (224,128,32).
     * T# RGBA reads those bytes, then DST_SEL B,G,R returns (32,128,224).
     * A BGRA view followed by the same DST_SEL swaps the channels twice. */
    CHECK(view(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, false, 0) == VK_FORMAT_R8G8B8A8_UNORM);
}

static void test_rgba_number_interpretation(void) {
    CHECK(view(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, false, 0) == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(view(VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, false, 0) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(view(VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UNORM, false, 0) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(view(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT, false, 1) == VK_FORMAT_R8G8B8A8_UINT);
}

static void test_uv_number_interpretation(void) {
    VkFormat allowed[VK_IMAGE_ALIAS_CAPACITY+1]={0};
    allowed[VK_IMAGE_ALIAS_CAPACITY]=VK_FORMAT_D32_SFLOAT;
    uint32_t n=vk_image_alias_formats(VK_FORMAT_R8G8_UINT,2,allowed);
    CHECK(n<=VK_IMAGE_ALIAS_CAPACITY && allowed[VK_IMAGE_ALIAS_CAPACITY]==VK_FORMAT_D32_SFLOAT);
    CHECK(vk_image_descriptor_view_format(VK_FORMAT_R8G8_UINT,VK_FORMAT_R8G8_UNORM,false,0,allowed,n)==VK_FORMAT_R8G8_UNORM);
    for(uint32_t i=0;i<n;i++)for(uint32_t j=0;j<i;j++)CHECK(allowed[i]!=allowed[j]);
    n=vk_image_alias_formats(VK_FORMAT_R8G8_UNORM,2,allowed);
    CHECK(vk_image_descriptor_view_format(VK_FORMAT_R8G8_UNORM,VK_FORMAT_R8G8_UINT,true,1,allowed,n)==VK_FORMAT_R8G8_UINT);
    CHECK(vk_image_descriptor_view_format(VK_FORMAT_R8G8_UNORM,VK_FORMAT_R8G8_SNORM,false,0,allowed,n)==VK_FORMAT_R8G8_SNORM);
}

static void test_alias_list_bounds_and_existing_views(void) {
    const VkFormat bases[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB,
                              VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM,
                              VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_SRGB};
    for (uint32_t k = 0; k < sizeof(bases) / sizeof(bases[0]); k++) {
        VkFormat formats[VK_IMAGE_ALIAS_CAPACITY + 1];
        formats[VK_IMAGE_ALIAS_CAPACITY] = VK_FORMAT_D32_SFLOAT;
        uint32_t n = vk_image_alias_formats(bases[k], 4, formats);
        CHECK(n <= VK_IMAGE_ALIAS_CAPACITY && formats[VK_IMAGE_ALIAS_CAPACITY] == VK_FORMAT_D32_SFLOAT);
        CHECK(formats[0] == bases[k]);
        const VkFormat requested[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM,
                                       VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_SRGB};
        for (uint32_t i = 0; i < sizeof(requested) / sizeof(requested[0]); i++)
            CHECK(view(bases[k], requested[i], false, requested[i] == VK_FORMAT_R8G8B8A8_UINT ? 1 : requested[i] == VK_FORMAT_R8G8B8A8_SINT ? 2 : 0) == requested[i]);
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t j = 0; j < i; j++) CHECK(formats[i] != formats[j]);
    }
    CHECK(view(VK_FORMAT_R32_UINT, VK_FORMAT_R32_SFLOAT, false, 0) == VK_FORMAT_R32_SFLOAT);
    CHECK(view(VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, true, 1) == VK_FORMAT_R32_UINT);
    CHECK(view(VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, false, 2) == VK_FORMAT_R32_SINT);
    CHECK(view(VK_FORMAT_R32_SINT, VK_FORMAT_R32_UINT, false, 1) == VK_FORMAT_R32_UINT);
    CHECK(view(VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, false, 0) == VK_FORMAT_R32_SFLOAT);
    CHECK(view(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, false, 0) == VK_FORMAT_R8G8B8A8_UNORM);
}

/* A GPU-only red surface must survive churn of CPU-recreatable textures.
 * Run manually with PS5C_TEST_GPU=1; this fixture owns its backing budget. */
static void test_gpu_residency(void) {
    CHECK(pal_init() == PS5C_OK);
    gmem m;
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 8 * GUEST_PAGE_SIZE, GPROT_RW, 0, "image residency", &a) == PS5C_OK);
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 GPU image residency", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m);
    CHECK(r && r->device);
    CHECK(r->image_count == 0);
    r->image_limit = 64;
    if (!r->image_backing_limit) r->image_backing_limit = 16u * 1024u * 1024u;
    vk_begin(r);
    const struct {
        uint32_t width, height, layers, depth, bpp;
        VkFormat format;
        uint8_t texel[8];
    } fixtures[] = {{1, 1, 1, 0, 4, VK_FORMAT_R8G8B8A8_UNORM, {255, 0, 0, 255}},
                    {2, 2, 6, 0, 4, VK_FORMAT_R8G8B8A8_UNORM, {255, 0, 0, 255}},
                    {2, 2, 1, 3, 4, VK_FORMAT_R8G8B8A8_UNORM, {255, 0, 0, 255}},
                    {2, 2, 1, 0, 8, VK_FORMAT_R16G16B16A16_SFLOAT, {0, 60, 0, 0, 0, 0, 0, 60}},
                    {2, 2, 1, 0, 4, VK_FORMAT_B10G11R11_UFLOAT_PACK32, {192, 3, 0, 0}},
                    {2, 2, 1, 0, 1, VK_FORMAT_R8_UINT, {173}},
                    {2, 2, 1, 0, 4, VK_FORMAT_R8G8B8A8_UINT, {19, 31, 47, 63}},
                    {2, 2, 1, 0, 4, VK_FORMAT_R32_UINT, {0x78, 0x56, 0x34, 0x12}}};
    VkClearColorValue color = {{1.0f, 0.0f, 0.0f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (unsigned k = 0; k < PS5C_ARRAY_COUNT(fixtures); k++) {
        vk_image *img =
            fixtures[k].depth
                ? vk_image_get_3d(r, a + k * 0x100, fixtures[k].width, fixtures[k].height,
                                  fixtures[k].depth, fixtures[k].format)
                : vk_image_get_layers(r, a + k * 0x100, fixtures[k].width, fixtures[k].height,
                                      fixtures[k].format, fixtures[k].layers);
        CHECK(img);
        vk_barrier(r->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
        range.layerCount = img->depth ? 1 : img->layers;
        VkClearColorValue value = color;
        if (k == 5)
            value.uint32[0] = 173;
        if (k == 6) {
            value.uint32[0] = 19;
            value.uint32[1] = 31;
            value.uint32[2] = 47;
            value.uint32[3] = 63;
        }
        if (k == 7)
            value.uint32[0] = 0x12345678;
        vkCmdClearColorImage(r->cmd, img->image, img->layout, &value, 1, &range);
        img->gpu_written = true;
        img->written_seq = ++r->write_seq;
    }
    range.layerCount = 1;
    vk_flush(r);
    for (unsigned batch = 0; batch < 4; batch++) {
        vk_begin(r);
        for (unsigned i = 0; i < 56; i++) {
            guest_addr address = a + 0x10000 + (batch * 56 + i) * 0x100;
            vk_image *texture = vk_image_get(r, address, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
            CHECK(texture);
            if (!batch) { /* force the next batch to spill GPU-only contents */
                vk_barrier(r->cmd, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                vkCmdClearColorImage(r->cmd, texture->image, texture->layout, &color, 1, &range);
                texture->gpu_written = true;
                texture->written_seq = ++r->write_seq;
            }
        }
        vk_flush(r);
    }
    for (unsigned k = 0; k < PS5C_ARRAY_COUNT(fixtures); k++) {
        vk_begin(r);
        if (k == 5 || k == 7) {
            /* Reject a different texel size and an unlisted same-size alias.
             * Neither request may consume the original backing record. */
            vk_image *wrong = vk_image_get(r, a + k * 0x100, 2, 2, VK_FORMAT_R8G8B8A8_UNORM);
            CHECK(wrong && !wrong->gpu_written);
        }
        /* Spill in the writer's integer format, restore through the sampler's
         * compatible normalized format. There is no CPU copy to reconstruct. */
        VkFormat restored = k == 5   ? VK_FORMAT_R8_UNORM
                            : k == 6 ? VK_FORMAT_R8G8B8A8_UNORM
                                     : fixtures[k].format;
        vk_image *img =
            fixtures[k].depth
                ? vk_image_get_3d(r, a + k * 0x100, fixtures[k].width, fixtures[k].height,
                                  fixtures[k].depth, fixtures[k].format)
                : vk_image_get_layers(r, a + k * 0x100, fixtures[k].width, fixtures[k].height,
                                      restored, fixtures[k].layers);
        CHECK(img && img->gpu_written);
        size_t texels =
            fixtures[k].width * fixtures[k].height * (img->depth ? img->depth : img->layers);
        vk_buffer readback = {0};
        CHECK(vk_buffer_create(r, texels * fixtures[k].bpp, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &readback));
        vk_barrier(r->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {0};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = img->depth ? 1 : img->layers;
        copy.imageExtent = (VkExtent3D){img->width, img->height, img->depth ? img->depth : 1};
        vkCmdCopyImageToBuffer(r->cmd, img->image, img->layout, readback.buffer, 1, &copy);
        vk_flush(r);
        CHECK(vkQueueWaitIdle(r->queue) == VK_SUCCESS);
        for (size_t i = 0; i < texels; i++)
            CHECK(memcmp((uint8_t *)readback.mapped + i * fixtures[k].bpp, fixtures[k].texel, fixtures[k].bpp) == 0);
        vk_buffer_destroy(r, &readback);
    }
    /* At capacity with GPU-only contents in every slot, ask the caller for
     * pressure recovery instead of silently destroying an image. */
    vk_begin(r);
    for (uint32_t i = 0; i < r->image_count; i++) {
        vk_image *img = &r->images[i];
        if (img->addr == a) continue;
        vk_barrier(r->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        range.layerCount = img->depth ? 1 : img->layers;
        vkCmdClearColorImage(r->cmd, img->image, img->layout, &color, 1, &range);
        img->gpu_written = true;
        img->written_seq = ++r->write_seq;
        img->last_use = r->use_seq;
    }
    vk_flush(r);
    vk_begin(r);
    uint64_t budget = r->image_backing_limit;
    r->image_backing_limit = 1; /* insufficient for one RGBA8 texel */
    CHECK(!vk_image_get(r, a + 0x30000, 1, 1, VK_FORMAT_R8G8B8A8_UNORM));
    CHECK(r->image_cache_pressure && r->image_count == 64);
    CHECK(vk_image_find(r, a, 1, 1, VK_FORMAT_R8G8B8A8_UNORM));
    r->image_backing_limit = budget;
    CHECK(vk_image_get(r, a + 0x30000, 1, 1, VK_FORMAT_R8G8B8A8_UNORM));
    CHECK(r->image_spills >= 6 && r->image_restores >= 5 && r->image_backing_bytes <= budget);
    vk_flush(r);
    renderer_destroy(r);
    pal_window_destroy(window);
    gmem_free(&m);
    pal_shutdown();
    puts("2D, array, 3D, HDR and integer-to-normalized images survive 224 allocations across four batches (64 slots)");
    puts("RAM budget exhaustion preserves contents; sufficient budget lets a full GPU-written cache advance");
}

/* A compute writer and a sampler may reinterpret one BGRA image as RGBA.
 * The view usage restriction must not be shared between the two bindings. */
static void test_gpu_view_usage(void) {
    static const uint32_t store[] = {
        0x7e000280, 0x7e020280,
        0x7e0802ff, 0x3e800000, 0x7e0a02ff, 0x3f000000,
        0x7e0c02ff, 0x3f400000, 0x7e0e02f2,
        0xf0200f08, 0x00000400, 0xbf810000
    };
    static const uint32_t load[] = {
        0x7e000280, 0x7e020280, 0xf0000f08, 0x00000400,
        0xbf8c3f70, 0xe0780000, 0x80030400, 0xbf810000
    };
    CHECK(pal_init() == PS5C_OK);
    gmem m; gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 8 * GUEST_PAGE_SIZE, GPROT_RW, 0, "view usage", &a) == PS5C_OK);
    CHECK(gmem_write(&m, a, store, sizeof(store)) && gmem_write(&m, a + 0x100, load, sizeof(load)));
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 image view usage regression", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m); CHECK(r);
    gpu g; CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    guest_addr image = a + 0x4000, out = a + 0x1000;
    vk_begin(r);
    vk_image *img = vk_image_get(r, image, 16, 16, VK_FORMAT_B8G8R8A8_UNORM); CHECK(img);
    /* Storage through RGBA requires a mutable view of the BGRA surface. */
    CHECK(vk_image_promote(r, img));
    uint32_t *u = g.regs + mmCOMPUTE_USER_DATA_0;
    u[0] = (uint32_t)(image >> 8); u[1] = (uint32_t)(image >> 40) | (56u << 20) | (3u << 30);
    u[2] = 3u | (15u << 14); u[3] = (9u << 28) | 0xfacu;
    u[12] = (uint32_t)out; u[13] = (uint32_t)(out >> 32); u[14] = 16; u[15] = 0x00016204;
    g.regs[mmCOMPUTE_PGM_RSRC2] = 16u << 1;
    g.regs[mmCOMPUTE_NUM_THREAD_X] = g.regs[mmCOMPUTE_NUM_THREAD_Y] = g.regs[mmCOMPUTE_NUM_THREAD_Z] = 1;
    g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(a >> 8); g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)(a >> 40);
    if(getenv("PS5C_TEST_COMPUTE_CAPTURE")) {
        char value[32];snprintf(value,sizeof(value),"%llx",(unsigned long long)a);
        CHECK(_putenv_s("PS5C_SAVE_AFTER",value)==0); ps5c_env_reset();
        snprintf(value,sizeof(value),"%llx",(unsigned long long)image);
        CHECK(_putenv_s("PS5C_SAVE_AFTER_IMAGE",value)==0 && _putenv_s("PS5C_SAVE_AFTER_FRAME","0")==0); ps5c_env_reset();
    }
    renderer_dispatch(r, &g, 1, 1, 1);
    CHECK(img->sub_count == 1);
    VkImageView written_view = img->sub[0].view;
    g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)((a + 0x100) >> 8);
    g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)((a + 0x100) >> 40);
    renderer_dispatch(r, &g, 1, 1, 1);
    renderer_sync_mem(r, out, 16);
    CHECK(img->sub_count == 2 && img->sub[1].view != written_view);
    float values[4]; CHECK(gmem_read(&m, out, values, sizeof(values)));
    CHECK(values[0] > 0.24f && values[0] < 0.26f && values[1] > 0.49f && values[1] < 0.51f &&
          values[2] > 0.74f && values[2] < 0.76f && values[3] == 1.0f);
    /* Each usage must reuse its own cached view on subsequent bindings. */
    g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(a >> 8); g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)(a >> 40);
    renderer_dispatch(r, &g, 1, 1, 1);
    g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)((a + 0x100) >> 8);
    g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)((a + 0x100) >> 40);
    renderer_dispatch(r, &g, 1, 1, 1);
    renderer_wait_idle(r);
    CHECK(img->sub_count == 2);
    renderer_destroy(r); free(g.regs); pal_mutex_destroy(g.lock);
    pal_window_destroy(window); gmem_free(&m); pal_shutdown();
    puts("storage then sampling: separate cached views preserve the RGBA texel");
}

/* Exercise the backend's render pass, descriptor and barrier agreement.
 * A degenerate triangle is sufficient: validation checks descriptor layouts
 * even when it produces no fragments. This is not a lighting/pixel test. */
static void test_gpu_depth_sample_attachment(void) {
    static const uint32_t vs[] = {
        0x7e000280, 0x7e020280, 0x7e040280, 0x7e0602f2,
        0xf80008cf, 0x03020100, 0xbf810000
    };
    static const uint32_t ps[] = {
        0x7e000280, 0x7e020280,
        0xf0000f08, 0x00000400, /* image_load v[4:7], v[0:1], s[0:7] */
        0xbf8c3f70, 0xf800180f, 0x07060504, 0xbf810000
    };
    static const uint32_t solid_ps[] = {
        0x7e0002f2, 0x7e020280, 0x7e040280, 0x7e0602f2,
        0xf800180f, 0x03020100, 0xbf810000
    };
    CHECK(pal_init() == PS5C_OK);
    gmem m; gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 8 * GUEST_PAGE_SIZE, GPROT_RW, 0, "depth sampling", &a) == PS5C_OK);
    CHECK(gmem_write(&m, a, vs, sizeof(vs)) && gmem_write(&m, a + 0x100, ps, sizeof(ps)) &&
          gmem_write(&m, a + 0x200, solid_ps, sizeof(solid_ps)));
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 read-only depth regression", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m); CHECK(r);
    gpu g; CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    guest_addr color = a + 0x4000, depth = a + 0x8000;
    g.regs[mmSPI_SHADER_PGM_LO_ES] = (uint32_t)(a >> 8);
    g.regs[mmSPI_SHADER_PGM_HI_ES] = (uint32_t)(a >> 40);
    g.regs[mmSPI_SHADER_PGM_LO_PS] = (uint32_t)((a + 0x100) >> 8);
    g.regs[mmSPI_SHADER_PGM_HI_PS] = (uint32_t)((a + 0x100) >> 40);
    g.regs[mmSPI_SHADER_PGM_RSRC2_PS] = 8u << 1;
    g.regs[mmCB_COLOR_CONTROL] = 0xCC0010;
    g.regs[mmCB_TARGET_MASK] = 15;
    g.regs[mmCB_COLOR0_BASE] = (uint32_t)(color >> 8);
    g.regs[mmCB_COLOR0_INFO] = 10u << 2;
    g.regs[mmCB_COLOR0_ATTRIB2] = (15u << 14) | 15;
    g.regs[mmDB_Z_INFO] = 3;
    g.regs[mmDB_Z_READ_BASE] = g.regs[mmDB_Z_WRITE_BASE] = (uint32_t)(depth >> 8);
    g.regs[mmDB_DEPTH_SIZE_XY] = (15u << 16) | 15;
    g.regs[mmDB_DEPTH_CONTROL] = 2 | (7u << 4); /* test ALWAYS, no writes */
    g.regs[mmVGT_PRIMITIVE_TYPE] = GPU_PRIM_TRILIST;
    uint32_t *u = g.regs + mmSPI_SHADER_USER_DATA_PS_0;
    u[0] = (uint32_t)(depth >> 8); u[1] = (uint32_t)(depth >> 40) | (22u << 20) | (3u << 30);
    u[2] = 3u | (15u << 14); u[3] = (9u << 28) | 0xfacu;
    gpu_draw draw = {.count = 3, .instances = 1};
    for (unsigned round = 0; round < 2; round++) {
        g.regs[mmSPI_SHADER_PGM_LO_PS] = (uint32_t)((a + 0x100) >> 8);
        g.regs[mmDB_DEPTH_CONTROL] = 2 | (7u << 4);
        renderer_draw(r, &g, &draw);
        vk_image *z = vk_image_find(r, depth, 16, 16, VK_FORMAT_D32_SFLOAT); CHECK(z);
        CHECK(z->layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        /* Keep both passes queued: a host fence here would hide the missing
         * dependency on the read-only attachment's storeOp. */
        /* Returning to a depth-writing pass must use a distinct cached
         * render pass/framebuffer and restore the writable layout. */
        g.regs[mmSPI_SHADER_PGM_LO_PS] = (uint32_t)((a + 0x200) >> 8);
        g.regs[mmDB_DEPTH_CONTROL] = 6 | (7u << 4);
        renderer_draw(r, &g, &draw);
        CHECK(z->layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        renderer_wait_idle(r);
    }
    renderer_surface surface={.addr=color,.width=16,.height=16,.pitch=16,.pixel_format=0x8000000000000000ull};
    for(unsigned flip=0;flip<6;flip++)renderer_present(r,&surface);
    renderer_wait_idle(r);
    renderer_destroy(r); free(g.regs); pal_mutex_destroy(g.lock);
    pal_window_destroy(window); gmem_free(&m); pal_shutdown();
}

/* Reaching the sampler cache limit must not silently switch clamp/filter
 * modes. Sample outside a white texture: repeat is white, border is black. */
static unsigned failed_sampler_updates,failed_sampler_dispatches;
static VKAPI_ATTR VkResult VKAPI_CALL fail_sampler(VkDevice device,const VkSamplerCreateInfo *info,
    const VkAllocationCallbacks *allocator,VkSampler *sampler) {
    (void)device;(void)info;(void)allocator;*sampler=VK_NULL_HANDLE;return VK_ERROR_TOO_MANY_OBJECTS;
}
static VKAPI_ATTR void VKAPI_CALL capture_failed_sampler_update(VkDevice device,uint32_t count,
    const VkWriteDescriptorSet *writes,uint32_t copy_count,const VkCopyDescriptorSet *copies) {
    (void)device;(void)count;(void)writes;(void)copy_count;(void)copies;failed_sampler_updates++;
}
static VKAPI_ATTR void VKAPI_CALL capture_failed_sampler_dispatch(VkCommandBuffer cmd,uint32_t x,uint32_t y,uint32_t z) {
    (void)cmd;(void)x;(void)y;(void)z;failed_sampler_dispatches++;
}
static void test_gpu_sampler_churn(void) {
    static const uint32_t sample[] = {
        0x7e0002ff, 0x3fa00000, 0x7e0202ff, 0x3fa00000,
        0xf0800f08, 0x00400400, /* image_sample v[4:7], v[0:1], s[0:7], s[8:11] */
        0xbf8c3f70, 0xe0780000, 0x80030400, 0xbf810000
    };
    CHECK(pal_init() == PS5C_OK);
    gmem m; gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 8 * GUEST_PAGE_SIZE, GPROT_RW, 0, "sampler churn", &a) == PS5C_OK);
    CHECK(gmem_write(&m, a, sample, sizeof(sample)));
    guest_addr image = a + 0x4000, out = a + 0x1000;
    memset(gmem_host(&m, image, 4096), 255, 4096);
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 sampler cache regression", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m); CHECK(r);
    gpu g; CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    uint32_t *u = g.regs + mmCOMPUTE_USER_DATA_0;
    u[0] = (uint32_t)(image >> 8); u[1] = (uint32_t)(image >> 40) | (1u << 20) | (3u << 30);
    u[2] = 3u | (15u << 14); u[3] = (9u << 28) | 0xfacu;
    u[10] = (1u << 20) | (1u << 22);
    u[12] = (uint32_t)out; u[13] = (uint32_t)(out >> 32); u[14] = 16; u[15] = 0x00016204;
    g.regs[mmCOMPUTE_PGM_RSRC2] = 16u << 1;
    g.regs[mmCOMPUTE_NUM_THREAD_X] = g.regs[mmCOMPUTE_NUM_THREAD_Y] = g.regs[mmCOMPUTE_NUM_THREAD_Z] = 1;
    g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(a >> 8); g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)(a >> 40);
    for (uint32_t i = 0; i < 64; i++) {
        u[8] = i;
        renderer_dispatch(r, &g, 1, 1, 1);
        renderer_sync_mem(r, out, 16);
    }
    u[8] = 4u | (4u << 3) | (4u << 6);
    renderer_dispatch(r, &g, 1, 1, 1);
    renderer_sync_mem(r, out, 16);
    float values[4]; CHECK(gmem_read(&m, out, values, sizeof(values)));
    fprintf(stderr, "after 64 sampler keys: border red=%g (expected 0)\n", values[0]);
    bool border_ok = values[0] == 0.0f;
    u[8] = 0;
    renderer_dispatch(r, &g, 1, 1, 1);
    renderer_sync_mem(r, out, 16);
    CHECK(gmem_read(&m, out, values, sizeof(values)));
    bool repeat_ok = values[0] == 1.0f;
    /* Visit every effective combination, then request integer filtering.
     * This crosses the old limit with genuinely different states too. */
    const uint32_t clamps[] = {0, 1, 2, 4};
    const uint32_t mip_filters[] = {1, 2, 0}; /* point, linear, disabled */
    bool all_modes_ok = true;
    for (uint32_t mode = 0; mode < 2304; mode++) {
        uint32_t base=mode%768,bias=(mode/768)*64;
        uint32_t cu = base & 3, cv = (base >> 2) & 3, cw = (base >> 4) & 3;
        u[8] = clamps[cu] | (clamps[cv] << 3) | (clamps[cw] << 6);
        u[10] = ((base >> 6) & 1) << 20 | ((base >> 7) & 1) << 22 | mip_filters[base >> 8] << 26 | bias;
        renderer_dispatch(r, &g, 1, 1, 1);
        renderer_sync_mem(r, out, 16);
        CHECK(gmem_read(&m, out, values, sizeof(values)));
        all_modes_ok &= values[0] == (cu == 3 || cv == 3 ? 0.0f : 1.0f);
    }
    u[1] = (uint32_t)(image >> 40) | (5u << 20) | (3u << 30); /* R8_UINT */
    u[8] = 0; u[10] = (1u << 20) | (1u << 22); /* guest requests linear */
    renderer_dispatch(r, &g, 1, 1, 1);
    renderer_sync_mem(r, out, 16);
    uint32_t integer = 0; CHECK(gmem_read(&m, out, &integer, sizeof(integer)));
    /* Fail a genuinely new effective sampler. Intercept update/dispatch so
     * the negative test can detect forwarding a null sampler without feeding
     * an invalid descriptor to the driver. A retry with allocation restored
     * must execute normally rather than poisoning the pipeline cache. */
    PFN_vkCreateSampler create_sampler=vkCreateSampler;
    PFN_vkUpdateDescriptorSets update_sets=vkUpdateDescriptorSets;
    PFN_vkCmdDispatch dispatch=vkCmdDispatch;
    vkCreateSampler=fail_sampler;vkUpdateDescriptorSets=capture_failed_sampler_update;
    vkCmdDispatch=capture_failed_sampler_dispatch;failed_sampler_updates=failed_sampler_dispatches=0;
    u[1]=(u[1]&~(0x1ffu<<20))|(1u<<20);u[8]=0;
    u[10]=(1u<<20)|(1u<<22)|(1u<<26)|32u;
    renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,16);
    bool failed_sampler_safe=!failed_sampler_updates && !failed_sampler_dispatches;
    vkCreateSampler=create_sampler;vkUpdateDescriptorSets=update_sets;vkCmdDispatch=dispatch;
    renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,16);
    CHECK(gmem_read(&m,out,values,sizeof(values)));bool retry_ok=values[0]==1.f;
    renderer_destroy(r); free(g.regs); pal_mutex_destroy(g.lock);
    pal_window_destroy(window); gmem_free(&m); pal_shutdown();
    CHECK(border_ok && repeat_ok && all_modes_ok && integer == 255 && failed_sampler_safe && retry_ok);
}

/* Guest formats 11 and 62 are integer images, not the RGBA8 fallback. */
static void gpu_integer_image(uint32_t format, VkFormat expected) {
    uint32_t store[] = {
        0x7e000280, 0x7e020280, 0x7e0802ff, 0x1234, 0x7e0a02ff, 0x56789,
        0xf0200308, 0x00000400, 0xbf810000
    };
    static const uint32_t load[] = {
        0x7e000280, 0x7e020280, 0xf0000308, 0x00000400,
        0xbf8c3f70, 0xe0740000, 0x80030400, 0xbf810000
    };
    if(format==18){store[3]=52;store[5]=137;}
    CHECK(pal_init() == PS5C_OK);
    gmem m; gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 8 * GUEST_PAGE_SIZE, GPROT_RW, 0, "R16 UINT", &a) == PS5C_OK);
    CHECK(gmem_write(&m, a, store, sizeof(store)) && gmem_write(&m, a + 0x100, load, sizeof(load)));
    guest_addr image = a + 0x4000, out = a + 0x1000;
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 R16 integer image", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m); CHECK(r);
    gpu g; CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    uint32_t *u = g.regs + mmCOMPUTE_USER_DATA_0;
    u[0] = (uint32_t)(image >> 8); u[1] = (uint32_t)(image >> 40) | (format << 20) | (3u << 30);
    u[2] = 3u | (15u << 14); u[3] = (9u << 28) | 0xfacu;
    u[12] = (uint32_t)out; u[13] = (uint32_t)(out >> 32); u[14] = 16; u[15] = 0x00016204;
    g.regs[mmCOMPUTE_PGM_RSRC2] = 16u << 1;
    g.regs[mmCOMPUTE_NUM_THREAD_X] = g.regs[mmCOMPUTE_NUM_THREAD_Y] = g.regs[mmCOMPUTE_NUM_THREAD_Z] = 1;
    g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(a >> 8); g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)(a >> 40);
    renderer_dispatch(r, &g, 1, 1, 1);
    vk_image *img = vk_image_find(r, image, 16, 16, expected);
    bool format_ok = img != NULL;
    g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)((a + 0x100) >> 8);
    renderer_dispatch(r, &g, 1, 1, 1);
    renderer_sync_mem(r, out, 8);
    uint32_t value[2] = {0}; CHECK(gmem_read(&m, out, value, sizeof(value)));
    fprintf(stderr, "integer image format %u roundtrip = %x %x\n", format, value[0], value[1]);
    bool normalized_ok=true;
    if(format==18) {
        /* Same UV texels: copy shader stores bytes, consumer requests UNORM.
         * Validate real GPU reads, not just the alias-format lookup table. */
        u[1]=(uint32_t)(image>>40)|(14u<<20)|(3u<<30);
        renderer_dispatch(r,&g,1,1,1);
        renderer_sync_mem(r,out,8);
        float normalized[2]={0};CHECK(gmem_read(&m,out,normalized,sizeof(normalized)));
        fprintf(stderr,"UV normalized read = %.8f %.8f\n",normalized[0],normalized[1]);
        normalized_ok=fabsf(normalized[0]-52.f/255.f)<1e-6f && fabsf(normalized[1]-137.f/255.f)<1e-6f;
    }
    renderer_destroy(r); free(g.regs); pal_mutex_destroy(g.lock);
    pal_window_destroy(window); gmem_free(&m); pal_shutdown();
    CHECK(format_ok && normalized_ok && value[0] == (format==18?0x34u:0x1234u) &&
          value[1] == (format==62?0x56789u:format==18?0x89u:0u));
}

static void test_gpu_uv_integer_to_normalized(void) {
    gpu_integer_image(18,VK_FORMAT_R8G8_UINT);
}

static void test_gpu_integer_images(void) {
    gpu_integer_image(11, VK_FORMAT_R16_UINT);
    gpu_integer_image(62, VK_FORMAT_R32G32_UINT);
}

/* The same guest bytes are used as a depth attachment and an integer
 * image by copy shaders. Verify exact bits in both directions, not colors. */
static void gpu_depth_integer_alias(VkFormat depth_format, uint32_t guest_format) {
    static const uint32_t load[] = {
        0x7e000280, 0x7e020280, 0xf0000108, 0x00000400,
        0xbf8c3f70, 0xe0700000, 0x80030400, 0xbf810000
    };
    uint32_t store[] = {0x7e000280, 0x7e020280, 0x7e0802ff, 0,
                        0xf0200108, 0x00000400, 0xbf810000};
    static const uint32_t vs[] = {
        0x7e000280, 0x7e020280, 0x7e040280, 0x7e0602f2,
        0xf80008cf, 0x03020100, 0xbf810000
    };
    static const uint32_t ps[] = {
        0x7e0002f2, 0x7e020280, 0x7e040280, 0x7e0602f2,
        0xf800180f, 0x03020100, 0xbf810000
    };
    bool d16 = depth_format == VK_FORMAT_D16_UNORM;
    store[3] = d16 ? 0xa123u : 0x3f400000u;
    CHECK(pal_init() == PS5C_OK);
    gmem m; gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 8 * GUEST_PAGE_SIZE, GPROT_RW, 0, "depth integer alias", &a) == PS5C_OK);
    CHECK(gmem_write(&m, a, load, sizeof(load)) && gmem_write(&m, a + 0x100, store, sizeof(store)) &&
          gmem_write(&m, a + 0x200, vs, sizeof(vs)) && gmem_write(&m, a + 0x300, ps, sizeof(ps)));
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 depth integer coherence", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m); CHECK(r);
    gpu g; CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    guest_addr depth = a + 0x4000, color = a + 0x8000, out = a + 0x1000;
    vk_begin(r);
    vk_image *zimg = vk_image_get(r, depth, 16, 16, depth_format); CHECK(zimg);
    uint32_t *u = g.regs + mmCOMPUTE_USER_DATA_0;
    u[0] = (uint32_t)(depth >> 8); u[1] = (uint32_t)(depth >> 40) | (guest_format << 20) | (3u << 30);
    u[2] = 3u | (15u << 14); u[3] = (9u << 28) | 0xfacu;
    u[12] = (uint32_t)out; u[13] = (uint32_t)(out >> 32); u[14] = 16; u[15] = 0x00016204;
    g.regs[mmCOMPUTE_PGM_RSRC2] = 16u << 1;
    g.regs[mmCOMPUTE_NUM_THREAD_X] = g.regs[mmCOMPUTE_NUM_THREAD_Y] = g.regs[mmCOMPUTE_NUM_THREAD_Z] = 1;
    g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)(a >> 40);
    vk_buffer seed; CHECK(vk_buffer_create(r, 1024, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, &seed));
    bool reads_ok = true;
    for (unsigned round = 0; round < 2; round++) {
        /* Seed exact bytes. Float depth clears can legitimately round .5
         * to different adjacent D16 values on different GPUs. */
        uint32_t expected = d16 ? (round ? 0x8000u : 0x4000u) : (round ? 0x3f000000u : 0x3e800000u);
        for (unsigned pixel = 0; pixel < 256; pixel++)
            memcpy((uint8_t *)seed.mapped + pixel * (d16 ? 2 : 4), &expected, d16 ? 2 : 4);
        VkCommandBuffer cmd = vk_begin(r);
        vk_barrier(cmd, zimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy upload = {0};
        upload.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        upload.imageSubresource.layerCount = 1;
        upload.imageExtent.width = upload.imageExtent.height = 16; upload.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cmd, seed.buffer, zimg->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload);
        zimg->gpu_written = true; zimg->written_seq = ++r->write_seq;
        g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(a >> 8);
        renderer_dispatch(r, &g, 1, 1, 1);
        renderer_sync_mem(r, out, 4);
        uint32_t value = 0; CHECK(gmem_read(&m, out, &value, sizeof(value)));
        fprintf(stderr, "depth fmt %d -> UINT round %u: %08x expected %08x\n", depth_format, round, value, expected);
        reads_ok &= value == expected;
    }
    g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)((a + 0x100) >> 8);
    renderer_dispatch(r, &g, 1, 1, 1); /* writes just texel (0,0) */
    renderer_wait_idle(r);
    /* Force the newer integer image out of the resident cache. Its RAM
     * backing must be considered before reusing the older depth image. */
    /* The eviction fixture owns its budget, independent of the runner's environment. */
    if (!r->image_backing_limit) r->image_backing_limit = 16u * 1024u * 1024u;
    r->image_limit = r->image_count;
    vk_begin(r);
    CHECK(vk_image_find(r, depth, 16, 16, depth_format) == zimg);
    CHECK(vk_image_get(r, a + 0xc000, 2, 2, VK_FORMAT_R8G8B8A8_UNORM));
    bool spill_ok = r->image_spills > 0 &&
                    !vk_image_find(r, depth, 16, 16, d16 ? VK_FORMAT_R16_UINT : VK_FORMAT_R32_UINT);
    r->image_limit += 2;

    g.regs[mmSPI_SHADER_PGM_LO_ES] = (uint32_t)((a + 0x200) >> 8);
    g.regs[mmSPI_SHADER_PGM_HI_ES] = (uint32_t)((a + 0x200) >> 40);
    g.regs[mmSPI_SHADER_PGM_LO_PS] = (uint32_t)((a + 0x300) >> 8);
    g.regs[mmSPI_SHADER_PGM_HI_PS] = (uint32_t)((a + 0x300) >> 40);
    g.regs[mmCB_COLOR_CONTROL] = 0xCC0010; g.regs[mmCB_TARGET_MASK] = 15;
    g.regs[mmCB_COLOR0_BASE] = (uint32_t)(color >> 8);
    g.regs[mmCB_COLOR0_INFO] = 10u << 2;
    g.regs[mmCB_COLOR0_ATTRIB2] = (15u << 14) | 15;
    g.regs[mmDB_Z_INFO] = d16 ? 1 : 3;
    g.regs[mmDB_STENCIL_INFO] = depth_format==VK_FORMAT_D32_SFLOAT_S8_UINT?1:0;
    g.regs[mmDB_Z_READ_BASE] = g.regs[mmDB_Z_WRITE_BASE] = (uint32_t)(depth >> 8);
    g.regs[mmDB_DEPTH_SIZE_XY] = (15u << 16) | 15;
    g.regs[mmDB_DEPTH_CONTROL] = 2 | (7u << 4);
    g.regs[mmVGT_PRIMITIVE_TYPE] = GPU_PRIM_TRILIST;
    gpu_draw draw = {.count = 3, .instances = 1};
    renderer_draw(r, &g, &draw);
    renderer_wait_idle(r);
    vk_buffer raw; CHECK(vk_buffer_create(r, 1024, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &raw));
    VkCommandBuffer cmd = vk_begin(r);
    vk_barrier(cmd, zimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c = {0}; c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    c.imageSubresource.layerCount = 1; c.imageExtent.width = c.imageExtent.height = 16; c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, zimg->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, raw.buffer, 1, &c);
    renderer_wait_idle(r);
    uint32_t actual = 0, untouched = 0;
    memcpy(&actual, raw.mapped, d16 ? 2 : 4);
    memcpy(&untouched, (uint8_t *)raw.mapped + (d16 ? 2 : 4), d16 ? 2 : 4);
    fprintf(stderr, "UINT -> depth fmt %d: %08x expected %08x; neighbor %08x\n", depth_format, actual, store[3], untouched);
    bool writes_ok = actual == store[3] && untouched == (d16 ? 0x8000u : 0x3f000000u);
    vk_buffer_destroy(r, &raw);
    vk_buffer_destroy(r, &seed);
    renderer_destroy(r); free(g.regs); pal_mutex_destroy(g.lock);
    pal_window_destroy(window); gmem_free(&m); pal_shutdown();
    CHECK(reads_ok && writes_ok && spill_ok);
}

/* A single metadata fill clears all three cascades. Consuming the surface's
 * clear on the first layer must not leave later layers at their old depth. */
/* The same masked load/AND/OR/store shape used by RADV for aspect clears. */
static const uint32_t masked_htile_cs[] = {
    0xd7460000,0x04010c06,0x34000084,0xdc388000,0x04020000,0xbf8c3f70,
    0xd7710008,0x00120805,0xd7710009,0x00120a05,
    0xd771000a,0x00120c05,0xd771000b,0x00120e05,
    0xdc788000,0x00020800,0xbf810000
};

static void test_masked_htile_policy(void) {
    sh_program *p=calloc(1,sizeof(*p));sh_eval *e=calloc(1,sizeof(*e));CHECK(p && e);
    CHECK(sh_analyze(masked_htile_cs,PS5C_ARRAY_COUNT(masked_htile_cs),0x10000,p));
    sh_stage_info info={.cs_tgid_sgpr=6,.local_size={64,1,1},.cs_tgid_enable=1,.cs_tid_components=1};
    e->sgpr_init[5]=0x3f0;
    CHECK(vk_htile_masked_depth_clear(p,e,&info));
    e->sgpr_init[5]=0xc00;e->sgpr_init[4]=0xf0; /* depth + stencil, preserve reserved bits */
    CHECK(vk_htile_masked_depth_clear(p,e,&info));
    e->sgpr_init[4]=0;
    e->sgpr_init[5]=0xfffffc0f; /* stencil-only update */
    CHECK(!vk_htile_masked_depth_clear(p,e,&info));
    e->sgpr_init[5]=UINT32_MAX; /* unchanged metadata is not a clear */
    CHECK(!vk_htile_masked_depth_clear(p,e,&info));
    e->sgpr_init[5]=0x3f0;e->sgpr_init[4]=15; /* expanded ZMASK */
    CHECK(!vk_htile_masked_depth_clear(p,e,&info));
    e->sgpr_init[4]=0;p->insts[8].offset=16;
    CHECK(!vk_htile_masked_depth_clear(p,e,&info));
    p->insts[8].offset=0;p->insts[4].neg=1;
    CHECK(!vk_htile_masked_depth_clear(p,e,&info));
    free(e);free(p);
}

static void test_gpu_mip_promotion(void) {
    if(!getenv("PS5C_TEST_GPU"))return;
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,4*GUEST_PAGE_SIZE,GPROT_RW,0,"mip promotion",&a)==PS5C_OK);
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 mip promotion",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);vk_begin(r);
    vk_image *img=vk_image_get_levels(r,a,64,32,VK_FORMAT_R8G8B8A8_UINT,2,7);CHECK(img && !img->mutable_fmt);
    vk_barrier(r->cmd,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    for(unsigned level=0;level<7;level++)for(unsigned layer=0;layer<2;layer++) {
        VkClearColorValue value={.uint32={17+level,53+layer,101+level+layer,255}};
        VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,level,1,layer,1};
        vkCmdClearColorImage(r->cmd,img->image,img->layout,&value,1,&range);
    }
    img->gpu_written=true;img->written_seq=img->chain_seq=37;
    CHECK(vk_image_promote(r,img));CHECK(img->levels==7 && img->chain_seq==37 && img->mutable_fmt);
    vk_buffer pixels;CHECK(vk_buffer_create(r,14*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&pixels));
    VkBufferImageCopy copies[14]={0};
    for(unsigned level=0;level<7;level++)for(unsigned layer=0;layer<2;layer++) {
        unsigned i=level*2+layer;copies[i].bufferOffset=i*4;
        copies[i].imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,level,layer,1};
        copies[i].imageExtent=(VkExtent3D){1,1,1};
    }
    vk_barrier(r->cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyImageToBuffer(r->cmd,img->image,img->layout,pixels.buffer,14,copies);renderer_wait_idle(r);
    bool correct=true;const uint8_t *bytes=pixels.mapped;
    for(unsigned level=0;level<7;level++)for(unsigned layer=0;layer<2;layer++) {
        const uint8_t expected[]={17+level,53+layer,101+level+layer,255};
        correct&=!memcmp(bytes+(level*2+layer)*4,expected,4);
    }
    vk_buffer_destroy(r,&pixels);renderer_destroy(r);pal_window_destroy(window);gmem_free(&m);pal_shutdown();CHECK(correct);
}

static void test_gpu_masked_htile(void) {
    if(!getenv("PS5C_TEST_GPU"))return;
    static const uint32_t vs[]={0x7e000280,0x7e020280,0x7e040280,0x7e0602f2,
        0xf80008cf,0x03020100,0xbf810000};
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"masked HTILE",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,vs,sizeof(vs)));CHECK(gmem_write(&m,a+0x100,masked_htile_cs,sizeof(masked_htile_cs)));
    guest_addr depth=a+0x4000,meta=a+0x10000;
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 masked depth clear",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(a>>8);g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(a>>40);
    g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_TRILIST;
    g.regs[mmDB_Z_INFO]=3|GPU_DB_Z_INFO_TILE_SURFACE_ENABLE;g.regs[mmDB_STENCIL_INFO]=1;
    g.regs[mmDB_Z_READ_BASE]=g.regs[mmDB_Z_WRITE_BASE]=(uint32_t)(depth>>8);
    g.regs[mmDB_DEPTH_SIZE_XY]=(63u<<16)|63u;
    g.regs[mmDB_DEPTH_CONTROL]=2|(7u<<4); /* no depth writes by the degenerate draw */
    g.regs[mmDB_DEPTH_CLEAR]=0x3e800000; /* .25 */
    g.regs[mmDB_HTILE_DATA_BASE]=(uint32_t)(meta>>8);
    g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x100)>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)((a+0x100)>>40);
    g.regs[mmCOMPUTE_PGM_RSRC2]=(6u<<1)|(1u<<7);
    g.regs[mmCOMPUTE_NUM_THREAD_X]=64;g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;u[2]=(uint32_t)meta;u[3]=(uint32_t)(meta>>32);
    gpu_draw draw={.count=3,.instances=1};renderer_draw(r,&g,&draw);renderer_wait_idle(r);
    vk_image *z=vk_image_find(r,depth,64,64,VK_FORMAT_D32_SFLOAT_S8_UINT);CHECK(z);
    vk_buffer pixels;CHECK(vk_buffer_create(r,4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&pixels));
    bool correct=true;
    for(unsigned round=0;round<4;round++) {
        VkCommandBuffer cmd=vk_begin(r);vk_barrier(cmd,z,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearDepthStencilValue initial={.75f,0x55};VkImageSubresourceRange all={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
        vkCmdClearDepthStencilImage(cmd,z->image,z->layout,&initial,1,&all);renderer_wait_idle(r);
        uint32_t *words=gmem_host(&m,meta,1024);CHECK(words);
        for(unsigned i=0;i<256;i++)words[i]=0xfffff33f;
        u[4]=round==1?0xf0:0;u[5]=round==0?0x3f0:round==1?0xc00:round==2?0xfffffc0f:UINT32_MAX;
        renderer_dispatch(r,&g,1,1,1);renderer_draw(r,&g,&draw);renderer_wait_idle(r);
        cmd=vk_begin(r);vk_barrier(cmd,z,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};copy.imageExtent=(VkExtent3D){1,1,1};
        vkCmdCopyImageToBuffer(cmd,z->image,z->layout,pixels.buffer,1,&copy);renderer_wait_idle(r);
        float value;memcpy(&value,pixels.mapped,4);
        fprintf(stderr,"masked HTILE round %u: depth %g\n",round,value);
        correct&=value==(round<2?.25f:.75f);
    }
    vk_buffer_destroy(r,&pixels);renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);
    pal_window_destroy(window);gmem_free(&m);pal_shutdown();CHECK(correct);
}

static void test_gpu_htile_layers(void) {
    if(!getenv("PS5C_TEST_GPU"))return;
    static const uint32_t vs[]={0x36000a81,0x2c020a81,0x7e000d00,0x7e020d01,
        0x100000ff,0x42800000,0x100202ff,0x42800000,0x060000ff,0xc1000000,
        0x060202ff,0xc1000000,0x7e0402ff,0x3f000000,0x7e0602f2,0xf80008cf,0x03020100,0xbf810000};
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,32u<<20,GPROT_RW,0,"layered HTILE clear",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,vs,sizeof(vs)));guest_addr depth=a+(1u<<20),meta=a+(28u<<20);
    const uint32_t slice_bytes=256u<<10; /* 2048^2 pixels, one HTILE dword per 8x8 tile */
    void *metadata=gmem_host(&m,meta,3u*slice_bytes);CHECK(metadata);memset(metadata,0,3u*slice_bytes);
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 layered HTILE regression",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(a>>8);g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(a>>40);
    g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_TRILIST;
    g.regs[mmDB_Z_INFO]=1|GPU_DB_Z_INFO_TILE_SURFACE_ENABLE;
    g.regs[mmDB_Z_READ_BASE]=g.regs[mmDB_Z_WRITE_BASE]=(uint32_t)(depth>>8);
    g.regs[mmDB_DEPTH_SIZE_XY]=(2047u<<16)|2047u;
    g.regs[mmDB_DEPTH_CONTROL]=6|(3u<<4);g.regs[mmDB_DEPTH_CLEAR]=0x3f800000;
    g.regs[mmDB_HTILE_DATA_BASE]=(uint32_t)(meta>>8);
    gpu_draw draw={.count=3,.instances=1};
    for(unsigned layer=0;layer<3;layer++) {
        g.regs[mmDB_DEPTH_VIEW]=layer|(layer<<13);renderer_draw(r,&g,&draw);
    }
    renderer_wait_idle(r);
    vk_image *z=vk_image_find_array(r,depth,2048,2048,VK_FORMAT_D16_UNORM);CHECK(z && z->layers==3);
    VkCommandBuffer cmd=vk_begin(r);vk_barrier(cmd,z,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearDepthStencilValue zero={0,0};VkImageSubresourceRange all={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,3};
    vkCmdClearDepthStencilImage(cmd,z->image,z->layout,&zero,1,&all);renderer_wait_idle(r);
    renderer_mem_written(r,meta,3u*slice_bytes);
    for(unsigned layer=0;layer<3;layer++) {
        g.regs[mmDB_DEPTH_VIEW]=layer|(layer<<13);renderer_draw(r,&g,&draw);
    }
    renderer_wait_idle(r);
    vk_buffer pixels;CHECK(vk_buffer_create(r,24,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&pixels));
    VkBufferImageCopy copies[6]={0};
    for(unsigned sample=0;sample<6;sample++) {
        copies[sample].bufferOffset=4u*sample;
        copies[sample].imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_DEPTH_BIT,0,sample/2,1};
        if(sample&1)copies[sample].imageOffset=(VkOffset3D){128,128,0};
        copies[sample].imageExtent=(VkExtent3D){1,1,1};
    }
    bool correct=true;
    for(unsigned round=0;round<2;round++) {
        if(round) {
            /* Clearing only layer one must preserve the other two, and a
             * later draw must not consume the same clear a second time. */
            renderer_mem_written(r,meta+slice_bytes,slice_bytes);
            g.regs[mmDB_DEPTH_CONTROL]=2|(7u<<4);g.regs[mmDB_DEPTH_CLEAR]=0x3e800000;
            for(unsigned layer=0;layer<3;layer++) {
                g.regs[mmDB_DEPTH_VIEW]=layer|(layer<<13);renderer_draw(r,&g,&draw);
            }
            g.regs[mmDB_DEPTH_CLEAR]=0x3f400000;
            g.regs[mmDB_DEPTH_VIEW]=1|(1u<<13);renderer_draw(r,&g,&draw);
            renderer_wait_idle(r);
        }
        cmd=vk_begin(r);vk_barrier(cmd,z,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyImageToBuffer(cmd,z->image,z->layout,pixels.buffer,6,copies);renderer_wait_idle(r);
        for(unsigned layer=0;layer<3;layer++) {
            const uint16_t *values=pixels.mapped;uint16_t inside=values[layer*4],outside=values[layer*4+2];
            fprintf(stderr,"HTILE round %u layer %u: drawn %04x clear %04x\n",round,layer,inside,outside);
            if(round && layer==1)correct&=inside>=0x3fff && inside<=0x4000 && outside==inside;
            else correct&=inside>=0x7fff && inside<=0x8000 && outside==0xffff;
        }
    }
    vk_buffer_destroy(r,&pixels);renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);
    pal_window_destroy(window);gmem_free(&m);pal_shutdown();CHECK(correct);
}

static void test_gpu_depth_integer_alias(void) {
    gpu_depth_integer_alias(VK_FORMAT_D16_UNORM, 11);
    gpu_depth_integer_alias(VK_FORMAT_D32_SFLOAT, 20);
    gpu_depth_integer_alias(VK_FORMAT_D32_SFLOAT_S8_UINT, 22);
}

/* Exercise translated guest exports through the real graphics pipeline and
 * read back pixels, including a single PS reused for UINT and float targets. */
static void test_gpu_integer_color_exports(void) {
    static const uint32_t vs[] = {
        0x36000a81, 0x2c020a81, /* v0=vertex_id&1, v1=vertex_id>>1 (input v5) */
        0x7e000d00, 0x7e020d01, /* convert to float */
        0x100000ff, 0x42800000, 0x100202ff, 0x42800000, /* *=64 */
        0x060000ff, 0xc1000000, 0x060202ff, 0xc1000000, /* -=8 */
        0x7e040280, 0x7e0602f2, 0xf80008cf, 0x03020100, 0xbf810000
    };
    const struct {
        VkFormat format;
        uint32_t info, col_format, compressed, values[4], expected[4], bytes;
    } cases[] = {
        {VK_FORMAT_R16_UINT, (2u << 2) | (4u << 8), 1, 0,
         {0xbeef,0,0,0}, {0xbeef,0,0,0}, 2},
        {VK_FORMAT_R32G32_UINT, (11u << 2) | (4u << 8), 2, 0,
         {0x12345678,0x9abcdef0,0,0}, {0x12345678,0x9abcdef0,0,0}, 8},
        {VK_FORMAT_R32_UINT, (4u << 2) | (4u << 8), 1, 0,
         {0x12345678,0,0,0}, {0x12345678,0,0,0}, 4},
        {VK_FORMAT_R32_SFLOAT, (4u << 2) | (7u << 8), 1, 0,
         {0x12345678,0,0,0}, {0x12345678,0,0,0}, 4},
        {VK_FORMAT_R32_SINT, (4u << 2) | (5u << 8), 1, 0,
         {0xffed2979,0,0,0}, {0xffed2979,0,0,0}, 4},
        {VK_FORMAT_R16G16B16A16_UINT, (12u << 2) | (4u << 8), 7, 1,
         {0x8123abcd,0x9001ffff,0,0}, {0x8123abcd,0x9001ffff,0,0}, 8},
        {VK_FORMAT_R32_SINT, (4u << 2) | (5u << 8), 8, 1,
         {0x1234abcd,0,0,0}, {0xffffabcd,0,0,0}, 4}
    };
    CHECK(pal_init() == PS5C_OK);
    gmem m; gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 16 * GUEST_PAGE_SIZE, GPROT_RW, 0, "integer color exports", &a) == PS5C_OK);
    CHECK(gmem_write(&m, a, vs, sizeof(vs)));
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 integer color export regression", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m); CHECK(r);
    gpu g; CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    g.regs[mmSPI_SHADER_PGM_LO_ES] = (uint32_t)(a >> 8);
    g.regs[mmSPI_SHADER_PGM_HI_ES] = (uint32_t)(a >> 40);
    g.regs[mmCB_COLOR_CONTROL] = 0xCC0010; g.regs[mmCB_TARGET_MASK] = 15;
    g.regs[mmCB_COLOR0_ATTRIB2] = (15u << 14) | 15;
    g.regs[mmVGT_PRIMITIVE_TYPE] = GPU_PRIM_TRILIST;
    gpu_draw draw = {.count = 3, .instances = 1};
    bool correct = true;
    for (uint32_t k = 0; k < PS5C_ARRAY_COUNT(cases); k++) {
        uint32_t ps[] = {0x7e0002ff,cases[k].values[0],0x7e0202ff,cases[k].values[1],
                         0x7e0402ff,cases[k].values[2],0x7e0602ff,cases[k].values[3],
                         0xf800180f | (cases[k].compressed << 10),0x03020100,0xbf810000};
        /* The identical UINT/float programs deliberately share code address. */
        guest_addr shader = a + 0x100 * (k == 3 ? 3 : k + 1), color = a + 0x4000 * (k + 1);
        CHECK(gmem_write(&m, shader, ps, sizeof(ps)));
        g.regs[mmSPI_SHADER_PGM_LO_PS] = (uint32_t)(shader >> 8);
        g.regs[mmSPI_SHADER_PGM_HI_PS] = (uint32_t)(shader >> 40);
        g.regs[mmCB_COLOR0_BASE] = (uint32_t)(color >> 8);
        g.regs[mmCB_COLOR0_INFO] = cases[k].info;
        g.regs[mmSPI_SHADER_COL_FORMAT] = cases[k].col_format;
        renderer_draw(r, &g, &draw);
        renderer_wait_idle(r);
        vk_image *img = vk_image_find(r, color, 16, 16, cases[k].format); CHECK(img);
        vk_buffer raw; CHECK(vk_buffer_create(r, 256 * cases[k].bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &raw));
        VkCommandBuffer cmd = vk_begin(r);
        vk_barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {0}; copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1; copy.imageExtent = (VkExtent3D){16,16,1};
        vkCmdCopyImageToBuffer(cmd, img->image, img->layout, raw.buffer, 1, &copy);
        renderer_wait_idle(r);
        bool matches = true;
        for (uint32_t pixel = 0; pixel < 256; pixel++)
            matches &= !memcmp((uint8_t *)raw.mapped + pixel * cases[k].bytes, cases[k].expected, cases[k].bytes);
        fprintf(stderr, "integer color export case %u fmt %d: all 256 pixels %s\n", k, cases[k].format, matches ? "correct" : "WRONG");
        correct &= matches;
        vk_buffer_destroy(r, &raw);
    }
    renderer_destroy(r); free(g.regs); pal_mutex_destroy(g.lock);
    pal_window_destroy(window); gmem_free(&m); pal_shutdown();
    CHECK(correct);
}

/* MRTZ Z is sample coverage, not a depth export. A mask-only pixel shader
 * must write the interpolated 0.5 depth; an X export must still replace it. */
static void test_gpu_mrtz_depth(void) {
    static const uint32_t vs[] = {
        0x36000a81, 0x2c020a81, 0x7e000d00, 0x7e020d01,
        0x100000ff, 0x42800000, 0x100202ff, 0x42800000,
        0x060000ff, 0xc1000000, 0x060202ff, 0xc1000000,
        0x7e0402f0, 0x7e0602f2, 0xf80008cf, 0x03020100, 0xbf810000
    };
    CHECK(pal_init() == PS5C_OK);
    gmem m; gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 16 * GUEST_PAGE_SIZE, GPROT_RW, 0, "MRTZ depth", &a) == PS5C_OK);
    CHECK(gmem_write(&m, a, vs, sizeof(vs)));
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 MRTZ depth regression", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m); CHECK(r);
    gpu g; CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    g.regs[mmSPI_SHADER_PGM_LO_ES] = (uint32_t)(a >> 8);
    g.regs[mmSPI_SHADER_PGM_HI_ES] = (uint32_t)(a >> 40);
    g.regs[mmCB_COLOR_CONTROL] = 0xCC0010; g.regs[mmCB_TARGET_MASK] = 15;
    g.regs[mmCB_COLOR0_INFO] = 10u << 2;
    g.regs[mmCB_COLOR0_ATTRIB2] = (15u << 14) | 15;
    g.regs[mmSPI_SHADER_COL_FORMAT] = 9;
    g.regs[mmDB_Z_INFO] = 3;
    g.regs[mmDB_DEPTH_SIZE_XY] = (15u << 16) | 15;
    g.regs[mmDB_DEPTH_CONTROL] = 6 | (7u << 4); /* ALWAYS, write */
    g.regs[mmVGT_PRIMITIVE_TYPE] = GPU_PRIM_TRILIST;
    gpu_draw draw = {.count = 3, .instances = 1};
    bool correct = true;
    const unsigned masks[] = {4, 2, 6, 1, 3, 5, 7};
    for (unsigned k = 0; k < PS5C_ARRAY_COUNT(masks); k++) {
        uint32_t ps[] = {
            0x7e0002f2, 0x7e020280, 0x7e040280, 0x7e0602f2,
            0x7e0802ff, 0x3e800000, /* v4 = exported depth 0.25 */
            0x7e0a0281, 0x7e0c02c1, /* v5=stencil 1, v6=sample mask -1 */
            0xf8000080u | masks[k], 0x00060504,
            0xf800180f, 0x03020100, 0xbf810000
        };
        guest_addr shader = a + 0x100 * (k + 1), color = a + 0x4000, depth = a + 0x8000;
        CHECK(gmem_write(&m, shader, ps, sizeof(ps)));
        g.regs[mmSPI_SHADER_PGM_LO_PS] = (uint32_t)(shader >> 8);
        g.regs[mmSPI_SHADER_PGM_HI_PS] = (uint32_t)(shader >> 40);
        g.regs[mmCB_COLOR0_BASE] = (uint32_t)(color >> 8);
        g.regs[mmDB_Z_READ_BASE] = g.regs[mmDB_Z_WRITE_BASE] = (uint32_t)(depth >> 8);
        renderer_draw(r, &g, &draw);
        renderer_wait_idle(r);
        vk_image *img = vk_image_find(r, depth, 16, 16, VK_FORMAT_D32_SFLOAT); CHECK(img);
        vk_buffer raw; CHECK(vk_buffer_create(r, 256 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &raw));
        VkCommandBuffer cmd = vk_begin(r);
        vk_barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {0}; copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        copy.imageSubresource.layerCount = 1; copy.imageExtent = (VkExtent3D){16,16,1};
        vkCmdCopyImageToBuffer(cmd, img->image, img->layout, raw.buffer, 1, &copy);
        renderer_wait_idle(r);
        uint32_t expected = masks[k] & 1 ? 0x3e800000u : 0x3f000000u;
        bool matches = true;
        for (unsigned pixel = 0; pixel < 256; pixel++)
            matches &= ((uint32_t *)raw.mapped)[pixel] == expected;
        fprintf(stderr, "MRTZ mask %u: expected depth %g, first %g, all 256 pixels %s\n",
                masks[k], masks[k] & 1 ? 0.25 : 0.5, *(float *)raw.mapped, matches ? "correct" : "WRONG");
        correct &= matches;
        vk_buffer_destroy(r, &raw);
    }
    renderer_destroy(r); free(g.regs); pal_mutex_destroy(g.lock);
    pal_window_destroy(window); gmem_free(&m); pal_shutdown();
    CHECK(correct);
}

/* One PS exposes hardware FrontFacing as red/green. The baseline orientation
 * is observed with culling off; FACE must invert it, and front/back/both
 * culling must remove precisely the corresponding fragments. All states
 * reuse the same shaders and target, then revisit warm cached pipelines. */
static void test_gpu_raster_faces(void) {
    static const uint32_t vs[] = {
        0x36000a81, 0x2c020a81, 0x7e000d00, 0x7e020d01,
        0x100000ff, 0x42800000, 0x100202ff, 0x42800000,
        0x060000ff, 0xc1000000, 0x060202ff, 0xc1000000,
        0x7e040280, 0x7e0602f2, 0xf80008cf, 0x03020100, 0xbf810000
    };
    static const uint32_t ps[] = {
        0x080200f2, /* v1 = 1.0 - v0; v0 is FRONT_FACE input */
        0x7e040280, 0x7e0602f2, 0xf800180f, 0x03020100, 0xbf810000
    };
    CHECK(pal_init() == PS5C_OK);
    gmem m; gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 8 * GUEST_PAGE_SIZE, GPROT_RW, 0, "raster faces", &a) == PS5C_OK);
    CHECK(gmem_write(&m, a, vs, sizeof(vs)) && gmem_write(&m, a + 0x100, ps, sizeof(ps)));
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 raster face regression", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m); CHECK(r);
    gpu g; CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    guest_addr color = a + 0x4000;
    g.regs[mmSPI_SHADER_PGM_LO_ES] = (uint32_t)(a >> 8);
    g.regs[mmSPI_SHADER_PGM_HI_ES] = (uint32_t)(a >> 40);
    g.regs[mmSPI_SHADER_PGM_LO_PS] = (uint32_t)((a + 0x100) >> 8);
    g.regs[mmSPI_SHADER_PGM_HI_PS] = (uint32_t)((a + 0x100) >> 40);
    g.regs[mmSPI_PS_INPUT_ADDR] = 1u << 12; /* FRONT_FACE is first VGPR */
    g.regs[mmCB_COLOR_CONTROL] = 0xCC0010; g.regs[mmCB_TARGET_MASK] = 15;
    g.regs[mmCB_COLOR0_BASE] = (uint32_t)(color >> 8);
    g.regs[mmCB_COLOR0_INFO] = 10u << 2;
    g.regs[mmCB_COLOR0_ATTRIB2] = (15u << 14) | 15;
    g.regs[mmSPI_SHADER_COL_FORMAT] = 9;
    g.regs[mmVGT_PRIMITIVE_TYPE] = GPU_PRIM_TRILIST;
    gpu_draw draw = {.count = 3, .instances = 1};
    const uint32_t states[] = {0,1,2,3,4,5,6,7,0,6,2,4};
    bool correct = true, baseline_front = false;
    for (unsigned k = 0; k < PS5C_ARRAY_COUNT(states) + 2; k++) {
        bool rect = k >= PS5C_ARRAY_COUNT(states);
        uint32_t state = rect ? 3u | ((k - PS5C_ARRAY_COUNT(states)) << 2) : states[k];
        vk_image *img = vk_image_get(r, color, 16, 16, VK_FORMAT_R8G8B8A8_UNORM); CHECK(img);
        VkCommandBuffer cmd = vk_begin(r);
        vk_barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue black = {{0,0,0,0}};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(cmd, img->image, img->layout, &black, 1, &range);
        img->gpu_written = true; img->written_seq = ++r->write_seq;
        renderer_wait_idle(r);
        g.regs[mmPA_SU_SC_MODE_CNTL] = state;
        g.regs[mmVGT_PRIMITIVE_TYPE] = rect ? GPU_PRIM_RECTLIST : GPU_PRIM_TRILIST;
        renderer_draw(r, &g, &draw);
        renderer_wait_idle(r);
        vk_buffer raw; CHECK(vk_buffer_create(r, 256 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &raw));
        cmd = vk_begin(r);
        vk_barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {0}; copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1; copy.imageExtent = (VkExtent3D){16,16,1};
        vkCmdCopyImageToBuffer(cmd, img->image, img->layout, raw.buffer, 1, &copy);
        renderer_wait_idle(r);
        const uint32_t red = 0xff0000ffu, green = 0xff00ff00u;
        uint32_t first = *(uint32_t *)raw.mapped;
        if (!k) {
            CHECK(first == red || first == green);
            baseline_front = first == red;
        }
        bool front = baseline_front ^ ((state & 4) != 0);
        bool culled = (state & (front ? 1u : 2u)) != 0;
        uint32_t expected = culled ? 0 : front ? red : green;
        if (rect) { /* Both cull bits must preserve rectangle coverage. */
            CHECK(first == red || first == green);
            expected = first;
        }
        bool matches = true;
        for (unsigned pixel = 0; pixel < 256; pixel++) matches &= ((uint32_t *)raw.mapped)[pixel] == expected;
        fprintf(stderr, "raster state %u%s: expected %08x, first %08x, all 256 pixels %s\n",
                state, rect ? " rect" : "", expected, first, matches ? "correct" : "WRONG");
        correct &= matches;
        vk_buffer_destroy(r, &raw);
    }
    renderer_destroy(r); free(g.regs); pal_mutex_destroy(g.lock);
    pal_window_destroy(window); gmem_free(&m); pal_shutdown();
    CHECK(correct);
}

/* A real triangle samples the depth attachment and then writes a new depth.
 * The next draw must see that write, through the same warm binding and pipeline.
 * Repeat on array slice 1 so a feedback copy cannot silently bind slice 0. */
static void test_gpu_depth_feedback(void) {
    static const uint32_t vs[] = {
        0x36000a81, 0x2c020a81, 0x7e000d00, 0x7e020d01,
        0x100000ff, 0x42800000, 0x100202ff, 0x42800000,
        0x060000ff, 0xc1000000, 0x060202ff, 0xc1000000,
        0x7e0402ff, 0x3e800000, 0x7e0602f2, 0xf80008cf, 0x03020100, 0xbf810000
    };
    static const uint32_t ps[] = {
        0x7e000280, 0x7e020280, 0xf0000f08, 0x00000400,
        0xbf8c3f70, 0xf800180f, 0x07060504, 0xbf810000
    };
    CHECK(pal_init() == PS5C_OK);
    gmem m; gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 8 * GUEST_PAGE_SIZE, GPROT_RW, 0, "depth feedback", &a) == PS5C_OK);
    CHECK(gmem_write(&m, a, vs, sizeof(vs)) && gmem_write(&m, a + 0x100, ps, sizeof(ps)));
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 depth feedback regression", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m); CHECK(r);
    gpu g; CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    guest_addr color = a + 0x4000, depth = a + 0x8000;
    g.regs[mmSPI_SHADER_PGM_LO_ES] = (uint32_t)(a >> 8);
    g.regs[mmSPI_SHADER_PGM_HI_ES] = (uint32_t)(a >> 40);
    g.regs[mmSPI_SHADER_PGM_LO_PS] = (uint32_t)((a + 0x100) >> 8);
    g.regs[mmSPI_SHADER_PGM_HI_PS] = (uint32_t)((a + 0x100) >> 40);
    g.regs[mmSPI_SHADER_PGM_RSRC2_PS] = 8u << 1;
    g.regs[mmCB_COLOR_CONTROL] = 0xCC0010; g.regs[mmCB_TARGET_MASK] = 15;
    g.regs[mmCB_COLOR0_BASE] = (uint32_t)(color >> 8);
    g.regs[mmCB_COLOR0_INFO] = 10u << 2;
    g.regs[mmCB_COLOR0_ATTRIB2] = (15u << 14) | 15;
    g.regs[mmSPI_SHADER_COL_FORMAT] = 9;
    g.regs[mmDB_Z_INFO] = 3;
    g.regs[mmDB_Z_READ_BASE] = g.regs[mmDB_Z_WRITE_BASE] = (uint32_t)(depth >> 8);
    g.regs[mmDB_DEPTH_SIZE_XY] = (15u << 16) | 15;
    g.regs[mmDB_DEPTH_CONTROL] = 6 | (7u << 4);
    g.regs[mmVGT_PRIMITIVE_TYPE] = GPU_PRIM_TRILIST;
    uint32_t *u = g.regs + mmSPI_SHADER_USER_DATA_PS_0;
    u[0] = (uint32_t)(depth >> 8);
    u[1] = (uint32_t)(depth >> 40) | (22u << 20) | (3u << 30);
    u[2] = 3u | (15u << 14);
    gpu_draw draw = {.count = 3, .instances = 1};
    vk_buffer raw; CHECK(vk_buffer_create(r, 1024, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &raw));
    bool correct = true;
    for (unsigned slice = 0; slice < 2; slice++) {
        g.regs[mmDB_DEPTH_VIEW] = slice | (slice << 13);
        u[3] = ((slice ? 13u : 9u) << 28) | 0xfacu;
        u[4] = slice | (slice << 16);
        vk_image *z = vk_image_get_layers(r, depth, 16, 16, VK_FORMAT_D32_SFLOAT, slice + 1); CHECK(z);
        VkCommandBuffer cmd = vk_begin(r);
        vk_barrier(cmd, z, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearDepthStencilValue initial = {0.75f, 0};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, slice + 1};
        vkCmdClearDepthStencilImage(cmd, z->image, z->layout, &initial, 1, &range);
        z->gpu_written = true; z->written_seq = ++r->write_seq;
        renderer_wait_idle(r);
        for (unsigned round = 0; round < 3; round++) {
            renderer_draw(r, &g, &draw);
            renderer_wait_idle(r);
            vk_image *img = vk_image_find(r, color, 16, 16, VK_FORMAT_R8G8B8A8_UNORM); CHECK(img);
            cmd = vk_begin(r);
            vk_barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy = {0}; copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1; copy.imageExtent = (VkExtent3D){16,16,1};
            vkCmdCopyImageToBuffer(cmd, img->image, img->layout, raw.buffer, 1, &copy);
            renderer_wait_idle(r);
            uint32_t expected = round ? 0xff000040u : 0xff0000bfu;
            uint32_t first = *(uint32_t *)raw.mapped;
            bool matches = true;
            for (unsigned pixel = 0; pixel < 256; pixel++) matches &= ((uint32_t *)raw.mapped)[pixel] == expected;
            fprintf(stderr, "depth feedback slice %u round %u: expected %08x first %08x, %s\n",
                    slice, round, expected, first, matches ? "correct" : "WRONG");
            correct &= matches;
        }
    }
    vk_buffer_destroy(r, &raw);
    renderer_destroy(r); free(g.regs); pal_mutex_destroy(g.lock);
    pal_window_destroy(window); gmem_free(&m); pal_shutdown();
    CHECK(correct);
}

static void test_gpu_quad_coverage(void) {
    static const uint32_t vs[] = {
        0x36000a81, 0x2c020a81, 0x7e000d00, 0x7e020d01,
        0x100000ff, 0x42800000, 0x100202ff, 0x42800000,
        0x060000ff, 0xc1000000, 0x060202ff, 0xc1000000,
        0x7e0402ff, 0x3e800000, 0x7e0602f2, 0xf80008cf, 0x03020100, 0xbf810000
    };
    static const uint32_t ps[] = {
        0x7e0002f2, 0x7e020280, 0x7e040280, 0x7e0602f2,
        0xf800180f, 0x03020100, 0xbf810000
    };
    CHECK(pal_init() == PS5C_OK);
    gmem m; gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 8 * GUEST_PAGE_SIZE, GPROT_RW, 0, "quad coverage", &a) == PS5C_OK);
    CHECK(gmem_write(&m, a, vs, sizeof(vs)) && gmem_write(&m, a + 0x100, ps, sizeof(ps)));
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 quad coverage regression", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m); CHECK(r);
    gpu g; CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    guest_addr color = a + 0x4000, depth = a + 0x8000;
    g.regs[mmSPI_SHADER_PGM_LO_ES] = (uint32_t)(a >> 8);
    g.regs[mmSPI_SHADER_PGM_HI_ES] = (uint32_t)(a >> 40);
    g.regs[mmSPI_SHADER_PGM_LO_PS] = (uint32_t)((a + 0x100) >> 8);
    g.regs[mmSPI_SHADER_PGM_HI_PS] = (uint32_t)((a + 0x100) >> 40);
    g.regs[mmCB_COLOR_CONTROL] = 0xCC0010; g.regs[mmCB_TARGET_MASK] = 15;
    g.regs[mmCB_COLOR0_BASE] = (uint32_t)(color >> 8);
    g.regs[mmCB_COLOR0_INFO] = 10u << 2; g.regs[mmCB_COLOR0_ATTRIB2] = (15u << 14) | 15;
    g.regs[mmSPI_SHADER_COL_FORMAT] = 9;
    g.regs[mmDB_Z_INFO] = 3;
    g.regs[mmDB_Z_READ_BASE] = g.regs[mmDB_Z_WRITE_BASE] = (uint32_t)(depth >> 8);
    g.regs[mmDB_DEPTH_SIZE_XY] = (15u << 16) | 15;
    g.regs[mmDB_DEPTH_CONTROL] = 6 | (7u << 4);
    g.regs[mmVGT_PRIMITIVE_TYPE] = GPU_PRIM_TRILIST;
    gpu_draw draw = {.count = 3, .instances = 1};
    vk_buffer raw; CHECK(vk_buffer_create(r, 2048, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &raw));
    const uint32_t masks[] = {15,2,9,0,1,4,8,15,2};
    bool correct = true;
    bool native_helpers = r->demote;
    for (unsigned round = 0; round < 2 * PS5C_ARRAY_COUNT(masks); round++) {
        if (round == PS5C_ARRAY_COUNT(masks)) r->demote = false; /* exercise hosts without demote */
        uint32_t mask = masks[round % PS5C_ARRAY_COUNT(masks)];
        g.regs[0xA30E] = (mask & 1 ? 0xffffu : 0) | (mask & 2 ? 0xffff0000u : 0);
        g.regs[0xA30F] = (mask & 4 ? 0xffffu : 0) | (mask & 8 ? 0xffff0000u : 0);
        vk_image *c = vk_image_get(r, color, 16, 16, VK_FORMAT_R8G8B8A8_UNORM); CHECK(c);
        vk_image *z = vk_image_get(r, depth, 16, 16, VK_FORMAT_D32_SFLOAT); CHECK(z);
        VkCommandBuffer cmd = vk_begin(r);
        vk_barrier(cmd, c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vk_barrier(cmd, z, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue black = {{0,0,0,0}};
        VkClearDepthStencilValue initial = {0.75f,0};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(cmd, c->image, c->layout, &black, 1, &range);
        range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCmdClearDepthStencilImage(cmd, z->image, z->layout, &initial, 1, &range);
        c->gpu_written = z->gpu_written = true;
        c->written_seq = ++r->write_seq; z->written_seq = ++r->write_seq;
        renderer_wait_idle(r);
        renderer_draw(r, &g, &draw);
        renderer_wait_idle(r);
        cmd = vk_begin(r);
        vk_barrier(cmd, c, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vk_barrier(cmd, z, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {0}; copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1; copy.imageExtent = (VkExtent3D){16,16,1};
        vkCmdCopyImageToBuffer(cmd, c->image, c->layout, raw.buffer, 1, &copy);
        copy.bufferOffset = 1024; copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCmdCopyImageToBuffer(cmd, z->image, z->layout, raw.buffer, 1, &copy);
        renderer_wait_idle(r);
        unsigned bad = 0;
        for (unsigned pixel = 0; pixel < 256; pixel++) {
            unsigned bit = (pixel & 1u) | (((pixel / 16u) & 1u) << 1);
            bool covered = (mask >> bit) & 1u;
            bad += ((uint32_t *)raw.mapped)[pixel] != (covered ? 0xff0000ffu : 0);
            bad += ((uint32_t *)raw.mapped)[256 + pixel] != (covered ? 0x3e800000u : 0x3f400000u);
        }
        fprintf(stderr, "quad mask %x round %u helpers %u: %u incorrect color/depth pixels\n", mask, round, r->demote, bad);
        correct &= !bad;
    }
    r->demote = native_helpers;
    vk_buffer_destroy(r, &raw);
    renderer_destroy(r); free(g.regs); pal_mutex_destroy(g.lock);
    pal_window_destroy(window); gmem_free(&m); pal_shutdown();
    CHECK(correct);
}

/* A depth-only opaque draw may leave an old alpha-test PS address in the
 * registers. With no PS depth/kill/mask/side effects enabled, it must fill Z.
 * Otherwise a subsequent EQUAL material shows holes through the mesh. */
static void test_gpu_opaque_depth_prepass(void) {
    static const uint32_t vs[]={
        0x36000a81,0x2c020a81,0x7e000d00,0x7e020d01,
        0x100000ff,0x42800000,0x100202ff,0x42800000,
        0x060000ff,0xc1000000,0x060202ff,0xc1000000,
        0x7e0402f0,0x7e0602f2,0xf80008cf,0x03020100,0xbf810000};
    static const uint32_t kill[]={0xbefe0480,0xf8001890,0,0xbf810000};
    static const uint32_t red[]={0x7e0002f2,0x7e020280,0x7e040280,0x7e0602f2,
        0xf800180f,0x03020100,0xbf810000};
    CHECK(pal_init()==PS5C_OK);
    gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"opaque prepass",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,vs,sizeof(vs)) && gmem_write(&m,a+0x100,kill,sizeof(kill)) && gmem_write(&m,a+0x200,red,sizeof(red)));
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 opaque prepass",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(a>>8);g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(a>>40);
    g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_TRILIST;
    g.regs[mmDB_Z_INFO]=3;g.regs[mmDB_DEPTH_SIZE_XY]=(15u<<16)|15;
    g.regs[mmCB_COLOR0_INFO]=10u<<2;g.regs[mmCB_COLOR0_ATTRIB2]=(15u<<14)|15;
    g.regs[mmSPI_SHADER_COL_FORMAT]=4;
    gpu_draw draw={.count=3,.instances=1};bool correct=true;
    for(unsigned k=0;k<3;k++) {
        guest_addr color=a+0x4000+k*0x1000,depth=a+0x8000+k*0x1000;
        g.regs[mmDB_Z_READ_BASE]=g.regs[mmDB_Z_WRITE_BASE]=(uint32_t)(depth>>8);
        g.regs[mmDB_DEPTH_CONTROL]=6|(7u<<4);
        g.regs[mmCB_COLOR_CONTROL]=0;g.regs[mmCB_TARGET_MASK]=0;
        g.regs[mmDB_SHADER_CONTROL]=k==1?1u<<6:0; /* only second case enables discard */
        guest_addr ps=k==2?0:a+0x100;
        g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)(ps>>8);g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)(ps>>40);
        renderer_draw(r,&g,&draw);
        g.regs[mmDB_DEPTH_CONTROL]=2|(2u<<4); /* EQUAL, do not change Z */
        g.regs[mmDB_SHADER_CONTROL]=0x10;
        g.regs[mmCB_COLOR_CONTROL]=0xCC0010;g.regs[mmCB_TARGET_MASK]=15;
        g.regs[mmCB_COLOR0_BASE]=(uint32_t)(color>>8);
        g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x200)>>8);g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)((a+0x200)>>40);
        renderer_draw(r,&g,&draw);renderer_wait_idle(r);
        vk_image *img=vk_image_find(r,color,16,16,VK_FORMAT_R8G8B8A8_UNORM);CHECK(img);
        vk_buffer raw;CHECK(vk_buffer_create(r,1024,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&raw));
        VkCommandBuffer cmd=vk_begin(r);
        vk_barrier(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy={0};copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount=1;copy.imageExtent=(VkExtent3D){16,16,1};
        vkCmdCopyImageToBuffer(cmd,img->image,img->layout,raw.buffer,1,&copy);renderer_wait_idle(r);
        unsigned painted=0;for(unsigned p=0;p<256;p++)if(((uint8_t*)raw.mapped)[p*4]==255)painted++;
        fprintf(stderr,"opaque prepass case %u: %u red pixels, expected %u\n",k,painted,k==1?0:256);
        correct &= painted==(k==1?0u:256u);vk_buffer_destroy(r,&raw);
    }
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&m);pal_shutdown();CHECK(correct);
}

/* Changing the PS input layout must select a new live pipeline even when the
 * shader address, shader bytes, attachments and interpolation state match. */
static void test_gpu_pixel_input_layout(void) {
    static const uint32_t vs[] = {
        0x36000a81,0x2c020a81,0x7e000d00,0x7e020d01,
        0x100000ff,0x42800000,0x100202ff,0x42800000,
        0x060000ff,0xc1000000,0x060202ff,0xc1000000,
        0x7e0402ff,0x3e800000,0x7e0602f2,0xf80008cf,0x03020100,0xbf810000
    };
    /* Red = input VGPR0 / 16. Switching POS_X -> POS_Y must rotate the ramp. */
    static const uint32_t ps[] = {
        0x100000ff,0x3d800000,0x7e020280,0x7e040280,0x7e0602f2,
        0xf800180f,0x03020100,0xbf810000
    };
    CHECK(pal_init()==PS5C_OK);
    gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr a;CHECK(gmem_map(&m,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"PS input layout",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,vs,sizeof(vs)) && gmem_write(&m,a+0x100,ps,sizeof(ps)));
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 PS input layout",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);
    gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    guest_addr color=a+0x4000;
    g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(a>>8);g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(a>>40);
    g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x100)>>8);g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)((a+0x100)>>40);
    g.regs[mmCB_COLOR_CONTROL]=0xCC0010;g.regs[mmCB_TARGET_MASK]=g.regs[mmCB_SHADER_MASK]=15;
    g.regs[mmCB_COLOR0_BASE]=(uint32_t)(color>>8);g.regs[mmCB_COLOR0_INFO]=10u<<2;
    g.regs[mmCB_COLOR0_ATTRIB2]=(15u<<14)|15;g.regs[mmSPI_SHADER_COL_FORMAT]=9;
    g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_TRILIST;
    gpu_draw draw={.count=3,.instances=1};
    vk_buffer raw;CHECK(vk_buffer_create(r,1024,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&raw));
    bool correct=true;
    for(unsigned round=0;round<3;round++) {
        bool y=round==1;
        g.regs[mmSPI_PS_INPUT_ADDR]=g.regs[mmSPI_PS_INPUT_ENA]=1u<<(y?9:8);
        renderer_draw(r,&g,&draw);renderer_wait_idle(r);
        vk_image *img=vk_image_find(r,color,16,16,VK_FORMAT_R8G8B8A8_UNORM);CHECK(img);
        VkCommandBuffer cmd=vk_begin(r);
        vk_barrier(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copy.imageExtent=(VkExtent3D){16,16,1};
        vkCmdCopyImageToBuffer(cmd,img->image,img->layout,raw.buffer,1,&copy);renderer_wait_idle(r);
        unsigned bad=0;
        for(unsigned pixel=0;pixel<256;pixel++) {
            unsigned coord=y?pixel/16:pixel%16;
            int expected=(int)(((float)coord+.5f)*255.0f/16.0f+.5f);
            int red=((uint8_t *)raw.mapped)[pixel*4];
            bad+=abs(red-expected)>1;
        }
        fprintf(stderr,"PS input layout %s round %u: %u incorrect pixels\n",y?"Y":"X",round,bad);
        correct&=!bad;
    }
    vk_buffer_destroy(r,&raw);renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);
    pal_window_destroy(window);gmem_free(&m);pal_shutdown();CHECK(correct);
}

/* A VM export rejects EXEC-off fragments even after an earlier color export. */
static void test_gpu_export_valid_mask(void) {
    static const uint32_t vs[]={0x36000a81,0x2c020a81,0x7e000d00,0x7e020d01,
        0x100000ff,0x42800000,0x100202ff,0x42800000,0x060000ff,0xc1000000,
        0x060202ff,0xc1000000,0x7e0402ff,0x3e800000,0x7e0602f2,0xf80008cf,0x03020100,0xbf810000};
    CHECK(pal_init()==PS5C_OK);
    gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr a;CHECK(gmem_map(&m,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"export VM",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,vs,sizeof(vs)));
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 export valid mask",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(a>>8);g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(a>>40);
    g.regs[mmCB_COLOR_CONTROL]=0xCC0010;g.regs[mmCB_TARGET_MASK]=g.regs[mmCB_SHADER_MASK]=15;
    g.regs[mmCB_COLOR0_INFO]=10u<<2;g.regs[mmCB_COLOR0_ATTRIB2]=(15u<<14)|15;
    g.regs[mmSPI_SHADER_COL_FORMAT]=9;g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_TRILIST;
    gpu_draw draw={.count=3,.instances=1};vk_buffer raw;
    CHECK(vk_buffer_create(r,1024,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&raw));bool correct=true;
    for(unsigned round=0;round<5;round++) {
        uint32_t ps[20]={0x7e0002f2,0x7e020280,0x7e040280,0x7e0602f2,
            0xf800000fu,0x03020100,0xbefe0480,0xf8001890u,0,0xbf810000};
        if(round==1) {ps[4]|=0x1000u;ps[7]&=~0x1000u;} /* VM keep, then non-VM with EXEC off */
        if(round==2) ps[6]=0xbefe04c1u; /* EXEC all ones before VM */
        if(round>=3) {
            /* A scalar skip to a second terminator must preserve the discard
             * decision. This is the sampled-mask/ANDN2/early-end control flow
             * used by post effects, reduced to a constant zero/one mask. */
            const uint32_t branched[]={0x7e160280,0xbeea03ff,0x38d1b717,
                0xbe9e047e,0xbefe0a7e,0x7c08166a,0x8a9e6a1e,0xbf840007,
                0x7e0002f2,0x7e020280,0x7e040280,0x7e0602f2,
                0xf800180f,0x03020100,0xbf810000,
                0xbefe0480,0xf8001890,0,0xbf810000};
            memcpy(ps,branched,sizeof(branched));
            if(round==4) ps[0]=0x7e1602f2; /* valid mask */
        }
        guest_addr pa=a+0x100+round*0x100,color=a+0x4000+round*0x1000;
        CHECK(gmem_write(&m,pa,ps,sizeof(ps)));
        g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)(pa>>8);g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)(pa>>40);
        g.regs[mmCB_COLOR0_BASE]=(uint32_t)(color>>8);
        vk_image *img=vk_image_get(r,color,16,16,VK_FORMAT_R8G8B8A8_UNORM);CHECK(img);
        VkCommandBuffer cmd=vk_begin(r);
        vk_barrier(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue black={{0,0,0,0}};VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(cmd,img->image,img->layout,&black,1,&range);img->gpu_written=true;img->written_seq=++r->write_seq;
        renderer_draw(r,&g,&draw);renderer_wait_idle(r);
        cmd=vk_begin(r);vk_barrier(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=(VkExtent3D){16,16,1};
        vkCmdCopyImageToBuffer(cmd,img->image,img->layout,raw.buffer,1,&copy);renderer_wait_idle(r);
        unsigned bad=0;uint32_t expected=(round==0||round==3)?0:0xff0000ffu;
        for(unsigned i=0;i<256;i++)bad+=((uint32_t *)raw.mapped)[i]!=expected;
        fprintf(stderr,"export VM round %u: %u incorrect pixels, expected %08x\n",round,bad,expected);correct&=!bad;
    }
    vk_buffer_destroy(r,&raw);renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);
    pal_window_destroy(window);gmem_free(&m);pal_shutdown();CHECK(correct);
}

/* Every quad has live and inactive pixels. SALU inside an EXEC skip still
 * runs for the whole wave when any pixel is live, and survives EXEC restore. */
static void test_gpu_fragment_scalar_flow(void) {
    static const uint32_t vs[]={0x36000a81,0x2c020a81,0x7e000d00,0x7e020d01,
        0x100000ff,0x42800000,0x100202ff,0x42800000,0x060000ff,0xc1000000,
        0x060202ff,0xc1000000,0x7e0402ff,0x3e800000,0x7e0602f2,0xf80008cf,0x03020100,0xbf810000};
    CHECK(pal_init()==PS5C_OK);
    gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr a;CHECK(gmem_map(&m,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"fragment scalar flow",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,vs,sizeof(vs)));
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 fragment scalar flow",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(a>>8);g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(a>>40);
    g.regs[mmSPI_PS_INPUT_ADDR]=1u<<8; /* pixel X */
    g.regs[mmCB_COLOR_CONTROL]=0xCC0010;g.regs[mmCB_TARGET_MASK]=g.regs[mmCB_SHADER_MASK]=15;
    g.regs[mmCB_COLOR0_INFO]=10u<<2;g.regs[mmCB_COLOR0_ATTRIB2]=(15u<<14)|15;
    g.regs[mmSPI_SHADER_COL_FORMAT]=9;g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_TRILIST;
    gpu_draw draw={.count=3,.instances=1};vk_buffer raw;
    CHECK(vk_buffer_create(r,1024,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&raw));bool correct=true;
    for(unsigned round=0;round<9;round++) {
        uint32_t ps[20]={0xbe800380,0xbe82047e,0x7e000f00,0x36000081,
            0x7da40080,0xbf880001,0xbe8003f2,0xbefe0402,
            0x7e000200,0x7e020280,0x7e040280,0x7e0602f2,0xf800180f,0x03020100,0xbf810000};
        size_t ps_bytes=15*sizeof(uint32_t);
        if(round>=3) {
            /* Like a post-process shader: a scalar mask skip followed by WQM
             * must let helper pixels run, before restoring the export mask. */
            const uint32_t quad_ps[]={0xbe800380,0xbe82047e,0x7e000f00,0x36000081,
                0x7e020280,0x7da40080,0x8884807e,0xbf840002,0xbefe0a04,0x7e0202f2,
                0xbefe0402,0x7e000301,0x7e020280,0x7e040280,0x7e0602f2,0xf800180f,0x03020100,0xbf810000};
            memcpy(ps,quad_ps,sizeof(quad_ps));ps_bytes=sizeof(quad_ps);
            if(round>=6) {
                /* VCCZ is another scalar whole-mask decision. */
                memmove(ps+9,ps+8,10*sizeof(uint32_t));
                ps[7]=0xbeea047e; /* s_mov_b64 vcc, exec */
                ps[8]=0xbf860002; /* s_cbranch_vccz past WQM and its VALU */
                ps_bytes+=sizeof(uint32_t);
            }
        }
        if(round%3==1) ps[3]=0x7e000281; /* whole wave inactive */
        if(round%3==2) ps[3]=0x7e000280; /* whole wave live */
        guest_addr pa=a+0x100+round*0x100,color=a+0x4000+round*0x1000;
        CHECK(gmem_write(&m,pa,ps,ps_bytes));
        g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)(pa>>8);g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)(pa>>40);
        g.regs[mmCB_COLOR0_BASE]=(uint32_t)(color>>8);renderer_draw(r,&g,&draw);renderer_wait_idle(r);
        vk_image *img=vk_image_find(r,color,16,16,VK_FORMAT_R8G8B8A8_UNORM);CHECK(img);
        VkCommandBuffer cmd=vk_begin(r);
        vk_barrier(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=(VkExtent3D){16,16,1};
        vkCmdCopyImageToBuffer(cmd,img->image,img->layout,raw.buffer,1,&copy);renderer_wait_idle(r);
        unsigned bad=0;uint32_t expected=round%3==1?0xff000000u:0xff0000ffu;
        for(unsigned i=0;i<256;i++)bad+=((uint32_t *)raw.mapped)[i]!=expected;
        fprintf(stderr,"fragment scalar flow round %u: %u incorrect pixels, expected %08x\n",round,bad,expected);correct&=!bad;
    }
    vk_buffer_destroy(r,&raw);renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);
    pal_window_destroy(window);gmem_free(&m);pal_shutdown();CHECK(correct);
}

static void test_gpu_fragment_many_spills(void) {
    static const uint32_t vs[]={0x36000a81,0x2c020a81,0x7e000d00,0x7e020d01,
        0x100000ff,0x42800000,0x100202ff,0x42800000,0x060000ff,0xc1000000,
        0x060202ff,0xc1000000,0x7e0402ff,0x3e800000,0x7e0602f2,0xf80008cf,0x03020100,0xbf810000};
    uint32_t ps[160]={0xB07C0000u},at=1;
    for(uint32_t i=0;i<32;i++) {
        ps[at++]=0x7e0e02ff;ps[at++]=(i&1)?0x3f800000:0x3e800000;
        ps[at++]=0xDAC00000u|(i==31?65532u:i*256u);ps[at++]=0x00000700u;
    }
    const uint32_t tail[]={0xDAC40000u,0x00000000u,0xDAC4FFFCu,0x01000000u,
        0x7e040280,0x7e0602f2,0xf800180f,0x03020100,0xbf810000};
    memcpy(ps+at,tail,sizeof(tail));at+=PS5C_ARRAY_COUNT(tail);
    CHECK(pal_init()==PS5C_OK);
    gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr a;CHECK(gmem_map(&m,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"fragment spills",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,vs,sizeof(vs)) && gmem_write(&m,a+0x1000,ps,at*4));
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 fragment spills",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(a>>8);g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(a>>40);
    g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x1000)>>8);g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)(a>>40);
    g.regs[mmCB_COLOR_CONTROL]=0xCC0010;g.regs[mmCB_TARGET_MASK]=g.regs[mmCB_SHADER_MASK]=15;
    g.regs[mmCB_COLOR0_INFO]=10u<<2;g.regs[mmCB_COLOR0_ATTRIB2]=(15u<<14)|15;
    g.regs[mmSPI_SHADER_COL_FORMAT]=9;g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_TRILIST;
    guest_addr color=a+0x4000;g.regs[mmCB_COLOR0_BASE]=(uint32_t)(color>>8);
    gpu_draw draw={.count=3,.instances=1};renderer_draw(r,&g,&draw);renderer_wait_idle(r);
    vk_image *img=vk_image_find(r,color,16,16,VK_FORMAT_R8G8B8A8_UNORM);CHECK(img);
    vk_buffer raw;CHECK(vk_buffer_create(r,1024,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&raw));
    VkCommandBuffer cmd=vk_begin(r);vk_barrier(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=(VkExtent3D){16,16,1};
    vkCmdCopyImageToBuffer(cmd,img->image,img->layout,raw.buffer,1,&copy);renderer_wait_idle(r);
    unsigned bad=0;for(unsigned i=0;i<256;i++)bad+=((uint32_t*)raw.mapped)[i]!=0xff00ff40u;
    fprintf(stderr,"32 fragment spills: %u incorrect pixels, first %08x\n",bad,((uint32_t*)raw.mapped)[0]);
    vk_buffer_destroy(r,&raw);renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);
    pal_window_destroy(window);gmem_free(&m);pal_shutdown();CHECK(bad==0);
}

static void test_gpu_constant_blend(void) {
    if(!getenv("PS5C_TEST_GPU"))return;
    static const uint32_t vs[]={0x36000a81,0x2c020a81,0x7e000d00,0x7e020d01,
        0x100000ff,0x42800000,0x100202ff,0x42800000,0x060000ff,0xc1000000,
        0x060202ff,0xc1000000,0x7e0402ff,0x3e800000,0x7e0602f2,0xf80008cf,0x03020100,0xbf810000};
    static const float source[4]={.125f,.25f,.375f,.5f},destination[4]={.5f,.25f,.125f,.75f};
    static const float constants[][4]={{0,0,0,0},{.25f,.5f,.75f,.25f},{.75f,.25f,.5f,.75f},
                                      {.5f,.25f,.125f,.25f},{1,1,1,1}};
    const unsigned factors[]={13,14,19,20};
    CHECK(pal_init()==PS5C_OK);
    gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr a;CHECK(gmem_map(&m,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"constant blend",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,vs,sizeof(vs)));
    for(unsigned program=0;program<2;program++) {
        const float *values=program?source:destination;
        uint32_t ps[]={0x7e0002ff,0,0x7e0202ff,0,0x7e0402ff,0,0x7e0602ff,0,0xf800180f,0x03020100,0xbf810000};
        for(unsigned channel=0;channel<4;channel++)memcpy(&ps[2*channel+1],&values[channel],4);
        CHECK(gmem_write(&m,a+0x100*(program+1),ps,sizeof(ps)));
    }
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 constant blend",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(a>>8);g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(a>>40);
    g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)(a>>40);
    g.regs[mmCB_COLOR_CONTROL]=0xCC0010;g.regs[mmCB_SHADER_MASK]=g.regs[mmCB_TARGET_MASK]=15;
    g.regs[mmCB_COLOR0_ATTRIB2]=(15u<<14)|15;g.regs[mmSPI_SHADER_COL_FORMAT]=9;
    g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_TRILIST;
    gpu_draw draw={.count=3,.instances=1};vk_buffer raw;
    CHECK(vk_buffer_create(r,2048,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&raw));unsigned total_bad=0;
    for(unsigned masked=0;masked<2;masked++)for(unsigned swap=0;swap<2;swap++)for(unsigned side=0;side<2;side++)for(unsigned factor=0;factor<4;factor++) {
        guest_addr color=a+0x4000;
        g.regs[mmCB_TARGET_MASK]=15;
        g.regs[mmCB_COLOR0_INFO]=(12u<<2)|(7u<<8)|(swap<<11);
        g.regs[mmCB_COLOR0_BASE]=(uint32_t)(color>>8);g.regs[mmCB_BLEND0_CONTROL]=0;
        g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x100)>>8);renderer_draw(r,&g,&draw);
        float expected[4];memcpy(expected,destination,sizeof(expected));
        g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x200)>>8);
        unsigned sf=side?factors[factor]:1,df=side?1:factors[factor];
        g.regs[mmCB_TARGET_MASK]=masked?7:15;
        g.regs[mmCB_BLEND0_CONTROL]=(1u<<30)|sf|(df<<8)|(masked?((1u<<29)|(sf<<16)|(df<<24)):0);
        for(unsigned step=0;step<PS5C_ARRAY_COUNT(constants);step++) {
            /* Avoid a final zero destination weight erasing earlier mistakes. */
            unsigned ci=!side && (factor&1)?PS5C_ARRAY_COUNT(constants)-1-step:step;
            for(unsigned channel=0;channel<4;channel++) {
                /* CB_BLEND_RED..ALPHA are FP32 at byte registers 0x28414..20. */
                memcpy(&g.regs[0xA105u+channel],&constants[ci][channel],4);
                float weight=constants[ci][factor>=2?3:channel];
                if(factor&1)weight=1-weight;
                if(!masked || channel!=3)
                    expected[channel]=source[channel]*(side?weight:1)+expected[channel]*(side?1:weight);
            }
            renderer_draw(r,&g,&draw);
            if(step==1)renderer_wait_idle(r); /* same pipeline within and across command buffers */
        }
        renderer_wait_idle(r);
        vk_image *img=vk_image_find(r,color,16,16,VK_FORMAT_R16G16B16A16_SFLOAT);CHECK(img);
        VkCommandBuffer cmd=vk_begin(r);vk_barrier(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=(VkExtent3D){16,16,1};
        vkCmdCopyImageToBuffer(cmd,img->image,img->layout,raw.buffer,1,&copy);renderer_wait_idle(r);
        unsigned bad=0;
        for(unsigned pixel=0;pixel<256;pixel++)for(unsigned channel=0;channel<4;channel++) {
            unsigned logical=swap && (channel==0 || channel==2)?2-channel:channel;
            uint16_t bits=((uint16_t*)raw.mapped)[pixel*4+channel];
            float value=ldexpf((float)((bits&1023u)|1024u),(int)((bits>>10)&31u)-25);
            if(!(bits&0x7fffu))value=0;
            if(bits&0x8000u)value=-value;
            bad+=fabsf(value-expected[logical])>.0001f;
        }
        fprintf(stderr,"constant blend mask %x swap %u source-factor %u factor %u: %u wrong channels\n",masked?7:15,swap,side,factors[factor],bad);total_bad+=bad;
    }
    vk_buffer_destroy(r,&raw);renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);
    pal_window_destroy(window);gmem_free(&m);pal_shutdown();CHECK(total_bad==0);
}

static void test_gpu_hdr_channel_order(void) {
    if (!getenv("PS5C_TEST_GPU")) return;
    static const uint32_t vs[]={0x36000a81,0x2c020a81,0x7e000d00,0x7e020d01,
        0x100000ff,0x42800000,0x100202ff,0x42800000,0x060000ff,0xc1000000,
        0x060202ff,0xc1000000,0x7e0402ff,0x3e800000,0x7e0602f2,0xf80008cf,0x03020100,0xbf810000};
    static const uint32_t ps[]={0x7e0002f2,0x7e0202ff,0x3e800000,0x7e0402f0,
        0x7e0602f2,0xf800180f,0x03020100,0xbf810000};
    CHECK(pal_init()==PS5C_OK);
    gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr a;CHECK(gmem_map(&m,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"HDR channel order",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,vs,sizeof(vs)));CHECK(gmem_write(&m,a+0x100,ps,sizeof(ps)));
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 HDR channel order",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(a>>8);g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(a>>40);
    g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x100)>>8);g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)(a>>40);
    g.regs[mmCB_COLOR_CONTROL]=0xCC0010;g.regs[mmCB_SHADER_MASK]=15;
    g.regs[mmCB_COLOR0_ATTRIB2]=(15u<<14)|15;g.regs[mmSPI_SHADER_COL_FORMAT]=9;
    g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_TRILIST;
    gpu_draw draw={.count=3,.instances=1};vk_buffer raw;
    CHECK(vk_buffer_create(r,2048,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&raw));bool correct=true;
    const uint32_t masks[]={15,15,1,4,15,15,1,15};
    const uint16_t values[]={0x3c00,0x3400,0x3800,0x3c00};
    for(unsigned round=0;round<PS5C_ARRAY_COUNT(masks);round++) {
        bool alt=round!=0;guest_addr color=a+0x4000+round*0x1000;
        bool integer=round==5 || round==6, compressed=round==4 || round==6;
        uint32_t shader[]={0x7e0002ff,integer?11u:0x3f800000u,
                           0x7e0202ff,integer?22u:0x3e800000u,
                           0x7e0402ff,integer?33u:0x3f000000u,
                           0x7e0602ff,integer?44u:0x3f800000u,
                           0xf800180fu|((uint32_t)compressed<<10),0x03020100,0xbf810000};
        if(compressed) {
            shader[1]=integer?0x0016000bu:0x34003c00u;
            shader[3]=integer?0x002c0021u:0x3c003800u;
        }
        guest_addr program=a+0x100*(round+1);
        CHECK(gmem_write(&m,program,shader,sizeof(shader)));
        g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)(program>>8);
        g.regs[mmSPI_SHADER_COL_FORMAT]=integer?7:compressed?4:9;
        /* SRC_COLOR blending must square the logical channels before storage. */
        g.regs[mmCB_BLEND0_CONTROL]=round==7?(1u<<30)|2u:0;
        g.regs[mmCB_COLOR0_INFO]=(12u<<2)|((integer?4u:7u)<<8)|((uint32_t)alt<<11);
        g.regs[mmCB_TARGET_MASK]=masks[round];g.regs[mmCB_COLOR0_BASE]=(uint32_t)(color>>8);
        renderer_draw(r,&g,&draw);renderer_wait_idle(r);
        vk_image *img=vk_image_find(r,color,16,16,integer?VK_FORMAT_R16G16B16A16_UINT:VK_FORMAT_R16G16B16A16_SFLOAT);CHECK(img);
        VkCommandBuffer cmd=vk_begin(r);
        vk_barrier(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=(VkExtent3D){16,16,1};
        vkCmdCopyImageToBuffer(cmd,img->image,img->layout,raw.buffer,1,&copy);renderer_wait_idle(r);
        unsigned bad=0;
        for(unsigned i=0;i<256;i++)for(unsigned k=0;k<4;k++) {
            unsigned source=alt && (k==0 || k==2)?2-k:k;
            uint16_t expected=integer?(uint16_t)(11*(source+1)):values[source];
            if(round==7 && source==1)expected=0x2c00; /* .25 squared = .0625 */
            if(round==7 && source==2)expected=0x3400; /* .5 squared = .25 */
            if(!((masks[round]>>source)&1))expected=0;
            bad+=((uint16_t *)raw.mapped)[i*4+k]!=expected;
        }
        fprintf(stderr,"HDR swap round %u mask %x: %u incorrect channels\n",round,masks[round],bad);correct&=!bad;
    }
    vk_buffer_destroy(r,&raw);renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);
    pal_window_destroy(window);gmem_free(&m);pal_shutdown();CHECK(correct);
}

/* A buffer fill over an existing color resource must also replace its GPU
 * contents; keeping the old image leaves masks from earlier frames visible. */
static void gpu_buffer_pattern_clear_layout(bool mask_first) {
    if(!getenv("PS5C_TEST_GPU"))return;
    uint32_t fill[]={0xbfa00001,0xd7460000,0x04010c0c,0xf4201a84,0xfa000000,
        0xbf8cc07f,0x7da8006a,0xbf880009,0xf4201a84,0xfa000004,0xbf8cc07f,0x3602006a,
        0xe0002000,0x80000101,0xbf8c3f70,0xe0102000,0x80010100,0xbf810000};
    if(mask_first){fill[4]=0xfa000004;fill[9]=0xfa000000;}
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"buffer pattern clear",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,fill,sizeof(fill)));
    guest_addr pattern=a+0x1000,params=a+0x1100,color=a+0x4000;
    uint32_t count_index=mask_first?1:0,mask_index=mask_first?0:1;
    uint32_t limits[4]={0};limits[count_index]=128;
    CHECK(gmem_write(&m,params,limits,sizeof(limits)));
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 buffer/image clear",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(a>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(a>>40);
    g.regs[mmCOMPUTE_PGM_RSRC2]=(12u<<1)|(1u<<7);
    g.regs[mmCOMPUTE_NUM_THREAD_X]=64;g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    u[0]=(uint32_t)pattern;u[1]=(uint32_t)(pattern>>32)|(4u<<16);u[2]=1;u[3]=0x14004;
    u[4]=(uint32_t)color;u[5]=(uint32_t)(color>>32)|(4u<<16);u[6]=128;u[7]=0x14004;
    u[8]=(uint32_t)params;u[9]=(uint32_t)(params>>32)|(16u<<16);u[10]=1;u[11]=0x4dfac;
    if(mask_first){u[3]|=0x200;u[7]|=0x200;} /* unused W selector; X remains unchanged */
    vk_buffer raw;CHECK(vk_buffer_create(r,512,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&raw));
    const uint8_t bytes[]={0,0x33,0xfe,0x77};bool correct=true;
    for(unsigned round=0;round<PS5C_ARRAY_COUNT(bytes);round++) {
        uint32_t word=bytes[round]*0x01010101u,uniform[]={word,word,word,word};
        CHECK(gmem_write(&m,pattern,uniform,sizeof(uniform)));
        u[2]=4;limits[mask_index]=round;CHECK(gmem_write(&m,params,limits,sizeof(limits)));
        memset(gmem_host(&m,color,512),0xa5,512);
        VkCommandBuffer cmd=vk_begin(r);vk_image *img=vk_image_get(r,color,16,16,VK_FORMAT_R8G8_UNORM);CHECK(img);
        vk_barrier(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue white={{1,1,1,1}};VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(cmd,img->image,img->layout,&white,1,&range);img->gpu_written=true;img->written_seq=++r->write_seq;
        renderer_dispatch(r,&g,2,1,1);renderer_sync_mem(r,color,512);
        const uint8_t *guest=gmem_host(&m,color,512);unsigned guest_bad=0;
        for(unsigned i=0;i<512;i++)guest_bad+=guest[i]!=bytes[round];
        cmd=vk_begin(r);vk_barrier(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copy.imageExtent=(VkExtent3D){16,16,1};vkCmdCopyImageToBuffer(cmd,img->image,img->layout,raw.buffer,1,&copy);renderer_wait_idle(r);
        unsigned image_bad=0;for(unsigned i=0;i<512;i++)image_bad+=((uint8_t *)raw.mapped)[i]!=bytes[round];
        fprintf(stderr,"buffer pattern clear %u: guest %u, image %u incorrect bytes\n",round,guest_bad,image_bad);
        correct&=!guest_bad && !image_bad;
    }
    /* A nonuniform pattern falls back to the GPU. Counts smaller than the
     * dispatch must preserve the remaining bytes, including a single dword. */
    u[4]=(uint32_t)(a+0x8000);u[2]=2;
    uint32_t words[]={0x12345678,0x87654321};CHECK(gmem_write(&m,pattern,words,sizeof(words)));
    for(unsigned round=0;round<5;round++) {
        limits[count_index]=round==0 || round==4?128:round==1?1:round==2?64:0;
        limits[mask_index]=round==0?1:0;
        u[6]=round==4?37:128;
        CHECK(gmem_write(&m,params,limits,sizeof(limits)));
        memset(gmem_host(&m,a+0x8000,512),0xa5,512);
        renderer_dispatch(r,&g,2,1,1);renderer_sync_mem(r,a+0x8000,512);
        const uint32_t *guest=gmem_host(&m,a+0x8000,512);unsigned bad=0;
        unsigned count=limits[count_index]<u[6]?limits[count_index]:u[6];
        for(unsigned i=0;i<128;i++)bad+=guest[i]!=(i<count?words[i&limits[mask_index]]:0xa5a5a5a5u);
        fprintf(stderr,"buffer pattern bounds %u: %u incorrect words\n",round,bad);correct&=!bad;
    }
    vk_buffer_destroy(r,&raw);renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);
    pal_window_destroy(window);gmem_free(&m);pal_shutdown();CHECK(correct);
}

static void test_gpu_buffer_pattern_clear(void) {
    gpu_buffer_pattern_clear_layout(false);
    gpu_buffer_pattern_clear_layout(true);
}

static void test_gpu_fp32_instruction_rounding(void) {
    if(!getenv("PS5C_TEST_GPU"))return;
    /* (1+2^-23)*(1-2^-23) rounds to one before the separate add of -1.
     * Contracting the two instructions instead returns -2^-46. */
    uint32_t code[]={0xf4280000,0xfa000000,0xbf8cc07f,0x7e000200,0x7e020201,
        0x10040300,0x060604f3,0x7e080303,0xe0700000,0x80030400,0xbf810000};
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"FP32 rounding",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,code,sizeof(code)));
    uint32_t inputs[]={0x3f800001,0x3f7ffffe,0,0};CHECK(gmem_write(&m,a+0x1000,inputs,sizeof(inputs)));
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 FP32 rounding",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(a>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(a>>40);
    g.regs[mmCOMPUTE_PGM_RSRC2]=16u<<1;
    g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;guest_addr input=a+0x1000,out=a+0x2000;
    u[0]=(uint32_t)input;u[1]=(uint32_t)(input>>32);u[2]=16;u[3]=0x16204;
    u[12]=(uint32_t)out;u[13]=(uint32_t)(out>>32);u[14]=4;u[15]=0x16204;
    bool correct=true;
    for(unsigned round=0;round<2;round++) {
        if(round) {code[5]=0xd54b0003;code[6]=0x03ce0300;} /* v_fma_f32 v3,v0,v1,-1 */
        guest_addr program=a+0x100*round;CHECK(gmem_write(&m,program,code,sizeof(code)));
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(program>>8);
        renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,4);
        uint32_t value=~0u;CHECK(gmem_read(&m,out,&value,4));
        uint32_t expected=round?0xa8800000u:0;
        fprintf(stderr,"FP32 %s instructions: %08x (expected %08x)\n",round?"fused":"separate",value,expected);
        correct&=value==expected;
    }
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&m);pal_shutdown();
    CHECK(correct);
}

/* A shader can explicitly bypass alpha-to-coverage while the global control
 * stays enabled. Alpha zero then must not remove an opaque material. */
static void test_gpu_alpha_coverage_override(void) {
    if(!getenv("PS5C_TEST_GPU"))return;
    static const uint32_t vs[]={0x36000a81,0x2c020a81,0x7e000d00,0x7e020d01,
        0x100000ff,0x42800000,0x100202ff,0x42800000,0x060000ff,0xc1000000,
        0x060202ff,0xc1000000,0x7e0402f0,0x7e0602f2,0xf80008cf,0x03020100,0xbf810000};
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"alpha coverage override",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,vs,sizeof(vs)));
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 alpha coverage override",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(a>>8);g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(a>>40);
    g.regs[mmCB_COLOR_CONTROL]=0xCC0010;g.regs[mmCB_TARGET_MASK]=g.regs[mmCB_SHADER_MASK]=15;
    g.regs[mmCB_COLOR0_INFO]=10u<<2;g.regs[mmCB_COLOR0_ATTRIB2]=(15u<<14)|15;
    g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_TRILIST;gpu_draw draw={.count=3,.instances=1};
    vk_buffer raw;CHECK(vk_buffer_create(r,1024,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&raw));bool correct=true;
    for(unsigned round=0;round<6;round++) {
        bool enabled=round!=0,disabled=round==2 || round==5,alpha_one=round==3,compressed=round>=4;
        uint32_t ps[]={0x7e0002ff,compressed?0x00003c00u:0x3f800000u,
            0x7e020280,0x7e040280,0x7e060280,0xf800180fu|((uint32_t)compressed<<10),0x03020100,0xbf810000};
        if(alpha_one)ps[4]=0x7e0602f2;
        guest_addr program=a+0x100*(round+1),color=a+0x4000+0x1000*round;
        CHECK(gmem_write(&m,program,ps,sizeof(ps)));
        g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)(program>>8);g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)(a>>40);
        g.regs[mmSPI_SHADER_COL_FORMAT]=compressed?4:9;g.regs[mmDB_SHADER_CONTROL]=disabled?0x800:0;
        g.regs[0xA2DCu]=enabled?1:0; /* DB_ALPHA_TO_MASK */
        g.regs[mmCB_COLOR0_BASE]=(uint32_t)(color>>8);renderer_draw(r,&g,&draw);renderer_wait_idle(r);
        vk_image *img=vk_image_find(r,color,16,16,VK_FORMAT_R8G8B8A8_UNORM);CHECK(img);
        VkCommandBuffer cmd=vk_begin(r);vk_barrier(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=(VkExtent3D){16,16,1};
        vkCmdCopyImageToBuffer(cmd,img->image,img->layout,raw.buffer,1,&copy);renderer_wait_idle(r);
        unsigned painted=0;for(unsigned i=0;i<256;i++)painted+=((uint8_t *)raw.mapped)[i*4]==255;
        unsigned expected=!enabled || disabled || alpha_one?256:0;
        fprintf(stderr,"alpha coverage round %u: %u painted, expected %u\n",round,painted,expected);correct&=painted==expected;
    }
    vk_buffer_destroy(r,&raw);renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);
    pal_window_destroy(window);gmem_free(&m);pal_shutdown();CHECK(correct);
}

/* A scalar snapshot read earlier in a submission must be refreshed after
 * a compute store, including stores reusing an existing writeback window. */
static void test_gpu_scalar_snapshot_writeback(void) {
    if(!getenv("PS5C_TEST_GPU"))return;
    static const uint32_t reader[]={0xf4000400,0xfa000000,0xbf8cc07f,
        0x7e080210,0xe0700000,0x80030400,0xbf810000};
    static const uint32_t writer[]={0x7e080208,0xe0700000,0x80010400,0xbf810000};
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"scalar snapshot writeback",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,reader,sizeof(reader)));CHECK(gmem_write(&m,a+0x100,writer,sizeof(writer)));
    uint32_t buffered_reader[PS5C_ARRAY_COUNT(reader)];memcpy(buffered_reader,reader,sizeof(reader));
    buffered_reader[0]=0xf4200400; /* same load through S_BUFFER, staged on the GPU */
    CHECK(gmem_write(&m,a+0x200,buffered_reader,sizeof(buffered_reader)));
    uint32_t one=1;guest_addr input=a+0x1000,out=a+0x2000;CHECK(gmem_write(&m,input,&one,4));
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 scalar snapshot",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(a>>40);g.regs[mmCOMPUTE_PGM_RSRC2]=16u<<1;
    g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    u[0]=(uint32_t)input;u[1]=(uint32_t)(input>>32);
    u[4]=(uint32_t)input;u[5]=(uint32_t)(input>>32);u[6]=4;u[7]=0x16204;
    u[12]=(uint32_t)out;u[13]=(uint32_t)(out>>32);u[14]=4;u[15]=0x16204;
    bool correct=true;
    uint32_t rounds=getenv("PS5C_EVAL_CACHE_CHECK")?6:3;
    bool saw_rotation=false;
    for(uint32_t round=0;round<rounds;round++) {
        CHECK(gmem_write(&m,input,&one,4));u[4]=(uint32_t)(input+(round==2?16:0));u[8]=2;
        guest_addr read_program=a+(round>=4?0x200:0);
        u[2]=round==5?4096:round==4?4:0;u[3]=round>=4?0x16204:0;
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(read_program>>8);renderer_dispatch(r,&g,1,1,1);
        if(round>=3) {
            /* Diagnostic check: a cached scalar snapshot cannot represent
             * changed CPU bytes, even when its SGPRs and epoch still match. */
            uint32_t four=4;CHECK(gmem_write(&m,input,&four,4));
        } else {
            g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x100)>>8);renderer_dispatch(r,&g,1,1,1);
            if(round==1) {u[8]=3;renderer_dispatch(r,&g,1,1,1);}
        }
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(read_program>>8);renderer_dispatch(r,&g,1,1,1);
        if(round>=3)renderer_dispatch(r,&g,1,1,1); /* refreshed entry remains reusable */
        /* The result remains in its owner slot; timeline retirement permits
         * the next batch to use the other slot. Exercise that slot before
         * demanding the original results, rather than requiring serialization. */
        guest_addr label=a+0x3000,scratch=a+0x5000;uint32_t zero=0,seen=0;
        CHECK(gmem_write(&m,label,&zero,4));
        gpu_release release={.addr=label,.data=round+1,.data_sel=1};
        CHECK(renderer_release(r,&g,&release));
        CHECK(gmem_read(&m,label,&seen,4) && seen==0);
        bool rotate=vk_work_can_overlap(r);
        saw_rotation|=rotate;
        const char *retire=getenv("PS5C_VK_RETIRE"),*overlap=getenv("PS5C_WORK_OVERLAP");
        if(!r->work_timeline || (retire && !strcmp(retire,"0")) || (overlap && !strcmp(overlap,"0")))CHECK(!rotate);
        uint32_t slot=r->work_index;
        vk_flush(r);vk_begin(r);
        CHECK(r->work_index==(rotate?(slot^1):slot));
        uint32_t saved[3]={u[4],u[5],u[8]},program=g.regs[mmCOMPUTE_PGM_LO];
        u[4]=(uint32_t)scratch;u[5]=(uint32_t)(scratch>>32);u[8]=0x12345678;
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x100)>>8);renderer_dispatch(r,&g,1,1,1);
        u[4]=saved[0];u[5]=saved[1];u[8]=saved[2];g.regs[mmCOMPUTE_PGM_LO]=program;
        renderer_sync_mem(r,out,4);uint32_t value=0;CHECK(gmem_read(&m,out,&value,4));
        uint32_t expected=round==0?2:round==1?3:round==2?1:4;
        uint32_t written=0;CHECK(gmem_read(&m,input+(round==2?16:0),&written,4));
        fprintf(stderr,"scalar snapshot after compute write %u: %u (expected %u)\n",round,value,expected);
        correct&=value==expected && written==(round>=3?4u:round==1?3u:2u);
        renderer_wait_idle(r);
        CHECK(gmem_read(&m,label,&seen,4) && seen==round+1);
        CHECK(gmem_read(&m,scratch,&seen,4) && seen==0x12345678);
    }
    const char *retire=getenv("PS5C_VK_RETIRE"),*overlap=getenv("PS5C_WORK_OVERLAP");
    bool require_rotation=r->work_timeline && retire && !strcmp(retire,"1") && overlap && !strcmp(overlap,"1");
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&m);pal_shutdown();
    if(require_rotation)CHECK(saw_rotation);
    CHECK(correct);
}

/* Keep the initial CPU/GPU version identical, then merge only changed GPU
 * bytes. A later CPU write to an unchanged byte must survive the writeback. */
static void test_gpu_writeback_initial_version(void) {
    if(!getenv("PS5C_TEST_GPU"))return;
    static const uint32_t writer[]={0x7e080208,0xe0700000,0x80010400,0xbf810000};
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,8u<<20,GPROT_RW,0,"writeback initial version",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,writer,sizeof(writer)));
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 writeback version",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(a>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(a>>40);
    g.regs[mmCOMPUTE_PGM_RSRC2]=16u<<1;
    g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    guest_addr input=a+0x4000;uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    u[4]=(uint32_t)input;u[5]=(uint32_t)(input>>32);u[7]=0x16204;u[8]=0x5a5aaabb;
    const uint32_t sizes[]={32,(3u<<20)+19};bool correct=true;
    for(unsigned round=0;round<8;round++) {
        uint32_t size=sizes[round%PS5C_ARRAY_COUNT(sizes)];u[6]=size;
        uint8_t *bytes=gmem_host(&m,input,size+16);CHECK(bytes);memset(bytes,0x5a,size+16);
        renderer_dispatch(r,&g,1,1,1);
        if(size>4096) {
            u[4]=(uint32_t)(input+4092);u[6]=size-4092;u[8]=0x5a223344;
            renderer_dispatch(r,&g,1,1,1);
            u[4]=(uint32_t)(input+4096);u[6]=size-4096;u[8]=0x55667788;
            renderer_dispatch(r,&g,1,1,1);
            u[4]=(uint32_t)input;u[6]=size;u[8]=0x5a5aaabb;
        }
        /* Later dispatches may flush the first store while rebinding. Write
         * only the unchanged byte, so that case has the same expected result
         * whether the GPU value has already become CPU-visible or not. */
        if(size>4096)bytes[3]=0x99;
        else {uint32_t cpu=0x995a5a5a;CHECK(gmem_write(&m,input,&cpu,4));}
        bytes[17]=0x71;bytes[size-1]=0x83;
        if(size>4096)bytes[4095]=0x91;
        renderer_sync_mem(r,input,size);
        uint32_t first;memcpy(&first,bytes,4);unsigned bad=first!=0x995aaabb;
        const uint8_t across_page[]={0x44,0x33,0x22,0x91,0x88,0x77,0x66,0x55};
        for(uint32_t i=4;i<size+16;i++) {
            uint8_t expected=i==17?0x71:i==size-1?0x83:0x5a;
            if(size>4096 && i>=4092 && i<4100)expected=across_page[i-4092];
            bad+=bytes[i]!=expected;
        }
        fprintf(stderr,"writeback initial version %u bytes: %u incorrect values\n",size,bad);correct&=!bad;
    }
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&m);pal_shutdown();
    CHECK(correct);
}

/* Temporal history uses a second RGB10A2 target, with VM on that export
 * and the alpha half packed by the VOP3 form of CVT_PKRTZ. */
static void test_gpu_temporal_history_export(void) {
    if(!getenv("PS5C_TEST_GPU"))return;
    static const uint32_t vs[]={0x36000a81,0x2c020a81,0x7e000d00,0x7e020d01,
        0x100000ff,0x42800000,0x100202ff,0x42800000,0x060000ff,0xc1000000,
        0x060202ff,0xc1000000,0x7e040280,0x7e0602f2,0xf80008cf,0x03020100,0xbf810000};
    uint32_t ps[]={0x7e0802ff,0x3e800000,0x7e0a02ff,0x3f000000,0x7e0c02ff,0x3f400000,
        0x5e000b04,0xd52f0001,0x0001e506,0x5e040d06,0xf800141f,0x00000100,0xf8000c0f,0x00000200,0xbf810000};
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"temporal history export",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,vs,sizeof(vs)));pal_window *window=NULL;
    CHECK(pal_window_open("ZeraPS5 temporal history export",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(a>>8);g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(a>>40);
    g.regs[mmCB_COLOR_CONTROL]=0xCC0010;g.regs[mmCB_TARGET_MASK]=g.regs[mmCB_SHADER_MASK]=0xf7;
    g.regs[mmSPI_SHADER_COL_FORMAT]=0x44;g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_TRILIST;
    gpu_draw draw={.count=3,.instances=1};vk_buffer raw;CHECK(vk_buffer_create(r,1024,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&raw));bool correct=true;
    for(unsigned round=0;round<3;round++) {
        if(round) {ps[7]=0x7e0e02f2;ps[8]=0x5e020f06;}
        guest_addr program=a+0x100*(round+1),color=a+0x4000+round*0x4000,history=color+0x2000;
        if(round==2) {
            /* Invalid previous history masks off the blend body, then EXEC
             * is restored twice before the VM export. Match that control flow. */
            uint32_t masked[40]={0xbe9e047e,0xbefe0a7e,0x7e0002f2,0x7c080080,0x8a9e6a1e,
                0xbf840011,0xbefe0a1e,0xbe98047e,0x7e000280,0x7c2400f2,0xbf880001,0xbe980480,0xbefe0418};
            ps[7]=0xd52f0001;ps[8]=0x0001e506;
            memcpy(masked+13,ps,10*4);masked[23]=0xbefe041e;memcpy(masked+24,ps+10,5*4);
            CHECK(gmem_write(&m,program,masked,29*4));
        } else CHECK(gmem_write(&m,program,ps,sizeof(ps)));
        g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)(program>>8);g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)(a>>40);
        for(unsigned rt=0;rt<2;rt++) {
            unsigned o=rt*GPU_CB_STRIDE;g.regs[mmCB_COLOR0_BASE+o]=(uint32_t)((rt?history:color)>>8);
            g.regs[mmCB_COLOR0_INFO+o]=rt?9u<<2:(6u<<2)|(7u<<8);
            g.regs[mmCB_COLOR0_ATTRIB2+rt]=(15u<<14)|15;
        }
        renderer_draw(r,&g,&draw);renderer_wait_idle(r);
        vk_image *img=vk_image_find(r,history,16,16,VK_FORMAT_A2B10G10R10_UNORM_PACK32);CHECK(img);
        VkCommandBuffer cmd=vk_begin(r);vk_barrier(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=(VkExtent3D){16,16,1};
        vkCmdCopyImageToBuffer(cmd,img->image,img->layout,raw.buffer,1,&copy);renderer_wait_idle(r);
        unsigned bad=0;const uint32_t *pixels=raw.mapped;
        for(unsigned i=0;i<256;i++)bad+=(pixels[i]>>30)!=3 || (pixels[i]&1023)<255 || (pixels[i]&1023)>256 ||
            ((pixels[i]>>10)&1023)<511 || ((pixels[i]>>10)&1023)>512 || ((pixels[i]>>20)&1023)<767 || ((pixels[i]>>20)&1023)>768;
        fprintf(stderr,"temporal RGB10A2 %s: %u incorrect pixels, first %08x\n",round==2?"EXEC restore":round?"VOP2":"VOP3",bad,pixels[0]);correct&=!bad;
    }
    vk_buffer_destroy(r,&raw);renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&m);pal_shutdown();CHECK(correct);
}

/* A previous upload's hash cannot override a newer GPU drawing merely
 * because the CPU changed the bytes BEFORE that drawing was submitted. */
static void gpu_texture_write_order(bool large) {
    if(!getenv("PS5C_TEST_GPU"))return;
    static const uint32_t vs[]={0x36000a81,0x2c020a81,0x7e000d00,0x7e020d01,
        0x100000ff,0x42800000,0x100202ff,0x42800000,0x060000ff,0xc1000000,
        0x060202ff,0xc1000000,0x7e040280,0x7e0602f2,0xf80008cf,0x03020100,0xbf810000};
    static const uint32_t ps[]={0x7e0002f2,0x7e020280,0x7e040280,0x7e0602f2,0xf800180f,0x03020100,0xbf810000};
    static const uint32_t load[]={0x7e000280,0x7e020280,0xf0000f08,0x00000400,
        0xbf8c3f70,0xe0780000,0x80030400,0xbf810000};
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr a;CHECK(gmem_map(&m,0,64*GUEST_PAGE_SIZE,GPROT_RW,0,"image ownership",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,vs,sizeof(vs)) && gmem_write(&m,a+0x100,ps,sizeof(ps)) && gmem_write(&m,a+0x200,load,sizeof(load)));
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 small image write order",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);guest_addr image=a+0x4000,out=a+0x1000;
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;u[0]=(uint32_t)(image>>8);u[1]=(uint32_t)(image>>40)|(56u<<20)|(3u<<30);
    u[2]=3|(15u<<14);u[3]=(9u<<28)|0xfac;u[12]=(uint32_t)out;u[13]=(uint32_t)(out>>32);u[14]=16;u[15]=0x16204;
    g.regs[mmCOMPUTE_PGM_RSRC2]=16u<<1;g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x200)>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(a>>40);
    renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,16); /* establish the upload hash */
    g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(a>>8);g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(a>>40);
    g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x100)>>8);g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)(a>>40);
    g.regs[mmCB_COLOR_CONTROL]=0xCC0010;g.regs[mmCB_TARGET_MASK]=g.regs[mmCB_SHADER_MASK]=15;g.regs[mmSPI_SHADER_COL_FORMAT]=9;
    g.regs[mmCB_COLOR0_BASE]=(uint32_t)(image>>8);g.regs[mmCB_COLOR0_INFO]=10u<<2;g.regs[mmCB_COLOR0_ATTRIB2]=(15u<<14)|15;
    g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_TRILIST;gpu_draw draw={.count=3,.instances=1};bool correct=true;
    const uint32_t w=large?257:16,h=large?96:16;
    const size_t pitch=(w*4u+255u)&~255u,span=pitch*h;
    if(large) {
        u[1]&=~(3u<<30);u[2]=((w-1)>>2)|((h-1)<<14);
        g.regs[mmCB_COLOR0_ATTRIB2]=((w-1)<<14)|(h-1);
        renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,16);
    }
    for(unsigned round=0;round<4;round++) {
        if(round==3) {
            /* A 4x4 RGBA8 image in a 256-byte standard tile leaves the
             * final dword outside its active texels. */
            image=a+0x40000;u[0]=(uint32_t)(image>>8);
            u[2]=large?((w-1)>>2)|((h-1)<<14):3u<<14;u[3]=(9u<<28)|(1u<<20)|0xfac;
            g.regs[mmCB_COLOR0_BASE]=(uint32_t)(image>>8);
            g.regs[mmCB_COLOR0_ATTRIB2]=large?((w-1)<<14)|(h-1):(3u<<14)|3;
            renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,16);
        }
        memset(gmem_host(&m,image,span),255,span);renderer_submit_begin(r);renderer_draw(r,&g,&draw);
        if(round==1) { /* CPU overwrites AFTER drawing: its new green bytes must still win. */
            renderer_wait_idle(r);uint32_t *p=gmem_host(&m,image,span);for(size_t n=0;n<span/4;n++)p[n]=0xff00ff00u;renderer_submit_begin(r);
        }
        if(round==2) { /* 16 RGBA8 texels occupy 64 of the 256 row bytes. */
            renderer_wait_idle(r);if(large) {
                uint8_t *p=gmem_host(&m,image,span);
                for(uint32_t y=0;y<h;y++)memset(p+pitch*y+w*4u,0,pitch-w*4u);
            } else ((uint32_t*)gmem_host(&m,image,4096))[16]=0;renderer_submit_begin(r);
        }
        if(round==3) {
            renderer_wait_idle(r);
            if(large) {
                uint8_t *p=gmem_host(&m,image,span);
                for(uint32_t y=0;y<h;y++)for(uint32_t x=w;x<264;x++) {
                    uint64_t off=((uint64_t)(y/8)*33+x/8)*256+std_offset(1,x,y,4);
                    memset(p+off,0,4);
                }
            } else ((uint32_t*)gmem_host(&m,image,4096))[63]=0;
            renderer_submit_begin(r);
        }
        renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,16);float got[4];CHECK(gmem_read(&m,out,got,sizeof(got)));
        fprintf(stderr,"%s texture %s GPU draw: %g %g %g %g\n",large?"large":"small",round==3?"tiled padding after":round==2?"linear padding after":round?"CPU after":"CPU before",(double)got[0],(double)got[1],(double)got[2],(double)got[3]);
        correct&=got[0]==(round==1?0.f:1.f) && got[1]==(round==1?1.f:0.f) && got[2]==0.f && got[3]==1.f;
    }
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&m);pal_shutdown();CHECK(correct);
}

static void test_gpu_small_texture_write_order(void) {gpu_texture_write_order(false);}
static void test_gpu_large_texture_write_order(void) {gpu_texture_write_order(true);}

static void test_gpu_deferred_snapshots(void) {
    if(!getenv("PS5C_TEST_GPU"))return;
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 deferred snapshots",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);vk_begin(r);
    vk_image *src=vk_image_get(r,0x100000,16,16,VK_FORMAT_R8G8B8A8_UNORM);CHECK(src);
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    for(unsigned n=0;n<3;n++) {
        VkCommandBuffer cmd=vk_begin(r);vk_barrier(cmd,src,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue value={0};value.float32[n]=1.f;value.float32[3]=1.f;
        vkCmdClearColorImage(cmd,src->image,src->layout,&value,1,&range);src->gpu_written=true;src->written_seq=++r->write_seq;
        if(n<2)CHECK(vk_capture_defer(r,src,n?"unused-green.bmp":"unused-red.bmp"));
    }
    /* Only now wait: the same source was overwritten twice after the first
     * snapshot. Neither capture may silently turn into the final blue image. */
    renderer_wait_idle(r);CHECK(r->capture_count==2 && !src->feedback);
    vk_buffer raw;CHECK(vk_buffer_create(r,1024,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&raw));bool correct=true;
    for(unsigned n=0;n<3;n++) {
        vk_image *image=n<2?r->captures[n].image:src;VkCommandBuffer cmd=vk_begin(r);
        vk_barrier(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=(VkExtent3D){16,16,1};
        vkCmdCopyImageToBuffer(cmd,image->image,image->layout,raw.buffer,1,&copy);renderer_wait_idle(r);
        uint32_t expected=0xff000000u|(255u<<(n*8));for(unsigned i=0;i<256;i++)correct&=((uint32_t*)raw.mapped)[i]==expected;
        fprintf(stderr,"deferred snapshot %u: %08x expected %08x\n",n,((uint32_t*)raw.mapped)[0],expected);
    }
    vk_buffer_destroy(r,&raw);renderer_destroy(r);pal_window_destroy(window);gmem_free(&m);pal_shutdown();CHECK(correct);
}

/* A sky triangle can cross W=0 while Z=W throughout. Against foreground
 * depth, neither those triangles nor an ordinary far plane may paint. */
static void test_gpu_constant_depth_bias(void) {
    if(!getenv("PS5C_TEST_GPU"))return;
    static const uint32_t vs[]={0xe0382000,0x80020005,0xbf8c3f70,0xf80008cf,0x03030100,0xbf810000};
    static const uint32_t ps[]={0x7e0002f2,0x7e020280,0x7e040280,0x7e0602f2,0xf800180f,0x03020100,0xbf810000};
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,8u*1024u*1024u,GPROT_RW,0,"constant depth bias",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,vs,sizeof(vs)) && gmem_write(&m,a+0x100,ps,sizeof(ps)));
    float positions[192][4];const float ws[]={1,-1,.00001f,-.00001f,4,-4,0,100};
    for(unsigned i=0;i<192;i++) {
        positions[i][0]=i%3==0?-3.f:3.f;positions[i][1]=i%3==2?3.f:-3.f;
        positions[i][2]=positions[i][3]=ws[(i/3+i%3)%PS5C_ARRAY_COUNT(ws)];
    }
    CHECK(gmem_write(&m,a+0x1000,positions,sizeof(positions)));
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 constant-depth bias",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    const uint32_t width=1024,height=576;guest_addr color=a+0x10000,depth=a+0x400000;
    g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(a>>8);g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(a>>40);
    g.regs[mmSPI_SHADER_PGM_RSRC2_GS]=4u<<1;uint32_t *u=g.regs+mmSPI_SHADER_USER_DATA_GS_0;
    u[0]=(uint32_t)(a+0x1000);u[1]=(uint32_t)((a+0x1000)>>32)|(16u<<16);u[2]=192;u[3]=0x4dfac;
    g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x100)>>8);g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)((a+0x100)>>40);
    g.regs[mmCB_COLOR_CONTROL]=0xCC0010;g.regs[mmCB_TARGET_MASK]=g.regs[mmCB_SHADER_MASK]=15;
    g.regs[mmCB_COLOR0_BASE]=(uint32_t)(color>>8);g.regs[mmCB_COLOR0_INFO]=10u<<2;
    g.regs[mmCB_COLOR0_ATTRIB2]=((width-1)<<14)|(height-1);g.regs[mmSPI_SHADER_COL_FORMAT]=9;
    g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_TRILIST;
    g.regs[mmDB_Z_READ_BASE]=g.regs[mmDB_Z_WRITE_BASE]=(uint32_t)(depth>>8);
    g.regs[mmDB_Z_INFO]=3;g.regs[mmDB_DEPTH_SIZE_XY]=((height-1)<<16)|(width-1);
    g.regs[mmDB_DEPTH_CONTROL]=2|(3u<<4);g.regs[mmPA_SU_SC_MODE_CNTL]=1u<<11;
    float amount=-19.2f,offset=-9.f;memcpy(&g.regs[0xA2E0],&amount,4);memcpy(&g.regs[0xA2E1],&offset,4);
    g.regs[mmPA_CL_VTE_CNTL]=63;float viewport[]={width/2.f,width/2.f,height/2.f,height/2.f,1.f,0.f};
    memcpy(g.regs+mmPA_CL_VPORT_XSCALE,viewport,sizeof(viewport));
    vk_buffer raw;CHECK(vk_buffer_create(r,(uint64_t)width*height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&raw));
    bool correct=true;
    for(unsigned control=0;control<3;control++) {
        VkCommandBuffer cmd=vk_begin(r);vk_image *c=vk_image_get(r,color,width,height,VK_FORMAT_R8G8B8A8_UNORM);
        vk_image *z=vk_image_get(r,depth,width,height,VK_FORMAT_D32_SFLOAT);CHECK(c && z);
        vk_barrier(cmd,c,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue green={{0,1,0,1}};VkImageSubresourceRange cr={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(cmd,c->image,c->layout,&green,1,&cr);c->gpu_written=true;c->written_seq=++r->write_seq;
        vk_barrier(cmd,z,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearDepthStencilValue foreground={control==2?1.f:.5f,0};VkImageSubresourceRange zr={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
        vkCmdClearDepthStencilImage(cmd,z->image,z->layout,&foreground,1,&zr);z->gpu_written=true;z->written_seq=++r->write_seq;
        g.regs[mmDB_DEPTH_CONTROL]=2|((control==2?1u:control?7u:3u)<<4);
        gpu_draw draw={.count=192,.instances=1};renderer_draw(r,&g,&draw);renderer_wait_idle(r);
        cmd=vk_begin(r);vk_barrier(cmd,c,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=(VkExtent3D){width,height,1};
        vkCmdCopyImageToBuffer(cmd,c->image,c->layout,raw.buffer,1,&copy);renderer_wait_idle(r);
        unsigned painted=0;for(unsigned i=0;i<width*height;i++)painted+=((uint32_t*)raw.mapped)[i]==0xff0000ffu;
        fprintf(stderr,"constant-depth bias %s: %u red foreground pixels\n",control==2?"LESS at 1":control?"ALWAYS control":"LEQUAL",painted);
        correct&=control?painted>0:painted==0;
    }
    vk_buffer_destroy(r,&raw);renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&m);pal_shutdown();CHECK(correct);
}

/* Queue several reads before a fence, then overwrite CPU bytes in a new
 * guest submission. Old draws must keep their versions, while the next draw
 * sees the change. Also exercise aligned/unaligned contained descriptors. */
static void test_gpu_readonly_buffer_versions(void) {
    if(!getenv("PS5C_TEST_GPU"))return;
    static const uint32_t vs[]={0x36000a81,0x2c020a81,0x7e000d00,0x7e020d01,
        0x100000ff,0x42800000,0x100202ff,0x42800000,0x060000ff,0xc1000000,
        0x060202ff,0xc1000000,0x7e040280,0x7e0602f2,0xf80008cf,0x03020100,0xbf810000};
    static const uint32_t ps[]={0xf4200400,0xfa000000,0xbf8cc07f,
        0x7e000210,0x7e020280,0x7e040280,0x7e0602f2,0xf800180f,0x03020100,0xbf810000};
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,8u<<20,GPROT_RW,0,"readonly buffer versions",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,vs,sizeof(vs))&&gmem_write(&m,a+0x100,ps,sizeof(ps)));
    const uint32_t parent_bytes=1u<<20; /* larger than the former 512 KiB cap */
    guest_addr input=a+0x10000;float *values=gmem_host(&m,input,parent_bytes);CHECK(values);
    for(unsigned i=0;i<parent_bytes/4;i++)values[i]=i<1024?.25f:.75f;
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 buffer versions",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(a>>8);g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(a>>40);
    g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x100)>>8);g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)(a>>40);
    g.regs[mmSPI_SHADER_PGM_RSRC2_PS]=4u<<1;
    g.regs[mmCB_COLOR_CONTROL]=0xCC0010;g.regs[mmCB_TARGET_MASK]=g.regs[mmCB_SHADER_MASK]=15;
    g.regs[mmCB_COLOR0_INFO]=10u<<2;g.regs[mmCB_COLOR0_ATTRIB2]=(15u<<14)|15;
    g.regs[mmSPI_SHADER_COL_FORMAT]=9;g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_TRILIST;
    uint32_t *u=g.regs+mmSPI_SHADER_USER_DATA_PS_0;
    gpu_draw draw={.count=3,.instances=1};bool correct=true;
    const unsigned starts[]={0,0,4096,0,4,0};
    const uint32_t expected[]={64,64,191,255,64,32};
    for(unsigned n=0;n<6;n++) {
        if(n==3) {values[0]=1.f;renderer_submit_begin(r);}
        if(n==5) {renderer_wait_idle(r);values[0]=.125f;renderer_submit_begin(r);}
        guest_addr base=input+starts[n];u[0]=(uint32_t)base;u[1]=(uint32_t)(base>>32);
        u[2]=n==0 || n==1 || n==3 || n==5?parent_bytes:65536;u[3]=0x16204;
        g.regs[mmCB_COLOR0_BASE]=(uint32_t)((a+0x4000+n*0x1000)>>8);
        renderer_draw(r,&g,&draw);
    }
    renderer_wait_idle(r);
    uint64_t bytes,uses;vk_readonly_cache_counts(r,&bytes,&uses);
    const char *setting=getenv("PS5C_READONLY_CACHE");bool enabled=setting&&atoi(setting)!=0;
    CHECK(enabled ? bytes>0&&bytes<=128u*1024u*1024u&&uses>=5 : bytes==0&&uses==0);
    vk_buffer raw;CHECK(vk_buffer_create(r,1024,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&raw));
    for(unsigned n=0;n<6;n++) {
        vk_image *img=vk_image_find(r,a+0x4000+n*0x1000,16,16,VK_FORMAT_R8G8B8A8_UNORM);CHECK(img);
        VkCommandBuffer cmd=vk_begin(r);vk_barrier(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=(VkExtent3D){16,16,1};
        vkCmdCopyImageToBuffer(cmd,img->image,img->layout,raw.buffer,1,&copy);renderer_wait_idle(r);
        unsigned bad=0;for(unsigned i=0;i<256;i++)bad+=((uint32_t*)raw.mapped)[i]!=(0xff000000u|expected[n]);
        fprintf(stderr,"readonly version %u: first %08x expected %08x, %u bad pixels (cache %llu bytes, %llu uses)\n",
                n,((uint32_t*)raw.mapped)[0],0xff000000u|expected[n],bad,(unsigned long long)bytes,(unsigned long long)uses);
        correct&=bad==0;
    }
    /* A contained snapshot in a later batch must retain the zero guard
     * bytes of an independent arena copy. Reading one dword just beyond
     * the descriptor must not expose unrelated bytes of the parent. */
    guest_addr guard_input=input+(2u<<20);float *guard_values=gmem_host(&m,guard_input,parent_bytes);CHECK(guard_values);
    for(unsigned i=0;i<parent_bytes/4;i++)guard_values[i]=.75f;
    u[0]=(uint32_t)guard_input;u[1]=(uint32_t)(guard_input>>32);u[2]=parent_bytes;
    renderer_submit_begin(r);g.regs[mmCB_COLOR0_BASE]=(uint32_t)((a+0x4000)>>8);
    renderer_draw(r,&g,&draw);renderer_wait_idle(r);
    uint32_t guard_ps[PS5C_ARRAY_COUNT(ps)];memcpy(guard_ps,ps,sizeof(ps));guard_ps[1]|=65536;
    CHECK(gmem_write(&m,a+0x200,guard_ps,sizeof(guard_ps)));
    guard_input+=4096;u[0]=(uint32_t)guard_input;u[1]=(uint32_t)(guard_input>>32);u[2]=65536;
    g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x200)>>8);renderer_submit_begin(r);
    renderer_draw(r,&g,&draw);renderer_wait_idle(r);
    vk_image *guard_image=vk_image_find(r,a+0x4000,16,16,VK_FORMAT_R8G8B8A8_UNORM);CHECK(guard_image);
    VkCommandBuffer guard_cmd=vk_begin(r);vk_barrier(guard_cmd,guard_image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy guard_copy={0};guard_copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};guard_copy.imageExtent=(VkExtent3D){16,16,1};
    vkCmdCopyImageToBuffer(guard_cmd,guard_image->image,guard_image->layout,raw.buffer,1,&guard_copy);renderer_wait_idle(r);
    unsigned guard_bad=0;for(unsigned i=0;i<256;i++)guard_bad+=((uint32_t*)raw.mapped)[i]!=0xff000000u;
    fprintf(stderr,"readonly subrange zero guard: first %08x, %u incorrect pixels\n",((uint32_t*)raw.mapped)[0],guard_bad);correct&=guard_bad==0;
    g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x100)>>8);
    /* More distinct 4 MiB windows than the 128 MiB quota can retain.
     * Completed allocations must be safely recycled; in-use versions above
     * were checked separately before their targets could be overwritten. */
    for(unsigned n=0;n<40;n++) {
        guest_addr base=input+n*256;float value=n%3==0?.25f:n%3==1?.75f:1.f;
        CHECK(gmem_write(&m,base,&value,sizeof(value)));renderer_submit_begin(r);
        u[0]=(uint32_t)base;u[1]=(uint32_t)(base>>32);u[2]=4u<<20;
        g.regs[mmCB_COLOR0_BASE]=(uint32_t)((a+0x4000)>>8);renderer_draw(r,&g,&draw);renderer_wait_idle(r);
        vk_image *img=vk_image_find(r,a+0x4000,16,16,VK_FORMAT_R8G8B8A8_UNORM);CHECK(img);
        VkCommandBuffer cmd=vk_begin(r);vk_barrier(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=(VkExtent3D){16,16,1};
        vkCmdCopyImageToBuffer(cmd,img->image,img->layout,raw.buffer,1,&copy);renderer_wait_idle(r);
        uint32_t expected_pixel=0xff000000u|(n%3==0?64:n%3==1?191:255);
        for(unsigned i=0;i<256;i++)correct&=((uint32_t*)raw.mapped)[i]==expected_pixel;
    }
    vk_readonly_cache_counts(r,&bytes,&uses);
    CHECK(enabled ? bytes>120u*1024u*1024u&&bytes<=128u*1024u*1024u&&uses>=45 : bytes==0&&uses==0);
    fprintf(stderr,"readonly pressure: 40 windows, %llu retained bytes, %llu uses, pixels %s\n",
            (unsigned long long)bytes,(unsigned long long)uses,correct?"correct":"WRONG");
    vk_buffer_destroy(r,&raw);renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);
    pal_window_destroy(window);gmem_free(&m);pal_shutdown();CHECK(correct);
}

static void test_gpu_validation_log_control(void) {
        /* Manual negative control for the validation collector, outside the
         * normal suite: an unused sampler deliberately violates 01072.
         * A later clean instance must not erase this earlier report. */
        CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
        CHECK(gmem_init(&m,&cfg)==PS5C_OK);pal_window *window=NULL;
        CHECK(pal_window_open("ZeraPS5 validation log control",320,180,&window)==PS5C_OK);
        renderer *r=renderer_create(window,&m);CHECK(r);
        VkSamplerCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter=VK_FILTER_NEAREST;ci.minFilter=VK_FILTER_LINEAR;ci.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU=ci.addressModeV=ci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.unnormalizedCoordinates=VK_TRUE;VkSampler sampler=VK_NULL_HANDLE;
        VkResult result=vkCreateSampler(r->device,&ci,NULL,&sampler);
        if(result==VK_SUCCESS)vkDestroySampler(r->device,sampler,NULL);
        renderer_destroy(r);pal_window_destroy(window);gmem_free(&m);pal_shutdown();
}

/* Hand-encoded compute program, independent of any game. With workgroup
 * barriers, a forward EXECZ skip is uniform: inactive lanes reach the store
 * with stale VGPRs and VALID coordinates. Only lanes 0..15 may write. */
static void test_gpu_image_store_exec(void) {
    if(!getenv("PS5C_TEST_GPU"))return;
    CHECK(pal_init()==PS5C_OK);
    gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"image store EXEC",&a)==PS5C_OK);
    pal_window *window=NULL;
    CHECK(pal_window_open("ZeraPS5 image store EXEC",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);
    gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_PGM_RSRC2]=(8u<<1)|(1u<<15);
    g.regs[mmCOMPUTE_NUM_THREAD_X]=64;
    g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    bool correct=true;
    const VkFormat formats[]={VK_FORMAT_R16_SFLOAT,VK_FORMAT_R32_UINT};
    for(unsigned k=0;k<PS5C_ARRAY_COUNT(formats);k++) {
        uint32_t code[]={
            0x7e040300,                 /* v_mov_b32 v2, v0 (valid x for EVERY lane) */
            0x7e060280,                 /* v_mov_b32 v3, 0 (y) */
            0x7e0802ff,0xfffffff1,      /* v_mov_b32 v4, poison (NaN as float) */
            0x34020082,                 /* v_lshlrev_b32 v1, 2, v0 */
            0xd8340000,0x00000401,      /* ds_write_b32 v1, v4 */
            0xbf8a0000,                 /* s_barrier */
            0xb06a000f,                 /* s_movk_i32 vcc_lo, 15 */
            0x7dac006a,                 /* v_cmpx_ge_u32 vcc_lo, v0 */
            0xbf880004,                 /* s_cbranch_execz end */
            0x7e0802ff,k?37u:0x3e800000u, /* active lanes replace poison */
            0xf0200108,0x00000402,      /* image_store v4, v[2:3], s[0:7] */
            0xbf810000
        };
        guest_addr pc=a+0x100*k,color=a+0x4000+0x1000*k;
        CHECK(gmem_write(&m,pc,code,sizeof(code)));
        uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;memset(u,0,8*4);
        u[0]=(uint32_t)(color>>8);
        u[1]=(uint32_t)(color>>40)|((k?20u:13u)<<20)|(3u<<30);
        u[2]=(63u>>2)|(1u<<14);u[3]=(9u<<28)|0xfacu; /* 64 x 2 */
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);
        g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
        VkCommandBuffer cmd=vk_begin(r);
        vk_image *im=vk_image_get(r,color,64,2,formats[k]);CHECK(im);
        vk_barrier(cmd,im,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue clear={0};if(k)clear.uint32[0]=19;else clear.float32[0]=.5f;
        VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(cmd,im->image,im->layout,&clear,1,&range);
        im->gpu_written=true;im->written_seq=++r->write_seq;
        renderer_dispatch(r,&g,1,1,1);renderer_wait_idle(r);
        vk_buffer raw;CHECK(vk_buffer_create(r,128*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&raw));
        cmd=vk_begin(r);vk_barrier(cmd,im,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy cp={0};cp.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};cp.imageExtent=(VkExtent3D){64,2,1};
        vkCmdCopyImageToBuffer(cmd,im->image,im->layout,raw.buffer,1,&cp);renderer_wait_idle(r);
        unsigned bad=0;
        for(unsigned i=0;i<128;i++) {
            uint32_t expected=k?(i<16?37u:19u):(i<16?0x3400u:0x3800u);
            uint32_t actual=k?((uint32_t *)raw.mapped)[i]:((uint16_t *)raw.mapped)[i];
            bad+=actual!=expected;
        }
        fprintf(stderr,"image store EXEC fmt %d: %u incorrect texels\n",formats[k],bad);
        correct&=!bad;vk_buffer_destroy(r,&raw);g.stats.submits++;
    }
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);
    pal_window_destroy(window);gmem_free(&m);pal_shutdown();CHECK(correct);
}

/* Pin the established RGBA byte-copy order and the narrow movie-plane
 * routing independently: read native image bytes, without an image_load
 * that could cancel an erroneous store swizzle. No game shaders/assets. */
static void test_gpu_image_store_channels(void) {
    if(!getenv("PS5C_TEST_GPU"))return;
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,16*GUEST_PAGE_SIZE,GPROT_RW,0,"image store channel order",&a)==PS5C_OK);
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 image store channel order",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_PGM_RSRC2]=8u<<1;
    g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    struct channel_case {uint32_t format,selectors,dmask,components;bool integer;uint8_t expected[4];};
    const struct channel_case cases[]={
        {56,0xf2e,15,4,false,{32,64,128,191}}, /* RGBA bytes, BGR selector */
        {60,0xf2e,15,4,true, {17,34,51,68}},
        {1, 0x800,15,1,false,{191}},          /* R8 plane supplied in w */
        {5, 0x800,15,1,true, {68}},
        {14,0x22c,15,2,false,{32,64}},        /* ordinary UV plane */
        {18,0x22c,15,2,true, {17,34}},
        {5, 0x800,3, 1,true, {0}},           /* w missing from DMASK */
        {60,0xfac,1, 4,true, {17,0,0,0}},    /* absent alpha is zero too */
    };
    vk_buffer raw;CHECK(vk_buffer_create(r,16,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&raw));bool correct=true;
    for(unsigned n=0;n<PS5C_ARRAY_COUNT(cases);n++) {
        const struct channel_case *c=&cases[n];
        uint32_t code[]={0x7e000280,0x7e020280,
            0x7e0802ff,c->integer?17u:0x3e000000u,0x7e0a02ff,c->integer?34u:0x3e800000u,
            0x7e0c02ff,c->integer?51u:0x3f000000u,0x7e0e02ff,c->integer?68u:0x3f400000u,
            0xf0200008u|(c->dmask<<8),0x00000400,0xbf810000};
        guest_addr pc=a+n*0x100,color=a+0x4000+n*0x1000;
        CHECK(gmem_write(&m,pc,code,sizeof(code)));
        uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;memset(u,0,8*4);
        u[0]=(uint32_t)(color>>8);u[1]=(uint32_t)(color>>40)|(c->format<<20)|(3u<<30);
        u[2]=0;u[3]=(9u<<28)|c->selectors; /* 4x1 linear */
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
        renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_wait_idle(r);
        vk_image *im=vk_image_find_any(r,color,4,1);CHECK(im);
        VkCommandBuffer cmd=vk_begin(r);vk_barrier(cmd,im,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy cp={0};cp.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};cp.imageExtent=(VkExtent3D){4,1,1};
        vkCmdCopyImageToBuffer(cmd,im->image,im->layout,raw.buffer,1,&cp);renderer_wait_idle(r);
        unsigned bad=0;for(unsigned k=0;k<c->components;k++) {
            int delta=(int)((uint8_t*)raw.mapped)[k]-(int)c->expected[k];
            /* UNORM conversion permits a one-unit quantization difference. */
            bad+=abs(delta)>(c->integer?0:1);
        }
        fprintf(stderr,"image store channel case %u fmt %u mask %x: %u wrong bytes\n",n,c->format,c->dmask,bad);correct&=!bad;
    }
    vk_buffer_destroy(r,&raw);renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&m);pal_shutdown();CHECK(correct);
}

/* Compare accelerated lookup with the original scan. Candidate lists must
 * not freeze the winner, hide physical aliases, or survive slot reuse. The
 * physical-span cache is primed directly: no guest bytes/GPU are accessed. */
static void test_image_lookup_candidates(void) {
    renderer *r=calloc(1,sizeof(*r));CHECK(r);gmem dummy={0};r->mem=&dummy;
    const guest_addr a=GUEST_PH_BASE+0x10000,b=GUEST_PH_BASE+0x20000;
    r->phys_cached=2;
    r->phys_cache[0].start=a;r->phys_cache[0].end=a+0x1000;r->phys_cache[0].phys=0x18000;
    r->phys_cache[1].start=b;r->phys_cache[1].end=b+0x1000;r->phys_cache[1].phys=0x18000;
    r->image_lookup_generation=1;r->image_count=80;
    for(unsigned i=0;i<r->image_count;i++) {
        vk_image *im=&r->images[i];im->addr=a+0x100*(i%4);im->phys=0x18000+0x100*(i%4);
        im->width=i%3==0?32:16;im->height=16;im->layers=1u<<(i%3);
        im->levels=i%7==0?2:1;im->format=i%2?VK_FORMAT_R32_UINT:VK_FORMAT_R32_SFLOAT;
        im->written_seq=i/3;
    }
    /* >16 aliases exercises the bounded-cache fallback, then smaller sets
     * exercise positive/negative entries and the exact-address tie break. */
    for(unsigned round=0;round<100;round++) {
        if(round==20 || round==60) {r->image_count=round==20?12:10;r->image_lookup_generation++;}
        if(round==40) {
            r->images[1].addr=b;r->images[1].phys=0x18000;
            r->image_lookup_generation++; /* reused slot, unchanged count */
        }
        if(round==70)r->phys_cache[1].phys=0x28000; /* remapped alias: key must change */
        r->images[round%r->image_count].written_seq+=100;
        for(unsigned query=0;query<6;query++)for(unsigned kind=0;kind<3;kind++) {
            guest_addr addr=query==4?b:a+query*0x100;
            uint32_t width=query%2?32:16,layers=query%3+1;
            VkFormat fmt=query%2?VK_FORMAT_R32_UINT:VK_FORMAT_R32_SFLOAT;
            vk_image *expected,*actual;r->cpu_prepare_enabled=false;
            expected=kind==0?vk_image_find_array(r,addr,width,16,fmt):
                     kind==1?vk_image_find_layers(r,addr,width,16,fmt,layers):vk_image_find_any(r,addr,width,16);
            r->cpu_prepare_enabled=true;
            actual=kind==0?vk_image_find_array(r,addr,width,16,fmt):
                   kind==1?vk_image_find_layers(r,addr,width,16,fmt,layers):vk_image_find_any(r,addr,width,16);
            CHECK(actual==expected);
        }
    }
    /* Exact-address ties and changing winners without rebuilding candidates. */
    r->image_count=2;r->image_lookup_generation++;r->phys_cache[1].phys=0x18000;
    r->images[0]=(vk_image){.addr=a,.phys=0x18000,.width=16,.height=16,.layers=1,.levels=1,.format=VK_FORMAT_R32_UINT,.written_seq=10};
    r->images[1]=r->images[0];r->images[1].addr=b;
    CHECK(vk_image_find_layers(r,a,16,16,VK_FORMAT_R32_UINT,1)==&r->images[0]);
    CHECK(vk_image_find_layers(r,b,16,16,VK_FORMAT_R32_UINT,1)==&r->images[1]);
    r->images[1].written_seq++;
    CHECK(vk_image_find_layers(r,a,16,16,VK_FORMAT_R32_UINT,1)==&r->images[1]);
    r->images[1].addr=b+0x400;r->images[1].phys=0x18400;r->image_lookup_generation++;
    CHECK(vk_image_find_layers(r,a,16,16,VK_FORMAT_R32_UINT,1)==&r->images[0]);
    /* Explicitly prime a negative result and reuse a matching slot. */
    CHECK(!vk_image_find_any(r,a+0x800,16,16));
    r->images[0]=(vk_image){.addr=a+0x800,.phys=0x18800,.width=16,.height=16,.layers=1,.levels=1,.format=VK_FORMAT_R32_UINT};
    r->image_lookup_generation++;
    CHECK(vk_image_find_any(r,a+0x800,16,16)==&r->images[0]);
    CHECK(!vk_image_find_any(r,a,16,16));
    r->images[2]=(vk_image){.addr=a,.phys=0x18000,.width=16,.height=16,.layers=1,.levels=1,.format=VK_FORMAT_R32_UINT};
    r->image_count=3;r->image_lookup_generation++;
    CHECK(vk_image_find_any(r,a,16,16)==&r->images[2]);
    CHECK(vk_image_find_any(r,b,16,16)==&r->images[2]);
    r->phys_cache[1].phys=0x28000;
    CHECK(!vk_image_find_any(r,b,16,16));
    free(r);
}

/* Forty distinct immutable 4 MiB snapshots exceed the old arena, but fit
 * a large batch. Verify every draw after the fence, including snapshots of
 * CPU bytes overwritten while earlier commands were still only recorded. */
static void test_draw_key_equivalence(void) {
    test_cached_spirv_locations();
    CHECK(vk_pipeline_test_draw_keys());
}

static void test_gpu_draw_batch_versions(void) {
    if(!getenv("PS5C_TEST_GPU"))return;
    static const uint32_t vs[]={0x36000a81,0x2c020a81,0x7e000d00,0x7e020d01,
        0x100000ff,0x42800000,0x100202ff,0x42800000,0x060000ff,0xc1000000,
        0x060202ff,0xc1000000,0x7e040280,0x7e0602f2,0xf80008cf,0x03020100,0xbf810000};
    static const uint32_t ps[]={0xf4200400,0xfa000000,0xbf8cc07f,
        0x7e000210,0x7e020280,0x7e040280,0x7e0602f2,0xf800180f,0x03020100,0xbf810000};
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,8u<<20,GPROT_RW,0,"draw batch versions",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,vs,sizeof(vs))&&gmem_write(&m,a+0x100,ps,sizeof(ps)));
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 draw batch versions",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(a>>8);g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(a>>40);
    g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x100)>>8);g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)(a>>40);
    g.regs[mmSPI_SHADER_PGM_RSRC2_PS]=4u<<1;
    g.regs[mmCB_COLOR_CONTROL]=0xCC0010;g.regs[mmCB_TARGET_MASK]=g.regs[mmCB_SHADER_MASK]=15;
    g.regs[mmCB_COLOR0_INFO]=10u<<2;g.regs[mmCB_COLOR0_ATTRIB2]=(15u<<14)|15;
    g.regs[mmSPI_SHADER_COL_FORMAT]=9;g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_TRILIST;
    uint32_t *u=g.regs+mmSPI_SHADER_USER_DATA_PS_0;guest_addr input=a+(3u<<20);
    u[0]=(uint32_t)input;u[1]=(uint32_t)(input>>32);u[2]=4u<<20;u[3]=0x16204;
    const char *setting=getenv("PS5C_LARGE_DRAW_BATCH");bool file=setting&&setting[0]=='@';
    const char *overlap=getenv("PS5C_WORK_OVERLAP");bool overlap_test=overlap && overlap[0]=='@';
    const char *scratch=getenv("PS5C_REUSE_DRAW_STAGE");bool scratch_file=scratch && scratch[0]=='@';
    unsigned rounds=(file || overlap_test || scratch_file)?4:1;bool correct=true;
    vk_buffer raw;CHECK(vk_buffer_create(r,1024,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&raw));
    for(unsigned round=0;round<rounds;round++) {
        bool wide=file?(round&1):setting&&!strcmp(setting,"1");
        if(file) {FILE *f=fopen(setting+1,"w");CHECK(f);fprintf(f,"%u\n",wide);fclose(f);}
        if(overlap_test) {FILE *f=fopen(overlap+1,"w");CHECK(f);fprintf(f,"%u\n",round&1);fclose(f);}
        if(scratch_file) {FILE *f=fopen(scratch+1,"w");CHECK(f);fprintf(f,"%u\n",round&1);fclose(f);}
        g.stats.flips++; /* file-controlled CPU preparation is selected per flip */
        uint64_t before=r->use_seq;
        for(unsigned n=0;n<40;n++) {
            float value=(n+round)%2?.75f:.25f;CHECK(gmem_write(&m,input,&value,4));
            renderer_submit_begin(r);
            guest_addr target=overlap_test?a+0x2c000:a+0x4000+n*0x1000;
            g.regs[mmCB_COLOR0_BASE]=(uint32_t)(target>>8);
            uint32_t slot=r->work_index;
            gpu_draw draw={.count=3,.instances=1};renderer_draw(r,&g,&draw);
            if(overlap_test) {
                correct&=r->work_index==(slot^(round&1));
                /* Reuse one render target across submissions, preserving
                 * its changing versions in distinct GPU images. Barriers
                 * must cover earlier slots on the same queue too. */
                vk_work_end(r);
                vk_image *src=vk_image_find(r,target,16,16,VK_FORMAT_R8G8B8A8_UNORM);CHECK(src);
                vk_image *dst=vk_image_get(r,a+0x4000+n*0x1000,16,16,VK_FORMAT_R8G8B8A8_UNORM);CHECK(dst);
                vk_barrier(r->cmd,src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
                vk_barrier(r->cmd,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkImageCopy cp={0};cp.srcSubresource=cp.dstSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
                cp.extent=(VkExtent3D){16,16,1};
                vkCmdCopyImage(r->cmd,src->image,src->layout,dst->image,dst->layout,1,&cp);
                vk_flush(r); /* no wait: the next draw must retain this snapshot */
            }
        }
        renderer_wait_idle(r);uint64_t batches=r->use_seq-before;
        fprintf(stderr,"draw batch versions round %u mode %u: %llu batches\n",round,wide,(unsigned long long)batches);
        correct&=overlap_test?batches==40:wide?batches==1:batches>=2;
        for(unsigned n=0;n<40;n++) {
            vk_image *im=vk_image_find(r,a+0x4000+n*0x1000,16,16,VK_FORMAT_R8G8B8A8_UNORM);CHECK(im);
            VkCommandBuffer cmd=vk_begin(r);vk_barrier(cmd,im,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy cp={0};cp.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};cp.imageExtent=(VkExtent3D){16,16,1};
            vkCmdCopyImageToBuffer(cmd,im->image,im->layout,raw.buffer,1,&cp);renderer_wait_idle(r);
            uint32_t expected=0xff000000u|((n+round)%2?191u:64u);
            for(unsigned i=0;i<256;i++)correct&=((uint32_t *)raw.mapped)[i]==expected;
        }
    }
    vk_buffer_destroy(r,&raw);renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);
    pal_window_destroy(window);gmem_free(&m);pal_shutdown();CHECK(correct);
}

int main(int argc, char **argv) {
    if(argc==2 && !strcmp(argv[1],"--constant-blend")) {
        const test_case cases[]={TEST_CASE(test_gpu_constant_blend)};
        return test_run_all("constant_blend",cases,PS5C_ARRAY_COUNT(cases));
    }
    if (argc == 2 && !strcmp(argv[1], "--residency")) {
        const test_case residency[] = {TEST_CASE(test_gpu_residency)};
        return test_run_all("image_residency", residency, 1);
    }
    if(argc==2 && !strcmp(argv[1],"--many-spills")) {
        const test_case cases[]={TEST_CASE(test_gpu_fragment_many_spills)};
        return test_run_all("fragment_spills",cases,PS5C_ARRAY_COUNT(cases));
    }
    if(argc==2 && !strcmp(argv[1],"--video-views")) {
        const test_case cases[]={TEST_CASE(test_uv_number_interpretation),TEST_CASE(test_gpu_uv_integer_to_normalized)};
        return test_run_all("video_views",cases,PS5C_ARRAY_COUNT(cases));
    }
    if(argc==2 && !strcmp(argv[1],"--draw-prep")) {
        const test_case cases[]={TEST_CASE(test_draw_key_equivalence),TEST_CASE(test_gpu_draw_batch_versions)};
        return test_run_all("draw_preparation",cases,PS5C_ARRAY_COUNT(cases));
    }
    if(argc==2 && !strcmp(argv[1],"--work-overlap")) {
        const test_case cases[]={TEST_CASE(test_gpu_draw_batch_versions),TEST_CASE(test_gpu_scalar_snapshot_writeback)};
        return test_run_all("work_overlap",cases,PS5C_ARRAY_COUNT(cases));
    }
    if(argc==2 && !strcmp(argv[1],"--compute-capture")) {
        const test_case cases[]={TEST_CASE(test_gpu_view_usage)};
        return test_run_all("compute_capture",cases,PS5C_ARRAY_COUNT(cases));
    }
    if(argc==2 && !strcmp(argv[1],"--draw-batches")) {
        const test_case cases[]={TEST_CASE(test_gpu_draw_batch_versions)};
        return test_run_all("draw_batch_versions",cases,PS5C_ARRAY_COUNT(cases));
    }
    if(argc==2 && !strcmp(argv[1],"--image-store-channels")) {
        const test_case cases[]={TEST_CASE(test_gpu_image_store_channels)};
        return test_run_all("image_store_channels",cases,PS5C_ARRAY_COUNT(cases));
    }
    if(argc==2 && !strcmp(argv[1],"--image-store-exec")) {
        const test_case cases[]={TEST_CASE(test_gpu_image_store_exec)};
        return test_run_all("image_store_exec",cases,PS5C_ARRAY_COUNT(cases));
    }
    if(argc==2 && !strcmp(argv[1],"--readonly-buffers")) {
        const test_case cases[]={TEST_CASE(test_gpu_readonly_buffer_versions)};
        return test_run_all("readonly_buffer_versions",cases,PS5C_ARRAY_COUNT(cases));
    }
    if(argc==2 && !strcmp(argv[1],"--constant-depth")) {
        const test_case cases[]={TEST_CASE(test_constant_depth_bias_policy),TEST_CASE(test_gpu_constant_depth_bias)};
        return test_run_all("constant_depth_bias",cases,PS5C_ARRAY_COUNT(cases));
    }
    if(argc==2 && !strcmp(argv[1],"--deferred-capture")) {
        const test_case cases[]={TEST_CASE(test_gpu_deferred_snapshots)};
        return test_run_all("deferred_snapshots",cases,PS5C_ARRAY_COUNT(cases));
    }
    if(argc==2 && !strcmp(argv[1],"--image-ownership")) {
        const test_case cases[]={TEST_CASE(test_gpu_small_texture_write_order),TEST_CASE(test_gpu_large_texture_write_order)};
        return test_run_all("small_image_write_order",cases,PS5C_ARRAY_COUNT(cases));
    }
    if(argc==2 && !strcmp(argv[1],"--history-export")) {
        const test_case cases[]={TEST_CASE(test_gpu_temporal_history_export)};
        return test_run_all("temporal_history_export",cases,PS5C_ARRAY_COUNT(cases));
    }
    if(argc==2 && !strcmp(argv[1],"--scalar-writeback")) {
        const test_case cases[]={TEST_CASE(test_gpu_scalar_snapshot_writeback),TEST_CASE(test_gpu_writeback_initial_version)};
        return test_run_all("scalar_snapshot_writeback",cases,PS5C_ARRAY_COUNT(cases));
    }
    if(argc==2 && !strcmp(argv[1],"--validation-log-control")) {
        const test_case cases[]={TEST_CASE(test_gpu_validation_log_control),TEST_CASE(test_gpu_fp32_instruction_rounding)};
        return test_run_all("validation_log_control",cases,PS5C_ARRAY_COUNT(cases));
    }
    if(argc==2 && !strcmp(argv[1],"--alpha-coverage")) {
        const test_case cases[]={TEST_CASE(test_gpu_alpha_coverage_override)};
        return test_run_all("alpha_coverage",cases,PS5C_ARRAY_COUNT(cases));
    }
    if(argc==2 && !strcmp(argv[1],"--fp32-rounding")) {
        const test_case cases[]={TEST_CASE(test_gpu_fp32_instruction_rounding)};
        return test_run_all("fp32_rounding",cases,PS5C_ARRAY_COUNT(cases));
    }
    if(argc==2 && !strcmp(argv[1],"--pattern-clear")) {
        const test_case cases[]={TEST_CASE(test_gpu_buffer_pattern_clear)};
        return test_run_all("buffer_pattern_clear",cases,PS5C_ARRAY_COUNT(cases));
    }
    if(argc==2 && !strcmp(argv[1],"--hdr-swap")) {
        const test_case cases[]={TEST_CASE(test_gpu_hdr_channel_order)};
        return test_run_all("hdr_channel_order",cases,PS5C_ARRAY_COUNT(cases));
    }
    if(argc==2 && !strcmp(argv[1],"--scalar-flow")) {
        const test_case cases[]={TEST_CASE(test_gpu_fragment_scalar_flow)};
        return test_run_all("fragment_scalar_flow",cases,PS5C_ARRAY_COUNT(cases));
    }
    if(argc==2 && !strcmp(argv[1],"--export-mask")) {
        const test_case mask[]={TEST_CASE(test_gpu_export_valid_mask)};
        return test_run_all("export_valid_mask",mask,1);
    }
    if(argc==2 && !strcmp(argv[1],"--input-layout")) {
        const test_case layout[]={TEST_CASE(test_gpu_pixel_input_layout)};
        return test_run_all("pixel_input_layout",layout,1);
    }
    if(argc==2 && !strcmp(argv[1],"--depth-sample")) {
        const test_case sample[]={TEST_CASE(test_gpu_depth_sample_attachment)};
        return test_run_all("depth_sample",sample,1);
    }
    if(argc==2 && !strcmp(argv[1],"--opaque-prepass")) {
        const test_case prepass[]={TEST_CASE(test_gpu_opaque_depth_prepass)};
        return test_run_all("opaque_prepass",prepass,1);
    }
    if (argc == 2 && !strcmp(argv[1], "--quad-coverage")) {
        const test_case coverage[] = {TEST_CASE(test_gpu_quad_coverage)};
        return test_run_all("quad_coverage", coverage, 1);
    }
    if (argc == 2 && !strcmp(argv[1], "--depth-feedback")) {
        const test_case feedback[] = {TEST_CASE(test_gpu_depth_feedback)};
        return test_run_all("depth_feedback", feedback, 1);
    }
    const test_case tests[] = {TEST_CASE(test_draw_key_equivalence),TEST_CASE(test_masked_htile_policy),TEST_CASE(test_image_lookup_candidates),TEST_CASE(test_constant_depth_bias_policy),TEST_CASE(test_opaque_depth_policy),TEST_CASE(test_reads_before_image_overwrite),TEST_CASE(test_gta_bgra_bytes_before_swizzle),
                               TEST_CASE(test_rgba_number_interpretation),TEST_CASE(test_uv_number_interpretation),
                               TEST_CASE(test_alias_list_bounds_and_existing_views),TEST_CASE(test_gpu_fragment_many_spills), TEST_CASE(test_gpu_uv_integer_to_normalized),TEST_CASE(test_gpu_image_store_exec),TEST_CASE(test_gpu_image_store_channels),
                               TEST_CASE(test_gpu_residency), TEST_CASE(test_gpu_view_usage),
                                 TEST_CASE(test_gpu_depth_sample_attachment), TEST_CASE(test_gpu_sampler_churn), TEST_CASE(test_gpu_integer_images), TEST_CASE(test_gpu_depth_integer_alias),TEST_CASE(test_gpu_htile_layers),TEST_CASE(test_gpu_masked_htile),TEST_CASE(test_gpu_mip_promotion), TEST_CASE(test_gpu_integer_color_exports), TEST_CASE(test_gpu_mrtz_depth), TEST_CASE(test_gpu_raster_faces), TEST_CASE(test_gpu_depth_feedback), TEST_CASE(test_gpu_quad_coverage), TEST_CASE(test_gpu_opaque_depth_prepass), TEST_CASE(test_gpu_pixel_input_layout), TEST_CASE(test_gpu_export_valid_mask),TEST_CASE(test_gpu_hdr_channel_order),TEST_CASE(test_gpu_buffer_pattern_clear),TEST_CASE(test_gpu_fp32_instruction_rounding),TEST_CASE(test_gpu_alpha_coverage_override),TEST_CASE(test_gpu_scalar_snapshot_writeback),TEST_CASE(test_gpu_writeback_initial_version),TEST_CASE(test_gpu_temporal_history_export),TEST_CASE(test_gpu_small_texture_write_order),TEST_CASE(test_gpu_large_texture_write_order),TEST_CASE(test_gpu_constant_depth_bias)};
    size_t count = sizeof(tests) / sizeof(tests[0]);
    if (!getenv("PS5C_TEST_GPU")) count = 10; /* CPU cases precede optional native GPU fixtures. */
    return test_run_all("image_views", tests, count);
}
