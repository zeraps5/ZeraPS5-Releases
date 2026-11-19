/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_VK_DCC_METADATA_H
#define PS5C_VK_DCC_METADATA_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* A whole-image clear requires one clear code throughout the registered
 * metadata interval. Mixed/padded/unknown layouts cannot prove that every
 * texel is cleared; preserve the image rather than extrapolate a majority.
 * This recognizes uniform clears, not partial clears or compressed tiles. */
static inline int vk_dcc_uniform_clear(const uint8_t *metadata, size_t bytes) {
    if (!metadata || !bytes) return -1;
    uint8_t code = metadata[0];
    if (code != 0x00 && code != 0x40 && code != 0x80 && code != 0xc0 && code != 0x20) return -1;
    uint64_t pattern = UINT64_C(0x0101010101010101) * code;
    size_t i = 0;
    for (; bytes - i >= sizeof(pattern); i += sizeof(pattern)) {
        uint64_t word;
        memcpy(&word, metadata + i, sizeof(word)); /* metadata need not be aligned */
        if (word != pattern) return -1;
    }
    for (; i < bytes; i++) if (metadata[i] != code) return -1;
    return code;
}

#endif
