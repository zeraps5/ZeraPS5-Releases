/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceRtc beyond the clock reads in lib_system.c: calendar arithmetic on
 * SceRtcDateTime (u16 year, month, day, hour, minute, second; u32 microsecond)
 * and SceRtcTick (u64 microseconds since 0001-01-01 00:00:00, proleptic
 * Gregorian), conversions to time_t, DOS and Win32 file times, RFC 2822/3339
 * text. Setting the system clock is accepted and ignored. */
#include "hle/hle.h"
#include "hle/lib_rtc.h"
#include "mem/gmem.h"

void hle_register_table(const hle_export *table, size_t count);

#include <stdio.h>
#include <string.h>
#include <time.h>

#define RTC_ERR_INVALID_POINTER ((int32_t)0x80B50002)
#define RTC_ERR_INVALID_VALUE ((int32_t)0x80B50003)
#define RTC_ERR_INVALID_YEAR ((int32_t)0x80B50008)
#define RTC_ERR_INVALID_MONTH ((int32_t)0x80B50009)
#define RTC_ERR_INVALID_DAY ((int32_t)0x80B5000A)
#define RTC_ERR_INVALID_HOUR ((int32_t)0x80B5000B)
#define RTC_ERR_INVALID_MINUTE ((int32_t)0x80B5000C)
#define RTC_ERR_INVALID_SECOND ((int32_t)0x80B5000D)
#define RTC_ERR_INVALID_MICROSECOND ((int32_t)0x80B5000E)

#define US_PER_DAY 86400000000ull
#define RTC_DAYS_MAX 3652059ull /* 0001-01-01 .. 9999-12-31 */
#define RTC_TICK_MAX (RTC_DAYS_MAX * US_PER_DAY - 1)

/* ---- calendar (shared with the unit test through lib_rtc.h) ---- */

bool rtc_is_leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int rtc_days_in_month(int64_t y, int64_t m) {
    static const int k[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && rtc_is_leap(y) ? 29 : k[m - 1];
}

/* days from 0001-01-01 */
int64_t rtc_days_from_civil(int64_t y, int64_t m, int64_t d) {
    if (m < 3) {
        y--;
        m += 12;
    }
    return 365 * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + d - 1 - 306;
}

void rtc_civil_from_days(int64_t days, int64_t *y, int64_t *m, int64_t *d) {
    /* shift to an era-based count from 0000-03-01 */
    int64_t z = days + 306;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*m <= 2);
}

int32_t rtc_check(const rtc_datetime *t) {
    if (t->year < 1 || t->year > 9999) return RTC_ERR_INVALID_YEAR;
    if (t->month < 1 || t->month > 12) return RTC_ERR_INVALID_MONTH;
    if (t->day < 1 || t->day > rtc_days_in_month(t->year, t->month)) return RTC_ERR_INVALID_DAY;
    if (t->hour > 23) return RTC_ERR_INVALID_HOUR;
    if (t->minute > 59) return RTC_ERR_INVALID_MINUTE;
    if (t->second > 59) return RTC_ERR_INVALID_SECOND;
    if (t->microsecond > 999999) return RTC_ERR_INVALID_MICROSECOND;
    return 0;
}

uint64_t rtc_to_tick(const rtc_datetime *t) {
    uint64_t days = (uint64_t)rtc_days_from_civil(t->year, t->month, t->day);
    return days * US_PER_DAY + ((uint64_t)t->hour * 3600 + (uint64_t)t->minute * 60 + t->second) * 1000000ull + t->microsecond;
}

void rtc_from_tick(uint64_t tick, rtc_datetime *t) {
    int64_t y, m, d;
    rtc_civil_from_days((int64_t)(tick / US_PER_DAY), &y, &m, &d);
    uint64_t us = tick % US_PER_DAY, s = us / 1000000;
    t->year = (uint16_t)y;
    t->month = (uint16_t)m;
    t->day = (uint16_t)d;
    t->hour = (uint16_t)(s / 3600);
    t->minute = (uint16_t)(s / 60 % 60);
    t->second = (uint16_t)(s % 60);
    t->microsecond = (uint32_t)(us % 1000000);
}

/* Adds months to a tick: the day is kept, or the month's last day when it is shorter. */
bool rtc_add_months(uint64_t tick, int64_t months, uint64_t *out) {
    rtc_datetime t;
    rtc_from_tick(tick, &t);
    int64_t total = (int64_t)t.year * 12 + (t.month - 1) + months;
    if (total < 12 || total >= 10000 * 12) return false;
    t.year = (uint16_t)(total / 12);
    t.month = (uint16_t)(total % 12 + 1);
    int dim = rtc_days_in_month(t.year, t.month);
    if (t.day > dim) t.day = (uint16_t)dim;
    *out = rtc_to_tick(&t);
    return true;
}

/* "Wed, 01 Jan 2020 12:00:00 +0900" */
void rtc_format_rfc2822(char *out, size_t n, uint64_t local_tick, int tz_minutes) {
    static const char *const wd[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const mo[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    rtc_datetime t;
    rtc_from_tick(local_tick, &t);
    int64_t days = (int64_t)(local_tick / US_PER_DAY);
    int tz = tz_minutes < 0 ? -tz_minutes : tz_minutes;
    snprintf(out, n, "%s, %02u %s %04u %02u:%02u:%02u %c%02d%02d", wd[(days + 1) % 7], t.day, mo[t.month - 1], t.year,
             t.hour, t.minute, t.second, tz_minutes < 0 ? '-' : '+', tz / 60, tz % 60);
}

/* "2020-01-01T12:00:00.00+09:00" (hundredths; precise: microseconds), Z at UTC */
void rtc_format_rfc3339(char *out, size_t n, uint64_t local_tick, int tz_minutes, bool precise) {
    rtc_datetime t;
    rtc_from_tick(local_tick, &t);
    char zone[8];
    int tz = tz_minutes < 0 ? -tz_minutes : tz_minutes;
    if (!tz_minutes) snprintf(zone, sizeof(zone), "Z");
    else snprintf(zone, sizeof(zone), "%c%02d:%02d", tz_minutes < 0 ? '-' : '+', tz / 60, tz % 60);
    if (precise)
        snprintf(out, n, "%04u-%02u-%02uT%02u:%02u:%02u.%06u%s", t.year, t.month, t.day, t.hour, t.minute, t.second,
                 t.microsecond, zone);
    else
        snprintf(out, n, "%04u-%02u-%02uT%02u:%02u:%02u.%02u%s", t.year, t.month, t.day, t.hour, t.minute, t.second,
                 t.microsecond / 10000, zone);
}

static bool digits(const char **p, int n, int *v) {
    *v = 0;
    for (int i = 0; i < n; i++) {
        if ((*p)[i] < '0' || (*p)[i] > '9') return false;
        *v = *v * 10 + ((*p)[i] - '0');
    }
    *p += n;
    return true;
}

/* RFC 3339 ("2020-01-01T12:00:00.5+09:00", 'T' or ' ', zone Z/±hh:mm or none = UTC) -> UTC tick */
bool rtc_parse_rfc3339(const char *s, uint64_t *utc) {
    int y, mo, d, h, mi, se;
    const char *p = s;
    while (*p == ' ') p++;
    if (!digits(&p, 4, &y) || *p++ != '-' || !digits(&p, 2, &mo) || *p++ != '-' || !digits(&p, 2, &d)) return false;
    if (*p != 'T' && *p != 't' && *p != ' ') return false;
    p++;
    if (!digits(&p, 2, &h) || *p++ != ':' || !digits(&p, 2, &mi) || *p++ != ':' || !digits(&p, 2, &se)) return false;
    uint32_t us = 0;
    if (*p == '.') {
        p++;
        uint32_t scale = 100000;
        while (*p >= '0' && *p <= '9') {
            us += (uint32_t)(*p - '0') * scale;
            scale /= 10;
            p++;
        }
    }
    int tz = 0;
    if (*p == 'Z' || *p == 'z') p++;
    else if (*p == '+' || *p == '-') {
        int sign = *p++ == '-' ? -1 : 1, th, tm;
        if (!digits(&p, 2, &th)) return false;
        if (*p == ':') p++;
        if (!digits(&p, 2, &tm)) return false;
        tz = sign * (th * 60 + tm);
    }
    rtc_datetime t = {(uint16_t)y, (uint16_t)mo, (uint16_t)d, (uint16_t)h, (uint16_t)mi, (uint16_t)se, us};
    if (rtc_check(&t)) return false;
    int64_t tick = (int64_t)rtc_to_tick(&t) - (int64_t)tz * 60000000;
    if (tick < 0) return false;
    *utc = (uint64_t)tick;
    return true;
}

/* RFC 2822 ("Wed, 01 Jan 2020 12:00:00 +0900", weekday optional) -> UTC tick */
bool rtc_parse_rfc2822(const char *s, uint64_t *utc) {
    static const char *const mo[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const char *p = s;
    while (*p == ' ') p++;
    const char *comma = strchr(p, ',');
    if (comma) p = comma + 1;
    while (*p == ' ') p++;
    int d = 0, y, h, mi, se = 0, m = 0;
    while (*p >= '0' && *p <= '9') d = d * 10 + (*p++ - '0');
    while (*p == ' ') p++;
    for (int i = 0; i < 12 && !m; i++)
        if (!strncmp(p, mo[i], 3)) m = i + 1;
    if (!m) return false;
    p += 3;
    while (*p == ' ') p++;
    if (!digits(&p, 4, &y)) return false;
    while (*p == ' ') p++;
    if (!digits(&p, 2, &h) || *p++ != ':' || !digits(&p, 2, &mi)) return false;
    if (*p == ':') {
        p++;
        if (!digits(&p, 2, &se)) return false;
    }
    while (*p == ' ') p++;
    int tz = 0;
    if (*p == '+' || *p == '-') {
        int sign = *p++ == '-' ? -1 : 1, hh;
        if (!digits(&p, 4, &hh)) return false;
        tz = sign * (hh / 100 * 60 + hh % 100);
    }
    rtc_datetime t = {(uint16_t)y, (uint16_t)m, (uint16_t)d, (uint16_t)h, (uint16_t)mi, (uint16_t)se, 0};
    if (rtc_check(&t)) return false;
    int64_t tick = (int64_t)rtc_to_tick(&t) - (int64_t)tz * 60000000;
    if (tick < 0) return false;
    *utc = (uint64_t)tick;
    return true;
}

/* ---- HLE ---- */

#define RTC_UNIX_EPOCH_TICKS 62135596800000000ull
#define RTC_WIN32_EPOCH_TICKS 50491123200000000ull /* 1601-01-01 */

static bool read_dt(hle_call *c, guest_addr a, rtc_datetime *t) { return a && gmem_read(c->mem, a, t, sizeof(*t)); }
static bool write_dt(hle_call *c, guest_addr a, const rtc_datetime *t) { return a && gmem_write(c->mem, a, t, sizeof(*t)); }
static bool read_tick(hle_call *c, guest_addr a, uint64_t *v) { return a && gmem_read_u64(c->mem, a, v); }
static bool write_tick(hle_call *c, guest_addr a, uint64_t v) { return a && gmem_write(c->mem, a, &v, 8); }

static uint64_t now_tick(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return RTC_UNIX_EPOCH_TICKS + (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* host time zone offset (minutes east of UTC) at a UTC tick */
static int local_minutes(uint64_t utc) {
    time_t t = (time_t)((int64_t)(utc / 1000000) - (int64_t)(RTC_UNIX_EPOCH_TICKS / 1000000));
    struct tm l, g;
    const struct tm *r = localtime(&t);
    if (!r) return 0;
    l = *r;
    if (!(r = gmtime(&t))) return 0;
    g = *r;
    int64_t dl = rtc_days_from_civil(l.tm_year + 1900, l.tm_mon + 1, l.tm_mday);
    int64_t dg = rtc_days_from_civil(g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
    return (int)((dl - dg) * 1440 + (l.tm_hour - g.tm_hour) * 60 + (l.tm_min - g.tm_min));
}

static void rtc_ok(hle_call *c) { hle_ret_s32(c, 0); }

static void rtc_check_valid(hle_call *c) {
    rtc_datetime t;
    hle_ret_s32(c, read_dt(c, hle_arg_ptr(c, 0), &t) ? rtc_check(&t) : RTC_ERR_INVALID_POINTER);
}

static void rtc_is_leap_year(hle_call *c) {
    int32_t y = hle_arg_s32(c, 0);
    hle_ret_s32(c, y < 1 ? RTC_ERR_INVALID_YEAR : rtc_is_leap(y));
}

static void rtc_get_days_in_month(hle_call *c) {
    int32_t y = hle_arg_s32(c, 0), m = hle_arg_s32(c, 1);
    hle_ret_s32(c, y < 1 ? RTC_ERR_INVALID_YEAR : m < 1 || m > 12 ? RTC_ERR_INVALID_MONTH : rtc_days_in_month(y, m));
}

/* int sceRtcGetDayOfWeek(int year, int month, int day): 0 Sunday .. 6 Saturday */
static void rtc_get_day_of_week(hle_call *c) {
    int32_t y = hle_arg_s32(c, 0), m = hle_arg_s32(c, 1), d = hle_arg_s32(c, 2);
    if (y < 1) { hle_ret_s32(c, RTC_ERR_INVALID_YEAR); return; }
    if (m < 1 || m > 12) { hle_ret_s32(c, RTC_ERR_INVALID_MONTH); return; }
    if (d < 1 || d > rtc_days_in_month(y, m)) { hle_ret_s32(c, RTC_ERR_INVALID_DAY); return; }
    hle_ret_s32(c, (int32_t)((rtc_days_from_civil(y, m, d) + 1) % 7)); /* 0001-01-01 was a Monday */
}

/* int sceRtcSetTick(SceRtcDateTime *out, const SceRtcTick *tick) */
static void rtc_set_tick(hle_call *c) {
    uint64_t tick;
    rtc_datetime t;
    if (!read_tick(c, hle_arg_ptr(c, 1), &tick)) { hle_ret_s32(c, RTC_ERR_INVALID_POINTER); return; }
    if (tick > RTC_TICK_MAX) { hle_ret_s32(c, RTC_ERR_INVALID_VALUE); return; }
    rtc_from_tick(tick, &t);
    hle_ret_s32(c, write_dt(c, hle_arg_ptr(c, 0), &t) ? 0 : RTC_ERR_INVALID_POINTER);
}

/* int sceRtcCompareTick(const SceRtcTick *a, const SceRtcTick *b): -1, 0, 1 */
static void rtc_compare_tick(hle_call *c) {
    uint64_t a, b;
    if (!read_tick(c, hle_arg_ptr(c, 0), &a) || !read_tick(c, hle_arg_ptr(c, 1), &b))
        { hle_ret_s32(c, RTC_ERR_INVALID_POINTER); return; }
    hle_ret_s32(c, a < b ? -1 : a > b ? 1 : 0);
}

/* sceRtcTickAdd*(SceRtcTick *out, const SceRtcTick *in, amount): 64-bit amounts for ticks,
 * microseconds, seconds and minutes, 32-bit for the larger units */
static void tick_add(hle_call *c, int64_t unit_us, bool wide) {
    uint64_t in;
    if (!read_tick(c, hle_arg_ptr(c, 1), &in)) { hle_ret_s32(c, RTC_ERR_INVALID_POINTER); return; }
    int64_t amount = wide ? (int64_t)hle_arg(c, 2) : (int64_t)hle_arg_s32(c, 2);
    int64_t r = (int64_t)in + amount * unit_us;
    if (r < 0 || (uint64_t)r > RTC_TICK_MAX) { hle_ret_s32(c, RTC_ERR_INVALID_VALUE); return; }
    hle_ret_s32(c, write_tick(c, hle_arg_ptr(c, 0), (uint64_t)r) ? 0 : RTC_ERR_INVALID_POINTER);
}
static void rtc_add_ticks(hle_call *c) { tick_add(c, 1, true); }
static void rtc_add_us(hle_call *c) { tick_add(c, 1, true); }
static void rtc_add_seconds(hle_call *c) { tick_add(c, 1000000, true); }
static void rtc_add_minutes(hle_call *c) { tick_add(c, 60000000, true); }
static void rtc_add_hours(hle_call *c) { tick_add(c, 3600000000ll, false); }
static void rtc_add_days(hle_call *c) { tick_add(c, 86400000000ll, false); }
static void rtc_add_weeks(hle_call *c) { tick_add(c, 7 * 86400000000ll, false); }

static void month_add(hle_call *c, int64_t factor) {
    uint64_t in, out;
    if (!read_tick(c, hle_arg_ptr(c, 1), &in)) { hle_ret_s32(c, RTC_ERR_INVALID_POINTER); return; }
    if (in > RTC_TICK_MAX || !rtc_add_months(in, (int64_t)hle_arg_s32(c, 2) * factor, &out))
        { hle_ret_s32(c, RTC_ERR_INVALID_VALUE); return; }
    hle_ret_s32(c, write_tick(c, hle_arg_ptr(c, 0), out) ? 0 : RTC_ERR_INVALID_POINTER);
}
static void rtc_add_months_fn(hle_call *c) { month_add(c, 1); }
static void rtc_add_years(hle_call *c) { month_add(c, 12); }

/* int sceRtcSetTime_t(SceRtcDateTime *out, time_t t) / GetTime_t(const SceRtcDateTime *in, time_t *out) */
static void rtc_set_time_t(hle_call *c) {
    int64_t t = (int64_t)hle_arg(c, 1);
    if (t < 0) { hle_ret_s32(c, RTC_ERR_INVALID_VALUE); return; }
    rtc_datetime dt;
    rtc_from_tick(RTC_UNIX_EPOCH_TICKS + (uint64_t)t * 1000000u, &dt);
    hle_ret_s32(c, write_dt(c, hle_arg_ptr(c, 0), &dt) ? 0 : RTC_ERR_INVALID_POINTER);
}
static void rtc_get_time_t(hle_call *c) {
    rtc_datetime dt;
    if (!read_dt(c, hle_arg_ptr(c, 0), &dt) || !hle_arg_ptr(c, 1)) { hle_ret_s32(c, RTC_ERR_INVALID_POINTER); return; }
    int32_t e = rtc_check(&dt);
    if (e) { hle_ret_s32(c, e); return; }
    uint64_t tick = rtc_to_tick(&dt);
    if (tick < RTC_UNIX_EPOCH_TICKS) { hle_ret_s32(c, RTC_ERR_INVALID_VALUE); return; }
    int64_t t = (int64_t)((tick - RTC_UNIX_EPOCH_TICKS) / 1000000u);
    hle_ret_s32(c, gmem_write(c->mem, hle_arg_ptr(c, 1), &t, 8) ? 0 : RTC_ERR_INVALID_POINTER);
}

/* DOS time: second/2 [4:0], minute [10:5], hour [15:11], day [20:16], month [24:21], year-1980 [31:25] */
static void rtc_set_dos_time(hle_call *c) {
    uint32_t v = hle_arg_u32(c, 1);
    rtc_datetime dt = {(uint16_t)((v >> 25) + 1980), (uint16_t)((v >> 21) & 15), (uint16_t)((v >> 16) & 31),
                       (uint16_t)((v >> 11) & 31), (uint16_t)((v >> 5) & 63), (uint16_t)((v & 31) * 2), 0};
    hle_ret_s32(c, write_dt(c, hle_arg_ptr(c, 0), &dt) ? 0 : RTC_ERR_INVALID_POINTER);
}
static void rtc_get_dos_time(hle_call *c) {
    rtc_datetime dt;
    if (!read_dt(c, hle_arg_ptr(c, 0), &dt) || !hle_arg_ptr(c, 1)) { hle_ret_s32(c, RTC_ERR_INVALID_POINTER); return; }
    int32_t e = rtc_check(&dt);
    if (e) { hle_ret_s32(c, e); return; }
    if (dt.year < 1980 || dt.year > 2107) { hle_ret_s32(c, RTC_ERR_INVALID_YEAR); return; }
    uint32_t v = ((uint32_t)(dt.year - 1980) << 25) | ((uint32_t)dt.month << 21) | ((uint32_t)dt.day << 16) |
                 ((uint32_t)dt.hour << 11) | ((uint32_t)dt.minute << 5) | (dt.second / 2u);
    hle_ret_s32(c, gmem_write(c->mem, hle_arg_ptr(c, 1), &v, 4) ? 0 : RTC_ERR_INVALID_POINTER);
}

/* Win32 FILETIME: 100 ns units since 1601-01-01 */
static void rtc_set_win32_file_time(hle_call *c) {
    uint64_t ft = hle_arg(c, 1), tick = RTC_WIN32_EPOCH_TICKS + ft / 10;
    if (tick > RTC_TICK_MAX) { hle_ret_s32(c, RTC_ERR_INVALID_VALUE); return; }
    rtc_datetime dt;
    rtc_from_tick(tick, &dt);
    hle_ret_s32(c, write_dt(c, hle_arg_ptr(c, 0), &dt) ? 0 : RTC_ERR_INVALID_POINTER);
}
static void rtc_get_win32_file_time(hle_call *c) {
    rtc_datetime dt;
    if (!read_dt(c, hle_arg_ptr(c, 0), &dt) || !hle_arg_ptr(c, 1)) { hle_ret_s32(c, RTC_ERR_INVALID_POINTER); return; }
    int32_t e = rtc_check(&dt);
    if (e) { hle_ret_s32(c, e); return; }
    uint64_t tick = rtc_to_tick(&dt);
    if (tick < RTC_WIN32_EPOCH_TICKS) { hle_ret_s32(c, RTC_ERR_INVALID_VALUE); return; }
    uint64_t ft = (tick - RTC_WIN32_EPOCH_TICKS) * 10;
    hle_ret_s32(c, write_tick(c, hle_arg_ptr(c, 1), ft) ? 0 : RTC_ERR_INVALID_POINTER);
}

/* network clocks: the host's (synchronized) */
static void rtc_current_tick(hle_call *c) {
    hle_ret_s32(c, write_tick(c, hle_arg_ptr(c, 0), now_tick()) ? 0 : RTC_ERR_INVALID_POINTER);
}

/* sceRtcFormatRFC*(char *out, const SceRtcTick *utc, int tz_minutes); the LocalTime variants
 * take no zone and use the host's. 64 bytes is the longest text plus room. */
static void format(hle_call *c, int kind, bool local) {
    uint64_t utc;
    char buf[64];
    if (!hle_arg_ptr(c, 0) || !read_tick(c, hle_arg_ptr(c, 1), &utc)) { hle_ret_s32(c, RTC_ERR_INVALID_POINTER); return; }
    if (utc > RTC_TICK_MAX) { hle_ret_s32(c, RTC_ERR_INVALID_VALUE); return; }
    int tz = local ? local_minutes(utc) : hle_arg_s32(c, 2);
    int64_t lt = (int64_t)utc + (int64_t)tz * 60000000;
    if (lt < 0 || (uint64_t)lt > RTC_TICK_MAX) { hle_ret_s32(c, RTC_ERR_INVALID_VALUE); return; }
    if (kind == 0) rtc_format_rfc2822(buf, sizeof(buf), (uint64_t)lt, tz);
    else rtc_format_rfc3339(buf, sizeof(buf), (uint64_t)lt, tz, kind == 2);
    hle_ret_s32(c, gmem_write(c->mem, hle_arg_ptr(c, 0), buf, strlen(buf) + 1) ? 0 : RTC_ERR_INVALID_POINTER);
}
static void rtc_format_2822(hle_call *c) { format(c, 0, false); }
static void rtc_format_2822_local(hle_call *c) { format(c, 0, true); }
static void rtc_format_3339(hle_call *c) { format(c, 1, false); }
static void rtc_format_3339_local(hle_call *c) { format(c, 1, true); }
static void rtc_format_3339_precise(hle_call *c) { format(c, 2, false); }
static void rtc_format_3339_precise_local(hle_call *c) { format(c, 2, true); }

/* int sceRtcParseRFC3339(SceRtcTick *utc, const char *text) / sceRtcParseDateTime (either form) */
static void parse(hle_call *c, bool any) {
    char text[128];
    if (!hle_arg_ptr(c, 0) || !hle_read_str(c, hle_arg_ptr(c, 1), text, sizeof(text)))
        { hle_ret_s32(c, RTC_ERR_INVALID_POINTER); return; }
    uint64_t utc;
    if (!rtc_parse_rfc3339(text, &utc) && !(any && rtc_parse_rfc2822(text, &utc)))
        { hle_ret_s32(c, RTC_ERR_INVALID_VALUE); return; }
    hle_ret_s32(c, write_tick(c, hle_arg_ptr(c, 0), utc) ? 0 : RTC_ERR_INVALID_POINTER);
}
static void rtc_parse_3339(hle_call *c) { parse(c, false); }
static void rtc_parse_date_time(hle_call *c) { parse(c, true); }

#define RTC "libSceRtc"
static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(RTC, "sceRtcInit", rtc_ok),
    HLE_FUNC_ENTRY(RTC, "sceRtcEnd", rtc_ok),
    HLE_FUNC_ENTRY(RTC, "sceRtcSetConf", rtc_ok),
    HLE_FUNC_ENTRY(RTC, "sceRtcCheckValid", rtc_check_valid),
    HLE_FUNC_ENTRY(RTC, "sceRtcIsLeapYear", rtc_is_leap_year),
    HLE_FUNC_ENTRY(RTC, "sceRtcGetDaysInMonth", rtc_get_days_in_month),
    HLE_FUNC_ENTRY(RTC, "sceRtcGetDayOfWeek", rtc_get_day_of_week),
    HLE_FUNC_ENTRY(RTC, "sceRtcSetTick", rtc_set_tick),
    HLE_FUNC_ENTRY(RTC, "sceRtcCompareTick", rtc_compare_tick),
    HLE_FUNC_ENTRY(RTC, "sceRtcTickAddTicks", rtc_add_ticks),
    HLE_FUNC_ENTRY(RTC, "sceRtcTickAddMicroseconds", rtc_add_us),
    HLE_FUNC_ENTRY(RTC, "sceRtcTickAddSeconds", rtc_add_seconds),
    HLE_FUNC_ENTRY(RTC, "sceRtcTickAddMinutes", rtc_add_minutes),
    HLE_FUNC_ENTRY(RTC, "sceRtcTickAddHours", rtc_add_hours),
    HLE_FUNC_ENTRY(RTC, "sceRtcTickAddDays", rtc_add_days),
    HLE_FUNC_ENTRY(RTC, "sceRtcTickAddWeeks", rtc_add_weeks),
    HLE_FUNC_ENTRY(RTC, "sceRtcTickAddMonths", rtc_add_months_fn),
    HLE_FUNC_ENTRY(RTC, "sceRtcTickAddYears", rtc_add_years),
    HLE_FUNC_ENTRY(RTC, "sceRtcSetTime_t", rtc_set_time_t),
    HLE_FUNC_ENTRY(RTC, "sceRtcGetTime_t", rtc_get_time_t),
    HLE_FUNC_ENTRY(RTC, "sceRtcSetDosTime", rtc_set_dos_time),
    HLE_FUNC_ENTRY(RTC, "sceRtcGetDosTime", rtc_get_dos_time),
    HLE_FUNC_ENTRY(RTC, "sceRtcSetWin32FileTime", rtc_set_win32_file_time),
    HLE_FUNC_ENTRY(RTC, "sceRtcGetWin32FileTime", rtc_get_win32_file_time),
    HLE_FUNC_ENTRY(RTC, "sceRtcGetCurrentAdNetworkTick", rtc_current_tick),
    HLE_FUNC_ENTRY(RTC, "sceRtcGetCurrentDebugNetworkTick", rtc_current_tick),
    HLE_FUNC_ENTRY(RTC, "sceRtcGetCurrentRawNetworkTick", rtc_current_tick),
    /* the system clock is the host's: setting it is accepted and ignored */
    HLE_FUNC_ENTRY(RTC, "sceRtcSetCurrentTick", rtc_ok),
    HLE_FUNC_ENTRY(RTC, "sceRtcSetCurrentNetworkTick", rtc_ok),
    HLE_FUNC_ENTRY(RTC, "sceRtcSetCurrentAdNetworkTick", rtc_ok),
    HLE_FUNC_ENTRY(RTC, "sceRtcSetCurrentDebugNetworkTick", rtc_ok),
    HLE_FUNC_ENTRY(RTC, "sceRtcFormatRFC2822", rtc_format_2822),
    HLE_FUNC_ENTRY(RTC, "sceRtcFormatRFC2822LocalTime", rtc_format_2822_local),
    HLE_FUNC_ENTRY(RTC, "sceRtcFormatRFC3339", rtc_format_3339),
    HLE_FUNC_ENTRY(RTC, "sceRtcFormatRFC3339LocalTime", rtc_format_3339_local),
    HLE_FUNC_ENTRY(RTC, "sceRtcFormatRFC3339Precise", rtc_format_3339_precise),
    HLE_FUNC_ENTRY(RTC, "sceRtcFormatRFC3339PreciseLocalTime", rtc_format_3339_precise_local),
    HLE_FUNC_ENTRY(RTC, "sceRtcParseRFC3339", rtc_parse_3339),
    HLE_FUNC_ENTRY(RTC, "sceRtcParseDateTime", rtc_parse_date_time),
};

void hle_register_rtc(void) { hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports)); }
