/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_AVPLAYER_PICTURE_H
#define PS5C_AVPLAYER_PICTURE_H
#include <stdint.h>
#include <string.h>

/* The decoder's compact NV12 planes become GFX10 linear textures. Both Y
 * and interleaved UV rows need the same 256-byte pitch; the frame metadata
 * exposes that pitch and crops the padded columns from the visible image. */
static inline uint32_t avp_picture_pitch(uint32_t width) {
    return (width + 255u) & ~255u;
}

/* PS4 (GCN linear aligned): 64-byte rows hold both planes (R8 luma: 64 elements; RG8 chroma:
 * 32), and with a 16-aligned height every plane is a whole number of 256-byte interleaves.
 * Games size the textures from the picture width and take this pitch from it. */
static inline uint32_t avp_picture_pitch_ps4(uint32_t width) {
    return (width + 63u) & ~63u;
}

static inline void avp_picture_pack(uint8_t *dst, const uint8_t *nv12, uint32_t width,
                                    uint32_t height, uint32_t pitch, uint32_t plane_height) {
    for (uint32_t y = 0; y < height; y++)
        memcpy(dst + (uint64_t)y * pitch, nv12 + (uint64_t)y * width, width);
    uint8_t *uv = dst + (uint64_t)pitch * plane_height;
    for (uint32_t y = 0; y < height / 2; y++)
        memcpy(uv + (uint64_t)y * pitch, nv12 + (uint64_t)width * height + (uint64_t)y * width,
               width);
}
#endif
