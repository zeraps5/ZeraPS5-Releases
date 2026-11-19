/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hle/hle.h"
#include "hle/amm.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include "test.h"

#ifdef PS5C_TEST_MAP_COMMIT_FAILURE
ps5c_err __real_pal_vm_commit(void *addr, uint64_t size, uint32_t prot);
ps5c_err __wrap_pal_vm_commit(void *addr, uint64_t size, uint32_t prot);
static void *fail_commit_address;
static unsigned commit_failures;
ps5c_err __wrap_pal_vm_commit(void *addr, uint64_t size, uint32_t prot) {
    if (addr == fail_commit_address && size == GUEST_PAGE_SIZE) {
        fail_commit_address = NULL;
        commit_failures++;
        return PS5C_ERR_NOMEM;
    }
    return __real_pal_vm_commit(addr, size, prot);
}
#endif

static int32_t call6(gmem *mem, const char *name, uint64_t a0, uint64_t a1,
                    uint64_t a2, uint64_t a3,uint64_t a4,uint64_t a5) {
    const hle_export *exp = hle_find("libkernel", nid_hash_name(name));
    if (!exp || !exp->fn) return INT32_MIN;
    hle_args args = {.gpr = {a0, a1, a2, a3,a4,a5}};
    hle_call c = {.a = &args, .mem = mem};
    exp->fn(&c);
    return (int32_t)args.ret[0];
}

static int32_t call(gmem *mem,const char *name,uint64_t a0,uint64_t a1,uint64_t a2,uint64_t a3) {
    return call6(mem,name,a0,a1,a2,a3,0,0);
}

static void test_configured_flexible_budget(void) {
    CHECK(hle_registry_init() == PS5C_OK);
    gmem mem;
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    CHECK(gmem_init(&mem, &cfg) == PS5C_OK);
    guest_addr page = 0;
    CHECK(gmem_map(&mem, GUEST_MODULE_BASE + 16 * GUEST_PAGE_SIZE, GUEST_PAGE_SIZE,
                   GPROT_RW, GMAP_FIXED, "outputs", &page) == PS5C_OK);

    const uint64_t guard = 0xfeed1234abcdef01ull;
    CHECK(gmem_write_u64(&mem, page, guard));
    CHECK(gmem_write_u64(&mem, page + 16, guard));
    CHECK(call(&mem, "sceKernelConfiguredFlexibleMemorySize", page + 8, 0, 0, 0) == 0);
    uint64_t budget = 0, value = 0;
    CHECK(gmem_read_u64(&mem, page + 8, &budget) && budget == (448ull << 20));
    CHECK(gmem_read_u64(&mem, page, &value) && value == guard);
    CHECK(gmem_read_u64(&mem, page + 16, &value) && value == guard);
    CHECK(call(&mem, "sceKernelAvailableFlexibleMemorySize", page + 24, 0, 0, 0) == 0);
    CHECK(gmem_read_u64(&mem, page + 24, &value) && value == budget);

    guest_addr image;
    CHECK(gmem_map(&mem, 0, 2 * GUEST_PAGE_SIZE, GPROT_RW, 0, "executable", &image) == PS5C_OK);
    CHECK(gmem_protect_title_image(&mem, image, GUEST_PAGE_SIZE, GPROT_RX) == PS5C_OK);
    CHECK(gmem_protect_title_image(&mem, image + GUEST_PAGE_SIZE, GUEST_PAGE_SIZE, GPROT_NONE) == PS5C_OK);
    CHECK(gmem_protect(&mem, image, GUEST_PAGE_SIZE, GPROT_READ) == PS5C_OK);
    CHECK(call(&mem, "sceKernelAvailableFlexibleMemorySize", page + 24, 0, 0, 0) == 0);
    CHECK(gmem_read_u64(&mem, page + 24, &value) && value == budget - GUEST_PAGE_SIZE);
    CHECK(gmem_write_u64(&mem, page + 32, 0));
    CHECK(call(&mem, "sceKernelMapFlexibleMemory", page + 32, budget, 3, 0) == SCE_KERNEL_ERROR_ENOMEM);
    CHECK(gmem_unmap(&mem, image, 2 * GUEST_PAGE_SIZE) == PS5C_OK);
    CHECK(call(&mem, "sceKernelAvailableFlexibleMemorySize", page + 24, 0, 0, 0) == 0);
    CHECK(gmem_read_u64(&mem, page + 24, &value) && value == budget);

    /* Allocation reduces the available budget, never the configured total. */
    CHECK(gmem_write_u64(&mem, page + 32, 0));
    CHECK(call(&mem, "sceKernelMapFlexibleMemory", page + 32, GUEST_PAGE_SIZE, 3, 0) == 0);
    CHECK(call(&mem, "sceKernelAvailableFlexibleMemorySize", page + 24, 0, 0, 0) == 0);
    CHECK(gmem_read_u64(&mem, page + 24, &value) && value == budget - GUEST_PAGE_SIZE);
    CHECK(call(&mem, "sceKernelConfiguredFlexibleMemorySize", page + 8, 0, 0, 0) == 0);
    CHECK(gmem_read_u64(&mem, page + 8, &value) && value == budget);

    CHECK(call(&mem, "sceKernelConfiguredFlexibleMemorySize", 0, 0, 0, 0) == SCE_KERNEL_ERROR_EINVAL);
    CHECK(call(&mem, "sceKernelConfiguredFlexibleMemorySize", 0x1000, 0, 0, 0) == SCE_KERNEL_ERROR_EINVAL);
    /* Reject a partially mapped output without writing its first bytes. */
    uint32_t sentinel = 0x12345678, tail = 0;
    CHECK(gmem_write(&mem, page + GUEST_PAGE_SIZE - 4, &sentinel, sizeof(sentinel)));
    CHECK(call(&mem, "sceKernelConfiguredFlexibleMemorySize", page + GUEST_PAGE_SIZE - 4, 0, 0, 0)
          == SCE_KERNEL_ERROR_EINVAL);
    CHECK(gmem_read(&mem, page + GUEST_PAGE_SIZE - 4, &tail, sizeof(tail)) && tail == sentinel);
    gmem_free(&mem);
    hle_registry_free();
}

static void test_flexible_budget_reclaimed(void) {
    CHECK(hle_registry_init()==PS5C_OK);
    gmem mem;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&mem,&cfg)==PS5C_OK);
    guest_addr page;CHECK(gmem_map(&mem,GUEST_MODULE_BASE+16*GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,GPROT_RW,GMAP_FIXED,"outputs",&page)==PS5C_OK);
    const uint64_t budget=448ull<<20,P=GUEST_PAGE_SIZE;uint64_t available,at;
    CHECK(gmem_write_u64(&mem,page,0));
    CHECK(call(&mem,"sceKernelMapFlexibleMemory",page,3*P,3,0)==0);
    CHECK(gmem_read_u64(&mem,page,&at));
    CHECK(gmem_write_u64(&mem,at+2*P,0x123456789abcdef0ull));
    CHECK(call(&mem,"sceKernelMunmap",at+P,P,0,0)==0);
    CHECK(call(&mem,"sceKernelAvailableFlexibleMemorySize",page+8,0,0,0)==0);
    CHECK(gmem_read_u64(&mem,page+8,&available) && available==budget-2*P);
    CHECK(call(&mem,"sceKernelMunmap",at+1,P,0,0)==SCE_KERNEL_ERROR_EINVAL);
    hle_call context={.mem=&mem};
    CHECK(kernel_amm_map(&context,at,P,3,false,0)==0); /* replace flex head with plain memory */
    CHECK(call(&mem,"sceKernelAvailableFlexibleMemorySize",page+8,0,0,0)==0);
    CHECK(gmem_read_u64(&mem,page+8,&available) && available==budget-P);
    uint64_t sentinel;CHECK(gmem_read_u64(&mem,at+2*P,&sentinel) && sentinel==0x123456789abcdef0ull);
    CHECK(kernel_amm_unmap(&context,at+2*P,P)==0);
    CHECK(call(&mem,"sceKernelMunmap",at,P,0,0)==0);
    for(unsigned cycle=0;cycle<6;cycle++) {
        CHECK(gmem_write_u64(&mem,page,0));
        CHECK(call(&mem,"sceKernelMapFlexibleMemory",page,96ull<<20,3,0)==0);
        CHECK(gmem_read_u64(&mem,page,&at));
        CHECK(call(&mem,"sceKernelMunmap",at,96ull<<20,0,0)==0);
        CHECK(call(&mem,"sceKernelAvailableFlexibleMemorySize",page+8,0,0,0)==0);
        CHECK(gmem_read_u64(&mem,page+8,&available) && available==budget);
    }
    CHECK(gmem_write_u64(&mem,page,0));
    CHECK(call(&mem,"sceKernelMapFlexibleMemory",page,budget,3,0)==0);
    CHECK(gmem_read_u64(&mem,page,&at));
    CHECK(gmem_write_u64(&mem,page+16,0));
    CHECK(call(&mem,"sceKernelMapFlexibleMemory",page+16,P,3,0)==SCE_KERNEL_ERROR_ENOMEM);
    CHECK(gmem_write_u64(&mem,page,at));
    CHECK(call(&mem,"sceKernelMapFlexibleMemory",page,P,3,0x10)==0); /* replacement at full budget */
    CHECK(gmem_protect(&mem,at,P,GPROT_READ)==PS5C_OK);
    CHECK(gmem_protect(&mem,at,P,GPROT_RW)==PS5C_OK);
    CHECK(call(&mem,"sceKernelAvailableFlexibleMemorySize",page+8,0,0,0)==0);
    CHECK(gmem_read_u64(&mem,page+8,&available) && available==0);
    CHECK(kernel_amm_map(&context,at,budget,3,false,0)==0);
    CHECK(gmem_write_u64(&mem,at,0xabcdeffedcba1234ull));
    CHECK(gmem_write_u64(&mem,page,at));
    CHECK(call(&mem,"sceKernelMapFlexibleMemory",page,budget+P,3,0x10)==SCE_KERNEL_ERROR_ENOMEM);
    CHECK(gmem_read_u64(&mem,at,&sentinel) && sentinel==0xabcdeffedcba1234ull);
    CHECK(call(&mem,"sceKernelMapFlexibleMemory",page,P,3,0x10)==0);
    CHECK(gmem_unmap(&mem,at,P)==PS5C_OK); /* every gmem mutation is authoritative */
    CHECK(call(&mem,"sceKernelAvailableFlexibleMemorySize",page+8,0,0,0)==0);
    CHECK(gmem_read_u64(&mem,page+8,&available) && available==budget);
    /* Quota rejection over a direct mapping must preserve its physical
     * translation as well as its bytes, including a later unmap/remap. */
    bool have_backing=pal_shm_create(GUEST_SHM_SIZE,&mem.shm)==PS5C_OK;
    CHECK(call(&mem,"sceKernelAllocateMainDirectMemory",P,P,0,page+24)==0);
    uint64_t prefix;CHECK(gmem_read_u64(&mem,page+24,&prefix));
    CHECK(call(&mem,"sceKernelAllocateMainDirectMemory",3*P,P,0,page+24)==0);
    uint64_t phys;CHECK(gmem_read_u64(&mem,page+24,&phys) && phys!=0);
    const guest_addr direct=0xA00000000ull;
    CHECK(gmem_write_u64(&mem,page,direct));
    CHECK(call6(&mem,"sceKernelMapDirectMemory",page,3*P,3,0x10,phys,P)==0);
    CHECK(gmem_write_u64(&mem,direct,0x778899aabbccddeeull));
    CHECK(call(&mem,"sceKernelMapFlexibleMemory",page,budget+P,3,0x10)==SCE_KERNEL_ERROR_ENOMEM);
    CHECK(call(&mem,"sceKernelVirtualQuery",direct,0,page+128,72)==0);
    CHECK(gmem_read_u64(&mem,page+144,&available) && available==phys);
    CHECK(call(&mem,"sceKernelMunmap",direct,3*P,0,0)==0);
    CHECK(gmem_write_u64(&mem,page,direct+16*P));
    CHECK(call6(&mem,"sceKernelMapDirectMemory",page,3*P,3,0x10,phys,P)==0);
    if(have_backing)CHECK(gmem_read_u64(&mem,direct+16*P,&sentinel) && sentinel==0x778899aabbccddeeull);
    else puts("    SKIP: private direct byte persistence requires a shared backing object");
    CHECK(call(&mem,"sceKernelMunmap",direct+16*P,3*P,0,0)==0);
    CHECK(call(&mem,"sceKernelReleaseDirectMemory",phys,3*P,0,0)==0);
    CHECK(call(&mem,"sceKernelReleaseDirectMemory",prefix,P,0,0)==0);
    gmem_free(&mem);hle_registry_free();
}

typedef struct flex_attempt {
    gmem *mem;pal_mutex *gate;pal_cond *ready;bool *start;
    ps5c_err result;guest_addr out;
} flex_attempt;

static int32_t flexible_attempt(void *arg) {
    flex_attempt *a=arg;pal_mutex_lock(a->gate);
    while(!*a->start)pal_cond_wait(a->ready,a->gate);
    pal_mutex_unlock(a->gate);
    a->result=gmem_map_flexible(a->mem,0,GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,GPROT_RW,0,GUEST_PAGE_SIZE,"quota race",&a->out,NULL);
    return 0;
}

static void test_flexible_quota_concurrent(void) {
    gmem mem;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&mem,&cfg)==PS5C_OK);
    pal_mutex *gate;pal_cond *ready;CHECK(pal_mutex_create(&gate)==PS5C_OK);CHECK(pal_cond_create(&ready)==PS5C_OK);
    bool start=false;flex_attempt args[8];pal_thread *threads[8];
    pal_mutex_lock(gate);
    for(unsigned i=0;i<8;i++) {
        args[i]=(flex_attempt){.mem=&mem,.gate=gate,.ready=ready,.start=&start,.out=UINT64_MAX};
        CHECK(pal_thread_create(&threads[i],flexible_attempt,&args[i],"flex quota",0)==PS5C_OK);
    }
    start=true;pal_cond_broadcast(ready);pal_mutex_unlock(gate);
    unsigned successes=0;guest_addr winner=0;
    for(unsigned i=0;i<8;i++) {
        CHECK(pal_thread_join(threads[i],NULL)==PS5C_OK);
        if(args[i].result==PS5C_OK){successes++;winner=args[i].out;}
        else CHECK(args[i].result==PS5C_ERR_NOMEM && args[i].out==UINT64_MAX);
    }
    CHECK(successes==1 && gmem_flexible_used(&mem)==GUEST_PAGE_SIZE);
    CHECK(gmem_unmap(&mem,winner,GUEST_PAGE_SIZE)==PS5C_OK && gmem_flexible_used(&mem)==0);
    CHECK(gmem_map(&mem,0,GUEST_PAGE_SIZE,GPROT_RW,0,"flexible",&winner)==PS5C_OK);
    CHECK(gmem_flexible_used(&mem)==0); /* classification is independent of the caller's name */
    pal_cond_destroy(ready);pal_mutex_destroy(gate);gmem_free(&mem);
}

#ifdef PS5C_TEST_MAP_COMMIT_FAILURE
static void test_failed_replacement_drops_removed_direct_translation(void) {
    CHECK(hle_registry_init()==PS5C_OK);
    gmem mem;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&mem,&cfg)==PS5C_OK);
    const uint64_t P=GUEST_PAGE_SIZE;guest_addr output;
    CHECK(gmem_map(&mem,0,P,GPROT_RW,0,"outputs",&output)==PS5C_OK);
    CHECK(call(&mem,"sceKernelAllocateMainDirectMemory",P,P,0,output+24)==0);
    uint64_t prefix;CHECK(gmem_read_u64(&mem,output+24,&prefix));
    CHECK(call(&mem,"sceKernelAllocateMainDirectMemory",P,P,0,output+24)==0);
    uint64_t phys;CHECK(gmem_read_u64(&mem,output+24,&phys) && phys!=0);
    const guest_addr target=0xA00000000ull;
    CHECK(gmem_write_u64(&mem,output,target));
    CHECK(call6(&mem,"sceKernelMapDirectMemory",output,P,3,0x10,phys,P)==0);
    CHECK(gmem_write_u64(&mem,target,0x123456789abcdef0ull));
    fail_commit_address=gmem_host(&mem,target,P);CHECK(fail_commit_address!=NULL);
    commit_failures=0;
    CHECK(call(&mem,"sceKernelMapFlexibleMemory",output,P,3,0x10)==SCE_KERNEL_ERROR_ENOMEM);
    CHECK(commit_failures==1 && fail_commit_address==NULL);
    uint64_t unchanged;CHECK(gmem_read_u64(&mem,output,&unchanged) && unchanged==target);
    CHECK(!gmem_is_mapped(&mem,target,1) && gmem_flexible_used(&mem)==0);
    /* Build a private VMA that VirtualQuery classifies as direct, without
     * creating HLE direct metadata. Its physical offset exposes a stale entry
     * left by the failed replacement; merely querying the hole would not. */
    guest_addr probe;
    CHECK(gmem_map(&mem,target,P,GPROT_RW,GMAP_FIXED,"direct bookkeeping probe",&probe)==PS5C_OK);
    CHECK(call(&mem,"sceKernelVirtualQuery",target,0,output+128,72)==0);
    uint64_t offset;CHECK(gmem_read_u64(&mem,output+144,&offset) && offset==0);
    CHECK(gmem_unmap(&mem,target,P)==PS5C_OK);
    CHECK(call(&mem,"sceKernelMapFlexibleMemory",output,P,3,0x10)==0);
    CHECK(gmem_flexible_used(&mem)==P);
    CHECK(call(&mem,"sceKernelMunmap",target,P,0,0)==0);
    CHECK(call(&mem,"sceKernelReleaseDirectMemory",phys,P,0,0)==0);
    CHECK(call(&mem,"sceKernelReleaseDirectMemory",prefix,P,0,0)==0);
    gmem_free(&mem);hle_registry_free();
}
#endif

/* A streaming allocator can release/reallocate pieces of a physical pool and
 * later map one range across several allocations of the same memory type.
 * Validate every page, rather than requiring one bookkeeping entry. */
static void test_contiguous_direct_allocations(void) {
    CHECK(hle_registry_init() == PS5C_OK);
    gmem mem;
    gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&mem, &cfg) == PS5C_OK);
    guest_addr page;
    const uint64_t P = GUEST_PAGE_SIZE;
    CHECK(gmem_map(&mem, 0, P, GPROT_RW, 0, "direct outputs", &page) == PS5C_OK);
    uint64_t blocks[3];
    for (unsigned i = 0; i < 3; i++) {
        CHECK(call(&mem, "sceKernelAllocateMainDirectMemory", 2*P, P, 0, page+8) == 0);
        CHECK(gmem_read_u64(&mem, page+8, &blocks[i]));
        if (i) CHECK(blocks[i] == blocks[i-1] + 2*P);
    }
    guest_addr target = GUEST_MODULE_BASE + 128*P;
    CHECK(gmem_write_u64(&mem, page, target));
    CHECK(call6(&mem, "sceKernelMapDirectMemory", page, 4*P, 3, 0x10, blocks[0]+P, P) == 0);
    CHECK(gmem_write_u64(&mem, target, 0xabcdef1234567890ull));
    CHECK(gmem_write_u64(&mem, target+4*P-8, 0x1234567890abcdefull));
    CHECK(call(&mem, "sceKernelMunmap", target, 4*P, 0, 0) == 0);
    CHECK(call(&mem, "sceKernelReleaseDirectMemory", blocks[1], 2*P, 0, 0) == 0);
    guest_addr existing;
    CHECK(gmem_map(&mem, target, P, GPROT_RW, GMAP_FIXED, "replacement guard", &existing) == PS5C_OK);
    CHECK(gmem_write_u64(&mem, target, 0xfeed1234abcdef00ull));
    CHECK(gmem_write_u64(&mem, page, target));
    CHECK((uint32_t)call6(&mem, "sceKernelMapDirectMemory", page, 4*P, 3, 0x10, blocks[0]+P, P)
          == 0x8002000du); /* A hole between allocations must remain inaccessible. */
    uint64_t guard;
    CHECK(gmem_read_u64(&mem, target, &guard) && guard == 0xfeed1234abcdef00ull);
    CHECK(!gmem_is_mapped(&mem, target+P, 1));
    CHECK(gmem_unmap(&mem, target, P) == PS5C_OK);
    /* Adjacent ranges with different memory types are not one uniform pool. */
    CHECK(call6(&mem, "sceKernelAllocateDirectMemory", blocks[1], blocks[1]+2*P, 2*P, P, 11, page+8) == 0);
    CHECK((uint32_t)call6(&mem, "sceKernelMapDirectMemory", page, 4*P, 3, 0x10, blocks[0]+P, P)
          == 0x8002000du);
    CHECK(call(&mem, "sceKernelReleaseDirectMemory", blocks[1], 2*P, 0, 0) == 0);
    CHECK(call6(&mem, "sceKernelAllocateDirectMemory", blocks[1], blocks[1]+2*P, 2*P, P, 0, page+8) == 0);
    struct { uint64_t start, offset, length; uint8_t prot, type; uint16_t reserved; int32_t op; }
        entry = {target, blocks[0]+P, 4*P, 3, 0, 0, 0};
    CHECK(gmem_write(&mem, page+32, &entry, sizeof(entry)));
    CHECK(call(&mem, "sceKernelBatchMap2", page+32, 1, page+24, 0) == 0);
    uint32_t done;
    CHECK(gmem_read(&mem, page+24, &done, 4) && done == 1);
    CHECK(call(&mem, "sceKernelMunmap", target, 4*P, 0, 0) == 0);
    for (unsigned i = 0; i < 3; i++)
        CHECK(call(&mem, "sceKernelReleaseDirectMemory", blocks[i], 2*P, 0, 0) == 0);
    gmem_free(&mem); hle_registry_free();
}

/* Match the existing shared-view mapping path: gmem rounds lengths up,
 * while physical offsets and allocations stay page aligned. */
static void test_direct_mapping_length_rounding(void) {
    CHECK(hle_registry_init() == PS5C_OK);
    gmem mem;
    gmem_config cfg = gmem_default_config(); cfg.identity = true;
    CHECK(gmem_init(&mem, &cfg) == PS5C_OK);
    if (!mem.shm || mem.ph_start == mem.res_end) {
        puts("    SKIP: shared direct views require host placeholder support");
        gmem_free(&mem); hle_registry_free(); return;
    }
    const uint64_t P = GUEST_PAGE_SIZE;
    guest_addr page;
    CHECK(gmem_map(&mem, 0, P, GPROT_RW, 0, "rounding outputs", &page) == PS5C_OK);
    CHECK(call(&mem, "sceKernelAllocateMainDirectMemory", 2*P, P, 0, page+8) == 0);
    uint64_t phys;
    CHECK(gmem_read_u64(&mem, page+8, &phys));
    const guest_addr target = mem.ph_start + 128*P;
    CHECK(gmem_write_u64(&mem, page, target));
    CHECK(call6(&mem, "sceKernelMapDirectMemory", page, P+1, 3, 0x10, phys, P) == 0);
    CHECK(gmem_write_u64(&mem, target+2*P-8, 0xabcdef1234567890ull));
    CHECK(call(&mem, "sceKernelMunmap", target, 2*P, 0, 0) == 0);
    CHECK(call(&mem, "sceKernelReleaseDirectMemory", phys, 2*P, 0, 0) == 0);
    gmem_free(&mem); hle_registry_free();
}

int main(int argc,char **argv) {
    pal_init();
    if(argc==2 && !strcmp(argv[1],"--flex-reclaim")) {
        const test_case cases[]={TEST_CASE(test_flexible_budget_reclaimed),TEST_CASE(test_flexible_quota_concurrent),
#ifdef PS5C_TEST_MAP_COMMIT_FAILURE
            TEST_CASE(test_failed_replacement_drops_removed_direct_translation),
#endif
        };
        return test_run_all("flexible_reclaim",cases,PS5C_ARRAY_COUNT(cases));
    }
    const test_case tests[] = {TEST_CASE(test_configured_flexible_budget), TEST_CASE(test_contiguous_direct_allocations),
                              TEST_CASE(test_direct_mapping_length_rounding)};
    return test_run_all("kernel_memory", tests, PS5C_ARRAY_COUNT(tests));
}
