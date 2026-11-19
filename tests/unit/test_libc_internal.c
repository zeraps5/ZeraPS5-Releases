/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/runtime.h"
#include "hle/hle.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "libc line %d: %s\n", __LINE__, #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static ps5_runtime rt;
static module_set ms;
static guest_thread thread;
static guest_addr page;
static uint64_t invoke(const char *lib, const char *name, uint64_t a, uint64_t b, uint64_t d,
                       uint64_t e) {
    const hle_export *exp = hle_find(lib, nid_hash_name(name));
    CHECK(exp && exp->fn);
    hle_args args = {.gpr = {a, b, d, e}};
    hle_call c = {.rt = &rt, .thread = &thread, .mem = &rt.mem, .a = &args};
    exp->fn(&c);
    return args.ret[0];
}
#define C(n, a, b, d, e) invoke("libSceLibcInternal", n, a, b, d, e)
#define K(n, a, b, d, e) invoke("libkernel", n, a, b, d, e)
static void put(guest_addr a, const void *p, size_t n) {
    CHECK(gmem_write(&rt.mem, a, p, n));
}
static void eq(guest_addr a, const void *p, size_t n) {
    void *v = malloc(n);
    CHECK(v && gmem_read(&rt.mem, a, v, n) && !memcmp(v, p, n));
    free(v);
}
int main(void) {
    CHECK(pal_init() == PS5C_OK && hle_registry_init() == PS5C_OK);
    module_set_init(&ms);
    rt.ms = &ms;
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    CHECK(gmem_init(&rt.mem, &cfg) == PS5C_OK);
    CHECK(pal_mutex_create(&rt.threads_lock) == PS5C_OK);
    CHECK(gmem_map(&rt.mem, 0, 2 * GUEST_PAGE_SIZE, GPROT_RW, 0, "libc fixture", &page) == PS5C_OK);
    thread.errno_addr = page + 2048;
    put(page, "Quake III", 10);
    CHECK(C("strlen", page, 0, 0, 0) == 9);
    CHECK(C("strcpy", page + 32, page, 0, 0) == page + 32);
    eq(page + 32, "Quake III", 10);
    CHECK(C("strchr", page, 'I', 0, 0) == page + 6 && C("strrchr", page, 'I', 0, 0) == page + 8);
    CHECK(C("memmove", page + 1, page, 5, 0) == page + 1);
    eq(page, "QQuakeIII", 9);
    put(page, "abcdef", 7);
    CHECK(C("strncpy", page + 32, page, 3, 0) == page + 32);
    eq(page + 32, "abc", 3);
    put(page + 64, "x", 2);
    CHECK(C("strncpy", page + 32, page + 64, 8, 0) == page + 32);
    eq(page + 32, "x\0\0\0\0\0\0\0", 8);
    CHECK(C("memcpy", UINT64_MAX, UINT64_MAX, 0, 0) == UINT64_MAX);
    put(page + 96, "4294967296!", 12);
    CHECK(C("strtol", page + 96, page + 120, 10, 0) == 4294967296ull);
    uint64_t end;
    CHECK(gmem_read_u64(&rt.mem, page + 120, &end) && end == page + 106);
    /* Bounded comparisons may end on an unmapped page without a NUL. */
    put(page + GUEST_PAGE_SIZE - 3, "abc", 3);
    CHECK(gmem_protect(&rt.mem, page + GUEST_PAGE_SIZE, GUEST_PAGE_SIZE, GPROT_NONE) == PS5C_OK);
    CHECK(C("strnlen", page + GUEST_PAGE_SIZE - 3, 3, 0, 0) == 3);
    CHECK((int64_t)C("strncmp", page, page + GUEST_PAGE_SIZE - 3, 3, 0) == 0);
    put(page + 200, "A", 2);
    uint32_t canary = 0xaaaaaaaa;
    put(page + 210, &canary, 4);
    CHECK(C("mbtowc", page + 210, page + 200, 1, 0) == 1);
    const uint8_t wide[] = {65, 0, 0xaa, 0xaa};
    eq(page + 210, wide, 4);
    const uint16_t wide_string[] = {65, 66, 0};
    put(page + 220, wide_string, 6);
    CHECK(C("wcslen", page + 220, 0, 0, 0) == 2);
    const hle_export *powexp = hle_find("libSceLibcInternal", nid_hash_name("powf"));
    hle_args math = {0};
    float x = 2, y = 3, result;
    memcpy(&math.xmm[0][0], &x, 4);
    memcpy(&math.xmm[1][0], &y, 4);
    hle_call mc = {.a = &math};
    powexp->fn(&mc);
    memcpy(&result, &math.ret_xmm[0][0], 4);
    CHECK(result == 8);
    /* FP input uses XMM0, while both output pointers use GP registers 0/1. */
    hle_args sc={.gpr={page+740,page+756}};double angle=0.75,sine,cosine;
    memcpy(&sc.xmm[0][0],&angle,8);
    hle_call scc={.a=&sc,.mem=&rt.mem};
    const hle_export *sce=hle_find("libSceLibcInternal",nid_hash_name("sincos"));CHECK(sce && sce->fn);
    sce->fn(&scc);CHECK(gmem_read(&rt.mem,page+740,&sine,8) && gmem_read(&rt.mem,page+756,&cosine,8));
    CHECK(fabs(sine-sin(angle))<1e-14 && fabs(cosine-cos(angle))<1e-14);
    float fa=-0.5f,fs,fc;memcpy(&sc.xmm[0][0],&fa,4);
    sce=hle_find("libSceLibcInternal",nid_hash_name("sincosf"));CHECK(sce && sce->fn);sce->fn(&scc);
    CHECK(gmem_read(&rt.mem,page+740,&fs,4) && gmem_read(&rt.mem,page+756,&fc,4));
    CHECK(fabsf(fs-sinf(fa))<1e-6f && fabsf(fc-cosf(fa))<1e-6f);
    guest_addr a = C("calloc", 8, 16, 0, 0);
    CHECK(a && !(a & 15));
    uint8_t zero[128] = {0};
    eq(a, zero, 128);
    put(a, "persistent", 11);
    CHECK(C("realloc", a, UINT64_MAX, 0, 0) == 0);
    eq(a, "persistent", 11);
    guest_addr b = C("realloc", a, 4096, 0, 0);
    CHECK(b);
    eq(b, "persistent", 11);
    CHECK(!gmem_is_mapped(&rt.mem, a, 1));
    C("free", b, 0, 0, 0);
    CHECK(!gmem_is_mapped(&rt.mem, b, 1));
    CHECK(C("posix_memalign", page + 128, 0x200000, 123, 0) == 0);
    CHECK(gmem_read_u64(&rt.mem, page + 128, &a) && !(a & 0x1fffff));
    C("free", a, 0, 0, 0);
    put(page + 256, "Q3_TEST", 8);
    put(page + 272, "one", 4);
    put(page + 288, "two", 4);
    CHECK(C("getenv", page + 256, 0, 0, 0) == 0 && C("setenv", page + 256, page + 272, 1, 0) == 0);
    CHECK(C("setenv", page + 256, page + 288, 0, 0) == 0);
    eq(C("getenv", page + 256, 0, 0, 0), "one", 4);
    CHECK(C("setenv", page + 256, page + 288, 1, 0) == 0);
    eq(C("getenv", page + 256, 0, 0, 0), "two", 4);
    CHECK(C("unsetenv", page + 256, 0, 0, 0) == 0 && C("getenv", page + 256, 0, 0, 0) == 0);
    put(page + 320, "%s:%d", 6);
    CHECK(C("asprintf", page + 352, page + 320, page + 272, 3) == 5);
    CHECK(gmem_read_u64(&rt.mem, page + 352, &a));
    eq(a, "one:3", 6);
    C("free", a, 0, 0, 0);
    /* FILE operations must use the guest VFS, not leak host FILE addresses. */
    pal_mkdir("libc-fixture");
    CHECK(hle_fs_mount("/libc-fixture", "libc-fixture"));
    put(page + 384, "/libc-fixture/io.bin", 20);
    put(page + 416, "w+b", 4);
    guest_addr f = C("fopen", page + 384, page + 416, 0, 0);
    CHECK(f && gmem_is_mapped(&rt.mem, f, 1));
    put(page + 448, "abcdefghijkl", 12);
    CHECK(C("fwrite", page + 448, 4, 3, f) == 3 && C("ftell", f, 0, 0, 0) == 12);
    CHECK(C("fseek", f, (uint64_t)-4, 2, 0) == 0 && C("fread", page + 480, 4, 1, f) == 1);
    eq(page + 480, "ijkl", 4);
    CHECK(C("fread", page + 480, 1, 1, f) == 0 && C("feof", f, 0, 0, 0) == 1);
    CHECK(C("fseek", f, 0, 0, 0) == 0 && C("feof", f, 0, 0, 0) == 0 &&
          C("fclose", f, 0, 0, 0) == 0);
    CHECK(K("unlink", page + 384, 0, 0, 0) == 0);
    hle_fs_unmount("/libc-fixture");
    CHECK(pal_rmdir("libc-fixture") == PS5C_OK);
    /* Native SDK's stderr capture: a FILE wraps one pipe end. */
    CHECK(K("pipe", page + 512, 0, 0, 0) == 0);
    int pipes[2];
    CHECK(gmem_read(&rt.mem, page + 512, pipes, 8));
    put(page + 528, "w", 2);
    f = C("fdopen", (uint64_t)pipes[1], page + 528, 0, 0);
    CHECK(f);
    CHECK(C("fprintf", f, page + 320, page + 272, 3) == 5);
    CHECK(K("read", (uint64_t)pipes[0], page + 544, 5, 0) == 5);
    eq(page + 544, "one:3", 5);
    CHECK(C("fclose", f, 0, 0, 0) == 0 && K("read", (uint64_t)pipes[0], page + 544, 1, 0) == 0);
    CHECK(K("close", (uint64_t)pipes[0], 0, 0, 0) == 0);
    CHECK(K("sysconf", 47, 0, 0, 0) == GUEST_PAGE_SIZE);
    CHECK(C("gethostname", page + 600, 64, 0, 0) == 0);
    eq(page + 600, "ZeraPS5", 8);
    /* A connect userinfo exceeds 256 bytes; its trailing challenge must survive. */
    char userinfo[1024];memset(userinfo,'a',900);memcpy(userinfo+900,"\\challenge\\123456",18);
    put(page+0x1000,userinfo,918);put(page+700,"%s",3);
    CHECK(C("snprintf",page+0x2000,1024,page+700,page+0x1000)==917);
    eq(page+0x2000,userinfo,918);
    CHECK(C("snprintf",page+0x2000,32,page+700,page+0x1000)==917);
    char truncated[32];memset(truncated,'a',31);truncated[31]=0;eq(page+0x2000,truncated,32);
    put(page+700,"[%8.3s]",8);CHECK(C("snprintf",page+0x2000,32,page+700,page+0x1000)==10);
    eq(page+0x2000,"[     aaa]",11);
    put(page+700,"[%-8.3s]",9);CHECK(C("snprintf",page+0x2000,32,page+700,page+0x1000)==10);
    eq(page+0x2000,"[aaa     ]",11);
    hle_libc_reset(&rt);
    gmem_free(&rt.mem);
    pal_mutex_destroy(rt.threads_lock);
    module_set_free(&ms);
    hle_registry_free();
    pal_shutdown();
    puts("firmware libc: guest pointers, bounded strings, 16-bit wchar, numeric ABI, heap "
         "lifetime, VFS streams, stderr pipe and environment verified");
    return 0;
}
