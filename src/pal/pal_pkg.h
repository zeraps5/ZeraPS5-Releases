/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PS5 and PS4 package files (.pkg): container headers and the CNT entry
 * table (param.json, icons, ...). Retail packages are recognised and their
 * metadata read; their file system needs the console's keys and is refused. */
#ifndef PS5C_PAL_PKG_H
#define PS5C_PAL_PKG_H

#include "pal/pal.h"
#include "pal/pal_pfs.h"
#include "base/crypto.h"

typedef enum pkg_kind { PKG_UNKNOWN = 0, PKG_PS5, PKG_PS4 } pkg_kind;

typedef struct pkg_entry {
    uint32_t id, flags1, flags2;
    uint64_t offset, size; /* offset from the start of the .pkg file */
    char name[64];         /* "" when the entry has no name */
} pkg_entry;

typedef struct pkg_info {
    pkg_kind kind;
    bool retail;          /* PS5: signed for retail consoles (FIH byte 5 = 0x80) */
    bool outer_encrypted; /* PS5: the outer PFS superblock says AES-XTS */
    char content_id[37];
    uint32_t format_version;
    uint64_t file_size;
    uint64_t pfs_offset, pfs_size, superblock_offset, cnt_offset;
    pkg_entry *entries;
    uint32_t nentries;
    /* PS5 outer PFS superblock */
    uint8_t seed[16];
    uint32_t block_size;
    uint64_t pfs_blocks;
    uint32_t fih_flags;   /* FIH bytes 4..7 */
    /* pkg_find_key: the outer PFS is readable, as stored (debug packages:
     * "plaintext, no auth" although the superblock mode says encrypted) or
     * with the fake/debug key schedule */
    bool have_key;
    bool outer_plain;
    bool signed_tweak; /* metadata blocks use unit = block | 1 << 47 */
    uint8_t ekpfs[32], tweak_key[16], data_key[16];
} pkg_info;

#define PKG_ENTRY_ENCRYPTED 0x80000000u

bool pkg_path(const char *path); /* ends in .pkg */
/* PS5C_ERR_UNSUPPORTED when the file is not a package. */
ps5c_err pkg_inspect(const char *path, pkg_info *out);
void pkg_info_free(pkg_info *info);
const pkg_entry *pkg_find(const pkg_info *info, const char *name);
/* Reads a plaintext entry into a malloc'd buffer (NUL-terminated past size).
 * PS5C_ERR_UNSUPPORTED for encrypted entries. */
ps5c_err pkg_read_entry(const char *path, const pkg_entry *e, uint8_t **data, uint64_t *size);
const char *pkg_kind_name(pkg_kind kind);
/* PS5: checks whether the outer PFS metadata block is readable as stored, or
 * else derives the fake/debug package keys from the content ID and passcode
 * (NULL: 32 zeros) and checks them against it.
 * PS5C_ERR_NOT_FOUND when no candidate decrypts it (a retail package). */
ps5c_err pkg_find_key(const char *path, pkg_info *info, const char *passcode);

/* The outer PFS of a PS5 package whose outer image pkg_find_key found
 * stored as plaintext. */
typedef struct pkg_outer {
    pal_file *file;
    uint64_t base;
    pfs_image fs;
    bool xts;  /* PS4: the image is AES-XTS encrypted (pkg_ps4_decrypt) */
    aes_xts x;
} pkg_outer;
ps5c_err pkg_outer_open(const char *path, const pkg_info *info, pkg_outer *out);
void pkg_outer_close(pkg_outer *o);

/* PS4 fake packages (pal_pkg_ps4.c). The RSA keys come from a text file the
 * user supplies ("fake_n/fake_d/dk3_n/dk3_d = hex", PS5C_PS4_PKG_KEYS or
 * user/ps4_pkg_keys.txt); the emulator ships none. PS5C_ERR_NOT_FOUND: no
 * key file, or its keys do not open this package; PS5C_ERR_INVALID: the file
 * lacks one of the four values. */
const char *pkg_ps4_keys_path(void);
ps5c_err pkg_ps4_find_key(const char *path, pkg_info *info);
/* Decrypts whole 4 KiB sectors of the outer image read at byte `off` of it. */
void pkg_ps4_decrypt(const aes_xts *x, uint8_t *buf, uint64_t size, uint64_t off);

#endif
