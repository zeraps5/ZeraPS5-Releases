/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/ue_scale.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "base/sha1.h"
#include "core/settings.h"
#include "pal/pal.h"

#define PAK_MAGIC 0x5A6F12E1u
#define PAK_FOOTER_V10 221u /* key GUID, encrypted flag, magic, version, index offset/size/hash, 5 x 32 names */
#define PAK_ENTRY_HEADER 53u /* offset, sizes, compression, hash, flags, block size (uncompressed) */
#define GEN_DIR "zeraps5-escala"
#define GEN_PAK "zz_zeraps5_escala_P.pak"

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static bool ends_with_ci(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    if (b > a) return false;
    for (size_t i = 0; i < b; i++)
        if (lower((unsigned char)s[a - b + i]) != lower((unsigned char)suffix[i])) return false;
    return true;
}
static bool contains_ci(const char *s, size_t n, const char *needle) {
    size_t m = strlen(needle);
    for (size_t i = 0; i + m <= n; i++) {
        size_t k = 0;
        while (k < m && lower((unsigned char)s[i + k]) == lower((unsigned char)needle[k])) k++;
        if (k == m) return true;
    }
    return false;
}

/* ---- Reading the game's pak (versions 10 and 11: path hash and full directory indexes) ---- */

typedef struct cursor {
    const uint8_t *p;
    size_t n, at;
    bool bad;
} cursor;

static uint32_t c32(cursor *c) {
    if (c->bad || c->n - c->at < 4) { c->bad = true; return 0; }
    uint32_t v = rd32(c->p + c->at);
    c->at += 4;
    return v;
}
static uint64_t c64(cursor *c) {
    if (c->bad || c->n - c->at < 8) { c->bad = true; return 0; }
    uint64_t v = rd64(c->p + c->at);
    c->at += 8;
    return v;
}
static void cskip(cursor *c, size_t n) {
    if (c->bad || c->n - c->at < n) c->bad = true;
    else c->at += n;
}
/* FString: a length with its terminator; negative for UTF-16 (kept as ASCII, others '?') */
static void cstr(cursor *c, char *out, size_t cap) {
    int32_t len = (int32_t)c32(c);
    size_t w = 0;
    if (len < 0) {
        size_t chars = (size_t)-(int64_t)len;
        if (c->bad || chars > (c->n - c->at) / 2) { c->bad = true; chars = 0; }
        for (size_t i = 0; i < chars; i++) {
            uint16_t u = (uint16_t)(c->p[c->at + 2 * i] | c->p[c->at + 2 * i + 1] << 8);
            if (u && w + 1 < cap) out[w++] = u < 128 ? (char)u : '?';
        }
        if (!c->bad) c->at += chars * 2;
    } else if (len > 0) {
        if (c->bad || (size_t)len > c->n - c->at) { c->bad = true; len = 0; }
        for (int32_t i = 0; i < len; i++)
            if (c->p[c->at + i] && w + 1 < cap) out[w++] = (char)c->p[c->at + i];
        if (!c->bad) c->at += (size_t)len;
    }
    out[w] = 0;
}

static bool read_at(pal_file *f, uint64_t offset, uint64_t size, uint8_t **out) {
    *out = (uint8_t *)malloc(size ? size : 1);
    uint64_t got = 0;
    if (*out && pal_file_pread(f, *out, size, offset, &got) == PS5C_OK && got == size) return true;
    free(*out);
    *out = NULL;
    return false;
}

static bool is_profile(const char *path) {
    return strncmp(path, "Engine/", 7) && (ends_with_ci(path, "/Config/PS5/PS5DeviceProfiles.ini") ||
                                           ends_with_ci(path, "/Platforms/PS5/Config/PS5DeviceProfiles.ini"));
}

/* The PS5 device profile of the game's project in one pak: its path and contents */
static bool pak_profile(const char *pak, char *name, size_t name_cap, uint8_t **data, size_t *size) {
    pal_file *f = NULL;
    if (pal_file_open(pak, PAL_FILE_READ, &f) != PS5C_OK) return false;
    bool ok = false;
    uint64_t total = 0;
    uint8_t *foot = NULL, *index = NULL, *fdi = NULL;
    if (pal_file_size(f, &total) != PS5C_OK || total < PAK_FOOTER_V10 ||
        !read_at(f, total - PAK_FOOTER_V10, PAK_FOOTER_V10, &foot))
        goto done;
    uint32_t version = rd32(foot + 21);
    uint64_t ioff = rd64(foot + 25), isize = rd64(foot + 33);
    if (rd32(foot + 17) != PAK_MAGIC || version < 10 || version > 11 || foot[16] /* encrypted index */ ||
        isize > (64u << 20) || ioff > total || isize > total - ioff || !read_at(f, ioff, isize, &index))
        goto done;
    cursor c = {index, (size_t)isize, 0, false};
    char text[1024];
    cstr(&c, text, sizeof(text)); /* mount point */
    c32(&c);                      /* entries */
    c64(&c);                      /* path hash seed */
    if (c32(&c)) cskip(&c, 8 + 8 + 20);
    if (!c32(&c)) goto done; /* no full directory index */
    uint64_t foff = c64(&c), fsize = c64(&c);
    cskip(&c, 20);
    uint32_t enc_size = c32(&c);
    if (c.bad || enc_size > c.n - c.at) goto done;
    const uint8_t *enc = index + c.at;
    if (fsize > (64u << 20) || foff > total || fsize > total - foff || !read_at(f, foff, fsize, &fdi)) goto done;
    cursor d = {fdi, (size_t)fsize, 0, false};
    int64_t found = -1;
    char dir[512], file[512];
    for (uint32_t dirs = c32(&d), i = 0; !d.bad && found < 0 && i < dirs; i++) {
        cstr(&d, dir, sizeof(dir));
        for (uint32_t files = c32(&d), k = 0; !d.bad && k < files; k++) {
            cstr(&d, file, sizeof(file));
            int32_t e = (int32_t)c32(&d);
            snprintf(text, sizeof(text), "%s%s", dir, file);
            if (found < 0 && e >= 0 && is_profile(text)) {
                found = e;
                snprintf(name, name_cap, "%.500s", text);
            }
        }
    }
    if (found < 0 || (size_t)found + 4 > enc_size) goto done;
    /* encoded entry: flags word, [block size], offset, uncompressed size (32 or 64 bits) */
    cursor e = {enc, enc_size, (size_t)found, false};
    uint32_t v = c32(&e);
    if ((v & 0x3F) == 0x3F) c32(&e);
    uint64_t offset = (v >> 31) & 1 ? c32(&e) : c64(&e);
    uint64_t usize = (v >> 30) & 1 ? c32(&e) : c64(&e);
    bool compressed = (v >> 23) & 0x3F, encrypted = (v >> 22) & 1;
    if (e.bad || compressed || encrypted || usize > (1u << 20) || offset > total || usize + PAK_ENTRY_HEADER > total - offset)
        goto done;
    ok = read_at(f, offset + PAK_ENTRY_HEADER, usize, data);
    *size = (size_t)usize;
done:
    free(foot);
    free(index);
    free(fdi);
    pal_file_close(f);
    return ok;
}

/* ---- The replacement profile ---- */

size_t ue_scale_profile(const char *in, size_t n, int percent, char *out, size_t cap) {
    /* the scene's own percentages only: r.RayTracing.Reflections.ScreenPercentage and the like stay
     * (Commandos Origins) */
    static const char *const dropped[] = {"=r.screenpercentage=", "secondaryscreenpercentage", "dynamicres.operationmode",
                                          "fsr.quality", "qualitymode"};
    size_t w = 0;
    bool profile = false;
    for (size_t at = 0; at < n;) {
        size_t end = at;
        while (end < n && in[end] != '\n') end++;
        size_t len = end - at, line_end = end < n ? end + 1 : end;
        const char *line = in + at;
        size_t s = 0;
        while (s < len && (line[s] == ' ' || line[s] == '\t')) s++;
        bool keep = true;
        if (s < len && line[s] == '[') {
            profile = contains_ci(line, len, " DeviceProfile]");
        } else if (profile) {
            for (size_t i = 0; i < sizeof(dropped) / sizeof(dropped[0]) && keep; i++)
                keep = !contains_ci(line, len, dropped[i]);
        }
        if (keep) {
            if (w + (line_end - at) >= cap) return 0;
            memcpy(out + w, line, line_end - at);
            w += line_end - at;
        }
        if (s < len && line[s] == '[' && profile) {
            if (end == n) { /* a header on the last line */
                if (w + 2 >= cap) return 0;
                out[w++] = '\r', out[w++] = '\n';
            }
            int k = snprintf(out + w, cap - w, "+CVars=r.DynamicRes.OperationMode=0\r\n+CVars=r.ScreenPercentage=%d\r\n", percent);
            if (k < 0 || (size_t)k >= cap - w) return 0;
            w += (size_t)k;
        }
        at = line_end;
    }
    if (w >= cap) return 0;
    out[w] = 0;
    return w;
}

/* ---- Writing the mod pak (version 7: legacy index, no compression names) ---- */

typedef struct out_buf {
    uint8_t *p;
    size_t n, cap;
    bool bad;
} out_buf;

static void put(out_buf *b, const void *d, size_t n) {
    if (b->bad) return;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        while (cap < b->n + n) cap *= 2;
        uint8_t *p = (uint8_t *)realloc(b->p, cap);
        if (!p) { b->bad = true; return; }
        b->p = p, b->cap = cap;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put32(out_buf *b, uint32_t v) {
    uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    put(b, x, 4);
}
static void put64(out_buf *b, uint64_t v) { put32(b, (uint32_t)v), put32(b, (uint32_t)(v >> 32)); }
static void put_str(out_buf *b, const char *s) {
    put32(b, (uint32_t)strlen(s) + 1);
    put(b, s, strlen(s) + 1);
}
static void put_entry(out_buf *b, uint64_t offset, const void *data, size_t n) {
    uint8_t hash[20];
    sha1(data, n, hash);
    put64(b, offset), put64(b, n), put64(b, n);
    put32(b, 0); /* no compression */
    put(b, hash, 20);
    uint8_t flags = 0;
    put(b, &flags, 1);
    put32(b, 0); /* compression block size */
}

uint8_t *ue_scale_pak(const char *name, const void *data, size_t n, size_t *size) {
    out_buf b = {0};
    put_entry(&b, 0, data, n); /* the header before the data carries offset 0, as UnrealPak writes it */
    put(&b, data, n);
    size_t index_at = b.n;
    put_str(&b, "../../../");
    put32(&b, 1);
    put_str(&b, name);
    put_entry(&b, 0, data, n);
    size_t index_size = b.n - index_at;
    uint8_t hash[20], zero[17] = {0};
    if (!b.bad) sha1(b.p + index_at, index_size, hash);
    put(&b, zero, 17); /* encryption key GUID, index not encrypted */
    put32(&b, PAK_MAGIC), put32(&b, 7), put64(&b, index_at), put64(&b, index_size);
    put(&b, hash, 20);
    if (b.bad) {
        free(b.p);
        return NULL;
    }
    *size = b.n;
    return b.p;
}

/* ---- Setting it up ---- */

typedef struct scan {
    const char *base;
    char found[64][160];
    uint32_t count;
} scan;

static bool scan_entry(void *user, const char *name, bool is_dir) {
    scan *s = (scan *)user;
    bool want = s->base ? !is_dir && ends_with_ci(name, ".pak") : is_dir && name[0] != '.';
    if (want && s->count < 64 && strlen(name) < sizeof(s->found[0])) snprintf(s->found[s->count++], sizeof(s->found[0]), "%s", name);
    return true;
}

static int cmp_names(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

/* Removes this module's pak and the folders it made, and nothing else */
static void remove_generated(const char *gen) {
    scan projects = {NULL, {{0}}, 0};
    if (pal_dir_list(gen, scan_entry, &projects) != PS5C_OK) return;
    char path[700];
    for (uint32_t i = 0; i < projects.count; i++) {
        snprintf(path, sizeof(path), "%s/%s/Saved/Paks/" GEN_PAK, gen, projects.found[i]);
        if (pal_file_remove(path) == PS5C_OK) LOG_I(CORE, "Unreal: internal resolution back to the game's own");
        snprintf(path, sizeof(path), "%s/%s/Saved/Paks", gen, projects.found[i]);
        pal_rmdir(path);
        snprintf(path, sizeof(path), "%s/%s/Saved", gen, projects.found[i]);
        pal_rmdir(path);
        snprintf(path, sizeof(path), "%s/%s", gen, projects.found[i]);
        pal_rmdir(path);
    }
    pal_rmdir(gen);
}

void ue_scale_prepare(const char *app_dir, const char *title_id) {
    int64_t percent = settings_get_int("geral", "escala_unreal", 0);
    const char *env = getenv("PS5C_UE_SCALE");
    if (env && *env) percent = strtol(env, NULL, 10);
    /* no Unreal scale chosen: the internal resolution, as a share of the 4K output the
     * scale applies to (1080p -> 50%, 720p -> 33%) */
    if (percent == 0 && settings_internal_height()) percent = (settings_internal_height() * 100 + 1080) / 2160;
    char gen[200];
    snprintf(gen, sizeof(gen), "user/mods/%s/" GEN_DIR, title_id);
    remove_generated(gen);
    if (percent <= 0 || percent >= 100) return;

    /* the project folders beside engine/, with their paks in <project>/content/paks */
    scan projects = {NULL, {{0}}, 0};
    if (pal_dir_list(app_dir, scan_entry, &projects) != PS5C_OK) return;
    qsort(projects.found, projects.count, sizeof(projects.found[0]), cmp_names);
    char profile_name[512] = {0}, path[900];
    uint8_t *profile = NULL;
    size_t profile_size = 0;
    for (uint32_t i = 0; i < projects.count && !profile; i++) {
        if (!strcmp(projects.found[i], "engine") || !strcmp(projects.found[i], "Engine") || !strncmp(projects.found[i], "sce_", 4))
            continue;
        char paks_dir[600];
        snprintf(paks_dir, sizeof(paks_dir), "%s/%s/content/paks", app_dir, projects.found[i]);
        scan paks = {paks_dir, {{0}}, 0};
        if (pal_dir_list(paks_dir, scan_entry, &paks) != PS5C_OK) continue;
        qsort(paks.found, paks.count, sizeof(paks.found[0]), cmp_names);
        for (uint32_t k = 0; k < paks.count && !profile; k++) {
            snprintf(path, sizeof(path), "%s/%s", paks_dir, paks.found[k]);
            if (!pak_profile(path, profile_name, sizeof(profile_name), &profile, &profile_size)) profile = NULL;
        }
    }
    if (!profile) {
        LOG_W(CORE, "Unreal: [geral] escala_unreal=%lld, but no readable PS5 device profile in the game's paks "
                    "(not an Unreal game, or its paks are encrypted)", (long long)percent);
        return;
    }
    size_t cap = profile_size * 2 + 4096;
    char *ini = (char *)malloc(cap);
    size_t ini_size = ini ? ue_scale_profile((const char *)profile, profile_size, (int)percent, ini, cap) : 0;
    free(profile);
    size_t pak_size = 0;
    uint8_t *pak = ini_size ? ue_scale_pak(profile_name, ini, ini_size, &pak_size) : NULL;
    free(ini);
    if (!pak) return;
    char project[128];
    snprintf(project, sizeof(project), "%.*s", (int)strcspn(profile_name, "/"), profile_name);
    pal_mkdir("user");
    pal_mkdir("user/mods");
    snprintf(path, sizeof(path), "user/mods/%s", title_id);
    pal_mkdir(path);
    pal_mkdir(gen);
    snprintf(path, sizeof(path), "%s/%s", gen, project);
    pal_mkdir(path);
    snprintf(path, sizeof(path), "%s/%s/Saved", gen, project);
    pal_mkdir(path);
    snprintf(path, sizeof(path), "%s/%s/Saved/Paks", gen, project);
    pal_mkdir(path);
    snprintf(path, sizeof(path), "%s/%s/Saved/Paks/" GEN_PAK, gen, project);
    pal_file *f = NULL;
    bool ok = pal_file_open(path, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE, &f) == PS5C_OK &&
              pal_file_write(f, pak, pak_size, NULL) == PS5C_OK;
    if (f) pal_file_close(f);
    free(pak);
    if (ok) settings_internal_height_native();
    if (ok) LOG_I(CORE, "Unreal: internal resolution %lld%% (%s, %s)", (long long)percent, profile_name, path);
    else LOG_W(CORE, "Unreal: cannot write %s", path);
}
