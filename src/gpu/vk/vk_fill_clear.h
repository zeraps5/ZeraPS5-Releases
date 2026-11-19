/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_VK_FILL_CLEAR_H
#define PS5C_VK_FILL_CLEAR_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "gpu/vk/vk_api.h"

/* A compute fill writes one 16-byte pattern over an image's memory. When the
 * pattern is a whole number of texels of the image's format, all the same, it
 * is a clear to that texel: its value as a Vulkan clear color. Sonic Frontiers
 * clears its RGBA16F glow target to (0, 0, 0, 1) this way every frame; a clear
 * not taken left every light trail of the stage on screen. */

static inline float vk_fill_half(uint16_t h) {
    uint32_t e = (h >> 10) & 0x1F, m = h & 0x3FF, bits;
    float f;
    if (e == 0) { /* zero or subnormal */
        f = (float)m / 16777216.0f;
        return (h & 0x8000) ? -f : f;
    }
    bits = ((uint32_t)(h & 0x8000) << 16) | (e == 31 ? 0x7F800000u | (m << 13) : ((e + 112) << 23) | (m << 13));
    memcpy(&f, &bits, 4);
    return f;
}

/* False: a format or pattern this does not decode (the fill then only writes memory). */
static inline bool vk_fill_clear_value(VkFormat format, const uint32_t pattern[4], VkClearColorValue *out) {
    uint8_t p[16];
    memcpy(p, pattern, 16);
    uint32_t texel;
    switch (format) {
    case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: texel = 16; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: texel = 8; break;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_R8G8B8A8_UINT: texel = 4; break;
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R8G8_UNORM: texel = 2; break;
    case VK_FORMAT_R8_UNORM: texel = 1; break;
    default: texel = 0; break;
    }
    bool zero = true;
    for (int i = 0; i < 16; i++) zero = zero && !p[i];
    memset(out, 0, sizeof(*out));
    if (zero) return true; /* zero bits are zero in every format */
    if (!texel) return false;
    for (uint32_t i = texel; i < 16; i++)
        if (p[i] != p[i % texel]) return false;
    uint16_t h[8];
    memcpy(h, p, 16);
    switch (format) {
    case VK_FORMAT_R32G32B32A32_SFLOAT: memcpy(out->float32, p, 16); return true;
    case VK_FORMAT_R32G32_SFLOAT: memcpy(out->float32, p, 8); return true;
    case VK_FORMAT_R32_SFLOAT: memcpy(out->float32, p, 4); return true;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: memcpy(out->uint32, p, 16); return true;
    case VK_FORMAT_R32G32_UINT: memcpy(out->uint32, p, 8); return true;
    case VK_FORMAT_R32_UINT: memcpy(out->uint32, p, 4); return true;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        for (int k = 0; k < 4; k++) out->float32[k] = vk_fill_half(h[k]);
        return true;
    case VK_FORMAT_R16G16_SFLOAT:
        for (int k = 0; k < 2; k++) out->float32[k] = vk_fill_half(h[k]);
        return true;
    case VK_FORMAT_R16_SFLOAT: out->float32[0] = vk_fill_half(h[0]); return true;
    case VK_FORMAT_R16G16B16A16_UNORM:
        for (int k = 0; k < 4; k++) out->float32[k] = h[k] / 65535.0f;
        return true;
    case VK_FORMAT_R16G16_UNORM:
        for (int k = 0; k < 2; k++) out->float32[k] = h[k] / 65535.0f;
        return true;
    case VK_FORMAT_R16_UNORM: out->float32[0] = h[0] / 65535.0f; return true;
    case VK_FORMAT_R16G16B16A16_UINT:
        for (int k = 0; k < 4; k++) out->uint32[k] = h[k];
        return true;
    case VK_FORMAT_R8G8B8A8_UNORM:
        for (int k = 0; k < 4; k++) out->float32[k] = p[k] / 255.0f;
        return true;
    case VK_FORMAT_B8G8R8A8_UNORM: /* memory B, G, R, A */
        out->float32[0] = p[2] / 255.0f, out->float32[1] = p[1] / 255.0f;
        out->float32[2] = p[0] / 255.0f, out->float32[3] = p[3] / 255.0f;
        return true;
    case VK_FORMAT_R8G8B8A8_UINT:
        for (int k = 0; k < 4; k++) out->uint32[k] = p[k];
        return true;
    case VK_FORMAT_R8G8_UNORM:
        for (int k = 0; k < 2; k++) out->float32[k] = p[k] / 255.0f;
        return true;
    case VK_FORMAT_R8_UNORM: out->float32[0] = p[0] / 255.0f; return true;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
        uint32_t v = pattern[0];
        out->float32[0] = (v & 0x3FF) / 1023.0f, out->float32[1] = ((v >> 10) & 0x3FF) / 1023.0f;
        out->float32[2] = ((v >> 20) & 0x3FF) / 1023.0f, out->float32[3] = (v >> 30) / 3.0f;
        return true;
    }
    default: return false; /* B10G11R11: nonzero patterns are not decoded */
    }
}

#endif
