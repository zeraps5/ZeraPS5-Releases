/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A synthetic PS4 fake package, built here with the tests' own RSA key: entry
 * keys and image key blobs, an AES-XTS outer PFS holding pfs_image.dat, a
 * PFSC stream (one stored block, the others zlib) and an inner PFS with
 * uroot/eboot.bin, a file whose blocks are out of order and a file
 * compressed on its own (its own PFSC stream). It checks that pal_pkg_ps4 + pal_pkg_inner read the file
 * back; it says nothing about real packages beyond the documented layout. */
#include "pal/pal_pkg.h"
#include "pal/pal_pkg_inner.h"
#include "base/crypto.h"
#include "base/rsa.h"
#include "base/base.h"
#include "test.h"
#include "rsa_test_key.h"

#include <stdio.h>
#include <stdlib.h>

#define BS 0x10000u
#define PFS_AT 0x10000u
#define IN_BLOCKS 10 /* inner: sb, dinodes, 2 dirs, eboot.bin (4-5), comp.bin (6-7), scat.bin (9, 8) */
#define FSIZE 0x18000u
#define CSIZE 0x14000u

static const char *g_dir = ".";

static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static void le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }
static void le64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> 8 * i); }

/* RSA public operation with the test key, over a PKCS #1 v1.5 block */
static void rsa_seal(const uint8_t *msg, size_t len, uint8_t out[256]) {
    uint8_t m[256], e[256] = {0};
    m[0] = 0, m[1] = 2;
    memset(m + 2, 0x5A, 256 - 3 - len);
    m[256 - 1 - len] = 0;
    memcpy(m + 256 - len, msg, len);
    e[253] = 1, e[254] = 0, e[255] = 1; /* 65537 */
    rsa_private(rsa_n, e, m, 256, out);
}

/* PFS: superblock (version 1, mode), unsigned 0xA8 dinodes in block 1:
 * 0 root (dir block 2: "uroot"), 1 uroot (dir block 3: `name`), 2 the file
 * at block 4 (`size` bytes). */
static void make_pfs(uint8_t *img, uint16_t mode, const char *name, uint64_t size, const uint8_t seed[16]) {
    uint8_t *sb = img;
    le64(sb, 1);
    le64(sb + 8, 20130315);
    le16(sb + 0x1c, mode);
    le32(sb + 0x20, BS);
    le64(sb + 0x30, 3);
    le64(sb + 0x40, 1);
    if (seed) memcpy(sb + 0x370, seed, 16);
    uint8_t *din = img + BS;
    uint32_t blocks[3] = {2, 3, 4};
    uint64_t sizes[3] = {BS, BS, size};
    for (int i = 0; i < 3; i++) {
        uint8_t *d = din + 0xA8 * i;
        le16(d, i < 2 ? 0x4000 : 0x8000);
        le64(d + 8, sizes[i]);
        le32(d + 0x60, i < 2 ? 1 : (uint32_t)((size + BS - 1) / BS));
        le32(d + 0x64, blocks[i]);
        for (int k = 1; k < 12; k++) le32(d + 0x64 + 4 * k, 0xFFFFFFFFu);
    }
    const char *names[2] = {"uroot", name};
    for (int i = 0; i < 2; i++) {
        uint8_t *e = img + (uint64_t)(2 + i) * BS;
        uint32_t len = (uint32_t)strlen(names[i]), ent = (16 + len + 7) & ~7u;
        le32(e, (uint32_t)i + 1);
        le32(e + 4, i ? 2 : 3);
        le32(e + 8, len);
        le32(e + 12, ent);
        memcpy(e + 16, names[i], len);
    }
}

/* a zlib stream of stored deflate blocks */
static size_t zlib_stored(const uint8_t *src, size_t len, uint8_t *dst) {
    size_t o = 0;
    dst[o++] = 0x78, dst[o++] = 0x01;
    size_t at = 0;
    do {
        size_t n = len - at < 65535 ? len - at : 65535;
        dst[o++] = at + n == len;
        dst[o++] = (uint8_t)n, dst[o++] = (uint8_t)(n >> 8);
        dst[o++] = (uint8_t)~n, dst[o++] = (uint8_t)(~n >> 8);
        memcpy(dst + o, src + at, n);
        o += n, at += n;
    } while (at < len);
    memset(dst + o, 0, 4); /* Adler-32, unchecked */
    return o + 4;
}

/* a PFSC stream of len bytes: block 0 stored, the others zlib */
static size_t make_pfsc(const uint8_t *src, uint64_t len, uint8_t *dst) {
    uint64_t nb = (len + BS - 1) / BS, at = 0x30 + (nb + 1) * 8;
    memcpy(dst, "PFSC", 4);
    le32(dst + 0xC, BS);
    le64(dst + 0x10, BS);
    le64(dst + 0x18, 0x30);
    le64(dst + 0x20, at);
    le64(dst + 0x28, len);
    for (uint64_t b = 0; b < nb; b++) {
        uint64_t n = len - b * BS < BS ? len - b * BS : BS;
        le64(dst + 0x30 + 8 * b, at);
        if (b == 0 && n == BS) { memcpy(dst + at, src, BS); at += BS; }
        else at += zlib_stored(src + b * BS, (size_t)n, dst + at);
    }
    le64(dst + 0x30 + 8 * nb, at);
    return (size_t)at;
}

/* the inner PFS: dinodes 0 root, 1 uroot, 2 eboot.bin (blocks 4-5),
 * 3 comp.bin (flag 1, PFSC in blocks 6-7), 4 scat.bin (blocks 9 then 8);
 * file contents are patterns, returned in eboot/comp/scat */
static void make_inner(uint8_t *img, uint8_t *eboot, uint8_t *comp, uint8_t *scat) {
    for (uint64_t i = 0; i < FSIZE; i++) eboot[i] = (uint8_t)(i * 7 + (i >> 9)), scat[i] = (uint8_t)(i * 13 + (i >> 11) + 1);
    for (uint64_t i = 0; i < CSIZE; i++) comp[i] = (uint8_t)(i * 3 + (i >> 8) + 5);
    le64(img, 1);
    le64(img + 8, 20130315);
    le32(img + 0x20, BS);
    le64(img + 0x30, 5);
    le64(img + 0x40, 1);
    memcpy(img + 4 * BS, eboot, FSIZE);
    uint64_t csz = make_pfsc(comp, CSIZE, img + 6 * BS);
    memcpy(img + 9 * BS, scat, BS);
    memcpy(img + 8 * BS, scat + BS, FSIZE - BS);
    struct { uint16_t mode; uint32_t flags; uint64_t size, csize; uint32_t nb, db0, db1; } d[5] = {
        {0x4000, 0, BS, BS, 1, 2, 0xFFFFFFFFu},
        {0x4000, 0, BS, BS, 1, 3, 0xFFFFFFFFu},
        {0x8000, 0, FSIZE, FSIZE, 2, 4, 5},
        {0x8000, 1, CSIZE, csz, 2, 6, 7},
        {0x8000, 0, FSIZE, FSIZE, 2, 9, 8},
    };
    for (int i = 0; i < 5; i++) {
        uint8_t *x = img + BS + 0xA8 * i;
        le16(x, d[i].mode);
        le32(x + 4, d[i].flags);
        le64(x + 8, d[i].size);
        le64(x + 0x10, d[i].csize);
        le32(x + 0x60, d[i].nb);
        le32(x + 0x64, d[i].db0);
        le32(x + 0x68, d[i].db1);
        for (int k = 2; k < 12; k++) le32(x + 0x64 + 4 * k, 0xFFFFFFFFu);
    }
    const char *names[4] = {"uroot", "eboot.bin", "comp.bin", "scat.bin"};
    uint8_t *e = img + 2 * BS;
    for (int i = 0; i < 4; i++) {
        if (i == 1) e = img + 3 * BS;
        uint32_t len = (uint32_t)strlen(names[i]), ent = (16 + len + 7) & ~7u;
        le32(e, (uint32_t)i + 1);
        le32(e + 4, i ? 2 : 3);
        le32(e + 8, len);
        le32(e + 12, ent);
        memcpy(e + 16, names[i], len);
        e += ent;
    }
}

static void check_file(pkg_inner *in, const char *name, const uint8_t *want, uint64_t size) {
    uint32_t n = pkg_inner_lookup(in, name);
    CHECK(n != UINT32_MAX);
    CHECK_EQ_U64(in->nodes[n].size, size);
    uint8_t *back = malloc((size_t)size);
    CHECK(back);
    CHECK(pkg_inner_read_file(in, &in->nodes[n], back, size, 0) == PS5C_OK);
    CHECK(!memcmp(back, want, (size_t)size));
    /* a read across a block boundary, after the first */
    memset(back, 0, 0x100);
    CHECK(pkg_inner_read_file(in, &in->nodes[n], back, 0x100, BS - 0x80) == PS5C_OK);
    CHECK(!memcmp(back, want + BS - 0x80, 0x100));
    free(back);
}

static void test_ps4_package(void) {
    char pkg[600], keys[600];
    snprintf(pkg, sizeof pkg, "%s/test.pkg", g_dir);
    snprintf(keys, sizeof keys, "%s/keys.txt", g_dir);
    /* keys file: the same test key for both keysets */
    FILE *kf = fopen(keys, "w");
    CHECK(kf);
    const char *names[4] = {"fake_n", "fake_d", "dk3_n", "dk3_d"};
    for (int k = 0; k < 4; k++) {
        const uint8_t *v = k & 1 ? rsa_d : rsa_n;
        if (k < 2) { /* fake keys as a C array over several lines, the way emulator sources print them */
            fprintf(kf, "%s = { // RSA-2048\n", names[k]);
            for (int i = 0; i < 256; i++) fprintf(kf, "0x%02X,%s", v[i], i % 16 == 15 ? "\n" : " ");
            fprintf(kf, "};\n\n");
        } else { /* dk3 as one hex string */
            fprintf(kf, "%s = 0x", names[k]);
            for (int i = 0; i < 256; i++) fprintf(kf, "%02x", v[i]);
            fprintf(kf, "\n");
        }
    }
    fclose(kf);
    char env[700];
    snprintf(env, sizeof env, "PS5C_PS4_PKG_KEYS=%s", keys);
#ifdef _WIN32
    _putenv(env);
#else
    putenv(strdup(env));
#endif

    /* inner PFS, file contents patterns */
    uint8_t *inner = calloc(IN_BLOCKS, BS), *eboot = malloc(FSIZE), *comp = malloc(CSIZE), *scat = malloc(FSIZE);
    CHECK(inner && eboot && comp && scat);
    make_inner(inner, eboot, comp, scat);
    /* PFSC: block 0 stored, the rest zlib (stored deflate) */
    uint8_t *pfsc = calloc(1, 0x30 + (IN_BLOCKS + 1) * 8 + IN_BLOCKS * (BS + 64));
    CHECK(pfsc);
    memcpy(pfsc, "PFSC", 4);
    le32(pfsc + 0xC, BS);
    le64(pfsc + 0x10, BS);
    le64(pfsc + 0x18, 0x30);
    le64(pfsc + 0x28, (uint64_t)IN_BLOCKS * BS);
    uint64_t at = 0x30 + (IN_BLOCKS + 1) * 8;
    le64(pfsc + 0x20, at);
    for (int b = 0; b < IN_BLOCKS; b++) {
        le64(pfsc + 0x30 + 8 * b, at);
        if (b == 0) { memcpy(pfsc + at, inner, BS); at += BS; }
        else at += zlib_stored(inner + (uint64_t)b * BS, BS, pfsc + at);
    }
    le64(pfsc + 0x30 + 8 * IN_BLOCKS, at);
    uint64_t pfsc_size = at;
    /* outer PFS, encrypted (mode 4), pfs_image.dat = the PFSC stream */
    uint64_t outer_blocks = 4 + (pfsc_size + BS - 1) / BS;
    uint8_t *outer = calloc((size_t)outer_blocks, BS);
    CHECK(outer);
    uint8_t seed[16];
    for (int i = 0; i < 16; i++) seed[i] = (uint8_t)(0xA0 + i);
    make_pfs(outer, 4, "pfs_image.dat", pfsc_size, seed);
    memcpy(outer + 4 * BS, pfsc, (size_t)pfsc_size);
    le64(outer + 0x38, outer_blocks);

    /* keys: dk3, ekpfs; the image key record and blobs */
    uint8_t dk3[32], ekpfs[32];
    for (int i = 0; i < 32; i++) dk3[i] = (uint8_t)(i + 1), ekpfs[i] = (uint8_t)(0x40 + i);
    uint8_t *file = calloc(1, PFS_AT + (size_t)outer_blocks * BS);
    CHECK(file);
    memcpy(file, "\x7f" "CNT", 4);
    be32(file + 0x10, 2);
    be32(file + 0x18, 0x2000);
    memcpy(file + 0x40, "UP0000-CUSA00000_00-TESTPACKAGE00000", 36);
    be32(file + 0x410 + 4, PFS_AT);
    be32(file + 0x418 + 4, (uint32_t)(outer_blocks * BS));
    uint8_t *r0 = file + 0x2000, *r1 = r0 + 32;
    be32(r0, 0x10);
    be32(r0 + 16, 0x3000);
    be32(r0 + 20, 2048);
    be32(r1, 0x20);
    be32(r1 + 8, 0xE0000000u);
    be32(r1 + 16, 0x3800);
    be32(r1 + 20, 256);
    rsa_seal(dk3, 32, file + 0x3000 + 32 + 7 * 32 + 3 * 256);
    uint8_t cat[64], ivkey[32], img[256];
    memcpy(cat, r1, 32);
    memcpy(cat + 32, dk3, 32);
    sha256(cat, 64, ivkey);
    rsa_seal(ekpfs, 32, img);
    aes128 a;
    aes128_init(&a, ivkey + 16);
    const uint8_t *prev = ivkey;
    for (int k = 0; k < 256; k += 16) {
        uint8_t x[16];
        for (int j = 0; j < 16; j++) x[j] = img[k + j] ^ prev[j];
        aes128_encrypt(&a, x, file + 0x3800 + k);
        prev = file + 0x3800 + k;
    }
    /* XTS: HMAC(ekpfs, LE32(1) || seed) = tweak || data, sectors 16 on */
    uint8_t msg[20] = {1, 0, 0, 0}, mac[32];
    memcpy(msg + 4, seed, 16);
    hmac_sha256(ekpfs, 32, msg, sizeof msg, mac);
    aes_xts x;
    aes_xts_init(&x, mac + 16, mac);
    for (uint64_t s = 16; s < outer_blocks * BS / 0x1000; s++) aes_xts_encrypt(&x, outer + s * 0x1000, 0x1000, s);
    memcpy(file + PFS_AT, outer, (size_t)(outer_blocks * BS));
    FILE *pf = fopen(pkg, "wb");
    CHECK(pf);
    fwrite(file, 1, PFS_AT + (size_t)outer_blocks * BS, pf);
    fclose(pf);
    free(file), free(outer), free(pfsc);

    pkg_info info;
    CHECK(pkg_inspect(pkg, &info) == PS5C_OK);
    CHECK(info.kind == PKG_PS4);
    CHECK(info.outer_encrypted);
    CHECK_EQ_U64(info.pfs_offset, PFS_AT);
    CHECK(pkg_ps4_find_key(pkg, &info) == PS5C_OK);
    CHECK(!memcmp(info.ekpfs, ekpfs, 32));
    pkg_inner *in = NULL;
    CHECK(pkg_inner_open(pkg, &info, &in) == PS5C_OK);
    CHECK(in->tree_err == PS5C_OK);
    CHECK_EQ_U64(in->nblocks, IN_BLOCKS);
    CHECK(in->blocks[0].kind == PKG_SPAN_STORED && in->blocks[1].kind == PKG_SPAN_ZLIB);
    check_file(in, "eboot.bin", eboot, FSIZE);
    CHECK(!in->nodes[pkg_inner_lookup(in, "eboot.bin")].pfs);
    check_file(in, "comp.bin", comp, CSIZE);
    check_file(in, "scat.bin", scat, FSIZE);
    CHECK_EQ_U64(in->bad_blocks, 0);
    pkg_inner_close(in);
    pkg_info_free(&info);
    free(inner), free(eboot), free(comp), free(scat);

    /* a key file whose keys are wrong: not found, nothing opened */
    kf = fopen(keys, "w");
    CHECK(kf);
    for (int k = 0; k < 4; k++) {
        fprintf(kf, "%s = ", names[k]);
        for (int i = 0; i < 256; i++) fprintf(kf, "%02x", (k & 1 ? rsa_d : rsa_n)[i] ^ (k == 1 && i == 200));
        fprintf(kf, "\n");
    }
    fclose(kf);
    CHECK(pkg_inspect(pkg, &info) == PS5C_OK);
    CHECK(pkg_ps4_find_key(pkg, &info) == PS5C_ERR_NOT_FOUND);
    pkg_info_free(&info);
    remove(pkg);
    remove(keys);
}

int main(int argc, char **argv) {
    if (argc > 1) g_dir = argv[1];
    static const test_case tests[] = {
        TEST_CASE(test_ps4_package),
    };
    return test_run_all("pkg_ps4", tests, PS5C_ARRAY_COUNT(tests));
}
