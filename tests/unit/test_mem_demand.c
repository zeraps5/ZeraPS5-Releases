/* SPDX-License-Identifier: GPL-2.0-or-later */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "base/base.h"
#include <windows.h>
#include <psapi.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "mem/gmem.h"
#include "hle/hle.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include "test.h"

static gmem memory;
static atomic_uint faults;
static bool fault(void *user, pal_fault_info *info) {
    if (info->kind != PAL_FAULT_ACCESS) return false;
    atomic_fetch_add(&faults, 1);
    return gmem_commit_fault(user, info->addr, info->execute ? GPROT_EXEC : info->write ? GPROT_WRITE : GPROT_READ);
}
static bool committed(guest_addr a) {
    uint64_t run = 0;
    return pal_vm_committed((void *)(uintptr_t)a, &run);
}
static bool init(void) {
    gmem_config cfg = gmem_default_config();
    cfg.demand_anonymous = true;
    atomic_store(&faults, 0);
    if (gmem_init(&memory, &cfg) != PS5C_OK) return false;
    return pal_fault_handler_install(fault, &memory) == PS5C_OK;
}
static void finish(void) {
    pal_fault_handler_install(NULL, NULL);
    gmem_free(&memory);
}

static void test_native_touch_host_copy_and_protect(void) {
    CHECK(init());
    guest_addr a;
    uint64_t size = 64ull << 20;
    CHECK(gmem_map(&memory, 0, size, GPROT_RW, GMAP_DEMAND, "large pool", &a) == PS5C_OK);
    CHECK(gmem_is_mapped(&memory, a, size) && memory.mapped_bytes == size);
    CHECK(!committed(a) && !committed(a + size - GUEST_PAGE_SIZE));
    CHECK(gmem_write_u64(&memory, a, 0x123456789abcdef0ull));
    uint64_t value = 0;
    CHECK(gmem_read_u64(&memory, a, &value) && value == 0x123456789abcdef0ull);
    CHECK(atomic_load(&faults) == 0); /* host copies cannot recurse under the map lock */
    CHECK(!committed(a + (32ull << 20)));
    CHECK(gmem_pin_mappings(&memory));
    volatile uint64_t *native = (volatile uint64_t *)(uintptr_t)(a + (32ull << 20));
    CHECK(*native == 0); /* real first-chance exception, not a simulated call */
    *native = 0xfedcba9876543210ull;
    gmem_unpin_mappings(&memory);
    CHECK(atomic_load(&faults) > 0 && committed(a + (32ull << 20)));
    CHECK(gmem_read_u64(&memory, a + (32ull << 20), &value) && value == *native);
    CHECK(!committed(a + (48ull << 20)));
    CHECK(gmem_protect(&memory, a + (48ull << 20), GUEST_PAGE_SIZE, GPROT_READ) == PS5C_OK);
    CHECK(committed(a + (48ull << 20))); /* restricted aliases must never acquire RW holes */
    CHECK(!gmem_commit_fault(&memory, a + (48ull << 20), GPROT_WRITE));
    CHECK(!gmem_commit_fault(&memory, a + (48ull << 20), GPROT_EXEC));
    CHECK(*(volatile uint64_t *)(uintptr_t)(a + (48ull << 20)) == 0);
    CHECK(gmem_protect(&memory, a + (48ull << 20), GUEST_PAGE_SIZE, GPROT_RW) == PS5C_OK);
    *(volatile uint64_t *)(uintptr_t)(a + (48ull << 20)) = 99;
    CHECK(gmem_protect(&memory, a + (56ull << 20), GUEST_PAGE_SIZE, GPROT_NONE) == PS5C_OK);
    CHECK(!gmem_commit_fault(&memory, a + (56ull << 20), GPROT_READ));
    CHECK(committed(a + (56ull << 20)));
    CHECK(gmem_unmap(&memory, a + (32ull << 20), GUEST_PAGE_SIZE) == PS5C_OK);
    CHECK(!gmem_commit_fault(&memory, a + (32ull << 20), GPROT_WRITE));
    guest_addr again;
    CHECK(gmem_map(&memory, a + (32ull << 20), GUEST_PAGE_SIZE, GPROT_RW, GMAP_FIXED, "reused", &again) == PS5C_OK);
    CHECK(gmem_read_u64(&memory, again, &value) && value == 0);
    CHECK(gmem_read_u64(&memory, a, &value) && value == 0x123456789abcdef0ull);
    finish();
}

typedef struct writer {guest_addr address;uint64_t value;} writer;
static int32_t write_native(void *arg) {
    writer *w = arg;
    *(volatile uint64_t *)(uintptr_t)w->address = w->value;
    return 0;
}
static void test_concurrent_first_touch(void) {
    CHECK(init());
    guest_addr a;
    CHECK(gmem_map(&memory, 0, 64ull << 20, GPROT_RW, GMAP_DEMAND, "concurrent", &a) == PS5C_OK);
    for (unsigned i = 0; i < 32; i++) {
        writer w[2] = {{a + i * 0x10000, i + 1}, {a + i * 0x10000 + 8, i + 101}};
        pal_thread *t[2];
        CHECK(pal_thread_create(&t[0], write_native, &w[0], "touch0", 0) == PS5C_OK);
        CHECK(pal_thread_create(&t[1], write_native, &w[1], "touch1", 0) == PS5C_OK);
        CHECK(pal_thread_join(t[0], NULL) == PS5C_OK);
        CHECK(pal_thread_join(t[1], NULL) == PS5C_OK);
        CHECK(*(volatile uint64_t *)(uintptr_t)w[0].address == w[0].value);
        CHECK(*(volatile uint64_t *)(uintptr_t)w[1].address == w[1].value);
    }
    CHECK(!committed(a + (32ull << 20)));
    finish();
}

static void test_small_code_reserved_and_shared_stay_eager(void) {
    CHECK(init());
    guest_addr a;
    CHECK(gmem_map(&memory, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "small", &a) == PS5C_OK && committed(a));
    CHECK(gmem_map(&memory, 0, 64ull << 20, GPROT_RW, 0, "CPU heap or fiber stack", &a) == PS5C_OK && committed(a));
    CHECK(gmem_map(&memory, 0, 64ull << 20, GPROT_READ | GPROT_EXEC, 0, "code", &a) == PS5C_OK && committed(a));
    CHECK(gmem_map(&memory, 0, 64ull << 20, GPROT_RW, GMAP_RESERVE, "reserved", &a) == PS5C_OK);
    CHECK(!gmem_commit_fault(&memory, a, GPROT_READ));
    if (pal_ph_supported()) {
        bool shared;
        CHECK(gmem_map_direct(&memory, 0, 64ull << 20, GUEST_PAGE_SIZE, GPROT_RW,
                             GMAP_SHARED_AREA, 0, "shared", &a, &shared) == PS5C_OK && shared);
        CHECK(committed(a));
        gmem_vma v;
        CHECK(gmem_query_vma(&memory, a, false, &v) && !v.demand);
    }
    finish();
}

static uint64_t commit_charge(void) {
    struct { PROCESS_MEMORY_COUNTERS_EX counters; SIZE_T private_ws; ULONG64 shared; } info = {0};
    info.counters.cb = sizeof(info);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&info, sizeof(info)))
        return UINT64_MAX;
    return info.counters.PrivateUsage + info.shared;
}

static DWORD protection(guest_addr addr) {
    MEMORY_BASIC_INFORMATION mi;
    return VirtualQuery((void *)(uintptr_t)addr, &mi, sizeof(mi)) ? mi.Protect : 0;
}

static void test_shared_demand_aliases_and_charge(void) {
    CHECK(init());
    if (!pal_ph_supported()) { finish(); return; }
    const uint64_t size = 64ull << 20;
    guest_addr a = memory.ph_start, b = a + size + 0x10000, out;
    bool shared;
    uint64_t before = commit_charge();
    CHECK(before != UINT64_MAX);
    CHECK(gmem_map_direct(&memory, a, size, GUEST_PAGE_SIZE, GPROT_RW, GMAP_FIXED | GMAP_DEMAND,
                          0, "GPU pool", &out, &shared) == PS5C_OK && shared);
    CHECK(gmem_map_direct(&memory, b, size, GUEST_PAGE_SIZE, GPROT_RW, GMAP_FIXED | GMAP_DEMAND,
                          0, "GPU alias", &out, &shared) == PS5C_OK && shared);
    CHECK(!committed(a) && !committed(b));
    CHECK(commit_charge() - before < (8ull << 20)); /* two 64 MiB views must not charge 128 MiB */
    *(volatile uint64_t *)(uintptr_t)(a + 0x80000) = 0x123456789abcdef0ull;
    CHECK(*(volatile uint64_t *)(uintptr_t)(b + 0x80000) == 0x123456789abcdef0ull);
    *(volatile uint64_t *)(uintptr_t)(b + 0x80000) = 42;
    CHECK(*(volatile uint64_t *)(uintptr_t)(a + 0x80000) == 42);
    CHECK(!committed(a + (32ull << 20)) && !committed(b + (32ull << 20)));

    CHECK(gmem_protect(&memory, b + (1ull << 20), GUEST_PAGE_SIZE, GPROT_READ) == PS5C_OK);
    *(volatile uint64_t *)(uintptr_t)(a + (1ull << 20)) = 123;
    CHECK(*(volatile uint64_t *)(uintptr_t)(b + (1ull << 20)) == 123);
    CHECK(protection(b + (1ull << 20)) == PAGE_READONLY);
    CHECK(!gmem_commit_fault(&memory, b + (1ull << 20), GPROT_WRITE));
    CHECK(gmem_protect(&memory, b + (2ull << 20), GUEST_PAGE_SIZE, GPROT_NONE) == PS5C_OK);
    *(volatile uint64_t *)(uintptr_t)(a + (2ull << 20)) = 456;
    CHECK(protection(b + (2ull << 20)) == PAGE_NOACCESS);
    CHECK(!gmem_commit_fault(&memory, b + (2ull << 20), GPROT_READ));
    CHECK(!committed(a + (32ull << 20)));

    CHECK(gmem_unmap(&memory, a + GUEST_PAGE_SIZE, GUEST_PAGE_SIZE) == PS5C_OK);
    CHECK(!gmem_is_mapped(&memory, a + GUEST_PAGE_SIZE, 1));
    CHECK(*(volatile uint64_t *)(uintptr_t)(a + 0x80000) == 42);
    CHECK(*(volatile uint64_t *)(uintptr_t)(b + 0x80000) == 42);
    CHECK(!committed(a + (32ull << 20)));
    CHECK(commit_charge() - before < (8ull << 20));
    printf("    two 64 MiB aliases: %llu KiB additional private+shared commit\n",
           (unsigned long long)((commit_charge() - before) >> 10));
    finish();
}

static int32_t kernel_call(const char *name, uint64_t a0, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    const hle_export *exp = hle_find("libkernel", nid_hash_name(name));
    if (!exp || !exp->fn) return INT32_MIN;
    hle_args args = {.gpr = {a0,a1,a2,a3,a4,a5}};
    hle_call call = {.a = &args, .mem = &memory};
    exp->fn(&call);
    return (int32_t)args.ret[0];
}

static void test_kernel_memory_type_policy(void) {
    CHECK(init());
    CHECK(hle_registry_init() == PS5C_OK);
    guest_addr outputs;
    CHECK(gmem_map(&memory, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "outputs", &outputs) == PS5C_OK);
    const int types[] = {0,3,10,11,12};
    for (unsigned i = 0; i < PS5C_ARRAY_COUNT(types); i++) {
        uint64_t size = 64ull << 20, phys;
        guest_addr at = 0x500000000ull + i * size;
        CHECK(kernel_call("sceKernelAllocateDirectMemory", 0, 14ull << 30, size,
                          GUEST_PAGE_SIZE, (uint32_t)types[i], outputs) == 0);
        CHECK(gmem_read_u64(&memory, outputs, &phys));
        CHECK(gmem_write_u64(&memory, outputs + 8, at));
        CHECK(kernel_call("sceKernelMapDirectMemory", outputs + 8, size, 0x33, 0x10,
                          phys, GUEST_PAGE_SIZE) == 0);
        gmem_vma v;
        CHECK(gmem_query_vma(&memory, at, false, &v));
        CHECK(v.demand == (types[i] == 3 || types[i] == 11));
        CHECK(committed(at) == (types[i] != 3 && types[i] != 11));
    }
    if (pal_ph_supported()) {
        uint64_t phys, at;
        CHECK(kernel_call("sceKernelAllocateDirectMemory", 0, 14ull << 30, 64ull << 20,
                          GUEST_PAGE_SIZE, 11, outputs) == 0);
        CHECK(gmem_read_u64(&memory, outputs, &phys));
        CHECK(gmem_write_u64(&memory, outputs + 8, 0));
        CHECK(kernel_call("sceKernelMapDirectMemory", outputs + 8, 64ull << 20, 0xf2, 0,
                          phys, GUEST_PAGE_SIZE) == 0);
        CHECK(gmem_read_u64(&memory, outputs + 8, &at));
        gmem_vma v;
        CHECK(gmem_query_vma(&memory, at, false, &v) && v.kind == GVMA_VIEW && v.demand);
        CHECK(!committed(at));
    }
    finish();
    hle_registry_free();
}

static void test_sparse_file_backing_and_cleanup(void) {
    if (!pal_ph_supported()) return;
    char path[128];
    snprintf(path, sizeof(path), "astro-physical-test-%lu-%llu.bin", (unsigned long)GetCurrentProcessId(),
             (unsigned long long)GetTickCount64());
    CHECK(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
    CHECK(_putenv_s("PS5C_SHM_BACKING_FILE", path) == 0); ps5c_env_reset();
    CHECK(init());
    uint64_t before = commit_charge();
    guest_addr a = memory.ph_start, b = a + (64ull << 20), out;
    bool shared;
    CHECK(gmem_map_direct(&memory, a, 64ull << 20, GUEST_PAGE_SIZE, GPROT_RW,
                          GMAP_FIXED | GMAP_DEMAND, 0, "file GPU pool", &out, &shared) == PS5C_OK && shared);
    CHECK(gmem_map_direct(&memory, b, 64ull << 20, GUEST_PAGE_SIZE, GPROT_RW,
                          GMAP_FIXED, 0, "file alias", &out, &shared) == PS5C_OK && shared);
    CHECK(commit_charge() - before < (8ull << 20));
    CHECK(*(volatile uint64_t *)(uintptr_t)a == 0);
    CHECK(gmem_write_u64(&memory, a, 0x1122334455667788ull));
    CHECK(*(volatile uint64_t *)(uintptr_t)b == 0x1122334455667788ull);
    void *host = NULL;
    CHECK(pal_shm_map(memory.shm, GUEST_SHM_SIZE, &host) == PS5C_OK);
    CHECK(pal_vm_commit(host, GUEST_PAGE_SIZE, GPROT_RW) == PS5C_OK);
    *(volatile uint64_t *)host = 999;
    CHECK(*(volatile uint64_t *)(uintptr_t)a == 999 && *(volatile uint64_t *)(uintptr_t)b == 999);
    CHECK(UnmapViewOfFile(host));
    CHECK(gmem_unmap(&memory, a, 64ull << 20) == PS5C_OK);
    CHECK(gmem_map_direct(&memory, a, 64ull << 20, GUEST_PAGE_SIZE, GPROT_RW,
                          GMAP_FIXED | GMAP_DEMAND, 0, "file remap", &out, &shared) == PS5C_OK && shared);
    CHECK(*(volatile uint64_t *)(uintptr_t)a == 999);
    finish();
    CHECK(_putenv_s("PS5C_SHM_BACKING_FILE", "") == 0); ps5c_env_reset();
    CHECK(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);

    /* An existing file is never overwritten, even on initialization failure. */
    HANDLE sentinel = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(sentinel != INVALID_HANDLE_VALUE);
    const uint64_t marker = 0xfeed1234abcdef01ull;
    DWORD done;
    CHECK(WriteFile(sentinel, &marker, sizeof(marker), &done, NULL) && done == sizeof(marker));
    CloseHandle(sentinel);
    CHECK(_putenv_s("PS5C_SHM_BACKING_FILE", path) == 0); ps5c_env_reset();
    gmem_config cfg = gmem_default_config();
    CHECK(gmem_init(&memory, &cfg) != PS5C_OK);
    CHECK(_putenv_s("PS5C_SHM_BACKING_FILE", "") == 0); ps5c_env_reset();
    sentinel = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(sentinel != INVALID_HANDLE_VALUE);
    uint64_t read_marker;
    CHECK(ReadFile(sentinel, &read_marker, sizeof(read_marker), &done, NULL) && done == sizeof(read_marker));
    CloseHandle(sentinel);
    CHECK(read_marker == marker);
    CHECK(DeleteFileA(path));
}

static void test_eager_cpu_file_aliases(void) {
    if (!pal_ph_supported()) return;
    char path[128], old_split[32] = {0};
    const char *split = getenv("PS5C_LOW_SHARED_AREA");
    if (split) snprintf(old_split, sizeof(old_split), "%s", split);
    snprintf(path, sizeof(path), "astro-cpu-test-%lu-%llu.bin", (unsigned long)GetCurrentProcessId(),
             (unsigned long long)GetTickCount64());
    CHECK(_putenv_s("PS5C_LOW_SHARED_AREA", "3") == 0); ps5c_env_reset();
    CHECK(_putenv_s("PS5C_SHM_BACKING_FILE", path) == 0); ps5c_env_reset();
    CHECK(init());
    CHECK(memory.ph_start == 0x280000000ull);
    const guest_addr cpu = 0x300000000ull, alias = 0x400000000ull;
    const uint64_t size = 128ull << 20;
    uint64_t before = commit_charge();
    guest_addr out;
    bool shared;
    CHECK(gmem_map_direct(&memory, cpu, size, GUEST_PAGE_SIZE, GPROT_RW,
                          GMAP_FIXED, 0, "eager CPU heap", &out, &shared) == PS5C_OK && shared);
    CHECK(gmem_map_direct(&memory, alias, size, GUEST_PAGE_SIZE, GPROT_RW,
                          GMAP_FIXED, 0, "CPU alias", &out, &shared) == PS5C_OK && shared);
    gmem_vma v;
    CHECK(gmem_query_vma(&memory, cpu, false, &v) && v.kind == GVMA_VIEW && !v.demand);
    CHECK(committed(cpu) && committed(cpu + size - GUEST_PAGE_SIZE));
    CHECK(protection(cpu) == PAGE_READWRITE);
    CHECK(commit_charge() - before < (8ull << 20));
    CHECK(*(volatile uint64_t *)(uintptr_t)(cpu + size - 8) == 0);
    writer w = {cpu + size - 8, 0x123456789abcdef0ull};
    pal_thread *t;
    CHECK(pal_thread_create(&t, write_native, &w, "CPU file page", 0) == PS5C_OK);
    CHECK(pal_thread_join(t, NULL) == PS5C_OK);
    CHECK(*(volatile uint64_t *)(uintptr_t)(alias + size - 8) == w.value);
    CHECK(atomic_load(&faults) == 0);
    CHECK(gmem_protect(&memory, alias, GUEST_PAGE_SIZE, GPROT_READ) == PS5C_OK);
    CHECK(protection(alias) == PAGE_READONLY);
    CHECK(protection(cpu) == PAGE_READWRITE);
    *(volatile uint64_t *)(uintptr_t)cpu = 999;
    CHECK(*(volatile uint64_t *)(uintptr_t)alias == 999);
    finish();
    CHECK(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
    CHECK(_putenv_s("PS5C_SHM_BACKING_FILE", "") == 0); ps5c_env_reset();
    CHECK(_putenv_s("PS5C_LOW_SHARED_AREA", old_split) == 0); ps5c_env_reset();
}

int main(void) {
    if (pal_init() != PS5C_OK) return 1;
    static const test_case tests[] = {TEST_CASE(test_native_touch_host_copy_and_protect),
        TEST_CASE(test_concurrent_first_touch), TEST_CASE(test_small_code_reserved_and_shared_stay_eager),
        TEST_CASE(test_shared_demand_aliases_and_charge), TEST_CASE(test_kernel_memory_type_policy),
        TEST_CASE(test_sparse_file_backing_and_cleanup), TEST_CASE(test_eager_cpu_file_aliases)};
    return test_run_all("mem_demand", tests, PS5C_ARRAY_COUNT(tests));
}
