/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Guest 3D standard tiles must not be unpacked as independent 2D slices. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gpu/gpu_regs.h"
#include "gpu/renderer.h"
#include "gpu/vk/vk_texture_3d.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

/* Independent GFX10 32-bpp S3 address equation (AMD addrlib).
 * Low bits: byte,byte,X0,Y0,Z0,Y1,X1,Z1,Y2,X2,Z2,Y3.
 * 64 KB extends it with X3,Z3,Y4,X4. */
static uint64_t reference(uint32_t mode, uint32_t w, uint32_t h,
                          uint32_t x, uint32_t y, uint32_t z) {
    uint32_t bw = mode == 9 ? 32 : 8, bh = mode == 9 ? 32 : 16, bd = mode == 9 ? 16 : 8;
    uint64_t block = ((uint64_t)(z / bd) * ((h + bh - 1) / bh) + y / bh) * ((w + bw - 1) / bw) + x / bw;
    uint32_t off = ((x & 1) << 2) | ((y & 1) << 3) | ((z & 1) << 4) |
                   ((y & 2) << 4) | ((x & 2) << 5) | ((z & 2) << 6) |
                   ((y & 4) << 6) | ((x & 4) << 7) | ((z & 4) << 8) | ((y & 8) << 8);
    if (mode == 9) off |= ((x & 8) << 9) | ((z & 8) << 10) | ((y & 16) << 10) | ((x & 16) << 11);
    return block * (mode == 9 ? 65536 : 4096) + off;
}

static int cpu_test(void) {
    vk_3d_layout l;
    CHECK(!vk_std_3d_layout(8, 4, &l));
    CHECK(!vk_std_3d_layout(9, 0, &l));
    CHECK(!vk_std_3d_layout(9, 3, &l));
    CHECK(!vk_std_3d_layout(9, 32, &l));
    CHECK(vk_std_3d_layout(9, 4, &l));
    CHECK(l.bw == 32 && l.bh == 32 && l.bd == 16);
    CHECK(vk_std_3d_bytes(&l, 64, 64, 64) == 1024 * 1024);
    CHECK(vk_std_3d_offset(&l, 64, 64, 1, 2, 3) == 180);
    CHECK(vk_std_3d_offset(&l, 64, 64, 32, 32, 16) == 7 * 65536);
    /* Each block must cover every element once, including byte sizes not used
     * by Sonic. Prevent overlap/out-of-range reads at 4K/64K block boundaries. */
    for (uint32_t mode = 5; mode <= 9; mode += 4) for (uint32_t bpe = 1; bpe <= 16; bpe *= 2) {
        CHECK(vk_std_3d_layout(mode, bpe, &l));
        uint8_t *seen = calloc(l.bytes / bpe, 1); CHECK(seen);
        for (uint32_t z = 0; z < l.bd; z++) for (uint32_t y = 0; y < l.bh; y++) for (uint32_t x = 0; x < l.bw; x++) {
            uint32_t off = vk_std_3d_block_offset(&l, x, y, z);
            CHECK(off % bpe == 0 && off + bpe <= l.bytes);
            CHECK(!seen[off / bpe]); seen[off / bpe] = 1;
        }
        for (uint32_t i = 0; i < l.bytes / bpe; i++) CHECK(seen[i]);
        free(seen);
    }
    for (uint32_t mode = 5; mode <= 9; mode += 4) {
        uint32_t w = 33, h = 35, d = 67;
        CHECK(vk_std_3d_layout(mode, 4, &l));
        uint64_t size = vk_std_3d_bytes(&l, w, h, d);
        uint8_t *src = calloc((size_t)size, 1), *dst = malloc(w*h*d*4); CHECK(src && dst);
        for (uint32_t z = 0; z < d; z++) for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            uint64_t off = reference(mode, w, h, x, y, z);
            CHECK(off + 4 <= size);
            src[off] = (uint8_t)x; src[off+1] = (uint8_t)y; src[off+2] = (uint8_t)z; src[off+3] = (uint8_t)(x+y+z);
        }
        vk_std_3d_unpack(src, dst, &l, w, h, d);
        for (uint32_t z = 0; z < d; z++) for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            uint64_t off = ((uint64_t)z*h*w + (uint64_t)y*w + x)*4;
            CHECK(dst[off] == x && dst[off+1] == y && dst[off+2] == z && dst[off+3] == x+y+z);
        }
        free(src); free(dst);
    }
    return 0;
}

static int gpu_test(void) {
    static const uint32_t code[] = {
        0x7e000208, 0x7e020209, 0x7e04020a, /* v[0:2]=s[8:10] */
        0xf0000f10, 0x00000400, /* image_load v[4:7], v[0:2], s[0:7], dim=3D */
        0xbf8c3f70,
        0xe0780000, 0x80030400, /* buffer_store_dwordx4 v[4:7], s[12:15] */
        0xbf810000
    };
    CHECK(pal_init() == PS5C_OK);
    gmem m; gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 320 * GUEST_PAGE_SIZE, GPROT_RW, 0, "3D tile fixture", &a) == PS5C_OK);
    CHECK(gmem_write(&m, a, code, sizeof(code)));
    uint32_t *out = gmem_host(&m, a + 0x1000, 16);
    uint8_t *src = gmem_host(&m, a + 0x10000, 4 * 1024 * 1024); CHECK(out && src);
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 3D texture regression", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m); CHECK(r);
    gpu g; CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(a >> 8);
    g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)(a >> 40);
    g.regs[mmCOMPUTE_PGM_RSRC2] = 16u << 1;
    g.regs[mmCOMPUTE_NUM_THREAD_X] = g.regs[mmCOMPUTE_NUM_THREAD_Y] = g.regs[mmCOMPUTE_NUM_THREAD_Z] = 1;
    uint32_t *u = g.regs + mmCOMPUTE_USER_DATA_0;
    guest_addr image = a + 0x10000;
    u[0] = (uint32_t)(image >> 8);
    u[1] = (uint32_t)(image >> 40) | (60u << 20); /* RGBA8 uint */
    u[3] = (10u << 28) | (9u << 20) | 0xfac;
    u[12] = (uint32_t)(a + 0x1000); u[13] = (uint32_t)((a + 0x1000) >> 32);
    u[14] = 16; u[15] = 0x16204;
    static const uint32_t points[][3] = {{0,0,0},{1,2,3},{8,9,15},{31,31,15},{32,32,16},{63,63,63}};
    for (uint32_t mode = 5; mode <= 9; mode += 4) {
        uint32_t w = mode == 9 ? 64 : 33, h = mode == 9 ? 64 : 35, d = mode == 9 ? 64 : 67;
        memset(src, 0xCD, 4 * 1024 * 1024);
        for (uint32_t z = 0; z < d; z++) for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            uint64_t off = reference(mode, w, h, x, y, z);
            src[off] = (uint8_t)x; src[off+1] = (uint8_t)y; src[off+2] = (uint8_t)z; src[off+3] = (uint8_t)(x+y+z);
        }
        u[1] = (u[1] & 0x3fffffff) | (((w-1) & 3) << 30);
        u[2] = ((w-1) >> 2) | ((h-1) << 14); u[3] = (10u << 28) | (mode << 20) | 0xfac; u[4] = d-1;
        for (uint32_t p = 0; p < PS5C_ARRAY_COUNT(points); p++) {
            uint32_t x = points[p][0] % w, y = points[p][1] % h, z = points[p][2] % d;
            u[8] = x; u[9] = y; u[10] = z;
            memset(out, 0, 16); renderer_submit_begin(r); renderer_dispatch(r, &g, 1, 1, 1);
            renderer_sync_mem(r, a + 0x1000, 16);
            uint32_t expected[] = {x,y,z,x+y+z};
            for (uint32_t c = 0; c < 4; c++) {
                if (out[c] != expected[c]) fprintf(stderr,"mode %u (%u,%u,%u) component %u: %u expected %u\n",mode,x,y,z,c,out[c],expected[c]);
                CHECK(out[c] == expected[c]);
            }
        }
        /* The fog pass uses non-sequential address VGPRs and normalized
         * sampling, rather than image_load. Check both uint and linear UNORM. */
        for (uint32_t normalized = 0; normalized < 2; normalized++) {
            uint32_t format = normalized ? 56 : 60;
            u[1] = (u[1] & ~(0x1ffu << 20)) | (format << 20);
            memset(u + 8, 0, 4 * sizeof(uint32_t));
            if (normalized) u[10] = (1u << 20) | (1u << 22);
            for (uint32_t p = 0; p < PS5C_ARRAY_COUNT(points); p++) {
                uint32_t x = points[p][0] % w, y = points[p][1] % h, z = points[p][2] % d;
                float coord[] = {(x + 0.5f)/w, (y + 0.5f)/h, (z + 0.5f)/d};
                uint32_t bits[3]; memcpy(bits, coord, sizeof(bits));
                uint32_t sample[] = {
                    0x7e1e02ff, bits[0], /* v15=x */
                    0x7e1c02ff, bits[1], /* v14=y */
                    0x7e2a02ff, bits[2], /* v21=z */
                    0xf09c0f12, 0x0040080f, 0x0000150e, /* sample_lz v[8:11], [v15,v14,v21] */
                    0xbf8c3f70, 0xe0780000, 0x80030800, 0xbf810000
                };
                CHECK(gmem_write(&m, a + 0x200, sample, sizeof(sample)));
                g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)((a + 0x200) >> 8);
                memset(out, 0, 16); renderer_submit_begin(r); renderer_dispatch(r, &g, 1, 1, 1);
                renderer_sync_mem(r, a + 0x1000, 16);
                uint32_t expected[] = {x,y,z,x+y+z};
                for (uint32_t c = 0; c < 4; c++) {
                    if (normalized) {
                        float actual; memcpy(&actual, out+c, 4);
                        if (fabsf(actual - expected[c]/255.0f) > 0.000001f)
                            fprintf(stderr, "sample mode %u (%u,%u,%u) component %u: %g expected %g\n", mode,x,y,z,c,actual,expected[c]/255.0f);
                        CHECK(fabsf(actual - expected[c]/255.0f) <= 0.000001f);
                    } else CHECK(out[c] == expected[c]);
                }
            }
        }
        g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(a >> 8);
        u[1] = (u[1] & ~(0x1ffu << 20)) | (60u << 20);
    }
    renderer_destroy(r); free(g.regs); pal_mutex_destroy(g.lock);
    pal_window_destroy(window); gmem_free(&m); pal_shutdown();
    puts("GPU 3D tiles: padded volumes, both block sizes, all components passed");
    return 0;
}

int main(void) {
    CHECK(cpu_test() == 0);
    if (getenv("PS5C_TEST_GPU")) CHECK(gpu_test() == 0);
    puts("3D texture layout: ok"); return 0;
}
