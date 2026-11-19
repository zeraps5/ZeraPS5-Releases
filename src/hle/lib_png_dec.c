/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libScePngDec: PNG pictures in the game's memory to RGBA (title cards, menus,
 * Jurassic Park Classic Games Collection). The structures, checks and results are
 * as KytyPS5's libPngDec.cpp (GPL-2.0) documents them; the decoder is ours
 * (hle/png_decode.c).
 *
 * Create parameters (12 bytes): u32 size, u32 attribute (0, or 1: 16-bit output),
 * u32 maximum width. The context (the game's memory): u64 magic.
 * Parse parameters (16 bytes): +0 PNG address, +8 u32 PNG size, +12 u32 reserved (0).
 * Decode parameters (32 bytes): +0 PNG address, +8 picture address, +16 u32 PNG size,
 * +20 u32 picture size, +24 u16 pixel format (0 RGBA, 1 BGRA), +26 u16 alpha for
 * pictures without one, +28 u32 pitch (bytes, 0: width * 4).
 * Picture information (16 bytes): u32 width, u32 height, u16 color space (2 gray,
 * 3 RGB, 4 palette, 18 gray + alpha, 19 RGBA), u16 bit depth, u32 flags (1 Adam7,
 * 2 tRNS). Decode returns width << 16 | height. */
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "hle/hle.h"
#include "hle/png_decode.h"

void hle_register_table(const hle_export *table, size_t count);

#define E_ADDR ((int32_t)0x80690001)
#define E_SIZE ((int32_t)0x80690002)
#define E_PARAM ((int32_t)0x80690003)
#define E_HANDLE ((int32_t)0x80690004)
#define E_WORK_MEMORY ((int32_t)0x80690005)
#define E_DATA ((int32_t)0x80690010)
#define E_DECODE ((int32_t)0x80690012)
#define MAGIC 0x44474e5043355350ull /* "PS5CPNGD" */
#define CONTEXT_SIZE 16u

static int32_t check_create(hle_call *c, guest_addr param) {
    uint32_t p[3] = {0};
    if (!param || !gmem_read(c->mem, param, p, sizeof(p))) return E_PARAM;
    if (p[1] > 1) return E_PARAM;
    if (!p[2]) return E_SIZE;
    return 0;
}

/* int scePngDecQueryMemorySize(const param *) */
static void query_memory_size(hle_call *c) {
    int32_t e = check_create(c, hle_arg_ptr(c, 0));
    hle_ret_s32(c, e ? e : (int32_t)CONTEXT_SIZE);
}

/* int scePngDecCreate(const param *, void *memory, uint32_t size, void **handle) */
static void create(hle_call *c) {
    int32_t e = check_create(c, hle_arg_ptr(c, 0));
    guest_addr mem = hle_arg_ptr(c, 1), out = hle_arg_ptr(c, 3);
    if (!e && !out) e = E_PARAM;
    if (!e && !mem) e = E_ADDR;
    if (!e && hle_arg_u32(c, 2) < CONTEXT_SIZE) e = E_WORK_MEMORY;
    uint64_t ctx[2] = {MAGIC, 0};
    if (!e && (!gmem_write(c->mem, mem, ctx, sizeof(ctx)) || !gmem_write_u64(c->mem, out, mem))) e = E_ADDR;
    hle_ret_s32(c, e);
}

static bool valid_handle(hle_call *c, guest_addr handle) {
    uint64_t magic = 0;
    return handle && gmem_read_u64(c->mem, handle, &magic) && magic == MAGIC;
}

/* int scePngDecDelete(void *handle) */
static void destroy(hle_call *c) {
    guest_addr handle = hle_arg_ptr(c, 0);
    if (!valid_handle(c, handle)) {
        hle_ret_s32(c, E_HANDLE);
        return;
    }
    gmem_write_u64(c->mem, handle, 0);
    hle_ret_s32(c, 0);
}

static void write_info(hle_call *c, guest_addr info, const png_header *h) {
    static const uint16_t spaces[7] = {2, 0, 3, 4, 18, 0, 19};
    uint8_t out[16];
    uint16_t space = h->color_type < 7 ? spaces[h->color_type] : 0, depth = h->bit_depth;
    uint32_t flags = (h->interlace ? 1u : 0u) | (h->has_trns ? 2u : 0u);
    memcpy(out, &h->width, 4), memcpy(out + 4, &h->height, 4), memcpy(out + 8, &space, 2), memcpy(out + 10, &depth, 2),
        memcpy(out + 12, &flags, 4);
    gmem_write(c->mem, info, out, sizeof(out));
}

/* int scePngDecParseHeader(const parse param *, info *) */
static void parse_header(hle_call *c) {
    guest_addr param = hle_arg_ptr(c, 0), info = hle_arg_ptr(c, 1);
    uint8_t p[16];
    if (!param || !info || !gmem_read(c->mem, param, p, sizeof(p))) {
        hle_ret_s32(c, E_PARAM);
        return;
    }
    uint64_t png;
    uint32_t size, reserved;
    memcpy(&png, p, 8), memcpy(&size, p + 8, 4), memcpy(&reserved, p + 12, 4);
    if (!png) {
        hle_ret_s32(c, E_ADDR);
        return;
    }
    if (reserved) {
        hle_ret_s32(c, E_PARAM);
        return;
    }
    const uint8_t *data = size ? (const uint8_t *)hle_mem(c, png, size) : NULL;
    png_header h;
    if (!data || !png_parse_header(data, size, &h)) {
        hle_ret_s32(c, E_DATA);
        return;
    }
    write_info(c, info, &h);
    hle_ret_s32(c, 0);
}

/* int scePngDecDecode(void *handle, const decode param *, info *) */
static void decode(hle_call *c) {
    guest_addr handle = hle_arg_ptr(c, 0), param = hle_arg_ptr(c, 1), info = hle_arg_ptr(c, 2);
    if (!valid_handle(c, handle)) {
        hle_ret_s32(c, E_HANDLE);
        return;
    }
    uint8_t p[32];
    if (!param || !gmem_read(c->mem, param, p, sizeof(p))) {
        hle_ret_s32(c, E_PARAM);
        return;
    }
    uint64_t png, picture;
    uint32_t png_size, picture_size, pitch;
    uint16_t format, alpha;
    memcpy(&png, p, 8), memcpy(&picture, p + 8, 8), memcpy(&png_size, p + 16, 4), memcpy(&picture_size, p + 20, 4);
    memcpy(&format, p + 24, 2), memcpy(&alpha, p + 26, 2), memcpy(&pitch, p + 28, 4);
    if (!png || !picture) {
        hle_ret_s32(c, E_ADDR);
        return;
    }
    if (format > 1) {
        hle_ret_s32(c, E_PARAM);
        return;
    }
    const uint8_t *data = png_size ? (const uint8_t *)hle_mem(c, png, png_size) : NULL;
    png_header h;
    if (!data || !png_parse_header(data, png_size, &h)) {
        hle_ret_s32(c, E_DATA);
        return;
    }
    uint64_t row = (uint64_t)h.width * 4, stride = pitch ? pitch : row;
    uint64_t need = stride * (h.height - 1) + row;
    uint8_t *out = stride >= row && need <= picture_size ? (uint8_t *)hle_mem(c, picture, need) : NULL;
    if (!out) {
        hle_ret_s32(c, E_SIZE);
        return;
    }
    if (info) write_info(c, info, &h);
    if (!png_decode_rgba(data, png_size, out, (size_t)stride, format == 1, (uint8_t)(alpha > 255 ? 255 : alpha))) {
        LOG_W(HLE, "PNG %ux%u (type %u, %u bits%s) at 0x%llx: invalid data", h.width, h.height, h.color_type, h.bit_depth,
              h.interlace ? ", interlaced" : "", (unsigned long long)png);
        hle_ret_s32(c, E_DECODE);
        return;
    }
    hle_ret_s32(c, h.width > 32767 || h.height > 32767 ? 0 : (int32_t)(h.width << 16 | h.height));
}

#define L "libScePngDec"

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(L, "scePngDecQueryMemorySize", query_memory_size),
    HLE_FUNC_ENTRY(L, "scePngDecCreate", create),
    HLE_FUNC_ENTRY(L, "scePngDecDecode", decode),
    HLE_FUNC_ENTRY(L, "scePngDecDelete", destroy),
    HLE_FUNC_ENTRY(L, "scePngDecParseHeader", parse_header),
};

void hle_register_png_dec(void) {
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}
