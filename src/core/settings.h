/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The user settings of this run (see core/user_config.h): the game's own
 * file first, then the global one. The global file is created with the
 * defaults when missing and never rewritten; a key it lacks (a setting
 * newer than the file) reads as absent, and callers use their default. */
#ifndef PS5C_SETTINGS_H
#define PS5C_SETTINGS_H

#include "base/base.h"

/* game_dir: the folder holding the eboot (its sce_sys/param.json names the
 * title ID of the per-game file); NULL for none. */
void settings_load(const char *game_dir);
const char *settings_get(const char *section, const char *key);
int64_t settings_get_int(const char *section, const char *key, int64_t def);
/* An integer of the game's sce_sys/param.json ("userDefinedParam1": 3), or
 * of [jogo] in the game's settings file when set there: false when absent. */
bool settings_param_int(const char *key, int64_t *value);
/* A string of the game's param.json (its first "titleName" is the default
 * language's name): false when absent. */
bool settings_param_string(const char *key, char *out, size_t cap);
/* The game's name in the language of an SCE language code (param.json localizedParameters),
 * else in its default language, else the first one given */
bool settings_param_title(int32_t language, char *out, size_t cap);
/* The TV the game sees: a 4K one when [geral] resolucao=2160, else 1080p;
 * PS5C_RESOLUTION=4k|1080 overrides the settings */
bool settings_output_4k(void);
/* The height a game renders at when it can be set (game fixes, Unreal games): [geral] resolucao_interna
 * or PS5C_INTERNAL_RES = 1440, 1080, 900, 720 or 540 (16:9), else 0: the game's own size */
int settings_internal_height(void);
/* The game itself renders at that height (a game fix, an Unreal scale): the renderer's
 * generic scaling of render targets stays off */
void settings_internal_height_native(void);
bool settings_internal_height_is_native(void);
/* The [teclado] bindings to the host keyboard input */
void settings_apply_keyboard(void);

#endif
