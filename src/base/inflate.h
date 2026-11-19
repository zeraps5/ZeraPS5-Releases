/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DEFLATE (RFC 1951) and zlib (RFC 1950) decoding into a caller's buffer.
 * Small and self-contained: PS4 package images (PFSC blocks of 64 KiB). */
#ifndef PS5C_INFLATE_H
#define PS5C_INFLATE_H

#include <stddef.h>
#include <stdint.h>

/* Decodes a raw DEFLATE stream. Returns the bytes written, or -1 when the
 * stream is malformed or does not fit in `cap`. */
long inflate_raw(const uint8_t *src, size_t len, uint8_t *dst, size_t cap);
/* The same behind a zlib header (no preset dictionary); the Adler-32
 * trailer is not checked. */
long inflate_zlib(const uint8_t *src, size_t len, uint8_t *dst, size_t cap);

#endif
