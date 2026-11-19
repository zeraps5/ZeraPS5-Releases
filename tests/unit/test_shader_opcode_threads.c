/* SPDX-License-Identifier: GPL-2.0-or-later */
/* First opcode lookup must be safe before any single-threaded warmup. */
#include "gpu/shader/shader_ops.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

enum { WORKERS = 32 };
static HANDLE gate;
static unsigned errors[WORKERS];

static DWORD WINAPI lookup_worker(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    if (WaitForSingleObject(gate, INFINITE) != WAIT_OBJECT_0) return 1;
    const rdna2_inst add = {.name = "v_add_f32"};
    const rdna2_inst end = {.name = "s_endpgm"};
    const rdna2_inst load = {.name = "global_load_dwordx4"};
    for (unsigned i = 0; i < 64; i++) {
        errors[id] += sh_op_of(&add) != SH_OP_V_ADD_F32;
        errors[id] += sh_op_of(&end) != SH_OP_S_ENDPGM;
        errors[id] += sh_op_of(&load) != SH_OP_GLOBAL_LOAD;
    }
    return 0;
}

int main(void) {
    HANDLE threads[WORKERS];
    gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!gate) return 2;
    for (unsigned i = 0; i < WORKERS; i++) {
        threads[i] = CreateThread(NULL, 0, lookup_worker, (void *)(uintptr_t)i, 0, NULL);
        if (!threads[i]) return 3;
    }
    if (!SetEvent(gate)) return 4;
    if (WaitForMultipleObjects(WORKERS, threads, TRUE, 10000) != WAIT_OBJECT_0) {
        fputs("Concurrent first opcode lookups did not complete\n", stderr);
        return 5;
    }
    unsigned total = 0;
    for (unsigned i = 0; i < WORKERS; i++) {
        DWORD code;
        if (!GetExitCodeThread(threads[i], &code) || code) return 6;
        total += errors[i];
        CloseHandle(threads[i]);
    }
    CloseHandle(gate);
    printf("Concurrent first opcode lookups: %u mismatches\n", total);
    return total ? 1 : 0;
}
