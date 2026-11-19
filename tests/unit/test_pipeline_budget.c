/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real queue, threads and files; a controlled compiler models source cost. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "gpu/vk/vk_pipeline_async.h"
#include "gpu/vk/vk_shader_cache.h"
#include "pal/pal.h"
#include "ps5c/shader_cache_version.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line%d: %s\n",__LINE__,#x); return 1; } } while (0)
typedef struct recipe { uint32_t id; uint8_t pad[(512u<<10)-4]; } recipe;
static char dir[400],path[440];
static atomic_uint allowed,started,builds;
static unsigned order[16];
static uint64_t source_bytes;
static bool missing;
static VKAPI_ATTR void VKAPI_CALL destroy_pipe(VkDevice d,VkPipeline p,const VkAllocationCallbacks*a) {(void)d;(void)p;(void)a;}
static VKAPI_ATTR void VKAPI_CALL destroy_layout(VkDevice d,VkPipelineLayout p,const VkAllocationCallbacks*a) {(void)d;(void)p;(void)a;}
static VKAPI_ATTR void VKAPI_CALL destroy_dsl(VkDevice d,VkDescriptorSetLayout p,const VkAllocationCallbacks*a) {(void)d;(void)p;(void)a;}
PFN_vkDestroyPipeline vkDestroyPipeline=destroy_pipe;
PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout=destroy_layout;
PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout=destroy_dsl;
const char *vk_shader_cache_dir(void) { return dir; }
/* no translator change in these tests */
bool vk_shader_cache_migrating(void) { return false; }
bool vk_pipeline_recipe_migrate(void *recipe) { (void)recipe; return false; }
size_t vk_pipeline_recipe_size(void) { return sizeof(recipe); }
bool vk_pipeline_prewarm_size(const void *data,uint64_t *bytes) {
    *bytes=source_bytes;return !missing || ((const recipe *)data)->id!=2;
}
bool vk_pipeline_build(renderer*r,const void*data,const uint32_t*const words[2],const size_t n[2],vk_built_pipeline*out) {
    (void)r;(void)n;unsigned id=((const recipe *)data)->id;
    if(id>4 || (missing && id==2 && (!words || !words[0])))return false;
    unsigned at=atomic_fetch_add(&builds,1);if(at>=PS5C_ARRAY_COUNT(order))return false;
    order[at]=id;atomic_fetch_or(&started,1u<<id);
    while(!(atomic_load(&allowed)&(1u<<id)))pal_sleep_ns(1000000);
    out->pipe=(VkPipeline)(uintptr_t)(id+1);return true;
}
static void option(const char *key,const char *value) {
#ifdef _WIN32
    _putenv_s(key,value);
#else
    setenv(key,value,1);
#endif
    ps5c_env_reset();
}
static void defaults(void) {
    option("PS5C_PIPELINES_ASYNC","1");option("PS5C_PIPELINE_THREADS","1");
    option("PS5C_PIPELINE_PREWARM_COUNT","64");option("PS5C_PIPELINE_PREWARM_MB","16");
    option("PS5C_PIPELINE_PREWARM_RECIPE_MB","8");option("PS5C_PIPELINE_LIST_MB","256");
    option("PS5C_PIPELINE_LIST_ENTRIES","65536");source_bytes=600u<<10;missing=false;
    atomic_store(&allowed,~0u);atomic_store(&started,0);atomic_store(&builds,0);
}
static int make_list(const char *parent,unsigned which) {
    snprintf(dir,sizeof(dir),"%s/pipeline-budget-%llu-%u",parent,(unsigned long long)pal_time_ns(),which);
    CHECK(pal_mkdir(dir)==PS5C_OK);snprintf(path,sizeof(path),"%s/pipelines-list.bin",dir);
    pal_file *f=NULL;CHECK(pal_file_open(path,PAL_FILE_WRITE|PAL_FILE_CREATE|PAL_FILE_TRUNCATE,&f)==PS5C_OK);
    const uint64_t header[]={0x3154534C505A5350ull,PS5C_SHADER_TRANSLATOR_HASH,sizeof(recipe)};
    uint64_t done=0;CHECK(pal_file_write(f,header,sizeof(header),&done)==PS5C_OK && done==sizeof(header));
    recipe *rc=calloc(1,sizeof(*rc));CHECK(rc);
    for(unsigned id=0;id<4;id++) { rc->id=id;CHECK(pal_file_write(f,rc,sizeof(*rc),&done)==PS5C_OK && done==sizeof(*rc)); }
    free(rc);pal_file_close(f);return 0;
}
static uint64_t list_size(void) {
    pal_file *f=NULL;uint64_t size=UINT64_MAX;
    if(pal_file_open(path,PAL_FILE_READ,&f)==PS5C_OK){pal_file_size(f,&size);pal_file_close(f);}return size;
}
static int drain(unsigned expected) {
    uint32_t done,total,draws;uint64_t end=pal_time_ns()+2000000000ull;
    while(vk_pipeline_async_progress(&done,&total,&draws) && pal_time_ns()<end)pal_sleep_ns(1000000);
    CHECK(!vk_pipeline_async_progress(&done,&total,&draws) && done==expected && total==expected && !draws);return 0;
}
static int request(renderer *r,unsigned id) {
    recipe *rc=calloc(1,sizeof(*rc));CHECK(rc);rc->id=id;
    uint32_t code=123;const uint32_t *words[]={&code,NULL};size_t n[]={1,0};
    vk_built_pipeline out={0};bool shared=false,pending=false;
    bool ok=vk_pipeline_async_get(r,rc,words,n,&out,&shared,&pending,true);free(rc);
    CHECK(ok && shared && !pending && out.pipe==(VkPipeline)(uintptr_t)(id+1));return 0;
}
typedef struct request_arg { renderer *r;unsigned id;atomic_bool entered;int result; } request_arg;
static int32_t request_thread(void *p) { request_arg *q=p;atomic_store(&q->entered,true);q->result=request(q->r,q->id);return 0; }
int main(int argc,char **argv) {
    CHECK(argc==2 && pal_init()==PS5C_OK);renderer *r=calloc(1,sizeof(*r));CHECK(r);
    for(unsigned test=0;test<8;test++) {
        defaults();CHECK(make_list(argv[1],test)==0);unsigned expected=4;
        switch(test) {
        case 0:option("PS5C_PIPELINE_PREWARM_COUNT","1");expected=1;break;
        case 1:option("PS5C_PIPELINE_PREWARM_MB","1");expected=1;break;
        case 2:option("PS5C_PIPELINE_PREWARM_RECIPE_MB","1");expected=2;break;
        case 3:option("PS5C_PIPELINE_PREWARM_COUNT","0");expected=0;break;
        case 4:option("PS5C_PIPELINE_LIST_MB","1");expected=1;break;
        case 5:option("PS5C_PIPELINE_LIST_ENTRIES","1");expected=1;break;
        case 6:missing=true;expected=3;break;
        case 7:option("PS5C_PIPELINE_PREWARM_COUNT","-1");break; /* invalid: use default */
        }
        uint64_t size=list_size();vk_pipeline_async_start(r);CHECK(vk_pipeline_async_on());CHECK(drain(expected)==0);
        CHECK(atomic_load(&builds)==expected);CHECK(request(r,3)==0 && request(r,2)==0);
        CHECK(drain(expected)==0 && vk_pipeline_async_stop(r));CHECK(list_size()==size);
    }
    /* A live request jumps ahead of untouched prewarm work; promoting an
     * existing speculative job must still complete the progress accounting. */
    for(unsigned id=3;id<=4;id++) {
        defaults();CHECK(make_list(argv[1],id+10)==0);atomic_store(&allowed,~1u);
        vk_pipeline_async_start(r);uint64_t end=pal_time_ns()+2000000000ull;
        while(!(atomic_load(&started)&1) && pal_time_ns()<end)pal_sleep_ns(1000000);
        CHECK(atomic_load(&started)&1);request_arg q={.r=r,.id=id};pal_thread *thread=NULL;
        CHECK(pal_thread_create(&thread,request_thread,&q,"priority build",0)==PS5C_OK);
        while(!atomic_load(&q.entered) && pal_time_ns()<end)pal_sleep_ns(1000000);
        CHECK(atomic_load(&q.entered));uint32_t done,total,draws=0;
        while(!draws && pal_time_ns()<end) { vk_pipeline_async_progress(&done,&total,&draws);pal_sleep_ns(1000000); }
        CHECK(draws);atomic_fetch_or(&allowed,1);
        CHECK(pal_thread_join(thread,NULL)==PS5C_OK && q.result==0);CHECK(drain(4)==0);
        CHECK(vk_pipeline_async_stop(r) && order[0]==0 && order[1]==id);
        CHECK(list_size()==24+(id==3?4ull:5ull)*sizeof(recipe));
    }
    free(r);pal_shutdown();puts("pipeline budgets: count/source/recipes/disk/index, no truncation, live priority and promotion verified");return 0;
}
