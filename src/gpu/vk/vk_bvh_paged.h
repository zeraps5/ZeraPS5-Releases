/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Demand-read fallback for host allocations Vulkan cannot import. A trial's
 * buffer stores and image writes are rolled back whenever it requests more
 * pages. Only a complete execution is published, never a partial traversal. */
#include "gpu/shader/shader_bvh_address.h"
#define BVH_REPLAY_IMAGES 64u
typedef struct bvh_replay_state {
    struct {vk_image *target,*copy;bool written;uint64_t sequence;} image[BVH_REPLAY_IMAGES];
    vk_image *retired[BVH_REPLAY_IMAGES];
    uint32_t retired_count;
    uint32_t count;
    bool failed;
} bvh_replay_state;

static bool bvh_save_targets(renderer *r,const bind_list *bl) {
    bvh_replay_state *state=r->pipes->bvh_replay;
    if(!state)return true;
    if(bl->written_count>BVH_REPLAY_IMAGES)return false;
    for(uint32_t i=0;i<bl->written_count;i++) {
        vk_image *target=bl->written[i];
        bool seen=false;
        for(uint32_t j=0;j<state->count;j++)seen|=state->image[j].target==target;
        if(seen)continue;
        if(state->count==BVH_REPLAY_IMAGES || target->depth || target->aspect!=VK_IMAGE_ASPECT_COLOR_BIT ||
           target->levels>16)return false;
        vk_image *copy=vk_image_snapshot(r,target);
        if(!copy)return false;
        uint32_t n=state->count++;
        state->image[n].target=target;state->image[n].copy=copy;
        state->image[n].written=target->gpu_written;state->image[n].sequence=target->written_seq;
    }
    return true;
}

static void bvh_restore_targets(renderer *r,bvh_replay_state *state) {
    for(uint32_t i=0;i<state->count;i++) {
        vk_image *target=state->image[i].target,*copy=state->image[i].copy;
        vk_barrier(r->cmd,copy,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        vk_barrier(r->cmd,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy regions[16]={0};uint32_t levels=target->levels?target->levels:1;
        for(uint32_t k=0;k<levels;k++) {
            regions[k].srcSubresource=regions[k].dstSubresource=(VkImageSubresourceLayers){target->aspect,k,0,target->layers};
            regions[k].extent=(VkExtent3D){target->width>>k?target->width>>k:1,target->height>>k?target->height>>k:1,1};
        }
        vkCmdCopyImage(r->cmd,copy->image,copy->layout,target->image,target->layout,levels,regions);
        target->gpu_written=state->image[i].written;target->written_seq=state->image[i].sequence;
        state->retired[state->retired_count++]=copy;
    }
    state->count=0;
}

static void bvh_free_retired(renderer *r,bvh_replay_state *state) {
    for(uint32_t i=0;i<state->retired_count;i++)vk_image_snapshot_destroy(r,state->retired[i]);
    state->retired_count=0;
}

/* Conservative image footprint, including tiled padding and mip tails. CPU
 * bytes cannot substitute for a GPU-written image. Refuse that read instead. */
static bool bvh_page_cpu_authoritative(renderer *r,guest_addr base,uint64_t bytes) {
    for(uint32_t i=0;i<r->image_count;i++) {
        const vk_image *im=r->images+i;if(!im->image || !im->gpu_written)continue;
        uint64_t w=1,h=1,d=1;
        while(w<im->width)w*=2;
        while(h<im->height)h*=2;
        while(d<im->depth)d*=2;
        uint64_t footprint=w*h*d*16*(im->layers?im->layers:1)*2;
        if(footprint<65536)footprint=65536;
        if(footprint>UINT64_MAX-im->addr)return false;
        if(dynamic_ranges_alias(r->mem,base,bytes,im->addr,footprint))return false;
    }
    return true;
}

static int bvh_entry_order(const void *a,const void *b) {
    const uint64_t *aa=a,*bb=b;return aa[0]<bb[0]?-1:aa[0]>bb[0];
}

static bool bvh_page_load(renderer *r,bvh_address_map *map,guest_addr page) {
    if(map->allocations>=MAX_BUFFER_COPY/SH_BVH_PAGE_BYTES)return false;
    /* One owned, fully committed allocation, even when guest pages are views
     * of a huge sparse section. Never ask the driver to import those views. */
    vk_buffer *buffer=map->imports+map->allocations++;
    if(!vk_buffer_create(r,SH_BVH_PAGE_BYTES,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true,buffer))return false;
    VkBufferDeviceAddressInfoKHR info={.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR,.buffer=buffer->buffer};
    uint64_t device=r->buffer_address(r->device,&info);if(!device)return false;
    memset(buffer->mapped,0,SH_BVH_PAGE_BYTES);
    guest_addr end=page+SH_BVH_PAGE_BYTES;
    for(guest_addr at=page;at<end;) {
        gmem_vma v;bool found=gmem_query_vma(r->mem,at,true,&v),readable=false;
        guest_addr high=end;
        if(found && v.start<end) {
            if(v.start>at)high=v.start;
            else {high=v.start+v.size;if(high>end)high=end;readable=!v.reserved && (v.prot&GPROT_READ);}
        }
        if(high<=at || map->count==BVH_MAP_MAX)return false;
        if(readable) {
            if(!gmem_read(r->mem,at,(uint8_t*)buffer->mapped+(at-page),high-at))return false;
            map->copied+=high-at;
        }
        uint64_t *entry=map->entries[map->count++];
        entry[0]=at;entry[1]=high-at;entry[2]=readable?device+at-page:0;entry[3]=0;
        at=high;
    }
    return true;
}

static bool bvh_publish_table(renderer *r,bvh_address_map *map) {
    uint64_t bytes=SH_BVH_PAGED_HEADER*4+(uint64_t)map->count*sizeof(*map->entries);
    vk_buffer *workspace=&r->pipes->bvh_paged_table;
    if(!workspace->buffer) {
        uint64_t capacity=SH_BVH_PAGED_HEADER*4+(uint64_t)BVH_MAP_MAX*sizeof(*map->entries);
        if(!vk_buffer_create(r,capacity,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true,workspace))return false;
    }
    map->table=*workspace;map->table.size=bytes;
    memset(map->table.mapped,0,(size_t)bytes);
    ((uint32_t*)map->table.mapped)[0]=map->count;
    qsort(map->entries,map->count,sizeof(*map->entries),bvh_entry_order);
    memcpy((uint8_t*)map->table.mapped+SH_BVH_PAGED_HEADER*4,map->entries,map->count*sizeof(*map->entries));
    return true;
}

static bool bvh_actual_reads_safe(renderer *r,const uint32_t *stats,const char **reason) {
    vk_pipelines *P=r->pipes;
    for(uint32_t slot=0;slot<SH_BVH_REQUEST_SLOTS;slot++)if(stats[4+slot]) {
        guest_addr page=(guest_addr)stats[4+slot]<<SH_BVH_PAGE_SHIFT;
        for(uint32_t word=0;word<SH_BVH_READ_MASK_WORDS;word++) {
            uint32_t bits=stats[SH_BVH_READ_MASK_BASE+slot*SH_BVH_READ_MASK_WORDS+word];
            for(uint32_t bit=0;bits && bit<32;bit++)if(bits&(1u<<bit)) {
                bits&=~(1u<<bit);
                guest_addr node=page+((uint64_t)word*32+bit)*64;
                if(!bvh_page_cpu_authoritative(r,node,64)){*reason="GPU image/node alias";return false;}
                for(uint32_t w=0;w<P->wb_count;w++)
                    if(dynamic_ranges_alias(r->mem,P->wb[w].addr,P->wb[w].size,node,64)) {
                        *reason="node/store alias";return false;
                    }
            }
        }
    }
    return true;
}

static bool bvh_paged_dispatch(renderer *r,stage_state *cs,uint32_t x,uint32_t y,uint32_t z) {
    vk_pipelines *P=r->pipes;
    bvh_address_map *map=calloc(1,sizeof(*map));
    bvh_replay_state *state=calloc(1,sizeof(*state));
    sh_site_shape *saved=malloc(cs->prog->site_count*sizeof(*saved));
    bool result=false,pinned=false,saved_shapes=false;uint32_t iteration=0,hits=0,misses=0;
    const char *reason="allocation";
    if(!map || !state || !saved)goto done;
    map->imports=calloc(BVH_MAP_MAX,sizeof(*map->imports));map->entries=calloc(BVH_MAP_MAX,sizeof(*map->entries));
    if(!map->imports || !map->entries)goto done;
    memcpy(saved,cs->shapes,cs->prog->site_count*sizeof(*saved));saved_shapes=true;
    end_pass(r);sync_batch(r);apply_writebacks(r);vk_begin(r);
    if(!(pinned=gmem_retain_mappings(r->mem))){reason="mapping pin";goto done;}
    if(r->mem->res_start<SH_BVH_PAGE_BYTES){reason="zero-address mapping unsupported";goto done;}
    for(uint32_t s=0;s<cs->prog->site_count;s++)if(cs->prog->sites[s].kind==SH_SITE_BVH)cs->shapes[s].address_map=4;
    /* Address zero cannot contain guest allocations in this memory model. */
    map->entries[0][0]=0;map->entries[0][1]=SH_BVH_PAGE_BYTES;map->count=1;
    P->bvh_replay=state;
    for(iteration=0;iteration<64;iteration++) {
        if(!bvh_publish_table(r,map)){reason="table allocation";break;}
        P->bvh_table=&map->table;
        VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
        vkCmdPipelineBarrier(r->cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,NULL,0,NULL);
        uint64_t before=P->dispatches;
        bool accepted=do_dispatch(r,cs,x,y,z);
        barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(r->cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
        sync_batch(r);
        bvh_free_retired(r,state);
        uint32_t *stats=map->table.mapped;hits=stats[1];misses=stats[2];
        bool valid=accepted && P->dispatches>before && !stats[3];
        /* Snapshot BVH reads must not hide writes through another VA. This
         * fallback supports read-only nodes, not in-dispatch tree mutation. */
        if(valid)valid=bvh_actual_reads_safe(r,stats,&reason);
        if(valid && !misses) {
            for(uint32_t w=0;w<P->wb_count;w++)htile_written(P,P->wb[w].addr,P->wb[w].size,"compute",cs->prog->hash,false);
            apply_writebacks(r);result=true;reason="completed";break;
        }
        drop_writebacks(P,0);P->htile_check_count=0;
        vk_begin(r);bvh_restore_targets(r,state);
        if(!valid){if(!accepted || P->dispatches==before)reason="dispatch preparation";else if(stats[3])reason="request table overflow";break;}
        uint32_t old_count=map->count;
        for(uint32_t slot=0;slot<SH_BVH_REQUEST_SLOTS;slot++)if(stats[4+slot]) {
            guest_addr page=(guest_addr)stats[4+slot]<<SH_BVH_PAGE_SHIFT;
            bool loaded=false;
            for(uint32_t i=0;i<map->count;i++)loaded|=map->entries[i][0]>=page && map->entries[i][0]<page+SH_BVH_PAGE_BYTES;
            if(loaded)continue;
            if(!bvh_page_load(r,map,page)){reason="page budget or GPU image alias";valid=false;break;}
        }
        if(!valid || old_count==map->count)break;
    }
done:
    if(pinned && state && !state->failed) {
        if(!result){drop_writebacks(P,0);vk_begin(r);if(state)bvh_restore_targets(r,state);}
        sync_batch(r); /* includes every recorded rollback before releasing resources */
    }
    if(pinned && state && state->failed)drop_writebacks(P,0);
    P->bvh_table=NULL;P->bvh_replay=NULL;
    if(state) {
        bvh_free_retired(r,state);
        for(uint32_t i=0;i<state->count;i++)vk_image_snapshot_destroy(r,state->image[i].copy);
    }
    if(map) {
        static uint32_t reports,active_reports;
        if(reports++<32 || !result || (hits && active_reports++<32))LOG_I(GPU,"BVH paged CS %016llx: %s, passes %u pages %u copied %llu KiB hits %u requests %u (%s)",
            (unsigned long long)cs->prog->hash,result?"completed":"rejected",iteration+1,map->allocations,
            (unsigned long long)(map->copied>>10),hits,misses,reason);
        if(map->imports)for(uint32_t i=0;i<map->allocations;i++)vk_buffer_destroy(r,map->imports+i);
        /* table is the renderer's fenced, reusable workspace */
        free(map->imports);free(map->entries);
    }
    if(saved_shapes)memcpy(cs->shapes,saved,cs->prog->site_count*sizeof(*saved));
    if(pinned){gmem_unpin_mappings(r->mem);vk_begin(r);}
    free(map);free(state);free(saved);return result;
}
