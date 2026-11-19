/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_LOG_RECORD_FILE_H
#define PS5C_LOG_RECORD_FILE_H
#include "base/log_record.h"
#include <stdatomic.h>

/* Numeric, privacy-safe status. No paths, formats, arguments or environment. */
typedef struct log_record_file_status {
    uint64_t open_error, write_error, flush_error, setup_error;
    uint64_t rejected, limited, busy, finalized;
} log_record_file_status;
typedef struct log_record_file {
    FILE *records, *status;
    atomic_flag writing;
    atomic_uint_fast64_t rejected, busy;
    uint64_t bytes, limit;
    log_record_file_status state;
    /* Injectable I/O for deterministic local failure tests. */
    size_t (*write)(const void *, size_t, size_t, FILE *);
    int (*flush)(FILE *);
} log_record_file;
void log_record_file_init(log_record_file *file, FILE *records, FILE *status, uint64_t limit);
void log_record_file_sink(void *user, const void *data, size_t size);
void log_record_file_rejected(void *user);
void log_record_file_finish(log_record_file *file);
bool log_record_file_status_read(FILE *file, log_record_file_status *status);
#endif
