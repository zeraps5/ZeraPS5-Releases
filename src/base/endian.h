/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Little-endian loads/stores that are safe for unaligned data. Guest data is
 * always little-endian; memcpy compiles down to a single mov on x86-64. */
#ifndef PS5C_ENDIAN_H
#define PS5C_ENDIAN_H

#include <string.h>

#include "base/base.h"

static inline uint16_t rd_le16(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint16_t)(b[0] | (b[1] << 8));
}

static inline uint32_t rd_le32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static inline uint64_t rd_le64(const void *p) {
    return (uint64_t)rd_le32(p) | ((uint64_t)rd_le32((const uint8_t *)p + 4) << 32);
}

static inline void wr_le16(void *p, uint16_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}

static inline void wr_le32(void *p, uint32_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

static inline void wr_le64(void *p, uint64_t v) {
    wr_le32(p, (uint32_t)v);
    wr_le32((uint8_t *)p + 4, (uint32_t)(v >> 32));
}

#endif
