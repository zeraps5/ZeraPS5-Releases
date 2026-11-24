/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/shader/shader_dynamic.h"
#include "gpu/shader/shader_ops.h"
#include "base/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provenance, not a representative value: every reaching definition must agree.
 * T is a word in the selected table record; O is a word in the object T names. */
enum { E_EMPTY, E_BUSY, E_UNKNOWN, E_CONST, E_ROOT, E_INDEX, E_TABLE, E_OBJECT };
typedef struct expr { uint32_t kind, a, b; } expr;
typedef struct edge { uint32_t from; int next; } edge;
typedef struct checked_word {guest_addr addr;uint32_t value;} checked_word;
typedef struct analysis {
    const sh_program *p;
    const sh_eval *initial;
    const sh_stage_info *info;
    gmem *mem;
    sh_dynamic_plan *out;
    int *heads;
    edge *edges;
    expr *cache;
    sh_eval *shape_eval;
    uint32_t root, maximum, stride, records;
    guest_addr table_base;
    uint8_t *table;
    checked_word *limits;
    uint32_t limit_count;
    bool failed;
    uint32_t depth;
} analysis;

static expr value(uint32_t kind, uint32_t a, uint32_t b) { return (expr){kind,a,b}; }
static bool same(expr a, expr b) { return a.kind==b.kind && a.a==b.a && a.b==b.b; }
static bool fail(analysis *a, const char *why, uint32_t i) {
    if (!a->failed) snprintf(a->out->reason,sizeof(a->out->reason),"%s at +0x%x",why,i<a->p->count?a->p->pc[i]*4:0);
    a->failed=true;return false;
}
static bool check_word(analysis *a,guest_addr addr,uint32_t v) {
    for(uint32_t i=0;i<a->limit_count;i++)if(a->limits[i].addr==addr)
        return a->limits[i].value==v || fail(a,"object changed during analysis",0);
    if(a->limit_count==4096)return fail(a,"object check capacity",0);
    a->limits[a->limit_count++]=(checked_word){addr,v};return true;
}
static bool resolve(analysis *a,expr x,uint32_t choice,uint32_t *out) {
    if(x.kind==E_CONST){*out=x.a;return true;}
    const uint8_t *record=a->table+(size_t)choice*a->stride;
    if(x.kind==E_TABLE && x.a+4<=a->stride){*out=rd_le32(record+x.a);return true;}
    if(x.kind==E_OBJECT && x.a+8<=a->stride) {
        guest_addr ptr=(rd_le64(record+x.a)&0xffffffffffffull)+x.b;
        return gmem_read(a->mem,ptr,out,4) && check_word(a,ptr,*out);
    }
    return false;
}
static uint32_t count(uint32_t op) {
    switch(op) {
    case SH_OP_S_LOAD_DWORDX2:case SH_OP_S_BUFFER_LOAD_DWORDX2:return 2;
    case SH_OP_S_LOAD_DWORDX4:case SH_OP_S_BUFFER_LOAD_DWORDX4:return 4;
    case SH_OP_S_LOAD_DWORDX8:case SH_OP_S_BUFFER_LOAD_DWORDX8:return 8;
    case SH_OP_S_LOAD_DWORDX16:case SH_OP_S_BUFFER_LOAD_DWORDX16:return 16;
    default:return 1;
    }
}
static bool branch_op(uint32_t op) { return op>=SH_OP_S_BRANCH && op<=SH_OP_S_CBRANCH_EXECNZ; }
static bool writes(const sh_program *p,uint32_t i,uint32_t reg,bool vector) {
    const rdna2_inst *in=p->insts+i;uint32_t op=p->ops[i],d=in->dst,n=1;
    if (vector) {
        if(in->enc==RDNA2_ENC_VOPC || (in->enc==RDNA2_ENC_VOP3 && in->valu<0x100) ||
           op==SH_OP_V_READLANE_B32 || op==SH_OP_V_READFIRSTLANE_B32 || in->enc==RDNA2_ENC_EXP)return false;
        if(in->enc==RDNA2_ENC_MUBUF || in->enc==RDNA2_ENC_MTBUF || in->enc==RDNA2_ENC_MIMG || in->enc==RDNA2_ENC_FLAT) {
            int site=p->site_of[i];if(site<0 || (p->sites[site].store && (!in->name || !strstr(in->name,"atomic"))))return false;
            d=RDNA2_OP_VGPR+in->vdata;n=p->vmem_width[i];
            if(in->name && strstr(in->name,"atomic"))n=(strstr(in->name,"_x2")?2u:1u)+(in->tfe?1u:0u);
        } else if(in->enc==RDNA2_ENC_DS) {
            /* The LDS destination width is deliberately overestimated. */
            n=16;
        } else n=(p->iflags[i]&SH_IF_64)?2:1;
        return d>=RDNA2_OP_VGPR && reg>=d && reg-d<n;
    }
    if(in->enc==RDNA2_ENC_SOPC || in->enc==RDNA2_ENC_SOPP || in->enc==RDNA2_ENC_EXP)return false;
    if(in->enc==RDNA2_ENC_SMEM)n=op==SH_OP_S_MEMTIME || op==SH_OP_S_MEMREALTIME?2:count(op);
    else if(in->enc==RDNA2_ENC_SOP1 || in->enc==RDNA2_ENC_SOP2 || in->enc==RDNA2_ENC_SOPK) {
        if(in->name && (strstr(in->name,"saveexec") || strstr(in->name,"wrexec")) &&
           reg>=RDNA2_OP_EXEC_LO && reg<=RDNA2_OP_EXEC_LO+((p->iflags[i]&SH_IF_64)?1u:0u))return true;
        if(op==SH_OP_S_SETREG_B32 || op==SH_OP_S_SETREG_IMM32_B32 ||
           (in->name && !strncmp(in->name,"s_cmpk",6)))return false;
        n=(p->iflags[i]&SH_IF_64)?2:1;
        if(in->name && (strstr(in->name,"_i32_b64") || strstr(in->name,"_i32_i64")))n=1;
    } else if(in->enc==RDNA2_ENC_VOPC || (in->enc==RDNA2_ENC_VOP3 && in->valu<0x100)) {
        d=op==SH_OP_V_CMPX?RDNA2_OP_EXEC_LO:in->dst;
        if(op!=SH_OP_V_CMPX && in->enc==RDNA2_ENC_VOPC && in->src[0]==RDNA2_OP_SDWA && ((in->dpp_sdwa>>15)&1))
            d=(in->dpp_sdwa>>8)&127;
        n=p->wave32?1:2;
    } else if(op==SH_OP_V_READLANE_B32 || op==SH_OP_V_READFIRSTLANE_B32){d=in->dst&127;n=1;}
    else if(p->iflags[i]&SH_IF_CARRY) {d=in->enc==RDNA2_ENC_VOP3?in->sdst:RDNA2_OP_VCC_LO;n=2;}
    else return false;
    return d<SH_SGPRS && d!=RDNA2_OP_NULL && reg>=d && reg-d<n;
}

/* All reaching definitions, including entry. A loop cannot hide another writer. */
static bool definitions(analysis *a,uint32_t at,uint32_t reg,bool vector,uint32_t *defs,uint32_t *nd) {
    uint32_t n=a->p->count,*queue=malloc(n*sizeof(*queue));uint8_t *seen=calloc(n,1);
    if(!queue || !seen){free(queue);free(seen);return fail(a,"allocation",at);}
    uint32_t first=0,last=0;*nd=0;
    for(int e=a->heads[at];e>=0;e=a->edges[e].next) {
        uint32_t v=a->edges[e].from;if(!seen[v]){seen[v]=1;queue[last++]=v;}
    }
    bool entry=at==0;
    while(first<last) {
        uint32_t v=queue[first++];
        if(writes(a->p,v,reg,vector)) {
            if(*nd==16){free(queue);free(seen);return fail(a,"too many definitions",at);}
            defs[(*nd)++]=v;continue;
        }
        if(v==0)entry=true;
        for(int e=a->heads[v];e>=0;e=a->edges[e].next) {
            uint32_t prev=a->edges[e].from;if(!seen[prev]){seen[prev]=1;queue[last++]=prev;}
        }
    }
    if(entry) {if(*nd==16){free(queue);free(seen);return fail(a,"definition overflow",at);}defs[(*nd)++]=UINT32_MAX;}
    free(queue);free(seen);return true;
}

static expr before(analysis *a,uint32_t at,uint32_t reg);
static expr operand(analysis *a,uint32_t at,uint32_t code) {
    uint32_t v;if(code<SH_SGPRS)return code==RDNA2_OP_NULL?value(E_CONST,0,0):before(a,at,code);
    if(sh_inline_const(code,a->p->insts[at].literal,&v))return value(E_CONST,v,0);
    return value(E_UNKNOWN,0,0);
}
static bool pointer(expr lo,expr hi,guest_addr *out) {
    if(lo.kind!=E_CONST || hi.kind!=E_CONST)return false;
    *out=(guest_addr)lo.a | ((guest_addr)(hi.a&65535)<<32);return true;
}
static bool table_pointer(expr lo,expr hi,uint32_t *field) {
    if(lo.kind!=E_TABLE || hi.kind!=E_TABLE || hi.a!=lo.a+4)return false;
    *field=lo.a;return true;
}
static expr defined(analysis *a,uint32_t i,uint32_t reg) {
    const rdna2_inst *in=a->p->insts+i;uint32_t op=a->p->ops[i],k=reg-in->dst;
    if(i==a->root)return value(E_ROOT,0,0);
    if(in->enc==RDNA2_ENC_SMEM) {
        if(op<SH_OP_S_LOAD_DWORD || op>SH_OP_S_BUFFER_LOAD_DWORDX16)return value(E_UNKNOWN,0,0);
        expr lo=before(a,i,in->sbase),hi=before(a,i,in->sbase+1);
        expr off=in->soffset==RDNA2_OP_NULL?value(E_CONST,0,0):before(a,i,in->soffset);
        if(op>=SH_OP_S_BUFFER_LOAD_DWORD && op<=SH_OP_S_BUFFER_LOAD_DWORDX16) {
            uint32_t d[4];
            for(uint32_t c=0;c<4;c++){expr x=before(a,i,in->sbase+c);if(x.kind!=E_CONST)return value(E_UNKNOWN,0,0);d[c]=x.a;}
            guest_addr base=(guest_addr)d[0]|((guest_addr)(d[1]&65535)<<32);
            uint32_t stride=(d[1]>>16)&16383;
            if(off.kind!=E_INDEX || off.a!=stride || off.b || !stride || (stride&3) || in->offset<0 || (in->offset&3) ||
               (uint64_t)in->offset+4*count(op)>stride || (uint64_t)a->maximum*stride+in->offset+4*count(op)>UINT32_MAX)
                {fail(a,"table offset is not bounded record indexing",i);return value(E_UNKNOWN,0,0);}
            if(!a->table) {
                uint64_t bytes=(uint64_t)stride*d[2];
                if(!d[2] || d[2]>SH_DYNAMIC_MAX_CHOICES || bytes>SH_DYNAMIC_MAX_TABLE_BYTES || !base)
                    {fail(a,"table size",i);return value(E_UNKNOWN,0,0);}
                a->table=malloc((size_t)bytes);
                if(!a->table || !gmem_read(a->mem,base,a->table,bytes)){fail(a,"table inaccessible",i);return value(E_UNKNOWN,0,0);}
                a->table_base=base;a->stride=stride;a->records=d[2];
            } else if(base!=a->table_base || stride!=a->stride || d[2]!=a->records)
                {fail(a,"multiple record tables",i);return value(E_UNKNOWN,0,0);}
            return value(E_TABLE,(uint32_t)in->offset+4*k,0);
        }
        if(off.kind!=E_CONST)return value(E_UNKNOWN,0,0);
        uint64_t offset=(uint32_t)in->offset+(uint64_t)off.a+4*k;guest_addr base;uint32_t field,v;
        if(pointer(lo,hi,&base)) {
            if(offset>UINT32_MAX || !gmem_read(a->mem,base+offset,&v,4) || !check_word(a,base+offset,v))return value(E_UNKNOWN,0,0);
            return value(E_CONST,v,0);
        }
        if(table_pointer(lo,hi,&field) && offset<=UINT32_MAX)return value(E_OBJECT,field,(uint32_t)offset);
        return value(E_UNKNOWN,0,0);
    }
    if(op==SH_OP_S_MOV_B32 || op==SH_OP_S_MOV_B64) {
        if(in->src[0]<SH_SGPRS)return operand(a,i,in->src[0]+k);
        expr x=operand(a,i,in->src[0]);
        if(k && x.kind==E_CONST)x.a=in->src[0]>=193 && in->src[0]<=208?UINT32_MAX:0;
        return x;
    }
    if(op==SH_OP_S_MOVK_I32)return value(E_CONST,(uint32_t)in->simm16,0);
    if(op==SH_OP_S_MUL_I32) {
        expr x=operand(a,i,in->src[0]),y=operand(a,i,in->src[1]);
        if(x.kind==E_CONST && y.kind==E_ROOT){expr tmp=x;x=y;y=tmp;}
        if(x.kind==E_ROOT && y.kind==E_CONST)return value(E_INDEX,y.a,0);
        if(x.kind==E_CONST && y.kind==E_CONST)return value(E_CONST,x.a*y.a,0);
    }
    return value(E_UNKNOWN,0,0);
}
static expr before(analysis *a,uint32_t at,uint32_t reg) {
    if(reg>=SH_SGPRS || at>=a->p->count || a->depth>=128)return value(E_UNKNOWN,0,0);
    expr *cached=a->cache+(size_t)at*SH_SGPRS+reg;
    if(cached->kind)return cached->kind==E_BUSY?value(E_UNKNOWN,0,0):*cached;
    cached->kind=E_BUSY;a->depth++;
    uint32_t defs[16],n=0;expr result=value(E_EMPTY,0,0);
    if(definitions(a,at,reg,false,defs,&n))for(uint32_t d=0;d<n;d++) {
        uint32_t ids=0;
        for(uint32_t dim=0;dim<3;dim++)ids+=(a->info->cs_tgid_enable>>dim)&1;
        bool runtime=reg>=a->info->cs_tgid_sgpr && reg-a->info->cs_tgid_sgpr<ids;
        expr x=defs[d]==UINT32_MAX?(runtime?value(E_UNKNOWN,0,0):value(E_CONST,a->initial->sgpr_init[reg],0)):defined(a,defs[d],reg);
        if(result.kind==E_EMPTY)result=x;
        else if(!same(result,x)){result=value(E_UNKNOWN,0,0);break;}
    }
    if(result.kind==E_EMPTY)result=value(E_UNKNOWN,0,0);
    a->depth--;*cached=result;return result;
}

/* Edge dominance: omit the fallthrough edge and prove the body unreachable. */
static bool guarded(analysis *a,uint32_t guard,uint32_t target) {
    uint32_t n=a->p->count,*q=malloc(n*sizeof(*q));uint8_t *seen=calloc(n,1);
    if(!q || !seen){free(q);free(seen);return false;}
    uint32_t head=0,tail=1;q[0]=0;seen[0]=1;bool reachable=false;
    while(head<tail) {
        uint32_t i=q[head++];if(i==target){reachable=true;break;}
        uint32_t op=a->p->ops[i],next[2]={i+1,a->p->target[i]},cnt=branch_op(op)?2:1;
        if(op==SH_OP_S_ENDPGM || op==SH_OP_S_CODE_END)continue;
        if(op==SH_OP_S_BRANCH){next[0]=next[1];cnt=1;}
        for(uint32_t k=0;k<cnt;k++)if(!(i==guard && next[k]==i+1) && next[k]<n && !seen[next[k]]) {
            seen[next[k]]=1;q[tail++]=next[k];
        }
    }
    free(q);free(seen);return !reachable;
}

static bool span_add(analysis *a,uint32_t site,guest_addr base,uint64_t size,bool store) {
    if(!size)return true;
    gmem_vma v;if(!base || base>UINT64_MAX-size || !gmem_query_vma(a->mem,base,false,&v) || v.reserved ||
       (v.prot&(store?GPROT_RW:GPROT_READ))!=(store?GPROT_RW:GPROT_READ) || base+size>v.start+v.size)
        return fail(a,"resource mapping/protection",a->p->sites[site].inst);
    sh_dynamic_span *s=a->out->spans+site;
    bool reader=!store && a->p->sites[site].kind==SH_SITE_BUFFER;
    if(reader) {
        if(size>(32u<<20))return fail(a,"readonly range exceeds 32 MiB",a->p->sites[site].inst);
        /* A VMA may contain distant geometry pools. Merge overlapping/touching
         * choices first; do not turn every choice in that VMA into one large
         * copy. Only bridge a checked, minimal gap when the range table is full. */
        sh_dynamic_range ranges[SH_DYNAMIC_MAX_RANGES+1];
        uint32_t n=s->range_count;
        if(n)memcpy(ranges,s->ranges,n*sizeof(*ranges));
        guest_addr low=base,end=base+size;
        for(uint32_t k=0;k<n;) {
            guest_addr rb=ranges[k].base,re=rb+ranges[k].size;
            if(rb>=v.start && re<=v.start+v.size && rb<=end && re>=low) {
                if(rb<low)low=rb;
                if(re>end)end=re;
                ranges[k]=ranges[--n];k=0;
            } else k++;
        }
        if(end-low>(32u<<20))return fail(a,"readonly range exceeds 32 MiB",a->p->sites[site].inst);
        ranges[n++]=(sh_dynamic_range){low,(uint32_t)(end-low),0};
        if(n>SH_DYNAMIC_MAX_RANGES) {
            uint32_t left=UINT32_MAX,right=0;int64_t best=INT64_MAX;
            for(uint32_t j=0;j<n;j++) {
                gmem_vma region;
                if(!gmem_query_vma(a->mem,ranges[j].base,false,&region) || region.reserved || !(region.prot&GPROT_READ))continue;
                for(uint32_t k=j+1;k<n;k++) {
                    guest_addr b=ranges[j].base<ranges[k].base?ranges[j].base:ranges[k].base;
                    guest_addr e=ranges[j].base+ranges[j].size>ranges[k].base+ranges[k].size?
                        ranges[j].base+ranges[j].size:ranges[k].base+ranges[k].size;
                    if(b<region.start || e>region.start+region.size)continue;
                    int64_t extra=(int64_t)((e-b+3)&~3ull)-((ranges[j].size+3u)&~3u)-((ranges[k].size+3u)&~3u);
                    if(extra<best){best=extra;left=j;right=k;}
                }
            }
            if(left==UINT32_MAX)return fail(a,"readonly range capacity",a->p->sites[site].inst);
            low=ranges[left].base<ranges[right].base?ranges[left].base:ranges[right].base;
            end=ranges[left].base+ranges[left].size>ranges[right].base+ranges[right].size?
                ranges[left].base+ranges[left].size:ranges[right].base+ranges[right].size;
            if(end-low>(32u<<20))return fail(a,"readonly choices exceed 32 MiB of copied data",a->p->sites[site].inst);
            ranges[left]=(sh_dynamic_range){low,(uint32_t)(end-low),0};ranges[right]=ranges[--n];
        }
        uint64_t total=0;
        for(uint32_t k=0;k<n;k++) {
            ranges[k].offset=(uint32_t)total;total+=(ranges[k].size+3u)&~3u;
        }
        if(total>(32u<<20))return fail(a,"readonly choices exceed 32 MiB of copied data",a->p->sites[site].inst);
        if(!s->ranges)s->ranges=calloc(SH_DYNAMIC_MAX_RANGES,sizeof(*s->ranges));
        if(!s->ranges)return fail(a,"readonly range allocation",a->p->sites[site].inst);
        memcpy(s->ranges,ranges,n*sizeof(*ranges));s->range_count=n;s->packed_size=(uint32_t)total;
    }
    if(!s->size){s->base=base;s->size=size;}
    else {guest_addr low=s->base<base?s->base:base,end=s->base+s->size>base+size?s->base+s->size:base+size;s->base=low;s->size=end-low;}
    s->store|=store;
    if(!reader && s->size>(32u<<20))return fail(a,"resource choices span more than 32 MiB",a->p->sites[site].inst);
    return true;
}

/* Conservative unsigned range over every reaching VGPR definition. A logical
 * right shift/mask can bound even arbitrary source data; an unproved source
 * otherwise keeps the entire 32-bit range. */
static uint32_t vector_max(analysis *a,uint32_t at,uint32_t reg,uint32_t depth) {
    if(depth>=16 || reg<RDNA2_OP_VGPR)return UINT32_MAX;
    uint32_t defs[16],nd=0,result=0;
    if(!definitions(a,at,reg,true,defs,&nd) || !nd)return UINT32_MAX;
    for(uint32_t k=0;k<nd;k++) {
        uint32_t bound=UINT32_MAX,i=defs[k];
        if(i==UINT32_MAX)return UINT32_MAX;
        const rdna2_inst *in=a->p->insts+i;uint32_t op=a->p->ops[i];
        if(op==SH_OP_BUFFER_LOAD_UBYTE)bound=255;
        else if(op==SH_OP_BUFFER_LOAD_USHORT)bound=65535;
        else if(in->enc==RDNA2_ENC_VOP1 && op==SH_OP_V_MOV_B32) {
            expr src=operand(a,i,in->src[0]);
            if(src.kind==E_CONST)bound=src.a;
            else if(in->src[0]>=RDNA2_OP_VGPR)bound=vector_max(a,i,in->src[0],depth+1);
        } else if(in->enc==RDNA2_ENC_VOP2 &&
                  (op==SH_OP_V_LSHRREV_B32 || op==SH_OP_V_LSHLREV_B32)) {
            expr shift=operand(a,i,in->src[0]);
            if(shift.kind==E_CONST) {
                uint32_t b=vector_max(a,i,in->src[1],depth+1),bits=shift.a&31;
                if(op==SH_OP_V_LSHRREV_B32)bound=b>>bits;
                else if(((uint64_t)b<<bits)<=UINT32_MAX)bound=b<<bits;
            }
        } else if(in->enc==RDNA2_ENC_VOP2 && op==SH_OP_V_AND_B32 &&
                  in->src[0]!=RDNA2_OP_SDWA && in->src[0]!=RDNA2_OP_DPP16 &&
                  in->src[0]!=RDNA2_OP_DPP8 && in->src[0]!=RDNA2_OP_DPP8FI) {
            expr x=operand(a,i,in->src[0]),y=operand(a,i,in->src[1]);
            if(x.kind==E_CONST)bound=x.a;
            if(y.kind==E_CONST && y.a<bound)bound=y.a;
        }
        if(bound>result)result=bound;
    }
    return result;
}

/* Include the complete access, including an immediate/scalar offset past the
 * last record. NUM_RECORDS limits the index of a strided access, not its offset.
 * Raw offsets are bounded by their OOB mode instead. */
static bool buffer_extent(analysis *a,uint32_t site,const uint32_t desc[4],
                          const sh_site_shape *shape,uint64_t *size) {
    uint32_t i=a->p->sites[site].inst,op=a->p->ops[i];
    const rdna2_inst *in=a->p->insts+i;
    const char *tail=in->name?strrchr(in->name,'_'):NULL;
    uint32_t n=1;
    if(tail && (!strcmp(tail,"_xyzw") || !strcmp(tail,"_dwordx4")))n=4;
    else if(tail && (!strcmp(tail,"_xyz") || !strcmp(tail,"_dwordx3")))n=3;
    else if(tail && (!strcmp(tail,"_xy") || !strcmp(tail,"_dwordx2") || !strcmp(tail,"_x2")))n=2;
    uint32_t bytes=n*4,stride=(desc[1]>>16)&16383;
    if(op==SH_OP_BUFFER_LOAD_UBYTE || op==SH_OP_BUFFER_LOAD_SBYTE || op==SH_OP_BUFFER_STORE_BYTE)bytes=1;
    if(op==SH_OP_BUFFER_LOAD_USHORT || op==SH_OP_BUFFER_LOAD_SSHORT || op==SH_OP_BUFFER_STORE_SHORT)bytes=2;
    bool format=in->enc==RDNA2_ENC_MTBUF || (in->name && strstr(in->name,"_format"));
    sh_buf_format f;
    if(format && sh_buffer_format(shape->buf_format,&f)) {
        uint32_t bits=0;
        for(uint32_t k=0;k<f.comps && (!a->p->sites[site].store || k<n);k++)bits+=f.bits[k];
        bytes=(bits+7)/8;
    }
    if(!desc[2]){*size=0;return true;}
    uint64_t end;
    if(in->idxen && stride) {
        expr soff=operand(a,i,in->soffset);
        uint32_t voff=in->offen?vector_max(a,i,RDNA2_OP_VGPR+in->vaddr+1,0):0;
        if(voff==UINT32_MAX || soff.kind!=E_CONST || in->offset<0)
            return fail(a,"strided buffer offset has no finite bound",i);
        end=(uint64_t)(desc[2]-1)*stride+voff+soff.a+(uint32_t)in->offset+bytes;
        if(end>UINT32_MAX)return fail(a,"strided buffer address can wrap",i);
    } else {
        if(((desc[3]>>28)&3)==2)return fail(a,"unbounded raw OOB mode",i);
        end=(uint64_t)desc[2]+bytes-1;
    }
    uint64_t declared=(uint64_t)desc[2]*(stride?stride:1);
    *size=end>declared?end:declared;
    return true;
}

static bool scalar_bound(analysis *a,uint32_t at,uint32_t soff,uint32_t *max_offset) {
    uint32_t defs[16],n=0;
    if(!definitions(a,at,soff,false,defs,&n) || n!=1 || defs[0]==UINT32_MAX)return false;
    uint32_t producer=defs[0];const rdna2_inst *prod=a->p->insts+producer;
    if(a->p->ops[producer]!=SH_OP_S_LSHL4_ADD_U32 || prod->src[0]>=SH_SGPRS)return false;
    expr add=operand(a,producer,prod->src[1]);if(add.kind!=E_CONST)return false;
    uint32_t counter=prod->src[0];
    for(uint32_t guard=0;guard<producer;guard++) {
        if(a->p->ops[guard]!=SH_OP_S_CBRANCH_VCCZ || guard<4 || a->p->target[guard]<=at)continue;
        const rdna2_inst *g=a->p->insts+guard;
        (void)g;
        const rdna2_inst *mv=a->p->insts+guard-2,*exec=mv+1,*sel=mv-1;
        if(a->p->ops[guard-1]!=SH_OP_S_MOV_B64 || exec->dst!=RDNA2_OP_EXEC_LO ||
           a->p->ops[guard-2]!=SH_OP_S_MOV_B64 || mv->dst!=RDNA2_OP_VCC_LO ||
           a->p->ops[guard-3]!=SH_OP_S_CSELECT_B64 || sel->src[0]!=RDNA2_OP_EXEC_LO || sel->src[1]!=128 ||
           exec->src[0]!=sel->dst || mv->src[0]!=sel->dst)continue;
        uint32_t cmp=guard-4;while(cmp && a->p->ops[cmp]==SH_OP_S_NOP)cmp--;
        const rdna2_inst *test=a->p->insts+cmp;
        if(a->p->ops[cmp]!=SH_OP_S_CMP_LT_I32 || test->src[0]!=counter || test->src[1]>=SH_SGPRS || !guarded(a,guard,at))continue;
        expr limit=before(a,cmp,test->src[1]);if(limit.kind!=E_OBJECT || limit.a+8>a->stride)continue;
        if(!definitions(a,producer,counter,false,defs,&n))return false;
        bool initialized=false,incremented=false,ok=true;
        for(uint32_t d=0;d<n;d++) {
            if(defs[d]==UINT32_MAX){ok=false;break;}
            const rdna2_inst *in=a->p->insts+defs[d];uint32_t op=a->p->ops[defs[d]],v;
            if(op==SH_OP_S_MOV_B32 && sh_inline_const(in->src[0],in->literal,&v) && v==0)initialized=true;
            else if((op==SH_OP_S_ADD_I32 || op==SH_OP_S_ADD_U32) && in->src[0]==counter &&
                    sh_inline_const(in->src[1],in->literal,&v) && v==1 && guarded(a,guard,defs[d]))incremented=true;
            else ok=false;
        }
        if(!ok || !initialized || !incremented)continue;
        uint32_t bound=0;
        for(uint32_t c=0;c<a->records;c++) {
            uint64_t ptr=rd_le64(a->table+(size_t)c*a->stride+limit.a);uint32_t v;
            ptr&=0xffffffffffffull;
            if(!gmem_read(a->mem,ptr+limit.b,&v,4) || (int32_t)v>1024)return false;
            if(!check_word(a,ptr+limit.b,v))return false;
            if((int32_t)v>0 && v>bound)bound=v;
        }
        uint64_t off=(bound?bound-1:0)*16ull+add.a;
        if(off>UINT32_MAX)return false;
        *max_offset=(uint32_t)off;return true;
    }
    return false;
}

bool sh_dynamic_plan_build_with_prepare(const sh_program *p,const sh_eval *initial,const sh_stage_info *info,
                                        gmem *mem,sh_dynamic_plan *out,bool (*prepare)(void *),void *context) {
    memset(out,0,sizeof(*out));analysis a={.p=p,.initial=initial,.info=info,.mem=mem,.out=out,.root=UINT32_MAX};
    if(!info || info->stage!=SH_STAGE_CS){snprintf(out->reason,sizeof(out->reason),"compute stage metadata required");return false;}
    /* Appended offsets and out->words are u32 even on a 64-bit host. Check
     * the worst-case capacity before allocation, narrowing or any copy. */
    uint64_t uniform_capacity=(uint64_t)initial->smem_words+276ull*p->site_count;
    if(p->site_count>SH_MAX_SITES || uniform_capacity>UINT32_MAX || uniform_capacity>SIZE_MAX/sizeof(uint32_t)) {
        snprintf(out->reason,sizeof(out->reason),"uniform word capacity overflow");return false;
    }
    a.heads=malloc(p->count*sizeof(*a.heads));a.edges=calloc(2*p->count,sizeof(*a.edges));
    if(!a.heads || !a.edges){fail(&a,"allocation",0);goto done;}
    for(uint32_t i=0;i<p->count;i++)a.heads[i]=-1;
    uint32_t ne=0;
    for(uint32_t i=0;i<p->count;i++) {
        uint32_t op=p->ops[i];
        if(p->insts[i].name && (strstr(p->insts[i].name,"setpc") || strstr(p->insts[i].name,"swappc") || strstr(p->insts[i].name,"movrel")))
            {fail(&a,"indirect control flow",i);goto done;}
        if(op==SH_OP_V_READLANE_B32 || op==SH_OP_V_READFIRSTLANE_B32) {
            if(a.root!=UINT32_MAX){fail(&a,"more than one vector scalarization",i);goto done;}a.root=i;
        }
        if(op==SH_OP_S_ENDPGM || op==SH_OP_S_CODE_END)continue;
        uint32_t targets[2]={i+1,p->target[i]},n=branch_op(op)?2:1;
        if(op==SH_OP_S_BRANCH){targets[0]=targets[1];n=1;}
        for(uint32_t k=0;k<n;k++)if(targets[k]<p->count) {
            a.edges[ne]=(edge){i,a.heads[targets[k]]};a.heads[targets[k]]=(int)ne++;
        }
    }
    if(a.root==UINT32_MAX){fail(&a,"no bounded vector index",0);goto done;}
    uint32_t defs[16],nd=0;const rdna2_inst *root=p->insts+a.root;
    if(root->src[0]<RDNA2_OP_VGPR || !definitions(&a,a.root,root->src[0],true,defs,&nd) || nd!=1 || defs[0]==UINT32_MAX)
        {fail(&a,"ambiguous vector index source",a.root);goto done;}
    const rdna2_inst *shift=p->insts+defs[0];uint32_t bits;
    if(p->ops[defs[0]]!=SH_OP_V_LSHRREV_B32 || shift->enc!=RDNA2_ENC_VOP2 ||
       !sh_inline_const(shift->src[0],shift->literal,&bits) || (bits&31)<16)
        {fail(&a,"index is not bounded by a logical shift",a.root);goto done;}
    a.maximum=UINT32_MAX>>(bits&31);
    a.cache=calloc((size_t)p->count*SH_SGPRS,sizeof(*a.cache));
    a.shape_eval=calloc(1,sizeof(*a.shape_eval));
    a.limits=calloc(4096,sizeof(*a.limits));
    if(!a.cache || !a.shape_eval || !a.limits){fail(&a,"allocation",0);goto done;}
    if(prepare && !prepare(context)){fail(&a,"guest memory preparation failed",a.root);goto done;}
    /* Discover/check the table through every SBUFFER instruction. */
    for(uint32_t s=0;s<p->site_count;s++)if(p->sites[s].kind==SH_SITE_SBUFFER)
        (void)defined(&a,p->sites[s].inst,p->insts[p->sites[s].inst].dst);
    if(a.failed || !a.table){fail(&a,"no verified record table",a.root);goto done;}
    out->choices=a.records;
    out->words=initial->smem_words;
    /* Worst case: 64 compact ranges (four metadata dwords each) per site,
     * plus scalar values/image guards. Only the used prefix is written. */
    out->uniforms=malloc((size_t)uniform_capacity*sizeof(uint32_t));
    if(!out->uniforms){fail(&a,"uniform allocation",0);goto done;}
    memcpy(out->uniforms,initial->smem_data,out->words*sizeof(uint32_t));
    for(uint32_t s=0;s<p->site_count && !a.failed;s++) {
        const sh_site *site=p->sites+s;uint32_t i=site->inst;const rdna2_inst *in=p->insts+i;
        sh_site_shape_of(p,initial,s,out->shapes+s);
        if(site->kind==SH_SITE_SBUFFER) {
            sh_dynamic_span *span=out->spans+s;span->base=a.table_base;span->size=(uint64_t)a.stride*a.records;
            span->snapshot=malloc((size_t)span->size);
            if(!span->snapshot){fail(&a,"table allocation",i);break;}
            memcpy(span->snapshot,a.table,(size_t)span->size);continue;
        }
        if(site->kind==SH_SITE_SMEM) {
            if(p->ops[i]<SH_OP_S_LOAD_DWORD || p->ops[i]>SH_OP_S_LOAD_DWORDX16)
                {fail(&a,"unsupported scalar memory operation",i);break;}
            expr lo=before(&a,i,in->sbase),hi=before(&a,i,in->sbase+1);
            expr off=in->soffset==RDNA2_OP_NULL?value(E_CONST,0,0):before(&a,i,in->soffset);
            guest_addr base;uint32_t field,n=count(p->ops[i]);
            if(pointer(lo,hi,&base) && off.kind==E_CONST) {
                uint64_t addr=base+(uint32_t)in->offset+off.a;
                out->uniforms[SH_U_SITE+s]=out->words;
                if(!gmem_read(mem,addr,out->uniforms+out->words,n*4)){fail(&a,"constant scalar load mapping",i);break;}
                sh_dynamic_span *span=out->spans+s;span->base=addr;span->size=n*4;
                span->snapshot=malloc(n*4);
                if(!span->snapshot){fail(&a,"constant scalar snapshot allocation",i);break;}
                memcpy(span->snapshot,out->uniforms+out->words,n*4);
                out->words+=n;continue;
            }
            if(!table_pointer(lo,hi,&field) || field+8>a.stride){fail(&a,"scalar pointer provenance",i);break;}
            uint32_t maxoff;
            if(off.kind==E_CONST)maxoff=off.a;
            else if(!scalar_bound(&a,i,in->soffset,&maxoff)){fail(&a,"scalar offset bound",i);break;}
            for(uint32_t c=0;c<a.records;c++) {
                base=rd_le64(a.table+(size_t)c*a.stride+field)&0xffffffffffffull;
                if(!span_add(&a,s,base+(uint32_t)in->offset,(uint64_t)maxoff+n*4,false))break;
            }
            out->shapes[s].address_map=1;
        } else if(site->kind==SH_SITE_BUFFER) {
            if(in->enc!=RDNA2_ENC_MUBUF && in->enc!=RDNA2_ENC_MTBUF){fail(&a,"non-descriptor buffer access",i);break;}
            expr d[4];uint32_t field=0;bool table=true,constant=true,object=true;
            for(uint32_t k=0;k<4;k++){d[k]=before(&a,i,in->srsrc+k);table&=d[k].kind==E_TABLE;constant&=d[k].kind==E_CONST;object&=d[k].kind==E_OBJECT;}
            if(table) {field=d[0].a;for(uint32_t k=0;k<4;k++)table&=d[k].a==field+4*k;}
            if(object)for(uint32_t k=0;k<4;k++)object&=d[k].a==d[0].a && d[k].b==d[0].b+4*k;
            if((!constant && !table && !object) || (table && field+16>a.stride)){fail(&a,"buffer descriptor provenance",i);break;}
            bool typed=in->enc==RDNA2_ENC_MTBUF || (in->name && strstr(in->name,"_format"));
            bool shaped=false;
            for(uint32_t c=0;c<(constant?1:a.records);c++) {
                uint32_t desc[4];bool read=true;
                for(uint32_t k=0;k<4;k++)read&=resolve(&a,d[k],c,desc+k);
                if(!read){fail(&a,"object buffer descriptor unreadable",i);break;}
                if(!desc[2])continue;
                memcpy(a.shape_eval->site[s].desc,desc,sizeof(desc));sh_site_shape shape;sh_site_shape_of(p,a.shape_eval,s,&shape);
                if(shaped && typed && (shape.buf_format!=out->shapes[s].buf_format || memcmp(shape.dst_sel,out->shapes[s].dst_sel,4)))
                    {fail(&a,"table mixes typed formats",i);break;}
                out->shapes[s]=shape;shaped=true;
                uint64_t size;
                if(!buffer_extent(&a,s,desc,&shape,&size))break;
                guest_addr base=(guest_addr)desc[0]|((guest_addr)(desc[1]&65535)<<32);
                if(!span_add(&a,s,base,size,site->store))break;
            }
            out->shapes[s].address_map=1;out->shapes[s].buf_stride=0;
            if(!site->store && out->spans[s].size>(32u<<20)) {
                out->shapes[s].address_map=2;
                out->shapes[s].buf_stride=out->spans[s].range_count;
            }
        } else if(site->kind==SH_SITE_IMAGE && !site->store) {
            if(in->tfe || in->lwe){fail(&a,"image status form not covered by finite plan",i);break;}
            expr words[12];
            for(uint32_t k=0;k<8;k++)words[k]=before(&a,i,in->srsrc+k);
            for(uint32_t k=0;k<4;k++)words[8+k]=before(&a,i,in->ssamp+k);
            uint32_t first[12]={0};
            for(uint32_t c=0;c<a.records;c++) {
                uint32_t current[12];bool read=true;
                for(uint32_t k=0;k<12;k++)read&=resolve(&a,words[k],c,current+k);
                if(!read){fail(&a,"image descriptor provenance",i);break;}
                if(c && memcmp(first,current,sizeof(first))){fail(&a,"object choices select different images or samplers",i);break;}
                memcpy(first,current,sizeof(first));
            }
            sh_site_value *v=out->images+s;v->valid=true;memcpy(v->desc,first,32);memcpy(v->sampler,first+8,16);
            a.shape_eval->site[s]=*v;sh_site_shape_of(p,a.shape_eval,s,out->shapes+s);
            out->shapes[s].address_map=1; /* guard the live descriptor, including an out-of-table selection */
            continue;
        } else {fail(&a,"unsupported resource in finite table dispatch",i);break;}
        if(a.failed)break;
        sh_dynamic_span *span=out->spans+s;
        if(site->kind==SH_SITE_SMEM && span->size) {
            span->snapshot=malloc((size_t)span->size);
            if(!span->snapshot || !gmem_read(mem,span->base,span->snapshot,span->size)){fail(&a,"scalar snapshot",i);break;}
        }
    }
    for(uint32_t c=0;c<a.limit_count && !a.failed;c++) {
        bool found=false;
        for(uint32_t s=0;s<p->site_count;s++) {
            const sh_dynamic_span *span=out->spans+s;
            if(p->sites[s].kind==SH_SITE_SMEM && span->snapshot && a.limits[c].addr>=span->base &&
               a.limits[c].addr+4<=span->base+span->size) {
                found=true;
                if(rd_le32(span->snapshot+(a.limits[c].addr-span->base))!=a.limits[c].value)
                    {fail(&a,"scalar input changed during snapshot",p->sites[s].inst);break;}
            }
        }
        if(!found)fail(&a,"scalar input has no immutable source",0);
    }
    /* Imported stores cannot change a scalar snapshot or the selection table. */
    for(uint32_t s=0;s<p->site_count && !a.failed;s++)if(out->spans[s].store)
        for(uint32_t k=0;k<p->site_count;k++)if(out->spans[k].snapshot &&
            out->spans[s].base<out->spans[k].base+out->spans[k].size && out->spans[k].base<out->spans[s].base+out->spans[s].size)
            {fail(&a,"store aliases immutable scalar input",p->sites[s].inst);break;}
done:
    free(a.heads);free(a.edges);free(a.cache);free(a.table);free(a.shape_eval);free(a.limits);
    if(a.failed){char reason[sizeof(out->reason)];memcpy(reason,out->reason,sizeof(reason));sh_dynamic_plan_free(out);memcpy(out->reason,reason,sizeof(reason));return false;}
    return true;
}
bool sh_dynamic_plan_build(const sh_program *p,const sh_eval *initial,const sh_stage_info *info,gmem *mem,sh_dynamic_plan *out) {
    return sh_dynamic_plan_build_with_prepare(p,initial,info,mem,out,NULL,NULL);
}
void sh_dynamic_plan_free(sh_dynamic_plan *plan) {
    for(uint32_t s=0;s<SH_MAX_SITES;s++){free(plan->spans[s].snapshot);free(plan->spans[s].ranges);}
    free(plan->uniforms);memset(plan,0,sizeof(*plan));
}
