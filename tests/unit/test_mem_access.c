/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdatomic.h>
#include <stdio.h>
#include "mem/gmem_access.h"
#include "pal/pal.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line%d: %s\n",__LINE__,#x); return 1; } } while (0)

typedef struct mutation {
    gmem *mem;
    guest_addr addr;
    ps5c_err result;
    atomic_bool done;
} mutation;

static int32_t unmap(void *arg) {
    mutation *w = arg;
    w->result = gmem_unmap(w->mem,w->addr,GUEST_PAGE_SIZE);
    atomic_store(&w->done,true);
    return 0;
}

static bool writer_waits(gmem *mem) {
    uint64_t until = pal_time_ns()+2000000000ull;
    do {
        pal_mutex_lock(mem->lock);
        bool waiting = mem->mapping_writers != 0;
        pal_mutex_unlock(mem->lock);
        if (waiting) return true;
        pal_thread_yield();
    } while (pal_time_ns()<until);
    return false;
}

int main(void) {
    CHECK(pal_init()==PS5C_OK);
    gmem mem;
    gmem_config cfg={.start=0x200000000ull,.size=16*GUEST_PAGE_SIZE,.identity=false};
    CHECK(gmem_init(&mem,&cfg)==PS5C_OK);
    gmem_access a={0};
    guest_addr at;
    CHECK(!gmem_retain_mappings(&mem));
    CHECK(gmem_map(&mem,0,2*GUEST_PAGE_SIZE,GPROT_RW,0,"scope",&at)==PS5C_OK);
    CHECK(gmem_write_u64(&mem,at,0x1122334455667788ull));
    CHECK(!gmem_access_host(&a,at,8));
    CHECK(gmem_access_begin(&a,&mem,false));
    uint64_t first_generation=a.generation;
    uint64_t *p=gmem_access_host(&a,at,8);
    CHECK(p && *p==0x1122334455667788ull);
    CHECK(gmem_access_host(&a,at+GUEST_PAGE_SIZE,8) && a.count==1);
    mutation w={.mem=&mem,.addr=at};
    atomic_init(&w.done,false);
    pal_thread *thread;
    CHECK(pal_thread_create(&thread,unmap,&w,"access unmap",0)==PS5C_OK);
    CHECK(writer_waits(&mem) && !atomic_load(&w.done));
    /* A nested device import may retain the active CPU pin even with a writer
     * queued; acquiring another fair pin here would deadlock the owner. */
    CHECK(gmem_retain_mappings(&mem));
    CHECK(*p==0x1122334455667788ull && gmem_is_mapped(&mem,at,8));
    gmem_unpin_mappings(&mem);
    CHECK(!atomic_load(&w.done));
    gmem_access_end(&a);
    CHECK(pal_thread_join(thread,NULL)==PS5C_OK && w.result==PS5C_OK);
    CHECK(gmem_access_begin(&a,&mem,false) && a.generation!=first_generation);
    CHECK(a.count==0 && !gmem_access_host(&a,at,8));
    CHECK(gmem_access_host(&a,at+GUEST_PAGE_SIZE,8));
    gmem_access_end(&a);
    CHECK(!gmem_access_host(&a,at+GUEST_PAGE_SIZE,8));
    CHECK(gmem_map(&mem,at,GUEST_PAGE_SIZE,GPROT_RW,GMAP_FIXED,"replacement",&at)==PS5C_OK);
    CHECK(gmem_write_u64(&mem,at,0x9988776655443322ull));
    CHECK(gmem_access_begin(&a,&mem,false));
    p=gmem_access_host(&a,at,8);CHECK(p && *p==0x9988776655443322ull);
    uint64_t generation=a.generation;
    gmem_access_end(&a);
    CHECK(gmem_protect(&mem,at,GUEST_PAGE_SIZE,GPROT_READ)==PS5C_OK);
    CHECK(gmem_access_begin(&a,&mem,false) && a.generation!=generation);
    p=gmem_access_host(&a,at,8);CHECK(p && *p==0x9988776655443322ull);
    CHECK(!gmem_access_host(&a,UINT64_MAX-3,8));
    gmem_access_end(&a);
    guest_addr last=cfg.start+cfg.size-GUEST_PAGE_SIZE;
    CHECK(gmem_map(&mem,last,GUEST_PAGE_SIZE,GPROT_RW,GMAP_FIXED,"last page",&last)==PS5C_OK);
    CHECK(gmem_access_begin(&a,&mem,false));
    CHECK(gmem_access_host(&a,last,8));
    CHECK(gmem_access_host(&a,cfg.start+cfg.size-1,1));
    gmem_access_end(&a);
    /* An existing device pin can also lend a fresh CPU scope while a writer
     * waits. Both owners must release before the mapping may disappear. */
    CHECK(gmem_pin_mappings(&mem));
    mutation second={.mem=&mem,.addr=last};atomic_init(&second.done,false);
    CHECK(pal_thread_create(&thread,unmap,&second,"retained access",0)==PS5C_OK);
    CHECK(writer_waits(&mem));
    CHECK(gmem_access_begin(&a,&mem,true));
    CHECK(gmem_access_host(&a,last,8));
    gmem_access_end(&a);CHECK(!atomic_load(&second.done));
    gmem_unpin_mappings(&mem);
    CHECK(pal_thread_join(thread,NULL)==PS5C_OK && second.result==PS5C_OK);
    CHECK(mem.mapping_pins==0);
    gmem_free(&mem);pal_shutdown();
    puts("mapping access: scope lifetime, generation invalidation, remap/protect and nested import verified");
    return 0;
}
