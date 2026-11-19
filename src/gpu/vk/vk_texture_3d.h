/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GFX10 standard 3D surfaces. Equations follow AMD addrlib's S3 tables.
 * Unlike a 2D array, a hardware block interleaves X, Y and Z. */
#ifndef PS5C_VK_TEXTURE_3D_H
#define PS5C_VK_TEXTURE_3D_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Table information derived from AMD gfx10SwizzlePattern.h:
 * Copyright (c) 2007-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

typedef struct vk_3d_layout {
    uint32_t bw, bh, bd, bytes, bpe, log_bpe;
} vk_3d_layout;

static bool vk_std_3d_layout(uint32_t mode, uint32_t bpe, vk_3d_layout *l) {
    if ((mode != 5 && mode != 9) || !bpe || bpe > 16 || (bpe & (bpe-1))) return false;
    uint32_t k = 0;
    while ((1u << k) < bpe) k++;
    static const uint8_t small[5][3] = {{4,4,4},{3,4,4},{3,4,3},{3,3,3},{2,3,3}};
    static const uint8_t large[5][3] = {{6,5,5},{5,5,5},{5,5,4},{5,4,4},{4,4,4}};
    const uint8_t *dims = mode == 9 ? large[k] : small[k];
    *l = (vk_3d_layout){1u << dims[0], 1u << dims[1], 1u << dims[2], mode == 9 ? 65536u : 4096u, bpe, k};
    return true;
}

static uint64_t vk_std_3d_bytes(const vk_3d_layout *l, uint32_t w, uint32_t h, uint32_t d) {
    return (uint64_t)((w+l->bw-1)/l->bw) * ((h+l->bh-1)/l->bh) * ((d+l->bd-1)/l->bd) * l->bytes;
}

static uint32_t vk_std_3d_block_offset(const vk_3d_layout *l, uint32_t x, uint32_t y, uint32_t z) {
    /* Encoded source bits: X0..5=0..5, Y0..4=16..20, Z0..4=32..36, zero=255. */
    static const uint8_t bits[5][16] = {
        {0,1,32,16,33,17,2,34,18,3,35,19,4,36,20,5},
        {255,0,32,16,33,17,1,34,18,2,35,19,3,36,20,4},
        {255,255,0,16,32,17,1,33,18,2,34,19,3,35,20,4},
        {255,255,255,0,32,16,1,33,17,2,34,18,3,35,19,4},
        {255,255,255,255,32,16,0,33,17,1,34,18,2,35,19,3}
    };
    uint32_t coord[] = {x,y,z}, off = 0;
    uint32_t count = l->bytes == 65536 ? 16 : 12;
    for (uint32_t b = l->log_bpe; b < count; b++) {
        uint32_t s = bits[l->log_bpe][b];
        off |= ((coord[s >> 4] >> (s & 15)) & 1) << b;
    }
    return off;
}

static uint64_t vk_std_3d_offset(const vk_3d_layout *l, uint32_t w, uint32_t h,
                                uint32_t x, uint32_t y, uint32_t z) {
    uint64_t block = ((uint64_t)(z/l->bd) * ((h+l->bh-1)/l->bh) + y/l->bh) * ((w+l->bw-1)/l->bw) + x/l->bw;
    return block * l->bytes + vk_std_3d_block_offset(l,x,y,z);
}

static void vk_std_3d_unpack(const uint8_t *src, uint8_t *dst, const vk_3d_layout *l,
                             uint32_t w, uint32_t h, uint32_t d) {
    for (uint32_t z = 0; z < d; z++) for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        uint64_t to = ((uint64_t)z*h*w + (uint64_t)y*w + x) * l->bpe;
        memcpy(dst+to, src+vk_std_3d_offset(l,w,h,x,y,z), l->bpe);
    }
}
#endif
