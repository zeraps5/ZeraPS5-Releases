/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Read-only raw exFAT partition images, per Microsoft's exFAT specification:
 * https://learn.microsoft.com/en-us/windows/win32/fileio/exfat-specification
 * Metadata and coalesced extents are cached; file data stays in the image.
 * All data I/O is positional. Metadata is immutable after publication.
 * A fake PS5 package (.pkg) is exposed the same way: its inner file system
 * is decoded on demand by pal_pkg_inner, nothing is extracted. */
#include "pal/pal_exfat.h"
#include "pal/pal_pkg_inner.h"
#include "base/endian.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_DIR_BYTES (64u << 20)
#define MAX_NODES 1000000u
#define MAX_FAT_BYTES (128u << 20)
typedef struct extent { uint64_t logical, physical, size; } extent;
typedef struct node {
    char *name;
    uint16_t wide[256];
    uint32_t parent, first, child, sibling;
    bool dir;
    uint64_t size, valid;
    uint64_t pkg_offset; /* .pkg: start in the inner image, or in the .pkg file for an entry */
    bool pkg_entry;      /* .pkg: a metadata entry (param.json, ...) read from the file itself */
    uint32_t pkg_file;   /* .pkg: inner node + 1 of a file read through its block list (PS4), 0 = at pkg_offset */
    extent *runs;
    uint32_t nruns, run_cap;
} node;
typedef struct volume {
    char *path;
    pal_file *file;
    uint64_t size, heap, cluster;
    uint32_t count;
    bool have_upcase, have_game;
    uint32_t game_root, decrypted_root;
    uint8_t *fat;
    uint16_t upcase[65536];
    node *nodes;
    uint32_t nnodes, cap;
    pkg_inner *pkg;      /* a .pkg volume: reads go through its span decoder */
    struct volume *next;
} volume;
struct exfat_file { volume *v; uint32_t id; uint64_t pos; pal_mutex *lock; };
static volume *g_volumes;
static pal_mutex *g_lock;

static bool sep(char c) { return c == '/' || c == '\\'; }
static const char *const g_exts[] = {".exfat", ".pkg"};
/* length of the image extension at q, 0 if none */
static size_t ext_at(const char *q) {
    for (size_t e = 0; e < PS5C_ARRAY_COUNT(g_exts); e++) {
        const char *ext = g_exts[e];
        size_t k = 0;
        for (; ext[k] && q[k]; k++) {
            char c = q[k];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != ext[k]) break;
        }
        if (!ext[k]) return k;
    }
    return 0;
}
/* a host folder named like an image (the user folder of a game run from a
 * .pkg is named after the package) holds host files, not an image */
static bool host_dir(const char *p, size_t len) {
    char dir[1024];
    if (len >= sizeof dir) return false;
    memcpy(dir, p, len);
    dir[len] = 0;
    pal_path_info info;
    return pal_path_stat(dir, &info) == PS5C_OK && info.is_dir;
}
static const char *boundary(const char *p) {
    for (const char *q = p; *q; q++) {
        if (*q != '.') continue;
        size_t k = ext_at(q);
        if (k && sep(q[k]) && !host_dir(p, (size_t)(q + k - p))) return q + k;
    }
    return NULL;
}
bool exfat_path(const char *p) { return p && boundary(p); }
bool exfat_image_path(const char *p) {
    if (!p) return false;
    const char *dot = strrchr(p, '.');
    if (!dot) return false;
    size_t k = ext_at(dot);
    return k && !dot[k];
}
ps5c_err exfat_init(void) { return g_lock ? PS5C_OK : pal_mutex_create(&g_lock); }
static void free_volume(volume *v) {
    if (!v) return;
    for (uint32_t i = 0; i < v->nnodes; i++) { free(v->nodes[i].name); free(v->nodes[i].runs); }
    free(v->nodes); free(v->fat); free(v->path);
    if (v->file) pal_file_close(v->file);
    pkg_inner_close(v->pkg);
    free(v);
}
void exfat_shutdown(void) {
    while (g_volumes) { volume *v = g_volumes; g_volumes = v->next; free_volume(v); }
    if (g_lock) pal_mutex_destroy(g_lock);
    g_lock = NULL;
}
static ps5c_err raw(volume *v, uint64_t off, void *buf, uint64_t n) {
    if (off > v->size || n > v->size - off) return PS5C_ERR_INVALID;
    uint64_t got = 0;
    ps5c_err e = pal_file_pread(v->file, buf, n, off, &got);
    return e == PS5C_OK && got != n ? PS5C_ERR_IO : e;
}
static bool cluster_ok(volume *v, uint32_t c) { return c >= 2 && (uint64_t)c < (uint64_t)v->count + 2; }
static ps5c_err add_run(node *n, uint64_t off, uint64_t bytes) {
    if (n->nruns && n->runs[n->nruns-1].physical + n->runs[n->nruns-1].size == off) {
        n->runs[n->nruns-1].size += bytes;
    } else {
        if (n->nruns == n->run_cap) {
            uint32_t cap = n->run_cap ? n->run_cap * 2 : 4;
            extent *r = (extent *)realloc(n->runs, (size_t)cap * sizeof(extent));
            if (!r) return PS5C_ERR_NOMEM;
            n->runs = r; n->run_cap = cap;
        }
        extent *r = n->runs;
        uint64_t logical = n->nruns ? r[n->nruns-1].logical + r[n->nruns-1].size : 0;
        r[n->nruns++] = (extent){logical, off, bytes};
    }
    return PS5C_OK;
}
static ps5c_err chain(volume *v, node *n, bool contiguous, bool root) {
    if (!root && !n->size) return n->first ? PS5C_ERR_INVALID : PS5C_OK;
    if (!cluster_ok(v, n->first) || (!root && (n->valid > n->size || n->size > v->size))) return PS5C_ERR_INVALID;
    uint64_t needed = root ? v->count : (n->size + v->cluster - 1) / v->cluster;
    if (needed > v->count) return PS5C_ERR_INVALID;
    if (contiguous) {
        if ((uint64_t)n->first - 2 + needed > v->count) return PS5C_ERR_INVALID;
        return add_run(n, v->heap + ((uint64_t)n->first - 2) * v->cluster, needed * v->cluster);
    }
    uint8_t *seen = (uint8_t *)calloc(((size_t)v->count + 9) / 8, 1);
    if (!seen) return PS5C_ERR_NOMEM;
    uint32_t c = n->first;
    ps5c_err err = PS5C_OK;
    uint64_t walked = 0;
    for (; walked < needed; walked++) {
        if (!cluster_ok(v, c) || (seen[c/8] & (1u << (c%8)))) { err = PS5C_ERR_INVALID; break; }
        seen[c/8] |= (uint8_t)(1u << (c%8));
        if (root && (walked + 1) * v->cluster > MAX_DIR_BYTES) { err = PS5C_ERR_UNSUPPORTED; break; }
        err = add_run(n, v->heap + ((uint64_t)c - 2) * v->cluster, v->cluster);
        if (err) break;
        c = rd_le32(v->fat + (uint64_t)c * 4);
        if (c >= 0xfffffff8u) { walked++; break; }
    }
    if (!err && ((!root && walked != needed) || (root && c < 0xfffffff8u))) err = PS5C_ERR_INVALID;
    if (root && !err) n->valid = n->size = walked * v->cluster;
    free(seen);
    return err;
}
static ps5c_err read_node(volume *v, const node *n, void *buf, uint64_t bytes, uint64_t off, uint64_t *got) {
    if (got) *got = 0;
    if (off >= n->size) return PS5C_OK;
    bytes = PS5C_MIN(bytes, n->size - off);
    if (bytes > SIZE_MAX) return PS5C_ERR_OUT_OF_RANGE;
    if (v->pkg) {
        ps5c_err err = n->pkg_entry ? raw(v, n->pkg_offset + off, buf, bytes)
                       : n->pkg_file ? pkg_inner_read_file(v->pkg, &v->pkg->nodes[n->pkg_file - 1], buf, bytes, off)
                                     : pkg_inner_read(v->pkg, buf, bytes, n->pkg_offset + off);
        if (!err && got) *got = bytes;
        return err;
    }
    uint64_t data = off < n->valid ? PS5C_MIN(bytes, n->valid - off) : 0;
    uint64_t done = 0;
    /* Binary search the immutable coalesced extent index. */
    uint32_t lo = 0, hi = n->nruns;
    while (lo < hi) { uint32_t m = lo + (hi-lo)/2; if (n->runs[m].logical + n->runs[m].size <= off) lo = m+1; else hi = m; }
    for (uint32_t i = lo; i < n->nruns && done < data; i++) {
        const extent *r = &n->runs[i];
        uint64_t within = off + done - r->logical;
        uint64_t want = PS5C_MIN(data - done, r->size - within);
        ps5c_err err = raw(v, r->physical + within, (uint8_t *)buf + done, want);
        if (err) return err;
        done += want;
    }
    if (done != data) return PS5C_ERR_INVALID;
    if (bytes > data) memset((uint8_t *)buf + data, 0, (size_t)(bytes-data));
    if (got) *got = bytes;
    return PS5C_OK;
}
static bool name_utf8(node *n, const uint16_t *w, unsigned len) {
    char buf[1021]; size_t used = 0;
    for (unsigned i = 0; i < len; i++) {
        uint32_t c = w[i];
        n->wide[i] = w[i];
        if (!c || c == '/' || c == '\\') return false;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == len || w[i] < 0xdc00 || w[i] > 0xdfff) return false;
            n->wide[i] = w[i]; c = 0x10000 + ((c - 0xd800) << 10) + w[i] - 0xdc00;
        } else if (c >= 0xdc00 && c <= 0xdfff) return false;
        if (c < 0x80) buf[used++] = (char)c;
        else if (c < 0x800) { buf[used++] = (char)(0xc0 | (c>>6)); buf[used++] = (char)(0x80 | (c&63)); }
        else if (c < 0x10000) { buf[used++] = (char)(0xe0 | (c>>12)); buf[used++] = (char)(0x80 | ((c>>6)&63)); buf[used++] = (char)(0x80 | (c&63)); }
        else { buf[used++] = (char)(0xf0 | (c>>18)); buf[used++] = (char)(0x80 | ((c>>12)&63)); buf[used++] = (char)(0x80 | ((c>>6)&63)); buf[used++] = (char)(0x80 | (c&63)); }
    }
    buf[used] = 0;
    if (!used || !strcmp(buf, ".") || !strcmp(buf, "..")) return false;
    n->name = (char *)malloc(used+1);
    if (!n->name) return false;
    memcpy(n->name, buf, used+1);
    return true;
}
static ps5c_err push_node(volume *v, node *n, uint32_t *id) {
    if (v->nnodes == MAX_NODES) return PS5C_ERR_UNSUPPORTED;
    if (v->nnodes == v->cap) {
        uint32_t cap = v->cap ? v->cap * 2 : 64;
        node *p = (node *)realloc(v->nodes, (size_t)cap * sizeof(node));
        if (!p) return PS5C_ERR_NOMEM;
        v->nodes = p; v->cap = cap;
    }
    *id = v->nnodes; v->nodes[v->nnodes++] = *n;
    return PS5C_OK;
}
static ps5c_err parse_dir(volume *v, uint32_t id, unsigned depth) {
    node dir = v->nodes[id]; /* realloc of the node table must not invalidate this */
    if (depth > 64 || dir.size > MAX_DIR_BYTES) return PS5C_ERR_UNSUPPORTED;
    uint8_t *b = (uint8_t *)malloc((size_t)dir.size);
    if (!b) return PS5C_ERR_NOMEM;
    ps5c_err err = read_node(v, &dir, b, dir.size, 0, NULL);
    uint32_t last = 0;
    for (uint64_t at = 0; !err && at + 32 <= dir.size; at += 32) {
        uint8_t *e = b + at;
        if (!e[0]) break;
        if (!(e[0] & 0x80)) continue; /* deleted */
        if (e[0] == 0x82 && id == 0) { /* Unicode up-case table */
            if (v->have_upcase) { err = PS5C_ERR_INVALID; break; }
            v->have_upcase = true;
            node tab = {0}; tab.first = rd_le32(e+20); tab.size = tab.valid = rd_le64(e+24);
            if (!tab.size || tab.size > 131072 || (tab.size & 1)) { err = PS5C_ERR_INVALID; break; }
            err = chain(v, &tab, false, false);
            uint8_t *t = !err ? (uint8_t *)malloc((size_t)tab.size) : NULL;
            if (!err && !t) err = PS5C_ERR_NOMEM;
            if (!err) err = read_node(v, &tab, t, tab.size, 0, NULL);
            uint32_t sum = 0, index = 0;
            if (!err) {
                for (uint64_t k = 0; k < tab.size; k++) sum = (sum>>1 | sum<<31) + t[k];
                if (sum != rd_le32(e+4)) err = PS5C_ERR_INVALID;
                for (uint64_t k = 0; !err && k < tab.size; k += 2) {
                    uint16_t x = rd_le16(t+k);
                    if (index >= 65536) { err = PS5C_ERR_INVALID; break; }
                    if (x == 0xffff && index != 65535 && tab.size != 131072) {
                        if (k+4 > tab.size) { err = PS5C_ERR_INVALID; break; }
                        uint32_t skip = rd_le16(t+k+2); k += 2;
                        if (!skip || skip > 65536-index) { err = PS5C_ERR_INVALID; break; }
                        while (skip--) { v->upcase[index] = (uint16_t)index; index++; }
                    } else v->upcase[index++] = x;
                }
                if (index != 65536) err = PS5C_ERR_INVALID;
            }
            free(t); free(tab.runs); continue;
        }
        if (e[0] != 0x85) {
            if (!(e[0]&0x20) && e[0] != 0x81 && e[0] != 0x83) err = PS5C_ERR_UNSUPPORTED;
            continue;
        }
        uint32_t bytes = ((uint32_t)e[1]+1)*32;
        if (e[1] < 2 || bytes > dir.size-at || e[32] != 0xc0) { err = PS5C_ERR_INVALID; break; }
        uint16_t sum = 0;
        for (uint32_t k = 0; k < bytes; k++) if (k != 2 && k != 3) sum = (uint16_t)((sum>>1 | sum<<15) + e[k]);
        if (sum != rd_le16(e+2)) { err = PS5C_ERR_INVALID; break; }
        uint8_t *stream = e+32;
        unsigned len = stream[3], names = 0;
        uint16_t wide[256] = {0};
        for (uint32_t k = 64; k < bytes; k += 32) {
            if (e[k] == 0xc1) {
                for (unsigned j = 0; j < 15 && names < len; j++) wide[names++] = rd_le16(e+k+2+j*2);
            } else if (!(e[k]&0x20)) { err = PS5C_ERR_UNSUPPORTED; break; }
        }
        if (err || !len || names != len || !(stream[1]&1)) { err = PS5C_ERR_INVALID; break; }
        node n = {0}; n.parent = id; n.dir = (rd_le16(e+4)&16) != 0;
        n.first = rd_le32(stream+20); n.size = rd_le64(stream+24); n.valid = rd_le64(stream+8);
        if (!name_utf8(&n, wide, len)) { err = PS5C_ERR_INVALID; break; }
        err = chain(v, &n, (stream[1]&2) != 0, false);
        uint32_t child = 0;
        if (!err) err = push_node(v, &n, &child);
        if (err) { free(n.name); free(n.runs); break; }
        if (last) v->nodes[last].sibling = child; else v->nodes[id].child = child;
        last = child;
        if (n.dir) err = parse_dir(v, child, depth+1);
        at += bytes-32;
    }
    free(b); return err;
}
static bool equal_name(volume *v, const uint16_t *w, const char *p, size_t len);
/* UTF-8 name to the NUL-terminated UTF-16 form equal_name compares */
static bool wide_name(uint16_t *w, const char *s) {
    unsigned n = 0;
    for (size_t i = 0; s[i];) {
        uint32_t c = (uint8_t)s[i++];
        unsigned more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (more) c &= 0x3fu >> more;
        while (more--) {
            if (((uint8_t)s[i] & 0xc0) != 0x80) return false;
            c = c << 6 | ((uint8_t)s[i++] & 63);
        }
        if (c >= 0x10000) {
            if (n + 2 >= 256) return false;
            w[n++] = (uint16_t)(0xd800 + ((c - 0x10000) >> 10));
            c = 0xdc00 + ((c - 0x10000) & 1023);
        }
        if (n + 1 >= 256) return false;
        w[n++] = (uint16_t)c;
    }
    w[n] = 0;
    return n > 0;
}
/* child of dir named name (exact), 0 when none */
static uint32_t pkg_child(volume *v, uint32_t dir, const char *name) {
    for (uint32_t c = v->nodes[dir].child; c; c = v->nodes[c].sibling)
        if (!strcmp(v->nodes[c].name, name)) return c;
    return 0;
}
/* appends a node below dir */
static ps5c_err pkg_add(volume *v, uint32_t dir, const char *name, bool is_dir, uint64_t size, uint64_t offset,
                        uint32_t *out) {
    node n = {0};
    n.parent = dir; n.dir = is_dir;
    n.size = n.valid = size;
    n.pkg_offset = offset;
    n.pkg_entry = !is_dir;
    if (!wide_name(n.wide, name)) return PS5C_ERR_INVALID;
    size_t len = strlen(name);
    if (!(n.name = (char *)malloc(len + 1))) return PS5C_ERR_NOMEM;
    memcpy(n.name, name, len + 1);
    uint32_t id;
    ps5c_err err = push_node(v, &n, &id);
    if (err) { free(n.name); return err; }
    uint32_t c = v->nodes[dir].child;
    if (!c) v->nodes[dir].child = id;
    else {
        while (v->nodes[c].sibling) c = v->nodes[c].sibling;
        v->nodes[c].sibling = id;
    }
    *out = id;
    return PS5C_OK;
}
/* The package's plaintext metadata entries (param.json, playgo-chunk.dat,
 * icons, trophy2/...) are what a console shows in /app0/sce_sys; the inner
 * file system does not hold them. */
static ps5c_err pkg_add_entries(volume *v, const pkg_info *info) {
    uint32_t sys = pkg_child(v, v->game_root, "sce_sys");
    if (!sys || !v->nodes[sys].dir) {
        ps5c_err err = pkg_add(v, v->game_root, "sce_sys", true, 0, 0, &sys);
        if (err) return err;
    }
    for (uint32_t i = 0; i < info->nentries; i++) {
        const pkg_entry *e = &info->entries[i];
        if (!e->name[0] || (e->flags1 & PKG_ENTRY_ENCRYPTED) || e->offset > v->size || e->size > v->size - e->offset)
            continue;
        uint32_t dir = sys;
        char part[64];
        const char *p = e->name;
        for (const char *slash; (slash = strchr(p, '/'));) {
            size_t n = (size_t)(slash - p);
            if (!n || n >= sizeof part) { dir = 0; break; }
            memcpy(part, p, n); part[n] = 0;
            uint32_t d = pkg_child(v, dir, part);
            if (!d) {
                ps5c_err err = pkg_add(v, dir, part, true, 0, 0, &d);
                if (err) return err;
            }
            if (!v->nodes[d].dir) { dir = 0; break; }
            dir = d;
            p = slash + 1;
        }
        if (!dir || !*p || pkg_child(v, dir, p)) continue; /* the inner file system wins */
        uint32_t id;
        ps5c_err err = pkg_add(v, dir, p, false, e->size, e->offset, &id);
        if (err) return err;
    }
    return PS5C_OK;
}
/* a fake package: the inner tree becomes the node table, node ids match */
static ps5c_err load_pkg(volume *v) {
    pkg_info info;
    ps5c_err err = pkg_inspect(v->path, &info);
    if (err) return err;
    if (info.kind == PKG_PS4) {
        err = pkg_ps4_find_key(v->path, &info);
        if (err == PS5C_ERR_NOT_FOUND || err == PS5C_ERR_INVALID)
            LOG_E(CORE, "PS4 package %s: its file system is encrypted; it opens with the PS4 fake-package RSA keys in %s "
                        "(fake_n, fake_d, dk3_n, dk3_d), which the emulator does not ship", v->path, pkg_ps4_keys_path());
    } else {
        if (info.kind != PKG_PS5 || info.retail) err = PS5C_ERR_UNSUPPORTED;
        if (info.kind == PKG_PS5 && info.retail)
            LOG_E(CORE, "PS5 package %s: a store (retail) package, encrypted for the console; only fake packages "
                        "(fPKG) open", v->path);
        if (!err && info.outer_encrypted) err = pkg_find_key(v->path, &info, getenv("PS5C_PKG_PASSCODE"));
    }
    if (!err) err = pkg_inner_open(v->path, &info, &v->pkg);
    if (!err && v->pkg->tree_err) err = v->pkg->tree_err;
    if (!err) err = pal_file_open(v->path, PAL_FILE_READ, &v->file);
    if (!err) err = pal_file_size(v->file, &v->size);
    if (err) { pkg_info_free(&info); return err; }
    for (uint32_t i = 0; i < 65536; i++) v->upcase[i] = (uint16_t)(i >= 'a' && i <= 'z' ? i-'a'+'A' : i);
    v->have_upcase = true;
    const pkg_inner *in = v->pkg;
    for (uint32_t i = 0; !err && i < in->count; i++) {
        const pkg_inner_node *p = &in->nodes[i];
        node n = {0};
        n.parent = p->parent; n.child = p->child; n.sibling = p->sibling; n.dir = p->dir;
        n.size = n.valid = p->dir ? 0 : p->size;
        n.pkg_offset = p->offset;
        n.pkg_file = p->pfs ? i + 1 : 0;
        if (i && !wide_name(n.wide, p->name)) n.wide[0] = 0xfffe; /* unmatchable */
        size_t len = strlen(p->name);
        n.name = (char *)malloc(len + 1);
        if (!n.name) { err = PS5C_ERR_NOMEM; break; }
        memcpy(n.name, p->name, len + 1);
        uint32_t id;
        if ((err = push_node(v, &n, &id))) free(n.name);
    }
    v->have_game = in->uroot != 0;
    v->game_root = in->uroot;
    if (!err && v->have_game) err = pkg_add_entries(v, &info);
    pkg_info_free(&info);
    if (err) return err;
    LOG_I(CORE, "pkg: %s, %u entries, %u spans, read-only; nothing extracted", v->path, v->nnodes, in->nblocks);
    return PS5C_OK;
}
static ps5c_err load_volume(volume *v) {
    const char *dot = strrchr(v->path, '.');
    if (dot && ext_at(dot) == 4) return load_pkg(v);
    ps5c_err err = pal_file_open(v->path, PAL_FILE_READ, &v->file);
    if (err) return err;
    if ((err = pal_file_size(v->file, &v->size))) return err;
    uint8_t b[512]; if ((err = raw(v, 0, b, sizeof(b)))) return err;
    if (memcmp(b+3, "EXFAT   ", 8) || rd_le16(b+510) != 0xaa55 || rd_le16(b+104) != 0x100 || b[108] < 9 || b[108] > 12 || (unsigned)b[108]+b[109] > 25 || !b[110] || b[110]>2) return PS5C_ERR_INVALID;
    uint64_t sector = 1ull << b[108], sectors = rd_le64(b+72);
    uint64_t fats = rd_le32(b+84), fat_off = rd_le32(b+80), heap = rd_le32(b+88);
    uint32_t active = rd_le16(b+106)&1;
    if (active >= b[110] || sectors > v->size/sector || fat_off < 24 || fat_off + fats*b[110] > heap) return PS5C_ERR_INVALID;
    v->size = sectors*sector; v->heap = heap*sector; v->cluster = sector << b[109]; v->count = rd_le32(b+92);
    uint64_t fat_bytes = ((uint64_t)v->count+2)*4;
    if (!v->count || v->count > 0xfffffff5u || heap > sectors || v->count > (v->size-v->heap)/v->cluster || fat_bytes > fats*sector) return PS5C_ERR_INVALID;
    if (fat_bytes > MAX_FAT_BYTES) return PS5C_ERR_UNSUPPORTED;
    uint8_t *boot = (uint8_t *)malloc((size_t)sector*12);
    if (!boot) return PS5C_ERR_NOMEM;
    err = raw(v, 0, boot, sector*12);
    uint32_t sum = 0;
    if (!err) {
        for (uint64_t k = 0; k < sector*11; k++) if (k != 106 && k != 107 && k != 112) sum = (sum>>1 | sum<<31) + boot[k];
        for (uint64_t k = sector*11; k < sector*12; k += 4) if (rd_le32(boot+k) != sum) { err = PS5C_ERR_INVALID; break; }
    }
    free(boot); if (err) return err;
    v->fat = (uint8_t *)malloc((size_t)fat_bytes);
    if (!v->fat) return PS5C_ERR_NOMEM;
    if ((err = raw(v, (fat_off + active*fats)*sector, v->fat, fat_bytes))) return err;
    for (uint32_t i = 0; i < 65536; i++) v->upcase[i] = (uint16_t)(i >= 'a' && i <= 'z' ? i-'a'+'A' : i);
    node root = {0}; root.dir = true; root.first = rd_le32(b+96);
    uint32_t id = 0;
    err = chain(v, &root, false, true);
    if (!err) err = push_node(v, &root, &id);
    if (err) { free(root.runs); return err; }
    err = parse_dir(v, 0, 0);
    if (!err && !v->have_upcase) err = PS5C_ERR_INVALID;
    for (uint32_t i = 0; !err && i < 128; i++) {
        uint16_t want = (uint16_t)(i >= 'a' && i <= 'z' ? i-'a'+'A' : i);
        if (v->upcase[i] != want) err = PS5C_ERR_INVALID;
    }
    if (!err) {
        uint32_t candidate = UINT32_MAX;
        for (uint32_t i = 1; i < v->nnodes; i++) {
            if (v->nodes[i].dir || !equal_name(v, v->nodes[i].wide, "eboot.bin", 9)) continue;
            uint32_t parent = v->nodes[i].parent;
            if (equal_name(v, v->nodes[parent].wide, "decrypted", 9)) parent = v->nodes[parent].parent;
            if (candidate != UINT32_MAX && candidate != parent) { candidate = UINT32_MAX; break; }
            candidate = parent;
        }
        if (candidate != UINT32_MAX) {
            v->have_game = true; v->game_root = candidate;
            for (uint32_t i = v->nodes[candidate].child; i; i = v->nodes[i].sibling)
                if (v->nodes[i].dir && equal_name(v, v->nodes[i].wide, "decrypted", 9)) v->decrypted_root = i;
        }
    }
    if (!err) LOG_I(CORE, "exFAT: %s, %u entries, read-only; file data stays in image", v->path, v->nnodes);
    return err;
}
static bool equal_name(volume *v, const uint16_t *w, const char *p, size_t len) {
    unsigned wi = 0;
    for (size_t i = 0; i < len;) {
        uint32_t c = (uint8_t)p[i++], min = 0; unsigned more = 0;
        if (c >= 0xc2 && c <= 0xdf) { more = 1; min = 0x80; c &= 31; }
        else if (c >= 0xe0 && c <= 0xef) { more = 2; min = 0x800; c &= 15; }
        else if (c >= 0xf0 && c <= 0xf4) { more = 3; min = 0x10000; c &= 7; }
        else if (c >= 0x80) return false;
        if (more > len-i) return false;
        while (more--) { uint8_t b = (uint8_t)p[i++]; if ((b&0xc0)!=0x80) return false; c = (c<<6)|(b&63); }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
        if (c >= 0x10000) { uint16_t h = (uint16_t)(0xd800 + ((c-0x10000)>>10)); if (wi >= 255 || w[wi++] != h) return false; c = 0xdc00 + ((c-0x10000)&1023); }
        if (wi >= 255 || v->upcase[w[wi++]] != v->upcase[c]) return false;
    }
    return wi <= 255 && w[wi] == 0;
}
static ps5c_err walk(volume *v, uint32_t root, const char *p, uint32_t *id) {
    uint32_t at = root;
    for (; *p;) {
        while (sep(*p)) p++;
        if (!*p) break;
        const char *end = p; while (*end && !sep(*end)) end++;
        size_t n = (size_t)(end-p);
        if (n == 1 && *p == '.') { p = end; continue; }
        if (n == 2 && p[0] == '.' && p[1] == '.') { if (at != root) at = v->nodes[at].parent; p = end; continue; }
        if (!v->nodes[at].dir) return PS5C_ERR_NOT_FOUND;
        uint32_t child = v->nodes[at].child;
        while (child && !equal_name(v, v->nodes[child].wide, p, n)) child = v->nodes[child].sibling;
        if (!child) return PS5C_ERR_NOT_FOUND;
        at = child; p = end;
    }
    *id = at; return PS5C_OK;
}
static ps5c_err lookup_extra(const char *path, volume **out, uint32_t *id, uint32_t *other) {
    const char *cut = boundary(path);
    if (!cut || !g_lock) return PS5C_ERR_INVALID;
    size_t len = (size_t)(cut-path);
    pal_mutex_lock(g_lock);
    volume *v;
    for (v = g_volumes; v; v = v->next) if (strlen(v->path) == len && !memcmp(v->path, path, len)) break;
    ps5c_err err = PS5C_OK;
    if (!v) {
        v = (volume *)calloc(1, sizeof(volume));
        if (v) v->path = (char *)malloc(len+1);
        if (!v || !v->path) err = PS5C_ERR_NOMEM;
        else { memcpy(v->path, path, len); v->path[len] = 0; err = load_volume(v); }
        if (err) { free_volume(v); v = NULL; }
        else { v->next = g_volumes; g_volumes = v; }
    }
    pal_mutex_unlock(g_lock);
    if (err) return err;
    const char *p = cut; while (sep(*p)) p++;
    bool game = !strncmp(p, "@game", 5) && (!p[5] || sep(p[5]));
    if (other) *other = UINT32_MAX;
    if (!game) { *out = v; return walk(v, 0, p, id); }
    if (!v->have_game) return PS5C_ERR_INVALID;
    p += 5;
    uint32_t base = 0, dec = 0;
    ps5c_err be = walk(v, v->game_root, p, &base);
    ps5c_err de = v->decrypted_root ? walk(v, v->decrypted_root, p, &dec) : PS5C_ERR_NOT_FOUND;
    if (be && de) return PS5C_ERR_NOT_FOUND;
    /* Decrypted executables replace their original files; directories merge.
     * Explicit raw paths (without @game) continue exposing the original image. */
    if (!de && (!v->nodes[dec].dir || be)) *id = dec;
    else *id = base;
    if (other && !be && !de && v->nodes[base].dir && v->nodes[dec].dir) *other = dec;
    *out = v; return PS5C_OK;
}
static ps5c_err lookup(const char *path, volume **out, uint32_t *id) { return lookup_extra(path, out, id, NULL); }

ps5c_err exfat_open(const char *path, uint32_t flags, exfat_file **out) {
    if ((flags & ~PAL_FILE_RECOVER_READ) != PAL_FILE_READ) return PS5C_ERR_BUSY;
    volume *v; uint32_t id; ps5c_err err = lookup(path, &v, &id);
    if (err) return err;
    if (v->nodes[id].dir) return PS5C_ERR_INVALID;
    exfat_file *f = (exfat_file *)calloc(1, sizeof(*f));
    if (!f) return PS5C_ERR_NOMEM;
    if ((err = pal_mutex_create(&f->lock))) { free(f); return err; }
    f->v = v; f->id = id; *out = f; return PS5C_OK;
}
void exfat_close(exfat_file *f) { if (f) { pal_mutex_destroy(f->lock); free(f); } }
uint64_t exfat_size(exfat_file *f) { return f->v->nodes[f->id].size; }
ps5c_err exfat_read(exfat_file *f, void *buf, uint64_t size, uint64_t off, uint64_t *got) { return read_node(f->v, &f->v->nodes[f->id], buf, size, off, got); }
ps5c_err exfat_read_current(exfat_file *f, void *buf, uint64_t size, uint64_t *got) {
    pal_mutex_lock(f->lock);
    uint64_t n = 0; ps5c_err err = exfat_read(f, buf, size, f->pos, &n);
    if (!err) f->pos += n;
    pal_mutex_unlock(f->lock);
    if (got) *got = n;
    return err;
}
ps5c_err exfat_seek(exfat_file *f, int64_t off, int whence, uint64_t *pos) {
    if (whence < 0 || whence > 2) return PS5C_ERR_INVALID;
    pal_mutex_lock(f->lock);
    uint64_t base = whence == PAL_SEEK_CUR ? f->pos : whence == PAL_SEEK_END ? exfat_size(f) : 0;
    uint64_t neg = off < 0 ? (uint64_t)(-(off+1))+1 : 0;
    ps5c_err err = (off < 0 ? neg > base : (uint64_t)off > INT64_MAX || base > (uint64_t)INT64_MAX-(uint64_t)off) ? PS5C_ERR_INVALID : PS5C_OK;
    if (!err) { f->pos = off < 0 ? base-neg : base+(uint64_t)off; if (pos) *pos = f->pos; }
    pal_mutex_unlock(f->lock); return err;
}
ps5c_err exfat_stat(const char *path, pal_path_info *out) {
    volume *v; uint32_t id; ps5c_err err = lookup(path, &v, &id);
    if (!err) { memset(out, 0, sizeof(*out)); out->is_dir = v->nodes[id].dir; out->size = out->is_dir ? 0 : v->nodes[id].size; }
    return err;
}
ps5c_err exfat_list(const char *path, pal_dir_fn fn, void *user) {
    volume *v; uint32_t id, other; ps5c_err err = lookup_extra(path, &v, &id, &other);
    if (err) return err;
    if (!v->nodes[id].dir) return PS5C_ERR_INVALID;
    bool game = strstr(path, "/@game") != NULL || strstr(path, "\\@game") != NULL;
    for (uint32_t c = v->nodes[id].child; c; c = v->nodes[c].sibling) {
        if (game && id == v->game_root && c == v->decrypted_root) continue;
        if (!fn(user, v->nodes[c].name, v->nodes[c].dir)) return PS5C_OK;
    }
    if (other != UINT32_MAX) for (uint32_t c = v->nodes[other].child; c; c = v->nodes[c].sibling) {
        bool duplicate = false;
        for (uint32_t b = v->nodes[id].child; b; b = v->nodes[b].sibling)
            if (equal_name(v, v->nodes[b].wide, v->nodes[c].name, strlen(v->nodes[c].name))) { duplicate = true; break; }
        if (!duplicate && !fn(user, v->nodes[c].name, v->nodes[c].dir)) break;
    }
    return PS5C_OK;
}
ps5c_err exfat_pkg_range_present(const char *path, uint64_t offset, uint64_t size, bool *present) {
    *present = false;
    volume *v; uint32_t id; ps5c_err err = lookup(path, &v, &id);
    if (err) return err;
    if (!v->pkg) return PS5C_ERR_UNSUPPORTED;
    if (!size) { *present = true; return PS5C_OK; }
    uint64_t real = 0; /* v->size is the size seen at mount; ask the file again */
    if ((err = pal_file_size(v->file, &real))) return err;
    if (offset > real || size > real - offset) return PS5C_OK;
    uint8_t probe[4096];
    uint64_t n = PS5C_MIN(size, (uint64_t)sizeof probe), got = 0;
    err = pal_file_pread(v->file, probe, n, offset, &got);
    if (!err && got == n && size > n) err = pal_file_pread(v->file, probe, n, offset + size - n, &got);
    if (err || got != n) return err ? err : PS5C_ERR_IO;
    *present = true;
    return PS5C_OK;
}
ps5c_err exfat_game_path(const char *image, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s/", image);
    if (n < 0 || (size_t)n >= cap) return PS5C_ERR_OUT_OF_RANGE;
    volume *v; uint32_t root; ps5c_err err = lookup(out, &v, &root);
    if (err) return err;
    if (!v->have_game) return PS5C_ERR_INVALID;
    n = snprintf(out, cap, "%s/@game/eboot.bin", image);
    if (n < 0 || (size_t)n >= cap) return PS5C_ERR_OUT_OF_RANGE;
    return lookup(out, &v, &root);
}
