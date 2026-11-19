/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Guest memory: the PS5 virtual address space, backed by host memory.
 *
 * Guest addresses (guest_addr) and host pointers are distinct types in the
 * API; code that needs to touch guest memory from the host asks gmem_host()
 * for a pointer to a mapped range. In identity mode (required by the native
 * x86-64 CPU backend, which dereferences guest pointers directly) the host
 * pointer equals the guest address; in offset mode it is host_base + addr.
 *
 * Memory is managed in guest pages of 16 KiB. Every mapping is a VMA with a
 * name and a protection, kept sorted by address. */
#ifndef PS5C_GMEM_H
#define PS5C_GMEM_H

#include "base/base.h"
#include "base/dynarray.h"

typedef uint64_t guest_addr;

#define GUEST_PAGE_SIZE 0x4000ull

/* PS5 address space layout used by the loader (same scheme as KytyPS5):
 * system-reserved area where modules are loaded. */
#define GUEST_SYSTEM_RESERVED_BASE 0x0000000200000000ull
#define GUEST_SYSTEM_RESERVED_SIZE 0x0000000E00000000ull /* 56 GiB (libc maps its heap at 0x300000000: Astro Bot) */
/* The whole guest range the emulator reserves up front: modules plus room for
 * games that reserve huge virtual ranges (Unreal asks for 512 GiB at
 * 0x1000000000). Address space only: pages are committed when mapped. */
#define GUEST_SPACE_END 0x0000010000000000ull
/* The top of the guest range is placeholder address space (where the host
 * supports it): memory there is placed piece by piece and direct memory is
 * mapped as views of one shared object, so a physical page can appear at
 * several addresses (PS5 aliasing: mirrored ring buffers, second views). */
#define GUEST_PH_BASE 0x000000C000000000ull
#define GUEST_SHM_SIZE (16ull << 30) /* physical (direct) memory object */
#define GUEST_MODULE_BASE 0x0000000900000000ull

/* Protection flags (same bits as PAL_PROT_*). */
enum { GPROT_NONE = 0, GPROT_READ = 1, GPROT_WRITE = 2, GPROT_EXEC = 4 };
#define GPROT_RW (GPROT_READ | GPROT_WRITE)
#define GPROT_RX (GPROT_READ | GPROT_EXEC)

enum {
    GMAP_FIXED = 1,
    GMAP_RESERVE = 2,     /* address space only: no memory, not accessible (VMA.reserved) */
    GMAP_SHARED_AREA = 4, /* without GMAP_FIXED: place it in the placeholder area */
    GMAP_REPLACE = 8,     /* with GMAP_FIXED: unmap what is there first, atomically */
    GMAP_DEMAND = 16,     /* caller selected a non-stack graphics pool for the Windows experiment */
};

/* What backs a VMA in the placeholder area (os_* is the host mapping it is part of) */
enum { GVMA_PLAIN = 0, GVMA_PRIVATE = 1, GVMA_VIEW = 2 };

typedef struct gmem_vma {
    guest_addr start;
    uint64_t size;
    uint32_t prot;
    bool reserved; /* placeholder from GMAP_RESERVE: no backing memory */
    bool demand;   /* accessible anonymous range, committed in pieces on host faults */
    bool flexible; /* charged to the live flexible-memory budget; survives VMA splits */
    uint8_t kind;  /* GVMA_* */
    bool title_image; /* loaded executable pages charged to the flexible budget */
    guest_addr os_base;
    uint64_t os_size;
    uint64_t phys; /* views: offset in the physical memory object of os_base */
    char name[40];
} gmem_vma;

typedef struct pal_mutex pal_mutex;
typedef struct pal_cond pal_cond;

typedef struct gmem {
    bool identity;
    uint8_t *host_base;   /* host = host_base + addr (offset mode) */
    void *reservation;    /* host address of the reservation */
    guest_addr res_start; /* guest range covered by the reservation */
    guest_addr res_end;
    guest_addr alloc_hint; /* where "anywhere" allocations start searching */
    DYNARRAY(gmem_vma) vmas;
    pal_mutex *lock;
    pal_cond *mapping_idle;
    uint32_t mapping_pins, mapping_writers;
    uint64_t mapping_generation;
    bool closing;
    bool demand_anonymous; /* Windows experiment for explicitly selected graphics pools */
    uint64_t mapped_bytes;
    guest_addr ph_start; /* placeholder area [ph_start, res_end); res_end when there is none */
    void *shm;           /* physical memory object for views */
    uint8_t *phys_win;   /* host view of the whole object (gmem_phys_window), made when first asked for */
    struct gmem_history *history; /* optional bounded map history for fault diagnostics */
} gmem;

typedef struct gmem_config {
    guest_addr start; /* page aligned */
    uint64_t size;
    bool identity; /* reserve at the same host address (native CPU backend) */
    guest_addr alloc_hint;
    bool demand_anonymous;
} gmem_config;

/* Default: identity mapping of the system-reserved area. */
gmem_config gmem_default_config(void);

/* Reserves the default identity range right away. Call first thing in main():
 * later, host allocations may already occupy it. gmem_init adopts it. */
ps5c_err gmem_reserve_early(void);

/* Starts the shared-memory area (where views of one physical page can sit at
 * several addresses) at `base` instead of GUEST_PH_BASE, for programs that map
 * aliases at fixed addresses below it. Before gmem_init: the early reservation
 * is made again. PS5C_LOW_SHARED_AREA, when set, wins. */
ps5c_err gmem_set_shared_area_base(guest_addr base);

ps5c_err gmem_init(gmem *m, const gmem_config *cfg);
void gmem_free(gmem *m);

/* Maps `size` bytes (rounded up to guest pages), zero-filled. Without
 * GMAP_FIXED, `addr` is a hint (0 = any). Fails with PS5C_ERR_BUSY if a fixed
 * range overlaps an existing mapping. */
ps5c_err gmem_map(gmem *m, guest_addr addr, uint64_t size, uint32_t prot, uint32_t flags,
                  const char *name, guest_addr *out);
/* Same, placing the mapping at a multiple of `align` (a power of two). */
ps5c_err gmem_map_aligned(gmem *m, guest_addr addr, uint64_t size, uint64_t align, uint32_t prot,
                          uint32_t flags, const char *name, guest_addr *out);
/* Flexible quota is checked under the mapping lock before replacement. Only
 * live, tagged VMAs count; unmap/replacement/protection share that metadata.
 * Optional replacement_started reports entry into destructive replacement,
 * even on failure; it does not promise rollback of the old backing. */
ps5c_err gmem_map_flexible(gmem *m, guest_addr addr, uint64_t size, uint64_t align, uint32_t prot,
                           uint32_t flags, uint64_t budget, const char *name, guest_addr *out, bool *replacement_started);
uint64_t gmem_flexible_used(gmem *m);
/* Unmaps every page in [addr, addr + size) (munmap semantics: VMAs are split,
 * unmapped holes are ignored). */
ps5c_err gmem_unmap(gmem *m, guest_addr addr, uint64_t size);
/* Changes protection; the range must be fully mapped or reserved (reserved
 * parts get memory unless prot is GPROT_NONE). Splits VMAs as needed. */
ps5c_err gmem_protect(gmem *m, guest_addr addr, uint64_t size, uint32_t prot);
/* Finalize a loaded executable range. Accessible pages count against the
 * flexible budget until their backing is unmapped or replaced. */
ps5c_err gmem_protect_title_image(gmem *m, guest_addr addr, uint64_t size, uint32_t prot);

/* Host pointer for [addr, addr + size) if the whole range is mapped, else NULL.
 * Host-side access ignores guest protection but not host protection: write only
 * to ranges whose current protection includes GPROT_WRITE. */
void *gmem_host(gmem *m, guest_addr addr, uint64_t size);
/* Host pointer for an address the caller knows is mapped: no lookup, no lock. */
void *gmem_host_unlocked(gmem *m, guest_addr addr);
bool gmem_is_mapped(gmem *m, guest_addr addr, uint64_t size);
/* Borrowed pointer, invalidated by map/unmap/protect. Concurrent callers use
 * gmem_query_vma to copy the metadata while holding the map lock. */
const gmem_vma *gmem_find_vma(gmem *m, guest_addr addr);
/* Copies the containing VMA, or the next VMA in a gap when find_next is true.
 * On failure, out is unchanged. The copy does not pin the mapped pages. */
bool gmem_query_vma(gmem *m, guest_addr addr, bool find_next, gmem_vma *out);

/* Protect mapping lifetimes while an external device uses host pages. This
 * does not freeze their contents: ordinary reads/writes and queries still work.
 * Map/unmap/protect wait until every pin is released. A holder must not mutate
 * mappings, and must finish device work/destroy imports before unpinning. */
bool gmem_pin_mappings(gmem *m);
/* Captures the mapping generation under the same lock as the pin. retain=true
 * extends a pin the caller already owns, without waiting behind a writer that
 * is itself waiting for that pin. It must not be used to start a new access. */
bool gmem_pin_mappings_version(gmem *m, bool retain, uint64_t *generation);
bool gmem_retain_mappings(gmem *m);
void gmem_unpin_mappings(gmem *m);

/* Maps physical memory [phys, phys + size): a view of the shared object where
 * the destination is in the placeholder area (*shared = true, every view of a
 * page shows the same bytes), else plain zeroed memory (*shared = false). */
ps5c_err gmem_map_direct(gmem *m, guest_addr addr, uint64_t size, uint64_t align, uint32_t prot, uint32_t flags,
                         uint64_t phys, const char *name, guest_addr *out, bool *shared);

/* A guest access to addr faulted: true when it can be retried because the
 * page is mapped with that access now (a remap in the shared-memory area was
 * in progress on another thread when it faulted). */
bool gmem_retry_fault(gmem *m, guest_addr addr, bool write);
/* Commit a valid demand-backed page for READ/WRITE/EXEC access. Invalid access
 * and unmapped/reserved ranges remain faults. No mapping lifetime is changed. */
bool gmem_commit_fault(gmem *m, guest_addr addr, uint32_t access);
/* fn(addr, size) after every change of mappings or protection (map, unmap, protect, demand
 * commits), with the map lock held: the GPU write tracker resets those pages (gpu_watch.h). */
void gmem_set_change_hook(void (*fn)(guest_addr addr, uint64_t size));
/* Hooks of the copy-on-write write tracker (gpu_watch.h), both called with the map lock held.
 * release(addr, size, phys_copy): the view [addr, addr + size) is about to be unmapped (for
 * good, or to be mapped again in parts); phys_copy shows the same physical memory through the
 * host's own view, or is NULL when there is none. Pages the tracker let become private copies
 * must be written there now, or their contents are lost.
 * aliased(addr, size): another view of the physical memory of the view [addr, addr + size) is
 * about to be mapped; true when the tracker has pages armed (or private) there, and then the
 * view is unmapped and mapped again (release runs) so that both views share every page. */
void gmem_set_cow_hooks(void (*release)(guest_addr addr, uint64_t size, uint8_t *phys_copy),
                        bool (*aliased)(guest_addr addr, uint64_t size));
/* Stops every guest thread but the calling one (true when it did) and lets them go again. The
 * copy-on-write release runs between the two: a guest write that lands between the copy of a
 * private page and the unmap of its view would be lost, and a guest access to a neighbour part
 * of the view while it is being placed again would fault. Nothing between stop and resume may
 * take a lock a guest thread can hold (heap, log, runtime locks). */
void gmem_set_world_hooks(bool (*stop)(void *user), void (*resume)(void *user), void *user);
/* Another backed view than the one at `self` shows physical memory of [phys, phys + size). */
bool gmem_phys_aliased(gmem *m, guest_addr self, uint64_t phys, uint64_t size);
/* The host's view of the whole physical memory object (NULL: none). */
uint8_t *gmem_phys_window(gmem *m);

/* PS5C_FAULT_MAPS: log copied VMAs and recent operations at a terminal fault.
 * Diagnostic only; does not change mappings or retry an invalid access. */
void gmem_log_fault_state(gmem *m, guest_addr addr);

/* Copy while holding the map lock: map/unmap/protect cannot invalidate the
 * guest range during memcpy. Caller supplies a valid host buffer and suitable
 * host protection; native guest/CPU/GPU writes are not synchronized by this
 * lock. Failed range validation leaves the destination unchanged. */
bool gmem_read(gmem *m, guest_addr addr, void *dst, uint64_t size);
bool gmem_write(gmem *m, guest_addr addr, const void *src, uint64_t size);
bool gmem_write_u64(gmem *m, guest_addr addr, uint64_t value);
bool gmem_read_u64(gmem *m, guest_addr addr, uint64_t *value);
/* Aligned native atomic RMW, with mapping lifetime protected until completion.
 * Requires a readable/writable VMA. On a valid comparison, exchanged reports
 * success and expected receives the observed value on mismatch. Invalid ranges
 * leave both outputs unchanged. Guest CPU atomics participate in the same RMW. */
bool gmem_compare_exchange_u64(gmem *m, guest_addr addr, uint64_t *expected,
                               uint64_t desired, bool *exchanged);

#endif
