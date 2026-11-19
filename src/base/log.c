/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "base/log.h"
#include "base/log_record.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG_LINE_MAX 2048

static const char *const k_cat_names[LOG_CAT_COUNT] = {
    "CORE", "CPU", "MEM", "LOADER", "HLE", "KERNEL", "GPU",
    "SHADER", "AUDIO", "VIDEO", "INPUT", "FS", "RECORDER",
};

static const char *const k_level_names[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "NONE"};

PS5C_STATIC_ASSERT(PS5C_ARRAY_COUNT(k_cat_names) == LOG_CAT_COUNT, "log category names");

static void default_sink(void *user, log_level level, const char *line, size_t len) {
    PS5C_UNUSED(user);
    PS5C_UNUSED(level);
    fwrite(line, 1, len, stderr);
    fflush(stderr);
}

/* Configuration is expected to be set during startup, before threads exist. */
static log_sink_fn g_sink = default_sink;
static void *g_sink_user;
static log_clock_fn g_clock;
static uint64_t g_clock_origin;
log_level g_log_levels[LOG_CAT_COUNT] = {
    LOG_LVL_INFO, LOG_LVL_INFO, LOG_LVL_INFO, LOG_LVL_INFO, LOG_LVL_INFO, LOG_LVL_INFO, LOG_LVL_INFO,
    LOG_LVL_INFO, LOG_LVL_INFO, LOG_LVL_INFO, LOG_LVL_INFO, LOG_LVL_INFO, LOG_LVL_INFO,
};

void log_set_sink(log_sink_fn sink, void *user) {
    g_sink = sink ? sink : default_sink;
    g_sink_user = sink ? user : NULL;
}

void log_set_clock(log_clock_fn now_ns) {
    g_clock = now_ns;
    g_clock_origin = now_ns ? now_ns() : 0;
}

void log_set_level(log_cat cat, log_level level) {
    if ((unsigned)cat < LOG_CAT_COUNT) {
        g_log_levels[cat] = level;
    }
}

void log_set_level_all(log_level level) {
    for (int i = 0; i < LOG_CAT_COUNT; i++) {
        g_log_levels[i] = level;
    }
}

const char *log_cat_name(log_cat cat) {
    return (unsigned)cat < LOG_CAT_COUNT ? k_cat_names[cat] : "?";
}

const char *log_level_name(log_level level) {
    return (unsigned)level <= LOG_LVL_NONE ? k_level_names[level] : "?";
}

void log_emit(log_cat cat, log_level level, const char *fmt, ...) {
    char line[LOG_LINE_MAX];
    int n;
    uint64_t us = 0;
    if (g_clock) {
        us = (g_clock() - g_clock_origin) / 1000u;
        n = snprintf(line, sizeof(line), "[%6u.%06u] [%-5s] [%s] ", (unsigned)(us / 1000000u),
                     (unsigned)(us % 1000000u), log_level_name(level), log_cat_name(cat));
    } else {
        n = snprintf(line, sizeof(line), "[%-5s] [%s] ", log_level_name(level), log_cat_name(cat));
    }
    if (n < 0) {
        return;
    }

    size_t len = (size_t)n;
    va_list ap;
    va_start(ap, fmt);
    if (log_record_enabled()) log_record_emit(cat, level, us, fmt, ap);
    int m = vsnprintf(line + len, sizeof(line) - len, fmt, ap);
    va_end(ap);
    if (m < 0) {
        m = 0;
    }
    len += (size_t)m;

    /* Leave room for the newline; mark truncated messages. */
    if (len > sizeof(line) - 2) {
        len = sizeof(line) - 2;
        memcpy(line + len - 3, "...", 3);
    }
    line[len++] = '\n';
    line[len] = '\0';
    g_sink(g_sink_user, level, line, len);
}
