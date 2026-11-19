/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The instructions the translator gained when the ISA was completed: one
 * program per case (tools/gen_isa_tests.py), each must analyze and translate;
 * PS5C_TEST_GPU=1 runs them on the host Vulkan device and checks the value
 * all 64 lanes store against the instruction's definition, and for memory
 * instructions the data buffer (s[8:11], its address in s[8:9]) after them. */
#include "gpu/gpu_regs.h"
#include "gpu/renderer.h"
#include "gpu/shader/shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

typedef struct isa_case {
    const char *name, *text;
    uint32_t n;
    uint32_t code[48];
    uint32_t expect[2];
    int check;            /* the lanes' stored values */
    uint32_t mem_off, mem_len; /* bytes of the data buffer after the dispatch */
    uint8_t mem[256];
} isa_case;

#define ISA_PATTERN
#include "test_shader_isa_cases.inc" /* k_pattern: the data buffer before each case */
#undef ISA_PATTERN
static const isa_case k_cases[] = {
#include "test_shader_isa_cases.inc"
};

static int gpu_test(void) {
    gmem m;
    guest_addr a;
    CHECK(pal_init() == PS5C_OK);
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    CHECK(gmem_map(&m, 0, 64 * GUEST_PAGE_SIZE, GPROT_RW, 0, "ISA", &a) == PS5C_OK);
    guest_addr out_addr = a + 60 * GUEST_PAGE_SIZE, data_addr = a + 56 * GUEST_PAGE_SIZE;
    uint32_t *output = gmem_host(&m, out_addr, 64 * 8);
    uint8_t *data = gmem_host(&m, data_addr, sizeof(k_pattern));
    CHECK(output && data);
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 ISA regression", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m);
    CHECK(r);
    gpu g;
    CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    g.regs[mmCOMPUTE_PGM_RSRC2] = (16u << 1) | (1u << 11);
    g.regs[mmCOMPUTE_NUM_THREAD_X] = 64;
    g.regs[mmCOMPUTE_NUM_THREAD_Y] = g.regs[mmCOMPUTE_NUM_THREAD_Z] = 1;
    uint32_t *u = g.regs + mmCOMPUTE_USER_DATA_0;
    u[12] = (uint32_t)out_addr;
    u[13] = (uint32_t)(out_addr >> 32) | (8u << 16); /* stride 8: v[4:5] per lane */
    u[14] = 64;
    u[15] = 0x00016204;
    u[8] = (uint32_t)data_addr; /* raw buffer over the data */
    u[9] = (uint32_t)(data_addr >> 32);
    u[10] = (uint32_t)sizeof(k_pattern);
    u[11] = 0x00016204;
    unsigned failed = 0, checked = 0;
    for (unsigned k = 0; k < PS5C_ARRAY_COUNT(k_cases); k++) {
        const isa_case *c = &k_cases[k];
        guest_addr pc = a + k * 0x200;
        CHECK(pc + c->n * 4 < data_addr);
        CHECK(gmem_write(&m, pc, c->code, c->n * 4));
        g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(pc >> 8);
        g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)(pc >> 40);
        for (unsigned i = 0; i < 128; i++) output[i] = 0xabababab;
        memcpy(data, k_pattern, sizeof(k_pattern));
        renderer_submit_begin(r);
        renderer_dispatch(r, &g, 1, 1, 1);
        renderer_sync_mem(r, out_addr, 64 * 8);
        renderer_sync_mem(r, data_addr, sizeof(k_pattern));
        if (c->mem_len) {
            checked++;
            for (unsigned i = 0; i < c->mem_len; i++) {
                if (data[c->mem_off + i] == c->mem[i]) continue;
                fprintf(stderr, "%s (%s) byte 0x%x: %02x, expected %02x\n", c->name, c->text, c->mem_off + i,
                        data[c->mem_off + i], c->mem[i]);
                failed++;
                break;
            }
        }
        if (!c->check) continue;
        checked++;
        for (unsigned i = 0; i < 64; i++) {
            if (output[2 * i] == c->expect[0] && output[2 * i + 1] == c->expect[1]) continue;
            fprintf(stderr, "%s (%s) lane %u: %08x %08x, expected %08x %08x\n", c->name, c->text, i, output[2 * i],
                    output[2 * i + 1], c->expect[0], c->expect[1]);
            failed++;
            break;
        }
    }
    renderer_destroy(r);
    free(g.regs);
    pal_mutex_destroy(g.lock);
    pal_window_destroy(window);
    gmem_free(&m);
    pal_shutdown();
    printf("ISA GPU: %u of %u cases match\n", checked - failed, checked);
    return failed ? 1 : 0;
}

int main(void) {
    sh_program *p = calloc(1, sizeof(*p));
    CHECK(p);
    sh_stage_info info = {.stage = SH_STAGE_CS, .cs_tid_components = 1, .cs_tgid_sgpr = 16, .local_size = {64, 1, 1}};
    sh_site_shape shapes[4];
    memset(shapes, 0, sizeof(shapes));
    unsigned bad = 0;
    for (unsigned k = 0; k < PS5C_ARRAY_COUNT(k_cases); k++) {
        const isa_case *c = &k_cases[k];
        uint32_t *spv = NULL;
        size_t words = 0;
        if (!sh_analyze(c->code, c->n, 0x100000, p) || !sh_translate(p, &info, shapes, 0, &spv, &words)) {
            fprintf(stderr, "%s (%s): not translated\n", c->name, c->text);
            bad++;
            continue;
        }
        const char *dir = getenv("PS5C_TEST_SPIRV");
        if (dir) {
            char path[512];
            snprintf(path, sizeof(path), "%s/isa_%s.spv", dir, c->name);
            FILE *f = fopen(path, "wb");
            CHECK(f);
            CHECK(fwrite(spv, 4, words, f) == words);
            CHECK(fclose(f) == 0);
        }
        free(spv);
    }
    free(p);
    CHECK(bad == 0);
    printf("ISA translation: %u cases\n", (unsigned)PS5C_ARRAY_COUNT(k_cases));
    if (getenv("PS5C_TEST_GPU")) CHECK(gpu_test() == 0);
    return 0;
}
