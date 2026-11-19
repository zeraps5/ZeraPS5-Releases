/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/game_fixes.h"
#include "base/crypto.h"
#include "test.h"

typedef struct fixture {
    gmem mem;
    loaded_module module;
    sce_segment segment;
    compat_patch patch;
    uint8_t file[512], before[512];
    char hash[65];
} fixture;
static const uint8_t original[] = {0xb8, 0xff, 0xff, 0xff, 0xff, 0xc3, 0xcc};
static const uint8_t replacement[] = {0x8b, 0x05, 0xfe, 0x32, 0xdf, 0x00, 0xc3};

static bool setup(fixture *f, bool identity) {
    memset(f, 0, sizeof(*f));
    memset(f->file, 0xcc, sizeof(f->file));
    memcpy(f->file + 0x98, original, sizeof(original));
    memcpy(f->before, f->file, sizeof(f->file));
    uint8_t digest[32];
    sha256(f->file, sizeof(f->file), digest);
    for (unsigned i = 0; i < 32; i++) snprintf(f->hash + i * 2, 3, "%02x", digest[i]);
    f->patch = (compat_patch){"test", "PPSA01880", "01.006.000", "test", "test", f->hash,
                              0x98, original, replacement, sizeof(original), true};
    gmem_config cfg = gmem_default_config();
    cfg.identity = identity;
    if (gmem_init(&f->mem, &cfg) != PS5C_OK) return false;
    guest_addr addr;
    if (gmem_map(&f->mem, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "patch fixture", &addr) != PS5C_OK) return false;
    f->segment.h = (elf64_phdr){.p_type = PT_LOAD, .p_flags = PF_R | PF_X, .p_vaddr = 0x100,
                               .p_filesz = 0x100, .p_memsz = 0x100};
    f->segment.data_off = 0x80;
    f->segment.has_data = true;
    f->module.is_main = f->module.mapped = true;
    f->module.base = f->module.bias = addr;
    f->module.map_size = GUEST_PAGE_SIZE;
    f->module.img.file = f->file;
    f->module.img.file_size = sizeof(f->file);
    f->module.img.segments = &f->segment;
    f->module.img.segment_count = 1;
    return gmem_write(&f->mem, addr + 0x100, f->file + 0x80, 0x100);
}
static compat_patch_status apply(fixture *f, const char *title, const char *version, bool enabled) {
    return game_compat_apply(&f->mem, &f->module, &f->patch, title, version, enabled);
}
static bool unchanged(fixture *f) {
    uint8_t bytes[0x100];
    return !memcmp(f->file, f->before, sizeof(f->file)) &&
           gmem_read(&f->mem, f->module.base + 0x100, bytes, sizeof(bytes)) &&
           !memcmp(bytes, f->before + 0x80, sizeof(bytes));
}

static void memory_only_and_relocation(void) {
    for (int identity = 0; identity < 2; identity++) {
        fixture f;
        CHECK(setup(&f, identity != 0));
        CHECK(apply(&f, "PPSA01880", "01.006.000", true) == COMPAT_PATCH_APPLIED);
        uint8_t bytes[0x100], expected[0x100];
        memcpy(expected, f.before + 0x80, sizeof(expected));
        memcpy(expected + 0x18, replacement, sizeof(replacement));
        CHECK(gmem_read(&f.mem, f.module.base + 0x100, bytes, sizeof(bytes)));
        CHECK(!memcmp(bytes, expected, sizeof(bytes)));
        CHECK(!memcmp(f.file, f.before, sizeof(f.file)));
        gmem_free(&f.mem);
    }
}
static void identity_and_opt_out(void) {
    fixture f;
    CHECK(setup(&f, false));
    CHECK(apply(&f, "PPSA01880", "01.006.000", false) == COMPAT_PATCH_DISABLED);
    CHECK(apply(&f, "PPSA99319", "01.006.000", true) == COMPAT_PATCH_WRONG_TITLE);
    CHECK(apply(&f, "PPSA01880", "01.009.000", true) == COMPAT_PATCH_WRONG_VERSION);
    CHECK(apply(&f, "PPSA01880", "", true) == COMPAT_PATCH_WRONG_VERSION);
    CHECK(apply(&f, NULL, NULL, true) == COMPAT_PATCH_WRONG_TITLE);
    CHECK(unchanged(&f));
    f.file[0] ^= 1; /* even edits away from the patch site invalidate the signature */
    CHECK(apply(&f, "PPSA01880", "01.006.000", true) == COMPAT_PATCH_WRONG_BINARY);
    f.file[0] ^= 1;
    CHECK(unchanged(&f));
    gmem_free(&f.mem);
}
static void reject_unsafe_mapping(void) {
    fixture f;
    CHECK(setup(&f, false));
    f.module.is_main = false;
    CHECK(apply(&f, "PPSA01880", "01.006.000", true) == COMPAT_PATCH_UNMAPPED);
    f.module.is_main = true;
    f.segment.h.p_flags = PF_R;
    CHECK(apply(&f, "PPSA01880", "01.006.000", true) == COMPAT_PATCH_UNMAPPED);
    f.segment.h.p_flags = PF_R | PF_X;
    f.segment.h.p_filesz = 0x19;
    CHECK(apply(&f, "PPSA01880", "01.006.000", true) == COMPAT_PATCH_UNMAPPED);
    f.segment.h.p_filesz = 0x100;
    f.module.bias = UINT64_MAX - 0x80;
    CHECK(apply(&f, "PPSA01880", "01.006.000", true) == COMPAT_PATCH_UNMAPPED);
    f.module.bias = f.module.base;
    CHECK(unchanged(&f));
    uint8_t edited = 0x90;
    CHECK(gmem_write(&f.mem, f.module.base + 0x118, &edited, 1));
    CHECK(apply(&f, "PPSA01880", "01.006.000", true) == COMPAT_PATCH_WRONG_BYTES);
    CHECK(!memcmp(f.file, f.before, sizeof(f.file)));
    gmem_free(&f.mem);
}
static void catalog_and_short_input(void) {
    const compat_patch *p = compat_patch_at(0);
    CHECK(p && !strcmp(p->title_id, "PPSA01880") && !strcmp(p->version, "01.006.000"));
    CHECK(!strcmp(p->id, "quake1_direct_ip_host") && p->default_enabled);
    CHECK(p->size == 7 && compat_patch_at(compat_patch_count()) == NULL);
    CHECK(compat_patch_check(p, p->title_id, p->version, NULL, 0) == COMPAT_PATCH_WRONG_BINARY);
    uint8_t short_file[16] = {0};
    CHECK(compat_patch_check(p, p->title_id, p->version, short_file, sizeof(short_file)) == COMPAT_PATCH_WRONG_BINARY);
}
int main(void) {
    const test_case tests[] = {TEST_CASE(memory_only_and_relocation), TEST_CASE(identity_and_opt_out),
                              TEST_CASE(reject_unsafe_mapping), TEST_CASE(catalog_and_short_input)};
    return test_run_all("compat_patches", tests, PS5C_ARRAY_COUNT(tests));
}
