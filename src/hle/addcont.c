/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Installed add-on content (DLC): one folder per add-on under
 * user/dlc/<title id>/<folder>/, holding the add-on's files as dumped (its
 * sce_sys/param.json names it). The unified entitlement label is the last 16
 * characters of its contentId (UP0000-PPSA00000_00-LABEL0123456789); without a
 * param.json the folder name is the label. libSceAppContent lists and mounts
 * them, libSceNpEntitlementAccess reports them as installed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "core/settings.h"
#include "hle/addcont.h"
#include "pal/pal.h"

#define ADDCONT_MAX 64

static struct {
    char label[17];
    char dir[512];
} g_ac[ADDCONT_MAX];
static uint32_t g_ac_count;
static bool g_ac_scanned;

typedef struct scan_ctx {
    const char *root;
} scan_ctx;

/* "contentId": "UP0177-PPSA03831_00-SONICFRONTDLC001" -> SONICFRONTDLC001 */
static bool label_from_param(const char *dir, char out[17]) {
    char path[600];
    snprintf(path, sizeof(path), "%s/sce_sys/param.json", dir);
    uint8_t *data = NULL;
    uint64_t size = 0;
    if (pal_file_read_all(path, &data, &size) != PS5C_OK) return false;
    bool ok = false;
    char *text = (char *)realloc(data, size + 1);
    if (text) {
        text[size] = 0;
        const char *k = strstr(text, "\"contentId\"");
        const char *q = k ? strchr(k + 11, '"') : NULL;
        const char *e = q ? strchr(q + 1, '"') : NULL;
        if (q && e) {
            const char *id = q + 1;
            size_t n = (size_t)(e - id);
            const char *dash = n > 20 ? (const char *)memchr(id + 7, '-', n - 7) : NULL; /* after "UP0000-" */
            if (dash && (size_t)(e - (dash + 1)) == 16) {
                memcpy(out, dash + 1, 16);
                out[16] = 0;
                ok = true;
            }
        }
        free(text);
    } else {
        free(data);
    }
    return ok;
}

static bool on_entry(void *user, const char *name, bool is_dir) {
    scan_ctx *ctx = (scan_ctx *)user;
    if (!is_dir || name[0] == '.' || g_ac_count >= ADDCONT_MAX) return true;
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/%s", ctx->root, name);
    char label[17] = {0};
    if (!label_from_param(dir, label)) {
        size_t n = strlen(name);
        if (n > 16) {
            LOG_W(HLE, "add-on folder '%s': no sce_sys/param.json and a name longer than 16 characters: skipped", name);
            return true;
        }
        memcpy(label, name, n);
    }
    memcpy(g_ac[g_ac_count].label, label, sizeof(label));
    snprintf(g_ac[g_ac_count].dir, sizeof(g_ac[g_ac_count].dir), "%s", dir);
    LOG_I(HLE, "add-on content %s: %s", label, dir);
    g_ac_count++;
    return true;
}

static void scan(void) {
    if (g_ac_scanned) return;
    g_ac_scanned = true;
    char title[32];
    if (!settings_param_string("titleId", title, sizeof(title)) || !title[0]) return;
    char root[256];
    snprintf(root, sizeof(root), "user/dlc/%s", title);
    scan_ctx ctx = {root};
    pal_dir_list(root, on_entry, &ctx);
}

uint32_t addcont_count(void) {
    scan();
    return g_ac_count;
}

const char *addcont_label(uint32_t i) {
    scan();
    return i < g_ac_count ? g_ac[i].label : NULL;
}

const char *addcont_dir(uint32_t i) {
    scan();
    return i < g_ac_count ? g_ac[i].dir : NULL;
}

int32_t addcont_find(const char *label16) {
    scan();
    for (uint32_t i = 0; i < g_ac_count; i++)
        if (!strncmp(g_ac[i].label, label16, 16)) return (int32_t)i;
    return -1;
}
