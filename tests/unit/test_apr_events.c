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

static void completion_preserves_signed_ids(void) {
    static const uint32_t ids[] = {0, 0x7fffffffu, 0x80000000u, 0xaa33aa33u, 0xffffffffu};
    bool correct = true;
    for (unsigned system = 0; system < 2; system++) {
        for (unsigned format = 0; format < 3; format++) {
            for (unsigned i = 0; i < PS5C_ARRAY_COUNT(ids); i++) {
                uint32_t id = ids[i];
                /* int32_t ABI, independently expressed as the expected 64-bit key. */
                uint64_t ident = id < 0x80000000u ? id : 0xffffffff00000000ull | id;
                uint64_t queue = equeue_create("APR signed id");
                CHECK(queue && queue < (1ull << 48));
                const uint64_t udata = 0x900123450ull, first = 0xc000021000ull, second = 0xc000022000ull;
                const char *add = system ? "sceKernelAddAmprSystemEvent" : "sceKernelAddAmprEvent";
                const char *del = system ? "sceKernelDeleteAmprSystemEvent" : "sceKernelDeleteAmprEvent";
                CHECK(call("libkernel", add, queue, id, udata, 0) == 0);
                CHECK(K("AddUserEvent", queue, id, 0) == 0);
                CHECK(A("CommandBufferConstructor", page, 0, 0, 0) == 0);
                CHECK(A("CommandBufferSetBuffer", page, page + 128, 256, 0) == 0);
                if (!format) {
                    CHECK(A("CommandBufferWriteKernelEventQueueOnCompletion", page, queue, id, first) == 0);
                    CHECK(A("CommandBufferWriteKernelEventQueueOnCompletion", page, queue, id, second) == 0);
                } else {
                    uint32_t opcode = (format == 1 ? 0x408u : 0x478u) | (uint32_t)(queue >> 32) << 16;
                    uint32_t records[10] = {opcode, (uint32_t)queue, id, (uint32_t)first, (uint32_t)(first >> 32),
                                           opcode, (uint32_t)queue, id, (uint32_t)second, (uint32_t)(second >> 32)};
                    uint32_t bytes = sizeof(records);
                    CHECK(gmem_write(&rt.mem, page + 128, records, bytes));
                    CHECK(gmem_write(&rt.mem, page + 4, &bytes, 4));
                }
                guest_kevent event = {0};
                CHECK(equeue_wait(queue, &event, 1, 0) == 0);
                CHECK(K("AprSubmitCommandBuffer", page, 0, 0) == 0);
                CHECK(K("AprWaitCommandBuffer", 0, 0, 0) == 0);
                bool delivered = equeue_wait(queue, &event, 1, 0) == 1;
                bool first_ok = delivered && event.filter == -25 && event.ident == ident &&
                                event.udata == udata && (uint64_t)event.data == first;
                delivered = equeue_wait(queue, &event, 1, 0) == 1;
                bool second_ok = delivered && event.filter == -25 && event.ident == ident &&
                                 event.udata == udata && (uint64_t)event.data == second;
                fprintf(stderr, "APR signed id %08x format %u system %u: %s\n", id, format, system,
                        first_ok && second_ok ? "delivered" : "lost or mismatched");
                correct &= first_ok && second_ok;
                CHECK(equeue_wait(queue, &event, 1, 0) == 0);
                CHECK(call("libkernel", del, queue, id, 0, 0) == 0);
                CHECK(!equeue_push(queue, ident, EVFILT_AMPR_, 0));
                CHECK(K("TriggerUserEvent", queue, id, 0x1234) == 0);
                CHECK(equeue_wait(queue, &event, 1, 0) == 1);
                CHECK(event.filter == -11 && event.ident == ident && event.udata == 0x1234);
                /* Generic queues retain distinct full-width unsigned keys. */
                if (id >= 0x80000000u) {
                    CHECK(equeue_add(queue, id, EVFILT_USER_, udata));
                    CHECK(equeue_push(queue, id, EVFILT_USER_, 0x5678));
                    CHECK(equeue_wait(queue, &event, 1, 0) == 1);
                    CHECK(event.filter == -11 && event.ident == id && event.udata == udata && event.data == 0x5678);
                }
                CHECK(equeue_destroy(queue));
            }
        }
    }
    CHECK(correct);
}

int main(void) {
    if (pal_init() != PS5C_OK || hle_registry_init() != PS5C_OK) return 2;
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    if (gmem_init(&rt.mem, &cfg) != PS5C_OK ||
        gmem_map(&rt.mem, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "APR test", &page) != PS5C_OK) return 2;
    const test_case tests[] = {TEST_CASE(completion_uses_ampr_filter),
                               TEST_CASE(native_completion_keeps_user_registration),
                               TEST_CASE(completion_preserves_signed_ids)};
    int result = test_run_all("apr_events", tests, PS5C_ARRAY_COUNT(tests));
    gmem_free(&rt.mem);
    hle_registry_free();
    pal_shutdown();
    return result;
}
