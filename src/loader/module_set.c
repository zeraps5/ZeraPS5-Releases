/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "loader/module_set.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "pal/pal.h"

static char *str_dup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

static uint64_t fnv1a(const char *s) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (; *s; s++) {
        h = (h ^ (uint8_t)*s) * 0x100000001B3ull;
    }
    return h;
}

static uint64_t export_key(const char *library, uint64_t nid) { return nid ^ hash_u64(fnv1a(library)); }

static const char *base_name(const char *path) {
    const char *s = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') s = p + 1;
    }
    return s;
}

void module_set_init(module_set *ms) {
    memset(ms, 0, sizeof(*ms));
    hmap64_init(&ms->exports);
}

static void free_module(loaded_module *m) {
    sce_image_free(&m->img);
    free(m->owned_data);
    pal_file_view_close(&m->file_view);
    free(m->path);
    free(m);
}

void module_set_free(module_set *ms) {
    for (size_t i = 0; i < ms->modules.len; i++) {
        loaded_module *m = ms->modules.data[i];
        free_module(m);
    }
    dyn_free(&ms->modules);
    for (size_t i = 0; i < ms->policies.len; i++) {
        free(ms->policies.data[i].library);
    }
    dyn_free(&ms->policies);
    hmap64_free(&ms->exports);
}

static ps5c_err register_exports(module_set *ms, uint32_t module_index) {
    const loaded_module *m = ms->modules.data[module_index];
    for (uint32_t i = 0; i < m->img.sym_count; i++) {
        const sce_sym *s = &m->img.syms[i];
        if (s->shndx == SHN_UNDEF || !s->has_nid || (s->bind != STB_GLOBAL && s->bind != STB_WEAK)) {
            continue;
        }
        const char *lib = sce_image_sym_lib_name(&m->img, s);
        if (!lib) {
            continue;
        }
        uint64_t key = export_key(lib, s->nid);
        if (hmap64_get(&ms->exports, key, NULL)) {
            continue; /* first provider wins */
        }
        if (!hmap64_put(&ms->exports, key, ((uint64_t)module_index << 32) | i)) {
            return PS5C_ERR_NOMEM;
        }
    }
    return PS5C_OK;
}

static ps5c_err add_module(module_set *ms, const char *path, uint8_t *owned,
                           pal_file_view view, const uint8_t *data,
                           size_t size, bool is_main, loaded_module **out) {
    if (is_main && module_set_main(ms)) {
        free(owned);
        pal_file_view_close(&view);
        return PS5C_ERR_INVALID;
    }
    loaded_module *m = (loaded_module *)calloc(1, sizeof(*m));
    if (!m) {
        free(owned);
        pal_file_view_close(&view);
        return PS5C_ERR_NOMEM;
    }
    m->path = str_dup(path);
    m->owned_data = owned;
    m->file_view = view; /* ownership transfers even if parsing fails */
    m->is_main = is_main;
    if (!m->path) {
        free_module(m);
        return PS5C_ERR_NOMEM;
    }

    ps5c_err err = sce_image_parse(&m->img, data, size);
    if (err != PS5C_OK) {
        LOG_E(LOADER, "%s: %s", path, m->img.error[0] ? m->img.error : ps5c_err_str(err));
        free_module(m);
        return err;
    }
    m->name = m->img.has_module_info ? m->img.module_info.name : base_name(m->path);

    if (!dyn_push(&ms->modules, m)) {
        free_module(m);
        return PS5C_ERR_NOMEM;
    }
    err = register_exports(ms, (uint32_t)(ms->modules.len - 1));
    if (err != PS5C_OK) {
        return err;
    }
    LOG_I(LOADER, "loaded %s '%s' (%s, %u import libs, %u export libs)",
          is_main ? "executable" : "module", m->name, sce_elf_type_name(m->img.elf_type),
          m->img.import_lib_count, m->img.export_lib_count);
    if (out) {
        *out = m;
    }
    return PS5C_OK;
}

ps5c_err module_set_add_file(module_set *ms, const char *path, bool is_main, loaded_module **out) {
    const char *enabled = getenv("PS5C_MODULE_FILE_VIEWS");
    if (enabled && strcmp(enabled, "1") == 0) {
        pal_file_view view = {0};
        ps5c_err view_err = pal_file_view_open(path, &view);
        if (view_err == PS5C_OK) {
            LOG_I(LOADER, "read-only module view: %s (%llu bytes)", path,
                  (unsigned long long)view.size);
            return add_module(ms, path, NULL, view, view.data, (size_t)view.size, is_main, out);
        }
        /* Includes virtual exfat files: their existing reader remains valid. */
        LOG_D(LOADER, "module view unavailable for %s: %s; reading normally",
              path, ps5c_err_str(view_err));
    }
    uint8_t *data = NULL;
    uint64_t size = 0;
    ps5c_err err = pal_file_read_all(path, &data, &size);
    if (err != PS5C_OK) {
        LOG_E(LOADER, "cannot read '%s': %s", path, ps5c_err_str(err));
        return err;
    }
    if (size > SIZE_MAX) { free(data); return PS5C_ERR_OUT_OF_RANGE; }
    return add_module(ms, path, data, (pal_file_view){0}, data, (size_t)size, is_main, out);
}

ps5c_err module_set_add_buffer(module_set *ms, const char *name, const uint8_t *data, size_t size,
                               bool is_main, loaded_module **out) {
    return add_module(ms, name, NULL, (pal_file_view){0}, data, size, is_main, out);
}

const loaded_module *module_set_main(const module_set *ms) {
    for (size_t i = 0; i < ms->modules.len; i++) {
        if (ms->modules.data[i]->is_main) return ms->modules.data[i];
    }
    return NULL;
}

const loaded_module *module_set_find_by_name(const module_set *ms, const char *module_name) {
    for (size_t i = 0; i < ms->modules.len; i++) {
        if (strcmp(ms->modules.data[i]->name, module_name) == 0) return ms->modules.data[i];
    }
    return NULL;
}

static bool is_forced_hle(const char *library) {
    /* The kernel talks to hardware and the OS; it is always reimplemented.
     * libScePosix is exported by the libkernel module too. */
    return strncmp(library, "libkernel", 9) == 0 || strcmp(library, "libScePosix") == 0;
}

/* Libraries HLE'd by default even when their module is available (override
 * with an explicit LLE policy). The graphics stack: libSceAgcDriver talks to
 * the GPU through undocumented kernel ioctls, and libSceAgc calls private
 * AgcDriver entry points, so the public Agc + AgcDriver API is the boundary
 * (the same one KytyPS5 uses). */
/* Some homebrew packages require a loader companion called libc.prx even
 * though their code imports the firmware's private libc. Load the companion,
 * but do not use its exports as private-libc implementations: the generated
 * BlackBearReloaded companion deliberately exports compatibility stubs. */
static const char *companion_name(const char *library) {
    return strcmp(library, "libSceLibcInternal") == 0 ? "libc" : NULL;
}

static bool load_named(module_set *ms,const char *name,const char *const *dirs,uint32_t count) {
    static const char *const extensions[]={".sprx",".prx"};
    for(uint32_t d=0;d<count;d++)for(size_t e=0;e<PS5C_ARRAY_COUNT(extensions);e++) {
        char path[1024];int length=snprintf(path,sizeof(path),"%s/%s%s",dirs[d],name,extensions[e]);
        if(length<0 || (size_t)length>=sizeof(path))continue;
        bool present=false;
        for(size_t i=0;i<ms->modules.len;i++)if(ms->modules.data[i]->path && !strcmp(ms->modules.data[i]->path,path)){present=true;break;}
        if(present)continue;
        pal_file *file;
        if(pal_file_open(path,PAL_FILE_READ,&file)!=PS5C_OK)continue;
        pal_file_close(file);
        if(module_set_add_file(ms,path,false,NULL)==PS5C_OK)return true;
    }
    return false;
}

/* A game's own libc.prx, asked for by name by one of its modules (the retail
 * case), serves the firmware's private libc: same heap and FILE objects as the
 * rest of the game. Only a libc loaded as a homebrew companion (above) does not:
 * its exports there are placeholders. */
static bool libc_named(const module_set *ms) {
    for (size_t i = 0; i < ms->modules.len; i++) {
        const sce_image *img = &ms->modules.data[i]->img;
        for (uint32_t n = 0; n < img->needed_module_count; n++)
            if (img->needed_modules[n].name && !strcmp(img->needed_modules[n].name, "libc")) return true;
    }
    return false;
}

static lib_policy default_policy(const char *library) {
    if (strcmp(library, "libSceAgcDriver") == 0) return LIB_POLICY_HLE;
    /* Sony's libSceAgc builds packets and register tables for us; we override
     * what must talk to our GPU. */
    if (strcmp(library, "libSceAgc") == 0) return LIB_POLICY_HYBRID;
    /* Ours (lib_ampr.c) rather than the third-party stand-in dumps for older
     * firmware ship in fakelib/ (PS5C_AMPR_LLE=1 keeps that one) */
    if (strcmp(library, "libSceAmpr") == 0 && !getenv("PS5C_AMPR_LLE")) return LIB_POLICY_HLE;
    return LIB_POLICY_AUTO;
}

uint32_t module_set_load_needed(module_set *ms, const char *const *dirs, uint32_t dir_count) {
    uint32_t added = 0;
    bool progress = true;
    while (progress) {
        progress = false;
        for (size_t i = 0; i < ms->modules.len; i++) {
            const sce_image *img = &ms->modules.data[i]->img;
            for (uint32_t n = 0; n < img->needed_module_count; n++) {
                const char *name = img->needed_modules[n].name;
                const char *companion = companion_name(name);
                if (module_set_get_policy(ms, name) == LIB_POLICY_HLE || module_set_find_by_name(ms, name)) {
                    continue;
                }
                /* Search every directory for the actual firmware library
                 * before considering a differently named companion. */
                bool loaded=load_named(ms,name,dirs,dir_count);
                if(!loaded && companion && !module_set_find_by_name(ms,companion))
                    loaded=load_named(ms,companion,dirs,dir_count);
                if(loaded) {
                    added++;
                    progress=true;
                }
                /* `ms->modules` may have grown: reload the image pointer. */
                img = &ms->modules.data[i]->img;
            }
        }
    }
    return added;
}

ps5c_err module_set_set_policy(module_set *ms, const char *library, lib_policy policy) {
    for (size_t i = 0; i < ms->policies.len; i++) {
        if (strcmp(ms->policies.data[i].library, library) == 0) {
            ms->policies.data[i].policy = policy;
            return PS5C_OK;
        }
    }
    lib_policy_entry e;
    e.library = str_dup(library);
    e.policy = policy;
    if (!e.library || !dyn_push(&ms->policies, e)) {
        free(e.library);
        return PS5C_ERR_NOMEM;
    }
    return PS5C_OK;
}

lib_policy module_set_get_policy(const module_set *ms, const char *library) {
    if (is_forced_hle(library)) {
        return LIB_POLICY_HLE;
    }
    for (size_t i = 0; i < ms->policies.len; i++) {
        if (strcmp(ms->policies.data[i].library, library) == 0) return ms->policies.data[i].policy;
    }
    return default_policy(library);
}

bool module_set_library_is_lle(const module_set *ms, const char *library) {
    for (size_t i = 0; i < ms->modules.len; i++) {
        const loaded_module *m = ms->modules.data[i];
        if (m->is_main) {
            continue;
        }
        for (uint32_t l = 0; l < m->img.export_lib_count; l++) {
            if (strcmp(m->img.export_libs[l].name, library) == 0) return true;
        }
    }
    return false;
}

static bool find_export(const module_set *ms, const char *library, uint64_t nid,
                        import_resolution *r) {
    uint64_t v;
    if (!hmap64_get(&ms->exports, export_key(library, nid), &v)) {
        return false;
    }
    uint32_t mi = (uint32_t)(v >> 32), si = (uint32_t)v;
    const loaded_module *m = ms->modules.data[mi];
    const sce_sym *s = &m->img.syms[si];
    const char *lib = sce_image_sym_lib_name(&m->img, s);
    if (s->nid != nid || !lib || strcmp(lib, library) != 0) {
        return false; /* 64-bit key collision */
    }
    r->module = m;
    r->sym = s;
    return true;
}

import_resolution module_set_resolve(const module_set *ms, const char *library, uint64_t nid) {
    import_resolution r;
    memset(&r, 0, sizeof(r));
    r.kind = PROVIDER_MISSING;
    r.policy = module_set_get_policy(ms, library);
    r.lle_available = module_set_library_is_lle(ms, library);

    const char *alias = companion_name(library);
    if (r.policy == LIB_POLICY_AUTO && alias && !r.lle_available && libc_named(ms)) {
        /* The game's libc first, HLE for what it doesn't export. */
        if (find_export(ms, alias, nid, &r)) r.kind = PROVIDER_LLE;
        else if (ms->hle_query && ms->hle_query(ms->hle_user, library, nid)) r.kind = PROVIDER_HLE;
        return r;
    }
    if (r.policy == LIB_POLICY_HYBRID) {
        if (ms->hle_query && ms->hle_query(ms->hle_user, library, nid)) r.kind = PROVIDER_HLE;
        else if (find_export(ms, library, nid, &r)) r.kind = PROVIDER_LLE;
        return r;
    }
    bool use_lle = r.policy == LIB_POLICY_LLE || (r.policy == LIB_POLICY_AUTO && r.lle_available);
    if (use_lle) {
        if (find_export(ms, library, nid, &r)) {
            r.kind = PROVIDER_LLE;
        }
        return r;
    }
    if (ms->hle_query && ms->hle_query(ms->hle_user, library, nid)) {
        r.kind = PROVIDER_HLE;
    }
    return r;
}

const char *lib_policy_name(lib_policy p) {
    switch (p) {
    case LIB_POLICY_AUTO: return "auto";
    case LIB_POLICY_HLE: return "hle";
    case LIB_POLICY_LLE: return "lle";
    case LIB_POLICY_HYBRID: return "hybrid";
    default: return "?";
    }
}

const char *provider_kind_name(provider_kind k) {
    switch (k) {
    case PROVIDER_MISSING: return "missing";
    case PROVIDER_HLE: return "hle";
    case PROVIDER_LLE: return "lle";
    default: return "?";
    }
}
