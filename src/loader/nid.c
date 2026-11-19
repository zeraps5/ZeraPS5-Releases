/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "loader/nid.h"

#include <string.h>

#include "base/endian.h"
#include "base/sha1.h"

static const char k_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";

static const uint8_t k_nid_salt[16] = {0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1,
                                       0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30};

static int alphabet_index(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '-') return 63;
    return -1;
}

uint64_t nid_hash_name(const char *name) {
    sha1_ctx ctx;
    uint8_t digest[20];
    sha1_init(&ctx);
    sha1_update(&ctx, name, strlen(name));
    sha1_update(&ctx, k_nid_salt, sizeof(k_nid_salt));
    sha1_final(&ctx, digest);
    return rd_le64(digest);
}

/* The 64 bits are written most significant first: 10 characters of 6 bits,
 * then the low 4 bits shifted into the top of the 11th character. */
void nid_to_str(uint64_t nid, char out[NID_STR_LEN + 1]) {
    for (int i = 0; i < 10; i++) {
        out[i] = k_alphabet[(nid >> (58 - i * 6)) & 0x3F];
    }
    out[10] = k_alphabet[(nid & 0xF) << 2];
    out[NID_STR_LEN] = '\0';
}

bool nid_from_str(const char *s, size_t len, uint64_t *out) {
    if (len != NID_STR_LEN) {
        return false;
    }
    uint64_t v = 0;
    for (int i = 0; i < 10; i++) {
        int x = alphabet_index(s[i]);
        if (x < 0) {
            return false;
        }
        v |= (uint64_t)x << (58 - i * 6);
    }
    int last = alphabet_index(s[10]);
    if (last < 0 || (last & 3) != 0) {
        return false;
    }
    *out = v | (uint64_t)(last >> 2);
    return true;
}

bool nid_decode_id(const char *s, size_t len, uint16_t *out) {
    if (len == 0 || len > 3) {
        return false;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < len; i++) {
        int x = alphabet_index(s[i]);
        if (x < 0) {
            return false;
        }
        v = (v << 6) | (uint32_t)x;
    }
    if (v > 0xFFFF) {
        return false;
    }
    *out = (uint16_t)v;
    return true;
}

void nid_encode_id(uint16_t id, char out[4]) {
    size_t n = 0;
    if (id >= 0x1000) {
        out[n++] = k_alphabet[(id >> 12) & 0x3F];
    }
    if (id >= 0x40) {
        out[n++] = k_alphabet[(id >> 6) & 0x3F];
    }
    out[n++] = k_alphabet[id & 0x3F];
    out[n] = '\0';
}

bool nid_parse_symbol(const char *s, nid_symbol *out) {
    const char *h1 = strchr(s, '#');
    if (!h1 || (size_t)(h1 - s) != NID_STR_LEN) {
        return false;
    }
    const char *h2 = strchr(h1 + 1, '#');
    if (!h2) {
        return false;
    }
    return nid_from_str(s, NID_STR_LEN, &out->nid) &&
           nid_decode_id(h1 + 1, (size_t)(h2 - h1 - 1), &out->lib_id) &&
           nid_decode_id(h2 + 1, strlen(h2 + 1), &out->mod_id);
}
