/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Parser for PS4/PS5 executables and libraries (ELF or decrypted SELF).
 * Parsing only: nothing is mapped into guest memory here. Every offset and
 * index read from the file is bounds-checked; malformed input produces an
 * error, never a crash. */
#ifndef PS5C_SCE_IMAGE_H
#define PS5C_SCE_IMAGE_H

#include "base/arena.h"
#include "base/base.h"
#include "guest/elf_sce.h"

typedef enum {
    SCE_CONTAINER_ELF,
    SCE_CONTAINER_SELF_PS4,
    SCE_CONTAINER_SELF_PS5,
} sce_container;

typedef struct sce_segment {
    elf64_phdr h;
    uint64_t data_off; /* offset of the segment data in the file buffer */
    bool has_data;     /* false if p_filesz is 0 or the data isn't in the file */
} sce_segment;

typedef struct sce_modref {
    const char *name;
    uint16_t id;
    uint8_t ver_major;
    uint8_t ver_minor;
} sce_modref;

typedef struct sce_libref {
    const char *name;
    uint16_t id;
    uint16_t version;
} sce_libref;

typedef struct sce_sym {
    const char *name; /* raw name, usually "NID#L#M" */
    uint64_t value;
    uint64_t size;
    uint16_t shndx;
    uint8_t bind;
    uint8_t type;
    bool has_nid;
    uint64_t nid;
    uint16_t lib_id;
    uint16_t mod_id;
} sce_sym;

typedef struct sce_reloc {
    uint64_t offset; /* vaddr to patch */
    int64_t addend;
    uint32_t type;
    uint32_t sym; /* index into syms (0 = none) */
    bool plt;     /* from DT_SCE_JMPREL */
} sce_reloc;

typedef struct sce_image {
    arena mem; /* owns every array and string below */
    const uint8_t *file;
    size_t file_size;
    char error[160];

    sce_container container;
    uint16_t elf_type;
    uint8_t osabi;
    uint64_t entry;
    uint32_t self_flag_warnings; /* SELF segments flagged encrypted/compressed */

    sce_segment *segments;
    uint32_t segment_count;
    uint64_t image_lo, image_hi; /* vaddr range of loadable segments; hi == 0 if none */

    bool has_dynamic;
    bool tables_in_memory; /* PS5 layout: dynamic tables live in a loadable segment */
    const char *original_filename;
    const char *soname;
    bool has_module_info;
    sce_modref module_info;
    sce_modref *needed_modules;
    uint32_t needed_module_count;
    sce_libref *import_libs;
    uint32_t import_lib_count;
    sce_libref *export_libs;
    uint32_t export_lib_count;
    const char **needed; /* DT_NEEDED file names */
    uint32_t needed_count;

    sce_sym *syms;
    uint32_t sym_count;
    sce_reloc *relocs; /* DT_SCE_RELA entries first, then DT_SCE_JMPREL */
    uint32_t reloc_count;

    uint64_t pltgot_vaddr, init_vaddr, fini_vaddr;
    bool has_init, has_fini; /* DT_INIT / DT_FINI present: PS4 libc's init is at address 0 */
    uint64_t init_array_vaddr, init_array_size, fini_array_vaddr, fini_array_size;
    uint64_t preinit_array_vaddr, preinit_array_size;
    uint64_t procparam_vaddr, procparam_size;
    uint64_t eh_frame_hdr_vaddr, eh_frame_hdr_size;

    bool has_tls;
    uint64_t tls_vaddr, tls_filesz, tls_memsz, tls_align;
} sce_image;

/* `data` must stay alive and unmodified while the image is in use. */
ps5c_err sce_image_parse(sce_image *img, const uint8_t *data, size_t size);
void sce_image_free(sce_image *img);

bool sce_image_is_library(const sce_image *img);
const sce_libref *sce_image_find_import_lib(const sce_image *img, uint16_t id);
const sce_libref *sce_image_find_export_lib(const sce_image *img, uint16_t id);
/* Library name for a symbol's lib_id: import table for undefined symbols,
 * export table for defined ones. NULL if unknown. */
const char *sce_image_sym_lib_name(const sce_image *img, const sce_sym *sym);

const char *sce_container_name(sce_container c);
const char *sce_elf_type_name(uint16_t type);
const char *sce_phdr_type_name(uint32_t type);
const char *sce_reloc_type_name(uint32_t type);

#endif
