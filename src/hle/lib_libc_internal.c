/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceLibcInternal: the firmware's private libc, used by system libraries
 * (the game's own libc.prx runs LLE). Console output goes to the log. */
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/guest_printf.h"
#include "hle/hle.h"

void hle_register_table(const hle_export *table, size_t count);

#define LIB "libSceLibcInternal"
#define OUT_MAX 4096

static void log_output(const char *text) {
    size_t n = strlen(text);
    while (n && (text[n - 1] == '\n' || text[n - 1] == '\r')) n--;
    if (n) LOG_I(KERNEL, "[guest] %.*s", (int)n, text);
}

static void puts_(hle_call *c) {
    char text[OUT_MAX];
    hle_read_str(c, hle_arg_ptr(c, 0), text, sizeof(text));
    log_output(text);
    hle_ret_s32(c, 0);
}

/* Formats with the format string at argument `fmt_index`. */
static void print_to_log(hle_call *c, uint32_t fmt_index, guest_addr va_list) {
    guest_va va;
    if (va_list) guest_va_from_list(&va, c, va_list);
    else guest_va_from_call(&va, c, fmt_index + 1);
    char text[OUT_MAX];
    size_t n = guest_format(c, hle_arg_ptr(c, fmt_index), &va, text, sizeof(text));
    log_output(text);
    hle_ret_s32(c, (int32_t)n);
}

/* FILE objects belong to the HLE'd libc: every stream goes to the log. */
static void printf_(hle_call *c) { print_to_log(c, 0, 0); }
static void vprintf_(hle_call *c) { print_to_log(c, 0, hle_arg_ptr(c, 1)); }
static void print_to_stream(hle_call *c, guest_addr list) {
    guest_va va;
    if(list)guest_va_from_list(&va,c,list);else guest_va_from_call(&va,c,2);
    char text[OUT_MAX];size_t n=guest_format(c,hle_arg_ptr(c,1),&va,text,sizeof(text));
    size_t bytes=n<sizeof(text)?n:sizeof(text)-1;
    int64_t written=hle_libc_write_text(c,hle_arg_ptr(c,0),text,bytes);
    hle_ret_s32(c,written<0?-1:(int32_t)written);
}
static void fprintf_(hle_call *c) { print_to_stream(c,0); }
static void vfprintf_(hle_call *c) { print_to_stream(c,hle_arg_ptr(c,2)); }

/* Formats into guest memory: dst, size (UINT64_MAX = unbounded), fmt at fmt_index. */
static void print_to_guest(hle_call *c, guest_addr dst, uint64_t size, uint32_t fmt_index, guest_addr va_list) {
    guest_va va;
    if (va_list) guest_va_from_list(&va, c, va_list);
    else guest_va_from_call(&va, c, fmt_index + 1);
    char text[OUT_MAX];
    size_t n = guest_format(c, hle_arg_ptr(c, fmt_index), &va, text, sizeof(text));
    if (size) {
        size_t len = n < sizeof(text) ? n : sizeof(text) - 1;
        if (size != UINT64_MAX && len >= size) len = (size_t)size - 1;
        char *out = (char *)hle_mem(c, dst, len + 1);
        if (out) {
            memcpy(out, text, len);
            out[len] = '\0';
        }
    }
    hle_ret_s32(c, (int32_t)n);
}

static void sprintf_(hle_call *c) { print_to_guest(c, hle_arg_ptr(c, 0), UINT64_MAX, 1, 0); }
static void vsprintf_(hle_call *c) { print_to_guest(c, hle_arg_ptr(c, 0), UINT64_MAX, 1, hle_arg_ptr(c, 2)); }
static void snprintf_(hle_call *c) { print_to_guest(c, hle_arg_ptr(c, 0), hle_arg(c, 1), 2, 0); }
static void vsnprintf_(hle_call *c) { print_to_guest(c, hle_arg_ptr(c, 0), hle_arg(c, 1), 2, hle_arg_ptr(c, 3)); }

/* thread_local destructors registered through the firmware libc:
 * int _sceLibcInternalThreadAtexit(void (*dtor)(void *), void *obj, void *dso) adds one for the
 * calling thread (the game's libc forwards __cxa_thread_atexit here), and
 * void _sceLibcInternalThreadDtors(void), called by the exiting thread, runs its own in
 * reverse order. */
typedef struct thread_dtor {
    uint32_t thread;
    guest_addr fn, obj;
} thread_dtor;
static thread_dtor *g_dtors;
static uint32_t g_dtor_count, g_dtor_cap;
static volatile int g_dtor_lock;
static void dtor_lock(void) { while (__atomic_exchange_n(&g_dtor_lock, 1, __ATOMIC_ACQUIRE)) {} }
static void dtor_unlock(void) { __atomic_store_n(&g_dtor_lock, 0, __ATOMIC_RELEASE); }

static void thread_atexit(hle_call *c) {
    guest_addr fn = hle_arg_ptr(c, 0);
    if (!fn || !c->thread) {
        hle_ret_s32(c, fn ? 0 : -1);
        return;
    }
    dtor_lock();
    if (g_dtor_count == g_dtor_cap) {
        uint32_t cap = g_dtor_cap ? g_dtor_cap * 2 : 64;
        thread_dtor *grown = (thread_dtor *)realloc(g_dtors, cap * sizeof(*grown));
        if (grown) {
            g_dtors = grown;
            g_dtor_cap = cap;
        }
    }
    bool ok = g_dtor_count < g_dtor_cap;
    if (ok) g_dtors[g_dtor_count++] = (thread_dtor){c->thread->id, fn, hle_arg_ptr(c, 1)};
    dtor_unlock();
    hle_ret_s32(c, ok ? 0 : -1);
}

static void thread_dtors(hle_call *c) {
    for (;;) {
        thread_dtor d = {0};
        dtor_lock();
        for (uint32_t i = g_dtor_count; c->thread && i-- > 0;) {
            if (g_dtors[i].thread != c->thread->id) continue;
            d = g_dtors[i];
            memmove(&g_dtors[i], &g_dtors[i + 1], (g_dtor_count - i - 1) * sizeof(*g_dtors));
            g_dtor_count--;
            break;
        }
        dtor_unlock();
        if (!d.fn) break;
        uint64_t rax;
        runtime_call_nested(c->rt, d.fn, d.obj, 0, 0, &rax); /* may register more */
    }
    hle_ret_u64(c, 0);
}

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(LIB, "puts", puts_),
    HLE_FUNC_ENTRY(LIB, "printf", printf_),
    HLE_FUNC_ENTRY(LIB, "vprintf", vprintf_),
    HLE_FUNC_ENTRY(LIB, "fprintf", fprintf_),
    HLE_FUNC_ENTRY(LIB, "vfprintf", vfprintf_),
    HLE_FUNC_ENTRY(LIB, "sprintf", sprintf_),
    HLE_FUNC_ENTRY(LIB, "vsprintf", vsprintf_),
    HLE_FUNC_ENTRY(LIB, "snprintf", snprintf_),
    HLE_FUNC_ENTRY(LIB, "vsnprintf", vsnprintf_),
    HLE_FUNC_ENTRY("libSceLibcInternalExt", "_sceLibcInternalThreadDtors", thread_dtors),
    HLE_FUNC_ENTRY("libSceLibcInternalExt", "_sceLibcInternalThreadAtexit", thread_atexit),
};

void hle_register_libc_internal(void) {
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
    hle_register_libc_memory();
    hle_register_libc_process();
    hle_register_libc_jumps();
}
