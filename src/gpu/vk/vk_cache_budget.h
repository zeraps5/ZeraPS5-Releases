/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_VK_CACHE_BUDGET_H
#define PS5C_VK_CACHE_BUDGET_H

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

/* Session options: reject overflow, signs and trailing text instead of turning
 * an invalid setting into a very large allocation. Zero is a valid budget. */
static inline uint64_t vk_cache_budget(const char *name, uint64_t fallback, uint64_t maximum) {
    const char *text = getenv(name);
    if (!text || *text < '0' || *text > '9') return fallback;
    char *end;
    errno = 0;
    unsigned long long n = strtoull(text, &end, 10);
    return errno || *end || n > maximum ? fallback : (uint64_t)n;
}

static inline uint64_t vk_cache_budget_mb(const char *name, uint64_t fallback) {
    return vk_cache_budget(name, fallback, 4096) << 20;
}
#endif
