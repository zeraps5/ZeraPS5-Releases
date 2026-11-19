/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pal/native_input.h"
#include "pal/pal.h"
#include <stdatomic.h>
#include <string.h>
#include <limits.h>

#define KEY_QUEUE 1024
#define MOUSE_QUEUE 256
static atomic_flag g_lock = ATOMIC_FLAG_INIT;
static bool g_keyboard, g_mouse, g_controller;
static native_key g_keys[KEY_QUEUE], g_held[256];
static native_mouse g_mice[MOUSE_QUEUE];
static unsigned g_kr, g_kw, g_mr, g_mw;
static uint32_t g_buttons;
static void lock(void) { while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) {} }
static void unlock(void) { atomic_flag_clear_explicit(&g_lock, memory_order_release); }
static void release_keys(void) {
    for (unsigned i = 0; i < 256; i++) if (g_held[i].code) {
        native_key key = g_held[i]; key.event = 258;
        key.status = key.character ? 3 : 1; key.time_us = pal_time_ns() / 1000;
        g_keys[g_kw++ % KEY_QUEUE] = key;
    }
    memset(g_held, 0, sizeof(g_held));
}
void native_input_configure(bool keyboard, bool mouse) {
    lock(); g_keyboard = keyboard; g_mouse = mouse; g_controller = false;
    g_kr = g_kw = g_mr = g_mw = g_buttons = 0;
    memset(g_held, 0, sizeof(g_held)); unlock();
}
void native_input_controller(bool connected) {
    lock();
    if (g_controller && !connected) {
        g_kr = g_kw = 0; release_keys();
    }
    g_controller = connected; unlock();
}
bool native_keyboard_enabled(void) { lock(); bool on = g_keyboard && g_controller; unlock(); return on; }
bool native_keyboard_requested(void) { lock(); bool on = g_keyboard; unlock(); return on; }
bool native_mouse_enabled(void) { lock(); bool on = g_mouse; unlock(); return on; }
void native_input_key(const native_key *key) {
    if (!key || !key->code || key->code >= 256) return;
    lock();
    if (g_keyboard && g_controller) {
        native_key value = *key;
        /* Preserve the down character on key-up even if modifiers/layout
         * changed. Quake uses characters for the letter keys on both edges. */
        if (value.event == 258 && !value.character && g_held[value.code].character) {
            value.character = g_held[value.code].character; value.status |= 2;
        }
        /* Overflow resets all held keys before accepting new input, so a lost
         * key-up cannot leave the game walking indefinitely. */
        if (g_kw - g_kr >= KEY_QUEUE - 256) { g_kr = g_kw = 0; release_keys(); }
        if (key->event == 258) memset(&g_held[key->code], 0, sizeof(native_key));
        else g_held[key->code] = *key;
        g_keys[g_kw++ % KEY_QUEUE] = value;
    }
    unlock();
}
static int32_t add_delta(int32_t a, int32_t b) {
    int64_t sum = (int64_t)a + b; return sum > INT32_MAX ? INT32_MAX : sum < INT32_MIN ? INT32_MIN : (int32_t)sum;
}
void native_input_mouse(const native_mouse *mouse) {
    if (!mouse) return;
    lock();
    if (g_mouse) {
        /* Merge motion between button transitions: 1000 Hz mice must not
         * build a queue of old aim positions behind a 30 Hz game. */
        if (g_mw != g_mr && g_mice[(g_mw-1) % MOUSE_QUEUE].buttons == mouse->buttons) {
            native_mouse *last = &g_mice[(g_mw-1) % MOUSE_QUEUE];
            last->x=add_delta(last->x,mouse->x); last->y=add_delta(last->y,mouse->y);
            last->wheel=add_delta(last->wheel,mouse->wheel); last->tilt=add_delta(last->tilt,mouse->tilt);
            last->time_us=mouse->time_us; unlock(); return;
        }
        if (g_mw - g_mr == MOUSE_QUEUE) g_mr++;
        g_mice[g_mw++ % MOUSE_QUEUE] = *mouse; g_buttons = mouse->buttons;
    }
    unlock();
}
void native_input_release(void) {
    lock();
    g_kr = g_kw = 0; release_keys();
    g_mr = g_mw = 0; g_buttons = 0;
    if (g_mouse) g_mice[g_mw++ % MOUSE_QUEUE] = (native_mouse){.time_us = pal_time_ns() / 1000};
    unlock();
}
bool native_input_read_key(native_key *key) {
    lock(); bool found = g_kr != g_kw;
    if (found) *key = g_keys[g_kr++ % KEY_QUEUE];
    unlock(); return found;
}
bool native_input_read_mouse(native_mouse *mouse) {
    lock(); bool found = g_mr != g_mw;
    *mouse = found ? g_mice[g_mr++ % MOUSE_QUEUE] : (native_mouse){.buttons = g_buttons, .time_us = pal_time_ns() / 1000};
    unlock(); return found;
}
void native_input_clear_keys(void) { lock(); g_kr = g_kw = 0; memset(g_held, 0, sizeof(g_held)); unlock(); }
void native_input_clear_mouse(void) { lock(); g_mr = g_mw = g_buttons = 0; unlock(); }
