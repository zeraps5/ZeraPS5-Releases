/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Streaming JSON writer into a growable buffer. Output is deterministic
 * (callers emit keys in a fixed order) so profiles can be diffed. */
#ifndef PS5C_JSON_H
#define PS5C_JSON_H

#include "base/base.h"
#include "base/dynarray.h"

#define JSON_MAX_DEPTH 32

typedef struct json_writer {
    DYNARRAY(char) buf;
    int depth;
    bool first[JSON_MAX_DEPTH]; /* no value written yet at this depth */
    bool after_key;
    bool pretty;
    bool failed; /* allocation failure or misuse; checked once at the end */
} json_writer;

void json_init(json_writer *w, bool pretty);
void json_free(json_writer *w);
/* Returns the NUL-terminated document, or NULL if writing failed. */
const char *json_result(json_writer *w, size_t *len_out);

void json_begin_object(json_writer *w);
void json_end_object(json_writer *w);
void json_begin_array(json_writer *w);
void json_end_array(json_writer *w);
void json_key(json_writer *w, const char *key);

void json_string(json_writer *w, const char *s); /* NULL writes null */
void json_u64(json_writer *w, uint64_t v);
void json_i64(json_writer *w, int64_t v);
void json_bool(json_writer *w, bool v);
void json_null(json_writer *w);
void json_hex(json_writer *w, uint64_t v); /* "0x..." string */

/* key + value shorthands */
void json_kv_string(json_writer *w, const char *key, const char *s);
void json_kv_u64(json_writer *w, const char *key, uint64_t v);
void json_kv_bool(json_writer *w, const char *key, bool v);
void json_kv_hex(json_writer *w, const char *key, uint64_t v);

#endif
