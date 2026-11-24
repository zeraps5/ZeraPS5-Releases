/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceCamera: the PlayStation Camera, fed by a host camera (pal_camera.h).
 * Which one is a setting: [acessorios] camera = desligada (default), generica (the
 * first webcam), ps4 (the PS4 camera through Sony's adapter) or part of a camera's
 * name; PS5C_CAMERA overrides it (0, 1 or a name). Off, no camera is attached, as
 * before.
 *
 * The PS camera has two lenses: a PS4 Camera gives both (device 0 left, 1 right),
 * another host camera the same picture to both, so depth does not work. Frames are YUV 4:2:2 (Y0 U Y1 V): 1280x800 at format level 0,
 * then halves (640x400, 320x200, 160x100) at levels 1-3.
 *
 * Structures (Tearaway Unfolded reads them so): SceCameraConfig = { u32 sizeThis
 * (0x68); u32 configType; 2 x { u32 format[4]; u32 resolution, framerate, width,
 * height, reserved; void *option } }; SceCameraStartParameter = { u32 sizeThis (0x18);
 * u32 formatLevel[2] (bit n: level n); void *option }; SceCameraFrameData = { u32
 * sizeThis (0x208); u32 readMode; framePosition[2][4] {x, y, xSize, ySize} at 0x08;
 * void *pFramePointerList[2][4] at 0x88; u32 frameSize[2][4] at 0xc8; u32 status[2]
 * at 0xe8; meta at 0xf0: u32 metaMode, u32 format[2][4], u64 frame[2] at 0x118, u64
 * timestamp[2] at 0x128 ... }. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "core/settings.h"
#include "gpu/gpu_watch.h"
#include "hle/hle.h"
#include "pal/pal.h"
#include "pal/pal_camera.h"

void hle_register_table(const hle_export *table, size_t count);

#define CAMERA_ERR_PARAM ((int32_t)0x802E0000)
#define CAMERA_ERR_ALREADY_OPEN ((int32_t)0x802E0002)
#define CAMERA_ERR_NOT_OPEN ((int32_t)0x802E0003)
#define CAMERA_ERR_FATAL ((int32_t)0x802E00FF)
#define CAMERA_ERR_NO_FRAME ((int32_t)0x802E0006) /* no picture to hand out yet */
#define CAMERA_ERR_NOT_CONNECTED ((int32_t)0x802E0010) /* Tearaway closes the camera on it */

#define DEVICES 2
#define LEVELS 4
#define HANDLE 1
#define LEVEL0_W 1280u
#define LEVEL0_H 800u

static struct {
    pal_mutex *lock;
    bool opened, started;
    pal_camera *cam;
    char want[128];     /* the setting: "" off, "*" the first webcam, else part of a name */
    guest_addr frames;  /* guest buffers: [device][level] at level_off + device * eye_size */
    uint64_t level_off[LEVELS], eye_size;
    uint64_t seq, delivered;
    bool stereo;        /* the last pictures came from two lenses */
    uint8_t *picture[DEVICES]; /* level 0, YUYV */
    int64_t started_us;
} g_cam;

static uint32_t level_w(uint32_t l) { return LEVEL0_W >> l; }
static uint32_t level_h(uint32_t l) { return LEVEL0_H >> l; }

/* The setting, read once */
static const char *wanted(void) {
    static bool read;
    if (read) return g_cam.want;
    read = true;
    const char *v = getenv("PS5C_CAMERA");
    if (!v || !*v) v = settings_get("acessorios", "camera");
    if (!v || !*v || !strcmp(v, "0") || !strcmp(v, "desligada") || !strcmp(v, "off")) g_cam.want[0] = 0;
    else if (!strcmp(v, "1") || !strcmp(v, "generica") || !strcmp(v, "generic")) strcpy(g_cam.want, "*");
    else if (!strcmp(v, "ps4")) strcpy(g_cam.want, "OV580");
    else snprintf(g_cam.want, sizeof(g_cam.want), "%s", v);
    if (g_cam.want[0]) LOG_I(HLE, "camera: PlayStation Camera from the host camera '%s'", g_cam.want);
    return g_cam.want;
}

/* A host camera to use is present (listed); opening it starts the capture */
static bool host_camera_present(void) {
    const char *w = wanted();
    if (!*w) return false;
    if (g_cam.cam) return true;
    char names[16][128];
    uint32_t n = pal_camera_list(names, 16);
    for (uint32_t i = 0; i < n && i < 16; i++) {
        bool ps4 = strstr(names[i], "OV580") != NULL;
        if (!strcmp(w, "*") ? !ps4 : strstr(names[i], w) != NULL) return true;
    }
    /* a PS4 Camera in USB Boot: opening it loads the firmware */
    return strstr(w, "OV580") && pal_camera_ps4_waiting();
}

/* int sceCameraIsAttached(int32 index): 1 attached, 0 not */
static void camera_is_attached(hle_call *c) {
    int32_t index = hle_arg_s32(c, 0);
    pal_mutex_lock(g_cam.lock);
    static int64_t checked_us = -1;
    static bool present;
    int64_t now = (int64_t)(pal_time_ns() / 1000);
    /* games poll it every few seconds; listing the devices is slow-ish */
    if (checked_us < 0 || now - checked_us > 2000000) present = host_camera_present(), checked_us = now;
    pal_mutex_unlock(g_cam.lock);
    hle_ret_s32(c, index == 0 && present ? 1 : 0);
}

/* int sceCameraOpen(userId, int32 type, int32 index, SceCameraOpenParameter *): the handle */
static void camera_open(hle_call *c) {
    int32_t index = hle_arg_s32(c, 2);
    pal_mutex_lock(g_cam.lock);
    int32_t ret;
    if (index != 0 || !*wanted()) ret = CAMERA_ERR_NOT_CONNECTED;
    else if (g_cam.opened) ret = CAMERA_ERR_ALREADY_OPEN;
    else {
        if (!g_cam.frames) {
            uint64_t size = 0;
            for (uint32_t l = 0; l < LEVELS; l++) g_cam.level_off[l] = size, size += (uint64_t)level_w(l) * level_h(l) * 2;
            g_cam.eye_size = size;
            size = (size * DEVICES + 0xFFFF) & ~0xFFFFull;
            for (uint32_t d = 0; d < DEVICES; d++) g_cam.picture[d] = (uint8_t *)malloc((size_t)LEVEL0_W * LEVEL0_H * 2);
            if (!g_cam.picture[0] || !g_cam.picture[1] ||
                gmem_map(c->mem, 0, size, GPROT_RW, 0, "camera frames", &g_cam.frames) != PS5C_OK)
                g_cam.frames = 0;
        }
        g_cam.opened = g_cam.frames != 0;
        ret = g_cam.opened ? HANDLE : CAMERA_ERR_FATAL;
    }
    pal_mutex_unlock(g_cam.lock);
    LOG_I(HLE, "sceCameraOpen(user %d, type %d, index %d) -> 0x%x", hle_arg_s32(c, 0), hle_arg_s32(c, 1), index, ret);
    hle_ret_s32(c, ret);
}

static void stop_capture(void) {
    pal_camera_close(g_cam.cam);
    g_cam.cam = NULL;
    g_cam.started = false;
}

static void camera_close(hle_call *c) {
    pal_mutex_lock(g_cam.lock);
    int32_t ret = g_cam.opened && hle_arg_s32(c, 0) == HANDLE ? 0 : CAMERA_ERR_NOT_OPEN;
    if (!ret) stop_capture(), g_cam.opened = false;
    pal_mutex_unlock(g_cam.lock);
    hle_ret_s32(c, ret);
}

/* int sceCameraSetConfig(handle, SceCameraConfig *): the pictures are always the
 * 1280x800 YUV 4:2:2 set; the asked configuration is logged */
static void camera_set_config(hle_call *c) {
    uint32_t cfg[4] = {0};
    if (hle_arg_ptr(c, 1)) gmem_read(c->mem, hle_arg_ptr(c, 1), cfg, sizeof(cfg));
    LOG_I(HLE, "sceCameraSetConfig: size 0x%x, type %u, format %u %u", cfg[0], cfg[1], cfg[2], cfg[3]);
    hle_ret_s32(c, g_cam.opened && hle_arg_s32(c, 0) == HANDLE ? 0 : CAMERA_ERR_NOT_OPEN);
}

static void camera_start(hle_call *c) {
    pal_mutex_lock(g_cam.lock);
    int32_t ret = g_cam.opened && hle_arg_s32(c, 0) == HANDLE ? 0 : CAMERA_ERR_NOT_OPEN;
    if (!ret && !g_cam.cam) {
        const char *w = wanted();
        g_cam.cam = pal_camera_open(!strcmp(w, "*") ? NULL : w);
        if (!g_cam.cam) ret = CAMERA_ERR_NOT_CONNECTED;
    }
    if (!ret) g_cam.started = true, g_cam.started_us = (int64_t)(pal_time_ns() / 1000);
    pal_mutex_unlock(g_cam.lock);
    hle_ret_s32(c, ret);
}

static void camera_stop(hle_call *c) {
    pal_mutex_lock(g_cam.lock);
    int32_t ret = g_cam.opened && hle_arg_s32(c, 0) == HANDLE ? 0 : CAMERA_ERR_NOT_OPEN;
    if (!ret) stop_capture();
    pal_mutex_unlock(g_cam.lock);
    hle_ret_s32(c, ret);
}

/* Halves a YUYV picture (2x2 averages) */
static void halve(const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst) {
    uint32_t dw = w / 2, dh = h / 2;
    for (uint32_t y = 0; y < dh; y++) {
        const uint8_t *a = src + (size_t)(2 * y) * w * 2, *b = a + (size_t)w * 2;
        uint8_t *o = dst + (size_t)y * dw * 2;
        for (uint32_t x = 0; x < dw; x += 2) { /* out pixels x, x+1 from in 2x..2x+3 */
            const uint8_t *p = a + (size_t)x * 4, *q = b + (size_t)x * 4;
            o[0] = (uint8_t)((p[0] + p[2] + q[0] + q[2] + 2) / 4);
            o[2] = (uint8_t)((p[4] + p[6] + q[4] + q[6] + 2) / 4);
            o[1] = (uint8_t)((p[1] + p[5] + q[1] + q[5] + 2) / 4);
            o[3] = (uint8_t)((p[3] + p[7] + q[3] + q[7] + 2) / 4);
            o += 4;
        }
    }
}

/* int sceCameraGetFrameData(handle, SceCameraFrameData *): waits up to a frame for a new
 * picture (the game's camera thread polls it), then hands out the newest one */
static void camera_get_frame_data(hle_call *c) {
    guest_addr fd = hle_arg_ptr(c, 1);
    uint32_t size = 0;
    if (!fd || !gmem_read(c->mem, fd, &size, 4) || size < 0x130) {
        hle_ret_s32(c, CAMERA_ERR_PARAM);
        return;
    }
    pal_mutex_lock(g_cam.lock);
    if (!g_cam.opened || hle_arg_s32(c, 0) != HANDLE || !g_cam.started || !g_cam.cam) {
        pal_mutex_unlock(g_cam.lock);
        hle_ret_s32(c, g_cam.opened ? CAMERA_ERR_NOT_CONNECTED : CAMERA_ERR_NOT_OPEN);
        return;
    }
    pal_camera *cam = g_cam.cam;
    uint64_t seq = g_cam.seq, old = seq;
    pal_mutex_unlock(g_cam.lock);
    /* the capture keeps running; the lock is not held while waiting */
    bool have = pal_camera_read_eye(cam, 0, g_cam.picture[0], LEVEL0_W, LEVEL0_H, &seq, 40);
    bool stereo = have && seq != old && pal_camera_stereo(cam);
    if (stereo) { /* the right lens of the same (or a newer) capture, without waiting */
        uint64_t right = old;
        stereo = pal_camera_read_eye(cam, 1, g_cam.picture[1], LEVEL0_W, LEVEL0_H, &right, 0) && right != old;
    }
    pal_mutex_lock(g_cam.lock);
    if (g_cam.cam != cam) { /* stopped meanwhile */
        pal_mutex_unlock(g_cam.lock);
        hle_ret_s32(c, CAMERA_ERR_NOT_CONNECTED);
        return;
    }
    if (have && seq != g_cam.seq) {
        uint32_t eyes = stereo ? DEVICES : 1;
        uint8_t *all = (uint8_t *)hle_mem(c, g_cam.frames, g_cam.eye_size * eyes);
        /* not a guest instruction: the GPU's write tracking would keep the first picture */
        if (all) gpu_watch_prepare_write(g_cam.frames, g_cam.eye_size * eyes);
        for (uint32_t d = 0; all && d < eyes; d++) {
            uint8_t *base = all + d * g_cam.eye_size;
            memcpy(base, g_cam.picture[d], (size_t)LEVEL0_W * LEVEL0_H * 2);
            for (uint32_t l = 1; l < LEVELS; l++) halve(base + g_cam.level_off[l - 1], level_w(l - 1), level_h(l - 1), base + g_cam.level_off[l]);
        }
        g_cam.stereo = stereo;
        g_cam.seq = seq;
        g_cam.delivered++;
    }
    if (!g_cam.delivered) { /* no picture yet: games copy from the frame pointers on success */
        pal_mutex_unlock(g_cam.lock);
        hle_ret_s32(c, CAMERA_ERR_NO_FRAME);
        return;
    }
    uint8_t out[0x208];
    memset(out, 0, sizeof(out));
    uint32_t n = size < sizeof(out) ? size : (uint32_t)sizeof(out);
    gmem_read(c->mem, fd, out, 8); /* sizeThis, readMode */
    for (uint32_t d = 0; d < DEVICES; d++)
        for (uint32_t l = 0; l < LEVELS; l++) {
            uint32_t pos[4] = {0, 0, level_w(l), level_h(l)}, bytes = level_w(l) * level_h(l) * 2;
            guest_addr p = g_cam.frames + (g_cam.stereo ? d * g_cam.eye_size : 0) + g_cam.level_off[l];
            memcpy(out + 0x08 + (d * LEVELS + l) * 16, pos, 16);
            memcpy(out + 0x88 + (d * LEVELS + l) * 8, &p, 8);
            memcpy(out + 0xc8 + (d * LEVELS + l) * 4, &bytes, 4);
        }
    uint64_t frame = g_cam.delivered, stamp = (uint64_t)((int64_t)(pal_time_ns() / 1000) - g_cam.started_us);
    for (uint32_t d = 0; d < DEVICES; d++) {
        memcpy(out + 0x118 + d * 8, &frame, 8);
        memcpy(out + 0x128 + d * 8, &stamp, 8);
    }
    pal_mutex_unlock(g_cam.lock);
    gmem_write(c->mem, fd, out, n);
    hle_ret_s32(c, 0);
}

#define CAM "libSceCamera"
static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(CAM, "sceCameraIsAttached", camera_is_attached),
    HLE_FUNC_ENTRY(CAM, "sceCameraOpen", camera_open),
    HLE_FUNC_ENTRY(CAM, "sceCameraClose", camera_close),
    HLE_FUNC_ENTRY(CAM, "sceCameraSetConfig", camera_set_config),
    HLE_FUNC_ENTRY(CAM, "sceCameraStart", camera_start),
    HLE_FUNC_ENTRY(CAM, "sceCameraStop", camera_stop),
    HLE_FUNC_ENTRY(CAM, "sceCameraGetFrameData", camera_get_frame_data),
};

void hle_register_camera(void) {
    if (!g_cam.lock && pal_mutex_create(&g_cam.lock) != PS5C_OK) {
        LOG_E(HLE, "cannot create the camera lock");
        return;
    }
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}
