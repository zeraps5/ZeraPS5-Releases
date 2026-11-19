/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <string.h>
#include "core/runtime.h"
#include "hle/acm.h"
#include "loader/nid.h"
#include "pal/pal.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static int32_t call(ps5_runtime *rt, const char *name, uint64_t a, uint64_t b,
                    uint64_t c, uint64_t d, uint64_t e) {
    const hle_export *exp = hle_find("libSceAcm", nid_hash_name(name));
    if (!exp || !exp->fn) return INT32_MIN;
    hle_args args = {.gpr = {a,b,c,d,e}};
    hle_call context = {.rt=rt, .mem=&rt->mem, .a=&args};
    exp->fn(&context); return (int32_t)args.ret[0];
}
#define CREATE(rt, out) call(rt, "sceAcmContextCreate", out, 0, 0, 0, 0)
#define DESTROY(rt, id) call(rt, "sceAcmContextDestroy", id, 0, 0, 0, 0)
static bool init(ps5_runtime *rt, guest_addr *page) {
    memset(rt,0,sizeof(*rt));
    gmem_config cfg = gmem_default_config(); cfg.identity=false;
    return pal_mutex_create(&rt->threads_lock)==PS5C_OK &&
        gmem_init(&rt->mem,&cfg)==PS5C_OK &&
        gmem_map(&rt->mem,0,3*GUEST_PAGE_SIZE,GPROT_RW,0,"ACM test",page)==PS5C_OK;
}
static void finish(ps5_runtime *rt) {
    hle_acm_reset(rt); gmem_free(&rt->mem); pal_mutex_destroy(rt->threads_lock);
}
int main(void) {
    CHECK(pal_init()==PS5C_OK); CHECK(hle_registry_init()==PS5C_OK);
    ps5_runtime a,b; guest_addr pa,pb;
    CHECK(init(&a,&pa)); CHECK(init(&b,&pb));
    uint8_t *bytes=gmem_host(&a.mem,pa,3*GUEST_PAGE_SIZE); CHECK(bytes);
    memset(bytes,0xa5,3*GUEST_PAGE_SIZE);
    CHECK(CREATE(&a,0)==ACM_INVALID_PARAMETER);
    CHECK(CREATE(&a,UINT64_MAX-1)==ACM_INVALID_PARAMETER);
    CHECK(CREATE(&a,pa+3*GUEST_PAGE_SIZE-2)==ACM_INVALID_PARAMETER);
    CHECK(bytes[3*GUEST_PAGE_SIZE-2]==0xa5); /* no partial write */
    CHECK(gmem_protect(&a.mem,pa+GUEST_PAGE_SIZE,GUEST_PAGE_SIZE,GPROT_READ)==PS5C_OK);
    CHECK(CREATE(&a,pa+GUEST_PAGE_SIZE)==ACM_INVALID_PARAMETER);
    CHECK(CREATE(&a,pa+GUEST_PAGE_SIZE-2)==ACM_INVALID_PARAMETER);
    CHECK(bytes[GUEST_PAGE_SIZE-2]==0xa5);
    CHECK(CREATE(&a,pa+1)==0); /* unaligned u32 output: memcpy, no host alignment dependency */
    uint32_t first; memcpy(&first,bytes+1,4); CHECK(first!=0);
    CHECK(bytes[0]==0xa5 && bytes[5]==0xa5); /* exactly four bytes */
    CHECK(DESTROY(&b,first)==ACM_INVALID_PARAMETER); /* no global contexts */
    CHECK(CREATE(&b,pb)==0);
    uint32_t other; CHECK(gmem_read(&b.mem,pb,&other,4)); CHECK(other!=0);
    CHECK(DESTROY(&b,other)==0); CHECK(DESTROY(&b,other)==ACM_INVALID_PARAMETER);
    CHECK(DESTROY(&a,0)==ACM_INVALID_PARAMETER);
    CHECK(call(&a,"sceAcmBatchStartBuffer",first,pa,4,0,pa+32)==ACM_UNSUPPORTED);
    CHECK(call(&a,"sceAcmBatchStartBuffers",first,1,pa,0,pa+32)==ACM_UNSUPPORTED);
    CHECK(call(&a,"sceAcmBatchWait",first,123,0,0,0)==ACM_UNSUPPORTED);
    uint32_t untouched; memcpy(&untouched,bytes+32,4); CHECK(untouched==0xa5a5a5a5u);
    CHECK(call(&a,"sceAcmBatchStartBuffer",first,pa,4,0,0)==ACM_INVALID_PARAMETER);
    CHECK(DESTROY(&a,first)==0); CHECK(DESTROY(&a,first)==ACM_INVALID_PARAMETER);
    CHECK(call(&a,"sceAcmBatchWait",first,123,0,0,0)==ACM_INVALID_PARAMETER);
    uint32_t ids[64];
    for(size_t i=0;i<64;i++) {
        CHECK(CREATE(&a,pa+64)==0); memcpy(&ids[i],bytes+64,4);
        CHECK(ids[i]!=first); for(size_t j=0;j<i;j++) CHECK(ids[j]!=ids[i]);
    }
    CHECK(CREATE(&a,pa+32)==ACM_NOMEM);
    memcpy(&untouched,bytes+32,4); CHECK(untouched==0xa5a5a5a5u);
    CHECK(DESTROY(&a,ids[0])==0); CHECK(CREATE(&a,pa+64)==0);
    uint32_t replacement; memcpy(&replacement,bytes+64,4); CHECK(replacement!=ids[0]);
    hle_acm_reset(&a); CHECK(a.acm==NULL); CHECK(DESTROY(&a,replacement)==ACM_INVALID_PARAMETER);
    finish(&a);finish(&b);hle_registry_free();pal_shutdown();
    puts("ACM: u32 ABI, bounds/protection, process isolation, lifecycle, capacity and unsupported DSP verified");
    return 0;
}
