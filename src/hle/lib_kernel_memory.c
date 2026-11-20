/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libkernel memory: direct memory (physical pages the game allocates and maps
 * itself), flexible memory (anonymous pages from a budget), mmap/munmap/
 * mprotect and queries.
 *
 * Physical direct memory is bookkeeping only: each allocation is a range in a
 * separate "physical" space. Mappings are views of one shared memory object
 * where the host allows it; elsewhere they are private copies whose bytes
 * follow the physical pages from one mapping to the next (phys_save). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/dynarray.h"
#include "base/log.h"
#include "core/runtime.h"
#include "hle/amm.h"
#include "hle/hle.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

#define SCE_KERNEL_ERROR_EAGAIN ((int32_t)0x80020023)
#define SCE_KERNEL_ERROR_EACCES ((int32_t)0x8002000D)
#define SCE_KERNEL_ERROR_ENOENT ((int32_t)0x80020002)

/* Budgets of a PS5 game process */
/* Astro Bot allocates 12.2 GB up front (its libc heap included) and more after:
 * pages are only committed when mapped, so a larger budget costs nothing */
#define DIRECT_MEMORY_SIZE (14ull << 30)
#define FLEXIBLE_MEMORY_SIZE (448ull << 20)

/* Map flags */
#define MAP_SHARED 0x1
#define MAP_PRIVATE 0x2
#define MAP_FIXED 0x10
#define MAP_NO_OVERWRITE 0x80
#define MAP_ANON 0x1000
#define MAP_FAILED_ADDR ((uint64_t)-1)

/* SCE protection bits: CPU read/write/exec, GPU read/write */
#define SCE_PROT_CPU_READ 0x01
#define SCE_PROT_CPU_WRITE 0x02
#define SCE_PROT_CPU_EXEC 0x04
#define SCE_PROT_GPU_READ 0x10
#define SCE_PROT_GPU_WRITE 0x20
#define SCE_PROT_AMPR_READ 0x40  /* the I/O engine (APR) may read it */
#define SCE_PROT_AMPR_WRITE 0x80 /* ... and write it */

typedef struct dmem_block {
    uint64_t start, size; /* physical range */
    int32_t type;
    guest_addr mapped;    /* 0 = not mapped */
} dmem_block;

static DYNARRAY(dmem_block) g_dmem; /* sorted by start */

/* Live mappings of direct memory, to tell real aliasing (one physical page at
 * two addresses) from mapping different parts of one allocation. */
typedef struct dmem_map {
    uint64_t phys, len;
    guest_addr addr;
} dmem_map;
static DYNARRAY(dmem_map) g_dmaps;

/* Second views of direct memory placed on an existing view (see
 * map_direct_at): unmapping them leaves the memory alone. */
typedef struct dmem_alias {
    guest_addr addr;
    uint64_t len;
} dmem_alias;
static DYNARRAY(dmem_alias) g_aliases;

/* Caller holds g_lock. Forgets mappings inside [addr, addr + len). */
static void dmaps_forget(guest_addr addr, uint64_t len) {
    for (size_t i = 0; i < g_dmaps.len;) {
        dmem_map *m = &g_dmaps.data[i];
        if (m->addr >= addr + len || m->addr + m->len <= addr) {
            i++;
            continue;
        }
        if (m->addr >= addr && m->addr + m->len <= addr + len) {
            *m = g_dmaps.data[--g_dmaps.len];
            continue;
        }
        /* partly unmapped: keep the part below or above (a hole keeps both ends, approximated by the head) */
        if (m->addr < addr) {
            m->len = addr - m->addr;
        } else {
            uint64_t cut = addr + len - m->addr;
            m->addr += cut;
            m->phys += cut;
            m->len -= cut;
        }
        i++;
    }
}
static pal_mutex *g_lock;

/* Physical pages released since they were last mapped: the shared memory
 * object keeps their bytes, while the console hands out zeroed memory, so
 * the next view of them clears them first. One bit per 16 KiB page. */
static uint8_t g_stale[GUEST_SHM_SIZE / GUEST_PAGE_SIZE / 8];

static void stale_mark(uint64_t start, uint64_t end) {
    for (uint64_t p = start / GUEST_PAGE_SIZE; p < end / GUEST_PAGE_SIZE && p < sizeof(g_stale) * 8; p++)
        g_stale[p >> 3] |= (uint8_t)(1u << (p & 7));
}

/* Zeroes the stale pages of the view [addr, addr + len) of phys (then fresh). */
static void stale_clear(gmem *mem, guest_addr addr, uint64_t phys, uint64_t len, uint32_t gprot) {
    bool any = false;
    for (uint64_t o = 0; o < len && !any; o += GUEST_PAGE_SIZE) {
        uint64_t p = (phys + o) / GUEST_PAGE_SIZE;
        any = p < sizeof(g_stale) * 8 && (g_stale[p >> 3] >> (p & 7)) & 1;
    }
    if (!any) return;
    if (!(gprot & GPROT_WRITE)) gmem_protect(mem, addr, len, GPROT_RW);
    for (uint64_t o = 0; o < len; o += GUEST_PAGE_SIZE) {
        uint64_t p = (phys + o) / GUEST_PAGE_SIZE;
        if (p >= sizeof(g_stale) * 8 || !((g_stale[p >> 3] >> (p & 7)) & 1)) continue;
        memset(gmem_host(mem, addr + o, GUEST_PAGE_SIZE), 0, GUEST_PAGE_SIZE);
        g_stale[p >> 3] &= (uint8_t)~(1u << (p & 7));
    }
    if (!(gprot & GPROT_WRITE)) gmem_protect(mem, addr, len, gprot);
}

/* Direct memory mapped where views cannot go (fixed addresses below the
 * shared-memory area) is a private copy. While it is not mapped its bytes are
 * kept in the physical memory object: saved when the mapping goes, loaded when
 * the memory is mapped again. Games move pools around in virtual memory this
 * way (GTA V's streaming heap remaps loaded files). */
static uint8_t *g_phys_win; /* host view of the whole physical memory object */
static uint64_t g_phys_saved;

/* Caller holds g_lock. */
static uint8_t *phys_window(gmem *mem) {
    static bool failed;
    if (!g_phys_win && mem->shm && !failed) {
        void *p = NULL;
        if (pal_shm_map(mem->shm, GUEST_SHM_SIZE, &p) == PS5C_OK) g_phys_win = (uint8_t *)p;
        else {
            failed = true;
            LOG_W(KERNEL, "no host view of physical memory: remapped direct memory will lose its contents");
        }
    }
    return g_phys_win;
}

static bool stale_page(uint64_t phys) {
    uint64_t p = phys / GUEST_PAGE_SIZE;
    return p < sizeof(g_stale) * 8 && (g_stale[p >> 3] >> (p & 7)) & 1;
}

/* Caller holds g_lock. Saves the private direct mappings in [addr, addr + len). */
static void phys_save(gmem *mem, guest_addr addr, uint64_t len) {
    for (size_t i = 0; i < g_dmaps.len; i++) {
        const dmem_map *m = &g_dmaps.data[i];
        if (m->addr >= mem->ph_start) continue; /* a view: the object has the bytes */
        guest_addr a = PS5C_MAX(m->addr, addr), b = PS5C_MIN(m->addr + m->len, addr + len);
        if (a >= b) continue;
        uint64_t phys = m->phys + (a - m->addr);
        if (phys + (b - a) > GUEST_SHM_SIZE) continue;
        uint8_t *win = phys_window(mem);
        if (!win) return;
        gmem_vma snapshot;
        if (!gmem_query_vma(mem, a, false, &snapshot) || snapshot.reserved) continue;
        uint8_t *src = gmem_host(mem, a, b - a);
        if (!src) continue;
        if (pal_vm_commit(win + phys, b - a, GPROT_RW) != PS5C_OK) {
            LOG_W(KERNEL, "direct memory 0x%llx+0x%llx: no memory to keep its contents", (unsigned long long)phys,
                  (unsigned long long)(b - a));
            continue;
        }
        uint32_t prot = snapshot.prot;
        if (!(prot & GPROT_READ)) pal_vm_protect(src, b - a, GPROT_READ);
        memcpy(win + phys, src, (size_t)(b - a));
        if (!(prot & GPROT_READ)) pal_vm_protect(src, b - a, prot);
        g_phys_saved += b - a;
        LOG_D(KERNEL, "direct memory 0x%llx+0x%llx at 0x%llx: contents kept (%llu MB so far)", (unsigned long long)phys,
              (unsigned long long)(b - a), (unsigned long long)a, (unsigned long long)(g_phys_saved >> 20));
    }
}

/* Caller holds g_lock. Fills the new private mapping [addr, addr + len) of phys
 * with the bytes kept for it (released pages stay zero). */
static void phys_load(gmem *mem, guest_addr addr, uint64_t phys, uint64_t len, uint32_t gprot) {
    if (phys + len > GUEST_SHM_SIZE) return;
    uint8_t *win = g_phys_win; /* none yet: nothing was kept */
    bool opened = false;
    uint64_t run_end = 0, loaded = 0;
    bool committed = false;
    for (uint64_t o = 0; o < len; o += GUEST_PAGE_SIZE) {
        uint64_t p = phys + o;
        if (stale_page(p)) {
            if (win && pal_vm_committed(win + p, &(uint64_t){0})) memset(win + p, 0, GUEST_PAGE_SIZE);
            uint64_t n = p / GUEST_PAGE_SIZE;
            g_stale[n >> 3] &= (uint8_t)~(1u << (n & 7));
            continue;
        }
        if (!win) continue;
        if (p >= run_end) {
            uint64_t run = 0;
            committed = pal_vm_committed(win + p, &run);
            run_end = p + (run ? run : GUEST_PAGE_SIZE);
        }
        if (!committed) continue;
        if (!opened && !(gprot & GPROT_WRITE)) gmem_protect(mem, addr, len, GPROT_RW);
        opened = true;
        memcpy(gmem_host_unlocked(mem, addr + o), win + p, GUEST_PAGE_SIZE);
        loaded += GUEST_PAGE_SIZE;
    }
    if (opened && !(gprot & GPROT_WRITE)) gmem_protect(mem, addr, len, gprot);
    if (loaded)
        LOG_D(KERNEL, "direct memory 0x%llx+0x%llx at 0x%llx: 0x%llx bytes of kept contents loaded",
              (unsigned long long)phys, (unsigned long long)len, (unsigned long long)addr, (unsigned long long)loaded);
}

/* GPU access is emulated by the CPU, so GPU bits imply CPU access. */
static uint32_t to_gprot(uint32_t prot) {
    uint32_t g = 0;
    if (prot & (SCE_PROT_CPU_READ | SCE_PROT_CPU_WRITE | SCE_PROT_GPU_READ | SCE_PROT_GPU_WRITE)) g |= GPROT_READ;
    if (prot & (SCE_PROT_CPU_WRITE | SCE_PROT_GPU_WRITE)) g |= GPROT_WRITE;
    if (prot & SCE_PROT_CPU_EXEC) g |= GPROT_EXEC | GPROT_READ;
    return g;
}

/* The emulated I/O engine reaches all memory: AMPR access goes with CPU
 * access (GTA V reads files straight into buffers it may write, else through
 * a bounce buffer). */
static uint32_t to_sce_prot(uint32_t g) {
    uint32_t p = 0;
    if (g & GPROT_READ) p |= SCE_PROT_CPU_READ | SCE_PROT_GPU_READ | SCE_PROT_AMPR_READ;
    if (g & GPROT_WRITE) p |= SCE_PROT_CPU_WRITE | SCE_PROT_GPU_WRITE | SCE_PROT_AMPR_WRITE;
    if (g & GPROT_EXEC) p |= SCE_PROT_CPU_EXEC;
    return p;
}

static bool is_pow2(uint64_t v) { return v && !(v & (v - 1)); }

/* ---- Direct memory ---- */

/* First fit in [start, end) at `align`. Caller holds g_lock. */
static bool dmem_find(uint64_t start, uint64_t end, uint64_t size, uint64_t align, uint64_t *out) {
    uint64_t cand = PS5C_ALIGN_UP(start, align);
    for (size_t i = 0; i <= g_dmem.len; i++) {
        uint64_t limit = i < g_dmem.len ? g_dmem.data[i].start : DIRECT_MEMORY_SIZE;
        if (limit > end) limit = end;
        if (limit >= cand && limit - cand >= size) {
            *out = cand;
            return true;
        }
        if (i < g_dmem.len) {
            cand = PS5C_ALIGN_UP(PS5C_MAX(cand, g_dmem.data[i].start + g_dmem.data[i].size), align);
        }
        if (cand >= end) break;
    }
    return false;
}

static int32_t dmem_allocate(uint64_t start, uint64_t end, uint64_t size, uint64_t align, int32_t type,
                             uint64_t *out) {
    if (!align) align = GUEST_PAGE_SIZE;
    if (!size || size % GUEST_PAGE_SIZE || !is_pow2(align) || align % GUEST_PAGE_SIZE || start >= end) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    if (end > DIRECT_MEMORY_SIZE) end = DIRECT_MEMORY_SIZE;
    pal_mutex_lock(g_lock);
    uint64_t at = 0;
    int32_t ret = SCE_KERNEL_ERROR_EAGAIN;
    if (dmem_find(start, end, size, align, &at)) {
        dmem_block b = {at, size, type, 0};
        size_t i = 0;
        while (i < g_dmem.len && g_dmem.data[i].start < at) i++;
        if (dyn_reserve(&g_dmem, g_dmem.len + 1)) {
            memmove(&g_dmem.data[i + 1], &g_dmem.data[i], (g_dmem.len - i) * sizeof(dmem_block));
            g_dmem.data[i] = b;
            g_dmem.len++;
            *out = at;
            ret = 0;
        } else {
            ret = SCE_KERNEL_ERROR_ENOMEM;
        }
    }
    pal_mutex_unlock(g_lock);
    return ret;
}

/* int sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len,
 *                                   size_t align, int type, off_t *phys_out) */
static void allocate_direct_memory(hle_call *c) {
    uint64_t *out = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 5), 8);
    uint64_t phys = 0;
    int32_t ret = out ? dmem_allocate(hle_arg(c, 0), hle_arg(c, 1), hle_arg(c, 2), hle_arg(c, 3),
                                      hle_arg_s32(c, 4), &phys)
                      : SCE_KERNEL_ERROR_EINVAL;
    if (ret == 0) *out = phys;
    LOG_D(KERNEL, "AllocateDirectMemory(len 0x%llx, align 0x%llx) -> 0x%llx (0x%x)",
          (unsigned long long)hle_arg(c, 2), (unsigned long long)hle_arg(c, 3), (unsigned long long)phys,
          (unsigned)ret);
    hle_ret_s32(c, ret);
}

/* int sceKernelAllocateMainDirectMemory(size_t len, size_t align, int type, off_t *phys_out) */
static void allocate_main_direct_memory(hle_call *c) {
    uint64_t *out = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 3), 8);
    uint64_t phys = 0;
    int32_t ret = out ? dmem_allocate(0, DIRECT_MEMORY_SIZE, hle_arg(c, 0), hle_arg(c, 1), hle_arg_s32(c, 2), &phys)
                      : SCE_KERNEL_ERROR_EINVAL;
    if (ret == 0) *out = phys;
    hle_ret_s32(c, ret);
}

/* int sceKernelReleaseDirectMemory(off_t start, size_t len) */
/* Releases every allocated page in [start, start + len): blocks inside are
 * dropped, blocks crossing an edge are trimmed or split. `checked` fails with
 * ENOENT when nothing in the range was allocated. */
static int32_t release_direct(uint64_t start, uint64_t len, bool checked) {
    if (!len || start % GUEST_PAGE_SIZE || len % GUEST_PAGE_SIZE) return SCE_KERNEL_ERROR_EINVAL;
    uint64_t end = start + len;
    bool touched = false;
    int32_t ret = 0;
    pal_mutex_lock(g_lock);
    stale_mark(start, end);
    for (size_t i = 0; i < g_dmem.len;) {
        dmem_block *b = &g_dmem.data[i];
        uint64_t bs = b->start, be = b->start + b->size;
        if (be <= start || bs >= end) {
            i++;
            continue;
        }
        touched = true;
        if (bs >= start && be <= end) { /* fully released */
            memmove(b, b + 1, (g_dmem.len - i - 1) * sizeof(dmem_block));
            g_dmem.len--;
            continue;
        }
        if (bs < start && be > end) { /* hole in the middle: split */
            if (!dyn_reserve(&g_dmem, g_dmem.len + 1)) {
                ret = SCE_KERNEL_ERROR_ENOMEM;
                break;
            }
            b = &g_dmem.data[i];
            dmem_block tail = *b;
            tail.start = end;
            tail.size = be - end;
            tail.mapped = 0;
            b->size = start - bs;
            memmove(&g_dmem.data[i + 2], &g_dmem.data[i + 1], (g_dmem.len - i - 1) * sizeof(dmem_block));
            g_dmem.data[i + 1] = tail;
            g_dmem.len++;
            i += 2;
            continue;
        }
        if (bs < start) b->size = start - bs; /* keep the head */
        else {                                /* keep the tail */
            b->size = be - end;
            b->start = end;
            b->mapped = 0;
        }
        i++;
    }
    pal_mutex_unlock(g_lock);
    if (ret == 0 && checked && !touched) ret = SCE_KERNEL_ERROR_ENOENT;
    return ret;
}

static void release_direct_memory(hle_call *c) { hle_ret_s32(c, release_direct(hle_arg(c, 0), hle_arg(c, 1), false)); }
static void checked_release_direct_memory(hle_call *c) {
    hle_ret_s32(c, release_direct(hle_arg(c, 0), hle_arg(c, 1), true));
}

/* int sceKernelDirectMemoryQuery(off_t offset, int flags, info *out, size_t size):
 * the allocation at offset (flags & 1: or the next one), as {start, end, s32 type}
 * (GTA V walks its allocations with it) */
static void direct_memory_query(hle_call *c) {
    uint64_t off = hle_arg(c, 0);
    bool next = hle_arg_u32(c, 1) & 1;
    guest_addr out = hle_arg_ptr(c, 2);
    uint64_t size = hle_arg(c, 3);
    if (!out) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    const dmem_block *hit = NULL;
    pal_mutex_lock(g_lock);
    for (size_t i = 0; i < g_dmem.len && !hit; i++) {
        const dmem_block *b = &g_dmem.data[i];
        if ((off >= b->start && off < b->start + b->size) || (next && b->start >= off)) hit = b;
    }
    uint64_t info[3] = {0};
    if (hit) {
        info[0] = hit->start;
        info[1] = hit->start + hit->size;
        info[2] = (uint64_t)(uint32_t)hit->type;
    }
    pal_mutex_unlock(g_lock);
    if (!hit) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EACCES);
        return;
    }
    gmem_write(c->mem, out, info, size && size < sizeof(info) ? size : 20);
    hle_ret_s32(c, 0);
}

static void get_direct_memory_size(hle_call *c) { hle_ret_u64(c, DIRECT_MEMORY_SIZE); }

/* int sceKernelAvailableDirectMemorySize(off_t start, off_t end, size_t align,
 *                                        off_t *phys_out, size_t *size_out) */
static void available_direct_memory_size(hle_call *c) {
    uint64_t start = hle_arg(c, 0), end = hle_arg(c, 1), align = hle_arg(c, 2);
    uint64_t *phys_out = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 3), 8);
    uint64_t *size_out = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 4), 8);
    if (!align) align = GUEST_PAGE_SIZE;
    if (!phys_out || !size_out || !is_pow2(align)) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    if (end > DIRECT_MEMORY_SIZE) end = DIRECT_MEMORY_SIZE;
    /* Largest free, aligned range */
    uint64_t best_at = 0, best = 0;
    pal_mutex_lock(g_lock);
    uint64_t cursor = start;
    for (size_t i = 0; i <= g_dmem.len; i++) {
        uint64_t limit = i < g_dmem.len ? PS5C_MIN(g_dmem.data[i].start, end) : end;
        uint64_t cand = PS5C_ALIGN_UP(cursor, align);
        if (limit > cand && limit - cand > best) {
            best = limit - cand;
            best_at = cand;
        }
        if (i < g_dmem.len) cursor = PS5C_MAX(cursor, g_dmem.data[i].start + g_dmem.data[i].size);
    }
    pal_mutex_unlock(g_lock);
    if (!best) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_ENOMEM);
        return;
    }
    *phys_out = best_at;
    *size_out = best;
    hle_ret_s32(c, 0);
}

/* ---- Mapping ---- */

/* Common path of every map call. `*addr` is in/out. */
static int32_t map_memory(hle_call *c, guest_addr *addr, uint64_t len, uint32_t prot, uint32_t flags,
                          uint64_t align, const char *name, bool demand,bool flexible) {
    if (!len || len % GUEST_PAGE_SIZE) return SCE_KERNEL_ERROR_EINVAL;
    if (align && (!is_pow2(align) || align % GUEST_PAGE_SIZE)) return SCE_KERNEL_ERROR_EINVAL;
    gmem *mem = c->mem;
    uint32_t gflags = 0;
    if (flags & MAP_FIXED) {
        if (*addr % GUEST_PAGE_SIZE || *addr>UINT64_MAX-len) return SCE_KERNEL_ERROR_EINVAL;
        if (flags & MAP_NO_OVERWRITE) {
            for (guest_addr p = *addr; p < *addr + len; p += GUEST_PAGE_SIZE) {
                if (gmem_is_mapped(mem, p, 1)) return SCE_KERNEL_ERROR_ENOMEM;
            }
        }
        /* MAP_FIXED replaces existing pages and reserved placeholders
         * (with NO_OVERWRITE, only placeholders are left at this point), in
         * one step: another thread must not take the range in between. */
        gflags = GMAP_FIXED | GMAP_REPLACE;
        pal_mutex_lock(g_lock);
        phys_save(mem, *addr, len);
        if(!flexible)dmaps_forget(*addr,len);
        /* Keep direct translation until the replacement actually succeeds.
         * Lock order is memory bookkeeping -> gmem; mapping-pin holders must
         * finish device work without calling this HLE mapping layer. */
    }
    guest_addr out = 0;
    if (demand) gflags |= GMAP_DEMAND;
    bool replacement_started=false;
    ps5c_err err = flexible
        ?gmem_map_flexible(mem,*addr,len,align?align:GUEST_PAGE_SIZE,to_gprot(prot),gflags,FLEXIBLE_MEMORY_SIZE,name,&out,&replacement_started)
        :gmem_map_aligned(mem,*addr,len,align?align:GUEST_PAGE_SIZE,to_gprot(prot),gflags,name,&out);
    if(flags&MAP_FIXED) {
        /* A quota rejection precedes any unmap and keeps direct metadata.
         * Once replacement starts, retain main's conservative invalidation
         * even if a host unit/backing fails after partial removal. */
        if(flexible && (err==PS5C_OK || replacement_started))dmaps_forget(*addr,len);
        pal_mutex_unlock(g_lock);
    }
    if (err != PS5C_OK) {
        LOG_W(KERNEL, "map '%s' 0x%llx bytes at 0x%llx failed: %s", name, (unsigned long long)len,
              (unsigned long long)*addr, ps5c_err_str(err));
        return SCE_KERNEL_ERROR_ENOMEM;
    }
    *addr = out;
    return 0;
}

/* `*addr` is in/out. */
static int32_t map_direct_at(hle_call *c, guest_addr *addr, uint64_t len, uint32_t prot, uint32_t flags,
                             uint64_t phys, uint64_t align, const char *name) {
    if (phys % GUEST_PAGE_SIZE || !len || len > UINT64_MAX - phys)
        return SCE_KERNEL_ERROR_EINVAL;
    /* Keep the existing gmem page rounding for lengths. Physical allocation
     * boundaries are page aligned, so complete byte coverage also covers the
     * rounded final page; do not introduce a new size-alignment rejection. */
    /* Every physical page must be allocated. A streaming pool may span
     * several adjacent allocation records after pieces are freed/reused.
     * Preserve a uniform memory type and reject gaps before changing VMAs. */
    pal_mutex_lock(g_lock);
    /* It may span several allocations that touch (Unreal's PS5 allocator
     * allocates in chunks and maps runs of neighbours in one BatchMap entry);
     * blk is the first, whose type the mapping takes. g_dmem is sorted. */
    dmem_block *blk = NULL;
    uint64_t covered = phys;
    for (size_t i = 0; i < g_dmem.len; i++) {
        dmem_block *b = &g_dmem.data[i];
        if (b->start + b->size <= covered) continue;
        if (b->start > covered || (blk && b->type != blk->type)) break;
        if (!blk) blk = b;
        covered = PS5C_MIN(phys + len, b->start + b->size);
        if (covered == phys + len) break;
    }
    if (covered != phys + len) blk = NULL;
    /* Views of the physical memory object: anywhere we choose, or fixed in
     * the shared-memory area. Every view of a page shows the same bytes. */
    gmem *mem = c->mem;
    int32_t memory_type = blk ? blk->type : -1;
    if (blk && mem->shm && phys + len <= GUEST_SHM_SIZE && (!(flags & MAP_FIXED) || *addr >= mem->ph_start)) {
        pal_mutex_unlock(g_lock);
        if (len >= (64ull << 20) && getenv("PS5C_LOG_MAPS"))
            LOG_I(KERNEL, "direct pool '%s': type %d prot 0x%x phys 0x%llx size %llu MiB (shared path)",
                  name, memory_type, prot, (unsigned long long)phys, (unsigned long long)(len >> 20));
        uint32_t gflags = GMAP_SHARED_AREA;
        guest_addr requested=*addr;
        if(!requested && !(flags&MAP_FIXED) && (prot&(SCE_PROT_GPU_READ|SCE_PROT_GPU_WRITE)) &&
            ((c->rt && c->rt->opts.direct_memory_window) || getenv("PS5C_DMEM_WINDOW"))) {
            /* PS5 shader address32 window. The title profile reserves the
             * matching placeholder boundary before runtime initialization. */
            uint64_t a=align?align:GUEST_PAGE_SIZE;
            requested=PS5C_ALIGN_UP(PS5C_MAX(0x200000000ull,mem->ph_start),a);
            gmem_vma v;
            while(requested<0x300000000ull && gmem_query_vma(mem,requested,true,&v) && v.start<requested+len)
                requested=PS5C_ALIGN_UP(v.start+v.size,a);
            if(requested>=0x300000000ull || len>0x300000000ull-requested)return SCE_KERNEL_ERROR_ENOMEM;
            gflags=GMAP_FIXED;
        }
        if (flags & MAP_FIXED) {
            if (*addr % GUEST_PAGE_SIZE) return SCE_KERNEL_ERROR_EINVAL;
            if (flags & MAP_NO_OVERWRITE) {
                for (guest_addr p = *addr; p < *addr + len; p += GUEST_PAGE_SIZE) {
                    if (gmem_is_mapped(mem, p, 1)) return SCE_KERNEL_ERROR_ENOMEM;
                }
            }
            gflags = GMAP_FIXED | GMAP_REPLACE;
        }
        if (memory_type == 3 || memory_type == 11) gflags |= GMAP_DEMAND;
        guest_addr out = 0;
        bool shared = false;
        ps5c_err err = gmem_map_direct(mem, requested, len, align ? align : GUEST_PAGE_SIZE, to_gprot(prot), gflags, phys,
                                       name, &out, &shared);
        if (err != PS5C_OK) {
            LOG_W(KERNEL, "map direct 0x%llx+0x%llx at 0x%llx failed: %s", (unsigned long long)phys,
                  (unsigned long long)len, (unsigned long long)*addr, ps5c_err_str(err));
            return SCE_KERNEL_ERROR_ENOMEM;
        }
        *addr = out;
        pal_mutex_lock(g_lock);
        if (shared) stale_clear(mem, out, phys, len, to_gprot(prot));
        dmaps_forget(out, len);
        if (dyn_reserve(&g_dmaps, g_dmaps.len + 1)) g_dmaps.data[g_dmaps.len++] = (dmem_map){phys, len, out};
        /* blk belongs to g_dmem and may have moved or been removed while
         * g_lock was released. Its unused mapping hint must not be written
         * through that stale pointer. Live mappings are tracked in g_dmaps. */
        pal_mutex_unlock(g_lock);
        LOG_D(KERNEL, "mapped direct 0x%llx -> 0x%llx (0x%llx bytes, prot 0x%x) '%s'%s", (unsigned long long)phys,
              (unsigned long long)out, (unsigned long long)len, prot, name, shared ? " (view)" : "");
        return 0;
    }
    bool aliased = false;
    dmem_map other = {0, 0, 0};
    const dmem_map *container = NULL;
    for (size_t i = 0; blk && i < g_dmaps.len; i++) {
        const dmem_map *m = &g_dmaps.data[i];
        if (m->phys < phys + len && phys < m->phys + m->len && !(m->addr == *addr && m->phys == phys)) {
            aliased = true;
            other = *m;
            /* a view of all (or much) of direct memory, of which this is a part */
            if (m->phys <= phys && phys + len <= m->phys + m->len && m->len >= 16 * len && m->len >= (1ull << 30))
                container = m;
        }
    }
    /* A second view at an address of our choosing: the view that already
     * shows this memory, so both see the same bytes (host memory cannot be
     * mapped twice in the guest range). */
    if (aliased && container && !(flags & MAP_FIXED)) {
        guest_addr at = container->addr + (phys - container->phys);
        if (align > GUEST_PAGE_SIZE && at % align)
            LOG_W(KERNEL, "direct memory 0x%llx: shared view at 0x%llx is not aligned to 0x%llx", (unsigned long long)phys,
                  (unsigned long long)at, (unsigned long long)align);
        if (dyn_reserve(&g_aliases, g_aliases.len + 1)) {
            g_aliases.data[g_aliases.len++] = (dmem_alias){at, len};
            pal_mutex_unlock(g_lock);
            LOG_I(KERNEL, "direct memory 0x%llx+0x%llx mapped again: shares the view at 0x%llx", (unsigned long long)phys,
                  (unsigned long long)len, (unsigned long long)at);
            *addr = at;
            return 0;
        }
    }
    bool allocated = blk != NULL;
    pal_mutex_unlock(g_lock);
    if (len >= (64ull << 20) && getenv("PS5C_LOG_MAPS"))
        LOG_I(KERNEL, "direct pool '%s': type %d prot 0x%x phys 0x%llx size %llu MiB (private path)",
              name, memory_type, prot, (unsigned long long)phys, (unsigned long long)(len >> 20));
    if (!allocated) {
        LOG_W(KERNEL, "map of unallocated direct memory 0x%llx+0x%llx", (unsigned long long)phys,
              (unsigned long long)len);
        if (getenv("PS5C_LOG_MAPS")) {
            pal_mutex_lock(g_lock);
            for (size_t i = 0; i < g_dmem.len; i++) {
                const dmem_block *b = &g_dmem.data[i];
                if (b->start <= phys + len && b->start + b->size >= phys)
                    LOG_I(KERNEL, "direct allocation: 0x%llx+0x%llx, type %d",
                          (unsigned long long)b->start, (unsigned long long)b->size, b->type);
            }
            pal_mutex_unlock(g_lock);
        }
        return SCE_KERNEL_ERROR_EACCES;
    }
    if (aliased) {
        LOG_W(KERNEL,
              "direct memory 0x%llx+0x%llx mapped twice (at 0x%llx, already at 0x%llx for 0x%llx+0x%llx): aliasing "
              "not supported, contents will diverge",
              (unsigned long long)phys, (unsigned long long)len, (unsigned long long)*addr, (unsigned long long)other.addr,
              (unsigned long long)other.phys, (unsigned long long)other.len);
    }
    /* Where the address is ours to pick, align it like the memory: later
     * views of parts of this range share it (above) and keep the alignment
     * their callers ask for. */
    if (!(flags & MAP_FIXED) && len >= (1ull << 30)) {
        uint64_t natural = 1ull << 30;
        while (natural > len && natural > GUEST_PAGE_SIZE) natural >>= 1;
        if (natural > align) align = natural;
    }
    /* CPU heaps may contain native fiber stacks. Only the graphics pool types
     * participate: legacy type 3 and Gen5 type 11 (observed for Astro's Garlic
     * and resource pools). Its libc heap is type 12 and must stay eager. */
    bool demand = (memory_type == 3 || memory_type == 11) && !(prot & SCE_PROT_CPU_EXEC);
    int32_t ret = map_memory(c, addr, len, prot, flags, align, name, demand,false);
    if (ret == 0) {
        pal_mutex_lock(g_lock);
        dmaps_forget(*addr, len);
        if (dyn_reserve(&g_dmaps, g_dmaps.len + 1)) g_dmaps.data[g_dmaps.len++] = (dmem_map){phys, len, *addr};
        phys_load(c->mem, *addr, phys, len, to_gprot(prot));
        pal_mutex_unlock(g_lock);
        /* Do not dereference blk after dropping g_lock: g_dmem is mutable. */
        LOG_D(KERNEL, "mapped direct 0x%llx -> 0x%llx (0x%llx bytes, prot 0x%x) '%s'", (unsigned long long)phys,
              (unsigned long long)*addr, (unsigned long long)len, prot, name);
    }
    return ret;
}

static int32_t map_direct(hle_call *c, guest_addr addr_ptr, uint64_t len, uint32_t prot, uint32_t flags,
                          uint64_t phys, uint64_t align, const char *name) {
    uint64_t *inout = (uint64_t *)hle_mem(c, addr_ptr, 8);
    if (!inout) return SCE_KERNEL_ERROR_EINVAL;
    guest_addr addr = *inout;
    int32_t ret = map_direct_at(c, &addr, len, prot, flags, phys, align, name);
    if (ret == 0) *inout = addr;
    return ret;
}

/* int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags,
 *                              off_t phys, size_t align) */
static void map_direct_memory(hle_call *c) {
    hle_ret_s32(c, map_direct(c, hle_arg_ptr(c, 0), hle_arg(c, 1), hle_arg_u32(c, 2), hle_arg_u32(c, 3),
                              hle_arg(c, 4), hle_arg(c, 5), "direct"));
}

/* int sceKernelMapDirectMemory2(void **addr, size_t len, int type, int prot, int flags,
 *                               off_t phys, size_t align): the memory type of the
 * mapping is the allocation's here (Jurassic Park Classic Games Collection) */
static void map_direct_memory2(hle_call *c) {
    hle_ret_s32(c, map_direct(c, hle_arg_ptr(c, 0), hle_arg(c, 1), hle_arg_u32(c, 3), hle_arg_u32(c, 4),
                              hle_arg(c, 5), hle_arg(c, 6), "direct"));
}

/* ... + const char *name (7th argument, on the stack) */
static void map_named_direct_memory(hle_call *c) {
    char name[32] = "direct";
    hle_read_str(c, hle_arg_ptr(c, 6), name, sizeof(name));
    hle_ret_s32(c, map_direct(c, hle_arg_ptr(c, 0), hle_arg(c, 1), hle_arg_u32(c, 2), hle_arg_u32(c, 3),
                              hle_arg(c, 4), hle_arg(c, 5), name));
}

static int32_t map_flexible_at(hle_call *c, guest_addr *addr, uint64_t len, uint32_t prot, uint32_t flags,
                               const char *name) {
    return map_memory(c, addr, len, prot, flags, 0, name, false,true);
}

static int32_t map_flexible(hle_call *c, guest_addr addr_ptr, uint64_t len, uint32_t prot, uint32_t flags,
                            const char *name) {
    uint64_t *inout = (uint64_t *)hle_mem(c, addr_ptr, 8);
    if (!inout) return SCE_KERNEL_ERROR_EINVAL;
    guest_addr addr = *inout;
    int32_t ret = map_flexible_at(c, &addr, len, prot, flags, name);
    if (ret == 0) *inout = addr;
    return ret;
}

/* int sceKernelMapFlexibleMemory(void **addr, size_t len, int prot, int flags) */
static void map_flexible_memory(hle_call *c) {
    hle_ret_s32(c, map_flexible(c, hle_arg_ptr(c, 0), hle_arg(c, 1), hle_arg_u32(c, 2), hle_arg_u32(c, 3),
                                "flexible"));
}

static void map_named_flexible_memory(hle_call *c) {
    char name[32] = "flexible";
    hle_read_str(c, hle_arg_ptr(c, 4), name, sizeof(name));
    hle_ret_s32(c, map_flexible(c, hle_arg_ptr(c, 0), hle_arg(c, 1), hle_arg_u32(c, 2), hle_arg_u32(c, 3), name));
}

/* int sceKernelConfiguredFlexibleMemorySize(size_t *out): total configured
 * budget, independent of how many flexible pages have already been mapped. */
static void configured_flexible_memory_size(hle_call *c) {
    guest_addr out = hle_arg_ptr(c, 0);
    bool ok = out && gmem_write_u64(c->mem, out, FLEXIBLE_MEMORY_SIZE);
    hle_ret_s32(c, ok ? 0 : SCE_KERNEL_ERROR_EINVAL);
}

static void available_flexible_memory_size(hle_call *c) {
    uint64_t *out = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 0), 8);
    if (!out) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    uint64_t used = gmem_flexible_used(c->mem);
    *out = used < FLEXIBLE_MEMORY_SIZE ? FLEXIBLE_MEMORY_SIZE - used : 0;
    hle_ret_s32(c, 0);
}

static int32_t do_munmap(hle_call *c, guest_addr addr, uint64_t len) {
    if (addr % GUEST_PAGE_SIZE || !len) return SCE_KERNEL_ERROR_EINVAL;
    len = PS5C_ALIGN_UP(len, GUEST_PAGE_SIZE);
    pal_mutex_lock(g_lock);
    for (size_t i = 0; i < g_aliases.len; i++) {
        if (g_aliases.data[i].addr == addr && g_aliases.data[i].len == len) {
            g_aliases.data[i] = g_aliases.data[--g_aliases.len]; /* a shared view: the memory stays */
            pal_mutex_unlock(g_lock);
            return 0;
        }
    }
    phys_save(c->mem, addr, len);
    dmaps_forget(addr, len);
    pal_mutex_unlock(g_lock);
    return gmem_unmap(c->mem, addr, len) == PS5C_OK ? 0 : SCE_KERNEL_ERROR_EINVAL;
}

static void kernel_munmap(hle_call *c) { hle_ret_s32(c, do_munmap(c, hle_arg_ptr(c, 0), hle_arg(c, 1))); }
static void posix_munmap(hle_call *c) { hle_ret_s32(c, do_munmap(c, hle_arg_ptr(c, 0), hle_arg(c, 1)) ? -1 : 0); }

static int32_t do_mprotect(hle_call *c, guest_addr addr, uint64_t len, uint32_t prot) {
    guest_addr start = PS5C_ALIGN_DOWN(addr, GUEST_PAGE_SIZE);
    uint64_t size = PS5C_ALIGN_UP(addr + len, GUEST_PAGE_SIZE) - start;
    uint32_t gprot = to_gprot(prot);
    /* The import stubs stay writable and executable: hooking libraries patch
     * them (and the emulator adds stubs at run time for dlsym). */
    guest_addr stubs = c->rt->lk.import_code;
    if (start < stubs + IMPORT_CODE_AREA_SIZE && start + size > stubs) gprot |= GPROT_RW | GPROT_EXEC;
    ps5c_err err = gmem_protect(c->mem, start, size, gprot);
    if (err == PS5C_ERR_OUT_OF_RANGE) {
        /* The console succeeds over holes and changes the mapped parts only (rps5e 6b0d37c). */
        err = PS5C_OK;
        gmem_vma v;
        for (guest_addr cur = start; err == PS5C_OK && cur < start + size && gmem_query_vma(c->mem, cur, true, &v);) {
            if (v.start >= start + size) break;
            guest_addr a = PS5C_MAX(cur, v.start), b = PS5C_MIN(start + size, v.start + v.size);
            err = gmem_protect(c->mem, a, b - a, gprot);
            cur = b;
        }
    }
    return err == PS5C_OK ? 0 : SCE_KERNEL_ERROR_EINVAL;
}

static void kernel_mprotect(hle_call *c) {
    hle_ret_s32(c, do_mprotect(c, hle_arg_ptr(c, 0), hle_arg(c, 1), hle_arg_u32(c, 2)));
}
static void posix_mprotect(hle_call *c) {
    hle_ret_s32(c, do_mprotect(c, hle_arg_ptr(c, 0), hle_arg(c, 1), hle_arg_u32(c, 2)) ? -1 : 0);
}

/* mmap(addr, len, prot, flags, fd, offset): anonymous mappings only for now. */
static int32_t do_mmap(hle_call *c, guest_addr *addr, uint64_t len, uint32_t prot, uint32_t flags, int32_t fd) {
    if (!(flags & MAP_ANON) && fd != -1) {
        LOG_W(KERNEL, "mmap of file descriptor %d not implemented", fd);
        return SCE_KERNEL_ERROR_ENOSYS;
    }
    len = PS5C_ALIGN_UP(len, GUEST_PAGE_SIZE);
    return map_memory(c, addr, len, prot, flags, 0, "mmap", false,false);
}

static void posix_mmap(hle_call *c) {
    guest_addr addr = hle_arg_ptr(c, 0);
    int32_t ret = do_mmap(c, &addr, hle_arg(c, 1), hle_arg_u32(c, 2), hle_arg_u32(c, 3), hle_arg_s32(c, 4));
    hle_ret_u64(c, ret ? MAP_FAILED_ADDR : addr);
}

/* int sceKernelMmap(void *addr, size_t len, int prot, int flags, int fd, off_t off, void **res) */
static void kernel_mmap(hle_call *c) {
    uint64_t *res = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 6), 8);
    guest_addr addr = hle_arg_ptr(c, 0);
    int32_t ret = res ? do_mmap(c, &addr, hle_arg(c, 1), hle_arg_u32(c, 2), hle_arg_u32(c, 3), hle_arg_s32(c, 4))
                      : SCE_KERNEL_ERROR_EINVAL;
    if (ret == 0) *res = addr;
    hle_ret_s32(c, ret);
}

/* Reserves address space only (GMAP_RESERVE placeholders). `*addr` is in/out. */
static int32_t do_reserve(hle_call *c, guest_addr *addr, uint64_t len, uint32_t flags, uint64_t align) {
    if (!len || len % GUEST_PAGE_SIZE || (align && (!is_pow2(align) || align % GUEST_PAGE_SIZE)))
        return SCE_KERNEL_ERROR_EINVAL;
    /* ranges of our choosing go to the shared-memory area, where direct
     * memory mapped into them can alias */
    uint32_t gflags = GMAP_RESERVE | GMAP_SHARED_AREA;
    if (flags & MAP_FIXED) {
        if (*addr % GUEST_PAGE_SIZE) return SCE_KERNEL_ERROR_EINVAL;
        gflags |= GMAP_FIXED | (flags & MAP_NO_OVERWRITE ? 0u : (uint32_t)GMAP_REPLACE);
        pal_mutex_lock(g_lock);
        if (!(flags & MAP_NO_OVERWRITE)) {
            phys_save(c->mem, *addr, len);
            dmaps_forget(*addr, len);
        }
        pal_mutex_unlock(g_lock);
    }
    guest_addr out = 0;
    if (gmem_map_aligned(c->mem, *addr, len, align ? align : GUEST_PAGE_SIZE, GPROT_NONE, gflags, "reserved", &out) !=
        PS5C_OK) {
        LOG_W(KERNEL, "sceKernelReserveVirtualRange(0x%llx, 0x%llx, flags 0x%x, align 0x%llx): no room",
              (unsigned long long)*addr, (unsigned long long)len, flags, (unsigned long long)align);
        return SCE_KERNEL_ERROR_ENOMEM;
    }
    *addr = out;
    return 0;
}

/* int sceKernelReserveVirtualRange(void **addr, size_t len, int flags, size_t align) */
static void reserve_virtual_range(hle_call *c) {
    uint64_t *inout = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 0), 8);
    if (!inout) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    guest_addr addr = *inout;
    int32_t ret = do_reserve(c, &addr, hle_arg(c, 1), hle_arg_u32(c, 2), hle_arg(c, 3));
    if (ret == 0) *inout = addr;
    hle_ret_s32(c, ret);
}

/* ---- Memory pools (reference: KytyPS5 kernel/memory.cpp) ----
 * Reserve takes 2 MiB-granular address space; Commit backs 64 KiB-granular
 * parts of it with memory, Decommit turns them back into reservation. There
 * is no separate physical pool here: committed pages are plain anonymous
 * memory, counted against POOL_BUDGET only for GetBlockStats. */
#define POOL_RESERVE_ALIGN 0x200000ull
#define POOL_BLOCK 0x10000ull
#define POOL_BUDGET (2ull << 30)
#define SCE_KERNEL_ERROR_EBUSY ((int32_t)0x80020010)
static uint64_t g_pool_committed;

/* int sceKernelMemoryPoolReserve(void *addr_in, size_t len, size_t align, int flags, void **addr_out) */
static void memory_pool_reserve(hle_call *c) {
    uint64_t len = hle_arg(c, 1), align = hle_arg(c, 2);
    uint32_t flags = hle_arg_u32(c, 3);
    uint64_t *out = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 4), 8);
    if (!out || !len || len % POOL_RESERVE_ALIGN || (align && (!is_pow2(align) || align % POOL_RESERVE_ALIGN))) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    guest_addr addr = hle_arg_ptr(c, 0);
    int32_t ret = do_reserve(c, &addr, len, flags, align ? align : POOL_RESERVE_ALIGN);
    if (ret == 0) *out = addr;
    LOG_D(KERNEL, "sceKernelMemoryPoolReserve(0x%llx, 0x%llx, align 0x%llx, flags 0x%x) = 0x%x -> 0x%llx",
          (unsigned long long)hle_arg_ptr(c, 0), (unsigned long long)len, (unsigned long long)align, flags,
          (unsigned)ret, (unsigned long long)addr);
    hle_ret_s32(c, ret);
}

/* int sceKernelMemoryPoolCommit(void *addr, size_t len, int type, int prot, int flags) */
static void memory_pool_commit(hle_call *c) {
    guest_addr addr = hle_arg_ptr(c, 0);
    uint64_t len = hle_arg(c, 1);
    uint32_t prot = hle_arg_u32(c, 3);
    if (!addr || !len || len % POOL_BLOCK || addr % POOL_BLOCK || addr > UINT64_MAX - len || (prot & 0x04)) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    /* already committed pages: the console reports EBUSY */
    for (guest_addr p = addr; p < addr + len; p += GUEST_PAGE_SIZE) {
        if (gmem_is_mapped(c->mem, p, 1)) {
            hle_ret_s32(c, SCE_KERNEL_ERROR_EBUSY);
            return;
        }
    }
    guest_addr at = addr;
    int32_t ret = map_memory(c, &at, len, prot, MAP_FIXED, 0, "pool", false, false);
    if (ret == 0) __atomic_fetch_add(&g_pool_committed, len, __ATOMIC_RELAXED);
    LOG_D(KERNEL, "sceKernelMemoryPoolCommit(0x%llx, 0x%llx, type %d, prot 0x%x) = 0x%x", (unsigned long long)addr,
          (unsigned long long)len, hle_arg_s32(c, 2), prot, (unsigned)ret);
    hle_ret_s32(c, ret);
}

/* int sceKernelMemoryPoolDecommit(void *addr, size_t len, int flags) */
static void memory_pool_decommit(hle_call *c) {
    guest_addr addr = hle_arg_ptr(c, 0);
    uint64_t len = hle_arg(c, 1);
    if (!addr || !len || len % POOL_BLOCK || addr % POOL_BLOCK || addr > UINT64_MAX - len) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    uint64_t was = 0;
    for (guest_addr p = addr; p < addr + len; p += GUEST_PAGE_SIZE)
        if (gmem_is_mapped(c->mem, p, 1)) was += GUEST_PAGE_SIZE;
    /* back to a reservation in one step, so nothing else takes the range */
    guest_addr at = addr;
    int32_t ret = do_reserve(c, &at, len, MAP_FIXED, 0);
    if (ret == 0) {
        uint64_t cur = __atomic_load_n(&g_pool_committed, __ATOMIC_RELAXED), next;
        do next = cur > was ? cur - was : 0;
        while (!__atomic_compare_exchange_n(&g_pool_committed, &cur, next, false, __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED));
    } else {
        ret = SCE_KERNEL_ERROR_EACCES;
    }
    hle_ret_s32(c, ret);
}

/* int sceKernelMemoryPoolGetBlockStats(SceKernelMemoryPoolBlockStats *out, size_t size):
 * {available flushed, available cached, allocated flushed, allocated cached} in 64 KiB blocks */
static void memory_pool_get_block_stats(hle_call *c) {
    guest_addr out = hle_arg_ptr(c, 0);
    uint64_t size = hle_arg(c, 1);
    if (!out && size) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EFAULT);
        return;
    }
    uint64_t used = __atomic_load_n(&g_pool_committed, __ATOMIC_RELAXED);
    int32_t stats[4] = {(int32_t)((used < POOL_BUDGET ? POOL_BUDGET - used : 0) / POOL_BLOCK), 0,
                        (int32_t)(used / POOL_BLOCK), 0};
    if (size > sizeof(stats)) size = sizeof(stats);
    if (size && !gmem_write(c->mem, out, stats, size)) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EFAULT);
        return;
    }
    hle_ret_s32(c, 0);
}

/* ---- Batch map ---- */

/* SceKernelBatchMapEntry */
typedef struct batch_entry {
    uint64_t start;  /* guest address */
    uint64_t offset; /* physical offset (direct memory) */
    uint64_t length;
    uint8_t protection;
    uint8_t type;    /* memory type for MAP_TYPE_PROTECT */
    int16_t reserved;
    int32_t operation;
} batch_entry;

enum { BATCH_MAP_DIRECT, BATCH_UNMAP, BATCH_PROTECT, BATCH_MAP_FLEXIBLE, BATCH_TYPE_PROTECT };

/* int sceKernelBatchMap2(entries, int count, int *done, int flags): the
 * entries are applied in order; `done` gets how many succeeded. */
static int32_t batch_map(hle_call *c, guest_addr entries, int32_t count, guest_addr done_ptr, uint32_t flags) {
    int32_t done = 0, ret = 0;
    if (count < 0) return SCE_KERNEL_ERROR_EINVAL;
    for (; done < count; done++) {
        const batch_entry *e = (const batch_entry *)hle_mem(c, entries + (uint64_t)done * sizeof(batch_entry),
                                                            sizeof(batch_entry));
        if (!e) {
            ret = SCE_KERNEL_ERROR_EFAULT;
            break;
        }
        guest_addr addr = e->start;
        switch (e->operation) {
        case BATCH_MAP_DIRECT:
            ret = map_direct_at(c, &addr, e->length, e->protection, flags | MAP_FIXED, e->offset, 0, "direct");
            break;
        case BATCH_UNMAP: ret = do_munmap(c, addr, e->length); break;
        case BATCH_PROTECT:
        case BATCH_TYPE_PROTECT: ret = do_mprotect(c, addr, e->length, e->protection); break;
        case BATCH_MAP_FLEXIBLE:
            ret = map_flexible_at(c, &addr, e->length, e->protection, flags | MAP_FIXED, "flexible");
            break;
        default: ret = SCE_KERNEL_ERROR_EINVAL; break;
        }
        if (ret) {
            LOG_W(KERNEL, "BatchMap entry %d (op %d, 0x%llx+0x%llx) failed: 0x%x", done, e->operation,
                  (unsigned long long)e->start, (unsigned long long)e->length, (unsigned)ret);
            break;
        }
    }
    uint32_t *out = (uint32_t *)hle_mem(c, done_ptr, 4);
    if (out) *out = (uint32_t)done;
    return ret;
}

static void kernel_batch_map(hle_call *c) {
    hle_ret_s32(c, batch_map(c, hle_arg_ptr(c, 0), hle_arg_s32(c, 1), hle_arg_ptr(c, 2), MAP_FIXED));
}

static void kernel_batch_map2(hle_call *c) {
    hle_ret_s32(c, batch_map(c, hle_arg_ptr(c, 0), hle_arg_s32(c, 1), hle_arg_ptr(c, 2), hle_arg_u32(c, 3)));
}

/* ---- Queries ---- */

/* int sceKernelQueryMemoryProtection(void *addr, void **start, void **end, int *prot) */
static void query_memory_protection(hle_call *c) {
    gmem_vma snapshot;
    if (!gmem_query_vma(c->mem, hle_arg_ptr(c, 0), false, &snapshot)) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EACCES);
        return;
    }
    const gmem_vma *v = &snapshot;
    uint64_t *start = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 1), 8);
    uint64_t *end = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 2), 8);
    uint32_t *prot = (uint32_t *)hle_mem(c, hle_arg_ptr(c, 3), 4);
    if (start) *start = v->start;
    if (end) *end = v->start + v->size - 1;
    if (prot) *prot = to_sce_prot(v->prot);
    hle_ret_s32(c, 0);
}

/* SceKernelVirtualQueryInfo */
typedef struct vquery_info {
    uint64_t start, end, offset;
    int32_t protection, memory_type;
    uint8_t flags; /* isFlexible:1 isDirect:1 isStack:1 isPooled:1 isCommitted:1 */
    char name[32];
} vquery_info;

/* int sceKernelVirtualQuery(const void *addr, int flags, info *out, size_t size) */
static void virtual_query(hle_call *c) {
    guest_addr addr = hle_arg_ptr(c, 0);
    bool find_next = hle_arg_s32(c, 1) & 1;
    uint64_t size = hle_arg(c, 3);
    vquery_info *out = (vquery_info *)hle_mem(c, hle_arg_ptr(c, 2), size);
    if (!out || size < sizeof(vquery_info)) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    gmem_vma snapshot;
    if (!gmem_query_vma(c->mem, addr, find_next, &snapshot)) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EACCES);
        return;
    }
    const gmem_vma *v = &snapshot;
    memset(out, 0, sizeof(*out));
    out->start = v->start;
    out->end = v->start + v->size;
    out->protection = (int32_t)to_sce_prot(v->prot);
    bool direct = v->kind == GVMA_VIEW || strncmp(v->name, "direct", 6) == 0;
    out->flags = v->reserved ? 0 : (uint8_t)((direct ? 0x02 : 0x01) | 0x10);
    if (strncmp(v->name, "stack", 5) == 0) out->flags |= 0x04;
    if (direct && !v->reserved) {
        /* the physical offset of the region's first page (games release it by this) */
        if (v->kind == GVMA_VIEW) {
            out->offset = v->phys + (v->start - v->os_base);
        } else {
            pal_mutex_lock(g_lock);
            for (size_t i = 0; i < g_dmaps.len; i++) {
                const dmem_map *m = &g_dmaps.data[i];
                if (v->start >= m->addr && v->start < m->addr + m->len) out->offset = m->phys + (v->start - m->addr);
            }
            pal_mutex_unlock(g_lock);
        }
        pal_mutex_lock(g_lock);
        for (size_t i = 0; i < g_dmem.len; i++) {
            const dmem_block *b = &g_dmem.data[i];
            if (out->offset >= b->start && out->offset < b->start + b->size) out->memory_type = b->type;
        }
        pal_mutex_unlock(g_lock);
    }
    snprintf(out->name, sizeof(out->name), "%.31s", v->name);
    hle_ret_s32(c, 0);
}

static void set_virtual_range_name(hle_call *c) { hle_ret_s32(c, 0); }

/* ---- libc heap hooks ---- */

/* libc hands the kernel its heap API table (used by the dynamic linker). */
static void rtld_set_application_heap_api(hle_call *c) {
    c->rt->app_heap_api = hle_arg_ptr(c, 0);
    hle_ret_s32(c, 0);
}

/* No sanitizer runtime: NULL tells libc to use its own operator new (and malloc). */
static void get_sanitizer_new_replace_external(hle_call *c) { hle_ret_u64(c, 0); }

/* int sceKernelMtypeprotect(void *addr, size_t len, int mtype, int prot): the
 * memory type only matters to the GPU caches, the protection is applied */
static void kernel_mtypeprotect(hle_call *c) {
    /* flexible memory has no memory type to change: EOPNOTSUPP (rps5e 6b0d37c) */
    guest_addr start = PS5C_ALIGN_DOWN(hle_arg_ptr(c, 0), GUEST_PAGE_SIZE);
    guest_addr end = PS5C_ALIGN_UP(hle_arg_ptr(c, 0) + hle_arg(c, 1), GUEST_PAGE_SIZE);
    gmem_vma v;
    for (guest_addr cur = start; cur < end && gmem_query_vma(c->mem, cur, true, &v) && v.start < end;
         cur = v.start + v.size) {
        if (v.flexible && !v.reserved) {
            static int logged;
            if (logged++ < 4) LOG_I(KERNEL, "sceKernelMtypeprotect over flexible memory at 0x%llx: EOPNOTSUPP",
                                    (unsigned long long)v.start);
            hle_ret_s32(c, (int32_t)0x8002002D);
            return;
        }
    }
    hle_ret_s32(c, do_mprotect(c, hle_arg_ptr(c, 0), hle_arg(c, 1), hle_arg_u32(c, 3)));
}

/* Partially resident texture apertures: int sceKernelSetPrtAperture(int index,
 * void *addr, size_t len) and sceKernelGetPrtAperture(int index, void **addr,
 * size_t *len). Recorded only: guest memory is always resident. */
#define PRT_APERTURES 3
static uint64_t g_prt[PRT_APERTURES][2];
static void set_prt_aperture(hle_call *c) {
    int32_t i = hle_arg_s32(c, 0);
    if (i < 0 || i >= PRT_APERTURES) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    pal_mutex_lock(g_lock);
    g_prt[i][0] = hle_arg_ptr(c, 1), g_prt[i][1] = hle_arg(c, 2);
    pal_mutex_unlock(g_lock);
    hle_ret_s32(c, 0);
}
static void get_prt_aperture(hle_call *c) {
    int32_t i = hle_arg_s32(c, 0);
    guest_addr addr = hle_arg_ptr(c, 1), len = hle_arg_ptr(c, 2);
    if (i < 0 || i >= PRT_APERTURES || !addr || !len) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    pal_mutex_lock(g_lock);
    uint64_t v[2] = {g_prt[i][0], g_prt[i][1]};
    pal_mutex_unlock(g_lock);
    hle_ret_s32(c, gmem_write(c->mem, addr, &v[0], 8) && gmem_write(c->mem, len, &v[1], 8) ? 0 : SCE_KERNEL_ERROR_EFAULT);
}

/* int sceLibcHeapGetTraceInfo(info *out): heap tracing state shared with the
 * kernel. We hand out zeroed tables so tracing stays off. */
static void libc_heap_get_trace_info(hle_call *c) {
    /* { u64 size; u32 flag; u32 get_segment_info; u64 *mspace_atomic_id_mask; u64 *mstate_table; } */
    uint8_t *info = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 32);
    if (!info) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    if (!c->rt->heap_trace_area) {
        gmem_map(c->mem, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "libc heap trace", &c->rt->heap_trace_area);
    }
    uint32_t zero = 0;
    uint64_t mask = c->rt->heap_trace_area, table = c->rt->heap_trace_area + 0x100;
    memcpy(info + 0x0C, &zero, 4);
    memcpy(info + 0x10, &mask, 8);
    memcpy(info + 0x18, &table, 8);
    hle_ret_s32(c, 0);
}

#define K "libkernel"
#define P "libScePosix"
#define POSIX_ENTRY(name, fn) HLE_FUNC_ENTRY(K, name, fn), HLE_FUNC_ENTRY(P, name, fn)

/* ---- AMM (libSceAmpr's address memory manager, lib_ampr.c / lib_kernel_apr.c) ---- */

/* sceAmprAmmGiveDirectMemory: direct memory the AMM may map (2 MiB blocks) */
int32_t kernel_amm_give_direct(uint64_t start, uint64_t end, uint64_t size, uint64_t align, uint64_t *out) {
    return dmem_allocate(start, end, size, align ? align : 0x200000, 0, out);
}

/* An AMM map command: memory of its own (automatic) or the direct memory at phys, at a fixed address */
int32_t kernel_amm_map(hle_call *c, guest_addr va, uint64_t size, uint32_t prot, bool direct, uint64_t phys) {
    guest_addr a = va;
    return direct ? map_direct_at(c, &a, size, prot, MAP_FIXED, phys, 0, "amm direct")
                  : map_memory(c, &a, size, prot, MAP_FIXED, 0, "amm", false,false);
}

int32_t kernel_amm_unmap(hle_call *c, guest_addr va, uint64_t size) {
    return do_munmap(c, va, size);
}

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(K, "sceKernelGetDirectMemorySize", get_direct_memory_size),
    HLE_FUNC_ENTRY(K, "sceKernelDirectMemoryQuery", direct_memory_query),
    HLE_FUNC_ENTRY(K, "sceKernelAllocateDirectMemory", allocate_direct_memory),
    HLE_FUNC_ENTRY(K, "sceKernelAllocateMainDirectMemory", allocate_main_direct_memory),
    HLE_FUNC_ENTRY(K, "sceKernelReleaseDirectMemory", release_direct_memory),
    HLE_FUNC_ENTRY(K, "sceKernelCheckedReleaseDirectMemory", checked_release_direct_memory),
    HLE_FUNC_ENTRY(K, "sceKernelAvailableDirectMemorySize", available_direct_memory_size),
    HLE_FUNC_ENTRY(K, "sceKernelMapDirectMemory", map_direct_memory),
    HLE_FUNC_ENTRY(K, "sceKernelMapDirectMemory2", map_direct_memory2),
    HLE_FUNC_ENTRY(K, "sceKernelMapNamedDirectMemory", map_named_direct_memory),
    HLE_FUNC_ENTRY(K, "sceKernelMapFlexibleMemory", map_flexible_memory),
    HLE_FUNC_ENTRY(K, "sceKernelMapNamedFlexibleMemory", map_named_flexible_memory),
    /* the libc of GTA V maps its heap with this one (same arguments) */
    HLE_FUNC_ENTRY(K, "sceKernelMapNamedFlexibleMemoryInternal", map_named_flexible_memory),
    HLE_FUNC_ENTRY(K, "sceKernelAvailableFlexibleMemorySize", available_flexible_memory_size),
    HLE_FUNC_ENTRY(K, "sceKernelConfiguredFlexibleMemorySize", configured_flexible_memory_size),
    HLE_FUNC_ENTRY(K, "sceKernelMunmap", kernel_munmap),
    HLE_FUNC_ENTRY(K, "sceKernelMprotect", kernel_mprotect),
    HLE_FUNC_ENTRY(K, "sceKernelMmap", kernel_mmap),
    HLE_FUNC_ENTRY(K, "sceKernelQueryMemoryProtection", query_memory_protection),
    HLE_FUNC_ENTRY(K, "sceKernelVirtualQuery", virtual_query),
    HLE_FUNC_ENTRY(K, "sceKernelSetVirtualRangeName", set_virtual_range_name),
    HLE_FUNC_ENTRY(K, "sceKernelReserveVirtualRange", reserve_virtual_range),
    HLE_FUNC_ENTRY(K, "sceKernelMemoryPoolReserve", memory_pool_reserve),
    HLE_FUNC_ENTRY(K, "sceKernelMemoryPoolCommit", memory_pool_commit),
    HLE_FUNC_ENTRY(K, "sceKernelMemoryPoolDecommit", memory_pool_decommit),
    HLE_FUNC_ENTRY(K, "sceKernelMemoryPoolGetBlockStats", memory_pool_get_block_stats),
    HLE_FUNC_ENTRY(K, "sceKernelBatchMap", kernel_batch_map),
    HLE_FUNC_ENTRY(K, "sceKernelBatchMap2", kernel_batch_map2),
    POSIX_ENTRY("mmap", posix_mmap),
    POSIX_ENTRY("munmap", posix_munmap),
    POSIX_ENTRY("mprotect", posix_mprotect),
    HLE_FUNC_ENTRY(K, "_sceKernelRtldSetApplicationHeapAPI", rtld_set_application_heap_api),
    HLE_FUNC_ENTRY(K, "sceKernelGetSanitizerNewReplaceExternal", get_sanitizer_new_replace_external),
    HLE_FUNC_ENTRY(K, "sceKernelGetSanitizerMallocReplaceExternal", get_sanitizer_new_replace_external),
    HLE_FUNC_ENTRY(K, "sceKernelMtypeprotect", kernel_mtypeprotect),
    HLE_FUNC_ENTRY(K, "sceKernelSetPrtAperture", set_prt_aperture),
    HLE_FUNC_ENTRY(K, "sceKernelGetPrtAperture", get_prt_aperture),
    HLE_FUNC_ENTRY("libSceLibcInternalExt", "sceLibcHeapGetTraceInfo", libc_heap_get_trace_info),
};

void hle_register_libkernel_memory(void) {
    if (!g_lock && pal_mutex_create(&g_lock) != PS5C_OK) {
        LOG_E(KERNEL, "cannot create the memory lock");
        return;
    }
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}
