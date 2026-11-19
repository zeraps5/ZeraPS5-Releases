/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PS5C_GPU_SAMPLE=<seconds>: sampling profiler of the GPU (command processor)
 * thread, a debugging aid. Every millisecond the thread is suspended and its
 * pc read. Samples in the emulator count by pc; samples elsewhere (the Vulkan
 * driver, the C library's memcpy, the kernel while the thread waits) count by
 * module and by the emulator code that called it: the first return address
 * into ps5c.exe found on the stack (and the next one). Every <seconds> the
 * period's counts go to user/gpu_samples_<n>.txt ("<module or exe> <offset>
 * <next caller> <count>"; resolve offsets with addr2line -f -e ps5c.exe
 * 0x140000000+offset) and the log gets the share of each module. */
#include "gpu/gpu_sampler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "pal/pal.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define SLOTS 32768u
#define MODULES 64u
#define STACK_SCAN 4096u

typedef struct sample_slot {
    uint32_t module; /* 0: ps5c.exe (offset = pc), else module index + 1 (offset = caller) */
    uint64_t offset, offset2, offset3, offset4, count; /* next ps5c.exe return addresses on the stack */
} sample_slot;

static struct {
    HANDLE thread;
    uint32_t seconds;
    pal_thread *sampler;
    uint64_t exe_base, exe_end;
    struct {
        uint64_t base, end;
        char name[48];
        uint64_t count;
    } mods[MODULES];
    uint32_t mod_count;
} g;

static uint64_t module_end(uint64_t base) {
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)(uintptr_t)base;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(uintptr_t)(base + (uint64_t)dos->e_lfanew);
    return base + nt->OptionalHeader.SizeOfImage;
}

/* Index + 1 of the module holding pc (registered on first sight), 0 if none */
static uint32_t module_of(uint64_t pc) {
    for (uint32_t i = 0; i < g.mod_count; i++) {
        if (pc >= g.mods[i].base && pc < g.mods[i].end) return i + 1;
    }
    HMODULE m = NULL;
    if (g.mod_count == MODULES ||
        !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)pc, &m) || !m)
        return 0;
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(m, path, sizeof(path));
    const char *name = path;
    for (DWORD k = 0; k < n; k++) {
        if (path[k] == '\\' || path[k] == '/') name = path + k + 1;
    }
    uint32_t i = g.mod_count++;
    g.mods[i].base = (uint64_t)(uintptr_t)m;
    g.mods[i].end = module_end(g.mods[i].base);
    snprintf(g.mods[i].name, sizeof(g.mods[i].name), "%.47s", n ? name : "?");
    return i + 1;
}

static void count(sample_slot *tab, uint32_t module, const uint64_t off[4]) {
    uint64_t h = ((off[0] ^ (off[1] * 31u) ^ (off[2] * 961u) ^ (off[3] * 29791u) ^ ((uint64_t)module << 40)) *
                  0x9E3779B97F4A7C15ull) >> 49;
    for (uint32_t i = 0; i < SLOTS; i++) {
        sample_slot *s = &tab[(h + i) & (SLOTS - 1)];
        if (!s->count || (s->module == module && s->offset == off[0] && s->offset2 == off[1] && s->offset3 == off[2] &&
                          s->offset4 == off[3])) {
            s->module = module;
            s->offset = off[0];
            s->offset2 = off[1];
            s->offset3 = off[2];
            s->offset4 = off[3];
            s->count++;
            return;
        }
    }
}

static void save(const sample_slot *tab, uint64_t total, uint64_t in_exe, uint32_t n) {
    char path[64];
    snprintf(path, sizeof(path), "user/gpu_samples_%u.txt", n);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "# %llu samples\n", (unsigned long long)total);
        for (uint32_t i = 0; i < SLOTS; i++) {
            if (!tab[i].count) continue;
            fprintf(f, "%s 0x%llx 0x%llx 0x%llx 0x%llx %llu\n", tab[i].module ? g.mods[tab[i].module - 1].name : "exe",
                    (unsigned long long)tab[i].offset, (unsigned long long)tab[i].offset2, (unsigned long long)tab[i].offset3,
                    (unsigned long long)tab[i].offset4, (unsigned long long)tab[i].count);
        }
        fclose(f);
    }
    char line[512];
    int len = snprintf(line, sizeof(line), "GPU thread samples: %llu, ps5c.exe %.0f%%", (unsigned long long)total,
                       total ? 100.0 * (double)in_exe / (double)total : 0.0);
    for (uint32_t i = 0; i < g.mod_count && len < 440; i++) {
        if (g.mods[i].count * 50 >= total) /* 2% or more */
            len += snprintf(line + len, sizeof(line) - (size_t)len, ", %s %.0f%%", g.mods[i].name,
                            100.0 * (double)g.mods[i].count / (double)total);
        g.mods[i].count = 0;
    }
    LOG_I(GPU, "%s (%s)", line, path);
}

static int32_t sampler_main(void *arg) {
    PS5C_UNUSED(arg);
    sample_slot *tab = (sample_slot *)calloc(SLOTS, sizeof(sample_slot));
    uint64_t *stack = (uint64_t *)malloc(STACK_SCAN);
    if (!tab || !stack) return 0;
    uint64_t total = 0, in_exe = 0, next = pal_time_ns() + (uint64_t)g.seconds * 1000000000ull;
    uint32_t reports = 0;
    for (;;) {
        pal_sleep_ns(1000000);
        if (SuspendThread(g.thread) == (DWORD)-1) continue;
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL;
        SIZE_T got = 0;
        bool ok = GetThreadContext(g.thread, &ctx);
        if (ok) ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)ctx.Rsp, stack, STACK_SCAN, &got);
        ResumeThread(g.thread);
        if (!ok) continue;
        total++;
        bool in = ctx.Rip >= g.exe_base && ctx.Rip < g.exe_end;
        uint32_t m = in ? 0 : module_of(ctx.Rip);
        if (in) in_exe++;
        else if (m) g.mods[m - 1].count++;
        /* the pc (in the emulator) or nothing, then return addresses into the emulator found on the stack */
        uint64_t off[4] = {in ? ctx.Rip - g.exe_base : 0, 0, 0, 0};
        uint32_t n = in ? 1 : 0;
        for (size_t k = 0; k < got / 8 && n < 4; k++) {
            if (stack[k] > g.exe_base + 0x1000 && stack[k] < g.exe_end) off[n++] = stack[k] - g.exe_base;
        }
        count(tab, m, off);
        if (pal_time_ns() >= next) {
            save(tab, total, in_exe, reports++);
            memset(tab, 0, sizeof(sample_slot) * SLOTS);
            total = in_exe = 0;
            next += (uint64_t)g.seconds * 1000000000ull;
        }
    }
}

void gpu_sampler_start(void) {
    const char *e = getenv("PS5C_GPU_SAMPLE");
    if (!e || g.sampler) return;
    g.seconds = atoi(e) > 0 ? (uint32_t)atoi(e) : 10;
    g.exe_base = (uint64_t)(uintptr_t)GetModuleHandleW(NULL);
    g.exe_end = module_end(g.exe_base);
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g.thread, 0, FALSE,
                         DUPLICATE_SAME_ACCESS) ||
        pal_thread_create(&g.sampler, sampler_main, NULL, "gpu sampler", 0) != PS5C_OK) {
        g.sampler = NULL;
        return;
    }
    LOG_I(GPU, "sampling the GPU thread every millisecond (report every %u s)", g.seconds);
}

#else
void gpu_sampler_start(void) {}
#endif
