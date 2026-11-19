/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceFiber: user-scheduled contexts on the calling thread.
 *
 * A fiber runs on the context memory (its stack) the game gives it. Guest
 * code runs natively, so a switch is a real stack switch
 * (x64_native_switch). It happens inside the HLE call that asks for it: the
 * context left behind is parked inside that call and resumes by returning
 * from it. A fiber that has never run starts at the fiber boot glue, which
 * calls entry(argOnInitialize, argOnRun).
 *
 * sceFiberRun is called by a thread; the fiber then runs on that thread
 * until it calls sceFiberReturnToThread, possibly passing the thread to other
 * fibers with sceFiberSwitch on the way. */
#include <string.h>
#include <stdatomic.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/hle.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

#define FIBER_ERR_NULL ((int32_t)0x80590001)
#define FIBER_ERR_ALIGNMENT ((int32_t)0x80590002)
#define FIBER_ERR_RANGE ((int32_t)0x80590003)
#define FIBER_ERR_INVALID ((int32_t)0x80590004)
#define FIBER_ERR_PERMISSION ((int32_t)0x80590005)
#define FIBER_ERR_STATE ((int32_t)0x80590006)

#define MAX_FIBERS 1024
#define CONTEXT_MIN 512 /* the switch frame plus room for the entry to start */

enum { FIBER_FREE, FIBER_INIT, FIBER_IDLE, FIBER_RUNNING };

typedef struct fiber {
    guest_addr addr; /* the SceFiber object */
    _Atomic uint32_t state;
    guest_addr entry;
    uint64_t arg_init;
    guest_addr context, context_size;
    uint64_t rsp;          /* saved stack pointer while not running */
    guest_addr arg_run_to; /* where the argument of the next run goes (0: none) */
    char name[32];
} fiber;

static struct {
    pal_mutex *lock;
    fiber fibers[MAX_FIBERS];
} g_fib;

/* Per thread: the fiber running on it and the parked thread context. */
static _Thread_local fiber *t_current;
static _Thread_local uint64_t t_thread_rsp;
static _Thread_local guest_addr t_return_arg; /* sceFiberRun's argOnReturn */

static fiber *lookup_locked(guest_addr addr) {
    if (!addr) return NULL;
    fiber *f = NULL;
    for (int i = 0; i < MAX_FIBERS && !f; i++) {
        if (g_fib.fibers[i].state != FIBER_FREE && g_fib.fibers[i].addr == addr) f = &g_fib.fibers[i];
    }
    return f;
}
static fiber *lookup(guest_addr addr) {
    pal_mutex_lock(g_fib.lock);fiber *f=lookup_locked(addr);pal_mutex_unlock(g_fib.lock);return f;
}
/* Claim under the same lock as initialize/finalize. Returning a pointer
 * from lookup alone does not reserve its suspended context. */
static fiber *claim(guest_addr addr,bool *first,int32_t *error) {
    pal_mutex_lock(g_fib.lock);fiber *f=lookup_locked(addr);
    *error=!f?FIBER_ERR_INVALID:(f->state!=FIBER_INIT && f->state!=FIBER_IDLE)?FIBER_ERR_STATE:0;
    if(!*error){*first=f->state==FIBER_INIT;f->state=FIBER_RUNNING;}
    pal_mutex_unlock(g_fib.lock);return *error?NULL:f;
}

static void put_u64(hle_call *c, guest_addr addr, uint64_t v) {
    uint64_t *p = addr ? (uint64_t *)hle_mem(c, addr, 8) : NULL;
    if (p) *p = v;
}

/* Frame for a fiber's first switch: starts at the boot glue. */
static bool build_start_frame(hle_call *c, fiber *f, uint64_t arg_run) {
    guest_addr top = (f->context + f->context_size) & ~(guest_addr)15;
    guest_addr sp = top - 256;
    uint8_t *frame = (uint8_t *)hle_mem(c, sp, X64_SWITCH_FRAME);
    if (!frame) return false;
    memset(frame, 0, X64_SWITCH_FRAME);
    uint32_t mxcsr = 0x1F80;
    uint16_t fpucw = 0x037F;
    memcpy(frame + 160, &mxcsr, 4);
    memcpy(frame + 164, &fpucw, 2);
    uint64_t v = f->entry;
    memcpy(frame + X64_SWITCH_R12, &v, 8);
    memcpy(frame + X64_SWITCH_R13, &f->arg_init, 8);
    memcpy(frame + X64_SWITCH_R14, &arg_run, 8);
    v = c->rt->cpu.fiber_boot_addr;
    memcpy(frame + X64_SWITCH_RET, &v, 8);
    f->rsp = sp;
    return true;
}

/* Hands the thread to `to` and parks the current context in *save; returns
 * when something switches back to it. */
static bool switch_to(hle_call *c,uint64_t *save,fiber *to,uint64_t arg_run,bool first,fiber *from) {
    guest_thread *t = c->thread;
    const struct hle_stub_info *in_hle = t ? t->in_hle : NULL;
    guest_addr hle_ret = t ? t->hle_ret : 0, hle_fp = t ? t->hle_fp : 0;
    if (first) {
        if(!build_start_frame(c,to,arg_run)){to->state=FIBER_INIT;return false;}
        if (t) t->in_hle = NULL; /* the new fiber starts in guest code */
    } else {
        put_u64(c, to->arg_run_to, arg_run);
    }
    t_current = to;
    /* Leave the outgoing context RUNNING until its stack is sealed. A
     * premature IDLE lets another thread resume or destroy that live stack. */
    x64_native_switch(&c->rt->cpu,save,to->rsp,
        from?(volatile uint32_t *)&from->state:NULL,FIBER_IDLE);
    /* resumed: back inside this call */
    if (t) {
        /* The parked fiber may have been claimed by another host thread.
         * Both this restoration and hle_dispatch's leave belong to its
         * current thread, not the one that originally parked the stack. */
        c->thread = t = runtime_current_thread(c->rt);
        if (t) {
            t->in_hle = in_hle;
            t->hle_ret = hle_ret;
            t->hle_fp = hle_fp;
        }
    }
    return true;
}

/* int _sceFiberInitializeImpl(SceFiber *fiber, const char *name, entry, uint64_t argOnInitialize,
 *                             void *addrContext, uint64_t sizeContext, const SceFiberOptParam *opt, ...) */
static void fiber_initialize(hle_call *c) {
    guest_addr addr = hle_arg_ptr(c, 0), name = hle_arg_ptr(c, 1), entry = hle_arg_ptr(c, 2);
    guest_addr context = hle_arg_ptr(c, 4), size = hle_arg(c, 5);
    if (!addr || !entry) {
        hle_ret_s32(c, FIBER_ERR_NULL);
        return;
    }
    if ((addr & 7) || (context & 15)) {
        hle_ret_s32(c, FIBER_ERR_ALIGNMENT);
        return;
    }
    if (!context || size < CONTEXT_MIN) {
        LOG_W(HLE, "sceFiberInitialize: context 0x%llx size 0x%llx not supported", (unsigned long long)context,
              (unsigned long long)size);
        hle_ret_s32(c, FIBER_ERR_RANGE);
        return;
    }
    pal_mutex_lock(g_fib.lock);
    fiber *f = NULL;
    for (int i = 0; i < MAX_FIBERS && !f; i++) {
        if (g_fib.fibers[i].state != FIBER_FREE && g_fib.fibers[i].addr == addr) f = &g_fib.fibers[i];
    }
    for (int i = 0; i < MAX_FIBERS && !f; i++) {
        if (g_fib.fibers[i].state == FIBER_FREE) f = &g_fib.fibers[i];
    }
    if(f && f->state==FIBER_RUNNING){pal_mutex_unlock(g_fib.lock);hle_ret_s32(c,FIBER_ERR_STATE);return;}
    if (f) {
        memset(f, 0, sizeof(*f));
        f->addr = addr;
        f->state = FIBER_INIT;
        f->entry = entry;
        f->arg_init = hle_arg(c, 3);
        f->context = context;
        f->context_size = size;
        const char *n = name ? (const char *)hle_mem(c, name, 1) : NULL;
        if (n) {
            for (size_t i = 0; i + 1 < sizeof(f->name) && n[i]; i++) f->name[i] = n[i];
        }
    }
    pal_mutex_unlock(g_fib.lock);
    if (!f) {
        hle_ret_s32(c, FIBER_ERR_RANGE);
        return;
    }
    LOG_D(HLE, "fiber '%s' at 0x%llx: entry 0x%llx, context 0x%llx+0x%llx", f->name, (unsigned long long)addr,
          (unsigned long long)entry, (unsigned long long)context, (unsigned long long)size);
    hle_ret_s32(c, 0);
}

static void fiber_finalize(hle_call *c) {
    pal_mutex_lock(g_fib.lock);fiber *f = lookup_locked(hle_arg_ptr(c, 0));
    if (!f) {
        pal_mutex_unlock(g_fib.lock);
        hle_ret_s32(c, FIBER_ERR_INVALID);
        return;
    }
    if (f->state == FIBER_RUNNING) {
        pal_mutex_unlock(g_fib.lock);
        hle_ret_s32(c, FIBER_ERR_STATE);
        return;
    }
    f->state = FIBER_FREE;
    pal_mutex_unlock(g_fib.lock);
    hle_ret_s32(c, 0);
}

/* int sceFiberRun(SceFiber *fiber, uint64_t argOnRunTo, uint64_t *argOnReturn) */
static void fiber_run(hle_call *c) {
    if (t_current) {
        hle_ret_s32(c,lookup(hle_arg_ptr(c,0))?FIBER_ERR_PERMISSION:FIBER_ERR_INVALID);
        return;
    }
    bool first=false;int32_t error=0;fiber *f=claim(hle_arg_ptr(c,0),&first,&error);
    if(!f){hle_ret_s32(c,error);return;}
    t_return_arg = hle_arg_ptr(c, 2);
    hle_ret_s32(c,switch_to(c,&t_thread_rsp,f,hle_arg(c,1),first,NULL)?0:FIBER_ERR_RANGE);
}

/* int sceFiberSwitch(SceFiber *fiber, uint64_t argOnRunTo, uint64_t *argOnRun) */
static void fiber_switch(hle_call *c) {
    fiber *from = t_current;
    if (!from) {
        hle_ret_s32(c,lookup(hle_arg_ptr(c,0))?FIBER_ERR_PERMISSION:FIBER_ERR_INVALID);
        return;
    }
    bool first=false;int32_t error=0;fiber *to=claim(hle_arg_ptr(c,0),&first,&error);
    if(!to){hle_ret_s32(c,error);return;}
    from->arg_run_to = hle_arg_ptr(c, 2);
    hle_ret_s32(c,switch_to(c,&from->rsp,to,hle_arg(c,1),first,from)?0:FIBER_ERR_RANGE);
}

/* int sceFiberReturnToThread(uint64_t argOnReturn, uint64_t *argOnRun) */
static void fiber_return_to_thread(hle_call *c) {
    fiber *f = t_current;
    if (!f) {
        hle_ret_s32(c, FIBER_ERR_PERMISSION);
        return;
    }
    guest_thread *t = c->thread;
    const struct hle_stub_info *in_hle = t ? t->in_hle : NULL;
    guest_addr hle_ret = t ? t->hle_ret : 0, hle_fp = t ? t->hle_fp : 0;
    f->arg_run_to = hle_arg_ptr(c, 1);
    put_u64(c, t_return_arg, hle_arg(c, 0));
    t_current = NULL;
    x64_native_switch(&c->rt->cpu,&f->rsp,t_thread_rsp,(volatile uint32_t *)&f->state,FIBER_IDLE);
    /* run again later */
    if (t) {
        c->thread = t = runtime_current_thread(c->rt);
        if (t) {
            t->in_hle = in_hle;
            t->hle_ret = hle_ret;
            t->hle_fp = hle_fp;
        }
    }
    hle_ret_s32(c, 0);
}

/* int sceFiberGetSelf(SceFiber **fiber) */
static void fiber_get_self(hle_call *c) {
    if (!t_current) {
        hle_ret_s32(c, FIBER_ERR_PERMISSION);
        return;
    }
    put_u64(c, hle_arg_ptr(c, 0), t_current->addr);
    hle_ret_s32(c, 0);
}

/* SceFiberInfo: size, entry, argOnInitialize, addrContext, sizeContext, name[32], sizeContextMargin */
static void fiber_get_info(hle_call *c) {
    fiber *f = lookup(hle_arg_ptr(c, 0));
    uint8_t *info = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 1), 80);
    if (!f || !info) {
        hle_ret_s32(c, f ? FIBER_ERR_NULL : FIBER_ERR_INVALID);
        return;
    }
    uint64_t v[5] = {80, f->entry, f->arg_init, f->context, f->context_size};
    memcpy(info, v, sizeof(v));
    memcpy(info + 40, f->name, 32);
    uint64_t margin = 0;
    memcpy(info + 72, &margin, 8);
    hle_ret_s32(c, 0);
}

static void fiber_rename(hle_call *c) {
    fiber *f = lookup(hle_arg_ptr(c, 0));
    const char *n = hle_arg_ptr(c, 1) ? (const char *)hle_mem(c, hle_arg_ptr(c, 1), 1) : NULL;
    if (!f) {
        hle_ret_s32(c, FIBER_ERR_INVALID);
        return;
    }
    memset(f->name, 0, sizeof(f->name));
    for (size_t i = 0; n && i + 1 < sizeof(f->name) && n[i]; i++) f->name[i] = n[i];
    hle_ret_s32(c, 0);
}

/* SceFiberOptParam: reserved bytes */
static void fiber_opt_param_initialize(hle_call *c) {
    uint8_t *p = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 128);
    if (p) memset(p, 0, 128);
    hle_ret_s32(c, p ? 0 : FIBER_ERR_NULL);
}

static void ok(hle_call *c) { hle_ret_s32(c, 0); }

#define L "libSceFiber"

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(L, "_sceFiberInitializeImpl", fiber_initialize),
    HLE_FUNC_ENTRY(L, "_sceFiberInitializeWithInternalOptionImpl", fiber_initialize),
    HLE_FUNC_ENTRY(L, "sceFiberFinalize", fiber_finalize),
    HLE_FUNC_ENTRY(L, "sceFiberRun", fiber_run),
    HLE_FUNC_ENTRY(L, "sceFiberSwitch", fiber_switch),
    HLE_FUNC_ENTRY(L, "sceFiberReturnToThread", fiber_return_to_thread),
    HLE_FUNC_ENTRY(L, "sceFiberGetSelf", fiber_get_self),
    HLE_FUNC_ENTRY(L, "sceFiberGetInfo", fiber_get_info),
    HLE_FUNC_ENTRY(L, "sceFiberRename", fiber_rename),
    HLE_FUNC_ENTRY(L, "sceFiberOptParamInitialize", fiber_opt_param_initialize),
    HLE_FUNC_ENTRY(L, "sceFiberStartContextSizeCheck", ok),
    HLE_FUNC_ENTRY(L, "sceFiberStopContextSizeCheck", ok),
};

void hle_register_fiber(void) {
    if (!g_fib.lock && pal_mutex_create(&g_fib.lock) != PS5C_OK) {
        LOG_E(HLE, "cannot create the fiber lock");
        return;
    }
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}
