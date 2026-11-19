/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise actual APR queues: blocked jobs must stay pending across other
 * queues' progress, queue-full inline execution, and explicit sync mode. */
#include <stdatomic.h>
#include <stdlib.h>
#include "core/runtime.h"
#include "hle/amm.h"
#include "hle/ampr_records.h"
#include "hle/hle.h"
#include "pal/pal.h"
#include "test.h"

static ps5_runtime rt;
static guest_addr page;

static uint64_t value(guest_addr at) {
    uint64_t out = 0;
    gmem_read_u64(&rt.mem, at, &out);
    return out;
}

static bool until_value(guest_addr at, uint64_t expected) {
    uint64_t end = pal_time_ns() + 2000000000ull;
    while (value(at) != expected && pal_time_ns() < end) pal_sleep_ns(1000000);
    return value(at) == expected;
}

static void record(guest_addr at, uint8_t op, guest_addr address, uint64_t val) {
    uint64_t words[4] = {op, address, val, 0};
    gmem_write(&rt.mem, at, words, sizeof(words));
}

static void blocked_buffer(guest_addr at, guest_addr entered, guest_addr gate, guest_addr output) {
    gmem_write_u64(&rt.mem, entered, 0);
    gmem_write_u64(&rt.mem, gate, 0);
    gmem_write_u64(&rt.mem, output, 0);
    record(at, AMPR_OP_WRITE, entered, 1);
    record(at + 32, AMPR_OP_WAIT, gate, 1);
    record(at + 64, AMPR_OP_WRITE, output, 0x123456789abcdef0ull);
}

typedef struct operation {
    uint32_t id;
    guest_addr cb, result;
    bool fail_copy;
    atomic_bool entered, returned, registered, copy_failed;
} operation;

#ifdef PS5C_TEST_APR_WAIT_HANDSHAKE
/* Observe entry to a real blocking primitive, without adding a production
 * hook or replacing APR logic. TLS excludes worker-side waits/sleeps. */
static _Thread_local operation *waiting_op;
static _Thread_local operation *submitting_op;
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size);
void *__wrap_malloc(size_t size) {
    if (submitting_op && submitting_op->fail_copy && size == 96 &&
        !atomic_exchange(&submitting_op->copy_failed, true)) return NULL;
    return __real_malloc(size);
}
void __real_pal_cond_wait(pal_cond *cond, pal_mutex *mutex);
void __wrap_pal_cond_wait(pal_cond *cond, pal_mutex *mutex);
void __wrap_pal_cond_wait(pal_cond *cond, pal_mutex *mutex) {
    if (waiting_op) atomic_store(&waiting_op->registered, true);
    __real_pal_cond_wait(cond, mutex);
}
void __real_pal_sleep_ns(uint64_t ns);
void __wrap_pal_sleep_ns(uint64_t ns);
void __wrap_pal_sleep_ns(uint64_t ns) {
    if (waiting_op) atomic_store(&waiting_op->registered, true);
    __real_pal_sleep_ns(ns);
}
#endif

static int32_t submit_thread(void *arg) {
    operation *op = arg;
    hle_call c = {.rt = &rt, .mem = &rt.mem};
    atomic_store(&op->entered, true);
#ifdef PS5C_TEST_APR_WAIT_HANDSHAKE
    submitting_op = op;
#endif
    op->id = apr_submit_amm(&c, op->cb, 96, 0, op->result);
#ifdef PS5C_TEST_APR_WAIT_HANDSHAKE
    submitting_op = NULL;
#endif
    atomic_store(&op->returned, true);
    return 0;
}

static int32_t wait_thread(void *arg) {
    operation *op = arg;
    atomic_store(&op->entered, true);
#ifdef PS5C_TEST_APR_WAIT_HANDSHAKE
    waiting_op = op;
#endif
    apr_wait(op->id);
    atomic_store(&op->returned, true);
    return 0;
}

static bool wait_started(operation *op) {
    uint64_t end = pal_time_ns() + 2000000000ull;
#ifdef PS5C_TEST_APR_WAIT_HANDSHAKE
    while (!atomic_load(&op->registered) && !atomic_load(&op->returned) && pal_time_ns() < end)
        pal_sleep_ns(1000000);
    return atomic_load(&op->entered) && (atomic_load(&op->registered) || atomic_load(&op->returned));
#else
    while (!atomic_load(&op->entered) && pal_time_ns() < end) pal_sleep_ns(1000000);
    return atomic_load(&op->entered);
#endif
}

static void old_id_stays_pending_while_another_queue_advances(void) {
    hle_call c = {.rt = &rt, .mem = &rt.mem};
    guest_addr entered = page + 4096, gate = entered + 8, output = entered + 16;
    guest_addr result = entered + 24;
    blocked_buffer(page, entered, gate, output);
    gmem_write_u64(&rt.mem, result, UINT64_MAX);
    uint32_t slow = apr_submit_amm(&c, page, 96, 0, result);
    bool reached = until_value(entered, 1);
    uint8_t noop[4] = {AMPR_OP_SKIP, 4, 0, 0};
    gmem_write(&rt.mem, page + 256, noop, sizeof(noop));
    for (unsigned i = 0; i < 1100; i++) apr_wait(apr_submit_amm(&c, page + 256, 4, 1, 0));
    operation wait = {.id = slow};
    pal_thread *thread = NULL;
    bool made = pal_thread_create(&thread, wait_thread, &wait, "APR old id", 0) == PS5C_OK;
    bool started = made && wait_started(&wait);
    pal_sleep_ns(30000000);
    bool premature = atomic_load(&wait.returned);
    /* Always release the actual worker, including on a failing old build. */
    gmem_write_u64(&rt.mem, gate, 1);
    if (made) pal_thread_join(thread, NULL);
    apr_wait(slow);
    apr_wait(0);
    CHECK(reached && made && started && atomic_load(&wait.returned));
    CHECK(!premature);
    CHECK_EQ_U64(value(output), 0x123456789abcdef0ull);
    CHECK_EQ_U64(value(result), 0);
}

static void global_wait_includes_queue_full_inline_execution(void) {
    hle_call c = {.rt = &rt, .mem = &rt.mem};
    guest_addr e1 = page + 4096, gate1 = e1 + 8, output1 = e1 + 16;
    guest_addr e2 = page + 8192, gate2 = e2 + 8, output2 = e2 + 16, result2 = e2 + 24;
    blocked_buffer(page, e1, gate1, output1);
    blocked_buffer(page + 512, e2, gate2, output2);
    gmem_write_u64(&rt.mem, result2, UINT64_MAX);
    apr_submit_amm(&c, page, 96, 0, 0);
    bool reached1 = until_value(e1, 1);
    uint8_t noop[4] = {AMPR_OP_SKIP, 4, 0, 0};
    gmem_write(&rt.mem, page + 256, noop, sizeof(noop));
    uint32_t last = 0;
    for (unsigned i = 0; i < 255; i++) last = apr_submit_amm(&c, page + 256, 4, 0, 0);
    operation submit = {.cb = page + 512, .result = result2}, wait = {0};
    pal_thread *st = NULL, *wt = NULL;
    bool smade = pal_thread_create(&st, submit_thread, &submit, "APR full fallback", 0) == PS5C_OK;
    bool reached2 = smade && until_value(e2, 1);
    bool wmade = pal_thread_create(&wt, wait_thread, &wait, "APR all jobs", 0) == PS5C_OK;
    bool started = wmade && wait_started(&wait);
    gmem_write_u64(&rt.mem, gate1, 1);
    apr_wait(last); /* the queue has drained; inline work is still blocked */
    pal_sleep_ns(30000000);
    bool premature = atomic_load(&wait.returned);
    gmem_write_u64(&rt.mem, gate2, 1);
    if (smade) pal_thread_join(st, NULL);
    if (wmade) pal_thread_join(wt, NULL);
    apr_wait(0);
    CHECK(reached1 && reached2 && smade && wmade && started);
    CHECK(!premature);
    CHECK_EQ_U64(value(output2), 0x123456789abcdef0ull);
    CHECK_EQ_U64(value(result2), 0);
}

static void check_inline_execution(bool fail_copy) {
    guest_addr entered = page + 4096, gate = entered + 8, output = entered + 16, result = entered + 24;
    blocked_buffer(page, entered, gate, output);
    gmem_write_u64(&rt.mem, result, UINT64_MAX);
    operation submit = {.cb = page, .result = result, .fail_copy = fail_copy}, wait = {0};
    pal_thread *st = NULL, *wt = NULL;
    bool smade = pal_thread_create(&st, submit_thread, &submit, "APR sync submit", 0) == PS5C_OK;
    bool reached = smade && until_value(entered, 1);
    bool wmade = pal_thread_create(&wt, wait_thread, &wait, "APR sync wait", 0) == PS5C_OK;
    bool started = wmade && wait_started(&wait);
    pal_sleep_ns(30000000);
    bool premature = atomic_load(&wait.returned);
    gmem_write_u64(&rt.mem, gate, 1);
    if (smade) pal_thread_join(st, NULL);
    if (wmade) pal_thread_join(wt, NULL);
    CHECK(reached && smade && wmade && started);
    CHECK(!fail_copy || atomic_load(&submit.copy_failed));
    CHECK(!premature);
    CHECK_EQ_U64(value(output), 0x123456789abcdef0ull);
    CHECK_EQ_U64(value(result), 0);
}

static void global_wait_includes_explicit_sync_execution(void) {
    check_inline_execution(false);
}

#ifdef PS5C_TEST_APR_WAIT_HANDSHAKE
static void global_wait_includes_records_allocation_failure(void) {
    check_inline_execution(true);
}

static void global_wait_excludes_submissions_after_its_snapshot(void) {
    hle_call c = {.rt = &rt, .mem = &rt.mem};
    guest_addr e1 = page + 4096, gate1 = e1 + 8, output1 = e1 + 16;
    guest_addr e2 = page + 8192, gate2 = e2 + 8, output2 = e2 + 16;
    blocked_buffer(page, e1, gate1, output1);
    blocked_buffer(page + 512, e2, gate2, output2);
    apr_submit_amm(&c, page, 96, 0, 0);
    bool reached1 = until_value(e1, 1);
    operation wait = {0};
    pal_thread *wt = NULL;
    bool made = pal_thread_create(&wt, wait_thread, &wait, "APR snapshot", 0) == PS5C_OK;
    bool started = made && wait_started(&wait) && atomic_load(&wait.registered);
    /* Wait has reached a blocking primitive after taking its snapshot. The
     * new job is on a later queue to expose the old per-queue drain barrier. */
    uint32_t later = apr_submit_amm(&c, page + 512, 96, 1, 0);
    bool reached2 = until_value(e2, 1);
    gmem_write_u64(&rt.mem, gate1, 1);
    uint64_t end = pal_time_ns() + 2000000000ull;
    while (!atomic_load(&wait.returned) && pal_time_ns() < end) pal_sleep_ns(1000000);
    bool returned_without_later = atomic_load(&wait.returned);
    uint64_t future_output = value(output2);
    gmem_write_u64(&rt.mem, gate2, 1);
    if (made) pal_thread_join(wt, NULL);
    apr_wait(later);
    CHECK(reached1 && reached2 && made && started);
    CHECK(returned_without_later && future_output == 0);
    CHECK_EQ_U64(value(output1), 0x123456789abcdef0ull);
}
#endif

int main(int argc, char **argv) {
    bool sync = argc == 2 && !strcmp(argv[1], "--sync");
    if (pal_init() != PS5C_OK || hle_registry_init() != PS5C_OK) return 2;
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    if (gmem_init(&rt.mem, &cfg) != PS5C_OK ||
        gmem_map(&rt.mem, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "APR pending", &page) != PS5C_OK) return 2;
    const test_case async_tests[] = {TEST_CASE(old_id_stays_pending_while_another_queue_advances),
                                   TEST_CASE(global_wait_includes_queue_full_inline_execution),
#ifdef PS5C_TEST_APR_WAIT_HANDSHAKE
                                   TEST_CASE(global_wait_includes_records_allocation_failure),
                                   TEST_CASE(global_wait_excludes_submissions_after_its_snapshot),
#endif
    };
    const test_case sync_tests[] = {TEST_CASE(global_wait_includes_explicit_sync_execution)};
    int result = sync ? test_run_all("apr_pending_sync", sync_tests, PS5C_ARRAY_COUNT(sync_tests))
                      : test_run_all("apr_pending", async_tests, PS5C_ARRAY_COUNT(async_tests));
    gmem_free(&rt.mem);
    hle_registry_free();
    pal_shutdown();
    return result;
}
