/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The inner file system of a PS5 fake/debug package. The outer PFS holds
 * pfs_image.dat, a data-first stream of stored and Kraken-compressed spans
 * (at most 256 KiB each), and naps_pkg_layout.dat, whose CblockInfo records
 * give each span's geometry. Spans are decoded on demand, so the uncompressed
 * image (the game's size) is never held in memory. Follows the NAPS reader in
 * LibProsperoPkg (Copyright (C) 2026 SvenGDK and Drakmor, GPL-3.0-or-later). */
#ifndef PS5C_PAL_PKG_INNER_H
#define PS5C_PAL_PKG_INNER_H

#include "pal/pal_pkg.h"

typedef enum pkg_inner_kind { PKG_SPAN_STORED, PKG_SPAN_KRAKEN, PKG_SPAN_ZERO, PKG_SPAN_ZLIB } pkg_inner_kind;

typedef struct pkg_inner_block {
    uint64_t uoff;      /* offset in the uncompressed image */
    uint64_t disk;      /* offset in pfs_image.dat */
    uint32_t ulen, clen;
    uint32_t first;     /* compressed length of the first 128 KiB half */
    uint8_t kind;       /* pkg_inner_kind */
    uint8_t flags;      /* Kraken per-half flags (0x02/0x20 newLZ, 0x01/0x10 sub literals) */
} pkg_inner_block;

typedef struct pkg_inner_node {
    char *name;
    uint32_t parent, child, sibling; /* node indexes, 0 = none */
    bool dir;
    uint64_t size, offset;           /* offset: in the uncompressed image */
} pkg_inner_node;

typedef struct pkg_inner {
    pkg_outer outer;
    uint32_t image_node;             /* pfs_image.dat in outer.fs */
    pkg_inner_block *blocks;
    uint32_t nblocks;
    uint64_t mount_size, superblock, block_size;
    uint32_t naps_files, naps_cblocks, naps_ublocks;
    /* decode state, shared by all reading threads under lock */
    struct pal_mutex *lock;
    struct pkg_slot *slots;          /* decoded Kraken spans, least recently used is reused */
    struct pkg_decoder *decoders;    /* idle scratch buffers, one per thread decoding at once */
    uint64_t tick;
    uint64_t bad_blocks;             /* spans that did not decode (read as zeros) */
    int last_status;                 /* kraken_status of the last Kraken span, -1 none */
    /* tree: node 0 is the super-root; uroot is the application root */
    pkg_inner_node *nodes;
    uint32_t count, cap, uroot;
    ps5c_err tree_err;               /* the block table loaded but the tree did not */
} pkg_inner;

/* PS5: needs a package whose outer image pkg_find_key found stored as
 * plaintext. PS4: a package pkg_ps4_find_key opened; its outer image holds
 * pfs_image.dat, a PFSC stream of zlib blocks whose content is the inner
 * PFS (classic layout, superblock first).
 * Succeeds once the block table is built; tree_err says whether the file
 * tree loaded (count is 0 when not). */
ps5c_err pkg_inner_open(const char *path, const pkg_info *info, pkg_inner **out);
void pkg_inner_close(pkg_inner *in);
/* Bytes of the uncompressed inner image. Safe to call from several threads. */
ps5c_err pkg_inner_read(pkg_inner *in, void *buf, uint64_t size, uint64_t off);
/* Bytes of a file node. */
ps5c_err pkg_inner_read_file(pkg_inner *in, const pkg_inner_node *n, void *buf, uint64_t size, uint64_t off);
/* Drops the decoded spans, so the next reads decode again. */
void pkg_inner_flush(pkg_inner *in);
/* "a/b/c" below uroot. */
void pkg_inner_path(const pkg_inner *in, uint32_t node, char *out, size_t cap);
uint32_t pkg_inner_lookup(const pkg_inner *in, const char *path); /* UINT32_MAX when missing */

#endif
