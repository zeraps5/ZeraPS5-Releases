/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Builds small synthetic PS5-style executables and libraries in memory, so
 * the loader can be tested without any real game or firmware file. */
#ifndef PS5C_TEST_SCE_BUILDER_H
#define PS5C_TEST_SCE_BUILDER_H

#include "base/base.h"

#define SB_MAX_ITEMS 64
#define SB_SEG_SIZE 0x1000

typedef struct sb_modlib {
    const char *name;
    uint16_t id;
    uint16_t version; /* libraries: version; modules: (major << 8) | minor */
} sb_modlib;

typedef struct sb_sym {
    char name[48];
    uint8_t info;
    uint16_t shndx;
    uint64_t value;
} sb_sym;

typedef struct sb_rel {
    uint64_t offset;
    uint64_t info;
    int64_t addend;
    bool plt;
} sb_rel;

typedef struct sce_builder {
    uint16_t elf_type;
    uint64_t entry;
    uint8_t seg[SB_SEG_SIZE]; /* single RWX load segment at vaddr 0 */
    uint32_t got_next;        /* next free GOT slot offset in seg */
    const char *original_filename;
    bool has_module_info;
    sb_modlib module_info;
    sb_modlib needed[SB_MAX_ITEMS];
    uint32_t needed_count;
    sb_modlib imports[SB_MAX_ITEMS];
    uint32_t import_count;
    sb_modlib exports[SB_MAX_ITEMS];
    uint32_t export_count;
    sb_sym syms[SB_MAX_ITEMS]; /* index 0 is the null symbol */
    uint32_t sym_count;
    sb_rel rels[SB_MAX_ITEMS];
    uint32_t rel_count;
    bool use_v1_tags;      /* emit the *_1 variants of the module/library tags */
    bool with_tls;
    bool ps5_layout;       /* tables in a loadable segment, standard DT_* tags with vaddrs */
    bool omit_symtab_size; /* no DT_SCE_SYMTABSZ: symbol count must come from DT_HASH */
} sce_builder;

void sb_init(sce_builder *b, uint16_t elf_type);
void sb_add_needed(sce_builder *b, const char *name, uint16_t id);
void sb_add_import_lib(sce_builder *b, const char *name, uint16_t id);
void sb_add_export_lib(sce_builder *b, const char *name, uint16_t id);
void sb_set_module_info(sce_builder *b, const char *name, uint16_t id);

/* Adds an undefined symbol for `func` (NID computed from the name) and a
 * JUMP_SLOT relocation to a new GOT slot. Returns the slot vaddr. */
uint64_t sb_import(sce_builder *b, const char *func, uint16_t lib_id, uint16_t mod_id, uint8_t type);
void sb_export(sce_builder *b, const char *func, uint16_t lib_id, uint16_t mod_id, uint64_t value);
void sb_relative(sce_builder *b, uint64_t offset, int64_t addend);

/* Returns a malloc'd file image. */
uint8_t *sb_build(const sce_builder *b, size_t *size_out);
/* Wraps an ELF image into a decrypted SELF container with the given magic. */
uint8_t *sb_wrap_self(const uint8_t *elf, size_t elf_size, uint32_t magic, size_t *size_out);

#endif
