/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Read-only PlayStation file system (PFS) images: superblock, dinodes and
 * directories, read through a caller's positional read function. Images
 * whose data is AES-XTS encrypted are not handled here. */
#ifndef PS5C_PAL_PFS_H
#define PS5C_PAL_PFS_H

#include "base/base.h"

/* Reads size bytes at offset off of the image (not of the file holding it). */
typedef ps5c_err (*pfs_read_fn)(void *user, void *buf, uint64_t size, uint64_t off);

typedef struct pfs_node {
    char *name;            /* "" for the super-root */
    uint32_t parent;       /* node index; the super-root is its own parent */
    uint32_t inode;
    bool dir;
    uint32_t flags;        /* inode flags: 1 = compressed */
    uint64_t size, compressed_size;
    int64_t start;         /* first block */
    uint64_t nblocks;
    int64_t *blocks;       /* block list when not contiguous, else NULL */
    uint32_t child, sibling; /* first child / next sibling, 0 = none */
} pfs_node;

typedef struct pfs_image {
    pfs_read_fn read;
    void *user;
    uint64_t block_size, superblock; /* superblock: byte offset in the image */
    uint16_t mode;
    pfs_node *nodes;
    uint32_t count, cap;
    uint32_t uroot;        /* node index of "uroot", the application root */
} pfs_image;

/* superblock: byte offset of the superblock in the image (0 for the
 * classic layout; a PS5 package's data-first outer image has it near the end). */
ps5c_err pfs_open(pfs_image *img, pfs_read_fn read, void *user, uint64_t superblock);
void pfs_close(pfs_image *img);
/* Path below uroot ("a/b/c"); UINT32_MAX when missing. */
uint32_t pfs_lookup(const pfs_image *img, const char *path);
/* Stored bytes (compressed files are not decompressed). */
ps5c_err pfs_read(const pfs_image *img, const pfs_node *n, void *buf, uint64_t size, uint64_t off);
/* "a/b/c" below uroot, written into out. */
void pfs_path(const pfs_image *img, uint32_t node, char *out, size_t cap);

#endif
