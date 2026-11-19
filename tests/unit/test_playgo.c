/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/runtime.h"
#include "hle/hle.h"
#include "hle/playgo_metadata.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include "test.h"
#include <string.h>

#define ERR(n) ((int32_t)(0x80b20000u + (n)))
static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (i * 8));
}
static void put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (i * 8));
}
/* Synthetic PLGX: chunk 0 requires two microchunks (3+5 bytes), chunk 1
 * has no payload, chunk 2 requires microchunk 1. Never commercial game data. */
static void metadata(uint8_t data[416]) {
    memset(data, 0, 416);
    put32(data, 0x78676c70);
    put32(data + 4, 0x1000);
    data[8] = 1;
    data[10] = 3;
    put32(data + 16, 416);
    put32(data + 32, 2);
    const uint32_t sections[8] = {256, 96, 352, 12, 364, 6, 384, 32};
    for (size_t i = 0; i < 8; i++)
        put32(data + 192 + i * 4, sections[i]);
    for (int i = 0; i < 3; i++) {
        data[256 + i * 32 + 2] = 3;
        put32(data + 256 + i * 32 + 28, (uint32_t)i * 2);
    }
    put32(data + 260, 2);
    put64(data + 272, 0x4000000000000000ull);
    put32(data + 320 + 4, 1);
    put32(data + 320 + 24, 8);
    put32(data + 352, 0);
    put32(data + 356, 1);
    put32(data + 360, 1);
    memcpy(data + 364, "a\0b\0c", 6);
    put64(data + 384 + 8, 3);
    put64(data + 400, 3);
    put64(data + 400 + 8, 5);
}
static void test_plgx(void) {
    uint8_t data[416];
    metadata(data);
    playgo_metadata m = {0};
    CHECK(playgo_metadata_parse(data, sizeof(data), &m));
    CHECK(m.count == 3 && m.chunks[0].size == 8 && m.chunks[1].size == 0 && m.chunks[2].size == 5);
    CHECK(m.chunks[0].required_locus == 3 && m.chunks[0].language_mask == 0x4000000000000000ull);
    /* the mchunk ranges each chunk needs, for an installation backend */
    CHECK(m.nranges == 3 && m.chunks[0].first_range == 0 && m.chunks[0].nranges == 2);
    CHECK(m.chunks[1].nranges == 0 && m.chunks[2].first_range == 2 && m.chunks[2].nranges == 1);
    CHECK(m.ranges[0].offset == 0 && m.ranges[0].length == 3 && m.ranges[1].offset == 3 && m.ranges[1].length == 5);
    CHECK(m.ranges[2].offset == 3 && m.ranges[2].length == 5);
    playgo_metadata_free(&m);
}
static void test_malformed_plgx(void) {
    uint8_t data[416];
    playgo_metadata m = {0};
    metadata(data);
    CHECK(!playgo_metadata_parse(data, 415, &m));
    metadata(data);
    put32(data, 0x6f676c70);
    CHECK(!playgo_metadata_parse(data, 416, &m));
    metadata(data);
    put32(data + 196, UINT32_MAX);
    CHECK(!playgo_metadata_parse(data, 416, &m));
    metadata(data);
    put32(data + 208, 256);
    CHECK(!playgo_metadata_parse(data, 416, &m));
    metadata(data);
    put32(data + 260, UINT32_MAX);
    CHECK(!playgo_metadata_parse(data, 416, &m));
    metadata(data);
    put32(data + 280, 1);
    CHECK(!playgo_metadata_parse(data, 416, &m));
    metadata(data);
    put32(data + 352, 2);
    CHECK(!playgo_metadata_parse(data, 416, &m));
    metadata(data);
    put32(data + 284, 6);
    CHECK(!playgo_metadata_parse(data, 416, &m));
    metadata(data);
    memset(data + 364, 'x', 6);
    CHECK(!playgo_metadata_parse(data, 416, &m));
    CHECK(m.count == 0 && !m.chunks);
}
typedef struct fixture {
    ps5_runtime rt;
    module_set ms;
    guest_addr page;
    uint8_t loci[3];
    bool known[3];
} fixture;
static fixture f;
/* Synthetic installation evidence, independent of PLGX requirements. */
static bool query_locus(void *user, uint16_t id, uint8_t *out) {
    fixture *state = user;
    if (id >= 3 || !state->known[id])
        return false;
    *out = state->loci[id];
    return true;
}
static int32_t call_rt(ps5_runtime *rt, const char *name, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d) {
    const hle_export *exp = hle_find("libScePlayGo", nid_hash_name(name));
    if (!exp || !exp->fn)
        return INT32_MIN;
    hle_args args = {.gpr = {a, b, c, d}};
    hle_call context = {.rt = rt, .mem = &rt->mem, .a = &args};
    exp->fn(&context);
    return (int32_t)args.ret[0];
}
#define CALL(name, a, b, c, d) call_rt(&f.rt, "scePlayGo" name, a, b, c, d)
static void setup(void) {
    memset(&f, 0, sizeof(f));
    f.rt.ms = &f.ms;
    memset(f.known, 1, sizeof(f.known));
    f.known[1] = false; /* Empty chunks require no installation evidence. */
    f.rt.opts.playgo_query_locus = query_locus;
    f.rt.opts.playgo_query_user = &f;
    CHECK(hle_registry_init() == PS5C_OK);
    CHECK(pal_mutex_create(&f.rt.threads_lock) == PS5C_OK);
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    CHECK(gmem_init(&f.rt.mem, &cfg) == PS5C_OK);
    CHECK(gmem_map(&f.rt.mem, GUEST_MODULE_BASE + 16 * GUEST_PAGE_SIZE,
                   (2u << 20) + GUEST_PAGE_SIZE, GPROT_RW, GMAP_FIXED, "PlayGo test",
                   &f.page) == PS5C_OK);
    uint8_t data[416];
    metadata(data);
    pal_mkdir("playgo-fixture");
    pal_mkdir("playgo-fixture/sce_sys");
    pal_file *file = NULL;
    uint64_t written = 0;
    CHECK(pal_file_open("playgo-fixture/sce_sys/playgo-chunk.dat",
                        PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE, &file) == PS5C_OK);
    CHECK(pal_file_write(file, data, sizeof(data), &written) == PS5C_OK && written == sizeof(data));
    pal_file_close(file);
    CHECK(hle_fs_mount("/app0", "playgo-fixture"));
    struct {
        uint64_t buffer;
        uint32_t size, reserved;
    } p = {f.page + GUEST_PAGE_SIZE, 2u << 20, 0};
    CHECK(gmem_write(&f.rt.mem, f.page, &p, sizeof(p)));
}
static void teardown(void) {
    hle_playgo_reset(&f.rt);
    pal_mutex_destroy(f.rt.threads_lock);
    gmem_free(&f.rt.mem);
    hle_fs_unmount("/app0");
    pal_file_remove("playgo-fixture/sce_sys/playgo-chunk.dat");
    hle_registry_free();
}
static uint32_t open_handle(void) {
    if (CALL("Initialize", f.page, 0, 0, 0) || CALL("Open", f.page + 32, 0, 0, 0))
        return 0;
    uint32_t h = 0;
    return gmem_read(&f.rt.mem, f.page + 32, &h, 4) ? h : 0;
}
static void test_lifecycle(void) {
    setup();
    CHECK(CALL("Open", f.page + 32, 0, 0, 0) == ERR(5));
    CHECK(CALL("Initialize", 0, 0, 0, 0) == ERR(10));
    uint32_t small = 16;
    CHECK(gmem_write(&f.rt.mem, f.page + 8, &small, 4));
    CHECK(CALL("Initialize", f.page, 0, 0, 0) == ERR(11));
    small = 2u << 20;
    CHECK(gmem_write(&f.rt.mem, f.page + 8, &small, 4));
    uint32_t h = open_handle();
    CHECK(h);
    CHECK(CALL("Initialize", f.page, 0, 0, 0) == ERR(6));
    CHECK(CALL("Open", f.page + 32, 1, 0, 0) == ERR(4));
    CHECK(CALL("GetChunkId", h + 1, 0, 0, f.page + 40) == ERR(9));
    CHECK(CALL("Close", h, 0, 0, 0) == 0);
    CHECK(CALL("GetChunkId", h, 0, 0, f.page + 40) == ERR(9));
    CHECK(CALL("Open", f.page + 32, 0, 0, 0) == 0);
    uint32_t h2 = 0;
    CHECK(gmem_read(&f.rt.mem, f.page + 32, &h2, 4) && h2 != h);
    CHECK(CALL("Terminate", 0, 0, 0, 0) == 0);
    CHECK(CALL("Terminate", 0, 0, 0, 0) == ERR(5));
    CHECK(CALL("Initialize", f.page, 0, 0, 0) == 0);
    CHECK(CALL("Open", f.page + 32, 0, 0, 0) == 0);
    CHECK(gmem_read(&f.rt.mem, f.page + 32, &h2, 4) && h2 != h);
    teardown();
}
static void test_chunk_outputs(void) {
    setup();
    uint32_t h = open_handle(), n = 0;
    CHECK(h);
    CHECK(CALL("GetChunkId", h, 0, 0, f.page + 40) == 0);
    CHECK(gmem_read(&f.rt.mem, f.page + 40, &n, 4) && n == 3);
    CHECK(CALL("GetChunkId", h, f.page + 64, 2, f.page + 40) == 0);
    uint16_t ids[3] = {99, 99, 99};
    CHECK(gmem_read(&f.rt.mem, f.page + 64, ids, 4) && ids[0] == 0 && ids[1] == 1);
    CHECK(gmem_read(&f.rt.mem, f.page + 40, &n, 4) && n == 2);
    CHECK(CALL("GetChunkId", h, f.page + 64, 1000, f.page + 40) == 0);
    CHECK(gmem_read(&f.rt.mem, f.page + 64, ids, 6) && ids[2] == 2);
    CHECK(CALL("GetLocus", h, f.page + 64, 3, f.page + 80) == 0);
    uint8_t loci[3];
    CHECK(gmem_read(&f.rt.mem, f.page + 80, loci, 3));
    CHECK(loci[0] == 0 && loci[1] == 3 && loci[2] == 0);
    CHECK(CALL("GetProgress", h, f.page + 64, 3, f.page + 96) == 0);
    uint64_t progress[2];
    CHECK(gmem_read(&f.rt.mem, f.page + 96, progress, 16));
    CHECK(progress[0] == 0 && progress[1] == 13);
    /* Count is an array length, not the number of unique chunks. */
    uint16_t repeated[4] = {0, 0, 1, 2};
    CHECK(gmem_write(&f.rt.mem, f.page + 64, repeated, sizeof(repeated)));
    CHECK(CALL("GetLocus", h, f.page + 64, 4, f.page + 80) == 0);
    uint8_t repeated_loci[4];
    CHECK(gmem_read(&f.rt.mem, f.page + 80, repeated_loci, 4));
    CHECK(repeated_loci[0] == 0 && repeated_loci[1] == 0 && repeated_loci[2] == 3 &&
          repeated_loci[3] == 0);

    ids[1] = 3;
    CHECK(gmem_write(&f.rt.mem, f.page + 64, ids, 6));
    CHECK(CALL("GetLocus", h, f.page + 64, 3, f.page + 80) == ERR(12));
    CHECK(CALL("GetLocus", h, f.page + 64, 0, f.page + 80) == ERR(11));
    teardown();
}
static void test_pointer_guards(void) {
    setup();
    uint32_t h = open_handle(), guard = 0xabcdef01, value = 0;
    CHECK(h);
    guest_addr end = f.page + (2u << 20) + GUEST_PAGE_SIZE;
    CHECK(gmem_write(&f.rt.mem, f.page + 40, &guard, 4));
    CHECK(gmem_write(&f.rt.mem, end - 2, &guard, 2));
    CHECK(CALL("GetChunkId", h, end - 2, 3, f.page + 40) == ERR(10));
    CHECK(gmem_read(&f.rt.mem, f.page + 40, &value, 4) && value == guard);
    CHECK(CALL("GetChunkId", h, f.page + 64, 3, end - 2) == ERR(10));
    CHECK(CALL("Open", end - 2, 0, 0, 0) == ERR(10));
    CHECK(CALL("GetLocus", h, 0, 1, f.page + 80) == ERR(10));
    CHECK(CALL("GetToDoList", h, 0, 0, 0) == ERR(10));
    CHECK(gmem_protect(&f.rt.mem, f.page, GUEST_PAGE_SIZE, GPROT_READ) == PS5C_OK);
    CHECK(CALL("GetChunkId", h, 0, 0, f.page + 40) == ERR(10));
    CHECK(CALL("GetLocus", h, f.page + 64, 1, f.page + 80) == ERR(10));
    CHECK(gmem_read(&f.rt.mem, f.page + 40, &value, 4) && value == guard);
    teardown();
}
static void test_pending_and_isolation(void) {
    setup();
    uint32_t h = open_handle();
    CHECK(h);
    uint8_t todo[12] = {0, 0, 3, 0, 1, 0, 3, 0, 2, 0, 2, 0};
    CHECK(gmem_write(&f.rt.mem, f.page + 64, todo, 12));
    CHECK(CALL("SetToDoList", h, f.page + 64, 3, 0) == 0);
    CHECK(CALL("GetToDoList", h, 0, 0, f.page + 40) == 0);
    uint32_t n = 0;
    CHECK(gmem_read(&f.rt.mem, f.page + 40, &n, 4) && n == 2);
    CHECK(CALL("GetToDoList", h, f.page + 80, 1, f.page + 40) == 0);
    uint8_t item[4];
    CHECK(gmem_read(&f.rt.mem, f.page + 80, item, 4) && item[0] == 0 && item[2] == 3);
    todo[2] = 1;
    CHECK(gmem_write(&f.rt.mem, f.page + 64, todo, 4));
    CHECK(CALL("SetToDoList", h, f.page + 64, 1, 0) == ERR(16));
    CHECK(CALL("GetToDoList", h, 0, 0, f.page + 40) == 0);
    CHECK(gmem_read(&f.rt.mem, f.page + 40, &n, 4) && n == 2);
    CHECK(CALL("SetToDoList", h, 0, 0, 0) == 0);
    CHECK(CALL("GetToDoList", h, 0, 0, f.page + 40) == 0);
    CHECK(gmem_read(&f.rt.mem, f.page + 40, &n, 4) && n == 0);
    ps5_runtime other = {0};
    other.mem = f.rt.mem;
    CHECK(pal_mutex_create(&other.threads_lock) == PS5C_OK);
    CHECK(call_rt(&other, "scePlayGoGetChunkId", h, 0, 0, f.page + 40) == ERR(5));
    pal_mutex_destroy(other.threads_lock);
    teardown();
}
static void test_unknown_installation(void) {
    setup();
    CHECK(CALL("Initialize", f.page, 0, 0, 0) == 0);
    uint32_t guard = 0x12345678, value = 0;
    CHECK(gmem_write(&f.rt.mem, f.page + 32, &guard, 4));
    /* nothing to ask (a folder or a plain .exfat dump): every chunk is local */
    f.rt.opts.playgo_query_locus = NULL;
    CHECK(CALL("Open", f.page + 32, 0, 0, 0) == 0);
    uint32_t local = 0;
    CHECK(gmem_read(&f.rt.mem, f.page + 32, &local, 4) && local);
    uint16_t local_id = 2;
    uint8_t locus = 0;
    CHECK(gmem_write(&f.rt.mem, f.page + 64, &local_id, 2));
    CHECK(CALL("GetLocus", local, f.page + 64, 1, f.page + 80) == 0);
    CHECK(gmem_read(&f.rt.mem, f.page + 80, &locus, 1) && locus == 3);
    CHECK(CALL("Close", local, 0, 0, 0) == 0);
    CHECK(gmem_write(&f.rt.mem, f.page + 32, &guard, 4));
    f.rt.opts.playgo_query_locus = query_locus;
    f.known[2] = false;
    CHECK(CALL("Open", f.page + 32, 0, 0, 0) == ERR(14));
    CHECK(gmem_read(&f.rt.mem, f.page + 32, &value, 4) && value == guard);
    f.known[2] = true;
    f.loci[2] = 1;
    CHECK(CALL("Open", f.page + 32, 0, 0, 0) == ERR(14));
    CHECK(gmem_read(&f.rt.mem, f.page + 32, &value, 4) && value == guard);
    f.loci[2] = 0;
    CHECK(CALL("Open", f.page + 32, 0, 0, 0) == 0);
    uint32_t h = 0;
    CHECK(gmem_read(&f.rt.mem, f.page + 32, &h, 4) && h);
    uint16_t id = 2;
    CHECK(gmem_write(&f.rt.mem, f.page + 64, &id, 2));
    CHECK(gmem_write(&f.rt.mem, f.page + 80, &guard, 4));
    f.known[2] = false;
    CHECK(CALL("GetLocus", h, f.page + 64, 1, f.page + 80) == ERR(14));
    CHECK(CALL("GetProgress", h, f.page + 64, 1, f.page + 80) == ERR(14));
    CHECK(gmem_read(&f.rt.mem, f.page + 80, &value, 4) && value == guard);
    /* Losing installation evidence must still allow service cleanup. */
    CHECK(CALL("Close", h, 0, 0, 0) == 0);
    teardown();
}
static void test_installation_changes(void) {
    setup();
    uint32_t h = open_handle();
    CHECK(h);
    uint16_t ids[3] = {0, 1, 2};
    uint8_t todo[8] = {0, 0, 3, 0, 2, 0, 2, 0};
    CHECK(gmem_write(&f.rt.mem, f.page + 64, ids, 6));
    CHECK(gmem_write(&f.rt.mem, f.page + 128, todo, 8));
    CHECK(CALL("SetToDoList", h, f.page + 128, 2, 0) == 0);
    f.loci[0] = 2;
    CHECK(CALL("GetProgress", h, f.page + 64, 3, f.page + 96) == 0);
    uint64_t progress[2];
    CHECK(gmem_read(&f.rt.mem, f.page + 96, progress, 16));
    CHECK(progress[0] == 8 && progress[1] == 13);
    f.loci[2] = 2;
    CHECK(CALL("GetToDoList", h, f.page + 80, 2, f.page + 40) == 0);
    uint32_t n = 0;
    CHECK(gmem_read(&f.rt.mem, f.page + 40, &n, 4) && n == 1);
    uint8_t item[4];
    CHECK(gmem_read(&f.rt.mem, f.page + 80, item, 4) && item[0] == 0 && item[2] == 3);
    f.loci[0] = 3;
    CHECK(CALL("GetToDoList", h, 0, 0, f.page + 40) == 0);
    CHECK(gmem_read(&f.rt.mem, f.page + 40, &n, 4) && n == 0);
    CHECK(CALL("GetLocus", h, f.page + 64, 3, f.page + 80) == 0);
    uint8_t loci[3];
    CHECK(gmem_read(&f.rt.mem, f.page + 80, loci, 3));
    CHECK(loci[0] == 3 && loci[1] == 3 && loci[2] == 2);
    CHECK(CALL("GetProgress", h, f.page + 64, 3, f.page + 96) == 0);
    CHECK(gmem_read(&f.rt.mem, f.page + 96, progress, 16));
    CHECK(progress[0] == 13 && progress[1] == 13);
    teardown();
}
static void test_empty_installation(void) {
    setup();
    uint8_t data[416];
    metadata(data);
    put32(data + 260, 0);
    put32(data + 324, 0);
    pal_file *file = NULL;
    uint64_t written = 0;
    CHECK(pal_file_open("playgo-fixture/sce_sys/playgo-chunk.dat",
                        PAL_FILE_WRITE | PAL_FILE_TRUNCATE, &file) == PS5C_OK);
    CHECK(pal_file_write(file, data, sizeof(data), &written) == PS5C_OK && written == sizeof(data));
    pal_file_close(file);
    f.rt.opts.playgo_query_locus = NULL;
    uint32_t h = open_handle();
    CHECK(h);
    uint16_t ids[3] = {0, 1, 2};
    CHECK(gmem_write(&f.rt.mem, f.page + 64, ids, 6));
    CHECK(CALL("GetLocus", h, f.page + 64, 3, f.page + 80) == 0);
    uint8_t loci[3];
    CHECK(gmem_read(&f.rt.mem, f.page + 80, loci, 3));
    CHECK(loci[0] == 3 && loci[1] == 3 && loci[2] == 3);
    CHECK(CALL("GetProgress", h, f.page + 64, 3, f.page + 96) == 0);
    uint64_t progress[2];
    CHECK(gmem_read(&f.rt.mem, f.page + 96, progress, 16));
    CHECK(progress[0] == 0 && progress[1] == 0);
    teardown();
}
static void test_bad_metadata_open(void) {
    setup();
    CHECK(CALL("Initialize", f.page, 0, 0, 0) == 0);
    uint32_t guard = 0x12345678, value = 0;
    CHECK(gmem_write(&f.rt.mem, f.page + 32, &guard, 4));
    pal_file *file = NULL;
    uint8_t bad = 0;
    uint64_t written = 0;
    CHECK(pal_file_open("playgo-fixture/sce_sys/playgo-chunk.dat",
                        PAL_FILE_WRITE | PAL_FILE_TRUNCATE, &file) == PS5C_OK);
    CHECK(pal_file_write(file, &bad, 1, &written) == PS5C_OK);
    pal_file_close(file);
    CHECK(CALL("Open", f.page + 32, 0, 0, 0) == ERR(14));
    CHECK(gmem_read(&f.rt.mem, f.page + 32, &value, 4) && value == guard);
    teardown();
}
int main(void) {
    pal_init();
    const test_case tests[] = {TEST_CASE(test_plgx),
                               TEST_CASE(test_malformed_plgx),
                               TEST_CASE(test_lifecycle),
                               TEST_CASE(test_chunk_outputs),
                               TEST_CASE(test_pointer_guards),
                               TEST_CASE(test_pending_and_isolation),
                               TEST_CASE(test_unknown_installation),
                               TEST_CASE(test_installation_changes),
                               TEST_CASE(test_empty_installation),
                               TEST_CASE(test_bad_metadata_open)};
    return test_run_all("playgo", tests, PS5C_ARRAY_COUNT(tests));
}
