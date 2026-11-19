/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GFX10 detiling (vk_texture_tiling.h): every supported swizzle mode maps a
 * block's elements one to one onto its bytes, SW_4KB_S / SW_64KB_S follow
 * the AMD standard-swizzle bit order, and a surface tiled element by element
 * (partial blocks included) comes back as the same packed rows. */
#include <stdio.h>
#include <stdlib.h>
#include "gpu/vk/vk_texture_tiling.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static const uint32_t k_modes[] = {1, 5, 9, 22, 24, 27};

/* Previous addressing, deliberately retaining the masks and divisions.
 * Using the optimized writer and reader together would hide a shared error. */
static uint32_t reference_offset(uint32_t mode,uint32_t x,uint32_t y,uint32_t bpe){
    if(mode==22) {
        /* Independent bit equations from AMD AddrLib's 16-pipe PATINFO:
         * nibble01 rows 5,1,2,6,7; nibble2 rows 21..25. A positive entry
         * selects x[entry-1], a negative entry y[-entry-1]. */
        static const int8_t low[5][8]={{1,2,3,-2,-1,-3,4,-4},{0,1,2,3,-1,-2,-3,4},
            {0,0,1,2,-1,-2,-3,3},{0,0,0,1,-1,2,3,-2},{0,0,0,0,1,-1,2,-2}};
        static const uint8_t high_x[5][4]={{7,4,6,5},{7,4,6,5},{6,3,5,4},{6,3,5,4},{5,2,4,3}};
        static const uint8_t high_y[5][4]={{4,7,5,6},{3,6,4,5},{3,6,4,5},{2,5,3,4},{2,5,3,4}};
        unsigned row=(unsigned)__builtin_ctz(bpe);uint32_t off=0;
        for(unsigned bit=0;bit<8;bit++){int c=low[row][bit];if(c)off|=((c>0?x>>(c-1):y>>(-c-1))&1u)<<bit;}
        for(unsigned bit=0;bit<4;bit++)off|=(((x>>high_x[row][bit])^(y>>high_y[row][bit]))&1u)<<(bit+8);
        return off;
    }
    uint32_t bw=bpe<=2?64:bpe<=8?32:16,bh=4096/(bw*bpe);
    if(mode==24 || mode==27){
        x&=bw*4-1;y&=bh*4-1;
        return mode==27?rt64k_offset(x,y,bpe):z64k_offset(x,y,bpe);
    }
    if(mode==1){bw=bpe<=2?16:bpe<=8?8:4;bh=256/(bw*bpe);return std4k_offset(x&(bw-1),y&(bh-1),bpe)&255;}
    uint32_t off=std4k_offset(x,y,bpe);
    if(mode==9){uint32_t tx=(x/bw)&3,ty=(y/bh)&3;off|=((ty&1)<<12)|((tx&1)<<13)|((ty>>1)<<14)|((tx>>1)<<15);}
    return off;
}
static int check_spans(uint32_t mode,uint32_t bpe){
    uint32_t bw,bh,bytes;std_block(mode,bpe,&bw,&bh,&bytes);
    for(uint32_t y=0;y<bh*2+3;y++)for(uint32_t x=0;x<bw*2+3;x++){
        if(std_offset(mode,x,y,bpe)!=reference_offset(mode,x,y,bpe)){fprintf(stderr,"offset mismatch mode%u bpe%u xy%u,%u: %u != %u\n",mode,bpe,x,y,std_offset(mode,x,y,bpe),reference_offset(mode,x,y,bpe));return 1;}
        /* Include arbitrary row endings as well as complete micro-rows. */
        for(uint32_t left=1;left<=19;left+=3){
            uint32_t n=std_contig_elements(mode,bpe,x,x+left);
            CHECK(n && n<=left && x/bw==(x+n-1)/bw);
            for(uint32_t k=1;k<n;k++)CHECK(reference_offset(mode,x+k,y,bpe)==reference_offset(mode,x,y,bpe)+k*bpe);
        }
    }
    return 0;
}

/* each element of one block at its own bpe-aligned offset inside the block */
static int check_bijection(uint32_t mode, uint32_t bpe) {
    uint32_t bw, bh, bytes;
    std_block(mode, bpe, &bw, &bh, &bytes);
    CHECK(bw * bh * bpe == bytes);
    uint8_t *seen = calloc(bytes / bpe, 1);
    CHECK(seen);
    for (uint32_t y = 0; y < bh; y++)
        for (uint32_t x = 0; x < bw; x++) {
            uint32_t off = std_offset(mode, x, y, bpe);
            if (off % bpe || off >= bytes || seen[off / bpe]) {
                fprintf(stderr, "mode %u bpe %u: (%u, %u) -> 0x%x\n", mode, bpe, x, y, off);
                free(seen);
                return 1;
            }
            seen[off / bpe] = 1;
        }
    free(seen);
    return 0;
}

static int check_roundtrip_offset(uint32_t mode, uint32_t bpe, uint32_t w, uint32_t h, uint32_t ox, uint32_t oy) {
    uint32_t bw, bh, bytes;
    std_block(mode, bpe, &bw, &bh, &bytes);
    uint64_t size = surface_bytes(mode, w + ox, h + oy, bpe);
    CHECK(size == (uint64_t)((w + ox + bw - 1) / bw) * ((h + oy + bh - 1) / bh) * bytes);
    uint8_t *tiled = calloc((size_t)size, 1), *rows = calloc((size_t)w * h * bpe, 1);
    CHECK(tiled && rows);
    uint32_t per_row = (w + ox + bw - 1) / bw;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t gx = x + ox, gy = y + oy;
            uint64_t off = ((uint64_t)(gy / bh) * per_row + gx / bw) * bytes + reference_offset(mode, gx, gy, bpe);
            for (uint32_t b = 0; b < bpe; b++) tiled[off + b] = (uint8_t)(x * 7 + y * 13 + b * 101 + 1);
        }
    untile(tiled, rows, mode, w, h, bpe, ox, oy);
    int bad = 0;
    for (uint32_t y = 0; y < h && !bad; y++)
        for (uint32_t x = 0; x < w && !bad; x++)
            for (uint32_t b = 0; b < bpe; b++)
                if (rows[((uint64_t)y * w + x) * bpe + b] != (uint8_t)(x * 7 + y * 13 + b * 101 + 1)) {
                    fprintf(stderr, "mode %u bpe %u %ux%u+%u,%u: element (%u, %u) byte %u wrong\n", mode, bpe, w, h, ox, oy, x, y, b);
                    bad = 1;
                    break;
                }
    free(tiled);
    free(rows);
    return bad;
}

static int check_roundtrip(uint32_t mode, uint32_t bpe, uint32_t w, uint32_t h) {
    return check_roundtrip_offset(mode, bpe, w, h, 0, 0);
}

/* untile_layer splits the offset into x and y halves (XOR-linear swizzles) and copies rows
 * from a table of x offsets: it must read the same bytes as the element-by-element walk. */
static int check_untile_split(uint32_t mode, uint32_t bpe, uint32_t w, uint32_t h, uint32_t ox, uint32_t oy, uint32_t layer) {
    uint32_t bw, bh, bytes;
    std_block(mode, bpe, &bw, &bh, &bytes);
    uint32_t lx = gcn_layer_xor(mode, bpe, layer);
    uint64_t size = surface_bytes(mode, w + ox, h + oy, bpe);
    uint8_t *tiled = malloc((size_t)size), *got = malloc((size_t)w * h * bpe), *want = malloc((size_t)w * h * bpe);
    CHECK(tiled && got && want);
    uint32_t seed = mode * 977u + bpe * 131u + w + h * 7u + layer;
    for (uint64_t i = 0; i < size; i++) seed = seed * 1103515245u + 12345u, tiled[i] = (uint8_t)(seed >> 16);
    uint64_t per_row = std_blocks_per_row(mode, w + ox, h + oy, bpe);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t gx = x + ox, gy = y + oy;
            uint64_t off = ((((uint64_t)(gy / bh) * per_row + gx / bw) * bytes) + std_offset(mode, gx, gy, bpe)) ^ lx;
            memcpy(want + ((uint64_t)y * w + x) * bpe, tiled + off, bpe);
        }
    untile_layer(tiled, got, mode, w, h, bpe, ox, oy, lx);
    int bad = memcmp(got, want, (size_t)w * h * bpe) != 0;
    if (bad) fprintf(stderr, "untile split: mode %u bpe %u %ux%u+%u,%u layer %u differs\n", mode, bpe, w, h, ox, oy, layer);
    free(tiled);
    free(got);
    free(want);
    return bad;
}

int main(void) {
    for (size_t m = 0; m < sizeof(k_modes) / sizeof(k_modes[0]); m++) {
        CHECK(std_supported(k_modes[m]));
        for (uint32_t bpe = 1; bpe <= 16; bpe *= 2) {
            if (k_modes[m] == 24 && bpe == 16) continue; /* no 128-bit depth */
            CHECK(check_bijection(k_modes[m], bpe) == 0);
            CHECK(check_spans(k_modes[m], bpe) == 0);
            CHECK(check_roundtrip(k_modes[m], bpe, 100, 70) == 0);
            CHECK(check_roundtrip(k_modes[m], bpe, 3, 1) == 0);
            CHECK(check_roundtrip_offset(k_modes[m], bpe, 99, 67, 5, 7) == 0);
        }
    }
    CHECK(std_supported(0) && !std_supported(2) && !std_supported(23));
    {
        static const uint32_t split_modes[] = {1, 5, 9, 24, 27, GCN_SW_1D_THIN, GCN_SW_1D_DISPLAY, GCN_SW_2D_THIN,
                                               GCN_SW_2D_DISPLAY};
        for (size_t m = 0; m < sizeof(split_modes) / sizeof(split_modes[0]); m++)
            for (uint32_t bpe = 1; bpe <= 16; bpe *= 2) {
                uint32_t mode = split_modes[m];
                if (mode == 24 && bpe == 16) continue;
                CHECK(check_untile_split(mode, bpe, 256, 40, 0, 0, 0) == 0);  /* table path */
                CHECK(check_untile_split(mode, bpe, 512, 9, 0, 0, 1) == 0);   /* table path, bank rotation */
                CHECK(check_untile_split(mode, bpe, 99, 67, 5, 7, 0) == 0);   /* span path */
                CHECK(check_untile_split(mode, bpe, 4100, 3, 0, 0, 2) == 0);  /* too wide for the table */
            }
    }

    /* SW_4KB_S, 32 bpp (a 32x32 block): byte bits 2-11 = x0 x1 y0 y1 y2 x2 y3 x3 y4 x4 */
    uint32_t bw, bh, bytes;
    std_block(5, 4, &bw, &bh, &bytes);
    CHECK(bw == 32 && bh == 32 && bytes == 4096);
    CHECK(std_offset(5, 1, 0, 4) == 0x004 && std_offset(5, 2, 0, 4) == 0x008 && std_offset(5, 0, 1, 4) == 0x010);
    CHECK(std_offset(5, 0, 4, 4) == 0x040 && std_offset(5, 4, 0, 4) == 0x080 && std_offset(5, 0, 8, 4) == 0x100);
    CHECK(std_offset(5, 8, 0, 4) == 0x200 && std_offset(5, 0, 16, 4) == 0x400 && std_offset(5, 16, 0, 4) == 0x800);
    CHECK(std_offset(5, 31, 31, 4) == 0xFFC);
    /* SW_4KB_S, 8 bpp (64x64): bits 0-3 = x0-x3, 4-8 = y0-y4, then x4 y5 x5 */
    CHECK(std_offset(5, 15, 0, 1) == 0x00F && std_offset(5, 0, 31, 1) == 0x1F0 && std_offset(5, 16, 0, 1) == 0x200);
    CHECK(std_offset(5, 0, 32, 1) == 0x400 && std_offset(5, 32, 0, 1) == 0x800);
    /* SW_64KB_S: 4x4 blocks of 4 KB, block bits 12-15 = y, x, y, x */
    std_block(9, 4, &bw, &bh, &bytes);
    CHECK(bw == 128 && bh == 128 && bytes == 65536);
    CHECK(std_offset(9, 0, 32, 4) == 0x1000 && std_offset(9, 32, 0, 4) == 0x2000);
    CHECK(std_offset(9, 0, 64, 4) == 0x4000 && std_offset(9, 64, 0, 4) == 0x8000);
    CHECK(std_offset(9, 33, 0, 4) == 0x2004);
    /* SW_256B_S: the low byte of the 4 KB pattern, 8x8 at 32 bpp */
    std_block(1, 4, &bw, &bh, &bytes);
    CHECK(bw == 8 && bh == 8 && bytes == 256 && std_offset(1, 7, 7, 4) == 0xFC);

    /* mips (SW_64KB_S, 256x256 at 32 bpp, 9 levels): the levels that fit
     * in half a block share the tail block first, then 1, then 0 */
    mip_place m0, m1, m2, m8;
    mip_locate(9, 256, 256, 1, 4, 9, 0, &m0);
    mip_locate(9, 256, 256, 1, 4, 9, 1, &m1);
    mip_locate(9, 256, 256, 1, 4, 9, 2, &m2);
    mip_locate(9, 256, 256, 1, 4, 9, 8, &m8);
    CHECK(!m0.tail && !m1.tail && m2.tail && m8.tail);
    CHECK(m1.offset == 65536 && m0.offset == 65536 * 2);
    CHECK(m2.ox == 64 && m2.oy == 0 && m8.ox == 0 && m8.oy == 24);

    /* PS4 (addrlib SI): linear pitch aligned to max(8, 64 / bpe) until the slice holds a
     * multiple of max(64, 256 / bpe) elements; 1D pitch grows by micro tiles until the slice
     * is 256-byte aligned; 2D array slices rotate the bank by banks / 2 - 1 */
    CHECK(gcn_linear_pitch(1920, 1088, 1) == 1920);
    CHECK(gcn_linear_pitch(100, 10, 1) == 128);
    CHECK(gcn_linear_pitch(100, 1, 1) == 256);
    CHECK(gcn_linear_pitch(30, 30, 4) == 32);
    CHECK(gcn_tiles_per_row(8, 32, 1) == 1 && gcn_tiles_per_row(8, 8, 1) == 4 && gcn_tiles_per_row(8, 8, 4) == 1);
    CHECK(surface_bytes(GCN_SW_LINEAR, 100, 10, 1) == 1280);
    CHECK(gcn_layer_xor(GCN_SW_2D_THIN, 4, 1) == 7u << 11 && gcn_layer_xor(GCN_SW_2D_THIN, 16, 1) == 3u << 11);
    CHECK(gcn_layer_xor(GCN_SW_1D_THIN, 4, 1) == 0 && gcn_layer_xor(GCN_SW_2D_THIN, 4, 0) == 0);
    {
        /* level-major array: 64x64 RGBA8 1D, 3 slices, 2 levels: level 0 is 16 KiB a slice,
         * level 1 4 KiB; level 1 slice 2 starts after 3 x 16 KiB and 2 x 4 KiB */
        mip_place l1;
        mip_locate(GCN_SW_1D_THIN, 64, 64, 1, 4, 2, 1, &l1);
        CHECK(l1.offset == 16384);
        CHECK(l1.offset + tex_layer_offset(GCN_SW_1D_THIN, 64, 64, 1, 4, 2, 1, 3, 2, 16384) == 3 * 16384 + 2 * 4096);
        CHECK(tex_layer_offset(GCN_SW_1D_THIN, 64, 64, 1, 4, 1, 0, 3, 2, 16384) == 2 * 16384);
        CHECK(tex_layer_offset(9, 64, 64, 1, 4, 2, 1, 3, 2, 99) == 2 * 99);
    }

    printf("texture tiling: ok\n");
    return 0;
}
