/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The emulated process: modules + guest memory + linker + HLE + CPU backend.
 *
 *   runtime_init      modules are already in `ms` (loaded by the caller)
 *   runtime_link      map, relocate, patch and bind everything
 *   runtime_run_main  create the main guest thread and call the entry point
 */
#ifndef PS5C_RUNTIME_H
#define PS5C_RUNTIME_H

#include "cpu/x64_native.h"
#include "hle/hle.h"
#include "loader/linker.h"
#include "loader/module_set.h"
#include "loader/nid_db.h"
#include "mem/gmem.h"

#define GUEST_MAIN_STACK_SIZE (8ull << 20)
#define GUEST_MIN_STACK_SIZE (64ull << 10)
#define GUEST_TCB_SIZE 0x100u
#define GUEST_THREAD_KEYS 512

/* Exit codes returned by runtime_run_main when the guest didn't finish. */
#define RUNTIME_EXIT_MISSING_IMPORT 0x70000001
#define RUNTIME_EXIT_STACK_SMASH 0x70000002

typedef struct pal_thread pal_thread;
typedef struct pal_cond pal_cond;

#define GUEST_THREAD_CLEANUPS 16

/* A guest thread. Its address is the guest's pthread_t / ScePthread handle. */
typedef struct guest_thread {
    uint32_t id;
    guest_addr stack_base;
    uint64_t stack_size;
    guest_addr tls_block;
    uint64_t tls_block_size;
    guest_addr tcb; /* thread pointer (fs base on real hardware) */
    guest_addr dtv;
    guest_addr errno_addr; /* int returned by __error() */
    x64_enter_ctx enter; /* live while the thread runs guest code */
    int32_t priority;    /* 0 = default */
    uint64_t affinity;   /* 0 = default */
    char name[32];
    uint64_t specific[GUEST_THREAD_KEYS]; /* pthread_getspecific values */
    struct ps5_runtime *rt;

    /* Lifecycle (protected by rt->threads_lock) */
    guest_addr entry;       /* start routine */
    uint64_t arg;
    pal_thread *host;       /* NULL for the main thread */
    bool stack_owned;       /* stack mapped by us (not from the attribute) */
    bool detached;
    bool finished;
    bool joining;           /* someone is waiting in join */
    bool start_ready;       /* handle and attributes published; threads_lock */
    uint64_t retval;        /* start routine result or pthread_exit value */
    /* Set when guest code must stop on this thread */
    bool exit_requested;
    bool thread_exit;       /* pthread_exit (only this thread) vs process exit */
    int64_t exit_code;

    /* pthread_cancel, deferred: the thread ends with PTHREAD_CANCELED when one of
     * its own HLE calls that is a cancellation point returns. Only the thread
     * itself writes the state and type. */
    volatile bool cancel_pending;
    bool cancel_disabled; /* PTHREAD_CANCEL_DISABLE */
    bool cancel_async;    /* PTHREAD_CANCEL_ASYNCHRONOUS: every HLE call is a point */
    /* pthread_cleanup_push handlers, run last pushed first when the thread
     * exits or is cancelled */
    struct { guest_addr fn; uint64_t arg; } cleanup[GUEST_THREAD_CLEANUPS];
    uint32_t cleanup_count;

    /* Profiling (PS5C_SAMPLE) */
    uintptr_t sample_handle;
    const struct hle_stub_info *volatile in_hle; /* HLE call in progress */
    volatile guest_addr hle_ret, hle_fp;
    volatile uint64_t hle_arg0; /* first argument of the current HLE call (thread dumps) */

    /* Asynchronous exceptions (sceKernelRaiseException). A handler runs on a
     * proxy host thread that stands in for this thread while it is frozen:
     * suspended in guest code, or held on return from its current HLE call. */
    struct guest_thread *proxy_of; /* set on proxies: the thread they stand in for */
    pal_mutex *sig_lock;
    pal_cond *sig_cond;
    uint32_t sig_hold; /* handlers running for this thread */
} guest_thread;

/* The thread whose identity guest code sees (pthread_self, TLS keys). */
static inline guest_thread *guest_thread_identity(guest_thread *t) {
    return t && t->proxy_of ? t->proxy_of : t;
}

typedef struct runtime_options {
    bool stop_on_missing; /* bail out on the first call to an unimplemented import */
    bool trace_hle;       /* log every HLE call */
    bool direct_memory_window; /* RADV PS5 shader address32 compatibility */
    uint32_t missing_log_limit; /* per import */
    const nid_db *names;        /* optional: symbol names for logs */
    /* Optional installation backend: true means the locus is known (0 absent,
     * 2 local slow, 3 local fast). Called under threads_lock; do not reacquire it.
     * NULL means installation is unknown, not that payload is absent. */
    bool (*playgo_query_locus)(void *user, uint16_t chunk_id, uint8_t *locus);
    void *playgo_query_user;
} runtime_options;

typedef struct ps5_runtime {
    module_set *ms;
    struct playgo_state *playgo; /* per-process; protected by threads_lock */
    struct libc_state *libc; /* HLE firmware libc, separate from game libc.prx */
    gmem mem;
    linker lk;
    x64_native cpu;
    runtime_options opts;
    uint32_t tcb_slot;    /* host TLS: current guest thread's TCB (read by patched code) */
    uint32_t thread_slot; /* host TLS: current guest_thread* */
    DYNARRAY(hle_stub_info) stubs; /* parallel to lk.stubs */
    guest_addr exit_handler;       /* guest-callable address passed to the entry point */
    guest_addr process_params;
    guest_addr sandbox_word;
    uint32_t next_thread_id;
    uint32_t tls_patches;
    uint64_t hle_calls;
    pal_thread *sampler;
    pal_thread *watchdog; /* logs the threads when no frame is shown for a while */
    uint64_t missing_calls;
    guest_addr thread_dtors, thread_atexit_count, thread_atexit_report; /* libc hooks */
    guest_addr app_heap_api;    /* _sceKernelRtldSetApplicationHeapAPI */
    guest_addr heap_trace_area; /* sceLibcHeapGetTraceInfo tables */
    pal_mutex *threads_lock;
    pal_cond *threads_changed;
    DYNARRAY(guest_thread *) threads; /* live and unjoined threads */
    guest_addr exception_handlers[128]; /* sceKernelInstallExceptionHandler, by signal - 1 */
    DYNARRAY(struct sig_delivery *) deliveries; /* protected by threads_lock */
    struct acm_state *acm; /* per-process ACM contexts, protected by threads_lock */
} ps5_runtime;

ps5c_err runtime_init(ps5_runtime *rt, module_set *ms, const runtime_options *opts);
void runtime_free(ps5_runtime *rt);
ps5c_err runtime_link(ps5_runtime *rt);
/* Runs the main executable's entry point until it returns or exits. */
ps5c_err runtime_run_main(ps5_runtime *rt, int64_t *exit_code);

/* Logs the frame-pointer backtrace of the current guest thread's HLE caller. */
void runtime_log_backtrace(ps5_runtime *rt);
/* PS5C_WATCH_WRITE debugging aid: (re)arms the watch on its guest page. */
void runtime_watch_arm(ps5_runtime *rt);
void runtime_watch_move(gmem *m, guest_addr addr);
guest_addr runtime_debug_addr(void);
/* Current guest thread (NULL on non-guest threads). */
guest_thread *runtime_current_thread(ps5_runtime *rt);
/* Calls guest fn(arg0, arg1, arg2) from HLE code running on the current guest
 * thread, below the current frame of its stack. False if the guest exited. */
bool runtime_call_nested(ps5_runtime *rt, guest_addr fn, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t *rax);
/* The same with a fourth argument */
bool runtime_call_nested4(ps5_runtime *rt, guest_addr fn, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                          uint64_t *rax);
/* Ends the process with `code` (exit/_exit, fatal guest errors). On the main
 * thread runtime_run_main returns `code`; elsewhere the host process exits. */
void runtime_exit_thread(ps5_runtime *rt, int64_t code);

/* ---- Guest threads (errors are FreeBSD errno values) ---- */
/* Creates a host thread held before entry(arg). stack_addr 0 = allocate the
 * stack. Publish its guest handle/attributes, then runtime_release_thread. */
int runtime_start_thread(ps5_runtime *rt, guest_addr entry, uint64_t arg, guest_addr stack_addr,
                         uint64_t stack_size, bool detached, const char *name, guest_thread **out);
void runtime_release_thread(ps5_runtime *rt, guest_thread *t);
/* The live thread for a guest handle, or NULL. */
guest_thread *runtime_lookup_thread(ps5_runtime *rt, uint64_t handle);
int runtime_join_thread(ps5_runtime *rt, uint64_t handle, uint64_t *retval);
int runtime_detach_thread(ps5_runtime *rt, uint64_t handle);
/* pthread_exit: ends only the calling thread. */
void runtime_exit_current_thread(ps5_runtime *rt, uint64_t retval);
/* pthread_cancel: marks the thread, which ends at its next cancellation point.
 * 0, or ESRCH for an unknown handle. */
int runtime_cancel_thread(ps5_runtime *rt, uint64_t handle);
/* True for the names of HLE functions that are cancellation points. */
bool runtime_is_cancel_point(const char *name);
/* Runs the installed handler for `sig` on behalf of thread `handle` while that
 * thread is frozen. Returns a FreeBSD errno value. */
int runtime_raise_exception(ps5_runtime *rt, uint64_t handle, int sig);
/* Starts the stall watchdog, and the sampling profiler when PS5C_SAMPLE is set. */
void runtime_start_sampler(ps5_runtime *rt);
/* Stack range [lo, hi) of the thread whose stack contains addr. */
bool runtime_find_stack(ps5_runtime *rt, guest_addr addr, guest_addr *lo, guest_addr *hi);
/* Threads started with runtime_start_thread that have not finished. */
uint32_t runtime_running_threads(ps5_runtime *rt);

/* Resolves a symbol by name (or "NID#L#M"-style NID) the way the dynamic
 * linker would: exports of loaded modules first, then HLE functions (a stub is
 * created on demand). 0 if unknown. */
guest_addr runtime_resolve_symbol(ps5_runtime *rt, const char *name);
/* The same, looking first in module `handle` (index + 1, as sceKernelLoadStartModule
 * returns it): modules that export the same names (PSPHD's per-SKU AOT libraries)
 * each answer for themselves. */
guest_addr runtime_resolve_symbol_in(ps5_runtime *rt, uint32_t handle, const char *name);

/* Module that contains `addr`, or NULL. */
const loaded_module *runtime_find_module(ps5_runtime *rt, guest_addr addr);

#endif
