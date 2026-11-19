/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GFX10 tiled surfaces: element offsets of the swizzle modes the renderer
 * reads, block sizes, mip placement and detiling (from vk_pipeline.c). */
#ifndef PS5C_VK_TEXTURE_TILING_H
#define PS5C_VK_TEXTURE_TILING_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "gpu/vk/vk_texture_layout.h"

/* Byte offset of element (x, y) inside a 4 KiB standard-swizzle block (SW_4KB_S). */
static inline uint32_t std4k_offset(uint32_t x, uint32_t y, uint32_t bpe) {
    switch (bpe) {
    case 1:
        return ((y << 4) & 0x1F0u) ^ ((y << 5) & 0x400u) ^ (x & 0x00Fu) ^ ((x << 5) & 0x200u) ^ ((x << 6) & 0x800u);
    case 2:
        return ((y << 4) & 0x070u) ^ ((y << 5) & 0x100u) ^ ((y << 6) & 0x400u) ^ ((x << 1) & 0x00Eu) ^
               ((x << 4) & 0x080u) ^ ((x << 5) & 0x200u) ^ ((x << 6) & 0x800u);
    case 4:
        return ((y << 4) & 0x070u) ^ ((y << 5) & 0x100u) ^ ((y << 6) & 0x400u) ^ ((x << 2) & 0x00Cu) ^
               ((x << 5) & 0x080u) ^ ((x << 6) & 0x200u) ^ ((x << 7) & 0x800u);
    case 8:
        return ((y << 4) & 0x030u) ^ ((y << 6) & 0x100u) ^ ((y << 7) & 0x400u) ^ ((x << 3) & 0x008u) ^
               ((x << 5) & 0x0C0u) ^ ((x << 6) & 0x200u) ^ ((x << 7) & 0x800u);
    default:
        return ((y << 4) & 0x030u) ^ ((y << 6) & 0x100u) ^ ((y << 7) & 0x400u) ^ ((x << 6) & 0x0C0u) ^
               ((x << 7) & 0x200u) ^ ((x << 8) & 0x800u);
    }
}

static inline void std4k_block(uint32_t bpe, uint32_t *bw, uint32_t *bh) {
    *bw = bpe <= 2 ? 64 : bpe <= 8 ? 32 : 16;
    *bh = bpe == 1 ? 64 : bpe <= 4 ? 32 : 16;
}

/* SW_64KB_R_X (mode 27, render targets): byte offset of element (x, y) in
 * its 64 KiB block, as the PS5 lays it out (x, y below the block size). */
static inline uint32_t rt64k_offset(uint32_t x, uint32_t y, uint32_t bpe) {
    switch (bpe) {
    case 1:
        return ((y << 2) & 0x0008u) ^ ((y << 4) & 0x0010u) ^ ((y << 3) & 0x00A0u) ^ ((y << 5) & 0x0F00u) ^
               ((y << 6) & 0x1000u) ^ ((y << 7) & 0x4000u) ^ (x & 0x0007u) ^ ((x << 3) & 0x0040u) ^
               ((x << 5) & 0x0300u) ^ ((x << 4) & 0x0400u) ^ ((x << 6) & 0x0800u) ^ ((x << 7) & 0x2000u) ^
               ((x << 8) & 0x8000u);
    case 2:
        return ((y << 4) & 0x0070u) ^ ((y << 5) & 0x0F00u) ^ ((y << 8) & 0x5000u) ^ ((x << 1) & 0x000Eu) ^
               ((x << 4) & 0x0480u) ^ ((x << 5) & 0x0300u) ^ ((x << 6) & 0x0800u) ^ ((x << 7) & 0x2000u) ^
               ((x << 8) & 0x8000u);
    case 4:
        return ((y << 4) & 0x0070u) ^ ((y << 5) & 0x0F00u) ^ ((y << 9) & 0x1000u) ^ ((y << 8) & 0x4000u) ^
               ((x << 2) & 0x000Cu) ^ ((x << 5) & 0x0380u) ^ ((x << 4) & 0x0400u) ^ ((x << 6) & 0x0800u) ^
               ((x << 9) & 0xA000u);
    case 8:
        return ((y << 4) & 0x0010u) ^ ((y << 6) & 0x0080u) ^ ((y << 5) & 0x0F00u) ^ ((y << 10) & 0x5000u) ^
               ((x << 3) & 0x0008u) ^ ((x << 4) & 0x0460u) ^ ((x << 5) & 0x0300u) ^ ((x << 6) & 0x0800u) ^
               ((x << 10) & 0x2000u) ^ ((x << 9) & 0x8000u);
    default:
        return ((x << 4) & 0x0410u) ^ ((x << 5) & 0x0340u) ^ ((x << 6) & 0x0800u) ^ ((x << 11) & 0xA000u) ^
               ((y << 5) & 0x0F20u) ^ ((y << 6) & 0x0080u) ^ ((y << 10) & 0x1000u) ^ ((y << 11) & 0x4000u);
    }
}

/* SW_64KB_Z_X (mode 24, depth and textures sharing its layout); up to 8 bytes per element. */
static inline uint32_t z64k_offset(uint32_t x, uint32_t y, uint32_t bpe) {
    switch (bpe) {
    case 1:
        return (x & 0x0001u) ^ ((x << 1) & 0x0004u) ^ ((x << 2) & 0x0010u) ^ ((x << 3) & 0x0040u) ^
               ((x << 5) & 0x0300u) ^ ((x << 4) & 0x0400u) ^ ((x << 6) & 0x0800u) ^ ((x << 7) & 0x2000u) ^
               ((x << 8) & 0x8000u) ^ ((y << 1) & 0x0002u) ^ ((y << 2) & 0x0008u) ^ ((y << 3) & 0x00A0u) ^
               ((y << 5) & 0x0F00u) ^ ((y << 6) & 0x1000u) ^ ((y << 7) & 0x4000u);
    case 2:
        return ((x << 1) & 0x0002u) ^ ((x << 2) & 0x0008u) ^ ((x << 3) & 0x0020u) ^ ((x << 4) & 0x0480u) ^
               ((x << 5) & 0x0300u) ^ ((x << 6) & 0x0800u) ^ ((x << 7) & 0x2000u) ^ ((x << 8) & 0x8000u) ^
               ((y << 2) & 0x0004u) ^ ((y << 3) & 0x0010u) ^ ((y << 4) & 0x0040u) ^ ((y << 5) & 0x0F00u) ^
               ((y << 8) & 0x5000u);
    case 4:
        return ((x << 2) & 0x0004u) ^ ((x << 3) & 0x0010u) ^ ((x << 4) & 0x0440u) ^ ((x << 5) & 0x0300u) ^
               ((x << 6) & 0x0800u) ^ ((x << 9) & 0xA000u) ^ ((y << 3) & 0x0008u) ^ ((y << 4) & 0x0020u) ^
               ((y << 5) & 0x0F80u) ^ ((y << 9) & 0x1000u) ^ ((y << 8) & 0x4000u);
    default:
        return ((x << 3) & 0x0008u) ^ ((x << 4) & 0x0420u) ^ ((x << 5) & 0x0380u) ^ ((x << 6) & 0x0800u) ^
               ((x << 10) & 0x2000u) ^ ((x << 9) & 0x8000u) ^ ((y << 4) & 0x0010u) ^ ((y << 5) & 0x0F40u) ^
               ((y << 10) & 0x5000u);
    }
}

/* PS4 (GCN) 1D-tiled surfaces: 8x8-element micro tiles in rows, as many per row as the pitch
 * holds; the pitch covers whole micro tiles of at least 256 bytes per row (addrlib's micro-tiled
 * alignment). Renderer-internal swizzle values, past GFX10's 5-bit field:
 * GCN_SW_1D_THIN (tiling index 13): elements in x0 y0 x1 y1 x2 y2 order;
 * GCN_SW_1D_DISPLAY (index 9): the displayable order, by element size. */
#define GCN_SW_1D_THIN 32u
#define GCN_SW_1D_DISPLAY 33u
#define GCN_SW_2D_THIN 34u    /* tiling index 14 */
#define GCN_SW_2D_DISPLAY 35u /* tiling index 10 */
#define GCN_SW_LINEAR 36u     /* tiling index 8, linear aligned (SI pitch rules, level 0 first) */
#define GCN_SW_UNSUPPORTED 37u

static inline bool gcn_sw(uint32_t sw_mode) { return sw_mode >= GCN_SW_1D_THIN; }
static inline bool gcn_macro_sw(uint32_t sw_mode) { return sw_mode == GCN_SW_2D_THIN || sw_mode == GCN_SW_2D_DISPLAY; }

/* PS4 2D (macro) tiling, PS4 configuration: 8 pipes (P8_32x32_16x16), 256-byte pipe interleave,
 * no tile split for color (a micro tile never exceeds the split size) and macro tile modes by
 * micro tile size {bank height, banks, aspect}. A macro tile is blocks of 8x8-element micro
 * tiles (inner order as in the 1D modes) spread over pipes and banks; the pipe and bank bits
 * XOR x/y bits of the surface coordinates, including bits above the macro tile (the bank
 * rotates from one macro tile to the next). Equation bit i = parity of (mask & (x | y << 12)).
 * Layout per AMD addrlib (SI/CI macro tiling), arranged as in AnyPS5's GcnTiling. */
typedef struct gcn_macro_geom {
    uint32_t bw, bh, bytes, bits, banks;
    uint32_t eq[16];
} gcn_macro_geom;

static inline uint32_t gcn_x(uint32_t bit) { return 1u << bit; }
static inline uint32_t gcn_y(uint32_t bit) { return 1u << (12u + bit); }

static inline void gcn_micro_order(bool display, uint32_t log2b, uint32_t order[6]) {
    static const uint8_t thin[6] = {0, 12, 1, 13, 2, 14}; /* x0 y0 x1 y1 x2 y2, y bit n as 12 + n */
    static const uint8_t disp[5][6] = {{0, 1, 2, 13, 12, 14}, {0, 1, 2, 12, 13, 14}, {0, 1, 12, 2, 13, 14},
                                       {0, 12, 1, 2, 13, 14}, {12, 0, 1, 2, 13, 14}}; /* as gcn_micro_offset (addrlib) */
    for (uint32_t i = 0; i < 6; i++) order[i] = 1u << (display ? disp[log2b][i] : thin[i]);
}

static inline const gcn_macro_geom *gcn_macro(uint32_t sw_mode, uint32_t bpe) {
    static gcn_macro_geom cache[2][5];
    static int ready[2][5];
    uint32_t log2b = 0;
    while ((1u << log2b) < bpe && log2b < 4) log2b++;
    uint32_t d = sw_mode == GCN_SW_2D_DISPLAY;
    gcn_macro_geom *g = &cache[d][log2b];
    if (__atomic_load_n(&ready[d][log2b], __ATOMIC_ACQUIRE)) return g;
    static const uint8_t modes[5][3] = {{4, 16, 4}, {2, 16, 2}, {1, 16, 2}, {1, 16, 2}, {1, 8, 1}};
    uint32_t bank_h = modes[log2b][0], banks = modes[log2b][1], aspect = modes[log2b][2];
    uint32_t log2bh = (uint32_t)__builtin_ctz(bank_h), bank_bits = (uint32_t)__builtin_ctz(banks);
    uint32_t tile_bytes = 64u << log2b, order[6], inner[16] = {0}, bank[4] = {0};
    gcn_micro_order(d, log2b, order);
    for (uint32_t i = 0; i < 6; i++) inner[log2b + i] = order[i];
    uint32_t inner_bits = 6 + log2b;
    for (uint32_t i = 0; i < log2bh; i++) inner[inner_bits++] = gcn_y(3 + i);
    const uint32_t pipe[3] = {gcn_x(3) | gcn_y(3) | gcn_x(4), gcn_x(4) | gcn_y(4), gcn_x(5) | gcn_y(5)};
    uint32_t tx[4], ty[4];
    for (uint32_t i = 0; i < 4; i++) tx[i] = gcn_x(6 + i), ty[i] = gcn_y(3 + log2bh + i);
    if (banks == 16) {
        bank[0] = tx[0] | ty[3], bank[1] = tx[1] | ty[2] | ty[3], bank[2] = tx[2] | ty[1], bank[3] = tx[3] | ty[0];
    } else {
        bank[0] = tx[0] | ty[2], bank[1] = tx[1] | ty[1] | ty[2], bank[2] = tx[2] | ty[0];
    }
    gcn_macro_geom n;
    memset(&n, 0, sizeof(n));
    n.bw = 64 * aspect;
    n.bh = 8 * bank_h * banks / aspect;
    n.bytes = tile_bytes * bank_h * 8 * banks;
    n.bits = inner_bits + 3 + bank_bits;
    n.banks = banks;
    for (uint32_t i = 0; i < 8; i++) n.eq[i] = inner[i];
    for (uint32_t i = 0; i < 3; i++) n.eq[8 + i] = pipe[i];
    for (uint32_t i = 0; i < bank_bits; i++) n.eq[11 + i] = bank[i];
    for (uint32_t i = 8; i < inner_bits; i++) n.eq[i + 3 + bank_bits] = inner[i];
    *g = n;
    __atomic_store_n(&ready[d][log2b], 1, __ATOMIC_RELEASE);
    return g;
}

static inline uint32_t gcn_macro_offset(uint32_t sw_mode, uint32_t x, uint32_t y, uint32_t bpe) {
    const gcn_macro_geom *g = gcn_macro(sw_mode, bpe);
    uint32_t c = (x & 0xFFFu) | (y & 0xFFFu) << 12, off = 0;
    for (uint32_t i = 0; i < g->bits; i++) off |= (uint32_t)__builtin_parity(g->eq[i] & c) << i;
    return off;
}

/* A mip level of a 2D-tiled surface stays macro tiled while it covers a whole macro tile
 * (level 0 always); smaller levels are 1D (micro) tiled in the same element order. */
static inline uint32_t gcn_level_sw(uint32_t sw_mode, uint32_t level, uint32_t w_el, uint32_t h_el, uint32_t bpe) {
    if (!gcn_macro_sw(sw_mode) || level == 0) return sw_mode;
    const gcn_macro_geom *g = gcn_macro(sw_mode, bpe);
    if (w_el >= g->bw && h_el >= g->bh) return sw_mode;
    return sw_mode == GCN_SW_2D_THIN ? GCN_SW_1D_THIN : GCN_SW_1D_DISPLAY;
}

/* Micro tiles in a row of a GCN 1D surface w_el x h_el elements: the pitch covers whole micro
 * tiles and grows by one tile until the slice is a whole number of 256-byte pipe interleaves
 * (addrlib SI: HwlGetPitchAlignmentMicroTiled 8, HwlGetSizeAdjustmentMicroTiled). */
static inline uint32_t gcn_tiles_per_row(uint32_t w_el, uint32_t h_el, uint32_t bpe) {
    uint64_t rows = (uint64_t)(h_el + 7) / 8 * 8;
    uint32_t tiles = (w_el + 7) / 8;
    if (!rows) return tiles;
    while ((uint64_t)tiles * 8 * rows * bpe % 256) tiles++;
    return tiles;
}

/* Elements in a row of a GCN linear-aligned surface: aligned to max(8, 64 / bpe) and grown
 * until the slice holds a multiple of max(64, 256 / bpe) elements (addrlib SI linear). */
static inline uint32_t gcn_linear_pitch(uint32_t w_el, uint32_t h_el, uint32_t bpe) {
    uint32_t pa = 64 / bpe > 8 ? 64 / bpe : 8, sa = 256 / bpe > 64 ? 256 / bpe : 64;
    uint32_t p = (w_el + pa - 1) / pa * pa;
    if (!p) p = pa;
    while (h_el && (uint64_t)p * h_el % sa) p += pa;
    return p;
}

/* Bank rotation between the slices of a 2D-tiled array (not the PRT modes): slice n XORs
 * (banks / 2 - 1) * n into the bank bits, right above the pipe bits (addrlib, shadPS4). */
static inline uint32_t gcn_layer_xor(uint32_t sw_mode, uint32_t bpe, uint32_t layer) {
    if (!gcn_macro_sw(sw_mode) || !layer) return 0;
    const gcn_macro_geom *g = gcn_macro(sw_mode, bpe);
    return (((g->banks / 2 - 1) * layer) & (g->banks - 1)) << 11;
}

static inline uint32_t gcn_micro_offset(uint32_t sw_mode, uint32_t x, uint32_t y, uint32_t bpe) {
    uint32_t x0 = x & 1, x1 = (x >> 1) & 1, x2 = (x >> 2) & 1, y0 = y & 1, y1 = (y >> 1) & 1, y2 = (y >> 2) & 1, i;
    if (sw_mode == GCN_SW_1D_THIN || sw_mode == GCN_SW_2D_THIN) i = x0 | y0 << 1 | x1 << 2 | y1 << 3 | x2 << 4 | y2 << 5;
    else if (bpe == 1) i = x0 | x1 << 1 | x2 << 2 | y1 << 3 | y0 << 4 | y2 << 5;
    else if (bpe == 2) i = x0 | x1 << 1 | x2 << 2 | y0 << 3 | y1 << 4 | y2 << 5;
    else if (bpe == 4) i = x0 | x1 << 1 | y0 << 2 | x2 << 3 | y1 << 4 | y2 << 5;
    else if (bpe == 8) i = x0 | y0 << 1 | x1 << 2 | x2 << 3 | y1 << 4 | y2 << 5;
    else i = y0 | x0 << 1 | x1 << 2 | x2 << 3 | y1 << 4 | y2 << 5;
    return i * bpe;
}

/* Supported swizzles: SW_256B_S (mode 1), SW_4KB_S (mode 5), SW_64KB_S (mode 9), SW_64KB_Z_X
 * (24) and SW_64KB_R_X (27). The 64 KB standard pattern is the 4 KB one plus
 * four bits that keep alternating Y, X, Y, X above it, so a 64 KB block is
 * 4x4 blocks of 4 KB; the _X modes use the same 64 KB block dimensions. */
static inline bool std_supported(uint32_t sw_mode) {
    return sw_mode == 0 || sw_mode == 1 || sw_mode == 5 || sw_mode == 9 || sw_mode == 22 || sw_mode == 24 || sw_mode == 27 ||
           sw_mode == GCN_SW_1D_THIN || sw_mode == GCN_SW_1D_DISPLAY || gcn_macro_sw(sw_mode) ||
           sw_mode == GCN_SW_LINEAR;
}

/* SW_4KB_D_X (22), PS5's 16-pipe, 256-byte-interleave configuration.
 * AMD AddrLib GFX10_SW_4K_D_X_PATINFO (16 pipes) selects nibble01
 * {5,1,2,6,7} and nibble2 {21,22,23,24,25}. The high coordinates rotate
 * pipe bits between blocks, so retain them instead of masking to a block. */
static inline uint32_t display4k_x_offset(uint32_t x,uint32_t y,uint32_t bpe) {
    uint32_t low;
    if(bpe==1)low=(x&7u)|((y<<4)&0x90u)|((y<<2)&8u)|((y<<3)&0x20u)|((x<<3)&0x40u);
    else if(bpe==2 || bpe==4)low=std4k_offset(x,y,bpe)&255u;
    else if(bpe==8)low=((x<<3)&8u)|((y<<4)&0x10u)|((x<<4)&0x60u)|((y<<6)&0x80u);
    else low=((x<<4)&0x10u)|((x<<5)&0x40u)|((y<<5)&0x20u)|((y<<6)&0x80u);
    uint32_t bx=bpe<=2?6:bpe<=8?5:4,by=bpe==1?6:bpe<=4?5:4;
    return low | ((((x>>(bx+1))^(y>>(by-2)))&1u)<<8) |
        ((((x>>(bx-2))^(y>>(by+1)))&1u)<<9) |
        ((((x>>bx)^(y>>(by-1)))&1u)<<10) |
        ((((x>>(bx-1))^(y>>by))&1u)<<11);
}

static inline void std_block(uint32_t sw_mode, uint32_t bpe, uint32_t *bw, uint32_t *bh, uint32_t *bytes) {
    if (gcn_macro_sw(sw_mode)) {
        const gcn_macro_geom *g = gcn_macro(sw_mode, bpe);
        *bw = g->bw, *bh = g->bh, *bytes = g->bytes;
        return;
    }
    if (gcn_sw(sw_mode)) { /* a micro tile */
        *bw = *bh = 8;
        *bytes = 64 * bpe;
        return;
    }
    if (sw_mode == 1) { /* SW_256B_S: the low byte of the 4 KB pattern */
        *bw = bpe <= 2 ? 16 : bpe <= 8 ? 8 : 4;
        *bh = bpe == 1 ? 16 : bpe <= 4 ? 8 : 4;
        *bytes = 256;
        return;
    }
    std4k_block(bpe, bw, bh);
    *bytes = 4096;
    if (sw_mode == 9 || sw_mode == 24 || sw_mode == 27) {
        *bw *= 4;
        *bh *= 4;
        *bytes = 65536;
    }
}

static inline uint32_t std_offset(uint32_t sw_mode, uint32_t x, uint32_t y, uint32_t bpe) {
    if(sw_mode==22)return display4k_x_offset(x,y,bpe);
    if (gcn_macro_sw(sw_mode)) return gcn_macro_offset(sw_mode, x, y, bpe);
    if (gcn_sw(sw_mode)) return gcn_micro_offset(sw_mode, x, y, bpe);
    if (sw_mode == 24 || sw_mode == 27) {
        /* The 64-bit layouts read y bits above the block height. Keep
         * coordinates block-local (for example mode 24, bpe 8, y=64). */
        uint32_t bw, bh, bytes;
        std_block(sw_mode, bpe, &bw, &bh, &bytes);
        x &= bw - 1;
        y &= bh - 1;
        return sw_mode == 27 ? rt64k_offset(x, y, bpe) : z64k_offset(x, y, bpe);
    }
    if (sw_mode == 1) return std4k_offset(x, y, bpe) & 0xFFu;
    uint32_t off = std4k_offset(x, y, bpe);
    if (sw_mode == 9) {
        uint32_t tx = (x >> (bpe <= 2 ? 6u : bpe <= 8 ? 5u : 4u)) & 3u;
        uint32_t ty = (y >> (bpe == 1 ? 6u : bpe <= 4 ? 5u : 4u)) & 3u;
        off |= ((ty & 1u) << 12) | ((tx & 1u) << 13) | ((ty >> 1) << 14) | ((tx >> 1) << 15);
    }
    return off;
}

/* Number of contiguous horizontal elements starting at gx (within [gx, gx_end))
 * whose tiled byte offsets increase linearly by bpe inside the same block. */
static inline uint32_t std_contig_elements(uint32_t sw_mode, uint32_t bpe, uint32_t gx, uint32_t gx_end) {
    uint32_t mask = 0;
    if (sw_mode == GCN_SW_1D_THIN || sw_mode == GCN_SW_2D_THIN) {
        mask = 1u;
    } else if (sw_mode == GCN_SW_1D_DISPLAY || sw_mode == GCN_SW_2D_DISPLAY) {
        mask = bpe <= 2 ? 7u : bpe == 4 ? 3u : bpe == 8 ? 1u : 0u;
    } else if (sw_mode == 1 || sw_mode == 5 || sw_mode == 9) {
        mask = bpe == 1 ? 15u : bpe == 2 ? 7u : bpe == 4 ? 3u : bpe == 8 ? 1u : 0u;
    } else if (sw_mode == 27) {
        mask = bpe <= 2 ? 7u : bpe == 4 ? 3u : bpe == 8 ? 1u : 0u;
    } else if (sw_mode == 22) {
        mask = bpe <= 2 ? 7u : bpe == 4 ? 3u : bpe == 8 ? 1u : 0u;
    } else if (sw_mode == 24) {
        mask = bpe <= 8 ? 1u : 0u;
    }
    uint32_t span = (mask + 1u) - (gx & mask);
    uint32_t rem = gx_end - gx;
    return span < rem ? span : rem;
}

/* Mip levels of a tiled surface. GFX10 stores a mip chain from the smallest
 * level up: first the mip tail (one block holding every level from the first
 * one whose size in elements fits in half a block, at most 12 levels in a
 * 64 KB block, 8 in a 4 KB one), then the larger levels in decreasing index,
 * level 0 last. A level in the tail starts at a fixed element position of
 * the tail block (AMD addrlib tables, by log2 bytes per element). */
static const uint8_t k_tail64k[5][12][2] = {
    {{128, 0}, {0, 128}, {64, 0}, {0, 64}, {32, 0}, {16, 32}, {0, 48}, {0, 32}, {16, 16}, {16, 0}, {0, 16}, {0, 0}},
    {{128, 0}, {0, 64}, {64, 0}, {0, 32}, {32, 0}, {16, 16}, {0, 24}, {0, 16}, {16, 8}, {16, 0}, {0, 8}, {0, 0}},
    {{64, 0}, {0, 64}, {32, 0}, {0, 32}, {16, 0}, {8, 16}, {0, 24}, {0, 16}, {8, 8}, {8, 0}, {0, 8}, {0, 0}},
    {{64, 0}, {0, 32}, {32, 0}, {0, 16}, {16, 0}, {8, 8}, {0, 12}, {0, 8}, {8, 4}, {8, 0}, {0, 4}, {0, 0}},
    {{32, 0}, {0, 32}, {16, 0}, {0, 16}, {8, 0}, {4, 8}, {0, 12}, {0, 8}, {4, 4}, {4, 0}, {0, 4}, {0, 0}},
};
static const uint8_t k_tail4k[5][8][2] = {
    {{32, 0}, {16, 32}, {0, 48}, {0, 32}, {16, 16}, {16, 0}, {0, 16}, {0, 0}},
    {{32, 0}, {16, 16}, {0, 24}, {0, 16}, {16, 8}, {16, 0}, {0, 8}, {0, 0}},
    {{16, 0}, {8, 16}, {0, 24}, {0, 16}, {8, 8}, {8, 0}, {0, 8}, {0, 0}},
    {{16, 0}, {8, 8}, {0, 12}, {0, 8}, {8, 4}, {8, 0}, {0, 4}, {0, 0}},
    {{8, 0}, {4, 8}, {0, 12}, {0, 8}, {4, 4}, {4, 0}, {0, 4}, {0, 0}},
};

typedef struct mip_place {
    uint64_t offset; /* of the level's first block (the tail block for tail levels) */
    uint32_t ox, oy; /* element origin inside its blocks (tail levels) */
    bool tail;
} mip_place;

static inline uint32_t shift_ceil(uint32_t v, uint32_t s) { return (v + (1u << s) - 1) >> s; }

static inline uint64_t surface_bytes(uint32_t sw_mode, uint32_t w_el, uint32_t h_el, uint32_t bpe);

static inline void mip_locate(uint32_t sw_mode, uint32_t w, uint32_t h, uint32_t block, uint32_t bpe, uint32_t levels,
                       uint32_t level, mip_place *out) {
    memset(out, 0, sizeof(*out));
    if (!sw_mode) {
        out->offset = vk_linear_mip_offset((w + block - 1) / block, (h + block - 1) / block,
                                           bpe, levels, level);
        return;
    }
    if (gcn_sw(sw_mode)) { /* level 0 first, each level after the previous one */
        for (uint32_t l = 0; l < level; l++) {
            uint32_t mw = w >> l ? w >> l : 1, mh = h >> l ? h >> l : 1;
            uint32_t we = (mw + block - 1) / block, he = (mh + block - 1) / block;
            out->offset += surface_bytes(gcn_level_sw(sw_mode, l, we, he, bpe), we, he, bpe);
        }
        return;
    }
    uint32_t bw, bh, bytes;
    std_block(sw_mode, bpe, &bw, &bh, &bytes);
    uint32_t w0 = (w + block - 1) / block, h0 = (h + block - 1) / block;
    uint32_t wlim = bw / 2, hlim = bh, max_tail = bytes == 65536 ? 12 : bytes == 4096 ? 8 : 0, lb = 0;
    if (sw_mode == 24 && bpe < 4) wlim = 64, hlim = 128;
    while ((1u << lb) < bpe && lb < 4) lb++;
    uint32_t first_tail = levels;
    for (uint32_t l = 0; levels > 1 && l < levels; l++) {
        if (shift_ceil(w0, l) <= wlim && shift_ceil(h0, l) <= hlim && levels - l <= max_tail) {
            first_tail = l;
            break;
        }
    }
    if (level >= first_tail) {
        uint32_t i = level - first_tail;
        out->tail = true;
        out->ox = bytes == 65536 ? k_tail64k[lb][i][0] : k_tail4k[lb][i][0];
        out->oy = bytes == 65536 ? k_tail64k[lb][i][1] : k_tail4k[lb][i][1];
        return;
    }
    uint64_t offset = first_tail < levels ? bytes : 0;
    for (uint32_t l = first_tail; l-- > level + 1;) {
        uint32_t mw = shift_ceil(w0, l), mh = shift_ceil(h0, l);
        offset += (uint64_t)((mw + bw - 1) / bw) * ((mh + bh - 1) / bh) * bytes;
    }
    out->offset = offset;
}

/* Blocks in a row of a tiled surface w_el elements wide */
static inline uint32_t std_blocks_per_row(uint32_t sw_mode, uint32_t w_el, uint32_t h_el, uint32_t bpe) {
    if (gcn_sw(sw_mode) && !gcn_macro_sw(sw_mode)) return gcn_tiles_per_row(w_el, h_el, bpe);
    uint32_t bw, bh, bytes;
    std_block(sw_mode, bpe, &bw, &bh, &bytes);
    return (w_el + bw - 1) / bw;
}

/* Guest bytes a tiled or linear surface covers. */
static inline uint64_t surface_bytes(uint32_t sw_mode, uint32_t w_el, uint32_t h_el, uint32_t bpe) {
    if (sw_mode == 0) return vk_linear_pitch(w_el, bpe) * h_el;
    if (sw_mode == GCN_SW_LINEAR) return (uint64_t)gcn_linear_pitch(w_el, h_el, bpe) * bpe * h_el;
    uint32_t bw, bh, bytes;
    std_block(sw_mode, bpe, &bw, &bh, &bytes);
    return (uint64_t)std_blocks_per_row(sw_mode, w_el, h_el, bpe) * ((h_el + bh - 1) / bh) * bytes;
}

/* Linear surfaces: GFX10's, or PS4's (GCN_SW_LINEAR) */
static inline bool tex_linear(uint32_t sw_mode) { return sw_mode == 0 || sw_mode == GCN_SW_LINEAR; }
static inline uint64_t tex_linear_pitch(uint32_t sw_mode, uint32_t w_el, uint32_t h_el, uint32_t bpe) {
    return sw_mode == GCN_SW_LINEAR ? (uint64_t)gcn_linear_pitch(w_el, h_el, bpe) * bpe : vk_linear_pitch(w_el, bpe);
}

/* Guest surface -> tightly packed rows; (ox, oy): element origin of a mip tail level;
 * layer_xor: the bank rotation of a 2D-tiled PS4 array slice (gcn_layer_xor). */
static inline void untile_layer(const uint8_t *src, uint8_t *dst, uint32_t sw_mode, uint32_t w_el, uint32_t h_el,
                                uint32_t bpe, uint32_t ox, uint32_t oy, uint32_t layer_xor) {
    if (sw_mode == 0) {
        vk_linear_unpack(src, dst, w_el, h_el, bpe);
        return;
    }
    if (sw_mode == GCN_SW_LINEAR) {
        uint64_t pitch = tex_linear_pitch(sw_mode, w_el, h_el, bpe), row = (uint64_t)w_el * bpe;
        for (uint32_t y = 0; y < h_el; y++) memcpy(dst + y * row, src + y * pitch, (size_t)row);
        return;
    }
    uint32_t bw, bh, bytes;
    std_block(sw_mode, bpe, &bw, &bh, &bytes);
    uint32_t bw_shift = (uint32_t)__builtin_ctz(bw);
    uint32_t bh_shift = (uint32_t)__builtin_ctz(bh);
    uint32_t block_shift = (uint32_t)__builtin_ctz(bytes);
    uint32_t per_row = std_blocks_per_row(sw_mode, w_el + ox, h_el + oy, bpe);
    uint32_t gx_end = ox + w_el;
    /* Every swizzle here is linear over XOR (bit interleaves, parity equations, masks), so
     * std_offset(x, y) = std_offset(x, 0) ^ std_offset(0, y); the bank rotation XORs bits
     * inside the block too. Rows of whole runs of 8 or 16 bytes use a table of x offsets. */
    uint32_t step = std_contig_elements(sw_mode, bpe, 0, 16), step_bytes = step * bpe;
    uint32_t xo[512];
    if (!ox && !(w_el & (step - 1)) && (step_bytes == 16 || step_bytes == 8) && w_el / step <= 512) {
        uint32_t nx = w_el / step;
        for (uint32_t k = 0; k < nx; k++) xo[k] = std_offset(sw_mode, k * step, 0, bpe);
        for (uint32_t y = 0; y < h_el; y++) {
            uint8_t *row = dst + (uint64_t)y * w_el * bpe;
            uint32_t gy = y + oy;
            const uint8_t *row_src = src + (((uint64_t)(gy >> bh_shift) * per_row) << block_shift);
            uint32_t y_off = std_offset(sw_mode, 0, gy, bpe) ^ layer_xor;
            for (uint32_t k = 0; k < nx; k++, row += step_bytes) {
                uint64_t off = ((uint64_t)((k * step) >> bw_shift) << block_shift) + (y_off ^ xo[k]);
                memcpy(row, row_src + off, step_bytes == 16 ? 16 : 8);
            }
        }
        return;
    }
    for (uint32_t y = 0; y < h_el; y++) {
        uint8_t *row = dst + (uint64_t)y * w_el * bpe;
        uint32_t gy = y + oy;
        const uint8_t *row_src = src + (((uint64_t)(gy >> bh_shift) * per_row) << block_shift);
        uint32_t y_off = std_offset(sw_mode, 0, gy, bpe) ^ layer_xor;
        for (uint32_t x = 0; x < w_el;) {
            uint32_t gx = x + ox;
            uint32_t span = std_contig_elements(sw_mode, bpe, gx, gx_end);
            uint64_t off = ((uint64_t)(gx >> bw_shift) << block_shift) + (y_off ^ std_offset(sw_mode, gx, 0, bpe));
            memcpy(row + (uint64_t)x * bpe, row_src + off, (size_t)span * bpe);
            x += span;
        }
    }
}

static inline void untile(const uint8_t *src, uint8_t *dst, uint32_t sw_mode, uint32_t w_el, uint32_t h_el, uint32_t bpe,
                          uint32_t ox, uint32_t oy) {
    untile_layer(src, dst, sw_mode, w_el, h_el, bpe, ox, oy, 0);
}

/* Where slice `layer` of a mip level starts, relative to the level's mip_locate offset in the
 * layer-major layout (each slice a whole chain, `slice` bytes apart). A PS4 array with mips is
 * level-major instead: every slice of level 0, then every slice of level 1, ... (addrlib). */
static inline uint64_t tex_layer_offset(uint32_t sw_mode, uint32_t w, uint32_t h, uint32_t block, uint32_t bpe,
                                        uint32_t levels, uint32_t level, uint32_t layers, uint32_t layer, uint64_t slice) {
    if (!gcn_sw(sw_mode) || levels < 2 || layers < 2) return slice * layer;
    uint64_t before = 0, size = 0; /* level-major: the levels below hold `layers` slices each */
    for (uint32_t l = 0; l <= level; l++) {
        uint32_t mw = w >> l ? w >> l : 1, mh = h >> l ? h >> l : 1;
        uint32_t we = (mw + block - 1) / block, he = (mh + block - 1) / block;
        size = surface_bytes(gcn_level_sw(sw_mode, l, we, he, bpe), we, he, bpe);
        if (l < level) before += size;
    }
    /* mip_locate put the level after one slice of each level below it */
    return before * (layers - 1) + size * layer;
}

#endif
