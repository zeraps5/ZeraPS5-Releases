/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_BUNDLED_ASSETS_H
#define PS5C_BUNDLED_ASSETS_H
#define PS5C_BUNDLED_FONT 201
#define PS5C_BUNDLED_INTRO 202
#ifndef RC_INVOKED
#include <stddef.h>
/* Read-only bytes owned by the executable until process exit. No file writes.
 * Missing/unknown resources return NULL and size zero. */
#if defined(_WIN32)
const void *pal_bundled_asset(unsigned id, size_t *size);
#else
static inline const void *pal_bundled_asset(unsigned id, size_t *size) {
    (void)id;
    if (size) *size = 0;
    return NULL;
}
#endif
#endif
#endif
