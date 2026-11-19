/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceFont text: text sources, strings of characters and their writing (render
 * steps), as PSPHD lays out its menus. Ported to C and to guest memory from
 * AnyPS5's libSceFont Text.cpp and WritingLine.cpp (boykopovar/AnyPS5, commit 4af3a1e,
 * GPL-2.0-or-later, "Copyright 2024 shadPS4 Emulator Project"); strings are left to
 * right only: every character is its own cluster at bidi level 0.
 *
 * The game owns the memory: strings and characters come from the allocator of its
 * SceFontMemory, a writing lives in the game's SceFontWriting (0x100 bytes), and
 * the text source's parser is a guest function we call for each character. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/font_raster.h"
#include "hle/hle.h"

void hle_register_table(const hle_export *table, size_t count);

#define FONT_OK 0
#define FONT_ERROR_INVALID_PARAMETER ((int32_t)0x80460002)
#define FONT_ERROR_INVALID_MEMORY ((int32_t)0x80460003)
#define FONT_ERROR_INVALID_TEXT_SOURCE ((int32_t)0x80460008)
#define FONT_ERROR_INVALID_STRING ((int32_t)0x80460009)
#define FONT_ERROR_INVALID_WRITING ((int32_t)0x8046000A)
#define FONT_ERROR_ALLOCATION_FAILED ((int32_t)0x80460010)

#define FONT_MEMORY_MAGIC 0x0F00 /* lib_font.c's sceFontMemoryInit */
#define TEXT_SOURCE_MAGIC 0x0F04
#define STRING_MAGIC 0x0F05
#define WRITING_MAGIC 0x0F06
#define FORM_HORIZONTAL 0x10
#define FORM_HORIZONTAL_RTL 0x11
#define FORM_HORIZONTAL_LTR 0x12
#define MAX_CHARACTERS 4096
#define FLAG_FORMAT (1ull << 42)

#pragma pack(push, 1)
typedef struct font_memory { /* SceFontMemory */
    uint16_t kind, attr;
    uint32_t size;
    uint64_t base, mspace, iface, on_destroy, destroy_object, ctx1, ctx2;
} font_memory;

typedef struct text_source { /* SceFontTextSource; the game's parser reads end, current and font */
    uint64_t system0; /* magic, writing form << 32 */
    uint64_t start, end, current, parser, object, font;
    uint64_t saved[5]; /* start, end, parser, object, font: for a rewind */
} text_source;

typedef struct text_char { /* SceFontTextCharacter */
    uint64_t prev, next, order, font, shape;
    uint32_t code;
    uint8_t reserved_2c[4];
    uint8_t cluster_span, cluster_index, cluster_kind;
    int8_t synthetic;
    uint8_t reserved_34[4];
    uint64_t flags;
    uint8_t reserved_40[136];
} text_char;

typedef struct font_string { /* ours, in the game's memory */
    uint16_t magic;
    uint8_t flags, reserved;
    uint32_t form;
    uint64_t memory, source, font;
    uint32_t terminate_code, pad;
    uint64_t terminate_order;
    uint32_t count, render_count;
    uint64_t characters;
} font_string;

typedef struct writing_step { /* SceFontWritingStep */
    float x, y, advance_x, advance_y;
    uint64_t font;
    uint32_t profile; /* character count : 8, invisible glyph : 1 */
    uint32_t glyph;
    float positioning[2];
    float metrics[8]; /* SceFontGlyphMetrics */
} writing_step;

typedef struct letter_step { /* SceFontWritingLetterStep */
    float x, y, advance_x, advance_y;
    uint32_t texts_count, texts_index, glyphs_count, glyphs_index;
    uint8_t base_components_count, base_components_index, base_focus_count, base_focus_index;
    uint8_t opposite_direction, character_text_count, base_text_count, mark_text_count;
    uint32_t marks_text_count, marks_text_number, reserved[3];
    int32_t text_letter_offset;
} letter_step;

typedef struct writing { /* ours, in the game's SceFontWriting */
    uint16_t magic, reserved;
    uint32_t form;
    uint64_t string, font;
    uint32_t next, current;
    float pen_x, pen_y;
    uint8_t has_metrics, has_step, has_character, mask_format, has_raw, pad[3];
    writing_step step;
    float metrics[6]; /* SceFontWritingMetrics: advance x, y, extent top, bottom, left, right */
    float raw[8];
    letter_step letter;
} writing;
#pragma pack(pop)

_Static_assert(sizeof(font_memory) == 0x40, "SceFontMemory");
_Static_assert(sizeof(text_source) == 0x60, "SceFontTextSource");
_Static_assert(sizeof(text_char) == 0xC8, "SceFontTextCharacter");
_Static_assert(sizeof(writing_step) == 0x48, "SceFontWritingStep");
_Static_assert(offsetof(writing, step) % 8 == 0, "step alignment");
_Static_assert(sizeof(writing) <= 0x100, "SceFontWriting");

static bool is_white_space(uint32_t code) {
    switch (code) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20: case 0x85: case 0xA0: case 0x1680:
    case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
        return true;
    default:
        return code >= 0x2000 && code <= 0x200A;
    }
}

static bool is_format(uint32_t code) {
    switch (code) {
    case 0xAD: case 0x61C: case 0x200B: case 0x200C: case 0x200D: case 0x200E: case 0x200F: case 0x2060:
    case 0x2061: case 0x2062: case 0x2063: case 0x2064: case 0x2066: case 0x2067: case 0x2068: case 0x2069:
    case 0xFE0E: case 0xFE0F: case 0xFEFF:
        return true;
    default:
        return false;
    }
}

static font_string *get_string(hle_call *c, guest_addr s) {
    font_string *d = s ? hle_mem(c, s, sizeof(font_string)) : NULL;
    return d && d->magic == STRING_MAGIC ? d : NULL;
}

static writing *get_writing(hle_call *c, guest_addr w) {
    writing *d = w ? hle_mem(c, w, sizeof(writing)) : NULL;
    return d && d->magic == WRITING_MAGIC ? d : NULL;
}

static text_char *get_char(hle_call *c, guest_addr ch) { return ch ? hle_mem(c, ch, sizeof(text_char)) : NULL; }

static text_source *get_source(hle_call *c, guest_addr s) {
    text_source *d = s ? hle_mem(c, s, sizeof(text_source)) : NULL;
    return d && (uint16_t)d->system0 == TEXT_SOURCE_MAGIC ? d : NULL;
}

/* The game's allocator: iface->alloc(mspace, size), iface->dealloc(mspace, p) */
static guest_addr mem_alloc(hle_call *c, const font_memory *m, uint32_t size) {
    uint64_t fn = 0, rax = 0;
    if (!gmem_read_u64(c->mem, m->iface, &fn) || !fn || !runtime_call_nested(c->rt, fn, m->mspace, size, 0, &rax))
        return 0;
    return rax;
}

static void mem_free(hle_call *c, const font_memory *m, guest_addr p) {
    uint64_t fn = 0, rax;
    if (p && gmem_read_u64(c->mem, m->iface + 8, &fn) && fn) runtime_call_nested(c->rt, fn, m->mspace, p, 0, &rax);
}

/* ---- Text sources ---- */

/* sceFontTextSourceInit(src, const void *text, u32 bytes (0: until the parser ends it),
 * int (*parser)(src, void **order, result *), void *object) */
static void text_source_init(hle_call *c) {
    guest_addr s = hle_arg_ptr(c, 0), text = hle_arg_ptr(c, 1), parser = hle_arg_ptr(c, 3);
    uint32_t bytes = hle_arg_u32(c, 2);
    text_source *d = s ? hle_mem(c, s, sizeof(text_source)) : NULL;
    if (!d) {
        hle_ret_s32(c, FONT_ERROR_INVALID_PARAMETER);
        return;
    }
    memset(d, 0, sizeof(*d));
    if (!parser) {
        hle_ret_s32(c, FONT_ERROR_INVALID_PARAMETER);
        return;
    }
    guest_addr end = bytes ? text + bytes : 0;
    d->system0 = ((uint64_t)FORM_HORIZONTAL << 32) | TEXT_SOURCE_MAGIC;
    d->start = d->current = d->saved[0] = text;
    d->end = d->saved[1] = end;
    d->parser = d->saved[2] = parser;
    d->object = d->saved[3] = hle_arg(c, 4);
    hle_ret_s32(c, FONT_OK);
}

static void text_source_rewind(hle_call *c) {
    text_source *d = get_source(c, hle_arg_ptr(c, 0));
    if (d) {
        d->start = d->current = d->saved[0];
        d->end = d->saved[1];
        d->parser = d->saved[2];
        d->object = d->saved[3];
        d->font = d->saved[4];
    }
    hle_ret_s32(c, d ? FONT_OK : FONT_ERROR_INVALID_TEXT_SOURCE);
}

static void text_source_set_default_font(hle_call *c) {
    text_source *d = get_source(c, hle_arg_ptr(c, 0));
    if (d) d->font = d->saved[4] = hle_arg(c, 1);
    hle_ret_s32(c, d ? FONT_OK : FONT_ERROR_INVALID_TEXT_SOURCE);
}

static void text_source_set_writing_form(hle_call *c) {
    text_source *d = get_source(c, hle_arg_ptr(c, 0));
    int32_t form = hle_arg_s32(c, 1);
    if (!d) {
        hle_ret_s32(c, FONT_ERROR_INVALID_TEXT_SOURCE);
        return;
    }
    if (form < FORM_HORIZONTAL || form > FORM_HORIZONTAL_LTR) {
        hle_ret_s32(c, FONT_ERROR_INVALID_PARAMETER);
        return;
    }
    d->system0 = (d->system0 & 0xFFFFFFFFull) | ((uint64_t)(uint32_t)form << 32);
    hle_ret_s32(c, FONT_OK);
}

/* ---- Strings ---- */

/* sceFontCreateString(const SceFontMemory *, src, const detail *, SceFontString *out):
 * the parser's results until it ends the text (0) or fails (-1); 1 is a character
 * {font, code}. */
static void create_string(hle_call *c) {
    guest_addr mem = hle_arg_ptr(c, 0), src = hle_arg_ptr(c, 1), out = hle_arg_ptr(c, 3);
    if (out) gmem_write_u64(c->mem, out, 0);
    font_memory m;
    text_source *s = get_source(c, src);
    if (!mem || !src || !out || !gmem_read(c->mem, mem, &m, sizeof(m))) {
        hle_ret_s32(c, FONT_ERROR_INVALID_PARAMETER);
        return;
    }
    if (!s || !s->parser) {
        hle_ret_s32(c, FONT_ERROR_INVALID_TEXT_SOURCE);
        return;
    }
    if (m.kind != FONT_MEMORY_MAGIC || !m.iface) {
        hle_ret_s32(c, FONT_ERROR_INVALID_MEMORY);
        return;
    }
    guest_addr detail = hle_arg_ptr(c, 2), default_font = s->font, parser = s->parser;
    uint32_t form = (uint32_t)(s->system0 >> 32);
    if (!default_font && detail) gmem_read_u64(c->mem, detail + 8, &default_font);
    /* scratch for the parser's outputs: void *order, then the 32-byte result */
    guest_addr scratch = mem_alloc(c, &m, 0x30), at = 0;
    text_char *chars = NULL; /* host copy until the count is known */
    uint32_t n = 0, cap = 0, terminate_code = 0;
    uint64_t terminate_order = 0;
    int32_t state = scratch ? FONT_ERROR_INVALID_TEXT_SOURCE : FONT_ERROR_ALLOCATION_FAILED;
    while (scratch && n < MAX_CHARACTERS) {
        uint64_t ret = 0, order = 0, result[4] = {0};
        gmem_write(c->mem, scratch, result, sizeof(result) + 8);
        if (!runtime_call_nested(c->rt, parser, src, scratch, scratch + 8, &ret)) break;
        gmem_read_u64(c->mem, scratch, &order);
        gmem_read(c->mem, scratch + 8, result, sizeof(result));
        int32_t r = (int32_t)ret;
        if (r == 1) {
            uint64_t font = result[0] ? result[0] : default_font;
            if (!font) break;
            if (n == cap) {
                text_char *grown = realloc(chars, (size_t)(cap = cap ? cap * 2 : 64) * sizeof(text_char));
                if (!grown) {
                    state = FONT_ERROR_ALLOCATION_FAILED;
                    break;
                }
                chars = grown;
            }
            text_char *ch = &chars[n++];
            memset(ch, 0, sizeof(*ch));
            ch->order = order;
            ch->font = font;
            ch->code = (uint32_t)result[1];
            ch->cluster_span = 1;
            ch->flags = (uint64_t)(is_white_space(ch->code) ? 0x0E : 0) << 8;
            if (is_format(ch->code)) ch->flags |= FLAG_FORMAT;
            continue;
        }
        if (r == 0) {
            terminate_order = order;
            terminate_code = (uint32_t)result[1];
            state = FONT_OK;
        } else if (r == -1 && (int32_t)(uint32_t)result[1] < 0) {
            state = (int32_t)(uint32_t)result[1];
        }
        break;
    }
    if (scratch) mem_free(c, &m, scratch);
    guest_addr str = state == FONT_OK ? mem_alloc(c, &m, sizeof(font_string)) : 0;
    if (str && n) at = mem_alloc(c, &m, n * (uint32_t)sizeof(text_char));
    font_string *d = str ? hle_mem(c, str, sizeof(font_string)) : NULL;
    text_char *dst = at ? hle_mem(c, at, (uint64_t)n * sizeof(text_char)) : NULL;
    if (state == FONT_OK && (!d || (n && !dst))) state = FONT_ERROR_ALLOCATION_FAILED;
    if (state != FONT_OK) {
        mem_free(c, &m, at);
        mem_free(c, &m, str);
        free(chars);
        hle_ret_s32(c, state);
        return;
    }
    if (form == FORM_HORIZONTAL) form = FORM_HORIZONTAL_LTR; /* right-to-left scripts are not laid out */
    for (uint32_t i = 0; i < n; i++) {
        chars[i].prev = i ? at + (uint64_t)(i - 1) * sizeof(text_char) : 0;
        chars[i].next = i + 1 < n ? at + (uint64_t)(i + 1) * sizeof(text_char) : 0;
    }
    if (n) memcpy(dst, chars, (size_t)n * sizeof(text_char));
    free(chars);
    memset(d, 0, sizeof(*d));
    d->magic = STRING_MAGIC;
    d->form = form;
    d->memory = mem;
    d->source = src;
    d->font = default_font;
    d->terminate_code = terminate_code;
    d->terminate_order = terminate_order;
    d->count = d->render_count = n;
    d->characters = n ? at : 0;
    gmem_write_u64(c->mem, out, str);
    hle_ret_s32(c, FONT_OK);
}

/* sceFontDestroyString(SceFontString *) */
static void destroy_string(hle_call *c) {
    guest_addr p = hle_arg_ptr(c, 0), str = 0;
    if (!p || !gmem_read_u64(c->mem, p, &str)) {
        hle_ret_s32(c, FONT_ERROR_INVALID_PARAMETER);
        return;
    }
    font_string *d = get_string(c, str);
    font_memory m;
    gmem_write_u64(c->mem, p, 0);
    if (!d || !gmem_read(c->mem, d->memory, &m, sizeof(m))) {
        hle_ret_s32(c, FONT_ERROR_INVALID_STRING);
        return;
    }
    guest_addr chars = d->characters;
    d->magic = 0;
    mem_free(c, &m, chars);
    mem_free(c, &m, str);
    hle_ret_s32(c, FONT_OK);
}

static void string_terminate_code(hle_call *c) {
    font_string *d = get_string(c, hle_arg_ptr(c, 0));
    hle_ret_u32(c, d ? d->terminate_code : 0);
}

static void string_terminate_order(hle_call *c) {
    font_string *d = get_string(c, hle_arg_ptr(c, 0));
    hle_ret_u64(c, d ? d->terminate_order : 0);
}

static void string_writing_form(hle_call *c) {
    font_string *d = get_string(c, hle_arg_ptr(c, 0));
    hle_ret_s32(c, d ? (int32_t)d->form : 0);
}

/* SceFontTextCharacter *sceFontStringRefersTextCharacters(string, u32 *count) */
static void string_text_characters(hle_call *c) {
    font_string *d = get_string(c, hle_arg_ptr(c, 0));
    guest_addr count = hle_arg_ptr(c, 1);
    uint32_t n = d ? d->count : 0;
    if (count) gmem_write(c->mem, count, &n, 4);
    hle_ret_u64(c, n ? d->characters : 0);
}

/* sceFontStringRefersRenderCharacters(string, start (NULL: the first), last (NULL: to the
 * end), u32 *count): in rendering order, which left to right is the text order */
static void string_render_characters(hle_call *c) {
    font_string *d = get_string(c, hle_arg_ptr(c, 0));
    guest_addr start = hle_arg_ptr(c, 1), last = hle_arg_ptr(c, 2), count = hle_arg_ptr(c, 3);
    uint32_t n = 0;
    guest_addr first = 0;
    if (d && count && d->render_count) {
        guest_addr begin = d->characters, end = begin + (uint64_t)d->render_count * sizeof(text_char);
        if (!start) start = begin;
        if (start >= begin && start < end && (!last || (last >= start && last < end)) &&
            (start - begin) % sizeof(text_char) == 0 && (!last || (last - begin) % sizeof(text_char) == 0)) {
            first = start;
            n = (uint32_t)(((last ? last : end - sizeof(text_char)) - start) / sizeof(text_char)) + 1;
        }
    }
    if (count) gmem_write(c->mem, count, &n, 4);
    hle_ret_u64(c, first);
}

/* ---- Characters ---- */

static void character_bidi_level(hle_call *c) {
    text_char *ch = get_char(c, hle_arg_ptr(c, 0));
    guest_addr out = hle_arg_ptr(c, 1);
    int32_t level = ch ? (int32_t)((ch->flags >> 24) & 0xFF) : 0;
    if (out) gmem_write(c->mem, out, &level, 4);
    hle_ret_s32(c, ch && out ? FONT_OK : FONT_ERROR_INVALID_PARAMETER);
}

static void character_syllable_state(hle_call *c) {
    guest_addr out = hle_arg_ptr(c, 1);
    int32_t state = 0;
    if (out) gmem_write(c->mem, out, &state, 4);
    hle_ret_s32(c, hle_arg_ptr(c, 0) && out ? FONT_OK : FONT_ERROR_INVALID_PARAMETER);
}

/* sceFontCharacterGetTextFontCode(ch, SceFontHandle *font, u32 *code) */
static void character_font_code(hle_call *c) {
    text_char *ch = get_char(c, hle_arg_ptr(c, 0));
    guest_addr font = hle_arg_ptr(c, 1), code = hle_arg_ptr(c, 2);
    uint64_t f = ch ? ch->font : 0;
    uint32_t k = ch ? ch->code : 0;
    if (font) gmem_write_u64(c->mem, font, f);
    if (code) gmem_write(c->mem, code, &k, 4);
    hle_ret_s32(c, ch && font && code ? FONT_OK : FONT_ERROR_INVALID_PARAMETER);
}

static void character_text_order(hle_call *c) {
    text_char *ch = get_char(c, hle_arg_ptr(c, 0));
    guest_addr out = hle_arg_ptr(c, 1);
    if (out) gmem_write_u64(c->mem, out, ch ? ch->order : 0);
    hle_ret_s32(c, ch && out ? FONT_OK : FONT_ERROR_INVALID_PARAMETER);
}

/* The code when the character is white space (or a format character), else 0 */
static void character_looks_white_space(hle_call *c) {
    text_char *ch = get_char(c, hle_arg_ptr(c, 0));
    hle_ret_u32(c, ch && ((ch->flags >> 8) & 0xFF) == 0x0E ? ch->code : 0);
}

static void character_looks_format(hle_call *c) {
    text_char *ch = get_char(c, hle_arg_ptr(c, 0));
    hle_ret_u32(c, ch && (ch->flags & FLAG_FORMAT) ? ch->code : 0);
}

static void character_next(hle_call *c) {
    text_char *ch = get_char(c, hle_arg_ptr(c, 0));
    hle_ret_u64(c, ch ? ch->next : 0);
}

static void character_back(hle_call *c) {
    text_char *ch = get_char(c, hle_arg_ptr(c, 0));
    hle_ret_u64(c, ch ? ch->prev : 0);
}

/* ---- Writing: one render step per character, the pen moving right ---- */

static bool populate_step(hle_call *c, writing *w, uint32_t index) {
    font_string *d = get_string(c, w->string);
    text_char *ch = d && index < d->count ? get_char(c, d->characters + (uint64_t)index * sizeof(text_char)) : NULL;
    if (!ch) return false;
    uint64_t font = ch->font ? ch->font : w->font;
    memset(&w->step, 0, sizeof(w->step));
    w->step.x = w->pen_x;
    w->step.y = w->pen_y;
    w->step.font = font;
    w->step.profile = 1 | ((ch->flags & FLAG_FORMAT) && w->mask_format ? 0x100u : 0u);
    w->step.glyph = ch->code;
    memset(w->raw, 0, sizeof(w->raw));
    w->has_raw = 0;
    if (font && ch->code) {
        font_object_metrics(c, font, ch->code, w->raw);
        w->has_raw = 1;
        w->step.advance_x = w->raw[4];
        memcpy(w->step.metrics, w->raw, sizeof(w->raw));
        w->step.positioning[0] = w->step.x;
        w->step.positioning[1] = w->step.y;
    }
    w->pen_x += w->step.advance_x;
    w->pen_y += w->step.advance_y;
    return true;
}

static void accumulate_metrics(writing *w) {
    const float *g = w->has_raw ? w->raw : w->step.metrics;
    float left = w->step.x + g[2], right = left + g[0], top = w->step.y - g[3], bottom = top + g[1];
    w->metrics[0] = w->step.x + w->step.advance_x;
    w->metrics[1] = w->step.y + w->step.advance_y;
    if (!w->has_metrics) {
        w->metrics[2] = top, w->metrics[3] = bottom, w->metrics[4] = left, w->metrics[5] = right;
        w->has_metrics = 1;
        return;
    }
    if (top < w->metrics[2]) w->metrics[2] = top;
    if (bottom > w->metrics[3]) w->metrics[3] = bottom;
    if (left < w->metrics[4]) w->metrics[4] = left;
    if (right > w->metrics[5]) w->metrics[5] = right;
}

/* sceFontWritingInit(SceFontWriting *, string, const SceFontTextCharacter *first) */
static void writing_init(hle_call *c) {
    guest_addr wa = hle_arg_ptr(c, 0), str = hle_arg_ptr(c, 1), ch = hle_arg_ptr(c, 2);
    writing *w = wa ? hle_mem(c, wa, 0x100) : NULL;
    if (!w || !str || !ch) {
        hle_ret_s32(c, FONT_ERROR_INVALID_PARAMETER);
        return;
    }
    font_string *d = get_string(c, str);
    if (!d) {
        hle_ret_s32(c, FONT_ERROR_INVALID_STRING);
        return;
    }
    memset(w, 0, 0x100);
    guest_addr begin = d->characters;
    if (!d->count || ch < begin || ch >= begin + (uint64_t)d->count * sizeof(text_char) || (ch - begin) % sizeof(text_char)) {
        hle_ret_s32(c, FONT_ERROR_INVALID_PARAMETER);
        return;
    }
    w->magic = WRITING_MAGIC;
    w->form = d->form;
    w->string = str;
    w->font = d->font;
    w->mask_format = 1;
    w->next = w->current = (uint32_t)((ch - begin) / sizeof(text_char));
    populate_step(c, w, w->next);
    w->pen_x = w->pen_y = 0.0f;
    hle_ret_s32(c, FONT_OK);
}

/* sceFontWritingGetRenderMetrics(writing, SceFontWritingMetrics *out) */
static void writing_render_metrics(hle_call *c) {
    writing *w = get_writing(c, hle_arg_ptr(c, 0));
    guest_addr out = hle_arg_ptr(c, 1);
    float m[6] = {0};
    if (w) memcpy(m, w->metrics, sizeof(m));
    if (out) gmem_write(c->mem, out, m, sizeof(m));
    hle_ret_s32(c, !out ? FONT_ERROR_INVALID_PARAMETER : w ? FONT_OK : FONT_ERROR_INVALID_WRITING);
}

/* const SceFontWritingStep *sceFontWritingRefersRenderStep(writing): the next one, NULL at the end */
static void writing_render_step(hle_call *c) {
    guest_addr wa = hle_arg_ptr(c, 0);
    writing *w = get_writing(c, wa);
    if (!w || !populate_step(c, w, w->next)) {
        hle_ret_u64(c, 0);
        return;
    }
    w->current = w->next++;
    w->has_step = w->has_character = 1;
    accumulate_metrics(w);
    hle_ret_u64(c, wa + offsetof(writing, step));
}

/* SceFontTextCharacter *sceFontWritingRefersRenderStepCharacter(writing,
 * const SceFontWritingLetterStep **out): the character of the current step, once */
static void writing_render_step_character(hle_call *c) {
    guest_addr wa = hle_arg_ptr(c, 0), out = hle_arg_ptr(c, 1);
    if (out) gmem_write_u64(c->mem, out, 0);
    writing *w = get_writing(c, wa);
    font_string *d = w && w->has_step && w->has_character ? get_string(c, w->string) : NULL;
    if (!d || w->current >= d->count) {
        hle_ret_u64(c, 0);
        return;
    }
    w->has_character = 0;
    memset(&w->letter, 0, sizeof(w->letter));
    w->letter.x = w->step.x;
    w->letter.y = w->step.y;
    w->letter.advance_x = w->step.advance_x;
    w->letter.advance_y = w->step.advance_y;
    w->letter.texts_count = w->letter.glyphs_count = 1;
    w->letter.texts_index = w->letter.glyphs_index = w->current;
    w->letter.character_text_count = (uint8_t)((w->step.profile & 0xFF) ? (w->step.profile & 0xFF) : 1);
    if (out) gmem_write_u64(c->mem, out, wa + offsetof(writing, letter));
    hle_ret_u64(c, d->characters + (uint64_t)w->current * sizeof(text_char));
}

static void writing_set_mask_invisible(hle_call *c) {
    writing *w = get_writing(c, hle_arg_ptr(c, 0));
    int32_t mask = hle_arg_s32(c, 1);
    if (!w) {
        hle_ret_s32(c, FONT_ERROR_INVALID_WRITING);
        return;
    }
    if (mask != 0 && mask != 1) {
        hle_ret_s32(c, FONT_ERROR_INVALID_PARAMETER);
        return;
    }
    w->mask_format = mask == 0;
    hle_ret_s32(c, FONT_OK);
}

/* ---- Writing lines: runs the game measured (orders), placed along a line by bidi level ---- */

#define LINE_MAGIC 0x0F0A
#define ATTRIBUTE_ID 0xFA
#define SPACE_BEFORE (1ull << 30)
#define SPACE_AFTER (1ull << 31)
#define LEVEL_COUNT 0x7F

#pragma pack(push, 1)
typedef struct writing_order {
    uint64_t orderer, prev, next, reserved, attribute;
    float metrics[6]; /* SceFontWritingMetrics */
} writing_order;

typedef struct line_step { /* SceFontWritingLineStep */
    float x, y, advance_x, advance_y, spacing_progress, pad;
    uint64_t orderer;
    float adjust[2];
    float metrics[6];
} line_step;

typedef struct writing_line {
    uint16_t magic, reserved;
    int32_t form;
    uint64_t memory;
    float metrics[6];
    line_step step;
    float advance_x, advance_y, ordering_space, stepped_space;
    uint32_t last_level, reserved_level;
    uint64_t first, last, head, tail, cursor, reserved_cursor;
    uint64_t levels[LEVEL_COUNT][2]; /* first, last */
} writing_line;
#pragma pack(pop)

_Static_assert(sizeof(writing_order) == 64, "writing order");
_Static_assert(sizeof(line_step) == 0x40 && offsetof(line_step, orderer) == 0x18, "SceFontWritingLineStep");
_Static_assert(sizeof(writing_line) == 2208 && offsetof(writing_line, step) == 40 && offsetof(writing_line, levels) == 176,
               "writing line");

enum { M_ADVANCE_X, M_ADVANCE_Y, M_TOP, M_BOTTOM, M_LEFT, M_RIGHT };

static writing_order *get_order(hle_call *c, guest_addr o) { return o ? hle_mem(c, o, sizeof(writing_order)) : NULL; }

/* With m, also the line's memory, which must still be a valid SceFontMemory */
static writing_line *get_line(hle_call *c, guest_addr l, font_memory *m) {
    writing_line *d = l ? hle_mem(c, l, sizeof(writing_line)) : NULL;
    if (!d || d->magic != LINE_MAGIC) return NULL;
    if (m && (!gmem_read(c->mem, d->memory, m, sizeof(*m)) || m->kind != FONT_MEMORY_MAGIC || !m->iface)) return NULL;
    return d;
}

static bool line_rtl(const writing_line *l) { return l->form == FORM_HORIZONTAL_RTL; }

static bool is_spaced(const writing_order *before, const writing_order *after) {
    return (before->attribute & SPACE_AFTER) || (after->attribute & SPACE_BEFORE);
}

static float head_space(const writing_line *l, const writing_order *o) {
    return line_rtl(l) ? o->metrics[M_ADVANCE_X] - o->metrics[M_LEFT] : o->metrics[M_LEFT];
}

static float tail_space(const writing_line *l, const writing_order *o) {
    return line_rtl(l) ? o->metrics[M_RIGHT] : o->metrics[M_ADVANCE_X] - o->metrics[M_RIGHT];
}

static void reset_orders(hle_call *c, writing_line *l, const font_memory *m) {
    for (guest_addr o = l->head; o;) {
        writing_order *d = get_order(c, o);
        guest_addr next = d ? d->next : 0;
        mem_free(c, m, o);
        o = next;
    }
    memset(l->levels, 0, sizeof(l->levels));
    l->first = l->last = l->head = l->tail = l->cursor = 0;
    l->advance_x = l->advance_y = l->ordering_space = 0.0f;
}

static void insert_after(hle_call *c, guest_addr anchor, guest_addr o) {
    writing_order *a = get_order(c, anchor), *d = get_order(c, o), *n;
    d->prev = anchor;
    d->next = a->next;
    if ((n = get_order(c, a->next))) n->prev = o;
    a->next = o;
}

static void insert_before(hle_call *c, guest_addr anchor, guest_addr o) {
    writing_order *a = get_order(c, anchor), *d = get_order(c, o), *p;
    d->next = anchor;
    d->prev = a->prev;
    if ((p = get_order(c, a->prev))) p->next = o;
    a->prev = o;
}

/* Places an order after the last one, by level: even levels run with the line, odd
 * ones against it, and closing a deeper level goes around what it held */
static void link_order(hle_call *c, writing_line *l, guest_addr o, uint32_t level) {
    uint64_t *slot = l->levels[level];
    writing_order *d = get_order(c, o);
    if (!l->last) {
        slot[0] = slot[1] = o;
        return;
    }
    if (level >= l->last_level) {
        if (l->last_level % 2 == 0) insert_after(c, l->last, o);
        else insert_before(c, l->last, o);
        if (!slot[1] || d->prev == slot[1]) slot[1] = o;
        if (!slot[0] || d->next == slot[0]) slot[0] = o;
        return;
    }
    guest_addr first = l->levels[l->last_level][0], last = l->levels[l->last_level][1];
    l->levels[l->last_level][0] = l->levels[l->last_level][1] = 0;
    for (uint32_t closing = l->last_level - 1; closing > level; closing--) {
        uint64_t *closed = l->levels[closing];
        if (closing % 2 == 0) {
            if (closed[0]) first = closed[0];
        } else if (closed[1]) {
            last = closed[1];
        }
        closed[0] = closed[1] = 0;
    }
    if (level % 2 == 0) {
        if (!slot[0]) slot[0] = first;
        insert_after(c, last, o);
        slot[1] = o;
    } else {
        if (!slot[1]) slot[1] = last;
        insert_before(c, first, o);
        slot[0] = o;
    }
}

static float ordering_space_delta(hle_call *c, const writing_line *l, const writing_order *o) {
    const writing_order *before = get_order(c, o->prev), *after = get_order(c, o->next);
    bool split = before && after && is_spaced(before, after);
    float sign = split ? -1.0f : 1.0f, delta = 0.0f;
    if (before) {
        bool spaced = is_spaced(before, o);
        if (spaced) delta = head_space(l, o);
        if (spaced != split) delta += sign * tail_space(l, before);
    }
    if (after) {
        bool spaced = is_spaced(o, after);
        if (spaced) delta += tail_space(l, o);
        if (spaced != split) delta += sign * head_space(l, after);
    }
    return delta;
}

/* The step just rendered widens the line's metrics; the pen moves past it */
static void extend_render_metrics(writing_line *l) {
    const float *run = l->step.metrics;
    float *total = l->metrics;
    float x = l->step.x + l->step.adjust[0], y = l->step.y + l->step.adjust[1];
    l->step.x += run[M_ADVANCE_X];
    l->step.y += run[M_ADVANCE_Y];
    total[M_ADVANCE_X] = x + run[M_ADVANCE_X];
    total[M_ADVANCE_Y] = y + run[M_ADVANCE_Y];
    if (run[M_LEFT] != run[M_RIGHT]) {
        float left = x + run[M_LEFT], right = x + run[M_RIGHT];
        if (total[M_LEFT] == 0.0f && total[M_RIGHT] == 0.0f) {
            total[M_LEFT] = left, total[M_RIGHT] = right;
        } else {
            if (total[M_LEFT] > left) total[M_LEFT] = left;
            if (right > total[M_RIGHT]) total[M_RIGHT] = right;
        }
    }
    if (run[M_TOP] != run[M_BOTTOM]) {
        float top = y + run[M_TOP], bottom = y + run[M_BOTTOM];
        if (total[M_TOP] == 0.0f && total[M_BOTTOM] == 0.0f) {
            total[M_TOP] = top, total[M_BOTTOM] = bottom;
        } else {
            if (total[M_TOP] > top) total[M_TOP] = top;
            if (bottom > total[M_BOTTOM]) total[M_BOTTOM] = bottom;
        }
    }
}

/* sceFontCreateWritingLine(const SceFontMemory *, form (right or left to right), detail (NULL), line *out) */
static void create_writing_line(hle_call *c) {
    guest_addr mem = hle_arg_ptr(c, 0), out = hle_arg_ptr(c, 3);
    int32_t form = hle_arg_s32(c, 1);
    font_memory m;
    if (!mem || !gmem_read(c->mem, mem, &m, sizeof(m))) {
        hle_ret_s32(c, FONT_ERROR_INVALID_PARAMETER);
        return;
    }
    if (m.kind != FONT_MEMORY_MAGIC || !m.iface) {
        hle_ret_s32(c, FONT_ERROR_INVALID_MEMORY);
        return;
    }
    if (!out || hle_arg_ptr(c, 2) || (form != FORM_HORIZONTAL_RTL && form != FORM_HORIZONTAL_LTR)) {
        hle_ret_s32(c, FONT_ERROR_INVALID_PARAMETER);
        return;
    }
    guest_addr at = mem_alloc(c, &m, sizeof(writing_line));
    writing_line *l = at ? hle_mem(c, at, sizeof(writing_line)) : NULL;
    if (!l) {
        mem_free(c, &m, at);
        hle_ret_s32(c, FONT_ERROR_ALLOCATION_FAILED);
        return;
    }
    memset(l, 0, sizeof(*l));
    l->magic = LINE_MAGIC;
    l->form = form;
    l->memory = mem;
    gmem_write_u64(c->mem, out, at);
    hle_ret_s32(c, FONT_OK);
}

/* sceFontDestroyWritingLine(line *) */
static void destroy_writing_line(hle_call *c) {
    guest_addr p = hle_arg_ptr(c, 0), at = 0;
    if (!p || !gmem_read_u64(c->mem, p, &at) || !at) {
        hle_ret_s32(c, FONT_ERROR_INVALID_PARAMETER);
        return;
    }
    font_memory m;
    writing_line *l = get_line(c, at, &m);
    if (!l) {
        hle_ret_s32(c, FONT_ERROR_INVALID_WRITING);
        return;
    }
    reset_orders(c, l, &m);
    l->magic = 0;
    mem_free(c, &m, at);
    gmem_write_u64(c->mem, p, 0);
    hle_ret_s32(c, FONT_OK);
}

static void writing_line_clear(hle_call *c) {
    guest_addr at = hle_arg_ptr(c, 0);
    font_memory m;
    writing_line *l = get_line(c, at, &m);
    if (l) reset_orders(c, l, &m);
    hle_ret_s32(c, !at ? FONT_ERROR_INVALID_PARAMETER : l ? FONT_OK : FONT_ERROR_INVALID_WRITING);
}

/* sceFontWritingLineWritesOrder(line, u64 attribute (0xFA | level << 8 | spacing), const
 * SceFontWritingMetrics *, void *orderer): a run, after the one written before */
static void writing_line_writes_order(hle_call *c) {
    guest_addr at = hle_arg_ptr(c, 0), metrics = hle_arg_ptr(c, 2), orderer = hle_arg_ptr(c, 3);
    uint64_t attribute = hle_arg(c, 1);
    uint32_t level = (uint32_t)(attribute >> 8) & 0xFF;
    if (!at || !orderer || (attribute & 0xFF) != ATTRIBUTE_ID || level >= LEVEL_COUNT) {
        hle_ret_s32(c, FONT_ERROR_INVALID_PARAMETER);
        return;
    }
    font_memory m;
    writing_line *l = get_line(c, at, NULL);
    if (!l) {
        hle_ret_s32(c, FONT_ERROR_INVALID_WRITING);
        return;
    }
    if (!get_line(c, at, &m)) {
        hle_ret_s32(c, FONT_ERROR_INVALID_PARAMETER);
        return;
    }
    if (l->cursor) reset_orders(c, l, &m); /* rendering began: a new line */
    guest_addr o = mem_alloc(c, &m, sizeof(writing_order));
    writing_order *d = get_order(c, o);
    if (!d) {
        mem_free(c, &m, o);
        hle_ret_s32(c, FONT_ERROR_ALLOCATION_FAILED);
        return;
    }
    memset(d, 0, sizeof(*d));
    if (!l->last) l->first = o;
    d->orderer = orderer;
    d->attribute = attribute;
    if (metrics) gmem_read(c->mem, metrics, d->metrics, sizeof(d->metrics));
    link_order(c, l, o, level);
    if (!d->prev) l->head = o;
    if (!d->next) l->tail = o;
    l->ordering_space += ordering_space_delta(c, l, d);
    l->advance_x += d->metrics[M_ADVANCE_X];
    l->advance_y += d->metrics[M_ADVANCE_Y];
    l->last = o;
    l->last_level = level;
    hle_ret_s32(c, FONT_OK);
}

/* const SceFontWritingLineStep *sceFontWritingLineRefersRenderStep(line): the runs in
 * display order, NULL after the last (and the next call starts over) */
static void writing_line_render_step(hle_call *c) {
    guest_addr at = hle_arg_ptr(c, 0);
    writing_line *l = get_line(c, at, NULL);
    if (!l) {
        hle_ret_u64(c, 0);
        return;
    }
    if (l->cursor) {
        extend_render_metrics(l);
    } else {
        l->stepped_space = l->step.x = l->step.y = l->step.spacing_progress = 0.0f;
        memset(l->step.metrics, 0, sizeof(l->step.metrics));
        memset(l->metrics, 0, sizeof(l->metrics));
    }
    bool rtl = line_rtl(l);
    writing_order *cur = get_order(c, l->cursor);
    guest_addr next = rtl ? (cur ? cur->prev : l->tail) : (cur ? cur->next : l->head);
    writing_order *n = get_order(c, next);
    if (!n) {
        l->cursor = 0;
        memset(l->step.metrics, 0, sizeof(l->step.metrics));
        hle_ret_u64(c, 0);
        return;
    }
    const writing_order *stepped = get_order(c, rtl ? n->next : n->prev);
    if (stepped && (rtl ? is_spaced(n, stepped) : is_spaced(stepped, n)))
        l->stepped_space += rtl ? stepped->metrics[M_ADVANCE_X] - stepped->metrics[M_LEFT] + n->metrics[M_RIGHT]
                                : stepped->metrics[M_ADVANCE_X] - stepped->metrics[M_RIGHT] + n->metrics[M_LEFT];
    l->cursor = next;
    memcpy(l->step.metrics, n->metrics, sizeof(n->metrics));
    l->step.orderer = n->orderer;
    l->step.adjust[0] = l->step.adjust[1] = 0.0f;
    l->step.spacing_progress = 0.0f;
    if (l->stepped_space != 0.0f && l->ordering_space != 0.0f) l->step.spacing_progress = l->stepped_space / l->ordering_space;
    hle_ret_u64(c, at + offsetof(writing_line, step));
}

/* sceFontWritingLineGetRenderMetrics(line, SceFontWritingMetrics *out) */
static void writing_line_render_metrics(hle_call *c) {
    guest_addr at = hle_arg_ptr(c, 0), out = hle_arg_ptr(c, 1);
    writing_line *l = get_line(c, at, NULL);
    float m[6] = {0};
    if (l) memcpy(m, l->metrics, sizeof(m));
    if (out) gmem_write(c->mem, out, m, sizeof(m));
    hle_ret_s32(c, !out || !at ? FONT_ERROR_INVALID_PARAMETER : l ? FONT_OK : FONT_ERROR_INVALID_WRITING);
}

/* sceFontWritingLineGetOrderingSpace(line, float *advance, float *head, float *inline, float *tail) */
static void writing_line_ordering_space(hle_call *c) {
    guest_addr at = hle_arg_ptr(c, 0), advance = hle_arg_ptr(c, 1), head = hle_arg_ptr(c, 2), in = hle_arg_ptr(c, 3),
               tail = hle_arg_ptr(c, 4);
    writing_line *l = get_line(c, at, NULL);
    int32_t err = !at ? FONT_ERROR_INVALID_PARAMETER
                  : !l ? FONT_ERROR_INVALID_WRITING
                  : l->form != FORM_HORIZONTAL_RTL && l->form != FORM_HORIZONTAL_LTR ? FONT_ERROR_INVALID_PARAMETER
                                                                                     : FONT_OK;
    float v[4] = {0}; /* advance, head, inline, tail */
    if (err == FONT_OK) {
        bool rtl = line_rtl(l);
        const writing_order *lead = get_order(c, rtl ? l->tail : l->head), *trail = get_order(c, rtl ? l->head : l->tail);
        v[0] = l->advance_x;
        if (rtl) {
            v[1] = lead ? lead->metrics[M_RIGHT] : 0.0f;
            v[3] = trail ? trail->metrics[M_ADVANCE_X] - trail->metrics[M_LEFT] : 0.0f;
        } else {
            v[1] = lead ? lead->metrics[M_LEFT] : 0.0f;
            v[3] = trail ? trail->metrics[M_ADVANCE_X] - trail->metrics[M_RIGHT] : 0.0f;
        }
        v[2] = l->ordering_space;
    }
    if (advance) gmem_write(c->mem, advance, &v[0], 4);
    if (head) gmem_write(c->mem, head, &v[1], 4);
    if (in) gmem_write(c->mem, in, &v[2], 4);
    if (tail) gmem_write(c->mem, tail, &v[3], 4);
    hle_ret_s32(c, err);
}

#define N "libSceFont"

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(N, "sceFontTextSourceInit", text_source_init),
    HLE_FUNC_ENTRY(N, "sceFontTextSourceRewind", text_source_rewind),
    HLE_FUNC_ENTRY(N, "sceFontTextSourceSetDefaultFont", text_source_set_default_font),
    HLE_FUNC_ENTRY(N, "sceFontTextSourceSetWritingForm", text_source_set_writing_form),
    HLE_FUNC_ENTRY(N, "sceFontCreateString", create_string),
    HLE_FUNC_ENTRY(N, "sceFontDestroyString", destroy_string),
    HLE_FUNC_ENTRY(N, "sceFontStringGetTerminateCode", string_terminate_code),
    HLE_FUNC_ENTRY(N, "sceFontStringGetTerminateOrder", string_terminate_order),
    HLE_FUNC_ENTRY(N, "sceFontStringGetWritingForm", string_writing_form),
    HLE_FUNC_ENTRY(N, "sceFontStringRefersTextCharacters", string_text_characters),
    HLE_FUNC_ENTRY(N, "sceFontStringRefersRenderCharacters", string_render_characters),
    HLE_FUNC_ENTRY(N, "sceFontCharacterGetBidiLevel", character_bidi_level),
    HLE_FUNC_ENTRY(N, "sceFontCharacterGetSyllableStringState", character_syllable_state),
    HLE_FUNC_ENTRY(N, "sceFontCharacterGetTextFontCode", character_font_code),
    HLE_FUNC_ENTRY(N, "sceFontCharacterGetTextOrder", character_text_order),
    HLE_FUNC_ENTRY(N, "sceFontCharacterLooksWhiteSpace", character_looks_white_space),
    HLE_FUNC_ENTRY(N, "sceFontCharacterLooksFormatCharacters", character_looks_format),
    HLE_FUNC_ENTRY(N, "sceFontCharacterRefersTextNext", character_next),
    HLE_FUNC_ENTRY(N, "sceFontCharacterRefersTextBack", character_back),
    HLE_FUNC_ENTRY(N, "sceFontWritingInit", writing_init),
    HLE_FUNC_ENTRY(N, "sceFontWritingGetRenderMetrics", writing_render_metrics),
    HLE_FUNC_ENTRY(N, "sceFontWritingRefersRenderStep", writing_render_step),
    HLE_FUNC_ENTRY(N, "sceFontWritingRefersRenderStepCharacter", writing_render_step_character),
    HLE_FUNC_ENTRY(N, "sceFontWritingSetMaskInvisible", writing_set_mask_invisible),
    HLE_FUNC_ENTRY(N, "sceFontCreateWritingLine", create_writing_line),
    HLE_FUNC_ENTRY(N, "sceFontDestroyWritingLine", destroy_writing_line),
    HLE_FUNC_ENTRY(N, "sceFontWritingLineClear", writing_line_clear),
    HLE_FUNC_ENTRY(N, "sceFontWritingLineWritesOrder", writing_line_writes_order),
    HLE_FUNC_ENTRY(N, "sceFontWritingLineRefersRenderStep", writing_line_render_step),
    HLE_FUNC_ENTRY(N, "sceFontWritingLineGetRenderMetrics", writing_line_render_metrics),
    HLE_FUNC_ENTRY(N, "sceFontWritingLineGetOrderingSpace", writing_line_ordering_space),
};

void hle_register_font_text(void) { hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports)); }
