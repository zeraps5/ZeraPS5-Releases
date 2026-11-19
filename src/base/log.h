/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Categorized logging. Calls below PS5C_LOG_MIN_LEVEL are removed at compile
 * time; the rest are filtered at runtime per category. Output goes to a sink
 * (stderr by default; the frontend installs the PAL sink). */
#ifndef PS5C_LOG_H
#define PS5C_LOG_H

#include "base/base.h"

typedef enum {
    LOG_CAT_CORE,
    LOG_CAT_CPU,
    LOG_CAT_MEM,
    LOG_CAT_LOADER,
    LOG_CAT_HLE,
    LOG_CAT_KERNEL,
    LOG_CAT_GPU,
    LOG_CAT_SHADER,
    LOG_CAT_AUDIO,
    LOG_CAT_VIDEO,
    LOG_CAT_INPUT,
    LOG_CAT_FS,
    LOG_CAT_RECORDER,
    LOG_CAT_COUNT
} log_cat;

typedef enum {
    LOG_LVL_TRACE = 0,
    LOG_LVL_DEBUG = 1,
    LOG_LVL_INFO = 2,
    LOG_LVL_WARN = 3,
    LOG_LVL_ERROR = 4,
    LOG_LVL_FATAL = 5,
    LOG_LVL_NONE = 6
} log_level;

#ifndef PS5C_LOG_MIN_LEVEL
#define PS5C_LOG_MIN_LEVEL 0
#endif

/* The sink receives one complete, newline-terminated line per call. */
typedef void (*log_sink_fn)(void *user, log_level level, const char *line, size_t len);
typedef uint64_t (*log_clock_fn)(void);

void log_set_sink(log_sink_fn sink, void *user);
void log_set_clock(log_clock_fn now_ns);
void log_set_level(log_cat cat, log_level level);
void log_set_level_all(log_level level);
extern log_level g_log_levels[LOG_CAT_COUNT];
static inline bool log_enabled(log_cat cat, log_level level) {
    return (unsigned)cat < LOG_CAT_COUNT && level >= g_log_levels[cat] && level < LOG_LVL_NONE;
}
const char *log_cat_name(log_cat cat);
const char *log_level_name(log_level level);

#if defined(__MINGW32__)
/* MinGW's `printf` archetype means the legacy msvcrt rules (no %zu); the UCRT
 * runtime we link against implements C99 formats. */
#define PS5C_PRINTF_FMT(fmt_idx, args_idx) __attribute__((format(gnu_printf, fmt_idx, args_idx)))
#elif defined(__GNUC__) || defined(__clang__)
#define PS5C_PRINTF_FMT(fmt_idx, args_idx) __attribute__((format(printf, fmt_idx, args_idx)))
#else
#define PS5C_PRINTF_FMT(fmt_idx, args_idx)
#endif

void log_emit(log_cat cat, log_level level, const char *fmt, ...) PS5C_PRINTF_FMT(3, 4);

#define PS5C_LOG(cat, level, ...)                                                                  \
    do {                                                                                           \
        if ((level) >= PS5C_LOG_MIN_LEVEL && log_enabled((cat), (level))) {                        \
            log_emit((cat), (level), __VA_ARGS__);                                                 \
        }                                                                                          \
    } while (0)

#define LOG_T(cat, ...) PS5C_LOG(LOG_CAT_##cat, LOG_LVL_TRACE, __VA_ARGS__)
#define LOG_D(cat, ...) PS5C_LOG(LOG_CAT_##cat, LOG_LVL_DEBUG, __VA_ARGS__)
#define LOG_I(cat, ...) PS5C_LOG(LOG_CAT_##cat, LOG_LVL_INFO, __VA_ARGS__)
#define LOG_W(cat, ...) PS5C_LOG(LOG_CAT_##cat, LOG_LVL_WARN, __VA_ARGS__)
#define LOG_E(cat, ...) PS5C_LOG(LOG_CAT_##cat, LOG_LVL_ERROR, __VA_ARGS__)

#endif
