/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stateless firmware libc operations. Guest addresses are translated before
 * use; returned pointers retain their guest address, including nonidentity
 * mappings. Guest long is 64 bits even on Windows (LLP64). */
#include "hle/hle.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void hle_register_table(const hle_export *, size_t);

/* Find a string's length without reading past mapped, readable memory. A
 * bounded search need not have a terminator. Scan VMAs rather than locking
 * the address space once for every byte of large strings. */
static bool string_length(hle_call *c, guest_addr a, uint64_t limit, uint64_t *out) {
    uint64_t n = 0;
    while (n < limit) {
        gmem_vma v;
        if (a > UINT64_MAX - n || !gmem_query_vma(c->mem, a + n, false, &v) || v.reserved ||
            !(v.prot & GPROT_READ))
            return false;
        uint64_t span = v.size - (a + n - v.start);
        if (span > limit - n)
            span = limit - n;
        const char *p = hle_mem(c, a + n, span);
        if (!p)
            return false;
        const char *end = memchr(p, 0, (size_t)span);
        if (end) {
            *out = n + (uint64_t)(end - p);
            return true;
        }
        n += span;
    }
    *out = n;
    return true;
}
static const char *string(hle_call *c, unsigned arg, uint64_t *n) {
    guest_addr a = hle_arg_ptr(c, arg);
    return string_length(c, a, UINT64_MAX, n) ? hle_mem(c, a, *n + 1) : NULL;
}
static void length_(hle_call *c) {
    uint64_t n = 0;
    string_length(c, hle_arg_ptr(c, 0), UINT64_MAX, &n);
    hle_ret_u64(c, n);
}
static void nlength_(hle_call *c) {
    uint64_t n = 0;
    string_length(c, hle_arg_ptr(c, 0), hle_arg(c, 1), &n);
    hle_ret_u64(c, n);
}
static void copy_memory(hle_call *c) {
    uint64_t n = hle_arg(c, 2);
    guest_addr d = hle_arg_ptr(c, 0);
    void *dst = n ? hle_mem(c, d, n) : NULL;
    const void *src = n ? hle_mem(c, hle_arg_ptr(c, 1), n) : NULL;
    if (n && (!src || !dst)) {
        hle_ret_u64(c, 0);
        return;
    }
    if (n)
        memmove(dst, src, (size_t)n);
    hle_ret_u64(c, d);
}
static void set_memory(hle_call *c) {
    uint64_t n = hle_arg(c, 2);
    guest_addr d = hle_arg_ptr(c, 0);
    void *dst = n ? hle_mem(c, d, n) : NULL;
    if (n && !dst) {
        hle_ret_u64(c, 0);
        return;
    }
    if (n)
        memset(dst, hle_arg_s32(c, 1), (size_t)n);
    hle_ret_u64(c, d);
}
static void compare_memory(hle_call *c) {
    uint64_t n = hle_arg(c, 2);
    const void *a = n ? hle_mem(c, hle_arg_ptr(c, 0), n) : NULL;
    const void *b = n ? hle_mem(c, hle_arg_ptr(c, 1), n) : NULL;
    hle_ret_s32(c, n && a && b ? memcmp(a, b, (size_t)n) : 0);
}
static void find_memory(hle_call *c) {
    uint64_t n = hle_arg(c, 2);
    guest_addr a = hle_arg_ptr(c, 0);
    const char *p = n ? hle_mem(c, a, n) : NULL;
    const char *found = p ? memchr(p, hle_arg_s32(c, 1), (size_t)n) : NULL;
    hle_ret_u64(c, found ? a + (uint64_t)(found - p) : 0);
}
static void copy_string(hle_call *c) {
    uint64_t n;
    const char *s = string(c, 1, &n);
    guest_addr a = hle_arg_ptr(c, 0);
    char *d = s ? hle_mem(c, a, n + 1) : NULL;
    if (d)
        memcpy(d, s, (size_t)n + 1);
    hle_ret_u64(c, d ? a : 0);
}
static void copy_nstring(hle_call *c) {
    uint64_t n = hle_arg(c, 2), len = 0;
    guest_addr a = hle_arg_ptr(c, 0);
    if (!n) {
        hle_ret_u64(c, a);
        return;
    }
    bool ok = string_length(c, hle_arg_ptr(c, 1), n, &len);
    const char *s = ok ? hle_mem(c, hle_arg_ptr(c, 1), len ? len : 1) : NULL;
    char *d = ok ? hle_mem(c, a, n) : NULL;
    if (s && d) {
        memcpy(d, s, (size_t)len);
        memset(d + len, 0, (size_t)(n - len));
    }
    hle_ret_u64(c, s && d ? a : 0);
}
static void concatenate(hle_call *c, bool bounded) {
    uint64_t dl, sl;
    guest_addr a = hle_arg_ptr(c, 0);
    const char *dst = string(c, 0, &dl);
    uint64_t max = bounded ? hle_arg(c, 2) : UINT64_MAX;
    bool ok = string_length(c, hle_arg_ptr(c, 1), max, &sl);
    const char *src = ok && sl ? hle_mem(c, hle_arg_ptr(c, 1), sl) : NULL;
    char *d = dst && ok && sl < UINT64_MAX - dl ? hle_mem(c, a, dl + sl + 1) : NULL;
    if (d && (!sl || src)) {
        if (sl)
            memcpy(d + dl, src, (size_t)sl);
        d[dl + sl] = 0;
    } else
        d = NULL;
    hle_ret_u64(c, d ? a : 0);
}
static void concat_(hle_call *c) {
    concatenate(c, false);
}
static void nconcat_(hle_call *c) {
    concatenate(c, true);
}
static void compare_string(hle_call *c, bool bounded, bool fold) {
    uint64_t max = bounded ? hle_arg(c, 2) : UINT64_MAX, na = 0, nb = 0;
    if (!max) {
        hle_ret_s32(c, 0);
        return;
    }
    bool ok = string_length(c, hle_arg_ptr(c, 0), max, &na) &&
              string_length(c, hle_arg_ptr(c, 1), max, &nb);
    const unsigned char *a = ok ? hle_mem(c, hle_arg_ptr(c, 0), na < max ? na + 1 : na) : NULL;
    const unsigned char *b = ok ? hle_mem(c, hle_arg_ptr(c, 1), nb < max ? nb + 1 : nb) : NULL;
    int result = 0;
    if (a && b)
        for (uint64_t i = 0; i < max; i++) {
            int x = a[i], y = b[i];
            if (fold) {
                x = tolower(x);
                y = tolower(y);
            }
            if (x != y || !x) {
                result = x - y;
                break;
            }
        }
    hle_ret_s32(c, result);
}
static void cmp_(hle_call *c) {
    compare_string(c, false, false);
}
static void ncmp_(hle_call *c) {
    compare_string(c, true, false);
}
static void icmp_(hle_call *c) {
    compare_string(c, false, true);
}
static void nicmp_(hle_call *c) {
    compare_string(c, true, true);
}
static void find_char(hle_call *c, bool reverse) {
    uint64_t n;
    const char *s = string(c, 0, &n);
    const char *p =
        s ? (reverse ? strrchr(s, hle_arg_s32(c, 1)) : strchr(s, hle_arg_s32(c, 1))) : NULL;
    hle_ret_u64(c, p ? hle_arg_ptr(c, 0) + (uint64_t)(p - s) : 0);
}
static void chr_(hle_call *c) {
    find_char(c, false);
}
static void rchr_(hle_call *c) {
    find_char(c, true);
}
static void find_string(hle_call *c) {
    uint64_t a, b;
    const char *s = string(c, 0, &a), *needle = string(c, 1, &b),
               *p = s && needle ? strstr(s, needle) : NULL;
    hle_ret_u64(c, p ? hle_arg_ptr(c, 0) + (uint64_t)(p - s) : 0);
}
static void span(hle_call *c, int mode) {
    uint64_t a, b;
    const char *s = string(c, 0, &a), *chars = string(c, 1, &b);
    uint64_t result = 0;
    if (s && chars) {
        if (mode == 2) {
            const char *p = strpbrk(s, chars);
            result = p ? hle_arg_ptr(c, 0) + (uint64_t)(p - s) : 0;
        } else
            result = mode ? strcspn(s, chars) : strspn(s, chars);
    }
    hle_ret_u64(c, result);
}
static void spn_(hle_call *c) {
    span(c, 0);
}
static void cspn_(hle_call *c) {
    span(c, 1);
}
static void pbrk_(hle_call *c) {
    span(c, 2);
}
static void integer(hle_call *c, bool unsig) {
    uint64_t n;
    const char *s = string(c, 0, &n);
    char *end = NULL;
    uint64_t result = 0;
    if (s)
        result = unsig ? strtoull(s, &end, hle_arg_s32(c, 2))
                       : (uint64_t)strtoll(s, &end, hle_arg_s32(c, 2));
    if (hle_arg_ptr(c, 1)) {
        uint64_t a = end ? hle_arg_ptr(c, 0) + (uint64_t)(end - s) : 0;
        gmem_write(c->mem, hle_arg_ptr(c, 1), &a, 8);
    }
    hle_ret_u64(c, result);
}
static void strtol_(hle_call *c) {
    integer(c, false);
}
static void strtoul_(hle_call *c) {
    integer(c, true);
}
static void atoi_(hle_call *c) {
    uint64_t n;
    const char *s = string(c, 0, &n);
    hle_ret_s32(c, s ? atoi(s) : 0);
}
static void floating(hle_call *c, bool endptr, bool single) {
    uint64_t n;
    const char *s = string(c, 0, &n);
    char *end = NULL;
    double v = s ? strtod(s, &end) : 0;
    if (endptr && hle_arg_ptr(c, 1)) {
        uint64_t a = end ? hle_arg_ptr(c, 0) + (uint64_t)(end - s) : 0;
        gmem_write(c->mem, hle_arg_ptr(c, 1), &a, 8);
    }
    if (single) {
        float f = (float)v;
        memcpy(&c->a->ret_xmm[0][0], &f, 4);
    } else
        memcpy(&c->a->ret_xmm[0][0], &v, 8);
}
static void atof_(hle_call *c) {
    floating(c, false, false);
}
static void strtod_(hle_call *c) {
    floating(c, true, false);
}
static void strtof_(hle_call *c) {
    floating(c, true, true);
}
static void lroundf_(hle_call *c) {
    float x;
    memcpy(&x, &c->a->xmm[0][0], 4);
    hle_ret_u64(c, (uint64_t)llroundf(x));
}
/* C locale: every single byte maps to its unsigned wchar_t value. The PS5
 * SDK uses 16-bit wchar_t (__SCE__ in freebsd/x86/_types.h). Do not use the
 * host wchar_t type: non-Windows builds can use a different width. */
static void mbtowc_(hle_call *c) {
    guest_addr src = hle_arg_ptr(c, 1);
    uint8_t byte;
    if (!src) {
        hle_ret_s32(c, 0);
        return;
    }
    if (!hle_arg(c, 2) || !gmem_read(c->mem, src, &byte, 1)) {
        hle_ret_s32(c, -1);
        return;
    }
    uint16_t wc = byte;
    if (hle_arg_ptr(c, 0) && !gmem_write(c->mem, hle_arg_ptr(c, 0), &wc, 2)) {
        hle_ret_s32(c, -1);
        return;
    }
    hle_ret_s32(c, byte ? 1 : 0);
}
static void wctob_(hle_call *c) {
    uint32_t v = hle_arg_u32(c, 0);
    hle_ret_s32(c, v <= 255 ? (int)v : -1);
}
static void wcslen_(hle_call *c) {
    uint64_t n = 0;
    uint16_t v;
    while (n < UINT64_MAX / 2 && gmem_read(c->mem, hle_arg_ptr(c, 0) + n * 2, &v, 2) && v)
        n++;
    hle_ret_u64(c, n);
}
#define CTYPE(name)                                                                                \
    static void name##_(hle_call *c) {                                                             \
        int v = hle_arg_s32(c, 0);                                                                 \
        hle_ret_s32(c, v == EOF ? name(EOF) : name((unsigned char)v));                             \
    }
/* Spell EOF locally: no host FILE ABI is exposed. */
#ifndef EOF
#define EOF (-1)
#endif
CTYPE(tolower)
CTYPE(toupper) CTYPE(isspace) CTYPE(isalnum) CTYPE(isalpha) CTYPE(isdigit) CTYPE(isxdigit)
#define SINCOS(name, type, sine, cosine)                                                            \
    static void name##_(hle_call *c) {                                                             \
        type x, s, co;                                                                            \
        memcpy(&x, &c->a->xmm[0][0], sizeof(x));                                                    \
        type *out_s = hle_mem(c, hle_arg_ptr(c, 0), sizeof(type));                                  \
        type *out_c = hle_mem(c, hle_arg_ptr(c, 1), sizeof(type));                                  \
        if (!out_s || !out_c) return;                                                             \
        s = sine(x); co = cosine(x);                                                               \
        memcpy(out_s, &s, sizeof(s)); memcpy(out_c, &co, sizeof(co));                               \
    }
SINCOS(sincos, double, sin, cos)
SINCOS(sincosf, float, sinf, cosf)
#undef SINCOS
#define MATH1(name, type)                                                                          \
    static void name##_(hle_call *c) {                                                             \
        type x, v;                                                                                 \
        memcpy(&x, &c->a->xmm[0][0], sizeof(x));                                                   \
        v = name(x);                                                                               \
        memcpy(&c->a->ret_xmm[0][0], &v, sizeof(v));                                               \
    }
#define MATH2(name, type)                                                                          \
    static void name##_(hle_call *c) {                                                             \
        type x, y, v;                                                                              \
        memcpy(&x, &c->a->xmm[0][0], sizeof(x));                                                   \
        memcpy(&y, &c->a->xmm[1][0], sizeof(y));                                                   \
        v = name(x, y);                                                                            \
        memcpy(&c->a->ret_xmm[0][0], &v, sizeof(v));                                               \
    }
    MATH1(sin, double) MATH1(cos, double) MATH1(tan, double) MATH1(acos, double) MATH1(log2, double)
        MATH1(log10, double) MATH1(exp2, double) MATH1(sinf, float) MATH1(cosf, float)
            MATH1(acosf, float) MATH1(logf, float) MATH1(log2f, float) MATH1(exp2f, float)
                MATH2(pow, double) MATH2(powf, float) MATH2(atan2, double)
#define E(name, fn) HLE_FUNC_ENTRY("libSceLibcInternal", name, fn)
                    static const hle_export exports[] = {
                        E("memcpy", copy_memory),
                        E("memmove", copy_memory),
                        E("memset", set_memory),
                        E("memcmp", compare_memory),
                        E("memchr", find_memory),
                        E("strlen", length_),
                        E("strnlen", nlength_),
                        E("strcpy", copy_string),
                        E("strncpy", copy_nstring),
                        E("strcat", concat_),
                        E("strncat", nconcat_),
                        E("strcmp", cmp_),
                        E("strncmp", ncmp_),
                        E("strcasecmp", icmp_),
                        E("strncasecmp", nicmp_),
                        E("strchr", chr_),
                        E("strrchr", rchr_),
                        E("strstr", find_string),
                        E("strspn", spn_),
                        E("strcspn", cspn_),
                        E("strpbrk", pbrk_),
                        E("strtol", strtol_),
                        E("strtoll", strtol_),
                        E("strtoul", strtoul_),
                        E("strtoull", strtoul_),
                        E("atoi", atoi_),
                        E("atof", atof_),
                        E("strtod", strtod_),
                        E("strtof", strtof_),
                        E("mbtowc", mbtowc_),
                        E("wctob", wctob_),
                        E("wcslen", wcslen_),
                        E("lroundf", lroundf_),
                        E("tolower", tolower_),
                        E("toupper", toupper_),
                        E("isspace", isspace_),
                        E("isalnum", isalnum_),
                        E("isalpha", isalpha_),
                        E("isdigit", isdigit_),
                        E("isxdigit", isxdigit_),
                        E("sin", sin_),
                        E("sincos", sincos_),
                        E("sincosf", sincosf_),
                        E("cos", cos_),
                        E("tan", tan_),
                        E("acos", acos_),
                        E("atan2", atan2_),
                        E("pow", pow_),
                        E("log2", log2_),
                        E("log10", log10_),
                        E("exp2", exp2_),
                        E("sinf", sinf_),
                        E("cosf", cosf_),
                        E("acosf", acosf_),
                        E("powf", powf_),
                        E("logf", logf_),
                        E("log2f", log2f_),
                        E("exp2f", exp2f_),
};
void hle_register_libc_memory(void) {
    hle_register_table(exports, PS5C_ARRAY_COUNT(exports));
}
