/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PFS layout, as observed in PS5 packages and documented by LibOrbisPkg /
 * LibProsperoPkg: superblock (version, magic 20130315, mode at 0x1c, block
 * size at 0x20, dinode count at 0x30, dinode blocks at 0x40, and a 64-bit
 * signed dinode at 0x50 whose indirect blocks precede the dinode table);
 * dinodes of 0xA8 (unsigned), 0x2C8 (signed) or 0x310 (signed 64-bit)
 * bytes; directories of {inode, type, name length, entry size, name}. */
#include "pal/pal_pfs.h"
#include "base/endian.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define PFS_MAGIC 20130315u
#define MODE_SIGNED 1u
#define MODE_64BIT 2u
#define DIRENT_FILE 2u
#define DIRENT_DIR 3u
#define MAX_DINODES 4000000u
#define MAX_BLOCKS 100000000ull

typedef struct layout {
    size_t size;                 /* dinode bytes */
    size_t blocks_at;            /* block count field */
    bool blocks64;
    size_t db_at, ib_at, stride, ptr_at; /* pointer k of db/ib: (db|ib)_at + stride*k + ptr_at */
    bool ptr64;
} layout;

static layout dinode_layout(uint16_t mode) {
    if (!(mode & MODE_SIGNED)) return (layout){0xA8, 0x60, false, 0x64, 0x94, 4, 0, false};
    if (mode & MODE_64BIT) return (layout){0x310, 0x60, true, 0x68, 0x248, 40, 32, true};
    return (layout){0x2C8, 0x60, false, 0x64, 0x214, 36, 32, false};
}

static int64_t ptr(const layout *l, const uint8_t *d, size_t base, unsigned k) {
    const uint8_t *p = d + base + l->stride * k + l->ptr_at;
    return l->ptr64 ? (int64_t)rd_le64(p) : (int64_t)(int32_t)rd_le32(p);
}

typedef struct dinode {
    uint16_t mode;
    uint32_t flags;
    uint64_t size, csize, nblocks;
    int64_t db[12], ib[5];
} dinode;

static ps5c_err rd(const pfs_image *img, void *buf, uint64_t size, uint64_t off) { return img->read(img->user, buf, size, off); }

static ps5c_err add_node(pfs_image *img, pfs_node **out) {
    if (img->count == img->cap) {
        uint32_t cap = img->cap ? img->cap * 2 : 64;
        pfs_node *n = realloc(img->nodes, cap * sizeof *n);
        if (!n) return PS5C_ERR_NOMEM;
        img->nodes = n;
        img->cap = cap;
    }
    *out = &img->nodes[img->count++];
    memset(*out, 0, sizeof **out);
    return PS5C_OK;
}

/* Block list of a signed file whose blocks are not contiguous: 12 direct
 * pointers, then single and double indirect blocks of {signature, pointer}. */
static ps5c_err block_list(pfs_image *img, const layout *l, const dinode *di, pfs_node *n) {
    if (n->nblocks <= 1 || di->db[1] == -1 || di->db[1] == di->db[0] + 1) return PS5C_OK;
    if (n->nblocks > MAX_BLOCKS) return PS5C_ERR_UNSUPPORTED; /* unsigned: 32-bit pointers, stride 4 */
    int64_t *b = malloc(n->nblocks * sizeof *b);
    uint8_t *ind = malloc(img->block_size), *ind2 = malloc(img->block_size);
    ps5c_err e = b && ind && ind2 ? PS5C_OK : PS5C_ERR_NOMEM;
    uint64_t per = img->block_size / l->stride, k = 0;
    for (; !e && k < 12 && k < n->nblocks; k++) b[k] = di->db[k];
    if (!e && k < n->nblocks) {
        e = rd(img, ind, img->block_size, (uint64_t)di->ib[0] * img->block_size);
        for (uint64_t i = 0; !e && i < per && k < n->nblocks; i++) b[k++] = ptr(l, ind, 0, (unsigned)i);
    }
    for (uint64_t j = 0; !e && k < n->nblocks && j < per; j++) {
        if (j == 0) e = rd(img, ind2, img->block_size, (uint64_t)di->ib[1] * img->block_size);
        if (e) break;
        e = rd(img, ind, img->block_size, (uint64_t)ptr(l, ind2, 0, (unsigned)j) * img->block_size);
        for (uint64_t i = 0; !e && i < per && k < n->nblocks; i++) b[k++] = ptr(l, ind, 0, (unsigned)i);
    }
    if (!e && k < n->nblocks) e = PS5C_ERR_UNSUPPORTED; /* triple indirect */
    free(ind);
    free(ind2);
    if (e) { free(b); return e; }
    bool contiguous = true;
    for (uint64_t i = 1; i < n->nblocks && contiguous; i++) contiguous = b[i] == b[i - 1] + 1;
    if (contiguous) free(b);
    else n->blocks = b;
    return PS5C_OK;
}

ps5c_err pfs_open(pfs_image *img, pfs_read_fn read, void *user, uint64_t superblock) {
    memset(img, 0, sizeof *img);
    img->read = read;
    img->user = user;
    img->superblock = superblock;
    img->uroot = UINT32_MAX;
    uint8_t sb[0x400];
    ps5c_err e = rd(img, sb, sizeof sb, superblock);
    if (e) return e;
    if ((rd_le64(sb) != 1 && rd_le64(sb) != 2) || rd_le64(sb + 8) != PFS_MAGIC) return PS5C_ERR_INVALID;
    img->mode = rd_le16(sb + 0x1c);
    img->block_size = rd_le32(sb + 0x20);
    uint64_t ndinodes = rd_le64(sb + 0x30), dinode_blocks = rd_le64(sb + 0x40);
    if (img->block_size < 0x1000 || img->block_size > (1u << 20) || !ndinodes || ndinodes > MAX_DINODES) return PS5C_ERR_INVALID;
    layout l = dinode_layout(img->mode), sig = dinode_layout(MODE_SIGNED | MODE_64BIT);
    uint64_t skip = 0;
    for (unsigned k = 0; k < 5; k++) skip += ptr(&sig, sb + 0x50, sig.ib_at, k) > 0;
    uint64_t per = img->block_size / l.size;
    if (dinode_blocks * per < ndinodes) return PS5C_ERR_INVALID;

    dinode *din = calloc(ndinodes, sizeof *din);
    uint8_t *blk = malloc(img->block_size);
    if (!din || !blk) { free(din); free(blk); return PS5C_ERR_NOMEM; }
    uint64_t at = superblock + img->block_size * (1 + skip);
    for (uint64_t i = 0; !e && i < ndinodes; i++) {
        if (i % per == 0) e = rd(img, blk, img->block_size, at + img->block_size * (i / per));
        if (e) break;
        const uint8_t *d = blk + (i % per) * l.size;
        dinode *x = &din[i];
        x->mode = rd_le16(d);
        x->flags = rd_le32(d + 4);
        x->size = rd_le64(d + 8);
        x->csize = rd_le64(d + 0x10);
        x->nblocks = l.blocks64 ? rd_le64(d + l.blocks_at) : rd_le32(d + l.blocks_at);
        for (unsigned k = 0; k < 12; k++) x->db[k] = ptr(&l, d, l.db_at, k);
        for (unsigned k = 0; k < 5; k++) x->ib[k] = ptr(&l, d, l.ib_at, k);
    }
    pfs_node *root;
    if (!e) e = add_node(img, &root);
    if (!e) {
        root->name = strdup("");
        root->dir = true;
        root->start = din[0].db[0];
        root->nblocks = din[0].nblocks;
        if (!root->name) e = PS5C_ERR_NOMEM;
    }
    /* Breadth-first over directories: node indexes stay valid as the array grows. */
    uint8_t *seen = e ? NULL : calloc((size_t)ndinodes, 1);
    if (!e && !seen) e = PS5C_ERR_NOMEM;
    if (seen) seen[0] = 1;
    for (uint32_t ni = 0; !e && ni < img->count; ni++) {
        if (!img->nodes[ni].dir) continue;
        uint64_t start = (uint64_t)img->nodes[ni].start, nb = img->nodes[ni].nblocks;
        if (!nb || nb > 4096 || img->nodes[ni].start < 0) continue;
        uint32_t last = 0;
        for (uint64_t bi = 0; !e && bi < nb; bi++) {
            e = rd(img, blk, img->block_size, (start + bi) * img->block_size);
            for (uint64_t p = 0; !e && p + 16 <= img->block_size;) {
                uint32_t ino = rd_le32(blk + p), type = rd_le32(blk + p + 4), len = rd_le32(blk + p + 8), ent = rd_le32(blk + p + 12);
                if (!ent || ent > img->block_size - p || len > ent - 16) break;
                p += ent;
                if ((type != DIRENT_FILE && type != DIRENT_DIR) || ino >= ndinodes || seen[ino]) continue;
                seen[ino] = 1;
                pfs_node *n;
                if ((e = add_node(img, &n))) break;
                n->name = malloc(len + 1);
                if (!n->name) { e = PS5C_ERR_NOMEM; break; }
                memcpy(n->name, blk + p - ent + 16, len);
                n->name[len] = 0;
                n->parent = ni;
                n->inode = ino;
                n->dir = type == DIRENT_DIR;
                n->flags = din[ino].flags;
                n->size = din[ino].size;
                n->compressed_size = din[ino].csize;
                n->start = din[ino].db[0];
                n->nblocks = din[ino].nblocks;
                uint32_t idx = img->count - 1;
                if (!n->dir) e = block_list(img, &l, &din[ino], n);
                if (last) img->nodes[last].sibling = idx;
                else img->nodes[ni].child = idx;
                last = idx;
                if (ni == 0 && n->dir && !strcmp(n->name, "uroot")) img->uroot = idx;
            }
        }
    }
    free(seen);
    free(din);
    free(blk);
    if (!e && img->uroot == UINT32_MAX) e = PS5C_ERR_INVALID;
    if (e) pfs_close(img);
    return e;
}

void pfs_close(pfs_image *img) {
    for (uint32_t i = 0; i < img->count; i++) {
        free(img->nodes[i].name);
        free(img->nodes[i].blocks);
    }
    free(img->nodes);
    memset(img, 0, sizeof *img);
}

uint32_t pfs_lookup(const pfs_image *img, const char *path) {
    uint32_t cur = img->uroot;
    while (*path && cur != UINT32_MAX) {
        while (*path == '/') path++;
        size_t len = strcspn(path, "/");
        if (!len) break;
        uint32_t c = img->nodes[cur].child;
        for (; c; c = img->nodes[c].sibling)
            if (strlen(img->nodes[c].name) == len && !memcmp(img->nodes[c].name, path, len)) break;
        cur = c ? c : UINT32_MAX;
        path += len;
    }
    return cur;
}

ps5c_err pfs_read(const pfs_image *img, const pfs_node *n, void *buf, uint64_t size, uint64_t off) {
    uint64_t stored = n->nblocks * img->block_size; /* raw or PFSC: bounded by the blocks it holds */
    if (off > stored || size > stored - off) return PS5C_ERR_OUT_OF_RANGE;
    uint8_t *out = buf;
    while (size) {
        uint64_t bi = off / img->block_size, in = off % img->block_size;
        uint64_t take = img->block_size - in < size ? img->block_size - in : size;
        if (bi >= n->nblocks && n->nblocks) return PS5C_ERR_OUT_OF_RANGE;
        int64_t phys = n->blocks ? n->blocks[bi] : n->start + (int64_t)bi;
        if (phys < 0) return PS5C_ERR_INVALID;
        ps5c_err e = rd(img, out, take, (uint64_t)phys * img->block_size + in);
        if (e) return e;
        out += take, off += take, size -= take;
    }
    return PS5C_OK;
}

void pfs_path(const pfs_image *img, uint32_t node, char *out, size_t cap) {
    uint32_t chain[64];
    unsigned depth = 0;
    for (uint32_t c = node; c != img->uroot && c && depth < 64; c = img->nodes[c].parent) chain[depth++] = c;
    size_t len = 0;
    out[0] = 0;
    while (depth--) {
        int w = snprintf(out + len, cap > len ? cap - len : 0, "%s%s", len ? "/" : "", img->nodes[chain[depth]].name);
        if (w < 0 || (size_t)w >= cap - len) break;
        len += (size_t)w;
    }
}
