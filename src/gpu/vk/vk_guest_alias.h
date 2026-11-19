/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_VK_GUEST_ALIAS_H
#define PS5C_VK_GUEST_ALIAS_H
#include "mem/gmem.h"

/* Prove two complete mapped spans disjoint, including every shared-memory
 * view. The caller pins mapping lifetimes. Gaps, reserved/unknown mappings,
 * arithmetic overflow or excessive fragmentation cannot justify skipping a
 * fence. Unlike finite-window lookups, an unmapped gap is not ignored here. */
static inline bool vk_guest_ranges_disjoint(gmem *mem,guest_addr a,uint64_t an,guest_addr b,uint64_t bn) {
    if(!an || !bn)return true;
    if(!mem || a>UINT64_MAX-an || b>UINT64_MAX-bn)return false;
    guest_addr ae=a+an,be=b+bn;
    if(a<be && b<ae)return false;
    unsigned queries=0;
    for(guest_addr ca=a;ca<ae;) {
        gmem_vma va;
        if(++queries>128 || !gmem_query_vma(mem,ca,false,&va) || va.reserved ||
           va.start>ca || va.size>UINT64_MAX-va.start || va.start+va.size<=ca || va.kind>GVMA_VIEW)return false;
        guest_addr ah=va.start+va.size;if(ah>ae)ah=ae;
        for(guest_addr cb=b;cb<be;) {
            gmem_vma vb;
            if(++queries>128 || !gmem_query_vma(mem,cb,false,&vb) || vb.reserved ||
               vb.start>cb || vb.size>UINT64_MAX-vb.start || vb.start+vb.size<=cb || vb.kind>GVMA_VIEW)return false;
            guest_addr bh=vb.start+vb.size;if(bh>be)bh=be;
            if(va.kind==GVMA_VIEW && vb.kind==GVMA_VIEW) {
                if(ca<va.os_base || cb<vb.os_base || va.phys>UINT64_MAX-(ca-va.os_base) ||
                   vb.phys>UINT64_MAX-(cb-vb.os_base))return false;
                uint64_t ap=va.phys+ca-va.os_base,bp=vb.phys+cb-vb.os_base;
                if(ap>UINT64_MAX-(ah-ca) || bp>UINT64_MAX-(bh-cb))return false;
                if(ap<bp+bh-cb && bp<ap+ah-ca)return false;
            }
            cb=bh;
        }
        ca=ah;
    }
    return true;
}
#endif
