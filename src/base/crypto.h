/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Primitives for reading package files: AES-128 (FIPS 197) with XTS
 * (IEEE 1619), SHA-256 and HMAC-SHA256 (FIPS 180-4, RFC 2104) and SHA3-256
 * (FIPS 202). Written for decoding game data, not for protecting secrets:
 * no constant-time guarantees. */
#ifndef PS5C_CRYPTO_H
#define PS5C_CRYPTO_H

#include "base/base.h"

typedef struct aes128 {
    uint8_t enc[176], dec[176]; /* round keys */
} aes128;

void aes128_init(aes128 *a, const uint8_t key[16]);
void aes128_encrypt(const aes128 *a, const uint8_t in[16], uint8_t out[16]);
void aes128_decrypt(const aes128 *a, const uint8_t in[16], uint8_t out[16]);

typedef struct aes_xts {
    aes128 data, tweak;
} aes_xts;

void aes_xts_init(aes_xts *x, const uint8_t data_key[16], const uint8_t tweak_key[16]);
/* Decrypts (or encrypts) one data unit in place; size is a multiple of 16.
 * The tweak is the unit number as a 128-bit little-endian value. */
void aes_xts_decrypt(const aes_xts *x, uint8_t *buf, size_t size, uint64_t unit);
void aes_xts_encrypt(const aes_xts *x, uint8_t *buf, size_t size, uint64_t unit);

typedef struct sha256_ctx {
    uint32_t h[8];
    uint64_t length;
    uint8_t block[64];
    size_t block_len;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t size);
void sha256_final(sha256_ctx *c, uint8_t digest[32]);
void sha256(const void *data, size_t size, uint8_t digest[32]);
void hmac_sha256(const void *key, size_t key_size, const void *msg, size_t msg_size, uint8_t mac[32]);

void sha3_256(const void *data, size_t size, uint8_t digest[32]);

#endif
