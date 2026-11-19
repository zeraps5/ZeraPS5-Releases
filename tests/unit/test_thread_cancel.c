/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pthread_cancel: guest threads that end at a cancellation point, the cancel
 * state, and the cleanup handlers. */
#include "core/runtime.h"
#include "hle/hle.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

#define CANCELED 1ull

static ps5_runtime *g_rt;
static module_set g_ms;
static guest_addr g_page; /* code at +0, the thread's argument block at +0x800 */
#define BLOCK (g_page + 0x800)
static int g_bad; /* failures inside helpers that return a value */

static void setup(void) {
    module_set_init(&g_ms);
    g_rt = calloc(1, sizeof(*g_rt));
    runtime_options opts = {0};
    CHECK(g_rt && runtime_init(g_rt, &g_ms, &opts) == PS5C_OK);
    CHECK(gmem_map(&g_rt->mem, 0, GUEST_PAGE_SIZE, GPROT_RW | GPROT_EXEC, 0, "cancel-test", &g_page) == PS5C_OK);
}

static void teardown(void) {
    CHECK(g_bad == 0);
    runtime_free(g_rt);
    free(g_rt);
    module_set_free(&g_ms);
}

static void load_code(const uint8_t *code, size_t size) {
    CHECK(gmem_write(&g_rt->mem, g_page, code, size));
    pal_icache_flush(gmem_host(&g_rt->mem, g_page, size), size);
}

static void put(uint32_t slot, uint64_t v) { CHECK(gmem_write(&g_rt->mem, BLOCK + slot * 8, &v, 8)); }
static uint64_t get(uint32_t slot) {
    uint64_t v = 0;
    g_bad += !gmem_read(&g_rt->mem, BLOCK + slot * 8, &v, 8);
    return v;
}
static uint64_t stub(const char *name) {
    guest_addr a = runtime_resolve_symbol(g_rt, name);
    g_bad += !a;
    return a;
}
static bool finished(guest_thread *t) {
    pal_mutex_lock(g_rt->threads_lock);
    bool f = t->finished;
    pal_mutex_unlock(g_rt->threads_lock);
    return f;
}

/* entry(block): for (;;) block[0](); */
static const uint8_t k_loop[] = {0x53, 0x48, 0x89, 0xfb, 0xff, 0x13, 0xeb, 0xfc};

static void ends_at_a_cancellation_point(void) {
    setup();
    load_code(k_loop, sizeof(k_loop));
    put(0, stub("sched_yield"));
    guest_thread *t = NULL;
    CHECK(runtime_start_thread(g_rt, g_page, BLOCK, 0, 0x20000, false, "loop", &t) == 0);
    uint64_t handle = (uint64_t)(uintptr_t)t, value = 0;
    runtime_release_thread(g_rt, t);
    CHECK(runtime_cancel_thread(g_rt, handle) == 0);
    pal_sleep_ns(50000000);
    CHECK(!finished(t)); /* sched_yield is not a cancellation point */
    put(0, stub("pthread_testcancel"));
    CHECK(runtime_join_thread(g_rt, handle, &value) == 0);
    CHECK_EQ_U64(value, CANCELED);
    teardown();
}

/* entry(block): setcancelstate(DISABLE, 0); do block[0](); while (!block[2]);
 * setcancelstate(ENABLE, 0); for (;;) block[0]();   with block[1] = setcancelstate */
static const uint8_t k_disabled[] = {0x53, 0x48, 0x89, 0xfb, 0xbf, 0x01, 0x00, 0x00, 0x00, 0x31, 0xf6, 0xff,
                                     0x53, 0x08, 0xff, 0x13, 0x48, 0x83, 0x7b, 0x10, 0x00, 0x74, 0xf7, 0x31,
                                     0xff, 0x31, 0xf6, 0xff, 0x53, 0x08, 0xff, 0x13, 0xeb, 0xfc};

static void waits_while_disabled(void) {
    setup();
    load_code(k_disabled, sizeof(k_disabled));
    put(0, stub("pthread_testcancel"));
    put(1, stub("pthread_setcancelstate"));
    put(2, 0);
    guest_thread *t = NULL;
    CHECK(runtime_start_thread(g_rt, g_page, BLOCK, 0, 0x20000, false, "disabled", &t) == 0);
    uint64_t handle = (uint64_t)(uintptr_t)t, value = 0;
    runtime_release_thread(g_rt, t);
    pal_sleep_ns(20000000); /* let it disable cancellation first */
    CHECK(runtime_cancel_thread(g_rt, handle) == 0);
    pal_sleep_ns(50000000);
    CHECK(!finished(t));
    put(2, 1);
    CHECK(runtime_join_thread(g_rt, handle, &value) == 0);
    CHECK_EQ_U64(value, CANCELED);
    teardown();
}

/* entry(block): __pthread_cleanup_push_imp(block[1], block[2], 0); for (;;) block[0]();
 * with block[3] = the push; the handler at +0x40: *(uint64_t *)arg = 42 */
static const uint8_t k_cleanup[] = {0x53, 0x48, 0x89, 0xfb, 0x48, 0x8b, 0x7b, 0x08, 0x48, 0x8b, 0x73,
                                    0x10, 0x31, 0xd2, 0xff, 0x53, 0x18, 0xff, 0x13, 0xeb, 0xfc};
static const uint8_t k_handler[] = {0x48, 0xc7, 0x07, 0x2a, 0x00, 0x00, 0x00, 0xc3};

static void runs_cleanup_handlers(void) {
    setup();
    uint8_t code[0x48] = {0};
    memcpy(code, k_cleanup, sizeof(k_cleanup));
    memcpy(code + 0x40, k_handler, sizeof(k_handler));
    load_code(code, sizeof(code));
    put(0, stub("pthread_testcancel"));
    put(1, g_page + 0x40);
    put(2, BLOCK + 4 * 8);
    put(3, stub("__pthread_cleanup_push_imp"));
    put(4, 0);
    guest_thread *t = NULL;
    CHECK(runtime_start_thread(g_rt, g_page, BLOCK, 0, 0x20000, false, "cleanup", &t) == 0);
    uint64_t handle = (uint64_t)(uintptr_t)t, value = 0;
    runtime_release_thread(g_rt, t);
    pal_sleep_ns(20000000); /* let it install the handler */
    CHECK(runtime_cancel_thread(g_rt, handle) == 0);
    CHECK(runtime_join_thread(g_rt, handle, &value) == 0);
    CHECK_EQ_U64(value, CANCELED);
    CHECK_EQ_U64(get(4), 42);
    teardown();
}

static uint64_t call(const char *lib, const char *name, guest_thread *self, uint64_t a0, uint64_t a1, uint64_t a2) {
    const hle_export *e = hle_find(lib, nid_hash_name(name));
    g_bad += !(e && e->fn);
    hle_args args = {.gpr = {a0, a1, a2}};
    hle_call c = {.rt = g_rt, .mem = &g_rt->mem, .a = &args, .thread = self};
    if (e && e->fn) e->fn(&c);
    return (uint32_t)args.ret[0];
}

static void state_and_errors(void) {
    setup();
    guest_thread self;
    memset(&self, 0, sizeof(self));
    guest_addr old = BLOCK;
    /* state: ENABLE 0 / DISABLE 1 */
    put(0, 99);
    CHECK_EQ_U64(call("libScePosix", "pthread_setcancelstate", &self, 1, old, 0), 0);
    CHECK_EQ_U64((uint32_t)get(0), 0);
    CHECK(self.cancel_disabled);
    CHECK_EQ_U64(call("libkernel", "scePthreadSetcancelstate", &self, 0, old, 0), 0);
    CHECK_EQ_U64((uint32_t)get(0), 1);
    CHECK(!self.cancel_disabled);
    CHECK_EQ_U64(call("libScePosix", "pthread_setcancelstate", &self, 2, 0, 0), 22);
    CHECK_EQ_U64(call("libkernel", "scePthreadSetcancelstate", &self, 2, 0, 0), 0x80020016u);
    /* type: DEFERRED 0 / ASYNCHRONOUS 2 */
    CHECK_EQ_U64(call("libScePosix", "pthread_setcanceltype", &self, 2, old, 0), 0);
    CHECK_EQ_U64((uint32_t)get(0), 0);
    CHECK(self.cancel_async);
    CHECK_EQ_U64(call("libScePosix", "pthread_setcanceltype", &self, 0, old, 0), 0);
    CHECK_EQ_U64((uint32_t)get(0), 2);
    CHECK_EQ_U64(call("libScePosix", "pthread_setcanceltype", &self, 1, 0, 0), 22);
    /* unknown threads */
    CHECK_EQ_U64(call("libScePosix", "pthread_cancel", &self, 0x1234, 0, 0), 3);
    CHECK_EQ_U64(call("libkernel", "scePthreadCancel", &self, 0x1234, 0, 0), 0x80020003u);
    CHECK_EQ_U64(call("libScePosix", "pthread_getschedparam", &self, 0x1234, old, old + 8), 3);
    /* the points */
    CHECK(runtime_is_cancel_point("usleep") && runtime_is_cancel_point("scePthreadCondWait"));
    CHECK(!runtime_is_cancel_point("sched_yield") && !runtime_is_cancel_point("pthread_mutex_lock"));
    CHECK(!runtime_is_cancel_point(NULL));
    teardown();
}

int main(void) {
    if (pal_init() != PS5C_OK) return 2;
    const test_case tests[] = {TEST_CASE(state_and_errors), TEST_CASE(ends_at_a_cancellation_point),
                               TEST_CASE(waits_while_disabled), TEST_CASE(runs_cleanup_handlers)};
    int result = test_run_all("thread_cancel", tests, PS5C_ARRAY_COUNT(tests));
    pal_shutdown();
    return result;
}
