/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_SHADER_APERTURE_H
#define PS5C_SHADER_APERTURE_H
#include <stdbool.h>
#include <stdint.h>
/* Experimental 64-bit address-space model. Window size/alignment follows
 * RDNA2/LLVM; high halves are inferred from guest pointers, not measured on
 * console. Generic FLAT routing must use this definition before enabling it. */
#define SH_SHARED_APERTURE_HIGH 0x80000000u
#define SH_PRIVATE_APERTURE_HIGH 0x70000000u
static inline bool sh_aperture_value(uint32_t code,uint64_t *out) {
    if(code<235 || code>238)return false;
    uint32_t high=code<237?SH_SHARED_APERTURE_HIGH:SH_PRIVATE_APERTURE_HIGH;
    *out=((uint64_t)high<<32)|((code&1u)?0ull:0xffffffffull);
    return true;
}
#endif
