/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
#include <stdatomic.h>

#include "loader/linker.h"
#include "loader/nid.h"
#include "mem/gmem.h"
#include "pal/pal.h"
#include "support/sce_builder.h"
#include "test.h"

static bool init_mem(gmem *m, bool identity) {
    gmem_config cfg = gmem_default_config();
    cfg.identity = identity;
    return gmem_init(m, &cfg) == PS5C_OK;
}

/* ---------------------------------------------------------------------------
 * gmem
 * ------------------------------------------------------------------------- */

static void test_gmem_map_and_access(void) {
    for (int identity = 0; identity < 2; identity++) {
        gmem m;
        CHECK(init_mem(&m, identity != 0));
        guest_addr a = 0;
        CHECK(gmem_map(&m, 0, 100, GPROT_RW, 0, "small", &a) == PS5C_OK);
        CHECK(a == GUEST_MODULE_BASE); /* anywhere allocations start at the hint */
        CHECK(a % GUEST_PAGE_SIZE == 0);
        CHECK(gmem_is_mapped(&m, a, GUEST_PAGE_SIZE));      /* rounded up to one page */
        CHECK(!gmem_is_mapped(&m, a, GUEST_PAGE_SIZE + 1));
        uint8_t *host = (uint8_t *)gmem_host(&m, a, 16);
        CHECK(host != NULL);
        if (identity) {
            CHECK((uintptr_t)host == a);
        }
        CHECK(host[0] == 0 && host[GUEST_PAGE_SIZE - 1] == 0); /* zero-filled */
        CHECK(gmem_write_u64(&m, a + 8, 0x1122334455667788ull));
        uint64_t v = 0;
        CHECK(gmem_read_u64(&m, a + 8, &v) && v == 0x1122334455667788ull);
        CHECK(gmem_host(&m, a + GUEST_PAGE_SIZE, 1) == NULL);
        CHECK(!gmem_write_u64(&m, a + GUEST_PAGE_SIZE - 4, 1)); /* crosses into unmapped */
        gmem_free(&m);
    }
}

static void test_gmem_fixed_and_overlap(void) {
    gmem m;
    CHECK(init_mem(&m, false));
    guest_addr a, b, c;
    guest_addr at = GUEST_MODULE_BASE + 16 * GUEST_PAGE_SIZE;
    CHECK(gmem_map(&m, at, 4 * GUEST_PAGE_SIZE, GPROT_RW, GMAP_FIXED, "fixed", &a) == PS5C_OK);
    CHECK(a == at);
    CHECK(gmem_map(&m, at + GUEST_PAGE_SIZE, GUEST_PAGE_SIZE, GPROT_RW, GMAP_FIXED, "x", &b) == PS5C_ERR_BUSY);
    CHECK(gmem_map(&m, at + 1, GUEST_PAGE_SIZE, GPROT_RW, GMAP_FIXED, "x", &b) == PS5C_ERR_OUT_OF_RANGE);
    CHECK(gmem_map(&m, 0x1000, GUEST_PAGE_SIZE, GPROT_RW, GMAP_FIXED, "x", &b) == PS5C_ERR_OUT_OF_RANGE);

    /* A 20-page allocation doesn't fit in the 16-page gap before `a`: it goes after. */
    CHECK(gmem_map(&m, 0, 20 * GUEST_PAGE_SIZE, GPROT_RW, 0, "big", &b) == PS5C_OK);
    CHECK(b == at + 4 * GUEST_PAGE_SIZE);
    /* A small one fills the gap. */
    CHECK(gmem_map(&m, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "gap", &c) == PS5C_OK);
    CHECK(c == GUEST_MODULE_BASE);
    CHECK(m.vmas.len == 3);
    CHECK(m.vmas.data[0].start == c && m.vmas.data[1].start == a && m.vmas.data[2].start == b);
    /* Adjacent VMAs form one contiguous mapped range. */
    CHECK(gmem_is_mapped(&m, a, 24 * GUEST_PAGE_SIZE));
    gmem_free(&m);
}

static void test_gmem_protect_split_unmap(void) {
    gmem m;
    CHECK(init_mem(&m, false));
    guest_addr a;
    CHECK(gmem_map(&m, 0, 8 * GUEST_PAGE_SIZE, GPROT_RW, 0, "img", &a) == PS5C_OK);
    CHECK(gmem_protect(&m, a + 2 * GUEST_PAGE_SIZE, 3 * GUEST_PAGE_SIZE, GPROT_READ) == PS5C_OK);
    CHECK(m.vmas.len == 3);
    CHECK(m.vmas.data[0].size == 2 * GUEST_PAGE_SIZE && m.vmas.data[0].prot == GPROT_RW);
    CHECK(m.vmas.data[1].size == 3 * GUEST_PAGE_SIZE && m.vmas.data[1].prot == GPROT_READ);
    CHECK(m.vmas.data[2].size == 3 * GUEST_PAGE_SIZE && m.vmas.data[2].prot == GPROT_RW);
    const gmem_vma *v = gmem_find_vma(&m, a + 3 * GUEST_PAGE_SIZE);
    CHECK(v && v->prot == GPROT_READ && strcmp(v->name, "img") == 0);
    uint64_t x;
    CHECK(gmem_read_u64(&m, a + 2 * GUEST_PAGE_SIZE, &x)); /* read-only is still readable */

    CHECK(gmem_protect(&m, a + 1, GUEST_PAGE_SIZE, GPROT_RW) == PS5C_ERR_INVALID);
    CHECK(gmem_protect(&m, a, 9 * GUEST_PAGE_SIZE, GPROT_RW) == PS5C_ERR_OUT_OF_RANGE);

    CHECK(gmem_unmap(&m, a + 1, GUEST_PAGE_SIZE) == PS5C_ERR_INVALID); /* unaligned */
    CHECK(gmem_unmap(&m, a + GUEST_PAGE_SIZE, GUEST_PAGE_SIZE) == PS5C_OK); /* hole in the first VMA */
    CHECK(m.vmas.len == 3 && gmem_is_mapped(&m, a, 1) && !gmem_is_mapped(&m, a + GUEST_PAGE_SIZE, 1));
    CHECK(m.mapped_bytes == 7 * GUEST_PAGE_SIZE);
    CHECK(gmem_unmap(&m, a, 8 * GUEST_PAGE_SIZE) == PS5C_OK); /* spans VMAs and the hole */
    CHECK(m.vmas.len == 0 && m.mapped_bytes == 0);
    CHECK(!gmem_is_mapped(&m, a, 1));
    /* The range is reusable. */
    guest_addr b;
    CHECK(gmem_map(&m, a, GUEST_PAGE_SIZE, GPROT_RW, GMAP_FIXED, "again", &b) == PS5C_OK);
    CHECK(((uint8_t *)gmem_host(&m, b, 1))[0] == 0);
    /* Aligned placement */
    guest_addr c;
    CHECK(gmem_map_aligned(&m, 0, GUEST_PAGE_SIZE, 0x200000, GPROT_RW, 0, "aligned", &c) == PS5C_OK);
    CHECK(c % 0x200000 == 0);
    CHECK(gmem_map_aligned(&m, 0, GUEST_PAGE_SIZE, 0x6000, GPROT_RW, 0, "bad", &c) == PS5C_ERR_INVALID);
    /* Reserved ranges: occupied but not accessible, and replaceable by a fixed map. */
    guest_addr r, f;
    uint64_t before = m.mapped_bytes;
    CHECK(gmem_map(&m, 0, 4 * GUEST_PAGE_SIZE, GPROT_RW, GMAP_RESERVE, "reserved", &r) == PS5C_OK);
    CHECK(m.mapped_bytes == before && !gmem_is_mapped(&m, r, 1) && gmem_find_vma(&m, r)->reserved);
    CHECK(gmem_map(&m, r, GUEST_PAGE_SIZE, GPROT_RW, GMAP_FIXED, "x", &f) == PS5C_ERR_BUSY);
    CHECK(gmem_unmap(&m, r + GUEST_PAGE_SIZE, GUEST_PAGE_SIZE) == PS5C_OK);
    CHECK(gmem_map(&m, r + GUEST_PAGE_SIZE, GUEST_PAGE_SIZE, GPROT_RW, GMAP_FIXED, "x", &f) == PS5C_OK);
    CHECK(gmem_is_mapped(&m, f, GUEST_PAGE_SIZE) && !gmem_is_mapped(&m, r, 1));
    CHECK(gmem_protect(&m, r, GUEST_PAGE_SIZE, GPROT_RW) == PS5C_OK); /* commits the placeholder */
    CHECK(gmem_is_mapped(&m, r, 2 * GUEST_PAGE_SIZE) && !gmem_find_vma(&m, r)->reserved);
    gmem_free(&m);
}

static void test_gmem_query_snapshot(void) {
    gmem m;
    CHECK(init_mem(&m, false));
    guest_addr first = GUEST_MODULE_BASE, next = first + 3 * GUEST_PAGE_SIZE, mapped;
    CHECK(gmem_map(&m, first, GUEST_PAGE_SIZE, GPROT_RW, GMAP_FIXED, "first", &mapped) == PS5C_OK);
    CHECK(gmem_map(&m, next, GUEST_PAGE_SIZE, GPROT_READ, GMAP_FIXED | GMAP_RESERVE, "next", &mapped) == PS5C_OK);
    gmem_vma snapshot;
    CHECK(gmem_query_vma(&m, first + 1, false, &snapshot));
    CHECK(snapshot.start == first && snapshot.size == GUEST_PAGE_SIZE && snapshot.prot == GPROT_RW);
    CHECK(strcmp(snapshot.name, "first") == 0 && !snapshot.reserved);
    CHECK(gmem_protect(&m, first, GUEST_PAGE_SIZE, GPROT_READ) == PS5C_OK);
    CHECK(gmem_unmap(&m, first, GUEST_PAGE_SIZE) == PS5C_OK);
    CHECK(snapshot.start == first && snapshot.prot == GPROT_RW && strcmp(snapshot.name, "first") == 0);
    CHECK(!gmem_query_vma(&m, first, false, &snapshot));
    CHECK(snapshot.start == first && snapshot.prot == GPROT_RW); /* failure does not change the copy */
    CHECK(gmem_query_vma(&m, first, true, &snapshot));
    CHECK(snapshot.start == next && snapshot.reserved);
    CHECK(gmem_query_vma(&m, next, false, &snapshot));
    CHECK(!gmem_query_vma(&m, next + GUEST_PAGE_SIZE, true, &snapshot));
    CHECK(snapshot.start == next);
    gmem_free(&m);
}

typedef struct query_churn {
    gmem *mem;
    guest_addr anchor;
    atomic_bool start;
    atomic_bool ready;
    atomic_bool queried;
} query_churn;

static int32_t churn_vmas(void *user) {
    query_churn *churn = user;
    while (!atomic_load(&churn->start)) pal_thread_yield();
    for (size_t cycle = 0; cycle < 8; cycle++) {
        for (size_t i = 0; i < 192; i++) {
            guest_addr addr = churn->anchor + (4 + 2 * i) * GUEST_PAGE_SIZE, mapped;
            if (gmem_map(churn->mem, addr, GUEST_PAGE_SIZE, GPROT_RW,
                         GMAP_FIXED | GMAP_RESERVE, "churn", &mapped) != PS5C_OK) {
                atomic_store(&churn->ready, true);
                return -1;
            }
            if (cycle == 0 && i == 0) {
                atomic_store(&churn->ready, true);
                while (!atomic_load(&churn->queried)) pal_thread_yield();
            }
            if (gmem_protect(churn->mem, churn->anchor, GUEST_PAGE_SIZE,
                             i & 1 ? GPROT_READ : GPROT_RW) != PS5C_OK) return -1;
        }
        for (size_t i = 0; i < 192; i++)
            if (gmem_unmap(churn->mem, churn->anchor + (4 + 2 * i) * GUEST_PAGE_SIZE, GUEST_PAGE_SIZE) != PS5C_OK) return -1;
    }
    return 0;
}

static void test_gmem_query_concurrent_churn(void) {
    gmem m;
    CHECK(init_mem(&m, false));
    guest_addr anchor;
    CHECK(gmem_map(&m, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "anchor", &anchor) == PS5C_OK);
    query_churn churn;
    churn.mem = &m;
    churn.anchor = anchor;
    atomic_init(&churn.start, false);
    atomic_init(&churn.ready, false);
    atomic_init(&churn.queried, false);
    pal_thread *thread;
    CHECK(pal_thread_create(&thread, churn_vmas, &churn, "vma-churn", 0) == PS5C_OK);
    atomic_store(&churn.start, true);
    while (!atomic_load(&churn.ready)) pal_thread_yield();
    bool valid = true;
    for (size_t i = 0; i < 20000; i++) {
        gmem_vma snapshot;
        bool found = gmem_query_vma(&m, anchor + 1, false, &snapshot);
        valid = valid && found && snapshot.start == anchor && snapshot.size == GUEST_PAGE_SIZE && !snapshot.reserved;
        valid = valid && (snapshot.prot == GPROT_READ || snapshot.prot == GPROT_RW);
        valid = valid && strcmp(snapshot.name, "anchor") == 0;
        if (gmem_query_vma(&m, anchor + GUEST_PAGE_SIZE, true, &snapshot)) {
            valid = valid && snapshot.start >= anchor + 4 * GUEST_PAGE_SIZE;
            valid = valid && snapshot.size == GUEST_PAGE_SIZE && snapshot.reserved && strcmp(snapshot.name, "churn") == 0;
        }
        atomic_store(&churn.queried, true);
    }
    int32_t result = -1;
    ps5c_err joined = pal_thread_join(thread, &result);
    CHECK(joined == PS5C_OK && result == 0);
    CHECK(valid);
    CHECK(m.vmas.len == 1);
    gmem_free(&m);
}

/* ---------------------------------------------------------------------------
 * Linker
 * ------------------------------------------------------------------------- */

typedef struct fault_capture {
    gmem *mem;
    guest_addr addr;
    uint64_t last_sequence;
    size_t events, vmas;
    bool advanced_history;
    bool nested_report;
    size_t busy_reports;
} fault_capture;

static void capture_fault(void *user, log_level level, const char *line, size_t len) {
    PS5C_UNUSED(level);
    PS5C_UNUSED(len);
    fault_capture *capture = user;
    if (strstr(line, "fault-map VMA ")) capture->vmas++;
    if (strstr(line, "fault-map history snapshot busy:")) capture->busy_reports++;
    const char *event = strstr(line, "fault-map #");
    if (!event) return;
    uint64_t sequence = strtoull(event + strlen("fault-map #"), NULL, 10);
    if (capture->events) CHECK(sequence == capture->last_sequence + 1);
    capture->last_sequence = sequence;
    capture->events++;
    if (!capture->advanced_history) {
        capture->advanced_history = true;
        /* Logging must release the map lock. Overwrite the ring while its
         * snapshot is being printed: remaining lines must stay unchanged. */
        for (size_t i = 0; i < 620; i++)
            CHECK(gmem_protect(capture->mem, capture->addr, GUEST_PAGE_SIZE, GPROT_RW) == PS5C_OK);
        /* A nested report still prints VMAs but cannot overwrite the active
         * snapshot. It must return without waiting for this callback. */
        capture->nested_report = true;
        gmem_log_fault_state(capture->mem, capture->addr);
    }
}

static void test_gmem_fault_snapshot(void) {
    gmem m;
    CHECK(init_mem(&m, false));
    guest_addr addr;
    CHECK(gmem_map(&m, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "fault-test", &addr) == PS5C_OK);
    for (size_t i = 0; i < 620; i++)
        CHECK(gmem_protect(&m, addr, GUEST_PAGE_SIZE, GPROT_RW) == PS5C_OK);
    fault_capture capture = {0};
    capture.mem = &m;
    capture.addr = addr;
    log_set_sink(capture_fault, &capture);
    gmem_log_fault_state(&m, addr);
    log_set_sink(NULL, NULL);
    if (m.history && PS5C_LOG_MIN_LEVEL <= LOG_LVL_ERROR) {
        CHECK(capture.vmas == 2 && capture.events == 512);
        CHECK(capture.last_sequence == 621 && capture.advanced_history);
        CHECK(capture.nested_report && capture.busy_reports == 1);
        /* Ownership is released after logging; the next report uses the new
         * ring contents rather than a permanently busy or stale snapshot. */
        capture = (fault_capture){0};
        capture.mem = &m;
        capture.addr = addr;
        capture.advanced_history = true;
        log_set_sink(capture_fault, &capture);
        gmem_log_fault_state(&m, addr);
        log_set_sink(NULL, NULL);
        CHECK(capture.vmas == 1 && capture.events == 512);
        CHECK(capture.last_sequence == 1241 && capture.busy_reports == 0);
    } else {
        CHECK(capture.vmas == 0 && capture.events == 0);
    }
    CHECK(gmem_write_u64(&m, addr, 1234));
    uint64_t value;
    CHECK(gmem_read_u64(&m, addr, &value) && value == 1234);
    gmem_free(&m);
}

typedef struct copy_race {
    gmem *mem;
    guest_addr addr;
    pal_mutex *lock;
    pal_cond *cond;
    unsigned phase;
} copy_race;

static int32_t remap_during_copies(void *arg) {
    copy_race *race = arg;
    ps5c_err first_unmap = gmem_unmap(race->mem, race->addr, 2 * GUEST_PAGE_SIZE);
    pal_mutex_lock(race->lock);
    race->phase = 1;
    pal_cond_signal(race->cond);
    while (race->phase == 1) pal_cond_wait(race->cond, race->lock);
    pal_mutex_unlock(race->lock);
    if (first_unmap != PS5C_OK) return 1;
    for (unsigned i = 0; i < 400; i++) {
        guest_addr mapped;
        if (gmem_map(race->mem, race->addr, 2 * GUEST_PAGE_SIZE, GPROT_RW, GMAP_FIXED,
                     "copy-race", &mapped) != PS5C_OK || mapped != race->addr) return 2;
        pal_thread_yield();
        if (gmem_unmap(race->mem, race->addr, 2 * GUEST_PAGE_SIZE) != PS5C_OK) return 3;
        pal_thread_yield();
    }
    return 0;
}

static void test_gmem_copy_with_remap(void) {
    /* Keep the host buffers separate from the guest region being remapped. */
    const size_t size = (size_t)(2 * GUEST_PAGE_SIZE);
    uint8_t *src = malloc(size), *dst = malloc(size);
    CHECK(src && dst);
    memset(src, 0xa5, size);
    for (unsigned identity = 0; identity < 2; identity++) {
        gmem m;
        CHECK(init_mem(&m, identity != 0));
        copy_race race = {0};
        race.mem = &m;
        CHECK(gmem_map(&m, 0, size, GPROT_RW, 0, "copy-race", &race.addr) == PS5C_OK);
        CHECK(gmem_write(&m, race.addr, src, size));
        CHECK(gmem_read(&m, race.addr, dst, size));
        CHECK(memcmp(src, dst, size) == 0);
        /* Invalid sizes/ranges do not modify either destination. */
        memset(dst, 0x3c, size);
        CHECK(!gmem_read(&m, race.addr, dst, 0));
        CHECK(!gmem_read(&m, UINT64_MAX - 7, dst, 16));
        CHECK(!gmem_read(&m, race.addr, dst, size + 1));
        CHECK(!gmem_write(&m, race.addr, src, 0));
        CHECK(!gmem_write(&m, UINT64_MAX - 7, src, 16));
        CHECK(!gmem_write(&m, race.addr, src, size + 1));
        for (size_t i = 0; i < size; i++) CHECK(dst[i] == 0x3c);
        CHECK(pal_mutex_create(&race.lock) == PS5C_OK);
        CHECK(pal_cond_create(&race.cond) == PS5C_OK);
        pal_thread *mapper;
        CHECK(pal_thread_create(&mapper, remap_during_copies, &race, "copy-remap", 0) == PS5C_OK);
        pal_mutex_lock(race.lock);
        while (race.phase != 1) pal_cond_wait(race.cond, race.lock);
        /* The first unmap is complete and the mapper waits: both operations
         * must fail without modifying the host destination. */
        bool gap_ok = !gmem_read(&m, race.addr, dst, size);
        gap_ok = !gmem_write(&m, race.addr, src, size) && gap_ok;
        for (size_t i = 0; i < size; i++) if (dst[i] != 0x3c) gap_ok = false;
        race.phase = 2;
        pal_cond_signal(race.cond);
        pal_mutex_unlock(race.lock);
        bool copies_ok = true;
        for (unsigned n = 0; n < 4000 && copies_ok; n++) {
            (void)gmem_write(&m, race.addr, src, size);
            if (gmem_read(&m, race.addr, dst, size)) {
                /* Each mapping begins zeroed; a successful host write copies
                 * the whole pattern. A read must not span two mappings. */
                copies_ok = dst[0] == 0 || dst[0] == 0xa5;
                for (size_t i = 1; i < size; i++) if (dst[i] != dst[0]) copies_ok = false;
            }
            pal_thread_yield();
        }
        int32_t mapper_result;
        CHECK(pal_thread_join(mapper, &mapper_result) == PS5C_OK);
        pal_cond_destroy(race.cond);
        pal_mutex_destroy(race.lock);
        bool unmapped = !gmem_is_mapped(&m, race.addr, size);
        gmem_free(&m);
        CHECK(mapper_result == 0 && gap_ok && copies_ok && unmapped);
    }
    free(src);
    free(dst);
}

enum { LIB_KERNEL = 1, LIB_VIDEOOUT = 2, LIB_NGS2 = 3 };

typedef struct link_fixture {
    uint8_t *game, *ngs2;
    size_t game_size, ngs2_size;
    module_set ms;
    gmem mem;
    linker lk;
    loaded_module *main_mod, *ngs2_mod;
    uint64_t slot_usleep, slot_guard, slot_vo, slot_create, slot_missing, slot_weak;
} link_fixture;

static bool fake_hle(void *user, const char *library, uint64_t nid) {
    PS5C_UNUSED(user);
    return strcmp(library, "libkernel") == 0 && nid == nid_hash_name("sceKernelUsleep");
}

static bool link_setup(link_fixture *f, bool ps5_layout) {
    memset(f, 0, sizeof(*f));
    sce_builder b;
    sb_init(&b, ET_SCE_DYNEXEC);
    b.with_tls = true;
    b.ps5_layout = ps5_layout;
    sb_add_needed(&b, "libSceNgs2", LIB_NGS2);
    sb_add_import_lib(&b, "libkernel", LIB_KERNEL);
    sb_add_import_lib(&b, "libSceVideoOut", LIB_VIDEOOUT);
    sb_add_import_lib(&b, "libSceNgs2", LIB_NGS2);
    f->slot_usleep = sb_import(&b, "sceKernelUsleep", LIB_KERNEL, LIB_KERNEL, STT_FUNC);
    f->slot_guard = sb_import(&b, "__stack_chk_guard", LIB_KERNEL, LIB_KERNEL, STT_OBJECT);
    f->slot_vo = sb_import(&b, "sceVideoOutOpen", LIB_VIDEOOUT, LIB_VIDEOOUT, STT_FUNC);
    f->slot_create = sb_import(&b, "sceNgs2SystemCreate", LIB_NGS2, LIB_NGS2, STT_FUNC);
    f->slot_missing = sb_import(&b, "sceNgs2NotExported", LIB_NGS2, LIB_NGS2, STT_FUNC);
    sb_relative(&b, 0x900, 0x100);
    memset(b.seg + 0x200, 0x5A, 0x10); /* recognizable data */
    f->game = sb_build(&b, &f->game_size);

    sb_init(&b, ET_SCE_DYNAMIC);
    b.ps5_layout = ps5_layout;
    sb_set_module_info(&b, "libSceNgs2", 1);
    sb_add_export_lib(&b, "libSceNgs2", 1);
    sb_export(&b, "sceNgs2SystemCreate", 1, 1, 0x100);
    /* A weak data import nobody provides must become 0. */
    sb_add_import_lib(&b, "libSceOptional", 2);
    f->slot_weak = sb_import(&b, "sceOptionalData", 2, 2, STT_OBJECT);
    b.syms[b.sym_count - 1].info = ELF64_ST_INFO(STB_WEAK, STT_OBJECT);
    f->ngs2 = sb_build(&b, &f->ngs2_size);

    module_set_init(&f->ms);
    f->ms.hle_query = fake_hle;
    return f->game && f->ngs2 && init_mem(&f->mem, true) &&
           module_set_add_buffer(&f->ms, "eboot.bin", f->game, f->game_size, true, &f->main_mod) == PS5C_OK &&
           module_set_add_buffer(&f->ms, "libSceNgs2.sprx", f->ngs2, f->ngs2_size, false, &f->ngs2_mod) == PS5C_OK &&
           linker_init(&f->lk, &f->mem, &f->ms) == PS5C_OK;
}

static void link_teardown(link_fixture *f) {
    linker_free(&f->lk);
    gmem_free(&f->mem);
    module_set_free(&f->ms);
    free(f->game);
    free(f->ngs2);
}

static void check_linked(link_fixture *f) {
    CHECK(linker_link_all(&f->lk) == PS5C_OK);
    loaded_module *m = f->main_mod, *n = f->ngs2_mod;
    CHECK(m->mapped && n->mapped);
    CHECK(m->base == GUEST_MODULE_BASE + 2 * IMPORT_CODE_AREA_SIZE ||  /* after the import areas */
          m->base >= GUEST_MODULE_BASE);
    CHECK(n->base > m->base + m->map_size); /* guard gap between modules */
    CHECK(m->base % GUEST_PAGE_SIZE == 0 && n->base % 0x100000 == 0);

    /* Segment contents and .bss */
    const uint8_t *code = (const uint8_t *)gmem_host(&f->mem, m->bias + 0x100, 6);
    CHECK(code && memcmp(code, "\xB8\x2A\x00\x00\x00\xC3", 6) == 0);
    const uint8_t *data = (const uint8_t *)gmem_host(&f->mem, m->bias + 0x200, 16);
    CHECK(data && data[0] == 0x5A && data[15] == 0x5A);
    const uint8_t *bss = (const uint8_t *)gmem_host(&f->mem, m->bias + SB_SEG_SIZE + 0x10, 1);
    CHECK(bss && bss[0] == 0);

    uint64_t v;
    /* RELATIVE */
    CHECK(gmem_read_u64(&f->mem, m->bias + 0x900, &v) && v == m->bias + 0x100);
    /* LLE import -> the exporting module */
    CHECK(gmem_read_u64(&f->mem, m->bias + f->slot_create, &v) && v == n->bias + 0x100);
    /* HLE and missing imports -> distinct stubs in the stub area */
    uint64_t usleep, vo, missing, guard;
    CHECK(gmem_read_u64(&f->mem, m->bias + f->slot_usleep, &usleep));
    CHECK(gmem_read_u64(&f->mem, m->bias + f->slot_vo, &vo));
    CHECK(gmem_read_u64(&f->mem, m->bias + f->slot_missing, &missing));
    CHECK(gmem_read_u64(&f->mem, m->bias + f->slot_guard, &guard));
    CHECK(usleep >= f->lk.import_code && usleep < f->lk.import_code + IMPORT_CODE_AREA_SIZE);
    CHECK(usleep != vo && vo != missing && usleep % IMPORT_STUB_SIZE == 0);
    const import_stub *st = linker_find_stub(&f->lk, usleep);
    CHECK(st && st->kind == PROVIDER_HLE && strcmp(st->library, "libkernel") == 0);
    st = linker_find_stub(&f->lk, vo);
    CHECK(st && st->kind == PROVIDER_MISSING && st->nid == nid_hash_name("sceVideoOutOpen"));
    /* Stubs trap until the CPU backend fills them in */
    const uint8_t *stub_code = (const uint8_t *)gmem_host(&f->mem, vo, IMPORT_STUB_SIZE);
    CHECK(stub_code && stub_code[0] == 0xCC && stub_code[IMPORT_STUB_SIZE - 1] == 0xCC);
    /* Data import -> zeroed data slot */
    CHECK(guard >= f->lk.import_data && guard < f->lk.import_data + IMPORT_DATA_AREA_SIZE);
    CHECK(gmem_read_u64(&f->mem, guard, &v) && v == 0);
    /* Weak data import without provider -> 0 */
    CHECK(gmem_read_u64(&f->mem, n->bias + f->slot_weak, &v) && v == 0);
    CHECK(f->lk.stats.weak_zero == 1);

    /* TLS: main executable is module 1, block of 0x40 bytes aligned to 16 */
    CHECK(m->tls_index == 1 && m->tls_offset == 0x40 && n->tls_index == 0);
    CHECK(f->lk.stats.static_tls_size == 0x40);

    CHECK(f->lk.stats.relocs_unsupported == 0);
    CHECK(f->lk.stats.binds_lle == 1 && f->lk.stats.binds_hle == 1 && f->lk.stats.binds_missing == 3);

    /* Final protection from the segment flags (builder code segment is RWX) */
    const gmem_vma *vma = gmem_find_vma(&f->mem, m->bias + 0x100);
    CHECK(vma && vma->prot == (GPROT_RW | GPROT_EXEC));
}

static void test_link_ps4_layout(void) {
    link_fixture f;
    CHECK(link_setup(&f, false));
    check_linked(&f);
    link_teardown(&f);
}

static void test_link_ps5_layout(void) {
    link_fixture f;
    CHECK(link_setup(&f, true));
    check_linked(&f);
    /* The tables segment (read-only) is mapped as part of the image */
    const gmem_vma *vma = gmem_find_vma(&f.mem, f.main_mod->bias + 0x10000);
    CHECK(vma && vma->prot == GPROT_READ);
    link_teardown(&f);
}

static void test_link_rejects_reloc_outside_module(void) {
    sce_builder b;
    sb_init(&b, ET_SCE_DYNEXEC);
    sb_relative(&b, 0x40000000, 0); /* far outside the image */
    size_t size;
    uint8_t *file = sb_build(&b, &size);
    module_set ms;
    module_set_init(&ms);
    gmem mem;
    linker lk;
    CHECK(init_mem(&mem, false));
    CHECK(module_set_add_buffer(&ms, "bad", file, size, true, NULL) == PS5C_OK);
    CHECK(linker_init(&lk, &mem, &ms) == PS5C_OK);
    CHECK(linker_link_all(&lk) == PS5C_ERR_INVALID);
    linker_free(&lk);
    gmem_free(&mem);
    module_set_free(&ms);
    free(file);
}

int main(void) {
    pal_init();
    log_set_level_all(LOG_LVL_ERROR);
    static const test_case tests[] = {
        TEST_CASE(test_gmem_map_and_access),
        TEST_CASE(test_gmem_fixed_and_overlap),
        TEST_CASE(test_gmem_protect_split_unmap),
        TEST_CASE(test_gmem_query_snapshot),
        TEST_CASE(test_gmem_query_concurrent_churn),
        TEST_CASE(test_gmem_fault_snapshot),
        TEST_CASE(test_gmem_copy_with_remap),
        TEST_CASE(test_link_ps4_layout),
        TEST_CASE(test_link_ps5_layout),
        TEST_CASE(test_link_rejects_reloc_outside_module),
    };
    return test_run_all("mem", tests, PS5C_ARRAY_COUNT(tests));
}
