/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_REPORT_ARCHIVE_H
#define PS5C_REPORT_ARCHIVE_H
#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define REPORT_ARCHIVE_FILES 5
typedef struct report_archive_entry {
    char name[64];
    wchar_t path[MAX_PATH];
    uint32_t size,crc,offset;
} report_archive_entry;
bool report_archive_prepare(report_archive_entry *entries,unsigned count);
/* ZIP STORE: bounded streaming, no subprocess, no external tool or extraction. */
bool report_archive_write(const wchar_t *path,report_archive_entry *entries,unsigned count);
bool report_archive_verify(const wchar_t *path,const report_archive_entry *entries,unsigned count);
#endif
