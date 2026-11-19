/* SPDX-License-Identifier: GPL-2.0-or-later */
/* NGG mesh outputs. Included after the SPIR-V emitter helpers. Independent
 * Point or line GS or passthrough triangles, indexed or generated. Native mesh and
 * compute/replay share the guest ABI; all outputs must fit the host group. */
static uint32_t mesh_global(tr *t,uint32_t type,uint32_t storage) {
    uint32_t v=spv_id(&t->b);
    spv_emit(&t->b,SPV_SEC_GLOBAL,SpvOpVariable,
             (uint32_t[]){spv_type_pointer(&t->b,storage,type),v,storage},3);
    return v;
}
static uint32_t mesh_array(tr*t,uint32_t type,uint32_t count) {
    uint32_t v=mesh_global(t,spv_type_array(&t->b,type,cu(t,count)),SpvStorageClassOutput);
    t->iface[t->n_iface++]=v;return v;
}
static uint32_t mesh_element(tr*t,uint32_t var,uint32_t type,uint32_t index,uint32_t storage) {
    return spv_op(&t->b,SpvOpAccessChain,spv_type_pointer(&t->b,storage,type),(uint32_t[]){var,index},2);
}
static uint32_t mesh_count(tr*t,unsigned k) {
    return mesh_element(t,t->mesh_counts,t->t_u32,cu(t,k),SpvStorageClassWorkgroup);
}
static void mesh_declare(tr*t) {
    t->in_lid=input_var(t,t->t_v3u,SpvBuiltInLocalInvocationId);
    t->in_wgid=input_var(t,t->t_v3u,SpvBuiltInWorkgroupId);
    if(t->wave_groups || t->mesh_ballot32 || t->full_wave32) {
        t->in_sgid=input_var(t,t->t_u32,SpvBuiltInSubgroupLocalInvocationId);
        t->in_subgroup=input_var(t,t->t_u32,40 /* SubgroupId */);
        uint32_t subgroup_cap=SpvCapabilityGroupNonUniform;
        spv_emit(&t->b,SPV_SEC_CAPABILITY,SpvOpCapability,&subgroup_cap,1);
    }
    t->mesh_counts=mesh_global(t,spv_type_array(&t->b,t->t_u32,cu(t,2)),SpvStorageClassWorkgroup);
    t->mesh_position=mesh_global(t,t->t_v4f,SpvStorageClassPrivate);
    t->mesh_primitive=mesh_global(t,t->t_u32,SpvStorageClassPrivate);
    if(t->info->mesh_compute) {
        t->mesh_numgroups=input_var(t,t->t_v3u,24 /* NumWorkgroups */);
        t->mesh_output=mesh_global(t,t->t_ssbo,SpvStorageClassStorageBuffer);
        uint32_t zero=0,binding=t->info->mesh_output_binding;
        spv_decorate(&t->b,t->mesh_output,SpvDecorationDescriptorSet,&zero,1);
        spv_decorate(&t->b,t->mesh_output,SpvDecorationBinding,&binding,1);
        for(unsigned k=0;k<32;k++)if(t->p->param_mask>>k&1)
            t->mesh_attributes[k]=mesh_global(t,t->t_v4f,SpvStorageClassPrivate);
        return;
    }
    uint32_t cap=5283; /* MeshShadingEXT */
    spv_emit(&t->b,SPV_SEC_CAPABILITY,SpvOpCapability,&cap,1);
    uint32_t ext[16],n=spv_string("SPV_EXT_mesh_shader",ext,16);
    spv_emit(&t->b,SPV_SEC_EXTENSION,10,ext,n);
    t->mesh_position_out=mesh_array(t,t->t_v4f,t->info->mesh_max_vertices);
    t->mesh_indices_out=mesh_array(t,t->t_v3u,t->info->mesh_max_primitives);
    t->mesh_cull_out=mesh_array(t,t->t_bool,t->info->mesh_max_primitives);
    uint32_t bi=SpvBuiltInPosition;spv_decorate(&t->b,t->mesh_position_out,SpvDecorationBuiltIn,&bi,1);
    bi=5296;spv_decorate(&t->b,t->mesh_indices_out,SpvDecorationBuiltIn,&bi,1);
    bi=5299;spv_decorate(&t->b,t->mesh_cull_out,SpvDecorationBuiltIn,&bi,1);
    spv_decorate(&t->b,t->mesh_cull_out,5271,NULL,0); /* PerPrimitiveEXT */
    for(unsigned k=0;k<32;k++)if((t->p->param_mask>>k)&1u) {
        t->mesh_attributes[k]=mesh_global(t,t->t_v4f,SpvStorageClassPrivate);
        t->out_param[k]=mesh_array(t,t->t_v4f,t->info->mesh_max_vertices);
        spv_decorate(&t->b,t->out_param[k],SpvDecorationLocation,&k,1);
        if((t->info->vs_param_alias_mask>>k)&1u) {
            uint32_t at=t->info->vs_param_alias[k];
            t->out_alias[k]=mesh_array(t,t->t_v4f,t->info->mesh_max_vertices);
            spv_decorate(&t->b,t->out_alias[k],SpvDecorationLocation,&at,1);
            spv_decorate(&t->b,t->out_alias[k],SpvDecorationFlat,NULL,0);
        }
    }
}
static void mesh_allocate(tr*t) {
    uint32_t own=begin_if(t,op2(t,SpvOpIEqual,t->t_bool,local_flat(t),cu(t,0)));
    uint32_t m0=get_s(t,RDNA2_OP_M0);
    st(t,mesh_count(t,0),op3(t,SpvOpBitFieldUExtract,t->t_u32,m0,cu(t,0),cu(t,10)));
    st(t,mesh_count(t,1),op3(t,SpvOpBitFieldUExtract,t->t_u32,m0,cu(t,12),cu(t,11)));
    end_if(t,own);
}
static void mesh_export(tr*t,const rdna2_inst*in,uint32_t value,const uint32_t c[4]) {
    uint32_t join=begin_if(t,active(t)),target=in->exp_target;
    if(target==12) {
        uint32_t f=t->t_f32,w=c[3],out[4];
        for(unsigned k=0;k<2;k++) {
            uint32_t sc=fbits(t,sb_load(t,t->u_var,cu(t,SH_U_VPORT+2*k)));
            uint32_t of=fbits(t,sb_load(t,t->u_var,cu(t,SH_U_VPORT+2*k+1)));
            uint32_t inv=fbits(t,sb_load(t,t->u_var,cu(t,SH_U_VPORT+4+k)));
            uint32_t px=op2(t,SpvOpFAdd,f,op2(t,SpvOpFMul,f,c[k],sc),op2(t,SpvOpFMul,f,w,of));
            out[k]=op2(t,SpvOpFSub,f,op2(t,SpvOpFMul,f,px,inv),w);
        }
        uint32_t zs=fbits(t,sb_load(t,t->u_var,cu(t,SH_U_VPORT+6)));
        uint32_t zo=fbits(t,sb_load(t,t->u_var,cu(t,SH_U_VPORT+7)));
        out[2]=op2(t,SpvOpFAdd,f,op2(t,SpvOpFMul,f,c[2],zs),op2(t,SpvOpFMul,f,w,zo));out[3]=w;
        value=spv_op(&t->b,SpvOpCompositeConstruct,t->t_v4f,out,4);
        st(t,t->mesh_position,value);
    } else if(target>=32 && target<64 && t->mesh_attributes[target-32])st(t,t->mesh_attributes[target-32],value);
    end_if(t,join);
}
uint32_t sh_mesh_output_words(const sh_program*p,const sh_stage_info*info) {
    return 8+info->mesh_max_vertices*(4+4*(uint32_t)__builtin_popcount(p->param_mask))+info->mesh_max_primitives;
}
static void mesh_compute_finish(tr*t,uint32_t vertices,uint32_t prims) {
    uint32_t u=t->t_u32,b=t->t_bool,lane=local_flat(t),stride=sh_mesh_output_words(t->p,t->info);
    uint32_t id=ld(t,t->t_v3u,t->in_wgid),ng=ld(t,t->t_v3u,t->mesh_numgroups);
    uint32_t group=op2(t,SpvOpIAdd,u,op2(t,SpvOpCompositeExtract,u,id,0),
        op2(t,SpvOpIMul,u,op2(t,SpvOpCompositeExtract,u,id,1),op2(t,SpvOpCompositeExtract,u,ng,0)));
    uint32_t base=op2(t,SpvOpIMul,u,group,cu(t,stride));
    uint32_t own=begin_if(t,op2(t,SpvOpIEqual,b,lane,cu(t,0)));
    uint32_t values[]={op2(t,SpvOpIMul,u,prims,cu(t,3)),cu(t,1),
        op2(t,SpvOpIMul,u,group,cu(t,t->info->mesh_max_primitives*3)),cu(t,0),vertices,prims,cu(t,0),cu(t,0)};
    for(unsigned k=0;k<8;k++)st(t,sb_ptr(t,t->mesh_output,op2(t,SpvOpIAdd,u,base,cu(t,k))),values[k]);
    end_if(t,own);
    uint32_t record=4+4*(uint32_t)__builtin_popcount(t->p->param_mask);
    uint32_t at=op2(t,SpvOpIAdd,u,base,op2(t,SpvOpIAdd,u,cu(t,8),op2(t,SpvOpIMul,u,lane,cu(t,record))));
    own=begin_if(t,op2(t,SpvOpULessThan,b,lane,vertices));
    unsigned slot=0;
    for(unsigned k=0;k<33;k++) {
        uint32_t var=k?t->mesh_attributes[k-1]:t->mesh_position;
        if(!var)continue;
        uint32_t value=ld(t,t->t_v4f,var);
        for(unsigned c=0;c<4;c++)st(t,sb_ptr(t,t->mesh_output,op2(t,SpvOpIAdd,u,at,cu(t,4*slot+c))),
            ubits(t,op2(t,SpvOpCompositeExtract,t->t_f32,value,c)));
        slot++;
    }
    end_if(t,own);
    own=begin_if(t,op2(t,SpvOpULessThan,b,lane,prims));
    at=op2(t,SpvOpIAdd,u,base,op2(t,SpvOpIAdd,u,cu(t,8+t->info->mesh_max_vertices*record),lane));
    st(t,sb_ptr(t,t->mesh_output,at),ld(t,u,t->mesh_primitive));
    end_if(t,own);
}
static void mesh_finish(tr*t) {
    spv_op_void(&t->b,SpvOpControlBarrier,(uint32_t[]){cu(t,2),cu(t,2),cu(t,0x108)},3);
    uint32_t u=t->t_u32,b=t->t_bool,lane=local_flat(t),vertices=ld(t,u,mesh_count(t,0)),prims=ld(t,u,mesh_count(t,1));
    if(t->vs_probe) {
        /* Reuse the bounded VS diagnostic tail, one lane in the first mesh
         * group only. This must precede count clamping so invalid allocation
         * requests remain visible without allowing out-of-bounds exports. */
        uint32_t group=ld(t,t->t_v3u,t->in_wgid);
        uint32_t length=sb_length(t,t->u_var);
        uint32_t base=op2(t,SpvOpISub,u,length,cu(t,32));
        uint32_t first=op2(t,SpvOpLogicalAnd,b,
            op2(t,SpvOpIEqual,b,op2(t,SpvOpCompositeExtract,u,group,0),cu(t,0)),
            op2(t,SpvOpIEqual,b,lane,cu(t,t->vs_probe_vid)));
        uint32_t join=begin_if(t,first);
        uint32_t values[]={vertices,prims,cu(t,t->info->mesh_max_vertices),cu(t,t->info->mesh_max_primitives),ld(t,u,t->mesh_primitive)};
        for(unsigned k=0;k<5;k++)st(t,sb_ptr(t,t->u_var,op2(t,SpvOpIAdd,u,base,cu(t,k))),values[k]);
        uint32_t position=ld(t,t->t_v4f,t->mesh_position);
        const unsigned slots[]={5,6,7,14};
        for(unsigned k=0;k<4;k++) {
            uint32_t value=k<t->probe_count?ld(t,u,t->probe_vars[k]):
                ubits(t,op2(t,SpvOpCompositeExtract,t->t_f32,position,k));
            st(t,sb_ptr(t,t->u_var,op2(t,SpvOpIAdd,u,base,cu(t,slots[k]))),value);
        }
        for(unsigned k=4;k<t->probe_count && k<8;k++)
            st(t,sb_ptr(t,t->u_var,op2(t,SpvOpIAdd,u,base,cu(t,23+k-4))),ld(t,u,t->probe_vars[k]));
        st(t,sb_ptr(t,t->u_var,op2(t,SpvOpIAdd,u,base,cu(t,15))),lane);
        end_if(t,join);
        join=begin_if(t,op2(t,SpvOpIEqual,b,lane,cu(t,0)));
        uint32_t sums[]={vertices,prims,cu(t,1),vertices,prims};
        for(unsigned k=0;k<5;k++) {
            uint32_t ptr=sb_ptr(t,t->u_var,op2(t,SpvOpIAdd,u,base,cu(t,8+k)));
            spv_op(&t->b,k<3?SpvOpAtomicIAdd:SpvOpAtomicUMax,u,(uint32_t[]){ptr,cu(t,1),cu(t,0),sums[k]},4);
        }
        end_if(t,join);
    }
    uint32_t valid=op2(t,SpvOpLogicalAnd,b,op2(t,SpvOpULessThanEqual,b,vertices,cu(t,t->info->mesh_max_vertices)),
                       op2(t,SpvOpULessThanEqual,b,prims,cu(t,t->info->mesh_max_primitives)));
    vertices=sel(t,u,valid,vertices,cu(t,0));prims=sel(t,u,valid,prims,cu(t,0));
    if(t->info->mesh_compute){mesh_compute_finish(t,vertices,prims);return;}
    spv_op_void(&t->b,5295,(uint32_t[]){vertices,prims},2); /* SetMeshOutputsEXT */
    uint32_t join=begin_if(t,op2(t,SpvOpULessThan,b,lane,vertices));
    uint32_t position=ld(t,t->t_v4f,t->mesh_position);
    st(t,mesh_element(t,t->mesh_position_out,t->t_v4f,lane,SpvStorageClassOutput),position);
    if(t->vs_probe) {
        uint32_t xyz[4];for(unsigned k=0;k<4;k++)xyz[k]=op2(t,SpvOpCompositeExtract,t->t_f32,position,k);
        uint32_t positive=op2(t,SpvOpFOrdGreaterThan,b,xyz[3],cf(t,0));
        uint32_t negative=op2(t,SpvOpFOrdLessThan,b,xyz[3],cf(t,0));
        uint32_t zero=op2(t,SpvOpFOrdEqual,b,xyz[3],cf(t,0));
        uint32_t inside=positive,nan=spv_const_bool(&t->b,false),inf=nan;
        for(unsigned k=0;k<4;k++) {
            nan=op2(t,SpvOpLogicalOr,b,nan,op1(t,SpvOpIsNan,b,xyz[k]));
            inf=op2(t,SpvOpLogicalOr,b,inf,op1(t,SpvOpIsInf,b,xyz[k]));
            if(k<2)inside=op2(t,SpvOpLogicalAnd,b,inside,op2(t,SpvOpFOrdLessThanEqual,b,ext1(t,t->t_f32,GLSLstd450FAbs,xyz[k]),xyz[3]));
        }
        inside=op2(t,SpvOpLogicalAnd,b,inside,op2(t,SpvOpFOrdGreaterThanEqual,b,xyz[2],cf(t,0)));
        inside=op2(t,SpvOpLogicalAnd,b,inside,op2(t,SpvOpFOrdLessThanEqual,b,xyz[2],xyz[3]));
        uint32_t flags[]={spv_const_bool(&t->b,true),positive,negative,zero,inside,nan,inf};
        uint32_t length=sb_length(t,t->u_var);
        uint32_t base=op2(t,SpvOpISub,u,length,cu(t,32));
        for(unsigned k=0;k<7;k++)spv_op(&t->b,SpvOpAtomicIAdd,u,
            (uint32_t[]){sb_ptr(t,t->u_var,op2(t,SpvOpIAdd,u,base,cu(t,16+k))),cu(t,1),cu(t,0),sel(t,u,flags[k],cu(t,1),cu(t,0))},4);
    }
    for(unsigned k=0;k<32;k++)if(t->mesh_attributes[k]) {
        uint32_t v=ld(t,t->t_v4f,t->mesh_attributes[k]);
        st(t,mesh_element(t,t->out_param[k],t->t_v4f,lane,SpvStorageClassOutput),v);
        if(t->out_alias[k])st(t,mesh_element(t,t->out_alias[k],t->t_v4f,lane,SpvStorageClassOutput),v);
    }
    end_if(t,join);
    join=begin_if(t,op2(t,SpvOpULessThan,b,lane,prims));
    uint32_t packed=ld(t,u,t->mesh_primitive),idx[3];
    uint32_t culled=op2(t,SpvOpINotEqual,b,op2(t,SpvOpBitwiseAnd,u,packed,cu(t,0x80000000u)),cu(t,0));
    for(unsigned k=0;k<3;k++) {
        uint32_t x=op3(t,SpvOpBitFieldUExtract,u,packed,cu(t,10*k),cu(t,9));
        uint32_t fits=op2(t,SpvOpULessThan,b,x,vertices);
        culled=op2(t,SpvOpLogicalOr,b,culled,op1(t,SpvOpLogicalNot,b,fits));idx[k]=sel(t,u,fits,x,cu(t,0));
    }
    if(t->vs_probe) {
        uint32_t triangle=op1(t,SpvOpLogicalNot,b,culled);
        for(unsigned k=0;k<3;k++)triangle=op2(t,SpvOpLogicalAnd,b,triangle,
            op2(t,SpvOpINotEqual,b,idx[k],idx[(k+1)%3]));
        uint32_t length=sb_length(t,t->u_var);
        uint32_t ptr=sb_ptr(t,t->u_var,op2(t,SpvOpISub,u,length,cu(t,19)));
        spv_op(&t->b,SpvOpAtomicIAdd,u,(uint32_t[]){ptr,cu(t,1),cu(t,0),sel(t,u,triangle,cu(t,1),cu(t,0))},4);
    }
    st(t,mesh_element(t,t->mesh_indices_out,t->t_v3u,lane,SpvStorageClassOutput),spv_op(&t->b,SpvOpCompositeConstruct,t->t_v3u,idx,3));
    st(t,mesh_element(t,t->mesh_cull_out,b,lane,SpvStorageClassOutput),culled);end_if(t,join);
}
static uint32_t mesh_pop(tr*t,uint32_t op,uint32_t type,const uint32_t*args,unsigned n) {
    return spv_prologue_op(&t->b,op,type,args,n);
}
static uint32_t mesh_uniform(tr*t,unsigned at) {
    uint32_t ptr=mesh_pop(t,SpvOpAccessChain,t->p_sb_u32,(uint32_t[]){t->u_var,cu(t,0),cu(t,sh_uniform_index(t->uniform_shift,at))},3);
    return mesh_pop(t,SpvOpLoad,t->t_u32,&ptr,1);
}
static void mesh_prologue(tr*t) {
    uint32_t u=t->t_u32,wg=mesh_pop(t,SpvOpLoad,t->t_v3u,&t->in_wgid,1),lid=mesh_pop(t,SpvOpLoad,t->t_v3u,&t->in_lid,1);
    uint32_t local=mesh_pop(t,SpvOpCompositeExtract,u,(uint32_t[]){lid,0},2);
    if(t->wave_groups || t->mesh_ballot32 || t->full_wave32) {
        uint32_t subgroup=mesh_pop(t,SpvOpLoad,u,&t->in_subgroup,1);
        uint32_t sublane=mesh_pop(t,SpvOpLoad,u,&t->in_sgid,1);
        local=mesh_pop(t,SpvOpIMul,u,(uint32_t[]){subgroup,cu(t,32)},2);
        local=mesh_pop(t,SpvOpIAdd,u,(uint32_t[]){local,sublane},2);
        if(t->wave_groups) {
            uint32_t wave=mesh_pop(t,SpvOpShiftRightLogical,u,(uint32_t[]){local,cu(t,6)},2);
            uint32_t ptr=mesh_pop(t,SpvOpAccessChain,spv_type_pointer(&t->b,SpvStorageClassWorkgroup,u),(uint32_t[]){t->wave_arrivals,wave},2);
            spv_prologue_void(&t->b,SpvOpAtomicStore,(uint32_t[]){ptr,cu(t,2),cu(t,0x104),cu(t,0)},4);
            spv_prologue_void(&t->b,SpvOpStore,(uint32_t[]){t->wave_generation,cu(t,0)},2);
            spv_prologue_void(&t->b,SpvOpControlBarrier,(uint32_t[]){cu(t,2),cu(t,2),cu(t,0x108)},3);
        }
    }
    uint32_t group=mesh_pop(t,SpvOpCompositeExtract,u,(uint32_t[]){wg,0},2);
    uint32_t chunk=mesh_pop(t,SpvOpIMul,u,(uint32_t[]){group,cu(t,t->info->mesh_input_vertices)},2);
    uint32_t count=mesh_uniform(t,SH_U_MESH),base=mesh_uniform(t,SH_U_MESH+1),instance=mesh_uniform(t,SH_U_MESH+2);
    uint32_t gt=mesh_pop(t,SpvOpUGreaterThan,t->t_bool,(uint32_t[]){count,chunk},2);
    uint32_t rem=mesh_pop(t,SpvOpISub,u,(uint32_t[]){count,chunk},2);
    rem=mesh_pop(t,SpvOpSelect,u,(uint32_t[]){gt,rem,cu(t,0)},3);
    uint32_t small=mesh_pop(t,SpvOpULessThan,t->t_bool,(uint32_t[]){rem,cu(t,t->info->mesh_input_vertices)},2);
    uint32_t n=mesh_pop(t,SpvOpSelect,u,(uint32_t[]){small,rem,cu(t,t->info->mesh_input_vertices)},3);
    uint32_t s2a=mesh_pop(t,SpvOpShiftLeftLogical,u,(uint32_t[]){n,cu(t,12)},2);
    uint32_t primitive_vertices=t->info->mesh_primitive_vertices?t->info->mesh_primitive_vertices:1;
    uint32_t np=mesh_pop(t,SpvOpUDiv,u,(uint32_t[]){n,cu(t,primitive_vertices)},2);
    uint32_t s2b=mesh_pop(t,SpvOpShiftLeftLogical,u,(uint32_t[]){np,cu(t,22)},2);
    uint32_t s2=mesh_pop(t,SpvOpBitwiseOr,u,(uint32_t[]){s2a,s2b},2);
    uint32_t vn=n,pn=np,wave_bits=cu(t,0x10000000u);
    if(t->wave_groups || t->full_wave32) {
        uint32_t shift=t->full_wave32?5:6;
        uint32_t wave=mesh_pop(t,SpvOpShiftRightLogical,u,(uint32_t[]){local,cu(t,shift)},2);
        uint32_t first_wave=mesh_pop(t,SpvOpIEqual,t->t_bool,(uint32_t[]){wave,cu(t,0)},2);
        vn=mesh_pop(t,SpvOpSelect,u,(uint32_t[]){first_wave,n,cu(t,0)},3);
        pn=mesh_pop(t,SpvOpSelect,u,(uint32_t[]){first_wave,np,cu(t,0)},3);
        wave_bits=mesh_pop(t,SpvOpShiftLeftLogical,u,(uint32_t[]){wave,cu(t,24)},2);
        wave_bits=mesh_pop(t,SpvOpBitwiseOr,u,(uint32_t[]){wave_bits,cu(t,(t->info->local_size[0]>>shift)<<28)},2);
    }
    uint32_t s3a=mesh_pop(t,SpvOpShiftLeftLogical,u,(uint32_t[]){pn,cu(t,8)},2);
    uint32_t s3=mesh_pop(t,SpvOpBitwiseOr,u,(uint32_t[]){vn,s3a},2);
    s3=mesh_pop(t,SpvOpBitwiseOr,u,(uint32_t[]){s3,wave_bits},2);
    spv_prologue_void(&t->b,SpvOpStore,(uint32_t[]){sreg(t,2),s2},2);
    spv_prologue_void(&t->b,SpvOpStore,(uint32_t[]){sreg(t,3),s3},2);
    uint32_t first=mesh_pop(t,SpvOpIMul,u,(uint32_t[]){local,cu(t,primitive_vertices)},2);
    uint32_t v0=mesh_pop(t,SpvOpShiftLeftLogical,u,(uint32_t[]){first,cu(t,2)},2),v1=cu(t,0);
    if(primitive_vertices==2) {
        /* a real GS on lines: the two vertex offsets [15:2] and [31:18], like the point's */
        uint32_t second=mesh_pop(t,SpvOpIAdd,u,(uint32_t[]){first,cu(t,1)},2);
        second=mesh_pop(t,SpvOpShiftLeftLogical,u,(uint32_t[]){second,cu(t,18)},2);
        v0=mesh_pop(t,SpvOpBitwiseOr,u,(uint32_t[]){v0,second},2);
    }
    if(primitive_vertices==3) {
        uint32_t second=mesh_pop(t,SpvOpIAdd,u,(uint32_t[]){first,cu(t,1)},2);
        uint32_t third=mesh_pop(t,SpvOpIAdd,u,(uint32_t[]){first,cu(t,2)},2);
        if(t->info->mesh_indexed&SH_MESH_NGG_CULL) {
            /* Culling shaders address per-vertex LDS records with these
             * unpacked 16-bit indices, then construct their own export. */
            second=mesh_pop(t,SpvOpShiftLeftLogical,u,(uint32_t[]){second,cu(t,16)},2);
            v0=mesh_pop(t,SpvOpBitwiseOr,u,(uint32_t[]){first,second},2);
            v1=third;
        } else {
            /* Passthrough receives the final 10-bit primitive-export word. */
            second=mesh_pop(t,SpvOpShiftLeftLogical,u,(uint32_t[]){second,cu(t,10)},2);
            third=mesh_pop(t,SpvOpShiftLeftLogical,u,(uint32_t[]){third,cu(t,20)},2);
            v0=mesh_pop(t,SpvOpBitwiseOr,u,(uint32_t[]){first,second},2);
            v0=mesh_pop(t,SpvOpBitwiseOr,u,(uint32_t[]){v0,third},2);
        }
    }
    uint32_t vid=mesh_pop(t,SpvOpIAdd,u,(uint32_t[]){chunk,local},2);
    bool fast=(t->info->mesh_indexed&SH_MESH_FAST_LAUNCH)!=0;
    /* Fast Launch1 broadcasts one input point's base vertex to all lanes.
     * Its compact ABI places InstanceID in v6, not normal merged ES/GS v8
     * (AMD RDNA3 ISA3.5.4, Fast Launch1 combined). v6 is not a user VGPR in
     * this mode; leaving it undefined corrupts an object table index. */
    if(fast)vid=chunk;
    if(t->info->mesh_indexed&2u) {
        /* Rasterization of strips alternates winding. Keep original vertex
         * IDs while duplicating the shared endpoints in the output list. */
        uint32_t tri=mesh_pop(t,SpvOpUDiv,u,(uint32_t[]){vid,cu(t,3)},2);
        uint32_t corner=mesh_pop(t,SpvOpUMod,u,(uint32_t[]){vid,cu(t,3)},2);
        uint32_t odd=mesh_pop(t,SpvOpBitwiseAnd,u,(uint32_t[]){tri,cu(t,1)},2);
        uint32_t flip=mesh_pop(t,SpvOpULessThan,t->t_bool,(uint32_t[]){corner,cu(t,2)},2);
        uint32_t swapped=mesh_pop(t,SpvOpBitwiseXor,u,(uint32_t[]){corner,odd},2);
        corner=mesh_pop(t,SpvOpSelect,u,(uint32_t[]){flip,swapped,corner},3);
        vid=mesh_pop(t,SpvOpIAdd,u,(uint32_t[]){tri,corner},2);
    }
    if(t->info->mesh_indexed&1u) {
        /* EXEC-off lanes still run the uniform prologue. Read element zero
         * outside the input range; the guest EXEC mask excludes that lane. */
        uint32_t inside=mesh_pop(t,SpvOpULessThan,t->t_bool,(uint32_t[]){vid,count},2);
        vid=mesh_pop(t,SpvOpSelect,u,(uint32_t[]){inside,vid,cu(t,0)},3);
        uint32_t start=mesh_uniform(t,SH_U_MESH+3);
        uint32_t at=mesh_pop(t,SpvOpIAdd,u,(uint32_t[]){start,vid},2);
        /* The renderer appends this table after logical smem_words (at
         * least SH_U_DATA) and checks that start + count cannot wrap. */
        if(t->uniform_shift)at=mesh_pop(t,SpvOpISub,u,(uint32_t[]){at,cu(t,t->uniform_shift)},2);
        uint32_t ptr=mesh_pop(t,SpvOpAccessChain,t->p_sb_u32,(uint32_t[]){t->u_var,cu(t,0),at},3);
        vid=mesh_pop(t,SpvOpLoad,u,&ptr,1);
    }
    vid=mesh_pop(t,SpvOpIAdd,u,(uint32_t[]){vid,base},2);
    uint32_t iid=mesh_pop(t,SpvOpCompositeExtract,u,(uint32_t[]){wg,1},2);iid=mesh_pop(t,SpvOpIAdd,u,(uint32_t[]){iid,instance},2);
    spv_prologue_void(&t->b,SpvOpStore,(uint32_t[]){vreg(t,0),v0},2);
    spv_prologue_void(&t->b,SpvOpStore,(uint32_t[]){vreg(t,1),v1},2);
    spv_prologue_void(&t->b,SpvOpStore,(uint32_t[]){vreg(t,5),vid},2);
    if(fast) {
        spv_prologue_void(&t->b,SpvOpStore,(uint32_t[]){vreg(t,2),chunk},2);
        spv_prologue_void(&t->b,SpvOpStore,(uint32_t[]){vreg(t,6),iid},2);
    } else spv_prologue_void(&t->b,SpvOpStore,(uint32_t[]){vreg(t,8),iid},2);
    spv_prologue_void(&t->b,SpvOpStore,(uint32_t[]){t->mesh_primitive,cu(t,0x80000000u)},2);
}
