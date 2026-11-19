/* SPDX-License-Identifier: GPL-2.0-or-later */
/* gpu_watch: page write tracking with real protection and faults. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu/gpu_watch.h"
#include "pal/pal.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static gmem *_Atomic g_retry_mem; /* like the runtime: a write the mapping allows is retried */
static bool on_fault(void *user, pal_fault_info *info) {
    (void)user;
    if (info->kind != PAL_FAULT_ACCESS) return false;
    if (gpu_watch_fault(info->addr, info->write)) return true;
    gmem_vma v; /* gmem_retry_fault, which only covers the placeholder area */
    return g_retry_mem && gmem_query_vma(g_retry_mem, info->addr, false, &v) && v.start <= info->addr &&
           !v.reserved && (v.prot & GPROT_WRITE);
}

static void prepare(void *buf, uint64_t size) { gpu_watch_prepare_write((guest_addr)(uintptr_t)buf, size); }

static guest_addr g_saved[8];
static uint32_t g_saved_count;
static uint8_t g_saved_byte;
static void saver(guest_addr page) {
    if (g_saved_count < 8) g_saved[g_saved_count++] = page;
    if (g_saved_count == 1) g_saved_byte = ((const uint8_t *)(uintptr_t)page)[9];
}

#define PAGE 4096u
#define PAGES 16u

typedef struct writer {
    volatile uint8_t *base;
    uint32_t page;
    atomic_bool *stop;
    uint64_t writes;
} writer;

static int32_t write_loop(void *arg) {
    writer *w = (writer *)arg;
    while (!atomic_load(w->stop)) {
        w->base[w->page * PAGE + (w->writes % PAGE)] = (uint8_t)w->writes;
        w->writes++;
    }
    return 0;
}

int main(void) {
#ifdef _WIN32
    _putenv("PS5C_GPU_WATCH=1");
#else
    setenv("PS5C_GPU_WATCH", "1", 1);
#endif
    CHECK(pal_init() == PS5C_OK);
    CHECK(gpu_watch_enabled());
    CHECK(pal_fault_handler_install(on_fault, NULL) == PS5C_OK);
    pal_set_write_prepare(prepare);
    gmem m;
    gmem_config cfg = {.start = 0x7f00000000ull, .size = 0x100000, .identity = true};
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    gmem_set_change_hook(gpu_watch_mapping_changed);
    guest_addr a = 0, ro = 0;
    CHECK(gmem_map(&m, cfg.start, PAGES * PAGE, GPROT_RW, GMAP_FIXED, "watched", &a) == PS5C_OK);
    CHECK(gmem_map(&m, cfg.start + 0x80000, PAGES * PAGE, GPROT_READ, GMAP_FIXED, "read-only", &ro) == PS5C_OK);
    void *mem = (void *)(uintptr_t)a;
    volatile uint8_t *p = (volatile uint8_t *)mem;

    /* not started: nothing is armed */
    CHECK(!gpu_watch_arm(GPU_WATCH_TEXTURES, a, PAGE));
    gpu_watch_start(&m);
    /* read-only and unmapped memory is never armed */
    CHECK(!gpu_watch_arm(GPU_WATCH_TEXTURES, ro, PAGE));
    CHECK(!gpu_watch_arm(GPU_WATCH_TEXTURES, a + PAGES * PAGE, PAGE));
    CHECK(!gpu_watch_arm(GPU_WATCH_TEXTURES, a + PAGES * PAGE - PAGE, 2 * PAGE));

    /* never armed: written for every client */
    CHECK(gpu_watch_written(GPU_WATCH_TEXTURES, a, PAGE));
    /* armed: clean until a write; other clients unaffected */
    CHECK(gpu_watch_arm(GPU_WATCH_TEXTURES, a, 4 * PAGE));
    CHECK(!gpu_watch_written(GPU_WATCH_TEXTURES, a, 4 * PAGE));
    CHECK(gpu_watch_written(GPU_WATCH_BUFFERS, a, 4 * PAGE));
    CHECK(p[100] == 0); /* reads do not fault */
    p[PAGE + 5] = 0x5a; /* faults once, retried */
    CHECK(p[PAGE + 5] == 0x5a);
    CHECK(gpu_watch_written(GPU_WATCH_TEXTURES, a + PAGE, 1));
    CHECK(!gpu_watch_written(GPU_WATCH_TEXTURES, a, PAGE));
    /* the fault released the protected pages after it too (bulk writes go on there) */
    CHECK(gpu_watch_written(GPU_WATCH_TEXTURES, a + 2 * PAGE, PAGE) && gpu_watch_written(GPU_WATCH_TEXTURES, a + 3 * PAGE, PAGE));
    p[3 * PAGE] = 1; /* no second fault */
    p[PAGE + 6] = 0x5b; /* writable now: no fault */
    uint64_t faults, protected_pages;
    gpu_watch_counts(&faults, &protected_pages);
    CHECK(faults == 1 && protected_pages == 4);

    /* two clients on one page: one write marks both */
    CHECK(gpu_watch_arm(GPU_WATCH_TEXTURES, a, PAGE));
    CHECK(gpu_watch_arm(GPU_WATCH_BUFFERS, a, PAGE));
    p[7] = 1;
    CHECK(gpu_watch_written(GPU_WATCH_TEXTURES, a, PAGE) && gpu_watch_written(GPU_WATCH_BUFFERS, a, PAGE));

    /* the OS writing into a watched buffer (file read) */
    const char *path = "test_gpu_watch.tmp";
    pal_file *f = NULL;
    CHECK(pal_file_open(path, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE, &f) == PS5C_OK);
    uint8_t data[2 * PAGE];
    for (uint32_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 7);
    CHECK(pal_file_write(f, data, sizeof(data), NULL) == PS5C_OK);
    pal_file_close(f);
    CHECK(gpu_watch_arm(GPU_WATCH_TEXTURES, a + 4 * PAGE, 4 * PAGE));
    CHECK(pal_file_open(path, PAL_FILE_READ, &f) == PS5C_OK);
    uint64_t got = 0;
    CHECK(pal_file_pread(f, (void *)(p + 5 * PAGE), sizeof(data), 0, &got) == PS5C_OK && got == sizeof(data));
    pal_file_close(f);
    pal_file_remove(path);
    CHECK(!memcmp((const void *)(p + 5 * PAGE), data, sizeof(data)));
    CHECK(gpu_watch_written(GPU_WATCH_TEXTURES, a + 5 * PAGE, 2 * PAGE));
    CHECK(!gpu_watch_written(GPU_WATCH_TEXTURES, a + 4 * PAGE, PAGE));
    CHECK(!gpu_watch_written(GPU_WATCH_TEXTURES, a + 7 * PAGE, PAGE));

    /* a mapping change replaced the protection: written, and no stale fault handling */
    CHECK(gpu_watch_arm(GPU_WATCH_TEXTURES, a + 8 * PAGE, 4 * PAGE));
    CHECK(pal_vm_protect((void *)(p + 8 * PAGE), 4 * PAGE, PAL_PROT_READ | PAL_PROT_WRITE) == PS5C_OK);
    gpu_watch_mapping_changed(a + 8 * PAGE, 4 * PAGE);
    CHECK(gpu_watch_written(GPU_WATCH_TEXTURES, a + 8 * PAGE, PAGE));
    CHECK(pal_vm_protect((void *)(p + 8 * PAGE), PAGE, PAL_PROT_READ) == PS5C_OK);
    CHECK(!gpu_watch_fault(a + 8 * PAGE, true)); /* a guest read-only page is not ours */
    CHECK(pal_vm_protect((void *)(p + 8 * PAGE), PAGE, PAL_PROT_READ | PAL_PROT_WRITE) == PS5C_OK);

    /* the race of a mapping change and an arm: protected again, then the state cleared.
     * The write is retried until the page is found stale and made writable. */
    CHECK(gpu_watch_arm(GPU_WATCH_TEXTURES, a + 9 * PAGE, PAGE));
    gpu_watch_mapping_changed(a + 9 * PAGE, PAGE);
    g_retry_mem = &m;
    p[9 * PAGE + 5] = 0x5a;
    g_retry_mem = NULL;
    CHECK(p[9 * PAGE + 5] == 0x5a && gpu_watch_written(GPU_WATCH_TEXTURES, a + 9 * PAGE, PAGE));

    /* writers on other threads while the range is armed again and again */
    atomic_bool stop;
    atomic_init(&stop, false);
    writer w[4];
    pal_thread *t[4];
    for (uint32_t i = 0; i < 4; i++) {
        w[i] = (writer){p, 12 + i, &stop, 0};
        CHECK(pal_thread_create(&t[i], write_loop, &w[i], "writer", 0) == PS5C_OK);
    }
    /* re-armed while every writer keeps faulting, at least 2000 times */
    for (uint32_t k = 0; k < 2000 || w[0].writes < 100000 || w[1].writes < 100000 || w[2].writes < 100000 ||
                         w[3].writes < 100000; k++)
        gpu_watch_arm(GPU_WATCH_BUFFERS, a + 12 * PAGE, 4 * PAGE);
    atomic_store(&stop, true);
    for (uint32_t i = 0; i < 4; i++) {
        pal_thread_join(t[i], NULL);
        CHECK(w[i].writes > 0);
        CHECK(p[w[i].page * PAGE + ((w[i].writes - 1) % PAGE)] == (uint8_t)(w[i].writes - 1));
    }
    CHECK(gpu_watch_arm(GPU_WATCH_BUFFERS, a + 12 * PAGE, 4 * PAGE));
    CHECK(!gpu_watch_written(GPU_WATCH_BUFFERS, a + 12 * PAGE, 4 * PAGE));
    p[13 * PAGE] = 9;
    CHECK(gpu_watch_written(GPU_WATCH_BUFFERS, a + 13 * PAGE, 1));
    CHECK(!gpu_watch_written(GPU_WATCH_BUFFERS, a + 12 * PAGE, PAGE));

    /* copy-on-write saver, generation and written runs (resident buffers) */
    gpu_watch_set_saver(GPU_WATCH_BUFFERS, saver);
    p[2 * PAGE + 9] = 0x77;
    CHECK(gpu_watch_arm(GPU_WATCH_BUFFERS, a, 4 * PAGE));
    guest_addr run = 0;
    uint64_t len = 0;
    CHECK(!gpu_watch_find_written(GPU_WATCH_BUFFERS, a, 4 * PAGE, &run, &len));
    p[2 * PAGE + 9] = 0x11;
    uint64_t gen = gpu_watch_generation();
    p[2 * PAGE + 10] = 0x22; /* page 2 (and 3, released with it) are writable: nothing saved again */
    CHECK(gpu_watch_generation() == gen);
    CHECK(g_saved_count == 2 && g_saved[0] == a + 2 * PAGE && g_saved[1] == a + 3 * PAGE);
    CHECK(g_saved_byte == 0x77 && p[2 * PAGE + 9] == 0x11); /* the saver saw the page before the write */
    CHECK(gpu_watch_find_written(GPU_WATCH_BUFFERS, a, 4 * PAGE, &run, &len) && run == a + 2 * PAGE && len == 2 * PAGE);
    CHECK(gpu_watch_find_written(GPU_WATCH_BUFFERS, a + 3 * PAGE + 8, 100, &run, &len) && run == a + 3 * PAGE + 8 && len == 100);
    gpu_watch_prepare_write(a, PAGE);
    CHECK(g_saved_count == 3 && g_saved[2] == a && gpu_watch_generation() != gen);
    gpu_watch_set_saver(GPU_WATCH_BUFFERS, NULL);
    CHECK(gpu_watch_eligible(a, PAGES * PAGE) && !gpu_watch_eligible(ro, PAGE));

    /* a guest protection change resets the watch */
    CHECK(gpu_watch_arm(GPU_WATCH_TEXTURES, a, PAGE));
    CHECK(gmem_protect(&m, a, PAGES * PAGE, GPROT_RW) == PS5C_OK);
    CHECK(gpu_watch_written(GPU_WATCH_TEXTURES, a, PAGE));
    p[3] = 4; /* writable without a fault */
    CHECK(gmem_unmap(&m, a, PAGES * PAGE) == PS5C_OK);

    /* two views of the same physical memory are watched together (views exist in the placeholder area) */
    gmem_free(&m);
    cfg = (gmem_config){.start = GUEST_PH_BASE - 4 * GUEST_PAGE_SIZE, .size = 0x1000000, .identity = true,
                        .alloc_hint = GUEST_PH_BASE};
    guest_addr o1 = 0, o2 = 0;
    bool s1 = false, s2 = false;
    bool views = pal_ph_supported() && gmem_init(&m, &cfg) == PS5C_OK;
    if (views) {
        gmem_set_change_hook(gpu_watch_mapping_changed);
        gpu_watch_start(&m);
    }
    if (views &&
        gmem_map_direct(&m, GUEST_PH_BASE + 0x40000, 4 * PAGE, PAGE, GPROT_RW, GMAP_FIXED, 0x10000, "view 1", &o1, &s1) == PS5C_OK &&
        gmem_map_direct(&m, GUEST_PH_BASE + 0x80000, 4 * PAGE, PAGE, GPROT_RW, GMAP_FIXED, 0x10000, "view 2", &o2, &s2) == PS5C_OK &&
        s1 && s2) {
        volatile uint8_t *q1 = (volatile uint8_t *)(uintptr_t)o1, *q2 = (volatile uint8_t *)(uintptr_t)o2;
        q1[0] = 1;
        CHECK(q2[0] == 1);
        CHECK(gpu_watch_eligible(o1, PAGE) && gpu_watch_eligible(o2, PAGE));
        CHECK(gpu_watch_arm(GPU_WATCH_TEXTURES, o1, 2 * PAGE));
        CHECK(!gpu_watch_written(GPU_WATCH_TEXTURES, o1, 2 * PAGE));
        q2[PAGE + 3] = 7; /* through the other view: faults, and the armed view knows */
        CHECK(q1[PAGE + 3] == 7);
        CHECK(gpu_watch_written(GPU_WATCH_TEXTURES, o1, 2 * PAGE));
        CHECK(!gpu_watch_written(GPU_WATCH_TEXTURES, o1, PAGE)); /* only the page written */
        CHECK(gpu_watch_arm(GPU_WATCH_TEXTURES, o1, 2 * PAGE));
        q1[5] = 9; /* through the armed view itself */
        CHECK(q2[5] == 9 && gpu_watch_written(GPU_WATCH_TEXTURES, o1, PAGE));
        CHECK(gpu_watch_arm(GPU_WATCH_TEXTURES, o1, 2 * PAGE));
        gpu_watch_prepare_write(o2, PAGE); /* the emulator writes through the other view */
        CHECK(gpu_watch_written(GPU_WATCH_TEXTURES, o1, PAGE));
        q2[1] = 2; /* writable again without a fault loop */
        /* a read-only view in the group is no obstacle: nothing is written through it */
        guest_addr o3 = 0;
        bool s3 = false;
        CHECK(gmem_map_direct(&m, GUEST_PH_BASE + 0xc0000, 4 * PAGE, PAGE, GPROT_READ, GMAP_FIXED, 0x10000, "view 3", &o3, &s3) == PS5C_OK);
        CHECK(gpu_watch_eligible(o1, PAGE) && !gpu_watch_eligible(o3, PAGE));
        CHECK(gpu_watch_arm(GPU_WATCH_TEXTURES, o1, PAGE));
        q2[9] = 3;
        CHECK(gpu_watch_written(GPU_WATCH_TEXTURES, o1, PAGE));
        CHECK(((volatile uint8_t *)(uintptr_t)o3)[9] == 3);
        printf("gpu_watch: views of the same memory ok\n");
    } else {
        printf("gpu_watch: no shared views on this host, their test is skipped\n");
    }
    if (views) gmem_free(&m);
    printf("gpu_watch: ok\n");
    return 0;
}
