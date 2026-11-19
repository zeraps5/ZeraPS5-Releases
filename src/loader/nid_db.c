/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "loader/nid_db.h"

#include <string.h>

#include "loader/nid.h"

void nid_db_init(nid_db *db) {
    hmap64_init(&db->map);
    arena_init(&db->strings, 256 * 1024);
    db->count = 0;
}

void nid_db_free(nid_db *db) {
    hmap64_free(&db->map);
    arena_free(&db->strings);
    db->count = 0;
}

static bool add_n(nid_db *db, const char *name, size_t n) {
    char *copy = (char *)arena_alloc(&db->strings, n + 1, 1);
    if (!copy) {
        return false;
    }
    memcpy(copy, name, n);
    copy[n] = '\0';
    uint64_t nid = nid_hash_name(copy);
    if (!hmap64_get(&db->map, nid, NULL)) {
        if (!hmap64_put(&db->map, nid, (uint64_t)(uintptr_t)copy)) {
            return false;
        }
        db->count++;
    }
    return true;
}

bool nid_db_add(nid_db *db, const char *name) { return add_n(db, name, strlen(name)); }

ps5c_err nid_db_load_text(nid_db *db, const char *text, size_t len) {
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && text[i] != '\n') {
            i++;
        }
        size_t end = i;
        i++; /* skip '\n' */
        while (end > start && (text[end - 1] == '\r' || text[end - 1] == ' ' || text[end - 1] == '\t')) {
            end--;
        }
        while (start < end && (text[start] == ' ' || text[start] == '\t')) {
            start++;
        }
        if (start == end || text[start] == '#') {
            continue;
        }
        if (!add_n(db, text + start, end - start)) {
            return PS5C_ERR_NOMEM;
        }
    }
    return PS5C_OK;
}

const char *nid_db_lookup(const nid_db *db, uint64_t nid) {
    uint64_t v;
    return hmap64_get(&db->map, nid, &v) ? (const char *)(uintptr_t)v : NULL;
}
