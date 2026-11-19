/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Inject device loss at the actual positional-read boundary. All recovery
 * opens, identity checks, reads and sharing rules remain real Windows I/O. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
static BOOL injected_read(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
#define PS5C_WIN32_PREAD_READ injected_read
#include "../../src/pal_win32/pal_win32.c"

static HANDLE broken_handle;
static volatile LONG calls,fail_all,short_read;
static DWORD injected_error=ERROR_DEV_NOT_EXIST;
static BOOL injected_read(HANDLE h,LPVOID b,DWORD n,LPDWORD got,LPOVERLAPPED ov) {
    InterlockedIncrement(&calls);
    if(h==broken_handle || InterlockedCompareExchange(&fail_all,0,0)) {
        *got=0;SetLastError(injected_error);return FALSE;
    }
    if(InterlockedCompareExchange(&short_read,0,0) && n>8)n=8;
    return ReadFile(h,b,n,got,ov);
}
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"line %d: %s (Windows %lu)\n",__LINE__,#x,(unsigned long)GetLastError());return 2;}}while(0)

typedef struct reader {pal_file *file;uint64_t offset;unsigned char data[32];uint64_t got;ps5c_err error;} reader;
static DWORD WINAPI read_worker(void *p) {
    reader *r=p;r->error=pal_file_pread(r->file,r->data,sizeof(r->data),r->offset,&r->got);return 0;
}
static bool write_fixture(const char *path,const unsigned char *bytes,size_t n) {
    pal_file *file=NULL;uint64_t written=0;
    if(pal_file_open(path,PAL_FILE_WRITE|PAL_FILE_CREATE|PAL_FILE_TRUNCATE,&file)!=PS5C_OK)return false;
    ps5c_err err=pal_file_write(file,bytes,n,&written);pal_file_close(file);return err==PS5C_OK && written==n;
}
static bool restore_write_time(const char *path,const FILETIME *time) {
    HANDLE h=CreateFileA(path,FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    if(h==INVALID_HANDLE_VALUE)return false;
    bool ok=SetFileTime(h,NULL,NULL,time)!=0;CloseHandle(h);return ok;
}
static void arm(pal_file *f,DWORD error,bool all) {
    broken_handle=f->handle;injected_error=error;InterlockedExchange(&calls,0);InterlockedExchange(&fail_all,all);
}
int main(void) {
    char temporary[MAX_PATH],directory[MAX_PATH],path[MAX_PATH],moved[MAX_PATH];
    unsigned char bytes[256],out[32];for(unsigned i=0;i<sizeof(bytes);i++)bytes[i]=(unsigned char)(i^0xa5);
    REQUIRE(GetTempPathA(sizeof(temporary),temporary));
    REQUIRE(GetTempFileNameA(temporary,"prr",0,directory));REQUIRE(DeleteFileA(directory));REQUIRE(CreateDirectoryA(directory,NULL));
    REQUIRE(snprintf(path,sizeof(path),"%s\\input.bin",directory)>0);
    REQUIRE(snprintf(moved,sizeof(moved),"%s\\moved.bin",directory)>0);
    REQUIRE(write_fixture(path,bytes,sizeof(bytes)));
    pal_file *f=NULL;uint64_t got=0;
    REQUIRE(pal_file_open(path,PAL_FILE_READ,&f)==PS5C_OK);REQUIRE(!f->read_path);pal_file_close(f);
    const uint32_t recover=PAL_FILE_READ|PAL_FILE_RECOVER_READ;
    REQUIRE(pal_file_open(path,recover,&f)==PS5C_OK);REQUIRE(f->read_path);
    arm(f,ERROR_DEV_NOT_EXIST,false);
    REQUIRE(pal_file_pread(f,out,sizeof(out),73,&got)==PS5C_OK && got==sizeof(out));
    REQUIRE(!memcmp(out,bytes+73,sizeof(out)) && calls==2);
    DWORD flags;REQUIRE(GetHandleInformation(f->handle,&flags));
    puts("transient device loss: exact offset and original handle preserved");

    arm(f,ERROR_DEV_NOT_EXIST,true);got=99;
    REQUIRE(pal_file_pread(f,out,sizeof(out),0,&got)!=PS5C_OK && calls==2);
    arm(f,ERROR_ACCESS_DENIED,false);
    REQUIRE(pal_file_pread(f,out,sizeof(out),0,&got)!=PS5C_OK && calls==1);
    puts("persistent and unrelated errors: bounded retry, fail closed");
    arm(f,ERROR_DEV_NOT_EXIST,false);InterlockedExchange(&short_read,1);
    REQUIRE(pal_file_pread(f,out,sizeof(out),0,&got)!=PS5C_OK && calls==3);
    InterlockedExchange(&short_read,0);
    puts("short recovered read: at most one recovery per public call");

    arm(f,ERROR_DEV_NOT_EXIST,false);
    reader readers[2]={{.file=f,.offset=19},{.file=f,.offset=173}};
    HANDLE threads[2]={CreateThread(NULL,0,read_worker,&readers[0],0,NULL),CreateThread(NULL,0,read_worker,&readers[1],0,NULL)};
    REQUIRE(threads[0] && threads[1]);REQUIRE(WaitForMultipleObjects(2,threads,TRUE,5000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<2;i++){CloseHandle(threads[i]);REQUIRE(readers[i].error==PS5C_OK && readers[i].got==32);REQUIRE(!memcmp(readers[i].data,bytes+readers[i].offset,32));}
    REQUIRE(calls==4);pal_file_close(f);
    puts("concurrent recovery: separate handles and offsets");

    REQUIRE(pal_file_open(path,recover|PAL_FILE_WRITE,&f)==PS5C_OK);REQUIRE(!f->read_path);
    arm(f,ERROR_DEV_NOT_EXIST,false);REQUIRE(pal_file_pread(f,out,sizeof(out),0,&got)!=PS5C_OK && calls==1);pal_file_close(f);
    REQUIRE(pal_file_open(path,recover,&f)==PS5C_OK);
    HANDLE writer=CreateFileA(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    REQUIRE(writer!=INVALID_HANDLE_VALUE);arm(f,ERROR_DEV_NOT_EXIST,false);
    REQUIRE(pal_file_pread(f,out,sizeof(out),0,&got)!=PS5C_OK && calls==1);CloseHandle(writer);pal_file_close(f);
    puts("writable opens and active writers: no recovery");

    REQUIRE(pal_file_open(path,recover,&f)==PS5C_OK);
    REQUIRE(MoveFileA(path,moved));REQUIRE(write_fixture(path,bytes,sizeof(bytes)));
    REQUIRE(restore_write_time(path,&f->read_identity.info.ftLastWriteTime));
    arm(f,ERROR_DEV_NOT_EXIST,false);REQUIRE(pal_file_pread(f,out,sizeof(out),0,&got)!=PS5C_OK && calls==1);pal_file_close(f);
    REQUIRE(DeleteFileA(moved));
    puts("replacement with identical bytes: rejected by file identity");

    REQUIRE(pal_file_open(path,recover,&f)==PS5C_OK);
    REQUIRE(write_fixture(path,bytes,sizeof(bytes)-1));
    REQUIRE(restore_write_time(path,&f->read_identity.info.ftLastWriteTime));arm(f,ERROR_DEV_NOT_EXIST,false);
    REQUIRE(pal_file_pread(f,out,sizeof(out),0,&got)!=PS5C_OK && calls==1);pal_file_close(f);
    REQUIRE(pal_file_open(path,recover,&f)==PS5C_OK);
    writer=CreateFileA(path,FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    REQUIRE(writer!=INVALID_HANDLE_VALUE);FILETIME changed=f->read_identity.info.ftLastWriteTime;
    ULARGE_INTEGER time;time.LowPart=changed.dwLowDateTime;time.HighPart=changed.dwHighDateTime;time.QuadPart+=100000000;
    changed.dwLowDateTime=time.LowPart;changed.dwHighDateTime=time.HighPart;
    REQUIRE(SetFileTime(writer,NULL,NULL,&changed));CloseHandle(writer);arm(f,ERROR_DEV_NOT_EXIST,false);
    REQUIRE(pal_file_pread(f,out,sizeof(out),0,&got)!=PS5C_OK && calls==1);pal_file_close(f);
    puts("changed size or write time: rejected");
    REQUIRE(DeleteFileA(path));REQUIRE(RemoveDirectoryA(directory));
    puts("Windows positional-read recovery passed");return 0;
}
