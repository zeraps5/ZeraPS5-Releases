/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Hosts without camera capture (pal_camera.h): no cameras. */
#include "pal/pal_camera.h"

#include <stddef.h>

uint32_t pal_camera_list(char (*names)[128], uint32_t max) {
    (void)names;
    (void)max;
    return 0;
}

pal_camera *pal_camera_open(const char *match) {
    (void)match;
    return NULL;
}

void pal_camera_close(pal_camera *c) { (void)c; }

bool pal_camera_read(pal_camera *c, uint8_t *dst, uint32_t w, uint32_t h, uint64_t *seq, uint32_t wait_ms) {
    (void)c, (void)dst, (void)w, (void)h, (void)seq, (void)wait_ms;
    return false;
}

bool pal_camera_read_eye(pal_camera *c, uint32_t eye, uint8_t *dst, uint32_t w, uint32_t h, uint64_t *seq,
                         uint32_t wait_ms) {
    (void)eye;
    return pal_camera_read(c, dst, w, h, seq, wait_ms);
}

bool pal_camera_stereo(const pal_camera *c) {
    (void)c;
    return false;
}

bool pal_camera_ps4_waiting(void) { return false; }
