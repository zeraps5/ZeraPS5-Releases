/* SPDX-License-Identifier: GPL-2.0-or-later */
/* These checks include required I/O and synchronization; keep them enabled in
 * Release builds as well as development builds. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "base/log_record.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void discard_raw(void *user,log_level level,const char *data,size_t size) {
    (void)user;(void)level;(void)data;(void)size;
}
static DWORD WINAPI writer(void *user) {
    unsigned id=(unsigned)(uintptr_t)user;
    for(unsigned n=0;n<250;n++)log_emit(LOG_CAT_GPU,LOG_LVL_INFO,"thread %u item %u",id,n);
    return 0;
}
#endif

static FILE *capture;
static char raw[4096];
static void record_sink(void *user,const void *data,size_t size) { assert(fwrite(data,1,size,user)==size); }
static void raw_sink(void *user,log_level level,const char *data,size_t size) {
    (void)user;(void)level;assert(size<sizeof(raw));memcpy(raw,data,size);raw[size]=0;
}
int main(void) {
    capture=tmpfile();assert(capture);
    log_set_sink(raw_sink,NULL);
    log_set_record_sink(record_sink,capture);
    log_emit(LOG_CAT_GPU,LOG_LVL_WARN,"buffer %08x: %.*s %.2f %%",42,3,"abcdef",1.25);
    assert(!strcmp(raw,"[WARN ] [GPU] buffer 0000002a: abc 1.25 %\n"));
    rewind(capture);
    log_record *r=malloc(sizeof(*r));assert(r);
    assert(log_record_read(capture,r)==1);
    assert(r->count==3 && r->category==LOG_CAT_GPU && r->level==LOG_LVL_WARN);
    char out[256];
    assert(log_record_render(r,"búfer %08x: %.*s %.2f %%",out,sizeof(out)));
    assert(!strcmp(out,"búfer 0000002a: abc 1.25 %"));
    assert(!log_record_render(r,"wrong %s %.*s %.2f %%",out,sizeof(out)));
    assert(!log_record_render(r,"missing %08x %.*s",out,sizeof(out)));
    assert(!log_record_render(r,r->format,out,5));
    assert(log_record_read(capture,r)==0);fclose(capture);
    capture=tmpfile();assert(capture);log_set_record_sink(record_sink,capture);
    log_emit(LOG_CAT_CPU,LOG_LVL_INFO,"width %*.*f string=%ls empty=%s",8,2,2.5,L"wide","");
    log_emit(LOG_CAT_CORE,LOG_LVL_INFO,"plain text 100%%");
    rewind(capture);
    assert(log_record_read(capture,r)==1);
    assert(log_record_render(r,r->format,out,sizeof(out)));
    assert(!strcmp(out,"width     2.50 string=wide empty="));
    assert(log_record_read(capture,r)==1 && r->count==0);
    assert(log_record_render(r,"texto 100%%",out,sizeof(out)) && !strcmp(out,"texto 100%"));
    assert(log_record_read(capture,r)==0);fclose(capture);
    capture=tmpfile();assert(capture);log_set_record_sink(record_sink,capture);
    /* Values containing the record delimiter must not corrupt the next record. */
    log_emit(LOG_CAT_CORE,LOG_LVL_INFO,"value %s","a\x1f" "b");
    log_emit(LOG_CAT_CORE,LOG_LVL_INFO,"after malformed value");
    rewind(capture);assert(log_record_read(capture,r)==1 && !strcmp(r->format,"after malformed value"));
    assert(log_record_read(capture,r)==0);fclose(capture);
    capture=tmpfile();assert(capture);fwrite("ZLR1",1,4,capture);rewind(capture);
    assert(log_record_read(capture,r)==-1);fclose(capture);
#ifdef _WIN32
    capture=tmpfile();assert(capture);
    log_set_sink(discard_raw,NULL);log_set_record_sink(record_sink,capture);
    HANDLE threads[4];
    for(unsigned i=0;i<4;i++) { threads[i]=CreateThread(NULL,0,writer,(void *)(uintptr_t)i,0,NULL);assert(threads[i]); }
    assert(WaitForMultipleObjects(4,threads,TRUE,10000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<4;i++)CloseHandle(threads[i]);
    bool seen[4][250]={{false}};unsigned count=0;
    rewind(capture);
    while(log_record_read(capture,r)==1) {
        assert(r->count==2);
        unsigned id=(unsigned)strtoul(r->fields[0],NULL,10),item=(unsigned)strtoul(r->fields[1],NULL,10);
        assert(id<4 && item<250 && !seen[id][item]);seen[id][item]=true;count++;
    }
    assert(count==1000);fclose(capture);log_set_sink(raw_sink,NULL);
#endif
    log_set_record_sink(NULL,NULL);assert(!log_record_enabled());
    log_emit(LOG_CAT_CORE,LOG_LVL_INFO,"original %d",7);
    assert(!strcmp(raw,"[INFO ] [CORE] original 7\n"));
    free(r);puts("Structured logs preserve arguments and raw output; corruption rejected");return 0;
}
