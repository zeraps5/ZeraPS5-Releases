/* SPDX-License-Identifier: GPL-2.0-or-later */
/* printf-style formatting of guest format strings and System V varargs.
 *
 * Variadic arguments come either straight from an HLE call (the registers
 * after the fixed arguments, then the stack) or from a guest va_list:
 *   struct { u32 gp_offset; u32 fp_offset; u64 overflow_arg_area; u64 reg_save_area; }
 */
#ifndef PS5C_GUEST_PRINTF_H
#define PS5C_GUEST_PRINTF_H

#include "hle/hle.h"

typedef struct guest_va {
    hle_call *c;
    guest_addr va_list; /* 0 = read from the call's registers/stack */
    uint32_t gp, fp;    /* next register index (direct) */
    guest_addr overflow;
} guest_va;

/* Varargs of the current call, after `fixed_gp` integer arguments. */
void guest_va_from_call(guest_va *va, hle_call *c, uint32_t fixed_gp);
/* Varargs from a guest va_list at `va_list`. */
void guest_va_from_list(guest_va *va, hle_call *c, guest_addr va_list);

uint64_t guest_va_int(guest_va *va);
double guest_va_double(guest_va *va);

/* Formats into `out` (always NUL-terminated if out_size > 0). Returns the
 * length the full output would have, like snprintf. */
size_t guest_format(hle_call *c, guest_addr fmt, guest_va *va, char *out, size_t out_size);

#endif
