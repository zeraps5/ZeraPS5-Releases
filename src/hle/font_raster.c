/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hle/font_raster.h"

#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "pal/pal.h"
#include "pal/bundled_assets.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/stb/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

struct font_face {
    stbtt_fontinfo info;
    uint8_t *data;
    int ascent, descent, gap; /* font units */
    float units_per_em;
};

font_face *font_face_create(uint8_t *data, size_t size) {
    font_face *f = calloc(1, sizeof(*f));
    int offset = data && size >= 12 ? stbtt_GetFontOffsetForIndex(data, 0) : -1;
    if (!f || offset < 0 || !stbtt_InitFont(&f->info, data, offset)) {
        free(f);
        free(data);
        return NULL;
    }
    f->data = data;
    stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->gap);
    float scale = stbtt_ScaleForMappingEmToPixels(&f->info, 1.0f);
    f->units_per_em = scale > 0.0f ? 1.0f / scale : 1000.0f;
    return f;
}

void font_face_destroy(font_face *f) {
    if (!f) return;
    free(f->data);
    free(f);
}

static bool is_font_file(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot || strlen(dot) != 4) return false;
    char ext[4];
    for (int i = 0; i < 3; i++) ext[i] = (char)tolower((unsigned char)dot[1 + i]);
    ext[3] = 0;
    return !strcmp(ext, "ttf") || !strcmp(ext, "otf") || !strcmp(ext, "ttc") || !strcmp(ext, "otc");
}

/* The alphabetically first font file of user/fonts/ */
static bool pick_user_font(void *user, const char *name, bool is_dir) {
    char *best = user;
    if (!is_dir && is_font_file(name) && strlen(name) < 200 && (!best[0] || strcmp(name, best) < 0)) strcpy(best, name);
    return true;
}

static font_face *load_file(const char *path) {
    uint8_t *data;
    uint64_t size;
    if (pal_file_read_all(path, &data, &size) != PS5C_OK) return NULL;
    font_face *f = font_face_create(data, (size_t)size);
    if (f) LOG_I(HLE, "font: drawing with %s", path);
    else LOG_W(HLE, "font: %s is not a font stb_truetype reads", path);
    return f;
}

const font_face *font_face_default(void) {
    static _Atomic(font_face *) face;
    static atomic_bool tried;
    font_face *f = atomic_load(&face);
    if (f || atomic_load(&tried)) return f;
    char name[200] = {0}, path[256];
    if (pal_dir_list("user/fonts", pick_user_font, name) == PS5C_OK && name[0]) {
        snprintf(path, sizeof(path), "user/fonts/%s", name);
        f = load_file(path);
    }
    if (!f) f = load_file("data/fonts/NotoSans-Regular.ttf");
    if (!f) {
        size_t size = 0;
        const void *bundled = pal_bundled_asset(PS5C_BUNDLED_FONT, &size);
        uint8_t *copy = bundled && size ? malloc(size) : NULL;
        if (copy) {
            memcpy(copy, bundled, size);
            f = font_face_create(copy, size); /* owns copy, also on failure */
            if (f) LOG_I(HLE, "font: drawing with embedded Noto Sans");
        }
    }
    if (!f) LOG_W(HLE, "font: no readable user, data or embedded font: text stays empty");
    font_face *none = NULL;
    if (f && !atomic_compare_exchange_strong(&face, &none, f)) { /* another thread loaded it first */
        font_face_destroy(f);
        f = none;
    }
    atomic_store(&tried, true);
    return f;
}

void font_line_layout(const font_face *f, float sx, float sy, float out[3]) {
    (void)sx;
    if (!f) {
        out[0] = sy * 0.8f, out[1] = sy * 1.2f, out[2] = 0.0f;
        return;
    }
    float k = sy / f->units_per_em;
    out[0] = (float)f->ascent * k;
    out[1] = (float)(f->ascent - f->descent + f->gap) * k;
    out[2] = 0.0f;
}

void font_glyph_metrics(const font_face *f, uint32_t code, float sx, float sy, float out[8]) {
    if (!f) {
        float adv = code == ' ' ? sx * 0.3f : sx * 0.55f;
        float m[8] = {0.0f, 0.0f, 0.0f, sy * 0.8f, adv, -sx * 0.5f, 0.0f, sy};
        memcpy(out, m, sizeof(m));
        return;
    }
    int glyph = stbtt_FindGlyphIndex(&f->info, (int)code);
    int advance = 0, lsb = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphHMetrics(&f->info, glyph, &advance, &lsb);
    if (!stbtt_GetGlyphBox(&f->info, glyph, &x0, &y0, &x1, &y1)) x0 = y0 = x1 = y1 = 0; /* blank (space) */
    float kx = sx / f->units_per_em, ky = sy / f->units_per_em;
    float w = (float)(x1 - x0) * kx, h = (float)(y1 - y0) * ky;
    out[0] = w;
    out[1] = h;
    out[2] = (float)x0 * kx;
    out[3] = (float)y1 * ky;
    out[4] = (float)advance * kx;
    out[5] = -w * 0.5f; /* vertical: centered on the column, from the top of the em */
    out[6] = (float)(f->ascent - y1) * ky;
    out[7] = sy;
}

bool font_draw_glyph(const font_face *f, uint32_t code, float sx, float sy, float x, float y, const font_surface *s) {
    if (!f || !s || !s->pixels || (s->pixel_bytes != 1 && s->pixel_bytes != 4)) return false;
    int glyph = stbtt_FindGlyphIndex(&f->info, (int)code);
    float kx = sx / f->units_per_em, ky = sy / f->units_per_em;
    if (!(kx > 0.0f) || !(ky > 0.0f) || kx > 16.0f || ky > 16.0f) return false;
    float fx = floorf(x), fy = floorf(y);
    int bw = 0, bh = 0, ox = 0, oy = 0;
    unsigned char *bits = stbtt_GetGlyphBitmapSubpixel(&f->info, kx, ky, x - fx, y - fy, glyph, &bw, &bh, &ox, &oy);
    if (!bits) return false;
    int left = (int)fx + ox, top = (int)fy + oy;
    int cx0 = s->clip[0] > 0 ? s->clip[0] : 0, cy0 = s->clip[1] > 0 ? s->clip[1] : 0;
    int cx1 = s->clip[2] < s->width ? s->clip[2] : s->width, cy1 = s->clip[3] < s->height ? s->clip[3] : s->height;
    bool drawn = false;
    for (int r = 0; r < bh; r++) {
        int py = top + r;
        if (py < cy0 || py >= cy1) continue;
        uint8_t *row = s->pixels + (size_t)py * (size_t)s->pitch;
        for (int col = 0; col < bw; col++) {
            int px = left + col;
            uint8_t v = bits[r * bw + col];
            if (px < cx0 || px >= cx1 || !v) continue;
            drawn = true;
            if (s->pixel_bytes == 1) {
                if (row[px] < v) row[px] = v;
            } else {
                uint8_t *p = row + (size_t)px * 4;
                if (p[3] < v) p[0] = p[1] = p[2] = p[3] = v;
            }
        }
    }
    stbtt_FreeBitmap(bits, NULL);
    return drawn;
}
