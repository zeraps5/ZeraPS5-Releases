/* SPDX-License-Identifier: GPL-2.0-or-later */
/* gpu_watch in copy-on-write mode: writes are found without any fault, and the private copies
 * they leave survive unmapping, partial unmapping, protection changes and a second view. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu/gpu_watch.h"
#include "pal/pal.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

#define PAGE 4096u
#define VIEW (8u * PAGE) /* two guest pages */

static atomic_uint g_faults;
static bool on_fault(void *user, pal_fault_info *info) {
    (void)user, (void)info;
    atomic_fetch_add(&g_faults, 1); /* this mode must never fault */
    return false;
}

static int32_t write_loop(void *arg) {
    volatile uint8_t *p = (volatile uint8_t *)arg;
    for (uint32_t i = 0; i < 200000; i++) p[i % PAGE] = (uint8_t)i;
    return 0;
}

/* A guest thread writing a neighbour page while part of its view is unmapped: each slot is
 * written once, so a write that lands between the copy of the private page and the unmap stays
 * lost. The world hooks stop it (as runtime.c stops every guest thread). */
static atomic_uintptr_t g_writer;
static atomic_uint g_written, g_stops, g_resumes;
static atomic_int g_writer_done;
static int32_t slot_writer(void *arg) {
    atomic_store(&g_writer, pal_thread_open_current());
    volatile uint32_t *w = (volatile uint32_t *)arg;
    for (uint32_t i = 0; i < PAGE / 4; i++) {
        w[i] = i + 1;
        atomic_store(&g_written, i + 1);
        for (volatile uint32_t k = 0; k < 2000; k++) {}
    }
    /* alive until the unmap is done: a finished thread cannot be suspended (a busy PC
     * could let it finish first) */
    while (!atomic_load(&g_writer_done)) pal_thread_yield();
    return 0;
}
static bool stop_writer(void *user) {
    (void)user;
    uintptr_t h = atomic_load(&g_writer);
    atomic_fetch_add(&g_stops, 1);
    if (!h || !pal_thread_suspend(h)) return false;
    pal_cpu_regs regs;
    pal_thread_get_regs(h, &regs);
    return true;
}
static void resume_writer(void *user) {
    (void)user;
    atomic_fetch_add(&g_resumes, 1);
    pal_thread_resume(atomic_load(&g_writer));
}

int main(void) {
#ifdef _WIN32
    _putenv("PS5C_GPU_WATCH=cow");
    _putenv("PS5C_GPU_WATCH_SWEEP=0"); /* exact checks: every result here is immediate */
#else
    setenv("PS5C_GPU_WATCH", "cow", 1);
    setenv("PS5C_GPU_WATCH_SWEEP", "0", 1);
#endif
    CHECK(pal_init() == PS5C_OK);
    if (!pal_vm_cow_supported() || !pal_ph_supported()) {
        printf("gpu_watch cow: not supported on this host, skipped\n");
        return 0;
    }
    CHECK(gpu_watch_enabled());
    CHECK(pal_fault_handler_install(on_fault, NULL) == PS5C_OK);
    gmem m;
    gmem_config cfg = {.start = GUEST_PH_BASE - 4 * GUEST_PAGE_SIZE, .size = 0x1000000, .identity = true,
                       .alloc_hint = GUEST_PH_BASE};
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    gmem_set_change_hook(gpu_watch_mapping_changed);
    gpu_watch_start(&m);

    guest_addr o1 = 0;
    bool shared = false;
    CHECK(gmem_map_direct(&m, GUEST_PH_BASE + 0x40000, VIEW, PAGE, GPROT_RW, GMAP_FIXED, 0x10000, "view 1", &o1, &shared) == PS5C_OK && shared);
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)o1;
    for (uint32_t i = 0; i < VIEW; i++) p[i] = (uint8_t)(i >> 12);

    /* armed: clean until a write, which raises nothing and marks only its page */
    CHECK(gpu_watch_eligible(o1, VIEW));
    CHECK(gpu_watch_written(GPU_WATCH_TEXTURES, o1, PAGE));
    CHECK(gpu_watch_arm(GPU_WATCH_TEXTURES, o1, 4 * PAGE));
    CHECK(!gpu_watch_written(GPU_WATCH_TEXTURES, o1, 4 * PAGE));
    CHECK(gpu_watch_written(GPU_WATCH_BUFFERS, o1, 4 * PAGE));
    CHECK(p[100] == 0);
    p[PAGE + 5] = 0x5a;
    CHECK(p[PAGE + 5] == 0x5a);
    CHECK(gpu_watch_written(GPU_WATCH_TEXTURES, o1 + PAGE, 1));
    CHECK(!gpu_watch_written(GPU_WATCH_TEXTURES, o1, PAGE));
    CHECK(!gpu_watch_written(GPU_WATCH_TEXTURES, o1 + 2 * PAGE, 2 * PAGE));
    guest_addr run = 0;
    uint64_t len = 0;
    CHECK(gpu_watch_find_written(GPU_WATCH_TEXTURES, o1, 4 * PAGE, &run, &len) && run == o1 + PAGE && len == PAGE);
    /* a written page is a private copy: it cannot be armed again, its neighbours can */
    CHECK(!gpu_watch_arm(GPU_WATCH_TEXTURES, o1, 4 * PAGE));
    CHECK(gpu_watch_written(GPU_WATCH_TEXTURES, o1 + PAGE, PAGE));
    CHECK(gpu_watch_arm(GPU_WATCH_TEXTURES, o1, PAGE) && !gpu_watch_written(GPU_WATCH_TEXTURES, o1, PAGE));
    /* two clients on one page: one write marks both */
    CHECK(gpu_watch_arm(GPU_WATCH_BUFFERS, o1, PAGE));
    p[7] = 1;
    CHECK(gpu_watch_written(GPU_WATCH_TEXTURES, o1, PAGE) && gpu_watch_written(GPU_WATCH_BUFFERS, o1, PAGE));

    /* the system writing into an armed page (file read): no error, found like any write */
    const char *path = "test_gpu_watch_cow.tmp";
    pal_file *f = NULL;
    CHECK(pal_file_open(path, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE, &f) == PS5C_OK);
    uint8_t data[PAGE];
    for (uint32_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 7 + 1);
    CHECK(pal_file_write(f, data, sizeof(data), NULL) == PS5C_OK);
    pal_file_close(f);
    CHECK(gpu_watch_arm(GPU_WATCH_VERTICES, o1 + 2 * PAGE, 2 * PAGE));
    CHECK(pal_file_open(path, PAL_FILE_READ, &f) == PS5C_OK);
    uint64_t got = 0;
    CHECK(pal_file_pread(f, (void *)(p + 2 * PAGE), sizeof(data), 0, &got) == PS5C_OK && got == sizeof(data));
    pal_file_close(f);
    pal_file_remove(path);
    CHECK(!memcmp((const void *)(p + 2 * PAGE), data, sizeof(data)));
    CHECK(gpu_watch_written(GPU_WATCH_VERTICES, o1 + 2 * PAGE, PAGE));
    CHECK(!gpu_watch_written(GPU_WATCH_VERTICES, o1 + 3 * PAGE, PAGE));

    /* another thread writing while the range is armed and checked */
    CHECK(gpu_watch_arm(GPU_WATCH_VERTICES, o1 + 6 * PAGE, PAGE));
    pal_thread *t = NULL;
    CHECK(pal_thread_create(&t, write_loop, (void *)(p + 6 * PAGE), "writer", 0) == PS5C_OK);
    pal_thread_join(t, NULL);
    CHECK(gpu_watch_written(GPU_WATCH_VERTICES, o1 + 6 * PAGE, PAGE));

    /* unmapped and mapped again elsewhere: the private pages went back into the physical memory */
    CHECK(gmem_unmap(&m, o1, VIEW) == PS5C_OK);
    guest_addr o2 = 0;
    CHECK(gmem_map_direct(&m, GUEST_PH_BASE + 0x80000, VIEW, PAGE, GPROT_RW, GMAP_FIXED, 0x10000, "view 1 again", &o2, &shared) == PS5C_OK);
    volatile uint8_t *q = (volatile uint8_t *)(uintptr_t)o2;
    CHECK(q[PAGE + 5] == 0x5a && q[7] == 1 && q[PAGE + 6] == 1 && q[5 * PAGE] == 5);
    CHECK(!memcmp((const void *)(q + 2 * PAGE), data, sizeof(data)));
    CHECK(q[6 * PAGE + (199999 % PAGE)] == (uint8_t)199999);

    /* a fresh view is armable again; a second view of its memory makes it shared and ineligible */
    CHECK(gpu_watch_arm(GPU_WATCH_TEXTURES, o2, 2 * PAGE));
    q[9] = 0x77; /* private in o2 */
    guest_addr o3 = 0;
    CHECK(gmem_map_direct(&m, GUEST_PH_BASE + 0xc0000, VIEW, PAGE, GPROT_RW, GMAP_FIXED, 0x10000, "view 2", &o3, &shared) == PS5C_OK);
    volatile uint8_t *r = (volatile uint8_t *)(uintptr_t)o3;
    CHECK(r[9] == 0x77);       /* written back before the second view appeared */
    q[10] = 0x78;              /* and the first view shares its pages again */
    CHECK(r[10] == 0x78);
    r[PAGE + 1] = 0x79;
    CHECK(q[PAGE + 1] == 0x79);
    CHECK(!gpu_watch_eligible(o2, PAGE) && !gpu_watch_arm(GPU_WATCH_TEXTURES, o2, PAGE));
    CHECK(gpu_watch_written(GPU_WATCH_TEXTURES, o2, PAGE));
    CHECK(gmem_unmap(&m, o3, VIEW) == PS5C_OK);
    CHECK(gmem_unmap(&m, o2, VIEW) == PS5C_OK);

    /* partial unmap: the part that stays is mapped again and keeps its private pages */
    guest_addr o4 = 0;
    CHECK(gmem_map_direct(&m, GUEST_PH_BASE + 0x100000, VIEW, PAGE, GPROT_RW, GMAP_FIXED, 0x40000, "view 3", &o4, &shared) == PS5C_OK);
    volatile uint8_t *s = (volatile uint8_t *)(uintptr_t)o4;
    for (uint32_t i = 0; i < VIEW; i++) s[i] = (uint8_t)(0x80 + (i >> 12));
    CHECK(gpu_watch_arm(GPU_WATCH_TEXTURES, o4, VIEW));
    s[6 * PAGE + 3] = 0x11;
    s[PAGE + 3] = 0x12;
    CHECK(gmem_unmap(&m, o4, GUEST_PAGE_SIZE) == PS5C_OK);
    CHECK(s[6 * PAGE + 3] == 0x11 && s[5 * PAGE] == 0x85);
    /* a protection change does not make the tracker forget the page it let go private */
    CHECK(gmem_protect(&m, o4 + GUEST_PAGE_SIZE, VIEW - GUEST_PAGE_SIZE, GPROT_READ) == PS5C_OK);
    CHECK(gmem_protect(&m, o4 + GUEST_PAGE_SIZE, VIEW - GUEST_PAGE_SIZE, GPROT_RW) == PS5C_OK);
    CHECK(gpu_watch_arm(GPU_WATCH_TEXTURES, o4 + 4 * PAGE, PAGE));
    s[4 * PAGE + 1] = 0x13;
    CHECK(gmem_protect(&m, o4 + GUEST_PAGE_SIZE, VIEW - GUEST_PAGE_SIZE, GPROT_READ) == PS5C_OK);
    CHECK(gmem_unmap(&m, o4 + GUEST_PAGE_SIZE, VIEW - GUEST_PAGE_SIZE) == PS5C_OK);
    guest_addr o5 = 0;
    CHECK(gmem_map_direct(&m, GUEST_PH_BASE + 0x140000, VIEW, PAGE, GPROT_RW, GMAP_FIXED, 0x40000, "view 3 again", &o5, &shared) == PS5C_OK);
    volatile uint8_t *u = (volatile uint8_t *)(uintptr_t)o5;
    CHECK(u[PAGE + 3] == 0x12 && u[6 * PAGE + 3] == 0x11 && u[4 * PAGE + 1] == 0x13 && u[7 * PAGE] == 0x87);

    /* partial unmap while another thread writes the part that stays: no write lost */
    gmem_set_world_hooks(stop_writer, resume_writer, NULL);
    guest_addr o6 = 0;
    CHECK(gmem_map_direct(&m, GUEST_PH_BASE + 0x180000, VIEW, PAGE, GPROT_RW, GMAP_FIXED, 0x80000, "view 4", &o6, &shared) == PS5C_OK);
    volatile uint8_t *v6 = (volatile uint8_t *)(uintptr_t)o6;
    for (uint32_t i = 0; i < VIEW; i++) v6[i] = 0;
    CHECK(gpu_watch_arm(GPU_WATCH_TEXTURES, o6, VIEW));
    v6[3] = 1; /* the tracker holds private pages in this view */
    CHECK(pal_thread_create(&t, slot_writer, (void *)(v6 + 6 * PAGE), "slot writer", 0) == PS5C_OK);
    while (atomic_load(&g_written) < 64) pal_thread_yield();
    CHECK(gmem_unmap(&m, o6, GUEST_PAGE_SIZE) == PS5C_OK);
    atomic_store(&g_writer_done, 1);
    pal_thread_join(t, NULL);
    CHECK(atomic_load(&g_stops) == 1 && atomic_load(&g_resumes) == 1);
    for (uint32_t i = 0; i < PAGE / 4; i++) CHECK(((volatile uint32_t *)(v6 + 6 * PAGE))[i] == i + 1);
    gmem_set_world_hooks(NULL, NULL, NULL);
    pal_thread_close_handle(atomic_load(&g_writer));
    CHECK(gmem_unmap(&m, o6 + GUEST_PAGE_SIZE, VIEW - GUEST_PAGE_SIZE) == PS5C_OK);

    /* what the sweeper does: a write found without any client asking the system */
    CHECK(gpu_watch_arm(GPU_WATCH_TEXTURES, o5 + 7 * PAGE, PAGE));
    u[7 * PAGE + 9] = 0x21;
    gpu_watch_sweep();
    CHECK(gpu_watch_find_written(GPU_WATCH_TEXTURES, o5 + 7 * PAGE, PAGE, &run, &len) && run == o5 + 7 * PAGE);

    CHECK(atomic_load(&g_faults) == 0);
    gmem_free(&m);
    printf("gpu_watch cow: ok\n");
    return 0;
}
