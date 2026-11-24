/* SPDX-License-Identifier: GPL-2.0-or-later */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define COBJMACROS
#include <windows.h>
#include <winioctl.h>
#include <dxgi.h>
#include <mmsystem.h>
#include <shellapi.h>
#ifndef WAVE_FORMAT_IEEE_FLOAT
#define WAVE_FORMAT_IEEE_FLOAT 0x0003
#endif

#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pal/pal.h"
#include "pal/native_input.h"
#include "pal_win32/native_input_win32.h"
#include "pal/pal_exfat.h"
#include "pal_win32/dualsense.h"
#include "pal_win32/keynames.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static uint64_t g_page_size = 4096;
static uint64_t g_alloc_granularity = 65536;
static uint64_t g_qpc_freq = 1;

typedef HRESULT(WINAPI *set_thread_description_fn)(HANDLE, PCWSTR);
static set_thread_description_fn g_set_thread_description;
static wchar_t *utf8_to_wide(const char *s);

ps5c_err pal_init(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_page_size = si.dwPageSize;
    g_alloc_granularity = si.dwAllocationGranularity;

    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_freq = (uint64_t)f.QuadPart;

    /* Windows 10 1607+; resolved dynamically so older SDK headers still build. */
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32) {
        g_set_thread_description =
            (set_thread_description_fn)(void (*)(void))GetProcAddress(kernel32, "SetThreadDescription");
    }

    SetConsoleOutputCP(CP_UTF8);
    return exfat_init();
}

void pal_shutdown(void) { exfat_shutdown(); }

const char *pal_os_name(void) { return "Windows"; }

/* ---------------------------------------------------------------------------
 * Virtual memory
 * ------------------------------------------------------------------------- */

static DWORD win_prot(uint32_t prot) {
    switch (prot & PAL_PROT_RWX) {
    case PAL_PROT_NONE: return PAGE_NOACCESS;
    case PAL_PROT_READ: return PAGE_READONLY;
    case PAL_PROT_WRITE:
    case PAL_PROT_RW: return PAGE_READWRITE;
    case PAL_PROT_EXEC: return PAGE_EXECUTE;
    case PAL_PROT_RX: return PAGE_EXECUTE_READ;
    default: return PAGE_EXECUTE_READWRITE; /* W+X and RWX */
    }
}

uint64_t pal_page_size(void) { return g_page_size; }

uint64_t pal_alloc_granularity(void) { return g_alloc_granularity; }

ps5c_err pal_vm_reserve(void *want, uint64_t size, bool fixed, void **out) {
    if (size == 0 || (fixed && !want)) {
        return PS5C_ERR_INVALID;
    }
    if (want && ((uintptr_t)want % g_alloc_granularity) != 0) {
        return PS5C_ERR_INVALID;
    }
    size = PS5C_ALIGN_UP(size, g_page_size);

    void *p = VirtualAlloc(want, (SIZE_T)size, MEM_RESERVE, PAGE_NOACCESS);
    if (!p && want && !fixed) {
        p = VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE, PAGE_NOACCESS);
    }
    if (!p) {
        return fixed ? PS5C_ERR_BUSY : PS5C_ERR_NOMEM;
    }
    if (fixed && p != want) {
        VirtualFree(p, 0, MEM_RELEASE);
        return PS5C_ERR_BUSY;
    }
    *out = p;
    return PS5C_OK;
}

ps5c_err pal_vm_commit(void *addr, uint64_t size, uint32_t prot) {
    if (!VirtualAlloc(addr, (SIZE_T)size, MEM_COMMIT, win_prot(prot))) {
        DWORD error = GetLastError();
        /* A real-file-backed shared view is already committed by the memory
         * manager. It cannot be committed with VirtualAlloc (SEC_RESERVE is
         * only for page-file sections). Preserve the requested protection. */
        MEMORY_BASIC_INFORMATION mi;
        if (VirtualQuery(addr, &mi, sizeof(mi)) && mi.Type == MEM_MAPPED && mi.State == MEM_COMMIT &&
            (uint8_t *)addr + size <= (uint8_t *)mi.BaseAddress + mi.RegionSize)
            return pal_vm_protect(addr, size, prot);
        return error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_COMMITMENT_LIMIT
                   ? PS5C_ERR_NOMEM
                   : PS5C_ERR_OS;
    }
    return PS5C_OK;
}

ps5c_err pal_vm_decommit(void *addr, uint64_t size) {
    return VirtualFree(addr, (SIZE_T)size, MEM_DECOMMIT) ? PS5C_OK : PS5C_ERR_OS;
}

ps5c_err pal_vm_protect(void *addr, uint64_t size, uint32_t prot) {
    DWORD old;
    return VirtualProtect(addr, (SIZE_T)size, win_prot(prot), &old) ? PS5C_OK : PS5C_ERR_OS;
}

ps5c_err pal_vm_release(void *addr, uint64_t size) {
    PS5C_UNUSED(size);
    return VirtualFree(addr, 0, MEM_RELEASE) ? PS5C_OK : PS5C_ERR_OS;
}

/* ---- Placeholders (VirtualAlloc2 / MapViewOfFile3, resolved at run time) ---- */

typedef PVOID(WINAPI *virtual_alloc2_fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef PVOID(WINAPI *map_view3_fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef BOOL(WINAPI *unmap_view2_fn)(HANDLE, PVOID, ULONG);
static virtual_alloc2_fn g_virtual_alloc2;
static map_view3_fn g_map_view3;
static unmap_view2_fn g_unmap_view2;
static int g_ph_state = -1;

#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif

bool pal_ph_supported(void) {
    if (g_ph_state < 0) {
        HMODULE kb = LoadLibraryW(L"kernelbase.dll");
        if (kb) {
            g_virtual_alloc2 = (virtual_alloc2_fn)(void (*)(void))GetProcAddress(kb, "VirtualAlloc2");
            g_map_view3 = (map_view3_fn)(void (*)(void))GetProcAddress(kb, "MapViewOfFile3");
            g_unmap_view2 = (unmap_view2_fn)(void (*)(void))GetProcAddress(kb, "UnmapViewOfFile2");
        }
        g_ph_state = g_virtual_alloc2 && g_map_view3 && g_unmap_view2;
    }
    return g_ph_state == 1;
}

static ps5c_err os_err(void) {
    DWORD e = GetLastError();
    return e == ERROR_NOT_ENOUGH_MEMORY || e == ERROR_COMMITMENT_LIMIT ? PS5C_ERR_NOMEM : PS5C_ERR_OS;
}

ps5c_err pal_ph_reserve(void *addr, uint64_t size) {
    if (!pal_ph_supported()) return PS5C_ERR_UNSUPPORTED;
    void *p = g_virtual_alloc2(GetCurrentProcess(), addr, (SIZE_T)size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                               PAGE_NOACCESS, NULL, 0);
    if (p && p != addr) {
        VirtualFree(p, 0, MEM_RELEASE);
        return PS5C_ERR_BUSY;
    }
    return p ? PS5C_OK : os_err();
}

ps5c_err pal_ph_split(void *addr, uint64_t size) {
    return VirtualFree(addr, (SIZE_T)size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) ? PS5C_OK : os_err();
}

ps5c_err pal_ph_coalesce(void *addr, uint64_t size) {
    return VirtualFree(addr, (SIZE_T)size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS) ? PS5C_OK : os_err();
}

ps5c_err pal_ph_alloc(void *addr, uint64_t size, uint32_t prot) {
    void *p = g_virtual_alloc2(GetCurrentProcess(), addr, (SIZE_T)size, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                               win_prot(prot), NULL, 0);
    return p == addr ? PS5C_OK : os_err();
}

ps5c_err pal_ph_free(void *addr, uint64_t size) { return pal_ph_split(addr, size); }

ps5c_err pal_ph_map_view(void *shm, uint64_t offset, void *addr, uint64_t size, uint32_t prot, bool demand) {
    void *p = g_map_view3((HANDLE)shm, GetCurrentProcess(), addr, offset, (SIZE_T)size, MEM_REPLACE_PLACEHOLDER,
                          PAGE_EXECUTE_READWRITE, NULL, 0);
    if (p != addr) return os_err();
    if (demand) {
        /* Section commitment is shared by all views. Existing pages must keep
         * this new view's logical protection; untouched pages stay reserved. */
        for (uint64_t offset_in_view = 0; offset_in_view < size;) {
            MEMORY_BASIC_INFORMATION mi;
            uint8_t *at = (uint8_t *)addr + offset_in_view;
            if (!VirtualQuery(at, &mi, sizeof(mi))) goto demand_failed;
            uint64_t run = (uint64_t)((uint8_t *)mi.BaseAddress + mi.RegionSize - at);
            if (!run) goto demand_failed;
            run = PS5C_MIN(run, size - offset_in_view);
            DWORD old;
            if (mi.State == MEM_COMMIT && !VirtualProtect(at, (SIZE_T)run, win_prot(prot), &old))
                goto demand_failed;
            offset_in_view += run;
        }
        return PS5C_OK;
    }
    /* the object's pages are committed through the first view that uses them */
    MEMORY_BASIC_INFORMATION mi;
    bool committed = VirtualQuery(addr, &mi, sizeof(mi)) && mi.State == MEM_COMMIT &&
        (uint8_t *)addr + size <= (uint8_t *)mi.BaseAddress + mi.RegionSize;
    if (!committed && !VirtualAlloc(addr, (SIZE_T)size, MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
        ps5c_err e = os_err();
        g_unmap_view2(GetCurrentProcess(), addr, MEM_PRESERVE_PLACEHOLDER);
        return e;
    }
    DWORD old;
    return VirtualProtect(addr, (SIZE_T)size, win_prot(prot), &old) ? PS5C_OK : os_err();
demand_failed: {
        ps5c_err err = os_err();
        g_unmap_view2(GetCurrentProcess(), addr, MEM_PRESERVE_PLACEHOLDER);
        return err;
    }
}

ps5c_err pal_ph_unmap_view(void *addr) {
    return g_unmap_view2(GetCurrentProcess(), addr, MEM_PRESERVE_PLACEHOLDER) ? PS5C_OK : os_err();
}

ps5c_err pal_vm_release_range(void *addr, uint64_t size) {
    uint8_t *p = (uint8_t *)addr, *end = p + size;
    ps5c_err err = PS5C_OK;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(p, &mi, sizeof(mi))) return os_err();
        uint8_t *next = (uint8_t *)mi.BaseAddress + mi.RegionSize;
        /* a whole allocation at a time: its later regions are then free */
        if (mi.State != MEM_FREE && (mi.Type == MEM_MAPPED ? !UnmapViewOfFile(mi.AllocationBase)
                                                           : !VirtualFree(mi.AllocationBase, 0, MEM_RELEASE)))
            err = os_err();
        p = next;
    }
    return err;
}

void pal_shm_destroy(void *shm) {
    if (shm) CloseHandle((HANDLE)shm);
}

ps5c_err pal_shm_create(uint64_t size, void **out) {
    HANDLE file = INVALID_HANDLE_VALUE;
    const char *backing = getenv("PS5C_SHM_BACKING_FILE");
    if (backing && *backing) {
        wchar_t *path = utf8_to_wide(backing);
        if (!path) return PS5C_ERR_NOMEM;
        /* CREATE_NEW can never overwrite an existing file. A section retains
         * the file until its final view/handle closes, even on process exit. */
        file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | DELETE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        free(path);
        if (file == INVALID_HANDLE_VALUE) return os_err();
        DWORD returned;
        LARGE_INTEGER length;
        length.QuadPart = (LONGLONG)size;
        if (!DeviceIoControl(file, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &returned, NULL) ||
            !SetFilePointerEx(file, length, NULL, FILE_BEGIN) || !SetEndOfFile(file)) {
            ps5c_err err = os_err();
            CloseHandle(file);
            return err;
        }
    }
    HANDLE h = CreateFileMappingW(file, NULL, PAGE_EXECUTE_READWRITE | (file == INVALID_HANDLE_VALUE ? SEC_RESERVE : 0), (DWORD)(size >> 32),
                                  (DWORD)size, NULL);
    DWORD error = GetLastError();
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    SetLastError(error);
    if (!h) return os_err();
    *out = h;
    return PS5C_OK;
}

ps5c_err pal_shm_map(void *shm, uint64_t size, void **out) {
    void *p = MapViewOfFile((HANDLE)shm, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)size);
    if (!p) return os_err();
    *out = p;
    return PS5C_OK;
}

bool pal_vm_cow_supported(void) { return true; }

bool pal_vm_cow_arm(void *addr, uint64_t size) {
    DWORD old;
    return VirtualProtect(addr, (SIZE_T)size, PAGE_WRITECOPY, &old) != 0;
}

/* Page classes from the working-set attributes (PSAPI_WORKING_SET_EX_INFORMATION: Valid is bit 0,
 * Win32Protection bits 4-14, Shared bit 15). Asked per page: VirtualQuery walks the page tables
 * to the end of a run, which takes milliseconds in a view of gigabytes. */
enum { COW_CLEAN, COW_SHARED, COW_PRIVATE, COW_OTHER };
typedef struct ws_info { void *address; ULONG_PTR attributes; } ws_info;
static BOOL ws_query(ws_info *info, uint32_t count) {
    static BOOL (WINAPI *query)(HANDLE, void *, DWORD);
    if (!query) {
        HMODULE kb = GetModuleHandleW(L"kernel32.dll");
        query = kb ? (BOOL (WINAPI *)(HANDLE, void *, DWORD))(void (*)(void))GetProcAddress(kb, "K32QueryWorkingSetEx") : NULL;
    }
    return query && query(GetCurrentProcess(), info, (DWORD)(count * sizeof(ws_info)));
}
/* Brings a page that is out of the working set back in by reading it through ReadProcessMemory,
 * which fails instead of faulting when the page is gone (unmapped meanwhile) or not readable.
 * VirtualQuery would also tell, but it walks the page tables to the end of the run: milliseconds
 * in a view of gigabytes, for every page asked. */
static bool ws_touch(ws_info *info) {
    uint8_t b;
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), info->address, &b, 1, &got) || got != 1) return false;
    return ws_query(info, 1) && (info->attributes & 1);
}
static int cow_class(ws_info info) {
    if (!(info.attributes & 1) && !ws_touch(&info)) return COW_OTHER; /* not in the working set */
    if (!(info.attributes & (1u << 15))) return COW_PRIVATE;
    return ((info.attributes >> 4) & 0x7FF) == PAGE_WRITECOPY ? COW_CLEAN : COW_SHARED;
}

bool pal_vm_cow_clean(void *addr, uint64_t size, uint64_t *off, uint64_t *len, bool *is_private) {
    uint8_t *at = (uint8_t *)((uintptr_t)addr & ~(uintptr_t)0xFFF), *end = (uint8_t *)addr + size;
    while (at < end) {
        ws_info first = {at, 0};
        int c = ws_query(&first, 1) ? cow_class(first) : COW_OTHER;
        if (c == COW_CLEAN) {
            /* an armed run: one VirtualQuery finds its end (about 10 ns a page). Never asked of
             * a page that is not armed: its run may go on for gigabytes. */
            MEMORY_BASIC_INFORMATION mi;
            uint64_t run = 0x1000;
            if (VirtualQuery(at, &mi, sizeof(mi)) && mi.State == MEM_COMMIT && mi.Protect == PAGE_WRITECOPY)
                run = (uint64_t)((uint8_t *)mi.BaseAddress + mi.RegionSize - at);
            at += run ? run : 0x1000;
            continue;
        }
        uint8_t *run = at, *stop = at + 0x1000; /* the pages after it of the same class */
        while (stop < end) {
            ws_info info[256];
            uint32_t n = 0, same = 0;
            for (uint8_t *p = stop; p < end && n < 256; p += 0x1000) info[n++] = (ws_info){p, 0};
            if (!ws_query(info, n)) break;
            while (same < n && cow_class(info[same]) == c) same++;
            stop += (uint64_t)same * 0x1000;
            if (same < n) break;
        }
        if (run < (uint8_t *)addr) run = (uint8_t *)addr;
        if (stop > end) stop = end;
        *off = (uint64_t)(run - (uint8_t *)addr);
        *len = (uint64_t)(stop - run);
        *is_private = c == COW_PRIVATE;
        return false;
    }
    return true;
}

bool pal_vm_page_private(void *addr) {
    ws_info info = {addr, 0};
    if (!ws_query(&info, 1)) return true; /* unknown: the caller copies it back to be safe */
    int c = cow_class(info);
    return c == COW_PRIVATE || c == COW_OTHER;
}

void pal_vm_private_pages(void *addr, uint32_t pages, uint8_t *out) {
    for (uint32_t at = 0; at < pages;) {
        ws_info info[256];
        uint32_t n = pages - at < 256 ? pages - at : 256;
        for (uint32_t i = 0; i < n; i++) info[i] = (ws_info){(uint8_t *)addr + ((uint64_t)(at + i) << 12), 0};
        bool ok = ws_query(info, n);
        for (uint32_t i = 0; i < n; i++) {
            int c = ok ? cow_class(info[i]) : COW_OTHER;
            out[at + i] = c == COW_PRIVATE || c == COW_OTHER; /* unknown: copied back to be safe */
        }
        at += n;
    }
}

bool pal_vm_committed(void *addr, uint64_t *run) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(addr, &mi, sizeof(mi))) {
        *run = 0;
        return false;
    }
    *run = (uint64_t)((uint8_t *)mi.BaseAddress + mi.RegionSize - (uint8_t *)addr);
    return mi.State == MEM_COMMIT;
}

void pal_icache_flush(void *addr, uint64_t size) {
    FlushInstructionCache(GetCurrentProcess(), addr, (SIZE_T)size);
}

/* ---------------------------------------------------------------------------
 * Time
 * ------------------------------------------------------------------------- */

uint64_t pal_time_ns(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    uint64_t ticks = (uint64_t)c.QuadPart;
    return (ticks / g_qpc_freq) * 1000000000ull + (ticks % g_qpc_freq) * 1000000000ull / g_qpc_freq;
}

void pal_sleep_ns(uint64_t ns) {
    if (ns == 0) {
        SwitchToThread();
        return;
    }
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due;
        uint64_t units = (ns + 99) / 100; /* relative time in 100 ns units */
        due.QuadPart = -(LONGLONG)PS5C_MIN(units, (uint64_t)INT64_MAX);
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            CloseHandle(timer);
            return;
        }
        CloseHandle(timer);
    }
    uint64_t ms = (ns + 999999) / 1000000;
    Sleep((DWORD)PS5C_MIN(ms, (uint64_t)(INFINITE - 1)));
}

/* ---------------------------------------------------------------------------
 * UTF-8 <-> UTF-16
 * ------------------------------------------------------------------------- */

static wchar_t *utf8_to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) {
        return NULL;
    }
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n) {
        free(w);
        return NULL;
    }
    return w;
}

void pal_utf8_args(int *argc, char ***argv) {
    int n = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!wargv) return;
    char **out = (char **)calloc((size_t)n + 1, sizeof(char *)); /* kept for the process lifetime */
    for (int i = 0; out && i < n; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        out[i] = len > 0 ? (char *)malloc((size_t)len) : NULL;
        if (!out[i] || WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, out[i], len, NULL, NULL) != len) {
            LocalFree(wargv);
            return; /* keep the ANSI arguments */
        }
    }
    LocalFree(wargv);
    if (!out) return;
    *argc = n;
    *argv = out;
}

/* ---------------------------------------------------------------------------
 * Threads
 * ------------------------------------------------------------------------- */

struct pal_thread {
    HANDLE handle;
    pal_thread_fn fn;
    void *arg;
    int32_t ret;
};

uintptr_t pal_thread_open_current(void) {
    HANDLE h = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
        return 0;
    }
    return (uintptr_t)h;
}

void pal_thread_close_handle(uintptr_t h) {
    if (h) CloseHandle((HANDLE)h);
}

bool pal_thread_sample(uintptr_t h, uint64_t *pc, uint64_t *sp, uint64_t *fp) {
    if (!h || SuspendThread((HANDLE)h) == (DWORD)-1) return false;
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    BOOL ok = GetThreadContext((HANDLE)h, &ctx);
    ResumeThread((HANDLE)h);
    if (!ok) return false;
    *pc = ctx.Rip;
    *sp = ctx.Rsp;
    *fp = ctx.Rbp;
    return true;
}

bool pal_thread_suspend(uintptr_t h) {
    return h && SuspendThread((HANDLE)h) != (DWORD)-1;
}

/* Debugging aid: how many suspensions the thread currently has. */
bool pal_wait_on_address(volatile void *addr, uint64_t expected, uint32_t size, uint64_t timeout_ns) {
    uint64_t deadline = timeout_ns == UINT64_MAX ? UINT64_MAX : pal_time_ns() + timeout_ns;
    for (;;) {
        uint64_t cur = size == 8 ? *(volatile uint64_t *)addr : *(volatile uint32_t *)addr;
        if (cur != expected) return true;
        uint64_t now = pal_time_ns();
        if (now >= deadline) return false;
        uint64_t left_ms = (deadline - now + 999999) / 1000000;
        DWORD ms = deadline == UINT64_MAX || left_ms > 100 ? 100 : (DWORD)left_ms;
        if (WaitOnAddress(addr, &expected, size, ms)) return true;
    }
}

void pal_wake_on_address(volatile void *addr, int32_t count) {
    if (count < 0 || count > 64) {
        WakeByAddressAll((PVOID)addr);
        return;
    }
    for (int32_t i = 0; i < count; i++) WakeByAddressSingle((PVOID)addr);
}

int pal_thread_suspend_count(uintptr_t h) {
    DWORD c = h ? SuspendThread((HANDLE)h) : (DWORD)-1;
    if (c == (DWORD)-1) return -1;
    ResumeThread((HANDLE)h);
    return (int)c;
}

void pal_thread_resume(uintptr_t h) {
    if (h) ResumeThread((HANDLE)h);
}

bool pal_thread_get_regs(uintptr_t h, pal_cpu_regs *out) {
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT;
    if (!h || !GetThreadContext((HANDLE)h, &ctx)) return false;
    const DWORD64 regs[16] = {ctx.Rax, ctx.Rcx, ctx.Rdx, ctx.Rbx, ctx.Rsp, ctx.Rbp, ctx.Rsi, ctx.Rdi,
                              ctx.R8,  ctx.R9,  ctx.R10, ctx.R11, ctx.R12, ctx.R13, ctx.R14, ctx.R15};
    for (int i = 0; i < 16; i++) out->gpr[i] = regs[i];
    out->rip = ctx.Rip;
    out->rflags = ctx.EFlags;
    memcpy(out->fxsave, &ctx.FltSave, sizeof(out->fxsave));
    return true;
}

static unsigned __stdcall thread_entry(void *param) {
    pal_thread *t = (pal_thread *)param;
    t->ret = t->fn(t->arg);
    return 0;
}

ps5c_err pal_thread_create(pal_thread **out, pal_thread_fn fn, void *arg, const char *name,
                           uint64_t stack_size) {
    pal_thread *t = (pal_thread *)calloc(1, sizeof(*t));
    if (!t) {
        return PS5C_ERR_NOMEM;
    }
    t->fn = fn;
    t->arg = arg;
    uintptr_t h = _beginthreadex(NULL, (unsigned)PS5C_MIN(stack_size, (uint64_t)UINT32_MAX),
                                 thread_entry, t, CREATE_SUSPENDED, NULL);
    if (h == 0) {
        free(t);
        return PS5C_ERR_OS;
    }
    t->handle = (HANDLE)h;
    if (name && g_set_thread_description) {
        wchar_t *wname = utf8_to_wide(name);
        if (wname) {
            g_set_thread_description(t->handle, wname);
            free(wname);
        }
    }
    ResumeThread(t->handle);
    *out = t;
    return PS5C_OK;
}

ps5c_err pal_thread_join(pal_thread *t, int32_t *ret_out) {
    if (WaitForSingleObject(t->handle, INFINITE) != WAIT_OBJECT_0) {
        return PS5C_ERR_OS;
    }
    CloseHandle(t->handle);
    if (ret_out) {
        *ret_out = t->ret;
    }
    free(t);
    return PS5C_OK;
}

uint64_t pal_thread_current_id(void) { return GetCurrentThreadId(); }

void pal_thread_yield(void) { SwitchToThread(); }

struct pal_mutex {
    SRWLOCK lock;
};

ps5c_err pal_mutex_create(pal_mutex **out) {
    pal_mutex *m = (pal_mutex *)malloc(sizeof(*m));
    if (!m) {
        return PS5C_ERR_NOMEM;
    }
    InitializeSRWLock(&m->lock);
    *out = m;
    return PS5C_OK;
}

void pal_mutex_destroy(pal_mutex *m) { free(m); }
void pal_mutex_lock(pal_mutex *m) { AcquireSRWLockExclusive(&m->lock); }
bool pal_mutex_trylock(pal_mutex *m) { return TryAcquireSRWLockExclusive(&m->lock) != 0; }
void pal_mutex_unlock(pal_mutex *m) { ReleaseSRWLockExclusive(&m->lock); }

struct pal_cond {
    CONDITION_VARIABLE cv;
};

ps5c_err pal_cond_create(pal_cond **out) {
    pal_cond *c = (pal_cond *)malloc(sizeof(*c));
    if (!c) {
        return PS5C_ERR_NOMEM;
    }
    InitializeConditionVariable(&c->cv);
    *out = c;
    return PS5C_OK;
}

void pal_cond_destroy(pal_cond *c) { free(c); }

void pal_cond_wait(pal_cond *c, pal_mutex *m) {
    SleepConditionVariableSRW(&c->cv, &m->lock, INFINITE, 0);
}

ps5c_err pal_cond_wait_ns(pal_cond *c, pal_mutex *m, uint64_t timeout_ns) {
    uint64_t ms = (timeout_ns + 999999) / 1000000;
    DWORD wait = (DWORD)PS5C_MIN(ms, (uint64_t)(INFINITE - 1));
    if (!SleepConditionVariableSRW(&c->cv, &m->lock, wait, 0)) {
        return GetLastError() == ERROR_TIMEOUT ? PS5C_ERR_TIMEOUT : PS5C_ERR_OS;
    }
    return PS5C_OK;
}

void pal_cond_signal(pal_cond *c) { WakeConditionVariable(&c->cv); }
void pal_cond_broadcast(pal_cond *c) { WakeAllConditionVariable(&c->cv); }

/* ---------------------------------------------------------------------------
 * Thread-local slots
 * ------------------------------------------------------------------------- */

ps5c_err pal_tls_alloc(uint32_t *slot_out) {
    DWORD slot = TlsAlloc();
    if (slot == TLS_OUT_OF_INDEXES) {
        return PS5C_ERR_NOMEM;
    }
    *slot_out = slot;
    return PS5C_OK;
}

void pal_tls_free(uint32_t slot) { TlsFree(slot); }
void pal_tls_set(uint32_t slot, void *value) { TlsSetValue(slot, value); }
void *pal_tls_get(uint32_t slot) { return TlsGetValue(slot); }

uint32_t pal_tls_slot_gs_offset(uint32_t slot) {
    /* TEB64.TlsSlots[64] lives at gs:0x1480 (the layout TlsGetValue reads). */
    return slot < 64 ? 0x1480u + slot * 8u : 0;
}

/* NT_TIB.StackBase/StackLimit, plus TEB64.DeallocationStack at 0x1478. */
#define TEB_DEALLOCATION_STACK 0x1478u

void pal_thread_get_stack_bounds(pal_stack_bounds *out) {
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    out->low = (uint64_t)(uintptr_t)tib->StackLimit;
    out->high = (uint64_t)(uintptr_t)tib->StackBase;
    memcpy(&out->dealloc, (const uint8_t *)tib + TEB_DEALLOCATION_STACK, 8);
}

uint64_t pal_thread_cpu_ns(void) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    uint64_t kt = ((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime, ut = ((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime;
    return (kt + ut) * 100;
}

bool pal_thread_raise_priority(void) { return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != 0; }

bool pal_thread_prefer_fast_cores(void) {
    ULONG len = 0;
    GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    uint8_t *info = len ? malloc(len) : NULL;
    if (!info || !GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)info, len, &len, GetCurrentProcess(), 0)) {
        free(info);
        return false;
    }
    ULONG ids[256];
    uint32_t n = 0, total = 0;
    BYTE best = 0;
    for (int pass = 0; pass < 2; pass++) { /* the highest efficiency class is the fastest */
        for (ULONG o = 0; o < len;) {
            const SYSTEM_CPU_SET_INFORMATION *e = (const SYSTEM_CPU_SET_INFORMATION *)(info + o);
            if (e->Type == CpuSetInformation) {
                if (!pass) {
                    if (e->CpuSet.EfficiencyClass > best) best = e->CpuSet.EfficiencyClass;
                    total++;
                } else if (e->CpuSet.EfficiencyClass == best && n < 256) {
                    ids[n++] = e->CpuSet.Id;
                }
            }
            o += e->Size;
        }
    }
    free(info);
    return n && n < total && SetThreadSelectedCpuSets(GetCurrentThread(), ids, n);
}

bool pal_process_full_speed(void) {
    PROCESS_POWER_THROTTLING_STATE s = {.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                        .ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
                                                       PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION,
                                        .StateMask = 0};
    return SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s)) != 0;
}

void pal_thread_set_stack_bounds(const pal_stack_bounds *b) {
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    tib->StackLimit = (void *)(uintptr_t)b->low;
    tib->StackBase = (void *)(uintptr_t)b->high;
    memcpy((uint8_t *)tib + TEB_DEALLOCATION_STACK, &b->dealloc, 8);
}

/* ---------------------------------------------------------------------------
 * Faults
 * ------------------------------------------------------------------------- */

static pal_fault_fn g_fault_fn;
static void *g_fault_user;

/* The upper halves of ymm0-15 in a fault's context (XSTATE_AVX), NULL when
 * the context does not carry them. A clear AVX bit in the context's feature
 * mask means the halves are zero (the area is not filled): they are zeroed
 * and the bit set, so that what the handler writes is restored. */
typedef PVOID(WINAPI *xstate_locate_fn)(PCONTEXT, DWORD, PDWORD);
typedef BOOL(WINAPI *xstate_get_mask_fn)(PCONTEXT, PDWORD64);
typedef BOOL(WINAPI *xstate_set_mask_fn)(PCONTEXT, DWORD64);
static M128A *ymm_upper(CONTEXT *ctx) {
    static xstate_locate_fn locate;
    static xstate_get_mask_fn get_mask;
    static xstate_set_mask_fn set_mask;
    static int init;
    if (!init) {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        FARPROC f = k ? GetProcAddress(k, "LocateXStateFeature") : NULL;
        memcpy(&locate, &f, sizeof(locate));
        f = k ? GetProcAddress(k, "GetXStateFeaturesMask") : NULL;
        memcpy(&get_mask, &f, sizeof(get_mask));
        f = k ? GetProcAddress(k, "SetXStateFeaturesMask") : NULL;
        memcpy(&set_mask, &f, sizeof(set_mask));
        init = 1;
    }
    if (!locate || !get_mask || !set_mask || (ctx->ContextFlags & 0x00100040) != 0x00100040) return NULL;
    DWORD len = 0;
    M128A *p = (M128A *)locate(ctx, 2 /* XSTATE_AVX */, &len);
    DWORD64 mask = 0;
    if (!p || len < 16 * sizeof(M128A) || !get_mask(ctx, &mask)) return NULL;
    if (!(mask & 4)) {
        memset(p, 0, 16 * sizeof(M128A));
        if (!set_mask(ctx, mask | 4)) return NULL;
    }
    return p;
}

static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *ctx = ep->ContextRecord;
    pal_fault_info info;
    memset(&info, 0, sizeof(info));
    info.native_code = er->ExceptionCode;
    switch (er->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        info.kind = PAL_FAULT_ACCESS;
        info.write = er->NumberParameters > 0 && er->ExceptionInformation[0] == 1;
        info.execute = er->NumberParameters > 0 && er->ExceptionInformation[0] == 8;
        info.addr = er->NumberParameters > 1 ? er->ExceptionInformation[1] : 0;
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION: info.kind = PAL_FAULT_ILLEGAL_INSTRUCTION; break;
    case EXCEPTION_BREAKPOINT: info.kind = PAL_FAULT_BREAKPOINT; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: info.kind = PAL_FAULT_DIVIDE; break;
    case EXCEPTION_SINGLE_STEP: info.kind = PAL_FAULT_SINGLE_STEP; break;
    default:
        /* C++ exceptions, debugger events, stack overflow probes...: not ours. */
        return EXCEPTION_CONTINUE_SEARCH;
    }
    info.pc = ctx->Rip;
    info.rsp = ctx->Rsp;
    const DWORD64 *regs[16] = {&ctx->Rax, &ctx->Rcx, &ctx->Rdx, &ctx->Rbx, &ctx->Rsp, &ctx->Rbp,
                               &ctx->Rsi, &ctx->Rdi, &ctx->R8,  &ctx->R9,  &ctx->R10, &ctx->R11,
                               &ctx->R12, &ctx->R13, &ctx->R14, &ctx->R15};
    for (int i = 0; i < 16; i++) info.gpr[i] = *regs[i];
    info.rflags = ctx->EFlags;
    M128A *xmm = &ctx->Xmm0;
    for (int i = 0; i < 16; i++) {
        info.xmm[i][0] = xmm[i].Low;
        info.xmm[i][1] = (uint64_t)xmm[i].High;
    }
    M128A *ymm_hi = info.kind == PAL_FAULT_ILLEGAL_INSTRUCTION ? ymm_upper(ctx) : NULL;
    if (ymm_hi) {
        info.has_ymm_hi = true;
        for (int i = 0; i < 16; i++) {
            info.ymm_hi[i][0] = ymm_hi[i].Low;
            info.ymm_hi[i][1] = (uint64_t)ymm_hi[i].High;
        }
    }

    if (g_fault_fn && g_fault_fn(g_fault_user, &info)) {
        ctx->Rip = info.pc;
        for (int i = 0; i < 16; i++)
            if (i != 4) *(DWORD64 *)regs[i] = info.gpr[i];
        ctx->EFlags = (DWORD)info.rflags;
        if (info.single_step) ctx->EFlags |= 0x100; /* TF: trap after one instruction */
        for (int i = 0; i < 16; i++) {
            xmm[i].Low = info.xmm[i][0];
            xmm[i].High = (LONGLONG)info.xmm[i][1];
            if (ymm_hi) {
                ymm_hi[i].Low = info.ymm_hi[i][0];
                ymm_hi[i].High = (LONGLONG)info.ymm_hi[i][1];
            }
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

ps5c_err pal_fault_handler_install(pal_fault_fn fn, void *user) {
    g_fault_fn = fn;
    g_fault_user = user;
    static PVOID s_handle;
    if (!s_handle) {
        s_handle = AddVectoredExceptionHandler(1, vectored_handler);
    }
    return s_handle ? PS5C_OK : PS5C_ERR_OS;
}

void pal_exit_process(int code) {
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), (UINT)code);
}

/* ---------------------------------------------------------------------------
 * Files
 * ------------------------------------------------------------------------- */

typedef struct native_read_identity {
    FILE_ID_INFO id;
    BY_HANDLE_FILE_INFORMATION info;
} native_read_identity;
struct pal_file {
    exfat_file *image;
    HANDLE handle;
    wchar_t *read_path; /* absolute path, only for immutable read-only recovery */
    native_read_identity read_identity;
};

static bool file_read_identity(HANDLE handle,native_read_identity *out) {
    return GetFileType(handle)==FILE_TYPE_DISK &&
        GetFileInformationByHandle(handle,&out->info) && !(out->info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) &&
        GetFileInformationByHandleEx(handle,FileIdInfo,&out->id,sizeof(out->id));
}
static bool file_same_read_identity(const native_read_identity *a,const native_read_identity *b) {
    return a->id.VolumeSerialNumber==b->id.VolumeSerialNumber &&
        !memcmp(&a->id.FileId,&b->id.FileId,sizeof(a->id.FileId)) &&
        a->info.nFileSizeHigh==b->info.nFileSizeHigh && a->info.nFileSizeLow==b->info.nFileSizeLow &&
        a->info.ftLastWriteTime.dwHighDateTime==b->info.ftLastWriteTime.dwHighDateTime &&
        a->info.ftLastWriteTime.dwLowDateTime==b->info.ftLastWriteTime.dwLowDateTime;
}
static wchar_t *file_absolute_path(const wchar_t *path) {
    DWORD size=GetFullPathNameW(path,0,NULL,NULL);if(!size)return NULL;
    wchar_t *absolute=malloc((size_t)size*sizeof(*absolute));if(!absolute)return NULL;
    DWORD written=GetFullPathNameW(path,size,absolute,NULL);
    if(!written || written>=size){free(absolute);return NULL;}
    return absolute;
}

static ps5c_err map_file_error(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return PS5C_ERR_NOT_FOUND;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION: return PS5C_ERR_BUSY;
    case ERROR_NOT_ENOUGH_MEMORY: return PS5C_ERR_NOMEM;
    default: return PS5C_ERR_IO;
    }
}

typedef struct native_file_view { HANDLE file, mapping; } native_file_view;

void pal_file_view_close(pal_file_view *view) {
    if (!view) return;
    native_file_view *v = (native_file_view *)view->opaque;
    if (v) {
        if (view->data) UnmapViewOfFile(view->data);
        if (v->mapping) CloseHandle(v->mapping);
        CloseHandle(v->file);
        free(v);
    }
    memset(view, 0, sizeof(*view));
}

ps5c_err pal_file_view_open(const char *path, pal_file_view *out) {
    if (!path || !out) return PS5C_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    if (exfat_path(path)) return PS5C_ERR_UNSUPPORTED;
    wchar_t *wpath = utf8_to_wide(path);
    if (!wpath) return PS5C_ERR_INVALID;
    HANDLE file = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD e = GetLastError();
    free(wpath);
    if (file == INVALID_HANDLE_VALUE) return map_file_error(e);
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)) {
        e = GetLastError(); CloseHandle(file); return map_file_error(e);
    }
    if (size.QuadPart < 0 || (uint64_t)size.QuadPart > SIZE_MAX) {
        CloseHandle(file); return PS5C_ERR_OUT_OF_RANGE;
    }
    native_file_view *v = (native_file_view *)calloc(1, sizeof(*v));
    if (!v) { CloseHandle(file); return PS5C_ERR_NOMEM; }
    v->file = file;
    out->opaque = v;
    out->size = (uint64_t)size.QuadPart;
    if (!out->size) return PS5C_OK;
    v->mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (v->mapping) out->data = MapViewOfFile(v->mapping, FILE_MAP_READ, 0, 0, 0);
    if (!out->data) {
        e = GetLastError(); pal_file_view_close(out); return map_file_error(e);
    }
    return PS5C_OK;
}

ps5c_err pal_file_open(const char *path, uint32_t flags, pal_file **out) {
    if (exfat_path(path)) {
        pal_file *f = (pal_file *)calloc(1, sizeof(*f));
        if (!f) return PS5C_ERR_NOMEM;
        ps5c_err err = exfat_open(path, flags, &f->image);
        if (err) { free(f); return err; }
        *out = f; return PS5C_OK;
    }
    DWORD access = 0;
    if (flags & PAL_FILE_READ) access |= GENERIC_READ;
    if (flags & PAL_FILE_WRITE) access |= GENERIC_WRITE;
    if (access == 0) {
        return PS5C_ERR_INVALID;
    }

    DWORD disposition;
    if ((flags & PAL_FILE_CREATE) && (flags & PAL_FILE_TRUNCATE)) disposition = CREATE_ALWAYS;
    else if (flags & PAL_FILE_CREATE) disposition = OPEN_ALWAYS;
    else if (flags & PAL_FILE_TRUNCATE) disposition = TRUNCATE_EXISTING;
    else disposition = OPEN_EXISTING;

    wchar_t *wpath = utf8_to_wide(path);
    if (!wpath) {
        return PS5C_ERR_INVALID;
    }
    wchar_t *absolute=flags==(PAL_FILE_READ|PAL_FILE_RECOVER_READ)?file_absolute_path(wpath):NULL;
    HANDLE h = CreateFileW(absolute?absolute:wpath, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD err = GetLastError();
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) {
        free(absolute);
        return map_file_error(err);
    }

    pal_file *f = (pal_file *)calloc(1,sizeof(*f));
    if (!f) {
        CloseHandle(h);
        free(absolute);
        return PS5C_ERR_NOMEM;
    }
    f->image = NULL;
    f->handle = h;
    if(absolute && file_read_identity(h,&f->read_identity))f->read_path=absolute;
    else free(absolute);
    *out = f;
    return PS5C_OK;
}

void pal_file_close(pal_file *f) {
    if (f && f->image) { exfat_close(f->image); free(f); return; }
    if (f) {
        CloseHandle(f->handle);
        free(f->read_path);
        free(f);
    }
}

#define FILE_IO_CHUNK (1u << 30)

static void (*g_write_prepare)(void *buf, uint64_t size);
void pal_set_write_prepare(void (*fn)(void *buf, uint64_t size)) { g_write_prepare = fn; }

ps5c_err pal_file_read(pal_file *f, void *buf, uint64_t size, uint64_t *read_out) {
    if (f->image) return exfat_read_current(f->image, buf, size, read_out);
    uint64_t total = 0;
    uint32_t noaccess_retries = 0;
    while (total < size) {
        DWORD want = (DWORD)PS5C_MIN(size - total, (uint64_t)FILE_IO_CHUNK);
        DWORD got = 0;
        if (g_write_prepare) g_write_prepare((uint8_t *)buf + total, want);
        if (!ReadFile(f->handle, (uint8_t *)buf + total, want, &got, NULL)) {
            /* a page protected again between the preparation and the read: once more */
            if (GetLastError() == ERROR_NOACCESS && g_write_prepare && noaccess_retries++ < 8) continue;
            return map_file_error(GetLastError());
        }
        if (got == 0) {
            break; /* end of file */
        }
        total += got;
    }
    if (read_out) {
        *read_out = total;
    }
    return PS5C_OK;
}

ps5c_err pal_file_write(pal_file *f, const void *buf, uint64_t size, uint64_t *written_out) {
    if (f->image) { if (written_out) *written_out = 0; return PS5C_ERR_BUSY; }
    uint64_t total = 0;
    while (total < size) {
        DWORD want = (DWORD)PS5C_MIN(size - total, (uint64_t)FILE_IO_CHUNK);
        DWORD put = 0;
        if (!WriteFile(f->handle, (const uint8_t *)buf + total, want, &put, NULL)) {
            return map_file_error(GetLastError());
        }
        total += put;
    }
    if (written_out) {
        *written_out = total;
    }
    return PS5C_OK;
}

#ifndef PS5C_WIN32_PREAD_READ
#define PS5C_WIN32_PREAD_READ ReadFile
#endif
/* A transient device loss can leave a cached read handle unusable. Retry once
 * with a LOCAL handle, preserving concurrent callers' handle and position.
 * Reject writers and any change of identity/size/time; never retry writes. */
static bool file_recover_pread(pal_file *f,void *buf,DWORD want,uint64_t offset,DWORD *got,DWORD cause) {
    if(cause!=ERROR_DEV_NOT_EXIST || !f->read_path)return false;
    HANDLE local=CreateFileW(f->read_path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,
                             NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(local==INVALID_HANDLE_VALUE)return false;
    native_read_identity before,after;bool ok=false;
    if(file_read_identity(local,&before) && file_same_read_identity(&before,&f->read_identity)) {
        OVERLAPPED ov={0};ov.Offset=(DWORD)offset;ov.OffsetHigh=(DWORD)(offset>>32);DWORD read=0;
        bool completed=PS5C_WIN32_PREAD_READ(local,buf,want,&read,&ov)!=0;
        if(!completed && GetLastError()==ERROR_HANDLE_EOF){read=0;completed=true;}
        if(completed && file_read_identity(local,&after) && file_same_read_identity(&after,&f->read_identity)) {
            *got=read;ok=true;
        }
    }
    CloseHandle(local);
    if(ok)LOG_W(FS,"positional read recovered after Windows error %lu using a verified read-only handle",(unsigned long)cause);
    return ok;
}
ps5c_err pal_file_pread(pal_file *f, void *buf, uint64_t size, uint64_t offset, uint64_t *read_out) {
    if (f->image) return exfat_read(f->image, buf, size, offset, read_out);
    uint64_t total = 0;
    bool recovery_attempted=false;
    uint32_t noaccess_retries = 0;
    while (total < size) {
        DWORD want = (DWORD)PS5C_MIN(size - total, (uint64_t)FILE_IO_CHUNK);
        DWORD got = 0;
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        uint64_t at = offset + total;
        ov.Offset = (DWORD)at;
        ov.OffsetHigh = (DWORD)(at >> 32);
        if (g_write_prepare) g_write_prepare((uint8_t *)buf + total, want);
        if (!PS5C_WIN32_PREAD_READ(f->handle, (uint8_t *)buf + total, want, &got, &ov)) {
            DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF) break;
            if (e == ERROR_NOACCESS && g_write_prepare && noaccess_retries++ < 8) continue; /* protected again meanwhile */
            bool recovered=false;
            if(!recovery_attempted && e==ERROR_DEV_NOT_EXIST) {
                recovery_attempted=true;
                recovered=file_recover_pread(f,(uint8_t*)buf+total,want,at,&got,e);
            }
            if(recovered) {
                if(!got)break;
                total+=got;continue;
            }
            LOG_W(FS, "positional read failed: Windows error %lu, offset 0x%llx size 0x%lx destination %p",
                  (unsigned long)e, (unsigned long long)at, (unsigned long)want, (uint8_t *)buf + total);
            return map_file_error(e);
        }
        if (got == 0) break;
        total += got;
    }
    if (read_out) *read_out = total;
    return PS5C_OK;
}

ps5c_err pal_file_pwrite(pal_file *f, const void *buf, uint64_t size, uint64_t offset, uint64_t *written_out) {
    if (f->image) { if (written_out) *written_out = 0; return PS5C_ERR_BUSY; }
    uint64_t total = 0;
    while (total < size) {
        DWORD want = (DWORD)PS5C_MIN(size - total, (uint64_t)FILE_IO_CHUNK);
        DWORD put = 0;
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        uint64_t at = offset + total;
        ov.Offset = (DWORD)at;
        ov.OffsetHigh = (DWORD)(at >> 32);
        if (!WriteFile(f->handle, (const uint8_t *)buf + total, want, &put, &ov)) return map_file_error(GetLastError());
        if (put == 0) break;
        total += put;
    }
    if (written_out) *written_out = total;
    return PS5C_OK;
}

ps5c_err pal_file_seek(pal_file *f, int64_t offset, int whence, uint64_t *pos_out) {
    if (f->image) return exfat_seek(f->image, offset, whence, pos_out);
    DWORD method = whence == PAL_SEEK_CUR ? FILE_CURRENT : whence == PAL_SEEK_END ? FILE_END : FILE_BEGIN;
    LARGE_INTEGER dist, pos;
    dist.QuadPart = offset;
    if (!SetFilePointerEx(f->handle, dist, &pos, method)) {
        return PS5C_ERR_INVALID;
    }
    if (pos_out) {
        *pos_out = (uint64_t)pos.QuadPart;
    }
    return PS5C_OK;
}

ps5c_err pal_file_size(pal_file *f, uint64_t *size_out) {
    if (f->image) { *size_out = exfat_size(f->image); return PS5C_OK; }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(f->handle, &size)) {
        return map_file_error(GetLastError());
    }
    *size_out = (uint64_t)size.QuadPart;
    return PS5C_OK;
}

ps5c_err pal_file_truncate(pal_file *f, uint64_t size) {
    if (f->image) return PS5C_ERR_BUSY;
    FILE_END_OF_FILE_INFO info;
    info.EndOfFile.QuadPart = (LONGLONG)size;
    if (!SetFileInformationByHandle(f->handle, FileEndOfFileInfo, &info, sizeof(info))) return map_file_error(GetLastError());
    return PS5C_OK;
}

/* FILETIME (100 ns since 1601) -> Unix time */
static void filetime_to_unix(FILETIME ft, int64_t *sec, uint32_t *nsec) {
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ull;
    *sec = (int64_t)(t / 10000000u);
    *nsec = (uint32_t)(t % 10000000u) * 100u;
}

ps5c_err pal_path_stat(const char *path, pal_path_info *out) {
    if (exfat_path(path)) return exfat_stat(path, out);
    wchar_t *wpath = utf8_to_wide(path);
    if (!wpath) return PS5C_ERR_INVALID;
    WIN32_FILE_ATTRIBUTE_DATA data;
    BOOL ok = GetFileAttributesExW(wpath, GetFileExInfoStandard, &data);
    DWORD err = GetLastError();
    free(wpath);
    if (!ok) return map_file_error(err);
    out->is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out->size = out->is_dir ? 0 : ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    filetime_to_unix(data.ftLastWriteTime, &out->mtime_sec, &out->mtime_nsec);
    return PS5C_OK;
}

ps5c_err pal_mkdir(const char *path) {
    if (exfat_path(path)) return PS5C_ERR_BUSY;
    wchar_t *wpath = utf8_to_wide(path);
    if (!wpath) return PS5C_ERR_INVALID;
    BOOL ok = CreateDirectoryW(wpath, NULL);
    DWORD err = GetLastError();
    free(wpath);
    if (ok) return PS5C_OK;
    return err == ERROR_ALREADY_EXISTS ? PS5C_ERR_BUSY : map_file_error(err);
}

ps5c_err pal_dir_list(const char *path, pal_dir_fn fn, void *user) {
    if (exfat_path(path)) return exfat_list(path, fn, user);
    size_t n = strlen(path);
    char *pattern = (char *)malloc(n + 3);
    if (!pattern) return PS5C_ERR_NOMEM;
    memcpy(pattern, path, n);
    memcpy(pattern + n, "\\*", 3);
    wchar_t *wpattern = utf8_to_wide(pattern);
    free(pattern);
    if (!wpattern) return PS5C_ERR_INVALID;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpattern, &fd);
    DWORD err = GetLastError();
    free(wpattern);
    if (h == INVALID_HANDLE_VALUE) return err == ERROR_FILE_NOT_FOUND ? PS5C_OK : map_file_error(err);
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        char name[1024];
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name), NULL, NULL) <= 0) continue;
        if (!fn(user, name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return PS5C_OK;
}

ps5c_err pal_rmdir(const char *path) {
    if (exfat_path(path)) return PS5C_ERR_BUSY;
    wchar_t *wpath = utf8_to_wide(path);
    if (!wpath) return PS5C_ERR_INVALID;
    BOOL ok = RemoveDirectoryW(wpath);
    DWORD err = GetLastError();
    free(wpath);
    return ok ? PS5C_OK : map_file_error(err);
}

ps5c_err pal_file_rename(const char *from, const char *to) {
    if (exfat_path(from) || exfat_path(to)) return PS5C_ERR_BUSY;
    wchar_t *wfrom = utf8_to_wide(from), *wto = utf8_to_wide(to);
    BOOL ok = wfrom && wto && MoveFileExW(wfrom, wto, MOVEFILE_REPLACE_EXISTING);
    DWORD err = GetLastError();
    free(wfrom);
    free(wto);
    if (!wfrom || !wto) return PS5C_ERR_INVALID;
    return ok ? PS5C_OK : map_file_error(err);
}

ps5c_err pal_file_remove(const char *path) {
    if (exfat_path(path)) return PS5C_ERR_BUSY;
    wchar_t *wpath = utf8_to_wide(path);
    if (!wpath) {
        return PS5C_ERR_INVALID;
    }
    BOOL ok = DeleteFileW(wpath);
    DWORD err = GetLastError();
    free(wpath);
    return ok ? PS5C_OK : map_file_error(err);
}

/* ---------------------------------------------------------------------------
 * Window
 * ------------------------------------------------------------------------- */

struct pal_window {
    HANDLE thread;
    HANDLE ready;
    HWND hwnd;
    /* Triple buffering: the presenter fills slot back_i, the UI thread draws
     * front_i, ready_i is the latest complete frame. `lock` guards the swap. */
    CRITICAL_SECTION lock;
    struct {
        uint8_t *px; /* tightly packed BGRX */
        uint32_t w, h;
    } slot[3];
    int back_i, ready_i, front_i;
    bool fresh; /* ready_i holds a frame the UI has not taken yet */
    volatile LONG closed;
    volatile LONG external; /* a GPU presenter owns the client area */
    volatile LONG mode;     /* PAL_WINDOW_*; changed on the UI thread only */
    wchar_t *title;
    uint32_t width, height;
};

static const wchar_t k_window_class[] = L"PS5CWindow";
/* Retain short F10 presses while the renderer is busy with a slow frame. */
static volatile LONG g_panel_key_presses;
#define WM_PAL_MODE (WM_APP + 1) /* wParam PAL_WINDOW_*, lParam the HMONITOR to show on (NULL: the current one) */

/* The window rectangle whose client area keeps the frame's aspect and fits in 90% of
 * the work area, centered */
static RECT fitted_rect(const pal_window *w, RECT work) {
    double fit_w = 0.9 * (work.right - work.left), fit_h = 0.9 * (work.bottom - work.top) - 40, scale = 1.0;
    if (w->width > fit_w) scale = fit_w / w->width;
    if (w->height * scale > fit_h) scale = fit_h / w->height;
    RECT r = {0, 0, (LONG)(w->width * scale), (LONG)(w->height * scale)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    LONG win_w = r.right - r.left, win_h = r.bottom - r.top;
    LONG x = work.left + ((work.right - work.left) - win_w) / 2, y = work.top + ((work.bottom - work.top) - win_h) / 2;
    if (y < work.top) y = work.top;
    RECT out = {x, y, x + win_w, y + win_h};
    return out;
}

/* On the UI thread: full screen is a borderless window over the whole display (the
 * swapchain follows the new client size) */
static void apply_mode(pal_window *w, int mode, HMONITOR mon) {
    HWND h = w->hwnd;
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!mon) mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(mon, &mi)) return;
    InterlockedExchange(&w->mode, mode);
    if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
    if (mode == PAL_WINDOW_FULLSCREEN) {
        const RECT *m = &mi.rcMonitor;
        SetWindowLongPtrW(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, m->left, m->top, m->right - m->left, m->bottom - m->top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        return;
    }
    RECT r = fitted_rect(w, mi.rcWork);
    SetWindowLongPtrW(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    SetWindowPos(h, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    if (mode == PAL_WINDOW_MAXIMIZED) ShowWindow(h, SW_MAXIMIZE);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    pal_window *w = (pal_window *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp); /* messages of CreateWindowEx */
    if (native_win32_message(hwnd, msg, wp, lp)) return msg == WM_SETCURSOR ? TRUE : 0;
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wp == VK_F10) {
        if (!(lp & ((LPARAM)1 << 30))) InterlockedIncrement(&g_panel_key_presses);
        return 0;
    }
    switch (msg) {
    case WM_PAL_MODE: apply_mode(w, (int)wp, (HMONITOR)lp); return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && w->mode == PAL_WINDOW_FULLSCREEN) {
            apply_mode(w, PAL_WINDOW_NORMAL, NULL);
            return 0;
        }
        if (wp == VK_F11 && !(lp & (1 << 30))) { /* not the key's auto-repeat */
            apply_mode(w, w->mode == PAL_WINDOW_FULLSCREEN ? PAL_WINDOW_NORMAL : PAL_WINDOW_FULLSCREEN, NULL);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_SETCURSOR:
        if (w->mode == PAL_WINDOW_FULLSCREEN && LOWORD(lp) == HTCLIENT) {
            SetCursor(NULL);
            return TRUE;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        if (InterlockedCompareExchange(&w->external, 0, 0)) {
            ValidateRect(hwnd, NULL);
            return 0;
        }
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        EnterCriticalSection(&w->lock);
        if (w->fresh) {
            int t = w->front_i;
            w->front_i = w->ready_i;
            w->ready_i = t;
            w->fresh = false;
        }
        LeaveCriticalSection(&w->lock);
        const uint8_t *frame = w->slot[w->front_i].px;
        uint32_t frame_w = w->slot[w->front_i].w, frame_h = w->slot[w->front_i].h;
        if (frame && frame_w && frame_h) {
            BITMAPINFO bi;
            memset(&bi, 0, sizeof(bi));
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = (LONG)frame_w;
            bi.bmiHeader.biHeight = -(LONG)frame_h; /* top-down */
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, (int)frame_w, (int)frame_h, frame, &bi,
                          DIB_RGB_COLORS, SRCCOPY);
        } else {
            FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        InterlockedExchange(&w->closed, 1);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static DWORD WINAPI window_thread(LPVOID arg) {
    pal_window *w = (pal_window *)arg;
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); /* the ZeraPS5 icon, when the executable has one */
    wc.lpszClassName = k_window_class;
    RegisterClassExW(&wc); /* fails harmlessly if already registered */

    RECT work = {0, 0, 1920, 1080};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT r = fitted_rect(w, work);
    w->hwnd = CreateWindowExW(0, k_window_class, w->title, WS_OVERLAPPEDWINDOW, r.left, r.top, r.right - r.left,
                              r.bottom - r.top, NULL, NULL, inst, NULL);
    if (w->hwnd) {
        SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);
        native_win32_open(w->hwnd);
        ShowWindow(w->hwnd, SW_SHOWNORMAL);
    }
    SetEvent(w->ready);
    if (!w->hwnd) return 1;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    InterlockedExchange(&w->closed, 1);
    return 0;
}

ps5c_err pal_window_open(const char *title, uint32_t width, uint32_t height, pal_window **out) {
    pal_window *w = (pal_window *)calloc(1, sizeof(*w));
    if (!w) return PS5C_ERR_NOMEM;
    w->title = utf8_to_wide(title ? title : "PS5C");
    w->width = width ? width : 1280;
    w->height = height ? height : 720;
    InitializeCriticalSection(&w->lock);
    w->back_i = 0, w->ready_i = 1, w->front_i = 2;
    w->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    w->thread = w->ready ? CreateThread(NULL, 0, window_thread, w, 0, NULL) : NULL;
    if (!w->thread) {
        if (w->ready) CloseHandle(w->ready);
        DeleteCriticalSection(&w->lock);
        free(w->title);
        free(w);
        return PS5C_ERR_OS;
    }
    WaitForSingleObject(w->ready, INFINITE);
    if (!w->hwnd) {
        WaitForSingleObject(w->thread, INFINITE);
        CloseHandle(w->thread);
        CloseHandle(w->ready);
        DeleteCriticalSection(&w->lock);
        free(w->title);
        free(w);
        return PS5C_ERR_OS;
    }
    *out = w;
    return PS5C_OK;
}

void pal_window_present(pal_window *w, const void *pixels, uint32_t width, uint32_t height, uint32_t pitch_bytes) {
    if (!w || !pixels || !width || !height) return;
    /* Only the presenting thread touches back_i: no lock while copying. */
    int b = w->back_i;
    if (w->slot[b].w != width || w->slot[b].h != height) {
        free(w->slot[b].px);
        w->slot[b].px = (uint8_t *)malloc((size_t)width * height * 4);
        w->slot[b].w = w->slot[b].px ? width : 0;
        w->slot[b].h = w->slot[b].px ? height : 0;
    }
    if (!w->slot[b].px) return;
    for (uint32_t y = 0; y < height; y++) {
        memcpy(w->slot[b].px + (size_t)y * width * 4, (const uint8_t *)pixels + (size_t)y * pitch_bytes,
               (size_t)width * 4);
    }
    EnterCriticalSection(&w->lock);
    w->back_i = w->ready_i;
    w->ready_i = b;
    w->fresh = true;
    LeaveCriticalSection(&w->lock);
    InvalidateRect(w->hwnd, NULL, FALSE);
}

void pal_window_native(pal_window *w, void **display, void **handle) {
    *display = (void *)GetModuleHandleW(NULL);
    *handle = w ? (void *)w->hwnd : NULL;
}

void pal_window_client_size(pal_window *w, uint32_t *width, uint32_t *height) {
    RECT rc = {0, 0, 0, 0};
    if (w && w->hwnd) GetClientRect(w->hwnd, &rc);
    *width = (uint32_t)(rc.right - rc.left);
    *height = (uint32_t)(rc.bottom - rc.top);
}

/* ---- Displays per GPU (DXGI outputs of each adapter) ---- */

typedef HRESULT(WINAPI *create_factory_fn)(REFIID, void **);
static const GUID k_iid_factory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};

static IDXGIFactory1 *dxgi_factory(void) {
    static HMODULE lib;
    if (!lib) lib = LoadLibraryW(L"dxgi.dll");
    FARPROC proc = lib ? GetProcAddress(lib, "CreateDXGIFactory1") : NULL;
    create_factory_fn create = NULL;
    if (proc) memcpy(&create, &proc, sizeof(create)); /* no function/object pointer casts in ISO C */
    IDXGIFactory1 *f = NULL;
    if (!create || FAILED(create(&k_iid_factory1, (void **)&f))) return NULL;
    return f;
}

typedef bool (*output_visit)(void *user, const DXGI_ADAPTER_DESC1 *a, const DXGI_OUTPUT_DESC *o);

/* Calls visit for every display attached to the desktop, until it returns false */
static void each_output(output_visit visit, void *user) {
    IDXGIFactory1 *f = dxgi_factory();
    if (!f) return;
    bool go = true;
    IDXGIAdapter1 *a;
    for (UINT i = 0; go && IDXGIFactory1_EnumAdapters1(f, i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 ad;
        IDXGIOutput *o;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &ad))) {
            for (UINT k = 0; go && IDXGIAdapter1_EnumOutputs(a, k, &o) == S_OK; k++) {
                DXGI_OUTPUT_DESC od;
                if (SUCCEEDED(IDXGIOutput_GetDesc(o, &od)) && od.AttachedToDesktop) go = visit(user, &ad, &od);
                IDXGIOutput_Release(o);
            }
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
}

typedef struct display_list {
    char *out;
    size_t cap, len;
} display_list;

static bool list_visit(void *user, const DXGI_ADAPTER_DESC1 *a, const DXGI_OUTPUT_DESC *o) {
    display_list *l = (display_list *)user;
    char dev[64], gpu[128];
    WideCharToMultiByte(CP_UTF8, 0, o->DeviceName, -1, dev, sizeof(dev), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, a->Description, -1, gpu, sizeof(gpu), NULL, NULL);
    const RECT *r = &o->DesktopCoordinates;
    if (l->len < l->cap) {
        int n = snprintf(l->out + l->len, l->cap - l->len, "%s %ldx%ld at (%ld,%ld) on %s\n", dev, r->right - r->left,
                         r->bottom - r->top, r->left, r->top, gpu);
        if (n > 0) l->len += (size_t)n < l->cap - l->len ? (size_t)n : l->cap - l->len - 1;
    }
    return true;
}

size_t pal_display_list(char *out, size_t cap) {
    display_list l = {out, cap, 0};
    if (cap) out[0] = 0;
    each_output(list_visit, &l);
    return l.len;
}

typedef struct gpu_display {
    uint8_t luid[8];
    HMONITOR current; /* the window's display */
    bool on_it, found;
    HMONITOR target;
} gpu_display;

static bool gpu_visit(void *user, const DXGI_ADAPTER_DESC1 *a, const DXGI_OUTPUT_DESC *o) {
    gpu_display *g = (gpu_display *)user;
    if (memcmp(&a->AdapterLuid, g->luid, 8) != 0) return true;
    if (o->Monitor == g->current) g->on_it = true;
    if (!g->found) g->target = o->Monitor, g->found = true;
    return true;
}

int pal_window_move_to_gpu(pal_window *w, const uint8_t luid[8]) {
    if (!w || !w->hwnd) return -1;
    gpu_display g;
    memset(&g, 0, sizeof(g));
    memcpy(g.luid, luid, 8);
    g.current = MonitorFromWindow(w->hwnd, MONITOR_DEFAULTTONEAREST);
    each_output(gpu_visit, &g);
    if (g.on_it) return 0;
    if (!g.found) return -1;
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(g.target, &mi)) return -1;
    /* the same mode on that display: fitted as at creation, maximized or over all of it */
    SendMessageW(w->hwnd, WM_PAL_MODE, (WPARAM)InterlockedCompareExchange(&w->mode, 0, 0), (LPARAM)g.target);
    return 1;
}

void pal_window_set_external_present(pal_window *w, bool on) {
    if (w) InterlockedExchange(&w->external, on ? 1 : 0);
}

void pal_window_set_mode(pal_window *w, int mode) {
    if (!w || !w->hwnd || pal_window_closed(w)) return;
    if (mode != PAL_WINDOW_FULLSCREEN && mode != PAL_WINDOW_MAXIMIZED) mode = PAL_WINDOW_NORMAL;
    if (mode == InterlockedCompareExchange(&w->mode, 0, 0)) return;
    SendMessageW(w->hwnd, WM_PAL_MODE, (WPARAM)mode, 0); /* the UI thread owns the window */
}

pal_library *pal_library_open(const char *name) {
    wchar_t *wide = utf8_to_wide(name);
    HMODULE m = wide ? LoadLibraryW(wide) : NULL;
    free(wide);
    return (pal_library *)m;
}

pal_proc pal_library_symbol(pal_library *lib, const char *name) {
    return lib ? (pal_proc)(void (*)(void))GetProcAddress((HMODULE)lib, name) : NULL;
}

void pal_window_set_title(pal_window *w, const char *title) {
    if (!w || !title || pal_window_closed(w)) return;
    wchar_t *wide = utf8_to_wide(title);
    if (wide) SetWindowTextW(w->hwnd, wide);
    free(wide);
}

bool pal_window_closed(pal_window *w) { return w && InterlockedCompareExchange(&w->closed, 0, 0) != 0; }

void pal_window_destroy(pal_window *w) {
    if (!w) return;
    if (!pal_window_closed(w)) PostMessageW(w->hwnd, WM_CLOSE, 0, 0);
    WaitForSingleObject(w->thread, INFINITE);
    CloseHandle(w->thread);
    CloseHandle(w->ready);
    DeleteCriticalSection(&w->lock);
    for (int i = 0; i < 3; i++) free(w->slot[i].px);
    free(w->title);
    free(w);
}

/* ---------------------------------------------------------------------------
 * Input: PlayStation controllers (HID, dualsense.c), XInput gamepads (loaded
 * at runtime) and the keyboard
 * ------------------------------------------------------------------------- */

typedef struct {
    WORD buttons;
    BYTE left_trigger, right_trigger;
    SHORT lx, ly, rx, ry;
} xinput_gamepad;
typedef struct {
    DWORD packet;
    xinput_gamepad pad;
} xinput_state;
typedef DWORD(WINAPI *xinput_get_state_fn)(DWORD, xinput_state *);
typedef struct {
    WORD left, right;
} xinput_vibration;
typedef DWORD(WINAPI *xinput_set_state_fn)(DWORD, xinput_vibration *);

static INIT_ONCE g_xinput_once = INIT_ONCE_STATIC_INIT;
static xinput_get_state_fn g_xinput_get_state;
static xinput_set_state_fn g_xinput_set_state;
static volatile LONG g_xinput_slot = -1;       /* last connected user index */
static volatile LONGLONG g_xinput_next_scan;   /* qpc time of the next scan while none is connected */

static BOOL CALLBACK xinput_load(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    PS5C_UNUSED(once);
    PS5C_UNUSED(param);
    PS5C_UNUSED(ctx);
    static const wchar_t *const k_dlls[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (size_t i = 0; i < sizeof(k_dlls) / sizeof(k_dlls[0]) && !g_xinput_get_state; i++) {
        HMODULE m = LoadLibraryW(k_dlls[i]);
        if (m) g_xinput_get_state = (xinput_get_state_fn)(void (*)(void))GetProcAddress(m, "XInputGetState");
        if (m) g_xinput_set_state = (xinput_set_state_fn)(void (*)(void))GetProcAddress(m, "XInputSetState");
    }
    return TRUE;
}

static uint8_t axis_u8(SHORT v, bool invert) {
    int x = (v >> 8) + 128; /* -32768..32767 -> 0..255 */
    if (invert) x = 255 - x;
    return (uint8_t)x;
}

/* Querying an empty slot takes about a millisecond, so while no gamepad is
 * connected the slots are scanned at most once a second. */
static bool xinput_read(pal_pad_state *out) {
    InitOnceExecuteOnce(&g_xinput_once, xinput_load, NULL, NULL);
    if (!g_xinput_get_state) return false;
    xinput_state st;
    LONG slot = g_xinput_slot;
    if (slot >= 0 && g_xinput_get_state((DWORD)slot, &st) != ERROR_SUCCESS) {
        slot = -1;
        InterlockedExchange(&g_xinput_slot, -1);
    }
    if (slot < 0) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (now.QuadPart < g_xinput_next_scan) return false;
        g_xinput_next_scan = now.QuadPart + (LONGLONG)g_qpc_freq;
        for (DWORD i = 0; i < 4 && slot < 0; i++)
            if (g_xinput_get_state(i, &st) == ERROR_SUCCESS) slot = (LONG)i;
        if (slot < 0) return false;
        InterlockedExchange(&g_xinput_slot, slot);
    }
    static const struct {
        WORD xi;
        uint32_t ps;
    } k_map[] = {
        {0x0001, PAL_PAD_UP},     {0x0002, PAL_PAD_DOWN},     {0x0004, PAL_PAD_LEFT},   {0x0008, PAL_PAD_RIGHT},
        {0x0010, PAL_PAD_OPTIONS}, {0x0020, PAL_PAD_TOUCH_PAD}, {0x0040, PAL_PAD_L3},    {0x0080, PAL_PAD_R3},
        {0x0100, PAL_PAD_L1},     {0x0200, PAL_PAD_R1},       {0x1000, PAL_PAD_CROSS},  {0x2000, PAL_PAD_CIRCLE},
        {0x4000, PAL_PAD_SQUARE}, {0x8000, PAL_PAD_TRIANGLE},
    };
    for (size_t i = 0; i < sizeof(k_map) / sizeof(k_map[0]); i++)
        if (st.pad.buttons & k_map[i].xi) out->buttons |= k_map[i].ps;
    out->lx = axis_u8(st.pad.lx, false);
    out->ly = axis_u8(st.pad.ly, true);
    out->rx = axis_u8(st.pad.rx, false);
    out->ry = axis_u8(st.pad.ry, true);
    out->l2 = st.pad.left_trigger;
    out->r2 = st.pad.right_trigger;
    if (out->l2 > 30) out->buttons |= PAL_PAD_L2;
    if (out->r2 > 30) out->buttons |= PAL_PAD_R2;
    return true;
}

/* The other XInput slots, in index order, for local players 2-4. The connected set is
 * rescanned at most once a second (an empty slot costs about a millisecond). */
static SRWLOCK g_players_lock = SRWLOCK_INIT;
static LONG g_player_slot[4] = {-1, -1, -1, -1};
static uint64_t g_players_scan;

bool pal_input_read_player(uint32_t player, pal_pad_state *out) {
    if (!player || player > 3) return false;
    InitOnceExecuteOnce(&g_xinput_once, xinput_load, NULL, NULL);
    if (!g_xinput_get_state) return false;
    xinput_state st;
    uint64_t now = pal_time_ns();
    AcquireSRWLockExclusive(&g_players_lock);
    if (!g_players_scan || now - g_players_scan >= 1000000000ull) {
        g_players_scan = now;
        LONG first = g_xinput_slot;
        uint32_t n = 1;
        for (int i = 1; i < 4; i++) g_player_slot[i] = -1;
        for (DWORD i = 0; i < 4 && n < 4; i++)
            if ((LONG)i != first && g_xinput_get_state(i, &st) == ERROR_SUCCESS) g_player_slot[n++] = (LONG)i;
    }
    LONG slot = g_player_slot[player];
    ReleaseSRWLockExclusive(&g_players_lock);
    if (slot < 0 || g_xinput_get_state((DWORD)slot, &st) != ERROR_SUCCESS) return false;
    static const struct {
        WORD xi;
        uint32_t ps;
    } k_map[] = {
        {0x0001, PAL_PAD_UP},     {0x0002, PAL_PAD_DOWN},     {0x0004, PAL_PAD_LEFT},   {0x0008, PAL_PAD_RIGHT},
        {0x0010, PAL_PAD_OPTIONS}, {0x0020, PAL_PAD_TOUCH_PAD}, {0x0040, PAL_PAD_L3},    {0x0080, PAL_PAD_R3},
        {0x0100, PAL_PAD_L1},     {0x0200, PAL_PAD_R1},       {0x1000, PAL_PAD_CROSS},  {0x2000, PAL_PAD_CIRCLE},
        {0x4000, PAL_PAD_SQUARE}, {0x8000, PAL_PAD_TRIANGLE},
    };
    for (size_t i = 0; i < sizeof(k_map) / sizeof(k_map[0]); i++)
        if (st.pad.buttons & k_map[i].xi) out->buttons |= k_map[i].ps;
    out->lx = axis_u8(st.pad.lx, false);
    out->ly = axis_u8(st.pad.ly, true);
    out->rx = axis_u8(st.pad.rx, false);
    out->ry = axis_u8(st.pad.ry, true);
    out->l2 = st.pad.left_trigger;
    out->r2 = st.pad.right_trigger;
    if (out->l2 > 30) out->buttons |= PAL_PAD_L2;
    if (out->r2 > 30) out->buttons |= PAL_PAD_R2;
    return true;
}

static bool key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool pal_input_panel_toggled(void) {
    LONG pending = InterlockedCompareExchange(&g_panel_key_presses, 0, 0);
    while (pending > 0) {
        LONG observed = InterlockedCompareExchange(&g_panel_key_presses, pending - 1, pending);
        if (observed == pending) return true;
        pending = observed;
    }
    return false;
}

/* Keyboard bindings: key -> control (a PAL_PAD_* button or a stick
 * direction), from the user settings; defaults:
 *   arrows D-pad, WASD left stick, IJKL right stick,
 *   Space/Z cross, X/Backspace circle, C square, V triangle,
 *   Q L1, E R1, 1 L2, 3 R2, F L3, G R3, Enter options, T touch pad, R its left half. */
#define MAX_KEY_BINDS 128
static struct {
    int vk;
    uint32_t control;
} g_keys[MAX_KEY_BINDS] = {
    {VK_UP, PAL_PAD_UP},          {VK_DOWN, PAL_PAD_DOWN},          {VK_LEFT, PAL_PAD_LEFT},
    {VK_RIGHT, PAL_PAD_RIGHT},    {VK_SPACE, PAL_PAD_CROSS},        {'Z', PAL_PAD_CROSS},
    {'X', PAL_PAD_CIRCLE},        {VK_BACK, PAL_PAD_CIRCLE},        {'C', PAL_PAD_SQUARE},
    {'V', PAL_PAD_TRIANGLE},      {'Q', PAL_PAD_L1},                {'E', PAL_PAD_R1},
    {'1', PAL_PAD_L2},            {'3', PAL_PAD_R2},                {'F', PAL_PAD_L3},
    {'G', PAL_PAD_R3},            {VK_RETURN, PAL_PAD_OPTIONS},     {'T', PAL_PAD_TOUCH_PAD},
    {'R', PAL_PAD_TOUCH_LEFT},
    {'W', PAL_STICK_LEFT_UP},     {'S', PAL_STICK_LEFT_DOWN},       {'A', PAL_STICK_LEFT_LEFT},
    {'D', PAL_STICK_LEFT_RIGHT},  {'I', PAL_STICK_RIGHT_UP},        {'K', PAL_STICK_RIGHT_DOWN},
    {'J', PAL_STICK_RIGHT_LEFT},  {'L', PAL_STICK_RIGHT_RIGHT},
};
static uint32_t g_key_count = 27;

bool pal_input_bind_keys(uint32_t control, const char *keys) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_key_count; i++) /* drop the control's old keys */
        if (g_keys[i].control != control) g_keys[n++] = g_keys[i];
    g_key_count = n;
    bool ok = true;
    char name[32];
    for (const char *p = keys; *p;) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        while (len && (*p == ' ' || *p == '	')) p++, len--;
        while (len && (p[len - 1] == ' ' || p[len - 1] == '	')) len--;
        if (len && len < sizeof(name)) {
            memcpy(name, p, len);
            name[len] = 0;
            int vk = key_from_name(name);
            if (vk && g_key_count < MAX_KEY_BINDS) {
                g_keys[g_key_count].vk = vk;
                g_keys[g_key_count++].control = control;
            } else {
                ok = false;
            }
        } else if (len) {
            ok = false;
        }
        if (!end) break;
        p = end + 1;
    }
    return ok;
}

static void keyboard_read(pal_pad_state *out) {
    if (native_keyboard_enabled()) return;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg || (GetWindowThreadProcessId(fg, &pid), pid != GetCurrentProcessId())) return;
    uint32_t held = 0;
    for (uint32_t i = 0; i < g_key_count; i++)
        if (key_down(g_keys[i].vk)) held |= g_keys[i].control;
    out->buttons |= held & 0xFFFFFFu;
    if (held & PAL_PAD_L2) out->l2 = 255;
    if (held & PAL_PAD_R2) out->r2 = 255;
    if (held & PAL_STICK_LEFT_LEFT) out->lx = 0;
    if (held & PAL_STICK_LEFT_RIGHT) out->lx = 255;
    if (held & PAL_STICK_LEFT_UP) out->ly = 0;
    if (held & PAL_STICK_LEFT_DOWN) out->ly = 255;
    if (held & PAL_STICK_RIGHT_LEFT) out->rx = 0;
    if (held & PAL_STICK_RIGHT_RIGHT) out->rx = 255;
    if (held & PAL_STICK_RIGHT_UP) out->ry = 0;
    if (held & PAL_STICK_RIGHT_DOWN) out->ry = 255;
}

/* A PlayStation controller: buttons add up; a stick or trigger takes the
 * device pushed furthest (an XInput pad and a DualSense both connected). */
static int deflection(uint8_t v) { return v < 128 ? 128 - v : v - 128; }
static bool dualsense_read(pal_pad_state *out) {
    ds_start();
    ds_state s;
    if (!ds_read(&s) || !s.reports) return false;
    out->buttons |= s.buttons;
    uint8_t *axes[4] = {&out->lx, &out->ly, &out->rx, &out->ry};
    const uint8_t ds_axes[4] = {s.lx, s.ly, s.rx, s.ry};
    for (int i = 0; i < 4; i++)
        if (deflection(ds_axes[i]) > deflection(*axes[i])) *axes[i] = ds_axes[i];
    if (s.l2 > out->l2) out->l2 = s.l2;
    if (s.r2 > out->r2) out->r2 = s.r2;
    out->touch_count = 0;
    for (int i = 0; i < 2; i++) {
        if (!s.touch[i].down) continue;
        out->touch[out->touch_count].x = s.touch[i].x;
        out->touch[out->touch_count].y = s.touch[i].y;
        out->touch[out->touch_count].id = s.touch[i].id;
        out->touch_count++;
    }
    out->touch_w = s.touch_w;
    out->touch_h = s.touch_h;
    out->motion = true;
    memcpy(out->accel, s.accel, sizeof(out->accel));
    memcpy(out->gyro, s.gyro, sizeof(out->gyro));
    return true;
}

/* Games poll several times per frame: host devices are read at most once a millisecond. */
static SRWLOCK g_input_lock = SRWLOCK_INIT;
static pal_pad_state g_input_cached;
static uint64_t g_input_time;
static volatile LONG g_input_kind; /* 0 keyboard, 1 XInput, 2 PlayStation (pal_input_device) */

bool pal_input_has_controller(void) {
    static uint64_t checked;
    static bool connected;
    uint64_t now = pal_time_ns();
    AcquireSRWLockExclusive(&g_input_lock);
    if (!checked || now - checked >= 100000000u) {
        pal_pad_state state = {0}; state.lx = state.ly = state.rx = state.ry = 128;
        bool xi = xinput_read(&state), ds = dualsense_read(&state);
        connected = xi || ds; checked = now;
        native_input_controller(connected);
    }
    bool result = connected;
    ReleaseSRWLockExclusive(&g_input_lock);
    return result;
}

void pal_input_read(pal_pad_state *out) {
    uint64_t now = pal_time_ns();
    AcquireSRWLockExclusive(&g_input_lock);
    if (!g_input_time || now - g_input_time >= 1000000u) {
        pal_pad_state st;
        memset(&st, 0, sizeof(st));
        st.lx = st.ly = st.rx = st.ry = 128;
        bool xi = xinput_read(&st);
        bool ds = dualsense_read(&st);
        g_input_kind = ds ? 2 : xi ? 1 : 0;
        native_input_controller(xi || ds);
        keyboard_read(&st);
        g_input_cached = st;
        g_input_time = now;
    }
    *out = g_input_cached;
    ReleaseSRWLockExclusive(&g_input_lock);
}

void pal_input_set_rumble(uint8_t large, uint8_t small) {
    ds_start();
    ds_set_rumble(large, small);
    InitOnceExecuteOnce(&g_xinput_once, xinput_load, NULL, NULL);
    LONG slot = g_xinput_slot;
    if (g_xinput_set_state && slot >= 0) {
        xinput_vibration v = {(WORD)(large * 257u), (WORD)(small * 257u)};
        g_xinput_set_state((DWORD)slot, &v);
    }
}

void pal_input_set_light(uint8_t r, uint8_t g, uint8_t b) {
    ds_start();
    ds_set_light(r, g, b);
}

const char *pal_input_device(void) {
    if (g_input_kind == 2) {
        static char name[64];
        ds_state s;
        ds_read(&s);
        snprintf(name, sizeof(name), "%s", ds_name(&s));
        return name;
    }
    return g_input_kind == 1 ? "XInput" : "";
}

/* ---------------------------------------------------------------------------
 * Logging
 * ------------------------------------------------------------------------- */

void pal_log_sink(void *user, log_level level, const char *line, size_t len) {
    PS5C_UNUSED(user);
    PS5C_UNUSED(level);
    fwrite(line, 1, len, stderr);
    fflush(stderr);
    if (IsDebuggerPresent()) {
        OutputDebugStringA(line);
    }
}

/* ---------------------------------------------------------------------------
 * Audio: waveOut, 48 kHz stereo float, a ring of short buffers refilled by a
 * thread as the device finishes them (about 64 ms of latency).
 * ------------------------------------------------------------------------- */

#define AUDIO_RATE 48000
#define AUDIO_FRAMES 512 /* per buffer: 10.7 ms */
#define AUDIO_BUFFERS 6

static struct {
    HWAVEOUT dev;
    HANDLE event, thread;
    volatile LONG stop;
    WAVEHDR hdr[AUDIO_BUFFERS];
    float data[AUDIO_BUFFERS][AUDIO_FRAMES * 2];
    pal_audio_fill_fn fill;
    void *user;
} g_audio;

static unsigned __stdcall audio_thread(void *arg) {
    PS5C_UNUSED(arg);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while (!g_audio.stop) {
        WaitForSingleObject(g_audio.event, 20);
        for (int i = 0; i < AUDIO_BUFFERS && !g_audio.stop; i++) {
            WAVEHDR *h = &g_audio.hdr[i];
            if (!(h->dwFlags & WHDR_DONE)) continue;
            g_audio.fill(g_audio.user, g_audio.data[i], AUDIO_FRAMES);
            h->dwFlags &= ~WHDR_DONE;
            waveOutWrite(g_audio.dev, h, sizeof(*h));
        }
    }
    return 0;
}

ps5c_err pal_audio_start(pal_audio_fill_fn fill, void *user) {
    if (g_audio.dev) return PS5C_OK;
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wf.nChannels = 2;
    wf.nSamplesPerSec = AUDIO_RATE;
    wf.wBitsPerSample = 32;
    wf.nBlockAlign = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    g_audio.event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_audio.event) return PS5C_ERR_NOMEM;
    if (waveOutOpen(&g_audio.dev, WAVE_MAPPER, &wf, (DWORD_PTR)g_audio.event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        CloseHandle(g_audio.event);
        memset(&g_audio, 0, sizeof(g_audio));
        return PS5C_ERR_UNSUPPORTED;
    }
    g_audio.fill = fill;
    g_audio.user = user;
    g_audio.stop = 0;
    for (int i = 0; i < AUDIO_BUFFERS; i++) { /* start with silence: the thread refills them */
        WAVEHDR *h = &g_audio.hdr[i];
        memset(h, 0, sizeof(*h));
        memset(g_audio.data[i], 0, sizeof(g_audio.data[i]));
        h->lpData = (LPSTR)g_audio.data[i];
        h->dwBufferLength = sizeof(g_audio.data[i]);
        waveOutPrepareHeader(g_audio.dev, h, sizeof(*h));
        waveOutWrite(g_audio.dev, h, sizeof(*h));
    }
    g_audio.thread = (HANDLE)_beginthreadex(NULL, 0, audio_thread, NULL, 0, NULL);
    return g_audio.thread ? PS5C_OK : PS5C_ERR_NOMEM;
}

void pal_audio_stop(void) {
    if (!g_audio.dev) return;
    InterlockedExchange(&g_audio.stop, 1);
    SetEvent(g_audio.event);
    if (g_audio.thread) {
        WaitForSingleObject(g_audio.thread, INFINITE);
        CloseHandle(g_audio.thread);
    }
    waveOutReset(g_audio.dev);
    for (int i = 0; i < AUDIO_BUFFERS; i++) waveOutUnprepareHeader(g_audio.dev, &g_audio.hdr[i], sizeof(g_audio.hdr[i]));
    waveOutClose(g_audio.dev);
    CloseHandle(g_audio.event);
    memset(&g_audio, 0, sizeof(g_audio));
}

/* Diagnostics: the host state of a page. */
bool pal_host_module(uint64_t addr, char *name, size_t cap, uint64_t *base) {
    HMODULE h = NULL;
    char path[MAX_PATH];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)addr, &h) || !h || !GetModuleFileNameA(h, path, sizeof path))
        return false;
    const char *slash = strrchr(path, 0x5c); /* backslash */
    snprintf(name, cap, "%s", slash ? slash + 1 : path);
    *base = (uint64_t)(uintptr_t)h;
    return true;
}

void pal_log_host_page(uint64_t addr) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)addr, &mi, sizeof(mi))) {
        LOG_W(CORE, "host page 0x%llx: VirtualQuery failed %lu", (unsigned long long)addr, GetLastError());
        return;
    }
    LOG_W(CORE, "host page 0x%llx: base %p alloc %p size 0x%llx state 0x%lx protect 0x%lx alloc-protect 0x%lx type 0x%lx",
          (unsigned long long)addr, mi.BaseAddress, mi.AllocationBase, (unsigned long long)mi.RegionSize, mi.State,
          mi.Protect, mi.AllocationProtect, mi.Type);
}
