/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libScePngEnc: 8-bit RGB(A) pictures to PNG files in the game's memory (screenshots,
 * save icons). The structures and checks are as KytyPS5's libPngEnc.cpp (GPL-2.0)
 * documents them; the PNG is ours: filter 0 on every row and stored (uncompressed)
 * deflate blocks, which every PNG reader takes.
 *
 * Create parameters (16 bytes): u32 size (16), u32 attribute (0), u32 maximum width,
 * u32 maximum number of filters (0..4). The context (16 bytes, the game's memory):
 * u32 magic, u32 maximum width, u32 maximum number of filters.
 * Encode parameters (48 bytes): +0 picture address, +8 PNG address, +16 picture size,
 * +20 PNG buffer size, +24 width, +28 height, +32 pitch (bytes), +36 u16 pixel format
 * (0 RGBA, 1 BGRA), +38 u16 color space (3 RGB, 19 RGBA), +40 u16 bit depth (8),
 * +42 u16 palette entries (0), +44 u16 filter bits, +46 u16 compression level (0..9).
 * Output (8 bytes): u32 PNG size, u32 rows done. */
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "hle/hle.h"

void hle_register_table(const hle_export *table, size_t count);

#define E_ADDR ((int32_t)0x80690101)
#define E_SIZE ((int32_t)0x80690102)
#define E_PARAM ((int32_t)0x80690103)
#define E_HANDLE ((int32_t)0x80690104)
#define E_OVERFLOW ((int32_t)0x80690110)
#define E_FATAL ((int32_t)0x80690120)
#define MAGIC 0x504E4745u /* "EGNP" */
#define MAX_SIDE 1000000u

static int32_t check_create(hle_call *c, guest_addr param, uint32_t *max_width, uint32_t *max_filters) {
    uint32_t p[4] = {0};
    if (!param || !gmem_read(c->mem, param, p, sizeof(p))) return E_ADDR;
    if (p[0] != 16 || p[1] || !p[2] || p[2] > MAX_SIDE || p[3] > 4) return E_PARAM;
    *max_width = p[2], *max_filters = p[3];
    return 0;
}

/* int scePngEncQueryMemorySize(const param *) */
static void query_memory_size(hle_call *c) {
    uint32_t w, f;
    int32_t e = check_create(c, hle_arg_ptr(c, 0), &w, &f);
    hle_ret_s32(c, e ? e : 16);
}

/* int scePngEncCreate(const param *, void *memory, uint32_t size, void **handle) */
static void create(hle_call *c) {
    uint32_t w, f;
    int32_t e = check_create(c, hle_arg_ptr(c, 0), &w, &f);
    guest_addr mem = hle_arg_ptr(c, 1), out = hle_arg_ptr(c, 3);
    if (!e && (!mem || !out)) e = E_ADDR;
    if (!e && hle_arg_u32(c, 2) < 16) e = E_SIZE;
    if (!e) {
        uint32_t ctx[4] = {MAGIC, w, f, 0};
        if (!gmem_write(c->mem, mem, ctx, sizeof(ctx)) || !gmem_write_u64(c->mem, out, mem)) e = E_ADDR;
    }
    hle_ret_s32(c, e);
}

/* int scePngEncDelete(void *handle) */
static void destroy(hle_call *c) {
    guest_addr h = hle_arg_ptr(c, 0);
    uint32_t magic = 0;
    if (!h || !gmem_read(c->mem, h, &magic, 4) || magic != MAGIC) {
        hle_ret_s32(c, E_HANDLE);
        return;
    }
    magic = 0;
    gmem_write(c->mem, h, &magic, 4);
    hle_ret_s32(c, 0);
}

static uint32_t g_crc[256];

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    if (!g_crc[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            g_crc[i] = v;
        }
    for (size_t i = 0; i < n; i++) crc = g_crc[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

typedef struct png_out {
    uint8_t *buf;
    size_t len, cap;
    bool overflow;
} png_out;

static void put(png_out *o, const void *p, size_t n) {
    if (o->len + n > o->cap) {
        o->overflow = true;
        return;
    }
    memcpy(o->buf + o->len, p, n);
    o->len += n;
}

static void put_be32(png_out *o, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    put(o, b, 4);
}

/* A chunk whose data is already at o->buf + start (after its length and type) */
static void end_chunk(png_out *o, size_t start) {
    if (o->overflow) return;
    uint32_t len = (uint32_t)(o->len - start);
    uint8_t *at = o->buf + start - 8;
    at[0] = (uint8_t)(len >> 24), at[1] = (uint8_t)(len >> 16), at[2] = (uint8_t)(len >> 8), at[3] = (uint8_t)len;
    put_be32(o, crc32_update(0xFFFFFFFFu, at + 4, len + 4) ^ 0xFFFFFFFFu);
}

/* int scePngEncEncode(void *handle, const param *, output *) */
static void encode(hle_call *c) {
    guest_addr h = hle_arg_ptr(c, 0), param = hle_arg_ptr(c, 1), info = hle_arg_ptr(c, 2);
    uint32_t ctx[4] = {0};
    if (!h || !gmem_read(c->mem, h, ctx, sizeof(ctx)) || ctx[0] != MAGIC) {
        hle_ret_s32(c, E_HANDLE);
        return;
    }
    uint8_t p[48];
    if (!param || !gmem_read(c->mem, param, p, sizeof(p))) {
        hle_ret_s32(c, E_ADDR);
        return;
    }
    uint64_t src_addr, dst_addr;
    uint32_t src_size, dst_size, w, h_px, pitch;
    uint16_t fmt, space, depth, clut, filters, level;
    memcpy(&src_addr, p, 8), memcpy(&dst_addr, p + 8, 8), memcpy(&src_size, p + 16, 4), memcpy(&dst_size, p + 20, 4);
    memcpy(&w, p + 24, 4), memcpy(&h_px, p + 28, 4), memcpy(&pitch, p + 32, 4);
    memcpy(&fmt, p + 36, 2), memcpy(&space, p + 38, 2), memcpy(&depth, p + 40, 2), memcpy(&clut, p + 42, 2);
    memcpy(&filters, p + 44, 2), memcpy(&level, p + 46, 2);
    int32_t e = 0;
    if (!src_addr || !dst_addr || (src_addr & 3)) e = E_ADDR;
    else if (fmt > 1 || (space != 3 && space != 19) || depth != 8 || clut || level > 9 || (filters & ~0xFu) ||
             (uint32_t)__builtin_popcount(filters) > ctx[2])
        e = E_PARAM;
    else if (!dst_size || !w || w > ctx[1] || !h_px || h_px > MAX_SIDE || pitch < w * 4 || (pitch & 3) ||
             (uint64_t)pitch * h_px > src_size)
        e = E_SIZE;
    const uint8_t *src = e ? NULL : (const uint8_t *)gmem_host(c->mem, src_addr, (uint64_t)pitch * h_px);
    uint8_t *dst = e ? NULL : (uint8_t *)gmem_host(c->mem, dst_addr, dst_size);
    if (!e && (!src || !dst)) e = E_ADDR;
    uint32_t channels = space == 19 ? 4 : 3, row = 1 + w * channels;
    uint64_t raw = (uint64_t)row * h_px;
    uint8_t *line = e ? NULL : (uint8_t *)malloc(row);
    if (!e && !line) e = E_FATAL;
    if (e) {
        free(line);
        if (info) gmem_write(c->mem, info, (uint32_t[2]){0, 0}, 8);
        hle_ret_s32(c, e);
        return;
    }
    png_out o = {dst, 0, dst_size, false};
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    put(&o, sig, 8);
    put_be32(&o, 0), put(&o, "IHDR", 4);
    size_t start = o.len;
    put_be32(&o, w), put_be32(&o, h_px);
    uint8_t ihdr[5] = {8, (uint8_t)(channels == 4 ? 6 : 2), 0, 0, 0};
    put(&o, ihdr, 5);
    end_chunk(&o, start);
    put_be32(&o, 0), put(&o, "IDAT", 4);
    start = o.len;
    static const uint8_t zhdr[2] = {0x78, 0x01};
    put(&o, zhdr, 2);
    /* the rows in stored deflate blocks of at most 65535 bytes, Adler-32 at the end */
    uint32_t a = 1, b = 0;
    uint64_t left = raw, in_block = 0;
    for (uint32_t y = 0; y < h_px && !o.overflow; y++) {
        const uint8_t *s = src + (size_t)y * pitch;
        line[0] = 0;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r = s[x * 4], g = s[x * 4 + 1], bl = s[x * 4 + 2];
            if (fmt == 1) r = s[x * 4 + 2], bl = s[x * 4];
            uint8_t *d = line + 1 + x * channels;
            d[0] = r, d[1] = g, d[2] = bl;
            if (channels == 4) d[3] = s[x * 4 + 3];
        }
        for (uint32_t i = 0; i < row && !o.overflow;) {
            if (!in_block) {
                uint32_t n = left > 65535 ? 65535 : (uint32_t)left;
                uint8_t bh[5] = {(uint8_t)(left <= 65535), (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)~n, (uint8_t)(~n >> 8)};
                put(&o, bh, 5);
                in_block = n;
            }
            uint32_t n = row - i < in_block ? row - i : (uint32_t)in_block;
            put(&o, line + i, n);
            for (uint32_t k = 0; k < n; k++) a = (a + line[i + k]) % 65521, b = (b + a) % 65521;
            i += n, in_block -= n, left -= n;
        }
    }
    put_be32(&o, b << 16 | a);
    end_chunk(&o, start);
    put_be32(&o, 0), put(&o, "IEND", 4);
    end_chunk(&o, o.len);
    free(line);
    uint32_t out_info[2] = {o.overflow ? 0 : (uint32_t)o.len, o.overflow ? 0 : h_px};
    if (info) gmem_write(c->mem, info, out_info, sizeof(out_info));
    hle_ret_s32(c, o.overflow ? E_OVERFLOW : (int32_t)o.len);
}

#define L "libScePngEnc"

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(L, "scePngEncQueryMemorySize", query_memory_size),
    HLE_FUNC_ENTRY(L, "scePngEncCreate", create),
    HLE_FUNC_ENTRY(L, "scePngEncEncode", encode),
    HLE_FUNC_ENTRY(L, "scePngEncDelete", destroy),
};

void hle_register_png_enc(void) {
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}
