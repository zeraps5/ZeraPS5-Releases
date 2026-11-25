/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The set of modules that make up a running program: the main executable
 * plus the libraries it needs, either shipped with the game (sce_module/) or
 * dumped from the console firmware. Decides, per library, who provides each
 * imported symbol:
 *
 *   HLE  - our C reimplementation (hle/)
 *   LLE  - the real Sony library code, from a loaded .prx/.sprx
 *
 * Policy is per library, not per function, because a library's functions
 * usually share internal state (HYBRID is the reviewed exception).
 * libkernel is always HLE. */
#ifndef PS5C_MODULE_SET_H
#define PS5C_MODULE_SET_H

#include "base/base.h"
#include "base/dynarray.h"
#include "base/hashmap.h"
#include "loader/sce_image.h"
#include "pal/pal.h"

/* AUTO: LLE when a module exports the library, else HLE.
 * HYBRID: HLE for the functions we implement, the real module for the rest.
 *   Only for libraries whose remaining functions keep no state of their own
 *   (libSceAgc: register tables and packet builders writing to caller memory). */
typedef enum { LIB_POLICY_AUTO, LIB_POLICY_HLE, LIB_POLICY_LLE, LIB_POLICY_HYBRID } lib_policy;
typedef enum { PROVIDER_MISSING, PROVIDER_HLE, PROVIDER_LLE } provider_kind;

/* Answers "is this function implemented in HLE?". NULL means nothing is. */
typedef bool (*hle_query_fn)(void *user, const char *library, uint64_t nid);

typedef struct loaded_module {
    char *path;       /* file path or caller-supplied name */
    const char *name; /* module name (DT_SCE_MODULE_INFO) or file name */
    uint8_t *owned_data;
    pal_file_view file_view; /* alternative owner of img.file; opt-in native files */
    bool is_main;
    /* Loaded with the process but started by the game (sceKernelLoadStartModule):
     * DT_INIT and module_start run then, not at boot. */
    bool deferred_start;
    bool started;
    sce_image img;

    /* Filled in when the linker maps the module into guest memory. */
    bool mapped;
    uint64_t base;      /* guest address of the lowest mapped page */
    uint64_t bias;      /* guest address = bias + ELF vaddr */
    uint64_t map_size;
    uint32_t tls_index; /* 0 = no TLS; the main executable is 1 */
    uint64_t tls_offset; /* static TLS: block ends at thread pointer - tls_offset */
} loaded_module;

typedef struct lib_policy_entry {
    char *library;
    lib_policy policy;
} lib_policy_entry;

typedef struct module_set {
    DYNARRAY(loaded_module *) modules;
    DYNARRAY(lib_policy_entry) policies;
    hmap64 exports; /* key(library, nid) -> (module index << 32) | symbol index */
    hle_query_fn hle_query;
    void *hle_user;
} module_set;

typedef struct import_resolution {
    provider_kind kind;
    lib_policy policy;           /* effective policy for the library */
    const loaded_module *module; /* LLE provider */
    const sce_sym *sym;          /* LLE symbol */
    bool lle_available;          /* some loaded module exports this library */
} import_resolution;

/* Guest address of `library`'s export `nid` in a loaded module, 0 if none */
uint64_t module_set_lle_address(const module_set *ms, const char *library, uint64_t nid);

void module_set_init(module_set *ms);
void module_set_free(module_set *ms);

ps5c_err module_set_add_file(module_set *ms, const char *path, bool is_main, loaded_module **out);
/* `data` is borrowed and must outlive the set. */
ps5c_err module_set_add_buffer(module_set *ms, const char *name, const uint8_t *data, size_t size,
                               bool is_main, loaded_module **out);

/* For every needed module not loaded yet, tries <dir>/<name>.sprx and
 * <dir>/<name>.prx in each directory. Repeats until no new module loads
 * (libraries have dependencies too). Returns the number of modules added. */
uint32_t module_set_load_needed(module_set *ms, const char *const *dirs, uint32_t dir_count);

const loaded_module *module_set_main(const module_set *ms);
const loaded_module *module_set_find_by_name(const module_set *ms, const char *module_name);

ps5c_err module_set_set_policy(module_set *ms, const char *library, lib_policy policy);
lib_policy module_set_get_policy(const module_set *ms, const char *library);
bool module_set_library_is_lle(const module_set *ms, const char *library);

import_resolution module_set_resolve(const module_set *ms, const char *library, uint64_t nid);

const char *lib_policy_name(lib_policy p);
const char *provider_kind_name(provider_kind k);

#endif
