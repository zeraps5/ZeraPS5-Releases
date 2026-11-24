/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceJpegEnc: baseline JPEG encoding of RGBA/BGRA, YUYV or Y8 images (save
 * icons, photo mode). Our own encoder: standard (JPEG Annex K) quantization and
 * Huffman tables scaled by quality, 4:4:4 / 4:2:2 / 4:2:0 chroma, restart markers.
 *
 * The parameter checks, error codes, structure layouts and the compression ratio
 * to quality mapping follow the AnyPS5 project's libSceJpegEnc (boykopovar/AnyPS5,
 * GPL-2.0):
 *   CreateParam { u32 size (8); u32 attr (0) }
 *   EncodeParam { void *image; void *jpeg; u32 imageSize, jpegSize, width, height, pitch;
 *                 u16 pixelFormat, encodeMode, colorSpace; u8 sampling, ratio; i32 restart } (0x30)
 *   OutputInfo { u32 size; u32 height } */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "hle/hle.h"

void hle_register_table(const hle_export *table, size_t count);

#define JPEGENC_ERR_INVALID_ADDR ((int32_t)0x80650101)
#define JPEGENC_ERR_INVALID_SIZE ((int32_t)0x80650102)
#define JPEGENC_ERR_INVALID_PARAM ((int32_t)0x80650103)
#define JPEGENC_ERR_INVALID_HANDLE ((int32_t)0x80650104)

#define MEMORY_SIZE 0x800u
#define HANDLE_ALIGN 0x20u

enum { PF_RGBA = 0, PF_BGRA = 1, PF_YUYV = 10, PF_Y8 = 11 };
enum { SAMPLING_444 = 0, SAMPLING_422 = 1, SAMPLING_420 = 2 };

/* ---- tables ---- */

static const uint8_t k_zigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                     12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                     35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                     58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
static const uint8_t k_quant_luma[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                                         14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                                         18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                                         49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
static const uint8_t k_quant_chroma[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
                                           24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                                           99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                           99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

static const uint8_t k_dc_luma_bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t k_dc_chroma_bits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t k_dc_vals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t k_ac_luma_bits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const uint8_t k_ac_luma_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14,
    0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09,
    0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65,
    0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
    0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
    0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
    0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
static const uint8_t k_ac_chroma_bits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const uint8_t k_ac_chroma_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32,
    0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16,
    0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
    0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86,
    0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8,
    0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

typedef struct huff {
    uint16_t code[256];
    uint8_t len[256];
} huff;

static void huff_build(huff *h, const uint8_t bits[16], const uint8_t *vals) {
    memset(h, 0, sizeof(*h));
    uint16_t code = 0;
    int k = 0;
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < bits[l - 1]; i++, k++) {
            h->code[vals[k]] = code++;
            h->len[vals[k]] = (uint8_t)l;
        }
        code <<= 1;
    }
}

/* ---- output ---- */

typedef struct jw {
    uint8_t *p;
    size_t n, cap;
    bool fail;
    uint32_t bitbuf;
    int bitcnt;
} jw;

static void put_byte(jw *w, uint8_t b) {
    if (w->n == w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 65536;
        uint8_t *p = (uint8_t *)realloc(w->p, cap);
        if (!p) {
            w->fail = true;
            return;
        }
        w->p = p, w->cap = cap;
    }
    w->p[w->n++] = b;
}
static void put_u16(jw *w, uint16_t v) {
    put_byte(w, (uint8_t)(v >> 8));
    put_byte(w, (uint8_t)v);
}
static void put_bits(jw *w, uint32_t bits, int count) {
    for (int i = count - 1; i >= 0; i--) {
        w->bitbuf = w->bitbuf << 1 | ((bits >> i) & 1u);
        if (++w->bitcnt == 8) {
            put_byte(w, (uint8_t)w->bitbuf);
            if ((uint8_t)w->bitbuf == 0xFF) put_byte(w, 0); /* byte stuffing */
            w->bitbuf = 0, w->bitcnt = 0;
        }
    }
}
static void flush_bits(jw *w) {
    while (w->bitcnt) put_bits(w, 1, 1); /* pad with ones */
}

/* ---- transform ---- */

static float g_cos[8][8]; /* cos((2x + 1) u pi / 16) * c(u) / 2 */

static void fdct_quant(const float in[64], const float qinv[64], int out[64]) {
    float tmp[64];
    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++) {
            float s = 0;
            for (int x = 0; x < 8; x++) s += in[y * 8 + x] * g_cos[u][x];
            tmp[y * 8 + u] = s;
        }
    for (int u = 0; u < 8; u++)
        for (int v = 0; v < 8; v++) {
            float s = 0;
            for (int y = 0; y < 8; y++) s += tmp[y * 8 + u] * g_cos[v][y];
            out[v * 8 + u] = (int)lroundf(s * qinv[v * 8 + u]);
        }
}

static int bit_size(int v) {
    if (v < 0) v = -v;
    int n = 0;
    while (v) n++, v >>= 1;
    return n;
}

static void encode_block(jw *w, const float px[64], const float qinv[64], int *dc_pred, const huff *dc,
                         const huff *ac) {
    int coef[64];
    fdct_quant(px, qinv, coef);
    int diff = coef[0] - *dc_pred;
    *dc_pred = coef[0];
    int n = bit_size(diff);
    put_bits(w, dc->code[n], dc->len[n]);
    if (n) put_bits(w, (uint32_t)(diff < 0 ? diff - 1 : diff) & ((1u << n) - 1), n);
    int run = 0;
    for (int k = 1; k < 64; k++) {
        int v = coef[k_zigzag[k]];
        if (!v) {
            run++;
            continue;
        }
        while (run > 15) {
            put_bits(w, ac->code[0xF0], ac->len[0xF0]);
            run -= 16;
        }
        n = bit_size(v);
        int sym = run << 4 | n;
        put_bits(w, ac->code[sym], ac->len[sym]);
        put_bits(w, (uint32_t)(v < 0 ? v - 1 : v) & ((1u << n) - 1), n);
        run = 0;
    }
    if (run) put_bits(w, ac->code[0], ac->len[0]);
}

static void write_dht(jw *w, int class_id, const uint8_t bits[16], const uint8_t *vals) {
    int count = 0;
    for (int i = 0; i < 16; i++) count += bits[i];
    put_u16(w, 0xFFC4);
    put_u16(w, (uint16_t)(2 + 1 + 16 + count));
    put_byte(w, (uint8_t)class_id);
    for (int i = 0; i < 16; i++) put_byte(w, bits[i]);
    for (int i = 0; i < count; i++) put_byte(w, vals[i]);
}

/* Planes of one image: Y, Cb, Cr (0..255) at full resolution */
typedef struct planes {
    uint32_t w, h;
    int comps;
    uint8_t *p[3];
} planes;

static float sample(const planes *pl, int c, int64_t x, int64_t y) {
    if (x >= pl->w) x = pl->w - 1;
    if (y >= pl->h) y = pl->h - 1;
    return pl->p[c][(size_t)y * pl->w + (size_t)x];
}

/* Encodes; returns the JPEG (malloc) or NULL */
static uint8_t *jpeg_encode(const planes *pl, int quality, int sampling, int32_t restart, size_t *out_len) {
    static bool cos_ready;
    if (!cos_ready) {
        for (int u = 0; u < 8; u++)
            for (int x = 0; x < 8; x++)
                g_cos[u][x] = (float)(cos((2 * x + 1) * u * 3.14159265358979 / 16) * (u ? 0.5 : 0.5 / sqrt(2.0)));
        cos_ready = true;
    }
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    uint8_t q[2][64];
    float qinv[2][64];
    for (int i = 0; i < 64; i++) {
        for (int t = 0; t < 2; t++) {
            int v = ((t ? k_quant_chroma : k_quant_luma)[i] * scale + 50) / 100;
            q[t][i] = (uint8_t)(v < 1 ? 1 : v > 255 ? 255 : v);
            qinv[t][i] = 1.0f / q[t][i];
        }
    }
    huff dcl, acl, dcc, acc;
    huff_build(&dcl, k_dc_luma_bits, k_dc_vals);
    huff_build(&acl, k_ac_luma_bits, k_ac_luma_vals);
    huff_build(&dcc, k_dc_chroma_bits, k_dc_vals);
    huff_build(&acc, k_ac_chroma_bits, k_ac_chroma_vals);

    int hs = pl->comps == 1 || sampling == SAMPLING_444 ? 1 : 2;
    int vs = pl->comps == 1 || sampling != SAMPLING_420 ? 1 : 2;
    uint32_t mcu_w = 8u * hs, mcu_h = 8u * vs;
    uint32_t mcus_x = (pl->w + mcu_w - 1) / mcu_w, mcus_y = (pl->h + mcu_h - 1) / mcu_h;
    uint32_t interval = restart < 0 ? mcus_x : (uint32_t)restart;
    if (interval > 0xFFFF) interval = 0xFFFF;

    jw w = {0};
    put_u16(&w, 0xFFD8);
    /* JFIF APP0 */
    put_u16(&w, 0xFFE0);
    put_u16(&w, 16);
    static const uint8_t jfif[14] = {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    for (int i = 0; i < 14; i++) put_byte(&w, jfif[i]);
    for (int t = 0; t < (pl->comps == 1 ? 1 : 2); t++) {
        put_u16(&w, 0xFFDB);
        put_u16(&w, 67);
        put_byte(&w, (uint8_t)t);
        for (int i = 0; i < 64; i++) put_byte(&w, q[t][k_zigzag[i]]);
    }
    put_u16(&w, 0xFFC0);
    put_u16(&w, (uint16_t)(8 + 3 * pl->comps));
    put_byte(&w, 8);
    put_u16(&w, (uint16_t)pl->h);
    put_u16(&w, (uint16_t)pl->w);
    put_byte(&w, (uint8_t)pl->comps);
    for (int c = 0; c < pl->comps; c++) {
        put_byte(&w, (uint8_t)(c + 1));
        put_byte(&w, (uint8_t)(c ? 0x11 : (hs << 4 | vs)));
        put_byte(&w, (uint8_t)(c ? 1 : 0));
    }
    write_dht(&w, 0x00, k_dc_luma_bits, k_dc_vals);
    write_dht(&w, 0x10, k_ac_luma_bits, k_ac_luma_vals);
    if (pl->comps == 3) {
        write_dht(&w, 0x01, k_dc_chroma_bits, k_dc_vals);
        write_dht(&w, 0x11, k_ac_chroma_bits, k_ac_chroma_vals);
    }
    if (interval) {
        put_u16(&w, 0xFFDD);
        put_u16(&w, 4);
        put_u16(&w, (uint16_t)interval);
    }
    put_u16(&w, 0xFFDA);
    put_u16(&w, (uint16_t)(6 + 2 * pl->comps));
    put_byte(&w, (uint8_t)pl->comps);
    for (int c = 0; c < pl->comps; c++) {
        put_byte(&w, (uint8_t)(c + 1));
        put_byte(&w, (uint8_t)(c ? 0x11 : 0x00));
    }
    put_byte(&w, 0);
    put_byte(&w, 63);
    put_byte(&w, 0);

    int pred[3] = {0, 0, 0};
    uint32_t mcu_index = 0, rst = 0, total = mcus_x * mcus_y;
    float blk[64];
    for (uint32_t my = 0; my < mcus_y && !w.fail; my++) {
        for (uint32_t mx = 0; mx < mcus_x && !w.fail; mx++) {
            if (interval && mcu_index && mcu_index % interval == 0) {
                flush_bits(&w);
                put_u16(&w, (uint16_t)(0xFFD0 + (rst++ & 7)));
                pred[0] = pred[1] = pred[2] = 0;
            }
            int64_t x0 = (int64_t)mx * mcu_w, y0 = (int64_t)my * mcu_h;
            for (int by = 0; by < vs; by++)
                for (int bx = 0; bx < hs; bx++) {
                    for (int y = 0; y < 8; y++)
                        for (int x = 0; x < 8; x++)
                            blk[y * 8 + x] = sample(pl, 0, x0 + bx * 8 + x, y0 + by * 8 + y) - 128.0f;
                    encode_block(&w, blk, qinv[0], &pred[0], &dcl, &acl);
                }
            for (int c = 1; c < pl->comps; c++) {
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) {
                        float s = 0;
                        for (int sy = 0; sy < vs; sy++)
                            for (int sx = 0; sx < hs; sx++)
                                s += sample(pl, c, x0 + (int64_t)x * hs + sx, y0 + (int64_t)y * vs + sy);
                        blk[y * 8 + x] = s / (float)(hs * vs) - 128.0f;
                    }
                encode_block(&w, blk, qinv[1], &pred[c], &dcc, &acc);
            }
            mcu_index++;
        }
    }
    (void)total;
    flush_bits(&w);
    put_u16(&w, 0xFFD9);
    if (w.fail) {
        free(w.p);
        return NULL;
    }
    *out_len = w.n;
    return w.p;
}

/* ---- the library ---- */

static int32_t check_create_param(hle_call *c, guest_addr param) {
    const uint32_t *p = param ? (const uint32_t *)hle_mem(c, param, 8) : NULL;
    if (!p) return JPEGENC_ERR_INVALID_ADDR;
    if (p[0] != 8) return JPEGENC_ERR_INVALID_SIZE;
    if (p[1] != 0) return JPEGENC_ERR_INVALID_PARAM;
    return 0;
}

/* int sceJpegEncQueryMemorySize(const CreateParam *) */
static void jpegenc_query_memory_size(hle_call *c) {
    int32_t r = check_create_param(c, hle_arg_ptr(c, 0));
    hle_ret_s32(c, r ? r : (int32_t)MEMORY_SIZE);
}

/* The handle is an aligned address in the game's memory that holds its own address */
static bool handle_valid(hle_call *c, guest_addr h) {
    const uint64_t *self = h && h % HANDLE_ALIGN == 0 ? (const uint64_t *)hle_mem(c, h, 8) : NULL;
    return self && *self == h;
}

/* int sceJpegEncCreate(const CreateParam *, void *memory, u32 memorySize, void **handle) */
static void jpegenc_create(hle_call *c) {
    int32_t r = check_create_param(c, hle_arg_ptr(c, 0));
    guest_addr mem = hle_arg_ptr(c, 1), out = hle_arg_ptr(c, 3);
    if (!r && !mem) r = JPEGENC_ERR_INVALID_ADDR;
    if (!r && hle_arg_u32(c, 2) < MEMORY_SIZE) r = JPEGENC_ERR_INVALID_SIZE;
    uint64_t *handle_out = !r && out ? (uint64_t *)hle_mem(c, out, 8) : NULL;
    if (!r && !handle_out) r = JPEGENC_ERR_INVALID_ADDR;
    if (r) {
        hle_ret_s32(c, r);
        return;
    }
    guest_addr h = (mem + HANDLE_ALIGN - 1) & ~(guest_addr)(HANDLE_ALIGN - 1);
    uint64_t *self = (uint64_t *)hle_mem(c, h, 8);
    if (!self) {
        hle_ret_s32(c, JPEGENC_ERR_INVALID_ADDR);
        return;
    }
    *self = h;
    *handle_out = h;
    hle_ret_s32(c, 0);
}

/* int sceJpegEncDelete(void *handle) */
static void jpegenc_delete(hle_call *c) {
    guest_addr h = hle_arg_ptr(c, 0);
    if (!handle_valid(c, h)) {
        hle_ret_s32(c, JPEGENC_ERR_INVALID_HANDLE);
        return;
    }
    *(uint64_t *)hle_mem(c, h, 8) = 0;
    hle_ret_s32(c, 0);
}

static uint8_t clamp_byte(float v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f); }

/* int sceJpegEncEncode(void *handle, const EncodeParam *, OutputInfo *): the JPEG size */
static void jpegenc_encode(hle_call *c) {
    if (!handle_valid(c, hle_arg_ptr(c, 0))) {
        hle_ret_s32(c, JPEGENC_ERR_INVALID_HANDLE);
        return;
    }
    const uint8_t *pp = hle_arg_ptr(c, 1) ? (const uint8_t *)hle_mem(c, hle_arg_ptr(c, 1), 0x30) : NULL;
    if (!pp) {
        hle_ret_s32(c, JPEGENC_ERR_INVALID_ADDR);
        return;
    }
    uint64_t image, jpeg;
    uint32_t image_size, jpeg_size, width, height, pitch;
    uint16_t format, mode, space;
    uint8_t sampling, ratio;
    int32_t restart;
    memcpy(&image, pp, 8), memcpy(&jpeg, pp + 8, 8);
    memcpy(&image_size, pp + 0x10, 4), memcpy(&jpeg_size, pp + 0x14, 4);
    memcpy(&width, pp + 0x18, 4), memcpy(&height, pp + 0x1C, 4), memcpy(&pitch, pp + 0x20, 4);
    memcpy(&format, pp + 0x24, 2), memcpy(&mode, pp + 0x26, 2), memcpy(&space, pp + 0x28, 2);
    sampling = pp[0x2A], ratio = pp[0x2B];
    memcpy(&restart, pp + 0x2C, 4);

    bool gray = format == PF_Y8;
    int32_t r = 0;
    if (!image || (!gray && image % 4) || !jpeg) r = JPEGENC_ERR_INVALID_ADDR;
    else if (!image_size || !jpeg_size) r = JPEGENC_ERR_INVALID_SIZE;
    else if (!width || !height || width > 0xFFFF || height > 0xFFFF || !pitch || pitch > 0xFFFFFFF ||
             (!gray && pitch % 4) || (uint64_t)height * pitch > 0x7FFFFFFF || (uint64_t)height * pitch > image_size ||
             mode > 1 || (space != 1 && space != 2) || sampling > 2 || restart < -1 || restart > 0xFFFF)
        r = JPEGENC_ERR_INVALID_PARAM;
    else if (format == PF_RGBA || format == PF_BGRA) {
        if (pitch / 4 < width || space != 1 || sampling == SAMPLING_444) r = JPEGENC_ERR_INVALID_PARAM;
    } else if (format == PF_YUYV) {
        if (pitch / 2 < ((width + 1) & ~1u) || space != 1 || sampling == SAMPLING_444) r = JPEGENC_ERR_INVALID_PARAM;
    } else if (gray) {
        if (pitch < width || space != 2 || sampling != SAMPLING_444) r = JPEGENC_ERR_INVALID_PARAM;
    } else
        r = JPEGENC_ERR_INVALID_PARAM;
    const uint8_t *src = r ? NULL : (const uint8_t *)hle_mem(c, image, (uint64_t)height * pitch);
    uint8_t *dst = r ? NULL : (uint8_t *)hle_mem(c, jpeg, jpeg_size);
    if (!r && (!src || !dst)) r = JPEGENC_ERR_INVALID_ADDR;
    if (!r && mode == 1) {
        static int logged;
        if (logged++ < 2) LOG_W(HLE, "sceJpegEncEncode: MJPEG mode is not implemented");
        r = JPEGENC_ERR_INVALID_PARAM;
    }
    if (r) {
        hle_ret_s32(c, r);
        return;
    }

    planes pl = {width, height, gray ? 1 : 3, {NULL, NULL, NULL}};
    size_t n = (size_t)width * height;
    for (int i = 0; i < pl.comps; i++) pl.p[i] = (uint8_t *)malloc(n);
    bool ok = pl.p[0] && (gray || (pl.p[1] && pl.p[2]));
    for (uint32_t y = 0; ok && y < height; y++) {
        const uint8_t *row = src + (size_t)y * pitch;
        for (uint32_t x = 0; x < width; x++) {
            size_t i = (size_t)y * width + x;
            if (gray) {
                pl.p[0][i] = row[x];
            } else if (format == PF_YUYV) {
                const uint8_t *pair = row + (x / 2) * 4;
                pl.p[0][i] = pair[(x % 2) * 2], pl.p[1][i] = pair[1], pl.p[2][i] = pair[3];
            } else {
                float R = row[x * 4 + (format == PF_BGRA ? 2 : 0)], G = row[x * 4 + 1],
                      B = row[x * 4 + (format == PF_BGRA ? 0 : 2)];
                pl.p[0][i] = clamp_byte(0.299f * R + 0.587f * G + 0.114f * B);
                pl.p[1][i] = clamp_byte(-0.168736f * R - 0.331264f * G + 0.5f * B + 128.0f);
                pl.p[2][i] = clamp_byte(0.5f * R - 0.418688f * G - 0.081312f * B + 128.0f);
            }
        }
    }
    size_t len = 0;
    uint8_t *out = ok ? jpeg_encode(&pl, 100 - ratio * 99 / 255, sampling, restart, &len) : NULL;
    for (int i = 0; i < 3; i++) free(pl.p[i]);
    if (!out) {
        hle_ret_s32(c, JPEGENC_ERR_INVALID_SIZE);
        return;
    }
    if (len > jpeg_size) {
        free(out);
        hle_ret_s32(c, JPEGENC_ERR_INVALID_SIZE);
        return;
    }
    memcpy(dst, out, len);
    free(out);
    uint32_t info[2] = {(uint32_t)len, height};
    if (hle_arg_ptr(c, 2)) gmem_write(c->mem, hle_arg_ptr(c, 2), info, sizeof(info));
    hle_ret_s32(c, (int32_t)len);
}

void hle_register_jpeg_enc(void) {
    static const hle_export table[] = {
        HLE_FUNC_ENTRY("libSceJpegEnc", "sceJpegEncQueryMemorySize", jpegenc_query_memory_size),
        HLE_FUNC_ENTRY("libSceJpegEnc", "sceJpegEncCreate", jpegenc_create),
        HLE_FUNC_ENTRY("libSceJpegEnc", "sceJpegEncDelete", jpegenc_delete),
        HLE_FUNC_ENTRY("libSceJpegEnc", "sceJpegEncEncode", jpegenc_encode),
    };
    hle_register_table(table, PS5C_ARRAY_COUNT(table));
}
