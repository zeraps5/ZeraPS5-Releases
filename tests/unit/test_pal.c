/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "pal/pal.h"
#include "test.h"

static void test_vm_lifecycle(void) {
    uint64_t size = PS5C_MiB(4);
    void *base = NULL;
    CHECK(pal_vm_reserve(NULL, size, false, &base) == PS5C_OK);
    CHECK(base != NULL);
    CHECK(((uintptr_t)base % pal_alloc_granularity()) == 0);

    uint64_t page = pal_page_size();
    uint8_t *p = (uint8_t *)base + page * 3;
    CHECK(pal_vm_commit(p, page * 2, PAL_PROT_RW) == PS5C_OK);
    p[0] = 0x11;
    p[page * 2 - 1] = 0x22;
    CHECK(p[0] == 0x11 && p[page * 2 - 1] == 0x22);
    CHECK(pal_vm_protect(p, page * 2, PAL_PROT_READ) == PS5C_OK);
    CHECK(p[0] == 0x11); /* still readable */
    CHECK(pal_vm_protect(p, page * 2, PAL_PROT_RW) == PS5C_OK);
    CHECK(pal_vm_decommit(p, page * 2) == PS5C_OK);
    CHECK(pal_vm_commit(p, page, PAL_PROT_RW) == PS5C_OK);
    CHECK(p[0] == 0); /* decommit dropped the contents */

    CHECK(pal_vm_release(base, size) == PS5C_OK);
}

static void test_vm_fixed(void) {
    uint64_t size = PS5C_MiB(1);
    void *base = NULL;
    CHECK(pal_vm_reserve(NULL, size, false, &base) == PS5C_OK);

    /* The same range can't be reserved twice at a fixed address... */
    void *again = NULL;
    CHECK(pal_vm_reserve(base, size, true, &again) == PS5C_ERR_BUSY);

    /* ...but can be once it is free again. */
    CHECK(pal_vm_release(base, size) == PS5C_OK);
    CHECK(pal_vm_reserve(base, size, true, &again) == PS5C_OK);
    CHECK(again == base);
    CHECK(pal_vm_release(again, size) == PS5C_OK);

    CHECK(pal_vm_reserve(NULL, size, true, &again) == PS5C_ERR_INVALID);
}

static void test_vm_exec(void) {
    /* mov eax, 42 ; ret — checks that committed RX memory is executable. */
    static const uint8_t code[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
    uint64_t size = pal_page_size();
    void *base = NULL;
    CHECK(pal_vm_reserve(NULL, size, false, &base) == PS5C_OK);
    CHECK(pal_vm_commit(base, size, PAL_PROT_RW) == PS5C_OK);
    memcpy(base, code, sizeof(code));
    CHECK(pal_vm_protect(base, size, PAL_PROT_RX) == PS5C_OK);
    pal_icache_flush(base, size);

    int (*fn)(void);
    void *ptr = base;
    memcpy(&fn, &ptr, sizeof(fn)); /* object -> function pointer without a cast warning */
    CHECK(fn() == 42);
    CHECK(pal_vm_release(base, size) == PS5C_OK);
}

static void test_time_and_sleep(void) {
    uint64_t t0 = pal_time_ns();
    pal_sleep_ns(2000000); /* 2 ms */
    uint64_t t1 = pal_time_ns();
    CHECK(t1 > t0);
    CHECK(t1 - t0 >= 1900000); /* allow timer rounding */
    CHECK(t1 - t0 < 2000000000ull);
}

typedef struct {
    pal_mutex *lock;
    uint64_t counter;
} shared_counter;

static int32_t counter_worker(void *arg) {
    shared_counter *s = (shared_counter *)arg;
    for (int i = 0; i < 20000; i++) {
        pal_mutex_lock(s->lock);
        s->counter++;
        pal_mutex_unlock(s->lock);
    }
    return 7;
}

static void test_threads_mutex(void) {
    shared_counter s;
    s.counter = 0;
    CHECK(pal_mutex_create(&s.lock) == PS5C_OK);
    pal_thread *threads[4];
    for (int i = 0; i < 4; i++) {
        CHECK(pal_thread_create(&threads[i], counter_worker, &s, "ps5c-test", 0) == PS5C_OK);
    }
    for (int i = 0; i < 4; i++) {
        int32_t ret = 0;
        CHECK(pal_thread_join(threads[i], &ret) == PS5C_OK);
        CHECK(ret == 7);
    }
    CHECK(s.counter == 4 * 20000);
    CHECK(pal_mutex_trylock(s.lock));
    pal_mutex_unlock(s.lock);
    pal_mutex_destroy(s.lock);
}

typedef struct {
    pal_mutex *lock;
    pal_cond *cond;
    int value;
} mailbox;

static int32_t producer(void *arg) {
    mailbox *mb = (mailbox *)arg;
    pal_sleep_ns(1000000);
    pal_mutex_lock(mb->lock);
    mb->value = 99;
    pal_cond_signal(mb->cond);
    pal_mutex_unlock(mb->lock);
    return 0;
}

static void test_condition_variable(void) {
    mailbox mb;
    mb.value = 0;
    CHECK(pal_mutex_create(&mb.lock) == PS5C_OK);
    CHECK(pal_cond_create(&mb.cond) == PS5C_OK);

    pal_mutex_lock(mb.lock);
    CHECK(pal_cond_wait_ns(mb.cond, mb.lock, 1000000) == PS5C_ERR_TIMEOUT);
    pal_mutex_unlock(mb.lock);

    pal_thread *t;
    CHECK(pal_thread_create(&t, producer, &mb, "producer", 0) == PS5C_OK);
    pal_mutex_lock(mb.lock);
    while (mb.value == 0) {
        pal_cond_wait(mb.cond, mb.lock);
    }
    pal_mutex_unlock(mb.lock);
    CHECK(mb.value == 99);
    CHECK(pal_thread_join(t, NULL) == PS5C_OK);

    pal_cond_destroy(mb.cond);
    pal_mutex_destroy(mb.lock);
}

static void test_files(void) {
    const char *path = "ps5c_pal_test.tmp";
    static const char payload[] = "PS5C file roundtrip \xC3\xA7\xC3\xA3o";
    pal_file *f;

    CHECK(pal_file_open(path, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE, &f) == PS5C_OK);
    uint64_t written = 0;
    CHECK(pal_file_write(f, payload, sizeof(payload) - 1, &written) == PS5C_OK);
    CHECK(written == sizeof(payload) - 1);
    pal_file_close(f);

    uint8_t *data = NULL;
    uint64_t size = 0;
    CHECK(pal_file_read_all(path, &data, &size) == PS5C_OK);
    CHECK(size == sizeof(payload) - 1);
    CHECK(memcmp(data, payload, (size_t)size) == 0);
    CHECK(data[size] == 0);
    free(data);

    CHECK(pal_file_open(path, PAL_FILE_READ, &f) == PS5C_OK);
    uint64_t pos = 0;
    CHECK(pal_file_seek(f, 5, PAL_SEEK_SET, &pos) == PS5C_OK && pos == 5);
    char buf[4];
    uint64_t got = 0;
    CHECK(pal_file_read(f, buf, 4, &got) == PS5C_OK && got == 4);
    CHECK(memcmp(buf, "file", 4) == 0);
    CHECK(pal_file_size(f, &size) == PS5C_OK && size == sizeof(payload) - 1);
    CHECK(pal_file_seek(f, 0, PAL_SEEK_END, &pos) == PS5C_OK && pos == size);
    CHECK(pal_file_read(f, buf, 4, &got) == PS5C_OK && got == 0); /* EOF */
    pal_file_close(f);

    CHECK(pal_file_remove(path) == PS5C_OK);
    CHECK(pal_file_open(path, PAL_FILE_READ, &f) == PS5C_ERR_NOT_FOUND);
    CHECK(pal_file_read_all("does/not/exist.bin", &data, &size) == PS5C_ERR_NOT_FOUND);
}

static void test_file_views(void) {
    const char *path = "ps5c_pal_view_\xC3\xA7.tmp";
    uint8_t payload[8195];
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)(i * 37 + 9);
    pal_file *f = NULL;
    CHECK(pal_file_open(path, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE, &f) == PS5C_OK);
    uint64_t written = 0;
    CHECK(pal_file_write(f, payload, sizeof(payload), &written) == PS5C_OK && written == sizeof(payload));
    pal_file_close(f);
    pal_file_view v = {0}, second = {0};
    CHECK(pal_file_view_open(path, &v) == PS5C_OK);
    CHECK(v.data && v.opaque && v.size == sizeof(payload));
    CHECK(memcmp(v.data, payload, sizeof(payload)) == 0);
    CHECK(pal_file_view_open(path, &second) == PS5C_OK);
    CHECK(memcmp(second.data, payload, sizeof(payload)) == 0);
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info;
    CHECK(VirtualQuery(v.data, &info, sizeof(info)) == sizeof(info));
    CHECK(info.Type == MEM_MAPPED && info.Protect == PAGE_READONLY);
    CHECK(pal_file_open(path, PAL_FILE_WRITE, &f) == PS5C_ERR_BUSY);
    CHECK(pal_file_remove(path) == PS5C_ERR_BUSY);
#endif
    pal_file_view_close(&second);
    CHECK(memcmp(v.data, payload, sizeof(payload)) == 0);
    pal_file_view_close(&v);
    CHECK(!v.data && !v.size && !v.opaque);
    pal_file_view_close(&v);
    pal_file_view_close(NULL);
    CHECK(pal_file_open(path, PAL_FILE_WRITE | PAL_FILE_TRUNCATE, &f) == PS5C_OK);
    pal_file_close(f);
    CHECK(pal_file_view_open(path, &v) == PS5C_OK && !v.data && !v.size);
    pal_file_view_close(&v);
    CHECK(pal_file_remove(path) == PS5C_OK);
    CHECK(pal_file_view_open(path, &v) == PS5C_ERR_NOT_FOUND);
    CHECK(!v.data && !v.size && !v.opaque);
    CHECK(pal_file_view_open("unmounted.exfat/eboot.bin", &v) == PS5C_ERR_UNSUPPORTED);
    CHECK(!v.data && !v.size && !v.opaque);
}

static void test_host_diagnostics(void) {
    pal_host_info info;
    pal_host_get_info(&info);
    CHECK(info.cpu[0] && info.os[0]);
    CHECK(info.cpu[sizeof(info.cpu) - 1] == 0 && info.os[sizeof(info.os) - 1] == 0);
    CHECK(info.logical_processors > 0 && info.physical_cores <= info.logical_processors);
    pal_host_stats a, b;
    pal_host_get_stats(&a);
    pal_sleep_ns(10000000);
    pal_host_get_stats(&b);
#ifdef _WIN32
    CHECK((a.valid & (PAL_HOST_RAM | PAL_HOST_COMMIT | PAL_HOST_PROCESS_RAM | PAL_HOST_PROCESS_COMMIT | PAL_HOST_PROCESS_CPU)) ==
          (PAL_HOST_RAM | PAL_HOST_COMMIT | PAL_HOST_PROCESS_RAM | PAL_HOST_PROCESS_COMMIT | PAL_HOST_PROCESS_CPU));
    CHECK(a.process_ram > 0 && a.process_commit > 0 && a.commit_limit > 0);
#endif
    if (a.valid & PAL_HOST_RAM) CHECK(a.ram_total > 0 && a.ram_available <= a.ram_total);
    if (a.valid & b.valid & PAL_HOST_PROCESS_CPU) CHECK(b.process_cpu_ns >= a.process_cpu_ns);
    if (a.valid & b.valid & PAL_HOST_SYSTEM_CPU) {
        CHECK(b.system_cpu_ns >= a.system_cpu_ns && b.system_idle_ns >= a.system_idle_ns);
        CHECK(a.system_idle_ns <= a.system_cpu_ns);
    }
    printf("host: %s; %s; %u cores/%u threads; RAM %llu/%llu; process %llu; commit %llu/%llu\n",
           info.cpu, info.os, info.physical_cores, info.logical_processors,
           (unsigned long long)(a.ram_total - a.ram_available), (unsigned long long)a.ram_total,
           (unsigned long long)a.process_ram, (unsigned long long)a.commit_total, (unsigned long long)a.commit_limit);
}

#ifdef _WIN32
static void test_panel_key_messages(void) {
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 panel key test", 96, 64, &window) == PS5C_OK);
    void *display = NULL, *handle = NULL;
    pal_window_native(window, &display, &handle);
    HWND hwnd = (HWND)handle;
    /* The window has already received the key event; the render thread may
     * not ask until a slow frame ends. No physical key is held during it. */
    SendMessageW(hwnd, WM_KEYDOWN, VK_F10, 1);
    pal_sleep_ns(100000000ull);
    bool first = pal_input_panel_toggled(), consumed = pal_input_panel_toggled();
    SendMessageW(hwnd, WM_KEYDOWN, VK_F10, 1 | ((LPARAM)1 << 30));
    bool repeated = pal_input_panel_toggled();
    SendMessageW(hwnd, WM_KEYDOWN, VK_F10, 1);
    SendMessageW(hwnd, WM_KEYDOWN, VK_F10, 1);
    bool queued1 = pal_input_panel_toggled(), queued2 = pal_input_panel_toggled();
    bool empty = pal_input_panel_toggled();
    SendMessageW(hwnd, WM_SYSKEYDOWN, VK_F10, 1);
    bool system = pal_input_panel_toggled(), system_empty = pal_input_panel_toggled();
    SendMessageW(hwnd, WM_KEYDOWN, VK_F9, 1);
    bool other = pal_input_panel_toggled();
    pal_window_destroy(window);
    printf("F10 messages: delayed %u, consumed %u, repeat %u, queued %u/%u, system %u\n",
           first, consumed, repeated, queued1, queued2, system);
    CHECK(first && !consumed && !repeated && queued1 && queued2 && !empty && system && !system_empty && !other);
}
#endif

int main(void) {
    if (pal_init() != PS5C_OK) {
        printf("pal_init failed\n");
        return 1;
    }
    static const test_case tests[] = {
        TEST_CASE(test_vm_lifecycle),  TEST_CASE(test_vm_fixed),          TEST_CASE(test_vm_exec),
        TEST_CASE(test_time_and_sleep), TEST_CASE(test_threads_mutex),
        TEST_CASE(test_condition_variable), TEST_CASE(test_files), TEST_CASE(test_file_views), TEST_CASE(test_host_diagnostics),
#ifdef _WIN32
        TEST_CASE(test_panel_key_messages),
#endif
    };
    int rc = test_run_all("pal", tests, PS5C_ARRAY_COUNT(tests));
    pal_shutdown();
    return rc;
}
