/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Small system libraries: libSceSysmodule, libSceAppContent, libSceRtc, and the
 * Razor CPU profiler markers (no profiler is attached: they do nothing). */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "base/log.h"
#include "core/runtime.h"
#include "core/settings.h"
#include "hle/addcont.h"
#include "hle/hle.h"

void hle_register_table(const hle_export *table, size_t count);

/* ---- libSceSysmodule ----
 * Loads firmware modules by id. The libraries a game uses are resolved at link
 * time (LLE or HLE), so loading is a no-op that reports success. */

static void sysmodule_load(hle_call *c) {
    LOG_D(HLE, "sceSysmoduleLoadModule(0x%x)", hle_arg_u32(c, 0));
    hle_ret_s32(c, 0);
}

static void sysmodule_ok(hle_call *c) { hle_ret_s32(c, 0); }

/* ---- libSceAppContent ---- */

static void app_content_initialize(hle_call *c) { hle_ret_s32(c, 0); }

/* int sceAppContentAppParamGetInt(int param_id, int *value)
 * 0 = SKU flag (1 = full game), 1..4 = user-defined params from param.json. */
static void app_content_param_get_int(hle_call *c) {
    int32_t id = hle_arg_s32(c, 0);
    int32_t *out = (int32_t *)hle_mem(c, hle_arg_ptr(c, 1), 4);
    if (!out || id < 0 || id > 4) {
        hle_ret_s32(c, (int32_t)0x80D90002); /* SCE_APP_CONTENT_ERROR_PARAMETER */
        return;
    }
    int64_t v = 0;
    if (id == 0) {
        v = 1;
    } else {
        char key[24];
        snprintf(key, sizeof(key), "userDefinedParam%d", id);
        settings_param_int(key, &v);
    }
    *out = (int32_t)v;
    hle_ret_s32(c, 0);
}

/* int sceAppContentTemporaryDataMount2(int option, SceAppContentMountPoint *mount_point)
 * mount_point: char data[16] */
static void app_content_temporary_data_mount2(hle_call *c) {
    char *out = (char *)hle_mem(c, hle_arg_ptr(c, 1), 16);
    if (!out) {
        hle_ret_s32(c, (int32_t)0x80D90002);
        return;
    }
    memset(out, 0, 16);
    memcpy(out, "/temp0", 6);
    hle_ret_s32(c, 0);
}

/* int sceAppContentTemporaryDataFormat(const SceAppContentMountPoint *mount_point) and
 * sceAppContentTemporaryDataUnmount(same): /temp0 is a host folder that stays mounted;
 * formatting leaves its files (a cache the game rewrites), as KytyPS5 does */
static void app_content_temporary_data_keep(hle_call *c) {
    hle_ret_s32(c, hle_arg_ptr(c, 0) ? 0 : (int32_t)0x80D90002);
}

/* int sceAppContentDownloadDataGetAvailableSpaceKb(const SceAppContentMountPoint *mp,
 * size_t *kb): the quota param.json grants (downloadDataSize, MiB), 0 without one */
static void app_content_download_space(hle_call *c) {
    uint64_t *out = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 1), 8);
    if (!out || !hle_arg_ptr(c, 0)) {
        hle_ret_s32(c, (int32_t)0x80D90002);
        return;
    }
    int64_t mib = 0;
    if (!settings_param_int("downloadDataSize", &mib) || mib < 0) mib = 0;
    *out = (uint64_t)mib * 1024u;
    hle_ret_s32(c, 0);
}

/* int sceAppContentTemporaryDataGetAvailableSpaceKb(const SceAppContentMountPoint *mp,
 * size_t *kb): /temp0 is a host folder; 4 GiB is what a console grants */
static void app_content_temporary_space(hle_call *c) {
    uint64_t *out = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 1), 8);
    if (!out || !hle_arg_ptr(c, 0)) {
        hle_ret_s32(c, (int32_t)0x80D90002);
        return;
    }
    *out = 4ull * 1024 * 1024;
    hle_ret_s32(c, 0);
}

/* Add-on content installed under user/dlc/<title id>/ (addcont.c).
 * SceAppContentAddcontInfo { SceNpUnifiedEntitlementLabel { char data[17]; char pad[3]; }; u32 status; },
 * status 4: installed. */
#define AC_ERR_PARAMETER ((int32_t)0x80D90002)
#define AC_ERR_NOT_FOUND ((int32_t)0x80D90005)
#define AC_STATUS_INSTALLED 4u
static bool addcont_info(hle_call *c, guest_addr out, uint32_t i) {
    uint8_t e[24] = {0};
    const char *label = addcont_label(i);
    memcpy(e, label, strlen(label));
    uint32_t status = AC_STATUS_INSTALLED;
    memcpy(e + 20, &status, 4);
    return gmem_write(c->mem, out, e, sizeof(e));
}

/* int sceAppContentGetAddcontInfoList(u32 service_label, SceAppContentAddcontInfo *list, u32 num, u32 *hit) */
static void app_content_get_addcont_info_list(hle_call *c) {
    guest_addr list = hle_arg_ptr(c, 1), hit = hle_arg_ptr(c, 3);
    uint32_t num = hle_arg_u32(c, 2), n = addcont_count();
    if (!list && !hit) {
        hle_ret_s32(c, AC_ERR_PARAMETER);
        return;
    }
    uint32_t written = 0;
    for (uint32_t i = 0; list && i < n && i < num; i++) written += addcont_info(c, list + (guest_addr)i * 24, i);
    uint32_t count = list ? written : n;
    if (hit) gmem_write(c->mem, hit, &count, 4);
    hle_ret_s32(c, 0);
}

/* int sceAppContentGetAddcontInfo(u32 service_label, const SceNpUnifiedEntitlementLabel *label, SceAppContentAddcontInfo *info) */
static void app_content_get_addcont_info(hle_call *c) {
    char label[17] = {0};
    if (!hle_arg_ptr(c, 2) || !gmem_read(c->mem, hle_arg_ptr(c, 1), label, 16)) {
        hle_ret_s32(c, AC_ERR_PARAMETER);
        return;
    }
    int32_t i = addcont_find(label);
    hle_ret_s32(c, i < 0 ? AC_ERR_NOT_FOUND : addcont_info(c, hle_arg_ptr(c, 2), (uint32_t)i) ? 0 : AC_ERR_PARAMETER);
}

/* int sceAppContentAddcontMount(u32 service_label, const SceNpUnifiedEntitlementLabel *label,
 * SceAppContentMountPoint *mp): the add-on's folder as /addcont<n> */
static void app_content_addcont_mount(hle_call *c) {
    char label[17] = {0};
    char *mp = (char *)hle_mem(c, hle_arg_ptr(c, 2), 16);
    if (!mp || !gmem_read(c->mem, hle_arg_ptr(c, 1), label, 16)) {
        hle_ret_s32(c, AC_ERR_PARAMETER);
        return;
    }
    int32_t i = addcont_find(label);
    if (i < 0) {
        hle_ret_s32(c, AC_ERR_NOT_FOUND);
        return;
    }
    char point[16];
    snprintf(point, sizeof(point), "/addcont%d", (int)i);
    if (!hle_fs_mount(point, addcont_dir((uint32_t)i))) {
        hle_ret_s32(c, AC_ERR_NOT_FOUND);
        return;
    }
    memset(mp, 0, 16);
    memcpy(mp, point, strlen(point));
    LOG_I(HLE, "add-on %s mounted at %s", label, point);
    hle_ret_s32(c, 0);
}

/* int sceAppContentAddcontUnmount(const SceAppContentMountPoint *mp) */
static void app_content_addcont_unmount(hle_call *c) {
    char point[17] = {0};
    if (!gmem_read(c->mem, hle_arg_ptr(c, 0), point, 16)) {
        hle_ret_s32(c, AC_ERR_PARAMETER);
        return;
    }
    hle_fs_unmount(point);
    hle_ret_s32(c, 0);
}

/* int sceRtcGetCurrentTick(SceRtcTick *tick): microseconds since 0001-01-01 UTC */
#define RTC_UNIX_EPOCH_TICKS 62135596800000000ull
static void rtc_get_current_tick(hle_call *c) {
    uint64_t *tick = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 0), 8);
    if (!tick) {
        hle_ret_s32(c, (int32_t)0x80B50002); /* invalid pointer */
        return;
    }
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    *tick = RTC_UNIX_EPOCH_TICKS + (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    hle_ret_s32(c, 0);
}

/* unsigned int sceRtcGetTickResolution(void): ticks per second */
static void rtc_get_tick_resolution(hle_call *c) { hle_ret_s32(c, 1000000); }

/* Days from 0001-01-01 to y-m-d (proleptic Gregorian) */
static int64_t civil_days(int64_t y, int64_t m, int64_t d) {
    if (m < 3) {
        y--;
        m += 12;
    }
    return 365 * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + d - 1 - 306;
}

/* The host time zone offset at Unix time t, seconds east of UTC */
static int64_t local_offset(time_t t) {
    struct tm l, g;
    const struct tm *r = localtime(&t);
    if (!r) return 0;
    l = *r;
    if (!(r = gmtime(&t))) return 0;
    g = *r;
    int64_t dl = civil_days(l.tm_year + 1900, l.tm_mon + 1, l.tm_mday), dg = civil_days(g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
    return (dl - dg) * 86400 + (l.tm_hour - g.tm_hour) * 3600 + (l.tm_min - g.tm_min) * 60 + (l.tm_sec - g.tm_sec);
}

/* int sceRtcConvertUtcToLocalTime(const SceRtcTick *utc, SceRtcTick *local) and the
 * inverse: the host time zone, as sceRtcGetCurrentClockLocalTime uses */
#define RTC_MAX_TICK (3652059ull * 86400000000ull - 1)
static void rtc_convert(hle_call *c, int64_t sign) {
    uint64_t in = 0, *out = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 1), 8);
    if (!out || !gmem_read_u64(c->mem, hle_arg_ptr(c, 0), &in)) {
        hle_ret_s32(c, (int32_t)0x80B50002);
        return;
    }
    if (in > RTC_MAX_TICK) {
        hle_ret_s32(c, (int32_t)0x80B50003);
        return;
    }
    int64_t unix_s = (int64_t)(in / 1000000u) - (int64_t)(RTC_UNIX_EPOCH_TICKS / 1000000u);
    int64_t r = (int64_t)in + sign * local_offset((time_t)unix_s) * 1000000;
    if (r < 0 || (uint64_t)r > RTC_MAX_TICK) {
        hle_ret_s32(c, (int32_t)0x80B50003);
        return;
    }
    *out = (uint64_t)r;
    hle_ret_s32(c, 0);
}
static void rtc_utc_to_local(hle_call *c) { rtc_convert(c, 1); }
static void rtc_local_to_utc(hle_call *c) { rtc_convert(c, -1); }


/* int sceRtcGetTick(const SceRtcDateTime *in, SceRtcTick *tick) */
static void rtc_get_tick(hle_call *c) {
    const uint8_t *in = (const uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 16);
    uint64_t *tick = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 1), 8);
    if (!in || !tick) {
        hle_ret_s32(c, (int32_t)0x80B50002);
        return;
    }
    uint16_t f[6];
    uint32_t us;
    memcpy(f, in, 12);
    memcpy(&us, in + 12, 4);
    /* days since 0001-01-01 (proleptic Gregorian) */
    int64_t y = f[0], m = f[1], d = f[2];
    if (m < 3) {
        y--;
        m += 12;
    }
    int64_t days = 365 * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + d - 1 - 306;
    uint64_t secs = (uint64_t)days * 86400u + f[3] * 3600u + f[4] * 60u + f[5];
    *tick = secs * 1000000u + us;
    hle_ret_s32(c, 0);
}

/* SceRtcDateTime: u16 year, month, day, hour, minute, second; u32 microsecond */
static int32_t rtc_fill(hle_call *c, guest_addr out_ptr, bool local, int32_t offset_min) {
    uint8_t *out = (uint8_t *)hle_mem(c, out_ptr, 16);
    if (!out) return (int32_t)0x80B50002;
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    time_t t = ts.tv_sec + (time_t)offset_min * 60;
    struct tm tm;
    const struct tm *r = local ? localtime(&t) : gmtime(&t);
    if (!r) return (int32_t)0x80B50002;
    tm = *r;
    uint16_t f[6] = {(uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_mday,
                     (uint16_t)tm.tm_hour, (uint16_t)tm.tm_min, (uint16_t)tm.tm_sec};
    uint32_t us = (uint32_t)(ts.tv_nsec / 1000);
    memcpy(out, f, 12);
    memcpy(out + 12, &us, 4);
    return 0;
}

/* int sceRtcGetCurrentClock(SceRtcDateTime *out, int time_zone_minutes) */
static void rtc_get_current_clock(hle_call *c) {
    hle_ret_s32(c, rtc_fill(c, hle_arg_ptr(c, 0), false, hle_arg_s32(c, 1)));
}

/* int sceRtcGetCurrentClockLocalTime(SceRtcDateTime *out) */
static void rtc_get_current_clock_local(hle_call *c) { hle_ret_s32(c, rtc_fill(c, hle_arg_ptr(c, 0), true, 0)); }

static void razor_ok(hle_call *c) { hle_ret_s32(c, 0); }

#define RZ "libSceRazorCpu"
static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(RZ, "sceRazorCpuPushMarker", razor_ok),
    HLE_FUNC_ENTRY(RZ, "sceRazorCpuPushMarkerStatic", razor_ok),
    HLE_FUNC_ENTRY(RZ, "sceRazorCpuPushMarkerWithHud", razor_ok),
    HLE_FUNC_ENTRY(RZ, "sceRazorCpuPopMarker", razor_ok),
    HLE_FUNC_ENTRY(RZ, "sceRazorCpuNamedSync", razor_ok),
    HLE_FUNC_ENTRY(RZ, "sceRazorCpuPlotValue", razor_ok),
    HLE_FUNC_ENTRY(RZ, "sceRazorCpuSync", razor_ok),
    HLE_FUNC_ENTRY(RZ, "sceRazorCpuStartCapture", razor_ok),
    HLE_FUNC_ENTRY(RZ, "sceRazorCpuStopCapture", razor_ok),
    HLE_FUNC_ENTRY(RZ, "sceRazorCpuIsCapturing", razor_ok),
    HLE_FUNC_ENTRY(RZ, "sceRazorCpuInitializeInternal", razor_ok),
    HLE_FUNC_ENTRY(RZ, "sceRazorCpuSetPopMarkerCallback", razor_ok),
    HLE_FUNC_ENTRY(RZ, "sceRazorCpuSetPushMarkerCallback", razor_ok),
    HLE_FUNC_ENTRY(RZ, "sceRazorCpuSetPushMarkerStaticCallback", razor_ok),
    HLE_FUNC_ENTRY("libSceRtc", "sceRtcGetCurrentClock", rtc_get_current_clock),
    HLE_FUNC_ENTRY("libSceRtc", "sceRtcGetCurrentClockLocalTime", rtc_get_current_clock_local),
    HLE_FUNC_ENTRY("libSceRtc", "sceRtcGetCurrentTick", rtc_get_current_tick),
    /* network time: the host clock (it is synchronized) */
    HLE_FUNC_ENTRY("libSceRtc", "sceRtcGetCurrentNetworkTick", rtc_get_current_tick),
    HLE_FUNC_ENTRY("libSceRtc", "sceRtcGetTickResolution", rtc_get_tick_resolution),
    HLE_FUNC_ENTRY("libSceRtc", "sceRtcGetTick", rtc_get_tick),
    HLE_FUNC_ENTRY("libSceRtc", "sceRtcConvertUtcToLocalTime", rtc_utc_to_local),
    HLE_FUNC_ENTRY("libSceRtc", "sceRtcConvertLocalTimeToUtc", rtc_local_to_utc),
    HLE_FUNC_ENTRY("libSceSysmodule", "sceSysmoduleLoadModule", sysmodule_load),
    HLE_FUNC_ENTRY("libSceSysmodule", "sceSysmoduleLoadModuleInternal", sysmodule_load),
    HLE_FUNC_ENTRY("libSceSysmodule", "sceSysmoduleUnloadModule", sysmodule_ok),
    HLE_FUNC_ENTRY("libSceSysmodule", "sceSysmoduleIsLoaded", sysmodule_ok),
    HLE_FUNC_ENTRY("libSceAppContent", "sceAppContentInitialize", app_content_initialize),
    HLE_FUNC_ENTRY("libSceAppContent", "sceAppContentAppParamGetInt", app_content_param_get_int),
    HLE_FUNC_ENTRY("libSceAppContent", "sceAppContentTemporaryDataMount2", app_content_temporary_data_mount2),
    HLE_FUNC_ENTRY("libSceAppContent", "sceAppContentTemporaryDataFormat", app_content_temporary_data_keep),
    HLE_FUNC_ENTRY("libSceAppContent", "sceAppContentTemporaryDataUnmount", app_content_temporary_data_keep),
    HLE_FUNC_ENTRY("libSceAppContent", "sceAppContentDownloadDataGetAvailableSpaceKb", app_content_download_space),
    HLE_FUNC_ENTRY("libSceAppContent", "sceAppContentTemporaryDataGetAvailableSpaceKb", app_content_temporary_space),
    HLE_FUNC_ENTRY("libSceAppContent", "sceAppContentAddcontMount", app_content_addcont_mount),
    HLE_FUNC_ENTRY("libSceAppContent", "sceAppContentAddcontUnmount", app_content_addcont_unmount),
    HLE_FUNC_ENTRY("libSceAppContent", "sceAppContentGetAddcontInfoList", app_content_get_addcont_info_list),
    HLE_FUNC_ENTRY("libSceAppContent", "sceAppContentGetAddcontInfo", app_content_get_addcont_info),
};

void hle_register_system(void) { hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports)); }
