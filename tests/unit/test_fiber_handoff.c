/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/runtime.h"
#include "hle/hle.h"
#include "loader/nid.h"
#include "pal/pal.h"

#define CHECK(x) do { if(!(x)){fprintf(stderr,"fiber handoff line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
static ps5_runtime *rt;
static guest_addr object,page;
static atomic_int inject,window,observed;
static int32_t finalize_result,run_result;
static uint64_t entered;

static int32_t call(const char *name,uint64_t a,uint64_t b,uint64_t d,uint64_t e,uint64_t f,uint64_t g) {
    const hle_export *exp=hle_find("libSceFiber",nid_hash_name(name));CHECK(exp && exp->fn);
    hle_args args={.gpr={a,b,d,e,f,g}};hle_call c={.rt=rt,.mem=&rt->mem,.a=&args};
    exp->fn(&c);return (int32_t)args.ret[0];
}

/* Pause at the real stack-switch boundary, after the HLE requested a yield
 * but before its context has been saved. A second host thread attempts to
 * finalize that still-executing fiber through the public API. */
void __real_x64_native_switch(x64_native *cpu,uint64_t *save,uint64_t resume,volatile uint32_t *done,uint32_t value);
void __wrap_x64_native_switch(x64_native *cpu,uint64_t *save,uint64_t resume,volatile uint32_t *done,uint32_t value);
void __wrap_x64_native_switch(x64_native *cpu,uint64_t *save,uint64_t resume,volatile uint32_t *done,uint32_t value) {
    if(atomic_load(&inject)==2) {
        atomic_store(&inject,0);atomic_store(&window,1);
        uint64_t deadline=pal_time_ns()+5000000000ull;
        while(!atomic_load(&observed) && pal_time_ns()<deadline)pal_thread_yield();
        CHECK(atomic_load(&observed));
    }
    __real_x64_native_switch(cpu,save,resume,done,value);
}
static int32_t finalize_while_switching(void *unused) {
    PS5C_UNUSED(unused);
    uint64_t deadline=pal_time_ns()+5000000000ull;
    while(!atomic_load(&window) && pal_time_ns()<deadline)pal_thread_yield();
    if(!atomic_load(&window)){atomic_store(&observed,1);return 1;}
    run_result=call("sceFiberRun",object,0,page+0x1900,0,0,0);
    finalize_result=call("sceFiberFinalize",object,0,0,0,0,0);
    atomic_store(&observed,1);return 0;
}
static void __attribute__((sysv_abi)) entry(uint64_t initial,uint64_t run) {
    entered=initial+run;
    if(atomic_load(&inject)==1)atomic_store(&inject,2);
    CHECK(call("sceFiberReturnToThread",entered,0,0,0,0,0)==0);
    /* The fixture intentionally leaves a parked context, never returning
     * through the guest boot glue's invalid-entry-return trap. */
    for(;;)CHECK(call("sceFiberReturnToThread",entered,0,0,0,0,0)==0);
}
static void unused_dispatch(hle_args *args){PS5C_UNUSED(args);abort();}
static void initialize_at(guest_addr at,guest_addr stack,void (__attribute__((sysv_abi)) *fn)(uint64_t,uint64_t),uint64_t initial) {
    uint64_t address=0;
    memcpy(&address,&fn,sizeof(address));
    CHECK(call("_sceFiberInitializeImpl",at,0,address,initial,stack,0x100000)==0);
}
static void initialize(void){initialize_at(object,page+0x10000,entry,17);}
static void __attribute__((sysv_abi)) exchange(uint64_t initial,uint64_t run) {
    register uint64_t preserved __asm__("r13")=0xfedcba9876543210ull+initial;
    __asm__ volatile("" : "+r"(preserved));
    CHECK((initial==100 && run==7000) || (initial==200 && run==1000));
    for(unsigned i=0;i<64;i++) {
        uint64_t self=0;
        CHECK(call("sceFiberGetSelf",page+0x2300,0,0,0,0,0)==0);
        CHECK(gmem_read(&rt->mem,page+0x2300,&self,8) && self==object+(initial==100?0:0x100));
        guest_addr target=object+(initial==100?0x100:0),out=page+(initial==100?0x2000:0x2100);
        CHECK(call("sceFiberSwitch",target,(initial==100?1000:2000)+i,out,0,0,0)==0);
        __asm__ volatile("" : "+r"(preserved));
        CHECK(preserved==0xfedcba9876543210ull+initial);
        uint64_t value=0;CHECK(gmem_read(&rt->mem,out,&value,8));
        CHECK(value==(initial==100?2000+i:1001+i));
    }
    CHECK(initial==100);
    CHECK(call("sceFiberReturnToThread",0xaabb,0,0,0,0,0)==0);
    abort();
}

static guest_thread migrate_a,migrate_b;
static const hle_stub_info marker_a={0},marker_b={0};
static uint64_t expected_migration;
/* Use real native stacks and host TLS. A parked HLE frame follows its fiber
 * between threads; its old c.thread and diagnostic markers must not. */
static int32_t call_tracked(const char *name,uint64_t a,uint64_t b,uint64_t d) {
    const hle_export *exp=hle_find("libSceFiber",nid_hash_name(name));CHECK(exp && exp->fn);
    hle_args args={.gpr={a,b,d}};
    guest_thread *before=runtime_current_thread(rt);CHECK(before);
    const hle_stub_info *prev=before->in_hle;
    guest_addr ret=before->hle_ret,fp=before->hle_fp;
    hle_call c={.rt=rt,.mem=&rt->mem,.a=&args,.thread=before};
    exp->fn(&c);
    guest_thread *now=runtime_current_thread(rt);CHECK(now);
    if(c.thread!=now) fprintf(stderr,"Migrated %s: stale thread %u, running thread %u\n",name,c.thread?c.thread->id:0,now->id);
    CHECK(c.thread==now);
    CHECK(now->in_hle==prev && now->hle_ret==ret && now->hle_fp==fp);
    return (int32_t)args.ret[0];
}
static void __attribute__((sysv_abi)) migrate_entry(uint64_t initial,uint64_t run) {
    CHECK(initial==17 && run==10);
    CHECK(runtime_current_thread(rt)==&migrate_a);
    CHECK(call_tracked("sceFiberReturnToThread",101,0,0)==0);
    CHECK(runtime_current_thread(rt)==&migrate_b);
    CHECK(migrate_a.in_hle==&marker_a);
    CHECK(call_tracked("sceFiberReturnToThread",202,0,0)==0);
    CHECK(runtime_current_thread(rt)==&migrate_a);
    CHECK(migrate_b.in_hle==&marker_b);
    CHECK(call_tracked("sceFiberReturnToThread",303,0,0)==0);
    abort();
}
static void __attribute__((sysv_abi)) migrate_switch_target(uint64_t initial,uint64_t run) {
    CHECK(initial==44 && run==55 && runtime_current_thread(rt)==&migrate_a);
    CHECK(call_tracked("sceFiberReturnToThread",101,0,0)==0);
    abort();
}
static void __attribute__((sysv_abi)) migrate_switch_from(uint64_t initial,uint64_t run) {
    CHECK(initial==33 && run==10 && runtime_current_thread(rt)==&migrate_a);
    CHECK(call_tracked("sceFiberSwitch",object+0x100,55,0)==0);
    CHECK(runtime_current_thread(rt)==&migrate_b && migrate_a.in_hle==&marker_a);
    CHECK(call_tracked("sceFiberReturnToThread",404,0,0)==0);
    abort();
}
static int32_t migrate_on_second_thread(void *unused) {
    PS5C_UNUSED(unused);
    pal_tls_set(rt->thread_slot,&migrate_b);
    CHECK(call_tracked("sceFiberRun",object,20,page+0x1000)==0);
    uint64_t result=0;CHECK(gmem_read(&rt->mem,page+0x1000,&result,8) && result==expected_migration);
    CHECK(migrate_a.in_hle==&marker_a && migrate_b.in_hle==&marker_b);
    CHECK(migrate_a.hle_ret==0xa001 && migrate_b.hle_ret==0xb001);
    CHECK(migrate_a.hle_fp==0xa002 && migrate_b.hle_fp==0xb002);
    pal_tls_set(rt->thread_slot,NULL);return 0;
}
static void migration_test(void) {
    CHECK(pal_tls_alloc(&rt->thread_slot)==PS5C_OK);
    migrate_a.id=101;migrate_b.id=202;migrate_a.in_hle=&marker_a;migrate_b.in_hle=&marker_b;
    migrate_a.hle_ret=0xa001;migrate_a.hle_fp=0xa002;migrate_b.hle_ret=0xb001;migrate_b.hle_fp=0xb002;
    pal_tls_set(rt->thread_slot,&migrate_a);
    initialize_at(object,page+0x10000,migrate_entry,17);
    CHECK(call_tracked("sceFiberRun",object,10,page+0x1000)==0);
    uint64_t result=0;CHECK(gmem_read(&rt->mem,page+0x1000,&result,8) && result==101);
    expected_migration=202;
    pal_thread *second=NULL;CHECK(pal_thread_create(&second,migrate_on_second_thread,NULL,"fiber-migration",0)==PS5C_OK);
    int32_t code=0;CHECK(pal_thread_join(second,&code)==PS5C_OK && code==0);
    CHECK(call_tracked("sceFiberRun",object,30,page+0x1000)==0);
    CHECK(gmem_read(&rt->mem,page+0x1000,&result,8) && result==303);
    CHECK(call("sceFiberFinalize",object,0,0,0,0,0)==0);
    /* Also resume a sceFiberSwitch frame on the other thread. Its target
     * returned to A, leaving the source parked for B to claim. */
    initialize_at(object,page+0x10000,migrate_switch_from,33);
    initialize_at(object+0x100,page+0x130000,migrate_switch_target,44);
    CHECK(call_tracked("sceFiberRun",object,10,page+0x1000)==0);
    CHECK(gmem_read(&rt->mem,page+0x1000,&result,8) && result==101);
    expected_migration=404;
    CHECK(pal_thread_create(&second,migrate_on_second_thread,NULL,"fiber-switch-migration",0)==PS5C_OK);
    CHECK(pal_thread_join(second,&code)==PS5C_OK && code==0);
    CHECK(call("sceFiberFinalize",object,0,0,0,0,0)==0);
    CHECK(call("sceFiberFinalize",object+0x100,0,0,0,0,0)==0);
    pal_tls_set(rt->thread_slot,NULL);pal_tls_free(rt->thread_slot);
    puts("Cross-thread fiber migration preserves current thread and HLE ownership");
}

int main(void) {
    CHECK(pal_init()==PS5C_OK && hle_registry_init()==PS5C_OK);
    rt=calloc(1,sizeof(*rt));CHECK(rt);
    gmem_config cfg=gmem_default_config();CHECK(gmem_init(&rt->mem,&cfg)==PS5C_OK);
    guest_addr glue;CHECK(gmem_map(&rt->mem,0,GUEST_PAGE_SIZE,GPROT_RW|GPROT_EXEC,0,"fiber glue",&glue)==PS5C_OK);
    CHECK(x64_native_init(&rt->cpu,&rt->mem,glue,unused_dispatch,8)==PS5C_OK);
    CHECK(gmem_map(&rt->mem,0,0x240000,GPROT_RW,0,"fiber fixture",&page)==PS5C_OK);object=page;
    initialize();entered=0;
    CHECK(call("sceFiberRun",object,23,page+0x1000,0,0,0)==0 && entered==40);
    uint64_t returned=0;CHECK(gmem_read(&rt->mem,page+0x1000,&returned,8) && returned==40);
    CHECK(call("sceFiberFinalize",object,0,0,0,0,0)==0);
    initialize();atomic_store(&inject,1);
    pal_thread *other=NULL;CHECK(pal_thread_create(&other,finalize_while_switching,NULL,"fiber-finalizer",0)==PS5C_OK);
    CHECK(call("sceFiberRun",object,31,page+0x1000,0,0,0)==0 && entered==48);
    int32_t result=0;CHECK(pal_thread_join(other,&result)==PS5C_OK && result==0);
    printf("Finalize before the outgoing stack was saved: 0x%08x (expected busy 0x80590006)\n",(unsigned)finalize_result);
    CHECK(finalize_result==(int32_t)0x80590006);
    CHECK(run_result==(int32_t)0x80590006);
    CHECK(call("sceFiberFinalize",object,0,0,0,0,0)==0);
    /* 128 real fiber-to-fiber hops, not just return-to-thread. Verify that
     * only the outgoing context becomes idle, and its saved GPR/arguments
     * survive until it is claimed by the next switch. */
    initialize_at(object,page+0x10000,exchange,100);
    initialize_at(object+0x100,page+0x130000,exchange,200);
    CHECK(call("sceFiberRun",object,7000,page+0x1000,0,0,0)==0);
    CHECK(gmem_read(&rt->mem,page+0x1000,&returned,8) && returned==0xaabb);
    CHECK(call("sceFiberFinalize",object,0,0,0,0,0)==0);
    CHECK(call("sceFiberFinalize",object+0x100,0,0,0,0,0)==0);
    migration_test();
    gmem_free(&rt->mem);free(rt);hle_registry_free();pal_shutdown();
    puts("Fiber: native arguments, result, real stack switch and concurrent handoff lifetime passed");return 0;
}
