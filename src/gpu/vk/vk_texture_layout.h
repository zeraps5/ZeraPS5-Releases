/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GFX10 linear surfaces: 256-byte rows, smallest mip first (AMD addrlib). */
#ifndef PS5C_VK_TEXTURE_LAYOUT_H
#define PS5C_VK_TEXTURE_LAYOUT_H

#include <stdint.h>
#include <string.h>

static uint64_t vk_linear_pitch(uint32_t elements, uint32_t bytes_per_element) {
    return ((uint64_t)elements * bytes_per_element + 255u) & ~255ull;
}

/* Dimensions are in elements (BC formats: compression blocks). GFX10 rounds
 * these up when locating a mip, even if the sampled level's extent is smaller. */
static uint64_t vk_linear_mip_offset(uint32_t w, uint32_t h, uint32_t bpe, uint32_t levels, uint32_t level) {
    uint64_t offset = 0;
    for (uint32_t l = levels; l-- > level + 1;) {
        uint32_t mw = (w + (1u << l) - 1) >> l, mh = (h + (1u << l) - 1) >> l;
        offset += vk_linear_pitch(mw, bpe) * mh;
    }
    return offset;
}

static void vk_linear_unpack(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h, uint32_t bpe) {
    uint64_t pitch = vk_linear_pitch(w, bpe), row = (uint64_t)w * bpe;
    for (uint32_t y = 0; y < h; y++) memcpy(dst + y * row, src + y * pitch, (size_t)row);
}

#endif
