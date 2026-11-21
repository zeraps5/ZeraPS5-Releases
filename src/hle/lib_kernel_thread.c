/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libkernel threads: attributes and per-thread properties.
 * (Thread creation comes with multi-threading support.) */
#include <stdio.h>
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/hle.h"
#include "hle/kernel_objects.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

#define G_EINVAL 22
#define G_ENOMEM 12
#define G_ESRCH 3
#define SCE_ERR(e) ((int32_t)(0x80020000u | (uint32_t)(e)))

/* Scheduling defaults of a normal-priority game thread */
#define SCHED_FIFO 1
#define SCHED_OTHER 2
#define SCHED_RR 3
#define INHERIT_SCHED 4
#define EXPLICIT_SCHED 0
#define DEFAULT_PRIORITY 700
#define DEFAULT_AFFINITY 0x7Full /* the 7 cores a game may use */
#define DEFAULT_STACK_SIZE 0x10000ull
#define DEFAULT_GUARD_SIZE 0x1000ull

typedef struct threadattr_obj {
    obj_kind kind;
    uint64_t stack_addr, stack_size, guard_size;
    int32_t detach_state;
    int32_t sched_policy, inherit_sched, priority;
    int32_t solo_sched; /* recorded for the getter: the host schedules */
    uint64_t affinity;
} threadattr_obj;

static threadattr_obj *attr_get(hle_call *c, guest_addr h) {
    uint64_t *slot = (uint64_t *)hle_mem(c, h, 8);
    return slot ? (threadattr_obj *)kobj_check(*slot, OBJ_THREADATTR) : NULL;
}

static int do_attr_init(hle_call *c, guest_addr h) {
    uint64_t *slot = (uint64_t *)hle_mem(c, h, 8);
    if (!slot) return G_EINVAL;
    threadattr_obj *a = (threadattr_obj *)kobj_new(OBJ_THREADATTR, sizeof(threadattr_obj));
    if (!a) return G_ENOMEM;
    a->stack_size = DEFAULT_STACK_SIZE;
    a->guard_size = DEFAULT_GUARD_SIZE;
    a->sched_policy = SCHED_FIFO;
    a->inherit_sched = INHERIT_SCHED;
    a->priority = DEFAULT_PRIORITY;
    a->affinity = DEFAULT_AFFINITY;
    *slot = (uint64_t)(uintptr_t)a;
    return 0;
}

static int do_attr_destroy(hle_call *c, guest_addr h) {
    threadattr_obj *a = attr_get(c, h);
    if (!a) return G_EINVAL;
    kobj_delete(a);
    *(uint64_t *)hle_mem(c, h, 8) = 0;
    return 0;
}

/* Fills an attribute object with the properties of a running thread. */
static int do_attr_get(hle_call *c, uint64_t thread, guest_addr h) {
    threadattr_obj *a = attr_get(c, h);
    if (!a) return G_EINVAL;
    guest_thread *t = runtime_lookup_thread(c->rt, thread);
    if (!t) return G_ESRCH;
    a->stack_addr = t->stack_base;
    a->stack_size = t->stack_size;
    a->guard_size = 0;
    a->detach_state = 0;
    return 0;
}

static int store_u64(hle_call *c, guest_addr p, uint64_t v) {
    uint64_t *out = (uint64_t *)hle_mem(c, p, 8);
    if (!out) return G_EINVAL;
    *out = v;
    return 0;
}

static int store_s32(hle_call *c, guest_addr p, int32_t v) {
    int32_t *out = (int32_t *)hle_mem(c, p, 4);
    if (!out) return G_EINVAL;
    *out = v;
    return 0;
}

/* Getters/setters share one shape: (attr, value-or-pointer). */
typedef enum {
    F_STACKSIZE, F_STACKADDR, F_GUARDSIZE, F_DETACH, F_POLICY, F_INHERIT, F_PRIORITY, F_AFFINITY, F_SOLO
} attr_field;

static int attr_getter(hle_call *c, attr_field f) {
    threadattr_obj *a = attr_get(c, hle_arg_ptr(c, 0));
    if (!a) return G_EINVAL;
    guest_addr out = hle_arg_ptr(c, 1);
    switch (f) {
    case F_STACKSIZE: return store_u64(c, out, a->stack_size);
    case F_STACKADDR: return store_u64(c, out, a->stack_addr);
    case F_GUARDSIZE: return store_u64(c, out, a->guard_size);
    case F_DETACH: return store_s32(c, out, a->detach_state);
    case F_POLICY: return store_s32(c, out, a->sched_policy);
    case F_INHERIT: return store_s32(c, out, a->inherit_sched);
    case F_PRIORITY: return store_s32(c, out, a->priority); /* struct sched_param { int prio; } */
    case F_AFFINITY: return store_u64(c, out, a->affinity);
    case F_SOLO: return store_s32(c, out, a->solo_sched);
    }
    return G_EINVAL;
}

static int attr_setter(hle_call *c, attr_field f) {
    threadattr_obj *a = attr_get(c, hle_arg_ptr(c, 0));
    if (!a) return G_EINVAL;
    uint64_t v = hle_arg(c, 1);
    switch (f) {
    case F_STACKSIZE:
        if (v < 0x4000) return G_EINVAL;
        a->stack_size = v;
        return 0;
    case F_STACKADDR: a->stack_addr = v; return 0;
    case F_GUARDSIZE: a->guard_size = v; return 0;
    case F_DETACH:
        if ((int32_t)v != 0 && (int32_t)v != 1) return G_EINVAL;
        a->detach_state = (int32_t)v;
        return 0;
    case F_POLICY:
        if ((int32_t)v < SCHED_FIFO || (int32_t)v > SCHED_RR) return G_EINVAL;
        a->sched_policy = (int32_t)v;
        return 0;
    case F_INHERIT:
        if ((int32_t)v != INHERIT_SCHED && (int32_t)v != EXPLICIT_SCHED) return G_EINVAL;
        a->inherit_sched = (int32_t)v;
        return 0;
    case F_PRIORITY: {
        const int32_t *param = (const int32_t *)hle_mem(c, v, 4);
        if (!param) return G_EINVAL;
        a->priority = *param;
        return 0;
    }
    case F_AFFINITY: a->affinity = v; return 0;
    case F_SOLO: a->solo_sched = (int32_t)v != 0; return 0;
    }
    return G_EINVAL;
}

static int attr_getstack(hle_call *c) {
    threadattr_obj *a = attr_get(c, hle_arg_ptr(c, 0));
    if (!a) return G_EINVAL;
    int err = store_u64(c, hle_arg_ptr(c, 1), a->stack_addr);
    return err ? err : store_u64(c, hle_arg_ptr(c, 2), a->stack_size);
}

static int attr_setstack(hle_call *c) {
    threadattr_obj *a = attr_get(c, hle_arg_ptr(c, 0));
    if (!a) return G_EINVAL;
    a->stack_addr = hle_arg(c, 1);
    a->stack_size = hle_arg(c, 2);
    return 0;
}

#define RET_POSIX(expr) hle_ret_s32(c, (expr))
#define RET_SCE(expr)                                                                                     \
    do {                                                                                                  \
        int e_ = (expr);                                                                                  \
        hle_ret_s32(c, e_ ? SCE_ERR(e_) : 0);                                                             \
    } while (0)

/* Generates the sce and POSIX wrappers of one attribute field. */
#define ATTR_ACCESSORS(tag, field)                                                                        \
    static void sce_get_##tag(hle_call *c) { RET_SCE(attr_getter(c, field)); }                             \
    static void sce_set_##tag(hle_call *c) { RET_SCE(attr_setter(c, field)); }                             \
    static void px_get_##tag(hle_call *c) { RET_POSIX(attr_getter(c, field)); }                            \
    static void px_set_##tag(hle_call *c) { RET_POSIX(attr_setter(c, field)); }

ATTR_ACCESSORS(stacksize, F_STACKSIZE)
ATTR_ACCESSORS(stackaddr, F_STACKADDR)
ATTR_ACCESSORS(guardsize, F_GUARDSIZE)
ATTR_ACCESSORS(detachstate, F_DETACH)
ATTR_ACCESSORS(schedpolicy, F_POLICY)
ATTR_ACCESSORS(inheritsched, F_INHERIT)
ATTR_ACCESSORS(schedparam, F_PRIORITY)
ATTR_ACCESSORS(solosched, F_SOLO)
/* POSIX affinity uses cpuset_t (pthread_attr_*affinity_np): SCE flavour only. */
static void sce_get_affinity(hle_call *c) { RET_SCE(attr_getter(c, F_AFFINITY)); }
static void sce_set_affinity(hle_call *c) { RET_SCE(attr_setter(c, F_AFFINITY)); }

static void sce_attr_init(hle_call *c) { RET_SCE(do_attr_init(c, hle_arg_ptr(c, 0))); }
static void sce_attr_destroy(hle_call *c) { RET_SCE(do_attr_destroy(c, hle_arg_ptr(c, 0))); }
static void sce_attr_get(hle_call *c) { RET_SCE(do_attr_get(c, hle_arg(c, 0), hle_arg_ptr(c, 1))); }
static void sce_attr_getstack(hle_call *c) { RET_SCE(attr_getstack(c)); }
static void sce_attr_setstack(hle_call *c) { RET_SCE(attr_setstack(c)); }
static void px_attr_init(hle_call *c) { RET_POSIX(do_attr_init(c, hle_arg_ptr(c, 0))); }
static void px_attr_destroy(hle_call *c) { RET_POSIX(do_attr_destroy(c, hle_arg_ptr(c, 0))); }
static void px_attr_get_np(hle_call *c) { RET_POSIX(do_attr_get(c, hle_arg(c, 0), hle_arg_ptr(c, 1))); }
static void px_attr_getstack(hle_call *c) { RET_POSIX(attr_getstack(c)); }
static void px_attr_setstack(hle_call *c) { RET_POSIX(attr_setstack(c)); }

/* ---- Properties of running threads ---- */

static guest_thread *thread_arg(hle_call *c, uint32_t i) { return runtime_lookup_thread(c->rt, hle_arg(c, i)); }

/* ---- Creation, join, detach, exit ---- */

/* create(thread_out, attr, entry, arg, name) */
static int do_create(hle_call *c, guest_addr out_addr, guest_addr attr_h, guest_addr entry, uint64_t arg,
                     guest_addr name_addr) {
    uint64_t *out = (uint64_t *)hle_mem(c, out_addr, 8);
    if (!out || !entry) return G_EINVAL;
    uint64_t stack_size = DEFAULT_STACK_SIZE;
    guest_addr stack_addr = 0;
    bool detached = false;
    int32_t priority = 0;
    uint64_t affinity = 0;
    if (attr_h) {
        threadattr_obj *a = attr_get(c, attr_h);
        if (!a) return G_EINVAL;
        stack_size = a->stack_size;
        stack_addr = a->stack_addr;
        detached = a->detach_state == 1;
        if (a->inherit_sched == EXPLICIT_SCHED) priority = a->priority;
        affinity = a->affinity;
    }
    char name[32] = "";
    if (name_addr) hle_read_str(c, name_addr, name, sizeof(name));
    guest_thread *t = NULL;
    int err = runtime_start_thread(c->rt, entry, arg, stack_addr, stack_size, detached, name, &t);
    if (err) return err;
    t->priority = priority;
    t->affinity = affinity;
    *out = (uint64_t)(uintptr_t)t;
    runtime_release_thread(c->rt, t);
    return 0;
}

static void sce_create(hle_call *c) {
    RET_SCE(do_create(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1), hle_arg_ptr(c, 2), hle_arg(c, 3), hle_arg_ptr(c, 4)));
}
static void px_create(hle_call *c) {
    RET_POSIX(do_create(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1), hle_arg_ptr(c, 2), hle_arg(c, 3), 0));
}
static void px_create_name_np(hle_call *c) {
    RET_POSIX(do_create(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1), hle_arg_ptr(c, 2), hle_arg(c, 3), hle_arg_ptr(c, 4)));
}

static int do_join(hle_call *c) {
    uint64_t retval = 0;
    int err = runtime_join_thread(c->rt, hle_arg(c, 0), &retval);
    guest_addr out = hle_arg_ptr(c, 1);
    if (!err && out) err = store_u64(c, out, retval);
    return err;
}

static void sce_join(hle_call *c) { RET_SCE(do_join(c)); }
static void px_join(hle_call *c) { RET_POSIX(do_join(c)); }
static void sce_detach(hle_call *c) { RET_SCE(runtime_detach_thread(c->rt, hle_arg(c, 0))); }
static void px_detach(hle_call *c) { RET_POSIX(runtime_detach_thread(c->rt, hle_arg(c, 0))); }
static void thread_exit(hle_call *c) { runtime_exit_current_thread(c->rt, hle_arg(c, 0)); }

static void get_prio(hle_call *c) {
    guest_thread *t = thread_arg(c, 0);
    if (!t) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_ESRCH);
        return;
    }
    RET_SCE(store_s32(c, hle_arg_ptr(c, 1), t->priority ? t->priority : DEFAULT_PRIORITY));
}

static void set_prio(hle_call *c) {
    guest_thread *t = thread_arg(c, 0);
    if (!t) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_ESRCH);
        return;
    }
    t->priority = hle_arg_s32(c, 1);
    hle_ret_s32(c, 0);
}

static void get_affinity(hle_call *c) {
    guest_thread *t = thread_arg(c, 0);
    if (!t) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_ESRCH);
        return;
    }
    RET_SCE(store_u64(c, hle_arg_ptr(c, 1), t->affinity ? t->affinity : DEFAULT_AFFINITY));
}

static void set_affinity(hle_call *c) {
    guest_thread *t = thread_arg(c, 0);
    if (!t) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_ESRCH);
        return;
    }
    t->affinity = hle_arg(c, 1); /* host scheduling decides; recorded for getters */
    hle_ret_s32(c, 0);
}

/* int scePthreadGetschedparam(ScePthread t, int *policy, SceKernelSchedParam *param) */
static int do_get_schedparam(hle_call *c) {
    guest_thread *t = thread_arg(c, 0);
    if (!t) return G_ESRCH;
    int err = store_s32(c, hle_arg_ptr(c, 1), SCHED_FIFO);
    if (!err) err = store_s32(c, hle_arg_ptr(c, 2), t->priority ? t->priority : DEFAULT_PRIORITY);
    return err;
}
static void get_schedparam(hle_call *c) { RET_SCE(do_get_schedparam(c)); }
static void px_get_schedparam_of(hle_call *c) { RET_POSIX(do_get_schedparam(c)); }

/* ---- Cancellation (the runtime acts on it at the thread's cancellation points) ---- */
#define CANCEL_ENABLE 0
#define CANCEL_DISABLE 1
#define CANCEL_DEFERRED 0
#define CANCEL_ASYNCHRONOUS 2

static void sce_cancel(hle_call *c) { RET_SCE(runtime_cancel_thread(c->rt, hle_arg(c, 0))); }
static void px_cancel(hle_call *c) { RET_POSIX(runtime_cancel_thread(c->rt, hle_arg(c, 0))); }
/* the dispatcher treats these names as cancellation points when the call returns */
static void testcancel(hle_call *c) { hle_ret_s32(c, 0); }

/* int pthread_setcancelstate(int state, int *old) */
static int do_setcancelstate(hle_call *c) {
    guest_thread *t = c->thread;
    int32_t state = hle_arg_s32(c, 0);
    guest_addr old = hle_arg_ptr(c, 1);
    if (!t || (state != CANCEL_ENABLE && state != CANCEL_DISABLE)) return G_EINVAL;
    if (old && store_s32(c, old, t->cancel_disabled ? CANCEL_DISABLE : CANCEL_ENABLE)) return G_EINVAL;
    t->cancel_disabled = state == CANCEL_DISABLE;
    return 0;
}
/* int pthread_setcanceltype(int type, int *old) */
static int do_setcanceltype(hle_call *c) {
    guest_thread *t = c->thread;
    int32_t type = hle_arg_s32(c, 0);
    guest_addr old = hle_arg_ptr(c, 1);
    if (!t || (type != CANCEL_DEFERRED && type != CANCEL_ASYNCHRONOUS)) return G_EINVAL;
    if (old && store_s32(c, old, t->cancel_async ? CANCEL_ASYNCHRONOUS : CANCEL_DEFERRED)) return G_EINVAL;
    t->cancel_async = type == CANCEL_ASYNCHRONOUS;
    return 0;
}
static void sce_setcancelstate(hle_call *c) { RET_SCE(do_setcancelstate(c)); }
static void px_setcancelstate(hle_call *c) { RET_POSIX(do_setcancelstate(c)); }
static void sce_setcanceltype(hle_call *c) { RET_SCE(do_setcanceltype(c)); }
static void px_setcanceltype(hle_call *c) { RET_POSIX(do_setcanceltype(c)); }

/* void __pthread_cleanup_push_imp(void (*routine)(void *), void *arg, struct _pthread_cleanup_info *) and
 * void __pthread_cleanup_pop_imp(int execute): what the pthread_cleanup_push/pop macros expand to */
static void cleanup_push(hle_call *c) {
    guest_thread *t = c->thread;
    if (!t) return;
    if (t->cleanup_count == GUEST_THREAD_CLEANUPS) {
        LOG_W(KERNEL, "pthread_cleanup_push: more than %d handlers on thread %u, one dropped", GUEST_THREAD_CLEANUPS,
              t->id);
        return;
    }
    t->cleanup[t->cleanup_count].fn = hle_arg_ptr(c, 0);
    t->cleanup[t->cleanup_count].arg = hle_arg(c, 1);
    t->cleanup_count++;
}
static void cleanup_pop(hle_call *c) {
    guest_thread *t = c->thread;
    if (!t || !t->cleanup_count) return;
    uint32_t i = --t->cleanup_count;
    uint64_t rax;
    if (hle_arg_s32(c, 0) && t->cleanup[i].fn)
        runtime_call_nested(c->rt, t->cleanup[i].fn, t->cleanup[i].arg, 0, 0, &rax);
}

/* int scePthreadSetschedparam(ScePthread t, int policy, const SceKernelSchedParam *param) */
static void set_schedparam(hle_call *c) {
    guest_thread *t = thread_arg(c, 0);
    const int32_t *param = (const int32_t *)hle_mem(c, hle_arg_ptr(c, 2), 4);
    if (!t) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_ESRCH);
        return;
    }
    if (param) t->priority = *param;
    hle_ret_s32(c, 0);
}

/* int sceKernelIsStack(void *addr, void **start, void **end) */
static void is_stack(hle_call *c) {
    guest_addr lo = 0, hi = 0;
    runtime_find_stack(c->rt, hle_arg_ptr(c, 0), &lo, &hi);
    if (hle_arg_ptr(c, 1)) store_u64(c, hle_arg_ptr(c, 1), lo);
    if (hle_arg_ptr(c, 2)) store_u64(c, hle_arg_ptr(c, 2), hi);
    hle_ret_s32(c, 0);
}

static void get_thread_id(hle_call *c) { hle_ret_s32(c, c->thread ? (int32_t)c->thread->id : 0); }

static void rename_(hle_call *c) {
    guest_thread *t = thread_arg(c, 0);
    if (t) hle_read_str(c, hle_arg_ptr(c, 1), t->name, sizeof(t->name));
    hle_ret_s32(c, t ? 0 : SCE_KERNEL_ERROR_ESRCH);
}

/* int scePthreadGetname(ScePthread thread, char name[32]) */
static void get_name(hle_call *c) {
    guest_thread *t = thread_arg(c, 0);
    guest_addr out = hle_arg_ptr(c, 1);
    if (!t || !out) {
        hle_ret_s32(c, !out ? SCE_KERNEL_ERROR_EINVAL : SCE_KERNEL_ERROR_ESRCH);
        return;
    }
    char name[32];
    snprintf(name, sizeof(name), "%s", t->name);
    gmem_write(c->mem, out, name, strlen(name) + 1);
    hle_ret_s32(c, 0);
}

static void px_rename(hle_call *c) {
    guest_thread *t = thread_arg(c, 0);
    if (t) hle_read_str(c, hle_arg_ptr(c, 1), t->name, sizeof(t->name));
    RET_POSIX(t ? 0 : G_ESRCH);
}

/* sched_get_priority_max/min(policy): the console's range, 256 (highest) to 767 (lowest) */
static void prio_max(hle_call *c) { hle_ret_s32(c, 256); }
static void prio_min(hle_call *c) { hle_ret_s32(c, 767); }

static void yield_(hle_call *c) {
    pal_thread_yield();
    hle_ret_s32(c, 0);
}

/* ---- Thread-specific data (pthread keys) ---- */

typedef struct key_slot {
    bool used;
    guest_addr destructor;
} key_slot;

static key_slot g_keys[GUEST_THREAD_KEYS];

static int do_key_create(hle_call *c, guest_addr key_out, guest_addr destructor) {
    int32_t *out = (int32_t *)hle_mem(c, key_out, 4);
    if (!out) return G_EINVAL;
    kobj_lock();
    int32_t key = -1;
    for (int32_t i = 0; i < GUEST_THREAD_KEYS; i++) {
        if (!g_keys[i].used) {
            g_keys[i].used = true;
            g_keys[i].destructor = destructor;
            key = i;
            break;
        }
    }
    kobj_unlock();
    if (key < 0) return 35; /* EAGAIN */
    *out = key;
    return 0;
}

static void key_clear_values(ps5_runtime *rt, int32_t key);

static int do_key_delete(ps5_runtime *rt, int32_t key) {
    if (key < 0 || key >= GUEST_THREAD_KEYS || !g_keys[key].used) return G_EINVAL;
    kobj_lock();
    g_keys[key].used = false;
    g_keys[key].destructor = 0;
    kobj_unlock();
    key_clear_values(rt, key);
    return 0;
}

bool kernel_thread_take_specific(guest_thread *t, uint32_t *cursor, uint64_t *destructor, uint64_t *value) {
    for (uint32_t i = *cursor; t && i < GUEST_THREAD_KEYS; i++) {
        kobj_lock();
        guest_addr fn = g_keys[i].used ? g_keys[i].destructor : 0;
        kobj_unlock();
        uint64_t v = t->specific[i];
        if (!fn || !v) continue;
        t->specific[i] = 0;
        *cursor = i + 1;
        *destructor = fn;
        *value = v;
        return true;
    }
    return false;
}

/* A deleted key's values are gone in every thread: a key created later in the
 * same slot starts at NULL, not at an old thread's value. */
static void key_clear_values(ps5_runtime *rt, int32_t key) {
    if (!rt || !rt->threads_lock) return;
    pal_mutex_lock(rt->threads_lock);
    for (size_t i = 0; i < rt->threads.len; i++) rt->threads.data[i]->specific[key] = 0;
    pal_mutex_unlock(rt->threads_lock);
}

static void getspecific(hle_call *c) {
    int32_t key = hle_arg_s32(c, 0);
    guest_thread *t = guest_thread_identity(c->thread);
    bool ok = t && key >= 0 && key < GUEST_THREAD_KEYS;
    hle_ret_u64(c, ok ? t->specific[key] : 0);
}

static int do_setspecific(hle_call *c, int32_t key, uint64_t value) {
    guest_thread *t = guest_thread_identity(c->thread);
    if (!t || key < 0 || key >= GUEST_THREAD_KEYS || !g_keys[key].used) return G_EINVAL;
    t->specific[key] = value;
    return 0;
}

static void px_key_create(hle_call *c) { RET_POSIX(do_key_create(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1))); }
static void px_key_delete(hle_call *c) { RET_POSIX(do_key_delete(c->rt, hle_arg_s32(c, 0))); }
static void px_setspecific(hle_call *c) { RET_POSIX(do_setspecific(c, hle_arg_s32(c, 0), hle_arg(c, 1))); }
static void sce_key_create(hle_call *c) { RET_SCE(do_key_create(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1))); }
static void sce_key_delete(hle_call *c) { RET_SCE(do_key_delete(c->rt, hle_arg_s32(c, 0))); }
static void sce_setspecific(hle_call *c) { RET_SCE(do_setspecific(c, hle_arg_s32(c, 0), hle_arg(c, 1))); }

#define K "libkernel"
#define P "libScePosix"
#define POSIX_ENTRY(name, fn) HLE_FUNC_ENTRY(K, name, fn), HLE_FUNC_ENTRY(P, name, fn)
#define SCE_ATTR(Name, tag)                                                                               \
    HLE_FUNC_ENTRY(K, "scePthreadAttrGet" Name, sce_get_##tag), HLE_FUNC_ENTRY(K, "scePthreadAttrSet" Name, sce_set_##tag)
#define PX_ATTR(name, tag)                                                                                \
    POSIX_ENTRY("pthread_attr_get" name, px_get_##tag), POSIX_ENTRY("pthread_attr_set" name, px_set_##tag)

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(K, "scePthreadAttrInit", sce_attr_init),
    HLE_FUNC_ENTRY(K, "scePthreadAttrDestroy", sce_attr_destroy),
    HLE_FUNC_ENTRY(K, "scePthreadAttrGet", sce_attr_get),
    HLE_FUNC_ENTRY(K, "scePthreadAttrGetstack", sce_attr_getstack),
    HLE_FUNC_ENTRY(K, "scePthreadAttrSetstack", sce_attr_setstack),
    SCE_ATTR("stacksize", stacksize),
    SCE_ATTR("stackaddr", stackaddr),
    SCE_ATTR("guardsize", guardsize),
    SCE_ATTR("detachstate", detachstate),
    SCE_ATTR("schedpolicy", schedpolicy),
    SCE_ATTR("inheritsched", inheritsched),
    SCE_ATTR("schedparam", schedparam),
    SCE_ATTR("affinity", affinity),
    SCE_ATTR("solosched", solosched),

    POSIX_ENTRY("pthread_attr_init", px_attr_init),
    POSIX_ENTRY("pthread_attr_destroy", px_attr_destroy),
    POSIX_ENTRY("pthread_attr_get_np", px_attr_get_np),
    POSIX_ENTRY("pthread_attr_getstack", px_attr_getstack),
    POSIX_ENTRY("pthread_attr_setstack", px_attr_setstack),
    PX_ATTR("stacksize", stacksize),
    PX_ATTR("stackaddr", stackaddr),
    PX_ATTR("guardsize", guardsize),
    PX_ATTR("detachstate", detachstate),
    PX_ATTR("schedpolicy", schedpolicy),
    PX_ATTR("inheritsched", inheritsched),
    PX_ATTR("schedparam", schedparam),
    POSIX_ENTRY("pthread_attr_getsolosched_np", px_get_solosched),
    POSIX_ENTRY("pthread_attr_setsolosched_np", px_set_solosched),

    HLE_FUNC_ENTRY(K, "scePthreadCreate", sce_create),
    HLE_FUNC_ENTRY(K, "scePthreadJoin", sce_join),
    HLE_FUNC_ENTRY(K, "scePthreadDetach", sce_detach),
    HLE_FUNC_ENTRY(K, "scePthreadExit", thread_exit),
    POSIX_ENTRY("pthread_create", px_create),
    POSIX_ENTRY("pthread_create_name_np", px_create_name_np),
    POSIX_ENTRY("pthread_join", px_join),
    POSIX_ENTRY("pthread_detach", px_detach),
    POSIX_ENTRY("pthread_exit", thread_exit),
    HLE_FUNC_ENTRY(K, "scePthreadGetprio", get_prio),
    HLE_FUNC_ENTRY(K, "scePthreadSetprio", set_prio),
    HLE_FUNC_ENTRY(K, "scePthreadGetaffinity", get_affinity),
    HLE_FUNC_ENTRY(K, "scePthreadSetaffinity", set_affinity),
    HLE_FUNC_ENTRY(K, "scePthreadGetthreadid", get_thread_id),
    POSIX_ENTRY("pthread_getthreadid_np", get_thread_id),
    HLE_FUNC_ENTRY(K, "scePthreadGetschedparam", get_schedparam),
    HLE_FUNC_ENTRY(K, "scePthreadSetschedparam", set_schedparam),
    HLE_FUNC_ENTRY("libScePosix", "pthread_setschedparam", set_schedparam), /* priority only */
    POSIX_ENTRY("pthread_getschedparam", px_get_schedparam_of),
    HLE_FUNC_ENTRY(K, "scePthreadCancel", sce_cancel),
    HLE_FUNC_ENTRY(K, "scePthreadTestcancel", testcancel),
    HLE_FUNC_ENTRY(K, "scePthreadSetcancelstate", sce_setcancelstate),
    HLE_FUNC_ENTRY(K, "scePthreadSetcanceltype", sce_setcanceltype),
    POSIX_ENTRY("pthread_cancel", px_cancel),
    POSIX_ENTRY("pthread_testcancel", testcancel),
    POSIX_ENTRY("pthread_setcancelstate", px_setcancelstate),
    POSIX_ENTRY("pthread_setcanceltype", px_setcanceltype),
    POSIX_ENTRY("__pthread_cleanup_push_imp", cleanup_push),
    POSIX_ENTRY("__pthread_cleanup_pop_imp", cleanup_pop),
    HLE_FUNC_ENTRY(K, "sceKernelIsStack", is_stack),
    HLE_FUNC_ENTRY(K, "scePthreadRename", rename_),
    HLE_FUNC_ENTRY(K, "pthread_set_name_np", rename_), /* same arguments (Burnout Revenge) */
    HLE_FUNC_ENTRY(K, "scePthreadGetname", get_name),
    HLE_FUNC_ENTRY(K, "scePthreadYield", yield_),
    POSIX_ENTRY("pthread_yield", yield_),
    POSIX_ENTRY("sched_yield", yield_),
    POSIX_ENTRY("pthread_rename_np", px_rename),
    POSIX_ENTRY("sched_get_priority_max", prio_max),
    POSIX_ENTRY("sched_get_priority_min", prio_min),

    POSIX_ENTRY("pthread_key_create", px_key_create),
    POSIX_ENTRY("pthread_key_delete", px_key_delete),
    POSIX_ENTRY("pthread_getspecific", getspecific),
    POSIX_ENTRY("pthread_setspecific", px_setspecific),
    HLE_FUNC_ENTRY(K, "scePthreadKeyCreate", sce_key_create),
    HLE_FUNC_ENTRY(K, "scePthreadKeyDelete", sce_key_delete),
    HLE_FUNC_ENTRY(K, "scePthreadGetspecific", getspecific),
    HLE_FUNC_ENTRY(K, "scePthreadSetspecific", sce_setspecific),
};

void hle_register_libkernel_thread(void) {
    if (!kobj_init()) {
        LOG_E(KERNEL, "cannot create the kernel object registry");
        return;
    }
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}
