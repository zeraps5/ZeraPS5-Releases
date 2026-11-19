/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RSA private-key operation (m = c^d mod n) and PKCS #1 v1.5 unpadding, for
 * the key blobs of PS4 packages. Not constant time: it only ever runs on
 * package metadata with keys the user supplies. */
#ifndef PS5C_RSA_H
#define PS5C_RSA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RSA_MAX_BYTES 512 /* 4096-bit moduli */

/* Big-endian n, d and c of `len` bytes (n's length; d and c are padded to
 * it). out receives len bytes. False when len is out of range, n is even or
 * c >= n. */
bool rsa_private(const uint8_t *n, const uint8_t *d, const uint8_t *c, size_t len, uint8_t *out);

/* The message inside an RSAES-PKCS1-v1_5 block (00 02 PS 00 M, PS at least
 * 8 nonzero bytes): its offset in `block`, or -1 when the padding is wrong. */
long rsa_pkcs1_unpad(const uint8_t *block, size_t len);

#endif
