/* SPDX-License-Identifier: GPL-2.0-or-later */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/vk/vk_shader_cache.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line%d: %s\n",__LINE__,#x); return 100; } } while (0)
typedef struct driver_blob {
    uint32_t size,version,vendor,device;
    uint8_t uuid[16];
    uint64_t value;
} driver_blob;
static driver_blob blob={32,VK_PIPELINE_CACHE_HEADER_VERSION_ONE,0x1234,0x5678,{0},1};
static VkResult query_error,copy_error;
static atomic_uint queries,active,max_active;
static atomic_bool block_query,query_entered,second_entered;
static bool note_during_copy;
static uint64_t loaded_value;
static size_t loaded_size, reported_size;
static bool reject_initial;
static unsigned cache_creates;

static VKAPI_ATTR VkResult VKAPI_CALL create_cache(VkDevice d,const VkPipelineCacheCreateInfo*i,const VkAllocationCallbacks*a,VkPipelineCache*p) {
    (void)d;(void)a;cache_creates++;*p=VK_NULL_HANDLE;
    loaded_value=0;loaded_size=i->initialDataSize;
    if(reject_initial && i->initialDataSize)return VK_ERROR_OUT_OF_HOST_MEMORY;
    *p=(VkPipelineCache)(uintptr_t)1;
    if(i->initialDataSize==sizeof(blob))memcpy(&loaded_value,(const uint8_t *)i->pInitialData+32,8);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_cache(VkDevice d,VkPipelineCache p,const VkAllocationCallbacks*a) {
    (void)d;(void)p;(void)a;
}
static VKAPI_ATTR VkResult VKAPI_CALL get_cache(VkDevice d,VkPipelineCache p,size_t*n,void*data) {
    (void)d;(void)p;atomic_fetch_add(&queries,1);
    unsigned now=atomic_fetch_add(&active,1)+1,maximum=atomic_load(&max_active);
    while(now>maximum && !atomic_compare_exchange_weak(&max_active,&maximum,now)) {}
    if(!data && atomic_load(&block_query)) {
        atomic_store(&query_entered,true);
        while(atomic_load(&block_query))pal_sleep_ns(1000000);
    }
    VkResult result=data?copy_error:query_error;
    if(data) {
        size_t bytes=*n<sizeof(blob)?*n:sizeof(blob);memcpy(data,&blob,bytes);
        if(note_during_copy){note_during_copy=false;vk_shader_cache_note_pipeline(1);}
    }
    *n=reported_size?reported_size:sizeof(blob);atomic_fetch_sub(&active,1);return result;
}

static uint64_t stored(const char *path) {
    uint8_t *data=NULL;uint64_t size=0,value=UINT64_MAX;
    if(pal_file_read_all(path,&data,&size)==PS5C_OK && size==16+sizeof(blob))memcpy(&value,data+size-8,8);
    free(data);return value;
}
static void dirty(uint64_t value) {blob.value=value;vk_shader_cache_note_pipeline(1000000);}
static int32_t save_thread(void *arg) {
    if(arg)atomic_store(&second_entered,true);
    renderer_save_caches(NULL);return 0;
}
static bool entered(atomic_bool *flag) {
    uint64_t end=pal_time_ns()+2000000000ull;
    while(!atomic_load(flag) && pal_time_ns()<end)pal_sleep_ns(1000000);
    return atomic_load(flag);
}

static void option(const char *key,const char *value) {
    _putenv_s(key,value);ps5c_env_reset();
}

static uint64_t file_size(const char *path) {
    pal_file *f=NULL;uint64_t size=UINT64_MAX;
    if(pal_file_open(path,PAL_FILE_READ,&f)==PS5C_OK){pal_file_size(f,&size);pal_file_close(f);}
    return size;
}

static bool replace_file(const char *path,const void *data,uint64_t size) {
    pal_file *f=NULL;uint64_t done=0;
    if(pal_file_open(path,PAL_FILE_WRITE|PAL_FILE_CREATE|PAL_FILE_TRUNCATE,&f)!=PS5C_OK)return false;
    bool ok=pal_file_write(f,data,size,&done)==PS5C_OK && done==size;
    pal_file_close(f);return ok;
}

static int compatibility_test(renderer *r,const char *dir,const char *path) {
    uint8_t *original=NULL,*translated=NULL;uint64_t size=0,translated_size=0;
    CHECK(pal_file_read_all(path,&original,&size)==PS5C_OK && size==16+sizeof(blob));
    char spath[320];snprintf(spath,sizeof(spath),"%s/cache/spirv.bin",dir);
    CHECK(pal_file_read_all(spath,&translated,&translated_size)==PS5C_OK && translated_size>16);
    uint8_t changed[16+sizeof(blob)];memcpy(changed,original,sizeof(changed));
    changed[8]^=0x80;translated[8]^=0x80; /* translator version only, opaque body intact */
    CHECK(replace_file(path,changed,sizeof(changed)) && replace_file(spath,translated,translated_size));
    vk_shader_cache_open(r);CHECK(loaded_size==sizeof(blob) && loaded_value==10);
    uint32_t *copy=NULL;size_t words=0;
    CHECK(!vk_shader_cache_find(0x123456,&copy,&words) && !copy); /* SPIR-V is still invalidated */
    vk_shader_cache_close(r);
    translated[8]^=0x80;CHECK(replace_file(spath,translated,translated_size));
    /* Incorrect magic, header length/version, GPU identifiers, UUID and a
     * truncated header are never handed to the driver as initial data. */
    const unsigned offsets[]={0,16,20,24,28,32};
    for(unsigned i=0;i<PS5C_ARRAY_COUNT(offsets);i++) {
        memcpy(changed,original,sizeof(changed));changed[offsets[i]]^=4;
        CHECK(replace_file(path,changed,sizeof(changed)));
        vk_shader_cache_open(r);CHECK(loaded_size==0);vk_shader_cache_close(r);
    }
    CHECK(replace_file(path,original,16+31));
    vk_shader_cache_open(r);CHECK(loaded_size==0);vk_shader_cache_close(r);
    CHECK(replace_file(path,original,size));
    reject_initial=true;unsigned before=cache_creates;
    vk_shader_cache_open(r);CHECK(cache_creates==before+2 && loaded_size==0);
    CHECK(vk_shader_cache_pipelines());vk_shader_cache_close(r);reject_initial=false;
    CHECK(stored(path)==10); /* rejection/fallback did not rewrite a valid file */
    vk_shader_cache_open(r);CHECK(loaded_size==sizeof(blob) && loaded_value==10);vk_shader_cache_close(r);
    free(translated);free(original);
    puts("shader cache: opaque driver data survives translator updates; SPIR-V, device/header checks and fallback preserved");
    return 0;
}

static int budget_test(renderer *r,const char *parent) {
    char dir[280],path[320];snprintf(dir,sizeof(dir),"%s/budgets",parent);
    snprintf(path,sizeof(path),"%s/cache/spirv.bin",dir);CHECK(pal_mkdir(dir)==PS5C_OK);
    option("PS5C_SHADER_CACHE_RAM_MB","1");option("PS5C_SHADER_CACHE_DISK_MB","8");
    renderer_set_cache_dir(dir);vk_shader_cache_open(r);
    const size_t words=150001; /* non-eight-byte-aligned record, about 586 KiB */
    uint32_t *body=malloc(words*4);CHECK(body);
    for(unsigned id=0;id<4;id++) {
        for(size_t j=0;j<words;j++)body[j]=0xabc00000u+id+(uint32_t)j;
        vk_shader_cache_store(100+id,body,words);
        CHECK(vk_shader_cache_memory().resident_bytes<=1u<<20);
    }
    free(body);CHECK(vk_shader_cache_memory().indexed_entries==4);
    uint64_t original=file_size(path);CHECK(original==16+4*(24+words*4));
    for(unsigned pass=0;pass<2;pass++) {
        for(unsigned id=0;id<4;id++) {
            uint32_t *copy=NULL;size_t n=0;CHECK(vk_shader_cache_find(100+id,&copy,&n) && n==words);
            for(size_t j=0;j<n;j++)CHECK(copy[j]==0xabc00000u+id+(uint32_t)j);
            free(copy);CHECK(vk_shader_cache_memory().resident_bytes<=1u<<20);
        }
        vk_shader_cache_close(r);vk_shader_cache_open(r);
        CHECK(vk_shader_cache_memory().resident_bytes==0 && vk_shader_cache_memory().indexed_entries==4);
    }
    vk_shader_cache_close(r);
    /* A small index or scan budget preserves the complete file, including its tail. */
    option("PS5C_SHADER_CACHE_ENTRIES","2");vk_shader_cache_open(r);
    CHECK(vk_shader_cache_memory().indexed_entries==2 && vk_shader_cache_size(100)==words*4);
    uint32_t code=123;vk_shader_cache_store(200,&code,1);
    CHECK(vk_shader_cache_memory().indexed_entries==2 && file_size(path)==original);
    vk_shader_cache_close(r);option("PS5C_SHADER_CACHE_ENTRIES","");
    option("PS5C_SHADER_CACHE_DISK_MB","1");vk_shader_cache_open(r);
    CHECK(vk_shader_cache_memory().indexed_entries==1 && file_size(path)==original);
    vk_shader_cache_store(200,&code,1);CHECK(file_size(path)==original);vk_shader_cache_close(r);
    option("PS5C_SHADER_CACHE_DISK_MB","8");option("PS5C_SHADER_CACHE_RAM_MB","0");
    vk_shader_cache_open(r);
    uint32_t *copy=NULL;size_t n=0;
    CHECK(vk_shader_cache_find(103,&copy,&n) && n==words);free(copy);
    CHECK(vk_shader_cache_memory().resident_bytes==0);vk_shader_cache_close(r);
    /* A damaged tail is repaired in place, without losing valid entries. */
    pal_file *f=NULL;uint64_t done=0;
    CHECK(pal_file_open(path,PAL_FILE_WRITE,&f)==PS5C_OK);
    CHECK(pal_file_pwrite(f,&code,3,original,&done)==PS5C_OK && done==3);pal_file_close(f);
    vk_shader_cache_open(r);CHECK(file_size(path)==original && vk_shader_cache_memory().indexed_entries==4);
    vk_shader_cache_close(r);
    /* Read-only caches remain useful. Never truncate after failure to get write access. */
    HANDLE held=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    CHECK(held!=INVALID_HANDLE_VALUE);vk_shader_cache_open(r);
    CHECK(vk_shader_cache_find(100,&copy,&n) && n==words);free(copy);
    vk_shader_cache_close(r);CloseHandle(held);CHECK(file_size(path)==original);
    /* Re-check a disk body on a lazy lookup; reject bytes changed since indexing. */
    vk_shader_cache_open(r);CHECK(pal_file_open(path,PAL_FILE_WRITE,&f)==PS5C_OK);
    CHECK(pal_file_pwrite(f,&code,4,16+24,&done)==PS5C_OK && done==4);pal_file_close(f);
    copy=NULL;CHECK(!vk_shader_cache_find(100,&copy,&n) && !copy);vk_shader_cache_close(r);
    vk_shader_cache_open(r);CHECK(vk_shader_cache_memory().indexed_entries==0 && file_size(path)==16);
    vk_shader_cache_close(r);
    option("PS5C_SHADER_CACHE_RAM_MB","");option("PS5C_SHADER_CACHE_DISK_MB","");
    renderer_set_cache_dir(parent);vk_shader_cache_open(r);
    CHECK(loaded_size==sizeof(blob));vk_shader_cache_close(r);
    /* An oversized snapshot remains pending, but does not allocate its reported size. */
    option("PS5C_DRIVER_CACHE_MB","1");vk_shader_cache_open(r);
    reported_size=2u<<20;dirty(11);renderer_save_caches(r);
    snprintf(path,sizeof(path),"%s/cache/pipelines-1234-5678.bin",parent);CHECK(stored(path)==10);
    reported_size=0;renderer_save_caches(r);CHECK(stored(path)==11);vk_shader_cache_close(r);
    CHECK(pal_file_open(path,PAL_FILE_WRITE,&f)==PS5C_OK && pal_file_truncate(f,2u<<20)==PS5C_OK);pal_file_close(f);
    vk_shader_cache_open(r);CHECK(loaded_size==0 && file_size(path)==2u<<20);vk_shader_cache_close(r);
    option("PS5C_DRIVER_CACHE_MB","0");vk_shader_cache_open(r);
    CHECK(!vk_shader_cache_pipelines() && vk_shader_cache_dir());vk_shader_cache_close(r);
    option("PS5C_DRIVER_CACHE_MB","");
    puts("shader cache: bounded residency, disk fallback, read-only files, integrity and admission verified");
    return 0;
}

int main(int argc,char **argv) {
    CHECK(pal_init()==PS5C_OK);
    vkCreatePipelineCache=create_cache;vkDestroyPipelineCache=destroy_cache;vkGetPipelineCacheData=get_cache;
    renderer *r=calloc(1,sizeof(*r));CHECK(r);
    r->device=(VkDevice)(uintptr_t)1;r->props.vendorID=blob.vendor;r->props.deviceID=blob.device;
    char dir[240],path[320],tmp[340];
    snprintf(dir,sizeof(dir),"cache-regression-%llu",(unsigned long long)pal_time_ns());
    snprintf(path,sizeof(path),"%s/cache/pipelines-1234-5678.bin",dir);
    snprintf(tmp,sizeof(tmp),"%s.tmp",path);
    CHECK(pal_mkdir(dir)==PS5C_OK);renderer_set_cache_dir(dir);vk_shader_cache_open(r);
    CHECK(vk_shader_cache_dir() && vk_shader_cache_pipelines());
    if(argc==2 && !strcmp(argv[1],"--lost")) {
        query_error=VK_ERROR_DEVICE_LOST;dirty(1);renderer_save_caches(r);return 93;
    }
    dirty(1);renderer_save_caches(r);CHECK(stored(path)==1);
    query_error=VK_ERROR_OUT_OF_HOST_MEMORY;dirty(2);renderer_save_caches(r);CHECK(stored(path)==1);
    query_error=VK_SUCCESS;renderer_save_caches(r);CHECK(stored(path)==2);
    copy_error=VK_INCOMPLETE;dirty(3);renderer_save_caches(r);CHECK(stored(path)==2);
    copy_error=VK_SUCCESS;renderer_save_caches(r);CHECK(stored(path)==3);
    /* A real open failure must keep the old file and remain retryable. */
    CHECK(CreateDirectoryA(tmp,NULL));dirty(4);renderer_save_caches(r);CHECK(stored(path)==3);
    CHECK(RemoveDirectoryA(tmp));renderer_save_caches(r);CHECK(stored(path)==4);
    /* Deny rename/delete on the existing cache without modifying its bytes. */
    HANDLE held=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    CHECK(held!=INVALID_HANDLE_VALUE);dirty(5);renderer_save_caches(r);CHECK(stored(path)==4);
    CloseHandle(held);renderer_save_caches(r);CHECK(stored(path)==5);
    /* Busy compilers also retain the pending save. */
    vk_shader_cache_use_begin();dirty(6);renderer_save_caches(r);CHECK(stored(path)==5);
    vk_shader_cache_use_end();renderer_save_caches(r);CHECK(stored(path)==6);
    /* Automatic retries back off, then occur without a new pipeline. */
    query_error=VK_ERROR_OUT_OF_HOST_MEMORY;dirty(7);pal_sleep_ns(2100000000ull);renderer_save_caches(r);
    query_error=VK_SUCCESS;unsigned before=atomic_load(&queries);
    vk_shader_cache_tick();CHECK(atomic_load(&queries)==before && stored(path)==6);
    pal_sleep_ns(2100000000ull);vk_shader_cache_tick();CHECK(stored(path)==7);
    /* A new pipeline during the snapshot must remain pending afterwards. */
    dirty(8);note_during_copy=true;renderer_save_caches(r);CHECK(stored(path)==8);
    blob.value=9;renderer_save_caches(r);CHECK(stored(path)==9);
    before=atomic_load(&queries);renderer_save_caches(r);CHECK(atomic_load(&queries)==before);
    /* Two savers must not overlap vkGetPipelineCacheData or the .tmp writer. */
    dirty(10);atomic_store(&block_query,true);atomic_store(&max_active,0);
    pal_thread *first,*second;
    CHECK(pal_thread_create(&first,save_thread,NULL,"cache first save",0)==PS5C_OK);
    CHECK(entered(&query_entered));vk_shader_cache_note_pipeline(1);
    CHECK(pal_thread_create(&second,save_thread,r,"cache second save",0)==PS5C_OK);
    CHECK(entered(&second_entered));pal_sleep_ns(200000000);
    atomic_store(&block_query,false);
    CHECK(pal_thread_join(first,NULL)==PS5C_OK && pal_thread_join(second,NULL)==PS5C_OK);
    CHECK(atomic_load(&max_active)==1 && stored(path)==10);
    /* SPIR-V entries survive reopen; disabling one session must not poison the next. */
    const uint32_t code[]={0x07230203,0x00010300,0,1,0};
    vk_shader_cache_store(0x123456,code,PS5C_ARRAY_COUNT(code));vk_shader_cache_close(r);
    option("PS5C_NO_SHADER_CACHE","1");vk_shader_cache_open(r);CHECK(!vk_shader_cache_dir());vk_shader_cache_close(r);
    option("PS5C_NO_SHADER_CACHE","");vk_shader_cache_open(r);CHECK(vk_shader_cache_dir() && loaded_value==10);
    uint32_t *copy=NULL;size_t words=0;CHECK(vk_shader_cache_find(0x123456,&copy,&words));
    CHECK(words==PS5C_ARRAY_COUNT(code) && !memcmp(copy,code,sizeof(code)));free(copy);
    vk_shader_cache_close(r);CHECK(compatibility_test(r,dir,path)==0);
    CHECK(budget_test(r,dir)==0);free(r);pal_shutdown();
    /* _spawnv joins the arguments unquoted: a path with spaces needs its own quotes */
    char quoted[1024];snprintf(quoted,sizeof(quoted),"\"%s\"",argv[0]);
    const char *child[]={quoted,"--lost",NULL};CHECK(_spawnv(_P_WAIT,argv[0],child)==1);
    puts("shader cache: retry/backoff, atomic replacement, concurrent saves and session isolation verified");return 0;
}
