/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceRtc calendar helpers (lib_rtc.c), exposed for the unit test. */
#ifndef PS5C_HLE_LIB_RTC_H
#define PS5C_HLE_LIB_RTC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* SceRtcDateTime */
typedef struct rtc_datetime {
    uint16_t year, month, day, hour, minute, second;
    uint32_t microsecond;
} rtc_datetime;

bool rtc_is_leap(int64_t y);
int rtc_days_in_month(int64_t y, int64_t m);
int64_t rtc_days_from_civil(int64_t y, int64_t m, int64_t d);
void rtc_civil_from_days(int64_t days, int64_t *y, int64_t *m, int64_t *d);
int32_t rtc_check(const rtc_datetime *t);
uint64_t rtc_to_tick(const rtc_datetime *t);
void rtc_from_tick(uint64_t tick, rtc_datetime *t);
bool rtc_add_months(uint64_t tick, int64_t months, uint64_t *out);
void rtc_format_rfc2822(char *out, size_t n, uint64_t local_tick, int tz_minutes);
void rtc_format_rfc3339(char *out, size_t n, uint64_t local_tick, int tz_minutes, bool precise);
bool rtc_parse_rfc3339(const char *s, uint64_t *utc);
bool rtc_parse_rfc2822(const char *s, uint64_t *utc);

#endif
