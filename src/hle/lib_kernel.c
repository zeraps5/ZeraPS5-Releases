/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libkernel: process, thread, time and TLS services. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#include "base/log.h"
#include "core/runtime.h"
#include "core/settings.h"
#include "hle/hle.h"
#include "hle/kernel_objects.h"
#include "pal/pal.h"

#define LIB "libkernel"

void hle_register_table(const hle_export *table, size_t count);

static uint64_t g_process_start_ns;

static uint64_t return_address(hle_call *c) {
    uint64_t ret = 0;
    gmem_read_u64(c->mem, c->a->stack - 8, &ret);
    return ret;
}

/* ---- Stack protector ---- */

static void stack_chk_guard_init(struct ps5_runtime *rt, void *slot, size_t size) {
    PS5C_UNUSED(rt);
    PS5C_UNUSED(size);
    uint64_t v = pal_time_ns() * 0x9E3779B97F4A7C15ull;
    v &= ~0xFFull; /* zero low byte, like real canaries: stops string-based overflows */
    memcpy(slot, &v, sizeof(v));
}

/* Return addresses on the guest stack above the HLE call: values in a module
 * that follow a call instruction (where the corrupted frame was) */
static void log_stack_calls(hle_call *c) {
    int shown = 0;
    for (guest_addr a = c->a->stack - 8; a < c->a->stack + 0x1000 && shown < 12; a += 8) {
        uint64_t v = 0;
        uint8_t code[6];
        if (!gmem_read_u64(c->mem, a, &v)) break;
        const loaded_module *m = v > 6 ? runtime_find_module(c->rt, v) : NULL;
        if (!m || !gmem_read(c->mem, v - 6, code, sizeof(code))) continue;
        bool call = code[1] == 0xE8 || (code[0] == 0xFF && (code[1] & 0x38) == 0x10) ||
                    (code[3] == 0xFF && (code[4] & 0x38) == 0x10) || (code[4] == 0xFF && (code[5] & 0x38) == 0x10);
        if (!call) continue;
        LOG_E(KERNEL, "  [rsp+0x%llx] %s+0x%llx", (unsigned long long)(a - (c->a->stack - 8)), m->name,
              (unsigned long long)(v - m->base));
        shown++;
    }
}

static void stack_chk_fail(hle_call *c) {
    const loaded_module *m = runtime_find_module(c->rt, return_address(c));
    LOG_E(KERNEL, "__stack_chk_fail: stack corruption detected in %s (return address 0x%llx)",
          m ? m->name : "?", (unsigned long long)return_address(c));
    runtime_log_backtrace(c->rt);
    log_stack_calls(c);
    runtime_exit_thread(c->rt, RUNTIME_EXIT_STACK_SMASH);
}

/* ---- TLS ---- */

/* void *__tls_get_addr(tls_index *ti), tls_index = { uint64 module; uint64 offset; } */
static void tls_get_addr(hle_call *c) {
    guest_addr ti = hle_arg_ptr(c, 0);
    uint64_t module = 0, offset = 0;
    if (!c->thread || !gmem_read_u64(c->mem, ti, &module) || !gmem_read_u64(c->mem, ti + 8, &offset)) {
        LOG_E(KERNEL, "__tls_get_addr: bad tls_index 0x%llx", (unsigned long long)ti);
        hle_ret_u64(c, 0);
        return;
    }
    uint64_t count = 0, block = 0;
    gmem_read_u64(c->mem, c->thread->dtv + 8, &count);
    if (module == 0 || module > count || !gmem_read_u64(c->mem, c->thread->dtv + (1 + module) * 8, &block)) {
        LOG_E(KERNEL, "__tls_get_addr: TLS module %llu not loaded", (unsigned long long)module);
        hle_ret_u64(c, 0);
        return;
    }
    hle_ret_u64(c, block + offset);
}

/* ---- Time ---- */

static uint64_t process_time_us(void) {
    if (!g_process_start_ns) g_process_start_ns = pal_time_ns();
    return (pal_time_ns() - g_process_start_ns) / 1000u;
}

static void get_process_time(hle_call *c) { hle_ret_u64(c, process_time_us()); }

/* The counter runs at 1 GHz: nanoseconds since process start. */
static void get_process_time_counter(hle_call *c) {
    if (!g_process_start_ns) g_process_start_ns = pal_time_ns();
    hle_ret_u64(c, pal_time_ns() - g_process_start_ns);
}

static void get_process_time_counter_frequency(hle_call *c) { hle_ret_u64(c, 1000000000ull); }

/* The TSC: the host's, since guest code may also read it with rdtsc; its
 * rate is measured once against the host clock (Astro Bot divides by it) */
static void read_tsc(hle_call *c) { hle_ret_u64(c, __rdtsc()); }

static void get_tsc_frequency(hle_call *c) {
    static uint64_t freq;
    if (!freq) {
        uint64_t t0 = pal_time_ns(), c0 = __rdtsc();
        while (pal_time_ns() - t0 < 20000000ull) {
        }
        uint64_t t1 = pal_time_ns(), c1 = __rdtsc();
        freq = (c1 - c0) * 1000000000ull / (t1 - t0);
        freq = (freq + 500000) / 1000000 * 1000000; /* whole MHz */
        LOG_I(KERNEL, "TSC frequency: %llu Hz", (unsigned long long)freq);
    }
    hle_ret_u64(c, freq);
}

/* FreeBSD clock ids */
enum {
    CLK_REALTIME = 0, CLK_VIRTUAL = 1, CLK_PROF = 2, CLK_MONOTONIC = 4, CLK_UPTIME = 5,
    CLK_UPTIME_PRECISE = 7, CLK_UPTIME_FAST = 8, CLK_REALTIME_PRECISE = 9, CLK_REALTIME_FAST = 10,
    CLK_MONOTONIC_PRECISE = 11, CLK_MONOTONIC_FAST = 12, CLK_SECOND = 13, CLK_THREAD_CPUTIME = 14,
    CLK_PROCESS_CPUTIME = 15
};

bool kernel_clock_now(int32_t id, int64_t *sec, int64_t *nsec) {
    switch (id) {
    case CLK_REALTIME:
    case CLK_REALTIME_PRECISE:
    case CLK_REALTIME_FAST:
    case CLK_SECOND: {
        struct timespec ts;
        timespec_get(&ts, TIME_UTC);
        *sec = (int64_t)ts.tv_sec;
        *nsec = id == CLK_SECOND ? 0 : (int64_t)ts.tv_nsec;
        return true;
    }
    case CLK_MONOTONIC:
    case CLK_MONOTONIC_PRECISE:
    case CLK_MONOTONIC_FAST:
    case CLK_UPTIME:
    case CLK_UPTIME_PRECISE:
    case CLK_UPTIME_FAST:
    case CLK_VIRTUAL:
    case CLK_PROF:
    case CLK_THREAD_CPUTIME:
    case CLK_PROCESS_CPUTIME: {
        /* CPU-time clocks approximated by process uptime */
        uint64_t ns = pal_time_ns() - g_process_start_ns;
        *sec = (int64_t)(ns / 1000000000u);
        *nsec = (int64_t)(ns % 1000000000u);
        return true;
    }
    default: return false;
    }
}

/* int clock_gettime(clockid_t id, struct timespec *ts): 0 or an errno value */
static int32_t do_clock_gettime(hle_call *c) {
    int32_t id = hle_arg_s32(c, 0);
    int64_t *ts = (int64_t *)hle_mem(c, hle_arg_ptr(c, 1), 16);
    int64_t sec, nsec;
    if (!ts || !kernel_clock_now(id, &sec, &nsec)) {
        if (ts) LOG_W(KERNEL, "clock_gettime: unknown clock %d", id);
        return 22; /* EINVAL */
    }
    ts[0] = sec;
    ts[1] = nsec;
    return 0;
}

static void clock_gettime_(hle_call *c) { hle_ret_s32(c, do_clock_gettime(c) ? -1 : 0); }
static void kernel_clock_gettime(hle_call *c) {
    int32_t e = do_clock_gettime(c);
    hle_ret_s32(c, e ? (int32_t)(0x80020000u | (uint32_t)e) : 0);
}

static void clock_getres_(hle_call *c) {
    int64_t *ts = (int64_t *)hle_mem(c, hle_arg_ptr(c, 1), 16);
    if (ts) {
        ts[0] = 0;
        ts[1] = 1;
    }
    hle_ret_s32(c, 0);
}

/* int gettimeofday(struct timeval *tv, struct timezone *tz) */
static void gettimeofday_(hle_call *c) {
    int64_t *tv = (int64_t *)hle_mem(c, hle_arg_ptr(c, 0), 16);
    if (tv) {
        struct timespec ts;
        timespec_get(&ts, TIME_UTC);
        tv[0] = (int64_t)ts.tv_sec;
        tv[1] = (int64_t)ts.tv_nsec / 1000;
    }
    /* tz is obsolete and left alone: games pass stale pointers there (Smurfs passes
     * one into its read-only data, where the console's copyout would just fail) */
    hle_ret_s32(c, 0);
}

/* The host clock is taken as UTC and the console runs in UTC.
 * int sceKernelConvertUtcToLocaltime(time_t utc, time_t *local, SceKernelTimesec *st, uint64_t *dst_seconds)
 * SceKernelTimesec { time_t t; uint32_t west_sec; uint32_t dst_sec; } */
static void convert_utc_to_localtime(hle_call *c) {
    int64_t utc = (int64_t)hle_arg(c, 0);
    int64_t *local = (int64_t *)hle_mem(c, hle_arg_ptr(c, 1), 8);
    uint8_t *st = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 2), 16);
    uint64_t *dst = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 3), 8);
    if (local) *local = utc;
    if (st) {
        memcpy(st, &utc, 8);
        memset(st + 8, 0, 8); /* west_sec, dst_sec */
    }
    if (dst) *dst = 0;
    hle_ret_s32(c, 0);
}

/* int sceKernelConvertLocaltimeToUtc(time_t local, int is_dst, time_t *utc, SceKernelTimesec *st, int *dst_seconds)
 * (libc's mktime reads st->west_sec and st->dst_sec) */
static void convert_localtime_to_utc(hle_call *c) {
    int64_t local = (int64_t)hle_arg(c, 0);
    int64_t *utc = (int64_t *)hle_mem(c, hle_arg_ptr(c, 2), 8);
    uint8_t *st = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 3), 16);
    int32_t *dst = (int32_t *)hle_mem(c, hle_arg_ptr(c, 4), 4);
    if (utc) *utc = local;
    if (st) {
        memcpy(st, &local, 8);
        memset(st + 8, 0, 8); /* west_sec, dst_sec */
    }
    if (dst) *dst = 0;
    hle_ret_s32(c, 0);
}

/* int scePthreadOnce(pthread_once_t *once, void (*init)(void)): once is
 * { int state; ... }, state 0 never run, 1 done, 2 running. The first caller
 * runs init; the others wait until it is done. If init could not run, the
 * state goes back to 0 so a later caller tries again: it is never marked done. */
static void pthread_once_(hle_call *c) {
    volatile int32_t *state = (volatile int32_t *)hle_mem(c, hle_arg_ptr(c, 0), 4);
    guest_addr init = hle_arg_ptr(c, 1);
    if (!state || !init) {
        hle_ret_s32(c, 22); /* EINVAL */
        return;
    }
    for (;;) {
        int32_t expected = 0;
        if (__atomic_compare_exchange_n(state, &expected, 2, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            uint64_t rax = 0;
            if (!runtime_call_nested(c->rt, init, 0, 0, 0, &rax)) {
                __atomic_store_n(state, 0, __ATOMIC_RELEASE);
                hle_ret_s32(c, 11); /* EAGAIN */
                return;
            }
            __atomic_store_n(state, 1, __ATOMIC_RELEASE);
            break;
        }
        if (expected == 1) break;
        while ((expected = __atomic_load_n(state, __ATOMIC_ACQUIRE)) == 2) pal_sleep_ns(100000);
        if (expected == 1) break;
    }
    hle_ret_s32(c, 0);
}

/* int getargc(void), char **getargv(void): the process parameters main() received
 * (runtime.c: u32 argc, pad, inline argv pointers) */
static void getargc_(hle_call *c) {
    uint32_t argc = 0;
    if (c->rt && c->rt->process_params) gmem_read(c->mem, c->rt->process_params, &argc, 4);
    hle_ret_s32(c, (int32_t)argc);
}
static void getargv_(hle_call *c) { hle_ret_u64(c, c->rt && c->rt->process_params ? c->rt->process_params + 8 : 0); }

/* int getrusage(int who, struct rusage *ru): 144 bytes (two timevals, 14 longs); the
 * user time is the process's run time so far, everything else 0 */
static void getrusage_(hle_call *c) {
    int64_t ru[18] = {0};
    static uint64_t start;
    if (!start) start = pal_time_ns();
    uint64_t ns = pal_time_ns() - start;
    ru[0] = (int64_t)(ns / 1000000000ull);
    ru[1] = (int64_t)(ns % 1000000000ull / 1000);
    if (!hle_arg_ptr(c, 1) || !gmem_write(c->mem, hle_arg_ptr(c, 1), ru, sizeof(ru))) {
        hle_ret_s32(c, -1);
        return;
    }
    hle_ret_s32(c, 0);
}

/* sig_t signal(int, sig_t): the previous handler, SIG_DFL (no host signal reaches the guest) */
static void signal_(hle_call *c) { hle_ret_u64(c, 0); }

/* int nanosleep(const struct timespec *req, struct timespec *rem) */
static void nanosleep_(hle_call *c) {
    const int64_t *req = (const int64_t *)hle_mem(c, hle_arg_ptr(c, 0), 16);
    if (!req || req[0] < 0 || req[1] < 0 || req[1] >= 1000000000) {
        hle_ret_s32(c, 22); /* EINVAL */
        return;
    }
    pal_sleep_ns((uint64_t)req[0] * 1000000000ull + (uint64_t)req[1]);
    int64_t *rem = (int64_t *)hle_mem(c, hle_arg_ptr(c, 1), 16);
    if (rem) rem[0] = rem[1] = 0;
    hle_ret_s32(c, 0);
}

static void usleep_(hle_call *c) {
    pal_sleep_ns((uint64_t)hle_arg_u32(c, 0) * 1000u);
    hle_ret_s32(c, 0);
}

static void sleep_(hle_call *c) { /* seconds (No Man's Sky) */
    pal_sleep_ns((uint64_t)hle_arg_u32(c, 0) * 1000000000ull);
    hle_ret_s32(c, 0);
}

/* ---- Misc ---- */

static void debug_out_text(hle_call *c) {
    char text[1024];
    hle_read_str(c, hle_arg_ptr(c, 1), text, sizeof(text));
    size_t n = strlen(text);
    while (n && (text[n - 1] == '\n' || text[n - 1] == '\r')) text[--n] = '\0';
    LOG_I(KERNEL, "[guest] %s", text);
    hle_ret_s32(c, 0);
}

/* The SDK version the executable was built with: its SceProcParam's (u32 at +0x10, e.g.
 * 0x04508001), 5.00 without one */
static uint32_t compiled_sdk_version(hle_call *c) {
    const loaded_module *m = module_set_main(c->rt->ms);
    uint32_t v = 0;
    if (m && m->mapped && m->img.procparam_vaddr && m->img.procparam_size >= 0x14)
        gmem_read(c->mem, m->bias + m->img.procparam_vaddr + 0x10, &v, 4);
    static bool logged;
    if (!logged) {
        logged = true;
        LOG_I(KERNEL, "executable built with SDK 0x%08x", v);
    }
    return v ? v : 0x05000000u;
}

static void get_compiled_sdk_version(hle_call *c) {
    uint32_t *out = (uint32_t *)hle_mem(c, hle_arg_ptr(c, 0), 4);
    if (!out) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    *out = compiled_sdk_version(c);
    hle_ret_s32(c, 0);
}

static void exit_(hle_call *c) {
    int32_t code = hle_arg_s32(c, 0);
    LOG_I(KERNEL, "guest called _exit(%d)", code);
    runtime_exit_thread(c->rt, code);
}

/* ---- Process ---- */

/* The executable's SceProcParam (PT_SCE_PROCPARAM): libc reads its heap and
 * malloc-replacement settings from here. */
static void get_proc_param(hle_call *c) {
    const loaded_module *m = module_set_main(c->rt->ms);
    hle_ret_u64(c, m && m->mapped && m->img.procparam_vaddr ? m->bias + m->img.procparam_vaddr : 0);
}

/* libc registers per-thread destructor / atexit hooks; called at thread exit. */
static void set_thread_dtors(hle_call *c) { c->rt->thread_dtors = hle_arg_ptr(c, 0); }
static void set_thread_atexit_count(hle_call *c) { c->rt->thread_atexit_count = hle_arg_ptr(c, 0); }
static void set_thread_atexit_report(hle_call *c) { c->rt->thread_atexit_report = hle_arg_ptr(c, 0); }

static void debug_raise_exception(hle_call *c) {
    uint32_t code = hle_arg_u32(c, 0);
    const loaded_module *m = runtime_find_module(c->rt, return_address(c));
    LOG_E(KERNEL, "sceKernelDebugRaiseException(0x%08x) from %s: guest aborted", code, m ? m->name : "?");
    runtime_log_backtrace(c->rt);
    runtime_exit_thread(c->rt, (int64_t)code);
}

/* int sceKernelUuidCreate(SceKernelUuid *uuid): random (version 4) UUID */
static void uuid_create(hle_call *c) {
    uint8_t *u = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 16);
    if (!u) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    uint64_t s = pal_time_ns() ^ (uint64_t)(uintptr_t)u;
    for (int i = 0; i < 16; i++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        u[i] = (uint8_t)(s >> 56);
    }
    u[6] = (uint8_t)((u[6] & 0x0F) | 0x40);
    u[8] = (uint8_t)((u[8] & 0x3F) | 0x80);
    hle_ret_s32(c, 0);
}

/* Chip id. libSceAgc compares (id & ~0x1f) with 0x840fc0 to pick a register
 * table variant; 0 selects the default (retail PS5) path. */
static void get_main_soc_id(hle_call *c) { hle_ret_u32(c, 0); }

static void getpid_(hle_call *c) { hle_ret_s32(c, 1); }
static void sysconf_(hle_call *c) {
    int64_t value=-1;
    /* FreeBSD/PS5 unistd.h numbers, never the Windows sysconf namespace. */
    switch(hle_arg_s32(c,0)) {
    case 1:value=262144;break;
    case 3:value=100;break;
    case 5:case 26:value=1024;break;
    case 47:case 93:value=GUEST_PAGE_SIZE;break;
    case 57:case 58:value=8;break;
    case 85:value=4;break;
    case 86:value=GUEST_THREAD_KEYS;break;
    case 96:value=200809;break;
    case 121:value=(14ull<<30)/GUEST_PAGE_SIZE;break;
    default:if(c->thread&&c->thread->errno_addr){int e=22;gmem_write(c->mem,c->thread->errno_addr,&e,4);}break;
    }
    hle_ret_u64(c,(uint64_t)value);
}

/* ---- Exception handlers (signals) ---- */

#define SCE_KERNEL_ERROR_EAGAIN ((int32_t)0x80020023)

/* int sceKernelInstallExceptionHandler(int sig, void (*handler)(int sig, void *ucontext)) */
static void install_exception_handler(hle_call *c) {
    int32_t sig = hle_arg_s32(c, 0);
    if (sig < 1 || sig > 128 || !hle_arg_ptr(c, 1)) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    if (c->rt->exception_handlers[sig - 1]) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EAGAIN);
        return;
    }
    c->rt->exception_handlers[sig - 1] = hle_arg_ptr(c, 1);
    LOG_I(KERNEL, "exception handler for signal %d at 0x%llx", sig, (unsigned long long)hle_arg_ptr(c, 1));
    hle_ret_s32(c, 0);
}

static void remove_exception_handler(hle_call *c) {
    int32_t sig = hle_arg_s32(c, 0);
    if (sig < 1 || sig > 128) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    c->rt->exception_handlers[sig - 1] = 0;
    hle_ret_s32(c, 0);
}

/* int sceKernelRaiseException(ScePthread thread, int sig) */
static void raise_exception(hle_call *c) {
    int err = runtime_raise_exception(c->rt, hle_arg(c, 0), hle_arg_s32(c, 1));
    hle_ret_s32(c, err ? (int32_t)(0x80020000u | (uint32_t)err) : 0);
}

/* int _is_signal_return(uintptr_t pc): the unwinder asks whether a frame is
 * a signal trampoline. We never deliver signals into guest code. */
static void is_signal_return(hle_call *c) { hle_ret_s32(c, 0); }

/* int sceKernelTitleWorkaroundIsEnabled(const void *info, int id, int *enabled) */
static void title_workaround_is_enabled(hle_call *c) {
    int32_t *out = (int32_t *)hle_mem(c, hle_arg_ptr(c, 2), 4);
    if (out) *out = 0;
    hle_ret_s32(c, 0);
}

/* void sceKernelError(int code): libraries report an error code. */
static void kernel_error(hle_call *c) {
    LOG_D(KERNEL, "sceKernelError(%d)", hle_arg_s32(c, 0));
    hle_ret_s32(c, 0);
}

/* ---- Modules ---- */

/* int sceKernelDlsym(SceKernelModule handle, const char *symbol, void **addr)
 * The handle's module answers first, then the rest of the process (HLE included). */
static void dlsym_(hle_call *c) {
    char name[512];
    uint64_t *out = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 2), 8);
    if (!out || !hle_read_str(c, hle_arg_ptr(c, 1), name, sizeof(name))) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    guest_addr addr = runtime_resolve_symbol_in(c->rt, hle_arg_u32(c, 0), name);
    LOG_D(KERNEL, "dlsym(0x%x, \"%s\") -> 0x%llx", hle_arg_u32(c, 0), name, (unsigned long long)addr);
    if (!addr) {
        LOG_W(KERNEL, "dlsym: '%s' not found", name);
        hle_ret_s32(c, SCE_KERNEL_ERROR_ESRCH);
        return;
    }
    *out = addr;
    hle_ret_s32(c, 0);
}

/* Module names compare without case (dumps do not always keep the case the game asks for). */
static int name_casecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca) return ca - cb;
    }
    return 0;
}

static void sandbox_random_word(hle_call *c) {
    pal_mutex_lock(c->rt->threads_lock);
    if(!c->rt->sandbox_word) {
        guest_addr a=0;
        if(gmem_map(c->mem,0,GUEST_PAGE_SIZE,GPROT_RW,0,"sandbox name",&a)==PS5C_OK) {
            static const char word[]="zeraps5";
            gmem_write(c->mem,a,word,sizeof(word));
            gmem_protect(c->mem,a,GUEST_PAGE_SIZE,GPROT_READ);
            c->rt->sandbox_word=a;
        }
    }
    hle_ret_u64(c,c->rt->sandbox_word);
    pal_mutex_unlock(c->rt->threads_lock);
}

/* SceKernelModule sceKernelLoadStartModule(const char *path, size_t argc, const void *argv,
 *                                          uint32_t flags, const void *opt, int *result)
 * Modules the game loads at run time (Unity native plugins) are loaded with
 * the process by the front end; this finds one by file name. Its module_start
 * is not called yet. */
static void load_start_module(hle_call *c) {
    char path[512];
    if (!hle_read_str(c, hle_arg_ptr(c, 0), path, sizeof(path))) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char name[512];
    snprintf(name, sizeof(name), "%.255s", base);
    char *dot = strrchr(name, '.');
    if (dot) *dot = 0;
    const module_set *ms = c->rt->ms;
    for (size_t i = 0; i < ms->modules.len; i++) {
        const loaded_module *m = ms->modules.data[i];
        const char *file = m->path ? strrchr(m->path, '/') : NULL;
        const char *file2 = m->path ? strrchr(m->path, '\\') : NULL;
        if (file2 > file) file = file2;
        file = file ? file + 1 : m->path;
        bool match = !name_casecmp(m->name, name, (size_t)-1) || (file && !name_casecmp(file, name, strlen(name)) && file[strlen(name)] == '.');
        if (!match || !m->mapped) continue;
        int32_t start_result = 0;
        if (m->deferred_start && !m->started) {
            loaded_module *lm = ms->modules.data[i];
            lm->started = true;
            /* the PRX init entry forwards (argc, argv) to module_start */
            guest_addr start = m->img.has_init ? m->bias + m->img.init_vaddr : m->img.entry ? m->bias + m->img.entry : 0;
            if (start) {
                uint64_t rax = 0;
                runtime_call_nested(c->rt, start, hle_arg(c, 1), hle_arg(c, 2), 0, &rax);
                start_result = (int32_t)rax;
                LOG_I(KERNEL, "module_start of %s (argc 0x%llx, argv 0x%llx) returned %d", m->name,
                      (unsigned long long)hle_arg(c, 1), (unsigned long long)hle_arg(c, 2), start_result);
            }
        }
        int32_t *result = (int32_t *)hle_mem(c, hle_arg_ptr(c, 5), 4);
        if (result) *result = start_result;
        LOG_I(KERNEL, "sceKernelLoadStartModule(\"%s\") -> module %zu ('%s')", path, i + 1, m->name);
        hle_ret_s32(c, (int32_t)(i + 1));
        return;
    }
    /* A system module implemented in HLE has no PRX to start. Give it a real
     * library-scoped handle so later dlsym stays within that library. */
    uint32_t hle_handle=hle_library_handle(name);
    if(hle_handle) {
        int32_t zero=0;
        if(hle_arg_ptr(c,5))gmem_write(c->mem,hle_arg_ptr(c,5),&zero,4);
        LOG_I(KERNEL,"sceKernelLoadStartModule(\"%s\") -> HLE %s",path,name);
        hle_ret_u32(c,hle_handle);return;
    }
    LOG_W(KERNEL, "sceKernelLoadStartModule(\"%s\"): module not loaded", path);
    hle_ret_s32(c, (int32_t)0x80020002); /* ENOENT */
}

/* Size of .eh_frame: CIE/FDE records up to the zero terminator. */
static uint64_t eh_frame_size(hle_call *c, guest_addr start, guest_addr limit) {
    guest_addr p = start;
    while (p + 4 <= limit) {
        uint32_t len32 = 0;
        if (!gmem_read(c->mem, p, &len32, 4)) break;
        if (len32 == 0) return p + 4 - start;
        uint64_t len = len32, hdr = 4;
        if (len32 == 0xFFFFFFFFu) {
            if (!gmem_read_u64(c->mem, p + 4, &len)) break;
            hdr = 12;
        }
        p += hdr + len;
    }
    return p - start;
}

/* int sceKernelGetModuleInfoForUnwind(uintptr_t addr, int flags, info *out)
 * info: u64 st_size; char name[N]; then five u64: eh_frame_hdr address,
 * eh_frame address, eh_frame size, first segment address and size.
 * N is 256 on the PS5 (st_size 304) and 512 in the PS4 layout (st_size 560). */
#define UNWIND_TAIL_BYTES 40
/* The unwinder asks on every frame of every C++ throw and walking .eh_frame
 * is linear in its size: remember the size per module. */
static uint64_t cached_eh_frame_size(hle_call *c, const loaded_module *m, guest_addr eh_frame) {
    static struct {
        const loaded_module *m;
        guest_addr eh_frame;
        uint64_t size;
    } cache[128];
    uint64_t size = 0;
    bool found = false;
    kobj_lock();
    for (size_t i = 0; i < PS5C_ARRAY_COUNT(cache) && cache[i].m; i++) {
        if (cache[i].m == m && cache[i].eh_frame == eh_frame) {
            size = cache[i].size;
            found = true;
            break;
        }
    }
    kobj_unlock();
    if (found) return size;
    size = eh_frame_size(c, eh_frame, m->base + m->map_size);
    kobj_lock();
    for (size_t i = 0; i < PS5C_ARRAY_COUNT(cache); i++) {
        if (!cache[i].m) {
            cache[i].eh_frame = eh_frame;
            cache[i].size = size;
            cache[i].m = m;
            break;
        }
    }
    kobj_unlock();
    return size;
}

/* The module's eh_frame_hdr, the eh_frame it points to and that one's size (0 when absent) */
static void module_eh_frame(hle_call *c, const loaded_module *m, uint64_t *hdr_out, uint64_t *frame, uint64_t *size) {
    *hdr_out = *frame = *size = 0;
    if (!m->img.eh_frame_hdr_vaddr) return;
    /* eh_frame_hdr: version, eh_frame_ptr_enc, fde_count_enc, table_enc, eh_frame_ptr */
    guest_addr hdr = m->bias + m->img.eh_frame_hdr_vaddr;
    uint8_t h[4] = {0};
    int32_t rel = 0;
    *hdr_out = hdr;
    if (gmem_read(c->mem, hdr, h, 4) && h[0] == 1 && gmem_read(c->mem, hdr + 4, &rel, 4)) {
        if (h[1] == 0x1B) *frame = hdr + 4 + (int64_t)rel;                 /* pcrel | sdata4 */
        else if (h[1] == 0x03 || h[1] == 0x0B) *frame = (uint32_t)rel;      /* absolute */
        else LOG_W(KERNEL, "%s: eh_frame_ptr encoding 0x%02x not supported", m->name, h[1]);
    }
    if (*frame) *size = cached_eh_frame_size(c, m, *frame);
}

static void get_module_info_for_unwind(hle_call *c) {
    guest_addr addr = hle_arg_ptr(c, 0), out = hle_arg_ptr(c, 2);
    int32_t flags = hle_arg_s32(c, 1);
    uint64_t st_size = 0;
    if (!gmem_read_u64(c->mem, out, &st_size)) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EFAULT);
        return;
    }
    if (flags >= 3 || (st_size != 304 && st_size < 560)) {
        LOG_W(KERNEL, "unwind info for 0x%llx: flags %d, st_size %llu not supported", (unsigned long long)addr, flags,
              (unsigned long long)st_size);
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    uint64_t name_len = st_size == 304 ? 256 : 512;
    uint8_t *info = (uint8_t *)hle_mem(c, out, 8 + name_len + UNWIND_TAIL_BYTES);
    const loaded_module *m = runtime_find_module(c->rt, addr);
    if (!info || !m) {
        hle_ret_s32(c, info ? SCE_KERNEL_ERROR_ESRCH : SCE_KERNEL_ERROR_EFAULT);
        return;
    }
    uint64_t tail[5] = {0}; /* eh_frame_hdr, eh_frame, eh_frame size, seg0 address, seg0 size */
    module_eh_frame(c, m, &tail[0], &tail[1], &tail[2]);
    for (uint32_t i = 0; i < m->img.segment_count; i++) {
        const elf64_phdr *ph = &m->img.segments[i].h;
        if (ph->p_type == PT_LOAD) {
            tail[3] = m->bias + ph->p_vaddr;
            tail[4] = ph->p_memsz;
            break;
        }
    }
    memset(info + 8, 0, name_len);
    snprintf((char *)info + 8, name_len, "%s", m->name);
    memcpy(info + 8 + name_len, tail, sizeof(tail));
    LOG_D(KERNEL, "unwind info for 0x%llx (flags %d): %s eh_frame 0x%llx+0x%llx", (unsigned long long)addr, flags,
          m->name, (unsigned long long)tail[1], (unsigned long long)tail[2]);
    hle_ret_s32(c, 0);
}

/* int sceKernelGetModuleInfoFromAddr(uintptr_t addr, int flags, SceKernelModuleInfoEx *info)
 * info (0x1A8 bytes): u64 st_size; char name[256]; s32 handle; u32 tls_index;
 * u64 tls_init_addr; u32 tls_init_size, tls_size, tls_offset, tls_align;
 * u64 init_proc, fini_proc, reserved[2], eh_frame_hdr_addr, eh_frame_addr;
 * u32 eh_frame_hdr_size, eh_frame_size; {u64 address; u32 size; s32 prot} segments[4];
 * u32 segment_count, ref_count. Handles are the module index + 1, as
 * sceKernelLoadStartModule returns them. */
#define MODULE_INFO_EX_SIZE 0x1A8u
static void get_module_info_from_addr(hle_call *c) {
    guest_addr addr = hle_arg_ptr(c, 0);
    uint8_t *info = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 2), MODULE_INFO_EX_SIZE);
    const loaded_module *m = runtime_find_module(c->rt, addr);
    if (!info || !m) {
        hle_ret_s32(c, info ? SCE_KERNEL_ERROR_ESRCH : SCE_KERNEL_ERROR_EFAULT);
        return;
    }
    int32_t handle = 0;
    for (size_t i = 0; i < c->rt->ms->modules.len; i++)
        if (c->rt->ms->modules.data[i] == m) handle = (int32_t)i + 1;
    uint64_t eh[3];
    module_eh_frame(c, m, &eh[0], &eh[1], &eh[2]);
    memset(info, 0, MODULE_INFO_EX_SIZE);
    uint64_t v64 = MODULE_INFO_EX_SIZE;
    memcpy(info, &v64, 8);
    snprintf((char *)info + 8, 256, "%s", m->name);
    uint32_t v32[4] = {(uint32_t)handle, m->tls_index};
    memcpy(info + 0x108, v32, 8);
    if (m->img.has_tls) {
        v64 = m->bias + m->img.tls_vaddr;
        v32[0] = (uint32_t)m->img.tls_filesz, v32[1] = (uint32_t)m->img.tls_memsz;
        v32[2] = (uint32_t)m->tls_offset, v32[3] = (uint32_t)m->img.tls_align;
        memcpy(info + 0x110, &v64, 8);
        memcpy(info + 0x118, v32, 16);
    }
    uint64_t procs[2] = {m->img.has_init ? m->bias + m->img.init_vaddr : 0,
                         m->img.has_fini ? m->bias + m->img.fini_vaddr : 0};
    memcpy(info + 0x128, procs, 16);
    memcpy(info + 0x148, eh, 16);
    v32[0] = (uint32_t)m->img.eh_frame_hdr_size, v32[1] = (uint32_t)eh[2];
    memcpy(info + 0x158, v32, 8);
    uint32_t count = 0;
    for (uint32_t i = 0; i < m->img.segment_count && count < 4; i++) {
        const elf64_phdr *ph = &m->img.segments[i].h;
        if (ph->p_type != PT_LOAD) continue;
        /* p_flags: X 1, W 2, R 4; kernel protection: read 1, write 2, execute 4 */
        uint32_t prot = ((ph->p_flags & 4) ? 1u : 0) | ((ph->p_flags & 2) ? 2u : 0) | ((ph->p_flags & 1) ? 4u : 0);
        uint64_t seg_addr = m->bias + ph->p_vaddr;
        uint32_t seg[2] = {(uint32_t)ph->p_memsz, prot};
        memcpy(info + 0x160 + count * 16, &seg_addr, 8);
        memcpy(info + 0x168 + count * 16, seg, 8);
        count++;
    }
    v32[0] = count, v32[1] = 1;
    memcpy(info + 0x1A0, v32, 8);
    hle_ret_s32(c, 0);
}

/* int sceKernelGetAppInfo(int pid, SceKernelAppInfo *info): the app id (1) at 0
 * and the title id (char[10]) at 0x10 */
static void get_app_info(hle_call *c) {
    int32_t pid = hle_arg_s32(c, 0);
    guest_addr out = hle_arg_ptr(c, 1);
    if (pid != 0 && pid != 1) {
        hle_ret_s32(c, (int32_t)0x80020001); /* EPERM: another process */
        return;
    }
    uint8_t *info = out ? (uint8_t *)hle_mem(c, out, 0x1A) : NULL;
    if (info) {
        char title[16] = {0};
        if (!settings_param_string("titleId", title, sizeof(title))) title[0] = 0;
        uint32_t app_id = 1;
        memcpy(info, &app_id, 4);
        memset(info + 0x10, 0, 10);
        memcpy(info + 0x10, title, strnlen(title, 9));
    }
    hle_ret_s32(c, 0);
}

/* int sceKernelGetCurrentCpu(void): one of the cores the thread may run on,
 * fixed per thread (games index per-core caches with it) */
static void get_current_cpu(hle_call *c) {
    uint64_t mask = c->thread && c->thread->affinity ? c->thread->affinity : 0x7Full;
    uint32_t n = (uint32_t)__builtin_popcountll(mask), pick = c->thread ? c->thread->id % (n ? n : 1) : 0;
    int32_t cpu = 0;
    for (int32_t i = 0; i < 64; i++) {
        if (!(mask >> i & 1)) continue;
        if (pick-- == 0) {
            cpu = i;
            break;
        }
    }
    hle_ret_s32(c, cpu);
}

/* int _sigprocmask(int how, const sigset_t *set, sigset_t *oset): the process
 * mask is only recorded (signals are delivered through exception handlers).
 * how: 1 block, 2 unblock, 3 set; sigset_t is u32[4]. */
static uint32_t g_sigmask[4];
static void sigprocmask_(hle_call *c) {
    int32_t how = hle_arg_s32(c, 0);
    guest_addr set = hle_arg_ptr(c, 1), oset = hle_arg_ptr(c, 2);
    uint32_t s[4] = {0};
    if (set && (how < 1 || how > 3 || !gmem_read(c->mem, set, s, sizeof(s)))) {
        hle_ret_s32(c, -1);
        return;
    }
    kobj_lock();
    if (oset) gmem_write(c->mem, oset, g_sigmask, sizeof(g_sigmask));
    for (int i = 0; set && i < 4; i++) {
        if (how == 1) g_sigmask[i] |= s[i];
        else if (how == 2) g_sigmask[i] &= ~s[i];
        else g_sigmask[i] = s[i];
    }
    kobj_unlock();
    hle_ret_s32(c, 0);
}

/* void sceKernelDebugRaiseExceptionOnReleaseMode(int code1, int code2): a debug
 * break in development builds, nothing in retail */
static void debug_raise_exception_on_release(hle_call *c) {
    LOG_W(KERNEL, "sceKernelDebugRaiseExceptionOnReleaseMode(%d, %d) from 0x%llx", hle_arg_s32(c, 0),
          hle_arg_s32(c, 1), (unsigned long long)return_address(c));
    hle_ret_s32(c, 0);
}

static void kernel_ok(hle_call *c) { hle_ret_s32(c, 0); }

/* int sceKernelIsTrinityMode(void): the game runs in PS5 Pro mode. Only when the settings ask
 * for a PS5 Pro ([geral] console=1) and the game supports it (param.json attribute3 0x400000);
 * else a PS5, as before. */
static bool trinity_mode(void) {
    static int pro = -1;
    if (pro < 0) {
        int64_t attr3 = 0;
        pro = settings_get_int("geral", "console", 0) == 1 && settings_param_int("attribute3", &attr3) &&
              (attr3 & 0x400000);
        LOG_I(KERNEL, "console: %s", pro ? "PS5 Pro" : "PS5");
    }
    return pro;
}
static void kernel_is_trinity_mode(hle_call *c) { hle_ret_s32(c, trinity_mode()); }

/* int sceKernelGetOperationMode(int *mode, int *submode): 2 = PS5, 3 = PS5 Pro mode; no submode */
static void kernel_get_operation_mode(hle_call *c) {
    int32_t mode = trinity_mode() ? 3 : 2, submode = 0;
    if (hle_arg_ptr(c, 0)) gmem_write(c->mem, hle_arg_ptr(c, 0), &mode, 4);
    if (hle_arg_ptr(c, 1)) gmem_write(c->mem, hle_arg_ptr(c, 1), &submode, 4);
    hle_ret_s32(c, 0);
}

/* int shm_open(const char *path, int flags, mode_t mode), physhm_open(...) and their
 * unlinks: no POSIX/physical shared memory objects here; ENOSYS lets the caller fall back. */
static void shm_unsupported(hle_call *c) {
    int32_t e = 78; /* ENOSYS */
    if (c->thread && c->thread->errno_addr) gmem_write(c->mem, c->thread->errno_addr, &e, 4);
    hle_ret_s32(c, -1);
}

/* int sysarch(int op, void *parms): the thread pointer. AMD64_GET/SET_FSBASE (128/129) and
 * GSBASE (130/131) take a 64-bit base, I386_GET/SET_FSBASE (7/8) and GSBASE (9/10) a 32-bit
 * one. FS holds the TCB (the patched code reads it from the host TLS slot); GS is unused by
 * the guest and reads as 0. Other operations: EINVAL. */
static void sysarch_(hle_call *c) {
    int32_t op = hle_arg_s32(c, 0);
    guest_addr parms = hle_arg_ptr(c, 1);
    int32_t err = 0;
    bool wide = op >= 128;
    switch (op) {
    case 7: case 9: case 128: case 130: {
        uint64_t v = (op == 7 || op == 128) && c->thread ? c->thread->tcb : 0;
        if (!parms || !gmem_write(c->mem, parms, &v, wide ? 8 : 4)) err = 14; /* EFAULT */
        break;
    }
    case 8: case 129: {
        uint64_t v = 0;
        if (!parms || !gmem_read(c->mem, parms, &v, wide ? 8 : 4)) err = 14;
        else if (c->thread) {
            c->thread->tcb = v;
            pal_tls_set(c->rt->tcb_slot, (void *)(uintptr_t)v);
        }
        break;
    }
    case 10: case 131: break; /* GS base: not used by guest code here */
    default: err = 22; /* EINVAL */
    }
    if (err && c->thread && c->thread->errno_addr) gmem_write(c->mem, c->thread->errno_addr, &err, 4);
    hle_ret_s32(c, err ? -1 : 0);
}

/* int sceKernelGetOpenPsId(SceKernelOpenPsId *id): 16 bytes that identify the console to
 * services; a fixed one (no account, no network) */
static void get_open_ps_id(hle_call *c) {
    static const uint8_t k_id[16] = {'Z', 'e', 'r', 'a', 'P', 'S', '5', 0, 0x50, 0x53, 0x35, 0x43, 0, 0, 0, 1};
    uint8_t *out = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 16);
    if (out) memcpy(out, k_id, 16);
    hle_ret_s32(c, out ? 0 : SCE_KERNEL_ERROR_EFAULT);
}

/* int sceKernelWriteThrottlingStatus(u64 status[4]): no throttling (the budget all free) */
static void write_throttling_status(hle_call *c) {
    uint64_t v[4] = {0xFFFFFFFFu, 0, 0, 0};
    hle_ret_s32(c, gmem_write(c->mem, hle_arg_ptr(c, 0), v, sizeof(v)) ? 0 : SCE_KERNEL_ERROR_EFAULT);
}

/* int fcntl(int fd, int cmd, ...): descriptor and status flags are accepted and read back as 0
 * (Minecraft sets its sockets non-blocking; nothing here blocks on them) */
static void ok_flock(hle_call *c) { hle_ret_s32(c, 0); }
/* int sceKernelIsNeoMode(void) (PS4): 0, a base PS4 rather than a PS4 Pro */
static void is_neo_mode(hle_call *c) { hle_ret_s32(c, 0); }

static void fcntl_(hle_call *c) {
    int32_t cmd = hle_arg_s32(c, 1);
    hle_ret_s32(c, cmd >= 0 && cmd <= 4 ? 0 : -1); /* F_DUPFD..F_SETFL */
}

/* ---- POSIX odds and ends of PS2 games wrapped for PS5 (Burnout Revenge, Prince of Persia) ---- */

/* sigset_t is 128 bits; signal n is bit n - 1 */
static void sigset_fill(hle_call *c, uint8_t v) {
    uint8_t *s = hle_arg_ptr(c, 0) ? (uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 16) : NULL;
    if (s) memset(s, v, 16);
    hle_ret_s32(c, s ? 0 : -1);
}
static void sigemptyset_(hle_call *c) { sigset_fill(c, 0); }
static void sigfillset_(hle_call *c) { sigset_fill(c, 0xFF); }
static void sigset_bit(hle_call *c, int op) {
    uint32_t *s = hle_arg_ptr(c, 0) ? (uint32_t *)hle_mem(c, hle_arg_ptr(c, 0), 16) : NULL;
    int32_t n = hle_arg_s32(c, 1);
    if (!s || n < 1 || n > 128) {
        hle_ret_s32(c, -1);
        return;
    }
    uint32_t bit = 1u << ((n - 1) & 31), *w = &s[(n - 1) / 32];
    if (op == 0) *w |= bit;
    else if (op == 1) *w &= ~bit;
    hle_ret_s32(c, op == 2 ? (*w & bit) != 0 : 0);
}
static void sigaddset_(hle_call *c) { sigset_bit(c, 0); }
static void sigdelset_(hle_call *c) { sigset_bit(c, 1); }
static void sigismember_(hle_call *c) { sigset_bit(c, 2); }
/* sigaction(sig, act, oldact): handlers are accepted and never called (no signals are raised) */
/* sigaction(sig, act, oldact): struct sigaction {handler; int flags; sigset_t mask} (32 bytes).
 * SIGSEGV and SIGBUS handlers are called on faults in guest code (runtime.c, deliver_signal);
 * the others are kept and never raised. */
static void sigaction_(hle_call *c) {
    int32_t sig = hle_arg_s32(c, 0);
    if (sig < 1 || sig > 32) {
        hle_ret_s32(c, -1);
        return;
    }
    uint8_t *old = hle_arg_ptr(c, 2) ? (uint8_t *)hle_mem(c, hle_arg_ptr(c, 2), 32) : NULL;
    if (old) {
        memset(old, 0, 32);
        memcpy(old, &c->rt->sig_handler[sig], 8);
        memcpy(old + 8, &c->rt->sig_flags[sig], 4);
    }
    const uint8_t *act = hle_arg_ptr(c, 1) ? (const uint8_t *)hle_mem(c, hle_arg_ptr(c, 1), 32) : NULL;
    if (act) {
        guest_addr h;
        uint32_t f;
        memcpy(&h, act, 8);
        memcpy(&f, act + 8, 4);
        c->rt->sig_flags[sig] = f;
        __atomic_store_n(&c->rt->sig_handler[sig], h > 1 ? h : 0, __ATOMIC_RELEASE); /* SIG_DFL 0, SIG_IGN 1 */
        if (h > 1 && (sig == 10 || sig == 11)) LOG_I(KERNEL, "guest %s handler at 0x%llx", sig == 11 ? "SIGSEGV" : "SIGBUS", (unsigned long long)h);
    }
    hle_ret_s32(c, 0);
}
static void sigmask_(hle_call *c) { /* sigprocmask / pthread_sigmask(how, set, oldset) */
    uint8_t *old = hle_arg_ptr(c, 2) ? (uint8_t *)hle_mem(c, hle_arg_ptr(c, 2), 16) : NULL;
    if (old) memset(old, 0, 16);
    hle_ret_s32(c, 0);
}
static void attr_getscope_(hle_call *c) { /* PTHREAD_SCOPE_PROCESS */
    int32_t v = 0;
    if (hle_arg_ptr(c, 1)) gmem_write(c->mem, hle_arg_ptr(c, 1), &v, 4);
    hle_ret_s32(c, 0);
}
static void ok0_(hle_call *c) { hle_ret_s32(c, 0); }
static void posix_sleep_(hle_call *c) { /* unsigned sleep(unsigned seconds): 0 left */
    pal_sleep_ns((uint64_t)hle_arg_u32(c, 0) * 1000000000ull);
    hle_ret_u32(c, 0);
}
/* sysctl(mib, n, old, oldlen, new, newlen): hw.ncpu and hw.pagesize; anything else reads as
 * zeros (kern.proc.pid: a process no debugger traces), each new name logged once */
static void sysctl_(hle_call *c) {
    uint32_t n = hle_arg_u32(c, 1), mib[8] = {0};
    if (n > 8) n = 8;
    if (hle_arg_ptr(c, 0)) gmem_read(c->mem, hle_arg_ptr(c, 0), mib, n * 4);
    guest_addr old = hle_arg_ptr(c, 2), lenp = hle_arg_ptr(c, 3);
    uint64_t len = 0;
    if (lenp) gmem_read(c->mem, lenp, &len, 8);
    int32_t value = -1;
    if (n == 2 && mib[0] == 6 && mib[1] == 3) value = 8;          /* hw.ncpu */
    else if (n == 2 && mib[0] == 6 && mib[1] == 7) value = 16384; /* hw.pagesize */
    if (value >= 0) {
        if (old && len >= 4) gmem_write(c->mem, old, &value, 4);
        len = 4;
    } else {
        static uint32_t seen[16][3];
        static uint32_t nseen;
        bool known = false;
        for (uint32_t i = 0; i < nseen; i++) known |= seen[i][0] == n && seen[i][1] == mib[0] && seen[i][2] == mib[1];
        if (!known && nseen < 16) {
            seen[nseen][0] = n, seen[nseen][1] = mib[0], seen[nseen][2] = mib[1], nseen++;
            LOG_I(KERNEL, "sysctl {%u, %u, %u, %u} (%u names): zeros", mib[0], mib[1], mib[2], mib[3], n);
        }
        uint8_t *o = old && len ? (uint8_t *)hle_mem(c, old, len) : NULL;
        if (o) memset(o, 0, (size_t)len);
    }
    if (lenp) gmem_write(c->mem, lenp, &len, 8);
    hle_ret_s32(c, 0);
}
/* sysctlbyname(name, old, oldlen, new, newlen): hw.ncpu, hw.pagesize; other names read as zeros */
static void sysctlbyname_(hle_call *c) {
    char name[64] = "";
    hle_read_str(c, hle_arg_ptr(c, 0), name, sizeof name);
    guest_addr old = hle_arg_ptr(c, 1), lenp = hle_arg_ptr(c, 2);
    uint64_t len = 0;
    if (lenp) gmem_read(c->mem, lenp, &len, 8);
    int32_t value = !strcmp(name, "hw.ncpu") ? 8 : !strcmp(name, "hw.pagesize") ? 16384 : -1;
    if (value >= 0) {
        if (old && len >= 4) gmem_write(c->mem, old, &value, 4);
        len = 4;
    } else {
        static uint32_t logged;
        if (logged++ < 8) LOG_I(KERNEL, "sysctlbyname \"%s\": zeros", name);
        uint8_t *o = old && len ? (uint8_t *)hle_mem(c, old, len) : NULL;
        if (o) memset(o, 0, (size_t)len);
    }
    if (lenp) gmem_write(c->mem, lenp, &len, 8);
    hle_ret_s32(c, 0);
}
/* sceKernelGetExecutableModuleHandle(): the main module's handle (index + 1, as GetModuleInfoFromAddr) */
static int32_t main_handle(hle_call *c) {
    for (size_t i = 0; i < c->rt->ms->modules.len; i++)
        if (c->rt->ms->modules.data[i]->is_main) return (int32_t)i + 1;
    return 1;
}
static void executable_handle_(hle_call *c) { hle_ret_s32(c, main_handle(c)); }
/* sceKernelGetModuleInfo(handle, SceKernelModuleInfo *): size, name[256], segments[4] {addr, size u32, prot u32},
 * segment count, fingerprint[20] */
static void get_module_info_(hle_call *c) {
    int32_t handle = hle_arg_s32(c, 0);
    uint8_t *info = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 1), 0x160);
    if (!info) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EFAULT);
        return;
    }
    if (handle < 1 || (size_t)handle > c->rt->ms->modules.len) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_ESRCH);
        return;
    }
    const loaded_module *m = c->rt->ms->modules.data[handle - 1];
    memset(info, 0, 0x160);
    uint64_t size = 0x160;
    memcpy(info, &size, 8);
    snprintf((char *)info + 8, 256, "%s", m->name ? m->name : "");
    uint32_t count = 0;
    for (uint32_t i = 0; i < m->img.segment_count && count < 4; i++) {
        const elf64_phdr *ph = &m->img.segments[i].h;
        if (ph->p_type != PT_LOAD) continue;
        uint32_t prot = ((ph->p_flags & 4) ? 1u : 0) | ((ph->p_flags & 2) ? 2u : 0) | ((ph->p_flags & 1) ? 4u : 0);
        uint64_t addr = m->bias + ph->p_vaddr;
        uint32_t seg[2] = {(uint32_t)ph->p_memsz, prot};
        memcpy(info + 0x108 + count * 16, &addr, 8);
        memcpy(info + 0x110 + count * 16, seg, 8);
        count++;
    }
    memcpy(info + 0x148, &count, 4);
    hle_ret_s32(c, 0);
}

/* pthread_barrier_t: a pointer the guest keeps; here, a host barrier. init(barrier *, attr, count),
 * wait(barrier *): PTHREAD_BARRIER_SERIAL_THREAD (-1) for one thread of each round, destroy(barrier *).
 * Prince of Persia's reSOT lines up its disk threads with them. */
typedef struct host_barrier {
    pal_mutex *lock;
    pal_cond *cond;
    uint32_t count, waiting;
    uint64_t round;
} host_barrier;
static host_barrier *barrier_at(hle_call *c) {
    uint64_t v = 0;
    if (!hle_arg_ptr(c, 0) || !gmem_read(c->mem, hle_arg_ptr(c, 0), &v, 8)) return NULL;
    return (host_barrier *)(uintptr_t)v;
}
static void barrier_init_(hle_call *c) {
    uint32_t count = hle_arg_u32(c, 2);
    host_barrier *b = count && hle_arg_ptr(c, 0) ? calloc(1, sizeof *b) : NULL;
    if (!b || pal_mutex_create(&b->lock) != PS5C_OK || pal_cond_create(&b->cond) != PS5C_OK) {
        free(b);
        hle_ret_s32(c, 22); /* EINVAL */
        return;
    }
    b->count = count;
    uint64_t v = (uint64_t)(uintptr_t)b;
    gmem_write(c->mem, hle_arg_ptr(c, 0), &v, 8);
    hle_ret_s32(c, 0);
}
static void barrier_wait_(hle_call *c) {
    host_barrier *b = barrier_at(c);
    if (!b) {
        hle_ret_s32(c, 22);
        return;
    }
    pal_mutex_lock(b->lock);
    uint64_t round = b->round;
    int32_t r = 0;
    if (++b->waiting == b->count) {
        b->waiting = 0;
        b->round++;
        pal_cond_broadcast(b->cond);
        r = -1;
    } else {
        while (round == b->round) pal_cond_wait(b->cond, b->lock);
    }
    pal_mutex_unlock(b->lock);
    hle_ret_s32(c, r);
}
static void barrier_destroy_(hle_call *c) {
    host_barrier *b = barrier_at(c);
    if (b) {
        pal_cond_destroy(b->cond);
        pal_mutex_destroy(b->lock);
        free(b);
        uint64_t zero = 0;
        gmem_write(c->mem, hle_arg_ptr(c, 0), &zero, 8);
    }
    hle_ret_s32(c, 0);
}

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(LIB, "pthread_barrier_init", barrier_init_),
    HLE_FUNC_ENTRY(LIB, "pthread_barrier_wait", barrier_wait_),
    HLE_FUNC_ENTRY(LIB, "pthread_barrier_destroy", barrier_destroy_),
    HLE_FUNC_ENTRY(LIB, "flock", ok0_),
    HLE_FUNC_ENTRY(LIB, "sigaltstack", ok0_),
    HLE_FUNC_ENTRY(LIB, "sigemptyset", sigemptyset_),
    HLE_FUNC_ENTRY(LIB, "sigfillset", sigfillset_),
    HLE_FUNC_ENTRY(LIB, "sigaddset", sigaddset_),
    HLE_FUNC_ENTRY(LIB, "sigdelset", sigdelset_),
    HLE_FUNC_ENTRY(LIB, "sigismember", sigismember_),
    HLE_FUNC_ENTRY(LIB, "sigaction", sigaction_),
    HLE_FUNC_ENTRY(LIB, "pthread_sigmask", sigmask_),
    HLE_FUNC_ENTRY(LIB, "pthread_attr_getscope", attr_getscope_),
    HLE_FUNC_ENTRY(LIB, "pthread_attr_setscope", ok0_),
    HLE_FUNC_ENTRY(LIB, "sleep", posix_sleep_),
    HLE_FUNC_ENTRY(LIB, "sysctl", sysctl_),
    HLE_FUNC_ENTRY(LIB, "sysctlbyname", sysctlbyname_),
    HLE_FUNC_ENTRY(LIB, "sceKernelGetExecutableModuleHandle", executable_handle_),
    HLE_FUNC_ENTRY(LIB, "sceKernelGetModuleInfo", get_module_info_),
    HLE_DATA_ENTRY(LIB, "__stack_chk_guard", stack_chk_guard_init),
    HLE_FUNC_ENTRY(LIB, "__stack_chk_fail", stack_chk_fail),
    HLE_FUNC_ENTRY(LIB, "__tls_get_addr", tls_get_addr),
    HLE_FUNC_ENTRY(LIB, "sceKernelGetProcessTime", get_process_time),
    HLE_FUNC_ENTRY(LIB, "sceKernelGetProcessTimeCounter", get_process_time_counter),
    HLE_FUNC_ENTRY(LIB, "sceKernelGetProcessTimeCounterFrequency", get_process_time_counter_frequency),
    HLE_FUNC_ENTRY(LIB, "sceKernelReadTsc", read_tsc),
    HLE_FUNC_ENTRY(LIB, "sceKernelGetTscFrequency", get_tsc_frequency),
    HLE_FUNC_ENTRY(LIB, "sceKernelUsleep", usleep_),
    HLE_FUNC_ENTRY(LIB, "sceKernelSleep", sleep_),
    HLE_FUNC_ENTRY(LIB, "usleep", usleep_),
    HLE_FUNC_ENTRY("libScePosix", "usleep", usleep_),
    HLE_FUNC_ENTRY(LIB, "nanosleep", nanosleep_),
    HLE_FUNC_ENTRY("libScePosix", "nanosleep", nanosleep_),
    HLE_FUNC_ENTRY(LIB, "sceKernelNanosleep", nanosleep_),
    HLE_FUNC_ENTRY(LIB, "clock_gettime", clock_gettime_),
    HLE_FUNC_ENTRY("libScePosix", "clock_gettime", clock_gettime_),
    HLE_FUNC_ENTRY(LIB, "sceKernelClockGettime", kernel_clock_gettime),
    HLE_FUNC_ENTRY(LIB, "clock_getres", clock_getres_),
    HLE_FUNC_ENTRY(LIB, "sceKernelClockGetres", clock_getres_),
    HLE_FUNC_ENTRY("libScePosix", "clock_getres", clock_getres_),
    HLE_FUNC_ENTRY(LIB, "gettimeofday", gettimeofday_),
    HLE_FUNC_ENTRY(LIB, "sceKernelConvertUtcToLocaltime", convert_utc_to_localtime),
    HLE_FUNC_ENTRY(LIB, "sceKernelConvertLocaltimeToUtc", convert_localtime_to_utc),
    HLE_FUNC_ENTRY(LIB, "scePthreadOnce", pthread_once_),
    HLE_FUNC_ENTRY(LIB, "pthread_once", pthread_once_),
    HLE_FUNC_ENTRY("libScePosix", "pthread_once", pthread_once_),
    HLE_FUNC_ENTRY("libScePosix", "gettimeofday", gettimeofday_),
    HLE_FUNC_ENTRY(LIB, "sceKernelGettimeofday", gettimeofday_),
    HLE_FUNC_ENTRY(LIB, "sceKernelDebugOutText", debug_out_text),
    HLE_FUNC_ENTRY(LIB, "sceKernelGetCompiledSdkVersion", get_compiled_sdk_version),
    HLE_FUNC_ENTRY(LIB, "sceKernelIsNeoMode", is_neo_mode),
    /* unnamed: libSceAgc reads the executable's SDK version through it at
     * initialization (a top byte of 4 turns on its SDK 4 title workarounds) */
    HLE_FUNC_ENTRY(LIB, "#GGeRJk1XdWc", get_compiled_sdk_version),
    HLE_FUNC_ENTRY(LIB, "_exit", exit_),
    HLE_FUNC_ENTRY(LIB, "sceKernelGetFsSandboxRandomWord", sandbox_random_word),
    HLE_FUNC_ENTRY(LIB, "sceKernelGetProcParam", get_proc_param),
    HLE_FUNC_ENTRY(LIB, "_sceKernelSetThreadDtors", set_thread_dtors),
    HLE_FUNC_ENTRY(LIB, "_sceKernelSetThreadAtexitCount", set_thread_atexit_count),
    HLE_FUNC_ENTRY(LIB, "_sceKernelSetThreadAtexitReport", set_thread_atexit_report),
    HLE_FUNC_ENTRY(LIB, "sceKernelDebugRaiseException", debug_raise_exception),
    HLE_FUNC_ENTRY(LIB, "sceKernelGetModuleInfoForUnwind", get_module_info_for_unwind),
    HLE_FUNC_ENTRY("libSceSysmodule", "sceSysmoduleGetModuleInfoForUnwind", get_module_info_for_unwind),
    HLE_FUNC_ENTRY(LIB, "sceKernelDlsym", dlsym_),
    HLE_FUNC_ENTRY(LIB, "sceKernelLoadStartModule", load_start_module),
    HLE_FUNC_ENTRY(LIB, "sceKernelUuidCreate", uuid_create),
    HLE_FUNC_ENTRY(LIB, "sceKernelGetMainSocId", get_main_soc_id),
    HLE_FUNC_ENTRY(LIB, "getargc", getargc_),
    HLE_FUNC_ENTRY(LIB, "getargv", getargv_),
    HLE_FUNC_ENTRY(LIB, "getrusage", getrusage_),
    HLE_FUNC_ENTRY(LIB, "signal", signal_),
    HLE_FUNC_ENTRY(LIB, "_nanosleep", nanosleep_),
    HLE_FUNC_ENTRY(LIB, "__pthread_cxa_finalize", kernel_ok),
    HLE_FUNC_ENTRY(LIB, "sceKernelMlock", kernel_ok), /* host memory is never paged out for the guest's sake */
    HLE_FUNC_ENTRY(LIB, "sceKernelSetGPO", kernel_ok),
    HLE_FUNC_ENTRY(LIB, "sceKernelGetAppCategoryType", kernel_ok), /* 0: a game */
    HLE_FUNC_ENTRY(LIB, "getpid", getpid_),
    HLE_FUNC_ENTRY(LIB, "sysconf", sysconf_),
    HLE_FUNC_ENTRY(LIB, "getuid", getpid_),
    HLE_FUNC_ENTRY(LIB, "geteuid", getpid_),
    HLE_FUNC_ENTRY(LIB, "getgid", getpid_),
    HLE_FUNC_ENTRY(LIB, "getegid", getpid_),
    HLE_FUNC_ENTRY(LIB, "_is_signal_return", is_signal_return),
    HLE_FUNC_ENTRY(LIB, "sceKernelInstallExceptionHandler", install_exception_handler),
    HLE_FUNC_ENTRY(LIB, "sceKernelRemoveExceptionHandler", remove_exception_handler),
    HLE_FUNC_ENTRY(LIB, "sceKernelRaiseException", raise_exception),
    HLE_FUNC_ENTRY("libkernel_unity", "sceKernelInstallExceptionHandler", install_exception_handler),
    HLE_FUNC_ENTRY("libkernel_unity", "sceKernelRemoveExceptionHandler", remove_exception_handler),
    HLE_FUNC_ENTRY("libkernel_unity", "sceKernelRaiseException", raise_exception),
    HLE_FUNC_ENTRY("libScePosix", "getpid", getpid_),
    HLE_FUNC_ENTRY("libScePosix", "sysconf", sysconf_),
    HLE_FUNC_ENTRY("libScePosix", "getuid", getpid_),
    HLE_FUNC_ENTRY("libScePosix", "geteuid", getpid_),
    HLE_FUNC_ENTRY("libScePosix", "getgid", getpid_),
    HLE_FUNC_ENTRY("libScePosix", "getegid", getpid_),
    HLE_FUNC_ENTRY(LIB, "sceKernelTitleWorkaroundIsEnabled", title_workaround_is_enabled),
    HLE_FUNC_ENTRY(LIB, "sceKernelError", kernel_error),
    HLE_FUNC_ENTRY(LIB, "sceKernelGetModuleInfoFromAddr", get_module_info_from_addr),
    HLE_FUNC_ENTRY(LIB, "sceKernelGetAppInfo", get_app_info),
    HLE_FUNC_ENTRY(LIB, "sceKernelGetCurrentCpu", get_current_cpu),
    HLE_FUNC_ENTRY(LIB, "_sigprocmask", sigprocmask_),
    HLE_FUNC_ENTRY(LIB, "sigprocmask", sigprocmask_),
    HLE_FUNC_ENTRY("libScePosix", "sigprocmask", sigprocmask_),
    HLE_FUNC_ENTRY(LIB, "sceKernelDebugRaiseExceptionOnReleaseMode", debug_raise_exception_on_release),
    /* thread_local destructors of a module: the module is never unloaded */
    HLE_FUNC_ENTRY(LIB, "_sceKernelRtldThreadAtexitIncrement", kernel_ok),
    HLE_FUNC_ENTRY(LIB, "_sceKernelRtldThreadAtexitDecrement", kernel_ok),
    HLE_FUNC_ENTRY(LIB, "sceKernelIsAddressSanitizerEnabled", kernel_ok), /* 0: no sanitizer */
    HLE_FUNC_ENTRY(LIB, "sysarch", sysarch_),
    HLE_FUNC_ENTRY(LIB, "shm_open", shm_unsupported),
    HLE_FUNC_ENTRY(LIB, "shm_unlink", shm_unsupported),
    HLE_FUNC_ENTRY(LIB, "physhm_open", shm_unsupported),
    HLE_FUNC_ENTRY(LIB, "physhm_unlink", shm_unsupported),
    HLE_FUNC_ENTRY(LIB, "sceKernelIsTrinityMode", kernel_is_trinity_mode),
    HLE_FUNC_ENTRY(LIB, "sceKernelGetOperationMode", kernel_get_operation_mode),
    HLE_FUNC_ENTRY(LIB, "sceKernelGetOpenPsId", get_open_ps_id),
    HLE_FUNC_ENTRY("libSceOpenPsId", "sceKernelGetOpenPsId", get_open_ps_id),
    HLE_FUNC_ENTRY("libkernel_write_throttling", "sceKernelWriteThrottlingStatus", write_throttling_status),
    HLE_FUNC_ENTRY("libScePosix", "fcntl", fcntl_),
    HLE_FUNC_ENTRY("libScePosix", "flock", ok_flock), /* one process: advisory locks always granted */
};

void hle_register_libkernel(void) {
    if (!g_process_start_ns) g_process_start_ns = pal_time_ns();
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}
