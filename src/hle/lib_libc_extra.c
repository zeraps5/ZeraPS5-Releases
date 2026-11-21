/* SPDX-License-Identifier: GPL-2.0-or-later */
/* More stateless libc functions of libSceLibcInternal: the math, wide
 * character, multibyte and string functions PS2 games wrapped for PS5 import
 * (Burnout Revenge PPSA92006, Prince of Persia PPSA99024) and other games did
 * not. Guest wchar_t is 16 bits, long 64 bits; the multibyte encoding is UTF-8. */
#include "hle/hle.h"
#include "pal/pal.h"
#include <math.h>
#include <string.h>
#include <wctype.h>

void hle_register_table(const hle_export *, size_t);

static double xd(hle_call *c, int i) { double v; memcpy(&v, &c->a->xmm[i][0], 8); return v; }
static float xf(hle_call *c, int i) { float v; memcpy(&v, &c->a->xmm[i][0], 4); return v; }
static void rd(hle_call *c, double v) { memcpy(&c->a->ret_xmm[0][0], &v, 8); }
static void rf(hle_call *c, float v) { memcpy(&c->a->ret_xmm[0][0], &v, 4); }

#define D1(name) static void name##_(hle_call *c) { rd(c, name(xd(c, 0))); }
#define F1(name) static void name##_(hle_call *c) { rf(c, name(xf(c, 0))); }
#define D2(name) static void name##_(hle_call *c) { rd(c, name(xd(c, 0), xd(c, 1))); }
#define F2(name) static void name##_(hle_call *c) { rf(c, name(xf(c, 0), xf(c, 1))); }
D1(asin) D1(atan) D1(log) D1(sqrt) D1(cbrt) D1(cosh) D1(sinh) D1(tanh) D1(fabs) D1(asinh) D1(acosh) D1(atanh)
D1(expm1) D1(log1p) D1(trunc) D1(floor) D1(ceil) D1(nearbyint) D1(rint)
F1(asinf) F1(atanf) F1(log10f) F1(tanf) F1(sqrtf) F1(sinhf) F1(coshf) F1(tanhf) F1(fabsf) F1(floorf) F1(ceilf)
F1(truncf) F1(roundf) F1(cbrtf) F1(log1pf) F1(expm1f) F1(rintf)
D2(fmod) D2(hypot) D2(fmin) D2(fmax) D2(copysign) D2(remainder)
F2(atan2f) F2(fmodf) F2(hypotf) F2(fminf) F2(fmaxf) F2(copysignf)
#undef D1
#undef F1
#undef D2
#undef F2

static void ldexpf_(hle_call *c) { rf(c, ldexpf(xf(c, 0), hle_arg_s32(c, 0))); }
static void scalbn_(hle_call *c) { rd(c, scalbn(xd(c, 0), hle_arg_s32(c, 0))); }
static void scalbnf_(hle_call *c) { rf(c, scalbnf(xf(c, 0), hle_arg_s32(c, 0))); }
static void frexpf_(hle_call *c) {
    int e = 0;
    float v = frexpf(xf(c, 0), &e);
    int32_t e32 = e;
    if (hle_arg_ptr(c, 0)) gmem_write(c->mem, hle_arg_ptr(c, 0), &e32, 4);
    rf(c, v);
}
static void modf_(hle_call *c) {
    double ip = 0, v = modf(xd(c, 0), &ip);
    if (hle_arg_ptr(c, 0)) gmem_write(c->mem, hle_arg_ptr(c, 0), &ip, 8);
    rd(c, v);
}
static void modff_(hle_call *c) {
    float ip = 0, v = modff(xf(c, 0), &ip);
    if (hle_arg_ptr(c, 0)) gmem_write(c->mem, hle_arg_ptr(c, 0), &ip, 4);
    rf(c, v);
}
static void lrint_(hle_call *c) { hle_ret_u64(c, (uint64_t)llrint(xd(c, 0))); }
static void lrintf_(hle_call *c) { hle_ret_u64(c, (uint64_t)llrintf(xf(c, 0))); }
static void isinf_(hle_call *c) { hle_ret_s32(c, isinf(xd(c, 0)) ? (xd(c, 0) > 0 ? 1 : -1) : 0); }
static void isinff_(hle_call *c) { hle_ret_s32(c, isinf(xf(c, 0)) ? (xf(c, 0) > 0 ? 1 : -1) : 0); }
static void isnan_(hle_call *c) { hle_ret_s32(c, isnan(xd(c, 0)) != 0); }
static void isnanf_(hle_call *c) { hle_ret_s32(c, isnan(xf(c, 0)) != 0); }
static void isnormal_(hle_call *c) { hle_ret_s32(c, isnormal(xd(c, 0)) != 0); }
static void isnormalf_(hle_call *c) { hle_ret_s32(c, isnormal(xf(c, 0)) != 0); }
static void signbit_(hle_call *c) { hle_ret_s32(c, signbit(xd(c, 0)) != 0); }
static void signbitf_(hle_call *c) { hle_ret_s32(c, signbit(xf(c, 0)) != 0); }

/* ldiv_t ldiv(long, long): the quotient and remainder come back in rax:rdx */
static void ldiv_(hle_call *c) {
    int64_t n = (int64_t)hle_arg(c, 0), d = (int64_t)hle_arg(c, 1);
    if (!d || (n == INT64_MIN && d == -1)) {
        c->a->ret[0] = c->a->ret[1] = 0;
        return;
    }
    c->a->ret[0] = (uint64_t)(n / d);
    c->a->ret[1] = (uint64_t)(n % d);
}

/* ---- strings ---- */

/* a NUL-terminated guest string, copied (up to cap - 1 bytes); false if unmapped */
static bool gstr(hle_call *c, guest_addr a, char *out, size_t cap) { return a && hle_read_str(c, a, out, cap); }

static void bcmp_(hle_call *c) {
    uint64_t n = hle_arg(c, 2);
    const void *a = n ? hle_mem(c, hle_arg_ptr(c, 0), n) : NULL, *b = n ? hle_mem(c, hle_arg_ptr(c, 1), n) : NULL;
    hle_ret_s32(c, n && a && b ? memcmp(a, b, (size_t)n) != 0 : 0);
}
static void atol_(hle_call *c) {
    char s[128];
    hle_ret_u64(c, gstr(c, hle_arg_ptr(c, 0), s, sizeof s) ? (uint64_t)strtoll(s, NULL, 10) : 0);
}
static void strcoll_(hle_call *c) {
    char a[4096], b[4096];
    hle_ret_s32(c, gstr(c, hle_arg_ptr(c, 0), a, sizeof a) && gstr(c, hle_arg_ptr(c, 1), b, sizeof b) ? strcmp(a, b) : 0);
}
/* size_t strxfrm(char *dst, const char *src, size_t n): the C locale copies */
static void strxfrm_(hle_call *c) {
    char s[4096];
    if (!gstr(c, hle_arg_ptr(c, 1), s, sizeof s)) {
        hle_ret_u64(c, 0);
        return;
    }
    size_t len = strlen(s), n = (size_t)hle_arg(c, 2);
    if (n && hle_arg_ptr(c, 0)) gmem_write(c->mem, hle_arg_ptr(c, 0), s, len < n ? len + 1 : n);
    hle_ret_u64(c, len);
}
/* char *strnstr(const char *big, const char *little, size_t len) */
static void strnstr_(hle_call *c) {
    guest_addr big = hle_arg_ptr(c, 0);
    char little[1024];
    uint64_t len = hle_arg(c, 2);
    if (!big || !gstr(c, hle_arg_ptr(c, 1), little, sizeof little)) {
        hle_ret_u64(c, 0);
        return;
    }
    size_t ll = strlen(little);
    if (!ll) {
        hle_ret_u64(c, big);
        return;
    }
    for (uint64_t i = 0; i + ll <= len; i++) {
        char ch;
        if (!gmem_read(c->mem, big + i, &ch, 1) || !ch) break;
        const char *p = hle_mem(c, big + i, ll);
        if (p && !memcmp(p, little, ll)) {
            hle_ret_u64(c, big + i);
            return;
        }
    }
    hle_ret_u64(c, 0);
}
/* char *strtok_r(char *s, const char *sep, char **last) */
static guest_addr tok(hle_call *c, guest_addr s, guest_addr sep_at, guest_addr *last) {
    char sep[256];
    if (!gstr(c, sep_at, sep, sizeof sep)) return 0;
    if (!s) s = *last;
    if (!s) return 0;
    char ch;
    while (gmem_read(c->mem, s, &ch, 1) && ch && strchr(sep, ch)) s++;
    if (!gmem_read(c->mem, s, &ch, 1) || !ch) {
        *last = 0;
        return 0;
    }
    guest_addr start = s;
    while (gmem_read(c->mem, s, &ch, 1) && ch && !strchr(sep, ch)) s++;
    if (ch) {
        char zero = 0;
        gmem_write(c->mem, s, &zero, 1);
        *last = s + 1;
    } else {
        *last = s;
    }
    return start;
}
static void strtok_r_(hle_call *c) {
    guest_addr last = 0, at = hle_arg_ptr(c, 2);
    if (!at || !gmem_read(c->mem, at, &last, 8)) {
        hle_ret_u64(c, 0);
        return;
    }
    guest_addr r = tok(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1), &last);
    gmem_write(c->mem, at, &last, 8);
    hle_ret_u64(c, r);
}
static void strtok_(hle_call *c) {
    static guest_addr last; /* one per process, as the console's */
    hle_ret_u64(c, tok(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1), &last));
}

/* ---- wide characters (16-bit wchar_t) ---- */

static uint16_t wat(hle_call *c, guest_addr a, uint64_t i) {
    uint16_t v = 0;
    gmem_read(c->mem, a + i * 2, &v, 2);
    return v;
}
#define WCTYPE(name)                                                                               \
    static void name##_(hle_call *c) {                                                             \
        uint32_t w = hle_arg_u32(c, 0);                                                            \
        hle_ret_s32(c, w <= 0xFFFF && name((wint_t)w) ? 1 : 0);                                    \
    }
WCTYPE(iswalnum) WCTYPE(iswalpha) WCTYPE(iswblank) WCTYPE(iswcntrl) WCTYPE(iswdigit) WCTYPE(iswgraph)
WCTYPE(iswlower) WCTYPE(iswprint) WCTYPE(iswpunct) WCTYPE(iswspace) WCTYPE(iswupper) WCTYPE(iswxdigit)
#undef WCTYPE
static void towlower_(hle_call *c) {
    uint32_t w = hle_arg_u32(c, 0);
    hle_ret_u32(c, w <= 0xFFFF ? (uint32_t)towlower((wint_t)w) : w);
}
static void towupper_(hle_call *c) {
    uint32_t w = hle_arg_u32(c, 0);
    hle_ret_u32(c, w <= 0xFFFF ? (uint32_t)towupper((wint_t)w) : w);
}
static const char *const k_classes[] = {"alnum", "alpha", "blank", "cntrl", "digit", "graph",
                                        "lower", "print", "punct", "space", "upper", "xdigit"};
static void wctype_(hle_call *c) {
    char name[16];
    uint64_t r = 0;
    if (gstr(c, hle_arg_ptr(c, 0), name, sizeof name))
        for (size_t i = 0; i < PS5C_ARRAY_COUNT(k_classes); i++)
            if (!strcmp(name, k_classes[i])) r = i + 1;
    hle_ret_u64(c, r);
}
static void iswctype_(hle_call *c) {
    uint32_t w = hle_arg_u32(c, 0);
    uint64_t t = hle_arg(c, 1);
    if (w > 0xFFFF || !t || t > PS5C_ARRAY_COUNT(k_classes)) {
        hle_ret_s32(c, 0);
        return;
    }
    static int (*const k_fn[])(wint_t) = {iswalnum, iswalpha, iswblank, iswcntrl, iswdigit, iswgraph,
                                          iswlower, iswprint, iswpunct, iswspace, iswupper, iswxdigit};
    hle_ret_s32(c, k_fn[t - 1]((wint_t)w) ? 1 : 0);
}
static void wcscmp_(hle_call *c) {
    guest_addr a = hle_arg_ptr(c, 0), b = hle_arg_ptr(c, 1);
    for (uint64_t i = 0;; i++) {
        uint16_t x = wat(c, a, i), y = wat(c, b, i);
        if (x != y || !x) {
            hle_ret_s32(c, (int32_t)x - (int32_t)y);
            return;
        }
    }
}
static void wcsncmp_(hle_call *c) {
    guest_addr a = hle_arg_ptr(c, 0), b = hle_arg_ptr(c, 1);
    uint64_t n = hle_arg(c, 2);
    for (uint64_t i = 0; i < n; i++) {
        uint16_t x = wat(c, a, i), y = wat(c, b, i);
        if (x != y || !x) {
            hle_ret_s32(c, (int32_t)x - (int32_t)y);
            return;
        }
    }
    hle_ret_s32(c, 0);
}
static void wcsstr_(hle_call *c) {
    guest_addr big = hle_arg_ptr(c, 0), little = hle_arg_ptr(c, 1);
    uint64_t ll = 0;
    while (ll < 4096 && wat(c, little, ll)) ll++;
    for (uint64_t i = 0;; i++) {
        uint64_t k = 0;
        while (k < ll && wat(c, big, i + k) == wat(c, little, k)) k++;
        if (k == ll) {
            hle_ret_u64(c, big + i * 2);
            return;
        }
        if (!wat(c, big, i)) break;
    }
    hle_ret_u64(c, 0);
}
static void wmemchr_(hle_call *c) {
    guest_addr s = hle_arg_ptr(c, 0);
    uint16_t w = (uint16_t)hle_arg_u32(c, 1);
    uint64_t n = hle_arg(c, 2);
    for (uint64_t i = 0; i < n; i++)
        if (wat(c, s, i) == w) {
            hle_ret_u64(c, s + i * 2);
            return;
        }
    hle_ret_u64(c, 0);
}
/* long wcstol(const wchar_t *s, wchar_t **end, int base) */
static void wcstol_(hle_call *c) {
    guest_addr s = hle_arg_ptr(c, 0);
    char buf[128];
    uint64_t n = 0;
    while (n < 127 && wat(c, s, n) && wat(c, s, n) < 0x80) buf[n] = (char)wat(c, s, n), n++;
    buf[n] = 0;
    char *end = buf;
    int64_t v = strtoll(buf, &end, hle_arg_s32(c, 2));
    if (hle_arg_ptr(c, 1)) {
        guest_addr e = s + (uint64_t)(end - buf) * 2;
        gmem_write(c->mem, hle_arg_ptr(c, 1), &e, 8);
    }
    hle_ret_u64(c, (uint64_t)v);
}

/* ---- multibyte: UTF-8 ---- */

static size_t utf8_put(uint32_t w, uint8_t *out) {
    if (w < 0x80) return out[0] = (uint8_t)w, 1;
    if (w < 0x800) return out[0] = (uint8_t)(0xC0 | w >> 6), out[1] = (uint8_t)(0x80 | (w & 63)), 2;
    out[0] = (uint8_t)(0xE0 | w >> 12), out[1] = (uint8_t)(0x80 | ((w >> 6) & 63)), out[2] = (uint8_t)(0x80 | (w & 63));
    return 3;
}
/* bytes of one character at s (n available): 0 for NUL, -1 invalid, -2 incomplete */
static int64_t utf8_get(const uint8_t *s, uint64_t n, uint32_t *w) {
    if (!n) return -2;
    uint32_t b = s[0], len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 0;
    if (!len) return -1;
    if (n < len) return -2;
    uint32_t v = len == 1 ? b : len == 2 ? b & 31 : b & 15;
    for (uint32_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) return -1;
        v = v << 6 | (s[i] & 63);
    }
    *w = v;
    return v ? (int64_t)len : 0;
}
/* size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps) */
static void wcrtomb_(hle_call *c) {
    uint8_t out[4];
    size_t n = utf8_put(hle_arg_u32(c, 1) & 0xFFFF, out);
    if (!hle_arg_ptr(c, 0)) {
        hle_ret_u64(c, 1);
        return;
    }
    hle_ret_u64(c, gmem_write(c->mem, hle_arg_ptr(c, 0), out, n) ? n : (uint64_t)-1);
}
static void btowc_(hle_call *c) {
    int32_t ch = hle_arg_s32(c, 0);
    hle_ret_u32(c, ch >= 0 && ch < 0x80 ? (uint32_t)ch : 0xFFFFFFFFu);
}
/* size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps) */
static int64_t mbr(hle_call *c, guest_addr pwc, guest_addr s, uint64_t n) {
    if (!s) return 0;
    uint8_t b[3] = {0};
    uint64_t have = n < 3 ? n : 3;
    for (uint64_t i = 0; i < have; i++)
        if (!gmem_read(c->mem, s + i, &b[i], 1)) have = i;
    uint32_t w = 0;
    int64_t r = utf8_get(b, have, &w);
    if (r >= 0 && pwc) {
        uint16_t w16 = (uint16_t)w;
        gmem_write(c->mem, pwc, &w16, 2);
    }
    return r;
}
static void mbrtowc_(hle_call *c) { hle_ret_u64(c, (uint64_t)mbr(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1), hle_arg(c, 2))); }
static void mbrlen_(hle_call *c) { hle_ret_u64(c, (uint64_t)mbr(c, 0, hle_arg_ptr(c, 0), hle_arg(c, 1))); }
/* size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *ps) */
static void mbsrtowcs_(hle_call *c) {
    guest_addr dst = hle_arg_ptr(c, 0), srcp = hle_arg_ptr(c, 1), s = 0;
    uint64_t len = hle_arg(c, 2), count = 0;
    if (!srcp || !gmem_read(c->mem, srcp, &s, 8) || !s) {
        hle_ret_u64(c, 0);
        return;
    }
    for (;;) {
        if (dst && count == len) break;
        uint16_t w = 0;
        int64_t r = mbr(c, dst ? dst + count * 2 : 0, s, 3);
        if (r < 0) {
            hle_ret_u64(c, (uint64_t)-1);
            return;
        }
        if (!r) {
            if (dst) gmem_write(c->mem, dst + count * 2, &w, 2), s = 0;
            break;
        }
        s += (uint64_t)r, count++;
    }
    if (dst) gmem_write(c->mem, srcp, &s, 8);
    hle_ret_u64(c, count);
}

/* __cxa_guard_acquire(guard *): 1 when the caller is to initialize (byte 0 clear, byte 1
 * claims it); release marks it done. A host lock is not held across the guest's
 * initializer (a nested static would deadlock); a second thread waits by polling. */
static void guard_acquire_(hle_call *c) {
    guest_addr g = hle_arg_ptr(c, 0);
    uint8_t *p = g ? (uint8_t *)hle_mem(c, g, 2) : NULL;
    if (!p) {
        hle_ret_s32(c, 0);
        return;
    }
    for (;;) {
        if (__atomic_load_n(&p[0], __ATOMIC_ACQUIRE)) {
            hle_ret_s32(c, 0);
            return;
        }
        uint8_t free_ = 0;
        if (__atomic_compare_exchange_n(&p[1], &free_, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            hle_ret_s32(c, 1);
            return;
        }
        pal_sleep_ns(100000);
    }
}
static void guard_release_(hle_call *c) {
    uint8_t *p = hle_arg_ptr(c, 0) ? (uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 2) : NULL;
    if (p) __atomic_store_n(&p[0], 1, __ATOMIC_RELEASE), __atomic_store_n(&p[1], 0, __ATOMIC_RELEASE);
    hle_ret_s32(c, 0);
}
static void guard_abort_(hle_call *c) {
    uint8_t *p = hle_arg_ptr(c, 0) ? (uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 2) : NULL;
    if (p) __atomic_store_n(&p[1], 0, __ATOMIC_RELEASE);
    hle_ret_s32(c, 0);
}

#define E(name, fn) HLE_FUNC_ENTRY("libSceLibcInternal", name, fn)
#define M(name) E(#name, name##_)
static const hle_export exports[] = {
    M(asin), M(atan), M(log), M(sqrt), M(cbrt), M(cosh), M(sinh), M(tanh), M(fabs), M(asinh), M(acosh), M(atanh),
    M(expm1), M(log1p), M(trunc), M(floor), M(ceil), M(nearbyint), M(rint),
    M(asinf), M(atanf), M(log10f), M(tanf), M(sqrtf), M(sinhf), M(coshf), M(tanhf), M(fabsf), M(floorf), M(ceilf),
    M(truncf), M(roundf), M(cbrtf), M(log1pf), M(expm1f), M(rintf),
    M(fmod), M(hypot), M(fmin), M(fmax), M(copysign), M(remainder),
    M(atan2f), M(fmodf), M(hypotf), M(fminf), M(fmaxf), M(copysignf),
    M(ldexpf), M(scalbn), M(scalbnf), M(frexpf), M(modf), M(modff),
    M(lrint), M(lrintf), E("llrint", lrint_), E("llrintf", lrintf_),
    E("__isinf", isinf_), E("__isinff", isinff_), E("isnan", isnan_), E("__isnan", isnan_), E("isnanf", isnanf_),
    E("__isnanf", isnanf_), E("__isnormal", isnormal_), E("__isnormalf", isnormalf_), E("__signbit", signbit_),
    E("__signbitf", signbitf_), M(ldiv), E("lldiv", ldiv_),
    M(bcmp), M(atol), E("atoll", atol_), M(strcoll), M(strxfrm), M(strnstr), M(strtok), M(strtok_r),
    M(iswalnum), M(iswalpha), M(iswblank), M(iswcntrl), M(iswdigit), M(iswgraph), M(iswlower), M(iswprint),
    M(iswpunct), M(iswspace), M(iswupper), M(iswxdigit), M(towlower), M(towupper), M(wctype), M(iswctype),
    M(wcscmp), M(wcsncmp), E("wcscoll", wcscmp_), M(wcsstr), M(wmemchr), M(wcstol),
    M(wcrtomb), M(btowc), M(mbrtowc), M(mbrlen), M(mbsrtowcs),
    E("__cxa_guard_acquire", guard_acquire_), E("__cxa_guard_release", guard_release_),
    E("__cxa_guard_abort", guard_abort_),
};
#undef M
#undef E

void hle_register_libc_extra(void) { hle_register_table(exports, PS5C_ARRAY_COUNT(exports)); }
