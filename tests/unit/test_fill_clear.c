/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A compute fill of an image's memory as a clear color of the image's format. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/vk/vk_fill_clear.h"
#include "gpu/vk/vk_internal.h"
#include "gpu/gpu_regs.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int gpu_fill_memory(uint32_t buffer_format,uint32_t comps) {
    /* 64 lanes write one vec4 each: index = group*64 + local_id. */
    const uint32_t program[] = {
        0xd7460000,0x04010c0c,
        0x7e080200,0x7e0a0201,0x7e0c0202,0x7e0e0203,
        0xe01c2000,0x80010400,0xbf810000
    };
    const uint32_t inline_zero[] = {
        0xd7460000,0x04010c0c,
        0x7e080280,0x7e0a0280,0x7e0c0280,0x7e0e0280,
        0xe01c2000,0x80010400,0xbf810000
    };
    const uint32_t inline_color[] = {
        0xd7460000,0x04010c0c,
        0x7e0802f2,0x7e0a02f0,0x7e0c02f4,0x7e0e02f3,
        0xe01c2000,0x80010400,0xbf810000
    };
    const uint32_t literal_color[] = {
        0xd7460000,0x04010c0c,
        0x7e0802ff,0x3e800000,0x7e0a02ff,0x3f800000,
        0x7e0c02ff,0x3f000000,0x7e0e02ff,0xbf800000,
        0xe01c2000,0x80010400,0xbf810000
    };
    const uint32_t scalar_program[]={0xd7460000,0x04010c0c,0x7e080200,0xe0102000,0x80010400,0xbf810000};
    const uint32_t scalar_zero[]={0xd7460000,0x04010c0c,0x7e080280,0xe0102000,0x80010400,0xbf810000};
    const uint32_t scalar_one[]={0xd7460000,0x04010c0c,0x7e0802f2,0xe0102000,0x80010400,0xbf810000};
    const uint32_t scalar_literal[]={0xd7460000,0x04010c0c,0x7e0802ff,0x3e800000,0xe0102000,0x80010400,0xbf810000};
    const uint32_t scalar_loaded[]={
        0xd7460000,0x04010c0c,
        0xf4200404,0xfa000000, /* s_buffer_load_dword s16, s[8:11], 0 */
        0xbf8cc07f,0x7e080210, /* wait; v4 = s16 */
        0xe0102000,0x80010400,0xbf810000
    };
    /* The nop excludes the CPU pattern, so this really produces a pending GPU
     * write. A one-record descriptor leaves only lane zero inside the buffer. */
    const uint32_t scalar_producer[]={0xbf800000,0xd7460000,0x04010c0c,0x7e080200,0xe0102000,0x80010400,0xbf810000};
    CHECK(pal_init() == PS5C_OK);
    gmem m; gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 4*GUEST_PAGE_SIZE, GPROT_RW, 0, "fill memory", &a) == PS5C_OK);
    CHECK(gmem_write(&m, a, program, sizeof(program)));
    CHECK(gmem_write(&m, a+0x400, inline_zero, sizeof(inline_zero)));
    CHECK(gmem_write(&m, a+0x800, inline_color, sizeof(inline_color)));
    CHECK(gmem_write(&m, a+0xc00, literal_color, sizeof(literal_color)));
    if(comps==1) {
        CHECK(gmem_write(&m,a,scalar_program,sizeof(scalar_program)));
        CHECK(gmem_write(&m,a+0x400,scalar_zero,sizeof(scalar_zero)));
        CHECK(gmem_write(&m,a+0x800,scalar_one,sizeof(scalar_one)));
        CHECK(gmem_write(&m,a+0xc00,scalar_literal,sizeof(scalar_literal)));
        CHECK(gmem_write(&m,a+0x1000,scalar_loaded,sizeof(scalar_loaded)));
        CHECK(gmem_write(&m,a+0x1400,scalar_producer,sizeof(scalar_producer)));
    }
    guest_addr base = a + 0x4000;
    guest_addr source=a+0x3000;
    uint32_t bytes=64*comps*4;
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 fill coherence regression", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m); CHECK(r);
    gpu g; CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(a >> 8);
    g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)(a >> 40);
    g.regs[mmCOMPUTE_PGM_RSRC2] = (12u << 1) | (1u << 7);
    g.regs[mmCOMPUTE_NUM_THREAD_X] = 64;
    g.regs[mmCOMPUTE_NUM_THREAD_Y] = g.regs[mmCOMPUTE_NUM_THREAD_Z] = 1;
    uint32_t *u = g.regs + mmCOMPUTE_USER_DATA_0;
    u[4] = (uint32_t)base;
    u[5] = (uint32_t)(base >> 32) | ((comps*4) << 16);
    u[6] = 64;
    u[7] = (comps==4?0xfacu:4u) | (buffer_format << 12);
    u[8]=(uint32_t)source;u[9]=(uint32_t)(source>>32);u[10]=4;u[11]=4u|(20u<<12);
    /* The reference GPU path verifies the buffer bytes directly. It does not
     * currently update an already cached image alias of that buffer. The fast
     * path must additionally keep such an image coherent. */
    bool original_gpu = getenv("PS5C_NO_CPU_FILL") != NULL;
    vk_image *image = original_gpu ? NULL : vk_image_get(r, base, 8, 8,
        comps==4?VK_FORMAT_R32G32B32A32_SFLOAT:VK_FORMAT_R32_SFLOAT);
    CHECK(original_gpu || image);
    vk_buffer readback; CHECK(vk_buffer_create(r, 1024, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &readback));
    const uint32_t patterns[][4] = {
        {0,0,0,0},{0x3f800000,0x3f000000,0x3e800000,0x3f800000},
        {0,0,0,0},{0x3f800000,0x3f000000,0x40000000,0xbf800000},
        {0x3e800000,0x3f800000,0x3f000000,0xbf800000},
        {0x40400000,0,0,0},{0x3e000000,0,0,0},{0xbf000000,0,0,0}
    };
    bool correct = true;
    for (unsigned k = 0; k < (comps==4?5:PS5C_ARRAY_COUNT(patterns)); k++) {
        memset(gmem_host(&m, base-16, bytes+32), 0xa5, bytes+32);
        memcpy(u, patterns[k], 16);
        /* Constants must not accidentally come from these unrelated SGPRs. */
        if(k>=2) for(unsigned j=0;j<4;j++) u[j]=0xdeadbeefu;
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+(k>=5?0x1000:k>=2?(k-1)*0x400:0))>>8);
        if(k>=5) {
            uint32_t value=k==7?0xabad1deau:patterns[k][0];
            CHECK(gmem_write(&m,source,&value,4));renderer_mem_written(r,source,4);
        }
        renderer_submit_begin(r);
        if(k==7) {
            u[0]=patterns[k][0];u[4]=(uint32_t)source;u[5]=(uint32_t)(source>>32)|(4u<<16);u[6]=1;
            g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x1400)>>8);
            renderer_dispatch(r,&g,1,1,1);
            u[0]=0xdeadbeefu;u[4]=(uint32_t)base;u[5]=(uint32_t)(base>>32)|(4u<<16);u[6]=64;
            g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x1000)>>8);
        }
        renderer_dispatch(r, &g, 1, 1, 1);
        renderer_wait_idle(r);
        bool matches = true;
        for (unsigned pixel = 0; pixel < 64; pixel++)
            matches &= !memcmp((uint8_t *)gmem_host(&m, base, bytes) + pixel*comps*4, patterns[k], comps*4);
        for(unsigned j=0;j<16;j++)
            matches &= ((uint8_t *)gmem_host(&m,base-16,bytes+32))[j]==0xa5 &&
                       ((uint8_t *)gmem_host(&m,base-16,bytes+32))[bytes+16+j]==0xa5;
        fprintf(stderr, "fill %u components pattern %u: guest buffer %s, image seq %llu\n", comps,k, matches ? "correct" : "STALE",
                (unsigned long long)(image ? image->written_seq : 0));
        correct &= matches;
        if(original_gpu) continue;
        VkCommandBuffer cmd = vk_begin(r);
        vk_barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {0}; copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1; copy.imageExtent = (VkExtent3D){8,8,1};
        vkCmdCopyImageToBuffer(cmd, image->image, image->layout, readback.buffer, 1, &copy);
        renderer_wait_idle(r);
        for (unsigned pixel = 0; pixel < 64; pixel++)
            correct &= !memcmp((uint8_t *)readback.mapped + pixel*comps*4, patterns[k], comps*4);
    }
    vk_buffer_destroy(r, &readback);
    renderer_destroy(r); free(g.regs); pal_mutex_destroy(g.lock);
    pal_window_destroy(window); gmem_free(&m); pal_shutdown();
    CHECK(correct);
    return 0;
}

int main(void) {
    VkClearColorValue c;
    /* Sonic Frontiers' glow target: RGBA16F (0, 0, 0, 1) */
    const uint32_t black_opaque[4] = {0x00000000, 0x3c000000, 0x00000000, 0x3c000000};
    CHECK(vk_fill_clear_value(VK_FORMAT_R16G16B16A16_SFLOAT, black_opaque, &c));
    CHECK(c.float32[0] == 0.0f && c.float32[1] == 0.0f && c.float32[2] == 0.0f && c.float32[3] == 1.0f);
    /* a 16-byte format takes the whole pattern as one texel */
    CHECK(vk_fill_clear_value(VK_FORMAT_R32G32B32A32_SFLOAT, black_opaque, &c));
    CHECK(c.uint32[1] == 0x3c000000u);
    /* not one repeated 4-byte texel */
    CHECK(!vk_fill_clear_value(VK_FORMAT_R8G8B8A8_UNORM, black_opaque, &c));
    /* zeros: any format, even one not decoded */
    const uint32_t zero[4] = {0, 0, 0, 0};
    CHECK(vk_fill_clear_value(VK_FORMAT_B10G11R11_UFLOAT_PACK32, zero, &c));
    CHECK(!c.uint32[0] && !c.uint32[1] && !c.uint32[2] && !c.uint32[3]);
    CHECK(vk_fill_clear_value(VK_FORMAT_D32_SFLOAT, zero, &c));
    /* a nonzero pattern of a format not decoded */
    const uint32_t ones[4] = {0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000};
    CHECK(!vk_fill_clear_value(VK_FORMAT_B10G11R11_UFLOAT_PACK32, ones, &c));
    CHECK(vk_fill_clear_value(VK_FORMAT_R32_SFLOAT, ones, &c) && c.float32[0] == 1.0f);
    /* BGRA bytes in memory */
    const uint32_t bgra[4] = {0xff0080ff, 0xff0080ff, 0xff0080ff, 0xff0080ff};
    CHECK(vk_fill_clear_value(VK_FORMAT_B8G8R8A8_UNORM, bgra, &c));
    CHECK(c.float32[0] == 0.0f && c.float32[2] == 1.0f && c.float32[3] == 1.0f && c.float32[1] > 0.5f && c.float32[1] < 0.51f);
    /* half floats: -2, 0.5, subnormal */
    const uint32_t halves[4] = {0x3800c000, 0x00010000, 0x3800c000, 0x00010000};
    CHECK(vk_fill_clear_value(VK_FORMAT_R16G16B16A16_SFLOAT, halves, &c));
    CHECK(c.float32[0] == -2.0f && c.float32[1] == 0.5f && c.float32[2] == 0.0f && c.float32[3] > 0.0f);
    printf("fill clear: ok\n");
    if(getenv("PS5C_TEST_GPU")) {
        CHECK(gpu_fill_memory(77,4)==0); /* FLOAT */
        CHECK(gpu_fill_memory(75,4)==0); /* UINT, including literal-zero clears */
        CHECK(gpu_fill_memory(22,1)==0); /* scalar FLOAT and loaded values */
        CHECK(gpu_fill_memory(20,1)==0); /* scalar UINT and pending producer */
    }
    return 0;
}
