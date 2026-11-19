/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real renderer regression: a mapped mip tail, an unmapped mip0, fractional
 * resource MIN_LOD, live residency changes, BASE_LEVEL and guest resinfo. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/gpu_regs.h"
#include "gpu/renderer.h"
#include "gpu/vk/vk_internal.h"
#include "gpu/vk/vk_texture_tiling.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)

static const uint32_t sample_l[]={
    0x7e0002f0,0x7e0202f0,0x7e040210, /* x=y=.5, LOD=s16 */
    0xf0900f08,0x00400400,0xbf8c3f70,
    0xe0780000,0x80030400,0xbf810000};
static const uint32_t sample_lz[]={
    0x7e0002f0,0x7e0202f0,
    0xf09c0f08,0x00400400,0xbf8c3f70,
    0xe0780000,0x80030400,0xbf810000};
static const uint32_t resinfo[]={
    0x7e000280,0xf0380f08,0x00000400,0xbf8c3f70,
    0xe0780000,0x80030400,0xbf810000};
static const uint32_t store_mip[]={
    0x7e0002ff,16,0x7e0202ff,16,0x7e040210, /* texel (16,16), level=s16 */
    0x7e0802ff,0x3f000000, /* R=.5 */
    0xf0240108,0x00000400,0xbf810000};

static bool fill_level(gmem *m,guest_addr image,unsigned level,float value) {
    uint32_t w=64u>>level;
    uint64_t off=0;
    for(unsigned lv=7;lv-- > level+1;)off+=(64u>>lv)*256u;
    for(unsigned y=0;y<w;y++)for(unsigned x=0;x<w;x++)
        if(!gmem_write(m,image+off+y*256+x*4,&value,4))return false;
    return true;
}
static void dispatch(renderer *r,gpu *g,guest_addr code,guest_addr output) {
    g->regs[mmCOMPUTE_PGM_LO]=(uint32_t)(code>>8);
    g->regs[mmCOMPUTE_PGM_HI]=(uint32_t)(code>>40);
    renderer_submit_begin(r);renderer_dispatch(r,g,1,1,1);
    renderer_sync_mem(r,output,16);
}
static bool bc_fill_level(gmem *m,guest_addr image,unsigned lv,uint64_t texel) {
    mip_place mp;mip_locate(9,4096,4096,4,8,13,lv,&mp);
    uint32_t w=((4096u>>lv)+3)/4,bw,bh,bytes;std_block(9,8,&bw,&bh,&bytes);
    uint32_t pitch=(w+mp.ox+bw-1)/bw;
    for(unsigned y=0;y<w;y++)for(unsigned x=0;x<w;x++) {
        uint32_t gx=x+mp.ox,gy=y+mp.oy;
        uint64_t off=((uint64_t)(gy/bh)*pitch+gx/bw)*bytes+std_offset(9,gx,gy,8);
        void *p=gmem_host(m,image+mp.offset+off,8);if(!p)return false;
        memcpy(p,&texel,8);
    }
    return true;
}
static int bc_cases(renderer *r,gpu *g,gmem *m,guest_addr a,float *output) {
    guest_addr image,mapped;
    CHECK(gmem_map(m,0,16*1024*1024,GPROT_RW,GMAP_RESERVE,"BC1 virtual resource",&image)==PS5C_OK);
    CHECK(gmem_map(m,image,65536,GPROT_RW,GMAP_FIXED|GMAP_REPLACE,"BC1 mip tail",&mapped)==PS5C_OK);
    uint8_t *tail=gmem_host(m,image,65536);CHECK(tail);
    memset(tail,0,65536);
    for(unsigned lv=5;lv<=12;lv++) {
        mip_place mp;mip_locate(9,4096,4096,4,8,13,lv,&mp);
        CHECK(mp.offset==0 && mp.tail);
        uint32_t w=((4096u>>lv)+3)/4;
        uint64_t texel=lv==5?0x07e007e0ull:lv==6?0xf800f800ull:0x001f001full;
        uint32_t bw,bh,bytes;std_block(9,8,&bw,&bh,&bytes);
        uint32_t pitch=(w+mp.ox+bw-1)/bw;
        for(unsigned y=0;y<w;y++)for(unsigned x=0;x<w;x++) {
            uint32_t gx=x+mp.ox,gy=y+mp.oy;
            uint64_t off=((uint64_t)(gy/bh)*pitch+gx/bw)*bytes+std_offset(9,gx,gy,8);
            CHECK(off+8<=65536);memcpy(tail+off,&texel,8);
        }
    }
    uint32_t *u=g->regs+mmCOMPUTE_USER_DATA_0;
    u[0]=(uint32_t)(image>>8);u[1]=(uint32_t)(image>>40)|(169u<<20)|(3u<<30)|(1280u<<8);
    u[2]=1023u|(4095u<<14);u[3]=(9u<<28)|(9u<<20)|(12u<<16)|0xfac;
    dispatch(r,g,a,a+4096);CHECK(output[0]==0 && output[1]==1 && output[2]==0);
    u[1]=(u[1]&~0xfff00u)|(1408u<<8);
    dispatch(r,g,a,a+4096);
    if(fabsf(output[0]-.5f)>1.f/255 || fabsf(output[1]-.5f)>1.f/255 || output[2]!=0)
        fprintf(stderr,"BC1 fractional result: %g %g %g\n",output[0],output[1],output[2]);
    CHECK(fabsf(output[0]-.5f)<=1.f/255 && fabsf(output[1]-.5f)<=1.f/255 && output[2]==0);
    dispatch(r,g,a+512,a+4096);
    const uint32_t *dims=(const uint32_t*)output;
    CHECK(dims[0]==4096 && dims[1]==4096 && dims[3]==13);
    if(getenv("PS5C_TEST_REQUIRE_SPARSE")) {
        CHECK(r->sparse_mips);
        vk_image *chain=NULL;
        for(uint32_t i=0;i<r->image_count;i++)if(r->images[i].addr==image && r->images[i].levels==13 &&
            r->images[i].sparse_first==5)chain=&r->images[i];
        CHECK(chain && chain->allocated_bytes && chain->allocated_bytes<=512u*1024);
        printf("sparse BC1: %llu physical bytes for4096x4096, original dimensions preserved\n",
               (unsigned long long)chain->allocated_bytes);
        /* Stream a finer mip into guest memory. An older image with only
         * mip5 must never be returned for a request that can sample mip4. */
        mip_place mp;mip_locate(9,4096,4096,4,8,13,4,&mp);
        uint64_t tile=mp.offset&~UINT64_C(65535);
        if(!gmem_host(m,image+tile,65536))
            CHECK(gmem_map(m,image+tile,65536,GPROT_RW,GMAP_FIXED|GMAP_REPLACE,"BC1 finer mip",&mapped)==PS5C_OK);
        CHECK(bc_fill_level(m,image,4,UINT64_C(0xffe0ffe0)));
        u[1]=(u[1]&~0xfff00u)|(1024u<<8);
        dispatch(r,g,a,a+4096);CHECK(output[0]==1 && output[1]==1 && output[2]==0);
        vk_image *finer=NULL;
        for(uint32_t i=0;i<r->image_count;i++)if(r->images[i].addr==image && r->images[i].levels==13 &&
            r->images[i].sparse_first==4)finer=&r->images[i];
        CHECK(finer && finer!=chain && finer->allocated_bytes<=512u*1024);
        CHECK(bc_fill_level(m,image,5,UINT64_C(0xf800f800)));
        u[1]=(u[1]&~0xfff00u)|(1280u<<8);
        dispatch(r,g,a,a+4096);CHECK(output[0]==1 && output[1]==0 && output[2]==0);
        u[1]=(u[1]&~0xfff00u)|(1152u<<8);
        dispatch(r,g,a,a+4096);
        CHECK(output[0]==1 && fabsf(output[1]-.5f)<=1.f/255 && output[2]==0);
        /* Mip3 is outside BC1's Vulkan mip tail: exercise tiled binds too. */
        mip_locate(9,4096,4096,4,8,13,3,&mp);tile=mp.offset&~UINT64_C(65535);
        uint64_t bytes=surface_bytes(9,128+mp.ox,128+mp.oy,8);
        uint64_t span=PS5C_ALIGN_UP(mp.offset-tile+bytes,UINT64_C(65536));
        CHECK(gmem_map(m,image+tile,span,GPROT_RW,GMAP_FIXED|GMAP_REPLACE,"BC1 non-tail mip",&mapped)==PS5C_OK);
        CHECK(bc_fill_level(m,image,3,UINT64_C(0x001f001f)));
        u[1]=(u[1]&~0xfff00u)|(768u<<8);
        dispatch(r,g,a,a+4096);CHECK(output[0]==0 && output[1]==0 && output[2]==1);
        vk_image *tiles=NULL;
        for(uint32_t i=0;i<r->image_count;i++)if(r->images[i].addr==image && r->images[i].levels==13 &&
            r->images[i].sparse_first==3)tiles=&r->images[i];
        CHECK(tiles && tiles->allocated_bytes>65536 && tiles->allocated_bytes<=512u*1024);
        u[1]=(u[1]&~0xfff00u)|(896u<<8);
        dispatch(r,g,a,a+4096);
        CHECK(fabsf(output[0]-.5f)<=1.f/255 && fabsf(output[1]-.5f)<=1.f/255 && fabsf(output[2]-.5f)<=1.f/255);
        /* Ordinary callers require fully resident memory, regardless of
         * which partial chain was used by the last sampling descriptor. */
        vk_image *dense=vk_image_get_levels(r,image,4096,4096,VK_FORMAT_BC1_RGBA_UNORM_BLOCK,1,13);
        CHECK(dense && dense!=chain && dense!=finer && !dense->sparse_first);
        puts("sparse BC1: mip residency growth, CPU edits and dense lookup isolation verified");
    }
    puts("resident mip BC1 GPU: 4096x4096 SW64KB_S, only 64KiB mapped, fractional clamp and original dimensions verified");
    return 0;
}
static void bc7_gray(uint8_t block[16],unsigned value) {
    memset(block,0,16);block[0]=1u<<6; /* BC7 mode6: eight7-bit endpoints and two shared P bits */
    unsigned at=7;
    for(unsigned endpoint=0;endpoint<8;endpoint++)for(unsigned bit=0;bit<7;bit++,at++)
        block[at>>3]|=(uint8_t)((((value>>1)>>bit)&1u)<<(at&7));
    for(unsigned i=0;i<2;i++,at++)block[at>>3]|=(uint8_t)((value&1u)<<(at&7));
}
static bool bc128_level(gmem *m,guest_addr image,unsigned lv,const uint8_t block[16]) {
    mip_place mp;mip_locate(9,4096,4096,4,16,13,lv,&mp);
    uint32_t w=((4096u>>lv)+3)/4,bw,bh,bytes;std_block(9,16,&bw,&bh,&bytes);
    uint32_t pitch=(w+mp.ox+bw-1)/bw;
    for(unsigned y=0;y<w;y++)for(unsigned x=0;x<w;x++) {
        uint32_t gx=x+mp.ox,gy=y+mp.oy;
        uint64_t off=((uint64_t)(gy/bh)*pitch+gx/bw)*bytes+std_offset(9,gx,gy,16);
        void *p=gmem_host(m,image+mp.offset+off,16);if(!p)return false;memcpy(p,block,16);
    }
    return true;
}
static int bc128_cases(renderer *r,gpu *g,gmem *m,guest_addr a,float *output) {
    for(unsigned format=0;format<2;format++) {
        guest_addr image,mapped;
        CHECK(gmem_map(m,0,32u<<20,GPROT_RW,GMAP_RESERVE,"BC128 virtual resource",&image)==PS5C_OK);
        CHECK(gmem_map(m,image,256u<<10,GPROT_RW,GMAP_FIXED|GMAP_REPLACE,"BC128 resident tail",&mapped)==PS5C_OK);
        CHECK(!gmem_host(m,image,16u<<20));
        uint8_t low[16]={64,64,0,0,0,0,0,0,192,192},high[16]={192,192,0,0,0,0,0,0,64,64};
        if(format){bc7_gray(low,255);bc7_gray(high,128);}
        for(unsigned lv=4;lv<=12;lv++)CHECK(bc128_level(m,image,lv,lv==4?low:high));
        uint32_t *u=g->regs+mmCOMPUTE_USER_DATA_0;
        u[0]=(uint32_t)(image>>8);u[1]=(uint32_t)(image>>40)|((format?181u:177u)<<20)|(3u<<30)|(1024u<<8);
        u[2]=1023u|(4095u<<14);u[3]=(9u<<28)|(9u<<20)|(12u<<16)|0xfac;
        u[16]=0;dispatch(r,g,a,a+4096);
        float expected[4]={format?1.f:64.f/255,format?1.f:192.f/255,format?1.f:0.f,1};
        for(unsigned k=0;k<4;k++)CHECK(fabsf(output[k]-expected[k])<=1.f/255);
        u[1]=(u[1]&~0xfff00u)|(1152u<<8);dispatch(r,g,a,a+4096);
        for(unsigned k=0;k<4;k++) {
            float half=format?191.5f/255:k<2?128.f/255:k==2?0.f:1.f;
            CHECK(fabsf(output[k]-half)<=1.f/255);
        }
        if(getenv("PS5C_TEST_REQUIRE_SPARSE")) {
            vk_image *chain=NULL;
            for(uint32_t i=0;i<r->image_count;i++)if(r->images[i].addr==image && r->images[i].levels==13 &&
                r->images[i].sparse_first==4)chain=&r->images[i];
            CHECK(chain && chain->allocated_bytes>0 && chain->allocated_bytes<=512u*1024);
            printf("sparse BC%u: %llu physical bytes for4096x4096\n",format?7:5,(unsigned long long)chain->allocated_bytes);
        }
        CHECK(bc128_level(m,image,4,high));u[1]=(u[1]&~0xfff00u)|(1024u<<8);
        dispatch(r,g,a,a+4096);
        expected[0]=format?128.f/255:192.f/255;expected[1]=format?128.f/255:64.f/255;
        expected[2]=format?128.f/255:0;expected[3]=format?128.f/255:1;
        for(unsigned k=0;k<4;k++)CHECK(fabsf(output[k]-expected[k])<=1.f/255);
        dispatch(r,g,a+512,a+4096);const uint32_t *dims=(const uint32_t*)output;
        CHECK(dims[0]==4096 && dims[1]==4096 && dims[3]==13);
    }
    puts("resident BC5/BC7: fractional clamp, original dimensions and CPU rewrites verified");return 0;
}
static int run(void) {
    int test_result=0;
    CHECK(pal_init()==PS5C_OK);
    gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr a,image;
    CHECK(gmem_map(&m,0,GUEST_PAGE_SIZE,GPROT_RW,0,"resident mip code",&a)==PS5C_OK);
    CHECK(gmem_map(&m,0,2*GUEST_PAGE_SIZE,GPROT_RW,GMAP_RESERVE,"resident mip resource",&image)==PS5C_OK);
    guest_addr mapped;
    CHECK(gmem_map(&m,image,GUEST_PAGE_SIZE,GPROT_RW,GMAP_FIXED|GMAP_REPLACE,"resident mip tail",&mapped)==PS5C_OK);
    CHECK(gmem_write(&m,a,sample_l,sizeof(sample_l)) && gmem_write(&m,a+256,sample_lz,sizeof(sample_lz)));
    CHECK(gmem_write(&m,a+512,resinfo,sizeof(resinfo)));
    CHECK(gmem_write(&m,a+768,store_mip,sizeof(store_mip)));
    CHECK(!gmem_host(&m,image,32512)); /* mip0 extends beyond the mapped tail */
    for(unsigned lv=1;lv<7;lv++)CHECK(fill_level(&m,image,lv,(float)lv/8));
    float *output=gmem_host(&m,a+4096,16);CHECK(output);
    pal_window *window=NULL;
    CHECK(pal_window_open("ZeraPS5 resident mip regression",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);
    gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_PGM_RSRC2]=20u<<1;
    g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    u[0]=(uint32_t)(image>>8);u[1]=(uint32_t)(image>>40)|(22u<<20)|(3u<<30)|(256u<<8);
    u[2]=15u|(63u<<14);u[3]=(9u<<28)|(6u<<16)|0x924;
    u[9]=0xfffu<<12;u[10]=2u<<26; /* linear mip interpolation */
    u[12]=(uint32_t)(a+4096);u[13]=(uint32_t)((a+4096)>>32);u[14]=16;u[15]=0x16204;
    const unsigned clamps[]={256,384,512,768,256};
    for(unsigned k=0;k<PS5C_ARRAY_COUNT(clamps);k++) {
        u[1]=(u[1]&~0xfff00u)|(clamps[k]<<8);
        for(unsigned lz=0;lz<2;lz++) {
            memset(output,0xcc,16);dispatch(r,&g,a+lz*256,a+4096);
            float expected=(float)clamps[k]/2048;
            if(fabsf(output[0]-expected)>1e-6f)fprintf(stderr,"clamp %g lz %u: %g expected %g\n",clamps[k]/256.,lz,output[0],expected);
            CHECK(fabsf(output[0]-expected)<1e-6f);
        }
    }
    float lod=2.25f;memcpy(&u[16],&lod,4);dispatch(r,&g,a,a+4096);
    CHECK(fabsf(output[0]-.28125f)<1e-6f);
    u[16]=0;
    dispatch(r,&g,a+512,a+4096);
    const uint32_t *dims=(const uint32_t*)output;
    CHECK(dims[0]==64 && dims[1]==64 && dims[3]==7);
    u[3]|=1u<<12;
    dispatch(r,&g,a,a+4096);CHECK(output[0]==.125f);
    dispatch(r,&g,a+512,a+4096);CHECK(dims[0]==32 && dims[1]==32 && dims[3]==6);
    CHECK(fill_level(&m,image,1,.75f));
    dispatch(r,&g,a,a+4096);CHECK(output[0]==.75f);
    u[1]=(u[1]&~0xfff00u)|(384u<<8);
    dispatch(r,&g,a,a+4096);CHECK(output[0]==.5f);
    /* Make the full resource resident without replacing its original address. */
    guest_addr extra;CHECK(gmem_map(&m,image+GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,GPROT_RW,GMAP_FIXED|GMAP_REPLACE,"mip0",&extra)==PS5C_OK);
    CHECK(extra==image+GUEST_PAGE_SIZE && fill_level(&m,image,0,.875f));
    u[1]&=~0xfff00u;u[3]&=~0xf000u;
    dispatch(r,&g,a,a+4096);CHECK(output[0]==.875f);
    if(getenv("PS5C_TEST_SAMPLER_MIP_MODE")) {
        /* All levels are resident and distinct. MIP_FILTER=0 disables mip
         * selection, whereas point mipmapping must honor explicit LOD=2.
         * Revisit both states to expose an incorrectly shared VkSampler. */
        uint32_t saved_sampler=u[10],saved_lod=u[16];
        const unsigned modes[]={1,0,1,0};
        float explicit_lod=2;memcpy(&u[16],&explicit_lod,4);
        for(unsigned k=0;k<PS5C_ARRAY_COUNT(modes);k++) {
            u[10]=(saved_sampler&~(3u<<26))|(modes[k]<<26);
            dispatch(r,&g,a,a+4096);
            float expected=modes[k]?.25f:.875f;
            printf("sampler MIP_FILTER %u LOD 2: %g expected %g\n",modes[k],output[0],expected);
            if(output[0]!=expected){test_result=1;goto cleanup;}
        }
        /* The base level is relative to the selected resource view. */
        u[3]|=1u<<12;
        dispatch(r,&g,a,a+4096);
        if(output[0]!=.75f){test_result=1;goto cleanup;}
        u[3]&=~0xf000u;
        /* A maxLod=0 shortcut would incorrectly select MAG even for a
         * minifying sample. Straddle the boundary between texels31/32. */
        uint64_t base_offset=0;
        for(unsigned lv=1;lv<7;lv++)base_offset+=(64u>>lv)*256u;
        for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++) {
            float value=x>=32?1.f:0.f;
            CHECK(gmem_write(&m,image+base_offset+y*256+x*4,&value,4));
        }
        const unsigned filters[]={1u<<22,1u<<20}; /* MINlinear/MAGpoint, inverse */
        for(unsigned k=0;k<2;k++)for(unsigned minify=0;minify<2;minify++) {
            u[10]=filters[k];float level=minify?2.f:0.f;memcpy(&u[16],&level,4);
            memset(output,0xcc,16);dispatch(r,&g,a,a+4096);
            float expected=(minify!=(k!=0))?.5f:1.f;
            printf("mip disabled spatial filter %u LOD %g: %g expected %g\n",k,level,output[0],expected);
            if(output[0]!=expected){test_result=1;goto cleanup;}
        }
        CHECK(fill_level(&m,image,0,.875f));
        u[10]=saved_sampler;u[16]=saved_lod;
        puts("sampler: disabled/point mipmapping and nonzero BASE_LEVEL verified");
    }
    /* The runtime comparison changes the content-hash algorithm, not the
     * resource. Each mode must observe CPU edits in the same image slot. */
    const char *hash_file=getenv("PS5C_TEST_RESIDENT_HASH_FILE");
    if(hash_file) {
        for(unsigned k=0;k<4;k++) {
            FILE *f=fopen(hash_file,"w");CHECK(f && fprintf(f,"%u\n",k&1)>0 && !fclose(f));
            g.stats.flips++;
            float value=.375f+.125f*(k&1);
            CHECK(fill_level(&m,image,0,value));
            dispatch(r,&g,a,a+4096);CHECK(output[0]==value);
        }
        CHECK(fill_level(&m,image,0,.875f));
    }
    /* A real shader write to one mip must take precedence over the earlier
     * CPU-built chain, without replacing the other levels. */
    u[16]=1;dispatch(r,&g,a+768,a+4096);
    lod=1;memcpy(&u[16],&lod,4);dispatch(r,&g,a,a+4096);
    CHECK(output[0]==.5f);
    u[16]=0;dispatch(r,&g,a,a+4096);CHECK(output[0]==.875f);
    CHECK(bc_cases(r,&g,&m,a,output)==0);
    CHECK(bc128_cases(r,&g,&m,a,output)==0);
cleanup:
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);
    pal_window_destroy(window);gmem_free(&m);pal_shutdown();
    if(test_result)return test_result;
    puts("resident mip GPU: unmapped mip0, fractional clamp, base level, resinfo and CPU edits verified");
    return 0;
}
int main(void) {
    if(getenv("PS5C_TEST_GPU"))return run();
    puts("resident mip GPU test skipped (PS5C_TEST_GPU=1, PS5C_RESIDENT_MIPS=1)");return 0;
}
