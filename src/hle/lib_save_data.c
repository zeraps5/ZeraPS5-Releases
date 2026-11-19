/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceSaveData: the "save data memory" API, a small per-user blob (the
 * Unity player keeps PlayerPrefs in it), backed by host files under
 * user/<game folder>/savedata/, one directory per game. Directory-based save
 * data is not implemented yet.
 *
 * Layouts (SDK):
 *   SceSaveDataMemoryData   { void *buf; size_t bufSize; off_t offset; u8 reserved[40]; }       0x40 bytes
 *   SceSaveDataMemorySetup2 { u32 option; s32 userId; size_t memorySize; size_t iconMemorySize;
 *                             const Param *initParam; const Icon *initIcon; u32 slotId; ... }
 *   SceSaveDataMemoryGet2   { s32 userId; u8 pad[4]; Data *data; Param *param; Icon *icon; u32 slotId; ... }
 *   SceSaveDataMemorySet2   { s32 userId; u8 pad[4]; const Data *data; const Param *param;
 *                             const Icon *icon; u32 dataNum; u8 pad2[4]; u32 slotId; ... } */
#include <stdio.h>
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/hle.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

#define SD_ERR_PARAMETER ((int32_t)0x809F0000)
#define SD_ERR_MEMORY_NOT_READY ((int32_t)0x809F0012)
#define SD_MAX_SLOTS 16

static char g_dir[300]; /* user/<game folder>/savedata */

static void init_dir(hle_call *c) {
    if (g_dir[0]) return;
    snprintf(g_dir, sizeof(g_dir), "%.280s/savedata", hle_user_dir(c->rt));
    pal_mkdir(g_dir);
}

static void memory_path(uint32_t slot, char *out, size_t n) { snprintf(out, n, "%.280s/memory%u.bin", g_dir, slot); }

/* Reads [offset, offset + size) of the slot file; bytes past its end read as zero. */
static bool memory_read(uint32_t slot, uint64_t offset, uint8_t *dst, uint64_t size) {
    char path[256];
    memory_path(slot, path, sizeof(path));
    memset(dst, 0, size);
    pal_file *f;
    if (pal_file_open(path, PAL_FILE_READ, &f) != PS5C_OK) return false;
    uint64_t got = 0;
    if (pal_file_seek(f, (int64_t)offset, PAL_SEEK_SET, NULL) == PS5C_OK) pal_file_read(f, dst, size, &got);
    pal_file_close(f);
    return true;
}

static bool memory_write(uint32_t slot, uint64_t offset, const uint8_t *src, uint64_t size) {
    char path[256];
    memory_path(slot, path, sizeof(path));
    pal_file *f;
    if (pal_file_open(path, PAL_FILE_READ | PAL_FILE_WRITE | PAL_FILE_CREATE, &f) != PS5C_OK) return false;
    uint64_t put = 0;
    bool ok = pal_file_seek(f, (int64_t)offset, PAL_SEEK_SET, NULL) == PS5C_OK &&
              pal_file_write(f, src, size, &put) == PS5C_OK && put == size;
    pal_file_close(f);
    return ok;
}

static bool memory_exists(uint32_t slot) {
    char path[256];
    memory_path(slot, path, sizeof(path));
    pal_file *f;
    if (pal_file_open(path, PAL_FILE_READ, &f) != PS5C_OK) return false;
    pal_file_close(f);
    return true;
}

static uint64_t memory_size(uint32_t slot) {
    char path[256];
    memory_path(slot, path, sizeof(path));
    pal_path_info info;
    if (pal_path_stat(path, &info) != PS5C_OK) return 0;
    return info.size;
}

static void sd_ok(hle_call *c) { hle_ret_s32(c, 0); }

/* int sceSaveDataSetupSaveDataMemory2(const SceSaveDataMemorySetup2 *setup, SceSaveDataMemorySetupResult *result) */
static void setup_memory2(hle_call *c) {
    init_dir(c);
    const uint8_t *s = (const uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 0x30);
    if (!s) {
        hle_ret_s32(c, SD_ERR_PARAMETER);
        return;
    }
    uint64_t size;
    uint32_t slot;
    memcpy(&size, s + 8, 8);
    memcpy(&slot, s + 0x28, 4);
    if (slot >= SD_MAX_SLOTS) slot = 0;
    uint64_t existed = memory_exists(slot) ? memory_size(slot) : 0;
    if (!existed && size) {
        static const uint8_t zero = 0;
        memory_write(slot, size - 1, &zero, 1); /* the file grows zero-filled */
    }
    uint64_t *result = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 1), 8);
    if (result) *result = existed;
    LOG_I(HLE, "save data memory slot %u: %llu bytes (%s)", slot, (unsigned long long)size, existed ? "existing" : "new");
    hle_ret_s32(c, 0);
}

/* int sceSaveDataGetSaveDataMemory2(SceSaveDataMemoryGet2 *get) */
static void get_memory2(hle_call *c) {
    init_dir(c);
    const uint8_t *g = (const uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 0x28);
    if (!g) {
        hle_ret_s32(c, SD_ERR_PARAMETER);
        return;
    }
    uint64_t data_ptr;
    uint32_t slot;
    memcpy(&data_ptr, g + 8, 8);
    memcpy(&slot, g + 0x20, 4);
    if (slot >= SD_MAX_SLOTS) slot = 0;
    if (!memory_exists(slot)) {
        LOG_D(HLE, "sceSaveDataGetSaveDataMemory2: slot %u not set up", slot);
        hle_ret_s32(c, SD_ERR_MEMORY_NOT_READY);
        return;
    }
    const uint8_t *d = data_ptr ? (const uint8_t *)hle_mem(c, data_ptr, 0x18) : NULL;
    if (d) {
        uint64_t buf, size, offset;
        memcpy(&buf, d, 8);
        memcpy(&size, d + 8, 8);
        memcpy(&offset, d + 16, 8);
        uint8_t *dst = size ? (uint8_t *)hle_mem(c, buf, size) : NULL;
        if (dst) memory_read(slot, offset, dst, size);
    }
    hle_ret_s32(c, 0);
}

/* int sceSaveDataSetSaveDataMemory2(const SceSaveDataMemorySet2 *set) */
static void set_memory2(hle_call *c) {
    init_dir(c);
    const uint8_t *s = (const uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 0x30);
    if (!s) {
        hle_ret_s32(c, SD_ERR_PARAMETER);
        return;
    }
    uint64_t data_ptr;
    uint32_t count, slot;
    memcpy(&data_ptr, s + 8, 8);
    memcpy(&count, s + 0x20, 4);
    memcpy(&slot, s + 0x24, 4); /* right after dataNum (0x28 sent Sonic Origins' slot 1 writes to slot 0) */
    if (slot >= SD_MAX_SLOTS) slot = 0;
    if (!count && data_ptr) count = 1;
    for (uint32_t i = 0; i < count && data_ptr; i++) {
        const uint8_t *d = (const uint8_t *)hle_mem(c, data_ptr + (uint64_t)i * 0x40, 0x18);
        if (!d) break;
        uint64_t buf, size, offset;
        memcpy(&buf, d, 8);
        memcpy(&size, d + 8, 8);
        memcpy(&offset, d + 16, 8);
        const uint8_t *src = size ? (const uint8_t *)hle_mem(c, buf, size) : NULL;
        if (src) memory_write(slot, offset, src, size);
    }
    hle_ret_s32(c, 0);
}

#define LIB "libSceSaveData_native"
static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(LIB, "sceSaveDataInitialize3", sd_ok),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataTerminate", sd_ok),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataSetupSaveDataMemory2", setup_memory2),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataGetSaveDataMemory2", get_memory2),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataSetSaveDataMemory2", set_memory2),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataSyncSaveDataMemory", sd_ok),
};

void hle_register_save_data(void) { hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports)); }
