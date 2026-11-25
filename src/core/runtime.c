/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/runtime.h"
#include "core/game_fixes.h"
#include "core/process_params.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "gpu/gpu_watch.h"
#include "cpu/x64_compat.h"
#include "hle/equeue.h"
#include "hle/acm.h"
#include "hle/kernel_objects.h"
#include "loader/nid.h"
#include "pal/pal.h"

/* The dispatcher is entered from generated code with no user pointer. There is
 * one emulated process per host process, so a global is the honest choice. */
static ps5_runtime *g_rt;

const loaded_module *runtime_find_module(ps5_runtime *rt, guest_addr addr) {
    for (size_t i = 0; i < rt->ms->modules.len; i++) {
        const loaded_module *m = rt->ms->modules.data[i];
        if (m->mapped && addr >= m->base && addr < m->base + m->map_size) return m;
    }
    return NULL;
}

static void describe_addr(ps5_runtime *rt, guest_addr addr, char *buf, size_t n) {
    const loaded_module *m = runtime_find_module(rt, addr);
    if (m) {
        snprintf(buf, n, "0x%llx (%s+0x%llx)", (unsigned long long)addr, m->name,
                 (unsigned long long)(addr - m->bias));
        return;
    }
    const import_stub *s = linker_find_stub(&rt->lk, addr);
    if (s) {
        char nid[NID_STR_LEN + 1];
        nid_to_str(s->nid, nid);
        snprintf(buf, n, "0x%llx (import stub %s %s)", (unsigned long long)addr, s->library, nid);
        return;
    }
#ifdef _WIN32
    /* the emulator's own code: an offset to look up in the executable's symbols */
    extern char __ImageBase;
    uint64_t host = (uint64_t)(uintptr_t)&__ImageBase;
    if (addr >= host && addr - host < 0x4000000) {
        snprintf(buf, n, "0x%llx (ps5c.exe+0x%llx)", (unsigned long long)addr, (unsigned long long)(addr - host));
        return;
    }
#endif
    char dll[96];
    uint64_t base = 0;
    if (addr >= (1ull << 40) && pal_host_module(addr, dll, sizeof dll, &base)) { /* a host DLL (driver, CRT) */
        snprintf(buf, n, "0x%llx (%s+0x%llx)", (unsigned long long)addr, dll, (unsigned long long)(addr - base));
        return;
    }
    snprintf(buf, n, "0x%llx", (unsigned long long)addr);
}

guest_thread *runtime_current_thread(ps5_runtime *rt) {
    return (guest_thread *)pal_tls_get(rt->thread_slot);
}

void runtime_exit_thread(ps5_runtime *rt, int64_t code) {
    guest_thread *t = runtime_current_thread(rt);
    if (!t) {
        LOG_E(CORE, "exit requested outside a guest thread");
        pal_exit_process((int)code);
    }
    t->exit_requested = true;
    t->thread_exit = false;
    t->exit_code = code;
    x64_native_bail(&rt->cpu, &t->enter, (uint64_t)code);
}

void runtime_exit_current_thread(ps5_runtime *rt, uint64_t retval) {
    guest_thread *t = runtime_current_thread(rt);
    if (!t) return;
    /* the handlers of pthread_cleanup_push still installed, last pushed first;
     * one that exits the thread itself ends the others */
    while (t->cleanup_count) {
        uint32_t i = --t->cleanup_count;
        uint64_t rax;
        runtime_call_nested(rt, t->cleanup[i].fn, t->cleanup[i].arg, 0, 0, &rax);
    }
    t->exit_requested = true;
    t->thread_exit = true;
    t->exit_code = (int64_t)retval;
    x64_native_bail(&rt->cpu, &t->enter, retval);
}

/* ---- pthread_cancel ----
 * Deferred cancellation as POSIX has it: the request only marks the thread,
 * which ends when one of its own calls that is a cancellation point returns
 * (a thread blocked in a wait with no timeout ends once that wait is over). */

/* POSIX's list among the functions implemented in HLE, with the SCE names of
 * the same calls. The sceKernel waits on kernel objects are not points. */
bool runtime_is_cancel_point(const char *name) {
    static const char *const points[] = {
        "pthread_testcancel", "scePthreadTestcancel", "pthread_join", "scePthreadJoin",
        "pthread_cond_wait", "pthread_cond_timedwait", "scePthreadCondWait", "scePthreadCondTimedwait",
        "sem_wait", "sem_timedwait", "sleep", "usleep", "nanosleep", "clock_nanosleep",
        "sceKernelSleep", "sceKernelUsleep", "sceKernelNanosleep",
        "open", "close", "read", "write", "pread", "pwrite", "readv", "writev", "fsync", "fcntl",
        "accept", "connect", "recv", "recvfrom", "recvmsg", "send", "sendto", "sendmsg",
        "select", "poll", "wait", "waitpid", "sigwait", "sigsuspend", "pause",
    };
    if (!name) return false;
    for (size_t i = 0; i < PS5C_ARRAY_COUNT(points); i++)
        if (!strcmp(name, points[i])) return true;
    return false;
}

int runtime_cancel_thread(ps5_runtime *rt, uint64_t handle) {
    int err = 3; /* ESRCH */
    pal_mutex_lock(rt->threads_lock);
    for (size_t i = 0; i < rt->threads.len; i++) {
        guest_thread *t = rt->threads.data[i];
        if ((uint64_t)(uintptr_t)t != handle) continue;
        if (!t->finished) t->cancel_pending = true;
        err = 0;
        break;
    }
    pal_mutex_unlock(rt->threads_lock);
    return err;
}

static void cancel_now(ps5_runtime *rt, guest_thread *t, const char *where) {
    t->cancel_pending = false;
    t->cancel_disabled = true; /* as POSIX: no second cancel while the handlers run */
    LOG_I(CORE, "thread %u '%s' cancelled in %s", t->id, t->name, where);
    runtime_exit_current_thread(rt, 1); /* PTHREAD_CANCELED */
}

/* ---------------------------------------------------------------------------
 * HLE dispatch (called from generated code, host ABI, on the guest stack)
 * ------------------------------------------------------------------------- */

/* Marks `t` as inside an HLE call (host code). Returns the previous marker:
 * HLE functions may call back into guest code, which may call HLE again. */
static const hle_stub_info *hle_enter(guest_thread *t, const hle_stub_info *si, guest_addr ret_addr) {
    if (!t) return NULL;
    const hle_stub_info *prev = t->in_hle;
    t->hle_ret = ret_addr;
    t->in_hle = si;
    return prev;
}

/* Back to guest code: wait while an exception handler stands in for `t`. */
static void hle_leave(guest_thread *t, const hle_stub_info *prev) {
    if (!t) return;
    if (!t->sig_lock) {
        t->in_hle = prev;
        return;
    }
    pal_mutex_lock(t->sig_lock);
    while (t->sig_hold) pal_cond_wait(t->sig_cond, t->sig_lock);
    t->in_hle = prev;
    pal_mutex_unlock(t->sig_lock);
}

/* PS5C_TRACE=<substr>,<substr>,... limits --trace-hle to matching names. */
static bool trace_wanted(const char *name) {
    static const char *filter;
    static bool init;
    if (!init) {
        filter = getenv("PS5C_TRACE");
        init = true;
    }
    if (!filter || !*filter) return true;
    for (const char *p = filter; *p;) {
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        for (const char *h = name; n && *h; h++) {
            if (!strncmp(h, p, n)) return true;
        }
        if (!end) break;
        p = end + 1;
    }
    return false;
}

static void hle_dispatch(hle_args *a) {
    ps5_runtime *rt = g_rt;
    memset(a->ret, 0, sizeof(a->ret));
    memset(a->ret_xmm, 0, sizeof(a->ret_xmm));

    if (a->stub_id >= rt->stubs.len) {
        LOG_E(HLE, "call through unknown stub id %llu", (unsigned long long)a->stub_id);
        runtime_exit_thread(rt, RUNTIME_EXIT_MISSING_IMPORT);
        return;
    }
    hle_stub_info *si = &rt->stubs.data[a->stub_id];
    si->calls++;
    hle_call c = {a, rt, runtime_current_thread(rt), &rt->mem};

    /* The thread counts as inside the call from here on: an exception
     * raised at it now is held until the call returns. The return address
     * is on the thread's own stack, which the call instruction just wrote,
     * so it is read without the memory map lock. */
    const hle_stub_info *prev = hle_enter(c.thread, si, 0);
    uint64_t ret_addr = 0;
    uint64_t fp = 0; /* the entry glue pushed the guest's rbp below the return address */
    const void *ret_slot = gmem_host_unlocked(&rt->mem, a->stack - 16);
    if (ret_slot) {
        memcpy(&fp, ret_slot, 8);
        memcpy(&ret_addr, (const uint8_t *)ret_slot + 8, 8);
    }
    if (c.thread) {
        c.thread->hle_ret = ret_addr;
        c.thread->hle_fp = fp;
        c.thread->hle_arg0 = a->gpr[0];
    }

    if (si->exp) {
        rt->hle_calls++;
        static int trace_env = -1; /* PS5C_TRACE also turns the trace on */
        if (trace_env < 0) trace_env = getenv("PS5C_TRACE") != NULL;
        bool trace_call = (rt->opts.trace_hle || trace_env) && trace_wanted(si->exp->name);
        if (trace_call) {
            char where[128];
            describe_addr(rt, ret_addr, where, sizeof(where));
            LOG_I(HLE, "[T%u] %s(0x%llx, 0x%llx, 0x%llx) from %s", c.thread ? c.thread->id : 0, si->exp->name,
                  (unsigned long long)a->gpr[0], (unsigned long long)a->gpr[1], (unsigned long long)a->gpr[2], where);
        }
        /* read once: getenv takes the C runtime's environment lock, and a guest
         * thread suspended while holding it stalls every other one (DBZ froze) */
        static const char *bt;
        static int bt_read;
        if (!bt_read) bt = getenv("PS5C_BACKTRACE"), bt_read = 1;
        if (bt && strstr(si->exp->name, bt)) runtime_log_backtrace(rt);
        si->exp->fn(&c);
        if (trace_call && getenv("PS5C_TRACE_RETURNS"))
            LOG_I(HLE, "[T%u] %s returned 0x%llx", c.thread ? c.thread->id : 0, si->exp->name,
                  (unsigned long long)a->ret[0]);
        hle_leave(c.thread, prev);
        /* a cancellation point; not inside a callback of another HLE call, whose
         * host frames the exit would cut through */
        if (c.thread && c.thread->cancel_pending && !prev && !c.thread->cancel_disabled &&
            (c.thread->cancel_async || runtime_is_cancel_point(si->exp->name)))
            cancel_now(rt, c.thread, si->exp->name);
        return;
    }

    rt->missing_calls++;
    if (si->calls <= rt->opts.missing_log_limit || rt->opts.stop_on_missing) {
        char nid[NID_STR_LEN + 1], where[128];
        nid_to_str(si->nid, nid);
        describe_addr(rt, ret_addr, where, sizeof(where));
        LOG_W(HLE, "unimplemented %s::%s called from %s (args 0x%llx, 0x%llx, 0x%llx)%s", si->library,
              si->name ? si->name : nid,
              where, (unsigned long long)a->gpr[0], (unsigned long long)a->gpr[1],
              (unsigned long long)a->gpr[2], rt->opts.stop_on_missing ? "" : " -> returning 0");
        /* PS5C_BACKTRACE=<name>: the guest call chain into that unimplemented function (debugging aid) */
        const char *bt = getenv("PS5C_BACKTRACE");
        if (bt && si->name && strstr(si->name, bt)) runtime_log_backtrace(rt);
    }
    if (rt->opts.stop_on_missing) {
        runtime_exit_thread(rt, RUNTIME_EXIT_MISSING_IMPORT);
    }
    hle_leave(c.thread, prev);
}

/* Lists the unimplemented imports the guest actually called, most used first. */
static void report_missing_calls(ps5_runtime *rt) {
    uint32_t count = 0;
    for (size_t i = 0; i < rt->stubs.len; i++) {
        if (!rt->stubs.data[i].exp && rt->stubs.data[i].calls) count++;
    }
    if (!count) return;
    LOG_I(HLE, "%u unimplemented imports were called:", count);
    uint64_t prev_max = UINT64_MAX;
    for (uint32_t printed = 0; printed < count;) {
        uint64_t max = 0;
        for (size_t i = 0; i < rt->stubs.len; i++) {
            const hle_stub_info *si = &rt->stubs.data[i];
            if (!si->exp && si->calls < prev_max && si->calls > max) max = si->calls;
        }
        for (size_t i = 0; i < rt->stubs.len; i++) {
            const hle_stub_info *si = &rt->stubs.data[i];
            if (si->exp || si->calls != max) continue;
            char nid[NID_STR_LEN + 1];
            nid_to_str(si->nid, nid);
            LOG_I(HLE, "  %6llu  %s::%s (%s)", (unsigned long long)si->calls, si->library,
                  si->name ? si->name : "?", nid);
            printed++;
        }
        prev_max = max;
    }
}

/* ---------------------------------------------------------------------------
 * Faults in guest code
 * ------------------------------------------------------------------------- */

/* SSE4a (AMD: the PS5's Zen 2 has it, Intel hosts do not): EXTRQ and INSERTQ,
 * register forms, emulated on the faulting context.
 *   66 [REX] 0F 78 /0 ib ib   EXTRQ   xmm, len, idx
 *   66 [REX] 0F 79 /r         EXTRQ   xmm, xmm   (len = src[5:0], idx = src[13:8])
 *   F2 [REX] 0F 78 /r ib ib   INSERTQ xmm, xmm, len, idx
 *   F2 [REX] 0F 79 /r         INSERTQ xmm, xmm   (len = src[69:64], idx = src[77:72])
 * A length of 0 means 64. The upper quadword of the destination is left as is. */
/* MONITORX (0F 01 FA) and MWAITX (0F 01 FB): AMD's user-mode wait on a cache line, which
 * Intel hosts lack (Wolverine spins with them). Without a monitor, the wait is a short pause:
 * the loop around it reads the line again. The instruction is rewritten in place (a 3-byte
 * NOP, or PAUSE and NOP) so the loop does not fault on every turn. */
static bool emulate_mwaitx(ps5_runtime *rt, pal_fault_info *info) {
    uint8_t *p = (uint8_t *)gmem_host(&rt->mem, info->pc, 3);
    if (!p || p[0] != 0x0F || p[1] != 0x01 || (p[2] != 0xFA && p[2] != 0xFB)) return false;
    static const uint8_t nop3[3] = {0x0F, 0x1F, 0x00}, pause_nop[3] = {0xF3, 0x90, 0x90};
    void *page = (void *)((uintptr_t)p & ~(uintptr_t)(GUEST_PAGE_SIZE - 1));
    pal_vm_protect(page, GUEST_PAGE_SIZE * 2, GPROT_RW | GPROT_EXEC);
    memcpy(p, p[2] == 0xFA ? nop3 : pause_nop, 3);
    pal_vm_protect(page, GUEST_PAGE_SIZE * 2, GPROT_READ | GPROT_EXEC);
    return true; /* runs the rewritten bytes */
}

static bool emulate_sse4a(ps5_runtime *rt, pal_fault_info *info) {
    const uint8_t *p = (const uint8_t *)gmem_host(&rt->mem, info->pc, 16);
    if (!p || (p[0] != 0x66 && p[0] != 0xF2)) return false;
    uint32_t i = 1;
    uint8_t rex = 0;
    if ((p[i] & 0xF0) == 0x40) rex = p[i++];
    if (p[i] != 0x0F || (p[i + 1] != 0x78 && p[i + 1] != 0x79)) return false;
    bool imm = p[i + 1] == 0x78, insert = p[0] == 0xF2;
    uint8_t modrm = p[i + 2];
    if ((modrm >> 6) != 3) return false;
    uint32_t reg = ((modrm >> 3) & 7) | ((rex & 4) ? 8u : 0u), rm = (modrm & 7) | ((rex & 1) ? 8u : 0u);
    uint32_t len, idx, size = i + 3;
    if (imm) {
        len = p[size] & 0x3F;
        idx = p[size + 1] & 0x3F;
        size += 2;
    } else if (insert) {
        len = info->xmm[rm][1] & 0x3F;
        idx = (info->xmm[rm][1] >> 8) & 0x3F;
    } else {
        len = info->xmm[rm][0] & 0x3F;
        idx = (info->xmm[rm][0] >> 8) & 0x3F;
    }
    uint64_t mask = len == 0 ? ~0ull : (1ull << len) - 1;
    if (insert) {
        uint64_t *d = &info->xmm[reg][0];
        *d = (*d & ~(mask << idx)) | ((info->xmm[rm][0] & mask) << idx);
    } else {
        uint32_t dst = imm ? rm : reg; /* EXTRQ imm form: the register is in r/m */
        info->xmm[dst][0] = (info->xmm[dst][0] >> idx) & mask;
    }
    info->pc += size;
    return true;
}

/* PS5C_WATCH_WRITE=<hex addr>: the guest page holding it is made read-only
 * (again after each APR read) and each write to it is logged with the code
 * that made it (debugging aid: who fills a buffer). PS5C_WATCH_EXACT=<bytes>:
 * only accesses to that many bytes from the address are logged, and the page
 * is protected again every millisecond (who changes a state variable).
 * PS5C_WATCH_STRIDE=<hex> + PS5C_WATCH_COUNT=<n>: the same watch at n addresses that
 * far apart (a field of buffers handed out from a pool of equal pages). */
static guest_addr g_watch_page, g_watch_addr;
static uint64_t g_watch_exact, g_watch_stride;
static uint32_t g_watch_count = 1;
static volatile bool g_watch_deferred; /* PS5C_WATCH_DEFER: armed by runtime_watch_move (from a GPU trigger) */
static bool g_watch_reads; /* PS5C_WATCH_READS: reads too (the page is made inaccessible) */

static void break_install(gmem *m);

static int32_t watch_rearm_main(void *arg) {
    for (;;) {
        pal_sleep_ns(1000000);
        runtime_watch_arm((ps5_runtime *)arg);
    }
    return 0;
}

void runtime_watch_arm(ps5_runtime *rt) {
    static int init;
    if (!init) {
        const char *e = getenv("PS5C_WATCH_WRITE");
        g_watch_addr = e ? strtoull(e, NULL, 16) : 0;
        g_watch_page = g_watch_addr & ~(GUEST_PAGE_SIZE - 1);
        g_watch_reads = getenv("PS5C_WATCH_READS") != NULL;
        g_watch_exact = getenv("PS5C_WATCH_EXACT") ? strtoull(getenv("PS5C_WATCH_EXACT"), NULL, 0) : 0;
        g_watch_stride = getenv("PS5C_WATCH_STRIDE") ? strtoull(getenv("PS5C_WATCH_STRIDE"), NULL, 16) : 0;
        if (g_watch_stride && getenv("PS5C_WATCH_COUNT")) g_watch_count = (uint32_t)atoi(getenv("PS5C_WATCH_COUNT"));
        if (g_watch_count < 1 || g_watch_count > 256) g_watch_count = 1;
        g_watch_deferred = getenv("PS5C_WATCH_DEFER") != NULL;
        if (!getenv("PS5C_BREAK_DEFER") && !getenv("PS5C_BREAK_AFTER")) break_install(&rt->mem); /* else later */
        init = 1;
        static pal_thread *rearm;
        if (g_watch_page && g_watch_exact) pal_thread_create(&rearm, watch_rearm_main, rt, "watch", 0);
    }
    static uint64_t break_after; /* PS5C_BREAK_AFTER=<seconds>: the breakpoint goes in at the first call after that */
    if (!break_after && getenv("PS5C_BREAK_AFTER")) break_after = pal_time_ns() + strtoull(getenv("PS5C_BREAK_AFTER"), NULL, 10) * 1000000000ull;
    if (break_after && break_after != UINT64_MAX && pal_time_ns() >= break_after) {
        break_after = UINT64_MAX;
        break_install(&rt->mem);
    }
    for (uint32_t k = 0; g_watch_page && !g_watch_deferred && k < g_watch_count; k++) {
        guest_addr a = (g_watch_addr + k * g_watch_stride) & ~(GUEST_PAGE_SIZE - 1);
        void *page = gmem_host_unlocked(&rt->mem, a);
        if (page) pal_vm_protect(page, GUEST_PAGE_SIZE, g_watch_reads ? GPROT_NONE : GPROT_READ);
    }
}

/* Moves an armed watch (PS5C_WATCH_WRITE) to another address: a buffer the GPU side
 * finds only at run time (PS5C_LOG_SITES_WATCH). */
void runtime_watch_move(gmem *m, guest_addr addr) {
    if (getenv("PS5C_BREAK_DEFER")) break_install(m);
    if (!g_watch_page) return;
    if (g_watch_deferred) { /* PS5C_WATCH_DEFER: the watch as configured starts now */
        g_watch_deferred = false;
        LOG_I(CPU, "watch armed");
        return;
    }
    void *old = gmem_host_unlocked(m, g_watch_page);
    if (old) pal_vm_protect(old, GUEST_PAGE_SIZE, GPROT_RW);
    g_watch_addr = addr;
    g_watch_page = addr & ~(GUEST_PAGE_SIZE - 1);
    if (old) pal_vm_protect(old, GUEST_PAGE_SIZE, GPROT_RW);
    LOG_I(CPU, "watch moved to 0x%llx", (unsigned long long)addr);
}

/* The page a thread's watched access unprotected: protected again after that one instruction
 * (single step), so no access slips through while it is open (Windows; elsewhere the re-arm
 * thread does it within a millisecond). */
static _Thread_local guest_addr t_watch_reprotect;

/* PS5C_BREAK=<hex pc>: an int3 there logs the registers each time the code runs (debugging
 * aid; the original byte runs in a single step, then the int3 goes back). PS5C_BREAK_MATCH=
 * <reg>,<hex offset>,<hex u16>: only when the 16 bits at that register + offset match, and
 * then the 16 bytes from register + offset - 4 too. */
static guest_addr g_break_pc;
/* PS5C_BREAK_PUBLISH=<reg>: the break publishes that register's value (runtime_debug_addr) */
static volatile guest_addr g_debug_addr;
guest_addr runtime_debug_addr(void) { return g_debug_addr; }
static uint8_t g_break_orig;
static _Thread_local bool t_break_step;

static void break_poke(gmem *m, uint8_t byte) {
    uint8_t *code = (uint8_t *)gmem_host_unlocked(m, g_break_pc);
    if (!code) return;
    static bool writable; /* stays writable: threads poke it concurrently */
    if (!writable) {
        void *page = (void *)((uintptr_t)code & ~(uintptr_t)(GUEST_PAGE_SIZE - 1));
        pal_vm_protect(page, GUEST_PAGE_SIZE * 2, GPROT_RW | GPROT_EXEC);
        writable = true;
    }
    *(volatile uint8_t *)code = byte;
}

static void break_install(gmem *m) {
    const char *e = getenv("PS5C_BREAK");
    if (!e || g_break_pc) return;
    g_break_pc = strtoull(e, NULL, 16);
    const uint8_t *code = (const uint8_t *)gmem_host_unlocked(m, g_break_pc);
    if (!code) {
        g_break_pc = 0;
        return;
    }
    g_break_orig = *code;
    break_poke(m, 0xCC);
    LOG_I(CPU, "breakpoint at 0x%llx (byte %02x)", (unsigned long long)g_break_pc, g_break_orig);
}

static bool break_hit(ps5_runtime *rt, pal_fault_info *info) {
    if (info->kind == PAL_FAULT_SINGLE_STEP && t_break_step) {
        t_break_step = false;
        break_poke(&rt->mem, 0xCC);
        return true;
    }
    if (!g_break_pc || info->kind != PAL_FAULT_BREAKPOINT) return false;
    if (info->pc == g_break_pc + 1) info->pc = g_break_pc; /* reported after the int3 */
    if (info->pc != g_break_pc) return false;
    static int have = -1;
    static uint32_t reg, off, val, logged;
    if (have < 0) {
        const char *m = getenv("PS5C_BREAK_MATCH");
        have = m != NULL;
        if (m) {
            char *e;
            reg = (uint32_t)strtoul(m, &e, 10) & 15;
            if (*e == ',') off = (uint32_t)strtoul(e + 1, &e, 16);
            if (*e == ',') val = (uint32_t)strtoul(e + 1, NULL, 16);
        }
    }
    const uint16_t *at = have ? (const uint16_t *)gmem_host(&rt->mem, info->gpr[reg] + off - 4, 16) : NULL;
    static uint32_t max_logs; /* PS5C_BREAK_MAX=<n> (200) */
    if (!max_logs) max_logs = getenv("PS5C_BREAK_MAX") ? (uint32_t)atoi(getenv("PS5C_BREAK_MAX")) : 200;
    if ((!have || (at && at[2] == val)) && logged < max_logs) {
        logged++;
        guest_thread *t = runtime_current_thread(rt);
        LOG_I(CPU, "break 0x%llx (thread '%s'): rax %llx rcx %llx rdx %llx rbx %llx rsi %llx rdi %llx r8 %llx r9 %llx r13 %llx",
              (unsigned long long)info->pc, t ? t->name : "?", (unsigned long long)info->gpr[0],
              (unsigned long long)info->gpr[1], (unsigned long long)info->gpr[2], (unsigned long long)info->gpr[3],
              (unsigned long long)info->gpr[6], (unsigned long long)info->gpr[7], (unsigned long long)info->gpr[8],
              (unsigned long long)info->gpr[9], (unsigned long long)info->gpr[13]);
        if (at)
            LOG_I(CPU, "break   [reg%+d]: %04x %04x %04x %04x %04x %04x %04x %04x", (int)off - 4, at[0], at[1], at[2], at[3],
                  at[4], at[5], at[6], at[7]);
        const char *dump = getenv("PS5C_BREAK_DUMP"); /* <reg>[+<hex>]: the 36 dwords it (plus the offset) points to */
        const char *plus = dump ? strchr(dump, '+') : NULL;
        const uint32_t *dv = dump ? (const uint32_t *)gmem_host(&rt->mem, info->gpr[atoi(dump) & 15] +
                                                                    (plus ? strtoull(plus + 1, NULL, 16) : 0), 144) : NULL;
        for (int k = 0; dv && k < 36; k += 12)
            LOG_I(CPU, "break     %08x %08x %08x %08x  %08x %08x %08x %08x  %08x %08x %08x %08x", dv[k], dv[k + 1], dv[k + 2],
                  dv[k + 3], dv[k + 4], dv[k + 5], dv[k + 6], dv[k + 7], dv[k + 8], dv[k + 9], dv[k + 10], dv[k + 11]);
        /* PS5C_BREAK_ADDR=<hex>: the 36 dwords at a fixed address */
        const char *fixed = getenv("PS5C_BREAK_ADDR");
        const uint32_t *fv = fixed ? (const uint32_t *)gmem_host(&rt->mem, strtoull(fixed, NULL, 16), 144) : NULL;
        for (int k = 0; fv && k < 36; k += 12)
            LOG_I(CPU, "break  @  %08x %08x %08x %08x  %08x %08x %08x %08x  %08x %08x %08x %08x", fv[k], fv[k + 1], fv[k + 2],
                  fv[k + 3], fv[k + 4], fv[k + 5], fv[k + 6], fv[k + 7], fv[k + 8], fv[k + 9], fv[k + 10], fv[k + 11]);
        /* PS5C_BREAK_DEREF=<reg>[+<reg>]: the qword there and the 36 dwords it points to */
        const char *d = getenv("PS5C_BREAK_DEREF");
        if (d) {
            char *e;
            uint32_t r1 = (uint32_t)strtoul(d, &e, 10) & 15, r2 = *e == '+' ? (uint32_t)strtoul(e + 1, NULL, 10) & 15 : 16;
            guest_addr a = info->gpr[r1] + (r2 < 16 ? info->gpr[r2] : 0);
            const uint64_t *q = (const uint64_t *)gmem_host(&rt->mem, a, 16);
            const uint32_t *v = q ? (const uint32_t *)gmem_host(&rt->mem, q[0], 144) : NULL;
            if (q) LOG_I(CPU, "break   [0x%llx] = %llx %llx", (unsigned long long)a, (unsigned long long)q[0], (unsigned long long)q[1]);
            for (int k = 0; v && k < 36; k += 12)
                LOG_I(CPU, "break     %08x %08x %08x %08x  %08x %08x %08x %08x  %08x %08x %08x %08x", v[k], v[k + 1], v[k + 2],
                      v[k + 3], v[k + 4], v[k + 5], v[k + 6], v[k + 7], v[k + 8], v[k + 9], v[k + 10], v[k + 11]);
        }
    }
    if (getenv("PS5C_BREAK_PUBLISH")) g_debug_addr = info->gpr[atoi(getenv("PS5C_BREAK_PUBLISH")) & 15];
    break_poke(&rt->mem, g_break_orig);
    t_break_step = true;
    info->single_step = true;
    return true;
}

static bool watch_hit(ps5_runtime *rt, pal_fault_info *info) {
    if (info->kind == PAL_FAULT_SINGLE_STEP && t_watch_reprotect) {
        bool still = false; /* the watch may have moved meanwhile (PS5C_WATCH_FOLLOW) */
        for (uint32_t k = 0; !still && k < g_watch_count; k++)
            still = ((g_watch_addr + k * g_watch_stride) & ~(GUEST_PAGE_SIZE - 1)) == t_watch_reprotect;
        void *page = still ? gmem_host_unlocked(&rt->mem, t_watch_reprotect) : NULL;
        if (page) pal_vm_protect(page, GUEST_PAGE_SIZE, g_watch_reads ? GPROT_NONE : GPROT_READ);
        t_watch_reprotect = 0;
        return true;
    }
    if (!g_watch_page || info->kind != PAL_FAULT_ACCESS || (!info->write && !g_watch_reads)) return false;
    uint32_t k = 0;
    while (k < g_watch_count &&
           info->addr - ((g_watch_addr + k * g_watch_stride) & ~(GUEST_PAGE_SIZE - 1)) >= GUEST_PAGE_SIZE)
        k++;
    if (k == g_watch_count) return false;
    guest_addr watched = g_watch_addr + k * g_watch_stride, page = watched & ~(GUEST_PAGE_SIZE - 1);
    if (g_watch_exact && info->addr - watched >= g_watch_exact) {
        pal_vm_protect(gmem_host_unlocked(&rt->mem, page), GUEST_PAGE_SIZE, GPROT_RW);
        t_watch_reprotect = page; /* another variable on the page: protected again after it */
        info->single_step = true;
        return true;
    }
    char where[160], caller[160];
    describe_addr(rt, info->pc, where, sizeof(where));
    uint64_t ret = 0;
    const void *top = gmem_host(&rt->mem, info->rsp, 8);
    if (top) memcpy(&ret, top, 8);
    describe_addr(rt, ret, caller, sizeof(caller));
    guest_thread *t = runtime_current_thread(rt);
    LOG_I(CPU, "watch: %s 0x%llx by %s (thread %u '%s', stack 0x%llx+0x%llx; rsp %llx rbp %llx; [rsp] %s; rsi %llx rdi %llx rcx %llx)",
          info->write ? "write to" : "read of", (unsigned long long)info->addr, where, t ? t->id : 0, t ? t->name : "?",
          (unsigned long long)(t ? t->stack_base : 0), (unsigned long long)(t ? t->stack_size : 0),
          (unsigned long long)info->rsp, (unsigned long long)info->gpr[5], caller, (unsigned long long)info->gpr[6],
          (unsigned long long)info->gpr[7], (unsigned long long)info->gpr[1]);
    static uint32_t reports; /* registers and stack for the first ones only */
    for (int i = 0; reports < 100 && i < 16; i += 4)
        LOG_I(CPU, "watch:   %016llx %016llx %016llx %016llx", (unsigned long long)info->gpr[i],
              (unsigned long long)info->gpr[i + 1], (unsigned long long)info->gpr[i + 2],
              (unsigned long long)info->gpr[i + 3]);
    const uint64_t *st = (const uint64_t *)gmem_host(&rt->mem, info->rsp, 0x200);
    for (int i = 0; st && reports < 100 && i < 0x40; i += 4)
        LOG_I(CPU, "watch:   [rsp+%03x] %016llx %016llx %016llx %016llx", i * 8, (unsigned long long)st[i],
              (unsigned long long)st[i + 1], (unsigned long long)st[i + 2], (unsigned long long)st[i + 3]);
    reports++;
    pal_vm_protect(gmem_host_unlocked(&rt->mem, page), GUEST_PAGE_SIZE, GPROT_RW);
    /* PS5C_WATCH_FOLLOW=<src reg>:<dst reg>[@<hex pc>],...: a copy into the watched bytes moves
     * the watch to the same bytes of its source (src + watched - dst; registers numbered rax=0
     * .. r15=15, 6:7 for memcpy's rsi/rdi), one stage after the other: who fills the data the
     * copy carries */
    static uint32_t stage;
    const char *follow = getenv("PS5C_WATCH_FOLLOW");
    for (uint32_t n = 0; follow && *follow && n < stage; n++) follow = strchr(follow, ',') ? strchr(follow, ',') + 1 : "";
    uint32_t sreg = 0, dreg = 0;
    uint64_t want_pc = 0, sadd = 0;
    if (follow && *follow) {
        char *e;
        sreg = (uint32_t)strtoul(follow, &e, 10) & 15;
        if (*e == '+') sadd = strtoull(e + 1, &e, 16); /* <src reg>+<hex>: the source is that far on */
        if (*e == ':') dreg = (uint32_t)strtoul(e + 1, &e, 10) & 15;
        if (*e == '@') want_pc = strtoull(e + 1, NULL, 16);
    }
    uint64_t rsi = info->gpr[sreg], rdi = info->gpr[dreg];
    if (follow && *follow && (!want_pc || info->pc == want_pc) && rsi && rdi <= watched && watched - rdi < 0x10000) {
        stage++;
        for (uint32_t j = 0; j < g_watch_count; j++)
            pal_vm_protect(gmem_host_unlocked(&rt->mem, (g_watch_addr + j * g_watch_stride) & ~(GUEST_PAGE_SIZE - 1)),
                           GUEST_PAGE_SIZE, GPROT_RW);
        g_watch_count = 1;
        g_watch_stride = 0;
        g_watch_addr = rsi + sadd + (watched - rdi);
        g_watch_page = g_watch_addr & ~(GUEST_PAGE_SIZE - 1);
        LOG_I(CPU, "watch follows the copy's source (stage %u): 0x%llx", stage, (unsigned long long)g_watch_addr);
        return true; /* the pages are open: no single step needed */
    }
    t_watch_reprotect = page;
    info->single_step = true;
    return true;
}

/* The OS is about to write into guest memory (file reads): pages the GPU caches watch first. */
static void watch_prepare_write(void *buf, uint64_t size) { gpu_watch_prepare_write((guest_addr)(uintptr_t)buf, size); }

/* gmem_set_world_hooks, with the map lock held: every other guest thread suspended, its registers
 * read back so that the suspension has taken effect. threads_lock stays held until world_resume,
 * so no thread starts or ends meanwhile. A thread holding threads_lock may wait for the map lock:
 * after ~50 ms of trying, the release goes ahead without stopping anyone (as before). */
static struct { uintptr_t *handles; size_t len, cap; uint64_t stops, skipped, ns; } g_world;
static bool world_stop(void *user) {
    ps5_runtime *rt = (ps5_runtime *)user;
    uint64_t t0 = pal_time_ns();
    while (!pal_mutex_trylock(rt->threads_lock)) {
        if (pal_time_ns() - t0 > 50000000ull) {
            if (g_world.skipped++ < 8) /* nobody stands still yet: logging is safe */
                LOG_W(CORE, "copy-on-write release: the thread list stayed busy, guest threads keep running");
            return false;
        }
        pal_thread_yield();
    }
    if (g_world.cap < rt->threads.len) { /* allocated before anyone stands still (heap lock) */
        uintptr_t *h = (uintptr_t *)realloc(g_world.handles, rt->threads.len * 2 * sizeof(*h));
        if (!h) {
            pal_mutex_unlock(rt->threads_lock);
            g_world.skipped++;
            return false;
        }
        g_world.handles = h, g_world.cap = rt->threads.len * 2;
    }
    guest_thread *self = (guest_thread *)pal_tls_get(rt->thread_slot);
    for (;;) {
        g_world.len = 0;
        for (size_t i = 0; i < rt->threads.len; i++) {
            guest_thread *t = rt->threads.data[i];
            if (t == self || t == guest_thread_identity(self) || t->finished || !t->sample_handle) continue;
            if (!pal_thread_suspend(t->sample_handle)) continue;
            pal_cpu_regs regs;
            pal_thread_get_regs(t->sample_handle, &regs); /* SuspendThread is asynchronous */
            g_world.handles[g_world.len++] = t->sample_handle;
        }
        /* A thread stopped inside the tracker (a lock of it held) would leave the release
         * spinning: let everyone go and try again; after ~50 ms, release without stopping. */
        if (gpu_watch_locks_free()) break;
        for (size_t i = 0; i < g_world.len; i++) pal_thread_resume(g_world.handles[i]);
        g_world.len = 0;
        if (pal_time_ns() - t0 > 50000000ull) {
            pal_mutex_unlock(rt->threads_lock);
            g_world.skipped++;
            return false;
        }
        pal_thread_yield();
    }
    g_world.stops++;
    g_world.ns += pal_time_ns() - t0;
    return true;
}

static void world_resume(void *user) {
    ps5_runtime *rt = (ps5_runtime *)user;
    for (size_t i = 0; i < g_world.len; i++) pal_thread_resume(g_world.handles[i]);
    g_world.len = 0;
    pal_mutex_unlock(rt->threads_lock);
    uint64_t n = g_world.stops;
    if (!(n & (n - 1)) && n >= 4) /* 4, 8, 16, ... */
        LOG_I(CORE, "copy-on-write releases: guest threads stopped %llu times, %llu us each on average",
              (unsigned long long)n, (unsigned long long)(g_world.ns / n / 1000));
}

bool runtime_longshot_commit(ps5_runtime *rt, guest_addr addr, uint64_t size) {
    static _Thread_local bool committing;
    const uint64_t granule = 0x10000, max_commit = 32ull << 20;
    if (!rt || !rt->longshot_allocator || committing || !size ||
        addr < LONGSHOT_WINDOW_START + 0x100000 || addr >= LONGSHOT_WINDOW_END ||
        size > LONGSHOT_WINDOW_END - addr)
        return false;
    const guest_addr end = PS5C_ALIGN_UP(addr + size, granule);
    for (guest_addr cur = PS5C_ALIGN_DOWN(addr, granule); cur < end;) {
        gmem_vma v;
        if (!gmem_query_vma(&rt->mem, cur, false, &v)) return false;
        guest_addr next = PS5C_MIN(end, v.start + v.size);
        if (v.reserved) {
            next = PS5C_MIN(next, cur + max_commit);
            /* Keep Longshot's direct-memory bookkeeping in sync. It
             * deliberately leaves large regions unbacked for first touch. */
            if (next % granule) return false;
            uint64_t actual = 0;
            committing = true;
            bool ok = runtime_call_nested4(rt, rt->longshot_allocator, cur, next - cur, 0x1000, 4, &actual);
            committing = false;
            if (!ok || actual != cur || !gmem_is_mapped(&rt->mem, cur, next - cur)) return false;
        } else if (!(v.prot & GPROT_WRITE)) {
            return false;
        }
        cur = next;
    }
    return true;
}

static bool fault_handler(void *user, pal_fault_info *info) {
    ps5_runtime *rt = (ps5_runtime *)user;
#ifdef _WIN32
    /* Native libraries may handle first-chance arithmetic exceptions themselves.
     * Let Windows continue dispatching them outside the guest reservation. */
    if (info->kind == PAL_FAULT_DIVIDE && rt->mem.identity &&
        (info->pc < rt->mem.res_start || info->pc >= rt->mem.res_end))
        return false;
#endif
    /* a write to a page the GPU caches watch (gpu_watch.h): noted, made writable, retried */
    if (info->kind == PAL_FAULT_ACCESS && rt->mem.identity && gpu_watch_fault(info->addr, info->write)) return true;
    if (break_hit(rt, info)) return true;
    if (watch_hit(rt, info)) return true;
    if (info->kind == PAL_FAULT_ILLEGAL_INSTRUCTION && emulate_mwaitx(rt, info)) return true;
    if (info->kind == PAL_FAULT_ILLEGAL_INSTRUCTION && x64_compat_fault(info)) return true;
    if (info->kind == PAL_FAULT_ILLEGAL_INSTRUCTION && emulate_sse4a(rt, info)) {
        static bool once;
        if (!once) {
            once = true;
            LOG_I(CPU, "emulating SSE4a instructions (the host CPU does not have them)");
        }
        return true;
    }
    if (info->kind == PAL_FAULT_ACCESS &&
        gmem_commit_fault(&rt->mem, info->addr, info->execute ? GPROT_EXEC : info->write ? GPROT_WRITE : GPROT_READ))
        return true;
    if (info->kind == PAL_FAULT_ACCESS && gmem_retry_fault(&rt->mem, info->addr, info->write)) {
        static uint32_t retried;
        if (retried++ < 4) LOG_I(CPU, "access to 0x%llx during a remap: retried", (unsigned long long)info->addr);
        /* the same access retried without end (a page the mapping allows but the host does not): its state, once */
        static _Thread_local guest_addr same_at;
        static _Thread_local uint32_t same_count;
        if (same_at != info->addr) same_at = info->addr, same_count = 0;
        if (++same_count == 5000) {
            LOG_W(CPU, "access to 0x%llx (%s, pc 0x%llx) retried 5000 times", (unsigned long long)info->addr,
                  info->write ? "write" : "read", (unsigned long long)info->pc);
            gmem_log_fault_state(&rt->mem, info->addr);
            pal_log_host_page(info->addr);
            gpu_watch_log_page(info->addr);
        }
        return true;
    }
    if (info->kind == PAL_FAULT_ACCESS && !info->execute && rt->longshot_allocator &&
        info->addr >= LONGSHOT_WINDOW_START && info->addr < LONGSHOT_WINDOW_END &&
        runtime_find_module(rt, info->pc) == module_set_main(rt->ms)) {
        gmem_vma v;
        if (gmem_query_vma(&rt->mem, info->addr, false, &v) && v.reserved &&
            runtime_longshot_commit(rt, info->addr, 1))
            return true;
    }
    /* One report: the process ends after it, and threads faulting meanwhile
     * (often because of the first fault) would interleave their lines. */
    static atomic_flag reporting = ATOMIC_FLAG_INIT;
    if (atomic_flag_test_and_set(&reporting)) {
        for (;;) pal_sleep_ns(1000000000ull);
    }
    static const char *const kinds[] = {"access violation", "illegal instruction", "breakpoint",
                                        "divide error", "exception", "single step"};
    char where[160];
    describe_addr(rt, info->pc, where, sizeof(where));
    guest_thread *ft = runtime_current_thread(rt);
    LOG_E(CPU, "guest %s at %s (thread %u '%s')", kinds[info->kind], where, ft ? ft->id : 0, ft ? ft->name : "?");
    if (info->kind == PAL_FAULT_ACCESS) {
        char target[160];
        describe_addr(rt, info->addr, target, sizeof(target));
        LOG_E(CPU, "  %s address %s", info->write ? "writing" : "reading", target);
    }
    static const char *const names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                          "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
    for (int i = 0; i < 16; i += 4) {
        LOG_E(CPU, "  %-3s %016llx  %-3s %016llx  %-3s %016llx  %-3s %016llx", names[i],
              (unsigned long long)info->gpr[i], names[i + 1], (unsigned long long)info->gpr[i + 1],
              names[i + 2], (unsigned long long)info->gpr[i + 2], names[i + 3],
              (unsigned long long)info->gpr[i + 3]);
    }
    if (!ft) {
        /* a host thread (the GPU worker, a driver's): return addresses in the executable on
         * its stack, for tools/host_stack.py */
        pal_stack_bounds b;
        pal_thread_get_stack_bounds(&b);
        uint64_t sp = info->gpr[4];
        if (sp >= b.low && sp < b.high) {
            uintptr_t self = (uintptr_t)fault_handler;
            char line[700];
            int n = snprintf(line, sizeof(line), "    host stack:");
            for (uint64_t a = sp & ~7ull; a + 8 <= b.high && a < sp + (1u << 20) && n < 680; a += 8) {
                uint64_t v = *(const uint64_t *)(uintptr_t)a;
                if (v + 0x4000000u > self && v < self + 0x4000000u) n += snprintf(line + n, sizeof(line) - (size_t)n, " %llx", (unsigned long long)v);
            }
            LOG_E(CPU, "  host thread (fault_handler at %p)", (void *)self);
            LOG_E(CPU, "%s", line);
        }
    }
    const uint8_t *code = (const uint8_t *)gmem_host(&rt->mem, info->pc, 16);
    if (code) {
        char hex[64];
        for (int i = 0; i < 16; i++) snprintf(hex + i * 3, sizeof(hex) - (size_t)i * 3, "%02X ", code[i]);
        LOG_E(CPU, "  code: %s", hex);
    }
    if (getenv("PS5C_FAULT_CODE")) { /* 1 KiB of code before the fault to user/fault_<pc - 0x400>.bin (debugging aid) */
        const uint8_t *before = (const uint8_t *)gmem_host(&rt->mem, info->pc - 0x400, 0x440);
        char path[64];
        snprintf(path, sizeof(path), "user/fault_%llx.bin", (unsigned long long)(info->pc - 0x400));
        pal_file *f;
        if (before && pal_file_open(path, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE, &f) == PS5C_OK) {
            pal_file_write(f, before, 0x440, NULL);
            pal_file_close(f);
        }
        /* PS5C_FAULT_DUMP_AT=<address>: 1 KiB there too, to user/dump_<address>.bin */
        const char *da = getenv("PS5C_FAULT_DUMP_AT"); /* an address, or r<n>[+hex] (a register, 0 = rax .. 15 = r15) */
        guest_addr at = 0;
        if (da && da[0] == 'r') {
            char *e;
            unsigned long n = strtoul(da + 1, &e, 10);
            if (n < 16) at = info->gpr[n] + (*e == '+' ? strtoull(e + 1, NULL, 16) : 0);
        } else if (da) {
            at = strtoull(da, NULL, 16);
        }
        const uint8_t *mem_at = at ? (const uint8_t *)gmem_host(&rt->mem, at, 0x400) : NULL;
        snprintf(path, sizeof(path), "user/dump_%llx.bin", (unsigned long long)at);
        if (mem_at && pal_file_open(path, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE, &f) == PS5C_OK) {
            pal_file_write(f, mem_at, 0x400, NULL);
            pal_file_close(f);
        }
        /* PS5C_FAULT_PEEK=<object>: its first qword (a vtable) and the 16 slots there, named */
        const char *pk = getenv("PS5C_FAULT_PEEK");
        uint64_t obj = pk ? strtoull(pk, NULL, 16) : 0, vt = 0;
        if (obj && gmem_read_u64(&rt->mem, obj, &vt)) {
            for (uint32_t k = 0; k < 16; k++) {
                uint64_t fn = 0;
                if (!gmem_read_u64(&rt->mem, vt + k * 8, &fn)) break;
                describe_addr(rt, fn, where, sizeof(where));
                LOG_E(CPU, "  vtable 0x%llx [+0x%x] = %s", (unsigned long long)vt, k * 8, where);
            }
        }
    }
    uint64_t ret = 0;
    if (gmem_read_u64(&rt->mem, info->rsp, &ret)) {
        describe_addr(rt, ret, where, sizeof(where));
        LOG_E(CPU, "  [rsp] = %s", where);
    }
    /* Frame-pointer backtrace: guest code is built with rbp frames. */
    uint64_t fp = info->gpr[5];
    for (int depth = 0; depth < 16 && fp; depth++) {
        uint64_t next = 0, ret_addr = 0;
        if (!gmem_read_u64(&rt->mem, fp, &next) || !gmem_read_u64(&rt->mem, fp + 8, &ret_addr) || !ret_addr) break;
        describe_addr(rt, ret_addr, where, sizeof(where));
        LOG_E(CPU, "  #%-2d %s", depth, where);
        if (next <= fp) break;
        fp = next;
    }
    /* Capture the original instruction and registers before the potentially
     * long map history: another guest thread can abort the process meanwhile. */
    if (info->kind == PAL_FAULT_ACCESS) gmem_log_fault_state(&rt->mem, info->addr);
    report_missing_calls(rt);
    pal_exit_process(3);
    return false;
}

/* ---------------------------------------------------------------------------
 * Setup
 * ------------------------------------------------------------------------- */

static void on_relocated(void *user, loaded_module *m) {
    ps5_runtime *rt = (ps5_runtime *)user;
    const sce_image *img = &m->img;
    for (uint32_t i = 0; i < img->segment_count; i++) {
        const elf64_phdr *ph = &img->segments[i].h;
        if ((ph->p_type == PT_LOAD || ph->p_type == PT_SCE_RELRO) && (ph->p_flags & PF_X) && ph->p_filesz) {
            rt->tls_patches += x64_native_patch_tls(&rt->cpu, m->bias + ph->p_vaddr, ph->p_filesz);
            guest_addr first = 0;
            uint32_t other = x64_native_count_fs(&rt->cpu, m->bias + ph->p_vaddr, ph->p_filesz, &first);
            if (other)
                LOG_I(CPU, "%s: %u fs:[disp] accesses left unpatched, first at 0x%llx", m->name, other,
                      (unsigned long long)first);
            x64_compat_patch(&rt->mem, m->name, m->bias + ph->p_vaddr, ph->p_filesz, m->base + m->map_size);
        }
    }
    guest_addr longshot_allocator = game_longshot_allocator_rva(m);
    if (longshot_allocator) {
        rt->longshot_allocator = m->bias + longshot_allocator;
        LOG_I(CORE, "Longshot: backing reserved pages through the guest allocator");
    }
    game_fixes_apply(&rt->mem, m);
}

ps5c_err runtime_init(ps5_runtime *rt, module_set *ms, const runtime_options *opts) {
    memset(rt, 0, sizeof(*rt));
    rt->ms = ms;
    rt->opts = *opts;
    if (rt->opts.missing_log_limit == 0) rt->opts.missing_log_limit = 3;
    rt->next_thread_id = 1;

    ps5c_err err = hle_registry_init();
    if (err != PS5C_OK) return err;
    ms->hle_query = hle_query;

    gmem_config cfg = gmem_default_config();
    if ((err = gmem_init(&rt->mem, &cfg)) != PS5C_OK) return err;
    if (rt->mem.identity && gpu_watch_enabled()) { /* PS5C_GPU_WATCH: see gpu_watch.h */
        gmem_set_change_hook(gpu_watch_mapping_changed);
        pal_set_write_prepare(watch_prepare_write);
        gmem_set_world_hooks(world_stop, world_resume, rt);
        gpu_watch_start(&rt->mem);
    }
    if ((err = pal_tls_alloc(&rt->tcb_slot)) != PS5C_OK) return err;
    if ((err = pal_tls_alloc(&rt->thread_slot)) != PS5C_OK) return err;
    uint32_t gs_off = pal_tls_slot_gs_offset(rt->tcb_slot);
    if (gs_off == 0) {
        LOG_E(CPU, "host TLS slot %u can't be addressed from generated code", rt->tcb_slot);
        return PS5C_ERR_UNSUPPORTED;
    }
    if ((err = linker_init(&rt->lk, &rt->mem, ms)) != PS5C_OK) return err;
    rt->lk.on_relocated = on_relocated;
    rt->lk.hook_user = rt;
    if ((err = x64_native_init(&rt->cpu, &rt->mem, rt->lk.import_code, hle_dispatch, gs_off)) != PS5C_OK) {
        return err;
    }
    if ((err = pal_mutex_create(&rt->threads_lock)) != PS5C_OK) return err;
    if ((err = pal_cond_create(&rt->threads_changed)) != PS5C_OK) return err;
    g_rt = rt;
    return pal_fault_handler_install(fault_handler, rt);
}

void runtime_free(ps5_runtime *rt) {
    if (g_rt == rt) {
        pal_fault_handler_install(NULL, NULL);
        g_rt = NULL;
    }
    dyn_free(&rt->stubs);
    for (size_t i = 0; i < rt->threads.len; i++) free(rt->threads.data[i]);
    dyn_free(&rt->threads);
    hle_playgo_reset(rt);
    hle_acm_reset(rt);
    hle_vdecsw_reset(rt);
    hle_libc_reset(rt);
    if (rt->threads_changed) pal_cond_destroy(rt->threads_changed);
    if (rt->threads_lock) pal_mutex_destroy(rt->threads_lock);
    linker_free(&rt->lk);
    gmem_free(&rt->mem);
}

/* Resolves stub records against the HLE registry and installs the thunks. */
static ps5c_err bind_stubs(ps5_runtime *rt) {
    dyn_free(&rt->stubs);
    for (size_t i = 0; i < rt->lk.stubs.len; i++) {
        const import_stub *s = &rt->lk.stubs.data[i];
        hle_stub_info si;
        memset(&si, 0, sizeof(si));
        si.library = s->library;
        si.nid = s->nid;
        si.name = rt->opts.names ? nid_db_lookup(rt->opts.names, s->nid) : NULL;
        si.exp = hle_find(s->library, s->nid);
        if (!dyn_push(&rt->stubs, si)) return PS5C_ERR_NOMEM;

        if (s->is_data) {
            void *slot = gmem_host(&rt->mem, s->addr, IMPORT_DATA_SLOT_SIZE);
            if (si.exp && si.exp->kind == HLE_DATA && si.exp->data_init && slot) {
                si.exp->data_init(rt, slot, IMPORT_DATA_SLOT_SIZE);
            }
        } else if (!x64_native_install_thunk(&rt->cpu, s->addr, (uint32_t)i)) {
            return PS5C_ERR_INVALID;
        }
    }
    /* Install native ABI bridges after ordinary imports have been bound:
     * their initializers may resolve additional HLE helper functions. */
    for (size_t i=0;i<rt->stubs.len;i++) {
        const hle_export *e=rt->stubs.data[i].exp;
        if(e && e->kind==HLE_NATIVE && e->data_init) {
            void *slot=gmem_host(&rt->mem,rt->lk.stubs.data[i].addr,IMPORT_STUB_SIZE);
            if(!slot)return PS5C_ERR_INVALID;
            e->data_init(rt,slot,IMPORT_STUB_SIZE);
        }
    }
    return PS5C_OK;
}

static const char *dup_name(const char *name) {
    size_t n = strlen(name) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, name, n);
    return d;
}

static guest_addr module_symbol(const loaded_module *m, uint64_t nid) {
    if (!m->mapped) return 0;
    for (uint32_t s = 1; s < m->img.sym_count; s++) {
        const sce_sym *sym = &m->img.syms[s];
        if (sym->has_nid && sym->nid == nid && sym->shndx != 0 && sym->value) return m->bias + sym->value;
    }
    return 0;
}

static guest_addr resolve_hle_export(ps5_runtime *rt,const hle_export *exp);
guest_addr runtime_resolve_symbol_in(ps5_runtime *rt, uint32_t handle, const char *name) {
    const char *library=hle_library_from_handle(handle);
    if(library)return resolve_hle_export(rt,hle_find(library,nid_hash_name(name)));
    if (handle >= 1 && handle <= rt->ms->modules.len) {
        guest_addr addr = module_symbol(rt->ms->modules.data[handle - 1], nid_hash_name(name));
        if (addr) return addr;
    }
    return runtime_resolve_symbol(rt, name);
}

guest_addr runtime_resolve_symbol(ps5_runtime *rt, const char *name) {
    uint64_t nid;
    nid = nid_hash_name(name);

    for (size_t i = 0; i < rt->ms->modules.len; i++) {
        guest_addr addr = module_symbol(rt->ms->modules.data[i], nid);
        if (addr) return addr;
    }

    /* An HLE function. Names nothing implements stay unresolved: callers
     * such as AMPR_EMU probe for optional kernel features and take another
     * path when they are missing. */
    return resolve_hle_export(rt,hle_find_any(nid));
}

static guest_addr resolve_hle_export(ps5_runtime *rt,const hle_export *exp) {
    if (!exp || (exp->kind != HLE_FUNC && exp->kind != HLE_NATIVE)) return 0;
    uint64_t nid=nid_hash_name(exp->name);
    if(exp->name[0]=='#' && !nid_from_str(exp->name+1,strlen(exp->name+1),&nid))return 0;
    const char *name=exp->name;
    for (size_t i = 0; i < rt->stubs.len; i++) {
        if (exp ? rt->stubs.data[i].exp == exp : rt->stubs.data[i].nid == nid) return rt->lk.stubs.data[i].addr;
    }
    const char *library = exp ? exp->library : "libkernel";
    guest_addr addr = 0;
    if (!linker_add_stub(&rt->lk, library, nid, PROVIDER_HLE, false, &addr)) return 0;
    hle_stub_info si;
    memset(&si, 0, sizeof(si));
    si.library = library;
    si.nid = nid;
    si.name = exp ? exp->name : dup_name(name);
    si.exp = exp;
    uint32_t id = (uint32_t)(rt->lk.stubs.len - 1);
    if (id != rt->stubs.len || !dyn_push(&rt->stubs, si) || !x64_native_install_thunk(&rt->cpu, addr, id)) {
        LOG_E(HLE, "cannot create a stub for %s", name);
        return 0;
    }
    if(exp->kind==HLE_NATIVE && exp->data_init)exp->data_init(rt,gmem_host(&rt->mem,addr,IMPORT_STUB_SIZE),IMPORT_STUB_SIZE);
    return addr;
}

static void exit_handler_fn(hle_call *c) {
    LOG_I(CORE, "guest called the process exit handler");
    runtime_exit_thread(c->rt, 0);
}

static const hle_export k_runtime_exports[] = {
    HLE_FUNC_ENTRY("ps5c", "ps5c_exit_handler", exit_handler_fn),
};

ps5c_err runtime_link(ps5_runtime *rt) {
    ps5c_err err = linker_link_all(&rt->lk);
    if (err != PS5C_OK) return err;
    LOG_I(CPU, "patched %u thread-pointer reads (fs:[0] -> gs:[TEB slot])", rt->tls_patches);

    if (!linker_add_stub(&rt->lk, "ps5c", nid_hash_name("ps5c_exit_handler"), PROVIDER_HLE, false,
                         &rt->exit_handler)) {
        return PS5C_ERR_NOMEM;
    }
    if ((err = bind_stubs(rt)) != PS5C_OK) return err;
    /* Internal exports aren't in the registry: hook them up directly. */
    for (size_t i = 0; i < rt->stubs.len; i++) {
        if (strcmp(rt->stubs.data[i].library, "ps5c") == 0) rt->stubs.data[i].exp = &k_runtime_exports[0];
    }

    uint32_t implemented = 0;
    for (size_t i = 0; i < rt->stubs.len; i++) implemented += rt->stubs.data[i].exp != NULL;
    LOG_I(HLE, "%u import stubs, %u implemented in HLE (%u functions registered)", (unsigned)rt->stubs.len,
          implemented, hle_registry_count());
    /* PS5C_LIST_UNIMPLEMENTED=<file>: the imports without an HLE implementation, one
     * "library<TAB>name" line each ("library<TAB>?NID" when the name is unknown), then the
     * process ends (a static audit of a game's missing functions; tools/missing_imports.py
     * merges the lists of many games) */
    const char *missing_list = getenv("PS5C_LIST_UNIMPLEMENTED");
    if (missing_list && *missing_list) {
        FILE *f = fopen(missing_list, "w");
        for (size_t i = 0; f && i < rt->stubs.len; i++) {
            const hle_stub_info *si = &rt->stubs.data[i];
            if (si->exp) continue;
            char nid[NID_STR_LEN + 1];
            nid_to_str(si->nid, nid);
            if (si->name) fprintf(f, "%s\t%s\n", si->library, si->name);
            else fprintf(f, "%s\t?%s\n", si->library, nid);
        }
        if (f) fclose(f);
        LOG_I(HLE, "unimplemented imports written to %s", missing_list);
        exit(0);
    }
    /* PS5C_DUMP_MODULE=<name>: the relocated image, for offline tools (tools/il2cpp_symbols.py) */
    const char *dump = getenv("PS5C_DUMP_MODULE");
    for (size_t i = 0; dump && i < rt->ms->modules.len; i++) {
        const loaded_module *m = rt->ms->modules.data[i];
        if (!m->mapped || strcmp(m->name, dump) != 0) continue;
        char path[300];
        snprintf(path, sizeof(path), "user/%s_%llx.bin", m->name, (unsigned long long)m->base);
        FILE *f = fopen(path, "wb");
        uint64_t written = 0, skipped = 0;
        bool ok = f != NULL;
        for (uint64_t o = 0; f && o < m->map_size; o += GUEST_PAGE_SIZE) {
            uint8_t page[GUEST_PAGE_SIZE];
            uint64_t size = PS5C_MIN(sizeof(page), m->map_size - o);
            guest_addr addr = m->base + o;
            gmem_vma v;
            /* Module images include PROT_NONE padding between ELF segments.
             * Mapped does not mean readable. VMAs are stable here, before
             * module initialization and guest threads start. */
            bool readable = gmem_query_vma(&rt->mem, addr, false, &v) && !v.reserved &&
                            (v.prot & GPROT_READ) && size <= v.size - (addr - v.start);
            if (!readable || !gmem_read(&rt->mem, addr, page, size)) {
                memset(page, 0, sizeof(page));
                skipped += size;
            }
            if (fwrite(page, 1, (size_t)size, f) != size) {
                ok = false;
                break;
            }
            written += size;
        }
        if (f && fclose(f) != 0) ok = false;
        if (ok)
            LOG_I(CORE, "dumped %s (0x%llx bytes at 0x%llx, 0x%llx unreadable bytes zero-filled) to %s", m->name,
                  (unsigned long long)written, (unsigned long long)m->base, (unsigned long long)skipped, path);
        else
            LOG_W(CORE, "could not finish dumping %s to %s (0x%llx bytes written)", m->name, path,
                  (unsigned long long)written);
    }
    return PS5C_OK;
}

/* ---------------------------------------------------------------------------
 * Threads
 * ------------------------------------------------------------------------- */

/* Guest-side resources of a thread: stack (unless given), static TLS, TCB, DTV. */
static ps5c_err thread_create(ps5_runtime *rt, guest_thread *t, guest_addr stack_addr, uint64_t stack_size) {
    memset(t, 0, sizeof(*t));
    t->rt = rt;
    pal_mutex_lock(rt->threads_lock);
    t->id = rt->next_thread_id++;
    pal_mutex_unlock(rt->threads_lock);
    char name[40];
    ps5c_err err = PS5C_OK;
    if (stack_addr) {
        t->stack_base = stack_addr;
    } else {
        stack_size = PS5C_ALIGN_UP(PS5C_MAX(stack_size, GUEST_MIN_STACK_SIZE), GUEST_PAGE_SIZE);
        snprintf(name, sizeof(name), "stack thread %u", t->id);
        err = gmem_map(&rt->mem, 0, stack_size, GPROT_RW, 0, name, &t->stack_base);
        if (err != PS5C_OK) return err;
        t->stack_owned = true;
    }
    t->stack_size = stack_size;
    if (pal_mutex_create(&t->sig_lock) != PS5C_OK || pal_cond_create(&t->sig_cond) != PS5C_OK) {
        return PS5C_ERR_NOMEM;
    }

    /* Static TLS (variant II): module blocks below the TCB, then the TCB, then the DTV. */
    uint64_t static_size = PS5C_ALIGN_UP(rt->lk.static_tls_end, 256);
    uint32_t module_count = rt->lk.next_tls_index; /* indices 1..count-1 used */
    uint64_t dtv_size = (2ull + module_count) * 8;
    t->tls_block_size = static_size + GUEST_TCB_SIZE + dtv_size + 16; /* + errno */
    snprintf(name, sizeof(name), "tls thread %u", t->id);
    err = gmem_map(&rt->mem, 0, t->tls_block_size, GPROT_RW, 0, name, &t->tls_block);
    if (err != PS5C_OK) return err;
    t->tcb = t->tls_block + static_size;
    t->dtv = t->tcb + GUEST_TCB_SIZE;
    t->errno_addr = PS5C_ALIGN_UP(t->dtv + dtv_size, 8);

    gmem_write_u64(&rt->mem, t->tcb, t->tcb);     /* tcb_self */
    gmem_write_u64(&rt->mem, t->tcb + 8, t->dtv); /* tcb_dtv */
    gmem_write_u64(&rt->mem, t->tcb + 16, (uint64_t)(uintptr_t)t); /* tcb_thread (pthread_t) */
    gmem_write_u64(&rt->mem, t->dtv, 1);          /* generation */
    gmem_write_u64(&rt->mem, t->dtv + 8, module_count ? module_count - 1 : 0);

    for (size_t i = 0; i < rt->ms->modules.len; i++) {
        const loaded_module *m = rt->ms->modules.data[i];
        if (!m->tls_index) continue;
        guest_addr block = t->tcb - m->tls_offset;
        gmem_write_u64(&rt->mem, t->dtv + (1ull + m->tls_index) * 8, block);
        if (m->img.tls_filesz) {
            const void *src = gmem_host(&rt->mem, m->bias + m->img.tls_vaddr, m->img.tls_filesz);
            if (!src || !gmem_write(&rt->mem, block, src, m->img.tls_filesz)) {
                LOG_E(CORE, "%s: TLS image out of range", m->name);
                return PS5C_ERR_INVALID;
            }
        }
    }
    return PS5C_OK;
}

/* GUEST_ARGV's newline-delimited file takes precedence. Otherwise preserve
 * main's space-delimited GUEST_ARGS (six arguments including argv[0]). */
static ps5c_err write_process_params(ps5_runtime *rt, const char *argv0) {
    /* Optional diagnostic arguments are data, never a host shell command. */
    const char *args[PROCESS_ARG_MAX]={argv0};uint32_t count=1;
    uint8_t *extra=NULL;uint64_t extra_bytes=0;
    const char *setting=getenv("PS5C_GUEST_ARGV");
    if(setting && *setting) {
        if(setting[0]!='@' || pal_file_read_all(setting+1,&extra,&extra_bytes)!=PS5C_OK || extra_bytes>=GUEST_PAGE_SIZE) {
            free(extra);LOG_E(CORE,"invalid PS5C_GUEST_ARGV file");return PS5C_ERR_INVALID;
        }
        uint8_t *text=realloc(extra,(size_t)extra_bytes+1);
        if(!text){free(extra);return PS5C_ERR_NOMEM;}
        extra=text;extra[extra_bytes]=0;
        if(!process_args_lines((char*)extra,(size_t)extra_bytes,args,&count)) {
            free(extra);LOG_E(CORE,"guest argument file exceeds limits or contains NUL");return PS5C_ERR_INVALID;
        }
    } else {
        const char *legacy=getenv("PS5C_GUEST_ARGS");
        if(legacy && *legacy) {
            extra_bytes=strlen(legacy);
            if(extra_bytes>=GUEST_PAGE_SIZE)return PS5C_ERR_INVALID;
            extra=malloc((size_t)extra_bytes+1);if(!extra)return PS5C_ERR_NOMEM;
            memcpy(extra,legacy,(size_t)extra_bytes+1);
            if(!process_args_words((char*)extra,args,&count,6)){free(extra);return PS5C_ERR_INVALID;}
        }
    }
    ps5c_err err = gmem_map(&rt->mem, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "process params", &rt->process_params);
    if(err!=PS5C_OK){free(extra);return err;}
    uint8_t *params=malloc(GUEST_PAGE_SIZE);
    if(!params){free(extra);return PS5C_ERR_NOMEM;}
    bool ok=process_params_pack(params,GUEST_PAGE_SIZE,rt->process_params,args,count) &&
            gmem_write(&rt->mem,rt->process_params,params,GUEST_PAGE_SIZE);
    free(params);free(extra);
    if(ok && count>1)LOG_I(CORE,"guest startup arguments: %u entries",count);
    return ok?PS5C_OK:PS5C_ERR_INVALID;
}

/* Calls fn(arg0, arg1, arg2) on thread `t`. False if the guest asked to exit. */
static bool call_guest(ps5_runtime *rt, guest_thread *t, guest_addr fn, uint64_t arg0, uint64_t arg1,
                       uint64_t arg2, uint64_t *rax) {
    t->enter.fn = fn;
    t->enter.rsp = PS5C_ALIGN_DOWN(t->stack_base + t->stack_size - 64, 16);
    t->enter.arg0 = arg0;
    t->enter.arg1 = arg1;
    t->enter.arg2 = arg2;
    t->exit_requested = false;
#if defined(__GNUC__) && (defined(__x86_64__) || defined(_M_X64))
    /* FreeBSD starts threads with the x87 at 64-bit precision (control word 0x037F);
     * Windows threads get 53 bits (0x027F), which changes long double results. */
    { const uint16_t fpucw = 0x037F; __asm__ volatile("fldcw %0" : : "m"(fpucw)); }
#endif
    /* While guest code (and HLE code, which runs on the guest stack) executes,
     * the OS must see the guest stack as the thread's stack. Only switch
     * around the call itself: host code on the host stack needs the real
     * bounds to grow its stack through the guard page. The bounds cover the
     * whole guest range: games switch to stacks of their own (fibers, job
     * systems), and below the declared limit Windows' stack probes in HLE
     * code would walk down from it writing to every page (Astro Bot's
     * stacks in its heap at 0x300000000). */
    pal_stack_bounds host_stack, guest_stack = {rt->mem.res_start, rt->mem.res_end, rt->mem.res_start};
    pal_thread_get_stack_bounds(&host_stack);
    pal_thread_set_stack_bounds(&guest_stack);
    *rax = x64_native_enter(&rt->cpu, &t->enter);
    pal_thread_set_stack_bounds(&host_stack);
    return !t->exit_requested;
}

bool runtime_call_nested(ps5_runtime *rt, guest_addr fn, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t *rax) {
    return runtime_call_nested4(rt, fn, arg0, arg1, arg2, 0, rax);
}

/* Room a nested guest call needs below its rsp. The callee runs on the
 * caller's stack, as on the console, so a 64 KiB stack (the libkernel default)
 * or a fiber's stack outside the thread's own range must still be accepted. */
#define NESTED_STACK_HEADROOM 0x2000ull

/* True if [lo, hi) lies in the thread's stack or in writable guest memory
 * (fibers and job systems run on stacks of their own). */
static bool nested_stack_ok(ps5_runtime *rt, const guest_thread *t, uint64_t lo, uint64_t hi) {
    if (lo >= hi || lo < rt->mem.res_start || hi > rt->mem.res_end) return false;
    if (lo >= t->stack_base && hi <= t->stack_base + t->stack_size) return true;
    for (uint64_t cur = lo; cur < hi;) {
        gmem_vma v;
        if (!gmem_query_vma(&rt->mem, cur, false, &v) || v.reserved || !(v.prot & GPROT_WRITE)) return false;
        cur = v.start + v.size;
    }
    return true;
}

bool runtime_call_nested4(ps5_runtime *rt, guest_addr fn, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                          uint64_t *rax) {
    guest_thread *t = runtime_current_thread(rt);
    if (!t) return false;
    /* HLE code runs on the guest stack: continue below this frame */
    volatile uint8_t marker = 0;
    uint64_t here = (uint64_t)(uintptr_t)&marker;
    uint64_t rsp = PS5C_ALIGN_DOWN(here - 0x1000, 16);
    if (!nested_stack_ok(rt, t, rsp - NESTED_STACK_HEADROOM, here)) {
        static uint32_t refused;
        uint32_t n = __atomic_add_fetch(&refused, 1, __ATOMIC_RELAXED);
        if (n <= 8 || (n & 1023) == 0)
            LOG_W(CORE, "nested guest call 0x%llx refused (%u): rsp 0x%llx has under %u KiB of writable stack "
                        "(thread stack 0x%llx+0x%llx)",
                  (unsigned long long)fn, n, (unsigned long long)rsp, (unsigned)(NESTED_STACK_HEADROOM >> 10),
                  (unsigned long long)t->stack_base, (unsigned long long)t->stack_size);
        return false;
    }
    if (here < t->stack_base + 0x10000 || here > t->stack_base + t->stack_size) {
        /* Refused before 2026-11: 64 KiB thread stacks and fibers. */
        static uint32_t small;
        uint32_t n = __atomic_add_fetch(&small, 1, __ATOMIC_RELAXED);
        if (n == 1 || (n & (n - 1)) == 0)
            LOG_I(CORE, "nested guest call 0x%llx on a %s stack (%u so far)", (unsigned long long)fn,
                  here > t->stack_base + t->stack_size || here < t->stack_base ? "fiber" : "small thread", n);
    }
    x64_enter_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fn = fn;
    ctx.rsp = rsp;
    ctx.arg0 = arg0;
    ctx.arg1 = arg1;
    ctx.arg2 = arg2;
    ctx.arg3 = arg3;
    *rax = x64_native_enter(&rt->cpu, &ctx);
    return !t->exit_requested;
}

/* ---- Thread lifecycle ---- */

static void thread_release(ps5_runtime *rt, guest_thread *t) {
    if (t->sig_lock) pal_mutex_destroy(t->sig_lock);
    if (t->sig_cond) pal_cond_destroy(t->sig_cond);
    pal_thread_close_handle(t->sample_handle);
    if (t->stack_owned) gmem_unmap(&rt->mem, t->stack_base, t->stack_size);
    gmem_unmap(&rt->mem, t->tls_block, PS5C_ALIGN_UP(t->tls_block_size, GUEST_PAGE_SIZE));
    free(t);
}

/* Removes `t` from the registry. Caller holds threads_lock. */
static void thread_unregister(ps5_runtime *rt, guest_thread *t) {
    for (size_t i = 0; i < rt->threads.len; i++) {
        if (rt->threads.data[i] == t) {
            rt->threads.data[i] = rt->threads.data[--rt->threads.len];
            return;
        }
    }
}

guest_thread *runtime_lookup_thread(ps5_runtime *rt, uint64_t handle) {
    guest_thread *found = NULL;
    pal_mutex_lock(rt->threads_lock);
    for (size_t i = 0; i < rt->threads.len; i++) {
        if ((uint64_t)(uintptr_t)rt->threads.data[i] == handle) {
            found = rt->threads.data[i];
            break;
        }
    }
    pal_mutex_unlock(rt->threads_lock);
    return found;
}

bool runtime_find_stack(ps5_runtime *rt, guest_addr addr, guest_addr *lo, guest_addr *hi) {
    bool found = false;
    pal_mutex_lock(rt->threads_lock);
    for (size_t i = 0; i < rt->threads.len && !found; i++) {
        const guest_thread *t = rt->threads.data[i];
        if (addr >= t->stack_base && addr < t->stack_base + t->stack_size) {
            *lo = t->stack_base;
            *hi = t->stack_base + t->stack_size;
            found = true;
        }
    }
    pal_mutex_unlock(rt->threads_lock);
    return found;
}

uint32_t runtime_running_threads(ps5_runtime *rt) {
    uint32_t n = 0;
    pal_mutex_lock(rt->threads_lock);
    for (size_t i = 0; i < rt->threads.len; i++) n += rt->threads.data[i]->host && !rt->threads.data[i]->finished;
    pal_mutex_unlock(rt->threads_lock);
    return n;
}

/* exit() on a secondary thread ends the process. */
static void thread_check_process_exit(ps5_runtime *rt, guest_thread *t) {
    if (!t->exit_requested || t->thread_exit) return;
    LOG_I(CORE, "thread %u '%s' ended the process with code %lld", t->id, t->name, (long long)t->exit_code);
    report_missing_calls(rt);
    pal_exit_process((int)t->exit_code);
}

/* The thread is over (returned, or pthread_exit after its cleanup handlers):
 * as libthr does, run libc's thread-exit hook (C++ thread_local destructors)
 * and then the pthread key destructors, in up to 4 passes
 * (PTHREAD_DESTRUCTOR_ITERATIONS) since a destructor may set values again.
 * Each runs from the top of the thread's own stack, which is free now.
 * PS5C_THREAD_DTORS=0 skips them all (as before 2026-11). */
static void thread_exit_dtors(ps5_runtime *rt, guest_thread *t) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *s = getenv("PS5C_THREAD_DTORS");
        enabled = !(s && s[0] == '0');
    }
    if (!enabled) return;
    uint64_t rax;
    if (rt->thread_dtors) {
        t->thread_exit = false;
        call_guest(rt, t, rt->thread_dtors, 0, 0, 0, &rax);
        thread_check_process_exit(rt, t);
    }
    /* thread_local destructors the HLE libc took (_sceLibcInternalThreadAtexit) and the
     * thread did not run itself through _sceLibcInternalThreadDtors */
    uint32_t atexit_dtors = 0;
    uint64_t dfn, dobj;
    while (atexit_dtors < 4096 && libc_thread_take_dtor(t->id, &dfn, &dobj)) {
        atexit_dtors++;
        t->thread_exit = false;
        call_guest(rt, t, dfn, dobj, 0, 0, &rax);
        thread_check_process_exit(rt, t);
    }
    uint32_t keys = 0;
    for (int pass = 0; pass < 4; pass++) {
        uint32_t cursor = 0;
        uint64_t fn, value;
        bool any = false;
        while (kernel_thread_take_specific(t, &cursor, &fn, &value)) {
            any = true;
            keys++;
            t->thread_exit = false;
            call_guest(rt, t, fn, value, 0, 0, &rax);
            thread_check_process_exit(rt, t);
        }
        if (!any) break;
    }
    static uint32_t logged;
    if ((keys || atexit_dtors || rt->thread_dtors) && __atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 8)
        LOG_I(CORE, "thread %u '%s' exit: libc hook %s, %u thread_local destructor(s), %u key destructor(s)",
              t->id, t->name, rt->thread_dtors ? "run" : "none", atexit_dtors, keys);
}

static int32_t guest_thread_main(void *arg) {
    guest_thread *t = (guest_thread *)arg;
    ps5_runtime *rt = t->rt;
    pal_mutex_lock(rt->threads_lock);
    while (!t->start_ready) pal_cond_wait(rt->threads_changed, rt->threads_lock);
    pal_mutex_unlock(rt->threads_lock);
    t->sample_handle = pal_thread_open_current();
    pal_tls_set(rt->tcb_slot, (void *)(uintptr_t)t->tcb);
    pal_tls_set(rt->thread_slot, t);
    LOG_D(CORE, "thread %u '%s' starts at 0x%llx", t->id, t->name, (unsigned long long)t->entry);

    uint64_t rax = 0;
    call_guest(rt, t, t->entry, t->arg, 0, 0, &rax);
    thread_check_process_exit(rt, t);
    uint64_t retval = t->exit_requested ? (uint64_t)t->exit_code : rax;
    thread_exit_dtors(rt, t);
    LOG_D(CORE, "thread %u '%s' finished", t->id, t->name);

    pal_mutex_lock(rt->threads_lock);
    t->retval = retval;
    t->finished = true;
    bool release = t->detached;
    if (release) thread_unregister(rt, t);
    pal_cond_broadcast(rt->threads_changed);
    pal_mutex_unlock(rt->threads_lock);
    pal_tls_set(rt->thread_slot, NULL);
    pal_tls_set(rt->tcb_slot, NULL);
    if (release) thread_release(rt, t); /* the host thread handle of a detached thread is not joined */
    return 0;
}

/* The host thread only runs glue and HLE code between guest calls: a small stack is enough. */
#define HOST_THREAD_STACK (256u << 10)

int runtime_start_thread(ps5_runtime *rt, guest_addr entry, uint64_t arg, guest_addr stack_addr,
                         uint64_t stack_size, bool detached, const char *name, guest_thread **out) {
    guest_thread *t = (guest_thread *)calloc(1, sizeof(guest_thread));
    if (!t) return 12; /* ENOMEM */
    if (thread_create(rt, t, stack_addr, stack_size) != PS5C_OK) {
        free(t);
        return 35; /* EAGAIN */
    }
    t->entry = entry;
    t->arg = arg;
    t->detached = detached;
    snprintf(t->name, sizeof(t->name), "%s", name && *name ? name : "thread");
    pal_mutex_lock(rt->threads_lock);
    bool ok = dyn_push(&rt->threads, t);
    pal_mutex_unlock(rt->threads_lock);
    if (!ok) {
        thread_release(rt, t);
        return 12;
    }
    *out = t; /* visible to the caller before the thread can finish */
    char host_name[48];
    snprintf(host_name, sizeof(host_name), "guest %u %s", t->id, t->name);
    if (pal_thread_create(&t->host, guest_thread_main, t, host_name, HOST_THREAD_STACK) != PS5C_OK) {
        pal_mutex_lock(rt->threads_lock);
        thread_unregister(rt, t);
        pal_mutex_unlock(rt->threads_lock);
        thread_release(rt, t);
        return 35;
    }
    LOG_I(CORE, "created thread %u '%s' (entry 0x%llx, stack 0x%llx bytes)", t->id, t->name,
          (unsigned long long)entry, (unsigned long long)t->stack_size);
    return 0;
}

void runtime_release_thread(ps5_runtime *rt, guest_thread *t) {
    pal_mutex_lock(rt->threads_lock);
    t->start_ready = true;
    pal_cond_broadcast(rt->threads_changed);
    pal_mutex_unlock(rt->threads_lock);
    /* A detached thread may already have freed t here. */
}

int runtime_join_thread(ps5_runtime *rt, uint64_t handle, uint64_t *retval) {
    guest_thread *self = runtime_current_thread(rt);
    if ((uint64_t)(uintptr_t)self == handle) return 11; /* EDEADLK */
    pal_mutex_lock(rt->threads_lock);
    guest_thread *t = NULL;
    for (size_t i = 0; i < rt->threads.len; i++) {
        if ((uint64_t)(uintptr_t)rt->threads.data[i] == handle) t = rt->threads.data[i];
    }
    if (!t || !t->host) {
        pal_mutex_unlock(rt->threads_lock);
        return 3; /* ESRCH */
    }
    if (t->detached || t->joining) {
        pal_mutex_unlock(rt->threads_lock);
        return 22; /* EINVAL */
    }
    t->joining = true;
    while (!t->finished) pal_cond_wait(rt->threads_changed, rt->threads_lock);
    thread_unregister(rt, t);
    pal_mutex_unlock(rt->threads_lock);
    if (retval) *retval = t->retval;
    pal_thread_join(t->host, NULL);
    thread_release(rt, t);
    return 0;
}

int runtime_detach_thread(ps5_runtime *rt, uint64_t handle) {
    pal_mutex_lock(rt->threads_lock);
    guest_thread *t = NULL;
    for (size_t i = 0; i < rt->threads.len; i++) {
        if ((uint64_t)(uintptr_t)rt->threads.data[i] == handle) t = rt->threads.data[i];
    }
    int err = 0;
    bool release = false;
    if (!t || !t->host) err = 3;
    else if (t->detached) err = 22;
    else {
        t->detached = true;
        if (t->finished) {
            thread_unregister(rt, t);
            release = true;
        }
    }
    pal_mutex_unlock(rt->threads_lock);
    if (release) {
        pal_thread_join(t->host, NULL);
        thread_release(rt, t);
    }
    return err;
}

/* Depth-first over DT_SCE_NEEDED_MODULE so dependencies initialize first. */
static void collect_init_order(ps5_runtime *rt, const loaded_module *m, bool *visited,
                               const loaded_module **order, uint32_t *count) {
    size_t idx = 0;
    while (idx < rt->ms->modules.len && rt->ms->modules.data[idx] != m) idx++;
    if (idx == rt->ms->modules.len || visited[idx]) return;
    visited[idx] = true;
    for (uint32_t i = 0; i < m->img.needed_module_count; i++) {
        const loaded_module *dep = module_set_find_by_name(rt->ms, m->img.needed_modules[i].name);
        if (dep) collect_init_order(rt, dep, visited, order, count);
    }
    order[(*count)++] = m;
}

/* Runs DT_INIT of every loaded library (not the executable: its entry point
 * does its own initialization). */
static bool start_modules(ps5_runtime *rt, guest_thread *t) {
    size_t n = rt->ms->modules.len;
    bool *visited = (bool *)calloc(n ? n : 1, sizeof(bool));
    const loaded_module **order = (const loaded_module **)calloc(n ? n : 1, sizeof(*order));
    uint32_t count = 0;
    bool ok = visited && order;
    if (ok) {
        /* The game's libc goes first, as the PS5 loader does: its init installs the
         * game's malloc replacement (user_malloc_init) before other libraries' static
         * constructors register atexit handlers in libc's small static pool. */
        const loaded_module *libc = module_set_find_by_name(rt->ms, "libc");
        if (libc) collect_init_order(rt, libc, visited, order, &count);
        for (size_t i = 0; i < n; i++) collect_init_order(rt, rt->ms->modules.data[i], visited, order, &count);
    }
    for (uint32_t i = 0; ok && i < count; i++) {
        const loaded_module *m = order[i];
        if (m->is_main || !m->mapped || !m->img.has_init || m->deferred_start) continue;
        guest_addr init = m->bias + m->img.init_vaddr;
        LOG_I(CORE, "initializing %s (DT_INIT 0x%llx)", m->name, (unsigned long long)init);
        uint64_t rax = 0;
        if (!call_guest(rt, t, init, 0, 0, 0, &rax)) {
            LOG_W(CORE, "%s: exit requested during initialization (code %lld)", m->name, (long long)rax);
            ok = false;
        }
    }
    free(visited);
    free(order);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Asynchronous exceptions (sceKernelRaiseException)
 *
 * Guest code uses them to stop threads (the IL2CPP garbage collector's
 * stop-the-world: the handler records the registers and stack pointer, posts
 * a semaphore and waits for the collector to finish). The handler must see
 * the target's identity and registers and the target must not run meanwhile,
 * but it need not run on the target's host thread: a proxy host thread runs
 * it with the target's TLS while the target stays frozen.
 * ------------------------------------------------------------------------- */

#define SIG_STACK_SIZE (256u << 10)
#define UC_MCONTEXT 0x40 /* FreeBSD ucontext_t: sigmask, padding, mcontext */
#define UC_SIZE 0x540

typedef struct sig_delivery {
    ps5_runtime *rt;
    guest_thread *target;
    int sig;
    guest_addr handler;
    pal_cpu_regs regs;
    bool suspended; /* target frozen in guest code: resume it afterwards */
    pal_thread *host;
    volatile bool done;
} sig_delivery;

static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static void write_ucontext(ps5_runtime *rt, guest_addr uc, const pal_cpu_regs *r, guest_addr tcb) {
    uint8_t buf[UC_SIZE];
    memset(buf, 0, sizeof(buf));
    uint8_t *mc = buf + UC_MCONTEXT;
    /* mcontext: rdi rsi rdx rcx r8 r9 rax rbx rbp r10 r11 r12 r13 r14 r15 from +8 */
    static const uint8_t order[15] = {7, 6, 2, 1, 8, 9, 0, 3, 5, 10, 11, 12, 13, 14, 15};
    for (int i = 0; i < 15; i++) put64(mc + 8 + i * 8, r->gpr[order[i]]);
    put64(mc + 0xa0, r->rip);
    put64(mc + 0xa8, 0x43); /* cs */
    put64(mc + 0xb0, r->rflags);
    put64(mc + 0xb8, r->gpr[4]); /* rsp */
    put64(mc + 0xc0, 0x3b);      /* ss */
    put64(mc + 0xc8, 0x480);     /* mc_len */
    put64(mc + 0xd0, 0x10002);   /* _MC_FPFMT_XMM */
    put64(mc + 0xd8, 0x20001);   /* _MC_FPOWNED_PCB */
    memcpy(mc + 0x100, r->fxsave, sizeof(r->fxsave));
    put64(mc + 0x440, tcb); /* fsbase */
    gmem_write(&rt->mem, uc, buf, sizeof(buf));
}

static int32_t sig_proxy_main(void *arg) {
    sig_delivery *d = (sig_delivery *)arg;
    ps5_runtime *rt = d->rt;
    guest_thread *target = d->target;
    guest_thread *p = (guest_thread *)calloc(1, sizeof(guest_thread));
    guest_addr base = 0;
    if (p && gmem_map(&rt->mem, 0, SIG_STACK_SIZE + GUEST_PAGE_SIZE, GPROT_RW, 0, "signal stack", &base) == PS5C_OK) {
        p->rt = rt;
        p->id = target->id;
        p->proxy_of = target;
        p->tcb = target->tcb;
        p->dtv = target->dtv;
        p->errno_addr = target->errno_addr;
        p->priority = target->priority;
        memcpy(p->name, target->name, sizeof(p->name));
        p->stack_base = base;
        p->stack_size = SIG_STACK_SIZE;
        guest_addr uc = base + SIG_STACK_SIZE; /* the page above the stack */
        write_ucontext(rt, uc, &d->regs, target->tcb);

        pal_tls_set(rt->tcb_slot, (void *)(uintptr_t)p->tcb);
        pal_tls_set(rt->thread_slot, p);
        uint64_t rax = 0;
        uint64_t t0 = pal_time_ns();
        call_guest(rt, p, d->handler, (uint64_t)d->sig, uc, 0, &rax);
        uint64_t dt = pal_time_ns() - t0;
        if (dt > 20000000) LOG_D(CORE, "exception %d handler for thread %u returned after %.1f ms", d->sig, target->id, (double)dt / 1e6);
        if (p->exit_requested && !p->thread_exit) {
            LOG_I(CORE, "exception handler on thread %u ended the process with code %lld", target->id,
                  (long long)p->exit_code);
            pal_exit_process((int)p->exit_code);
        }
        pal_tls_set(rt->thread_slot, NULL);
        pal_tls_set(rt->tcb_slot, NULL);
        gmem_unmap(&rt->mem, base, SIG_STACK_SIZE + GUEST_PAGE_SIZE);
    } else {
        LOG_E(CORE, "no memory to run exception handler for thread %u", target->id);
    }
    free(p);

    pal_mutex_lock(target->sig_lock);
    if (d->suspended) pal_thread_resume(target->sample_handle);
    target->sig_hold--;
    pal_cond_broadcast(target->sig_cond);
    pal_mutex_unlock(target->sig_lock);
    d->done = true;
    return 0;
}

/* Joins finished proxy threads. Caller holds threads_lock. */
static void reap_deliveries(ps5_runtime *rt) {
    for (size_t i = 0; i < rt->deliveries.len;) {
        sig_delivery *d = rt->deliveries.data[i];
        if (!d->done) {
            i++;
            continue;
        }
        if (d->host) pal_thread_join(d->host, NULL);
        free(d);
        rt->deliveries.data[i] = rt->deliveries.data[--rt->deliveries.len];
    }
}

/* Freezes `t` and captures its registers. True when frozen: suspended in
 * guest code (d->suspended) or inside an HLE call (held when it returns). */
static bool freeze_thread(ps5_runtime *rt, guest_thread *t, sig_delivery *d) {
    for (int attempt = 0; attempt < 20000; attempt++) {
        if (t->finished) return false;
        pal_mutex_lock(t->sig_lock);
        if (!pal_thread_suspend(t->sample_handle)) {
            pal_mutex_unlock(t->sig_lock);
            return false;
        }
        bool regs = pal_thread_get_regs(t->sample_handle, &d->regs);
        if (regs && t->in_hle) {
            /* Host code: registers and stack are captured; the guest part of
             * the stack stays put until the call returns into the gate. */
            t->sig_hold++;
            pal_thread_resume(t->sample_handle);
            pal_mutex_unlock(t->sig_lock);
            d->suspended = false;
            return true;
        }
        if (regs && runtime_find_module(rt, d->regs.rip)) {
            t->sig_hold++;
            pal_mutex_unlock(t->sig_lock);
            d->suspended = true; /* stays suspended until the handler returns */
            return true;
        }
        /* Glue code between guest and host: try again shortly. */
        if (attempt == 10 || attempt == 1000 || attempt == 5000) {
            static int warned;
            if (warned++ < 12)
                LOG_W(CORE, "stopping thread %u '%s': outside guest and HLE code (pc 0x%llx, exe fn at %p, sp 0x%llx, regs %d, "
                      "suspended %d times, sig_hold %u)",
                      t->id, t->name, (unsigned long long)d->regs.rip, (void *)(uintptr_t)freeze_thread,
                      (unsigned long long)d->regs.gpr[4], (int)regs, pal_thread_suspend_count(t->sample_handle) - 1, t->sig_hold);
        }
        pal_thread_resume(t->sample_handle);
        pal_mutex_unlock(t->sig_lock);
        /* A yield, not a sleep: host sleeps are rounded up to the timer tick
         * (~15 ms on Windows), and a thread that calls HLE functions in a loop
         * is often caught between the guest and the call. */
        if (attempt < 1000) pal_thread_yield();
        else pal_sleep_ns(50000);
    }
    return false;
}

int runtime_raise_exception(ps5_runtime *rt, uint64_t handle, int sig) {
    if (sig < 1 || sig > 128 || !rt->exception_handlers[sig - 1]) return 22; /* EINVAL */
    guest_thread *t = runtime_lookup_thread(rt, handle);
    if (!t || t->finished || !t->sample_handle || !t->sig_lock) return 3; /* ESRCH */
    if (guest_thread_identity(runtime_current_thread(rt)) == t) {
        LOG_W(CORE, "exception %d raised on the calling thread: not supported", sig);
        return 22;
    }
    sig_delivery *d = (sig_delivery *)calloc(1, sizeof(sig_delivery));
    if (!d) return 12;
    d->rt = rt;
    d->target = t;
    d->sig = sig;
    d->handler = rt->exception_handlers[sig - 1];
    uint64_t t_start = pal_time_ns();
    if (!freeze_thread(rt, t, d)) {
        static int warned;
        if (warned++ < 20) {
            uint64_t pc = 0, sp = 0, fp = 0;
            if (t->sample_handle) pal_thread_sample(t->sample_handle, &pc, &sp, &fp);
            LOG_W(CORE, "exception %d: cannot stop thread %u '%s' (pc 0x%llx, in HLE %s, finished %d)", sig, t->id,
                  t->name, (unsigned long long)pc, t->in_hle && t->in_hle->exp ? t->in_hle->exp->name : t->in_hle ? "?" : "no",
                  (int)t->finished);
        }
        free(d);
        return 3;
    }
    uint64_t t_frozen = pal_time_ns();
    pal_mutex_lock(rt->threads_lock);
    uint64_t t_locked = pal_time_ns();
    reap_deliveries(rt);
    bool ok = dyn_push(&rt->deliveries, d);
    pal_mutex_unlock(rt->threads_lock);
    uint64_t t_reaped = pal_time_ns();
    char name[48];
    snprintf(name, sizeof(name), "signal %d for %u", sig, t->id);
    bool suspended = d->suspended;
    if (!ok || pal_thread_create(&d->host, sig_proxy_main, d, name, HOST_THREAD_STACK) != PS5C_OK) {
        pal_mutex_lock(t->sig_lock);
        if (suspended) pal_thread_resume(t->sample_handle);
        t->sig_hold--;
        pal_cond_broadcast(t->sig_cond);
        pal_mutex_unlock(t->sig_lock);
        if (ok) d->done = true; /* reaped without a join: host is NULL */
        else free(d);
        LOG_E(CORE, "cannot start exception handler thread for thread %u", t->id);
        return 35; /* EAGAIN */
    }
    uint64_t t_end = pal_time_ns();
    LOG_D(CORE, "exception %d delivered to thread %u (%s): freeze %.2f ms, lock %.2f ms, reap %.2f ms, start %.2f ms", sig,
          t->id, suspended ? "running" : "in HLE", (double)(t_frozen - t_start) / 1e6, (double)(t_locked - t_frozen) / 1e6,
          (double)(t_reaped - t_locked) / 1e6, (double)(t_end - t_reaped) / 1e6);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Sampling profiler (env PS5C_SAMPLE=<report seconds>)
 *
 * Every millisecond each guest thread is suspended and its pc read. Samples
 * are keyed by (thread, pc, caller); the busiest keys are printed at each
 * report. A thread inside an HLE call is attributed to that call.
 * ------------------------------------------------------------------------- */

#define SAMPLE_SLOTS 8192u

typedef struct sample_slot {
    uint32_t tid;
    const hle_stub_info *hle;
    guest_addr pc, caller;
    uint64_t count;
} sample_slot;

static sample_slot *sample_find(sample_slot *tab, uint32_t tid, const hle_stub_info *hle, guest_addr pc,
                                guest_addr caller) {
    uint64_t h = (pc ^ (caller * 31u) ^ ((uint64_t)tid << 40) ^ (uint64_t)(uintptr_t)hle) * 0x9E3779B97F4A7C15ull;
    h >>= 40;
    for (uint32_t i = 0; i < SAMPLE_SLOTS; i++) {
        sample_slot *s = &tab[(h + i) & (SAMPLE_SLOTS - 1)];
        if (!s->count) {
            s->tid = tid, s->hle = hle, s->pc = pc, s->caller = caller;
            return s;
        }
        if (s->tid == tid && s->hle == hle && s->pc == pc && s->caller == caller) return s;
    }
    return NULL;
}

/* Per thread: the three busiest (pc, caller) keys, as a share of that thread's samples. */
static void sample_report(ps5_runtime *rt, sample_slot *tab, uint64_t total) {
    LOG_I(CORE, "profile: %llu samples", (unsigned long long)total);
    for (;;) {
        uint32_t tid = UINT32_MAX;
        for (uint32_t i = 0; i < SAMPLE_SLOTS; i++) {
            if (tab[i].count && tab[i].tid < tid) tid = tab[i].tid;
        }
        if (tid == UINT32_MAX) break;
        uint64_t thread_total = 0;
        for (uint32_t i = 0; i < SAMPLE_SLOTS; i++) {
            if (tab[i].count && tab[i].tid == tid) thread_total += tab[i].count;
        }
        for (int n = 0; n < 3; n++) {
            sample_slot *best = NULL;
            for (uint32_t i = 0; i < SAMPLE_SLOTS; i++) {
                if (tab[i].count && tab[i].tid == tid && (!best || tab[i].count > best->count)) best = &tab[i];
            }
            if (!best) break;
            char where[160], from[160];
            if (best->hle) {
                snprintf(where, sizeof(where), "HLE %s", best->hle->exp ? best->hle->exp->name : "?");
            } else {
                describe_addr(rt, best->pc, where, sizeof(where));
            }
            describe_addr(rt, best->caller, from, sizeof(from));
            LOG_I(CORE, "  T%-3u %5.1f%% %s <- %s", tid, 100.0 * (double)best->count / (double)thread_total, where,
                  from);
            best->count = 0;
        }
        for (uint32_t i = 0; i < SAMPLE_SLOTS; i++) {
            if (tab[i].tid == tid) tab[i].count = 0;
        }
    }
    memset(tab, 0, SAMPLE_SLOTS * sizeof(sample_slot));
}

void runtime_log_backtrace(ps5_runtime *rt) {
    guest_thread *t = runtime_current_thread(rt);
    if (!t) return;
    char where[160];
    describe_addr(rt, t->hle_ret, where, sizeof(where));
    LOG_E(CORE, "  T%u '%s' called from %s", t->id, t->name, where);
    uint64_t fp = t->hle_fp;
    for (int depth = 0; depth < 32 && fp; depth++) {
        uint64_t next = 0, ret_addr = 0;
        if (!gmem_read_u64(&rt->mem, fp, &next) || !gmem_read_u64(&rt->mem, fp + 8, &ret_addr) || !ret_addr) break;
        describe_addr(rt, ret_addr, where, sizeof(where));
        LOG_E(CORE, "    #%-2d %s", depth, where);
        if (next <= fp) break;
        fp = next;
    }
    /* IL2CPP managed exceptions near the top of the stack: klass->name ends with "Exception" */
    {
        volatile uint8_t m2 = 0;
        uint64_t s0 = (uint64_t)(uintptr_t)&m2 & ~7ull;
        int shown = 0;
        /* only on the thread's own stack (games run fibers on stacks of their own) */
        if (s0 < t->stack_base || s0 >= t->stack_base + t->stack_size) shown = 4;
        for (uint64_t a = s0; a + 8 <= t->stack_base + t->stack_size && a < s0 + 0x4000 && shown < 4; a += 8) {
            uint64_t v = 0;
            if (!gmem_read_u64(&rt->mem, a, &v) || v < 0x100000000ull) continue;
            for (int deref = 0; deref < 2 && shown < 4; deref++) {
                uint64_t o = v;
                if (deref && !gmem_read_u64(&rt->mem, v, &o)) break;
                uint64_t klass = 0, name_ptr = 0, ns_ptr = 0, msg = 0;
                char name[96] = {0}, ns[96] = {0};
                if (!gmem_read_u64(&rt->mem, o, &klass) || klass < 0x100000000ull) continue;
                if (!gmem_read_u64(&rt->mem, klass + 0x10, &name_ptr) || !gmem_read(&rt->mem, name_ptr, name, sizeof(name) - 1)) continue;
                size_t nl = strnlen(name, sizeof(name));
                if (nl < 9 || nl >= sizeof(name) - 1 || strcmp(name + nl - 9, "Exception") != 0) continue;
                gmem_read_u64(&rt->mem, klass + 0x18, &ns_ptr);
                if (ns_ptr) gmem_read(&rt->mem, ns_ptr, ns, sizeof(ns) - 1);
                char text[400] = {0};
                if (gmem_read_u64(&rt->mem, o + 0x18, &msg) && msg) {
                    int32_t len = 0;
                    gmem_read(&rt->mem, msg + 0x10, &len, 4);
                    for (int32_t k = 0; k < len && k < (int32_t)sizeof(text) - 1; k++) {
                        uint16_t ch = 0;
                        gmem_read(&rt->mem, msg + 0x14 + (uint64_t)k * 2, &ch, 2);
                        text[k] = ch < 128 ? (char)ch : '?';
                    }
                }
                LOG_E(CORE, "  managed exception %s.%s: %s", ns, name, text);
                /* stack trace string at +0x40 */
                uint64_t trace = 0;
                if (gmem_read_u64(&rt->mem, o + 0x40, &trace) && trace) {
                    int32_t len = 0;
                    gmem_read(&rt->mem, trace + 0x10, &len, 4);
                    char tt[600] = {0};
                    for (int32_t k = 0; k < len && k < (int32_t)sizeof(tt) - 1; k++) {
                        uint16_t ch = 0;
                        gmem_read(&rt->mem, trace + 0x14 + (uint64_t)k * 2, &ch, 2);
                        tt[k] = ch == 10 ? '|' : ch < 128 ? (char)ch : '?';
                    }
                    if (len) LOG_E(CORE, "    trace: %s", tt);
                }
                shown++;
                a += 8;
                break;
            }
        }
    }
    /* managed strings referenced from the stack (klass->name == "String") */
    {
        volatile uint8_t m3 = 0;
        uint64_t s0 = (uint64_t)(uintptr_t)&m3 & ~7ull;
        int shown = 0;
        for (uint64_t a = s0; a + 8 <= t->stack_base + t->stack_size && a < s0 + 0x3000 && shown < 40; a += 8) {
            uint64_t o = 0, klass = 0, name_ptr = 0;
            char name[8] = {0};
            if (!gmem_read_u64(&rt->mem, a, &o) || o < 0x100000000ull) continue;
            if (!gmem_read_u64(&rt->mem, o, &klass) || klass < 0x100000000ull) continue;
            if (!gmem_read_u64(&rt->mem, klass + 0x10, &name_ptr) || !gmem_read(&rt->mem, name_ptr, name, 7)) continue;
            if (strcmp(name, "String") != 0) continue;
            int32_t len = 0;
            if (!gmem_read(&rt->mem, o + 0x10, &len, 4) || len < 0 || len > 2000) continue;
            char text[200] = {0};
            for (int32_t k = 0; k < len && k < (int32_t)sizeof(text) - 1; k++) {
                uint16_t ch = 0;
                gmem_read(&rt->mem, o + 0x14 + (uint64_t)k * 2, &ch, 2);
                text[k] = ch >= 32 && ch < 127 ? (char)ch : '.';
            }
            LOG_E(CORE, "    string [sp+0x%llx] (%d) \"%s\"", (unsigned long long)(a - s0), len, text);
            shown++;
        }
    }
    /* stack scan: return addresses (preceded by a call) between here and the stack top */
    volatile uint8_t marker = 0;
    uint64_t sp = (uint64_t)(uintptr_t)&marker & ~7ull, top = t->stack_base + t->stack_size;
    if (sp < t->stack_base || sp >= top) return;
    int found = 0;
    for (uint64_t a = sp; a + 8 <= top && found < 48; a += 8) {
        uint64_t v = 0;
        uint8_t pre[6];
        if (!gmem_read_u64(&rt->mem, a, &v) || !runtime_find_module(rt, v)) continue;
        if (!gmem_read(&rt->mem, v - 6, pre, 6)) continue;
        bool call = pre[1] == 0xE8 || (pre[4] == 0xFF && (pre[5] & 0x38) == 0x10) || (pre[0] == 0xFF && (pre[1] & 0x38) == 0x10) ||
                    (pre[3] == 0xFF && (pre[4] & 0x38) == 0x10);
        if (!call) continue;
        describe_addr(rt, v, where, sizeof(where));
        LOG_E(CORE, "    [sp+0x%llx] %s", (unsigned long long)(a - sp), where);
        found++;
    }
}

/* Every thread's current HLE call (or pc) with a frame-pointer backtrace. */
static void dump_threads(ps5_runtime *rt) {
    /* PS5C_PEEK=addr,addr,...: 8 qwords at each address (debugging stalls);
     * addr>off>off... follows pointers: the qword at addr plus off, and so on */
    for (const char *pk = getenv("PS5C_PEEK"); pk && *pk;) {
        char *end;
        uint64_t a = strtoull(pk, &end, 16);
        while (*end == '>') {
            uint64_t p = 0;
            gmem_read_u64(&rt->mem, a, &p);
            a = p + strtoull(end + 1, &end, 16);
        }
        for (int k = 0; k < 8; k++) {
            uint64_t v = 0;
            if (gmem_read_u64(&rt->mem, a + (uint64_t)k * 8, &v)) LOG_I(CORE, "peek [0x%llx] = %016llx", (unsigned long long)(a + (uint64_t)k * 8), (unsigned long long)v);
        }
        pk = *end == ',' ? end + 1 : end;
        if (end == pk) break;
    }
    LOG_I(CORE, "thread dump:");
    for (size_t i = 0; i < rt->threads.len; i++) {
        guest_thread *t = rt->threads.data[i];
        if (t->finished || !t->sample_handle) continue;
        uint64_t pc = 0, sp = 0, fp = 0;
        if (!pal_thread_sample(t->sample_handle, &pc, &sp, &fp)) continue;
        const hle_stub_info *hle = t->in_hle;
        char where[160];
        {
            char line[600];
            int n = snprintf(line, sizeof(line), "  T%u host frames:", t->id);
            for (int k = 0; k < 400 && n < 580; k++) {
                uint64_t v = 0;
                if (!gmem_read_u64(&rt->mem, sp + (uint64_t)k * 8, &v)) break;
                if ((v >> 32) >= 0x7FF0 && (v >> 32) <= 0x7FFF)
                    n += snprintf(line + n, sizeof(line) - (size_t)n, " %llx", (unsigned long long)v);
            }
            LOG_I(CORE, "%s (pc %llx, dump_threads at %p)", line, (unsigned long long)pc, (void *)(uintptr_t)dump_threads);
        }
        if (hle) {
            describe_addr(rt, t->hle_ret, where, sizeof(where));
            LOG_I(CORE, "T%u '%s': in %s(0x%llx) from %s", t->id, t->name, hle->exp ? hle->exp->name : "?",
                  (unsigned long long)t->hle_arg0, where);
            if (hle->exp && !strcmp(hle->exp->name, "sceKernelWaitEqueue")) equeue_log_state(t->hle_arg0);
            fp = t->hle_fp;
            if (getenv("PS5C_DUMP_FRAMES")) {
                for (uint64_t o = 0x100; o >= 8; o -= 8) {
                    uint64_t v = 0;
                    if (gmem_read_u64(&rt->mem, fp - o, &v)) LOG_I(CORE, "      [fp-0x%03llx] %016llx", (unsigned long long)o, (unsigned long long)v);
                    if (o == 0xc8 && v > 0x1000000000ull && v < 0x9000000000ull) {
                        for (int k = 0; k < 10; k++) {
                            uint64_t e = 0, d = 0;
                            gmem_read_u64(&rt->mem, v + (uint64_t)k * 8, &e);
                            bool ptr = e > 0x800000000ull && e < 0x10000000000ull && gmem_read_u64(&rt->mem, e, &d);
                            LOG_I(CORE, "        entry+0x%02x = %016llx%s%016llx", k * 8, (unsigned long long)e, ptr ? " -> " : "", ptr ? (unsigned long long)d : 0ull);
                        }
                    }
                }
            }
        } else if (runtime_find_module(rt, pc)) {
            describe_addr(rt, pc, where, sizeof(where));
            LOG_I(CORE, "T%u '%s': at %s", t->id, t->name, where);
        } else {
            bool glue = pc >= rt->lk.import_code && pc < rt->lk.import_code + IMPORT_CODE_AREA_SIZE;
            LOG_I(CORE, "T%u '%s': in host code, pc 0x%llx%s sp 0x%llx fp 0x%llx, sig_hold %u", t->id, t->name,
                  (unsigned long long)pc, glue ? " (import stubs / glue)" : "", (unsigned long long)sp,
                  (unsigned long long)fp, t->sig_hold);
            /* host return addresses near the stack top (debugging: exe addresses
             * resolve with addr2line after rebasing on dump_threads) */
            char line[400];
            int n = snprintf(line, sizeof(line), "    stack (dump_threads at %p):", (void *)(uintptr_t)dump_threads);
            for (int k = 0; k < 160 && n < 380; k++) {
                uint64_t v = 0;
                if (!gmem_read_u64(&rt->mem, sp + (uint64_t)k * 8, &v)) break;
                if (v >> 40 == 0x7F && (v >> 32) >= 0x7FF0) n += snprintf(line + n, sizeof(line) - (size_t)n, " %llx", (unsigned long long)v);
            }
            LOG_I(CORE, "%s", line);
            if (!glue) continue;
        }
        for (int depth = 0; depth < 12 && fp; depth++) {
            uint64_t next = 0, ret_addr = 0;
            if (!gmem_read_u64(&rt->mem, fp, &next) || !gmem_read_u64(&rt->mem, fp + 8, &ret_addr) || !ret_addr) break;
            describe_addr(rt, ret_addr, where, sizeof(where));
            LOG_I(CORE, "    #%-2d %s", depth, where);
            if (next <= fp) break;
            fp = next;
        }
    }
}

static int32_t sampler_main(void *arg) {
    ps5_runtime *rt = (ps5_runtime *)arg;
    int secs = atoi(getenv("PS5C_SAMPLE"));
    uint64_t period_ns = (uint64_t)(secs > 0 ? secs : 10) * 1000000000ull;
    sample_slot *tab = (sample_slot *)calloc(SAMPLE_SLOTS, sizeof(sample_slot));
    if (!tab) return 0;
    uint64_t next = pal_time_ns() + period_ns, total = 0;
    for (;;) {
        pal_sleep_ns(1000000);
        pal_mutex_lock(rt->threads_lock);
        for (size_t i = 0; i < rt->threads.len; i++) {
            guest_thread *t = rt->threads.data[i];
            if (t->finished || !t->sample_handle) continue;
            uint64_t pc = 0, sp = 0, fp = 0;
            if (!pal_thread_sample(t->sample_handle, &pc, &sp, &fp)) continue;
            const hle_stub_info *hle = t->in_hle;
            guest_addr caller = 0;
            if (hle) {
                pc = 0;
                caller = t->hle_ret;
            } else if (runtime_find_module(rt, pc)) {
                gmem_read_u64(&rt->mem, fp + 8, &caller);
            } else {
                continue; /* host glue */
            }
            sample_slot *s = sample_find(tab, t->id, hle, pc, caller);
            if (s) s->count++;
            total++;
        }
        pal_mutex_unlock(rt->threads_lock);
        if (pal_time_ns() >= next) {
            if (total) sample_report(rt, tab, total);
            if (getenv("PS5C_DUMP")) {
                pal_mutex_lock(rt->threads_lock);
                dump_threads(rt);
                pal_mutex_unlock(rt->threads_lock);
            }
            total = 0;
            next = pal_time_ns() + period_ns;
        }
    }
    return 0;
}

/* Stall watchdog: when no frame reaches the screen for STALL_SECONDS (at the
 * start or later; PS5C_STALL_SECONDS changes it), every thread's state goes to
 * the log once, and again after another minute: testers' reports then show
 * where a game waits. */
#define STALL_SECONDS 30

uint64_t video_out_flip_count(void);

static int32_t watchdog_main(void *arg) {
    ps5_runtime *rt = (ps5_runtime *)arg;
    const char *e = getenv("PS5C_STALL_SECONDS");
    uint64_t stall = e && atoi(e) > 0 ? (uint64_t)atoi(e) : STALL_SECONDS;
    uint64_t last_flips = video_out_flip_count(), last_change = pal_time_ns(), next_dump = stall;
    for (int dumps = 0; dumps < 3;) {
        pal_sleep_ns(1000000000ull);
        uint64_t flips = video_out_flip_count(), now = pal_time_ns();
        if (flips != last_flips) {
            last_flips = flips;
            last_change = now;
            next_dump = stall;
            continue;
        }
        if (now - last_change < next_dump * 1000000000ull) continue;
        LOG_W(CORE, "no new frame for %llu s (%llu frames so far): thread states follow",
              (unsigned long long)((now - last_change) / 1000000000ull), (unsigned long long)flips);
        pal_mutex_lock(rt->threads_lock);
        dump_threads(rt);
        pal_mutex_unlock(rt->threads_lock);
        next_dump += 60;
        dumps++;
    }
    return 0;
}

void runtime_start_sampler(ps5_runtime *rt) {
    if (!rt->watchdog && !getenv("PS5C_NO_WATCHDOG") &&
        pal_thread_create(&rt->watchdog, watchdog_main, rt, "watchdog", 0) != PS5C_OK)
        rt->watchdog = NULL;
    if (!getenv("PS5C_SAMPLE") || rt->sampler) return;
    if (pal_thread_create(&rt->sampler, sampler_main, rt, "sampler", 0) != PS5C_OK) rt->sampler = NULL;
}

ps5c_err runtime_run_main(ps5_runtime *rt, int64_t *exit_code) {
    const loaded_module *main_mod = module_set_main(rt->ms);
    if (!main_mod || !main_mod->mapped) return PS5C_ERR_INVALID;
    /* PS5C_BREAK / PS5C_WATCH_WRITE: armed before the game runs (else at the first APR read) */
    if (getenv("PS5C_BREAK") || getenv("PS5C_WATCH_WRITE")) runtime_watch_arm(rt);

    guest_thread *t = (guest_thread *)calloc(1, sizeof(guest_thread));
    if (!t) return PS5C_ERR_NOMEM;
    ps5c_err err = thread_create(rt, t, 0, GUEST_MAIN_STACK_SIZE);
    if (err == PS5C_OK) err = write_process_params(rt, "eboot.bin");
    if (err == PS5C_OK) {
        snprintf(t->name, sizeof(t->name), "main");
        pal_mutex_lock(rt->threads_lock);
        if (!dyn_push(&rt->threads, t)) err = PS5C_ERR_NOMEM;
        pal_mutex_unlock(rt->threads_lock);
    }
    if (err != PS5C_OK) {
        free(t);
        return err;
    }

    pal_tls_set(rt->tcb_slot, (void *)(uintptr_t)t->tcb);
    pal_tls_set(rt->thread_slot, t);
    t->sample_handle = pal_thread_open_current();
    runtime_start_sampler(rt);

    uint64_t start = pal_time_ns();
    uint64_t rax = 0;
    if (start_modules(rt, t)) {
        guest_addr entry = main_mod->bias + main_mod->img.entry;
        LOG_I(CORE, "entering guest: %s entry 0x%llx, tcb 0x%llx", main_mod->name, (unsigned long long)entry,
              (unsigned long long)t->tcb);
        call_guest(rt, t, entry, rt->process_params, rt->exit_handler, 0, &rax);
    } else {
        rax = (uint64_t)t->exit_code;
    }
    uint64_t elapsed_ms = (pal_time_ns() - start) / 1000000u;

    pal_tls_set(rt->thread_slot, NULL);
    pal_tls_set(rt->tcb_slot, NULL);
    *exit_code = (int64_t)rax;
    LOG_I(CORE, "guest returned 0x%llx after %llu ms (%llu HLE calls, %llu calls to unimplemented imports)",
          (unsigned long long)rax, (unsigned long long)elapsed_ms, (unsigned long long)rt->hle_calls,
          (unsigned long long)rt->missing_calls);
    report_missing_calls(rt);
    return PS5C_OK;
}
