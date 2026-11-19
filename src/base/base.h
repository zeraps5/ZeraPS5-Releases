/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Common definitions for the whole core. Portable C11 only: no OS headers,
 * no compiler extensions. */
#ifndef PS5C_BASE_H
#define PS5C_BASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* getenv walks the whole environment (~2.4 us with the Windows CRT) and debugging switches are read on per-draw
 * paths: every lookup in the core goes through a cache instead. The emulator never changes its environment; code
 * that does (tests) calls ps5c_env_reset() after the change. stdlib.h comes first, so it is not declared again. */
char *ps5c_getenv(const char *name);
/* Test-only reset at a quiescent point: no concurrent lookups, and no borrowed
 * values may be used afterwards. This invalidates all previously returned pointers. */
void ps5c_env_reset(void);
#define getenv(name) ps5c_getenv(name)

/* ---- Error codes (host side). Guest SCE error codes (0x8002xxxx) are a
 * separate space and must never be mixed with these. ---- */
typedef int32_t ps5c_err;

enum {
    PS5C_OK = 0,
    PS5C_ERR_NOMEM = -1,
    PS5C_ERR_INVALID = -2,
    PS5C_ERR_IO = -3,
    PS5C_ERR_NOT_FOUND = -4,
    PS5C_ERR_UNSUPPORTED = -5,
    PS5C_ERR_OS = -6,
    PS5C_ERR_BUSY = -7,
    PS5C_ERR_TIMEOUT = -8,
    PS5C_ERR_OUT_OF_RANGE = -9,
};

const char *ps5c_err_str(ps5c_err err);

/* ---- Utility macros ---- */
#define PS5C_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#define PS5C_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define PS5C_MIN(a, b) ((a) < (b) ? (a) : (b))
#define PS5C_MAX(a, b) ((a) > (b) ? (a) : (b))

/* `align` must be a power of two. */
#define PS5C_ALIGN_UP(x, align) (((x) + ((align) - 1)) & ~((uint64_t)(align) - 1))
#define PS5C_ALIGN_DOWN(x, align) ((x) & ~((uint64_t)(align) - 1))
#define PS5C_IS_POW2(x) ((x) != 0 && (((x) & ((x) - 1)) == 0))

#define PS5C_KiB(x) ((uint64_t)(x) << 10)
#define PS5C_MiB(x) ((uint64_t)(x) << 20)
#define PS5C_GiB(x) ((uint64_t)(x) << 30)

#define PS5C_UNUSED(x) ((void)(x))

#endif
