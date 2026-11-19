/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A PS may read one parameter with both interpolation modes. Check the real
 * VS/PS interface and pixel values, rather than only successful translation. */
#include "gpu/gpu_regs.h"
#include "gpu/vk/vk_internal.h"
#include "gpu/shader/shader.h"
#include "gpu/vk/vk_shader_code.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static uint32_t bits(float x) { uint32_t b; memcpy(&b,&x,4); return b; }

static const uint32_t vertex[] = {
    0x36000a81,0x2c020a81,0x36020281,0x7e000d00,0x7e020d01,
    0x420000f6,0xbf800000,0x420202f6,0xbf800000,0x7e040280,0x7e0602f2,
    /* Round-trip X through the final dword of a 512-byte LDS allocation.
     * A shift of 20 in RSRC2 reads that allocation as zero. */
    0x7e140280, /* v10 = 0 */
    0xd83401fc,0x0000000a, /* ds_write_b32 v10,v0 offset:508 */
    0xd8d801fc,0x0000000a, /* ds_read_b32 v0,v10 offset:508 */
    0xf800020f,0x03020100, /* param0 = clip position */
    0xf80008cf,0x03020100,0xbf810000
};
static const uint32_t pixel[] = {
    0xc8100000,0xc8110001, /* attr0.x P1(I), P2(J) -> v4 */
    0xc8140500,0xc8150501, /* attr1.y P1(I), P2(J) -> v5 */
    0x7e040280,0x7e0602f2,0xf800080f,0x03020504,0xbf810000
};

static int long_program(void) {
    sh_program *p=calloc(1,sizeof(*p)); uint32_t *code=malloc(4*(SH_MAX_INSTS+1)); CHECK(p && code);
    /* The branch crosses the old instruction limit; next_scalar/target must
     * retain the complete 16-bit instruction index. */
    for(unsigned i=0;i<SH_MAX_INSTS+1;i++) code[i]=0xbf800000;
    code[0]=0xbf820000u|9000u; code[10000]=0xbf810000;
    CHECK(sh_analyze(code,10001,0,p) && p->count==10001 && p->target[0]==9001);
    /* Reproduce the size class of a material rejected before reaching its
     * terminator. Keep the long branch index, not a truncated 8/14-bit value. */
    code[0]=0xbf820000u|20000u; code[10000]=0xbf800000;
    code[21176]=0xbf810000;
    CHECK(sh_analyze(code,21177,0,p) && p->count==21177 && p->target[0]==20001);
    sh_stage_info info={.stage=SH_STAGE_CS,.local_size={1,1,1}};
    uint32_t *spv; size_t words; CHECK(sh_translate(p,&info,NULL,0,&spv,&words)); free(spv);
    code[0]=0xbf800000; code[21176]=0xbf800000; code[SH_MAX_INSTS-1]=0xbf810000;
    CHECK(sh_analyze(code,SH_MAX_INSTS,0,p) && p->count==SH_MAX_INSTS);
    code[SH_MAX_INSTS-1]=0xbf800000; code[SH_MAX_INSTS]=0xbf810000;
    CHECK(!sh_analyze(code,SH_MAX_INSTS+1,0,p));
    /* Scalar loads require uniform slots, not a Vulkan descriptor each.
     * Check the real large-material size, exact capacity and one past it. */
    for(unsigned i=0;i<=SH_MAX_SITES;i++) {
        code[2*i]=0xf4000200; code[2*i+1]=0xfa000000; /* s_load_dword s8, s0, 0 */
    }
    code[6066]=0xbf810000;
    CHECK(sh_analyze(code,6067,0,p) && p->site_count==3033 && p->site_of[3032]==3032);
    code[6066]=0xf4000200;
    code[2*SH_MAX_SITES]=0xbf810000;
    CHECK(sh_analyze(code,2*SH_MAX_SITES+1,0,p) && p->site_count==SH_MAX_SITES);
    code[2*SH_MAX_SITES]=0xf4000200;
    code[2*SH_MAX_SITES+2]=0xbf810000;
    CHECK(!sh_analyze(code,2*SH_MAX_SITES+3,0,p));
    free(code);free(p); puts("long shaders: branch past 8192, exact capacity and overflow verified"); return 0;
}

static int graphics_lds_register(void) {
    CHECK(sh_ps_input_pack(0x424)==4); /* OFFSET[5] + FLAT_SHADE: raw vertex parameter4 */
    CHECK(sh_ps_input_pack(0x20)==0x20); /* no flat shade: default constant */
    CHECK(sh_ps_input_pack(0x124)==0x64);
    CHECK(sh_graphics_lds_dwords(0x00000000)==0);
    CHECK(sh_graphics_lds_dwords(0x00080000)==128);
    CHECK(sh_graphics_lds_dwords(0x00100000)==256);
    CHECK(sh_graphics_lds_dwords(0x07f80000)==32640);
    CHECK(sh_graphics_lds_dwords(0x08000000)==0); /* user SGPR bit alone */
    CHECK(sh_graphics_lds_dwords(0x08080000)==128);
    CHECK(sh_graphics_lds_dwords(0xf807ffff)==0); /* all unrelated bits */
    return 0;
}

static int program_window(void) {
    gmem m; gmem_config cfg=gmem_default_config(); cfg.identity=false;
    CHECK(pal_init()==PS5C_OK && gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr base;
    CHECK(gmem_map(&m,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"shader window",&base)==PS5C_OK);
    guest_addr addr=base+GUEST_PAGE_SIZE/2;
    uint64_t bytes=8*GUEST_PAGE_SIZE-GUEST_PAGE_SIZE/2;
    uint32_t *code=gmem_host(&m,addr,bytes); CHECK(code);
    for(uint64_t i=0;i<bytes/4;i++) code[i]=0xbf800000;
    code[21176]=0xbf810000;
    uint32_t words=0;
    const uint32_t *span=vk_shader_code(&m,addr,&words);
    CHECK(span==code && words==bytes/4 && words>0x4000);
    sh_program *p=calloc(1,sizeof(*p));CHECK(p);
    CHECK(!sh_analyze(span,0x4000,addr,p)); /* the old64KiB window */
    CHECK(sh_analyze(span,words,addr,p) && p->count==21177 && p->dwords==21177);
    CHECK(gmem_protect(&m,base+4*GUEST_PAGE_SIZE,4*GUEST_PAGE_SIZE,GPROT_READ)==PS5C_OK);
    CHECK(vk_shader_code(&m,addr,&words)==code && words==bytes/4);
    CHECK(sh_analyze(code,words,addr,p) && p->count==21177);
    CHECK(gmem_protect(&m,base+4*GUEST_PAGE_SIZE,4*GUEST_PAGE_SIZE,GPROT_RW)==PS5C_OK);
    code[21176]=0xbf800000;
    CHECK(!sh_analyze(span,words,addr,p)); /* still bounded at the VMA end */
    code[0]=0xbf9f0000; code[1]=0xbe802006;
    CHECK(sh_front_dwords(code,2)==0); /* do not borrow a neighboring fetch body */
    CHECK(!vk_shader_code(&m,addr+1,&words) && words==0);
    CHECK(!vk_shader_code(&m,base+8*GUEST_PAGE_SIZE,&words) && words==0);
    CHECK(gmem_protect(&m,base,8*GUEST_PAGE_SIZE,GPROT_NONE)==PS5C_OK);
    CHECK(!vk_shader_code(&m,addr,&words) && words==0);
    guest_addr large;
    CHECK(gmem_map(&m,0,(uint64_t)SH_MAX_CODE_DWORDS*4+GUEST_PAGE_SIZE,GPROT_RW,0,"bounded shader",&large)==PS5C_OK);
    CHECK(vk_shader_code(&m,large,&words) && words==SH_MAX_CODE_DWORDS);
    free(p);gmem_free(&m);pal_shutdown();return 0;
}

static int gpu_test(void) {
    CHECK(pal_init()==PS5C_OK); gmem m; gmem_config cfg=gmem_default_config(); cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK); guest_addr a;
    CHECK(gmem_map(&m,0,4*GUEST_PAGE_SIZE,GPROT_RW,0,"mixed interpolation fixture",&a)==PS5C_OK);
    CHECK(gmem_write(&m,a,vertex,sizeof(vertex)) && gmem_write(&m,a+0x100,pixel,sizeof(pixel)));
    pal_window *win=NULL; CHECK(pal_window_open("ZeraPS5 mixed interpolation regression",320,180,&win)==PS5C_OK);
    renderer *r=renderer_create(win,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(a>>8);g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(a>>40);
    g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x100)>>8);g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)((a+0x100)>>40);
    g.regs[mmCB_COLOR0_BASE]=(uint32_t)((a+0x2000)>>8);
    g.regs[mmCB_COLOR0_INFO]=(14u<<2)|(7u<<8); /* R32G32B32A32_SFLOAT */
    g.regs[mmCB_COLOR0_ATTRIB2]=(3u<<14)|3u;
    g.regs[mmCB_COLOR_CONTROL]=0x10;g.regs[mmCB_TARGET_MASK]=15;
    g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_TRILIST;g.regs[mmPA_CL_VTE_CNTL]=15;
    g.regs[mmSPI_PS_INPUT_ADDR]=2; /* perspective centre I/J in v0/v1 */
    g.regs[mmPA_CL_VPORT_XSCALE]=g.regs[mmPA_CL_VPORT_XOFFSET]=bits(2);
    g.regs[mmPA_CL_VPORT_YSCALE]=g.regs[mmPA_CL_VPORT_YOFFSET]=bits(2);
    gpu_draw draw={.count=3,.instances=1};
    vk_buffer out;CHECK(vk_buffer_create(r,7*5*16,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&out));
    for(unsigned pass=0;pass<(getenv("PS5C_TEST_RAW_VERTEX")?16u:getenv("PS5C_TEST_PS_WAVES")?12u:getenv("PS5C_PS_VERTEX")?8u:3u);pass++) {
        if(pass==3) {
            /* Packed vertex data must be fetched before interpolation. Each
             * selector reads a different vertex, independent of pixel position. */
            const uint32_t pv[]={0xc8020002,0xc8060000,0xc80a0001,0x7e0602f2,0xf800080f,0x03020100,0xbf810000};
            CHECK(gmem_write(&m,a+0x200,pv,sizeof(pv)));
            g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x200)>>8);
            g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)((a+0x200)>>40);
        }
        if(pass==4) {
            const uint32_t bc[]={0x7e040280,0x7e0602f2,0xf800080f,0x03020100,0xbf810000};
            CHECK(gmem_write(&m,a+0x300,bc,sizeof(bc)));
            g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x300)>>8);
            g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)((a+0x300)>>40);
            g.regs[mmSPI_PS_INPUT_ADDR]=2;
        }
        if(pass==5) {
            const uint32_t pack[]={0x7e0002ff,0x7f7fffff,0x7e0202ff,0xff7fffff,
                0x7e0402ff,0x3dcccccd,0x7e0602ff,0x3ecccccd,
                0x5e000300,0x5e020702,0xf8001c0f,0x00000100,0xbf810000};
            CHECK(gmem_write(&m,a+0x400,pack,sizeof(pack)));
            g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x400)>>8);
            g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)((a+0x400)>>40);
        }
        if(pass==6) {
            /* A constant non-dyadic payload must retain its bits over every
             * pixel. Summing three rounded weighted copies changes packed IDs
             * on NVIDIA; the guest's two interpolation steps preserve it. */
            uint32_t constant_vertex[64],count=0;
            for(unsigned i=0;i<PS5C_ARRAY_COUNT(vertex);i++) {
                if(vertex[i]==0xf800020f) {
                    constant_vertex[count++]=0x7e2802ff; /* v20 = payload */
                    constant_vertex[count++]=0x40a12345;
                    constant_vertex[count++]=vertex[i++];
                    constant_vertex[count++]=0x14141414;
                } else constant_vertex[count++]=vertex[i];
            }
            const uint32_t constant_pixel[]={0xc80a0002, /* MOV P0 enables explicit vertex access */
                0xc8000000,0xc8010001, /* P1 I then P2 J */
                0x7e020280,0x7e040280,0x7e0602f2,0xf800080f,0x03020100,0xbf810000};
            CHECK(gmem_write(&m,a,constant_vertex,count*4) && gmem_write(&m,a+0x500,constant_pixel,sizeof(constant_pixel)));
            g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x500)>>8);
            g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)((a+0x500)>>40);
            g.regs[mmCB_COLOR0_ATTRIB2]=(6u<<14)|4u;
            g.regs[mmPA_CL_VPORT_XSCALE]=g.regs[mmPA_CL_VPORT_XOFFSET]=bits(3.5f);
            g.regs[mmPA_CL_VPORT_YSCALE]=g.regs[mmPA_CL_VPORT_YOFFSET]=bits(2.5f);
            g.regs[mmSPI_PS_INPUT_ADDR]=2;
        }
        if(pass==7) {
            /* The instructions consume their actual source VGPRs and P2's
             * prior destination. P1(X, .25)=0; P2(Y, .75, prior=0)=3.
             * Screen-space interpolation cannot satisfy these constant pixels. */
            const uint32_t custom_pixel[]={0x7e0c02ff,0x3e800000,0x7e0e02ff,0x3f400000,
                0xc8100006,0x7e0a0304,0xc8110107,
                0x7e040280,0x7e0602f2,0xf800080f,0x03020504,0xbf810000};
            CHECK(gmem_write(&m,a,vertex,sizeof(vertex)) && gmem_write(&m,a+0x600,custom_pixel,sizeof(custom_pixel)));
            g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x600)>>8);
            g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)((a+0x600)>>40);
        }
        if(pass>=8) {
            const uint32_t wave_pixel[]={
                0xbeeb03ff,0x40400000, /* VCC_HI scalar temporary = 3.0 bits */
                0x7e000281,0x7d840080, /* v0=1; false compare clears both wave64 halves */
                0x7e00026b,0x7e020280,0x7e040280,0x7e0602f2,
                0xf800080f,0x03020100,0xbf810000
            };
            CHECK(gmem_write(&m,a+0x700,wave_pixel,sizeof(wave_pixel)));
            g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x700)>>8);
            g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)((a+0x700)>>40);
            g.regs[0xA1B6u]=(pass&1)?0x8000u:0;
        }
        if(pass>=12) {
            const uint32_t payloads[]={0xffffffff,0x80000001,0x00003c00,0x7fa12345};
            uint32_t code[64],n=0;
            for(unsigned i=0;i<PS5C_ARRAY_COUNT(vertex);i++) {
                if(vertex[i]==0xf800020f) {
                    code[n++]=0x7e2802ff;code[n++]=payloads[pass-12];
                    code[n++]=vertex[i++];code[n++]=0x14141414;
                } else code[n++]=vertex[i];
            }
            const uint32_t raw_pixel[]={0xc8020002, /* MOV P0 -> v0 */
                0x7e020280,0x7e040280,0x7e0602f2,0xf800080f,0x03020100,0xbf810000};
            CHECK(gmem_write(&m,a,code,n*4) && gmem_write(&m,a+0x800,raw_pixel,sizeof(raw_pixel)));
            g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x800)>>8);
            g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)((a+0x800)>>40);
        }
        g.regs[mmSPI_SHADER_PGM_RSRC2_GS]=pass==1?0x08080000:0x00080000;
        /* Reuse the VS with flat Y, flat X, then both smooth. */
        g.regs[mmSPI_PS_INPUT_CNTL_0]=pass==3?0x420:pass==1?1u<<10:0;
        g.regs[mmSPI_PS_INPUT_CNTL_0+1]=pass==0?1u<<10:0;
        renderer_submit_begin(r);renderer_draw(r,&g,&draw);renderer_wait_idle(r);
        unsigned width=pass>=6?7:4,height=pass>=6?5:4;
        vk_image *img=vk_image_find(r,a+0x2000,width,height,VK_FORMAT_R32G32B32A32_SFLOAT);CHECK(img && img->gpu_written);
        VkCommandBuffer cmd=vk_begin(r);
        vk_barrier(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy c={0};c.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;c.imageSubresource.layerCount=1;
        c.imageExtent=(VkExtent3D){width,height,1};vkCmdCopyImageToBuffer(cmd,img->image,img->layout,out.buffer,1,&c);
        renderer_wait_idle(r);
        const float *v=out.mapped;
        for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++) {
            unsigned at=4*(y*width+x);
            if(pass==6) {
                if(bits(v[at])!=0x40a12345)fprintf(stderr,"constant interpolation pixel %u,%u: %08x expected 40a12345\n",x,y,bits(v[at]));
                CHECK(bits(v[at])==0x40a12345 && v[at+1]==0 && v[at+2]==0 && v[at+3]==1);continue;
            }
            if(pass>=12) {
                const uint32_t payloads[]={0xffffffff,0x80000001,0x00003c00,0x7fa12345};
                if(bits(v[at])!=payloads[pass-12])fprintf(stderr,"raw vertex pixel payload: %08x expected %08x\n",bits(v[at]),payloads[pass-12]);
                CHECK(bits(v[at])==payloads[pass-12] && v[at+1]==0 && v[at+2]==0 && v[at+3]==1);continue;
            }
            if(pass>=8) {
                float expected=(pass&1)?3:0;
                if(v[at]!=expected)fprintf(stderr,"pixel wave%u: %g expected %g\n",(pass&1)?32:64,v[at],expected);
                CHECK(v[at]==expected && v[at+1]==0 && v[at+2]==0 && v[at+3]==1);continue;
            }
            if(pass==7){CHECK(v[at]==3 && v[at+1]==0 && v[at+2]==0 && v[at+3]==1);continue;}
            if(pass==3){CHECK(v[at]==-1 && v[at+1]==3 && v[at+2]==-1 && v[at+3]==1);continue;}
            if(pass==5){CHECK(v[at]==65504 && v[at+1]==-65504 && v[at+2]==0.0999755859375f && v[at+3]==0.39990234375f);continue;}
            if(pass==4) {
                CHECK(fabsf(v[at]-(x+0.5f)/8)<0.0001f && fabsf(v[at+1]-(y+0.5f)/8)<0.0001f && v[at+2]==0 && v[at+3]==1);continue;
            }
            float ex=pass==1?-1.0f:(x+0.5f)/2-1,ey=pass==0?-1.0f:(y+0.5f)/2-1;
            if(fabsf(v[at]-ex)>0.0001f || fabsf(v[at+1]-ey)>0.0001f)
                fprintf(stderr,"pass %u pixel %u,%u: %g,%g expected %g,%g\n",pass,x,y,v[at],v[at+1],ex,ey);
            CHECK(fabsf(v[at]-ex)<0.0001f && fabsf(v[at+1]-ey)<0.0001f && v[at+2]==0 && v[at+3]==1);
        }
    }
    if(getenv("PS5C_TEST_RAW_VERTEX"))puts("raw per-vertex payloads: NaNs and subnormals retain all bits over 140 pixels");
    if(getenv("PS5C_TEST_PS_WAVES"))puts("pixel packet waves: four 32/64 mode switches and 140 RGBA pixels verified");
    vk_buffer_destroy(r,&out);renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);
    pal_window_destroy(win);gmem_free(&m);pal_shutdown();
    puts("mixed interpolation: 48 RGBA pixels verified (166 with per-vertex inputs, exact constant interpolation, programmable P1/P2, barycentric VGPRs and packed-color limits), LDS round-trip, unrelated SGPR bit and reused VS");return 0;
}
/* AMD POS_W_FLOAT is the reciprocal of Vulkan FragCoord.w.
 * W=1 hides a missing reciprocal; use two constants and a varying-W triangle,
 * and both sparse/full SPI input layouts. No interpolated attributes needed. */
static int gpu_position_w(void) {
    CHECK(pal_init()==PS5C_OK); gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&m,0,4*GUEST_PAGE_SIZE,GPROT_RW,0,"pixel W ABI fixture",&a)==PS5C_OK);
    pal_window *win=NULL;CHECK(pal_window_open("ZeraPS5 pixel W regression",320,180,&win)==PS5C_OK);
    renderer *r=renderer_create(win,&m);CHECK(r);gpu g;CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(a>>8);g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(a>>40);
    g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x300)>>8);g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)((a+0x300)>>40);
    g.regs[mmCB_COLOR0_BASE]=(uint32_t)((a+0x2000)>>8);
    g.regs[mmCB_COLOR0_INFO]=(14u<<2)|(7u<<8);g.regs[mmCB_COLOR0_ATTRIB2]=(6u<<14)|4u;
    g.regs[mmCB_COLOR_CONTROL]=0x10;g.regs[mmCB_TARGET_MASK]=15;
    g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_TRILIST;g.regs[mmPA_CL_VTE_CNTL]=15;
    g.regs[mmSPI_SHADER_PGM_RSRC2_GS]=0x00080000;
    g.regs[mmPA_CL_VPORT_XSCALE]=g.regs[mmPA_CL_VPORT_XOFFSET]=bits(3.5f);
    g.regs[mmPA_CL_VPORT_YSCALE]=g.regs[mmPA_CL_VPORT_YOFFSET]=bits(2.5f);
    gpu_draw draw={.count=3,.instances=1};vk_buffer out;
    CHECK(vk_buffer_create(r,7*5*16,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&out));
    for(unsigned pass=0;pass<6;pass++) {
        uint32_t code[96],n=0;
        for(unsigned i=0;i<PS5C_ARRAY_COUNT(vertex);i++) {
            if(vertex[i]==0xf80008cf) {
                if(pass%3==2) { /* clip W = vertex index +2 */
                    code[n++]=0x7e280d05;code[n++]=0x062828f4;
                } else {
                    code[n++]=0x7e2802ff;code[n++]=bits(pass%3?4.0f:2.0f);
                }
                code[n++]=0x10000114; /* X *= W */
                code[n++]=0x10020314; /* Y *= W */
                code[n++]=0x7e060314; /* clip W = v20 */
            }
            code[n++]=vertex[i];
        }
        uint32_t ps[]={0x7e000300,0x7e020280,0x7e040280,0x7e0602f2,0xf800080f,0x03020100,0xbf810000};
        unsigned wreg=pass<3?2:5;
        ps[0]+=wreg;
        g.regs[mmSPI_PS_INPUT_ADDR]=pass<3?0x802:0x1f02;
        CHECK(gmem_write(&m,a,code,n*4) && gmem_write(&m,a+0x300,ps,sizeof(ps)));
        renderer_submit_begin(r);renderer_draw(r,&g,&draw);renderer_wait_idle(r);
        vk_image *img=vk_image_find(r,a+0x2000,7,5,VK_FORMAT_R32G32B32A32_SFLOAT);CHECK(img && img->gpu_written);
        VkCommandBuffer cmd=vk_begin(r);
        vk_barrier(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy={0};copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;copy.imageSubresource.layerCount=1;
        copy.imageExtent=(VkExtent3D){7,5,1};vkCmdCopyImageToBuffer(cmd,img->image,img->layout,out.buffer,1,&copy);
        renderer_wait_idle(r);const float *pixels=out.mapped;
        for(unsigned y=0;y<5;y++)for(unsigned x=0;x<7;x++) {
            float i=(x+.5f)/14,j=(y+.5f)/10;
            float reciprocal=(1-i-j)/2+i/3+j/4;
            float expected=pass%3==2?1.0f/reciprocal:pass%3?4.0f:2.0f;
            unsigned o=4*(y*7+x);
            if(fabsf(pixels[o]-expected)>1e-6f)fprintf(stderr,"POS_W pass%u pixel%u,%u: %.9g expected %.9g\n",pass,x,y,pixels[o],expected);
            CHECK(fabsf(pixels[o]-expected)<1e-6f && pixels[o+1]==0 && pixels[o+2]==0 && pixels[o+3]==1);
        }
    }
    vk_buffer_destroy(r,&out);renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);
    pal_window_destroy(win);gmem_free(&m);pal_shutdown();
    puts("POS_W: 210 RGBA pixels, clip W2/W4/varying and sparse/full inputs verified");return 0;
}

int main(void) {
    CHECK(graphics_lds_register()==0);
    CHECK(long_program()==0);
    CHECK(program_window()==0);
    if(getenv("PS5C_TEST_GPU")) {
        if(getenv("PS5C_TEST_POS_W")) CHECK(gpu_position_w()==0);
        else CHECK(gpu_test()==0);
    }
    return 0;
}
