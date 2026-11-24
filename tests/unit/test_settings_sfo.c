/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/settings.h"
#include "pal/pal.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "settings_sfo line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static void put16(uint8_t *p, unsigned value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, unsigned value) {
    put16(p, value);
    put16(p + 2, value >> 16);
}

static bool write_fixture(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool ok = fwrite(data, 1, size, file) == size;
    return fclose(file) == 0 && ok;
}

int main(void) {
    CHECK(pal_init() == PS5C_OK);
    pal_mkdir("sce_sys");
    pal_mkdir("invalid");
    pal_mkdir("invalid/sce_sys");
    const char *keys[] = {"TITLE_ID", "CONTENT_ID", "TITLE", "TITLE_17", "TITLE_02"};
    const char *values[] = {"CUSA12345", "UP0000-CUSA12345_00-0000000000000000", "Game", "Jogo \"A\" \\ B", "French"};
    enum { KEY_TABLE = 100, VALUE_TABLE = 160 };
    uint8_t sfo[256] = {0, 'P', 'S', 'F'};
    put32(sfo + 4, 0x101);
    put32(sfo + 8, KEY_TABLE);
    put32(sfo + 12, VALUE_TABLE);
    put32(sfo + 16, PS5C_ARRAY_COUNT(keys));
    unsigned key_off = 0, value_off = 0;
    for (unsigned i = 0; i < PS5C_ARRAY_COUNT(keys); i++) {
        unsigned key_len = (unsigned)strlen(keys[i]) + 1;
        unsigned value_len = (unsigned)strlen(values[i]) + 1;
        uint8_t *entry = sfo + 20 + i * 16;
        put16(entry, key_off);
        put16(entry + 2, i == 4 ? 0x0404 : 0x0204);
        put32(entry + 4, value_len);
        put32(entry + 8, value_len);
        put32(entry + 12, value_off);
        memcpy(sfo + KEY_TABLE + key_off, keys[i], key_len);
        memcpy(sfo + VALUE_TABLE + value_off, values[i], value_len);
        key_off += key_len;
        value_off += value_len;
    }
    remove("sce_sys/param.json");
    CHECK(write_fixture("sce_sys/param.sfo", sfo, sizeof(sfo)));
    char text[128];
    settings_load(".");
    CHECK(settings_param_string("titleId", text, sizeof(text)) && !strcmp(text, values[0]));
    CHECK(settings_param_string("contentId", text, sizeof(text)) && !strcmp(text, values[1]));
    CHECK(settings_param_title(17, text, sizeof(text)) && !strcmp(text, values[3]));
    CHECK(settings_param_title(2, text, sizeof(text)) && !strcmp(text, values[2]));
    static const char json[] =
        "{\"titleName\":\"Base title\",\"localizedParameters\":{\"defaultLanguage\":\"en-US\","
        "\"pt-BR\":{\"titleName\":\"Jogo em JSON\"},\"en-US\":{\"titleName\":\"JSON default\"}}}";
    CHECK(write_fixture("sce_sys/param.json", json, sizeof(json) - 1));
    settings_load(".");
    CHECK(settings_param_title(17, text, sizeof(text)) && !strcmp(text, "Jogo em JSON"));
    CHECK(settings_param_title(2, text, sizeof(text)) && !strcmp(text, "JSON default"));
    put32(sfo + 8, 20); /* Key table overlaps the entries. */
    CHECK(write_fixture("invalid/sce_sys/param.sfo", sfo, sizeof(sfo)));
    settings_load("invalid");
    CHECK(!settings_param_title(17, text, sizeof(text)));
    pal_shutdown();
    return 0;
}
