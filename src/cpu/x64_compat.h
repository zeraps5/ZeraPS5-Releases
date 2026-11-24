/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Older host CPUs: the PS5's Zen 2 has instruction sets that CPUs before
 * Intel Haswell / AMD Zen lack (BMI1, BMI2, LZCNT, MOVBE, FMA3, AVX2). The
 * guest code is scanned when a module is loaded, and each instruction of a
 * set the host lacks is replaced:
 *
 *   5 bytes or more  a jmp to a stub of its own, which saves the whole
 *                    register state, runs the instruction in C and returns
 *                    after it;
 *   shorter          a stub also relocates the following instruction(s),
 *                    unless a known branch target prevents it; then UD2
 *                    routes execution through x64_compat_fault.
 *
 * LZCNT and TZCNT matter even without a fault: older CPUs run them as BSR
 * and BSF, with other results.
 *
 * PS5C_CPU_COMPAT=ivb treats the host as an Ivy Bridge (3rd generation Core:
 * SSE4.2, AVX, F16C), so the replacements can be tried on a newer CPU;
 * PS5C_CPU_COMPAT=off disables host ISA compatibility patching. */
#ifndef PS5C_X64_COMPAT_H
#define PS5C_X64_COMPAT_H

#include "base/base.h"
#include "mem/gmem.h"
#include "pal/pal.h"

enum {
    X64_SET_BMI1 = 1u << 0,
    X64_SET_BMI2 = 1u << 1,
    X64_SET_LZCNT = 1u << 2,
    X64_SET_MOVBE = 1u << 3,
    X64_SET_FMA = 1u << 4,
    X64_SET_AVX2 = 1u << 5,
    X64_SET_TSC = 1u << 6,
};

/* The sets to replace on this host (once, from CPUID and PS5C_CPU_COMPAT). */
uint32_t x64_compat_missing(void);

/* PS5C_GUEST_TSC=1 makes inline reads and HLE timestamps use the same guest clock. */
#define X64_GUEST_TSC_HZ 1600000000ull
bool x64_guest_tsc_enabled(void);
uint64_t x64_guest_tsc_read(void);

/* Scans [code, code + size) of a module and replaces what the host lacks.
 * Stubs are placed in new guest memory near the code. */
void x64_compat_patch(gmem *mem, const char *module, guest_addr code, uint64_t size, guest_addr near_hint);

/* An illegal instruction: true when it was one of ours (info updated). */
bool x64_compat_fault(pal_fault_info *info);

/* Decoded instruction (exposed for tests). */
typedef struct x64_insn {
    uint8_t len;
    uint8_t map; /* 0: one byte, 1: 0F, 2: 0F38, 3: 0F3A */
    uint8_t op;
    uint8_t rex;
    bool p66, p67, lock;
    uint8_t rep; /* last of F2 / F3, or 0 */
    uint8_t seg; /* 0x64 / 0x65 / other segment prefix, or 0 */
    bool vex;
    uint8_t pp; /* VEX: 0 none, 1 66, 2 F3, 3 F2 */
    bool vex_l, vex_w;
    uint8_t vvvv; /* VEX source register (0-15) */
    bool has_modrm;
    uint8_t mod, reg, rm; /* reg and rm with their REX/VEX extensions */
    bool has_sib, rip_rel;
    uint8_t base, index, scale; /* index 4 (no REX.X) = none */
    bool no_base;
    int32_t disp;
    uint8_t disp_off; /* offset of disp32 in the bytes, for RIP-relative forms */
    uint8_t imm_len;
    uint64_t imm;
} x64_insn;

/* Decodes one instruction (lengths of all of x86-64, operands of what we
 * emulate); false for an invalid encoding. */
bool x64_decode(const uint8_t *p, size_t avail, x64_insn *out);
/* The set an instruction belongs to (one X64_SET_*), 0 for none / a form
 * we do not emulate. */
uint32_t x64_compat_set(const x64_insn *in);

#endif
