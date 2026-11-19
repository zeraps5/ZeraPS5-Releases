/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minimal test harness. Each test binary defines a table of tests and calls
 * test_run_all() from main. */
#ifndef PS5C_TEST_H
#define PS5C_TEST_H

#include <stdio.h>
#include <string.h>

typedef void (*test_fn)(void);

typedef struct test_case {
    const char *name;
    test_fn fn;
} test_case;

#define TEST_CASE(fn) {#fn, fn}

static int g_test_failed;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                             \
            g_test_failed = 1;                                                                     \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define CHECK_EQ_U64(a, b)                                                                         \
    do {                                                                                           \
        unsigned long long va_ = (unsigned long long)(a), vb_ = (unsigned long long)(b);           \
        if (va_ != vb_) {                                                                          \
            printf("    FAIL %s:%d: %s == %s (0x%llx != 0x%llx)\n", __FILE__, __LINE__, #a, #b,    \
                   va_, vb_);                                                                      \
            g_test_failed = 1;                                                                     \
            return;                                                                                \
        }                                                                                          \
    } while (0)

static int test_run_all(const char *suite, const test_case *tests, size_t count) {
    int failures = 0;
    printf("[%s] running %u tests\n", suite, (unsigned)count);
    for (size_t i = 0; i < count; i++) {
        g_test_failed = 0;
        tests[i].fn();
        printf("  %-40s %s\n", tests[i].name, g_test_failed ? "FAILED" : "ok");
        failures += g_test_failed;
    }
    printf("[%s] %d failed, %d passed\n", suite, failures, (int)count - failures);
    return failures == 0 ? 0 : 1;
}

#endif
