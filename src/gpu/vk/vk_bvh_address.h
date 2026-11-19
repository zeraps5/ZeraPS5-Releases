/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Isolated, opt-in compute BVH VA mapping. Included after do_dispatch so the
 * ordinary pipeline, descriptors and writebacks remain the single draw path.
 * No 48-bit virtual address space allocation and no raw host GPU pointers. */
#define BVH_MAP_MAX 4096u
typedef struct bvh_address_map {
    vk_buffer *imports;
    uint64_t (*entries)[4];
    uint32_t count, allocations, excluded;
    uint64_t copied, imported;
    vk_buffer table;
} bvh_address_map;

/* CPU storage of an image last written by Vulkan is not authoritative.
 * Until image->guest resolves exist, omit its entire containing VMA (and
 * direct-memory aliases), rather than exposing stale image bytes as nodes. */
static bool bvh_image_alias(renderer *r, const gmem_vma *v) {
    for(uint32_t i=0;i<r->image_count;i++) {
        const vk_image *im=r->images+i;
        if(!im->image || !im->gpu_written)continue;
        gmem_vma iv;
        if(!gmem_query_vma(r->mem,im->addr,false,&iv) || iv.reserved)continue;
        if(v->start<iv.start+iv.size && iv.start<v->start+v->size)return true;
        if(v->kind==GVMA_VIEW && iv.kind==GVMA_VIEW) {
            uint64_t a=v->phys+v->start-v->os_base,b=iv.phys+iv.start-iv.os_base;
            if(a<b+iv.size && b<a+v->size)return true;
        }
    }
    return false;
}

static bool bvh_address_storage(renderer *r,bvh_address_map *map,guest_addr addr,uint64_t size,bool copy) {
    if(map->count==BVH_MAP_MAX || map->allocations==BVH_MAP_MAX || !size)return false;
    vk_buffer *buffer=map->imports+map->allocations++;
    if(!copy && !vk_buffer_import_host(r,gmem_host(r->mem,addr,size),size,buffer)) {
        /* Some drivers cannot import a read-only host mapping. Keep the
         * fallback bounded; never duplicate a large unimportable heap. */
        vk_buffer_destroy(r,buffer);
        static uint32_t failed_import;
        if(failed_import++<12)LOG_W(GPU,"BVH VA import unavailable: 0x%llx + 0x%llx",
            (unsigned long long)addr,(unsigned long long)size);
        copy=true;
    }
    if(copy) {
        /* Small non-importable regions and alignment edges only. Raising this
         * to duplicate whole game heaps is not a VA/coherency implementation. */
        if(size>MAX_BUFFER_COPY || map->copied+size>MAX_BUFFER_COPY) {
            LOG_W(GPU,"BVH VA bounded copy refused: 0x%llx + 0x%llx (already %llu KiB)",
                (unsigned long long)addr,(unsigned long long)size,(unsigned long long)(map->copied>>10));
            return false;
        }
        if(!vk_buffer_create(r,size,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true,buffer) ||
           !gmem_read(r->mem,addr,buffer->mapped,size))return false;
        map->copied+=size;
    } else map->imported+=size;
    VkBufferDeviceAddressInfoKHR info={.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR,.buffer=buffer->buffer};
    uint64_t device_addr=r->buffer_address(r->device,&info);
    if(!device_addr || (device_addr&3))return false;
    uint64_t *entry=map->entries[map->count++];
    gmem_vma v;
    entry[0]=addr;entry[1]=size;entry[2]=device_addr;
    entry[3]=gmem_query_vma(r->mem,addr,false,&v) && v.kind==GVMA_VIEW?v.phys+addr-v.os_base:UINT64_MAX;
    return true;
}

/* Import each physical page once. EXT_external_memory_host does not promise
 * that importing two aliases of the same pages is supported. Mirror only the
 * checked device address in the VA table; retain the original allocation. */
static bool bvh_address_add(renderer *r,bvh_address_map *map,guest_addr addr,uint64_t size,bool copy) {
    gmem_vma v;
    if(!gmem_query_vma(r->mem,addr,false,&v))return false;
    if(v.kind==GVMA_VIEW) {
        uint64_t phys=v.phys+addr-v.os_base;
        for(uint32_t i=0;i<map->count;i++) {
            const uint64_t *old=map->entries[i];
            if(old[3]==UINT64_MAX)continue;
            if(phys>=old[3] && phys-old[3]<old[1]) {
                uint64_t bytes=old[1]-(phys-old[3]);if(bytes>size)bytes=size;
                if(map->count==BVH_MAP_MAX)return false;
                uint64_t *entry=map->entries[map->count++];
                entry[0]=addr;entry[1]=bytes;entry[2]=old[2]+phys-old[3];entry[3]=phys;
                return bytes==size || bvh_address_add(r,map,addr+bytes,size-bytes,copy);
            }
            if(phys<old[3] && old[3]-phys<size) {
                uint64_t bytes=old[3]-phys;
                return bvh_address_add(r,map,addr,bytes,copy) &&
                       bvh_address_add(r,map,addr+bytes,size-bytes,copy);
            }
        }
    }
    return bvh_address_storage(r,map,addr,size,copy);
}

static bool bvh_address_build(renderer *r,bvh_address_map *map) {
    map->imports=calloc(BVH_MAP_MAX,sizeof(*map->imports));
    map->entries=calloc(BVH_MAP_MAX,sizeof(*map->entries));
    if(!map->imports || !map->entries)return false;
    uint64_t align=r->host_import_align;
    /* Windows allocations may end on a guest 16-KiB boundary while Vulkan's
     * requirements round large buffers to 64 KiB. Snapshot only those edges. */
    if(align<65536)align=65536;
    uint64_t chunk=r->props.limits.maxStorageBufferRange;
    if(chunk>(16ull<<20))chunk=16ull<<20;
    chunk&=~(align-1);
    if(!chunk)return false;
    for(guest_addr cursor=0;;) {
        gmem_vma v;
        if(!gmem_query_vma(r->mem,cursor,true,&v))break;
        guest_addr end=v.start+v.size;
        if(end<=cursor || end>(1ull<<48))return false;
        cursor=end;
        if(v.reserved || !(v.prot&GPROT_READ))continue;
        if(bvh_image_alias(r,&v)){map->excluded++;continue;}
        guest_addr addr=v.start;
        uintptr_t host=(uintptr_t)gmem_host(r->mem,addr,v.size);
        if(!host)return false;
        uint64_t edge=(align-host%align)%align;
        if(edge>v.size)edge=v.size;
        if(edge && !bvh_address_add(r,map,addr,edge,true))return false;
        addr+=edge;
        uint64_t interior=(end-addr)&~(align-1);
        while(interior) {
            uint64_t bytes=interior<chunk?interior:chunk;
            if(!bvh_address_add(r,map,addr,bytes,false))return false;
            addr+=bytes;interior-=bytes;
        }
        if(addr<end && !bvh_address_add(r,map,addr,end-addr,true))return false;
    }
    uint64_t bytes=16+(uint64_t)map->count*sizeof(*map->entries);
    if(!vk_buffer_create(r,bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true,&map->table))return false;
    memset(map->table.mapped,0,(size_t)bytes);
    ((uint32_t*)map->table.mapped)[0]=map->count;
    memcpy((uint8_t*)map->table.mapped+16,map->entries,(size_t)bytes-16);
    return true;
}

static bool bvh_paged_dispatch(renderer *r,stage_state *cs,uint32_t x,uint32_t y,uint32_t z);
static bool bvh_address_dispatch(renderer *r,stage_state *cs,uint32_t x,uint32_t y,uint32_t z) {
    const char *paged=getenv("PS5C_BVH_PAGED");
    if(paged && !strcmp(paged,"1"))return bvh_paged_dispatch(r,cs,x,y,z);
    vk_pipelines *P=r->pipes;
    bool result=false,pinned=false;
    uint64_t before=0;
    bvh_address_map *map=calloc(1,sizeof(*map));
    sh_site_shape *saved=malloc(cs->prog->site_count*sizeof(*saved));
    if(!map || !saved)goto done;
    memcpy(saved,cs->shapes,cs->prog->site_count*sizeof(*saved));
    end_pass(r);sync_batch(r);apply_writebacks(r);vk_begin(r);
    if(!(pinned=gmem_retain_mappings(r->mem)))goto done;
    for(uint32_t s=0;s<cs->prog->site_count;s++)
        if(cs->prog->sites[s].kind==SH_SITE_BVH)cs->shapes[s].address_map=3;
    uint64_t corpus_key=texture_hash_words(cs->shapes,cs->prog->site_count*sizeof(*cs->shapes),cs->prog->hash);
    corpus_key=texture_hash_words(&cs->info,sizeof(cs->info),corpus_key);
    corpus_add(corpus_key,cs->prog,&cs->info,cs->shapes,0,cs->prog->base,cs->prog->code,cs->prog->dwords);
    if(!bvh_address_build(r,map))goto done;
    P->bvh_table=&map->table;
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(r->cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,NULL,0,NULL);
    before=P->dispatches;
    bool accepted=do_dispatch(r,cs,x,y,z);
    barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(r->cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
    /* Even a rejected preparation can have recorded image copies. Retain all
     * imports until completion, including errors and arena exhaustion. */
    sync_batch(r);
    apply_writebacks(r); /* sync_batch validated completion */
    result=accepted && P->dispatches>before;
done:
    P->bvh_table=NULL;
    if(map) {
        static uint32_t completed,rejected;
        if(result?completed++<24:rejected++<24) {
            uint32_t *stats=map->table.mapped;
            LOG_I(GPU,"BVH VA CS %016llx: %s, %u ranges, imported %llu MiB, copied %llu KiB, excluded image VMAs %u, resolved %u missing %u",
                (unsigned long long)cs->prog->hash,result?"completed":"rejected",map->count,
                (unsigned long long)(map->imported>>20),(unsigned long long)(map->copied>>10),map->excluded,
                stats?stats[1]:0,stats?stats[2]:0);
        }
        if(map->imports)for(uint32_t i=0;i<map->allocations;i++)vk_buffer_destroy(r,map->imports+i);
        vk_buffer_destroy(r,&map->table);
        free(map->imports);free(map->entries);
    }
    if(pinned) {
        memcpy(cs->shapes,saved,cs->prog->site_count*sizeof(*saved));
        gmem_unpin_mappings(r->mem);
        vk_begin(r);
    }
    free(map);free(saved);
    return result;
}

#include "gpu/vk/vk_bvh_paged.h"
