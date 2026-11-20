/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceJpegDec: JPEG pictures in the game's memory to RGBA, BGRA, YUYV or Y8.
 * The structures, checks and results are as KytyPS5's libJpegDec.cpp (GPL-2.0)
 * documents them; the pictures are decoded by stb_image (third_party/stb).
 *
 * Create parameters (12 bytes): u32 size, u32 attribute (1 standard sampling,
 * 2 full sampling), u32 maximum width. The context (the game's memory): u64
 * magic, u32 attribute, u32 maximum width.
 * Parse parameters (16 bytes): +0 JPEG address, +8 u32 JPEG size, +12 u16 mode
 * (0 JPEG, 1 MJPEG), +14 u16 down scale (1, 2, 4, 8).
 * Decode parameters (48 bytes): +0 JPEG, +8 picture, +16 coefficient memory,
 * +24 u32 JPEG size, +28 u32 picture size, +32 u32 coefficient size, +36 u16
 * mode, +38 u16 down scale, +40 u16 pixel format (0 RGBA, 1 BGRA, 10 YUYV,
 * 11 Y8), +42 u16 alpha, +44 u32 pitch (0: the row size).
 * Picture information (36 bytes): u32 width, u32 height, u16 color space
 * (1 YCC, 2 gray), u16 components, u8 ids[4], u8 sampling[4], u32 coefficient
 * size, u32 suitable attribute, u32 output width, u32 output height.
 * Decode returns width << 16 | height (0 past 32767). */
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "hle/hle.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
/* zeroed allocations: a scan using a Huffman table no DHT defined (an MJPEG
 * frame) must not read uninitialised tables (as KytyPS5) */
#define STBI_MALLOC(size) calloc(1, size)
#define STBI_REALLOC(p, size) realloc(p, size)
#define STBI_FREE(p) free(p)
#include "../../third_party/stb/stb_image.h"
#pragma GCC diagnostic pop

void hle_register_table(const hle_export *table, size_t count);

#define E_ADDR ((int32_t)0x80650001)
#define E_SIZE ((int32_t)0x80650002)
#define E_PARAM ((int32_t)0x80650003)
#define E_HANDLE ((int32_t)0x80650004)
#define E_COEF_MEMORY ((int32_t)0x80650005)
#define E_WORK_MEMORY ((int32_t)0x80650006)
#define E_DATA ((int32_t)0x80650010)
#define E_UNSUPPORTED ((int32_t)0x80650011)
#define E_DECODE ((int32_t)0x80650012)

#define ATTR_STD 1
#define ATTR_FULL 2
#define FMT_RGBA 0
#define FMT_BGRA 1
#define FMT_YUYV 10
#define FMT_Y8 11
#define CS_UNKNOWN 0
#define CS_YCC 1
#define CS_GRAY 2

#define MAGIC 0x44475045434a5350ull /* "PSJCEPGD" */
#define CONTEXT_SIZE 16u

typedef struct jpeg_info {
    uint32_t width, height;
    uint16_t color_space, components;
    uint8_t ids[4], sampling[4];
    uint32_t coef_size, attribute, out_width, out_height;
} jpeg_info;
_Static_assert(sizeof(jpeg_info) == 36, "SceJpegDecImageInfo");

static int32_t check_create(hle_call *c, guest_addr param, uint32_t *attr, uint32_t *max_width) {
    uint32_t p[3];
    if (!param || !gmem_read(c->mem, param, p, sizeof(p))) return E_ADDR;
    if (p[0] != sizeof(p)) return E_SIZE;
    if ((p[1] != ATTR_STD && p[1] != ATTR_FULL) || !p[2] || p[2] > 0xFFFF) return E_PARAM;
    if (attr) *attr = p[1];
    if (max_width) *max_width = p[2];
    return 0;
}

static bool check_scale(uint16_t mode, uint16_t scale) {
    return mode <= 1 && (scale == 1 || scale == 2 || scale == 4 || scale == 8);
}

/* The frame header (T.81 B.2.2) before the first scan */
static int32_t parse_header(const uint8_t *d, uint32_t size, uint16_t scale, jpeg_info *info) {
    if (size < 4 || d[0] != 0xFF || d[1] != 0xD8) return E_DATA;
    for (uint32_t off = 2; off + 4 <= size;) {
        uint8_t marker = d[off + 1];
        if (d[off] != 0xFF || marker == 0xDA || marker == 0xD9) return E_DATA;
        if (marker == 0xFF) {
            off++;
            continue;
        }
        uint32_t len = (uint32_t)d[off + 2] << 8 | d[off + 3];
        const uint8_t *seg = d + off + 4;
        if (len < 2 || off + 2 + len > size) return E_DATA;
        off += 2 + len;
        /* SOF0-SOF15 except DHT, JPG and DAC */
        if (marker < 0xC0 || marker > 0xCF || marker == 0xC4 || marker == 0xC8 || marker == 0xCC) continue;
        uint32_t comps = len < 8 ? 0 : seg[5];
        if (!comps || len < 8 + comps * 3 || !(seg[3] | seg[4])) return E_DATA;
        memset(info, 0, sizeof(*info));
        info->height = (uint32_t)seg[1] << 8 | seg[2];
        info->width = (uint32_t)seg[3] << 8 | seg[4];
        /* baseline, extended and progressive Huffman 8-bit frames with a known height */
        if (marker > 0xC2 || seg[0] != 8 || comps > 4 || !info->height) return E_UNSUPPORTED;
        uint32_t h_max = 1, v_max = 1;
        for (uint32_t i = 0; i < comps; i++) {
            uint8_t f = seg[7 + i * 3];
            if ((f >> 4) - 1u > 3u || (f & 15) - 1u > 3u) return E_DATA;
            info->ids[i] = seg[6 + i * 3];
            info->sampling[i] = f;
            if ((uint32_t)(f >> 4) > h_max) h_max = f >> 4;
            if ((uint32_t)(f & 15) > v_max) v_max = f & 15;
        }
        info->components = (uint16_t)comps;
        if (marker == 0xC2) { /* progressive: every 16-bit coefficient of the MCU-padded picture */
            uint32_t mx = (info->width + h_max * 8 - 1) / (h_max * 8), my = (info->height + v_max * 8 - 1) / (v_max * 8);
            for (uint32_t i = 0; i < comps; i++)
                info->coef_size += mx * (info->sampling[i] >> 4) * my * (info->sampling[i] & 15) * 128u;
        }
        info->color_space = comps == 1 ? CS_GRAY : comps == 3 ? CS_YCC : CS_UNKNOWN;
        /* standard: gray, or 1x1 chroma with luma at most 2x2 (4:4:4, 4:2:2, 4:2:0) */
        bool standard = comps == 1 || (info->sampling[1] == 0x11 && info->sampling[2] == 0x11 && h_max <= 2 && v_max <= 2);
        info->attribute = standard ? ATTR_STD : ATTR_FULL;
        info->out_width = (info->width + scale - 1) / scale;
        info->out_height = (info->height + scale - 1) / scale;
        return 0;
    }
    return E_DATA;
}

static uint8_t luma(const uint8_t *p) { return (uint8_t)((77 * p[0] + 150 * p[1] + 29 * p[2] + 128) >> 8); }

/* JFIF chroma of the average of two pixels, rounding half down so a saturated pair stays 255 */
static uint8_t chroma(const uint8_t *p, const uint8_t *q, int r, int g, int b) {
    return (uint8_t)((r * (p[0] + q[0]) + g * (p[1] + q[1]) + b * (p[2] + q[2]) + 65791) >> 9);
}

/* int sceJpegDecQueryMemorySize(const param *) */
static void query_memory_size(hle_call *c) {
    int32_t e = check_create(c, hle_arg_ptr(c, 0), NULL, NULL);
    hle_ret_s32(c, e ? e : (int32_t)CONTEXT_SIZE);
}

/* int sceJpegDecCreate(const param *, void *memory, uint32_t size, void **handle) */
static void create(hle_call *c) {
    uint32_t attr = 0, max_width = 0;
    int32_t e = check_create(c, hle_arg_ptr(c, 0), &attr, &max_width);
    guest_addr mem = hle_arg_ptr(c, 1), out = hle_arg_ptr(c, 3);
    if (!e && (!mem || !out)) e = E_ADDR;
    if (!e && hle_arg_u32(c, 2) < CONTEXT_SIZE) e = E_SIZE;
    uint64_t ctx[2] = {MAGIC, (uint64_t)max_width << 32 | attr};
    if (!e && (!gmem_write(c->mem, mem, ctx, sizeof(ctx)) || !gmem_write_u64(c->mem, out, mem))) e = E_ADDR;
    hle_ret_s32(c, e);
}

static bool read_context(hle_call *c, guest_addr handle, uint32_t *attr, uint32_t *max_width) {
    uint64_t ctx[2];
    if (!handle || !gmem_read(c->mem, handle, ctx, sizeof(ctx)) || ctx[0] != MAGIC) return false;
    *attr = (uint32_t)ctx[1];
    *max_width = (uint32_t)(ctx[1] >> 32);
    return true;
}

/* int sceJpegDecDelete(void *handle) */
static void destroy(hle_call *c) {
    guest_addr h = hle_arg_ptr(c, 0);
    uint32_t attr, max_width;
    if (!read_context(c, h, &attr, &max_width)) {
        hle_ret_s32(c, E_HANDLE);
        return;
    }
    gmem_write_u64(c->mem, h, 0);
    hle_ret_s32(c, 0);
}

/* int sceJpegDecParseHeader(const parse_param *, info *) */
static void parse_header_call(hle_call *c) {
    guest_addr param = hle_arg_ptr(c, 0), info_addr = hle_arg_ptr(c, 1);
    uint8_t p[16];
    if (!param || !info_addr || !gmem_read(c->mem, param, p, sizeof(p))) {
        hle_ret_s32(c, E_ADDR);
        return;
    }
    uint64_t jpeg;
    uint32_t size;
    uint16_t mode, scale;
    memcpy(&jpeg, p, 8), memcpy(&size, p + 8, 4), memcpy(&mode, p + 12, 2), memcpy(&scale, p + 14, 2);
    int32_t e = !jpeg ? E_ADDR : !size ? E_SIZE : !check_scale(mode, scale) ? E_PARAM : 0;
    const uint8_t *data = e ? NULL : (const uint8_t *)hle_mem(c, jpeg, size);
    if (!e && !data) e = E_ADDR;
    jpeg_info info;
    if (!e) e = parse_header(data, size, scale, &info);
    if (!e && !gmem_write(c->mem, info_addr, &info, sizeof(info))) e = E_ADDR;
    hle_ret_s32(c, e);
}

/* int sceJpegDecDecode(void *handle, const decode_param *, info *) */
static void decode(hle_call *c) {
    uint32_t attr, max_width;
    if (!read_context(c, hle_arg_ptr(c, 0), &attr, &max_width)) {
        hle_ret_s32(c, E_HANDLE);
        return;
    }
    guest_addr param = hle_arg_ptr(c, 1), info_addr = hle_arg_ptr(c, 2);
    uint8_t p[48];
    if (!param || !gmem_read(c->mem, param, p, sizeof(p))) {
        hle_ret_s32(c, E_ADDR);
        return;
    }
    uint64_t jpeg, picture, coef;
    uint32_t jpeg_size, picture_size, coef_size, pitch;
    uint16_t mode, scale, format, alpha;
    memcpy(&jpeg, p, 8), memcpy(&picture, p + 8, 8), memcpy(&coef, p + 16, 8);
    memcpy(&jpeg_size, p + 24, 4), memcpy(&picture_size, p + 28, 4), memcpy(&coef_size, p + 32, 4);
    memcpy(&mode, p + 36, 2), memcpy(&scale, p + 38, 2), memcpy(&format, p + 40, 2), memcpy(&alpha, p + 42, 2);
    memcpy(&pitch, p + 44, 4);
    if (!jpeg || !picture || (format != FMT_Y8 && (picture & 3))) {
        hle_ret_s32(c, E_ADDR);
        return;
    }
    if (!jpeg_size || !picture_size) {
        hle_ret_s32(c, E_SIZE);
        return;
    }
    if (!check_scale(mode, scale) || alpha > 255 ||
        (format != FMT_RGBA && format != FMT_BGRA && format != FMT_YUYV && format != FMT_Y8) ||
        (format != FMT_Y8 && (pitch & 3))) {
        hle_ret_s32(c, E_PARAM);
        return;
    }
    const uint8_t *data = (const uint8_t *)hle_mem(c, jpeg, jpeg_size);
    if (!data) {
        hle_ret_s32(c, E_ADDR);
        return;
    }
    jpeg_info info;
    int32_t e = parse_header(data, jpeg_size, scale, &info);
    if (!e && info.color_space == CS_UNKNOWN) e = E_UNSUPPORTED;
    if (!e && (info.out_width > max_width || info.attribute > attr)) e = E_WORK_MEMORY;
    if (!e && (info.coef_size > coef_size || (info.coef_size && !coef))) e = E_COEF_MEMORY;
    uint32_t w = info.out_width, h = info.out_height;
    bool large = w > 32767 || h > 32767;
    if (!e && large && !info_addr) e = E_ADDR;
    uint32_t row = format == FMT_YUYV ? (w + 1) / 2 * 4 : format == FMT_Y8 ? w : w * 4;
    uint32_t stride = pitch ? pitch : row;
    if (!e && stride < row) e = E_PARAM;
    if (!e && (uint64_t)stride * h > picture_size) e = E_SIZE;
    uint8_t *dst = e ? NULL : (uint8_t *)hle_mem(c, picture, (uint64_t)stride * h);
    if (!e && !dst) e = E_ADDR;
    if (e) {
        hle_ret_s32(c, e);
        return;
    }
    int sw = 0, sh = 0;
    uint8_t *rgb = stbi_load_from_memory(data, (int)jpeg_size, &sw, &sh, NULL, 3);
    if (!rgb || (uint32_t)sw != info.width || (uint32_t)sh != info.height) {
        LOG_W(HLE, "JPEG %ux%u at 0x%llx: cannot decode (%s)", info.width, info.height, (unsigned long long)jpeg,
              rgb ? "size differs" : stbi_failure_reason());
        stbi_image_free(rgb);
        hle_ret_s32(c, E_DECODE);
        return;
    }
    /* each output pixel averages its scale x scale block, packed in place: an
     * output pixel never lands past a source pixel still to be read */
    if (scale > 1) {
        uint8_t *o = rgb;
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                uint32_t xe = (x + 1) * scale < info.width ? (x + 1) * scale : info.width;
                uint32_t ye = (y + 1) * scale < info.height ? (y + 1) * scale : info.height;
                uint32_t n = (xe - x * scale) * (ye - y * scale), sum[3] = {0, 0, 0};
                for (uint32_t sy = y * scale; sy < ye; sy++)
                    for (uint32_t sx = x * scale; sx < xe; sx++) {
                        const uint8_t *s = rgb + ((size_t)sy * info.width + sx) * 3;
                        sum[0] += s[0], sum[1] += s[1], sum[2] += s[2];
                    }
                for (int k = 0; k < 3; k++) *o++ = (uint8_t)((sum[k] + n / 2) / n);
            }
        }
    }
    const uint8_t *s = rgb;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *d = dst + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++, s += 3) {
            switch (format) {
            case FMT_RGBA: d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = (uint8_t)alpha, d += 4; break;
            case FMT_BGRA: d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = (uint8_t)alpha, d += 4; break;
            case FMT_Y8: *d++ = luma(s); break;
            default:
                if (!(x & 1)) { /* an odd width repeats the last pixel */
                    const uint8_t *q = x + 1 < w ? s + 3 : s;
                    d[0] = luma(s), d[1] = chroma(s, q, -43, -85, 128);
                    d[2] = luma(q), d[3] = chroma(s, q, 128, -107, -21);
                    d += 4;
                }
                break;
            }
        }
    }
    stbi_image_free(rgb);
    if (info_addr) gmem_write(c->mem, info_addr, &info, sizeof(info));
    hle_ret_s32(c, large ? 0 : (int32_t)(w << 16 | h));
}

#define L "libSceJpegDec"

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(L, "sceJpegDecQueryMemorySize", query_memory_size),
    HLE_FUNC_ENTRY(L, "sceJpegDecCreate", create),
    HLE_FUNC_ENTRY(L, "sceJpegDecDecode", decode),
    HLE_FUNC_ENTRY(L, "sceJpegDecDelete", destroy),
    HLE_FUNC_ENTRY(L, "sceJpegDecParseHeader", parse_header_call),
};

void hle_register_jpeg_dec(void) { hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports)); }
