/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "base/base.h"
#include <stdlib.h>

#include "base/json.h"
#include "loader/module_set.h"
#include "loader/nid.h"
#include "loader/nid_db.h"
#include "loader/sce_image.h"
#include "pal/pal.h"
#include "recorder/profile.h"
#include "support/sce_builder.h"
#include "test.h"

/* ---------------------------------------------------------------------------
 * NIDs
 * ------------------------------------------------------------------------- */

static void check_nid(const char *name, const char *expected, int *ok) {
    char s[NID_STR_LEN + 1];
    nid_to_str(nid_hash_name(name), s);
    if (strcmp(s, expected) != 0) {
        printf("    %s -> %s (expected %s)\n", name, s, expected);
        *ok = 0;
    }
}

static void test_nid_known_values(void) {
    /* Values confirmed in shadPS4 aerolib.inl and KytyPS5 libVideoOut.cpp. */
    int ok = 1;
    check_nid("__stack_chk_fail", "Ou3iL1abvng", &ok);
    check_nid("sceVideoOutOpen", "Up36PTk687E", &ok);
    CHECK(ok);
}

static void test_nid_string_roundtrip(void) {
    uint64_t x = 0x0123456789ABCDEFull;
    for (int i = 0; i < 1000; i++) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        char s[NID_STR_LEN + 1];
        nid_to_str(x, s);
        uint64_t back = 0;
        CHECK(nid_from_str(s, NID_STR_LEN, &back));
        CHECK_EQ_U64(back, x);
    }
    uint64_t v;
    CHECK(!nid_from_str("Up36PTk687", 10, &v));  /* too short */
    CHECK(!nid_from_str("Up36PTk687/", 11, &v)); /* '/' isn't in the alphabet */
    CHECK(!nid_from_str("Up36PTk687B", 11, &v)); /* last char must have its low 2 bits clear */
}

static void test_nid_ids(void) {
    for (uint32_t id = 0; id <= 0xFFFF; id++) {
        char s[4];
        nid_encode_id((uint16_t)id, s);
        uint16_t back = 0;
        CHECK(nid_decode_id(s, strlen(s), &back));
        CHECK(back == id);
    }
    uint16_t v;
    CHECK(!nid_decode_id("", 0, &v));
    CHECK(!nid_decode_id("ABCD", 4, &v));
}

static void test_nid_parse_symbol(void) {
    nid_symbol s;
    CHECK(nid_parse_symbol("Up36PTk687E#B#C", &s));
    CHECK(s.lib_id == 1 && s.mod_id == 2);
    CHECK_EQ_U64(s.nid, nid_hash_name("sceVideoOutOpen"));
    CHECK(nid_parse_symbol("Up36PTk687E#BA#A", &s) && s.lib_id == 64 && s.mod_id == 0);
    CHECK(!nid_parse_symbol("Up36PTk687E#B", &s));
    CHECK(!nid_parse_symbol("memcpy", &s));
    CHECK(!nid_parse_symbol("Up36PTk687#B#C", &s));
}

static void test_nid_db(void) {
    nid_db db;
    nid_db_init(&db);
    const char *text = "# comment\r\nsceVideoOutOpen\r\n\n  __stack_chk_fail  \nsceVideoOutOpen\n";
    CHECK(nid_db_load_text(&db, text, strlen(text)) == PS5C_OK);
    CHECK(db.count == 2); /* duplicate ignored */
    const char *n = nid_db_lookup(&db, nid_hash_name("__stack_chk_fail"));
    CHECK(n && strcmp(n, "__stack_chk_fail") == 0);
    CHECK(nid_db_lookup(&db, 12345) == NULL);
    nid_db_free(&db);
}

/* ---------------------------------------------------------------------------
 * Image parsing
 * ------------------------------------------------------------------------- */

enum { LIB_KERNEL = 1, LIB_VIDEOOUT = 2, LIB_NGS2 = 3 };

static void build_game(sce_builder *b) {
    sb_init(b, ET_SCE_DYNEXEC);
    b->original_filename = "eboot.bin";
    b->with_tls = true;
    sb_add_needed(b, "libkernel", LIB_KERNEL);
    sb_add_needed(b, "libSceVideoOut", LIB_VIDEOOUT);
    sb_add_needed(b, "libSceNgs2", LIB_NGS2);
    sb_add_import_lib(b, "libkernel", LIB_KERNEL);
    sb_add_import_lib(b, "libSceVideoOut", LIB_VIDEOOUT);
    sb_add_import_lib(b, "libSceNgs2", LIB_NGS2);
    sb_import(b, "sceKernelUsleep", LIB_KERNEL, LIB_KERNEL, STT_FUNC);
    sb_import(b, "__stack_chk_guard", LIB_KERNEL, LIB_KERNEL, STT_OBJECT);
    sb_import(b, "sceVideoOutOpen", LIB_VIDEOOUT, LIB_VIDEOOUT, STT_FUNC);
    sb_import(b, "sceNgs2SystemCreate", LIB_NGS2, LIB_NGS2, STT_FUNC);
    sb_import(b, "sceNgs2NotExported", LIB_NGS2, LIB_NGS2, STT_FUNC);
    sb_relative(b, 0x900, 0x100);
}

static void build_ngs2_module(sce_builder *b) {
    sb_init(b, ET_SCE_DYNAMIC);
    sb_set_module_info(b, "libSceNgs2", 1);
    sb_add_export_lib(b, "libSceNgs2", 1);
    sb_export(b, "sceNgs2SystemCreate", 1, 1, 0x100);
}

static void check_game_image(const sce_image *img, sce_container expected_container) {
    CHECK(img->container == expected_container);
    CHECK(img->elf_type == ET_SCE_DYNEXEC);
    CHECK(img->osabi == ELF_OSABI_FREEBSD);
    CHECK_EQ_U64(img->entry, 0x100);
    CHECK(img->segment_count == 4);
    CHECK_EQ_U64(img->image_lo, 0);
    if (img->tables_in_memory) {
        CHECK(img->image_hi > 0x10000); /* the tables segment is mapped too */
    } else {
        CHECK_EQ_U64(img->image_hi, SB_SEG_SIZE + 0x1000);
    }
    CHECK(img->has_tls && img->tls_memsz == 0x40 && img->tls_align == 16);
    CHECK(img->original_filename && strcmp(img->original_filename, "eboot.bin") == 0);

    CHECK(img->needed_module_count == 3);
    CHECK(strcmp(img->needed_modules[1].name, "libSceVideoOut") == 0);
    CHECK(img->needed_modules[1].id == LIB_VIDEOOUT);
    CHECK(img->needed_modules[1].ver_major == 1 && img->needed_modules[1].ver_minor == 1);
    CHECK(img->import_lib_count == 3 && img->export_lib_count == 0);

    CHECK(img->sym_count == 6); /* null + 5 imports */
    const sce_sym *vo = &img->syms[3];
    CHECK(vo->has_nid && vo->shndx == SHN_UNDEF && vo->type == STT_FUNC);
    CHECK_EQ_U64(vo->nid, nid_hash_name("sceVideoOutOpen"));
    const char *lib = sce_image_sym_lib_name(img, vo);
    CHECK(lib && strcmp(lib, "libSceVideoOut") == 0);
    CHECK(img->syms[2].type == STT_OBJECT);

    /* DT_SCE_RELA entries come first, then DT_SCE_JMPREL. */
    CHECK(img->reloc_count == 6);
    CHECK(img->relocs[0].type == R_X86_64_GLOB_DAT && img->relocs[0].sym == 2 && !img->relocs[0].plt);
    CHECK(img->relocs[1].type == R_X86_64_RELATIVE && img->relocs[1].addend == 0x100);
    CHECK(img->relocs[2].type == R_X86_64_JUMP_SLOT && img->relocs[2].plt && img->relocs[2].sym == 1);
    CHECK_EQ_U64(img->pltgot_vaddr, 0x800);
}

static void test_parse_elf(void) {
    sce_builder b;
    build_game(&b);
    size_t size;
    uint8_t *file = sb_build(&b, &size);
    CHECK(file != NULL);
    sce_image img;
    ps5c_err err = sce_image_parse(&img, file, size);
    if (err != PS5C_OK) printf("    parse error: %s\n", img.error);
    CHECK(err == PS5C_OK);
    check_game_image(&img, SCE_CONTAINER_ELF);
    sce_image_free(&img);
    free(file);
}

static void test_parse_self(void) {
    sce_builder b;
    build_game(&b);
    size_t size, self_size;
    uint8_t *elf = sb_build(&b, &size);
    CHECK(elf != NULL);
    const uint32_t magics[2] = {SELF_MAGIC_PS4, SELF_MAGIC_PS5};
    const sce_container kinds[2] = {SCE_CONTAINER_SELF_PS4, SCE_CONTAINER_SELF_PS5};
    for (int i = 0; i < 2; i++) {
        uint8_t *self = sb_wrap_self(elf, size, magics[i], &self_size);
        CHECK(self != NULL);
        sce_image img;
        ps5c_err err = sce_image_parse(&img, self, self_size);
        if (err != PS5C_OK) printf("    parse error: %s\n", img.error);
        CHECK(err == PS5C_OK);
        check_game_image(&img, kinds[i]);
        /* Segment data must come from the SELF blocks, not the ELF offsets. */
        CHECK(img.segments[0].has_data);
        CHECK(memcmp(self + img.segments[0].data_off + 0x100, "\xB8\x2A\x00\x00\x00\xC3", 6) == 0);
        sce_image_free(&img);
        free(self);
    }
    free(elf);
}

static void test_parse_v1_tags(void) {
    sce_builder b;
    build_game(&b);
    b.use_v1_tags = true;
    size_t size;
    uint8_t *file = sb_build(&b, &size);
    sce_image img;
    CHECK(sce_image_parse(&img, file, size) == PS5C_OK);
    check_game_image(&img, SCE_CONTAINER_ELF);
    sce_image_free(&img);
    free(file);
}

/* Layout observed in real PS5 executables: no PT_SCE_DYNLIBDATA; standard
 * DT_STRTAB/DT_SYMTAB/DT_RELA/... tags hold vaddrs inside a LOAD segment. */
static void test_parse_ps5_layout(void) {
    for (int variant = 0; variant < 4; variant++) {
        sce_builder b;
        build_game(&b);
        b.ps5_layout = true;
        b.use_v1_tags = true;
        b.omit_symtab_size = (variant & 1) != 0; /* symbol count from DT_HASH */
        size_t size;
        uint8_t *elf = sb_build(&b, &size);
        CHECK(elf != NULL);
        uint8_t *input = elf;
        size_t input_size = size;
        if (variant & 2) {
            input = sb_wrap_self(elf, size, SELF_MAGIC_PS4, &input_size); /* real PS5 dumps use it */
            CHECK(input != NULL);
        }
        sce_image img;
        ps5c_err err = sce_image_parse(&img, input, input_size);
        if (err != PS5C_OK) printf("    variant %d: %s\n", variant, img.error);
        CHECK(err == PS5C_OK);
        CHECK(img.tables_in_memory);
        check_game_image(&img, (variant & 2) ? SCE_CONTAINER_SELF_PS4 : SCE_CONTAINER_ELF);
        sce_image_free(&img);
        if (input != elf) free(input);
        free(elf);
    }
}

static void test_parse_rejects_bad_input(void) {
    sce_image img;
    static const uint8_t junk[64] = {1, 2, 3, 4};
    CHECK(sce_image_parse(&img, junk, sizeof(junk)) == PS5C_ERR_INVALID);
    CHECK(strstr(img.error, "unknown file format") != NULL);
    sce_image_free(&img);

    CHECK(sce_image_parse(&img, junk, 2) == PS5C_ERR_INVALID);
    sce_image_free(&img);

    /* An encrypted SELF has no readable ELF header after the segment table. */
    sce_builder b;
    build_game(&b);
    size_t size, self_size;
    uint8_t *elf = sb_build(&b, &size);
    uint8_t *self = sb_wrap_self(elf, size, SELF_MAGIC_PS5, &self_size);
    size_t ehdr_pos = sizeof(sce_self_header) + 3 * sizeof(sce_self_segment);
    memset(self + ehdr_pos, 0xAB, 4);
    CHECK(sce_image_parse(&img, self, self_size) == PS5C_ERR_INVALID);
    CHECK(strstr(img.error, "encrypted") != NULL);
    sce_image_free(&img);
    free(self);

    /* Relocation pointing past the symbol table. */
    b.rels[0].info = ELF64_R_INFO(60, R_X86_64_64);
    uint8_t *bad = sb_build(&b, &size);
    CHECK(sce_image_parse(&img, bad, size) == PS5C_ERR_INVALID);
    CHECK(strstr(img.error, "references symbol") != NULL);
    sce_image_free(&img);
    free(bad);
    free(elf);
}

static void test_parse_truncation_never_crashes(void) {
    sce_builder b;
    build_game(&b);
    size_t size;
    uint8_t *file = sb_build(&b, &size);
    for (size_t len = 0; len < size; len++) {
        /* Copy into an exact-size buffer so out-of-bounds reads hit the heap guard. */
        uint8_t *cut = (uint8_t *)malloc(len ? len : 1);
        memcpy(cut, file, len);
        sce_image img;
        ps5c_err err = sce_image_parse(&img, cut, len);
        CHECK(err == PS5C_OK || err == PS5C_ERR_INVALID);
        sce_image_free(&img);
        free(cut);
    }
    free(file);
}

static void test_parse_bitflips_never_crash(void) {
    sce_builder b;
    build_game(&b);
    size_t size;
    uint8_t *file = sb_build(&b, &size);
    uint8_t *mut = (uint8_t *)malloc(size);
    uint64_t rng = 0x5EED;
    uint32_t accepted = 0;
    for (int iter = 0; iter < 4000; iter++) {
        memcpy(mut, file, size);
        int flips = 1 + (int)(rng % 4);
        for (int f = 0; f < flips; f++) {
            rng = rng * 6364136223846793005ull + 1442695040888963407ull;
            /* Focus on headers and dynamic data, where parsing decisions are made. */
            size_t pos = (size_t)(rng >> 33) % size;
            if (iter % 2 == 0 && pos >= 0x1000 && pos < 0x2000) pos %= 0x200;
            mut[pos] ^= (uint8_t)(1u << ((rng >> 20) & 7));
        }
        sce_image img;
        ps5c_err err = sce_image_parse(&img, mut, size);
        CHECK(err == PS5C_OK || err == PS5C_ERR_INVALID);
        accepted += err == PS5C_OK;
        sce_image_free(&img);
    }
    CHECK(accepted > 0); /* flips in code bytes don't affect parsing */
    free(mut);
    free(file);
}

/* ---------------------------------------------------------------------------
 * Module set / resolution
 * ------------------------------------------------------------------------- */

static bool fake_hle(void *user, const char *library, uint64_t nid) {
    PS5C_UNUSED(user);
    return strcmp(library, "libkernel") == 0 && nid == nid_hash_name("sceKernelUsleep");
}

typedef struct fixture {
    uint8_t *game, *ngs2;
    size_t game_size, ngs2_size;
    module_set ms;
} fixture;

static bool fixture_setup(fixture *f) {
    sce_builder b;
    build_game(&b);
    f->game = sb_build(&b, &f->game_size);
    build_ngs2_module(&b);
    f->ngs2 = sb_build(&b, &f->ngs2_size);
    module_set_init(&f->ms);
    f->ms.hle_query = fake_hle;
    return f->game && f->ngs2 &&
           module_set_add_buffer(&f->ms, "eboot.bin", f->game, f->game_size, true, NULL) == PS5C_OK &&
           module_set_add_buffer(&f->ms, "libSceNgs2.sprx", f->ngs2, f->ngs2_size, false, NULL) == PS5C_OK;
}

static void fixture_teardown(fixture *f) {
    module_set_free(&f->ms);
    free(f->game);
    free(f->ngs2);
}

static void test_resolution(void) {
    fixture f;
    CHECK(fixture_setup(&f));
    module_set *ms = &f.ms;

    CHECK(module_set_main(ms) != NULL);
    CHECK(module_set_find_by_name(ms, "libSceNgs2") != NULL);

    import_resolution r = module_set_resolve(ms, "libkernel", nid_hash_name("sceKernelUsleep"));
    CHECK(r.kind == PROVIDER_HLE && r.policy == LIB_POLICY_HLE);

    r = module_set_resolve(ms, "libkernel", nid_hash_name("sceKernelMissing"));
    CHECK(r.kind == PROVIDER_MISSING);

    r = module_set_resolve(ms, "libSceNgs2", nid_hash_name("sceNgs2SystemCreate"));
    CHECK(r.kind == PROVIDER_LLE && r.lle_available && r.policy == LIB_POLICY_AUTO);
    CHECK(r.module && strcmp(r.module->name, "libSceNgs2") == 0);
    CHECK_EQ_U64(r.sym->value, 0x100);

    /* The library is LLE, so a function it doesn't export is missing (no HLE mixing). */
    r = module_set_resolve(ms, "libSceNgs2", nid_hash_name("sceNgs2NotExported"));
    CHECK(r.kind == PROVIDER_MISSING);

    r = module_set_resolve(ms, "libSceVideoOut", nid_hash_name("sceVideoOutOpen"));
    CHECK(r.kind == PROVIDER_MISSING && !r.lle_available);

    /* Forcing HLE on a library with an available module. */
    CHECK(module_set_set_policy(ms, "libSceNgs2", LIB_POLICY_HLE) == PS5C_OK);
    r = module_set_resolve(ms, "libSceNgs2", nid_hash_name("sceNgs2SystemCreate"));
    CHECK(r.kind == PROVIDER_MISSING && r.policy == LIB_POLICY_HLE);

    /* Hybrid: HLE where implemented, the module's export otherwise. */
    CHECK(module_set_set_policy(ms, "libSceNgs2", LIB_POLICY_HYBRID) == PS5C_OK);
    r = module_set_resolve(ms, "libSceNgs2", nid_hash_name("sceNgs2SystemCreate"));
    CHECK(r.kind == PROVIDER_LLE && r.policy == LIB_POLICY_HYBRID);
    r = module_set_resolve(ms, "libSceNgs2", nid_hash_name("sceNgs2NotExported"));
    CHECK(r.kind == PROVIDER_MISSING);
    CHECK(module_set_get_policy(ms, "libSceAgc") == LIB_POLICY_HYBRID);
    CHECK(module_set_get_policy(ms, "libSceAgcDriver") == LIB_POLICY_HLE);
    /* libSceLibcInternal has no module here and "libc" isn't loaded: missing. */
    r = module_set_resolve(ms, "libSceLibcInternal", nid_hash_name("memcpy"));
    CHECK(r.kind == PROVIDER_MISSING);

    /* libkernel can't be switched to LLE. */
    CHECK(module_set_set_policy(ms, "libkernel", LIB_POLICY_LLE) == PS5C_OK);
    CHECK(module_set_get_policy(ms, "libkernel") == LIB_POLICY_HLE);

    /* Only one main executable. */
    CHECK(module_set_add_buffer(ms, "again", f.game, f.game_size, true, NULL) == PS5C_ERR_INVALID);
    fixture_teardown(&f);
}

static void test_companion_libc(void) {
    const char *dir="loader-companion-fixture", *path="loader-companion-fixture/libc.prx";
    pal_mkdir(dir);
    sce_builder b; sb_init(&b,ET_SCE_DYNAMIC); sb_set_module_info(&b,"libc",1);
    sb_add_export_lib(&b,"libc",1); sb_export(&b,"memcpy",1,1,0x100);
    /* Its own private libc dependency must not load the file again. */
    sb_add_needed(&b,"libSceLibcInternal",2);
    size_t size; uint8_t *bytes=sb_build(&b,&size); CHECK(bytes);
    pal_file *file;
    CHECK(pal_file_open(path,PAL_FILE_WRITE|PAL_FILE_CREATE|PAL_FILE_TRUNCATE,&file)==PS5C_OK);
    CHECK(pal_file_write(file,bytes,size,NULL)==PS5C_OK); pal_file_close(file); free(bytes);
    const char *dirs[]={dir};
    for (unsigned hle=0;hle<2;hle++) {
        sb_init(&b,ET_SCE_DYNEXEC); sb_add_needed(&b,"libSceLibcInternal",1);
        bytes=sb_build(&b,&size); CHECK(bytes);
        module_set ms; module_set_init(&ms);
        CHECK(module_set_add_buffer(&ms,"companion-test",bytes,size,true,NULL)==PS5C_OK);
        if (hle) CHECK(module_set_set_policy(&ms,"libSceLibcInternal",LIB_POLICY_HLE)==PS5C_OK);
        CHECK(module_set_load_needed(&ms,dirs,1)==(hle?0u:1u));
        CHECK(module_set_load_needed(&ms,dirs,1)==0);
        import_resolution r=module_set_resolve(&ms,"libSceLibcInternal",nid_hash_name("memcpy"));
        /* Sharing a NID does not make libc's export a firmware function. */
        CHECK(r.kind==PROVIDER_MISSING);
        r=module_set_resolve(&ms,"libc",nid_hash_name("memcpy"));
        CHECK(r.kind==(hle?PROVIDER_MISSING:PROVIDER_LLE));
        CHECK(ms.modules.len==(hle?1u:2u));
        module_set_free(&ms); free(bytes);
    }
    CHECK(pal_file_remove(path)==PS5C_OK); CHECK(pal_rmdir(dir)==PS5C_OK);
}

static void module_view_mode(const char *value) {
#ifdef _WIN32
    _putenv_s("PS5C_MODULE_FILE_VIEWS", value); ps5c_env_reset();
#else
    setenv("PS5C_MODULE_FILE_VIEWS", value, 1); ps5c_env_reset();
#endif
}

static void test_module_file_views(void) {
    const char *path = "ps5c_module_view.tmp";
    sce_builder b;
    build_game(&b);
    size_t elf_size;
    uint8_t *elf = sb_build(&b, &elf_size);
    CHECK(elf != NULL);
    const char *old = getenv("PS5C_MODULE_FILE_VIEWS");
    char *saved = NULL;
    if (old) { saved = malloc(strlen(old) + 1); CHECK(saved != NULL); strcpy(saved, old); }
    for (int kind = 0; kind < 3; ++kind) {
        size_t size = elf_size;
        uint8_t *data = kind ? sb_wrap_self(elf, elf_size, kind == 1 ? SELF_MAGIC_PS4 : SELF_MAGIC_PS5, &size) : elf;
        CHECK(data != NULL);
        pal_file *f;
        CHECK(pal_file_open(path, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE, &f) == PS5C_OK);
        uint64_t written = 0;
        CHECK(pal_file_write(f, data, size, &written) == PS5C_OK && written == size);
        pal_file_close(f);
        for (int mode = 0; mode < 2; ++mode) {
            module_view_mode(mode ? "1" : "0");
            module_set ms;
            module_set_init(&ms);
            loaded_module *m = NULL;
            CHECK(module_set_add_file(&ms, path, true, &m) == PS5C_OK && m);
            CHECK((m->owned_data != NULL) == !mode);
            CHECK((m->file_view.opaque != NULL) == !!mode);
            CHECK(m->img.file_size == size && memcmp(m->img.file, data, size) == 0);
            check_game_image(&m->img, kind == 0 ? SCE_CONTAINER_ELF : kind == 1 ? SCE_CONTAINER_SELF_PS4 : SCE_CONTAINER_SELF_PS5);
            CHECK(memcmp(m->img.file + m->img.segments[0].data_off + 0x100, "\xB8\x2A\x00\x00\x00\xC3", 6) == 0);
            /* Rejecting a second main must close its own view without closing the first. */
            CHECK(module_set_add_file(&ms, path, true, NULL) == PS5C_ERR_INVALID);
            CHECK(memcmp(m->img.file, data, size) == 0);
            module_set_free(&ms);
            /* Windows deletion checks all view/file handles were released. */
        }
#ifdef _WIN32
        /* A writer prevents immutable mapping; the previous read-all path still works. */
        CHECK(pal_file_open(path, PAL_FILE_WRITE, &f) == PS5C_OK);
        module_set ms;
        module_set_init(&ms);
        loaded_module *m = NULL;
        CHECK(module_set_add_file(&ms, path, true, &m) == PS5C_OK && m);
        CHECK(m->owned_data && !m->file_view.opaque);
        module_set_free(&ms);
        pal_file_close(f);
#endif
        CHECK(pal_file_remove(path) == PS5C_OK);
        if (kind) free(data);
    }
    free(elf);
    /* Parser failure and empty-file paths must release mapping ownership too. */
    for (int empty = 0; empty < 2; ++empty) {
        pal_file *f;
        CHECK(pal_file_open(path, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE, &f) == PS5C_OK);
        if (!empty) CHECK(pal_file_write(f, "bad", 3, NULL) == PS5C_OK);
        pal_file_close(f);
        module_set ms;
        module_set_init(&ms);
        CHECK(module_set_add_file(&ms, path, true, NULL) == PS5C_ERR_INVALID);
        CHECK(ms.modules.len == 0);
        module_set_free(&ms);
        CHECK(pal_file_remove(path) == PS5C_OK);
    }
    module_view_mode(saved ? saved : "0");
    free(saved);
}

static void test_static_profile(void) {
    fixture f;
    CHECK(fixture_setup(&f));
    nid_db db;
    nid_db_init(&db);
    nid_db_add(&db, "sceVideoOutOpen");
    nid_db_add(&db, "sceNgs2SystemCreate");

    json_writer w;
    json_init(&w, true);
    profile_summary sum;
    CHECK(profile_write_static(&w, &f.ms, &db, &sum) == PS5C_OK);
    const char *doc = json_result(&w, NULL);
    CHECK(doc != NULL);

    CHECK(sum.modules_loaded == 2);
    CHECK(sum.imports_unique == 5);
    CHECK(sum.imports_named == 2);
    CHECK(sum.by_provider[PROVIDER_HLE] == 1);
    CHECK(sum.by_provider[PROVIDER_LLE] == 1);
    CHECK(sum.by_provider[PROVIDER_MISSING] == 3);
    CHECK(sum.modules_missing == 1); /* libSceVideoOut; libkernel is HLE by design */

    CHECK(strstr(doc, "\"missing_modules\": [\n    \"libSceVideoOut\"\n  ]") != NULL);
    CHECK(strstr(doc, "\"name\": \"sceNgs2SystemCreate\"") != NULL);
    CHECK(strstr(doc, "\"provider_module\": \"libSceNgs2\"") != NULL);
    CHECK(strstr(doc, "\"nid\": \"Up36PTk687E\"") != NULL);
    CHECK(strstr(doc, "\"JUMP_SLOT\": 4") != NULL);

    /* Deterministic: writing twice gives identical output. */
    json_writer w2;
    json_init(&w2, true);
    CHECK(profile_write_static(&w2, &f.ms, &db, NULL) == PS5C_OK);
    CHECK(strcmp(doc, json_result(&w2, NULL)) == 0);

    json_free(&w2);
    json_free(&w);
    nid_db_free(&db);
    fixture_teardown(&f);
}

int main(void) {
    pal_init();
    log_set_level_all(LOG_LVL_ERROR);
    static const test_case tests[] = {
        TEST_CASE(test_nid_known_values),
        TEST_CASE(test_nid_string_roundtrip),
        TEST_CASE(test_nid_ids),
        TEST_CASE(test_nid_parse_symbol),
        TEST_CASE(test_nid_db),
        TEST_CASE(test_parse_elf),
        TEST_CASE(test_parse_self),
        TEST_CASE(test_parse_v1_tags),
        TEST_CASE(test_parse_ps5_layout),
        TEST_CASE(test_parse_rejects_bad_input),
        TEST_CASE(test_parse_truncation_never_crashes),
        TEST_CASE(test_parse_bitflips_never_crash),
        TEST_CASE(test_resolution),
        TEST_CASE(test_companion_libc),
        TEST_CASE(test_module_file_views),
        TEST_CASE(test_static_profile),
    };
    return test_run_all("loader", tests, PS5C_ARRAY_COUNT(tests));
}
