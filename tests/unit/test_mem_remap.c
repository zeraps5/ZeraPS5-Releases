/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
#include <stdatomic.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <string.h>

#include "mem/gmem.h"
#include "gpu/gpu_watch.h"
#include "pal/pal.h"
#include "test.h"

/* Fail only allocations in the operation under test, using the real PAL and
 * gmem. A split can grow the VMA table with realloc, which is not intercepted. */
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);
static unsigned fail_malloc, fail_calloc, injected;

void *__wrap_malloc(size_t size) {
    if (fail_malloc && --fail_malloc == 0) { injected++; return NULL; }
    return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size) {
    if (fail_calloc && --fail_calloc == 0) { injected++; return NULL; }
    return __real_calloc(count, size);
}

static void test_private_partial_unmap_without_snapshots(void) {
    if (!pal_ph_supported()) { puts("    SKIP: host has no placeholders"); return; }
    for (unsigned scenario = 0; scenario < 3; scenario++) {
        gmem m;
        gmem_config cfg = gmem_default_config();
        CHECK(gmem_init(&m, &cfg) == PS5C_OK);
        guest_addr at = m.ph_start, mapped = 0;
        CHECK(gmem_map_flexible(&m,at,4*GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,GPROT_RW,GMAP_FIXED,
                                4*GUEST_PAGE_SIZE,"private",&mapped,NULL)==PS5C_OK);
        const uint64_t head = 0x123456789abcdef0ull, tail = 0xfedcba9876543210ull;
        CHECK(gmem_write_u64(&m, at, head));
        CHECK(gmem_write_u64(&m, at + 3 * GUEST_PAGE_SIZE, tail));
        CHECK(gmem_protect(&m, at, GUEST_PAGE_SIZE, GPROT_READ) == PS5C_OK);
        uint64_t before = m.mapped_bytes;
        injected = 0;
        fail_calloc = scenario == 0 ? 1 : 0;
        fail_malloc = scenario;
        ps5c_err err = gmem_unmap(&m, at + GUEST_PAGE_SIZE, GUEST_PAGE_SIZE);
        fail_malloc = fail_calloc = 0;
        uint64_t h = 0, t = 0;
        const gmem_vma *v = gmem_find_vma(&m, at);
        bool readable = gmem_read_u64(&m, at, &h) &&
            gmem_read_u64(&m, at + 3 * GUEST_PAGE_SIZE, &t);
        const gmem_vma *tv=gmem_find_vma(&m,at+3*GUEST_PAGE_SIZE);
        bool preserved = injected == 0 && err == PS5C_OK && m.mapped_bytes == before-GUEST_PAGE_SIZE &&
            gmem_flexible_used(&m)==3*GUEST_PAGE_SIZE &&
            !gmem_is_mapped(&m,at+GUEST_PAGE_SIZE,1) && v && v->prot == GPROT_READ &&
            v->os_base==at && v->os_size==GUEST_PAGE_SIZE && tv && tv->prot==GPROT_RW &&
            tv->os_base==at+2*GUEST_PAGE_SIZE && tv->os_size==2*GUEST_PAGE_SIZE &&
            readable && h == head && t == tail;
        if (!preserved)
            printf("    scenario %u: injected %u, result %d, bytes %llu/%llu, head %llx, tail %llx\n",
                   scenario, injected, (int)err, (unsigned long long)m.mapped_bytes,
                   (unsigned long long)before, (unsigned long long)h, (unsigned long long)t);
        bool retry = false;
        if (preserved) {
            retry = gmem_map_flexible(&m,at+GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,GPROT_RW,
                                     GMAP_FIXED,4*GUEST_PAGE_SIZE,"hole reuse",&mapped,NULL)==PS5C_OK &&
                gmem_is_mapped(&m,at,4*GUEST_PAGE_SIZE) && m.mapped_bytes==before &&
                gmem_flexible_used(&m)==4*GUEST_PAGE_SIZE &&
                gmem_read_u64(&m, at, &h) && h == head &&
                gmem_read_u64(&m, at + 3 * GUEST_PAGE_SIZE, &t) && t == tail;
            bool shared=false;
            retry = retry && gmem_map_direct(&m,at+GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,GPROT_RW,
                                             GMAP_FIXED|GMAP_REPLACE,0,"hole becomes view",&mapped,&shared)==PS5C_OK && shared &&
                gmem_read_u64(&m,at,&h) && h==head &&
                gmem_read_u64(&m,at+3*GUEST_PAGE_SIZE,&t) && t==tail;
        }
        gmem_free(&m);
        CHECK(preserved);
        CHECK(retry);
    }
}

static void test_private_full_unmap_needs_no_snapshot(void) {
    if (!pal_ph_supported()) { puts("    SKIP: host has no placeholders"); return; }
    gmem m;
    gmem_config cfg = gmem_default_config();
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr at = m.ph_start, mapped = 0;
    CHECK(gmem_map(&m, at, 4 * GUEST_PAGE_SIZE, GPROT_RW, GMAP_FIXED, "private", &mapped) == PS5C_OK);
    injected = 0; fail_malloc = fail_calloc = 1;
    ps5c_err err = gmem_unmap(&m, at, 4 * GUEST_PAGE_SIZE);
    fail_malloc = fail_calloc = 0;
    bool ok = err == PS5C_OK && injected == 0 && !gmem_is_mapped(&m, at, 1) && m.mapped_bytes == 0;
    gmem_free(&m);
    CHECK(ok);
}

static void test_shared_partial_unmap_keeps_alias_bytes(void) {
    if (!pal_ph_supported()) { puts("    SKIP: host has no placeholders"); return; }
    gmem m;
    gmem_config cfg = gmem_default_config();
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a = m.ph_start, b = a + 4 * GUEST_PAGE_SIZE, out = 0;
    bool shared = false;
    CHECK(gmem_map_direct(&m, a, 4 * GUEST_PAGE_SIZE, GUEST_PAGE_SIZE, GPROT_RW,
                         GMAP_FIXED, 0, "view a", &out, &shared) == PS5C_OK && shared);
    CHECK(gmem_map_direct(&m, b, 4 * GUEST_PAGE_SIZE, GUEST_PAGE_SIZE, GPROT_RW,
                         GMAP_FIXED, 0, "view b", &out, &shared) == PS5C_OK && shared);
    CHECK(gmem_write_u64(&m, a, 0x1122334455667788ull));
    CHECK(gmem_write_u64(&m, a + 3 * GUEST_PAGE_SIZE, 0x8877665544332211ull));
    injected = 0; fail_malloc = fail_calloc = 1;
    ps5c_err err = gmem_unmap(&m, a + GUEST_PAGE_SIZE, GUEST_PAGE_SIZE);
    fail_malloc = fail_calloc = 0;
    uint64_t h = 0, t = 0;
    bool ok = err == PS5C_OK && injected == 0 && m.mapped_bytes == 7 * GUEST_PAGE_SIZE &&
        !gmem_is_mapped(&m, a + GUEST_PAGE_SIZE, 1) &&
        gmem_read_u64(&m, a, &h) && h == 0x1122334455667788ull &&
        gmem_read_u64(&m, a + 3 * GUEST_PAGE_SIZE, &t) && t == 0x8877665544332211ull &&
        gmem_read_u64(&m, b, &h) && h == 0x1122334455667788ull &&
        gmem_read_u64(&m, b + 3 * GUEST_PAGE_SIZE, &t) && t == 0x8877665544332211ull;
    gmem_free(&m);
    CHECK(ok);
}

static void test_shared_protection_splits_survive_unmaps(void) {
    if(!pal_ph_supported())return;
    gmem m;gmem_config cfg=gmem_default_config();CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr a=m.ph_start,b=a+32*GUEST_PAGE_SIZE,out=0;
    bool shared=false;
    CHECK(gmem_map_direct(&m,a,16*GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,GPROT_RW,
        GMAP_FIXED,0,"split view",&out,&shared)==PS5C_OK && shared);
    CHECK(gmem_map_direct(&m,b,16*GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,GPROT_RW,
        GMAP_FIXED,0,"whole alias",&out,&shared)==PS5C_OK && shared);
    for(unsigned i=0;i<16;i++) {
        CHECK(gmem_write_u64(&m,a+i*GUEST_PAGE_SIZE,0x1122334400000000ull+i));
        if(i&1)CHECK(gmem_protect(&m,a+i*GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,GPROT_READ)==PS5C_OK);
    }
    /* Each protection creates a VMA, but the OS mapping initially remains
     * one unit. Remove both ends and a middle page in separate operations. */
    bool ok=gmem_unmap(&m,a,2*GUEST_PAGE_SIZE)==PS5C_OK &&
        gmem_unmap(&m,a+14*GUEST_PAGE_SIZE,2*GUEST_PAGE_SIZE)==PS5C_OK &&
        gmem_unmap(&m,a+7*GUEST_PAGE_SIZE,GUEST_PAGE_SIZE)==PS5C_OK;
    for(unsigned i=0;i<16;i++) {
        uint64_t v=0;ok &= gmem_read_u64(&m,b+i*GUEST_PAGE_SIZE,&v) && v==0x1122334400000000ull+i;
        if(i>=2 && i<14 && i!=7) {
            ok &= gmem_read_u64(&m,a+i*GUEST_PAGE_SIZE,&v) && v==0x1122334400000000ull+i;
            const gmem_vma *piece=gmem_find_vma(&m,a+i*GUEST_PAGE_SIZE);
            ok &= piece && piece->prot==((i&1)?GPROT_READ:GPROT_RW);
        } else ok &= !gmem_is_mapped(&m,a+i*GUEST_PAGE_SIZE,1);
    }
    gmem_free(&m);CHECK(ok);
}

static void test_shared_large_tail_remap(void) {
    if(!pal_ph_supported())return;
    gmem m;gmem_config cfg=gmem_default_config();CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr a=m.ph_start,b=a+(128ull<<20),out=0;const uint64_t size=64ull<<20;
    bool shared=false;
    CHECK(gmem_map_direct(&m,a,size,GUEST_PAGE_SIZE,GPROT_RW,GMAP_FIXED,0,"large tail",&out,&shared)==PS5C_OK && shared);
    CHECK(gmem_map_direct(&m,b,GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,GPROT_RW,GMAP_FIXED,size-GUEST_PAGE_SIZE,"tail alias",&out,&shared)==PS5C_OK && shared);
    CHECK(gmem_write_u64(&m,a+size-GUEST_PAGE_SIZE,0x1122334455667788ull));
    /* The title repeatedly protects pieces with the same logical protection,
     * leaving multiple adjacent VMAs before a long final surviving tail. */
    for(unsigned i=10;i<32;i+=2)CHECK(gmem_protect(&m,a+((uint64_t)i<<20),2ull<<20,GPROT_RW)==PS5C_OK);
    bool ok=gmem_unmap(&m,a+(5ull<<20),1ull<<20)==PS5C_OK;
    uint64_t v=0;ok &= gmem_read_u64(&m,a+size-GUEST_PAGE_SIZE,&v) && v==0x1122334455667788ull;
    ok &= gmem_read_u64(&m,b,&v) && v==0x1122334455667788ull;
    gmem_free(&m);CHECK(ok);
}

/* A second live alias can hide loss of section contents when the only view
 * is removed and reconstructed. Exercise a large pool with no such alias,
 * then introduce one after all replacements to compare physical contents. */
static void test_shared_large_pool_replacements_without_alias(void) {
    if(!pal_ph_supported())return;
    gmem m;gmem_config cfg=gmem_default_config();CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    const uint64_t pool_size=UINT64_C(0x108200000),physical=UINT64_C(0x65e00000);
    guest_addr base=m.ph_start,out=0;bool shared=false;
    CHECK(gmem_map_direct(&m,base,pool_size,GUEST_PAGE_SIZE,GPROT_RW,GMAP_FIXED,
                           0,"single large pool",&out,&shared)==PS5C_OK && shared);
    const size_t bytes=1u<<20;
    uint8_t *expected=malloc(bytes),*actual=malloc(bytes);
    if(!expected || !actual){free(expected);free(actual);gmem_free(&m);CHECK(false);}
    uint32_t rng=0x36af10e7;
    for(size_t i=0;i<bytes;i++){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;expected[i]=(uint8_t)rng;}
    CHECK(gmem_write(&m,base+physical,expected,bytes));
    for(unsigned i=0;i<12;i++)
        CHECK(gmem_protect(&m,base+UINT64_C(0x40000000)+(uint64_t)i*(8u<<20),2u<<20,GPROT_RW)==PS5C_OK);
#ifdef _WIN32
    /* Evict this test process's resident pages, then do not read the target
     * until all replacements finish. A per-iteration check would keep the
     * very pages whose lifetime is under test hot in the working set. */
    if(!SetProcessWorkingSetSize(GetCurrentProcess(),(SIZE_T)-1,(SIZE_T)-1))
        printf("    working-set trim unavailable: %lu\n",(unsigned long)GetLastError());
#endif
    bool ok=true;
    for(unsigned i=0;i<96;i++) {
        guest_addr at=base+UINT64_C(0x49000000)+(uint64_t)i*(1u<<20);
        if(gmem_map_direct(&m,at,1u<<20,GUEST_PAGE_SIZE,GPROT_RW,GMAP_FIXED|GMAP_REPLACE,
                            (8ull<<30)+(uint64_t)i*(1u<<20),"replacement",&out,&shared)!=PS5C_OK || !shared) {
            printf("    pool replacement %u failed\n",i);ok=false;break;
        }
    }
    ok=ok && gmem_read(&m,base+physical,actual,bytes) && !memcmp(actual,expected,bytes);
    if(!ok)puts("    cold pool contents were not preserved");
    guest_addr alias=base+(8ull<<30);
    bool alias_ok=gmem_map_direct(&m,alias,bytes,GUEST_PAGE_SIZE,GPROT_RW,GMAP_FIXED,
                                  physical,"late alias",&out,&shared)==PS5C_OK && shared &&
        gmem_read(&m,alias,actual,bytes) && !memcmp(actual,expected,bytes);
    if(!alias_ok)puts("    physical contents also missing through the late alias");
    free(expected);free(actual);gmem_free(&m);CHECK(ok && alias_ok);
}

typedef struct concurrent_view {
    gmem *mem;
    guest_addr at;
    atomic_bool stop, ready, bad;
    atomic_uint faults;
    uint64_t increments;
} concurrent_view;

static bool retry_native_view(void *arg, pal_fault_info *info) {
    concurrent_view *v=arg;
    if(info->kind!=PAL_FAULT_ACCESS || info->addr<v->at || info->addr>=v->at+24)return false;
    bool ok=gmem_retry_fault(v->mem,info->addr,info->write);
    if(ok)atomic_fetch_add(&v->faults,1);
    return ok;
}

static int32_t access_shared_view(void *arg) {
    concurrent_view *v=arg;
    volatile uint64_t *words=(volatile uint64_t *)(uintptr_t)v->at;
    atomic_store(&v->ready,true);
    uint64_t count=0;
    while(!atomic_load(&v->stop)) {
        __atomic_fetch_add((uint64_t *)(uintptr_t)v->at,1,__ATOMIC_SEQ_CST);
        count++;
        if(words[1]!=UINT64_C(0x123456789abcdef0) || words[2]!=UINT64_C(0xfedcba9876543210))
            atomic_store(&v->bad,true);
    }
    v->increments=count;
    return 0;
}

/* A private snapshot loses native writes made after its copy was taken, even
 * if transient unmapped accesses are retried correctly. The writer's page is
 * never part of a replacement; every increment and sentinel must survive. */
static void test_private_remap_with_native_cpu_access(void) {
    if(!pal_ph_supported())return;
    gmem m;gmem_config cfg=gmem_default_config();CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr base=m.ph_start,out=0;uint64_t bytes=8u<<20;
    CHECK(gmem_map(&m,base,bytes,GPROT_RW,GMAP_FIXED,"concurrent private",&out)==PS5C_OK);
    concurrent_view v={.mem=&m,.at=base+bytes-GUEST_PAGE_SIZE};
    atomic_init(&v.stop,false);atomic_init(&v.ready,false);atomic_init(&v.bad,false);atomic_init(&v.faults,0);
    CHECK(gmem_write_u64(&m,v.at,0) && gmem_write_u64(&m,v.at+8,UINT64_C(0x123456789abcdef0)) &&
          gmem_write_u64(&m,v.at+16,UINT64_C(0xfedcba9876543210)));
    CHECK(pal_fault_handler_install(retry_native_view,&v)==PS5C_OK);
    pal_thread *thread=NULL;bool ok=pal_thread_create(&thread,access_shared_view,&v,"native private access",0)==PS5C_OK;
    if(ok) {
        while(!atomic_load(&v.ready) || __atomic_load_n((uint64_t *)(uintptr_t)v.at,__ATOMIC_SEQ_CST)<10000)
            pal_thread_yield();
        for(unsigned i=0;i<96;i++) {
            guest_addr at=base+(2*i+1)*GUEST_PAGE_SIZE;
            if(gmem_map(&m,at,GUEST_PAGE_SIZE,GPROT_RW,GMAP_FIXED|GMAP_REPLACE,"private replacement",&out)!=PS5C_OK) {
                ok=false;break;
            }
        }
        atomic_store(&v.stop,true);
        if(pal_thread_join(thread,NULL)!=PS5C_OK)ok=false;
    }
    pal_fault_handler_install(NULL,NULL);
    uint64_t actual=0;
    bool readable=gmem_read_u64(&m,v.at,&actual);
    ok=ok && !atomic_load(&v.bad) && !atomic_load(&v.faults) && v.increments>=10000 &&
        readable && actual==v.increments;
    printf("    native private: %llu increments, %u remap faults, stored %llu\n",
           (unsigned long long)v.increments,atomic_load(&v.faults),(unsigned long long)actual);
    gmem_free(&m);CHECK(ok);
}

/* The native CPU does not take the map lock for every memory instruction.
 * Reads and atomic writes in an untouched physical page must survive while
 * the containing host view is removed to replace a different subrange. */
static void test_shared_remap_with_native_cpu_access(void) {
    if(!pal_ph_supported())return;
    gmem m;gmem_config cfg=gmem_default_config();CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr base=m.ph_start,out=0;bool shared=false;
    CHECK(gmem_map_direct(&m,base,2ull<<30,GUEST_PAGE_SIZE,GPROT_RW,GMAP_FIXED,
                         0,"concurrent view",&out,&shared)==PS5C_OK && shared);
    concurrent_view v={.mem=&m,.at=base+UINT64_C(0x65e625f0)};
    atomic_init(&v.stop,false);atomic_init(&v.ready,false);atomic_init(&v.bad,false);atomic_init(&v.faults,0);
    CHECK(gmem_write_u64(&m,v.at,0) && gmem_write_u64(&m,v.at+8,UINT64_C(0x123456789abcdef0)) &&
          gmem_write_u64(&m,v.at+16,UINT64_C(0xfedcba9876543210)));
    CHECK(pal_fault_handler_install(retry_native_view,&v)==PS5C_OK);
    pal_thread *thread=NULL;bool ok=pal_thread_create(&thread,access_shared_view,&v,"native remap access",0)==PS5C_OK;
    if(ok) {
        while(!atomic_load(&v.ready))pal_thread_yield();
        for(unsigned i=0;i<96;i++) {
            guest_addr at=base+UINT64_C(0x49000000)+(uint64_t)i*(1u<<20);
            if(gmem_map_direct(&m,at,1u<<20,GUEST_PAGE_SIZE,GPROT_RW,GMAP_FIXED|GMAP_REPLACE,
                              (8ull<<30)+(uint64_t)i*(1u<<20),"replacement",&out,&shared)!=PS5C_OK || !shared) {
                ok=false;break;
            }
        }
        atomic_store(&v.stop,true);
        if(pal_thread_join(thread,NULL)!=PS5C_OK)ok=false;
    }
    pal_fault_handler_install(NULL,NULL);
    uint64_t actual=0;
    ok=ok && !atomic_load(&v.bad) && v.increments>0 && gmem_read_u64(&m,v.at,&actual) && actual==v.increments;
    printf("    native CPU: %llu increments, %u remap faults, stored %llu\n",
           (unsigned long long)v.increments,atomic_load(&v.faults),(unsigned long long)actual);
    gmem_free(&m);CHECK(ok);
}

/* Actual GTA-like fixed addresses and a physical subrange, through real PAL.
 * The second view is only 16 KiB and its physical offset is not 64 KiB aligned. */
static void test_low_fixed_physical_views(void) {
    const char *low = getenv("PS5C_LOW_SHARED_AREA");
    if (!pal_ph_supported() || !low || strcmp(low, "1") != 0) return;
    gmem m;
    gmem_config cfg = gmem_default_config();
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    CHECK(m.ph_start == 0x1000000000ull);
    guest_addr a = 0x14d5640000ull, b = 0x14e6380000ull, out = 0;
    uint64_t phys = 0xdb6c0000ull, value = 0;
    bool shared = false;
    CHECK(gmem_map_direct(&m, a, 4 * GUEST_PAGE_SIZE, GUEST_PAGE_SIZE, GPROT_RW,
                         GMAP_FIXED, phys, "GTA pool", &out, &shared) == PS5C_OK && shared && out == a);
    CHECK(gmem_map_direct(&m, b, GUEST_PAGE_SIZE, GUEST_PAGE_SIZE, GPROT_RW,
                         GMAP_FIXED, phys + 3 * GUEST_PAGE_SIZE, "GTA alias", &out, &shared) == PS5C_OK && shared && out == b);
    CHECK(gmem_write_u64(&m, a + 3 * GUEST_PAGE_SIZE, 0x1122334455667788ull));
    CHECK(gmem_read_u64(&m, b, &value) && value == 0x1122334455667788ull);
    CHECK(gmem_write_u64(&m, b, 0x8877665544332211ull));
    CHECK(gmem_read_u64(&m, a + 3 * GUEST_PAGE_SIZE, &value) && value == 0x8877665544332211ull);
    CHECK(gmem_unmap(&m, b, GUEST_PAGE_SIZE) == PS5C_OK);
    CHECK(gmem_read_u64(&m, a + 3 * GUEST_PAGE_SIZE, &value) && value == 0x8877665544332211ull);
    CHECK(gmem_map_direct(&m, b, GUEST_PAGE_SIZE, GUEST_PAGE_SIZE, GPROT_RW,
                         GMAP_FIXED, phys + 3 * GUEST_PAGE_SIZE, "GTA remap", &out, &shared) == PS5C_OK && shared);
    CHECK(gmem_read_u64(&m, b, &value) && value == 0x8877665544332211ull);
    CHECK(gmem_map_direct(&m, 0, GUEST_PAGE_SIZE, GUEST_PAGE_SIZE, GPROT_RW,
                         GMAP_SHARED_AREA, 0, "usual placement", &out, &shared) == PS5C_OK && shared);
    CHECK(out == GUEST_PH_BASE);
    gmem_free(&m);
}

/* The COW tracker's physical window is an ordinary host mapping, outside the
 * guest reservation. Closing the section handle alone does not release it. */
static void test_physical_window_freed(void) {
#ifdef _WIN32
    if (!pal_ph_supported()) return;
    gmem m;gmem_config cfg=gmem_default_config();
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr out=0;bool shared=false;
    CHECK(gmem_map_direct(&m,m.ph_start,GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,GPROT_RW,
                         GMAP_FIXED,0,"physical window lifecycle",&out,&shared)==PS5C_OK && shared);
    uint8_t *window=gmem_phys_window(&m);CHECK(window);
    MEMORY_BASIC_INFORMATION mi;
    CHECK(VirtualQuery(window,&mi,sizeof(mi))==sizeof(mi) && mi.AllocationBase==window && mi.Type==MEM_MAPPED);
    gmem_free(&m);
    CHECK(VirtualQuery(window,&mi,sizeof(mi))==sizeof(mi) && mi.State==MEM_FREE);
#endif
}

/* Default tracking must not turn live neighboring pages into snapshots that
 * can overwrite native writes during an unrelated partial unmap. */
static void test_default_tracking_native_coherence(void) {
#ifdef _WIN32
    if (!pal_ph_supported()) return;
    /* Explicit reproducer for the retained experiment; normal CTest uses the
     * product default. With PS5C_TEST_UNSAFE_COW=1 this must detect lost writes. */
    _putenv(getenv("PS5C_TEST_UNSAFE_COW")?"PS5C_GPU_WATCH=cow":"PS5C_GPU_WATCH=");
    _putenv("PS5C_GPU_WATCH_SWEEP=0");
    gmem m;gmem_config cfg=gmem_default_config();
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr base=m.ph_start,out=0;uint64_t bytes=8u<<20;bool shared=false;
    CHECK(gmem_map_direct(&m,base,bytes,GUEST_PAGE_SIZE,GPROT_RW,GMAP_FIXED,0,
                         "default tracking pool",&out,&shared)==PS5C_OK && shared);
    gmem_set_change_hook(gpu_watch_mapping_changed);
    (void)gpu_watch_enabled(); /* initialize the selected backend before start */
    gpu_watch_start(&m);
    concurrent_view v={.mem=&m,.at=base+bytes-GUEST_PAGE_SIZE};
    atomic_init(&v.stop,false);atomic_init(&v.ready,false);atomic_init(&v.bad,false);atomic_init(&v.faults,0);
    CHECK(gmem_write_u64(&m,v.at,0) && gmem_write_u64(&m,v.at+8,UINT64_C(0x123456789abcdef0)) &&
          gmem_write_u64(&m,v.at+16,UINT64_C(0xfedcba9876543210)));
    _Atomic uint64_t *counter=gmem_host(&m,v.at,8);CHECK(counter);
    CHECK(pal_fault_handler_install(retry_native_view,&v)==PS5C_OK);
    pal_thread *thread=NULL;bool ok=pal_thread_create(&thread,access_shared_view,&v,"default tracking writer",0)==PS5C_OK;
    if(ok) {
        while(!atomic_load(&v.ready))pal_thread_yield();
        for(unsigned i=0;i<96;i++) {
            uint64_t before=atomic_load(counter);
            /* An immediate native write can make an arm return false; the
             * tracker may still own a private page that must survive unmap. */
            (void)gpu_watch_arm(GPU_WATCH_BUFFERS,v.at,GUEST_PAGE_SIZE);
            while(atomic_load(counter)<before+1000)pal_thread_yield();
            guest_addr at=base+(uint64_t)i*GUEST_PAGE_SIZE;
            if(gmem_unmap(&m,at,GUEST_PAGE_SIZE)!=PS5C_OK ||
               gmem_map_direct(&m,at,GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,GPROT_RW,GMAP_FIXED,
                               (uint64_t)i*GUEST_PAGE_SIZE,"neighbor reuse",&out,&shared)!=PS5C_OK || !shared) {
                ok=false;break;
            }
        }
        atomic_store(&v.stop,true);
        if(pal_thread_join(thread,NULL)!=PS5C_OK)ok=false;
    }
    pal_fault_handler_install(NULL,NULL);
    uint64_t actual=atomic_load(counter);
    ok=ok && !atomic_load(&v.bad) && v.increments>=96000 && actual==v.increments;
    printf("    default tracking: %llu increments, stored %llu, %u transient view faults\n",
           (unsigned long long)v.increments,(unsigned long long)actual,atomic_load(&v.faults));
    gmem_set_change_hook(NULL);gmem_set_cow_hooks(NULL,NULL);gmem_free(&m);CHECK(ok);
#endif
}

int main(void) {
    pal_init();
    log_set_level_all(LOG_LVL_ERROR);
    /* Exercise adopting the early reservation with the experimental split. */
    if (pal_ph_supported() && gmem_reserve_early() != PS5C_OK) {
        puts("cannot reserve guest memory early");
        return 1;
    }
    static const test_case tests[] = {
        TEST_CASE(test_private_partial_unmap_without_snapshots),
        TEST_CASE(test_private_full_unmap_needs_no_snapshot),
        TEST_CASE(test_shared_partial_unmap_keeps_alias_bytes),
        TEST_CASE(test_shared_protection_splits_survive_unmaps),
        TEST_CASE(test_shared_large_tail_remap),
        TEST_CASE(test_shared_large_pool_replacements_without_alias),
        TEST_CASE(test_shared_remap_with_native_cpu_access),
        TEST_CASE(test_private_remap_with_native_cpu_access),
        TEST_CASE(test_low_fixed_physical_views),
        TEST_CASE(test_physical_window_freed),
        TEST_CASE(test_default_tracking_native_coherence),
    };
    return test_run_all("mem_remap", tests, PS5C_ARRAY_COUNT(tests));
}
