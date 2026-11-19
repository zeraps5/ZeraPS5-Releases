/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceFont / libSceFontFt: the console's font engine. Libraries, renderers,
 * fonts and glyphs are guest objects we hand out so the game's font setup
 * succeeds (Astro Bot asserts on a null font handle). Every font measures and
 * draws with one host face (font_raster.h) at its pixel scale. */
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/font_raster.h"
#include "hle/hle.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

#define OBJECT_SIZE 0x200
#define POOL_SIZE (4ull << 20)
#define SCALE_AT 0x100 /* where an object keeps its pixel scale (two floats) */

static struct {
    pal_mutex *lock;
    guest_addr pool, next;
} g_font;

/* A zeroed guest object (never freed: fonts are few and live long) */
static guest_addr new_object(hle_call *c) {
    guest_addr at = 0;
    pal_mutex_lock(g_font.lock);
    if (!g_font.pool && gmem_map(c->mem, 0, POOL_SIZE, GPROT_RW, 0, "font objects", &g_font.pool) == PS5C_OK)
        g_font.next = g_font.pool;
    if (g_font.pool && g_font.next + OBJECT_SIZE <= g_font.pool + POOL_SIZE) {
        at = g_font.next;
        g_font.next += OBJECT_SIZE;
    }
    pal_mutex_unlock(g_font.lock);
    if (at) {
        float scale[2] = {16.0f, 16.0f};
        gmem_write(c->mem, at + SCALE_AT, scale, sizeof(scale));
    }
    return at;
}

/* Creates an object and stores it through argument `out` */
static void create_into(hle_call *c, uint32_t out) {
    guest_addr p = hle_arg_ptr(c, out), obj = p ? new_object(c) : 0;
    if (obj) gmem_write_u64(c->mem, p, obj);
    hle_ret_s32(c, obj ? 0 : (p ? (int32_t)0x80460001 : (int32_t)0x80460002));
}

static void font_ok(hle_call *c) { hle_ret_s32(c, 0); }

/* sceFontMemoryInit(SceFontMemory *mem, void *address, u32 size, const SceFontMemoryInterface
 * *iface, void *mspace, destroy callback, void *destroy object): the game's text code
 * allocates through mem (+0x10 the mspace, +0x18 the interface whose first entry is malloc) */
static void font_memory_init(hle_call *c) {
    guest_addr mem = hle_arg_ptr(c, 0);
    if (!mem) {
        hle_ret_s32(c, (int32_t)0x80460002);
        return;
    }
    /* kind 0x0F00 marks it ours (lib_font_text.c checks it); the last word repeats the mspace */
    uint64_t m[8] = {(uint64_t)hle_arg_u32(c, 2) << 32 | 0x0F00, hle_arg(c, 1), hle_arg(c, 4), hle_arg(c, 3),
                     hle_arg(c, 5), hle_arg(c, 6), 0, hle_arg(c, 4)};
    gmem_write(c->mem, mem, m, sizeof(m));
    hle_ret_s32(c, 0);
}

/* const SceFontLibrarySelection *sceFontSelectLibraryFt(int), sceFontSelectRendererFt(int) */
static void font_select(hle_call *c) { hle_ret_u64(c, new_object(c)); }

/* sceFontCreateLibraryWithEdition(memory, selection, edition, SceFontLibrary *out),
 * sceFontCreateRendererWithEdition(memory, selection, edition, SceFontRenderer *out) */
static void font_create_arg3(hle_call *c) { create_into(c, 3); }
/* sceFontOpenFontSet(library, set type, open mode, detail, SceFontHandle *out),
 * sceFontOpenFontMemory(library, address, size, detail, SceFontHandle *out) */
static void font_create_arg4(hle_call *c) { create_into(c, 4); }
/* sceFontOpenFontInstance(font, setup font, SceFontHandle *out),
 * sceFontGenerateCharGlyph(font, code, params, SceFontGlyph *out) */
static void font_create_arg2(hle_call *c) { create_into(c, 2); }
static void font_create_arg3b(hle_call *c) { create_into(c, 3); }

static void get_scale(hle_call *c, guest_addr font, float *w, float *h) {
    float s[2] = {16.0f, 16.0f};
    if (font) gmem_read(c->mem, font + SCALE_AT, s, sizeof(s));
    *w = s[0] > 0.0f ? s[0] : 16.0f;
    *h = s[1] > 0.0f ? s[1] : 16.0f;
}

/* sceFontSetScalePixel(font, float w, float h), sceFontSetupRenderScalePixel(font, w, h) */
static void font_set_scale(hle_call *c) {
    guest_addr font = hle_arg_ptr(c, 0);
    float s[2];
    memcpy(&s[0], &c->a->xmm[0][0], 4);
    memcpy(&s[1], &c->a->xmm[1][0], 4);
    if (font) gmem_write(c->mem, font + SCALE_AT, s, sizeof(s));
    hle_ret_s32(c, 0);
}

/* sceFontGetHorizontalLayout(font, {float baseline y, line height, effect height} *out) */
static void font_get_horizontal_layout(hle_call *c) {
    float w, h, l[3];
    get_scale(c, hle_arg_ptr(c, 0), &w, &h);
    font_line_layout(font_face_default(), w, h, l);
    guest_addr out = hle_arg_ptr(c, 1);
    if (out) gmem_write(c->mem, out, l, sizeof(l));
    hle_ret_s32(c, 0);
}

/* sceFontGetVerticalLayout(font, {float baseline x, column width, effect width} *out) */
static void font_get_vertical_layout(hle_call *c) {
    float w, h;
    get_scale(c, hle_arg_ptr(c, 0), &w, &h);
    float l[3] = {w * 0.5f, w, 0.0f};
    guest_addr out = hle_arg_ptr(c, 1);
    if (out) gmem_write(c->mem, out, l, sizeof(l));
    hle_ret_s32(c, 0);
}

/* SceFontGlyphMetrics: width, height, horizontal {bearing x, bearing y, advance},
 * vertical {bearing x, bearing y, advance} */
static void write_metrics(hle_call *c, guest_addr font, uint32_t code, guest_addr out) {
    if (!out) return;
    float w, h, m[8];
    get_scale(c, font, &w, &h);
    font_glyph_metrics(font_face_default(), code, w, h, m);
    gmem_write(c->mem, out, m, sizeof(m));
}

void font_object_metrics(hle_call *c, guest_addr font, uint32_t code, float out[8]) {
    float w, h;
    get_scale(c, font, &w, &h);
    font_glyph_metrics(font_face_default(), code, w, h, out);
}

/* sceFontGetRenderCharGlyphMetrics and sceFontGetCharGlyphMetrics(font, code, metrics *out) */
static void font_get_char_metrics(hle_call *c) {
    write_metrics(c, hle_arg_ptr(c, 0), hle_arg_u32(c, 1), hle_arg_ptr(c, 2));
    hle_ret_s32(c, 0);
}

/* sceFontRenderCharGlyphImageHorizontal(font, code, surface, float x, float y,
 * metrics *out, SceFontRenderOutput *result): the glyph's origin (pen on the baseline)
 * at x, y of the surface (sceFontRenderSurfaceInit's layout below) */
static void font_render_char(hle_call *c) {
    guest_addr font = hle_arg_ptr(c, 0), surface = hle_arg_ptr(c, 2);
    uint32_t code = hle_arg_u32(c, 1);
    write_metrics(c, font, code, hle_arg_ptr(c, 3));
    uint8_t b[40];
    if (surface && gmem_read(c->mem, surface, b, sizeof(b))) {
        uint64_t buf;
        int8_t px;
        font_surface s;
        memcpy(&buf, b, 8);
        memcpy(&s.pitch, b + 8, 4);
        memcpy(&px, b + 12, 1);
        memcpy(&s.width, b + 16, 4);
        memcpy(&s.height, b + 20, 4);
        memcpy(s.clip, b + 24, sizeof(s.clip));
        s.pixel_bytes = px;
        s.pixels = NULL;
        if (buf && s.pitch > 0 && s.height > 0 && s.height <= 16384 && (px == 1 || px == 4) &&
            (int64_t)s.width * px <= s.pitch)
            s.pixels = (uint8_t *)hle_mem(c, buf, (uint64_t)s.pitch * (uint64_t)s.height);
        float x, y, w, h;
        memcpy(&x, &c->a->xmm[0][0], 4);
        memcpy(&y, &c->a->xmm[1][0], 4);
        get_scale(c, font, &w, &h);
        if (s.pixels) font_draw_glyph(font_face_default(), code, w, h, x, y, &s);
    }
    hle_ret_s32(c, 0);
}

/* sceFontRenderSurfaceInit(surface, buffer, width in bytes, pixel size, width, height):
 * {buffer, s32 width bytes, s8 pixel size, pad, s32 width, height, scissor {x0, y0, x1, y1}} */
static void font_render_surface_init(hle_call *c) {
    guest_addr s = hle_arg_ptr(c, 0);
    if (s) {
        uint8_t b[40];
        memset(b, 0, sizeof(b));
        uint64_t buf = hle_arg(c, 1);
        int32_t wb = hle_arg_s32(c, 2), w = hle_arg_s32(c, 4), h = hle_arg_s32(c, 5);
        int8_t px = (int8_t)hle_arg_s32(c, 3);
        memcpy(b, &buf, 8);
        memcpy(b + 8, &wb, 4);
        memcpy(b + 12, &px, 1);
        memcpy(b + 16, &w, 4);
        memcpy(b + 20, &h, 4);
        int32_t sc[4] = {0, 0, w, h};
        memcpy(b + 24, sc, sizeof(sc));
        gmem_write(c->mem, s, b, sizeof(b));
    }
    hle_ret_s32(c, 0);
}

#define N "libSceFont"

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(N, "sceFontMemoryInit", font_memory_init),
    HLE_FUNC_ENTRY("libSceFontFt", "sceFontSelectLibraryFt", font_select),
    HLE_FUNC_ENTRY("libSceFontFt", "sceFontSelectRendererFt", font_select),
    HLE_FUNC_ENTRY(N, "sceFontCreateLibraryWithEdition", font_create_arg3),
    HLE_FUNC_ENTRY(N, "sceFontCreateRendererWithEdition", font_create_arg3),
    HLE_FUNC_ENTRY(N, "sceFontSupportSystemFonts", font_ok),
    HLE_FUNC_ENTRY(N, "sceFontSupportExternalFonts", font_ok),
    HLE_FUNC_ENTRY(N, "sceFontAttachDeviceCacheBuffer", font_ok),
    HLE_FUNC_ENTRY(N, "sceFontOpenFontSet", font_create_arg4),
    HLE_FUNC_ENTRY(N, "sceFontOpenFontMemory", font_create_arg4),
    HLE_FUNC_ENTRY(N, "sceFontOpenFontInstance", font_create_arg2),
    HLE_FUNC_ENTRY(N, "sceFontCloseFont", font_ok),
    HLE_FUNC_ENTRY(N, "sceFontBindRenderer", font_ok),
    HLE_FUNC_ENTRY(N, "sceFontUnbindRenderer", font_ok),
    HLE_FUNC_ENTRY(N, "sceFontRebindRenderer", font_ok),
    HLE_FUNC_ENTRY(N, "sceFontDestroyRenderer", font_ok),
    HLE_FUNC_ENTRY(N, "sceFontDestroyLibrary", font_ok),
    HLE_FUNC_ENTRY(N, "sceFontMemoryTerm", font_ok),
    HLE_FUNC_ENTRY(N, "sceFontRenderSurfaceSetScissor", font_ok),
    HLE_FUNC_ENTRY(N, "sceFontSetScalePixel", font_set_scale),
    HLE_FUNC_ENTRY(N, "sceFontSetupRenderScalePixel", font_set_scale),
    HLE_FUNC_ENTRY(N, "sceFontSetEffectSlant", font_ok),
    HLE_FUNC_ENTRY(N, "sceFontSetEffectWeight", font_ok),
    HLE_FUNC_ENTRY(N, "sceFontSetupRenderEffectSlant", font_ok),
    HLE_FUNC_ENTRY(N, "sceFontSetupRenderEffectWeight", font_ok),
    HLE_FUNC_ENTRY(N, "sceFontGetHorizontalLayout", font_get_horizontal_layout),
    HLE_FUNC_ENTRY(N, "sceFontGetVerticalLayout", font_get_vertical_layout),
    HLE_FUNC_ENTRY(N, "sceFontGetRenderCharGlyphMetrics", font_get_char_metrics),
    HLE_FUNC_ENTRY(N, "sceFontGetCharGlyphMetrics", font_get_char_metrics),
    HLE_FUNC_ENTRY(N, "sceFontRenderCharGlyphImageHorizontal", font_render_char),
    HLE_FUNC_ENTRY(N, "sceFontRenderSurfaceInit", font_render_surface_init),
    HLE_FUNC_ENTRY(N, "sceFontGenerateCharGlyph", font_create_arg3b),
    HLE_FUNC_ENTRY(N, "sceFontDeleteGlyph", font_ok),
    HLE_FUNC_ENTRY(N, "sceFontGlyphDefineAttribute", font_ok),
};

void hle_register_font(void) {
    if (!g_font.lock && pal_mutex_create(&g_font.lock) != PS5C_OK) {
        LOG_E(HLE, "cannot create the font lock");
        return;
    }
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
    hle_register_font_text();
}
