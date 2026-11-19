/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "support/sce_builder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/dynarray.h"
#include "guest/elf_sce.h"
#include "loader/nid.h"

#define LOAD_FILE_OFF 0x1000u
#define GOT_BASE 0x800u
#define TLS_VADDR 0xC00u
#define TABLES_VADDR 0x10000u

void sb_init(sce_builder *b, uint16_t elf_type) {
    memset(b, 0, sizeof(*b));
    b->elf_type = elf_type;
    b->entry = 0x100;
    b->got_next = GOT_BASE;
    b->sym_count = 1; /* null symbol */
    /* A recognizable code stub at the entry: mov eax, 42; ret */
    static const uint8_t k_code[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
    memcpy(b->seg + b->entry, k_code, sizeof(k_code));
}

static void add_ref(sb_modlib *arr, uint32_t *count, const char *name, uint16_t id, uint16_t ver) {
    if (*count < SB_MAX_ITEMS) {
        arr[*count].name = name;
        arr[*count].id = id;
        arr[*count].version = ver;
        (*count)++;
    }
}

void sb_add_needed(sce_builder *b, const char *name, uint16_t id) {
    add_ref(b->needed, &b->needed_count, name, id, 0x0101);
}
void sb_add_import_lib(sce_builder *b, const char *name, uint16_t id) {
    add_ref(b->imports, &b->import_count, name, id, 1);
}
void sb_add_export_lib(sce_builder *b, const char *name, uint16_t id) {
    add_ref(b->exports, &b->export_count, name, id, 1);
}
void sb_set_module_info(sce_builder *b, const char *name, uint16_t id) {
    b->has_module_info = true;
    b->module_info.name = name;
    b->module_info.id = id;
    b->module_info.version = 0x0100;
}

static uint32_t add_sym(sce_builder *b, const char *func, uint16_t lib_id, uint16_t mod_id, uint8_t info,
                        uint16_t shndx, uint64_t value) {
    if (b->sym_count >= SB_MAX_ITEMS) {
        return 0;
    }
    char nid[NID_STR_LEN + 1], lib[4], mod[4];
    nid_to_str(nid_hash_name(func), nid);
    nid_encode_id(lib_id, lib);
    nid_encode_id(mod_id, mod);
    sb_sym *s = &b->syms[b->sym_count];
    snprintf(s->name, sizeof(s->name), "%s#%s#%s", nid, lib, mod);
    s->info = info;
    s->shndx = shndx;
    s->value = value;
    return b->sym_count++;
}

uint64_t sb_import(sce_builder *b, const char *func, uint16_t lib_id, uint16_t mod_id, uint8_t type) {
    uint32_t sym = add_sym(b, func, lib_id, mod_id, ELF64_ST_INFO(STB_GLOBAL, type), SHN_UNDEF, 0);
    uint64_t slot = b->got_next;
    b->got_next += 8;
    if (b->rel_count < SB_MAX_ITEMS && sym) {
        sb_rel *r = &b->rels[b->rel_count++];
        r->offset = slot;
        r->info = ELF64_R_INFO(sym, type == STT_OBJECT ? R_X86_64_GLOB_DAT : R_X86_64_JUMP_SLOT);
        r->addend = 0;
        r->plt = type != STT_OBJECT;
    }
    return slot;
}

void sb_export(sce_builder *b, const char *func, uint16_t lib_id, uint16_t mod_id, uint64_t value) {
    add_sym(b, func, lib_id, mod_id, ELF64_ST_INFO(STB_GLOBAL, STT_FUNC), 9, value);
}

void sb_relative(sce_builder *b, uint64_t offset, int64_t addend) {
    if (b->rel_count < SB_MAX_ITEMS) {
        sb_rel *r = &b->rels[b->rel_count++];
        r->offset = offset;
        r->info = ELF64_R_INFO(0, R_X86_64_RELATIVE);
        r->addend = addend;
        r->plt = false;
    }
}

/* ---- byte buffer helpers ---- */

typedef DYNARRAY(uint8_t) bytes;

static void put_bytes(bytes *out, const void *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        dyn_push(out, ((const uint8_t *)p)[i]);
    }
}

static void pad_to(bytes *out, size_t align) {
    while (out->len % align) {
        dyn_push(out, 0);
    }
}

static uint32_t add_str(bytes *strtab, const char *s) {
    uint32_t off = (uint32_t)strtab->len;
    put_bytes(strtab, s, strlen(s) + 1);
    return off;
}

static void put_dyn(bytes *out, int64_t tag, uint64_t val) {
    elf64_dyn d = {tag, val};
    put_bytes(out, &d, sizeof(d));
}

static uint64_t modval(uint32_t name_off, uint16_t ver, uint16_t id) {
    return (uint64_t)name_off | ((uint64_t)ver << 32) | ((uint64_t)id << 48);
}

uint8_t *sb_build(const sce_builder *b, size_t *size_out) {
    /* dynlibdata: strtab, symtab, rela, jmprel */
    bytes strtab = {0}, dynlib = {0}, dyn = {0};
    dyn_push(&strtab, 0);
    uint32_t sym_name_off[SB_MAX_ITEMS] = {0};
    for (uint32_t i = 1; i < b->sym_count; i++) sym_name_off[i] = add_str(&strtab, b->syms[i].name);
    uint32_t needed_off[SB_MAX_ITEMS], import_off[SB_MAX_ITEMS], export_off[SB_MAX_ITEMS];
    for (uint32_t i = 0; i < b->needed_count; i++) needed_off[i] = add_str(&strtab, b->needed[i].name);
    for (uint32_t i = 0; i < b->import_count; i++) import_off[i] = add_str(&strtab, b->imports[i].name);
    for (uint32_t i = 0; i < b->export_count; i++) export_off[i] = add_str(&strtab, b->exports[i].name);
    uint32_t mi_off = b->has_module_info ? add_str(&strtab, b->module_info.name) : 0;
    uint32_t orig_off = b->original_filename ? add_str(&strtab, b->original_filename) : 0;

    put_bytes(&dynlib, strtab.data, strtab.len);
    uint64_t strtab_size = strtab.len;
    pad_to(&dynlib, 8);
    uint64_t symtab_off = dynlib.len;
    for (uint32_t i = 0; i < b->sym_count; i++) {
        elf64_sym s;
        memset(&s, 0, sizeof(s));
        if (i > 0) {
            s.st_name = sym_name_off[i];
            s.st_info = b->syms[i].info;
            s.st_shndx = b->syms[i].shndx;
            s.st_value = b->syms[i].value;
        }
        put_bytes(&dynlib, &s, sizeof(s));
    }
    uint64_t symtab_size = dynlib.len - symtab_off;
    /* Non-PLT relocations first (DT_SCE_RELA), then PLT ones (DT_SCE_JMPREL). */
    uint64_t rela_off = dynlib.len;
    for (uint32_t pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < b->rel_count; i++) {
            if (b->rels[i].plt != (pass == 1)) continue;
            elf64_rela r = {b->rels[i].offset, b->rels[i].info, b->rels[i].addend};
            put_bytes(&dynlib, &r, sizeof(r));
        }
    }
    uint64_t n_rela = 0, n_plt = 0;
    for (uint32_t i = 0; i < b->rel_count; i++) {
        if (b->rels[i].plt) n_plt++;
        else n_rela++;
    }
    uint64_t jmprel_off = rela_off + n_rela * sizeof(elf64_rela);

    /* ELF hash table (nbucket = 1): its nchain field gives the symbol count. */
    pad_to(&dynlib, 8);
    uint64_t hash_off = dynlib.len;
    uint32_t hash_hdr[3] = {1, b->sym_count, 0};
    put_bytes(&dynlib, hash_hdr, sizeof(hash_hdr));
    for (uint32_t i = 0; i < b->sym_count; i++) {
        uint32_t zero = 0;
        put_bytes(&dynlib, &zero, sizeof(zero));
    }

    /* PS4: tables are offsets into PT_SCE_DYNLIBDATA. PS5: standard tags with
     * virtual addresses of the same blob, mapped by a loadable segment. */
    uint64_t tb = b->ps5_layout ? TABLES_VADDR : 0;

    /* dynamic table */
    int64_t t_needed = b->use_v1_tags ? DT_SCE_NEEDED_MODULE_1 : DT_SCE_NEEDED_MODULE;
    int64_t t_import = b->use_v1_tags ? DT_SCE_IMPORT_LIB_1 : DT_SCE_IMPORT_LIB;
    int64_t t_export = b->use_v1_tags ? DT_SCE_EXPORT_LIB_1 : DT_SCE_EXPORT_LIB;
    int64_t t_modinfo = b->use_v1_tags ? DT_SCE_MODULE_INFO_1 : DT_SCE_MODULE_INFO;
    int64_t t_orig = b->use_v1_tags ? DT_SCE_ORIGINAL_FILENAME_1 : DT_SCE_ORIGINAL_FILENAME;
    bool p5 = b->ps5_layout;
    put_dyn(&dyn, p5 ? DT_STRTAB : DT_SCE_STRTAB, tb + 0);
    put_dyn(&dyn, p5 ? DT_STRSZ : DT_SCE_STRSZ, strtab_size);
    put_dyn(&dyn, p5 ? DT_SYMTAB : DT_SCE_SYMTAB, tb + symtab_off);
    if (!b->omit_symtab_size) put_dyn(&dyn, DT_SCE_SYMTABSZ, symtab_size);
    put_dyn(&dyn, p5 ? DT_SYMENT : DT_SCE_SYMENT, sizeof(elf64_sym));
    put_dyn(&dyn, p5 ? DT_HASH : DT_SCE_HASH, tb + hash_off);
    put_dyn(&dyn, p5 ? DT_RELA : DT_SCE_RELA, tb + rela_off);
    put_dyn(&dyn, p5 ? DT_RELASZ : DT_SCE_RELASZ, n_rela * sizeof(elf64_rela));
    put_dyn(&dyn, p5 ? DT_RELAENT : DT_SCE_RELAENT, sizeof(elf64_rela));
    put_dyn(&dyn, p5 ? DT_JMPREL : DT_SCE_JMPREL, tb + jmprel_off);
    put_dyn(&dyn, p5 ? DT_PLTRELSZ : DT_SCE_PLTRELSZ, n_plt * sizeof(elf64_rela));
    put_dyn(&dyn, p5 ? DT_PLTREL : DT_SCE_PLTREL, DT_RELA);
    put_dyn(&dyn, p5 ? DT_PLTGOT : DT_SCE_PLTGOT, GOT_BASE);
    for (uint32_t i = 0; i < b->needed_count; i++)
        put_dyn(&dyn, t_needed, modval(needed_off[i], b->needed[i].version, b->needed[i].id));
    for (uint32_t i = 0; i < b->import_count; i++)
        put_dyn(&dyn, t_import, modval(import_off[i], b->imports[i].version, b->imports[i].id));
    for (uint32_t i = 0; i < b->export_count; i++)
        put_dyn(&dyn, t_export, modval(export_off[i], b->exports[i].version, b->exports[i].id));
    if (b->has_module_info)
        put_dyn(&dyn, t_modinfo, modval(mi_off, b->module_info.version, b->module_info.id));
    if (b->original_filename) put_dyn(&dyn, t_orig, orig_off);
    put_dyn(&dyn, DT_NULL, 0);

    /* file layout
     *   PS4: LOAD(code) | DYNAMIC | PT_SCE_DYNLIBDATA(tables), tables not loaded
     *   PS5: LOAD(code) | LOAD(tables + dynamic) with PT_DYNAMIC inside it */
    uint64_t tables_file_off = LOAD_FILE_OFF + SB_SEG_SIZE;
    uint64_t dyn_file_off, dynlib_file_off;
    if (b->ps5_layout) {
        dynlib_file_off = tables_file_off;
        dyn_file_off = PS5C_ALIGN_UP(dynlib_file_off + dynlib.len, 16);
    } else {
        dyn_file_off = tables_file_off;
        dynlib_file_off = PS5C_ALIGN_UP(dyn_file_off + dyn.len, 16);
    }
    size_t total = (size_t)PS5C_MAX(dyn_file_off + dyn.len, dynlib_file_off + dynlib.len);
    uint8_t *file = (uint8_t *)calloc(1, total);
    if (!file) {
        dyn_free(&strtab);
        dyn_free(&dynlib);
        dyn_free(&dyn);
        return NULL;
    }

    elf64_phdr ph[4];
    memset(ph, 0, sizeof(ph));
    uint32_t phnum = 0;
    elf64_phdr *code = &ph[phnum++];
    code->p_type = PT_LOAD;
    code->p_flags = PF_R | PF_W | PF_X;
    code->p_offset = LOAD_FILE_OFF;
    code->p_filesz = SB_SEG_SIZE;
    code->p_memsz = SB_SEG_SIZE + 0x1000; /* some .bss */
    code->p_align = 0x4000;
    if (b->ps5_layout) {
        elf64_phdr *tables = &ph[phnum++];
        tables->p_type = PT_LOAD;
        tables->p_flags = PF_R;
        tables->p_offset = dynlib_file_off;
        tables->p_vaddr = TABLES_VADDR;
        tables->p_filesz = tables->p_memsz = total - dynlib_file_off;
        tables->p_align = 16;
    }
    elf64_phdr *dynph = &ph[phnum++];
    dynph->p_type = PT_DYNAMIC;
    dynph->p_flags = PF_R;
    dynph->p_offset = dyn_file_off;
    dynph->p_vaddr = b->ps5_layout ? TABLES_VADDR + (dyn_file_off - dynlib_file_off) : 0;
    dynph->p_filesz = dynph->p_memsz = dyn.len;
    dynph->p_align = 8;
    if (!b->ps5_layout) {
        elf64_phdr *dl = &ph[phnum++];
        dl->p_type = PT_SCE_DYNLIBDATA;
        dl->p_flags = PF_R;
        dl->p_offset = dynlib_file_off;
        dl->p_filesz = dynlib.len;
        dl->p_align = 16;
    }
    if (b->with_tls) {
        elf64_phdr *tls = &ph[phnum++];
        tls->p_type = PT_TLS;
        tls->p_flags = PF_R;
        tls->p_offset = LOAD_FILE_OFF + TLS_VADDR;
        tls->p_vaddr = TLS_VADDR;
        tls->p_filesz = 0x10;
        tls->p_memsz = 0x40;
        tls->p_align = 16;
    }

    elf64_ehdr eh;
    memset(&eh, 0, sizeof(eh));
    memcpy(eh.e_ident, "" "ELF", 4);
    eh.e_ident[4] = ELF_CLASS64;
    eh.e_ident[5] = ELF_DATA_LSB;
    eh.e_ident[6] = 1;
    eh.e_ident[7] = ELF_OSABI_FREEBSD;
    eh.e_type = b->elf_type;
    eh.e_machine = EM_X86_64;
    eh.e_version = 1;
    eh.e_entry = b->entry;
    eh.e_phoff = sizeof(elf64_ehdr);
    eh.e_ehsize = sizeof(elf64_ehdr);
    eh.e_phentsize = sizeof(elf64_phdr);
    eh.e_phnum = (uint16_t)phnum;
    memcpy(file, &eh, sizeof(eh));
    memcpy(file + sizeof(eh), ph, phnum * sizeof(elf64_phdr));
    memcpy(file + LOAD_FILE_OFF, b->seg, SB_SEG_SIZE);
    memcpy(file + dyn_file_off, dyn.data, dyn.len);
    memcpy(file + dynlib_file_off, dynlib.data, dynlib.len);

    dyn_free(&strtab);
    dyn_free(&dynlib);
    dyn_free(&dyn);
    *size_out = total;
    return file;
}

uint8_t *sb_wrap_self(const uint8_t *elf, size_t elf_size, uint32_t magic, size_t *size_out) {
    elf64_ehdr eh;
    memcpy(&eh, elf, sizeof(eh));
    elf64_phdr ph[16];
    uint32_t phnum = PS5C_MIN(eh.e_phnum, 16u);
    memcpy(ph, elf + eh.e_phoff, phnum * sizeof(elf64_phdr));

    /* One blocked segment per program header with its own data (TLS lives
     * inside the LOAD range and is reached through it). */
    uint32_t seg_ids[16], nseg = 0;
    for (uint32_t i = 0; i < phnum; i++) {
        bool inside_load = false;
        for (uint32_t j = 0; j < phnum; j++) {
            if (j != i && ph[j].p_type == PT_LOAD && ph[i].p_offset >= ph[j].p_offset &&
                ph[i].p_offset + ph[i].p_filesz <= ph[j].p_offset + ph[j].p_filesz) {
                inside_load = true;
            }
        }
        if (ph[i].p_filesz && !inside_load) seg_ids[nseg++] = i;
    }
    size_t header_size = sizeof(sce_self_header) + nseg * sizeof(sce_self_segment);
    size_t elf_hdrs = (size_t)(eh.e_phoff + phnum * sizeof(elf64_phdr));
    size_t data_off = PS5C_ALIGN_UP(header_size + elf_hdrs, 16);
    size_t total = data_off;
    for (uint32_t s = 0; s < nseg; s++) total = PS5C_ALIGN_UP(total + ph[seg_ids[s]].p_filesz, 16);

    uint8_t *out = (uint8_t *)calloc(1, total);
    if (!out) return NULL;
    sce_self_header sh;
    memset(&sh, 0, sizeof(sh));
    sh.magic = magic;
    sh.mode = 1;
    sh.endian = 1;
    sh.attributes = 0x12;
    sh.key_type = 0x101;
    sh.header_size = (uint16_t)header_size;
    sh.file_size = total;
    sh.num_segments = (uint16_t)nseg;
    sh.unknown = 0x22;
    memcpy(out, &sh, sizeof(sh));
    memcpy(out + header_size, elf, elf_hdrs);

    size_t cur = data_off;
    for (uint32_t s = 0; s < nseg; s++) {
        const elf64_phdr *p = &ph[seg_ids[s]];
        sce_self_segment seg;
        seg.flags = SELF_SEG_BLOCKED | SELF_SEG_ORDERED | SELF_SEG_MAKE_ID(seg_ids[s]);
        seg.offset = cur;
        seg.compressed_size = seg.decompressed_size = p->p_filesz;
        memcpy(out + sizeof(sh) + s * sizeof(seg), &seg, sizeof(seg));
        if (p->p_offset + p->p_filesz <= elf_size) memcpy(out + cur, elf + p->p_offset, (size_t)p->p_filesz);
        cur = PS5C_ALIGN_UP(cur + p->p_filesz, 16);
    }
    *size_out = total;
    return out;
}
