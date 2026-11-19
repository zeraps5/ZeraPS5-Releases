/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/shader/shader.h"
#ifndef SH_UNIFORM_CPU_ONLY
#include "gpu/vk/vk_internal.h"
#endif
#define CHECK(x) do { if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;} } while(0)

/* Deliberately derive the retained prefix from the used metadata rather than
 * asking the implementation for its cut/shift. This also keeps a common
 * producer/consumer off-by-one from making the test pass accidentally. */
static uint32_t reference_prefix(uint32_t sites) {
    uint32_t retained = ((sites + 1 + 3) / 4) * 4;
    return SH_U_SITE + (retained < SH_MAX_SITES ? retained : SH_MAX_SITES);
}

static uint32_t reference_index(uint32_t prefix, uint32_t logical) {
    if (logical < prefix || prefix == SH_U_VPORT) return logical;
    if (logical < SH_U_VPORT) return prefix - 1;
    return prefix + (logical - SH_U_VPORT);
}

static void reference_pack(uint32_t *dst, const uint32_t *src, uint32_t words, uint32_t prefix) {
    for (uint32_t i = 0; i < prefix; i++) dst[i] = src[i];
    if (prefix < SH_U_VPORT) dst[prefix - 1] = 0;
    for (uint32_t i = SH_U_VPORT; i < words; i++) dst[prefix + i - SH_U_VPORT] = src[i];
}

static int rejected_inputs(void) {
    enum { WORDS = SH_U_DATA + 37 };
    uint32_t *src = calloc(WORDS + 1, sizeof(*src));
    uint32_t *dst = malloc((WORDS + 1) * sizeof(*dst));
    uint32_t *copy = malloc((WORDS + 1) * sizeof(*copy));
    CHECK(src && dst && copy);
    for (uint32_t i = 0; i <= WORDS; i++) dst[i] = 0xfeed0000u + i;
    memcpy(copy, dst, (WORDS + 1) * sizeof(*dst));
    const uint32_t counts[] = {0, 1, 3, 14, 17, SH_MAX_SITES - 5, SH_MAX_SITES};
    for (unsigned i = 0; i < PS5C_ARRAY_COUNT(counts); i++) {
        uint32_t sites = counts[i], shift = sh_uniform_compact_shift(sites);
        uint32_t packed = WORDS - shift;
        CHECK(!sh_uniform_pack(dst, packed - 1, src, WORDS, sites, shift));
        CHECK(!sh_uniform_pack(dst, WORDS, src, SH_U_DATA - 1, sites, shift));
        CHECK(!sh_uniform_pack(dst, WORDS, src, UINT32_MAX, sites, shift));
        CHECK(!memcmp(dst, copy, WORDS * sizeof(*dst)));
    }
    const uint32_t invalid_shifts[] = {1, 2, 3, 4, SH_MAX_SITES, SH_U_VPORT, UINT32_MAX};
    for (unsigned i = 0; i < PS5C_ARRAY_COUNT(invalid_shifts); i++) {
        CHECK(!sh_uniform_shift_valid(0, invalid_shifts[i]));
        CHECK(!sh_uniform_pack(dst, WORDS, src, WORDS, 0, invalid_shifts[i]));
        CHECK(!memcmp(dst, copy, WORDS * sizeof(*dst)));
    }
    CHECK(!sh_uniform_shift_valid(SH_MAX_SITES + 1, 0));
    CHECK(!sh_uniform_shift_valid(UINT32_MAX, 0));
    CHECK(!sh_uniform_pack(dst, WORDS, src, WORDS, SH_MAX_SITES + 1, 0));
    CHECK(!sh_uniform_pack(dst, WORDS, src, WORDS, UINT32_MAX, 0));
    CHECK(!sh_uniform_pack(NULL, WORDS, src, WORDS, 0, 0));
    CHECK(!sh_uniform_pack(dst, WORDS, NULL, WORDS, 0, 0));
    /* No pointer may wrap when converting word counts to address ranges.
     * These synthetic addresses are rejected before any memory access. */
    uintptr_t last_word = UINTPTR_MAX - (sizeof(uint32_t) - 1);
    CHECK(!sh_uniform_pack(dst, SIZE_MAX, (const uint32_t *)last_word, UINT32_MAX, 0, 0));
    CHECK(!sh_uniform_pack((uint32_t *)last_word, SIZE_MAX, src, UINT32_MAX, 0, 0));
    CHECK(!memcmp(dst, copy, WORDS * sizeof(*dst)));
    /* Both partial-overlap directions must fail atomically, including legacy
     * mode. Identical pointers are covered separately by the packing test. */
    for (unsigned mode = 0; mode < 2; mode++) {
        uint32_t shift = mode ? sh_uniform_compact_shift(0) : 0;
        CHECK(!sh_uniform_pack(dst + 1, WORDS, dst, WORDS, 0, shift));
        CHECK(!sh_uniform_pack(dst, WORDS, dst + 1, WORDS, 0, shift));
        CHECK(!memcmp(dst, copy, WORDS * sizeof(*dst)));
    }
    free(src);free(dst);free(copy);
    return 0;
}

static int appended_data(void) {
    const uint32_t words = SH_U_DATA + 37, indices = 11, spare = 32;
    uint32_t *src = calloc(words, sizeof(*src));
    uint32_t *dst = malloc((words + indices + spare) * sizeof(*dst));
    CHECK(src && dst);
    src[SH_U_SITE] = SH_U_DATA + 4;
    src[SH_U_SITE + 1] = SH_U_DATA + 17;
    src[SH_U_SITE + 2] = UINT32_MAX; /* metadata may be a length, not an offset */
    src[SH_U_MESH + 3] = words; /* backend keeps this mesh index start logical */
    for (uint32_t i = SH_U_VPORT; i < words; i++) if (i != SH_U_MESH + 3) src[i] = 0xabcd0000u + i;
    for (unsigned mode = 0; mode < 2; mode++) {
        uint32_t shift = mode ? sh_uniform_compact_shift(3) : 0, packed = words - shift;
        CHECK(sh_uniform_pack(dst, packed, src, words, 3, shift));
        for (uint32_t i = 0; i < indices; i++) dst[packed + i] = 0x80000000u + i;
        for (uint32_t i = 0; i < spare; i++) dst[packed + indices + i] = 0xeeee0000u + i;
        for (uint32_t site = 0; site < 2; site++) {
            uint32_t logical = dst[sh_uniform_index(shift, SH_U_SITE + site)];
            CHECK(logical == src[SH_U_SITE + site]);
            for (uint32_t i = 0; i < 4; i++) CHECK(dst[sh_uniform_index(shift, logical + i)] == src[logical + i]);
        }
        CHECK(dst[sh_uniform_index(shift, SH_U_SITE + 2)] == UINT32_MAX);
        uint32_t start = dst[sh_uniform_index(shift, SH_U_MESH + 3)];
        CHECK(start == words);
        for (uint32_t i = 0; i < indices; i++) {
            CHECK(sh_uniform_index(shift, start + i) == packed + i);
            CHECK(dst[sh_uniform_index(shift, start + i)] == 0x80000000u + i);
        }
        uint32_t logical_length = packed + indices + spare + shift;
        CHECK(logical_length == words + indices + spare);
        for (uint32_t i = 0; i < spare; i++) {
            uint32_t at = sh_uniform_index(shift, logical_length - spare + i);
            CHECK(at == packed + indices + i && dst[at] == 0xeeee0000u + i);
        }
        CHECK(sh_uniform_index(shift, logical_length) == packed + indices + spare);
    }
    free(src);free(dst);
    return 0;
}

static int cpu_test(void) {
    const uint32_t maximum = SH_U_DATA + 37, spare = 32, guard = 0xfeedf00du;
    uint32_t *src = malloc(maximum * sizeof(*src)), *copy = malloc(maximum * sizeof(*copy));
    uint32_t *dst = malloc((maximum + spare + 2) * sizeof(*dst));
    uint32_t *expected = malloc(maximum * sizeof(*expected)), *inplace = malloc(maximum * sizeof(*inplace));
    CHECK(src && copy && dst && expected && inplace);
    /* Known hot-shader sizes, including alignment and one reserved sentinel. */
    CHECK(SH_U_DATA == 4264 && SH_U_VPORT == 4224);
    CHECK(sh_uniform_compact_shift(0) == 4092 && sh_uniform_compact_shift(3) == 4092);
    CHECK(sh_uniform_compact_shift(14) == 4080 && sh_uniform_compact_shift(17) == 4076);
    CHECK(sh_uniform_compact_shift(SH_MAX_SITES - 4) == 0);
    for (uint32_t n = 0; n <= SH_MAX_SITES; n++) {
        const uint32_t lengths[] = {SH_U_DATA, maximum};
        for (unsigned length = 0; length < PS5C_ARRAY_COUNT(lengths); length++) {
            uint32_t words = lengths[length];
            memset(src, 0, words * sizeof(*src));
            for (uint32_t i = 0; i < SH_SGPRS; i++) src[SH_U_SGPR + i] = 0x11110000u + i;
            for (uint32_t i = 0; i < n; i++) src[SH_U_SITE + i] = SH_U_DATA + i;
            for (uint32_t i = SH_U_VPORT; i < words; i++) src[i] = 0xaabb0000u + i;
            memcpy(copy, src, words * sizeof(*src));
            uint32_t prefix = reference_prefix(n), shift = SH_U_VPORT - prefix;
            CHECK(sh_uniform_compact_shift(n) == shift);
            CHECK(!(shift & 3u) && sh_uniform_shift_valid(n, shift) && sh_uniform_shift_valid(n, 0));
            CHECK(prefix >= SH_U_SITE + n && (!shift || prefix > SH_U_SITE + n));
            for (unsigned mode = 0; mode < 2; mode++) {
                uint32_t s = mode ? shift : 0, p = words - s, cut = mode ? prefix : SH_U_VPORT;
                reference_pack(expected, src, words, cut);
                for (uint32_t i = 0; i < maximum + spare + 2; i++) dst[i] = guard;
                /* Extra capacity belongs to the caller's mesh/probe sidecar;
                 * the packer must not copy it from beyond logical_words. */
                CHECK(sh_uniform_pack(dst + 1, p + spare, src, words, n, s));
                CHECK(!memcmp(dst + 1, expected, p * sizeof(*dst)));
                CHECK(dst[0] == guard);
                for (uint32_t i = p + 1; i < maximum + spare + 2; i++) CHECK(dst[i] == guard);
                for (uint32_t i = 0; i < words; i++) {
                    uint32_t index = sh_uniform_index(s, i);
                    CHECK(index == reference_index(cut, i) && index < p && dst[1 + index] == src[i]);
                    if (i >= SH_U_VPORT) CHECK((index & 3u) == (i & 3u));
                }
                const uint32_t starts[] = {0, SH_SGPRS - 1, SH_U_SITE + n, cut - 2, cut - 1,
                    cut, SH_U_VPORT - 2, SH_U_VPORT - 1, SH_U_VPORT, SH_U_UCP,
                    SH_U_MESH, SH_U_DATA, words - 1, UINT32_MAX - 3};
                for (unsigned j = 0; j < PS5C_ARRAY_COUNT(starts); j++) for (uint32_t k = 0; k < 16; k++) {
                    /* Match uint32 shader arithmetic, including wrap to SGPRs. */
                    uint32_t logical = starts[j] + k;
                    CHECK(sh_uniform_index(s, logical) == reference_index(cut, logical));
                }
                CHECK(sh_uniform_index(s, words) == p && sh_uniform_index(s, words + 1) == p + 1);
                CHECK(sh_uniform_index(s, UINT32_MAX) >= p);
                /* ArrayLength is physical plus shift, including appended data.
                 * Its final words must address the same probe/capture slots. */
                CHECK((uint64_t)p + spare + s == (uint64_t)words + spare);
                for (uint32_t k = 1; k <= spare; k++)
                    CHECK(sh_uniform_index(s, words + spare - k) == p + spare - k);
                memcpy(inplace, src, words * sizeof(*src));
                CHECK(sh_uniform_pack(inplace, words, inplace, words, n, s));
                CHECK(!memcmp(inplace, expected, p * sizeof(*inplace)));
                CHECK(!memcmp(src, copy, words * sizeof(*src)));
            }
        }
    }
    /* The sentinel must be written explicitly, even if a caller's reserved
     * source word is dirty. Every omitted word reads that one zero word. */
    uint32_t shift = sh_uniform_compact_shift(3), prefix = reference_prefix(3);
    memset(src, 0, maximum * sizeof(*src));
    for (uint32_t i = prefix - 1; i < SH_U_VPORT; i++) src[i] = 0xddddddddu;
    CHECK(sh_uniform_pack(dst, maximum - shift, src, maximum, 3, shift));
    for (uint32_t i = prefix - 1; i < SH_U_VPORT; i++) CHECK(dst[sh_uniform_index(shift, i)] == 0);
    memcpy(inplace, src, maximum * sizeof(*src));
    CHECK(sh_uniform_pack(inplace, maximum, inplace, maximum, 3, shift) && inplace[prefix - 1] == 0);
    CHECK(rejected_inputs() == 0 && appended_data() == 0);
    free(src);free(copy);free(dst);free(expected);free(inplace);
    puts("uniform CPU: 4097 site counts, independent byte oracle, dynamic boundaries/wrap, logical offsets/lengths, sidecars, alignment, sentinel, aliasing and rejection checks passed");
    return 0;
}

#ifndef SH_UNIFORM_CPU_ONLY
static void option(const char *key,const char *value) {
#ifdef _WIN32
    _putenv_s(key,value);
#else
    setenv(key,value,1);
#endif
    ps5c_env_reset();
}

static int gpu_test(void) {
    /* A real translated scalar load uses metadata to find its logical input;
     * four VGPR moves/store return it. Probe v4 at the store, exercising the
     * logical ArrayLength and stores into the relocated sidecar as well. */
    const uint32_t code[]={0xf4080100,0xfa000000,0xbf8cc07f,0x7e080204,0x7e0a0205,
        0x7e0c0206,0x7e0e0207,0xe0780000,0x80020400,0xbf810000};
    sh_program *p=calloc(1,sizeof(*p));CHECK(p && sh_analyze(code,PS5C_ARRAY_COUNT(code),0,p) && p->site_count==2);
    char probe[64];snprintf(probe,sizeof(probe),"%llx:0:1c:4",(unsigned long long)p->hash);option("PS5C_VS_PROBE",probe);
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 uniform ABI regression",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);
    VkDescriptorSetLayoutBinding bindings[2]={{.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};
    VkDescriptorSetLayout dsl;CHECK(vkCreateDescriptorSetLayout(r->device,&dl,NULL,&dsl)==VK_SUCCESS);
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&dsl};
    VkPipelineLayout layout;CHECK(vkCreatePipelineLayout(r->device,&pl,NULL,&layout)==VK_SUCCESS);
    VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
    VkDescriptorPoolCreateInfo pc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};
    VkDescriptorPool pool;CHECK(vkCreateDescriptorPool(r->device,&pc,NULL,&pool)==VK_SUCCESS);
    VkDescriptorSetAllocateInfo sa={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&dsl};
    VkDescriptorSet set;CHECK(vkAllocateDescriptorSets(r->device,&sa,&set)==VK_SUCCESS);
    const uint32_t words=SH_U_DATA+32;uint32_t *src=calloc(words,4);CHECK(src);
    vk_buffer output;CHECK(vk_buffer_create(r,16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true,&output));
    const uint32_t starts[]={SH_U_DATA,SH_U_VPORT,SH_U_MESH,SH_U_SITE+2,SH_U_VPORT-2,SH_U_SGPR+126};
    for(unsigned mode=0;mode<2;mode++) {
        sh_stage_info info={.stage=SH_STAGE_CS,.local_size={1,1,1}};
        info.uniform_shift=mode?sh_uniform_compact_shift(p->site_count):0;
        sh_site_shape shapes[SH_MAX_SITES]={0};uint32_t *spv=NULL;size_t n=0;CHECK(sh_translate(p,&info,shapes,0,&spv,&n));
        const char *dump=getenv("PS5C_TEST_SPIRV");
        if(dump) {
            char path[512];snprintf(path,sizeof(path),"%s/uniform-layout-%u.spv",dump,mode);
            FILE *f=fopen(path,"wb");CHECK(f && fwrite(spv,4,n,f)==n && !fclose(f));
        }
        VkShaderModuleCreateInfo mc={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=n*4,.pCode=spv};
        VkShaderModule module;CHECK(vkCreateShaderModule(r->device,&mc,NULL,&module)==VK_SUCCESS);free(spv);
        VkComputePipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=layout,
            .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"}};
        VkPipeline pipe;CHECK(vkCreateComputePipelines(r->device,VK_NULL_HANDLE,1,&pi,NULL,&pipe)==VK_SUCCESS);
        uint32_t physical=words-info.uniform_shift;vk_buffer uniforms;
        CHECK(vk_buffer_create(r,(uint64_t)physical*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true,&uniforms));
        VkDescriptorBufferInfo bi[2]={{uniforms.buffer,0,physical*4},{output.buffer,0,16}};
        VkWriteDescriptorSet writes[2];memset(writes,0,sizeof(writes));
        for(unsigned k=0;k<2;k++)writes[k]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=k,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=bi+k};
        vkUpdateDescriptorSets(r->device,2,writes,0,NULL);
        for(unsigned c=0;c<PS5C_ARRAY_COUNT(starts);c++) {
            memset(src,0,words*4);src[126]=src[127]=UINT32_MAX;src[10]=16;src[11]=0x30016204;
            for(uint32_t i=SH_U_VPORT;i<SH_U_DATA+16;i++)src[i]=0x20000000u+i;
            src[SH_U_MESH+3]=SH_U_DATA+4;src[SH_U_SITE]=starts[c];src[SH_U_SITE+1]=4;
            for(uint32_t i=words-16;i<words;i++)src[i]=0xeeeeeeeeu;
            CHECK(sh_uniform_pack(uniforms.mapped,physical,src,words,p->site_count,info.uniform_shift));memset(output.mapped,0xcd,16);
            VkCommandBuffer cmd=vk_begin(r);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,NULL);vkCmdDispatch(cmd,1,1,1);
            VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
            renderer_wait_idle(r);uint32_t *got=output.mapped,*u=uniforms.mapped;
            for(unsigned k=0;k<4;k++)CHECK(got[k]==src[starts[c]+k]);
            CHECK(u[physical-16]==got[0] && u[physical-1]==0);
        }
        vk_buffer_destroy(r,&uniforms);vkDestroyPipeline(r->device,pipe,NULL);vkDestroyShaderModule(r->device,module,NULL);
    }
    free(src);free(p);vk_buffer_destroy(r,&output);vkDestroyDescriptorPool(r->device,pool,NULL);
    vkDestroyPipelineLayout(r->device,layout,NULL);vkDestroyDescriptorSetLayout(r->device,dsl,NULL);
    renderer_destroy(r);pal_window_destroy(window);gmem_free(&m);pal_shutdown();
    puts("uniform GPU: scalar payload/control/gap boundaries and relocated probe sidecar match both layouts");return 0;
}

#endif

int main(void) {
    CHECK(cpu_test() == 0);
#ifndef SH_UNIFORM_CPU_ONLY
    const char *gpu_mode = getenv("PS5C_TEST_GPU");
    if (gpu_mode && !strcmp(gpu_mode, "1")) return gpu_test();
#endif
    return 0;
}
