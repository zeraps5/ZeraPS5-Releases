/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "base/log_record_file.h"
#include "pal/pal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static void discard(void *u,log_level l,const char *p,size_t n) {(void)u;(void)l;(void)p;(void)n;}
static log_record_file output;
static void start(FILE *records,FILE *status,uint64_t limit) {
    log_record_file_init(&output,records,status,limit);
    log_set_record_sink(log_record_file_sink,&output);
    log_set_record_reject_sink(log_record_file_rejected,&output);
    log_set_sink(discard,NULL);
}
static log_record_file_status read_status(FILE *file) {
    rewind(file);log_record_file_status s;assert(log_record_file_status_read(file,&s));return s;
}
static size_t short_write(const void *p,size_t size,size_t count,FILE *f) {return fwrite(p,size,count/2,f);}
static int fail_flush(FILE *f) {(void)f;return EOF;}
static DWORD WINAPI writer(void *arg) {
    unsigned id=(unsigned)(uintptr_t)arg;
    for(unsigned i=0;i<250;i++)log_emit(LOG_CAT_CORE,LOG_LVL_INFO,"worker %u item %u",id,i);
    return 0;
}
int wmain(int argc,wchar_t **argv);
int wmain(int argc,wchar_t **argv) {
    if(argc==3 && !wcscmp(argv[1],L"terminate")) {
        wchar_t status_path[MAX_PATH];swprintf(status_path,MAX_PATH,L"%ls.status",argv[2]);
        FILE *records=_wfopen(argv[2],L"wb"),*status=_wfopen(status_path,L"wb");
        assert(records && status);start(records,status,1024*1024);
        log_emit(LOG_CAT_CORE,LOG_LVL_ERROR,"critical synthetic event %u",17u);
        /* Exact production exit path; no pal_init, guest, renderer or GPU. */
        pal_exit_process(17);return 99;
    }
    assert(argc==2);
    FILE *records=tmpfile(),*status=tmpfile();assert(records && status);
    start(NULL,status,1024);assert(read_status(status).open_error);fclose(status);
    status=tmpfile();start(records,status,1);
    log_emit(LOG_CAT_CORE,LOG_LVL_WARN,"bounded record");
    assert(read_status(status).limited);rewind(records);log_record r;assert(log_record_read(records,&r)==0);
    fclose(records);fclose(status);

    records=tmpfile();status=tmpfile();start(records,status,1024*1024);output.write=short_write;
    log_emit(LOG_CAT_CORE,LOG_LVL_ERROR,"short write");assert(read_status(status).write_error);
    fclose(records);fclose(status);
    records=tmpfile();status=tmpfile();start(records,status,1024*1024);output.flush=fail_flush;
    log_emit(LOG_CAT_CORE,LOG_LVL_ERROR,"flush failure");assert(read_status(status).flush_error);
    fclose(records);fclose(status);

    records=tmpfile();status=tmpfile();start(records,status,1024*1024);
    log_emit(LOG_CAT_CORE,LOG_LVL_INFO,"value %s","a\x1f" "b");
    assert(read_status(status).rejected==1);
    atomic_flag_test_and_set(&output.writing);
    log_emit(LOG_CAT_CORE,LOG_LVL_ERROR,"must not wait for busy writer");
    atomic_flag_clear(&output.writing);
    log_emit(LOG_CAT_CORE,LOG_LVL_INFO,"persist updated counters");
    assert(read_status(status).busy==1);log_record_file_finish(&output);
    assert(read_status(status).finalized);fclose(records);fclose(status);

    records=tmpfile();status=tmpfile();start(records,status,1024*1024);
    HANDLE threads[4];for(unsigned i=0;i<4;i++)threads[i]=CreateThread(NULL,0,writer,(void *)(uintptr_t)i,0,NULL);
    assert(WaitForMultipleObjects(4,threads,TRUE,10000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<4;i++)CloseHandle(threads[i]);
    log_record_file_finish(&output);log_record_file_status state=read_status(status);
    rewind(records);unsigned read=0;int result;while((result=log_record_read(records,&r))==1)read++;
    assert(result==0 && read+state.busy==1000 && read>0 && !state.write_error && !state.flush_error);
    fclose(records);fclose(status);
    log_set_record_sink(NULL,NULL);log_set_record_reject_sink(NULL,NULL);

    status=tmpfile();assert(status);start(NULL,status,1024);
    char malformed[180];rewind(status);assert(fread(malformed,1,177,status)==177);
    memset(malformed+5,'9',20);rewind(status);assert(fwrite(malformed,1,177,status)==177);rewind(status);
    assert(!log_record_file_status_read(status,&state));fclose(status);
    status=tmpfile();assert(status);start(NULL,status,1024);
    fseek(status,0,SEEK_END);fputc('x',status);rewind(status);
    assert(!log_record_file_status_read(status,&state));fclose(status);
    log_set_record_sink(NULL,NULL);log_set_record_reject_sink(NULL,NULL);

    wchar_t exe[MAX_PATH],cmd[3*MAX_PATH],path[MAX_PATH],status_path[MAX_PATH];
    GetModuleFileNameW(NULL,exe,MAX_PATH);swprintf(path,MAX_PATH,L"%ls\\terminated.records",argv[1]);
    swprintf(status_path,MAX_PATH,L"%ls.status",path);
    swprintf(cmd,3*MAX_PATH,L"\"%ls\" terminate \"%ls\"",exe,path);
    STARTUPINFOW si={0};si.cb=sizeof(si);PROCESS_INFORMATION pi;
    assert(CreateProcessW(exe,cmd,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi));
    assert(WaitForSingleObject(pi.hProcess,10000)==WAIT_OBJECT_0);DWORD code=0;GetExitCodeProcess(pi.hProcess,&code);
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);assert(code==17);
    records=_wfopen(path,L"rb");assert(records && log_record_read(records,&r)==1);
    assert(!strcmp(r.format,"critical synthetic event %u") && !strcmp(r.fields[0],"17"));
    assert(log_record_read(records,&r)==0);fclose(records);
    status=_wfopen(status_path,L"rb");assert(status);state=read_status(status);fclose(status);
    assert(!state.finalized && !state.write_error && !state.flush_error);
    puts("Unbuffered records survive production termination; open/write/flush/limit/reject/contention states verified");
    return 0;
}
