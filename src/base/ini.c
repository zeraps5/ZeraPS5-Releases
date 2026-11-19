/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "base/ini.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Each line is kept as written; section and key lines are also parsed. */
typedef struct ini_line {
    char *text;
    char *section; /* the section this line belongs to ("" before any header) */
    char *key;     /* key lines: name and value, trimmed */
    char *value;
    bool header;
} ini_line;

struct ini {
    ini_line *lines;
    uint32_t count, cap;
};

static bool same(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    }
    return *a == *b;
}

static char *dup_range(const char *s, size_t n) {
    char *d = (char *)malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = 0;
    }
    return d;
}

static void trim(const char **s, size_t *n) {
    while (*n && isspace((unsigned char)**s)) (*s)++, (*n)--;
    while (*n && isspace((unsigned char)(*s)[*n - 1])) (*n)--;
}

static void free_line(ini_line *l) {
    free(l->text);
    free(l->section);
    free(l->key);
    free(l->value);
}

/* Parses `text` into a line at `at` (moving the rest down); `section` is the enclosing one. */
static bool insert_line(ini *f, uint32_t at, const char *text, size_t len, const char *section) {
    if (f->count == f->cap) {
        uint32_t cap = f->cap ? f->cap * 2 : 32;
        ini_line *n = (ini_line *)realloc(f->lines, cap * sizeof(ini_line));
        if (!n) return false;
        f->lines = n;
        f->cap = cap;
    }
    ini_line l;
    memset(&l, 0, sizeof(l));
    l.text = dup_range(text, len);
    const char *s = text;
    size_t n = len;
    trim(&s, &n);
    if (n >= 2 && s[0] == '[' && s[n - 1] == ']') {
        const char *name = s + 1;
        size_t nn = n - 2;
        trim(&name, &nn);
        l.header = true;
        l.section = dup_range(name, nn);
    } else {
        l.section = dup_range(section, strlen(section));
        const char *eq = n && s[0] != ';' && s[0] != '#' ? memchr(s, '=', n) : NULL;
        if (eq) {
            const char *k = s, *v = eq + 1;
            size_t kn = (size_t)(eq - s), vn = n - kn - 1;
            trim(&k, &kn);
            trim(&v, &vn);
            if (kn) {
                l.key = dup_range(k, kn);
                l.value = dup_range(v, vn);
            }
        }
    }
    if (!l.text || !l.section || (l.key && !l.value)) {
        free_line(&l);
        return false;
    }
    memmove(&f->lines[at + 1], &f->lines[at], (f->count - at) * sizeof(ini_line));
    f->lines[at] = l;
    f->count++;
    return true;
}

ini *ini_parse(const char *text, size_t len) {
    ini *f = (ini *)calloc(1, sizeof(ini));
    if (!f) return NULL;
    if (len >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) text += 3, len -= 3;
    const char *section = "";
    size_t pos = 0;
    while (pos < len) {
        size_t end = pos;
        while (end < len && text[end] != '\n') end++;
        size_t n = end - pos;
        if (n && text[pos + n - 1] == '\r') n--;
        if (!insert_line(f, f->count, text + pos, n, section)) {
            ini_free(f);
            return NULL;
        }
        if (f->lines[f->count - 1].header) section = f->lines[f->count - 1].section;
        pos = end + 1;
    }
    return f;
}

void ini_free(ini *f) {
    if (!f) return;
    for (uint32_t i = 0; i < f->count; i++) free_line(&f->lines[i]);
    free(f->lines);
    free(f);
}

char *ini_text(const ini *f, size_t *len_out) {
    size_t len = 0;
    for (uint32_t i = 0; i < f->count; i++) len += strlen(f->lines[i].text) + 2;
    char *out = (char *)malloc(len + 1), *p = out;
    if (!out) return NULL;
    for (uint32_t i = 0; i < f->count; i++) {
        size_t n = strlen(f->lines[i].text);
        memcpy(p, f->lines[i].text, n);
        p += n;
        *p++ = '\r';
        *p++ = '\n';
    }
    *p = 0;
    if (len_out) *len_out = len;
    return out;
}

static int find_key(const ini *f, const char *section, const char *key) {
    for (uint32_t i = 0; i < f->count; i++) {
        const ini_line *l = &f->lines[i];
        if (l->key && same(l->section, section) && same(l->key, key)) return (int)i;
    }
    return -1;
}

const char *ini_get(const ini *f, const char *section, const char *key) {
    int i = f ? find_key(f, section, key) : -1;
    return i < 0 ? NULL : f->lines[i].value;
}

int64_t ini_get_int(const ini *f, const char *section, const char *key, int64_t def) {
    const char *v = ini_get(f, section, key);
    if (!v || !*v) return def;
    char *end;
    long long n = strtoll(v, &end, 0);
    return end == v ? def : (int64_t)n;
}

bool ini_set(ini *f, const char *section, const char *key, const char *value) {
    int i = find_key(f, section, key);
    if (!value) {
        if (i >= 0) {
            free_line(&f->lines[i]);
            memmove(&f->lines[i], &f->lines[i + 1], (f->count - (uint32_t)i - 1) * sizeof(ini_line));
            f->count--;
        }
        return true;
    }
    size_t n = strlen(key) + strlen(value) + 2;
    char *text = (char *)malloc(n);
    if (!text) return false;
    snprintf(text, n, "%s=%s", key, value);
    bool ok;
    if (i >= 0) {
        free_line(&f->lines[i]);
        memmove(&f->lines[i], &f->lines[i + 1], (f->count - (uint32_t)i - 1) * sizeof(ini_line));
        f->count--;
        ok = insert_line(f, (uint32_t)i, text, strlen(text), section);
    } else {
        /* after the section's last key line, or a new section at the end */
        int last = -1, header = -1;
        for (uint32_t k = 0; k < f->count; k++) {
            if (!same(f->lines[k].section, section)) continue;
            if (f->lines[k].header) header = (int)k;
            if (f->lines[k].key || f->lines[k].header) last = (int)k;
        }
        if (header < 0 && *section) {
            char *h = (char *)malloc(strlen(section) + 3);
            ok = h != NULL;
            if (ok) {
                sprintf(h, "[%s]", section);
                if (f->count && f->lines[f->count - 1].text[0]) ok = insert_line(f, f->count, "", 0, section);
                ok = ok && insert_line(f, f->count, h, strlen(h), section);
                free(h);
            }
            ok = ok && insert_line(f, f->count, text, strlen(text), section);
        } else {
            ok = insert_line(f, (uint32_t)(last + 1), text, strlen(text), section);
        }
    }
    free(text);
    return ok;
}

uint32_t ini_keys(const ini *f, const char *section, const char **keys, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t i = 0; f && i < f->count; i++) {
        const ini_line *l = &f->lines[i];
        if (!l->key || !same(l->section, section)) continue;
        if (n < max) keys[n] = l->key;
        n++;
    }
    return n;
}
