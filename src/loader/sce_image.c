/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "loader/sce_image.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "base/endian.h"
#include "base/log.h"
#include "loader/nid.h"

#define MAX_PHDRS 256
#define MAX_SELF_SEGMENTS 1024

PS5C_PRINTF_FMT(2, 3)
static ps5c_err fail(sce_image *img, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(img->error, sizeof(img->error), fmt, ap);
    va_end(ap);
    return PS5C_ERR_INVALID;
}

static bool in_range(uint64_t off, uint64_t size, uint64_t limit) {
    return off <= limit && size <= limit - off;
}

/* ---------------------------------------------------------------------------
 * Container
 * ------------------------------------------------------------------------- */

typedef struct parse_ctx {
    sce_image *img;
    uint64_t elf_off; /* where the ELF header starts in the file */
    const sce_self_segment *self_segs;
    uint32_t self_seg_count;
    elf64_phdr *phdrs; /* raw program headers (arena) */
    uint32_t phnum;
    /* Dynamic section data */
    const elf64_dyn *dyn;
    size_t dyn_count;
    const uint8_t *dynlib;
    uint64_t dynlib_size;
    const char *strtab;
    uint64_t strtab_size;
} parse_ctx;

/* Translates an ELF file range to an offset in the input buffer. For a plain
 * ELF that's the identity; in a SELF it goes through the blocked segment that
 * holds the program header whose file range contains `off`. */
static bool map_file_range(const parse_ctx *c, uint64_t off, uint64_t size, uint64_t *out) {
    const sce_image *img = c->img;
    if (!c->self_segs) {
        if (!in_range(off, size, img->file_size)) {
            return false;
        }
        *out = off;
        return true;
    }
    for (uint32_t i = 0; i < c->self_seg_count; i++) {
        const sce_self_segment *s = &c->self_segs[i];
        if (!(s->flags & SELF_SEG_BLOCKED)) {
            continue;
        }
        uint32_t id = SELF_SEG_PHDR_ID(s->flags);
        if (id >= c->phnum) {
            continue;
        }
        const elf64_phdr *ph = &c->phdrs[id];
        if (off < ph->p_offset || !in_range(off - ph->p_offset, size, ph->p_filesz)) {
            continue;
        }
        uint64_t rel = off - ph->p_offset;
        if (!in_range(rel, size, s->decompressed_size) || s->offset > UINT64_MAX - rel ||
            !in_range(s->offset + rel, size, img->file_size)) {
            return false;
        }
        *out = s->offset + rel;
        return true;
    }
    return false;
}

static ps5c_err parse_container(parse_ctx *c) {
    sce_image *img = c->img;
    if (img->file_size < 4) {
        return fail(img, "file too small (%zu bytes)", img->file_size);
    }
    uint32_t magic = rd_le32(img->file);
    if (magic == SELF_MAGIC_PS4 || magic == SELF_MAGIC_PS5) {
        if (img->file_size < sizeof(sce_self_header)) {
            return fail(img, "truncated SELF header");
        }
        sce_self_header sh;
        memcpy(&sh, img->file, sizeof(sh));
        if (sh.num_segments == 0 || sh.num_segments > MAX_SELF_SEGMENTS) {
            return fail(img, "bad SELF segment count %u", sh.num_segments);
        }
        uint64_t seg_bytes = (uint64_t)sh.num_segments * sizeof(sce_self_segment);
        if (!in_range(sizeof(sh), seg_bytes, img->file_size)) {
            return fail(img, "SELF segment table out of bounds");
        }
        sce_self_segment *segs = ARENA_NEW_ARRAY(&img->mem, sce_self_segment, sh.num_segments);
        if (!segs) {
            return PS5C_ERR_NOMEM;
        }
        memcpy(segs, img->file + sizeof(sh), (size_t)seg_bytes);
        for (uint32_t i = 0; i < sh.num_segments; i++) {
            if (segs[i].flags & (SELF_SEG_ENCRYPTED | SELF_SEG_COMPRESSED)) {
                img->self_flag_warnings++;
            }
        }
        c->self_segs = segs;
        c->self_seg_count = sh.num_segments;
        c->elf_off = sizeof(sh) + seg_bytes;
        img->container = magic == SELF_MAGIC_PS4 ? SCE_CONTAINER_SELF_PS4 : SCE_CONTAINER_SELF_PS5;
    } else {
        c->elf_off = 0;
        img->container = SCE_CONTAINER_ELF;
    }
    return PS5C_OK;
}

static ps5c_err parse_headers(parse_ctx *c) {
    sce_image *img = c->img;
    if (!in_range(c->elf_off, sizeof(elf64_ehdr), img->file_size)) {
        return fail(img, "ELF header out of bounds");
    }
    elf64_ehdr eh;
    memcpy(&eh, img->file + c->elf_off, sizeof(eh));
    if (memcmp(eh.e_ident, "\x7F" "ELF", 4) != 0) {
        if (img->container == SCE_CONTAINER_ELF) {
            return fail(img, "unknown file format (magic %08X)", (unsigned)rd_le32(img->file));
        }
        return fail(img, "no ELF header inside SELF (encrypted file?)");
    }
    if (eh.e_ident[4] != ELF_CLASS64 || eh.e_ident[5] != ELF_DATA_LSB) {
        return fail(img, "not a little-endian ELF64 file");
    }
    if (eh.e_machine != EM_X86_64) {
        return fail(img, "unsupported machine %u", eh.e_machine);
    }
    if (eh.e_phentsize != sizeof(elf64_phdr) || eh.e_phnum == 0 || eh.e_phnum > MAX_PHDRS) {
        return fail(img, "bad program header table (entsize %u, count %u)", eh.e_phentsize, eh.e_phnum);
    }
    uint64_t ph_bytes = (uint64_t)eh.e_phnum * sizeof(elf64_phdr);
    if (eh.e_phoff > img->file_size || !in_range(c->elf_off + eh.e_phoff, ph_bytes, img->file_size)) {
        return fail(img, "program headers out of bounds");
    }

    img->elf_type = eh.e_type;
    img->osabi = eh.e_ident[7];
    img->entry = eh.e_entry;

    c->phnum = eh.e_phnum;
    c->phdrs = ARENA_NEW_ARRAY(&img->mem, elf64_phdr, c->phnum);
    img->segments = ARENA_NEW_ARRAY(&img->mem, sce_segment, c->phnum);
    if (!c->phdrs || !img->segments) {
        return PS5C_ERR_NOMEM;
    }
    memcpy(c->phdrs, img->file + c->elf_off + eh.e_phoff, (size_t)ph_bytes);
    img->segment_count = c->phnum;

    uint64_t lo = UINT64_MAX, hi = 0;
    for (uint32_t i = 0; i < c->phnum; i++) {
        const elf64_phdr *ph = &c->phdrs[i];
        sce_segment *seg = &img->segments[i];
        seg->h = *ph;
        seg->has_data = ph->p_filesz != 0 && map_file_range(c, ph->p_offset, ph->p_filesz, &seg->data_off);

        if (ph->p_memsz > UINT64_MAX - ph->p_vaddr || ph->p_filesz > ph->p_memsz) {
            if (ph->p_type == PT_LOAD || ph->p_type == PT_SCE_RELRO) {
                return fail(img, "segment %u has inconsistent sizes", i);
            }
        }

        switch (ph->p_type) {
        case PT_LOAD:
        case PT_SCE_RELRO:
            if (ph->p_memsz != 0) {
                lo = PS5C_MIN(lo, ph->p_vaddr);
                hi = PS5C_MAX(hi, ph->p_vaddr + ph->p_memsz);
            }
            if (ph->p_filesz != 0 && !seg->has_data) {
                return fail(img, "loadable segment %u has no data in the file", i);
            }
            break;
        case PT_DYNAMIC:
            if (seg->has_data) {
                c->dyn = (const elf64_dyn *)(const void *)(img->file + seg->data_off);
                c->dyn_count = (size_t)(ph->p_filesz / sizeof(elf64_dyn));
            }
            img->has_dynamic = true;
            break;
        case PT_SCE_DYNLIBDATA:
            if (seg->has_data) {
                c->dynlib = img->file + seg->data_off;
                c->dynlib_size = ph->p_filesz;
            }
            break;
        case PT_TLS:
            img->has_tls = true;
            img->tls_vaddr = ph->p_vaddr;
            img->tls_filesz = ph->p_filesz;
            img->tls_memsz = ph->p_memsz;
            img->tls_align = ph->p_align ? ph->p_align : 1;
            break;
        case PT_SCE_PROCPARAM:
            img->procparam_vaddr = ph->p_vaddr;
            img->procparam_size = ph->p_memsz;
            break;
        case PT_GNU_EH_FRAME:
            img->eh_frame_hdr_vaddr = ph->p_vaddr;
            img->eh_frame_hdr_size = ph->p_memsz;
            break;
        default: break;
        }
    }
    if (hi != 0) {
        img->image_lo = lo;
        img->image_hi = hi;
    }
    return PS5C_OK;
}

/* ---------------------------------------------------------------------------
 * Dynamic section
 * ------------------------------------------------------------------------- */

static const char *dyn_str(parse_ctx *c, uint64_t off) {
    if (off >= c->strtab_size) {
        return NULL;
    }
    /* Must be NUL-terminated inside the table. */
    const char *s = c->strtab + off;
    if (!memchr(s, '\0', (size_t)(c->strtab_size - off))) {
        return NULL;
    }
    return arena_strdup(&c->img->mem, s);
}

static uint64_t dyn_read(const elf64_dyn *d, size_t i, int64_t *tag) {
    /* The dynamic table may be unaligned inside the file buffer. */
    const uint8_t *p = (const uint8_t *)(const void *)d + i * sizeof(elf64_dyn);
    *tag = (int64_t)rd_le64(p);
    return rd_le64(p + 8);
}

static uint32_t count_tag(parse_ctx *c, int64_t want_a, int64_t want_b) {
    uint32_t n = 0;
    for (size_t i = 0; i < c->dyn_count; i++) {
        int64_t tag;
        dyn_read(c->dyn, i, &tag);
        if (tag == DT_NULL) break;
        if (tag == want_a || tag == want_b) n++;
    }
    return n;
}

typedef struct table_ref {
    bool set;
    bool is_vaddr; /* PS5: virtual address; PS4: offset into PT_SCE_DYNLIBDATA */
    uint64_t value;
} table_ref;

static void set_ref(table_ref *r, uint64_t v, bool is_vaddr) {
    r->set = true;
    r->is_vaddr = is_vaddr;
    r->value = v;
}

static void set_ref_if_unset(table_ref *r, uint64_t v, bool is_vaddr) {
    if (!r->set) {
        set_ref(r, v, is_vaddr);
    }
}

/* Returns a pointer into the file buffer for `size` bytes of a table, or NULL. */
static const uint8_t *resolve_table(const parse_ctx *c, const table_ref *r, uint64_t size) {
    if (!r->set) {
        return NULL;
    }
    if (!r->is_vaddr) {
        return c->dynlib && in_range(r->value, size, c->dynlib_size) ? c->dynlib + r->value : NULL;
    }
    for (uint32_t i = 0; i < c->phnum; i++) {
        const elf64_phdr *ph = &c->phdrs[i];
        if (ph->p_type != PT_LOAD && ph->p_type != PT_SCE_RELRO) {
            continue;
        }
        if (r->value < ph->p_vaddr || !in_range(r->value - ph->p_vaddr, size, ph->p_filesz)) {
            continue;
        }
        uint64_t off;
        if (map_file_range(c, ph->p_offset + (r->value - ph->p_vaddr), size, &off)) {
            return c->img->file + off;
        }
    }
    return NULL;
}

static ps5c_err parse_dynamic(parse_ctx *c) {
    sce_image *img = c->img;
    if (!img->has_dynamic) {
        return PS5C_OK;
    }
    if (!c->dyn) {
        return fail(img, "PT_DYNAMIC has no data in the file");
    }
    /* Table locations come in two flavors:
     *   PS4: DT_SCE_* tags, offsets into PT_SCE_DYNLIBDATA (not loaded)
     *   PS5: standard DT_* tags, virtual addresses inside a loadable segment
     * A file uses one or the other; the Sony tag wins if both appear. */
    table_ref strtab = {0}, symtab = {0}, rela = {0}, jmprel = {0}, hash = {0};
    uint64_t symtab_size = 0, syment = sizeof(elf64_sym), relaent = sizeof(elf64_rela);
    uint64_t rela_size = 0, jmprel_size = 0, sce_strsz = 0, std_strsz = 0;

    for (size_t i = 0; i < c->dyn_count; i++) {
        int64_t tag;
        uint64_t v = dyn_read(c->dyn, i, &tag);
        if (tag == DT_NULL) break;
        switch (tag) {
        case DT_SCE_STRTAB: set_ref(&strtab, v, false); break;
        case DT_STRTAB: set_ref_if_unset(&strtab, v, true); break;
        case DT_SCE_STRSZ: sce_strsz = v; break;
        case DT_STRSZ: std_strsz = v; break;
        case DT_SCE_SYMTAB: set_ref(&symtab, v, false); break;
        case DT_SYMTAB: set_ref_if_unset(&symtab, v, true); break;
        case DT_SCE_SYMTABSZ: symtab_size = v; break;
        case DT_SCE_SYMENT:
        case DT_SYMENT: syment = v; break;
        case DT_SCE_HASH: set_ref(&hash, v, false); break;
        case DT_HASH: set_ref_if_unset(&hash, v, true); break;
        case DT_SCE_RELA: set_ref(&rela, v, false); break;
        case DT_RELA: set_ref_if_unset(&rela, v, true); break;
        case DT_SCE_RELASZ:
        case DT_RELASZ: rela_size = v; break;
        case DT_SCE_RELAENT:
        case DT_RELAENT: relaent = v; break;
        case DT_SCE_JMPREL: set_ref(&jmprel, v, false); break;
        case DT_JMPREL: set_ref_if_unset(&jmprel, v, true); break;
        case DT_SCE_PLTRELSZ:
        case DT_PLTRELSZ: jmprel_size = v; break;
        case DT_SCE_PLTGOT:
        case DT_PLTGOT: img->pltgot_vaddr = v; break;
        case DT_INIT: img->init_vaddr = v, img->has_init = true; break;
        case DT_FINI: img->fini_vaddr = v, img->has_fini = true; break;
        case DT_INIT_ARRAY: img->init_array_vaddr = v; break;
        case DT_INIT_ARRAYSZ: img->init_array_size = v; break;
        case DT_FINI_ARRAY: img->fini_array_vaddr = v; break;
        case DT_FINI_ARRAYSZ: img->fini_array_size = v; break;
        case DT_PREINIT_ARRAY: img->preinit_array_vaddr = v; break;
        case DT_PREINIT_ARRAYSZ: img->preinit_array_size = v; break;
        default: break;
        }
    }
    img->tables_in_memory = strtab.is_vaddr;

    if (strtab.is_vaddr) {
        c->strtab_size = std_strsz ? std_strsz : sce_strsz;
    } else {
        c->strtab_size = sce_strsz ? sce_strsz : std_strsz;
    }
    c->strtab = (const char *)resolve_table(c, &strtab, c->strtab_size);
    if (!c->strtab) {
        return fail(img, "missing or invalid string table");
    }
    if (syment != sizeof(elf64_sym) || relaent != sizeof(elf64_rela)) {
        return fail(img, "unexpected table entry sizes (sym %llu, rela %llu)",
                    (unsigned long long)syment, (unsigned long long)relaent);
    }

    /* Symbol count: DT_SCE_SYMTABSZ, or nchain from the ELF hash table. */
    if (symtab_size == 0 && hash.set) {
        const uint8_t *h = resolve_table(c, &hash, 8);
        if (h) {
            symtab_size = (uint64_t)rd_le32(h + 4) * sizeof(elf64_sym);
        }
    }

    /* Symbols */
    if (symtab.set && symtab_size != 0) {
        const uint8_t *syms = resolve_table(c, &symtab, symtab_size);
        if (!syms) {
            return fail(img, "symbol table out of bounds");
        }
        uint32_t n = (uint32_t)PS5C_MIN(symtab_size / sizeof(elf64_sym), (uint64_t)UINT32_MAX);
        img->syms = ARENA_NEW_ARRAY(&img->mem, sce_sym, n);
        if (!img->syms) {
            return PS5C_ERR_NOMEM;
        }
        for (uint32_t i = 0; i < n; i++) {
            elf64_sym raw;
            memcpy(&raw, syms + (uint64_t)i * sizeof(raw), sizeof(raw));
            sce_sym *s = &img->syms[i];
            s->name = raw.st_name ? dyn_str(c, raw.st_name) : "";
            if (!s->name) {
                return fail(img, "symbol %u has an invalid name offset", i);
            }
            s->value = raw.st_value;
            s->size = raw.st_size;
            s->shndx = raw.st_shndx;
            s->bind = ELF64_ST_BIND(raw.st_info);
            s->type = ELF64_ST_TYPE(raw.st_info);
            nid_symbol ns;
            if (nid_parse_symbol(s->name, &ns)) {
                s->has_nid = true;
                s->nid = ns.nid;
                s->lib_id = ns.lib_id;
                s->mod_id = ns.mod_id;
            }
        }
        img->sym_count = n;
    }

    /* Relocations: RELA entries first, then JMPREL */
    const uint8_t *rela_data = NULL, *jmprel_data = NULL;
    if (rela.set && rela_size && !(rela_data = resolve_table(c, &rela, rela_size))) {
        return fail(img, "RELA table out of bounds");
    }
    if (jmprel.set && jmprel_size && !(jmprel_data = resolve_table(c, &jmprel, jmprel_size))) {
        return fail(img, "JMPREL table out of bounds");
    }
    uint64_t n_rela = rela_data ? rela_size / sizeof(elf64_rela) : 0;
    uint64_t n_jmp = jmprel_data ? jmprel_size / sizeof(elf64_rela) : 0;
    if (n_rela + n_jmp > UINT32_MAX) {
        return fail(img, "too many relocations");
    }
    img->reloc_count = (uint32_t)(n_rela + n_jmp);
    if (img->reloc_count) {
        img->relocs = ARENA_NEW_ARRAY(&img->mem, sce_reloc, img->reloc_count);
        if (!img->relocs) {
            return PS5C_ERR_NOMEM;
        }
    }
    for (uint32_t i = 0; i < img->reloc_count; i++) {
        bool plt = i >= n_rela;
        const uint8_t *src = plt ? jmprel_data + (i - n_rela) * sizeof(elf64_rela)
                                 : rela_data + (uint64_t)i * sizeof(elf64_rela);
        elf64_rela raw;
        memcpy(&raw, src, sizeof(raw));
        sce_reloc *r = &img->relocs[i];
        r->offset = raw.r_offset;
        r->addend = raw.r_addend;
        r->type = ELF64_R_TYPE(raw.r_info);
        r->sym = ELF64_R_SYM(raw.r_info);
        r->plt = plt;
        if (r->sym != 0 && r->sym >= img->sym_count) {
            return fail(img, "relocation %u references symbol %u of %u", i, r->sym, img->sym_count);
        }
    }

    /* Module and library tables, plus strings */
    img->needed_modules = ARENA_NEW_ARRAY(&img->mem, sce_modref,
                                          count_tag(c, DT_SCE_NEEDED_MODULE, DT_SCE_NEEDED_MODULE_1));
    img->import_libs = ARENA_NEW_ARRAY(&img->mem, sce_libref,
                                       count_tag(c, DT_SCE_IMPORT_LIB, DT_SCE_IMPORT_LIB_1));
    img->export_libs = ARENA_NEW_ARRAY(&img->mem, sce_libref,
                                       count_tag(c, DT_SCE_EXPORT_LIB, DT_SCE_EXPORT_LIB_1));
    img->needed = ARENA_NEW_ARRAY(&img->mem, const char *, count_tag(c, DT_NEEDED, DT_NEEDED));
    if (!img->needed_modules || !img->import_libs || !img->export_libs || !img->needed) {
        return PS5C_ERR_NOMEM;
    }

    for (size_t i = 0; i < c->dyn_count; i++) {
        int64_t tag;
        uint64_t v = dyn_read(c->dyn, i, &tag);
        if (tag == DT_NULL) break;
        const char *name = NULL;
        bool named = tag == DT_SCE_NEEDED_MODULE || tag == DT_SCE_NEEDED_MODULE_1 ||
                     tag == DT_SCE_MODULE_INFO || tag == DT_SCE_MODULE_INFO_1 ||
                     tag == DT_SCE_IMPORT_LIB || tag == DT_SCE_IMPORT_LIB_1 ||
                     tag == DT_SCE_EXPORT_LIB || tag == DT_SCE_EXPORT_LIB_1 ||
                     tag == DT_SCE_ORIGINAL_FILENAME || tag == DT_SCE_ORIGINAL_FILENAME_1 ||
                     tag == DT_NEEDED || tag == DT_SONAME;
        if (!named) {
            continue;
        }
        name = dyn_str(c, SCE_DYN_NAME_OFF(v));
        if (!name) {
            return fail(img, "dynamic tag 0x%llx has an invalid string offset", (unsigned long long)tag);
        }
        sce_modref mod = {name, SCE_DYN_ID(v), SCE_DYN_MOD_MAJOR(v), SCE_DYN_MOD_MINOR(v)};
        sce_libref lib = {name, SCE_DYN_ID(v), SCE_DYN_LIB_VERSION(v)};
        switch (tag) {
        case DT_SCE_NEEDED_MODULE:
        case DT_SCE_NEEDED_MODULE_1: img->needed_modules[img->needed_module_count++] = mod; break;
        case DT_SCE_MODULE_INFO:
        case DT_SCE_MODULE_INFO_1:
            img->module_info = mod;
            img->has_module_info = true;
            break;
        case DT_SCE_IMPORT_LIB:
        case DT_SCE_IMPORT_LIB_1: img->import_libs[img->import_lib_count++] = lib; break;
        case DT_SCE_EXPORT_LIB:
        case DT_SCE_EXPORT_LIB_1: img->export_libs[img->export_lib_count++] = lib; break;
        case DT_SCE_ORIGINAL_FILENAME:
        case DT_SCE_ORIGINAL_FILENAME_1: img->original_filename = name; break;
        case DT_NEEDED: img->needed[img->needed_count++] = name; break;
        case DT_SONAME: img->soname = name; break;
        default: break;
        }
    }
    return PS5C_OK;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

ps5c_err sce_image_parse(sce_image *img, const uint8_t *data, size_t size) {
    memset(img, 0, sizeof(*img));
    arena_init(&img->mem, 16 * 1024);
    img->file = data;
    img->file_size = size;

    parse_ctx c;
    memset(&c, 0, sizeof(c));
    c.img = img;

    ps5c_err err = parse_container(&c);
    if (err == PS5C_OK) err = parse_headers(&c);
    if (err == PS5C_OK) err = parse_dynamic(&c);
    if (err != PS5C_OK) {
        if (err == PS5C_ERR_NOMEM) {
            snprintf(img->error, sizeof(img->error), "out of memory");
        }
        LOG_D(LOADER, "parse failed: %s", img->error);
        return err;
    }
    if (img->self_flag_warnings) {
        LOG_W(LOADER, "%u SELF segments are flagged encrypted/compressed; only decrypted files load",
              img->self_flag_warnings);
    }
    return PS5C_OK;
}

void sce_image_free(sce_image *img) {
    arena_free(&img->mem);
    memset(img, 0, sizeof(*img));
}

bool sce_image_is_library(const sce_image *img) { return img->elf_type == ET_SCE_DYNAMIC; }

const sce_libref *sce_image_find_import_lib(const sce_image *img, uint16_t id) {
    for (uint32_t i = 0; i < img->import_lib_count; i++) {
        if (img->import_libs[i].id == id) return &img->import_libs[i];
    }
    return NULL;
}

const sce_libref *sce_image_find_export_lib(const sce_image *img, uint16_t id) {
    for (uint32_t i = 0; i < img->export_lib_count; i++) {
        if (img->export_libs[i].id == id) return &img->export_libs[i];
    }
    return NULL;
}

const char *sce_image_sym_lib_name(const sce_image *img, const sce_sym *sym) {
    if (!sym->has_nid) {
        return NULL;
    }
    const sce_libref *lib = sym->shndx == SHN_UNDEF ? sce_image_find_import_lib(img, sym->lib_id)
                                                   : sce_image_find_export_lib(img, sym->lib_id);
    return lib ? lib->name : NULL;
}

const char *sce_container_name(sce_container c) {
    switch (c) {
    case SCE_CONTAINER_ELF: return "ELF";
    case SCE_CONTAINER_SELF_PS4: return "SELF (PS4, decrypted)";
    case SCE_CONTAINER_SELF_PS5: return "SELF (PS5, decrypted)";
    default: return "?";
    }
}

const char *sce_elf_type_name(uint16_t type) {
    switch (type) {
    case ET_EXEC: return "EXEC";
    case ET_DYN: return "DYN";
    case ET_SCE_EXEC: return "SCE_EXEC";
    case ET_SCE_REPLAY_EXEC: return "SCE_REPLAY_EXEC";
    case ET_SCE_RELEXEC: return "SCE_RELEXEC";
    case ET_SCE_STUBLIB: return "SCE_STUBLIB";
    case ET_SCE_DYNEXEC: return "SCE_DYNEXEC";
    case ET_SCE_DYNAMIC: return "SCE_DYNAMIC";
    default: return "UNKNOWN";
    }
}

const char *sce_phdr_type_name(uint32_t type) {
    switch (type) {
    case PT_NULL: return "NULL";
    case PT_LOAD: return "LOAD";
    case PT_DYNAMIC: return "DYNAMIC";
    case PT_INTERP: return "INTERP";
    case PT_NOTE: return "NOTE";
    case PT_TLS: return "TLS";
    case PT_GNU_EH_FRAME: return "GNU_EH_FRAME";
    case PT_GNU_RELRO: return "GNU_RELRO";
    case PT_SCE_RELA: return "SCE_RELA";
    case PT_SCE_DYNLIBDATA: return "SCE_DYNLIBDATA";
    case PT_SCE_PROCPARAM: return "SCE_PROCPARAM";
    case PT_SCE_MODULE_PARAM: return "SCE_MODULE_PARAM";
    case PT_SCE_RELRO: return "SCE_RELRO";
    case PT_SCE_COMMENT: return "SCE_COMMENT";
    case PT_SCE_LIBVERSION: return "SCE_LIBVERSION";
    default: return "UNKNOWN";
    }
}

const char *sce_reloc_type_name(uint32_t type) {
    switch (type) {
    case R_X86_64_NONE: return "NONE";
    case R_X86_64_64: return "64";
    case R_X86_64_GLOB_DAT: return "GLOB_DAT";
    case R_X86_64_JUMP_SLOT: return "JUMP_SLOT";
    case R_X86_64_RELATIVE: return "RELATIVE";
    case R_X86_64_DTPMOD64: return "DTPMOD64";
    case R_X86_64_DTPOFF64: return "DTPOFF64";
    case R_X86_64_TPOFF64: return "TPOFF64";
    default: return "OTHER";
    }
}
