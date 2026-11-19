/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Game updates (patches). On the console an update is installed beside the
 * game and laid over it: /app0 shows the update's files where it has them and
 * the game's elsewhere, and the update's eboot.bin and sce_module/ are the ones
 * that run. Here a patch is a folder holding the update's files as dumped:
 *   user/patch/<title id>/                    (what the launcher's import makes)
 *   <PPSAxxxxx-patch0 or -patch> beside the game's <PPSAxxxxx-app0 or -app>
 * with a sce_sys/param.json of the same title id. PS5C_NO_PATCH=1 runs the
 * game without it. */
#ifndef PS5C_CORE_GAME_PATCH_H
#define PS5C_CORE_GAME_PATCH_H

#include <stdbool.h>
#include <stddef.h>

/* The patch folder of the game in game_dir (with or without a trailing slash),
 * without a trailing slash. False: none. */
bool game_patch_find(const char *game_dir, char *out, size_t cap);

/* A folder of decrypted executables beside the game (dumps keep the game's encrypted files
 * in <TITLEID>-app0 and the decrypted eboot.bin and sce_module/ in a folder of their own, without
 * a param.json): the game folder it belongs to, the only sibling with a sce_sys/param.json and an
 * eboot.bin, or the folder holding it (backport dumps: <game>/decrypted/). It is then laid
 * over that game like an update. False: dir is not one. */
bool game_patch_base_of(const char *dir, char *out, size_t cap);

/* The running game's folders (both without a trailing slash); NULL patch: none. */
void game_patch_set(const char *base_dir, const char *patch_dir);
const char *game_patch_base(void);  /* NULL when no patch is in use */
const char *game_patch_dir(void);   /* NULL when no patch is in use */

#endif
