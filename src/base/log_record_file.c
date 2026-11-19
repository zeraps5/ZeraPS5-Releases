/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "base/log_record_file.h"
#include <string.h>
#include <inttypes.h>

static void save_status(log_record_file *file) {
    file->state.rejected = atomic_load_explicit(&file->rejected, memory_order_relaxed);
    file->state.busy = atomic_load_explicit(&file->busy, memory_order_relaxed);
    if (!file->status) return;
    /* Fixed width, bounded and unbuffered. Incomplete status is rejected by the reader. */
    if (fseek(file->status, 0, SEEK_SET)) return;
    const log_record_file_status *s = &file->state;
    (void)fprintf(file->status, "ZRS1 %020" PRIu64 " %020" PRIu64 " %020" PRIu64 " %020" PRIu64
                  " %020" PRIu64 " %020" PRIu64 " %020" PRIu64 " %020" PRIu64 " END\n",
                  s->open_error, s->write_error, s->flush_error, s->setup_error,
                  s->rejected, s->limited, s->busy, s->finalized);
}

void log_record_file_init(log_record_file *file, FILE *records, FILE *status, uint64_t limit) {
    memset(file, 0, sizeof(*file));
    atomic_flag_clear(&file->writing);
    atomic_init(&file->rejected, 0); atomic_init(&file->busy, 0);
    file->records = records; file->status = status; file->limit = limit;
    file->write = fwrite; file->flush = fflush;
    file->state.open_error = records == NULL;
    if (status && setvbuf(status, NULL, _IONBF, 0)) { file->status = NULL; }
    /* No pending CRT buffer when TerminateProcess skips atexit. Do not call
     * fflush(NULL), add a teardown lock, or recurse into the logger on failure. */
    if (records && setvbuf(records, NULL, _IONBF, 0)) {
        file->state.setup_error = 1; file->records = NULL;
    }
    save_status(file);
}

void log_record_file_sink(void *user, const void *data, size_t size) {
    log_record_file *file = user;
    /* Never wait for another logging thread (including a suspended/faulting
     * writer). Contention loses optional records, explicitly counted. */
    if (atomic_flag_test_and_set_explicit(&file->writing, memory_order_acquire)) {
        atomic_fetch_add_explicit(&file->busy, 1, memory_order_relaxed); return;
    }
    log_record_file_status before = file->state;
    if (!file->records || file->state.write_error || file->state.flush_error) goto done;
    if (file->state.limited) goto done;
    if (size > file->limit - file->bytes) { file->state.limited = 1; goto done; }
    size_t written = file->write(data, 1, size, file->records);
    file->bytes += written;
    if (written != size) file->state.write_error = 1;
    /* Critical events reach the OS before the next instruction can terminate
     * the process. This is process-exit durability, not power-loss durability. */
    if (size >= 10 && ((const unsigned char *)data)[9] >= LOG_LVL_WARN && file->flush(file->records))
        file->state.flush_error = 1;
done:
    if (memcmp(&before, &file->state, sizeof(before)) ||
        atomic_load_explicit(&file->rejected, memory_order_relaxed) != file->state.rejected ||
        atomic_load_explicit(&file->busy, memory_order_relaxed) != file->state.busy) save_status(file);
    atomic_flag_clear_explicit(&file->writing, memory_order_release);
}

void log_record_file_rejected(void *user) {
    log_record_file *file = user;
    atomic_fetch_add_explicit(&file->rejected, 1, memory_order_relaxed);
    if (!atomic_flag_test_and_set_explicit(&file->writing, memory_order_acquire)) {
        save_status(file); atomic_flag_clear_explicit(&file->writing, memory_order_release);
    }
}

void log_record_file_finish(log_record_file *file) {
    if (atomic_flag_test_and_set_explicit(&file->writing, memory_order_acquire)) return;
    if (file->records && file->flush(file->records)) file->state.flush_error = 1;
    file->state.finalized = 1; save_status(file);
    atomic_flag_clear_explicit(&file->writing, memory_order_release);
    /* The CRT owns streams and configuration remains immutable during teardown. */
}

bool log_record_file_status_read(FILE *file, log_record_file_status *status) {
    log_record_file_status s = {0}; char text[180];
    if(!fgets(text,sizeof(text),file) || strlen(text)!=177 || memcmp(text,"ZRS1 ",5))return false;
    const char *p=text+5;
    uint64_t *fields[]={&s.open_error,&s.write_error,&s.flush_error,&s.setup_error,&s.rejected,&s.limited,&s.busy,&s.finalized};
    for(unsigned n=0;n<8;n++) {
        uint64_t value=0;
        for(unsigned digit=0;digit<20;digit++,p++) {
            if(*p<'0' || *p>'9' || value>(UINT64_MAX-(unsigned)(*p-'0'))/10)return false;
            value=value*10+(unsigned)(*p-'0');
        }
        if(*p++!=' ')return false;
        *fields[n]=value;
    }
    if (strcmp(p,"END\n") || fgetc(file)!=EOF || ferror(file) || s.open_error > 1 || s.write_error > 1 ||
        s.flush_error > 1 || s.setup_error > 1 || s.limited > 1 || s.finalized > 1) return false;
    *status = s; return true;
}
