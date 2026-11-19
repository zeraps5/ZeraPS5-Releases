/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hle/equeue.h"
#include "hle/hle.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "hrtimer line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static gmem mem;
static guest_addr timespec_addr;
static int32_t call(const char *name, uint64_t eq, int32_t id, uint64_t ts, uint64_t udata) {
    const hle_export *exp = hle_find("libkernel", nid_hash_name(name));
    CHECK(exp && exp->fn);
    hle_args args = {.gpr = {eq, (uint64_t)(int64_t)id, ts, udata}};
    hle_call c = {.mem = &mem, .a = &args};
    exp->fn(&c);
    return (int32_t)args.ret[0];
}
static int32_t arm(uint64_t eq, int32_t id, int64_t sec, int64_t ns, uint64_t udata) {
    int64_t ts[2] = {sec, ns};
    CHECK(gmem_write(&mem, timespec_addr, ts, sizeof(ts)));
    return call("sceKernelAddHRTimerEvent", eq, id, timespec_addr, udata);
}
static int32_t cancel(uint64_t eq, int32_t id) {
    return call("sceKernelDeleteHRTimerEvent", eq, id, 0, 0);
}
static int32_t arm_from_thread(void *arg) {
    pal_sleep_ns(20000000);
    return arm(*(uint64_t *)arg, 123, 0, 1000000, 456);
}
int main(void) {
    CHECK(pal_init() == PS5C_OK && hle_registry_init() == PS5C_OK);
    gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&mem, &cfg) == PS5C_OK);
    CHECK(gmem_map(&mem, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "hrtimer test", &timespec_addr) == PS5C_OK);
    uint64_t eq = equeue_create("hrtimer test"); CHECK(eq);
    guest_kevent ev[8];
    CHECK(arm(eq, 7, -1, 0, 0) == SCE_KERNEL_ERROR_EINVAL);
    CHECK(arm(eq, 7, 0, -1, 0) == SCE_KERNEL_ERROR_EINVAL);
    CHECK(arm(eq, 7, 0, 1000000000, 0) == SCE_KERNEL_ERROR_EINVAL);
    CHECK(call("sceKernelAddHRTimerEvent", eq, 7, UINT64_MAX, 0) == SCE_KERNEL_ERROR_EFAULT);
    CHECK(arm(0, 7, 0, 1, 0) != 0);
    CHECK(equeue_wait(eq, ev, 8, 0) == 0);

    /* A relative deadline cannot be reported early, and is removed on delivery. */
    uint64_t start = pal_time_ns();
    CHECK(arm(eq, 7, 0, 200000000, 0x1234) == 0);
    CHECK(equeue_wait(eq, ev, 8, 1000000) == 0);
    CHECK(equeue_wait(eq, ev, 8, 3000000000ull) == 1);
    CHECK(pal_time_ns() - start >= 200000000);
    CHECK(ev[0].ident == 7 && ev[0].filter == -15 && ev[0].udata == 0x1234);
    CHECK(ev[0].flags == 0x11 && ev[0].fflags == 0 && ev[0].data == 0);
    CHECK(cancel(eq, 7) == (int32_t)0x80020002);
    CHECK(equeue_wait(eq, ev, 8, 1000000) == 0);

    /* Zero delay is immediate; signed IDs and all expiries survive a swap/remove. */
    const int32_t ids[] = {0, 1, INT32_MAX, INT32_MIN, -1, -1439450573};
    for (unsigned i = 0; i < PS5C_ARRAY_COUNT(ids); i++) CHECK(arm(eq, ids[i], 0, 0, i + 10) == 0);
    unsigned seen = 0;
    for (int batch = 0; batch < 3; batch++) {
        CHECK(equeue_wait(eq, ev, 2, 0) == 2);
        for (int k = 0; k < 2; k++) {
            CHECK(ev[k].filter == -15 && ev[k].udata >= 10 && ev[k].udata < 16);
            unsigned i = (unsigned)(ev[k].udata - 10);
            CHECK(ev[k].ident == (uint64_t)(int64_t)ids[i] && !(seen & (1u << i)));
            seen |= 1u << i;
        }
    }
    CHECK(seen == 63 && equeue_wait(eq, ev, 8, 0) == 0);

    /* Rearming replaces the old deadline/udata, including an already-due timer. */
    CHECK(arm(eq, 7, 0, 0, 1) == 0 && arm(eq, 7, 1, 0, 2) == 0);
    CHECK(equeue_wait(eq, ev, 8, 0) == 0);
    CHECK(arm(eq, 7, 0, 0, 3) == 0 && equeue_wait(eq, ev, 8, 0) == 1);
    CHECK(ev[0].udata == 3);
    CHECK(arm(eq, 7, 0, 1000000, 4) == 0 && cancel(eq, 7) == 0);
    CHECK(equeue_wait(eq, ev, 8, 2000000) == 0);

    /* Same identifier may coexist across filters; periodic timers still repeat. */
    CHECK(equeue_add(eq, 7, EVFILT_USER_, 55));
    CHECK(equeue_push(eq, 7, EVFILT_USER_, 99));
    CHECK(equeue_add_timer(eq, 7, 1000000, 66));
    CHECK(arm(eq, 7, 0, 0, 77) == 0);
    int n = equeue_wait(eq, ev, 8, 0); CHECK(n >= 2);
    bool user = false, hr = false;
    for (int i = 0; i < n; i++) {
        if (ev[i].filter == EVFILT_USER_) user = ev[i].udata == 55 && ev[i].data == 99;
        if (ev[i].filter == -15) hr = ev[i].udata == 77;
    }
    CHECK(user && hr);
    CHECK(equeue_wait(eq, ev, 8, 100000000) == 1 && ev[0].filter == EVFILT_TIMER_ && ev[0].data >= 1);
    CHECK(equeue_wait(eq, ev, 8, 100000000) == 1 && ev[0].filter == EVFILT_TIMER_);
    CHECK(equeue_remove(eq, 7, EVFILT_TIMER_) && equeue_remove(eq, 7, EVFILT_USER_));
    /* Large durations must not wrap to an immediate expiry. */
    CHECK(arm(eq, 7, INT64_MAX, 999999999, 0) == 0);
    CHECK(equeue_wait(eq, ev, 8, 1000000) == 0 && cancel(eq, 7) == 0);
    /* A newly armed timer wakes a waiter that originally had no timer deadline. */
    pal_thread *worker = NULL;
    CHECK(pal_thread_create(&worker, arm_from_thread, &eq, "hrtimer arm", 0) == PS5C_OK);
    CHECK(equeue_wait(eq, ev, 8, 3000000000ull) == 1);
    CHECK(ev[0].ident == 123 && ev[0].filter == -15 && ev[0].udata == 456);
    int32_t result = -1;
    CHECK(pal_thread_join(worker, &result) == PS5C_OK && result == 0);
    CHECK(equeue_destroy(eq));
    gmem_free(&mem); hle_registry_free(); pal_shutdown();
    puts("hrtimer: deadline, one-shot removal, replacement, cancellation, IDs and filter isolation verified");
    return 0;
}
