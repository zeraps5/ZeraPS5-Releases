/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <io.h>
static unsigned requests;
static BOOL WINAPI no_network(HINTERNET h,LPCWSTR a,DWORD b,LPVOID c,DWORD d,DWORD e,DWORD_PTR f) {
    (void)h;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;requests++;return FALSE;
}
#define WinHttpSendRequest no_network
int WINAPI launcher_unused_entry(HINSTANCE inst,HINSTANCE prev,PWSTR cmdline,int show);
#define wWinMain launcher_unused_entry
#include "../../frontends/launcher/launcher.c"
#undef wWinMain
static game sample;
static unsigned fault;
static wchar_t last_output[MAX_PATH];
static void discard(void *u,log_level l,const char *p,size_t n){(void)u;(void)l;(void)p;(void)n;}
static FILE *fault_open(const wchar_t *path,const wchar_t *mode) {
    wcsncpy(last_output,path,MAX_PATH-1);last_output[MAX_PATH-1]=0;
    if(fault==1&&wcsstr(path,L"info.txt"))return NULL;
    if(fault==4&&wcsstr(path,L".log")) {
        FILE *f=_wfopen(path,L"wb");assert(f);fclose(f);return _wfopen(path,L"rb");
    }
    return _wfopen(path,mode);
}
static int fault_close(FILE *f) {
    bool quality=wcsstr(last_output,L"report-quality.txt")!=NULL,session=wcsstr(last_output,L"session-info.txt")!=NULL;
    if(fault==3&&quality) {assert(!fflush(f));assert(!_chsize_s(_fileno(f),12));}
    int result=fclose(f);
    if(fault==9&&session)assert(DeleteFileW(last_output));
    return fault==2&&quality?EOF:result;
}
static bool fault_archive(const wchar_t *path,report_archive_entry *entries,unsigned count) {
    if(fault==5) {FILE *f=_wfopen(path,L"wb");assert(f);fputs("PARTIAL",f);fclose(f);return false;}
    if(fault==7)strcpy(entries[0].name,"unexpected.txt");
    if(fault==8) {FILE *f=_wfopen(entries[0].path,L"ab");assert(f);fputs("CHANGED",f);fclose(f);}
    bool ok=report_archive_write(path,entries,count);
    if(ok&&fault==6) {FILE *f=_wfopen(path,L"ab");assert(f);fputc('x',f);fclose(f);}
    return ok;
}
static unsigned count_files(const wchar_t *pattern) {
    WIN32_FIND_DATAW data;HANDLE f=FindFirstFileW(pattern,&data);unsigned count=0;
    if(f==INVALID_HANDLE_VALUE)return 0;
    do {count++;}while(FindNextFileW(f,&data));FindClose(f);return count;
}
static uint64_t fingerprint(const wchar_t *path) {
    FILE *f=_wfopen(path,L"rb");assert(f);uint64_t hash=UINT64_C(14695981039346656037);int c;
    while((c=fgetc(f))!=EOF)hash=(hash^(unsigned char)c)*UINT64_C(1099511628211);
    assert(!ferror(f));fclose(f);return hash;
}
static void clean_temps(void) {
    assert(count_files(L"user\\reports\\*.partial")==0);
    assert(count_files(L"user\\reports\\zqr*.tmp")==0);
}
static void records(report_session_store *store,const log_session_meta *m,const char *value,unsigned rows) {
    wchar_t path[MAX_PATH],status_path[MAX_PATH];assert(report_session_path(store,m->id,L"run.log.records",path));
    swprintf(status_path,MAX_PATH,L"%ls.status",path);
    FILE *file=_wfopen(path,L"wb"),*status=_wfopen(status_path,L"wb");assert(file&&status);
    log_record_file writer;log_record_file_init(&writer,file,status,64u*1024u*1024u);
    assert(log_session_header_write(file,m->id));
    log_set_sink(discard,NULL);log_set_record_sink(log_record_file_sink,&writer);
    for(unsigned n=0;n<rows;n++)log_emit(LOG_CAT_CORE,LOG_LVL_INFO,"%s: %s","synthetic",value);
    log_set_record_sink(NULL,NULL);log_record_file_finish(&writer);fclose(file);fclose(status);
}
static void core(report_session_store *store,const log_session_meta *m,bool guest,int64_t code) {
    log_session_meta meta={.role=2,.stage=4,.build=701,.guest_known=guest,.guest_code=code};
    strcpy(meta.id,m->id);strcpy(meta.version,"2.3.4+fixture");
    wchar_t path[MAX_PATH];assert(report_session_path(store,m->id,L"core.meta",path));assert(report_session_meta_save(path,&meta));
}
static void snapshot_text(const report_session_snapshot *snap,char *text,size_t cap) {
    FILE *f=tmpfile();assert(f);assert(report_session_info_write(f,snap));rewind(f);
    size_t n=fread(text,1,cap-1,f);text[n]=0;fclose(f);
}
static void test_playtime(void) {
    const wchar_t *id=L"PLAYTIME-TEST";
    report_session_store store;
    uint64_t total;
    assert(report_session_open(g_base,id,&store));
    DeleteFileW(store.playtime);
    assert(report_session_playtime(g_base,id)==0);
    assert(report_session_add_playtime(&store,750,&total)&&total==750);
    report_session_close(&store);
    assert(report_session_playtime(g_base,id)==750);
    assert(report_session_open(g_base,id,&store));
    assert(report_session_add_playtime(&store,1725,&total)&&total==2475);
    assert(report_session_playtime(g_base,L"ANOTHER-PLAYTIME-TEST")==0);
    FILE *bad=_wfopen(store.playtime,L"wb");assert(bad);
    fputs("invalid\n",bad);assert(!fclose(bad));
    uint64_t original=fingerprint(store.playtime);
    assert(!report_session_add_playtime(&store,1000,&total));
    assert(fingerprint(store.playtime)==original);
    assert(DeleteFileW(store.playtime));
    report_session_close(&store);
}
int wmain(int argc,wchar_t **argv);
int wmain(int argc,wchar_t **argv) {
    assert(argc==2);wcsncpy(g_base,argv[1],MAX_PATH-1);assert(SetCurrentDirectoryW(g_base));
    CreateDirectoryW(L"user",NULL);CreateDirectoryW(L"user\\logs",NULL);CreateDirectoryW(L"user\\reports",NULL);
    test_playtime();
    g_global=ini_parse(user_config_default_text,strlen(user_config_default_text));assert(g_global);
    ini_set(g_global,"relatorios","servidor","");ini_set(g_global,"relatorios","enviar_ao_fechar","0");
    wcscpy(sample.id,L"SESSIONTEST");wcscpy(sample.title,L"Synthetic session fixture");wcscpy(sample.version,L"TEST");
    g_ui_locale=ui_language_index("en");
    report_session_store store;log_session_meta a,b,c,d,e;HANDLE raw;
    wchar_t safe[48],again[48];
    assert(report_session_game_key(L"Jogo sem ID 日本語",safe)&&report_session_game_key(L"Jogo sem ID 日本語",again)&&!wcscmp(safe,again));
    assert(wcslen(safe)==42&&!wcspbrk(safe,L"\\/:*?\"<>| "));
    assert(report_session_game_key(L"CON",again)&&_wcsicmp(again,L"CON")!=0);
    assert(report_session_game_key(L"..\\escape",again)&&!wcspbrk(again,L"\\/:*?\"<>| "));
    assert(report_session_open(g_base,L"Jogo sem ID 日本語",&store));
    log_session_meta unnamed;assert(report_session_begin(&store,"1.0.0+fixture",600,&unnamed,&raw));CloseHandle(raw);
    assert(report_session_created(&store,&unnamed,false,ERROR_FILE_NOT_FOUND));report_session_close(&store);
    report_session_snapshot unnamed_snapshot;assert(report_session_snapshot_open(g_base,L"Jogo sem ID 日本語",&unnamed_snapshot));
    assert(unnamed_snapshot.current_known&&!strcmp(unnamed_snapshot.current.id,unnamed.id));report_session_snapshot_close(&unnamed_snapshot);

    assert(report_session_open(g_base,sample.id,&store));
    assert(report_session_begin(&store,"1.2.3+fixture",601,&a,&raw));CloseHandle(raw);
    assert(report_session_created(&store,&a,true,0));assert(report_session_stage(&store,&a,SESSION_RUNNING));
    records(&store,&a,"SYNTHETIC-A-ONLY",1);core(&store,&a,true,-9);
    assert(report_session_ended(&store,&a,true,5));report_session_close(&store);
    report_session_snapshot snap;assert(report_session_snapshot_open(g_base,sample.id,&snap));
    assert(!snap.legacy&&snap.current_known&&snap.core_known&&!strcmp(snap.current.id,a.id)&&!snap.current_identity_bad);
    char text[8192];snapshot_text(&snap,text,sizeof(text));
    assert(strstr(text,"current_process_exit=5\n")&&strstr(text,"current_guest_exit=-9\n"));
    assert(strstr(text,"current_launcher_build=601\n")&&strstr(text,"current_core_build=701\n"));
    report_session_snapshot_close(&snap);

    /* Atomic index replacement fails while a real non-delete-sharing reader holds it. */
    assert(report_session_open(g_base,sample.id,&store));
    assert(report_session_begin(&store,"1.2.3+fixture",602,&b,&raw));CloseHandle(raw);
    wchar_t path[MAX_PATH];swprintf(path,MAX_PATH,L"%ls\\index",store.root);
    HANDLE blocked=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);assert(blocked!=INVALID_HANDLE_VALUE);
    assert(!report_session_created(&store,&b,true,0));assert(!strcmp(store.index.current,a.id));
    CloseHandle(blocked);report_session_close(&store);
    assert(report_session_snapshot_open(g_base,sample.id,&snap));
    assert(!strcmp(snap.current.id,b.id)&&snap.current.stage==SESSION_UNAVAILABLE&&!strcmp(snap.previous.id,a.id));
    report_session_snapshot_close(&snap);
    assert(report_session_open(g_base,sample.id,&store));
    assert(report_session_begin(&store,"1.2.3+fixture",603,&c,&raw));CloseHandle(raw);
    assert(report_session_created(&store,&c,false,ERROR_FILE_NOT_FOUND));report_session_close(&store);
    assert(report_session_snapshot_open(g_base,sample.id,&snap));
    snapshot_text(&snap,text,sizeof(text));
    assert(!strcmp(snap.current.id,c.id)&&!strcmp(snap.previous.id,a.id));
    assert(strstr(text,"current_process_creation=failed\n")&&strstr(text,"current_process_exit=unknown\n"));
    report_session_snapshot_close(&snap);

    assert(report_session_open(g_base,sample.id,&store));
    assert(report_session_begin(&store,"1.2.3+fixture",604,&d,&raw));CloseHandle(raw);
    assert(report_session_created(&store,&d,true,0));assert(!strcmp(store.index.previous,a.id));
    assert(report_session_stage(&store,&d,SESSION_RUNNING));records(&store,&d,"SYNTHETIC-D-ONLY",1);
    report_session_store other;assert(!report_session_open(g_base,sample.id,&other));
    g_proc.hProcess=CreateEventW(NULL,TRUE,FALSE,NULL);assert(g_proc.hProcess);
    wchar_t zip[MAX_PATH];assert(!build_report(&sample,L"Synthetic steps: open menu, app stops.",REPORT_MANUAL,zip,NULL));
    CloseHandle(g_proc.hProcess);
    assert(!report_collection_idle()); /* A failed wait is not proof of exit. */
    assert(!build_report(&sample,L"Synthetic steps: open menu, app stops.",REPORT_MANUAL,zip,NULL));
    g_proc.hProcess=NULL;
    report_session_close(&store);
    /* Even after a lost launcher lease, an unconfirmed active stage cannot be collected. */
    assert(!report_session_snapshot_open(g_base,sample.id,&snap));
    assert(report_session_open(g_base,sample.id,&store));
    assert(report_session_ended(&store,&d,false,0));report_session_close(&store);
    assert(report_session_snapshot_open(g_base,sample.id,&snap));snapshot_text(&snap,text,sizeof(text));
    assert(strstr(text,"current_process_exit=unknown\n")&&strstr(text,"current_guest_exit=unknown\n"));
    assert(strstr(text,"current_core_version=unknown\n"));report_session_snapshot_close(&snap);
    game second=sample;wcscpy(second.id,L"OTHERGAME");
    session_unavailable(&sample);session_unavailable(&second);session_available(&second);
    assert(session_collection_blocked(&sample)&&!session_collection_blocked(&second));
    assert(!build_report(&sample,L"Synthetic steps: open menu, app stops.",REPORT_MANUAL,zip,NULL));session_available(&sample);

    /* Previous structured data is included even when the corresponding raw log is absent. */
    assert(report_session_open(g_base,sample.id,&store));assert(report_session_path(&store,a.id,L"run.log",path));assert(DeleteFileW(path));
    report_session_close(&store);
    CreateDirectoryW(L"user\\reports\\tmp",NULL);FILE *poison=_wfopen(L"user\\reports\\tmp\\poison.txt",L"wb");assert(poison);
    fputs("PRIVATE_RESIDUE_MUST_NOT_APPEAR",poison);fclose(poison);
    report_log_quality q;
    assert(build_report(&sample,L"Synthetic steps: open menu, app stops.",REPORT_MANUAL,zip,&q));
    assert(q.useful&&!q.session_unverified&&!q.identity_mismatch);
    wchar_t preserved[MAX_PATH];wcscpy(preserved,zip);uint64_t original=fingerprint(preserved);
    unsigned before=count_files(L"user\\reports\\*.zip");
    g_report_package_io=(report_package_io){fault_open,fault_close,fault_archive,report_archive_verify};
    for(fault=1;fault<=9;fault++) {
        assert(!build_report(&sample,L"Synthetic steps: open menu, app stops.",REPORT_MANUAL,zip,NULL));
        assert(count_files(L"user\\reports\\*.zip")==before&&fingerprint(preserved)==original);clean_temps();
    }
    fault=0;g_report_package_io=(report_package_io){_wfopen,fclose,report_archive_write,report_archive_verify};
    /* An identity mismatch is never packaged as the current session's diagnostic. */
    assert(report_session_open(g_base,sample.id,&store));
    assert(report_session_path(&store,d.id,L"run.log.records",path));FILE *wrong=_wfopen(path,L"r+b");assert(wrong);
    assert(log_session_header_write(wrong,a.id));fclose(wrong);report_session_close(&store);
    assert(build_report(&sample,L"Synthetic steps: open menu, app stops.",REPORT_MANUAL,zip,&q));
    assert(q.identity_mismatch&&q.corrupt&&!q.useful);clean_temps();
    assert(report_session_open(g_base,sample.id,&store));records(&store,&d,"SYNTHETIC-D-RESTORED",1);
    core(&store,&d,true,0);report_session_close(&store);
    assert(report_session_snapshot_open(g_base,sample.id,&snap));snapshot_text(&snap,text,sizeof(text));
    assert(strstr(text,"current_guest_exit=0\n")&&strstr(text,"current_process_exit=unknown\n"));report_session_snapshot_close(&snap);
    assert(report_session_open(g_base,sample.id,&store));assert(report_session_path(&store,d.id,L"core.meta",path));
    log_session_meta sensitive={.role=2,.stage=4,.build=9};strcpy(sensitive.id,d.id);strcpy(sensitive.version,"1.0.0+ghp_FAKE");
    assert(report_session_meta_save(path,&sensitive));report_session_close(&store);
    assert(!build_report(&sample,L"Synthetic steps: open menu, app stops.",REPORT_MANUAL,zip,NULL));clean_temps();
    assert(report_session_open(g_base,sample.id,&store));core(&store,&d,true,0);
    char large[257];memset(large,'L',256);large[256]=0;records(&store,&d,large,24000);report_session_close(&store);
    assert(build_report(&sample,L"Synthetic steps: open menu, app stops.",REPORT_MANUAL,zip,&q));
    assert(q.limited&&q.useful&&q.omitted&&!report_log_complete(&q));clean_temps();assert(fingerprint(preserved)==original);
    /* An unconfirmed older current session is not substituted for the last ended session. */
    assert(report_session_open(g_base,sample.id,&store));
    assert(report_session_stage(&store,&d,SESSION_RUNNING));report_session_close(&store);
    assert(report_session_open(g_base,sample.id,&store));
    assert(report_session_begin(&store,"1.2.3+fixture",605,&e,&raw));CloseHandle(raw);
    assert(report_session_created(&store,&e,true,0));assert(!strcmp(store.index.previous,a.id));
    assert(report_session_stage(&store,&e,SESSION_RUNNING));records(&store,&e,"SYNTHETIC-E-ONLY",1);
    assert(report_session_ended(&store,&e,true,0));report_session_close(&store);
    assert(report_session_snapshot_open(g_base,sample.id,&snap));snapshot_text(&snap,text,sizeof(text));
    assert(!strcmp(snap.current.id,e.id)&&!strcmp(snap.previous.id,a.id));
    assert(strstr(text,"current_process_exit=0\n"));report_session_snapshot_close(&snap);
    assert(build_report(&sample,L"Synthetic steps: open menu, app stops.",REPORT_MANUAL,zip,&q));
    assert(q.useful&&!q.identity_mismatch);clean_temps();
    assert(!requests&&!ini_get_int(g_global,"relatorios","enviar_ao_fechar",0));ini_free(g_global);
    puts("COL04-06: session identity/rotation, active leases, separate unknown/zero exits, nine I/O/archive failures and bounded packages passed; zero HTTP requests");
    return 0;
}
