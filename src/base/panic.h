/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_PANIC_H
#define PS5C_PANIC_H

#include "base/base.h"

#if defined(__GNUC__) || defined(__clang__)
#define PS5C_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define PS5C_NORETURN __declspec(noreturn)
#else
#define PS5C_NORETURN
#endif

/* Logs a fatal message and aborts. Used for broken invariants, never for
 * conditions caused by guest input (those return errors). */
PS5C_NORETURN void ps5c_panic(const char *file, int line, const char *msg);

/* Always checked, in every build type. */
#define PS5C_CHECK(cond)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ps5c_panic(__FILE__, __LINE__, "check failed: " #cond);                                \
        }                                                                                          \
    } while (0)

/* Checked only in debug builds. */
#ifdef NDEBUG
#define PS5C_ASSERT(cond) ((void)0)
#else
#define PS5C_ASSERT(cond) PS5C_CHECK(cond)
#endif

#define PS5C_UNREACHABLE() ps5c_panic(__FILE__, __LINE__, "unreachable")

#endif
