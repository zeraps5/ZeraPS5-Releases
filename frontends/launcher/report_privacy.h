/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ZERAPS5_REPORT_PRIVACY_H
#define ZERAPS5_REPORT_PRIVACY_H
#include <ctype.h>
#include <stdbool.h>
#include <string.h>
static bool report_contains(const char *text, const char *needle) {
    if (!needle || !*needle) return false;
    for (; *text; text++) {
        size_t n = 0;
        while (needle[n] && text[n] && tolower((unsigned char)text[n]) == tolower((unsigned char)needle[n])) n++;
        if (!needle[n]) return true;
    }
    return false;
}
/* Drop whole lines: partial path replacement can expose surnames after spaces. */
static bool report_private_line(const char *line, const char *user, const char *computer) {
    if (report_contains(line, user) || report_contains(line, computer)) return true;
    const char *sensitive[] = {"\\\\", "@", "Authorization:", "Bearer ", "password", "access_token", "api_key", "token=", "secret=", "Cookie:", "github_pat_", "ghp_", "C:/Users/", "C:\\Users\\"};
    for (size_t n = 0; n < sizeof(sensitive)/sizeof(sensitive[0]); n++)
        if (report_contains(line, sensitive[n])) return true;
    for (const char *p = line; p[0] && p[1] && p[2]; p++)
        if (isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '/' || p[2] == '\\')) return true;
    return false;
}
#endif
