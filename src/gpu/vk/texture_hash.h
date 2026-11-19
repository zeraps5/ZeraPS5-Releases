/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_TEXTURE_HASH_H
#define PS5C_TEXTURE_HASH_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define TEXTURE_FULL_HASH_LIMIT (64u << 10)

/* Word-at-a-time hash for keys and change detection (not the code hash sh_analyze stores). */
static inline uint64_t texture_hash_words(const void *data, size_t size, uint64_t h) {
    const uint8_t *p = (const uint8_t *)data;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ (w * 0xC2B2AE3D27D4EB4Full)) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; i < size; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static inline uint64_t texture_content_hash(const uint8_t *p, uint64_t size) {
    uint64_t h = 0xCBF29CE484222325ull;
    /* Small dynamic textures are checked at every use. Sampling them can miss
     * a CPU rewrite in a reused slot and leave old glyphs or tiles visible. */
    if (size <= TEXTURE_FULL_HASH_LIMIT) return texture_hash_words(p, (size_t)size, h) | 1;
    /* Large surfaces keep the inexpensive sampled detector. Spread the
     * remainder too, so the final sample always covers the final 64 bytes. */
    for (uint32_t i = 0; i < 256; i++) {
        uint64_t offset = ((size - 64) / 255) * i + (((size - 64) % 255) * i) / 255;
        h = texture_hash_words(p + offset, 64, h);
    }
    return h | 1;
}

/* XXH64 algorithm, independently implemented from Yann Collet's specification:
 * https://github.com/Cyan4973/xxHash/blob/dev/doc/xxhash_spec.md
 * Four independent accumulators avoid the serial dependency of hash_words.
 * These are transient CPU content hashes, never ISA/disk-cache keys. */
#define TXH_P1 UINT64_C(11400714785074694791)
#define TXH_P2 UINT64_C(14029467366897019727)
#define TXH_P3 UINT64_C(1609587929392839161)
#define TXH_P4 UINT64_C(9650029242287828579)
#define TXH_P5 UINT64_C(2870177450012600261)
static inline uint64_t texture_hash_rot(uint64_t v, unsigned n) { return (v << n) | (v >> (64 - n)); }
static inline uint64_t texture_hash_le64(const uint8_t *p) {
    uint64_t v; memcpy(&v, p, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap64(v);
#endif
    return v;
}
static inline uint32_t texture_hash_le32(const uint8_t *p) {
    uint32_t v; memcpy(&v, p, 4);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    return v;
}
static inline uint64_t texture_hash_round(uint64_t v, uint64_t word) {
    return texture_hash_rot(v + word * TXH_P2, 31) * TXH_P1;
}
typedef struct texture_hash_lanes { uint64_t v[4]; } texture_hash_lanes;
static inline texture_hash_lanes texture_hash_init(uint64_t seed) {
    return (texture_hash_lanes){{seed + TXH_P1 + TXH_P2, seed + TXH_P2, seed, seed - TXH_P1}};
}
static inline void texture_hash_stripe(texture_hash_lanes *s, const uint8_t *p) {
    for (unsigned k = 0; k < 4; ++k) s->v[k] = texture_hash_round(s->v[k], texture_hash_le64(p + 8 * k));
}
static inline uint64_t texture_hash_merge(const texture_hash_lanes *s) {
    uint64_t h = texture_hash_rot(s->v[0], 1) + texture_hash_rot(s->v[1], 7) +
                 texture_hash_rot(s->v[2], 12) + texture_hash_rot(s->v[3], 18);
    for (unsigned k = 0; k < 4; ++k) h = (h ^ texture_hash_round(0, s->v[k])) * TXH_P1 + TXH_P4;
    return h;
}
static inline uint64_t texture_hash_finish(uint64_t h) {
    h ^= h >> 33; h *= TXH_P2; h ^= h >> 29; h *= TXH_P3; return h ^ (h >> 32);
}
static inline uint64_t texture_xxh64(const uint8_t *p, size_t size, uint64_t seed) {
    size_t left = size;
    uint64_t h = seed + TXH_P5;
    if (left >= 32) {
        texture_hash_lanes s = texture_hash_init(seed);
        do { texture_hash_stripe(&s, p); p += 32; left -= 32; } while (left >= 32);
        h = texture_hash_merge(&s);
    }
    h += size;
    while (left >= 8) { h = texture_hash_rot(h ^ texture_hash_round(0, texture_hash_le64(p)), 27) * TXH_P1 + TXH_P4; p += 8; left -= 8; }
    if (left >= 4) { h = texture_hash_rot(h ^ ((uint64_t)texture_hash_le32(p) * TXH_P1), 23) * TXH_P2 + TXH_P3; p += 4; left -= 4; }
    while (left) { h = texture_hash_rot(h ^ ((uint64_t)*p++ * TXH_P5), 11) * TXH_P1; --left; }
    return texture_hash_finish(h);
}
static inline uint64_t texture_content_hash_fast(const uint8_t *p, uint64_t size) {
    const uint64_t seed = UINT64_C(0xCBF29CE484222325);
    if (size <= TEXTURE_FULL_HASH_LIMIT) return texture_xxh64(p, (size_t)size, seed) | 1;
    /* Hash the concatenation of exactly the existing samples. No unsampled
     * bytes become sampled, and no previously sampled byte is omitted. */
    texture_hash_lanes s = texture_hash_init(seed);
    for (uint32_t i = 0; i < 256; ++i) {
        uint64_t at = ((size - 64) / 255) * i + (((size - 64) % 255) * i) / 255;
        texture_hash_stripe(&s, p + at); texture_hash_stripe(&s, p + at + 32);
    }
    return texture_hash_finish(texture_hash_merge(&s) + 256u * 64u) | 1;
}
#endif
