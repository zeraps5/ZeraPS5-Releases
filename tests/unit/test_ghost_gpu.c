/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/shader/shader.h"
#include "gpu/shader/spv.h"
#include "gpu/vk/vk_internal.h"
#include "gpu/vk/vk_compute_shape.h"
#include "gpu/gpu_regs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static renderer *cleanup_renderer;
static pal_window *cleanup_window;
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);if(cleanup_renderer)renderer_destroy(cleanup_renderer);if(cleanup_window)pal_window_destroy(cleanup_window);return 1;}}while(0)
static uint32_t bits(float f){uint32_t u;memcpy(&u,&f,4);return u;}
static float real(uint32_t u){float f;memcpy(&f,&u,4);return f;}
static const uint32_t group_code[]={
    0xd8d4041f,0x03000200, /* v3 = v2 of lane ^ 1; crossbar, no LDS memory */
    0xe0782000,0x80000002,0xbf810000 /* store v0:3 indexed by guest local Z */
};
static const uint32_t gds_add_code[]={0x7e000201,0x7e020200,0xbefc0302,
    0xd8020004,0x00000100,0xbf810000}; /* GDS[M0.base + v0 + 4] += v1 */
static int cpu_test(void){
    VkPhysicalDeviceLimits limits={.maxComputeWorkGroupInvocations=1024,.maxComputeWorkGroupSize={1024,1024,64}};
    uint32_t host[3];CHECK(vk_compute_shape((uint32_t[]){1,1,256},&limits,host));
    CHECK(host[0]==256 && host[1]==1 && host[2]==1);
    CHECK(!vk_compute_shape((uint32_t[]){1,1,1025},&limits,host));
    CHECK(!vk_compute_shape((uint32_t[]){0,1,1},&limits,host));
    CHECK(vk_compute_shape((uint32_t[]){8,8,1},&limits,host) && !host[0]);
    sh_program *p=calloc(1,sizeof(*p));CHECK(p && sh_analyze(group_code,5,0,p));
    for(unsigned i=0;i<p->count;i++) CHECK(!sh_inst_uses_lds(&p->insts[i]));
    sh_stage_info info={.stage=SH_STAGE_CS,.local_size={1,1,256},.host_local_size={256,1,1},.cs_tid_components=3};
    sh_site_shape shapes[SH_MAX_SITES]={0};uint32_t *spv;size_t words;
    CHECK(sh_translate(p,&info,shapes,0,&spv,&words));
    for(size_t i=5;i<words;i+=spv[i]>>16){
        CHECK(spv[i]>>16);
        if((spv[i]&65535)==SpvOpVariable) CHECK(spv[i+3]!=SpvStorageClassWorkgroup);
    }
    if(getenv("PS5C_TEST_SPIRV")){
        char path[512];snprintf(path,sizeof(path),"%s/ghost_group.spv",getenv("PS5C_TEST_SPIRV"));
        FILE *f=fopen(path,"wb");CHECK(f && fwrite(spv,4,words,f)==words && !fclose(f));
    }
    free(spv);free(p);puts("group shape and crossbar without spurious 64 KiB LDS: ok");return 0;
}
static void pc(gpu *g,guest_addr a){g->regs[mmCOMPUTE_PGM_LO]=(uint32_t)(a>>8);g->regs[mmCOMPUTE_PGM_HI]=(uint32_t)(a>>40);}
static void image_desc(uint32_t *u,guest_addr a,unsigned format,unsigned width){
    u[0]=(uint32_t)(a>>8);u[1]=(uint32_t)(a>>40)|(format<<20)|(((width-1)&3)<<30);
    u[2]=(width-1)>>2;u[3]=(9u<<28)|0xfac;
}
static int gpu_test(void){
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"Ghost GPU fixtures",&a)==PS5C_OK);
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 Ghost GPU regression",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);
    cleanup_renderer=r;cleanup_window=window;
    gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    guest_addr out=a+0x4000,imgaddr=a+0x8000;
    uint32_t *output=gmem_host(&m,out,4096);CHECK(output);
    u[0]=(uint32_t)out;u[1]=(uint32_t)(out>>32)|(16u<<16);u[2]=256;u[3]=0x16204;
    CHECK(gmem_write(&m,a,group_code,sizeof(group_code)));pc(&g,a);
    g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=1;g.regs[mmCOMPUTE_NUM_THREAD_Z]=256;
    g.regs[mmCOMPUTE_PGM_RSRC2]=(16u<<1)|(2u<<11);
    renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,4096);
    for(unsigned i=0;i<256;i++) CHECK(output[4*i]==0 && output[4*i+1]==0 && output[4*i+2]==i && output[4*i+3]==(i^1));
    puts("256 guest Z threads: every XYZ and crossbar result verified on GPU");
    CHECK(gmem_write(&m,a+0x700,gds_add_code,sizeof(gds_add_code)));pc(&g,a+0x700);
    sh_gds_base=a+0x10000;uint32_t *gds=gmem_host(&m,sh_gds_base,SH_GDS_SIZE);CHECK(gds);
    for(unsigned i=0;i<SH_GDS_SIZE/4;i++)gds[i]=0x12345678;
    gds[0x108/4]=7;g.regs[mmCOMPUTE_PGM_RSRC2]=3u<<1;
    g.regs[mmCOMPUTE_NUM_THREAD_X]=64;g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    u[1]=4;u[2]=0x01000020; /* M0={base=0x100,size=0x20}, not base=0x20 */
    for(unsigned pass=0;pass<2;pass++){
        u[0]=pass?5:3;renderer_submit_begin(r);renderer_dispatch(r,&g,2,1,1);
        renderer_sync_mem(r,sh_gds_base,SH_GDS_SIZE);
        CHECK(gds[0x108/4]==7+128*(pass?8:3));
        for(unsigned i=0;i<SH_GDS_SIZE/4;i++)if(i!=0x108/4)CHECK(gds[i]==0x12345678);
    }
    /* Equal-size GDS allocations have independent bases. Size bits must not
     * alias the physical counters that the CP resets before another frame. */
    gds[0x208/4]=19;u[2]=0x02000020;u[0]=7;
    renderer_submit_begin(r);renderer_dispatch(r,&g,2,1,1);
    renderer_sync_mem(r,sh_gds_base,SH_GDS_SIZE);
    CHECK(gds[0x108/4]==1031 && gds[0x208/4]==19+128*7);
    for(unsigned i=0;i<SH_GDS_SIZE/4;i++)if(i!=0x108/4 && i!=0x208/4)CHECK(gds[i]==0x12345678);
    gds[0x208/4]=0x12345678;u[2]=0x01000020;
    /* Unaligned/out-of-range addresses must not corrupt neighboring memory. */
    for(unsigned pass=0;pass<2;pass++){
        u[1]=pass?SH_GDS_SIZE:5;renderer_submit_begin(r);renderer_dispatch(r,&g,2,1,1);
        renderer_sync_mem(r,sh_gds_base,SH_GDS_SIZE);CHECK(gds[0x108/4]==1031);
        for(unsigned i=0;i<SH_GDS_SIZE/4;i++)if(i!=0x108/4)CHECK(gds[i]==0x12345678);
    }
    /* Every lane reserves one slot and reads the same device counter through
     * a separate readonly GDS binding. Returned values must be a permutation. */
    const uint32_t gds_return[]={0x7e060300,0x7e000201,0x7e020200,0xbefc0302,
        0xd8da0004,0x06000000,0xd8820004,0x04000100,0xd8da0004,0x05000000,
        0x7e0e0280,0xe0782000,0x80010403,0xbf810000};
    CHECK(gmem_write(&m,a+0x900,gds_return,sizeof(gds_return)));pc(&g,a+0x900);
    g.regs[mmCOMPUTE_NUM_THREAD_X]=128;g.regs[mmCOMPUTE_PGM_RSRC2]=8u<<1;
    u[0]=1;u[2]=0x01000020;u[4]=(uint32_t)out;u[5]=(uint32_t)(out>>32)|(16u<<16);u[6]=128;u[7]=0x16204;
    gds[0x108/4]=17;
    for(unsigned pass=0;pass<3;pass++) {
        u[1]=pass==0?4:pass==1?5:SH_GDS_SIZE;
        memset(output,0xcd,2048);renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,2048);
        renderer_sync_mem(r,sh_gds_base,SH_GDS_SIZE);
        bool seen[128]={0};
        for(unsigned i=0;i<128;i++) {
            uint32_t old=output[4*i],now=output[4*i+1],before=output[4*i+2];
            if(!pass) {CHECK(old>=17 && old<145 && !seen[old-17]);seen[old-17]=true;
                if(now<old+1 || now>145)fprintf(stderr,"GDS lane %u old %u now %u counter %u\n",i,old,now,gds[0x108/4]);
                CHECK(now>=old+1 && now<=145 && before>=17 && before<=old);}
            else CHECK(old==0 && now==0 && before==0);
            CHECK(output[4*i+3]==0);
        }
        CHECK(gds[0x108/4]==145);
        for(unsigned i=0;i<SH_GDS_SIZE/4;i++)if(i!=0x108/4)CHECK(gds[i]==0x12345678);
    }
    puts("GDS returning add/read: 128 unique old values, coherent counter reads, OOB/alignment and untouched neighbors verified");
    const char *wave_test=getenv("PS5C_TEST_GDS_WAVE");
    if(wave_test) {
        bool wave32=atoi(wave_test)==32;
        g.regs[mmCOMPUTE_PGM_RSRC2]=5u<<1;
        g.regs[mmCOMPUTE_DISPATCH_INITIATOR]=wave32?0x8041:0x41;
        for(unsigned consume=0;consume<2;consume++) {
            /* A wave reserves popcount(EXEC) slots once and broadcasts the
             * pre-operation counter. The caller supplies its own lane rank.
             * Only upper-half lanes are active in the paired64 fixture. */
            const uint32_t code[]={0x7e060300,0xbefc0304,
                0x7e0802ff,0xdeadbeef,0x7e0a0280,0x7e0c0280,0x7e0e0280,
                0xbefe03ff,wave32?0x80000001u:0u,0xbeff03ff,0x80000001u,
                consume?0xd8f60008u:0xd8fa0008u,0x04000000,
                0xbefe04c1,0xe0782000,0x80000403,0xbf810000};
            guest_addr code_addr=a+0xa00+consume*0x100;
            CHECK(gmem_write(&m,code_addr,code,sizeof(code)));pc(&g,code_addr);
            u[0]=(uint32_t)out;u[1]=(uint32_t)(out>>32)|(16u<<16);u[2]=128;u[3]=0x16204;u[4]=0x01000020;
            uint32_t initial=consume?1000:17;gds[0x108/4]=initial;
            for(unsigned repetition=0;repetition<2;repetition++) {
            memset(output,0xcd,2048);renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);
            renderer_sync_mem(r,out,2048);renderer_sync_mem(r,sh_gds_base,SH_GDS_SIZE);
            unsigned wave_size=wave32?32:64,waves=128/wave_size;bool seen[4]={0};
            for(unsigned wave=0;wave<waves;wave++) {
                unsigned first=wave*wave_size+(wave32?0:32),last=wave*wave_size+wave_size-1;
                uint32_t old=output[first*4];
                CHECK(old==output[last*4]);
                uint32_t delta=consume?initial-old:old-initial;
                CHECK(!(delta&1) && delta/2<waves && !seen[delta/2]);seen[delta/2]=true;
                for(unsigned lane=wave*wave_size;lane<(wave+1)*wave_size;lane++)
                    if(lane!=first && lane!=last)CHECK(output[lane*4]==0xdeadbeef);
            }
            CHECK(gds[0x108/4]==(consume?initial-2*waves:initial+2*waves));
            initial=gds[0x108/4];
            }
        }
        puts("GDS wave append/consume: one atomic reservation, sparse EXEC, old-value broadcast and upper-half leader verified");
    }
    sh_gds_base=0;g.regs[mmCOMPUTE_PGM_RSRC2]=(16u<<1)|(2u<<11);
    puts("GDS add: device atomics across 2 groups, M0+address+offset, repeated dispatch and untouched neighbors verified");
    g.regs[mmCOMPUTE_NUM_THREAD_X]=64;g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    g.regs[mmCOMPUTE_PGM_RSRC2]=(16u<<1)|(2u<<11);
    for(unsigned mode=0;mode<2;mode++){
        renderer_submit_begin(r);vk_begin(r);
        vk_image *img=vk_image_get(r,imgaddr,1,1,VK_FORMAT_R32_SFLOAT);CHECK(img);
        VkClearColorValue cv={{mode?5.0f:-3.0f,0,0,0}};VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vk_barrier(r->cmd,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdClearColorImage(r->cmd,img->image,img->layout,&cv,1,&range);
        img->gpu_written=true;img->written_seq=++r->write_seq;
        image_desc(u,imgaddr,22,1);u[8]=bits(mode?-2.0f:2.0f);
        u[12]=(uint32_t)out;u[13]=(uint32_t)(out>>32)|(16u<<16);u[14]=64;u[15]=0x16204;
        const uint32_t atom[]={0x7e040300,0x7e000280,0x7e020280,0x7e080208,
            0xf0002108|((mode?0x1e:0x1f)<<18),0x00000400,
            0xe0782000,0x80030402,0xbf810000};
        CHECK(gmem_write(&m,a+0x100,atom,sizeof(atom)));pc(&g,a+0x100);
        memset(output,0xcd,1024);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,1024);
        unsigned old=0;
        for(unsigned i=0;i<64;i++){
            float v=real(output[4*i]);
            if(v==(mode?5.0f:-3.0f))old++;else CHECK(v==(mode?-2.0f:2.0f));
        }
        CHECK(old==1);
        const uint32_t load[]={0x7e000280,0x7e020280,0xf0000f08,0x00000400,0xe0780000,0x80030400,0xbf810000};
        CHECK(gmem_write(&m,a+0x200,load,sizeof(load)));pc(&g,a+0x200);
        g.regs[mmCOMPUTE_NUM_THREAD_X]=1;
        renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,16);
        CHECK(real(output[0])==(mode?-2.0f:2.0f));g.regs[mmCOMPUTE_NUM_THREAD_X]=64;
    }
    puts("float image min/max: 64 contending lanes, returned old values and sampled result verified");
    /* Packed 3D stores need a 3D R32_UINT alias, never the base float view. */
    const uint32_t volume_store[]={0x7e000280,0x7e020280,0x7e040280,
        0x7e080208,0x7e0a0209,0x7e0c020a,0x7e0e020b,0xf0200f10,0x00000400,0xbf810000};
    const uint32_t volume_load[]={0x7e000280,0x7e020280,0x7e040280,
        0xf0000f10,0x00000400,0xe0780000,0x80030400,0xbf810000};
    CHECK(gmem_write(&m,a+0x400,volume_store,sizeof(volume_store)) && gmem_write(&m,a+0x500,volume_load,sizeof(volume_load)));
    image_desc(u,a+0xc000,36,1);u[3]=(10u<<28)|0xfac;u[4]=1;
    u[8]=bits(0.5f);u[9]=bits(1);u[10]=bits(2);u[11]=bits(1);
    g.regs[mmCOMPUTE_NUM_THREAD_X]=1;pc(&g,a+0x400);
    renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);pc(&g,a+0x500);
    renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,16);
    CHECK(real(output[0])==0.5f && real(output[1])==1 && real(output[2])==2 && real(output[3])==1);
    puts("3D packed float store through integer view and native float load verified");
    const uint32_t load_srgb[]={0xf0000f08,0x00000400,0xe0782000,0x80030400,0xbf810000};
    CHECK(gmem_write(&m,a+0x300,load_srgb,sizeof(load_srgb)));pc(&g,a+0x300);
    g.regs[mmCOMPUTE_NUM_THREAD_X]=3;
    for(unsigned channels=1;channels<=2;channels++){
        guest_addr src=imgaddr+channels*0x1000;
        unsigned char pixels[16]={0};pixels[channels]=128;pixels[2*channels]=255;
        if(channels==2){pixels[1]=255;pixels[3]=128;pixels[5]=0;}
        CHECK(gmem_write(&m,src,pixels,sizeof(pixels)));image_desc(u,src,127+channels,3);
        renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,48);
        for(unsigned i=0;i<3;i++) fprintf(stderr,"sRGB %u pixel %u: %.8g %.8g %.8g %.8g\n",channels,i,
            real(output[4*i]),real(output[4*i+1]),real(output[4*i+2]),real(output[4*i+3]));
        /* Native sRGB conversion may use a table rounded to half precision. */
        CHECK(real(output[0])==0 && fabsf(real(output[4])-0.2158605f)<0.0001f && real(output[8])==1);
        for(unsigned i=0;i<3;i++) CHECK(real(output[4*i+2])==0 && real(output[4*i+3])==1);
        if(channels==1) for(unsigned i=0;i<3;i++) CHECK(real(output[4*i+1])==0);
        else CHECK(real(output[1])==1 && fabsf(real(output[5])-0.2158605f)<0.0001f && real(output[9])==0);
        vk_image *native=vk_image_find(r,src,3,1,channels==1?VK_FORMAT_R8_SRGB:VK_FORMAT_R8G8_SRGB);
        if(native){vk_begin(r);CHECK(vk_image_promote(r,native));renderer_wait_idle(r);}
        /* A GPU-written linear alias must retain bytes when sampled as sRGB.
         * Exercise both expansion and refreshing the private sampled copy. */
        for(unsigned pass=0;pass<2;pass++) {
            vk_begin(r);vk_image *raw=vk_image_get(r,src,3,1,channels==1?VK_FORMAT_R8_UNORM:VK_FORMAT_R8G8_UNORM);CHECK(raw);
            VkClearColorValue cv={{pass?0.25f:0.5f,0,0,1}};VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vk_barrier(r->cmd,raw,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkCmdClearColorImage(r->cmd,raw->image,raw->layout,&cv,1,&range);raw->gpu_written=true;raw->written_seq=++r->write_seq;
            renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,48);
            fprintf(stderr,"sRGB GPU alias %u pass %u: %.8g %.8g %.8g expanded %d\n",channels,pass,
                real(output[0]),real(output[4]),real(output[8]),raw->feedback?raw->feedback->expanded_srgb:0);
            /* UNORM blits and native sRGB tables can quantize by one source
             * code point; compare in encoded space, not float ULPs. */
            for(unsigned i=0;i<3;i++) {
                float linear=real(output[4*i]);
                float encoded=linear<=0.0031308f?12.92f*linear:1.055f*powf(linear,1.0f/2.4f)-0.055f;
                CHECK(fabsf(encoded-(pass?0.25f:0.5f))<1.1f/255.0f);
            }
            CHECK(raw->feedback && raw->feedback->expanded_srgb);
        }
    }
    puts("R8/RG8 sRGB: black, middle, white, default channels and decoded GPU samples verified");
    /* GFX10 integer descriptors must never silently become UNORM. Verify
     * stores and sampled loads, including negative values and four channels. */
    const uint32_t integer_store[]={0x7e000280,0x7e020280,
        0x7e080208,0x7e0a0209,0x7e0c020a,0x7e0e020b,0xf0200f08,0x00000400,0xbf810000};
    const uint32_t integer_load[]={0x7e000280,0x7e020280,0xf0000f08,0x00000400,
        0xe0780000,0x80030400,0xbf810000};
    CHECK(gmem_write(&m,a+0x600,integer_store,sizeof(integer_store)) &&
          gmem_write(&m,a+0x700,integer_load,sizeof(integer_load)));
    const unsigned integer_formats[]={21,61,69,70,75,76};
    for(unsigned f=0;f<sizeof(integer_formats)/sizeof(integer_formats[0]);f++) {
        unsigned fmt=integer_formats[f];bool signed_format=fmt==21 || fmt==61 || fmt==70 || fmt==76;
        image_desc(u,imgaddr+0x10000+f*0x1000,fmt,1);
        u[8]=signed_format?(uint32_t)-3:65530;u[9]=signed_format?(uint32_t)-7:40000;
        u[10]=5;u[11]=signed_format?(uint32_t)-1:17;
        g.regs[mmCOMPUTE_NUM_THREAD_X]=1;pc(&g,a+0x600);
        renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);pc(&g,a+0x700);
        renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,16);
        CHECK(output[0]==u[8]);
        for(unsigned c=1;c<4;c++) CHECK(output[c]==(fmt==21?(c==3?1:0):u[8+c]));
    }
    puts("signed and unsigned GFX10 image stores/loads: negative values and four channels verified");
    /* Float stores to a depth alias use the synchronized color backing,
     * never a storage view of D32 (unsupported by Vulkan). */
    guest_addr depth_addr=imgaddr+0x20000;
    renderer_submit_begin(r);vk_begin(r);
    vk_image *depth=vk_image_get(r,depth_addr,1,1,VK_FORMAT_D32_SFLOAT_S8_UINT);CHECK(depth);
    vk_barrier(r->cmd,depth,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearDepthStencilValue dc={0.25f,0};VkImageSubresourceRange dr={depth->aspect,0,1,0,1};
    vkCmdClearDepthStencilImage(r->cmd,depth->image,depth->layout,&dc,1,&dr);
    depth->gpu_written=true;depth->written_seq=++r->write_seq;
    image_desc(u,depth_addr,22,1);u[8]=bits(0.75f);pc(&g,a+0x600);
    renderer_dispatch(r,&g,1,1,1);pc(&g,a+0x700);
    renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,16);
    CHECK(real(output[0])==0.75f);
    puts("float depth store: synchronized color alias read back correctly");
    renderer_destroy(r);cleanup_renderer=NULL;free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);cleanup_window=NULL;gmem_free(&m);pal_shutdown();return 0;
}
int main(void){CHECK(cpu_test()==0);return getenv("PS5C_TEST_GPU")?gpu_test():0;}
