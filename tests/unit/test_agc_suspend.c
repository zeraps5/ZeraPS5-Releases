/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hle/hle.h"
#include "core/runtime.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include "test.h"

/* The driver owns a process-long worker; keep its test memory alive until
 * process exit, as the runtime does. No console modules or host GPU needed. */
static gmem mem;
static ps5_runtime empty_runtime;
static bool use_empty_runtime;

static int32_t call(uint64_t nid, uint64_t packet, uint64_t status) {
    const hle_export *exp = hle_find("libSceAgcDriver", nid);
    if (!exp || !exp->fn) return INT32_MIN;
    hle_args args = {.gpr = {packet, status, 1}};
    hle_call c = {.a = &args, .mem = &mem, .rt = use_empty_runtime ? &empty_runtime : NULL};
    exp->fn(&c);
    return (int32_t)args.ret[0];
}

static void test_suspend_submits_once_and_drains(void) {
    CHECK(hle_registry_init() == PS5C_OK);
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    CHECK(gmem_init(&mem, &cfg) == PS5C_OK);
    guest_addr base = 0;
    const uint32_t repeats = 32768, words = repeats * 8;
    uint64_t size = ((uint64_t)words * 4 + 4096 + GUEST_PAGE_SIZE - 1) & ~(GUEST_PAGE_SIZE - 1);
    CHECK(gmem_map(&mem, GUEST_MODULE_BASE, size, GPROT_RW, GMAP_FIXED, "AGC test", &base) == PS5C_OK);
    guest_addr packet = base, empty_packet = base + 32, status = base + 64, pending_packet = base + 80;
    guest_addr counter = base + 128, cmd = base + 4096;
    uint32_t *p = (uint32_t *)gmem_host(&mem, cmd, (uint64_t)words * 4);
    CHECK(p != NULL);
    /* ATOMIC_MEM add 1: replay is observable and cannot be hidden by an
     * idempotent write. Enough work to expose an asynchronous early return. */
    for (uint32_t i = 0; i < repeats; i++) {
        uint32_t a[8] = {0xc0061e00u, 9, (uint32_t)counter, (uint32_t)(counter >> 32), 1, 0, 0, 0};
        memcpy(p + i * 8, a, sizeof(a));
    }
    CHECK(gmem_write_u64(&mem, packet, cmd));
    CHECK(gmem_write(&mem, packet + 8, &words, 4));
    const uint32_t one_atomic = 8;
    CHECK(gmem_write_u64(&mem, pending_packet, cmd));
    CHECK(gmem_write(&mem, pending_packet + 8, &one_atomic, 4));
    uint64_t suspend = 0;
    CHECK(nid_from_str("kAPt2J4U3v8", NID_STR_LEN, &suspend));
    CHECK(call(nid_hash_name("sceAgcDriverSubmitDcb"), packet, 0) == 0);
    uint32_t sentinel = 0xdeadbeef, value = 0;
    CHECK(gmem_write(&mem, status, &sentinel, 4));
    CHECK(call(suspend, pending_packet, status) == 0);
    CHECK(gmem_read(&mem, counter, &value, 4) && value == repeats + 1);
    CHECK(gmem_read(&mem, status, &value, 4) && value == 0);
    /* An empty descriptor still suspends preceding submissions. */
    CHECK(call(nid_hash_name("sceAgcDriverSubmitDcb"), packet, 0) == 0);
    CHECK(call(suspend, empty_packet, 0) == 0);
    CHECK(gmem_read(&mem, counter, &value, 4) && value == repeats * 2 + 1);
    CHECK(call(suspend, 0, 0) == SCE_KERNEL_ERROR_EINVAL);
    CHECK(call(suspend, empty_packet, 1) == SCE_KERNEL_ERROR_EINVAL);
}

int main(int argc, char **argv) {
    use_empty_runtime = argc == 2 && !strcmp(argv[1], "--empty-runtime");
    pal_init();
    const test_case tests[] = {TEST_CASE(test_suspend_submits_once_and_drains)};
    return test_run_all("agc_suspend", tests, PS5C_ARRAY_COUNT(tests));
}
