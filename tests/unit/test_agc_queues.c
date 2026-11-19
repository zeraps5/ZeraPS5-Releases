/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/settings.h"
#include "hle/hle.h"
#include "hle/equeue.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include "test.h"

static gmem mem;
static guest_addr base;
static volatile uint32_t barrier_done;
static int32_t invoke4(const char *name, uint64_t a, uint64_t b, uint64_t d, uint64_t e) {
    uint64_t nid = nid_hash_name(name);
    if (name[0] == '#')
        nid_from_str(name + 1, NID_STR_LEN, &nid);
    const hle_export *exp = hle_find("libSceAgcDriver", nid);
    if (!exp || !exp->fn)
        return INT32_MIN;
    hle_args args = {.gpr = {a, b, d, e}};
    hle_call c = {.a = &args, .mem = &mem};
    exp->fn(&c);
    return (int32_t)args.ret[0];
}
static int32_t invoke(const char *name, uint64_t a, uint64_t b, uint64_t d) {
    return invoke4(name, a, b, d, 0);
}
static int32_t barrier_thread(void *arg) {
    PS5C_UNUSED(arg);
    int32_t result = invoke("#kAPt2J4U3v8", base + 128, 0, 1);
    __atomic_store_n(&barrier_done, result == 0 ? 1u : 2u, __ATOMIC_RELEASE);
    return result;
}
static void put(guest_addr at, const uint32_t *p, uint32_t n) {
    gmem_write(&mem, at, p, n * 4);
}
static uint32_t value(guest_addr at) {
    uint32_t v = 0;
    gmem_read(&mem, at, &v, 4);
    return v;
}
static bool wait_value(guest_addr at, uint32_t v) {
    uint64_t end = pal_time_ns() + 3000000000ull;
    do {
        if (value(at) == v)
            return true;
        pal_sleep_ns(1000000);
    } while (pal_time_ns() < end);
    return false;
}
static void descriptor(guest_addr at, guest_addr cmd, uint32_t n) {
    gmem_write_u64(&mem, at, cmd);
    put(at + 8, &n, 1);
}
static void atomic_add(uint32_t *p, guest_addr at) {
    uint32_t a[] = {0xc0061e00, 9, (uint32_t)at, (uint32_t)(at >> 32), 1, 0, 0, 0};
    memcpy(p, a, sizeof(a));
}
static void test_cross_queue_release_and_completion_prefix(void) {
    guest_addr reached = base + 8, label = base + 16, after = base + 24, compute = base + 32,
               tail = base + 40;
    guest_addr acmd = base + 1024, bcmd = base + 2048, tcmd = base + 3072;
    uint32_t a[35] = {0xc0017600, 0x207, 11};
    atomic_add(a + 3, reached);
    uint32_t wait[] = {0xc0053c00, 0x13, (uint32_t)label, (uint32_t)(label >> 32), 1, ~0u, 4};
    memcpy(a + 11, wait, sizeof(wait));
    a[18] = 0xc0005900;
    a[19] = 0;
    uint32_t regwait[] = {0xc0053c00, 3, 0x2e07, 0, 11, ~0u, 4};
    memcpy(a + 20, regwait, sizeof(regwait));
    atomic_add(a + 27, after);
    put(acmd, a, 35);
    descriptor(base + 64, acmd, 35);
    uint32_t b[16] = {
        0xc0017600, 0x207, 22, 0xc0033700, 0x500, (uint32_t)label, (uint32_t)(label >> 32), 1};
    atomic_add(b + 8, compute);
    put(bcmd, b, 16);
    descriptor(base + 80, bcmd, 16);
    uint32_t t[8];
    atomic_add(t, tail);
    put(tcmd, t, 8);
    descriptor(base + 96, tcmd, 8);
    descriptor(base + 112, bcmd + 32, 8);
    CHECK(invoke("sceAgcDriverSubmitDcb", base + 64, 0, 0) == 0);
    CHECK(wait_value(reached, 1));
    CHECK(invoke("sceAgcDriverSubmitDcb", base + 96, 0, 0) == 0);
    pal_thread *thread = NULL;
    CHECK(pal_thread_create(&thread, barrier_thread, NULL, "CP barrier", 0) == PS5C_OK);
    pal_sleep_ns(20000000);
    CHECK(!__atomic_load_n(&barrier_done, __ATOMIC_ACQUIRE));
    CHECK(invoke("sceAgcDriverSubmitAcb", 32, base + 80, 0) == 0);
    CHECK(invoke("sceAgcDriverSubmitAcb", 32, base + 112, 0) == 0);
    CHECK(invoke("sceAgcDriverSubmitAcb", 32, base + 112, 0) == 0);
    CHECK(wait_value(compute, 3));
    pal_sleep_ns(20000000);
    CHECK(value(after) == 0 && value(tail) == 0 &&
          !__atomic_load_n(&barrier_done, __ATOMIC_ACQUIRE));
    uint32_t valid = 0x80000000;
    put(acmd + 19 * 4, &valid, 1);
    CHECK(wait_value(after, 1) && wait_value(tail, 1));
    uint64_t end = pal_time_ns() + 3000000000ull;
    while (!__atomic_load_n(&barrier_done, __ATOMIC_ACQUIRE) && pal_time_ns() < end)
        pal_sleep_ns(1000000);
    CHECK(__atomic_load_n(&barrier_done, __ATOMIC_ACQUIRE) == 1);
    int32_t ret = -1;
    CHECK(pal_thread_join(thread, &ret) == PS5C_OK && ret == 0);
    CHECK(value(reached) == 1);
    /* Exercise slot reuse past the ring's 1024 entries. */
    for (uint32_t i = 0; i < 1100; i++)
        CHECK(invoke("sceAgcDriverSubmitDcb", base + 96, 0, 0) == 0);
    CHECK(invoke("#kAPt2J4U3v8", base + 128, 0, 1) == 0);
    CHECK(value(tail) == 1101);
}
static void test_multi_acbs_execute_in_order_and_reject_invalid_batch(void) {
    guest_addr counter = base + 160, cmd = base + 4096, addrs = base + 256, sizes = base + 288;
    uint32_t zero = 0, counts[] = {11, 15};
    put(counter, &zero, 1);
    uint32_t first[11] = {0xc0017600, 0x207, 33};
    atomic_add(first + 3, counter);
    uint32_t second[15] = {0xc0053c00, 3, 0x2e07, 0, 33, ~0u, 4};
    atomic_add(second + 7, counter);
    put(cmd, first, 11);
    put(cmd + 128, second, 15);
    gmem_write_u64(&mem, addrs, cmd);
    gmem_write_u64(&mem, addrs + 8, cmd + 128);
    put(sizes, counts, 2);
    CHECK(invoke4("sceAgcDriverSubmitMultiAcbs", 35, addrs, sizes, 2) == 0);
    CHECK(invoke("#kAPt2J4U3v8", base + 128, 0, 1) == 0);
    CHECK(value(counter) == 2);
    /* Do not execute the valid first entry if the second is unmapped. */
    put(counter, &zero, 1);
    gmem_write_u64(&mem, addrs + 8, 0xdead000000ull);
    CHECK(invoke4("sceAgcDriverSubmitMultiAcbs", 35, addrs, sizes, 2) == SCE_KERNEL_ERROR_EINVAL);
    CHECK(invoke("#kAPt2J4U3v8", base + 128, 0, 1) == 0);
    CHECK(value(counter) == 0);
    CHECK(invoke4("sceAgcDriverSubmitMultiAcbs", 35, 0, 0, 0) == 0);
    CHECK(invoke4("sceAgcDriverSubmitMultiAcbs", 35, 0, 0, 1) == SCE_KERNEL_ERROR_EINVAL);
    CHECK(invoke4("sceAgcDriverSubmitMultiAcbs", 35, addrs, sizes, 4097) ==
          SCE_KERNEL_ERROR_EINVAL);
    gmem_write_u64(&mem, addrs, cmd + 1);
    CHECK(invoke4("sceAgcDriverSubmitMultiAcbs", 35, addrs, sizes, 1) == SCE_KERNEL_ERROR_EINVAL);
}
static void test_semaphore_wait_allows_other_queue_to_signal(void) {
    guest_addr counter=base+640,done=base+648,wait_cmd=base+4096,signal_cmd=base+4224;
    CHECK(gmem_write_u64(&mem,counter,0) && gmem_write_u64(&mem,done,0));
    uint32_t wait[12]={0xc0023900,(uint32_t)counter,(uint32_t)(counter>>32),0xe0000000};
    atomic_add(wait+4,done);
    uint32_t signal[]={0xc0023900,(uint32_t)counter,(uint32_t)(counter>>32),0xc0000000};
    put(wait_cmd,wait,12);put(signal_cmd,signal,4);
    descriptor(base+256,wait_cmd,12);descriptor(base+272,signal_cmd,4);
    CHECK(invoke("sceAgcDriverSubmitAcb",40,base+256,0)==0);
    pal_sleep_ns(20000000);
    bool parked=value(done)==0;
    /* Always submit the producer, so a failing assertion cannot strand the worker. */
    CHECK(invoke("sceAgcDriverSubmitAcb",41,base+272,0)==0);
    CHECK(invoke("#kAPt2J4U3v8",base+128,0,1)==0);
    /* The suspend point does not wait for a queue parked on a CP wait (suspend_ready), so it
     * can return after the signal but before the parked waiter's next retry. Wait for the
     * waiter itself; it must also finish before later tests reuse its command buffer. */
    CHECK(parked && wait_value(done,1) && value(counter)==0);
}
static void test_interrupt_registrations_grow_without_losing_notifications(void) {
    CHECK(equeue_init());
    uint64_t eq=equeue_create("many GPU events"),other=equeue_create("shared GPU event");
    CHECK(eq && other);
    for(uint32_t i=0;i<80;i++)
        CHECK(invoke("sceAgcDriverAddEqEvent",eq,32+i,0xa000+i)==0);
    CHECK(invoke("sceAgcDriverAddEqEvent",eq,111,0xb079)==0);
    CHECK(invoke("sceAgcDriverAddEqEvent",other,111,0xc079)==0);
    const guest_addr label=base+600,cmd=base+4096,packet=base+256;
    uint32_t release[]={0xc0064900,0x14,0x21010000,(uint32_t)label,(uint32_t)(label>>32),1,0,0x1234};
    put(cmd,release,8);descriptor(packet,cmd,8);
    CHECK(invoke("sceAgcDriverSubmitAcb",111,packet,0)==0);
    guest_kevent events[4];
    CHECK(equeue_wait(eq,events,4,3000000000ull)==1);
    CHECK(events[0].ident==111 && events[0].filter==EVFILT_GRAPHICS_ &&
          events[0].data==0x1234006f && events[0].udata==0xb079);
    CHECK(gmem_write(&mem,base+768,events,sizeof(events[0])));
    CHECK((uint32_t)invoke("sceAgcDriverGetEqContextId",base+768,0,0)==0x1234);
    CHECK((uint32_t)invoke("sceAgcDriverGetEqEventType",base+768,0,0)==0x1234006f);
    CHECK(equeue_wait(other,events,4,3000000000ull)==1 && events[0].ident==111 && events[0].udata==0xc079);
    CHECK(equeue_wait(eq,events,4,0)==0 && equeue_wait(other,events,4,0)==0);
    CHECK(invoke("sceAgcDriverDeleteEqEvent",eq,111,0)==0);
    release[7]=0x5678;put(cmd,release,8);
    CHECK(invoke("sceAgcDriverSubmitAcb",111,packet,0)==0);
    CHECK(equeue_wait(other,events,4,3000000000ull)==1 && events[0].data==0x5678006f);
    CHECK(equeue_wait(eq,events,4,0)==0);
    CHECK(invoke("sceAgcDriverAddEqEvent",eq,111,0xd079)==0);
    release[7]=0x07654321;put(cmd,release,8);
    CHECK(invoke("sceAgcDriverSubmitAcb",111,packet,0)==0);
    CHECK(equeue_wait(eq,events,4,3000000000ull)==1 && events[0].udata==0xd079 && events[0].data==INT64_C(0x07654321006f));
    CHECK(gmem_write(&mem,base+768,events,sizeof(events[0])));
    CHECK((uint32_t)invoke("sceAgcDriverGetEqContextId",base+768,0,0)==0x07654321);
    CHECK((uint32_t)invoke("sceAgcDriverGetEqEventType",base+768,0,0)==0x4321006f);
    CHECK(equeue_wait(other,events,4,3000000000ull)==1 && events[0].data==INT64_C(0x07654321006f));
    for(uint32_t i=0;i<80;i++)CHECK(invoke("sceAgcDriverDeleteEqEvent",eq,32+i,0)==0);
    CHECK(invoke("sceAgcDriverDeleteEqEvent",other,111,0)==0);
    CHECK(equeue_destroy(eq) && equeue_destroy(other));
}
static void test_eq_context_uses_packed_data_not_ident(void) {
    guest_kevent event={.ident=55,.filter=EVFILT_GRAPHICS_,
                       .data=(int64_t)((UINT64_C(0x01234567)<<16)|111)};
    CHECK(gmem_write(&mem,base+768,&event,sizeof(event)));
    CHECK((uint32_t)invoke("sceAgcDriverGetEqContextId",base+768,0,0)==0x01234567);
}
static void test_eq_type_uses_low_data_word_not_ident(void) {
    guest_kevent event={.ident=55,.filter=EVFILT_GRAPHICS_,
                       .data=(int64_t)((UINT64_C(0x01234567)<<16)|111)};
    CHECK(gmem_write(&mem,base+768,&event,sizeof(event)));
    CHECK((uint32_t)invoke("sceAgcDriverGetEqEventType",base+768,0,0)==0x4567006f);
}
int main(int argc, char **argv) {
    pal_init();
    if (hle_registry_init() != PS5C_OK)
        return 2;
    if (argc > 1)
        settings_load(argv[1]);
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    if (gmem_init(&mem, &cfg) != PS5C_OK ||
        gmem_map(&mem, GUEST_MODULE_BASE, GUEST_PAGE_SIZE, GPROT_RW, GMAP_FIXED, "CP queues test",
                 &base) != PS5C_OK)
        return 2;
    const test_case cases[] = {
        TEST_CASE(test_cross_queue_release_and_completion_prefix),
        TEST_CASE(test_multi_acbs_execute_in_order_and_reject_invalid_batch),
        TEST_CASE(test_semaphore_wait_allows_other_queue_to_signal),
        TEST_CASE(test_eq_context_uses_packed_data_not_ident),
        TEST_CASE(test_eq_type_uses_low_data_word_not_ident),
        TEST_CASE(test_interrupt_registrations_grow_without_losing_notifications)};
    return test_run_all("agc_queues", cases, PS5C_ARRAY_COUNT(cases));
}
