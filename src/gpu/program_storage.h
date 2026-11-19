/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_PROGRAM_STORAGE_H
#define PS5C_PROGRAM_STORAGE_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/shader/shader.h"
#include "pal/pal.h"

_Static_assert(sizeof(sh_program) == PS5C_ALIGN_UP(offsetof(sh_program, back_branch) + sizeof(bool),
                                                  _Alignof(sh_program)),
               "update program_storage_copy when extending sh_program");

/* Storage for analyzed programs in the renderer cache. Sparse mode owns a
 * separate VM reservation: never decommit pages belonging to the C heap.
 * The mode is immutable for a program's lifetime. Call prepare before any
 * reanalysis, since decoding may grow all fixed-capacity arrays again. */
static inline sh_program *program_storage_alloc(bool sparse) {
    if (!sparse) return malloc(sizeof(sh_program));
    uint64_t size = PS5C_ALIGN_UP(sizeof(sh_program), pal_page_size());
    void *p = NULL;
    if (pal_vm_reserve(NULL, size, false, &p) != PS5C_OK) return NULL;
    if (pal_vm_commit(p, size, PAL_PROT_RW) != PS5C_OK) {
        pal_vm_release(p, size);
        return NULL;
    }
    return p;
}

static inline void program_storage_free(sh_program *p, bool sparse) {
    if (!p) return;
    if (sparse) pal_vm_release(p, PS5C_ALIGN_UP(sizeof(*p), pal_page_size()));
    else free(p);
}

static inline bool program_storage_prepare(sh_program *p, bool sparse) {
    return p && (!sparse || pal_vm_commit(p, PS5C_ALIGN_UP(sizeof(*p), pal_page_size()), PAL_PROT_RW) == PS5C_OK);
}

/* Boundary pages may contain the next array or scalar metadata. Release only
 * whole pages strictly within the unused tail of this particular array. */
static inline uint64_t program_storage_tail(sh_program *p, size_t offset, size_t used, size_t capacity) {
    uint64_t page = pal_page_size();
    size_t lo = (size_t)PS5C_ALIGN_UP(offset + used, page);
    size_t hi = (size_t)PS5C_ALIGN_DOWN(offset + capacity, page);
    if (hi <= lo) return 0;
    return pal_vm_decommit((uint8_t *)p + lo, hi - lo) == PS5C_OK ? hi - lo : 0;
}

static inline uint64_t program_storage_trim(sh_program *p, bool sparse) {
    if (!sparse || !p || p->count > SH_MAX_INSTS || p->site_count > SH_MAX_SITES) return 0;
    uint64_t saved = 0;
#define PROGRAM_TRIM(field, n) saved += program_storage_tail(p, offsetof(sh_program, field), \
        (size_t)(n) * sizeof(p->field[0]), sizeof(p->field))
    PROGRAM_TRIM(insts, p->count);
    PROGRAM_TRIM(ops, p->count);
    PROGRAM_TRIM(pc, p->count);
    PROGRAM_TRIM(sites, p->site_count);
    PROGRAM_TRIM(site_of, p->count);
    PROGRAM_TRIM(target, p->count);
    PROGRAM_TRIM(next_scalar, p->count + 1u);
    PROGRAM_TRIM(iflags, p->count);
    PROGRAM_TRIM(vmem_width, p->count);
#undef PROGRAM_TRIM
    return saved;
}

/* All consumers use count/site_count, plus next_scalar[count]. Copy metadata
 * explicitly; memcpy(sizeof(sh_program)) would read decommitted array tails.
 * No translator layout/ABI change and no truncation of a large program. */
static inline bool program_storage_copy(sh_program *dst, const sh_program *src, bool sparse) {
    if (!src || src->count > SH_MAX_INSTS || src->site_count > SH_MAX_SITES ||
        !program_storage_prepare(dst, sparse)) return false;
    if (dst == src) { program_storage_trim(dst, sparse); return true; }
    if (!sparse) { memcpy(dst, src, sizeof(*src)); return true; }
    memcpy(dst, src, offsetof(sh_program, insts));
#define PROGRAM_COPY(field, n) memcpy(dst->field, src->field, (size_t)(n) * sizeof(src->field[0]))
    PROGRAM_COPY(insts, src->count);
    PROGRAM_COPY(ops, src->count);
    PROGRAM_COPY(pc, src->count);
    PROGRAM_COPY(sites, src->site_count);
    PROGRAM_COPY(site_of, src->count);
    PROGRAM_COPY(target, src->count);
    PROGRAM_COPY(next_scalar, src->count + 1u);
    PROGRAM_COPY(iflags, src->count);
    PROGRAM_COPY(vmem_width, src->count);
#undef PROGRAM_COPY
    dst->count = src->count;
    dst->site_count = src->site_count;
    dst->param_mask = src->param_mask;
    dst->mrt_mask = src->mrt_mask;
    dst->attr_mask = src->attr_mask;
    dst->has_stores = src->has_stores;
    dst->wave32 = src->wave32;
    dst->has_flow = src->has_flow;
    dst->eval_forward_branches = src->eval_forward_branches;
    dst->back_branch = src->back_branch;
    dst->lane_reads = src->lane_reads;
    program_storage_trim(dst, sparse);
    return true;
}

#endif
