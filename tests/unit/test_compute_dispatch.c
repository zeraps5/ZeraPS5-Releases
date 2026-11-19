/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Coverage, integer boundaries, and real GPU guest group IDs across parts. */
#include "gpu/gpu_regs.h"
#include "gpu/shader/shader.h"
#include "gpu/shader/shader_barrier.h"
#include "gpu/shader/spv.h"
#include "gpu/vk/vk_compute_dispatch.h"
#include "gpu/vk/vk_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int partition_test(void) {
    for (uint32_t x = 1; x <= 10; x++)
        for (uint32_t y = 1; y <= 10; y++)
            for (uint32_t z = 1; z <= 10; z++) {
                uint32_t total[3] = {x, y, z}, limit[3] = {3, 4, 2};
                unsigned char seen[1000] = {0};
                vk_compute_partition p;
                vk_compute_part part;
                vk_compute_partition_init(&p, total, limit);
                while (vk_compute_partition_next(&p, &part)) {
                    for (unsigned k = 0; k < 3; k++)
                        CHECK(part.count[k] && part.count[k] <= limit[k] &&
                              part.count[k] <= total[k] - part.base[k]);
                    for (uint32_t zz = 0; zz < part.count[2]; zz++)
                        for (uint32_t yy = 0; yy < part.count[1]; yy++)
                            for (uint32_t xx = 0; xx < part.count[0]; xx++) {
                                uint32_t i = part.base[0] + xx +
                                             x * (part.base[1] + yy + y * (part.base[2] + zz));
                                CHECK(seen[i]++ == 0);
                            }
                }
                for (uint32_t i = 0; i < x * y * z; i++)
                    CHECK(seen[i] == 1);
            }
    uint32_t total[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX},
             limit[3] = {UINT32_MAX - 1, UINT32_MAX - 1, UINT32_MAX - 1};
    vk_compute_partition p;
    vk_compute_part part;
    vk_compute_partition_init(&p, total, limit);
    unsigned parts = 0;
    while (vk_compute_partition_next(&p, &part)) {
        for (unsigned k = 0; k < 3; k++) {
            CHECK(part.base[k] == (((parts >> k) & 1) ? UINT32_MAX - 1 : 0));
            CHECK(part.count[k] == (((parts >> k) & 1) ? 1 : UINT32_MAX - 1));
        }
        CHECK(++parts <= 8);
    }
    CHECK(parts == 8 && !vk_compute_partition_next(&p, &part));
    for (unsigned k = 0; k < 3; k++) {
        uint32_t zero[3] = {1, 1, 1};
        zero[k] = 0;
        vk_compute_partition_init(&p, zero, limit);
        CHECK(!vk_compute_partition_next(&p, &part));
        vk_compute_partition_init(&p, total, zero);
        CHECK(!vk_compute_partition_next(&p, &part));
    }
    puts("compute partition: 1000 grids, UINT32_MAX and empty axes verified");
    return 0;
}
static uint32_t sop2(unsigned op, unsigned dst, unsigned a, unsigned b) {
    return 0x80000000u | (op << 23) | (dst << 16) | (b << 8) | a;
}
/* Cross-wave LDS exchange, with a uniform SCC exit over the barrier and
 * lane-dependent VCC flow entirely inside the preceding phase. */
static unsigned lds_program(uint32_t *code, bool reuse) {
    unsigned n = 0;
    code[n++] = 0xbe8a0304; /* s10 = s4, immutable workgroup-uniform condition */
    code[n++] = 0xbf06800a; /* s_cmp_eq_u32 s10, 0 */
    unsigned exit = n++;
    code[n++] = 0x36060081; /* v3 = tid & 1 */
    code[n++] = 0x7d840680; /* v_cmp_eq_u32 vcc, 0, v3 */
    code[n++] = 0xbf870001; /* s_cbranch_vccnz +1 (reconverges before LDS) */
    code[n++] = 0xbf800000; /* s_nop */
    code[n++] = 0x34020082; /* v1 = tid * 4 */
    code[n++] = 0xd8340000; code[n++] = 0x00000001; /* ds_write_b32 v1, v0 */
    code[n++] = 0xbf8a0000;
    code[n++] = 0x3a0400ff; code[n++] = reuse ? 1 : 128; /* partner within/across waves */
    code[n++] = 0x34040482; /* v2 *= 4 */
    code[n++] = 0xd8d80000; code[n++] = 0x04000002; /* ds_read_b32 v4, v2 */
    code[n++] = 0xbf8cc17f; /* lgkmcnt(1): this is not yet completion */
    code[n++] = 0x7e0c0280; /* independent ALU between LDS and its completed wait */
    code[n++] = 0xbf8cc07f; /* s_waitcnt */
    if (reuse) {
        code[n++] = 0x7e0402ff; code[n++] = 0xfeed1234;
        code[n++] = 0xd8340000; code[n++] = 0x00000201; /* overwrite after every neighbour has read */
    }
    code[n++] = 0x7e060208; /* v3 = workgroup X (s8) */
    code[n++] = 0x34060688; /* v3 *= 256 */
    code[n++] = 0x4a060700; /* v3 += tid */
    code[n++] = 0xe0702000; code[n++] = 0x80000403; /* store v4, indexed by v3 */
    code[exit] = 0xbf850000 | (n - exit - 1);
    code[n++] = 0xbf810000;
    return n;
}
static unsigned lds_broadcast_program(uint32_t *code) {
    unsigned n = 0;
    code[n++] = 0x34020082; /* v1 = tid * 4 */
    code[n++] = 0xd8340000; code[n++] = 0x00000001;
    code[n++] = 0xbf8a0000;
    code[n++] = 0x7e020280; /* constant address zero */
    code[n++] = 0xd8d80000; code[n++] = 0x04000001; /* broadcast LDS[0] into v4 */
    code[n++] = 0xbf8cc07f;
    code[n++] = 0x7d840804; /* v_cmp_eq_u32 vcc, s4, v4 */
    unsigned exit = n++;
    code[n++] = 0xbf8a0000; /* every lane either takes the uniform exit or reaches this barrier */
    code[n++] = 0x7e060208;
    code[n++] = 0x34060688;
    code[n++] = 0x4a060700;
    code[n++] = 0xe0702000; code[n++] = 0x80000403;
    code[exit] = 0xbf870000 | (n - exit - 1);
    code[n++] = 0xbf810000;
    return n;
}
/* The barrier is in a forward-only prefix; the counted loop is reached only
 * after all LDS exchanges and must not disable that earlier phase boundary. */
static unsigned lds_suffix_program(uint32_t *code, bool reuse) {
    unsigned n = lds_program(code, reuse) - 1;
    code[n++] = 0xb00a0003; /* s_movk_i32 s10, 3 */
    unsigned loop = n;
    code[n++] = sop2(1, 10, 10, 129); /* s_sub_u32 s10, s10, 1 */
    code[n++] = 0xbf07800a; /* s_cmp_lg_u32 s10, 0 */
    code[n] = 0xbf850000u | ((loop - n - 1) & 0xffff); n++;
    code[n++] = 0xbf810000;
    return n;
}
/* Two entries into the counted loop defeat sf_range's nested-loop model.
 * The prefix still has just one uniform exit and no edge can re-enter it. */
static unsigned lds_dispatch_suffix_program(uint32_t *code, bool reuse) {
    unsigned n = lds_program(code, reuse) - 1;
    code[n++] = 0xb00a0003;
    code[n++] = 0xbf068108; /* s_cmp_eq_u32 s8, 1 */
    code[n++] = 0xbf840002;
    code[n++] = 0xbf068004; /* s_cmp_eq_u32 s4, 0 */
    code[n++] = 0xbf840001;
    unsigned loop = n;
    code[n++] = 0xbf800000;
    code[n++] = sop2(1, 10, 10, 129);
    code[n++] = 0xbf07800a;
    code[n] = 0xbf850000u | ((loop - n - 1) & 0xffff); n++;
    code[n++] = 0xbf810000;
    code[2] = 0xbf850000u | (n - 4); /* the prefix's uniform exit targets terminal END */
    return n;
}
/* A VCC exit cannot use the earlier prefix proof. The partial variant
 * terminates the upper half of the group before the remaining waves rendezvous. */
static unsigned lds_vcc_program(uint32_t *code,bool partial) {
    unsigned n=lds_dispatch_suffix_program(code,partial);
    code[0]=partial?0x2c0a0087:0x7e0a0204; /* v5=tid>>7, or uniform s4 */
    code[1]=partial?0x7d840a81:0x7d840a80;
    code[2]=(code[2]&0xffff)|0xbf870000;
    return n;
}
/* Two native wave32s take different arms, write LDS, then read the other
 * wave's data after the real workgroup barrier. No commercial shader. */
static unsigned join_prefix_program(uint32_t code[64],uint32_t low,uint32_t high) {
    const uint32_t body[]={0x7e080500,0xbf068004,0xbf840003,
        0x7e0402ff,low,0xbf820002,0x7e0402ff,high,
        0x34020082,0xd8340000,0x00000201,0xbf8a0000,
        0x3a0400a0,0x34040482,0xd8d80000,0x04000002,0xbf8cc07f,
        0x7e060208,0x34060686,0x4a060700,0xe0702000,0x80000403,0xbf810000};
    memcpy(code,body,sizeof(body));return PS5C_ARRAY_COUNT(body);
}
static int join_prefix_test(void) {
    sh_program *p=calloc(1,sizeof(*p));uint32_t code[64];CHECK(p);
    unsigned n=join_prefix_program(code,11,29);CHECK(sh_analyze(code,n,0,p));
    unsigned barrier=0;while(barrier<p->count && p->ops[barrier]!=SH_OP_S_BARRIER)barrier++;
    CHECK(sh_barrier_join_prefix(p,barrier));
    unsigned branch=2;uint16_t saved=p->target[branch];
    p->target[branch]=(uint16_t)(barrier+1);CHECK(!sh_barrier_join_prefix(p,barrier));
    p->target[branch]=0;CHECK(!sh_barrier_join_prefix(p,barrier));p->target[branch]=saved;
    uint16_t op=p->ops[p->count-1];uint8_t flags=p->iflags[p->count-1];
    p->ops[p->count-1]=SH_OP_S_BRANCH;p->target[p->count-1]=0;CHECK(!sh_barrier_join_prefix(p,barrier));
    p->ops[p->count-1]=SH_OP_S_SWAPPC_B64;p->iflags[p->count-1]=SH_IF_FLOW;CHECK(!sh_barrier_join_prefix(p,barrier));
    p->ops[p->count-1]=SH_OP_S_NOP;CHECK(sh_barrier_join_prefix(p,barrier)); /* debugger-disabled no-op */
    p->ops[p->count-1]=op;p->iflags[p->count-1]=flags;
    p->ops[0]=SH_OP_S_BARRIER;CHECK(!sh_barrier_join_prefix(p,barrier));
    free(p);puts("first LDS barrier: acyclic if/else join, escape/backedge/indirection refusals correct");return 0;
}
static int barrier_test(void) {
    CHECK(join_prefix_test()==0);
    sh_program *p = calloc(1, sizeof(*p));
    bool *safe = calloc(SH_MAX_INSTS, sizeof(*safe));
    sh_site_shape *shapes = calloc(SH_MAX_SITES, sizeof(*shapes));
    uint32_t code[64]; unsigned n = lds_program(code, true);
    CHECK(p && safe && shapes && sh_analyze(code, n, 0, p));
    sh_barrier_convergence(p, shapes, safe);
    unsigned barrier = 0;
    while (barrier < p->count && p->ops[barrier] != SH_OP_S_BARRIER) barrier++;
    CHECK(barrier < p->count && safe[barrier]);
    sh_stage_info info = {.stage = SH_STAGE_CS, .local_size = {256, 1, 1},
                         .cs_tgid_sgpr = 8, .cs_tgid_enable = 1,
                         .cs_tid_components = 1, .lds_dwords = 256};
    uint32_t *spv = NULL; size_t words = 0; unsigned controls = 0;
    CHECK(sh_translate(p, &info, shapes, 0, &spv, &words));
    for (size_t i = 5; i < words; i += spv[i] >> 16) {
        CHECK(spv[i] >> 16);
        controls += (spv[i] & 65535) == SpvOpControlBarrier;
    }
    CHECK(controls == 2); /* workgroup phase boundary and wave-local LDS wait */
    unsigned wait = 0;
    while (wait < p->count && !sh_barrier_lds_wait(p, wait)) wait++;
    CHECK(wait < p->count);
    p->insts[wait].simm16 |= 0x100;
    CHECK(!sh_barrier_lds_wait(p, wait)); /* lgkmcnt(1) is not completion */
    p->insts[wait].simm16 &= ~0x100;
    unsigned read = wait;
    while (read && p->insts[read].enc != RDNA2_ENC_DS) read--;
    CHECK(read && p->insts[read].enc == RDNA2_ENC_DS);
    p->insts[read].gds = true;
    CHECK(!sh_barrier_lds_wait(p, wait));
    p->insts[read].gds = false;
    if (getenv("PS5C_TEST_SPIRV")) {
        char path[512]; snprintf(path, sizeof(path), "%s/compute_lds_phase.spv", getenv("PS5C_TEST_SPIRV"));
        FILE *f = fopen(path, "wb");
        CHECK(f && fwrite(spv, 4, words, f) == words && !fclose(f));
    }
    free(spv);
    /* Rewriting the scalar after divergence, lane masks and unknown transfers
     * must never enable a workgroup execution barrier. */
    unsigned compare = p->ops[1], branch = p->ops[2], saved = p->insts[0].src[0];
    p->insts[0].src[0] = RDNA2_OP_VCC_LO;
    sh_barrier_convergence(p, shapes, safe); CHECK(!safe[barrier]);
    p->insts[0].src[0] = saved;
    p->insts[6].enc = RDNA2_ENC_SOP1; p->insts[6].dst = 10;
    sh_barrier_convergence(p, shapes, safe); CHECK(!safe[barrier]);
    p->insts[6].enc = RDNA2_ENC_SOPP;
    p->ops[2] = SH_OP_S_CBRANCH_VCCNZ;
    sh_barrier_convergence(p, shapes, safe); CHECK(!safe[barrier]);
    p->ops[2] = SH_OP_S_CBRANCH_EXECZ;
    sh_barrier_convergence(p, shapes, safe); CHECK(!safe[barrier]);
    p->ops[2] = branch; p->ops[1] = SH_OP_S_ADD_U32;
    sh_barrier_convergence(p, shapes, safe); CHECK(!safe[barrier]);
    p->ops[1] = compare; p->target[2] = 0;
    sh_barrier_convergence(p, shapes, safe); CHECK(!safe[barrier]);
    p->target[2] = p->count - 1;
    p->ops[0] = SH_OP_S_CBRANCH_VCCNZ; p->insts[0].enc = RDNA2_ENC_SOPP; p->target[0] = 2;
    sh_barrier_convergence(p, shapes, safe); CHECK(!safe[barrier]); /* skipped SCC compare */
    p->target[0] = 3;
    sh_barrier_convergence(p, shapes, safe); CHECK(!safe[barrier]); /* bypassed the entire scalar exit */
    p->ops[0] = SH_OP_S_LOAD_DWORD; p->insts[0].enc = RDNA2_ENC_SMEM;
    p->site_of[0] = 0; p->sites[0].kind = SH_SITE_SMEM;
    p->insts[6].enc = RDNA2_ENC_SMEM; p->insts[6].dst = 2;
    p->ops[6] = SH_OP_S_LOAD_DWORDX8;
    sh_barrier_convergence(p, shapes, safe); CHECK(safe[barrier]); /* writes s[2:9], not s10 */
    p->ops[6] = SH_OP_S_LOAD_DWORDX16;
    sh_barrier_convergence(p, shapes, safe); CHECK(!safe[barrier]);
    p->insts[6].enc = RDNA2_ENC_VOP3; p->insts[6].valu = 0x84; p->insts[6].dst = 10;
    sh_barrier_convergence(p, shapes, safe); CHECK(!safe[barrier]); /* e64 compare overwrites scalar */
    p->insts[6].enc = RDNA2_ENC_VOPC; p->insts[6].src[0] = RDNA2_OP_SDWA;
    p->insts[6].dpp_sdwa = (1u << 15) | (10u << 8);
    sh_barrier_convergence(p, shapes, safe); CHECK(!safe[barrier]);
    n = lds_broadcast_program(code);
    CHECK(sh_analyze(code, n, 0, p));
    sh_barrier_convergence(p, shapes, safe);
    unsigned barriers = 0, second = 0;
    for (unsigned i = 0; i < p->count; i++) if (p->ops[i] == SH_OP_S_BARRIER) {
        CHECK(safe[i]); barriers++; second = i;
    }
    CHECK(barriers == 2);
    unsigned addr = second - 5;
    CHECK(p->ops[addr] == SH_OP_V_MOV_B32);
    p->insts[addr].src[0] = RDNA2_OP_VGPR;
    sh_barrier_convergence(p, shapes, safe); CHECK(!safe[second]); /* lane-dependent LDS address */
    p->insts[addr].src[0] = RDNA2_OP_ZERO;
    p->iflags[second - 2] |= SH_IF_64;
    sh_barrier_convergence(p, shapes, safe); CHECK(!safe[second]); /* other half is not broadcast */
    p->iflags[second - 2] &= ~SH_IF_64;
    p->insts[0].enc = RDNA2_ENC_SOP1; p->insts[0].dst = RDNA2_OP_EXEC_HI;
    p->ops[0] = SH_OP_S_MOV_B32;
    sh_barrier_convergence(p, shapes, safe); CHECK(!safe[second]); /* partial EXEC */
    n = lds_suffix_program(code, true);
    CHECK(sh_analyze(code, n, 0, p));
    sh_barrier_convergence(p, shapes, safe);
    barrier = 0;
    while (barrier < p->count && p->ops[barrier] != SH_OP_S_BARRIER) barrier++;
    CHECK(barrier < p->count && safe[barrier]);
    CHECK(sh_translate(p, &info, shapes, 0, &spv, &words));
    controls = 0;
    for (size_t i = 5; i < words; i += spv[i] >> 16)
        controls += (spv[i] & 65535) == SpvOpControlBarrier;
    CHECK(controls == 2); /* workgroup boundary still precedes the later loop */
    free(spv);
    unsigned loop_branch = p->count - 2, back = p->target[loop_branch];
    p->target[loop_branch] = barrier;
    sh_barrier_convergence(p, shapes, safe); CHECK(!safe[barrier]);
    p->target[loop_branch] = back;
    branch = p->ops[2]; p->ops[2] = SH_OP_S_CBRANCH_VCCNZ;
    sh_barrier_convergence(p, shapes, safe); CHECK(!safe[barrier]);
    p->ops[2] = branch;
    /* A scalar derived from a lane is not uniform across guest waves. */
    p->ops[0] = SH_OP_V_READFIRSTLANE_B32; p->insts[0].enc = RDNA2_ENC_VOP1;
    sh_barrier_convergence(p, shapes, safe); CHECK(!safe[barrier]);
    n = lds_suffix_program(code, false);
    CHECK(sh_analyze(code, n, 0, p));
    p->iflags[p->count - 3] |= SH_IF_FLOW; /* unknown scalar transfer in the suffix */
    sh_barrier_convergence(p, shapes, safe); CHECK(!safe[barrier]);
    n = lds_dispatch_suffix_program(code, true);
    CHECK(sh_analyze(code, n, 0, p));
    for (unsigned mode = 0; mode < 2; mode++) {
        info.barrier_prefix = mode != 0;
        CHECK(sh_translate(p, &info, shapes, 0, &spv, &words));
        controls = 0;
        for (size_t i = 5; i < words; i += spv[i] >> 16)
            controls += (spv[i] & 65535) == SpvOpControlBarrier;
        if (controls != 1 + mode) fprintf(stderr, "prefix mode %u: %u controls expected %u\n", mode, controls, 1 + mode);
        CHECK(controls == 1 + mode);
        if (mode && getenv("PS5C_TEST_SPIRV")) {
            char path[512]; snprintf(path, sizeof(path), "%s/compute_lds_prefix_dispatch.spv", getenv("PS5C_TEST_SPIRV"));
            FILE *f = fopen(path, "wb");
            CHECK(f && fwrite(spv, 4, words, f) == words && !fclose(f));
        }
        free(spv);
    }
    for(unsigned partial=0;partial<2;partial++) {
        n=lds_vcc_program(code,partial!=0);CHECK(sh_analyze(code,n,0,p));
        info.barrier_prefix=false;
        for(unsigned wave_mode=1;wave_mode<=2;wave_mode++)for(unsigned enabled=0;enabled<2;enabled++) {
            p->wave32=wave_mode==2;info.cs_wave_mode=(uint8_t)wave_mode;info.wave_barriers=enabled!=0;
            CHECK(sh_translate(p,&info,shapes,0,&spv,&words));
            uint32_t*constants=calloc(spv[3],sizeof(*constants));CHECK(constants);unsigned workgroup=0;
            for(size_t i=5;i<words;i+=spv[i]>>16)
                if((spv[i]&65535)==SpvOpConstant && spv[i]>>16==4)constants[spv[i+2]]=spv[i+3];
            for(size_t i=5;i<words;i+=spv[i]>>16)
                if((spv[i]&65535)==SpvOpControlBarrier && constants[spv[i+1]]==2)workgroup++;
            CHECK(workgroup==(wave_mode==1?1u:0u)+(enabled?2u:0u));
            free(constants);free(spv);
        }
    }
    free(shapes); free(safe); free(p);
    puts("LDS convergence: scalar exit, in-phase divergence, later loop and unsafe paths verified");
    return 0;
}
static uint32_t program(uint32_t code[20], unsigned enable) {
    unsigned src[3] = {128, 128, 128}, n = 16; /* disabled dimensions are zero */
    for (unsigned k = 0; k < 3; k++)
        if ((enable >> k) & 1)
            src[k] = n++;
    n = 0;
    code[n++] = sop2(0x26, 19, src[1], 4); /* index = y * width + x + z * slice */
    code[n++] = sop2(0, 19, 19, src[0]);
    code[n++] = sop2(0x26, 20, src[2], 5);
    code[n++] = sop2(0, 19, 19, 20);
    for (unsigned k = 0; k < 3; k++)
        code[n++] = 0x7e000200 | (k << 17) | src[k];
    code[n++] = 0x7e060213; /* v3 = linear index */
    code[n++] = 0x7e080213; /* v4 = record index */
    code[n++] = 0xe0782000;
    code[n++] = 0x80000004; /* indexed store XYZW to s0:3 */
    code[n++] = 0xbf810000;
    return n;
}
static int large_read_test(renderer *r,gmem *m,gpu *g,guest_addr code_addr,guest_addr out) {
    guest_addr large;
    CHECK(gmem_map(m,code_addr+0x20000000,64u<<20,GPROT_RW,GMAP_FIXED,"large compute readonly fixture",&large)==PS5C_OK);
    uint32_t payload=0x13579bdf,guards[3]={0xdeadbeef,0,0xdeadbeef};
    CHECK(gmem_write(m,large+(48u<<20),&payload,4) && gmem_write(m,out,guards,sizeof(guards)));
    uint32_t code[]={0x7e0002ff,48u<<20,0xe0301000,0x80000100,0xbf8c3f70,
                          0xe0700000,0x80010100,0xbf810000};
    CHECK(gmem_write(m,code_addr,code,sizeof(code)));
    g->regs[mmCOMPUTE_PGM_LO]=(uint32_t)(code_addr>>8);g->regs[mmCOMPUTE_PGM_HI]=(uint32_t)(code_addr>>40);
    g->regs[mmCOMPUTE_PGM_RSRC2]=8u<<1;
    uint32_t *u=g->regs+mmCOMPUTE_USER_DATA_0;
    u[0]=(uint32_t)large;u[1]=(uint32_t)(large>>32);u[2]=64u<<20;u[3]=0x00016204;
    u[4]=(uint32_t)(out+4);u[5]=(uint32_t)((out+4)>>32);u[6]=4;u[7]=0x00016204;
    renderer_submit_begin(r);renderer_dispatch(r,g,1,1,1);renderer_sync_mem(r,out,sizeof(guards));
    CHECK(gmem_read(m,out,guards,sizeof(guards)));
    fprintf(stderr,"compute64MiB read48MiB: %08x expected %08x, guards %08x/%08x\n",guards[1],payload,guards[0],guards[2]);
    CHECK(guards[0]==0xdeadbeef && guards[1]==payload && guards[2]==0xdeadbeef);
    /* A writer in the input VMA must reject the readonly import and keep
     * the old bounded path. Otherwise CPU snapshots and the import diverge. */
    guest_addr alias=large+0x100;
    guards[0]=guards[2]=0xdeadbeef;guards[1]=0;
    CHECK(gmem_write(m,alias,guards,sizeof(guards)));
    u[4]=(uint32_t)(alias+4);u[5]=(uint32_t)((alias+4)>>32);
    renderer_submit_begin(r);renderer_dispatch(r,g,1,1,1);renderer_sync_mem(r,alias,sizeof(guards));
    CHECK(gmem_read(m,alias,guards,sizeof(guards)));
    CHECK(guards[0]==0xdeadbeef && guards[1]==0 && guards[2]==0xdeadbeef);
    fprintf(stderr,"compute import: writer VMA excluded, bounded fallback and guards preserved\n");
    if(getenv("PS5C_TEST_LARGE_WRITES")) {
        guest_addr large_out;
        CHECK(gmem_map(m,code_addr+0x30000000,128u<<20,GPROT_RW,GMAP_FIXED,"large compute output fixture",&large_out)==PS5C_OK);
        CHECK(vk_image_get(r,large_out+(96u<<20),1,1,VK_FORMAT_R32_UINT)); /* after the64MiB output */
        guards[0]=guards[2]=0xdeadbeef;guards[1]=0;
        CHECK(gmem_write(m,large_out+(48u<<20)-4,guards,sizeof(guards)));
        uint32_t alias_code[]={0x7e0002ff,48u<<20,0xe0301000,0x80000100,0xbf8c3f70,
            0xe0701000,0x80010100,0xbf8c3f70,
            0xe0300000,0x80020200,0xbf8c3f70, /* small readonly alias of the stored word */
            0xe0700000,0x80030200,0xbf810000};
        CHECK(gmem_write(m,code_addr,alias_code,sizeof(alias_code)));
        u[4]=(uint32_t)large_out;u[5]=(uint32_t)(large_out>>32);u[6]=64u<<20;
        u[8]=(uint32_t)(large_out+(48u<<20));u[9]=(uint32_t)((large_out+(48u<<20))>>32);u[10]=4;u[11]=0x00016204;
        u[12]=(uint32_t)(out+4);u[13]=(uint32_t)((out+4)>>32);u[14]=4;u[15]=0x00016204;
        uint32_t zero=0;CHECK(gmem_write(m,out+4,&zero,4));g->regs[mmCOMPUTE_PGM_RSRC2]=16u<<1;
        renderer_submit_begin(r);renderer_dispatch(r,g,1,1,1);renderer_sync_mem(r,large_out+(48u<<20)-4,sizeof(guards));
        CHECK(gmem_read(m,large_out+(48u<<20)-4,guards,sizeof(guards)));
        fprintf(stderr,"compute64MiB store48MiB: %08x expected %08x, guards %08x/%08x\n",guards[1],payload,guards[0],guards[2]);
        CHECK(guards[0]==0xdeadbeef && guards[1]==payload && guards[2]==0xdeadbeef);
        uint32_t alias_value=0;CHECK(gmem_read(m,out+4,&alias_value,4) && alias_value==payload);
        fprintf(stderr,"compute output: small alias read the same GPU write before CPU publication\n");
        CHECK(vk_image_get(r,large_out+(60u<<20),1,1,VK_FORMAT_R32_UINT)); /* overlapping: must refuse */
        zero=0;CHECK(gmem_write(m,large_out+(48u<<20),&zero,4));
        renderer_submit_begin(r);renderer_dispatch(r,g,1,1,1);renderer_sync_mem(r,large_out+(48u<<20),4);
        CHECK(gmem_read(m,large_out+(48u<<20),&zero,4) && zero==0);
        fprintf(stderr,"compute output: later private image allowed; overlapping image refused\n");
    }
    return 0;
}
static int join_prefix_gpu(renderer *r,gmem *m,gpu *g,guest_addr a,guest_addr out) {
    uint32_t code[64];uint32_t *data=gmem_host(m,out,200*4);CHECK(data);
    g->regs[mmCOMPUTE_PGM_LO]=(uint32_t)(a>>8);g->regs[mmCOMPUTE_PGM_HI]=(uint32_t)(a>>40);
    g->regs[mmCOMPUTE_NUM_THREAD_X]=64;g->regs[mmCOMPUTE_NUM_THREAD_Y]=g->regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    g->regs[mmCOMPUTE_PGM_RSRC2]=(8u<<1)|(1u<<7)|(1u<<15);
    g->regs[mmCOMPUTE_DISPATCH_INITIATOR]=1|(1u<<15);
    uint32_t *u=g->regs+mmCOMPUTE_USER_DATA_0;guest_addr dest=out+16;
    u[0]=(uint32_t)dest;u[1]=(uint32_t)(dest>>32)|(4u<<16);u[2]=192;u[3]=0x16204;
    for(unsigned pass=0;pass<4;pass++) {
        uint32_t low=pass&1?0x12345678:0x31415926,high=pass&1?0x87654321:0x27182818;
        unsigned n=join_prefix_program(code,low,high);CHECK(gmem_write(m,a,code,n*4));
        for(unsigned i=0;i<200;i++)data[i]=0xdeadbeef;
        renderer_submit_begin(r);renderer_dispatch(r,g,3,1,1);renderer_sync_mem(r,out,200*4);
        for(unsigned i=0;i<4;i++)CHECK(data[i]==0xdeadbeef && data[196+i]==0xdeadbeef);
        for(unsigned group=0;group<3;group++)for(unsigned lane=0;lane<64;lane++)
            CHECK(data[4+group*64+lane]==(lane<32?high:low));
    }
    puts("first LDS barrier GPU: 4 dispatches, 3 groups, two different wave32 arms, 768 cross-wave values and guards correct");return 0;
}
/* Synthetic, same-dispatch aliases. The read binding includes a legal load
 * tail beyond the store descriptor. A separate snapshot reads stale data;
 * an early reader must also share the allocation with the later writer. */
static int buffer_alias_gpu(renderer *r,gmem *m,gpu *g) {
    guest_addr a;CHECK(gmem_map(m,0,2*GUEST_PAGE_SIZE,GPROT_RW,0,"buffer alias proof",&a)==PS5C_OK);
    uint32_t *state=gmem_host(m,a+0x1000,64),*out=gmem_host(m,a+0x2000,16);CHECK(state && out);
    g->regs[mmCOMPUTE_PGM_RSRC2]=13u<<1;
    g->regs[mmCOMPUTE_NUM_THREAD_X]=g->regs[mmCOMPUTE_NUM_THREAD_Y]=g->regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    g->regs[mmCOMPUTE_DISPATCH_INITIATOR]=0x8041;
    for(unsigned pass=0;pass<8;pass++) {
        bool before=(pass&1)!=0;uint32_t records=(pass&2)?8:4;
        uint32_t old=0x13579000u+pass,payload=0x24681000u+pass;
        for(unsigned k=0;k<16;k++)state[k]=old+k;
        for(unsigned k=0;k<4;k++)out[k]=0xdeadbeef;
        uint32_t *u=g->regs+mmCOMPUTE_USER_DATA_0;memset(u,0,16*4);
        for(unsigned k=0;k<3;k++) {
            guest_addr base=a+(k==2?0x2000:0x1000);
            u[k*4]=(uint32_t)base;u[k*4+1]=(uint32_t)(base>>32)|((k==2?8u:4u)<<16);
            u[k*4+2]=k==0?records:k==1?4:1;u[k*4+3]=0x16204;
        }
        u[12]=payload;uint32_t code[16],n=0;code[n++]=0x7e00020c; /* v0=s12 */
        if(before){code[n++]=0xe0300000;code[n++]=0x80000100;} /* read v1, s[0:3] */
        code[n++]=0xe0700000;code[n++]=0x80010000; /* store v0, s[4:7] */
        if(!before){code[n++]=0xe0300000;code[n++]=0x80000100;}
        code[n++]=0xe0300000;code[n++]=0x80000200; /* read v2, s[0:3] */
        code[n++]=0xe0740000;code[n++]=0x80020100; /* store v[1:2], s[8:11] */
        code[n++]=0xbf810000;
        guest_addr pc=a+pass*0x100;CHECK(gmem_write(m,pc,code,n*4));
        g->regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);g->regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
        renderer_submit_begin(r);renderer_dispatch(r,g,1,1,1);renderer_sync_mem(r,a+0x1000,0x1010);
        if(out[0]!=(before?old:payload) || out[1]!=payload)
            fprintf(stderr,"alias pass %u: %08x/%08x expected %08x/%08x\n",pass,out[0],out[1],before?old:payload,payload);
        CHECK(out[0]==(before?old:payload) && out[1]==payload && state[0]==payload);
        CHECK(out[2]==0xdeadbeef && out[3]==0xdeadbeef);
        for(unsigned k=1;k<16;k++)CHECK(state[k]==old+k);
    }
    puts("buffer aliases GPU: early/late readers, wider descriptors, CPU changes and untouched tails verified");return 0;
}
static int gpu_test(void) {
    CHECK(pal_init() == PS5C_OK);
    gmem m;
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 4 * GUEST_PAGE_SIZE, GPROT_RW, 0, "compute code", &a) == PS5C_OK);
    pal_window *window = NULL;
    CHECK(pal_window_open("ZeraPS5 compute dispatch regression", 320, 180, &window) == PS5C_OK);
    renderer *r = renderer_create(window, &m);
    CHECK(r);
    uint32_t real_limit[3];
    memcpy(real_limit, r->props.limits.maxComputeWorkGroupCount, sizeof(real_limit));
    bool small = getenv("PS5C_TEST_COMPUTE_SMALL") != NULL;
    CHECK(small || real_limit[0] < UINT32_MAX - 3);
    uint32_t native = small ? 256 : real_limit[0] + 3;
    /* Bound allocations on hosts with an unusually large native limit. */
    CHECK(native <= 1048576);
    uint64_t bytes = ((uint64_t)native + 2) * 16;
    guest_addr out;
    CHECK(gmem_map(&m, 0, PS5C_ALIGN_UP(bytes, GUEST_PAGE_SIZE), GPROT_RW, 0, "compute output",
                   &out) == PS5C_OK);
    uint32_t *output = gmem_host(&m, out, bytes);
    CHECK(output);
    gpu g;
    CHECK(gpu_init(&g, &m, NULL) == PS5C_OK);
    if(getenv("PS5C_TEST_BUFFER_ALIAS")) {
        CHECK(buffer_alias_gpu(r,&m,&g)==0);goto gpu_done;
    }
    if(getenv("PS5C_TEST_JOIN_PREFIX")) {
        CHECK(join_prefix_gpu(r,&m,&g,a,out)==0);goto gpu_done;
    }
    g.regs[mmCOMPUTE_NUM_THREAD_X] = g.regs[mmCOMPUTE_NUM_THREAD_Y] =
        g.regs[mmCOMPUTE_NUM_THREAD_Z] = 1;
    if(getenv("PS5C_TEST_LARGE_COMPUTE")) {
        CHECK(large_read_test(r,&m,&g,a,out)==0);
        goto gpu_done;
    }
    struct {
        uint32_t total[3], limit[3];
        unsigned enable;
    } cases[] = {{{native, 1, 1}, {real_limit[0], real_limit[1], real_limit[2]}, 1},
                 {{7, 5, 4}, {3, 2, 2}, 7},
                 {{7, 1, 4}, {3, 2, 2}, 5},
                 {{1, 5, 1}, {3, 2, 2}, 2},
                 {{1, 1, 1}, {3, 2, 2}, 7}};
    for (unsigned c = getenv("PS5C_TEST_COMPUTE_SMALL") ? 1 : 0; c < PS5C_ARRAY_COUNT(cases); c++) {
        const uint32_t *total = cases[c].total;
        uint32_t records = total[0] * total[1] * total[2];
        memcpy(r->props.limits.maxComputeWorkGroupCount, cases[c].limit, sizeof(real_limit));
        g.regs[mmCOMPUTE_PGM_RSRC2] = (16u << 1) | (cases[c].enable << 7);
        uint32_t *u = g.regs + mmCOMPUTE_USER_DATA_0;
        u[0] = (uint32_t)out;
        u[1] = (uint32_t)(out >> 32) | (16u << 16);
        u[2] = records;
        u[3] = 0x00016204;
        u[4] = total[0];
        u[5] = total[0] * total[1];
        uint32_t code[20], n = program(code, cases[c].enable);
        guest_addr pc = a + c * 0x100;
        CHECK(gmem_write(&m, pc, code, n * 4));
        g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(pc >> 8);
        g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)(pc >> 40);
        for (unsigned i = 0; i < (records + 2) * 4; i++)
            output[i] = 0xdeadbeef;
        renderer_submit_begin(r);
        renderer_dispatch(r, &g, total[0], total[1], total[2]);
        renderer_sync_mem(r, out, (uint64_t)(records + 2) * 16);
        for (uint32_t z = 0; z < total[2]; z++)
            for (uint32_t y = 0; y < total[1]; y++)
                for (uint32_t x = 0; x < total[0]; x++) {
                    uint32_t i = x + total[0] * (y + total[1] * z), expected[4] = {x, y, z, i};
                    for (unsigned k = 0; k < 4; k++) {
                        if (output[i * 4 + k] != expected[k])
                            fprintf(stderr, "case %u record %u component %u: %08x expected %08x\n",
                                    c, i, k, output[i * 4 + k], expected[k]);
                        CHECK(output[i * 4 + k] == expected[k]);
                    }
                }
        for (unsigned i = records * 4; i < (records + 2) * 4; i++)
            CHECK(output[i] == 0xdeadbeef);
        printf("compute GPU case %u: %u,%u,%u groups, XYZ and tail verified\n", c, total[0],
               total[1], total[2]);
    }
    memcpy(r->props.limits.maxComputeWorkGroupCount, real_limit, sizeof(real_limit));
    {
        uint32_t code[64];
        uint32_t saved_dispatch = g.regs[mmCOMPUTE_DISPATCH_INITIATOR];
        if (getenv("PS5C_TEST_NATIVE32")) g.regs[mmCOMPUTE_DISPATCH_INITIATOR] = 0x8041;
        g.regs[mmCOMPUTE_NUM_THREAD_X] = 256;
        g.regs[mmCOMPUTE_PGM_RSRC2] = (8u << 1) | (1u << 7) | (2u << 15);
        uint32_t *u = g.regs + mmCOMPUTE_USER_DATA_0;
        u[0] = (uint32_t)out; u[1] = (uint32_t)(out >> 32) | (4u << 16);
        u[2] = 3 * 256; u[3] = 0x16204;
        unsigned passes = getenv("PS5C_TEST_WG_PC") ? 28 : getenv("PS5C_TEST_PREFIX") ? 20 : 16;
        for (unsigned pass = 0; pass < passes; pass++) {
            bool reuse = (pass & 2) != 0, broadcast = pass >= 8 && pass < 12, suffix = pass >= 12, dispatch = pass >= 16;
            unsigned n = pass>=20 ? lds_vcc_program(code,(pass>=22 && pass<24) || pass>=26) : dispatch ? lds_dispatch_suffix_program(code, reuse) : suffix ? lds_suffix_program(code, reuse) :
                         broadcast ? lds_broadcast_program(code) : lds_program(code, reuse);
            if(pass>=24) {
                /* Match Astro's 8x8, two-native-wave contract. Reconstruct
                 * linear tid for this fixture, not by assuming local X=tid. */
                g.regs[mmCOMPUTE_NUM_THREAD_X]=8;g.regs[mmCOMPUTE_NUM_THREAD_Y]=8;
                g.regs[mmCOMPUTE_PGM_RSRC2]|=1u<<11;
                for(unsigned k=0;k<n;k++) {
                    if(code[k]==0x2c0a0087)code[k]=0x2c0a0085;
                    if(k && code[k-1]==0x3a0400ff && code[k]==128)code[k]=32;
                }
                memmove(code+2,code,n*4);n+=2;
                code[0]=0x34020283;code[1]=0x4a000300;
            }
            guest_addr pc = a + 0x1000 + (pass>=24 ? 0x1000+(pass>=26?0x100:0) : 0) + (pass>=20 ? 0xc00+(pass>=22?0x100:0) : 0) + (broadcast ? 0x200 : reuse ? 0x100 : 0) + (dispatch ? 0x800 : suffix ? 0x400 : 0);
            CHECK(gmem_write(&m, pc, code, n * 4));
            g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(pc >> 8);
            g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)(pc >> 40);
            u[4] = pass & 1;
            for (unsigned i = 0; i < 3 * 256 + 2; i++) output[i] = 0xdeadbeef;
            renderer_submit_begin(r);
            renderer_dispatch(r, &g, 3, 1, 1);
            renderer_sync_mem(r, out, (3 * 256 + 2) * 4);
            for (unsigned i = 0; i < 3 * 256; i++) {
                unsigned expected = !u[4] ? 0xdeadbeef : broadcast ? 0 : ((i & 255) ^ (reuse ? 1 : 128));
                if(pass>=26)expected=getenv("PS5C_TEST_NATIVE32") && (i&255)<32?((i&255)^1):0xdeadbeef;
                else if(pass>=24)expected=u[4] && (i&255)<64?((i&255)^32):0xdeadbeef;
                else if(pass>=22)expected=(i&255)<128?((i&255)^1):0xdeadbeef;
                else if(pass>=20)expected=u[4]?((i&255)^128):0xdeadbeef;
                if (output[i] != expected)
                    fprintf(stderr, "LDS pass %u lane %u: %u expected %u\n", pass, i, output[i], expected);
                CHECK(output[i] == expected);
            }
            CHECK(output[3 * 256] == 0xdeadbeef && output[3 * 256 + 1] == 0xdeadbeef);
            g.stats.submits++;
        }
        printf("LDS GPU%s: %u dispatches, 3 groups, 256 lanes, cross-wave exchange, later loop, wave-local reuse and broadcast exit verified\n",
               getenv("PS5C_TEST_NATIVE32") ? " native32" : "", passes);
        g.regs[mmCOMPUTE_DISPATCH_INITIATOR] = saved_dispatch;
    }
    if (getenv("PS5C_TEST_WAVE64")) {
        /* Four independent waves in a 2D group. Read across native32 halves,
         * pick the first EXEC-active upper lane, and write with EXEC empty. */
        const uint32_t code[] = {
            0xd5430000, 144u | (257u<<9) | (256u<<18), /* v0 = 16*v1+v0 */
            0x7e080300, /* v4 = v0 */
            0xbefe0480, /* exec = 0 */
            0xd7610004, 6u | (4u<<9), /* writelane v4,s6,s4, ignores EXEC */
            0xd760000a, 256u | (4u<<9), /* readlane s10,v0,s4 */
            0xd760000c, 260u | (4u<<9), /* readlane s12,v4,s4 */
            0xd760000d, 260u | (5u<<9), /* readlane s13,v4,s5 */
            0xbeff03c1, /* exec_hi = -1, only upper half active */
            0x7e160500, /* readfirstlane s11,v0 */
            0xbf06a00b, 0xbf850002, /* first wave skips a cross-lane phase */
            0xd760000e, 256u | (4u<<9),
            0xd760000a, 256u | (4u<<9), /* all waves reconverge */
            0xbefe04c1, /* exec = -1 */
            0x7e08020a, 0x7e0a020b, 0x7e0c020c, 0x7e0e020d, /* v[4:7] = s[10:13] */
            0x7e040208, /* v2 = group X */
            0x34040488, /* v2 <<= 8 */
            0x4a040500, /* v2 += v0 */
            0xe0782000, 0x80000402, /* buffer_store_dwordx4 v[4:7],v2 */
            0x7d8400a0, /* v_cmp_eq_u32 vcc,32,v0: high-half bit in first wave */
            0x7e08026a,0x7e0a026b,0x7e0c0300,0x7e0e0208,
            0x4a0404ff,768, /* next output bank */
            0xe0782000,0x80000402,
            0xbf810000
        };
        guest_addr pc=a+0x3000, dest;
        CHECK(gmem_write(&m,pc,code,sizeof(code)));
        CHECK(gmem_map(&m,0,2*GUEST_PAGE_SIZE,GPROT_RW,0,"wave64 output",&dest)==PS5C_OK);
        uint32_t *data=gmem_host(&m,dest,3*256*32+8);CHECK(data);
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
        g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=16;
        g.regs[mmCOMPUTE_PGM_RSRC2]=(8u<<1)|(1u<<7)|(1u<<11);
        g.regs[mmCOMPUTE_DISPATCH_INITIATOR]=0x41;
        uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
        u[0]=(uint32_t)dest;u[1]=(uint32_t)(dest>>32)|(16u<<16);u[2]=2*3*256;u[3]=0x16204;
        u[4]=63;u[5]=1;u[6]=0x13572468;
        for(unsigned i=0;i<2*3*256*4+2;i++)data[i]=0xdeadbeef;
        renderer_submit_begin(r);renderer_dispatch(r,&g,3,1,1);renderer_sync_mem(r,dest,3*256*32+8);
        for(unsigned i=0;i<3*256;i++) {
            uint32_t base=(i&255u)&~63u,expected[4]={base+63,base+32,0x13572468,base+1};
            for(unsigned c=0;c<4;c++) {
                if(data[i*4+c]!=expected[c])fprintf(stderr,"wave64 lane %u component %u: %08x expected %08x\n",i,c,data[i*4+c],expected[c]);
                CHECK(data[i*4+c]==expected[c]);
            }
        }
        for(unsigned i=0;i<3*256;i++) {
            uint32_t expected[4]={0,(i&255u)<64?1:0,i&255u,i/256};
            for(unsigned c=0;c<4;c++) {
                uint32_t actual=data[(3*256+i)*4+c];
                if(actual!=expected[c])fprintf(stderr,"wave64 ballot lane %u component %u: %08x expected %08x\n",i,c,actual,expected[c]);
                CHECK(actual==expected[c]);
            }
        }
        CHECK(data[2*3*256*4]==0xdeadbeef && data[2*3*256*4+1]==0xdeadbeef);
        puts("compute wave64 GPU: 3 groups, four waves, cross-half reads, first active upper lane EXEC-independent writes, divergent wave phases and full high-half ballots verified");
        g.regs[mmCOMPUTE_NUM_THREAD_Y]=1;
    }
    if (getenv("PS5C_TEST_WAVE64_LDS")) {
        /* A single guest wave executes these LDS writes before its reads.
         * There is no S_BARRIER: two host subgroups must still preserve that
         * ordering. Separate groups must not exchange their scratch data. */
        const uint32_t code[]={
            0x34020082, /* v1 = tid * 4 */
            0x7e060208,0x34060688,0x4a060700, /* v3 = group * 256 + tid */
            0xd8340000,0x00000301, /* LDS[tid] = v3 */
            0x3a0400a0,0x34040482, /* v2 = (tid xor 32) * 4 */
            0xd8d80000,0x04000002,0xbf8cc07f, /* v4 = LDS[partner]; wait */
            0xe0702000,0x80000403,0xbf810000
        };
        guest_addr pc=a+0x3800,dest;
        CHECK(gmem_write(&m,pc,code,sizeof(code)));
        CHECK(gmem_map(&m,0,GUEST_PAGE_SIZE,GPROT_RW,0,"wave64 LDS output",&dest)==PS5C_OK);
        uint32_t *data=gmem_host(&m,dest,512*4);CHECK(data);
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
        g.regs[mmCOMPUTE_NUM_THREAD_X]=64;g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
        g.regs[mmCOMPUTE_PGM_RSRC2]=(8u<<1)|(1u<<7)|(1u<<15);
        g.regs[mmCOMPUTE_DISPATCH_INITIATOR]=0x41;
        uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
        u[0]=(uint32_t)dest;u[1]=(uint32_t)(dest>>32)|(4u<<16);u[2]=512;u[3]=0x16204;
        const char *selection=getenv("PS5C_TEST_WAVE64_FILE");uint64_t hash=0;
        if(selection) {
            sh_program *program=calloc(1,sizeof(*program));CHECK(program);
            CHECK(sh_analyze(code,PS5C_ARRAY_COUNT(code),pc,program));hash=program->hash;free(program);
        }
        for(unsigned pass=0;pass<8;pass++) {
            if(selection) {
                FILE *f=fopen(selection,"w");CHECK(f);
                CHECK(fprintf(f,"%llx\n",(unsigned long long)(pass%3==0?0:pass%3==1?hash:1))>0 && fclose(f)==0);
                g.stats.flips++; /* reload only at the next guest frame */
            }
            for(unsigned i=0;i<512;i++)data[i]=0xdeadbeef;
            renderer_submit_begin(r);renderer_dispatch(r,&g,2,1,1);renderer_sync_mem(r,dest,512*4);
            for(unsigned i=0;i<512;i++) {
                uint32_t expected=(i&255)<64?((i&255)^32)+(i&~255u):0xdeadbeef;
                if(data[i]!=expected)fprintf(stderr,"LDS wave64 pass %u lane %u: %08x expected %08x\n",pass,i,data[i],expected);
                CHECK(data[i]==expected);
            }
        }
        puts("wave64 LDS without S_BARRIER: 8 dispatches, 2 groups, both native32 halves and untouched gaps verified");
        if(selection)puts("wave64 file selection: disabled/hash/all, 8 frame changes and cached mode switches verified");
    }
    if (getenv("PS5C_TEST_NATIVE32")) {
        /* Eight guest waves of 32 in a 16x16 group: scalar lane operations
         * must exchange VGPRs, with full masks but no change to VCC_HI. */
        const uint32_t code[]={
            0xd5430000,144u|(257u<<9)|(256u<<18),
            0x7e080300,0xbefe0380, /* v4=tid; exec_lo=0 */
            0xd7610004,6u|(4u<<9),
            0xd760000a,256u|(4u<<9),
            0xd760000c,260u|(4u<<9),
            0xd760000d,260u|(5u<<9),
            0xbefe03ff,0xffff0000, /* only lanes16..31 active */
            0x7e160500,
            0xbf06900b,0xbf850002, /* wave0 skips a lane exchange */
            0xd760000e,256u|(4u<<9),
            0xd760000a,256u|(4u<<9),
            0xbefe03c1,
            0x7e08020a,0x7e0a020b,0x7e0c020c,0x7e0e020d,
            0x7e040208,0x34040488,0x4a040500,
            0xe0782000,0x80000402,
            0xbeeb03ff,0x24681357,
            0x7d840091, /* VCC = tid==17, full mask must be visible */
            0x7e08026a,0x7e0a026b,0x7e0c0300,0x7e0e0208,
            0x4a0404ff,768,0xe0782000,0x80000402,0xbf810000
        };
        guest_addr pc=a+0x4000,dest;
        CHECK(r->cs_subgroup==32 && gmem_write(&m,pc,code,sizeof(code)));
        CHECK(gmem_map(&m,0,2*GUEST_PAGE_SIZE,GPROT_RW,0,"native32 output",&dest)==PS5C_OK);
        uint32_t *data=gmem_host(&m,dest,3*256*32+8);CHECK(data);
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
        g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=16;
        g.regs[mmCOMPUTE_PGM_RSRC2]=(8u<<1)|(1u<<7)|(1u<<11);
        g.regs[mmCOMPUTE_DISPATCH_INITIATOR]=0x8041;
        uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
        u[0]=(uint32_t)dest;u[1]=(uint32_t)(dest>>32)|(16u<<16);u[2]=2*3*256;u[3]=0x16204;
        u[4]=31;u[5]=1;u[6]=0x13572468;
        for(unsigned i=0;i<2*3*256*4+2;i++)data[i]=0xdeadbeef;
        renderer_submit_begin(r);renderer_dispatch(r,&g,3,1,1);renderer_sync_mem(r,dest,3*256*32+8);
        for(unsigned i=0;i<3*256;i++) {
            uint32_t base=(i&255u)&~31u;
            uint32_t expected[8]={base+31,base+16,0x13572468,base+1,
                (i&255u)<32?1u<<17:0,0x24681357,i&255u,i/256};
            for(unsigned c=0;c<8;c++) {
                uint32_t actual=data[((c<4?0:3*256)+i)*4+(c&3)];
                if(actual!=expected[c])fprintf(stderr,"native32 lane %u component %u: %08x expected %08x\n",i,c,actual,expected[c]);
                CHECK(actual==expected[c]);
            }
        }
        CHECK(data[2*3*256*4]==0xdeadbeef && data[2*3*256*4+1]==0xdeadbeef);
        puts("native32 GPU: 3 groups, 8 waves, selected/first-active lane exchange, EXEC-independent write, full ballot and VCC_HI preserved");
        g.regs[mmCOMPUTE_NUM_THREAD_Y]=1;
    }
    if (getenv("PS5C_TEST_PACKET_WAVES")) {
        /* VCC_HI is a legal scalar temporary before the next wave64 compare.
         * The same instruction stream also works in wave32, where that high
         * half remains an independent scalar. Opcode inference cannot select
         * the mode; the dispatch packet and the pipeline key must distinguish it. */
        const uint32_t wave_code[] = {
            0xbeeb03ff, 0x13572468, /* s_mov_b32 vcc_hi, payload */
            0x7d840080,             /* v_cmp_eq_u32 vcc, 0, v0 */
            0x7e02026b,             /* v_mov_b32 v1, vcc_hi */
            0xe0702000, 0x80000100, /* buffer_store_dword v1, v0, s[0:3] */
            0xbf810000
        };
        guest_addr pc = a + 0x2000;
        CHECK(gmem_write(&m, pc, wave_code, sizeof(wave_code)));
        g.regs[mmCOMPUTE_PGM_LO] = (uint32_t)(pc >> 8);
        g.regs[mmCOMPUTE_PGM_HI] = (uint32_t)(pc >> 40);
        g.regs[mmCOMPUTE_NUM_THREAD_X] = 1;
        g.regs[mmCOMPUTE_PGM_RSRC2] = 4u << 1;
        uint32_t *u = g.regs + mmCOMPUTE_USER_DATA_0;
        u[0]=(uint32_t)out;u[1]=(uint32_t)(out>>32)|(4u<<16);u[2]=1;u[3]=0x16204;
        for(unsigned pass=0;pass<4;pass++) {
            bool wave32=pass&1u;
            g.regs[mmCOMPUTE_DISPATCH_INITIATOR]=0x41u|(wave32?0x8000u:0);
            output[0]=output[1]=0xdeadbeef;
            renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,out,8);
            if(output[0]!=(wave32?0x13572468u:0))fprintf(stderr,"packet wave%u result %08x expected %08x\n",wave32?32:64,output[0],wave32?0x13572468u:0);
            CHECK(output[0]==(wave32?0x13572468u:0) && output[1]==0xdeadbeef);
        }
        puts("compute packet wave mode: VCC_HI temporary, 32/64 comparisons and cached mode switches verified");
    }
gpu_done:
    renderer_destroy(r);
    free(g.regs);
    pal_mutex_destroy(g.lock);
    pal_window_destroy(window);
    gmem_free(&m);
    pal_shutdown();
    return 0;
}
int main(void) {
    CHECK(partition_test() == 0);
    CHECK(barrier_test() == 0);
    sh_program *p = calloc(1, sizeof(*p));
    CHECK(p);
    uint32_t code[20], n = program(code, 7), *spv = NULL;
    size_t words;
    sh_stage_info info = {.stage = SH_STAGE_CS,
                          .cs_tgid_sgpr = 16,
                          .cs_tgid_enable = 7,
                          .cs_tid_components = 1,
                          .local_size = {1, 1, 1}};
    sh_site_shape shape = {0};
    CHECK(sh_analyze(code, n, 0x100000, p));
    CHECK(sh_translate(p, &info, &shape, 0, &spv, &words));
    const char *dir = getenv("PS5C_TEST_SPIRV");
    if (dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/compute_grid.spv", dir);
        FILE *f = fopen(path, "wb");
        CHECK(f && fwrite(spv, 4, words, f) == words && fclose(f) == 0);
    }
    free(spv);
    free(p);
    if (getenv("PS5C_TEST_GPU"))
        CHECK(gpu_test() == 0);
    return 0;
}
