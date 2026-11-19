/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>

#include "base/arena.h"
#include "base/dynarray.h"
#include "base/endian.h"
#include "base/hashmap.h"
#include "base/ini.h"
#include "base/json.h"
#include "base/log.h"
#include "base/sha1.h"
#include "test.h"

static void hex(const uint8_t *d, size_t n, char *out) {
    static const char k[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = k[d[i] >> 4];
        out[i * 2 + 1] = k[d[i] & 15];
    }
    out[n * 2] = '\0';
}

static void test_sha1_vectors(void) {
    uint8_t d[20];
    char s[41];
    sha1("abc", 3, d);
    hex(d, 20, s);
    CHECK(strcmp(s, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0);

    sha1("", 0, d);
    hex(d, 20, s);
    CHECK(strcmp(s, "da39a3ee5e6b4b0d3255bfef95601890afd80709") == 0);

    const char *two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha1(two_blocks, strlen(two_blocks), d);
    hex(d, 20, s);
    CHECK(strcmp(s, "84983e441c3bd26ebaae4aa1f95129e5e54670f1") == 0);
}

static void test_sha1_incremental(void) {
    /* Feeding byte by byte must match a single update. */
    const char *msg = "The quick brown fox jumps over the lazy dog";
    uint8_t a[20], b[20];
    sha1(msg, strlen(msg), a);
    sha1_ctx ctx;
    sha1_init(&ctx);
    for (size_t i = 0; msg[i]; i++) {
        sha1_update(&ctx, &msg[i], 1);
    }
    sha1_final(&ctx, b);
    CHECK(memcmp(a, b, 20) == 0);
}

static void test_endian(void) {
    uint8_t buf[9] = {0};
    wr_le64(buf + 1, 0x1122334455667788ull); /* unaligned */
    CHECK(buf[1] == 0x88 && buf[8] == 0x11);
    CHECK_EQ_U64(rd_le64(buf + 1), 0x1122334455667788ull);
    CHECK_EQ_U64(rd_le32(buf + 1), 0x55667788u);
    CHECK_EQ_U64(rd_le16(buf + 1), 0x7788u);
    wr_le16(buf, 0xBEEF);
    CHECK_EQ_U64(rd_le16(buf), 0xBEEFu);
}

static void test_arena_alignment(void) {
    arena a;
    arena_init(&a, 256);
    for (size_t align = 1; align <= 4096; align <<= 1) {
        uint8_t *p = (uint8_t *)arena_alloc(&a, 3, align);
        CHECK(p != NULL);
        CHECK(((uintptr_t)p & (align - 1)) == 0);
        p[0] = p[2] = 0xAA;
    }
    CHECK(arena_alloc(&a, 8, 3) == NULL); /* non power-of-two alignment */
    arena_free(&a);
}

static void test_arena_many_and_large(void) {
    arena a;
    arena_init(&a, 1024);
    uint32_t *ptrs[1000];
    for (uint32_t i = 0; i < 1000; i++) {
        ptrs[i] = ARENA_NEW(&a, uint32_t);
        CHECK(ptrs[i] != NULL);
        *ptrs[i] = i;
    }
    for (uint32_t i = 0; i < 1000; i++) {
        CHECK(*ptrs[i] == i); /* nothing got overwritten across chunks */
    }
    uint8_t *big = (uint8_t *)arena_alloc(&a, 1u << 20, 16); /* bigger than a chunk */
    CHECK(big != NULL);
    memset(big, 0x5A, 1u << 20);
    char *s = arena_strdup(&a, "prospero");
    CHECK(s && strcmp(s, "prospero") == 0);
    arena_free(&a);
}

static void test_arena_mark_reset(void) {
    arena a;
    arena_init(&a, 128);
    void *keep = arena_alloc(&a, 16, 8);
    CHECK(keep != NULL);
    arena_mark m = arena_get_mark(&a);
    size_t used_at_mark = a.bytes_used;
    for (int i = 0; i < 100; i++) {
        CHECK(arena_alloc(&a, 64, 8) != NULL);
    }
    CHECK(a.bytes_used > used_at_mark);
    arena_reset_to(&a, m);
    CHECK(a.bytes_used == used_at_mark);
    /* The next allocation reuses the space right after `keep`. */
    uint8_t *next = (uint8_t *)arena_alloc(&a, 16, 8);
    CHECK(next == (uint8_t *)keep + 16);

    arena_reset(&a);
    CHECK(a.bytes_used == 0);
    CHECK(arena_alloc(&a, 16, 8) != NULL);
    arena_free(&a);
}

static void test_dynarray(void) {
    DYNARRAY(uint64_t) v = {0};
    for (uint64_t i = 0; i < 1000; i++) {
        CHECK(dyn_push(&v, i * 3));
    }
    CHECK(v.len == 1000);
    CHECK(v.cap >= 1000);
    for (uint64_t i = 0; i < 1000; i++) {
        CHECK(v.data[i] == i * 3);
    }
    CHECK(dyn_last(&v) == 999 * 3);
    CHECK(dyn_pop(&v) == 999 * 3);
    CHECK(v.len == 999);
    CHECK(dyn_reserve(&v, 5000));
    CHECK(v.cap >= 5000 && v.len == 999);
    dyn_free(&v);
    CHECK(v.data == NULL && v.len == 0 && v.cap == 0);
}

static void test_hashmap_basic(void) {
    hmap64 m;
    hmap64_init(&m);
    uint64_t out = 0;
    CHECK(!hmap64_get(&m, 1, &out)); /* empty map */

    CHECK(hmap64_put(&m, 0, 100));          /* key 0 is a valid key */
    CHECK(hmap64_put(&m, UINT64_MAX, 200)); /* and so is all-ones */
    CHECK(hmap64_get(&m, 0, &out) && out == 100);
    CHECK(hmap64_get(&m, UINT64_MAX, &out) && out == 200);
    CHECK(hmap64_put(&m, 0, 101)); /* replace */
    CHECK(hmap64_get(&m, 0, &out) && out == 101);
    CHECK(m.len == 2);
    hmap64_free(&m);
}

static void test_hashmap_bulk_and_remove(void) {
    hmap64 m;
    hmap64_init(&m);
    const uint64_t n = 20000;
    for (uint64_t i = 0; i < n; i++) {
        CHECK(hmap64_put(&m, i * 0x9E3779B97F4A7C15ull, i));
    }
    CHECK(m.len == n);
    for (uint64_t i = 0; i < n; i += 2) {
        CHECK(hmap64_remove(&m, i * 0x9E3779B97F4A7C15ull));
    }
    CHECK(m.len == n / 2);
    uint64_t out;
    for (uint64_t i = 0; i < n; i++) {
        bool found = hmap64_get(&m, i * 0x9E3779B97F4A7C15ull, &out);
        CHECK(found == (i % 2 == 1));
        if (found) {
            CHECK(out == i);
        }
    }
    /* Reinsert over tombstones. */
    for (uint64_t i = 0; i < n; i += 2) {
        CHECK(hmap64_put(&m, i * 0x9E3779B97F4A7C15ull, i + 1));
    }
    CHECK(m.len == n);
    CHECK(hmap64_get(&m, 4 * 0x9E3779B97F4A7C15ull, &out) && out == 5);
    CHECK(!hmap64_remove(&m, 12345));
    hmap64_free(&m);
}

typedef struct {
    char last[256];
    int count;
} capture;

static void capture_sink(void *user, log_level level, const char *line, size_t len) {
    capture *c = (capture *)user;
    PS5C_UNUSED(level);
    size_t n = PS5C_MIN(len, sizeof(c->last) - 1);
    memcpy(c->last, line, n);
    c->last[n] = '\0';
    c->count++;
}

static void test_log_levels_and_format(void) {
    capture cap = {{0}, 0};
    log_set_sink(capture_sink, &cap);
    log_set_level_all(LOG_LVL_WARN);

    LOG_I(LOADER, "hidden %d", 1);
    CHECK(cap.count == 0);
    LOG_W(LOADER, "shown %d", 2);
    CHECK(cap.count == 1);
    CHECK(strcmp(cap.last, "[WARN ] [LOADER] shown 2\n") == 0);

    log_set_level(LOG_CAT_GPU, LOG_LVL_TRACE);
    LOG_T(GPU, "packet 0x%x", 0x10);
    CHECK(cap.count == 2);
    CHECK(strstr(cap.last, "[GPU] packet 0x10") != NULL);
    LOG_T(CPU, "still hidden");
    CHECK(cap.count == 2);

    log_set_sink(NULL, NULL);
    log_set_level_all(LOG_LVL_INFO);
}

static void test_json_compact(void) {
    json_writer w;
    json_init(&w, false);
    json_begin_object(&w);
    json_kv_string(&w, "name", "a\"b\\c\n\x01");
    json_kv_u64(&w, "n", 42);
    json_key(&w, "list");
    json_begin_array(&w);
    json_i64(&w, -7);
    json_bool(&w, true);
    json_null(&w);
    json_string(&w, NULL);
    json_begin_object(&w);
    json_end_object(&w);
    json_end_array(&w);
    json_kv_hex(&w, "addr", 0x800000000ull);
    json_end_object(&w);
    const char *doc = json_result(&w, NULL);
    CHECK(doc != NULL);
    CHECK(strcmp(doc, "{\"name\":\"a\\\"b\\\\c\\n\\u0001\",\"n\":42,\"list\":[-7,true,null,null,{}],"
                      "\"addr\":\"0x800000000\"}") == 0);
    json_free(&w);
}

static void test_json_pretty_and_misuse(void) {
    json_writer w;
    json_init(&w, true);
    json_begin_object(&w);
    json_key(&w, "a");
    json_begin_array(&w);
    json_u64(&w, 1);
    json_u64(&w, 2);
    json_end_array(&w);
    json_end_object(&w);
    CHECK(strcmp(json_result(&w, NULL), "{\n  \"a\": [\n    1,\n    2\n  ]\n}") == 0);
    json_free(&w);

    json_init(&w, false);
    json_begin_object(&w);
    CHECK(json_result(&w, NULL) == NULL); /* unclosed */
    json_end_object(&w);
    json_end_object(&w); /* one too many */
    CHECK(json_result(&w, NULL) == NULL);
    json_free(&w);
}

static void test_ini(void) {
    static const char text[] = "\xEF\xBB\xBF; comment\r\n[Geral]\nidioma = 17\n\n[pastas]\njogos1=games\njogos2=D:\\Jogos PS5\n";
    ini *f = ini_parse(text, sizeof(text) - 1);
    CHECK(f != NULL);
    CHECK(ini_get_int(f, "geral", "IDIOMA", 0) == 17); /* names without case */
    CHECK(strcmp(ini_get(f, "pastas", "jogos2"), "D:\\Jogos PS5") == 0);
    CHECK(ini_get(f, "geral", "resolucao") == NULL);
    const char *keys[4];
    CHECK(ini_keys(f, "pastas", keys, 4) == 2 && strcmp(keys[1], "jogos2") == 0);
    CHECK(ini_set(f, "geral", "resolucao", "2160")); /* added after the section's last key */
    CHECK(ini_set(f, "pastas", "jogos1", NULL));    /* removed */
    CHECK(ini_set(f, "teclado", "cruz", "SPACE,Z")); /* new section at the end */
    char *out = ini_text(f, NULL);
    CHECK(out && strcmp(out, "; comment\r\n[Geral]\r\nidioma = 17\r\nresolucao=2160\r\n\r\n[pastas]\r\n"
                             "jogos2=D:\\Jogos PS5\r\n\r\n[teclado]\r\ncruz=SPACE,Z\r\n") == 0);
    free(out);
    ini_free(f);
}

static void test_error_strings(void) {
    CHECK(strcmp(ps5c_err_str(PS5C_OK), "ok") == 0);
    CHECK(strcmp(ps5c_err_str(PS5C_ERR_NOT_FOUND), "not found") == 0);
    CHECK(strcmp(ps5c_err_str(-12345), "unknown error") == 0);
}

int main(void) {
    static const test_case tests[] = {
        TEST_CASE(test_sha1_vectors),     TEST_CASE(test_sha1_incremental),
        TEST_CASE(test_endian),           TEST_CASE(test_arena_alignment),
        TEST_CASE(test_arena_many_and_large), TEST_CASE(test_arena_mark_reset),
        TEST_CASE(test_dynarray),         TEST_CASE(test_hashmap_basic),
        TEST_CASE(test_hashmap_bulk_and_remove), TEST_CASE(test_log_levels_and_format),
        TEST_CASE(test_json_compact),     TEST_CASE(test_json_pretty_and_misuse),
        TEST_CASE(test_ini),              TEST_CASE(test_error_strings),
    };
    return test_run_all("base", tests, PS5C_ARRAY_COUNT(tests));
}
