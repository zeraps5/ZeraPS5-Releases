/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/runtime.h"
#include "hle/hle.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "readv line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static ps5_runtime rt;
static module_set ms;
static guest_thread thread;
static guest_addr page;
typedef struct guest_iov { uint64_t base, len; } guest_iov;

static int64_t call(const char *lib, const char *name, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    const hle_export *exp = hle_find(lib, nid_hash_name(name));
    CHECK(exp && exp->fn);
    hle_args args = {.gpr = {a, b, c, d}};
    hle_call context = {.rt = &rt, .mem = &rt.mem, .thread = &thread, .a = &args};
    exp->fn(&context);
    return (int64_t)args.ret[0];
}
#define K(n, a, b, c, d) call("libkernel", n, a, b, c, d)

static void put(guest_addr addr, const void *data, size_t size) { CHECK(gmem_write(&rt.mem, addr, data, size)); }
static void bytes(guest_addr addr, const char *expected, size_t size) {
    char actual[32];
    CHECK(size <= sizeof(actual) && gmem_read(&rt.mem, addr, actual, size));
    CHECK(!memcmp(actual, expected, size));
}
static void expect_error(const char *lib, const char *name, int fd, guest_addr vec, int count,
                         int64_t offset, int error) {
    int sentinel = 777, seen = 0;
    put(thread.errno_addr, &sentinel, sizeof(sentinel));
    bool sce = !strcmp(name, "sceKernelPreadv");
    int64_t result = call(lib, name, (uint64_t)(int64_t)fd, vec, (uint64_t)(int64_t)count, (uint64_t)offset);
    CHECK(result == (sce ? (int64_t)(int32_t)(0x80020000u | (uint32_t)error) : -1));
    CHECK(gmem_read(&rt.mem, thread.errno_addr, &seen, sizeof(seen)));
    CHECK(seen == (sce ? sentinel : error));
}

int main(void) {
    CHECK(pal_init() == PS5C_OK && hle_registry_init() == PS5C_OK);
    module_set_init(&ms); rt.ms = &ms;
    gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&rt.mem, &cfg) == PS5C_OK);
    CHECK(gmem_map(&rt.mem, 0, 4 * GUEST_PAGE_SIZE, GPROT_RW, 0, "readv fixture", &page) == PS5C_OK);
    thread.errno_addr = page + 128;
    const char *fixture = "test-kernel-readv-fixture.bin";
    FILE *file = fopen(fixture, "wb");
    CHECK(file && fwrite("abcdefghijkl", 1, 12, file) == 12 && fclose(file) == 0);
    put(page, fixture, strlen(fixture) + 1);
    int fd = (int)K("open", page, 0, 0, 0); CHECK(fd >= 3);
    guest_addr vec = page + 256, first = page + 0x6000, second = first + 64;
    const char *libs[] = {"libkernel", "libkernel", "libScePosix"};
    const char *names[] = {"sceKernelPreadv", "preadv", "preadv"};
    for (unsigned api = 0; api < PS5C_ARRAY_COUNT(names); api++) {
        const char *lib = libs[api], *name = names[api];
        CHECK(K("lseek", fd, 7, 0, 0) == 7);
        guest_iov vectors[3] = {{UINT64_MAX, 0}, {first, 3}, {second, 5}};
        put(vec, vectors, sizeof(vectors));
        CHECK(call(lib, name, fd, vec, 3, 2) == 8);
        bytes(first, "cde", 3); bytes(second, "fghij", 5);
        CHECK(K("lseek", fd, 0, 1, 0) == 7);
        put(first, "--------", 8); put(second, "--------", 8);
        CHECK(call(lib, name, fd, vec, 3, 10) == 2);
        bytes(first, "kl------", 8); bytes(second, "--------", 8);
        CHECK(call(lib, name, fd, vec, 3, 12) == 0);
        CHECK(K("lseek", fd, 0, 1, 0) == 7);
        expect_error(lib, name, fd, vec, 3, -1, 22);
        expect_error(lib, name, -1, vec, 3, 0, 9);
        expect_error(lib, name, fd, UINT64_MAX, 1, 0, 14);
        expect_error(lib, name, fd, page + 4 * GUEST_PAGE_SIZE - 8, 1, 0, 14);
        expect_error(lib, name, fd, vec, -1, 0, 22);
        expect_error(lib, name, fd, vec, 0, 0, 22);
        expect_error(lib, name, fd, vec, 1025, 0, 22);
        vectors[0] = (guest_iov){first, INT64_MAX}; vectors[1] = (guest_iov){second, 1};
        put(vec, vectors, 2 * sizeof(*vectors));
        expect_error(lib, name, fd, vec, 2, 0, 22);
        vectors[0] = (guest_iov){UINT64_MAX, 4};
        put(vec, vectors, sizeof(*vectors));
        expect_error(lib, name, fd, vec, 1, 0, 14);
        vectors[0] = (guest_iov){first, 3}; vectors[1] = (guest_iov){UINT64_MAX, 2};
        put(vec, vectors, 2 * sizeof(*vectors));
        CHECK(call(lib, name, fd, vec, 2, 2) == 3); bytes(first, "cde", 3);
        CHECK(K("lseek", fd, 0, 1, 0) == 7);
        guest_iov empty[1024] = {{0}};
        put(vec, empty, sizeof(empty));
        CHECK(call(lib, name, fd, vec, 1024, 0) == 0);
        CHECK(K("lseek", fd, 0, 1, 0) == 7);
        int ends[2];
        CHECK(K("pipe", first + 128, 0, 0, 0) == 0);
        CHECK(gmem_read(&rt.mem, first + 128, ends, sizeof(ends)));
        vectors[0] = (guest_iov){first, 3}; put(vec, vectors, sizeof(*vectors));
        expect_error(lib, name, ends[0], vec, 1, 0, 29);
        CHECK(K("close", ends[0], 0, 0, 0) == 0 && K("close", ends[1], 0, 0, 0) == 0);
    }
    /* The shared readv path still advances the ordinary descriptor position. */
    guest_iov vectors[2] = {{first, 2}, {second, 3}};
    put(vec, vectors, sizeof(vectors));
    CHECK(K("lseek", fd, 3, 0, 0) == 3 && K("readv", fd, vec, 2, 0) == 5);
    bytes(first, "de", 2); bytes(second, "fgh", 3);
    CHECK(K("lseek", fd, 0, 1, 0) == 8);
    CHECK(K("close", fd, 0, 0, 0) == 0 && remove(fixture) == 0);
    gmem_free(&rt.mem); module_set_free(&ms); hle_registry_free(); pal_shutdown();
    puts("vectored reads: SCE/POSIX bytes, order, EOF, cursor, validation, partial error and pipe ESPIPE verified");
    return 0;
}
