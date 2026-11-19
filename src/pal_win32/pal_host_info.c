/* SPDX-License-Identifier: GPL-2.0-or-later */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pal/pal.h"

static void registry_text(HKEY key, const wchar_t *path, const wchar_t *name, char *out, size_t cap) {
    wchar_t value[128] = {0};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(key, path, name, RRF_RT_REG_SZ, NULL, value, &bytes) != ERROR_SUCCESS) return;
    value[PS5C_ARRAY_COUNT(value) - 1] = 0;
    char utf8[384];
    if (!WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8, sizeof(utf8), NULL, NULL)) return;
    char *start = utf8;
    while (*start == ' ') start++;
    size_t len = strlen(start);
    while (len && start[len - 1] == ' ') len--;
    if (!len) return;
    snprintf(out, cap, "%.*s", (int)len, start);
}

void pal_host_get_info(pal_host_info *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->cpu, sizeof(out->cpu), "N/A");
    snprintf(out->os, sizeof(out->os), "Windows x64");
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    out->logical_processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (!out->logical_processors) out->logical_processors = si.dwNumberOfProcessors;
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &bytes);
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *cores = bytes ? malloc(bytes) : NULL;
    if (cores && GetLogicalProcessorInformationEx(RelationProcessorCore, cores, &bytes)) {
        for (DWORD pos = 0; pos + sizeof(DWORD) * 2 <= bytes;) {
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *item = (void *)((uint8_t *)cores + pos);
            if (item->Size < sizeof(DWORD) * 2 || item->Size > bytes - pos) break;
            if (item->Relationship == RelationProcessorCore) out->physical_cores++;
            pos += item->Size;
        }
    }
    free(cores);
    registry_text(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                  L"ProcessorNameString", out->cpu, sizeof(out->cpu));

    /* RtlGetVersion reports the actual build without a version-helper manifest. */
    typedef LONG(WINAPI *version_fn)(OSVERSIONINFOEXW *);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    version_fn version = ntdll ? (version_fn)(void (*)(void))GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    OSVERSIONINFOEXW v = {0};
    v.dwOSVersionInfoSize = sizeof(v);
    if (version && version(&v) == 0) {
        const char *family = v.wProductType != VER_NT_WORKSTATION ? "Windows Server" :
                             v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000 ? "Windows 11" :
                             v.dwMajorVersion == 10 ? "Windows 10" : "Windows";
        const char *arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "ARM64" : "x64";
        char release[32] = "";
        const wchar_t *key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
        registry_text(HKEY_LOCAL_MACHINE, key, L"DisplayVersion", release, sizeof(release));
        DWORD revision = 0, size = sizeof(revision);
        bool have_revision = RegGetValueW(HKEY_LOCAL_MACHINE, key, L"UBR", RRF_RT_REG_DWORD, NULL,
                                         &revision, &size) == ERROR_SUCCESS;
        if (have_revision)
            snprintf(out->os, sizeof(out->os), "%s %s (%lu.%lu.%lu.%lu) %s", family, release,
                     v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, revision, arch);
        else
            snprintf(out->os, sizeof(out->os), "%s %s (%lu.%lu.%lu) %s", family, release,
                     v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, arch);
    }
}

static uint64_t filetime_ns(FILETIME t) {
    return (((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime) * 100;
}

void pal_host_get_stats(pal_host_stats *out) {
    memset(out, 0, sizeof(*out));
    PERFORMANCE_INFORMATION info = {0};
    info.cb = sizeof(info);
    if (K32GetPerformanceInfo(&info, sizeof(info))) {
        out->ram_total = (uint64_t)info.PhysicalTotal * info.PageSize;
        out->ram_available = (uint64_t)info.PhysicalAvailable * info.PageSize;
        out->commit_total = (uint64_t)info.CommitTotal * info.PageSize;
        out->commit_limit = (uint64_t)info.CommitLimit * info.PageSize;
        out->valid |= PAL_HOST_RAM | PAL_HOST_COMMIT;
    }
    PROCESS_MEMORY_COUNTERS_EX process = {0};
    process.cb = sizeof(process);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&process, sizeof(process))) {
        out->process_ram = process.WorkingSetSize;
        out->process_commit = process.PrivateUsage;
        out->valid |= PAL_HOST_PROCESS_RAM | PAL_HOST_PROCESS_COMMIT;
    }
    FILETIME created, exited, kernel, user, idle;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        out->process_cpu_ns = filetime_ns(kernel) + filetime_ns(user);
        out->valid |= PAL_HOST_PROCESS_CPU;
    }
    /* GetSystemTimes covers one group on >64-CPU machines; do not mislabel it
     * as whole-system utilization there. The process measurement still works. */
    if (GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) <= 64 && GetSystemTimes(&idle, &kernel, &user)) {
        out->system_cpu_ns = filetime_ns(kernel) + filetime_ns(user); /* includes idle */
        out->system_idle_ns = filetime_ns(idle);
        out->valid |= PAL_HOST_SYSTEM_CPU;
    }
}
