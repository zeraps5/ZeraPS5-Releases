/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceFont's host face: the bundled font (run from the build directory, where
 * data/fonts/ is copied) measures and draws glyphs. */
#include <stdio.h>
#include <string.h>
#include "hle/font_raster.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static unsigned count(const uint8_t *p, int pitch, int x0, int y0, int x1, int y1) {
    unsigned n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) n += p[y * pitch + x] > 128;
    return n;
}

int main(void) {
    const font_face *f = font_face_default();
    CHECK(f);
    CHECK(font_face_default() == f); /* loaded once */

    float l[3], m[8], space[8], wide[8];
    font_line_layout(f, 32.0f, 32.0f, l);
    CHECK(l[0] > 25.0f && l[0] < 40.0f && l[1] > l[0]); /* ascent about an em, line taller */
    font_glyph_metrics(f, 'A', 32.0f, 32.0f, m);
    CHECK(m[0] > 15.0f && m[0] < 25.0f);  /* width */
    CHECK(m[3] > 20.0f && m[3] < 25.0f);  /* cap height above the baseline */
    CHECK(m[4] > m[0] && m[4] < 32.0f);   /* advance */
    font_glyph_metrics(f, ' ', 32.0f, 32.0f, space);
    CHECK(space[0] == 0.0f && space[4] > 4.0f); /* blank, still advances */
    font_glyph_metrics(f, 'A', 64.0f, 32.0f, wide);
    CHECK(wide[0] > m[0] * 1.9f && wide[1] == m[1]); /* horizontal scale only */

    static uint8_t gray[64 * 64];
    font_surface s = {gray, 64, 1, 64, 64, {0, 0, 64, 64}};
    CHECK(font_draw_glyph(f, 'A', 32.0f, 32.0f, 8.0f, 40.0f, &s));
    CHECK(count(gray, 64, 0, 0, 64, 40 - 24) == 0); /* nothing above the cap height */
    CHECK(count(gray, 64, 0, 41, 64, 64) == 0);     /* nor below the baseline */
    CHECK(count(gray, 64, 8, 16, 32, 40) > 60);     /* the A itself */
    CHECK(!font_draw_glyph(f, ' ', 32.0f, 32.0f, 8.0f, 40.0f, &s));

    static uint8_t rgba[64 * 64 * 4];
    font_surface c = {rgba, 64 * 4, 4, 64, 64, {0, 0, 20, 64}}; /* clipped at x 20 */
    CHECK(font_draw_glyph(f, 'H', 32.0f, 32.0f, 8.0f, 40.0f, &c));
    unsigned left = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const uint8_t *p = rgba + (y * 64 + x) * 4;
            CHECK(x < 20 || !p[3]);
            CHECK(p[0] == p[3] && p[1] == p[3] && p[2] == p[3]);
            left += p[3] > 128;
        }
    CHECK(left > 30);
    puts("font_raster: ok");
    return 0;
}
