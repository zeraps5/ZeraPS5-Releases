/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Kernel event queues (kqueue-style), shared by libkernel user events,
 * VideoOut flip/vblank events and GPU events.
 *
 * A queue holds registrations keyed by (ident, filter). Triggering a
 * registration marks it ready and updates its data; sceKernelWaitEqueue
 * returns ready events and clears them (EV_CLEAR semantics). */
#ifndef PS5C_EQUEUE_H
#define PS5C_EQUEUE_H

#include "base/base.h"

/* Filters (FreeBSD + Sony) */
#define EVFILT_TIMER_ (-7)
#define EVFILT_USER_ (-11)
#define EVFILT_VIDEO_OUT_ (-13)
#define EVFILT_GRAPHICS_ (-14)
#define EVFILT_HRTIMER_ (-15)
/* APR completions have their own filter, independent of user events. */
#define EVFILT_AMPR_ (-25)

/* struct kevent as the guest sees it (32 bytes) */
typedef struct guest_kevent {
    uint64_t ident;
    int16_t filter;
    uint16_t flags;
    uint32_t fflags;
    int64_t data;
    uint64_t udata;
} guest_kevent;

/* Computes the new `data` of a triggered event from the previous one. */
typedef int64_t (*equeue_data_fn)(int64_t old_data, void *ctx);

/* A periodic EVFILT_TIMER event (sceKernelAddTimerEvent). */
bool equeue_add_timer(uint64_t handle, uint64_t ident, uint64_t period_ns, uint64_t udata);

bool equeue_init(void);
/* Handle of a live queue, or 0. */
uint64_t equeue_create(const char *name);
bool equeue_destroy(uint64_t handle);
bool equeue_valid(uint64_t handle);

bool equeue_add(uint64_t handle, uint64_t ident, int16_t filter, uint64_t udata);
bool equeue_remove(uint64_t handle, uint64_t ident, int16_t filter);
/* Marks (ident, filter) ready. `fn` NULL keeps `data` as given in ctx (int64_t*). */
bool equeue_trigger(uint64_t handle, uint64_t ident, int16_t filter, equeue_data_fn fn, void *ctx, uint64_t udata,
                    bool set_udata);

/* Delivers one event with `data`; triggers that come before the previous one
 * is received are queued, not merged (AMPR completion events). */
bool equeue_push(uint64_t handle, uint64_t ident, int16_t filter, int64_t data);

/* Waits up to timeout_ns (UINT64_MAX = forever) for ready events and copies
 * at most `max` of them. Returns the count, or -1 if the handle is invalid. */
int equeue_wait(uint64_t handle, guest_kevent *out, int max, uint64_t timeout_ns);

/* Logs the queue's registrations (thread dumps: what a waiting thread waits for). */
void equeue_log_state(uint64_t handle);

void hle_register_equeue(void);

#endif
