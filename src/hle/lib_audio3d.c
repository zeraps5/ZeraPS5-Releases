/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceAudio3d: the console's 3D audio service. A game opens one port,
 * reserves object ids on it and sets per-object attributes; the audio of the
 * objects reaches the speakers through the port's push/advance calls. We keep
 * the port and object bookkeeping with the console's argument checks, but mix
 * no audio: the games seen so far (3D MiniGolf) import no push call.
 *
 * Argument checks, error codes and constants follow the AnyPS5 project's
 * libSceAudio3d (boykopovar/AnyPS5, GPL-2.0). */
#include <string.h>

#include "base/log.h"
#include "hle/hle.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

#define A3D_ERROR_INVALID_PORT ((int32_t)0x80EA0002)
#define A3D_ERROR_INVALID_OBJECT ((int32_t)0x80EA0003)
#define A3D_ERROR_INVALID_PARAMETER ((int32_t)0x80EA0004)
#define A3D_ERROR_OUT_OF_RESOURCES ((int32_t)0x80EA0006)
#define A3D_ERROR_NOT_READY ((int32_t)0x80EA0007)

#define A3D_USER_SYSTEM 0xFFu
#define A3D_PORT_ID 0u
#define A3D_OBJECT_ID_MASK 0x7FFFFFFFu
#define A3D_BUFFER_MODE_ADVANCE_AND_PUSH 2u

static pal_mutex *g_lock;
static bool g_init, g_port_open;
static uint32_t g_max_objects, g_objects, g_next_object;

/* int sceAudio3dInitialize(int64_t reserved) */
static void a3d_initialize(hle_call *c) {
    if (hle_arg(c, 0) != 0) {
        hle_ret_s32(c, A3D_ERROR_INVALID_PARAMETER);
        return;
    }
    pal_mutex_lock(g_lock);
    g_init = true;
    pal_mutex_unlock(g_lock);
    hle_ret_s32(c, 0);
}

/* int sceAudio3dTerminate(void) */
static void a3d_terminate(hle_call *c) {
    int32_t r = 0;
    pal_mutex_lock(g_lock);
    if (!g_init || g_port_open) r = A3D_ERROR_NOT_READY;
    else g_init = false;
    pal_mutex_unlock(g_lock);
    hle_ret_s32(c, r);
}

/* int sceAudio3dPortOpen(SceUserServiceUserId userId,
 *                        const SceAudio3dOpenParameters *params, SceAudio3dPortId *id)
 * params: {size_t size_this; uint32 granularity; uint32 rate; uint32 max_objects;
 *          uint32 queue_depth; uint32 buffer_mode; ...; uint32 num_beds at 0x20} */
static void a3d_port_open(hle_call *c) {
    uint32_t user = hle_arg_u32(c, 0);
    guest_addr pp = hle_arg_ptr(c, 1), idp = hle_arg_ptr(c, 2);
    const uint8_t *p = pp ? (const uint8_t *)hle_mem(c, pp, 0x20) : NULL;
    uint32_t *id = idp ? (uint32_t *)hle_mem(c, idp, 4) : NULL;
    int32_t r = 0;
    if (!p || !id) {
        hle_ret_s32(c, A3D_ERROR_INVALID_PARAMETER);
        return;
    }
    uint64_t size_this;
    uint32_t granularity, rate, max_objects, queue_depth, buffer_mode;
    memcpy(&size_this, p, 8);
    memcpy(&granularity, p + 8, 4);
    memcpy(&rate, p + 12, 4);
    memcpy(&max_objects, p + 16, 4);
    memcpy(&queue_depth, p + 20, 4);
    memcpy(&buffer_mode, p + 24, 4);
    if (user != A3D_USER_SYSTEM || (size_this != 0x20 && size_this != 0x28) || rate != 0 ||
        granularity < 0x100 || (granularity & 0xFF) || !max_objects || !queue_depth ||
        buffer_mode != A3D_BUFFER_MODE_ADVANCE_AND_PUSH) {
        LOG_W(HLE, "sceAudio3dPortOpen: rejected user=0x%x size=0x%llx gran=%u rate=%u objs=%u depth=%u mode=%u",
              user, (unsigned long long)size_this, granularity, rate, max_objects, queue_depth, buffer_mode);
        hle_ret_s32(c, A3D_ERROR_INVALID_PARAMETER);
        return;
    }
    pal_mutex_lock(g_lock);
    if (!g_init) r = A3D_ERROR_NOT_READY;
    else if (g_port_open) r = A3D_ERROR_OUT_OF_RESOURCES;
    else {
        g_port_open = true;
        g_max_objects = max_objects;
        g_objects = 0;
        g_next_object = 0;
        *id = A3D_PORT_ID;
    }
    pal_mutex_unlock(g_lock);
    LOG_I(HLE, "sceAudio3dPortOpen: granularity %u, %u objects, depth %u -> 0x%x (no 3D mixing)",
          granularity, max_objects, queue_depth, (uint32_t)r);
    hle_ret_s32(c, r);
}

static bool port_ok(uint32_t port) { return g_init && g_port_open && port == A3D_PORT_ID; }

/* int sceAudio3dPortClose(SceAudio3dPortId port) */
static void a3d_port_close(hle_call *c) {
    int32_t r = 0;
    pal_mutex_lock(g_lock);
    if (!port_ok(hle_arg_u32(c, 0))) r = A3D_ERROR_INVALID_PORT;
    else g_port_open = false;
    pal_mutex_unlock(g_lock);
    hle_ret_s32(c, r);
}

/* int sceAudio3dPortFlush(SceAudio3dPortId port): nothing is queued */
static void a3d_port_flush(hle_call *c) {
    int32_t r = 0;
    pal_mutex_lock(g_lock);
    if (!port_ok(hle_arg_u32(c, 0))) r = A3D_ERROR_INVALID_PORT;
    pal_mutex_unlock(g_lock);
    hle_ret_s32(c, r);
}

/* int sceAudio3dObjectReserve(SceAudio3dPortId port, SceAudio3dObjectId *id) */
static void a3d_object_reserve(hle_call *c) {
    guest_addr idp = hle_arg_ptr(c, 1);
    uint32_t *id = idp ? (uint32_t *)hle_mem(c, idp, 4) : NULL;
    int32_t r = 0;
    pal_mutex_lock(g_lock);
    if (!port_ok(hle_arg_u32(c, 0))) r = A3D_ERROR_INVALID_PORT;
    else if (!id) r = A3D_ERROR_INVALID_PARAMETER;
    else if (g_objects >= g_max_objects) r = A3D_ERROR_OUT_OF_RESOURCES;
    else {
        *id = g_next_object++ & A3D_OBJECT_ID_MASK;
        g_objects++;
    }
    pal_mutex_unlock(g_lock);
    hle_ret_s32(c, r);
}

/* int sceAudio3dObjectSetAttributes(SceAudio3dPortId port, SceAudio3dObjectId obj,
 *                                   size_t count, const SceAudio3dAttribute *attrs) */
static void a3d_object_set_attributes(hle_call *c) {
    uint32_t obj = hle_arg_u32(c, 1);
    uint64_t count = hle_arg(c, 2);
    int32_t r = 0;
    pal_mutex_lock(g_lock);
    if (!port_ok(hle_arg_u32(c, 0))) r = A3D_ERROR_INVALID_PORT;
    else if (obj > A3D_OBJECT_ID_MASK || obj >= g_next_object) r = A3D_ERROR_INVALID_OBJECT;
    else if (count && !hle_arg_ptr(c, 3)) r = A3D_ERROR_INVALID_PARAMETER;
    pal_mutex_unlock(g_lock);
    hle_ret_s32(c, r);
}

void hle_register_audio3d(void) {
    static const hle_export table[] = {
        HLE_FUNC_ENTRY("libSceAudio3d", "sceAudio3dInitialize", a3d_initialize),
        HLE_FUNC_ENTRY("libSceAudio3d", "sceAudio3dTerminate", a3d_terminate),
        HLE_FUNC_ENTRY("libSceAudio3d", "sceAudio3dPortOpen", a3d_port_open),
        HLE_FUNC_ENTRY("libSceAudio3d", "sceAudio3dPortClose", a3d_port_close),
        HLE_FUNC_ENTRY("libSceAudio3d", "sceAudio3dPortFlush", a3d_port_flush),
        HLE_FUNC_ENTRY("libSceAudio3d", "sceAudio3dObjectReserve", a3d_object_reserve),
        HLE_FUNC_ENTRY("libSceAudio3d", "sceAudio3dObjectSetAttributes", a3d_object_set_attributes),
    };
    if (!g_lock && pal_mutex_create(&g_lock) != PS5C_OK) {
        LOG_E(HLE, "libSceAudio3d: no lock");
        return;
    }
    hle_register_table(table, PS5C_ARRAY_COUNT(table));
}
