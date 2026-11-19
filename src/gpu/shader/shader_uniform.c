/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/shader/shader.h"
#include <string.h>

PS5C_STATIC_ASSERT(SH_U_VPORT == SH_U_SITE + SH_MAX_SITES, "site metadata precedes the uniform tail");
PS5C_STATIC_ASSERT(!(SH_U_VPORT & 3u) && !(SH_MAX_SITES & 3u), "uniform tail is 16-byte aligned");

uint32_t sh_uniform_compact_shift(uint32_t site_count) {
    if (site_count >= SH_MAX_SITES) return 0;
    /* Keep at least one zero padding word for reads into the omitted gap. */
    return (SH_MAX_SITES - site_count - 1) & ~3u;
}

bool sh_uniform_shift_valid(uint32_t site_count, uint32_t shift) {
    return site_count <= SH_MAX_SITES && (!shift || shift == sh_uniform_compact_shift(site_count));
}

uint32_t sh_uniform_index(uint32_t shift, uint32_t logical) {
    if (!shift) return logical;
    uint32_t cut = SH_U_VPORT - shift;
    if (logical >= SH_U_VPORT) return logical - shift;
    return logical >= cut ? cut - 1 : logical;
}

bool sh_uniform_pack(uint32_t *dst, size_t capacity_words, const uint32_t *src,
                     uint32_t logical_words, uint32_t site_count, uint32_t shift) {
    if (!dst || !src || logical_words < SH_U_DATA || !sh_uniform_shift_valid(site_count, shift) ||
        capacity_words < logical_words - shift) return false;
    size_t src_bytes = (size_t)logical_words * sizeof(*src);
    size_t dst_bytes = (size_t)(logical_words - shift) * sizeof(*dst);
    uintptr_t source = (uintptr_t)src, destination = (uintptr_t)dst;
    /* Reject partial overlap before touching either buffer. In-place packing
     * is safe because the tail moves down, but memcpy of an overlapping
     * prefix is not. Check address arithmetic before constructing the ends. */
    if (src_bytes > UINTPTR_MAX - source || dst_bytes > UINTPTR_MAX - destination ||
        (dst != src && destination < source + src_bytes && source < destination + dst_bytes)) return false;
    if (!shift) {
        if (dst != src) memcpy(dst, src, src_bytes);
        return true;
    }
    uint32_t cut = SH_U_VPORT - shift;
    if (dst != src) memcpy(dst, src, (size_t)cut * sizeof(*dst));
    memmove(dst + cut, src + SH_U_VPORT, (size_t)(logical_words - SH_U_VPORT) * sizeof(*dst));
    /* A removed logical word aliases this reserved padding word. Metadata
     * values (including logical offsets into the tail) are copied verbatim. */
    dst[cut - 1] = 0;
    return true;
}
