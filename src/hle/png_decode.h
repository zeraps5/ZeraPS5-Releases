/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PNG pictures to 8-bit RGBA (libScePngDec): every color type and bit depth,
 * palettes, tRNS and Adam7 interlacing, with our own inflate. */
#ifndef PS5C_HLE_PNG_DECODE_H
#define PS5C_HLE_PNG_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct png_header {
    uint32_t width, height;
    uint8_t bit_depth, color_type, interlace;
    bool has_trns;     /* a tRNS chunk before the image data */
} png_header;

/* Signature, IHDR and the chunks up to the first IDAT. */
bool png_parse_header(const uint8_t *data, size_t size, png_header *out);

/* Decodes into `out` (height rows of width * 4 bytes, `pitch` apart): R, G, B, A
 * (or B, G, R, A). Pictures without alpha (no alpha channel and no tRNS) get
 * `alpha` in every pixel. False: invalid or truncated data. */
bool png_decode_rgba(const uint8_t *data, size_t size, uint8_t *out, size_t pitch, bool bgra, uint8_t alpha);

/* zlib stream (RFC 1950/1951) into out[cap]; *out_len the bytes written.
 * False: invalid data or more than cap bytes. */
bool png_inflate_zlib(const uint8_t *in, size_t len, uint8_t *out, size_t cap, size_t *out_len);

#endif
