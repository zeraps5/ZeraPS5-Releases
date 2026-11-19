/* SPDX-License-Identifier: GPL-2.0-or-later */
/* NID -> symbol name database, used only for readable logs and profiles.
 * The text format is one symbol name per line ('#' starts a comment); NIDs
 * are computed from the names, so a wrong entry can never mislabel a NID. */
#ifndef PS5C_NID_DB_H
#define PS5C_NID_DB_H

#include "base/arena.h"
#include "base/base.h"
#include "base/hashmap.h"

typedef struct nid_db {
    hmap64 map; /* nid -> pointer to name (stored in `strings`) */
    arena strings;
    uint32_t count;
} nid_db;

void nid_db_init(nid_db *db);
void nid_db_free(nid_db *db);
bool nid_db_add(nid_db *db, const char *name);
ps5c_err nid_db_load_text(nid_db *db, const char *text, size_t len);
const char *nid_db_lookup(const nid_db *db, uint64_t nid);

#endif
