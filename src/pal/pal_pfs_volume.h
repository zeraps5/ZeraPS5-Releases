/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A game in a raw PFS image (".ffpfsc" dumps), read in place: the image's
 * own tree, or the PFS image it holds as its one file (PPSA25009.ffpfs,
 * PFSC-compressed). Compressed files are decoded a block at a time, on
 * demand; nothing is extracted. */
#ifndef PS5C_PAL_PFS_VOLUME_H
#define PS5C_PAL_PFS_VOLUME_H

#include "pal/pal_pfs.h"

typedef struct pfs_volume pfs_volume;

/* INVALID: not a PFS image (an exFAT .ffpfsc goes to the exFAT reader). */
ps5c_err pfs_volume_open(const char *path, pfs_volume **out);
void pfs_volume_close(pfs_volume *v);
/* The tree the game is in: nodes, uroot. */
const pfs_image *pfs_volume_tree(const pfs_volume *v);
/* A file node's length (decompressed); 0 for a directory. */
uint64_t pfs_volume_size(const pfs_volume *v, uint32_t node);
/* Bytes of a file node of the tree, decompressed. Thread-safe. */
ps5c_err pfs_volume_read(pfs_volume *v, uint32_t node, void *buf, uint64_t size, uint64_t off);
/* The game is in an exFAT image the PFS holds (<id>.exfat): its bytes, decompressed. */
bool pfs_volume_exfat(const pfs_volume *v);
uint64_t pfs_volume_image_size(const pfs_volume *v);
ps5c_err pfs_volume_image_read(pfs_volume *v, void *buf, uint64_t size, uint64_t off);

#endif
