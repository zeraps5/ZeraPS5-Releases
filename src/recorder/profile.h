/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Game requirement recorder: writes game_profile.json. The static part
 * (this file) needs no execution: it lists modules, libraries and every
 * imported symbol with who provides it (HLE, LLE or nobody yet). */
#ifndef PS5C_PROFILE_H
#define PS5C_PROFILE_H

#include "base/json.h"
#include "loader/module_set.h"
#include "loader/nid_db.h"

#define PROFILE_SCHEMA_VERSION 1

typedef struct profile_summary {
    uint32_t modules_loaded;
    uint32_t modules_missing; /* needed but not loaded and not forced HLE */
    uint32_t imports_unique;
    uint32_t imports_named; /* resolved to a name through the NID database */
    uint32_t by_provider[3]; /* indexed by provider_kind */
} profile_summary;

/* `db` may be NULL. `summary_out` may be NULL. */
ps5c_err profile_write_static(json_writer *w, const module_set *ms, const nid_db *db,
                              profile_summary *summary_out);

#endif
