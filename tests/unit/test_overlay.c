/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Performance panel text (vk_overlay.h): glyphs land where the cells are. */
#include <stdio.h>
#include <stdlib.h>

#include "gpu/vk/vk_overlay.h"

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int lit(const uint8_t *px, uint32_t w, uint32_t x, uint32_t y) { return px[4 * (y * w + x) + 1] == 255; }

int main(void) {
    uint32_t w, h;
    vk_overlay_size(4, 1, &w, &h);
    CHECK(w == 4 * VK_OVERLAY_CELL_W + 2 * VK_OVERLAY_PAD && h == VK_OVERLAY_CELL_H + 2 * VK_OVERLAY_PAD);
    uint8_t *px = (uint8_t *)malloc((size_t)w * h * 4);
    const char *line = "1-T ";
    vk_overlay_render(px, w, h, &line, 1, true);
    uint32_t P = VK_OVERLAY_PAD, S = VK_OVERLAY_SCALE, C = VK_OVERLAY_CELL_W;
    /* '1': its top row has only the middle column (0x04) */
    CHECK(!lit(px, w, P + 0 * S, P) && lit(px, w, P + 2 * S, P) && !lit(px, w, P + 4 * S, P));
    /* '-': only row 3, all five columns */
    CHECK(!lit(px, w, P + C, P + 2 * S) && lit(px, w, P + C, P + 3 * S) && lit(px, w, P + C + 4 * S, P + 3 * S));
    /* lowercase draws as uppercase; 't' -> 'T' top row full */
    CHECK(lit(px, w, P + 2 * C, P) && lit(px, w, P + 2 * C + 4 * S, P));
    /* the background is opaque and dark, in BGRA order */
    CHECK(px[4 * (h - 1) * w + 0] == 24 && px[4 * (h - 1) * w + 2] == 16 && px[4 * (h - 1) * w + 3] == 255);
    CHECK(vk_overlay_glyph('#') == NULL);
    free(px);
    /* The compact font and a clipped small window must not write past their
     * image. Both pixel orders and extra/long lines are exercised. */
    const char *many[] = {"1-T@|? TEXT PAST THE RIGHT EDGE", "lowercase", "CLIPPED LINE"};
    for (uint32_t scale = 2; scale <= 4; scale++) {
        vk_overlay_size_scaled(3, 2, scale, &w, &h);
        size_t bytes = (size_t)w * h * 4;
        uint8_t *guard = malloc(bytes + 32);
        CHECK(guard != NULL);
        memset(guard, 0xA5, bytes + 32);
        px = guard + 16;
        vk_overlay_render_scaled(px, w, h, many, 3, false, scale);
        CHECK(lit(px, w, VK_OVERLAY_PAD + scale, VK_OVERLAY_PAD));
        CHECK(px[4 * ((size_t)(h - 1) * w) + 0] == 16 && px[4 * ((size_t)(h - 1) * w) + 2] == 24);
        for (unsigned i = 0; i < 16; i++) CHECK(guard[i] == 0xA5 && guard[16 + bytes + i] == 0xA5);
        free(guard);
    }
    printf("overlay: ok\n");
    return 0;
}
