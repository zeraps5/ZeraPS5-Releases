/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include "gpu/vk/vk_vertex_ranges.h"

/* the analysis reads per-instruction flags that sh_analyze fills in */
static uint64_t prefix_of(sh_program *p, const sh_stage_info *info, const sh_site_value *v, uint32_t vmax, uint32_t imax,
                          uint32_t stride) {
    sh_program_flags(p);
    return vr_site_prefix(p, 0, info, v, vmax, imax, stride);
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int check_flow_cache(void) {
    sh_program *p = calloc(1, sizeof(*p));
    sh_program *q = calloc(1, sizeof(*q));
    CHECK(p && q);
    vr_flow_cache cache = {0};
    const char *names[] = {"s_nop", "s_setpc_b64", "s_swappc_b64", "v_movrel_b32", NULL};
    p->count = 32;
    p->has_flow = true;
    /* Reanalysis in the SAME allocation must never retain a previous verdict.
     * Alternate accepted/rejected programs and repeated lookups between epochs. */
    for (unsigned n = 0; n < 80; n++) {
        vr_flow_invalidate(&cache);
        p->insts[31].name = names[n % 5];
        p->has_stores = (n % 7 == 0);
        p->back_branch = (n % 11 == 0);
        bool expected = vr_straight_line(p);
        for (unsigned draw = 0; draw < 100; draw++) {
            CHECK(vr_flow_cached(&cache, p) == expected);
            CHECK(vr_flow_cached(&cache, q) == vr_straight_line(q));
        }
        /* A colliding cache entry is replaced, never used for another shader. */
        size_t slot = ((uintptr_t)p >> 4) % 64;
        cache.entries[slot].program = q;
        cache.entries[slot].straight = !expected;
        CHECK(vr_flow_cached(&cache, p) == expected);
    }
    vr_flow_invalidate(&cache); /* owner invalidates before destruction/reuse */
    for (unsigned n = 0; n < 64; n++) CHECK(cache.entries[n].program == NULL);
    free(p);
    free(q);
    return 0;
}

int main(void) {
    CHECK(check_flow_cache() == 0);
    sh_program *p = calloc(1, sizeof(*p));
    if (!p) return 1;
    sh_stage_info info = {.stage = SH_STAGE_VS, .vertex_id_vgpr = 5, .instance_id_vgpr = 8};
    p->count = 4;
    p->site_count = 1;
    p->sites[0] = (sh_site){.inst = 3, .kind = SH_SITE_BUFFER};
    p->insts[0] = (rdna2_inst){.enc = RDNA2_ENC_SOP2, .name = "s_bfe_u32", .dst = 107,
        .src = {53, 255, 125}, .literal = 0xc000e};
    p->ops[0] = SH_OP_S_BFE_U32;
    p->insts[1] = (rdna2_inst){.enc = RDNA2_ENC_VOP3, .name = "v_cndmask_b32", .dst = 257,
        .src = {264, 261, 48}};
    p->ops[1] = SH_OP_V_CNDMASK_B32;
    p->insts[2] = (rdna2_inst){.enc = RDNA2_ENC_SOPP, .name = "s_waitcnt"};
    p->ops[2] = SH_OP_S_WAITCNT;
    p->insts[3] = (rdna2_inst){.enc = RDNA2_ENC_MUBUF, .name = "buffer_load_format_xyz",
        .vaddr = 1, .vdata = 9, .idxen = true, .soffset = 107};
    p->ops[3] = SH_OP_BUFFER_LOAD_FORMAT;
    sh_program_flags(p);
    CHECK(vr_straight_line(p));
    CHECK(vr_vertex_max(31, 288) == 319);
    CHECK(vr_vertex_max(31, -1) == UINT32_MAX);
    CHECK(vr_vertex_max(UINT32_MAX - 3, 4) == UINT32_MAX);
    CHECK(vr_vertex_max(UINT32_MAX, 0) == UINT32_MAX);
    for (uint32_t max = 0; max < 2048; max++) {
        uint32_t instances = max % 17, index_max = max > instances ? max : instances;
        uint64_t prefix = prefix_of(p, &info, NULL, max, instances, 24);
        CHECK(prefix == (uint64_t)index_max * 24 + 4095 + 64);
        /* Actual accesses at either input and every scalar offset are covered. */
        for (uint32_t soff = 0; soff <= 4095; soff += 7) {
            CHECK((uint64_t)max * 24 + soff + 16 <= prefix);
            CHECK((uint64_t)instances * 24 + soff + 16 <= prefix);
        }
        /* A later draw starts within the same vertex buffer. Its effective
         * indices, including the offset, must stay inside the staged prefix. */
        for (int32_t base = 1; base <= 288; base += 41) {
            uint64_t shifted = prefix_of(p, &info, NULL, vr_vertex_max(max, base), instances, 24);
            CHECK((uint64_t)(max + base) * 24 + 4095 + 16 <= shifted);
        }
    }
    CHECK(prefix_of(p, &info, NULL, UINT32_MAX, 0, 24) == UINT64_MAX);
    CHECK(prefix_of(p, &info, NULL, UINT32_MAX - 1, 0, 24) == UINT64_MAX);
    CHECK(prefix_of(p, &info, NULL, 100, 200, 24) == 200 * 24 + 4095 + 64);
    p->insts[3].offen = true;
    CHECK(prefix_of(p, &info, NULL, 100, 0, 24) == UINT64_MAX);
    p->insts[3].offen = false;
    p->insts[3].soffset = 50; /* scalar memory value: not proven */
    CHECK(prefix_of(p, &info, NULL, 100, 0, 24) == UINT64_MAX);
    /* unless the evaluation knows it */
    sh_site_value v = {.valid = true, .soffset = 12, .soffset_known = true};
    CHECK(prefix_of(p, &info, &v, 100, 0, 24) == 100 * 24 + 12 + 64);
    v.soffset_known = false;
    CHECK(prefix_of(p, &info, &v, 100, 0, 24) == UINT64_MAX);
    p->insts[3].soffset = 107;
    p->insts[2] = (rdna2_inst){.enc = RDNA2_ENC_VOP1, .name = "v_mov_b32", .dst = 257, .src = {40}};
    p->ops[2] = SH_OP_V_MOV_B32;
    CHECK(prefix_of(p, &info, NULL, 100, 0, 24) == UINT64_MAX);
    p->insts[2] = (rdna2_inst){.enc = RDNA2_ENC_SOP1, .name = "s_mov_b64", .dst = 126};
    p->ops[2] = SH_OP_S_MOV_B64;
    CHECK(prefix_of(p, &info, NULL, 100, 0, 24) == UINT64_MAX); /* inactive lanes may be enabled */
    p->insts[2] = (rdna2_inst){.enc = RDNA2_ENC_SOPP, .name = "s_cbranch_execz"};
    p->ops[2] = SH_OP_S_CBRANCH_EXECZ;
    for (uint32_t i = 0; i < p->count; i++) p->target[i] = (uint16_t)p->count;
    p->target[2] = 4; /* forward, past the load */
    sh_program_flags(p);
    CHECK(vr_straight_line(p));
    CHECK(prefix_of(p, &info, NULL, 100, 0, 24) == 100 * 24 + 4095 + 64);
    p->target[2] = 3; /* lands on the load: a path that skips nothing still writes first, but a join is refused */
    CHECK(prefix_of(p, &info, NULL, 100, 0, 24) == UINT64_MAX);
    p->insts[0] = (rdna2_inst){.enc = RDNA2_ENC_SOPP, .name = "s_cbranch_scc1"};
    p->ops[0] = SH_OP_S_CBRANCH_SCC1;
    p->target[0] = 2; /* skips the index write: the load may see v1 unwritten */
    p->target[2] = 4;
    CHECK(prefix_of(p, &info, NULL, 100, 0, 24) == UINT64_MAX);
    p->insts[0] = (rdna2_inst){.enc = RDNA2_ENC_SOP2, .name = "s_bfe_u32", .dst = 107,
        .src = {53, 255, 125}, .literal = 0xc000e};
    p->ops[0] = SH_OP_S_BFE_U32;
    p->target[0] = 4;
    p->back_branch = true; /* a loop */
    CHECK(!vr_straight_line(p));
    p->back_branch = false;
    for (uint32_t i = 0; i < p->count; i++) p->target[i] = 0;
    p->insts[2] = (rdna2_inst){.enc = RDNA2_ENC_SOPP, .name = "s_waitcnt"};
    p->ops[2] = SH_OP_S_WAITCNT;
    p->insts[1].dpp_sdwa = 1;
    CHECK(prefix_of(p, &info, NULL, 100, 0, 24) == UINT64_MAX);
    p->insts[1].dpp_sdwa = 0;
    /* the second attribute of a fetch shader: an earlier fetch wrote v[0:2] only, so the vertex
     * and instance ids (v5, v8) it selects from are intact; a fetch into v[4:6] would not leave them */
    rdna2_inst saved1 = p->insts[1];
    p->insts[1] = (rdna2_inst){.enc = RDNA2_ENC_MUBUF, .name = "buffer_load_format_xyz", .vdata = 0, .idxen = true};
    p->ops[1] = SH_OP_BUFFER_LOAD_FORMAT;
    p->insts[2] = saved1;
    p->insts[2].dst = 259; /* v3 */
    p->ops[2] = SH_OP_V_CNDMASK_B32;
    p->insts[3].vaddr = 3;
    CHECK(prefix_of(p, &info, NULL, 100, 0, 24) == 100 * 24 + 4095 + 64);
    p->insts[1].vdata = 4;
    CHECK(prefix_of(p, &info, NULL, 100, 0, 24) == UINT64_MAX);
    p->insts[1] = saved1;
    p->ops[1] = SH_OP_V_CNDMASK_B32;
    p->insts[2] = (rdna2_inst){.enc = RDNA2_ENC_SOPP, .name = "s_waitcnt"};
    p->ops[2] = SH_OP_S_WAITCNT;
    p->insts[3].vaddr = 1;
    p->has_stores = true;
    sh_program_flags(p);
    CHECK(!vr_straight_line(p));
    free(p);
    puts("vertex ranges: conservative bounds and fallback checks passed");
    return 0;
}
