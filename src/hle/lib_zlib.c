/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceZlib: the console's zlib decompression service. A request inflates one
 * whole zlib stream into a destination of up to 64 KiB (a multiple of 2 KiB);
 * the game collects finished request ids with sceZlibWaitForDone and their
 * output size and status with sceZlibGetResult. We decode at once, so every
 * request is finished when sceZlibInflate returns.
 *
 * Argument checks, error codes and the status values follow the AnyPS5
 * project's libSceZlib (boykopovar/AnyPS5, GPL-2.0); the decoder is ours
 * (png_decode.c). */
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "hle/hle.h"
#include "hle/png_decode.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

#define ZLIB_ERROR_NOT_FOUND ((int32_t)0x81120002)
#define ZLIB_ERROR_INVALID ((int32_t)0x81120016)
#define ZLIB_ERROR_NO_SPACE ((int32_t)0x8112001C)
#define ZLIB_ERROR_TIMED_OUT ((int32_t)0x81120027)
#define ZLIB_ERROR_NOT_INITIALIZED ((int32_t)0x81120032)
#define ZLIB_ERROR_ALREADY_INITIALIZED ((int32_t)0x81120033)
#define ZLIB_ERROR_FATAL ((int32_t)0x811200FF)

#define RESULTS 256u /* finished requests kept for sceZlibGetResult */

static pal_mutex *g_lock;
static pal_cond *g_done_cond;
static bool g_init;
static uint64_t g_next_id = 1;
static struct {
    uint64_t id;
    uint32_t produced;
    int32_t status;
} g_results[RESULTS];
static uint64_t g_done[RESULTS]; /* ids not yet taken by sceZlibWaitForDone */
static uint32_t g_done_head, g_done_count;

/* int sceZlibInitialize(const void *buffer, size_t length) */
static void zlib_initialize(hle_call *c) {
    bool buffer = hle_arg_ptr(c, 0) != 0, length = hle_arg(c, 1) != 0;
    int32_t r = 0;
    pal_mutex_lock(g_lock);
    if (g_init) r = ZLIB_ERROR_ALREADY_INITIALIZED;
    else if (buffer != length) r = ZLIB_ERROR_INVALID;
    else g_init = true;
    pal_mutex_unlock(g_lock);
    hle_ret_s32(c, r);
}

/* int sceZlibFinalize(void) */
static void zlib_finalize(hle_call *c) {
    int32_t r = 0;
    pal_mutex_lock(g_lock);
    if (!g_init) r = ZLIB_ERROR_NOT_INITIALIZED;
    else {
        g_init = false;
        memset(g_results, 0, sizeof(g_results));
        g_done_head = g_done_count = 0;
        pal_cond_broadcast(g_done_cond);
    }
    pal_mutex_unlock(g_lock);
    hle_ret_s32(c, r);
}

/* Inflates src into dst; the status of a request: 0, NO_SPACE (the stream is
 * longer than dst: dst is filled) or FATAL (not a valid zlib stream). */
static int32_t inflate_request(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len, uint32_t *produced) {
    size_t n = 0;
    *produced = 0;
    if (png_inflate_zlib(src, src_len, dst, dst_len, &n)) {
        *produced = (uint32_t)n;
        return 0;
    }
    /* Too long for dst, or invalid: decode once more into room for any stream
     * of this size (deflate expands at most ~1032:1) to tell them apart. */
    size_t cap = src_len < (64u << 20) / 1032u ? src_len * 1032u + 64u : (64u << 20);
    uint8_t *tmp = (uint8_t *)malloc(cap);
    int32_t status = ZLIB_ERROR_FATAL;
    if (tmp && png_inflate_zlib(src, src_len, tmp, cap, &n) && n > dst_len) {
        memcpy(dst, tmp, dst_len);
        *produced = (uint32_t)dst_len;
        status = ZLIB_ERROR_NO_SPACE;
    }
    free(tmp);
    return status;
}

/* int sceZlibInflate(const void *src, uint32_t srcLen, void *dst, uint32_t dstLen, uint64_t *requestId) */
static void zlib_inflate(hle_call *c) {
    guest_addr src = hle_arg_ptr(c, 0), dst = hle_arg_ptr(c, 2), id_out = hle_arg_ptr(c, 4);
    uint32_t src_len = hle_arg_u32(c, 1), dst_len = hle_arg_u32(c, 3);
    pal_mutex_lock(g_lock);
    bool init = g_init;
    pal_mutex_unlock(g_lock);
    if (!init) { hle_ret_s32(c, ZLIB_ERROR_NOT_INITIALIZED); return; }
    if (!src || !src_len || !dst || !id_out || !dst_len || dst_len > 65536 || dst_len % 2048) {
        hle_ret_s32(c, ZLIB_ERROR_INVALID);
        return;
    }
    const uint8_t *in = (const uint8_t *)hle_mem(c, src, src_len);
    uint8_t *out = (uint8_t *)hle_mem(c, dst, dst_len);
    uint64_t *id_ptr = (uint64_t *)hle_mem(c, id_out, 8);
    if (!in || !out || !id_ptr) { hle_ret_s32(c, ZLIB_ERROR_INVALID); return; }
    uint32_t produced;
    int32_t status = inflate_request(in, src_len, out, dst_len, &produced);
    static int logged;
    if (status && logged++ < 4)
        LOG_W(HLE, "sceZlibInflate: %u bytes in, status 0x%08x (%u bytes out)", src_len, (uint32_t)status, produced);
    pal_mutex_lock(g_lock);
    uint64_t id = g_next_id++;
    g_results[id % RESULTS].id = id;
    g_results[id % RESULTS].produced = produced;
    g_results[id % RESULTS].status = status;
    if (g_done_count == RESULTS) { /* the oldest id nobody waited for */
        g_done_head = (g_done_head + 1) % RESULTS;
        g_done_count--;
    }
    g_done[(g_done_head + g_done_count++) % RESULTS] = id;
    pal_cond_broadcast(g_done_cond);
    pal_mutex_unlock(g_lock);
    *id_ptr = id;
    hle_ret_s32(c, 0);
}

/* int sceZlibWaitForDone(uint64_t *requestId, const uint32_t *timeout): timeout in
 * microseconds, NULL waits without limit */
static void zlib_wait_for_done(hle_call *c) {
    uint64_t *id_ptr = hle_arg_ptr(c, 0) ? (uint64_t *)hle_mem(c, hle_arg_ptr(c, 0), 8) : NULL;
    const uint32_t *timeout = hle_arg_ptr(c, 1) ? (const uint32_t *)hle_mem(c, hle_arg_ptr(c, 1), 4) : NULL;
    pal_mutex_lock(g_lock);
    int32_t r = 0;
    if (!g_init) r = ZLIB_ERROR_NOT_INITIALIZED;
    else if (!id_ptr) r = ZLIB_ERROR_INVALID;
    else {
        uint64_t deadline = timeout ? pal_time_ns() + (uint64_t)*timeout * 1000u : 0;
        while (g_init && !g_done_count) {
            if (!timeout) { pal_cond_wait(g_done_cond, g_lock); continue; }
            uint64_t now = pal_time_ns();
            if (now >= deadline) break;
            pal_cond_wait_ns(g_done_cond, g_lock, deadline - now);
        }
        if (!g_init) r = ZLIB_ERROR_NOT_INITIALIZED;
        else if (!g_done_count) r = ZLIB_ERROR_TIMED_OUT;
        else {
            *id_ptr = g_done[g_done_head];
            g_done_head = (g_done_head + 1) % RESULTS;
            g_done_count--;
        }
    }
    pal_mutex_unlock(g_lock);
    hle_ret_s32(c, r);
}

/* int sceZlibGetResult(uint64_t requestId, uint32_t *dstLen, int *status) */
static void zlib_get_result(hle_call *c) {
    uint64_t id = hle_arg(c, 0);
    uint32_t *len = hle_arg_ptr(c, 1) ? (uint32_t *)hle_mem(c, hle_arg_ptr(c, 1), 4) : NULL;
    int32_t *status = hle_arg_ptr(c, 2) ? (int32_t *)hle_mem(c, hle_arg_ptr(c, 2), 4) : NULL;
    int32_t r = 0;
    pal_mutex_lock(g_lock);
    if (!g_init) r = ZLIB_ERROR_NOT_INITIALIZED;
    else if (!len || !status) r = ZLIB_ERROR_INVALID;
    else if (!id || g_results[id % RESULTS].id != id) r = ZLIB_ERROR_NOT_FOUND;
    else {
        *len = g_results[id % RESULTS].produced;
        *status = g_results[id % RESULTS].status;
    }
    pal_mutex_unlock(g_lock);
    hle_ret_s32(c, r);
}

void hle_register_zlib(void) {
    static const hle_export table[] = {
        HLE_FUNC_ENTRY("libSceZlib", "sceZlibInitialize", zlib_initialize),
        HLE_FUNC_ENTRY("libSceZlib", "sceZlibFinalize", zlib_finalize),
        HLE_FUNC_ENTRY("libSceZlib", "sceZlibInflate", zlib_inflate),
        HLE_FUNC_ENTRY("libSceZlib", "sceZlibWaitForDone", zlib_wait_for_done),
        HLE_FUNC_ENTRY("libSceZlib", "sceZlibGetResult", zlib_get_result),
    };
    if (!g_lock && (pal_mutex_create(&g_lock) != PS5C_OK || pal_cond_create(&g_done_cond) != PS5C_OK)) {
        LOG_E(HLE, "libSceZlib: no lock");
        return;
    }
    hle_register_table(table, PS5C_ARRAY_COUNT(table));
}
