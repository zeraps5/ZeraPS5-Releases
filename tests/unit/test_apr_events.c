/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/runtime.h"
#include "hle/equeue.h"
#include "hle/hle.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include "test.h"

static ps5_runtime rt;
static guest_addr page;

static int32_t call(const char *library, const char *name, uint64_t a, uint64_t b,
                    uint64_t c, uint64_t d) {
    const hle_export *exp = hle_find(library, nid_hash_name(name));
    if (!exp || !exp->fn) return INT32_MIN;
    hle_args args = {.gpr = {a, b, c, d}};
    hle_call context = {.rt = &rt, .mem = &rt.mem, .a = &args};
    exp->fn(&context);
    return (int32_t)args.ret[0];
}
#define K(name, a, b, c) call("libkernel", "sceKernel" name, a, b, c, 0)
#define A(name, a, b, c, d) call("libSceAmpr", "sceAmpr" name, a, b, c, d)

static void completion_uses_ampr_filter(void) {
    uint64_t queue = equeue_create("APR ABI");
    CHECK(queue);
    const uint64_t udata = 0x900123450ull, first = 0xc000021000ull, second = 0xc000022000ull;
    CHECK(K("AddAmprEvent", queue, 1, udata) == 0);
    CHECK(A("CommandBufferConstructor", page, 0, 0, 0) == 0);
    CHECK(A("CommandBufferSetBuffer", page, page + 128, 256, 0) == 0);
    CHECK(A("CommandBufferWriteKernelEventQueueOnCompletion", page, queue, 1, first) == 0);
    CHECK(A("CommandBufferWriteKernelEventQueueOnCompletion", page, queue, 1, second) == 0);
    CHECK(K("AprSubmitCommandBuffer", page, 0, 0) == 0);
    CHECK(K("AprWaitCommandBuffer", 0, 0, 0) == 0);
    guest_kevent event = {0};
    CHECK(equeue_wait(queue, &event, 1, 0) == 1);
    /* Literal guest ABI: a title dispatches completions on -25, not USER -11. */
    CHECK(event.filter == -25 && event.ident == 1);
    CHECK(event.udata == udata && (uint64_t)event.data == first);
    CHECK(equeue_wait(queue, &event, 1, 0) == 1);
    CHECK(event.filter == -25 && (uint64_t)event.data == second);
    CHECK(equeue_wait(queue, &event, 1, 0) == 0);
    CHECK(K("DeleteAmprEvent", queue, 1, 0) == 0);
    CHECK(equeue_destroy(queue));
}

static void native_completion_keeps_user_registration(void) {
    uint64_t queue = equeue_create("APR and USER");
    CHECK(queue && queue < (1ull << 48));
    const uint64_t udata = 0x900001000ull, tag = 0xc000012340ull;
    CHECK(K("AddUserEvent", queue, 1, 0) == 0);
    CHECK(K("AddAmprEvent", queue, 1, udata) == 0);
    CHECK(A("CommandBufferConstructor", page, 0, 0, 0) == 0);
    CHECK(A("CommandBufferSetBuffer", page, page + 128, 256, 0) == 0);
    /* Console-format completion record: 0x08, five dwords. */
    uint32_t record[5] = {0x408u | (uint32_t)(queue >> 32) << 16,
                          (uint32_t)queue, 1, (uint32_t)tag, (uint32_t)(tag >> 32)};
    uint32_t bytes = sizeof(record);
    CHECK(gmem_write(&rt.mem, page + 128, record, bytes));
    CHECK(gmem_write(&rt.mem, page + 4, &bytes, 4));
    CHECK(K("AprSubmitCommandBuffer", page, 0, 0) == 0);
    CHECK(K("AprWaitCommandBuffer", 0, 0, 0) == 0);
    guest_kevent event = {0};
    CHECK(equeue_wait(queue, &event, 1, 0) == 1);
    CHECK(event.filter == -25 && event.ident == 1);
    CHECK(event.udata == udata && (uint64_t)event.data == tag);
    CHECK(K("DeleteAmprEvent", queue, 1, 0) == 0);
    CHECK(K("TriggerUserEvent", queue, 1, 0x1234) == 0);
    CHECK(equeue_wait(queue, &event, 1, 0) == 1);
    CHECK(event.filter == -11 && event.ident == 1 && event.udata == 0x1234);
    CHECK(equeue_destroy(queue));
}

int main(void) {
    if (pal_init() != PS5C_OK || hle_registry_init() != PS5C_OK) return 2;
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    if (gmem_init(&rt.mem, &cfg) != PS5C_OK ||
        gmem_map(&rt.mem, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "APR test", &page) != PS5C_OK) return 2;
    const test_case tests[] = {TEST_CASE(completion_uses_ampr_filter),
                               TEST_CASE(native_completion_keeps_user_registration)};
    int result = test_run_all("apr_events", tests, PS5C_ARRAY_COUNT(tests));
    gmem_free(&rt.mem);
    hle_registry_free();
    pal_shutdown();
    return result;
}
