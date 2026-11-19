/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Kraken (Oodle newLZ) block decoder: a C port of LibProsperoPkg's
 * KrakenDecoder.cs (Copyright (C) 2026 SvenGDK), whose decode logic derives
 * from ooz kraken.cpp by Powzix (GPL-3.0, per LibProsperoPkg NOTICE);
 * LibProsperoPkg is GPL-3.0-or-later.
 *
 * Indices are kept as ints over the source buffer, as in the reference, and
 * every place where the reference would throw on an out-of-range index is an
 * explicit check that fails the decode. Variable shifts of 32-bit values use
 * the reference's (C#) semantics: the count is taken modulo 32. */
#include "base/kraken.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK_MAX 0x20000
#define SEED_SIZE 8

#define SHL(x, n) ((uint32_t)(x) << ((n) & 31))
#define SHR(x, n) ((uint32_t)(x) >> ((n) & 31))

struct kraken_scratch {
    uint8_t lit[CHUNK_MAX], cmd[CHUNK_MAX], offs_packed[CHUNK_MAX], offs_extra[CHUNK_MAX];
    uint8_t litlen[CHUNK_MAX];
    int32_t offs[CHUNK_MAX], lens[CHUNK_MAX];
};

kraken_scratch *kraken_scratch_new(void) { return malloc(sizeof(kraken_scratch)); }
void kraken_scratch_free(kraken_scratch *s) { free(s); }

#include "kraken_rice.inc"

/* ---- helpers ------------------------------------------------------------ */

static int bsr(uint32_t x) { return x == 0 ? 0 : 31 - __builtin_clz(x); }
static int bsf(uint32_t x) { return x == 0 ? 0 : __builtin_ctz(x); }
static int clz(uint32_t x) { return x == 0 ? 31 : __builtin_clz(x); }
static uint32_t rotl(uint32_t x, int n)
{
    n &= 31;
    return n ? (x << n) | (x >> (32 - n)) : x;
}
static uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }

/* bounded little-endian accessors (zero outside the array, writes dropped) */
static uint8_t at(const uint8_t *b, int len, int i) { return (unsigned)i < (unsigned)len ? b[i] : 0; }
static uint32_t rd32(const uint8_t *b, int len, int off)
{
    uint32_t r = 0;
    for (int i = 0; i < 4; i++)
        r |= (uint32_t)at(b, len, off + i) << (8 * i);
    return r;
}
static uint64_t rd64(const uint8_t *b, int len, int off)
{
    uint64_t r = 0;
    for (int i = 0; i < 8; i++)
        r |= (uint64_t)at(b, len, off + i) << (8 * i);
    return r;
}
static void wr32(uint8_t *b, int len, int off, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        if ((unsigned)(off + i) < (unsigned)len)
            b[off + i] = (uint8_t)(v >> (8 * i));
}
static void wr64(uint8_t *b, int len, int off, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        if ((unsigned)(off + i) < (unsigned)len)
            b[off + i] = (uint8_t)(v >> (8 * i));
}

/* ---- MSB-first bit reader with a 24-bit refill window ------------------- */

typedef struct bitreader {
    const uint8_t *B;
    int P;     /* current byte index */
    int Bound; /* forward: high bound (exclusive); backward: low bound */
    uint32_t Bits;
    int BitPos;
    bool bwd;
} bitreader;

static void refill_f(bitreader *r)
{
    while (r->BitPos > 0) {
        r->Bits |= SHL(r->P < r->Bound ? r->B[r->P] : 0, r->BitPos);
        r->BitPos -= 8;
        r->P++;
    }
}
static void refill_b(bitreader *r)
{
    while (r->BitPos > 0) {
        r->P--;
        r->Bits |= SHL(r->P >= r->Bound ? r->B[r->P] : 0, r->BitPos);
        r->BitPos -= 8;
    }
}
static void refill(bitreader *r)
{
    if (r->bwd)
        refill_b(r);
    else
        refill_f(r);
}
static bitreader br_forward(const uint8_t *b, int start, int end)
{
    bitreader r = {b, start, end, 0, 24, false};
    refill_f(&r);
    return r;
}
static bitreader br_backward(const uint8_t *b, int low, int high)
{
    bitreader r = {b, high, low, 0, 24, true};
    refill_b(&r);
    return r;
}
static int read_bit(bitreader *r)
{
    refill(r);
    int v = (int)(r->Bits >> 31);
    r->Bits <<= 1;
    r->BitPos += 1;
    return v;
}
static int read_bit_nr(bitreader *r)
{
    int v = (int)(r->Bits >> 31);
    r->Bits <<= 1;
    r->BitPos += 1;
    return v;
}
static uint32_t read_bits_nr(bitreader *r, int n)
{
    uint32_t v = SHR(r->Bits, 32 - n);
    r->Bits = SHL(r->Bits, n);
    r->BitPos += n;
    return v;
}
static uint32_t read_bits_nr_zero(bitreader *r, int n)
{
    uint32_t v = SHR(r->Bits >> 1, 31 - n);
    r->Bits = SHL(r->Bits, n);
    r->BitPos += n;
    return v;
}
static int read_fluff(bitreader *r, int num_symbols)
{
    if (num_symbols == 256)
        return 0;
    int x = 257 - num_symbols;
    if (x > num_symbols)
        x = num_symbols;
    x *= 2;
    int y = bsr((uint32_t)(x - 1)) + 1;
    uint32_t v = SHR(r->Bits, 32 - y);
    uint32_t z = SHL(1u, y) - (uint32_t)x;
    if ((v >> 1) >= z) {
        r->Bits = SHL(r->Bits, y);
        r->BitPos += y;
        return (int)(v - z);
    }
    r->Bits = SHL(r->Bits, y - 1);
    r->BitPos += y - 1;
    return (int)(v >> 1);
}
static uint32_t read_more_than_24(bitreader *r, int n)
{
    uint32_t rv;
    if (n <= 24) {
        rv = read_bits_nr_zero(r, n);
    } else {
        rv = SHL(read_bits_nr(r, 24), n - 24);
        refill(r);
        rv += read_bits_nr(r, n - 24);
    }
    refill(r);
    return rv;
}
static uint32_t read_distance(bitreader *r, uint32_t v)
{
    uint32_t w, m, rv;
    int n;
    if (v < 0xF0) {
        n = (int)(v >> 4) + 4;
        w = rotl(r->Bits | 1, n);
        r->BitPos += n;
        m = SHL(2u, n) - 1;
        r->Bits = w & ~m;
        rv = ((w & m) << 4) + (v & 0xF) - 248;
    } else {
        n = (int)(v - 0xF0) + 4;
        w = rotl(r->Bits | 1, n);
        r->BitPos += n;
        m = SHL(2u, n) - 1;
        r->Bits = w & ~m;
        rv = 8322816 + ((w & m) << 12);
        refill(r);
        rv += r->Bits >> 20;
        r->BitPos += 12;
        r->Bits <<= 12;
    }
    refill(r);
    return rv;
}
static bool read_length(bitreader *r, uint32_t *v)
{
    *v = 0;
    int n = 31 - bsr(r->Bits);
    if (n > 12)
        return false;
    r->BitPos += n;
    r->Bits = SHL(r->Bits, n);
    refill(r);
    n += 7;
    r->BitPos += n;
    *v = SHR(r->Bits, 32 - n) - 64;
    r->Bits = SHL(r->Bits, n);
    refill(r);
    return true;
}

/* ---- Huffman (DecodeBytes types 2 and 4) -------------------------------- */

static const uint32_t g_code_prefix_org[12] = {0x0,   0x0,   0x2,   0x6,   0xE,   0x1E,
                                               0x3E,  0x7E,  0xFE,  0x1FE, 0x2FE, 0x3FE};

typedef struct huff_lut {
    uint8_t bits2len[2048 + 16], bits2sym[2048 + 16];
} huff_lut;

static bool huff_make_lut(const uint32_t *org, const uint32_t *cur, huff_lut *lut, const uint8_t *syms)
{
    uint32_t currslot = 0;
    for (uint32_t i = 1; i < 11; i++) {
        uint32_t start = org[i], count = cur[i] - start;
        if (count == 0)
            continue;
        uint32_t step = 1u << (11 - i), num = count << (11 - i);
        if (currslot + num > 2048)
            return false;
        memset(lut->bits2len + currslot, (int)i, num);
        uint32_t p = currslot;
        for (uint32_t j = 0; j != count; j++, p += step)
            memset(lut->bits2sym + p, syms[start + j], step);
        currslot += num;
    }
    if (cur[11] - org[11] != 0) {
        uint32_t num = cur[11] - org[11];
        if (currslot + num > 2048)
            return false;
        memset(lut->bits2len + currslot, 11, num);
        memcpy(lut->bits2sym + currslot, syms + org[11], num);
        currslot += num;
    }
    return currslot == 2048;
}

static int reverse11(int v)
{
    int r = 0;
    for (int k = 0; k < 11; k++) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

static int code_lengths_old(bitreader *bits, uint8_t *syms, uint32_t *prefix)
{
    if (read_bit_nr(bits) != 0) {
        int sym = 0, codelen, num_symbols = 0, avg_bits_x4 = 32, n;
        int forced = (int)read_bits_nr(bits, 2);
        uint32_t thres = 1u << (31 - (int)(20u >> forced));
        bool skip = read_bit(bits) != 0;
        for (;;) {
            if (!skip) {
                if ((bits->Bits & 0xFF000000u) == 0)
                    return -1;
                sym += (int)read_bits_nr(bits, 2 * (clz(bits->Bits) + 1)) - 2 + 1;
                if (sym >= 256)
                    break;
            }
            skip = false;
            refill(bits);
            if ((bits->Bits & 0xFF000000u) == 0)
                return -1;
            n = (int)read_bits_nr(bits, 2 * (clz(bits->Bits) + 1)) - 2 + 1;
            if (sym + n > 256)
                return -1;
            refill(bits);
            num_symbols += n;
            do {
                if (bits->Bits < thres)
                    return -1;
                int lz = clz(bits->Bits);
                int v = (int)read_bits_nr(bits, lz + forced + 1) + ((lz - 1) << forced);
                codelen = (-(v & 1) ^ (v >> 1)) + ((avg_bits_x4 + 2) >> 2);
                if (codelen < 1 || codelen > 11)
                    return -1;
                avg_bits_x4 = codelen + ((3 * avg_bits_x4 + 2) >> 2);
                refill(bits);
                syms[prefix[codelen]++] = (uint8_t)sym++;
            } while (--n != 0);
            if (sym == 256)
                break;
        }
        return (sym == 256 && num_symbols >= 2) ? num_symbols : -1;
    }
    int num_symbols = (int)read_bits_nr(bits, 8);
    if (num_symbols == 0)
        return -1;
    if (num_symbols == 1) {
        syms[0] = (uint8_t)read_bits_nr(bits, 8);
    } else {
        int codelen_bits = (int)read_bits_nr(bits, 3);
        if (codelen_bits > 4)
            return -1;
        for (int i = 0; i < num_symbols; i++) {
            refill(bits);
            int sym = (int)read_bits_nr(bits, 8);
            int codelen = (int)read_bits_nr_zero(bits, codelen_bits) + 1;
            if (codelen > 11)
                return -1;
            syms[prefix[codelen]++] = (uint8_t)sym;
        }
    }
    return num_symbols;
}

typedef struct bitreader2 {
    const uint8_t *B;
    int Blen, P, PEnd, BitPos;
} bitreader2;

#define CODELEN_SIZE (512 + 16)

static bool golomb_rice_lengths(uint8_t *dst, int size, bitreader2 *br)
{
    int p = br->P, p_end = br->PEnd, dst_pos = 0, dst_end = size;
    if (p >= p_end)
        return false;
    int count = -br->BitPos;
    uint32_t v = (uint32_t)(at(br->B, br->Blen, p++) & (255 >> br->BitPos));
    for (;;) {
        if (v == 0) {
            count += 8;
        } else {
            uint32_t x = g_rice_val[v];
            wr32(dst, CODELEN_SIZE, dst_pos, (uint32_t)count + (x & 0x0F0F0F0Fu));
            wr32(dst, CODELEN_SIZE, dst_pos + 4, (x >> 4) & 0x0F0F0F0Fu);
            dst_pos += g_rice_len[v];
            if (dst_pos >= dst_end)
                break;
            count = (int)(x >> 28);
        }
        if (p >= p_end)
            return false;
        v = at(br->B, br->Blen, p++);
    }
    if (dst_pos > dst_end) {
        int nn = dst_pos - dst_end;
        do
            v &= v - 1;
        while (--nn != 0);
    }
    int bitpos = 0;
    if ((v & 1) == 0) {
        p--;
        bitpos = 8 - bsf(v);
    }
    br->P = p;
    br->BitPos = bitpos;
    return true;
}

static bool golomb_rice_bits(uint8_t *dst, int size, int bitcount, bitreader2 *br)
{
    if (bitcount == 0)
        return true;
    int dst_pos = 0, dst_end = size, p = br->P, bitpos = br->BitPos;
    int bits_required = bitpos + bitcount * size;
    int bytes_required = (bits_required + 7) >> 3;
    if (bytes_required > br->PEnd - p)
        return false;
    br->P = p + (bits_required >> 3);
    br->BitPos = bits_required & 7;
    uint64_t bak = rd64(dst, CODELEN_SIZE, dst_end);
    if (bitcount == 1) {
        do {
            uint64_t bits = (uint8_t)(bswap32(rd32(br->B, br->Blen, p)) >> (24 - bitpos));
            p += 1;
            bits = (bits | (bits << 28)) & 0xF0000000Full;
            bits = (bits | (bits << 14)) & 0x3000300030003ull;
            bits = (bits | (bits << 7)) & 0x0101010101010101ull;
            uint64_t cur = rd64(dst, CODELEN_SIZE, dst_pos);
            wr64(dst, CODELEN_SIZE, dst_pos, cur * 2 + bswap64(bits));
            dst_pos += 8;
        } while (dst_pos < dst_end);
    } else if (bitcount == 2) {
        do {
            uint64_t bits = (uint16_t)(bswap32(rd32(br->B, br->Blen, p)) >> (16 - bitpos));
            p += 2;
            bits = (bits | (bits << 24)) & 0xFF000000FFull;
            bits = (bits | (bits << 12)) & 0xF000F000F000Full;
            bits = (bits | (bits << 6)) & 0x0303030303030303ull;
            uint64_t cur = rd64(dst, CODELEN_SIZE, dst_pos);
            wr64(dst, CODELEN_SIZE, dst_pos, cur * 4 + bswap64(bits));
            dst_pos += 8;
        } while (dst_pos < dst_end);
    } else {
        do {
            uint64_t bits = (bswap32(rd32(br->B, br->Blen, p)) >> (8 - bitpos)) & 0xFFFFFF;
            p += 3;
            bits = (bits | (bits << 20)) & 0xFFF00000FFFull;
            bits = (bits | (bits << 10)) & 0x3F003F003F003Full;
            bits = (bits | (bits << 5)) & 0x0707070707070707ull;
            uint64_t cur = rd64(dst, CODELEN_SIZE, dst_pos);
            wr64(dst, CODELEN_SIZE, dst_pos, cur * 8 + bswap64(bits));
            dst_pos += 8;
        } while (dst_pos < dst_end);
    }
    wr64(dst, CODELEN_SIZE, dst_end, bak);
    return true;
}

typedef struct huff_range {
    int symbol, num;
} huff_range;

static int convert_to_ranges(huff_range *range, int num_symbols, int p, const uint8_t *symlen, int off,
                             bitreader *bits)
{
    int num_ranges = p >> 1, v, sym_idx = 0;
    if (p & 1) {
        refill(bits);
        v = symlen[off++];
        if (v >= 8)
            return -1;
        sym_idx = (int)read_bits_nr(bits, v + 1) + (1 << (v + 1)) - 1;
    }
    int syms_used = 0;
    for (int i = 0; i < num_ranges; i++) {
        refill(bits);
        v = symlen[off];
        if (v >= 9)
            return -1;
        int num = (int)read_bits_nr_zero(bits, v) + (1 << v);
        v = symlen[off + 1];
        if (v >= 8)
            return -1;
        int space = (int)read_bits_nr(bits, v + 1) + (1 << (v + 1)) - 1;
        range[i].symbol = sym_idx;
        range[i].num = num;
        syms_used += num;
        sym_idx += num + space;
        off += 2;
    }
    if (sym_idx >= 256 || syms_used >= num_symbols || sym_idx + num_symbols - syms_used > 256)
        return -1;
    range[num_ranges].symbol = sym_idx;
    range[num_ranges].num = num_symbols - syms_used;
    return num_ranges + 1;
}

static int code_lengths_new(bitreader *bits, int blen, uint8_t *syms, uint32_t *prefix)
{
    int forced = (int)read_bits_nr(bits, 2);
    int num_symbols = (int)read_bits_nr(bits, 8) + 1;
    int fluff = read_fluff(bits, num_symbols);
    uint8_t code_len[CODELEN_SIZE];
    memset(code_len, 0, sizeof(code_len));
    bitreader2 br2 = {bits->B, blen, bits->P - ((24 - bits->BitPos + 7) >> 3), bits->Bound,
                      (bits->BitPos - 24) & 7};
    if (!golomb_rice_lengths(code_len, num_symbols + fluff, &br2))
        return -1;
    for (int z = 0; z < 16; z++)
        code_len[num_symbols + fluff + z] = 0;
    if (!golomb_rice_bits(code_len, num_symbols, forced, &br2))
        return -1;
    /* restart the bit reader at br2's position */
    bits->BitPos = 24;
    bits->P = br2.P;
    bits->Bits = 0;
    refill(bits);
    bits->Bits = SHL(bits->Bits, br2.BitPos);
    bits->BitPos += br2.BitPos;
    uint32_t running_sum = 0x1e;
    for (int i = 0; i < num_symbols; i++) {
        int v = code_len[i];
        v = -(v & 1) ^ (v >> 1);
        int cl = v + (int)(running_sum >> 2) + 1;
        if (cl < 1 || cl > 11)
            return -1;
        code_len[i] = (uint8_t)cl;
        running_sum = (uint32_t)((int)running_sum + v);
    }
    huff_range range[128];
    int ranges = convert_to_ranges(range, num_symbols, fluff, code_len, num_symbols, bits);
    if (ranges <= 0)
        return -1;
    int cp = 0;
    for (int i = 0; i < ranges; i++) {
        int sym = range[i].symbol, nn = range[i].num;
        do {
            if (cp >= CODELEN_SIZE || sym > 255)
                return -1;
            syms[prefix[code_len[cp++]]++] = (uint8_t)sym++;
        } while (--nn > 0);
    }
    return num_symbols;
}

typedef struct huff_reader {
    const uint8_t *B;
    int Blen;
    uint8_t *out;
    int src, src_end, src_mid, src_mid_org;
    int out_off, out_end;
} huff_reader;

/* 3-stream scalar Huffman core */
static bool decode_bytes_core(const huff_reader *hr, const huff_lut *lut)
{
    const uint8_t *B = hr->B;
    int blen = hr->Blen;
    uint8_t *outp = hr->out;
    int src = hr->src, src_mid = hr->src_mid, src_end = hr->src_end;
    uint32_t src_bits = 0, src_mid_bits = 0, src_end_bits = 0;
    int src_bitpos = 0, src_mid_bitpos = 0, src_end_bitpos = 0;
    int dst = hr->out_off, dst_end = hr->out_end, k, n;
    if (src > src_mid)
        return false;
    while (dst < dst_end) {
        if (src_mid - src <= 1) {
            if (src_mid - src == 1)
                src_bits |= SHL(at(B, blen, src), src_bitpos);
        } else {
            src_bits |= SHL(at(B, blen, src) | (at(B, blen, src + 1) << 8), src_bitpos);
        }
        k = (int)(src_bits & 0x7FF);
        n = lut->bits2len[k];
        src_bitpos -= n;
        src_bits = SHR(src_bits, n);
        outp[dst++] = lut->bits2sym[k];
        src += (7 - src_bitpos) >> 3;
        src_bitpos &= 7;
        if (dst < dst_end) {
            if (src_end - src_mid <= 1) {
                if (src_end - src_mid == 1) {
                    src_end_bits |= SHL(at(B, blen, src_mid), src_end_bitpos);
                    src_mid_bits |= SHL(at(B, blen, src_mid), src_mid_bitpos);
                }
            } else {
                uint32_t vv = (uint32_t)(at(B, blen, src_end - 2) | (at(B, blen, src_end - 1) << 8));
                src_end_bits |= SHL(((vv >> 8) | (vv << 8)) & 0xFFFF, src_end_bitpos);
                src_mid_bits |= SHL(at(B, blen, src_mid) | (at(B, blen, src_mid + 1) << 8), src_mid_bitpos);
            }
            k = (int)(src_end_bits & 0x7FF);
            n = lut->bits2len[k];
            outp[dst++] = lut->bits2sym[k];
            src_end_bitpos -= n;
            src_end_bits = SHR(src_end_bits, n);
            src_end -= (7 - src_end_bitpos) >> 3;
            src_end_bitpos &= 7;
            if (dst < dst_end) {
                k = (int)(src_mid_bits & 0x7FF);
                n = lut->bits2len[k];
                outp[dst++] = lut->bits2sym[k];
                src_mid_bitpos -= n;
                src_mid_bits = SHR(src_mid_bits, n);
                src_mid += (7 - src_mid_bitpos) >> 3;
                src_mid_bitpos &= 7;
            }
        }
        if (src > src_mid || src_mid > src_end)
            return false;
    }
    return src == hr->src_mid_org && src_end == src_mid;
}

static int decode_bytes_type12(const uint8_t *B, int blen, int src_start, int src_size, uint8_t *output,
                               int output_size, int type)
{
    int src_end = src_start + src_size;
    bitreader bits = br_forward(B, src_start, src_end);
    uint32_t prefix[12];
    memcpy(prefix, g_code_prefix_org, sizeof(prefix));
    uint8_t syms[1280];
    int num_syms;
    if (read_bit_nr(&bits) == 0)
        num_syms = code_lengths_old(&bits, syms, prefix);
    else if (read_bit_nr(&bits) == 0)
        num_syms = code_lengths_new(&bits, blen, syms, prefix);
    else
        return -1;
    if (num_syms < 1)
        return -1;
    int sp = bits.P - ((24 - bits.BitPos) / 8);
    if (num_syms == 1) {
        memset(output, syms[0], (size_t)output_size);
        return src_size;
    }
    huff_lut lut, rev;
    memset(&lut, 0, sizeof(lut));
    memset(&rev, 0, sizeof(rev));
    if (!huff_make_lut(g_code_prefix_org, prefix, &lut, syms))
        return -1;
    for (int i = 0; i < 2048; i++) {
        int j = reverse11(i);
        rev.bits2len[i] = lut.bits2len[j];
        rev.bits2sym[i] = lut.bits2sym[j];
    }
    if (type == 1) {
        if (sp + 3 > src_end)
            return -1;
        int split_mid = at(B, blen, sp) | (at(B, blen, sp + 1) << 8);
        sp += 2;
        huff_reader hr = {B, blen, output, sp, src_end, sp + split_mid, sp + split_mid, 0, output_size};
        if (!decode_bytes_core(&hr, &rev))
            return -1;
    } else {
        if (sp + 6 > src_end)
            return -1;
        int half_out = (output_size + 1) >> 1;
        int split_mid = at(B, blen, sp) | (at(B, blen, sp + 1) << 8) | (at(B, blen, sp + 2) << 16);
        sp += 3;
        if (split_mid > src_end - sp)
            return -1;
        int src_mid = sp + split_mid;
        int split_left = at(B, blen, sp) | (at(B, blen, sp + 1) << 8);
        sp += 2;
        if (src_mid - sp < split_left + 2 || src_end - src_mid < 3)
            return -1;
        int split_right = at(B, blen, src_mid) | (at(B, blen, src_mid + 1) << 8);
        if (src_end - (src_mid + 2) < split_right + 2)
            return -1;
        huff_reader hr1 = {B, blen, output, sp, src_mid, sp + split_left, sp + split_left, 0, half_out};
        if (!decode_bytes_core(&hr1, &rev))
            return -1;
        huff_reader hr2 = {B,       blen, output, src_mid + 2, src_end, src_mid + 2 + split_right,
                           src_mid + 2 + split_right, half_out, output_size};
        if (!decode_bytes_core(&hr2, &rev))
            return -1;
    }
    return src_size;
}

/* DecodeBytes: returns the source bytes used, -1 malformed, -2 unsupported.
 * output must hold output_cap bytes. */
static int decode_bytes(const uint8_t *B, int blen, int sp, int src_end, int output_cap, uint8_t *output,
                        int *decoded)
{
    *decoded = 0;
    int src_org = sp, src_size, dst_size;
    if (src_end - sp < 2)
        return -1;
    int chunk_type = (B[sp] >> 4) & 7;
    if (chunk_type == 0) {
        if (B[sp] >= 0x80) {
            src_size = ((B[sp] << 8) | B[sp + 1]) & 0xFFF;
            sp += 2;
        } else {
            if (src_end - sp < 3)
                return -1;
            src_size = (B[sp] << 16) | (B[sp + 1] << 8) | B[sp + 2];
            if (src_size & ~0x3FFFF)
                return -1;
            sp += 3;
        }
        if (src_size > output_cap || src_end - sp < src_size)
            return -1;
        memcpy(output, B + sp, (size_t)src_size);
        *decoded = src_size;
        return sp + src_size - src_org;
    }
    if (B[sp] >= 0x80) {
        if (src_end - sp < 3)
            return -1;
        uint32_t bits = (uint32_t)((B[sp] << 16) | (B[sp + 1] << 8) | B[sp + 2]);
        src_size = (int)(bits & 0x3FF);
        dst_size = (int)(src_size + ((bits >> 10) & 0x3FF) + 1);
        sp += 3;
    } else {
        if (src_end - sp < 5)
            return -1;
        uint32_t bits = ((uint32_t)B[sp + 1] << 24) | ((uint32_t)B[sp + 2] << 16) | ((uint32_t)B[sp + 3] << 8) |
                        B[sp + 4];
        src_size = (int)(bits & 0x3FFFF);
        dst_size = (int)((((bits >> 18) | ((uint32_t)B[sp] << 14)) & 0x3FFFF) + 1);
        if (src_size >= dst_size)
            return -1;
        sp += 5;
    }
    if (src_end - sp < src_size || dst_size > output_cap)
        return -1;
    int used;
    switch (chunk_type) {
    case 2:
    case 4:
        used = decode_bytes_type12(B, blen, sp, src_size, output, dst_size, chunk_type >> 1);
        break;
    default:
        return -2;
    }
    if (used != src_size)
        return -1;
    *decoded = dst_size;
    return sp + src_size - src_org;
}

/* ---- LZ table ----------------------------------------------------------- */

typedef struct lz_table {
    int lit_size, cmd_size, offs_size, len_size;
} lz_table;

static bool unpack_offsets(kraken_scratch *s, const uint8_t *B, int bs_begin, int bs_end, bool excess_flag,
                           int excess_count, int offs_count, int offs_scaling, int len_count)
{
    bitreader a = br_forward(B, bs_begin, bs_end);
    bitreader b = br_backward(B, bs_begin, bs_end);
    int u32_size = 0;
    if (!excess_flag) {
        if (b.Bits < 0x2000)
            return false;
        int nn = 31 - bsr(b.Bits);
        b.BitPos += nn;
        b.Bits = SHL(b.Bits, nn);
        refill_b(&b);
        nn++;
        u32_size = (int)(SHR(b.Bits, 32 - nn) - 1);
        b.BitPos += nn;
        b.Bits = SHL(b.Bits, nn);
        refill_b(&b);
    } else {
        for (int q = 0; q < len_count; q++)
            if (s->litlen[q] == 255)
                u32_size++;
    }
    int32_t *offs = s->offs;
    if (offs_scaling == 0) {
        int i = 0;
        while (i < offs_count) {
            offs[i] = -(int32_t)read_distance(&a, s->offs_packed[i]);
            i++;
            if (i >= offs_count)
                break;
            offs[i] = -(int32_t)read_distance(&b, s->offs_packed[i]);
            i++;
        }
    } else {
        int i = 0;
        while (i < offs_count) {
            uint32_t cmd = s->offs_packed[i];
            if ((cmd >> 3) > 26)
                return false;
            uint32_t off = SHL(8u + (cmd & 7), (int)(cmd >> 3)) | read_more_than_24(&a, (int)(cmd >> 3));
            offs[i] = 8 - (int32_t)off;
            i++;
            if (i >= offs_count)
                break;
            cmd = s->offs_packed[i];
            if ((cmd >> 3) > 26)
                return false;
            off = SHL(8u + (cmd & 7), (int)(cmd >> 3)) | read_more_than_24(&b, (int)(cmd >> 3));
            offs[i] = 8 - (int32_t)off;
            i++;
        }
        /* ooz: offs = scaling * offs - extra (offs are negative distances) */
        if (offs_scaling != 1)
            for (int j = 0; j < offs_count; j++)
                offs[j] = offs_scaling * offs[j] - s->offs_extra[j];
    }
    if (u32_size > 512)
        return false;
    uint32_t u32[512];
    if (excess_flag) {
        /* escapes live in a separate dual bitstream over the excess tail */
        bitreader ea = br_forward(B, bs_end, bs_end + excess_count);
        bitreader eb = br_backward(B, bs_end, bs_end + excess_count);
        int i = 0;
        for (; i + 1 < u32_size; i += 2) {
            if (!read_length(&ea, &u32[i]) || !read_length(&eb, &u32[i + 1]))
                return false;
        }
        if (i < u32_size && !read_length(&ea, &u32[i]))
            return false;
    } else {
        int i = 0;
        for (; i + 1 < u32_size; i += 2) {
            if (!read_length(&a, &u32[i]) || !read_length(&b, &u32[i + 1]))
                return false;
        }
        if (i < u32_size && !read_length(&a, &u32[i]))
            return false;
    }
    int a_seam = a.P - ((24 - a.BitPos) >> 3);
    int b_seam = b.P + ((24 - b.BitPos) >> 3);
    if (a_seam != b_seam && !excess_flag)
        return false;
    int u = 0;
    for (int i = 0; i < len_count; i++) {
        uint32_t v = s->litlen[i];
        if (v == 255) {
            if (u >= u32_size)
                return false;
            v = u32[u++] + 255;
        }
        s->lens[i] = (int32_t)v + 3;
    }
    return u == u32_size;
}

/* returns the source position after the table, or -1 / -2 */
static int read_lz_table(kraken_scratch *s, const uint8_t *B, int blen, int sp, int src_end, uint8_t *dst,
                         int dst_start, int dst_size, int offset, lz_table *t)
{
    if (offset == 0) {
        if (src_end - sp < SEED_SIZE)
            return -1;
        memcpy(dst + dst_start, B + sp, SEED_SIZE);
        sp += SEED_SIZE;
    }
    bool excess_flag = false;
    int excess_count = 0;
    if (sp < src_end && (B[sp] & 0x80)) {
        uint8_t flag = B[sp++];
        if ((flag & 0xC0) != 0x80)
            return -1;
        excess_flag = true;
        excess_count = flag & 0x3F;
        if (excess_count > 0x1F) {
            if (sp >= src_end)
                return -1;
            excess_count += B[sp++] * 0x20;
        }
        src_end -= excess_count;
        if (src_end < sp)
            return -1;
    }
    int n = decode_bytes(B, blen, sp, src_end, dst_size, s->lit, &t->lit_size);
    if (n < 0)
        return n;
    sp += n;
    n = decode_bytes(B, blen, sp, src_end, dst_size, s->cmd, &t->cmd_size);
    if (n < 0)
        return n;
    sp += n;
    if (src_end - sp < 3)
        return -1;
    int offs_scaling = 0, offs_count;
    if (B[sp] & 0x80) {
        offs_scaling = B[sp] - 127;
        sp++;
        n = decode_bytes(B, blen, sp, src_end, t->cmd_size, s->offs_packed, &offs_count);
        if (n < 0)
            return n;
        sp += n;
        if (offs_scaling != 1) {
            int extra_count;
            n = decode_bytes(B, blen, sp, src_end, offs_count, s->offs_extra, &extra_count);
            if (n < 0)
                return n;
            if (extra_count != offs_count)
                return -1;
            sp += n;
        }
    } else {
        n = decode_bytes(B, blen, sp, src_end, t->cmd_size, s->offs_packed, &offs_count);
        if (n < 0)
            return n;
        sp += n;
    }
    t->offs_size = offs_count;
    int len_count;
    n = decode_bytes(B, blen, sp, src_end, dst_size >> 2, s->litlen, &len_count);
    if (n < 0)
        return n;
    sp += n;
    t->len_size = len_count;
    return unpack_offsets(s, B, sp, src_end, excess_flag, excess_count, offs_count, offs_scaling, len_count)
               ? sp
               : -1;
}

/* mode 1: raw literals; mode 0: literals are deltas against the last match */
static bool process_lz_runs(kraken_scratch *s, const lz_table *t, int mode, uint8_t *dst, int total,
                            int dst_pos, int dst_end, int dst_start)
{
    const uint8_t *cmd = s->cmd, *lit = s->lit;
    const int32_t *len = s->lens, *offs = s->offs;
    int cmd_pos = 0, len_pos = 0, lit_pos = 0, offs_pos = 0;
    int recent[7] = {0, 0, 0, -8, -8, -8, 0};
    int last_offset = -8;
    while (cmd_pos < t->cmd_size) {
        uint32_t f = cmd[cmd_pos++];
        int64_t litlen = f & 3;
        int offs_index = (int)(f >> 6);
        uint32_t matchlen = (f >> 2) & 0xF;
        if (litlen == 3) {
            if (len_pos >= t->len_size)
                return false;
            litlen = len[len_pos++];
        }
        recent[6] = offs_pos < t->offs_size ? offs[offs_pos] : 0;
        if (litlen != 0) {
            if (lit_pos + litlen > t->lit_size || dst_pos + litlen > dst_end)
                return false;
            if (mode == 1) {
                memcpy(dst + dst_pos, lit + lit_pos, (size_t)litlen);
            } else {
                int64_t from = (int64_t)dst_pos + last_offset;
                if (from < 0 || from + litlen > total)
                    return false;
                for (int c = 0; c < litlen; c++)
                    dst[dst_pos + c] = (uint8_t)(lit[lit_pos + c] + dst[from + c]);
            }
            dst_pos += (int)litlen;
            lit_pos += (int)litlen;
        }
        int offset = recent[offs_index + 3];
        recent[offs_index + 3] = recent[offs_index + 2];
        recent[offs_index + 2] = recent[offs_index + 1];
        recent[offs_index + 1] = recent[offs_index + 0];
        recent[3] = offset;
        last_offset = offset;
        offs_pos += ((offs_index + 1) & 4) >> 2;
        int64_t copy_from = (int64_t)dst_pos + offset;
        if (copy_from < dst_start)
            return false;
        uint32_t actual;
        if (matchlen != 15) {
            actual = matchlen + 2;
        } else {
            if (len_pos >= t->len_size)
                return false;
            actual = 14 + (uint32_t)len[len_pos++];
        }
        if ((int64_t)dst_pos + actual > dst_end || copy_from + actual > total)
            return false;
        for (uint32_t c = 0; c < actual; c++)
            dst[dst_pos + c] = dst[copy_from + c];
        dst_pos += (int)actual;
    }
    if (offs_pos != t->offs_size || len_pos != t->len_size)
        return false;
    int final_len = dst_end - dst_pos;
    if (final_len != t->lit_size - lit_pos)
        return false;
    if (mode == 1) {
        memcpy(dst + dst_pos, lit + lit_pos, (size_t)final_len);
    } else {
        int64_t from = (int64_t)dst_pos + last_offset;
        if (final_len > 0 && (from < 0 || from + final_len > total))
            return false;
        for (int c = 0; c < final_len; c++)
            dst[dst_pos + c] = (uint8_t)(lit[lit_pos + c] + dst[from + c]);
    }
    return true;
}

static kraken_status decode_chunk(kraken_scratch *s, const uint8_t *B, int blen, int src_start, int src_len,
                                  uint8_t *dst, int total, int dst_start, int dst_size, bool with_seed,
                                  int literal_mode)
{
    if (dst_size <= 0 || dst_start + dst_size > total || src_len < 0 || src_start + src_len > blen)
        return KRAKEN_MALFORMED;
    int offset = with_seed ? 0 : dst_start;
    lz_table t = {0};
    int rc = read_lz_table(s, B, blen, src_start, src_start + src_len, dst, dst_start, dst_size, offset, &t);
    if (rc < 0)
        return rc == -2 ? KRAKEN_UNSUPPORTED : KRAKEN_MALFORMED;
    int start = dst_start + (offset == 0 ? SEED_SIZE : 0);
    return process_lz_runs(s, &t, literal_mode, dst, total, start, dst_start + dst_size, dst_start - offset)
               ? KRAKEN_OK
               : KRAKEN_MALFORMED;
}

static kraken_status decode_sub_chunk(kraken_scratch *s, const uint8_t *B, int blen, int src_start, int src_len,
                                      uint8_t *dst, int total, int dst_start, int dst_len, bool lz, bool restart,
                                      int literal_mode)
{
    if (src_len < 0 || src_start + src_len > blen || dst_len < 0 || dst_start + dst_len > total)
        return KRAKEN_MALFORMED;
    if (src_len == dst_len) {
        memcpy(dst + dst_start, B + src_start, (size_t)dst_len);
        return KRAKEN_OK;
    }
    if (lz)
        return decode_chunk(s, B, blen, src_start, src_len, dst, total, dst_start, dst_len, restart, literal_mode);
    /* a bare entropy-coded array */
    int count;
    int n = decode_bytes(B, blen, src_start, src_start + src_len, dst_len, dst + dst_start, &count);
    if (n < 0)
        return n == -2 ? KRAKEN_UNSUPPORTED : KRAKEN_MALFORMED;
    return n == src_len && count == dst_len ? KRAKEN_OK : KRAKEN_MALFORMED;
}

kraken_status kraken_decode_block(kraken_scratch *s, const uint8_t *src, size_t src_len, int flags,
                                  int first_chunk_comp, uint8_t *dst, size_t dst_len)
{
    if (dst_len == 0)
        return src_len == 0 ? KRAKEN_OK : KRAKEN_MALFORMED;
    if (dst_len > 2 * CHUNK_MAX || src_len > 0x7fffffff)
        return KRAKEN_MALFORMED;
    int blen = (int)src_len, total = (int)dst_len;
    int c0_dst = total < CHUNK_MAX ? total : CHUNK_MAX;
    int c1_dst = total - c0_dst;
    int c0_comp = c1_dst == 0 ? blen : first_chunk_comp;
    if (c0_comp <= 0 || c0_comp > blen)
        return KRAKEN_MALFORMED;
    int c1_comp = blen - c0_comp;
    if (c1_dst == 0 && c1_comp != 0)
        return KRAKEN_MALFORMED;
    kraken_status st = decode_sub_chunk(s, src, blen, 0, c0_comp, dst, total, 0, c0_dst, (flags & 0x02) != 0, true,
                                        (flags & 0x01) ? 0 : 1);
    if (st != KRAKEN_OK || c1_dst == 0)
        return st;
    return decode_sub_chunk(s, src, blen, c0_comp, c1_comp, dst, total, c0_dst, c1_dst, (flags & 0x20) != 0,
                            (flags & 0x40) != 0, (flags & 0x10) ? 0 : 1);
}
