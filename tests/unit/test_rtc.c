/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceRtc calendar arithmetic: ticks, dates, months, DOS/RFC text. */
#include <stdio.h>
#include <string.h>

#include "hle/lib_rtc.h"

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#define UNIX_EPOCH 62135596800000000ull

int main(void) {
    /* the Unix epoch and back */
    rtc_datetime t = {1970, 1, 1, 0, 0, 0, 0};
    CHECK(rtc_to_tick(&t) == UNIX_EPOCH);
    rtc_datetime u;
    rtc_from_tick(UNIX_EPOCH + 86400000000ull * 19723 + 3723000004ull, &u); /* 2024-01-01 01:02:03.000004 */
    CHECK(u.year == 2024 && u.month == 1 && u.day == 1 && u.hour == 1 && u.minute == 2 && u.second == 3 && u.microsecond == 4);
    /* every day of four centuries round-trips */
    for (int64_t d = 0; d < 146097 * 2; d += 7) {
        int64_t y, m, dd;
        rtc_civil_from_days(d, &y, &m, &dd);
        CHECK(rtc_days_from_civil(y, m, dd) == d);
    }
    CHECK(rtc_is_leap(2000) && !rtc_is_leap(1900) && rtc_is_leap(2024) && !rtc_is_leap(2023));
    CHECK(rtc_days_in_month(2024, 2) == 29 && rtc_days_in_month(2023, 2) == 28 && rtc_days_in_month(2023, 4) == 30);
    /* 2024-01-01 was a Monday: (days + 1) % 7 == 1 */
    CHECK((rtc_days_from_civil(2024, 1, 1) + 1) % 7 == 1);
    rtc_datetime bad = {2023, 2, 29, 0, 0, 0, 0};
    CHECK(rtc_check(&bad) == (int32_t)0x80B5000A);
    bad.day = 28, bad.hour = 24;
    CHECK(rtc_check(&bad) == (int32_t)0x80B5000B);
    /* months keep the day or clamp to the month's last */
    rtc_datetime j31 = {2024, 1, 31, 12, 0, 0, 0};
    uint64_t out;
    CHECK(rtc_add_months(rtc_to_tick(&j31), 1, &out));
    rtc_from_tick(out, &u);
    CHECK(u.year == 2024 && u.month == 2 && u.day == 29 && u.hour == 12);
    CHECK(rtc_add_months(rtc_to_tick(&j31), -13, &out));
    rtc_from_tick(out, &u);
    CHECK(u.year == 2022 && u.month == 12 && u.day == 31);
    CHECK(!rtc_add_months(rtc_to_tick(&j31), 12 * 8000, &out));
    /* text */
    char buf[64];
    rtc_datetime n = {2020, 1, 1, 12, 0, 0, 250000};
    rtc_format_rfc2822(buf, sizeof(buf), rtc_to_tick(&n), 540);
    CHECK(!strcmp(buf, "Wed, 01 Jan 2020 12:00:00 +0900"));
    rtc_format_rfc3339(buf, sizeof(buf), rtc_to_tick(&n), 0, false);
    CHECK(!strcmp(buf, "2020-01-01T12:00:00.25Z"));
    rtc_format_rfc3339(buf, sizeof(buf), rtc_to_tick(&n), -180, true);
    CHECK(!strcmp(buf, "2020-01-01T12:00:00.250000-03:00"));
    uint64_t utc;
    CHECK(rtc_parse_rfc3339("2020-01-01T12:00:00.25+09:00", &utc));
    rtc_from_tick(utc, &u);
    CHECK(u.year == 2020 && u.day == 1 && u.hour == 3 && u.microsecond == 250000);
    CHECK(rtc_parse_rfc2822("Wed, 01 Jan 2020 12:00:00 -0300", &utc));
    rtc_from_tick(utc, &u);
    CHECK(u.hour == 15 && u.minute == 0);
    CHECK(!rtc_parse_rfc3339("2020-13-01T00:00:00Z", &utc));
    printf("rtc: ok\n");
    return 0;
}
