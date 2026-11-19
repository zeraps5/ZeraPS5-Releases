/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ZERAPS5_REPORT_QUALITY_H
#define ZERAPS5_REPORT_QUALITY_H
#include "base/log_record_file.h"
typedef struct report_log_quality {
    uint64_t read, rendered, omitted, filtered, useful, useful_lines;
    bool missing, empty, corrupt, limited, untranslated, all_filtered, io_error;
    bool status_known, status_invalid, partial;
    bool session_unverified,identity_mismatch;
    log_record_file_status channel;
} report_log_quality;
static const char *report_log_state(const report_log_quality *q) {
    if (q->io_error || q->channel.open_error || q->channel.write_error ||
        q->channel.flush_error || q->channel.setup_error) return "io_error";
    if (q->corrupt || q->status_invalid) return "damaged";
    if (q->limited) return "size_limited";
    if (q->missing) return "missing";
    if (q->empty) return "empty";
    if (q->all_filtered) return "fully_filtered";
    if (q->untranslated) return "not_rendered";
    if (q->channel.rejected || q->channel.busy || q->partial) return "partial";
    return q->useful ? "useful" : "not_rendered";
}
static bool report_log_complete(const report_log_quality *q) {
    return q->useful && !q->io_error && !q->corrupt && !q->limited && !q->status_invalid && !q->partial &&
           !q->channel.open_error && !q->channel.write_error && !q->channel.flush_error &&
           !q->channel.setup_error && !q->channel.rejected && !q->channel.busy && !q->omitted && !q->filtered;
}
#endif
