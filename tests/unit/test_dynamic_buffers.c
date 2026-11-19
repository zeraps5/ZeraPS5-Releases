/* SPDX-License-Identifier: GPL-2.0-or-later */
/* One shader chooses input/output descriptors and scalar pointers at execution.
 * GPU mode imports actual guest pages; no copied stand-in resources. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/shader/shader.h"
#include "gpu/vk/vk_internal.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static const uint32_t code[] = {
    0x7e020210, 0x34040284, 0x7e120502, /* s9 = workgroup X * 16 */
    0xf4280100, 0x12000000, /* input descriptor: s_buffer_load_dwordx4 s4,s0,s9 */
    0xe0302000, 0x80010200, /* buffer_load_dword v2,v0,s4,0 idxen */
    0xf4040286, 0x12000000, /* s_load_dwordx2 s10,s12,s9 */
    0xf4000385, 0xfa000000, /* s_load_dword s14,s10,0 */
    0x4a04040e,             /* v_add_nc_u32 v2,s14,v2 */
    0xf428050c, 0x12000000, /* output descriptor: s_buffer_load_dwordx4 s20,s24,s9 */
    0xe0702000, 0x80050200, /* buffer_store_dword v2,v0,s20,0 idxen */
    0xbf810000,
};

static void descriptor(uint32_t *d, guest_addr base, uint32_t stride, uint32_t count) {
    d[0] = (uint32_t)base;
    d[1] = (uint32_t)(base >> 32) | (stride << 16);
    d[2] = count; d[3] = 0x00016204;
}

int main(void) {
    CHECK(pal_init() == PS5C_OK);
    gmem mem;
    gmem_config cfg = {.start = 0x200000000ull, .size = 16 * GUEST_PAGE_SIZE, .identity = false};
    CHECK(gmem_init(&mem, &cfg) == PS5C_OK);
    guest_addr base;
    CHECK(gmem_map(&mem, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "dynamic buffers", &base) == PS5C_OK);
    uint32_t *data = gmem_host(&mem, base, GUEST_PAGE_SIZE);
    CHECK(data);
    for (uint32_t i = 0; i < GUEST_PAGE_SIZE / 4; i++) data[i] = 0xc0ffee00u ^ i;
    const uint32_t input_at[4] = {0x1000, 0x2000, 0x1000, (uint32_t)GUEST_PAGE_SIZE - 4};
    const uint32_t input_stride[4] = {8, 12, 4, 4};
    const uint32_t output_at[4] = {0x3000, 0x3400, 0x3800, 0x3c00};
    const uint32_t output_stride[4] = {4, 8, 4, 8};
    for (uint32_t group = 0; group < 4; group++) {
        descriptor(data + 0x100 / 4 + group * 4, base + input_at[group] + (group == 2 ? 1ull << 32 : 0), input_stride[group], 64);
        descriptor(data + 0x200 / 4 + group * 4, base + output_at[group], output_stride[group], 64);
        guest_addr pointer = base + 0x1c00 + 4 * group + (group == 3 ? 1ull << 32 : 0);
        memcpy(data + 0x400 / 4 + group * 4, &pointer, sizeof(pointer));
    }
    sh_program *p = calloc(1, sizeof(*p));
    CHECK(p && sh_analyze(code, PS5C_ARRAY_COUNT(code), base, p));
    CHECK(p->site_count == 6);
    sh_eval e = {0};
    descriptor(e.sgpr_init, base + 0x100, 16, 4);
    descriptor(e.sgpr_init + 24, base + 0x200, 16, 4);
    e.sgpr_init[12] = (uint32_t)(base + 0x400);
    e.sgpr_init[13] = (uint32_t)((base + 0x400) >> 32);
    e.sgpr_init[126] = e.sgpr_init[127] = 0xffffffff;
    e.group_sgpr = 16, e.group_sgprs = 1; /* the workgroup id, as the renderer sets it */
    CHECK(sh_evaluate(p, &mem, &e));
    CHECK(e.site[1].desc_unknown && e.site[5].desc_unknown);
    sh_site_shape shapes[6];
    for (uint32_t s = 0; s < 6; s++) {
        sh_site_shape_of(p, &e, s, shapes + s);
        shapes[s].address_map = 1;
    }
    CHECK(sh_binding_count(p) == 5 && sh_binding_count_shapes(p, shapes) == 7);
    uint32_t *uniforms = realloc(e.smem_data, (e.smem_words + 12) * sizeof(uint32_t));
    CHECK(uniforms);
    e.smem_data = uniforms;
    for (uint32_t s = 0; s < 6; s++) {
        uniforms[SH_U_SITE + s] = e.smem_words;
        uniforms[e.smem_words++] = (uint32_t)base;
        uniforms[e.smem_words++] = (uint32_t)(base >> 32);
    }
    sh_stage_info info = {.stage = SH_STAGE_CS, .local_size = {64, 1, 1},
        .cs_tgid_sgpr = 16, .cs_tgid_enable = 1, .cs_tid_components = 1};
    uint32_t *spv = NULL;
    size_t words = 0;
    CHECK(sh_translate(p, &info, shapes, 0, &spv, &words));
    const char *dir = getenv("PS5C_TEST_SPIRV");
    if (dir) {
        char path[512];snprintf(path, sizeof(path), "%s/dynamic-buffers.spv", dir);
        FILE *f = fopen(path, "wb");
        CHECK(f && fwrite(spv, 4, words, f) == words && !fclose(f));
    }
    renderer *disabled = calloc(1, sizeof(*disabled));
    vk_buffer rejected;
    CHECK(disabled && !vk_buffer_import_host(disabled, data, GUEST_PAGE_SIZE, &rejected));
    CHECK(!rejected.buffer && !rejected.memory);
    free(disabled);
    if (getenv("PS5C_TEST_GPU")) {
        pal_window *window = NULL;
        CHECK(pal_window_open("ZeraPS5 dynamic guest buffers", 320, 180, &window) == PS5C_OK);
        renderer *r = renderer_create(window, &mem);
        CHECK(r && r->host_import_align);
        CHECK(gmem_pin_mappings(&mem));
        vk_buffer imported, uniform;
        CHECK(vk_buffer_import_host(r, data, GUEST_PAGE_SIZE, &imported));
        CHECK(!imported.mapped);
        vk_buffer invalid;
        CHECK(!vk_buffer_import_host(r, (uint8_t *)data + 4, GUEST_PAGE_SIZE, &invalid));
        CHECK(!vk_buffer_import_host(r, data, GUEST_PAGE_SIZE - 1, &invalid));
        CHECK(vk_buffer_create(r, e.smem_words * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, &uniform));
        memcpy(uniform.mapped, uniforms, e.smem_words * 4);
        VkDescriptorSetLayoutBinding bindings[7];
        for (uint32_t b = 0; b < 7; b++) bindings[b] = (VkDescriptorSetLayoutBinding){.binding = b,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
        VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 7, .pBindings = bindings};
        VkDescriptorSetLayout dsl;
        CHECK(vkCreateDescriptorSetLayout(r->device, &dl, NULL, &dsl) == VK_SUCCESS);
        VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
        VkPipelineLayout layout;
        CHECK(vkCreatePipelineLayout(r->device, &pl, NULL, &layout) == VK_SUCCESS);
        VkShaderModuleCreateInfo mc = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * 4, .pCode = spv};
        VkShaderModule module;
        CHECK(vkCreateShaderModule(r->device, &mc, NULL, &module) == VK_SUCCESS);
        VkComputePipelineCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = layout};
        VkPipeline pipeline;
        CHECK(vkCreateComputePipelines(r->device, VK_NULL_HANDLE, 1, &pc, NULL, &pipeline) == VK_SUCCESS);
        VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7};
        VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
        VkDescriptorPool pool;
        CHECK(vkCreateDescriptorPool(r->device, &pool_info, NULL, &pool) == VK_SUCCESS);
        VkDescriptorSetAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &dsl};
        VkDescriptorSet set;
        CHECK(vkAllocateDescriptorSets(r->device, &alloc, &set) == VK_SUCCESS);
        VkDescriptorBufferInfo buffers[7];VkWriteDescriptorSet writes[7];
        for (uint32_t b = 0; b < 7; b++) {
            buffers[b] = (VkDescriptorBufferInfo){b ? imported.buffer : uniform.buffer, 0, b ? imported.size : uniform.size};
            writes[b] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = b,
                .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = buffers + b};
        }
        vkUpdateDescriptorSets(r->device, 7, writes, 0, NULL);
        uint32_t expected[GUEST_PAGE_SIZE / 4];
        for (uint32_t round = 0; round < 3; round++) {
            if (round == 2) descriptor(data + 0x100 / 4, base + input_at[0], 0, 4); /* raw: byte count, index ignored */
            for (uint32_t group = 0; group < 2; group++)
                for (uint32_t lane = 0; lane < 64; lane++)
                    data[(input_at[group] + lane * input_stride[group]) / 4] = 0x12340000 + round * 10000 + group * 100 + lane;
            for (uint32_t group = 0; group < 4; group++) data[0x1c00 / 4 + group] = (round + 1) * 1000 + group * 100;
            memcpy(expected, data, sizeof(expected));
            for (uint32_t group = 0; group < 4; group++) {
                uint32_t scalar = group == 3 ? 0 : data[0x1c00 / 4 + group];
                for (uint32_t lane = 0; lane < 64; lane++) {
                    uint32_t step = round == 2 && group == 0 ? 0 : input_stride[group];
                    uint32_t v = group < 2 || (group == 3 && lane == 0) ? data[(input_at[group] + lane * step) / 4] : 0;
                    expected[(output_at[group] + lane * output_stride[group]) / 4] = v + scalar;
                }
            }
            VkCommandBuffer cmd = vk_begin(r);
            VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
            vkCmdDispatch(cmd, 4, 1, 1);
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
            vk_flush(r);
            CHECK(vkWaitForFences(r->device, 1, &r->work_fence, VK_TRUE, 10000000000ull) == VK_SUCCESS);
            for (uint32_t i = 0; i < PS5C_ARRAY_COUNT(expected); i++) {
                if (data[i] != expected[i]) fprintf(stderr, "round %u word %x: %08x expected %08x\n", round, i, data[i], expected[i]);
                CHECK(data[i] == expected[i]);
            }
        }
        vkDestroyDescriptorPool(r->device, pool, NULL);
        vkDestroyPipeline(r->device, pipeline, NULL);
        vkDestroyShaderModule(r->device, module, NULL);
        vkDestroyPipelineLayout(r->device, layout, NULL);
        vkDestroyDescriptorSetLayout(r->device, dsl, NULL);
        vk_buffer_destroy(r, &uniform);vk_buffer_destroy(r, &imported);
        gmem_unpin_mappings(&mem);
        renderer_destroy(r);pal_window_destroy(window);
        puts("dynamic buffers: GPU verified 768 selections/stores, scalar pointers, runtime/raw strides, high-word/bounds rejection and unchanged neighbours");
    }
    free(spv);sh_eval_free(&e);free(p);
    gmem_free(&mem);pal_shutdown();
    puts("dynamic buffers: ok");
    return 0;
}
