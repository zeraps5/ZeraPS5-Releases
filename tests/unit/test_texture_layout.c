/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include "gpu/vk/vk_texture_layout.h"
#include "hle/avplayer_picture.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(void) {
    /* The real AvPlayer producer and GPU linear reader must agree for BOTH
     * NV12 planes at 1080p. A compact 1920-byte pitch corrupts every row
     * after the first when read as a GFX10 linear surface. */
    {
        uint32_t w=1920,h=1080,plane_h=1088,pitch=avp_picture_pitch(w);
        uint64_t compact=(uint64_t)w*h*3/2, padded=(uint64_t)pitch*plane_h*3/2;
        CHECK(pitch==vk_linear_pitch(w,1) && pitch==vk_linear_pitch(w/2,2));
        uint8_t *src=malloc((size_t)compact),*dst=malloc((size_t)padded+32),*readback=malloc((size_t)compact);
        CHECK(src && dst && readback);
        for(uint64_t i=0;i<compact;i++)src[i]=(uint8_t)(i*37+(i/w)*11);
        memset(dst,0xa5,(size_t)padded+32);
        avp_picture_pack(dst,src,w,h,pitch,plane_h);
        vk_linear_unpack(dst,readback,w,h,1);
        vk_linear_unpack(dst+(uint64_t)pitch*plane_h,readback+(uint64_t)w*h,w/2,h/2,2);
        CHECK(!memcmp(src,readback,(size_t)compact));
        for(uint32_t i=0;i<32;i++)CHECK(dst[padded+i]==0xa5);
        for(uint32_t y=h;y<plane_h;y++)for(uint32_t x=0;x<pitch;x++)CHECK(dst[(uint64_t)y*pitch+x]==0xa5);
        free(readback);free(dst);free(src);
    }
    CHECK(vk_linear_pitch(342, 8) == 2816); /* 1368px BC1 logo, not 3072 */
    CHECK(vk_linear_pitch(33, 16) == 768); /* BC3: not 1024 */
    CHECK(vk_linear_pitch(65, 1) == 256); /* R8: not 128 */
    CHECK(vk_linear_pitch(65, 2) == 256);
    CHECK(vk_linear_pitch(65, 4) == 512); /* existing RGBA8 remains identical */
    CHECK(vk_linear_mip_offset(64, 64, 8, 1, 0) == 0);
    CHECK(vk_linear_mip_offset(64, 64, 8, 3, 2) == 0);
    CHECK(vk_linear_mip_offset(64, 64, 8, 3, 1) == 4096);
    CHECK(vk_linear_mip_offset(64, 64, 8, 3, 0) == 12288);
    CHECK(vk_linear_mip_offset(33, 17, 16, 2, 0) == 512 * 9); /* odd BC dimensions */
    for (uint32_t bpe = 1; bpe <= 16; bpe *= 2) {
        uint32_t w = bpe == 8 ? 342 : 65, h = 3;
        uint64_t pitch = vk_linear_pitch(w, bpe), row = (uint64_t)w * bpe;
        uint8_t *src = malloc((size_t)pitch * h), *dst = malloc((size_t)row * h);
        CHECK(src && dst);
        memset(src, 0xED, (size_t)pitch * h);
        for (uint32_t y = 0; y < h; y++) memset(src + pitch * y, 17 + y, (size_t)row);
        vk_linear_unpack(src, dst, w, h, bpe);
        for (uint32_t y = 0; y < h; y++)
            for (uint64_t x = 0; x < row; x++) CHECK(dst[y * row + x] == 17 + y);
        free(src);
        free(dst);
    }
    puts("texture layout: ok");
    return 0;
}
