/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Games in raw PFS images (".ffpfsc" dumps, see pal_pfs_volume.h). Blue Prince
 * (PPSA25009): a PFS whose one file, PPSA25009.ffpfs, is a PFSC stream (64 KiB
 * zlib blocks behind a block table) holding a second PFS with the game. The
 * stream layout is the one pal_pkg_inner decodes for PS4 files compressed on
 * their own: "PFSC", block size at 0x10, table offset at 0x18, length at 0x28;
 * a block is zeros (no bytes), stored (a whole block's bytes) or zlib. */
#include "pal/pal_pfs_volume.h"
#include "pal/pal.h"
#include "base/endian.h"
#include "base/inflate.h"
#include "base/log.h"
#include <stdlib.h>
#include <string.h>

#define SLOTS 16
#define MAX_BLOCK (1u << 20)

typedef struct pfsc {
    const pfs_image *fs;
    const pfs_node *node;
    uint64_t bsz, len, nb;
    uint64_t *off; /* nb + 1 block starts in the stored bytes */
} pfsc;

/* decoded blocks; the nested image's stream and the tree's compressed files
 * each have one, so a file's decoding may read the image below it */
typedef struct cache {
    pal_mutex *lock;
    struct { uint32_t key; uint64_t blk, tick; uint8_t *buf; } slot[SLOTS];
    uint64_t tick;
    uint8_t *cbuf; /* compressed bytes of the block being decoded */
} cache;

struct pfs_volume {
    pal_file *file;
    uint64_t size;
    pfs_image outer, inner;
    bool nested;
    const pfs_node *image;  /* nested: the outer file holding the inner image */
    pfsc image_stream;      /* ...when it is compressed (image_stream.off != NULL) */
    pfsc *files;            /* per tree node: its stream when compressed */
    cache image_cache, file_cache;
};

static ps5c_err file_read(void *user, void *buf, uint64_t size, uint64_t off) {
    pfs_volume *v = user;
    uint64_t got = 0;
    ps5c_err e = off > v->size || size > v->size - off ? PS5C_ERR_OUT_OF_RANGE : pal_file_pread(v->file, buf, size, off, &got);
    return e ? e : got == size ? PS5C_OK : PS5C_ERR_IO;
}

static ps5c_err stream_open(const pfs_image *fs, const pfs_node *n, pfsc *s) {
    uint8_t h[0x30];
    if (n->nblocks > UINT64_MAX / fs->block_size) return PS5C_ERR_INVALID;
    uint64_t stored = n->nblocks * fs->block_size;
    ps5c_err e = stored < sizeof h ? PS5C_ERR_INVALID : pfs_read(fs, n, h, sizeof h, 0);
    if (e) return e;
    if (memcmp(h, "PFSC", 4)) return PS5C_ERR_UNSUPPORTED;
    uint64_t bsz = rd_le64(h + 0x10), table = rd_le64(h + 0x18), len = rd_le64(h + 0x28);
    if (!bsz || bsz > MAX_BLOCK) return PS5C_ERR_UNSUPPORTED;
    uint64_t nb = len / bsz + (len % bsz != 0);
    if (nb > (1u << 26) || table > stored || (nb + 1) * 8 > stored - table) return PS5C_ERR_INVALID;
    uint8_t *raw = malloc((size_t)(nb + 1) * 8);
    s->off = malloc((size_t)(nb + 1) * sizeof *s->off);
    e = raw && s->off ? pfs_read(fs, n, raw, (nb + 1) * 8, table) : PS5C_ERR_NOMEM;
    for (uint64_t i = 0; !e && i <= nb; i++) {
        s->off[i] = rd_le64(raw + i * 8);
        if (s->off[i] > stored || (i && (s->off[i] < s->off[i - 1] || s->off[i] - s->off[i - 1] > 2 * (uint64_t)bsz)))
            e = PS5C_ERR_INVALID;
    }
    free(raw);
    if (e) {
        free(s->off);
        s->off = NULL;
        return e;
    }
    s->fs = fs, s->node = n, s->bsz = bsz, s->len = len, s->nb = nb;
    return PS5C_OK;
}

/* block blk of s into dst (its length: the block's), under c->lock */
static ps5c_err decode(cache *c, const pfsc *s, uint64_t blk, uint8_t *dst) {
    uint64_t a = s->off[blk], clen = s->off[blk + 1] - a, ulen = PS5C_MIN(s->bsz, s->len - blk * s->bsz);
    if (!clen) {
        memset(dst, 0, (size_t)ulen);
        return PS5C_OK;
    }
    if (clen == s->bsz || clen == ulen) return pfs_read(s->fs, s->node, dst, ulen, a);
    ps5c_err e = pfs_read(s->fs, s->node, c->cbuf, clen, a);
    if (e) return e;
    if (inflate_zlib(c->cbuf, (size_t)clen, dst, (size_t)ulen) != (long)ulen) {
        static uint32_t logged;
        if (logged++ < 8) LOG_W(CORE, "PFS image: block %llu of a compressed file does not decode", (unsigned long long)blk);
        return PS5C_ERR_INVALID;
    }
    return PS5C_OK;
}

/* bytes [off, off + size) of the stream; whole blocks decode straight into buf */
static ps5c_err stream_read(cache *c, const pfsc *s, uint32_t key, uint8_t *buf, uint64_t size, uint64_t off) {
    if (off > s->len || size > s->len - off) return PS5C_ERR_OUT_OF_RANGE;
    pal_mutex_lock(c->lock);
    ps5c_err e = PS5C_OK;
    while (!e && size) {
        uint64_t blk = off / s->bsz, at = off % s->bsz, ulen = PS5C_MIN(s->bsz, s->len - blk * s->bsz);
        uint64_t n = PS5C_MIN(ulen - at, size);
        if (!at && n == ulen) {
            e = decode(c, s, blk, buf);
        } else {
            uint8_t *b = NULL;
            uint32_t victim = 0;
            for (uint32_t i = 0; i < SLOTS && !b; i++) {
                if (c->slot[i].buf && c->slot[i].key == key && c->slot[i].blk == blk) b = c->slot[i].buf, victim = i;
                else if (c->slot[i].tick < c->slot[victim].tick) victim = i;
            }
            if (!b) {
                if (!c->slot[victim].buf && !(c->slot[victim].buf = malloc(MAX_BLOCK))) e = PS5C_ERR_NOMEM;
                c->slot[victim].key = UINT32_MAX;
                if (!e && !(e = decode(c, s, blk, c->slot[victim].buf))) {
                    c->slot[victim].key = key, c->slot[victim].blk = blk;
                    b = c->slot[victim].buf;
                }
            }
            if (b) {
                c->slot[victim].tick = ++c->tick;
                memcpy(buf, b + at, (size_t)n);
            }
        }
        buf += n, off += n, size -= n;
    }
    pal_mutex_unlock(c->lock);
    return e;
}

static ps5c_err image_read(void *user, void *buf, uint64_t size, uint64_t off) {
    pfs_volume *v = user;
    if (!v->image_stream.off) return pfs_read(&v->outer, v->image, buf, size, off);
    return stream_read(&v->image_cache, &v->image_stream, 0, buf, size, off);
}

static ps5c_err cache_init(cache *c) {
    memset(c, 0, sizeof *c);
    c->cbuf = malloc(2 * MAX_BLOCK);
    if (!c->cbuf) return PS5C_ERR_NOMEM;
    return pal_mutex_create(&c->lock);
}

static void cache_free(cache *c) {
    for (uint32_t i = 0; i < SLOTS; i++) free(c->slot[i].buf);
    free(c->cbuf);
    if (c->lock) pal_mutex_destroy(c->lock);
}

/* the one file below the root that is a PFS image of its own (its first bytes
 * are a superblock), or NULL: the game is in the image's own tree */
static const pfs_node *nested_image(pfs_volume *v) {
    const pfs_image *o = &v->outer;
    const pfs_node *found = NULL;
    for (uint32_t c = o->nodes[o->uroot].child; c; c = o->nodes[c].sibling) {
        const pfs_node *n = &o->nodes[c];
        if (n->dir) {
            if (!strcmp(n->name, "sce_sys") || !strcmp(n->name, "Media")) return NULL;
            continue;
        }
        if (!strcmp(n->name, "eboot.bin")) return NULL;
        size_t len = strlen(n->name);
        if (len > 6 && !strcmp(n->name + len - 6, ".ffpfs")) {
            if (found) return NULL;
            found = n;
        }
    }
    return found;
}

ps5c_err pfs_volume_open(const char *path, pfs_volume **out) {
    *out = NULL;
    pfs_volume *v = calloc(1, sizeof *v);
    if (!v) return PS5C_ERR_NOMEM;
    ps5c_err e = pal_file_open(path, PAL_FILE_READ, &v->file);
    if (!e) e = pal_file_size(v->file, &v->size);
    if (!e) e = cache_init(&v->image_cache);
    if (!e) e = cache_init(&v->file_cache);
    if (!e) e = pfs_open(&v->outer, file_read, v, 0);
    if (!e && (v->image = nested_image(v))) {
        if (v->image->flags & 1) e = stream_open(&v->outer, v->image, &v->image_stream);
        if (!e) e = pfs_open(&v->inner, image_read, v, 0);
        v->nested = !e;
    }
    const pfs_image *t = !e ? pfs_volume_tree(v) : NULL;
    if (t && !(v->files = calloc(t->count ? t->count : 1, sizeof *v->files))) e = PS5C_ERR_NOMEM;
    for (uint32_t i = 0; t && !e && i < t->count; i++)
        if (!t->nodes[i].dir && (t->nodes[i].flags & 1)) e = stream_open(t, &t->nodes[i], &v->files[i]);
    if (e) {
        pfs_volume_close(v);
        return e;
    }
    LOG_I(CORE, "PFS image: %s, %u entries%s, read in place", path, t->count,
          v->nested ? (v->image_stream.off ? " in its compressed inner image" : " in its inner image") : "");
    *out = v;
    return PS5C_OK;
}

void pfs_volume_close(pfs_volume *v) {
    if (!v) return;
    const pfs_image *t = pfs_volume_tree(v);
    for (uint32_t i = 0; v->files && i < t->count; i++) free(v->files[i].off);
    free(v->files);
    free(v->image_stream.off);
    pfs_close(&v->inner);
    pfs_close(&v->outer);
    cache_free(&v->image_cache);
    cache_free(&v->file_cache);
    if (v->file) pal_file_close(v->file);
    free(v);
}

const pfs_image *pfs_volume_tree(const pfs_volume *v) { return v->nested ? &v->inner : &v->outer; }

uint64_t pfs_volume_size(const pfs_volume *v, uint32_t node) {
    const pfs_image *t = pfs_volume_tree(v);
    if (node >= t->count || t->nodes[node].dir) return 0;
    return v->files[node].off ? v->files[node].len : t->nodes[node].size;
}

ps5c_err pfs_volume_read(pfs_volume *v, uint32_t node, void *buf, uint64_t size, uint64_t off) {
    const pfs_image *t = pfs_volume_tree(v);
    if (node >= t->count || t->nodes[node].dir) return PS5C_ERR_INVALID;
    uint64_t total = pfs_volume_size(v, node);
    if (off > total || size > total - off) return PS5C_ERR_OUT_OF_RANGE;
    if (v->files[node].off) return stream_read(&v->file_cache, &v->files[node], node + 1, buf, size, off);
    return pfs_read(t, &t->nodes[node], buf, size, off);
}
