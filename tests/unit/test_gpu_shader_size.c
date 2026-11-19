/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/gpu.h"
#include "test.h"

static void test_scan_page_boundary(void) {
    CHECK(pal_init() == PS5C_OK);
    gmem mem;
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    CHECK(gmem_init(&mem, &cfg) == PS5C_OK);
    guest_addr addr;
    CHECK(gmem_map(&mem, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "shader boundary", &addr) == PS5C_OK);
    uint32_t *code = gmem_host(&mem, addr, GUEST_PAGE_SIZE);
    CHECK(code);
    memset(code, 0, GUEST_PAGE_SIZE);
    gpu g = {.mem = &mem};
    CHECK(gpu_shader_size(&g, addr) == 0);
    /* First word fits, but the next would straddle the unmapped boundary. */
    CHECK(gpu_shader_size(&g, addr + GUEST_PAGE_SIZE - 5) == 0);
    code[GUEST_PAGE_SIZE / 4 - 1] = 0xBF810000u;
    CHECK(gpu_shader_size(&g, addr) == GUEST_PAGE_SIZE / 4);
    CHECK(gpu_shader_size(&g, addr + GUEST_PAGE_SIZE - 4) == 1);
    CHECK(gpu_shader_size(&g, addr + GUEST_PAGE_SIZE) == 0);
    gmem_free(&mem);
    pal_shutdown();
}

int main(void) {
    const test_case tests[] = {TEST_CASE(test_scan_page_boundary)};
    return test_run_all("gpu_shader_size", tests, PS5C_ARRAY_COUNT(tests));
}
