/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Vertex replay of actual NGG outputs. No CPU readback or fabricated geometry.
 * The indirect draw for each group selects only the primitives it emitted. */
bool sh_mesh_replay(const sh_program*p,const sh_stage_info*info,uint32_t**spirv,size_t*words) {
    tr *t=calloc(1,sizeof(*t));sh_program *empty=calloc(1,sizeof(*empty));
    if(!t || !empty){free(t);free(empty);return false;}
    sh_stage_info vi=*info;vi.stage=SH_STAGE_VS;vi.vs_point_size=vi.vs_layer=vi.vs_ucp_mask=0;
    empty->param_mask=p->param_mask;t->p=empty;t->info=&vi;t->ok=true;t->binding_base=info->mesh_output_binding;
    spv_builder*b=&t->b;spv_init(b);
    /* Replay is the final position-producing stage, so it needs the same
     * invariant contract as an ordinary VS, including across varying masks. */
    const char*exact=getenv("PS5C_SHADER_EXACT");b->exact=!exact || atoi(exact)>=1;
    uint32_t cap=SpvCapabilityShader;
    spv_emit(b,SPV_SEC_CAPABILITY,SpvOpCapability,&cap,1);
    spv_emit(b,SPV_SEC_MEMORY_MODEL,SpvOpMemoryModel,(uint32_t[]){0,1},2);
    t->t_void=spv_type_void(b);t->t_bool=spv_type_bool(b);
    t->t_u32=spv_type_int(b,32,false);t->t_i32=spv_type_int(b,32,true);t->t_f32=spv_type_float(b,32);
    t->t_v4f=spv_type_vector(b,t->t_f32,4);t->t_v3u=spv_type_vector(b,t->t_u32,3);
    declare_resources(t);declare_io(t);
    uint32_t fn=spv_id(b);spv_begin_function(b,t->t_void,spv_type_function(b,t->t_void,NULL,0),fn);label(t);
    uint32_t u=t->t_u32,bo=t->t_bool;
    uint32_t vid=op1(t,SpvOpBitcast,u,ld(t,t->t_i32,t->in_vid));
    uint32_t group=op2(t,SpvOpUDiv,u,vid,cu(t,info->mesh_max_primitives*3));
    uint32_t primitive=op2(t,SpvOpUDiv,u,op2(t,SpvOpUMod,u,vid,cu(t,info->mesh_max_primitives*3)),cu(t,3));
    uint32_t corner=op2(t,SpvOpUMod,u,vid,cu(t,3));
    uint32_t base=op2(t,SpvOpIMul,u,group,cu(t,sh_mesh_output_words(p,info)));
    uint32_t vertices=sb_load(t,t->u_var,op2(t,SpvOpIAdd,u,base,cu(t,4)));
    uint32_t prims=sb_load(t,t->u_var,op2(t,SpvOpIAdd,u,base,cu(t,5)));
    uint32_t record=4+4*(uint32_t)__builtin_popcount(p->param_mask);
    uint32_t at=op2(t,SpvOpIAdd,u,base,op2(t,SpvOpIAdd,u,cu(t,8+info->mesh_max_vertices*record),primitive));
    uint32_t packed=sb_load(t,t->u_var,at);
    uint32_t shift=op2(t,SpvOpIMul,u,corner,cu(t,10));
    uint32_t index=op2(t,SpvOpBitwiseAnd,u,op2(t,SpvOpShiftRightLogical,u,packed,shift),cu(t,511));
    uint32_t valid=op2(t,SpvOpLogicalAnd,bo,op2(t,SpvOpULessThan,bo,primitive,prims),
        op2(t,SpvOpULessThanEqual,bo,vertices,cu(t,info->mesh_max_vertices)));
    valid=op2(t,SpvOpLogicalAnd,bo,valid,op2(t,SpvOpIEqual,bo,op2(t,SpvOpBitwiseAnd,u,packed,cu(t,0x80000000u)),cu(t,0)));
    for(unsigned k=0;k<3;k++)valid=op2(t,SpvOpLogicalAnd,bo,valid,
        op2(t,SpvOpULessThan,bo,op3(t,SpvOpBitFieldUExtract,u,packed,cu(t,k*10),cu(t,9)),vertices));
    uint32_t position=op2(t,SpvOpAccessChain,spv_type_pointer(b,SpvStorageClassOutput,t->t_v4f),t->pervertex,spv_const_i32(b,0));
    uint32_t zero=spv_op(b,SpvOpCompositeConstruct,t->t_v4f,(uint32_t[]){cf(t,0),cf(t,0),cf(t,0),cf(t,0)},4);
    st(t,position,zero);
    for(unsigned k=0;k<32;k++){if(t->out_param[k])st(t,t->out_param[k],zero);if(t->out_alias[k])st(t,t->out_alias[k],zero);}
    uint32_t own=begin_if(t,valid);
    at=op2(t,SpvOpIAdd,u,base,op2(t,SpvOpIAdd,u,cu(t,8),op2(t,SpvOpIMul,u,index,cu(t,record))));
    unsigned slot=0;
    for(unsigned k=0;k<33;k++) {
        if(k && !(p->param_mask>>(k-1)&1))continue;
        uint32_t c[4];for(unsigned j=0;j<4;j++)c[j]=fbits(t,sb_load(t,t->u_var,op2(t,SpvOpIAdd,u,at,cu(t,slot*4+j))));
        uint32_t v=spv_op(b,SpvOpCompositeConstruct,t->t_v4f,c,4);
        if(!k)st(t,position,v);
        else {st(t,t->out_param[k-1],v);if(t->out_alias[k-1])st(t,t->out_alias[k-1],v);}
        slot++;
    }
    end_if(t,own);spv_op_void(b,SpvOpReturn,NULL,0);spv_end_function(b);
    add_all_globals(b,SpvExecutionModelVertex,fn);
    bool ok=t->ok && spv_finish(b,spirv,words);if(ok)(*spirv)[1]=0x00010400;
    spv_free(b);free(empty);free(t);return ok;
}
