/* SPDX-License-Identifier: GPL-2.0-or-later */
/* sh_store_range on Minecraft's dword copy shader (dst[dstOff + i] = src[srcOff + i]
 * for i < count; count and offsets as floats in a constant buffer): the store
 * window follows the dispatch, and what the analysis cannot prove is refused. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/shader/shader.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static const uint32_t k_copy[] = {
    0xBFA00001u,              /* s_inst_prefetch 0x1 */
    0xD7460000u, 0x04010C0Cu, /* v_lshl_add_u32 v0, s12, 6, v0 */
    0xF4201A84u, 0xFA000010u, /* s_buffer_load_dword vcc_lo, s[8:11], 0x10 (count) */
    0xBF8CC07Fu,              /* s_waitcnt */
    0x7E020E6Au,              /* v_cvt_u32_f32 v1, vcc_lo */
    0x7DA20300u,              /* v_cmpx_lt_u32 v0, v1 */
    0xBF88000Fu,              /* s_cbranch_execz (to s_endpgm) */
    0xF4201A84u, 0xFA000000u, /* s_buffer_load_dword vcc_lo, s[8:11], 0x0 (srcOff) */
    0xBF8CC07Fu,
    0x7E020E6Au,              /* v_cvt_u32_f32 v1, vcc_lo */
    0xF4201A84u, 0xFA000020u, /* s_buffer_load_dword vcc_lo, s[8:11], 0x20 (dstOff) */
    0x4A040101u,              /* v_add_nc_u32 v2, v1, v0 */
    0xBF8CC07Fu,
    0x7E020E6Au,              /* v_cvt_u32_f32 v1, vcc_lo */
    0xE0302000u, 0x80000202u, /* buffer_load_dword v2, v2, s[0:3], 0 idxen */
    0x4A000101u,              /* v_add_nc_u32 v0, v1, v0 */
    0xBF8C3F70u,
    0xE0702000u, 0x80010200u, /* buffer_store_dword v2, v0, s[4:7], 0 idxen */
    0xBF810000u,              /* s_endpgm */
};

#define CONST_ADDR 0x10000ull
static float g_consts[16];

static void *host(void *user, guest_addr addr, uint64_t size) {
    (void)user;
    if (addr >= CONST_ADDR && addr + size <= CONST_ADDR + sizeof(g_consts)) return (uint8_t *)g_consts + (addr - CONST_ADDR);
    return NULL;
}

int main(void) {
    sh_program *p = calloc(1, sizeof(*p));
    sh_eval *e = calloc(1, sizeof(*e));
    uint32_t (*vsrc)[3] = calloc(SH_MAX_INSTS, sizeof(*vsrc));
    uint8_t (*vstate)[3] = calloc(SH_MAX_INSTS, sizeof(*vstate));
    CHECK(p && e && vsrc && vstate);
    CHECK(sh_analyze(k_copy, sizeof(k_copy) / 4, 0x100000, p));

    g_consts[0] = 5000.0f;  /* srcOff */
    g_consts[4] = 650.0f;   /* count */
    g_consts[8] = 100000.0f; /* dstOff */
    /* s[0:3] source V#, s[4:7] destination V# (stride 4, 4 MB), s[8:11] constants */
    uint32_t vsharp_dst[4] = {0x2000000u, (4u << 16), 0x100000u, 0};
    uint32_t vsharp_src[4] = {0x3000000u, (4u << 16), 0x100000u, 0};
    uint32_t vsharp_const[4] = {(uint32_t)CONST_ADDR, 0, sizeof(g_consts), 0};
    memcpy(&e->sgpr_init[0], vsharp_src, 16);
    memcpy(&e->sgpr_init[4], vsharp_dst, 16);
    memcpy(&e->sgpr_init[8], vsharp_const, 16);
    e->host = host;
    e->vsrc = vsrc;
    e->vsrc_state = vstate;
    CHECK(sh_evaluate(p, NULL, e));

    sh_stage_info info;
    memset(&info, 0, sizeof(info));
    info.stage = SH_STAGE_CS;
    info.local_size[0] = 64;
    info.local_size[1] = info.local_size[2] = 1;
    info.cs_tid_components = 1;
    info.cs_tgid_sgpr = 12;
    info.cs_tgid_enable = 1;
    uint32_t groups[3] = {11, 1, 1}; /* ceil(650 / 64) */
    uint32_t store = p->site_count; /* the buffer store (the scalar loads are sites too) */
    for (uint32_t i = 0; i < p->site_count; i++)
        if (p->sites[i].kind == SH_SITE_BUFFER && p->sites[i].store) store = i;
    CHECK(store < p->site_count);
    uint64_t lo = 0, hi = 0;
    CHECK(sh_store_range(p, e, &info, groups, store, &lo, &hi));
    /* The upper bound includes dstOff. Retaining inactive lanes' previous
     * values conservatively widens the lower bound to zero. */
    CHECK(lo == 0 && hi == (100000ull + 11 * 64 - 1) * 4 + 4);

    /* a larger dispatch widens the window */
    groups[0] = 1000;
    CHECK(sh_store_range(p, e, &info, groups, store, &lo, &hi) && hi == (100000ull + 1000 * 64 - 1) * 4 + 4);
    groups[0] = 11;

    /* without the operand records, or with the offset unknown, nothing is proven */
    e->vsrc = NULL;
    CHECK(!sh_store_range(p, e, &info, groups, store, &lo, &hi));
    e->vsrc = vsrc;
    for (uint32_t j = 0; j < p->count; j++) vstate[j][0] = vstate[j][0] == 1 ? 2 : vstate[j][0];
    CHECK(!sh_store_range(p, e, &info, groups, store, &lo, &hi));

    /* READFIRSTLANE encodes s0 in VDST, but leaves the global index in v0
     * intact. An EXEC restore must preserve bounds of that unchanged v0. */
    static const uint32_t restore[]={0xd7460000,0x04010c0c,0x7e020208,
        0xbeea047e,0x7da40080,0xbf880001,0x7e0a0280,0xbefe046a,
        0x7e000501,0xe0702010,0x80010200,0xbf810000};
    CHECK(sh_analyze(restore,sizeof(restore)/4,0x200000,p));
    CHECK(sh_evaluate(p,NULL,e));
    store=p->site_count-1;
    CHECK(sh_store_range(p,e,&info,groups,store,&lo,&hi));
    CHECK(lo==16 && hi==(11*64-1)*4+20);
    /* Unknown record count does not make a known dispatch index unknown. */
    e->site[store].records_unknown=true;
    CHECK(sh_store_range(p,e,&info,groups,store,&lo,&hi));
    CHECK(hi==(11*64-1)*4+20);
    /* A predicated v0 overwrite followed by EXEC restore keeps both values,
     * including the original indices in inactive lanes. */
    p->insts[6].dst=RDNA2_OP_VGPR;p->insts[6].src[0]=128;
    CHECK(sh_store_range(p,e,&info,groups,store,&lo,&hi));
    CHECK(hi==(11*64-1)*4+20);
    /* A readlane can replace a workgroup SGPR too. It is then a scalar
     * constant (9), not the original workgroup interval [0,10]. */
    static const uint32_t replaced_group[]={0x7e020289,0x7e180501,
        0xd7460000,0x04010c0c,0xe0702000,0x80010200,0xbf810000};
    CHECK(sh_analyze(replaced_group,sizeof(replaced_group)/4,0x300000,p));
    CHECK(sh_evaluate(p,NULL,e));store=p->site_count-1;
    /* Supply the scalar operand record explicitly: this test exercises the
     * address proof independently of the uniform evaluator's coverage. */
    vsrc[2][0]=9;vstate[2][0]=1;
    CHECK(sh_store_range(p,e,&info,groups,store,&lo,&hi));
    CHECK(lo==9*64*4 && hi==(9*64+63)*4+4);

    sh_eval_free(e);
    free(vsrc);
    free(vstate);
    free(e);
    free(p);
    printf("store range: ok\n");
    return 0;
}
