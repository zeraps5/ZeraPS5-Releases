/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_LOG_SESSION_H
#define PS5C_LOG_SESSION_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define LOG_SESSION_ID 33
#define LOG_SESSION_VERSION 81
enum { SESSION_PREPARED=1,SESSION_CREATED,SESSION_RUNNING,SESSION_ENDED,SESSION_FAILED,SESSION_UNAVAILABLE };
typedef struct log_session_meta {
    char id[LOG_SESSION_ID],version[LOG_SESSION_VERSION];
    uint32_t role,build,stage,creation_known,creation_ok,creation_error;
    uint32_t exit_known,exit_code,guest_known;
    int64_t guest_code;
} log_session_meta;
bool log_session_id_valid(const char *id);
bool log_session_meta_valid(const log_session_meta *meta);
bool log_session_header_write(FILE *file,const char *id);
/* 1 valid identity, 0 legacy (rewound), -1 invalid identity/I/O. */
int log_session_header_read(FILE *file,char *id);
bool log_session_meta_write(FILE *file,const log_session_meta *meta);
bool log_session_meta_read(FILE *file,log_session_meta *meta);
#endif
