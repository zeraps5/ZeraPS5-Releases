/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "base/crypto.h"
#include <string.h>

/* ---- AES-128 ---- */

static uint8_t g_sbox[256], g_inv[256];
static bool g_tables;

static uint8_t xt(uint8_t v) { return (uint8_t)(v << 1 ^ (v & 0x80 ? 0x1b : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (; b; b >>= 1, a = xt(a))
        if (b & 1) r ^= a;
    return r;
}

/* The S-box from its definition: multiplicative inverse in GF(2^8), then
 * the affine map. Built once; the result is the same in every thread. */
static void tables(void) {
    if (g_tables) return;
    for (int i = 0; i < 256; i++) {
        uint8_t inv = 0;
        if (i)
            for (int j = 1; j < 256; j++)
                if (gmul((uint8_t)i, (uint8_t)j) == 1) { inv = (uint8_t)j; break; }
        uint8_t s = inv, r = inv;
        for (int k = 0; k < 4; k++) {
            r = (uint8_t)(r << 1 | r >> 7);
            s ^= r;
        }
        s ^= 0x63;
        g_sbox[i] = s;
        g_inv[s] = (uint8_t)i;
    }
    g_tables = true;
}

static void inv_mix(uint8_t *s) {
    for (int c = 0; c < 4; c++) {
        uint8_t *p = s + 4 * c, a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
        p[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
        p[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
        p[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
    }
}

void aes128_init(aes128 *a, const uint8_t key[16]) {
    tables();
    uint8_t *w = a->enc, rcon = 1;
    memcpy(w, key, 16);
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = {w[i - 4], w[i - 3], w[i - 2], w[i - 1]};
        if (i % 16 == 0) {
            uint8_t t0 = t[0];
            t[0] = g_sbox[t[1]] ^ rcon;
            t[1] = g_sbox[t[2]];
            t[2] = g_sbox[t[3]];
            t[3] = g_sbox[t0];
            rcon = xt(rcon);
        }
        for (int k = 0; k < 4; k++) w[i + k] = w[i - 16 + k] ^ t[k];
    }
    /* Equivalent inverse cipher: round keys 1..9 go through InvMixColumns. */
    for (int r = 0; r <= 10; r++) memcpy(a->dec + 16 * r, a->enc + 16 * (10 - r), 16);
    for (int r = 1; r < 10; r++) inv_mix(a->dec + 16 * r);
}

void aes128_encrypt(const aes128 *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ a->enc[i];
    for (int r = 1; r <= 10; r++) {
        for (int i = 0; i < 16; i++) t[i] = g_sbox[s[(i + 4 * (i & 3)) & 15]]; /* SubBytes + ShiftRows */
        if (r < 10)
            for (int c = 0; c < 4; c++) {
                uint8_t *p = t + 4 * c, a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3], all = a0 ^ a1 ^ a2 ^ a3;
                p[0] ^= all ^ xt(a0 ^ a1);
                p[1] ^= all ^ xt(a1 ^ a2);
                p[2] ^= all ^ xt(a2 ^ a3);
                p[3] ^= all ^ xt(a3 ^ a0);
            }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ a->enc[16 * r + i];
    }
    memcpy(out, s, 16);
}

void aes128_decrypt(const aes128 *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ a->dec[i];
    for (int r = 1; r <= 10; r++) {
        for (int i = 0; i < 16; i++) t[i] = g_inv[s[(i - 4 * (i & 3)) & 15]]; /* InvSubBytes + InvShiftRows */
        if (r < 10) inv_mix(t);
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ a->dec[16 * r + i];
    }
    memcpy(out, s, 16);
}

/* ---- XTS ---- */

void aes_xts_init(aes_xts *x, const uint8_t data_key[16], const uint8_t tweak_key[16]) {
    aes128_init(&x->data, data_key);
    aes128_init(&x->tweak, tweak_key);
}

static void xts(const aes_xts *x, uint8_t *buf, size_t size, uint64_t unit, bool enc) {
    uint8_t t[16] = {0}, b[16];
    for (int i = 0; i < 8; i++) t[i] = (uint8_t)(unit >> (8 * i));
    aes128_encrypt(&x->tweak, t, t);
    for (size_t off = 0; off + 16 <= size; off += 16) {
        for (int i = 0; i < 16; i++) b[i] = buf[off + i] ^ t[i];
        if (enc) aes128_encrypt(&x->data, b, b);
        else aes128_decrypt(&x->data, b, b);
        for (int i = 0; i < 16; i++) buf[off + i] = b[i] ^ t[i];
        uint8_t carry = 0;
        for (int i = 0; i < 16; i++) {
            uint8_t v = t[i];
            t[i] = (uint8_t)(v << 1 | carry);
            carry = v >> 7;
        }
        if (carry) t[0] ^= 0x87;
    }
}
void aes_xts_decrypt(const aes_xts *x, uint8_t *buf, size_t size, uint64_t unit) { xts(x, buf, size, unit, false); }
void aes_xts_encrypt(const aes_xts *x, uint8_t *buf, size_t size, uint64_t unit) { xts(x, buf, size, unit, true); }

/* ---- SHA-256 ---- */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t ror(uint32_t v, int n) { return v >> n | v << (32 - n); }

static void sha256_block(sha256_ctx *c, const uint8_t *p) {
    uint32_t w[64], s[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ w[i - 15] >> 3;
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ w[i - 2] >> 10;
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(s, c->h, sizeof s);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = s[7] + (ror(s[4], 6) ^ ror(s[4], 11) ^ ror(s[4], 25)) + ((s[4] & s[5]) ^ (~s[4] & s[6])) + K256[i] + w[i];
        uint32_t t2 = (ror(s[0], 2) ^ ror(s[0], 13) ^ ror(s[0], 22)) + ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
        memmove(s + 1, s, 7 * sizeof *s);
        s[4] += t1;
        s[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) c->h[i] += s[i];
}

void sha256_init(sha256_ctx *c) {
    static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, h0, sizeof h0);
    c->length = 0;
    c->block_len = 0;
}

void sha256_update(sha256_ctx *c, const void *data, size_t size) {
    const uint8_t *p = data;
    c->length += size;
    while (size) {
        size_t n = 64 - c->block_len < size ? 64 - c->block_len : size;
        memcpy(c->block + c->block_len, p, n);
        c->block_len += n, p += n, size -= n;
        if (c->block_len == 64) { sha256_block(c, c->block); c->block_len = 0; }
    }
}

void sha256_final(sha256_ctx *c, uint8_t digest[32]) {
    uint64_t bits = c->length * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    sha256_update(c, &pad, 1);
    while (c->block_len != 56) sha256_update(c, &zero, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(c, len, 8);
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 4; k++) digest[4 * i + k] = (uint8_t)(c->h[i] >> (24 - 8 * k));
}

void sha256(const void *data, size_t size, uint8_t digest[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, size);
    sha256_final(&c, digest);
}

void hmac_sha256(const void *key, size_t key_size, const void *msg, size_t msg_size, uint8_t mac[32]) {
    uint8_t k[64] = {0}, pad[64], inner[32];
    if (key_size > 64) sha256(key, key_size, k);
    else memcpy(k, key, key_size);
    sha256_ctx c;
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&c);
    sha256_update(&c, pad, 64);
    sha256_update(&c, msg, msg_size);
    sha256_final(&c, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&c);
    sha256_update(&c, pad, 64);
    sha256_update(&c, inner, 32);
    sha256_final(&c, mac);
}

/* ---- SHA3-256 (Keccak-f[1600], rate 136, domain bits 01) ---- */

static uint64_t rol64(uint64_t v, int n) { return n ? v << n | v >> (64 - n) : v; }

static void keccak(uint64_t a[25]) {
    static const uint64_t rc[24] = {
        0x0000000000000001ull, 0x0000000000008082ull, 0x800000000000808aull, 0x8000000080008000ull,
        0x000000000000808bull, 0x0000000080000001ull, 0x8000000080008081ull, 0x8000000000008009ull,
        0x000000000000008aull, 0x0000000000000088ull, 0x0000000080008009ull, 0x000000008000000aull,
        0x000000008000808bull, 0x800000000000008bull, 0x8000000000008089ull, 0x8000000000008003ull,
        0x8000000000008002ull, 0x8000000000000080ull, 0x000000000000800aull, 0x800000008000000aull,
        0x8000000080008081ull, 0x8000000000008080ull, 0x0000000080000001ull, 0x8000000080008008ull};
    static const int rot[25] = {0, 1, 62, 28, 27, 36, 44, 6, 55, 20, 3, 10, 43, 25, 39, 41, 45, 15, 21, 8, 18, 2, 61, 56, 14};
    for (int round = 0; round < 24; round++) {
        uint64_t c[5], b[25];
        for (int x = 0; x < 5; x++) c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
        for (int x = 0; x < 5; x++) {
            uint64_t d = c[(x + 4) % 5] ^ rol64(c[(x + 1) % 5], 1);
            for (int y = 0; y < 25; y += 5) a[y + x] ^= d;
        }
        /* rho + pi: lane (x, y) moves to (y, 2x + 3y) */
        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++) b[y + 5 * ((2 * x + 3 * y) % 5)] = rol64(a[x + 5 * y], rot[x + 5 * y]);
        for (int y = 0; y < 25; y += 5)
            for (int x = 0; x < 5; x++) a[y + x] = b[y + x] ^ (~b[y + (x + 1) % 5] & b[y + (x + 2) % 5]);
        a[0] ^= rc[round];
    }
}

void sha3_256(const void *data, size_t size, uint8_t digest[32]) {
    enum { RATE = 136 };
    uint64_t a[25] = {0};
    uint8_t block[RATE];
    const uint8_t *p = data;
    for (;;) {
        size_t n = size < RATE ? size : RATE;
        memset(block, 0, RATE);
        memcpy(block, p, n);
        if (n < RATE) {
            block[n] ^= 0x06;
            block[RATE - 1] ^= 0x80;
        }
        for (int i = 0; i < RATE / 8; i++) {
            uint64_t v = 0;
            for (int k = 0; k < 8; k++) v |= (uint64_t)block[8 * i + k] << (8 * k);
            a[i] ^= v;
        }
        keccak(a);
        if (n < RATE) break;
        p += n, size -= n;
    }
    for (int i = 0; i < 32; i++) digest[i] = (uint8_t)(a[i / 8] >> (8 * (i % 8)));
}
