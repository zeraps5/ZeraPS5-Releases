/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Cached environment lookups (see base.h). */
#include "base/base.h"

#include <stdatomic.h>
#include <string.h>

#undef getenv

#define ENV_SLOTS 1024 /* power of two; there are a few hundred PS5C_* switches */

typedef struct env_slot {
    _Atomic(const char *) name; /* published last: a reader that sees it sees the value */
    char *value;                /* NULL = not set */
} env_slot;

static env_slot g_slots[ENV_SLOTS];
static atomic_flag g_lock = ATOMIC_FLAG_INIT;

static uint32_t name_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static char *copy(const char *s) {
    size_t n = strlen(s) + 1;
    char *c = (char *)malloc(n);
    if (c) memcpy(c, s, n);
    return c;
}

char *ps5c_getenv(const char *name) {
    uint32_t h = name_hash(name);
    for (uint32_t i = 0; i < ENV_SLOTS; i++) {
        env_slot *s = &g_slots[(h + i) & (ENV_SLOTS - 1)];
        const char *n = atomic_load_explicit(&s->name, memory_order_acquire);
        if (!n) break;
        if (!strcmp(n, name)) return s->value;
    }
    /* first lookup of this name */
    while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) {
    }
    char *value = getenv(name); /* answer for a full table */
    for (uint32_t i = 0; i < ENV_SLOTS; i++) {
        env_slot *s = &g_slots[(h + i) & (ENV_SLOTS - 1)];
        const char *n = atomic_load_explicit(&s->name, memory_order_relaxed);
        if (n && !strcmp(n, name)) {
            value = s->value;
            break;
        }
        if (n) continue;
        const char *real = value;
        char *key = copy(name);
        value = real ? copy(real) : NULL;
        if (key && (!real || value)) {
            s->value = value;
            atomic_store_explicit(&s->name, key, memory_order_release);
        } else { /* out of memory: answer uncached */
            free(key);
            free(value);
            value = real ? (char *)real : NULL;
        }
        break;
    }
    atomic_flag_clear_explicit(&g_lock, memory_order_release);
    return value;
}

void ps5c_env_reset(void) {
    while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) {
    }
    for (uint32_t i = 0; i < ENV_SLOTS; i++) {
        env_slot *s = &g_slots[i];
        char *n = (char *)atomic_load_explicit(&s->name, memory_order_relaxed);
        atomic_store_explicit(&s->name, NULL, memory_order_relaxed);
        free(n);
        free(s->value);
        s->value = NULL;
    }
    atomic_flag_clear_explicit(&g_lock, memory_order_release);
}
