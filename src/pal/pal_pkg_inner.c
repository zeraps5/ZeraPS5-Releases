/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Inner file system of PS5 fake/debug packages, decoded span by span.
 * Follows the NAPS reader of LibProsperoPkg (SvenGDK; Drakmor's
 * feature/full-ppr-pkg-roundtrip branch), GPL-3.0-or-later. */
#include "pal/pal_pkg_inner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/endian.h"
#include "base/inflate.h"
#include "base/kraken.h"
#include "base/log.h"

#define UBLOCK 0x40000u
#define CHUNK 0x20000u
#define CBUF_MAX 0x80000u
#define INODE_SIZE 0xA8u
#define MAX_INODES 1000000u
#define DIRENT_FILE 2
#define DIRENT_DIR 3

/* PS5C_LOG_PKG_STATS: read counters every 10 s (debugging aid) */
static int g_stats = -1;
static uint64_t g_calls, g_bytes, g_hits, g_misses, g_disk_reads, g_disk_bytes, g_disk_ns, g_decode_ns, g_last;
#define STAT_ADD(v, n) __atomic_fetch_add(&(v), (n), __ATOMIC_RELAXED)

static void stats_tick(void) {
    uint64_t now = pal_time_ns(), last = __atomic_load_n(&g_last, __ATOMIC_RELAXED);
    if (now - last < 10000000000ull || !__atomic_compare_exchange_n(&g_last, &last, now, false, __ATOMIC_RELAXED,
                                                                     __ATOMIC_RELAXED))
        return;
    LOG_I(CORE, "pkg reads: %llu calls, %.1f MB; spans %llu hit, %llu decoded; disk %llu reads, %.1f MB, %.1f s; "
                "decode %.1f s",
          (unsigned long long)g_calls, g_bytes / 1048576.0, (unsigned long long)g_hits, (unsigned long long)g_misses,
          (unsigned long long)g_disk_reads, g_disk_bytes / 1048576.0, g_disk_ns / 1e9, g_decode_ns / 1e9);
}

static ps5c_err image_read(pkg_inner *in, void *buf, uint64_t size, uint64_t off) {
    uint64_t t0 = g_stats > 0 ? pal_time_ns() : 0;
    const pfs_node *n = &in->outer.fs.nodes[in->image_node];
    uint64_t stored = n->nblocks * in->outer.fs.block_size;
    if (off >= stored) { memset(buf, 0, size); return PS5C_OK; }
    uint64_t take = size < stored - off ? size : stored - off;
    ps5c_err e = pfs_read(&in->outer.fs, n, buf, take, off);
    if (!e && take < size) memset((uint8_t *)buf + take, 0, size - take);
    if (g_stats > 0) {
        STAT_ADD(g_disk_reads, 1);
        STAT_ADD(g_disk_bytes, take);
        STAT_ADD(g_disk_ns, pal_time_ns() - t0);
    }
    return e;
}

static uint32_t find_node(const pfs_image *fs, const char *name) {
    for (uint32_t i = 1; i < fs->count; i++)
        if (!fs->nodes[i].dir && !strcmp(fs->nodes[i].name, name)) return i;
    return UINT32_MAX;
}

/* naps_pkg_layout.dat: a 16-byte header of two u64 words
 *   word0: files-1 (bits 0..23), compression (24..25), keys-1 (26..27),
 *          shuffle patterns (28..31), last ublock index (32..54)
 *   word1: outer 64 KiB blocks (0..23), CblockInfo records - 2 (24..47)
 * then 8-byte outer block digests, 8-byte shuffle patterns, one 6-byte fidx
 * record per file (40-bit uncompressed start + type; the last one is the
 * terminal boundary, type 0x40 marks a continuation), 10-byte u2c records
 * (first CblockInfo of each ublock: a 24-bit base and 7 byte deltas per 8
 * ublocks), CblockInfo aligned to 8 bytes, and a zero footer.
 *
 * A CblockInfo record is a 72-bit little-endian value with bit 18 set for a
 * run base: coffsetEndMod256K (0..17), tweakIdxStart (20..47, the run's
 * first 64 KiB block in pfs_image.dat), coffsetStart256K (50..71). A normal
 * record: coffsetStartMod256K (0..17), terminal (19), uoffsetStart within
 * the ublock (20..37), clenEvenMinus1 (38..54), even half mode (55..57), odd
 * half mode (58..60), KDE predictor (61..66), shuffle index (68..71). A
 * span's lengths are the 18-bit distances to the next records. */

static uint32_t bits72(const uint8_t *r, int pos, int n) {
    uint64_t lo = rd_le64(r), hi = r[8];
    uint64_t v = pos >= 64 ? hi >> (pos - 64) : (lo >> pos) | (pos ? hi << (64 - pos) : 0);
    return (uint32_t)(v & ((1ull << n) - 1));
}

typedef struct cbi {
    bool run, term;
    uint32_t coff, uoff, cem1, tweak, c256;
    uint8_t even, odd, kde, shuf;
} cbi;

static cbi decode_cbi(const uint8_t *r) {
    cbi c = {0};
    c.run = bits72(r, 18, 1);
    c.coff = bits72(r, 0, 18);
    if (c.run) {
        c.tweak = bits72(r, 20, 28);
        c.c256 = bits72(r, 50, 22);
    } else {
        c.term = bits72(r, 19, 1);
        c.uoff = bits72(r, 20, 18);
        c.cem1 = bits72(r, 38, 17);
        c.even = (uint8_t)bits72(r, 55, 3);
        c.odd = (uint8_t)bits72(r, 58, 3);
        c.kde = (uint8_t)bits72(r, 61, 6);
        c.shuf = (uint8_t)bits72(r, 68, 4);
    }
    return c;
}

static uint32_t delta18(uint32_t next, uint32_t prev) {
    int32_t d = (int32_t)next - (int32_t)prev;
    return (uint32_t)(d <= 0 ? d + (int32_t)UBLOCK : d);
}

typedef struct naps_view {
    const uint8_t *fidx, *u2c, *cb;
    uint64_t nfiles, nublocks, ncbi;
} naps_view;

static uint64_t naps_u2c(const naps_view *v, uint64_t ub) {
    const uint8_t *r = v->u2c + (ub >> 3) * 10;
    uint64_t base = r[0] | (uint32_t)r[1] << 8 | (uint32_t)r[2] << 16;
    return (ub & 7) ? base + r[3 + (ub & 7) - 1] : base;
}

static uint64_t naps_fidx(const naps_view *v, uint64_t i, uint8_t *type) {
    const uint8_t *r = v->fidx + i * 6;
    uint64_t off = 0;
    for (int k = 0; k < 5; k++) off |= (uint64_t)r[k] << (8 * k);
    if (type) *type = r[5];
    return off;
}

/* builds the span table from naps_pkg_layout.dat */
static ps5c_err parse_naps(pkg_inner *in, const uint8_t *nb, uint64_t len) {
    if (len < 16) return PS5C_ERR_INVALID;
    uint64_t w0 = rd_le64(nb), w1 = rd_le64(nb + 8);
    naps_view v;
    v.nfiles = (w0 & 0xFFFFFF) + 1;
    uint64_t shuffles = (w0 >> 28) & 0xF, last_ub = (w0 >> 32) & 0x7FFFFF;
    v.nublocks = last_ub + 1;
    uint64_t outer = w1 & 0xFFFFFF;
    v.ncbi = ((w1 >> 24) & 0xFFFFFF) + 2;
    in->naps_files = (uint32_t)v.nfiles;
    in->naps_cblocks = (uint32_t)v.ncbi;
    in->naps_ublocks = (uint32_t)v.nublocks;
    uint64_t pos = 16 + outer * 8 + shuffles * 8;
    v.fidx = nb + pos;
    pos += v.nfiles * 6;
    v.u2c = nb + pos;
    pos += ((last_ub + 8) >> 3) * 10;
    pos = (pos + 7) & ~7ull;
    v.cb = nb + pos;
    pos += v.ncbi * 9;
    if (pos > len || v.nfiles < 2) return PS5C_ERR_INVALID;

    in->mount_size = naps_fidx(&v, v.nfiles - 1, NULL);
    in->blocks = malloc((size_t)v.ncbi * sizeof *in->blocks);
    if (!in->blocks) return PS5C_ERR_NOMEM;
    bool have_prev = false;
    uint64_t prev_coff = 0, prev_clen = 0, prev_tweak = 0;
    for (uint64_t fi = 0; fi + 1 < v.nfiles; fi++) {
        uint8_t type;
        uint64_t p = naps_fidx(&v, fi, &type), end = naps_fidx(&v, fi + 1, NULL);
        if (end < p) return PS5C_ERR_INVALID;
        if ((type & 0x40) || end == p) continue;
        /* the first span of the file: the ublock's record whose start matches */
        uint64_t ub = p >> 18;
        if (ub >= v.nublocks) return PS5C_ERR_INVALID;
        uint64_t ci = naps_u2c(&v, ub), stop = ub + 1 < v.nublocks ? naps_u2c(&v, ub + 1) : v.ncbi;
        for (; ci < stop && ci < v.ncbi; ci++) {
            cbi c = decode_cbi(v.cb + ci * 9);
            if (!c.run && !c.term && p == (ub << 18) + c.uoff) break;
        }
        if (ci >= stop || ci >= v.ncbi) return PS5C_ERR_INVALID;
        while (p < end) {
            if (ci + 1 >= v.ncbi) return PS5C_ERR_INVALID;
            cbi c = decode_cbi(v.cb + ci * 9);
            if (c.run) { ci++; continue; }
            if (c.term) return PS5C_ERR_INVALID;
            cbi nx = decode_cbi(v.cb + (ci + 1) * 9);
            uint64_t li = nx.run ? ci + 2 : ci + 1;
            if (li >= v.ncbi) return PS5C_ERR_INVALID;
            cbi lg = decode_cbi(v.cb + li * 9);
            uint32_t clen = delta18(nx.coff, c.coff), ulen = delta18(lg.uoff, c.uoff);
            if (ulen > UBLOCK || ulen > end - p) return PS5C_ERR_INVALID;
            uint64_t coff, tweak;
            cbi run = ci ? decode_cbi(v.cb + (ci - 1) * 9) : (cbi){0};
            if (ci && run.run) {
                coff = (uint64_t)run.c256 << 18 | c.coff;
                tweak = run.tweak;
            } else if (have_prev) {
                coff = prev_coff + prev_clen;
                tweak = prev_tweak + ((coff >> 16) - (prev_coff >> 16));
            } else {
                return PS5C_ERR_INVALID;
            }
            if (c.kde || (shuffles ? c.shuf != shuffles : c.shuf)) return PS5C_ERR_UNSUPPORTED;
            if (in->nblocks >= v.ncbi) return PS5C_ERR_INVALID;
            pkg_inner_block *b = &in->blocks[in->nblocks++];
            b->uoff = p;
            b->disk = tweak * 0x10000 + (coff & 0xFFFF);
            b->ulen = ulen;
            b->clen = clen;
            b->first = c.cem1 + 1;
            b->flags = (uint8_t)(((c.even & 4) ? 0x02 : 0) | ((c.even & 2) ? 0x01 : 0) | ((c.odd & 4) ? 0x20 : 0) |
                                 ((c.odd & 2) ? 0x10 : 0));
            /* a hole points at a shared 8/16-byte token, not at data */
            bool hole = b->first == 8 && c.even == 1 &&
                        ((c.odd == 1 && clen == 0x10 && ulen > 0x10) ||
                         (c.odd == 0 && clen == 8 && ulen > 8 && ulen <= CHUNK));
            b->kind = hole ? PKG_SPAN_ZERO : clen == ulen ? PKG_SPAN_STORED : PKG_SPAN_KRAKEN;
            have_prev = true;
            prev_coff = coff;
            prev_clen = clen;
            prev_tweak = tweak;
            p += ulen;
            ci++;
        }
    }
    return in->nblocks ? PS5C_OK : PS5C_ERR_INVALID;
}

#define CACHE_SLOTS 64u /* 16 MiB of decoded spans */

typedef struct pkg_slot {
    int64_t span;    /* -1 = empty */
    uint64_t tick;   /* last use */
    uint32_t refs;   /* readers copying out of it, or the thread filling it */
    bool ready;
    uint8_t *buf;
} pkg_slot;

typedef struct pkg_decoder {
    struct kraken_scratch *ks;
    uint8_t *cbuf;
    struct pkg_decoder *next;
} pkg_decoder;

static void decoder_free(pkg_decoder *d) {
    if (!d) return;
    kraken_scratch_free(d->ks);
    free(d->cbuf);
    free(d);
}

/* a slot holding the decoded span idx, referenced; release it with slot_put */
static ps5c_err slot_get(pkg_inner *in, uint32_t idx, pkg_slot **out) {
    pal_mutex_lock(in->lock);
    pkg_slot *victim = NULL;
    for (uint32_t i = 0; i < CACHE_SLOTS; i++) {
        pkg_slot *sl = &in->slots[i];
        if (sl->span == (int64_t)idx && sl->ready) {
            if (g_stats > 0) g_hits++;
            sl->refs++;
            sl->tick = ++in->tick;
            pal_mutex_unlock(in->lock);
            *out = sl;
            return PS5C_OK;
        }
        if (!sl->refs && (!victim || sl->tick < victim->tick)) victim = sl;
    }
    if (!victim) { pal_mutex_unlock(in->lock); return PS5C_ERR_BUSY; }
    victim->span = idx;
    victim->ready = false;
    victim->refs = 1;
    victim->tick = ++in->tick;
    pkg_decoder *d = in->decoders;
    if (d) in->decoders = d->next;
    pal_mutex_unlock(in->lock);

    ps5c_err e = PS5C_OK;
    if (!d && (d = calloc(1, sizeof *d))) {
        d->ks = kraken_scratch_new();
        d->cbuf = malloc(CBUF_MAX);
        if (!d->ks || !d->cbuf) { decoder_free(d); d = NULL; }
    }
    const pkg_inner_block *b = &in->blocks[idx];
    kraken_status st = KRAKEN_MALFORMED;
    if (!d) e = PS5C_ERR_NOMEM;
    else if (b->clen > CBUF_MAX) e = PS5C_ERR_INVALID;
    else e = image_read(in, d->cbuf, b->clen, b->disk);
    if (!e && b->kind == PKG_SPAN_ZLIB) {
        uint64_t t0 = g_stats > 0 ? pal_time_ns() : 0;
        long got = inflate_zlib(d->cbuf, b->clen, victim->buf, b->ulen);
        st = got == (long)b->ulen ? KRAKEN_OK : KRAKEN_MALFORMED;
        if (st != KRAKEN_OK) memset(victim->buf, 0, b->ulen);
        if (g_stats > 0) STAT_ADD(g_decode_ns, pal_time_ns() - t0);
    } else if (!e) {
        uint64_t t0 = g_stats > 0 ? pal_time_ns() : 0;
        st = kraken_decode_block(d->ks, d->cbuf, b->clen, b->flags, b->ulen > CHUNK ? (int)b->first : 0, victim->buf,
                                 b->ulen);
        if (st != KRAKEN_OK) memset(victim->buf, 0, b->ulen);
        if (g_stats > 0) STAT_ADD(g_decode_ns, pal_time_ns() - t0);
    }
    pal_mutex_lock(in->lock);
    if (g_stats > 0) g_misses++;
    if (d) { d->next = in->decoders; in->decoders = d; }
    if (e) {
        victim->span = -1;
        victim->refs = 0;
    } else {
        victim->ready = true;
        in->last_status = (int)st;
        if (st != KRAKEN_OK) in->bad_blocks++;
    }
    pal_mutex_unlock(in->lock);
    if (e) return e;
    *out = victim;
    return PS5C_OK;
}

static void slot_put(pkg_inner *in, pkg_slot *sl) {
    pal_mutex_lock(in->lock);
    sl->refs--;
    pal_mutex_unlock(in->lock);
}

void pkg_inner_flush(pkg_inner *in) {
    pal_mutex_lock(in->lock);
    for (uint32_t i = 0; i < CACHE_SLOTS; i++)
        if (!in->slots[i].refs) in->slots[i].span = -1;
    pal_mutex_unlock(in->lock);
}

ps5c_err pkg_inner_read(pkg_inner *in, void *buf, uint64_t size, uint64_t off) {
    if (g_stats < 0) g_stats = getenv("PS5C_LOG_PKG_STATS") != NULL;
    if (g_stats > 0) {
        STAT_ADD(g_calls, 1);
        STAT_ADD(g_bytes, size);
        stats_tick();
    }
    uint8_t *out = buf;
    while (size) {
        /* last span starting at or before off */
        uint32_t lo = 0, hi = in->nblocks;
        while (hi - lo > 1) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (in->blocks[mid].uoff <= off) lo = mid; else hi = mid;
        }
        const pkg_inner_block *b = &in->blocks[lo];
        uint64_t n;
        if (off < b->uoff || off >= b->uoff + b->ulen) {
            /* not covered by any span: padding */
            uint64_t next = off < b->uoff ? b->uoff : lo + 1 < in->nblocks ? in->blocks[lo + 1].uoff : UINT64_MAX;
            n = next - off < size ? next - off : size;
            memset(out, 0, n);
        } else {
            uint64_t in_blk = off - b->uoff;
            n = b->ulen - in_blk < size ? b->ulen - in_blk : size;
            if (b->kind == PKG_SPAN_ZERO) {
                memset(out, 0, n);
            } else if (b->kind == PKG_SPAN_STORED) {
                ps5c_err e = image_read(in, out, n, b->disk + in_blk);
                if (e) return e;
            } else {
                pkg_slot *sl;
                ps5c_err e = slot_get(in, lo, &sl);
                if (e) return e;
                memcpy(out, sl->buf + in_blk, n);
                slot_put(in, sl);
            }
        }
        out += n;
        off += n;
        size -= n;
    }
    return PS5C_OK;
}

ps5c_err pkg_inner_read_file(pkg_inner *in, const pkg_inner_node *n, void *buf, uint64_t size, uint64_t off) {
    if (off > n->size || size > n->size - off) return PS5C_ERR_INVALID;
    return pkg_inner_read(in, buf, size, n->offset + off);
}

static ps5c_err add_node(pkg_inner *in, pkg_inner_node **out) {
    if (in->count == in->cap) {
        uint32_t cap = in->cap ? in->cap * 2 : 256;
        pkg_inner_node *n = realloc(in->nodes, cap * sizeof *n);
        if (!n) return PS5C_ERR_NOMEM;
        in->nodes = n;
        in->cap = cap;
    }
    *out = &in->nodes[in->count++];
    memset(*out, 0, sizeof **out);
    return PS5C_OK;
}

static bool is_superblock(pkg_inner *in, uint64_t off) {
    uint8_t h[16];
    return !pkg_inner_read(in, h, sizeof h, off) && rd_le64(h) == 2 && rd_le64(h + 8) == 20130315;
}

static ps5c_err load_tree(pkg_inner *in) {
    /* the metadata superblock sits near the top of the image */
    uint64_t sb = UINT64_MAX;
    uint64_t p = (in->mount_size - 0x10000) & ~0xFFFFull;
    for (uint32_t k = 0; k < 65536; k++, p -= 0x10000) {
        if (is_superblock(in, p)) { sb = p; break; }
        if (!p) break;
    }
    if (sb == UINT64_MAX) return PS5C_ERR_NOT_FOUND;
    uint8_t s[0x40];
    ps5c_err e = pkg_inner_read(in, s, sizeof s, sb);
    if (e) return e;
    in->superblock = sb;
    in->block_size = rd_le32(s + 0x20);
    uint64_t ninodes = rd_le64(s + 0x30);
    if (in->block_size < 0x1000 || in->block_size > (1u << 20) || !ninodes || ninodes > MAX_INODES)
        return PS5C_ERR_INVALID;
    uint8_t *it = malloc((size_t)(ninodes * INODE_SIZE));
    uint8_t *seen = calloc((size_t)ninodes, 1);
    if (!it || !seen) { free(it); free(seen); return PS5C_ERR_NOMEM; }
    e = pkg_inner_read(in, it, ninodes * INODE_SIZE, sb + in->block_size);
    pkg_inner_node *root = NULL;
    if (!e) e = add_node(in, &root);
    if (!e) {
        const uint8_t *d = it;
        root->name = calloc(1, 1);
        root->dir = (rd_le16(d) & 0x4000) != 0;
        root->size = rd_le64(d + 8);
        root->offset = rd_le64(d + 0x60);
        seen[0] = 1;
        if (!root->name) e = PS5C_ERR_NOMEM;
    }
    uint8_t *dirbuf = NULL;
    for (uint32_t ni = 0; !e && ni < in->count; ni++) {
        if (!in->nodes[ni].dir) continue;
        uint64_t dsize = in->nodes[ni].size, doff = in->nodes[ni].offset;
        if (!dsize || dsize > (64u << 20) || doff >= in->mount_size) continue;
        uint8_t *nbuf = realloc(dirbuf, (size_t)dsize);
        if (!nbuf) { e = PS5C_ERR_NOMEM; break; }
        dirbuf = nbuf;
        if ((e = pkg_inner_read(in, dirbuf, dsize, doff))) break;
        uint32_t last = 0;
        for (uint64_t q = 0; q + 16 <= dsize;) {
            uint32_t ino = rd_le32(dirbuf + q), type = rd_le32(dirbuf + q + 4), len = rd_le32(dirbuf + q + 8),
                     ent = rd_le32(dirbuf + q + 12);
            if (!ent || ent > dsize - q || len > ent - 16) {
                /* rest of this block is padding */
                uint64_t nxt = (q / in->block_size + 1) * in->block_size;
                if (nxt <= q || nxt >= dsize) break;
                q = nxt;
                continue;
            }
            uint64_t name_at = q + 16;
            q += ent;
            if ((type != DIRENT_FILE && type != DIRENT_DIR) || ino >= ninodes || seen[ino]) continue;
            seen[ino] = 1;
            pkg_inner_node *n;
            if ((e = add_node(in, &n))) break;
            n->name = malloc(len + 1);
            if (!n->name) { e = PS5C_ERR_NOMEM; break; }
            memcpy(n->name, dirbuf + name_at, len);
            n->name[len] = 0;
            const uint8_t *d = it + (uint64_t)ino * INODE_SIZE;
            n->parent = ni;
            n->dir = type == DIRENT_DIR;
            n->size = rd_le64(d + 8);
            n->offset = rd_le64(d + 0x60);
            uint32_t idx = in->count - 1;
            if (ni == 0 && n->dir && !strcmp(n->name, "uroot")) in->uroot = idx;
            if (last) in->nodes[last].sibling = idx;
            else in->nodes[ni].child = idx;
            last = idx;
        }
    }
    free(dirbuf);
    free(it);
    free(seen);
    if (!e && !in->uroot) e = PS5C_ERR_NOT_FOUND;
    return e;
}

/* PS4: pfs_image.dat is a PFSC stream: "PFSC" header (block size at 0x10,
 * offset table at 0x18, data length at 0x28; LibOrbisPkg's PFSCReader), a
 * table of block start offsets (one more than the blocks), and the blocks:
 * stored when a block is block-size long, else zlib. */
static ps5c_err parse_pfsc(pkg_inner *in) {
    uint8_t h[0x30];
    const pfs_node *n = &in->outer.fs.nodes[in->image_node];
    ps5c_err e = image_read(in, h, sizeof h, 0);
    if (e) return e;
    if (memcmp(h, "PFSC", 4)) return PS5C_ERR_INVALID;
    uint64_t bsz = rd_le64(h + 0x10), table = rd_le64(h + 0x18), len = rd_le64(h + 0x28);
    if (!bsz || bsz > UBLOCK || (bsz & 0xFFF) || !len) return PS5C_ERR_UNSUPPORTED;
    uint64_t nb = (len + bsz - 1) / bsz;
    if (nb > (1u << 26) || table + (nb + 1) * 8 > n->size) return PS5C_ERR_INVALID;
    uint8_t *t = malloc((size_t)(nb + 1) * 8);
    in->blocks = calloc((size_t)nb, sizeof *in->blocks);
    if (!t || !in->blocks) { free(t); return PS5C_ERR_NOMEM; }
    e = image_read(in, t, (nb + 1) * 8, table);
    for (uint64_t i = 0; !e && i < nb; i++) {
        uint64_t a = rd_le64(t + i * 8), z = rd_le64(t + i * 8 + 8);
        if (z < a || z > n->size || z - a > CBUF_MAX) { e = PS5C_ERR_INVALID; break; }
        pkg_inner_block *b = &in->blocks[in->nblocks++];
        b->uoff = i * bsz;
        b->ulen = (uint32_t)(len - b->uoff < bsz ? len - b->uoff : bsz);
        b->disk = a;
        b->clen = (uint32_t)(z - a);
        b->kind = !b->clen ? PKG_SPAN_ZERO : b->clen == bsz || b->clen == b->ulen ? PKG_SPAN_STORED : PKG_SPAN_ZLIB;
    }
    free(t);
    in->mount_size = len;
    return e;
}

static ps5c_err inner_pfs_read(void *user, void *buf, uint64_t size, uint64_t off) {
    return pkg_inner_read(user, buf, size, off);
}

/* PS4: the inner PFS through pal_pfs, as pkg_inner nodes (contiguous files) */
static ps5c_err load_tree_pfs(pkg_inner *in) {
    pfs_image fs;
    ps5c_err e = pfs_open(&fs, inner_pfs_read, in, 0);
    if (e) return e;
    in->superblock = 0;
    in->block_size = fs.block_size;
    uint32_t scattered = 0;
    for (uint32_t i = 0; !e && i < fs.count; i++) {
        const pfs_node *p = &fs.nodes[i];
        pkg_inner_node *n;
        if ((e = add_node(in, &n))) break;
        n->name = strdup(p->name);
        if (!n->name) { e = PS5C_ERR_NOMEM; break; }
        n->parent = p->parent;
        n->child = p->child;
        n->sibling = p->sibling;
        n->dir = p->dir;
        n->size = p->dir ? 0 : p->size;
        n->offset = p->start >= 0 ? (uint64_t)p->start * fs.block_size : 0;
        if (!p->dir && (p->blocks || (p->flags & 1))) {
            scattered++;
            n->size = 0; /* not one run of the image: not readable this way */
        }
    }
    in->uroot = fs.uroot;
    if (scattered) LOG_W(CORE, "PS4 package: %u files are scattered or compressed on their own; they read as empty", scattered);
    pfs_close(&fs);
    return e;
}

ps5c_err pkg_inner_open(const char *path, const pkg_info *info, pkg_inner **out) {
    *out = NULL;
    pkg_inner *in = calloc(1, sizeof *in);
    if (!in) return PS5C_ERR_NOMEM;
    in->last_status = -1;
    ps5c_err e = pkg_outer_open(path, info, &in->outer);
    if (e) { free(in); return e; }
    if (info->kind == PKG_PS4) {
        in->image_node = find_node(&in->outer.fs, "pfs_image.dat");
        e = in->image_node == UINT32_MAX ? PS5C_ERR_UNSUPPORTED : parse_pfsc(in);
        if (!e) {
            in->slots = calloc(CACHE_SLOTS, sizeof *in->slots);
            if (!in->slots || pal_mutex_create(&in->lock)) e = PS5C_ERR_NOMEM;
            for (uint32_t i = 0; !e && i < CACHE_SLOTS; i++) {
                in->slots[i].span = -1;
                if (!(in->slots[i].buf = malloc(UBLOCK))) e = PS5C_ERR_NOMEM;
            }
        }
        if (e) { pkg_inner_close(in); return e; }
        in->tree_err = load_tree_pfs(in);
        if (in->tree_err) {
            for (uint32_t i = 0; i < in->count; i++) free(in->nodes[i].name);
            in->count = 0;
        }
        *out = in;
        return PS5C_OK;
    }
    in->image_node = find_node(&in->outer.fs, "pfs_image.dat");
    uint32_t naps = find_node(&in->outer.fs, "naps_pkg_layout.dat");
    if (in->image_node == UINT32_MAX || naps == UINT32_MAX) e = PS5C_ERR_UNSUPPORTED;
    uint8_t *nb = NULL;
    uint64_t nlen = 0;
    if (!e) {
        const pfs_node *n = &in->outer.fs.nodes[naps];
        nlen = n->size;
        if (!nlen || nlen > (256u << 20)) e = PS5C_ERR_INVALID;
        else if (!(nb = malloc((size_t)nlen))) e = PS5C_ERR_NOMEM;
        else e = pfs_read(&in->outer.fs, n, nb, nlen, 0);
    }
    if (!e) e = parse_naps(in, nb, nlen);
    const char *dump = getenv("PS5C_PKG_NAPS"); /* debug: save the layout blob */
    if (nb && dump && *dump) {
        FILE *f = fopen(dump, "wb");
        if (f) { fwrite(nb, 1, (size_t)nlen, f); fclose(f); }
    }
    free(nb);
    if (!e) {
        in->slots = calloc(CACHE_SLOTS, sizeof *in->slots);
        if (!in->slots || pal_mutex_create(&in->lock)) e = PS5C_ERR_NOMEM;
        for (uint32_t i = 0; !e && i < CACHE_SLOTS; i++) {
            in->slots[i].span = -1;
            if (!(in->slots[i].buf = malloc(UBLOCK))) e = PS5C_ERR_NOMEM;
        }
    }
    if (e) { pkg_inner_close(in); return e; }
    in->tree_err = load_tree(in);
    if (in->tree_err) {
        for (uint32_t i = 0; i < in->count; i++) free(in->nodes[i].name);
        in->count = 0;
    }
    *out = in;
    return PS5C_OK;
}

void pkg_inner_close(pkg_inner *in) {
    if (!in) return;
    for (uint32_t i = 0; i < in->count; i++) free(in->nodes[i].name);
    free(in->nodes);
    free(in->blocks);
    for (uint32_t i = 0; in->slots && i < CACHE_SLOTS; i++) free(in->slots[i].buf);
    free(in->slots);
    while (in->decoders) {
        pkg_decoder *d = in->decoders;
        in->decoders = d->next;
        decoder_free(d);
    }
    if (in->lock) pal_mutex_destroy(in->lock);
    pkg_outer_close(&in->outer);
    free(in);
}

void pkg_inner_path(const pkg_inner *in, uint32_t node, char *out, size_t cap) {
    uint32_t chain[64];
    int depth = 0;
    for (uint32_t c = node; c && c != in->uroot && depth < 64; c = in->nodes[c].parent) chain[depth++] = c;
    size_t len = 0;
    if (cap) out[0] = 0;
    for (int i = depth - 1; i >= 0 && cap; i--) {
        int w = snprintf(out + len, cap - len, "%s%s", len ? "/" : "", in->nodes[chain[i]].name);
        if (w < 0 || (size_t)w >= cap - len) break;
        len += (size_t)w;
    }
}

uint32_t pkg_inner_lookup(const pkg_inner *in, const char *path) {
    if (!in->count) return UINT32_MAX;
    uint32_t cur = in->uroot;
    while (*path) {
        const char *sl = strchr(path, '/');
        size_t n = sl ? (size_t)(sl - path) : strlen(path);
        uint32_t c = in->nodes[cur].child;
        while (c && (strlen(in->nodes[c].name) != n || memcmp(in->nodes[c].name, path, n))) c = in->nodes[c].sibling;
        if (!c) return UINT32_MAX;
        cur = c;
        path += n;
        if (*path == '/') path++;
    }
    return cur;
}
