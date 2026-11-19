/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "loader/linker.h"

#include <string.h>

#include "base/log.h"
#include "loader/nid.h"

#define MODULE_ALIGN 0x100000ull /* modules start on 1 MiB boundaries, with a guard gap */

static uint64_t fnv1a(const char *s) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001B3ull;
    return h;
}

static uint32_t gprot_from_pflags(uint32_t pf) {
    uint32_t p = GPROT_NONE;
    if (pf & PF_R) p |= GPROT_READ;
    if (pf & PF_W) p |= GPROT_WRITE | GPROT_READ;
    if (pf & PF_X) p |= GPROT_EXEC | GPROT_READ; /* hosts can't do execute-only */
    return p;
}

ps5c_err linker_init(linker *l, gmem *mem, module_set *ms) {
    memset(l, 0, sizeof(*l));
    l->mem = mem;
    l->ms = ms;
    l->next_tls_index = 1;
    hmap64_init(&l->stub_index);

    ps5c_err err = gmem_map(mem, 0, IMPORT_CODE_AREA_SIZE, GPROT_RW | GPROT_EXEC, 0, "import stubs",
                            &l->import_code);
    if (err == PS5C_OK) {
        err = gmem_map(mem, 0, IMPORT_DATA_AREA_SIZE, GPROT_RW, 0, "import data", &l->import_data);
    }
    if (err != PS5C_OK) {
        LOG_E(LOADER, "cannot allocate import areas: %s", ps5c_err_str(err));
        return err;
    }
    /* int3 everywhere until the CPU backend installs real thunks. */
    memset(gmem_host(mem, l->import_code, IMPORT_CODE_AREA_SIZE), 0xCC, (size_t)IMPORT_CODE_AREA_SIZE);
    return PS5C_OK;
}

void linker_free(linker *l) {
    dyn_free(&l->stubs);
    hmap64_free(&l->stub_index);
}

const import_stub *linker_find_stub(const linker *l, guest_addr addr) {
    for (size_t i = 0; i < l->stubs.len; i++) {
        const import_stub *s = &l->stubs.data[i];
        uint64_t size = s->is_data ? IMPORT_DATA_SLOT_SIZE : IMPORT_STUB_SIZE;
        if (addr >= s->addr && addr < s->addr + size) return s;
    }
    return NULL;
}

static bool get_stub(linker *l, const char *library, uint64_t nid, provider_kind kind, bool is_data,
                     guest_addr *out) {
    uint64_t key = nid ^ hash_u64(fnv1a(library)) ^ (is_data ? 0xA5A5A5A5A5A5A5A5ull : 0);
    uint64_t idx;
    if (hmap64_get(&l->stub_index, key, &idx)) {
        const import_stub *s = &l->stubs.data[idx];
        if (s->nid == nid && s->is_data == is_data && strcmp(s->library, library) == 0) {
            *out = s->addr;
            return true;
        }
    }
    import_stub s;
    memset(&s, 0, sizeof(s));
    if (is_data) {
        if ((uint64_t)(l->data_used + 1) * IMPORT_DATA_SLOT_SIZE > IMPORT_DATA_AREA_SIZE) return false;
        s.addr = l->import_data + (uint64_t)l->data_used++ * IMPORT_DATA_SLOT_SIZE;
    } else {
        if (IMPORT_GLUE_RESERVE + (uint64_t)(l->code_used + 1) * IMPORT_STUB_SIZE > IMPORT_CODE_AREA_SIZE) return false;
        s.addr = l->import_code + IMPORT_GLUE_RESERVE + (uint64_t)l->code_used++ * IMPORT_STUB_SIZE;
    }
    s.library = library;
    s.nid = nid;
    s.kind = kind;
    s.is_data = is_data;
    if (!dyn_push(&l->stubs, s) || !hmap64_put(&l->stub_index, key, l->stubs.len - 1)) {
        return false;
    }
    if (log_enabled(LOG_CAT_LOADER, LOG_LVL_DEBUG)) {
        char n[NID_STR_LEN + 1];
        nid_to_str(nid, n);
        LOG_D(LOADER, "stub 0x%llx: %s::%s", (unsigned long long)s.addr, library, n);
    }
    *out = s.addr;
    return true;
}

bool linker_add_stub(linker *l, const char *library, uint64_t nid, provider_kind kind, bool is_data,
                     guest_addr *out) {
    return get_stub(l, library, nid, kind, is_data, out);
}

/* ---------------------------------------------------------------------------
 * Mapping
 * ------------------------------------------------------------------------- */

static ps5c_err map_module(linker *l, loaded_module *m) {
    const sce_image *img = &m->img;
    if (img->image_hi == 0) {
        LOG_E(LOADER, "%s: no loadable segments", m->name);
        return PS5C_ERR_INVALID;
    }
    guest_addr lo = PS5C_ALIGN_DOWN(img->image_lo, GUEST_PAGE_SIZE);
    guest_addr hi = PS5C_ALIGN_UP(img->image_hi, GUEST_PAGE_SIZE);
    uint64_t size = hi - lo;
    bool fixed = img->elf_type == ET_SCE_EXEC || img->elf_type == ET_EXEC;

    /* Next module goes after the highest one mapped so far, leaving a guard gap. */
    guest_addr hint = GUEST_MODULE_BASE;
    for (size_t i = 0; i < l->ms->modules.len; i++) {
        const loaded_module *o = l->ms->modules.data[i];
        if (o->mapped) {
            hint = PS5C_MAX(hint, PS5C_ALIGN_UP(o->base + o->map_size + GUEST_PAGE_SIZE, MODULE_ALIGN));
        }
    }

    guest_addr base;
    ps5c_err err = gmem_map(l->mem, fixed ? lo : hint, size, GPROT_RW, fixed ? GMAP_FIXED : 0, m->name, &base);
    if (err != PS5C_OK) {
        LOG_E(LOADER, "%s: cannot map %llu bytes: %s", m->name, (unsigned long long)size, ps5c_err_str(err));
        return err;
    }
    m->mapped = true;
    m->base = base;
    m->bias = base - lo;
    m->map_size = size;

    for (uint32_t i = 0; i < img->segment_count; i++) {
        const sce_segment *seg = &img->segments[i];
        if ((seg->h.p_type != PT_LOAD && seg->h.p_type != PT_SCE_RELRO) || !seg->has_data) {
            continue;
        }
        if (!gmem_write(l->mem, m->bias + seg->h.p_vaddr, img->file + seg->data_off, seg->h.p_filesz)) {
            LOG_E(LOADER, "%s: segment %u doesn't fit its mapping", m->name, i);
            return PS5C_ERR_INVALID;
        }
    }

    if (img->has_tls) {
        m->tls_index = l->next_tls_index++;
        uint64_t align = img->tls_align ? img->tls_align : 1;
        if (!PS5C_IS_POW2(align)) {
            LOG_E(LOADER, "%s: bad TLS alignment %llu", m->name, (unsigned long long)align);
            return PS5C_ERR_INVALID;
        }
        l->static_tls_end = PS5C_ALIGN_UP(l->static_tls_end + img->tls_memsz, align);
        m->tls_offset = l->static_tls_end;
    }
    l->stats.modules_mapped++;
    LOG_I(LOADER, "mapped '%s' at 0x%llx-0x%llx%s", m->name, (unsigned long long)base,
          (unsigned long long)(base + size), m->tls_index ? " (TLS)" : "");
    return PS5C_OK;
}

/* ---------------------------------------------------------------------------
 * Symbol binding
 * ------------------------------------------------------------------------- */

typedef struct binding {
    uint64_t value;              /* address, or raw TLS offset for TLS symbols */
    const loaded_module *owner;  /* module defining the symbol (NULL for stubs) */
} binding;

static bool bind_symbol(linker *l, const loaded_module *m, uint32_t sym_index, binding *b) {
    const sce_image *img = &m->img;
    const sce_sym *s = &img->syms[sym_index];
    bool tls = s->type == STT_TLS;
    b->owner = NULL;

    if (s->shndx != SHN_UNDEF) {
        b->value = tls ? s->value : m->bias + s->value;
        b->owner = m;
        l->stats.binds_local++;
        return true;
    }

    const char *lib = s->has_nid ? sce_image_sym_lib_name(img, s) : NULL;
    uint64_t nid = s->has_nid ? s->nid : nid_hash_name(s->name);
    if (!lib) lib = "?";

    import_resolution r = module_set_resolve(l->ms, lib, nid);
    if (r.kind == PROVIDER_LLE && r.module->mapped) {
        b->value = tls ? r.sym->value : r.module->bias + r.sym->value;
        b->owner = r.module;
        l->stats.binds_lle++;
        return true;
    }
    if (tls) {
        return false; /* TLS variables can't be stubbed */
    }
    bool is_data = s->type == STT_OBJECT;
    if (s->bind == STB_WEAK && is_data && r.kind != PROVIDER_HLE) {
        b->value = 0; /* ELF semantics for an absent weak symbol */
        LOG_D(LOADER, "%s: weak data import %s::%s bound to 0", m->name, lib, s->name ? s->name : "?");
        l->stats.weak_zero++;
        return true;
    }
    if (!get_stub(l, lib, nid, r.kind == PROVIDER_LLE ? PROVIDER_MISSING : r.kind, is_data, &b->value)) {
        LOG_E(LOADER, "import area exhausted");
        return false;
    }
    if (r.kind == PROVIDER_HLE) l->stats.binds_hle++;
    else l->stats.binds_missing++;
    return true;
}

/* ---------------------------------------------------------------------------
 * Relocation
 * ------------------------------------------------------------------------- */

static ps5c_err relocate_module(linker *l, loaded_module *m) {
    const sce_image *img = &m->img;
    uint32_t unsupported_logged = 0;

    for (uint32_t i = 0; i < img->reloc_count; i++) {
        const sce_reloc *r = &img->relocs[i];
        guest_addr where = m->bias + r->offset;
        if (where < m->base || where > m->base + m->map_size - 8) {
            LOG_E(LOADER, "%s: relocation %u targets 0x%llx outside the module", m->name, i,
                  (unsigned long long)r->offset);
            return PS5C_ERR_INVALID;
        }

        binding b = {0, m};
        bool needs_sym = r->type != R_X86_64_NONE && r->type != R_X86_64_RELATIVE;
        bool ok = true;
        if (needs_sym && r->sym != 0) {
            ok = bind_symbol(l, m, r->sym, &b);
        }

        uint64_t v = 0;
        bool write = ok;
        switch (r->type) {
        case R_X86_64_NONE: write = false; break;
        case R_X86_64_RELATIVE: v = m->bias + (uint64_t)r->addend; write = true; break;
        case R_X86_64_64: v = b.value + (uint64_t)r->addend; break;
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT: v = b.value; break;
        /* TLS relocations whose symbol names no module (symbol 0, or an untyped import that
         * resolves to nothing, as PS4 libc and libSceFios2 use): the module's own TLS block */
        case R_X86_64_DTPMOD64:
            v = b.owner ? b.owner->tls_index : m->tls_index;
            write = (ok || r->sym == 0) && v != 0;
            break;
        case R_X86_64_DTPOFF64: v = (b.owner ? b.value : 0) + (uint64_t)r->addend; write = ok || r->sym == 0; break;
        case R_X86_64_TPOFF64: {
            const loaded_module *owner = b.owner ? b.owner : m;
            write = (ok || r->sym == 0) && owner->tls_index != 0;
            if (write) v = (b.owner ? b.value : 0) + (uint64_t)r->addend - owner->tls_offset;
            break;
        }
        default: write = false; ok = false; break;
        }

        if (!write) {
            if (r->type != R_X86_64_NONE) {
                l->stats.relocs_unsupported++;
                if (unsupported_logged++ < 5) {
                    LOG_W(LOADER, "%s: relocation %u (type %s, symbol %u, %s, owner %s TLS %u) not applied", m->name, i,
                          sce_reloc_type_name(r->type), (unsigned)r->sym, ok ? "bound" : "unbound",
                          b.owner ? b.owner->name : "none", b.owner ? b.owner->tls_index : 0);
                }
            }
            continue;
        }
        if (!gmem_write_u64(l->mem, where, v)) {
            return PS5C_ERR_INVALID;
        }
        l->stats.relocs_applied++;
        l->stats.relocs_by_type[r->type < 32 ? r->type : 0]++;
    }
    return PS5C_OK;
}

/* Final page protections: union of the segment flags touching each page.
 * PT_GNU_RELRO ranges stay writable: games write there after start-up
 * (Minecraft initializes static objects in its RELRO segment while loading a
 * world), so the console's loader does not make them read-only. */
static ps5c_err protect_module(linker *l, loaded_module *m) {
    const sce_image *img = &m->img;
    uint64_t pages = m->map_size / GUEST_PAGE_SIZE;
    DYNARRAY(uint8_t) prot = {0};
    if (!dyn_reserve(&prot, (size_t)pages)) {
        return PS5C_ERR_NOMEM;
    }
    memset(prot.data, 0, (size_t)pages);
    prot.len = (size_t)pages;

    for (uint32_t i = 0; i < img->segment_count; i++) {
        const elf64_phdr *ph = &img->segments[i].h;
        if ((ph->p_type != PT_LOAD && ph->p_type != PT_SCE_RELRO) || ph->p_memsz == 0) continue;
        guest_addr s = PS5C_ALIGN_DOWN(m->bias + ph->p_vaddr, GUEST_PAGE_SIZE);
        guest_addr e = PS5C_ALIGN_UP(m->bias + ph->p_vaddr + ph->p_memsz, GUEST_PAGE_SIZE);
        for (guest_addr a = s; a < e && a < m->base + m->map_size; a += GUEST_PAGE_SIZE) {
            if (a < m->base) continue;
            uint8_t *p = &prot.data[(a - m->base) / GUEST_PAGE_SIZE];
            *p = (uint8_t)(*p | gprot_from_pflags(ph->p_flags));
        }
    }

    ps5c_err err = PS5C_OK;
    for (uint64_t i = 0; i < pages && err == PS5C_OK;) {
        uint64_t j = i;
        while (j < pages && prot.data[j] == prot.data[i]) j++;
        guest_addr addr = m->base + i * GUEST_PAGE_SIZE;
        uint64_t size = (j - i) * GUEST_PAGE_SIZE;
        err = m->is_main ? gmem_protect_title_image(l->mem, addr, size, prot.data[i])
                         : gmem_protect(l->mem, addr, size, prot.data[i]);
        i = j;
    }
    dyn_free(&prot);
    return err;
}

ps5c_err linker_link_all(linker *l) {
    module_set *ms = l->ms;
    ps5c_err err = PS5C_OK;

    /* The main executable must get TLS index 1, so it is mapped first. */
    for (int pass = 0; pass < 2 && err == PS5C_OK; pass++) {
        for (size_t i = 0; i < ms->modules.len && err == PS5C_OK; i++) {
            loaded_module *m = ms->modules.data[i];
            if (!m->mapped && m->is_main == (pass == 0)) {
                err = map_module(l, m);
            }
        }
    }
    for (size_t i = 0; i < ms->modules.len && err == PS5C_OK; i++) {
        err = relocate_module(l, ms->modules.data[i]);
        if (err == PS5C_OK && l->on_relocated) {
            l->on_relocated(l->hook_user, ms->modules.data[i]);
        }
    }
    for (size_t i = 0; i < ms->modules.len && err == PS5C_OK; i++) {
        err = protect_module(l, ms->modules.data[i]);
    }
    l->stats.static_tls_size = l->static_tls_end;
    if (err == PS5C_OK) {
        LOG_I(LOADER, "linked %u modules: %llu relocations applied, %llu not applied; imports: %u LLE, %u HLE, %u missing",
              l->stats.modules_mapped, (unsigned long long)l->stats.relocs_applied,
              (unsigned long long)l->stats.relocs_unsupported, l->stats.binds_lle, l->stats.binds_hle,
              l->stats.binds_missing);
    }
    return err;
}
