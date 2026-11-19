/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceSaveData, directory save data: each save is a host folder
 * user/<game folder>/savedata/<dir name>/ with the game's files and a
 * sce_sys/ folder for the metadata (param.bin, blocks.bin). Mounting one maps
 * it to /savedata<N> in the guest file system.
 *
 * Layouts (SDK):
 *   SceSaveDataMount3        { s32 userId; pad; const DirName *dirName; u64 blocks; u64 systemBlocks;
 *                              u32 mountMode; pad; s32 resource; u8 reserved[32]; }
 *   SceSaveDataMountResult   { char mountPoint[16]; u64 requiredBlocks; u32 unused; u32 mountStatus; ... }
 *   SceSaveDataParam         { char title[128]; char subTitle[128]; char detail[1024]; u32 userParam; pad;
 *                              s64 mtime; u8 reserved[32]; }                                   1328 bytes
 *   SceSaveDataDelete        { s32 userId; pad; const TitleId *titleId; const DirName *dirName; ... }
 *   SceSaveDataDirNameSearchCond   { s32 userId; pad; const TitleId *titleId; const DirName *dirName;
 *                                    u32 key; u32 order; u8 reserved[32]; }
 *   SceSaveDataDirNameSearchResult { u32 hitNum; pad; DirName *dirNames; u32 dirNamesNum; u32 setNum;
 *                                    Param *params; SearchInfo *infos; ... }
 *   SceSaveDataSearchInfo / MountInfo { u64 blocks; u64 freeBlocks; u8 reserved[32]; }     48 bytes */
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/hle.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

#define SD_ERR_PARAMETER ((int32_t)0x809F0000)
#define SD_ERR_BUSY ((int32_t)0x809F0003)
#define SD_ERR_NOT_MOUNTED ((int32_t)0x809F0004)
#define SD_ERR_EXISTS ((int32_t)0x809F0007)
#define SD_ERR_NOT_FOUND ((int32_t)0x809F0008)
#define SD_ERR_INTERNAL ((int32_t)0x809F000B)
#define SD_ERR_MOUNT_FULL ((int32_t)0x809F000C)
/* Observed Convert callers accept this result and mount without waiting for
 * completion event 5. The SDK spelling is not confirmed. */
#define SD_CONVERT_HOST_DIRECTORY ((int32_t)0x809F001E)

#define MODE_RDONLY 1u
#define MODE_RDWR 2u
#define MODE_CREATE 4u
#define MODE_CREATE2 32u

#define PARAM_SIZE 1328
#define MAX_MOUNTS 16
#define DEFAULT_BLOCKS 32768ull

static char g_root[300]; /* user/<game folder>/savedata */
static struct {
    bool used;
    char dir[64];
} g_mounts[MAX_MOUNTS];

static const char *root_dir(hle_call *c) {
    if (!g_root[0]) {
        snprintf(g_root, sizeof(g_root), "%.280s/savedata", hle_user_dir(c->rt));
        pal_mkdir(g_root);
    }
    return g_root;
}

/* Directory names are single path components of printable ASCII (games use
 * more than letters and digits: Minecraft world ids end in '@'); characters a
 * host file name cannot hold, and "." / "..", are refused. */
static bool read_dir_name(hle_call *c, guest_addr p, char *out) {
    const char *s = p ? (const char *)hle_mem(c, p, 32) : NULL;
    if (!s) return false;
    size_t n = 0;
    while (n < 32 && s[n]) {
        char ch = s[n];
        if (ch <= ' ' || ch > '~' || strchr("<>:\"/\\|?*", ch)) return false;
        out[n] = ch;
        n++;
    }
    out[n] = '\0';
    return n > 0 && n < 32 && out[n - 1] != '.';
}

static void save_path(hle_call *c, const char *dir, const char *sub, char *out, size_t n) {
    snprintf(out, n, "%s/%s%s%s", root_dir(c), dir, sub ? "/" : "", sub ? sub : "");
}

static bool is_dir(const char *path) {
    pal_path_info info;
    return pal_path_stat(path, &info) == PS5C_OK && info.is_dir;
}

static bool write_file(const char *path, const void *data, uint64_t size) {
    pal_file *f;
    if (pal_file_open(path, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE, &f) != PS5C_OK) return false;
    uint64_t put = 0;
    bool ok = pal_file_write(f, data, size, &put) == PS5C_OK && put == size;
    pal_file_close(f);
    return ok;
}

static bool read_file(const char *path, void *data, uint64_t size) {
    memset(data, 0, size);
    pal_file *f;
    if (pal_file_open(path, PAL_FILE_READ, &f) != PS5C_OK) return false;
    uint64_t got = 0;
    pal_file_read(f, data, size, &got);
    pal_file_close(f);
    return got > 0;
}

static uint64_t read_blocks(hle_call *c, const char *dir) {
    char path[400];
    save_path(c, dir, "sce_sys/blocks.bin", path, sizeof(path));
    uint64_t blocks = 0;
    read_file(path, &blocks, 8);
    return blocks ? blocks : DEFAULT_BLOCKS;
}

static int mount_slot_of(hle_call *c, guest_addr point_addr) {
    const char *p = point_addr ? (const char *)hle_mem(c, point_addr, 16) : NULL;
    int n;
    if (!p || sscanf(p, "/savedata%d", &n) != 1 || n < 0 || n >= MAX_MOUNTS || !g_mounts[n].used) return -1;
    return n;
}

/* Mounts with the request's fields at the given offsets (they differ between
 * the PS5 Mount3 and the PS4 Mount / Mount2 layouts). */
static void mount_at(hle_call *c, uint32_t dir_off, uint32_t blocks_off, uint32_t mode_off) {
    const uint8_t *m = (const uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 48);
    uint8_t *res = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 1), 64);
    char dir[32];
    uint64_t dir_ptr = 0, blocks = 0;
    uint32_t mode = 0;
    if (m) {
        memcpy(&dir_ptr, m + dir_off, 8);
        memcpy(&blocks, m + blocks_off, 8);
        memcpy(&mode, m + mode_off, 4);
    }
    if (!m || !res || !read_dir_name(c, dir_ptr, dir)) {
        hle_ret_s32(c, SD_ERR_PARAMETER);
        return;
    }
    bool create = (mode & MODE_CREATE) != 0, create2 = (mode & MODE_CREATE2) != 0;
    char path[400], sys[400];
    save_path(c, dir, NULL, path, sizeof(path));
    bool exists = is_dir(path);
    int32_t err = 0;
    if (!create && !create2 && !exists) err = SD_ERR_NOT_FOUND;
    else if (create && exists) err = SD_ERR_EXISTS;
    int slot = -1;
    for (int i = 0; i < MAX_MOUNTS && !err; i++) {
        if (g_mounts[i].used && !strcmp(g_mounts[i].dir, dir)) err = SD_ERR_BUSY;
        else if (!g_mounts[i].used && slot < 0) slot = i;
    }
    if (!err && slot < 0) err = SD_ERR_MOUNT_FULL;
    if (err) {
        LOG_D(HLE, "save data '%s' mount (mode 0x%x): error 0x%08x", dir, mode, (uint32_t)err);
        hle_ret_s32(c, err);
        return;
    }
    bool created = false;
    if (!exists) {
        save_path(c, dir, "sce_sys", sys, sizeof(sys));
        pal_mkdir(path);
        pal_mkdir(sys);
        save_path(c, dir, "sce_sys/blocks.bin", sys, sizeof(sys));
        if (!blocks) blocks = DEFAULT_BLOCKS;
        uint8_t param[PARAM_SIZE] = {0};
        bool ok = write_file(sys, &blocks, 8);
        save_path(c, dir, "sce_sys/param.bin", sys, sizeof(sys));
        if (!ok || !write_file(sys, param, sizeof(param))) {
            hle_ret_s32(c, SD_ERR_INTERNAL);
            return;
        }
        created = true;
    }
    char point[24];
    snprintf(point, sizeof(point), "/savedata%d", slot & 15);
    if (!hle_fs_mount(point, path)) {
        hle_ret_s32(c, SD_ERR_MOUNT_FULL);
        return;
    }
    g_mounts[slot].used = true;
    snprintf(g_mounts[slot].dir, sizeof(g_mounts[slot].dir), "%s", dir);
    memset(res, 0, 64);
    memcpy(res, point, strlen(point) + 1);
    uint32_t status = created ? 1 : 0;
    memcpy(res + 28, &status, 4);
    LOG_I(HLE, "save data '%s' mounted at %s (%s)", dir, point, created ? "created" : "existing");
    hle_ret_s32(c, 0);
}

/* int sceSaveDataMount3(const SceSaveDataMount3 *mount, SceSaveDataMountResult *result) */
static void mount3(hle_call *c) { mount_at(c, 8, 16, 32); }
/* PS4: sceSaveDataMount(const SceSaveDataMount *, result):
 *   { s32 userId; pad; const TitleId *titleId; const DirName *dirName; const Fingerprint *fp;
 *     u64 blocks; u32 mountMode; u8 reserved[32]; } */
static void mount_ps4(hle_call *c) { mount_at(c, 16, 32, 40); }
/* PS4: sceSaveDataMount2(const SceSaveDataMount2 *, result):
 *   { s32 userId; pad; const DirName *dirName; u64 blocks; u32 mountMode; u8 reserved[32]; } */
static void mount2_ps4(hle_call *c) { mount_at(c, 8, 16, 24); }

/* int sceSaveDataUmount2(uint32_t mode, const SceSaveDataMountPoint *mountPoint) */
static void umount2(hle_call *c) {
    int slot = mount_slot_of(c, hle_arg_ptr(c, 1));
    if (slot < 0) {
        hle_ret_s32(c, SD_ERR_NOT_MOUNTED);
        return;
    }
    char point[24];
    snprintf(point, sizeof(point), "/savedata%d", slot & 15);
    hle_fs_unmount(point);
    g_mounts[slot].used = false;
    hle_ret_s32(c, 0);
}

/* int sceSaveDataUmount(const SceSaveDataMountPoint *mountPoint) */
static void umount(hle_call *c) {
    int slot = mount_slot_of(c, hle_arg_ptr(c, 0));
    if (slot < 0) {
        hle_ret_s32(c, SD_ERR_NOT_MOUNTED);
        return;
    }
    char point[24];
    snprintf(point, sizeof(point), "/savedata%d", slot & 15);
    hle_fs_unmount(point);
    g_mounts[slot].used = false;
    hle_ret_s32(c, 0);
}

static bool remove_tree(const char *path);
typedef struct remove_ctx {
    const char *dir;
    bool ok;
} remove_ctx;
static bool remove_entry(void *user, const char *name, bool dir) {
    remove_ctx *r = (remove_ctx *)user;
    char p[600];
    snprintf(p, sizeof(p), "%s/%s", r->dir, name);
    if (dir ? !remove_tree(p) : pal_file_remove(p) != PS5C_OK) r->ok = false;
    return true;
}
static bool remove_tree(const char *path) {
    remove_ctx r = {path, true};
    pal_dir_list(path, remove_entry, &r);
    return pal_rmdir(path) == PS5C_OK && r.ok;
}

/* int sceSaveDataDelete(const SceSaveDataDelete *del) */
static void sd_delete(hle_call *c) {
    const uint8_t *d = (const uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 32);
    uint64_t dir_ptr = 0;
    char dir[32], path[400];
    if (d) memcpy(&dir_ptr, d + 16, 8);
    if (!d || !read_dir_name(c, dir_ptr, dir)) {
        hle_ret_s32(c, SD_ERR_PARAMETER);
        return;
    }
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (g_mounts[i].used && !strcmp(g_mounts[i].dir, dir)) {
            hle_ret_s32(c, SD_ERR_BUSY);
            return;
        }
    }
    save_path(c, dir, NULL, path, sizeof(path));
    if (!is_dir(path)) {
        hle_ret_s32(c, SD_ERR_NOT_FOUND);
        return;
    }
    LOG_I(HLE, "save data '%s' deleted", dir);
    hle_ret_s32(c, remove_tree(path) ? 0 : SD_ERR_INTERNAL);
}

/* Param fields by type: 0 all, 1 title, 2 subtitle, 3 detail, 4 user param, 5 mtime. */
static bool param_field(uint32_t type, uint32_t *off, uint32_t *size) {
    static const uint32_t k[6][2] = {{0, PARAM_SIZE}, {0, 128}, {128, 128}, {256, 1024}, {1280, 4}, {1288, 8}};
    if (type > 5) return false;
    *off = k[type][0];
    *size = k[type][1];
    return true;
}

/* int sceSaveDataSetParam(const MountPoint *mp, uint32_t type, const void *buf, size_t size) */
static void set_param(hle_call *c) {
    int slot = mount_slot_of(c, hle_arg_ptr(c, 0));
    uint32_t off, fsize, type = hle_arg_u32(c, 1);
    uint64_t size = hle_arg(c, 3);
    if (slot < 0) {
        hle_ret_s32(c, SD_ERR_NOT_MOUNTED);
        return;
    }
    const uint8_t *buf = size ? (const uint8_t *)hle_mem(c, hle_arg_ptr(c, 2), size) : NULL;
    if (!param_field(type, &off, &fsize) || !buf) {
        hle_ret_s32(c, SD_ERR_PARAMETER);
        return;
    }
    char path[400];
    uint8_t param[PARAM_SIZE];
    save_path(c, g_mounts[slot].dir, "sce_sys/param.bin", path, sizeof(path));
    read_file(path, param, sizeof(param));
    memcpy(param + off, buf, size < fsize ? size : fsize);
    int64_t now = (int64_t)time(NULL);
    memcpy(param + 1288, &now, 8);
    hle_ret_s32(c, write_file(path, param, sizeof(param)) ? 0 : SD_ERR_INTERNAL);
}

/* int sceSaveDataGetParam(const MountPoint *mp, uint32_t type, void *buf, size_t size, size_t *gotSize) */
static void get_param(hle_call *c) {
    int slot = mount_slot_of(c, hle_arg_ptr(c, 0));
    uint32_t off, fsize, type = hle_arg_u32(c, 1);
    uint64_t size = hle_arg(c, 3);
    if (slot < 0) {
        hle_ret_s32(c, SD_ERR_NOT_MOUNTED);
        return;
    }
    uint8_t *buf = size ? (uint8_t *)hle_mem(c, hle_arg_ptr(c, 2), size) : NULL;
    if (!param_field(type, &off, &fsize) || !buf) {
        hle_ret_s32(c, SD_ERR_PARAMETER);
        return;
    }
    char path[400];
    uint8_t param[PARAM_SIZE];
    save_path(c, g_mounts[slot].dir, "sce_sys/param.bin", path, sizeof(path));
    read_file(path, param, sizeof(param));
    uint64_t n = size < fsize ? size : fsize;
    memcpy(buf, param + off, n);
    uint64_t *got = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 4), 8);
    if (got) *got = n;
    hle_ret_s32(c, 0);
}

/* int sceSaveDataGetMountInfo(const MountPoint *mp, SceSaveDataMountInfo *info) */
static void get_mount_info(hle_call *c) {
    int slot = mount_slot_of(c, hle_arg_ptr(c, 0));
    uint64_t *info = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 1), 48);
    if (slot < 0 || !info) {
        hle_ret_s32(c, slot < 0 ? SD_ERR_NOT_MOUNTED : SD_ERR_PARAMETER);
        return;
    }
    memset(info, 0, 48);
    info[0] = read_blocks(c, g_mounts[slot].dir);
    info[1] = info[0];
    hle_ret_s32(c, 0);
}

/* SQL-like pattern of the search: '%' any run, '_' one character. */
static bool name_match(const char *pat, const char *s) {
    if (!*pat) return !*s;
    if (*pat == '%') return name_match(pat + 1, s) || (*s && name_match(pat, s + 1));
    return *s && (*pat == '_' || *pat == *s) && name_match(pat + 1, s + 1);
}

typedef struct search_ctx {
    hle_call *c;
    char pattern[33];
    char names[64][32];
    uint32_t count;
} search_ctx;

static bool search_entry(void *user, const char *name, bool dir) {
    search_ctx *s = (search_ctx *)user;
    if (!dir || strlen(name) >= 32 || s->count >= 64) return true;
    if (s->pattern[0] && !name_match(s->pattern, name)) return true;
    char sys[400];
    save_path(s->c, name, "sce_sys", sys, sizeof(sys));
    if (!is_dir(sys)) return true;
    snprintf(s->names[s->count++], 32, "%s", name);
    return true;
}

static int cmp_names(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

/* int sceSaveDataDirNameSearch(const SearchCond *cond, SearchResult *result) */
/* PS4 saves to carry over: there are none (hitNum and setNum 0) */
static void dir_name_search_ps4(hle_call *c) {
    uint8_t *res = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 1), 0x18);
    if (!res) {
        hle_ret_s32(c, SD_ERR_PARAMETER);
        return;
    }
    memset(res, 0, 4);
    memset(res + 0x14, 0, 4);
    hle_ret_s32(c, 0);
}

static void dir_name_search(hle_call *c) {
    const uint8_t *cond = (const uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 64);
    uint8_t *res = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 1), 64);
    if (!cond || !res) {
        hle_ret_s32(c, SD_ERR_PARAMETER);
        return;
    }
    search_ctx s; /* per call: games search from several threads */
    memset(&s, 0, sizeof(s));
    s.c = c;
    uint64_t pat_ptr = 0, names_ptr = 0, params_ptr = 0, infos_ptr = 0;
    uint32_t order = 0, names_num = 0;
    memcpy(&pat_ptr, cond + 16, 8);
    memcpy(&order, cond + 28, 4);
    const char *pat = pat_ptr ? (const char *)hle_mem(c, pat_ptr, 32) : NULL;
    if (pat) snprintf(s.pattern, sizeof(s.pattern), "%.32s", pat);
    pal_dir_list(root_dir(c), search_entry, &s);
    qsort(s.names, s.count, 32, cmp_names);
    if (order == 1) { /* descending */
        for (uint32_t i = 0; i < s.count / 2; i++) {
            char t[32];
            memcpy(t, s.names[i], 32);
            memcpy(s.names[i], s.names[s.count - 1 - i], 32);
            memcpy(s.names[s.count - 1 - i], t, 32);
        }
    }
    memcpy(&names_ptr, res + 8, 8);
    memcpy(&names_num, res + 16, 4);
    memcpy(&params_ptr, res + 24, 8);
    memcpy(&infos_ptr, res + 32, 8);
    uint32_t set = s.count < names_num ? s.count : names_num;
    for (uint32_t i = 0; i < set; i++) {
        char *dst = names_ptr ? (char *)hle_mem(c, names_ptr + (uint64_t)i * 32, 32) : NULL;
        if (dst) memcpy(dst, s.names[i], 32);
        if (params_ptr) {
            uint8_t *p = (uint8_t *)hle_mem(c, params_ptr + (uint64_t)i * PARAM_SIZE, PARAM_SIZE);
            char path[400];
            save_path(c, s.names[i], "sce_sys/param.bin", path, sizeof(path));
            if (p) read_file(path, p, PARAM_SIZE);
        }
        if (infos_ptr) {
            uint64_t *info = (uint64_t *)hle_mem(c, infos_ptr + (uint64_t)i * 48, 48);
            if (info) {
                memset(info, 0, 48);
                info[0] = info[1] = read_blocks(c, s.names[i]);
            }
        }
    }
    memcpy(res, &s.count, 4);
    memcpy(res + 20, &set, 4);
    hle_ret_s32(c, 0);
}

static int32_t g_next_resource = 1;
static void create_transaction_resource(hle_call *c) { hle_ret_s32(c, g_next_resource++); }
static void sd_ok(hle_call *c) { hle_ret_s32(c, 0); }
static void no_event(hle_call *c) { hle_ret_s32(c, SD_ERR_NOT_FOUND); }

/* sceSaveDataConvert: the observed request starts with userId/padding and
 * a DirName pointer at +8. The remaining console parameters are opaque.
 * Our save directories already use plain host files, without a console
 * filesystem container to convert. Zero announces an asynchronous conversion
 * and requires completion event 5; do not announce work that was not started.
 * Report the observed no-conversion result for an existing host directory.
 * A missing save must NOT report success.
 * Never create, truncate or modify the save's contents in this operation. */
static void convert(hle_call *c) {
    const uint8_t *request=hle_mem(c,hle_arg_ptr(c,0),16);uint64_t name_ptr=0;
    if(request)memcpy(&name_ptr,request+8,8);
    char name[32];
    if(!request || !read_dir_name(c,name_ptr,name)){hle_ret_s32(c,SD_ERR_PARAMETER);return;}
    char path[400];save_path(c,name,NULL,path,sizeof(path));pal_path_info info;
    ps5c_err found=pal_path_stat(path,&info);
    int32_t result=found==PS5C_ERR_NOT_FOUND?SD_ERR_NOT_FOUND:
        found!=PS5C_OK?SD_ERR_INTERNAL:!info.is_dir?SD_ERR_NOT_FOUND:SD_CONVERT_HOST_DIRECTORY;
    static atomic_uint logged;
    if(atomic_fetch_add_explicit(&logged,1,memory_order_relaxed)<8)
        LOG_I(HLE,"sceSaveDataConvert '%s': host directory lookup -> 0x%08x",name,(uint32_t)result);
    hle_ret_s32(c,result);
}

#define LIB "libSceSaveData_native"
static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(LIB, "sceSaveDataConvert", convert),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataMount3", mount3),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataUmount2", umount2),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataUmount", umount),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataDelete", sd_delete),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataSetParam", set_param),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataGetParam", get_param),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataGetMountInfo", get_mount_info),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataDirNameSearch", dir_name_search),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataDirNameSearch2", dir_name_search),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataDirNameSearchPs4", dir_name_search_ps4),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataCreateTransactionResource", create_transaction_resource),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataDeleteTransactionResource", sd_ok),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataPrepare", sd_ok),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataCommit", sd_ok),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataSaveIcon", sd_ok),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataSaveIconByPath", sd_ok),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataBackup", sd_ok),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataGetEventResult", no_event),
    /* no PS4 save to bring over (Astro Bot looks for one at start, Contra:
     * Operation Galuga for three PS4 title ids) */
    HLE_FUNC_ENTRY(LIB, "sceSaveDataTransferringMount", no_event),
    HLE_FUNC_ENTRY(LIB, "sceSaveDataTransferringMountPs4", no_event),
};

/* PS4 games import the library as libSceSaveData; the structures they pass are
 * laid out as above except the Mount requests. */
#define PS4 "libSceSaveData"
static const hle_export k_ps4_exports[] = {
    HLE_FUNC_ENTRY(PS4, "sceSaveDataInitialize", sd_ok),
    HLE_FUNC_ENTRY(PS4, "sceSaveDataInitialize2", sd_ok),
    HLE_FUNC_ENTRY(PS4, "sceSaveDataInitialize3", sd_ok),
    HLE_FUNC_ENTRY(PS4, "sceSaveDataTerminate", sd_ok),
    HLE_FUNC_ENTRY(PS4, "sceSaveDataMount", mount_ps4),
    HLE_FUNC_ENTRY(PS4, "sceSaveDataMount2", mount2_ps4),
    HLE_FUNC_ENTRY(PS4, "sceSaveDataUmount", umount),
    HLE_FUNC_ENTRY(PS4, "sceSaveDataUmount2", umount2),
    HLE_FUNC_ENTRY(PS4, "sceSaveDataDelete", sd_delete),
    HLE_FUNC_ENTRY(PS4, "sceSaveDataSetParam", set_param),
    HLE_FUNC_ENTRY(PS4, "sceSaveDataGetParam", get_param),
    HLE_FUNC_ENTRY(PS4, "sceSaveDataGetMountInfo", get_mount_info),
    HLE_FUNC_ENTRY(PS4, "sceSaveDataDirNameSearch", dir_name_search),
    HLE_FUNC_ENTRY(PS4, "sceSaveDataSaveIcon", sd_ok),
    HLE_FUNC_ENTRY(PS4, "sceSaveDataSaveIconByPath", sd_ok),
    HLE_FUNC_ENTRY(PS4, "sceSaveDataGetEventResult", no_event),
    HLE_FUNC_ENTRY(PS4, "sceSaveDataRegisterEventCallback", sd_ok),
    HLE_FUNC_ENTRY(PS4, "sceSaveDataSetEventInfo", sd_ok),
};

void hle_register_save_data_dir(void) {
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
    hle_register_table(k_ps4_exports, PS5C_ARRAY_COUNT(k_ps4_exports));
}
