/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/shader/shader.h"
#include "gpu/vk/vk_internal.h"
#include "gpu/gpu_regs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static uint32_t bits(float f) { uint32_t v; memcpy(&v,&f,4); return v; }
static float real(uint32_t v) { float f; memcpy(&f,&v,4); return f; }
static uint16_t half(float f) {
    /* Fixtures use zero, exact small normal numbers and infinity/NaN. */
    uint32_t v=bits(f), sign=(v>>16)&0x8000, exponent=(v>>23)&255;
    if(exponent==255) return (uint16_t)(sign | 0x7c00 | ((v&0x7fffff)?0x200:0));
    return (uint16_t)(sign | (exponent ? ((exponent-112)<<10) | ((v>>13)&1023) : 0));
}
static unsigned program(uint32_t *code, bool wide, bool a16, bool nsa, bool inactive) {
    unsigned count=(wide?2:1)+4+(a16?3:6), n=0, first=nsa?30:0;
    for(unsigned i=0;i<count;i++) code[n++]=0x7e000200u | ((first+i)<<17) | (8+i);
    for(unsigned i=0;i<4;i++) code[n++]=0x7e000200u | ((12+i)<<17) | (20+i);
    if(inactive) code[n++]=0xbefe0480; /* s_mov_b64 exec, 0 */
    unsigned extra=nsa?(count-1+3)/4:0;
    code[n++]=(wide?0xf19c9f01u:0xf1989f01u) | (extra<<1);
    code[n++]=(a16?1u<<30:0) | (12u<<8) | first;
    for(unsigned i=0;i<extra;i++) {
        uint32_t w=0; for(unsigned b=0;b<4;b++) w|=(first+1+4*i+b)<<(8*b); code[n++]=w;
    }
    if(inactive) code[n++]=0xbefe04c1; /* s_mov_b64 exec, -1 */
    code[n++]=0xe0780000; code[n++]=0x80010c00; /* raw store XYZW to s4:7 */
    code[n++]=0xbf810000; return n;
}

static int gpu_test(void) {
    CHECK(pal_init()==PS5C_OK);
    gmem mem; gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&mem,&cfg)==PS5C_OK);
    guest_addr code_addr,node_addr,out_addr;
    CHECK(gmem_map(&mem,0,2*GUEST_PAGE_SIZE,GPROT_RW,0,"BVH code",&code_addr)==PS5C_OK);
    CHECK(gmem_map(&mem,0,GUEST_PAGE_SIZE,GPROT_RW,0,"BVH node",&node_addr)==PS5C_OK);
    CHECK(gmem_map(&mem,0,GUEST_PAGE_SIZE,GPROT_RW,0,"BVH results",&out_addr)==PS5C_OK);
    pal_window *window=NULL; CHECK(pal_window_open("ZeraPS5 BVH regression",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&mem);CHECK(r);
    gpu g;CHECK(gpu_init(&g,&mem,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    g.regs[mmCOMPUTE_PGM_RSRC2]=24u<<1;
    uint32_t *node=gmem_host(&mem,node_addr,128),*output=gmem_host(&mem,out_addr,16);
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    unsigned cases=0;
    const char *dense=getenv("PS5C_TEST_BVH_WAVE");
    if(dense) {
        bool wave32=atoi(dense)==32;
        /* Keep 64-KiB request pages separate from earlier GPU image fixtures. */
        guest_addr nodes,node_hint=(mem.res_start+0x10ffffull)&~0xffffull;
        CHECK(gmem_map(&mem,node_hint,8*GUEST_PAGE_SIZE,GPROT_RW,0,"BVH dense nodes",&nodes)==PS5C_OK);
        uint32_t *data=gmem_host(&mem,nodes,8*GUEST_PAGE_SIZE);CHECK(data);
        g.regs[mmCOMPUTE_NUM_THREAD_X]=128;
        g.regs[mmCOMPUTE_PGM_RSRC2]=24u<<1;
        g.regs[mmCOMPUTE_DISPATCH_INITIATOR]=wave32?0x8041:0x41;
        output=gmem_host(&mem,out_addr,2048);CHECK(output);
        for(unsigned sparse=0;sparse<2;sparse++) {
            uint32_t original[48],code[80];unsigned total=program(original,true,false,false,false),n=0;
            CHECK(total==21);
            code[n++]=0x7e3c0300; /* v30 = local invocation index, before ray arguments replace v0 */
            memcpy(code+n,original,16*4);n+=16;
            code[n++]=(0x1au<<25)|(31u<<17)|(30u<<9)|135u; /* v31 = tid << 7 */
            code[n++]=(0x25u<<25)|(31u<<9)|256u; /* v0 += v31: absolute node address += tid * 1024 */
            if(sparse) {
                code[n++]=0xbefe03ff;code[n++]=0xaaaaaaaau;
                code[n++]=0xbeff03ff;code[n++]=0x55555555u;
            }
            code[n++]=original[16];code[n++]=original[17];
            if(sparse)code[n++]=0xbefe04c1;
            code[n++]=0xe0782000;code[n++]=0x80010c1e; /* four result words at output[tid] */
            code[n++]=0xbf810000;
            guest_addr pc=code_addr+0x1c00+sparse*0x100;CHECK(gmem_write(&mem,pc,code,n*4));
            g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
            for(unsigned repeat=0;repeat<2;repeat++) {
                memset(data,0,8*GUEST_PAGE_SIZE);memset(u,0,24*4);memset(output,0xcd,2048);
                for(unsigned tid=0;tid<128;tid++)for(unsigned child=0;child<4;child++) {
                    uint32_t *record=data+tid*256;
                    record[child]=tid*100+repeat*10000+11*(child+1);
                    float bounds[6]={-1,-1,(float)(1+child*2),1,1,(float)(2+child*2)};
                    for(unsigned k=0;k<6;k++)record[4+child*6+k]=bits(bounds[k]);
                }
                u[1]=1u<<31;u[2]=UINT32_MAX;u[3]=0x810003ff;
                u[4]=(uint32_t)out_addr;u[5]=(uint32_t)(out_addr>>32)|(16u<<16);u[6]=128;u[7]=0x16204;
                uint64_t ptr=(nodes>>3)|5;u[8]=(uint32_t)ptr;u[9]=(uint32_t)(ptr>>32);u[10]=bits(100);
                u[16]=bits(1);u[17]=u[18]=bits(INFINITY);u[19]=bits(1);
                for(unsigned k=0;k<4;k++)u[20+k]=0xabcd0000+k;
                renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out_addr,2048);
                for(unsigned tid=0;tid<128;tid++)for(unsigned k=0;k<4;k++) {
                    bool active=!sparse || ((tid%64<32 || wave32)?(tid&1):!(tid&1));
                    uint32_t expected=active?tid*100+repeat*10000+11*(k+1):0xabcd0000+k;
                    if(output[tid*4+k]!=expected)fprintf(stderr,"dense BVH %u/%u lane%u word%u=%08x expected%08x\n",sparse,repeat,tid,k,output[tid*4+k],expected);
                    CHECK(output[tid*4+k]==expected);
                }
            }
        }
        puts("BVH dense GPU: 128 distinct nodes, sparse EXEC, multiple request pages and repeated updates verified");
    }
    g.regs[mmCOMPUTE_NUM_THREAD_X]=1;g.regs[mmCOMPUTE_DISPATCH_INITIATOR]=0x41;
    for(unsigned variant=0;variant<8;variant++) for(unsigned test=0;test<12;test++) {
        bool wide=variant&1,a16=variant&2,nsa=variant&4,compressed=test==2;
        memset(node,0,128);memset(u,0,24*4);
        for(unsigned i=0;i<4;i++) u[20+i]=0xabcd0000+i;
        u[0]=(uint32_t)(node_addr>>8);u[1]=(uint32_t)(node_addr>>40)|(1u<<31);
        u[2]=compressed?0:1;u[3]=0x80000000;
        u[4]=(uint32_t)out_addr;u[5]=(uint32_t)(out_addr>>32);u[6]=16;u[7]=0x00016204;
        unsigned ray=wide?2:1;
        u[8]=compressed?4:5;u[8+ray]=bits(test==3?2.0f:100.0f);
        for(unsigned a=0;a<3;a++) {
            u[8+ray+1+a]=bits(test==5 && a==0?NAN:0.0f);
            if(!a16) {u[8+ray+4+a]=bits(a==2?1.0f:0);u[8+ray+7+a]=bits(a==2?1.0f:INFINITY);}
        }
        if(a16) {
            const float packed[6]={0,0,1,INFINITY,INFINITY,1};
            for(unsigned k=0;k<3;k++) u[8+ray+4+k]=(uint32_t)half(packed[2*k])|((uint32_t)half(packed[2*k+1])<<16);
        }
        const float z[4]={9,1,5,3};
        for(unsigned child=0;child<4;child++) {
            node[child]=11*(child+1);
            float bounds[6]={-1,-1,z[child],1,1,z[child]+1};
            if(compressed) for(unsigned k=0;k<3;k++) node[4+3*child+k]=(uint32_t)half(bounds[2*k])|((uint32_t)half(bounds[2*k+1])<<16);
            else for(unsigned k=0;k<6;k++) node[4+6*child+k]=bits(bounds[k]);
        }
        uint32_t expected[4]={22,44,33,11};
        if(test==1) {u[1]&=~(1u<<31);for(unsigned i=0;i<4;i++) expected[i]=11*(i+1);}
        if(test==3) {expected[0]=22;expected[1]=expected[2]=expected[3]=UINT32_MAX;}
        if(test==4) {u[2]=0;for(unsigned i=0;i<4;i++) expected[i]=UINT32_MAX;}
        if(test==5) for(unsigned i=0;i<4;i++) expected[i]=UINT32_MAX;
        if(test==6) for(unsigned i=0;i<4;i++) expected[i]=0xabcd0000+i;
        if(test==7) u[3]=0; /* Invalid descriptor type must never read the node. */
        if(test==8) u[8]=6; /* Reserved node type. */
        if(test==9) {u[8]=0xfffffffdu;if(wide) u[9]=0xffffffffu;}
        if(test==10) u[8+ray]=bits(NAN);
        if(test==11) u[8+ray+1]=bits(INFINITY);
        if(test>=7) for(unsigned i=0;i<4;i++) expected[i]=UINT32_MAX;
        uint32_t code[48];unsigned words=program(code,wide,a16,nsa,test==6);
        guest_addr pc=code_addr+variant*0x100;
        CHECK(gmem_write(&mem,pc,code,words*4));
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
        for(unsigned i=0;i<4;i++) output[i]=0xdeadbeef;
        renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out_addr,16);
        for(unsigned i=0;i<4;i++) {
            if(output[i]!=expected[i]) fprintf(stderr,"box variant %u test %u word %u = %08x expected %08x\n",variant,test,i,output[i],expected[i]);
            CHECK(output[i]==expected[i]);
        }
        cases++;
    }
    for(unsigned kind=0;kind<4;kind++) for(unsigned bary=0;bary<2;bary++)
    for(unsigned axis=0;axis<3;axis++) for(unsigned backwards=0;backwards<2;backwards++) {
        memset(node,0,128);memset(u,0,24*4);
        const float vertices[5][3]={{0,0,1},{2,0,1},{2,2,1},{0,2,1},{-1,1,1}};
        for(unsigned v=0;v<5;v++) for(unsigned a=0;a<3;a++) node[3*v+(a+axis)%3]=bits(vertices[v][a]);
        node[15]=0x09090909;
        u[0]=(uint32_t)(node_addr>>8);u[1]=(uint32_t)(node_addr>>40);u[2]=0;u[3]=0x80000000|(bary<<24);
        u[4]=(uint32_t)out_addr;u[5]=(uint32_t)(out_addr>>32);u[6]=16;u[7]=0x00016204;
        const unsigned fan[4][3]={{0,1,2},{1,3,2},{2,3,4},{2,4,0}};
        u[8]=kind;u[9]=bits(100);
        for(unsigned a=0;a<3;a++) {
            float centroid=(vertices[fan[kind][0]][a]+vertices[fan[kind][1]][a]+vertices[fan[kind][2]][a])/3;
            unsigned rotated=(a+axis)%3;
            u[10+rotated]=bits(a==2?(backwards?2:0):centroid);
            u[13+rotated]=bits(a==2?(backwards?-1:1):0);
            u[16+rotated]=bits(a==2?(backwards?-1:1):INFINITY);
        }
        uint32_t code[48];unsigned words=program(code,false,false,false,false);guest_addr pc=code_addr+0x1000;
        CHECK(gmem_write(&mem,pc,code,words*4));
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
        for(unsigned i=0;i<4;i++) output[i]=0xdeadbeef;
        renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out_addr,16);
        CHECK(isfinite(real(output[0])) && fabsf(real(output[0])/real(output[1])-1)<0.00001f);
        if(!bary) CHECK(output[2]==0x09090909+kind && output[3]==1);
        else for(unsigned i=2;i<4;i++) CHECK(fabsf(real(output[i])/real(output[1])-1.0f/3)<0.00001f);
        cases++;
    }
    printf("BVH GPU: %u box/triangle cases verified\n",cases);
    if(getenv("PS5C_BVH_VA")) {
        CHECK(r->bvh_address);
        guest_addr split;
        CHECK(gmem_map(&mem,0,2*GUEST_PAGE_SIZE,GPROT_RW,0,"BVH split node",&split)==PS5C_OK);
        uint32_t *split_node=gmem_host(&mem,split+GUEST_PAGE_SIZE-64,128);
        for(unsigned test=0;test<7;test++) {
            memset(u,0,24*4);memset(node,0,128);
            for(unsigned child=0;child<4;child++) {
                node[child]=11*(child+1);
                float bounds[6]={-1,-1,(float)(1+child*2),1,1,(float)(2+child*2)};
                for(unsigned k=0;k<6;k++)node[4+6*child+k]=bits(bounds[k]);
            }
            guest_addr actual=test==3?split+GUEST_PAGE_SIZE-64:node_addr;
            if(test==3) {
                memcpy(split_node,node,128);
                CHECK(gmem_protect(&mem,split+GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,GPROT_READ)==PS5C_OK);
            }
            u[0]=0;u[1]=1u<<31;u[2]=UINT32_MAX;u[3]=0x810003ff;
            uint64_t pointer=(actual>>3)|5;
            if(test==2)pointer=(GUEST_SPACE_END>>3)|5; /* no mapped page */
            if(test==4)pointer=UINT64_MAX-2; /* index outside 42-bit descriptor bound */
            if(test==5)u[3]=0; /* invalid descriptor, zero physical loads */
            if(test==6) {u[0]=(uint32_t)(actual>>8);u[1]|=(uint32_t)(actual>>40);pointer=5;}
            u[4]=(uint32_t)out_addr;u[5]=(uint32_t)(out_addr>>32);u[6]=16;u[7]=0x00016204;
            u[8]=(uint32_t)pointer;u[9]=(uint32_t)(pointer>>32);u[10]=bits(100);
            u[16]=bits(1);u[17]=u[18]=bits(INFINITY);u[19]=bits(1);
            uint32_t code[64];unsigned n=0;
            /* Runtime VGPR -> SGPR descriptor: host scalar evaluation cannot
             * substitute a snapshot for these four live shader values. */
            if(test==1 || test==6)for(unsigned k=0;k<4;k++) {
                code[n++]=0x7e000200u|((40+k)<<17)|k;
                code[n++]=0x7e000500u|(k<<17)|(40+k);
            }
            n+=program(code+n,true,false,false,false);
            guest_addr pc=code_addr+0x1200+test*0x100;
            CHECK(gmem_write(&mem,pc,code,n*4));
            if(test==1 || test==6) {
                sh_program *p=calloc(1,sizeof(*p));sh_eval eval={0};
                CHECK(p && sh_analyze(code,n,pc,p));memcpy(eval.sgpr_init,u,24*4);
                CHECK(sh_evaluate(p,&mem,&eval));bool unknown=false;
                for(uint32_t site=0;site<p->site_count;site++)
                    if(p->sites[site].kind==SH_SITE_BVH)unknown|=eval.site[site].desc_unknown;
                CHECK(unknown);sh_eval_free(&eval);free(p);
            }
            g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
            for(unsigned i=0;i<4;i++)output[i]=0xdeadbeef;
            renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out_addr,16);
            for(unsigned i=0;i<4;i++) {
                uint32_t want=(test==2 || test==4 || test==5)?UINT32_MAX:11*(i+1);
                if(output[i]!=want)fprintf(stderr,"VA test %u word %u = %08x want %08x\n",test,i,output[i],want);
                CHECK(output[i]==want);
            }
        }
        puts("BVH dynamic GPU: 7 absolute/runtime/unmapped/split/invalid cases verified");
    }
    if(getenv("PS5C_BVH_PAGED")) {
        guest_addr image_addr;
        CHECK(gmem_map(&mem,0,GUEST_PAGE_SIZE,GPROT_RW,0,"BVH replay image",&image_addr)==PS5C_OK);
        uint32_t code[64];unsigned words=0;
        code[words++]=0x7e3c0280;
        code[words++]=0xe0300000;code[words++]=0x8001151e; /* v21 = previous output[0] */
        code[words++]=0xbf8c3f70;
        words+=program(code+words,true,false,false,false)-1;
        code[words++]=0x7e3c0280;code[words++]=0x7e3e0280; /* coordinates v30,v31=0 */
        code[words++]=0xf0000108;code[words++]=0x0005141e; /* image_load v20, v[30:31], s[20:27] */
        code[words++]=0xbf8c3f70;
        code[words++]=0x4a28290c; /* v20 += BVH result v12 */
        code[words++]=0xf0200108;code[words++]=0x0005141e;
        code[words++]=0x4a182b0c; /* v12 += previous buffer value v21 */
        code[words++]=0xe0700000;code[words++]=0x80010c1e;
        code[words++]=0xbf810000;
        guest_addr pc=code_addr+0x1a00;CHECK(gmem_write(&mem,pc,code,words*4));
        g.regs[mmCOMPUTE_PGM_RSRC2]=28u<<1;
        memset(node,0,128);
        for(unsigned child=0;child<4;child++) {
            node[child]=11*(child+1);
            float bounds[6]={-1,-1,(float)(1+child*2),1,1,(float)(2+child*2)};
            for(unsigned k=0;k<6;k++)node[4+child*6+k]=bits(bounds[k]);
        }
        for(unsigned round=0;round<2;round++) {
            memset(u,0,28*4);u[1]=1u<<31;u[2]=UINT32_MAX;u[3]=0x810003ff;
            u[4]=(uint32_t)out_addr;u[5]=(uint32_t)(out_addr>>32);u[6]=16;u[7]=0x00016204;
            uint64_t ptr=(node_addr>>3)|5;u[8]=(uint32_t)ptr;u[9]=(uint32_t)(ptr>>32);u[10]=bits(100);
            u[16]=bits(1);u[17]=u[18]=bits(INFINITY);u[19]=bits(1);
            u[20]=(uint32_t)(image_addr>>8);u[21]=(uint32_t)(image_addr>>40)|(20u<<20)|(3u<<30);
            u[22]=3u|(15u<<14);u[23]=(9u<<28)|0xfacu;
            vk_begin(r);vk_image *image=vk_image_get(r,image_addr,16,16,VK_FORMAT_R32_UINT);CHECK(image);
            vk_barrier(r->cmd,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkClearColorValue clear={.uint32={5+2*round,0,0,0}};
            VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdClearColorImage(r->cmd,image->image,image->layout,&clear,1,&range);
            image->gpu_written=true;image->written_seq=++r->write_seq;
            output[0]=5+2*round;
            g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
            renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out_addr,16);
            vk_buffer readback={0};CHECK(vk_buffer_create(r,4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&readback));
            vk_begin(r);vk_barrier(r->cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={1,1,1}};
            vkCmdCopyImageToBuffer(r->cmd,image->image,image->layout,readback.buffer,1,&copy);
            vk_flush(r);CHECK(vkWaitForFences(r->device,1,&r->work_fence,VK_TRUE,UINT64_MAX)==VK_SUCCESS);
            uint32_t actual=*(uint32_t*)readback.mapped;
            if(actual!=16+2*round)fprintf(stderr,"BVH image transaction round %u = %u expected %u\n",round,actual,16+2*round);
            CHECK(actual==16+2*round && output[0]==16+2*round && !image->feedback);vk_buffer_destroy(r,&readback);
        }
        puts("BVH paged GPU: 2 image/buffer rollback transactions verified");
    }
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&mem);pal_shutdown();return 0;
}

int main(void) {
    sh_program *p = calloc(1,sizeof(*p)); CHECK(p);
    const uint32_t code[] = {0xf1989f01,0x00020c00,0xbf810000};
    CHECK(sh_analyze(code,3,0,p));
    CHECK(p->count==2 && p->site_count==1);
    CHECK(p->sites[0].kind==SH_SITE_BVH);
    sh_eval e={0};for(unsigned i=0;i<4;i++) e.sgpr_init[8+i]=0x12340000+i;
    CHECK(sh_evaluate(p,NULL,&e)); CHECK(e.site[0].valid && !e.site[0].desc_unknown);
    unsigned at=e.smem_data[SH_U_SITE];
    for(unsigned i=0;i<4;i++) CHECK(e.site[0].desc[i]==e.sgpr_init[8+i] && e.smem_data[at+i]==e.sgpr_init[8+i]);
    sh_stage_info info={.stage=SH_STAGE_CS,.local_size={1,1,1}};sh_site_shape shapes[SH_MAX_SITES]={0};
    uint32_t *spv=NULL;size_t words; CHECK(sh_translate(p,&info,shapes,0,&spv,&words));
    if(getenv("PS5C_TEST_SPIRV")) {char path[512];snprintf(path,sizeof(path),"%s/bvh.spv",getenv("PS5C_TEST_SPIRV"));FILE *f=fopen(path,"wb");CHECK(f && fwrite(spv,4,words,f)==words && !fclose(f));}
    free(spv);sh_eval_free(&e);free(p); puts("BVH analysis, descriptor and SPIR-V: ok");
    for(unsigned mode=0;mode<3;mode++) for(unsigned variant=0;variant<8;variant++) {
        uint32_t fixture[48];unsigned n=program(fixture,variant&1,variant&2,variant&4,false);
        p=calloc(1,sizeof(*p));CHECK(p && sh_analyze(fixture,n,0,p));
        for(unsigned site=0;site<p->site_count;site++)
            if(p->sites[site].kind==SH_SITE_BVH)shapes[site].address_map=mode?mode+2:0;
        CHECK(sh_translate(p,&info,shapes,0,&spv,&words));
        if(getenv("PS5C_TEST_SPIRV")) {
            char path[512];snprintf(path,sizeof(path),"%s/bvh-%u-%u.spv",getenv("PS5C_TEST_SPIRV"),mode,variant);
            FILE *f=fopen(path,"wb");CHECK(f && fwrite(spv,4,words,f)==words && !fclose(f));
        }
        free(spv);free(p);
    }
    return getenv("PS5C_TEST_GPU")?gpu_test():0;
}
