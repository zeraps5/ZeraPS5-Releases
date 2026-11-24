/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The bound must describe the immutable index snapshot consumed by this draw,
 * not the first contents seen at the same guest address in this epoch. */
#include "gpu/vk/vk_internal.h"
#include "gpu/shader/shader.h"
#include "gpu/gpu_regs.h"
#include "gpu/vk/vk_index_bounds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static renderer *cleanup_renderer;
static vk_buffer *cleanup_output;
static void cleanup_failed(void) {
    renderer *r=cleanup_renderer;cleanup_renderer=NULL;
    if(r){if(cleanup_output)vk_buffer_destroy(r,cleanup_output);renderer_destroy(r);}
    cleanup_output=NULL;
}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);cleanup_failed();return 1;}}while(0)

static int bounds(void) {
    vk_index_bounds cache={0};
    uint16_t tiny[]={0,1,2};CHECK(vk_index_maximum(&cache,tiny,3,2,1)==2);
    tiny[2]=66;CHECK(vk_index_maximum(&cache,tiny,3,2,1)==66 && cache.next==0);
    uint16_t *a=calloc(4096,2),*b=calloc(4096,2);CHECK(a && b);
    a[4095]=2;b[4095]=66;
    CHECK(vk_index_maximum(&cache,a,4096,2,1)==2 && cache.next==1);
    CHECK(vk_index_maximum(&cache,a,4096,2,1)==2 && cache.next==1);
    CHECK(vk_index_maximum(&cache,b,4096,2,1)==66 && cache.next==2);
    /* Reusing a completed slot's storage in another epoch must miss. */
    a[4095]=99;CHECK(vk_index_maximum(&cache,a,4096,2,2)==99 && cache.next==3);
    CHECK(vk_index_maximum(&cache,a,4095,2,2)==0 && cache.next==4);
    vk_index_bounds other={0};
    CHECK(vk_index_maximum(&other,a,2047,2,2)==0 && other.next==0);
    CHECK(vk_index_maximum(&other,a,2048,2,2)==0 && other.next==1);
    other.entries[0].size=4;other.entries[0].max=123; /* a different-width entry cannot hit */
    CHECK(vk_index_maximum(&other,a,2048,2,2)==0 && other.next==2);
    CHECK(vk_index_maximum(&cache,NULL,0,2,2)==0);
    CHECK(vk_index_maximum(&cache,NULL,1,2,2)==UINT32_MAX);
    const uint8_t bytes[] = {1, 7, 5};
    CHECK(vk_index_maximum(&cache, bytes, 3, 1, 2) == 7);
    CHECK(vk_index_maximum(&cache, a, 1, 3, 2) == UINT32_MAX);
    uint32_t *words=calloc(4096,4);CHECK(words);words[4095]=UINT32_MAX;
    CHECK(vk_index_maximum(&cache,words,4096,4,2)==UINT32_MAX);
    /* Exhaust every entry. Different snapshots keep bounded storage; the
     * last one hits and the evicted first one gets scanned again. */
    uint16_t *many=calloc((VK_INDEX_BOUNDS_ENTRIES+1)*4096,2);CHECK(many);
    memset(&cache,0,sizeof(cache));
    for(unsigned i=0;i<=VK_INDEX_BOUNDS_ENTRIES;i++) {
        uint16_t *s=many+i*4096;s[4095]=(uint16_t)(i+10);
        CHECK(vk_index_maximum(&cache,s,4096,2,7)==i+10);
    }
    CHECK(cache.next==VK_INDEX_BOUNDS_ENTRIES+1);
    CHECK(vk_index_maximum(&cache,many+VK_INDEX_BOUNDS_ENTRIES*4096,4096,2,7)==VK_INDEX_BOUNDS_ENTRIES+10 && cache.next==VK_INDEX_BOUNDS_ENTRIES+1);
    CHECK(vk_index_maximum(&cache,many,4096,2,7)==10 && cache.next==VK_INDEX_BOUNDS_ENTRIES+2);
    free(a);free(b);free(words);free(many);
    puts("index bounds: snapshot identity, tiny mutable inputs, epoch/count/width, overflow values and bounded eviction verified");return 0;
}

static int pixels(void) {
    /* PS5 ES user data begins at s8, vertex ID at v5. */
    static const uint32_t vs[]={0xe0382000,0x80020005,0xbf8cc07f,0xf80008cf,0x03020100,0xbf810000};
    static const uint32_t ps[]={0x7e0002f2,0x7e020280,0x7e040280,0x7e0602f2,0xf800180f,0x03020100,0xbf810000};
    sh_program *p=calloc(1,sizeof(*p));CHECK(p && sh_analyze(vs,PS5C_ARRAY_COUNT(vs),0,p));
    CHECK(p->site_count==1 && p->sites[0].kind==SH_SITE_BUFFER && !p->has_stores && !p->back_branch);free(p);
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;CHECK(gmem_map(&m,0,2u<<20,GPROT_RW,0,"vertex snapshot fixture",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,vs,sizeof(vs)) && gmem_write(&m,a+0x100,ps,sizeof(ps)));
    guest_addr vertices=a+0x10000,indices=a+0x20000,targets=a+0x80000;
    float *positions=gmem_host(&m,vertices,2048);CHECK(positions);memset(positions,0,2048);
    const float triangle[]={-8,-8,0,1,56,-8,0,1,-8,56,0,1};
    memcpy(positions,triangle,sizeof(triangle));memcpy(positions+64*4,triangle,sizeof(triangle));
    void *ix=gmem_host(&m,indices,65536);CHECK(ix);
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 immutable vertex indices",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m);CHECK(r);cleanup_renderer=r;
    gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(a>>8);g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(a>>40);
    g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x100)>>8);g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)((a+0x100)>>40);
    g.regs[mmSPI_SHADER_PGM_RSRC2_GS]=4u<<1;
    uint32_t *u=g.regs+mmSPI_SHADER_USER_DATA_GS_0;
    u[0]=(uint32_t)vertices;u[1]=(uint32_t)(vertices>>32)|(16u<<16);u[2]=128;u[3]=(77u<<12)|0x688;
    g.regs[mmCB_COLOR_CONTROL]=0xCC0010;g.regs[mmCB_TARGET_MASK]=g.regs[mmCB_SHADER_MASK]=15;
    g.regs[mmCB_COLOR0_INFO]=10u<<2;g.regs[mmCB_COLOR0_ATTRIB2]=(15u<<14)|15;
    g.regs[mmSPI_SHADER_COL_FORMAT]=9;g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_TRILIST;
    /* Each case is one epoch: change only indices, retain code/descriptor/
     * count/address. Small inputs are always copied afresh by the original
     * arena; large inputs are explicitly invalidated as a CP write would be. */
    for(unsigned test=0;test<4;test++) {
        unsigned bytes=test&1?4:2,count=test<2?3:6144;
        gpu_draw draw={.indexed=true,.count=count,.instances=1,.index_addr=indices,.index_size=bytes};
        renderer_submit_begin(r);
        for(unsigned version=0;version<3;version++) {
            if(version<2) {
                for(unsigned i=0;i<count;i++) {
                    uint32_t value=(version?64:0)+i%3;
                    if(bytes==2)((uint16_t*)ix)[i]=(uint16_t)value;else ((uint32_t*)ix)[i]=value;
                }
                if(test>=2)renderer_mem_written(r,indices,(uint64_t)bytes*count);
            }
            g.regs[mmCB_COLOR0_BASE]=(uint32_t)((targets+(test*3+version)*0x1000)>>8);
            renderer_draw(r,&g,&draw);
        }
        renderer_wait_idle(r);
    }
    vk_buffer output;CHECK(vk_buffer_create(r,1024,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&output));cleanup_output=&output;
    unsigned failed=0;
    for(unsigned n=0;n<12;n++) {
        vk_image *image=vk_image_find(r,targets+n*0x1000,16,16,VK_FORMAT_R8G8B8A8_UNORM);CHECK(image);
        VkCommandBuffer cmd=vk_begin(r);vk_barrier(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={16,16,1}};
        vkCmdCopyImageToBuffer(cmd,image->image,image->layout,output.buffer,1,&copy);renderer_wait_idle(r);
        for(unsigned i=0;i<256;i++) {
            uint32_t word=((uint32_t*)output.mapped)[i];
            if(word!=0xff0000ffu) {
                if(!i)fprintf(stderr,"vertex snapshot image %u: %08x expected ff0000ff\n",n,word);
                failed++;
            }
        }
    }
    printf("vertex snapshot GPU: %u incorrect RGBA pixels of 3072\n",failed);
    vk_buffer_destroy(r,&output);cleanup_output=NULL;cleanup_renderer=NULL;renderer_destroy(r);pal_window_destroy(window);
    free(g.regs);pal_mutex_destroy(g.lock);gmem_free(&m);pal_shutdown();CHECK(!failed);return 0;
}
int main(void) {CHECK(bounds()==0);if(getenv("PS5C_TEST_GPU"))return pixels();return 0;}
