/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_NATIVE_INPUT_H
#define PS5C_NATIVE_INPUT_H
#include "base/base.h"

/* Native USB-style input for games that implement it. Keyboard requires a
 * physical controller; keyboard-to-pad remains the fallback. */
typedef struct native_key {
    uint16_t code, character; /* USB HID usage, UTF-16 */
    uint32_t status, type, event; /* event: 257 down, 258 up, 259 repeat */
    uint64_t time_us;
} native_key;
typedef struct native_mouse {
    uint32_t buttons; /* left=1, right=2, middle=4, back=8, forward=16 */
    int32_t x, y, wheel, tilt;
    uint64_t time_us;
} native_mouse;
void native_input_configure(bool keyboard, bool mouse);
void native_input_controller(bool connected);
bool native_keyboard_enabled(void);
bool native_keyboard_requested(void);
bool native_mouse_enabled(void);
/* Window thread producers; bounded queues, no background/global hooks. */
void native_input_key(const native_key *key);
void native_input_mouse(const native_mouse *mouse);
void native_input_release(void); /* focus/capture lost: releases held keys/buttons */
bool native_input_read_key(native_key *key);
bool native_input_read_mouse(native_mouse *mouse);
void native_input_clear_keys(void);
void native_input_clear_mouse(void);
#endif
