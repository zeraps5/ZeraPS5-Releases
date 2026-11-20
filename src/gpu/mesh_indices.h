/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_MESH_INDICES_H
#define PS5C_MESH_INDICES_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

/* Separate arena allocations, native little-endian host. Preserve every index,
 * including primitive-restart values; expansion and GE base offset happen later. */
static inline bool mesh_indices_expand(uint32_t *dst, const void *src, uint32_t count, uint32_t size) {
    if (size != 1 && size != 2 && size != 4) return false;
    if (!count) return true;
    size_t bytes_count = (size_t)count * 4;
    if (!dst || !src || bytes_count / 4 != count) return false;
    if (size == 4) {
        memcpy(dst, src, bytes_count);
    } else if (size == 2) {
        const uint8_t *bytes = src;
        for (uint32_t i = 0; i < count; i++) {
            uint16_t index;
            memcpy(&index, bytes + (size_t)i * 2, 2);
            dst[i] = index;
        }
    } else {
        const uint8_t *bytes = src;
        for (uint32_t i = 0; i < count; i++) dst[i] = bytes[i];
    }
    return true;
}
#endif
