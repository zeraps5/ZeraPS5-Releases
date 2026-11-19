/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hle/png_decode.h"

#include <stdlib.h>
#include <string.h>

/* ---- inflate (RFC 1951) ---- */

#define FAST_BITS 9

typedef struct huff {
    uint16_t count[16];  /* codes of each length */
    uint16_t symbol[320]; /* symbols by code */
    uint16_t fast[1u << FAST_BITS]; /* low bits of the stream -> 0x8000 | length << 9 | symbol */
} huff;

typedef struct inflater {
    const uint8_t *in;
    size_t len, pos;
    uint64_t bitbuf;
    uint32_t bitcnt;
    uint8_t *out;
    size_t cap, n;
} inflater;

static void refill(inflater *s) {
    while (s->bitcnt <= 56 && s->pos < s->len) {
        s->bitbuf |= (uint64_t)s->in[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
}

/* need <= 32; false at the end of the input */
static bool take(inflater *s, uint32_t need, uint32_t *v) {
    if (s->bitcnt < need) refill(s);
    if (s->bitcnt < need) return false;
    *v = (uint32_t)(s->bitbuf & ((1ull << need) - 1));
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return true;
}

/* Canonical code from lengths. False: over-subscribed (incomplete codes are allowed:
 * a distance code may have one symbol). */
static bool build(huff *h, const uint8_t *lengths, uint32_t n) {
    memset(h->count, 0, sizeof(h->count));
    memset(h->fast, 0, sizeof(h->fast));
    for (uint32_t i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return false;
    }
    uint16_t offs[16], next[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (uint32_t i = 0; i < n; i++)
        if (lengths[i]) h->symbol[offs[lengths[i]]++] = (uint16_t)i;
    uint32_t code = 0;
    for (int len = 1; len < 16; len++) {
        next[len] = (uint16_t)code;
        code = (code + h->count[len]) << 1;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t len = lengths[i];
        if (!len || len > FAST_BITS) continue;
        uint32_t c = next[len]++, rev = 0;
        for (uint32_t b = 0; b < len; b++) rev |= ((c >> b) & 1u) << (len - 1 - b);
        for (uint32_t k = rev; k < (1u << FAST_BITS); k += 1u << len) h->fast[k] = (uint16_t)(0x8000u | len << 9 | i);
    }
    return true;
}

/* -1: invalid or truncated */
static int decode(inflater *s, const huff *h) {
    if (s->bitcnt < FAST_BITS) refill(s);
    if (s->bitcnt >= FAST_BITS) {
        uint16_t e = h->fast[s->bitbuf & ((1u << FAST_BITS) - 1)];
        if (e) {
            uint32_t len = (e >> 9) & 15;
            s->bitbuf >>= len;
            s->bitcnt -= len;
            return e & 511;
        }
    }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        uint32_t bit;
        if (!take(s, 1, &bit)) return -1;
        code |= (int)bit;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t k_lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                     35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t k_lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t k_dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                     257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t k_dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static bool codes(inflater *s, const huff *lit, const huff *dist) {
    for (;;) {
        int sym = decode(s, lit);
        if (sym < 0) return false;
        if (sym < 256) {
            if (s->n >= s->cap) return false;
            s->out[s->n++] = (uint8_t)sym;
            continue;
        }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) return false;
        uint32_t extra, len = k_lbase[sym];
        if (!take(s, k_lext[sym], &extra)) return false;
        len += extra;
        int d = decode(s, dist);
        if (d < 0 || d >= 30) return false;
        uint32_t distance = k_dbase[d];
        if (!take(s, k_dext[d], &extra)) return false;
        distance += extra;
        if (distance > s->n || len > s->cap - s->n) return false;
        uint8_t *o = s->out + s->n;
        const uint8_t *from = o - distance;
        for (uint32_t i = 0; i < len; i++) o[i] = from[i];
        s->n += len;
    }
}

static bool fixed_block(inflater *s) {
    static huff lit, dist;
    static bool ready;
    if (!ready) { /* the same tables every time: building twice is harmless */
        uint8_t l[288];
        for (int i = 0; i < 144; i++) l[i] = 8;
        for (int i = 144; i < 256; i++) l[i] = 9;
        for (int i = 256; i < 280; i++) l[i] = 7;
        for (int i = 280; i < 288; i++) l[i] = 8;
        build(&lit, l, 288);
        for (int i = 0; i < 30; i++) l[i] = 5;
        build(&dist, l, 30);
        ready = true;
    }
    return codes(s, &lit, &dist);
}

static bool dynamic_block(inflater *s) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint32_t nlen, ndist, ncode;
    if (!take(s, 5, &nlen) || !take(s, 5, &ndist) || !take(s, 4, &ncode)) return false;
    nlen += 257, ndist += 1, ncode += 4;
    if (nlen > 286 || ndist > 30) return false;
    uint8_t lengths[320] = {0};
    for (uint32_t i = 0; i < ncode; i++) {
        uint32_t v;
        if (!take(s, 3, &v)) return false;
        lengths[order[i]] = (uint8_t)v;
    }
    huff *lit = (huff *)malloc(sizeof(huff) * 2);
    if (!lit) return false;
    huff *dist = lit + 1;
    bool ok = build(lit, lengths, 19);
    memset(lengths, 0, sizeof(lengths));
    for (uint32_t i = 0; ok && i < nlen + ndist;) {
        int sym = decode(s, lit);
        if (sym < 0) { ok = false; break; }
        if (sym < 16) { lengths[i++] = (uint8_t)sym; continue; }
        uint32_t rep, v = 0;
        uint8_t value = 0;
        if (sym == 16) {
            if (!i) { ok = false; break; }
            value = lengths[i - 1];
            ok = take(s, 2, &v), rep = 3 + v;
        } else if (sym == 17) ok = take(s, 3, &v), rep = 3 + v;
        else ok = take(s, 7, &v), rep = 11 + v;
        if (!ok || i + rep > nlen + ndist) { ok = false; break; }
        while (rep--) lengths[i++] = value;
    }
    ok = ok && lengths[256] && build(lit, lengths, nlen) && build(dist, lengths + nlen, ndist);
    ok = ok && codes(s, lit, dist);
    free(lit);
    return ok;
}

bool png_inflate_zlib(const uint8_t *in, size_t len, uint8_t *out, size_t cap, size_t *out_len) {
    if (len < 2 || (in[0] & 15) != 8 || ((in[0] << 8) | in[1]) % 31 || (in[1] & 0x20)) return false;
    inflater s = {in, len, 2, 0, 0, out, cap, 0};
    uint32_t last = 0, type;
    while (!last) {
        if (!take(&s, 1, &last) || !take(&s, 2, &type)) return false;
        bool ok;
        if (type == 0) {
            /* to the byte boundary: the bits left are whole bytes taken ahead */
            uint32_t drop = s.bitcnt & 7u;
            s.bitbuf >>= drop;
            s.bitcnt -= drop;
            s.pos -= s.bitcnt / 8;
            s.bitbuf = 0, s.bitcnt = 0;
            if (s.len - s.pos < 4) return false;
            uint32_t n = s.in[s.pos] | (uint32_t)s.in[s.pos + 1] << 8, nn = s.in[s.pos + 2] | (uint32_t)s.in[s.pos + 3] << 8;
            s.pos += 4;
            if ((n ^ 0xffffu) != nn || s.len - s.pos < n || s.cap - s.n < n) return false;
            memcpy(s.out + s.n, s.in + s.pos, n);
            s.pos += n, s.n += n;
            ok = true;
        } else if (type == 1) ok = fixed_block(&s);
        else if (type == 2) ok = dynamic_block(&s);
        else ok = false;
        if (!ok) return false;
    }
    *out_len = s.n;
    return true;
}

/* ---- PNG ---- */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static const uint8_t k_sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

static uint32_t channels_of(uint8_t color_type) {
    return color_type == 0 ? 1 : color_type == 2 ? 3 : color_type == 3 ? 1 : color_type == 4 ? 2 : color_type == 6 ? 4 : 0;
}

static bool valid_depth(uint8_t ct, uint8_t d) {
    switch (ct) {
    case 0: return d == 1 || d == 2 || d == 4 || d == 8 || d == 16;
    case 3: return d == 1 || d == 2 || d == 4 || d == 8;
    case 2: case 4: case 6: return d == 8 || d == 16;
    default: return false;
    }
}

bool png_parse_header(const uint8_t *data, size_t size, png_header *out) {
    if (!data || size < 33 || memcmp(data, k_sig, 8) || be32(data + 8) != 13 || memcmp(data + 12, "IHDR", 4)) return false;
    memset(out, 0, sizeof(*out));
    out->width = be32(data + 16), out->height = be32(data + 20);
    out->bit_depth = data[24], out->color_type = data[25], out->interlace = data[28];
    if (!out->width || !out->height || out->width > 0x7fffffffu || out->height > 0x7fffffffu ||
        !valid_depth(out->color_type, out->bit_depth) || data[26] || data[27] || out->interlace > 1)
        return false;
    for (size_t off = 33; off + 12 <= size;) {
        uint32_t n = be32(data + off);
        if (n > size - off - 12) break;
        if (!memcmp(data + off + 4, "IDAT", 4)) break;
        if (!memcmp(data + off + 4, "tRNS", 4)) out->has_trns = true;
        off += 12 + (size_t)n;
    }
    return true;
}

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
    int p = (int)a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static bool unfilter(uint8_t *rows, uint32_t height, size_t rowbytes, uint32_t bpp) {
    uint8_t *prev = NULL;
    for (uint32_t y = 0; y < height; y++) {
        uint8_t *r = rows + (size_t)y * (rowbytes + 1), type = r[0], *p = r + 1;
        const uint8_t *q = prev ? prev + 1 : NULL;
        switch (type) {
        case 0: break;
        case 1: for (size_t i = bpp; i < rowbytes; i++) p[i] = (uint8_t)(p[i] + p[i - bpp]); break;
        case 2: if (q) for (size_t i = 0; i < rowbytes; i++) p[i] = (uint8_t)(p[i] + q[i]); break;
        case 3:
            for (size_t i = 0; i < rowbytes; i++)
                p[i] = (uint8_t)(p[i] + (((i >= bpp ? p[i - bpp] : 0) + (q ? q[i] : 0)) >> 1));
            break;
        case 4:
            for (size_t i = 0; i < rowbytes; i++)
                p[i] = (uint8_t)(p[i] + paeth(i >= bpp ? p[i - bpp] : 0, q ? q[i] : 0, q && i >= bpp ? q[i - bpp] : 0));
            break;
        default: return false;
        }
        prev = r;
    }
    return true;
}

typedef struct png_state {
    png_header h;
    uint8_t palette[256][4];
    uint32_t palette_size;
    uint16_t trns[3]; /* gray or RGB value shown transparent */
    bool trns_color;
    bool alpha_source;
    uint8_t alpha;
} png_state;

static uint32_t sample(const uint8_t *row, uint32_t index, uint8_t depth) {
    if (depth == 8) return row[index];
    if (depth == 16) return (uint32_t)row[2 * index] << 8 | row[2 * index + 1];
    uint32_t bit = index * depth, shift = 8 - depth - (bit & 7);
    return (row[bit >> 3] >> shift) & ((1u << depth) - 1);
}

static void put_pixel(const png_state *st, const uint8_t *row, uint32_t x, uint8_t *dst) {
    uint8_t d = st->h.bit_depth;
    uint32_t ch = channels_of(st->h.color_type), max = (1u << d) - 1;
    uint32_t v[4];
    for (uint32_t k = 0; k < ch; k++) v[k] = sample(row, x * ch + k, d);
    uint8_t to8[4];
    for (uint32_t k = 0; k < ch; k++) to8[k] = (uint8_t)(d == 16 ? v[k] >> 8 : d == 8 ? v[k] : v[k] * 255 / max);
    switch (st->h.color_type) {
    case 3: {
        uint32_t i = v[0];
        const uint8_t *p = i < st->palette_size ? st->palette[i] : (const uint8_t[4]){0, 0, 0, 255};
        memcpy(dst, p, 4);
        break;
    }
    case 0:
        dst[0] = dst[1] = dst[2] = to8[0];
        dst[3] = st->trns_color && v[0] == st->trns[0] ? 0 : 255;
        break;
    case 4: dst[0] = dst[1] = dst[2] = to8[0], dst[3] = to8[1]; break;
    case 2:
        dst[0] = to8[0], dst[1] = to8[1], dst[2] = to8[2];
        dst[3] = st->trns_color && v[0] == st->trns[0] && v[1] == st->trns[1] && v[2] == st->trns[2] ? 0 : 255;
        break;
    default: memcpy(dst, to8, 4); break;
    }
    if (!st->alpha_source) dst[3] = st->alpha;
}

bool png_decode_rgba(const uint8_t *data, size_t size, uint8_t *out, size_t pitch, bool bgra, uint8_t alpha) {
    png_state st;
    memset(&st, 0, sizeof(st));
    if (!png_parse_header(data, size, &st.h)) return false;
    const png_header *h = &st.h;
    uint32_t ch = channels_of(h->color_type), bits = ch * h->bit_depth, bpp = bits >= 8 ? bits / 8 : 1;
    /* image data: every IDAT, palette and transparency */
    size_t idat_len = 0, idat_cap = 0;
    uint8_t *idat = NULL;
    bool ok = true;
    for (size_t off = 8; ok && off + 12 <= size;) {
        uint32_t n = be32(data + off);
        if (n > size - off - 12) break;
        const uint8_t *type = data + off + 4, *body = data + off + 8;
        if (!memcmp(type, "IDAT", 4)) {
            if (idat_len + n > idat_cap) {
                size_t cap = idat_cap ? idat_cap * 2 : 65536;
                while (cap < idat_len + n) cap *= 2;
                uint8_t *grown = (uint8_t *)realloc(idat, cap);
                if (!grown) ok = false;
                else idat = grown, idat_cap = cap;
            }
            if (ok) memcpy(idat + idat_len, body, n), idat_len += n;
        } else if (!memcmp(type, "PLTE", 4)) {
            st.palette_size = n / 3 > 256 ? 256 : n / 3;
            for (uint32_t i = 0; i < st.palette_size; i++)
                st.palette[i][0] = body[3 * i], st.palette[i][1] = body[3 * i + 1], st.palette[i][2] = body[3 * i + 2],
                st.palette[i][3] = 255;
        } else if (!memcmp(type, "tRNS", 4)) {
            if (h->color_type == 3)
                for (uint32_t i = 0; i < n && i < 256; i++) st.palette[i][3] = body[i];
            else if (h->color_type == 0 && n >= 2)
                st.trns[0] = (uint16_t)(body[0] << 8 | body[1]), st.trns_color = true;
            else if (h->color_type == 2 && n >= 6)
                for (int k = 0; k < 3; k++) st.trns[k] = (uint16_t)(body[2 * k] << 8 | body[2 * k + 1]), st.trns_color = true;
        } else if (!memcmp(type, "IEND", 4)) break;
        off += 12 + (size_t)n;
    }
    st.alpha_source = h->color_type == 4 || h->color_type == 6 || h->has_trns;
    st.alpha = alpha;
    /* the passes (one without interlacing) and the raw size of each */
    static const uint8_t x0[7] = {0, 4, 0, 2, 0, 1, 0}, y0[7] = {0, 0, 4, 0, 2, 0, 1};
    static const uint8_t dx[7] = {8, 8, 4, 4, 2, 2, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
    uint32_t passes = h->interlace ? 7 : 1, pw[7], ph[7];
    size_t raw_size = 0, rowbytes[7];
    for (uint32_t p = 0; p < passes; p++) {
        pw[p] = h->interlace ? (h->width > x0[p] ? (h->width - x0[p] + dx[p] - 1) / dx[p] : 0) : h->width;
        ph[p] = h->interlace ? (h->height > y0[p] ? (h->height - y0[p] + dy[p] - 1) / dy[p] : 0) : h->height;
        rowbytes[p] = ((size_t)pw[p] * bits + 7) / 8;
        if (pw[p] && ph[p]) raw_size += (size_t)ph[p] * (rowbytes[p] + 1);
    }
    uint8_t *raw = ok && idat ? (uint8_t *)malloc(raw_size) : NULL;
    size_t got = 0;
    ok = raw && png_inflate_zlib(idat, idat_len, raw, raw_size, &got) && got == raw_size;
    free(idat);
    uint8_t *at = raw;
    for (uint32_t p = 0; ok && p < passes; p++) {
        if (!pw[p] || !ph[p]) continue;
        if (!unfilter(at, ph[p], rowbytes[p], bpp)) { ok = false; break; }
        for (uint32_t y = 0; y < ph[p]; y++) {
            const uint8_t *row = at + (size_t)y * (rowbytes[p] + 1) + 1;
            uint32_t oy = h->interlace ? y0[p] + y * dy[p] : y;
            for (uint32_t x = 0; x < pw[p]; x++) {
                uint32_t ox = h->interlace ? x0[p] + x * dx[p] : x;
                uint8_t *dst = out + (size_t)oy * pitch + (size_t)ox * 4;
                put_pixel(&st, row, x, dst);
                if (bgra) { uint8_t t = dst[0]; dst[0] = dst[2]; dst[2] = t; }
            }
        }
        at += (size_t)ph[p] * (rowbytes[p] + 1);
    }
    free(raw);
    return ok;
}
