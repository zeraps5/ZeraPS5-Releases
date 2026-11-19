/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real async queue/worker and a controlled compiler: compute must not vanish
 * at a time budget, even if a prior optional request marked the job late. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "base/base.h"
#include "gpu/vk/vk_pipeline_async.h"
#include "gpu/vk/vk_shader_cache.h"
#include "pal/pal.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);return 1;} } while(0)
static char dir[360];
static atomic_uint allowed, started, completed, builds, destroys;
static VKAPI_ATTR void VKAPI_CALL destroy_pipe(VkDevice d,VkPipeline p,const VkAllocationCallbacks*a) {
    (void)d;(void)p;(void)a;atomic_fetch_add(&destroys,1);
}
static VKAPI_ATTR void VKAPI_CALL destroy_layout(VkDevice d,VkPipelineLayout p,const VkAllocationCallbacks*a) {
    (void)d;(void)p;(void)a;atomic_fetch_add(&destroys,1);
}
static VKAPI_ATTR void VKAPI_CALL destroy_dsl(VkDevice d,VkDescriptorSetLayout p,const VkAllocationCallbacks*a) {
    (void)d;(void)p;(void)a;atomic_fetch_add(&destroys,1);
}
PFN_vkDestroyPipeline vkDestroyPipeline=destroy_pipe;
PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout=destroy_layout;
PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout=destroy_dsl;
const char *vk_shader_cache_dir(void) {return dir;}
/* no translator change in these tests */
bool vk_shader_cache_migrating(void) { return false; }
bool vk_pipeline_recipe_migrate(void *recipe) { (void)recipe; return false; }
size_t vk_pipeline_recipe_size(void) {return sizeof(uint32_t);}
bool vk_pipeline_prewarm_size(const void *recipe,uint64_t *bytes) {(void)recipe;*bytes=4;return true;}
bool vk_pipeline_build(renderer*r,const void*recipe,const uint32_t*const words[2],const size_t n[2],vk_built_pipeline*out) {
    (void)r;(void)words;(void)n;uint32_t id=*(const uint32_t*)recipe;
    atomic_fetch_add(&builds,1);
    atomic_fetch_or(&started,1u<<id);
    while(!(atomic_load(&allowed)&(1u<<id)))pal_sleep_ns(1000000);
    if(id==4 && (!words || !words[0]))return false; /* cache miss in speculative build */
    if(id==3)return false; /* terminal compiler failure is not a pending build */
    out->pipe=(VkPipeline)(uintptr_t)(id*3+1);
    out->layout=(VkPipelineLayout)(uintptr_t)(id*3+2);
    out->dsl=(VkDescriptorSetLayout)(uintptr_t)(id*3+3);
    atomic_fetch_or(&completed,1u<<id);return true;
}
typedef struct request {
    renderer *r;uint32_t id;atomic_bool entered,returned;
    bool ok,shared,pending;vk_built_pipeline out;
} request;
static int32_t required_request(void*arg) {
    request*q=arg;atomic_store(&q->entered,true);
    const uint32_t code=0x1234,*words[2]={&code,NULL};size_t n[2]={1,0};
    q->ok=vk_pipeline_async_get(q->r,&q->id,words,n,&q->out,&q->shared,&q->pending,true);
    atomic_store(&q->returned,true);return 0;
}
static int gated_request(renderer*r,uint32_t id,bool expect_ok) {
    request q={.r=r,.id=id};pal_thread*t=NULL;
    CHECK(pal_thread_create(&t,required_request,&q,"required pipeline",0)==PS5C_OK);
    uint64_t until=pal_time_ns()+2000000000ull;
    while(!atomic_load(&q.entered) && pal_time_ns()<until)pal_sleep_ns(1000000);
    pal_sleep_ns(20000000); /* the compiler cannot complete before our release */
    bool premature=atomic_load(&q.returned),entered=atomic_load(&q.entered);
    atomic_fetch_or(&allowed,1u<<id);
    CHECK(pal_thread_join(t,NULL)==PS5C_OK);
    CHECK(entered && !premature && atomic_load(&q.returned));
    CHECK(q.ok==expect_ok && !q.pending);
    if(expect_ok)CHECK(q.shared && q.out.pipe && (atomic_load(&completed)&(1u<<id)));
    return 0;
}
int main(int argc,char**argv) {
    CHECK(argc==2 && pal_init()==PS5C_OK);
    snprintf(dir,sizeof(dir),"%s/async-wait-%llu",argv[1],(unsigned long long)pal_time_ns());
    CHECK(pal_mkdir(dir)==PS5C_OK);
#ifdef _WIN32
    _putenv_s("PS5C_PIPELINES_ASYNC","1");_putenv_s("PS5C_PIPELINE_THREADS","1");_putenv_s("PS5C_PIPELINE_WAIT_MS","1"); ps5c_env_reset();
#else
    setenv("PS5C_PIPELINES_ASYNC","1",1);setenv("PS5C_PIPELINE_THREADS","1",1);setenv("PS5C_PIPELINE_WAIT_MS","1",1); ps5c_env_reset();
#endif
    renderer r={0};vk_pipeline_async_start(&r);CHECK(vk_pipeline_async_on());
    uint32_t first=1;vk_built_pipeline b={0};bool shared=false,pending=false;
    CHECK(!vk_pipeline_async_get(&r,&first,NULL,NULL,&b,&shared,&pending,false));
    CHECK(pending && !shared); /* optional graphics path still honors its budget */
    CHECK(gated_request(&r,1,true)==0); /* same job already marked over budget */
    CHECK(gated_request(&r,2,true)==0); /* first request is required */
    CHECK(gated_request(&r,3,false)==0); /* compiler failure is returned */
    CHECK(atomic_load(&builds)==3);
    CHECK(gated_request(&r,4,true)==0); /* record a recipe with live SPIR-V */
    CHECK(vk_pipeline_async_stop(&r));CHECK(atomic_load(&destroys)==9);
    atomic_store(&started,0);atomic_store(&allowed,(1u<<1)|(1u<<2));
    vk_pipeline_async_start(&r);CHECK(vk_pipeline_async_on());
    uint64_t until=pal_time_ns()+2000000000ull;
    while(!(atomic_load(&started)&(1u<<4)) && pal_time_ns()<until)pal_sleep_ns(1000000);
    CHECK(atomic_load(&started)&(1u<<4));
    CHECK(gated_request(&r,4,true)==0); /* running prewarm fails; live request must retry */
    CHECK(atomic_load(&builds)==8);
    CHECK(vk_pipeline_async_stop(&r));CHECK(atomic_load(&destroys)==18);
    /* A compiler still inside the driver must keep ownership after the stop
     * budget. Starting another session must not erase the live worker state. */
    atomic_store(&started,0);atomic_store(&completed,0);atomic_store(&allowed,0);
    vk_pipeline_async_start(&r);
    until=pal_time_ns()+2000000000ull;
    while(!atomic_load(&started) && pal_time_ns()<until)pal_sleep_ns(1000000);
    CHECK(atomic_load(&started));
    CHECK(!vk_pipeline_async_stop(&r));
    CHECK(vk_pipeline_async_on() && !atomic_load(&completed));
    CHECK(atomic_load(&destroys)==18);
    vk_pipeline_async_start(&r); /* rejected; the old job must still complete */
    atomic_store(&allowed,~0u);
    CHECK(vk_pipeline_async_stop(&r));
    CHECK(atomic_load(&completed) && atomic_load(&destroys)==21);
    CHECK(!vk_pipeline_async_on());
    pal_shutdown();puts("async pipelines: required work, bounded stop ownership and restart exclusion verified");return 0;
}
