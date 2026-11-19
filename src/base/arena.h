/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Chunked bump allocator. Objects that share a lifetime (one shader
 * compilation, one GPU frame, one loaded module) are allocated from the same
 * arena and released together. */
#ifndef PS5C_ARENA_H
#define PS5C_ARENA_H

#include "base/base.h"

typedef struct arena_chunk arena_chunk;

typedef struct arena {
    arena_chunk *head; /* most recent chunk */
    size_t chunk_size; /* default capacity of new chunks */
    size_t bytes_used; /* sum of live allocations, including alignment padding */
} arena;

typedef struct arena_mark {
    arena_chunk *chunk;
    size_t used;
    size_t bytes_used;
} arena_mark;

#define ARENA_DEFAULT_CHUNK ((size_t)64 * 1024)

void arena_init(arena *a, size_t chunk_size);
void arena_free(arena *a);

/* Returns NULL on allocation failure. `align` must be a power of two. */
void *arena_alloc(arena *a, size_t size, size_t align);
void *arena_zalloc(arena *a, size_t size, size_t align);
char *arena_strdup(arena *a, const char *s);

#define ARENA_NEW(a, T) ((T *)arena_zalloc((a), sizeof(T), _Alignof(T)))
#define ARENA_NEW_ARRAY(a, T, n) ((T *)arena_zalloc((a), sizeof(T) * (size_t)(n), _Alignof(T)))

/* Scratch usage: take a mark, allocate, then roll back to the mark. */
arena_mark arena_get_mark(const arena *a);
void arena_reset_to(arena *a, arena_mark mark);
void arena_reset(arena *a);

#endif
