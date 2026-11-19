/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Dynamic scalar buffer offsets: the evaluator's snapshot is insufficient.
 * PS5C_TEST_GPU=1 also executes the synthetic program on the host GPU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/gpu_regs.h"
#include "gpu/renderer.h"
#include "gpu/shader/shader.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static const uint32_t code[] = {
    0x7e020210,             /* v_mov_b32 v1, s16 (workgroup X) */
    0x34040282,             /* v_lshlrev_b32 v2, 2, v1 */
    0x7e120502,             /* v_readfirstlane_b32 s9, v2 */
    0xf4200100, 0x12000000, /* s_buffer_load_dword s4, s[0:3], s9 */
    0x7e040204,             /* v_mov_b32 v2, s4 */
    0xe0702000, 0x80030201, /* buffer_store_dword v2, v1, s[12:15], 0 idxen */
    0xbf810000,
};

static void descriptor(uint32_t *d, guest_addr addr, uint32_t records) {
    d[0] = (uint32_t)addr;
    d[1] = (uint32_t)(addr >> 32) | (4u << 16);
    d[2] = records;
    d[3] = 0x00016204;
}

int main(void) {
    CHECK(pal_init() == PS5C_OK);
    gmem m;
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "sbuffer fixture", &a) == PS5C_OK);
    CHECK(gmem_write(&m, a, code, sizeof(code)));
    uint32_t *input = gmem_host(&m, a + 0x100, 256);
    uint32_t *output = gmem_host(&m, a + 0x200, 256);
    CHECK(input && output);
    for (uint32_t i = 0; i < 64; i++) input[i] = 0x13570000 + i * 17;

    sh_program *p = calloc(1, sizeof(*p));
    CHECK(p && sh_analyze(code, PS5C_ARRAY_COUNT(code), a, p));
    CHECK(p->site_count == 2 && p->sites[0].kind == SH_SITE_SBUFFER);
    CHECK(sh_binding_count(p) == 3); /* uniforms, scalar input, vector output */
    sh_eval e = {0};
    descriptor(e.sgpr_init, a + 0x100, 64);
    descriptor(e.sgpr_init + 12, a + 0x200, 64);
    CHECK(sh_evaluate(p, &m, &e));
    CHECK(e.site[0].valid && e.site[0].desc[0] == (uint32_t)(a + 0x100));
    CHECK(e.site[0].desc[2] == 64);
    sh_site_shape shapes[2] = {0};
    for (uint32_t i = 0; i < 2; i++) sh_site_shape_of(p, &e, i, &shapes[i]);
    sh_stage_info info = {0};
    info.stage = SH_STAGE_CS;
    info.local_size[0] = info.local_size[1] = info.local_size[2] = 1;
    info.cs_tgid_sgpr = 16;
    info.cs_tgid_enable = info.cs_tid_components = 1;
    uint32_t *spv = NULL;
    size_t words = 0;
    CHECK(sh_translate(p, &info, shapes, 0, &spv, &words));
    bool array_length = false;
    for (size_t i = 5; i < words;) {
        uint32_t n = spv[i] >> 16;
        CHECK(n && i + n <= words);
        array_length |= (spv[i] & 0xffff) == 68; /* OpArrayLength: bounds at execution */
        i += n;
    }
    CHECK(array_length);
    free(spv);
    sh_eval_free(&e);
    /* A scalar load may overwrite its own source descriptor. Capture it
     * before updating the destination registers in the evaluator. */
    static const uint32_t overlap[] = {0xf4200000, 0xfa000000, 0xbf810000};
    CHECK(sh_analyze(overlap, PS5C_ARRAY_COUNT(overlap), a, p));
    memset(&e, 0, sizeof(e));
    descriptor(e.sgpr_init, a + 0x100, 64);
    CHECK(sh_evaluate(p, &m, &e));
    CHECK(e.site[0].desc[0] == (uint32_t)(a + 0x100) && e.site[0].desc[2] == 64);
    sh_eval_free(&e);

    /* A scalar loop loads sixteen different addresses through one static
     * instruction. SH_U_SITE exposes its final snapshot, not the obsolete
     * allocations of previous iterations. A new evaluation must read again. */
    static const uint32_t scalar_loop[] = {
        0xbe880380,             /* s_mov_b32 s8, 0 */
        0xf4000100,0x10000000, /* s_load_dword s4, s[0:1], s8 */
        0x80088408,             /* s_add_u32 s8, s8, 4 */
        0xbf0ac008,             /* s_cmp_lt_u32 s8, 64 */
        0xbf85fffb,             /* s_cbranch_scc1 -5 */
        0xbf810000,
    };
    CHECK(sh_analyze(scalar_loop,PS5C_ARRAY_COUNT(scalar_loop),a,p));
    CHECK(p->site_count==1 && p->back_branch);
    memset(&e,0,sizeof(e));
    e.sgpr_init[0]=(uint32_t)(a+0x100);e.sgpr_init[1]=(uint32_t)((a+0x100)>>32);
    CHECK(sh_evaluate(p,&m,&e));
    CHECK(e.site[0].valid && e.site[0].smem_offset>=SH_U_DATA);
    CHECK(e.smem_data[e.site[0].smem_offset]==input[15]);
    CHECK(e.smem_data[SH_U_SITE]==e.site[0].smem_offset);
    if(!getenv("PS5C_NO_COMPACT_SMEM"))CHECK(e.smem_words==SH_U_DATA+1);
    input[15]^=0x00abcd00;
    CHECK(sh_evaluate(p,&m,&e));
    CHECK(e.smem_data[e.site[0].smem_offset]==input[15]);
    input[15]^=0x00abcd00;
    sh_eval_free(&e);

    /* An exit controlled by VCC can be unknown while EXEC stays known.
     * Discover the loop's resources without running 1024 placeholder turns.
     * A vector-dependent branch wholly inside a known scalar loop must not
     * truncate that loop. Neither case changes the translated GPU program. */
    static const uint32_t vector_exit[] = {
        0xbe880380,             /* s8 = 0 */
        0x7d820501,             /* v_cmp_lt_u32 vcc,v1,v2 */
        0xbf860004,             /* s_cbranch_vccz end */
        0xf4000100,0x10000000, /* s_load_dword s4,s[0:1],s8 */
        0x80088408,             /* s8 += 4 */
        0xbf82fffa,             /* s_branch compare */
        0xbf810000,
    };
    CHECK(sh_analyze(vector_exit,PS5C_ARRAY_COUNT(vector_exit),a,p));
    memset(&e,0,sizeof(e));
    e.sgpr_init[0]=(uint32_t)(a+0x100);e.sgpr_init[1]=(uint32_t)((a+0x100)>>32);
    e.sgpr_init[126]=e.sgpr_init[127]=UINT32_MAX;
    CHECK(sh_evaluate(p,&m,&e));
    CHECK(e.site[0].valid && e.smem_data[e.site[0].smem_offset]==input[2]);
    sh_eval_free(&e);
    static const uint32_t internal_vector_branch[] = {
        0xbe880380,0x7d820501, /* s8 = 0; v_cmp_lt_u32 vcc,v1,v2 */
        0xbf860001,0xbf800000, /* unknown branch skips only s_nop */
        0xf4000100,0x10000000,
        0x80088408,0xbf0ac008, /* s8 += 4; s_cmp_lt_u32 s8,64 */
        0xbf85fff8,0xbf810000, /* back to compare, then end */
    };
    CHECK(sh_analyze(internal_vector_branch,PS5C_ARRAY_COUNT(internal_vector_branch),a,p));
    memset(&e,0,sizeof(e));
    e.sgpr_init[0]=(uint32_t)(a+0x100);e.sgpr_init[1]=(uint32_t)((a+0x100)>>32);
    e.sgpr_init[126]=e.sgpr_init[127]=UINT32_MAX;
    CHECK(sh_evaluate(p,&m,&e));
    CHECK(e.site[0].valid && e.smem_data[e.site[0].smem_offset]==input[15]);
    sh_eval_free(&e);

    /* A known scalar pointer load restores s8 after a vector-dependent
     * write. The old unknown mark must not reject the subsequent buffer. */
    static const uint32_t restored[] = {
        0x7e100500,             /* v_readfirstlane_b32 s8, v0 */
        0xf4080200, 0xfa000000, /* s_load_dwordx4 s[8:11], s[0:1], 0 */
        0xe0302000, 0x80020200, /* buffer_load_dword v2, v0, s[8:11], 0 idxen */
        0xbf810000,
    };
    uint32_t *table = gmem_host(&m, a + 0x300, 32);
    CHECK(table);
    descriptor(table, a + 0x100, 64);
    CHECK(sh_analyze(restored, PS5C_ARRAY_COUNT(restored), a, p));
    memset(&e, 0, sizeof(e));
    e.sgpr_init[0] = (uint32_t)(a + 0x300);
    e.sgpr_init[1] = (uint32_t)((a + 0x300) >> 32);
    CHECK(sh_evaluate(p, &m, &e));
    CHECK(p->site_count == 2 && e.site[1].valid && !e.site[1].desc_unknown);
    CHECK(e.site[1].desc[0] == (uint32_t)(a + 0x100));
    sh_eval_free(&e);

    static const uint32_t buffer_restored[] = {
        0x7e100500,             /* v_readfirstlane_b32 s8, v0 */
        0xf4280206, 0xfa000000, /* s_buffer_load_dwordx4 s[8:11], s[12:15], 0 */
        0xe0302000, 0x80020200,
        0xbf810000,
    };
    CHECK(sh_analyze(buffer_restored, PS5C_ARRAY_COUNT(buffer_restored), a, p));
    memset(&e, 0, sizeof(e));
    descriptor(e.sgpr_init + 12, a + 0x300, 8);
    CHECK(sh_evaluate(p, &m, &e));
    CHECK(e.site[1].valid && !e.site[1].desc_unknown);
    CHECK(e.site[1].desc[0] == (uint32_t)(a + 0x100));
    sh_eval_free(&e);

    /* Conversely, loading a descriptor through a vector-dependent offset
     * cannot make that descriptor known. The snapshot is only one choice. */
    static const uint32_t dynamic_desc[] = {
        0x7e120500,             /* v_readfirstlane_b32 s9, v0 */
        0xf4280206, 0x12000000, /* s_buffer_load_dwordx4 s[8:11], s[12:15], s9 */
        0xe0302000, 0x80020200,
        0xbf810000,
    };
    CHECK(sh_analyze(dynamic_desc, PS5C_ARRAY_COUNT(dynamic_desc), a, p));
    memset(&e, 0, sizeof(e));
    descriptor(e.sgpr_init + 12, a + 0x300, 8);
    CHECK(sh_evaluate(p, &m, &e));
    CHECK(p->site_count == 2 && e.site[1].valid && e.site[1].desc_unknown);
    CHECK(e.site[1].desc[0] == (uint32_t)(a + 0x100));
    sh_eval_free(&e);

    /* Source and destination overlap: all four results must inherit the
     * unknown offset while replacing their own source descriptor. */
    static const uint32_t dynamic_overlap[] = {
        0x7e120500,             /* v_readfirstlane_b32 s9, v0 */
        0xf4280000, 0x12000000, /* s_buffer_load_dwordx4 s[0:3], s[0:3], s9 */
        0xe0302000, 0x80000200, /* buffer_load_dword v2, v0, s[0:3], 0 idxen */
        0xbf810000,
    };
    CHECK(sh_analyze(dynamic_overlap, PS5C_ARRAY_COUNT(dynamic_overlap), a, p));
    memset(&e, 0, sizeof(e));
    descriptor(e.sgpr_init, a + 0x300, 8);
    CHECK(sh_evaluate(p, &m, &e));
    CHECK(e.site[1].valid && e.site[1].desc_unknown);
    CHECK(e.site[0].desc[0] == (uint32_t)(a + 0x300));
    sh_eval_free(&e);

    /* Scalar dependencies follow the actual operand widths. In particular,
     * restoring a descriptor one word at a time must not read its neighbour.
     * Keep genuinely dynamic words rejected. */
    static const struct {
        uint32_t prefix[8];
        uint32_t count;
        bool unknown;
    } tracked[] = {
        {{0x7e000500, 0x7e180500, /* s0 and s12 unknown */
          0xbe800308, 0xbe810309, 0xbe82030a, 0xbe83030b}, 6, false},
        {{0x7e0a0500, 0xbe880304}, 2, false}, /* s_mov_b32 s8,s4: not s5 */
        {{0x7e0a0500, 0xbe880404}, 2, true},  /* s_mov_b64 s[8:9],s[4:5] */
        {{0x7e0a0500, 0xbe871004}, 2, false}, /* bcnt1_i32_b64 writes only s7 */
        {{0x7e0a0500, 0xbe871404}, 2, false}, /* ff1_i32_b64 writes only s7 */
        {{0x7e000500, 0xb0080040}, 2, false}, /* movk s8,64 has no SGPR input */
        {{0x7e100500, 0xb0080040}, 2, false}, /* movk replaces unknown destination */
        {{0x7e0a0500, 0xbf008081, 0xbe880505}, 3, false}, /* false cmov retains s8 */
        {{0x7e0a0500, 0xbf008080, 0xbe880505}, 3, true}, /* true cmov reads s5 */
        {{0x7e0a0500, 0xbf008081, 0x85080205}, 3, false}, /* cselect chooses known s2 */
        {{0x7e0a0500, 0xbf008080, 0x85080205}, 3, true}, /* cselect chooses unknown s5 */
        {{0x7e0a0500, 0xbf008080, 0xbe870305, 0x85080502}, 4, false}, /* mov preserves SCC */
    };
    for (uint32_t t = 0; t < PS5C_ARRAY_COUNT(tracked); t++) {
        uint32_t fixture[12];
        memcpy(fixture, tracked[t].prefix, tracked[t].count * 4);
        uint32_t n = tracked[t].count;
        fixture[n++] = 0xe0302000;
        fixture[n++] = t == 0 ? 0x80000200 : 0x80020200;
        fixture[n++] = 0xbf810000;
        CHECK(sh_analyze(fixture, n, a, p));
        memset(&e, 0, sizeof(e));
        descriptor(e.sgpr_init + 8, a + 0x100, 64);
        e.sgpr_init[4] = (uint32_t)(a + 0x100);
        e.sgpr_init[2] = (uint32_t)(a + 0x100);
        CHECK(sh_evaluate(p, &m, &e));
        if (e.site[0].desc_unknown != tracked[t].unknown)
            fprintf(stderr, "scalar dependency case %u: unknown=%d expected=%d\n", t,
                    e.site[0].desc_unknown, tracked[t].unknown);
        CHECK(e.site[0].valid && e.site[0].desc_unknown == tracked[t].unknown);
        if (t == 0 || t == 3 || t == 4)
            CHECK(memcmp(e.site[0].desc, e.sgpr_init + 8, 4 * sizeof(uint32_t)) == 0);
        sh_eval_free(&e);
    }
    free(p);

    if (getenv("PS5C_TEST_GPU")) {
        pal_window *window = NULL;
        CHECK(pal_window_open("ZeraPS5 scalar buffer regression", 320, 180, &window) == PS5C_OK);
        renderer *r = renderer_create(window, &m);
        CHECK(r);
        gpu g;
        CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
        g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(a >> 8);
        g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)(a >> 40);
        g.regs[mmCOMPUTE_PGM_RSRC2] = (16u << 1) | (1u << 7);
        g.regs[mmCOMPUTE_NUM_THREAD_X] = g.regs[mmCOMPUTE_NUM_THREAD_Y] = g.regs[mmCOMPUTE_NUM_THREAD_Z] = 1;
        descriptor(g.regs + mmCOMPUTE_USER_DATA_0, a + 0x100, 64);
        descriptor(g.regs + mmCOMPUTE_USER_DATA_0 + 12, a + 0x200, 64);
        /* Include one out-of-bounds input. It must read zero safely. */
        g.regs[mmCOMPUTE_USER_DATA_0 + 2] = 63;
        renderer_dispatch(r, &g, 64, 1, 1);
        renderer_sync_mem(r, a + 0x200, 256);
        for (uint32_t i = 0; i < 64; i++) {
            uint32_t expected = i < 63 ? input[i] : 0;
            if (output[i] != expected) fprintf(stderr, "group %u: %08x, expected %08x\n", i, output[i], expected);
            CHECK(output[i] == expected);
        }
        /* The dispatch guard must admit a loop after a descriptor is restored
         * from known SGPRs, even if its neighbour came from a vector. Before
         * the dependency fix this entire dispatch was skipped. */
        static const uint32_t restored_store[] = {
            0x7e000210, 0x7e020210, /* v0/v1 = workgroup X */
            0x7e000500, 0x7e180500, /* vector-dependent s0 and s12 */
            0xbe800308, 0xbe810309, 0xbe82030a, 0xbe83030b,
            0xb0120002,             /* s_movk_i32 s18,2 */
            0xe0702000, 0x80000100, /* buffer_store_dword v1,v0,s[0:3],0 idxen */
            0x80928112,             /* s_sub_u32 s18,s18,1 */
            0xbf078012,             /* s_cmp_lg_u32 s18,0 */
            0xbf85fffb, 0xbf810000, /* s_cbranch_scc1 store; s_endpgm */
        };
        CHECK(gmem_write(&m, a + 0x400, restored_store, sizeof(restored_store)));
        for (uint32_t i = 0; i < 64; i++) output[i] = 0xdeadbeef;
        g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)((a + 0x400) >> 8);
        memset(g.regs + mmCOMPUTE_USER_DATA_0, 0, 16 * sizeof(uint32_t));
        descriptor(g.regs + mmCOMPUTE_USER_DATA_0 + 8, a + 0x200, 64);
        renderer_dispatch(r, &g, 64, 1, 1);
        renderer_sync_mem(r, a + 0x200, 256);
        for (uint32_t i = 0; i < 64; i++) CHECK(output[i] == i);
        /* The CPU's bounded resource discovery must not bound the actual
         * shader. VCC exits this loop after sixteen vector counter values;
         * all sixteen distinct dynamic scalar loads must reach the GPU. */
        static const uint32_t vector_store[] = {
            0x7e020280,0x7e040290, /* v1=0; v2=16 */
            0x7d820501,             /* v_cmp_lt_u32 vcc,v1,v2 */
            0xbf860009,             /* s_cbranch_vccz end */
            0x34060282,             /* v3 = v1 << 2 */
            0x7e120503,             /* s9 = readfirstlane(v3) */
            0xf4200100,0x12000000, /* s4 = input[s9/4] */
            0x7e060204,             /* v3 = s4 */
            0xe0702000,0x80030301, /* output[v1] = v3 */
            0x4a020281,             /* v1 += 1 */
            0xbf82fff5,             /* s_branch compare */
            0xbf810000,
        };
        CHECK(gmem_write(&m,a+0x600,vector_store,sizeof(vector_store)));
        for(uint32_t i=0;i<64;i++)output[i]=0xdeadbeef;
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x600)>>8);
        memset(g.regs+mmCOMPUTE_USER_DATA_0,0,16*sizeof(uint32_t));
        descriptor(g.regs+mmCOMPUTE_USER_DATA_0,a+0x100,64);
        descriptor(g.regs+mmCOMPUTE_USER_DATA_0+12,a+0x200,64);
        renderer_dispatch(r,&g,1,1,1);
        renderer_sync_mem(r,a+0x200,256);
        for(uint32_t i=0;i<64;i++){
            uint32_t expected=i<16?input[i]:0xdeadbeef;
            if(output[i]!=expected)fprintf(stderr,"vector exit %u: %08x expected %08x\n",i,output[i],expected);
            CHECK(output[i]==expected);
        }
        renderer_destroy(r);
        free(g.regs);
        pal_mutex_destroy(g.lock);
        pal_window_destroy(window);
        puts("scalar buffers: GPU verified 64 dynamic offsets, OOB, 64 restored-descriptor loops and all 16 vector-controlled iterations");
    }
    gmem_free(&m);
    pal_shutdown();
    puts("scalar buffers: ok");
    return 0;
}
