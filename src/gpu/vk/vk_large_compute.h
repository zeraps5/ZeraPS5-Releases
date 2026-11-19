/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Known readonly compute inputs. An independent opt-in stages bounded large
 * outputs and merges changed bytes after the fence. Never import writers. */
static ngg_large_reads *large_compute_begin(renderer *r,stage_state *cs,uint64_t flip) {
    static const char *setting;static bool init;static uint64_t selection,checked=UINT64_MAX;
    if(!init){setting=getenv("PS5C_COMPUTE_LARGE_BUFFERS");init=true;}
    if(!setting || !r->host_import_align)return NULL;
    if(setting[0]=='@') {
        if(checked!=flip){checked=flip;selection=0;FILE*f=fopen(setting+1,"r");unsigned long long v=0;
            if(f){if(fscanf(f,"%llx",&v)==1)selection=v;fclose(f);}}
    } else selection=strtoull(setting,NULL,16);
    if(!selection || (selection!=1 && selection!=cs->prog->hash && selection!=cs->prog->base))return NULL;
    static int writes=-1;
    if(writes<0){const char*s=getenv("PS5C_COMPUTE_LARGE_WRITES");writes=s && !strcmp(s,"1");}
    bool wanted=false;
    for(uint32_t i=0;i<cs->prog->site_count;i++) {
        const sh_site *s=cs->prog->sites+i;const sh_site_value *v=cs->eval.site+i;
        if((s->kind==SH_SITE_BUFFER || s->kind==SH_SITE_SBUFFER) && (!s->store || writes) && v->valid && !v->desc_unknown) {
            uint32_t stride=(v->desc[1]>>16)&0x3fff;
            uint64_t bytes=(uint64_t)v->desc[2]*(stride?stride:1);
            wanted|=bytes>MAX_BUFFER_COPY && bytes<=r->props.limits.maxStorageBufferRange;
        }
    }
    if(!wanted)return NULL;
    end_pass(r);sync_batch(r);apply_writebacks(r);vk_begin(r);
    ngg_large_reads *pool=calloc(1,sizeof(*pool));
    if(!pool)return NULL;
    if(!gmem_retain_mappings(r->mem)){free(pool);return NULL;}
    for(uint32_t i=0;i<cs->prog->site_count;i++) {
        const sh_site *s=cs->prog->sites+i;const sh_site_value *v=cs->eval.site+i;
        if(!s->store)continue;
        const rdna2_inst *in=cs->prog->insts+s->inst;
        if(!v->valid || v->desc_unknown || v->records_unknown)goto reject;
        guest_addr base;uint64_t bytes=1;
        if(s->kind==SH_SITE_BUFFER && (in->enc==RDNA2_ENC_MUBUF || in->enc==RDNA2_ENC_MTBUF)) {
            base=(guest_addr)v->desc[0]|((guest_addr)(v->desc[1]&65535)<<32);
            uint32_t stride=(v->desc[1]>>16)&0x3fff;bytes=(uint64_t)v->desc[2]*(stride?stride:1);
        } else if(s->kind==SH_SITE_IMAGE) {
            base=((guest_addr)v->desc[0]<<8)|((guest_addr)(v->desc[1]&255)<<40);
        } else goto reject; /* indirect/global/GDS stores need their own proof */
        if(!base || !bytes)continue;
        gmem_vma w;if(!gmem_query_vma(r->mem,base,false,&w) || w.reserved ||
            bytes>w.start+w.size-base)goto reject;
        bool found=false;
        for(uint32_t k=0;k<pool->excluded_count;k++)found|=pool->excluded[k].start==w.start;
        if(!found){if(pool->excluded_count==PS5C_ARRAY_COUNT(pool->excluded))goto reject;
            pool->excluded[pool->excluded_count++]=w;}
        if(writes && s->kind==SH_SITE_BUFFER && bytes>MAX_BUFFER_COPY) {
            VkDescriptorBufferInfo binding;
            int alias=large_write_bind(r,pool,base,bytes,&binding);
            if(alias<0)goto reject;
            if(alias>0)continue;
            if(bytes>(256ull<<20) || pool->write_bytes+bytes>(512ull<<20) ||
               pool->write_count==PS5C_ARRAY_COUNT(pool->writes) || !(w.prot&GPROT_WRITE) ||
               large_buffer_image_alias(r,&w,base,bytes))goto reject;
            uint32_t k=pool->write_count++;
            pool->writes[k].base=base;pool->writes[k].size=bytes;
            pool->writes[k].phys=w.kind==GVMA_VIEW?w.phys+base-w.os_base:UINT64_MAX;
            pool->write_bytes+=bytes;pool->writes[k].before=malloc((size_t)bytes);
            if(!pool->writes[k].before || !gmem_read(r->mem,base,pool->writes[k].before,bytes) ||
               !vk_buffer_create(r,bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true,&pool->writes[k].buffer))goto reject;
            memcpy(pool->writes[k].buffer.mapped,pool->writes[k].before,(size_t)bytes);
        }
    }
    for(uint32_t i=0;pool->write_count && i<cs->prog->site_count;i++) {
        const sh_site *s=cs->prog->sites+i;const sh_site_value*v=cs->eval.site+i;
        if(s->kind==SH_SITE_IMAGE)goto reject; /* no new image/output alias during preparation */
        if(s->kind!=SH_SITE_BUFFER && s->kind!=SH_SITE_SBUFFER)continue;
        if(v->desc_unknown)goto reject;
        uint64_t stride=(v->desc[1]>>16)&0x3fff,bytes=(uint64_t)v->desc[2]*(stride?stride:1);
        guest_addr base=(guest_addr)v->desc[0]|((guest_addr)(v->desc[1]&65535)<<32);
        VkDescriptorBufferInfo binding;if(large_write_bind(r,pool,base,bytes,&binding)<0)goto reject;
    }
    static uint32_t announced;
    if(pool->write_count && announced++<8)LOG_I(GPU,"compute large outputs: %u snapshots, %llu MiB, bounded differential publication after fence",
        pool->write_count,(unsigned long long)(pool->write_bytes>>20));
    cs->large_reads=pool;
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(r->cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0,1,&barrier,0,NULL,0,NULL);
    return pool;
reject:
    ngg_large_free(r,pool);gmem_unpin_mappings(r->mem);return NULL;
}
static void large_compute_end(renderer *r,stage_state *cs,ngg_large_reads *pool) {
    if(!pool)return;
    end_pass(r);sync_batch(r);
    for(uint32_t i=0;i<pool->write_count;i++) { /* sync_batch validated completion */
        uint64_t size=pool->writes[i].size;
        uint8_t *dst=gmem_host(r->mem,pool->writes[i].base,size);
        const uint8_t *src=pool->writes[i].buffer.mapped,*before=pool->writes[i].before;
        if(!dst)continue; /* mapping pin normally makes this impossible */
        for(uint64_t off=0;off<size;off+=4096) {
            uint64_t n=size-off<4096?size-off:4096;
            if(!memcmp(src+off,before+off,(size_t)n))continue;
            for(uint64_t k=off;k<off+n;k++)if(src[k]!=before[k])dst[k]=src[k];
        }
        copies_written(r->pipes,pool->writes[i].base,size);
        evals_written(r->pipes,pool->writes[i].base,size);
    }
    cs->large_reads=NULL;ngg_large_free(r,pool);gmem_unpin_mappings(r->mem);vk_begin(r);
}
