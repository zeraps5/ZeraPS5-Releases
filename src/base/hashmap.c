/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "base/hashmap.h"

#include <stdlib.h>

enum { SLOT_EMPTY = 0, SLOT_USED = 1, SLOT_DELETED = 2 };

uint64_t hash_u64(uint64_t x) {
    /* splitmix64 finalizer */
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x;
}

void hmap64_init(hmap64 *m) {
    m->keys = NULL;
    m->vals = NULL;
    m->state = NULL;
    m->cap = 0;
    m->len = 0;
    m->tombstones = 0;
}

void hmap64_free(hmap64 *m) {
    free(m->keys);
    free(m->vals);
    free(m->state);
    hmap64_init(m);
}

/* Returns the slot holding `key`, or SIZE_MAX. */
static size_t find_slot(const hmap64 *m, uint64_t key) {
    if (m->cap == 0) {
        return SIZE_MAX;
    }
    size_t mask = m->cap - 1;
    for (size_t i = (size_t)hash_u64(key) & mask, n = 0; n < m->cap; i = (i + 1) & mask, n++) {
        if (m->state[i] == SLOT_EMPTY) {
            return SIZE_MAX;
        }
        if (m->state[i] == SLOT_USED && m->keys[i] == key) {
            return i;
        }
    }
    return SIZE_MAX;
}

static bool rehash(hmap64 *m, size_t new_cap) {
    uint64_t *keys = (uint64_t *)malloc(new_cap * sizeof(uint64_t));
    uint64_t *vals = (uint64_t *)malloc(new_cap * sizeof(uint64_t));
    uint8_t *state = (uint8_t *)calloc(new_cap, 1);
    if (!keys || !vals || !state) {
        free(keys);
        free(vals);
        free(state);
        return false;
    }
    size_t mask = new_cap - 1;
    for (size_t i = 0; i < m->cap; i++) {
        if (m->state[i] != SLOT_USED) {
            continue;
        }
        size_t j = (size_t)hash_u64(m->keys[i]) & mask;
        while (state[j] == SLOT_USED) {
            j = (j + 1) & mask;
        }
        state[j] = SLOT_USED;
        keys[j] = m->keys[i];
        vals[j] = m->vals[i];
    }
    free(m->keys);
    free(m->vals);
    free(m->state);
    m->keys = keys;
    m->vals = vals;
    m->state = state;
    m->cap = new_cap;
    m->tombstones = 0;
    return true;
}

bool hmap64_put(hmap64 *m, uint64_t key, uint64_t val) {
    size_t existing = find_slot(m, key);
    if (existing != SIZE_MAX) {
        m->vals[existing] = val;
        return true;
    }

    /* Keep load (including tombstones) at or below 75%. */
    if ((m->len + m->tombstones + 1) * 4 > m->cap * 3) {
        size_t new_cap = m->cap ? m->cap : 16;
        while ((m->len + 1) * 4 > new_cap * 3 / 2) {
            new_cap *= 2;
        }
        if (!rehash(m, new_cap)) {
            return false;
        }
    }

    size_t mask = m->cap - 1;
    size_t i = (size_t)hash_u64(key) & mask;
    while (m->state[i] == SLOT_USED) {
        i = (i + 1) & mask;
    }
    if (m->state[i] == SLOT_DELETED) {
        m->tombstones--;
    }
    m->state[i] = SLOT_USED;
    m->keys[i] = key;
    m->vals[i] = val;
    m->len++;
    return true;
}

bool hmap64_get(const hmap64 *m, uint64_t key, uint64_t *val_out) {
    size_t i = find_slot(m, key);
    if (i == SIZE_MAX) {
        return false;
    }
    if (val_out) {
        *val_out = m->vals[i];
    }
    return true;
}

bool hmap64_remove(hmap64 *m, uint64_t key) {
    size_t i = find_slot(m, key);
    if (i == SIZE_MAX) {
        return false;
    }
    m->state[i] = SLOT_DELETED;
    m->len--;
    m->tombstones++;
    return true;
}
