/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_VK_COLOR_TARGET_H
#define PS5C_VK_COLOR_TARGET_H
#include <stdint.h>

/* GFX10.3 CB_COLOR_VIEW.MIP_LEVEL [29:26]; ATTRIB2 holds mip zero's
 * dimensions even when the color view selects a smaller level. */
static inline uint32_t vk_color_mip(uint32_t view) { return (view >> 26) & 15; }
static inline uint32_t vk_color_mip_size(uint32_t size, uint32_t level) {
    size >>= level;
    return size ? size : 1;
}
#endif
