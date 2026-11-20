/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native mouse and IME keyboard ABI. Off by default; controller emulation is
 * independent. Public ABI references: shadPS4 mouse.h and ime_common.h. */
#include "base/log.h"
#include "core/runtime.h"
#include "hle/hle.h"
#include "pal/native_input.h"
#include "pal/pal.h"
#include <stdatomic.h>
#include <string.h>

void hle_register_table(const hle_export *table, size_t count);
#define MOUSE_ARG ((int32_t)0x80DF0001)
#define MOUSE_HANDLE ((int32_t)0x80DF0003)
#define IME_ADDRESS ((int32_t)0x80BC0031)
#define IME_USER ((int32_t)0x80BC0010)
static bool g_mouse_init, g_mouse_open;
static guest_addr g_key_handler, g_key_arg;
static uint32_t g_key_option;
static bool g_key_open, g_key_announced, g_key_connected;
static atomic_flag g_updating = ATOMIC_FLAG_INIT;
static bool g_usb_key_init, g_usb_key_open, g_usb_pressed[256];
static uint32_t g_usb_modifiers;
static atomic_flag g_usb_reading = ATOMIC_FLAG_INIT;

typedef struct mouse_data {
    uint64_t timestamp;
    uint8_t connected, padding[3];
    uint32_t buttons;
    int32_t x, y, wheel, tilt;
    uint8_t reserved[8];
} mouse_data;
PS5C_STATIC_ASSERT(sizeof(mouse_data) == 40, "mouse ABI");

static void mouse_init(hle_call *c) {
    g_mouse_init = true;
    hle_ret_s32(c, 0);
}
static void mouse_open(hle_call *c) {
    if (!native_mouse_enabled()) {
        hle_ret_s32(c, MOUSE_HANDLE);
        return;
    }
    if (!g_mouse_init) {
        hle_ret_s32(c, (int32_t)0x80DF0005);
        return;
    }
    if (hle_arg_s32(c, 0) != 1 || hle_arg_s32(c, 1) || hle_arg_s32(c, 2)) {
        hle_ret_s32(c, MOUSE_ARG);
        return;
    }
    guest_addr param = hle_arg_ptr(c, 3);
    uint8_t flag = 0;
    /* Quake sets only the flag byte; the remaining reserved bytes are not
     * initialized by the title and must not be interpreted as flags. */
    if (param && (!gmem_read(c->mem, param, &flag, 1) || flag > 1)) {
        hle_ret_s32(c, MOUSE_ARG);
        return;
    }
    if (g_mouse_open) {
        hle_ret_s32(c, (int32_t)0x80DF0004);
        return;
    }
    g_mouse_open = true;
    native_input_clear_mouse();
    LOG_I(INPUT, "native mouse opened (click game to capture, F8 releases cursor)");
    hle_ret_s32(c, 1);
}
static void mouse_close(hle_call *c) {
    if (!g_mouse_open || hle_arg_s32(c, 0) != 1) {
        hle_ret_s32(c, MOUSE_HANDLE);
        return;
    }
    g_mouse_open = false;
    native_input_clear_mouse();
    hle_ret_s32(c, 0);
}
static void mouse_read(hle_call *c) {
    if (!g_mouse_open || hle_arg_s32(c, 0) != 1) {
        hle_ret_s32(c, MOUSE_HANDLE);
        return;
    }
    int32_t count = hle_arg_s32(c, 2);
    guest_addr addr = hle_arg_ptr(c, 1);
    if (count < 1 || count > 64 || !addr ||
        !hle_mem(c, addr, (uint64_t)count * sizeof(mouse_data))) {
        hle_ret_s32(c, MOUSE_ARG);
        return;
    }
    int32_t written = 0;
    do {
        native_mouse m;
        bool queued = native_input_read_mouse(&m);
        if (!queued && written)
            break;
        mouse_data d = {.timestamp = m.time_us,
                        .connected = native_mouse_enabled(),
                        .buttons = m.buttons,
                        .x = m.x,
                        .y = m.y,
                        .wheel = m.wheel,
                        .tilt = m.tilt};
        if (!gmem_write(c->mem, addr + (uint64_t)written * sizeof(d), &d, sizeof(d))) {
            hle_ret_s32(c, MOUSE_ARG);
            return;
        }
        written++;
        if (!queued)
            break;
    } while (written < count);
    hle_ret_s32(c, written);
}
static void keyboard_open(hle_call *c) {
    /* Opening without a device must still succeed (Minecraft depends on it). */
    if (!native_keyboard_requested()) {
        hle_ret_s32(c, 0);
        return;
    }
    uint8_t param[32];
    if (hle_arg_s32(c, 0) != 1) {
        hle_ret_s32(c, IME_USER);
        return;
    }
    if (!gmem_read(c->mem, hle_arg_ptr(c, 1), param, sizeof(param))) {
        hle_ret_s32(c, IME_ADDRESS);
        return;
    }
    guest_addr fn, arg;
    uint32_t option;
    memcpy(&option, param, 4);
    memcpy(&arg, param + 8, 8);
    memcpy(&fn, param + 16, 8);
    if (!fn) {
        hle_ret_s32(c, (int32_t)0x80BC0022);
        return;
    }
    if (g_key_open) {
        hle_ret_s32(c, (int32_t)0x80BC0001);
        return;
    }
    g_key_handler = fn;
    g_key_arg = arg;
    g_key_option = option;
    g_key_open = true;
    g_key_announced = false;
    g_key_connected = false;
    native_input_clear_keys();
    pal_input_has_controller();
    LOG_I(INPUT, "native keyboard opened");
    hle_ret_s32(c, 0);
}
static void keyboard_close(hle_call *c) {
    if (!native_keyboard_requested()) {
        hle_ret_s32(c, 0);
        return;
    }
    if (hle_arg_s32(c, 0) != 1) {
        hle_ret_s32(c, IME_USER);
        return;
    }
    g_key_open = false;
    g_key_handler = g_key_arg = 0;
    native_input_clear_keys();
    hle_ret_s32(c, 0);
}
static void keyboard_resource_id(hle_call *c) {
    pal_input_has_controller();
    if (!native_keyboard_enabled()) {
        hle_ret_s32(c, (int32_t)0x80BC0101);
        return;
    }
    if (hle_arg_s32(c, 0) != 1) {
        hle_ret_s32(c, IME_USER);
        return;
    }
    uint32_t ids[6] = {1, g_key_open ? 1u : 0u, 0, 0, 0, 0};
    hle_ret_s32(c, gmem_write(c->mem, hle_arg_ptr(c, 1), ids, sizeof(ids)) ? 0 : IME_ADDRESS);
}
static void keyboard_info(hle_call *c) {
    pal_input_has_controller();
    if (!native_keyboard_requested()) {
        uint8_t info[32] = {0};
        if (hle_mem(c, hle_arg_ptr(c, 1), sizeof(info)))
            gmem_write(c->mem, hle_arg_ptr(c, 1), info, sizeof(info));
        hle_ret_s32(c, 0);
        return;
    }
    uint32_t info[9] = {0};
    if (g_key_open && native_keyboard_enabled() && hle_arg_u32(c, 0) == 1) {
        info[0] = 1;
        info[2] = 4;
        info[3] = 500;
        info[4] = 33;
        info[5] = 1;
    }
    hle_ret_s32(c, gmem_write(c->mem, hle_arg_ptr(c, 1), info, sizeof(info)) ? 0 : IME_ADDRESS);
}
/* On the guest thread, HLE locals are on its mapped stack. The nested call
 * runs below this frame, so the event remains valid for the callback. */
static bool keyboard_event(hle_call *c, uint32_t id, const native_key *key) {
    uint64_t event[9] = {0};
    uint8_t *p = (uint8_t *)event;
    memcpy(p, &id, 4);
    if (key) {
        uint32_t user = 1;
        memcpy(p + 8, &key->code, 2);
        memcpy(p + 10, &key->character, 2);
        memcpy(p + 12, &key->status, 4);
        memcpy(p + 16, &key->type, 4);
        memcpy(p + 20, &user, 4);
        memcpy(p + 24, &user, 4);
        memcpy(p + 32, &key->time_us, 8);
    } else {
        uint32_t ids[6] = {1, g_key_connected ? 1u : 0u, 0, 0, 0, 0};
        memcpy(p + 8, ids, sizeof(ids));
    }
    uint64_t result;
    return runtime_call_nested(c->rt, g_key_handler, g_key_arg, (uint64_t)(uintptr_t)event, 0,
                               &result);
}
/* ---- libSceIme text editor (sceImeOpen): no panel is drawn. With the native
 * keyboard on, the PC keyboard types into it (Enter confirms, Esc cancels);
 * otherwise it confirms right away with the text the game put in it (a world's
 * default name). Events go to the game's handler from sceImeUpdate, like the
 * keyboard's. Param/event layouts and panel sizes as in KytyPS5's ime.h. */
#define IME_BUSY ((int32_t)0x80BC0001)
#define IME_NOT_OPENED ((int32_t)0x80BC0002)
#define IME_INVALID_TYPE ((int32_t)0x80BC0011)
#define IME_INVALID_OPTION ((int32_t)0x80BC0015)
#define IME_INVALID_MAX_TEXT_LENGTH ((int32_t)0x80BC0016)
#define IME_INVALID_TEXT_BUFFER ((int32_t)0x80BC0017)
#define IME_INVALID_WORK ((int32_t)0x80BC0020)
#define IME_INVALID_HANDLER ((int32_t)0x80BC0022)
#define IME_INVALID_PARAM ((int32_t)0x80BC0030)
#define IME_VALID_OPTIONS 0x7bffu
#define IME_OPTION_OVER_2K 0x4000u
#define IME_TYPE_NUMBER 4u
#define IME_MAX_TEXT 2048u
#define IME_WORK_SIZE (20u * 1024u)
enum { IME_EV_OPEN = 0, IME_EV_TEXT = 1, IME_EV_CARET = 2, IME_EV_CLOSE = 4, IME_EV_ENTER = 5 };
static struct {
    bool open;
    guest_addr handler, arg, work, input;
    uint32_t max, len, caret, type, option, session;
    float x, y;
    uint16_t text[IME_MAX_TEXT + 1];
    uint32_t events[64], event_param[64], count;
} g_ime;
static void ime_panel_size(uint32_t type, uint32_t option, uint32_t *w, uint32_t *h) {
    *w = type == IME_TYPE_NUMBER ? 370 : 793;
    *h = type == IME_TYPE_NUMBER ? 402 : 408;
    if (option & IME_OPTION_OVER_2K) *w *= 2, *h *= 2;
}
static void ime_queue(uint32_t id, uint32_t param) {
    if (g_ime.count < PS5C_ARRAY_COUNT(g_ime.events)) {
        g_ime.events[g_ime.count] = id;
        g_ime.event_param[g_ime.count++] = param;
    }
}
/* the text in the work area (what UPDATE_TEXT points at) and in the game's buffer */
static void ime_sync(hle_call *c) {
    uint16_t z = 0;
    for (int i = 0; i < 2; i++) {
        guest_addr a = i ? g_ime.input : g_ime.work;
        gmem_write(c->mem, a, g_ime.text, (uint64_t)g_ime.len * 2);
        gmem_write(c->mem, a + (uint64_t)g_ime.len * 2, &z, 2);
    }
}
/* int sceImeGetPanelSize(const SceImeParam *param, uint32_t *width, uint32_t *height) */
static void ime_get_panel_size(hle_call *c) {
    uint32_t p[9], *w = (uint32_t *)hle_mem(c, hle_arg_ptr(c, 1), 4), *h = (uint32_t *)hle_mem(c, hle_arg_ptr(c, 2), 4);
    if (!w || !h || !gmem_read(c->mem, hle_arg_ptr(c, 0), p, sizeof(p))) {
        hle_ret_s32(c, IME_ADDRESS);
        return;
    }
    if (p[1] > IME_TYPE_NUMBER) { hle_ret_s32(c, IME_INVALID_TYPE); return; }
    if (p[8] & ~IME_VALID_OPTIONS) { hle_ret_s32(c, IME_INVALID_OPTION); return; }
    ime_panel_size(p[1], p[8], w, h);
    hle_ret_s32(c, 0);
}
/* int sceImeOpen(const SceImeParam *param, const SceImeParamExtended *extended)
 * SceImeParam (0x60): +4 type, +0x20 option, +0x24 maxTextLength, +0x28 inputTextBuffer,
 * +0x30 posx, +0x34 posy (floats), +0x40 work, +0x48 arg, +0x50 handler */
static void ime_open(hle_call *c) {
    uint8_t p[0x60];
    if (g_ime.open) { hle_ret_s32(c, IME_BUSY); return; }
    if (!gmem_read(c->mem, hle_arg_ptr(c, 0), p, sizeof(p))) { hle_ret_s32(c, IME_ADDRESS); return; }
    uint32_t type, option, max;
    guest_addr input, work, arg, handler;
    memcpy(&type, p + 4, 4);
    memcpy(&option, p + 0x20, 4);
    memcpy(&max, p + 0x24, 4);
    memcpy(&input, p + 0x28, 8);
    memcpy(&work, p + 0x40, 8);
    memcpy(&arg, p + 0x48, 8);
    memcpy(&handler, p + 0x50, 8);
    int32_t err = type > IME_TYPE_NUMBER ? IME_INVALID_TYPE
                : option & ~IME_VALID_OPTIONS ? IME_INVALID_OPTION
                : !max || max > IME_MAX_TEXT ? IME_INVALID_MAX_TEXT_LENGTH
                : !input || !hle_mem(c, input, ((uint64_t)max + 1) * 2) ? IME_INVALID_TEXT_BUFFER
                : !work || !hle_mem(c, work, IME_WORK_SIZE) ? IME_INVALID_WORK
                : !handler ? IME_INVALID_HANDLER : 0;
    if (err) { hle_ret_s32(c, err); return; }
    uint32_t session = g_ime.session + 1;
    memset(&g_ime, 0, sizeof(g_ime));
    g_ime.session = session;
    g_ime.handler = handler, g_ime.arg = arg, g_ime.work = work, g_ime.input = input;
    g_ime.max = max, g_ime.type = type, g_ime.option = option;
    memcpy(&g_ime.x, p + 0x30, 4);
    memcpy(&g_ime.y, p + 0x34, 4);
    const uint16_t *start = (const uint16_t *)hle_mem(c, input, ((uint64_t)max + 1) * 2);
    while (g_ime.len < max && start[g_ime.len]) g_ime.len++;
    memcpy(g_ime.text, start, (size_t)g_ime.len * 2);
    g_ime.caret = g_ime.len;
    memset(hle_mem(c, work, IME_WORK_SIZE), 0, IME_WORK_SIZE);
    ime_sync(c);
    g_ime.open = true;
    ime_queue(IME_EV_OPEN, 0);
    bool typing = native_keyboard_enabled();
    if (typing) native_input_clear_keys();
    else ime_queue(IME_EV_ENTER, 0); /* no way to type: accept the text as it is */
    LOG_I(INPUT, "text input opened (%u characters, up to %u): %s", g_ime.len, max,
          typing ? "type with the PC keyboard, Enter confirms, Esc cancels" : "confirmed as it is (native keyboard off)");
    hle_ret_s32(c, 0);
}
/* int sceImeClose(void) */
static void ime_close(hle_call *c) {
    if (!g_ime.open) { hle_ret_s32(c, IME_NOT_OPENED); return; }
    g_ime.open = false;
    g_ime.count = 0;
    hle_ret_s32(c, 0);
}
/* int sceImeSetText(const char16_t *text, uint32_t length): replaces the text */
static void ime_set_text(hle_call *c) {
    if (!g_ime.open) { hle_ret_s32(c, IME_NOT_OPENED); return; }
    uint32_t n = hle_arg_u32(c, 1);
    if (n > g_ime.max) n = g_ime.max;
    const uint16_t *t = (const uint16_t *)hle_mem(c, hle_arg_ptr(c, 0), (uint64_t)n * 2);
    if (!hle_arg_ptr(c, 0) || (n && !t)) { hle_ret_s32(c, IME_ADDRESS); return; }
    g_ime.len = 0;
    while (g_ime.len < n && t[g_ime.len]) g_ime.text[g_ime.len] = t[g_ime.len], g_ime.len++;
    if (g_ime.caret > g_ime.len) g_ime.caret = g_ime.len;
    ime_sync(c);
    hle_ret_s32(c, 0);
}
/* int sceImeSetCaret(const SceImeCaret *caret): {float x, y; u32 height, index} */
static void ime_set_caret(hle_call *c) {
    if (!g_ime.open) { hle_ret_s32(c, IME_NOT_OPENED); return; }
    uint32_t caret[4];
    if (!gmem_read(c->mem, hle_arg_ptr(c, 0), caret, sizeof(caret))) { hle_ret_s32(c, IME_ADDRESS); return; }
    if (caret[3] > g_ime.len) { hle_ret_s32(c, IME_INVALID_PARAM); return; }
    g_ime.caret = caret[3];
    hle_ret_s32(c, 0);
}
/* int sceImeSetTextGeometry(SceImeTextAreaMode mode, const SceImeTextGeometry *geometry):
 * where the game draws the text; there is no panel to place */
static void ime_set_text_geometry(hle_call *c) {
    if (!g_ime.open) { hle_ret_s32(c, IME_NOT_OPENED); return; }
    if (!hle_mem(c, hle_arg_ptr(c, 1), 16)) { hle_ret_s32(c, IME_ADDRESS); return; }
    uint32_t mode = hle_arg_u32(c, 0);
    hle_ret_s32(c, mode == 2 || mode == 3 ? 0 : IME_INVALID_PARAM);
}
/* PC keys into the text: characters at the caret, Backspace, arrows, Enter, Esc */
static void ime_read_keys(hle_call *c) {
    native_key k;
    for (unsigned i = 0; i < 256 && native_input_read_key(&k); i++) {
        if (k.event != 257 && k.event != 259) continue; /* key downs and repeats */
        if (k.code == 0x28 || k.code == 0x58) ime_queue(IME_EV_ENTER, 0);
        else if (k.code == 0x29) ime_queue(IME_EV_CLOSE, 0);
        else if (k.code == 0x2A) {
            if (!g_ime.caret) continue;
            memmove(g_ime.text + g_ime.caret - 1, g_ime.text + g_ime.caret, (size_t)(g_ime.len - g_ime.caret) * 2);
            g_ime.caret--, g_ime.len--;
            ime_sync(c);
            ime_queue(IME_EV_TEXT, 0);
        } else if (k.code == 0x50 || k.code == 0x4F) {
            bool right = k.code == 0x4F;
            if (right ? g_ime.caret >= g_ime.len : !g_ime.caret) continue;
            g_ime.caret += right ? 1 : -1;
            ime_queue(IME_EV_CARET, right ? 2 : 1);
        } else if (k.character >= 0x20 && k.character != 0x7F && g_ime.len < g_ime.max) {
            memmove(g_ime.text + g_ime.caret + 1, g_ime.text + g_ime.caret, (size_t)(g_ime.len - g_ime.caret) * 2);
            g_ime.text[g_ime.caret++] = k.character;
            g_ime.len++;
            ime_sync(c);
            ime_queue(IME_EV_TEXT, 0);
        }
    }
}
/* SceImeEvent (0x48): u32 id; +8 param: rect {float x, y; u32 w, h}, edit text
 * {char16_t *str; u32 caretIndex, areaNum; {u32 mode, index; s32 length} area[4]}, or u32 caret move */
static void ime_update(hle_call *c) {
    if (atomic_flag_test_and_set_explicit(&g_updating, memory_order_acquire)) return;
    if (native_keyboard_enabled()) ime_read_keys(c);
    uint32_t session = g_ime.session;
    for (uint32_t i = 0; i < g_ime.count && g_ime.open && g_ime.session == session; i++) {
        uint64_t event[9] = {0};
        uint8_t *p = (uint8_t *)event;
        uint32_t id = g_ime.events[i], u[6];
        memcpy(p, &id, 4);
        if (id == IME_EV_OPEN) {
            memcpy(p + 8, &g_ime.x, 4);
            memcpy(p + 12, &g_ime.y, 4);
            ime_panel_size(g_ime.type, g_ime.option, &u[0], &u[1]);
            memcpy(p + 16, u, 8);
        } else if (id == IME_EV_TEXT) {
            memcpy(p + 8, &g_ime.work, 8);
            u[0] = g_ime.caret, u[1] = 1, u[2] = 1 /* edit */, u[3] = 0, u[4] = g_ime.len;
            memcpy(p + 16, u, 20);
        } else if (id == IME_EV_CARET) {
            memcpy(p + 8, &g_ime.event_param[i], 4);
        }
        uint64_t result;
        if (!runtime_call_nested(c->rt, g_ime.handler, g_ime.arg, (uint64_t)(uintptr_t)event, 0, &result)) break;
    }
    if (g_ime.session == session) g_ime.count = 0;
    atomic_flag_clear_explicit(&g_updating, memory_order_release);
}
static void keyboard_update(hle_call *c) {
    hle_ret_s32(c, 0);
    if (g_ime.open && hle_arg_ptr(c, 0) == g_ime.handler && c->rt) {
        ime_update(c);
        return;
    }
    if (!g_key_open || hle_arg_ptr(c, 0) != g_key_handler || !c->rt ||
        atomic_flag_test_and_set_explicit(&g_updating, memory_order_acquire))
        return;
    pal_input_has_controller();
    bool connected = native_keyboard_enabled();
    if (!g_key_announced) {
        g_key_connected = connected;
        g_key_announced = true;
        if (!keyboard_event(c, 256, NULL))
            goto done;
    }
    native_key key;
    /* Release events are delivered before a disconnect notification. */
    if (g_key_connected || connected)
        for (unsigned i = 0; i < 256 && g_key_open && native_input_read_key(&key); i++) {
            if (key.event == 259 && !(g_key_option & 3))
                continue;
            if (!keyboard_event(c, key.event, &key))
                goto done;
        }
    if (g_key_open && g_key_connected != connected) {
        g_key_connected = connected;
        keyboard_event(c, connected ? 260 : 261, NULL);
    }
done:
    atomic_flag_clear_explicit(&g_updating, memory_order_release);
}

/* libSceKeyboard on PS5 returns snapshots, unlike the IME event callbacks.
 * ABI recorded by the upstream port, ps5/src/platform.c at 45dea6a4. */
typedef struct usb_keyboard_data {
    uint64_t timestamp;
    uint8_t unknown, padding[7], connected, padding2[3];
    uint32_t count, unknown2, modifiers;
    uint16_t usages[16];
} usb_keyboard_data;
PS5C_STATIC_ASSERT(sizeof(usb_keyboard_data) == 64, "PS5 keyboard snapshot ABI");
static void usb_key_init(hle_call *c) {
    g_usb_key_init = true;
    hle_ret_s32(c, 0);
}
static void usb_key_open(hle_call *c) {
    if (!g_usb_key_init) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    if (hle_arg_s32(c, 0) != 1 || hle_arg_s32(c, 1) || hle_arg_s32(c, 2) || hle_arg_ptr(c, 3)) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    if (g_usb_key_open) {
        hle_ret_s32(c, (int32_t)0x80020010);
        return;
    }
    memset(g_usb_pressed, 0, sizeof(g_usb_pressed));
    g_usb_modifiers = 0;
    g_usb_key_open = true;
    native_input_clear_keys();
    hle_ret_s32(c, 1);
}
static void usb_key_close(hle_call *c) {
    if (!g_usb_key_open || hle_arg_s32(c, 0) != 1) {
        hle_ret_s32(c, (int32_t)0x80020009);
        return;
    }
    g_usb_key_open = false;
    memset(g_usb_pressed, 0, sizeof(g_usb_pressed));
    native_input_clear_keys();
    hle_ret_s32(c, 0);
}
static void usb_key_read(hle_call *c) {
    if (!g_usb_key_open || hle_arg_s32(c, 0) != 1) {
        hle_ret_s32(c, (int32_t)0x80020009);
        return;
    }
    int capacity = hle_arg_s32(c, 2);
    guest_addr out = hle_arg_ptr(c, 1);
    if (capacity < 1 || capacity > 64 ||
        !hle_mem(c, out, (uint64_t)capacity * sizeof(usb_keyboard_data))) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    if (atomic_flag_test_and_set_explicit(&g_usb_reading, memory_order_acquire)) {
        hle_ret_s32(c, 0);
        return;
    }
    pal_input_has_controller();
    bool connected = native_keyboard_enabled();
    int count = 0;
    if (!connected) {
        memset(g_usb_pressed, 0, sizeof(g_usb_pressed));
        g_usb_modifiers = 0;
    }
    do {
        native_key key;
        bool event = native_input_read_key(&key);
        if (event && connected && key.code < 256) {
            if (key.event != 259) {
                g_usb_pressed[key.code] = key.event != 258;
                g_usb_modifiers = (key.status >> 8) & 255;
            }
        }
        usb_keyboard_data sample = {.timestamp = event ? key.time_us : pal_time_ns() / 1000,
                                    .connected = connected,
                                    .modifiers = g_usb_modifiers};
        unsigned pressed = 0;
        for (unsigned i = 4; i < 0xe0; i++)
            if (g_usb_pressed[i]) {
                if (pressed < 16)
                    sample.usages[pressed] = (uint16_t)i;
                pressed++;
            }
        if (pressed > 16) {
            for (unsigned i = 0; i < 16; i++)
                sample.usages[i] = 1;
            sample.count = 16;
        } else
            sample.count = pressed ? pressed : 1;
        if (!gmem_write(c->mem, out + (uint64_t)count * sizeof(sample), &sample, sizeof(sample)))
            break;
        count++;
        if (!event)
            break;
    } while (count < capacity);
    atomic_flag_clear_explicit(&g_usb_reading, memory_order_release);
    hle_ret_s32(c, count);
}
static const hle_export exports[] = {
    HLE_FUNC_ENTRY("libSceKeyboard", "sceKeyboardInit", usb_key_init),
    HLE_FUNC_ENTRY("libSceKeyboard", "sceKeyboardOpen", usb_key_open),
    HLE_FUNC_ENTRY("libSceKeyboard", "sceKeyboardClose", usb_key_close),
    HLE_FUNC_ENTRY("libSceKeyboard", "sceKeyboardRead", usb_key_read),
    HLE_FUNC_ENTRY("libSceMouse", "sceMouseInit", mouse_init),
    HLE_FUNC_ENTRY("libSceMouse", "sceMouseOpen", mouse_open),
    HLE_FUNC_ENTRY("libSceMouse", "sceMouseClose", mouse_close),
    HLE_FUNC_ENTRY("libSceMouse", "sceMouseRead", mouse_read),
    HLE_FUNC_ENTRY("libSceIme", "sceImeKeyboardOpen", keyboard_open),
    HLE_FUNC_ENTRY("libSceIme", "sceImeKeyboardClose", keyboard_close),
    HLE_FUNC_ENTRY("libSceIme", "sceImeKeyboardGetResourceId", keyboard_resource_id),
    HLE_FUNC_ENTRY("libSceIme", "sceImeKeyboardGetInfo", keyboard_info),
    HLE_FUNC_ENTRY("libSceIme", "sceImeUpdate", keyboard_update),
    HLE_FUNC_ENTRY("libSceIme", "sceImeOpen", ime_open),
    HLE_FUNC_ENTRY("libSceIme", "sceImeClose", ime_close),
    HLE_FUNC_ENTRY("libSceIme", "sceImeGetPanelSize", ime_get_panel_size),
    HLE_FUNC_ENTRY("libSceIme", "sceImeSetText", ime_set_text),
    HLE_FUNC_ENTRY("libSceIme", "sceImeSetCaret", ime_set_caret),
    HLE_FUNC_ENTRY("libSceIme", "sceImeSetTextGeometry", ime_set_text_geometry),
};
void hle_register_input(void) {
    hle_register_table(exports, PS5C_ARRAY_COUNT(exports));
}
