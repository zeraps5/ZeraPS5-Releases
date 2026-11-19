#ifndef PS5C_VK_PACKED_FORMATS_H
#define PS5C_VK_PACKED_FORMATS_H

#include <stdbool.h>
#include <stdint.h>

/* Packed IMG_FORMATs of fewer than 8 bits a channel: 5_6_5 (133), 1_5_5_5, 5_5_5_1,
 * 4_4_4_4, 4_4 (137), UNORM; PSPHD's PSP 5650/5551/4444 textures are 133, 134 and 136
 * (133 with DST_SEL W=1: the alpha-less one). The name lists the channels from the top bit down,
 * X in the low bits. Sampled as RGBA8 here: no host format has every layout, and the
 * channel order differs from Vulkan's packed formats. Missing channels read 0, W 1. */
static inline uint32_t vk_packed_bytes(uint32_t fmt) {
    return fmt >= 133 && fmt <= 136 ? 2 : fmt == 137 ? 1 : 0;
}

static inline uint8_t vk_packed_unorm(uint32_t v, uint32_t bits) {
    return (uint8_t)((v * 255u + ((1u << bits) - 1) / 2) / ((1u << bits) - 1));
}

/* `count` texels of `fmt` at the start of `buf` become RGBA8 in place (from the last
 * texel back: each one grows). False for any other format. */
static inline bool vk_packed_expand(uint32_t fmt, uint8_t *buf, uint64_t count) {
    uint32_t bytes = vk_packed_bytes(fmt);
    if (!bytes) return false;
    for (uint64_t i = count; i-- > 0;) {
        uint32_t v = bytes == 2 ? (uint32_t)buf[i * 2] | (uint32_t)buf[i * 2 + 1] << 8 : buf[i];
        uint8_t *o = buf + i * 4;
        uint8_t x, y, z, w;
        switch (fmt) {
        case 133: x = vk_packed_unorm(v & 31, 5), y = vk_packed_unorm(v >> 5 & 63, 6), z = vk_packed_unorm(v >> 11, 5), w = 255; break;
        case 134: x = vk_packed_unorm(v & 31, 5), y = vk_packed_unorm(v >> 5 & 31, 5), z = vk_packed_unorm(v >> 10 & 31, 5),
                  w = v >> 15 ? 255 : 0; break;
        case 135: x = v & 1 ? 255 : 0, y = vk_packed_unorm(v >> 1 & 31, 5), z = vk_packed_unorm(v >> 6 & 31, 5),
                  w = vk_packed_unorm(v >> 11, 5); break;
        case 136: x = (uint8_t)((v & 15) * 17), y = (uint8_t)((v >> 4 & 15) * 17), z = (uint8_t)((v >> 8 & 15) * 17),
                  w = (uint8_t)((v >> 12) * 17); break;
        default: x = (uint8_t)((v & 15) * 17), y = (uint8_t)((v >> 4) * 17), z = 0, w = 255; break;
        }
        o[0] = x, o[1] = y, o[2] = z, o[3] = w;
    }
    return true;
}

#endif
