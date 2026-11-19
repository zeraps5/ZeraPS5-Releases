/* SPDX-License-Identifier: GPL-2.0-or-later */
/* On-disk formats of PS4/PS5 executables: ELF64 with Sony extensions, and the
 * SELF container. Pure data definitions: the layouts below are the file
 * layouts, verified with static asserts. Parse with memcpy into these structs
 * (the host is little-endian x86-64, same as the files). */
#ifndef PS5C_GUEST_ELF_SCE_H
#define PS5C_GUEST_ELF_SCE_H

#include <stddef.h>

#include "base/base.h"

/* ---- ELF64 ---- */
typedef struct elf64_ehdr {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr;

typedef struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr;

typedef struct elf64_dyn {
    int64_t d_tag;
    uint64_t d_val;
} elf64_dyn;

typedef struct elf64_sym {
    uint32_t st_name;
    uint8_t st_info;
    uint8_t st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} elf64_sym;

typedef struct elf64_rela {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
} elf64_rela;

PS5C_STATIC_ASSERT(sizeof(elf64_ehdr) == 64, "elf64_ehdr size");
PS5C_STATIC_ASSERT(offsetof(elf64_ehdr, e_entry) == 24, "elf64_ehdr.e_entry");
PS5C_STATIC_ASSERT(offsetof(elf64_ehdr, e_phnum) == 56, "elf64_ehdr.e_phnum");
PS5C_STATIC_ASSERT(sizeof(elf64_phdr) == 56, "elf64_phdr size");
PS5C_STATIC_ASSERT(offsetof(elf64_phdr, p_vaddr) == 16, "elf64_phdr.p_vaddr");
PS5C_STATIC_ASSERT(sizeof(elf64_dyn) == 16, "elf64_dyn size");
PS5C_STATIC_ASSERT(sizeof(elf64_sym) == 24, "elf64_sym size");
PS5C_STATIC_ASSERT(offsetof(elf64_sym, st_value) == 8, "elf64_sym.st_value");
PS5C_STATIC_ASSERT(sizeof(elf64_rela) == 24, "elf64_rela size");

#define ELF_CLASS64 2
#define ELF_DATA_LSB 1
#define ELF_OSABI_FREEBSD 9
#define EM_X86_64 62

#define ELF64_ST_BIND(info) ((uint8_t)((info) >> 4))
#define ELF64_ST_TYPE(info) ((uint8_t)((info)&0xF))
#define ELF64_ST_INFO(bind, type) ((uint8_t)(((bind) << 4) | ((type)&0xF)))
#define ELF64_R_SYM(info) ((uint32_t)((info) >> 32))
#define ELF64_R_TYPE(info) ((uint32_t)(info))
#define ELF64_R_INFO(sym, type) (((uint64_t)(sym) << 32) | (uint32_t)(type))

enum { STB_LOCAL = 0, STB_GLOBAL = 1, STB_WEAK = 2 };
enum { STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2, STT_SECTION = 3, STT_FILE = 4, STT_TLS = 6 };
#define SHN_UNDEF 0

enum { PF_X = 1, PF_W = 2, PF_R = 4 };

/* e_type */
#define ET_EXEC 0x0002
#define ET_DYN 0x0003
#define ET_SCE_EXEC 0xFE00
#define ET_SCE_REPLAY_EXEC 0xFE01
#define ET_SCE_RELEXEC 0xFE04
#define ET_SCE_STUBLIB 0xFE0C
#define ET_SCE_DYNEXEC 0xFE10 /* main executable (eboot.bin) */
#define ET_SCE_DYNAMIC 0xFE18 /* dynamic library (.prx / .sprx) */

/* p_type */
#define PT_NULL 0x00000000u
#define PT_LOAD 0x00000001u
#define PT_DYNAMIC 0x00000002u
#define PT_INTERP 0x00000003u
#define PT_NOTE 0x00000004u
#define PT_TLS 0x00000007u
#define PT_GNU_EH_FRAME 0x6474E550u
#define PT_GNU_RELRO 0x6474E552u /* PS5 */
#define PT_SCE_RELA 0x60000000u
#define PT_SCE_DYNLIBDATA 0x61000000u
#define PT_SCE_PROCPARAM 0x61000001u
#define PT_SCE_MODULE_PARAM 0x61000002u
#define PT_SCE_RELRO 0x61000010u
#define PT_SCE_COMMENT 0x6FFFFF00u
#define PT_SCE_LIBVERSION 0x6FFFFF01u

/* d_tag: standard. PS5 modules use the standard table tags (STRTAB, SYMTAB,
 * RELA, JMPREL, HASH), holding virtual addresses of tables inside a loadable
 * segment; PS4 modules use the DT_SCE_* equivalents below instead. */
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_PLTGOT 3
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_PLTREL 20
#define DT_JMPREL 23
#define DT_RELACOUNT 0x6FFFFFF9
#define DT_INIT 12
#define DT_FINI 13
#define DT_SONAME 14
#define DT_DEBUG 21
#define DT_TEXTREL 22
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_FLAGS 30
#define DT_PREINIT_ARRAY 32
#define DT_PREINIT_ARRAYSZ 33

/* d_tag: Sony. Table tags (STRTAB, SYMTAB, RELA, JMPREL) hold offsets into
 * PT_SCE_DYNLIBDATA, not virtual addresses. The *_1 tags are alternative tag
 * values with the same encoding (a file uses one or the other). */
#define DT_SCE_IDTABENTSZ 0x61000005
#define DT_SCE_FINGERPRINT 0x61000007
#define DT_SCE_ORIGINAL_FILENAME 0x61000009
#define DT_SCE_MODULE_INFO 0x6100000D
#define DT_SCE_NEEDED_MODULE 0x6100000F
#define DT_SCE_MODULE_ATTR 0x61000011
#define DT_SCE_EXPORT_LIB 0x61000013
#define DT_SCE_IMPORT_LIB 0x61000015
#define DT_SCE_EXPORT_LIB_ATTR 0x61000017
#define DT_SCE_IMPORT_LIB_ATTR 0x61000019
#define DT_SCE_HASH 0x61000025
#define DT_SCE_PLTGOT 0x61000027
#define DT_SCE_JMPREL 0x61000029
#define DT_SCE_PLTREL 0x6100002B
#define DT_SCE_PLTRELSZ 0x6100002D
#define DT_SCE_RELA 0x6100002F
#define DT_SCE_RELASZ 0x61000031
#define DT_SCE_RELAENT 0x61000033
#define DT_SCE_STRTAB 0x61000035
#define DT_SCE_STRSZ 0x61000037
#define DT_SCE_SYMTAB 0x61000039
#define DT_SCE_SYMENT 0x6100003B
#define DT_SCE_HASHSZ 0x6100003D
#define DT_SCE_SYMTABSZ 0x6100003F
#define DT_SCE_ORIGINAL_FILENAME_1 0x61000041
#define DT_SCE_MODULE_INFO_1 0x61000043
#define DT_SCE_NEEDED_MODULE_1 0x61000045
#define DT_SCE_EXPORT_LIB_1 0x61000047
#define DT_SCE_IMPORT_LIB_1 0x61000049

/* Module value (NEEDED_MODULE, MODULE_INFO):
 *   bits 0-31 name offset in strtab, 32-39 minor, 40-47 major, 48-63 id.
 * Library value (IMPORT_LIB, EXPORT_LIB):
 *   bits 0-31 name offset in strtab, 32-47 version, 48-63 id. */
#define SCE_DYN_NAME_OFF(v) ((uint32_t)(v))
#define SCE_DYN_ID(v) ((uint16_t)((v) >> 48))
#define SCE_DYN_MOD_MINOR(v) ((uint8_t)((v) >> 32))
#define SCE_DYN_MOD_MAJOR(v) ((uint8_t)((v) >> 40))
#define SCE_DYN_LIB_VERSION(v) ((uint16_t)((v) >> 32))

/* x86-64 relocation types used by PS4/PS5 modules */
#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define R_X86_64_DTPMOD64 16
#define R_X86_64_DTPOFF64 17
#define R_X86_64_TPOFF64 18

/* ---- SELF container ----
 * The ELF header and program headers follow the segment table directly.
 * A segment with SELF_SEG_BLOCKED carries the file data of program header
 * SELF_SEG_PHDR_ID(flags). PS4 and PS5 use the same layout; only the magic
 * differs. Only decrypted ("fake") SELFs can be loaded. */
#define SELF_MAGIC_PS4 0x1D3D154Fu /* bytes 4F 15 3D 1D */
#define SELF_MAGIC_PS5 0xEEF51454u /* bytes 54 14 F5 EE */

typedef struct sce_self_header {
    uint32_t magic;
    uint8_t version;
    uint8_t mode;
    uint8_t endian;
    uint8_t attributes;
    uint32_t key_type;
    uint16_t header_size;
    uint16_t meta_size;
    uint64_t file_size;
    uint16_t num_segments;
    uint16_t unknown;
    uint32_t pad;
} sce_self_header;

typedef struct sce_self_segment {
    uint64_t flags;
    uint64_t offset;
    uint64_t compressed_size;
    uint64_t decompressed_size;
} sce_self_segment;

PS5C_STATIC_ASSERT(sizeof(sce_self_header) == 32, "sce_self_header size");
PS5C_STATIC_ASSERT(offsetof(sce_self_header, file_size) == 16, "sce_self_header.file_size");
PS5C_STATIC_ASSERT(offsetof(sce_self_header, num_segments) == 24, "sce_self_header.num_segments");
PS5C_STATIC_ASSERT(sizeof(sce_self_segment) == 32, "sce_self_segment size");

#define SELF_SEG_ORDERED 0x1u
#define SELF_SEG_ENCRYPTED 0x2u
#define SELF_SEG_SIGNED 0x4u
#define SELF_SEG_COMPRESSED 0x8u
#define SELF_SEG_BLOCKED 0x800u
#define SELF_SEG_PHDR_ID(flags) ((uint32_t)(((flags) >> 20) & 0xFFFu))
#define SELF_SEG_MAKE_ID(id) ((uint64_t)((id)&0xFFFu) << 20)

#endif
