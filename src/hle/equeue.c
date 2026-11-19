/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hle/equeue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/hle.h"
#include "hle/net_loopback.h"
#include "hle/kernel_objects.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

typedef struct eq_event {
    guest_kevent ev;
    bool ready;
    /* Queued events (equeue_push): each trigger is delivered on its own,
     * with its own data, in order. */
    int64_t *queue;
    uint32_t queued, queue_cap;
    /* EVFILT_TIMER: fires every period_ns from next_ns on; data counts the
     * expirations since the last delivery. Expired while the queue is waited on. */
    uint64_t period_ns, next_ns;
} eq_event;

typedef struct equeue_obj {
    obj_kind kind;
    pal_mutex *lock;
    pal_cond *changed;
    eq_event *events;
    uint32_t count, cap;
    bool closing;
    char name[32];
} equeue_obj;

bool equeue_init(void) { return kobj_init(); }

static equeue_obj *get(uint64_t handle) { return (equeue_obj *)kobj_check(handle, OBJ_EQUEUE); }

bool equeue_valid(uint64_t handle) { return get(handle) != NULL; }

uint64_t equeue_create(const char *name) {
    equeue_obj *q = (equeue_obj *)kobj_new(OBJ_EQUEUE, sizeof(equeue_obj));
    if (!q) return 0;
    if (pal_mutex_create(&q->lock) != PS5C_OK || pal_cond_create(&q->changed) != PS5C_OK) {
        if (q->lock) pal_mutex_destroy(q->lock);
        kobj_delete(q);
        return 0;
    }
    snprintf(q->name, sizeof(q->name), "%s", name ? name : "");
    return (uint64_t)(uintptr_t)q;
}

bool equeue_destroy(uint64_t handle) {
    equeue_obj *q = get(handle);
    if (!q) return false;
    /* Waiters are woken and see an invalid handle; the object stays allocated
     * (leaked) so a waiter never touches freed memory. */
    pal_mutex_lock(q->lock);
    q->closing = true;
    pal_cond_broadcast(q->changed);
    pal_mutex_unlock(q->lock);
    kobj_forget(q);
    return true;
}

static eq_event *find(equeue_obj *q, uint64_t ident, int16_t filter) {
    for (uint32_t i = 0; i < q->count; i++) {
        if (q->events[i].ev.ident == ident && q->events[i].ev.filter == filter) return &q->events[i];
    }
    return NULL;
}

bool equeue_add(uint64_t handle, uint64_t ident, int16_t filter, uint64_t udata) {
    equeue_obj *q = get(handle);
    if (!q) return false;
    pal_mutex_lock(q->lock);
    eq_event *e = find(q, ident, filter);
    if (!e) {
        if (q->count == q->cap) {
            uint32_t cap = q->cap ? q->cap * 2 : 8;
            eq_event *n = (eq_event *)realloc(q->events, cap * sizeof(eq_event));
            if (!n) {
                pal_mutex_unlock(q->lock);
                return false;
            }
            q->events = n;
            q->cap = cap;
        }
        e = &q->events[q->count++];
        memset(e, 0, sizeof(*e));
        e->ev.ident = ident;
        e->ev.filter = filter;
    }
    e->ev.udata = udata;
    pal_mutex_unlock(q->lock);
    return true;
}

bool equeue_remove(uint64_t handle, uint64_t ident, int16_t filter) {
    equeue_obj *q = get(handle);
    if (!q) return false;
    pal_mutex_lock(q->lock);
    eq_event *e = find(q, ident, filter);
    if (e) {
        free(e->queue);
        *e = q->events[--q->count];
    }
    pal_mutex_unlock(q->lock);
    return e != NULL;
}

bool equeue_push(uint64_t handle, uint64_t ident, int16_t filter, int64_t data) {
    equeue_obj *q = get(handle);
    if (!q) return false;
    pal_mutex_lock(q->lock);
    eq_event *e = find(q, ident, filter);
    bool ok = e != NULL;
    if (e && !e->ready) {
        e->ev.data = data;
        e->ready = true;
    } else if (e) {
        if (e->queued == e->queue_cap) {
            uint32_t cap = e->queue_cap ? e->queue_cap * 2 : 8;
            int64_t *n = (int64_t *)realloc(e->queue, cap * sizeof(int64_t));
            ok = n != NULL;
            if (n) {
                e->queue = n;
                e->queue_cap = cap;
            }
        }
        if (ok) e->queue[e->queued++] = data;
    }
    if (ok) pal_cond_broadcast(q->changed);
    pal_mutex_unlock(q->lock);
    return ok;
}

bool equeue_trigger(uint64_t handle, uint64_t ident, int16_t filter, equeue_data_fn fn, void *ctx, uint64_t udata,
                    bool set_udata) {
    equeue_obj *q = get(handle);
    if (!q) return false;
    pal_mutex_lock(q->lock);
    eq_event *e = find(q, ident, filter);
    if (e) {
        e->ev.data = fn ? fn(e->ev.data, ctx) : (ctx ? *(const int64_t *)ctx : 0);
        if (e->ev.fflags < 0xF) e->ev.fflags++;
        if (set_udata) e->ev.udata = udata;
        e->ready = true;
        pal_cond_broadcast(q->changed);
    }
    pal_mutex_unlock(q->lock);
    return e != NULL;
}

/* Timers due by `now` become ready; returns the earliest pending expiry (UINT64_MAX: none). */
static uint64_t expire_timers(equeue_obj *q, uint64_t now) {
    uint64_t next = UINT64_MAX;
    for (uint32_t i = 0; i < q->count; i++) {
        eq_event *e = &q->events[i];
        if (!e->period_ns) continue;
        if (now >= e->next_ns) {
            uint64_t n = (now - e->next_ns) / e->period_ns + 1;
            e->ev.data = (e->ready ? e->ev.data : 0) + (int64_t)n;
            e->next_ns += n * e->period_ns;
            e->ready = true;
        }
        if (e->next_ns < next) next = e->next_ns;
    }
    return next;
}

bool equeue_add_timer(uint64_t handle, uint64_t ident, uint64_t period_ns, uint64_t udata) {
    if (!period_ns || !equeue_add(handle, ident, EVFILT_TIMER_, udata)) return false;
    equeue_obj *q = get(handle);
    pal_mutex_lock(q->lock);
    eq_event *e = find(q, ident, EVFILT_TIMER_);
    if (e) {
        e->period_ns = period_ns;
        e->next_ns = pal_time_ns() + period_ns;
        e->ready = false;
        pal_cond_broadcast(q->changed); /* a waiter recomputes its wake-up time */
    }
    pal_mutex_unlock(q->lock);
    return e != NULL;
}

int equeue_wait(uint64_t handle, guest_kevent *out, int max, uint64_t timeout_ns) {
    equeue_obj *q = get(handle);
    if (!q) return -1;
    uint64_t deadline = timeout_ns == UINT64_MAX ? UINT64_MAX : pal_time_ns() + timeout_ns;
    pal_mutex_lock(q->lock);
    int n = 0;
    for (;;) {
        if (q->closing) break;
        uint64_t timer = expire_timers(q, pal_time_ns());
        for (uint32_t i = 0; i < q->count && n < max; i++) {
            eq_event *e = &q->events[i];
            if (!e->ready) continue;
            out[n++] = e->ev;
            e->ready = false; /* EV_CLEAR */
            e->ev.fflags = 0;
            if (e->queued) { /* the next queued event is ready at once */
                e->ev.data = e->queue[0];
                memmove(e->queue, e->queue + 1, --e->queued * sizeof(int64_t));
                e->ready = true;
            }
        }
        if (n > 0) break;
        uint64_t wake = timer < deadline ? timer : deadline;
        if (wake == UINT64_MAX) {
            pal_cond_wait(q->changed, q->lock);
        } else {
            uint64_t now = pal_time_ns();
            if (now >= deadline) break;
            if (now < wake) pal_cond_wait_ns(q->changed, q->lock, wake - now);
        }
    }
    pal_mutex_unlock(q->lock);
    return n;
}

void equeue_log_state(uint64_t handle) {
    equeue_obj *q = get(handle);
    if (!q) return;
    pal_mutex_lock(q->lock);
    for (uint32_t i = 0; i < q->count; i++) {
        const eq_event *e = &q->events[i];
        LOG_I(CORE, "      equeue '%s' event ident 0x%llx filter %d: %s, %u queued, data 0x%llx", q->name,
              (unsigned long long)e->ev.ident, e->ev.filter, e->ready ? "ready" : "not ready", e->queued,
              (unsigned long long)e->ev.data);
    }
    pal_mutex_unlock(q->lock);
}

/* ---- libkernel entry points ---- */

#define SCE_KERNEL_ERROR_ETIMEDOUT ((int32_t)0x8002003C)
#define SCE_KERNEL_ERROR_EBADF ((int32_t)0x80020009)
#define SCE_KERNEL_ERROR_ENOENT_ ((int32_t)0x80020002)

/* int sceKernelCreateEqueue(SceKernelEqueue *eq, const char *name) */
static void create_equeue(hle_call *c) {
    uint64_t *out = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 0), 8);
    if (!out) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    char name[32] = "";
    if (hle_arg_ptr(c, 1)) hle_read_str(c, hle_arg_ptr(c, 1), name, sizeof(name));
    uint64_t h = equeue_create(name);
    if (!h) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_ENOMEM);
        return;
    }
    *out = h;
    hle_ret_s32(c, 0);
}

static void delete_equeue(hle_call *c) {
    hle_ret_s32(c, equeue_destroy(hle_arg(c, 0)) ? 0 : SCE_KERNEL_ERROR_EBADF);
}

/* int sceKernelWaitEqueue(SceKernelEqueue eq, SceKernelEvent *ev, int num, int *out, SceKernelUseconds *timo) */
static void wait_equeue(hle_call *c) {
    int32_t num = hle_arg_s32(c, 2);
    guest_kevent *ev = num > 0 ? (guest_kevent *)hle_mem(c, hle_arg_ptr(c, 1), (uint64_t)num * sizeof(guest_kevent))
                               : NULL;
    int32_t *out = (int32_t *)hle_mem(c, hle_arg_ptr(c, 3), 4);
    uint32_t *timo = (uint32_t *)hle_mem(c, hle_arg_ptr(c, 4), 4);
    /* every failure reports 0 events (rps5e 6b0d37c): callers loop over *out */
    if (out) *out = 0;
    if (!ev || !out) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    uint64_t timeout = timo ? (uint64_t)*timo * 1000u : UINT64_MAX;
    int n = equeue_wait(hle_arg(c, 0), ev, num, timeout);
    if (n < 0) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EBADF);
        return;
    }
    *out = n;
    hle_ret_s32(c, n == 0 && timo ? SCE_KERNEL_ERROR_ETIMEDOUT : 0);
}

static void add_user_event(hle_call *c) {
    hle_ret_s32(c, equeue_add(hle_arg(c, 0), (uint64_t)hle_arg_s32(c, 1), EVFILT_USER_, 0) ? 0 : SCE_KERNEL_ERROR_EBADF);
}

static void delete_user_event(hle_call *c) {
    hle_ret_s32(c, equeue_remove(hle_arg(c, 0), (uint64_t)hle_arg_s32(c, 1), EVFILT_USER_) ? 0
                                                                                            : SCE_KERNEL_ERROR_ENOENT_);
}

/* int sceKernelTriggerUserEvent(SceKernelEqueue eq, int id, void *udata) */
static void trigger_user_event(hle_call *c) {
    int64_t zero = 0;
    bool ok = equeue_trigger(hle_arg(c, 0), (uint64_t)hle_arg_s32(c, 1), EVFILT_USER_, NULL, &zero, hle_arg(c, 2), true);
    hle_ret_s32(c, ok ? 0 : SCE_KERNEL_ERROR_ENOENT_);
}

/* int sceKernelAddTimerEvent(SceKernelEqueue eq, int id, SceKernelUseconds usec, void *udata): periodic */
static void add_timer_event(hle_call *c) {
    uint64_t usec = (uint32_t)hle_arg(c, 2);
    if (!usec) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    hle_ret_s32(c, equeue_add_timer(hle_arg(c, 0), (uint64_t)hle_arg_s32(c, 1), usec * 1000u, hle_arg(c, 3))
                       ? 0
                       : SCE_KERNEL_ERROR_EBADF);
}

static void delete_timer_event(hle_call *c) {
    hle_ret_s32(c, equeue_remove(hle_arg(c, 0), (uint64_t)hle_arg_s32(c, 1), EVFILT_TIMER_) ? 0
                                                                                             : SCE_KERNEL_ERROR_ENOENT_);
}

static const guest_kevent *event_arg(hle_call *c) {
    return (const guest_kevent *)hle_mem(c, hle_arg_ptr(c, 0), sizeof(guest_kevent));
}

static void get_event_id(hle_call *c) {
    const guest_kevent *e = event_arg(c);
    hle_ret_u64(c, e ? e->ident : 0);
}
static void get_event_filter(hle_call *c) {
    const guest_kevent *e = event_arg(c);
    hle_ret_s32(c, e ? e->filter : 0);
}
static void get_event_data(hle_call *c) {
    const guest_kevent *e = event_arg(c);
    hle_ret_u64(c, e ? (uint64_t)e->data : 0);
}
static void get_event_fflags(hle_call *c) {
    const guest_kevent *e = event_arg(c);
    hle_ret_u32(c, e ? e->fflags : 0);
}
static void get_event_user_data(hle_call *c) {
    const guest_kevent *e = event_arg(c);
    hle_ret_u64(c, e ? e->udata : 0);
}

#define K "libkernel"

static void socket_event(hle_call *c, int16_t filter, bool remove) {
    int64_t result = 0;
    net_loopback_watch(hle_arg(c, 0), hle_arg_s32(c, 1), filter, hle_arg(c, 2), hle_arg(c, 3),
                       remove, &result);
    hle_ret_s32(c, result < 0 ? (int32_t)(0x80020000u | (uint32_t)-result) : 0);
}
static void add_read_event(hle_call *c) {
    socket_event(c, -1, false);
}
static void delete_read_event(hle_call *c) {
    socket_event(c, -1, true);
}
static void add_write_event(hle_call *c) {
    socket_event(c, -2, false);
}
static void delete_write_event(hle_call *c) {
    socket_event(c, -2, true);
}

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(K, "sceKernelCreateEqueue", create_equeue),
    HLE_FUNC_ENTRY(K, "sceKernelDeleteEqueue", delete_equeue),
    HLE_FUNC_ENTRY(K, "sceKernelWaitEqueue", wait_equeue),
    HLE_FUNC_ENTRY(K, "sceKernelAddUserEvent", add_user_event),
    /* Local socket channels signal actual buffered bytes; external offline
     * sockets continue to wait without synthetic data. */
    HLE_FUNC_ENTRY(K, "sceKernelAddReadEvent", add_read_event),
    HLE_FUNC_ENTRY(K, "sceKernelDeleteReadEvent", delete_read_event),
    HLE_FUNC_ENTRY(K, "sceKernelAddWriteEvent", add_write_event),
    HLE_FUNC_ENTRY(K, "sceKernelDeleteWriteEvent", delete_write_event),
    HLE_FUNC_ENTRY(K, "sceKernelAddUserEventEdge", add_user_event),
    HLE_FUNC_ENTRY(K, "sceKernelDeleteUserEvent", delete_user_event),
    HLE_FUNC_ENTRY(K, "sceKernelTriggerUserEvent", trigger_user_event),
    HLE_FUNC_ENTRY(K, "sceKernelAddTimerEvent", add_timer_event),
    HLE_FUNC_ENTRY(K, "sceKernelDeleteTimerEvent", delete_timer_event),
    HLE_FUNC_ENTRY(K, "sceKernelGetEventId", get_event_id),
    HLE_FUNC_ENTRY(K, "sceKernelGetEventFilter", get_event_filter),
    HLE_FUNC_ENTRY(K, "sceKernelGetEventData", get_event_data),
    HLE_FUNC_ENTRY(K, "sceKernelGetEventFflags", get_event_fflags),
    HLE_FUNC_ENTRY(K, "sceKernelGetEventUserData", get_event_user_data),
};

void hle_register_equeue(void) {
    if (!equeue_init()) {
        LOG_E(KERNEL, "cannot create the kernel object registry");
        return;
    }
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}
