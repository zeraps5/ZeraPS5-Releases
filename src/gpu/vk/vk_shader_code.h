/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_VK_SHADER_CODE_H
#define PS5C_VK_SHADER_CODE_H
#include "gpu/shader/shader.h"

/* A binary-halved fixed window can cut a valid shader in half near the end
 * of its mapping. Use the exact remaining readable range instead. As with
 * gmem_host, this does not pin the caller-owned code mapping. */
static inline const uint32_t *vk_shader_code(gmem *mem, guest_addr addr, uint32_t *dwords) {
    *dwords = 0;
    gmem_vma v;
    if ((addr & 3) || !gmem_query_vma(mem, addr, false, &v) || v.reserved ||
        v.prot == GPROT_NONE || addr < v.start || addr - v.start >= v.size) return NULL;
    const uint64_t cap = (uint64_t)SH_MAX_CODE_DWORDS * 4;
    uint64_t bytes = v.size - (addr - v.start);
    if (bytes > cap) bytes = cap;
    /* Protection changes can split one code allocation into adjacent VMAs.
     * Match gmem_host's host-access contract, without crossing a gap or an
     * inaccessible/reserved region. The total window is still bounded. */
    while (bytes < cap && addr <= UINT64_MAX - bytes) {
        gmem_vma next;
        if (!gmem_query_vma(mem, addr + bytes, false, &next) || next.start != addr + bytes ||
            !next.size || next.reserved || next.prot == GPROT_NONE) break;
        uint64_t add = next.size < cap - bytes ? next.size : cap - bytes;
        bytes += add;
    }
    uint64_t words = bytes / 4;
    if (!words) return NULL;
    const uint32_t *code = (const uint32_t *)gmem_host(mem, addr, words * 4);
    if (code) *dwords = (uint32_t)words;
    return code;
}
#endif
