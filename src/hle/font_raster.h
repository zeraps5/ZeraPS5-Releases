/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host fonts for libSceFont: a TrueType/OpenType face (stb_truetype) measured and
 * drawn at a pixel scale. The face is the first font in user/fonts/ (a PS5's own
 * system fonts, copied from the console) or the bundled data/fonts/NotoSans-Regular.ttf. */
#ifndef PS5C_FONT_RASTER_H
#define PS5C_FONT_RASTER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct font_face font_face;

/* The face fonts draw with, loaded once; NULL when there is none. */
const font_face *font_face_default(void);
/* A face over a font file's bytes (kept by the face, freed with it). */
font_face *font_face_create(uint8_t *data, size_t size);
void font_face_destroy(font_face *f);

/* Scales are pixels per em, horizontal and vertical. */
/* {baseline offset (ascent), line advance, effect height} */
void font_line_layout(const font_face *f, float sx, float sy, float out[3]);
/* SceFontGlyphMetrics: width, height, horizontal {bearing x, bearing y, advance},
 * vertical {bearing x, bearing y, advance}. Bearing y is up from the baseline. */
void font_glyph_metrics(const font_face *f, uint32_t code, float sx, float sy, float out[8]);

typedef struct font_surface {
    uint8_t *pixels;
    int32_t pitch;       /* bytes per row */
    int32_t pixel_bytes; /* 1: coverage, 4: 32-bit white with coverage in every channel */
    int32_t width, height;
    int32_t clip[4];     /* x0, y0, x1, y1 (exclusive) */
} font_surface;

/* Draws a glyph with its origin (pen on the baseline) at x, y; coverage keeps the
 * larger of what is there and the glyph. Returns false when nothing was drawn. */
bool font_draw_glyph(const font_face *f, uint32_t code, float sx, float sy, float x, float y, const font_surface *s);
#endif
