/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
#include <string.h>

#include "mem/gmem.h"
#include "pal/pal.h"
#include "test.h"

/* Sony's PSP emulator (PSPHD) maps the PSP main memory at 0x80_0800_0000 and
 * its uncached mirror at 0x80_4800_0000 onto the same physical pages: with the
 * shared-memory area moved below them, a write through one shows in the other. */
static void test_psp_mirrors_share_bytes(void) {
    if (!pal_ph_supported()) { puts("    SKIP: host has no placeholders"); return; }
    gmem m;
    gmem_config cfg = gmem_default_config();
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    CHECK(m.ph_start == 0x1000000000ull);
    guest_addr ram = 0x8008000000ull, mirror = 0x8048000000ull, out = 0;
    uint64_t phys = 0x18aa08000ull, value = 0;
    bool shared = false;
    CHECK(gmem_map_direct(&m, ram, 4 * GUEST_PAGE_SIZE, GUEST_PAGE_SIZE, GPROT_RW, GMAP_FIXED | GMAP_REPLACE, phys,
                          "PSP RAM", &out, &shared) == PS5C_OK && shared && out == ram);
    CHECK(gmem_map_direct(&m, mirror, 4 * GUEST_PAGE_SIZE, GUEST_PAGE_SIZE, GPROT_RW, GMAP_FIXED | GMAP_REPLACE, phys,
                          "PSP RAM mirror", &out, &shared) == PS5C_OK && shared && out == mirror);
    CHECK(gmem_write_u64(&m, mirror + GUEST_PAGE_SIZE + 8, 0x1122334455667788ull));
    CHECK(gmem_read_u64(&m, ram + GUEST_PAGE_SIZE + 8, &value) && value == 0x1122334455667788ull);
    CHECK(gmem_write_u64(&m, ram, 0x8877665544332211ull));
    CHECK(gmem_read_u64(&m, mirror, &value) && value == 0x8877665544332211ull);
    gmem_free(&m);
}

int main(void) {
    pal_init();
    log_set_level_all(LOG_LVL_ERROR);
    /* as the front end does: the early reservation first, then the move */
    if (pal_ph_supported() && (gmem_reserve_early() != PS5C_OK || gmem_set_shared_area_base(0x1000000000ull) != PS5C_OK)) {
        puts("cannot reserve guest memory early");
        return 1;
    }
    static const test_case tests[] = {
        TEST_CASE(test_psp_mirrors_share_bytes),
    };
    return test_run_all("mem_psp_mirrors", tests, PS5C_ARRAY_COUNT(tests));
}
