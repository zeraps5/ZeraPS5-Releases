/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_VK_NGG_LARGE_READS_H
#define PS5C_VK_NGG_LARGE_READS_H
/* Synchronous proof path: readonly resources only. The caller finishes older
 * GPU writes, pins mappings, then retains every import until its final fence.
 * No snapshots of whole heaps and no persistent raw host pointers. */
typedef struct ngg_large_read {
    guest_addr base;
    uint64_t size,phys;
    vk_buffer buffer;
} ngg_large_read;

typedef struct ngg_large_key {guest_addr base;uint64_t bytes;} ngg_large_key;
typedef struct ngg_large_reads {
    ngg_large_read entries[16];
    uint32_t count;
    uint64_t bytes;
    bool has_snapshot;
    gmem_vma excluded[16]; /* compute writers: imports must stay immutable */
    uint32_t excluded_count;
    struct {
        guest_addr base;
        uint64_t size,phys;
        vk_buffer buffer;
        uint8_t *before;
    } writes[4];
    uint32_t write_count;
    uint64_t write_bytes;
} ngg_large_reads;

/* Explicit diagnostic budget; the default ceiling remains 3GiB.
 * A small increase can distinguish clipping from arithmetic, without an
 * unbounded allocation knob or changing any alias exclusion. */
static uint64_t large_read_budget(void) {
    static uint64_t budget;
    if(!budget) {
        budget=3ull<<30;const char *s=getenv("PS5C_LARGE_READ_BUDGET_MIB");
        if(s) {
            char *end;unsigned long long mib=strtoull(s,&end,10);
            if(*s && !*end && mib>=1 && mib<=3328)budget=(uint64_t)mib<<20;
            else LOG_W(GPU,"invalid LARGE_READ_BUDGET_MIB: using 3072 MiB");
            LOG_I(GPU,"readonly buffer budget: %llu MiB",(unsigned long long)(budget>>20));
        }
    }
    return budget;
}

/* 1 contained, 0 disjoint, -1 partial/unaligned alias. No independent copies
 * of overlapping output bytes may reach a single compute dispatch. */
static int large_write_bind(renderer *r,ngg_large_reads *pool,guest_addr base,uint64_t bytes,VkDescriptorBufferInfo *out) {
    if(!pool || !pool->write_count || !base || !bytes)return 0;
    gmem_vma v;uint64_t phys=UINT64_MAX;
    if(gmem_query_vma(r->mem,base,false,&v) && !v.reserved && v.kind==GVMA_VIEW)
        phys=v.phys+base-v.os_base;
    if(base>UINT64_MAX-bytes)return -1;
    for(uint32_t i=0;i<pool->write_count;i++) {
        uint64_t b=pool->writes[i].base,n=pool->writes[i].size,p=pool->writes[i].phys;
        bool va=base>=b && base-b<=n && bytes<=n-(base-b);
        bool pa=phys!=UINT64_MAX && p!=UINT64_MAX && phys>=p && phys-p<=n && bytes<=n-(phys-p);
        if(va || pa) {
            uint64_t off=va?base-b:phys-p;
            if(off%r->props.limits.minStorageBufferOffsetAlignment)return -1;
            *out=(VkDescriptorBufferInfo){pool->writes[i].buffer.buffer,off,bytes};return 1;
        }
        if((base<b+n && b<base+bytes) || (phys!=UINT64_MAX && p!=UINT64_MAX &&
           phys<p+n && p<phys+bytes))return -1;
    }
    return 0;
}

/* Validate the bytes accessible through the binding, not unused aligned
 * import pages. In private allocations, an image starting after that span
 * cannot overlap it. Earlier images and physical-view aliases stay excluded;
 * no guest image extent or tiling footprint is guessed here. */
static void large_image_alias_audit(guest_addr base,uint64_t bytes,guest_addr image,const char *reason,
                                  const gmem_vma *v,const gmem_vma *iv) {
    static bool initialized;static guest_addr want;static bool all;
    static guest_addr seen_base[16],seen_image[16];static unsigned count;
    if(!initialized){const char*s=getenv("PS5C_LARGE_READ_AUDIT");all=s && !strcmp(s,"1");want=s?strtoull(s,NULL,16):0;initialized=true;}
    if((!all && (!want || base!=want)) || count>=16)return;
    for(unsigned i=0;i<count;i++)if(seen_base[i]==base && seen_image[i]==image)return;
    seen_base[count]=base;seen_image[count++]=image;
    LOG_I(GPU,"large read excluded: %llx %llu bytes image %llx (%s), VMA %llx+%llx kind %u physical %llx; image VMA %llx+%llx kind %u physical %llx",
        (unsigned long long)base,(unsigned long long)bytes,(unsigned long long)image,reason,
        (unsigned long long)v->start,(unsigned long long)v->size,v->kind,(unsigned long long)v->phys,
        (unsigned long long)iv->start,(unsigned long long)iv->size,iv->kind,(unsigned long long)iv->phys);
}
static bool large_buffer_image_alias(renderer *r,const gmem_vma *v,guest_addr base,uint64_t bytes) {
    for(uint32_t i=0;i<r->image_count;i++) {
        const vk_image *im=r->images+i;if(!im->image)continue;
        guest_addr starts[2]={im->addr,im->stencil_addr};
        for(unsigned k=0;k<2;k++) {
            if(!starts[k])continue;
            gmem_vma iv;if(!gmem_query_vma(r->mem,starts[k],false,&iv) || iv.reserved)continue;
            if(v->start<iv.start+iv.size && iv.start<v->start+v->size) {
                if(v->kind!=GVMA_VIEW && iv.kind!=GVMA_VIEW && starts[k]>=base+bytes)continue;
                large_image_alias_audit(base,bytes,starts[k],"virtual VMA overlap",v,&iv);
                return true;
            }
            if(v->kind==GVMA_VIEW && iv.kind==GVMA_VIEW) {
                uint64_t a=v->phys+v->start-v->os_base,b=iv.phys+iv.start-iv.os_base;
                if(a<b+iv.size && b<a+v->size){large_image_alias_audit(base,bytes,starts[k],"physical VMA overlap",v,&iv);return true;}
            }
        }
    }
    return false;
}
/* Diagnostic only: compare MAX_BUFFER_COPY truncation independently of host
 * imports. This owns an immutable CPU snapshot, never borrows host pages.
 * The normal image-alias exclusion below is unchanged. CPU RAM may still be
 * stale for GPU images in the same allocation: this is NOT an alias solution.
 * Caller has completed earlier buffer writebacks and pinned guest mappings. */
static bool ngg_large_snapshot_bind(renderer *r,ngg_large_reads *pool,guest_addr base,uint64_t bytes,uint64_t flip,
                                    VkDescriptorBufferInfo *out) {
    static bool initialized;static guest_addr wanted;static const char *file;static uint64_t checked=UINT64_MAX;
    static uint32_t logged,refused;
    if(!initialized) {
        const char *setting=getenv("PS5C_LARGE_READ_SNAPSHOT");
        if(setting && *setting=='@')file=setting+1;
        else if(setting){char *end;unsigned long long n=strtoull(setting,&end,16);if(!*end)wanted=n;}
        initialized=true;
    }
    if(file && checked!=flip) {
        checked=flip;unsigned long long value=0;char extra;FILE *f=fopen(file,"r");
        bool valid=f && fscanf(f,"%llx %c",&value,&extra)==1;if(f)fclose(f);
        guest_addr next=valid?(guest_addr)value:0;
        if(next!=wanted){LOG_I(GPU,"readonly snapshot selection: %llx",(unsigned long long)next);logged=refused=0;}
        wanted=next;
    }
    if(!wanted || base!=wanted || !pool || pool->write_count || pool->excluded_count || !bytes ||
       bytes>(256ull<<20) || bytes>r->props.limits.maxStorageBufferRange || base>UINT64_MAX-bytes)return false;
    gmem_vma v;
    if(!gmem_query_vma(r->mem,base,false,&v) || v.reserved || v.kind==GVMA_VIEW ||
       !(v.prot&GPROT_READ) || bytes>v.start+v.size-base)return false;
    for(uint32_t i=0;i<pool->count;i++) {
        ngg_large_read *e=pool->entries+i;
        if(base==e->base && bytes==e->size && e->buffer.mapped) {
            *out=(VkDescriptorBufferInfo){e->buffer.buffer,0,bytes};return true;
        }
        if(base<e->base+e->size && e->base<base+bytes)return false;
    }
    uint64_t budget=large_read_budget();
    if(pool->count==PS5C_ARRAY_COUNT(pool->entries) || pool->bytes>budget || bytes>budget-pool->bytes) {
        if(refused++<8)LOG_W(GPU,"readonly snapshot budget refusal: %llx needs %llu bytes; pool %llu bytes, %u entries",
            (unsigned long long)base,(unsigned long long)bytes,(unsigned long long)pool->bytes,pool->count);
        return false;
    }
    const void *source=gmem_host(r->mem,base,bytes);if(!source)return false;
    ngg_large_read *e=pool->entries+pool->count;
    if(!vk_buffer_create(r,bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true,&e->buffer))return false;
    memcpy(e->buffer.mapped,source,(size_t)bytes);
    pool->has_snapshot=true;
    e->base=base;e->size=bytes;e->phys=UINT64_MAX;pool->count++;pool->bytes+=bytes;
    *out=(VkDescriptorBufferInfo){e->buffer.buffer,0,bytes};
    if(logged++<8)LOG_I(GPU,"diagnostic readonly snapshot: 0x%llx %llu bytes; full CPU copy, no host import",
                       (unsigned long long)base,(unsigned long long)bytes);
    return true;
}
static bool ngg_large_bind(renderer *r,ngg_large_reads *pool,guest_addr base,uint64_t bytes,VkDescriptorBufferInfo *out) {
    if(!pool || !base || !bytes || bytes>r->props.limits.maxStorageBufferRange || base>UINT64_MAX-bytes ||
       base%r->props.limits.minStorageBufferOffsetAlignment)return false;
    /* Use the external-host requirement, not Windows reservation granularity.
     * A16KiB guest VMA may start within a64KiB reservation. Over-rounding
     * crosses into an unmapped prefix even when the actual import is valid.
     * vk_buffer_import_host still checks the real memory requirements. */
    uint64_t align=r->host_import_align;
    if(!align || (align&(align-1)))return false;
    guest_addr low=base&~(align-1),end=(base+bytes+align-1)&~(align-1);
    gmem_vma v;
    if(end<=low || !gmem_query_vma(r->mem,low,false,&v) || v.reserved || !(v.prot&GPROT_READ) ||
       end>v.start+v.size || large_buffer_image_alias(r,&v,base,bytes))return false;
    for(uint32_t i=0;i<pool->excluded_count;i++) {
        const gmem_vma *w=pool->excluded+i;
        if(v.start<w->start+w->size && w->start<v.start+v.size)return false;
        if(v.kind==GVMA_VIEW && w->kind==GVMA_VIEW) {
            uint64_t a=v.phys+v.start-v.os_base,b=w->phys+w->start-w->os_base;
            if(a<b+w->size && b<a+v.size)return false;
        }
    }
    uint64_t phys=v.kind==GVMA_VIEW?v.phys+low-v.os_base:UINT64_MAX;
    for(uint32_t i=0;i<pool->count;i++) {
        ngg_large_read *e=pool->entries+i;
        bool same_va=low>=e->base && end<=e->base+e->size;
        bool same_phys=phys!=UINT64_MAX && e->phys!=UINT64_MAX && phys>=e->phys &&
                       phys-e->phys<=e->size && end-low<=e->size-(phys-e->phys);
        if(same_va || same_phys) {
            uint64_t off=same_va?base-e->base:phys-e->phys+base-low;
            *out=(VkDescriptorBufferInfo){e->buffer.buffer,off,bytes};return true;
        }
        /* Do not import the same physical pages independently, even via an alias. */
        if((low<e->base+e->size && e->base<end) ||
           (phys!=UINT64_MAX && e->phys!=UINT64_MAX && phys<e->phys+e->size && e->phys<phys+end-low))return false;
    }
    uint64_t budget=large_read_budget();
    if(pool->count==PS5C_ARRAY_COUNT(pool->entries) || pool->bytes>budget || end-low>budget-pool->bytes)return false;
    ngg_large_read *e=pool->entries+pool->count;
    if(!vk_buffer_import_host(r,gmem_host(r->mem,low,end-low),end-low,&e->buffer))return false;
    e->base=low;e->size=end-low;e->phys=phys;pool->count++;pool->bytes+=e->size;
    *out=(VkDescriptorBufferInfo){e->buffer.buffer,base-low,bytes};
    static guest_addr logged[24];static uint32_t nlogged;bool seen=false;
    for(uint32_t i=0;i<nlogged;i++)seen|=logged[i]==low;
    if(!seen && nlogged<PS5C_ARRAY_COUNT(logged)) {
        logged[nlogged++]=low;
        LOG_I(GPU,"readonly buffer import: 0x%llx + %llu MiB, no arena copy",
              (unsigned long long)low,(unsigned long long)(e->size>>20));
    }
    return true;
}
static void ngg_large_free(renderer *r,ngg_large_reads *pool) {
    for(uint32_t i=0;i<pool->count;i++)vk_buffer_destroy(r,&pool->entries[i].buffer);
    for(uint32_t i=0;i<pool->write_count;i++) {
        vk_buffer_destroy(r,&pool->writes[i].buffer);free(pool->writes[i].before);
    }
    free(pool);
}
#endif
