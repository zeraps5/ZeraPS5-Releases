/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mem/gmem_access.h"

bool gmem_access_begin(gmem_access *a, gmem *mem, bool retain) {
    if (a->pinned) return a->mem == mem;
    uint64_t generation;
    if (!gmem_pin_mappings_version(mem, retain, &generation)) return false;
    if (a->mem != mem || a->generation != generation) a->count = a->next = 0;
    if (a->mem != mem) a->hole_count = a->hole_next = 0;
    a->mem = mem;
    a->generation = generation;
    a->pinned = true;
    return true;
}

void gmem_access_end(gmem_access *a) {
    if (!a->pinned) return;
    a->pinned = false;
    gmem_unpin_mappings(a->mem);
}

void *gmem_access_host(gmem_access *a, guest_addr addr, uint64_t size) {
    if (!a->pinned || size > UINT64_MAX - addr) return NULL;
    guest_addr end = addr + size;
    for (uint32_t i = 0; i < a->count; i++)
        if (addr >= a->spans[i].start && end <= a->spans[i].end)
            return a->spans[i].host + (addr - a->spans[i].start);
    /* holes: against the live generation, since memory may be placed in free
     * space while pinned (gmem: mapping_place_wait does not wait for pins) */
    uint64_t now = __atomic_load_n(&a->mem->mapping_generation, __ATOMIC_SEQ_CST);
    if (a->hole_count && a->hole_generation == now)
        for (uint32_t i = 0; i < a->hole_count; i++)
            if (addr >= a->holes[i].start && addr < a->holes[i].end) return NULL;
    uint8_t *p = gmem_host(a->mem, addr, size);
    if (!p) {
        guest_addr lo, hi;
        uint64_t generation;
        if (gmem_hole(a->mem, addr, &lo, &hi, &generation)) {
            if (generation != a->hole_generation) a->hole_count = a->hole_next = 0, a->hole_generation = generation;
            uint32_t i = a->hole_count < PS5C_ARRAY_COUNT(a->holes) ? a->hole_count++ : a->hole_next++ % PS5C_ARRAY_COUNT(a->holes);
            a->holes[i].start = lo;
            a->holes[i].end = hi;
        }
        return NULL;
    }
    guest_addr start = addr;
    uint8_t *host = p;
    gmem_vma v;
    /* Demand-backed ranges must pass ensure_backing for each newly accessed
     * interval; validating one page does not materialize the rest of the VMA. */
    if (gmem_query_vma(a->mem, addr, false, &v) && !v.reserved && !v.demand &&
        v.start <= addr && v.size <= UINT64_MAX - v.start && v.start + v.size >= end &&
        gmem_is_mapped(a->mem, v.start, v.size)) {
        uint8_t *base = gmem_host_unlocked(a->mem, v.start);
        if (base) { start = v.start; end = v.start + v.size; host = base; }
    }
    uint32_t i = a->count < PS5C_ARRAY_COUNT(a->spans) ? a->count++ : a->next++ % PS5C_ARRAY_COUNT(a->spans);
    a->spans[i].start = start;
    a->spans[i].end = end;
    a->spans[i].host = host;
    return p;
}
