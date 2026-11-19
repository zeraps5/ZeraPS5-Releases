/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Installed add-on content (addcont.c): user/dlc/<title id>/<folder>/. */
#ifndef PS5C_HLE_ADDCONT_H
#define PS5C_HLE_ADDCONT_H
#include <stdbool.h>
#include <stdint.h>

uint32_t addcont_count(void);
/* the 16-character unified entitlement label, NUL-terminated */
const char *addcont_label(uint32_t i);
/* the host folder with the add-on's files */
const char *addcont_dir(uint32_t i);
/* index of the add-on with that label (16 characters compared), -1 if none */
int32_t addcont_find(const char *label16);

#endif
