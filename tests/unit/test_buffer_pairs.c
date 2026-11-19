/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Publish {id, ~id} from competing workgroups. A reader must see one pair. */
#include "gpu/gpu_regs.h"
#include "gpu/vk/vk_internal.h"
#include "gpu/shader/shader.h"
#include "gpu/vk/vk_shader_cache.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int translation_test(void) {
    sh_program *p = calloc(1, sizeof(*p)); CHECK(p);
    sh_site_shape shapes[SH_MAX_SITES] = {0};
    sh_stage_info info = {.stage = SH_STAGE_CS, .local_size = {1,1,1}};
    uint32_t code[] = {0xe0344000,0x80000000,0xe0744000,0x80000000,0xbf810000};
    CHECK(sh_analyze(code, PS5C_ARRAY_COUNT(code), 0, p));
    CHECK(p->site_count == 2 && sh_binding_count(p) == 3);
    for (unsigned enabled = 0; enabled < 2; enabled++) {
        sh_set_buffer_atomic64(enabled != 0);
        for (unsigned dlc = 0; dlc < 3; dlc++) {
            p->insts[0].glc = p->insts[1].glc = dlc == 0;
            p->insts[0].dlc = p->insts[1].dlc = dlc == 1;
            uint32_t *spv = NULL; size_t words;
            CHECK(sh_translate(p, &info, shapes, 0, &spv, &words));
            unsigned loads = 0, stores = 0, caps = 0, bindings = 0;
            for (size_t i = 5; i < words;) {
                uint32_t op = spv[i] & 0xffff, n = spv[i] >> 16;
                CHECK(n && i+n <= words);
                if (op == 227) loads++;
                if (op == 228) stores++;
                if (op == 17 && spv[i+1] == 12) caps++;
                if (op == 71 && n == 4 && spv[i+2] == 33) { CHECK(spv[i+3] < 3); bindings++; }
                i += n;
            }
            unsigned atomic = enabled && dlc < 2;
            CHECK(loads == atomic && stores == atomic && caps == atomic);
            CHECK(bindings == 3 + 2*atomic); /* two views, the same three bindings */
            if (getenv("PS5C_TEST_SPIRV")) {
                char path[512]; snprintf(path,sizeof(path),"%s/pairs-%u-%u.spv",getenv("PS5C_TEST_SPIRV"),enabled,dlc);
                FILE *f = fopen(path,"wb"); CHECK(f && fwrite(spv,4,words,f)==words && !fclose(f));
            }
            free(spv);
        }
    }
    vk_shader_cache_device(false, false, false, false);
    uint64_t legacy = vk_shader_cache_key(p, &info, shapes, 0);
    vk_shader_cache_device(false, true, false, false);
    CHECK(legacy != vk_shader_cache_key(p, &info, shapes, 0));
    sh_set_buffer_atomic64(false);
    free(p); puts("coherent pairs: GLC/DLC lowering, capability fallback and cache separation verified");
    return 0;
}

int main(void) {
    CHECK(translation_test() == 0);
    if (!getenv("PS5C_TEST_GPU")) { puts("buffer pairs: set PS5C_TEST_GPU=1 for publication regression"); return 0; }
    CHECK(pal_init() == PS5C_OK);
    gmem m;
    gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr pc, out;
    const uint32_t groups = 16384, bytes = (groups + 1) * 8;
    CHECK(gmem_map(&m, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "pair code", &pc) == PS5C_OK);
    CHECK(gmem_map(&m, 0, PS5C_ALIGN_UP(bytes, GUEST_PAGE_SIZE), GPROT_RW, 0, "pairs", &out) == PS5C_OK);
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 coherent buffer pairs", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m); CHECK(r);
    VkPhysicalDeviceShaderAtomicInt64Features a64 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &a64};
    PFN_vkGetPhysicalDeviceFeatures2 get_f2 = (PFN_vkGetPhysicalDeviceFeatures2)vk_api_instance_proc(r->instance,"vkGetPhysicalDeviceFeatures2");
    if (get_f2) get_f2(r->phys, &f2);
    if (!a64.shaderBufferInt64Atomics) {
        puts("buffer pairs GPU: skipped, device lacks shaderBufferInt64Atomics");
        renderer_destroy(r); pal_window_destroy(window); gmem_free(&m); pal_shutdown(); return 0;
    }
    gpu g; CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    g.regs[mmCOMPUTE_NUM_THREAD_X] = g.regs[mmCOMPUTE_NUM_THREAD_Y] = g.regs[mmCOMPUTE_NUM_THREAD_Z] = 1;
    g.regs[mmCOMPUTE_PGM_RSRC2] = (4u << 1) | (1u << 7); /* descriptor s0:3, group ID s4 */
    uint32_t *u = g.regs + mmCOMPUTE_USER_DATA_0;
    u[0] = (uint32_t)out; u[1] = (uint32_t)(out >> 32) | (8u << 16);
    u[2] = groups + 1; u[3] = 0x00016204;
    const uint32_t code[] = {
        0x7e020204, /* v1 = group ID s4 */
        0x7e040000 | (0x37u << 9) | 257, /* v2 = ~v1 */
        0xe0744000, 0x80000100, /* buffer_store_dwordx2 v[1:2], offset 0, glc */
        0xe0344000, 0x80000300, /* buffer_load_dwordx2 v[3:4], offset 0, glc */
        0x80058004, /* s_add_u32 s5, s4, 1 */
        0x7e000205, /* v0 = s5 */
        0xe0742000, 0x80000300, /* store observed v[3:4] at (group+1)*8 */
        0xbf810000
    };
    CHECK(gmem_write(&m, pc, code, sizeof(code)));
    g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(pc >> 8);
    g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)(pc >> 40);
    uint32_t *p = gmem_host(&m, out, bytes); CHECK(p);
    unsigned torn = 0;
    for (unsigned run = 0; run < 16; run++) {
        for (uint32_t i = 0; i <= groups; i++) { p[2*i] = 0; p[2*i+1] = UINT32_MAX; }
        renderer_submit_begin(r);
        renderer_dispatch(r, &g, groups, 1, 1);
        renderer_sync_mem(r, out, bytes);
        for (uint32_t i = 1; i <= groups; i++) {
            if ((p[2*i] ^ p[2*i+1]) != UINT32_MAX) torn++;
            CHECK(p[2*i] < groups);
        }
    }
    printf("publication: %u torn pairs in %u observations\n", torn, groups * 16);
    /* An indexed pair cannot cross a four-byte record. The old split path
     * loaded the next record and overwrote it on store. */
    guest_addr bounds_pc = pc + 0x100;
    const uint32_t bounds_code[] = {
        0xe0346000,0x80000100, /* indexed coherent load v1:2, stride 4 */
        0xe0746000,0x80000100, /* indexed coherent store v1:2, stride 4 */
        0xe0742000,0x80010100, /* store the observed pair through descriptor s4:7 */
        0xbf810000
    };
    CHECK(gmem_write(&m, bounds_pc, bounds_code, sizeof(bounds_code)));
    g.regs[mmCOMPUTE_PGM_RSRC2] = 8u << 1;
    u[1] = (uint32_t)(out >> 32) | (4u << 16); u[2] = 4;
    u[4] = (uint32_t)(out+16); u[5] = (uint32_t)((out+16)>>32) | (8u<<16); u[6]=1; u[7]=0x00016204;
    p[0] = 0x12345678; p[1] = 0xaabbccdd; p[4] = p[5] = 0xdeadbeef;
    g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(bounds_pc >> 8);
    g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)(bounds_pc >> 40);
    renderer_submit_begin(r); renderer_dispatch(r, &g, 1,1,1); renderer_sync_mem(r,out,32);
    CHECK(p[0] == 0x12345678 && p[1] == 0xaabbccdd);
    CHECK(p[4] == 0x12345678 && p[5] == 0);
    puts("four-byte record: second component zero and adjacent record preserved");
    renderer_destroy(r); free(g.regs); pal_mutex_destroy(g.lock); pal_window_destroy(window); gmem_free(&m); pal_shutdown();
    CHECK(!torn);
    return 0;
}
