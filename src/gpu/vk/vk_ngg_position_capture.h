/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bounded diagnostic of actual, fenced compute/replay positions. Private files
 * only; no replacement positions, altered visibility or resource contents. */
static void ngg_position_capture(renderer*r,const gpu*g,const gpu_draw*d,const stage_state*vs,const stage_state*ps) {
    static bool initialized;static uint64_t wanted[3];static unsigned count;
    static guest_addr depth;static frame_trig trigger;static int64_t previous=-1;
    static unsigned captures,draws[3],vertices[3];
    if(!initialized) {
        initialized=true;const char*s=getenv("PS5C_GS_POSITION_HASHES");
        while(s && *s && count<3) {
            char*end;uint64_t h=strtoull(s,&end,16);if(end==s)break;
            wanted[count++]=h;s=end;while(*s==' ' || *s==',')s++;
        }
        s=getenv("PS5C_GS_POSITION_DEPTH");depth=s?strtoull(s,NULL,16):0;
    }
    if(!count || !vs->info.mesh_compute)return;
    unsigned which=0;while(which<count && wanted[which]!=vs->prog->hash)which++;
    if(which==count)return;
    guest_addr za;uint32_t zw,zh;VkFormat zf;
    if(depth && (!depth_target(g,&za,&zw,&zh,&zf) || za!=depth))return;
    int64_t frame=frame_trigger(&trigger,"PS5C_GS_POSITION_CAPTURE",g->stats.flips);
    if(frame<0 || (uint64_t)frame!=g->stats.flips)return;
    if(frame!=previous) {
        previous=frame;if(captures++>=4)return;
        memset(draws,0,sizeof(draws));memset(vertices,0,sizeof(vertices));
    }
    if(captures>4 || draws[which]>=64 || vertices[which]>=262144)return;
    uint32_t stride=sh_mesh_output_words(vs->prog,&vs->info);
    uint32_t record=4+4*(uint32_t)__builtin_popcount(vs->prog->param_mask);
    if(!stride || vs->mesh_output_groups>r->pipes->work->arena.size/4/stride)return;
    uint64_t bytes=(uint64_t)vs->mesh_output_groups*stride*4;
    if(!stride || vs->mesh_output_off>r->pipes->work->arena.size ||
       bytes>r->pipes->work->arena.size-vs->mesh_output_off)return;
    unsigned draw=draws[which]++;
    end_pass(r);sync_batch(r);
    if(vkGetFenceStatus(r->device,r->work_fence)!=VK_SUCCESS)return;
    const uint32_t*data=arena_ptr(r,vs->mesh_output_off);uint32_t total=0;
    for(uint32_t group=0;group<vs->mesh_output_groups;group++) {
        uint32_t n=data[(uint64_t)group*stride+4];
        if(n>vs->info.mesh_max_vertices){vk_begin(r);return;}
        if(n>262144-vertices[which]-total)break;
        total+=n;
    }
    if(!total){vk_begin(r);return;}
    pal_mkdir("user");pal_mkdir("user/frames");char path[192];
    snprintf(path,sizeof(path),"user/frames/positions_f%llu_%016llx_%02u.bin",
        (unsigned long long)g->stats.flips,(unsigned long long)vs->prog->hash,draw);
    FILE*f=fopen(path,"wb");bool ok=f!=NULL;
    /* Stable little-endian host fixture format: 4Q+12I+128I, then total
     * records of {group, local vertex, four raw float32 position words}. */
    uint64_t q[4]={0x31534f5047474eull,vs->prog->hash,ps->prog->hash,g->stats.flips};
    uint32_t h[12]={total,vs->mesh_output_groups,vs->info.mesh_max_vertices,vs->prog->param_mask,
        d->count,gpu_reg(g,mmDB_DEPTH_CONTROL)};
    for(unsigned k=0;k<6;k++)h[6+k]=gpu_reg(g,mmPA_CL_VPORT_XSCALE+k);
    if(ok)ok=fwrite(q,sizeof(q),1,f)==1 && fwrite(h,sizeof(h),1,f)==1 &&
        fwrite(vs->eval.sgpr_init,sizeof(vs->eval.sgpr_init),1,f)==1;
    uint32_t saved=0;
    for(uint32_t group=0;ok && group<vs->mesh_output_groups && saved<total;group++) {
        const uint32_t*w=data+(uint64_t)group*stride;
        for(uint32_t v=0;ok && v<w[4] && saved<total;v++,saved++) {
            uint32_t row[6]={group,v};memcpy(row+2,w+8+(uint64_t)v*record,16);
            ok=fwrite(row,sizeof(row),1,f)==1;
        }
    }
    if(f && fclose(f)!=0)ok=false;
    if(!ok)pal_file_remove(path);
    else {vertices[which]+=total;LOG_I(GPU,"NGG positions captured: %s (%u vertices)",path,total);}
    vk_begin(r);
}
