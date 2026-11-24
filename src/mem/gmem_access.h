/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_GMEM_ACCESS_H
#define PS5C_GMEM_ACCESS_H

#include "mem/gmem.h"

/* Zero-initialize for each gmem lifetime; caller serializes this cache.
 * A scope protects mapping lifetimes, not the
 * contents: CPU writes may continue. Pointers expire at access_end. */
typedef struct gmem_access {
    gmem *mem;
    uint64_t generation;
    bool pinned;
    uint32_t count, next;
    struct { guest_addr start, end; uint8_t *host; } spans[32];
    /* address space with no backed memory, as of mapping generation hole_generation:
     * reads there fail without the map lock (scalar shader evaluation follows stale
     * pointers of unused descriptor slots: Tearaway ~200000 such reads a second) */
    uint64_t hole_generation;
    uint32_t hole_count, hole_next;
    struct { guest_addr start, end; } holes[8];
} gmem_access;

bool gmem_access_begin(gmem_access *access, gmem *mem, bool retain);
void gmem_access_end(gmem_access *access);
void *gmem_access_host(gmem_access *access, guest_addr addr, uint64_t size);

#endif
