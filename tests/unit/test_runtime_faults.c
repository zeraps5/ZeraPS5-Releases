/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/runtime.h"
#include "pal/pal.h"
#include "test.h"
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

/* xor ecx,ecx; xor edx,edx; mov eax,1; div ecx; ret. */
static const uint8_t divide_code[] = {0x31, 0xc9, 0x31, 0xd2, 0xb8, 1, 0, 0, 0, 0xf7, 0xf1, 0xc3};
static uint64_t native_divide_pc;
static bool recovered;

/* Simulates a native library's own handler, later in the Windows chain. */
static LONG CALLBACK native_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_INT_DIVIDE_BY_ZERO ||
        ep->ContextRecord->Rip != native_divide_pc) return EXCEPTION_CONTINUE_SEARCH;
    recovered = true;
    ep->ContextRecord->Rax = 42;
    ep->ContextRecord->Rip += 2; /* this handler owns this exact test instruction */
    return EXCEPTION_CONTINUE_EXECUTION;
}

static int scenario(const char *mode) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (pal_init() != PS5C_OK) return 100;
    ps5_runtime *rt = calloc(1, sizeof(*rt));
    module_set ms;
    module_set_init(&ms);
    runtime_options opts = {0};
    if (!rt || runtime_init(rt, &ms, &opts) != PS5C_OK) return 101;
    bool guest = !strcmp(mode, "--guest");
    bool recover = !strcmp(mode, "--native-recover");
    void *code = NULL;
    guest_addr addr = 0;
    uint64_t size = pal_page_size();
    if (guest) {
        if (gmem_map(&rt->mem, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "divide-test", &addr) != PS5C_OK)
            return 102;
        code = gmem_host(&rt->mem, addr, sizeof(divide_code));
    } else {
        if (pal_vm_reserve(NULL, size, false, &code) != PS5C_OK ||
            pal_vm_commit(code, size, PAL_PROT_RW) != PS5C_OK) return 103;
        uint64_t pc = (uint64_t)(uintptr_t)code;
        if (pc >= rt->mem.res_start && pc < rt->mem.res_end) return 104;
    }
    if (!code) return 105;
    memcpy(code, divide_code, sizeof(divide_code));
    if (guest) {
        if (gmem_protect(&rt->mem, addr, GUEST_PAGE_SIZE, GPROT_RX) != PS5C_OK) return 106;
    } else if (pal_vm_protect(code, size, PAL_PROT_RX) != PS5C_OK) return 107;
    pal_icache_flush(code, sizeof(divide_code));
    PVOID handler = NULL;
    if (recover) {
        native_divide_pc = (uint64_t)(uintptr_t)code + 9;
        handler = AddVectoredExceptionHandler(0, native_handler);
        if (!handler) return 108;
    }
    int (*fn)(void);
    memcpy(&fn, &code, sizeof(fn));
    int value = fn();
    if (!recover || !recovered || value != 42) return 109;
    RemoveVectoredExceptionHandler(handler);
    pal_vm_release(code, size);
    runtime_free(rt);
    module_set_free(&ms);
    free(rt);
    pal_shutdown();
    return 0;
}

static uint32_t child_exit(const wchar_t *mode) {
    wchar_t path[32768], cmd[32832];
    DWORD len = GetModuleFileNameW(NULL, path, PS5C_ARRAY_COUNT(path));
    if (!len || len >= PS5C_ARRAY_COUNT(path)) return 110;
    if (swprintf(cmd, PS5C_ARRAY_COUNT(cmd), L"\"%ls\" %ls", path, mode) < 0) return 111;
    STARTUPINFOW si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(path, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return 112;
    DWORD code = 113;
    if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 114);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

static void test_native_library_can_handle_its_divide(void) {
    CHECK_EQ_U64(child_exit(L"--native-recover"), 0);
}
static void test_guest_divide_stays_fatal(void) {
    CHECK_EQ_U64(child_exit(L"--guest"), 3);
}
static void test_unhandled_native_divide_stays_fatal(void) {
    CHECK_EQ_U64(child_exit(L"--native-unhandled"), EXCEPTION_INT_DIVIDE_BY_ZERO);
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    if (argc == 2) return scenario(argv[1]);
    const test_case tests[] = {TEST_CASE(test_native_library_can_handle_its_divide),
                               TEST_CASE(test_guest_divide_stays_fatal),
                               TEST_CASE(test_unhandled_native_divide_stays_fatal)};
    return test_run_all("runtime_faults", tests, PS5C_ARRAY_COUNT(tests));
#else
    (void)argc;
    (void)argv;
    puts("runtime_faults: Windows exception dispatch test skipped on this platform");
    return 0;
#endif
}
