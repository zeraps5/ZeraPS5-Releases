/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PlayStation 4 Camera (CUH-ZEY1/ZEY2) on the host, through Sony's USB adapter.
 * Powered up, the camera is a loader device (USB 05A9:0580, "USB Boot") that
 * needs its firmware sent over USB before it works as a stereo camera
 * (05A9:058B, "USB Camera-OV580"; 058A on some units). The firmware is the user's own file (user/ps4_camera_firmware.bin),
 * never part of this project. On Windows the loader device needs the WinUSB
 * driver (Zadig). */
#ifndef PS5C_PS4_CAMERA_H
#define PS5C_PS4_CAMERA_H

#include <stddef.h>
#include <stdint.h>

typedef enum ps4_camera_state {
    PS4_CAMERA_ABSENT = 0,  /* neither device present */
    PS4_CAMERA_LOADER,      /* 05A9:0580 present, firmware not loaded */
    PS4_CAMERA_LOADER_NO_DRIVER, /* 05A9:0580 present without WinUSB */
    PS4_CAMERA_READY,       /* 05A9:058B/058A: firmware running */
} ps4_camera_state;

ps4_camera_state ps4_camera_query(void);
const char *ps4_camera_state_name(ps4_camera_state s);
/* Sends `fw` to the loader device; the camera then restarts as a UVC camera.
 * Returns 0, or a negative step number where it failed (message in the log). */
int ps4_camera_load_firmware(const uint8_t *fw, size_t size);

#endif
