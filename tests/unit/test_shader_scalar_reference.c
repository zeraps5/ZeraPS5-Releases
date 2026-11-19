/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/shader/shader.h"
#include "gpu/shader/shader_ops.h"
#include "gpu/program_storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static const char *const op_names[]={
#define X(id,name) name,
SH_OPS(X)
#undef X
};


static void init(sh_program *p,unsigned count) {
    memset(p,0,sizeof(*p));p->count=count;p->hash=0x1234;p->iflags[0]=SH_IF_ANALYZED;
    for(unsigned i=0;i<count;i++){p->site_of[i]=-1;p->target[i]=(uint16_t)count;}
}

static void inst(sh_program *p,unsigned i,sh_op op,rdna2_enc enc,unsigned dst,unsigned a,unsigned b) {
    p->ops[i]=(uint16_t)op;p->insts[i].enc=enc;p->insts[i].dst=(uint16_t)dst;
    p->insts[i].src[0]=(uint16_t)a;p->insts[i].src[1]=(uint16_t)b;
}

static void finish_fixture(sh_program *p) {
    for(unsigned i=0;i<p->count;i++) {
        p->insts[i].name=op_names[p->ops[i]];p->insts[i].words=1;
        p->pc[i]=i;p->next_scalar[i]=(uint16_t)i;
        if(p->ops[i]==SH_OP_S_BRANCH || (p->ops[i]>=SH_OP_S_CBRANCH_SCC0 && p->ops[i]<=SH_OP_S_CBRANCH_EXECNZ))
            p->insts[i].simm16=(int32_t)p->target[i]-(int32_t)i-1;
    }
    p->next_scalar[p->count]=(uint16_t)p->count;sh_program_flags(p);
}

static unsigned spill(sh_program *p,unsigned at,unsigned vgpr,unsigned lane,unsigned scalar) {
    inst(p,at,SH_OP_V_WRITELANE_B32,RDNA2_ENC_VOP3,256+vgpr,scalar,RDNA2_OP_ZERO+lane);
    p->insts[at].valu=0x361;return at+1;
}

static unsigned spill_site(sh_program *p,unsigned at,unsigned vgpr,unsigned lane,unsigned site) {
    inst(p,at,SH_OP_V_READLANE_B32,RDNA2_ENC_VOP3,268,256+vgpr,RDNA2_OP_ZERO+lane);p->insts[at].valu=0x360;
    inst(p,at+1,SH_OP_BUFFER_STORE_DWORD,RDNA2_ENC_MUBUF,0,0,0);p->insts[at+1].srsrc=12;p->insts[at+1].soffset=RDNA2_OP_ZERO;
    p->site_of[at+1]=(int16_t)site;p->sites[site]=(sh_site){.inst=at+1,.kind=SH_SITE_BUFFER,.store=true};
    if(p->site_count<site+1)p->site_count=site+1;
    return at+2;
}

typedef struct fixture_memory {unsigned calls;bool fail,pending,split;uint32_t words[128];} fixture_memory;

static void *read_fixture(void *user,guest_addr address,uint64_t size) {
    fixture_memory *m=user;m->calls++;
    if(m->fail || (m->split && size>4) || address<0x10000 || size>sizeof(m->words) || address-0x10000>sizeof(m->words)-size)return NULL;
    return (char *)m->words+(size_t)(address-0x10000);
}

static bool pending_fixture(void *user,guest_addr address,uint64_t size) {
    (void)address;(void)size;return ((fixture_memory *)user)->pending;
}

static int reference_spill_overwrite(bool include_first) {
    /* Real decoder input, with no manually forced lane_reads flag:
     * writelane v90,s8,2; mov v90,s10; readlane s12,v90,2; store V#s12. */
    uint32_t code[10];unsigned n=0;
    code[n++]=0xd761005au;code[n++]=8u|((128u+2u)<<9);
    code[n++]=0x7e000200u|(90u<<17)|10u;
    code[n++]=0xd760000cu;code[n++]=(256u+90u)|((128u+2u)<<9);
    if(include_first)code[n++]=0x7e000000u|(20u<<17)|(2u<<9)|(256u+90u);
    code[n++]=0xe0700000u;code[n++]=0x80030000u;code[n++]=0xbf810000u;
    sh_program *p=calloc(1,sizeof(*p));sh_eval *out=calloc(1,sizeof(*out));CHECK(p && out);
    CHECK(sh_analyze(code,n,0x900000000ull,p));CHECK(p->ops[0]==SH_OP_V_WRITELANE_B32);
    CHECK(p->ops[1]==SH_OP_V_MOV_B32 && p->ops[2]==SH_OP_V_READLANE_B32);
    CHECK(p->lane_reads && p->site_count==1);
    for(unsigned wave=0;wave<2;wave++) {
        p->wave32=wave!=0;memset(out->sgpr_init,0,sizeof(out->sgpr_init));
        out->sgpr_init[8]=0x12340000;out->sgpr_init[10]=0x34560000;
        out->sgpr_init[126]=out->sgpr_init[127]=UINT32_MAX;
        CHECK(sh_evaluate(p,NULL,out));CHECK(out->site[0].valid && !out->site[0].desc_unknown);
        if(out->site[0].desc[0]!=out->sgpr_init[10])fprintf(stderr,"reference spill: expected %08x actual %08x firstlane %u wave32 %u\n",out->sgpr_init[10],out->site[0].desc[0],include_first,wave);
        CHECK(out->site[0].desc[0]==out->sgpr_init[10]);
    }
    sh_eval_free(out);free(out);free(p);return 0;
}

static void reference_finish(sh_program *p,unsigned count) {
    p->count=count;p->lane_reads=true;finish_fixture(p);
    /* Model the skipped VALU interval used by the actual decoder. The raw
     * tests above separately verify that decoder/gate path. */
    for(unsigned i=count;i-->0;)if(p->ops[i]==SH_OP_V_MOV_B32 || p->ops[i]==SH_OP_V_SWAP_B32 || p->ops[i]==SH_OP_V_MOVRELD_B32 ||
        p->ops[i]==SH_OP_V_MOVRELS_B32 || p->ops[i]==SH_OP_V_MOVRELSD_B32 || p->ops[i]==SH_OP_V_NOP)
        p->next_scalar[i]=p->next_scalar[i+1];
}

static int reference_spill_matrix(void) {
    sh_program *p=calloc(1,sizeof(*p));sh_eval *out=calloc(1,sizeof(*out));CHECK(p && out);
    for(unsigned wave=0;wave<2;wave++)for(unsigned kind=0;kind<11;kind++) {
        init(p,32);p->wave32=wave!=0;unsigned at=0;
        at=spill(p,at,90,2,8);at=spill(p,at,90,3,9);at=spill(p,at,89,2,10);at=spill(p,at,91,2,11);
        if(kind==3)inst(p,at++,SH_OP_V_CMPX,RDNA2_ENC_VOPC,106,256,257);
        if(kind<=3 || kind==10) {
            inst(p,at,SH_OP_V_MOV_B32,RDNA2_ENC_VOP1,346,kind==10?346:10,0);p->insts[at++].valu=0x180;
        } else if(kind==4)inst(p,at++,SH_OP_V_ADD_F32,RDNA2_ENC_VOP2,346,256,257);
        else if(kind<=8) {
            bool store=kind==8,global=kind==7;unsigned i=at++;
            inst(p,i,global?SH_OP_GLOBAL_LOAD:store?SH_OP_BUFFER_STORE_DWORD:SH_OP_BUFFER_LOAD_DWORD,
                global?RDNA2_ENC_FLAT:RDNA2_ENC_MUBUF,global?346:0,0,0);
            p->insts[i].vdata=(kind==5 || global)?89:90;p->insts[i].srsrc=0;p->insts[i].soffset=RDNA2_OP_ZERO;
            p->insts[i].seg=global?2:0;p->site_of[i]=0;p->site_count=1;
            p->sites[0]=(sh_site){.inst=i,.kind=SH_SITE_BUFFER,.store=store};
        } else {inst(p,at,SH_OP_V_SWAP_B32,RDNA2_ENC_VOP1,347,346,0);at++;}
        unsigned first_site=p->site_count;
        at=spill_site(p,at,90,2,first_site);at=spill_site(p,at,90,3,first_site+1);
        at=spill_site(p,at,89,2,first_site+2);at=spill_site(p,at,91,2,first_site+3);
        inst(p,at++,SH_OP_S_ENDPGM,RDNA2_ENC_SOPP,0,0,0);reference_finish(p,at);
        if(kind==7){p->insts[4].name="global_load_dword";sh_program_flags(p);CHECK(p->vmem_width[4]==1);}
        memset(out->sgpr_init,0,sizeof(out->sgpr_init));
        out->sgpr_init[8]=0x12340000;out->sgpr_init[9]=0x23450000;out->sgpr_init[10]=0x34560000;out->sgpr_init[11]=0x45670000;
        out->sgpr_init[126]=out->sgpr_init[127]=UINT32_MAX;
        if(kind==1)out->sgpr_init[126]=out->sgpr_init[127]=0;
        if(kind==2){out->sgpr_init[126]=1u<<3;out->sgpr_init[127]=0;}
        CHECK(sh_evaluate(p,NULL,out));
        bool unknown2=kind==3 || kind==4 || kind==6 || kind==7 || kind==9;
        bool unknown3=unknown2 || kind==2;
        const sh_site_value *a=&out->site[first_site],*b=&out->site[first_site+1];
        if(a->desc_unknown!=unknown2 || b->desc_unknown!=unknown3)fprintf(stderr,"spill matrix wave%u kind%u: unknown %u/%u expected %u/%u\n",wave,kind,a->desc_unknown,b->desc_unknown,unknown2,unknown3);
        CHECK(a->valid && b->valid && a->desc_unknown==unknown2 && b->desc_unknown==unknown3);
        if(!unknown2)CHECK(a->desc[0]==out->sgpr_init[kind==0?10:8]);
        if(!unknown3)CHECK(b->desc[0]==out->sgpr_init[kind==0?10:9]);
        CHECK(out->site[first_site+2].desc_unknown==(kind==5));
        if(kind!=5)CHECK(out->site[first_site+2].desc[0]==out->sgpr_init[10]);
        if(out->site[first_site+3].desc_unknown!=(kind==9))fprintf(stderr,"neighbor91 wave%u kind%u unknown%u width%u\n",wave,kind,out->site[first_site+3].desc_unknown,p->vmem_width[4]);
        CHECK(out->site[first_site+3].desc_unknown==(kind==9));
        if(kind!=9)CHECK(out->site[first_site+3].desc[0]==out->sgpr_init[11]);
    }
    sh_eval_free(out);free(out);free(p);puts("reference spill matrix: EXEC, opaque writes, VMEM destinations/neighbors, store, swap and self-copy passed");return 0;
}

static int reference_spill_deferred(void) {
    sh_program *p=calloc(1,sizeof(*p));sh_eval *out=calloc(1,sizeof(*out));CHECK(p && out);
    fixture_memory memory={.pending=true};memory.words[0]=0x12340000;
    for(unsigned kind=0;kind<19;kind++) {
        init(p,16);unsigned at=0;
        inst(p,at,SH_OP_S_BUFFER_LOAD_DWORD,RDNA2_ENC_SMEM,8,0,0);p->insts[at].sbase=0;p->insts[at].soffset=RDNA2_OP_NULL;
        p->site_of[at]=0;p->site_count=1;p->sites[0]=(sh_site){.inst=at,.kind=SH_SITE_SBUFFER};at++;
        if(kind==18)at=spill(p,at,92,0,10);
        at=spill(p,at,kind==14?0:90,0,8);
        if(kind<=1 || kind==13) {
            unsigned dst=kind?380:276; /* M0 or s20 */
            inst(p,at,SH_OP_V_READLANE_B32,RDNA2_ENC_VOP3,dst,456,RDNA2_OP_ZERO);p->insts[at++].valu=0x360;
            if(!kind || kind==13)at=spill(p,at,kind==13?92:90,0,kind==13?8:10),p->insts[at-1].src[1]=20;
            else inst(p,at++,SH_OP_V_MOVRELD_B32,RDNA2_ENC_VOP1,256,10,0);
        } else if(kind<=3) {
            inst(p,at,SH_OP_V_MOV_B32,RDNA2_ENC_VOP1,346,kind==2?RDNA2_OP_SDWA:RDNA2_OP_DPP8,0);
            p->insts[at].valu=0x180;
            if(kind==2)p->insts[at].dpp_sdwa=10u|(2u<<11)|(6u<<16)|(1u<<23);
            at++;
        } else if(kind==4) {
            unsigned i=at++;inst(p,i,SH_OP_BUFFER_LOAD_USHORT,RDNA2_ENC_MUBUF,0,0,0);
            p->insts[i].vdata=90;p->insts[i].srsrc=0;p->insts[i].soffset=RDNA2_OP_ZERO;
            p->site_of[i]=1;p->site_count=2;p->sites[1]=(sh_site){.inst=i,.kind=SH_SITE_BUFFER};
        } else if(kind==5 || kind==12)inst(p,at++,SH_OP_V_SWAP_B32,RDNA2_ENC_VOP1,347,346,0);
        else if(kind==14) {
            inst(p,at++,SH_OP_V_NOP,RDNA2_ENC_VOP1,256,0,0);
            inst(p,at,SH_OP_V_MOV_B32,RDNA2_ENC_VOP1,347,256,0);p->insts[at++].valu=0x180;
        } else if(kind==18) {
            inst(p,at,SH_OP_V_MOV_B32,RDNA2_ENC_VOP1,347,346,0);p->insts[at++].valu=0x180;
            inst(p,at,SH_OP_V_MOV_B32,RDNA2_ENC_VOP1,348,347,0);p->insts[at++].valu=0x180;
        } else if(kind>=15) {
            if(kind==17){inst(p,at,SH_OP_V_READLANE_B32,RDNA2_ENC_VOP3,380,456,RDNA2_OP_ZERO);p->insts[at++].valu=0x360;}
            else {
                inst(p,at,SH_OP_S_MOV_B32,RDNA2_ENC_SOP1,124,RDNA2_OP_LITERAL,0);
                p->insts[at++].literal=90;
            }
            inst(p,at++,kind==16?SH_OP_V_MOVRELSD_B32:SH_OP_V_MOVRELS_B32,RDNA2_ENC_VOP1,kind==16?256:347,256,0);
        }
        else if(kind==11) {
            inst(p,at,SH_OP_V_READLANE_B32,RDNA2_ENC_VOP3,380,456,RDNA2_OP_ZERO);p->insts[at++].valu=0x360;
            inst(p,at++,SH_OP_V_MOVRELD_B32,RDNA2_ENC_VOP1,256,8,0);
        } else {
            inst(p,at,SH_OP_V_MOV_B32,RDNA2_ENC_VOP1,347,346,0);p->insts[at++].valu=0x180;
            if(kind==7){inst(p,at,SH_OP_V_MOV_B32,RDNA2_ENC_VOP1,348,347,0);p->insts[at++].valu=0x180;}
            if(kind==8) {
                inst(p,at++,SH_OP_S_MOV_B64,RDNA2_ENC_SOP1,126,RDNA2_OP_ZERO,0);
                inst(p,at,SH_OP_V_MOV_B32,RDNA2_ENC_VOP1,347,10,0);p->insts[at++].valu=0x180;
            }
            if(kind==9) {
                inst(p,at,SH_OP_V_MOV_B32,RDNA2_ENC_VOP1,347,RDNA2_OP_DPP8,0);p->insts[at++].valu=0x180;
            }
            if(kind==10) {
                unsigned i=at++;inst(p,i,SH_OP_BUFFER_LOAD_USHORT,RDNA2_ENC_MUBUF,0,0,0);
                p->insts[i].vdata=91;p->insts[i].srsrc=0;p->insts[i].soffset=RDNA2_OP_ZERO;
                p->site_of[i]=1;p->site_count=2;p->sites[1]=(sh_site){.inst=i,.kind=SH_SITE_BUFFER};
            }
        }
        unsigned result_vgpr=kind==16?90:kind==7 || kind==11 || kind==13 || kind==18?92:kind>=6?91:90;
        unsigned site=p->site_count;at=spill_site(p,at,result_vgpr,0,site);inst(p,at++,SH_OP_S_ENDPGM,RDNA2_ENC_SOPP,0,0,0);reference_finish(p,at);
        if(kind==4){p->insts[2].name="buffer_load_short_d16_hi";p->vmem_width[2]=1;}
        out->host=read_fixture;out->host_user=&memory;out->defer=pending_fixture;
        memset(out->sgpr_init,0,sizeof(out->sgpr_init));out->sgpr_init[0]=0x10000;out->sgpr_init[2]=sizeof(memory.words);
        out->sgpr_init[10]=0x34560000;out->sgpr_init[126]=out->sgpr_init[127]=UINT32_MAX;
        CHECK(sh_evaluate(p,NULL,out));
        if(!out->guessed)fprintf(stderr,"lost deferred spill dependency kind%u\n",kind);
        CHECK(out->deferred && out->guessed && out->site[site].desc_unknown);
    }
    sh_eval_free(out);free(out);free(p);puts("reference spill deferred dependencies: unknown indices/M0, partial writes and swap passed");return 0;
}

static int reference_spill_indices(void) {
    sh_program *p=calloc(1,sizeof(*p));sh_eval *out=calloc(1,sizeof(*out));CHECK(p && out);
    for(unsigned wave=0;wave<2;wave++) {
        init(p,12);p->wave32=wave!=0;unsigned at=0;
        at=spill(p,at,90,2,8);at=spill(p,at,90,34,9);
        at=spill_site(p,at,90,2,0);at=spill_site(p,at,90,34,1);
        inst(p,at++,SH_OP_S_ENDPGM,RDNA2_ENC_SOPP,0,0,0);reference_finish(p,at);
        memset(out->sgpr_init,0,sizeof(out->sgpr_init));out->sgpr_init[8]=0x12340000;out->sgpr_init[9]=0x23450000;
        out->sgpr_init[126]=out->sgpr_init[127]=UINT32_MAX;
        CHECK(sh_evaluate(p,NULL,out));
        CHECK(!out->site[0].desc_unknown && !out->site[1].desc_unknown);
        CHECK(out->site[0].desc[0]==out->sgpr_init[wave?9:8]);
        CHECK(out->site[1].desc[0]==out->sgpr_init[9]);
    }
    fixture_memory memory={0};memory.words[0]=0x12340000;
    const unsigned pseudo[]={RDNA2_OP_SCC,RDNA2_OP_VCCZ,RDNA2_OP_EXECZ};
    for(unsigned pending=0;pending<2;pending++)for(unsigned flag=0;flag<3;flag++)for(unsigned mode=0;mode<3;mode++) {
        init(p,16);unsigned at=0;memory.pending=pending!=0;
        inst(p,at,SH_OP_S_BUFFER_LOAD_DWORD,RDNA2_ENC_SMEM,8,0,0);p->insts[at].sbase=0;p->insts[at].soffset=RDNA2_OP_NULL;
        p->site_of[at]=0;p->site_count=1;p->sites[0]=(sh_site){.inst=at,.kind=SH_SITE_SBUFFER};at++;
        at=spill(p,at,90,0,8);at=spill(p,at,92,0,10);at=spill(p,at,92,1,11);
        if(!flag) {
            inst(p,at,SH_OP_V_READLANE_B32,RDNA2_ENC_VOP3,276,456,RDNA2_OP_ZERO);p->insts[at++].valu=0x360;
            inst(p,at++,SH_OP_S_CMP_EQ_U32,RDNA2_ENC_SOPC,0,pending?8:20,RDNA2_OP_ZERO);
        } else inst(p,at++,flag==1?SH_OP_V_CMP:SH_OP_V_CMPX,RDNA2_ENC_VOPC,106,346,RDNA2_OP_ZERO);
        if(mode) {
            at=spill(p,at,92,0,mode==2?pseudo[flag]:10);
            if(mode==1)p->insts[at-1].src[1]=(uint16_t)pseudo[flag];
        }
        unsigned read=at,site=p->site_count;at=spill_site(p,at,92,0,site);
        if(!mode)p->insts[read].src[1]=(uint16_t)pseudo[flag];
        inst(p,at++,SH_OP_S_ENDPGM,RDNA2_ENC_SOPP,0,0,0);reference_finish(p,at);
        out->host=read_fixture;out->host_user=&memory;out->defer=pending_fixture;
        memset(out->sgpr_init,0,sizeof(out->sgpr_init));out->sgpr_init[0]=0x10000;out->sgpr_init[2]=sizeof(memory.words);
        out->sgpr_init[10]=0x34560000;out->sgpr_init[11]=0x45670000;out->sgpr_init[126]=out->sgpr_init[127]=UINT32_MAX;
        CHECK(sh_evaluate(p,NULL,out));
        if(!out->site[site].desc_unknown || out->guessed!=(pending!=0))fprintf(stderr,"spill pseudo pending%u flag%u mode%u unknown%u guessed%u\n",pending,flag,mode,out->site[site].desc_unknown,out->guessed);
        CHECK(out->site[site].desc_unknown && out->guessed==(pending!=0));
    }
    sh_eval_free(out);free(out);free(p);puts("reference spill indices: wave32 aliases and unknown/deferred SCC, VCCZ, EXECZ passed");return 0;
}

static int reference_global_destination(void) {
    const uint32_t code[]={
        0xd761005au,8u|((128u+2u)<<9),0xd7610059u,10u|((128u+2u)<<9),
        0xdc308000u,(90u<<24)|(89u<<8),
        0xd760000cu,(256u+90u)|((128u+2u)<<9),0xe0700000u,0x80030000u,
        0xd7600010u,(256u+89u)|((128u+2u)<<9),0xe0700000u,0x80040000u,0xbf810000u};
    sh_program *p=calloc(1,sizeof(*p));sh_eval *out=calloc(1,sizeof(*out));CHECK(p && out);
    CHECK(sh_analyze(code,PS5C_ARRAY_COUNT(code),0x900000000ull,p));
    CHECK(p->ops[2]==SH_OP_GLOBAL_LOAD && p->insts[2].dst==346 && p->insts[2].vdata==89 && p->vmem_width[2]==1);
    CHECK(p->lane_reads && p->site_count==3);
    out->sgpr_init[8]=0x12340000;out->sgpr_init[10]=0x34560000;out->sgpr_init[126]=out->sgpr_init[127]=UINT32_MAX;
    CHECK(sh_evaluate(p,NULL,out));CHECK(out->site[1].desc_unknown);
    CHECK(out->site[2].valid && !out->site[2].desc_unknown && out->site[2].desc[0]==out->sgpr_init[10]);
    sh_eval_free(out);free(out);free(p);return 0;
}

static int reference_wqm(void) {
    sh_program *p=calloc(1,sizeof(*p));sh_eval *e=calloc(1,sizeof(*e));CHECK(p && e);
    const uint64_t input[]={0,1,8,0x10,0x100000000ull,0x8000000000000000ull,0x1000000100000001ull,UINT64_MAX};
    const uint64_t expected[]={0,15,15,0xf0,0xf00000000ull,0xf000000000000000ull,0xf000000f0000000full,UINT64_MAX};
    for(unsigned wide=0;wide<2;wide++) {
        init(p,4);inst(p,0,wide?SH_OP_S_WQM_B64:SH_OP_S_WQM_B32,RDNA2_ENC_SOP1,12,8,0);
        inst(p,1,SH_OP_S_ADDC_U32,RDNA2_ENC_SOP2,14,RDNA2_OP_ZERO,RDNA2_OP_ZERO);
        inst(p,2,SH_OP_BUFFER_STORE_DWORD,RDNA2_ENC_MUBUF,0,0,0);p->insts[2].srsrc=12;p->insts[2].soffset=RDNA2_OP_ZERO;
        p->site_count=1;p->site_of[2]=0;p->sites[0]=(sh_site){.inst=2,.kind=SH_SITE_BUFFER,.store=true};
        inst(p,3,SH_OP_S_ENDPGM,RDNA2_ENC_SOPP,0,0,0);finish_fixture(p);
        for(unsigned i=0;i<PS5C_ARRAY_COUNT(input);i++) {
            memset(e->sgpr_init,0,sizeof(e->sgpr_init));e->sgpr_init[8]=(uint32_t)input[i];e->sgpr_init[9]=(uint32_t)(input[i]>>32);
            CHECK(sh_evaluate(p,NULL,e));uint64_t v=wide?expected[i]:(uint32_t)expected[i];
            CHECK(e->site[0].desc[0]==(uint32_t)v && e->site[0].desc[1]==(uint32_t)(v>>32) && e->site[0].desc[2]==(v!=0));
        }
    }
    sh_eval_free(e);free(e);free(p);return 0;
}
static int reference_fork_capacity(void) {
    sh_program *p=calloc(1,sizeof(*p));sh_eval *e=calloc(1,sizeof(*e));CHECK(p && e);
    for(unsigned variant=0;variant<2;variant++) {
        unsigned arms=variant?513:40;init(p,arms*2+2);
        inst(p,0,SH_OP_S_CMP_EQ_U32,RDNA2_ENC_SOPC,0,8,8);
        for(unsigned k=0;k<arms;k++) {
            unsigned i=k*2+1;inst(p,i,SH_OP_S_CBRANCH_SCC1,RDNA2_ENC_SOPP,0,0,0);p->target[i]=(uint16_t)(i+2);
            inst(p,i+1,SH_OP_BUFFER_STORE_DWORD,RDNA2_ENC_MUBUF,0,0,0);p->insts[i+1].srsrc=0;p->insts[i+1].soffset=RDNA2_OP_ZERO;
            p->site_of[i+1]=(int16_t)k;p->sites[k]=(sh_site){.inst=i+1,.kind=SH_SITE_BUFFER,.store=true};
        }
        p->site_count=arms;inst(p,arms*2+1,SH_OP_S_ENDPGM,RDNA2_ENC_SOPP,0,0,0);finish_fixture(p);
        if(variant)CHECK(!sh_evaluate(p,NULL,e));
        else {
            CHECK(sh_evaluate(p,NULL,e));for(unsigned k=0;k<arms;k++)CHECK(e->site[k].valid);
            sh_program *clone=program_storage_alloc(true);CHECK(clone && program_storage_copy(clone,p,true));
            CHECK(clone->eval_forward_branches==arms && sh_evaluate(clone,NULL,e));
            for(unsigned k=0;k<arms;k++)CHECK(e->site[k].valid);
            program_storage_free(clone,true);
        }
    }
    sh_eval_free(e);free(e);free(p);return 0;
}
int main(void) {
    CHECK(reference_spill_overwrite(false)==0);CHECK(reference_spill_overwrite(true)==0);
    CHECK(reference_spill_matrix()==0);CHECK(reference_spill_deferred()==0);
    CHECK(reference_spill_indices()==0);CHECK(reference_global_destination()==0);
    CHECK(reference_wqm()==0);CHECK(reference_fork_capacity()==0);
    puts("scalar reference: spill lifetime/deferred/wave aliases/pseudoflags, WQM, GLOBAL and bounded forks passed");return 0;
}
