/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "base/arena.h"

#include <stdlib.h>
#include <string.h>

struct arena_chunk {
    arena_chunk *prev;
    size_t cap;  /* bytes available after the header */
    size_t used; /* bytes consumed after the header */
};

#define CHUNK_HEADER PS5C_ALIGN_UP(sizeof(arena_chunk), 16)

static uint8_t *chunk_data(arena_chunk *c) { return (uint8_t *)c + CHUNK_HEADER; }

void arena_init(arena *a, size_t chunk_size) {
    a->head = NULL;
    a->chunk_size = chunk_size ? chunk_size : ARENA_DEFAULT_CHUNK;
    a->bytes_used = 0;
}

void arena_free(arena *a) {
    arena_chunk *c = a->head;
    while (c) {
        arena_chunk *prev = c->prev;
        free(c);
        c = prev;
    }
    a->head = NULL;
    a->bytes_used = 0;
}

/* Offset (from chunk data start) where an allocation would begin, or SIZE_MAX if it doesn't fit. */
static size_t fit_offset(arena_chunk *c, size_t size, size_t align) {
    uintptr_t base = (uintptr_t)chunk_data(c);
    uintptr_t cur = base + c->used;
    uintptr_t aligned = (cur + (align - 1)) & ~(uintptr_t)(align - 1);
    size_t off = (size_t)(aligned - base);
    if (off > c->cap || size > c->cap - off) {
        return SIZE_MAX;
    }
    return off;
}

void *arena_alloc(arena *a, size_t size, size_t align) {
    if (!PS5C_IS_POW2(align)) {
        return NULL;
    }
    if (size == 0) {
        size = 1;
    }

    size_t off = a->head ? fit_offset(a->head, size, align) : SIZE_MAX;
    if (off == SIZE_MAX) {
        if (size > SIZE_MAX / 2 || align > SIZE_MAX / 4) {
            return NULL;
        }
        size_t cap = PS5C_MAX(a->chunk_size, size + align);
        arena_chunk *c = (arena_chunk *)malloc(CHUNK_HEADER + cap);
        if (!c) {
            return NULL;
        }
        c->prev = a->head;
        c->cap = cap;
        c->used = 0;
        a->head = c;
        off = fit_offset(c, size, align);
    }

    arena_chunk *c = a->head;
    a->bytes_used += off + size - c->used;
    c->used = off + size;
    return chunk_data(c) + off;
}

void *arena_zalloc(arena *a, size_t size, size_t align) {
    void *p = arena_alloc(a, size, align);
    if (p) {
        memset(p, 0, size ? size : 1);
    }
    return p;
}

char *arena_strdup(arena *a, const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)arena_alloc(a, n, 1);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

arena_mark arena_get_mark(const arena *a) {
    arena_mark m;
    m.chunk = a->head;
    m.used = a->head ? a->head->used : 0;
    m.bytes_used = a->bytes_used;
    return m;
}

void arena_reset_to(arena *a, arena_mark mark) {
    while (a->head && a->head != mark.chunk) {
        arena_chunk *prev = a->head->prev;
        free(a->head);
        a->head = prev;
    }
    if (a->head) {
        a->head->used = mark.used;
    }
    a->bytes_used = mark.bytes_used;
}

void arena_reset(arena *a) {
    /* Keep the oldest chunk so a reused scratch arena doesn't hit malloc again. */
    while (a->head && a->head->prev) {
        arena_chunk *prev = a->head->prev;
        free(a->head);
        a->head = prev;
    }
    if (a->head) {
        a->head->used = 0;
    }
    a->bytes_used = 0;
}
