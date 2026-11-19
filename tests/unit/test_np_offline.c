/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/runtime.h"
#include "hle/hle.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"NP offline line%d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void) {
    CHECK(pal_init()==PS5C_OK && hle_registry_init()==PS5C_OK);
    ps5_runtime *rt=calloc(1,sizeof(*rt));CHECK(rt);
    gmem_config config=gmem_default_config();config.identity=false;
    CHECK(gmem_init(&rt->mem,&config)==PS5C_OK);
    guest_addr page;CHECK(gmem_map(&rt->mem,0,GUEST_PAGE_SIZE,GPROT_RW,0,"offline NP sentinels",&page)==PS5C_OK);
    uint8_t before[256],after[256];memset(before,0xa5,sizeof(before));
    CHECK(gmem_write(&rt->mem,page,before,sizeof(before)));
    const char *names[]={"sceNpEntitlementAccessRequestUnifiedEntitlementInfoList",
        "sceNpEntitlementAccessPollUnifiedEntitlementInfoList",
        "sceNpEntitlementAccessRequestServiceEntitlementInfoList",
        "sceNpEntitlementAccessPollServiceEntitlementInfoList"};
    for(unsigned i=0;i<PS5C_ARRAY_COUNT(names);i++) {
        const hle_export *e=hle_find("libSceNpEntitlementAccess",nid_hash_name(names[i]));CHECK(e && e->fn);
        for(unsigned variant=0;variant<3;variant++) {
            hle_args args={0};
            for(unsigned k=0;k<6;k++)args.gpr[k]=variant==0?0:variant==1?page+16*k:UINT64_MAX;
            hle_call c={.rt=rt,.mem=&rt->mem,.a=&args};e->fn(&c);
            CHECK((uint32_t)args.ret[0]==0x80550006u);
            CHECK(gmem_read(&rt->mem,page,after,sizeof(after)) && !memcmp(before,after,sizeof(before)));
        }
    }
    gmem_free(&rt->mem);free(rt);hle_registry_free();pal_shutdown();
    puts("offline NP requests: signed-out result, no request or entitlement outputs fabricated");return 0;
}
