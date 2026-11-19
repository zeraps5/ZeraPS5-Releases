/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Open-addressing hash map from uint64_t to uint64_t. Keys are arbitrary
 * 64-bit values (NIDs, guest addresses, content hashes); values are usually an
 * index into a separate table. */
#ifndef PS5C_HASHMAP_H
#define PS5C_HASHMAP_H

#include "base/base.h"

typedef struct hmap64 {
    uint64_t *keys;
    uint64_t *vals;
    uint8_t *state; /* 0 = empty, 1 = used, 2 = deleted */
    size_t cap;     /* power of two, or 0 before the first insert */
    size_t len;
    size_t tombstones;
} hmap64;

void hmap64_init(hmap64 *m);
void hmap64_free(hmap64 *m);

/* Inserts or replaces. Returns false only on allocation failure. */
bool hmap64_put(hmap64 *m, uint64_t key, uint64_t val);
bool hmap64_get(const hmap64 *m, uint64_t key, uint64_t *val_out);
bool hmap64_remove(hmap64 *m, uint64_t key);

uint64_t hash_u64(uint64_t x);

#endif
