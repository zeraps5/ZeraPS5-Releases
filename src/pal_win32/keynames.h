/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Keyboard key names of the user settings ([teclado] in zeraps5.ini) and
 * their Windows virtual-key codes; letters and digits are their own names.
 * Shared by the input code and the launcher. */
#ifndef PS5C_KEYNAMES_H
#define PS5C_KEYNAMES_H

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const struct {
    const char *name;
    int vk;
} k_key_names[] = {
    {"UP", 0x26},        {"DOWN", 0x28},     {"LEFT", 0x25},    {"RIGHT", 0x27},   {"SPACE", 0x20},
    {"ENTER", 0x0D},     {"BACKSPACE", 0x08}, {"TAB", 0x09},    {"ESC", 0x1B},     {"SHIFT", 0x10},
    {"CTRL", 0x11},      {"ALT", 0x12},      {"INSERT", 0x2D},  {"DELETE", 0x2E},  {"HOME", 0x24},
    {"END", 0x23},       {"PAGEUP", 0x21},   {"PAGEDOWN", 0x22}, {"NUM0", 0x60},   {"NUM1", 0x61},
    {"NUM2", 0x62},      {"NUM3", 0x63},     {"NUM4", 0x64},    {"NUM5", 0x65},    {"NUM6", 0x66},
    {"NUM7", 0x67},      {"NUM8", 0x68},     {"NUM9", 0x69},    {"F1", 0x70},      {"F2", 0x71},
    {"F3", 0x72},        {"F4", 0x73},       {"F5", 0x74},      {"F6", 0x75},      {"F7", 0x76},
    {"F8", 0x77},        {"F9", 0x78},       {"F10", 0x79},     {"F11", 0x7A},     {"F12", 0x7B},
};

/* Virtual-key code of a key name (any case), 0 when unknown */
static inline int key_from_name(const char *name) {
    if (name[0] && !name[1] && isalnum((unsigned char)name[0])) return toupper((unsigned char)name[0]);
    for (size_t i = 0; i < sizeof(k_key_names) / sizeof(k_key_names[0]); i++) {
        const char *a = k_key_names[i].name, *b = name;
        while (*a && toupper((unsigned char)*b) == *a) a++, b++;
        if (!*a && !*b) return k_key_names[i].vk;
    }
    return 0;
}

/* Name of a virtual-key code into out (empty when it has none) */
static inline void key_name(int vk, char *out, size_t cap) {
    out[0] = 0;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        snprintf(out, cap, "%c", vk);
        return;
    }
    for (size_t i = 0; i < sizeof(k_key_names) / sizeof(k_key_names[0]); i++) {
        if (k_key_names[i].vk == vk) {
            snprintf(out, cap, "%s", k_key_names[i].name);
            return;
        }
    }
}

#endif
