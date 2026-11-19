/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu/x64_native.h"

#include <string.h>

#include "base/log.h"
#include "pal/pal.h"

/* ---------------------------------------------------------------------------
 * Tiny x86-64 emitter: only the forms the glue code needs.
 * ------------------------------------------------------------------------- */

typedef struct emitter {
    uint8_t *p;
    uint8_t *start;
    uint8_t *end;
    bool overflow;
} emitter;

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

static void emit(emitter *e, const uint8_t *bytes, size_t n) {
    if (e->p + n > e->end) {
        e->overflow = true;
        return;
    }
    memcpy(e->p, bytes, n);
    e->p += n;
}

static void emit1(emitter *e, uint8_t b) { emit(e, &b, 1); }

static void emit32(emitter *e, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    emit(e, b, 4);
}

static void emit64(emitter *e, uint64_t v) {
    emit32(e, (uint32_t)v);
    emit32(e, (uint32_t)(v >> 32));
}

/* <prefix> [REX] <opcode...> modrm(mod=10, reg, rm=100) SIB(0x24) disp32: [rsp + disp32] */
static void emit_rsp_mem(emitter *e, uint8_t prefix, bool rex_w, const uint8_t *op, size_t op_len,
                         int reg, int32_t disp) {
    if (prefix) emit1(e, prefix);
    uint8_t rex = (uint8_t)(0x40 | (rex_w ? 8 : 0) | (reg >= 8 ? 4 : 0));
    if (rex != 0x40) emit1(e, rex);
    emit(e, op, op_len);
    emit1(e, (uint8_t)(0x84 | ((reg & 7) << 3)));
    emit1(e, 0x24);
    emit32(e, (uint32_t)disp);
}

static void mov_rsp_mem_from_reg(emitter *e, int32_t disp, int reg) { /* mov [rsp+d], r64 */
    static const uint8_t op[] = {0x89};
    emit_rsp_mem(e, 0, true, op, 1, reg, disp);
}

static void mov_reg_from_rsp_mem(emitter *e, int reg, int32_t disp) { /* mov r64, [rsp+d] */
    static const uint8_t op[] = {0x8B};
    emit_rsp_mem(e, 0, true, op, 1, reg, disp);
}

static void movdqu_store(emitter *e, int32_t disp, int xmm) { /* movdqu [rsp+d], xmm */
    static const uint8_t op[] = {0x0F, 0x7F};
    emit_rsp_mem(e, 0xF3, false, op, 2, xmm, disp);
}

static void movdqu_load(emitter *e, int xmm, int32_t disp) { /* movdqu xmm, [rsp+d] */
    static const uint8_t op[] = {0x0F, 0x6F};
    emit_rsp_mem(e, 0xF3, false, op, 2, xmm, disp);
}

static void push_reg(emitter *e, int reg) {
    if (reg >= 8) emit1(e, 0x41);
    emit1(e, (uint8_t)(0x50 | (reg & 7)));
}

static void pop_reg(emitter *e, int reg) {
    if (reg >= 8) emit1(e, 0x41);
    emit1(e, (uint8_t)(0x58 | (reg & 7)));
}

static void jmp_rel32(emitter *e, const uint8_t *target) {
    emit1(e, 0xE9);
    emit32(e, (uint32_t)(int32_t)(target - (e->p + 4)));
}

/* ---------------------------------------------------------------------------
 * Glue code
 * ------------------------------------------------------------------------- */

#define XMM_SAVE_AREA 168 /* xmm6-15 (160 bytes) + 8 bytes to realign the stack */
static const int k_saved_gprs[] = {RBX, RBP, RDI, RSI, R12, R13, R14, R15};

static void emit_enter(emitter *e, uint8_t **epilogue_out) {
    /* Win64: rcx = x64_enter_ctx* */
    for (size_t i = 0; i < PS5C_ARRAY_COUNT(k_saved_gprs); i++) push_reg(e, k_saved_gprs[i]);
    static const uint8_t sub_rsp[] = {0x48, 0x81, 0xEC};
    emit(e, sub_rsp, 3);
    emit32(e, XMM_SAVE_AREA);
    for (int i = 0; i < 10; i++) movdqu_store(e, i * 16, 6 + i);

    static const uint8_t body[] = {
        0x48, 0x89, 0x61, 0x20, /* mov [rcx+32], rsp   ; ctx->host_rsp */
        0x48, 0x8B, 0x41, 0x08, /* mov rax, [rcx+8]    ; guest rsp */
        0x4C, 0x8B, 0x19,       /* mov r11, [rcx]      ; fn */
        0x48, 0x8B, 0x79, 0x10, /* mov rdi, [rcx+16]   ; arg0 */
        0x48, 0x8B, 0x71, 0x18, /* mov rsi, [rcx+24]   ; arg1 */
        0x48, 0x8B, 0x51, 0x28, /* mov rdx, [rcx+40]   ; arg2 */
        0x48, 0x89, 0xC4,      /* mov rsp, rax */
        0x51,                   /* push rcx            ; keep ctx on the guest stack */
        0x6A, 0x00,             /* push 0 */
        0x6A, 0x00,             /* push 0 */
        0x6A, 0x00,             /* push 0              ; call leaves rsp = 8 mod 16 */
        0x48, 0x89, 0xE5,       /* mov rbp, rsp        ; outermost frame {next 0, return 0}: */
                                /*   rbp-chain walkers (Unreal backtraces) stop on it */
        0x48, 0x8B, 0x49, 0x30, /* mov rcx, [rcx+48]   ; arg3 (ctx stays on the stack) */
        0x41, 0xFF, 0xD3,       /* call r11 */
        0x48, 0x83, 0xC4, 0x18, /* add rsp, 24 */
        0x59,                   /* pop rcx */
    };
    emit(e, body, sizeof(body));

    /* Epilogue, also the target of `bail`: rcx = ctx, rax = return value. */
    *epilogue_out = e->p;
    static const uint8_t restore_rsp[] = {0x48, 0x8B, 0x61, 0x20}; /* mov rsp, [rcx+32] */
    emit(e, restore_rsp, sizeof(restore_rsp));
    for (int i = 0; i < 10; i++) movdqu_load(e, 6 + i, i * 16);
    static const uint8_t add_rsp[] = {0x48, 0x81, 0xC4};
    emit(e, add_rsp, 3);
    emit32(e, XMM_SAVE_AREA);
    for (int i = (int)PS5C_ARRAY_COUNT(k_saved_gprs) - 1; i >= 0; i--) pop_reg(e, k_saved_gprs[i]);
    emit1(e, 0xC3); /* ret */
}

/* Stack switch between two contexts of one host thread (fibers). Win64:
 * rcx = where to save the current stack pointer, rdx = stack pointer to
 * resume, r8 = optional completion word, r9d = completion value. The Win64
 * callee-saved state (which covers System V's) goes on the stack being left. */
#define SWITCH_AREA 168 /* xmm6-15, mxcsr, x87 control word */
static void emit_switch(emitter *e) {
    for (size_t i = 0; i < PS5C_ARRAY_COUNT(k_saved_gprs); i++) push_reg(e, k_saved_gprs[i]);
    static const uint8_t sub_rsp[] = {0x48, 0x81, 0xEC};
    emit(e, sub_rsp, 3);
    emit32(e, SWITCH_AREA);
    for (int i = 0; i < 10; i++) movdqu_store(e, i * 16, 6 + i);
    static const uint8_t save[] = {
        0x0F, 0xAE, 0x9C, 0x24, 0xA0, 0x00, 0x00, 0x00, /* stmxcsr [rsp+160] */
        0xD9, 0xBC, 0x24, 0xA4, 0x00, 0x00, 0x00,       /* fnstcw [rsp+164] */
        0x48, 0x89, 0x21,                               /* mov [rcx], rsp */
        0x48, 0x89, 0xD4,                               /* mov rsp, rdx */
        0x4D, 0x85, 0xC0, 0x74, 0x03,                   /* test r8,r8; jz +3 */
        0x45, 0x89, 0x08,                               /* mov [r8], r9d (release old context) */
        0x0F, 0xAE, 0x94, 0x24, 0xA0, 0x00, 0x00, 0x00, /* ldmxcsr [rsp+160] */
        0xD9, 0xAC, 0x24, 0xA4, 0x00, 0x00, 0x00,       /* fldcw [rsp+164] */
    };
    emit(e, save, sizeof(save));
    for (int i = 0; i < 10; i++) movdqu_load(e, 6 + i, i * 16);
    static const uint8_t add_rsp[] = {0x48, 0x81, 0xC4};
    emit(e, add_rsp, 3);
    emit32(e, SWITCH_AREA);
    for (int i = (int)PS5C_ARRAY_COUNT(k_saved_gprs) - 1; i >= 0; i--) pop_reg(e, k_saved_gprs[i]);
    emit1(e, 0xC3);
}

/* First code of a fiber, reached by `ret` from emit_switch: r12 = entry,
 * r13 = first argument, r14 = second argument. */
static void emit_fiber_boot(emitter *e) {
    static const uint8_t code[] = {
        0x4C, 0x89, 0xEF,       /* mov rdi, r13 */
        0x4C, 0x89, 0xF6,       /* mov rsi, r14 */
        0x48, 0x83, 0xE4, 0xF0, /* and rsp, -16 */
        0x6A, 0x00, 0x6A, 0x00, /* push 0; push 0   ; outermost frame {next 0, return 0} */
        0x48, 0x89, 0xE5,       /* mov rbp, rsp */
        0x41, 0xFF, 0xD4,       /* call r12 */
        0x0F, 0x0B,             /* ud2              ; a fiber entry must not return */
    };
    emit(e, code, sizeof(code));
}

static void emit_bail(emitter *e, const uint8_t *epilogue) {
    /* Win64: rcx = ctx, rdx = value */
    static const uint8_t mov_rax_rdx[] = {0x48, 0x89, 0xD0};
    emit(e, mov_rax_rdx, sizeof(mov_rax_rdx));
    jmp_rel32(e, epilogue);
}

#define HLE_FRAME 288 /* 32 shadow space + hle_args (248), multiple of 16 */
#define ARGS 32       /* hle_args offset in the frame */

static void emit_hle_entry(emitter *e, x64_dispatch_fn dispatch) {
    /* Entered from a thunk: r11 = stub id, [rsp] = guest return address. */
    static const uint8_t prologue[] = {0x55, 0x48, 0x89, 0xE5, 0x48, 0x81, 0xEC}; /* push rbp; mov rbp,rsp; sub rsp, */
    emit(e, prologue, sizeof(prologue));
    emit32(e, HLE_FRAME);

    static const int arg_regs[6] = {RDI, RSI, RDX, RCX, R8, R9};
    for (int i = 0; i < 6; i++) mov_rsp_mem_from_reg(e, ARGS + HLE_ARGS_OFF_GPR + i * 8, arg_regs[i]);
    for (int i = 0; i < 8; i++) movdqu_store(e, ARGS + HLE_ARGS_OFF_XMM + i * 16, i);
    mov_rsp_mem_from_reg(e, ARGS + HLE_ARGS_OFF_AL, RAX);
    static const uint8_t lea_r10[] = {0x4C, 0x8D, 0x55, 0x10}; /* lea r10, [rbp+16] ; first stack argument */
    emit(e, lea_r10, sizeof(lea_r10));
    mov_rsp_mem_from_reg(e, ARGS + HLE_ARGS_OFF_STACK, R10);
    mov_rsp_mem_from_reg(e, ARGS + HLE_ARGS_OFF_STUB, R11);

    static const uint8_t lea_rcx[] = {0x48, 0x8D, 0x4C, 0x24, ARGS}; /* lea rcx, [rsp+32] */
    emit(e, lea_rcx, sizeof(lea_rcx));
    static const uint8_t mov_rax_imm[] = {0x48, 0xB8};
    emit(e, mov_rax_imm, 2);
    uint64_t target;
    memcpy(&target, &dispatch, sizeof(target)); /* function pointer -> address */
    emit64(e, target);
    static const uint8_t call_rax[] = {0xFF, 0xD0};
    emit(e, call_rax, 2);

    mov_reg_from_rsp_mem(e, RAX, ARGS + HLE_ARGS_OFF_RET);
    mov_reg_from_rsp_mem(e, RDX, ARGS + HLE_ARGS_OFF_RET + 8);
    movdqu_load(e, 0, ARGS + HLE_ARGS_OFF_RET_XMM);
    movdqu_load(e, 1, ARGS + HLE_ARGS_OFF_RET_XMM + 16);
    static const uint8_t leave_ret[] = {0xC9, 0xC3};
    emit(e, leave_ret, sizeof(leave_ret));
}

ps5c_err x64_native_init(x64_native *cpu, gmem *mem, guest_addr glue, x64_dispatch_fn dispatch,
                         uint32_t tcb_gs_offset) {
    memset(cpu, 0, sizeof(*cpu));
#if !defined(_WIN32) || !(defined(__x86_64__) || defined(_M_X64))
    PS5C_UNUSED(mem);
    PS5C_UNUSED(glue);
    PS5C_UNUSED(dispatch);
    PS5C_UNUSED(tcb_gs_offset);
    LOG_E(CPU, "the native x86-64 backend currently requires a Windows x64 host");
    return PS5C_ERR_UNSUPPORTED;
#else
    if (tcb_gs_offset == 0) {
        return PS5C_ERR_UNSUPPORTED;
    }
    uint8_t *base = (uint8_t *)gmem_host(mem, glue, X64_GLUE_SIZE);
    if (!base) {
        return PS5C_ERR_INVALID;
    }
    emitter e = {base, base, base + X64_GLUE_SIZE, false};
    memset(base, 0xCC, X64_GLUE_SIZE);

    uint8_t *epilogue;
    uint8_t *enter = e.p;
    emit_enter(&e, &epilogue);
    e.p = (uint8_t *)PS5C_ALIGN_UP((uintptr_t)e.p, 16);
    uint8_t *bail = e.p;
    emit_bail(&e, epilogue);
    e.p = (uint8_t *)PS5C_ALIGN_UP((uintptr_t)e.p, 16);
    uint8_t *hle = e.p;
    emit_hle_entry(&e, dispatch);
    e.p = (uint8_t *)PS5C_ALIGN_UP((uintptr_t)e.p, 16);
    uint8_t *sw = e.p;
    emit_switch(&e);
    e.p = (uint8_t *)PS5C_ALIGN_UP((uintptr_t)e.p, 16);
    uint8_t *boot = e.p;
    emit_fiber_boot(&e);
    if (e.overflow) {
        return PS5C_ERR_NOMEM;
    }
    pal_icache_flush(base, X64_GLUE_SIZE);

    cpu->mem = mem;
    cpu->glue = glue;
    cpu->enter_addr = glue + (uint64_t)(enter - base);
    cpu->bail_addr = glue + (uint64_t)(bail - base);
    cpu->hle_addr = glue + (uint64_t)(hle - base);
    cpu->switch_addr = glue + (uint64_t)(sw - base);
    cpu->fiber_boot_addr = glue + (uint64_t)(boot - base);
    cpu->tcb_gs_offset = tcb_gs_offset;
    LOG_D(CPU, "native x64 glue: enter 0x%llx, bail 0x%llx, hle 0x%llx (%u bytes)",
          (unsigned long long)cpu->enter_addr, (unsigned long long)cpu->bail_addr,
          (unsigned long long)cpu->hle_addr, (unsigned)(e.p - base));
    return PS5C_OK;
#endif
}

bool x64_native_install_thunk(x64_native *cpu, guest_addr stub, uint32_t id) {
    uint8_t *p = (uint8_t *)gmem_host(cpu->mem, stub, 16);
    uint8_t *hle = (uint8_t *)gmem_host(cpu->mem, cpu->hle_addr, 1);
    if (!p || !hle) {
        return false;
    }
    int64_t rel = (int64_t)(cpu->hle_addr - (stub + 11));
    if (rel < INT32_MIN || rel > INT32_MAX) {
        return false;
    }
    emitter e = {p, p, p + 16, false};
    static const uint8_t mov_r11d[] = {0x41, 0xBB}; /* mov r11d, imm32 */
    emit(&e, mov_r11d, 2);
    emit32(&e, id);
    emit1(&e, 0xE9); /* jmp rel32 */
    emit32(&e, (uint32_t)(int32_t)rel);
    return !e.overflow;
}

uint32_t x64_native_patch_tls(x64_native *cpu, guest_addr code, uint64_t size) {
    uint8_t *p = (uint8_t *)gmem_host(cpu->mem, code, size);
    if (!p || size < 9) {
        return 0;
    }
    uint32_t count = 0;
    /* 64 [48|4C] 8B modrm(mod=00, rm=100) 25 00 00 00 00  ->  65 ... disp32 = TCB slot */
    for (uint64_t i = 0; i + 9 <= size; i++) {
        if (p[i] != 0x64 || (p[i + 1] != 0x48 && p[i + 1] != 0x4C) || p[i + 2] != 0x8B ||
            (p[i + 3] & 0xC7) != 0x04 || p[i + 4] != 0x25 || p[i + 5] || p[i + 6] || p[i + 7] || p[i + 8]) {
            continue;
        }
        p[i] = 0x65;
        uint32_t d = cpu->tcb_gs_offset;
        p[i + 5] = (uint8_t)d;
        p[i + 6] = (uint8_t)(d >> 8);
        p[i + 7] = (uint8_t)(d >> 16);
        p[i + 8] = (uint8_t)(d >> 24);
        count++;
        i += 8;
    }
    pal_icache_flush(p, size);
    return count;
}

uint32_t x64_native_count_fs(x64_native *cpu, guest_addr code, uint64_t size, guest_addr *first) {
    const uint8_t *p = (const uint8_t *)gmem_host(cpu->mem, code, size);
    if (!p || size < 9) {
        return 0;
    }
    uint32_t count = 0;
    /* 64 [REX] op modrm(mod=00, rm=100) 25 disp32, disp32 != 0: an absolute
     * fs:[disp] load, store or ALU access, left as it is by x64_native_patch_tls */
    for (uint64_t i = 0; i + 9 <= size; i++) {
        if (p[i] != 0x64) {
            continue;
        }
        uint64_t j = i + 1;
        if ((p[j] & 0xF0) == 0x40) {
            j++;
        }
        if (j + 6 > size) {
            break;
        }
        uint8_t op = p[j];
        bool access = op == 0x8B || op == 0x89 || op == 0x8A || op == 0x88 || op == 0x03 || op == 0x2B ||
                      op == 0x3B || op == 0x39 || op == 0x33 || op == 0x0B || op == 0x23 || op == 0x01;
        if (!access || (p[j + 1] & 0xC7) != 0x04 || p[j + 2] != 0x25 ||
            !(p[j + 3] | p[j + 4] | p[j + 5] | p[j + 6])) {
            continue;
        }
        if (!count && first) {
            *first = code + i;
        }
        count++;
        i = j + 6;
    }
    return count;
}

uint64_t x64_native_enter(x64_native *cpu, x64_enter_ctx *ctx) {
    uint64_t (*fn)(x64_enter_ctx *);
    void *addr = gmem_host(cpu->mem, cpu->enter_addr, 1);
    memcpy(&fn, &addr, sizeof(fn));
    return fn(ctx);
}

void x64_native_switch(x64_native *cpu, uint64_t *save_rsp, uint64_t resume_rsp,
                       volatile uint32_t *completion, uint32_t completion_value) {
    void (*fn)(uint64_t *, uint64_t, volatile uint32_t *, uint32_t);
    void *addr = gmem_host(cpu->mem, cpu->switch_addr, 1);
    memcpy(&fn, &addr, sizeof(fn));
    fn(save_rsp, resume_rsp, completion, completion_value);
}

void x64_native_bail(x64_native *cpu, x64_enter_ctx *ctx, uint64_t value) {
    void (*fn)(x64_enter_ctx *, uint64_t);
    void *addr = gmem_host(cpu->mem, cpu->bail_addr, 1);
    memcpy(&fn, &addr, sizeof(fn));
    fn(ctx, value);
}
