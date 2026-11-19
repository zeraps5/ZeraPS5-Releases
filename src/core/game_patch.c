/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/game_patch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "pal/pal.h"

static char g_base[512], g_patch[512];

/* "titleId": "PPSA01234" of <dir>/sce_sys/param.json */
static bool title_of(const char *dir, char *out, size_t cap) {
    char path[700];
    snprintf(path, sizeof(path), "%s/sce_sys/param.json", dir);
    uint8_t *data;
    uint64_t size;
    if (pal_file_read_all(path, &data, &size) != PS5C_OK) return false;
    char *text = (char *)realloc(data, size + 1);
    if (!text) {
        free(data);
        return false;
    }
    text[size] = 0;
    bool ok = false;
    const char *p = strstr(text, "\"titleId\"");
    if (p) p = strchr(p + 9, ':');
    if (p) p = strchr(p, '"');
    const char *end = p ? strchr(p + 1, '"') : NULL;
    if (end && end - p - 1 > 0 && (size_t)(end - p - 1) < cap) {
        memcpy(out, p + 1, (size_t)(end - p - 1));
        out[end - p - 1] = 0;
        ok = true;
        for (const char *c = out; *c; c++)
            ok = ok && (*c == '_' || *c == '-' || (*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') ||
                        (*c >= 'a' && *c <= 'z'));
    }
    free(text);
    return ok;
}

/* dir holds an update of title id */
static bool is_patch_of(const char *dir, const char *id) {
    char other[64];
    pal_path_info info;
    return pal_path_stat(dir, &info) == PS5C_OK && info.is_dir && title_of(dir, other, sizeof(other)) && !strcmp(other, id);
}

bool game_patch_find(const char *game_dir, char *out, size_t cap) {
    if (getenv("PS5C_NO_PATCH")) return false;
    char base[512];
    snprintf(base, sizeof(base), "%s", game_dir);
    size_t n = strlen(base);
    while (n > 1 && (base[n - 1] == '/' || base[n - 1] == '\\')) base[--n] = 0;
    char id[64];
    if (!title_of(base, id, sizeof(id))) return false;

    char cand[6][600];
    uint32_t count = 0;
    snprintf(cand[count++], sizeof(cand[0]), "user/patch/%s", id);
    /* beside the game: <parent>/<leaf with -app0/-app as -patch0/-patch>, <parent>/<id>-patch0, <id>-patch */
    char parent[512];
    snprintf(parent, sizeof(parent), "%s", base);
    char *slash = strrchr(parent, '/'), *bslash = strrchr(parent, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    const char *leaf = slash ? slash + 1 : parent;
    char leaf_copy[256];
    size_t leaf_len = strlen(leaf);
    if (leaf_len >= sizeof(leaf_copy)) return false;
    memcpy(leaf_copy, leaf, leaf_len + 1);
    if (slash) *slash = 0;
    else snprintf(parent, sizeof(parent), ".");
    size_t ll = strlen(leaf_copy);
    if (ll > 5 && !strcmp(leaf_copy + ll - 5, "-app0"))
        snprintf(cand[count++], sizeof(cand[0]), "%s/%.*s-patch0", parent, (int)(ll - 5), leaf_copy);
    else if (ll > 4 && !strcmp(leaf_copy + ll - 4, "-app"))
        snprintf(cand[count++], sizeof(cand[0]), "%s/%.*s-patch", parent, (int)(ll - 4), leaf_copy);
    snprintf(cand[count++], sizeof(cand[0]), "%s/%s-patch0", parent, id);
    snprintf(cand[count++], sizeof(cand[0]), "%s/%s-patch", parent, id);

    for (uint32_t i = 0; i < count; i++) {
        if (!strcmp(cand[i], base) || !is_patch_of(cand[i], id)) continue;
        if (strlen(cand[i]) >= cap) return false;
        snprintf(out, cap, "%s", cand[i]);
        return true;
    }
    return false;
}

typedef struct sibling_scan {
    const char *parent, *self;
    uint64_t eboot_size; /* of self's eboot.bin */
    char found[512], same[512];
    uint32_t count, same_count;
} sibling_scan;

static bool sibling_entry(void *user, const char *name, bool is_dir) {
    sibling_scan *s = (sibling_scan *)user;
    if (!is_dir || !strcmp(name, ".") || !strcmp(name, "..")) return true;
    char dir[512], probe[600], id[64];
    snprintf(dir, sizeof(dir), "%s/%s", s->parent, name);
    if (!strcmp(dir, s->self)) return true;
    snprintf(probe, sizeof(probe), "%s/eboot.bin", dir);
    pal_path_info info;
    if (pal_path_stat(probe, &info) != PS5C_OK || info.is_dir) return true;
    if (title_of(dir, id, sizeof(id))) {
        snprintf(s->found, sizeof(s->found), "%s", dir);
        s->count++;
    } else if (s->eboot_size && info.size == s->eboot_size) {
        snprintf(s->same, sizeof(s->same), "%s", dir);
        s->same_count++;
    }
    return true;
}

bool game_patch_base_of(const char *dir, char *out, size_t cap) {
    if (getenv("PS5C_NO_PATCH")) return false;
    char self[512], id[64], probe[600];
    snprintf(self, sizeof(self), "%s", dir);
    size_t n = strlen(self);
    while (n > 1 && (self[n - 1] == '/' || self[n - 1] == '\\')) self[--n] = 0;
    if (title_of(self, id, sizeof(id))) return false; /* a game (or an update) of its own */
    char parent[512];
    snprintf(parent, sizeof(parent), "%s", self);
    char *slash = strrchr(parent, '/'), *bslash = strrchr(parent, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    if (slash) *slash = 0;
    else snprintf(parent, sizeof(parent), ".");
    pal_path_info info;
    /* backport dumps: decrypted/ inside the game folder (Trifox, Hello Kitty, Bubble Bobble) */
    snprintf(probe, sizeof(probe), "%s/eboot.bin", parent);
    if (title_of(parent, id, sizeof(id)) && pal_path_stat(probe, &info) == PS5C_OK && !info.is_dir) {
        if (strlen(parent) >= cap) return false;
        snprintf(out, cap, "%s", parent);
        return true;
    }
    sibling_scan scan = {parent, self, 0, {0}, {0}, 0, 0};
    snprintf(probe, sizeof(probe), "%s/eboot.bin", self);
    if (pal_path_stat(probe, &info) == PS5C_OK && !info.is_dir) scan.eboot_size = info.size;
    if (pal_dir_list(parent, sibling_entry, &scan) != PS5C_OK) return false;
    /* the one sibling game, or else, in a dump without param.json in either half
     * (Gothic 1: dec/ beside PPSA09826-app0/), the one sibling with the same eboot.bin size */
    const char *base = scan.count == 1 ? scan.found : !scan.count && scan.same_count == 1 ? scan.same : NULL;
    if (!base || strlen(base) >= cap) return false;
    snprintf(out, cap, "%s", base);
    return true;
}

void game_patch_set(const char *base_dir, const char *patch_dir) {
    if (!patch_dir) {
        g_base[0] = g_patch[0] = 0;
        return;
    }
    snprintf(g_base, sizeof(g_base), "%s", base_dir);
    snprintf(g_patch, sizeof(g_patch), "%s", patch_dir);
    size_t n = strlen(g_base);
    while (n > 1 && (g_base[n - 1] == '/' || g_base[n - 1] == '\\')) g_base[--n] = 0;
    LOG_I(LOADER, "update: %s over %s", g_patch, g_base);
}

const char *game_patch_base(void) { return g_patch[0] ? g_base : NULL; }
const char *game_patch_dir(void) { return g_patch[0] ? g_patch : NULL; }
