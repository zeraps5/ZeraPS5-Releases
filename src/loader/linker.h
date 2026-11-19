/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Linker: maps the modules of a module_set into guest memory, applies their
 * relocations and binds every import to its provider.
 *
 *   LLE import      -> address of the symbol inside the providing module
 *   HLE / missing   -> a unique stub slot in the import area. The CPU backend
 *                      turns each slot into a thunk (F3); until then slots
 *                      hold int3 so a stray call traps.
 *   data import     -> a zeroed per-import slot in the import data area
 *                      (unless LLE provides it)
 *
 * Static TLS follows the FreeBSD x86-64 layout (variant II): each module's
 * block sits below the thread pointer, main executable first. */
#ifndef PS5C_LINKER_H
#define PS5C_LINKER_H

#include "base/hashmap.h"
#include "loader/module_set.h"
#include "mem/gmem.h"

#define IMPORT_STUB_SIZE 32u
#define IMPORT_GLUE_RESERVE 0x1000u /* start of the stub area, for the CPU backend's glue code */
#define IMPORT_CODE_AREA_SIZE 0x100000ull /* 32768 stubs */
#define IMPORT_DATA_SLOT_SIZE 256u
#define IMPORT_DATA_AREA_SIZE 0x100000ull

typedef struct import_stub {
    guest_addr addr;
    const char *library; /* owned by the module image */
    uint64_t nid;
    provider_kind kind; /* HLE or MISSING */
    bool is_data;
} import_stub;

typedef struct link_stats {
    uint32_t modules_mapped;
    uint64_t relocs_applied;
    uint64_t relocs_by_type[32];
    uint64_t relocs_unsupported;
    uint32_t binds_local;
    uint32_t binds_lle;
    uint32_t binds_hle;
    uint32_t binds_missing;
    uint32_t weak_zero; /* weak undefined symbols bound to 0 */
    uint64_t static_tls_size;
} link_stats;

typedef struct linker {
    gmem *mem;
    module_set *ms;
    guest_addr import_code; /* stub area */
    guest_addr import_data; /* data slot area */
    uint32_t code_used, data_used;
    DYNARRAY(import_stub) stubs;
    hmap64 stub_index; /* key(library, nid, is_data) -> stubs index */
    uint32_t next_tls_index;
    uint64_t static_tls_end; /* running size of the static TLS area */
    link_stats stats;
    /* Called after a module is relocated and before its final protections
     * are applied (its pages are still writable): e.g. CPU code patching. */
    void (*on_relocated)(void *user, loaded_module *m);
    void *hook_user;
} linker;

ps5c_err linker_init(linker *l, gmem *mem, module_set *ms);
void linker_free(linker *l);

/* Maps, relocates and protects every module in the set not mapped yet. */
ps5c_err linker_link_all(linker *l);

/* Creates (or returns) a stub for (library, nid); used for internal entry points. */
bool linker_add_stub(linker *l, const char *library, uint64_t nid, provider_kind kind, bool is_data,
                     guest_addr *out);

/* Looks up the stub that owns a guest address (for logs and traps). */
const import_stub *linker_find_stub(const linker *l, guest_addr addr);

#endif
