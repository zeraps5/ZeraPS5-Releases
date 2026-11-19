/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fixes for single games, written into their code once a module is relocated
 * (before any of it runs). Each names the title, the module, an offset and the
 * bytes it expects there: another version of the game is left untouched.
 * PS5C_NO_GAME_FIXES=1 turns them all off. */
#ifndef PS5C_GAME_FIXES_H
#define PS5C_GAME_FIXES_H

#include "base/base.h"
#include "loader/module_set.h"
#include "mem/gmem.h"
#include "core/compat_catalog.h"

/* Applies the fixes of the running title that belong to module m. */
void game_fixes_apply(gmem *mem, const loaded_module *m);

/* Change only a relocated main executable's private guest mapping. */
compat_patch_status game_compat_apply(gmem *mem, const loaded_module *m,
                                     const compat_patch *patch, const char *title,
                                     const char *version, bool enabled);

#endif
