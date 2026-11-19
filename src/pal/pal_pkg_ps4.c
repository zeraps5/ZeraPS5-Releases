/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The outer image key of a PS4 fake package (fPKG), from RSA keys the user
 * supplies in a text file; the emulator ships no keys. Scheme as documented
 * by LibOrbisPkg (maxton) and implemented by shadPS4:
 *   - entry 0x10 (entry keys): a 32-byte seed digest, 7 digests of 32 bytes,
 *     then 7 RSA-2048 blobs of 256 bytes; blob 3 decrypts with the
 *     "derived key 3" keyset to the 32-byte DK3;
 *   - entry 0x20 (image key): 256 bytes, AES-128-CBC under
 *     SHA-256(raw 32-byte table record of entry 0x20 || DK3) (IV = first
 *     half, key = second half), then RSA with the "fake" keyset: the EKPFS;
 *   - PFS keys: HMAC-SHA256(EKPFS, LE32(1) || superblock seed) = tweak key ||
 *     data key; the image is AES-XTS per 4 KiB sector, sector number = byte
 *     offset / 0x1000, from sector 16 on (the superblock block is plain). */
#include "pal/pal_pkg.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/crypto.h"
#include "base/endian.h"
#include "base/log.h"
#include "base/rsa.h"

#define ENTRY_KEYS 0x10u
#define IMAGE_KEY 0x20u
#define SECTOR 0x1000u
#define PLAIN_SECTORS 16u

typedef struct rsa_key {
    uint8_t n[256], d[256];
    bool have_n, have_d;
} rsa_key;

const char *pkg_ps4_keys_path(void) {
    const char *e = getenv("PS5C_PS4_PKG_KEYS");
    return e && *e ? e : "user/ps4_pkg_keys.txt";
}

/* "name = value" entries; a value may run over several lines, up to the next
 * "name =" line. Values are big-endian, at most 256 bytes, right-aligned,
 * written either as one hex string ("1f72...", spaces, ':' and '_' allowed)
 * or as a C array as emulator sources print them ("{0x1F, 0x72, ...};"),
 * where each 0x token is one byte. '#' and "//" start comments. */
static bool parse_hex(const char *s, uint8_t out[256]) {
    uint8_t tmp[256];
    size_t n = 0;
    unsigned prefixed = 0; /* a C array has a 0x per byte; one hex string has at most one */
    for (const char *p = s; *p; p++)
        prefixed += p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && (p == s || !isxdigit((unsigned char)p[-1]));
    bool array = prefixed > 1;
    int half = -1;
    for (const char *p = s; *p;) {
        unsigned char c = (unsigned char)*p;
        if (isspace(c) || c == ':' || c == '_' || c == ',' || c == '{' || c == '}' || c == ';') {
            p++;
            continue;
        }
        if (array) {
            /* one byte: 0x then one or two hex digits */
            if (p[0] != '0' || (p[1] != 'x' && p[1] != 'X') || !isxdigit((unsigned char)p[2])) return false;
            p += 2;
            unsigned v = 0, digits = 0;
            for (; isxdigit((unsigned char)*p); p++, digits++) v = v << 4 | (unsigned)(isdigit((unsigned char)*p) ? *p - '0' : tolower((unsigned char)*p) - 'a' + 10);
            if (digits > 2 || n == sizeof tmp) return false;
            tmp[n++] = (uint8_t)v;
            continue;
        }
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && half < 0) { p += 2; continue; }
        if (!isxdigit(c)) return false;
        int v = isdigit(c) ? c - '0' : tolower(c) - 'a' + 10;
        p++;
        if (half < 0) half = v;
        else {
            if (n == sizeof tmp) return false;
            tmp[n++] = (uint8_t)(half << 4 | v);
            half = -1;
        }
    }
    if (half >= 0 || !n) return false;
    memset(out, 0, 256);
    memcpy(out + 256 - n, tmp, n);
    return true;
}

/* the key name of a "name = ..." line, lower case; false for other lines */
static bool key_line(const char *line, char name[64], const char **value) {
    const char *p = line;
    while (isspace((unsigned char)*p)) p++;
    size_t n = 0;
    while (isalnum((unsigned char)p[n]) || p[n] == '_') n++;
    if (!n || n >= 64) return false;
    const char *q = p + n;
    while (isspace((unsigned char)*q)) q++;
    if (*q != '=') return false;
    for (size_t i = 0; i < n; i++) name[i] = (char)tolower((unsigned char)p[i]);
    name[n] = 0;
    *value = q + 1;
    return true;
}

static void store_key(rsa_key *fake, rsa_key *dk3, const char *name, const char *value) {
    uint8_t *dst = NULL;
    bool *have = NULL;
    if (!strcmp(name, "fake_n")) dst = fake->n, have = &fake->have_n;
    else if (!strcmp(name, "fake_d")) dst = fake->d, have = &fake->have_d;
    else if (!strcmp(name, "dk3_n")) dst = dk3->n, have = &dk3->have_n;
    else if (!strcmp(name, "dk3_d")) dst = dk3->d, have = &dk3->have_d;
    if (!dst) return;
    if (parse_hex(value, dst)) *have = true;
    else LOG_W(CORE, "%s: %s is not a hexadecimal number or byte array of at most 256 bytes", pkg_ps4_keys_path(), name);
}

static ps5c_err load_keys(rsa_key *fake, rsa_key *dk3) {
    memset(fake, 0, sizeof *fake);
    memset(dk3, 0, sizeof *dk3);
    FILE *f = fopen(pkg_ps4_keys_path(), "r");
    if (!f) return PS5C_ERR_NOT_FOUND;
    enum { VALUE_MAX = 16384 };
    char line[4096], name[64] = "", *value = malloc(VALUE_MAX);
    size_t vlen = 0;
    if (!value) { fclose(f); return PS5C_ERR_NOMEM; }
    for (;;) {
        bool eof = !fgets(line, sizeof line, f);
        char *cut = eof ? NULL : strchr(line, '#');
        if (cut) *cut = 0;
        if (!eof && (cut = strstr(line, "//"))) *cut = 0;
        const char *rest = line;
        char next[64];
        bool starts = !eof && key_line(line, next, &rest);
        if (eof || starts) {
            if (name[0]) {
                value[vlen] = 0;
                store_key(fake, dk3, name, value);
            }
            if (eof) break;
            memcpy(name, next, sizeof name);
            vlen = 0;
        } else {
            rest = line;
        }
        size_t add = strlen(rest);
        if (name[0] && vlen + add + 2 < VALUE_MAX) {
            memcpy(value + vlen, rest, add);
            vlen += add;
            value[vlen++] = ' ';
        }
    }
    free(value);
    fclose(f);
    return fake->have_n && fake->have_d && dk3->have_n && dk3->have_d ? PS5C_OK : PS5C_ERR_INVALID;
}

/* RSA, then the PKCS #1 v1.5 payload, which must be `want` bytes. */
static bool rsa_open(const rsa_key *k, const uint8_t blob[256], uint8_t *out, size_t want) {
    uint8_t m[256];
    if (!rsa_private(k->n, k->d, blob, 256, m)) return false;
    long at = rsa_pkcs1_unpad(m, sizeof m);
    if (at < 0 || (size_t)(256 - at) != want) return false;
    memcpy(out, m + at, want);
    return true;
}

static ps5c_err pread_all(pal_file *f, void *buf, uint64_t n, uint64_t off) {
    uint64_t got = 0;
    ps5c_err e = pal_file_pread(f, buf, n, off, &got);
    return e == PS5C_OK && got != n ? PS5C_ERR_IO : e;
}

static bool looks_plain(const uint8_t *b, size_t n) {
    size_t zeros = 0;
    for (size_t i = 0; i < n; i++) zeros += !b[i];
    return zeros > n / 4;
}

void pkg_ps4_decrypt(const aes_xts *x, uint8_t *buf, uint64_t size, uint64_t off) {
    /* whole sectors only (callers read aligned) */
    for (uint64_t at = 0; at + SECTOR <= size; at += SECTOR) {
        uint64_t sector = (off + at) / SECTOR;
        if (sector >= PLAIN_SECTORS) aes_xts_decrypt(x, buf + at, SECTOR, sector);
    }
}

ps5c_err pkg_ps4_find_key(const char *path, pkg_info *info) {
    info->have_key = false;
    if (info->kind != PKG_PS4 || !info->pfs_offset || info->block_size < 0x1000) return PS5C_ERR_UNSUPPORTED;
    if (!info->outer_encrypted) {
        info->have_key = info->outer_plain = true;
        return PS5C_OK;
    }
    rsa_key fake, dk3k;
    ps5c_err e = load_keys(&fake, &dk3k);
    if (e) return e; /* NOT_FOUND: no key file; INVALID: incomplete */
    pal_file *f;
    if ((e = pal_file_open(path, PAL_FILE_READ, &f))) return e;
    uint8_t hdr[0x20], rec[32], blob[256], ekeys[2048];
    uint32_t n = 0, table = 0;
    bool have_rec = false, have_blob = false, have_keys = false;
    e = pread_all(f, hdr, sizeof hdr, 0);
    if (!e) {
        n = (uint32_t)hdr[0x10] << 24 | (uint32_t)hdr[0x11] << 16 | (uint32_t)hdr[0x12] << 8 | hdr[0x13];
        table = (uint32_t)hdr[0x18] << 24 | (uint32_t)hdr[0x19] << 16 | (uint32_t)hdr[0x1a] << 8 | hdr[0x1b];
    }
    for (uint32_t i = 0; !e && i < n && i < 4096; i++) {
        uint8_t r[32];
        if ((e = pread_all(f, r, sizeof r, (uint64_t)table + 32ull * i))) break;
        uint32_t id = (uint32_t)r[0] << 24 | (uint32_t)r[1] << 16 | (uint32_t)r[2] << 8 | r[3];
        uint32_t off = (uint32_t)r[16] << 24 | (uint32_t)r[17] << 16 | (uint32_t)r[18] << 8 | r[19];
        uint32_t size = (uint32_t)r[20] << 24 | (uint32_t)r[21] << 16 | (uint32_t)r[22] << 8 | r[23];
        if (id == IMAGE_KEY && size >= sizeof blob) {
            memcpy(rec, r, sizeof rec);
            e = pread_all(f, blob, sizeof blob, off);
            have_rec = have_blob = !e;
        } else if (id == ENTRY_KEYS && size >= sizeof ekeys) {
            e = pread_all(f, ekeys, sizeof ekeys, off);
            have_keys = !e;
        }
    }
    uint8_t *meta = NULL;
    uint64_t bs = info->block_size;
    if (!e && !(meta = malloc(bs))) e = PS5C_ERR_NOMEM;
    if (!e) e = pread_all(f, meta, bs, info->pfs_offset + bs); /* block 1: dinodes (mostly zeros once decrypted) */
    pal_file_close(f);
    if (!e && (!have_rec || !have_blob || !have_keys)) e = PS5C_ERR_INVALID;
    uint8_t dk3[32], ekpfs[32];
    if (!e && !rsa_open(&dk3k, ekeys + 32 + 7 * 32 + 3 * 256, dk3, sizeof dk3)) {
        LOG_W(CORE, "PS4 package: the dk3 key of %s does not open the entry keys", pkg_ps4_keys_path());
        e = PS5C_ERR_NOT_FOUND;
    }
    if (!e) {
        uint8_t cat[64], ivkey[32], img[256], prev[16];
        memcpy(cat, rec, 32);
        memcpy(cat + 32, dk3, 32);
        sha256(cat, sizeof cat, ivkey);
        aes128 a;
        aes128_init(&a, ivkey + 16);
        memcpy(prev, ivkey, 16);
        for (size_t k = 0; k < sizeof img; k += 16) {
            uint8_t out[16];
            aes128_decrypt(&a, blob + k, out);
            for (int j = 0; j < 16; j++) img[k + j] = out[j] ^ prev[j];
            memcpy(prev, blob + k, 16);
        }
        if (!rsa_open(&fake, img, ekpfs, sizeof ekpfs)) {
            LOG_W(CORE, "PS4 package: the fake key of %s does not open the image key", pkg_ps4_keys_path());
            e = PS5C_ERR_NOT_FOUND;
        }
    }
    if (!e) {
        uint8_t msg[20] = {1, 0, 0, 0}, mac[32];
        memcpy(msg + 4, info->seed, 16);
        hmac_sha256(ekpfs, 32, msg, sizeof msg, mac);
        aes_xts x;
        aes_xts_init(&x, mac + 16, mac);
        pkg_ps4_decrypt(&x, meta, bs, bs);
        if (looks_plain(meta, (size_t)bs)) {
            info->have_key = true;
            memcpy(info->ekpfs, ekpfs, 32);
            memcpy(info->tweak_key, mac, 16);
            memcpy(info->data_key, mac + 16, 16);
        } else {
            LOG_W(CORE, "PS4 package: the image key opened, but the file system does not decrypt with it");
            e = PS5C_ERR_NOT_FOUND;
        }
    }
    free(meta);
    return e;
}
