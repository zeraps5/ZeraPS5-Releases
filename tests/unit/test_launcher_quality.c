/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
/* Compile-time network tripwire: this fixture cannot send even on regression. */
static unsigned requests;
static BOOL WINAPI fixture_send_request(HINTERNET h,LPCWSTR a,DWORD b,LPVOID c,DWORD d,DWORD e,DWORD_PTR f) {
    (void)h;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;requests++;return FALSE;
}
#define WinHttpSendRequest fixture_send_request
int WINAPI launcher_unused_entry(HINSTANCE inst,HINSTANCE prev,PWSTR cmdline,int show);
#define wWinMain launcher_unused_entry
#include "../../frontends/launcher/launcher.c"
#undef wWinMain

static wchar_t raw_path[MAX_PATH],record_path[MAX_PATH],status_path[MAX_PATH];
static void sink(void *user,const void *p,size_t n) {assert(fwrite(p,1,n,user)==n);}
static void discard(void *u,log_level l,const char *p,size_t n) {(void)u;(void)l;(void)p;(void)n;}
static FILE *fixture(void) {
    DeleteFileW(status_path);
    FILE *raw=_wfopen(raw_path,L"wb");assert(raw);fputs("SECRET_RAW_MUST_NEVER_BE_PACKAGED\n",raw);fclose(raw);
    FILE *file=_wfopen(record_path,L"wb");assert(file);
    log_set_sink(discard,NULL);log_set_record_sink(sink,file);return file;
}
static void stop(FILE *file) {log_set_record_sink(NULL,NULL);fclose(file);}
static report_log_quality assess(char *text,size_t cap) {
    FILE *out=tmpfile();assert(out);report_log_quality q;assert(collect_log(raw_path,out,true,&q));
    rewind(out);size_t n=fread(text,1,cap-1,out);text[n]=0;fclose(out);
    assert(!strstr(text,"SECRET_RAW_MUST_NEVER_BE_PACKAGED"));return q;
}
static void status_fixture(unsigned limited) {
    FILE *records=_wfopen(record_path,L"ab"),*status=_wfopen(status_path,L"wb");assert(records && status);
    log_record_file writer;log_record_file_init(&writer,records,status,64u*1024u*1024u);
    writer.state.limited=limited;log_record_file_finish(&writer);fclose(records);fclose(status);
}
int wmain(int argc,wchar_t **argv);
int wmain(int argc,wchar_t **argv) {
    assert(argc==2);wcsncpy(g_base,argv[1],MAX_PATH-1);assert(SetCurrentDirectoryW(g_base));
    CreateDirectoryW(L"user",NULL);CreateDirectoryW(L"user\\logs",NULL);CreateDirectoryW(L"user\\reports",NULL);
    g_global=ini_parse(user_config_default_text,strlen(user_config_default_text));assert(g_global);
    /* Explicitly disable external destinations regardless of user preferences. */
    ini_set(g_global,"relatorios","servidor","");ini_set(g_global,"relatorios","enviar_ao_fechar","0");
    game sample={0};wcscpy(sample.id,L"QTEST");wcscpy(sample.title,L"Synthetic quality fixture");wcscpy(sample.version,L"TEST");
    log_path(&sample,L"",raw_path);swprintf(record_path,MAX_PATH,L"%ls.records",raw_path);
    swprintf(status_path,MAX_PATH,L"%ls.status",record_path);
    DeleteFileW(record_path);DeleteFileW(status_path);
    char text[32768];report_log_quality q=assess(text,sizeof(text));assert(q.missing && !q.useful && !strcmp(report_log_state(&q),"missing"));
    FILE *file=fixture();stop(file);q=assess(text,sizeof(text));assert(q.empty && !q.useful && !strcmp(report_log_state(&q),"empty"));
    file=fixture();fwrite("ZLR1",1,4,file);stop(file);q=assess(text,sizeof(text));assert(q.corrupt && !q.useful);
    file=fixture();log_emit(LOG_CAT_CORE,LOG_LVL_INFO,"%s: %s","safe","synthetic event");fwrite("bad",1,3,file);stop(file);
    q=assess(text,sizeof(text));assert(q.corrupt && q.read==1 && q.rendered==1 && q.useful==1 && !report_log_complete(&q));
    file=fixture();log_emit(LOG_CAT_CORE,LOG_LVL_INFO,"unknown synthetic format %s","safe");stop(file);
    q=assess(text,sizeof(text));assert(q.untranslated && q.omitted==1 && !q.useful);
    file=fixture();log_emit(LOG_CAT_CORE,LOG_LVL_INFO,"%s: %s","private","fictitious@example.invalid");stop(file);
    q=assess(text,sizeof(text));assert(q.all_filtered && q.filtered==1 && !q.useful && !strstr(text,"example.invalid"));
    file=fixture();log_emit(LOG_CAT_CORE,LOG_LVL_INFO,"%s: %s","private","safe\nAuthorization: Bearer FICTIONAL-SECRET");stop(file);
    q=assess(text,sizeof(text));assert(q.all_filtered && !strstr(text,"FICTIONAL-SECRET"));
    char long_value[8101];memset(long_value,'x',sizeof(long_value)-1);long_value[8100]=0;
    file=fixture();log_emit(LOG_CAT_CORE,LOG_LVL_INFO,"%s: %s","private",long_value);stop(file);
    q=assess(text,sizeof(text));assert(q.all_filtered && !q.useful);
    file=fixture();log_emit(LOG_CAT_CORE,LOG_LVL_INFO,"%s: %s","safe","synthetic event\nsecond safe line");stop(file);
    q=assess(text,sizeof(text));assert(q.useful==1 && q.useful_lines==2 && report_log_complete(&q));
    status_fixture(1);q=assess(text,sizeof(text));assert(q.limited && !q.corrupt && !report_log_complete(&q));
    DeleteFileW(status_path);file=_wfopen(record_path,L"wb");assert(file);const char marker[20]="ZLC1";fwrite(marker,1,20,file);fclose(file);
    q=assess(text,sizeof(text));assert(q.limited && !q.corrupt);
    file=fixture();log_emit(LOG_CAT_CORE,LOG_LVL_INFO,"%s: %s","safe","synthetic event");stop(file);status_fixture(0);
    q=assess(text,sizeof(text));assert(q.status_known && q.channel.finalized && report_log_complete(&q));
    file=_wfopen(status_path,L"wb");assert(file);fputs("ZRS1 incomplete",file);fclose(file);
    q=assess(text,sizeof(text));assert(q.status_invalid && !report_log_complete(&q));
    DeleteFileW(status_path);
    FILE *status=_wfopen(status_path,L"wb");assert(status);
    log_record_file failed;log_record_file_init(&failed,NULL,status,1024);fclose(status);
    q=assess(text,sizeof(text));assert(q.status_known && q.channel.open_error && !strcmp(report_log_state(&q),"io_error"));
    DeleteFileW(status_path);
    status=_wfopen(status_path,L"wb");assert(status);FILE *records=_wfopen(record_path,L"ab");assert(records);
    log_record_file_init(&failed,records,status,1024*1024);
    q=assess(text,sizeof(text));assert(q.partial && !report_log_complete(&q));
    log_record_file_finish(&failed);fclose(records);fclose(status);DeleteFileW(status_path);
    for(unsigned locale=0;locale<UI_LANGUAGE_COUNT;locale++) {
        g_ui_locale=locale;
        assert(!report_description_useful(ui_translate(locale,L"(escreva aqui)")));
        wchar_t upper[4000];
        assert(LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_UPPERCASE,ui_translate(locale,L"(escreva aqui)"),-1,upper,4000,NULL,NULL,0));
        assert(!report_description_useful(upper));
        assert(!report_description_useful(L" \t\r\n\u200b\u00a0\u2003\u2060\ufeff"));
        assert(!report_description_useful(L"fictitious@example.invalid\nC:/Users/Private User/save\ntoken=FICTIONAL-TOKEN"));
        assert(report_description_useful(L"Synthetic steps: open menu, app stops."));
        assert(report_dialog_choice(IDOK,L"")==-2);
        assert(report_dialog_choice(ID_REPORT_SAVE,L"")==0);
        assert(report_dialog_choice(IDCANCEL,L"Synthetic steps: open menu, app stops.")==-1);
    }
    g_ui_locale=ui_language_index("en");
    assert(!report_description_useful(NULL));assert(!report_description_useful(L"...................."));
    assert(!report_description_useful(L"abcd\x01\x02\x03\x04\u200e\u200f\u061c\u00ad"));
    assert(report_description_useful(L"起動した後メニュー画面で停止しました"));
    wchar_t zip[MAX_PATH];q=assess(text,sizeof(text));
    assert(!build_report(&sample,L"",REPORT_MANUAL,zip,NULL));
    assert(!build_report(&sample,L"(type here)",REPORT_MANUAL,zip,NULL));
    assert(!build_report(&sample,L"fictitious@example.invalid",REPORT_MANUAL,zip,NULL));
    assert(!report_send_allowed(REPORT_DRAFT,true,&q));
    assert(build_report(&sample,L"Synthetic steps: open menu, app stops.\nfictitious@example.invalid",REPORT_MANUAL,zip,&q));
    assert(q.useful==1);wprintf(L"USEFUL_ZIP=%ls\n",zip);
    DeleteFileW(record_path);DeleteFileW(status_path);
    assert(!build_report(&sample,L"",REPORT_AUTOMATIC,zip,NULL));
    assert(build_report(&sample,L"Synthetic steps: open menu, app stops.",REPORT_MANUAL,zip,&q));
    assert(q.missing && !q.useful);wprintf(L"MISSING_ZIP=%ls\n",zip);
    assert(build_report(&sample,L"",REPORT_DRAFT,zip,&q));wprintf(L"DRAFT_ZIP=%ls\n",zip);
    assert(!requests);assert(!ini_get_int(g_global,"relatorios","enviar_ao_fechar",0));ini_free(g_global);
    puts("Quality states, post-filter descriptions in 15 locales, local drafts and zero HTTP requests verified");return 0;
}
