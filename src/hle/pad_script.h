/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_PAD_SCRIPT_H
#define PS5C_PAD_SCRIPT_H
#include "pal/pal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SCRIPT_TOUCH_LEFT (1u << 21)
#define SCRIPT_TOUCH_RIGHT (1u << 22)
#define SCRIPT_SHAKE (1u << 23)
#define SCRIPT_LS_UP (1u << 24)
#define SCRIPT_LS_DOWN (1u << 25)
#define SCRIPT_LS_LEFT (1u << 26)
#define SCRIPT_LS_RIGHT (1u << 27)
#define SCRIPT_RS_UP (1u << 28)
#define SCRIPT_RS_DOWN (1u << 29)
#define SCRIPT_RS_LEFT (1u << 30)
#define SCRIPT_RS_RIGHT (1u << 31)

/* Optional /seconds or /Nf (presented frames). Frame holds also expire after
 * 300 seconds if rendering stalls. Unknown or malformed entries are inert. */
static inline uint32_t pad_script_parse_hold(const char *text, size_t length, double *duration, uint32_t *flips) {
    static const struct { const char *name; uint32_t bit; } names[] = {
        {"cross", PAL_PAD_CROSS}, {"circle", PAL_PAD_CIRCLE}, {"square", PAL_PAD_SQUARE},
        {"triangle", PAL_PAD_TRIANGLE}, {"up", PAL_PAD_UP}, {"down", PAL_PAD_DOWN},
        {"left", PAL_PAD_LEFT}, {"right", PAL_PAD_RIGHT}, {"options", PAL_PAD_OPTIONS},
        {"touch", PAL_PAD_TOUCH_PAD}, {"l1", PAL_PAD_L1}, {"r1", PAL_PAD_R1},
        {"l2", PAL_PAD_L2}, {"r2", PAL_PAD_R2}, {"l3", PAL_PAD_L3}, {"r3", PAL_PAD_R3},
        {"ls_up", SCRIPT_LS_UP}, {"ls_down", SCRIPT_LS_DOWN},
        {"ls_left", SCRIPT_LS_LEFT}, {"ls_right", SCRIPT_LS_RIGHT},
        {"rs_up", SCRIPT_RS_UP}, {"rs_down", SCRIPT_RS_DOWN},
        {"rs_left", SCRIPT_RS_LEFT}, {"rs_right", SCRIPT_RS_RIGHT}, {"shake", SCRIPT_SHAKE},
        {"touch_l", SCRIPT_TOUCH_LEFT}, {"touch_r", SCRIPT_TOUCH_RIGHT},
    };
    *duration = 0.15;
    *flips = 0;
    const char *slash = memchr(text, '/', length);
    size_t name_length = slash ? (size_t)(slash - text) : length;
    if (slash) {
        size_t n = length - name_length - 1;
        char number[32], *end;
        if (!n || n >= sizeof(number)) return 0;
        memcpy(number, slash + 1, n); number[n] = 0;
        if (number[n - 1] == 'f') {
            if (n == 1) return 0;
            uint32_t count = 0;
            for (size_t i = 0; i + 1 < n; i++) {
                if (number[i] < '0' || number[i] > '9') return 0;
                count = count * 10 + (uint32_t)(number[i] - '0');
                if (count > 120) return 0;
            }
            if (!count) return 0;
            *flips = count;
            *duration = 300;
        } else {
            double seconds = strtod(number, &end);
            if (end != number + n || !isfinite(seconds) || seconds <= 0 || seconds > 300) return 0;
            *duration = seconds;
        }
    }
    for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); ++i)
        if (strlen(names[i].name) == name_length && !memcmp(text, names[i].name, name_length))
            return names[i].bit;
    return 0;
}

/* Compatibility helper for callers that only understand wall-clock holds. */
static inline uint32_t pad_script_parse(const char *text, size_t length, double *duration) {
    uint32_t flips;
    uint32_t buttons = pad_script_parse_hold(text, length, duration, &flips);
    return flips ? 0 : buttons;
}

static inline bool pad_script_held(double now, double start, double duration,
                                    uint32_t hold_flips, uint64_t first_flip,
                                    uint64_t flips, uint32_t reads) {
    if (now < start) return false;
    if (hold_flips) {
        /* Unlike legacy pulses, the hard deadline wins even when no second
         * sample arrives. Subtraction avoids overflow near UINT64_MAX. */
        return now - start < duration &&
               (flips < first_flip || flips - first_flip < hold_flips || reads < 2);
    }
    return now - start < duration || reads < 2;
}

/* Test input is overlaid after reading real devices. Internal bits never reach the guest. */
static inline void pad_script_apply(pal_pad_state *in, uint32_t script, uint64_t sample_ns) {
    in->buttons |= script & 0x1fffffu;
    /* a touch pad click with a finger on one half (PSPHD: left SELECT, right START) */
    if ((script & (SCRIPT_TOUCH_LEFT | SCRIPT_TOUCH_RIGHT)) && in->touch_count < 2) {
        in->buttons |= PAL_PAD_TOUCH_PAD;
        in->touch[in->touch_count].x = script & SCRIPT_TOUCH_LEFT ? 480 : 1440;
        in->touch[in->touch_count].y = 470;
        in->touch[in->touch_count].id = 0x7f;
        in->touch_count++;
    }
    if (script & PAL_PAD_L2) in->l2 = 255;
    if (script & PAL_PAD_R2) in->r2 = 255;
    if (script & SCRIPT_LS_UP) in->ly = 0;
    if (script & SCRIPT_LS_DOWN) in->ly = 255;
    if (script & SCRIPT_LS_LEFT) in->lx = 0;
    if (script & SCRIPT_LS_RIGHT) in->lx = 255;
    if (script & SCRIPT_RS_UP) in->ry = 0;
    if (script & SCRIPT_RS_DOWN) in->ry = 255;
    if (script & SCRIPT_RS_LEFT) in->rx = 0;
    if (script & SCRIPT_RS_RIGHT) in->rx = 255;
    if (script & SCRIPT_SHAKE) {
        /* Alternating acceleration in G and angular velocity in rad/s, not a game-state shortcut. */
        float sign = (sample_ns / 100000000ull) & 1 ? 1.0f : -1.0f;
        in->motion = true;
        in->accel[0] = sign * 3.0f; in->accel[1] = 1.0f; in->accel[2] = -sign * 2.0f;
        in->gyro[0] = sign * 8.0f; in->gyro[1] = -sign * 6.0f; in->gyro[2] = 0;
    }
}
#endif
