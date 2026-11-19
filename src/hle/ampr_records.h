/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Records of the APR command buffers our libSceAmpr builds and libkernel's APR
 * executes (lib_ampr.c writes them, lib_kernel_apr.c runs them). Sizes are the
 * ones sceAmprMeasureCommandSize* report, as KytyPS5's libAmpr.cpp has them.
 * Little endian, 4-byte aligned; dw = 32-bit word, +n = byte offset.
 *
 * - 0x28 read file, 0x14 or 0x18 bytes: the console's format (lib_kernel_apr.c);
 *   0x18 when the file offset needs more than 32 bits.
 * - 0xE1 gather, 0x08: dw0 op | offset[17:0] << 8; dw1 size - 1.
 * - 0xE2 gather, 0x0C: dw0 op | offset[39:32] << 8; dw1 size - 1; dw2 offset[31:0].
 * - 0xE3 scatter, 0x0C: dw0 op | dest[47:32] << 8; dw1 size - 1; dw2 dest[31:0].
 * - 0xE4 gather+scatter, 0x10: dw0 op | dest[47:32] << 8; dw1 size - 1;
 *   dw2 dest[31:0]; dw3 offset[31:0]. 0xE5, 0x14: the same, dw4 offset[39:32].
 * - 0xE6 reset of the gather/scatter state, 0x04.
 * - 0xE7 kernel event, 0x20: dw1 event id; +8 equeue; +16 data.
 * - 0xE8 write address, 0x20: dw0 op | kind << 8 (0 the value, 1 the time
 *   counter, 2 zero); +8 address; +16 value.
 * - 0xE9 wait on address, 0x20: dw0 op | compare << 8; +8 address; +16 value.
 * - 0xEA AMM map (memory of its own), 0x20: dw0 op | prot << 8; dw1 type; +8 address; +16 size.
 * - 0xEB AMM map of direct memory, 0x30: as 0xEA; +24 direct memory offset.
 * - 0xEC AMM unmap, 0x20: +8 address; +16 size.
 * - 0xEF nothing (nops, markers, counters, map brackets): dw0 op | bytes << 8.
 *
 * Gather reads continue the last read's file at its next destination, scatter
 * reads at its next file offset; gather+scatter reads use its file. */
#ifndef PS5C_AMPR_RECORDS_H
#define PS5C_AMPR_RECORDS_H
#include <stdint.h>

#define AMPR_OP_READ 0x28u
#define AMPR_OP_GATHER 0xE1u
#define AMPR_OP_GATHER_EXT 0xE2u
#define AMPR_OP_SCATTER 0xE3u
#define AMPR_OP_GATHER_SCATTER 0xE4u
#define AMPR_OP_GATHER_SCATTER_EXT 0xE5u
#define AMPR_OP_RESET_GS 0xE6u
#define AMPR_OP_EVENT 0xE7u
#define AMPR_OP_WRITE 0xE8u
#define AMPR_OP_WAIT 0xE9u
#define AMPR_OP_AMM_MAP 0xEAu
#define AMPR_OP_AMM_MAP_DIRECT 0xEBu
#define AMPR_OP_AMM_UNMAP 0xECu
#define AMPR_OP_SKIP 0xEFu

#define AMPR_WRITE_VALUE 0u
#define AMPR_WRITE_TIME 1u
#define AMPR_WRITE_ZERO 2u

/* What an AMPR_WRITE_TIME record stores: the TSC, as sceKernelReadTsc (lib_ampr.c) */
uint64_t ampr_time_counter(void);

/* Size in bytes of one of our records (byte 0 in 0xE1..0xEF), 0 if not one. */
static inline uint32_t ampr_record_size(const uint8_t *rec) {
    switch (rec[0]) {
    case AMPR_OP_GATHER: return 0x08;
    case AMPR_OP_GATHER_EXT:
    case AMPR_OP_SCATTER: return 0x0C;
    case AMPR_OP_GATHER_SCATTER: return 0x10;
    case AMPR_OP_GATHER_SCATTER_EXT: return 0x14;
    case AMPR_OP_RESET_GS: return 0x04;
    case AMPR_OP_EVENT:
    case AMPR_OP_WRITE:
    case AMPR_OP_WAIT:
    case AMPR_OP_AMM_MAP:
    case AMPR_OP_AMM_UNMAP: return 0x20;
    case AMPR_OP_AMM_MAP_DIRECT: return 0x30;
    case AMPR_OP_SKIP: return (uint32_t)rec[1] | (uint32_t)rec[2] << 8 | (uint32_t)rec[3] << 16;
    default: return 0;
    }
}
#endif
