/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "base/json.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void put(json_writer *w, const char *s, size_t n) {
    if (w->failed || !dyn_reserve(&w->buf, w->buf.len + n + 1)) {
        w->failed = true;
        return;
    }
    memcpy(w->buf.data + w->buf.len, s, n);
    w->buf.len += n;
    w->buf.data[w->buf.len] = '\0';
}

static void puts_(json_writer *w, const char *s) { put(w, s, strlen(s)); }

static void newline_indent(json_writer *w) {
    if (!w->pretty) {
        return;
    }
    put(w, "\n", 1);
    for (int i = 0; i < w->depth; i++) {
        put(w, "  ", 2);
    }
}

/* Separator logic shared by every value and key. */
static void before_value(json_writer *w) {
    if (w->after_key) {
        w->after_key = false;
        return;
    }
    if (w->depth > 0) {
        if (!w->first[w->depth]) {
            put(w, ",", 1);
        }
        w->first[w->depth] = false;
        newline_indent(w);
    }
}

void json_init(json_writer *w, bool pretty) {
    memset(w, 0, sizeof(*w));
    w->pretty = pretty;
}

void json_free(json_writer *w) { dyn_free(&w->buf); }

const char *json_result(json_writer *w, size_t *len_out) {
    if (w->failed || w->depth != 0 || !w->buf.data) {
        return NULL;
    }
    if (len_out) {
        *len_out = w->buf.len;
    }
    return w->buf.data;
}

static void open_scope(json_writer *w, const char *c) {
    before_value(w);
    puts_(w, c);
    if (w->depth + 1 >= JSON_MAX_DEPTH) {
        w->failed = true;
        return;
    }
    w->depth++;
    w->first[w->depth] = true;
}

static void close_scope(json_writer *w, const char *c) {
    if (w->depth == 0 || w->after_key) {
        w->failed = true;
        return;
    }
    bool empty = w->first[w->depth];
    w->depth--;
    if (!empty) {
        newline_indent(w);
    }
    puts_(w, c);
}

void json_begin_object(json_writer *w) { open_scope(w, "{"); }
void json_end_object(json_writer *w) { close_scope(w, "}"); }
void json_begin_array(json_writer *w) { open_scope(w, "["); }
void json_end_array(json_writer *w) { close_scope(w, "]"); }

static void write_escaped(json_writer *w, const char *s) {
    put(w, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char esc[8];
        switch (*p) {
        case '"': put(w, "\\\"", 2); break;
        case '\\': put(w, "\\\\", 2); break;
        case '\n': put(w, "\\n", 2); break;
        case '\r': put(w, "\\r", 2); break;
        case '\t': put(w, "\\t", 2); break;
        default:
            if (*p < 0x20) {
                snprintf(esc, sizeof(esc), "\\u%04x", *p);
                puts_(w, esc);
            } else {
                put(w, (const char *)p, 1); /* UTF-8 bytes pass through */
            }
        }
    }
    put(w, "\"", 1);
}

void json_key(json_writer *w, const char *key) {
    before_value(w);
    write_escaped(w, key);
    put(w, w->pretty ? ": " : ":", w->pretty ? 2 : 1);
    w->after_key = true;
}

void json_string(json_writer *w, const char *s) {
    if (!s) {
        json_null(w);
        return;
    }
    before_value(w);
    write_escaped(w, s);
}

void json_u64(json_writer *w, uint64_t v) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%" PRIu64, v);
    before_value(w);
    puts_(w, tmp);
}

void json_i64(json_writer *w, int64_t v) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%" PRId64, v);
    before_value(w);
    puts_(w, tmp);
}

void json_bool(json_writer *w, bool v) {
    before_value(w);
    puts_(w, v ? "true" : "false");
}

void json_null(json_writer *w) {
    before_value(w);
    puts_(w, "null");
}

void json_hex(json_writer *w, uint64_t v) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "0x%" PRIx64, v);
    json_string(w, tmp);
}

void json_kv_string(json_writer *w, const char *key, const char *s) {
    json_key(w, key);
    json_string(w, s);
}

void json_kv_u64(json_writer *w, const char *key, uint64_t v) {
    json_key(w, key);
    json_u64(w, v);
}

void json_kv_bool(json_writer *w, const char *key, bool v) {
    json_key(w, key);
    json_bool(w, v);
}

void json_kv_hex(json_writer *w, const char *key, uint64_t v) {
    json_key(w, key);
    json_hex(w, v);
}
