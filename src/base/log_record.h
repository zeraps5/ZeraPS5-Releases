/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_LOG_RECORD_H
#define PS5C_LOG_RECORD_H
#include "base/log.h"
#include <stdarg.h>
#include <stdio.h>

/* Optional launcher sidecar. Original diagnostic output is never translated.
 * Configuration changes must happen before worker threads start. */
#define LOG_RECORD_MAX 16384
#define LOG_RECORD_FIELDS 64
typedef void (*log_record_sink_fn)(void *user, const void *record, size_t size);
void log_set_record_sink(log_record_sink_fn sink, void *user);
/* Startup-only; reports serialization drops without logging private values. */
typedef void (*log_record_reject_fn)(void *user);
void log_set_record_reject_sink(log_record_reject_fn sink, void *user);
void log_record_emit(log_cat cat, log_level level, uint64_t us, const char *fmt, va_list args);
bool log_record_enabled(void);

/* Return the byte length of one supported printf conversion, or zero.
 * Positional arguments and %n are deliberately unsupported. */
size_t log_format_spec(const char *text);
typedef struct log_record {
    log_cat category;
    log_level level;
    uint64_t us;
    const char *format;
    const char *fields[LOG_RECORD_FIELDS];
    unsigned count;
    char storage[LOG_RECORD_MAX];
} log_record;
/* 1: record, 0: clean EOF (including an empty file), -1: damaged/truncated,
 * -2: legacy size-limit marker. */
int log_record_read(FILE *file, log_record *record);
/* Reconstruct without interpreting translations as a printf format. */
bool log_record_render(const log_record *record, const char *translation, char *out, size_t cap);
#endif
