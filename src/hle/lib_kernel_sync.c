/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libkernel synchronization: pthread/scePthread mutexes and condition
 * variables, SCE semaphores and thread identity.
 *
 * Guest handles (pthread_mutex_t, ScePthreadCond, SceKernelSema, ...) are
 * 8-byte slots in guest memory. We store a host object pointer there and keep
 * every live object in a registry so garbage handles are rejected instead of
 * dereferenced. Small handle values are static initializers
 * (PTHREAD_MUTEX_INITIALIZER = 0, adaptive = 1, ...): the object is created on
 * first use.
 *
 * The POSIX entry points return errno values; the sce* ones return
 * 0x80020000 | errno. Both share one implementation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/hle.h"
#include "hle/kernel_objects.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

/* FreeBSD errno values (the PS5 kernel is FreeBSD-based) */
#define G_EPERM 1
#define G_ENOMEM 12
#define G_EBUSY 16
#define G_EINVAL 22
#define G_EDEADLK 11
#define G_EAGAIN 35
#define G_ETIMEDOUT 60

#define SCE_ERR(e) ((int32_t)(0x80020000u | (uint32_t)(e)))

enum { MUTEX_ERRORCHECK = 1, MUTEX_RECURSIVE = 2, MUTEX_NORMAL = 3, MUTEX_ADAPTIVE = 4 };
/* Handles below this are static initializers: created on first use. Values in the first 64 KiB
 * are never objects; libc++ std::mutex there is left as the allocator found it (No Man's Sky's
 * PlayFab client locks one holding a stale 0x20 and aborts on the error a rejection gives). */
#define STATIC_INIT_MAX 0x10000u


typedef struct mutex_obj {
    obj_kind kind;
    int type;
    pal_mutex *lock; /* protects owner/count */
    pal_cond *freed;
    uint64_t owner;  /* 0 = unlocked */
    uint32_t count;  /* recursion depth */
    uint32_t waiters; /* threads blocked in lock */
    uint32_t handoff_gen; /* bumped each time an unlock hands the mutex to its waiters */
    char name[32];
} mutex_obj;

/* owner while an unlock hands the mutex to the threads already waiting */
#define MUTEX_HANDOFF UINT64_MAX

typedef struct mutexattr_obj {
    obj_kind kind;
    int type;
    int protocol;
} mutexattr_obj;

typedef struct cond_obj {
    obj_kind kind;
    pal_mutex *lock;
    pal_cond *cv;
    uint64_t seq; /* bumped by every signal/broadcast */
    int32_t clock; /* of pthread_cond_timedwait deadlines (pthread_condattr_setclock) */
    char name[32];
} cond_obj;

typedef struct condattr_obj {
    obj_kind kind;
    int32_t clock; /* 0 CLOCK_REALTIME, 4 CLOCK_MONOTONIC, ... */
} condattr_obj;

typedef struct sema_obj {
    obj_kind kind;
    pal_mutex *lock;
    pal_cond *cv;
    int32_t count, max;
    uint32_t waiters;
    char name[32];
} sema_obj;

static void copy_name(hle_call *c, guest_addr addr, char *out, size_t size) {
    out[0] = '\0';
    if (addr) hle_read_str(c, addr, out, size);
}

static uint64_t self_id(hle_call *c) {
    return c->thread ? (uint64_t)c->thread->id : 0x100000000ull + pal_thread_current_id();
}

/* ---- Time helpers ---- */

#define NO_TIMEOUT UINT64_MAX

/* Absolute timespec of `clock` in guest memory -> relative ns (0 if past). */
static bool abs_clock_to_ns(hle_call *c, guest_addr ts, int32_t clock, uint64_t *out) {
    int64_t *t = (int64_t *)hle_mem(c, ts, 16);
    int64_t sec, nsec;
    if (!t || t[1] < 0 || t[1] >= 1000000000 || !kernel_clock_now(clock, &sec, &nsec)) return false;
    int64_t diff = (t[0] - sec) * 1000000000 + (t[1] - nsec);
    *out = diff > 0 ? (uint64_t)diff : 0;
    return true;
}

/* Absolute CLOCK_REALTIME timespec in guest memory -> relative ns (0 if past). */
static bool abs_timespec_to_ns(hle_call *c, guest_addr ts, uint64_t *out) { return abs_clock_to_ns(c, ts, 0, out); }

/* ---- Mutexes ---- */

static mutex_obj *mutex_create(int type, const char *name) {
    mutex_obj *m = (mutex_obj *)kobj_new(OBJ_MUTEX, sizeof(mutex_obj));
    if (!m) return NULL;
    if (pal_mutex_create(&m->lock) != PS5C_OK || pal_cond_create(&m->freed) != PS5C_OK) {
        if (m->lock) pal_mutex_destroy(m->lock);
        kobj_delete(m);
        return NULL;
    }
    m->type = type;
    if (name) snprintf(m->name, sizeof(m->name), "%s", name);
    return m;
}

/* Resolves the guest handle at `h`, creating the mutex for static initializers. */
static int mutex_get(hle_call *c, guest_addr h, mutex_obj **out) {
    uint64_t *slot = (uint64_t *)hle_mem(c, h, 8);
    if (!slot) return G_EINVAL;
    uint64_t v = *slot;
    if (v < STATIC_INIT_MAX) {
        kobj_lock();
        v = *slot; /* another thread may have won */
        if (v < STATIC_INIT_MAX) {
            kobj_unlock();
            mutex_obj *m = mutex_create(v == 1 ? MUTEX_ADAPTIVE : MUTEX_ERRORCHECK, "static");
            if (!m) return G_ENOMEM;
            kobj_lock();
            if (*slot < STATIC_INIT_MAX) {
                *slot = (uint64_t)(uintptr_t)m;
                m = NULL;
            }
            kobj_unlock();
            if (m) {
                pal_cond_destroy(m->freed);
                pal_mutex_destroy(m->lock);
                kobj_delete(m);
            }
            v = *slot;
        } else {
            kobj_unlock();
        }
    }
    *out = (mutex_obj *)kobj_check(v, OBJ_MUTEX);
    if (!*out) {
        static uint32_t logged;
        if (logged++ < 16) LOG_W(KERNEL, "mutex 0x%llx: handle 0x%llx is not a mutex", (unsigned long long)h, (unsigned long long)v);
    }
    return *out ? 0 : G_EINVAL;
}

/* Caller holds m->lock: the mutex goes to the threads waiting for it, if any */
static void mutex_release(mutex_obj *m) {
    if (m->waiters) {
        m->owner = MUTEX_HANDOFF;
        m->handoff_gen++;
    } else {
        m->owner = 0;
    }
    pal_cond_broadcast(m->freed);
}

static int mutex_lock(hle_call *c, mutex_obj *m, uint64_t timeout_ns, bool try_only) {
    uint64_t me = self_id(c);
    pal_mutex_lock(m->lock);
    if (m->owner == me) {
        int err = 0;
        if (m->type == MUTEX_RECURSIVE) m->count++;
        else err = try_only ? G_EBUSY : G_EDEADLK;
        if (err == G_EDEADLK && m->type == MUTEX_NORMAL) {
            LOG_W(KERNEL, "self-deadlock on normal mutex '%s' (returning EDEADLK instead of hanging)", m->name);
        }
        pal_mutex_unlock(m->lock);
        return err;
    }
    /* A thread that arrives while others wait queues behind them instead of
     * taking a mutex that was just released: an unlock with waiters hands the
     * mutex to the threads that were already waiting (MUTEX_HANDOFF), so a
     * thread that unlocks and relocks in a loop cannot starve them (GTA V's
     * main thread lost every race to its render thread). */
    if (m->owner != 0 || m->waiters != 0) {
        if (try_only) {
            pal_mutex_unlock(m->lock);
            return G_EBUSY;
        }
        uint64_t deadline = timeout_ns == NO_TIMEOUT ? 0 : pal_time_ns() + timeout_ns;
        uint32_t gen = m->handoff_gen;
        m->waiters++;
        while (m->owner != 0 && !(m->owner == MUTEX_HANDOFF && m->handoff_gen != gen)) {
            if (timeout_ns == NO_TIMEOUT) {
                /* a long wait is logged once with the owner (debugging deadlocks) */
                if (pal_cond_wait_ns(m->freed, m->lock, 5000000000ull) == PS5C_ERR_TIMEOUT && m->owner != 0 &&
                    m->owner != MUTEX_HANDOFF) {
                    static uint32_t logged;
                    if (logged++ < 8)
                        LOG_W(KERNEL, "thread 0x%llx waits over 5 s for mutex '%s' (%p) held by 0x%llx", (unsigned long long)me,
                              m->name, (void *)m, (unsigned long long)m->owner);
                }
            } else {
                uint64_t now = pal_time_ns();
                if (now >= deadline || pal_cond_wait_ns(m->freed, m->lock, deadline - now) == PS5C_ERR_TIMEOUT) {
                    if (m->owner != 0 && !(m->owner == MUTEX_HANDOFF && m->handoff_gen != gen)) {
                        m->waiters--;
                        /* a hand-off nobody older is left to take: free for anyone */
                        if (m->owner == MUTEX_HANDOFF) {
                            m->owner = 0;
                            pal_cond_broadcast(m->freed);
                        }
                        pal_mutex_unlock(m->lock);
                        return G_ETIMEDOUT;
                    }
                }
            }
        }
        m->waiters--;
    }
    m->owner = me;
    m->count = 1;
    pal_mutex_unlock(m->lock);
    return 0;
}

static int mutex_unlock(hle_call *c, mutex_obj *m) {
    pal_mutex_lock(m->lock);
    if (m->owner != self_id(c)) {
        pal_mutex_unlock(m->lock);
        return G_EPERM;
    }
    if (--m->count == 0) mutex_release(m);
    pal_mutex_unlock(m->lock);
    return 0;
}

static int do_mutex_init(hle_call *c, guest_addr h, guest_addr attr_h, guest_addr name_addr) {
    uint64_t *slot = (uint64_t *)hle_mem(c, h, 8);
    if (!slot) return G_EINVAL;
    int type = MUTEX_ERRORCHECK;
    if (attr_h) {
        uint64_t *attr_slot = (uint64_t *)hle_mem(c, attr_h, 8);
        mutexattr_obj *a = attr_slot ? (mutexattr_obj *)kobj_check(*attr_slot, OBJ_MUTEXATTR) : NULL;
        if (!a) return G_EINVAL;
        type = a->type;
    }
    char name[32];
    copy_name(c, name_addr, name, sizeof(name));
    mutex_obj *m = mutex_create(type, name);
    if (!m) return G_ENOMEM;
    *slot = (uint64_t)(uintptr_t)m;
    return 0;
}

static int do_mutex_destroy(hle_call *c, guest_addr h) {
    uint64_t *slot = (uint64_t *)hle_mem(c, h, 8);
    if (!slot) return G_EINVAL;
    if (*slot < STATIC_INIT_MAX) return 0; /* never used */
    mutex_obj *m = (mutex_obj *)kobj_check(*slot, OBJ_MUTEX);
    if (!m) return G_EINVAL;
    if (m->owner) return G_EBUSY;
    pal_cond_destroy(m->freed);
    pal_mutex_destroy(m->lock);
    kobj_delete(m);
    *slot = 0;
    return 0;
}

static int do_mutex_lock(hle_call *c, guest_addr h, uint64_t timeout_ns, bool try_only) {
    mutex_obj *m;
    int err = mutex_get(c, h, &m);
    return err ? err : mutex_lock(c, m, timeout_ns, try_only);
}

static int do_mutex_unlock(hle_call *c, guest_addr h) {
    mutex_obj *m;
    int err = mutex_get(c, h, &m);
    return err ? err : mutex_unlock(c, m);
}

/* ---- Mutex attributes ---- */

static int do_mutexattr_init(hle_call *c, guest_addr h) {
    uint64_t *slot = (uint64_t *)hle_mem(c, h, 8);
    if (!slot) return G_EINVAL;
    mutexattr_obj *a = (mutexattr_obj *)kobj_new(OBJ_MUTEXATTR, sizeof(mutexattr_obj));
    if (!a) return G_ENOMEM;
    a->type = MUTEX_ERRORCHECK;
    *slot = (uint64_t)(uintptr_t)a;
    return 0;
}

static mutexattr_obj *mutexattr_get(hle_call *c, guest_addr h) {
    uint64_t *slot = (uint64_t *)hle_mem(c, h, 8);
    return slot ? (mutexattr_obj *)kobj_check(*slot, OBJ_MUTEXATTR) : NULL;
}

static int do_mutexattr_destroy(hle_call *c, guest_addr h) {
    mutexattr_obj *a = mutexattr_get(c, h);
    if (!a) return G_EINVAL;
    kobj_delete(a);
    *(uint64_t *)hle_mem(c, h, 8) = 0;
    return 0;
}

static int do_mutexattr_settype(hle_call *c, guest_addr h, int type) {
    mutexattr_obj *a = mutexattr_get(c, h);
    if (!a || type < MUTEX_ERRORCHECK || type > MUTEX_ADAPTIVE) return G_EINVAL;
    a->type = type;
    return 0;
}

static int do_mutexattr_setprotocol(hle_call *c, guest_addr h, int protocol) {
    mutexattr_obj *a = mutexattr_get(c, h);
    if (!a || protocol < 0 || protocol > 2) return G_EINVAL;
    a->protocol = protocol; /* priority inheritance/protection: no effect on the host */
    return 0;
}

/* ---- Condition variables ---- */

static cond_obj *cond_create(const char *name) {
    cond_obj *cv = (cond_obj *)kobj_new(OBJ_COND, sizeof(cond_obj));
    if (!cv) return NULL;
    if (pal_mutex_create(&cv->lock) != PS5C_OK || pal_cond_create(&cv->cv) != PS5C_OK) {
        if (cv->lock) pal_mutex_destroy(cv->lock);
        kobj_delete(cv);
        return NULL;
    }
    if (name) snprintf(cv->name, sizeof(cv->name), "%s", name);
    return cv;
}

static int cond_get(hle_call *c, guest_addr h, cond_obj **out) {
    uint64_t *slot = (uint64_t *)hle_mem(c, h, 8);
    if (!slot) return G_EINVAL;
    if (*slot < STATIC_INIT_MAX) {
        cond_obj *cv = cond_create("static");
        if (!cv) return G_ENOMEM;
        kobj_lock();
        if (*slot < STATIC_INIT_MAX) {
            *slot = (uint64_t)(uintptr_t)cv;
            cv = NULL;
        }
        kobj_unlock();
        if (cv) {
            pal_cond_destroy(cv->cv);
            pal_mutex_destroy(cv->lock);
            kobj_delete(cv);
        }
    }
    *out = (cond_obj *)kobj_check(*slot, OBJ_COND);
    return *out ? 0 : G_EINVAL;
}

static int do_cond_init(hle_call *c, guest_addr h, guest_addr attr_h, guest_addr name_addr) {
    uint64_t *slot = (uint64_t *)hle_mem(c, h, 8);
    if (!slot) return G_EINVAL;
    char name[32];
    copy_name(c, name_addr, name, sizeof(name));
    cond_obj *cv = cond_create(name);
    if (!cv) return G_ENOMEM;
    uint64_t *aslot = attr_h ? (uint64_t *)hle_mem(c, attr_h, 8) : NULL;
    const condattr_obj *a = aslot ? (const condattr_obj *)kobj_check(*aslot, OBJ_CONDATTR) : NULL;
    if (a) cv->clock = a->clock;
    *slot = (uint64_t)(uintptr_t)cv;
    return 0;
}

static int do_cond_destroy(hle_call *c, guest_addr h) {
    uint64_t *slot = (uint64_t *)hle_mem(c, h, 8);
    if (!slot) return G_EINVAL;
    if (*slot < STATIC_INIT_MAX) return 0;
    cond_obj *cv = (cond_obj *)kobj_check(*slot, OBJ_COND);
    if (!cv) return G_EINVAL;
    pal_cond_destroy(cv->cv);
    pal_mutex_destroy(cv->lock);
    kobj_delete(cv);
    *slot = 0;
    return 0;
}

static int do_cond_wait(hle_call *c, guest_addr ch, guest_addr mh, uint64_t timeout_ns) {
    cond_obj *cv;
    mutex_obj *m;
    int err = cond_get(c, ch, &cv);
    if (!err) err = mutex_get(c, mh, &m);
    if (err) return err;

    uint64_t me = self_id(c);
    pal_mutex_lock(cv->lock);
    /* Release the guest mutex completely while holding cv->lock: a signaler
     * needs cv->lock too, so no wakeup can slip in between. */
    pal_mutex_lock(m->lock);
    if (m->owner != me) {
        pal_mutex_unlock(m->lock);
        pal_mutex_unlock(cv->lock);
        return G_EPERM;
    }
    uint32_t depth = m->count;
    m->count = 0;
    mutex_release(m);
    pal_mutex_unlock(m->lock);

    if (!c->thread || c->rt->next_thread_id <= 2) {
        if (timeout_ns == NO_TIMEOUT) {
            LOG_W(KERNEL, "cond '%s' wait with no other guest thread: will block forever", cv->name);
            runtime_log_backtrace(c->rt);
        }
    }
    uint64_t seq = cv->seq;
    int result = 0;
    uint64_t deadline = timeout_ns == NO_TIMEOUT ? 0 : pal_time_ns() + timeout_ns;
    while (cv->seq == seq) {
        if (timeout_ns == NO_TIMEOUT) {
            pal_cond_wait(cv->cv, cv->lock);
        } else {
            uint64_t now = pal_time_ns();
            if (now >= deadline) {
                result = G_ETIMEDOUT;
                break;
            }
            pal_cond_wait_ns(cv->cv, cv->lock, deadline - now);
        }
    }
    pal_mutex_unlock(cv->lock);

    mutex_lock(c, m, NO_TIMEOUT, false);
    m->count = depth;
    return result;
}

static int do_cond_signal(hle_call *c, guest_addr h, bool all) {
    {
        static guest_addr watch = 1;
        if (watch == 1) {
            const char *e = getenv("PS5C_TRACE_COND");
            watch = e ? strtoull(e, NULL, 16) : 0;
        }
        if (watch && h == watch) {
            char where[128];
            snprintf(where, sizeof(where), "0x%llx", (unsigned long long)(c->thread ? c->thread->hle_ret : 0));
            LOG_I(KERNEL, "cond 0x%llx %s by T%u from %s", (unsigned long long)h, all ? "broadcast" : "signal",
                  c->thread ? c->thread->id : 0, where);
            runtime_log_backtrace(c->rt);
        }
    }
    cond_obj *cv;
    int err = cond_get(c, h, &cv);
    if (err) return err;
    pal_mutex_lock(cv->lock);
    cv->seq++;
    if (all) pal_cond_broadcast(cv->cv);
    else pal_cond_signal(cv->cv);
    pal_mutex_unlock(cv->lock);
    return 0;
}

static int do_condattr_init(hle_call *c, guest_addr h) {
    uint64_t *slot = (uint64_t *)hle_mem(c, h, 8);
    if (!slot) return G_EINVAL;
    condattr_obj *a = (condattr_obj *)kobj_new(OBJ_CONDATTR, sizeof(condattr_obj));
    if (!a) return G_ENOMEM;
    *slot = (uint64_t)(uintptr_t)a;
    return 0;
}

/* int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock) */
static int do_condattr_setclock(hle_call *c, guest_addr h, int32_t clock) {
    uint64_t *slot = (uint64_t *)hle_mem(c, h, 8);
    condattr_obj *a = slot ? (condattr_obj *)kobj_check(*slot, OBJ_CONDATTR) : NULL;
    int64_t sec, nsec;
    if (!a || !kernel_clock_now(clock, &sec, &nsec)) return G_EINVAL;
    a->clock = clock;
    return 0;
}

static int do_condattr_getclock(hle_call *c, guest_addr h, guest_addr out) {
    uint64_t *slot = (uint64_t *)hle_mem(c, h, 8);
    condattr_obj *a = slot ? (condattr_obj *)kobj_check(*slot, OBJ_CONDATTR) : NULL;
    int32_t *o = out ? (int32_t *)hle_mem(c, out, 4) : NULL;
    if (!a || !o) return G_EINVAL;
    *o = a->clock;
    return 0;
}

static int do_condattr_destroy(hle_call *c, guest_addr h) {
    uint64_t *slot = (uint64_t *)hle_mem(c, h, 8);
    condattr_obj *a = slot ? (condattr_obj *)kobj_check(*slot, OBJ_CONDATTR) : NULL;
    if (!a) return G_EINVAL;
    kobj_delete(a);
    *slot = 0;
    return 0;
}

/* ---- Wrappers: POSIX (errno) and SCE (0x8002xxxx) flavours ---- */

#define RET_POSIX(expr) hle_ret_s32(c, (expr))
#define RET_SCE(expr)                                                                                     \
    do {                                                                                                  \
        int e_ = (expr);                                                                                  \
        hle_ret_s32(c, e_ ? SCE_ERR(e_) : 0);                                                             \
    } while (0)

static uint64_t usec_arg(hle_call *c, uint32_t i) { return (uint64_t)hle_arg_u32(c, i) * 1000u; }

static int timed_arg(hle_call *c, uint32_t i, uint64_t *ns) {
    return abs_timespec_to_ns(c, hle_arg_ptr(c, i), ns) ? 0 : G_EINVAL;
}

/* pthread_mutex_* */
static void px_mutex_init(hle_call *c) { RET_POSIX(do_mutex_init(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1), 0)); }
static void px_mutex_destroy(hle_call *c) { RET_POSIX(do_mutex_destroy(c, hle_arg_ptr(c, 0))); }
static void px_mutex_lock(hle_call *c) { RET_POSIX(do_mutex_lock(c, hle_arg_ptr(c, 0), NO_TIMEOUT, false)); }
static void px_mutex_trylock(hle_call *c) { RET_POSIX(do_mutex_lock(c, hle_arg_ptr(c, 0), NO_TIMEOUT, true)); }
static void px_mutex_unlock(hle_call *c) { RET_POSIX(do_mutex_unlock(c, hle_arg_ptr(c, 0))); }
static void px_mutex_timedlock(hle_call *c) {
    uint64_t ns;
    int err = timed_arg(c, 1, &ns);
    RET_POSIX(err ? err : do_mutex_lock(c, hle_arg_ptr(c, 0), ns, false));
}
static void px_mutexattr_init(hle_call *c) { RET_POSIX(do_mutexattr_init(c, hle_arg_ptr(c, 0))); }
static void px_mutexattr_destroy(hle_call *c) { RET_POSIX(do_mutexattr_destroy(c, hle_arg_ptr(c, 0))); }
static void px_mutexattr_settype(hle_call *c) {
    RET_POSIX(do_mutexattr_settype(c, hle_arg_ptr(c, 0), hle_arg_s32(c, 1)));
}
static void px_mutexattr_setprotocol(hle_call *c) {
    RET_POSIX(do_mutexattr_setprotocol(c, hle_arg_ptr(c, 0), hle_arg_s32(c, 1)));
}

/* scePthreadMutex* */
static void sce_mutex_init(hle_call *c) {
    RET_SCE(do_mutex_init(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1), hle_arg_ptr(c, 2)));
}
static void sce_mutex_destroy(hle_call *c) { RET_SCE(do_mutex_destroy(c, hle_arg_ptr(c, 0))); }
static void sce_mutex_lock(hle_call *c) { RET_SCE(do_mutex_lock(c, hle_arg_ptr(c, 0), NO_TIMEOUT, false)); }
static void sce_mutex_trylock(hle_call *c) { RET_SCE(do_mutex_lock(c, hle_arg_ptr(c, 0), NO_TIMEOUT, true)); }
static void sce_mutex_timedlock(hle_call *c) {
    RET_SCE(do_mutex_lock(c, hle_arg_ptr(c, 0), usec_arg(c, 1), false));
}
static void sce_mutex_unlock(hle_call *c) { RET_SCE(do_mutex_unlock(c, hle_arg_ptr(c, 0))); }
static void sce_mutexattr_init(hle_call *c) { RET_SCE(do_mutexattr_init(c, hle_arg_ptr(c, 0))); }
static void sce_mutexattr_destroy(hle_call *c) { RET_SCE(do_mutexattr_destroy(c, hle_arg_ptr(c, 0))); }
static void sce_mutexattr_settype(hle_call *c) {
    RET_SCE(do_mutexattr_settype(c, hle_arg_ptr(c, 0), hle_arg_s32(c, 1)));
}
static void sce_mutexattr_setprotocol(hle_call *c) {
    RET_SCE(do_mutexattr_setprotocol(c, hle_arg_ptr(c, 0), hle_arg_s32(c, 1)));
}

/* pthread_cond_* */
static void px_cond_init(hle_call *c) { RET_POSIX(do_cond_init(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1), 0)); }
static void px_cond_destroy(hle_call *c) { RET_POSIX(do_cond_destroy(c, hle_arg_ptr(c, 0))); }
static void px_cond_wait(hle_call *c) {
    RET_POSIX(do_cond_wait(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1), NO_TIMEOUT));
}
/* the deadline is on the clock the condition variable was created with */
static void px_cond_timedwait(hle_call *c) {
    cond_obj *cv;
    uint64_t ns = 0;
    int err = cond_get(c, hle_arg_ptr(c, 0), &cv);
    if (!err && !abs_clock_to_ns(c, hle_arg_ptr(c, 2), cv->clock, &ns)) err = G_EINVAL;
    RET_POSIX(err ? err : do_cond_wait(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1), ns));
}
static void px_cond_signal(hle_call *c) { RET_POSIX(do_cond_signal(c, hle_arg_ptr(c, 0), false)); }
static void px_cond_broadcast(hle_call *c) { RET_POSIX(do_cond_signal(c, hle_arg_ptr(c, 0), true)); }
static void px_condattr_init(hle_call *c) { RET_POSIX(do_condattr_init(c, hle_arg_ptr(c, 0))); }
static void px_condattr_destroy(hle_call *c) { RET_POSIX(do_condattr_destroy(c, hle_arg_ptr(c, 0))); }
static void px_condattr_setclock(hle_call *c) {
    RET_POSIX(do_condattr_setclock(c, hle_arg_ptr(c, 0), hle_arg_s32(c, 1)));
}
static void px_condattr_getclock(hle_call *c) {
    RET_POSIX(do_condattr_getclock(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1)));
}

/* scePthreadCond* */
static void sce_cond_init(hle_call *c) {
    RET_SCE(do_cond_init(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1), hle_arg_ptr(c, 2)));
}
static void sce_cond_destroy(hle_call *c) { RET_SCE(do_cond_destroy(c, hle_arg_ptr(c, 0))); }
static void sce_cond_wait(hle_call *c) {
    RET_SCE(do_cond_wait(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1), NO_TIMEOUT));
}
static void sce_cond_timedwait(hle_call *c) {
    RET_SCE(do_cond_wait(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1), usec_arg(c, 2)));
}
static void sce_cond_signal(hle_call *c) { RET_SCE(do_cond_signal(c, hle_arg_ptr(c, 0), false)); }
static void sce_cond_broadcast(hle_call *c) { RET_SCE(do_cond_signal(c, hle_arg_ptr(c, 0), true)); }
static void sce_condattr_init(hle_call *c) { RET_SCE(do_condattr_init(c, hle_arg_ptr(c, 0))); }
static void sce_condattr_destroy(hle_call *c) { RET_SCE(do_condattr_destroy(c, hle_arg_ptr(c, 0))); }

/* ---- SCE semaphores ---- */

#define SCE_KERNEL_ERROR_ETIMEDOUT SCE_ERR(G_ETIMEDOUT)
#define SCE_KERNEL_ERROR_EBUSY SCE_ERR(G_EBUSY)

static sema_obj *sema_get(hle_call *c, uint32_t i) {
    return (sema_obj *)kobj_check(hle_arg(c, i), OBJ_SEMA);
}

/* int sceKernelCreateSema(SceKernelSema *sem, const char *name, uint32_t attr,
 *                         int init, int max, const void *opt) */
static void create_sema(hle_call *c) {
    uint64_t *slot = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 0), 8);
    int32_t init = hle_arg_s32(c, 3), max = hle_arg_s32(c, 4);
    if (!slot || init < 0 || max <= 0 || init > max) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    sema_obj *s = (sema_obj *)kobj_new(OBJ_SEMA, sizeof(sema_obj));
    if (!s || pal_mutex_create(&s->lock) != PS5C_OK || pal_cond_create(&s->cv) != PS5C_OK) {
        if (s) {
            if (s->lock) pal_mutex_destroy(s->lock);
            kobj_delete(s);
        }
        hle_ret_s32(c, SCE_ERR(G_ENOMEM));
        return;
    }
    s->count = init;
    s->max = max;
    copy_name(c, hle_arg_ptr(c, 1), s->name, sizeof(s->name));
    *slot = (uint64_t)(uintptr_t)s;
    hle_ret_s32(c, 0);
}

static void delete_sema(hle_call *c) {
    sema_obj *s = sema_get(c, 0);
    if (!s) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_ESRCH);
        return;
    }
    if (s->waiters) LOG_W(KERNEL, "semaphore '%s' deleted with %u waiters", s->name, s->waiters);
    pal_cond_destroy(s->cv);
    pal_mutex_destroy(s->lock);
    kobj_delete(s);
    hle_ret_s32(c, 0);
}

/* int sceKernelWaitSema(SceKernelSema sem, int need, uint32_t *timeout_us) */
static void wait_sema(hle_call *c) {
    sema_obj *s = sema_get(c, 0);
    int32_t need = hle_arg_s32(c, 1);
    uint32_t *timeout = (uint32_t *)hle_mem(c, hle_arg_ptr(c, 2), 4);
    if (!s) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_ESRCH);
        return;
    }
    if (need <= 0 || need > s->max) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    uint64_t start = pal_time_ns();
    uint64_t deadline = timeout ? start + (uint64_t)*timeout * 1000u : 0;
    int32_t ret = 0;
    pal_mutex_lock(s->lock);
    s->waiters++;
    while (s->count < need) {
        if (!timeout) {
            pal_cond_wait(s->cv, s->lock);
            continue;
        }
        uint64_t now = pal_time_ns();
        if (now >= deadline) {
            ret = SCE_KERNEL_ERROR_ETIMEDOUT;
            break;
        }
        pal_cond_wait_ns(s->cv, s->lock, deadline - now);
    }
    if (ret == 0) s->count -= need;
    s->waiters--;
    pal_mutex_unlock(s->lock);
    if (timeout) {
        uint64_t elapsed_us = (pal_time_ns() - start) / 1000u;
        *timeout = elapsed_us >= *timeout ? 0 : *timeout - (uint32_t)elapsed_us;
    }
    hle_ret_s32(c, ret);
}

static void poll_sema(hle_call *c) {
    sema_obj *s = sema_get(c, 0);
    int32_t need = hle_arg_s32(c, 1);
    if (!s) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_ESRCH);
        return;
    }
    if (need <= 0 || need > s->max) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    pal_mutex_lock(s->lock);
    bool ok = s->count >= need;
    if (ok) s->count -= need;
    pal_mutex_unlock(s->lock);
    hle_ret_s32(c, ok ? 0 : SCE_KERNEL_ERROR_EBUSY);
}

static void signal_sema(hle_call *c) {
    sema_obj *s = sema_get(c, 0);
    int32_t n = hle_arg_s32(c, 1);
    if (!s) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_ESRCH);
        return;
    }
    pal_mutex_lock(s->lock);
    bool ok = n > 0 && s->count <= s->max - n;
    if (ok) {
        s->count += n;
        pal_cond_broadcast(s->cv);
    }
    pal_mutex_unlock(s->lock);
    hle_ret_s32(c, ok ? 0 : SCE_KERNEL_ERROR_EINVAL);
}

/* ---- POSIX semaphores (sem_t holds the semaphore object) ---- */

static void posix_fail(hle_call *c, int e) {
    if (c->thread && c->thread->errno_addr) gmem_write(c->mem, c->thread->errno_addr, &e, 4);
    hle_ret_s32(c, -1);
}

static sema_obj *psem_get(hle_call *c) {
    const uint64_t *slot = (const uint64_t *)hle_mem(c, hle_arg_ptr(c, 0), 8);
    return slot ? (sema_obj *)kobj_check(*slot, OBJ_SEMA) : NULL;
}

/* int sem_init(sem_t *sem, int pshared, unsigned value) */
static void psem_init(hle_call *c) {
    uint64_t *slot = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 0), 8);
    uint32_t value = hle_arg_u32(c, 2);
    if (!slot || value > 0x7FFFFFFFu) {
        posix_fail(c, G_EINVAL);
        return;
    }
    sema_obj *s = (sema_obj *)kobj_new(OBJ_SEMA, sizeof(sema_obj));
    if (!s || pal_mutex_create(&s->lock) != PS5C_OK || pal_cond_create(&s->cv) != PS5C_OK) {
        if (s) {
            if (s->lock) pal_mutex_destroy(s->lock);
            kobj_delete(s);
        }
        posix_fail(c, G_ENOMEM);
        return;
    }
    s->count = (int32_t)value;
    s->max = 0x7FFFFFFF;
    snprintf(s->name, sizeof(s->name), "sem_t");
    *slot = (uint64_t)(uintptr_t)s;
    hle_ret_s32(c, 0);
}

static void psem_destroy(hle_call *c) {
    sema_obj *s = psem_get(c);
    if (!s) {
        posix_fail(c, G_EINVAL);
        return;
    }
    pal_cond_destroy(s->cv);
    pal_mutex_destroy(s->lock);
    kobj_delete(s);
    hle_ret_s32(c, 0);
}

/* wait for one unit: ns = NO_TIMEOUT blocks, 0 polls */
static void psem_take(hle_call *c, uint64_t ns) {
    sema_obj *s = psem_get(c);
    if (!s) {
        posix_fail(c, G_EINVAL);
        return;
    }
    uint64_t deadline = ns == NO_TIMEOUT ? 0 : pal_time_ns() + ns;
    int err = 0;
    pal_mutex_lock(s->lock);
    s->waiters++;
    while (s->count < 1) {
        if (ns == NO_TIMEOUT) {
            pal_cond_wait(s->cv, s->lock);
            continue;
        }
        uint64_t now = pal_time_ns();
        if (now >= deadline) {
            err = ns ? G_ETIMEDOUT : G_EAGAIN;
            break;
        }
        pal_cond_wait_ns(s->cv, s->lock, deadline - now);
    }
    if (!err) s->count--;
    s->waiters--;
    pal_mutex_unlock(s->lock);
    if (err) posix_fail(c, err);
    else hle_ret_s32(c, 0);
}

static void psem_wait(hle_call *c) { psem_take(c, NO_TIMEOUT); }
static void psem_trywait(hle_call *c) { psem_take(c, 0); }
static void psem_timedwait(hle_call *c) {
    uint64_t ns;
    if (!abs_timespec_to_ns(c, hle_arg_ptr(c, 1), &ns)) {
        posix_fail(c, G_EINVAL);
        return;
    }
    psem_take(c, ns ? ns : 1);
}

static void psem_post(hle_call *c) {
    sema_obj *s = psem_get(c);
    if (!s) {
        posix_fail(c, G_EINVAL);
        return;
    }
    pal_mutex_lock(s->lock);
    s->count++;
    pal_cond_signal(s->cv);
    pal_mutex_unlock(s->lock);
    hle_ret_s32(c, 0);
}

/* int sem_getvalue(sem_t *sem, int *value) */
static void psem_getvalue(hle_call *c) {
    sema_obj *s = psem_get(c);
    int32_t *out = (int32_t *)hle_mem(c, hle_arg_ptr(c, 1), 4);
    if (!s || !out) {
        posix_fail(c, G_EINVAL);
        return;
    }
    pal_mutex_lock(s->lock);
    *out = s->count;
    pal_mutex_unlock(s->lock);
    hle_ret_s32(c, 0);
}

/* ---- Thread identity ---- */

/* ScePthread / pthread_t: opaque to the guest; we hand out the guest_thread. */
static void pthread_self_(hle_call *c) { hle_ret_u64(c, (uint64_t)(uintptr_t)guest_thread_identity(c->thread)); }
static void pthread_equal_(hle_call *c) { hle_ret_s32(c, hle_arg(c, 0) == hle_arg(c, 1)); }

/* ---- Read/write locks ---- */

typedef struct rwlock_obj {
    obj_kind kind;
    pal_mutex *lock;
    pal_cond *changed;
    uint32_t readers;
    uint64_t writer; /* thread id, 0 = none */
    uint32_t waiting_writers;
    char name[32];
} rwlock_obj;

static rwlock_obj *rwlock_create(const char *name) {
    rwlock_obj *rw = (rwlock_obj *)kobj_new(OBJ_RWLOCK, sizeof(rwlock_obj));
    if (!rw) return NULL;
    if (pal_mutex_create(&rw->lock) != PS5C_OK || pal_cond_create(&rw->changed) != PS5C_OK) {
        if (rw->lock) pal_mutex_destroy(rw->lock);
        kobj_delete(rw);
        return NULL;
    }
    if (name) snprintf(rw->name, sizeof(rw->name), "%s", name);
    return rw;
}

static void rwlock_free(rwlock_obj *rw) {
    pal_cond_destroy(rw->changed);
    pal_mutex_destroy(rw->lock);
    kobj_delete(rw);
}

static int rwlock_get(hle_call *c, guest_addr h, rwlock_obj **out) {
    uint64_t *slot = (uint64_t *)hle_mem(c, h, 8);
    if (!slot) return G_EINVAL;
    if (*slot < STATIC_INIT_MAX) {
        rwlock_obj *rw = rwlock_create("static");
        if (!rw) return G_ENOMEM;
        kobj_lock();
        if (*slot < STATIC_INIT_MAX) {
            *slot = (uint64_t)(uintptr_t)rw;
            rw = NULL;
        }
        kobj_unlock();
        if (rw) rwlock_free(rw);
    }
    *out = (rwlock_obj *)kobj_check(*slot, OBJ_RWLOCK);
    return *out ? 0 : G_EINVAL;
}

static int do_rwlock_init(hle_call *c, guest_addr h, guest_addr name_addr) {
    uint64_t *slot = (uint64_t *)hle_mem(c, h, 8);
    if (!slot) return G_EINVAL;
    char name[32];
    copy_name(c, name_addr, name, sizeof(name));
    rwlock_obj *rw = rwlock_create(name);
    if (!rw) return G_ENOMEM;
    *slot = (uint64_t)(uintptr_t)rw;
    return 0;
}

static int do_rwlock_destroy(hle_call *c, guest_addr h) {
    uint64_t *slot = (uint64_t *)hle_mem(c, h, 8);
    if (!slot) return G_EINVAL;
    if (*slot < STATIC_INIT_MAX) return 0;
    rwlock_obj *rw = (rwlock_obj *)kobj_check(*slot, OBJ_RWLOCK);
    if (!rw) return G_EINVAL;
    if (rw->readers || rw->writer) return G_EBUSY;
    rwlock_free(rw);
    *slot = 0;
    return 0;
}

/* wait_ns: 0 only tries, UINT64_MAX waits for good, else ETIMEDOUT after that long */
static int do_rwlock_lock(hle_call *c, guest_addr h, bool write, uint64_t wait_ns) {
    bool try_only = wait_ns == 0;
    uint64_t deadline = wait_ns == UINT64_MAX ? 0 : pal_time_ns() + wait_ns;
    rwlock_obj *rw;
    int err = rwlock_get(c, h, &rw);
    if (err) return err;
    uint64_t me = self_id(c);
    pal_mutex_lock(rw->lock);
    if (rw->writer == me) {
        pal_mutex_unlock(rw->lock);
        return G_EDEADLK;
    }
    if (write) {
        rw->waiting_writers++;
        while (rw->writer || rw->readers) {
            if (try_only) {
                rw->waiting_writers--;
                pal_mutex_unlock(rw->lock);
                return G_EBUSY;
            }
            if (deadline) {
                uint64_t now = pal_time_ns();
                if (now >= deadline || pal_cond_wait_ns(rw->changed, rw->lock, deadline - now) == PS5C_ERR_TIMEOUT) {
                    if (!(rw->writer || rw->readers)) break;
                    rw->waiting_writers--;
                    pal_mutex_unlock(rw->lock);
                    return G_ETIMEDOUT;
                }
                continue;
            }
            pal_cond_wait(rw->changed, rw->lock);
        }
        rw->waiting_writers--;
        rw->writer = me;
    } else {
        /* Writers go first so a stream of readers cannot starve them. */
        while (rw->writer || rw->waiting_writers) {
            if (try_only) {
                pal_mutex_unlock(rw->lock);
                return G_EBUSY;
            }
            if (deadline) {
                uint64_t now = pal_time_ns();
                if (now >= deadline || pal_cond_wait_ns(rw->changed, rw->lock, deadline - now) == PS5C_ERR_TIMEOUT) {
                    if (!(rw->writer || rw->waiting_writers)) break;
                    pal_mutex_unlock(rw->lock);
                    return G_ETIMEDOUT;
                }
                continue;
            }
            pal_cond_wait(rw->changed, rw->lock);
        }
        rw->readers++;
    }
    pal_mutex_unlock(rw->lock);
    return 0;
}

static int do_rwlock_unlock(hle_call *c, guest_addr h) {
    rwlock_obj *rw;
    int err = rwlock_get(c, h, &rw);
    if (err) return err;
    pal_mutex_lock(rw->lock);
    if (rw->writer == self_id(c)) {
        rw->writer = 0;
    } else if (rw->readers) {
        rw->readers--;
    } else {
        err = G_EPERM;
    }
    pal_cond_broadcast(rw->changed);
    pal_mutex_unlock(rw->lock);
    return err;
}

static void px_rwlock_init(hle_call *c) { RET_POSIX(do_rwlock_init(c, hle_arg_ptr(c, 0), 0)); }
static void px_rwlock_destroy(hle_call *c) { RET_POSIX(do_rwlock_destroy(c, hle_arg_ptr(c, 0))); }
static void px_rwlock_rdlock(hle_call *c) { RET_POSIX(do_rwlock_lock(c, hle_arg_ptr(c, 0), false, UINT64_MAX)); }
static void px_rwlock_wrlock(hle_call *c) { RET_POSIX(do_rwlock_lock(c, hle_arg_ptr(c, 0), true, UINT64_MAX)); }
static void px_rwlock_tryrdlock(hle_call *c) { RET_POSIX(do_rwlock_lock(c, hle_arg_ptr(c, 0), false, 0)); }
static void px_rwlock_trywrlock(hle_call *c) { RET_POSIX(do_rwlock_lock(c, hle_arg_ptr(c, 0), true, 0)); }
static void px_rwlock_unlock(hle_call *c) { RET_POSIX(do_rwlock_unlock(c, hle_arg_ptr(c, 0))); }
static void sce_rwlock_init(hle_call *c) { RET_SCE(do_rwlock_init(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 2))); }
static void sce_rwlock_destroy(hle_call *c) { RET_SCE(do_rwlock_destroy(c, hle_arg_ptr(c, 0))); }
static void sce_rwlock_rdlock(hle_call *c) { RET_SCE(do_rwlock_lock(c, hle_arg_ptr(c, 0), false, UINT64_MAX)); }
static void sce_rwlock_wrlock(hle_call *c) { RET_SCE(do_rwlock_lock(c, hle_arg_ptr(c, 0), true, UINT64_MAX)); }
static void sce_rwlock_tryrdlock(hle_call *c) { RET_SCE(do_rwlock_lock(c, hle_arg_ptr(c, 0), false, 0)); }
static void sce_rwlock_trywrlock(hle_call *c) { RET_SCE(do_rwlock_lock(c, hle_arg_ptr(c, 0), true, 0)); }
/* int scePthreadRwlockTimedrdlock/Timedwrlock(rwlock, SceKernelUseconds usec) */
static void sce_rwlock_timedrdlock(hle_call *c) {
    RET_SCE(do_rwlock_lock(c, hle_arg_ptr(c, 0), false, (uint64_t)hle_arg_u32(c, 1) * 1000 + 1));
}
static void sce_rwlock_timedwrlock(hle_call *c) {
    RET_SCE(do_rwlock_lock(c, hle_arg_ptr(c, 0), true, (uint64_t)hle_arg_u32(c, 1) * 1000 + 1));
}
static void sce_rwlock_unlock(hle_call *c) { RET_SCE(do_rwlock_unlock(c, hle_arg_ptr(c, 0))); }
/* rwlock attributes carry nothing we use */
static void rwlockattr_noop(hle_call *c) { hle_ret_s32(c, 0); }
/* int scePthreadRwlockattrGettype(attr, int *type) / pthread_rwlockattr_getpshared(attr, int *v):
 * the default (1: normal rwlock / 0: process-private) */
static void rwlockattr_gettype(hle_call *c) {
    int32_t *out = (int32_t *)hle_mem(c, hle_arg_ptr(c, 1), 4);
    if (out) *out = 1;
    hle_ret_s32(c, out ? 0 : SCE_KERNEL_ERROR_EINVAL);
}
static void rwlockattr_getpshared(hle_call *c) {
    int32_t *out = (int32_t *)hle_mem(c, hle_arg_ptr(c, 1), 4);
    if (out) *out = 0;
    hle_ret_s32(c, out ? 0 : 22 /* EINVAL */);
}

/* ---- Event flags ---- */

#define EVF_WAITMODE_AND 0x01
#define EVF_WAITMODE_OR 0x02
#define EVF_WAITMODE_CLEAR_ALL 0x10
#define EVF_WAITMODE_CLEAR_PAT 0x20
#define SCE_KERNEL_ERROR_ECANCELED ((int32_t)0x80020055)

typedef struct eventflag_obj {
    obj_kind kind;
    pal_mutex *lock;
    pal_cond *changed;
    uint64_t pattern;
    uint32_t waiters;
    uint64_t cancel_gen; /* bumped by cancel: waiters return ECANCELED */
    char name[32];
} eventflag_obj;

static eventflag_obj *evf_get(hle_call *c, uint32_t i) {
    return (eventflag_obj *)kobj_check(hle_arg(c, i), OBJ_EVENTFLAG);
}

/* int sceKernelCreateEventFlag(SceKernelEventFlag *ef, const char *name, uint32_t attr, uint64_t init, const void *opt) */
static void create_event_flag(hle_call *c) {
    uint64_t *out = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 0), 8);
    if (!out) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    eventflag_obj *f = (eventflag_obj *)kobj_new(OBJ_EVENTFLAG, sizeof(eventflag_obj));
    if (!f || pal_mutex_create(&f->lock) != PS5C_OK || pal_cond_create(&f->changed) != PS5C_OK) {
        if (f) {
            if (f->lock) pal_mutex_destroy(f->lock);
            kobj_delete(f);
        }
        hle_ret_s32(c, SCE_ERR(G_ENOMEM));
        return;
    }
    f->pattern = hle_arg(c, 3);
    copy_name(c, hle_arg_ptr(c, 1), f->name, sizeof(f->name));
    *out = (uint64_t)(uintptr_t)f;
    hle_ret_s32(c, 0);
}

static void delete_event_flag(hle_call *c) {
    eventflag_obj *f = evf_get(c, 0);
    if (!f) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_ESRCH);
        return;
    }
    /* Waiters may still hold it: unregister and wake them, keep the memory. */
    pal_mutex_lock(f->lock);
    f->cancel_gen++;
    pal_cond_broadcast(f->changed);
    pal_mutex_unlock(f->lock);
    kobj_forget(f);
    hle_ret_s32(c, 0);
}

static void set_event_flag(hle_call *c) {
    eventflag_obj *f = evf_get(c, 0);
    if (!f) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_ESRCH);
        return;
    }
    pal_mutex_lock(f->lock);
    f->pattern |= hle_arg(c, 1);
    pal_cond_broadcast(f->changed);
    pal_mutex_unlock(f->lock);
    hle_ret_s32(c, 0);
}

static void clear_event_flag(hle_call *c) {
    eventflag_obj *f = evf_get(c, 0);
    if (!f) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_ESRCH);
        return;
    }
    pal_mutex_lock(f->lock);
    f->pattern &= hle_arg(c, 1);
    pal_mutex_unlock(f->lock);
    hle_ret_s32(c, 0);
}

static bool evf_satisfied(uint64_t pattern, uint64_t want, uint32_t mode) {
    return (mode & EVF_WAITMODE_AND) ? (pattern & want) == want : (pattern & want) != 0;
}

static void evf_consume(eventflag_obj *f, uint64_t want, uint32_t mode) {
    if (mode & EVF_WAITMODE_CLEAR_ALL) f->pattern = 0;
    else if (mode & EVF_WAITMODE_CLEAR_PAT) f->pattern &= ~want;
}

/* int sceKernelWaitEventFlag(ef, uint64_t pattern, uint32_t mode, uint64_t *result, uint32_t *timeout_us) */
static void wait_event_flag(hle_call *c) {
    eventflag_obj *f = evf_get(c, 0);
    uint64_t want = hle_arg(c, 1);
    uint32_t mode = hle_arg_u32(c, 2);
    uint64_t *result = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 3), 8);
    uint32_t *timeout = (uint32_t *)hle_mem(c, hle_arg_ptr(c, 4), 4);
    if (!f) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_ESRCH);
        return;
    }
    if (!want || !(mode & (EVF_WAITMODE_AND | EVF_WAITMODE_OR))) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    uint64_t start = pal_time_ns();
    uint64_t deadline = timeout ? start + (uint64_t)*timeout * 1000u : 0;
    int32_t ret = 0;
    pal_mutex_lock(f->lock);
    uint64_t gen = f->cancel_gen;
    f->waiters++;
    while (!evf_satisfied(f->pattern, want, mode)) {
        if (f->cancel_gen != gen) {
            ret = SCE_KERNEL_ERROR_ECANCELED;
            break;
        }
        if (!timeout) {
            pal_cond_wait(f->changed, f->lock);
            continue;
        }
        uint64_t now = pal_time_ns();
        if (now >= deadline) {
            ret = SCE_ERR(G_ETIMEDOUT);
            break;
        }
        pal_cond_wait_ns(f->changed, f->lock, deadline - now);
    }
    if (result) *result = f->pattern;
    if (ret == 0) evf_consume(f, want, mode);
    f->waiters--;
    pal_mutex_unlock(f->lock);
    if (timeout) {
        uint64_t elapsed_us = (pal_time_ns() - start) / 1000u;
        *timeout = elapsed_us >= *timeout ? 0 : *timeout - (uint32_t)elapsed_us;
    }
    hle_ret_s32(c, ret);
}

/* int sceKernelPollEventFlag(ef, uint64_t pattern, uint32_t mode, uint64_t *result) */
static void poll_event_flag(hle_call *c) {
    eventflag_obj *f = evf_get(c, 0);
    uint64_t want = hle_arg(c, 1);
    uint32_t mode = hle_arg_u32(c, 2);
    uint64_t *result = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 3), 8);
    if (!f) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_ESRCH);
        return;
    }
    pal_mutex_lock(f->lock);
    bool ok = evf_satisfied(f->pattern, want, mode);
    if (result) *result = f->pattern;
    if (ok) evf_consume(f, want, mode);
    pal_mutex_unlock(f->lock);
    hle_ret_s32(c, ok ? 0 : SCE_ERR(G_EBUSY));
}

/* int sceKernelCancelEventFlag(ef, uint64_t set_pattern, int *num_wait_threads) */
static void cancel_event_flag(hle_call *c) {
    eventflag_obj *f = evf_get(c, 0);
    int32_t *num = (int32_t *)hle_mem(c, hle_arg_ptr(c, 2), 4);
    if (!f) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_ESRCH);
        return;
    }
    pal_mutex_lock(f->lock);
    if (num) *num = (int32_t)f->waiters;
    f->pattern = hle_arg(c, 1);
    f->cancel_gen++;
    pal_cond_broadcast(f->changed);
    pal_mutex_unlock(f->lock);
    hle_ret_s32(c, 0);
}

/* ---- Registration ---- */

#define K "libkernel"
#define P "libScePosix"
/* POSIX names are exported by both libkernel and libScePosix. */
#define POSIX_ENTRY(name, fn) HLE_FUNC_ENTRY(K, name, fn), HLE_FUNC_ENTRY(P, name, fn)

/* ---- libkernel_sync_on_address (PS5): futex-like waits on a memory word ----
 * int wait(volatile uint32_t/uint64_t *addr, value expected, const uint32_t *timeout_us)
 * int wake(volatile void *addr, int count)
 * Unity's job workers idle in `while (*p <= 0) wait(p, ...)`. */

#define SYNC_ADDR_ERR_EINVAL ((int32_t)0x80020016)

static void sync_wait(hle_call *c, uint32_t size) {
    guest_addr addr = hle_arg_ptr(c, 0);
    volatile void *p = (addr & (size - 1)) ? NULL : hle_mem(c, addr, size);
    if (!p) {
        hle_ret_s32(c, SYNC_ADDR_ERR_EINVAL);
        return;
    }
    uint64_t expected = size == 8 ? hle_arg(c, 1) : hle_arg_u32(c, 1);
    const uint32_t *timeout_us = (const uint32_t *)hle_mem(c, hle_arg_ptr(c, 2), 4);
    uint64_t timeout = timeout_us ? (uint64_t)*timeout_us * 1000u : UINT64_MAX;
    hle_ret_s32(c, pal_wait_on_address(p, expected, size, timeout) ? 0 : SCE_KERNEL_ERROR_ETIMEDOUT);
}

static void sync_wait32(hle_call *c) { sync_wait(c, 4); }
static void sync_wait64(hle_call *c) { sync_wait(c, 8); }

static void sync_wake(hle_call *c) {
    guest_addr addr = hle_arg_ptr(c, 0);
    volatile void *p = hle_mem(c, addr, 4);
    int32_t count = hle_arg_s32(c, 1);
    if (!p || (addr & 3)) {
        hle_ret_s32(c, SYNC_ADDR_ERR_EINVAL);
        return;
    }
    if (count != 0) pal_wake_on_address(p, count == 0x7FFFFFFF ? -1 : count);
    hle_ret_s32(c, 0);
}

/* FreeBSD int _umtx_op(void *obj, int op, u_long val, void *uaddr, void *uaddr2): only the
 * address wait/wake ops (same semantics as the sync-on-address calls above). The timeout is
 * uaddr2: a relative timespec when uaddr is 0, else a struct _umtx_time of uaddr bytes
 * {timespec, u32 flags (1 = absolute), u32 clock}. An absolute deadline is waited in 10 ms
 * slices that end as a spurious wake (callers re-check the value and the clock). Other ops
 * are logged once and fail with EINVAL. */
enum { UMTX_WAIT = 2, UMTX_WAKE = 3, UMTX_WAIT_UINT = 11, UMTX_WAIT_UINT_PRIVATE = 15,
       UMTX_WAKE_PRIVATE = 16 };
static void umtx_op(hle_call *c) {
    guest_addr addr = hle_arg_ptr(c, 0);
    int32_t op = hle_arg_s32(c, 1);
    if (op == UMTX_WAKE || op == UMTX_WAKE_PRIVATE) {
        volatile void *p = (addr & 3) ? NULL : hle_mem(c, addr, 4);
        if (!p) { posix_fail(c, G_EINVAL); return; }
        int64_t n = (int64_t)hle_arg(c, 2);
        if (n > 0) pal_wake_on_address(p, n >= 0x7FFFFFFF ? -1 : (int32_t)n);
        hle_ret_s32(c, 0);
        return;
    }
    if (op == UMTX_WAIT || op == UMTX_WAIT_UINT || op == UMTX_WAIT_UINT_PRIVATE) {
        uint32_t size = op == UMTX_WAIT ? 8 : 4;
        volatile void *p = (addr & (size - 1)) ? NULL : hle_mem(c, addr, size);
        if (!p) { posix_fail(c, G_EINVAL); return; }
        uint64_t expected = size == 8 ? hle_arg(c, 2) : hle_arg_u32(c, 2);
        uint64_t timeout = UINT64_MAX;
        bool absolute = false;
        guest_addr tp = hle_arg_ptr(c, 4);
        const int64_t *ts = (const int64_t *)hle_mem(c, tp, 16);
        if (ts) {
            if (hle_arg(c, 3) >= 24) {
                const uint32_t *flags = (const uint32_t *)hle_mem(c, tp + 16, 4);
                absolute = flags && (*flags & 1);
            }
            if (absolute) timeout = 10000000ull;
            else if (ts[0] < 0 || ts[1] < 0) timeout = 0;
            else timeout = (uint64_t)ts[0] * 1000000000ull + (uint64_t)ts[1];
        }
        if (!pal_wait_on_address(p, expected, size, timeout) && timeout != UINT64_MAX && !absolute) {
            posix_fail(c, G_ETIMEDOUT);
            return;
        }
        hle_ret_s32(c, 0);
        return;
    }
    static int logged;
    if (logged++ == 0) LOG_W(KERNEL, "_umtx_op: op %d not implemented", op);
    posix_fail(c, G_EINVAL);
}

#define SYNC_LIB "libkernel_sync_on_address"
static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(SYNC_LIB, "#Hc4CaR6JBL0", sync_wait32), /* sceKernelSyncOnAddressWait */
    HLE_FUNC_ENTRY(SYNC_LIB, "#B2n8aDorSH4", sync_wait32), /* sceKernelSyncOnAddressWait32 */
    HLE_FUNC_ENTRY(SYNC_LIB, "#PZQhiiLXRFs", sync_wait64), /* sceKernelSyncOnAddressWait64 */
    HLE_FUNC_ENTRY(SYNC_LIB, "#q2y-wDIVWZA", sync_wake),   /* sceKernelSyncOnAddressWake */
    /* libc imports them from libkernel */
    HLE_FUNC_ENTRY("libkernel", "#Hc4CaR6JBL0", sync_wait32),
    HLE_FUNC_ENTRY("libkernel", "#B2n8aDorSH4", sync_wait32),
    HLE_FUNC_ENTRY("libkernel", "#PZQhiiLXRFs", sync_wait64),
    HLE_FUNC_ENTRY("libkernel", "#q2y-wDIVWZA", sync_wake),
    HLE_FUNC_ENTRY("libkernel", "_umtx_op", umtx_op),
    POSIX_ENTRY("pthread_mutex_init", px_mutex_init),
    POSIX_ENTRY("pthread_mutex_destroy", px_mutex_destroy),
    POSIX_ENTRY("pthread_mutex_lock", px_mutex_lock),
    POSIX_ENTRY("pthread_mutex_trylock", px_mutex_trylock),
    POSIX_ENTRY("pthread_mutex_timedlock", px_mutex_timedlock),
    POSIX_ENTRY("pthread_mutex_unlock", px_mutex_unlock),
    POSIX_ENTRY("pthread_mutexattr_init", px_mutexattr_init),
    POSIX_ENTRY("pthread_mutexattr_destroy", px_mutexattr_destroy),
    POSIX_ENTRY("pthread_mutexattr_settype", px_mutexattr_settype),
    POSIX_ENTRY("pthread_mutexattr_setprotocol", px_mutexattr_setprotocol),
    POSIX_ENTRY("pthread_cond_init", px_cond_init),
    POSIX_ENTRY("pthread_cond_destroy", px_cond_destroy),
    POSIX_ENTRY("pthread_cond_wait", px_cond_wait),
    POSIX_ENTRY("pthread_cond_timedwait", px_cond_timedwait),
    POSIX_ENTRY("pthread_cond_signal", px_cond_signal),
    POSIX_ENTRY("pthread_cond_broadcast", px_cond_broadcast),
    POSIX_ENTRY("pthread_condattr_init", px_condattr_init),
    POSIX_ENTRY("pthread_condattr_destroy", px_condattr_destroy),
    POSIX_ENTRY("pthread_condattr_setclock", px_condattr_setclock),
    POSIX_ENTRY("pthread_condattr_getclock", px_condattr_getclock),
    POSIX_ENTRY("pthread_self", pthread_self_),
    POSIX_ENTRY("pthread_equal", pthread_equal_),

    HLE_FUNC_ENTRY(K, "scePthreadMutexInit", sce_mutex_init),
    HLE_FUNC_ENTRY(K, "scePthreadMutexDestroy", sce_mutex_destroy),
    HLE_FUNC_ENTRY(K, "scePthreadMutexLock", sce_mutex_lock),
    HLE_FUNC_ENTRY(K, "scePthreadMutexTrylock", sce_mutex_trylock),
    HLE_FUNC_ENTRY(K, "scePthreadMutexTimedlock", sce_mutex_timedlock),
    HLE_FUNC_ENTRY(K, "scePthreadMutexUnlock", sce_mutex_unlock),
    HLE_FUNC_ENTRY(K, "scePthreadMutexattrInit", sce_mutexattr_init),
    HLE_FUNC_ENTRY(K, "scePthreadMutexattrDestroy", sce_mutexattr_destroy),
    HLE_FUNC_ENTRY(K, "scePthreadMutexattrSettype", sce_mutexattr_settype),
    HLE_FUNC_ENTRY(K, "scePthreadMutexattrSetprotocol", sce_mutexattr_setprotocol),
    HLE_FUNC_ENTRY(K, "scePthreadCondInit", sce_cond_init),
    HLE_FUNC_ENTRY(K, "scePthreadCondDestroy", sce_cond_destroy),
    HLE_FUNC_ENTRY(K, "scePthreadCondWait", sce_cond_wait),
    HLE_FUNC_ENTRY(K, "scePthreadCondTimedwait", sce_cond_timedwait),
    HLE_FUNC_ENTRY(K, "scePthreadCondSignal", sce_cond_signal),
    HLE_FUNC_ENTRY(K, "scePthreadCondBroadcast", sce_cond_broadcast),
    HLE_FUNC_ENTRY(K, "scePthreadCondattrInit", sce_condattr_init),
    HLE_FUNC_ENTRY(K, "scePthreadCondattrDestroy", sce_condattr_destroy),
    HLE_FUNC_ENTRY(K, "scePthreadSelf", pthread_self_),
    HLE_FUNC_ENTRY(K, "scePthreadEqual", pthread_equal_),

    POSIX_ENTRY("pthread_rwlock_init", px_rwlock_init),
    POSIX_ENTRY("pthread_rwlock_destroy", px_rwlock_destroy),
    POSIX_ENTRY("pthread_rwlock_rdlock", px_rwlock_rdlock),
    POSIX_ENTRY("pthread_rwlock_wrlock", px_rwlock_wrlock),
    POSIX_ENTRY("pthread_rwlock_tryrdlock", px_rwlock_tryrdlock),
    POSIX_ENTRY("pthread_rwlock_trywrlock", px_rwlock_trywrlock),
    POSIX_ENTRY("pthread_rwlock_unlock", px_rwlock_unlock),
    POSIX_ENTRY("pthread_rwlockattr_init", rwlockattr_noop),
    POSIX_ENTRY("pthread_rwlockattr_destroy", rwlockattr_noop),
    HLE_FUNC_ENTRY(K, "scePthreadRwlockInit", sce_rwlock_init),
    HLE_FUNC_ENTRY(K, "scePthreadRwlockDestroy", sce_rwlock_destroy),
    HLE_FUNC_ENTRY(K, "scePthreadRwlockRdlock", sce_rwlock_rdlock),
    HLE_FUNC_ENTRY(K, "scePthreadRwlockWrlock", sce_rwlock_wrlock),
    HLE_FUNC_ENTRY(K, "scePthreadRwlockTryrdlock", sce_rwlock_tryrdlock),
    HLE_FUNC_ENTRY(K, "scePthreadRwlockTrywrlock", sce_rwlock_trywrlock),
    HLE_FUNC_ENTRY(K, "scePthreadRwlockTimedrdlock", sce_rwlock_timedrdlock),
    HLE_FUNC_ENTRY(K, "scePthreadRwlockTimedwrlock", sce_rwlock_timedwrlock),
    HLE_FUNC_ENTRY(K, "scePthreadRwlockUnlock", sce_rwlock_unlock),
    HLE_FUNC_ENTRY(K, "scePthreadRwlockattrInit", rwlockattr_noop),
    HLE_FUNC_ENTRY(K, "scePthreadRwlockattrDestroy", rwlockattr_noop),
    HLE_FUNC_ENTRY(K, "scePthreadRwlockattrSettype", rwlockattr_noop),
    HLE_FUNC_ENTRY(K, "scePthreadRwlockattrGettype", rwlockattr_gettype),
    POSIX_ENTRY("pthread_rwlockattr_setpshared", rwlockattr_noop),
    POSIX_ENTRY("pthread_rwlockattr_getpshared", rwlockattr_getpshared),

    HLE_FUNC_ENTRY(K, "sceKernelCreateEventFlag", create_event_flag),
    HLE_FUNC_ENTRY(K, "sceKernelDeleteEventFlag", delete_event_flag),
    HLE_FUNC_ENTRY(K, "sceKernelSetEventFlag", set_event_flag),
    HLE_FUNC_ENTRY(K, "sceKernelClearEventFlag", clear_event_flag),
    HLE_FUNC_ENTRY(K, "sceKernelWaitEventFlag", wait_event_flag),
    HLE_FUNC_ENTRY(K, "sceKernelPollEventFlag", poll_event_flag),
    HLE_FUNC_ENTRY(K, "sceKernelCancelEventFlag", cancel_event_flag),

    HLE_FUNC_ENTRY(K, "sceKernelCreateSema", create_sema),
    HLE_FUNC_ENTRY(K, "sceKernelDeleteSema", delete_sema),
    HLE_FUNC_ENTRY(K, "sceKernelWaitSema", wait_sema),
    HLE_FUNC_ENTRY(K, "sceKernelPollSema", poll_sema),
    HLE_FUNC_ENTRY(K, "sceKernelSignalSema", signal_sema),
    POSIX_ENTRY("sem_init", psem_init),
    POSIX_ENTRY("sem_destroy", psem_destroy),
    POSIX_ENTRY("sem_wait", psem_wait),
    POSIX_ENTRY("sem_trywait", psem_trywait),
    POSIX_ENTRY("sem_timedwait", psem_timedwait),
    POSIX_ENTRY("sem_post", psem_post),
    POSIX_ENTRY("sem_getvalue", psem_getvalue),
};

void hle_register_libkernel_sync(void) {
    if (!kobj_init()) {
        LOG_E(KERNEL, "cannot create the kernel object registry");
        return;
    }
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}
