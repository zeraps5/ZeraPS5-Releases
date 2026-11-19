/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pal_win32/report_session.h"
#include "base/sha1.h"
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <errno.h>
#include <io.h>
#include <fcntl.h>
static bool uuid(char *out) {
    GUID g;
    if(FAILED(CoCreateGuid(&g)))return false;
    snprintf(out,33,"%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x",
        (unsigned long)g.Data1,g.Data2,g.Data3,g.Data4[0],g.Data4[1],g.Data4[2],g.Data4[3],g.Data4[4],g.Data4[5],g.Data4[6],g.Data4[7]);
    return log_session_id_valid(out);
}
static bool directory(const wchar_t *path) {
    if(!CreateDirectoryW(path,NULL)&&GetLastError()!=ERROR_ALREADY_EXISTS)return false;
    DWORD attr=GetFileAttributesW(path);
    return attr!=INVALID_FILE_ATTRIBUTES && (attr&FILE_ATTRIBUTE_DIRECTORY) && !(attr&FILE_ATTRIBUTE_REPARSE_POINT);
}
static bool device_name(const wchar_t *name) {
    if(!_wcsicmp(name,L"CON")||!_wcsicmp(name,L"PRN")||!_wcsicmp(name,L"AUX")||!_wcsicmp(name,L"NUL"))return true;
    return wcslen(name)==4 && (!_wcsnicmp(name,L"COM",3)||!_wcsnicmp(name,L"LPT",3)) && name[3]>=L'1'&&name[3]<=L'9';
}
bool report_session_game_key(const wchar_t *game,wchar_t key[48]) {
    if(!game||!*game||wcslen(game)>31)return false;
    bool plain=!device_name(game);
    for(const wchar_t *p=game;*p;p++)if(!((*p>=L'A'&&*p<=L'Z')||(*p>=L'a'&&*p<=L'z')||(*p>=L'0'&&*p<=L'9')||*p==L'_'||*p==L'-'))plain=false;
    if(plain){wcscpy(key,game);return true;}
    /* Existing SHA-1 maps a short UTF-16 name to a filename only. It is not
     * authentication. Session identities remain fresh opaque UUIDs. */
    uint8_t digest[20];sha1(game,wcslen(game)*sizeof(*game),digest);
    const wchar_t hex[]=L"0123456789abcdef";
    key[0]=L'x';key[1]=L'_';
    for(unsigned n=0;n<20;n++){key[2+2*n]=hex[digest[n]>>4];key[3+2*n]=hex[digest[n]&15];}
    key[42]=0;
    return true;
}
static bool index_id(const char *id) { return !*id || log_session_id_valid(id);
}
static bool atomic_file(const wchar_t *path,bool (*write)(FILE *,const void *),const void *data) {
    /* Core startup must not generate UUIDs or initialize RPC before guest
     * address reservation. Temporary names are local-only, atomically reserved,
     * and never exported; the session identity itself remains an opaque UUID. */
    static LONG sequence;
    wchar_t tmp[MAX_PATH];
    DWORD serial=(DWORD)InterlockedIncrement(&sequence);
    if(swprintf(tmp,MAX_PATH,L"%ls.%08lx.%08lx.new",path,(unsigned long)GetCurrentProcessId(),(unsigned long)serial)<0)return false;
    HANDLE handle=CreateFileW(tmp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(handle==INVALID_HANDLE_VALUE)return false;
    int fd=_open_osfhandle((intptr_t)handle,_O_WRONLY|_O_BINARY|_O_NOINHERIT);
    if(fd<0){CloseHandle(handle);DeleteFileW(tmp);return false;}
    FILE *f=_fdopen(fd,"wb");
    if(!f){_close(fd);DeleteFileW(tmp);return false;}
    bool ok=write(f,data);
    if(fflush(f)||ferror(f))ok=false;
    if(fclose(f))ok=false;
    if(ok)ok=MoveFileExW(tmp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if(!ok)DeleteFileW(tmp);
    return ok;
}
static bool write_meta(FILE *f,const void *data) {
    return log_session_meta_write(f,data);
}
bool report_session_meta_save(const wchar_t *path,const log_session_meta *meta) {
    return log_session_meta_valid(meta)&&atomic_file(path,write_meta,meta);
}
bool report_session_meta_load(const wchar_t *path,log_session_meta *meta) {
    FILE *f=_wfopen(path,L"rb");
    if(!f)return false;
    bool ok=log_session_meta_read(f,meta);
    fclose(f);
    return ok;
}
static bool index_line(FILE *f,const char *key,char *id) {
    char text[80];
    if(!fgets(text,sizeof(text),f))return false;
    size_t k=strlen(key),n=strlen(text);
    if(strncmp(text,key,k)||n<=k||text[n-1]!='\n')return false;
    text[n-1]=0;
    const char *value=text+k;
    if(!strcmp(value,"-")){*id=0;
        return true;
    }
    if(!log_session_id_valid(value))return false;
    memcpy(id,value,33);
    return true;
}
static bool load_index(report_session_store *s) {
    wchar_t path[MAX_PATH];
    if(swprintf(path,MAX_PATH,L"%ls\\index",s->root)<0)return false;
    FILE *f=_wfopen(path,L"rb");
    if(!f)return errno==ENOENT;
    s->indexed=true;
    char line[16];
    report_session_index i={0};
    bool ok=fgets(line,sizeof(line),f)&&!strcmp(line,"ZIX1\n")&&index_line(f,"current=",i.current)&&
    index_line(f,"previous=",i.previous)&&index_line(f,"pending=",i.pending)&&
    fgets(line,sizeof(line),f)&&!strcmp(line,"END\n")&&fgetc(f)==EOF&&!ferror(f);
    fclose(f);
    if(ok && ((*i.current && !strcmp(i.current,i.previous)) || (*i.pending && (!strcmp(i.pending,i.current)||!strcmp(i.pending,i.previous)))))ok=false;
    if(ok)s->index=i;
    return ok;
}
static bool write_index(FILE *f,const void *data) {
    const report_session_index *index=data;
    return index_id(index->current)&&index_id(index->previous)&&index_id(index->pending)&&
        fprintf(f,"ZIX1\ncurrent=%s\nprevious=%s\npending=%s\nEND\n",
            *index->current?index->current:"-",*index->previous?index->previous:"-",*index->pending?index->pending:"-")>0;
}
static bool save_index(report_session_store *s,const report_session_index *index) {
    wchar_t path[MAX_PATH];
    if(swprintf(path,MAX_PATH,L"%ls\\index",s->root)<0)return false;
    if(!atomic_file(path,write_index,index))return false;
    s->index=*index;
    s->indexed=true;
    return true;
}
static bool playtime_path(const wchar_t *base,const wchar_t *key,wchar_t path[MAX_PATH]) {
    return swprintf(path,MAX_PATH,L"%ls\\user\\playtime\\%ls.txt",base,key)>=0;
}
static bool read_playtime(const wchar_t *path,uint64_t *total) {
    *total=0;
    FILE *f=_wfopen(path,L"rb");
    if(!f)return errno==ENOENT;
    char line[32],*end;
    bool ok=false;
    if(fgets(line,sizeof(line),f)&&line[0]>='0'&&line[0]<='9') {
        errno=0;
        uint64_t value=strtoull(line,&end,10);
        ok=errno==0&&!strcmp(end,"\n")&&fgetc(f)==EOF&&!ferror(f);
        if(ok)*total=value;
    }
    fclose(f);
    return ok;
}
uint64_t report_session_playtime(const wchar_t *base,const wchar_t *game) {
    wchar_t key[48],path[MAX_PATH];
    uint64_t total=0;
    if(report_session_game_key(game,key)&&playtime_path(base,key,path))read_playtime(path,&total);
    return total;
}
static bool write_playtime(FILE *f,const void *data) {
    const uint64_t *total=data;
    return fprintf(f,"%llu\n",(unsigned long long)*total)>0;
}
bool report_session_add_playtime(const report_session_store *s,uint64_t elapsed,uint64_t *total) {
    if(!s->lock||s->lock==INVALID_HANDLE_VALUE||!*s->playtime)return false;
    wchar_t dir[MAX_PATH];
    wcscpy(dir,s->playtime);
    wchar_t *slash=wcsrchr(dir,L'\\');
    if(!slash)return false;
    *slash=0;
    uint64_t previous;
    if(!directory(dir)||!read_playtime(s->playtime,&previous)||elapsed>UINT64_MAX-previous)return false;
    uint64_t sum=previous+elapsed;
    if(!atomic_file(s->playtime,write_playtime,&sum))return false;
    *total=sum;
    return true;
}
bool report_session_open(const wchar_t *base,const wchar_t *game,report_session_store *s) {
    memset(s,0,sizeof(*s));
    s->lock=INVALID_HANDLE_VALUE;
    wchar_t key[48];if(!report_session_game_key(game,key))return false;
    if(!playtime_path(base,key,s->playtime))s->playtime[0]=0;
    wchar_t path[MAX_PATH];
    if(swprintf(path,MAX_PATH,L"%ls\\user",base)<0||!directory(path))return false;
    if(swprintf(path,MAX_PATH,L"%ls\\user\\logs",base)<0||!directory(path))return false;
    if(swprintf(s->root,MAX_PATH,L"%ls\\user\\logs\\%ls.sessions",base,key)<0||!directory(s->root))return false;
    if(swprintf(path,MAX_PATH,L"%ls\\lease",s->root)<0)return false;
    s->lock=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(s->lock==INVALID_HANDLE_VALUE)return false;
    if(!load_index(s)){report_session_close(s);
        return false;
    }return true;
}
void report_session_close(report_session_store *s) {
    if(s->lock && s->lock!=INVALID_HANDLE_VALUE)CloseHandle(s->lock);
    s->lock=INVALID_HANDLE_VALUE;
}
bool report_session_path(const report_session_store *s,const char *id,const wchar_t *name,wchar_t *out) {
    return log_session_id_valid(id)&&swprintf(out,MAX_PATH,L"%ls\\%hs\\%ls",s->root,id,name)>=0;
}
static bool save_meta(report_session_store *s,const log_session_meta *m) {
    wchar_t path[MAX_PATH];
    return report_session_path(s,m->id,L"session.meta",path)&&report_session_meta_save(path,m);
}
bool report_session_begin(report_session_store *s,const char *version,uint32_t build,log_session_meta *m,HANDLE *raw) {
    *raw=INVALID_HANDLE_VALUE;
    *m=(log_session_meta){.role=1,.build=build,.stage=SESSION_PREPARED};
    if(strlen(version)>=sizeof(m->version)||!uuid(m->id))return false;
    strcpy(m->version,version);
    if(!log_session_meta_valid(m))return false;
    wchar_t path[MAX_PATH];
    if(swprintf(path,MAX_PATH,L"%ls\\%hs",s->root,m->id)<0||!CreateDirectoryW(path,NULL)||!save_meta(s,m))return false;
    if(!report_session_path(s,m->id,L"run.log",path))return false;
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE};
    *raw=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(*raw==INVALID_HANDLE_VALUE){m->stage=SESSION_UNAVAILABLE;
        save_meta(s,m);
        return false;
    }
    char header[40];
    snprintf(header,sizeof(header),"ZSID1 %s\n",m->id);
    DWORD written=0;
    if(!WriteFile(*raw,header,39,&written,NULL)||written!=39){m->stage=SESSION_UNAVAILABLE;
        save_meta(s,m);
        CloseHandle(*raw);
        *raw=INVALID_HANDLE_VALUE;
        return false;
    }
    report_session_index next=s->index;
    strcpy(next.pending,m->id);
    if(!save_index(s,&next)){m->stage=SESSION_UNAVAILABLE;
        save_meta(s,m);
        CloseHandle(*raw);
        *raw=INVALID_HANDLE_VALUE;
        return false;
    }
    return true;
}
static bool launched(report_session_store *s,const char *id) {
    wchar_t path[MAX_PATH];
    log_session_meta m;
    return *id && report_session_path(s,id,L"session.meta",path)&&report_session_meta_load(path,&m)&&
    !strcmp(m.id,id)&&m.role==1&&m.creation_ok&&m.stage==SESSION_ENDED;
}
bool report_session_created(report_session_store *s,log_session_meta *m,bool ok,DWORD error) {
    if(strcmp(s->index.pending,m->id))return false;
    m->creation_known=1;
    m->creation_ok=ok;
    m->creation_error=ok?0:error;
    m->stage=ok?SESSION_CREATED:SESSION_FAILED;
    if(!save_meta(s,m))return false;
    if(!ok)return true;
    report_session_index next={0};
    strcpy(next.current,m->id);
    const char *retained=launched(s,s->index.current)?s->index.current:(launched(s,s->index.previous)?s->index.previous:"");
    strcpy(next.previous,retained);
    if(!save_index(s,&next)){m->stage=SESSION_UNAVAILABLE;
        save_meta(s,m);
        return false;
    }
    return true;
}
bool report_session_stage(report_session_store *s,log_session_meta *m,uint32_t stage) {
    m->stage=stage;
    return save_meta(s,m);
}
bool report_session_ended(report_session_store *s,log_session_meta *m,bool known,DWORD code) {
    m->stage=SESSION_ENDED;
    m->exit_known=known;
    m->exit_code=known?code:0;
    return save_meta(s,m);
}
static bool snapshot_one(report_session_snapshot *snap,const char *id,bool previous) {
    if(!*id)return true;
    log_session_meta *m=previous?&snap->previous:&snap->current,*core=previous?&snap->previous_core:&snap->core;
    wchar_t *raw=previous?snap->previous_raw:snap->raw,path[MAX_PATH];
    if(!report_session_path(&snap->store,id,L"session.meta",path)||!report_session_meta_load(path,m)||strcmp(m->id,id)||m->role!=1)return false;
    if(m->stage==SESSION_RUNNING || m->stage==SESSION_CREATED || m->stage==SESSION_PREPARED)return false;
    if(previous)snap->previous_known=true;
    else snap->current_known=true;
    if(!report_session_path(&snap->store,id,L"run.log",raw)||!report_session_path(&snap->store,id,L"core.meta",path))return false;
    FILE *f=_wfopen(path,L"rb");
    if(f) {
        bool ok=log_session_meta_read(f,core)&&core->role==2&&!strcmp(core->id,id);
        fclose(f);
        if(!ok)return false;
        if(previous)snap->previous_core_known=true;
        else snap->core_known=true;
    }else if(errno!=ENOENT)return false;
    bool bad=false;
    char actual[33];
    const wchar_t *suffix[]={L"",L".records"};
    for(unsigned n=0;n<2;n++) {
        if(swprintf(path,MAX_PATH,L"%ls%ls",raw,suffix[n])<0)return false;
        f=_wfopen(path,L"rb");
        if(!f){if(errno!=ENOENT)bad=true;
            continue;
        }
        if(log_session_header_read(f,actual)!=1||strcmp(actual,id))bad=true;
        fclose(f);
    }
    if(previous)snap->previous_identity_bad=bad;
    else snap->current_identity_bad=bad;
    return true;
}
bool report_session_snapshot_open(const wchar_t *base,const wchar_t *game,report_session_snapshot *snap) {
    memset(snap,0,sizeof(*snap));
    if(!report_session_open(base,game,&snap->store))return false;
    if(!snap->store.indexed) {
        snap->legacy=true;
        wchar_t key[48];if(!report_session_game_key(game,key))goto fail;
        const wchar_t *legacy=wcspbrk(game,L"\\/:*?\"<>|")||!wcscmp(game,L".")||!wcscmp(game,L"..")||device_name(game)?key:game;
        if(swprintf(snap->raw,MAX_PATH,L"%ls\\user\\logs\\%ls.log",base,legacy)<0||
           swprintf(snap->previous_raw,MAX_PATH,L"%ls\\user\\logs\\%ls.anterior.log",base,legacy)<0)goto fail;
        return true;
    }
    const report_session_index *i=&snap->store.index;
    const char *current=*i->pending?i->pending:i->current,*previous=*i->pending?i->current:i->previous;
    if(!snapshot_one(snap,current,false)||!snapshot_one(snap,previous,true))goto fail;
    return true;
    fail:
    report_session_snapshot_close(snap);
    return false;
}
void report_session_snapshot_close(report_session_snapshot *snap) { report_session_close(&snap->store);
}
