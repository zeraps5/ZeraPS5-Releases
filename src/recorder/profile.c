/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "recorder/profile.h"

#include <stdlib.h>
#include <string.h>

#include "base/hashmap.h"
#include "loader/nid.h"
#include "ps5c/version.h"

typedef struct import_entry {
    const char *library; /* NULL if the lib id isn't in the import table */
    uint16_t lib_id;
    uint64_t nid;
    const char *name; /* from the NID database, or NULL */
    uint8_t type;
    uint32_t module_refs;
    import_resolution res;
} import_entry;

static const char *sym_type_name(uint8_t type) {
    switch (type) {
    case STT_NOTYPE: return "notype";
    case STT_OBJECT: return "object";
    case STT_FUNC: return "function";
    case STT_TLS: return "tls";
    default: return "other";
    }
}

static int cmp_str_null(const char *a, const char *b) {
    if (!a || !b) return (a != NULL) - (b != NULL);
    return strcmp(a, b);
}

static int import_cmp(const void *pa, const void *pb) {
    const import_entry *a = (const import_entry *)pa, *b = (const import_entry *)pb;
    int c = cmp_str_null(a->library, b->library);
    if (c) return c;
    /* Named symbols first, alphabetically; then unnamed by NID. */
    if ((a->name != NULL) != (b->name != NULL)) return a->name ? -1 : 1;
    if (a->name && (c = strcmp(a->name, b->name)) != 0) return c;
    return a->nid < b->nid ? -1 : a->nid > b->nid;
}

static uint64_t lib_hash(const char *s) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (; s && *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001B3ull;
    return h;
}

static void write_modref_array(json_writer *w, const char *key, const sce_modref *mods, uint32_t n) {
    json_key(w, key);
    json_begin_array(w);
    for (uint32_t i = 0; i < n; i++) {
        json_begin_object(w);
        json_kv_string(w, "name", mods[i].name);
        json_kv_u64(w, "id", mods[i].id);
        json_kv_u64(w, "version_major", mods[i].ver_major);
        json_kv_u64(w, "version_minor", mods[i].ver_minor);
        json_end_object(w);
    }
    json_end_array(w);
}

static void write_libref_array(json_writer *w, const char *key, const sce_libref *libs, uint32_t n) {
    json_key(w, key);
    json_begin_array(w);
    for (uint32_t i = 0; i < n; i++) {
        json_begin_object(w);
        json_kv_string(w, "name", libs[i].name);
        json_kv_u64(w, "id", libs[i].id);
        json_kv_u64(w, "version", libs[i].version);
        json_end_object(w);
    }
    json_end_array(w);
}

static void write_module(json_writer *w, const loaded_module *m) {
    const sce_image *img = &m->img;
    json_begin_object(w);
    json_kv_string(w, "name", m->name);
    json_kv_string(w, "path", m->path);
    json_kv_string(w, "role", m->is_main ? "main" : "library");
    json_kv_string(w, "container", sce_container_name(img->container));
    json_kv_string(w, "elf_type", sce_elf_type_name(img->elf_type));
    json_kv_string(w, "dynamic_layout", !img->has_dynamic ? "none" : img->tables_in_memory ? "ps5" : "ps4");
    json_kv_hex(w, "entry", img->entry);
    json_kv_string(w, "original_filename", img->original_filename);
    json_kv_hex(w, "image_size", img->image_hi - img->image_lo);
    if (img->procparam_vaddr) {
        json_kv_hex(w, "procparam_vaddr", img->procparam_vaddr);
    }

    json_key(w, "segments");
    json_begin_array(w);
    for (uint32_t i = 0; i < img->segment_count; i++) {
        const elf64_phdr *ph = &img->segments[i].h;
        char flags[4] = {ph->p_flags & PF_R ? 'R' : '-', ph->p_flags & PF_W ? 'W' : '-',
                         ph->p_flags & PF_X ? 'X' : '-', '\0'};
        json_begin_object(w);
        json_kv_string(w, "type", sce_phdr_type_name(ph->p_type));
        json_kv_string(w, "flags", flags);
        json_kv_hex(w, "vaddr", ph->p_vaddr);
        json_kv_hex(w, "memsz", ph->p_memsz);
        json_kv_hex(w, "filesz", ph->p_filesz);
        json_end_object(w);
    }
    json_end_array(w);

    json_key(w, "tls");
    if (img->has_tls) {
        json_begin_object(w);
        json_kv_hex(w, "vaddr", img->tls_vaddr);
        json_kv_hex(w, "filesz", img->tls_filesz);
        json_kv_hex(w, "memsz", img->tls_memsz);
        json_kv_u64(w, "align", img->tls_align);
        json_end_object(w);
    } else {
        json_null(w);
    }

    write_modref_array(w, "needed_modules", img->needed_modules, img->needed_module_count);
    write_libref_array(w, "import_libraries", img->import_libs, img->import_lib_count);
    write_libref_array(w, "export_libraries", img->export_libs, img->export_lib_count);

    /* Relocation histogram: tells which relocation types the linker must support. */
    uint32_t hist[32] = {0}, other = 0;
    for (uint32_t i = 0; i < img->reloc_count; i++) {
        if (img->relocs[i].type < 32) hist[img->relocs[i].type]++;
        else other++;
    }
    json_key(w, "relocations");
    json_begin_object(w);
    json_kv_u64(w, "total", img->reloc_count);
    for (uint32_t t = 0; t < 32; t++) {
        if (hist[t]) json_kv_u64(w, sce_reloc_type_name(t), hist[t]);
    }
    if (other) json_kv_u64(w, "unknown", other);
    json_end_object(w);

    uint32_t exported = 0;
    for (uint32_t i = 0; i < img->sym_count; i++) {
        if (img->syms[i].shndx != SHN_UNDEF && img->syms[i].has_nid) exported++;
    }
    json_kv_u64(w, "exported_symbols", exported);
    json_end_object(w);
}

ps5c_err profile_write_static(json_writer *w, const module_set *ms, const nid_db *db,
                              profile_summary *summary_out) {
    profile_summary sum;
    memset(&sum, 0, sizeof(sum));

    /* Collect unique (library, nid) imports across all modules. */
    DYNARRAY(import_entry) imports = {0};
    hmap64 index;
    hmap64_init(&index);
    ps5c_err err = PS5C_OK;

    for (size_t mi = 0; mi < ms->modules.len && err == PS5C_OK; mi++) {
        const loaded_module *m = ms->modules.data[mi];
        for (uint32_t si = 0; si < m->img.sym_count; si++) {
            const sce_sym *s = &m->img.syms[si];
            if (s->shndx != SHN_UNDEF || !s->has_nid) {
                continue;
            }
            const char *lib = sce_image_sym_lib_name(&m->img, s);
            uint64_t key = s->nid ^ hash_u64(lib_hash(lib) + (lib ? 0 : s->lib_id));
            uint64_t idx;
            if (hmap64_get(&index, key, &idx) && imports.data[idx].nid == s->nid &&
                cmp_str_null(imports.data[idx].library, lib) == 0) {
                imports.data[idx].module_refs++;
                continue;
            }
            import_entry e;
            memset(&e, 0, sizeof(e));
            e.library = lib;
            e.lib_id = s->lib_id;
            e.nid = s->nid;
            e.name = db ? nid_db_lookup(db, s->nid) : NULL;
            e.type = s->type;
            e.module_refs = 1;
            e.res = lib ? module_set_resolve(ms, lib, s->nid) : (import_resolution){0};
            if (!dyn_push(&imports, e) || !hmap64_put(&index, key, imports.len - 1)) {
                err = PS5C_ERR_NOMEM;
                break;
            }
        }
    }
    hmap64_free(&index);
    if (err != PS5C_OK) {
        dyn_free(&imports);
        return err;
    }
    if (imports.len) {
        qsort(imports.data, imports.len, sizeof(import_entry), import_cmp);
    }

    sum.modules_loaded = (uint32_t)ms->modules.len;
    sum.imports_unique = (uint32_t)imports.len;
    for (size_t i = 0; i < imports.len; i++) {
        sum.by_provider[imports.data[i].res.kind]++;
        if (imports.data[i].name) sum.imports_named++;
    }

    json_begin_object(w);
    json_kv_u64(w, "schema", PROFILE_SCHEMA_VERSION);
    json_kv_string(w, "generator", "ps5c " PS5C_VERSION_STRING);

    /* Needed modules that nobody provides. */
    json_key(w, "missing_modules");
    json_begin_array(w);
    for (size_t mi = 0; mi < ms->modules.len; mi++) {
        const sce_image *img = &ms->modules.data[mi]->img;
        for (uint32_t n = 0; n < img->needed_module_count; n++) {
            const char *name = img->needed_modules[n].name;
            if (module_set_find_by_name(ms, name) ||
                module_set_get_policy(ms, name) == LIB_POLICY_HLE) {
                continue;
            }
            /* Report each name once. */
            bool seen = false;
            for (size_t pj = 0; pj <= mi && !seen; pj++) {
                const sce_image *prev = &ms->modules.data[pj]->img;
                uint32_t limit = pj == mi ? n : prev->needed_module_count;
                for (uint32_t k = 0; k < limit; k++) {
                    if (strcmp(prev->needed_modules[k].name, name) == 0) {
                        seen = true;
                        break;
                    }
                }
            }
            if (!seen) {
                json_string(w, name);
                sum.modules_missing++;
            }
        }
    }
    json_end_array(w);

    json_key(w, "modules");
    json_begin_array(w);
    for (size_t mi = 0; mi < ms->modules.len; mi++) {
        write_module(w, ms->modules.data[mi]);
    }
    json_end_array(w);

    /* Imports grouped by library (the array is sorted by library). */
    json_key(w, "libraries");
    json_begin_array(w);
    size_t i = 0;
    while (i < imports.len) {
        const char *lib = imports.data[i].library;
        size_t j = i;
        uint32_t counts[3] = {0, 0, 0};
        while (j < imports.len && cmp_str_null(imports.data[j].library, lib) == 0) {
            counts[imports.data[j].res.kind]++;
            j++;
        }
        const import_resolution *r0 = &imports.data[i].res;
        json_begin_object(w);
        if (lib) {
            json_kv_string(w, "name", lib);
        } else {
            json_key(w, "name");
            json_null(w);
        }
        json_kv_string(w, "policy", lib_policy_name(r0->policy));
        json_kv_bool(w, "lle_available", r0->lle_available);
        json_kv_u64(w, "imports", j - i);
        json_kv_u64(w, "hle", counts[PROVIDER_HLE]);
        json_kv_u64(w, "lle", counts[PROVIDER_LLE]);
        json_kv_u64(w, "missing", counts[PROVIDER_MISSING]);
        json_key(w, "symbols");
        json_begin_array(w);
        for (size_t k = i; k < j; k++) {
            const import_entry *e = &imports.data[k];
            char nid[NID_STR_LEN + 1];
            nid_to_str(e->nid, nid);
            json_begin_object(w);
            json_kv_string(w, "nid", nid);
            json_kv_string(w, "name", e->name);
            json_kv_string(w, "type", sym_type_name(e->type));
            json_kv_string(w, "provider", provider_kind_name(e->res.kind));
            if (e->res.kind == PROVIDER_LLE) {
                json_kv_string(w, "provider_module", e->res.module->name);
            }
            json_kv_u64(w, "referenced_by_modules", e->module_refs);
            json_end_object(w);
        }
        json_end_array(w);
        json_end_object(w);
        i = j;
    }
    json_end_array(w);

    json_key(w, "summary");
    json_begin_object(w);
    json_kv_u64(w, "modules_loaded", sum.modules_loaded);
    json_kv_u64(w, "modules_missing", sum.modules_missing);
    json_kv_u64(w, "imports_unique", sum.imports_unique);
    json_kv_u64(w, "imports_named", sum.imports_named);
    json_kv_u64(w, "provider_hle", sum.by_provider[PROVIDER_HLE]);
    json_kv_u64(w, "provider_lle", sum.by_provider[PROVIDER_LLE]);
    json_kv_u64(w, "provider_missing", sum.by_provider[PROVIDER_MISSING]);
    json_end_object(w);

    json_end_object(w);
    dyn_free(&imports);

    if (summary_out) {
        *summary_out = sum;
    }
    return json_result(w, NULL) ? PS5C_OK : PS5C_ERR_NOMEM;
}
