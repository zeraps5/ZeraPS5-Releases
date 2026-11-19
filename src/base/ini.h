/* SPDX-License-Identifier: GPL-2.0-or-later */
/* INI documents (UTF-8): [section] headers, key=value lines, full-line
 * comments starting with ; or #. Names compare without case. Editing keeps
 * every other line (comments, order) as it was, so users can edit the files
 * by hand. The caller reads and writes the file. */
#ifndef PS5C_INI_H
#define PS5C_INI_H

#include "base/base.h"

typedef struct ini ini;

/* A document from text (a UTF-8 BOM is skipped); NULL only when out of memory. */
ini *ini_parse(const char *text, size_t len);
void ini_free(ini *f);
/* The document as text (malloc'd, NUL-terminated), lines ending in \r\n. */
char *ini_text(const ini *f, size_t *len_out);

/* The value, or NULL when the key is absent. */
const char *ini_get(const ini *f, const char *section, const char *key);
int64_t ini_get_int(const ini *f, const char *section, const char *key, int64_t def);
/* Replaces or adds the key (a new section goes at the end); NULL removes it. */
bool ini_set(ini *f, const char *section, const char *key, const char *value);
/* Key names of a section in file order; returns how many there are (at most max stored). */
uint32_t ini_keys(const ini *f, const char *section, const char **keys, uint32_t max);

#endif
