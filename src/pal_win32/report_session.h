/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_REPORT_SESSION_H
#define PS5C_REPORT_SESSION_H
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "base/log_session.h"
typedef struct report_session_index { char current[33],previous[33],pending[33]; } report_session_index;
typedef struct report_session_store {
    wchar_t root[MAX_PATH];
    wchar_t playtime[MAX_PATH];
    HANDLE lock;
    report_session_index index;
    bool indexed;
} report_session_store;
typedef struct report_session_snapshot {
    report_session_store store;
    log_session_meta current,previous,core,previous_core;
    bool legacy,current_known,previous_known,core_known,previous_core_known;
    bool current_identity_bad,previous_identity_bad;
    wchar_t raw[MAX_PATH],previous_raw[MAX_PATH];
} report_session_snapshot;
/* Filename mapping only; no authentication or reporter identity. */
bool report_session_game_key(const wchar_t *game,wchar_t key[48]);
bool report_session_open(const wchar_t *base,const wchar_t *game,report_session_store *store);
void report_session_close(report_session_store *store);
bool report_session_path(const report_session_store *store,const char *id,const wchar_t *name,wchar_t *out);
bool report_session_meta_save(const wchar_t *path,const log_session_meta *meta);
bool report_session_meta_load(const wchar_t *path,log_session_meta *meta);
bool report_session_begin(report_session_store *store,const char *version,uint32_t build,log_session_meta *meta,HANDLE *raw);
bool report_session_created(report_session_store *store,log_session_meta *meta,bool ok,DWORD error);
bool report_session_stage(report_session_store *store,log_session_meta *meta,uint32_t stage);
bool report_session_ended(report_session_store *store,log_session_meta *meta,bool known,DWORD code);
uint64_t report_session_playtime(const wchar_t *base,const wchar_t *game);
/* Add milliseconds while holding the per-game session lease. */
bool report_session_add_playtime(const report_session_store *store,uint64_t elapsed,uint64_t *total);
bool report_session_snapshot_open(const wchar_t *base,const wchar_t *game,report_session_snapshot *snapshot);
void report_session_snapshot_close(report_session_snapshot *snapshot);
#endif
