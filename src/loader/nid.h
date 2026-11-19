/* SPDX-License-Identifier: GPL-2.0-or-later */
/* NIDs: PS4/PS5 modules identify symbols by a 64-bit hash of the name
 * (first 8 bytes of SHA1(name || salt), read little-endian), written as 11
 * characters of a base64 variant. Imported/exported symbol names have the
 * form "NNNNNNNNNNN#L#M" where L is the library id and M the module id,
 * encoded with the same alphabet. */
#ifndef PS5C_NID_H
#define PS5C_NID_H

#include "base/base.h"

#define NID_STR_LEN 11

uint64_t nid_hash_name(const char *name);
void nid_to_str(uint64_t nid, char out[NID_STR_LEN + 1]);
/* `s` must be exactly NID_STR_LEN valid characters. */
bool nid_from_str(const char *s, size_t len, uint64_t *out);

/* Library/module ids: 1 to 3 characters. */
bool nid_decode_id(const char *s, size_t len, uint16_t *out);
void nid_encode_id(uint16_t id, char out[4]);

typedef struct nid_symbol {
    uint64_t nid;
    uint16_t lib_id;
    uint16_t mod_id;
} nid_symbol;

bool nid_parse_symbol(const char *s, nid_symbol *out);

#endif
