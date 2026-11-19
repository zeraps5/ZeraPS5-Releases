/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native x86-64 CPU backend: the guest (Zen 2) code runs directly on the host
 * CPU. The backend only generates a few pieces of glue code, into guest
 * memory next to the import stubs:
 *
 *   enter   host -> guest: saves host callee-saved state, switches to the
 *           guest stack, calls a guest function with System V arguments.
 *   bail    abandons guest execution and returns from `enter` (process exit).
 *   hle     guest -> host: entry point of every import thunk. Captures the
 *           System V argument registers into an hle_args block, calls the
 *           dispatcher with the host ABI, loads the return registers.
 *   thunks  one per import stub: mov r11d, <stub id>; jmp hle
 *
 * Guest code reads its thread pointer with `mov reg, fs:[0]` (FreeBSD TCB
 * self pointer). Windows gives user code no control over fs, so those
 * instructions are rewritten in place to `mov reg, gs:[slot]`: the same
 * length, reading a host TLS slot in the TEB that holds the current guest
 * thread's TCB address.
 *
 * Host requirement: Windows x64 (the thunks use the Win64 calling convention
 * and the TEB TLS slot layout). Other hosts get PS5C_ERR_UNSUPPORTED. */
#ifndef PS5C_X64_NATIVE_H
#define PS5C_X64_NATIVE_H

#include "base/base.h"
#include "hle/hle.h"
#include "mem/gmem.h"

/* Layout read by the generated `enter`/`bail` code. */
typedef struct x64_enter_ctx {
    uint64_t fn;       /* guest function */
    uint64_t rsp;      /* guest stack pointer, 16-byte aligned */
    uint64_t arg0;     /* rdi */
    uint64_t arg1;     /* rsi */
    uint64_t host_rsp; /* written by enter, used by bail */
    uint64_t arg2;     /* rdx */
    uint64_t arg3;     /* rcx */
} x64_enter_ctx;

PS5C_STATIC_ASSERT(offsetof(x64_enter_ctx, host_rsp) == 32, "x64_enter_ctx.host_rsp");
PS5C_STATIC_ASSERT(offsetof(x64_enter_ctx, arg2) == 40, "x64_enter_ctx.arg2");
PS5C_STATIC_ASSERT(offsetof(x64_enter_ctx, arg3) == 48, "x64_enter_ctx.arg3");

typedef void (*x64_dispatch_fn)(hle_args *a);

#define X64_GLUE_SIZE 0x1000u /* reserved at the start of the import stub area */

typedef struct x64_native {
    gmem *mem;
    guest_addr glue;      /* start of the generated glue code */
    guest_addr enter_addr;
    guest_addr bail_addr;
    guest_addr hle_addr;
    guest_addr switch_addr;     /* context switch (fibers) */
    guest_addr fiber_boot_addr; /* first code of a fiber */
    uint32_t tcb_gs_offset; /* gs displacement of the TCB slot */
} x64_native;

/* `glue` must point to X64_GLUE_SIZE bytes of writable+executable guest
 * memory within +-2 GiB of every import stub. */
ps5c_err x64_native_init(x64_native *cpu, gmem *mem, guest_addr glue, x64_dispatch_fn dispatch,
                         uint32_t tcb_gs_offset);

/* Writes the thunk for import stub `id` at `stub` (IMPORT_STUB_SIZE bytes). */
bool x64_native_install_thunk(x64_native *cpu, guest_addr stub, uint32_t id);

/* Rewrites `mov reg, fs:[0]` in [code, code + size). Returns the patch count. */
uint32_t x64_native_patch_tls(x64_native *cpu, guest_addr code, uint64_t size);

/* Calls ctx->fn(arg0, arg1, arg2) on the guest stack; returns the guest's rax.
 * Returns early with the value passed to x64_native_bail. */
uint64_t x64_native_enter(x64_native *cpu, x64_enter_ctx *ctx);
/* Saves the current context (callee-saved registers, xmm6-15, mxcsr, x87
 * control word) on the current stack, stores that stack pointer in
 * *save_rsp and resumes the context saved at resume_rsp. A new context is a
 * stack laid out as X64_SWITCH_FRAME describes. If completion is non-NULL,
 * publish completion_value there only after saving the old context and
 * leaving its stack. The aligned x86 store has release ordering, allowing
 * another host thread to acquire and resume/finalize the parked context. */
void x64_native_switch(x64_native *cpu, uint64_t *save_rsp, uint64_t resume_rsp,
                       volatile uint32_t *completion, uint32_t completion_value);
/* Frame x64_native_switch resumes: 160 bytes of xmm6-15, mxcsr (u32), x87
 * control word (u16), pad, then r15 r14 r13 r12 rsi rdi rbp rbx and the
 * return address. */
#define X64_SWITCH_FRAME 240
#define X64_SWITCH_R15 168
#define X64_SWITCH_R14 176
#define X64_SWITCH_R13 184
#define X64_SWITCH_R12 192
#define X64_SWITCH_RET 232

/* Must be called on the guest thread that is inside x64_native_enter(ctx). */
void x64_native_bail(x64_native *cpu, x64_enter_ctx *ctx, uint64_t value);

#endif
