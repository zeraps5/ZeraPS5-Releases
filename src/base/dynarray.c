/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "base/dynarray.h"

#include <stdlib.h>

void *dyn__grow(void *data, size_t *cap, size_t need, size_t elem_size, bool *ok) {
    size_t new_cap = *cap ? *cap : 8;
    while (new_cap < need) {
        if (new_cap > SIZE_MAX / 2) {
            *ok = false;
            return data;
        }
        new_cap *= 2;
    }
    if (elem_size == 0 || new_cap > SIZE_MAX / elem_size) {
        *ok = false;
        return data;
    }
    void *grown = realloc(data, new_cap * elem_size);
    if (!grown) {
        *ok = false;
        return data;
    }
    *cap = new_cap;
    *ok = true;
    return grown;
}

void dyn__free(void *data) { free(data); }
