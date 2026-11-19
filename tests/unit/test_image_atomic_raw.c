/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/shader/shader.h"
#include "gpu/gpu_regs.h"
#include "gpu/vk/vk_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;} }while(0)
static const uint32_t atomic_code[]={0x7e040280,0x7e060280,0x7e080208,
    0xf0542108,0x00000402, /* image_atomic_umin v4, v[2:3], s[0:7] glc */
    0xe0700000,0x80030400,0xbf810000};
static const uint32_t read_code[]={0x7e040280,0x7e060280,
    0xf0000108,0x00000402,0xe0700000,0x80030400,0xbf810000};
static int cpu_test(void) {
    sh_program *p=calloc(1,sizeof(*p));sh_eval *e=calloc(1,sizeof(*e));
    sh_site_shape *s=calloc(SH_MAX_SITES,sizeof(*s));CHECK(p && e && s);
    CHECK(sh_analyze(atomic_code,PS5C_ARRAY_COUNT(atomic_code),0,p));
    e->site[0].valid=true;e->site[0].desc[1]=22u<<20;e->site[0].desc[3]=9u<<28;
    sh_site_shape_of(p,e,0,&s[0]);
    CHECK(s[0].img_kind==1 && s[0].buf_format==SH_IMG_ATOMIC_UINT32);
    sh_stage_info info={.stage=SH_STAGE_CS,.local_size={1,1,1}};
    uint32_t *spv=NULL;size_t n=0;CHECK(sh_translate(p,&info,s,0,&spv,&n));free(spv);
    uint32_t maximum_code[PS5C_ARRAY_COUNT(atomic_code)];
    memcpy(maximum_code,atomic_code,sizeof(maximum_code));
    maximum_code[3]=(maximum_code[3]&~(0x7fu<<18))|(0x17u<<18);
    CHECK(sh_analyze(maximum_code,PS5C_ARRAY_COUNT(maximum_code),0,p));
    sh_site_shape_of(p,e,0,&s[0]);
    CHECK(s[0].img_kind==1 && s[0].buf_format==SH_IMG_ATOMIC_UINT32);
    CHECK(sh_translate(p,&info,s,0,&spv,&n));free(spv);
    /* DMASK=3 is a single unsigned 64-bit UMAX, rather than two independent
     * dword operations. Unsupported devices must refuse it before caching. */
    maximum_code[3]=(maximum_code[3]&~(15u<<8))|(3u<<8);
    CHECK(sh_analyze(maximum_code,PS5C_ARRAY_COUNT(maximum_code),0,p));
    e->site[0].desc[1]=62u<<20;sh_site_shape_of(p,e,0,&s[0]);
    CHECK(s[0].buf_format==SH_IMG_ATOMIC_UINT64 && s[0].img_kind==1);
    sh_set_image_atomic64(false);CHECK(!sh_translate(p,&info,s,0,&spv,&n));
    sh_set_image_atomic64(true);CHECK(sh_translate(p,&info,s,0,&spv,&n));free(spv);
    memset(&e->site[0],0,sizeof(e->site[0]));
    sh_site_shape_of(p,e,0,&s[0]);CHECK(s[0].buf_format==SH_IMG_ATOMIC_UINT64 && s[0].img_kind==1);
    CHECK(sh_translate(p,&info,s,0,&spv,&n));free(spv);
    e->site[0].valid=true;
    sh_site_shape_of(p,e,0,&s[0]);CHECK(s[0].buf_format==SH_IMG_ATOMIC_UINT64 && s[0].img_kind==1);
    CHECK(sh_translate(p,&info,s,0,&spv,&n));free(spv);
    /* A disabled branch can retain a live RGBA8 placeholder descriptor.
     * It must not prevent the valid 64-bit operations in the same module. */
    e->site[0].desc[1]=56u<<20;e->site[0].desc[3]=13u<<28;
    sh_site_shape_of(p,e,0,&s[0]);CHECK(s[0].buf_format==SH_IMG_ATOMIC_UINT64 && s[0].img_kind==1);
    CHECK(sh_translate(p,&info,s,0,&spv,&n));free(spv);
    sh_set_image_atomic64(false);
    CHECK(sh_analyze(atomic_code,PS5C_ARRAY_COUNT(atomic_code),0,p));
    /* Existing UINT path stays ordinary; unrelated float widths are refused. */
    e->site[0].desc[1]=20u<<20;sh_site_shape_of(p,e,0,&s[0]);
    CHECK(s[0].img_kind==1 && !s[0].buf_format);
    e->site[0].desc[1]=14u<<20;sh_site_shape_of(p,e,0,&s[0]);
    CHECK(!s[0].img_kind && !s[0].buf_format);
    memset(&e->site[0],0,sizeof(e->site[0]));sh_site_shape_of(p,e,0,&s[0]);
    CHECK(s[0].img_kind==1 && !s[0].buf_format);
    CHECK(sh_translate(p,&info,s,0,&spv,&n));free(spv);
    /* A dead resource still needs the opcode's integer type. Exercise each
     * existing integer atomic, including AND in a 2D-array pixel shader. */
    const unsigned ops[]={15,16,17,18,20,21,22,23,24,25,26,27,28};
    for(unsigned k=0;k<PS5C_ARRAY_COUNT(ops);k++) {
        uint32_t code[PS5C_ARRAY_COUNT(atomic_code)];memcpy(code,atomic_code,sizeof(code));
        code[3]=(code[3]&~((0x7fu<<18)|(15u<<8)))|(ops[k]<<18)|((ops[k]==16?3u:1u)<<8);
        CHECK(sh_analyze(code,PS5C_ARRAY_COUNT(code),0,p));
        memset(&e->site[0],0,sizeof(e->site[0]));sh_site_shape_of(p,e,0,&s[0]);
        CHECK(s[0].img_kind==(ops[k]==20 || ops[k]==22?2:1));
        CHECK(sh_translate(p,&info,s,0,&spv,&n));free(spv);
        e->site[0].valid=true; /* A visited null T# has no numeric format either. */
        sh_site_shape_of(p,e,0,&s[0]);CHECK(s[0].img_kind==(ops[k]==20 || ops[k]==22?2:1));
        CHECK(sh_translate(p,&info,s,0,&spv,&n));free(spv);
        e->site[0].desc_unknown=true;sh_site_shape_of(p,e,0,&s[0]);CHECK(!s[0].img_kind);
    }
    CHECK(sh_analyze(atomic_code,PS5C_ARRAY_COUNT(atomic_code),0,p));
    e->site[0].desc_unknown=true;sh_site_shape_of(p,e,0,&s[0]);CHECK(!s[0].img_kind);
    free(s);free(e);free(p);return 0;
}
static int gpu_test(void) {
    bool matrices_correct=true;
    CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,0x30000,GPROT_RW,0,"raw image atomic fixture",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,atomic_code,sizeof(atomic_code)) && gmem_write(&m,a+0x100,read_code,sizeof(read_code)));
    pal_window *w=NULL;CHECK(pal_window_open("ZeraPS5 raw image atomic regression",320,180,&w)==PS5C_OK);
    renderer *r=renderer_create(w,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    g.regs[mmCOMPUTE_PGM_RSRC2]=16u<<1;
    const uint32_t old[]={0x3f800000,0x80000000,0xbf800000,0x3f800000,0x80000000};
    const uint32_t value[]={0x3f000000,0x3f800000,0xbf000000,0x3f800000,0x7fc00123};
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    guest_addr out_addr=a+0x28000;uint32_t *out=gmem_host(&m,out_addr,8);CHECK(out);
    for(unsigned maximum=0;maximum<2;maximum++)for(unsigned fmt=0;fmt<2;fmt++)for(unsigned i=0;i<5;i++) {
        uint32_t operation[PS5C_ARRAY_COUNT(atomic_code)];memcpy(operation,atomic_code,sizeof(operation));
        if(maximum)operation[3]=(operation[3]&~(0x7fu<<18))|(0x17u<<18);
        CHECK(gmem_write(&m,a,operation,sizeof(operation)));
        guest_addr image_addr=a+0x4000+(maximum*10+fmt*5+i)*0x1000;
        memset(u,0,16*4);
        u[0]=(uint32_t)(image_addr>>8);u[1]=(uint32_t)(image_addr>>40)|((fmt?22u:20u)<<20);
        u[3]=(9u<<28)|0xfac;u[8]=value[i];
        u[12]=(uint32_t)out_addr;u[13]=(uint32_t)(out_addr>>32);u[14]=8;
        out[0]=out[1]=0;
        renderer_submit_begin(r);vk_begin(r);
        vk_image *img=vk_image_get(r,image_addr,1,1,fmt?VK_FORMAT_R32_SFLOAT:VK_FORMAT_R32_UINT);CHECK(img);
        vk_barrier(r->cmd,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue cv={.uint32={old[i],0,0,0}};
        VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(r->cmd,img->image,img->layout,&cv,1,&range);img->gpu_written=true;img->written_seq=++r->write_seq;
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(a>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(a>>40);
        renderer_dispatch(r,&g,1,1,1);renderer_gpu_idle(r);
        CHECK(out[0]==old[i]); /* GLC returns the old raw bits */
        if(getenv("PS5C_TEST_LINEAR_IMAGE_WB")) {
            uint32_t value_cpu=0,expected_cpu=maximum?(old[i]>value[i]?old[i]:value[i]):(old[i]<value[i]?old[i]:value[i]);
            CHECK(gmem_read(&m,image_addr,&value_cpu,4));
            fprintf(stderr,"linear CPU image %08x expected %08x\n",value_cpu,expected_cpu);
            CHECK(value_cpu==expected_cpu);
        }
        u[12]=(uint32_t)(out_addr+4);u[13]=(uint32_t)((out_addr+4)>>32);u[14]=4;
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x100)>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)((a+0x100)>>40);
        renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out_addr,8);
        uint32_t expected=maximum?(old[i]>value[i]?old[i]:value[i]):(old[i]<value[i]?old[i]:value[i]);
        fprintf(stderr,"raw %s fmt=%u case=%u old=%08x new=%08x expected=%08x\n",maximum?"UMAX":"UMIN",fmt?22u:20u,i,out[0],out[1],expected);
        CHECK(out[1]==expected);
    }
    /* The resource of a statically skipped atomic is never evaluated, but
     * compiling its block must not reject the whole dispatch. */
    uint32_t skipped[]={0xbf820002,0xf0542108,0x00000402,
        0x7e080287,0xe0700000,0x80030400,0xbf810000};
    for(unsigned k=0;k<(sh_image_atomic64_supported()?3u:2u);k++) {
    if(k)skipped[1]=(skipped[1]&~((0x7fu<<18)|(7u<<3)))|(24u<<18)|(5u<<3);
    if(k==2)skipped[1]=(skipped[1]&~((0x7fu<<18)|(7u<<3)|(15u<<8)))|(23u<<18)|(1u<<3)|(3u<<8);
    CHECK(gmem_write(&m,a+0x200,skipped,sizeof(skipped)));
    memset(u,0,16*4);u[12]=(uint32_t)out_addr;u[13]=(uint32_t)(out_addr>>32);u[14]=4;out[0]=0;
    g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x200)>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)((a+0x200)>>40);
    renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out_addr,8);
    fprintf(stderr,"unvisited atomic: visible store %u (expected 7)\n",out[0]);CHECK(out[0]==7);
    }
    if(getenv("PS5C_TEST_LINEAR_IMAGE_WB")) {
        /* A CPU readback matrix polls its last float, without a buffer-store
         * result or RELEASE_MEM. Exercise non-square, tightly packed rows. */
        const uint32_t store[]={0x7e080208,0xf0200108,0x00000400,0xbf810000};
        CHECK(gmem_write(&m,a+0x300,store,sizeof(store)));
        for(unsigned array=0;array<2;array++) {
        guest_addr image_addr=a+0x18000+array*0x1000;uint32_t initial[17];
        for(unsigned i=0;i<17;i++)initial[i]=0xbf800000;
        initial[0]=initial[16]=0xa5a5a5a5;
        CHECK(gmem_write(&m,image_addr-4,initial,sizeof(initial)));
        memset(u,0,16*4);u[0]=(uint32_t)(image_addr>>8);u[1]=(uint32_t)(image_addr>>40)|(22u<<20);
        u[2]=1u|(2u<<14);u[3]=((array?13u:9u)<<28)|0xfac;u[8]=array?0x3f400000:0x3f000000;
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x300)>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)((a+0x300)>>40);
        g.regs[mmCOMPUTE_PGM_RSRC2]=(16u<<1)|(1u<<11);
        g.regs[mmCOMPUTE_NUM_THREAD_X]=5;g.regs[mmCOMPUTE_NUM_THREAD_Y]=3;
        renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_gpu_idle(r);
        uint32_t observed[17];CHECK(gmem_read(&m,image_addr-4,observed,sizeof(observed)));
        unsigned bad=observed[0]!=0xa5a5a5a5 || observed[16]!=0xa5a5a5a5;
        for(unsigned i=1;i<=15;i++)bad+=observed[i]!=u[8];
        fprintf(stderr,"linear 5x3 %s: %u wrong CPU values; first %08x expected %08x\n",array?"array":"2D",bad,observed[1],u[8]);
        matrices_correct &= bad==0;
        }
    }
    if(sh_image_atomic64_supported()) {
        const uint64_t previous[]={0x100000000ull,0x00000000ffffffffull,0x8000000000000000ull,0xffffffffffffffffull};
        const uint64_t incoming[]={0x00000000ffffffffull,0x100000000ull,0x7fffffffffffffffull,0xffffffffffffffffull};
        for(unsigned maximum=0;maximum<2;maximum++)for(unsigned array=0;array<2;array++)for(unsigned i=0;i<4;i++) {
            uint32_t code[]={0x7e040280,0x7e060280,0x7e080208,0x7e0a0209,
                0xf0542308,0x00000402,0xe0740000,0x80030400,0xbf810000};
            if(maximum)code[4]=(code[4]&~(0x7fu<<18))|(0x17u<<18);
            CHECK(gmem_write(&m,a+0x400,code,sizeof(code)));
            guest_addr address=a+0x20000+(maximum*8+array*4+i)*0x100;
            memset(u,0,16*4);u[0]=(uint32_t)(address>>8);u[1]=(uint32_t)(address>>40)|(62u<<20);
            u[3]=((array?13u:9u)<<28)|0xfac;u[8]=(uint32_t)incoming[i];u[9]=(uint32_t)(incoming[i]>>32);
            u[12]=(uint32_t)out_addr;u[13]=(uint32_t)(out_addr>>32);u[14]=8;
            renderer_submit_begin(r);vk_begin(r);
            vk_image *img=vk_image_get(r,address,1,1,VK_FORMAT_R32G32_UINT);CHECK(img);
            vk_barrier(r->cmd,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkClearColorValue cv={.uint32={(uint32_t)previous[i],(uint32_t)(previous[i]>>32),0,0}};
            VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdClearColorImage(r->cmd,img->image,img->layout,&cv,1,&range);img->gpu_written=true;img->written_seq=++r->write_seq;
            g.regs[mmCOMPUTE_PGM_RSRC2]=16u<<1;
            g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=1;
            g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x400)>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)((a+0x400)>>40);
            out[0]=out[1]=0;renderer_dispatch(r,&g,1,1,1);renderer_gpu_idle(r);
            uint64_t returned=(uint64_t)out[0]|((uint64_t)out[1]<<32);
            vk_buffer raw={0};CHECK(vk_buffer_create(r,8,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&raw));
            vk_begin(r);vk_barrier(r->cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=(VkExtent3D){1,1,1};
            vkCmdCopyImageToBuffer(r->cmd,img->image,img->layout,raw.buffer,1,&copy);renderer_wait_idle(r);
            uint64_t actual=0;memcpy(&actual,raw.mapped,8);
            uint64_t expected=maximum?(previous[i]>incoming[i]?previous[i]:incoming[i]):(previous[i]<incoming[i]?previous[i]:incoming[i]);
            fprintf(stderr,"image64 %s %s case %u: old %016llx expected %016llx, new %016llx expected %016llx\n",
                maximum?"UMAX":"UMIN",array?"array":"2D",i,(unsigned long long)returned,(unsigned long long)previous[i],
                (unsigned long long)actual,(unsigned long long)expected);
            matrices_correct &= returned==previous[i] && actual==expected;vk_buffer_destroy(r,&raw);
        }
        /* An incompatible live placeholder must never be bound to R64ui or
         * overwritten through that view. Unsupported accesses use the typed
         * discard image, while the rest of the dispatch still executes. */
        const uint32_t incompatible[]={0x7e040280,0x7e060280,0x7e080208,0x7e0a0209,
            0xf05c2308,0x00000402,0xe0740000,0x80030400,0xbf810000};
        CHECK(gmem_write(&m,a+0x500,incompatible,sizeof(incompatible)));
        guest_addr placeholder=a+0x22000;
        memset(u,0,16*4);u[0]=(uint32_t)(placeholder>>8);u[1]=(uint32_t)(placeholder>>40)|(56u<<20);
        u[3]=(9u<<28)|0xfac;u[8]=0xffffffffu;u[9]=0xffffffffu;
        u[12]=(uint32_t)out_addr;u[13]=(uint32_t)(out_addr>>32);u[14]=8;
        renderer_submit_begin(r);vk_begin(r);
        vk_image *dummy=vk_image_get(r,placeholder,1,1,VK_FORMAT_R8G8B8A8_UNORM);CHECK(dummy);
        vk_barrier(r->cmd,dummy,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue cv={.float32={1.0f,0.0f,1.0f,1.0f}};
        VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(r->cmd,dummy->image,dummy->layout,&cv,1,&range);
        dummy->gpu_written=true;dummy->written_seq=++r->write_seq;
        out[0]=out[1]=0x55555555u;
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x500)>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)((a+0x500)>>40);
        renderer_dispatch(r,&g,1,1,1);renderer_gpu_idle(r);
        CHECK(out[0]==0 && out[1]==0);
        vk_buffer raw={0};CHECK(vk_buffer_create(r,4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&raw));
        vk_begin(r);vk_barrier(r->cmd,dummy,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=(VkExtent3D){1,1,1};
        vkCmdCopyImageToBuffer(r->cmd,dummy->image,dummy->layout,raw.buffer,1,&copy);renderer_wait_idle(r);
        uint32_t pixel=0;memcpy(&pixel,raw.mapped,4);CHECK(pixel==0xffff00ffu);vk_buffer_destroy(r,&raw);
    }
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(w);gmem_free(&m);pal_shutdown();
    return matrices_correct?0:1;
}
int main(void){int rc=cpu_test();if(rc)return rc;return getenv("PS5C_TEST_GPU")?gpu_test():0;}
