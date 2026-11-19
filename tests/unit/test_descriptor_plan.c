/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "gpu/vk/vk_descriptor_plan.h"
#include "gpu/vk/vk_spirv_bindings.h"
#include "gpu/vk/vk_resource_limits.h"
#include "gpu/shader/shader_ops.h"
#include "gpu/shader/spv.h"
#include "gpu/vk/vk_internal.h"
#include "gpu/gpu_regs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line%d: %s\n",__LINE__,#x); return 1; } } while (0)

static void sites(sh_program *p, sh_eval *e, sh_site_shape *shape, unsigned n, bool store) {
    p->count=p->site_count=n;
    memset(shape,0,n*sizeof(*shape));memset(e->site,0,n*sizeof(*e->site));
    for(unsigned i=0;i<n;i++) {
        p->sites[i]=(sh_site){.kind=SH_SITE_IMAGE,.inst=i,.store=store};
        p->ops[i]=store?SH_OP_IMAGE_STORE:SH_OP_IMAGE_LOAD;p->insts[i].dim=2;
        shape[i].img_type=9;for(unsigned k=0;k<4;k++)shape[i].dst_sel[k]=(uint8_t)(4+k);
        e->site[i].valid=true;e->site[i].desc[0]=0x123456;e->site[i].sampler[0]=7;
    }
}
static unsigned planned(sh_program *p,sh_eval *e,sh_site_shape *shape,vk_descriptor_plan *plan) {
    vk_descriptor_plan_init(plan);
    return vk_descriptor_plan_stage(plan,p,e,shape,true)?vk_descriptor_plan_unique(plan):UINT32_MAX;
}
static int plans(sh_program *p,sh_eval *e,sh_site_shape *shape) {
    vk_descriptor_plan plan;
    sites(p,e,shape,70,false);CHECK(planned(p,e,shape,&plan)==2 && plan.count==71);
    vk_descriptor_plan_init(&plan);CHECK(vk_descriptor_plan_stage(&plan,p,e,shape,false) && vk_descriptor_plan_unique(&plan)==71);
    CHECK(planned(p,e,shape,&plan)==2);
    for(unsigned i=1;i<plan.count;i++)CHECK(plan.binding[i]==1);
    CHECK(vk_descriptor_plan_stage(&plan,p,e,shape,true) && plan.count==142 && vk_descriptor_plan_unique(&plan)==4);
    CHECK(plan.binding[71]==71 && plan.binding[72]==72 && plan.binding[141]==72);
    e->site[1].sampler[0]++;CHECK(planned(p,e,shape,&plan)==3);e->site[1].sampler[0]--;
    e->site[1].desc[7]++;CHECK(planned(p,e,shape,&plan)==3);e->site[1].desc[7]--;
    e->site[1].valid=false;CHECK(planned(p,e,shape,&plan)==3);e->site[1].valid=true;
    e->site[1].desc_unknown=true;CHECK(planned(p,e,shape,&plan)==3);e->site[1].desc_unknown=false;
    e->site[1].records_unknown=true;CHECK(planned(p,e,shape,&plan)==3);e->site[1].records_unknown=false;
    shape[1].address_map=1;CHECK(planned(p,e,shape,&plan)==3);shape[1].address_map=0;
    shape[1].dst_sel[0]=5;CHECK(planned(p,e,shape,&plan)==3);shape[1].dst_sel[0]=4;
    p->sites[1].kind=SH_SITE_BUFFER;CHECK(planned(p,e,shape,&plan)==3);p->sites[1].kind=SH_SITE_IMAGE;
    p->sites[1].store=true;CHECK(planned(p,e,shape,&plan)==3);p->sites[1].store=false;
    for(unsigned i=0;i<70;i++)e->site[i].desc[0]+=i;
    CHECK(planned(p,e,shape,&plan)==71); /* different images never collapse */
    sites(p,e,shape,20,true);
    for(unsigned i=0;i<20;i++)e->site[i].sampler[0]+=i;
    CHECK(planned(p,e,shape,&plan)==2); /* storage has no sampler */
    p->ops[1]=SH_OP_IMAGE_ATOMIC;CHECK(planned(p,e,shape,&plan)==3);p->ops[1]=SH_OP_IMAGE_STORE;
    for(unsigned i=0;i<20;i++)shape[i].img_type=13;
    p->insts[1].dim=3;CHECK(planned(p,e,shape,&plan)==3); /* 2D and array views differ */
    sites(p,e,shape,SH_MAX_SITES,false);CHECK(planned(p,e,shape,&plan)==2);
    vk_descriptor_plan_identity(&plan);CHECK(vk_descriptor_plan_unique(&plan)==SH_MAX_SITES+1);
    while(plan.count<VK_DESCRIPTOR_BINDINGS)CHECK(vk_descriptor_plan_append(&plan));
    CHECK(!vk_descriptor_plan_append(&plan));plan.binding[2]=3;CHECK(!vk_descriptor_plan_valid(&plan));
    sites(p,e,shape,70,false);CHECK(planned(p,e,shape,&plan)==2);
    VkWriteDescriptorSet writes[71]={0};VkDescriptorImageInfo images[71]={0};
    for(unsigned i=0;i<71;i++) {
        writes[i].descriptorType=i?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].descriptorCount=1;
        images[i]=(VkDescriptorImageInfo){.imageView=(VkImageView)(uintptr_t)1,.sampler=(VkSampler)(uintptr_t)2,
            .imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    CHECK(vk_descriptor_plan_images_match(&plan,writes,images,71));
    images[2].imageView=(VkImageView)(uintptr_t)3;CHECK(!vk_descriptor_plan_images_match(&plan,writes,images,71));images[2]=images[1];
    images[2].sampler=(VkSampler)(uintptr_t)3;CHECK(!vk_descriptor_plan_images_match(&plan,writes,images,71));images[2]=images[1];
    images[2].imageLayout=VK_IMAGE_LAYOUT_GENERAL;CHECK(!vk_descriptor_plan_images_match(&plan,writes,images,71));images[2]=images[1];
    writes[2].descriptorCount=2;CHECK(!vk_descriptor_plan_images_match(&plan,writes,images,71));writes[2].descriptorCount=1;
    CHECK(!vk_descriptor_plan_images_match(&plan,writes,images,70));
    uint8_t type[71],stage[71];for(unsigned i=0;i<71;i++){type[i]=(uint8_t)writes[i].descriptorType;stage[i]=1;}
    VkPhysicalDeviceLimits lim={.maxPerStageDescriptorStorageBuffers=16,.maxDescriptorSetStorageBuffers=16,
        .maxPerStageDescriptorSamplers=64,.maxDescriptorSetSamplers=64,.maxPerStageDescriptorSampledImages=128,
        .maxDescriptorSetSampledImages=128,.maxPerStageResources=128,.maxFragmentCombinedOutputResources=16};
    CHECK(vk_pipeline_descriptor_error(&lim,type,stage,71,0)!=NULL);
    CHECK(vk_pipeline_descriptor_error(&lim,type,stage,2,0)==NULL);
    return 0;
}

static int spirv_test(sh_program *p,sh_eval *e,sh_site_shape *shape) {
    const uint32_t prefix[]={0x7e000280,0x7e020280,0x7e0802ff,0x3e800000,0x7e0a02ff,0x3f000000,
        0x7e0c02ff,0x3f400000,0x7e0e02f2};
    uint32_t code[64];unsigned n=PS5C_ARRAY_COUNT(prefix);memcpy(code,prefix,sizeof(prefix));
    for(unsigned i=0;i<20;i++){code[n++]=0xf0200f08;code[n++]=0x00000400;}
    code[n++]=0xf0000f08;code[n++]=0x00000400;code[n++]=0xbf810000;
    CHECK(sh_analyze(code,n,0x100000,p) && p->site_count==21);
    memset(shape,0,p->site_count*sizeof(*shape));memset(e->site,0,p->site_count*sizeof(*e->site));
    for(unsigned i=0;i<p->site_count;i++) {
        shape[i].img_type=9;for(unsigned j=0;j<4;j++)shape[i].dst_sel[j]=(uint8_t)(4+j);
        e->site[i].valid=true;e->site[i].desc[0]=123;
    }
    vk_descriptor_plan plan;CHECK(planned(p,e,shape,&plan)==3);
    sh_stage_info info={.stage=SH_STAGE_CS,.local_size={1,1,1}};
    uint32_t *original=NULL,*copy=NULL;size_t words=0,changed=0;
    CHECK(sh_translate(p,&info,shape,0,&original,&words));
    uint32_t *saved=malloc(words*4);CHECK(saved);memcpy(saved,original,words*4);
    CHECK(vk_spirv_bindings(original,words,&plan,&copy,&changed) && copy && changed==words+63);
    CHECK(!memcmp(saved,original,words*4));free(saved);
    unsigned bindings=0,aliases=0,writes=0;
    for(size_t at=5;at<changed;at+=copy[at]>>16) {
        unsigned op=copy[at]&0xffff;
        if(op==SpvOpDecorate && copy[at+2]==SpvDecorationBinding){CHECK(copy[at+3]<=1 || copy[at+3]==21);bindings++;}
        if(op==SpvOpDecorate && copy[at+2]==SpvDecorationAliased)aliases++;
        if(op==SpvOpImageWrite)writes++;
    }
    CHECK(bindings==22 && aliases==21 && writes==20);
    uint32_t *again=NULL;size_t again_words=0;
    CHECK(vk_spirv_bindings(copy,changed,&plan,&again,&again_words) && !again && again_words==changed);
    FILE *f=fopen("descriptor-bindings.spv","wb");CHECK(f && fwrite(copy,4,changed,f)==changed && fclose(f)==0);
    /* Malformed/incompatible data is rejected without modifying or leaking output. */
    uint32_t save=copy[5];copy[5]=0;CHECK(!vk_spirv_bindings(copy,changed,&plan,&again,&again_words) && !again);copy[5]=save;
    for(size_t at=5;at<changed;at+=copy[at]>>16)if((copy[at]&0xffff)==SpvOpDecorate && copy[at+2]==SpvDecorationDescriptorSet) {
        copy[at+3]=1;CHECK(!vk_spirv_bindings(copy,changed,&plan,&again,&again_words));copy[at+3]=0;break;
    }
    free(copy);vk_descriptor_plan_identity(&plan);
    CHECK(vk_spirv_bindings(original,words,&plan,&again,&again_words) && !again && again_words==words);
    free(original);
    /* Read-only image aliases do not need pessimistic memory decorations.
     * The SSBO output is a distinct memory object, not an image write. */
    const uint32_t reads[]={0x7e000280,0x7e020280,0xf0000f08,0x00000400,0xf0000f08,0x00000400,
        0xbf8c3f70,0xe0780000,0x80030400,0xbf810000};
    CHECK(sh_analyze(reads,PS5C_ARRAY_COUNT(reads),0x200000,p) && p->site_count==3);
    memset(shape,0,p->site_count*sizeof(*shape));memset(e->site,0,p->site_count*sizeof(*e->site));
    for(unsigned s=0;s<3;s++)if(p->sites[s].kind==SH_SITE_IMAGE){shape[s].img_type=9;e->site[s].valid=true;e->site[s].desc[0]=123;}
    CHECK(planned(p,e,shape,&plan)==3);original=NULL;copy=NULL;
    CHECK(sh_translate(p,&info,shape,0,&original,&words));
    CHECK(vk_spirv_bindings(original,words,&plan,&copy,&changed) && copy && changed==words);
    for(size_t at=5;at<changed;at+=copy[at]>>16)
        CHECK((copy[at]&0xffff)!=SpvOpDecorate || copy[at+2]!=SpvDecorationAliased);
    f=fopen("descriptor-bindings-readonly.spv","wb");CHECK(f && fwrite(copy,4,changed,f)==changed && fclose(f)==0);
    free(copy);free(original);return 0;
}

static PFN_vkCreateDescriptorSetLayout real_layout;
static PFN_vkUpdateDescriptorSets real_update;
static uint32_t storage_max,sampled_max,store_updates,load_updates,layout_calls;
static VKAPI_ATTR VkResult VKAPI_CALL create_layout(VkDevice d,const VkDescriptorSetLayoutCreateInfo *info,
                                                    const VkAllocationCallbacks *a,VkDescriptorSetLayout *layout) {
    uint32_t storage=0,sampled=0;
    layout_calls++;
    for(uint32_t i=0;i<info->bindingCount;i++) {
        if(info->pBindings[i].descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)storage+=info->pBindings[i].descriptorCount;
        if(info->pBindings[i].descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)sampled+=info->pBindings[i].descriptorCount;
    }
    if(storage>storage_max)storage_max=storage;
    if(sampled>sampled_max)sampled_max=sampled;
    return real_layout(d,info,a,layout);
}
static VKAPI_ATTR void VKAPI_CALL update_set(VkDevice d,uint32_t count,const VkWriteDescriptorSet *w,
                                            uint32_t copies,const VkCopyDescriptorSet *copy) {
    for(uint32_t i=0;i<count;i++) {
        if(w[i].descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)store_updates=count;
        if(w[i].descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)load_updates=count;
    }
    real_update(d,count,w,copies,copy);
}
static void option(const char *name,const char *value) {
#ifdef _WIN32
    _putenv_s(name,value);
#else
    setenv(name,value,1);
#endif
    ps5c_env_reset();
}
static int gpu_test(void) {
    bool fallback=getenv("PS5C_TEST_DESCRIPTOR_FALLBACK")!=NULL;
    if(!getenv("PS5C_DESCRIPTOR_COMPACT"))option("PS5C_DESCRIPTOR_COMPACT","1");
    option("PS5C_STRICT_DESCRIPTOR_LIMITS","1");
    const char *cache=getenv("PS5C_TEST_DESCRIPTOR_CACHE");bool cached=cache!=NULL;
    if(cached)renderer_set_cache_dir(cache);
    const uint32_t prefix[]={0x7e000280,0x7e020280,0x7e080208,0x7e0a0209,0x7e0c020a,0x7e0e020b};
    uint32_t stores[64],loads[160];unsigned ns=PS5C_ARRAY_COUNT(prefix),nl=0;
    memcpy(stores,prefix,sizeof(prefix));
    for(unsigned i=0;i<20;i++){stores[ns++]=0xf0200f08;stores[ns++]=0x00000400;}stores[ns++]=0xbf810000;
    loads[nl++]=0x7e000280;loads[nl++]=0x7e020280;
    for(unsigned i=0;i<70;i++){loads[nl++]=0xf0000f08;loads[nl++]=0x00000400;}
    loads[nl++]=0xbf8c3f70;loads[nl++]=0xe0780000;loads[nl++]=0x80030400;loads[nl++]=0xbf810000;
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"descriptor compact fixture",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,stores,ns*4) && gmem_write(&m,a+0x400,loads,nl*4));
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 descriptor compaction regression",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    /* Conservative artificial limits never exceed the real device limits. */
    CHECK(!fallback || (r->props.limits.maxPerStageDescriptorStorageImages>=20 &&
                       r->props.limits.maxPerStageDescriptorSamplers>=70));
    if(!cached) {
        r->props.limits.maxPerStageDescriptorStorageImages=r->props.limits.maxDescriptorSetStorageImages=fallback?20:1;
        r->props.limits.maxPerStageDescriptorSamplers=r->props.limits.maxDescriptorSetSamplers=fallback?70:1;
        real_layout=vkCreateDescriptorSetLayout;vkCreateDescriptorSetLayout=create_layout;
        real_update=vkUpdateDescriptorSets;vkUpdateDescriptorSets=update_set;
    }
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;guest_addr out=a+0x1000;
    u[12]=(uint32_t)out;u[13]=(uint32_t)(out>>32);u[14]=16;u[15]=0x16204;
    g.regs[mmCOMPUTE_PGM_RSRC2]=16u<<1;
    g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    for(unsigned round=0;round<2;round++) {
        guest_addr image=a+0x4000+round*0x4000;vk_begin(r);
        vk_image *img=vk_image_get(r,image,16,16,VK_FORMAT_B8G8R8A8_UNORM);CHECK(img && vk_image_promote(r,img));
        u[0]=(uint32_t)(image>>8);u[1]=(uint32_t)(image>>40)|(56u<<20)|(3u<<30);
        u[2]=3u|(15u<<14);u[3]=(9u<<28)|0xfacu;
        float expected[]={round?0.5f:0.25f,round?0.25f:0.5f,round?0.125f:0.75f,1.0f};memcpy(u+8,expected,sizeof(expected));
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(a>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(a>>40);
        renderer_dispatch(r,&g,1,1,1);
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x400)>>8);
        renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,16);
        float got[4];CHECK(gmem_read(&m,out,got,sizeof(got)));
        for(unsigned i=0;i<4;i++)CHECK(fabsf(got[i]-expected[i])<0.01f);
        printf("descriptor GPU round%u: RGBA %.3f %.3f %.3f %.3f verified\n",round,got[0],got[1],got[2],got[3]);
    }
    if(!cached) {
        CHECK(storage_max==(fallback?20u:1u) && sampled_max==(fallback?70u:1u));
        CHECK(store_updates==(fallback?21u:2u) && load_updates==(fallback?72u:3u));
    }
    if(!fallback) {
        /* Same code, changing aliases: the last store uses a second T#.
         * Equal -> different -> equal must select two compatible pipelines. */
        const uint32_t initial[]={0x7e000280,0x7e020280,0x7e0802ff,0x3e800000,0x7e0a02ff,0x3f000000,
            0x7e0c02ff,0x3f400000,0x7e0e02f2};
        uint32_t changing[64];unsigned n=PS5C_ARRAY_COUNT(initial);memcpy(changing,initial,sizeof(initial));
        for(unsigned i=0;i<19;i++){changing[n++]=0xf0200f08;changing[n++]=0x00000400;}
        changing[n++]=0x7e0802ff;changing[n++]=0x3f400000;
        changing[n++]=0xf0200f08;changing[n++]=0x00020400;changing[n++]=0xbf810000;
        CHECK(gmem_write(&m,a+0x800,changing,n*4));
        if(!cached){r->props.limits.maxPerStageDescriptorStorageImages=2;r->props.limits.maxDescriptorSetStorageImages=2;}
        guest_addr first=a+0xc000,second=a+0x10000;
        vk_begin(r);vk_image *one=vk_image_get(r,first,16,16,VK_FORMAT_B8G8R8A8_UNORM);
        vk_image *two=vk_image_get(r,second,16,16,VK_FORMAT_B8G8R8A8_UNORM);
        CHECK(one && two && vk_image_promote(r,one) && vk_image_promote(r,two));
        unsigned before=layout_calls;
        for(unsigned round=0;round<3;round++) {
            u[0]=(uint32_t)(first>>8);u[1]=(uint32_t)(first>>40)|(56u<<20)|(3u<<30);
            u[2]=3u|(15u<<14);u[3]=(9u<<28)|0xfacu;u[4]=u[5]=u[6]=u[7]=0;
            memcpy(u+8,u,32);if(round==1)u[8]=(uint32_t)(second>>8);
            g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x800)>>8);renderer_dispatch(r,&g,1,1,1);
            u[12]=(uint32_t)out;u[13]=(uint32_t)(out>>32);u[14]=16;u[15]=0x16204;
            g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x400)>>8);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,16);
            float got[4];CHECK(gmem_read(&m,out,got,sizeof(got)));
            CHECK(fabsf(got[0]-(round==1?0.25f:0.75f))<0.01f && fabsf(got[1]-0.5f)<0.01f &&
                  fabsf(got[2]-0.75f)<0.01f && got[3]==1.0f);
        }
        if(!cached)CHECK(layout_calls-before==2 && storage_max==2);
        puts("descriptor GPU: equal/different/equal aliases reuse two pipelines with correct pixels");
    }
    if(!cached){vkCreateDescriptorSetLayout=real_layout;vkUpdateDescriptorSets=real_update;}
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&m);pal_shutdown();
    if(cached)puts("descriptor GPU: cached pipelines preserve pixels across alias changes");
    else printf("descriptor GPU: storage %u, sampled %u; descriptor writes %u/%u\n",storage_max,sampled_max,store_updates,load_updates);
    return 0;
}
int main(void) {
    sh_program *p=calloc(1,sizeof(*p));sh_eval *e=calloc(1,sizeof(*e));
    sh_site_shape *shape=calloc(SH_MAX_SITES,sizeof(*shape));CHECK(p && e && shape);
    CHECK(plans(p,e,shape)==0 && spirv_test(p,e,shape)==0);
    free(shape);free(e);free(p);puts("descriptor plans: exact image identity, real-binding guard, limits and SPIR-V aliasing verified");
    if(getenv("PS5C_TEST_GPU"))CHECK(gpu_test()==0);
    return 0;
}
