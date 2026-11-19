/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Synthetic regression tests of the actual cache implementation. Including
 * the backend keeps its internal helpers out of the public production API.
 * No Vulkan device, game files or console libraries are required. */
#include "../../src/gpu/vk/vk_pipeline.c"
#include <stdio.h>
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 2;}}while(0)

static uint64_t texture_reference(renderer *r,const texture_cpu_source *s,bool full){
    const uint8_t *src=guest_host(r,s->addr,s->size);
    uint64_t packed=(uint64_t)s->w*s->h*s->bpe;
    if(!src || !packed || !s->layers)return 0;
    uint64_t hash=FNV_INIT;
    if(packed*s->layers<=SMALL_TEXTURE){
        for(uint32_t l=0;l<s->layers;l++){
            untile(src+s->slice*l,r->pipes->tex_hash_scratch,s->sw,s->w,s->h,s->bpe,s->ox,s->oy);
            hash=texture_hash_words(r->pipes->tex_hash_scratch,(size_t)packed,hash);
        }return hash|1;
    }
    /* Compare the padded path, whose fingerprint must remain identical. */
    if(!s->ox && !s->oy && s->size==packed*s->layers)return 0;
    uint32_t bw=0,bh=0,block=0;if(s->sw)std_block(s->sw,s->bpe,&bw,&bh,&block);
    uint64_t total=packed*s->layers,pitch=vk_linear_pitch(s->w,s->bpe);
    if(full && !s->sw){
        for(uint32_t l=0;l<s->layers;l++)for(uint32_t y=0;y<s->h;y++)
            hash=texture_hash_words(src+s->slice*l+((uint64_t)y+s->oy)*pitch+(uint64_t)s->ox*s->bpe,(size_t)s->w*s->bpe,hash);
        return hash|1;
    }
    uint64_t blocks=bw?((uint64_t)s->w+s->ox+bw-1)/bw:0,count=full?total:256*64;
    for(uint64_t i=0;i<count;){
        uint64_t logical=full?i:((total-64)/255)*(i/64)+(((total-64)%255)*(i/64))/255;
        uint64_t chunk=full?(total-i<64?total-i:64):64;uint8_t bytes[64];
        for(uint64_t k=0;k<chunk;){
            uint64_t at=logical+k,layer=at/packed,pixel=(at%packed)/s->bpe;
            uint32_t x=(uint32_t)(pixel%s->w)+s->ox,y=(uint32_t)(pixel/s->w)+s->oy;
            uint64_t offset=s->sw?((uint64_t)(y/bh)*blocks+x/bw)*block+std_offset(s->sw,x,y,s->bpe):(uint64_t)y*pitch+(uint64_t)x*s->bpe;
            uint64_t run=s->sw?s->bpe-at%s->bpe:(uint64_t)(s->w-(x-s->ox))*s->bpe-at%s->bpe;
            if(run>chunk-k)run=chunk-k;
            memcpy(bytes+k,src+layer*s->slice+offset+at%s->bpe,(size_t)run);k+=run;
        }
        hash=texture_hash_words(bytes,(size_t)chunk,hash);i+=chunk;
    }return hash|1;
}

static int texture_test(renderer *r,guest_addr a){
    unsigned cases=0;const unsigned modes[]={0,1,5,9,24,27};
    for(unsigned m=0;m<6;m++)for(unsigned k=0;k<5;k++)for(unsigned l=1;l<=3;l+=2){
        texture_cpu_source s={.addr=a,.sw=modes[m],.w=257+2*k,.h=259-k,.bpe=1u<<k,.ox=3+k,.oy=5,.layers=l};
        s.slice=surface_bytes(s.sw,s.w+s.ox,s.h+s.oy,s.bpe)+4096;
        s.size=s.slice*s.layers;
        for(unsigned full=0;full<2;full++){
            uint64_t old=texture_reference(r,&s,full),now=texture_cpu_hash(r,&s,full);
            if(old!=now){fprintf(stderr,"texture sw%u bpe%u layers%u full%u: %llx != %llx\n",s.sw,s.bpe,l,full,(unsigned long long)old,(unsigned long long)now);return 1;}
            cases++;
        }
    }
    printf("padded texture fingerprint equivalence: %u cases passed\n",cases);return 0;
}

static int context_test(renderer *r,guest_addr a){
    const uint32_t code[]={0x7e020204,0x4a060200,0x7e100503,0xbe890301,0xf4080304,0xfa000000,0xe0302000,0x80030400,0xbf810000};
    sh_program *p=calloc(1,sizeof(*p));stage_state *st=calloc(1,sizeof(*st));
    REQUIRE(p && st && sh_analyze(code,PS5C_ARRAY_COUNT(code),0,p));
    REQUIRE(p->lane_reads && p->site_count==2);
    uint32_t desc[]={0x12345678,0x00040000,64,0x16204};REQUIRE(gmem_write(r->mem,a,desc,sizeof(desc)));
    st->prog=p;st->info.stage=SH_STAGE_CS;st->info.cs_tgid_sgpr=5;st->eval.group_sgpr=5;
    st->eval.sgpr_init[0]=(uint32_t)a;st->eval.sgpr_init[1]=(uint32_t)(a>>32);
    st->eval.sgpr_init[126]=st->eval.sgpr_init[127]=UINT32_MAX;
    REQUIRE(setup_stage(r,st));
    bool first=st->eval.site[1].desc_unknown;uint64_t hits=g_stat_eval_hits;
    /* Same initial values, new legal RSRC2 interface: s4 used to be user
     * data 0; now it is representative workgroup X=0 (other groups differ). */
    st->eval.group_sgpr=4;st->eval.group_sgprs=1;st->info.cs_tgid_sgpr=4;st->info.cs_tgid_enable=1;r->pipes->epoch++;
    REQUIRE(setup_stage(r,st));bool cached=st->eval.site[1].desc_unknown;
    bool hit=g_stat_eval_hits>hits;
    for(unsigned i=0;i<EVAL_CACHE;i++)r->pipes->evals[i].key=0;
    REQUIRE(setup_stage(r,st));bool fresh=st->eval.site[1].desc_unknown;
    printf("context change: first_unknown=%u cached_unknown=%u fresh_unknown=%u cross_epoch_hit=%u\n",first,cached,fresh,hit);
    for(unsigned i=0;i<EVAL_CACHE;i++)r->pipes->evals[i].key=0;
    sh_eval_free(&st->eval);free(st);free(p);
    return !first && fresh && cached==fresh && !hit?0:1;
}

static int source_test(renderer *r,guest_addr a){
    const uint32_t code[]={0xf4000004,0xfa000000,0xbf810000};
    sh_program *p=calloc(1,sizeof(*p));stage_state *st=calloc(1,sizeof(*st));
    REQUIRE(p && st && sh_analyze(code,3,0,p));st->prog=p;st->info.stage=SH_STAGE_CS;
    st->eval.sgpr_init[8]=(uint32_t)a;st->eval.sgpr_init[9]=(uint32_t)(a>>32);
    REQUIRE(setup_stage(r,st));eval_entry *c=NULL;
    for(unsigned i=0;i<EVAL_CACHE;i++)if(r->pipes->evals[i].key && r->pipes->evals[i].prog==p)c=&r->pipes->evals[i];
    REQUIRE(c && c->source_hash_valid && c->read.count==1);uint64_t key=c->key;
    r->pipes->epoch++;REQUIRE(eval_lookup(r,st,key)==c);
    r->pipes->epoch++;r->pipes->wb_count=1;r->pipes->wb[0].addr=a;r->pipes->wb[0].size=4;
    REQUIRE(eval_lookup(r,st,key)==NULL);r->pipes->wb_count=0;
    r->pipes->held_count=1;r->pipes->held[0].rel.addr=a;r->pipes->held[0].rel.data_sel=1;
    REQUIRE(eval_lookup(r,st,key)==NULL);r->pipes->held_count=0;
    r->pipes->copy_count=1;r->pipes->copies[0].addr=a;r->pipes->copies[0].size=4;
    uint32_t changed=0x22334455;REQUIRE(gmem_write(r->mem,a,&changed,4));
    REQUIRE(eval_lookup(r,st,key)==NULL && !c->key && !r->pipes->copy_count);
    /* 16 non-contiguous scalar loads must stay within EVAL_RANGES without overflow. */
    uint32_t multi[33];
    for(unsigned i=0;i<16;i++){multi[2*i]=0xf4000004;multi[2*i+1]=0xfa000000u|(i*64u);}
    multi[32]=0xbf810000;
    sh_program *pm=calloc(1,sizeof(*pm));stage_state *stm=calloc(1,sizeof(*stm));
    REQUIRE(pm && stm && sh_analyze(multi,33,0,pm));stm->prog=pm;stm->info.stage=SH_STAGE_CS;
    stm->eval.sgpr_init[8]=(uint32_t)a;stm->eval.sgpr_init[9]=(uint32_t)(a>>32);
    REQUIRE(setup_stage(r,stm));
    eval_entry *cm=NULL;
    for(unsigned i=0;i<EVAL_CACHE;i++)if(r->pipes->evals[i].key && r->pipes->evals[i].prog==pm)cm=&r->pipes->evals[i];
    REQUIRE(cm && !cm->read.overflow && cm->read.count==16 && cm->source_hash_valid);
    r->pipes->epoch++;REQUIRE(eval_lookup(r,stm,cm->key)==cm);
    sh_eval_free(&stm->eval);free(stm);free(pm);
    puts("cross-epoch guards: stable hit, pending GPU write, held label, changed CPU bytes, 16-range load and staged-copy invalidation passed");
    sh_eval_free(&st->eval);free(st);free(p);return 0;
}

static int vector_header_test(renderer *r,guest_addr a){
    const uint32_t code[]={0x10040204,0xe0702000,0x80020200,0xbf810000};
    sh_program *p=calloc(1,sizeof(*p));stage_state *st=calloc(1,sizeof(*st));
    REQUIRE(p && st && sh_analyze(code,4,0,p));REQUIRE(!(p->eval_sgpr_lo&(1u<<4)));
    st->prog=p;st->info.stage=SH_STAGE_CS;
    st->eval.sgpr_init[8]=(uint32_t)a;st->eval.sgpr_init[9]=(uint32_t)(a>>32);st->eval.sgpr_init[10]=256;
    st->eval.sgpr_init[4]=0x12345678;REQUIRE(setup_stage(r,st));
    uint64_t hits=g_stat_eval_hits;st->eval.sgpr_init[4]=0x87654321;r->pipes->epoch++;
    REQUIRE(setup_stage(r,st) && g_stat_eval_hits>hits && st->eval.smem_data[4]==0x87654321);
    for(unsigned i=0;i<EVAL_CACHE;i++)r->pipes->evals[i].key=0;
    puts("vector-only SGPR: unmasked cache hit refreshes the uploaded SGPR header");
    sh_eval_free(&st->eval);free(st);free(p);return 0;
}

static int site_header_test(renderer *r){
    sh_program *p=calloc(1,sizeof(*p));sh_eval *e=calloc(1,sizeof(*e)),*small=calloc(1,sizeof(*small));
    uint32_t *code=calloc(2*SH_MAX_SITES+1,sizeof(*code));REQUIRE(p && e && small && code);
    for(unsigned i=0;i<SH_MAX_SITES;i++){code[2*i]=0xe0302000;code[2*i+1]=0x80020200;}
    code[2*SH_MAX_SITES]=0xbf810000;
    REQUIRE(sh_analyze(code,2*SH_MAX_SITES+1,0,p) && sh_evaluate(p,r->mem,e));
    REQUIRE(e->site_dirty==SH_MAX_SITES);
    /* Stage reuse must retain the dirty high-water mark with its allocation. */
    stage_state *reused=calloc(1,sizeof(*reused));REQUIRE(reused);
    reused->eval=*e;reset_stage(r->pipes,reused);*e=reused->eval;free(reused);
    REQUIRE(e->site_dirty==SH_MAX_SITES && e->smem_cap>=SH_U_DATA);
    for(unsigned i=SH_U_SITE;i<SH_U_DATA;i++)e->smem_data[i]=0xdeadbeef;
    code[2]=0xbf810000;REQUIRE(sh_analyze(code,3,0,p) && sh_evaluate(p,r->mem,small));
    REQUIRE(eval_copy_sparse(&e->smem_data,&e->smem_words,&e->smem_cap,&e->site_dirty,
                             small->smem_data,small->smem_words,p->site_count,NULL));
    for(unsigned i=SH_U_SITE;i<SH_U_DATA;i++)REQUIRE(e->smem_data[i]==0);
    REQUIRE(sh_evaluate(p,r->mem,e) && e->site_dirty==1);
    for(unsigned i=SH_U_SITE;i<SH_U_DATA;i++)REQUIRE(e->smem_data[i]==0);
    /* A cache allocation sized exactly to the header must also grow cleanly. */
    sh_eval *tight=calloc(1,sizeof(*tight));REQUIRE(tight);
    REQUIRE(eval_copy_sparse(&tight->smem_data,&tight->smem_words,&tight->smem_cap,&tight->site_dirty,
                             small->smem_data,small->smem_words,p->site_count,NULL));
    unsigned old=tight->smem_cap;const uint32_t load[]={0xf4000004,0xfa000000,0xbf810000};
    REQUIRE(sh_analyze(load,3,0,p) && sh_evaluate(p,r->mem,tight));
    REQUIRE(tight->smem_cap>old);
    for(unsigned i=old+1;i<tight->smem_cap;i++)REQUIRE(tight->smem_data[i]==0);
    puts("uniform header: 4096->1 sites, cache copy, viewport/mesh/clip tail and newly grown capacity passed");
    sh_eval_free(e);sh_eval_free(small);sh_eval_free(tight);free(e);free(small);free(tight);free(code);free(p);return 0;
}

static int sparse_copy_test(void){
    uint32_t *data=NULL,words=0,cap=0,dirty=0,rng=0x9e3779b9;
    uint32_t src[SH_U_DATA+257],expected[SH_U_DATA+257],sgpr[SH_U_SITE];
    for(unsigned turn=0;turn<300;turn++){
        rng=rng*1664525u+1013904223u;
        uint32_t sites=turn==0?SH_MAX_SITES:turn==1?0:rng%(SH_MAX_SITES+1);
        uint32_t n=SH_U_DATA+turn%258;
        for(uint32_t k=0;k<n;k++)src[k]=k*7919u+turn+1;
        memset(src+SH_U_SITE+sites,0,(SH_U_VPORT-SH_U_SITE-sites)*4);
        memcpy(expected,src,n*4);
        for(uint32_t k=0;k<SH_U_SITE;k++)sgpr[k]=0xa5000000u+turn+k;
        if(turn&1)memcpy(expected,sgpr,sizeof(sgpr));
        REQUIRE(eval_copy_sparse(&data,&words,&cap,&dirty,src,n,sites,turn&1?sgpr:NULL));
        REQUIRE(words==n && dirty==sites && !memcmp(data,expected,n*4));
    }
    free(data);puts("sparse uniforms: 300 varying site counts, dirty tails, allocation growth and SGPR overrides match full-copy semantics");
    return 0;
}

static int small_texture_write_test(renderer *r,guest_addr a){
    vk_pipelines *P=r->pipes;
    texture_cpu_source *s=&P->tex_cpu[0];
    *s=(texture_cpu_source){.addr=a,.size=1024,.slice=1024,.sw=0,.w=16,.h=16,.bpe=4,.layers=1};
    P->epoch=77;P->tex_hash[0]=texture_cpu_hash(r,s,false);P->tex_checked[0]=P->epoch;
    uint64_t old=P->tex_hash[0];uint32_t data=0xabcdef01;
    REQUIRE(gmem_write(r->mem,a,&data,4));
    /* CPU/DMA writes may occur after the last sample in this submission. */
    texture_gpu_write(r,&r->images[0]);
    REQUIRE(P->tex_hash[0]!=old && P->tex_hash[0]==texture_cpu_hash(r,s,false));
    REQUIRE(!P->tex_checked[0]);
    puts("small ring texture: write-time baseline rechecked within the submission");
    return 0;
}

/* Red reuses one guest address for BC3 and BC1 (different block sizes).
 * Exercise the actual texture uploader, not just a format predicate. */
static int compressed_alias_gpu_test(gmem *mem,guest_addr a){
    if(!getenv("PS5C_TEST_GPU"))return 0;
    pal_window *window=NULL;REQUIRE(pal_window_open("ZeraPS5 compressed texture aliases",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,mem);REQUIRE(r);
    vk_begin(r);REQUIRE(vk_image_get(r,a,512,512,VK_FORMAT_BC3_UNORM_BLOCK));
    vk_buffer raw;REQUIRE(vk_buffer_create(r,262144,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&raw));
    const uint32_t formats[]={169,173,170,169};
    const VkFormat expected[]={VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_FORMAT_BC3_UNORM_BLOCK,VK_FORMAT_BC1_RGBA_SRGB_BLOCK,VK_FORMAT_BC1_RGBA_UNORM_BLOCK};
    for(unsigned round=0;round<PS5C_ARRAY_COUNT(formats);round++){
        size_t size=round==1?262144:131072;uint8_t *src=gmem_host(mem,a,size);REQUIRE(src);memset(src,13+17*round,size);
        renderer_submit_begin(r);vk_begin(r);REQUIRE(vk_guest_access_begin(r));
        uint32_t t[8]={(uint32_t)(a>>8),(uint32_t)(a>>40)|(formats[round]<<20)|(3u<<30),127u|(511u<<14),0x90000fac};
        vk_image *img=texture(r,t);
        REQUIRE(img && img->format==expected[round]);
        memset(raw.mapped,0xcd,262144);
        vk_barrier(r->cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=(VkExtent3D){512,512,1};
        vkCmdCopyImageToBuffer(r->cmd,img->image,img->layout,raw.buffer,1,&copy);vk_guest_access_end(r);renderer_wait_idle(r);
        REQUIRE(!memcmp(raw.mapped,src,size));
        if(size<262144)REQUIRE(((uint8_t *)raw.mapped)[size]==0xcd);
    }
    vk_buffer_destroy(r,&raw);renderer_destroy(r);pal_window_destroy(window);
    puts("compressed aliases: BC3 -> BC1 -> BC3 -> sRGB BC1 -> BC1 at one address preserve every uploaded byte");return 0;
}

/* Exercise packed 3D uploads and same-address format aliases through Vulkan. */
static int packed_volume_gpu_test(gmem *mem, guest_addr a) {
    if (!getenv("PS5C_TEST_GPU")) return 0;
    pal_window *window = NULL;
    REQUIRE(pal_window_open("ZeraPS5 packed volume regression", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, mem); REQUIRE(r);
    const uint32_t count = 64 * 64 * 16;
    vk_buffer raw; REQUIRE(vk_buffer_create(r, count * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &raw));
    /* Unchanged guest bytes must be decoded again when only the packed format changes. */
    uint8_t *src = gmem_host(mem, a, count * 2); REQUIRE(src);
    for (uint32_t i = 0; i < count * 2; i++) src[i] = (uint8_t)(i * 37 + (i >> 8));
    uint8_t *expected = malloc(count * 4); REQUIRE(expected);
    const uint32_t formats[] = {133, 134, 135, 136, 137, 133};
    for (uint32_t k = 0; k < PS5C_ARRAY_COUNT(formats); k++) {
        uint32_t fmt = formats[k];
        uint32_t bpe = vk_packed_bytes(fmt);
        uint64_t slice = surface_bytes(0, 64, 64, bpe);
        for (uint32_t z = 0; z < 16; z++)
            untile(src + slice * z, expected + 64 * 64 * bpe * z, 0, 64, 64, bpe, 0, 0);
        REQUIRE(vk_packed_expand(fmt, expected, count));
        renderer_submit_begin(r); vk_begin(r); REQUIRE(vk_guest_access_begin(r));
        uint32_t t[8] = {(uint32_t)(a >> 8), (uint32_t)(a >> 40) | (fmt << 20) | (3u << 30),
                        15u | (63u << 14), 0xa0000fac, 15};
        vk_image *img = texture(r, t);
        REQUIRE(img && img->format == VK_FORMAT_R8G8B8A8_UNORM);
        vk_barrier(r->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {0};
        copy.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = (VkExtent3D){64, 64, 16};
        vkCmdCopyImageToBuffer(r->cmd, img->image, img->layout, raw.buffer, 1, &copy);
        vk_guest_access_end(r); renderer_wait_idle(r);
        REQUIRE(!memcmp(raw.mapped, expected, count * 4));
    }
    free(expected); vk_buffer_destroy(r, &raw); renderer_destroy(r); pal_window_destroy(window);
    puts("packed volume: five formats, 16 slices and unchanged-byte format aliases verified on GPU");
    return 0;
}

/* Constants at another address: the base-only SGPR leaves the key; the
 * content at the moved range decides; the V# base word is this draw's. */
static int relocate_test(renderer *r,guest_addr a){
    const uint32_t code[]={0xf4200100,0xfa000000,0xf4200140,0xfa000004,0xbf810000}; /* s_buffer_load_dword s4/s5, s[0:3], 0/4 */
    const uint32_t used[]={0xf4200100,0xfa000000,0x80058400,0xbf810000}; /* ... s_add_u32 s5, s0, 4 */
    sh_program *p=calloc(1,sizeof(*p));stage_state *st=calloc(1,sizeof(*st));
    REQUIRE(p && st && sh_analyze(used,PS5C_ARRAY_COUNT(used),0,p) && p->eval_reloc_sgprs==0);
    REQUIRE(sh_analyze(code,PS5C_ARRAY_COUNT(code),0,p) && p->eval_reloc_sgprs==1);
    const uint32_t same[]={0x11111111,0x22222222},other[]={0x11111111,0x33333333};
    REQUIRE(gmem_write(r->mem,a+0x10000,same,8) && gmem_write(r->mem,a+0x11000,same,8) && gmem_write(r->mem,a+0x12000,other,8));
    st->prog=p;st->info.stage=SH_STAGE_CS;
    st->eval.sgpr_init[1]=(uint32_t)(a>>32);st->eval.sgpr_init[2]=64;st->eval.sgpr_init[3]=0x16204;
    st->eval.sgpr_init[0]=(uint32_t)(a+0x10000);REQUIRE(setup_stage(r,st));
    uint64_t moved=g_stat_eval_relocated;
    st->eval.sgpr_init[0]=(uint32_t)(a+0x11000);REQUIRE(setup_stage(r,st) && g_stat_eval_relocated==moved+1);
    REQUIRE(st->eval.site[0].desc[0]==(uint32_t)(a+0x11000) && st->eval.site[1].desc[0]==(uint32_t)(a+0x11000));
    REQUIRE(st->eval.smem_data[0]==(uint32_t)(a+0x11000));
    st->eval.sgpr_init[0]=(uint32_t)(a+0x12000);REQUIRE(setup_stage(r,st) && g_stat_eval_relocated==moved+1);
    uint32_t at=st->eval.site[1].smem_offset;REQUIRE(st->eval.smem_data[at]==0x33333333);
    for(unsigned i=0;i<EVAL_CACHE;i++)r->pipes->evals[i].key=0;
    puts("relocated constants: base-only SGPR, moved hit with this draw's V# base, changed content evaluated again");
    sh_eval_free(&st->eval);free(st);free(p);return 0;
}

static int cached_capability_test(void) {
    uint32_t code[]={0x07230203,0x10300,0,8,0,(2u<<16)|SpvOpCapability,5016,
        (2u<<16)|SpvOpCapability,5347};
    REQUIRE(!spirv_features_allowed(code,PS5C_ARRAY_COUNT(code),false,false));
    REQUIRE(!spirv_features_allowed(code,PS5C_ARRAY_COUNT(code),true,false));
    REQUIRE(!spirv_features_allowed(code,PS5C_ARRAY_COUNT(code),false,true));
    REQUIRE(spirv_features_allowed(code,PS5C_ARRAY_COUNT(code),true,true));
    REQUIRE(spirv_features_allowed(code,7,false,true));
    REQUIRE(!spirv_features_allowed(code,6,true,true));
    code[5]=0;REQUIRE(!spirv_features_allowed(code,PS5C_ARRAY_COUNT(code),true,true));
    return 0;
}

static int local_compute_input_test(void) {
    VkPhysicalDeviceMemoryProperties memory={0};memory.memoryHeapCount=1;memory.memoryTypeCount=1;
    memory.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    memory.memoryHeaps[0].size=224395264;
    VkDeviceSize cap=local_arena_capacity(&memory,256u<<20,2);
    REQUIRE(cap>0 && cap*4<=memory.memoryHeaps[0].size && !(cap&65535) && cap<(256u<<20));
    REQUIRE(local_arena_capacity(&memory,8u<<20,2)==(8u<<20));
    memory.memoryHeaps[0].size=16ull<<30;
    REQUIRE(local_arena_capacity(&memory,256u<<20,2)==(256u<<20));
    memory.memoryTypes[0].propertyFlags&=~VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    REQUIRE(local_arena_capacity(&memory,256u<<20,2)==0 && local_arena_capacity(&memory,256u<<20,0)==0);
    /* Only the copied VMA metadata is needed by the physical-alias planner;
     * no guest bytes or Vulkan objects are accessed in this unit fixture. */
    gmem mem={0};REQUIRE(pal_mutex_create(&mem.lock)==PS5C_OK);
    gmem_vma maps[2]={{.start=0x10000,.size=0x1000,.kind=GVMA_VIEW,.os_base=0x10000,.phys=0x40000},
                      {.start=0x20000,.size=0x1000,.kind=GVMA_VIEW,.os_base=0x20000,.phys=0x50000}};
    mem.vmas.data=maps;mem.vmas.len=mem.vmas.cap=2;
    renderer *r=calloc(1,sizeof(*r));stage_state *st=calloc(1,sizeof(*st));sh_program *p=calloc(1,sizeof(*p));
    REQUIRE(r && st && p);r->mem=&mem;st->prog=p;p->site_count=2;
    p->sites[0].kind=p->sites[1].kind=SH_SITE_BUFFER;p->sites[1].store=true;
    st->eval.site[0].valid=st->eval.site[1].valid=true;
    guest_addr bases[2]={0x10000,0x20000};uint64_t sizes[2]={256,256};
    REQUIRE(local_compute_read_safe(r,st,0,bases,sizes,256));
    bases[1]=0x10080;REQUIRE(!local_compute_read_safe(r,st,0,bases,sizes,256));
    bases[1]=0x10100;REQUIRE(local_compute_read_safe(r,st,0,bases,sizes,256));
    bases[1]=0x20000;maps[1].phys=maps[0].phys;
    REQUIRE(!local_compute_read_safe(r,st,0,bases,sizes,256));
    maps[1].phys=maps[0].phys+256;REQUIRE(local_compute_read_safe(r,st,0,bases,sizes,256));
    maps[1].phys=maps[0].phys+128;REQUIRE(!local_compute_read_safe(r,st,0,bases,sizes,256));
    maps[1].phys=0x50000;st->eval.site[1].desc_unknown=true;
    REQUIRE(!local_compute_read_safe(r,st,0,bases,sizes,256));st->eval.site[1].desc_unknown=false;
    p->sites[1].kind=SH_SITE_IMAGE;REQUIRE(!local_compute_read_safe(r,st,0,bases,sizes,256));
    p->sites[1].kind=SH_SITE_BUFFER;st->eval.site[0].valid=false;
    REQUIRE(!local_compute_read_safe(r,st,0,bases,sizes,256));
    free(p);free(st);free(r);pal_mutex_destroy(mem.lock);
    puts("local compute inputs: disjoint spans accepted, virtual/physical overlaps and uncertain writes rejected");
    return 0;
}

static int writeback_scope_test(renderer *r,guest_addr a) {
    static const uint32_t code[]={0xd7460000,0x04010c0c,0x7e020208,0xe0702000,0x80010200,0xe0702004,0x80010200,0xbf810000};
    sh_program *p=calloc(1,sizeof(*p));stage_state *st=calloc(1,sizeof(*st));REQUIRE(p && st);
    REQUIRE(sh_analyze(code,PS5C_ARRAY_COUNT(code),a,p));st->prog=p;
    uint32_t (*values)[3]=calloc(SH_MAX_INSTS,sizeof(*values));uint8_t (*known)[3]=calloc(SH_MAX_INSTS,sizeof(*known));REQUIRE(values && known);
    st->eval.vsrc=values;st->eval.vsrc_state=known;st->eval.sgpr_init[4]=(uint32_t)(a+0x4000);
    st->eval.sgpr_init[5]=(uint32_t)((a+0x4000)>>32)|(32u<<16);st->eval.sgpr_init[6]=1024;st->eval.sgpr_init[7]=0x16204;st->eval.sgpr_init[8]=17;
    st->eval.group_sgpr=12;st->eval.group_sgprs=1;REQUIRE(sh_evaluate(p,r->mem,&st->eval));
    st->info.stage=SH_STAGE_CS;st->info.local_size[0]=64;st->info.local_size[1]=st->info.local_size[2]=1;
    st->info.cs_tid_components=1;st->info.cs_tgid_sgpr=12;st->info.cs_tgid_enable=1;
    st->groups[0]=2;st->groups[1]=st->groups[2]=1;
    st->footprint_hash_valid=eval_source_hash(r,&st->footprint_reads,&st->footprint_hash);REQUIRE(st->footprint_hash_valid);
    REQUIRE(footprint_linear(st));footprint_build(r,st);
    REQUIRE(st->footprint_count==1 && st->footprint[0].base==a+0x4000 && st->footprint[0].lo==0 && st->footprint[0].hi==127*32+8);
    uint32_t store=p->sites[p->site_count-1].inst;uint16_t old_offset=p->insts[store].soffset;
    p->insts[store].soffset=12;REQUIRE(!footprint_linear(st));p->insts[store].soffset=old_offset;
    uint16_t saved_flags=p->iflags[1];p->iflags[1]|=SH_IF_FLOW;
    REQUIRE(!footprint_linear(st));p->iflags[1]=saved_flags;
    rdna2_inst saved=p->insts[1];p->insts[1].enc=RDNA2_ENC_SMEM;p->insts[1].name="s_load_dword";p->insts[1].sbase=12;p->insts[1].soffset=RDNA2_OP_NULL;
    REQUIRE(!footprint_linear(st));p->insts[1]=saved;
    st->footprint_reads.count=1;st->footprint_reads.start[0]=a+0x4004;st->footprint_reads.end[0]=a+0x4008;
    REQUIRE(eval_source_hash(r,&st->footprint_reads,&st->footprint_hash));footprint_build(r,st);REQUIRE(st->footprint_count==0);
    st->footprint_reads.start[0]=a+0x8000;st->footprint_reads.end[0]=a+0x8004;
    REQUIRE(eval_source_hash(r,&st->footprint_reads,&st->footprint_hash));footprint_build(r,st);REQUIRE(st->footprint_count==1);
    uint8_t *byte=guest_host(r,a+0x8000,1);REQUIRE(byte);*byte^=1;footprint_build(r,st);REQUIRE(st->footprint_count==0);*byte^=1;
    writeback w={.addr=a,.size=4096,.scope_mode=1,.scope_lo=0,.scope_hi=4};
    REQUIRE(wb_pending(&w,a,4) && !wb_pending(&w,a+64,4));
    wb_scope_add(&w,a,64,68,true);REQUIRE(w.scope_hi==68 && wb_pending(&w,a+64,4) && !wb_pending(&w,a+100,4));
    w.scope_mode=2;REQUIRE(wb_pending(&w,a+100,4));w.scope_mode=1;
    wb_scope_add(&w,a,0,0,false);REQUIRE(!w.scope_mode && wb_pending(&w,a+100,4));
    wb_scope_add(&w,a,128,132,true);REQUIRE(!w.scope_mode);
    REQUIRE(wb_pending(&w,UINT64_MAX-1,8));
    static const uint32_t restore[]={0xd7460000,0x04010c0c,0x7e020208,
        0xbeea047e,0x7da40080,0xbf880001,0x7e0a0280,0xbefe046a,
        0x7e000501,0xe0702010,0x80010200,0xbf810000};
    REQUIRE(sh_analyze(restore,PS5C_ARRAY_COUNT(restore),a,p));
    REQUIRE(sh_evaluate(p,r->mem,&st->eval));st->footprint_reads.count=0;
    REQUIRE(eval_source_hash(r,&st->footprint_reads,&st->footprint_hash));
    st->eval.site[p->site_count-1].records_unknown=true;
    footprint_build(r,st);REQUIRE(st->footprint_count==1 && st->footprint[0].lo==16 && st->footprint[0].hi==127*32+20);
    sh_eval_free(&st->eval);free(values);free(known);free(st);free(p);
    puts("writeback footprints: multi-store union, TGID guards, mutable scalar sources, changed CPU bytes and conservative broadening passed");return 0;
}

int main(void){
    REQUIRE(cached_capability_test()==0);
    REQUIRE(pal_init()==PS5C_OK);REQUIRE(local_compute_input_test()==0);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    REQUIRE(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    REQUIRE(gmem_map(&m,0,16*1024*1024,GPROT_RW,0,"audit fixtures",&a)==PS5C_OK);
    uint8_t *bytes=gmem_host(&m,a,16*1024*1024);REQUIRE(bytes);
    for(unsigned i=0;i<16*1024*1024;i++)bytes[i]=(uint8_t)((i*37u)^(i>>8)^(i>>17));
    renderer *r=calloc(1,sizeof(*r));REQUIRE(r);r->pipes=calloc(1,sizeof(*r->pipes));REQUIRE(r->pipes);r->mem=&m;
    REQUIRE(vk_guest_access_begin(r));
    REQUIRE(writeback_scope_test(r,a)==0);int texture=texture_test(r,a),context=context_test(r,a);
    REQUIRE(sparse_copy_test()==0 && small_texture_write_test(r,a)==0 && source_test(r,a)==0 && vector_header_test(r,a)==0 && site_header_test(r)==0 && relocate_test(r,a)==0);
    printf("texture=%d context_regression=%d\n",texture,context);
    vk_guest_access_end(r);
    if(r->pipes->evals){
        for(unsigned i=0;i<EVAL_CACHE;i++)free(r->pipes->evals[i].smem_data);
        free(r->pipes->evals);
    }
    free(r->pipes);free(r);REQUIRE(compressed_alias_gpu_test(&m,a+2*1024*1024)==0);REQUIRE(packed_volume_gpu_test(&m,a+4*1024*1024)==0);gmem_free(&m);pal_shutdown();
    return texture?2:context?1:0;
}
