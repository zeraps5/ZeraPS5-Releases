/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Canonical-Huffman DEFLATE decoder, after the structure of RFC 1951 itself:
 * stored, fixed and dynamic blocks, codes decoded bit by bit from counts per
 * length. Speed is not the point: a PS4 package block is 64 KiB. */
#include "base/inflate.h"

#include <string.h>

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30

typedef struct state {
    const uint8_t *in;
    size_t inlen, incnt;
    uint32_t bitbuf, bitcnt;
    uint8_t *out;
    size_t outcap, outcnt;
    int err;
} state;

typedef struct huffman {
    short count[MAXBITS + 1];
    short symbol[MAXLCODES + 2]; /* the fixed literal/length code has 288 */
} huffman;

static int bits(state *s, int need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < (uint32_t)need) {
        if (s->incnt == s->inlen) { s->err = 1; return 0; }
        val |= (uint32_t)s->in[s->incnt++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= (uint32_t)need;
    return (int)(val & ((1u << need) - 1));
}

static int stored(state *s) {
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen) return -1;
    unsigned len = s->in[s->incnt] | (unsigned)s->in[s->incnt + 1] << 8;
    unsigned nlen = s->in[s->incnt + 2] | (unsigned)s->in[s->incnt + 3] << 8;
    s->incnt += 4;
    if (len != (~nlen & 0xFFFF)) return -1;
    if (s->incnt + len > s->inlen || s->outcnt + len > s->outcap) return -1;
    memcpy(s->out + s->outcnt, s->in + s->incnt, len);
    s->incnt += len;
    s->outcnt += len;
    return 0;
}

static int decode(state *s, const huffman *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= bits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* 0: complete, >0: incomplete (allowed for a single distance code), <0: over-subscribed */
static int construct(huffman *h, const short *length, int n) {
    short offs[MAXBITS + 1];
    for (int len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++)
        if (length[sym]) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static const short LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(state *s, const huffman *lencode, const huffman *distcode) {
    for (;;) {
        int sym = decode(s, lencode);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->outcnt == s->outcap) return -1;
            s->out[s->outcnt++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            size_t len = (size_t)(LBASE[sym] + bits(s, LEXT[sym]));
            int dsym = decode(s, distcode);
            if (dsym < 0 || dsym >= 30) return -1;
            size_t dist = (size_t)(DBASE[dsym] + bits(s, DEXT[dsym]));
            if (s->err || dist > s->outcnt || s->outcnt + len > s->outcap) return -1;
            uint8_t *o = s->out + s->outcnt;
            for (size_t k = 0; k < len; k++) o[k] = o[(ptrdiff_t)k - (ptrdiff_t)dist]; /* may overlap */
            s->outcnt += len;
        }
    }
}

/* built per block: cheap, and safe with several threads decoding at once */
static int fixed(state *s) {
    huffman lencode, distcode;
    short lengths[MAXLCODES + 2];
    int sym = 0;
    for (; sym < 144; sym++) lengths[sym] = 8;
    for (; sym < 256; sym++) lengths[sym] = 9;
    for (; sym < 280; sym++) lengths[sym] = 7;
    for (; sym < 288; sym++) lengths[sym] = 8;
    construct(&lencode, lengths, 288);
    for (sym = 0; sym < 30; sym++) lengths[sym] = 5;
    construct(&distcode, lengths, 30);
    return codes(s, &lencode, &distcode);
}

static int dynamic(state *s) {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[MAXLCODES + MAXDCODES];
    huffman lencode, distcode;
    int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
    if (s->err || nlen > MAXLCODES || ndist > MAXDCODES) return -1;
    int index = 0;
    for (; index < ncode; index++) lengths[order[index]] = (short)bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (s->err || construct(&lencode, lengths, 19) != 0) return -1;
    index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(s, &lencode);
        if (symbol < 0) return -1;
        if (symbol < 16) {
            lengths[index++] = (short)symbol;
        } else {
            short len = 0;
            if (symbol == 16) {
                if (!index) return -1;
                len = lengths[index - 1];
                symbol = 3 + bits(s, 2);
            } else if (symbol == 17) {
                symbol = 3 + bits(s, 3);
            } else {
                symbol = 11 + bits(s, 7);
            }
            if (s->err || index + symbol > nlen + ndist) return -1;
            while (symbol--) lengths[index++] = len;
        }
    }
    if (!lengths[256]) return -1;
    int err = construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return -1;
    err = construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return -1;
    return codes(s, &lencode, &distcode);
}

long inflate_raw(const uint8_t *src, size_t len, uint8_t *dst, size_t cap) {
    state s = {src, len, 0, 0, 0, dst, cap, 0, 0};
    int last;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (s.err) return -1;
        int r = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
        if (r || s.err) return -1;
    } while (!last);
    return (long)s.outcnt;
}

long inflate_zlib(const uint8_t *src, size_t len, uint8_t *dst, size_t cap) {
    if (len < 2 || (src[0] & 0x0F) != 8 || (src[0] >> 4) > 7 || ((src[0] << 8) | src[1]) % 31 || (src[1] & 0x20))
        return -1;
    return inflate_raw(src + 2, len - 2, dst, cap);
}
