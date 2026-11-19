/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/settings.h"
#include "pal/native_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/ini.h"
#include "base/log.h"
#include "core/user_config.h"
#include "pal/pal.h"

static ini *g_global, *g_game;
static char *g_param; /* the game's param.json text */

static ini *read_ini(const char *path) {
    uint8_t *data;
    uint64_t size;
    if (pal_file_read_all(path, &data, &size) != PS5C_OK) return NULL;
    ini *f = ini_parse((const char *)data, (size_t)size);
    free(data);
    return f;
}

static char *read_param(const char *game_dir) {
    char path[700];
    snprintf(path, sizeof(path), "%s%ssce_sys/param.json", game_dir,
             *game_dir && !strchr("/\\", game_dir[strlen(game_dir) - 1]) ? "/" : "");
    uint8_t *data;
    uint64_t size;
    if (pal_file_read_all(path, &data, &size) != PS5C_OK) return NULL;
    char *text = (char *)realloc(data, size + 1);
    if (!text) {
        free(data);
        return NULL;
    }
    text[size] = 0;
    return text;
}

/* The value after "key": in param.json, without a full JSON parser */
static const char *param_value(const char *key) {
    char quoted[80];
    snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    const char *p = g_param ? strstr(g_param, quoted) : NULL;
    if (p) p = strchr(p + strlen(quoted), ':');
    if (!p) return NULL;
    while (*p == ':' || *p == ' ' || *p == 9 || *p == 10 || *p == 13) p++;
    return p;
}

/* "titleId": "PPSA01234" */
static bool title_id(char *out, size_t cap) {
    const char *p = param_value("titleId");
    if (!p || *p != '"') return false;
    const char *end = strchr(p + 1, '"');
    if (!end || end - p - 1 <= 0 || (size_t)(end - p - 1) >= cap) return false;
    memcpy(out, p + 1, (size_t)(end - p - 1));
    out[end - p - 1] = 0;
    bool ok = true;
    for (char *c = out; *c; c++) ok = ok && (*c == '_' || *c == '-' || (*c >= '0' && *c <= '9') ||
                                             (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z'));
    return ok;
}

bool settings_param_int(const char *key, int64_t *value) {
    /* the game's own file may override it: [jogo] userDefinedParam1=2 */
    const char *p = ini_get(g_game, "jogo", key);
    if (!p || !*p) p = param_value(key);
    if (!p) return false;
    char *end;
    long long n = strtoll(p, &end, 10);
    if (end == p) return false;
    *value = (int64_t)n;
    return true;
}

bool settings_param_string(const char *key, char *out, size_t cap) {
    const char *p = param_value(key);
    if (!p || *p != '"' || !cap) return false;
    size_t n = 0;
    for (p++; *p && *p != '"' && n + 1 < cap; p++) {
        if (*p == '\\' && p[1]) p++; /* escaped quote or backslash (names carry no unicode escapes) */
        out[n++] = *p;
    }
    out[n] = 0;
    return n > 0;
}

/* The string value of key inside the object at p (up to its closing brace) */
static bool string_in_object(const char *p, const char *key, char *out, size_t cap) {
    char quoted[80];
    snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    const char *end = strchr(p, '}'), *k = strstr(p, quoted);
    if (!k || (end && k > end)) return false;
    k = strchr(k + strlen(quoted), ':');
    if (!k) return false;
    while (*k == ':' || *k == ' ' || *k == 9 || *k == 10 || *k == 13) k++;
    if (*k != '"' || !cap) return false;
    size_t n = 0;
    for (k++; *k && *k != '"' && n + 1 < cap; k++) {
        if (*k == '\\' && k[1]) k++;
        out[n++] = *k;
    }
    out[n] = 0;
    return n > 0;
}

/* The titleName of the localizedParameters entry for lang ("pt-BR") */
static bool localized_title(const char *lang, char *out, size_t cap) {
    const char *lp = g_param ? strstr(g_param, "\"localizedParameters\"") : NULL;
    char quoted[24];
    snprintf(quoted, sizeof(quoted), "\"%s\"", lang);
    const char *at = lp ? strstr(lp, quoted) : NULL;
    if (!at) return false;
    at = strchr(at + strlen(quoted), ':');
    while (at && (*at == ':' || *at == ' ' || *at == 9 || *at == 10 || *at == 13)) at++;
    return at && *at == '{' && string_in_object(at + 1, "titleName", out, cap);
}

bool settings_param_title(int32_t language, char *out, size_t cap) {
    /* SCE language codes and the locales of param.json */
    static const char *const k_locales[] = {"ja-JP", "en-US", "fr-FR", "es-ES", "de-DE", "it-IT", "nl-NL", "pt-PT",
                                            "ru-RU", "ko-KR", "zh-Hant", "zh-Hans", "fi-FI", "sv-SE", "da-DK", "no-NO",
                                            "pl-PL", "pt-BR", "en-GB", "tr-TR", "es-419", "ar-AE", "fr-CA", "cs-CZ",
                                            "hu-HU", "el-GR", "ro-RO", "th-TH", "vi-VN", "id-ID", "uk-UA"};
    char def[24] = "";
    const char *lp = g_param ? strstr(g_param, "\"localizedParameters\"") : NULL;
    if (lp) string_in_object(lp, "defaultLanguage", def, sizeof(def));
    if (language >= 0 && language < (int32_t)(sizeof(k_locales) / sizeof(k_locales[0])) &&
        localized_title(k_locales[language], out, cap))
        return true;
    if ((def[0] && localized_title(def, out, cap)) || localized_title("en-US", out, cap)) return true;
    return settings_param_string("titleName", out, cap); /* the first one in the file */
}

void settings_load(const char *game_dir) {
    ini_free(g_global);
    ini_free(g_game);
    g_global = read_ini(USER_CONFIG_FILE);
    if (!g_global) {
        /* first run: the defaults, written out for editing (never over an existing file) */
        pal_mkdir("user");
        pal_mkdir(USER_CONFIG_DIR);
        pal_file *f;
        if (pal_file_open(USER_CONFIG_FILE, PAL_FILE_WRITE | PAL_FILE_CREATE, &f) == PS5C_OK) {
            uint64_t size = 0;
            pal_file_size(f, &size);
            if (!size) pal_file_write(f, user_config_default_text, strlen(user_config_default_text), NULL);
            pal_file_close(f);
            LOG_I(CORE, "settings: created %s with the defaults", USER_CONFIG_FILE);
        }
        g_global = read_ini(USER_CONFIG_FILE);
        if (!g_global) g_global = ini_parse(user_config_default_text, strlen(user_config_default_text));
    }
    g_game = NULL;
    free(g_param);
    g_param = game_dir ? read_param(game_dir) : NULL;
    char id[64], path[128];
    if (title_id(id, sizeof(id))) {
        snprintf(path, sizeof(path), "%s/%s.ini", USER_CONFIG_DIR, id);
        g_game = read_ini(path);
        LOG_I(CORE, "settings: %s%s%s", USER_CONFIG_FILE, g_game ? " + " : "", g_game ? path : "");
    }
}

const char *settings_get(const char *section, const char *key) {
    const char *v = ini_get(g_game, section, key);
    return v ? v : ini_get(g_global, section, key);
}

int64_t settings_get_int(const char *section, const char *key, int64_t def) {
    const char *v = settings_get(section, key);
    if (!v || !*v) return def;
    char *end;
    long long n = strtoll(v, &end, 0);
    return end == v ? def : (int64_t)n;
}

bool settings_output_4k(void) {
    static int res = -1;
    if (res < 0) {
        const char *env = getenv("PS5C_RESOLUTION");
        if (env && *env) res = !strcmp(env, "4k") || !strcmp(env, "4K") || !strcmp(env, "2160");
        else res = settings_get_int("geral", "resolucao", USER_RESOLUTION_DEFAULT) == 2160;
    }
    return res != 0;
}

int settings_internal_height(void) {
    static int height = -1;
    if (height < 0) {
        const char *env = getenv("PS5C_INTERNAL_RES");
        int64_t v = env && *env ? atoll(env) : settings_get_int("geral", "resolucao_interna", 0);
        height = v == 1440 || v == 1080 || v == 900 || v == 720 || v == 540 ? (int)v : 0;
    }
    return height;
}

void settings_apply_keyboard(void) {
    native_input_configure(settings_get_int("entrada", "teclado_nativo", 0) == 1,
                           settings_get_int("entrada", "mouse_nativo", 0) == 1);
    pal_input_has_controller();
    for (uint32_t i = 0; i < user_control_count; i++) {
        const char *keys = settings_get("teclado", user_controls[i].key);
        if (keys && !pal_input_bind_keys(user_controls[i].control, keys))
            LOG_W(CORE, "settings: [teclado] %s=%s has an unknown key name", user_controls[i].key, keys);
    }
}
