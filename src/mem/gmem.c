/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mem/gmem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/endian.h"
#include "base/log.h"
#include "pal/pal.h"

#define GMEM_HISTORY_COUNT 512
typedef struct gmem_event {
    uint64_t sequence, time_ns, thread, size, phys;
    guest_addr addr;
    uint32_t prot, flags;
    ps5c_err result;
    const char *operation; /* static string */
} gmem_event;

struct gmem_history {
    uint64_t count;
    gmem_event events[GMEM_HISTORY_COUNT];
    /* Reserved at startup: a terminal fault must not allocate from the heap.
     * Map lock protects ownership, but logging runs with the lock released. */
    bool snapshot_busy;
    gmem_event snapshot[GMEM_HISTORY_COUNT];
    guest_addr trace_addr;
    uint32_t trace_size;
    bool trace_valid, trace_sampled;
    uint8_t trace_bytes[256];
};

static void trace_memory_locked(gmem *m, const char *phase, guest_addr changed_addr, uint64_t changed_size);

/* Caller holds the map lock. No file logging on the mapping hot path. */
static void record_map_locked(gmem *m, const char *op, guest_addr addr, uint64_t size,
                              uint64_t phys, uint32_t prot, uint32_t flags, ps5c_err result) {
    if (!m->history) return;
    uint64_t n = m->history->count++;
    m->history->events[n % GMEM_HISTORY_COUNT] =
        (gmem_event){n + 1, pal_time_ns(), pal_thread_current_id(), size, phys, addr, prot, flags, result, op};
    trace_memory_locked(m, op, addr, size);
}

gmem_config gmem_default_config(void) {
    gmem_config c;
    c.start = GUEST_SYSTEM_RESERVED_BASE;
    c.size = GUEST_SPACE_END - GUEST_SYSTEM_RESERVED_BASE;
    c.identity = true;
    c.alloc_hint = GUEST_MODULE_BASE;
    c.demand_anonymous = false;
#ifdef _WIN32
    const char *demand = getenv("PS5C_DEMAND_GPU");
    c.demand_anonymous = demand && !strcmp(demand, "1");
#endif
    return c;
}

/* Identity reservation made at process start, before the host heap or DLLs
 * can land in the guest range (ASLR). Adopted by the next gmem_init that
 * asks for exactly this range. The placeholder area above GUEST_PH_BASE is a
 * reservation of its own. */
static void *g_early_res;
static guest_addr g_early_start;
static uint64_t g_early_size;
static guest_addr g_early_ph_start;

/* Opt-in until fixed mappings of more titles have been verified. GTA V
 * places mirrored physical pages around 0x14..., below the default area. */
static guest_addr g_shared_area_base; /* gmem_set_shared_area_base; 0 = default */

static guest_addr shared_area_base(void) {
    const char *e = getenv("PS5C_LOW_SHARED_AREA");
    if (!e && g_shared_area_base) return g_shared_area_base;
    /* The Astro CPU pool at 0x3... can also use eagerly mapped physical
     * pages. File-backed views remain committed, including fiber stacks;
     * this does not enable first-touch fault handling for CPU memory. */
    if (e && strcmp(e, "3") == 0) return 0x280000000ull;
    /* Experimental lower placeholder boundary: graphics pools around 0x5...
     * can share their physical backing too; the CPU heap at 0x3... stays plain. */
    if (e && strcmp(e, "2") == 0) return 0x400000000ull;
    return e && strcmp(e, "1") == 0 ? 0x1000000000ull : GUEST_PH_BASE;
}

/* Reserves [start, end): below the selected split as plain address space,
 * the rest as placeholders when supported (*ph_start is end otherwise). */
static ps5c_err reserve_identity(guest_addr start, guest_addr end, void **res, guest_addr *ph_start) {
    *ph_start = end;
    guest_addr split = end;
    guest_addr base = shared_area_base();
    bool off = getenv("PS5C_NO_SHARED_AREA") != NULL; /* debugging aid: plain memory only */
    if (!off && pal_ph_supported() && start < base && end > base) split = base;
    ps5c_err err = pal_vm_reserve((void *)(uintptr_t)start, split - start, true, res);
    if (err != PS5C_OK || split == end) return err;
    if (pal_ph_reserve((void *)(uintptr_t)split, end - split) == PS5C_OK) {
        *ph_start = split;
        return PS5C_OK;
    }
    /* no placeholders: plain address space there too (one more reservation) */
    void *hi = NULL;
    if (pal_vm_reserve((void *)(uintptr_t)split, end - split, true, &hi) != PS5C_OK) {
        pal_vm_release(*res, split - start);
        return PS5C_ERR_BUSY;
    }
    return PS5C_OK;
}

ps5c_err gmem_reserve_early(void) {
    if (g_early_res) return PS5C_OK;
    gmem_config cfg = gmem_default_config();
    ps5c_err err = reserve_identity(cfg.start, cfg.start + cfg.size, &g_early_res, &g_early_ph_start);
    if (err != PS5C_OK) {
        g_early_res = NULL;
        return err;
    }
    g_early_start = cfg.start;
    g_early_size = cfg.size;
    return PS5C_OK;
}

ps5c_err gmem_set_shared_area_base(guest_addr base) {
    if (getenv("PS5C_LOW_SHARED_AREA")) return PS5C_OK;
    g_shared_area_base = base;
    if (!g_early_res || g_early_ph_start == base) return PS5C_OK;
    pal_vm_release_range(g_early_res, g_early_size);
    g_early_res = NULL;
    return gmem_reserve_early();
}

static uint8_t *host_ptr(const gmem *m, guest_addr a) {
    return m->identity ? (uint8_t *)(uintptr_t)a : m->host_base + a;
}

static bool in_ph(const gmem *m, guest_addr a) { return a >= m->ph_start && a < m->res_end; }

ps5c_err gmem_init(gmem *m, const gmem_config *cfg) {
    memset(m, 0, sizeof(*m));
    if (cfg->size == 0 || cfg->start % GUEST_PAGE_SIZE || cfg->size % GUEST_PAGE_SIZE ||
        cfg->start > UINT64_MAX - cfg->size) {
        return PS5C_ERR_INVALID;
    }
    ps5c_err err = pal_mutex_create(&m->lock);
    if (err != PS5C_OK) {
        return err;
    }
    err = pal_cond_create(&m->mapping_idle);
    if (err != PS5C_OK) {
        pal_mutex_destroy(m->lock);
        m->lock = NULL;
        return err;
    }
    void *res = NULL;
    guest_addr ph_start = cfg->start + cfg->size;
    if (cfg->identity && g_early_res && cfg->start == g_early_start && cfg->size == g_early_size) {
        res = g_early_res;
        ph_start = g_early_ph_start;
        g_early_res = NULL;
    } else if (cfg->identity) {
        err = reserve_identity(cfg->start, cfg->start + cfg->size, &res, &ph_start);
        if (err != PS5C_OK) {
            LOG_E(MEM, "cannot reserve guest range 0x%llx-0x%llx at the same host address (%s)",
                  (unsigned long long)cfg->start, (unsigned long long)(cfg->start + cfg->size),
                  ps5c_err_str(err));
        }
    } else {
        err = pal_vm_reserve(NULL, cfg->size, false, &res);
    }
    if (err != PS5C_OK) {
        pal_cond_destroy(m->mapping_idle);
        m->mapping_idle = NULL;
        pal_mutex_destroy(m->lock);
        m->lock = NULL;
        return err;
    }
    m->identity = cfg->identity;
    m->demand_anonymous = cfg->demand_anonymous;
    m->reservation = res;
    m->res_start = cfg->start;
    m->res_end = cfg->start + cfg->size;
    m->ph_start = m->res_end;
    if (ph_start < m->res_end) {
        err = pal_shm_create(GUEST_SHM_SIZE, &m->shm);
        if (err == PS5C_OK) m->ph_start = ph_start;
        else if (getenv("PS5C_SHM_BACKING_FILE")) {
            /* An explicitly requested backing must not silently become many
             * private copies, which would defeat its memory limit. */
            gmem_free(m);
            return err;
        }
    }
    m->host_base = cfg->identity ? NULL : (uint8_t *)res - cfg->start;
    m->alloc_hint = cfg->alloc_hint >= m->res_start && cfg->alloc_hint < m->res_end ? cfg->alloc_hint
                                                                                    : m->res_start;
    if (getenv("PS5C_FAULT_MAPS")) {
        m->history = calloc(1, sizeof(*m->history));
        if (!m->history) LOG_W(MEM, "fault map history unavailable: allocation failed");
        const char *trace = getenv("PS5C_TRACE_MEMORY");
        if (m->history && trace) {
            char *tail;
            uint64_t at = strtoull(trace, &tail, 16), size = *tail == ':' ? strtoull(tail + 1, &tail, 0) : 0;
            if (!*tail && at >= m->res_start && at < m->res_end && size &&
                size <= sizeof(m->history->trace_bytes) && size <= m->res_end - at) {
                m->history->trace_addr = at;
                m->history->trace_size = (uint32_t)size;
            }
        }
    }
    LOG_I(MEM, "guest memory: 0x%llx-0x%llx reserved (%s)%s", (unsigned long long)m->res_start,
          (unsigned long long)m->res_end, m->identity ? "identity" : "offset",
          m->ph_start < m->res_end ? ", shared-memory area from 0x" : "");
    if (m->ph_start < m->res_end) LOG_I(MEM, "  shared-memory area 0x%llx-0x%llx", (unsigned long long)m->ph_start,
                                        (unsigned long long)m->res_end);
    return PS5C_OK;
}

void gmem_free(gmem *m) {
    if (m->lock) {
        pal_mutex_lock(m->lock);
        __atomic_store_n(&m->closing, true, __ATOMIC_SEQ_CST);
        __atomic_add_fetch(&m->mapping_writers, 1, __ATOMIC_SEQ_CST); /* no more pins without the lock */
        while (__atomic_load_n(&m->mapping_pins, __ATOMIC_SEQ_CST)) pal_cond_wait(m->mapping_idle, m->lock);
        pal_mutex_unlock(m->lock);
    }
    if (m->reservation) {
        /* identity: the range may hold several reservations (the placeholder
         * area above GUEST_PH_BASE, the views and memory placed in it) */
        if (m->identity) pal_vm_release_range(m->reservation, m->res_end - m->res_start);
        else pal_vm_release(m->reservation, m->res_end - m->res_start);
    }
    /* The COW tracker's ordinary host view is outside the guest reservation.
     * Closing its section handle does not unmap it or release its pages. */
    if (m->phys_win) pal_vm_release_range(m->phys_win,GUEST_SHM_SIZE);
    pal_shm_destroy(m->shm);
    free(m->history);
    dyn_free(&m->vmas);
    if (m->mapping_idle) pal_cond_destroy(m->mapping_idle);
    if (m->lock) {
        pal_mutex_destroy(m->lock);
    }
    memset(m, 0, sizeof(*m));
}

bool gmem_pin_mappings(gmem *m) {
    return gmem_pin_mappings_version(m, false, NULL);
}

bool gmem_retain_mappings(gmem *m) {
    return gmem_pin_mappings_version(m, true, NULL);
}

/* Pins without the map lock while no writer waits (the renderer pins at every
 * draw; the lock is held through the host calls of mapping changes, ~1-2k a
 * second while Minecraft loads a world). mapping_pins and mapping_writers are
 * sequentially consistent: a writer counts itself and then reads the pins, a
 * pin counts itself and then reads the writers, so one of them sees the other.
 * A writer that sees no pins changes the mappings after it moved the
 * generation, and a pin taken meanwhile reads that generation: its first
 * lookups (which take the lock) wait for the change. PS5C_PIN_LOCKED=1: always
 * with the lock, as before. */
static bool pin_fast(gmem *m, bool retain, uint64_t *generation) {
    static int locked = -1;
    if (locked < 0) locked = getenv("PS5C_PIN_LOCKED") && atoi(getenv("PS5C_PIN_LOCKED"));
    if (locked) return false;
    uint32_t pins = __atomic_load_n(&m->mapping_pins, __ATOMIC_SEQ_CST);
    if (retain) { /* joins pins already held: never waits for writers */
        while (pins && pins != UINT32_MAX)
            if (__atomic_compare_exchange_n(&m->mapping_pins, &pins, pins + 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                if (generation) *generation = __atomic_load_n(&m->mapping_generation, __ATOMIC_SEQ_CST);
                return true;
            }
        return false;
    }
    if (__atomic_load_n(&m->mapping_writers, __ATOMIC_SEQ_CST) || __atomic_load_n(&m->closing, __ATOMIC_SEQ_CST) ||
        pins >= UINT32_MAX - 1024u)
        return false;
    __atomic_add_fetch(&m->mapping_pins, 1, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&m->mapping_writers, __ATOMIC_SEQ_CST)) {
        if (generation) *generation = __atomic_load_n(&m->mapping_generation, __ATOMIC_SEQ_CST);
        return true;
    }
    gmem_unpin_mappings(m); /* a writer came: it goes first */
    return false;
}

bool gmem_pin_mappings_version(gmem *m, bool retain, uint64_t *generation) {
    if (pin_fast(m, retain, generation)) return true;
    pal_mutex_lock(m->lock);
    /* Give a waiting mapping operation a turn between device submissions. */
    while (!retain && __atomic_load_n(&m->mapping_writers, __ATOMIC_SEQ_CST) && !m->closing)
        pal_cond_wait(m->mapping_idle, m->lock);
    uint32_t pins = __atomic_load_n(&m->mapping_pins, __ATOMIC_SEQ_CST);
    bool ok = (retain ? pins != 0 : !m->closing) && pins != UINT32_MAX;
    if (ok) {
        __atomic_add_fetch(&m->mapping_pins, 1, __ATOMIC_SEQ_CST);
        if (generation) *generation = __atomic_load_n(&m->mapping_generation, __ATOMIC_SEQ_CST);
    }
    pal_mutex_unlock(m->lock);
    return ok;
}

void gmem_unpin_mappings(gmem *m) {
    uint32_t pins = __atomic_load_n(&m->mapping_pins, __ATOMIC_SEQ_CST);
    while (pins && !__atomic_compare_exchange_n(&m->mapping_pins, &pins, pins - 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {}
    /* the last pin wakes a waiting writer (it counted itself before it read the pins) */
    if (pins == 1 && __atomic_load_n(&m->mapping_writers, __ATOMIC_SEQ_CST)) {
        pal_mutex_lock(m->lock);
        pal_cond_broadcast(m->mapping_idle);
        pal_mutex_unlock(m->lock);
    }
}

/* Caller holds the map lock. Readers release it while devices work, so CPU
 * queries remain available; only changes to the host mappings wait here. */
static void mapping_write_wait(gmem *m) {
    __atomic_add_fetch(&m->mapping_writers, 1, __ATOMIC_SEQ_CST);
    bool waited = false;
    while (__atomic_load_n(&m->mapping_pins, __ATOMIC_SEQ_CST)) pal_cond_wait(m->mapping_idle, m->lock), waited = true;
    /* Conservative invalidation also covers a mutation that later fails after
     * changing part of a range. No borrowed pointer may span this generation. */
    __atomic_add_fetch(&m->mapping_generation, 1, __ATOMIC_SEQ_CST);
    __atomic_sub_fetch(&m->mapping_writers, 1, __ATOMIC_SEQ_CST);
    if (waited) pal_cond_broadcast(m->mapping_idle);
}

static size_t lower_bound(const gmem *m, guest_addr addr);

/* Caller holds the map lock. True when [addr, addr + size) holds no backed
 * memory (free or reserved address space only): no borrowed host pointer can
 * point there, so placing memory there needs no wait for the pins. */
static bool range_unbacked(const gmem *m, guest_addr addr, uint64_t size) {
    for (size_t i = lower_bound(m, addr); i < m->vmas.len && m->vmas.data[i].start < addr + size; i++)
        if (!m->vmas.data[i].reserved) return false;
    return true;
}

/* mapping_write_wait for a change that only places memory where there was
 * none ([addr, addr + size) unbacked, or `fresh`: a range the map lock's
 * holder chooses itself among free addresses). Pinned readers keep their
 * pointers; the generation still moves, so their caches start again at the
 * next pin. PS5C_MAP_WAIT_ALWAYS=1: wait as for any change. */
static void mapping_place_wait(gmem *m, bool fresh, guest_addr addr, uint64_t size) {
    static int always = -1;
    if (always < 0) always = getenv("PS5C_MAP_WAIT_ALWAYS") && atoi(getenv("PS5C_MAP_WAIT_ALWAYS"));
    if (!always && (fresh || range_unbacked(m, addr, size))) __atomic_add_fetch(&m->mapping_generation, 1, __ATOMIC_SEQ_CST);
    else mapping_write_wait(m);
}

/* Mapping changes reset the host protection of their pages: the write tracker of the
 * GPU caches (gpu_watch) must forget what it protected there. Called with the lock held. */
static void (*g_change_hook)(guest_addr addr, uint64_t size);
void gmem_set_change_hook(void (*fn)(guest_addr addr, uint64_t size)) { g_change_hook = fn; }
static void (*g_release_hook)(guest_addr addr, uint64_t size, uint8_t *phys_copy);
static bool (*g_alias_hook)(guest_addr addr, uint64_t size);
void gmem_set_cow_hooks(void (*release)(guest_addr addr, uint64_t size, uint8_t *phys_copy),
                        bool (*aliased)(guest_addr addr, uint64_t size)) {
    g_release_hook = release;
    g_alias_hook = aliased;
}
static bool (*g_world_stop)(void *user);
static void (*g_world_resume)(void *user);
static void *g_world_user;
void gmem_set_world_hooks(bool (*stop)(void *user), void (*resume)(void *user), void *user) {
    g_world_user = user;
    g_world_resume = resume;
    g_world_stop = stop;
}
/* Caller holds the map lock. */
static uint8_t *phys_window(gmem *m) {
    if (!m->phys_win && m->shm) {
        void *p = NULL;
        if (pal_shm_map(m->shm, GUEST_SHM_SIZE, &p) == PS5C_OK) m->phys_win = (uint8_t *)p;
    }
    return m->phys_win;
}
bool gmem_phys_aliased(gmem *m, guest_addr self, uint64_t phys, uint64_t size) {
    bool found = false;
    pal_mutex_lock(m->lock);
    for (size_t i = 0; i < m->vmas.len && !found; i++) {
        const gmem_vma *v = &m->vmas.data[i];
        if (v->kind != GVMA_VIEW || v->reserved || v->start == self) continue;
        uint64_t vp = v->phys + (v->start - v->os_base);
        found = vp < phys + size && phys < vp + v->size;
    }
    pal_mutex_unlock(m->lock);
    return found;
}
uint8_t *gmem_phys_window(gmem *m) {
    pal_mutex_lock(m->lock);
    uint8_t *p = phys_window(m);
    pal_mutex_unlock(m->lock);
    return p;
}
/* 2 MiB blocks of demand views of 1 GiB or more known to be committed whole (map lock held):
 * host accesses skip VirtualQuery there, which walks the page tables to the end of a
 * run (slow on Dying Light The Beast's 14 GiB view, touched by every mutex call).
 * Any mapping change in a block clears it. */
#define DEMAND_BLOCK_BITS 21u
static uint8_t g_demand_done[(GUEST_SPACE_END >> DEMAND_BLOCK_BITS) / 8];
static bool demand_block_done(guest_addr a) {
    uint64_t b = a >> DEMAND_BLOCK_BITS;
    return a < GUEST_SPACE_END && (g_demand_done[b >> 3] & (1u << (b & 7)));
}
static void demand_block_set(const gmem_vma *v, guest_addr a) {
    guest_addr lo = a & ~((1ull << DEMAND_BLOCK_BITS) - 1), hi = lo + (1ull << DEMAND_BLOCK_BITS);
    if (v->size < (1ull << 30) || lo < v->start || hi > v->start + v->size || hi > GUEST_SPACE_END) return;
    uint64_t b = lo >> DEMAND_BLOCK_BITS;
    g_demand_done[b >> 3] |= (uint8_t)(1u << (b & 7));
}

static void changed(guest_addr addr, uint64_t size) {
    if (!size) return;
    for (uint64_t b = addr >> DEMAND_BLOCK_BITS; b <= (addr + size - 1) >> DEMAND_BLOCK_BITS &&
         b < (GUEST_SPACE_END >> DEMAND_BLOCK_BITS); b++) {
        if (!(b & 7) && b + 8 <= ((addr + size - 1) >> DEMAND_BLOCK_BITS) + 1) { g_demand_done[b >> 3] = 0; b += 7; continue; }
        g_demand_done[b >> 3] &= (uint8_t)~(1u << (b & 7));
    }
    if (g_change_hook) g_change_hook(addr, size);
}

/* Index of the first VMA whose end is above `addr` (VMAs are sorted and disjoint). */
static size_t lower_bound(const gmem *m, guest_addr addr) {
    size_t lo = 0, hi = m->vmas.len;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const gmem_vma *v = &m->vmas.data[mid];
        if (v->start + v->size <= addr) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* Read-only diagnostic, serialized with mapping mutations. No retained section
 * view or page protection changes: those would affect the mapping being traced.
 * CPU/GPU writes remain concurrent, so a change alone does not prove its writer. */
static void trace_memory_locked(gmem *m, const char *phase, guest_addr changed_addr, uint64_t changed_size) {
    struct gmem_history *h = m->history;
    if (!h || !h->trace_size) return;
    size_t i = lower_bound(m, h->trace_addr);
    const gmem_vma *v = i < m->vmas.len ? &m->vmas.data[i] : NULL;
    bool valid = v && v->start <= h->trace_addr && !v->reserved && !v->demand &&
        (v->prot & GPROT_READ) && h->trace_size <= v->start + v->size - h->trace_addr;
    uint8_t bytes[256] = {0};
    if (valid) memcpy(bytes, host_ptr(m, h->trace_addr), h->trace_size);
    if (h->trace_sampled && valid == h->trace_valid && (!valid || !memcmp(bytes, h->trace_bytes, h->trace_size))) return;
    unsigned nonzero = 0;
    for (uint32_t k = 0; valid && k < h->trace_size; k++) nonzero += bytes[k] != 0;
    LOG_I(MEM,"memory-trace %s 0x%llx+0x%llx T%llu: watched 0x%llx valid %u nonzero %u phys 0x%llx",
          phase,(unsigned long long)changed_addr,(unsigned long long)changed_size,
          (unsigned long long)pal_thread_current_id(),(unsigned long long)h->trace_addr,(unsigned)valid,nonzero,
          (unsigned long long)(valid && v->kind==GVMA_VIEW?v->phys+h->trace_addr-v->os_base:UINT64_MAX));
    if (valid) {
        char hex[sizeof(bytes) * 2 + 1];
        for(uint32_t k=0;k<h->trace_size;k++)snprintf(hex+k*2,3,"%02x",bytes[k]);
        LOG_I(MEM,"memory-trace bytes %s",hex);
    }
    h->trace_valid=valid;h->trace_sampled=true;
    memcpy(h->trace_bytes,bytes,sizeof(bytes));
}

static bool range_free(const gmem *m, guest_addr addr, uint64_t size) {
    size_t i = lower_bound(m, addr);
    return i == m->vmas.len || m->vmas.data[i].start >= addr + size;
}

static bool insert_vma(gmem *m, const gmem_vma *v) {
    size_t i = lower_bound(m, v->start);
    if (!dyn_reserve(&m->vmas, m->vmas.len + 1)) {
        return false;
    }
    memmove(&m->vmas.data[i + 1], &m->vmas.data[i], (m->vmas.len - i) * sizeof(gmem_vma));
    m->vmas.data[i] = *v;
    m->vmas.len++;
    return true;
}

/* A gap of `size` bytes in [lo, hi), from `from` on. */
static bool find_gap(const gmem *m, guest_addr from, uint64_t size, uint64_t align, guest_addr lo, guest_addr hi,
                     guest_addr *out) {
    guest_addr cand = PS5C_ALIGN_UP(PS5C_MAX(from, lo), align);
    /* Start at the VMA that may contain `cand`, not the next one. */
    size_t i = lower_bound(m, cand);
    if (i > 0 && m->vmas.data[i - 1].start + m->vmas.data[i - 1].size > cand) i--;
    for (; i <= m->vmas.len && cand < hi; i++) {
        guest_addr limit = i < m->vmas.len ? PS5C_MIN(m->vmas.data[i].start, hi) : hi;
        if (limit >= cand && limit - cand >= size) {
            *out = cand;
            return true;
        }
        if (i < m->vmas.len) {
            cand = PS5C_ALIGN_UP(PS5C_MAX(cand, m->vmas.data[i].start + m->vmas.data[i].size), align);
        }
    }
    return false;
}

/* ---- Placeholder area ----
 * Invariant: every maximal run of address space there that no memory backs
 * (gaps and reserved VMAs) is exactly one host placeholder. */

static bool backed(const gmem_vma *v) { return !v->reserved; }

/* The run of unbacked address space around [a, b) */
static void free_run(const gmem *m, guest_addr a, guest_addr b, guest_addr *r0, guest_addr *r1) {
    *r0 = m->ph_start;
    *r1 = m->res_end;
    size_t i = lower_bound(m, a);
    for (size_t k = i; k-- > 0;) {
        const gmem_vma *v = &m->vmas.data[k];
        if (v->start + v->size <= a && backed(v)) {
            *r0 = PS5C_MAX(v->start + v->size, m->ph_start);
            break;
        }
    }
    for (size_t k = i; k < m->vmas.len; k++) {
        const gmem_vma *v = &m->vmas.data[k];
        if (v->start >= b && backed(v)) {
            *r1 = v->start;
            break;
        }
    }
}

/* Makes [a, a + size) a placeholder of its own (inside one free run). */
static ps5c_err ph_carve(gmem *m, guest_addr a, uint64_t size) {
    guest_addr r0, r1;
    free_run(m, a, a + size, &r0, &r1);
    if (r0 == a && r1 == a + size) return PS5C_OK;
    ps5c_err err = pal_ph_split(host_ptr(m, a), size);
    if (err != PS5C_OK)
        LOG_E(MEM, "placeholder split 0x%llx+0x%llx (run 0x%llx-0x%llx) failed", (unsigned long long)a,
              (unsigned long long)size, (unsigned long long)r0, (unsigned long long)r1);
    return err;
}

/* Merges the free run around [a, b) back into one placeholder. */
static void ph_merge(gmem *m, guest_addr a, guest_addr b) {
    guest_addr r0, r1;
    free_run(m, a, b, &r0, &r1);
    pal_ph_coalesce(host_ptr(m, r0), r1 - r0); /* fails harmlessly when it is one placeholder already */
}

/* Backs the carved placeholder [a, a + size): a view of phys, or private memory. */
static ps5c_err ph_back(gmem *m, guest_addr a, uint64_t size, uint32_t prot, bool view, uint64_t phys, bool demand) {
    ps5c_err err = ph_carve(m, a, size);
    if (err != PS5C_OK) return err;
    err = view ? pal_ph_map_view(m->shm, phys, host_ptr(m, a), size, prot, demand) : pal_ph_alloc(host_ptr(m, a), size, prot);
    if (err != PS5C_OK) {
        if (err == PS5C_ERR_NOMEM) /* a large view is then tried again, committed as it is used */
            LOG_W(MEM, "%s 0x%llx+0x%llx: out of host memory", view ? "view" : "placeholder memory",
                  (unsigned long long)a, (unsigned long long)size);
        else
            LOG_E(MEM, "%s 0x%llx+0x%llx failed", view ? "view" : "placeholder memory", (unsigned long long)a,
                  (unsigned long long)size);
        ph_merge(m, a, a + size);
    }
    return err;
}

/* Private replacements can be split in place: never snapshot live neighboring
 * pages, since the native CPU writes them without taking the map lock. Views
 * still need a round trip, but their section keeps concurrent writes intact. */
static ps5c_err ph_release_unit(gmem *m, guest_addr os_base, guest_addr addr, guest_addr end) {
    trace_memory_locked(m,"before-unit-release",os_base,end-addr);
    /* the unit's VMAs: consecutive, inside [os_base, os_base + os_size) */
    size_t first = lower_bound(m, os_base), n = 0;
    while (first < m->vmas.len && m->vmas.data[first].os_base != os_base) first++;
    if (first == m->vmas.len) return PS5C_OK;
    uint64_t unit_end = os_base + m->vmas.data[first].os_size;
    while (first + n < m->vmas.len && m->vmas.data[first + n].start < unit_end) n++;
    if (!n) return PS5C_OK;
    gmem_vma *u = &m->vmas.data[first];
    bool view = u->kind == GVMA_VIEW;
    uint64_t os_size = u->os_size, phys = u->phys;
    if (!view) {
        guest_addr lo=PS5C_MAX(addr,os_base),hi=PS5C_MIN(end,unit_end);
        if (hi<=lo) return PS5C_OK;
        /* MEM_PRESERVE_PLACEHOLDER splits a private replacement atomically.
         * The two surviving allocations keep their bytes and protection;
         * update every VMA that belongs to either new host allocation. */
        ps5c_err err=pal_ph_free(host_ptr(m,lo),hi-lo);
        if (err!=PS5C_OK) return err; /* leave the mapping intact; no unsafe copy fallback */
        for(size_t k=0;k<n;k++) {
            gmem_vma *v=&m->vmas.data[first+k];
            if(v->start>=lo && v->start+v->size<=hi) {
                if(!v->reserved)m->mapped_bytes-=v->size;
                v->reserved=true;
            } else if(v->start<lo) {
                v->os_base=os_base;v->os_size=lo-os_base;
            } else {
                v->os_base=hi;v->os_size=unit_end-hi;
            }
        }
        changed(lo,hi-lo);
        trace_memory_locked(m,"after-private-split",lo,hi-lo);
        return PS5C_OK;
    }
    /* Guest threads stand still while the tracker's private pages are copied back and the view is
     * placed again (gmem_set_world_hooks): only when the tracker has pages in this view. */
    bool stopped = false;
    if (g_release_hook) { /* only views reach here: preserve the COW tracker's private pages */
        uint8_t *win = phys_window(m);
        stopped = g_world_stop && g_alias_hook && g_alias_hook(os_base, os_size) && g_world_stop(g_world_user);
        g_release_hook(os_base, os_size, win ? win + phys : NULL);
    }
    ps5c_err result=pal_ph_unmap_view(host_ptr(m,os_base));
    if(result!=PS5C_OK) {
        if (stopped) g_world_resume(g_world_user);
        return result;
    }
    for (size_t k = 0; k < n; k++) { /* unbacked for now */
        gmem_vma *v = &m->vmas.data[first + k];
        if (!v->reserved && v->start >= addr && v->start + v->size <= end) m->mapped_bytes -= v->size;
        v->reserved = true;
    }
    for (size_t k = 0; k < n; k++) {
        gmem_vma *v = &m->vmas.data[first + k];
        if (v->start >= addr && v->start + v->size <= end) continue;
        uint64_t vphys = phys + (v->start - os_base);
        /* Carving preceding pieces can leave this entire placeholder already
         * at its exact size. Windows may reject splitting it again. Attempt
         * replacement in either case: MapViewOfFile3/VirtualAlloc2 themselves
         * require an exact placeholder, so a real split failure is not hidden. */
        pal_ph_split(host_ptr(m, v->start), v->size);
        ps5c_err err = pal_ph_map_view(m->shm, vphys, host_ptr(m, v->start), v->size, v->prot, v->demand);
        if (err != PS5C_OK) {
            if (!stopped) /* no logging while the guest threads stand still (one may hold the log lock) */
                LOG_E(MEM,"remap of 0x%llx+0x%llx failed: %s",(unsigned long long)v->start,
                      (unsigned long long)v->size,ps5c_err_str(err));
            result=err;
            continue;
        }
        v->reserved = false;
        v->os_base = v->start;
        v->os_size = v->size;
        v->phys = vphys;
    }
    if (stopped) {
        g_world_resume(g_world_user);
        if (result != PS5C_OK)
            LOG_E(MEM, "remap of parts of 0x%llx+0x%llx failed: %s", (unsigned long long)os_base,
                  (unsigned long long)os_size, ps5c_err_str(result));
    }
    changed(os_base, os_size); /* the parts placed again have their own protection now */
    trace_memory_locked(m,"after-unit-release",os_base,os_size);
    return result;
}

ps5c_err gmem_map(gmem *m, guest_addr addr, uint64_t size, uint32_t prot, uint32_t flags,
                  const char *name, guest_addr *out) {
    return gmem_map_aligned(m, addr, size, GUEST_PAGE_SIZE, prot, flags, name, out);
}

static ps5c_err unmap_locked(gmem *m, guest_addr addr, uint64_t size);

static uint64_t flexible_used_locked(const gmem *m,guest_addr addr,uint64_t size) {
    uint64_t used=0;
    for(size_t i=0;i<m->vmas.len;i++) {
        const gmem_vma *v=&m->vmas.data[i];
        if (!v->flexible && !v->title_image) continue;
        if(!size)used+=v->size;
        else {
            guest_addr lo=PS5C_MAX(v->start,addr),hi=PS5C_MIN(v->start+v->size,addr+size);
            if(hi>lo)used+=hi-lo;
        }
    }
    return used;
}

uint64_t gmem_flexible_used(gmem *m) {
    pal_mutex_lock(m->lock);
    uint64_t used=flexible_used_locked(m,0,0);
    pal_mutex_unlock(m->lock);
    return used;
}

static bool flexible_fits_locked(const gmem *m,guest_addr addr,uint64_t size,uint32_t flags,uint64_t budget) {
    uint64_t used=flexible_used_locked(m,0,0);
    uint64_t replaced=(flags&(GMAP_FIXED|GMAP_REPLACE))==(GMAP_FIXED|GMAP_REPLACE)
        ?flexible_used_locked(m,addr,size):0;
    return used>=replaced && used-replaced<=budget && size<=budget-(used-replaced);
}

static ps5c_err replace_locked(gmem *m,guest_addr addr,uint64_t size,bool *started) {
    if(started)*started=true;
    return unmap_locked(m,addr,size);
}

static void cow_release_aliases(gmem *m, uint64_t phys, uint64_t size);

static ps5c_err map_common(gmem *m, guest_addr addr, uint64_t size, uint64_t align, uint32_t prot, uint32_t flags,
                           const char *name, bool view, uint64_t phys, guest_addr *out, bool *shared,uint64_t flexible_budget,bool *replacement_started) {
    if(replacement_started)*replacement_started=false;
    if (size == 0 || size > m->res_end - m->res_start) {
        return PS5C_ERR_INVALID;
    }
    if (align < GUEST_PAGE_SIZE) align = GUEST_PAGE_SIZE;
    if (align & (align - 1)) return PS5C_ERR_INVALID;
    size = PS5C_ALIGN_UP(size, GUEST_PAGE_SIZE);
    ps5c_err err = PS5C_OK;
    pal_mutex_lock(m->lock);
    /* The game's own reserve + map pairs (Minecraft: ~1-2k a second while a
     * world loads) waited for the GPU's pin at every draw, and it for them. */
    mapping_place_wait(m, !(flags & GMAP_FIXED), addr, size);
    if (view) cow_release_aliases(m, phys, size);

    bool have_ph = m->ph_start < m->res_end;
    guest_addr at = 0;
    if (flags & GMAP_FIXED) {
        if (addr % GUEST_PAGE_SIZE || addr < m->res_start || addr > m->res_end - size ||
            (addr < m->ph_start && addr + size > m->ph_start)) {
            err = PS5C_ERR_OUT_OF_RANGE;
        } else if (flexible_budget && !flexible_fits_locked(m,addr,size,flags,flexible_budget)) {
            err = PS5C_ERR_NOMEM; /* quota failure must leave the old mapping intact */
        } else if ((flags & GMAP_REPLACE) && (err = replace_locked(m, addr, size, replacement_started)) != PS5C_OK) {
            /* the old mapping could not go */
        } else if (!range_free(m, addr, size)) {
            err = PS5C_ERR_BUSY;
        } else {
            at = addr;
        }
    } else if (flexible_budget && !flexible_fits_locked(m,0,size,flags,flexible_budget)) {
        err = PS5C_ERR_NOMEM;
    } else if ((flags & GMAP_SHARED_AREA) && have_ph &&
               find_gap(m, addr >= m->ph_start ? addr : PS5C_MAX(m->ph_start, GUEST_PH_BASE),
                        size, align, m->ph_start, m->res_end, &at)) {
        /* Lowering the placeholder split enables fixed aliases; keep the
         * usual addresses of mappings whose address the emulator chooses. */
        /* placed in the shared-memory area (too big for it: the plain area below) */
    } else if (!find_gap(m, addr ? addr : m->alloc_hint, size, align, m->res_start, m->ph_start, &at) &&
               !find_gap(m, m->res_start, size, align, m->res_start, m->ph_start, &at)) {
        err = PS5C_ERR_NOMEM;
    }

    bool reserve_only = (flags & GMAP_RESERVE) != 0;
    bool ph = err == PS5C_OK && in_ph(m, at);
    gmem_vma v;
    memset(&v, 0, sizeof(v));
    if (err == PS5C_OK && !reserve_only) {
        bool can_demand = (flags & GMAP_DEMAND) && size >= (64ull << 20) && prot == GPROT_RW;
        bool demand = m->demand_anonymous && can_demand;
        if (ph) {
            v.demand = demand && view;
            err = ph_back(m, at, size, prot, view, phys, v.demand);
#ifdef _WIN32
            /* The host cannot commit all of it now (Dying Light The Beast maps 14 GiB of direct
             * memory at once; a 16 GB PC had ~11 GB of commit left): its pages are committed as
             * they are first touched instead, and so are the later large pools'.
             * PS5C_DEMAND_GPU=0 keeps the failure. */
            static int fallback = -1;
            if (fallback < 0) {
                const char *e = getenv("PS5C_DEMAND_GPU");
                fallback = !(e && !strcmp(e, "0"));
            }
            /* any large read/write view (not only graphics pools): without it the game stops here */
            if (err == PS5C_ERR_NOMEM && fallback && view && size >= (64ull << 20) && prot == GPROT_RW && !v.demand) {
                LOG_W(MEM, "view 0x%llx+0x%llx: not enough host memory to commit it at once, committed as it is used",
                      (unsigned long long)at, (unsigned long long)size);
                m->demand_anonymous = true;
                v.demand = true;
                err = ph_back(m, at, size, prot, view, phys, true);
            }
#endif
            v.kind = view ? GVMA_VIEW : GVMA_PRIVATE;
            v.os_base = at;
            v.os_size = size;
            v.phys = phys;
        } else {
            /* Explicitly selected private pools may leave unused pages reserved. */
            v.demand = demand && !view;
            if (!v.demand) err = pal_vm_commit(host_ptr(m, at), size, prot);
        }
    }
    if (err == PS5C_OK) {
        v.start = at;
        v.size = size;
        v.prot = reserve_only ? GPROT_NONE : prot;
        v.reserved = reserve_only;
        v.flexible = flexible_budget != 0;
        snprintf(v.name, sizeof(v.name), "%s", name ? name : "anon");
        if (!insert_vma(m, &v)) {
            if (!reserve_only && !ph) pal_vm_decommit(host_ptr(m, at), size);
            err = PS5C_ERR_NOMEM;
        } else {
            if (!reserve_only) m->mapped_bytes += size;
            *out = at;
            if (shared) *shared = ph && view;
        }
    }
    record_map_locked(m, view ? "map-view" : "map", err == PS5C_OK ? at : addr,
                      size, phys, prot, flags, err);
    if (err == PS5C_OK) changed(at, size);
    else if (flags & GMAP_FIXED) changed(addr, size);
    pal_mutex_unlock(m->lock);
    static int log_maps = -1;
    if (log_maps < 0) log_maps = getenv("PS5C_LOG_MAPS") != NULL;
    if (log_maps)
        LOG_I(MEM, "map%s 0x%llx+0x%llx (asked 0x%llx flags %u) prot %u%s phys 0x%llx '%s': %s", view ? " view" : "",
              (unsigned long long)at, (unsigned long long)size, (unsigned long long)addr, flags, prot,
              reserve_only ? " reserve" : "", (unsigned long long)phys, name ? name : "anon", ps5c_err_str(err));
    if (err == PS5C_OK) {
        LOG_D(MEM, "map 0x%llx-0x%llx prot %u '%s'%s", (unsigned long long)at, (unsigned long long)(at + size), prot,
              name ? name : "anon", ph ? (view ? " (view)" : " (placeholder)") : "");
    }
    return err;
}

ps5c_err gmem_map_aligned(gmem *m, guest_addr addr, uint64_t size, uint64_t align, uint32_t prot,
                          uint32_t flags, const char *name, guest_addr *out) {
    return map_common(m, addr, size, align, prot, flags, name, false, 0, out, NULL,0,NULL);
}

ps5c_err gmem_map_flexible(gmem *m,guest_addr addr,uint64_t size,uint64_t align,uint32_t prot,
                           uint32_t flags,uint64_t budget,const char *name,guest_addr *out,bool *replacement_started) {
    if(replacement_started)*replacement_started=false;
    if(!budget || (flags&GMAP_RESERVE))return PS5C_ERR_INVALID;
    return map_common(m,addr,size,align,prot,flags,name,false,0,out,NULL,budget,replacement_started);
}

/* Caller holds the map lock, before it has chosen where the new view goes (this may wait for
 * the pins). Views of [phys, phys + size) with pages of the copy-on-write tracker are mapped
 * again whole: a private copy would hide its writes from the view about to be made. */
static void cow_release_aliases(gmem *m, uint64_t phys, uint64_t size) {
    if (!g_alias_hook) return;
    for (;;) {
        guest_addr unit = 0;
        bool found = false;
        for (size_t i = 0; i < m->vmas.len && !found; i++) {
            const gmem_vma *v = &m->vmas.data[i];
            if (v->kind != GVMA_VIEW || v->reserved) continue;
            uint64_t vp = v->phys + (v->start - v->os_base);
            if (vp < phys + size && phys < vp + v->size && g_alias_hook(v->start, v->size)) unit = v->os_base, found = true;
        }
        if (!found) return;
        mapping_write_wait(m);
        if (ph_release_unit(m, unit, 0, 0) != PS5C_OK) return; /* the new view still goes ahead */
    }
}

ps5c_err gmem_map_direct(gmem *m, guest_addr addr, uint64_t size, uint64_t align, uint32_t prot, uint32_t flags,
                         uint64_t phys, const char *name, guest_addr *out, bool *shared) {
    *shared = false;
    if (phys % GUEST_PAGE_SIZE || phys + size > GUEST_SHM_SIZE) flags &= ~(uint32_t)GMAP_SHARED_AREA;
    bool view = m->shm && phys % GUEST_PAGE_SIZE == 0 && phys + size <= GUEST_SHM_SIZE;
    return map_common(m, addr, size, align, prot, flags & ~(uint32_t)GMAP_RESERVE, name, view, phys, out, shared,0,NULL);
}

/* Ensures a VMA boundary exists at `at` (splitting the VMA that contains it). */
static bool split_at(gmem *m, guest_addr at) {
    size_t i = lower_bound(m, at);
    if (i == m->vmas.len || m->vmas.data[i].start >= at) {
        return true; /* already a boundary, or unmapped */
    }
    gmem_vma tail = m->vmas.data[i];
    uint64_t head_size = at - tail.start;
    tail.start = at;
    tail.size -= head_size;
    if (!dyn_reserve(&m->vmas, m->vmas.len + 1)) {
        return false;
    }
    m->vmas.data[i].size = head_size;
    memmove(&m->vmas.data[i + 2], &m->vmas.data[i + 1], (m->vmas.len - i - 1) * sizeof(gmem_vma));
    m->vmas.data[i + 1] = tail;
    m->vmas.len++;
    return true;
}

static ps5c_err protect_range(gmem *m, guest_addr addr, uint64_t size, uint32_t prot, bool charge_image) {
    if (addr % GUEST_PAGE_SIZE) {
        return PS5C_ERR_INVALID;
    }
    size = PS5C_ALIGN_UP(size, GUEST_PAGE_SIZE);
    pal_mutex_lock(m->lock);
    mapping_write_wait(m);
    ps5c_err err = PS5C_OK;
    /* Whole range must be mapped with no holes. */
    guest_addr cur = addr;
    for (size_t i = lower_bound(m, addr); i < m->vmas.len && cur < addr + size; i++) {
        if (m->vmas.data[i].start > cur) break;
        cur = m->vmas.data[i].start + m->vmas.data[i].size;
    }
    if (cur < addr + size) {
        err = PS5C_ERR_OUT_OF_RANGE;
    } else if (!split_at(m, addr) || !split_at(m, addr + size)) {
        err = PS5C_ERR_NOMEM;
    } else {
        /* Reserved placeholders get memory when they become accessible
         * (PROT_NONE mapping semantics); the rest just changes protection. */
        for (size_t i = lower_bound(m, addr); err == PS5C_OK && i < m->vmas.len && m->vmas.data[i].start < addr + size;
             i++) {
            gmem_vma *v = &m->vmas.data[i];
            if (v->reserved) {
                if (prot == GPROT_NONE) continue;
                if (in_ph(m, v->start)) {
                    err = ph_back(m, v->start, v->size, prot, false, 0, false);
                    v = &m->vmas.data[i];
                    if (err == PS5C_OK) {
                        v->kind = GVMA_PRIVATE;
                        v->os_base = v->start;
                        v->os_size = v->size;
                    }
                } else {
                    err = pal_vm_commit(host_ptr(m, v->start), v->size, prot);
                }
                if (err == PS5C_OK) {
                    v->reserved = false;
                    m->mapped_bytes += v->size;
                }
            } else if (v->demand) {
                /* A shared section can gain committed pages through another
                 * alias. Materialize restricted ranges before protecting them:
                 * otherwise such pages could inherit the view's original RW
                 * protection without a fault on this alias. */
                if (prot != GPROT_RW) {
                    err = pal_vm_commit(host_ptr(m, v->start), v->size, prot);
                    if (err == PS5C_OK) err = pal_vm_protect(host_ptr(m, v->start), v->size, prot);
                } else {
                    /* VirtualProtect requires committed pages. Preserve the holes
                     * and apply the logical protection when each page is touched. */
                    for (uint64_t offset = 0; err == PS5C_OK && offset < v->size;) {
                        uint64_t run = 0;
                        bool committed = pal_vm_committed(host_ptr(m, v->start + offset), &run);
                        if (!run) { err = PS5C_ERR_OS; break; }
                        run = PS5C_MIN(run, v->size - offset);
                        if (committed) err = pal_vm_protect(host_ptr(m, v->start + offset), run, prot);
                        offset += run;
                    }
                }
            } else {
                err = pal_vm_protect(host_ptr(m, v->start), v->size, prot);
            }
            if (err == PS5C_OK) {
                v->prot = prot;
                v->title_image |= charge_image;
            }
        }
    }
    record_map_locked(m, "protect", addr, size, 0, prot, 0, err);
    changed(addr, size);
    pal_mutex_unlock(m->lock);
    return err;
}

ps5c_err gmem_protect(gmem *m, guest_addr addr, uint64_t size, uint32_t prot) {
    return protect_range(m, addr, size, prot, false);
}

ps5c_err gmem_protect_title_image(gmem *m, guest_addr addr, uint64_t size, uint32_t prot) {
    return protect_range(m, addr, size, prot, prot != GPROT_NONE);
}

static bool is_mapped_locked(const gmem *m, guest_addr addr, uint64_t size) {
    if (size == 0 || addr > UINT64_MAX - size) {
        return false;
    }
    guest_addr cur = addr;
    for (size_t i = lower_bound(m, addr); i < m->vmas.len; i++) {
        const gmem_vma *v = &m->vmas.data[i];
        if (v->start > cur || v->reserved) return false;
        cur = v->start + v->size;
        if (cur >= addr + size) return true;
    }
    return false;
}

bool gmem_is_mapped(gmem *m, guest_addr addr, uint64_t size) {
    pal_mutex_lock(m->lock);
    bool ok = is_mapped_locked(m, addr, size);
    pal_mutex_unlock(m->lock);
    return ok;
}

/* Host-side copies may run with the map lock held: commit their exact requested
 * ranges here rather than faulting recursively while memcpy owns that lock. */
static bool ensure_backing_locked(gmem *m, guest_addr addr, uint64_t size) {
    if (!m->demand_anonymous) return true;
    guest_addr end = addr + size;
    for (size_t i = lower_bound(m, addr); i < m->vmas.len && m->vmas.data[i].start < end; i++) {
        const gmem_vma *v = &m->vmas.data[i];
        if (!v->demand) continue;
        guest_addr a = PS5C_MAX(addr, v->start), b = PS5C_MIN(end, v->start + v->size);
        if (a < b) {
            if (v->size >= (1ull << 30)) { /* whole 2 MiB blocks, remembered */
                guest_addr x = a & ~((1ull << DEMAND_BLOCK_BITS) - 1);
                for (; x < b; x += 1ull << DEMAND_BLOCK_BITS) {
                    if (demand_block_done(x)) continue;
                    guest_addr lo = PS5C_MAX(x, v->start), hi = PS5C_MIN(x + (1ull << DEMAND_BLOCK_BITS), v->start + v->size);
                    if (pal_vm_commit(host_ptr(m, lo), hi - lo, v->prot) != PS5C_OK) return false;
                    demand_block_set(v, x);
                }
                continue;
            }
            uint64_t run;
            if (pal_vm_committed(host_ptr(m, a), &run) && run >= b - a) continue;
            if (pal_vm_commit(host_ptr(m, a), b - a, v->prot) != PS5C_OK) return false;
        }
    }
    return true;
}

/* Host pointer for an address known to be mapped (no lookup, no lock). */
void *gmem_host_unlocked(gmem *m, guest_addr addr) {
    return addr >= m->res_start && addr + 8 <= m->res_end ? host_ptr(m, addr) : NULL;
}

void *gmem_host(gmem *m, guest_addr addr, uint64_t size) {
    pal_mutex_lock(m->lock);
    bool ok = is_mapped_locked(m, addr, size) && ensure_backing_locked(m, addr, size);
    pal_mutex_unlock(m->lock);
    return ok ? host_ptr(m, addr) : NULL;
}

const gmem_vma *gmem_find_vma(gmem *m, guest_addr addr) {
    /* Pointer is valid until the next map/unmap/protect call. */
    pal_mutex_lock(m->lock);
    size_t i = lower_bound(m, addr);
    const gmem_vma *v = i < m->vmas.len && m->vmas.data[i].start <= addr ? &m->vmas.data[i] : NULL;
    pal_mutex_unlock(m->lock);
    return v;
}

bool gmem_query_vma(gmem *m, guest_addr addr, bool find_next, gmem_vma *out) {
    pal_mutex_lock(m->lock);
    size_t i = lower_bound(m, addr);
    bool found = i < m->vmas.len && (find_next || m->vmas.data[i].start <= addr);
    if (found) *out = m->vmas.data[i];
    pal_mutex_unlock(m->lock);
    return found;
}

bool gmem_commit_fault(gmem *m, guest_addr addr, uint32_t access) {
    if (!m->demand_anonymous || (access != GPROT_READ && access != GPROT_WRITE && access != GPROT_EXEC))
        return false;
    pal_mutex_lock(m->lock);
    size_t i = lower_bound(m, addr);
    const gmem_vma *v = i < m->vmas.len && m->vmas.data[i].start <= addr ? &m->vmas.data[i] : NULL;
    bool ok = v && v->demand && !v->reserved && (v->prot & access);
    if (ok) {
        /* Commit at most 64 KiB per fault, clipped to this VMA's protection.
         * Map/unmap/protect share the lock; concurrent first touches are safe.
         * Recommitting a page does not reset its contents. */
        /* 2 MiB at a time in views of 1 GiB or more: a heap the game fills at boot faults
         * 32 times less (Dying Light The Beast's 14 GiB view) */
        uint64_t unit = v->size >= (1ull << 30) ? (2ull << 20) : 0x10000ull;
        guest_addr block = addr & ~(unit - 1);
        guest_addr start = PS5C_MAX(block, v->start);
        guest_addr end = PS5C_MIN(block + unit, v->start + v->size);
        ok = pal_vm_commit(host_ptr(m, start), end - start, v->prot) == PS5C_OK;
        changed(start, end - start);
        if (ok) demand_block_set(v, start);
    }
    pal_mutex_unlock(m->lock);
    return ok;
}

bool gmem_retry_fault(gmem *m, guest_addr addr, bool write) {
    if (!in_ph(m, addr)) return false;
    pal_mutex_lock(m->lock); /* waits for a map/unmap in progress */
    size_t i = lower_bound(m, addr);
    const gmem_vma *v = i < m->vmas.len && m->vmas.data[i].start <= addr ? &m->vmas.data[i] : NULL;
    bool ok = v && !v->reserved && !v->demand && (v->prot & (write ? GPROT_WRITE : GPROT_READ));
    pal_mutex_unlock(m->lock);
    return ok;
}

void gmem_log_fault_state(gmem *m, guest_addr addr) {
    if (!m->history) return;
    /* The fault handler can run on a guest stack with a compromised heap.
     * Use the startup allocation; a concurrent/reentrant report must not
     * overwrite the snapshot while the first report is being printed. */
    gmem_event *events = NULL;
    gmem_vma nearby[3];
    size_t count = 0, nv = 0;
    uint64_t now = pal_time_ns(), total;
    bool contains = false;
    pal_mutex_lock(m->lock);
    size_t at = lower_bound(m, addr);
    contains = at < m->vmas.len && m->vmas.data[at].start <= addr;
    size_t first = at ? at - 1 : 0;
    for (size_t i = first; i < m->vmas.len && nv < PS5C_ARRAY_COUNT(nearby); i++) nearby[nv++] = m->vmas.data[i];
    total = m->history->count;
    uint64_t begin = total > GMEM_HISTORY_COUNT ? total - GMEM_HISTORY_COUNT : 0;
    if (!m->history->snapshot_busy) {
        m->history->snapshot_busy = true;
        events = m->history->snapshot;
    }
    if (events)
        for (uint64_t i = begin; i < total; i++) events[count++] = m->history->events[i % GMEM_HISTORY_COUNT];
    pal_mutex_unlock(m->lock);
    LOG_E(MEM, "fault-map 0x%llx: VMA %s, host thread %llu, latest %zu of %llu operations",
          (unsigned long long)addr, contains ? "present" : "absent", (unsigned long long)pal_thread_current_id(),
          count, (unsigned long long)total);
    for (size_t i = 0; i < nv; i++) {
        const gmem_vma *v = &nearby[i];
        LOG_E(MEM, "fault-map VMA 0x%llx+0x%llx prot %u reserved %u kind %u unit 0x%llx+0x%llx phys 0x%llx '%s'",
              (unsigned long long)v->start, (unsigned long long)v->size, v->prot, (unsigned)v->reserved,
              (unsigned)v->kind, (unsigned long long)v->os_base, (unsigned long long)v->os_size,
              (unsigned long long)v->phys, v->name);
    }
    if (!events) LOG_E(MEM, "fault-map history snapshot busy: another report is in progress");
    for (size_t i = 0; i < count; i++) {
        const gmem_event *e = &events[i];
        LOG_E(MEM, "fault-map #%llu age %.3fms T%llu %s 0x%llx+0x%llx phys 0x%llx prot %u flags %u result %d",
              (unsigned long long)e->sequence, (now >= e->time_ns ? now - e->time_ns : 0) / 1e6,
              (unsigned long long)e->thread, e->operation, (unsigned long long)e->addr,
              (unsigned long long)e->size, (unsigned long long)e->phys, e->prot, e->flags, (int)e->result);
    }
    if (events) {
        pal_mutex_lock(m->lock);
        m->history->snapshot_busy = false;
        pal_mutex_unlock(m->lock);
    }
}

bool gmem_read(gmem *m, guest_addr addr, void *dst, uint64_t size) {
    pal_mutex_lock(m->lock);
    bool ok = is_mapped_locked(m, addr, size) && ensure_backing_locked(m, addr, size);
    if (ok) memcpy(dst, host_ptr(m, addr), (size_t)size);
    pal_mutex_unlock(m->lock);
    return ok;
}

bool gmem_write(gmem *m, guest_addr addr, const void *src, uint64_t size) {
    pal_mutex_lock(m->lock);
    bool ok = is_mapped_locked(m, addr, size) && ensure_backing_locked(m, addr, size);
    if (ok) memcpy(host_ptr(m, addr), src, (size_t)size);
    pal_mutex_unlock(m->lock);
    return ok;
}

bool gmem_write_u64(gmem *m, guest_addr addr, uint64_t value) {
    uint8_t b[8];
    wr_le64(b, value);
    return gmem_write(m, addr, b, 8);
}

bool gmem_read_u64(gmem *m, guest_addr addr, uint64_t *value) {
    uint8_t b[8];
    if (!gmem_read(m, addr, b, 8)) return false;
    *value = rd_le64(b);
    return true;
}

bool gmem_compare_exchange_u64(gmem *m, guest_addr addr, uint64_t *expected,
                               uint64_t desired, bool *exchanged) {
    if (!m || !expected || !exchanged || (addr & 7) || addr > UINT64_MAX - 8) return false;
    pal_mutex_lock(m->lock);
    size_t i = lower_bound(m, addr);
    const gmem_vma *v = i < m->vmas.len ? &m->vmas.data[i] : NULL;
    bool ok = v && v->start <= addr && v->start + v->size >= addr + 8 && !v->reserved &&
              (v->prot & GPROT_RW) == GPROT_RW && ensure_backing_locked(m, addr, 8);
    if (ok)
        *exchanged = __atomic_compare_exchange_n((uint64_t *)host_ptr(m, addr), expected, desired,
                                                 false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    pal_mutex_unlock(m->lock);
    return ok;
}

static ps5c_err unmap_locked(gmem *m, guest_addr addr, uint64_t size);

ps5c_err gmem_unmap(gmem *m, guest_addr addr, uint64_t size) {
    size = PS5C_ALIGN_UP(size, GUEST_PAGE_SIZE);
    if (getenv("PS5C_LOG_MAPS")) LOG_I(MEM, "unmap 0x%llx+0x%llx", (unsigned long long)addr, (unsigned long long)size);
    if (addr % GUEST_PAGE_SIZE || size == 0 || addr < m->res_start || addr > m->res_end - size) {
        return PS5C_ERR_INVALID;
    }
    pal_mutex_lock(m->lock);
    mapping_write_wait(m);
    ps5c_err err = unmap_locked(m, addr, size);
    pal_mutex_unlock(m->lock);
    return err;
}

static ps5c_err unmap_locked(gmem *m, guest_addr addr, uint64_t size) {
    ps5c_err err = PS5C_OK;
    if (!split_at(m, addr) || !split_at(m, addr + size)) {
        err = PS5C_ERR_NOMEM;
    } else {
        /* placeholder area: each host mapping touched goes back to a
         * placeholder; the parts of it outside the range are placed again */
        bool touched_ph = false;
        for (size_t i = lower_bound(m, addr); i < m->vmas.len && m->vmas.data[i].start < addr + size; i++) {
            gmem_vma *v = &m->vmas.data[i];
            if (!in_ph(m, v->start) || !backed(v)) continue;
            err = ph_release_unit(m, v->os_base, addr, addr + size);
            if (err != PS5C_OK) goto done;
            touched_ph = true;
        }
        size_t first = lower_bound(m, addr), last = first;
        while (last < m->vmas.len && m->vmas.data[last].start < addr + size) {
            gmem_vma *v = &m->vmas.data[last];
            if (!v->reserved && !in_ph(m, v->start)) {
                pal_vm_decommit(host_ptr(m, v->start), v->size);
                m->mapped_bytes -= v->size;
            }
            last++;
        }
        memmove(&m->vmas.data[first], &m->vmas.data[last], (m->vmas.len - last) * sizeof(gmem_vma));
        m->vmas.len -= last - first;
        if (touched_ph) ph_merge(m, addr, addr + size);
    }
done:
    record_map_locked(m, "unmap", addr, size, 0, 0, 0, err);
    changed(addr, size);
    return err;
}
