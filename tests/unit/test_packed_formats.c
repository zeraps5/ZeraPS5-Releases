/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/vk/vk_packed_formats.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)

static int texel(uint32_t fmt, uint32_t v, uint8_t x, uint8_t y, uint8_t z, uint8_t w) {
    uint8_t buf[8] = {(uint8_t)v, (uint8_t)(v >> 8)};
    if (!vk_packed_expand(fmt, buf, 1)) return 0;
    return buf[0] == x && buf[1] == y && buf[2] == z && buf[3] == w;
}

int main(void) {
    CHECK(vk_packed_bytes(132) == 0 && vk_packed_bytes(133) == 2 && vk_packed_bytes(136) == 2);
    CHECK(vk_packed_bytes(137) == 1 && vk_packed_bytes(138) == 0 && vk_packed_bytes(56) == 0);
    /* X in the low bits: PSP 5650/5551/4444 keep R there */
    CHECK(texel(133, 0x001f, 255, 0, 0, 255));
    CHECK(texel(133, 0x07e0, 0, 255, 0, 255));
    CHECK(texel(133, 0xf800, 0, 0, 255, 255));
    CHECK(texel(134, 0x801f, 255, 0, 0, 255));
    CHECK(texel(134, 0x7c00, 0, 0, 255, 0));
    CHECK(texel(134, 0x0010, 132, 0, 0, 0)); /* 16/31 rounds to 132 */
    CHECK(texel(135, 0x0001, 255, 0, 0, 0));
    CHECK(texel(135, 0xf800, 0, 0, 0, 255));
    CHECK(texel(136, 0x4321, 17, 34, 51, 68));
    CHECK(texel(137, 0x00a5, 85, 170, 0, 255));
    CHECK(!vk_packed_expand(56, (uint8_t[4]){0}, 1));
    /* in place: every texel of a row grows without overwriting the ones still to read */
    uint8_t row[4 * 4] = {0x1f, 0x00, 0xe0, 0x07, 0x00, 0xf8, 0xff, 0xff};
    CHECK(vk_packed_expand(133, row, 4));
    const uint8_t want[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    CHECK(!memcmp(row, want, sizeof(want)));
    uint8_t nib[8] = {0x10, 0x0f};
    CHECK(vk_packed_expand(137, nib, 2));
    CHECK(nib[0] == 0 && nib[1] == 17 && nib[4] == 255 && nib[5] == 0);
    puts("packed_formats: ok");
    return 0;
}
