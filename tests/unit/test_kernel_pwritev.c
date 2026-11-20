/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/runtime.h"
#include "hle/hle.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "pwritev line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static ps5_runtime rt;
static module_set ms;
static guest_thread thread;
static guest_addr page;
typedef struct guest_iov { uint64_t base, len; } guest_iov;

static int64_t call(const char *lib, const char *name, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    const hle_export *exp = hle_find(lib, nid_hash_name(name)); CHECK(exp && exp->fn);
    hle_args args = {.gpr = {a, b, c, d}};
    hle_call context = {.rt = &rt, .mem = &rt.mem, .thread = &thread, .a = &args};
    exp->fn(&context); return (int64_t)args.ret[0];
}
#define K(n, a, b, c, d) call("libkernel", n, a, b, c, d)
static void put(guest_addr a, const void *p, size_t n) { CHECK(gmem_write(&rt.mem, a, p, n)); }
static void contents(int fd, const char *expected) {
    char actual[12];
    CHECK(K("pread", fd, page + 0x7000, sizeof(actual), 0) == sizeof(actual));
    CHECK(gmem_read(&rt.mem, page + 0x7000, actual, sizeof(actual)));
    CHECK(!memcmp(actual, expected, sizeof(actual)));
}
static void error(const char *lib, const char *name, int fd, guest_addr vec, int count, int64_t offset, int code) {
    int sentinel = 777, seen;
    put(thread.errno_addr, &sentinel, sizeof(sentinel));
    bool sce = !strcmp(name, "sceKernelPwritev");
    CHECK(call(lib, name, (uint64_t)(int64_t)fd, vec, (uint64_t)(int64_t)count, (uint64_t)offset) ==
          (sce ? (int64_t)(int32_t)(0x80020000u | (uint32_t)code) : -1));
    CHECK(gmem_read(&rt.mem, thread.errno_addr, &seen, sizeof(seen)));
    CHECK(seen == (sce ? sentinel : code));
}
int main(void) {
    CHECK(pal_init() == PS5C_OK && hle_registry_init() == PS5C_OK);
    module_set_init(&ms); rt.ms = &ms;
    gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&rt.mem, &cfg) == PS5C_OK);
    CHECK(gmem_map(&rt.mem, 0, 4 * GUEST_PAGE_SIZE, GPROT_RW, 0, "pwritev fixture", &page) == PS5C_OK);
    thread.errno_addr = page + 128;
    const char *fixture = "test-kernel-pwritev-fixture.bin";
    FILE *file = fopen(fixture, "wb");
    CHECK(file && fwrite("abcdefghijkl", 1, 12, file) == 12 && fclose(file) == 0);
    put(page, fixture, strlen(fixture) + 1);
    int fd = (int)K("open", page, 2, 0, 0); CHECK(fd >= 3);
    guest_addr vec = page + 256, first = page + 0x6000, second = first + 64;
    const char *libs[] = {"libkernel", "libkernel", "libScePosix"};
    const char *names[] = {"sceKernelPwritev", "pwritev", "pwritev"};
    for (unsigned api = 0; api < PS5C_ARRAY_COUNT(names); api++) {
        const char *lib = libs[api], *name = names[api];
        put(first, "abcdefghijkl", 12); CHECK(K("pwrite", fd, first, 12, 0) == 12);
        put(first, "ABC", 3); put(second, "DEF", 3);
        CHECK(K("lseek", fd, 5, 0, 0) == 5);
        guest_iov vectors[3] = {{UINT64_MAX, 0}, {first, 3}, {second, 3}};
        put(vec, vectors, sizeof(vectors));
        CHECK(call(lib, name, fd, vec, 3, 2) == 6);
        contents(fd, "abABCDEFijkl");
        CHECK(K("lseek", fd, 0, 1, 0) == 5);
        error(lib, name, fd, vec, 3, -1, 22);
        error(lib, name, -1, vec, 3, 0, 9);
        error(lib, name, fd, UINT64_MAX, 1, 0, 14);
        error(lib, name, fd, page + 4 * GUEST_PAGE_SIZE - 8, 1, 0, 14);
        error(lib, name, fd, vec, -1, 0, 22);
        error(lib, name, fd, vec, 0, 0, 22);
        error(lib, name, fd, vec, 1025, 0, 22);
        vectors[0] = (guest_iov){first, INT64_MAX}; vectors[1] = (guest_iov){second, 1};
        put(vec, vectors, 2 * sizeof(*vectors));
        error(lib, name, fd, vec, 2, 0, 22);
        contents(fd, "abABCDEFijkl");
        vectors[0] = (guest_iov){UINT64_MAX, 4}; put(vec, vectors, sizeof(*vectors));
        error(lib, name, fd, vec, 1, 0, 14);
        vectors[0] = (guest_iov){first, 3}; vectors[1] = (guest_iov){UINT64_MAX, 2};
        put(vec, vectors, 2 * sizeof(*vectors));
        CHECK(call(lib, name, fd, vec, 2, 0) == 3);
        contents(fd, "ABCBCDEFijkl");
        CHECK(K("lseek", fd, 0, 1, 0) == 5);
        guest_iov empty[1024] = {{0}}; put(vec, empty, sizeof(empty));
        CHECK(call(lib, name, fd, vec, 1024, 0) == 0);
        contents(fd, "ABCBCDEFijkl");
        int ends[2]; CHECK(K("pipe", first + 128, 0, 0, 0) == 0);
        CHECK(gmem_read(&rt.mem, first + 128, ends, sizeof(ends)));
        vectors[0] = (guest_iov){first, 3}; put(vec, vectors, sizeof(*vectors));
        error(lib, name, ends[1], vec, 1, 0, 29);
        CHECK(K("close", ends[0], 0, 0, 0) == 0 && K("close", ends[1], 0, 0, 0) == 0);
    }
    CHECK(K("close", fd, 0, 0, 0) == 0 && remove(fixture) == 0);
    gmem_free(&rt.mem); module_set_free(&ms); hle_registry_free(); pal_shutdown();
    puts("pwritev: SCE/POSIX persisted bytes, vector order, cursor, errors and partial writes verified");
    return 0;
}
