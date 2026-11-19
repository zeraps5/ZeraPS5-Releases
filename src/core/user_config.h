/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The user settings files (portable, in the emulator's folder):
 *   user/config/zeraps5.ini       every game
 *   user/config/<title id>.ini    one game: only what differs from the above
 * Sections: [geral] idioma, resolucao; [pastas] jogos1.., sistema;
 * [video] placa; [teclado] one key list per control; [controle] vibracao; [atualizacao] fonte
 * (optional: overrides the launcher's built-in update source). Shared by the emulator
 * and the launcher. */
#ifndef PS5C_USER_CONFIG_H
#define PS5C_USER_CONFIG_H

#include "base/ini.h"

#define USER_CONFIG_DIR "user/config"
#define USER_CONFIG_FILE "user/config/zeraps5.ini"

#define USER_LANGUAGE_DEFAULT 17 /* Portuguese (Brazil) */
#define USER_RESOLUTION_DEFAULT 1080

/* The file written when there is none: every setting at its default, with
 * comments for editing by hand. */
extern const char user_config_default_text[];

/* Controls of [teclado]: key in the file, name shown to users, PAL control, default keys */
typedef struct user_control {
    const char *key;
    const char *label;
    uint32_t control;
    const char *keys;
} user_control;

extern const user_control user_controls[];
extern const uint32_t user_control_count;

#endif
