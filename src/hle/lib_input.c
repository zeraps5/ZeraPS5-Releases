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
static void keyboard_update(hle_call *c) {
    hle_ret_s32(c, 0);
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
};
void hle_register_input(void) {
    hle_register_table(exports, PS5C_ARRAY_COUNT(exports));
}
