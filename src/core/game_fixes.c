/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/game_fixes.h"

#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "core/settings.h"

typedef struct game_fix {
    const char *title, *module, *what;
    uint64_t offset;           /* from the module's lowest mapped page */
    const uint8_t *expect;     /* the original bytes there */
    uint32_t size;
    bool (*apply)(uint8_t *at); /* changes the bytes; false when not needed in this run */
} game_fix;

/* TMNT: Splintered Fate (v01.017). Its engine sizes the framebuffers from the TV
 * (sceVideoOutGetOutputStatus: 1920x1080 or 3840x2160) but then starts its screen
 * size at a fixed 3840x2160, and only a later resize brings it to the TV's: the
 * opening screens (logos, intro art, terms) drew a 4K viewport into 1080p buffers,
 * the top-left quarter filling the screen. The two immediates become the TV's size. */
static const uint8_t k_tmnt_screen[] = {
    0xc7, 0x05, 0xe6, 0x5e, 0x60, 0x01, 0x00, 0x0f, 0x00, 0x00, /* mov dword [rip+0x1605ee6], 3840 */
    0xc7, 0x05, 0xe0, 0x5e, 0x60, 0x01, 0x70, 0x08, 0x00, 0x00, /* mov dword [rip+0x1605ee0], 2160 */
};

static bool tmnt_screen(uint8_t *at) {
    const int internal = settings_internal_height();
    if (settings_output_4k() && !internal) return false; /* a 4K TV: the fixed size is right */
    const uint32_t h = internal ? (uint32_t)internal : 1080, w = h * 16 / 9;
    memcpy(at + 6, &w, 4);
    memcpy(at + 16, &h, 4);
    return true;
}

/* Its framebuffer size from the TV's resolution: 1 -> 1920x1080, 2 -> 3840x2160.
 * [geral] resolucao_interna makes both that height at 16:9 (the window scales it):
 * 720p has 44% of the pixels of 1080p, for weak GPUs. */
static const uint8_t k_tmnt_framebuffer[] = {
    0x41, 0xbc, 0x80, 0x07, 0x00, 0x00, /* mov r12d, 1920 */
    0x41, 0xbf, 0x38, 0x04, 0x00, 0x00, /* mov r15d, 1080 */
    0xeb, 0x0c,                         /* jmp */
    0x41, 0xbc, 0x00, 0x0f, 0x00, 0x00, /* mov r12d, 3840 */
    0x41, 0xbf, 0x70, 0x08, 0x00, 0x00, /* mov r15d, 2160 */
};

static bool tmnt_framebuffer(uint8_t *at) {
    const int internal = settings_internal_height();
    if (!internal) return false;
    const uint32_t h = (uint32_t)internal, w = h * 16 / 9;
    memcpy(at + 2, &w, 4);
    memcpy(at + 8, &h, 4);
    memcpy(at + 16, &w, 4);
    memcpy(at + 22, &h, 4);
    return true;
}

/* Sonic Origins Plus (v02.002). Its renderer always draws at 3840x2160: both sizes
 * of its display setup (the swap chain's at +0x18, the scene's at +0x20) are fixed,
 * read at hiteR+0x8cc716 and +0x8cc742..0x8cc74f right before the swap chain and the
 * renderer get created. A 4K scene makes it GPU-bound on most PCs (Iris Xe: title
 * ~3 FPS, Green Hill ~8 FPS) and every frame it clears ~170 MB of 4K targets. Here
 * the display is 1920x1080 with [geral] resolucao=1080 (the default) or the
 * resolucao_interna height at 16:9, as a 1080p console would set it up; a 4K output keeps
 * the game's own size. The swap chain's loads become a jump into the 13 bytes of
 * padding after the next function, which write the size into the setup and come
 * back through the padding after the one after; the scene then takes the swap
 * chain's size. */
static const uint8_t k_origins_display[] = { /* hiteR+0x8cc70f..0x8cc7c0 */
    0x4c, 0x8b, 0x67, 0x40, 0x48, 0x89, 0xf3, 0x8b, 0x76, 0x18, 0x8b, 0x53,
    0x1c, 0x4c, 0x8b, 0x43, 0x28, 0x4c, 0x8b, 0x4b, 0x30, 0x49, 0x8b, 0x7c,
    0x24, 0x20, 0x4d, 0x8d, 0x74, 0x24, 0x28, 0x4c, 0x89, 0xf1, 0xe8, 0x9a,
    0x43, 0x23, 0x00, 0x41, 0x89, 0xc7, 0x84, 0xc0, 0x74, 0x32, 0x49, 0x8b,
    0x7c, 0x24, 0x20, 0x8b, 0x73, 0x18, 0x8b, 0x53, 0x1c, 0x8b, 0x4b, 0x20,
    0x44, 0x8b, 0x43, 0x24, 0x48, 0x8b, 0x43, 0x38, 0x4d, 0x89, 0xf1, 0x48,
    0x89, 0x04, 0x24, 0xe8, 0xd1, 0x51, 0x23, 0x00, 0x48, 0x8b, 0x05, 0x4a,
    0x5b, 0x61, 0x01, 0x49, 0x8b, 0x4c, 0x24, 0x28, 0x48, 0x89, 0x48, 0x60,
    0x44, 0x89, 0xf8, 0x48, 0x83, 0xc4, 0x10, 0x5b, 0x41, 0x5c, 0x41, 0x5e,
    0x41, 0x5f, 0x5d, 0xc3, 0xcc, 0x55, 0x48, 0x89, 0xe5, 0x48, 0x8b, 0x07,
    0xff, 0x50, 0x70, 0x48, 0x8b, 0x80, 0x30, 0x24, 0x00, 0x00, 0x5d, 0xc3,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0x55, 0x48, 0x89, 0xe5, 0x48, 0x8b, 0x07, 0xff, 0x50, 0x70, 0x48,
    0x8b, 0x80, 0xf8, 0x25, 0x00, 0x00, 0x5d, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
};

static bool origins_display(uint8_t *at) {
    const int internal = settings_internal_height();
    if (settings_output_4k() && !internal) return false; /* 4K output: the game's own size */
    const uint32_t h = internal ? (uint32_t)internal : 1080, w = h * 16 / 9;
    /* +0x07: jmp +0x84; then the original r8/r9 loads (back here from +0xa4) */
    static const uint8_t site[14] = {0xeb, 0x7b, 0x4c, 0x8b, 0x43, 0x28, 0x4c, 0x8b, 0x4b, 0x30, 0x90, 0x90, 0x90, 0x90};
    /* +0x39: the scene's size = the swap chain's (esi, edx were just loaded from +0x18) */
    static const uint8_t scene[7] = {0x89, 0xf1, 0x41, 0x89, 0xd0, 0x90, 0x90}; /* mov ecx, esi; mov r8d, edx */
    uint8_t pad1[12] = {0xbe, 0, 0, 0, 0, 0xba, 0, 0, 0, 0, 0xeb, 0x14}; /* +0x84: mov esi, w; mov edx, h; jmp +0xa4 */
    static const uint8_t pad2[11] = {0x89, 0x73, 0x18, 0x89, 0x53, 0x1c, /* +0xa4: mov [rbx+0x18], esi; mov [rbx+0x1c], edx */
                                     0xe9, 0x5a, 0xff, 0xff, 0xff};    /* jmp +0x09 */
    memcpy(pad1 + 1, &w, 4);
    memcpy(pad1 + 6, &h, 4);
    memcpy(at + 0xa4, pad2, sizeof(pad2));
    memcpy(at + 0x84, pad1, sizeof(pad1));
    memcpy(at + 0x39, scene, sizeof(scene));
    memcpy(at + 0x07, site, sizeof(site));
    return true;
}

static const game_fix k_fixes[] = {
    {"PPSA25582", "GameRebirth", "screen size of the opening screens follows the TV", 0x71ac80, k_tmnt_screen,
     sizeof(k_tmnt_screen), tmnt_screen},
    {"PPSA25582", "GameRebirth", "renders at 1280x720 (resolucao_interna=720)", 0x71a338, k_tmnt_framebuffer,
     sizeof(k_tmnt_framebuffer), tmnt_framebuffer},
    {"PPSA05324", "hiteR", "display set up at the output's size instead of a fixed 3840x2160", 0x8cc70f,
     k_origins_display, sizeof(k_origins_display), origins_display},
};

compat_patch_status game_compat_apply(gmem *mem, const loaded_module *m,
                                     const compat_patch *p, const char *title,
                                     const char *version, bool enabled) {
    if (!enabled) return COMPAT_PATCH_DISABLED;
    if (!m || !m->is_main || !m->mapped) return COMPAT_PATCH_UNMAPPED;
    compat_patch_status status = compat_patch_check(p, title, version, m->img.file, m->img.file_size);
    if (status != COMPAT_PATCH_READY) return status;
    for (uint32_t i = 0; i < m->img.segment_count; i++) {
        const sce_segment *s = &m->img.segments[i];
        if (s->h.p_type != PT_LOAD || !(s->h.p_flags & PF_X) || !s->has_data ||
            p->file_offset < s->data_off) continue;
        uint64_t off = p->file_offset - s->data_off;
        if (off > s->h.p_filesz || p->size > s->h.p_filesz - off) continue;
        if (s->h.p_vaddr > UINT64_MAX - m->bias ||
            off > UINT64_MAX - (m->bias + s->h.p_vaddr)) return COMPAT_PATCH_UNMAPPED;
        uint64_t addr = m->bias + s->h.p_vaddr + off;
        if (addr < m->base || addr - m->base > m->map_size ||
            p->size > m->map_size - (addr - m->base)) return COMPAT_PATCH_UNMAPPED;
        uint8_t *at = gmem_host(mem, addr, p->size);
        if (!at) return COMPAT_PATCH_UNMAPPED;
        if (memcmp(at, p->expected, p->size)) return COMPAT_PATCH_WRONG_BYTES;
        memcpy(at, p->replacement, p->size);
        return COMPAT_PATCH_APPLIED;
    }
    return COMPAT_PATCH_UNMAPPED;
}

static void apply_compat_patches(gmem *mem, const loaded_module *m) {
    if (!m->is_main) return;
    char title[32] = "", version[32] = "";
    if (!settings_param_string("titleId", title, sizeof(title))) return;
    settings_param_string("contentVersion", version, sizeof(version));
    for (size_t i = 0; i < compat_patch_count(); i++) {
        const compat_patch *p = compat_patch_at(i);
        if (strcmp(title, p->title_id)) continue;
        bool enabled = !getenv("PS5C_NO_GAME_FIXES") &&
                       settings_get_int("patches", p->id, p->default_enabled) == 1;
        compat_patch_status status = game_compat_apply(mem, m, p, title, version, enabled);
        if (status == COMPAT_PATCH_APPLIED)
            LOG_I(CORE, "compatibility patch %s applied in memory: %s %s (game file unchanged)", p->id, title, version);
        else if (status == COMPAT_PATCH_DISABLED)
            LOG_I(CORE, "compatibility patch %s disabled: %s", p->id, title);
        else
            LOG_W(CORE, "compatibility patch %s skipped: %s %s, status=%d (unsupported version, signature or mapping)",
                  p->id, title, version, (int)status);
    }
}

void game_fixes_apply(gmem *mem, const loaded_module *m) {
    apply_compat_patches(mem, m);
    if (getenv("PS5C_NO_GAME_FIXES") || !m->name) return;
    char title[32];
    bool have_title = false, asked = false;
    for (size_t i = 0; i < PS5C_ARRAY_COUNT(k_fixes); i++) {
        const game_fix *f = &k_fixes[i];
        if (strcmp(f->module, m->name) != 0) continue;
        if (!asked) have_title = settings_param_string("titleId", title, sizeof(title)), asked = true;
        if (!have_title || strcmp(f->title, title) != 0) continue;
        if (f->offset + f->size > m->map_size) continue;
        uint8_t *at = (uint8_t *)gmem_host(mem, m->base + f->offset, f->size);
        if (!at || memcmp(at, f->expect, f->size) != 0) {
            LOG_I(CORE, "game fix for %s (%s) not applied: another version of %s", f->title, f->what, f->module);
            continue;
        }
        if (f->apply(at)) LOG_I(CORE, "game fix for %s: %s", f->title, f->what);
    }
}
