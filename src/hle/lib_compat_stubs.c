/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Small imports our games call that had no handler (survey of 2026-11-18 against
 * SharpEmu, GPL-2.0, whose results and outputs these follow). Each one either has
 * nothing to do here (no PSN, no trophies UI, no disc) or reports a fixed state. */
#include <stdint.h>
#include <string.h>

#include "hle/hle.h"

void hle_register_table(const hle_export *table, size_t count);

static void ok(hle_call *c) { hle_ret_s32(c, 0); }

static bool put_u32(hle_call *c, guest_addr at, uint32_t v) {
    void *p = at ? hle_mem(c, at, 4) : NULL;
    if (p) memcpy(p, &v, 4);
    return p != NULL;
}
static bool put_u64(hle_call *c, guest_addr at, uint64_t v) {
    void *p = at ? hle_mem(c, at, 8) : NULL;
    if (p) memcpy(p, &v, 8);
    return p != NULL;
}

#define MEMORY_FAULT ((int32_t)0x8002000E) /* EFAULT */

/* sceSaveDataGetConvertProgress(float *progress): our save operations finish at once. */
static void save_convert_progress(hle_call *c) {
    float done = 1.0f;
    uint32_t bits;
    memcpy(&bits, &done, 4);
    put_u64(c, hle_arg_ptr(c, 0), bits);
    hle_ret_s32(c, 0);
}

/* sceDiscMapIsRequestOnHDD(path, offset, size, int *on_hdd): everything is on the drive. */
#define DISCMAP_INVALID_ARGUMENT ((int32_t)0x81100001)
static void discmap_on_hdd(hle_call *c) {
    if (!hle_arg_ptr(c, 0) || !hle_arg_ptr(c, 3)) { hle_ret_s32(c, DISCMAP_INVALID_ARGUMENT); return; }
    hle_ret_s32(c, put_u32(c, hle_arg_ptr(c, 3), 1) ? 0 : MEMORY_FAULT);
}
/* #fJgP+wqifno(path, offset, size, u64 *flags, u64 *a, u64 *b): no disc mapping. */
static void discmap_triple(hle_call *c) {
    if (!hle_arg_ptr(c, 0) || !hle_arg_ptr(c, 3) || !hle_arg_ptr(c, 4) || !hle_arg_ptr(c, 5)) {
        hle_ret_s32(c, DISCMAP_INVALID_ARGUMENT);
        return;
    }
    bool w = put_u64(c, hle_arg_ptr(c, 3), 0) && put_u64(c, hle_arg_ptr(c, 4), 0) && put_u64(c, hle_arg_ptr(c, 5), 0);
    hle_ret_s32(c, w ? 0 : MEMORY_FAULT);
}

static void page_size(hle_call *c) { hle_ret_s32(c, 16384); }

/* sceSysmoduleLoadModuleInternalWithArg(id, argc, argv, flags, int *result): the system
 * modules are HLE, so loading always succeeds. */
static void sysmodule_load_with_arg(hle_call *c) {
    guest_addr result = hle_arg_ptr(c, 4);
    if (result && !put_u32(c, result, 0)) { hle_ret_s32(c, MEMORY_FAULT); return; }
    hle_ret_s32(c, 0);
}

/* Platform privacy setting (id 1000 is the only one known): 0, nothing restricted. */
static void privacy_setting(hle_call *c) {
    if (hle_arg_s32(c, 0) != 1000) { hle_ret_s32(c, (int32_t)0x80960009); return; }
    if (!hle_arg_ptr(c, 1)) { hle_ret_s32(c, (int32_t)0x80960005); return; }
    hle_ret_s32(c, put_u32(c, hle_arg_ptr(c, 1), 0) ? 0 : MEMORY_FAULT);
}

/* __elf_phdr_match_addr(phdr_info, addr): libc's dl_iterate_phdr filter; accept. */
static void phdr_match_addr(hle_call *c) { hle_ret_s32(c, hle_arg_ptr(c, 0) ? 1 : 0); }

void hle_register_compat_stubs(void) {
    static const hle_export table[] = {
        HLE_FUNC_ENTRY("libSceNpTrophy2", "sceNpTrophy2UnregisterUnlockCallback", ok),
        HLE_FUNC_ENTRY("libSceNpTrophy2", "sceNpTrophy2ShowTrophyList", ok),
        HLE_FUNC_ENTRY("libSceRudp", "sceRudpInit", ok),
        HLE_FUNC_ENTRY("libSceRudp", "sceRudpSetEventHandler", ok),
        HLE_FUNC_ENTRY("libSceRudp", "sceRudpEnableInternalIOThread", ok),
        HLE_FUNC_ENTRY("libSceSaveData_native", "sceSaveDataCancel", ok),
        HLE_FUNC_ENTRY("libSceSaveData_native", "sceSaveDataGetConvertProgress", save_convert_progress),
        HLE_FUNC_ENTRY("libSceDiscMap", "sceDiscMapIsRequestOnHDD", discmap_on_hdd),
        HLE_FUNC_ENTRY("libSceDiscMap", "#fJgP+wqifno", discmap_triple),
        HLE_FUNC_ENTRY("libScePad", "#n3kSX62fgNo", ok),
        HLE_FUNC_ENTRY("libScePosix", "getpagesize", page_size),
        HLE_FUNC_ENTRY("libkernel", "getpagesize", page_size),
        HLE_FUNC_ENTRY("libSceSysmodule", "sceSysmoduleLoadModuleInternalWithArg", sysmodule_load_with_arg),
        HLE_FUNC_ENTRY("libSceUserServicePlatformPrivacyWs1", "#D-CzAxQL0XI", privacy_setting),
        HLE_FUNC_ENTRY("libkernel", "sceKernelGetGPI", ok),
        HLE_FUNC_ENTRY("libSceSsl", "sceSslClose", ok),
        HLE_FUNC_ENTRY("libkernel", "__elf_phdr_match_addr", phdr_match_addr),
        HLE_FUNC_ENTRY("libSceLibcInternalExt", "sceLibcInternalHeapErrorReportForGame", ok),
        HLE_FUNC_ENTRY("libkernel", "sceKernelDebugWriteCppExceptionInfo", ok),
    };
    hle_register_table(table, PS5C_ARRAY_COUNT(table));
}
