/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SHA-1, needed to compute PS4/PS5 NIDs. Not used for anything security related. */
#ifndef PS5C_SHA1_H
#define PS5C_SHA1_H

#include "base/base.h"

typedef struct sha1_ctx {
    uint32_t h[5];
    uint64_t length;
    uint8_t block[64];
    size_t block_len;
} sha1_ctx;

void sha1_init(sha1_ctx *ctx);
void sha1_update(sha1_ctx *ctx, const void *data, size_t size);
void sha1_final(sha1_ctx *ctx, uint8_t digest[20]);
void sha1(const void *data, size_t size, uint8_t digest[20]);

#endif
