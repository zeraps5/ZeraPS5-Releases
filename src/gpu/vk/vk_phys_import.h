/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_VK_PHYS_IMPORT_H
#define PS5C_VK_PHYS_IMPORT_H
/* Read-only buffers straight from the game's direct memory, with no copy into the arena
 * (PS5C_PHYS_IMPORT=1, step 5a of docs/plano-vulkan.md).
 *
 * gmem_phys_window is one host view of the whole physical memory object. Blocks of it are
 * imported once as Vulkan buffers (VK_EXT_external_memory_host) and kept: views of the same
 * physical pages at several guest addresses (Minecraft's vertex pool) land in the same block,
 * and a guest unmap does not take the pages away (the object outlives every view).
 *
 * The GPU reads the memory when the batch runs, not when the draw is recorded: what the game
 * writes in between is seen. The caller keeps the copy for stores, coherent or windowed reads,
 * and ranges that results of this batch still owe (writebacks). Off in copy-on-write tracking
 * (PS5C_GPU_WATCH=cow): a written page lives in a private copy there until it is released. */

#define PHYS_IMPORT_BLOCK (2ull << 20)
#define PHYS_IMPORT_MAX 512u

typedef struct phys_import {
    uint64_t lo, hi; /* physical offsets [lo, hi) */
    vk_buffer buffer;
    bool failed;     /* importing failed: the copy serves this range */
} phys_import;

static struct {
    int on;           /* -1 unread */
    uint8_t *window;
    uint64_t limit, pinned;
    phys_import entries[PHYS_IMPORT_MAX];
    uint32_t count, last;
    uint64_t hits, bytes, shifted, misaligned, outside, failed, full; /* per second (stats) */
} g_pimp = {.on = -1};

static bool phys_import_enabled(renderer *r) {
    if (g_pimp.on < 0) {
        const char *e = getenv("PS5C_PHYS_IMPORT"), *w = getenv("PS5C_GPU_WATCH");
        g_pimp.on = e && !strcmp(e, "1") && !(w && !strcmp(w, "cow")) && r->host_import_align &&
                    r->host_import_align <= PHYS_IMPORT_BLOCK && r->mem;
        if (g_pimp.on) g_pimp.window = gmem_phys_window(r->mem);
        if (!g_pimp.window) g_pimp.on = 0;
        const char *m = getenv("PS5C_PHYS_IMPORT_MAX_MB");
        g_pimp.limit = (m ? strtoull(m, NULL, 10) : 1024) << 20;
        if (e) LOG_I(GPU, "direct memory reads without copies: %s (up to %llu MiB imported)",
                     g_pimp.on ? "on (experimental)" : "unavailable", (unsigned long long)(g_pimp.limit >> 20));
    }
    return g_pimp.on > 0;
}

/* Bytes a shifted site's binding starts before its data (SH_BUFFER_SHIFTED), per site of the
 * stage being bound: added to the vertex-share distance in the stage's uniforms. */
static uint32_t g_pimp_extra[SH_MAX_SITES];

/* The imported buffer and offset that show guest [addr, addr + range), or VK_NULL_HANDLE.
 * shiftable: the shader adds a distance to its offsets, so a binding may start up to
 * align - 4 bytes before addr (*extra); otherwise addr must sit on the alignment. */
/* The VMAs binds found lately, valid while the mapping generation stays (every map, unmap and
 * protect moves it under the map lock): Minecraft binds ~30k ranges a second, and the locked
 * query of each one contended with the game's threads (28% of the GPU thread in its loading). */
#define PIMP_VMA_CACHE 8
static struct { uint64_t gen; uint32_t next; gmem_vma v[PIMP_VMA_CACHE]; bool ok[PIMP_VMA_CACHE]; } g_pimp_vmas;
static bool phys_import_vma(renderer *r, guest_addr addr, gmem_vma *out) {
    uint64_t gen = __atomic_load_n(&r->mem->mapping_generation, __ATOMIC_ACQUIRE);
    if (gen != g_pimp_vmas.gen) {
        memset(g_pimp_vmas.ok, 0, sizeof(g_pimp_vmas.ok));
        g_pimp_vmas.gen = gen;
    }
    for (uint32_t i = 0; i < PIMP_VMA_CACHE; i++)
        if (g_pimp_vmas.ok[i] && addr >= g_pimp_vmas.v[i].start && addr - g_pimp_vmas.v[i].start < g_pimp_vmas.v[i].size) {
            *out = g_pimp_vmas.v[i];
            return true;
        }
    if (!gmem_query_vma(r->mem, addr, false, out)) return false;
    if (addr < out->start || addr - out->start >= out->size) return true; /* a hole: not kept */
    uint32_t k = g_pimp_vmas.next++ % PIMP_VMA_CACHE;
    g_pimp_vmas.v[k] = *out;
    g_pimp_vmas.ok[k] = true;
    return true;
}

static VkBuffer phys_import_bind(renderer *r, guest_addr addr, uint64_t range, VkDeviceSize align, bool shiftable,
                                 VkDeviceSize *offset, uint32_t *extra) {
    *extra = 0;
    gmem_vma v;
    if (!phys_import_vma(r, addr, &v) || v.kind != GVMA_VIEW || v.reserved || v.demand ||
        !(v.prot & GPROT_READ) || addr < v.start || addr + range > v.start + v.size) {
        static uint32_t told;
        if (getenv("PS5C_PHYS_IMPORT_WHY") && told++ < 24)
            LOG_I(GPU, "direct memory import outside a view: 0x%llx + %llu in [0x%llx + %llu] kind %u%s%s%s prot %x '%s'",
                  (unsigned long long)addr, (unsigned long long)range, (unsigned long long)v.start,
                  (unsigned long long)v.size, v.kind, v.reserved ? " reserved" : "", v.demand ? " demand" : "",
                  v.flexible ? " flexible" : "", v.prot, v.name);
        g_pimp.outside++;
        return VK_NULL_HANDLE;
    }
    uint64_t phys = v.phys + (addr - v.os_base), end = phys + range;
    if (end > GUEST_SHM_SIZE) { g_pimp.outside++; return VK_NULL_HANDLE; }
    phys_import *e = NULL;
    if (g_pimp.last < g_pimp.count) {
        phys_import *l = &g_pimp.entries[g_pimp.last];
        if (phys >= l->lo && end <= l->hi) e = l;
    }
    for (uint32_t i = 0; !e && i < g_pimp.count; i++)
        if (phys >= g_pimp.entries[i].lo && end <= g_pimp.entries[i].hi) { e = &g_pimp.entries[i]; g_pimp.last = i; }
    if (!e) {
        uint64_t lo = phys & ~(PHYS_IMPORT_BLOCK - 1), hi = (end + PHYS_IMPORT_BLOCK - 1) & ~(PHYS_IMPORT_BLOCK - 1);
        if (g_pimp.count == PHYS_IMPORT_MAX || g_pimp.pinned + (hi - lo) > g_pimp.limit) { g_pimp.full++; return VK_NULL_HANDLE; }
        e = &g_pimp.entries[g_pimp.count];
        memset(e, 0, sizeof(*e));
        e->lo = lo; e->hi = hi;
        e->failed = !vk_buffer_import_host(r, g_pimp.window + lo, hi - lo, &e->buffer);
        g_pimp.last = g_pimp.count++;
        if (!e->failed) g_pimp.pinned += hi - lo;
        static uint32_t logged;
        if (logged++ < 8 || e->failed)
            LOG_I(GPU, "direct memory import: physical 0x%llx + %llu KiB %s (%llu MiB in all)", (unsigned long long)lo,
                  (unsigned long long)((hi - lo) >> 10), e->failed ? "failed, copies stay" : "imported",
                  (unsigned long long)(g_pimp.pinned >> 20));
    }
    if (e->failed) { g_pimp.failed++; return VK_NULL_HANDLE; }
    uint64_t rem = (phys - e->lo) % align;
    if (rem && (!shiftable || (rem & 3))) { g_pimp.misaligned++; return VK_NULL_HANDLE; }
    *offset = phys - e->lo - rem;
    *extra = (uint32_t)rem;
    if (rem) g_pimp.shifted++;
    g_pimp.hits++;
    g_pimp.bytes += range;
    return e->buffer.buffer;
}

static uint64_t g_pimp_mis[64];
static void phys_import_stats(void) {
    if (g_pimp.on <= 0) return;
    char ml[400]; int mn = 0;
    for (int k = 0; k < 64 && mn < 360; k++)
        if (g_pimp_mis[k]) mn += snprintf(ml + mn, sizeof(ml) - (size_t)mn, " st%d/k%d/e%d %llu", k / 8, (k / 2) & 3, k & 1, (unsigned long long)g_pimp_mis[k]);
    if (mn) LOG_I(GPU, "direct memory reads misaligned by stage/kind/eligible:%s", ml);
    memset(g_pimp_mis, 0, sizeof(g_pimp_mis));
    LOG_I(GPU, "direct memory reads: %llu bound (%llu shifted), %.1f MiB not copied; copied instead: %llu misaligned, %llu outside views, %llu failed, %llu over the limit",
          (unsigned long long)g_pimp.hits, (unsigned long long)g_pimp.shifted, g_pimp.bytes / 1048576.0, (unsigned long long)g_pimp.misaligned,
          (unsigned long long)g_pimp.outside, (unsigned long long)g_pimp.failed, (unsigned long long)g_pimp.full);
    g_pimp.hits = g_pimp.bytes = g_pimp.shifted = g_pimp.misaligned = g_pimp.outside = g_pimp.failed = g_pimp.full = 0;
}

static void phys_import_drop(renderer *r) {
    for (uint32_t i = 0; i < g_pimp.count; i++)
        if (!g_pimp.entries[i].failed) vk_buffer_destroy(r, &g_pimp.entries[i].buffer);
    g_pimp.count = 0;
    g_pimp.pinned = 0;
}

#endif
