/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Growable array for long-lived data (tables, module lists). Short-lived data
 * should use an arena instead.
 *
 *   DYNARRAY(uint32_t) v = {0};
 *   if (!dyn_push(&v, 42)) { ...out of memory... }
 *   dyn_free(&v);
 *
 * The array argument is evaluated more than once: pass a plain lvalue. */
#ifndef PS5C_DYNARRAY_H
#define PS5C_DYNARRAY_H

#include "base/base.h"

#define DYNARRAY(T)                                                                                \
    struct {                                                                                       \
        T *data;                                                                                   \
        size_t len;                                                                                \
        size_t cap;                                                                                \
        bool ok_;                                                                                  \
    }

/* Returns the (possibly moved) buffer; on failure returns `data` unchanged and sets *ok = false. */
void *dyn__grow(void *data, size_t *cap, size_t need, size_t elem_size, bool *ok);

#define dyn_reserve(a, n)                                                                          \
    ((size_t)(n) <= (a)->cap                                                                       \
         ? true                                                                                    \
         : ((a)->data = dyn__grow((a)->data, &(a)->cap, (size_t)(n), sizeof(*(a)->data), &(a)->ok_), \
            (a)->ok_))

#define dyn_push(a, v) (dyn_reserve((a), (a)->len + 1) ? ((a)->data[(a)->len++] = (v), true) : false)

#define dyn_pop(a) ((a)->data[--(a)->len])
#define dyn_last(a) ((a)->data[(a)->len - 1])
#define dyn_clear(a) ((a)->len = 0)

void dyn__free(void *data);
#define dyn_free(a) (dyn__free((a)->data), (a)->data = NULL, (a)->len = 0, (a)->cap = 0)

#endif
