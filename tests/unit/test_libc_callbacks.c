/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/runtime.h"
#include "hle/hle.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include "support/libc_jump_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "libc callbacks line %d: %s\n", __LINE__, #x);                         \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static ps5_runtime *rt;
static guest_addr page;
static uint64_t call(const char *lib, const char *name, uint64_t a, uint64_t b, uint64_t d,
                     uint64_t e, uint64_t f) {
    const hle_export *exp = hle_find(lib, nid_hash_name(name));
    CHECK(exp && exp->fn);
    hle_args args = {.gpr = {a, b, d, e, f}};
    hle_call c = {.rt = rt, .mem = &rt->mem, .a = &args, .thread = runtime_current_thread(rt)};
    exp->fn(&c);
    return args.ret[0];
}
static void put(guest_addr a, const void *p, size_t n) {
    CHECK(gmem_write(&rt->mem, a, p, n));
}
static uint64_t u64(guest_addr a) {
    uint64_t v;
    CHECK(gmem_read_u64(&rt->mem, a, &v));
    return v;
}
static void run(guest_addr code, guest_addr arg) {
    pal_icache_flush(gmem_host(&rt->mem, page, GUEST_PAGE_SIZE), GUEST_PAGE_SIZE);
    guest_thread *t;
    CHECK(runtime_start_thread(rt, code, arg, 0, 0x200000, false, "libc callbacks", &t) == 0);
    uint64_t handle = (uint64_t)(uintptr_t)t, result;
    runtime_release_thread(rt, t);
    CHECK(runtime_join_thread(rt, handle, &result) == 0 && result == 0);
}
static uint64_t guest_driver(void) {
    int values[] = {9, -2, 9, 0, INT32_MIN, INT32_MAX, 7};
    put(page + 0x900, values, sizeof(values));
    call("libSceLibcInternal", "qsort", page + 0x900, 7, 4, page + 0x300, 0);
    const int expected[] = {INT32_MIN, -2, 0, 7, 9, 9, INT32_MAX};
    int sorted[7];
    CHECK(gmem_read(&rt->mem, page + 0x900, sorted, sizeof(sorted)) &&
          !memcmp(sorted, expected, sizeof(expected)));
    int key = 7;
    put(page + 0x940, &key, 4);
    CHECK(call("libSceLibcInternal", "bsearch", page + 0x940, page + 0x900, 7, 4, page + 0x300) ==
          page + 0x90c);
    uint64_t zero = 0;
    put(page + 0x980, &zero, 8);
    CHECK(call("libSceLibcInternal", "__cxa_atexit", page + 0x340, page + 0x980, 11, 0, 0) == 0);
    CHECK(call("libSceLibcInternal", "__cxa_atexit", page + 0x340, page + 0x980, 22, 0, 0) == 0);
    call("libSceLibcInternal", "__cxa_finalize", 11, 0, 0, 0, 0);
    CHECK(u64(page + 0x980) == 1);
    call("libSceLibcInternal", "__cxa_finalize", 11, 0, 0, 0, 0);
    CHECK(u64(page + 0x980) == 1);
    call("libSceLibcInternal", "__cxa_finalize", 0, 0, 0, 0, 0);
    CHECK(u64(page + 0x980) == 2);
    return 0;
}
int main(void) {
    CHECK(pal_init() == PS5C_OK);
    module_set ms;
    module_set_init(&ms);
    rt = calloc(1, sizeof(*rt));
    CHECK(rt);
    runtime_options opts = {0};
    CHECK(runtime_init(rt, &ms, &opts) == PS5C_OK);
    CHECK(gmem_map(&rt->mem, 0, GUEST_PAGE_SIZE, GPROT_RW | GPROT_EXEC, 0, "libc ABI fixtures",
                   &page) == PS5C_OK);
    put(page, jump_fixture, sizeof(jump_fixture));
    guest_addr maskfn = runtime_resolve_symbol(rt, "_sigprocmask");
    CHECK(maskfn);
    const int returns[] = {0, 37, -5};
    for (unsigned plain = 0; plain < 2; plain++)
        for (unsigned i = 0; i < 3; i++) {
            const hle_export *save =
                hle_find("libSceLibcInternal", nid_hash_name(plain ? "_setjmp" : "setjmp"));
            const hle_export *load =
                hle_find("libSceLibcInternal", nid_hash_name(plain ? "_longjmp" : "longjmp"));
            CHECK(save && load && save->kind == HLE_NATIVE);
            save->data_init(rt, gmem_host(&rt->mem, page + 0x200, 16), 16);
            load->data_init(rt, gmem_host(&rt->mem, page + 0x220, 16), 16);
            uint32_t original[4] = {0x123, 0, 0, 0}, changed[4] = {0x456, 0, 0, 0};
            put(page + 0x800, original, 16);
            put(page + 0x820, changed, 16);
            CHECK(call("libkernel", "_sigprocmask", 3, page + 0x800, 0, 0, 0) == 0);
            uint64_t context[16] = {page + 0x200, page + 0x220, page + 0x400,
                                    (uint64_t)(int64_t)returns[i]};
            context[9] = maskfn;
            context[10] = page + 0x820;
            context[12] = 0x5f81;
            context[15] = (uint64_t)0x0b7f << 32;
            put(page + 0x600, context, sizeof(context));
            run(page, page + 0x600);
            CHECK(u64(page + 0x620) == (uint64_t)(int64_t)(returns[i] ? returns[i] : 1));
            CHECK(u64(page + 0x628) == 0x1122 && u64(page + 0x630) == 0x3344 &&
                  u64(page + 0x638) == 0x5566 && u64(page + 0x640) == 0x7788);
            CHECK(u64(page + 0x668) == u64(page + 0x670));
            uint32_t saved_mxcsr, restored_mxcsr;
            uint16_t saved_cw, restored_cw;
            CHECK(gmem_read(&rt->mem, page + 0x444, &saved_mxcsr, 4) &&
                  gmem_read(&rt->mem, page + 0x658, &restored_mxcsr, 4));
            CHECK((saved_mxcsr & ~63u) == (restored_mxcsr & ~63u) && (restored_mxcsr & 1));
            CHECK(gmem_read(&rt->mem, page + 0x440, &saved_cw, 2) &&
                  gmem_read(&rt->mem, page + 0x678, &restored_cw, 2) && saved_cw == restored_cw);
            CHECK(call("libkernel", "_sigprocmask", 1, 0, page + 0x840, 0, 0) == 0);
            uint32_t mask[4];
            CHECK(gmem_read(&rt->mem, page + 0x840, mask, 16) &&
                  mask[0] == (plain ? changed[0] : original[0]));
        }
    /* Signed comparator avoids overflow for INT_MIN/INT_MAX. */
    const uint8_t comparator[] = {0x8b, 0x07, 0x3b, 0x06, 0x0f, 0x9f, 0xc0, 0x0f, 0x9c, 0xc2,
                                  0x0f, 0xb6, 0xc0, 0x0f, 0xb6, 0xd2, 0x29, 0xd0, 0xc3};
    put(page + 0x300, comparator, sizeof(comparator));
    const uint8_t increment[] = {0x48, 0xff, 0x07, 0xc3};
    put(page + 0x340, increment, sizeof(increment));
    uint8_t adapter[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0xb8, 0,    0,    0,    0,   0,
                         0,    0,    0,    0xff, 0xd0, 0x48, 0x83, 0xc4, 0x28, 0xc3};
    uint64_t driver = (uint64_t)(uintptr_t)guest_driver;
    memcpy(adapter + 6, &driver, 8);
    put(page + 0x100, adapter, sizeof(adapter));
    run(page + 0x100, 0);
    uint32_t keyboard = hle_library_handle("libSceKeyboard");
    CHECK(keyboard && !hle_library_handle("libMissing"));
    CHECK(runtime_resolve_symbol_in(rt, keyboard, "sceKeyboardInit") != 0 &&
          runtime_resolve_symbol_in(rt, keyboard, "strlen") == 0);
    runtime_free(rt);
    free(rt);
    module_set_free(&ms);
    pal_shutdown();
    puts("libc native ABI: jumps, integer returns, saved registers, FP controls, signal masks, "
         "comparator callbacks, DSO destructors and scoped modules verified");
    return 0;
}
