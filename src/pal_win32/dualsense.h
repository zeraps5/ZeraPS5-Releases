/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Sony controllers read straight from HID (Windows has no XInput driver for
 * them): DualSense and DualSense Edge over USB or Bluetooth, DualShock 4 over
 * USB. A thread keeps the newest report and sends rumble and light bar
 * changes; whoever reads does not wait on the device. Shared by the emulator
 * (pal_win32) and the launcher's controller screen. */
#ifndef PS5C_PAL_WIN32_DUALSENSE_H
#define PS5C_PAL_WIN32_DUALSENSE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum ds_kind { DS_NONE, DS_DUALSENSE, DS_DUALSENSE_EDGE, DS_DUALSHOCK4 } ds_kind;

typedef struct ds_state {
    ds_kind kind;
    bool bluetooth;
    uint32_t buttons;       /* PAL_PAD_* bits */
    bool ps, create, mute;  /* buttons the game never sees (the system's) */
    uint8_t lx, ly, rx, ry; /* 0..255, 128 centered, y grows downwards */
    uint8_t l2, r2;
    struct {
        bool down;
        uint8_t id;
        uint16_t x, y;
    } touch[2];
    uint16_t touch_w, touch_h; /* touchpad resolution */
    float accel[3];            /* G */
    float gyro[3];             /* rad/s */
    uint8_t battery;           /* 0..100 */
    bool charging;
    uint32_t reports; /* input reports received so far (changes while it streams) */
} ds_state;

/* Starts the reader thread (once); it finds the controller and reconnects when it comes back. */
void ds_start(void);
/* Ends the reader thread and lets go of the controller (the launcher's screen closing). */
void ds_stop(void);
/* The newest state; false (and kind DS_NONE) while no controller is connected. */
bool ds_read(ds_state *out);
/* Rumble (large: left, low frequency; small: right) and light bar color. */
void ds_set_rumble(uint8_t large, uint8_t small);
void ds_set_light(uint8_t r, uint8_t g, uint8_t b);
/* Stops the motors (sent at once): process exit, a game closing. */
void ds_stop_output(void);
/* Name for users: "DualSense (USB)", ... "" when none. */
const char *ds_name(const ds_state *s);

#endif
