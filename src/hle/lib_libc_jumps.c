/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native System V register bridges for the FreeBSD amd64 jmp_buf ABI:
 * rip,rbx,rsp,rbp,r12,r13,r14,r15, x87 control (64), MXCSR (68),
 * sigset_t (72). This is guest code: host setjmp cannot unwind guest stacks.
 * No host frames remain on the stack when longjmp resumes its caller. */
#include "core/runtime.h"
#include "hle/hle.h"
#include <string.h>
void hle_register_table(const hle_export *, size_t);
static void emit(uint8_t **p, const void *data, size_t n) {
    memcpy(*p, data, n);
    *p += n;
}
#define BYTES(...)                                                                                 \
    do {                                                                                           \
        const uint8_t b_[] = {__VA_ARGS__};                                                        \
        emit(&p, b_, sizeof(b_));                                                                  \
    } while (0)
static void mask_call(uint8_t **out, guest_addr fn) {
    uint8_t *p = *out;
    BYTES(0x49, 0xbb);
    emit(&p, &fn, 8);        /* mov r11,fn */
    BYTES(0x41, 0xff, 0xd3); /* call r11 */
    *out = p;
}
static void install(ps5_runtime *rt, void *slot, size_t size, bool restore, bool mask) {
    if (!slot || size < 13)
        return;
    memset(slot, 0xcc, size);
    guest_addr sig = mask ? runtime_resolve_symbol(rt, "_sigprocmask") : 0;
    if (mask && !sig)
        return;
    guest_addr addr;
    if (gmem_map(&rt->mem, 0, GUEST_PAGE_SIZE, GPROT_RW | GPROT_EXEC, 0, "libc jump bridge",
                 &addr) != PS5C_OK)
        return;
    uint8_t *base = gmem_host(&rt->mem, addr, 512), *p = base;
    if (!restore) {
        BYTES(0x48, 0x8b, 0x04, 0x24, 0x48, 0x89, 0x07); /* [rdi]=return address */
        BYTES(0x48, 0x89, 0x5f, 0x08, 0x48, 0x89, 0x67, 0x10, 0x48, 0x89, 0x6f, 0x18);
        BYTES(0x4c, 0x89, 0x67, 0x20, 0x4c, 0x89, 0x6f, 0x28, 0x4c, 0x89, 0x77, 0x30, 0x4c, 0x89,
              0x7f, 0x38);
        BYTES(0xd9, 0x7f, 0x40, 0x0f, 0xae, 0x5f, 0x44); /* fnstcw,stmxcsr */
        if (mask) {
            BYTES(0x48, 0x8d, 0x57, 0x48, 0x31, 0xf6, 0xbf, 1, 0, 0, 0, 0x48, 0x83, 0xec, 8);
            mask_call(&p, sig);
            BYTES(0x48, 0x83, 0xc4, 8);
        }
        BYTES(0x31, 0xc0, 0xc3);
    } else {
        if (mask) {
            BYTES(0x57, 0x56, 0x48, 0x83, 0xec, 8, 0x48, 0x8d, 0x77, 0x48, 0x31, 0xd2, 0xbf, 3, 0,
                  0, 0);
            mask_call(&p, sig);
            BYTES(0x48, 0x83, 0xc4, 8, 0x5e, 0x5f);
        }
        /* Preserve current SSE exception flags, restoring the saved control
         * bits. Restore x87 control without restoring accrued exceptions. */
        BYTES(0x0f, 0xae, 0x5c, 0x24, 0xfc, 0x8b, 0x47, 0x44, 0x83, 0xe0, 0xc0);
        BYTES(0x8b, 0x54, 0x24, 0xfc, 0x83, 0xe2, 0x3f, 0x09, 0xd0, 0x89, 0x44, 0x24, 0xfc, 0x0f,
              0xae, 0x54, 0x24, 0xfc);
        BYTES(0xd9, 0x6f, 0x40, 0x48, 0x63, 0xc6); /* fldcw; movsxd rax,esi */
        BYTES(0x85, 0xc0, 0x75, 0x02, 0xff, 0xc0); /* longjmp(env,0) returns 1 */
        BYTES(0x48, 0x8b, 0x0f, 0x48, 0x8b, 0x5f, 8, 0x48, 0x8b, 0x6f, 0x18);
        BYTES(0x4c, 0x8b, 0x67, 0x20, 0x4c, 0x8b, 0x6f, 0x28, 0x4c, 0x8b, 0x77, 0x30, 0x4c, 0x8b,
              0x7f, 0x38);
        BYTES(0x48, 0x8b, 0x67, 0x10, 0x48, 0x89, 0x0c, 0x24, 0xc3);
    }
    pal_icache_flush(base, (uint64_t)(p - base));
    gmem_protect(&rt->mem, addr, GUEST_PAGE_SIZE, GPROT_READ | GPROT_EXEC);
    p = slot;
    BYTES(0x49, 0xbb);
    emit(&p, &addr, 8);
    BYTES(0x41, 0xff, 0xe3); /* jmp r11 */
    pal_icache_flush(slot, size);
}
static void save(ps5_runtime *rt, void *slot, size_t n) {
    install(rt, slot, n, false, true);
}
static void load(ps5_runtime *rt, void *slot, size_t n) {
    install(rt, slot, n, true, true);
}
static void save_nomask(ps5_runtime *rt, void *slot, size_t n) {
    install(rt, slot, n, false, false);
}
static void load_nomask(ps5_runtime *rt, void *slot, size_t n) {
    install(rt, slot, n, true, false);
}
static const hle_export exports[] = {
    HLE_NATIVE_ENTRY("libSceLibcInternal", "setjmp", save),
    HLE_NATIVE_ENTRY("libSceLibcInternal", "longjmp", load),
    HLE_NATIVE_ENTRY("libSceLibcInternal", "_setjmp", save_nomask),
    HLE_NATIVE_ENTRY("libSceLibcInternal", "_longjmp", load_nomask),
};
void hle_register_libc_jumps(void) {
    hle_register_table(exports, PS5C_ARRAY_COUNT(exports));
}
