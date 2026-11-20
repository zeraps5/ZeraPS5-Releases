/* SPDX-License-Identifier: GPL-2.0-or-later */
/* chdir/getcwd: the root holds the mount points, relative paths follow the
 * working directory. */
#include "core/runtime.h"
#include "hle/hle.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "cwd line %d: %s\n", __LINE__, #x);                                    \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static ps5_runtime rt;
static guest_thread thread;
static guest_addr page;
static int64_t call(const char *name, uint64_t a, uint64_t b) {
    const hle_export *e = hle_find("libkernel", nid_hash_name(name));
    CHECK(e && e->fn);
    hle_args args = {.gpr = {a, b}};
    hle_call c = {.a = &args, .rt = &rt, .mem = &rt.mem, .thread = &thread};
    e->fn(&c);
    return (int64_t)args.ret[0];
}
static int error(void) {
    int value;
    CHECK(gmem_read(&rt.mem, thread.errno_addr, &value, 4));
    return value;
}
static guest_addr str(const char *s) {
    CHECK(gmem_write(&rt.mem, page, s, strlen(s) + 1));
    return page;
}
static int chdir_(const char *s) { return (int)call("chdir", str(s), 0); }
static bool cwd_is(const char *want) {
    char got[64] = {0};
    return call("getcwd", page + 0x100, sizeof(got)) == (int64_t)(page + 0x100) &&
           gmem_read(&rt.mem, page + 0x100, got, sizeof(got) - 1) && !strcmp(got, want);
}
static bool is_dir(const char *s) {
    uint16_t mode = 0;
    return call("stat", str(s), page + 0x200) == 0 && gmem_read(&rt.mem, page + 0x208, &mode, 2) &&
           (mode & 0170000) == 0040000;
}
int main(void) {
    CHECK(pal_init() == PS5C_OK && hle_registry_init() == PS5C_OK);
    module_set ms;
    module_set_init(&ms);
    rt.ms = &ms; /* no main module: /app0 is the current directory */
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    CHECK(gmem_init(&rt.mem, &cfg) == PS5C_OK);
    CHECK(gmem_map(&rt.mem, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "cwd buffers", &page) == PS5C_OK);
    thread.errno_addr = page + 0x400;
    CHECK(cwd_is("/app0"));
    CHECK(chdir_("/") == 0 && cwd_is("/"));
    CHECK(is_dir(".") && is_dir("/") && is_dir("app0"));
    CHECK(chdir_("..") == 0 && cwd_is("/"));
    CHECK(chdir_("app0") == 0 && cwd_is("/app0"));
    CHECK(chdir_("//./") == 0 && cwd_is("/"));
    CHECK(chdir_("/no-such-mount") == -1 && error() == 2);
    CHECK(chdir_("") == -1 && error() == 2);
    CHECK(chdir_("/app0") == 0 && cwd_is("/app0"));
    puts("chdir/getcwd: root, relative paths from the root and missing paths verified");
    return 0;
}
