/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>
#include <string.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

/* src is the completed GPU result; orig is its immutable input snapshot.
 * dst may contain later CPU writes. Never store an unchanged GPU byte: even
 * a read/modify/write of a whole vector could lose a concurrent CPU update.
 * All loads are bounded, including unaligned ranges and partial tails.
 * orig == dst is also supported by the resident-buffer completion path. */
static inline void vk_writeback_merge(uint8_t *dst, const uint8_t *src,
                                      const uint8_t *orig, uint64_t size) {
    for (uint64_t o = 0; o < size;) {
        uint64_t block = size - o < 256 ? size - o : 256;
        if (!memcmp(src + o, orig + o, (size_t)block)) { o += block; continue; }
        uint64_t end = o + block;
#if defined(__SSE2__)
        for (; end - o >= 16; o += 16) {
            __m128i result = _mm_loadu_si128((const __m128i *)(src + o));
            __m128i before = _mm_loadu_si128((const __m128i *)(orig + o));
            unsigned changed = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(result, before)) ^ 0xffffu;
            if (changed == 0xffffu) {
                _mm_storeu_si128((__m128i *)(dst + o), result);
            } else {
                while (changed) {
                    unsigned k = (unsigned)__builtin_ctz(changed);
                    dst[o + k] = src[o + k];
                    changed &= changed - 1;
                }
            }
        }
#endif
        for (; o < end; o++) if (src[o] != orig[o]) dst[o] = src[o];
    }
}
