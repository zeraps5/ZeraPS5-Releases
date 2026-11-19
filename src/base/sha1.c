/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "base/sha1.h"

#include <string.h>

#define ROL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static void sha1_block(sha1_ctx *ctx, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3], e = ctx->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t t = ROL32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL32(b, 30);
        b = a;
        a = t;
    }
    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
}

void sha1_init(sha1_ctx *ctx) {
    ctx->h[0] = 0x67452301u;
    ctx->h[1] = 0xEFCDAB89u;
    ctx->h[2] = 0x98BADCFEu;
    ctx->h[3] = 0x10325476u;
    ctx->h[4] = 0xC3D2E1F0u;
    ctx->length = 0;
    ctx->block_len = 0;
}

void sha1_update(sha1_ctx *ctx, const void *data, size_t size) {
    const uint8_t *p = (const uint8_t *)data;
    ctx->length += size;
    while (size > 0) {
        size_t n = PS5C_MIN(size, 64 - ctx->block_len);
        memcpy(ctx->block + ctx->block_len, p, n);
        ctx->block_len += n;
        p += n;
        size -= n;
        if (ctx->block_len == 64) {
            sha1_block(ctx, ctx->block);
            ctx->block_len = 0;
        }
    }
}

void sha1_final(sha1_ctx *ctx, uint8_t digest[20]) {
    uint64_t bits = ctx->length * 8;
    static const uint8_t k_pad_start = 0x80;
    static const uint8_t k_zero = 0;
    sha1_update(ctx, &k_pad_start, 1);
    while (ctx->block_len != 56) {
        sha1_update(ctx, &k_zero, 1);
    }
    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) {
        len_be[i] = (uint8_t)(bits >> (56 - i * 8));
    }
    sha1_update(ctx, len_be, 8);
    for (int i = 0; i < 5; i++) {
        digest[i * 4] = (uint8_t)(ctx->h[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)ctx->h[i];
    }
}

void sha1(const void *data, size_t size, uint8_t digest[20]) {
    sha1_ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, size);
    sha1_final(&ctx, digest);
}
