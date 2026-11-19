/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Built-in compatibility patches: memory changes, never writable game files.
 * IDs are stable keys in [patches]. */
#ifndef PS5C_COMPAT_CATALOG_H
#define PS5C_COMPAT_CATALOG_H
#include "base/base.h"

typedef struct compat_patch {
    const char *id, *title_id, *version, *name, *description;
    const char *sha256; /* exact, unmodified executable container */
    uint64_t file_offset;
    const uint8_t *expected, *replacement;
    size_t size;
    bool default_enabled;
} compat_patch;

typedef enum compat_patch_status {
    COMPAT_PATCH_READY, COMPAT_PATCH_WRONG_TITLE, COMPAT_PATCH_WRONG_VERSION,
    COMPAT_PATCH_WRONG_BINARY, COMPAT_PATCH_WRONG_BYTES, COMPAT_PATCH_UNMAPPED,
    COMPAT_PATCH_DISABLED, COMPAT_PATCH_APPLIED
} compat_patch_status;

size_t compat_patch_count(void);
const compat_patch *compat_patch_at(size_t index);
/* Fail closed on absent metadata, unknown updates, edited files and short
 * input. The launcher and runtime use the same identity check. */
compat_patch_status compat_patch_check(const compat_patch *p, const char *title,
                                      const char *version, const uint8_t *file, size_t size);
#endif
