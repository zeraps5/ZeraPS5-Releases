/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/runtime.h"
#include "hle/hle.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include "test.h"
#include <stdlib.h>

/* Return the handle the parent published at arg[0]. */
static const uint8_t code[] = {0x48, 0x8b, 0x07, 0xc3};

static void publication_before_guest_entry(void) {
    module_set ms;
    module_set_init(&ms);
    ps5_runtime *rt = calloc(1, sizeof(*rt));
    runtime_options opts = {0};
    CHECK(rt && runtime_init(rt, &ms, &opts) == PS5C_OK);
    guest_addr page;
    CHECK(gmem_map(&rt->mem, 0, GUEST_PAGE_SIZE, GPROT_RW | GPROT_EXEC, 0, "start-test", &page) == PS5C_OK);
    CHECK(gmem_write(&rt->mem, page, code, sizeof(code)));
    pal_icache_flush(gmem_host(&rt->mem, page, sizeof(code)), sizeof(code));
    int failures = 0;
    for (unsigned i = 0; i < 32; ++i) {
        uint64_t zero = 0, value = 0;
        CHECK(gmem_write(&rt->mem, page + 64, &zero, sizeof(zero)));
        guest_thread *t = NULL;
        CHECK(runtime_start_thread(rt, page, page + 64, 0, 0x10000, false, "publication", &t) == 0);
        uint64_t handle = (uint64_t)(uintptr_t)t;
        /* Let the host schedule the child while the parent deliberately has
         * not published anything. The old runtime enters guest code here. */
        pal_sleep_ns(2000000);
        pal_mutex_lock(rt->threads_lock);
        bool ran_early = t->finished;
        pal_mutex_unlock(rt->threads_lock);
        failures += ran_early;
        CHECK(gmem_write(&rt->mem, page + 64, &handle, sizeof(handle)));
#ifndef PS5C_TEST_LEGACY
        runtime_release_thread(rt, t);
#endif
        CHECK(runtime_join_thread(rt, handle, &value) == 0);
        failures += value != handle;
    }
    /* Exercise the public HLE route too, including a child that exits as
     * soon as it can read the output slot. */
    const hle_export *create = hle_find("libkernel", nid_hash_name("scePthreadCreate"));
    CHECK(create && create->fn);
    for (unsigned i = 0; i < 128; ++i) {
        uint64_t zero = 0, handle, value;
        CHECK(gmem_write(&rt->mem, page + 64, &zero, sizeof(zero)));
        hle_args args = {.gpr = {page + 64, 0, page, page + 64, 0}};
        hle_call call = {.rt = rt, .mem = &rt->mem, .a = &args};
        create->fn(&call);
        CHECK(args.ret[0] == 0);
        CHECK(gmem_read(&rt->mem, page + 64, &handle, sizeof(handle)));
        CHECK(runtime_join_thread(rt, handle, &value) == 0);
        failures += value != handle;
    }
    runtime_free(rt);
    free(rt);
    module_set_free(&ms);
    CHECK(failures == 0);
}

int main(void) {
    if (pal_init() != PS5C_OK) return 2;
    const test_case tests[] = {TEST_CASE(publication_before_guest_entry)};
    int result = test_run_all("thread_start", tests, PS5C_ARRAY_COUNT(tests));
    pal_shutdown();
    return result;
}
