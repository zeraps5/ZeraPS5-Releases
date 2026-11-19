/* SPDX-License-Identifier: GPL-2.0-only */
/* Node intersection adapted to ZeraPS5's C emitter, 2026-10-22.
 * Algorithm reference: KytyPS5 contributors, spirvEmitterBvh.cpp at
 * f5f4e7d48a825c1e820f3d5bdc2c6138ba93303c (GPL-2.0).
 * Each invocation tests ONE node; the guest shader controls traversal.
 * The backend binds either a checked complete BVH span or a bounded table
 * translating live guest addresses to Vulkan buffer device addresses. */
#ifndef PS5C_SHADER_BVH_SPIRV_H
#define PS5C_SHADER_BVH_SPIRV_H
#include "gpu/shader/shader_bvh_address.h"

static uint32_t bvh_and(tr *t, uint32_t a, uint32_t b) { return op2(t, SpvOpLogicalAnd, t->t_bool, a, b); }
static uint32_t bvh_or(tr *t, uint32_t a, uint32_t b) { return op2(t, SpvOpLogicalOr, t->t_bool, a, b); }
static uint32_t bvh_cmp(tr *t, uint32_t op, uint32_t a, uint32_t b) { return op2(t, op, t->t_bool, a, b); }
static uint32_t bvh_fop(tr *t, uint32_t op, uint32_t a, uint32_t b) {
    bool exact=t->b.exact;
    t->b.exact=true; /* let the builder emit NoContraction exactly once */
    uint32_t v = op2(t, op, t->t_f32, a, b);
    t->b.exact=exact;
    return v;
}
static uint32_t bvh_pack(tr *t, uint32_t lo, uint32_t hi) {
    return op2(t, SpvOpBitwiseOr, t->t_u64, op1(t, SpvOpUConvert, t->t_u64, lo),
        op2(t, SpvOpShiftLeftLogical, t->t_u64, op1(t, SpvOpUConvert, t->t_u64, hi), cu(t, 32)));
}
/* Same exact count, one device atomic per participating host subgroup. The
 * complete CS modes already require Basic/Ballot subgroup support. */
static void bvh_count(tr *t,uint32_t var,uint32_t word,uint32_t predicate) {
    uint32_t branch=begin_if(t,predicate),count=cu(t,1),leader=0;
    if(t->info->cs_wave_mode) {
        uint32_t mask=spv_op(&t->b,SpvOpGroupNonUniformBallot,t->t_v4u,
            (uint32_t[]){cu(t,SpvScopeSubgroup),spv_const_bool(&t->b,true)},2);
        count=cu(t,0);
        for(unsigned i=0;i<4;i++)count=op2(t,SpvOpIAdd,t->t_u32,count,
            op1(t,SpvOpBitCount,t->t_u32,op2(t,SpvOpCompositeExtract,t->t_u32,mask,i)));
        uint32_t elected=spv_op(&t->b,SpvOpGroupNonUniformElect,t->t_bool,
            (uint32_t[]){cu(t,SpvScopeSubgroup)},1);
        leader=begin_if(t,elected);
    }
    spv_op(&t->b,SpvOpAtomicIAdd,t->t_u32,(uint32_t[]){sb_ptr(t,var,cu(t,word)),cu(t,1),cu(t,0),count},4);
    if(leader)end_if(t,leader);
    end_if(t,branch);
}
/* Table header: count, successful/missing resolutions, reserved. Entries
 * are sorted {guest base u64, byte size u64, Vulkan device address u64, pad}.
 * No guest/host pointer is ever dereferenced before this checked lookup. */
static uint32_t bvh_resolve(tr *t, uint32_t var, uint32_t address, bool paged) {
    uint32_t U=t->t_u32,L=t->t_u64;
    uint32_t count=sb_load(t,var,cu(t,0));
    uint32_t length=spv_op(&t->b,SpvOpArrayLength,U,(uint32_t[]){var,0},2);
    uint32_t capacity=op2(t,SpvOpShiftRightLogical,U,
        op2(t,SpvOpISub,U,length,cu(t,paged?SH_BVH_PAGED_HEADER:4)),cu(t,3));
    count=ext2(t,U,GLSLstd450UMin,count,ext2(t,U,GLSLstd450UMin,capacity,cu(t,4096)));
    uint32_t low=spv_local_var(&t->b,t->p_fn_u32), high=spv_local_var(&t->b,t->p_fn_u32);
    uint32_t result=spv_local_var(&t->b,spv_type_pointer(&t->b,SpvStorageClassFunction,L));
    uint32_t known=spv_local_var(&t->b,t->p_fn_u32);
    st(t,known,cu(t,0));st(t,low,cu(t,0));st(t,high,count);st(t,result,c64(t,0));
    /* Fixed bound is enough for all 4096 entries, and leaves no data-dependent
     * infinite traversal on malformed input. Empty tables never load an entry. */
    for(unsigned step=0;step<13;step++) {
        uint32_t lo=ld(t,U,low),hi=ld(t,U,high);
        uint32_t branch=begin_if(t,bvh_cmp(t,SpvOpULessThan,lo,hi));
        uint32_t mid=op2(t,SpvOpShiftRightLogical,U,op2(t,SpvOpIAdd,U,lo,hi),cu(t,1));
        uint32_t entry=op2(t,SpvOpIAdd,U,cu(t,paged?SH_BVH_PAGED_HEADER:4),op2(t,SpvOpShiftLeftLogical,U,mid,cu(t,3)));
        uint32_t base=bvh_pack(t,sb_load(t,var,entry),sb_load(t,var,op2(t,SpvOpIAdd,U,entry,cu(t,1))));
        uint32_t size=bvh_pack(t,sb_load(t,var,op2(t,SpvOpIAdd,U,entry,cu(t,2))),sb_load(t,var,op2(t,SpvOpIAdd,U,entry,cu(t,3))));
        uint32_t gpu=bvh_pack(t,sb_load(t,var,op2(t,SpvOpIAdd,U,entry,cu(t,4))),sb_load(t,var,op2(t,SpvOpIAdd,U,entry,cu(t,5))));
        uint32_t offset=op2(t,SpvOpISub,L,address,base);
        uint32_t found=bvh_and(t,bvh_cmp(t,SpvOpUGreaterThanEqual,address,base),bvh_cmp(t,SpvOpUGreaterThanEqual,size,c64(t,64)));
        found=bvh_and(t,found,bvh_cmp(t,SpvOpULessThanEqual,offset,op2(t,SpvOpISub,L,size,c64(t,64))));
        st(t,known,sel(t,U,found,cu(t,1),ld(t,U,known)));
        uint32_t physical=sel(t,L,bvh_cmp(t,SpvOpINotEqual,gpu,c64(t,0)),op2(t,SpvOpIAdd,L,gpu,offset),c64(t,0));
        st(t,result,sel(t,L,found,physical,ld(t,L,result)));
        uint32_t before=bvh_cmp(t,SpvOpULessThan,address,base);
        st(t,high,sel(t,U,before,mid,hi));
        st(t,low,sel(t,U,before,lo,op2(t,SpvOpIAdd,U,mid,cu(t,1))));
        end_if(t,branch);
    }
    uint32_t value=ld(t,L,result);
    uint32_t ok=bvh_cmp(t,SpvOpINotEqual,value,c64(t,0));
    uint32_t unknown=bvh_cmp(t,SpvOpIEqual,ld(t,U,known),cu(t,0));
    bvh_count(t,var,1,ok);
    bvh_count(t,var,2,unknown);
    if(paged) {
        uint32_t branch=begin_if(t,bvh_or(t,ok,unknown));
        uint32_t page=op1(t,SpvOpUConvert,U,op2(t,SpvOpShiftRightLogical,L,address,cu(t,SH_BVH_PAGE_SHIFT)));
        uint32_t hash=op2(t,SpvOpIMul,U,page,cu(t,2654435761u));
        uint32_t done=spv_local_var(&t->b,t->p_fn_bool);st(t,done,spv_const_bool(&t->b,false));
        for(unsigned probe=0;probe<16;probe++) {
            uint32_t attempt=begin_if(t,op1(t,SpvOpLogicalNot,t->t_bool,ld(t,t->t_bool,done)));
            uint32_t slot=op2(t,SpvOpBitwiseAnd,U,op2(t,SpvOpIAdd,U,hash,cu(t,probe)),cu(t,SH_BVH_REQUEST_SLOTS-1));
            uint32_t ptr=sb_ptr(t,var,op2(t,SpvOpIAdd,U,slot,cu(t,4)));
            /* Slots only transition from zero to a page until the dispatch
             * completes. An already populated slot requires no write-CAS. */
            uint32_t seen=spv_local_var(&t->b,t->p_fn_u32);
            st(t,seen,spv_op(&t->b,SpvOpAtomicLoad,U,(uint32_t[]){ptr,cu(t,1),cu(t,0)},3));
            uint32_t empty=begin_if(t,bvh_cmp(t,SpvOpIEqual,ld(t,U,seen),cu(t,0)));
            st(t,seen,spv_op(&t->b,SpvOpAtomicCompareExchange,U,
                (uint32_t[]){ptr,cu(t,1),cu(t,0),cu(t,0),page,cu(t,0)},6));
            end_if(t,empty);
            uint32_t old=ld(t,U,seen);
            uint32_t claimed=bvh_or(t,bvh_cmp(t,SpvOpIEqual,old,cu(t,0)),bvh_cmp(t,SpvOpIEqual,old,page));
            uint32_t mask_branch=begin_if(t,claimed);
            uint32_t unit=op2(t,SpvOpBitwiseAnd,U,op1(t,SpvOpUConvert,U,op2(t,SpvOpShiftRightLogical,L,address,cu(t,6))),cu(t,1023));
            uint32_t mask_word=op2(t,SpvOpIAdd,U,cu(t,SH_BVH_READ_MASK_BASE),
                op2(t,SpvOpIAdd,U,op2(t,SpvOpIMul,U,slot,cu(t,SH_BVH_READ_MASK_WORDS)),op2(t,SpvOpShiftRightLogical,U,unit,cu(t,5))));
            uint32_t bit=op2(t,SpvOpShiftLeftLogical,U,cu(t,1),op2(t,SpvOpBitwiseAnd,U,unit,cu(t,31)));
            uint32_t mask_ptr=sb_ptr(t,var,mask_word);
            uint32_t recorded=spv_op(&t->b,SpvOpAtomicLoad,U,(uint32_t[]){mask_ptr,cu(t,1),cu(t,0)},3);
            uint32_t absent=begin_if(t,bvh_cmp(t,SpvOpIEqual,op2(t,SpvOpBitwiseAnd,U,recorded,bit),cu(t,0)));
            spv_op(&t->b,SpvOpAtomicOr,U,(uint32_t[]){mask_ptr,cu(t,1),cu(t,0),bit},4);
            end_if(t,absent);
            end_if(t,mask_branch);st(t,done,claimed);
            end_if(t,attempt);
        }
        uint32_t overflow=begin_if(t,op1(t,SpvOpLogicalNot,t->t_bool,ld(t,t->t_bool,done)));
        spv_op(&t->b,SpvOpAtomicOr,U,(uint32_t[]){sb_ptr(t,var,cu(t,3)),cu(t,1),cu(t,0),cu(t,1)},4);
        end_if(t,overflow);end_if(t,branch);
    }
    return value;
}
static uint32_t bvh_word(tr *t, uint32_t var, uint32_t first, uint32_t second, uint32_t word) {
    if (var) return sb_load(t, var, op2(t, SpvOpIAdd, t->t_u32, first, word));
    /* Only reached inside the validated node branch. A full box may cross
     * two unrelated host allocations, so resolve its two 64-byte units. */
    uint32_t upper = bvh_cmp(t, SpvOpUGreaterThanEqual, word, cu(t,16));
    uint32_t base = sel(t,t->t_u64,upper,second,first);
    uint32_t offset = op2(t,SpvOpShiftLeftLogical,t->t_u32,
        op2(t,SpvOpBitwiseAnd,t->t_u32,word,cu(t,15)),cu(t,2));
    uint32_t address = op2(t,SpvOpIAdd,t->t_u64,base,op1(t,SpvOpUConvert,t->t_u64,offset));
    uint32_t ptr = op1(t,120 /* OpConvertUToPtr */,spv_type_pointer(&t->b,5349,t->t_u32),address);
    return spv_op(&t->b,SpvOpLoad,t->t_u32,(uint32_t[]){ptr,2 /* Aligned */,4},3);
}
static void bvh_boxes(tr *t, uint32_t var, uint32_t first, uint32_t second, uint32_t half, uint32_t grow, uint32_t sort,
                      uint32_t extent, const uint32_t origin[3], const uint32_t inverse[3], uint32_t out[4]) {
    uint32_t F = t->t_f32, U = t->t_u32, zero = cf(t, 0), infinity = fbits(t, cu(t, 0x7f800000));
    uint32_t depths[4];
    for (uint32_t child = 0; child < 4; child++) {
        uint32_t bounds[6], packed[6], full[6];
        /* A full box occupies two node units. Only read its second unit in
         * the full branch, after the caller checked the complete span. */
        for (unsigned k = 0; k < 6; k++) {
            bounds[k] = spv_local_var(&t->b, t->p_fn_u32);
        }
        uint32_t merge = begin_if(t, half);
        for (unsigned k = 0; k < 3; k++) {
            uint32_t w = bvh_word(t, var, first, second, cu(t, 4 + child * 3 + k));
            uint32_t vec = ext1(t, t->t_v2f, GLSLstd450UnpackHalf2x16, w);
            for (unsigned c = 0; c < 2; c++) {
                packed[2*k+c] = op2(t, SpvOpCompositeExtract, F, vec, c);
                st(t, bounds[2*k+c], ubits(t, packed[2*k+c]));
            }
        }
        end_if(t, merge);
        merge = begin_if(t, op1(t, SpvOpLogicalNot, t->t_bool, half));
        for (unsigned k = 0; k < 6; k++) {
            full[k] = bvh_word(t, var, first, second, cu(t, 4 + child * 6 + k));
            st(t, bounds[k], full[k]);
        }
        end_if(t, merge);
        uint32_t near[3], far[3];
        for (unsigned axis = 0; axis < 3; axis++) {
            uint32_t lo = fbits(t, ld(t, U, bounds[axis]));
            uint32_t hi = fbits(t, ld(t, U, bounds[axis+3]));
            uint32_t forward = bvh_cmp(t, SpvOpFOrdGreaterThanEqual, inverse[axis], zero);
            uint32_t lower = sel(t, F, forward, lo, hi), upper = sel(t, F, forward, hi, lo);
            near[axis] = bvh_fop(t, SpvOpFMul, bvh_fop(t, SpvOpFSub, lower, origin[axis]), inverse[axis]);
            far[axis] = bvh_fop(t, SpvOpFMul, bvh_fop(t, SpvOpFSub, upper, origin[axis]), inverse[axis]);
        }
        uint32_t entry = ext2(t, F, GLSLstd450FMax, ext2(t,F,GLSLstd450FMax,near[0],near[1]),near[2]);
        uint32_t exit = ext2(t, F, GLSLstd450FMin, ext2(t,F,GLSLstd450FMin,far[0],far[1]),far[2]);
        uint32_t ebits = ubits(t, sel(t,F,bvh_cmp(t,SpvOpFOrdEqual,exit,zero),zero,exit));
        uint32_t grown = fbits(t,ext2(t,U,GLSLstd450UMin,op2(t,SpvOpIAdd,U,ebits,grow),cu(t,0x7f800000)));
        uint32_t hit = bvh_and(t,bvh_cmp(t,SpvOpFOrdGreaterThanEqual,exit,zero),bvh_cmp(t,SpvOpFOrdLessThan,entry,extent));
        hit = bvh_and(t,hit,bvh_cmp(t,SpvOpFOrdLessThanEqual,entry,grown));
        depths[child] = ubits(t,sel(t,F,hit,ext2(t,F,GLSLstd450FMax,entry,zero),infinity));
        out[child] = sel(t,U,hit,bvh_word(t,var,first,second,cu(t,child)),cu(t,UINT32_MAX));
    }
    static const unsigned network[5][2] = {{0,1},{2,3},{0,2},{1,3},{1,2}};
    for (unsigned step = 0; step < 5; step++) {
        unsigned a=network[step][0], b=network[step][1];
        uint32_t swap=bvh_and(t,sort,bvh_cmp(t,SpvOpUGreaterThan,depths[a],depths[b]));
        uint32_t da=depths[a], ia=out[a];
        depths[a]=sel(t,U,swap,depths[b],da); depths[b]=sel(t,U,swap,da,depths[b]);
        out[a]=sel(t,U,swap,out[b],ia); out[b]=sel(t,U,swap,ia,out[b]);
    }
}

static void bvh_triangle(tr *t, uint32_t var, uint32_t first, uint32_t second, uint32_t kind, uint32_t bary,
                         const uint32_t origin[3], const uint32_t direction[3], uint32_t out[4]) {
    uint32_t U=t->t_u32,F=t->t_f32,zero=cf(t,0);
    uint32_t eq0=bvh_cmp(t,SpvOpIEqual,kind,cu(t,0)),eq2=bvh_cmp(t,SpvOpIEqual,kind,cu(t,2)),eq3=bvh_cmp(t,SpvOpIEqual,kind,cu(t,3));
    uint32_t vertices[3]={ext2(t,U,GLSLstd450UMin,kind,cu(t,2)),sel(t,U,eq0,cu(t,1),sel(t,U,eq3,cu(t,4),cu(t,3))),sel(t,U,eq2,cu(t,4),sel(t,U,eq3,cu(t,0),cu(t,2)))};
    uint32_t dabs[3]; for(unsigned i=0;i<3;i++) dabs[i]=ext1(t,F,GLSLstd450FAbs,direction[i]);
    uint32_t yl=bvh_cmp(t,SpvOpFOrdLessThan,dabs[0],dabs[1]);
    uint32_t zl=bvh_cmp(t,SpvOpFOrdLessThan,sel(t,F,yl,dabs[1],dabs[0]),dabs[2]);
    uint32_t o[3],d[3];
    for(unsigned i=0;i<3;i++) {
        o[i]=sel(t,F,zl,origin[i],sel(t,F,yl,origin[(i+2)%3],origin[(i+1)%3]));
        d[i]=sel(t,F,zl,direction[i],sel(t,F,yl,direction[(i+2)%3],direction[(i+1)%3]));
    }
    uint32_t projected[3][3];
    for(unsigned i=0;i<3;i++) {
        uint32_t pos[3],rel[3];
        for(unsigned a=0;a<3;a++) pos[a]=fbits(t,bvh_word(t,var,first,second,op2(t,SpvOpIAdd,U,op2(t,SpvOpIMul,U,vertices[i],cu(t,3)),cu(t,a))));
        for(unsigned a=0;a<3;a++) rel[a]=bvh_fop(t,SpvOpFSub,sel(t,F,zl,pos[a],sel(t,F,yl,pos[(a+2)%3],pos[(a+1)%3])),o[a]);
        projected[i][0]=bvh_fop(t,SpvOpFSub,bvh_fop(t,SpvOpFMul,rel[0],d[2]),bvh_fop(t,SpvOpFMul,d[0],rel[2]));
        projected[i][1]=bvh_fop(t,SpvOpFSub,bvh_fop(t,SpvOpFMul,rel[1],d[2]),bvh_fop(t,SpvOpFMul,d[1],rel[2]));
        projected[i][2]=rel[2];
    }
    uint32_t edge[3],weight[3],negative=spv_const_bool(&t->b,false),positive=negative;
    for(unsigned i=0;i<3;i++) {
        unsigned a=(i+1)%3,b=(i+2)%3;
        edge[i]=bvh_fop(t,SpvOpFSub,bvh_fop(t,SpvOpFMul,projected[b][0],projected[a][1]),bvh_fop(t,SpvOpFMul,projected[b][1],projected[a][0]));
        weight[i]=bvh_fop(t,SpvOpFMul,edge[i],d[2]);
        negative=bvh_or(t,negative,bvh_cmp(t,SpvOpFOrdLessThan,edge[i],zero));
        positive=bvh_or(t,positive,bvh_cmp(t,SpvOpFOrdGreaterThan,edge[i],zero));
    }
    uint32_t num=bvh_fop(t,SpvOpFAdd,bvh_fop(t,SpvOpFAdd,bvh_fop(t,SpvOpFMul,edge[0],projected[0][2]),bvh_fop(t,SpvOpFMul,edge[1],projected[1][2])),bvh_fop(t,SpvOpFMul,edge[2],projected[2][2]));
    uint32_t den=bvh_fop(t,SpvOpFAdd,bvh_fop(t,SpvOpFAdd,weight[0],weight[1]),weight[2]);
    uint32_t winding=bvh_cmp(t,SpvOpFOrdGreaterThan,den,zero);
    uint32_t miss=bvh_or(t,bvh_and(t,negative,positive),bvh_cmp(t,SpvOpFOrdEqual,den,zero));
    miss=bvh_or(t,miss,op1(t,SpvOpIsNan,t->t_bool,num));
    miss=bvh_or(t,miss,bvh_cmp(t,SpvOpFOrdLessThan,sel(t,F,winding,num,op1(t,SpvOpFNegate,F,num)),zero));
    for(unsigned i=0;i<3;i++) {
        uint32_t a=projected[(i+1)%3][1],b=projected[(i+2)%3][1];
        uint32_t az=bvh_cmp(t,SpvOpFOrdEqual,a,zero), horizontal=bvh_and(t,az,bvh_cmp(t,SpvOpFOrdEqual,b,zero));
        uint32_t below=bvh_or(t,bvh_cmp(t,SpvOpFOrdLessThan,a,zero),bvh_and(t,az,bvh_cmp(t,SpvOpFOrdGreaterThan,b,zero)));
        uint32_t right=bvh_cmp(t,SpvOpLogicalNotEqual,below,winding);
        uint32_t excluded=sel(t,t->t_bool,horizontal,bvh_cmp(t,SpvOpFOrdGreaterThan,projected[i][1],zero),right);
        miss=bvh_or(t,miss,bvh_and(t,bvh_cmp(t,SpvOpFOrdEqual,edge[i],zero),excluded));
    }
    uint32_t flag=bvh_word(t,var,first,second,cu(t,15));
    uint32_t remap=op2(t,SpvOpShiftRightLogical,U,flag,op2(t,SpvOpIMul,U,kind,cu(t,8)));
    uint32_t coords[2];
    for(unsigned i=0;i<2;i++) {
        uint32_t ix=op2(t,SpvOpBitwiseAnd,U,op2(t,SpvOpShiftRightLogical,U,remap,cu(t,2*i)),cu(t,3));
        coords[i]=ubits(t,sel(t,F,bvh_cmp(t,SpvOpIEqual,ix,cu(t,1)),weight[1],sel(t,F,bvh_cmp(t,SpvOpIEqual,ix,cu(t,2)),weight[2],weight[0])));
    }
    out[0]=ubits(t,sel(t,F,miss,fbits(t,cu(t,0x7f800000)),num));
    out[1]=ubits(t,sel(t,F,miss,cf(t,1),den));
    out[2]=sel(t,U,bary,coords[0],op2(t,SpvOpIAdd,U,flag,kind));
    out[3]=sel(t,U,bary,coords[1],sel(t,U,miss,cu(t,0),cu(t,1)));
}

static void bvh_op(tr *t, const rdna2_inst *in, int site) {
    uint32_t U=t->t_u32,L=t->t_u64,F=t->t_f32,var=t->site_var[site];
    bool wide=in->op==0xe7; unsigned ray=wide?2:1, count=ray+4+(in->a16?3:6);
    uint32_t args[12],desc[4];
    for(unsigned i=0;i<count;i++) {
        if(in->nsa_count && i && i-1>=in->nsa_count) { fail(t,"truncated BVH NSA address list"); return; }
        args[i]=get_v(t,in->nsa_count && i?in->nsa_addr[i-1]:in->vaddr+i);
    }
    bool dynamic=t->shapes[site].address_map==3 || t->shapes[site].address_map==4;
    bool paged=t->shapes[site].address_map==4;
    uint32_t expected=dynamic?0:sb_load(t,t->u_var,cu(t,SH_U_SITE+(uint32_t)site));
    uint32_t valid=active(t);
    for(unsigned i=0;i<4;i++) {
        desc[i]=get_s(t,in->srsrc+i);
        if(!dynamic) {
            uint32_t saved=sb_load(t,t->u_var,op2(t,SpvOpIAdd,U,expected,cu(t,i)));
            valid=bvh_and(t,valid,bvh_cmp(t,SpvOpIEqual,desc[i],saved));
        }
    }
    uint32_t node=wide?bvh_pack(t,args[0],args[1]):op1(t,SpvOpUConvert,L,args[0]);
    uint32_t kind=op2(t,SpvOpBitwiseAnd,U,args[0],cu(t,7));
    uint32_t tri=bvh_cmp(t,SpvOpULessThan,kind,cu(t,4)),full=bvh_cmp(t,SpvOpIEqual,kind,cu(t,5));
    uint32_t index=op2(t,SpvOpShiftRightLogical,L,node,cu(t,3));
    uint32_t last=bvh_pack(t,desc[2],op2(t,SpvOpBitwiseAnd,U,desc[3],cu(t,1023)));
    uint32_t last_index=op2(t,SpvOpIAdd,L,index,sel(t,L,full,c64(t,1),c64(t,0)));
    valid=bvh_and(t,valid,bvh_cmp(t,SpvOpIEqual,op2(t,SpvOpShiftRightLogical,U,desc[3],cu(t,28)),cu(t,8)));
    valid=bvh_and(t,valid,bvh_cmp(t,SpvOpULessThanEqual,kind,cu(t,5)));
    valid=bvh_and(t,valid,bvh_cmp(t,SpvOpULessThanEqual,last_index,last));
    uint32_t first=0,second=0;
    if(dynamic) {
        uint32_t base=op2(t,SpvOpShiftLeftLogical,L,bvh_pack(t,desc[0],op2(t,SpvOpBitwiseAnd,U,desc[1],cu(t,255))),cu(t,8));
        uint32_t address=op2(t,SpvOpIAdd,L,base,op2(t,SpvOpShiftLeftLogical,L,index,cu(t,6)));
        uint32_t end=op2(t,SpvOpIAdd,L,address,sel(t,L,full,c64(t,127),c64(t,63)));
        valid=bvh_and(t,valid,bvh_cmp(t,SpvOpULessThan,end,c64(t,1ull<<48)));
        /* Invalid hardware addresses must not count as an attempted map read. */
        uint32_t a=spv_local_var(&t->b,spv_type_pointer(&t->b,SpvStorageClassFunction,L));
        uint32_t b=spv_local_var(&t->b,spv_type_pointer(&t->b,SpvStorageClassFunction,L));
        st(t,a,c64(t,0));st(t,b,c64(t,0));
        uint32_t branch=begin_if(t,valid);
        st(t,a,bvh_resolve(t,var,address,paged));
        uint32_t full_branch=begin_if(t,full);
        st(t,b,bvh_resolve(t,var,op2(t,SpvOpIAdd,L,address,c64(t,64)),paged));
        end_if(t,full_branch);end_if(t,branch);
        first=ld(t,L,a);second=ld(t,L,b);
        valid=bvh_and(t,valid,bvh_cmp(t,SpvOpINotEqual,first,c64(t,0)));
        valid=bvh_and(t,valid,bvh_or(t,op1(t,SpvOpLogicalNot,t->t_bool,full),bvh_cmp(t,SpvOpINotEqual,second,c64(t,0))));
    } else {
        uint32_t len=spv_op(&t->b,SpvOpArrayLength,U,(uint32_t[]){var,0},2);
        uint32_t units=op2(t,SpvOpShiftRightLogical,U,len,cu(t,4));
        valid=bvh_and(t,valid,bvh_cmp(t,SpvOpULessThan,last_index,op1(t,SpvOpUConvert,L,units)));
        first=op1(t,SpvOpUConvert,U,op2(t,SpvOpShiftLeftLogical,L,index,cu(t,4)));
    }
    uint32_t extent=fbits(t,args[ray]),origin[3],direction[3],inverse[3];
    valid=bvh_and(t,valid,op1(t,SpvOpLogicalNot,t->t_bool,op1(t,SpvOpIsNan,t->t_bool,extent)));
    for(unsigned axis=0;axis<3;axis++) {
        origin[axis]=fbits(t,args[ray+1+axis]);
        if(!in->a16) { direction[axis]=fbits(t,args[ray+4+axis]); inverse[axis]=fbits(t,args[ray+7+axis]); }
        else for(unsigned k=0;k<2;k++) {
            unsigned component=axis+3*k;
            uint32_t v=ext1(t,t->t_v2f,GLSLstd450UnpackHalf2x16,args[ray+4+component/2]);
            uint32_t value=op2(t,SpvOpCompositeExtract,F,v,component%2);
            if(k) inverse[axis]=value; else direction[axis]=value;
        }
        uint32_t values[3]={origin[axis],direction[axis],inverse[axis]};
        for(unsigned k=0;k<3;k++) valid=bvh_and(t,valid,op1(t,SpvOpLogicalNot,t->t_bool,op1(t,SpvOpIsNan,t->t_bool,values[k])));
        valid=bvh_and(t,valid,op1(t,SpvOpLogicalNot,t->t_bool,op1(t,SpvOpIsInf,t->t_bool,origin[axis])));
    }
    uint32_t result[4];
    for(unsigned k=0;k<4;k++) {
        uint32_t invalid= k==0?cu(t,0x7f800000):k==1?cu(t,0x3f800000):k==2?cu(t,UINT32_MAX):cu(t,0);
        result[k]=spv_local_var(&t->b,t->p_fn_u32); st(t,result[k],sel(t,U,tri,invalid,cu(t,UINT32_MAX)));
    }
    uint32_t merge=begin_if(t,valid);
    uint32_t branch=begin_if(t,tri),value[4];
    uint32_t bary=bvh_cmp(t,SpvOpINotEqual,op2(t,SpvOpBitwiseAnd,U,desc[3],cu(t,1u<<24)),cu(t,0));
    bvh_triangle(t,dynamic?0:var,first,second,kind,bary,origin,direction,value);
    for(unsigned k=0;k<4;k++) st(t,result[k],value[k]);
    end_if(t,branch);
    branch=begin_if(t,op1(t,SpvOpLogicalNot,t->t_bool,tri));
    uint32_t grow=bfe_u(t,desc[1],cu(t,23),cu(t,8));
    uint32_t sort=bvh_cmp(t,SpvOpINotEqual,op2(t,SpvOpBitwiseAnd,U,desc[1],cu(t,0x80000000)),cu(t,0));
    bvh_boxes(t,dynamic?0:var,first,second,bvh_cmp(t,SpvOpIEqual,kind,cu(t,4)),grow,sort,extent,origin,inverse,value);
    for(unsigned k=0;k<4;k++) st(t,result[k],value[k]);
    end_if(t,branch); end_if(t,merge);
    for(unsigned k=0;k<4;k++) set_v(t,in->vdata+k,ld(t,U,result[k]));
}
#endif
