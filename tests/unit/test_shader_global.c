/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include "gpu/shader/shader.h"

/* A mapped source range: inference must preserve both halves of its address. */
static const gmem_vma source = {.start = 0xc012340000ull, .size = 4096};
void *gmem_host(gmem *mem, guest_addr addr, uint64_t size) {
    (void)mem; (void)addr; (void)size;
    return NULL; /* These programs contain no scalar memory loads. */
}
const gmem_vma *gmem_find_vma(gmem *mem, guest_addr addr) {
    (void)mem;
    return addr >= source.start && addr < source.start + source.size ? &source : NULL;
}

bool gmem_query_vma(gmem *mem, guest_addr addr, bool find_next, gmem_vma *out) {
    const gmem_vma *v = gmem_find_vma(mem, addr);
    (void)find_next;
    if (v) *out = *v;
    return v != NULL;
}

static int check(const uint32_t *code, size_t words) {
    sh_program *p = malloc(sizeof(*p));
    sh_eval *e = calloc(1, sizeof(*e));
    if (!p || !e) { free(p); free(e); return 1; }
    e->sgpr_init[12] = (uint32_t)source.start;
    e->sgpr_init[13] = (uint32_t)(source.start >> 32);
    e->sgpr_init[126] = e->sgpr_init[127] = 0xffffffff;
    bool ok = sh_analyze(code, (uint32_t)words, 0, p) && sh_evaluate(p, NULL, e);
    ok = ok && p->site_count == 1 && e->site[0].valid &&
         e->site[0].desc[0] == (uint32_t)source.start &&
         e->site[0].desc[1] == (uint32_t)(source.start >> 32) &&
         e->site[0].desc[2] == source.size &&
         e->smem_data[SH_U_SITE] == (uint32_t)source.start;
    sh_eval_free(e); free(e); free(p);
    return ok ? 0 : 2;
}

int main(void) {
    /* Synthetic pointer construction, then GLOBAL_LOAD_UBYTE v0,v[1:2],off.
     * SDWA SRC1_SGPR makes the encoded v13 operand refer to s13. */
    const uint32_t sdwa_high[] = {
        0xd70f6a01, 0x00020c0c, /* v_add_co_u32 v1,vcc,s12,v6 */
        0x50041af9, 0x86860680, /* v_add_co_ci v2,0,s13 (SDWA) */
        0xdc200000, 0x007d0001, 0xbf810000
    };
    const uint32_t sdwa_both[] = {
        0x4a020cf9, 0x0686060c, /* v_add_nc_u32 v1,s12,v6 (SDWA) */
        0x50041af9, 0x86860680,
        0xdc200000, 0x007d0001, 0xbf810000
    };
    const uint32_t ordinary[] = {
        0xd70f6a01, 0x00020c0c,
        0xd70f6a02, 0x00020e0d, /* v_add_co_u32 v2,vcc,s13,v7 */
        0xdc200000, 0x007d0001, 0xbf810000
    };
    int result = check(ordinary, PS5C_ARRAY_COUNT(ordinary));
    printf("ordinary pointer: %d\n", result);
    if (!result) result = check(sdwa_high, PS5C_ARRAY_COUNT(sdwa_high));
    printf("SDWA high pointer: %d\n", result);
    if (!result) result = check(sdwa_both, PS5C_ARRAY_COUNT(sdwa_both));
    if (!result) puts("PASS: GLOBAL source pointer inferred through ordinary and SDWA operands");
    else fprintf(stderr, "FAIL: GLOBAL source buffer was not inferred correctly (%d)\n", result);
    return result;
}
