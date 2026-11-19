/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Package containers. A PS5 package starts with a finalized-image header
 * ("\x7fFIH", little-endian) giving the outer PFS image and an embedded CNT
 * container; a PS4 package is a CNT container ("\x7fCNT", big-endian) at
 * offset 0. The CNT entry table lists the metadata files, named through the
 * entry-names entry (id 0x200). Layout as observed in packages and in
 * LibOrbisPkg / LibProsperoPkg. */
#include "pal/pal_pkg.h"
#include "base/crypto.h"
#include "base/endian.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define FIH_SIZE 0x100
#define CNT_HEADER 0x80
#define CNT_ENTRY 32u
#define MAX_ENTRIES 4096u
#define MAX_NAMES (1u << 20)
#define PFS_MAGIC 20130315u
#define ENTRY_NAMES 0x200u

static uint32_t rd_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

bool pkg_path(const char *p) {
    size_t n = p ? strlen(p) : 0;
    if (n < 4) return false;
    const char *e = p + n - 4;
    return e[0] == '.' && (e[1] | 32) == 'p' && (e[2] | 32) == 'k' && (e[3] | 32) == 'g';
}

const char *pkg_kind_name(pkg_kind k) {
    return k == PKG_PS5 ? "PS5" : k == PKG_PS4 ? "PS4" : "unknown";
}

static ps5c_err pread_exact(pal_file *f, void *buf, uint64_t n, uint64_t off) {
    uint64_t got = 0;
    ps5c_err e = pal_file_pread(f, buf, n, off, &got);
    return e == PS5C_OK && got != n ? PS5C_ERR_IO : e;
}

static ps5c_err read_cnt(pal_file *f, pkg_info *info, uint64_t base) {
    uint8_t h[CNT_HEADER];
    if (base > info->file_size || info->file_size - base < CNT_HEADER) return PS5C_ERR_INVALID;
    ps5c_err e = pread_exact(f, h, sizeof h, base);
    if (e) return e;
    if (memcmp(h, "\x7f" "CNT", 4)) return PS5C_ERR_INVALID;
    if (!info->content_id[0]) {
        memcpy(info->content_id, h + 0x40, 36);
        info->content_id[36] = 0;
    }
    uint32_t n = rd_be32(h + 0x10), table = rd_be32(h + 0x18);
    if (!n || n > MAX_ENTRIES || base + table + (uint64_t)n * CNT_ENTRY > info->file_size) return PS5C_ERR_INVALID;
    uint8_t *raw = malloc((size_t)n * CNT_ENTRY);
    info->entries = calloc(n, sizeof *info->entries);
    if (!raw || !info->entries) { free(raw); return PS5C_ERR_NOMEM; }
    e = pread_exact(f, raw, (uint64_t)n * CNT_ENTRY, base + table);
    if (e) { free(raw); return e; }
    uint32_t names_at = UINT32_MAX;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *r = raw + (size_t)i * CNT_ENTRY;
        pkg_entry *x = &info->entries[i];
        x->id = rd_be32(r);
        x->flags1 = rd_be32(r + 8);
        x->flags2 = rd_be32(r + 12);
        x->offset = base + rd_be32(r + 16);
        x->size = rd_be32(r + 20);
        if (x->offset > info->file_size || x->size > info->file_size - x->offset) x->size = 0;
        if (x->id == ENTRY_NAMES) names_at = i;
    }
    info->nentries = n;
    if (names_at != UINT32_MAX && !(info->entries[names_at].flags1 & PKG_ENTRY_ENCRYPTED) &&
        info->entries[names_at].size && info->entries[names_at].size <= MAX_NAMES) {
        const pkg_entry *ne = &info->entries[names_at];
        char *names = malloc((size_t)ne->size + 1);
        if (names && !pread_exact(f, names, ne->size, ne->offset)) {
            names[ne->size] = 0;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t at = rd_be32(raw + (size_t)i * CNT_ENTRY + 4);
                if (at && at < ne->size) snprintf(info->entries[i].name, sizeof info->entries[i].name, "%s", names + at);
            }
        }
        free(names);
    }
    free(raw);
    return PS5C_OK;
}

static void read_superblock(pal_file *f, pkg_info *info) {
    uint8_t sb[0x400];
    uint64_t at = info->superblock_offset;
    if (!at || at > info->file_size || info->file_size - at < sizeof sb || pread_exact(f, sb, sizeof sb, at)) return;
    /* Plaintext superblock: version 2, magic, mode at 0x1c (bit 2 = encrypted). */
    if ((rd_le64(sb) == 1 || rd_le64(sb) == 2) && rd_le64(sb + 8) == PFS_MAGIC) { /* PS4: version 1 */
        info->outer_encrypted = (rd_le16(sb + 0x1c) & 4) != 0;
        info->block_size = rd_le32(sb + 0x20);
        info->pfs_blocks = rd_le64(sb + 0x38);
        memcpy(info->seed, sb + 0x370, 16);
    } else {
        info->outer_encrypted = true;
    }
}

ps5c_err pkg_inspect(const char *path, pkg_info *out) {
    memset(out, 0, sizeof *out);
    pal_file *f;
    ps5c_err e = pal_file_open(path, PAL_FILE_READ, &f);
    if (e) return e;
    uint8_t h[FIH_SIZE];
    e = pal_file_size(f, &out->file_size);
    if (!e) e = out->file_size < FIH_SIZE ? PS5C_ERR_UNSUPPORTED : pread_exact(f, h, sizeof h, 0);
    if (e) goto done;
    if (!memcmp(h, "\x7f" "FIH", 4)) {
        out->kind = PKG_PS5;
        out->retail = h[5] == 0x80;
        out->fih_flags = rd_le32(h + 4);
        out->format_version = rd_le16(h + 6);
        out->pfs_offset = rd_le64(h + 0x10);
        out->pfs_size = rd_le64(h + 0x18);
        out->superblock_offset = rd_le64(h + 0x20);
        out->cnt_offset = rd_le64(h + 0x58);
        if (out->pfs_offset >= out->file_size) out->pfs_offset = 0x10000;
        if (out->pfs_size > out->file_size - out->pfs_offset) out->pfs_size = out->file_size - out->pfs_offset;
        read_superblock(f, out);
        /* Metadata is optional for the PS5 path: a missing CNT is not fatal. */
        if (read_cnt(f, out, out->cnt_offset) == PS5C_ERR_NOMEM) e = PS5C_ERR_NOMEM;
    } else if (!memcmp(h, "\x7f" "CNT", 4)) {
        out->kind = PKG_PS4;
        e = read_cnt(f, out, 0);
        /* the PFS image (big-endian offset and size at 0x410), superblock first */
        uint8_t ph[16];
        if (!e && out->file_size > 0x420 && !pread_exact(f, ph, sizeof ph, 0x410)) {
            uint64_t off = (uint64_t)rd_be32(ph) << 32 | rd_be32(ph + 4), size = (uint64_t)rd_be32(ph + 8) << 32 | rd_be32(ph + 12);
            if (off && off < out->file_size) {
                out->pfs_offset = out->superblock_offset = off;
                out->pfs_size = size <= out->file_size - off ? size : out->file_size - off;
                read_superblock(f, out);
            }
        }
    } else {
        e = PS5C_ERR_UNSUPPORTED;
    }
done:
    pal_file_close(f);
    if (e) pkg_info_free(out);
    return e;
}

/* EKPFS = H(H(BE32 index) || H(content ID padded to 48 bytes) || passcode),
 * H = SHA-256 or SHA3-256 (LibProsperoPkg tries both). */
static void derive_ekpfs(const char *content_id, const char *passcode, bool sha3, uint8_t out[32]) {
    void (*h)(const void *, size_t, uint8_t *) = sha3 ? sha3_256 : sha256;
    uint8_t data[96], index[4] = {0, 0, 0, 1}, id[48] = {0};
    memcpy(id, content_id, strlen(content_id) < 48 ? strlen(content_id) : 48);
    h(index, 4, data);
    h(id, 48, data + 32);
    memcpy(data + 64, passcode, 32);
    h(data, 96, out);
}

/* Image keys: HMAC-SHA256(key, LE32(1) || seed) split into tweak and data
 * keys, where key is the EKPFS or, in the new scheme, HMAC(EKPFS, seed). */
static void image_keys(const uint8_t ekpfs[32], const uint8_t seed[16], bool new_crypt, uint8_t tweak[16], uint8_t data[16]) {
    uint8_t key[32], msg[20] = {1, 0, 0, 0}, out[32];
    if (new_crypt) hmac_sha256(ekpfs, 32, seed, 16, key);
    else memcpy(key, ekpfs, 32);
    memcpy(msg + 4, seed, 16);
    hmac_sha256(key, 32, msg, sizeof msg, out);
    memcpy(tweak, out, 16);
    memcpy(data, out + 16, 16);
}

/* Decrypted PFS metadata is mostly zeros; ciphertext under a wrong key is not. */
static bool looks_plain(const uint8_t *b, size_t n) {
    size_t zeros = 0;
    for (size_t i = 0; i < n; i++) zeros += !b[i];
    return zeros > n / 4;
}

ps5c_err pkg_find_key(const char *path, pkg_info *info, const char *passcode) {
    info->have_key = false;
    if (info->kind != PKG_PS5 || !info->block_size || info->block_size > (1u << 20) || strlen(info->content_id) != 36)
        return PS5C_ERR_UNSUPPORTED;
    if (!passcode) passcode = "00000000000000000000000000000000";
    if (strlen(passcode) != 32) return PS5C_ERR_INVALID;
    /* The block after the superblock holds the inodes (data-first layout). */
    uint64_t bs = info->block_size, sb_block = (info->superblock_offset - info->pfs_offset) / bs, meta = sb_block + 1;
    if (info->superblock_offset < info->pfs_offset || meta >= info->pfs_blocks) return PS5C_ERR_UNSUPPORTED;
    pal_file *f;
    ps5c_err e = pal_file_open(path, PAL_FILE_READ, &f);
    if (e) return e;
    uint8_t *raw = malloc(bs), *buf = malloc(bs);
    e = raw && buf ? pread_exact(f, raw, bs, info->pfs_offset + meta * bs) : PS5C_ERR_NOMEM;
    pal_file_close(f);
    info->outer_plain = !e && looks_plain(raw, bs);
    info->have_key = info->outer_plain;
    for (int c = 0; !e && c < 8 && !info->have_key; c++) {
        bool sha3 = c & 1, new_crypt = c & 2, sig = c & 4;
        uint8_t ekpfs[32], tweak[16], data[16];
        derive_ekpfs(info->content_id, passcode, sha3, ekpfs);
        image_keys(ekpfs, info->seed, new_crypt, tweak, data);
        aes_xts x;
        aes_xts_init(&x, data, tweak);
        memcpy(buf, raw, bs);
        aes_xts_decrypt(&x, buf, bs, meta | (sig ? 1ull << 47 : 0));
        if (looks_plain(buf, bs)) {
            info->have_key = true;
            info->signed_tweak = sig;
            memcpy(info->ekpfs, ekpfs, 32);
            memcpy(info->tweak_key, tweak, 16);
            memcpy(info->data_key, data, 16);
        }
    }
    free(raw);
    free(buf);
    if (e) return e;
    return info->have_key ? PS5C_OK : PS5C_ERR_NOT_FOUND;
}

static ps5c_err outer_read(void *user, void *buf, uint64_t size, uint64_t off) {
    pkg_outer *o = user;
    if (!o->xts) return pread_exact(o->file, buf, size, o->base + off);
    /* whole sectors around the range, decrypted, then the part asked for */
    uint8_t stack[0x2000], *tmp = stack;
    uint64_t lo = off & ~0xFFFull, hi = (off + size + 0xFFF) & ~0xFFFull;
    if (hi - lo > sizeof stack && !(tmp = malloc((size_t)(hi - lo)))) return PS5C_ERR_NOMEM;
    ps5c_err e = pread_exact(o->file, tmp, hi - lo, o->base + lo);
    if (!e) {
        pkg_ps4_decrypt(&o->x, tmp, hi - lo, lo);
        memcpy(buf, tmp + (off - lo), (size_t)size);
    }
    if (tmp != stack) free(tmp);
    return e;
}

ps5c_err pkg_outer_open(const char *path, const pkg_info *info, pkg_outer *o) {
    memset(o, 0, sizeof *o);
    if (info->kind == PKG_PS4 ? !info->have_key : (info->kind != PKG_PS5 || !info->outer_plain)) return PS5C_ERR_UNSUPPORTED;
    ps5c_err e = pal_file_open(path, PAL_FILE_READ, &o->file);
    if (e) return e;
    o->base = info->pfs_offset;
    if (info->kind == PKG_PS4 && !info->outer_plain) {
        o->xts = true;
        aes_xts_init(&o->x, info->data_key, info->tweak_key);
    }
    e = pfs_open(&o->fs, outer_read, o, info->superblock_offset - info->pfs_offset);
    if (e) { pal_file_close(o->file); o->file = NULL; }
    return e;
}

void pkg_outer_close(pkg_outer *o) {
    pfs_close(&o->fs);
    if (o->file) pal_file_close(o->file);
    o->file = NULL;
}

void pkg_info_free(pkg_info *info) {
    free(info->entries);
    info->entries = NULL;
    info->nentries = 0;
}

const pkg_entry *pkg_find(const pkg_info *info, const char *name) {
    for (uint32_t i = 0; i < info->nentries; i++)
        if (!strcmp(info->entries[i].name, name)) return &info->entries[i];
    return NULL;
}

ps5c_err pkg_read_entry(const char *path, const pkg_entry *x, uint8_t **data, uint64_t *size) {
    *data = NULL;
    *size = 0;
    if (x->flags1 & PKG_ENTRY_ENCRYPTED) return PS5C_ERR_UNSUPPORTED;
    pal_file *f;
    ps5c_err e = pal_file_open(path, PAL_FILE_READ, &f);
    if (e) return e;
    uint8_t *buf = malloc((size_t)x->size + 1);
    if (!buf) e = PS5C_ERR_NOMEM;
    else e = pread_exact(f, buf, x->size, x->offset);
    pal_file_close(f);
    if (e) { free(buf); return e; }
    buf[x->size] = 0;
    *data = buf;
    *size = x->size;
    return PS5C_OK;
}
