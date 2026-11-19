/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Montgomery modular exponentiation over 32-bit limbs (little-endian limb
 * order), left-to-right square and multiply. */
#include "base/rsa.h"

#include <string.h>

#define MAXL (RSA_MAX_BYTES / 4)

static void from_be(uint32_t *x, size_t limbs, const uint8_t *b, size_t len) {
    memset(x, 0, limbs * 4);
    for (size_t i = 0; i < len; i++) x[i / 4] |= (uint32_t)b[len - 1 - i] << (8 * (i % 4));
}

static void to_be(uint8_t *b, size_t len, const uint32_t *x) {
    for (size_t i = 0; i < len; i++) b[len - 1 - i] = (uint8_t)(x[i / 4] >> (8 * (i % 4)));
}

static int cmp(const uint32_t *a, const uint32_t *b, size_t k) {
    for (size_t i = k; i--;)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* a -= b, returns the borrow */
static uint32_t sub(uint32_t *a, const uint32_t *b, size_t k) {
    uint64_t borrow = 0;
    for (size_t i = 0; i < k; i++) {
        uint64_t t = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)t;
        borrow = (t >> 32) & 1;
    }
    return (uint32_t)borrow;
}

/* r = a * b * R^-1 mod n (CIOS); r may not alias a or b */
static void mont_mul(uint32_t *r, const uint32_t *a, const uint32_t *b, const uint32_t *n, uint32_t n0, size_t k) {
    uint32_t t[MAXL + 2];
    memset(t, 0, (k + 2) * 4);
    for (size_t i = 0; i < k; i++) {
        uint64_t c = 0;
        for (size_t j = 0; j < k; j++) {
            uint64_t s = (uint64_t)a[j] * b[i] + t[j] + c;
            t[j] = (uint32_t)s;
            c = s >> 32;
        }
        uint64_t s = (uint64_t)t[k] + c;
        t[k] = (uint32_t)s;
        t[k + 1] = (uint32_t)(s >> 32);
        uint32_t m = t[0] * n0;
        s = (uint64_t)m * n[0] + t[0];
        c = s >> 32;
        for (size_t j = 1; j < k; j++) {
            s = (uint64_t)m * n[j] + t[j] + c;
            t[j - 1] = (uint32_t)s;
            c = s >> 32;
        }
        s = (uint64_t)t[k] + c;
        t[k - 1] = (uint32_t)s;
        t[k] = t[k + 1] + (uint32_t)(s >> 32);
    }
    if (t[k] || cmp(t, n, k) >= 0) sub(t, n, k);
    memcpy(r, t, k * 4);
}

bool rsa_private(const uint8_t *nb, const uint8_t *db, const uint8_t *cb, size_t len, uint8_t *out) {
    if (len < 16 || len > RSA_MAX_BYTES || !(nb[len - 1] & 1)) return false;
    size_t k = (len + 3) / 4;
    uint32_t n[MAXL], d[MAXL], c[MAXL], x[MAXL], cr[MAXL], t[MAXL], rr[MAXL];
    from_be(n, k, nb, len);
    from_be(d, k, db, len);
    from_be(c, k, cb, len);
    if (cmp(c, n, k) >= 0) return false;
    /* n0 = -n^-1 mod 2^32 (Newton) */
    uint32_t inv = 1;
    for (int i = 0; i < 5; i++) inv *= 2 - n[0] * inv;
    uint32_t n0 = (uint32_t)0 - inv;
    /* rr = R^2 mod n: 1 doubled 2 * 32k times, reduced at each step */
    memset(rr, 0, k * 4);
    rr[0] = 1;
    for (size_t i = 0; i < 64 * k; i++) {
        uint32_t carry = 0;
        for (size_t j = 0; j < k; j++) {
            uint32_t v = rr[j];
            rr[j] = v << 1 | carry;
            carry = v >> 31;
        }
        if (carry || cmp(rr, n, k) >= 0) sub(rr, n, k);
    }
    mont_mul(cr, c, rr, n, n0, k); /* c in Montgomery form */
    memset(t, 0, k * 4);
    t[0] = 1;
    mont_mul(x, t, rr, n, n0, k); /* 1 in Montgomery form */
    size_t top = 32 * k;
    while (top && !((d[(top - 1) / 32] >> ((top - 1) % 32)) & 1)) top--;
    for (size_t i = top; i--;) {
        mont_mul(t, x, x, n, n0, k);
        if ((d[i / 32] >> (i % 32)) & 1) mont_mul(x, t, cr, n, n0, k);
        else memcpy(x, t, k * 4);
    }
    memset(t, 0, k * 4);
    t[0] = 1;
    mont_mul(c, x, t, n, n0, k); /* out of Montgomery form */
    to_be(out, len, c);
    return true;
}

long rsa_pkcs1_unpad(const uint8_t *b, size_t len) {
    if (len < 11 || b[0] != 0 || b[1] != 2) return -1;
    size_t i = 2;
    while (i < len && b[i]) i++;
    if (i == len || i < 10) return -1;
    return (long)(i + 1);
}
