/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Write tracking of guest pages for the GPU caches (gpu_watch.h). */
#include "gpu/gpu_watch.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "pal/pal.h"

#define PAGE_BITS 12u
#define REGION_BITS 26u /* 64 MiB of guest memory per region */
#define REGION_PAGES (1u << (REGION_BITS - PAGE_BITS))
#define SPACE_BITS 40u /* GUEST_SPACE_END */
#define REGION_COUNT (1u << (SPACE_BITS - REGION_BITS))

/* Page state: a written bit per client, OURS (armed since the last mapping
 * change: a write fault on it is ours even when another thread has already
 * made it writable) and PROT (read-only because of this module). */
#define ST_WRITTEN 0x1Fu
#define ST_PRIVATE 0x20u /* copy-on-write mode: a private copy, which cannot be armed again */
#define ST_OURS 0x40u
#define ST_PROT 0x80u
#define FAULT_SPAN 16u /* pages a write fault releases at most */

typedef struct region {
    atomic_flag lock;
    _Atomic uint8_t page[REGION_PAGES];
} region;

static _Atomic(region *) g_regions[REGION_COUNT];
static _Atomic uint64_t g_faults, g_protected;
static gmem *_Atomic g_mem;
static void (*_Atomic g_saver[GPU_WATCH_CLIENTS])(guest_addr page);
static _Atomic uint64_t g_generation; /* gpu_watch_generation */
static _Atomic uint64_t g_map_epoch; /* mapping changes seen: invalidates the eligibility cache */

/* Tracking by copy-on-write pages (PS5C_GPU_WATCH=cow), with no exception on the writing thread:
 * arming makes the pages of a view PAGE_WRITECOPY-like (pal_vm_cow_arm); the first write turns a
 * page into a private read-write copy inside the system, and the tracker learns of it by asking
 * (pal_vm_cow_clean) when a client checks its range. Differences from the fault mode:
 *   - a written page cannot be armed again: its ranges read as written from then on;
 *   - only views of physical memory that no other address shows can be armed, and the memory
 *     manager maps a view again when a second one is about to appear (cow_aliased);
 *   - the private copies are written back into the physical memory object before their view
 *     goes (cow_release), so that a later mapping of that memory finds them;
 *   - no savers (a page changes without notice), no fault hook, no prepare_write.
 * ST_OURS here means: armed at some point since its view was mapped (it may be private). */
static int g_cow_mode = -1;
static bool cow(void) { return g_cow_mode > 0; }
static atomic_flag g_cow_gate = ATOMIC_FLAG_INIT; /* arming against cow_release / cow_aliased */
static void gate_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_cow_gate, memory_order_acquire)) {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#endif
    }
}
static void gate_unlock(void) { atomic_flag_clear_explicit(&g_cow_gate, memory_order_release); }
static _Atomic uint64_t g_cow_flushed;
static atomic_bool g_sweeper; /* the sweeper thread runs (gpu_watch_sweep) */

bool gpu_watch_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("PS5C_GPU_WATCH");
        /* Opt-in until COW flushes can exclude concurrent native writers: a
         * partial unmap snapshots neighboring private pages, losing writes
         * between the copy and the whole-view unmap. PS5C_GPU_WATCH=cow retains
         * that experiment. Making the pages read-only is not a safe fallback:
         * Windows' exception frame overwrites the guest's System V red zone
         * (KAGE's FMOD thread died reading [rsp-0x10]). The fault experiment is
         * still PS5C_GPU_WATCH=1 or =fault; 0/unset uses content validation. */
        bool fault = e && (!strcmp(e, "fault") || !strcmp(e, "1"));
        bool want_cow = e && !strcmp(e, "cow");
        g_cow_mode = want_cow && pal_vm_cow_supported();
        on = fault || g_cow_mode;
        if (on) LOG_I(GPU, "page write tracking for GPU caches: on (%s)", g_cow_mode ? "copy-on-write pages, experimental" : "write faults, experimental");
    }
    return on != 0;
}

static void cow_release(guest_addr addr, uint64_t size, uint8_t *phys_copy);
static bool cow_aliased(guest_addr addr, uint64_t size);
static int32_t sweeper_main(void *arg);

void gpu_watch_start(gmem *m) {
    if (cow()) {
        if (!gmem_phys_window(m)) {
            LOG_W(GPU, "page write tracking: no host view of physical memory, tracking stays off");
            return; /* g_mem stays NULL: nothing is eligible */
        }
        gmem_set_cow_hooks(cow_release, cow_aliased);
        const char *e = getenv("PS5C_GPU_WATCH_SWEEP");
        pal_thread *t = NULL;
        if (!(e && !strcmp(e, "0")) && !atomic_load(&g_sweeper) &&
            pal_thread_create(&t, sweeper_main, NULL, "gpu watch sweeper", 0) == PS5C_OK)
            atomic_store(&g_sweeper, true);
        LOG_I(GPU, "page write tracking: %s", atomic_load(&g_sweeper) ? "a sweeper thread keeps the page states current" : "each check asks the system");
    }
    atomic_store(&g_mem, m);
}

uint64_t gpu_watch_generation(void) { return atomic_load_explicit(&g_generation, memory_order_acquire); }

void gpu_watch_set_saver(uint32_t client, void (*fn)(guest_addr page)) {
    if (client < GPU_WATCH_CLIENTS) atomic_store(&g_saver[client], fn);
}

/* A protected page is about to become writable: the savers of the clients it is clean for. */
static void save_page(guest_addr page_index, uint8_t st) {
    for (uint32_t c = 0; c < GPU_WATCH_CLIENTS; c++) {
        if (st & (1u << c)) continue;
        void (*fn)(guest_addr) = atomic_load_explicit(&g_saver[c], memory_order_acquire);
        if (fn) fn(page_index << PAGE_BITS);
    }
}

/* Another view of physical memory [phys, phys + size) besides the VMA at `self`. */
static bool view_aliased(gmem *m, guest_addr self, uint64_t phys, uint64_t size) {
    gmem_vma v;
    for (guest_addr at = 0; gmem_query_vma(m, at, true, &v); at = v.start + v.size) {
        if (v.kind == GVMA_VIEW && v.start != self && !v.reserved) {
            uint64_t vp = v.phys + (v.start - v.os_base);
            if (vp < phys + size && phys < vp + v.size) return true;
        }
        if (v.start + v.size <= at) break;
    }
    return false;
}

/* ---- Views of the same physical memory ----
 * A game may map direct memory at two addresses (Minecraft's vertex pools:
 * the CPU fills one view, the GPU reads the other). A write through one view
 * changes what the other shows, so the views are protected together and a
 * write fault on one marks them all.
 *
 * The table lists the views whose physical ranges overlap another view's,
 * as of g_alias_epoch. It is rebuilt on the arming side only (never from a
 * fault): a fault uses the table that was current when the pages were armed. */
typedef struct alias_view {
    guest_addr start;
    uint64_t size, phys;
    bool plain;    /* no view of its group is executable: the group can be watched */
    bool writable; /* a view the guest cannot write through needs no protection */
} alias_view;
#define ALIAS_MAX 16384
#define ALIAS_TWINS 8
static alias_view g_alias[ALIAS_MAX];
static uint32_t g_alias_count;
static bool g_alias_overflow; /* more aliased views than the table holds: none of them is watched */
static uint64_t g_alias_epoch = ~0ull;
static atomic_flag g_alias_lock = ATOMIC_FLAG_INIT;

static void alias_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_alias_lock, memory_order_acquire)) {}
}
static void alias_unlock(void) { atomic_flag_clear_explicit(&g_alias_lock, memory_order_release); }

static void range_written(guest_addr start, uint64_t len, bool writable);

static int alias_by_phys(const void *a, const void *b) {
    const alias_view *x = (const alias_view *)a, *y = (const alias_view *)b;
    return x->phys < y->phys ? -1 : x->phys > y->phys;
}

/* Brings the table up to the current mapping epoch. */
static void alias_refresh(gmem *m) {
    /* Mappings change all the time (every first touch of a demand page counts),
     * and walking the map costs: at most one rebuild every 100 ms. A view that
     * became a twin in between is caught at the next rebuild, which marks the
     * views new to the table written, so whoever armed one of them alone copies
     * again and arms the pair. */
    static _Atomic uint64_t last_build;
    uint64_t epoch = atomic_load(&g_map_epoch), now = pal_time_ns();
    alias_lock();
    bool current = g_alias_epoch == epoch;
    alias_unlock();
    if (current || (atomic_load(&last_build) && now - atomic_load(&last_build) < 100000000ull)) return;
    atomic_store(&last_build, now);
    uint32_t cap = 1024, n = 0;
    alias_view *all = (alias_view *)malloc(cap * sizeof(alias_view));
    bool overflow = !all;
    gmem_vma v;
    for (guest_addr at = 0; all && gmem_query_vma(m, at, true, &v); at = v.start + v.size) {
        if (v.kind == GVMA_VIEW && !v.reserved) {
            if (n == cap) {
                alias_view *more = (alias_view *)realloc(all, 2 * cap * sizeof(alias_view));
                if (!more) { overflow = true; break; }
                all = more;
                cap *= 2;
            }
            all[n++] = (alias_view){v.start, v.size, v.phys + (v.start - v.os_base), !(v.prot & GPROT_EXEC),
                                    (v.prot & GPROT_WRITE) != 0};
        }
        if (v.start + v.size <= at) break;
    }
    static alias_view found[ALIAS_MAX]; /* only this function, under no lock: one arming thread at a time fills it */
    static atomic_flag building = ATOMIC_FLAG_INIT;
    while (atomic_flag_test_and_set_explicit(&building, memory_order_acquire)) {}
    uint32_t count = 0;
    if (all && !overflow) {
        qsort(all, n, sizeof(alias_view), alias_by_phys);
        /* groups of views whose physical ranges chain together */
        for (uint32_t i = 0; i < n;) {
            uint64_t end = all[i].phys + all[i].size;
            uint32_t j = i + 1;
            bool plain = all[i].plain;
            while (j < n && all[j].phys < end) {
                if (all[j].phys + all[j].size > end) end = all[j].phys + all[j].size;
                plain &= all[j].plain;
                j++;
            }
            if (j - i > 1) {
                for (uint32_t k = i; k < j; k++) {
                    if (count == ALIAS_MAX) { overflow = true; break; }
                    found[count] = all[k];
                    found[count++].plain = plain;
                }
            }
            i = j;
        }
    }
    free(all);
    {
        static uint32_t logged;
        uint32_t not_plain = 0;
        for (uint32_t k = 0; k < count; k++) not_plain += !found[k].plain;
        if (logged < 12 && (count || overflow)) {
            logged++;
            LOG_I(GPU, "page watch: %u views, %u share physical memory (%u in groups that cannot be watched)%s", n, count,
                  not_plain, overflow ? ", too many: none watched" : "");
        }
    }
    /* views new to the table (both lists are in physical order) */
    static alias_view fresh[ALIAS_MAX];
    uint32_t fresh_count = 0;
    alias_lock();
    for (uint32_t k = 0, o = 0; !overflow && k < count; k++) {
        while (o < g_alias_count && g_alias[o].phys < found[k].phys) o++;
        bool had = false;
        for (uint32_t q = o; q < g_alias_count && g_alias[q].phys == found[k].phys; q++)
            had |= g_alias[q].start == found[k].start && g_alias[q].size == found[k].size;
        if (!had) fresh[fresh_count++] = found[k];
    }
    memcpy(g_alias, found, count * sizeof(alias_view));
    g_alias_count = overflow ? 0 : count;
    g_alias_overflow = overflow;
    g_alias_epoch = epoch;
    alias_unlock();
    for (uint32_t k = 0; k < fresh_count; k++) range_written(fresh[k].start, fresh[k].size, fresh[k].writable);
    atomic_flag_clear_explicit(&building, memory_order_release);
}

/* The other views of the physical memory behind [addr, addr + size): up to
 * ALIAS_TWINS ranges, page aligned. Returns how many. */
static uint32_t alias_twins(guest_addr addr, uint64_t size, guest_addr *start, uint64_t *len, bool *writable) {
    uint32_t n = 0;
    guest_addr lo = addr & ~(GPU_WATCH_PAGE - 1), hi = (addr + size + GPU_WATCH_PAGE - 1) & ~(GPU_WATCH_PAGE - 1);
    alias_lock();
    for (uint32_t i = 0; i < g_alias_count && n < ALIAS_TWINS; i++) {
        const alias_view *e = &g_alias[i];
        if (e->start >= hi || lo >= e->start + e->size) continue;
        guest_addr a = lo > e->start ? lo : e->start, b = hi < e->start + e->size ? hi : e->start + e->size;
        uint64_t p = e->phys + (a - e->start), pe = p + (b - a);
        for (uint32_t j = 0; j < g_alias_count && n < ALIAS_TWINS; j++) {
            const alias_view *f = &g_alias[j];
            if (j == i || f->phys >= pe || p >= f->phys + f->size) continue;
            uint64_t q = p > f->phys ? p : f->phys, qe = pe < f->phys + f->size ? pe : f->phys + f->size;
            start[n] = f->start + (q - f->phys);
            writable[n] = f->writable;
            len[n++] = qe - q;
        }
    }
    alias_unlock();
    return n;
}

/* 1: the VMA is a view with twins that can all be watched; 0: no twins; -1: twins that cannot. */
static int alias_state(gmem *m, const gmem_vma *v) {
    alias_refresh(m);
    int state = 0;
    alias_lock();
    if (g_alias_overflow) state = -2;
    for (uint32_t i = 0; state >= 0 && i < g_alias_count; i++) {
        const alias_view *e = &g_alias[i];
        if (e->start < v->start + v->size && v->start < e->start + e->size) state = e->plain ? 1 : -1;
    }
    alias_unlock();
    if (state == -2) state = view_aliased(m, v->start, v->phys + (v->start - v->os_base), v->size) ? -1 : 0;
    return state;
}

/* Every page of [addr, addr + size) is ordinary read-write memory, and every
 * other view of it is too. Cached per VMA until the next mapping change. */
typedef struct eligible_entry {
    guest_addr start;
    uint64_t size, epoch;
    bool ok;
} eligible_entry;
static eligible_entry g_eligible[64];
static atomic_flag g_eligible_lock = ATOMIC_FLAG_INIT;

static bool vma_eligible(gmem *m, const gmem_vma *v) {
    uint64_t epoch = atomic_load(&g_map_epoch);
    uint32_t slot = (uint32_t)((v->start >> 14) * 0x9E3779B1u >> 26) & 63u;
    while (atomic_flag_test_and_set_explicit(&g_eligible_lock, memory_order_acquire)) {}
    eligible_entry e = g_eligible[slot];
    atomic_flag_clear_explicit(&g_eligible_lock, memory_order_release);
    if (e.start == v->start && e.size == v->size && e.epoch == epoch) return e.ok;
    bool ok = !v->reserved && v->prot == GPROT_RW && (v->kind != GVMA_VIEW || alias_state(m, v) >= 0);
    /* copy-on-write: exactly now (the alias table lags mapping changes by up to 100 ms, and a
     * view armed while another shows its memory would hide its writes from it) */
    if (cow()) ok = !v->reserved && v->prot == GPROT_RW && v->kind == GVMA_VIEW &&
                    !gmem_phys_aliased(m, v->start, v->phys + (v->start - v->os_base), v->size);
    while (atomic_flag_test_and_set_explicit(&g_eligible_lock, memory_order_acquire)) {}
    g_eligible[slot] = (eligible_entry){v->start, v->size, epoch, ok};
    atomic_flag_clear_explicit(&g_eligible_lock, memory_order_release);
    return ok;
}

static bool range_eligible(guest_addr addr, uint64_t size) {
    gmem *m = atomic_load(&g_mem);
    if (!m) return false;
    for (guest_addr at = addr; at < addr + size;) {
        gmem_vma v;
        if (!gmem_query_vma(m, at, false, &v) || v.start + v.size <= at || !vma_eligible(m, &v)) return false;
        at = v.start + v.size;
    }
    return true;
}

bool gpu_watch_eligible(guest_addr addr, uint64_t size) {
    return gpu_watch_enabled() && size && addr + size > addr && !((addr + size - 1) >> SPACE_BITS) &&
           range_eligible(addr, size);
}

static void lock(region *g) {
    while (atomic_flag_test_and_set_explicit(&g->lock, memory_order_acquire)) {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#endif
    }
}

static void unlock(region *g) { atomic_flag_clear_explicit(&g->lock, memory_order_release); }

static region *find_region(guest_addr addr) {
    if (addr >> SPACE_BITS) return NULL;
    return atomic_load_explicit(&g_regions[addr >> REGION_BITS], memory_order_acquire);
}

static region *get_region(guest_addr addr) {
    region *g = find_region(addr);
    if (g || (addr >> SPACE_BITS)) return g;
    region *n = (region *)malloc(sizeof(region));
    if (!n) return NULL;
    atomic_flag_clear(&n->lock);
    for (uint32_t i = 0; i < REGION_PAGES; i++) atomic_init(&n->page[i], (uint8_t)ST_WRITTEN);
    region *expected = NULL;
    if (!atomic_compare_exchange_strong(&g_regions[addr >> REGION_BITS], &expected, n)) {
        free(n);
        return expected;
    }
    return n;
}

static bool protect(guest_addr first_page, uint64_t pages, uint32_t prot) {
    return pal_vm_protect((void *)(uintptr_t)(first_page << PAGE_BITS), pages << PAGE_BITS, prot) == PS5C_OK;
}

/* Calls fn(region, region base page, first index, end index) over the pages of [addr, addr + size). */
#define FOR_REGIONS(addr, size, create, body)                                                                   \
    do {                                                                                                        \
        guest_addr p_ = (addr) >> PAGE_BITS, end_ = ((addr) + (size) + GPU_WATCH_PAGE - 1) >> PAGE_BITS;         \
        while (p_ < end_) {                                                                                     \
            guest_addr base_ = p_ & ~(guest_addr)(REGION_PAGES - 1);                                            \
            guest_addr stop_ = base_ + REGION_PAGES < end_ ? base_ + REGION_PAGES : end_;                       \
            region *g = (create) ? get_region(p_ << PAGE_BITS) : find_region(p_ << PAGE_BITS);                  \
            uint32_t first = (uint32_t)(p_ - base_), last = (uint32_t)(stop_ - base_);                          \
            guest_addr gbase = base_;                                                                           \
            (void)gbase;                                                                                        \
            body;                                                                                               \
            p_ = stop_;                                                                                         \
        }                                                                                                       \
    } while (0)

static bool protect_twins(guest_addr addr, uint64_t size);
static void twins_written(guest_addr addr, uint64_t size);
static void twins_save(guest_addr addr, uint64_t size);

/* ---- Copy-on-write mode ---- */

/* The pages of [addr, addr + size) the system no longer shows armed: written for every client. */
static bool cow_poll(guest_addr addr, uint64_t size) {
    guest_addr at = addr & ~(GPU_WATCH_PAGE - 1), end = (addr + size + GPU_WATCH_PAGE - 1) & ~(GPU_WATCH_PAGE - 1);
    bool found = false;
    while (at < end) {
        uint64_t off = 0, len = 0;
        bool priv = false;
        if (pal_vm_cow_clean((void *)(uintptr_t)at, end - at, &off, &len, &priv) || !len) break;
        uint8_t mark = (uint8_t)(ST_WRITTEN | (priv ? ST_PRIVATE : 0));
        guest_addr run = at + off;
        FOR_REGIONS(run, len, false, {
            if (g) {
                lock(g);
                for (uint32_t i = first; i < last; i++) {
                    uint8_t st = atomic_load_explicit(&g->page[i], memory_order_relaxed);
                    if (!(st & ST_PROT)) continue; /* released or marked meanwhile (the sweeper runs beside the clients) */
                    atomic_store_explicit(&g->page[i], (uint8_t)((st & (ST_OURS | ST_PRIVATE)) | mark), memory_order_release);
                }
                unlock(g);
            }
        });
        atomic_fetch_add_explicit(&g_generation, 1, memory_order_release);
        atomic_fetch_add_explicit(&g_faults, len >> PAGE_BITS, memory_order_relaxed);
        found = true;
        at = run + len;
    }
    return found;
}

/* cow_poll over the runs of [addr, addr + size) the tracker still holds armed. */
static void cow_sync(guest_addr addr, uint64_t size) {
    guest_addr end = (addr + size + GPU_WATCH_PAGE - 1) & ~(GPU_WATCH_PAGE - 1), run = 0;
    bool in_run = false;
    for (guest_addr p = addr & ~(GPU_WATCH_PAGE - 1); p <= end; p += GPU_WATCH_PAGE) {
        region *g = p < end ? find_region(p) : NULL;
        bool armed = g && (atomic_load_explicit(&g->page[(p >> PAGE_BITS) & (REGION_PAGES - 1)], memory_order_acquire) & ST_PROT);
        if (armed && !in_run) run = p, in_run = true;
        if (!armed && in_run) cow_poll(run, p - run), in_run = false;
    }
}

/* The sweeper: a thread that asks the system about every armed page, over and over, so that the
 * clients' checks are a look at the page states (as with write faults) instead of a question to
 * the system for every page of every texture and buffer used (a 4K texture has 8,000 pages).
 * A write is known one sweep later: a few milliseconds. PS5C_GPU_WATCH_SWEEP=0: no thread, each
 * check asks the system itself (exact, and slow for large ranges). */
static _Atomic uint64_t g_sweeps, g_sweep_ns;
void gpu_watch_sweep(void) {
    for (uint32_t r = 0; r < REGION_COUNT; r++) {
        region *g = atomic_load_explicit(&g_regions[r], memory_order_acquire);
        if (!g) continue;
        guest_addr base = (guest_addr)r << REGION_BITS;
        for (uint32_t i = 0; i < REGION_PAGES;) {
            if (!(atomic_load_explicit(&g->page[i], memory_order_relaxed) & ST_PROT)) {
                i++;
                continue;
            }
            uint32_t end = i + 1;
            while (end < REGION_PAGES && (atomic_load_explicit(&g->page[end], memory_order_relaxed) & ST_PROT)) end++;
            cow_poll(base + ((guest_addr)i << PAGE_BITS), (uint64_t)(end - i) << PAGE_BITS);
            i = end;
        }
    }
}
static int32_t sweeper_main(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t t0 = pal_time_ns();
        gpu_watch_sweep();
        uint64_t took = pal_time_ns() - t0;
        atomic_fetch_add_explicit(&g_sweeps, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_sweep_ns, took, memory_order_relaxed);
        pal_sleep_ns(took < 2000000ull ? 2000000ull : took); /* at most half of one core */
    }
    return 0;
}

static bool cow_arm(uint32_t client, guest_addr addr, uint64_t size) {
    if (atomic_load(&g_saver[client])) return false; /* a page would change without the saver seeing it */
    /* a private copy in the range: it cannot be armed, and asking the system again costs */
    FOR_REGIONS(addr, size, false, {
        if (g)
            for (uint32_t i = first; i < last; i++)
                if (atomic_load_explicit(&g->page[i], memory_order_acquire) & ST_PRIVATE) return false;
    });
    uint64_t epoch = atomic_load(&g_map_epoch);
    if (!range_eligible(addr, size)) return false;
    uint8_t bit = (uint8_t)(1u << client);
    bool ok = true;
    gate_lock();
    if (atomic_load(&g_map_epoch) != epoch) { /* the mappings moved meanwhile: the next use arms */
        gate_unlock();
        return false;
    }
    FOR_REGIONS(addr, size, true, {
        if (!g) {
            ok = false;
        } else {
            lock(g);
            /* the tracker's pages from here on, armed or not: cow_release looks at them all */
            for (uint32_t i = first; i < last; i++) atomic_fetch_or_explicit(&g->page[i], (uint8_t)ST_OURS, memory_order_relaxed);
            void *host = (void *)(uintptr_t)((gbase + first) << PAGE_BITS);
            uint64_t bytes = (uint64_t)(last - first) << PAGE_BITS;
            if (!pal_vm_cow_arm(host, bytes)) /* across host allocations: page by page */
                for (uint32_t i = first; i < last; i++) pal_vm_cow_arm((void *)(uintptr_t)((gbase + i) << PAGE_BITS), GPU_WATCH_PAGE);
            for (uint32_t i = first; i < last; i++) {
                if (!(atomic_load_explicit(&g->page[i], memory_order_relaxed) & ST_PROT)) atomic_fetch_add_explicit(&g_protected, 1, memory_order_relaxed);
                atomic_fetch_or_explicit(&g->page[i], (uint8_t)ST_PROT, memory_order_relaxed);
            }
            for (uint64_t at = 0; at < bytes;) { /* what the system says: private or failed pages are not armed */
                uint64_t off = 0;
                uint64_t len = 0;
                bool priv = false;
                if (pal_vm_cow_clean((uint8_t *)host + at, bytes - at, &off, &len, &priv) || !len) break;
                uint32_t from = first + (uint32_t)((at + off) >> PAGE_BITS);
                uint32_t to = from + (uint32_t)((len + GPU_WATCH_PAGE - 1) >> PAGE_BITS);
                for (uint32_t i = from; i < to && i < last; i++) atomic_store_explicit(&g->page[i], (uint8_t)(ST_OURS | ST_WRITTEN | (priv ? ST_PRIVATE : 0)), memory_order_release);
                ok = false;
                at += off + len;
            }
            for (uint32_t i = first; i < last; i++)
                if (atomic_load_explicit(&g->page[i], memory_order_relaxed) & ST_PROT)
                    atomic_fetch_and_explicit(&g->page[i], (uint8_t)~bit, memory_order_release);
            unlock(g);
        }
    });
    gate_unlock();
    return ok;
}

/* gmem hook (map lock held): the view [addr, addr + size) is about to be unmapped. Its private
 * copies go into the physical memory object, and the tracker forgets the pages. */
static void cow_release(guest_addr addr, uint64_t size, uint8_t *phys_copy) {
    if (!cow() || !size || (addr >> SPACE_BITS)) return;
    gate_lock(); /* an arm in flight on these pages ends first; one that has not started sees the epoch */
    atomic_fetch_add(&g_map_epoch, 1);
    gate_unlock();
    uint64_t t0 = pal_time_ns(), pages = 0;
    FOR_REGIONS(addr, size, false, {
        if (g) {
            lock(g);
            /* runs of the tracker's pages: made readable and asked about in batches. A question per
             * page costs too much here, with the map lock held (Wobbly Life stalled for seconds). */
            for (uint32_t i = first; i < last;) {
                if (!(atomic_load_explicit(&g->page[i], memory_order_relaxed) & ST_OURS)) {
                    i++;
                    continue;
                }
                uint32_t e = i + 1;
                while (e < last && (atomic_load_explicit(&g->page[e], memory_order_relaxed) & ST_OURS)) e++;
                uint8_t *host = (uint8_t *)(uintptr_t)((gbase + i) << PAGE_BITS);
                bool rw = pal_vm_protect(host, (uint64_t)(e - i) << PAGE_BITS, GPROT_RW) == PS5C_OK;
                for (uint32_t at = i; at < e;) {
                    uint8_t priv[256];
                    uint32_t n = e - at < 256 ? e - at : 256;
                    uint8_t *chunk = (uint8_t *)(uintptr_t)((gbase + at) << PAGE_BITS);
                    if (rw) {
                        pal_vm_private_pages(chunk, n, priv);
                    } else { /* across host allocations: page by page */
                        for (uint32_t k = 0; k < n; k++)
                            priv[k] = pal_vm_protect(chunk + ((uint64_t)k << PAGE_BITS), GPU_WATCH_PAGE, GPROT_RW) == PS5C_OK &&
                                      pal_vm_page_private(chunk + ((uint64_t)k << PAGE_BITS));
                    }
                    for (uint32_t k = 0; k < n; k++) {
                        if (priv[k] && phys_copy)
                            memcpy(phys_copy + (((gbase + at + k) << PAGE_BITS) - addr), chunk + ((uint64_t)k << PAGE_BITS), GPU_WATCH_PAGE);
                        if (priv[k]) atomic_fetch_add_explicit(&g_cow_flushed, 1, memory_order_relaxed);
                        atomic_store_explicit(&g->page[at + k], (uint8_t)ST_WRITTEN, memory_order_release);
                    }
                    at += n;
                }
                pages += e - i;
                i = e;
            }
            unlock(g);
        }
    });
    uint64_t took = pal_time_ns() - t0;
    if (took > 20000000ull)
        LOG_W(GPU, "page watch: releasing 0x%llx+0x%llx took %llu ms (%llu tracked pages)", (unsigned long long)addr,
              (unsigned long long)size, (unsigned long long)(took / 1000000ull), (unsigned long long)pages);
    atomic_fetch_add_explicit(&g_generation, 1, memory_order_release);
}

/* gmem hook (map lock held): another view of this view's physical memory is about to be mapped.
 * True when the tracker has pages here: the caller then maps the view again (cow_release runs). */
static bool cow_aliased(guest_addr addr, uint64_t size) {
    if (!cow() || !size || (addr >> SPACE_BITS)) return false;
    gate_lock();
    atomic_fetch_add(&g_map_epoch, 1);
    gate_unlock();
    bool any = false;
    FOR_REGIONS(addr, size, false, {
        if (g && !any)
            for (uint32_t i = first; i < last && !any; i++) any = (atomic_load_explicit(&g->page[i], memory_order_acquire) & ST_OURS) != 0;
    });
    return any;
}

bool gpu_watch_arm(uint32_t client, guest_addr addr, uint64_t size) {
    if (!gpu_watch_enabled() || client >= GPU_WATCH_CLIENTS || !size || addr + size < addr ||
        ((addr + size - 1) >> SPACE_BITS))
        return false;
    if (cow()) return cow_arm(client, addr, size);
    if (!range_eligible(addr, size)) return false;
    if (!protect_twins(addr, size)) return false;
    uint8_t bit = (uint8_t)(1u << client);
    bool ok = true;
    FOR_REGIONS(addr, size, true, {
        if (!g) return false;
        lock(g);
        uint32_t run = UINT32_MAX;
        for (uint32_t i = first; i <= last; i++) {
            bool need = i < last && !(atomic_load_explicit(&g->page[i], memory_order_relaxed) & ST_PROT);
            if (need && run == UINT32_MAX) run = i;
            if (!need && run != UINT32_MAX) { /* protect the pages [run, i) together */
                bool whole = protect(gbase + run, i - run, GPROT_READ);
                for (uint32_t k = run; k < i; k++) {
                    /* a run across host allocations (one VirtualProtect cannot): page by page */
                    if (!whole && !protect(gbase + k, 1, GPROT_READ)) {
                        ok = false;
                        continue;
                    }
                    atomic_fetch_or_explicit(&g->page[k], (uint8_t)(ST_PROT | ST_OURS), memory_order_relaxed);
                    atomic_fetch_add_explicit(&g_protected, 1, memory_order_relaxed);
                }
                run = UINT32_MAX;
            }
        }
        /* Cleared after the protection: a write before it changed the bytes the caller reads next. */
        for (uint32_t i = first; i < last; i++)
            if (atomic_load_explicit(&g->page[i], memory_order_relaxed) & ST_PROT)
                atomic_fetch_and_explicit(&g->page[i], (uint8_t)~bit, memory_order_release);
        unlock(g);
    });
    return ok;
}

/* The other views of an armed range become read-only too: a write through
 * them must fault. Their own written bits stay as they are. */
static bool protect_twins(guest_addr addr, uint64_t size) {
    guest_addr start[ALIAS_TWINS];
    uint64_t len[ALIAS_TWINS];
    bool writable[ALIAS_TWINS];
    uint32_t n = alias_twins(addr, size, start, len, writable);
    bool ok = true;
    for (uint32_t t = 0; t < n; t++) {
        if (!writable[t]) continue; /* nothing is written through it */
        FOR_REGIONS(start[t], len[t], true, {
            if (!g) return false;
            lock(g);
            for (uint32_t i = first; i < last; i++) {
                if (atomic_load_explicit(&g->page[i], memory_order_relaxed) & ST_PROT) continue;
                if (!protect(gbase + i, 1, GPROT_READ)) {
                    ok = false;
                    continue;
                }
                atomic_fetch_or_explicit(&g->page[i], (uint8_t)(ST_PROT | ST_OURS), memory_order_relaxed);
                atomic_fetch_add_explicit(&g_protected, 1, memory_order_relaxed);
            }
            unlock(g);
        });
    }
    return ok;
}

/* The pages of the range are written for every client, and as accessible as
 * the guest mapped them. Called with no region locked. */
static void range_written(guest_addr start, uint64_t len, bool writable) {
    FOR_REGIONS(start, len, false, {
        if (g) {
            lock(g);
            for (uint32_t i = first; i < last; i++) {
                uint8_t st = atomic_load_explicit(&g->page[i], memory_order_relaxed);
                if (st & ST_PROT) {
                    save_page(gbase + i, st);
                    if (!protect(gbase + i, 1, writable ? GPROT_RW : GPROT_READ)) continue;
                }
                atomic_store_explicit(&g->page[i], (uint8_t)((st & ST_OURS) | ST_WRITTEN), memory_order_release);
            }
            unlock(g);
        }
    });
    atomic_fetch_add_explicit(&g_generation, 1, memory_order_release);
}

/* Before any view of [addr, addr + size) becomes writable: the savers see the
 * protected pages of the other views. Otherwise a second thread could write
 * through the view just released before the saver of another view ran, and
 * that saver would keep the new bytes as what the GPU had read. */
static void twins_save(guest_addr addr, uint64_t size) {
    guest_addr start[ALIAS_TWINS];
    uint64_t len[ALIAS_TWINS];
    bool writable[ALIAS_TWINS];
    uint32_t n = alias_twins(addr, size, start, len, writable);
    for (uint32_t t = 0; t < n; t++) {
        FOR_REGIONS(start[t], len[t], false, {
            if (g) {
                lock(g);
                for (uint32_t i = first; i < last; i++) {
                    uint8_t st = atomic_load_explicit(&g->page[i], memory_order_relaxed);
                    if (st & ST_PROT) save_page(gbase + i, st);
                }
                unlock(g);
            }
        });
    }
}

/* [addr, addr + size) was written (or is about to be): so were the other views of it. */
static void twins_written(guest_addr addr, uint64_t size) {
    guest_addr start[ALIAS_TWINS];
    uint64_t len[ALIAS_TWINS];
    bool writable[ALIAS_TWINS];
    uint32_t n = alias_twins(addr, size, start, len, writable);
    for (uint32_t t = 0; t < n; t++) range_written(start[t], len[t], writable[t]);
}

bool gpu_watch_written(uint32_t client, guest_addr addr, uint64_t size) {
    if (!gpu_watch_enabled() || client >= GPU_WATCH_CLIENTS || !size || addr + size < addr) return true;
    uint8_t bit = (uint8_t)(1u << client);
    FOR_REGIONS(addr, size, false, {
        if (!g) return true;
        for (uint32_t i = first; i < last; i++)
            if (atomic_load_explicit(&g->page[i], memory_order_acquire) & bit) return true;
    });
    /* armed as far as the tracker knows: the sweeper keeps that current, or the system is asked now */
    return cow() && !atomic_load_explicit(&g_sweeper, memory_order_relaxed) && cow_poll(addr, size);
}

bool gpu_watch_find_written(uint32_t client, guest_addr addr, uint64_t size, guest_addr *run, uint64_t *len) {
    if (!size || addr + size < addr || client >= GPU_WATCH_CLIENTS) return false;
    guest_addr end = addr + size, start = 0;
    bool in_run = false;
    uint8_t bit = (uint8_t)(1u << client);
    bool on = gpu_watch_enabled();
    if (on && cow() && !atomic_load_explicit(&g_sweeper, memory_order_relaxed)) cow_sync(addr, size);
    region *g = NULL;
    guest_addr g_at = ~(guest_addr)0;
    for (guest_addr p = addr & ~(GPU_WATCH_PAGE - 1); p < end; p += GPU_WATCH_PAGE) {
        if ((p >> REGION_BITS) != g_at) g_at = p >> REGION_BITS, g = on ? find_region(p) : NULL;
        bool w = !g || (atomic_load_explicit(&g->page[(p >> PAGE_BITS) & (REGION_PAGES - 1)], memory_order_acquire) & bit);
        if (w && !in_run) start = p < addr ? addr : p, in_run = true;
        if (!w && in_run) break;
    }
    if (!in_run) return false;
    guest_addr stop = start;
    for (guest_addr p = start & ~(GPU_WATCH_PAGE - 1); p < end; p += GPU_WATCH_PAGE) {
        if ((p >> REGION_BITS) != g_at) g_at = p >> REGION_BITS, g = on ? find_region(p) : NULL;
        bool w = !g || (atomic_load_explicit(&g->page[(p >> PAGE_BITS) & (REGION_PAGES - 1)], memory_order_acquire) & bit);
        if (!w) break;
        stop = p + GPU_WATCH_PAGE;
    }
    *run = start;
    *len = (stop < end ? stop : end) - start;
    return true;
}

/* A write keeps faulting on a page that is not protected by this module (as far as
 * its state says) while its mapping is ordinary read-write memory. A mapping change
 * raced an arm: the change reset the host protection, the arm protected the page
 * again, then the change's hook cleared the page state. The fault would be retried
 * forever (gmem_retry_fault: the mapping allows the write), so the page is made
 * writable again and counts as written for every client. */
static _Atomic uint64_t g_repaired;
static bool repair_stale(region *g, uint32_t i, guest_addr page_index) {
    gmem *m = atomic_load(&g_mem);
    gmem_vma v;
    guest_addr at = page_index << PAGE_BITS;
    if (!m || !gmem_query_vma(m, at, false, &v) || v.start > at || v.start + v.size <= at || v.reserved ||
        v.prot != GPROT_RW)
        return false;
    lock(g);
    bool ok = protect(page_index, 1, GPROT_RW);
    if (ok) {
        atomic_store_explicit(&g->page[i], (uint8_t)ST_WRITTEN, memory_order_release);
        atomic_fetch_add_explicit(&g_generation, 1, memory_order_release);
    }
    unlock(g);
    if (ok && atomic_fetch_add(&g_repaired, 1) < 8)
        LOG_W(GPU, "page watch: 0x%llx stayed read-only after a mapping change, made writable again",
              (unsigned long long)at);
    return ok;
}

bool gpu_watch_fault(guest_addr addr, bool write) {
    if (!write || cow()) return false;
    region *g = find_region(addr);
    if (!g) return false;
    uint32_t i = (uint32_t)((addr >> PAGE_BITS) & (REGION_PAGES - 1));
    /* consecutive faults of this thread on this page that found it writable already */
    static _Thread_local guest_addr last;
    static _Thread_local uint32_t repeats;
    if (last != (addr >> PAGE_BITS)) {
        last = addr >> PAGE_BITS;
        repeats = 0;
    }
    if (atomic_load_explicit(&g->page[i], memory_order_relaxed) & ST_PROT)
        twins_save(addr & ~(GPU_WATCH_PAGE - 1), (uint64_t)FAULT_SPAN << PAGE_BITS);
    lock(g);
    uint8_t st = atomic_load_explicit(&g->page[i], memory_order_relaxed);
    bool handled = false;
    guest_addr released = 0;
    uint64_t released_size = 0;
    if (st & ST_PROT) {
        /* Bulk writes (memcpy, memset) go on into the next pages: those protected
         * after this one are released with it, which costs one fault per
         * FAULT_SPAN pages instead of one per page and only marks them early. */
        uint32_t end = i + 1;
        while (end < REGION_PAGES && end < i + FAULT_SPAN &&
               (atomic_load_explicit(&g->page[end], memory_order_relaxed) & ST_PROT))
            end++;
        guest_addr gbase = (addr >> PAGE_BITS) - i;
        for (uint32_t k = i; k < end; k++)
            save_page(gbase + k, atomic_load_explicit(&g->page[k], memory_order_relaxed));
        handled = protect(addr >> PAGE_BITS, end - i, GPROT_RW);
        if (!handled && end > i + 1) {
            /* the run crosses into another host allocation (one VirtualProtect cannot): this page only */
            end = i + 1;
            handled = protect(addr >> PAGE_BITS, 1, GPROT_RW);
        }
        if (handled) {
            repeats = 0;
            for (uint32_t k = i; k < end; k++)
                atomic_store_explicit(&g->page[k], (uint8_t)(ST_OURS | ST_WRITTEN), memory_order_release);
            atomic_fetch_add_explicit(&g_generation, 1, memory_order_release);
            atomic_fetch_add_explicit(&g_faults, 1, memory_order_relaxed);
            released = (gbase + i) << PAGE_BITS;
            released_size = (uint64_t)(end - i) << PAGE_BITS;
        }
    } else {
        /* OURS: another thread made it writable between the fault and this lock: retry. A page whose
         * protection changed without gpu_watch_mapping_changed would loop here: give up after a while. */
        repeats++;
        handled = (st & ST_OURS) && repeats < 64;
    }
    unlock(g);
    /* before the write is retried: no client may find a twin clean after it */
    if (released_size) twins_written(released, released_size);
    if (!handled && repeats >= 8 && (handled = repair_stale(g, i, addr >> PAGE_BITS))) repeats = 0;
    return handled;
}

void gpu_watch_mapping_changed(guest_addr addr, uint64_t size) {
    if (!size || addr + size < addr || (addr >> SPACE_BITS)) return;
    if ((addr + size - 1) >> SPACE_BITS) size = ((guest_addr)1 << SPACE_BITS) - addr;
    atomic_fetch_add(&g_map_epoch, 1);
    atomic_fetch_add(&g_generation, 1);
    FOR_REGIONS(addr, size, false, {
        if (g) {
            lock(g);
            for (uint32_t i = first; i < last; i++) {
                /* copy-on-write: a page armed before stays the tracker's until its view goes (cow_release) */
                uint8_t keep = cow() ? (uint8_t)(atomic_load_explicit(&g->page[i], memory_order_relaxed) & (ST_OURS | ST_PRIVATE)) : 0;
                atomic_store_explicit(&g->page[i], (uint8_t)(keep | ST_WRITTEN), memory_order_release);
            }
            unlock(g);
        }
    });
}

void gpu_watch_prepare_write(guest_addr addr, uint64_t size) {
    if (!size || addr + size < addr || ((addr + size - 1) >> SPACE_BITS)) return;
    if (cow()) return; /* the system's own writes turn the pages private like any other */
    twins_save(addr, size);
    FOR_REGIONS(addr, size, false, {
        if (g) {
            lock(g);
            for (uint32_t i = first; i < last;) {
                uint32_t end = i + 1;
                bool prot = atomic_load_explicit(&g->page[i], memory_order_relaxed) & ST_PROT;
                while (end < last && (bool)(atomic_load_explicit(&g->page[end], memory_order_relaxed) & ST_PROT) == prot)
                    end++;
                if (prot)
                    for (uint32_t k = i; k < end; k++)
                        save_page(gbase + k, atomic_load_explicit(&g->page[k], memory_order_relaxed));
                bool whole = !prot || protect(gbase + i, end - i, GPROT_RW);
                for (uint32_t k = i; k < end; k++) {
                    /* a run across host allocations: page by page */
                    if (!whole && !protect(gbase + k, 1, GPROT_RW)) continue;
                    uint8_t st = atomic_load_explicit(&g->page[k], memory_order_relaxed);
                    atomic_store_explicit(&g->page[k], (uint8_t)((st & ST_OURS) | ST_WRITTEN), memory_order_release);
                }
                if (prot) atomic_fetch_add_explicit(&g_generation, 1, memory_order_release);
                i = end;
            }
            unlock(g);
        }
    });
    twins_written(addr, size);
}

void gpu_watch_counts(uint64_t *faults, uint64_t *protected_pages) {
    *faults = atomic_exchange(&g_faults, 0);
    *protected_pages = atomic_exchange(&g_protected, 0);
}

/* Diagnostics: what the tracker and the mapping say about a page. */
void gpu_watch_log_page(guest_addr addr) {
    region *g = find_region(addr);
    gmem *m = atomic_load(&g_mem);
    gmem_vma v;
    memset(&v, 0, sizeof(v));
    bool have = m && gmem_query_vma(m, addr, false, &v);
    LOG_W(GPU, "page watch debug 0x%llx: region %p state 0x%02x; vma %d 0x%llx+0x%llx prot 0x%x reserved %d demand %d kind %d os_base 0x%llx phys 0x%llx '%s' eligible %d",
          (unsigned long long)addr, (void *)g,
          g ? atomic_load(&g->page[(addr >> PAGE_BITS) & (REGION_PAGES - 1)]) : 0, have,
          (unsigned long long)v.start, (unsigned long long)v.size, v.prot, v.reserved, v.demand, v.kind,
          (unsigned long long)v.os_base, (unsigned long long)v.phys, v.name, have && vma_eligible(m, &v));
    if (have && m) {
        int state = v.kind == GVMA_VIEW ? alias_state(m, &v) : 9;
        guest_addr ts[ALIAS_TWINS];
        uint64_t tl[ALIAS_TWINS];
        bool tw[ALIAS_TWINS];
        uint32_t tn = alias_twins(addr, 1, ts, tl, tw);
        LOG_W(GPU, "page watch debug: alias state %d, table %u views%s, %u twins (first 0x%llx%s), range_eligible(page) %d, old aliased %d",
              state, g_alias_count, g_alias_overflow ? " OVERFLOW" : "", tn, tn ? (unsigned long long)ts[0] : 0ull,
              tn && !tw[0] ? " read-only" : "", range_eligible(addr & ~(GPU_WATCH_PAGE - 1), GPU_WATCH_PAGE),
              v.kind == GVMA_VIEW && view_aliased(m, v.start, v.phys + (v.start - v.os_base), v.size));
    }
}
