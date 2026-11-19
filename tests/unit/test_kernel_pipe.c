/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/runtime.h"
#include "hle/hle.h"
#include "hle/net_loopback.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "pipe line %d: %s\n", __LINE__, #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static ps5_runtime rt;
static guest_thread thread;
static guest_addr page;
static int64_t call(const char *name, uint64_t a, uint64_t b, uint64_t d) {
    const hle_export *e = hle_find("libkernel", nid_hash_name(name));
    CHECK(e && e->fn);
    hle_args args = {.gpr = {a, b, d}};
    hle_call c = {.a = &args, .rt = &rt, .mem = &rt.mem, .thread = &thread};
    e->fn(&c);
    return (int64_t)args.ret[0];
}
static int error(void) {
    int value;
    CHECK(gmem_read(&rt.mem, thread.errno_addr, &value, 4));
    return value;
}
static atomic_int reader_started, reader_done;
static int read_fd;
static int32_t reader(void *unused) {
    (void)unused;
    atomic_store(&reader_started, 1);
    int64_t result = call("read", read_fd, page + 1024, 4);
    atomic_store(&reader_done, 1);
    return (int32_t)result;
}
int main(void) {
    CHECK(pal_init() == PS5C_OK && hle_registry_init() == PS5C_OK);
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    CHECK(gmem_init(&rt.mem, &cfg) == PS5C_OK);
    CHECK(gmem_map(&rt.mem, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "pipe buffers", &page) == PS5C_OK);
    thread.errno_addr = page + 512;
    CHECK(call("pipe", UINT64_MAX, 0, 0) == -1 && error() == 14);
    CHECK(call("pipe", page, 0, 0) == 0);
    int ends[2];
    CHECK(gmem_read(&rt.mem, page, ends, 8));
    CHECK(ends[0] >= 3 && ends[1] > ends[0]);
    CHECK(call("dup2", -1, ends[0], 0) == -1 && error() == 9);
    CHECK(call("dup2", ends[1], 2, 0) == -1 && error() == 1); /* console refusal, fdopen fallback */
    CHECK(call("dup2", ends[0], ends[0], 0) == ends[0]);
    CHECK(call("fcntl", ends[0], 4, 4) == 0);
    CHECK(call("read", ends[0], page + 1024, 1) == -1 && error() == 35);
    CHECK(call("fcntl", ends[0], 4, 0) == 0);
    CHECK(kernel_file_poll_fd(ends[0], false) == 0 && kernel_file_poll_fd(ends[1], true) == 1);
    struct {
        int32_t fd;
        uint16_t events, revents;
    } pollfds[3] = {{ends[0], 1, 99}, {-1, 1, 99}, {99999, 1, 99}};
    CHECK(gmem_write(&rt.mem, page + 256, pollfds, sizeof(pollfds)));
    CHECK(call("poll", page + 256, 3, 0) == 1);
    CHECK(gmem_read(&rt.mem, page + 256, pollfds, sizeof(pollfds)));
    CHECK(pollfds[0].revents == 0 && pollfds[1].revents == 0 && pollfds[2].revents == 32);
    CHECK(call("poll", UINT64_MAX, 1, 0) == -1 && error() == 14);
    read_fd = ends[0];
    pal_thread *t;
    CHECK(pal_thread_create(&t, reader, NULL, "pipe read", 0) == PS5C_OK);
    while (!atomic_load(&reader_started))
        pal_thread_yield();
    pal_sleep_ns(10000000);
    CHECK(!atomic_load(&reader_done));
    CHECK(gmem_write(&rt.mem, page + 128, "wake", 4));
    CHECK(call("write", ends[1], page + 128, 4) == 4);
    int32_t result;
    CHECK(pal_thread_join(t, &result) == PS5C_OK && result == 4);
    char bytes[4];
    CHECK(gmem_read(&rt.mem, page + 1024, bytes, 4) && !memcmp(bytes, "wake", 4));
    /* Reverse direction works too (FreeBSD pipe semantics). */
    CHECK(call("write", ends[0], page + 128, 4) == 4 && call("read", ends[1], page + 1024, 4) == 4);
    CHECK(call("pread", ends[0], page + 1024, 4) == -1 && error() == 29);
    CHECK(call("lseek", ends[0], 0, 0) == -1 && error() == 29);
    int duplicate = (int)call("fcntl", ends[1], 0, 100);
    CHECK(duplicate >= 100);
    CHECK(call("close", ends[1], 0, 0) == 0);
    CHECK(call("write", duplicate, page + 128, 4) == 4);
    CHECK(call("close", duplicate, 0, 0) == 0);
    CHECK(call("poll", page + 256, 1, 0) == 1);
    CHECK(gmem_read(&rt.mem, page + 256, pollfds, sizeof(pollfds)));
    CHECK((pollfds[0].revents & 17) == 17); /* readable data and peer hangup */
    CHECK(call("read", ends[0], page + 1024, 4) == 4 && call("read", ends[0], page + 1024, 1) == 0);
    CHECK(call("write", ends[0], page + 128, 1) == -1 && error() == 32);
    CHECK(call("read", ends[0], UINT64_MAX, 0) == 0);
    CHECK(call("close", ends[0], 0, 0) == 0);
    /* Allocation must release both descriptors on failure, and close/reuse
     * must not leak references or leave an unrelated file slot reserved. */
    for (unsigned i = 0; i < 80; i++) {
        CHECK(call("pipe", page, 0, 0) == 0);
        int next[2];
        CHECK(gmem_read(&rt.mem, page, next, 8));
        CHECK(next[0] == ends[0] && next[1] == ends[1]);
        CHECK(call("close", next[0], 0, 0) == 0 && call("close", next[1], 0, 0) == 0);
    }
    gmem_free(&rt.mem);
    hle_registry_free();
    pal_shutdown();
    puts("kernel pipes: blocking, bidirectional transfer, flags, dup lifetime, EOF, EPIPE and "
         "descriptor reuse verified");
    return 0;
}
