/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host cameras as a source of pictures for the emulated PlayStation Camera.
 * A capture thread keeps the newest picture of one host camera; readers get it
 * scaled to the size they ask for, as YUV 4:2:2 (Y0 U Y1 V, what the PS4 camera
 * library hands out). A PS4 Camera (USB Camera-OV580, through Sony's adapter)
 * gives each lens its own 1280x800 picture; with an ordinary webcam both "eyes"
 * see the same picture, so depth (stereo) features cannot work. */
#ifndef PS5C_PAL_CAMERA_H
#define PS5C_PAL_CAMERA_H

#include <stdbool.h>
#include <stdint.h>

typedef struct pal_camera pal_camera;

/* Names of the host cameras (UTF-8), at most `max`; returns how many there are. */
uint32_t pal_camera_list(char (*names)[128], uint32_t max);

/* Opens the first camera whose name contains `match` (case-insensitive), or with
 * NULL/"" the first ordinary webcam (not a PS4 camera, whose picture is two lenses
 * side by side). Capture starts at once. NULL: none, or no Media Foundation. */
pal_camera *pal_camera_open(const char *match);
void pal_camera_close(pal_camera *c);

/* The newest picture as YUYV, w x h (w even), cropped to that aspect ratio from
 * the middle. Waits up to wait_ms for a picture newer than *seq; *seq gets the
 * picture's number (0: none yet, dst untouched). false: no picture at all. */
bool pal_camera_read(pal_camera *c, uint8_t *dst, uint32_t w, uint32_t h, uint64_t *seq, uint32_t wait_ms);
/* The same for one lens: 0 left, 1 right. A PS4 Camera's lenses come whole (1280x800,
 * scaled for other sizes, never cropped); other cameras give both the same picture. */
bool pal_camera_read_eye(pal_camera *c, uint32_t eye, uint8_t *dst, uint32_t w, uint32_t h, uint64_t *seq,
                         uint32_t wait_ms);
/* c is a PS4 Camera: two lenses */
bool pal_camera_stereo(const pal_camera *c);
/* A PS4 Camera is plugged in but still waits for its firmware (USB Boot):
 * pal_camera_open("OV580") will load it */
bool pal_camera_ps4_waiting(void);

#endif
