/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "base/log_record.h"
#include <string.h>

static log_record_sink_fn record_sink;
static void *record_user;
static log_record_reject_fn reject_sink;
static void *reject_user;
void log_set_record_reject_sink(log_record_reject_fn sink, void *user) { reject_sink = sink; reject_user = user; }
static void rejected(void) { if (reject_sink) reject_sink(reject_user); }
void log_set_record_sink(log_record_sink_fn sink, void *user) { record_sink = sink; record_user = user; }
bool log_record_enabled(void) { return record_sink != NULL; }

size_t log_format_spec(const char *s) {
    if (*s != '%') return 0;
    const char *p = s + 1;
    if (*p == '%') return 2;
    while (*p && strchr("-+ #0", *p)) p++;
    if (*p == '*') p++;
    else while (*p >= '0' && *p <= '9') p++;
    if (*p == '.') {
        p++;
        if (*p == '*') p++;
        else while (*p >= '0' && *p <= '9') p++;
    }
    if ((*p == 'h' && p[1] == 'h') || (*p == 'l' && p[1] == 'l')) p += 2;
    else if (*p && strchr("hlLzjt", *p)) p++;
    return *p && strchr("diuoxXfFeEgGaAcsp", *p) ? (size_t)(p + 1 - s) : 0;
}
static void put16(char *p, unsigned v) { p[0] = (char)v; p[1] = (char)(v >> 8); }
static unsigned get16(const char *p) { return (unsigned char)p[0] | ((unsigned)(unsigned char)p[1] << 8); }

void log_record_emit(log_cat cat, log_level level, uint64_t us, const char *fmt, va_list args) {
    if (!record_sink) return;
    if ((unsigned)cat >= LOG_CAT_COUNT || (unsigned)level >= LOG_LVL_NONE) { rejected(); return; }
    /* Keep crash logging off large stacks. Each emitting thread owns scratch. */
    static _Thread_local char block[LOG_RECORD_MAX], conversions[4096];
    size_t n = 0, fmt_len = strlen(fmt) + 1;
    unsigned count = 0;
    for (const char *p = fmt; *p;) {
        if (*p != '%') { p++; continue; }
        size_t len = log_format_spec(p);
        if (!len) { rejected(); return; }
        if (p[1] != '%') {
            if (++count > LOG_RECORD_FIELDS || n + len + 2 >= sizeof(conversions)) { rejected(); return; }
            memcpy(conversions + n, p, len); n += len;
            conversions[n++] = '\x1f';
        }
        p += len;
    }
    conversions[n] = 0;
    if (fmt_len + 20 >= sizeof(block)) { rejected(); return; }
    size_t offset = 20 + fmt_len;
    va_list copy;
    va_copy(copy, args);
    int written = vsnprintf(block + offset, sizeof(block) - offset, conversions, copy);
    va_end(copy);
    if (written < 0 || (size_t)written >= sizeof(block) - offset) { rejected(); return; }
    /* Delimiters inside a value are ambiguous; never produce a wrong record. */
    unsigned delimiters = 0;
    for (int i = 0; i < written; i++) {
        if (!block[offset + (size_t)i]) { rejected(); return; } /* embedded NUL from %c */
        if (block[offset + (size_t)i] == '\x1f') delimiters++;
    }
    if (delimiters != count) { rejected(); return; }
    memcpy(block, "ZLR1", 4);
    put16(block + 4, (unsigned)fmt_len);
    put16(block + 6, (unsigned)written + 1);
    block[8] = (char)cat; block[9] = (char)level;
    put16(block + 10, count);
    for (unsigned i = 0; i < 8; i++) block[12 + i] = (char)(us >> (i * 8));
    memcpy(block + 20, fmt, fmt_len);
    record_sink(record_user, block, offset + (size_t)written + 1);
}

int log_record_read(FILE *f, log_record *r) {
    char header[20];
    size_t n = fread(header, 1, sizeof(header), f);
    if (!n) return ferror(f) ? -1 : 0;
    if (n == sizeof(header) && !memcmp(header, "ZLC1", 4)) return -2;
    if (n != sizeof(header) || memcmp(header, "ZLR1", 4)) return -1;
    unsigned fn = get16(header + 4), an = get16(header + 6), count = get16(header + 10);
    if (!fn || !an || fn + an > sizeof(r->storage) || count > LOG_RECORD_FIELDS ||
        (unsigned char)header[8] >= LOG_CAT_COUNT || (unsigned char)header[9] >= LOG_LVL_NONE) return -1;
    if (fread(r->storage, 1, fn + an, f) != fn + an || r->storage[fn - 1] || r->storage[fn + an - 1]) return -1;
    if (strlen(r->storage) != fn - 1 || strlen(r->storage + fn) != an - 1) return -1;
    r->category = (log_cat)(unsigned char)header[8]; r->level = (log_level)(unsigned char)header[9];
    r->us = 0; for (unsigned i = 0; i < 8; i++) r->us |= (uint64_t)(unsigned char)header[12 + i] << (8 * i);
    r->format = r->storage; r->count = 0;
    char *p = r->storage + fn, *start = p;
    for (; *p; p++) if (*p == '\x1f') {
        if (r->count >= count) return -1;
        *p = 0; r->fields[r->count++] = start; start = p + 1;
    }
    if (r->count != count || *start) return -1;
    return 1;
}
bool log_record_render(const log_record *r, const char *translation, char *out, size_t cap) {
    size_t used = 0;
    unsigned arg = 0;
    const char *original = r->format;
    for (const char *p = translation; *p;) {
        const char *value = p; size_t len = 1;
        if (*p == '%') {
            size_t spec = log_format_spec(p);
            if (!spec) return false;
            if (p[1] == '%') { value = "%"; len = 1; }
            else {
                while (*original) {
                    if (*original != '%') { original++; continue; }
                    size_t os = log_format_spec(original);
                    if (!os) return false;
                    if (original[1] == '%') { original += os; continue; }
                    break;
                }
                if (arg >= r->count || log_format_spec(original) != spec || strncmp(original, p, spec)) return false;
                original += spec;
                value = r->fields[arg++]; len = strlen(value);
            }
            p += spec;
        } else p++;
        if (used + len >= cap) return false;
        memcpy(out + used, value, len); used += len;
    }
    if (arg != r->count || !cap) return false;
    out[used] = 0;
    return true;
}
