/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RDNA2 -> SPIR-V translation.
 *
 * Registers are function variables (one per SGPR/VGPR touched), SCC a bool.
 * Programs with branches run as a loop over a switch on the block index
 * ("pc"); straight-line programs are emitted as they are. See shader.h for
 * the execution and resource model. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "base/log.h"
#include "gpu/shader/shader.h"
#include "gpu/shader/shader_aperture.h"
#include "gpu/shader/shader_ops.h"
#include "gpu/shader/shader_barrier.h"
#include "gpu/shader/spv.h"

#define PC_END 0xFFFFFFFFu
#define PC_BARRIER 0xFFFFFFFEu
#define MAX_BLOCKS 1024

/* Render targets may be drawn smaller than the guest's size (PS5C_RT_SCALE, or
 * [geral] resolucao_interna for a game that can't set its own; sh_set_rt_scale) */
static bool g_rt_scale;
void sh_set_rt_scale(bool on) { g_rt_scale = on; }

uint32_t sh_translation_flags(sh_stage stage) {
    const char *value = getenv("PS5C_SHADER_EXACT");
    int exact = value ? atoi(value) : 2;
    uint32_t flags = exact >= 2 || (exact == 1 && stage != SH_STAGE_PS && stage != SH_STAGE_CS)
                         ? SH_TRANSLATE_EXACT : 0;
    if (getenv("PS5C_NGG_REAL_LANES")) flags |= SH_TRANSLATE_REAL_LANES;
    if (getenv("PS5C_NO_BARRIER")) flags |= SH_TRANSLATE_NO_BARRIER;
    if (getenv("PS5C_NO_STRUCTURED")) flags |= SH_TRANSLATE_NO_STRUCTURED;
    if (g_rt_scale) flags |= SH_TRANSLATE_RT_SCALE; /* its translations differ: their own cache keys */
    return flags;
}

bool sh_translation_wave64_lds(uint64_t hash) {
    const char *value = getenv("PS5C_WAVE64_LDS");
    return value && (!strcmp(value,"1") || strtoull(value,NULL,16)==hash);
}

bool sh_compute_auto_wave64(const sh_program *p, const sh_stage_info *info) {
    if(info->stage!=SH_STAGE_CS || info->cs_wave_mode==2 ||
       (uint64_t)info->local_size[0]*info->local_size[1]*info->local_size[2]!=64)return false;
    bool read=false,wide_exec=false,unsafe=false,lds=false;
    for(uint32_t i=0;i<p->count;i++) {
        const rdna2_inst *q=&p->insts[i];uint32_t op=p->ops[i];
        read |= op==SH_OP_V_READLANE_B32;
        wide_exec |= op==SH_OP_S_MOV_B64 && q->dst==RDNA2_OP_EXEC_LO;
        lds |= sh_inst_uses_lds(q);
        unsafe |= op==SH_OP_V_WRITELANE_B32 || op==SH_OP_S_WQM_B32 || op==SH_OP_S_WQM_B64 ||
            op==SH_OP_S_QUADMASK_B32 || op==SH_OP_S_QUADMASK_B64;
    }
    /* READLANE must remain wave-wide even when the wave also uses LDS. A
     * per-invocation fallback duplicates lane-0 work allocation and cannot
     * broadcast its result, leaving work-stealing loops divergent. Exactly
     * one guest wave makes workgroup rendezvous safe, including EXEC-off
     * invocations. LDS-only shaders retain the explicit diagnostic selection. */
    bool compaction = p->wave_compaction && !p->wave32;
    return (read || compaction || (sh_translation_wave64_lds(p->hash) && lds && !p->wave32)) && wide_exec && !unsafe;
}

static bool g_vs_layer; /* sh_set_vs_layer */
void sh_set_vs_layer(bool on) { g_vs_layer = on; }
static bool g_ps_layer; /* sh_set_ps_layer */
void sh_set_ps_layer(bool on) { g_ps_layer = on; }
static bool g_preserve_inf_nan;
void sh_set_preserve_inf_nan(bool on) { g_preserve_inf_nan = on; }
static bool g_buffer_atomic64;
void sh_set_buffer_atomic64(bool on) { g_buffer_atomic64 = on; }
static bool g_image_atomic64;
void sh_set_image_atomic64(bool on) { g_image_atomic64=on; }
bool sh_image_atomic64_supported(void) { return g_image_atomic64; }
static bool g_lds_atomic64;
void sh_set_lds_atomic64(bool on) { g_lds_atomic64 = on; }
static bool g_pixel_interlock;
void sh_set_pixel_interlock(bool on) { g_pixel_interlock = on; }

typedef struct tr {
    spv_builder b;
    const sh_program *p;
    const sh_stage_info *info;
    const sh_site_shape *shapes;
    uint32_t binding_base;
    uint32_t translation_flags;
    uint32_t uniform_shift; /* replay binds mesh records, not this packed uniforms ABI */
    bool ok;
    uint32_t inst; /* instruction being translated (for messages) */
    bool nan_trace; uint32_t nan_found;
    uint32_t probe_var, probe_pc, probe_vgpr; /* PS5C_PS_PROBE (debugging aid): up to 3, in R, G, B */
    /* SGPRs holding a whole wave's lane mask as a plain value (constants, s_bitreplicate), not
     * the per-lane bit the translation keeps masks as: written to EXEC, each lane takes its own
     * bit (translation order; compute shaders, whose lanes are known) */
    bool raw_mask[SH_SGPRS];
    bool lane_mask[SH_SGPRS]; /* comparison/EXEC represented by this invocation's bit */
    bool scalar_use[SH_SGPRS]; /* read as a number by some scalar instruction (not only as a mask) */
    bool scalar_use_ready;
    bool full_wave32; /* complete masks and native32 scalar lane exchange */
    bool mesh_ballot32;
    bool full_wave64; /* complete scalar masks and lane exchange for guest waves64 */
    bool ordered_pixel; /* native interlock serializes the whole fragment */
    uint32_t wave_scratch, wave_threads;
    bool wave_groups; /* independent guest waves64 on pairs of native32 subgroups */
    uint32_t wave_arrivals, wave_generation, in_subgroup;
    uint32_t wg_states, wg_waves, wg_shift;
    uint32_t mesh_counts, mesh_position, mesh_primitive, mesh_position_out, mesh_indices_out, mesh_cull_out;
    uint32_t mesh_attributes[32], mesh_aliases[32];
    uint32_t mesh_output,mesh_numgroups;
    uint32_t probe_vars[8], probe_pcs[8], probe_vgprs[8], probe_count;
    bool ps_capture; /* register sidecar: preserves every guest pixel output */
    uint32_t capture_x,capture_y,capture_frag,capture_reached,capture_require_pc;
    bool capture_require;
    bool vs_probe; /* PS5C_VS_PROBE: the probed values go to the end of the uniform buffer */
    bool ps_solid; /* PS5C_PS_SOLID: every pixel the draw covers becomes magenta */
    uint32_t ps_solid_index; /* its place in a PS5C_PS_SOLID list: the color */
    uint32_t vs_probe_vid;

    uint32_t t_void, t_bool, t_u32, t_i32, t_f32, t_u64, t_v2f, t_v2i, t_v3u, t_v4f, t_v4u, t_v4i;
    uint32_t p_fn_u32, p_fn_bool, p_sb_u32, p_sb_u64;
    uint32_t sgpr[SH_SGPRS], vgpr[256 + 16], scc, pc; /* 256..: temporaries (VTMP) */
    uint32_t vgpr_array; /* per-invocation indexed bank for shaders with many relative accesses */
    uint32_t movrel_tmp; /* the value a v_movrels read picks in the branch-based path */
    uint32_t lanes[256]; /* per-lane copies of a VGPR written by v_writelane (spilled SGPRs) */
    uint32_t t_ssbo, u_var;
    uint32_t site_var[SH_MAX_SITES], site_sampled_type[SH_MAX_SITES], site_image_type[SH_MAX_SITES];
    uint32_t site_u64[SH_MAX_SITES]; /* aliases the same storage-buffer binding */
    uint32_t in_vid, in_iid, in_lid, in_wgid, in_host_lid;
    uint32_t pv_mask,pv_attr[32],bary_p,bary_n;
    uint32_t pervertex, out_param[32], out_alias[32], in_attr[32], out_mrt[8];
    uint32_t clip_member; /* gl_PerVertex member of gl_ClipDistance (user clip planes), 0 if none */
    uint32_t iface[96], n_iface;
    uint32_t sdwa; /* SDWA control dword of the instruction being translated, 0 if none */
    uint32_t dpp;  /* DPP16 control of the instruction being translated, 0 if none: [8:0] dpp_ctrl,
                    * [18] fetch_inactive, [19] bound_ctrl, [23:20] source neg/abs, [31:24] bank and row masks */
    uint32_t dpp_keep; /* bool: this lane keeps its old destination (DPP source lane invalid or masked) */
    uint32_t dpp8;     /* eight 3-bit lane selectors, separate from DPP16 modifiers */
    bool dpp8_on, dpp8_fi;
    uint32_t in_sgid; /* SubgroupLocalInvocationId (quad lane of DPP quad_perm) */
    bool ps_wave_exec; /* diagnostic: test EXEC emptiness across the native fragment subgroup */
    bool ps_quad_flow; /* diagnostic: uniform scalar branches and whole-quad helper execution */
    uint32_t lds, p_wg_u32; /* local data share: Workgroup array of dwords (compute) */
    uint32_t scratch, p_pr_u32; /* scratch_* memory: a Private array of SCRATCH_DWORDS per invocation */
    uint32_t clock; /* s_memtime / s_memrealtime: reads so far, a Private counter */
    uint32_t lds_dwords;    /* its size */
    uint32_t lds64;        /* explicit-layout u64 block alias of the u32 LDS */
    /* outside compute: ds_write/read_addtid_b32 reach only the lane's own dwords
     * (register spills), a Function variable per instruction offset */
    uint32_t *spill; /* optional 16-bit byte-offset -> Function variable ID */
    uint32_t spill_count;
    bool lds_private; /* vertex shaders (NGG halves): each invocation keeps its own LDS */
    bool flat_apertures; /* opt-in generic FLAT DWORD accesses, compute only */
    uint32_t out_layer; /* gl_Layer (layered targets), 0 if none */
    uint32_t exported; /* PS: bool variable, a color export happened for this lane */
    uint32_t vm_valid; /* PS: sticky validity from EXP.vm, independent of earlier color writes */
    uint32_t out_depth; /* PS: FragDepth (mrtz export) */
    uint32_t covered; /* PS: this position is enabled by PA_SC_AA_MASK */
    bool barrier_exec;  /* s_barrier synchronizes execution (every lane walks the same blocks) */
    bool barrier_safe[SH_MAX_INSTS]; /* proven convergence in structured forward CS */
} tr;

static void fail(tr *t, const char *what) {
    if (t->ok) {
        const rdna2_inst *in = &t->p->insts[t->inst];
        LOG_E(GPU, "shader 0x%llx +0x%x: %s (%s)", (unsigned long long)t->p->base, t->p->pc[t->inst] * 4, what,
              in->name ? in->name : "?");
    }
    t->ok = false;
}

/* ---- Emission helpers ---- */

static uint32_t cu(tr *t, uint32_t v) { return spv_const_u32(&t->b, v); }
static uint32_t cf(tr *t, float v) { return spv_const_f32(&t->b, v); }
static uint32_t op1(tr *t, uint32_t op, uint32_t type, uint32_t a) { return spv_op(&t->b, op, type, &a, 1); }
static uint32_t op2(tr *t, uint32_t op, uint32_t type, uint32_t a, uint32_t b) {
    uint32_t x[2] = {a, b};
    return spv_op(&t->b, op, type, x, 2);
}
static uint32_t op3(tr *t, uint32_t op, uint32_t type, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t x[3] = {a, b, c};
    return spv_op(&t->b, op, type, x, 3);
}
static uint32_t ext1(tr *t, uint32_t type, uint32_t f, uint32_t a) { return spv_ext(&t->b, type, f, &a, 1); }
static uint32_t ext2(tr *t, uint32_t type, uint32_t f, uint32_t a, uint32_t b) {
    uint32_t x[2] = {a, b};
    return spv_ext(&t->b, type, f, x, 2);
}
static uint32_t ext3(tr *t, uint32_t type, uint32_t f, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t x[3] = {a, b, c};
    return spv_ext(&t->b, type, f, x, 3);
}
static uint32_t ld(tr *t, uint32_t type, uint32_t ptr) { return op1(t, SpvOpLoad, type, ptr); }
static uint32_t sb_load(tr *t, uint32_t var, uint32_t index);
static void st(tr *t, uint32_t ptr, uint32_t v) {
    uint32_t a[2] = {ptr, v};
    spv_op_void(&t->b, SpvOpStore, a, 2);
}
static uint32_t sel(tr *t, uint32_t type, uint32_t c, uint32_t a, uint32_t b) { return op3(t, SpvOpSelect, type, c, a, b); }
static uint32_t fbits(tr *t, uint32_t u) { return op1(t, SpvOpBitcast, t->t_f32, u); }
static uint32_t ubits(tr *t, uint32_t v) { return op1(t, SpvOpBitcast, t->t_u32, v); }
static uint32_t ibits(tr *t, uint32_t v) { return op1(t, SpvOpBitcast, t->t_i32, v); }
static uint32_t b2u(tr *t, uint32_t c) { return sel(t, t->t_u32, c, cu(t, 1), cu(t, 0)); }
static uint32_t label(tr *t) {
    uint32_t id = spv_id(&t->b);
    spv_label(&t->b, id);
    return id;
}
static void branch(tr *t, uint32_t target) { spv_op_void(&t->b, SpvOpBranch, &target, 1); }

/* if (cond) { ... } : returns the merge label to pass to end_if */
static uint32_t begin_if(tr *t, uint32_t cond) {
    uint32_t then_l = spv_id(&t->b), merge = spv_id(&t->b);
    uint32_t sm[2] = {merge, SpvSelectionControlNone};
    spv_op_void(&t->b, SpvOpSelectionMerge, sm, 2);
    uint32_t bc[3] = {cond, then_l, merge};
    spv_op_void(&t->b, SpvOpBranchConditional, bc, 3);
    spv_label(&t->b, then_l);
    return merge;
}
static void end_if(tr *t, uint32_t merge) {
    branch(t, merge);
    spv_label(&t->b, merge);
}

/* ---- Registers ---- */

static uint32_t sreg(tr *t, uint32_t n) {
    if (!t->sgpr[n]) t->sgpr[n] = spv_local_var(&t->b, t->p_fn_u32);
    return t->sgpr[n];
}
#define VTMP 512 /* register numbers of the translator's own temporaries (vgpr[256..]) */
static uint32_t vreg(tr *t, uint32_t n) {
    n = n >= VTMP && n < VTMP + 16 ? 256 + (n - VTMP) : n & 0xFF;
    if (!t->vgpr[n]) {
        if (t->vgpr_array && n < 256)
            t->vgpr[n] = spv_prologue_op(&t->b, SpvOpAccessChain, t->p_fn_u32,
                                         (uint32_t[]){t->vgpr_array, cu(t, n)}, 2);
        else t->vgpr[n] = spv_local_var(&t->b, t->p_fn_u32);
    }
    return t->vgpr[n];
}
static uint32_t get_s(tr *t, uint32_t n) { return ld(t, t->t_u32, sreg(t, n & 0x7F)); }
static void set_s(tr *t, uint32_t n, uint32_t v) {
    if (n < SH_SGPRS && n != RDNA2_OP_NULL) st(t, sreg(t, n), v);
}
static uint32_t get_v(tr *t, uint32_t n) { return ld(t, t->t_u32, vreg(t, n)); }
static uint32_t get_scc(tr *t) { return ld(t, t->t_bool, t->scc); }
static void set_scc(tr *t, uint32_t c) { st(t, t->scc, c); }
static uint32_t bit0(tr *t, uint32_t v) {
    return op2(t, SpvOpINotEqual, t->t_bool, op2(t, SpvOpBitwiseAnd, t->t_u32, v, cu(t, 1)), cu(t, 0));
}
static uint32_t wave_lane(tr *t);
static uint32_t local_flat(tr *t) {
    uint32_t u=t->t_u32;
    if(t->wave_groups || t->full_wave32 || t->mesh_ballot32)return op2(t,SpvOpIAdd,u,
        op2(t,SpvOpIMul,u,ld(t,u,t->in_subgroup),cu(t,32)),ld(t,u,t->in_sgid));
    uint32_t lid=ld(t,t->t_v3u,t->in_lid);
    uint32_t x=op2(t,SpvOpCompositeExtract,u,lid,0);
    if(t->info->stage==SH_STAGE_MESH)return x; /* preserve the one-dimensional mesh output index */
    uint32_t y=op2(t,SpvOpCompositeExtract,u,lid,1),z=op2(t,SpvOpCompositeExtract,u,lid,2);
    return op2(t,SpvOpIAdd,u,x,op2(t,SpvOpIAdd,u,
        op2(t,SpvOpIMul,u,y,cu(t,t->info->local_size[0])),
        op2(t,SpvOpIMul,u,z,cu(t,t->info->local_size[0]*t->info->local_size[1]))));
}
static uint32_t mask_lane(tr *t, uint32_t reg) {
    if(t->full_wave32) return bit0(t,op2(t,SpvOpShiftRightLogical,t->t_u32,get_s(t,reg),ld(t,t->t_u32,t->in_sgid)));
    if(!t->full_wave64) return bit0(t,get_s(t,reg));
    uint32_t lane=wave_lane(t),U=t->t_u32;
    uint32_t word=sel(t,U,op2(t,SpvOpULessThan,t->t_bool,lane,cu(t,32)),get_s(t,reg),get_s(t,reg+1));
    return bit0(t,op2(t,SpvOpShiftRightLogical,U,word,op2(t,SpvOpBitwiseAnd,U,lane,cu(t,31))));
}
static uint32_t active(tr *t) { return mask_lane(t,RDNA2_OP_EXEC_LO); }
static uint32_t mask_nonzero(tr *t,uint32_t reg) {
    uint32_t value=get_s(t,reg);
    if(t->full_wave64) value=op2(t,SpvOpBitwiseOr,t->t_u32,value,get_s(t,reg+1));
    return op2(t,SpvOpINotEqual,t->t_bool,value,cu(t,0));
}
static void set_v(tr *t, uint32_t n, uint32_t v) {
    uint32_t old = get_v(t, n);
    st(t, vreg(t, n), sel(t, t->t_u32, active(t), v, old));
}
/* The lane of this invocation in its wave (compute: from the flat local invocation index) */
static uint32_t wave_lane(tr *t) {
    uint32_t U = t->t_u32;
    if(t->wave_groups || t->mesh_ballot32)return op2(t,SpvOpBitwiseAnd,U,local_flat(t),cu(t,63));
    uint32_t lid = ld(t, t->t_v3u, t->in_lid), ls[3], sx = t->info->local_size[0], sy = t->info->local_size[1];
    for (uint32_t k = 0; k < 3; k++) ls[k] = spv_op(&t->b, SpvOpCompositeExtract, U, (uint32_t[]){lid, k}, 2);
    uint32_t flat = op2(t, SpvOpIAdd, U, ls[0], op2(t, SpvOpIAdd, U, op2(t, SpvOpIMul, U, ls[1], cu(t, sx)),
                                                 op2(t, SpvOpIMul, U, ls[2], cu(t, sx * sy))));
    return op2(t, SpvOpBitwiseAnd, U, flat, cu(t, t->p->wave32 && !t->full_wave64 ? 31 : 63));
}

/* All scalar control flow in full_wave64 uses replicated, complete SGPRs.
 * Consequently every invocation reaches these barriers, including EXEC-off
 * lanes. Scratch is separate from guest LDS and reused only after a barrier. */
static uint32_t wave_ptr(tr *t,uint32_t index) {
    if(t->wave_groups)index=op2(t,SpvOpIAdd,t->t_u32,index,
        op2(t,SpvOpIMul,t->t_u32,op2(t,SpvOpShiftRightLogical,t->t_u32,local_flat(t),cu(t,6)),cu(t,66)));
    return op2(t,SpvOpAccessChain,spv_type_pointer(&t->b,SpvStorageClassWorkgroup,t->t_u32),t->wave_scratch,index);
}
static void wave_barrier(tr *t) {
    if(!t->wave_groups) {
        spv_op_void(&t->b,SpvOpControlBarrier,(uint32_t[]){cu(t,2),cu(t,2),cu(t,0x108)},3);return;
    }
    /* A workgroup barrier would deadlock when one guest wave skips the
     * other wave's LDS phase. Rendezvous only its two native32 subgroups.
     * Every subgroup in this small workgroup is resident together. The
     * monotonically increasing count avoids reusing a barrier generation. */
    uint32_t u=t->t_u32;
    uint32_t wave=op2(t,SpvOpShiftRightLogical,u,local_flat(t),cu(t,6));
    uint32_t ptr=op2(t,SpvOpAccessChain,spv_type_pointer(&t->b,SpvStorageClassWorkgroup,u),t->wave_arrivals,wave);
    uint32_t generation=op2(t,SpvOpIAdd,u,ld(t,u,t->wave_generation),cu(t,1));
    st(t,t->wave_generation,generation);
    spv_op_void(&t->b,SpvOpControlBarrier,(uint32_t[]){cu(t,3),cu(t,2),cu(t,0x108)},3);
    uint32_t leader=begin_if(t,op2(t,SpvOpIEqual,t->t_bool,ld(t,u,t->in_sgid),cu(t,0)));
    spv_op(&t->b,SpvOpAtomicIAdd,u,(uint32_t[]){ptr,cu(t,2),cu(t,0x108),cu(t,1)},4);
    /* For paired compute waves, only the native subgroup's leader polls.
     * The following subgroup barrier publishes its acquire to all lanes. */
    if(!t->info->cs_wave_mode)end_if(t,leader);
    uint32_t expected=op2(t,SpvOpIMul,u,generation,cu(t,2));
    uint32_t head=spv_id(&t->b),body=spv_id(&t->b),cont=spv_id(&t->b),merge=spv_id(&t->b);
    branch(t,head);spv_label(&t->b,head);
    spv_op_void(&t->b,SpvOpLoopMerge,(uint32_t[]){merge,cont,0},3);branch(t,body);spv_label(&t->b,body);
    uint32_t arrived=spv_op(&t->b,SpvOpAtomicLoad,u,(uint32_t[]){ptr,cu(t,2),cu(t,0x102)},3);
    spv_op_void(&t->b,SpvOpBranchConditional,(uint32_t[]){op2(t,SpvOpUGreaterThanEqual,t->t_bool,arrived,expected),merge,cont},3);
    spv_label(&t->b,cont);branch(t,head);spv_label(&t->b,merge);
    if(t->info->cs_wave_mode)end_if(t,leader);
    spv_op_void(&t->b,SpvOpControlBarrier,(uint32_t[]){cu(t,3),cu(t,2),cu(t,0x108)},3);
}
static void wave_ballot(tr *t,uint32_t pred,uint32_t *lo,uint32_t *hi) {
    uint32_t lane=wave_lane(t),U=t->t_u32;
    if(t->info->cs_wave_mode || t->mesh_ballot32) {
        /* The renderer requires native32. Each subgroup already has one
         * complete half-mask; exchange two words rather than 64 atomics. */
        uint32_t ballot=spv_op(&t->b,SpvOpGroupNonUniformBallot,t->t_v4u,
            (uint32_t[]){cu(t,SpvScopeSubgroup),pred},2);
        uint32_t word=spv_op(&t->b,SpvOpCompositeExtract,U,(uint32_t[]){ballot,0},2);
        uint32_t half=op2(t,SpvOpShiftRightLogical,U,lane,cu(t,5));
        uint32_t leader=begin_if(t,op2(t,SpvOpIEqual,t->t_bool,ld(t,U,t->in_sgid),cu(t,0)));
        st(t,wave_ptr(t,op2(t,SpvOpIAdd,U,cu(t,64),half)),word);end_if(t,leader);
        wave_barrier(t);
        *lo=ld(t,U,wave_ptr(t,cu(t,64)));*hi=ld(t,U,wave_ptr(t,cu(t,65)));
        wave_barrier(t);return;
    }
    uint32_t zero=op2(t,SpvOpIEqual,t->t_bool,lane,cu(t,0)),body=spv_id(&t->b),merge=spv_id(&t->b);
    spv_op_void(&t->b,SpvOpSelectionMerge,(uint32_t[]){merge,0},2);
    spv_op_void(&t->b,SpvOpBranchConditional,(uint32_t[]){zero,body,merge},3);
    spv_label(&t->b,body);st(t,wave_ptr(t,cu(t,64)),cu(t,0));st(t,wave_ptr(t,cu(t,65)),cu(t,0));branch(t,merge);spv_label(&t->b,merge);
    wave_barrier(t);
    uint32_t word=op2(t,SpvOpShiftRightLogical,U,lane,cu(t,5));
    uint32_t bit=op2(t,SpvOpShiftLeftLogical,U,cu(t,1),op2(t,SpvOpBitwiseAnd,U,lane,cu(t,31)));
    uint32_t data=sel(t,U,pred,bit,cu(t,0));
    spv_op(&t->b,SpvOpAtomicOr,U,(uint32_t[]){wave_ptr(t,op2(t,SpvOpIAdd,U,word,cu(t,64))),cu(t,2),cu(t,0),data},4);
    wave_barrier(t);*lo=ld(t,U,wave_ptr(t,cu(t,64)));*hi=ld(t,U,wave_ptr(t,cu(t,65)));wave_barrier(t);
}
static uint32_t wave_readlane(tr *t,uint32_t value,uint32_t lane) {
    st(t,wave_ptr(t,wave_lane(t)),value);wave_barrier(t);
    uint32_t idx=op2(t,SpvOpBitwiseAnd,t->t_u32,lane,cu(t,63));
    uint32_t result=ld(t,t->t_u32,wave_ptr(t,idx));wave_barrier(t);
    return sel(t,t->t_u32,op2(t,SpvOpULessThan,t->t_bool,idx,cu(t,t->wave_threads)),result,cu(t,0));
}

/* Lane mask result (own bit) into an SGPR pair */
static void set_mask(tr *t, uint32_t sdst, uint32_t cond) {
    /* NULL discards the whole mask, including its wave64 high half. */
    if (sdst >= SH_SGPRS || sdst == RDNA2_OP_NULL) return;
    if(t->full_wave32) {
        uint32_t ballot=spv_op(&t->b,SpvOpGroupNonUniformBallot,t->t_v4u,
            (uint32_t[]){cu(t,SpvScopeSubgroup),cond},2);
        uint32_t lo=spv_op(&t->b,SpvOpCompositeExtract,t->t_u32,(uint32_t[]){ballot,0},2);
        set_s(t,sdst,lo);return; /* VCC_HI remains an independent scalar */
    }
    if(t->full_wave64) {
        uint32_t lo,hi;wave_ballot(t,cond,&lo,&hi);set_s(t,sdst,lo);set_s(t,sdst+1,hi);return;
    }
    set_s(t, sdst, b2u(t, cond));
    /* This write replaces a previous plain value with the invocation's own
     * mask bit. An old constant's raw_mask tag must not make a later EXEC
     * write extract that bit a second time (disabling every other lane). */
    t->raw_mask[sdst] = false;
    t->lane_mask[sdst] = true;
    if (!t->p->wave32 && sdst + 1 < SH_SGPRS) {
        set_s(t, sdst + 1, cu(t, 0));
        t->raw_mask[sdst + 1] = false;
        t->lane_mask[sdst + 1] = true;
    } /* wave32: the high half is a scalar temporary */
}

/* ---- Operands ---- */

static uint32_t src_u(tr *t, const rdna2_inst *in, uint32_t code) {
    uint32_t v;
    uint64_t aperture;
    if(sh_aperture_value(code,&aperture))return cu(t,(uint32_t)aperture);
    if (code >= RDNA2_OP_VGPR) return get_v(t, code - RDNA2_OP_VGPR);
    if (code < SH_SGPRS) return get_s(t, code);
    if (code == RDNA2_OP_SCC) return b2u(t, get_scc(t));
    if (code == RDNA2_OP_VCCZ) return b2u(t, op1(t, SpvOpLogicalNot, t->t_bool, mask_nonzero(t,RDNA2_OP_VCC_LO)));
    if (code == RDNA2_OP_EXECZ) return b2u(t, op1(t, SpvOpLogicalNot, t->t_bool, mask_nonzero(t,RDNA2_OP_EXEC_LO)));
    /* Each fragment is a virtual singleton ordered wave. Its system collision
     * SGPRs start at zero (no older overlapping wave), as does the exiting ID.
     * This is valid ONLY inside the native ordered pixel interlock, which
     * provides the exclusion/visibility that the guest's POPS polling requests. */
    if (code == 239 && t->ordered_pixel) return cu(t, 0);
    if (sh_inline_const(code, in->literal, &v)) return cu(t, v);
    fail(t, "operand not supported");
    return cu(t, 0);
}

static bool is_vop3(const rdna2_inst *in) { return in->enc == RDNA2_ENC_VOP3; }

static uint32_t bfe_u(tr *t, uint32_t a, uint32_t off, uint32_t w);
static uint32_t bfe_i(tr *t, uint32_t a, uint32_t off, uint32_t w);

/* SDWA (sub-dword addressing), one control dword after VOP1/VOP2/VOPC:
 *   [7:0] src0 register, [10:8] dst_sel, [12:11] dst_unused, [13] clamp,
 *   [15:14] omod (VOPC: [14:8] sdst, [15] sd), [18:16] src0_sel, [19] src0_sext,
 *   [20] src0_neg, [21] src0_abs, [23] src0 is an SGPR, [26:24] src1_sel,
 *   [27] src1_sext, [28] src1_neg, [29] src1_abs, [31] src1 is an SGPR.
 * Selects: 0..3 byte n, 4..5 word n, 6 dword. */
static uint32_t sdwa_select(tr *t, uint32_t v, uint32_t sel_code, bool sext) {
    if (sel_code >= 6) return v;
    uint32_t off = sel_code < 4 ? sel_code * 8 : (sel_code - 4) * 16, bits = sel_code < 4 ? 8 : 16;
    return sext ? bfe_i(t, v, cu(t, off), cu(t, bits)) : bfe_u(t, v, cu(t, off), cu(t, bits));
}

/* DPP16 quad_perm: lane i of each quad of four reads lane perm[i]. The host
 * subgroup quads match (2x2 pixels in fragment shaders), so this is a quad
 * broadcast of each of the four lanes and a pick by the own quad lane. */
static uint32_t quad_perm(tr *t, uint32_t v) {
    uint32_t U = t->t_u32, B = t->t_bool;
    uint32_t own = op2(t, SpvOpBitwiseAnd, U, ld(t, U, t->in_sgid), cu(t, 3));
    uint32_t idx = op2(t, SpvOpBitwiseAnd, U,
                       op2(t, SpvOpShiftRightLogical, U, cu(t, t->dpp & 0xFF), op2(t, SpvOpShiftLeftLogical, U, own, cu(t, 1))),
                       cu(t, 3));
    uint32_t lane[4];
    for (uint32_t k = 0; k < 4; k++)
        lane[k] = spv_op(&t->b, SpvOpGroupNonUniformQuadBroadcast, U, (uint32_t[]){cu(t, SpvScopeSubgroup), v, cu(t, k)}, 3);
    uint32_t r = lane[3];
    for (int k = 2; k >= 0; k--) r = sel(t, U, op2(t, SpvOpIEqual, B, idx, cu(t, (uint32_t)k)), lane[k], r);
    return r;
}

/* EXEC of lane `idx` of the wave */
static uint32_t lane_active(tr *t, uint32_t idx) {
    uint32_t U = t->t_u32;
    if (t->full_wave32)
        return bit0(t, op2(t, SpvOpShiftRightLogical, U, get_s(t, RDNA2_OP_EXEC_LO),
                            op2(t, SpvOpBitwiseAnd, U, idx, cu(t, 31))));
    if (!t->full_wave64)
        return bit0(t, spv_op(&t->b, SpvOpGroupNonUniformShuffle, U,
                              (uint32_t[]){cu(t, SpvScopeSubgroup), get_s(t, RDNA2_OP_EXEC_LO), idx}, 3));
    uint32_t word = sel(t, U, op2(t, SpvOpULessThan, t->t_bool, op2(t, SpvOpBitwiseAnd, U, idx, cu(t, 63)), cu(t, 32)),
                        get_s(t, RDNA2_OP_EXEC_LO), get_s(t, RDNA2_OP_EXEC_LO + 1));
    return bit0(t, op2(t, SpvOpShiftRightLogical, U, word, op2(t, SpvOpBitwiseAnd, U, idx, cu(t, 31))));
}

/* DPP16 row modes (rows of 16 lanes): shifts, rotates, mirrors, share and
 * xmask read another lane of the row through a subgroup shuffle. A source
 * lane outside the row, or inactive without fetch_inactive, reads 0 with
 * bound_ctrl, else the lane keeps its old destination; row_mask / bank_mask
 * disable lanes the same way. */
static bool dpp_supported(uint32_t c) {
    return c <= 0xFF || (c >= 0x101 && c <= 0x10F) || (c >= 0x111 && c <= 0x11F) || (c >= 0x121 && c <= 0x12F) ||
           c == 0x140 || c == 0x141 || (c >= 0x150 && c <= 0x16F);
}
static uint32_t dpp_src(tr *t, uint32_t v) {
    uint32_t U = t->t_u32, B = t->t_bool, c = t->dpp & 0x1FF;
    uint32_t lane = t->full_wave64?wave_lane(t):ld(t, U, t->in_sgid);
    uint32_t masks = t->dpp >> 24, keep = 0;
    if (masks != 0xFF) {
        uint32_t row = op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpShiftRightLogical, U, lane, cu(t, 4)), cu(t, 3));
        uint32_t bank = op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpShiftRightLogical, U, lane, cu(t, 2)), cu(t, 3));
        uint32_t ron = bit0(t, op2(t, SpvOpShiftRightLogical, U, cu(t, masks >> 4), row));
        uint32_t bon = bit0(t, op2(t, SpvOpShiftRightLogical, U, cu(t, masks & 0xF), bank));
        keep = op1(t, SpvOpLogicalNot, B, op2(t, SpvOpLogicalAnd, B, ron, bon));
    }
    uint32_t idx, r, valid = 0;
    if (c <= 0xFF) {
        uint32_t sh=op2(t,SpvOpIMul,U,op2(t,SpvOpBitwiseAnd,U,lane,cu(t,3)),cu(t,2));
        idx=op2(t,SpvOpBitwiseOr,U,op2(t,SpvOpBitwiseAnd,U,lane,cu(t,~3u)),
            op2(t,SpvOpBitwiseAnd,U,op2(t,SpvOpShiftRightLogical,U,cu(t,c),sh),cu(t,3)));
        r = t->full_wave64 ? wave_readlane(t,v,idx) : quad_perm(t, v);
    } else {
        uint32_t i = op2(t, SpvOpBitwiseAnd, U, lane, cu(t, 15)), j, n = c & 15;
        if (c >= 0x101 && c <= 0x10F) { /* row_shl */
            j = op2(t, SpvOpIAdd, U, i, cu(t, n));
            valid = op2(t, SpvOpULessThan, B, j, cu(t, 16));
        } else if (c >= 0x111 && c <= 0x11F) { /* row_shr */
            valid = op2(t, SpvOpUGreaterThanEqual, B, i, cu(t, n));
            j = op2(t, SpvOpISub, U, i, cu(t, n));
        } else if (c >= 0x121 && c <= 0x12F) { /* row_ror */
            j = op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpISub, U, op2(t, SpvOpIAdd, U, i, cu(t, 16)), cu(t, n)), cu(t, 15));
        } else if (c == 0x140) { /* row_mirror */
            j = op2(t, SpvOpISub, U, cu(t, 15), i);
        } else if (c == 0x141) { /* row_half_mirror */
            j = op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, i, cu(t, 8)), op2(t, SpvOpISub, U, cu(t, 7), op2(t, SpvOpBitwiseAnd, U, i, cu(t, 7))));
        } else if (c < 0x160) { /* row_share */
            j = cu(t, n);
        } else { /* row_xmask */
            j = op2(t, SpvOpBitwiseXor, U, i, cu(t, n));
        }
        j = op2(t, SpvOpBitwiseAnd, U, j, cu(t, 15));
        idx = op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, lane, cu(t, ~15u)), j);
        r = t->full_wave64?wave_readlane(t,v,idx):spv_op(&t->b, SpvOpGroupNonUniformShuffle, U, (uint32_t[]){cu(t, SpvScopeSubgroup), v, idx}, 3);
    }
    if (!((t->dpp >> 18) & 1)) { /* without fetch_inactive an inactive source lane is invalid too */
        /* A quad permutation must move EXEC through the same quad operation
         * as its value. Quad-only shaders do not require subgroup shuffles,
         * and fragment helpers participate in quad operations. */
        uint32_t on = c <= 0xFF && !t->full_wave64 && !t->full_wave32
                          ? bit0(t, quad_perm(t, get_s(t, RDNA2_OP_EXEC_LO)))
                          : lane_active(t, idx);
        valid = valid ? op2(t, SpvOpLogicalAnd, B, valid, on) : on;
    }
    if (valid) {
        r = sel(t, U, valid, r, cu(t, 0));
        if (!((t->dpp >> 19) & 1)) {
            uint32_t inv = op1(t, SpvOpLogicalNot, B, valid);
            keep = keep ? op2(t, SpvOpLogicalOr, B, keep, inv) : inv;
        }
    }
    t->dpp_keep = keep;
    return r;
}

/* DPP8 stays inside each group of eight lanes. fi:0 supplies zero from
 * an inactive source; fi:1 reads its retained register value. Select before
 * shuffling so the source lane's EXEC bit, rather than the reader's, applies. */
static uint32_t dpp8_src(tr *t, uint32_t v) {
    uint32_t U = t->t_u32, lane = t->full_wave64?wave_lane(t):ld(t, U, t->in_sgid);
    uint32_t shift = op2(t, SpvOpIMul, U, op2(t, SpvOpBitwiseAnd, U, lane, cu(t, 7)), cu(t, 3));
    uint32_t selected = op2(t, SpvOpBitwiseAnd, U,
                            op2(t, SpvOpShiftRightLogical, U, cu(t, t->dpp8), shift), cu(t, 7));
    uint32_t idx =
        op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, lane, cu(t, ~7u)), selected);
    if (!t->dpp8_fi)
        v = sel(t, U, active(t), v, cu(t, 0));
    if(t->full_wave64) return wave_readlane(t,v,idx);
    return spv_op(&t->b, SpvOpGroupNonUniformShuffle, U,
                  (uint32_t[]){cu(t, SpvScopeSubgroup), v, idx}, 3);
}

static uint32_t src(tr *t, const rdna2_inst *in, int k) {
    uint32_t v = src_u(t, in, in->src[k]);
    if (t->dpp && k == 0) v = dpp_src(t, v);
    if (t->dpp8_on && k == 0)
        v = dpp8_src(t, v);
    if (t->sdwa && k < 2) {
        uint32_t c = t->sdwa >> (k ? 24 : 16);
        v = sdwa_select(t, v, c & 7, (c >> 3) & 1);
    }
    return v;
}

static uint32_t src_f(tr *t, const rdna2_inst *in, int k) {
    uint32_t v = fbits(t, src(t, in, k));
    uint32_t abs = 0, neg = 0;
    if (is_vop3(in)) {
        abs = in->abs;
        neg = in->neg;
    } else if (t->sdwa) {
        abs = ((t->sdwa >> 21) & 1) | ((t->sdwa >> 28) & 2);
        neg = ((t->sdwa >> 20) & 1) | ((t->sdwa >> 27) & 2);
    } else if (t->dpp) { /* [20] src0_neg [21] src0_abs [22] src1_neg [23] src1_abs */
        abs = ((t->dpp >> 21) & 1) | ((t->dpp >> 22) & 2);
        neg = ((t->dpp >> 20) & 1) | ((t->dpp >> 21) & 2);
    }
    if ((abs >> k) & 1) v = ext1(t, t->t_f32, GLSLstd450FAbs, v);
    if ((neg >> k) & 1) v = op1(t, SpvOpFNegate, t->t_f32, v);
    return v;
}

static void set_vu(tr *t, const rdna2_inst *in, uint32_t u) {
    uint32_t n = in->dst - RDNA2_OP_VGPR, sel_code = (t->sdwa >> 8) & 7, unused = (t->sdwa >> 11) & 3;
    if (t->dpp_keep) u = sel(t, t->t_u32, t->dpp_keep, get_v(t, n), u);
    if (t->sdwa && sel_code < 6) {
        uint32_t U = t->t_u32;
        uint32_t off = sel_code < 4 ? sel_code * 8 : (sel_code - 4) * 16, bits = sel_code < 4 ? 8 : 16;
        uint32_t field = ((1u << bits) - 1) << off;
        uint32_t placed = op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpShiftLeftLogical, U, u, cu(t, off)), cu(t, field));
        if (unused == 2) { /* preserve the other bits */
            u = op2(t, SpvOpBitwiseOr, U, placed, op2(t, SpvOpBitwiseAnd, U, get_v(t, n), cu(t, ~field)));
        } else if (unused == 1 && off + bits < 32) { /* sign-extend above the field */
            u = op2(t, SpvOpShiftRightArithmetic, U, op2(t, SpvOpShiftLeftLogical, U, placed, cu(t, 32 - off - bits)),
                    cu(t, 32 - off - bits));
        } else {
            u = placed;
        }
    }
    set_v(t, n, u);
}

static uint32_t saturate_output(tr *t, uint32_t f) {
    /* FClamp has undefined NaN behavior. The GPU saturates NaN to zero. */
    uint32_t nonnegative = ext2(t, t->t_f32, GLSLstd450NMax, f, cf(t, 0.0f));
    return ext2(t, t->t_f32, GLSLstd450NMin, nonnegative, cf(t, 1.0f));
}

static void set_vf(tr *t, const rdna2_inst *in, uint32_t f) {
    uint32_t omod = 0;
    bool clamp = false;
    if (is_vop3(in)) {
        omod = in->omod;
        clamp = in->clamp;
    } else if (t->sdwa && in->enc != RDNA2_ENC_VOPC) {
        omod = (t->sdwa >> 14) & 3;
        clamp = (t->sdwa >> 13) & 1;
    }
    if (omod == 1) f = op2(t, SpvOpFMul, t->t_f32, f, cf(t, 2.0f));
    else if (omod == 2) f = op2(t, SpvOpFMul, t->t_f32, f, cf(t, 4.0f));
    else if (omod == 3) f = op2(t, SpvOpFMul, t->t_f32, f, cf(t, 0.5f));
    if (clamp) f = saturate_output(t, f);
    if(t->nan_trace) {
        uint32_t bad=op2(t,SpvOpLogicalAnd,t->t_bool,op2(t,SpvOpLogicalOr,t->t_bool,op1(t,SpvOpIsNan,t->t_bool,f),op1(t,SpvOpIsInf,t->t_bool,f)),active(t));
        bad=op2(t,SpvOpLogicalAnd,t->t_bool,bad,op1(t,SpvOpLogicalNot,t->t_bool,ld(t,t->t_bool,t->nan_found)));
        uint32_t done=begin_if(t,bad);
        st(t,t->probe_vars[0],ubits(t,cf(t,(float)(t->p->pc[t->inst]*4))));
        st(t,t->probe_vars[1],ubits(t,src_f(t,in,0)));
        st(t,t->probe_vars[2],in->enc==RDNA2_ENC_VOP1?cu(t,0):ubits(t,src_f(t,in,1)));
        st(t,t->nan_found,spv_const_bool(&t->b,true));end_if(t,done);
    }
    set_vu(t, in, ubits(t, f));
}

/* f32 -> f16 rounding toward zero (v_cvt_pkrtz): PackHalf2x16 rounds to nearest, so a result
 * larger in magnitude than the input steps back one unit; finite values past the f16 range
 * become the largest finite one (65504), not infinity (Unreal's HDR scene colors reach it). */
static void trace_packed_nonfinite(tr*t,uint32_t f,uint32_t offset) {
    if(!t->nan_trace)return;
    uint32_t bad=op2(t,SpvOpLogicalOr,t->t_bool,op1(t,SpvOpIsNan,t->t_bool,f),op1(t,SpvOpIsInf,t->t_bool,f));
    bad=op2(t,SpvOpLogicalAnd,t->t_bool,bad,active(t));
    bad=op2(t,SpvOpLogicalAnd,t->t_bool,bad,op1(t,SpvOpLogicalNot,t->t_bool,ld(t,t->t_bool,t->nan_found)));
    uint32_t join=begin_if(t,bad);
    st(t,t->probe_vars[0],ubits(t,cf(t,(float)(t->p->pc[t->inst]*4+offset))));
    st(t,t->probe_vars[1],ubits(t,f));st(t,t->probe_vars[2],cu(t,0));
    st(t,t->nan_found,spv_const_bool(&t->b,true));end_if(t,join);
}
static uint32_t half_rtz(tr *t, uint32_t f) {
    trace_packed_nonfinite(t,f,16384);
    uint32_t U = t->t_u32, F = t->t_f32, B = t->t_bool;
    uint32_t h = op2(t, SpvOpBitwiseAnd, U,
                     ext1(t, U, GLSLstd450PackHalf2x16, op2(t, SpvOpCompositeConstruct, t->t_v2f, f, cf(t, 0.0f))), cu(t, 0xFFFF));
    uint32_t back = op2(t, SpvOpCompositeExtract, F, ext1(t, t->t_v2f, GLSLstd450UnpackHalf2x16, h), 0);
    uint32_t up = op2(t, SpvOpFOrdGreaterThan, B, ext1(t, F, GLSLstd450FAbs, back), ext1(t, F, GLSLstd450FAbs, f));
    return sel(t, U, up, op2(t, SpvOpISub, U, h, cu(t, 1)), h);
}

/* 64-bit scalar values (Int64) */
static uint32_t src64(tr *t, const rdna2_inst *in, uint32_t code) {
    uint64_t aperture;
    uint32_t lo, hi;
    if(sh_aperture_value(code,&aperture)) {
        lo=cu(t,(uint32_t)aperture);hi=cu(t,(uint32_t)(aperture>>32));
    } else if (code < SH_SGPRS - 1) {
        lo = get_s(t, code);
        hi = get_s(t, code + 1);
    } else {
        uint32_t v = 0;
        if (!sh_inline_const(code, in->literal, &v)) fail(t, "64-bit operand");
        lo = cu(t, v);
        hi = cu(t, code >= 193 && code <= 208 ? 0xFFFFFFFFu : 0);
    }
    uint32_t l = op1(t, SpvOpUConvert, t->t_u64, lo), h = op1(t, SpvOpUConvert, t->t_u64, hi);
    return op2(t, SpvOpBitwiseOr, t->t_u64, l, op2(t, SpvOpShiftLeftLogical, t->t_u64, h, cu(t, 32)));
}
static void set_s64(tr *t, uint32_t code, uint32_t v) {
    set_s(t, code, op1(t, SpvOpUConvert, t->t_u32, v));
    set_s(t, code + 1, op1(t, SpvOpUConvert, t->t_u32, op2(t, SpvOpShiftRightLogical, t->t_u64, v, cu(t, 32))));
}
static uint32_t nz64(tr *t, uint32_t v) {
    uint32_t zero = op1(t, SpvOpUConvert, t->t_u64, cu(t, 0));
    return op2(t, SpvOpINotEqual, t->t_bool, v, zero);
}

/* ---- Scalar ALU ---- */

static uint32_t mask_bits(tr *t, uint32_t w) {
    /* (1 << w) - 1 for w in 0..32 */
    uint32_t full = op2(t, SpvOpUGreaterThanEqual, t->t_bool, w, cu(t, 32));
    uint32_t m = op2(t, SpvOpISub, t->t_u32, op2(t, SpvOpShiftLeftLogical, t->t_u32, cu(t, 1), op2(t, SpvOpBitwiseAnd, t->t_u32, w, cu(t, 31))), cu(t, 1));
    return sel(t, t->t_u32, full, cu(t, 0xFFFFFFFFu), m);
}

static uint32_t bfe_u(tr *t, uint32_t a, uint32_t off, uint32_t w) {
    uint32_t sh = op2(t, SpvOpShiftRightLogical, t->t_u32, a, op2(t, SpvOpBitwiseAnd, t->t_u32, off, cu(t, 31)));
    return op2(t, SpvOpBitwiseAnd, t->t_u32, sh, mask_bits(t, w));
}

static uint32_t bfe_i(tr *t, uint32_t a, uint32_t off, uint32_t w) {
    off = op2(t, SpvOpBitwiseAnd, t->t_u32, off, cu(t, 31));
    w = ext2(t, t->t_u32, GLSLstd450UMin, w, op2(t, SpvOpISub, t->t_u32, cu(t, 32), off));
    uint32_t r = op3(t, SpvOpBitFieldSExtract, t->t_i32, ibits(t, a), off, w);
    return ubits(t, r);
}

/* The lane's own bit of a whole-wave mask (lo, hi) */
static uint32_t own_lane_bit(tr *t, uint32_t lo, uint32_t hi) {
    uint32_t U = t->t_u32, lane = wave_lane(t);
    uint32_t word = sel(t, U, op2(t, SpvOpULessThan, t->t_bool, lane, cu(t, 32)), lo, hi);
    return op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpShiftRightLogical, U, word, op2(t, SpvOpBitwiseAnd, U, lane, cu(t, 31))), cu(t, 1));
}
static uint32_t vector_mask(tr *t,const rdna2_inst *in,uint32_t code) {
    if(code==RDNA2_OP_NULL) return spv_const_bool(&t->b,false);
    uint32_t lo=src_u(t,in,code);
    if(t->full_wave32)return bit0(t,own_lane_bit(t,lo,cu(t,0)));
    if(!t->full_wave64) return bit0(t,lo);
    uint32_t hi=code<SH_SGPRS-1?get_s(t,code+1):cu(t,code>=193 && code<=208?UINT32_MAX:0);
    return bit0(t,own_lane_bit(t,lo,hi));
}

static uint32_t c64(tr *t, uint64_t v);

/* Whether a scalar instruction reads this SGPR as a number (arithmetic, bit fields,
 * comparisons, s_bitset's destination) rather than only as a lane mask in logic ops. */
static bool salu_logic_name(const char *name) {
    static const char *const k[] = {"s_mov_", "s_and_", "s_or_", "s_xor_", "s_andn2_", "s_orn2_", "s_nand_",
                                    "s_nor_", "s_xnor_", "s_not_", "s_cselect_", "s_andn1_", "s_orn1_"};
    for (size_t i = 0; name && i < sizeof(k) / sizeof(k[0]); i++)
        if (!strncmp(name, k[i], strlen(k[i]))) return true;
    return false;
}

static bool sgpr_scalar_use(tr *t, uint32_t code) {
    if (!t->scalar_use_ready) {
        t->scalar_use_ready = true;
        for (uint32_t i = 0; i < t->p->count; i++) {
            const rdna2_inst *q = &t->p->insts[i];
            bool sop = q->enc == RDNA2_ENC_SOP1 || q->enc == RDNA2_ENC_SOP2 || q->enc == RDNA2_ENC_SOPC ||
                       q->enc == RDNA2_ENC_SOPK;
            if (!sop || (q->enc != RDNA2_ENC_SOPC && salu_logic_name(q->name))) continue;
            uint32_t regs[3] = {q->src[0], q->enc == RDNA2_ENC_SOP1 ? q->dst : q->src[1],
                                q->enc == RDNA2_ENC_SOPK ? q->dst : RDNA2_OP_NULL};
            for (int k = 0; k < 3; k++)
                if (regs[k] < SH_SGPRS) {
                    t->scalar_use[regs[k]] = true;
                    if (regs[k] + 1 < SH_SGPRS) t->scalar_use[regs[k] + 1] = true;
                }
        }
    }
    return code < SH_SGPRS && (t->scalar_use[code] || (code + 1 < SH_SGPRS && t->scalar_use[code + 1]));
}

static void salu(tr *t, const rdna2_inst *in, uint32_t op) {
    uint32_t U = t->t_u32, B = t->t_bool, L = t->t_u64;
    uint32_t d = in->dst;
    uint32_t a = 0, b = 0;
    rdna2_inst conv; /* an EXEC write whose operands are plain masks (raw_mask): their own bits instead */
    static const char *const k_bitwise[] = {"s_mov_", "s_and_", "s_or_", "s_xor_", "s_andn2_", "s_orn2_", "s_nand_",
                                            "s_nor_", "s_xnor_", "s_not_", "s_cselect_", "s_andn1_", "s_orn1_"};
    bool bitwise = false; /* operands that are masks (not s_bitreplicate's or s_bfm's parameters) */
    for (size_t k = 0; in->name && k < sizeof(k_bitwise) / sizeof(k_bitwise[0]) && !bitwise; k++)
        bitwise = !strncmp(in->name, k_bitwise[k], strlen(k_bitwise[k]));
    bool lane_result = false;
    if (bitwise && (in->enc == RDNA2_ENC_SOP1 || in->enc == RDNA2_ENC_SOP2)) {
        for (int k = 0; k < (in->enc == RDNA2_ENC_SOP2 ? 2 : 1); k++) {
            uint32_t code = in->src[k];
            lane_result |= code == RDNA2_OP_EXEC_LO || (code < SH_SGPRS && t->lane_mask[code]);
        }
        lane_result |= d == RDNA2_OP_EXEC_LO || strstr(in->name, "saveexec") != NULL;
    }
    /* A scalar mask combined with an invocation's comparison bit must first
     * select that invocation's bit, even if the destination is VCC or an SGPR.
     * Deferring this until EXEC loses the mask at intermediate AND/OR writes. */
    if (bitwise && lane_result && !t->full_wave64 && !t->full_wave32 && t->info->stage == SH_STAGE_CS && t->in_lid &&
        (in->enc == RDNA2_ENC_SOP1 || in->enc == RDNA2_ENC_SOP2)) {
        conv = *in;
        bool changed = false;
        for (int k = 0; k < (in->enc == RDNA2_ENC_SOP2 ? 2 : 1); k++) {
            uint32_t code = in->src[k], lo = 0, hi = 0, v;
            if (code < SH_SGPRS - 1 && code != RDNA2_OP_EXEC_LO && t->raw_mask[code]) {
                lo = get_s(t, code), hi = get_s(t, code + 1);
            } else if (code >= 128 && code != RDNA2_OP_NULL && code != RDNA2_OP_SCC && sh_inline_const(code, in->literal, &v)) {
                uint32_t vhi = code >= 193 && code <= 208 ? 0xFFFFFFFFu : 0;
                if ((v == 0 && vhi == 0) || (v == 0xFFFFFFFFu && vhi == 0xFFFFFFFFu)) continue; /* the same in both forms */
                lo = cu(t, v), hi = cu(t, vhi);
            } else {
                continue;
            }
            uint32_t tmp = 120 + 2 * (uint32_t)k; /* TTMP12..15 as temporaries */
            set_s(t, tmp, own_lane_bit(t, lo, hi));
            set_s(t, tmp + 1, cu(t, 0));
            conv.src[k] = (uint16_t)tmp;
            changed = true;
        }
        if (changed) in = &conv;
    }
    /* An inverting logic op on lane masks (each invocation's own bit 0) must keep
     * only that bit: ~1 is 0xfffffffe, a set mask that never empties a loop's
     * remaining-lanes copy (Longshot's frame generation search, s_nand_b64 /
     * s_and_saveexec / s_andn2_b64 exec, ran to the loop limit). */
    bool lane_bit = lane_result && !t->full_wave64 && !t->full_wave32;
#define LB32(r) (lane_bit ? op2(t, SpvOpBitwiseAnd, U, (r), cu(t, 1)) : (r))
#define LB64(r) (lane_bit ? op2(t, SpvOpBitwiseAnd, L, (r), c64(t, 1)) : (r))
    if (in->enc == RDNA2_ENC_SOPK) {
        a = get_s(t, d);
        bool zext = op == SH_OP_S_CMPK_EQ_U32 || op == SH_OP_S_CMPK_LG_U32 || op == SH_OP_S_CMPK_GT_U32 ||
                    op == SH_OP_S_CMPK_GE_U32 || op == SH_OP_S_CMPK_LT_U32 || op == SH_OP_S_CMPK_LE_U32;
        b = cu(t, zext ? (uint32_t)in->simm16 & 0xFFFF : (uint32_t)in->simm16);
    } else if (op != SH_OP_S_GETPC_B64) {
        a = src(t, in, 0);
        if (in->enc != RDNA2_ENC_SOP1) b = src(t, in, 1);
    }
#define A32() a
#define B32() b
#define A64() src64(t, in, in->src[0])
#define B64() src64(t, in, in->src[1])
#define LOGIC32(OPC) { uint32_t r = op2(t, OPC, U, A32(), B32()); set_s(t, d, r); set_scc(t, op2(t, SpvOpINotEqual, B, r, cu(t, 0))); break; }
#define LOGIC64(OPC) { uint32_t r = op2(t, OPC, L, A64(), B64()); set_s64(t, d, r); set_scc(t, nz64(t, r)); break; }
#define CMP32(OPC) set_scc(t, op2(t, OPC, B, A32(), B32())); break;
#define ICMP32(OPC) set_scc(t, op2(t, OPC, B, ibits(t, A32()), ibits(t, B32()))); break;
    switch (op) {
    case SH_OP_S_MOV_B32: set_s(t, d, A32()); break;
    case SH_OP_S_MOV_B64: set_s64(t, d, A64()); break;
    case SH_OP_S_CMOV_B32: set_s(t, d, sel(t, U, get_scc(t), A32(), get_s(t, d))); break;
    case SH_OP_S_MOVK_I32: set_s(t, d, cu(t, (uint32_t)in->simm16)); break;
    case SH_OP_S_NOT_B32: { uint32_t r = LB32(op1(t, SpvOpNot, U, A32())); set_s(t, d, r); set_scc(t, op2(t, SpvOpINotEqual, B, r, cu(t, 0))); break; }
    case SH_OP_S_NOT_B64: { uint32_t r = LB64(op1(t, SpvOpNot, L, A64())); set_s64(t, d, r); set_scc(t, nz64(t, r)); break; }
    case SH_OP_S_BREV_B32: set_s(t, d, op1(t, SpvOpBitReverse, U, A32())); break;
    case SH_OP_S_BCNT1_I32_B32: { uint32_t r = ubits(t, op1(t, SpvOpBitCount, t->t_i32, A32())); set_s(t, d, r); set_scc(t, op2(t, SpvOpINotEqual, B, r, cu(t, 0))); break; }
    case SH_OP_S_FF1_I32_B32: set_s(t, d, ubits(t, ext1(t, t->t_i32, GLSLstd450FindILsb, A32()))); break;
    case SH_OP_S_FF1_I32_B64: { /* lowest set bit of the pair, -1 if none */
        uint32_t lo = get_s(t, in->src[0]), hi = src_u(t, in, in->src[0] + 1);
        uint32_t flo = ubits(t, ext1(t, t->t_i32, GLSLstd450FindILsb, lo)), fhi = ubits(t, ext1(t, t->t_i32, GLSLstd450FindILsb, hi));
        uint32_t h = sel(t, U, op2(t, SpvOpIEqual, B, hi, cu(t, 0)), cu(t, ~0u), op2(t, SpvOpIAdd, U, fhi, cu(t, 32)));
        set_s(t, d, sel(t, U, op2(t, SpvOpIEqual, B, lo, cu(t, 0)), h, flo));
        break;
    }
    case SH_OP_S_FLBIT_I32_B32: {
        uint32_t m = ubits(t, ext1(t, t->t_i32, GLSLstd450FindUMsb, A32()));
        set_s(t, d, sel(t, U, op2(t, SpvOpIEqual, B, A32(), cu(t, 0)), cu(t, 0xFFFFFFFFu), op2(t, SpvOpISub, U, cu(t, 31), m)));
        break;
    }
    case SH_OP_S_FLBIT_I32_B64: { /* leading zeros of the pair, -1 if none */
        uint32_t lo = get_s(t, in->src[0]), hi = src_u(t, in, in->src[0] + 1);
        uint32_t mlo = ubits(t, ext1(t, t->t_i32, GLSLstd450FindUMsb, lo)), mhi = ubits(t, ext1(t, t->t_i32, GLSLstd450FindUMsb, hi));
        uint32_t l = sel(t, U, op2(t, SpvOpIEqual, B, lo, cu(t, 0)), cu(t, 0xFFFFFFFFu), op2(t, SpvOpISub, U, cu(t, 63), mlo));
        set_s(t, d, sel(t, U, op2(t, SpvOpIEqual, B, hi, cu(t, 0)), l, op2(t, SpvOpISub, U, cu(t, 31), mhi)));
        break;
    }
    case SH_OP_S_BITREPLICATE_B64_B32: { /* every bit twice: bit k to bits 2k and 2k+1 */
        for (uint32_t h = 0; h < 2; h++) {
            uint32_t x = op2(t, SpvOpBitwiseAnd, U, h ? op2(t, SpvOpShiftRightLogical, U, A32(), cu(t, 16)) : A32(), cu(t, 0xFFFF));
            static const uint32_t k_sh[4] = {8, 4, 2, 1}, k_mask[4] = {0x00FF00FF, 0x0F0F0F0F, 0x33333333, 0x55555555};
            for (int k = 0; k < 4; k++)
                x = op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpBitwiseOr, U, x, op2(t, SpvOpShiftLeftLogical, U, x, cu(t, k_sh[k]))), cu(t, k_mask[k]));
            set_s(t, d + h, op2(t, SpvOpBitwiseOr, U, x, op2(t, SpvOpShiftLeftLogical, U, x, cu(t, 1))));
        }
        break;
    }
    case SH_OP_S_SEXT_I32_I8: set_s(t, d, bfe_i(t, A32(), cu(t, 0), cu(t, 8))); break;
    case SH_OP_S_SEXT_I32_I16: set_s(t, d, bfe_i(t, A32(), cu(t, 0), cu(t, 16))); break;
    case SH_OP_S_ABS_I32: { uint32_t r = ubits(t, ext1(t, t->t_i32, GLSLstd450SAbs, ibits(t, A32()))); set_s(t, d, r); set_scc(t, op2(t, SpvOpINotEqual, B, r, cu(t, 0))); break; }
    case SH_OP_S_GETPC_B64: {
        uint64_t pc = sh_inst_address(t->p,t->inst) + (uint64_t)in->words * 4;
        if (sh_pc_uniform(t->p)) { /* SH_U_PC + the offset: the same code wherever it is loaded */
            uint32_t rel = (uint32_t)(pc - t->p->base);
            uint32_t lo = op2(t, SpvOpIAdd, U, sb_load(t, t->u_var, cu(t, SH_U_PC)), cu(t, rel));
            uint32_t carry = op3(t, SpvOpSelect, U, op2(t, SpvOpULessThan, B, lo, cu(t, rel)), cu(t, 1), cu(t, 0));
            set_s(t, d, lo);
            set_s(t, d + 1, op2(t, SpvOpIAdd, U, sb_load(t, t->u_var, cu(t, SH_U_PC + 1)), carry));
            break;
        }
        set_s(t, d, cu(t, (uint32_t)pc));
        set_s(t, d + 1, cu(t, (uint32_t)(pc >> 32)));
        break;
    }
    case SH_OP_S_ADD_U32: {
        uint32_t r = op2(t, SpvOpIAdd, U, A32(), B32());
        set_s(t, d, r);
        set_scc(t, op2(t, SpvOpULessThan, B, r, A32()));
        break;
    }
    case SH_OP_S_SUB_U32: {
        set_s(t, d, op2(t, SpvOpISub, U, A32(), B32()));
        set_scc(t, op2(t, SpvOpUGreaterThan, B, B32(), A32()));
        break;
    }
    case SH_OP_S_ADD_I32: set_s(t, d, op2(t, SpvOpIAdd, U, A32(), B32())); break;
    case SH_OP_S_SUB_I32: set_s(t, d, op2(t, SpvOpISub, U, A32(), B32())); break;
    case SH_OP_S_ADDC_U32: {
        uint32_t c = b2u(t, get_scc(t));
        uint32_t s1 = op2(t, SpvOpIAdd, U, A32(), B32());
        uint32_t r = op2(t, SpvOpIAdd, U, s1, c);
        set_s(t, d, r);
        set_scc(t, op2(t, SpvOpLogicalOr, B, op2(t, SpvOpULessThan, B, s1, A32()), op2(t, SpvOpULessThan, B, r, s1)));
        break;
    }
    case SH_OP_S_SUBB_U32: {
        uint32_t c = b2u(t, get_scc(t));
        uint32_t r = op2(t, SpvOpISub, U, op2(t, SpvOpISub, U, A32(), B32()), c);
        set_s(t, d, r);
        uint32_t bl = op1(t, SpvOpUConvert, L, B32()), cl = op1(t, SpvOpUConvert, L, c), al = op1(t, SpvOpUConvert, L, A32());
        set_scc(t, op2(t, SpvOpUGreaterThan, B, op2(t, SpvOpIAdd, L, bl, cl), al));
        break;
    }
    case SH_OP_S_ADDK_I32: set_s(t, d, op2(t, SpvOpIAdd, U, a, b)); break;
    case SH_OP_S_MULK_I32: set_s(t, d, op2(t, SpvOpIMul, U, a, b)); break;
    case SH_OP_S_MIN_I32: { uint32_t c = op2(t, SpvOpSLessThan, B, ibits(t, A32()), ibits(t, B32())); set_s(t, d, sel(t, U, c, A32(), B32())); set_scc(t, c); break; }
    case SH_OP_S_MIN_U32: { uint32_t c = op2(t, SpvOpULessThan, B, A32(), B32()); set_s(t, d, sel(t, U, c, A32(), B32())); set_scc(t, c); break; }
    case SH_OP_S_MAX_I32: { uint32_t c = op2(t, SpvOpSGreaterThan, B, ibits(t, A32()), ibits(t, B32())); set_s(t, d, sel(t, U, c, A32(), B32())); set_scc(t, c); break; }
    case SH_OP_S_MAX_U32: { uint32_t c = op2(t, SpvOpUGreaterThan, B, A32(), B32()); set_s(t, d, sel(t, U, c, A32(), B32())); set_scc(t, c); break; }
    case SH_OP_S_AND_B32: LOGIC32(SpvOpBitwiseAnd)
    case SH_OP_S_OR_B32: LOGIC32(SpvOpBitwiseOr)
    case SH_OP_S_XOR_B32: LOGIC32(SpvOpBitwiseXor)
    case SH_OP_S_ANDN2_B32: { b = op1(t, SpvOpNot, U, B32()); LOGIC32(SpvOpBitwiseAnd) }
    case SH_OP_S_ORN2_B32: { uint32_t r = LB32(op2(t, SpvOpBitwiseOr, U, A32(), op1(t, SpvOpNot, U, B32()))); set_s(t, d, r); set_scc(t, op2(t, SpvOpINotEqual, B, r, cu(t, 0))); break; }
    case SH_OP_S_NAND_B32: { uint32_t r = LB32(op1(t, SpvOpNot, U, op2(t, SpvOpBitwiseAnd, U, A32(), B32()))); set_s(t, d, r); set_scc(t, op2(t, SpvOpINotEqual, B, r, cu(t, 0))); break; }
    case SH_OP_S_NOR_B32: { uint32_t r = LB32(op1(t, SpvOpNot, U, op2(t, SpvOpBitwiseOr, U, A32(), B32()))); set_s(t, d, r); set_scc(t, op2(t, SpvOpINotEqual, B, r, cu(t, 0))); break; }
    case SH_OP_S_XNOR_B32: { uint32_t r = LB32(op1(t, SpvOpNot, U, op2(t, SpvOpBitwiseXor, U, A32(), B32()))); set_s(t, d, r); set_scc(t, op2(t, SpvOpINotEqual, B, r, cu(t, 0))); break; }
    case SH_OP_S_LSHL_B32: { b = op2(t, SpvOpBitwiseAnd, U, B32(), cu(t, 31)); LOGIC32(SpvOpShiftLeftLogical) }
    case SH_OP_S_LSHR_B32: { b = op2(t, SpvOpBitwiseAnd, U, B32(), cu(t, 31)); LOGIC32(SpvOpShiftRightLogical) }
    case SH_OP_S_ASHR_I32: {
        uint32_t r = ubits(t, op2(t, SpvOpShiftRightArithmetic, t->t_i32, ibits(t, A32()), op2(t, SpvOpBitwiseAnd, U, B32(), cu(t, 31))));
        set_s(t, d, r); set_scc(t, op2(t, SpvOpINotEqual, B, r, cu(t, 0))); break;
    }
    case SH_OP_S_BFE_U32: {
        uint32_t r = bfe_u(t, A32(), B32(), op3(t, SpvOpBitFieldUExtract, U, B32(), cu(t, 16), cu(t, 7)));
        set_s(t, d, r); set_scc(t, op2(t, SpvOpINotEqual, B, r, cu(t, 0))); break;
    }
    case SH_OP_S_BFE_I32: {
        uint32_t r = bfe_i(t, A32(), B32(), op3(t, SpvOpBitFieldUExtract, U, B32(), cu(t, 16), cu(t, 7)));
        set_s(t, d, r); set_scc(t, op2(t, SpvOpINotEqual, B, r, cu(t, 0))); break;
    }
    case SH_OP_S_LSHL1_ADD_U32: case SH_OP_S_LSHL2_ADD_U32: case SH_OP_S_LSHL3_ADD_U32: case SH_OP_S_LSHL4_ADD_U32: {
        uint32_t n = op - SH_OP_S_LSHL1_ADD_U32 + 1;
        uint32_t r = op2(t, SpvOpIAdd, U, op2(t, SpvOpShiftLeftLogical, U, A32(), cu(t, n)), B32());
        set_s(t, d, r); set_scc(t, op2(t, SpvOpINotEqual, B, r, cu(t, 0))); break;
    }
    case SH_OP_S_AND_B64: LOGIC64(SpvOpBitwiseAnd)
    case SH_OP_S_OR_B64: LOGIC64(SpvOpBitwiseOr)
    case SH_OP_S_XOR_B64: LOGIC64(SpvOpBitwiseXor)
    case SH_OP_S_ANDN2_B64: { uint32_t r = op2(t, SpvOpBitwiseAnd, L, A64(), op1(t, SpvOpNot, L, B64())); set_s64(t, d, r); set_scc(t, nz64(t, r)); break; }
    case SH_OP_S_NAND_B64: { uint32_t r = LB64(op1(t, SpvOpNot, L, op2(t, SpvOpBitwiseAnd, L, A64(), B64()))); set_s64(t, d, r); set_scc(t, nz64(t, r)); break; }
    case SH_OP_S_NOR_B64: { uint32_t r = LB64(op1(t, SpvOpNot, L, op2(t, SpvOpBitwiseOr, L, A64(), B64()))); set_s64(t, d, r); set_scc(t, nz64(t, r)); break; }
    case SH_OP_S_XNOR_B64: { uint32_t r = LB64(op1(t, SpvOpNot, L, op2(t, SpvOpBitwiseXor, L, A64(), B64()))); set_s64(t, d, r); set_scc(t, nz64(t, r)); break; }
    case SH_OP_S_ORN2_B64: { uint32_t r = LB64(op2(t, SpvOpBitwiseOr, L, A64(), op1(t, SpvOpNot, L, B64()))); set_s64(t, d, r); set_scc(t, nz64(t, r)); break; }
    case SH_OP_S_LSHL_B64: { uint32_t r = op2(t, SpvOpShiftLeftLogical, L, A64(), op2(t, SpvOpBitwiseAnd, U, B32(), cu(t, 63))); set_s64(t, d, r); set_scc(t, nz64(t, r)); break; }
    case SH_OP_S_LSHR_B64: { uint32_t r = op2(t, SpvOpShiftRightLogical, L, A64(), op2(t, SpvOpBitwiseAnd, U, B32(), cu(t, 63))); set_s64(t, d, r); set_scc(t, nz64(t, r)); break; }
    case SH_OP_S_BFE_U64: {
        uint32_t w = op3(t, SpvOpBitFieldUExtract, U, B32(), cu(t, 16), cu(t, 7));
        uint32_t sh = op2(t, SpvOpShiftRightLogical, L, A64(), op2(t, SpvOpBitwiseAnd, U, B32(), cu(t, 63)));
        uint32_t ones = op1(t, SpvOpNot, L, op1(t, SpvOpUConvert, L, cu(t, 0)));
        uint32_t wc = ext2(t, U, GLSLstd450UMin, w, cu(t, 64));
        uint32_t m = op2(t, SpvOpShiftRightLogical, L, ones, op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpISub, U, cu(t, 64), wc), cu(t, 63)));
        m = sel(t, L, op2(t, SpvOpIEqual, B, wc, cu(t, 0)), op1(t, SpvOpUConvert, L, cu(t, 0)), m);
        uint32_t r = op2(t, SpvOpBitwiseAnd, L, sh, m);
        set_s64(t, d, r); set_scc(t, nz64(t, r)); break;
    }
    case SH_OP_S_BCNT1_I32_B64: {
        uint32_t v = A64();
        uint32_t lo = op1(t, SpvOpUConvert, U, v), hi = op1(t, SpvOpUConvert, U, op2(t, SpvOpShiftRightLogical, L, v, cu(t, 32)));
        uint32_t r = ubits(t, op2(t, SpvOpIAdd, t->t_i32, op1(t, SpvOpBitCount, t->t_i32, lo), op1(t, SpvOpBitCount, t->t_i32, hi)));
        set_s(t, d, r); set_scc(t, op2(t, SpvOpINotEqual, B, r, cu(t, 0))); break;
    }
    case SH_OP_S_BFM_B32: set_s(t, d, op2(t, SpvOpShiftLeftLogical, U, mask_bits(t, op2(t, SpvOpBitwiseAnd, U, A32(), cu(t, 31))), op2(t, SpvOpBitwiseAnd, U, B32(), cu(t, 31)))); break;
    case SH_OP_S_MUL_I32: set_s(t, d, op2(t, SpvOpIMul, U, A32(), B32())); break;
    case SH_OP_S_MUL_HI_U32: case SH_OP_S_MUL_HI_I32: {
        bool s = op == SH_OP_S_MUL_HI_I32;
        uint32_t x = op1(t, s ? SpvOpSConvert : SpvOpUConvert, L, s ? ibits(t, A32()) : A32());
        uint32_t y = op1(t, s ? SpvOpSConvert : SpvOpUConvert, L, s ? ibits(t, B32()) : B32());
        set_s(t, d, op1(t, SpvOpUConvert, U, op2(t, SpvOpShiftRightLogical, L, op2(t, SpvOpIMul, L, x, y), cu(t, 32))));
        break;
    }
    case SH_OP_S_BFM_B64: {
        uint32_t w = op2(t, SpvOpBitwiseAnd, U, A32(), cu(t, 63)), off = op2(t, SpvOpBitwiseAnd, U, B32(), cu(t, 63));
        uint32_t one = op1(t, SpvOpUConvert, L, cu(t, 1));
        uint32_t m = op2(t, SpvOpISub, L, op2(t, SpvOpShiftLeftLogical, L, one, w), one);
        set_s64(t, d, op2(t, SpvOpShiftLeftLogical, L, m, off));
        break;
    }
    case SH_OP_S_CSELECT_B32: set_s(t, d, sel(t, U, get_scc(t), A32(), B32())); break;
    case SH_OP_S_CSELECT_B64: set_s64(t, d, sel(t, L, get_scc(t), A64(), B64())); break;
    case SH_OP_S_PACK_LL_B32_B16: set_s(t, d, op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, A32(), cu(t, 0xFFFF)), op2(t, SpvOpShiftLeftLogical, U, B32(), cu(t, 16)))); break;
    case SH_OP_S_PACK_LH_B32_B16: set_s(t, d, op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, A32(), cu(t, 0xFFFF)), op2(t, SpvOpBitwiseAnd, U, B32(), cu(t, 0xFFFF0000u)))); break;
    case SH_OP_S_PACK_HH_B32_B16: set_s(t, d, op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpShiftRightLogical, U, A32(), cu(t, 16)), op2(t, SpvOpBitwiseAnd, U, B32(), cu(t, 0xFFFF0000u)))); break;
    case SH_OP_S_BITSET0_B32:
        set_s(t, d, op2(t, SpvOpBitwiseAnd, U, get_s(t, d),
                        op1(t, SpvOpNot, U, op2(t, SpvOpShiftLeftLogical, U, cu(t, 1), op2(t, SpvOpBitwiseAnd, U, A32(), cu(t, 31))))));
        break;
    case SH_OP_S_BITSET1_B32:
        set_s(t, d, op2(t, SpvOpBitwiseOr, U, get_s(t, d),
                        op2(t, SpvOpShiftLeftLogical, U, cu(t, 1), op2(t, SpvOpBitwiseAnd, U, A32(), cu(t, 31)))));
        break;
    case SH_OP_S_SETREG_B32:
    case SH_OP_S_SETREG_IMM32_B32: break; /* rounding/denormal modes: host defaults */
    case SH_OP_S_GETREG_B32: set_s(t, d, cu(t, 0)); break;
    case SH_OP_S_ORN2_SAVEEXEC_B64: case SH_OP_S_NAND_SAVEEXEC_B64: case SH_OP_S_NOR_SAVEEXEC_B64:
    case SH_OP_S_XNOR_SAVEEXEC_B64: case SH_OP_S_ANDN1_SAVEEXEC_B64: case SH_OP_S_ORN1_SAVEEXEC_B64: {
        uint32_t exec = src64(t, in, RDNA2_OP_EXEC_LO), s0 = A64();
        set_s64(t, d, exec);
        uint32_t nexec = op1(t, SpvOpNot, L, exec), ns0 = op1(t, SpvOpNot, L, s0);
        uint32_t r = op == SH_OP_S_ORN2_SAVEEXEC_B64 ? op2(t, SpvOpBitwiseOr, L, s0, nexec)
                   : op == SH_OP_S_NAND_SAVEEXEC_B64 ? op1(t, SpvOpNot, L, op2(t, SpvOpBitwiseAnd, L, s0, exec))
                   : op == SH_OP_S_NOR_SAVEEXEC_B64 ? op1(t, SpvOpNot, L, op2(t, SpvOpBitwiseOr, L, s0, exec))
                   : op == SH_OP_S_XNOR_SAVEEXEC_B64 ? op1(t, SpvOpNot, L, op2(t, SpvOpBitwiseXor, L, s0, exec))
                   : op == SH_OP_S_ANDN1_SAVEEXEC_B64 ? op2(t, SpvOpBitwiseAnd, L, ns0, exec)
                   : op2(t, SpvOpBitwiseOr, L, ns0, exec);
        r = LB64(r);
        set_s64(t, RDNA2_OP_EXEC_LO, r);
        set_scc(t, nz64(t, r));
        break;
    }
    case SH_OP_S_ORN2_SAVEEXEC_B32: case SH_OP_S_NAND_SAVEEXEC_B32: case SH_OP_S_NOR_SAVEEXEC_B32:
    case SH_OP_S_XNOR_SAVEEXEC_B32: case SH_OP_S_ANDN1_SAVEEXEC_B32: case SH_OP_S_ORN1_SAVEEXEC_B32: {
        uint32_t exec = get_s(t, RDNA2_OP_EXEC_LO), s0 = A32();
        set_s(t, d, exec);
        uint32_t nexec = op1(t, SpvOpNot, U, exec), ns0 = op1(t, SpvOpNot, U, s0);
        uint32_t r = op == SH_OP_S_ORN2_SAVEEXEC_B32 ? op2(t, SpvOpBitwiseOr, U, s0, nexec)
                   : op == SH_OP_S_NAND_SAVEEXEC_B32 ? op1(t, SpvOpNot, U, op2(t, SpvOpBitwiseAnd, U, s0, exec))
                   : op == SH_OP_S_NOR_SAVEEXEC_B32 ? op1(t, SpvOpNot, U, op2(t, SpvOpBitwiseOr, U, s0, exec))
                   : op == SH_OP_S_XNOR_SAVEEXEC_B32 ? op1(t, SpvOpNot, U, op2(t, SpvOpBitwiseXor, U, s0, exec))
                   : op == SH_OP_S_ANDN1_SAVEEXEC_B32 ? op2(t, SpvOpBitwiseAnd, U, ns0, exec)
                   : op2(t, SpvOpBitwiseOr, U, ns0, exec);
        r = LB32(r);
        set_s(t, RDNA2_OP_EXEC_LO, r);
        set_scc(t, op2(t, SpvOpINotEqual, B, r, cu(t, 0)));
        break;
    }
    case SH_OP_S_AND_SAVEEXEC_B64: case SH_OP_S_OR_SAVEEXEC_B64: case SH_OP_S_XOR_SAVEEXEC_B64: case SH_OP_S_ANDN2_SAVEEXEC_B64: {
        uint32_t exec = src64(t, in, RDNA2_OP_EXEC_LO), s0 = A64();
        set_s64(t, d, exec);
        uint32_t r = op == SH_OP_S_AND_SAVEEXEC_B64 ? op2(t, SpvOpBitwiseAnd, L, s0, exec)
                   : op == SH_OP_S_OR_SAVEEXEC_B64 ? op2(t, SpvOpBitwiseOr, L, s0, exec)
                   : op == SH_OP_S_XOR_SAVEEXEC_B64 ? op2(t, SpvOpBitwiseXor, L, s0, exec)
                   : op2(t, SpvOpBitwiseAnd, L, s0, op1(t, SpvOpNot, L, exec));
        set_s64(t, RDNA2_OP_EXEC_LO, r);
        set_scc(t, nz64(t, r));
        break;
    }
    case SH_OP_S_AND_SAVEEXEC_B32: case SH_OP_S_OR_SAVEEXEC_B32: case SH_OP_S_XOR_SAVEEXEC_B32: case SH_OP_S_ANDN2_SAVEEXEC_B32: {
        uint32_t exec = get_s(t, RDNA2_OP_EXEC_LO), s0 = A32();
        set_s(t, d, exec);
        uint32_t r = op == SH_OP_S_AND_SAVEEXEC_B32 ? op2(t, SpvOpBitwiseAnd, U, s0, exec)
                   : op == SH_OP_S_OR_SAVEEXEC_B32 ? op2(t, SpvOpBitwiseOr, U, s0, exec)
                   : op == SH_OP_S_XOR_SAVEEXEC_B32 ? op2(t, SpvOpBitwiseXor, U, s0, exec)
                   : op2(t, SpvOpBitwiseAnd, U, s0, op1(t, SpvOpNot, U, exec));
        set_s(t, RDNA2_OP_EXEC_LO, r);
        set_scc(t, op2(t, SpvOpINotEqual, B, r, cu(t, 0)));
        break;
    }
    case SH_OP_S_CMP_EQ_I32: case SH_OP_S_CMP_EQ_U32: case SH_OP_S_CMPK_EQ_I32: case SH_OP_S_CMPK_EQ_U32: CMP32(SpvOpIEqual)
    case SH_OP_S_CMP_LG_I32: case SH_OP_S_CMP_LG_U32: case SH_OP_S_CMPK_LG_I32: case SH_OP_S_CMPK_LG_U32: CMP32(SpvOpINotEqual)
    case SH_OP_S_CMP_GT_I32: case SH_OP_S_CMPK_GT_I32: ICMP32(SpvOpSGreaterThan)
    case SH_OP_S_CMP_GE_I32: case SH_OP_S_CMPK_GE_I32: ICMP32(SpvOpSGreaterThanEqual)
    case SH_OP_S_CMP_LT_I32: case SH_OP_S_CMPK_LT_I32: ICMP32(SpvOpSLessThan)
    case SH_OP_S_CMP_LE_I32: case SH_OP_S_CMPK_LE_I32: ICMP32(SpvOpSLessThanEqual)
    case SH_OP_S_CMP_GT_U32: case SH_OP_S_CMPK_GT_U32: CMP32(SpvOpUGreaterThan)
    case SH_OP_S_CMP_GE_U32: case SH_OP_S_CMPK_GE_U32: CMP32(SpvOpUGreaterThanEqual)
    case SH_OP_S_CMP_LT_U32: case SH_OP_S_CMPK_LT_U32: CMP32(SpvOpULessThan)
    case SH_OP_S_CMP_LE_U32: case SH_OP_S_CMPK_LE_U32: CMP32(SpvOpULessThanEqual)
    case SH_OP_S_CMP_EQ_U64: set_scc(t, op2(t, SpvOpIEqual, B, A64(), B64())); break;
    case SH_OP_S_CMP_LG_U64: set_scc(t, op2(t, SpvOpINotEqual, B, A64(), B64())); break;
    case SH_OP_S_BITCMP0_B32: set_scc(t, op1(t, SpvOpLogicalNot, B, bit0(t, op2(t, SpvOpShiftRightLogical, U, A32(), op2(t, SpvOpBitwiseAnd, U, B32(), cu(t, 31)))))); break;
    case SH_OP_S_BITCMP1_B32: set_scc(t, bit0(t, op2(t, SpvOpShiftRightLogical, U, A32(), op2(t, SpvOpBitwiseAnd, U, B32(), cu(t, 31))))); break;
    case SH_OP_S_BITCMP0_B64: case SH_OP_S_BITCMP1_B64: {
        uint32_t bit = op1(t, SpvOpUConvert, U, op2(t, SpvOpBitwiseAnd, L, op2(t, SpvOpShiftRightLogical, L, A64(), op2(t, SpvOpBitwiseAnd, U, B32(), cu(t, 63))),
                                                     op1(t, SpvOpUConvert, L, cu(t, 1))));
        uint32_t set = op2(t, SpvOpINotEqual, B, bit, cu(t, 0));
        set_scc(t, op == SH_OP_S_BITCMP1_B64 ? set : op1(t, SpvOpLogicalNot, B, set));
        break;
    }
    case SH_OP_S_ASHR_I64: {
        uint32_t r = op2(t, SpvOpShiftRightArithmetic, L, A64(), op2(t, SpvOpBitwiseAnd, U, B32(), cu(t, 63)));
        set_s64(t, d, r); set_scc(t, nz64(t, r)); break;
    }
    case SH_OP_S_BFE_I64: { /* signed field of S0 at S1[5:0], S1[22:16] bits wide */
        uint32_t off = op2(t, SpvOpBitwiseAnd, U, B32(), cu(t, 63));
        uint32_t w = ext2(t, U, GLSLstd450UMin, op3(t, SpvOpBitFieldUExtract, U, B32(), cu(t, 16), cu(t, 7)), op2(t, SpvOpISub, U, cu(t, 64), off));
        uint32_t wz = op2(t, SpvOpIEqual, B, w, cu(t, 0)), w1 = sel(t, U, wz, cu(t, 1), w);
        uint32_t up = op2(t, SpvOpShiftLeftLogical, L, A64(), op2(t, SpvOpISub, U, op2(t, SpvOpISub, U, cu(t, 64), off), w1));
        uint32_t r = op2(t, SpvOpShiftRightArithmetic, L, up, op2(t, SpvOpISub, U, cu(t, 64), w1));
        r = sel(t, L, wz, op1(t, SpvOpUConvert, L, cu(t, 0)), r);
        set_s64(t, d, r); set_scc(t, nz64(t, r)); break;
    }
    case SH_OP_S_CMOVK_I32: set_s(t, d, sel(t, U, get_scc(t), b, a)); break;
    case SH_OP_S_ABSDIFF_I32: {
        uint32_t r = ubits(t, ext1(t, t->t_i32, GLSLstd450SAbs, ibits(t, op2(t, SpvOpISub, U, A32(), B32()))));
        set_s(t, d, r); set_scc(t, op2(t, SpvOpINotEqual, B, r, cu(t, 0))); break;
    }
    case SH_OP_S_CMOV_B64: set_s64(t, d, sel(t, L, get_scc(t), A64(), src64(t, in, d))); break;
    case SH_OP_S_BCNT0_I32_B32: { uint32_t r = ubits(t, op1(t, SpvOpBitCount, t->t_i32, op1(t, SpvOpNot, U, A32()))); set_s(t, d, r); set_scc(t, op2(t, SpvOpINotEqual, B, r, cu(t, 0))); break; }
    case SH_OP_S_BCNT0_I32_B64: {
        uint32_t v = op1(t, SpvOpNot, L, A64());
        uint32_t lo = op1(t, SpvOpUConvert, U, v), hi = op1(t, SpvOpUConvert, U, op2(t, SpvOpShiftRightLogical, L, v, cu(t, 32)));
        uint32_t r = ubits(t, op2(t, SpvOpIAdd, t->t_i32, op1(t, SpvOpBitCount, t->t_i32, lo), op1(t, SpvOpBitCount, t->t_i32, hi)));
        set_s(t, d, r); set_scc(t, op2(t, SpvOpINotEqual, B, r, cu(t, 0))); break;
    }
    case SH_OP_S_FF0_I32_B32: set_s(t, d, ubits(t, ext1(t, t->t_i32, GLSLstd450FindILsb, op1(t, SpvOpNot, U, A32())))); break;
    case SH_OP_S_FF0_I32_B64: { /* lowest clear bit of the pair, -1 if none */
        uint32_t v = op1(t, SpvOpNot, L, A64());
        uint32_t lo = op1(t, SpvOpUConvert, U, v), hi = op1(t, SpvOpUConvert, U, op2(t, SpvOpShiftRightLogical, L, v, cu(t, 32)));
        uint32_t flo = ubits(t, ext1(t, t->t_i32, GLSLstd450FindILsb, lo)), fhi = ubits(t, ext1(t, t->t_i32, GLSLstd450FindILsb, hi));
        uint32_t h = sel(t, U, op2(t, SpvOpIEqual, B, hi, cu(t, 0)), cu(t, ~0u), op2(t, SpvOpIAdd, U, fhi, cu(t, 32)));
        set_s(t, d, sel(t, U, op2(t, SpvOpIEqual, B, lo, cu(t, 0)), h, flo));
        break;
    }
    case SH_OP_S_FLBIT_I32: { /* bits equal to the sign before the first that differs, -1 for 0 and -1 */
        uint32_t x = op2(t, SpvOpBitwiseXor, U, A32(), ubits(t, op2(t, SpvOpShiftRightArithmetic, t->t_i32, ibits(t, A32()), cu(t, 31))));
        uint32_t m = ubits(t, ext1(t, t->t_i32, GLSLstd450FindUMsb, x));
        set_s(t, d, sel(t, U, op2(t, SpvOpIEqual, B, x, cu(t, 0)), cu(t, 0xFFFFFFFFu), op2(t, SpvOpISub, U, cu(t, 31), m)));
        break;
    }
    case SH_OP_S_FLBIT_I32_I64: {
        uint32_t v = A64();
        v = op2(t, SpvOpBitwiseXor, L, v, op2(t, SpvOpShiftRightArithmetic, L, v, cu(t, 63)));
        uint32_t lo = op1(t, SpvOpUConvert, U, v), hi = op1(t, SpvOpUConvert, U, op2(t, SpvOpShiftRightLogical, L, v, cu(t, 32)));
        uint32_t mlo = ubits(t, ext1(t, t->t_i32, GLSLstd450FindUMsb, lo)), mhi = ubits(t, ext1(t, t->t_i32, GLSLstd450FindUMsb, hi));
        uint32_t l = sel(t, U, op2(t, SpvOpIEqual, B, lo, cu(t, 0)), cu(t, 0xFFFFFFFFu), op2(t, SpvOpISub, U, cu(t, 63), mlo));
        set_s(t, d, sel(t, U, op2(t, SpvOpIEqual, B, hi, cu(t, 0)), l, op2(t, SpvOpISub, U, cu(t, 31), mhi)));
        break;
    }
    case SH_OP_S_BREV_B64: {
        uint32_t lo = op1(t, SpvOpBitReverse, U, get_s(t, in->src[0])), hi = op1(t, SpvOpBitReverse, U, src_u(t, in, in->src[0] + 1));
        if (in->src[0] >= SH_SGPRS) { /* an inline constant: its high half is 0 or the sign */
            uint32_t v = A64();
            lo = op1(t, SpvOpBitReverse, U, op1(t, SpvOpUConvert, U, v));
            hi = op1(t, SpvOpBitReverse, U, op1(t, SpvOpUConvert, U, op2(t, SpvOpShiftRightLogical, L, v, cu(t, 32))));
        }
        set_s(t, d, hi);
        set_s(t, d + 1, lo);
        break;
    }
    case SH_OP_S_BITSET0_B64: case SH_OP_S_BITSET1_B64: {
        uint32_t m = op2(t, SpvOpShiftLeftLogical, L, op1(t, SpvOpUConvert, L, cu(t, 1)), op2(t, SpvOpBitwiseAnd, U, A32(), cu(t, 63)));
        uint32_t cur = src64(t, in, d);
        set_s64(t, d, op == SH_OP_S_BITSET1_B64 ? op2(t, SpvOpBitwiseOr, L, cur, m) : op2(t, SpvOpBitwiseAnd, L, cur, op1(t, SpvOpNot, L, m)));
        break;
    }
    case SH_OP_S_ANDN1_WREXEC_B64: case SH_OP_S_ANDN2_WREXEC_B64: { /* EXEC = ~S0 & EXEC / S0 & ~EXEC, D = EXEC */
        uint32_t exec = src64(t, in, RDNA2_OP_EXEC_LO), s0 = A64();
        uint32_t r = op == SH_OP_S_ANDN1_WREXEC_B64 ? op2(t, SpvOpBitwiseAnd, L, op1(t, SpvOpNot, L, s0), exec)
                                                    : op2(t, SpvOpBitwiseAnd, L, s0, op1(t, SpvOpNot, L, exec));
        set_s64(t, RDNA2_OP_EXEC_LO, r);
        set_s64(t, d, r);
        set_scc(t, nz64(t, r));
        break;
    }
    case SH_OP_S_ANDN1_WREXEC_B32: case SH_OP_S_ANDN2_WREXEC_B32: {
        uint32_t exec = get_s(t, RDNA2_OP_EXEC_LO), s0 = A32();
        uint32_t r = op == SH_OP_S_ANDN1_WREXEC_B32 ? op2(t, SpvOpBitwiseAnd, U, op1(t, SpvOpNot, U, s0), exec)
                                                    : op2(t, SpvOpBitwiseAnd, U, s0, op1(t, SpvOpNot, U, exec));
        set_s(t, RDNA2_OP_EXEC_LO, r);
        set_s(t, d, r);
        set_scc(t, op2(t, SpvOpINotEqual, B, r, cu(t, 0)));
        break;
    }
    default: fail(t, "scalar instruction not supported"); break;
    }
    if (!t->full_wave64 && !t->full_wave32 && in->enc != RDNA2_ENC_SOPC && d < SH_SGPRS) { /* plain lane masks (raw_mask) */
        bool wide = op == SH_OP_S_MOV_B64 || op == SH_OP_S_BITREPLICATE_B64_B32 || op == SH_OP_S_BFM_B64 ||
                    (in->name && strstr(in->name, "_b64"));
        bool raw = false;
        if (op == SH_OP_S_BITREPLICATE_B64_B32 || op == SH_OP_S_BFM_B64) raw = true;
        else if (op == SH_OP_S_MOVK_I32) raw = true;
        else if (op == SH_OP_S_MOV_B32 || op == SH_OP_S_MOV_B64)
            raw = in->src[0] >= 128 || (in->src[0] < SH_SGPRS && t->raw_mask[in->src[0]]);
        else if (bitwise && !lane_result && (in->enc == RDNA2_ENC_SOP1 || in->enc == RDNA2_ENC_SOP2)) {
            /* A logic op on plain masks and constants is a plain mask too: RADV's
             * s_mov_b64 vcc, 0 / s_not_b64 s[8:9], vcc / s_and_saveexec_b64 left the
             * whole-wave ~0 in EXEC, and a later v_cmpx (bit 0) never emptied a loop's
             * remaining-lanes copy of it (Longshot's Lanczos resample ran to the limit). */
            raw = true;
            for (int k = 0; k < (in->enc == RDNA2_ENC_SOP2 ? 2 : 1) && raw; k++) {
                uint32_t code = in->src[k], v;
                raw = (code < SH_SGPRS && t->raw_mask[code]) ||
                      (code >= 128 && code != RDNA2_OP_SCC && sh_inline_const(code, in->literal, &v));
            }
        }
        /* A whole-wave constant mask (s_mov_b64 / s_cselect_b64 of -1 and 0) becomes the
         * invocation's own bit at once, the form EXEC and its copies have. Paths that
         * meet at one instruction then agree: Longshot's frame generation reaches its
         * final s_not_b64 / s_and_saveexec_b64 with s[10:11] either -1 / 0 or a copy of
         * EXEC, and one static form for both turned ~1 into a lane-position mask. */
        if (t->info->stage == SH_STAGE_CS && t->in_lid && wide && d != RDNA2_OP_EXEC_LO && d + 1 < SH_SGPRS &&
            (op == SH_OP_S_MOV_B64 || op == SH_OP_S_CSELECT_B64) && !sgpr_scalar_use(t, d)) {
            bool constant_mask = true;
            for (int k = 0; k < (op == SH_OP_S_CSELECT_B64 ? 2 : 1) && constant_mask; k++) {
                uint32_t code = in->src[k], v;
                constant_mask = code >= 128 && code != RDNA2_OP_SCC && sh_inline_const(code, in->literal, &v) &&
                                (v == 0 || (v == 0xFFFFFFFFu && code >= 193 && code <= 208));
            }
            if (constant_mask) {
                set_s(t, d, op2(t, SpvOpBitwiseAnd, U, get_s(t, d), cu(t, 1)));
                set_s(t, d + 1, cu(t, 0));
                raw = false;
                lane_result = true;
            }
        }
        t->raw_mask[d] = raw;
        t->lane_mask[d] = lane_result;
        if (wide && d + 1 < SH_SGPRS) {
            t->raw_mask[d + 1] = raw;
            t->lane_mask[d + 1] = lane_result;
        }
        if (raw && d == RDNA2_OP_EXEC_LO && t->info->stage == SH_STAGE_CS && t->in_lid) {
            uint32_t lo = get_s(t, RDNA2_OP_EXEC_LO), hi = t->p->wave32 ? cu(t, 0) : get_s(t, RDNA2_OP_EXEC_HI);
            set_s(t, RDNA2_OP_EXEC_LO, own_lane_bit(t, lo, hi));
            if (!t->p->wave32) set_s(t, RDNA2_OP_EXEC_HI, cu(t, 0));
            t->raw_mask[RDNA2_OP_EXEC_LO] = t->raw_mask[RDNA2_OP_EXEC_HI] = false;
            t->lane_mask[RDNA2_OP_EXEC_LO] = t->lane_mask[RDNA2_OP_EXEC_HI] = true;
        }
        /* EXEC computed by arithmetic (an NGG shader's s_lshr_b64 exec, -1, 64-n) is a
         * whole-wave value, but each invocation here is a lane of its own, which is
         * bit 0. Left whole, a copy of it never emptied in a waterfall loop
         * (s_ff1 / v_readlane / v_cmp / s_andn2 clear only bit 0): Resident Evil 4's
         * shadow draws ran to the loop limit, ~270 ms each. */
        if (!bitwise && d == RDNA2_OP_EXEC_LO && !(t->info->stage == SH_STAGE_CS && t->in_lid)) {
            set_s(t, RDNA2_OP_EXEC_LO, op2(t, SpvOpBitwiseAnd, U, get_s(t, RDNA2_OP_EXEC_LO), cu(t, 1)));
            if (wide) set_s(t, RDNA2_OP_EXEC_HI, cu(t, 0));
            t->raw_mask[RDNA2_OP_EXEC_LO] = t->raw_mask[RDNA2_OP_EXEC_HI] = false;
            t->lane_mask[RDNA2_OP_EXEC_LO] = t->lane_mask[RDNA2_OP_EXEC_HI] = true;
        }
    }
#undef A32
#undef B32
#undef A64
#undef B64
#undef LOGIC32
#undef LOGIC64
#undef LB32
#undef LB64
#undef CMP32
#undef ICMP32
}

/* ---- Vector compares ---- */

static uint32_t srch(tr *t, const rdna2_inst *in, int k, bool hi, bool abs, bool neg);

/* v_cmp_class: bit of the class mask for the kind of float a is:
 * 0 signaling NaN, 1 quiet NaN, 2 -inf, 3 -normal, 4 -denormal, 5 -0, 6 +0,
 * 7 +denormal, 8 +normal, 9 +inf. Classification preserves the NaN quiet bit;
 * converting or quieting the operand first changes masks used as integer tests. */
/* Float classification from the bits, any width: `bits` (of type T) holds the sign at
 * `sign_bit`, an exponent field `exp_mask`, and is infinite at magnitude `inf_mag` */
static uint32_t class_of_bits(tr *t, uint32_t T, uint32_t bits, uint32_t sign, uint32_t mag_mask, uint32_t exp_mask,
                              uint32_t quiet_bit, uint32_t zero_c, uint32_t mask) {
    uint32_t U = t->t_u32, B = t->t_bool;
    uint32_t neg = op2(t, SpvOpINotEqual, B, op2(t, SpvOpBitwiseAnd, T, bits, sign), zero_c);
    uint32_t mag = op2(t, SpvOpBitwiseAnd, T, bits, mag_mask);
    uint32_t exp0 = op2(t, SpvOpIEqual, B, op2(t, SpvOpBitwiseAnd, T, mag, exp_mask), zero_c);
    uint32_t zero = op2(t, SpvOpIEqual, B, mag, zero_c);
    uint32_t nan = op2(t, SpvOpUGreaterThan, B, mag, exp_mask);
    uint32_t inf = op2(t, SpvOpIEqual, B, mag, exp_mask);
    uint32_t den = op2(t, SpvOpLogicalAnd, B, exp0, op1(t, SpvOpLogicalNot, B, zero));
    /* bit index: pick by kind, then sign */
    uint32_t pos = sel(t, U, zero, cu(t, 6), sel(t, U, den, cu(t, 7), sel(t, U, inf, cu(t, 9), cu(t, 8))));
    uint32_t negi = sel(t, U, zero, cu(t, 5), sel(t, U, den, cu(t, 4), sel(t, U, inf, cu(t, 2), cu(t, 3))));
    uint32_t quiet = op2(t, SpvOpINotEqual, B, op2(t, SpvOpBitwiseAnd, T, bits, quiet_bit), zero_c);
    uint32_t idx = sel(t, U, nan, sel(t, U, quiet, cu(t, 1), cu(t, 0)), sel(t, U, neg, negi, pos));
    return bit0(t, op2(t, SpvOpShiftRightLogical, U, mask, idx));
}
static uint32_t float_class(tr *t, uint32_t a, uint32_t mask) {
    return class_of_bits(t, t->t_u32, ubits(t, a), cu(t, 0x80000000u), cu(t, 0x7FFFFFFF), cu(t, 0x7F800000),
                         cu(t, 0x00400000), cu(t, 0), mask);
}
static uint32_t c64(tr *t, uint64_t v) {
    uint32_t L = t->t_u64;
    return op2(t, SpvOpBitwiseOr, L, op1(t, SpvOpUConvert, L, cu(t, (uint32_t)v)),
               op2(t, SpvOpShiftLeftLogical, L, op1(t, SpvOpUConvert, L, cu(t, (uint32_t)(v >> 32))), cu(t, 32)));
}

/* f64 compares on the bits (no 64-bit float on the host GPU needed): NaNs unordered,
 * -0 equal to +0, otherwise ordered as sign-magnitude integers */
static uint32_t f64_compare(tr *t, const char *cond, uint32_t a, uint32_t b) {
    uint32_t L = t->t_u64, B = t->t_bool;
    uint32_t mag_m = c64(t, 0x7FFFFFFFFFFFFFFFull), inf = c64(t, 0x7FF0000000000000ull), sign = c64(t, 0x8000000000000000ull);
    uint32_t ma = op2(t, SpvOpBitwiseAnd, L, a, mag_m), mb = op2(t, SpvOpBitwiseAnd, L, b, mag_m);
    uint32_t nan = op2(t, SpvOpLogicalOr, B, op2(t, SpvOpUGreaterThan, B, ma, inf), op2(t, SpvOpUGreaterThan, B, mb, inf));
    /* order key: zeros to 0, negatives below positives */
    uint32_t zero = c64(t, 0);
    uint32_t key[2], in[2] = {a, b}, mg[2] = {ma, mb};
    for (int k = 0; k < 2; k++) {
        uint32_t x = sel(t, L, op2(t, SpvOpIEqual, B, mg[k], zero), zero, in[k]);
        uint32_t neg = op2(t, SpvOpINotEqual, B, op2(t, SpvOpBitwiseAnd, L, x, sign), zero);
        key[k] = sel(t, L, neg, op1(t, SpvOpNot, L, x), op2(t, SpvOpBitwiseOr, L, x, sign));
    }
    uint32_t lt = op2(t, SpvOpULessThan, B, key[0], key[1]), eq = op2(t, SpvOpIEqual, B, key[0], key[1]);
    uint32_t gt = op2(t, SpvOpUGreaterThan, B, key[0], key[1]);
    uint32_t r;
    const char *c = cond;
    bool unord = c[0] == 'n'; /* nge, nlg, ngt, nle, neq, nlt: NaN or not the relation */
    if (unord) c++;
    if (!strcmp(c, "lt")) r = lt;
    else if (!strcmp(c, "eq")) r = eq;
    else if (!strcmp(c, "le")) r = op2(t, SpvOpLogicalOr, B, lt, eq);
    else if (!strcmp(c, "gt")) r = gt;
    else if (!strcmp(c, "lg")) r = op1(t, SpvOpLogicalNot, B, eq);
    else if (!strcmp(c, "ge")) r = op2(t, SpvOpLogicalOr, B, gt, eq);
    else if (!unord && !strcmp(c, "o")) return op1(t, SpvOpLogicalNot, B, nan);
    else if (!unord && !strcmp(c, "u")) return nan;
    else if (!unord && !strcmp(c, "f")) return spv_const_bool(&t->b, false);
    else if (!unord && !strcmp(c, "tru")) return spv_const_bool(&t->b, true);
    else return 0;
    if (unord) return op2(t, SpvOpLogicalOr, B, nan, op1(t, SpvOpLogicalNot, B, r));
    return op2(t, SpvOpLogicalAnd, B, op1(t, SpvOpLogicalNot, B, nan), r);
}

static uint32_t vsrc64(tr *t, const rdna2_inst *in, int k);
static uint32_t src16(tr *t, const rdna2_inst *in, int k, bool hi, bool f16);

static void vcmp(tr *t, const rdna2_inst *in, bool x) {
    const char *n = in->name + (x ? 7 : 6); /* after "v_cmpx_" / "v_cmp_" */
    char cond[8] = {0};
    size_t k = 0;
    while (n[k] && n[k] != '_' && k < 7) {
        cond[k] = n[k];
        k++;
    }
    const char *type = n[k] == '_' ? n + k + 1 : "";
    uint32_t B = t->t_bool, c = 0;
    if (!strcmp(cond, "class") && !strcmp(type, "f32")) {
        c = float_class(t, src_f(t, in, 0), src(t, in, 1));
    } else if (!strcmp(cond, "class") && !strcmp(type, "f16")) { /* on the half's own bits (its denormals) */
        bool v3 = is_vop3(in);
        uint32_t h = src16(t, in, 0, v3 && (in->opsel & 1), true);
        if (v3 && (in->abs & 1)) h = op2(t, SpvOpBitwiseAnd, t->t_u32, h, cu(t, 0x7FFF));
        if (v3 && (in->neg & 1)) h = op2(t, SpvOpBitwiseXor, t->t_u32, h, cu(t, 0x8000));
        c = class_of_bits(t, t->t_u32, h, cu(t, 0x8000), cu(t, 0x7FFF), cu(t, 0x7C00), cu(t, 0x0200),
                          cu(t, 0), src(t, in, 1));
    } else if (!strcmp(type, "f64")) { /* abs and neg act on the sign bit */
        uint32_t v[2], L = t->t_u64;
        static const uint64_t k_f64[9] = {0x3FE0000000000000ull, 0xBFE0000000000000ull, 0x3FF0000000000000ull,
                                          0xBFF0000000000000ull, 0x4000000000000000ull, 0xC000000000000000ull,
                                          0x4010000000000000ull, 0xC010000000000000ull, 0x3FC45F306DC9C882ull};
        for (int j = 0; j < 2; j++) {
            uint32_t code = in->src[j];
            /* float constants are doubles here; a literal is the high half */
            v[j] = code >= 240 && code <= 248 ? c64(t, k_f64[code - 240])
                 : code == RDNA2_OP_LITERAL ? c64(t, (uint64_t)in->literal << 32) : vsrc64(t, in, j);
            if (is_vop3(in) && ((in->abs >> j) & 1)) v[j] = op2(t, SpvOpBitwiseAnd, L, v[j], c64(t, 0x7FFFFFFFFFFFFFFFull));
            if (is_vop3(in) && ((in->neg >> j) & 1)) v[j] = op2(t, SpvOpBitwiseXor, L, v[j], c64(t, 0x8000000000000000ull));
        }
        if (!strcmp(cond, "class"))
            c = class_of_bits(t, L, v[0], c64(t, 0x8000000000000000ull), c64(t, 0x7FFFFFFFFFFFFFFFull),
                              c64(t, 0x7FF0000000000000ull), c64(t, 0x0008000000000000ull), c64(t, 0), src(t, in, 1));
        else
            c = f64_compare(t, cond, v[0], v[1]);
    } else if (!strcmp(type, "f32") || !strcmp(type, "f16")) {
        uint32_t a, b;
        if (type[1] == '1') { /* f16: VOP3 op_sel picks halves */
            bool v3 = is_vop3(in);
            uint32_t ab = v3 ? in->abs : 0, ng = v3 ? in->neg : 0, os = v3 ? in->opsel : 0;
            a = srch(t, in, 0, os & 1, ab & 1, ng & 1);
            b = srch(t, in, 1, (os >> 1) & 1, (ab >> 1) & 1, (ng >> 1) & 1);
        } else {
            a = src_f(t, in, 0);
            b = src_f(t, in, 1);
        }
        static const struct { const char *n; uint32_t op; } k_f[] = {
            {"lt", SpvOpFOrdLessThan}, {"eq", SpvOpFOrdEqual}, {"le", SpvOpFOrdLessThanEqual},
            {"gt", SpvOpFOrdGreaterThan}, {"lg", SpvOpFOrdNotEqual}, {"ge", SpvOpFOrdGreaterThanEqual},
            {"nge", SpvOpFUnordLessThan}, {"nlg", SpvOpFUnordEqual}, {"ngt", SpvOpFUnordLessThanEqual},
            {"nle", SpvOpFUnordGreaterThan}, {"neq", SpvOpFUnordNotEqual}, {"nlt", SpvOpFUnordGreaterThanEqual}};
        for (size_t i = 0; i < PS5C_ARRAY_COUNT(k_f) && !c; i++) {
            if (!strcmp(cond, k_f[i].n)) c = op2(t, k_f[i].op, B, a, b);
        }
        if (!c) {
            uint32_t nan = op2(t, SpvOpLogicalOr, B, op1(t, SpvOpIsNan, B, a), op1(t, SpvOpIsNan, B, b));
            if (!strcmp(cond, "f")) c = spv_const_bool(&t->b, false);
            else if (!strcmp(cond, "tru")) c = spv_const_bool(&t->b, true);
            else if (!strcmp(cond, "o")) c = op1(t, SpvOpLogicalNot, B, nan);
            else if (!strcmp(cond, "u")) c = nan;
        }
    } else if (!strcmp(type, "i16") || !strcmp(type, "u16")) {
        bool s = type[0] == 'i', v3 = is_vop3(in);
        uint32_t os = v3 ? in->opsel : 0;
        uint32_t a = src16(t, in, 0, os & 1, false), b = src16(t, in, 1, (os >> 1) & 1, false);
        a = s ? ibits(t, bfe_i(t, a, cu(t, 0), cu(t, 16))) : op2(t, SpvOpBitwiseAnd, t->t_u32, a, cu(t, 0xFFFF));
        b = s ? ibits(t, bfe_i(t, b, cu(t, 0), cu(t, 16))) : op2(t, SpvOpBitwiseAnd, t->t_u32, b, cu(t, 0xFFFF));
        if (!strcmp(cond, "lt")) c = op2(t, s ? SpvOpSLessThan : SpvOpULessThan, B, a, b);
        else if (!strcmp(cond, "eq")) c = op2(t, SpvOpIEqual, B, a, b);
        else if (!strcmp(cond, "le")) c = op2(t, s ? SpvOpSLessThanEqual : SpvOpULessThanEqual, B, a, b);
        else if (!strcmp(cond, "gt")) c = op2(t, s ? SpvOpSGreaterThan : SpvOpUGreaterThan, B, a, b);
        else if (!strcmp(cond, "ne") || !strcmp(cond, "lg")) c = op2(t, SpvOpINotEqual, B, a, b);
        else if (!strcmp(cond, "ge")) c = op2(t, s ? SpvOpSGreaterThanEqual : SpvOpUGreaterThanEqual, B, a, b);
    } else if (!strcmp(type, "i64") || !strcmp(type, "u64")) {
        bool s = type[0] == 'i';
        uint32_t a = vsrc64(t, in, 0), b = vsrc64(t, in, 1); /* S* compares read the bits as signed */
        if (!strcmp(cond, "lt")) c = op2(t, s ? SpvOpSLessThan : SpvOpULessThan, B, a, b);
        else if (!strcmp(cond, "eq")) c = op2(t, SpvOpIEqual, B, a, b);
        else if (!strcmp(cond, "le")) c = op2(t, s ? SpvOpSLessThanEqual : SpvOpULessThanEqual, B, a, b);
        else if (!strcmp(cond, "gt")) c = op2(t, s ? SpvOpSGreaterThan : SpvOpUGreaterThan, B, a, b);
        else if (!strcmp(cond, "ne") || !strcmp(cond, "lg")) c = op2(t, SpvOpINotEqual, B, a, b);
        else if (!strcmp(cond, "ge")) c = op2(t, s ? SpvOpSGreaterThanEqual : SpvOpUGreaterThanEqual, B, a, b);
        else if (!strcmp(cond, "f")) c = spv_const_bool(&t->b, false);
        else if (!strcmp(cond, "t")) c = spv_const_bool(&t->b, true);
    } else if (!strcmp(type, "i32") || !strcmp(type, "u32")) {
        bool s = type[0] == 'i';
        uint32_t a = src(t, in, 0), b = src(t, in, 1);
        if (s) {
            a = ibits(t, a);
            b = ibits(t, b);
        }
        if (!strcmp(cond, "lt")) c = op2(t, s ? SpvOpSLessThan : SpvOpULessThan, B, a, b);
        else if (!strcmp(cond, "eq")) c = op2(t, SpvOpIEqual, B, a, b);
        else if (!strcmp(cond, "le")) c = op2(t, s ? SpvOpSLessThanEqual : SpvOpULessThanEqual, B, a, b);
        else if (!strcmp(cond, "gt")) c = op2(t, s ? SpvOpSGreaterThan : SpvOpUGreaterThan, B, a, b);
        else if (!strcmp(cond, "ne") || !strcmp(cond, "lg")) c = op2(t, SpvOpINotEqual, B, a, b);
        else if (!strcmp(cond, "ge")) c = op2(t, s ? SpvOpSGreaterThanEqual : SpvOpUGreaterThanEqual, B, a, b);
        else if (!strcmp(cond, "f")) c = spv_const_bool(&t->b, false);
        else if (!strcmp(cond, "t")) c = spv_const_bool(&t->b, true);
    }
    if (!c) {
        fail(t, "compare not supported");
        return;
    }
    c = op2(t, SpvOpLogicalAnd, B, c, active(t));
    if (x) set_mask(t, RDNA2_OP_EXEC_LO, c);
    else if (in->enc != RDNA2_ENC_VOPC) set_mask(t, in->dst, c);
    else set_mask(t, t->sdwa && (t->sdwa >> 15) & 1 ? (t->sdwa >> 8) & 0x7F : RDNA2_OP_VCC_LO, c);
}

/* ---- Vector ALU ---- */

/* 16-bit operands: the low or high half of a 32-bit source; inline float
 * constants are f16 encodings for f16 math. */
static uint32_t src16(tr *t, const rdna2_inst *in, int k, bool hi, bool f16) {
    uint32_t code = in->src[k];
    static const uint16_t k_h[9] = {0x3800, 0xB800, 0x3C00, 0xBC00, 0x4000, 0xC000, 0x4400, 0xC400, 0x3118};
    if (f16 && code >= 240 && code <= 248) return cu(t, k_h[code - 240]);
    uint32_t v = src(t, in, k);
    if (code >= 128 && code <= 208) return op2(t, SpvOpBitwiseAnd, t->t_u32, v, cu(t, 0xFFFF)); /* same in both halves */
    if (hi) v = op2(t, SpvOpShiftRightLogical, t->t_u32, v, cu(t, 16));
    return op2(t, SpvOpBitwiseAnd, t->t_u32, v, cu(t, 0xFFFF));
}
static uint32_t h2f(tr *t, uint32_t h) {
    return op2(t, SpvOpCompositeExtract, t->t_f32, ext1(t, t->t_v2f, GLSLstd450UnpackHalf2x16, h), 0);
}
static uint32_t f2h(tr *t, uint32_t f) {
    uint32_t v2 = op2(t, SpvOpCompositeConstruct, t->t_v2f, f, cf(t, 0));
    return op2(t, SpvOpBitwiseAnd, t->t_u32, ext1(t, t->t_u32, GLSLstd450PackHalf2x16, v2), cu(t, 0xFFFF));
}
/* RDNA2 FP16_OVFL, adapted from AnyPS5 0bea402a / clampF16Overflow.
 * 65520 is the round-to-nearest overflow boundary. Computed infinities also
 * saturate, but an infinite source keeps IEEE infinity. NaNs stay NaNs.
 * Keep raw f2h for texture/buffer format packing, which is not an ALU result. */
static uint32_t f16_result(tr *t, uint32_t f, const uint32_t *sources, uint32_t count) {
    if (t->info->fp16_overflow) {
        uint32_t U = t->t_u32, B = t->t_bool, bits = ubits(t, f);
        uint32_t mag = op2(t, SpvOpBitwiseAnd, U, bits, cu(t, 0x7fffffffu));
        uint32_t overflow = op2(t, SpvOpLogicalAnd, B,
            op2(t, SpvOpUGreaterThanEqual, B, mag, cu(t, 0x477ff000u)),
            op2(t, SpvOpULessThanEqual, B, mag, cu(t, 0x7f800000u)));
        for (uint32_t i = 0; i < count; i++)
            overflow = op2(t, SpvOpLogicalAnd, B, overflow,
                           op1(t, SpvOpLogicalNot, B, op1(t, SpvOpIsInf, B, sources[i])));
        uint32_t largest = op2(t, SpvOpBitwiseOr, U,
            op2(t, SpvOpBitwiseAnd, U, bits, cu(t, 0x80000000u)), cu(t, 0x477fe000u));
        f = fbits(t, sel(t, U, overflow, largest, bits));
    }
    return f2h(t, f);
}
/* f16 operand as f32 with abs/neg */
static uint32_t srch(tr *t, const rdna2_inst *in, int k, bool hi, bool abs, bool neg) {
    uint32_t f = h2f(t, src16(t, in, k, hi, true));
    if (t->sdwa && k < 2) {
        /* SDWA selects the half in src(), then applies float modifiers.
         * The VOP3 flags passed by the caller do not carry these bits. */
        abs = (t->sdwa >> (k ? 29 : 21)) & 1;
        neg = (t->sdwa >> (k ? 28 : 20)) & 1;
    }
    if (abs) f = ext1(t, t->t_f32, GLSLstd450FAbs, f);
    if (neg) f = op1(t, SpvOpFNegate, t->t_f32, f);
    return f;
}
/* RDNA2 non-packed 16-bit results preserve the unselected half in either
 * direction. VOP3 op_sel[3] selects the high half. */
static void set_v16(tr *t, const rdna2_inst *in, uint32_t h) {
    uint32_t U = t->t_u32;
    h = op2(t, SpvOpBitwiseAnd, U, h, cu(t, 0xFFFF));
    if (is_vop3(in) && (in->opsel & 8)) {
        uint32_t old = get_v(t, in->dst - RDNA2_OP_VGPR);
        h = op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, old, cu(t, 0xFFFF)), op2(t, SpvOpShiftLeftLogical, U, h, cu(t, 16)));
    } else {
        uint32_t old = get_v(t, in->dst - RDNA2_OP_VGPR);
        h = op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, old, cu(t, 0xFFFF0000u)), h);
    }
    set_vu(t, in, h);
}
static uint32_t clamp01(tr *t, uint32_t f) { return ext3(t, t->t_f32, GLSLstd450FClamp, f, cf(t, 0.0f), cf(t, 1.0f)); }

/* VOP3P packed math: each half of the result from the halves op_sel (low)
 * and op_sel_hi (high) pick; neg / neg_hi negate float operands. */
static void packed(tr *t, const rdna2_inst *in, uint32_t op) {
    uint32_t U = t->t_u32, F = t->t_f32, I = t->t_i32, r[2];
    bool fl = op == SH_OP_V_PK_ADD_F16 || op == SH_OP_V_PK_MUL_F16 || op == SH_OP_V_PK_FMA_F16 || op == SH_OP_V_PK_MIN_F16 ||
              op == SH_OP_V_PK_MAX_F16;
    for (int h = 0; h < 2; h++) {
        uint32_t sel_bits = h ? in->opsel_hi : in->opsel, negb = h ? in->neg_hi : in->neg;
        uint32_t a, b, c = 0;
        if (fl) {
            a = srch(t, in, 0, sel_bits & 1, false, negb & 1);
            b = srch(t, in, 1, (sel_bits >> 1) & 1, false, (negb >> 1) & 1);
            if (op == SH_OP_V_PK_FMA_F16) c = srch(t, in, 2, (sel_bits >> 2) & 1, false, (negb >> 2) & 1);
            uint32_t f = op == SH_OP_V_PK_ADD_F16 ? op2(t, SpvOpFAdd, F, a, b)
                       : op == SH_OP_V_PK_MUL_F16 ? op2(t, SpvOpFMul, F, a, b)
                       : op == SH_OP_V_PK_FMA_F16 ? ext3(t, F, GLSLstd450Fma, a, b, c)
                       : op == SH_OP_V_PK_MIN_F16 ? ext2(t, F, GLSLstd450NMin, a, b)
                                                  : ext2(t, F, GLSLstd450NMax, a, b);
            if (in->clamp) f = clamp01(t, f);
            r[h] = f16_result(t, f, (uint32_t[]){a, b, c}, c ? 3 : 2);
            continue;
        }
        a = src16(t, in, 0, sel_bits & 1, false);
        b = src16(t, in, 1, (sel_bits >> 1) & 1, false);
        if (op == SH_OP_V_PK_MAD_U16 || op == SH_OP_V_PK_MAD_I16) c = src16(t, in, 2, (sel_bits >> 2) & 1, false);
        uint32_t sa = bfe_i(t, a, cu(t, 0), cu(t, 16)), sb = bfe_i(t, b, cu(t, 0), cu(t, 16)), v;
        switch (op) {
        case SH_OP_V_PK_ADD_U16: case SH_OP_V_PK_ADD_I16: v = op2(t, SpvOpIAdd, U, a, b); break;
        case SH_OP_V_PK_SUB_U16: case SH_OP_V_PK_SUB_I16: v = op2(t, SpvOpISub, U, a, b); break;
        case SH_OP_V_PK_MUL_LO_U16: v = op2(t, SpvOpIMul, U, a, b); break;
        case SH_OP_V_PK_MAD_U16: case SH_OP_V_PK_MAD_I16: v = op2(t, SpvOpIAdd, U, op2(t, SpvOpIMul, U, a, b), c); break;
        case SH_OP_V_PK_LSHLREV_B16: v = op2(t, SpvOpShiftLeftLogical, U, b, op2(t, SpvOpBitwiseAnd, U, a, cu(t, 15))); break;
        case SH_OP_V_PK_LSHRREV_B16: v = op2(t, SpvOpShiftRightLogical, U, b, op2(t, SpvOpBitwiseAnd, U, a, cu(t, 15))); break;
        case SH_OP_V_PK_ASHRREV_I16: v = ubits(t, op2(t, SpvOpShiftRightArithmetic, I, ibits(t, sb), op2(t, SpvOpBitwiseAnd, U, a, cu(t, 15)))); break;
        case SH_OP_V_PK_MAX_U16: v = ext2(t, U, GLSLstd450UMax, a, b); break;
        case SH_OP_V_PK_MIN_U16: v = ext2(t, U, GLSLstd450UMin, a, b); break;
        case SH_OP_V_PK_MAX_I16: v = ubits(t, ext2(t, I, GLSLstd450SMax, ibits(t, sa), ibits(t, sb))); break;
        default: v = ubits(t, ext2(t, I, GLSLstd450SMin, ibits(t, sa), ibits(t, sb))); break;
        }
        r[h] = op2(t, SpvOpBitwiseAnd, U, v, cu(t, 0xFFFF));
    }
    set_v(t, in->dst - RDNA2_OP_VGPR, op2(t, SpvOpBitwiseOr, U, r[0], op2(t, SpvOpShiftLeftLogical, U, r[1], cu(t, 16))));
}

/* v_fma_mix*: per operand, op_sel_hi clear = an f32 source, set = the f16
 * half op_sel picks; neg negates and neg_hi takes the absolute value. */
static void fma_mix(tr *t, const rdna2_inst *in, uint32_t op) {
    uint32_t U = t->t_u32, F = t->t_f32, v[3];
    for (int k = 0; k < 3; k++) {
        bool abs = (in->neg_hi >> k) & 1, neg = (in->neg >> k) & 1;
        if ((in->opsel_hi >> k) & 1) {
            v[k] = srch(t, in, k, (in->opsel >> k) & 1, abs, neg);
        } else {
            uint32_t f = fbits(t, src(t, in, k));
            if (abs) f = ext1(t, F, GLSLstd450FAbs, f);
            if (neg) f = op1(t, SpvOpFNegate, F, f);
            v[k] = f;
        }
    }
    uint32_t f = ext3(t, F, GLSLstd450Fma, v[0], v[1], v[2]);
    if (in->clamp) f = clamp01(t, f);
    uint32_t n = in->dst - RDNA2_OP_VGPR;
    if (op == SH_OP_V_FMA_MIX_F32) {
        set_v(t, n, ubits(t, f));
    } else {
        uint32_t h = f16_result(t, f, v, 3), old = get_v(t, n);
        set_v(t, n, op == SH_OP_V_FMA_MIXLO_F16
                        ? op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, old, cu(t, 0xFFFF0000u)), h)
                        : op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, old, cu(t, 0xFFFF)),
                              op2(t, SpvOpShiftLeftLogical, U, h, cu(t, 16))));
    }
}

/* 64-bit vector operands (VGPR or SGPR pairs, inline integers sign-extended) */
static uint32_t vsrc64(tr *t, const rdna2_inst *in, int k) {
    uint32_t code = in->src[k], L = t->t_u64;
    if (code >= RDNA2_OP_VGPR) {
        uint32_t lo = op1(t, SpvOpUConvert, L, get_v(t, code - RDNA2_OP_VGPR));
        uint32_t hi = op1(t, SpvOpUConvert, L, get_v(t, code - RDNA2_OP_VGPR + 1));
        return op2(t, SpvOpBitwiseOr, L, lo, op2(t, SpvOpShiftLeftLogical, L, hi, cu(t, 32)));
    }
    return src64(t, in, code);
}
static void set_v64(tr *t, const rdna2_inst *in, uint32_t v) {
    uint32_t n = in->dst - RDNA2_OP_VGPR, L = t->t_u64;
    set_v(t, n, op1(t, SpvOpUConvert, t->t_u32, v));
    set_v(t, n + 1, op1(t, SpvOpUConvert, t->t_u32, op2(t, SpvOpShiftRightLogical, L, v, cu(t, 32))));
}

/* v_permlane(x)16: each lane reads a lane of its own (permlane16) or the
 * other (permlanex16) row of 16, by a 4-bit select from src1 (lanes 0-7) or
 * src2 (lanes 8-15) */
static uint32_t permlane(tr *t, const rdna2_inst *in, bool cross) {
    uint32_t U = t->t_u32;
    uint32_t lane = t->full_wave64?wave_lane(t):ld(t, U, t->in_sgid), i = op2(t, SpvOpBitwiseAnd, U, lane, cu(t, 15));
    uint32_t sels = sel(t, U, op2(t, SpvOpULessThan, t->t_bool, i, cu(t, 8)), src(t, in, 1), src(t, in, 2));
    uint32_t s = op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpShiftRightLogical, U, sels, op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, i, cu(t, 7)), cu(t, 2))), cu(t, 15));
    uint32_t row = op2(t, SpvOpBitwiseAnd, U, lane, cu(t, ~15u));
    if (cross) row = op2(t, SpvOpBitwiseXor, U, row, cu(t, 16));
    uint32_t idx = op2(t, SpvOpBitwiseOr, U, row, s);
    if(t->full_wave64) return wave_readlane(t,src(t,in,0),idx);
    return spv_op(&t->b, SpvOpGroupNonUniformShuffle, U, (uint32_t[]){cu(t, SpvScopeSubgroup), src(t, in, 0), idx}, 3);
}

/* Legacy multiply: 0 times anything (infinity and NaN too) is 0 */
static uint32_t legacy_mul(tr *t, uint32_t a, uint32_t b) {
    uint32_t B = t->t_bool, F = t->t_f32;
    uint32_t z = op2(t, SpvOpLogicalOr, B, op2(t, SpvOpFOrdEqual, B, a, cf(t, 0)), op2(t, SpvOpFOrdEqual, B, b, cf(t, 0)));
    return sel(t, F, z, cf(t, 0), op2(t, SpvOpFMul, F, a, b));
}

/* f32 (from an f16) as a 16-bit snorm or unorm, rounded to nearest even; NaN gives 0 */
static uint32_t norm16(tr *t, uint32_t f, bool sgn) {
    uint32_t F = t->t_f32, U = t->t_u32;
    f = sel(t, F, op1(t, SpvOpIsNan, t->t_bool, f), cf(t, 0), f);
    f = ext3(t, F, GLSLstd450FClamp, f, cf(t, sgn ? -1.0f : 0.0f), cf(t, 1.0f));
    uint32_t r = ext1(t, F, GLSLstd450RoundEven, op2(t, SpvOpFMul, F, f, cf(t, sgn ? 32767.0f : 65535.0f)));
    r = sgn ? ubits(t, op1(t, SpvOpConvertFToS, t->t_i32, r)) : op1(t, SpvOpConvertFToU, U, r);
    return op2(t, SpvOpBitwiseAnd, U, r, cu(t, 0xFFFF));
}

/* Sum of |a - b| over the `bits`-wide fields of two dwords; masked: only where b's field is not 0 */
static uint32_t field_sad(tr *t, uint32_t a, uint32_t b, uint32_t bits, bool masked) {
    uint32_t U = t->t_u32, B = t->t_bool, sum = cu(t, 0);
    for (uint32_t off = 0; off < 32; off += bits) {
        uint32_t x = op3(t, SpvOpBitFieldUExtract, U, a, cu(t, off), cu(t, bits));
        uint32_t y = op3(t, SpvOpBitFieldUExtract, U, b, cu(t, off), cu(t, bits));
        uint32_t d = sel(t, U, op2(t, SpvOpUGreaterThanEqual, B, x, y), op2(t, SpvOpISub, U, x, y), op2(t, SpvOpISub, U, y, x));
        if (masked) d = sel(t, U, op2(t, SpvOpIEqual, B, y, cu(t, 0)), cu(t, 0), d);
        sum = op2(t, SpvOpIAdd, U, sum, d);
    }
    return sum;
}

/* v_dot{2,4,8}_{i32,u32}_*, v_dot4c_i32_i8: the dot product of the `bits`-wide
 * fields of a and b plus c, accumulated in 64 bits; clamp saturates to 32 */
static uint32_t int_dot(tr *t, uint32_t a, uint32_t b, uint32_t c, uint32_t bits, bool sgn, bool clamp) {
    uint32_t U = t->t_u32, L = t->t_u64, B = t->t_bool;
    uint32_t sum = sgn ? op1(t, SpvOpSConvert, L, ibits(t, c)) : op1(t, SpvOpUConvert, L, c);
    for (uint32_t off = 0; off < 32; off += bits) {
        uint32_t x = sgn ? bfe_i(t, a, cu(t, off), cu(t, bits)) : op3(t, SpvOpBitFieldUExtract, U, a, cu(t, off), cu(t, bits));
        uint32_t y = sgn ? bfe_i(t, b, cu(t, off), cu(t, bits)) : op3(t, SpvOpBitFieldUExtract, U, b, cu(t, off), cu(t, bits));
        uint32_t p = sgn ? op1(t, SpvOpSConvert, L, ibits(t, op2(t, SpvOpIMul, U, x, y)))
                         : op1(t, SpvOpUConvert, L, op2(t, SpvOpIMul, U, x, y));
        sum = op2(t, SpvOpIAdd, L, sum, p);
    }
    if (clamp) {
        if (sgn) {
            uint32_t lo = op1(t, SpvOpSConvert, L, ibits(t, cu(t, 0x80000000u))), hi = op1(t, SpvOpUConvert, L, cu(t, 0x7FFFFFFFu));
            sum = sel(t, L, op2(t, SpvOpSLessThan, B, sum, lo), lo, sel(t, L, op2(t, SpvOpSGreaterThan, B, sum, hi), hi, sum));
        } else {
            uint32_t hi = op1(t, SpvOpUConvert, L, cu(t, 0xFFFFFFFFu));
            sum = sel(t, L, op2(t, SpvOpUGreaterThan, B, sum, hi), hi, sum);
        }
    }
    return op1(t, SpvOpUConvert, U, sum);
}

/* v_div_fixup_f32(quotient, denominator, numerator): the quotient's magnitude with the
 * true sign, and the IEEE results of the special cases a reciprocal does not give */
static uint32_t div_fixup_f32(tr *t, uint32_t q, uint32_t den, uint32_t num) {
    uint32_t U = t->t_u32, B = t->t_bool;
#define MAG(x) op2(t, SpvOpBitwiseAnd, U, x, cu(t, 0x7FFFFFFFu))
#define IS_NAN(x) op2(t, SpvOpUGreaterThan, B, MAG(x), cu(t, 0x7F800000u))
#define IS_INF(x) op2(t, SpvOpIEqual, B, MAG(x), cu(t, 0x7F800000u))
#define IS_ZERO(x) op2(t, SpvOpIEqual, B, MAG(x), cu(t, 0))
#define QUIET(x) op2(t, SpvOpBitwiseOr, U, x, cu(t, 0x00400000u))
    uint32_t sign = op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpBitwiseXor, U, den, num), cu(t, 0x80000000u));
    uint32_t inf = op2(t, SpvOpBitwiseOr, U, sign, cu(t, 0x7F800000u));
    uint32_t ediff = op2(t, SpvOpISub, U, op3(t, SpvOpBitFieldUExtract, U, num, cu(t, 23), cu(t, 8)),
                         op3(t, SpvOpBitFieldUExtract, U, den, cu(t, 23), cu(t, 8)));
    uint32_t r = op2(t, SpvOpBitwiseOr, U, sign, MAG(q));
    r = sel(t, U, IS_NAN(q), inf, r);
    r = sel(t, U, op2(t, SpvOpSLessThan, B, ibits(t, ediff), ibits(t, cu(t, (uint32_t)-150))), sign, r);
    r = sel(t, U, op2(t, SpvOpLogicalOr, B, IS_INF(den), IS_ZERO(num)), sign, r);
    r = sel(t, U, op2(t, SpvOpLogicalOr, B, IS_ZERO(den), IS_INF(num)), inf, r);
    uint32_t invalid = op2(t, SpvOpLogicalOr, B, op2(t, SpvOpLogicalAnd, B, IS_ZERO(den), IS_ZERO(num)),
                           op2(t, SpvOpLogicalAnd, B, IS_INF(den), IS_INF(num)));
    r = sel(t, U, invalid, cu(t, 0xFFC00000u), r);
    r = sel(t, U, IS_NAN(den), QUIET(den), r);
    r = sel(t, U, IS_NAN(num), QUIET(num), r);
#undef MAG
#undef IS_NAN
#undef IS_INF
#undef IS_ZERO
#undef QUIET
    return r;
}

/* ---- f64 without 64-bit float on the host GPU ----
 * Values are their bits in a u64. Conversions, rounding, frexp, ldexp, min and max
 * are exact integer code; arithmetic (add, mul, fma, rcp, rsq, sqrt, division)
 * runs in f32 on the converted operands, as close as a GPU without float64 gets. */
#define F64_MANT 0x000FFFFFFFFFFFFFull
static uint32_t u64_of(tr *t, uint32_t u32) { return op1(t, SpvOpUConvert, t->t_u64, u32); }
static uint32_t u32_of(tr *t, uint32_t u64) { return op1(t, SpvOpUConvert, t->t_u32, u64); }
static uint32_t ci32(tr *t, int32_t v) { return spv_const_i32(&t->b, v); }
static uint32_t f64_exp(tr *t, uint32_t b) { /* biased exponent, u32 */
    return op2(t, SpvOpBitwiseAnd, t->t_u32, u32_of(t, op2(t, SpvOpShiftRightLogical, t->t_u64, b, cu(t, 52))), cu(t, 0x7FF));
}
static uint32_t f64_sign(tr *t, uint32_t b) { return op2(t, SpvOpBitwiseAnd, t->t_u64, b, c64(t, 0x8000000000000000ull)); }
static uint32_t f64_mag(tr *t, uint32_t b) { return op2(t, SpvOpBitwiseAnd, t->t_u64, b, c64(t, 0x7FFFFFFFFFFFFFFFull)); }
static uint32_t f64_mant(tr *t, uint32_t b) { return op2(t, SpvOpBitwiseAnd, t->t_u64, b, c64(t, F64_MANT)); }
static uint32_t f64_unbiased(tr *t, uint32_t b) { return op2(t, SpvOpISub, t->t_i32, ibits(t, f64_exp(t, b)), ci32(t, 1023)); }
static uint32_t bsel(tr *t, uint32_t c) { return sel(t, t->t_u64, c, c64(t, 1), c64(t, 0)); }
/* round-to-nearest-even increment: rem against half, r's low bit */
static uint32_t round_up(tr *t, uint32_t rem, uint32_t half, uint32_t r, uint32_t ty) {
    uint32_t B = t->t_bool, one = ty == t->t_u64 ? c64(t, 1) : cu(t, 1), zero = ty == t->t_u64 ? c64(t, 0) : cu(t, 0);
    return op2(t, SpvOpLogicalOr, B, op2(t, SpvOpUGreaterThan, B, rem, half),
               op2(t, SpvOpLogicalAnd, B, op2(t, SpvOpIEqual, B, rem, half),
                   op2(t, SpvOpINotEqual, B, op2(t, SpvOpBitwiseAnd, ty, r, one), zero)));
}
/* highest set bit of a nonzero u64 */
static uint32_t msb64(tr *t, uint32_t v) {
    uint32_t U = t->t_u32, hi = u32_of(t, op2(t, SpvOpShiftRightLogical, t->t_u64, v, cu(t, 32))), lo = u32_of(t, v);
    uint32_t mh = ext1(t, U, GLSLstd450FindUMsb, hi), ml = ext1(t, U, GLSLstd450FindUMsb, lo);
    return sel(t, U, op2(t, SpvOpINotEqual, t->t_bool, hi, cu(t, 0)), op2(t, SpvOpIAdd, U, mh, cu(t, 32)), ml);
}
/* v * 2^scale as f64 bits (v nonzero, rounded to nearest even; the result a normal double) */
static uint32_t u64_to_f64_scaled(tr *t, uint32_t v, uint32_t scale /* i32 */) {
    uint32_t U = t->t_u32, L = t->t_u64, B = t->t_bool;
    uint32_t m = msb64(t, v);
    uint32_t small = op2(t, SpvOpULessThanEqual, B, m, cu(t, 52));
    uint32_t mant_l = op2(t, SpvOpBitwiseAnd, L, op2(t, SpvOpShiftLeftLogical, L, v, op2(t, SpvOpISub, U, cu(t, 52), m)), c64(t, F64_MANT));
    uint32_t shr = sel(t, U, small, cu(t, 1), op2(t, SpvOpISub, U, m, cu(t, 52)));
    uint32_t r = op2(t, SpvOpShiftRightLogical, L, v, shr);
    uint32_t rem = op2(t, SpvOpBitwiseAnd, L, v, op2(t, SpvOpISub, L, op2(t, SpvOpShiftLeftLogical, L, c64(t, 1), shr), c64(t, 1)));
    uint32_t half = op2(t, SpvOpShiftLeftLogical, L, c64(t, 1), op2(t, SpvOpISub, U, shr, cu(t, 1)));
    uint32_t up = round_up(t, rem, half, r, L);
    uint32_t e = ubits(t, op2(t, SpvOpIAdd, t->t_i32, op2(t, SpvOpIAdd, t->t_i32, ibits(t, m), scale), ci32(t, 1023)));
    uint32_t ebits = op2(t, SpvOpShiftLeftLogical, L, u64_of(t, e), cu(t, 52));
    uint32_t base_l = op2(t, SpvOpBitwiseOr, L, ebits, mant_l);
    /* a rounding carry moves into the exponent field by itself */
    uint32_t base_r = op2(t, SpvOpIAdd, L, op2(t, SpvOpBitwiseOr, L, ebits, op2(t, SpvOpBitwiseAnd, L, r, c64(t, F64_MANT))), bsel(t, up));
    return sel(t, L, small, base_l, base_r);
}
static uint32_t u32_to_f64(tr *t, uint32_t u) {
    uint32_t v = u64_to_f64_scaled(t, u64_of(t, u), ci32(t, 0));
    return sel(t, t->t_u64, op2(t, SpvOpIEqual, t->t_bool, u, cu(t, 0)), c64(t, 0), v);
}
static uint32_t f32_to_f64(tr *t, uint32_t x /* f32 bits */) {
    uint32_t U = t->t_u32, L = t->t_u64, B = t->t_bool;
    uint32_t sign = op2(t, SpvOpShiftLeftLogical, L, u64_of(t, op2(t, SpvOpBitwiseAnd, U, x, cu(t, 0x80000000u))), cu(t, 32));
    uint32_t e = op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpShiftRightLogical, U, x, cu(t, 23)), cu(t, 0xFF));
    uint32_t m = op2(t, SpvOpBitwiseAnd, U, x, cu(t, 0x7FFFFF));
    uint32_t m29 = op2(t, SpvOpShiftLeftLogical, L, u64_of(t, m), cu(t, 29));
    uint32_t normal = op2(t, SpvOpBitwiseOr, L, op2(t, SpvOpShiftLeftLogical, L, u64_of(t, op2(t, SpvOpIAdd, U, e, cu(t, 896))), cu(t, 52)), m29);
    uint32_t infnan = op2(t, SpvOpBitwiseOr, L, c64(t, 0x7FF0000000000000ull), m29);
    uint32_t den = u64_to_f64_scaled(t, u64_of(t, sel(t, U, op2(t, SpvOpIEqual, B, m, cu(t, 0)), cu(t, 1), m)), ci32(t, -149));
    uint32_t r = sel(t, L, op2(t, SpvOpIEqual, B, e, cu(t, 0xFF)), infnan, normal);
    r = sel(t, L, op2(t, SpvOpIEqual, B, e, cu(t, 0)), sel(t, L, op2(t, SpvOpIEqual, B, m, cu(t, 0)), c64(t, 0), den), r);
    return op2(t, SpvOpBitwiseOr, L, r, sign);
}
/* f64 bits -> f32 bits, round to nearest even */
static uint32_t f64_to_f32(tr *t, uint32_t b) {
    uint32_t U = t->t_u32, I = t->t_i32, L = t->t_u64, B = t->t_bool;
    uint32_t sign = op2(t, SpvOpBitwiseAnd, U, u32_of(t, op2(t, SpvOpShiftRightLogical, L, b, cu(t, 32))), cu(t, 0x80000000u));
    uint32_t e = f64_exp(t, b), m = f64_mant(t, b), E = f64_unbiased(t, b);
    /* normal f32 */
    uint32_t r = u32_of(t, op2(t, SpvOpShiftRightLogical, L, m, cu(t, 29)));
    uint32_t rem = u32_of(t, op2(t, SpvOpBitwiseAnd, L, m, c64(t, 0x1FFFFFFF)));
    uint32_t up = round_up(t, rem, cu(t, 0x10000000), r, U);
    uint32_t e32 = ubits(t, op2(t, SpvOpIAdd, I, E, ci32(t, 127)));
    uint32_t nv = op2(t, SpvOpIAdd, U, op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpShiftLeftLogical, U, e32, cu(t, 23)), r), sel(t, U, up, cu(t, 1), cu(t, 0)));
    /* denormal f32: the mantissa with its leading bit, shifted right by (-126 - E) + 29 */
    uint32_t M = op2(t, SpvOpBitwiseOr, L, m, c64(t, 1ull << 52));
    uint32_t sh = ubits(t, op2(t, SpvOpISub, I, ci32(t, -126 + 29), E));
    uint32_t shc = ext2(t, U, GLSLstd450UMin, sh, cu(t, 63));
    uint32_t r2 = u32_of(t, op2(t, SpvOpShiftRightLogical, L, M, shc));
    uint32_t rem2 = op2(t, SpvOpBitwiseAnd, L, M, op2(t, SpvOpISub, L, op2(t, SpvOpShiftLeftLogical, L, c64(t, 1), shc), c64(t, 1)));
    uint32_t half2 = op2(t, SpvOpShiftLeftLogical, L, c64(t, 1), op2(t, SpvOpISub, U, shc, cu(t, 1)));
    uint32_t up2 = round_up(t, rem2, half2, u64_of(t, r2), L);
    uint32_t dv = sel(t, U, op2(t, SpvOpUGreaterThan, B, sh, cu(t, 54)), cu(t, 0), op2(t, SpvOpIAdd, U, r2, sel(t, U, up2, cu(t, 1), cu(t, 0))));
    uint32_t res = sel(t, U, op2(t, SpvOpSLessThan, B, E, ci32(t, -126)), dv, nv);
    res = sel(t, U, op2(t, SpvOpSGreaterThan, B, E, ci32(t, 127)), cu(t, 0x7F800000), res);
    res = sel(t, U, op2(t, SpvOpIEqual, B, e, cu(t, 0)), cu(t, 0), res);
    uint32_t nan = op2(t, SpvOpBitwiseOr, U, cu(t, 0x7FC00000), u32_of(t, op2(t, SpvOpShiftRightLogical, L, m, cu(t, 29))));
    uint32_t special = sel(t, U, op2(t, SpvOpIEqual, B, m, c64(t, 0)), cu(t, 0x7F800000), nan);
    res = sel(t, U, op2(t, SpvOpIEqual, B, e, cu(t, 0x7FF)), special, res);
    return op2(t, SpvOpBitwiseOr, U, res, sign);
}
/* |x| truncated to an integer (u64, for |x| < 2^63); E the unbiased exponent */
static uint32_t f64_int_mag(tr *t, uint32_t b, uint32_t E) {
    uint32_t U = t->t_u32, I = t->t_i32, L = t->t_u64, B = t->t_bool;
    uint32_t M = op2(t, SpvOpBitwiseOr, L, f64_mant(t, b), c64(t, 1ull << 52));
    uint32_t le52 = op2(t, SpvOpSLessThanEqual, B, E, ci32(t, 52));
    uint32_t rs = ext2(t, U, GLSLstd450UMin, ubits(t, op2(t, SpvOpISub, I, ci32(t, 52), E)), cu(t, 63));
    uint32_t ls = ext2(t, U, GLSLstd450UMin, ubits(t, op2(t, SpvOpISub, I, E, ci32(t, 52))), cu(t, 63));
    uint32_t v = sel(t, L, le52, op2(t, SpvOpShiftRightLogical, L, M, rs), op2(t, SpvOpShiftLeftLogical, L, M, ls));
    return sel(t, L, op2(t, SpvOpSLessThan, B, E, ci32(t, 0)), c64(t, 0), v);
}
static uint32_t f64_is_nan(tr *t, uint32_t b) {
    return op2(t, SpvOpUGreaterThan, t->t_bool, f64_mag(t, b), c64(t, 0x7FF0000000000000ull));
}
static uint32_t f64_neg_bit(tr *t, uint32_t b) { return op2(t, SpvOpINotEqual, t->t_bool, f64_sign(t, b), c64(t, 0)); }
/* f64 -> i32 (toward zero, saturating, NaN 0) or u32 */
static uint32_t f64_to_int(tr *t, uint32_t b, bool is_signed) {
    uint32_t U = t->t_u32, B = t->t_bool, E = f64_unbiased(t, b);
    uint32_t m32 = u32_of(t, f64_int_mag(t, b, E)), neg = f64_neg_bit(t, b), r;
    if (is_signed) {
        uint32_t big = op2(t, SpvOpSGreaterThanEqual, B, E, ci32(t, 31));
        r = sel(t, U, neg, op2(t, SpvOpISub, U, cu(t, 0), m32), m32);
        r = sel(t, U, big, sel(t, U, neg, cu(t, 0x80000000u), cu(t, 0x7FFFFFFFu)), r);
    } else {
        uint32_t big = op2(t, SpvOpSGreaterThanEqual, B, E, ci32(t, 32));
        r = sel(t, U, big, cu(t, 0xFFFFFFFFu), m32);
        r = sel(t, U, neg, cu(t, 0), r);
    }
    return sel(t, U, f64_is_nan(t, b), cu(t, 0), r);
}
/* the fraction of |x| below its integer part, in units 2^(E-52) (|x| < 2^52), and one unit of 1.0 */
static void f64_frac_units(tr *t, uint32_t b, uint32_t E, uint32_t *frac, uint32_t *one) {
    uint32_t I = t->t_i32, L = t->t_u64, B = t->t_bool;
    uint32_t M = op2(t, SpvOpBitwiseOr, L, f64_mant(t, b), c64(t, 1ull << 52));
    uint32_t ge0 = op2(t, SpvOpSGreaterThanEqual, B, E, ci32(t, 0));
    uint32_t sh = ext2(t, t->t_u32, GLSLstd450UMin, ubits(t, op2(t, SpvOpISub, I, ci32(t, 52), E)), cu(t, 63));
    *one = op2(t, SpvOpShiftLeftLogical, L, c64(t, 1), sh);
    *frac = sel(t, L, ge0, op2(t, SpvOpBitwiseAnd, L, M, op2(t, SpvOpISub, L, *one, c64(t, 1))), M);
}
/* trunc/floor/ceil/rndne: kind 0..3 */
static uint32_t f64_round(tr *t, uint32_t b, int kind) {
    uint32_t L = t->t_u64, B = t->t_bool, E = f64_unbiased(t, b);
    uint32_t n = f64_int_mag(t, b, E), neg = f64_neg_bit(t, b), mag = f64_mag(t, b);
    uint32_t lt1 = op2(t, SpvOpSLessThan, B, E, ci32(t, 0)), frac, one;
    f64_frac_units(t, b, E, &frac, &one);
    uint32_t has_frac = op2(t, SpvOpINotEqual, B, frac, c64(t, 0)), r = n;
    if (kind == 1 || kind == 2) { /* floor/ceil: away from zero when the fraction points that way */
        uint32_t away = kind == 1 ? neg : op1(t, SpvOpLogicalNot, B, neg);
        r = sel(t, L, op2(t, SpvOpLogicalAnd, B, away, has_frac), op2(t, SpvOpIAdd, L, n, c64(t, 1)), n);
    } else if (kind == 3) { /* half to even; below 1.0 the integer part is 0 (even) */
        uint32_t half = op2(t, SpvOpShiftRightLogical, L, one, cu(t, 1));
        uint32_t up = round_up(t, frac, half, n, L);
        /* |x| < 0.5 rounds to 0: E <= -2 has frac < half automatically except via the units (all of M) */
        up = op2(t, SpvOpLogicalAnd, B, up, op1(t, SpvOpLogicalNot, B, op2(t, SpvOpSLessThan, B, E, ci32(t, -1))));
        r = sel(t, L, up, op2(t, SpvOpIAdd, L, n, c64(t, 1)), n);
    }
    (void)lt1;
    uint32_t v = sel(t, L, op2(t, SpvOpIEqual, B, r, c64(t, 0)), c64(t, 0), u64_to_f64_scaled(t, sel(t, L, op2(t, SpvOpIEqual, B, r, c64(t, 0)), c64(t, 1), r), ci32(t, 0)));
    v = op2(t, SpvOpBitwiseOr, L, v, f64_sign(t, b)); /* -0 for negatives rounding to zero */
    uint32_t whole = op2(t, SpvOpLogicalOr, B, op2(t, SpvOpSGreaterThanEqual, B, E, ci32(t, 52)),
                         op2(t, SpvOpIEqual, B, mag, c64(t, 0)));
    return sel(t, L, whole, b, v); /* integers, inf, NaN and zeros as they are */
}
/* fract: x - floor(x), in [0, 1) */
static uint32_t f64_fract(tr *t, uint32_t b) {
    uint32_t I = t->t_i32, L = t->t_u64, B = t->t_bool, E = f64_unbiased(t, b);
    uint32_t neg = f64_neg_bit(t, b), mag = f64_mag(t, b), frac, one;
    f64_frac_units(t, b, E, &frac, &one);
    uint32_t fz = op2(t, SpvOpIEqual, B, frac, c64(t, 0));
    uint32_t scale = op2(t, SpvOpISub, I, E, ci32(t, 52));
    uint32_t safe = sel(t, L, fz, c64(t, 1), frac);
    uint32_t pos_v = u64_to_f64_scaled(t, safe, scale);
    uint32_t neg_v = u64_to_f64_scaled(t, sel(t, L, fz, c64(t, 1), op2(t, SpvOpISub, L, one, frac)), scale);
    uint32_t below1 = c64(t, 0x3FEFFFFFFFFFFFFFull);
    uint32_t r = sel(t, L, neg, neg_v, pos_v);
    r = sel(t, L, fz, c64(t, 0), r);
    /* |x| < 1: positives are themselves; tiny negatives round 1 - |x| up to 1 */
    uint32_t lt1 = op2(t, SpvOpSLessThan, B, E, ci32(t, 0));
    r = sel(t, L, op2(t, SpvOpLogicalAnd, B, op1(t, SpvOpLogicalNot, B, neg), lt1), mag, r);
    r = sel(t, L, op2(t, SpvOpLogicalAnd, B, neg, op2(t, SpvOpSLessThan, B, E, ci32(t, -11))), below1, r);
    r = ext2(t, L, GLSLstd450UMin, r, below1);
    r = sel(t, L, op2(t, SpvOpLogicalOr, B, op2(t, SpvOpSGreaterThanEqual, B, E, ci32(t, 52)), op2(t, SpvOpIEqual, B, mag, c64(t, 0))),
            c64(t, 0), r);
    /* inf and NaN: NaN */
    return sel(t, L, op2(t, SpvOpIEqual, B, f64_exp(t, b), cu(t, 0x7FF)), c64(t, 0x7FF8000000000000ull), r);
}
/* the mantissa scaled to [0.5, 1) (exponent field 1022) and the exponent; zeros, inf and NaN as
 * they are with exponent 0 */
static void f64_frexp(tr *t, uint32_t b, uint32_t *mant, uint32_t *exp) {
    uint32_t U = t->t_u32, I = t->t_i32, L = t->t_u64, B = t->t_bool;
    uint32_t e = f64_exp(t, b), m = f64_mant(t, b);
    uint32_t den = op2(t, SpvOpIEqual, B, e, cu(t, 0));
    uint32_t mm = msb64(t, sel(t, L, op2(t, SpvOpIEqual, B, m, c64(t, 0)), c64(t, 1), m));
    uint32_t sh = op2(t, SpvOpISub, U, cu(t, 52), mm);
    uint32_t dm = op2(t, SpvOpBitwiseAnd, L, op2(t, SpvOpShiftLeftLogical, L, m, sh), c64(t, F64_MANT));
    uint32_t mant_bits = sel(t, L, den, dm, m);
    uint32_t ex = sel(t, I, den, op2(t, SpvOpISub, I, ci32(t, -1021), ibits(t, sh)), op2(t, SpvOpISub, I, ibits(t, e), ci32(t, 1022)));
    uint32_t special = op2(t, SpvOpLogicalOr, B, op2(t, SpvOpIEqual, B, e, cu(t, 0x7FF)), op2(t, SpvOpIEqual, B, f64_mag(t, b), c64(t, 0)));
    *mant = sel(t, L, special, b, op2(t, SpvOpBitwiseOr, L, op2(t, SpvOpBitwiseOr, L, f64_sign(t, b), c64(t, 1022ull << 52)), mant_bits));
    *exp = sel(t, U, special, cu(t, 0), ubits(t, ex));
}
/* x * 2^n */
static uint32_t f64_ldexp(tr *t, uint32_t b, uint32_t n /* i32 */) {
    uint32_t I = t->t_i32, L = t->t_u64, B = t->t_bool, mant, ex;
    f64_frexp(t, b, &mant, &ex);
    uint32_t fe = op2(t, SpvOpIAdd, I, op2(t, SpvOpIAdd, I, ibits(t, ex), n), ci32(t, 1022));
    uint32_t sign = f64_sign(t, b), m = f64_mant(t, mant);
    uint32_t normal = op2(t, SpvOpBitwiseOr, L, op2(t, SpvOpBitwiseOr, L, sign, op2(t, SpvOpShiftLeftLogical, L, u64_of(t, ubits(t, fe)), cu(t, 52))), m);
    /* below the normal range: a denormal (truncated) or zero */
    uint32_t M = op2(t, SpvOpBitwiseOr, L, m, c64(t, 1ull << 52));
    uint32_t sh = ext2(t, t->t_u32, GLSLstd450UMin, ubits(t, op2(t, SpvOpISub, I, ci32(t, 1), fe)), cu(t, 63));
    uint32_t dv = op2(t, SpvOpBitwiseOr, L, sign, op2(t, SpvOpShiftRightLogical, L, M, sh));
    uint32_t r = sel(t, L, op2(t, SpvOpSLessThan, B, fe, ci32(t, 1)), dv, normal);
    r = sel(t, L, op2(t, SpvOpSGreaterThanEqual, B, fe, ci32(t, 0x7FF)), op2(t, SpvOpBitwiseOr, L, sign, c64(t, 0x7FF0000000000000ull)), r);
    uint32_t special = op2(t, SpvOpLogicalOr, B, op2(t, SpvOpIEqual, B, f64_exp(t, b), cu(t, 0x7FF)), op2(t, SpvOpIEqual, B, f64_mag(t, b), c64(t, 0)));
    return sel(t, L, special, b, r);
}
/* an f64 source of a VALU instruction: float inline constants are doubles, a literal is
 * the high half, VOP3 abs/neg act on the sign */
static uint32_t vsrc_f64(tr *t, const rdna2_inst *in, int j) {
    static const uint64_t k_f64[9] = {0x3FE0000000000000ull, 0xBFE0000000000000ull, 0x3FF0000000000000ull,
                                      0xBFF0000000000000ull, 0x4000000000000000ull, 0xC000000000000000ull,
                                      0x4010000000000000ull, 0xC010000000000000ull, 0x3FC45F306DC9C882ull};
    uint32_t L = t->t_u64, code = in->src[j];
    uint32_t v = code >= 240 && code <= 248 ? c64(t, k_f64[code - 240])
               : code == RDNA2_OP_LITERAL ? c64(t, (uint64_t)in->literal << 32) : vsrc64(t, in, j);
    if (is_vop3(in) && ((in->abs >> j) & 1)) v = op2(t, SpvOpBitwiseAnd, L, v, c64(t, 0x7FFFFFFFFFFFFFFFull));
    if (is_vop3(in) && ((in->neg >> j) & 1)) v = op2(t, SpvOpBitwiseXor, L, v, c64(t, 0x8000000000000000ull));
    return v;
}
/* an f64 operand as f32 (the arithmetic fallback) */
static uint32_t f64_as_f32(tr *t, const rdna2_inst *in, int j) { return fbits(t, f64_to_f32(t, vsrc_f64(t, in, j))); }
static void set_vf64(tr *t, const rdna2_inst *in, uint32_t bits) {
    if (in->clamp && is_vop3(in)) { /* [0, 1] on the bits: negatives and NaN to 0, above 1.0 to 1.0 */
        uint32_t L = t->t_u64, B = t->t_bool;
        bits = sel(t, L, op2(t, SpvOpLogicalOr, B, f64_neg_bit(t, bits), f64_is_nan(t, bits)), c64(t, 0),
                   ext2(t, L, GLSLstd450UMin, bits, c64(t, 0x3FF0000000000000ull)));
    }
    set_v64(t, in, bits);
}
static void set_vf64_from_f32(tr *t, const rdna2_inst *in, uint32_t f) { set_vf64(t, in, f32_to_f64(t, ubits(t, f))); }

/* v_min / v_max: NaN-avoiding, and -0 below +0 as on the GPU (NMin/NMax may return either zero) */
static uint32_t fminmax(tr *t, uint32_t a, uint32_t b, bool mx) {
    uint32_t F = t->t_f32, U = t->t_u32, B = t->t_bool;
    uint32_t r = ext2(t, F, mx ? GLSLstd450NMax : GLSLstd450NMin, a, b);
    uint32_t zeros = op2(t, SpvOpLogicalAnd, B, op2(t, SpvOpFOrdEqual, B, a, cf(t, 0.0f)), op2(t, SpvOpFOrdEqual, B, b, cf(t, 0.0f)));
    uint32_t z = op2(t, mx ? SpvOpBitwiseAnd : SpvOpBitwiseOr, U, ubits(t, a), ubits(t, b));
    return sel(t, F, zeros, fbits(t, z), r);
}

/* v_movrel*: the register base + m0 for an index in [lo, hi), by binary search.
 * M0 is uniform across a wave, so a wave takes one path of a few branches and
 * touches one register; a select over all 64 kept them all live (Little
 * Nightmares' radix sort spent 47 ms per dispatch on Iris Xe that way).
 * Writes store v there (active lanes); reads copy it into movrel_tmp. */
static void movrel_tree(tr *t, uint32_t m0, uint32_t lo, uint32_t hi, uint32_t base, uint32_t v, bool write) {
    if (hi - lo == 1) {
        if (write) set_v(t, base + lo, v);
        else st(t, t->movrel_tmp, get_v(t, base + lo));
        return;
    }
    uint32_t mid = (lo + hi) / 2, below = op2(t, SpvOpULessThan, t->t_bool, m0, cu(t, mid));
    uint32_t m = begin_if(t, below);
    movrel_tree(t, m0, lo, mid, base, v, write);
    end_if(t, m);
    m = begin_if(t, op1(t, SpvOpLogicalNot, t->t_bool, below));
    movrel_tree(t, m0, mid, hi, base, v, write);
    end_if(t, m);
}
/* M0 addresses the full VGPR file; the wave size does not limit the index. */
static void movrel(tr *t, uint32_t m0, uint32_t base, uint32_t v, bool write) {
    uint32_t count = 256 - base;
    uint32_t m = begin_if(t, op2(t, SpvOpULessThan, t->t_bool, m0, cu(t, count)));
    movrel_tree(t, m0, 0, count, base, v, write);
    end_if(t, m);
}

static void valu(tr *t, const rdna2_inst *in, uint32_t op) {
    uint32_t U = t->t_u32, F = t->t_f32, I = t->t_i32, B = t->t_bool;
#define FA() src_f(t, in, 0)
#define FB() src_f(t, in, 1)
#define FC() src_f(t, in, 2)
#define UA() src(t, in, 0)
#define UB() src(t, in, 1)
#define UC() src(t, in, 2)
#define F2(OPC) set_vf(t, in, op2(t, OPC, F, FA(), FB())); break;
#define U2(OPC) set_vu(t, in, op2(t, OPC, U, UA(), UB())); break;
#define FX1(G) set_vf(t, in, ext1(t, F, G, FA())); break;
    switch (op) {
    case SH_OP_V_NOP: break;
    case SH_OP_V_MOV_B32:
        /* SDWA abs/neg on a move act on the sign bit, as on a float */
        if (t->sdwa && (t->sdwa & ((1u << 20) | (1u << 21)))) set_vu(t, in, ubits(t, FA()));
        else set_vu(t, in, UA());
        break;
    case SH_OP_V_READFIRSTLANE_B32: {
        uint32_t value=UA();
        if(t->full_wave32) {
            uint32_t mask=get_s(t,RDNA2_OP_EXEC_LO);
            uint32_t first=sel(t,U,op2(t,SpvOpINotEqual,B,mask,cu(t,0)),ubits(t,ext1(t,I,GLSLstd450FindILsb,mask)),cu(t,0));
            value=spv_op(&t->b,SpvOpGroupNonUniformShuffle,U,(uint32_t[]){cu(t,SpvScopeSubgroup),value,first},3);
        } else if(t->full_wave64) {
            uint32_t lo=get_s(t,RDNA2_OP_EXEC_LO),hi=get_s(t,RDNA2_OP_EXEC_HI);
            uint32_t first=sel(t,U,op2(t,SpvOpINotEqual,B,lo,cu(t,0)),
                ubits(t,ext1(t,I,GLSLstd450FindILsb,lo)),
                op2(t,SpvOpIAdd,U,cu(t,32),ubits(t,ext1(t,I,GLSLstd450FindILsb,hi))));
            value=wave_readlane(t,value,first);
        }
        set_s(t,(in->dst-RDNA2_OP_VGPR)&0x7f,value);break;
    }
    case SH_OP_V_MOVRELS_B32: case SH_OP_V_MOVRELD_B32: case SH_OP_V_MOVRELSD_B32: {
        uint32_t m0 = get_s(t, RDNA2_OP_M0), sbase = (in->src[0] - RDNA2_OP_VGPR) & 0xFF;
        uint32_t dbase = (in->dst - RDNA2_OP_VGPR) & 0xFF;
        if (t->vgpr_array) {
            uint32_t v;
            if (op == SH_OP_V_MOVRELD_B32) v = UA();
            else {
                uint32_t valid = op2(t, SpvOpULessThan, B, m0, cu(t, 256 - sbase));
                uint32_t index = op2(t, SpvOpIAdd, U, cu(t, sbase), sel(t, U, valid, m0, cu(t, 0)));
                v = ld(t, U, op2(t, SpvOpAccessChain, t->p_fn_u32, t->vgpr_array, index));
            }
            if (op == SH_OP_V_MOVRELS_B32) set_v(t, dbase, v);
            else {
                uint32_t valid = op2(t, SpvOpULessThan, B, m0, cu(t, 256 - dbase));
                uint32_t merge = begin_if(t, op2(t, SpvOpLogicalAnd, B, active(t), valid));
                uint32_t index = op2(t, SpvOpIAdd, U, cu(t, dbase), m0);
                st(t, op2(t, SpvOpAccessChain, t->p_fn_u32, t->vgpr_array, index), v);
                end_if(t, merge);
            }
            break;
        }
        uint32_t v = UA();
        if (op != SH_OP_V_MOVRELD_B32) {
            if (!t->movrel_tmp) t->movrel_tmp = spv_local_var(&t->b, t->p_fn_u32);
            st(t, t->movrel_tmp, get_v(t, sbase)); /* an index past the registers reads the first */
            movrel(t, m0, sbase, 0, false);
            v = ld(t, U, t->movrel_tmp);
        }
        if (op == SH_OP_V_MOVRELS_B32) {
            set_v(t, dbase, v);
            break;
        }
        movrel(t, m0, dbase, v, true);
        break;
    }
    case SH_OP_V_CNDMASK_B32: {
        uint32_t m = is_vop3(in) ? src_u(t, in, in->src[2]) : get_s(t, RDNA2_OP_VCC_LO);
        uint32_t a = UA(), b = UB();
        if (is_vop3(in) && (in->abs | in->neg)) {
            a = ubits(t, FA());
            b = ubits(t, FB());
        }
        uint32_t code=is_vop3(in)?in->src[2]:RDNA2_OP_VCC_LO;
        uint32_t cond=(t->full_wave64 || t->full_wave32) ? vector_mask(t,in,code) : bit0(t,m);
        set_vu(t, in, sel(t, U, cond, b, a));
        break;
    }
    case SH_OP_V_ADD_F32: F2(SpvOpFAdd)
    case SH_OP_V_SUB_F32: F2(SpvOpFSub)
    case SH_OP_V_SUBREV_F32: set_vf(t, in, op2(t, SpvOpFSub, F, FB(), FA())); break;
    case SH_OP_V_MUL_F32: F2(SpvOpFMul)
    case SH_OP_V_MUL_LEGACY_F32: {
        uint32_t a = FA(), b = FB();
        uint32_t z = op2(t, SpvOpLogicalOr, B, op2(t, SpvOpFOrdEqual, B, a, cf(t, 0)), op2(t, SpvOpFOrdEqual, B, b, cf(t, 0)));
        set_vf(t, in, sel(t, F, z, cf(t, 0), op2(t, SpvOpFMul, F, a, b)));
        break;
    }
    case SH_OP_V_MIN_F32: set_vf(t, in, fminmax(t, FA(), FB(), false)); break;
    case SH_OP_V_MAX_F32: set_vf(t, in, fminmax(t, FA(), FB(), true)); break;
    case SH_OP_V_MAC_F32: {
        uint32_t acc = fbits(t, get_v(t, in->dst - RDNA2_OP_VGPR));
        set_vf(t, in, op2(t, SpvOpFAdd, F, op2(t, SpvOpFMul, F, FA(), FB()), acc));
        break;
    }
    case SH_OP_V_FMAC_F32: {
        uint32_t acc = fbits(t, get_v(t, in->dst - RDNA2_OP_VGPR));
        set_vf(t, in, ext3(t, F, GLSLstd450Fma, FA(), FB(), acc));
        break;
    }
    case SH_OP_V_MAD_F32: set_vf(t, in, op2(t, SpvOpFAdd, F, op2(t, SpvOpFMul, F, FA(), FB()), FC())); break;
    case SH_OP_V_FMA_F32: set_vf(t, in, ext3(t, F, GLSLstd450Fma, FA(), FB(), FC())); break;
    case SH_OP_V_MADMK_F32: set_vf(t, in, op2(t, SpvOpFAdd, F, op2(t, SpvOpFMul, F, FA(), fbits(t, cu(t, in->literal))), FB())); break;
    case SH_OP_V_MADAK_F32: set_vf(t, in, op2(t, SpvOpFAdd, F, op2(t, SpvOpFMul, F, FA(), FB()), fbits(t, cu(t, in->literal)))); break;
    case SH_OP_V_FMAMK_F32: set_vf(t, in, ext3(t, F, GLSLstd450Fma, FA(), fbits(t, cu(t, in->literal)), FB())); break;
    case SH_OP_V_FMAAK_F32: set_vf(t, in, ext3(t, F, GLSLstd450Fma, FA(), FB(), fbits(t, cu(t, in->literal)))); break;
    case SH_OP_V_MIN3_F32: set_vf(t, in, ext2(t, F, GLSLstd450NMin, ext2(t, F, GLSLstd450NMin, FA(), FB()), FC())); break;
    case SH_OP_V_MAX3_F32: set_vf(t, in, ext2(t, F, GLSLstd450NMax, ext2(t, F, GLSLstd450NMax, FA(), FB()), FC())); break;
    case SH_OP_V_MED3_F32: {
        uint32_t a = FA(), b = FB(), c = FC();
        uint32_t lo = ext2(t, F, GLSLstd450NMin, a, b), hi = ext2(t, F, GLSLstd450NMax, a, b);
        set_vf(t, in, ext2(t, F, GLSLstd450NMax, lo, ext2(t, F, GLSLstd450NMin, hi, c)));
        break;
    }
    case SH_OP_V_MUL_I32_I24: {
        uint32_t a = bfe_i(t, UA(), cu(t, 0), cu(t, 24)), b = bfe_i(t, UB(), cu(t, 0), cu(t, 24));
        set_vu(t, in, op2(t, SpvOpIMul, U, a, b));
        break;
    }
    case SH_OP_V_MAD_U32_U24: {
        uint32_t a = op2(t, SpvOpBitwiseAnd, U, UA(), cu(t, 0xFFFFFF)), b = op2(t, SpvOpBitwiseAnd, U, UB(), cu(t, 0xFFFFFF));
        set_vu(t, in, op2(t, SpvOpIAdd, U, op2(t, SpvOpIMul, U, a, b), UC()));
        break;
    }
    case SH_OP_V_MAD_I32_I24: {
        uint32_t a = bfe_i(t, UA(), cu(t, 0), cu(t, 24)), b = bfe_i(t, UB(), cu(t, 0), cu(t, 24));
        set_vu(t, in, op2(t, SpvOpIAdd, U, op2(t, SpvOpIMul, U, a, b), UC()));
        break;
    }
    case SH_OP_V_CVT_OFF_F32_I4: /* signed 4-bit value / 16 */
        set_vf(t, in, op2(t, SpvOpFMul, F, op1(t, SpvOpConvertSToF, F, ibits(t, bfe_i(t, UA(), cu(t, 0), cu(t, 4)))),
                          cf(t, 0.0625f)));
        break;
    case SH_OP_V_READLANE_B32: { /* lane copies written by v_writelane, else this lane's value */
        uint32_t n = (in->src[0] - RDNA2_OP_VGPR) & 0xFF, v = UA();
        if(t->full_wave32) v=spv_op(&t->b,SpvOpGroupNonUniformShuffle,U,
            (uint32_t[]){cu(t,SpvScopeSubgroup),v,op2(t,SpvOpBitwiseAnd,U,UB(),cu(t,31))},3);
        else if(t->full_wave64) v=wave_readlane(t,v,UB());
        if (in->src[0] >= RDNA2_OP_VGPR && t->lanes[n]) {
            uint32_t lane = op2(t, SpvOpBitwiseAnd, U, UB(), cu(t, 63));
            v = ld(t, U, op2(t, SpvOpAccessChain, t->p_fn_u32, t->lanes[n], lane));
        }
        set_s(t, (in->dst - RDNA2_OP_VGPR) & 0x7F, v);
        break;
    }
    case SH_OP_V_WRITELANE_B32: {
        uint32_t n = (in->dst - RDNA2_OP_VGPR) & 0xFF;
        if(t->full_wave64 || t->full_wave32) {
            /* This scalar instruction ignores EXEC and writes only the selected
             * VGPR lane. Other lanes retain their actual vector register. */
            uint32_t lane=op2(t,SpvOpBitwiseAnd,U,UB(),cu(t,t->full_wave32?31:63));
            st(t,vreg(t,n),sel(t,U,op2(t,SpvOpIEqual,B,wave_lane(t),lane),UA(),get_v(t,n)));
            break;
        }
        if (!t->lanes[n]) {
            uint32_t arr = spv_type_array(&t->b, U, cu(t, 64));
            t->lanes[n] = spv_local_var(&t->b, spv_type_pointer(&t->b, SpvStorageClassFunction, arr));
        }
        uint32_t lane = op2(t, SpvOpBitwiseAnd, U, UB(), cu(t, 63));
        st(t, op2(t, SpvOpAccessChain, t->p_fn_u32, t->lanes[n], lane), UA());
        break;
    }
    case SH_OP_V_CUBEID_F32: case SH_OP_V_CUBESC_F32: case SH_OP_V_CUBETC_F32: case SH_OP_V_CUBEMA_F32: {
        /* major axis z, then y, then x (ties toward z) */
        uint32_t x = FA(), y = FB(), z = FC();
        uint32_t ax = ext1(t, F, GLSLstd450FAbs, x), ay = ext1(t, F, GLSLstd450FAbs, y), az = ext1(t, F, GLSLstd450FAbs, z);
        uint32_t zmaj = op2(t, SpvOpLogicalAnd, B, op2(t, SpvOpFOrdGreaterThanEqual, B, az, ax), op2(t, SpvOpFOrdGreaterThanEqual, B, az, ay));
        uint32_t ymaj = op2(t, SpvOpFOrdGreaterThanEqual, B, ay, ax);
        uint32_t zero = cf(t, 0.0f), r;
        uint32_t xn = op2(t, SpvOpFOrdLessThan, B, x, zero), yn = op2(t, SpvOpFOrdLessThan, B, y, zero), zn = op2(t, SpvOpFOrdLessThan, B, z, zero);
        uint32_t nx = op1(t, SpvOpFNegate, F, x), ny = op1(t, SpvOpFNegate, F, y), nz = op1(t, SpvOpFNegate, F, z);
        if (op == SH_OP_V_CUBEID_F32) {
            r = sel(t, F, zmaj, sel(t, F, zn, cf(t, 5), cf(t, 4)),
                    sel(t, F, ymaj, sel(t, F, yn, cf(t, 3), cf(t, 2)), sel(t, F, xn, cf(t, 1), cf(t, 0))));
        } else if (op == SH_OP_V_CUBESC_F32) {
            r = sel(t, F, zmaj, sel(t, F, zn, nx, x), sel(t, F, ymaj, x, sel(t, F, xn, z, nz)));
        } else if (op == SH_OP_V_CUBETC_F32) {
            r = sel(t, F, zmaj, ny, sel(t, F, ymaj, sel(t, F, yn, nz, z), ny));
        } else {
            r = op2(t, SpvOpFMul, F, cf(t, 2.0f), sel(t, F, zmaj, z, sel(t, F, ymaj, y, x)));
        }
        set_vf(t, in, r);
        break;
    }
    case SH_OP_V_MUL_U32_U24: {
        uint32_t a = op2(t, SpvOpBitwiseAnd, U, UA(), cu(t, 0xFFFFFF)), b = op2(t, SpvOpBitwiseAnd, U, UB(), cu(t, 0xFFFFFF));
        set_vu(t, in, op2(t, SpvOpIMul, U, a, b));
        break;
    }
    case SH_OP_V_MUL_LO_U32: U2(SpvOpIMul)
    case SH_OP_V_MUL_HI_U32: case SH_OP_V_MUL_HI_I32: {
        uint32_t cv = op == SH_OP_V_MUL_HI_I32 ? SpvOpSConvert : SpvOpUConvert;
        bool sg = op == SH_OP_V_MUL_HI_I32;
        uint32_t a = op1(t, cv, t->t_u64, sg ? ibits(t, UA()) : UA()), b = op1(t, cv, t->t_u64, sg ? ibits(t, UB()) : UB());
        uint32_t m = op2(t, SpvOpIMul, t->t_u64, a, b);
        set_vu(t, in, op1(t, SpvOpUConvert, U, op2(t, SpvOpShiftRightLogical, t->t_u64, m, cu(t, 32))));
        break;
    }
    case SH_OP_V_MIN_I32: set_vu(t, in, ubits(t, ext2(t, I, GLSLstd450SMin, ibits(t, UA()), ibits(t, UB())))); break;
    case SH_OP_V_MAX_I32: set_vu(t, in, ubits(t, ext2(t, I, GLSLstd450SMax, ibits(t, UA()), ibits(t, UB())))); break;
    case SH_OP_V_MIN_U32: set_vu(t, in, ext2(t, U, GLSLstd450UMin, UA(), UB())); break;
    case SH_OP_V_MAX_U32: set_vu(t, in, ext2(t, U, GLSLstd450UMax, UA(), UB())); break;
    case SH_OP_V_MIN3_U32: set_vu(t, in, ext2(t, U, GLSLstd450UMin, ext2(t, U, GLSLstd450UMin, UA(), UB()), UC())); break;
    case SH_OP_V_MAX3_U32: set_vu(t, in, ext2(t, U, GLSLstd450UMax, ext2(t, U, GLSLstd450UMax, UA(), UB()), UC())); break;
    case SH_OP_V_MIN3_I32: set_vu(t, in, ubits(t, ext2(t, I, GLSLstd450SMin, ext2(t, I, GLSLstd450SMin, ibits(t, UA()), ibits(t, UB())), ibits(t, UC())))); break;
    case SH_OP_V_MAX3_I32: set_vu(t, in, ubits(t, ext2(t, I, GLSLstd450SMax, ext2(t, I, GLSLstd450SMax, ibits(t, UA()), ibits(t, UB())), ibits(t, UC())))); break;
    case SH_OP_V_MED3_U32: {
        uint32_t a = UA(), b = UB(), c = UC();
        uint32_t lo = ext2(t, U, GLSLstd450UMin, a, b), hi = ext2(t, U, GLSLstd450UMax, a, b);
        set_vu(t, in, ext2(t, U, GLSLstd450UMax, lo, ext2(t, U, GLSLstd450UMin, hi, c)));
        break;
    }
    case SH_OP_V_MED3_I32: {
        uint32_t a = ibits(t, UA()), b = ibits(t, UB()), c = ibits(t, UC());
        uint32_t lo = ext2(t, I, GLSLstd450SMin, a, b), hi = ext2(t, I, GLSLstd450SMax, a, b);
        set_vu(t, in, ubits(t, ext2(t, I, GLSLstd450SMax, lo, ext2(t, I, GLSLstd450SMin, hi, c))));
        break;
    }
    case SH_OP_V_LSHRREV_B32: set_vu(t, in, op2(t, SpvOpShiftRightLogical, U, UB(), op2(t, SpvOpBitwiseAnd, U, UA(), cu(t, 31)))); break;
    case SH_OP_V_ASHRREV_I32: set_vu(t, in, ubits(t, op2(t, SpvOpShiftRightArithmetic, I, ibits(t, UB()), op2(t, SpvOpBitwiseAnd, U, UA(), cu(t, 31))))); break;
    case SH_OP_V_LSHLREV_B32: set_vu(t, in, op2(t, SpvOpShiftLeftLogical, U, UB(), op2(t, SpvOpBitwiseAnd, U, UA(), cu(t, 31)))); break;
    case SH_OP_V_AND_B32: U2(SpvOpBitwiseAnd)
    case SH_OP_V_OR_B32: U2(SpvOpBitwiseOr)
    case SH_OP_V_XOR_B32: U2(SpvOpBitwiseXor)
    case SH_OP_V_XNOR_B32: set_vu(t, in, op1(t, SpvOpNot, U, op2(t, SpvOpBitwiseXor, U, UA(), UB()))); break;
    case SH_OP_V_NOT_B32: set_vu(t, in, op1(t, SpvOpNot, U, UA())); break;
    case SH_OP_V_BFREV_B32: set_vu(t, in, op1(t, SpvOpBitReverse, U, UA())); break;
    case SH_OP_V_ADD_NC_U32: U2(SpvOpIAdd)
    case SH_OP_V_SUB_NC_U32: U2(SpvOpISub)
    case SH_OP_V_SUBREV_NC_U32: set_vu(t, in, op2(t, SpvOpISub, U, UB(), UA())); break;
    case SH_OP_V_ADD_CO_U32: case SH_OP_V_SUB_CO_U32: case SH_OP_V_SUBREV_CO_U32: case SH_OP_V_ADD_CO_CI_U32:
    case SH_OP_V_SUB_CO_CI_U32: case SH_OP_V_SUBREV_CO_CI_U32: {
        uint32_t a = UA(), b = UB(), r, carry;
        uint32_t sd = is_vop3(in) ? in->sdst : RDNA2_OP_VCC_LO;
        if (op == SH_OP_V_ADD_CO_U32) {
            r = op2(t, SpvOpIAdd, U, a, b);
            carry = op2(t, SpvOpULessThan, B, r, a);
        } else if (op == SH_OP_V_ADD_CO_CI_U32) {
            uint32_t cin = b2u(t,(t->full_wave64 || t->full_wave32)?vector_mask(t,in,is_vop3(in)?in->src[2]:RDNA2_OP_VCC_LO):bit0(t, is_vop3(in) ? UC() : get_s(t, RDNA2_OP_VCC_LO)));
            uint32_t s1 = op2(t, SpvOpIAdd, U, a, b);
            r = op2(t, SpvOpIAdd, U, s1, cin);
            carry = op2(t, SpvOpLogicalOr, B, op2(t, SpvOpULessThan, B, s1, a), op2(t, SpvOpULessThan, B, r, s1));
        } else if (op == SH_OP_V_SUB_CO_CI_U32 || op == SH_OP_V_SUBREV_CO_CI_U32) { /* a - b - borrow in */
            uint32_t cin = b2u(t,(t->full_wave64 || t->full_wave32)?vector_mask(t,in,is_vop3(in)?in->src[2]:RDNA2_OP_VCC_LO):bit0(t, is_vop3(in) ? UC() : get_s(t, RDNA2_OP_VCC_LO)));
            if (op == SH_OP_V_SUBREV_CO_CI_U32) {
                uint32_t tmp = a;
                a = b;
                b = tmp;
            }
            uint32_t d1 = op2(t, SpvOpISub, U, a, b);
            r = op2(t, SpvOpISub, U, d1, cin);
            carry = op2(t, SpvOpLogicalOr, B, op2(t, SpvOpUGreaterThan, B, b, a), op2(t, SpvOpULessThan, B, d1, cin));
        } else {
            if (op == SH_OP_V_SUBREV_CO_U32) {
                uint32_t tmp = a;
                a = b;
                b = tmp;
            }
            r = op2(t, SpvOpISub, U, a, b);
            carry = op2(t, SpvOpUGreaterThan, B, b, a);
        }
        set_vu(t, in, r);
        set_mask(t, sd, op2(t, SpvOpLogicalAnd, B, carry, active(t)));
        break;
    }
    case SH_OP_V_ADD3_U32: set_vu(t, in, op2(t, SpvOpIAdd, U, op2(t, SpvOpIAdd, U, UA(), UB()), UC())); break;
    case SH_OP_V_SAD_U32: { /* |a - b| + c, unsigned */
        uint32_t a = UA(), b_ = UB();
        uint32_t diff = sel(t, U, op2(t, SpvOpUGreaterThanEqual, t->t_bool, a, b_), op2(t, SpvOpISub, U, a, b_),
                            op2(t, SpvOpISub, U, b_, a));
        set_vu(t, in, op2(t, SpvOpIAdd, U, diff, UC()));
        break;
    }
    case SH_OP_V_LSHL_ADD_U32: set_vu(t, in, op2(t, SpvOpIAdd, U, op2(t, SpvOpShiftLeftLogical, U, UA(), op2(t, SpvOpBitwiseAnd, U, UB(), cu(t, 31))), UC())); break;
    case SH_OP_V_ADD_LSHL_U32: set_vu(t, in, op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpIAdd, U, UA(), UB()), op2(t, SpvOpBitwiseAnd, U, UC(), cu(t, 31)))); break;
    case SH_OP_V_LSHL_OR_B32: set_vu(t, in, op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpShiftLeftLogical, U, UA(), op2(t, SpvOpBitwiseAnd, U, UB(), cu(t, 31))), UC())); break;
    case SH_OP_V_AND_OR_B32: set_vu(t, in, op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, UA(), UB()), UC())); break;
    case SH_OP_V_OR3_B32: set_vu(t, in, op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseOr, U, UA(), UB()), UC())); break;
    case SH_OP_V_XOR3_B32: set_vu(t, in, op2(t, SpvOpBitwiseXor, U, op2(t, SpvOpBitwiseXor, U, UA(), UB()), UC())); break;
    case SH_OP_V_BFE_U32: set_vu(t, in, bfe_u(t, UA(), UB(), op2(t, SpvOpBitwiseAnd, U, UC(), cu(t, 31)))); break;
    case SH_OP_V_BFE_I32: set_vu(t, in, bfe_i(t, UA(), UB(), op2(t, SpvOpBitwiseAnd, U, UC(), cu(t, 31)))); break;
    case SH_OP_V_BFI_B32: {
        uint32_t m = UA();
        set_vu(t, in, op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, m, UB()), op2(t, SpvOpBitwiseAnd, U, op1(t, SpvOpNot, U, m), UC())));
        break;
    }
    case SH_OP_V_BFM_B32: set_vu(t, in, op2(t, SpvOpShiftLeftLogical, U, mask_bits(t, op2(t, SpvOpBitwiseAnd, U, UA(), cu(t, 31))), op2(t, SpvOpBitwiseAnd, U, UB(), cu(t, 31)))); break;
    case SH_OP_V_BCNT_U32_B32: set_vu(t, in, op2(t, SpvOpIAdd, U, ubits(t, op1(t, SpvOpBitCount, I, UA())), UB())); break;
    case SH_OP_V_MBCNT_LO_U32_B32: case SH_OP_V_MBCNT_HI_U32_B32: {
        /* base + the mask's bits below this lane: the lane's index (mask -1), or its place among the
         * lanes a comparison selected (stream compaction). A comparison mask is kept as each lane's
         * own bit here; the subgroup's ballot gives the whole mask (subgroups are the guest's 32 lanes
         * in compute, VK_EXT_subgroup_size_control). The wave's lane: from the local invocation in
         * compute (wave_lane), else the subgroup's. */
        bool hi = op == SH_OP_V_MBCNT_HI_U32_B32;
        if (t->lds_private && !(t->translation_flags & SH_TRANSLATE_REAL_LANES)) { /* subgroup lane comparison */
            /* NGG vertex shader, one invocation per vertex with its own LDS: every invocation is
             * lane 0. The vertex half stores at slot 0, and the primitive half's vertex offsets
             * (v0/v1: 0) and the export's slot read that same vertex back. A geometry shader on
             * NGG (Unreal's slice GS for its 3D color LUT) then emits the invocation's own vertex
             * instead of the zeros of other lanes' slots. */
            set_vu(t, in, UB());
            break;
        }
        uint32_t code = in->src[0], mask;
        bool own_bit = code < SH_SGPRS && t->lane_mask[code];
        uint32_t lane;
        if (own_bit && t->in_sgid) {
            uint32_t bal = spv_op(&t->b, SpvOpGroupNonUniformBallot, t->t_v4u,
                                  (uint32_t[]){cu(t, SpvScopeSubgroup), op2(t, SpvOpINotEqual, B, UA(), cu(t, 0))}, 2);
            mask = op2(t, SpvOpCompositeExtract, U, bal, hi ? 1 : 0);
            lane = ld(t, U, t->in_sgid);
        } else {
            mask = UA();
            lane = t->full_wave64 || (t->in_lid && t->info->stage == SH_STAGE_CS) ? wave_lane(t) : t->in_sgid ? ld(t, U, t->in_sgid) : cu(t, 0);
        }
        /* lo counts lanes 0..min(lane, 32) - 1, hi lanes 32..lane - 1 */
        uint32_t n = hi ? sel(t, U, op2(t, SpvOpUGreaterThan, B, lane, cu(t, 32)), op2(t, SpvOpISub, U, lane, cu(t, 32)), cu(t, 0))
                        : ext2(t, U, GLSLstd450UMin, lane, cu(t, 32));
        uint32_t below = op2(t, SpvOpBitwiseAnd, U, mask, mask_bits(t, n));
        set_vu(t, in, op2(t, SpvOpIAdd, U, UB(), op1(t, SpvOpBitCount, U, below)));
        break;
    }
    case SH_OP_V_ALIGNBIT_B32: {
        uint32_t L = t->t_u64;
        uint32_t v = op2(t, SpvOpBitwiseOr, L, op2(t, SpvOpShiftLeftLogical, L, op1(t, SpvOpUConvert, L, UA()), cu(t, 32)), op1(t, SpvOpUConvert, L, UB()));
        set_vu(t, in, op1(t, SpvOpUConvert, U, op2(t, SpvOpShiftRightLogical, L, v, op2(t, SpvOpBitwiseAnd, U, UC(), cu(t, 31)))));
        break;
    }
    case SH_OP_V_FFBH_U32: {
        uint32_t a = UA();
        uint32_t m = ubits(t, ext1(t, I, GLSLstd450FindUMsb, a));
        set_vu(t, in, sel(t, U, op2(t, SpvOpIEqual, B, a, cu(t, 0)), cu(t, 0xFFFFFFFFu), op2(t, SpvOpISub, U, cu(t, 31), m)));
        break;
    }
    case SH_OP_V_FFBL_B32: set_vu(t, in, ubits(t, ext1(t, I, GLSLstd450FindILsb, UA()))); break;
    case SH_OP_V_CVT_F32_I32: set_vf(t, in, op1(t, SpvOpConvertSToF, F, ibits(t, UA()))); break;
    case SH_OP_V_CVT_F32_U32: set_vf(t, in, op1(t, SpvOpConvertUToF, F, UA())); break;
    case SH_OP_V_CVT_U32_F32: set_vu(t, in, op1(t, SpvOpConvertFToU, U, ext3(t, F, GLSLstd450FClamp, FA(), cf(t, 0), cf(t, 4294967040.0f)))); break;
    case SH_OP_V_CVT_I32_F32: set_vu(t, in, ubits(t, op1(t, SpvOpConvertFToS, I, ext3(t, F, GLSLstd450FClamp, FA(), cf(t, -2147483648.0f), cf(t, 2147483520.0f))))); break;
    case SH_OP_V_CVT_FLR_I32_F32: set_vu(t, in, ubits(t, op1(t, SpvOpConvertFToS, I, ext1(t, F, GLSLstd450Floor, FA())))); break;
    case SH_OP_V_CVT_RPI_I32_F32: set_vu(t, in, ubits(t, op1(t, SpvOpConvertFToS, I, ext1(t, F, GLSLstd450Floor, op2(t, SpvOpFAdd, F, FA(), cf(t, 0.5f)))))); break;
    case SH_OP_V_CVT_F16_F32: {
        uint32_t f = FA();
        set_vu(t, in, f16_result(t, f, &f, 1));
        break;
    }
    case SH_OP_V_CVT_F32_F16: {
        uint32_t v2 = ext1(t, t->t_v2f, GLSLstd450UnpackHalf2x16, UA());
        set_vf(t, in, op2(t, SpvOpCompositeExtract, F, v2, 0));
        break;
    }
    case SH_OP_V_CVT_F32_UBYTE0: case SH_OP_V_CVT_F32_UBYTE1: case SH_OP_V_CVT_F32_UBYTE2: case SH_OP_V_CVT_F32_UBYTE3: {
        uint32_t k = op - SH_OP_V_CVT_F32_UBYTE0;
        set_vf(t, in, op1(t, SpvOpConvertUToF, F, op3(t, SpvOpBitFieldUExtract, U, UA(), cu(t, k * 8), cu(t, 8))));
        break;
    }
    case SH_OP_V_CVT_PKRTZ_F16_F32:
        set_vu(t, in, op2(t, SpvOpBitwiseOr, U, half_rtz(t, FA()), op2(t, SpvOpShiftLeftLogical, U, half_rtz(t, FB()), cu(t, 16))));
        break;
    case SH_OP_V_CVT_PKNORM_U16_F32:
        set_vu(t, in, ext1(t, U, GLSLstd450PackUnorm2x16, op2(t, SpvOpCompositeConstruct, t->t_v2f, FA(), FB())));
        break;
    case SH_OP_V_CVT_PKNORM_I16_F32:
        set_vu(t, in, ext1(t, U, GLSLstd450PackSnorm2x16, op2(t, SpvOpCompositeConstruct, t->t_v2f, FA(), FB())));
        break;
    case SH_OP_V_CVT_PK_U8_F32: {
        uint32_t v = op1(t, SpvOpConvertFToU, U, ext3(t, F, GLSLstd450FClamp, ext1(t, F, GLSLstd450RoundEven, FA()), cf(t, 0), cf(t, 255)));
        uint32_t sh = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, UB(), cu(t, 3)), cu(t, 3));
        uint32_t keep = op1(t, SpvOpNot, U, op2(t, SpvOpShiftLeftLogical, U, cu(t, 0xFF), sh));
        set_vu(t, in, op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, UC(), keep), op2(t, SpvOpShiftLeftLogical, U, v, sh)));
        break;
    }
    case SH_OP_V_PACK_B32_F16: { /* op_sel picks each source's half (Unreal's TSR packs a high half: its
                                   * history normalization took garbage and grew 6x a frame); abs/neg on the sign */
        bool v3 = is_vop3(in);
        uint32_t hv[2];
        for (int k = 0; k < 2; k++) {
            hv[k] = op2(t, SpvOpBitwiseAnd, U, src16(t, in, k, v3 && ((in->opsel >> k) & 1), true), cu(t, 0xFFFF));
            if (v3 && ((in->abs >> k) & 1)) hv[k] = op2(t, SpvOpBitwiseAnd, U, hv[k], cu(t, 0x7FFF));
            if (v3 && ((in->neg >> k) & 1)) hv[k] = op2(t, SpvOpBitwiseXor, U, hv[k], cu(t, 0x8000));
        }
        set_vu(t, in, op2(t, SpvOpBitwiseOr, U, hv[0], op2(t, SpvOpShiftLeftLogical, U, hv[1], cu(t, 16))));
        break;
    }
    case SH_OP_V_FRACT_F32: FX1(GLSLstd450Fract)
    case SH_OP_V_TRUNC_F32: FX1(GLSLstd450Trunc)
    case SH_OP_V_CEIL_F32: FX1(GLSLstd450Ceil)
    case SH_OP_V_RNDNE_F32: FX1(GLSLstd450RoundEven)
    case SH_OP_V_FLOOR_F32: FX1(GLSLstd450Floor)
    case SH_OP_V_EXP_F32: FX1(GLSLstd450Exp2)
    case SH_OP_V_LOG_F32: FX1(GLSLstd450Log2)
    case SH_OP_V_SQRT_F32: FX1(GLSLstd450Sqrt)
    case SH_OP_V_RSQ_F32: FX1(GLSLstd450InverseSqrt)
    case SH_OP_V_RCP_F32:
    case SH_OP_V_RCP_IFLAG_F32: set_vf(t, in, op2(t, SpvOpFDiv, F, cf(t, 1.0f), FA())); break;
    case SH_OP_V_SIN_F32: set_vf(t, in, ext1(t, F, GLSLstd450Sin, op2(t, SpvOpFMul, F, FA(), cf(t, 6.28318530718f)))); break;
    case SH_OP_V_COS_F32: set_vf(t, in, ext1(t, F, GLSLstd450Cos, op2(t, SpvOpFMul, F, FA(), cf(t, 6.28318530718f)))); break;
    case SH_OP_V_LDEXP_F32: set_vf(t, in, ext2(t, F, GLSLstd450Ldexp, FA(), ibits(t, UB()))); break;
    /* 16-bit, non-packed: VOP3 op_sel picks source halves */
    case SH_OP_V_ADD_F16: case SH_OP_V_SUB_F16: case SH_OP_V_SUBREV_F16: case SH_OP_V_MUL_F16: case SH_OP_V_MIN_F16:
    case SH_OP_V_MAX_F16: case SH_OP_V_FMA_F16: case SH_OP_V_MAD_F16: case SH_OP_V_FMAC_F16: case SH_OP_V_MED3_F16:
    case SH_OP_V_MIN3_F16: case SH_OP_V_MAX3_F16: {
        bool v3 = is_vop3(in);
        uint32_t ab = v3 ? in->abs : 0, ng = v3 ? in->neg : 0, os = v3 ? in->opsel : 0;
        uint32_t a = srch(t, in, 0, os & 1, ab & 1, ng & 1), b = srch(t, in, 1, (os >> 1) & 1, (ab >> 1) & 1, (ng >> 1) & 1), f, third = 0;
        if (op == SH_OP_V_FMA_F16 || op == SH_OP_V_MAD_F16) {
            third = srch(t, in, 2, (os >> 2) & 1, (ab >> 2) & 1, (ng >> 2) & 1);
            f = ext3(t, F, GLSLstd450Fma, a, b, third);
        } else if (op == SH_OP_V_MED3_F16) {
            uint32_t c = third = srch(t, in, 2, (os >> 2) & 1, (ab >> 2) & 1, (ng >> 2) & 1);
            uint32_t lo = ext2(t, F, GLSLstd450NMin, a, b), hi = ext2(t, F, GLSLstd450NMax, a, b);
            f = ext2(t, F, GLSLstd450NMax, lo, ext2(t, F, GLSLstd450NMin, hi, c));
        } else if (op == SH_OP_V_MIN3_F16 || op == SH_OP_V_MAX3_F16) {
            uint32_t c = third = srch(t, in, 2, (os >> 2) & 1, (ab >> 2) & 1, (ng >> 2) & 1), fn = op == SH_OP_V_MIN3_F16 ? GLSLstd450NMin : GLSLstd450NMax;
            f = ext2(t, F, fn, ext2(t, F, fn, a, b), c);
        } else if (op == SH_OP_V_FMAC_F16) {
            third = h2f(t, op2(t, SpvOpBitwiseAnd, U, get_v(t, in->dst - RDNA2_OP_VGPR), cu(t, 0xFFFF)));
            f = ext3(t, F, GLSLstd450Fma, a, b, third);
        } else {
            f = op == SH_OP_V_ADD_F16 ? op2(t, SpvOpFAdd, F, a, b) : op == SH_OP_V_SUB_F16 ? op2(t, SpvOpFSub, F, a, b)
              : op == SH_OP_V_SUBREV_F16 ? op2(t, SpvOpFSub, F, b, a) : op == SH_OP_V_MUL_F16 ? op2(t, SpvOpFMul, F, a, b)
              : op == SH_OP_V_MIN_F16 ? fminmax(t, a, b, false) : fminmax(t, a, b, true);
        }
        if (t->sdwa & (1u << 13)) f = saturate_output(t, f);
        else if (v3 && in->clamp) f = clamp01(t, f);
        set_v16(t, in, f16_result(t, f, (uint32_t[]){a, b, third}, third ? 3 : 2));
        break;
    }
    case SH_OP_V_LSHLREV_B16: case SH_OP_V_LSHRREV_B16: case SH_OP_V_ASHRREV_I16: case SH_OP_V_ADD_U16:
    case SH_OP_V_SUB_U16: case SH_OP_V_MUL_LO_U16: {
        uint32_t os = is_vop3(in) ? in->opsel : 0;
        uint32_t a = src16(t, in, 0, os & 1, false), b = src16(t, in, 1, (os >> 1) & 1, false), sh = op2(t, SpvOpBitwiseAnd, U, a, cu(t, 15)), v;
        if (op == SH_OP_V_LSHLREV_B16) v = op2(t, SpvOpShiftLeftLogical, U, b, sh);
        else if (op == SH_OP_V_LSHRREV_B16) v = op2(t, SpvOpShiftRightLogical, U, b, sh);
        else if (op == SH_OP_V_ASHRREV_I16) v = ubits(t, op2(t, SpvOpShiftRightArithmetic, I, ibits(t, bfe_i(t, b, cu(t, 0), cu(t, 16))), sh));
        else if (op == SH_OP_V_ADD_U16) v = op2(t, SpvOpIAdd, U, a, b);
        else if (op == SH_OP_V_SUB_U16) v = op2(t, SpvOpISub, U, a, b);
        else v = op2(t, SpvOpIMul, U, a, b);
        set_v16(t, in, v);
        break;
    }
    case SH_OP_V_RSQ_F16: case SH_OP_V_SQRT_F16: case SH_OP_V_RCP_F16: case SH_OP_V_SIN_F16: case SH_OP_V_COS_F16:
    case SH_OP_V_EXP_F16: case SH_OP_V_LOG_F16: case SH_OP_V_FLOOR_F16: case SH_OP_V_CEIL_F16: case SH_OP_V_TRUNC_F16:
    case SH_OP_V_RNDNE_F16: case SH_OP_V_FRACT_F16: case SH_OP_V_LDEXP_F16: {
        bool v3 = is_vop3(in);
        uint32_t a = srch(t, in, 0, v3 && (in->opsel & 1), v3 && (in->abs & 1), v3 && (in->neg & 1)), f;
        switch (op) {
        case SH_OP_V_RSQ_F16: f = ext1(t, F, GLSLstd450InverseSqrt, a); break;
        case SH_OP_V_SQRT_F16: f = ext1(t, F, GLSLstd450Sqrt, a); break;
        case SH_OP_V_RCP_F16: f = op2(t, SpvOpFDiv, F, cf(t, 1.0f), a); break;
        case SH_OP_V_SIN_F16: f = ext1(t, F, GLSLstd450Sin, op2(t, SpvOpFMul, F, a, cf(t, 6.28318530718f))); break;
        case SH_OP_V_COS_F16: f = ext1(t, F, GLSLstd450Cos, op2(t, SpvOpFMul, F, a, cf(t, 6.28318530718f))); break;
        case SH_OP_V_EXP_F16: f = ext1(t, F, GLSLstd450Exp2, a); break;
        case SH_OP_V_LOG_F16: f = ext1(t, F, GLSLstd450Log2, a); break;
        case SH_OP_V_FLOOR_F16: f = ext1(t, F, GLSLstd450Floor, a); break;
        case SH_OP_V_CEIL_F16: f = ext1(t, F, GLSLstd450Ceil, a); break;
        case SH_OP_V_TRUNC_F16: f = ext1(t, F, GLSLstd450Trunc, a); break;
        case SH_OP_V_RNDNE_F16: f = ext1(t, F, GLSLstd450RoundEven, a); break;
        case SH_OP_V_FRACT_F16: f = ext1(t, F, GLSLstd450Fract, a); break;
        default: f = ext2(t, F, GLSLstd450Ldexp, a, ibits(t, bfe_i(t, UB(), cu(t, 0), cu(t, 16)))); break;
        }
        if (t->sdwa & (1u << 13)) f = saturate_output(t, f);
        else if (v3 && in->clamp) f = clamp01(t, f);
        set_v16(t, in, f16_result(t, f, &a, 1));
        break;
    }
    case SH_OP_V_ADD_NC_U16: case SH_OP_V_SUB_NC_U16: case SH_OP_V_MAX_U16: case SH_OP_V_MIN_U16: case SH_OP_V_MAX_I16:
    case SH_OP_V_MIN_I16: case SH_OP_V_ADD_NC_I16: case SH_OP_V_SUB_NC_I16: {
        uint32_t os = is_vop3(in) ? in->opsel : 0;
        uint32_t a = src16(t, in, 0, os & 1, false), b = src16(t, in, 1, (os >> 1) & 1, false), v;
        uint32_t sa = ibits(t, bfe_i(t, a, cu(t, 0), cu(t, 16))), sb = ibits(t, bfe_i(t, b, cu(t, 0), cu(t, 16)));
        if (op == SH_OP_V_ADD_NC_U16 || op == SH_OP_V_ADD_NC_I16) v = op2(t, SpvOpIAdd, U, a, b);
        else if (op == SH_OP_V_SUB_NC_U16 || op == SH_OP_V_SUB_NC_I16) v = op2(t, SpvOpISub, U, a, b);
        else if (op == SH_OP_V_MAX_U16) v = ext2(t, U, GLSLstd450UMax, a, b);
        else if (op == SH_OP_V_MIN_U16) v = ext2(t, U, GLSLstd450UMin, a, b);
        else if (op == SH_OP_V_MAX_I16) v = ubits(t, ext2(t, I, GLSLstd450SMax, sa, sb));
        else v = ubits(t, ext2(t, I, GLSLstd450SMin, sa, sb));
        set_v16(t, in, v);
        break;
    }
    case SH_OP_V_CVT_I16_F16: case SH_OP_V_CVT_U16_F16: {
        bool v3 = is_vop3(in);
        uint32_t f = srch(t, in, 0, v3 && (in->opsel & 1), v3 && (in->abs & 1), v3 && (in->neg & 1));
        uint32_t v = op == SH_OP_V_CVT_I16_F16
                   ? ubits(t, op1(t, SpvOpConvertFToS, I, ext3(t, F, GLSLstd450FClamp, f, cf(t, -32768.0f), cf(t, 32767.0f))))
                   : op1(t, SpvOpConvertFToU, U, ext3(t, F, GLSLstd450FClamp, f, cf(t, 0.0f), cf(t, 65535.0f)));
        set_v16(t, in, v);
        break;
    }
    case SH_OP_V_CVT_F16_I16: case SH_OP_V_CVT_F16_U16: {
        uint32_t a = src16(t, in, 0, is_vop3(in) && (in->opsel & 1), false);
        uint32_t f = op == SH_OP_V_CVT_F16_I16 ? op1(t, SpvOpConvertSToF, F, ibits(t, bfe_i(t, a, cu(t, 0), cu(t, 16))))
                                                : op1(t, SpvOpConvertUToF, F, a);
        set_v16(t, in, f16_result(t, f, NULL, 0));
        break;
    }
    case SH_OP_V_CVT_PK_U16_U32:
        set_vu(t, in, op2(t, SpvOpBitwiseOr, U, ext2(t, U, GLSLstd450UMin, UA(), cu(t, 0xFFFF)),
                          op2(t, SpvOpShiftLeftLogical, U, ext2(t, U, GLSLstd450UMin, UB(), cu(t, 0xFFFF)), cu(t, 16))));
        break;
    case SH_OP_V_CVT_PK_I16_I32: {
        uint32_t lo = ubits(t, ext3(t, I, GLSLstd450SClamp, ibits(t, UA()), ibits(t, cu(t, 0xFFFF8000u)), ibits(t, cu(t, 0x7FFF))));
        uint32_t hi = ubits(t, ext3(t, I, GLSLstd450SClamp, ibits(t, UB()), ibits(t, cu(t, 0xFFFF8000u)), ibits(t, cu(t, 0x7FFF))));
        set_vu(t, in, op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, lo, cu(t, 0xFFFF)), op2(t, SpvOpShiftLeftLogical, U, hi, cu(t, 16))));
        break;
    }
    case SH_OP_V_PK_ADD_F16: case SH_OP_V_PK_MUL_F16: case SH_OP_V_PK_FMA_F16: case SH_OP_V_PK_MIN_F16: case SH_OP_V_PK_MAX_F16:
    case SH_OP_V_PK_ADD_U16: case SH_OP_V_PK_SUB_U16: case SH_OP_V_PK_ADD_I16: case SH_OP_V_PK_SUB_I16:
    case SH_OP_V_PK_MUL_LO_U16: case SH_OP_V_PK_MAD_U16: case SH_OP_V_PK_MAD_I16: case SH_OP_V_PK_LSHLREV_B16:
    case SH_OP_V_PK_LSHRREV_B16: case SH_OP_V_PK_ASHRREV_I16: case SH_OP_V_PK_MAX_U16: case SH_OP_V_PK_MIN_U16:
    case SH_OP_V_PK_MAX_I16: case SH_OP_V_PK_MIN_I16: packed(t, in, op); break;
    case SH_OP_V_PK_FMAC_F16: { /* both halves: D = S0 * S1 + D */
        uint32_t dv = get_v(t, in->dst - RDNA2_OP_VGPR), r[2];
        for (uint32_t h = 0; h < 2; h++) {
            uint32_t c = h2f(t, op2(t, SpvOpBitwiseAnd, U, h ? op2(t, SpvOpShiftRightLogical, U, dv, cu(t, 16)) : dv, cu(t, 0xFFFF)));
            uint32_t a = srch(t, in, 0, h, false, false), b = srch(t, in, 1, h, false, false);
            r[h] = f16_result(t, ext3(t, F, GLSLstd450Fma, a, b, c), (uint32_t[]){a,b,c}, 3);
        }
        set_vu(t, in, op2(t, SpvOpBitwiseOr, U, r[0], op2(t, SpvOpShiftLeftLogical, U, r[1], cu(t, 16))));
        break;
    }
    case SH_OP_V_FMAMK_F16: case SH_OP_V_FMAAK_F16: { /* S0 * K + S1 / S0 * S1 + K, K the literal's low half */
        uint32_t k = h2f(t, cu(t, in->literal & 0xFFFF)), a = srch(t, in, 0, false, false, false), b = srch(t, in, 1, false, false, false);
        set_v16(t, in, f16_result(t, op == SH_OP_V_FMAMK_F16 ? ext3(t, F, GLSLstd450Fma, a, k, b) : ext3(t, F, GLSLstd450Fma, a, b, k), (uint32_t[]){a,b,k}, 3));
        break;
    }
    case SH_OP_V_FMA_MIX_F32: case SH_OP_V_FMA_MIXLO_F16: case SH_OP_V_FMA_MIXHI_F16: fma_mix(t, in, op); break;
    case SH_OP_V_PERMLANEX16_B32: case SH_OP_V_PERMLANE16_B32:
        if (!t->in_sgid) {
            fail(t, "lane permute outside a subgroup");
            break;
        }
        set_vu(t, in, permlane(t, in, op == SH_OP_V_PERMLANEX16_B32));
        break;
    case SH_OP_V_XAD_U32: set_vu(t, in, op2(t, SpvOpIAdd, U, op2(t, SpvOpBitwiseXor, U, UA(), UB()), UC())); break;
    case SH_OP_V_LSHLREV_B64: set_v64(t, in, op2(t, SpvOpShiftLeftLogical, t->t_u64, vsrc64(t, in, 1), op2(t, SpvOpBitwiseAnd, U, UA(), cu(t, 63)))); break;
    case SH_OP_V_LSHRREV_B64: set_v64(t, in, op2(t, SpvOpShiftRightLogical, t->t_u64, vsrc64(t, in, 1), op2(t, SpvOpBitwiseAnd, U, UA(), cu(t, 63)))); break;
    case SH_OP_V_ASHRREV_I64: set_v64(t, in, op2(t, SpvOpShiftRightArithmetic, t->t_u64, vsrc64(t, in, 1), op2(t, SpvOpBitwiseAnd, U, UA(), cu(t, 63)))); break;
    case SH_OP_V_MAD_U64_U32: case SH_OP_V_MAD_I64_I32: {
        uint32_t L = t->t_u64;
        bool s = op == SH_OP_V_MAD_I64_I32;
        uint32_t a = s ? op1(t, SpvOpSConvert, L, ibits(t, UA())) : op1(t, SpvOpUConvert, L, UA());
        uint32_t b = s ? op1(t, SpvOpSConvert, L, ibits(t, UB())) : op1(t, SpvOpUConvert, L, UB());
        uint32_t c = vsrc64(t, in, 2), r = op2(t, SpvOpIAdd, L, op2(t, SpvOpIMul, L, a, b), c);
        set_v64(t, in, r);
        if (in->sdst != RDNA2_OP_NULL) set_mask(t, in->sdst, spv_const_bool(&t->b, false)); /* carry out: none */
        break;
    }
    case SH_OP_V_ALIGNBYTE_B32: {
        uint32_t L = t->t_u64;
        uint32_t v = op2(t, SpvOpBitwiseOr, L, op2(t, SpvOpShiftLeftLogical, L, op1(t, SpvOpUConvert, L, UA()), cu(t, 32)), op1(t, SpvOpUConvert, L, UB()));
        uint32_t sh = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, UC(), cu(t, 3)), cu(t, 3));
        set_vu(t, in, op1(t, SpvOpUConvert, U, op2(t, SpvOpShiftRightLogical, L, v, sh)));
        break;
    }
    case SH_OP_V_MUL_HI_I32_I24: case SH_OP_V_MUL_HI_U32_U24: { /* bits 63:32 of the 48-bit product */
        uint32_t L = t->t_u64;
        bool s = op == SH_OP_V_MUL_HI_I32_I24;
        uint32_t a = s ? op1(t, SpvOpSConvert, L, ibits(t, bfe_i(t, UA(), cu(t, 0), cu(t, 24))))
                       : op1(t, SpvOpUConvert, L, op2(t, SpvOpBitwiseAnd, U, UA(), cu(t, 0xFFFFFF)));
        uint32_t b = s ? op1(t, SpvOpSConvert, L, ibits(t, bfe_i(t, UB(), cu(t, 0), cu(t, 24))))
                       : op1(t, SpvOpUConvert, L, op2(t, SpvOpBitwiseAnd, U, UB(), cu(t, 0xFFFFFF)));
        set_vu(t, in, op1(t, SpvOpUConvert, U, op2(t, SpvOpShiftRightLogical, L, op2(t, SpvOpIMul, L, a, b), cu(t, 32))));
        break;
    }
    case SH_OP_V_MAC_LEGACY_F32: {
        uint32_t acc = fbits(t, get_v(t, in->dst - RDNA2_OP_VGPR));
        set_vf(t, in, op2(t, SpvOpFAdd, F, legacy_mul(t, FA(), FB()), acc));
        break;
    }
    case SH_OP_V_MAD_LEGACY_F32: set_vf(t, in, op2(t, SpvOpFAdd, F, legacy_mul(t, FA(), FB()), FC())); break;
    case SH_OP_V_MULLIT_F32: { /* a * b, -FLT_MAX when b is -FLT_MAX or NaN or the limit c is not above 0 */
        uint32_t a = FA(), b = FB(), c = FC(), bb = ubits(t, b);
        uint32_t lowest = op2(t, SpvOpLogicalOr, B, op2(t, SpvOpUGreaterThan, B, bb, cu(t, 0xFF7FFFFEu)), op1(t, SpvOpIsNan, B, b));
        lowest = op2(t, SpvOpLogicalOr, B, lowest, op1(t, SpvOpLogicalNot, B, op2(t, SpvOpFOrdGreaterThan, B, c, cf(t, 0))));
        set_vf(t, in, sel(t, F, lowest, fbits(t, cu(t, 0xFF7FFFFFu)), legacy_mul(t, a, b)));
        break;
    }
    case SH_OP_V_FFBH_I32: {
        uint32_t x = op2(t, SpvOpBitwiseXor, U, UA(), ubits(t, op2(t, SpvOpShiftRightArithmetic, I, ibits(t, UA()), cu(t, 31))));
        uint32_t m = ubits(t, ext1(t, I, GLSLstd450FindUMsb, x));
        set_vu(t, in, sel(t, U, op2(t, SpvOpIEqual, B, x, cu(t, 0)), cu(t, 0xFFFFFFFFu), op2(t, SpvOpISub, U, cu(t, 31), m)));
        break;
    }
    case SH_OP_V_SWAP_B32: {
        if (in->src[0] < RDNA2_OP_VGPR) {
            fail(t, "v_swap_b32 of a scalar");
            break;
        }
        uint32_t n = in->dst - RDNA2_OP_VGPR, m = in->src[0] - RDNA2_OP_VGPR, x = get_v(t, n), y = get_v(t, m);
        set_v(t, n, y);
        set_v(t, m, x);
        break;
    }
    case SH_OP_V_CVT_NORM_I16_F16: case SH_OP_V_CVT_NORM_U16_F16: {
        bool v3 = is_vop3(in);
        uint32_t f = srch(t, in, 0, v3 && (in->opsel & 1), v3 && (in->abs & 1), v3 && (in->neg & 1));
        set_v16(t, in, norm16(t, f, op == SH_OP_V_CVT_NORM_I16_F16));
        break;
    }
    case SH_OP_V_CVT_PKNORM_I16_F16: case SH_OP_V_CVT_PKNORM_U16_F16: {
        bool s = op == SH_OP_V_CVT_PKNORM_I16_F16;
        uint32_t a = srch(t, in, 0, in->opsel & 1, in->abs & 1, in->neg & 1), b = srch(t, in, 1, (in->opsel >> 1) & 1, (in->abs >> 1) & 1, (in->neg >> 1) & 1);
        set_vu(t, in, op2(t, SpvOpBitwiseOr, U, norm16(t, a, s), op2(t, SpvOpShiftLeftLogical, U, norm16(t, b, s), cu(t, 16))));
        break;
    }
    case SH_OP_V_FREXP_EXP_I32_F32: case SH_OP_V_FREXP_MANT_F32: {
        /* f32 denormals are normalized through a product by 2^32 first */
        uint32_t f = FA(), bits = ubits(t, f);
        uint32_t den = op2(t, SpvOpIEqual, B, op3(t, SpvOpBitFieldUExtract, U, bits, cu(t, 23), cu(t, 8)), cu(t, 0));
        uint32_t nb = ubits(t, sel(t, F, den, op2(t, SpvOpFMul, F, f, cf(t, 4294967296.0f)), f));
        uint32_t e = op3(t, SpvOpBitFieldUExtract, U, nb, cu(t, 23), cu(t, 8));
        uint32_t special = op2(t, SpvOpLogicalOr, B, op2(t, SpvOpIEqual, B, e, cu(t, 0)), op2(t, SpvOpIEqual, B, e, cu(t, 255)));
        if (op == SH_OP_V_FREXP_EXP_I32_F32) {
            uint32_t ex = op2(t, SpvOpISub, U, e, sel(t, U, den, cu(t, 158), cu(t, 126)));
            set_vu(t, in, sel(t, U, special, cu(t, 0), ex));
        } else {
            uint32_t m = fbits(t, op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, nb, cu(t, 0x807FFFFFu)), cu(t, 0x3F000000u)));
            set_vf(t, in, sel(t, F, special, f, m));
        }
        break;
    }
    case SH_OP_V_PERM_B32: { /* each result byte picks a byte of {S0, S1} (S1 low) by S2's byte */
        uint32_t a = UA(), b = UB(), s = UC(), r = cu(t, 0);
        for (uint32_t k = 0; k < 4; k++) {
            uint32_t sl = op3(t, SpvOpBitFieldUExtract, U, s, cu(t, 8 * k), cu(t, 8));
            uint32_t src_word = sel(t, U, op2(t, SpvOpUGreaterThanEqual, B, sl, cu(t, 4)), a, b);
            uint32_t byte = op3(t, SpvOpBitFieldUExtract, U, src_word, op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, sl, cu(t, 3)), cu(t, 3)), cu(t, 8));
            /* 8..11: the sign of bytes 1, 3, 5, 7 replicated; 12: 0x00; 13 and up: 0xFF */
            uint32_t sbit = sel(t, U, op2(t, SpvOpIEqual, B, sl, cu(t, 8)), op3(t, SpvOpBitFieldUExtract, U, b, cu(t, 15), cu(t, 1)),
                            sel(t, U, op2(t, SpvOpIEqual, B, sl, cu(t, 9)), op3(t, SpvOpBitFieldUExtract, U, b, cu(t, 31), cu(t, 1)),
                                sel(t, U, op2(t, SpvOpIEqual, B, sl, cu(t, 10)), op3(t, SpvOpBitFieldUExtract, U, a, cu(t, 15), cu(t, 1)),
                                    op3(t, SpvOpBitFieldUExtract, U, a, cu(t, 31), cu(t, 1)))));
            uint32_t v = sel(t, U, op2(t, SpvOpULessThan, B, sl, cu(t, 8)), byte,
                             sel(t, U, op2(t, SpvOpULessThan, B, sl, cu(t, 12)), op2(t, SpvOpIMul, U, sbit, cu(t, 0xFF)),
                                 sel(t, U, op2(t, SpvOpIEqual, B, sl, cu(t, 12)), cu(t, 0), cu(t, 0xFF))));
            r = op2(t, SpvOpBitwiseOr, U, r, op2(t, SpvOpShiftLeftLogical, U, v, cu(t, 8 * k)));
        }
        set_vu(t, in, r);
        break;
    }
    case SH_OP_V_FREXP_EXP_I16_F16: case SH_OP_V_FREXP_MANT_F16: {
        /* through f32, where every f16 is normal: exponent - 126 and the mantissa in [0.5, 1);
         * zero, infinity and NaN give exponent 0 and themselves as mantissa */
        bool v3 = is_vop3(in);
        uint32_t f = srch(t, in, 0, v3 && (in->opsel & 1), v3 && (in->abs & 1), v3 && (in->neg & 1)), bits = ubits(t, f);
        uint32_t e = op3(t, SpvOpBitFieldUExtract, U, bits, cu(t, 23), cu(t, 8));
        uint32_t special = op2(t, SpvOpLogicalOr, B, op2(t, SpvOpIEqual, B, e, cu(t, 0)), op2(t, SpvOpIEqual, B, e, cu(t, 255)));
        if (op == SH_OP_V_FREXP_EXP_I16_F16) {
            set_v16(t, in, sel(t, U, special, cu(t, 0), op2(t, SpvOpISub, U, e, cu(t, 126))));
        } else {
            uint32_t m = fbits(t, op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, bits, cu(t, 0x807FFFFFu)), cu(t, 0x3F000000u)));
            set_v16(t, in, f16_result(t, sel(t, F, special, f, m), &f, 1));
        }
        break;
    }
    case SH_OP_V_SAT_PK_U8_I16: { /* both signed halves saturated to bytes 0 and 1 */
        uint32_t lo = ext3(t, I, GLSLstd450SClamp, ibits(t, bfe_i(t, UA(), cu(t, 0), cu(t, 16))), ibits(t, cu(t, 0)), ibits(t, cu(t, 255)));
        uint32_t hi = ext3(t, I, GLSLstd450SClamp, ibits(t, bfe_i(t, UA(), cu(t, 16), cu(t, 16))), ibits(t, cu(t, 0)), ibits(t, cu(t, 255)));
        set_vu(t, in, op2(t, SpvOpBitwiseOr, U, ubits(t, lo), op2(t, SpvOpShiftLeftLogical, U, ubits(t, hi), cu(t, 8))));
        break;
    }
    case SH_OP_V_ADD_NC_I32: case SH_OP_V_SUB_NC_I32: {
        uint32_t r = op2(t, op == SH_OP_V_ADD_NC_I32 ? SpvOpIAdd : SpvOpISub, U, UA(), UB());
        if (in->clamp) { /* signed saturation */
            uint32_t L = t->t_u64;
            uint32_t a = op1(t, SpvOpSConvert, L, ibits(t, UA())), b = op1(t, SpvOpSConvert, L, ibits(t, UB()));
            uint32_t w = op2(t, op == SH_OP_V_ADD_NC_I32 ? SpvOpIAdd : SpvOpISub, L, a, b);
            uint32_t lo = op1(t, SpvOpSConvert, L, ibits(t, cu(t, 0x80000000u))), hi = op1(t, SpvOpUConvert, L, cu(t, 0x7FFFFFFFu));
            w = sel(t, L, op2(t, SpvOpSLessThan, B, w, lo), lo, sel(t, L, op2(t, SpvOpSGreaterThan, B, w, hi), hi, w));
            r = op1(t, SpvOpUConvert, U, w);
        }
        set_vu(t, in, r);
        break;
    }
    case SH_OP_V_DIV_SCALE_F32: /* no scaling (only operands near the exponent limits need it): VCC stays clear */
        set_vf(t, in, FA());
        set_mask(t, in->sdst, spv_const_bool(&t->b, false));
        break;
    case SH_OP_V_DIV_FMAS_F32: set_vf(t, in, ext3(t, F, GLSLstd450Fma, FA(), FB(), FC())); break;
    case SH_OP_V_DIV_FIXUP_F32: set_vf(t, in, fbits(t, div_fixup_f32(t, ubits(t, FA()), ubits(t, FB()), ubits(t, FC())))); break;
    case SH_OP_V_CVT_F64_F32: set_vf64(t, in, f32_to_f64(t, ubits(t, FA()))); break;
    case SH_OP_V_CVT_F32_F64: set_vf(t, in, fbits(t, f64_to_f32(t, vsrc_f64(t, in, 0)))); break;
    case SH_OP_V_CVT_F64_U32: set_vf64(t, in, u32_to_f64(t, UA())); break;
    case SH_OP_V_CVT_F64_I32: {
        uint32_t a = UA(), neg = op2(t, SpvOpSLessThan, B, ibits(t, a), ci32(t, 0));
        uint32_t mag = sel(t, U, neg, op2(t, SpvOpISub, U, cu(t, 0), a), a);
        set_vf64(t, in, op2(t, SpvOpBitwiseOr, t->t_u64, u32_to_f64(t, mag),
                            sel(t, t->t_u64, neg, c64(t, 0x8000000000000000ull), c64(t, 0))));
        break;
    }
    case SH_OP_V_CVT_I32_F64: set_vu(t, in, f64_to_int(t, vsrc_f64(t, in, 0), true)); break;
    case SH_OP_V_CVT_U32_F64: set_vu(t, in, f64_to_int(t, vsrc_f64(t, in, 0), false)); break;
    case SH_OP_V_TRUNC_F64: set_vf64(t, in, f64_round(t, vsrc_f64(t, in, 0), 0)); break;
    case SH_OP_V_FLOOR_F64: set_vf64(t, in, f64_round(t, vsrc_f64(t, in, 0), 1)); break;
    case SH_OP_V_CEIL_F64: set_vf64(t, in, f64_round(t, vsrc_f64(t, in, 0), 2)); break;
    case SH_OP_V_RNDNE_F64: set_vf64(t, in, f64_round(t, vsrc_f64(t, in, 0), 3)); break;
    case SH_OP_V_FRACT_F64: set_vf64(t, in, f64_fract(t, vsrc_f64(t, in, 0))); break;
    case SH_OP_V_FREXP_MANT_F64: case SH_OP_V_FREXP_EXP_I32_F64: {
        uint32_t mant, ex;
        f64_frexp(t, vsrc_f64(t, in, 0), &mant, &ex);
        if (op == SH_OP_V_FREXP_MANT_F64) set_vf64(t, in, mant);
        else set_vu(t, in, ex);
        break;
    }
    case SH_OP_V_LDEXP_F64: set_vf64(t, in, f64_ldexp(t, vsrc_f64(t, in, 0), ibits(t, UB()))); break;
    case SH_OP_V_MAX_F64: case SH_OP_V_MIN_F64: { /* a NaN operand yields the other */
        uint32_t a = vsrc_f64(t, in, 0), b = vsrc_f64(t, in, 1), L = t->t_u64;
        uint32_t r = sel(t, L, f64_compare(t, op == SH_OP_V_MAX_F64 ? "ge" : "le", a, b), a, b);
        r = sel(t, L, f64_is_nan(t, a), b, r);
        r = sel(t, L, f64_is_nan(t, b), a, r);
        set_vf64(t, in, r);
        break;
    }
    case SH_OP_V_ADD_F64: set_vf64_from_f32(t, in, op2(t, SpvOpFAdd, F, f64_as_f32(t, in, 0), f64_as_f32(t, in, 1))); break;
    case SH_OP_V_MUL_F64: set_vf64_from_f32(t, in, op2(t, SpvOpFMul, F, f64_as_f32(t, in, 0), f64_as_f32(t, in, 1))); break;
    case SH_OP_V_FMA_F64: case SH_OP_V_DIV_FMAS_F64:
        set_vf64_from_f32(t, in, ext3(t, F, GLSLstd450Fma, f64_as_f32(t, in, 0), f64_as_f32(t, in, 1), f64_as_f32(t, in, 2)));
        break;
    case SH_OP_V_RCP_F64: set_vf64_from_f32(t, in, op2(t, SpvOpFDiv, F, cf(t, 1.0f), f64_as_f32(t, in, 0))); break;
    case SH_OP_V_RSQ_F64: set_vf64_from_f32(t, in, ext1(t, F, GLSLstd450InverseSqrt, f64_as_f32(t, in, 0))); break;
    case SH_OP_V_SQRT_F64: set_vf64_from_f32(t, in, ext1(t, F, GLSLstd450Sqrt, f64_as_f32(t, in, 0))); break;
    case SH_OP_V_DIV_SCALE_F64: /* no scaling, as the f32 one: VCC stays clear */
        set_vf64(t, in, vsrc_f64(t, in, 0));
        set_mask(t, in->sdst, spv_const_bool(&t->b, false));
        break;
    case SH_OP_V_DIV_FIXUP_F64: /* the quotient, numerator / denominator, directly */
        set_vf64_from_f32(t, in, op2(t, SpvOpFDiv, F, f64_as_f32(t, in, 2), f64_as_f32(t, in, 1)));
        break;
    case SH_OP_V_DIV_FIXUP_F16: { /* numerator / denominator, in f32 */
        uint32_t os = in->opsel, ab = in->abs, ng = in->neg;
        uint32_t den = srch(t, in, 1, (os >> 1) & 1, (ab >> 1) & 1, (ng >> 1) & 1), num = srch(t, in, 2, (os >> 2) & 1, (ab >> 2) & 1, (ng >> 2) & 1);
        uint32_t f = op2(t, SpvOpFDiv, F, num, den);
        if (in->clamp) f = clamp01(t, f);
        set_v16(t, in, f16_result(t, f, (uint32_t[]){num, den}, 2));
        break;
    }
    case SH_OP_V_LERP_U8: { /* per byte: (a + b + bit 0 of c's byte) / 2 */
        uint32_t a = UA(), b = UB(), c = UC(), r = cu(t, 0);
        for (uint32_t off = 0; off < 32; off += 8) {
            uint32_t s = op2(t, SpvOpIAdd, U, op2(t, SpvOpIAdd, U, op3(t, SpvOpBitFieldUExtract, U, a, cu(t, off), cu(t, 8)),
                                                  op3(t, SpvOpBitFieldUExtract, U, b, cu(t, off), cu(t, 8))),
                             op3(t, SpvOpBitFieldUExtract, U, c, cu(t, off), cu(t, 1)));
            r = op2(t, SpvOpBitwiseOr, U, r, op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpShiftRightLogical, U, s, cu(t, 1)), cu(t, off)));
        }
        set_vu(t, in, r);
        break;
    }
    case SH_OP_V_SAD_U8: set_vu(t, in, op2(t, SpvOpIAdd, U, field_sad(t, UA(), UB(), 8, false), UC())); break;
    case SH_OP_V_SAD_HI_U8: set_vu(t, in, op2(t, SpvOpIAdd, U, op2(t, SpvOpShiftLeftLogical, U, field_sad(t, UA(), UB(), 8, false), cu(t, 16)), UC())); break;
    case SH_OP_V_SAD_U16: set_vu(t, in, op2(t, SpvOpIAdd, U, field_sad(t, UA(), UB(), 16, false), UC())); break;
    case SH_OP_V_MSAD_U8: set_vu(t, in, op2(t, SpvOpIAdd, U, field_sad(t, UA(), UB(), 8, true), UC())); break;
    case SH_OP_V_MAD_I16: case SH_OP_V_MAD_U16: case SH_OP_V_MAX3_I16: case SH_OP_V_MAX3_U16: case SH_OP_V_MIN3_I16:
    case SH_OP_V_MIN3_U16: case SH_OP_V_MED3_I16: case SH_OP_V_MED3_U16: {
        uint32_t os = in->opsel;
        uint32_t a = src16(t, in, 0, os & 1, false), b = src16(t, in, 1, (os >> 1) & 1, false), c = src16(t, in, 2, (os >> 2) & 1, false);
        bool s = op == SH_OP_V_MAD_I16 || op == SH_OP_V_MAX3_I16 || op == SH_OP_V_MIN3_I16 || op == SH_OP_V_MED3_I16;
        uint32_t mn = s ? GLSLstd450SMin : GLSLstd450UMin, mx = s ? GLSLstd450SMax : GLSLstd450UMax, T = s ? I : U, v;
        uint32_t x = s ? ibits(t, bfe_i(t, a, cu(t, 0), cu(t, 16))) : a, y = s ? ibits(t, bfe_i(t, b, cu(t, 0), cu(t, 16))) : b;
        uint32_t z = s ? ibits(t, bfe_i(t, c, cu(t, 0), cu(t, 16))) : c;
        if (op == SH_OP_V_MAD_I16 || op == SH_OP_V_MAD_U16) {
            v = op2(t, SpvOpIAdd, T, op2(t, SpvOpIMul, T, x, y), z);
            if (in->clamp) v = s ? ext3(t, I, GLSLstd450SClamp, v, ibits(t, cu(t, 0xFFFF8000u)), ibits(t, cu(t, 0x7FFF)))
                                 : ext2(t, U, GLSLstd450UMin, v, cu(t, 0xFFFF));
        } else if (op == SH_OP_V_MAX3_I16 || op == SH_OP_V_MAX3_U16) {
            v = ext2(t, T, mx, ext2(t, T, mx, x, y), z);
        } else if (op == SH_OP_V_MIN3_I16 || op == SH_OP_V_MIN3_U16) {
            v = ext2(t, T, mn, ext2(t, T, mn, x, y), z);
        } else {
            v = ext2(t, T, mx, ext2(t, T, mn, x, y), ext2(t, T, mn, ext2(t, T, mx, x, y), z));
        }
        set_v16(t, in, s ? ubits(t, v) : v);
        break;
    }
    case SH_OP_V_MAD_I32_I16: case SH_OP_V_MAD_U32_U16: {
        uint32_t os = in->opsel;
        uint32_t a = src16(t, in, 0, os & 1, false), b = src16(t, in, 1, (os >> 1) & 1, false);
        if (op == SH_OP_V_MAD_I32_I16) a = bfe_i(t, a, cu(t, 0), cu(t, 16)), b = bfe_i(t, b, cu(t, 0), cu(t, 16));
        set_vu(t, in, op2(t, SpvOpIAdd, U, op2(t, SpvOpIMul, U, a, b), UC()));
        break;
    }
    case SH_OP_V_DOT2C_F32_F16: case SH_OP_V_DOT2_F32_F16: {
        /* v_dot2c accumulates into D; v_dot2 (packed) adds an f32 c, halves by op_sel / op_sel_hi */
        bool pk = op == SH_OP_V_DOT2_F32_F16;
        uint32_t lo_sel = pk ? in->opsel : 0, hi_sel = pk ? in->opsel_hi : 3, ng = pk ? in->neg : 0, nh = pk ? in->neg_hi : 0;
        uint32_t a0 = srch(t, in, 0, lo_sel & 1, false, ng & 1), b0 = srch(t, in, 1, (lo_sel >> 1) & 1, false, (ng >> 1) & 1);
        uint32_t a1 = srch(t, in, 0, hi_sel & 1, false, nh & 1), b1 = srch(t, in, 1, (hi_sel >> 1) & 1, false, (nh >> 1) & 1);
        uint32_t c = pk ? fbits(t, UC()) : fbits(t, get_v(t, in->dst - RDNA2_OP_VGPR));
        if (pk && (ng & 4)) c = op1(t, SpvOpFNegate, F, c);
        uint32_t f = op2(t, SpvOpFAdd, F, op2(t, SpvOpFAdd, F, op2(t, SpvOpFMul, F, a0, b0), op2(t, SpvOpFMul, F, a1, b1)), c);
        if (in->clamp) f = clamp01(t, f);
        set_v(t, in->dst - RDNA2_OP_VGPR, ubits(t, f));
        break;
    }
    case SH_OP_V_DOT4C_I32_I8: set_vu(t, in, int_dot(t, UA(), UB(), get_v(t, in->dst - RDNA2_OP_VGPR), 8, true, false)); break;
    case SH_OP_V_DOT2_I32_I16: case SH_OP_V_DOT2_U32_U16: case SH_OP_V_DOT4_I32_I8: case SH_OP_V_DOT4_U32_U8:
    case SH_OP_V_DOT8_I32_I4: case SH_OP_V_DOT8_U32_U4: {
        uint32_t bits = op == SH_OP_V_DOT2_I32_I16 || op == SH_OP_V_DOT2_U32_U16 ? 16 : op == SH_OP_V_DOT4_I32_I8 || op == SH_OP_V_DOT4_U32_U8 ? 8 : 4;
        bool s = op == SH_OP_V_DOT2_I32_I16 || op == SH_OP_V_DOT4_I32_I8 || op == SH_OP_V_DOT8_I32_I4;
        set_v(t, in->dst - RDNA2_OP_VGPR, int_dot(t, UA(), UB(), UC(), bits, s, in->clamp));
        break;
    }
    default: fail(t, "vector instruction not supported"); break;
    }
#undef FA
#undef FB
#undef FC
#undef UA
#undef UB
#undef UC
#undef F2
#undef U2
#undef FX1
}

/* ---- Memory ---- */

static uint32_t smem_count(uint32_t op) {
    switch (op) {
    case SH_OP_S_LOAD_DWORDX2: case SH_OP_S_BUFFER_LOAD_DWORDX2: return 2;
    case SH_OP_S_LOAD_DWORDX4: case SH_OP_S_BUFFER_LOAD_DWORDX4: return 4;
    case SH_OP_S_LOAD_DWORDX8: case SH_OP_S_BUFFER_LOAD_DWORDX8: return 8;
    case SH_OP_S_LOAD_DWORDX16: case SH_OP_S_BUFFER_LOAD_DWORDX16: return 16;
    default: return 1;
    }
}

/* Render targets may be drawn smaller than the guest's size (sh_set_rt_scale): pixel
 * shaders then take their position back at the guest's scale (SH_U_VPORT in a PS) */
static bool frag_scale_on(void) { return g_rt_scale; }

static uint32_t uniform_index(tr *t, uint32_t index) {
    if (!t->uniform_shift) return index;
    uint32_t value;
    if (spv_const_u32_value(&t->b, index, &value))
        return cu(t, sh_uniform_index(t->uniform_shift, value));
    uint32_t U=t->t_u32,cut=SH_U_VPORT-t->uniform_shift;
    uint32_t tail=op2(t,SpvOpUGreaterThanEqual,t->t_bool,index,cu(t,SH_U_VPORT));
    uint32_t gap=op2(t,SpvOpUGreaterThanEqual,t->t_bool,index,cu(t,cut));
    uint32_t low=sel(t,U,gap,cu(t,cut-1),index);
    return sel(t,U,tail,op2(t,SpvOpISub,U,index,cu(t,t->uniform_shift)),low);
}

static uint32_t sb_length(tr *t, uint32_t var) {
    uint32_t n=spv_op(&t->b,SpvOpArrayLength,t->t_u32,(uint32_t[]){var,0},2);
    return var==t->u_var && t->uniform_shift ? op2(t,SpvOpIAdd,t->t_u32,n,cu(t,t->uniform_shift)) : n;
}

static uint32_t sb_ptr(tr *t, uint32_t var, uint32_t index) {
    if (var==t->u_var) index=uniform_index(t,index);
    uint32_t a[3] = {var, cu(t, 0), index};
    return spv_op(&t->b, SpvOpAccessChain, t->p_sb_u32, a, 3);
}
static uint32_t sb_load(tr *t, uint32_t var, uint32_t index) { return ld(t, t->t_u32, sb_ptr(t, var, index)); }

static uint32_t buf_word(tr *t, uint32_t var, uint32_t dword_index);
static uint32_t buffer_dwords(tr *t, uint32_t var);

static uint32_t guest_pointer(tr *t, uint32_t sgpr) {
    uint32_t lo = op1(t, SpvOpUConvert, t->t_u64, get_s(t, sgpr));
    uint32_t hi = op1(t, SpvOpUConvert, t->t_u64,
                     op2(t, SpvOpBitwiseAnd, t->t_u32, get_s(t, sgpr + 1), cu(t, 0xffff)));
    return op2(t, SpvOpBitwiseOr, t->t_u64, lo, op2(t, SpvOpShiftLeftLogical, t->t_u64, hi, cu(t, 32)));
}

/* A real guest address, relative to an imported allocation. Check in 64 bits
 * before narrowing: a different high word must never alias this allocation. */
static uint32_t mapped_offset(tr *t, int site, uint32_t address, uint32_t bytes, uint32_t *valid) {
    uint32_t U = t->t_u32, L = t->t_u64, B = t->t_bool;
    uint32_t meta = sb_load(t, t->u_var, cu(t, SH_U_SITE + (uint32_t)site));
    if(t->shapes[site].address_map==2) {
        uint32_t result=cu(t,0);
        *valid=spv_const_bool(&t->b,false);
        uint32_t count=t->shapes[site].buf_stride;
        if(!count || count>64 || t->p->sites[site].store) {fail(t,"invalid compact readonly ranges");return result;}
        for(uint32_t k=0;k<count;k++) {
            uint32_t m=op2(t,SpvOpIAdd,U,meta,cu(t,4*k));
            uint32_t lo=op1(t,SpvOpUConvert,L,sb_load(t,t->u_var,m));
            uint32_t hi=op1(t,SpvOpUConvert,L,sb_load(t,t->u_var,op2(t,SpvOpIAdd,U,m,cu(t,1))));
            uint32_t base=op2(t,SpvOpBitwiseOr,L,lo,op2(t,SpvOpShiftLeftLogical,L,hi,cu(t,32)));
            uint32_t size=op1(t,SpvOpUConvert,L,sb_load(t,t->u_var,op2(t,SpvOpIAdd,U,m,cu(t,2))));
            uint32_t delta=op2(t,SpvOpISub,L,address,base);
            uint32_t end=op2(t,SpvOpIAdd,L,delta,op1(t,SpvOpUConvert,L,cu(t,bytes)));
            uint32_t inside=op2(t,SpvOpLogicalAnd,B,op2(t,SpvOpUGreaterThanEqual,B,address,base),
                                 op2(t,SpvOpULessThanEqual,B,end,size));
            uint32_t offset=sb_load(t,t->u_var,op2(t,SpvOpIAdd,U,m,cu(t,3)));
            uint32_t packed=op2(t,SpvOpIAdd,U,offset,op1(t,SpvOpUConvert,U,delta));
            result=sel(t,U,inside,packed,result);
            *valid=op2(t,SpvOpLogicalOr,B,*valid,inside);
        }
        return result;
    }
    uint32_t lo = op1(t, SpvOpUConvert, L, sb_load(t, t->u_var, meta));
    uint32_t hi = op1(t, SpvOpUConvert, L, sb_load(t, t->u_var, op2(t, SpvOpIAdd, U, meta, cu(t, 1))));
    uint32_t base = op2(t, SpvOpBitwiseOr, L, lo, op2(t, SpvOpShiftLeftLogical, L, hi, cu(t, 32)));
    uint32_t delta = op2(t, SpvOpISub, L, address, base);
    uint32_t len = spv_op(&t->b, SpvOpArrayLength, U, (uint32_t[]){t->site_var[site], 0}, 2);
    uint32_t size = op2(t, SpvOpShiftLeftLogical, L, op1(t, SpvOpUConvert, L, len), cu(t, 2));
    uint32_t end = op2(t, SpvOpIAdd, L, delta, op1(t, SpvOpUConvert, L, cu(t, bytes)));
    *valid = op2(t, SpvOpLogicalAnd, B, op2(t, SpvOpUGreaterThanEqual, B, address, base),
                 op2(t, SpvOpULessThanEqual, B, end, size));
    return sel(t, U, *valid, op1(t, SpvOpUConvert, U, delta), cu(t, 0));
}

static void smem(tr *t, const rdna2_inst *in, uint32_t op, int site) {
    uint32_t n = smem_count(op);
    if (t->shapes[site].address_map && t->p->sites[site].kind == SH_SITE_SMEM) {
        uint32_t U = t->t_u32, L = t->t_u64;
        uint32_t off = cu(t, (uint32_t)in->offset);
        if (in->soffset != RDNA2_OP_NULL) off = op2(t, SpvOpIAdd, U, off, src_u(t, in, in->soffset));
        off = op2(t, SpvOpBitwiseAnd, U, off, cu(t, ~3u));
        uint32_t address = op2(t, SpvOpIAdd, L, guest_pointer(t, in->sbase), op1(t, SpvOpUConvert, L, off));
        for (uint32_t k = 0; k < n; k++) {
            uint32_t valid;
            uint32_t at = op2(t, SpvOpIAdd, L, address, op1(t, SpvOpUConvert, L, cu(t, 4 * k)));
            uint32_t relative = mapped_offset(t, site, at, 4, &valid);
            uint32_t v = buf_word(t, t->site_var[site], op2(t, SpvOpShiftRightLogical, U, relative, cu(t, 2)));
            set_s(t, in->dst + k, sel(t, U, valid, v, cu(t, 0)));
        }
        return;
    }
    if (t->p->sites[site].kind == SH_SITE_SBUFFER) {
        /* S_BUFFER_LOAD offsets may come from v_readfirstlane in a loop.
         * Read the staged GPU buffer with the current scalar offset, rather
         * than reusing the evaluator's initial CPU snapshot on every visit. */
        uint32_t off = cu(t, (uint32_t)in->offset);
        if (in->soffset != RDNA2_OP_NULL)
            off = op2(t, SpvOpIAdd, t->t_u32, off, src_u(t, in, in->soffset));
        off = op2(t, SpvOpBitwiseAnd, t->t_u32, off, cu(t, ~3u));
        uint32_t stride = op2(t, SpvOpBitwiseAnd, t->t_u32,
                              op2(t, SpvOpShiftRightLogical, t->t_u32, get_s(t, in->sbase + 1), cu(t, 16)), cu(t, 0x3fff));
        uint32_t records = get_s(t, in->sbase + 2);
        uint32_t limit = op2(t, SpvOpIMul, t->t_u64, op1(t, SpvOpUConvert, t->t_u64, records),
                            op1(t, SpvOpUConvert, t->t_u64,
                                 sel(t, t->t_u32, op2(t, SpvOpINotEqual, t->t_bool, stride, cu(t, 0)), stride, cu(t, 1))));
        uint32_t dw = op2(t, SpvOpShiftRightLogical, t->t_u32, off, cu(t, 2));
        /* SH_BUFFER_SHIFTED: the binding starts SH_U_SITE bytes before the descriptor base
         * (direct memory reads bind from an aligned offset below it) */
        uint32_t shift_dw = 0;
        if (t->shapes[site].buffer_length == SH_BUFFER_SHIFTED && !t->shapes[site].address_map)
            shift_dw = op2(t, SpvOpShiftRightLogical, t->t_u32, sb_load(t, t->u_var, cu(t, SH_U_SITE + (uint32_t)site)), cu(t, 2));
        for (uint32_t k = 0; k < n; k++) {
            uint32_t end = op2(t, SpvOpIAdd, t->t_u64, op1(t, SpvOpUConvert, t->t_u64, off), op1(t, SpvOpUConvert, t->t_u64, cu(t, (k + 1) * 4)));
            uint32_t in_range = op2(t, SpvOpULessThanEqual, t->t_bool, end, limit);
            uint32_t index = op2(t, SpvOpIAdd, t->t_u32, dw, cu(t, k));
            if (t->shapes[site].address_map) {
                uint32_t valid;
                uint32_t relative = op2(t, SpvOpIAdd, t->t_u64, op1(t, SpvOpUConvert, t->t_u64, off),
                                        op1(t, SpvOpUConvert, t->t_u64, cu(t, 4 * k)));
                uint32_t address = op2(t, SpvOpIAdd, t->t_u64, guest_pointer(t, in->sbase), relative);
                relative = mapped_offset(t, site, address, 4, &valid);
                index = op2(t, SpvOpShiftRightLogical, t->t_u32, relative, cu(t, 2));
                in_range = op2(t, SpvOpLogicalAnd, t->t_bool, in_range, valid);
            }
            if (shift_dw) index = op2(t, SpvOpIAdd, t->t_u32, index, shift_dw);
            uint32_t v = buf_word(t, t->site_var[site], index);
            set_s(t, in->dst + k, sel(t, t->t_u32, in_range, v, cu(t, 0)));
        }
        return;
    }
    uint32_t base = sb_load(t, t->u_var, cu(t, SH_U_SITE + (uint32_t)site));
    for (uint32_t k = 0; k < n; k++) {
        set_s(t, in->dst + k, sb_load(t, t->u_var, op2(t, SpvOpIAdd, t->t_u32, base, cu(t, k))));
    }
}

/* Byte offset of a buffer access relative to the descriptor base, and whether its bytes are in range. */
static uint32_t buffer_offset(tr *t, const rdna2_inst *in, int site, const sh_site_shape *sh, uint32_t bytes, uint32_t *in_range) {
    uint32_t U = t->t_u32;
    uint32_t idx = cu(t, 0), voff = cu(t, 0);
    if (in->idxen) idx = get_v(t, in->vaddr);
    if (in->offen) voff = get_v(t, in->vaddr + (in->idxen ? 1u : 0u));
    uint32_t soff = src_u(t, in, in->soffset);
    uint32_t stride = sh->address_map ? op3(t, SpvOpBitFieldUExtract, U, get_s(t, in->srsrc + 1), cu(t, 16), cu(t, 14))
                                      : cu(t, sh->buf_stride);
    uint32_t off = op2(t, SpvOpIAdd, U, op2(t, SpvOpIMul, U, idx, stride), voff);
    off = op2(t, SpvOpIAdd, U, off, op2(t, SpvOpIAdd, U, soff, cu(t, (uint32_t)in->offset)));
    uint32_t records = get_s(t, in->srsrc + 2), B = t->t_bool;
    *in_range = !sh->address_map && sh->buf_stride && in->idxen ? op2(t, SpvOpULessThan, B, idx, records)
                                            : op2(t, SpvOpULessThan, B, off, records);
    if (sh->address_map || !sh->buf_stride || !in->idxen) {
        /* raw accesses, by V# OOB_SELECT [29:28]: 2 checks only NUM_RECORDS != 0 (Dragon Ball reads
         * a dword at offsets past a NUM_RECORDS of 1), 3 the whole access, the others the offset */
        uint32_t oob = op3(t, SpvOpBitFieldUExtract, U, get_s(t, in->srsrc + 3), cu(t, 28), cu(t, 2));
        uint32_t end = op2(t, SpvOpIAdd, U, off, cu(t, bytes));
        if (!sh->address_map && !in->idxen) {
            /* a structured buffer (V# stride != 0) without IDXEN is record 0: OOB_SELECT 0 also checks
             * the offset against the stride, 1 only the index (Wolverine's BVH build stores its counts
             * at offset 124 of a single 224-byte record) */
            uint32_t rt_stride = op3(t, SpvOpBitFieldUExtract, U, get_s(t, in->srsrc + 1), cu(t, 16), cu(t, 14));
            uint32_t any = op2(t, SpvOpINotEqual, B, records, cu(t, 0));
            uint32_t record = sel(t, B, op2(t, SpvOpIEqual, B, oob, cu(t, 0)),
                                  op2(t, SpvOpLogicalAnd, B, any, op2(t, SpvOpULessThanEqual, B, end, rt_stride)), any);
            *in_range = sel(t, B, op2(t, SpvOpINotEqual, B, rt_stride, cu(t, 0)), record, *in_range);
        }
        *in_range = sel(t, B, op2(t, SpvOpIEqual, B, oob, cu(t, 2)), op2(t, SpvOpINotEqual, B, records, cu(t, 0)),
                        sel(t, B, op2(t, SpvOpIEqual, B, oob, cu(t, 3)), op2(t, SpvOpULessThanEqual, B, end, records), *in_range));
        if (sh->address_map && in->idxen)
            *in_range = sel(t, B, op2(t, SpvOpINotEqual, B, stride, cu(t, 0)),
                            op2(t, SpvOpULessThan, B, idx, records), *in_range);
    }
    if (t->info->mesh_indexed & SH_GCN_SWIZZLE) {
        /* GCN V# (raw words in the SGPRs): ADD_TID_ENABLE word3 [23] adds the lane to the
         * index; SWIZZLE_ENABLE word1 [31] interleaves ELEMENT_SIZE [20:19] (2 << n bytes)
         * elements of INDEX_STRIDE [22:21] (8 << n) lanes:
         * soffset + (index / is) * stride * is + (offset / es) * es * is + (index % is) * es + offset % es */
        uint32_t w1 = get_s(t, in->srsrc + 1), w3 = get_s(t, in->srsrc + 3);
        uint32_t tid = op2(t, SpvOpIEqual, B, op2(t, SpvOpBitwiseAnd, U, w3, cu(t, 1u << 23)), cu(t, 1u << 23));
        uint32_t swz = op2(t, SpvOpIEqual, B, op2(t, SpvOpBitwiseAnd, U, w1, cu(t, 0x80000000u)), cu(t, 0x80000000u));
        uint32_t index = op2(t, SpvOpIAdd, U, idx, sel(t, U, tid, wave_lane(t), cu(t, 0)));
        uint32_t within = op2(t, SpvOpIAdd, U, voff, cu(t, (uint32_t)in->offset));
        uint32_t es = op2(t, SpvOpShiftLeftLogical, U, cu(t, 2), op3(t, SpvOpBitFieldUExtract, U, w3, cu(t, 19), cu(t, 2)));
        uint32_t is = op2(t, SpvOpShiftLeftLogical, U, cu(t, 8), op3(t, SpvOpBitFieldUExtract, U, w3, cu(t, 21), cu(t, 2)));
        uint32_t rstride = op3(t, SpvOpBitFieldUExtract, U, w1, cu(t, 16), cu(t, 14));
        uint32_t swizzled = op2(t, SpvOpIMul, U, op2(t, SpvOpIMul, U, op2(t, SpvOpUDiv, U, index, is), rstride), is);
        swizzled = op2(t, SpvOpIAdd, U, swizzled, op2(t, SpvOpIMul, U, op2(t, SpvOpIMul, U, op2(t, SpvOpUDiv, U, within, es), es), is));
        swizzled = op2(t, SpvOpIAdd, U, swizzled, op2(t, SpvOpIMul, U, op2(t, SpvOpUMod, U, index, is), es));
        swizzled = op2(t, SpvOpIAdd, U, swizzled, op2(t, SpvOpUMod, U, within, es));
        uint32_t linear = op2(t, SpvOpIAdd, U, op2(t, SpvOpIMul, U, index, rstride), within);
        uint32_t ring = op2(t, SpvOpIAdd, U, soff, sel(t, U, swz, swizzled, linear));
        uint32_t special = op2(t, SpvOpLogicalOr, B, tid, swz);
        off = sel(t, U, special, ring, off);
        *in_range = sel(t, B, special, op2(t, SpvOpULessThanEqual, B, op2(t, SpvOpIAdd, U, ring, cu(t, bytes)),
                                                op2(t, SpvOpShiftLeftLogical, U, buffer_dwords(t, t->site_var[site]), cu(t, 2))),
                        *in_range);
    }
    /* SH_BUFFER_SHIFTED: the binding starts before the descriptor base (interleaved vertex
     * attributes share one copy of their buffer); SH_U_SITE holds the byte distance */
    if (sh->buffer_length == SH_BUFFER_SHIFTED && !sh->address_map)
        off = op2(t, SpvOpIAdd, U, off, sb_load(t, t->u_var, cu(t, SH_U_SITE + (uint32_t)site)));
    return off;
}

static uint32_t buffer_dwords(tr *t, uint32_t var) {
    /* Some NVIDIA drivers return zero from OpArrayLength for bindings above
     * 2 GiB. The renderer supplies the actual descriptor range, never the
     * guest's declared size, so clipped/fallback bindings stay bounded too.
     * Address-mapped sites retain their existing SH_U_SITE metadata. */
    for(uint32_t s=0;s<t->p->site_count;s++)
        if(t->site_var[s]==var && t->shapes[s].buffer_length==SH_BUFFER_LENGTH && !t->shapes[s].address_map &&
           (t->p->sites[s].kind==SH_SITE_BUFFER || t->p->sites[s].kind==SH_SITE_SBUFFER))
            return sb_load(t,t->u_var,cu(t,SH_U_SITE+s));
    return spv_op(&t->b,SpvOpArrayLength,t->t_u32,(uint32_t[]){var,0},2);
}

/* Dword at `dword_index` of site buffer `var`, bounded by the actual binding. */
static uint32_t buf_word(tr *t, uint32_t var, uint32_t dword_index) {
    uint32_t len = buffer_dwords(t,var);
    uint32_t ok = op2(t, SpvOpULessThan, t->t_bool, dword_index, len);
    uint32_t v = sb_load(t, var, sel(t, t->t_u32, ok, dword_index, cu(t, 0)));
    return sel(t, t->t_u32, ok, v, cu(t, 0));
}

static uint32_t convert_comp(tr *t, uint32_t raw, uint32_t bits, uint32_t kind) {
    uint32_t F = t->t_f32;
    if (bits == 32) return raw;
    double maxu = (double)((1ull << bits) - 1), maxs = (double)((1ull << (bits - 1)) - 1);
    uint32_t sx = ibits(t, bfe_i(t, raw, cu(t, 0), cu(t, bits)));
    switch (kind) {
    case 0: return ubits(t, op2(t, SpvOpFMul, F, op1(t, SpvOpConvertUToF, F, raw), cf(t, (float)(1.0 / maxu))));
    case 1: return ubits(t, ext2(t, F, GLSLstd450FMax, op2(t, SpvOpFMul, F, op1(t, SpvOpConvertSToF, F, sx), cf(t, (float)(1.0 / maxs))), cf(t, -1.0f)));
    case 2: return ubits(t, op1(t, SpvOpConvertUToF, F, raw));
    case 3: return ubits(t, op1(t, SpvOpConvertSToF, F, sx));
    case 4: return raw;
    case 5: return ubits(t, sx);
    default:
        if (bits == 16) return ubits(t, op2(t, SpvOpCompositeExtract, F, ext1(t, t->t_v2f, GLSLstd450UnpackHalf2x16, raw), 0));
        return cu(t, 0); /* packed float formats: not yet */
    }
}

static uint32_t format_one(uint32_t kind) { return kind == 4 || kind == 5 ? 1u : 0x3F800000u; }

static uint32_t op_components(const char *name) {
    const char *s = strrchr(name, '_');
    if (!s) return 1;
    if (!strcmp(s, "_xyzw") || !strcmp(s, "_dwordx4")) return 4;
    if (!strcmp(s, "_xyz") || !strcmp(s, "_dwordx3")) return 3;
    if (!strcmp(s, "_xy") || !strcmp(s, "_dwordx2")) return 2;
    return 1;
}

/* A component in a buffer format's encoding (the inverse of convert_comp) */
static uint32_t pack_comp(tr *t, uint32_t v, uint32_t bits, uint32_t kind) {
    uint32_t F = t->t_f32, U = t->t_u32, I = t->t_i32;
    if (bits == 32) return v;
    double maxu = (double)((1ull << bits) - 1), maxs = (double)((1ull << (bits - 1)) - 1);
    uint32_t f = fbits(t, v);
    switch (kind) {
    case 0: return op1(t, SpvOpConvertFToU, U, ext1(t, F, GLSLstd450RoundEven, op2(t, SpvOpFMul, F, ext3(t, F, GLSLstd450FClamp, f, cf(t, 0), cf(t, 1)), cf(t, (float)maxu))));
    case 1: return ubits(t, op1(t, SpvOpConvertFToS, I, ext1(t, F, GLSLstd450RoundEven, op2(t, SpvOpFMul, F, ext3(t, F, GLSLstd450FClamp, f, cf(t, -1), cf(t, 1)), cf(t, (float)maxs)))));
    case 2: return op1(t, SpvOpConvertFToU, U, f);
    case 3: return ubits(t, op1(t, SpvOpConvertFToS, I, f));
    case 4: case 5: return v;
    default: return bits == 16 ? f2h(t, f) : cu(t, 0);
    }
}

/* Read-modify-writes done with a compare-exchange loop (atomic_loop) */
enum { RMW_INC, RMW_DEC, RMW_RSUB, RMW_MSKOR, RMW_FADD, RMW_FMIN, RMW_FMAX, RMW_WRAP };
static uint32_t atomic_loop(tr *t, uint32_t ptr, uint32_t scope, int kind, uint32_t data, uint32_t data2);

/* SPIR-V atomic of a 32-bit integer atomic's name tail ("add", "umax"...), 0 if none */
static uint32_t atomic_op_of(const char *k) {
    static const struct { const char *n; uint32_t op; } k_ops[] = {
        {"add", SpvOpAtomicIAdd}, {"sub", SpvOpAtomicISub}, {"smin", SpvOpAtomicSMin}, {"umin", SpvOpAtomicUMin},
        {"smax", SpvOpAtomicSMax}, {"umax", SpvOpAtomicUMax}, {"and", SpvOpAtomicAnd}, {"or", SpvOpAtomicOr},
        {"xor", SpvOpAtomicXor}, {"swap", SpvOpAtomicExchange}, {"cmpswap", SpvOpAtomicCompareExchange},
        {"fcmpswap", SpvOpAtomicCompareExchange} /* float compare as bits: differs only for -0 and NaNs */};
    for (size_t i = 0; i < PS5C_ARRAY_COUNT(k_ops); i++) {
        if (!strcmp(k, k_ops[i].n)) return k_ops[i].op;
    }
    return 0;
}

/* buffer_atomic_<op> / global_atomic_<op> (the same name length): on the dword at the offset;
 * glc returns the old value to VGPR `ret` */
static void buffer_atomic(tr *t, const rdna2_inst *in, uint32_t var, uint32_t dw, uint32_t in_range, uint32_t ret) {
    uint32_t U = t->t_u32;
    uint32_t aop = atomic_op_of(in->name + strlen("buffer_atomic_"));
    const char *k = in->name + strlen("buffer_atomic_");
    if (!strcmp(k, "fmin") || !strcmp(k, "fmax")) {
        /* Float min/max with integer atomics: non-negative floats order like
         * their bits as signed integers, negative ones in reverse as unsigned
         * (GTA V's prologue). fmin: v >= 0 ? smin : umax; fmax: v >= 0 ? smax : umin. */
        bool mn = k[1] == 'm' && k[2] == 'i';
        uint32_t cond = op2(t, SpvOpLogicalAnd, t->t_bool, in_range, active(t));
        uint32_t data = get_v(t, in->vdata);
        uint32_t neg = op2(t, SpvOpINotEqual, t->t_bool, op2(t, SpvOpBitwiseAnd, U, data, cu(t, 0x80000000u)), cu(t, 0));
        uint32_t res = spv_local_var(&t->b, t->p_fn_u32);
        st(t, res, data);
        uint32_t len = 0;
        for (int sign = 0; sign < 2; sign++) {
            uint32_t want = sign ? neg : op1(t, SpvOpLogicalNot, t->t_bool, neg);
            uint32_t merge = begin_if(t, op2(t, SpvOpLogicalAnd, t->t_bool, cond, want));
            len = buffer_dwords(t,var);
            uint32_t ptr = sb_ptr(t, var, ext2(t, U, GLSLstd450UMin, dw, op2(t, SpvOpISub, U, len, cu(t, 1))));
            uint32_t o = sign ? (mn ? SpvOpAtomicUMax : SpvOpAtomicUMin) : (mn ? SpvOpAtomicSMin : SpvOpAtomicSMax);
            st(t, res, spv_op(&t->b, o, U, (uint32_t[]){ptr, cu(t, 1), cu(t, 0), data}, 4));
            end_if(t, merge);
        }
        if (in->glc) set_v(t, ret, ld(t, U, res));
        return;
    }
    if (!strcmp(k, "or_x2") || !strcmp(k, "and_x2") || !strcmp(k, "xor_x2")) {
        /* bitwise on each dword of the pair: exactly the 64-bit result */
        uint32_t bop = k[0] == 'o' ? SpvOpAtomicOr : k[0] == 'a' ? SpvOpAtomicAnd : SpvOpAtomicXor;
        uint32_t cond = op2(t, SpvOpLogicalAnd, t->t_bool, in_range, active(t));
        uint32_t r[2];
        for (int h = 0; h < 2; h++) {
            r[h] = spv_local_var(&t->b, t->p_fn_u32);
            st(t, r[h], get_v(t, in->vdata + (uint32_t)h));
        }
        uint32_t merge = begin_if(t, cond);
        uint32_t len = buffer_dwords(t,var);
        uint32_t last = op2(t, SpvOpISub, U, len, cu(t, 1));
        for (int h = 0; h < 2; h++) {
            uint32_t p = sb_ptr(t, var, ext2(t, U, GLSLstd450UMin, op2(t, SpvOpIAdd, U, dw, cu(t, (uint32_t)h)), last));
            st(t, r[h], spv_op(&t->b, bop, U, (uint32_t[]){p, cu(t, 1), cu(t, 0), get_v(t, in->vdata + (uint32_t)h)}, 4));
        }
        end_if(t, merge);
        if (in->glc) {
            set_v(t, ret, ld(t, U, r[0]));
            set_v(t, ret + 1, ld(t, U, r[1]));
        }
        return;
    }
    if (!strcmp(k, "inc") || !strcmp(k, "dec")) {
        uint32_t data = get_v(t, in->vdata), res = spv_local_var(&t->b, t->p_fn_u32);
        st(t, res, data);
        uint32_t merge = begin_if(t, op2(t, SpvOpLogicalAnd, t->t_bool, in_range, active(t)));
        uint32_t len = buffer_dwords(t,var);
        uint32_t ptr = sb_ptr(t, var, ext2(t, U, GLSLstd450UMin, dw, op2(t, SpvOpISub, U, len, cu(t, 1))));
        st(t, res, atomic_loop(t, ptr, 1, k[0] == 'i' ? RMW_INC : RMW_DEC, data, 0));
        end_if(t, merge);
        if (in->glc) set_v(t, ret, ld(t, U, res));
        return;
    }
    if (!strcmp(k, "swap_x2") || !strcmp(k, "cmpswap_x2") || !strcmp(k, "add_x2") || !strcmp(k, "sub_x2") ||
        !strcmp(k, "smin_x2") || !strcmp(k, "umin_x2") || !strcmp(k, "smax_x2") || !strcmp(k, "umax_x2") ||
        !strcmp(k, "inc_x2") || !strcmp(k, "dec_x2")) {
        /* 64-bit atomics on two dwords: each dword alone is atomic for swap,
         * the pair is not (no 64-bit atomics on storage buffers here) */
        uint32_t L = t->t_u64;
        uint32_t cond = op2(t, SpvOpLogicalAnd, t->t_bool, in_range, active(t));
        uint32_t lo = get_v(t, in->vdata), hi = get_v(t, in->vdata + 1);
        uint32_t rlo = spv_local_var(&t->b, t->p_fn_u32), rhi = spv_local_var(&t->b, t->p_fn_u32);
        st(t, rlo, lo);
        st(t, rhi, hi);
        uint32_t merge = begin_if(t, cond);
        uint32_t len = buffer_dwords(t,var);
        uint32_t last = op2(t, SpvOpISub, U, len, cu(t, 1));
        uint32_t p0 = sb_ptr(t, var, ext2(t, U, GLSLstd450UMin, dw, last));
        uint32_t p1 = sb_ptr(t, var, ext2(t, U, GLSLstd450UMin, op2(t, SpvOpIAdd, U, dw, cu(t, 1)), last));
        if (!strcmp(k, "swap_x2")) {
            st(t, rlo, spv_op(&t->b, SpvOpAtomicExchange, U, (uint32_t[]){p0, cu(t, 1), cu(t, 0), lo}, 4));
            st(t, rhi, spv_op(&t->b, SpvOpAtomicExchange, U, (uint32_t[]){p1, cu(t, 1), cu(t, 0), hi}, 4));
        } else {
            uint32_t olo = ld(t, U, p0), ohi = ld(t, U, p1);
            uint32_t old = op2(t, SpvOpBitwiseOr, L, op1(t, SpvOpUConvert, L, olo),
                               op2(t, SpvOpShiftLeftLogical, L, op1(t, SpvOpUConvert, L, ohi), cu(t, 32)));
            uint32_t v = op2(t, SpvOpBitwiseOr, L, op1(t, SpvOpUConvert, L, lo),
                             op2(t, SpvOpShiftLeftLogical, L, op1(t, SpvOpUConvert, L, hi), cu(t, 32)));
            uint32_t nv, B = t->t_bool;
            if (k[0] == 'a') {
                nv = op2(t, SpvOpIAdd, L, old, v);
            } else if (!strcmp(k, "sub_x2")) {
                nv = op2(t, SpvOpISub, L, old, v);
            } else if (!strcmp(k, "smin_x2") || !strcmp(k, "umin_x2") || !strcmp(k, "smax_x2") || !strcmp(k, "umax_x2")) {
                bool mn = k[1] == 'm' && k[2] == 'i', sg = k[0] == 's';
                uint32_t lt = op2(t, sg ? SpvOpSLessThan : SpvOpULessThan, B, v, old);
                nv = sel(t, L, mn ? lt : op1(t, SpvOpLogicalNot, B, lt), v, old);
            } else if (!strcmp(k, "inc_x2")) {
                nv = sel(t, L, op2(t, SpvOpUGreaterThanEqual, B, old, v), c64(t, 0), op2(t, SpvOpIAdd, L, old, c64(t, 1)));
            } else if (!strcmp(k, "dec_x2")) {
                uint32_t wrap = op2(t, SpvOpLogicalOr, B, op2(t, SpvOpIEqual, B, old, c64(t, 0)), op2(t, SpvOpUGreaterThan, B, old, v));
                nv = sel(t, L, wrap, v, op2(t, SpvOpISub, L, old, c64(t, 1)));
            } else { /* cmpswap_x2: data in vdata[0:1], compare value in vdata[2:3] */
                uint32_t c = op2(t, SpvOpBitwiseOr, L, op1(t, SpvOpUConvert, L, get_v(t, in->vdata + 2)),
                                 op2(t, SpvOpShiftLeftLogical, L, op1(t, SpvOpUConvert, L, get_v(t, in->vdata + 3)), cu(t, 32)));
                nv = spv_op(&t->b, SpvOpSelect, L, (uint32_t[]){op2(t, SpvOpIEqual, t->t_bool, old, c), v, old}, 3);
            }
            st(t, p0, op1(t, SpvOpUConvert, U, nv));
            st(t, p1, op1(t, SpvOpUConvert, U, op2(t, SpvOpShiftRightLogical, L, nv, cu(t, 32))));
            st(t, rlo, olo);
            st(t, rhi, ohi);
        }
        end_if(t, merge);
        if (in->glc) {
            set_v(t, ret, ld(t, U, rlo));
            set_v(t, ret + 1, ld(t, U, rhi));
        }
        return;
    }
    if (!aop) {
        fail(t, "buffer atomic not supported");
        return;
    }
    uint32_t cond = op2(t, SpvOpLogicalAnd, t->t_bool, in_range, active(t));
    uint32_t data = get_v(t, in->vdata), cmp = aop == SpvOpAtomicCompareExchange ? get_v(t, in->vdata + 1) : 0;
    uint32_t res = spv_local_var(&t->b, t->p_fn_u32);
    st(t, res, data);
    uint32_t merge = begin_if(t, cond);
    uint32_t len = buffer_dwords(t,var);
    uint32_t ptr = sb_ptr(t, var, ext2(t, U, GLSLstd450UMin, dw, op2(t, SpvOpISub, U, len, cu(t, 1))));
    uint32_t old = aop == SpvOpAtomicCompareExchange
                 ? spv_op(&t->b, aop, U, (uint32_t[]){ptr, cu(t, 1), cu(t, 0), cu(t, 0), data, cmp}, 6)
                 : spv_op(&t->b, aop, U, (uint32_t[]){ptr, cu(t, 1), cu(t, 0), data}, 4);
    st(t, res, old);
    end_if(t, merge);
    if (in->glc) set_v(t, ret, ld(t, U, res));
}

/* GLOBAL loads and stores through the site's window (see eval_global_site):
 * byte offset = saddr form: vaddr + offset; 64-bit vaddr form: address low
 * bits - base low bits + offset. Out-of-window loads read zeros. */
#define SCRATCH_DWORDS 1024u
/* scratch_load / scratch_store: lane-private memory (spills, local arrays). The address is
 * SADDR (an SGPR, 0x7F: none) or else VADDR, plus the signed offset; a Private array of
 * SCRATCH_DWORDS per invocation stands in for the lane's slice, the index clamped to it. */
static void scratch_rw(tr *t, const char *name, bool load, uint32_t off, uint32_t d, uint32_t vdata);
static void scratch_op(tr *t, const rdna2_inst *in, uint32_t op) {
    uint32_t U = t->t_u32;
    if (!t->scratch) {
        fail(t, "scratch memory not declared");
        return;
    }
    uint32_t off = in->sbase != RDNA2_OP_NULL && in->sbase < SH_SGPRS ? get_s(t, in->sbase) : get_v(t, in->vaddr);
    off = op2(t, SpvOpIAdd, U, off, cu(t, (uint32_t)in->offset));
    scratch_rw(t, in->name, op == SH_OP_SCRATCH_LOAD, off, in->dst - RDNA2_OP_VGPR, in->vdata);
}

/* RADV/ACO scratch on GFX10: MUBUF through a V# the shader builds itself with
 * ADD_TID_ENABLE (word3 bit 23), so each lane addresses its own swizzled slice.
 * A guest buffer shared by the lanes would let every lane of a wave overwrite the
 * others (Longshot's optical flow came out constant per 8x8 wave). The V#'s word3
 * is a literal moved into the SGPR before the access; the lane-private array
 * stands in for the slice. */
static bool mubuf_scratch(const sh_program *p, uint32_t i) {
    const rdna2_inst *in = &p->insts[i];
    if (in->enc != RDNA2_ENC_MUBUF || in->idxen || in->tfe || !in->name ||
        strstr(in->name, "format") || strstr(in->name, "atomic") ||
        (strncmp(in->name, "buffer_load_", 12) && strncmp(in->name, "buffer_store_", 13)))
        return false;
    uint32_t r3 = in->srsrc + 3u;
    for (uint32_t j = i; j-- > 0;) {
        const rdna2_inst *w = &p->insts[j];
        uint32_t base, n = 1;
        if (w->enc == RDNA2_ENC_SOP1 || w->enc == RDNA2_ENC_SOP2 || w->enc == RDNA2_ENC_SOPK) {
            base = w->dst;
            n = (p->iflags[j] & SH_IF_64) ? 2 : 1;
        } else if (w->enc == RDNA2_ENC_SMEM) {
            base = w->dst;
            n = strstr(w->name ? w->name : "", "x16") ? 16 : strstr(w->name ? w->name : "", "x8") ? 8 :
                strstr(w->name ? w->name : "", "x4") ? 4 : strstr(w->name ? w->name : "", "x2") ? 2 : 1;
        } else if ((w->enc == RDNA2_ENC_VOP1 || w->enc == RDNA2_ENC_VOP2 || w->enc == RDNA2_ENC_VOP3) &&
                   w->dst < SH_SGPRS) {
            base = w->dst;
        } else {
            continue;
        }
        if (r3 < base || r3 >= base + n) continue;
        return w->enc == RDNA2_ENC_SOP1 && w->name && !strcmp(w->name, "s_mov_b32") &&
               w->src[0] == RDNA2_OP_LITERAL && ((w->literal >> 23) & 1);
    }
    return false;
}

static void scratch_buffer_op(tr *t, const rdna2_inst *in) {
    uint32_t U = t->t_u32;
    /* the wave's scratch offset (an SGPR) is the same for every access: only
     * VADDR (OFFEN), a constant SOFFSET and the instruction offset address the slice */
    uint32_t off = in->offen ? get_v(t, in->vaddr) : cu(t, 0);
    if (in->soffset >= 128 && in->soffset != RDNA2_OP_LITERAL) off = op2(t, SpvOpIAdd, U, off, src_u(t, in, in->soffset));
    off = op2(t, SpvOpIAdd, U, off, cu(t, (uint32_t)in->offset));
    bool load = !strncmp(in->name, "buffer_load_", 12);
    scratch_rw(t, in->name, load, off, in->vdata, in->vdata);
}

static void scratch_rw(tr *t, const char *name, bool load, uint32_t off, uint32_t d, uint32_t vdata) {
    uint32_t U = t->t_u32;
    if (!t->scratch) {
        fail(t, "scratch memory not declared");
        return;
    }
    uint32_t dw = op2(t, SpvOpShiftRightLogical, U, off, cu(t, 2));
    uint32_t n = strstr(name, "dwordx4") ? 4 : strstr(name, "dwordx3") ? 3 : strstr(name, "dwordx2") ? 2 : 1;
    bool dword = strstr(name, "dword") != NULL, shrt = strstr(name, "short") != NULL;
#define SCRATCH_PTR(i) op2(t, SpvOpAccessChain, t->p_pr_u32, t->scratch, ext2(t, U, GLSLstd450UMin, (i), cu(t, SCRATCH_DWORDS - 1)))
    uint32_t sh8 = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, off, cu(t, 3)), cu(t, 3));
    if (load) {
        if (dword) {
            uint32_t c[4];
            for (uint32_t k = 0; k < n; k++) c[k] = ld(t, U, SCRATCH_PTR(op2(t, SpvOpIAdd, U, dw, cu(t, k))));
            for (uint32_t k = 0; k < n; k++) set_v(t, d + k, c[k]);
            return;
        }
        bool sgn = strstr(name, "_sbyte") || strstr(name, "_sshort");
        uint32_t w = op2(t, SpvOpShiftRightLogical, U, ld(t, U, SCRATCH_PTR(dw)), sh8);
        uint32_t v = sgn ? bfe_i(t, w, cu(t, 0), cu(t, shrt ? 16 : 8)) : bfe_u(t, w, cu(t, 0), cu(t, shrt ? 16 : 8));
        if (strstr(name, "d16")) {
            uint32_t old = get_v(t, d);
            v = op2(t, SpvOpBitwiseAnd, U, v, cu(t, 0xFFFF));
            v = strstr(name, "d16_hi") ? op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, old, cu(t, 0xFFFF)), op2(t, SpvOpShiftLeftLogical, U, v, cu(t, 16)))
                                       : op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, old, cu(t, 0xFFFF0000u)), v);
        }
        set_v(t, d, v);
        return;
    }
    uint32_t merge = begin_if(t, active(t));
    if (dword) {
        for (uint32_t k = 0; k < n; k++) st(t, SCRATCH_PTR(op2(t, SpvOpIAdd, U, dw, cu(t, k))), get_v(t, vdata + k));
    } else {
        uint32_t ptr = SCRATCH_PTR(dw), fm = shrt ? 0xFFFF : 0xFF, v = get_v(t, vdata);
        if (strstr(name, "d16_hi")) v = op2(t, SpvOpShiftRightLogical, U, v, cu(t, 16));
        uint32_t m = op2(t, SpvOpShiftLeftLogical, U, cu(t, fm), sh8);
        v = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, v, cu(t, fm)), sh8);
        st(t, ptr, op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, ld(t, U, ptr), op1(t, SpvOpNot, U, m)), v));
    }
#undef SCRATCH_PTR
    end_if(t, merge);
}

static void global_op(tr *t, const rdna2_inst *in, uint32_t op, int site) {
    uint32_t U = t->t_u32;
    uint32_t var = t->site_var[site];
    uint32_t off = get_v(t, in->vaddr);
    if (in->sbase == RDNA2_OP_NULL) off = op2(t, SpvOpISub, U, off, sb_load(t, t->u_var, cu(t, SH_U_SITE + (uint32_t)site)));
    off = op2(t, SpvOpIAdd, U, off, cu(t, (uint32_t)in->offset));
    uint32_t dw = op2(t, SpvOpShiftRightLogical, U, off, cu(t, 2));
    const char *name = in->name;
    uint32_t n = strstr(name, "dwordx4") ? 4 : strstr(name, "dwordx3") ? 3 : strstr(name, "dwordx2") ? 2 : 1;
    bool dword = strstr(name, "dword") != NULL;
    if (op == SH_OP_GLOBAL_ATOMIC) {
        uint32_t len = buffer_dwords(t,var);
        buffer_atomic(t, in, var, dw, op2(t, SpvOpULessThan, t->t_bool, dw, len), in->dst - RDNA2_OP_VGPR);
        return;
    }
    if (strstr(name, "addtid")) {
        fail(t, "global access form not supported");
        return;
    }
    if (op == SH_OP_GLOBAL_LOAD) {
        if (dword) {
            uint32_t c[4];
            for (uint32_t k = 0; k < n; k++) c[k] = buf_word(t, var, op2(t, SpvOpIAdd, U, dw, cu(t, k)));
            for (uint32_t k = 0; k < n; k++) set_v(t, (in->dst - RDNA2_OP_VGPR) + k, c[k]);
            return;
        }
        bool shrt = strstr(name, "short") != NULL, sgn = strstr(name, "_sbyte") || strstr(name, "_sshort");
        uint32_t sh8 = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, off, cu(t, 3)), cu(t, 3));
        uint32_t w = op2(t, SpvOpShiftRightLogical, U, buf_word(t, var, dw), sh8), d = in->dst - RDNA2_OP_VGPR;
        uint32_t v = sgn ? bfe_i(t, w, cu(t, 0), cu(t, shrt ? 16 : 8)) : bfe_u(t, w, cu(t, 0), cu(t, shrt ? 16 : 8));
        if (strstr(name, "d16")) { /* into one half, the other kept */
            uint32_t old = get_v(t, d);
            v = op2(t, SpvOpBitwiseAnd, U, v, cu(t, 0xFFFF));
            v = strstr(name, "d16_hi") ? op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, old, cu(t, 0xFFFF)), op2(t, SpvOpShiftLeftLogical, U, v, cu(t, 16)))
                                       : op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, old, cu(t, 0xFFFF0000u)), v);
        }
        set_v(t, d, v);
        return;
    }
    uint32_t merge = begin_if(t, active(t));
    uint32_t len = buffer_dwords(t,var);
    if (dword) {
        for (uint32_t k = 0; k < n; k++) {
            uint32_t idx = op2(t, SpvOpIAdd, U, dw, cu(t, k));
            uint32_t m2 = begin_if(t, op2(t, SpvOpULessThan, t->t_bool, idx, len));
            st(t, sb_ptr(t, var, idx), get_v(t, in->vdata + k));
            end_if(t, m2);
        }
    } else { /* byte / short: merge into the dword */
        bool shrt = strstr(name, "short") != NULL, hi = strstr(name, "d16_hi") != NULL;
        uint32_t m2 = begin_if(t, op2(t, SpvOpULessThan, t->t_bool, dw, len));
        uint32_t ptr = sb_ptr(t, var, dw);
        uint32_t sh8 = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, off, cu(t, 3)), cu(t, 3));
        uint32_t fm = shrt ? 0xFFFF : 0xFF, v = get_v(t, in->vdata);
        if (hi) v = op2(t, SpvOpShiftRightLogical, U, v, cu(t, 16));
        uint32_t m = op2(t, SpvOpShiftLeftLogical, U, cu(t, fm), sh8);
        v = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, v, cu(t, fm)), sh8);
        st(t, ptr, op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, ld(t, U, ptr), op1(t, SpvOpNot, U, m)), v));
        end_if(t, m2);
    }
    end_if(t, merge);
}

static bool coherent_buffer_pair(const sh_program *p, uint32_t site) {
    const rdna2_inst *in = &p->insts[p->sites[site].inst];
    uint32_t op = p->ops[p->sites[site].inst];
    return g_buffer_atomic64 && in->enc == RDNA2_ENC_MUBUF && !in->tfe && (in->glc || in->dlc) &&
           (op == SH_OP_BUFFER_LOAD_DWORD || op == SH_OP_BUFFER_STORE_DWORD) && op_components(in->name) == 2;
}

/* Coherent publication of {flag,value} must not expose half a pair. An alias
 * over the same binding avoids changing the host's descriptor layout. Only
 * aligned, adjacent, fully in-range pairs may use the u64 access; stride-4
 * records and swizzled layouts keep per-component accesses. */
static void buffer_pair(tr *t, const rdna2_inst *in, int site, uint32_t off, uint32_t in_range, bool store) {
    uint32_t U = t->t_u32, B = t->t_bool, L = t->t_u64, var = t->site_var[site];
    uint32_t dw = op2(t, SpvOpShiftRightLogical, U, off, cu(t, 2));
    uint32_t len = buffer_dwords(t,var);
    uint32_t valid[2];
    for (uint32_t k = 0; k < 2; k++) {
        uint32_t idx = op2(t, SpvOpIAdd, U, dw, cu(t, k));
        valid[k] = op2(t, SpvOpLogicalAnd, B, in_range, op2(t, SpvOpULessThan, B, idx, len));
        if (k && (!in->idxen || !t->shapes[site].buf_stride)) {
            uint32_t oob = bfe_u(t, get_s(t, in->srsrc + 3), cu(t, 28), cu(t, 2));
            uint32_t second = op2(t, SpvOpIAdd, U, off, cu(t, 4));
            uint32_t fits = op2(t, SpvOpLogicalOr, B, op2(t, SpvOpIEqual, B, oob, cu(t, 2)),
                op2(t, SpvOpULessThan, B, second, get_s(t, in->srsrc + 2)));
            if (!t->shapes[site].address_map && !in->idxen) /* structured record 0: in_range checked the whole access */
                fits = op2(t, SpvOpLogicalOr, B, fits, op2(t, SpvOpLogicalAnd, B,
                    op2(t, SpvOpINotEqual, B, bfe_u(t, get_s(t, in->srsrc + 1), cu(t, 16), cu(t, 14)), cu(t, 0)),
                    op2(t, SpvOpINotEqual, B, oob, cu(t, 3))));
            valid[k] = op2(t, SpvOpLogicalAnd, B, valid[k], fits);
        }
        if (in->idxen && t->shapes[site].buf_stride) {
            uint32_t within = in->offen ? get_v(t, in->vaddr + 1) : cu(t, 0);
            within = op2(t, SpvOpIAdd, U, within, src_u(t, in, in->soffset));
            within = op2(t, SpvOpIAdd, U, within, cu(t, (uint32_t)in->offset));
            uint32_t stride = t->shapes[site].buf_stride;
            uint32_t fits = stride >= 4 * (k + 1)
                ? op2(t, SpvOpULessThanEqual, B, within, cu(t, stride - 4 * (k + 1)))
                : spv_const_bool(&t->b, false);
            valid[k] = op2(t, SpvOpLogicalAnd, B, valid[k], fits);
        }
    }
    uint32_t aligned = op2(t, SpvOpIEqual, B,
        op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpBitwiseOr, U, off, get_s(t, in->srsrc)), cu(t, 7)), cu(t, 0));
    /* SWIZZLE_ENABLE in V#.word1: no claim that two guest dwords are adjacent. */
    uint32_t linear = op2(t, SpvOpIEqual, B,
        op2(t, SpvOpBitwiseAnd, U, get_s(t, in->srsrc + 1), cu(t, 0x80000000u)), cu(t, 0));
    uint32_t pair = op2(t, SpvOpLogicalAnd, B, op2(t, SpvOpLogicalAnd, B, valid[0], valid[1]),
                       op2(t, SpvOpLogicalAnd, B, aligned, linear));
    uint32_t values[2], result[2] = {0};
    for (unsigned k = 0; k < 2; k++) {
        values[k] = store ? get_v(t, in->vdata + k) : cu(t, 0);
        if (!store) { result[k] = spv_local_var(&t->b, t->p_fn_u32); st(t, result[k], cu(t, 0)); }
    }
    uint32_t enabled = store ? active(t) : spv_const_bool(&t->b, true);
    uint32_t merge = begin_if(t, op2(t, SpvOpLogicalAnd, B, pair, enabled));
    uint32_t ptr = spv_op(&t->b, SpvOpAccessChain, t->p_sb_u64,
        (uint32_t[]){t->site_u64[site], cu(t, 0), op2(t, SpvOpShiftRightLogical, U, off, cu(t, 3))}, 3);
    if (store) {
        uint32_t value = op2(t, SpvOpBitwiseOr, L, op1(t, SpvOpUConvert, L, values[0]),
            op2(t, SpvOpShiftLeftLogical, L, op1(t, SpvOpUConvert, L, values[1]), cu(t, 32)));
        spv_op_void(&t->b, SpvOpAtomicStore, (uint32_t[]){ptr, cu(t, 1), cu(t, 0), value}, 4);
    } else {
        uint32_t value = spv_op(&t->b, SpvOpAtomicLoad, L, (uint32_t[]){ptr, cu(t, 1), cu(t, 0)}, 3);
        st(t, result[0], op1(t, SpvOpUConvert, U, value));
        st(t, result[1], op1(t, SpvOpUConvert, U, op2(t, SpvOpShiftRightLogical, L, value, cu(t, 32))));
    }
    end_if(t, merge);
    uint32_t fallback = op2(t, SpvOpLogicalAnd, B, op1(t, SpvOpLogicalNot, B, pair), enabled);
    for (uint32_t k = 0; k < 2; k++) {
        merge = begin_if(t, op2(t, SpvOpLogicalAnd, B, fallback, valid[k]));
        uint32_t p = sb_ptr(t, var, op2(t, SpvOpIAdd, U, dw, cu(t, k)));
        if (store) st(t, p, values[k]); else st(t, result[k], ld(t, U, p));
        end_if(t, merge);
    }
    if (!store) for (unsigned k = 0; k < 2; k++) set_v(t, in->vdata + k, ld(t, U, result[k]));
}

static void buffer_op(tr *t, const rdna2_inst *in, uint32_t op, int site) {
    uint32_t U = t->t_u32;
    const sh_site_shape *sh = &t->shapes[site];
    uint32_t var = t->site_var[site];
    uint32_t n = op_components(in->name);
    bool format = op == SH_OP_BUFFER_LOAD_FORMAT || op == SH_OP_TBUFFER_LOAD_FORMAT ||
                  op == SH_OP_BUFFER_STORE_FORMAT || op == SH_OP_TBUFFER_STORE_FORMAT;
    bool packed16 = format && strstr(in->name, "d16") != NULL;
    bool high16 = packed16 && strstr(in->name, "d16_hi") != NULL;
    bool store = t->p->sites[site].store;
    /* the bytes the access covers: a format access reads one element of its format (KAGE's
     * tile color is an RGBA8 element read as xyzw from a 4-byte buffer: counted as 16 bytes, the
     * whole-access range check returned 0 and no tile showed) */
    uint32_t bytes = n * 4;
    if (in->name && strstr(in->name, "atomic_") && strstr(in->name, "_x2")) bytes = 8;
    if (op == SH_OP_BUFFER_LOAD_UBYTE || op == SH_OP_BUFFER_LOAD_SBYTE || op == SH_OP_BUFFER_STORE_BYTE) bytes = 1;
    if (op == SH_OP_BUFFER_LOAD_USHORT || op == SH_OP_BUFFER_LOAD_SSHORT || op == SH_OP_BUFFER_STORE_SHORT) bytes = 2;
    sh_buf_format access_format;
    if (format && sh_buffer_format(sh->buf_format, &access_format)) {
        uint32_t bits = 0;
        for (uint32_t k = 0; k < access_format.comps && (!store || k < n); k++) bits += access_format.bits[k];
        bytes = (bits + 7) / 8;
    }
    uint32_t in_range;
    uint32_t off = buffer_offset(t, in, site, sh, bytes, &in_range);
    uint32_t dw = op2(t, SpvOpShiftRightLogical, U, off, cu(t, 2));
    if (sh->address_map) {
        uint32_t valid;
        uint32_t address = op2(t, SpvOpIAdd, t->t_u64, guest_pointer(t, in->srsrc), op1(t, SpvOpUConvert, t->t_u64, off));
        off = mapped_offset(t, site, address, bytes, &valid);
        in_range = op2(t, SpvOpLogicalAnd, t->t_bool, in_range, valid);
        if (format) {
            uint32_t expected = 0;
            for (uint32_t k = 0; k < 4; k++) expected |= (uint32_t)sh->dst_sel[k] << (3 * k);
            uint32_t swizzle = op2(t, SpvOpBitwiseAnd, U, get_s(t, in->srsrc + 3), cu(t, 0xfff));
            valid = op2(t, SpvOpIEqual, t->t_bool, swizzle, cu(t, expected));
            if (in->enc != RDNA2_ENC_MTBUF)
                valid = op2(t, SpvOpLogicalAnd, t->t_bool, valid, op2(t, SpvOpIEqual, t->t_bool,
                    op3(t, SpvOpBitFieldUExtract, U, get_s(t, in->srsrc + 3), cu(t, 12), cu(t, 7)), cu(t, sh->buf_format)));
            in_range = op2(t, SpvOpLogicalAnd, t->t_bool, in_range, valid);
        }
        dw = op2(t, SpvOpShiftRightLogical, U, off, cu(t, 2));
    }
    if (t->site_u64[site]) {
        buffer_pair(t, in, site, off, in_range, store);
        return;
    }
    sh_buf_format f = {1, {32, 0, 0, 0}, 4};
    if (format && (sh->buf_format == 0 || !sh_buffer_format(sh->buf_format, &f))) {
        /* BUF_FMT_INVALID or a value past the table (a disabled vertex stream,
         * layout words all ones): loads read zeros, stores drop */
        if (!store) {
            for (uint32_t k = 0; k < (packed16 ? (n + 1) / 2 : n); k++) set_v(t, in->vdata + k, cu(t, 0));
        }
        return;
    }
    if (op == SH_OP_BUFFER_LOAD_UBYTE || op == SH_OP_BUFFER_LOAD_USHORT) {
        uint32_t bits = op == SH_OP_BUFFER_LOAD_UBYTE ? 8 : 16;
        uint32_t word = buf_word(t, var, dw);
        uint32_t sh8 = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, off, cu(t, 3)), cu(t, 3));
        uint32_t v = op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpShiftRightLogical, U, word, sh8), cu(t, (1u << bits) - 1));
        set_v(t, in->vdata, sel(t, U, in_range, v, cu(t, 0)));
        return;
    }
    bool sub_load = op == SH_OP_BUFFER_LOAD_SBYTE || op == SH_OP_BUFFER_LOAD_SSHORT || op == SH_OP_BUFFER_LOAD_UBYTE_D16 ||
                    op == SH_OP_BUFFER_LOAD_UBYTE_D16_HI || op == SH_OP_BUFFER_LOAD_SBYTE_D16 ||
                    op == SH_OP_BUFFER_LOAD_SBYTE_D16_HI || op == SH_OP_BUFFER_LOAD_SHORT_D16 || op == SH_OP_BUFFER_LOAD_SHORT_D16_HI;
    if (sub_load) { /* signed loads extend to 32 bits; d16 ones fill one half and keep the other */
        bool shrt = op == SH_OP_BUFFER_LOAD_SSHORT || op == SH_OP_BUFFER_LOAD_SHORT_D16 || op == SH_OP_BUFFER_LOAD_SHORT_D16_HI;
        bool sgn = op == SH_OP_BUFFER_LOAD_SBYTE || op == SH_OP_BUFFER_LOAD_SSHORT || op == SH_OP_BUFFER_LOAD_SBYTE_D16 ||
                   op == SH_OP_BUFFER_LOAD_SBYTE_D16_HI;
        bool d16 = op != SH_OP_BUFFER_LOAD_SBYTE && op != SH_OP_BUFFER_LOAD_SSHORT;
        bool hi = op == SH_OP_BUFFER_LOAD_UBYTE_D16_HI || op == SH_OP_BUFFER_LOAD_SBYTE_D16_HI || op == SH_OP_BUFFER_LOAD_SHORT_D16_HI;
        uint32_t sh8 = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, off, cu(t, 3)), cu(t, 3));
        uint32_t w = op2(t, SpvOpShiftRightLogical, U, buf_word(t, var, dw), sh8);
        uint32_t v = sgn ? bfe_i(t, w, cu(t, 0), cu(t, shrt ? 16 : 8)) : bfe_u(t, w, cu(t, 0), cu(t, shrt ? 16 : 8));
        v = sel(t, U, in_range, v, cu(t, 0));
        if (d16) {
            uint32_t old = get_v(t, in->vdata);
            v = op2(t, SpvOpBitwiseAnd, U, v, cu(t, 0xFFFF));
            v = hi ? op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, old, cu(t, 0xFFFF)), op2(t, SpvOpShiftLeftLogical, U, v, cu(t, 16)))
                   : op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, old, cu(t, 0xFFFF0000u)), v);
        }
        set_v(t, in->vdata, v);
        return;
    }
    if (op == SH_OP_BUFFER_STORE_BYTE || op == SH_OP_BUFFER_STORE_BYTE_D16_HI || op == SH_OP_BUFFER_STORE_SHORT ||
        op == SH_OP_BUFFER_STORE_SHORT_D16_HI) {
        /* into its dword with two atomics (clear, then set its bits): lanes writing the
         * other bytes of the same dword do not lose each other's writes */
        bool shrt = op == SH_OP_BUFFER_STORE_SHORT || op == SH_OP_BUFFER_STORE_SHORT_D16_HI;
        bool hi = op == SH_OP_BUFFER_STORE_BYTE_D16_HI || op == SH_OP_BUFFER_STORE_SHORT_D16_HI;
        uint32_t fm = shrt ? 0xFFFF : 0xFF, v = get_v(t, in->vdata);
        if (hi) v = op2(t, SpvOpShiftRightLogical, U, v, cu(t, 16));
        uint32_t sh8 = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, off, cu(t, 3)), cu(t, 3));
        uint32_t m = op2(t, SpvOpShiftLeftLogical, U, cu(t, fm), sh8);
        v = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, v, cu(t, fm)), sh8);
        uint32_t merge = begin_if(t, op2(t, SpvOpLogicalAnd, t->t_bool, in_range, active(t)));
        uint32_t len = buffer_dwords(t,var);
        uint32_t ptr = sb_ptr(t, var, ext2(t, U, GLSLstd450UMin, dw, op2(t, SpvOpISub, U, len, cu(t, 1))));
        spv_op(&t->b, SpvOpAtomicAnd, U, (uint32_t[]){ptr, cu(t, 1), cu(t, 0), op1(t, SpvOpNot, U, m)}, 4);
        spv_op(&t->b, SpvOpAtomicOr, U, (uint32_t[]){ptr, cu(t, 1), cu(t, 0), v}, 4);
        end_if(t, merge);
        return;
    }
    if (!store) {
        uint32_t c[4];
        if (!format) {
            for (uint32_t k = 0; k < n; k++) c[k] = buf_word(t, var, op2(t, SpvOpIAdd, U, dw, cu(t, k)));
        } else {
            /* A typed access can start inside a dword (e.g. the second
             * R16_FLOAT of an interleaved vertex, at byte offset 2). */
            uint32_t scalar_offset;
            bool fixed_alignment = !sh->address_map && !in->offen &&
                (!in->idxen || !(sh->buf_stride & 3u)) && sh->buffer_length != SH_BUFFER_SHIFTED &&
                sh_inline_const(in->soffset, in->literal, &scalar_offset);
            uint32_t pos = fixed_alignment ? ((scalar_offset + in->offset) & 3u) * 8u : 0;
            uint32_t low_bits = fixed_alignment ? 0 :
                op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, off, cu(t, 3)), cu(t, 3));
            for (uint32_t k = 0; k < 4; k++) {
                if (k >= f.comps) {
                    c[k] = cu(t, k == 3 ? format_one(f.kind) : 0);
                    continue;
                }
                uint32_t bits = f.bits[k];
                uint32_t bit = fixed_alignment ? cu(t, pos) : op2(t, SpvOpIAdd, U, low_bits, cu(t, pos));
                uint32_t word_delta = fixed_alignment ? cu(t, pos / 32) :
                    op2(t, SpvOpShiftRightLogical, U, bit, cu(t, 5));
                uint32_t word_index = op2(t, SpvOpIAdd, U, dw, word_delta);
                uint32_t word = buf_word(t, var, word_index), raw;
                if (fixed_alignment && pos % 32 + bits <= 32) {
                    raw = bits == 32 ? word : bfe_u(t, word, cu(t, pos % 32), cu(t, bits));
                } else {
                    uint32_t shift = op2(t, SpvOpBitwiseAnd, U, bit, cu(t, 31));
                    uint32_t next = buf_word(t, var, op2(t, SpvOpIAdd, U, word_index, cu(t, 1)));
                    uint32_t left = op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpISub, U, cu(t, 32), shift), cu(t, 31));
                    uint32_t joined = op2(t, SpvOpBitwiseOr, U,
                        op2(t, SpvOpShiftRightLogical, U, word, shift), op2(t, SpvOpShiftLeftLogical, U, next, left));
                    raw = sel(t, U, op2(t, SpvOpIEqual, t->t_bool, shift, cu(t, 0)), word, joined);
                    if (bits < 32) raw = bfe_u(t, raw, cu(t, 0), cu(t, bits));
                }
                c[k] = convert_comp(t, raw, bits, f.kind);
                pos += bits;
            }
            uint32_t sw[4];
            for (uint32_t k = 0; k < 4; k++) {
                uint32_t s = sh->dst_sel[k];
                sw[k] = s >= 4 ? c[s - 4] : s == 1 ? cu(t, format_one(f.kind)) : cu(t, 0);
            }
            memcpy(c, sw, sizeof(c));
        }
        if (packed16) {
            for (uint32_t k = 0; k < n; k += 2) {
                uint32_t half[2];
                for (uint32_t j = 0; j < 2; j++) {
                    uint32_t v = k + j < n ? sel(t, U, in_range, c[k + j], cu(t, 0)) : cu(t, 0);
                    half[j] = f.kind == 4 || f.kind == 5 ? op2(t, SpvOpBitwiseAnd, U, v, cu(t, 0xFFFF)) : f2h(t, fbits(t, v));
                }
                uint32_t reg = in->vdata + k / 2, old = get_v(t, reg), value;
                if (high16) value = op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, old, cu(t, 0xFFFF)),
                                       op2(t, SpvOpShiftLeftLogical, U, half[0], cu(t, 16)));
                else value = op2(t, SpvOpBitwiseOr, U, half[0], k + 1 < n ?
                    op2(t, SpvOpShiftLeftLogical, U, half[1], cu(t, 16)) : op2(t, SpvOpBitwiseAnd, U, old, cu(t, 0xFFFF0000u)));
                set_v(t, reg, value);
            }
        } else for (uint32_t k = 0; k < n; k++) set_v(t, in->vdata + k, sel(t, U, in_range, c[k], cu(t, 0)));
        return;
    }
    if (op == SH_OP_BUFFER_ATOMIC) {
        buffer_atomic(t, in, var, dw, in_range, in->vdata);
        return;
    }
    /* stores */
    uint32_t cond = op2(t, SpvOpLogicalAnd, t->t_bool, in_range, active(t));
    uint32_t vals[4];
    for (uint32_t k = 0; k < n; k++) {
        if (packed16) {
            uint32_t pair = get_v(t, in->vdata + k / 2), shift = high16 ? 16 : (k & 1u) * 16;
            uint32_t raw = op3(t, SpvOpBitFieldUExtract, U, pair, cu(t, shift), cu(t, 16));
            vals[k] = f.kind == 4 ? raw : f.kind == 5 ?
                ubits(t, op3(t, SpvOpBitFieldSExtract, t->t_i32, ibits(t, pair), cu(t, shift), cu(t, 16))) : ubits(t, h2f(t, raw));
        } else vals[k] = get_v(t, in->vdata + k);
    }
    if (format && f.bits[0] != 32) {
        /* pack the components into dwords; formats narrower than a dword merge with memory */
        uint32_t words[4] = {0, 0, 0, 0}, pos = 0;
        for (uint32_t k = 0; k < f.comps && k < n; k++) {
            uint32_t bits = f.bits[k], v = pack_comp(t, vals[k], bits, f.kind);
            uint32_t w = pos / 32;
            uint32_t placed = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, v, cu(t, bits == 32 ? ~0u : (1u << bits) - 1)), cu(t, pos % 32));
            words[w] = words[w] ? op2(t, SpvOpBitwiseOr, U, words[w], placed) : placed;
            pos += bits;
        }
        uint32_t total = pos; /* only components named by the store may replace memory */
        uint32_t merge2 = begin_if(t, cond);
        uint32_t len2 = buffer_dwords(t,var);
        for (uint32_t w = 0; w * 32 < total; w++) {
            uint32_t idx = ext2(t, U, GLSLstd450UMin, op2(t, SpvOpIAdd, U, dw, cu(t, w)), op2(t, SpvOpISub, U, len2, cu(t, 1)));
            uint32_t ptr = sb_ptr(t, var, idx), v = words[w] ? words[w] : cu(t, 0);
            uint32_t used = total - w * 32 >= 32 ? 32 : total - w * 32;
            if (used < 32) { /* keep the bits of the dword this texel does not cover */
                uint32_t bsh = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, off, cu(t, 3)), cu(t, 3));
                uint32_t m = op2(t, SpvOpShiftLeftLogical, U, cu(t, (1u << used) - 1), bsh);
                v = op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, ld(t, U, ptr), op1(t, SpvOpNot, U, m)),
                        op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpShiftLeftLogical, U, v, bsh), m));
            }
            st(t, ptr, v);
        }
        end_if(t, merge2);
        return;
    }
    uint32_t merge = begin_if(t, cond);
    uint32_t len = buffer_dwords(t,var);
    for (uint32_t k = 0; k < n && (!format || k < f.comps); k++) {
        uint32_t idx = op2(t, SpvOpIAdd, U, dw, cu(t, k));
        idx = ext2(t, U, GLSLstd450UMin, idx, op2(t, SpvOpISub, U, len, cu(t, 1)));
        st(t, sb_ptr(t, var, idx), vals[k]);
    }
    end_if(t, merge);
}

/* Descriptor types sampled as 2D arrays: cube (its six faces are layers),
 * 1D array, 2D array, 2D MSAA array; written ones per sh_site_arrayed. */
static bool site_arrayed(const tr *t, int site) {
    return sh_site_arrayed(t->p, (uint32_t)site, t->shapes[site].img_type);
}


/* 3D image descriptors (TYPE 10): sampled and stored with three coordinates */
static bool site_3d(const tr *t, int site) { return t->shapes[site].img_type == 10; }

/* Address registers of an instruction's dim: 3D, cube and 2D arrays add the
 * slice (1D arrays are read as 2D here) */
static uint32_t dim_coords(const rdna2_inst *in) {
    return in->dim == 2 || in->dim == 3 || in->dim == 5 || in->dim == 7 ? 3 : 2;
}

/* Integer texel coordinates of a storage access: (x, y), plus the slice for 3D and arrayed images */
/* PS5C_RT_SCALE: a 2D image site may be bound to a target drawn smaller; SH_U_SITE then holds
 * host / guest as a float (0: full size). The scale as a float (1.0 when full size), or 0 when
 * the site can't be scaled (scaling off, arrays and volumes, address-mapped sites). */
static uint32_t site_scale(tr *t, int site) {
    if (!frag_scale_on() || site_3d(t, site) || site_arrayed(t, site) || t->shapes[site].address_map) return 0;
    uint32_t meta = sb_load(t, t->u_var, cu(t, SH_U_SITE + (uint32_t)site));
    uint32_t f = op1(t, SpvOpBitcast, t->t_f32, meta);
    return op3(t, SpvOpSelect, t->t_f32, op2(t, SpvOpINotEqual, t->t_bool, meta, cu(t, 0)), f, cf(t, 1.0f));
}
/* guest texel coordinates at the site's scale (floor keeps -1, the skipped lanes' mark) */
static uint32_t scaled_coord(tr *t, uint32_t s, uint32_t c) {
    uint32_t f = op2(t, SpvOpFMul, t->t_f32, op1(t, SpvOpConvertSToF, t->t_f32, c), s);
    return op1(t, SpvOpConvertFToS, t->t_i32, ext1(t, t->t_f32, GLSLstd450Floor, f));
}
/* a size query of a scaled site: the guest's size */
static uint32_t unscaled_size(tr *t, uint32_t s, uint32_t n) {
    uint32_t f = op2(t, SpvOpFDiv, t->t_f32, op1(t, SpvOpConvertSToF, t->t_f32, n), s);
    return op1(t, SpvOpConvertFToS, t->t_i32, ext1(t, t->t_f32, GLSLstd450Round, f));
}

static uint32_t storage_coord(tr *t, int site, const rdna2_inst *in, const uint32_t *addr) {
    uint32_t x = ibits(t, get_v(t, addr[0])), y = ibits(t, get_v(t, addr[1]));
    uint32_t s = site_scale(t, site);
    if (s) x = scaled_coord(t, s, x), y = scaled_coord(t, s, y);
    if (!site_3d(t, site) && !site_arrayed(t, site)) return op2(t, SpvOpCompositeConstruct, t->t_v2i, x, y);
    uint32_t z = dim_coords(in) == 3 ? ibits(t, get_v(t, addr[2])) : spv_const_i32(&t->b, 0);
    return spv_op(&t->b, SpvOpCompositeConstruct, spv_type_vector(&t->b, t->t_i32, 3), (uint32_t[]){x, y, z}, 3);
}

/* Sample coordinates from address VGPRs (x, y[, z]) for the site's image:
 * (x, y) or, for arrays, (x, y, layer), layer 0 when the instruction's dim
 * has none. A cube address is what the v_cube* results give: face
 * coordinates in [1, 2] and the face id (cube arrays: slice * 8 + face). */
static uint32_t sample_coord(tr *t, int site, const rdna2_inst *in, uint32_t x, uint32_t y, uint32_t z) {
    x = fbits(t, x);
    y = fbits(t, y);
    if (site_3d(t, site))
        return spv_op(&t->b, SpvOpCompositeConstruct, spv_type_vector(&t->b, t->t_f32, 3),
                      (uint32_t[]){x, y, dim_coords(in) == 3 || (in->dim == 1 && z) ? fbits(t, z) : cf(t, 0)}, 3); /* z: GCN 2D form */
    if (!site_arrayed(t, site)) return op2(t, SpvOpCompositeConstruct, t->t_v2f, x, y);
    z = dim_coords(in) == 3 ? fbits(t, z) : cf(t, 0);
    if (in->dim == 3) {
        x = op2(t, SpvOpFSub, t->t_f32, x, cf(t, 1.0f));
        y = op2(t, SpvOpFSub, t->t_f32, y, cf(t, 1.0f));
        uint32_t f = ext1(t, t->t_f32, GLSLstd450Floor, z);
        uint32_t slice = ext1(t, t->t_f32, GLSLstd450Floor, op2(t, SpvOpFMul, t->t_f32, f, cf(t, 0.125f)));
        uint32_t face = op2(t, SpvOpFSub, t->t_f32, f, op2(t, SpvOpFMul, t->t_f32, slice, cf(t, 8.0f)));
        z = op2(t, SpvOpFAdd, t->t_f32, op2(t, SpvOpFMul, t->t_f32, slice, cf(t, 6.0f)), face);
    }
    return spv_op(&t->b, SpvOpCompositeConstruct, spv_type_vector(&t->b, t->t_f32, 3), (uint32_t[]){x, y, z}, 3);
}

/* An offset or derivative (x, y) added to a coordinate of the site's shape */
static uint32_t coord_xy(tr *t, int site, uint32_t v2) {
    if (!site_arrayed(t, site)) return v2;
    uint32_t v3 = spv_type_vector(&t->b, t->t_f32, 3);
    return spv_op(&t->b, SpvOpCompositeConstruct, v3,
                  (uint32_t[]){op2(t, SpvOpCompositeExtract, t->t_f32, v2, 0), op2(t, SpvOpCompositeExtract, t->t_f32, v2, 1), cf(t, 0)}, 3);
}

/* 6-bit texel offsets packed as x [5:0], y [13:8] -> normalized coordinates of level 0 */
static uint32_t texel_offset(tr *t, int site, uint32_t simg, uint32_t packed) {
    uint32_t img = op1(t, SpvOpImage, t->site_image_type[site], simg);
    uint32_t szi;
    if (site_arrayed(t, site)) {
        uint32_t s3 = op2(t, SpvOpImageQuerySizeLod, spv_type_vector(&t->b, t->t_i32, 3), img, spv_const_i32(&t->b, 0));
        szi = spv_op(&t->b, SpvOpVectorShuffle, t->t_v2i, (uint32_t[]){s3, s3, 0, 1}, 4);
    } else {
        szi = op2(t, SpvOpImageQuerySizeLod, t->t_v2i, img, spv_const_i32(&t->b, 0));
    }
    uint32_t sz = op1(t, SpvOpConvertSToF, t->t_v2f, szi);
    uint32_t ox = op1(t, SpvOpConvertSToF, t->t_f32, ibits(t, bfe_i(t, packed, cu(t, 0), cu(t, 6))));
    uint32_t oy = op1(t, SpvOpConvertSToF, t->t_f32, ibits(t, bfe_i(t, packed, cu(t, 8), cu(t, 6))));
    return op2(t, SpvOpFDiv, t->t_v2f, op2(t, SpvOpCompositeConstruct, t->t_v2f, ox, oy), sz);
}

/* A 16-bit float (low bits of h) as the bits of a 32-bit float */
static uint32_t half_to_bits(tr *t, uint32_t h) {
    uint32_t v2 = ext1(t, t->t_v2f, GLSLstd450UnpackHalf2x16, h);
    return ubits(t, op2(t, SpvOpCompositeExtract, t->t_f32, v2, 0));
}

/* Results of an image instruction (32-bit values, components in order) into vdata:
 * D16 packs two per register (halves for float images, low 16 bits for integer ones). */
static void write_texels(tr *t, const rdna2_inst *in, const uint32_t *vals, uint32_t n, uint32_t kind) {
    if (!in->d16) {
        for (uint32_t k = 0; k < n; k++) set_v(t, in->vdata + k, vals[k]);
        return;
    }
    for (uint32_t k = 0; k < n; k += 2) {
        uint32_t lo, hi;
        if (kind == 0) {
            uint32_t f0 = fbits(t, vals[k]), f1 = k + 1 < n ? fbits(t, vals[k + 1]) : cf(t, 0.0f);
            uint32_t p = ext1(t, t->t_u32, GLSLstd450PackHalf2x16, op2(t, SpvOpCompositeConstruct, t->t_v2f, f0, f1));
            set_v(t, in->vdata + k / 2, p);
            continue;
        }
        lo = op2(t, SpvOpBitwiseAnd, t->t_u32, vals[k], cu(t, 0xFFFF));
        hi = k + 1 < n ? op2(t, SpvOpShiftLeftLogical, t->t_u32, vals[k + 1], cu(t, 16)) : cu(t, 0);
        set_v(t, in->vdata + k / 2, op2(t, SpvOpBitwiseOr, t->t_u32, lo, hi));
    }
}

/* A16: the address components come as 16-bit pairs, low half first; the 32-bit
 * ones ahead of them (offset, bias, depth reference) keep a register each. They
 * are unpacked into temporaries the instruction then reads as plain registers:
 * float for sampling, integer for loads, stores and the rest. */
static void a16_unpack(tr *t, const rdna2_inst *in, uint32_t op, uint32_t *addr) {
    const char *nm = in->name;
    uint32_t extras = 0;
    if (op == SH_OP_IMAGE_SAMPLE || op == SH_OP_IMAGE_GATHER4) {
        const char *suffix = strchr(nm + 6, '_'); /* past "image_" */
        extras = (strstr(suffix ? suffix : "", "_o") != NULL) + (strstr(nm, "_b") != NULL) + (strstr(nm, "_c") != NULL);
    }
    bool flt = op == SH_OP_IMAGE_SAMPLE || op == SH_OP_IMAGE_GATHER4 || op == SH_OP_IMAGE_GET_LOD;
    uint32_t regs[4];
    for (uint32_t k = 0; k < 4; k++) regs[k] = addr[k];
    for (uint32_t k = 0; k < 4; k++) {
        uint32_t v;
        if (k < extras) {
            v = get_v(t, regs[k]);
        } else {
            uint32_t r = extras + (k - extras) / 2;
            uint32_t w = get_v(t, regs[r < 4 ? r : 3]);
            uint32_t h = ((k - extras) & 1) ? op2(t, SpvOpShiftRightLogical, t->t_u32, w, cu(t, 16))
                                            : op2(t, SpvOpBitwiseAnd, t->t_u32, w, cu(t, 0xFFFF));
            v = flt ? half_to_bits(t, h) : h;
        }
        st(t, vreg(t, VTMP + k), v);
        addr[k] = VTMP + k;
    }
}

/* image_atomic_<op> on an integer storage image (R32ui/R32i, or R64ui for
 * unsigned min/max pairs; see declare_resources). GLC returns the old bits. */
static void image_atomic(tr *t, const rdna2_inst *in, int site, const uint32_t *addr) {
    uint32_t U = t->t_u32, kind = t->shapes[site].img_kind;
    bool wide=t->shapes[site].buf_format==SH_IMG_ATOMIC_UINT64;
    const char *k = in->name + strlen("image_atomic_");
    int rmw = !strcmp(k, "inc") ? RMW_INC : !strcmp(k, "dec") ? RMW_DEC : -1; /* unsigned images */
    if(t->shapes[site].buf_format == SH_IMG_ATOMIC_FLOAT32) {
        kind=1;
        rmw=!strcmp(k,"fmin")?RMW_FMIN:RMW_FMAX;
    }
    uint32_t aop = rmw >= 0 && kind == 1 ? SpvOpAtomicIAdd : atomic_op_of(k);
    bool cmpswap = aop == SpvOpAtomicCompareExchange;
    if (!aop || !kind || in->dmask != (wide || cmpswap ? 3u : 1u) ||
        (wide && (kind!=1 || (strcmp(k,"umin") && strcmp(k,"umax"))))) {
        fail(t, "image atomic not supported");
        return;
    }
    uint32_t T = wide ? t->t_u64 : kind == 1 ? U : t->t_i32;
    uint32_t coord = storage_coord(t, site, in, addr); /* 3D (a froxel volume) and arrays: three */
    uint32_t data = get_v(t, in->vdata), cmp = cmpswap ? get_v(t, in->vdata + 1) : 0;
    if(wide)data=op2(t,SpvOpBitwiseOr,T,op1(t,SpvOpUConvert,T,data),
        op2(t,SpvOpShiftLeftLogical,T,op1(t,SpvOpUConvert,T,get_v(t,in->vdata+1)),cu(t,32)));
    uint32_t res = spv_local_var(&t->b, wide?spv_type_pointer(&t->b,SpvStorageClassFunction,T):t->p_fn_u32);
    st(t, res, data);
    uint32_t merge = begin_if(t, active(t));
    uint32_t ptr = spv_op(&t->b, SpvOpImageTexelPointer, spv_type_pointer(&t->b, SpvStorageClassImage, T),
                          (uint32_t[]){t->site_var[site], coord, cu(t, 0)}, 3);
    if (kind == 2) {
        data = ibits(t, data);
        cmp = cmpswap ? ibits(t, cmp) : 0;
    }
    uint32_t old = rmw >= 0 ? atomic_loop(t, ptr, 1, rmw, data, 0)
                 : cmpswap ? spv_op(&t->b, aop, T, (uint32_t[]){ptr, cu(t, 1), cu(t, 0), cu(t, 0), data, cmp}, 6)
                           : spv_op(&t->b, aop, T, (uint32_t[]){ptr, cu(t, 1), cu(t, 0), data}, 4);
    st(t, res, kind == 1 ? old : ubits(t, old));
    end_if(t, merge);
    if (in->glc) {
        uint32_t value=ld(t,wide?T:U,res);
        set_v(t,in->vdata,wide?op1(t,SpvOpUConvert,U,value):value);
        if(wide)set_v(t,in->vdata+1,op1(t,SpvOpUConvert,U,op2(t,SpvOpShiftRightLogical,T,value,cu(t,32))));
    }
}

/* SH_IMG_PACKED_R11G11B10 sites: an R32_UINT view of a 10_11_11_FLOAT image. The 11- and 10-bit
 * floats share the half float's exponent, so a half's top bits are one (mantissa truncated;
 * negative values clamp to 0 as the unsigned format cannot hold them). */
static bool packed_site(tr *t, int site) {
    return sh_site_storage(&t->p->sites[site],&t->shapes[site]) && t->shapes[site].buf_format == SH_IMG_PACKED_R11G11B10;
}
static uint32_t pack_r11g11b10(tr *t, const uint32_t comps[4]) {
    uint32_t F = t->t_f32, U = t->t_u32, c[3];
    for (int k = 0; k < 3; k++) c[k] = ext2(t, F, GLSLstd450FMax, comps[k], cf(t, 0.0f));
    uint32_t rg = ext1(t, U, GLSLstd450PackHalf2x16, op2(t, SpvOpCompositeConstruct, t->t_v2f, c[0], c[1]));
    uint32_t bh = ext1(t, U, GLSLstd450PackHalf2x16, op2(t, SpvOpCompositeConstruct, t->t_v2f, c[2], cf(t, 0.0f)));
    uint32_t r = op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpShiftRightLogical, U, rg, cu(t, 4)), cu(t, 0x7FF));
    uint32_t g = op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpShiftRightLogical, U, rg, cu(t, 20)), cu(t, 0x7FF));
    uint32_t b = op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpShiftRightLogical, U, bh, cu(t, 5)), cu(t, 0x3FF));
    uint32_t v = op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseOr, U, r, op2(t, SpvOpShiftLeftLogical, U, g, cu(t, 11))),
                     op2(t, SpvOpShiftLeftLogical, U, b, cu(t, 22)));
    return spv_op(&t->b, SpvOpCompositeConstruct, t->t_v4u, (uint32_t[]){v, cu(t, 0), cu(t, 0), cu(t, 0)}, 4);
}
static uint32_t unpack_r11g11b10(tr *t, uint32_t texel) {
    uint32_t U = t->t_u32, F = t->t_f32;
    uint32_t v = op2(t, SpvOpCompositeExtract, U, texel, 0);
    uint32_t r = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, v, cu(t, 0x7FF)), cu(t, 4));
    uint32_t g = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpShiftRightLogical, U, v, cu(t, 11)), cu(t, 0x7FF)), cu(t, 20));
    uint32_t b = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpShiftRightLogical, U, v, cu(t, 22)), cu(t, 5));
    uint32_t rg = ext1(t, t->t_v2f, GLSLstd450UnpackHalf2x16, op2(t, SpvOpBitwiseOr, U, r, g));
    uint32_t bb = ext1(t, t->t_v2f, GLSLstd450UnpackHalf2x16, b);
    return spv_op(&t->b, SpvOpCompositeConstruct, t->t_v4f,
                  (uint32_t[]){op2(t, SpvOpCompositeExtract, F, rg, 0), op2(t, SpvOpCompositeExtract, F, rg, 1),
                               op2(t, SpvOpCompositeExtract, F, bb, 0), cf(t, 1.0f)}, 4);
}

/* image_sample / image_gather4 variants by their suffix (_c compare, _d derivatives, _b bias,
 * _l lod, _lz lod 0, _cl lod clamp, _o offsets). Address registers come in the hardware's order:
 * offsets, bias, compare value, derivatives (x then y), coordinates, lod, clamp. The clamp is
 * not applied; gathers read the view's level (no lod or bias). Compares are done here on the
 * sampled texels, by the sampler's DEPTH_COMPARE_FUNC. Gathers write their four texels (1);
 * samples return the texel in *out for image_op's component selection (2); 0 if unsupported. */
static int sample_variant(tr *t, const rdna2_inst *in, int site, uint32_t simg, const char *v, bool gather, uint32_t *out) {
    const sh_site_shape *sh = &t->shapes[site];
    uint32_t F = t->t_f32, B = t->t_bool, kind = sh->img_kind;
    uint32_t rtype = kind == 1 ? t->t_v4u : kind == 2 ? t->t_v4i : t->t_v4f;
    bool f_o = false, f_b = false, f_c = false, f_d = false, f_l = false, f_lz = false;
    for (const char *p0 = v; *p0;) { /* "_tok_tok...": cl (the clamp) is accepted and not applied */
        if (*p0 == '_') p0++;
        size_t n = strcspn(p0, "_");
        if (n == 1 && *p0 == 'o') f_o = true;
        else if (n == 1 && *p0 == 'b') f_b = true;
        else if (n == 1 && *p0 == 'c') f_c = true;
        else if ((n == 1 && *p0 == 'd') || (n == 2 && !strncmp(p0, "cd", 2))) f_d = true; /* cd: coarse */
        else if (n == 1 && *p0 == 'l') f_l = true;
        else if (n == 2 && !strncmp(p0, "lz", 2)) f_lz = true;
        else if (!(n == 2 && !strncmp(p0, "cl", 2))) return 0;
        p0 += n;
    }
    bool three = site_3d(t, site);
    if (in->a16 || (gather && three) || (f_c && kind)) return 0;
    uint32_t nc = dim_coords(in), nsp = in->dim == 2 ? 3 : in->dim == 0 || in->dim == 4 ? 1 : 2, p = 0;
#define AREG(k) (in->nsa_count && (k) ? ((k) - 1 < 12 ? in->nsa_addr[(k) - 1] : 0) : in->vaddr + (k))
    uint32_t off = f_o ? get_v(t, AREG(p++)) : 0;
    uint32_t bias = f_b ? fbits(t, get_v(t, AREG(p++))) : 0;
    uint32_t ref = f_c ? fbits(t, get_v(t, AREG(p++))) : 0;
    uint32_t gx[3] = {0}, gy[3] = {0};
    if (f_d) {
        for (uint32_t k = 0; k < nsp; k++) gx[k] = fbits(t, get_v(t, AREG(p + k)));
        for (uint32_t k = 0; k < nsp; k++) gy[k] = fbits(t, get_v(t, AREG(p + nsp + k)));
        p += 2 * nsp;
    }
    uint32_t x = get_v(t, AREG(p)), y = nc > 1 ? get_v(t, AREG(p + 1)) : cu(t, 0), z = nc > 2 ? get_v(t, AREG(p + 2)) : 0;
    p += nc;
    uint32_t lod = f_l ? fbits(t, get_v(t, AREG(p++))) : cf(t, 0);
#undef AREG
    uint32_t coord = sample_coord(t, site, in, x, y, z);
    if (f_o) {
        uint32_t o2 = texel_offset(t, site, simg, off);
        uint32_t o = three ? spv_op(&t->b, SpvOpCompositeConstruct, spv_type_vector(&t->b, F, 3),
                                    (uint32_t[]){op2(t, SpvOpCompositeExtract, F, o2, 0), op2(t, SpvOpCompositeExtract, F, o2, 1), cf(t, 0)}, 3)
                           : coord_xy(t, site, o2);
        coord = op2(t, SpvOpFAdd, three || site_arrayed(t, site) ? spv_type_vector(&t->b, F, 3) : t->t_v2f, coord, o);
    }
    uint32_t stype = f_c ? t->t_v4f : rtype, res;
    if (gather) {
        uint32_t comp = 0;
        while (comp < 3 && !((in->dmask >> comp) & 1)) comp++;
        res = op3(t, SpvOpImageGather, stype, simg, coord, spv_const_i32(&t->b, (int32_t)comp));
    } else if (f_d) {
        uint32_t gv = three ? spv_type_vector(&t->b, F, 3) : t->t_v2f, n = three ? 3 : 2;
        for (uint32_t k = nsp; k < n; k++) gx[k] = gy[k] = cf(t, 0);
        uint32_t ddx = spv_op(&t->b, SpvOpCompositeConstruct, gv, gx, n), ddy = spv_op(&t->b, SpvOpCompositeConstruct, gv, gy, n);
        res = spv_op(&t->b, SpvOpImageSampleExplicitLod, stype, (uint32_t[]){simg, coord, 4 /* Grad */, ddx, ddy}, 5);
    } else if (f_l || f_lz || t->info->stage != SH_STAGE_PS) {
        res = spv_op(&t->b, SpvOpImageSampleExplicitLod, stype, (uint32_t[]){simg, coord, 2 /* Lod */, lod}, 4);
    } else if (f_b) {
        res = spv_op(&t->b, SpvOpImageSampleImplicitLod, stype, (uint32_t[]){simg, coord, 1 /* Bias */, bias}, 4);
    } else {
        res = op2(t, SpvOpImageSampleImplicitLod, stype, simg, coord);
    }
    uint32_t vals[4];
    if (!gather && !f_c) {
        *out = res;
        return 2;
    }
    if (f_c) {
        static const uint32_t k_cmp[8] = {0, SpvOpFOrdLessThan, SpvOpFOrdEqual, SpvOpFOrdLessThanEqual,
                                          SpvOpFOrdGreaterThan, SpvOpFOrdNotEqual, SpvOpFOrdGreaterThanEqual, 0};
        uint32_t fn = sh->cmp_func & 7;
        for (uint32_t k = 0; k < 4; k++) {
            uint32_t texel = op2(t, SpvOpCompositeExtract, F, res, gather ? k : 0);
            uint32_t pass = fn == 0 ? spv_const_bool(&t->b, false) : fn == 7 ? spv_const_bool(&t->b, true)
                                                                           : op2(t, k_cmp[fn], B, ref, texel);
            vals[k] = ubits(t, sel(t, F, pass, cf(t, 1.0f), cf(t, 0.0f)));
        }
        if (!gather) {
            uint32_t r = fbits(t, vals[0]);
            *out = spv_op(&t->b, SpvOpCompositeConstruct, t->t_v4f, (uint32_t[]){r, r, r, r}, 4);
            return 2;
        }
    } else {
        uint32_t elem = kind == 1 ? t->t_u32 : kind == 2 ? t->t_i32 : F;
        for (uint32_t k = 0; k < 4; k++) {
            uint32_t c = op2(t, SpvOpCompositeExtract, elem, res, k);
            vals[k] = kind == 1 ? c : ubits(t, c);
        }
    }
    write_texels(t, in, vals, 4, f_c ? 0 : kind);
    return 1;
}

/* PCF at the view's base level. Gather respects sampler addressing and keeps
 * array slices; compare the depths before bilinear interpolation. This also
 * works for guest shadows stored in color images, unlike native Dref sampling. */
static uint32_t compare_linear_base(tr *t, int site, uint32_t simg, uint32_t coord, uint32_t ref) {
    const sh_site_shape *sh=&t->shapes[site];
    uint32_t F=t->t_f32, img=op1(t,SpvOpImage,t->site_image_type[site],simg);
    uint32_t size_type=site_arrayed(t,site)?spv_type_vector(&t->b,t->t_i32,3):t->t_v2i;
    uint32_t size=op2(t,SpvOpImageQuerySizeLod,size_type,img,spv_const_i32(&t->b,0));
    uint32_t frac[2],pass[4];
    for(uint32_t k=0;k<2;k++) {
        uint32_t xy=op2(t,SpvOpCompositeExtract,F,coord,k);
        uint32_t dim=op1(t,SpvOpConvertSToF,F,op2(t,SpvOpCompositeExtract,t->t_i32,size,k));
        frac[k]=ext1(t,F,GLSLstd450Fract,op2(t,SpvOpFSub,F,op2(t,SpvOpFMul,F,xy,dim),cf(t,.5f)));
    }
    uint32_t depths=op3(t,SpvOpImageGather,t->t_v4f,simg,coord,spv_const_i32(&t->b,0));
    static const uint32_t cmp[8]={0,SpvOpFOrdLessThan,SpvOpFOrdEqual,SpvOpFOrdLessThanEqual,
        SpvOpFOrdGreaterThan,SpvOpFOrdNotEqual,SpvOpFOrdGreaterThanEqual,0};
    uint32_t fn=sh->cmp_func&7;
    for(uint32_t k=0;k<4;k++) {
        uint32_t depth=op2(t,SpvOpCompositeExtract,F,depths,k);
        uint32_t ok=fn==0?spv_const_bool(&t->b,false):fn==7?spv_const_bool(&t->b,true):op2(t,cmp[fn],t->t_bool,ref,depth);
        pass[k]=sel(t,F,ok,cf(t,1),cf(t,0));
    }
    /* Gather order: (i0,j1), (i1,j1), (i1,j0), (i0,j0). */
    uint32_t rows[2];
    for(uint32_t y=0;y<2;y++) {
        uint32_t left=pass[y?0:3],right=pass[y?1:2];
        rows[y]=op2(t,SpvOpFAdd,F,left,op2(t,SpvOpFMul,F,frac[0],op2(t,SpvOpFSub,F,right,left)));
    }
    return op2(t,SpvOpFAdd,F,rows[0],op2(t,SpvOpFMul,F,frac[1],op2(t,SpvOpFSub,F,rows[1],rows[0])));
}

static void image_op(tr *t, const rdna2_inst *in, uint32_t op, int site) {
    const sh_site_shape *sh = &t->shapes[site];
    uint32_t kind = sh->img_kind;
    uint32_t rtype = kind == 1 ? t->t_v4u : kind == 2 ? t->t_v4i : t->t_v4f;
    uint32_t addr[4];
    uint32_t mip_source, mip_literal;
    bool bound_mip = sh->img_type == 9 &&
        sh_image_mip_source(t->p, t->info, (uint32_t)site, &mip_source, &mip_literal);
    for (uint32_t k = 0; k < 4; k++) addr[k] = in->nsa_count && k ? in->nsa_addr[k - 1] : in->vaddr + k;
    if (in->a16) a16_unpack(t, in, op, addr);
    /* coordinate registers. A PS4 (GCN) image instruction has no dimension of its own, the resource
     * gives it (gcn.c emits 2D): on a 3D resource it addresses x, y, z (LEGO City Undercover's color
     * grading LUT read only its first slice, a yellow picture). */
    uint32_t nc = in->dim == 1 && site_3d(t, site) ? 3 : dim_coords(in);
    uint32_t ctype = site_arrayed(t, site) || site_3d(t, site) ? spv_type_vector(&t->b, t->t_f32, 3) : t->t_v2f;
    if (op == SH_OP_IMAGE_ATOMIC) {
        image_atomic(t, in, site, addr);
        return;
    }
    uint32_t simg = ld(t, t->site_sampled_type[site], t->site_var[site]);
    uint32_t res;
    if (op == SH_OP_IMAGE_GET_LOD) {
        /* RDNA2 returns clampLOD then rawLOD. SPIR-V uses the same order.
         * Query coordinates exclude an array layer (unlike sampling). Cubes
         * currently use face-array images here, so their query needs a separate
         * implementation. Do not silently apply 2D derivatives to other dims. */
        bool match = (in->dim == 1 && sh->img_type == 9) ||
                     (in->dim == 2 && sh->img_type == 10) ||
                     (in->dim == 5 && sh->img_type == 13);
        if (t->info->stage != SH_STAGE_PS || !match || in->unorm || in->d16 || in->tfe || in->lwe ||
            !in->dmask || (in->dmask & ~3u)) {
            fail(t, "image_get_lod form not supported");
            return;
        }
        uint32_t coord = op2(t, SpvOpCompositeConstruct, t->t_v2f,
                             fbits(t, get_v(t, addr[0])), fbits(t, get_v(t, addr[1])));
        if (site_3d(t, site))
            coord = spv_op(&t->b, SpvOpCompositeConstruct, spv_type_vector(&t->b, t->t_f32, 3),
                           (uint32_t[]){fbits(t, get_v(t, addr[0])), fbits(t, get_v(t, addr[1])),
                                        fbits(t, get_v(t, addr[2]))}, 3);
        uint32_t lod = op2(t, SpvOpImageQueryLod, t->t_v2f, simg, coord), out = 0;
        for (uint32_t k = 0; k < 2; k++)
            if ((in->dmask >> k) & 1)
                set_v(t, in->vdata + out++, ubits(t, op2(t, SpvOpCompositeExtract, t->t_f32, lod, k)));
        return;
    } else if (op == SH_OP_IMAGE_SAMPLE) {
        /* the PS5 "_a" forms (sampler dword 3 adjusted) sample like the plain ones */
        char vbuf[24];
        snprintf(vbuf, sizeof(vbuf), "%s", in->name + strlen("image_sample"));
        size_t vl = strlen(vbuf);
        if (vl >= 2 && !strcmp(vbuf + vl - 2, "_a")) vbuf[vl - 2] = 0;
        const char *v = vbuf;
        bool bias = !strcmp(v, "_b"); /* LOD bias first, then the coordinates */
        uint32_t c0 = bias ? 1 : 0;
        uint32_t zc = nc == 3 ? get_v(t, addr[c0 + 2]) : 0;
        uint32_t coord = sample_coord(t, site, in, get_v(t, addr[c0]), get_v(t, addr[c0 + 1]), zc);
        if (bias && t->info->stage == SH_STAGE_PS) {
            uint32_t a[4] = {simg, coord, 1 /* Bias */, fbits(t, get_v(t, addr[0]))};
            res = spv_op(&t->b, SpvOpImageSampleImplicitLod, rtype, a, 4);
        } else if (bias) {
            uint32_t a[4] = {simg, coord, 2 /* Lod */, cf(t, 0)};
            res = spv_op(&t->b, SpvOpImageSampleExplicitLod, rtype, a, 4);
        } else if (!strcmp(v, "") && t->info->stage == SH_STAGE_PS) {
            res = op2(t, SpvOpImageSampleImplicitLod, rtype, simg, coord);
        } else if (!strcmp(v, "") || !strcmp(v, "_lz")) {
            uint32_t a[4] = {simg, coord, 2 /* Lod */, cf(t, 0)};
            res = spv_op(&t->b, SpvOpImageSampleExplicitLod, rtype, a, 4);
        } else if (!strcmp(v, "_l")) {
            uint32_t a[4] = {simg, coord, 2, fbits(t, get_v(t, addr[nc]))};
            res = spv_op(&t->b, SpvOpImageSampleExplicitLod, rtype, a, 4);
        } else if (!strcmp(v, "_lz_o") || !strcmp(v, "_l_o") || !strcmp(v, "_o")) {
            /* texel offsets (6-bit x, y) first: added in texel units of level 0 */
            if (nc == 3 && !strcmp(v, "_l_o")) {
                fail(t, "image_sample_l_o on an array not supported");
                return;
            }
            uint32_t c = sample_coord(t, site, in, get_v(t, addr[1]), get_v(t, addr[2]), nc == 3 ? get_v(t, addr[3]) : 0);
            c = op2(t, SpvOpFAdd, ctype, c, coord_xy(t, site, texel_offset(t, site, simg, get_v(t, addr[0]))));
            uint32_t lod = !strcmp(v, "_l_o") ? fbits(t, get_v(t, addr[3])) : cf(t, 0);
            if (!strcmp(v, "_o") && t->info->stage == SH_STAGE_PS) {
                res = op2(t, SpvOpImageSampleImplicitLod, rtype, simg, c);
            } else {
                uint32_t a[4] = {simg, c, 2, lod};
                res = spv_op(&t->b, SpvOpImageSampleExplicitLod, rtype, a, 4);
            }
        } else if (!strcmp(v, "_c") || !strcmp(v, "_c_lz")) {
            /* Depth compare (ref, then coordinates). Preserve the existing
             * base-level behavior; linear min+mag needs comparison before
             * filtering. Mixed min/mag and other LOD variants remain separate. */
            uint32_t ref = fbits(t, get_v(t, addr[0]));
            uint32_t c = sample_coord(t, site, in, get_v(t, addr[1]), get_v(t, addr[2]),
                                      nc == 3 ? get_v(t, addr[3]) : 0);
            if(sh->cmp_filter==3 && !kind && !site_3d(t,site)) {
                uint32_t r=compare_linear_base(t,site,simg,c,ref);
                res=spv_op(&t->b,SpvOpCompositeConstruct,t->t_v4f,(uint32_t[]){r,r,r,r},4);
            } else {
            uint32_t a[4] = {simg, c, 2 /* Lod */, cf(t, 0)};
            uint32_t texel = op2(t, SpvOpCompositeExtract, t->t_f32, spv_op(&t->b, SpvOpImageSampleExplicitLod, t->t_v4f, a, 4), 0);
            static const uint32_t k_cmp[8] = {0, SpvOpFOrdLessThan, SpvOpFOrdEqual, SpvOpFOrdLessThanEqual,
                                              SpvOpFOrdGreaterThan, SpvOpFOrdNotEqual, SpvOpFOrdGreaterThanEqual, 0};
            uint32_t f = sh->cmp_func & 7, pass;
            if (f == 0) pass = spv_const_bool(&t->b, false);
            else if (f == 7) pass = spv_const_bool(&t->b, true);
            else pass = op2(t, k_cmp[f], t->t_bool, ref, texel);
            uint32_t r = sel(t, t->t_f32, pass, cf(t, 1.0f), cf(t, 0.0f));
            res = spv_op(&t->b, SpvOpCompositeConstruct, t->t_v4f, (uint32_t[]){r, r, r, r}, 4);
            if (kind) res = op1(t, SpvOpBitcast, rtype, res);
            }
        } else if (!strcmp(v, "_d")) {
            /* derivatives: dx/dh, dy/dh, dx/dv, dy/dv, then the coordinates */
            uint32_t V = t->t_v2f, r6[6];
            for (uint32_t k = 0; k < 6; k++) r6[k] = in->nsa_count && k ? in->nsa_addr[k - 1] : in->vaddr + k;
            uint32_t ddx = op2(t, SpvOpCompositeConstruct, V, fbits(t, get_v(t, r6[0])), fbits(t, get_v(t, r6[1])));
            uint32_t ddy0 = op2(t, SpvOpCompositeConstruct, V, fbits(t, get_v(t, r6[2])), fbits(t, get_v(t, r6[3])));
            uint32_t c = coord_xy(t, site, op2(t, SpvOpCompositeConstruct, V, fbits(t, get_v(t, r6[4])), fbits(t, get_v(t, r6[5]))));
            uint32_t a[5] = {simg, c, 4 /* Grad */, ddx, ddy0};
            res = spv_op(&t->b, SpvOpImageSampleExplicitLod, rtype, a, 5);
        } else if (sample_variant(t, in, site, simg, v, false, &res) != 2) {
            fail(t, "image sample variant not supported");
            return;
        }
    } else if (op == SH_OP_IMAGE_GATHER4 &&
               (!strcmp(in->name, "image_gather4_c") || !strcmp(in->name, "image_gather4_c_lz") ||
                !strcmp(in->name, "image_gather4_c_o") ||
                !strcmp(in->name, "image_gather4_c_lz_o"))) {
        if (kind || (sh->img_type != 9 && sh->img_type != 13) || (in->dim != 1 && in->dim != 5) || in->unorm || in->tfe || in->lwe ||
            !in->dmask || (in->dmask & (in->dmask - 1)) || (in->dim == 5) != site_arrayed(t, site)) {
            fail(t, "comparison gather form not supported");
            return;
        }
        /* AMD addresses are offset (if present), comparison ref, then XY.
         * Gather keeps the four texels separate; compare each before D16 packing.
         * Host views contain one mip, so both forms gather from that level. */
        bool offset = in->name[strlen(in->name) - 1] == 'o';
        uint32_t ref_index = offset ? 1 : 0, comp = 0;
        while (!((in->dmask >> comp) & 1))
            comp++;
        uint32_t ref = fbits(t, get_v(t, addr[ref_index]));
        /* 2D arrays (cascaded shadow maps): the slice follows XY */
        uint32_t coord = sample_coord(t, site, in, get_v(t, addr[ref_index + 1]),
                                      get_v(t, addr[ref_index + 2]),
                                      in->dim == 5 ? get_v(t, addr[ref_index + 3]) : 0);
        /* Normalize the guest's signed six-bit offsets instead of using the
         * narrower Vulkan gather-offset range exposed by some devices. */
        if (offset)
            coord = op2(t, SpvOpFAdd, site_arrayed(t, site) ? spv_type_vector(&t->b, t->t_f32, 3) : t->t_v2f, coord,
                        coord_xy(t, site, texel_offset(t, site, simg, get_v(t, addr[0]))));
        res = op3(t, SpvOpImageGather, t->t_v4f, simg, coord, spv_const_i32(&t->b, (int32_t)comp));
        static const uint32_t cmp[8] = {0,
                                        SpvOpFOrdLessThan,
                                        SpvOpFOrdEqual,
                                        SpvOpFOrdLessThanEqual,
                                        SpvOpFOrdGreaterThan,
                                        SpvOpFOrdNotEqual,
                                        SpvOpFOrdGreaterThanEqual,
                                        0};
        uint32_t func = sh->cmp_func & 7, vals[4];
        for (uint32_t k = 0; k < 4; k++) {
            uint32_t texel = op2(t, SpvOpCompositeExtract, t->t_f32, res, k);
            uint32_t pass = func == 0   ? spv_const_bool(&t->b, false)
                            : func == 7 ? spv_const_bool(&t->b, true)
                                        : op2(t, cmp[func], t->t_bool, ref, texel);
            vals[k] = ubits(t, sel(t, t->t_f32, pass, cf(t, 1.0f), cf(t, 0.0f)));
        }
        write_texels(t, in, vals, 4, 0);
        return;
    } else if (op == SH_OP_IMAGE_GATHER4 &&
               (!strcmp(in->name, "image_gather4_lz") || !strcmp(in->name, "image_gather4_lz_o") ||
                !strcmp(in->name, "image_gather4") || !strcmp(in->name, "image_gather4_o"))) {
        /* the dmask bit picks the component; the four texels come back in x, y, z, w */
        bool o = in->name[strlen(in->name) - 1] == 'o';
        uint32_t c0 = o ? 1 : 0, comp = 0;
        while (comp < 3 && !((in->dmask >> comp) & 1)) comp++;
        uint32_t coord = sample_coord(t, site, in, get_v(t, addr[c0]), get_v(t, addr[c0 + 1]), nc == 3 ? get_v(t, addr[c0 + 2]) : 0);
        if (o) {
            uint32_t w = get_v(t, addr[0]);
            uint32_t ox = bfe_i(t, w, cu(t, 0), cu(t, 6)), oy = bfe_i(t, w, cu(t, 8), cu(t, 6));
            uint32_t offs = op2(t, SpvOpCompositeConstruct, t->t_v2i, ibits(t, ox), ibits(t, oy));
            uint32_t a[5] = {simg, coord, spv_const_i32(&t->b, (int32_t)comp), 0x10 /* Offset */, offs};
            res = spv_op(&t->b, SpvOpImageGather, rtype, a, 5);
        } else {
            res = op3(t, SpvOpImageGather, rtype, simg, coord, spv_const_i32(&t->b, (int32_t)comp));
        }
        uint32_t elem = kind == 1 ? t->t_u32 : kind == 2 ? t->t_i32 : t->t_f32, vals[4];
        for (uint32_t k = 0; k < 4; k++) {
            uint32_t v = op2(t, SpvOpCompositeExtract, elem, res, k);
            vals[k] = kind == 1 ? v : ubits(t, v);
        }
        write_texels(t, in, vals, 4, kind);
        return;
    } else if (op == SH_OP_IMAGE_GATHER4 && strncmp(in->name, "image_gather4h", 14) != 0) {
        if (!sample_variant(t, in, site, simg, in->name + strlen("image_gather4"), true, &res))
            fail(t, "image gather variant not supported");
        return;
    } else if (op == SH_OP_IMAGE_GET_RESINFO) {
        if (sh->img_type == 9 && in->dim == 1) {
            /* Host textures currently contain one guest mip each. Querying
             * host LOD > 0 is undefined. RESINFO describes the guest view,
             * whose dimensions/levels are in the live T# SGPRs. */
            uint32_t d1 = get_s(t, in->srsrc + 1), d2 = get_s(t, in->srsrc + 2);
            uint32_t d3 = get_s(t, in->srsrc + 3);
            uint32_t base = bfe_u(t, d3, cu(t, 12), cu(t, 4));
            uint32_t last = bfe_u(t, d3, cu(t, 16), cu(t, 4));
            uint32_t lod = ext2(t, t->t_u32, GLSLstd450UMin, get_v(t, addr[0]), cu(t, 31));
            uint32_t level = ext2(t, t->t_u32, GLSLstd450UMin,
                                 op2(t, SpvOpIAdd, t->t_u32, base, lod), cu(t, 31));
            uint32_t w = op2(t, SpvOpBitwiseOr, t->t_u32,
                            bfe_u(t, d1, cu(t, 30), cu(t, 2)),
                            op2(t, SpvOpShiftLeftLogical, t->t_u32,
                                bfe_u(t, d2, cu(t, 0), cu(t, 14)), cu(t, 2)));
            uint32_t h = bfe_u(t, d2, cu(t, 14), cu(t, 16));
            w = op2(t, SpvOpShiftRightLogical, t->t_u32, op2(t, SpvOpIAdd, t->t_u32, w, cu(t, 1)), level);
            h = op2(t, SpvOpShiftRightLogical, t->t_u32, op2(t, SpvOpIAdd, t->t_u32, h, cu(t, 1)), level);
            uint32_t levels = op2(t, SpvOpIAdd, t->t_u32, op2(t, SpvOpISub, t->t_u32, last, base), cu(t, 1));
            uint32_t valid = op2(t, SpvOpINotEqual, t->t_bool, d1, cu(t, 0));
            uint32_t vals[4] = {ext2(t, t->t_u32, GLSLstd450UMax, w, cu(t, 1)),
                                ext2(t, t->t_u32, GLSLstd450UMax, h, cu(t, 1)), cu(t, 1), levels};
            uint32_t out = 0;
            for (uint32_t k = 0; k < 4; k++)
                if ((in->dmask >> k) & 1)
                    set_v(t, in->vdata + out++, op3(t, SpvOpSelect, t->t_u32, valid, vals[k], cu(t, 0)));
            return;
        }
        /* width, height, depth/layers, mip levels (as dmask selects) */
        uint32_t sz, levels;
        if (t->p->sites[site].store) {
            sz = op1(t, SpvOpImageQuerySize, t->t_v2i, simg);
            levels = cu(t, 1);
        } else {
            uint32_t img = op1(t, SpvOpImage, t->site_image_type[site], simg);
            sz = op2(t, SpvOpImageQuerySizeLod, site_arrayed(t, site) || site_3d(t, site) ? spv_type_vector(&t->b, t->t_i32, 3) : t->t_v2i, img,
                     ibits(t, get_v(t, addr[0])));
            levels = ubits(t, op1(t, SpvOpImageQueryLevels, t->t_i32, img));
        }
        uint32_t sw = op2(t, SpvOpCompositeExtract, t->t_i32, sz, 0), shh = op2(t, SpvOpCompositeExtract, t->t_i32, sz, 1);
        uint32_t rs = site_scale(t, site);
        if (rs) sw = unscaled_size(t, rs, sw), shh = unscaled_size(t, rs, shh);
        uint32_t vals[4] = {ubits(t, sw), ubits(t, shh), cu(t, 1), levels};
        uint32_t out = 0;
        for (uint32_t k = 0; k < 4; k++) {
            if ((in->dmask >> k) & 1) set_v(t, in->vdata + out++, vals[k]);
        }
        return;
    } else if (op == SH_OP_IMAGE_STORE && (!strcmp(in->name, "image_store") ||
                                           (!strcmp(in->name, "image_store_mip") && bound_mip))) {
        /* RDNA2 sends enabled components in order; components outside DMASK are zero. */
        uint32_t coord = storage_coord(t, site, in, addr);
        uint32_t comps[4], n = 0;
        for (uint32_t k = 0; k < 4; k++) {
            uint32_t v;
            if (!((in->dmask >> k) & 1)) {
                v = cu(t, 0);
            } else if (in->d16) { /* D16: two 16-bit components per register */
                uint32_t r = get_v(t, in->vdata + n / 2), h = (n & 1) ? op2(t, SpvOpShiftRightLogical, t->t_u32, r, cu(t, 16))
                                                                       : op2(t, SpvOpBitwiseAnd, t->t_u32, r, cu(t, 0xFFFF));
                v = kind ? h : half_to_bits(t, h);
                n++;
            } else {
                v = get_v(t, in->vdata + n++);
            }
            comps[k] = kind == 1 ? v : kind == 2 ? ibits(t, v) : fbits(t, v);
        }
        /* Narrow movie planes can carry their physical channels in other data slots
         * (R8 with selects 0,0,0,X carries luma in w). Keep that routing bounded to
         * one/two-channel formats. Extending it to full RGBA permutations regressed
         * the existing BGRA byte-copy path; those keep the established store order
         * pending a hardware oracle for the complete channel-routing rules. */
        bool identity = sh->dst_sel[0] == 4 && sh->dst_sel[1] == 5 && sh->dst_sel[2] == 6 && sh->dst_sel[3] == 7;
        bool any = false;
        for (uint32_t k = 0; k < 4; k++) any |= sh->dst_sel[k] >= 4;
        if (!identity && any && sh->img_components && sh->img_components <= 2) {
            uint32_t mem[4];
            for (uint32_t m = 0; m < 4; m++) {
                mem[m] = 0;
                for (uint32_t k = 0; k < 4 && !mem[m]; k++)
                    if (sh->dst_sel[k] == 4 + m) mem[m] = comps[k];
                if (!mem[m]) {
                    uint32_t d = cu(t, 0);
                    mem[m] = kind == 1 ? d : kind == 2 ? ibits(t, d) : fbits(t, d);
                }
            }
            memcpy(comps, mem, sizeof(mem));
        }
        uint32_t texel = packed_site(t, site) ? pack_r11g11b10(t, comps)
                                              : spv_op(&t->b, SpvOpCompositeConstruct, rtype, comps, 4);
        /* The hardware drops a store outside the image; Vulkan leaves it undefined (the Iris Xe
         * writes it somewhere). Shaders aim lanes they skip at -1: Dragon Ball Sparking Zero's
         * bloom setup stores every pixel's color to its half- and quarter-size mips that way. */
        bool three = site_3d(t, site) || site_arrayed(t, site);
        uint32_t size = op1(t, SpvOpImageQuerySize, three ? spv_type_vector(&t->b, t->t_i32, 3) : t->t_v2i, simg);
        uint32_t inside = 0;
        for (uint32_t k = 0; k < (three ? 3u : 2u); k++) {
            uint32_t lt = op2(t, SpvOpULessThan, t->t_bool, op2(t, SpvOpCompositeExtract, t->t_i32, coord, k),
                              op2(t, SpvOpCompositeExtract, t->t_i32, size, k));
            inside = inside ? op2(t, SpvOpLogicalAnd, t->t_bool, inside, lt) : lt;
        }
        /* Uniform control flow around workgroup barriers can carry EXEC-off
         * lanes here with stale VGPRs and in-bounds coordinates. The store is
         * a vector side effect: inactive lanes must leave the image intact. */
        uint32_t merge = begin_if(t, op2(t, SpvOpLogicalAnd, t->t_bool, inside, active(t)));
        spv_op_void(&t->b, SpvOpImageWrite, (uint32_t[]){simg, coord, texel}, 3);
        end_if(t, merge);
        return;
    } else if (op == SH_OP_IMAGE_LOAD && !strcmp(in->name, "image_load") &&
               sh_site_storage(&t->p->sites[site],&t->shapes[site])) {
        uint32_t coord = storage_coord(t, site, in, addr);
        res = packed_site(t, site) ? unpack_r11g11b10(t, op2(t, SpvOpImageRead, t->t_v4u, simg, coord))
                                   : op2(t, SpvOpImageRead, rtype, simg, coord);
    } else if (op == SH_OP_IMAGE_LOAD && !strcmp(in->name, "image_load_mip") &&
               t->p->sites[site].store) {
        /* A proven uniform mip is bound at its real level by the renderer.
         * Other storage-mip loads retain the existing single-level limitation. */
        uint32_t coord = storage_coord(t, site, in, addr);
        res = packed_site(t, site) ? unpack_r11g11b10(t, op2(t, SpvOpImageRead, t->t_v4u, simg, coord))
                                   : op2(t, SpvOpImageRead, rtype, simg, coord);
    } else if (op == SH_OP_IMAGE_LOAD &&
               (!strcmp(in->name, "image_load") || !strcmp(in->name, "image_load_mip"))) {
        /* image_load_mip: the level follows the coordinates */
        uint32_t lod = !bound_mip && !strcmp(in->name, "image_load_mip") && nc < 4
                           ? ibits(t, get_v(t, addr[nc])) : spv_const_i32(&t->b, 0);
        uint32_t img = op1(t, SpvOpImage, t->site_image_type[site], simg);
        uint32_t coord = site_arrayed(t, site) || site_3d(t, site)
                             ? spv_op(&t->b, SpvOpCompositeConstruct, spv_type_vector(&t->b, t->t_i32, 3),
                                      (uint32_t[]){ibits(t, get_v(t, addr[0])), ibits(t, get_v(t, addr[1])),
                                                   nc == 3 ? ibits(t, get_v(t, addr[2])) : spv_const_i32(&t->b, 0)}, 3)
                             : op2(t, SpvOpCompositeConstruct, t->t_v2i, ibits(t, get_v(t, addr[0])), ibits(t, get_v(t, addr[1])));
        uint32_t fs = site_scale(t, site);
        if (fs) coord = op2(t, SpvOpCompositeConstruct, t->t_v2i, scaled_coord(t, fs, ibits(t, get_v(t, addr[0]))),
                            scaled_coord(t, fs, ibits(t, get_v(t, addr[1]))));
        uint32_t a[4] = {img, coord, 2, lod};
        res = spv_op(&t->b, SpvOpImageFetch, rtype, a, 4);
    } else {
        fail(t, "image instruction not supported");
        return;
    }
    uint32_t elem = kind == 1 ? t->t_u32 : kind == 2 ? t->t_i32 : t->t_f32;
    uint32_t c[4], vals[4], n = 0;
    for (uint32_t k = 0; k < 4; k++) c[k] = op2(t, SpvOpCompositeExtract, elem, res, k);
    if (sh->dst_sel[0] == SH_DST_SEL_DYNAMIC) { /* the selects of this draw's T# */
        uint32_t meta = sb_load(t, t->u_var, cu(t, SH_U_SITE + (uint32_t)site)), bits[4];
        for (uint32_t m = 0; m < 4; m++) bits[m] = kind == 1 ? c[m] : ubits(t, c[m]);
        for (uint32_t k = 0; k < 4; k++) {
            if (!((in->dmask >> k) & 1)) continue;
            uint32_t s = bfe_u(t, meta, cu(t, 3 * k), cu(t, 3));
            uint32_t v = sel(t, t->t_u32, op2(t, SpvOpIEqual, t->t_bool, s, cu(t, 1)), cu(t, kind ? 1u : 0x3F800000u), cu(t, 0));
            for (uint32_t m = 0; m < 4; m++) v = sel(t, t->t_u32, op2(t, SpvOpIEqual, t->t_bool, s, cu(t, 4 + m)), bits[m], v);
            vals[n++] = v;
        }
        write_texels(t, in, vals, n, kind);
        return;
    }
    for (uint32_t k = 0; k < 4; k++) {
        if (!((in->dmask >> k) & 1)) continue;
        uint32_t s = sh->dst_sel[k];
        vals[n++] = s >= 4 ? (kind == 1 ? c[s - 4] : ubits(t, c[s - 4])) : cu(t, s == 1 ? (kind ? 1u : 0x3F800000u) : 0u);
    }
    write_texels(t, in, vals, n, kind);
}

/* ---- Interpolation and exports ---- */

bool sh_link_graphics_inputs(const sh_program *vs, const sh_program *ps,
                             sh_stage_info *vs_info, sh_stage_info *ps_info) {
    uint32_t smooth = 0, flat = 0, occupied = vs->param_mask;
    uint8_t destinations[32] = {0}, locations[32];
    memcpy(locations, ps_info->ps_attr_location, sizeof(locations));
    for (uint32_t a = 0; a < 32; a++) {
        if (!((ps->attr_mask >> a) & 1) || (locations[a] & 32u)) continue;
        uint32_t bit = 1u << (locations[a] & 31u);
        occupied |= bit;
        if ((ps_info->ps_flat_mask >> a) & 1) flat |= bit;
        else smooth |= bit;
    }
    uint32_t mixed = smooth & flat;
    for (uint32_t bits = mixed; bits; bits &= bits - 1) {
        if (occupied == UINT32_MAX) return false;
        uint32_t src = (uint32_t)__builtin_ctz(bits), dst = (uint32_t)__builtin_ctz(~occupied);
        destinations[src] = (uint8_t)dst;
        occupied |= 1u << dst;
    }
    for (uint32_t a = 0; a < 32; a++) {
        if (!((ps->attr_mask >> a) & 1) || (locations[a] & 32u) ||
            !((ps_info->ps_flat_mask >> a) & 1)) continue;
        uint32_t src = locations[a] & 31u;
        if ((mixed >> src) & 1) locations[a] = destinations[src];
    }
    vs_info->vs_param_alias_mask = mixed;
    memcpy(vs_info->vs_param_alias, destinations, sizeof(destinations));
    memcpy(ps_info->ps_attr_location, locations, sizeof(locations));
    return true;
}

static uint32_t attr_input(tr *t, uint32_t attr) {
    if (!t->in_attr[attr]) {
        uint32_t loc = t->info->ps_attr_location[attr] & 31u;
        /* Two guest attributes may select the same parameter. One SPIR-V
         * interface variable per location, rather than overlapping declarations. */
        for (uint32_t i = 0; i < 32; i++) {
            if (!t->in_attr[i] || (t->info->ps_attr_location[i] & 31u) != loc) continue;
            if (((t->info->ps_flat_mask >> i) ^ (t->info->ps_flat_mask >> attr)) & 1u) {
                fail(t, "mixed interpolation at one parameter location not supported");
            }
            return t->in_attr[attr] = t->in_attr[i];
        }
        uint32_t ptr = spv_type_pointer(&t->b, SpvStorageClassInput, t->t_v4f);
        uint32_t v = spv_id(&t->b);
        uint32_t a[3] = {ptr, v, SpvStorageClassInput};
        spv_emit(&t->b, SPV_SEC_GLOBAL, SpvOpVariable, a, 3);
        spv_decorate(&t->b, v, SpvDecorationLocation, &loc, 1);
        if ((t->info->ps_flat_mask >> attr) & 1) spv_decorate(&t->b, v, SpvDecorationFlat, NULL, 0);
        t->in_attr[attr] = v;
        t->iface[t->n_iface++] = v;
    }
    return t->in_attr[attr];
}

#include "gpu/shader/shader_bvh_spirv.h"

static uint32_t ps_vertex_attr(tr*t,uint32_t attr,uint32_t vertex) {
    uint32_t loc=t->info->ps_attr_location[attr]&31u;
    if(!t->pv_attr[loc]) {
        uint32_t a=spv_type_array(&t->b,t->t_v4f,cu(t,3));
        uint32_t v=spv_id(&t->b);
        spv_emit(&t->b,SPV_SEC_GLOBAL,SpvOpVariable,(uint32_t[]){spv_type_pointer(&t->b,SpvStorageClassInput,a),v,SpvStorageClassInput},3);
        spv_decorate(&t->b,v,SpvDecorationLocation,&loc,1);
        spv_decorate(&t->b,v,5285,NULL,0); /* PerVertexKHR */
        t->iface[t->n_iface++]=v;t->pv_attr[loc]=v;
    }
    uint32_t ptr=spv_op(&t->b,SpvOpAccessChain,spv_type_pointer(&t->b,SpvStorageClassInput,t->t_v4f),
        (uint32_t[]){t->pv_attr[loc],cu(t,vertex)},2);
    return ld(t,t->t_v4f,ptr);
}
static void interp(tr *t, const rdna2_inst *in) {
    if (in->attr >= 32) {
        fail(t, "pixel attribute index out of range");
        return;
    }
    uint8_t input = t->info->ps_attr_location[in->attr];
    if (input & 32u) {
        /* OFFSET bit 5 selects one of 0000, 0001, 1110, 1111.
         * It does not read location zero, nor require a VS/GS export. */
        uint32_t def = input >> 6;
        bool one = in->attr_chan == 3 ? (def & 1u) != 0 : (def & 2u) != 0;
        set_v(t, in->dst - RDNA2_OP_VGPR, ubits(t, cf(t, one ? 1.0f : 0.0f)));
        return;
    }
    uint32_t loc=input&31u;
    if((t->pv_mask>>loc)&1u) {
        uint32_t v;
        if(t->p->ops[t->inst]==SH_OP_V_INTERP_MOV_F32) {
            const unsigned indices[3]={1,2,0};
            v=ps_vertex_attr(t,in->attr,indices[(in->src[0]-RDNA2_OP_VGPR)%3]);
        } else {
            /* The parameter cache supplies plane coefficients, not three
             * pre-weighted values. P1 starts with P0; P2 accumulates VDST.
             * Respecting the source VGPR also preserves custom barycentrics.
             * Equal finite vertex values have zero deltas and retain their
             * exact bits (weighted endpoint sums introduce noise on NVIDIA). */
            uint32_t f=t->t_f32,dst=in->dst-RDNA2_OP_VGPR;
            bool first=t->p->ops[t->inst]==SH_OP_V_INTERP_P1_F32;
            uint32_t p0=op2(t,SpvOpCompositeExtract,f,ps_vertex_attr(t,in->attr,0),in->attr_chan);
            uint32_t delta=cf(t,0);
            if(!((t->info->ps_flat_mask>>in->attr)&1u)) {
                uint32_t endpoint=op2(t,SpvOpCompositeExtract,f,ps_vertex_attr(t,in->attr,first?1:2),in->attr_chan);
                delta=op2(t,SpvOpFSub,f,endpoint,p0);
            }
            uint32_t base=first?p0:fbits(t,get_v(t,dst));
            uint32_t value=ext3(t,f,GLSLstd450Fma,delta,fbits(t,src(t,in,0)),base);
            set_v(t,dst,ubits(t,value));return;
        }
        set_v(t,in->dst-RDNA2_OP_VGPR,ubits(t,op2(t,SpvOpCompositeExtract,t->t_f32,v,in->attr_chan)));return;
    }
    uint32_t v = ld(t, t->t_v4f, attr_input(t, in->attr));
    set_v(t, in->dst - RDNA2_OP_VGPR, ubits(t, op2(t, SpvOpCompositeExtract, t->t_f32, v, in->attr_chan)));
}

static uint32_t output_var(tr *t, uint32_t type, uint32_t location) {
    uint32_t ptr = spv_type_pointer(&t->b, SpvStorageClassOutput, type);
    uint32_t v = spv_id(&t->b);
    uint32_t a[3] = {ptr, v, SpvStorageClassOutput};
    spv_emit(&t->b, SPV_SEC_GLOBAL, SpvOpVariable, a, 3);
    spv_decorate(&t->b, v, SpvDecorationLocation, &location, 1);
    t->iface[t->n_iface++] = v;
    return v;
}

static uint32_t input_var(tr *t,uint32_t type,uint32_t builtin);
#include "gpu/shader/shader_mesh_spirv.h"

static void export_op(tr *t, const rdna2_inst *in) {
    uint32_t F = t->t_f32, tgt = in->exp_target;
    if(t->info->stage==SH_STAGE_MESH && tgt==20) {
        uint32_t merge=begin_if(t,active(t));
        st(t,t->mesh_primitive,get_v(t,in->exp_src[0]));end_if(t,merge);return;
    }
    if (t->vm_valid && in->exp_vm && (!t->probe_var || t->ps_capture) && !t->ps_solid) {
        /* VM supplies the live fragment mask, including EXEC-off lanes. A
         * previous MRT write must not keep a fragment that this export rejects.
         * Delay host discard until finish so ordered interlocks stay balanced. */
        st(t,t->vm_valid,op2(t,SpvOpLogicalAnd,t->t_bool,ld(t,t->t_bool,t->vm_valid),active(t)));
    }
    uint32_t kind = t->info->stage == SH_STAGE_PS && tgt < 8 ? (t->info->ps_mrt_kind >> (2 * tgt)) & 3u : 0;
    if (kind) {
        /* Integer exports carry register bits. Converting the corresponding
         * float (or unpacking it as half) destroys IDs, masks and packed data. */
        uint32_t c[4], type = kind == 2 ? t->t_i32 : t->t_u32;
        uint32_t format = (t->info->ps_col_format >> (4 * tgt)) & 15u;
        if (in->exp_compr && (format < 4 || format > 8)) {
            fail(t, "unsupported compressed color export format");
            return;
        }
        for (uint32_t k = 0; k < 4; k++) {
            uint32_t value = cu(t, k == 3 ? 1 : 0);
            if (in->exp_compr) {
                uint32_t pair = get_v(t, in->exp_src[k / 2]);
                if (format >= 4 && format <= 6) {
                    /* The export encoding and attachment kind are independent.
                     * Decode FP16/normalized data first, then preserve its F32
                     * register bits for integer attachments (as with 32-bit
                     * exports). Numeric float-to-int conversion loses them. */
                    uint32_t unpack = format == 5 ? GLSLstd450UnpackUnorm2x16 :
                                      format == 6 ? GLSLstd450UnpackSnorm2x16 : GLSLstd450UnpackHalf2x16;
                    uint32_t decoded = ext1(t, t->t_v2f, unpack, pair);
                    value = ubits(t, op2(t, SpvOpCompositeExtract, F, decoded, k & 1u));
                } else value = format == 8 ? ubits(t, op3(t, SpvOpBitFieldSExtract, t->t_i32, ibits(t, pair),
                                                 cu(t, (k & 1u) * 16), cu(t, 16))) :
                                      op3(t, SpvOpBitFieldUExtract, t->t_u32, pair,
                                          cu(t, (k & 1u) * 16), cu(t, 16));
            } else if ((in->exp_en >> k) & 1u) {
                value = get_v(t, in->exp_src[k]);
            }
            c[k] = kind == 2 ? ibits(t, value) : value;
        }
        if ((t->info->ps_mrt_rb_swap >> tgt) & 1u) {
            uint32_t tmp = c[0]; c[0] = c[2]; c[2] = tmp;
        }
        uint32_t vec = spv_op(&t->b, SpvOpCompositeConstruct, spv_type_vector(&t->b, type, 4), c, 4);
        uint32_t merge = begin_if(t, active(t));
        st(t, t->out_mrt[tgt], vec);
        st(t, t->exported, spv_const_bool(&t->b, true));
        end_if(t, merge);
        return;
    }
    uint32_t c[4];
    if (in->exp_compr) {
        /* SPI_SHADER_COL_FORMAT: FP16_ABGR packs halves (v_cvt_pkrtz), UNORM16_ABGR / SNORM16_ABGR
         * normalized 16-bit values (v_cvt_pknorm_u16 / _i16) */
        uint32_t format = t->info->stage == SH_STAGE_PS && tgt < 8 ? (t->info->ps_col_format >> (4 * tgt)) & 15u : 4u;
        uint32_t unpack = format == 5 ? GLSLstd450UnpackUnorm2x16 : format == 6 ? GLSLstd450UnpackSnorm2x16 : GLSLstd450UnpackHalf2x16;
        uint32_t lo = ext1(t, t->t_v2f, unpack, get_v(t, in->exp_src[0]));
        uint32_t hi = ext1(t, t->t_v2f, unpack, get_v(t, in->exp_src[1]));
        c[0] = op2(t, SpvOpCompositeExtract, F, lo, 0);
        c[1] = op2(t, SpvOpCompositeExtract, F, lo, 1);
        c[2] = op2(t, SpvOpCompositeExtract, F, hi, 0);
        c[3] = op2(t, SpvOpCompositeExtract, F, hi, 1);
    } else {
        for (int k = 0; k < 4; k++) c[k] = (in->exp_en >> k) & 1 ? fbits(t, get_v(t, in->exp_src[k])) : cf(t, k == 3 ? 1.0f : 0.0f);
    }
    if(t->nan_trace && tgt==0)for(unsigned k=0;k<4;k++)trace_packed_nonfinite(t,c[k],32768);
    if (t->info->stage == SH_STAGE_PS && tgt < 8 && ((t->info->ps_mrt_rb_swap >> tgt) & 1u)) {
        uint32_t tmp = c[0]; c[0] = c[2]; c[2] = tmp;
    }
    uint32_t vec[4] = {c[0], c[1], c[2], c[3]};
    uint32_t v = spv_op(&t->b, SpvOpCompositeConstruct, t->t_v4f, vec, 4);
    if(t->info->stage==SH_STAGE_MESH) { mesh_export(t,in,v,c);return; }
    if (t->info->stage == SH_STAGE_VS && tgt == 12) {
        /* The guest viewport maps clip space to pixels without clipping x/y
         * at +-w (guard band); here the pixels are mapped back to clip space
         * of a viewport covering the whole target:
         *   x' = (x * xscale + w * xoffset) * 2 / width - w  (same for y)
         * and depth is the guest's viewport depth: z' = z * zscale + w * zoffset */
        uint32_t w = c[3], out[2];
        for (int k = 0; k < 2; k++) {
            uint32_t sc = fbits(t, sb_load(t, t->u_var, cu(t, SH_U_VPORT + 2 * (uint32_t)k)));
            uint32_t of = fbits(t, sb_load(t, t->u_var, cu(t, SH_U_VPORT + 2 * (uint32_t)k + 1)));
            uint32_t inv = fbits(t, sb_load(t, t->u_var, cu(t, SH_U_VPORT + 4 + (uint32_t)k)));
            uint32_t px = op2(t, SpvOpFAdd, F, op2(t, SpvOpFMul, F, c[k], sc), op2(t, SpvOpFMul, F, w, of));
            out[k] = op2(t, SpvOpFSub, F, op2(t, SpvOpFMul, F, px, inv), w);
        }
        uint32_t zs = fbits(t, sb_load(t, t->u_var, cu(t, SH_U_VPORT + 6)));
        uint32_t zo = fbits(t, sb_load(t, t->u_var, cu(t, SH_U_VPORT + 7)));
        uint32_t z = op2(t, SpvOpFAdd, F, op2(t, SpvOpFMul, F, c[2], zs), op2(t, SpvOpFMul, F, w, zo));
        uint32_t vec2[4] = {out[0], out[1], z, c[3]};
        v = spv_op(&t->b, SpvOpCompositeConstruct, t->t_v4f, vec2, 4);
        uint32_t a[2] = {t->pervertex, spv_const_i32(&t->b, 0)};
        uint32_t p = spv_op(&t->b, SpvOpAccessChain, spv_type_pointer(&t->b, SpvStorageClassOutput, t->t_v4f), a, 2);
        st(t, p, v);
        /* user clip planes: the clipper keeps dot(plane, guest clip position) >= 0 */
        uint32_t k = 0;
        for (uint32_t m = t->clip_member ? t->info->vs_ucp_mask & 63u : 0; m; m &= m - 1, k++) {
            uint32_t plane = (uint32_t)__builtin_ctz(m), dot = 0;
            for (uint32_t j = 0; j < 4; j++) {
                uint32_t term = op2(t, SpvOpFMul, F, c[j], fbits(t, sb_load(t, t->u_var, cu(t, SH_U_UCP + 4 * plane + j))));
                dot = j ? op2(t, SpvOpFAdd, F, dot, term) : term;
            }
            uint32_t ca[3] = {t->pervertex, spv_const_i32(&t->b, (int32_t)t->clip_member), spv_const_i32(&t->b, (int32_t)k)};
            st(t, spv_op(&t->b, SpvOpAccessChain, spv_type_pointer(&t->b, SpvStorageClassOutput, F), ca, 3), dot);
        }
    } else if (t->info->stage == SH_STAGE_VS && tgt >= 32 && tgt < 64) {
        st(t, t->out_param[tgt - 32], v);
        if (t->out_alias[tgt - 32]) st(t, t->out_alias[tgt - 32], v);
    } else if (t->info->stage == SH_STAGE_PS && tgt <= 9) {
        /* colors, depth (mrtz: depth in x) and null all keep the pixel. Probing, the
         * discard path's null export (no channels) writes the probed values too, from
         * every lane: what decided the discard */
        bool probe_kill = t->probe_var && !t->ps_capture && tgt == 0 && !in->exp_en && in->exp_done;
        uint32_t merge = begin_if(t, probe_kill ? spv_const_bool(&t->b, true) : active(t));
        if (t->probe_var && !t->ps_capture && tgt == 0) { /* the probed values: one gray, or R, G, B */
            uint32_t f[3];
            for (uint32_t k = 0; k < 3; k++) f[k] = fbits(t, ld(t, t->t_u32, t->probe_vars[k < t->probe_count ? k : 0]));
            v = spv_op(&t->b, SpvOpCompositeConstruct, t->t_v4f, (uint32_t[]){f[0], f[1], f[2], cf(t, 1.0f)}, 4);
        }
        if (tgt < 8) st(t, t->out_mrt[tgt], v);
        else if (tgt == 8 && t->out_depth && (in->exp_en & 1)) st(t, t->out_depth, c[0]);
        st(t, t->exported, spv_const_bool(&t->b, true));
        end_if(t, merge);
    }
    if (t->info->stage == SH_STAGE_VS && tgt == 13 && t->out_layer && ((in->exp_en >> 2) & 1) && !in->exp_compr) {
        /* the misc vector: z[10:0] is the target layer (z[19:16] the viewport) */
        uint32_t z = op2(t, SpvOpBitwiseAnd, t->t_u32, get_v(t, in->exp_src[2]), cu(t, 0x7FF));
        st(t, t->out_layer, ibits(t, z));
    }
    /* prim, pos1-3 (but the layer): ignored */
}

/* ---- Program structure ---- */

/* End of a pixel shader: a lane that exported no color is discarded (the
 * hardware drops pixels whose lanes are off at the final export). */
static void finish(tr *t) {
    if(t->info->stage==SH_STAGE_MESH) { mesh_finish(t);spv_op_void(&t->b,SpvOpReturn,NULL,0);return; }
    /* End before every return/discard, including a fragment without exports. */
    if(t->ordered_pixel) spv_op_void(&t->b,SpvOpEndInvocationInterlockEXT,NULL,0);
    if(t->ps_capture) {
        uint32_t U=t->t_u32,F=t->t_f32,frag=ld(t,t->t_v4f,t->capture_frag);
        uint32_t x=op1(t,SpvOpConvertFToU,U,op2(t,SpvOpCompositeExtract,F,frag,0));
        uint32_t y=op1(t,SpvOpConvertFToU,U,op2(t,SpvOpCompositeExtract,F,frag,1));
        uint32_t len=sb_length(t,t->u_var);
        uint32_t tx=cu(t,t->capture_x),ty=cu(t,t->capture_y);
        const char *point=getenv("PS5C_PS_CAPTURE_PIXEL");
        if(point && *point=='@') {
            tx=sb_load(t,t->u_var,op2(t,SpvOpISub,U,len,cu(t,8)));
            ty=sb_load(t,t->u_var,op2(t,SpvOpISub,U,len,cu(t,7)));
        }
        uint32_t mx=op2(t,SpvOpLogicalOr,t->t_bool,op2(t,SpvOpIEqual,t->t_bool,tx,cu(t,UINT32_MAX)),op2(t,SpvOpIEqual,t->t_bool,x,tx));
        uint32_t my=op2(t,SpvOpLogicalOr,t->t_bool,op2(t,SpvOpIEqual,t->t_bool,ty,cu(t,UINT32_MAX)),op2(t,SpvOpIEqual,t->t_bool,y,ty));
        uint32_t selected=begin_if(t,op2(t,SpvOpLogicalAnd,t->t_bool,
            op2(t,SpvOpLogicalAnd,t->t_bool,mx,my),ld(t,t->t_bool,t->capture_reached)));
        spv_op(&t->b,SpvOpAtomicIAdd,U,(uint32_t[]){
            sb_ptr(t,t->u_var,op2(t,SpvOpISub,U,len,cu(t,4))),cu(t,1),cu(t,0),cu(t,1)},4);
        uint32_t slot=sb_ptr(t,t->u_var,op2(t,SpvOpISub,U,len,cu(t,1)));
        uint32_t old=spv_op(&t->b,SpvOpAtomicCompareExchange,U,
            (uint32_t[]){slot,cu(t,1),cu(t,0),cu(t,0),cu(t,1),cu(t,0xEEEEEEEEu)},6);
        uint32_t claimed=begin_if(t,op2(t,SpvOpIEqual,t->t_bool,old,cu(t,0xEEEEEEEEu)));
        uint32_t start=op2(t,SpvOpISub,U,len,cu(t,16));
        for(uint32_t k=0;k<t->probe_count;k++)
            st(t,sb_ptr(t,t->u_var,op2(t,SpvOpIAdd,U,start,cu(t,k))),ld(t,U,t->probe_vars[k]));
        st(t,sb_ptr(t,t->u_var,op2(t,SpvOpIAdd,U,start,cu(t,10))),x);
        st(t,sb_ptr(t,t->u_var,op2(t,SpvOpIAdd,U,start,cu(t,11))),y);
        end_if(t,claimed);end_if(t,selected);
    }
    if (t->vs_probe) {
        /* the chosen vertex's probed values (vertex 4294967295: whichever claims the slot first) go to
         * the last 16 words of the uniform buffer; the last one holds the vertex index */
        uint32_t U = t->t_u32, vid;
        if (t->info->stage == SH_STAGE_CS) { /* a compute thread: global x | y << 16 */
            uint32_t wg = ld(t, t->t_v3u, t->in_wgid), lid = ld(t, t->t_v3u, t->in_lid), g[2];
            for (uint32_t k = 0; k < 2; k++)
                g[k] = op2(t, SpvOpIAdd, U, op2(t, SpvOpIMul, U, op2(t, SpvOpCompositeExtract, U, wg, k),
                                                cu(t, t->info->local_size[k] ? t->info->local_size[k] : 1)),
                           op2(t, SpvOpCompositeExtract, U, lid, k));
            vid = op2(t, SpvOpBitwiseOr, U, g[0], op2(t, SpvOpShiftLeftLogical, U, g[1], cu(t, 16)));
        } else {
            vid = op1(t, SpvOpBitcast, U, ld(t, t->t_i32, t->in_vid));
        }
        uint32_t len = sb_length(t,t->u_var);
        if (t->vs_probe_vid == 0xFFFFFFFEu && t->probe_count >= 4) {
            /* the first 4 vertices to get here: index, x/w, y/w of the first, second and fourth probes */
            uint32_t F = t->t_f32, base = op2(t, SpvOpISub, U, len, cu(t, 16));
            uint32_t ctr = sb_ptr(t, t->u_var, op2(t, SpvOpIAdd, U, base, cu(t, 14)));
            uint32_t n = op2(t, SpvOpISub, U,
                             spv_op(&t->b, SpvOpAtomicIAdd, U, (uint32_t[]){ctr, cu(t, 1), cu(t, 0), cu(t, 1)}, 4),
                             cu(t, 0xEEEEEEEEu));
            uint32_t few = begin_if(t, op2(t, SpvOpULessThan, t->t_bool, n, cu(t, 4)));
            uint32_t at = op2(t, SpvOpIAdd, U, base, op2(t, SpvOpIMul, U, n, cu(t, 3)));
            uint32_t w = fbits(t, ld(t, U, t->probe_vars[3]));
            st(t, sb_ptr(t, t->u_var, at), vid);
            for (uint32_t k = 0; k < 2; k++)
                st(t, sb_ptr(t, t->u_var, op2(t, SpvOpIAdd, U, at, cu(t, k + 1))),
                   ubits(t, op2(t, SpvOpFDiv, F, fbits(t, ld(t, U, t->probe_vars[k])), w)));
            st(t, sb_ptr(t, t->u_var, op2(t, SpvOpISub, U, len, cu(t, 1))), cu(t, 7));
            end_if(t, few);
            spv_op_void(&t->b, SpvOpReturn, NULL, 0);
            return;
        }
        uint32_t chosen = t->vs_probe_vid == 0xFFFFFFFFu ? spv_const_bool(&t->b, true)
                                                         : op2(t, SpvOpIEqual, t->t_bool, vid, cu(t, t->vs_probe_vid));
        if (t->info->stage == SH_STAGE_CS && getenv("PS5C_VS_PROBE_Z")) { /* PS5C_VS_PROBE_Z=<n>: and local z == n */
            uint32_t lz = op2(t, SpvOpCompositeExtract, U, ld(t, t->t_v3u, t->in_lid), 2);
            chosen = op2(t, SpvOpLogicalAnd, t->t_bool, chosen,
                         op2(t, SpvOpIEqual, t->t_bool, lz, cu(t, (uint32_t)strtoul(getenv("PS5C_VS_PROBE_Z"), NULL, 10))));
        }
        { /* PS5C_VS_PROBE_HALF_GT=<k>:<limit>: only where probe k's low half (a 16-bit float) exceeds the limit */
            const char *e = getenv("PS5C_VS_PROBE_HALF_GT");
            if (e && strchr(e, ':')) {
                uint32_t k = (uint32_t)atoi(e);
                float lim = (float)atof(strchr(e, ':') + 1);
                if (k < t->probe_count) {
                    uint32_t h = ext1(t, t->t_v2f, GLSLstd450UnpackHalf2x16, ld(t, U, t->probe_vars[k]));
                    uint32_t f = ext1(t, t->t_f32, GLSLstd450FAbs, op2(t, SpvOpCompositeExtract, t->t_f32, h, 0));
                    chosen = op2(t, SpvOpFOrdGreaterThan, t->t_bool, f, cf(t, lim));
                }
            }
        }
        uint32_t claim = begin_if(t, chosen);
        uint32_t slot = sb_ptr(t, t->u_var, op2(t, SpvOpISub, U, len, cu(t, 1)));
        uint32_t old = spv_op(&t->b, SpvOpAtomicCompareExchange, U,
                              (uint32_t[]){slot, cu(t, 1 /* device */), cu(t, 0), cu(t, 0), vid, cu(t, 0xEEEEEEEEu)}, 6);
        uint32_t merge = begin_if(t, op2(t, SpvOpIEqual, t->t_bool, old, cu(t, 0xEEEEEEEEu)));
        for (uint32_t k = 0; k < t->probe_count && k < 15; k++) {
            uint32_t at = op2(t, SpvOpIAdd, U, op2(t, SpvOpISub, U, len, cu(t, 16)), cu(t, k));
            st(t, sb_ptr(t, t->u_var, at), ld(t, U, t->probe_vars[k]));
        }
        end_if(t, merge);
        end_if(t, claim);
    }
    if (t->ps_solid) {
        for (uint32_t k = 0; k < 8; k++)
            if (t->out_mrt[k] && !((t->info->ps_mrt_kind >> (2 * k)) & 3u))
                st(t, t->out_mrt[k], spv_op(&t->b, SpvOpCompositeConstruct, t->t_v4f,
                                            (uint32_t[]){cf(t, (0x9Bu >> (t->ps_solid_index % 8)) & 1 ? 1.0f : 0.0f),
                                                         cf(t, (0xD4u >> (t->ps_solid_index % 8)) & 1 ? 1.0f : 0.0f),
                                                         cf(t, (0x69u >> (t->ps_solid_index % 8)) & 1 ? 1.0f : 0.0f), cf(t, 1.0f)}, 4));
        spv_op_void(&t->b, SpvOpReturn, NULL, 0);
        return;
    }
    if (t->info->stage == SH_STAGE_PS) {
        if (t->probe_var && !t->ps_capture && t->out_mrt[0] && !(t->info->ps_mrt_kind & 3u)) {
            /* A discard can branch past every color export. The probe disables
             * discard, so write its values here too instead of exposing an
             * uninitialized output on that path. Diagnostic mode only. */
            uint32_t f[4];
            for (uint32_t k=0;k<3;k++)
                f[k]=fbits(t,ld(t,t->t_u32,t->probe_vars[k<t->probe_count?k:0]));
            f[3]=cf(t,1.0f);
            st(t,t->out_mrt[0],spv_op(&t->b,SpvOpCompositeConstruct,t->t_v4f,f,4));
        }
        uint32_t kill_l = spv_id(&t->b), merge = spv_id(&t->b);
        uint32_t cond = op1(t, SpvOpLogicalNot, t->t_bool, ld(t, t->t_bool, t->exported));
        if (t->vm_valid)
            cond = op2(t,SpvOpLogicalOr,t->t_bool,cond,
                       op1(t,SpvOpLogicalNot,t->t_bool,ld(t,t->t_bool,t->vm_valid)));
        if (t->probe_var && !t->ps_capture) cond = spv_const_bool(&t->b, false); /* probing: every pixel shows its values */
        if (t->covered)
            cond = op2(t, SpvOpLogicalOr, t->t_bool, cond,
                       op1(t, SpvOpLogicalNot, t->t_bool, ld(t, t->t_bool, t->covered)));
        spv_op_void(&t->b, SpvOpSelectionMerge, (uint32_t[]){merge, SpvSelectionControlNone}, 2);
        spv_op_void(&t->b, SpvOpBranchConditional, (uint32_t[]){cond, kill_l, merge}, 3);
        spv_label(&t->b, kill_l);
        spv_op_void(&t->b, SpvOpKill, NULL, 0);
        spv_label(&t->b, merge);
    }
    spv_op_void(&t->b, SpvOpReturn, NULL, 0);
}

/* Read-modify-writes with no SPIR-V atomic of their own, as a compare-exchange
 * loop on `ptr` (scope 1 device, 2 workgroup). Returns the old value. */
static uint32_t atomic_loop(tr *t, uint32_t ptr, uint32_t scope, int kind, uint32_t data, uint32_t data2) {
    uint32_t U = t->t_u32, B = t->t_bool, F = t->t_f32;
    uint32_t header = spv_id(&t->b), body = spv_id(&t->b), cont = spv_id(&t->b), merge = spv_id(&t->b);
    uint32_t cur = spv_local_var(&t->b, t->p_fn_u32), done = spv_local_var(&t->b, t->p_fn_bool);
    /* an atomic load: image texel pointers take no plain load */
    st(t, cur, spv_op(&t->b, SpvOpAtomicLoad, U, (uint32_t[]){ptr, cu(t, scope), cu(t, 0)}, 3));
    st(t, done, spv_const_bool(&t->b, false));
    branch(t, header);
    spv_label(&t->b, header);
    uint32_t loop_cond = ld(t, B, done); /* before the merge: nothing may come between it and the branch */
    spv_op_void(&t->b, SpvOpLoopMerge, (uint32_t[]){merge, cont, 0}, 3);
    spv_op_void(&t->b, SpvOpBranchConditional, (uint32_t[]){loop_cond, merge, body}, 3);
    spv_label(&t->b, body);
    uint32_t c = ld(t, U, cur), want;
    switch (kind) {
    case RMW_INC: /* wraps to 0 past data */
        want = sel(t, U, op2(t, SpvOpUGreaterThanEqual, B, c, data), cu(t, 0), op2(t, SpvOpIAdd, U, c, cu(t, 1)));
        break;
    case RMW_DEC: /* wraps to data at 0 or above it */
        want = sel(t, U, op2(t, SpvOpLogicalOr, B, op2(t, SpvOpIEqual, B, c, cu(t, 0)), op2(t, SpvOpUGreaterThan, B, c, data)),
                   data, op2(t, SpvOpISub, U, c, cu(t, 1)));
        break;
    case RMW_RSUB: want = op2(t, SpvOpISub, U, data, c); break;
    case RMW_MSKOR: want = op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, c, op1(t, SpvOpNot, U, data)), data2); break;
    case RMW_FADD: want = ubits(t, op2(t, SpvOpFAdd, F, fbits(t, c), fbits(t, data))); break;
    case RMW_FMIN: want = ubits(t, ext2(t, F, GLSLstd450NMin, fbits(t, c), fbits(t, data))); break;
    case RMW_FMAX: want = ubits(t, ext2(t, F, GLSLstd450NMax, fbits(t, c), fbits(t, data))); break;
    default: /* RMW_WRAP: down by data while at least data, else up by data2 */
        want = sel(t, U, op2(t, SpvOpUGreaterThanEqual, B, c, data), op2(t, SpvOpISub, U, c, data), op2(t, SpvOpIAdd, U, c, data2));
        break;
    }
    uint32_t got = spv_op(&t->b, SpvOpAtomicCompareExchange, U, (uint32_t[]){ptr, cu(t, scope), cu(t, 0), cu(t, 0), want, c}, 6);
    st(t, done, op2(t, SpvOpIEqual, B, got, c));
    st(t, cur, got);
    branch(t, cont);
    spv_label(&t->b, cont);
    branch(t, header);
    spv_label(&t->b, merge);
    return ld(t, U, cur);
}

/* ---- Local data share (compute): a Workgroup array of dwords ---- */

static uint32_t lds_ptr(tr *t, uint32_t byte_addr) {
    /* clamped: an access past the array (undefined here, dropped on the hardware)
     * can take the host GPU down */
    uint32_t idx = ext2(t, t->t_u32, GLSLstd450UMin, op2(t, SpvOpShiftRightLogical, t->t_u32, byte_addr, cu(t, 2)),
                        cu(t, t->lds_dwords - 1));
    if(t->lds64) return spv_op(&t->b,SpvOpAccessChain,t->p_wg_u32,(uint32_t[]){t->lds,cu(t,0),idx},3);
    return op2(t, SpvOpAccessChain, t->p_wg_u32, t->lds, idx);
}
static uint32_t lds_ptr64(tr *t,uint32_t addr) {
    uint32_t idx=ext2(t,t->t_u32,GLSLstd450UMin,op2(t,SpvOpShiftRightLogical,t->t_u32,addr,cu(t,3)),cu(t,t->lds_dwords/2-1));
    return spv_op(&t->b,SpvOpAccessChain,spv_type_pointer(&t->b,SpvStorageClassWorkgroup,t->t_u64),
                  (uint32_t[]){t->lds64,cu(t,0),idx},3);
}
static uint32_t lds_addr(tr *t, uint32_t base, uint32_t off) { return op2(t, SpvOpIAdd, t->t_u32, base, cu(t, off)); }
static void lds_store(tr *t, uint32_t addr, uint32_t v) { st(t, lds_ptr(t, addr), v); }
static uint32_t lds_load(tr *t, uint32_t addr) { return ld(t, t->t_u32, lds_ptr(t, addr)); }

/* The same lane-private array backs explicit SCRATCH and private FLAT.
 * LDS is shared with DS instructions. No aperture bytes enter a global SSBO.
 * Capture the full address before writing potentially overlapping VGPRs. */
static void flat_op(tr *t,const rdna2_inst *in,uint32_t op,int site) {
    uint32_t U=t->t_u32,B=t->t_bool;
    uint32_t lo=get_v(t,in->vaddr),hi=get_v(t,in->vaddr+1);
    uint32_t off=op2(t,SpvOpIAdd,U,lo,cu(t,(uint32_t)in->offset));
    hi=op2(t,SpvOpIAdd,U,hi,sel(t,U,op2(t,SpvOpULessThan,B,off,lo),cu(t,1),cu(t,0)));
    uint32_t shared=op2(t,SpvOpIEqual,B,hi,cu(t,SH_SHARED_APERTURE_HIGH));
    uint32_t priv=op2(t,SpvOpIEqual,B,hi,cu(t,SH_PRIVATE_APERTURE_HIGH));
    uint32_t n=strstr(in->name,"dwordx4")?4:strstr(in->name,"dwordx3")?3:strstr(in->name,"dwordx2")?2:1;
    uint32_t value[4],result[4]={0};
    bool store=op==SH_OP_GLOBAL_STORE;
    for(uint32_t k=0;k<n;k++) {
        value[k]=store?get_v(t,in->vdata+k):cu(t,0);
        if(!store){result[k]=spv_local_var(&t->b,t->p_fn_u32);st(t,result[k],cu(t,0));}
    }
    uint32_t aligned=op2(t,SpvOpIEqual,B,op2(t,SpvOpBitwiseAnd,U,off,cu(t,3)),cu(t,0));
    for(unsigned space=0;space<2;space++) {
        uint32_t limit=space?SCRATCH_DWORDS:t->lds_dwords;
        if(limit<n)continue;
        uint32_t cond=op2(t,SpvOpLogicalAnd,B,space?priv:shared,aligned);
        cond=op2(t,SpvOpLogicalAnd,B,cond,op2(t,SpvOpULessThanEqual,B,off,cu(t,(limit-n)*4)));
        if(store)cond=op2(t,SpvOpLogicalAnd,B,cond,active(t));
        uint32_t merge=begin_if(t,cond);
        for(uint32_t k=0;k<n;k++) {
            uint32_t addr=op2(t,SpvOpIAdd,U,off,cu(t,k*4));
            uint32_t ptr=space?op2(t,SpvOpAccessChain,t->p_pr_u32,t->scratch,
                op2(t,SpvOpShiftRightLogical,U,addr,cu(t,2))):lds_ptr(t,addr);
            if(store)st(t,ptr,value[k]);else st(t,result[k],ld(t,U,ptr));
        }
        end_if(t,merge);
    }
    uint32_t local=op2(t,SpvOpLogicalOr,B,shared,priv);
    uint32_t merge=begin_if(t,op1(t,SpvOpLogicalNot,B,local));
    global_op(t,in,op,site);
    if(!store)for(uint32_t k=0;k<n;k++)st(t,result[k],get_v(t,in->dst-RDNA2_OP_VGPR+k));
    end_if(t,merge);
    if(!store)for(uint32_t k=0;k<n;k++)set_v(t,in->dst-RDNA2_OP_VGPR+k,ld(t,U,result[k]));
}

/* GDS uses the M0[31:16] base. Ordinary accesses are per lane;
 * APPEND/CONSUME below retain whole-wave reservation and broadcast.
 * Reads/writes of 1-4 dwords and 32-bit atomics share the physical buffer. */
static void gds_op(tr *t, const rdna2_inst *in, int site) {
    uint32_t U = t->t_u32, B = t->t_bool, hw = in->op;
    uint32_t nread = hw == 0x36 ? 1 : hw == 0x76 ? 2 : hw == 0xFE ? 3 : hw == 0xFF ? 4 : 0;
    uint32_t nwrite = hw == 0x0D ? 1 : hw == 0x4D ? 2 : hw == 0xDE ? 3 : hw == 0xDF ? 4 : 0;
    uint32_t a = hw >= 0x20 && hw <= 0x2D ? hw - 0x20 : hw; /* the atomic, _rtn or not */
    bool rtn = hw >= 0x20 && hw <= 0x2D, atomic = a <= 0x0B || (rtn && a == 0x0D);
    if (site >= 0 && (nread || nwrite || atomic)) {
        uint32_t var = t->site_var[site], dst = (in->dst - RDNA2_OP_VGPR) & 255, n = nread ? nread : nwrite ? nwrite : 1;
        uint32_t addr = op2(t, SpvOpIAdd, U, get_v(t, in->vaddr), cu(t, (uint32_t)in->offset & 0xffff));
        addr = op2(t, SpvOpIAdd, U, addr, op2(t, SpvOpShiftRightLogical, U, get_s(t, RDNA2_OP_M0), cu(t, 16)));
        uint32_t dw = op2(t, SpvOpShiftRightLogical, U, addr, cu(t, 2));
        uint32_t len = buffer_dwords(t,var);
        /* Arena bindings include guard padding, which is outside physical GDS. */
        len = ext2(t, U, GLSLstd450UMin, len, cu(t, SH_GDS_SIZE / 4));
        uint32_t aligned = op2(t, SpvOpIEqual, B, op2(t, SpvOpBitwiseAnd, U, addr, cu(t, 3)), cu(t, 0));
        uint32_t valid = op2(t, SpvOpLogicalAnd, B, aligned,
                             op2(t, SpvOpULessThanEqual, B, op2(t, SpvOpIAdd, U, dw, cu(t, n)), len));
        uint32_t result[4] = {0};
        if (nread || rtn)
            for (uint32_t k = 0; k < n; k++) {
                result[k] = spv_local_var(&t->b, t->p_fn_u32);
                st(t, result[k], cu(t, 0));
            }
        uint32_t merge = begin_if(t, op2(t, SpvOpLogicalAnd, B, active(t), valid));
        /* All waves/workgroups share this StorageBuffer, so use Device scope
         * rather than a Workgroup atomic. */
        for (uint32_t k = 0; k < n; k++) {
            uint32_t ptr = sb_ptr(t, var, op2(t, SpvOpIAdd, U, dw, cu(t, k))), data = get_v(t, in->vdata + k), old = 0;
            if (nread) old = spv_op(&t->b, SpvOpAtomicLoad, U, (uint32_t[]){ptr, cu(t, 1), cu(t, 0)}, 3);
            else if (nwrite) spv_op_void(&t->b, SpvOpAtomicStore, (uint32_t[]){ptr, cu(t, 1), cu(t, 0), data}, 4);
            else if (a == 0x02 || a == 0x03 || a == 0x04)
                old = atomic_loop(t, ptr, 1, a == 0x02 ? RMW_RSUB : a == 0x03 ? RMW_INC : RMW_DEC, data, 0);
            else {
                static const uint16_t ops[] = {SpvOpAtomicIAdd, SpvOpAtomicISub, 0, 0, 0, SpvOpAtomicSMin, SpvOpAtomicSMax,
                                            SpvOpAtomicUMin, SpvOpAtomicUMax, SpvOpAtomicAnd, SpvOpAtomicOr, SpvOpAtomicXor,
                                            0, SpvOpAtomicExchange};
                old = spv_op(&t->b, ops[a], U, (uint32_t[]){ptr, cu(t, 1), cu(t, 0), data}, 4);
            }
            if (result[k]) st(t, result[k], old);
        }
        end_if(t, merge);
        for (uint32_t k = 0; k < n; k++)
            if (result[k]) set_v(t, dst + k, ld(t, U, result[k]));
        return;
    }
    bool append = !strcmp(in->name, "ds_append"), consume = !strcmp(in->name, "ds_consume");
    if (site < 0 || (!append && !consume)) {
        fail(t, "GDS instruction not supported");
        return;
    }
    uint32_t var = t->site_var[site], dst = (in->dst - RDNA2_OP_VGPR) & 0xFF;
    uint32_t res = spv_local_var(&t->b, t->p_fn_u32);
    st(t, res, cu(t, 0));
    uint32_t len = buffer_dwords(t,var);
    len=ext2(t,U,GLSLstd450UMin,len,cu(t,SH_GDS_SIZE/4));
    uint32_t addr=op2(t,SpvOpIAdd,U,op2(t,SpvOpShiftRightLogical,U,get_s(t,RDNA2_OP_M0),cu(t,16)),
                      cu(t,(uint32_t)in->offset&0xffff));
    uint32_t dw=op2(t,SpvOpShiftRightLogical,U,addr,cu(t,2));
    uint32_t valid=op2(t,SpvOpLogicalAnd,t->t_bool,op2(t,SpvOpIEqual,t->t_bool,
                      op2(t,SpvOpBitwiseAnd,U,addr,cu(t,3)),cu(t,0)),op2(t,SpvOpULessThan,t->t_bool,dw,len));
    if(t->full_wave32 || t->full_wave64) {
        uint32_t lo=get_s(t,RDNA2_OP_EXEC_LO),hi=t->full_wave64?get_s(t,RDNA2_OP_EXEC_HI):cu(t,0);
        uint32_t count=op2(t,SpvOpIAdd,U,op1(t,SpvOpBitCount,U,lo),op1(t,SpvOpBitCount,U,hi));
        uint32_t first=sel(t,U,op2(t,SpvOpINotEqual,t->t_bool,lo,cu(t,0)),
            ext1(t,U,GLSLstd450FindILsb,lo),op2(t,SpvOpIAdd,U,cu(t,32),ext1(t,U,GLSLstd450FindILsb,hi)));
        uint32_t lane=t->full_wave32?ld(t,U,t->in_sgid):wave_lane(t);
        uint32_t leader=op2(t,SpvOpLogicalAnd,t->t_bool,op2(t,SpvOpLogicalAnd,t->t_bool,active(t),valid),
            op2(t,SpvOpIEqual,t->t_bool,lane,first));
        uint32_t join=begin_if(t,leader);
        uint32_t old=spv_op(&t->b,append?SpvOpAtomicIAdd:SpvOpAtomicISub,U,
            (uint32_t[]){sb_ptr(t,var,dw),cu(t,1),cu(t,0),count},4);
        st(t,res,old);end_if(t,join);
        old=ld(t,U,res);
        old=t->full_wave64?wave_readlane(t,old,first):spv_op(&t->b,SpvOpGroupNonUniformShuffle,U,
            (uint32_t[]){cu(t,SpvScopeSubgroup),old,first},3);
        set_v(t,dst,old);return;
    }
    uint32_t merge = begin_if(t,op2(t,SpvOpLogicalAnd,t->t_bool,active(t),valid));
    uint32_t old = spv_op(&t->b, append ? SpvOpAtomicIAdd : SpvOpAtomicISub, U,
                          (uint32_t[]){sb_ptr(t, var, dw), cu(t, 1), cu(t, 0), cu(t, 1)}, 4);
    st(t, res, consume ? op2(t, SpvOpISub, U, old, cu(t, 1)) : old);
    end_if(t, merge);
    set_v(t, dst, ld(t, U, res));
}

static void ds_op(tr *t, const rdna2_inst *in, int site) {
    if (t->full_wave64 && sh_inst_uses_lds(in)) wave_barrier(t);
    uint32_t U = t->t_u32;
    if (in->gds) {
        gds_op(t, in, site);
        return;
    }
    if (!strcmp(in->name, "ds_bpermute_b32")) {
        /* each lane reads data0 of the lane (addr + offset) / 4 of its wave,
         * through the LDS crossbar without touching LDS memory */
        if (!t->in_sgid) {
            fail(t, "lane permute outside a subgroup");
            return;
        }
        uint32_t addr = op2(t, SpvOpIAdd, U, get_v(t, in->vaddr), cu(t, (uint32_t)in->offset & 0xFFFF));
        uint32_t idx = op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpShiftRightLogical, U, addr, cu(t, 2)), cu(t, t->p->wave32 ? 31 : 63));
        uint32_t v = t->full_wave64?wave_readlane(t,get_v(t,in->vdata),idx):
            spv_op(&t->b, SpvOpGroupNonUniformShuffle, U, (uint32_t[]){cu(t, SpvScopeSubgroup), get_v(t, in->vdata), idx}, 3);
        set_v(t, (in->dst - RDNA2_OP_VGPR) & 0xFF, v);
        return;
    }
    if (in->op == 0x14) return; /* ds_nop needs no storage */
    if (!t->lds && in->op != 0x35 && !(t->spill_count && (in->op == 0xB0 || in->op == 0xB1))) {
        fail(t, "LDS outside compute shaders");
        return;
    }
    uint32_t hw = in->op, base = get_v(t, in->vaddr), dst = (in->dst - RDNA2_OP_VGPR) & 0xFF;
    uint32_t off = (uint32_t)in->offset & 0xFFFF, off0 = off & 0xFF, off1 = off >> 8;
    uint32_t d0 = in->vdata, d1 = in->src[1];
    if (hw == 0x35) { /* ds_swizzle_b32: a lane permute, no LDS memory */
        if (!t->in_sgid) {
            fail(t, "swizzle outside a subgroup");
            return;
        }
        uint32_t lane = t->full_wave64 ? wave_lane(t) : ld(t, U, t->in_sgid), idx;
        if (off >= 0xe000) { /* FFT decomposition within each group of 32 */
            uint32_t mask = off & 31, bits = 0;
            for (uint32_t m = mask; m; m >>= 1) bits += m & 1;
            uint32_t j = op2(t, SpvOpShiftRightLogical, U,
                op1(t, SpvOpBitReverse, U, op2(t, SpvOpBitwiseAnd, U, lane, cu(t, 31))), cu(t, 27));
            j = op2(t, SpvOpShiftRightLogical, U, j, cu(t, bits));
            j = op2(t, SpvOpBitwiseOr, U, j, op2(t, SpvOpBitwiseAnd, U, lane, cu(t, mask)));
            idx = op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, lane, cu(t, ~31u)), j);
        } else if (off >= 0xc000) { /* masked rotate; bit 10 selects right */
            uint32_t mask = off & 31, amount = (off >> 5) & 31;
            uint32_t j = op2(t, (off & 0x400) ? SpvOpISub : SpvOpIAdd, U, lane, cu(t, amount));
            j = op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, lane, cu(t, mask)),
                op2(t, SpvOpBitwiseAnd, U, j, cu(t, 31 & ~mask)));
            idx = op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, lane, cu(t, ~31u)), j);
        } else if (off & 0x8000) { /* quad mode: two bits per lane of the quad pick the source lane */
            uint32_t sh = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, lane, cu(t, 3)), cu(t, 1));
            uint32_t pick = op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpShiftRightLogical, U, cu(t, off & 0xFF), sh), cu(t, 3));
            idx = op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, lane, cu(t, ~3u)), pick);
        } else { /* bit masks on the lane within 32: ((lane & and) | or) ^ xor */
            uint32_t l = op2(t, SpvOpBitwiseAnd, U, lane, cu(t, 31));
            l = op2(t, SpvOpBitwiseXor, U, op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, l, cu(t, off & 0x1F)), cu(t, (off >> 5) & 0x1F)),
                    cu(t, (off >> 10) & 0x1F));
            idx = op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, lane, cu(t, ~31u)), l);
        }
        /* ADDR is the value operand, not DATA0. Both reads and writes obey
         * EXEC; keep paired-wave exchanges unconditional, including EXEC-off lanes. */
        uint32_t value = sel(t, U, active(t), base, cu(t, 0));
        value = t->full_wave64 ? wave_readlane(t, value, idx) :
            spv_op(&t->b, SpvOpGroupNonUniformShuffle, U, (uint32_t[]){cu(t, SpvScopeSubgroup), value, idx}, 3);
        set_v(t, dst, sel(t, U, lane_active(t, idx), value, cu(t, 0)));
        return;
    }
    if ((hw == 0xB0 || hw == 0xB1) && !t->lds) { /* M0 is the same at the write and the read */
        uint32_t var = t->spill ? t->spill[off] : 0;
        if (!var) {
            fail(t, "LDS spill offset was not declared");
            return;
        }
        if (hw == 0xB1) {
            set_v(t, dst, ld(t, U, var));
            return;
        }
        uint32_t merge = begin_if(t, active(t));
        st(t, var, get_v(t, d0));
        end_if(t, merge);
        return;
    }
    if (hw == 0xB0 || hw == 0xB1) { /* ds_write/read_addtid_b32: offset + M0[15:0] + lane * 4 */
        /* the lane in its wave, from the flat local invocation index */
        uint32_t lid = ld(t, t->t_v3u, t->in_lid), ls[3], sx = t->info->local_size[0], sy = t->info->local_size[1];
        for (uint32_t k = 0; k < 3; k++) ls[k] = spv_op(&t->b, SpvOpCompositeExtract, U, (uint32_t[]){lid, k}, 2);
        uint32_t flat = op2(t, SpvOpIAdd, U, ls[0], op2(t, SpvOpIAdd, U, op2(t, SpvOpIMul, U, ls[1], cu(t, sx)),
                                                     op2(t, SpvOpIMul, U, ls[2], cu(t, sx * sy))));
        uint32_t lane = op2(t, SpvOpBitwiseAnd, U, flat, cu(t, t->p->wave32 ? 31 : 63));
        uint32_t addr = op2(t, SpvOpIAdd, U, op2(t, SpvOpBitwiseAnd, U, get_s(t, RDNA2_OP_M0), cu(t, 0xFFFF)),
                            op2(t, SpvOpIAdd, U, cu(t, off), op2(t, SpvOpShiftLeftLogical, U, lane, cu(t, 2))));
        if (hw == 0xB1) {
            set_v(t, dst, lds_load(t, addr));
            return;
        }
        uint32_t merge = begin_if(t, active(t));
        lds_store(t, addr, get_v(t, d0));
        end_if(t, merge);
        return;
    }
    if (hw == 0x14) return; /* ds_nop */
    if (hw >= 0xA2 && hw <= 0xA7) { /* read_u8 / i8 / u16 _d16 (_hi): into one half, the other kept */
        uint32_t addr = lds_addr(t, base, off);
        uint32_t sh = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, addr, cu(t, 3)), cu(t, 3));
        uint32_t w = op2(t, SpvOpShiftRightLogical, U, lds_load(t, addr), sh), bits = hw <= 0xA5 ? 8 : 16;
        uint32_t v = hw == 0xA4 || hw == 0xA5 ? bfe_i(t, w, cu(t, 0), cu(t, bits)) : bfe_u(t, w, cu(t, 0), cu(t, bits));
        v = op2(t, SpvOpBitwiseAnd, U, v, cu(t, 0xFFFF));
        uint32_t old = get_v(t, dst);
        bool hi = hw & 1;
        set_v(t, dst, hi ? op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, old, cu(t, 0xFFFF)), op2(t, SpvOpShiftLeftLogical, U, v, cu(t, 16)))
                         : op2(t, SpvOpBitwiseOr, U, op2(t, SpvOpBitwiseAnd, U, old, cu(t, 0xFFFF0000u)), v));
        return;
    }
    if (hw == 0xB2) { /* ds_permute_b32: lane i's data goes to lane (addr_i + offset) / 4; each lane looks for its sender */
        if (!t->in_sgid) {
            fail(t, "lane permute outside a subgroup");
            return;
        }
        uint32_t B = t->t_bool, mask = t->p->wave32 ? 31 : 63, lanes = mask + 1;
        uint32_t me = ld(t, U, t->in_sgid);
        uint32_t target = op2(t, SpvOpBitwiseAnd, U, op2(t, SpvOpShiftRightLogical, U, op2(t, SpvOpIAdd, U, base, cu(t, off)), cu(t, 2)), cu(t, mask));
        uint32_t data = get_v(t, d0), res = spv_local_var(&t->b, t->p_fn_u32), k = spv_local_var(&t->b, t->p_fn_u32);
        st(t, res, cu(t, 0));
        st(t, k, cu(t, 0));
        uint32_t header = spv_id(&t->b), body = spv_id(&t->b), cont = spv_id(&t->b), merge = spv_id(&t->b);
        branch(t, header);
        spv_label(&t->b, header);
        uint32_t loop_cond = op2(t, SpvOpULessThan, B, ld(t, U, k), cu(t, lanes)); /* before the merge: nothing may come between it and the branch */
        spv_op_void(&t->b, SpvOpLoopMerge, (uint32_t[]){merge, cont, 0}, 3);
        spv_op_void(&t->b, SpvOpBranchConditional, (uint32_t[]){loop_cond, body, merge}, 3);
        spv_label(&t->b, body);
        uint32_t kv = ld(t, U, k);
        uint32_t tk = spv_op(&t->b, SpvOpGroupNonUniformShuffle, U, (uint32_t[]){cu(t, SpvScopeSubgroup), target, kv}, 3);
        uint32_t dk = spv_op(&t->b, SpvOpGroupNonUniformShuffle, U, (uint32_t[]){cu(t, SpvScopeSubgroup), data, kv}, 3);
        uint32_t ak = spv_op(&t->b, SpvOpGroupNonUniformShuffle, U, (uint32_t[]){cu(t, SpvScopeSubgroup), b2u(t, active(t)), kv}, 3);
        uint32_t hit = op2(t, SpvOpLogicalAnd, B, op2(t, SpvOpIEqual, B, tk, me), op2(t, SpvOpINotEqual, B, ak, cu(t, 0)));
        st(t, res, sel(t, U, hit, dk, ld(t, U, res)));
        branch(t, cont);
        spv_label(&t->b, cont);
        st(t, k, op2(t, SpvOpIAdd, U, kv, cu(t, 1)));
        branch(t, header);
        spv_label(&t->b, merge);
        set_v(t, dst, ld(t, U, res));
        return;
    }
    if (hw == 0x3D || hw == 0x3E) { /* ds_consume / ds_append on LDS: a counter at M0[15:0] + offset, one per lane */
        uint32_t addr = op2(t, SpvOpIAdd, U, op2(t, SpvOpBitwiseAnd, U, get_s(t, RDNA2_OP_M0), cu(t, 0xFFFF)), cu(t, off));
        uint32_t res = spv_local_var(&t->b, t->p_fn_u32);
        st(t, res, cu(t, 0));
        uint32_t merge = begin_if(t, active(t));
        uint32_t old = spv_op(&t->b, hw == 0x3E ? SpvOpAtomicIAdd : SpvOpAtomicISub, U,
                              (uint32_t[]){lds_ptr(t, addr), cu(t, 2), cu(t, 0), cu(t, 1)}, 4);
        st(t, res, hw == 0x3D ? op2(t, SpvOpISub, U, old, cu(t, 1)) : old);
        end_if(t, merge);
        set_v(t, dst, ld(t, U, res));
        return;
    }
    /* reads: set_v keeps inactive lanes */
    uint32_t nread = hw == 0x36 ? 1 : hw == 0x76 ? 2 : hw == 0xFE ? 3 : hw == 0xFF ? 4 : 0;
    if (nread) {
        uint32_t v[4];
        for (uint32_t k = 0; k < nread; k++) v[k] = lds_load(t, lds_addr(t, base, off + 4 * k));
        for (uint32_t k = 0; k < nread; k++) set_v(t, dst + k, v[k]);
        return;
    }
    if (hw == 0x37 || hw == 0x38 || hw == 0x77 || hw == 0x78) { /* read2 (st64): two elements at offset0/offset1 */
        uint32_t unit = hw >= 0x77 ? 8 : 4, scale = hw == 0x38 || hw == 0x78 ? 64 : 1, words = hw >= 0x77 ? 2 : 1, v[4];
        for (uint32_t e = 0; e < 2; e++)
            for (uint32_t k = 0; k < words; k++)
                v[e * words + k] = lds_load(t, lds_addr(t, base, (e ? off1 : off0) * unit * scale + 4 * k));
        for (uint32_t k = 0; k < 2 * words; k++) set_v(t, dst + k, v[k]);
        return;
    }
    if (hw >= 0x39 && hw <= 0x3C) { /* read_i8 / u8 / i16 / u16 */
        uint32_t addr = lds_addr(t, base, off);
        uint32_t sh = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, addr, cu(t, 3)), cu(t, 3));
        uint32_t w = op2(t, SpvOpShiftRightLogical, U, lds_load(t, addr), sh), bits = hw <= 0x3A ? 8 : 16;
        set_v(t, dst, hw == 0x39 || hw == 0x3B ? bfe_i(t, w, cu(t, 0), cu(t, bits)) : bfe_u(t, w, cu(t, 0), cu(t, bits)));
        return;
    }
    /* everything else writes memory: only active lanes */
    uint32_t merge = begin_if(t, active(t));
    uint32_t nwrite = hw == 0x0D ? 1 : hw == 0x4D ? 2 : hw == 0xDE ? 3 : hw == 0xDF ? 4 : 0;
    if (nwrite) {
        for (uint32_t k = 0; k < nwrite; k++) lds_store(t, lds_addr(t, base, off + 4 * k), get_v(t, d0 + k));
    } else if (hw == 0x0E || hw == 0x0F || hw == 0x4E || hw == 0x4F) { /* write2 (st64) */
        uint32_t unit = hw >= 0x4E ? 8 : 4, scale = hw == 0x0F || hw == 0x4F ? 64 : 1, words = hw >= 0x4E ? 2 : 1;
        for (uint32_t e = 0; e < 2; e++)
            for (uint32_t k = 0; k < words; k++)
                lds_store(t, lds_addr(t, base, (e ? off1 : off0) * unit * scale + 4 * k), get_v(t, (e ? d1 : d0) + k));
    } else if (hw == 0x1E || hw == 0x1F || hw == 0xA0 || hw == 0xA1) {
        /* write_b8 / b16 (d16_hi: the data's high half): clear the field, then set it */
        uint32_t addr = lds_addr(t, base, off), ptr = lds_ptr(t, addr), fmask = hw == 0x1E || hw == 0xA0 ? 0xFF : 0xFFFF;
        uint32_t sh = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, addr, cu(t, 3)), cu(t, 3));
        uint32_t m = op2(t, SpvOpShiftLeftLogical, U, cu(t, fmask), sh), data = get_v(t, d0);
        if (hw >= 0xA0) data = op2(t, SpvOpShiftRightLogical, U, data, cu(t, 16));
        uint32_t v = op2(t, SpvOpShiftLeftLogical, U, op2(t, SpvOpBitwiseAnd, U, data, cu(t, fmask)), sh);
        if(t->lds_private) {
            /* Private LDS belongs to one vertex invocation. Atomics on a
             * Private pointer are invalid SPIR-V and can crash the host
             * pipeline compiler. Preserve the untouched bytes locally. */
            uint32_t kept=op2(t,SpvOpBitwiseAnd,U,ld(t,U,ptr),op1(t,SpvOpNot,U,m));
            st(t,ptr,op2(t,SpvOpBitwiseOr,U,kept,v));
        } else {
            spv_op(&t->b, SpvOpAtomicAnd, U, (uint32_t[]){ptr, cu(t, 2), cu(t, 0), op1(t, SpvOpNot, U, m)}, 4);
            spv_op(&t->b, SpvOpAtomicOr, U, (uint32_t[]){ptr, cu(t, 2), cu(t, 0), v}, 4);
        }
    } else if(hw==0x4a) { /* ds_or_b64: one atomic on the shared 64-bit value */
        if(!t->lds64) fail(t,"64-bit LDS atomics require sharedInt64Atomics and explicit workgroup layout");
        else {
            uint32_t addr=lds_addr(t,base,off);
            uint32_t aligned=op2(t,SpvOpIEqual,t->t_bool,op2(t,SpvOpBitwiseAnd,U,addr,cu(t,7)),cu(t,0));
            uint32_t bounded=op2(t,SpvOpULessThanEqual,t->t_bool,addr,cu(t,t->lds_dwords*4-8));
            uint32_t valid=begin_if(t,op2(t,SpvOpLogicalAnd,t->t_bool,aligned,bounded));
            uint32_t data=op2(t,SpvOpBitwiseOr,t->t_u64,op1(t,SpvOpUConvert,t->t_u64,get_v(t,d0)),
                op2(t,SpvOpShiftLeftLogical,t->t_u64,op1(t,SpvOpUConvert,t->t_u64,get_v(t,d0+1)),cu(t,32)));
            spv_op(&t->b,SpvOpAtomicOr,t->t_u64,(uint32_t[]){lds_ptr64(t,addr),cu(t,2),cu(t,0),data},4);
            end_if(t,valid);
        }
    } else if (hw == 0x40 || hw == 0x41) { /* add / sub_u64 on the dword pair (not atomic as a whole) */
        uint32_t L = t->t_u64, a0 = lds_addr(t, base, off), a1 = lds_addr(t, base, off + 4);
        uint32_t old = op2(t, SpvOpBitwiseOr, L, op1(t, SpvOpUConvert, L, lds_load(t, a0)),
                           op2(t, SpvOpShiftLeftLogical, L, op1(t, SpvOpUConvert, L, lds_load(t, a1)), cu(t, 32)));
        uint32_t v = op2(t, SpvOpBitwiseOr, L, op1(t, SpvOpUConvert, L, get_v(t, d0)),
                         op2(t, SpvOpShiftLeftLogical, L, op1(t, SpvOpUConvert, L, get_v(t, d0 + 1)), cu(t, 32)));
        uint32_t r = op2(t, hw == 0x40 ? SpvOpIAdd : SpvOpISub, L, old, v);
        lds_store(t, a0, op1(t, SpvOpUConvert, U, r));
        lds_store(t, a1, op1(t, SpvOpUConvert, U, op2(t, SpvOpShiftRightLogical, L, r, cu(t, 32))));
    } else if (hw == 0x15 || hw == 0x55 || hw == 0x34) { /* add_f32 (_rtn); wrap_rtn_b32 */
        uint32_t ptr = lds_ptr(t, lds_addr(t, base, off));
        uint32_t old = hw == 0x34 ? atomic_loop(t, ptr, 2, RMW_WRAP, get_v(t, d0), get_v(t, d1))
                                  : atomic_loop(t, ptr, 2, RMW_FADD, get_v(t, d0), 0);
        if (hw != 0x15) set_v(t, dst, old);
    } else {
        /* atomics; the _rtn forms (opcode + 0x20) return the old value */
        uint32_t kind = hw >= 0x20 && hw < 0x40 ? hw - 0x20 : hw;
        bool rtn = hw >= 0x20 && hw < 0x40;
        static const uint32_t k_atomic[0x0C] = {SpvOpAtomicIAdd, SpvOpAtomicISub, 0, 0, 0, SpvOpAtomicSMin,
                                                SpvOpAtomicSMax, SpvOpAtomicUMin, SpvOpAtomicUMax, SpvOpAtomicAnd,
                                                SpvOpAtomicOr, SpvOpAtomicXor};
        uint32_t ptr = lds_ptr(t, lds_addr(t, base, off)), old = 0;
        if (kind < 0x0C && k_atomic[kind]) {
            old = spv_op(&t->b, k_atomic[kind], U, (uint32_t[]){ptr, cu(t, 2), cu(t, 0), get_v(t, d0)}, 4);
        } else if (kind == 0x0D && rtn) { /* ds_wrxchg_rtn_b32 */
            old = spv_op(&t->b, SpvOpAtomicExchange, U, (uint32_t[]){ptr, cu(t, 2), cu(t, 0), get_v(t, d0)}, 4);
        } else if (kind == 0x12 || kind == 0x13) { /* ds_min_f32 / ds_max_f32: compare-exchange loop */
            uint32_t B = t->t_bool, F = t->t_f32, data = fbits(t, get_v(t, d0));
            uint32_t header = spv_id(&t->b), body = spv_id(&t->b), cont = spv_id(&t->b), merge2 = spv_id(&t->b);
            uint32_t cur = spv_local_var(&t->b, t->p_fn_u32), done = spv_local_var(&t->b, t->p_fn_bool);
            st(t, cur, ld(t, U, ptr));
            st(t, done, spv_const_bool(&t->b, false));
            branch(t, header);
            spv_label(&t->b, header);
            uint32_t loop_cond = ld(t, B, done); /* before the merge: nothing may come between it and the branch */
            spv_op_void(&t->b, SpvOpLoopMerge, (uint32_t[]){merge2, cont, 0}, 3);
            spv_op_void(&t->b, SpvOpBranchConditional, (uint32_t[]){loop_cond, merge2, body}, 3);
            spv_label(&t->b, body);
            uint32_t c = ld(t, U, cur);
            uint32_t want = ubits(t, ext2(t, F, kind == 0x12 ? GLSLstd450NMin : GLSLstd450NMax, fbits(t, c), data));
            uint32_t got = spv_op(&t->b, SpvOpAtomicCompareExchange, U, (uint32_t[]){ptr, cu(t, 2), cu(t, 0), cu(t, 0), want, c}, 6);
            st(t, done, op2(t, SpvOpIEqual, B, got, c));
            st(t, cur, got);
            branch(t, cont);
            spv_label(&t->b, cont);
            branch(t, header);
            spv_label(&t->b, merge2);
            old = ld(t, U, cur);
        } else if (kind == 0x02 || kind == 0x03 || kind == 0x04 || kind == 0x0C) { /* rsub, inc, dec, mskor */
            int rmw = kind == 0x02 ? RMW_RSUB : kind == 0x03 ? RMW_INC : kind == 0x04 ? RMW_DEC : RMW_MSKOR;
            old = atomic_loop(t, ptr, 2, rmw, get_v(t, d0), kind == 0x0C ? get_v(t, d1) : 0);
        } else if (kind == 0x10) { /* ds_cmpst: (mem == data0) ? data1 : mem */
            old = spv_op(&t->b, SpvOpAtomicCompareExchange, U,
                         (uint32_t[]){ptr, cu(t, 2), cu(t, 0), cu(t, 0), get_v(t, d1), get_v(t, d0)}, 6);
        } else {
            fail(t, "LDS instruction not supported");
        }
        if (rtn && old) set_v(t, dst, old);
    }
    end_if(t, merge);
}

static bool is_branch_op(uint32_t op) {
    return op == SH_OP_S_BRANCH || op == SH_OP_S_CBRANCH_SCC0 || op == SH_OP_S_CBRANCH_SCC1 ||
           op == SH_OP_S_CBRANCH_VCCZ || op == SH_OP_S_CBRANCH_VCCNZ || op == SH_OP_S_CBRANCH_EXECZ ||
           op == SH_OP_S_CBRANCH_EXECNZ;
}

static bool is_nop_op(uint32_t op) {
    return op == SH_OP_S_NOP || op == SH_OP_S_WAITCNT || op == SH_OP_S_WAITCNT_VSCNT || op == SH_OP_S_WAITCNT_VMCNT ||
           op == SH_OP_S_WAITCNT_LGKMCNT || op == SH_OP_S_WAITCNT_EXPCNT || op == SH_OP_S_WAITCNT_DEPCTR ||
           op == SH_OP_S_BARRIER || op == SH_OP_S_SENDMSG || op == SH_OP_S_INST_PREFETCH || op == SH_OP_S_CLAUSE ||
           op == SH_OP_S_SETPRIO || op == SH_OP_S_SLEEP || op == SH_OP_S_TRAP || op == SH_OP_S_ICACHE_INV || op == SH_OP_S_DENORM_MODE ||
           op == SH_OP_S_ROUND_MODE || op == SH_OP_S_TTRACEDATA || op == SH_OP_S_DCACHE_INV || op == SH_OP_S_GL1_INV ||
           op == SH_OP_BUFFER_GL0_INV || op == SH_OP_BUFFER_GL1_INV || op == SH_OP_V_NOP;
}

static void translate_inst_body(tr *t, uint32_t i) {
    const sh_program *p = t->p;
    const rdna2_inst *in = &p->insts[i];
    uint32_t op = p->ops[i];
    int site = p->site_of[i];
    t->inst = i;
    for (uint32_t k = 0; t->probe_var && !t->ps_capture && !t->nan_trace && i && k < t->probe_count; k++)
        if (p->pc[i - 1] * 4 == t->probe_pcs[k]) {
            uint32_t reg=t->probe_vgprs[k];
            st(t,t->probe_vars[k],reg>=256?get_s(t,reg-256):get_v(t,reg));
        }
    if(t->info->stage==SH_STAGE_MESH && op==SH_OP_S_SENDMSG) {
        if((in->simm16&0xff)==9)mesh_allocate(t);else fail(t,"unsupported NGG message");
        return;
    }
    if (op == SH_OP_S_BARRIER && t->lds && (t->translation_flags & SH_TRANSLATE_NO_BARRIER)) return;
    if (op == SH_OP_S_BARRIER && t->lds_private) return; /* nothing shared to wait for */
    if (op == SH_OP_S_BARRIER && t->lds) {
        /* The PC executor parks this guest wave at the block boundary.
         * All host invocations rendezvous outside the divergent switch. */
        if(t->wg_states && !t->barrier_safe[i])return;
        /* workgroup execution and LDS memory barrier; where lanes may be at
         * different blocks only the memory part (an execution barrier there
         * deadlocks the host GPU) */
        if (t->barrier_exec || t->barrier_safe[i]) spv_op_void(&t->b, SpvOpControlBarrier, (uint32_t[]){cu(t, 2), cu(t, 2), cu(t, 0x108)}, 3);
        else spv_op_void(&t->b, SpvOpMemoryBarrier, (uint32_t[]){cu(t, 2), cu(t, 0x108)}, 2);
        return;
    }
    if ((t->info->stage == SH_STAGE_CS || t->info->stage == SH_STAGE_MESH) && t->lds && !t->lds_private && sh_barrier_lds_wait(p, i)) {
        /* Scope Subgroup synchronizes the active lanes in this dynamic
         * instance, including inside an EXEC skip. A workgroup barrier here
         * would deadlock when another wave skips this LDS phase. */
        if(t->full_wave64)wave_barrier(t);
        else spv_op_void(&t->b, SpvOpControlBarrier, (uint32_t[]){cu(t,3),cu(t,3),cu(t,0x108)}, 3);
        return;
    }
    if (is_nop_op(op) || op == SH_OP_S_ENDPGM || op == SH_OP_S_CODE_END || is_branch_op(op) || op==SH_OP_S_SWAPPC_B64) return;
    /* s_wqm: the lane mask stays the lane's own bit */
    if (op == SH_OP_S_QUADMASK_B32 || op == SH_OP_S_QUADMASK_B64) {
        set_s(t, in->dst, op2(t, SpvOpBitwiseAnd, t->t_u32, src_u(t, in, in->src[0]), cu(t, 1)));
        if (op == SH_OP_S_QUADMASK_B64) set_s(t, in->dst + 1, cu(t, 0));
        set_scc(t, op2(t, SpvOpINotEqual, t->t_bool, get_s(t, in->dst), cu(t, 0)));
        return;
    }
    if (op == SH_OP_S_WQM_B32 || op == SH_OP_S_WQM_B64) {
        if (t->ps_quad_flow) {
            uint32_t own=bit0(t,src_u(t,in,in->src[0])),live=spv_const_bool(&t->b,false);
            for(uint32_t k=0;k<4;k++) {
                uint32_t bit=spv_op(&t->b,SpvOpGroupNonUniformQuadBroadcast,t->t_bool,
                    (uint32_t[]){cu(t,SpvScopeSubgroup),own,cu(t,k)},3);
                live=op2(t,SpvOpLogicalOr,t->t_bool,live,bit);
            }
            set_s(t,in->dst,b2u(t,live));
            if(op==SH_OP_S_WQM_B64)set_s(t,in->dst+1,cu(t,0));
            uint32_t any=spv_op(&t->b,SpvOpGroupNonUniformAny,t->t_bool,
                (uint32_t[]){cu(t,SpvScopeSubgroup),live},2);
            set_scc(t,any);
            return;
        }
        set_s(t, in->dst, src_u(t, in, in->src[0]));
        if (op == SH_OP_S_WQM_B64) set_s(t, in->dst + 1, src_u(t, in, in->src[0] + 1));
        set_scc(t, op2(t, SpvOpINotEqual, t->t_bool, get_s(t, in->dst), cu(t, 0)));
        return;
    }
    rdna2_inst sdwa_copy;
    t->sdwa = 0;
    if (in->enc == RDNA2_ENC_VOP1 || in->enc == RDNA2_ENC_VOP2 || in->enc == RDNA2_ENC_VOPC) {
        uint32_t d = in->dpp_sdwa;
        if (in->src[0] == RDNA2_OP_DPP16 && dpp_supported((d >> 8) & 0x1FF) && t->in_sgid) {
            sdwa_copy = *in;
            sdwa_copy.src[0] = (uint16_t)((d & 0xFF) + RDNA2_OP_VGPR);
            t->dpp = ((d >> 8) & 0x1FF) | (d & 0xFFFC0000u);
            if (!t->dpp) t->dpp = 0x40000000u; /* quad_perm(0,0,0,0) with every lane masked: keep it non-zero */
            t->dpp_keep = 0;
            in = &sdwa_copy;
        } else if ((in->src[0] == RDNA2_OP_DPP8 || in->src[0] == RDNA2_OP_DPP8FI) && t->in_sgid) {
            sdwa_copy = *in;
            sdwa_copy.src[0] = (uint16_t)((d & 0xFF) + RDNA2_OP_VGPR);
            t->dpp8 = d >> 8;
            t->dpp8_on = true;
            t->dpp8_fi = in->src[0] == RDNA2_OP_DPP8FI;
            in = &sdwa_copy;
        } else if (in->src[0] == RDNA2_OP_DPP16 || in->src[0] == RDNA2_OP_DPP8 ||
                   in->src[0] == RDNA2_OP_DPP8FI) {
            fail(t, "DPP not supported");
            return;
        }
        if (in->src[0] == RDNA2_OP_SDWA) {
            sdwa_copy = *in;
            sdwa_copy.src[0] = (uint16_t)((d & 0xFF) + ((d >> 23) & 1 ? 0 : RDNA2_OP_VGPR));
            if (in->enc != RDNA2_ENC_VOP1 && in->src[1] >= RDNA2_OP_VGPR && (d >> 31))
                sdwa_copy.src[1] = (uint16_t)(in->src[1] - RDNA2_OP_VGPR);
            t->sdwa = in->dpp_sdwa | 0x40000000u; /* bit 30 (unused) keeps the value non-zero */
            in = &sdwa_copy;
        }
    }
    if (op == SH_OP_S_MEMTIME || op == SH_OP_S_MEMREALTIME) {
        /* 64-bit clock counters: no clock is read; each read moves on by 2^16
         * ticks (0.65 ms at the 100 MHz reference clock). A clock that stood
         * still kept "spin until this time" loops going until the GPU gave up
         * (Wolverine: 81 ms per dispatch of one workgroup). */
        uint32_t U = t->t_u32;
        if (!t->clock) {
            t->clock = spv_id(&t->b);
            spv_emit(&t->b, SPV_SEC_GLOBAL, SpvOpVariable,
                     (uint32_t[]){spv_type_pointer(&t->b, SpvStorageClassPrivate, U), t->clock, SpvStorageClassPrivate, cu(t, 0)}, 4);
        }
        uint32_t ptr = t->clock, reads = op2(t, SpvOpIAdd, U, ld(t, U, ptr), cu(t, 1));
        st(t, ptr, reads);
        set_s(t, in->dst, op2(t, SpvOpShiftLeftLogical, U, reads, cu(t, 16)));
        set_s(t, in->dst + 1, op2(t, SpvOpShiftRightLogical, U, reads, cu(t, 16)));
        return;
    }
    switch (in->enc) {
    case RDNA2_ENC_SOP1:
    case RDNA2_ENC_SOP2:
    case RDNA2_ENC_SOPK:
    case RDNA2_ENC_SOPC: salu(t, in, op); break;
    case RDNA2_ENC_SMEM: smem(t, in, op, site); break;
    case RDNA2_ENC_VOPC: vcmp(t, in, op == SH_OP_V_CMPX); break;
    case RDNA2_ENC_VOP3:
        if (in->valu < 0x100) vcmp(t, in, op == SH_OP_V_CMPX);
        else valu(t, in, op);
        break;
    case RDNA2_ENC_VOP1:
    case RDNA2_ENC_VOP2:
    case RDNA2_ENC_VOP3P: valu(t, in, op); break;
    case RDNA2_ENC_VINTRP: interp(t, in); break;
    case RDNA2_ENC_MUBUF:
        if (site >= 0 && mubuf_scratch(t->p, t->p->sites[site].inst)) { scratch_buffer_op(t, in); break; }
        buffer_op(t, in, op, site);
        break;
    case RDNA2_ENC_MTBUF: buffer_op(t, in, op, site); break;
    case RDNA2_ENC_MIMG:
        if (op == SH_OP_IMAGE_BVH || op == SH_OP_IMAGE_BVH64) bvh_op(t, in, site);
        else if(t->shapes[site].address_map) {
            /* Finite table plans bind an image only when every valid record
             * agrees. An out-of-range table/pointer load can still produce a
             * null T#, which must not return data from that real image. */
            if(t->info->stage!=SH_STAGE_CS || t->p->sites[site].store) {
                fail(t,"dynamic image guard requires a read-only compute image");break;
            }
            uint32_t meta=sb_load(t,t->u_var,cu(t,SH_U_SITE+(uint32_t)site));
            uint32_t valid=spv_const_bool(&t->b,true);
            for(uint32_t k=0;k<12;k++) {
                uint32_t reg=k<8?in->srsrc+k:in->ssamp+k-8;
                uint32_t expected=sb_load(t,t->u_var,op2(t,SpvOpIAdd,t->t_u32,meta,cu(t,k)));
                valid=op2(t,SpvOpLogicalAnd,t->t_bool,valid,op2(t,SpvOpIEqual,t->t_bool,get_s(t,reg),expected));
            }
            uint32_t n=op==SH_OP_IMAGE_GATHER4?4u:(uint32_t)__builtin_popcount(in->dmask&15);
            if(in->d16)n=(n+1)/2;
            if(in->tfe)n++;
            /* These operations only read an always-valid host binding. Mask
             * their results, preserving overlapping address/result VGPRs. */
            image_op(t,in,op,site);
            for(uint32_t k=0;k<n;k++)set_v(t,in->vdata+k,sel(t,t->t_u32,valid,get_v(t,in->vdata+k),cu(t,0)));
        }
        else image_op(t, in, op, site);
        break;
    case RDNA2_ENC_EXP: export_op(t, in); break;
    case RDNA2_ENC_DS: ds_op(t, in, site); break;
    case RDNA2_ENC_FLAT:
        if (in->seg == 1) scratch_op(t, in, op);
        else if(in->seg==0 && t->flat_apertures)flat_op(t,in,op,site);
        else global_op(t, in, op, site);
        break;
    default: fail(t, "instruction class not supported"); break;
    }
    t->sdwa = 0;
    t->dpp = 0;
    t->dpp_keep = 0;
    t->dpp8_on = false;
}

/* Sidecar values are sampled immediately after the actual instruction.
 * Sampling at the next static PC can falsely record a predecessor skipped
 * by a branch into that block. Ordinary legacy probes are unchanged. */
static void translate_inst(tr *t,uint32_t i) {
    translate_inst_body(t,i);
    if(t->ps_capture && t->capture_require && t->p->pc[i]*4==t->capture_require_pc)
        st(t,t->capture_reached,spv_const_bool(&t->b,true));
    for(uint32_t k=0;t->ps_capture && t->ok && k<t->probe_count;k++)
        if(t->p->pc[i]*4==t->probe_pcs[k]) {
            uint32_t reg=t->probe_vgprs[k];
            st(t,t->probe_vars[k],reg>=256?get_s(t,reg-256):get_v(t,reg));
        }
}

static uint32_t ps_wave_any(tr *t, uint32_t value) {
    return t->ps_quad_flow ? spv_op(&t->b,SpvOpGroupNonUniformAny,t->t_bool,
        (uint32_t[]){cu(t,SpvScopeSubgroup),value},2) : value;
}

static uint32_t branch_cond(tr *t, uint32_t op) {
    uint32_t B = t->t_bool;
    switch (op) {
    case SH_OP_S_CBRANCH_SCC0: return op1(t, SpvOpLogicalNot, B, ps_wave_any(t,get_scc(t)));
    case SH_OP_S_CBRANCH_SCC1: return ps_wave_any(t,get_scc(t));
    case SH_OP_S_CBRANCH_VCCZ: return op1(t, SpvOpLogicalNot, B, ps_wave_any(t,mask_nonzero(t,RDNA2_OP_VCC_LO)));
    case SH_OP_S_CBRANCH_VCCNZ: return ps_wave_any(t,mask_nonzero(t,RDNA2_OP_VCC_LO));
    case SH_OP_S_CBRANCH_EXECZ: case SH_OP_S_CBRANCH_EXECNZ: {
        uint32_t live=(t->full_wave64 || t->full_wave32)?mask_nonzero(t,RDNA2_OP_EXEC_LO):active(t);
        if(t->ps_wave_exec)
            live=spv_op(&t->b,SpvOpGroupNonUniformAny,B,(uint32_t[]){cu(t,SpvScopeSubgroup),live},2);
        return op==SH_OP_S_CBRANCH_EXECZ?op1(t,SpvOpLogicalNot,B,live):live;
    }
    default: return spv_const_bool(&t->b, true);
    }
}

/* ---- Structured control flow ----
 *
 * Programs whose branches nest (forward skips, if/else, loops closed by one
 * backward branch with exits to just past it) become SPIR-V selections and
 * loops, which drivers compile well. Anything else runs as a block dispatch
 * loop (a switch on a program counter). A first pass only checks the shape
 * (emit false); the second emits. Each lane takes the same path as it would
 * in the dispatch loop. */
#define SF_LOOP_LIMIT (1u << 16) /* iterations of all loops: one that never ends must not hang the host GPU */

typedef struct sf_state {
    tr *t;
    bool emit;
    bool skip_execz; /* forward exec skips fall through (barriers in uniform control flow) */
    uint32_t count;  /* loop iterations so far (function variable) */
    const struct sf_loop *top; /* the whole program as a loop run once: its break ends the program */
    bool execz_out;  /* forward exec skips out of their construct fall through */
} sf_state;

typedef struct sf_loop {
    uint32_t exit;  /* instruction after the backward branch */
    uint32_t merge; /* loop merge label */
} sf_loop;

static bool sf_span(sf_state *s, uint32_t a, uint32_t b, const sf_loop *loop, uint32_t alias);
static bool sf_range(sf_state *s, uint32_t a, uint32_t b, const sf_loop *loop) { return sf_span(s, a, b, loop, b); }

/* A fresh block after a branch that ends one (code after it is reached only
 * through the merge that follows). */
static void sf_dead_block(sf_state *s) { spv_label(&s->t->b, spv_id(&s->t->b)); }

/* A waterfall loop [h, j] that one lane per invocation runs exactly once:
 *   h:   v_readfirstlane_b32 sX, vY
 *        v_cmpx_eq_u32 sX, vY          exec: the lanes holding the first lane's value
 *        ... (no branch, no other write to exec or S)
 *        s_andn2_b64 S, S, exec        the lanes still to do
 *        ... (vector ALU only: SCC stays)
 *        s_mov_b64 exec, S
 *   j:   s_cbranch_scc1 / s_cbranch_execnz h
 * entered with S equal to exec (s_mov_b64 S, exec; or exec and S both moved from X).
 * Without a full wave the lane reads its own value, so the compare keeps exec and S & ~exec
 * is 0: the loop ends after its first pass. Preserving many such loops in SPIR-V
 * can take minutes to compile on affected drivers. */
static bool sf_waterfall_once(const tr *t, uint32_t h, uint32_t j) {
    const sh_program *p = t->p;
    if (t->full_wave32 || t->full_wave64 || h < 2 || j < h + 4) return false;
    if (p->ops[j] != SH_OP_S_CBRANCH_SCC1 && p->ops[j] != SH_OP_S_CBRANCH_EXECNZ) return false;
    const rdna2_inst *rf = &p->insts[h], *cmp = &p->insts[h + 1], *mov = &p->insts[j - 1];
    if (p->ops[h] != SH_OP_V_READFIRSTLANE_B32 || p->ops[h + 1] != SH_OP_V_CMPX || cmp->enc != RDNA2_ENC_VOPC ||
        !cmp->name || strncmp(cmp->name, "v_cmpx_eq_u32", 13))
        return false;
    uint32_t sx = (rf->dst - RDNA2_OP_VGPR) & 0x7F, vy = rf->src[0];
    if (vy < RDNA2_OP_VGPR || !((cmp->src[0] == sx && cmp->src[1] == vy) || (cmp->src[0] == vy && cmp->src[1] == sx)))
        return false;
    if (p->ops[j - 1] != SH_OP_S_MOV_B64 || mov->dst != RDNA2_OP_EXEC_LO) return false;
    uint32_t sreg = mov->src[0];
    if (sreg + 1 >= RDNA2_OP_EXEC_LO || (sreg & 1)) return false;
    /* The first-lane read must preserve both halves of the saved EXEC mask. */
    if (sx == sreg || sx == sreg + 1) return false;
    uint32_t andn2 = 0;
    for (uint32_t i = h + 2; i < j - 1; i++) {
        const rdna2_inst *in = &p->insts[i];
        if (is_branch_op(p->ops[i]) || p->ops[i] == SH_OP_S_SWAPPC_B64 || p->ops[i] == SH_OP_S_BARRIER ||
            p->ops[i] == SH_OP_S_ENDPGM || p->ops[i] == SH_OP_S_CODE_END)
            return false;
        if (p->ops[i] == SH_OP_S_ANDN2_B64 && in->dst == sreg && in->src[0] == sreg && in->src[1] == RDNA2_OP_EXEC_LO) {
            if (andn2) return false;
            andn2 = i;
            continue;
        }
        for (uint32_t r = 0; r < 2; r++)
            if (sh_barrier_writes_s(p, i, RDNA2_OP_EXEC_LO + r) || sh_barrier_writes_s(p, i, sreg + r)) return false;
        bool valu = in->enc == RDNA2_ENC_VOP1 || in->enc == RDNA2_ENC_VOP2 || in->enc == RDNA2_ENC_VOP3;
        if (andn2 && p->ops[j] == SH_OP_S_CBRANCH_SCC1 && !valu) return false; /* SCC from the s_andn2 */
    }
    if (!andn2) return false;
    /* entered only by falling through (the back edge aside), S equal to exec */
    for (uint32_t i = 0; i < p->count; i++)
        if (i != j && is_branch_op(p->ops[i]) && (uint32_t)p->target[i] + 2u > h && p->target[i] <= j) return false;
    /* back from h over vector ALU code: the last writes of S and exec are moves of exec, or of one X */
    uint32_t s_src = 0, exec_src = 0;
    bool s_found = false, exec_found = false;
    for (uint32_t k = h; k-- > 0 && k + 8 >= h && !(s_found && exec_found);) {
        const rdna2_inst *in = &p->insts[k];
        if (is_branch_op(p->ops[k])) return false;
        for (uint32_t i = 0; i < p->count; i++)
            if (is_branch_op(p->ops[i]) && p->target[i] == k + 1 && k + 1 < h) return false;
        bool to_s = sh_barrier_writes_s(p, k, sreg) || sh_barrier_writes_s(p, k, sreg + 1);
        bool to_exec = sh_barrier_writes_s(p, k, RDNA2_OP_EXEC_LO) || sh_barrier_writes_s(p, k, RDNA2_OP_EXEC_HI);
        if (to_s || to_exec) {
            if (p->ops[k] != SH_OP_S_MOV_B64 || (to_s && to_exec)) return false;
            if (to_s && !s_found) s_found = true, s_src = in->src[0];
            if (to_exec && !exec_found) exec_found = true, exec_src = in->src[0];
            if (s_found && s_src == RDNA2_OP_EXEC_LO && !exec_found) return true; /* S = exec, exec kept since */
            continue;
        }
        bool valu = in->enc == RDNA2_ENC_VOP1 || in->enc == RDNA2_ENC_VOP2 || in->enc == RDNA2_ENC_VOP3;
        if (!valu) return false;
        /* the moved value must not change between the two moves */
        if ((s_found || exec_found) && (sh_barrier_writes_s(p, k, s_found ? s_src : exec_src) ||
                                        sh_barrier_writes_s(p, k, (s_found ? s_src : exec_src) + 1)))
            return false;
    }
    return s_found && exec_found && s_src == exec_src && s_src + 1 < RDNA2_OP_EXEC_LO && s_src != sreg &&
           s_src + 1 != sreg && s_src != sreg + 1;
}

static bool sf_loop_at(sf_state *s, uint32_t h, uint32_t j, const sf_loop *outer) {
    tr *t = s->t;
    if (sf_waterfall_once(t, h, j)) return sf_range(s, h, j, outer); /* its body once, no loop */
    spv_builder *b = &t->b;
    uint32_t header = 0, body = 0, cont = 0, merge = 0, count = s->count;
    if (s->emit) {
        header = spv_id(b), body = spv_id(b), cont = spv_id(b), merge = spv_id(b);
        branch(t, header);
        spv_label(b, header);
        spv_op_void(b, SpvOpLoopMerge, (uint32_t[]){merge, cont, SpvLoopControlNone}, 3);
        branch(t, body);
        spv_label(b, body);
    }
    sf_loop inner = {j + 1, merge};
    if (!sf_range(s, h, j, &inner)) return false;
    if (s->emit) {
        branch(t, cont);
        spv_label(b, cont);
        uint32_t n = op2(t, SpvOpIAdd, t->t_u32, ld(t, t->t_u32, count), cu(t, 1));
        st(t, count, n);
        uint32_t again = op2(t, SpvOpULessThan, t->t_bool, n, cu(t, SF_LOOP_LIMIT));
        if (t->p->ops[j] != SH_OP_S_BRANCH) again = op2(t, SpvOpLogicalAnd, t->t_bool, again, branch_cond(t, t->p->ops[j]));
        spv_op_void(b, SpvOpBranchConditional, (uint32_t[]){again, header, merge}, 3);
        spv_label(b, merge);
    }
    return true;
}

/* True when only ends (or nothing) follow instruction k: reaching it ends the program. */
static bool sf_ends_from(const sh_program *p, uint32_t k) {
    for (; k < p->count; k++)
        if (p->ops[k] != SH_OP_S_ENDPGM && p->ops[k] != SH_OP_S_CODE_END) return false;
    return true;
}

/* The end of an exit block that starts at k: code after an s_endpgm that only jumps
 * reach (a pixel shader's discard), straight to its own end. 0 when k starts none. */
#define SF_EXIT_BLOCK_MAX 16
static uint32_t sf_exit_block(const sh_program *p, uint32_t k) {
    if (k == 0 || k >= p->count || p->ops[k - 1] != SH_OP_S_ENDPGM) return 0;
    uint32_t e = k;
    while (e < p->count && p->ops[e] != SH_OP_S_ENDPGM && p->ops[e] != SH_OP_S_CODE_END) {
        if (is_branch_op(p->ops[e]) || p->ops[e] == SH_OP_S_SWAPPC_B64 || p->ops[e] == SH_OP_S_BARRIER) return 0;
        e++;
    }
    return e < p->count && e - k <= SF_EXIT_BLOCK_MAX && sf_ends_from(p, e) ? e : 0;
}

/* EXEC=0 suppresses ordinary vector writes, not SALU, mask/carry writes,
 * readlane/writelane or barriers. Fall through an EXECZ skip only when the
 * entire skipped span (also beyond the current nested construct) is inert.
 * Keep the dispatcher when the proof fails, rather than reactivating lanes
 * at a scalar EXEC restore or changing a scalar used at the target. */
static bool sf_execz_inert(const sh_program *p,uint32_t first,uint32_t end) {
    if(first>end || end>p->count)return false;
    for(uint32_t i=first;i<end;i++) {
        const rdna2_inst *in=&p->insts[i];uint32_t op=p->ops[i];
        if(op==SH_OP_S_NOP || op==SH_OP_S_WAITCNT || op==SH_OP_S_WAITCNT_VSCNT ||
           op==SH_OP_S_WAITCNT_VMCNT || op==SH_OP_S_INST_PREFETCH || op==SH_OP_S_CLAUSE || op==SH_OP_V_NOP)continue;
        bool valu=in->enc==RDNA2_ENC_VOP1 || in->enc==RDNA2_ENC_VOP2 || in->enc==RDNA2_ENC_VOP3;
        if(valu && in->dst>=RDNA2_OP_VGPR && !(p->iflags[i]&(SH_IF_FLOW|SH_IF_CARRY)) &&
           in->src[0]!=RDNA2_OP_DPP16 && in->src[0]!=RDNA2_OP_DPP8 && in->src[0]!=RDNA2_OP_DPP8FI &&
           in->src[0]!=RDNA2_OP_LDS_DIRECT && op!=SH_OP_V_PERMLANEX16_B32 && op!=SH_OP_V_PERMLANE16_B32 &&
           !rdna2_valu_vop3b(in->valu) && op!=SH_OP_V_READFIRSTLANE_B32 && op!=SH_OP_V_READLANE_B32 &&
           op!=SH_OP_V_WRITELANE_B32 && op!=SH_OP_V_CMP && op!=SH_OP_V_CMPX)continue;
        if((in->enc==RDNA2_ENC_MUBUF || in->enc==RDNA2_ENC_MTBUF || in->enc==RDNA2_ENC_MIMG) &&
           (op==SH_OP_BUFFER_STORE_DWORD || op==SH_OP_BUFFER_STORE_FORMAT ||
            op==SH_OP_TBUFFER_STORE_FORMAT || op==SH_OP_IMAGE_STORE))continue;
        return false;
    }
    return true;
}

/* `alias`: an instruction where reaching it is the same as reaching b (b is a jump
 * there), so the arms of a switch-like chain may jump straight to the common end. */
static bool sf_span(sf_state *s, uint32_t a, uint32_t b, const sf_loop *loop, uint32_t alias) {
    tr *t = s->t;
    const sh_program *p = t->p;
    uint32_t i = a;
    while (i < b) {
        /* a loop starts here when a later branch of the range jumps back to i */
        uint32_t j = b;
        for (uint32_t k = i; k < b; k++) {
            if (is_branch_op(p->ops[k]) && p->target[k] == i) j = k;
        }
        if (j < b) {
            if (!sf_loop_at(s, i, j, loop)) return false;
            i = j + 1;
            continue;
        }
        uint32_t op = p->ops[i];
        if (op == SH_OP_S_ENDPGM || op == SH_OP_S_CODE_END) {
            bool tail = sf_ends_from(p, i + 1);
            if (loop && loop != s->top) return false; /* an early end in a loop: dispatch loop */
            if (!tail) {
                if (!loop) return false;
                if (s->emit) { /* an early end leaves the program loop */
                    branch(t, loop->merge);
                    sf_dead_block(s);
                }
            }
            i++;
            continue;
        }
        if (!is_branch_op(op)) {
            if (s->emit) translate_inst(t, i);
            i++;
            continue;
        }
        uint32_t target = p->target[i];
        if (target <= i) return false; /* backward, not closing a loop at the range start */
        if ((op == SH_OP_S_CBRANCH_EXECZ && s->skip_execz) || target == i + 1) {
            i++;
            continue;
        }
        if (loop && (target == loop->exit || (loop == s->top && sf_ends_from(p, target)))) { /* break */
            if (s->emit) {
                if (op == SH_OP_S_BRANCH) {
                    branch(t, loop->merge);
                    sf_dead_block(s);
                } else {
                    uint32_t brk = spv_id(&t->b), sm = spv_id(&t->b), cond = branch_cond(t, op); /* before the merge */
                    spv_op_void(&t->b, SpvOpSelectionMerge, (uint32_t[]){sm, SpvSelectionControlNone}, 2);
                    spv_op_void(&t->b, SpvOpBranchConditional, (uint32_t[]){cond, brk, sm}, 3);
                    spv_label(&t->b, brk);
                    branch(t, loop->merge);
                    spv_label(&t->b, sm);
                }
            }
            i++;
            continue;
        }
        uint32_t exit_end = loop && loop == s->top && target > b ? sf_exit_block(p, target) : 0;
        if (exit_end) { /* a jump to the exit block: its code here, then the program ends */
            if (s->emit) {
                uint32_t sm = 0;
                if (op != SH_OP_S_BRANCH) {
                    uint32_t ex = spv_id(&t->b), cond = branch_cond(t, op);
                    sm = spv_id(&t->b);
                    spv_op_void(&t->b, SpvOpSelectionMerge, (uint32_t[]){sm, SpvSelectionControlNone}, 2);
                    spv_op_void(&t->b, SpvOpBranchConditional, (uint32_t[]){cond, ex, sm}, 3);
                    spv_label(&t->b, ex);
                }
                for (uint32_t k = target; k < exit_end; k++) translate_inst(t, k);
                branch(t, loop->merge);
                if (sm) spv_label(&t->b, sm);
                else sf_dead_block(s);
            }
            i++;
            continue;
        }
        if (target == alias) target = b;
        if (op == SH_OP_S_BRANCH && target == b && i + 1 == b) {
            i++;
            continue;
        }
        /* Unreal's terminal masked stores are inert; scalar EXEC/SGPR writes
         * or other unproved effects in that span must still be skipped. */
        if (op == SH_OP_S_CBRANCH_EXECZ && s->execz_out && target > b && sf_execz_inert(p,i+1,target)) {
            i++;
            continue;
        }
        if (op == SH_OP_S_BRANCH || target > b) return false; /* a jump out of the construct */
        /* if (!cond) { i+1 .. target } or, when that part ends jumping further on,
         * if (!cond) { i+1 .. target-1 } else { target .. end } */
        uint32_t last = target - 1, end = 0;
        if (last > i && p->ops[last] == SH_OP_S_BRANCH && !(loop && p->target[last] == loop->exit)) {
            uint32_t to = p->target[last] == alias ? b : p->target[last];
            if (to > target && to <= b) end = to;
        } else if (last > i && p->ops[last] == SH_OP_S_ENDPGM && (!loop || loop == s->top) && b == p->count) {
            end = b; /* if (!cond) { ...; s_endpgm } else { the rest of the program } */
        }
        uint32_t m = 0, then_l = 0, else_l = 0;
        if (s->emit) {
            m = spv_id(&t->b), then_l = spv_id(&t->b), else_l = end ? spv_id(&t->b) : m;
            uint32_t cond = branch_cond(t, op); /* its loads go before the merge instruction */
            spv_op_void(&t->b, SpvOpSelectionMerge, (uint32_t[]){m, SpvSelectionControlNone}, 2);
            spv_op_void(&t->b, SpvOpBranchConditional, (uint32_t[]){cond, else_l, then_l}, 3);
            spv_label(&t->b, then_l);
        }
        uint32_t then_alias = !end ? (target == b ? alias : target) : p->ops[last] == SH_OP_S_BRANCH ? p->target[last] : last;
        if (!sf_span(s, i + 1, end ? last : target, loop, then_alias)) return false;
        if (s->emit) branch(t, m);
        if (end) {
            if (s->emit) spv_label(&t->b, else_l);
            if (!sf_span(s, target, end, loop, end == b ? alias : end)) return false;
            if (s->emit) branch(t, m);
        }
        if (s->emit) spv_label(&t->b, m);
        i = end ? end : target;
    }
    return true;
}

/* ---- Declarations ---- */

static uint32_t input_var(tr *t, uint32_t type, uint32_t builtin) {
    uint32_t ptr = spv_type_pointer(&t->b, SpvStorageClassInput, type);
    uint32_t v = spv_id(&t->b);
    uint32_t a[3] = {ptr, v, SpvStorageClassInput};
    spv_emit(&t->b, SPV_SEC_GLOBAL, SpvOpVariable, a, 3);
    spv_decorate(&t->b, v, SpvDecorationBuiltIn, &builtin, 1);
    t->iface[t->n_iface++] = v;
    return v;
}

static void declare_resources(tr *t) {
    spv_builder *b = &t->b;
    bool image64_declared=false;
    uint32_t rt = spv_type_runtime_array(b, t->t_u32);
    uint32_t four = 4, zero = 0, set = 0;
    spv_decorate(b, rt, SpvDecorationArrayStride, &four, 1);
    t->t_ssbo = spv_id(b);
    spv_emit(b, SPV_SEC_GLOBAL, SpvOpTypeStruct, (uint32_t[]){t->t_ssbo, rt}, 2);
    spv_decorate(b, t->t_ssbo, SpvDecorationBlock, NULL, 0);
    spv_member_decorate(b, t->t_ssbo, 0, SpvDecorationOffset, &zero, 1);
    uint32_t p_ssbo = spv_type_pointer(b, SpvStorageClassStorageBuffer, t->t_ssbo);
    t->p_sb_u32 = spv_type_pointer(b, SpvStorageClassStorageBuffer, t->t_u32);

    uint32_t binding = t->binding_base;
    t->u_var = spv_id(b);
    spv_emit(b, SPV_SEC_GLOBAL, SpvOpVariable, (uint32_t[]){p_ssbo, t->u_var, SpvStorageClassStorageBuffer}, 3);
    spv_decorate(b, t->u_var, SpvDecorationDescriptorSet, &set, 1);
    spv_decorate(b, t->u_var, SpvDecorationBinding, &binding, 1);
    const char *vprobe = t->info->stage != SH_STAGE_PS ? getenv("PS5C_VS_PROBE") : NULL; /* written by the probe */
    while (vprobe && strtoull(vprobe, NULL, 16) != t->p->hash && strchr(vprobe, ';')) vprobe = strchr(vprobe, ';') + 1;
    const char *pcapture=t->info->stage==SH_STAGE_PS?getenv("PS5C_PS_CAPTURE"):NULL;
    bool captures=pcapture && strtoull(pcapture,NULL,16)==t->p->hash;
    if ((!vprobe || strtoull(vprobe, NULL, 16) != t->p->hash) && !captures)
        spv_decorate(b, t->u_var, SpvDecorationNonWritable, NULL, 0);
    binding++;

    for (uint32_t s = 0; s < t->p->site_count; s++) {
        sh_site_kind k = t->p->sites[s].kind;
        if (k == SH_SITE_SMEM && !t->shapes[s].address_map) continue;
        uint32_t v = spv_id(b);
        if (k == SH_SITE_BUFFER || k == SH_SITE_SBUFFER || k == SH_SITE_BVH || k == SH_SITE_SMEM) {
            spv_emit(b, SPV_SEC_GLOBAL, SpvOpVariable, (uint32_t[]){p_ssbo, v, SpvStorageClassStorageBuffer}, 3);
            if (!t->p->sites[s].store && !(k == SH_SITE_BVH && (t->shapes[s].address_map == 3 || t->shapes[s].address_map == 4)))
                spv_decorate(b, v, SpvDecorationNonWritable, NULL, 0);
        } else {
            uint32_t kind = t->shapes[s].img_kind;
            bool storage = sh_site_storage(&t->p->sites[s],&t->shapes[s]); /* storage writes and coherent POPS reads */
            bool packed = storage && t->shapes[s].buf_format == SH_IMG_PACKED_R11G11B10; /* through R32_UINT */
            bool float_atomic=t->shapes[s].buf_format == SH_IMG_ATOMIC_FLOAT32;
            bool wide=t->shapes[s].buf_format==SH_IMG_ATOMIC_UINT64;
            if(wide && !image64_declared) {
                const char name[]="SPV_EXT_shader_image_int64";
                uint32_t ext[(sizeof(name)+3)/4];memset(ext,0,sizeof(ext));memcpy(ext,name,sizeof(name));
                spv_emit(b,SPV_SEC_EXTENSION,10,ext,PS5C_ARRAY_COUNT(ext));
                uint32_t cap=5016; /* Int64ImageEXT */
                spv_emit(b,SPV_SEC_CAPABILITY,SpvOpCapability,&cap,1);
                cap=12;spv_emit(b,SPV_SEC_CAPABILITY,SpvOpCapability,&cap,1); /* Int64Atomics */
                image64_declared=true;
            }
            uint32_t sampled = wide ? t->t_u64 : packed || float_atomic || kind == 1 ? t->t_u32 : kind == 2 ? t->t_i32 : t->t_f32;
            /* atomics need the format: a 32-bit integer one */
            uint32_t format = wide ? 40 /* R64ui, SPV_EXT_shader_image_int64 */ : packed ? SpvImageFormatR32ui
                            : t->p->ops[t->p->sites[s].inst] != SH_OP_IMAGE_ATOMIC ? SpvImageFormatUnknown
                            : kind == 2 && !float_atomic                        ? SpvImageFormatR32i
                                                                                : SpvImageFormatR32ui;
            uint32_t img = spv_type_image(b, (uint32_t[]){sampled, site_3d(t, (int)s) ? SpvDimDim3D : SpvDimDim2D, 0,
                                                          site_arrayed(t, (int)s) ? 1 : 0, 0, storage ? 2 : 1, format});
            uint32_t si = storage ? img : spv_type_sampled_image(b, img);
            uint32_t ptr = spv_type_pointer(b, SpvStorageClassUniformConstant, si);
            spv_emit(b, SPV_SEC_GLOBAL, SpvOpVariable, (uint32_t[]){ptr, v, SpvStorageClassUniformConstant}, 3);
            if(storage && !t->p->sites[s].store) spv_decorate(b,v,SpvDecorationNonWritable,NULL,0);
            t->site_image_type[s] = img;
            t->site_sampled_type[s] = si;
        }
        spv_decorate(b, v, SpvDecorationDescriptorSet, &set, 1);
        spv_decorate(b, v, SpvDecorationBinding, &binding, 1);
        /* Native interlock supplies acquire/release ordering; coherent accesses
         * make storage aliases visible between fragments. */
        if(t->ordered_pixel && (k!=SH_SITE_IMAGE || sh_site_storage(&t->p->sites[s],&t->shapes[s])))
            spv_decorate(b,v,23 /* Coherent */,NULL,0);
        /* For sampled images address_map is a live-descriptor guard, not
         * storage coherence. */
        if (t->shapes[s].address_map && (k!=SH_SITE_IMAGE || sh_site_storage(&t->p->sites[s],&t->shapes[s])))
            spv_decorate(b,v,23 /* Coherent */,NULL,0);
        if (k == SH_SITE_BUFFER && coherent_buffer_pair(t->p, s)) {
            if (!t->p_sb_u64) {
                uint32_t cap = 12; /* Int64Atomics */
                spv_emit(b, SPV_SEC_CAPABILITY, SpvOpCapability, &cap, 1);
                t->p_sb_u64 = spv_type_pointer(b, SpvStorageClassStorageBuffer, t->t_u64);
            }
            uint32_t array = spv_type_runtime_array(b, t->t_u64), eight = 8;
            spv_decorate(b, array, SpvDecorationArrayStride, &eight, 1);
            uint32_t block = spv_id(b);
            spv_emit(b, SPV_SEC_GLOBAL, SpvOpTypeStruct, (uint32_t[]){block, array}, 2);
            spv_decorate(b, block, SpvDecorationBlock, NULL, 0);
            spv_member_decorate(b, block, 0, SpvDecorationOffset, &zero, 1);
            uint32_t alias = spv_id(b), pointer = spv_type_pointer(b, SpvStorageClassStorageBuffer, block);
            spv_emit(b, SPV_SEC_GLOBAL, SpvOpVariable, (uint32_t[]){pointer, alias, SpvStorageClassStorageBuffer}, 3);
            spv_decorate(b, alias, SpvDecorationDescriptorSet, &set, 1);
            spv_decorate(b, alias, SpvDecorationBinding, &binding, 1);
            spv_decorate(b, alias, 20 /* Aliased */, NULL, 0);
            spv_decorate(b, v, 20 /* Aliased */, NULL, 0);
            spv_decorate(b, alias, 23 /* Coherent */, NULL, 0);
            spv_decorate(b, v, 23 /* Coherent */, NULL, 0);
            if (!t->p->sites[s].store) spv_decorate(b, alias, SpvDecorationNonWritable, NULL, 0);
            t->site_u64[s] = alias;
        }
        binding++;
        t->site_var[s] = v;
    }
}

static void declare_io(tr *t) {
    spv_builder *b = &t->b;
    const sh_stage_info *info = t->info;
    if(info->stage==SH_STAGE_MESH) { mesh_declare(t);return; }
    if (info->stage == SH_STAGE_VS) {
        t->in_vid = input_var(t, t->t_i32, SpvBuiltInVertexIndex);
        t->in_iid = input_var(t, t->t_i32, SpvBuiltInInstanceIndex);
        uint32_t blk = spv_id(b);
        uint32_t members[4] = {blk, t->t_v4f}, nm = 2, nclip = 0;
        if (info->vs_point_size) members[nm++] = t->t_f32; /* point lists: gl_PointSize, member 1 */
        for (uint32_t m = info->vs_ucp_mask & 63u; m; m &= m - 1) nclip++;
        if (nclip) { /* user clip planes: gl_ClipDistance[n] */
            t->clip_member = nm - 1;
            members[nm++] = spv_type_array(b, t->t_f32, spv_const_u32(b, nclip));
            uint32_t cap = 32; /* ClipDistance */
            spv_emit(b, SPV_SEC_CAPABILITY, SpvOpCapability, &cap, 1);
        }
        spv_emit(b, SPV_SEC_GLOBAL, SpvOpTypeStruct, members, nm);
        spv_decorate(b, blk, SpvDecorationBlock, NULL, 0);
        uint32_t pos = SpvBuiltInPosition;
        spv_member_decorate(b, blk, 0, SpvDecorationBuiltIn, &pos, 1);
        if (b->exact) spv_member_decorate(b, blk, 0, SpvDecorationInvariant, NULL, 0);
        if (info->vs_point_size) {
            uint32_t psz = SpvBuiltInPointSize;
            spv_member_decorate(b, blk, 1, SpvDecorationBuiltIn, &psz, 1);
        }
        if (t->clip_member) {
            uint32_t cd = 3; /* BuiltIn ClipDistance */
            spv_member_decorate(b, blk, t->clip_member, SpvDecorationBuiltIn, &cd, 1);
        }
        uint32_t ptr = spv_type_pointer(b, SpvStorageClassOutput, blk);
        t->pervertex = spv_id(b);
        spv_emit(b, SPV_SEC_GLOBAL, SpvOpVariable, (uint32_t[]){ptr, t->pervertex, SpvStorageClassOutput}, 3);
        t->iface[t->n_iface++] = t->pervertex;
        if (info->vs_layer && g_vs_layer) {
            uint32_t v = spv_id(b), bi = SpvBuiltInLayer;
            spv_emit(b, SPV_SEC_GLOBAL, SpvOpVariable,
                     (uint32_t[]){spv_type_pointer(b, SpvStorageClassOutput, t->t_i32), v, SpvStorageClassOutput}, 3);
            spv_decorate(b, v, SpvDecorationBuiltIn, &bi, 1);
            t->iface[t->n_iface++] = v;
            t->out_layer = v;
        }
        for (uint32_t k = 0; k < 32; k++) {
            if ((t->p->param_mask >> k) & 1) t->out_param[k] = output_var(t, t->t_v4f, k);
            if ((t->p->param_mask & info->vs_param_alias_mask) >> k & 1)
                t->out_alias[k] = output_var(t, t->t_v4f, info->vs_param_alias[k]);
        }
    } else if (info->stage == SH_STAGE_PS) {
        for (uint32_t k = 0; k < 8; k++) {
            uint32_t kind = (info->ps_mrt_kind >> (2 * k)) & 3u;
            uint32_t type = kind == 1 ? t->t_v4u : kind == 2 ? t->t_v4i : t->t_v4f;
            if ((t->p->mrt_mask >> k) & 1) t->out_mrt[k] = output_var(t, type, k);
        }
        /* MRTZ shares a target for depth (X), stencil (Y) and sample mask
         * (Z). Declaring FragDepth for a mask-only export would enable
         * DepthReplacing without ever writing the depth value. */
        bool exports_depth = false;
        for (uint32_t i = 0; i < t->p->count; i++) {
            const rdna2_inst *in = &t->p->insts[i];
            exports_depth |= in->enc == RDNA2_ENC_EXP && in->exp_target == 8 && (in->exp_en & 1);
        }
        if (exports_depth) {
            uint32_t v = spv_id(b), bi = SpvBuiltInFragDepth;
            spv_emit(b, SPV_SEC_GLOBAL, SpvOpVariable,
                     (uint32_t[]){spv_type_pointer(b, SpvStorageClassOutput, t->t_f32), v, SpvStorageClassOutput}, 3);
            spv_decorate(b, v, SpvDecorationBuiltIn, &bi, 1);
            t->iface[t->n_iface++] = v;
            t->out_depth = v;
        }
    } else {
        t->in_lid = input_var(t, t->t_v3u, SpvBuiltInLocalInvocationId);
        if (t->info->host_local_size[0] || t->info->cs_wave_mode) {
            t->in_host_lid = t->in_lid;
            t->in_lid = spv_id(&t->b);
            spv_emit(&t->b, SPV_SEC_GLOBAL, SpvOpVariable,
                     (uint32_t[]){spv_type_pointer(&t->b, SpvStorageClassPrivate, t->t_v3u), t->in_lid, SpvStorageClassPrivate}, 3);
        }
        t->in_wgid = input_var(t, t->t_v3u, SpvBuiltInWorkgroupId);
        if(t->info->cs_wave_mode) {
            if(!t->in_sgid)t->in_sgid=input_var(t,t->t_u32,SpvBuiltInSubgroupLocalInvocationId);
            t->in_subgroup=input_var(t,t->t_u32,40 /* SubgroupId */);
        }
    }
}

/* Initial register values, placed after the function's variables */
static void prologue(tr *t) {
    spv_builder *b = &t->b;
    const sh_stage_info *info = t->info;
    uint32_t U = t->t_u32;
    bool tgid[SH_SGPRS] = {0};
    if (info->stage == SH_STAGE_VS && info->vs_point_size) {
        uint32_t ptr = spv_prologue_op(b, SpvOpAccessChain, spv_type_pointer(b, SpvStorageClassOutput, t->t_f32),
                                       (uint32_t[]){t->pervertex, spv_const_i32(b, 1)}, 2);
        float f;
        memcpy(&f, &info->vs_point_size, 4);
        uint32_t val = cf(t, f);
        spv_prologue_void(b, SpvOpStore, (uint32_t[]){ptr, val}, 2);
    }
    if (info->stage == SH_STAGE_CS) {
        if(t->info->cs_wave_mode) {
            uint32_t sub=spv_prologue_op(b,SpvOpLoad,U,&t->in_subgroup,1);
            uint32_t lane=spv_prologue_op(b,SpvOpLoad,U,&t->in_sgid,1);
            uint32_t flat=spv_prologue_op(b,SpvOpIMul,U,(uint32_t[]){sub,cu(t,32)},2);
            flat=spv_prologue_op(b,SpvOpIAdd,U,(uint32_t[]){flat,lane},2);
            uint32_t x=spv_prologue_op(b,SpvOpUMod,U,(uint32_t[]){flat,cu(t,info->local_size[0])},2);
            uint32_t yz=spv_prologue_op(b,SpvOpUDiv,U,(uint32_t[]){flat,cu(t,info->local_size[0])},2);
            uint32_t y=spv_prologue_op(b,SpvOpUMod,U,(uint32_t[]){yz,cu(t,info->local_size[1])},2);
            uint32_t z=spv_prologue_op(b,SpvOpUDiv,U,(uint32_t[]){yz,cu(t,info->local_size[1])},2);
            uint32_t lid=spv_prologue_op(b,SpvOpCompositeConstruct,t->t_v3u,(uint32_t[]){x,y,z},3);
            spv_prologue_void(b,SpvOpStore,(uint32_t[]){t->in_lid,lid},2);
            if(t->wave_groups) {
                uint32_t wave=spv_prologue_op(b,SpvOpShiftRightLogical,U,(uint32_t[]){flat,cu(t,6)},2);
                uint32_t ptr=spv_prologue_op(b,SpvOpAccessChain,spv_type_pointer(b,SpvStorageClassWorkgroup,U),
                    (uint32_t[]){t->wave_arrivals,wave},2);
                spv_prologue_void(b,SpvOpAtomicStore,(uint32_t[]){ptr,cu(t,2),cu(t,0x104),cu(t,0)},4);
                spv_prologue_void(b,SpvOpStore,(uint32_t[]){t->wave_generation,cu(t,0)},2);
                spv_prologue_void(b,SpvOpControlBarrier,(uint32_t[]){cu(t,2),cu(t,2),cu(t,0x108)},3);
            }
        } else if (t->in_host_lid) {
            uint32_t host=spv_prologue_op(b,SpvOpLoad,t->t_v3u,&t->in_host_lid,1), c[3];
            for(unsigned k=0;k<3;k++) c[k]=spv_prologue_op(b,SpvOpCompositeExtract,U,(uint32_t[]){host,k},2);
            uint32_t y=spv_prologue_op(b,SpvOpIMul,U,(uint32_t[]){c[1],cu(t,info->host_local_size[0])},2);
            uint32_t z=spv_prologue_op(b,SpvOpIMul,U,(uint32_t[]){c[2],cu(t,info->host_local_size[0]*info->host_local_size[1])},2);
            uint32_t flat=spv_prologue_op(b,SpvOpIAdd,U,(uint32_t[]){c[0],y},2);
            flat=spv_prologue_op(b,SpvOpIAdd,U,(uint32_t[]){flat,z},2);
            uint32_t sx=info->local_size[0]?info->local_size[0]:64,sy=info->local_size[1]?info->local_size[1]:1;
            c[0]=spv_prologue_op(b,SpvOpUMod,U,(uint32_t[]){flat,cu(t,sx)},2);
            y=spv_prologue_op(b,SpvOpUDiv,U,(uint32_t[]){flat,cu(t,sx)},2);
            c[1]=spv_prologue_op(b,SpvOpUMod,U,(uint32_t[]){y,cu(t,sy)},2);
            c[2]=spv_prologue_op(b,SpvOpUDiv,U,(uint32_t[]){flat,cu(t,sx*sy)},2);
            uint32_t guest=spv_prologue_op(b,SpvOpCompositeConstruct,t->t_v3u,c,3);
            spv_prologue_void(b,SpvOpStore,(uint32_t[]){t->in_lid,guest},2);
        }
        uint32_t wg = spv_prologue_op(b, SpvOpLoad, t->t_v3u, &t->in_wgid, 1);
        uint32_t lid = spv_prologue_op(b, SpvOpLoad, t->t_v3u, &t->in_lid, 1);
        uint32_t n = info->cs_tgid_sgpr;
        for (uint32_t k = 0; k < 3; k++) {
            if (!((info->cs_tgid_enable >> k) & 1)) continue;
            if (n < SH_SGPRS && t->sgpr[n]) {
                uint32_t v = spv_prologue_op(b, SpvOpCompositeExtract, U, (uint32_t[]){wg, k}, 2);
                uint32_t ptr =
                    spv_prologue_op(b, SpvOpAccessChain, t->p_sb_u32,
                                    (uint32_t[]){t->u_var, cu(t, 0), cu(t, SH_U_SGPR + n)}, 3);
                uint32_t base = spv_prologue_op(b, SpvOpLoad, U, &ptr, 1);
                v = spv_prologue_op(b, SpvOpIAdd, U, (uint32_t[]){v, base}, 2);
                spv_prologue_void(b, SpvOpStore, (uint32_t[]){t->sgpr[n], v}, 2);
            }
            if (n < SH_SGPRS) tgid[n] = true;
            n++;
        }
        /* TG_SIZE_EN: the next SGPR holds the wave count of the workgroup (bits 5:0) and this
         * wave's index in it (bits 11:6). RADV's ACO derives each lane's local index from it
         * (v_mbcnt with s_and_b32 tg_size, 0xfc0); left unset, every wave loaded the same part
         * of a shared-memory tile (Longshot's optical flow read garbage and made NaN). */
        if (((info->cs_tgid_enable >> 3) & 1) && n < SH_SGPRS && t->sgpr[n]) {
            uint32_t sx = info->local_size[0] ? info->local_size[0] : 64, sy = info->local_size[1] ? info->local_size[1] : 1,
                     sz = info->local_size[2] ? info->local_size[2] : 1, wave = t->p->wave32 ? 32u : 64u;
            uint32_t lx = spv_prologue_op(b, SpvOpCompositeExtract, U, (uint32_t[]){lid, 0}, 2);
            uint32_t ly = spv_prologue_op(b, SpvOpCompositeExtract, U, (uint32_t[]){lid, 1}, 2);
            uint32_t lz = spv_prologue_op(b, SpvOpCompositeExtract, U, (uint32_t[]){lid, 2}, 2);
            uint32_t flat = spv_prologue_op(b, SpvOpIAdd, U, (uint32_t[]){lx,
                spv_prologue_op(b, SpvOpIMul, U, (uint32_t[]){spv_prologue_op(b, SpvOpIAdd, U, (uint32_t[]){ly,
                    spv_prologue_op(b, SpvOpIMul, U, (uint32_t[]){lz, cu(t, sy)}, 2)}, 2), cu(t, sx)}, 2)}, 2);
            uint32_t id = spv_prologue_op(b, SpvOpUDiv, U, (uint32_t[]){flat, cu(t, wave)}, 2);
            uint32_t waves = (sx * sy * sz + wave - 1) / wave;
            uint32_t v = spv_prologue_op(b, SpvOpBitwiseOr, U, (uint32_t[]){
                spv_prologue_op(b, SpvOpShiftLeftLogical, U, (uint32_t[]){id, cu(t, 6)}, 2), cu(t, waves & 63)}, 2);
            spv_prologue_void(b, SpvOpStore, (uint32_t[]){t->sgpr[n], v}, 2);
            tgid[n] = true; /* not reloaded from the user data afterwards */
        }
        for (uint32_t k = 0; k < info->cs_tid_components && k < 3; k++) {
            if (!t->vgpr[k]) continue;
            uint32_t v = spv_prologue_op(b, SpvOpCompositeExtract, U, (uint32_t[]){lid, k}, 2);
            spv_prologue_void(b, SpvOpStore, (uint32_t[]){t->vgpr[k], v}, 2);
        }
    } else if (info->stage == SH_STAGE_PS && (info->ps_input_addr || (info->ps_quad_disable & 15u))) {
        /* The input VGPRs in SPI_PS_INPUT_ADDR order: perspective sample, center, centroid
         * (2 each), pull model (3), linear sample, center, centroid (2 each), line stipple,
         * then one each: position x, y, z, w, front face, ancillary, sample coverage, fixed
         * point position. Barycentrics stay 0 (v_interp reads the attributes itself);
         * fullscreen passes read the pixel position (Unreal's SV_Position). */
        static const uint8_t k_count[16] = {2, 2, 2, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};
        uint32_t at[16], n = 0;
        for (uint32_t k = 0; k < 16; k++) {
            at[k] = (info->ps_input_addr >> k) & 1 ? n : 0xFFFFFFFFu;
            if ((info->ps_input_addr >> k) & 1) n += k_count[k];
        }
        if(t->bary_p)for(unsigned k=0;k<7;k++)if(at[k]<256 && k!=3) {
            uint32_t var=k<4?t->bary_p:t->bary_n;
            uint32_t weights=spv_prologue_op(b,SpvOpLoad,spv_type_vector(&t->b,t->t_f32,3),&var,1);
            for(unsigned c=0;c<2;c++)if(t->vgpr[at[k]+c]) {
                uint32_t x=spv_prologue_op(b,SpvOpCompositeExtract,t->t_f32,(uint32_t[]){weights,c+1},2);
                x=spv_prologue_op(b,SpvOpBitcast,U,&x,1);
                spv_prologue_void(b,SpvOpStore,(uint32_t[]){t->vgpr[at[k]+c],x},2);
            }
        }
        bool want_pos = false;
        for (uint32_t k = 8; k < 16; k++)
            if (k != 12 && k != 13 && k != 14 && at[k] < 256 && t->vgpr[at[k]]) want_pos = true;
        want_pos |= (info->ps_quad_disable & 15u) != 0;
        uint32_t frag = 0;
        if (want_pos) {
            uint32_t var = t->capture_frag?t->capture_frag:input_var(t, t->t_v4f, SpvBuiltInFragCoord);
            frag = spv_prologue_op(b, SpvOpLoad, t->t_v4f, &var, 1);
        }
        if (info->ps_quad_disable & 15u) {
            uint32_t xy[2];
            for (uint32_t k = 0; k < 2; k++) {
                uint32_t f = spv_prologue_op(b, SpvOpCompositeExtract, t->t_f32, (uint32_t[]){frag, k}, 2);
                uint32_t u = spv_prologue_op(b, SpvOpConvertFToU, U, &f, 1);
                xy[k] = spv_prologue_op(b, SpvOpBitwiseAnd, U, (uint32_t[]){u, cu(t, 1)}, 2);
            }
            uint32_t row = spv_prologue_op(b, SpvOpShiftLeftLogical, U, (uint32_t[]){xy[1], cu(t, 1)}, 2);
            uint32_t bit = spv_prologue_op(b, SpvOpBitwiseOr, U, (uint32_t[]){xy[0], row}, 2);
            uint32_t disabled = spv_prologue_op(b, SpvOpShiftRightLogical, U,
                (uint32_t[]){cu(t, info->ps_quad_disable & 15u), bit}, 2);
            disabled = spv_prologue_op(b, SpvOpBitwiseAnd, U, (uint32_t[]){disabled, cu(t, 1)}, 2);
            uint32_t covered = spv_prologue_op(b, SpvOpIEqual, t->t_bool, (uint32_t[]){disabled, cu(t, 0)}, 2);
            spv_prologue_void(b, SpvOpStore, (uint32_t[]){t->covered, covered}, 2);
            if (info->ps_quad_disable & SH_PS_QUAD_DEMOTE) {
                uint32_t masked = spv_id(b), merge = spv_id(b);
                spv_prologue_void(b, SpvOpSelectionMerge, (uint32_t[]){merge, SpvSelectionControlNone}, 2);
                spv_prologue_void(b, SpvOpBranchConditional, (uint32_t[]){covered, merge, masked}, 3);
                spv_prologue_void(b, SpvOpLabel, &masked, 1);
                spv_prologue_void(b, SpvOpDemoteToHelperInvocationEXT, NULL, 0);
                spv_prologue_void(b, SpvOpBranch, &merge, 1);
                spv_prologue_void(b, SpvOpLabel, &merge, 1);
            }
        }
        for (uint32_t k = 8; k < 16; k++) {
            if (at[k] >= 256 || !t->vgpr[at[k]]) continue;
            uint32_t v;
            if (k <= 11) { /* AMD POS_W_FLOAT is reciprocal FragCoord.w (clip W). */
                v = spv_prologue_op(b, SpvOpCompositeExtract, t->t_f32, (uint32_t[]){frag, k - 8}, 2);
                if (k == 11) v = spv_prologue_op(b, SpvOpFDiv, t->t_f32, (uint32_t[]){cf(t, 1.0f), v}, 2);
                if (k <= 9 && frag_scale_on()) { /* PS5C_RT_SCALE: x + x * (guest / host - 1), from SH_U_VPORT */
                    uint32_t idx = uniform_index(t, cu(t, SH_U_VPORT + (k - 8)));
                    uint32_t ptr = spv_prologue_op(b, SpvOpAccessChain, t->p_sb_u32, (uint32_t[]){t->u_var, cu(t, 0), idx}, 3);
                    uint32_t raw = spv_prologue_op(b, SpvOpLoad, U, &ptr, 1);
                    uint32_t f = spv_prologue_op(b, SpvOpBitcast, t->t_f32, &raw, 1);
                    uint32_t m = spv_prologue_op(b, SpvOpFMul, t->t_f32, (uint32_t[]){v, f}, 2);
                    v = spv_prologue_op(b, SpvOpFAdd, t->t_f32, (uint32_t[]){v, m}, 2);
                }
                v = spv_prologue_op(b, SpvOpBitcast, U, &v, 1);
            } else if (k == 12) { /* front face: 1.0 (non-zero as an integer, positive as a float) */
                uint32_t var = input_var(t, t->t_bool, SpvBuiltInFrontFacing);
                uint32_t ff = spv_prologue_op(b, SpvOpLoad, t->t_bool, &var, 1);
                v = spv_prologue_op(b, SpvOpSelect, U, (uint32_t[]){ff, cu(t, 0x3F800000u), cu(t, 0)}, 3);
            } else if (k == 14) { /* sample coverage: single sampled */
                v = cu(t, 1);
            } else if (k == 15) { /* fixed point position: x | y << 16 (whole pixels) */
                uint32_t x = spv_prologue_op(b, SpvOpCompositeExtract, t->t_f32, (uint32_t[]){frag, 0}, 2);
                uint32_t y = spv_prologue_op(b, SpvOpCompositeExtract, t->t_f32, (uint32_t[]){frag, 1}, 2);
                x = spv_prologue_op(b, SpvOpConvertFToU, U, &x, 1);
                y = spv_prologue_op(b, SpvOpConvertFToU, U, &y, 1);
                y = spv_prologue_op(b, SpvOpShiftLeftLogical, U, (uint32_t[]){y, cu(t, 16)}, 2);
                v = spv_prologue_op(b, SpvOpBitwiseOr, U, (uint32_t[]){x, y}, 2);
            } else if (g_ps_layer) { /* ancillary: render target array index [26:16] (sample id [11:8]: 0).
                                       * Layered passes read their slice there (Unreal's 3D color LUT). */
                uint32_t var = input_var(t, t->t_i32, SpvBuiltInLayer), flat = 0, cap = 2 /* Geometry */;
                spv_decorate(b, var, SpvDecorationFlat, &flat, 0);
                spv_emit(b, SPV_SEC_CAPABILITY, SpvOpCapability, &cap, 1);
                v = spv_prologue_op(b, SpvOpLoad, t->t_i32, &var, 1);
                v = spv_prologue_op(b, SpvOpBitcast, U, &v, 1);
                v = spv_prologue_op(b, SpvOpBitwiseAnd, U, (uint32_t[]){v, cu(t, 0x7FF)}, 2);
                v = spv_prologue_op(b, SpvOpShiftLeftLogical, U, (uint32_t[]){v, cu(t, 16)}, 2);
            } else {
                v = cu(t, 0);
            }
            spv_prologue_void(b, SpvOpStore, (uint32_t[]){t->vgpr[at[k]], v}, 2);
        }
    } else if (info->stage == SH_STAGE_VS) {
        if (t->vgpr[info->vertex_id_vgpr]) {
            uint32_t v = spv_prologue_op(b, SpvOpLoad, t->t_i32, &t->in_vid, 1);
            v = spv_prologue_op(b, SpvOpBitcast, U, &v, 1);
            spv_prologue_void(b, SpvOpStore, (uint32_t[]){t->vgpr[info->vertex_id_vgpr], v}, 2);
        }
        if (t->vgpr[info->instance_id_vgpr]) {
            uint32_t v = spv_prologue_op(b, SpvOpLoad, t->t_i32, &t->in_iid, 1);
            v = spv_prologue_op(b, SpvOpBitcast, U, &v, 1);
            spv_prologue_void(b, SpvOpStore, (uint32_t[]){t->vgpr[info->instance_id_vgpr], v}, 2);
        }
    }
    for (uint32_t n = 0; n < SH_SGPRS; n++) {
        if (!t->sgpr[n] || tgid[n]) continue;
        if (n == RDNA2_OP_EXEC_LO || n == RDNA2_OP_EXEC_LO + 1) {
            /* exec is the lane's own bit: set in the low half, the rest clear (mask
             * arithmetic such as s_andn2_b64 exec in a waterfall loop reaches 0) */
            uint64_t mask=t->wave_threads==64?UINT64_MAX:t->wave_threads?((1ull<<t->wave_threads)-1):0;
            uint32_t value=t->full_wave64?(uint32_t)(mask>>(n==RDNA2_OP_EXEC_LO?0:32)):
                t->full_wave32?(n==RDNA2_OP_EXEC_LO?UINT32_MAX:0):(n==RDNA2_OP_EXEC_LO);
            spv_prologue_void(b, SpvOpStore, (uint32_t[]){t->sgpr[n], cu(t,value)}, 2);
            continue;
        }
        uint32_t p = spv_prologue_op(b, SpvOpAccessChain, t->p_sb_u32, (uint32_t[]){t->u_var, cu(t, 0), cu(t, SH_U_SGPR + n)}, 3);
        uint32_t v = spv_prologue_op(b, SpvOpLoad, U, &p, 1);
        spv_prologue_void(b, SpvOpStore, (uint32_t[]){t->sgpr[n], v}, 2);
    }
    uint32_t f = spv_const_bool(b, false);
    spv_prologue_void(b, SpvOpStore, (uint32_t[]){t->scc, f}, 2);
    if(info->stage==SH_STAGE_MESH)mesh_prologue(t);
}

uint32_t sh_binding_count(const sh_program *p) {
    return sh_binding_count_shapes(p, NULL);
}

uint32_t sh_binding_count_shapes(const sh_program *p, const sh_site_shape *shapes) {
    uint32_t n = 1;
    for (uint32_t s = 0; s < p->site_count; s++) {
        if (p->sites[s].kind != SH_SITE_SMEM || (shapes && shapes[s].address_map)) n++;
    }
    return n;
}

static void add_name(spv_builder *b, uint32_t model, uint32_t fn, const uint32_t *iface, uint32_t n) {
    uint32_t *w=malloc((8+(size_t)n)*sizeof(*w));
    if(!w){b->failed=true;return;}
    w[0] = model;
    w[1] = fn;
    uint32_t sw = spv_string("main", w + 2, 2);
    memcpy(w + 2 + sw, iface, n * 4);
    spv_emit(b, SPV_SEC_ENTRY, SpvOpEntryPoint, w, 2 + sw + n);
    free(w);
}
static void add_all_globals(spv_builder *b,uint32_t model,uint32_t fn) {
    /* SPIR-V 1.4 entry interfaces include all used storage classes. The
     * declaration section also contains types/constants: collect Variables only. */
    spv_words *s=&b->sec[SPV_SEC_GLOBAL];uint32_t n=0;
    for(size_t i=0;i<s->len;i+=s->data[i]>>16) if((s->data[i]&65535)==SpvOpVariable)n++;
    uint32_t *ids=malloc((n?n:1)*sizeof(*ids));
    if(!ids){b->failed=true;return;}
    n=0;
    for(size_t i=0;i<s->len;i+=s->data[i]>>16) if((s->data[i]&65535)==SpvOpVariable)ids[n++]=s->data[i+2];
    add_name(b,model,fn,ids,n);free(ids);
}

bool sh_translate(const sh_program *p, const sh_stage_info *info, const sh_site_shape *shapes, uint32_t binding_base,
                  uint32_t **spirv, size_t *words) {
    for(uint32_t i=0;i<p->site_count;i++)
        if(shapes[i].buf_format==SH_IMG_ATOMIC_UINT64 && !g_image_atomic64)return false;
    if (!sh_uniform_shift_valid(p->site_count,info->uniform_shift)) return false;
    if(!sh_aperture_allowed(p))return false;
    if(sh_flat_apertures(p) && info->stage!=SH_STAGE_CS)return false;
    for(uint32_t i=0;i<p->count;i++) if(p->ops[i]==SH_OP_S_SWAPPC_B64 && p->insts[i].dst!=RDNA2_OP_NULL) {
        bool linked=false;
        for(uint32_t f=1;f<p->function_count;f++) linked|=p->functions[f].caller==i;
        if(!linked)return false;
    }
    /* Late discard preserves neighbouring lanes for implicit derivatives.
     * Storage side effects also require native helper invocations. */
    if (info->stage == SH_STAGE_PS && (info->ps_quad_disable & 15u) && !(info->ps_quad_disable & SH_PS_QUAD_DEMOTE))
        for (uint32_t i = 0; i < p->site_count; i++) if (p->sites[i].store) {
            LOG_E(GPU, "quad-masked pixel stores require shaderDemoteToHelperInvocation");
            return false;
        }
    tr *t = (tr *)calloc(1, sizeof(tr));
    if (!t) return false;
    t->p = p;
    t->translation_flags = sh_translation_flags(info->stage);
    t->uniform_shift = info->uniform_shift;
    t->info = info;
    t->shapes = shapes;
    t->binding_base = binding_base;
    t->ok = true;
    t->flat_apertures=sh_flat_apertures(p);
    t->barrier_exec = true;
    uint32_t relative_slots = 0;
    for(uint32_t i=0;i<p->count;i++) {
        const rdna2_inst *q=&p->insts[i];
        if (p->ops[i] == SH_OP_V_MOVRELS_B32 || p->ops[i] == SH_OP_V_MOVRELSD_B32)
            relative_slots += 256 - ((q->src[0] - RDNA2_OP_VGPR) & 0xFF);
        if (p->ops[i] == SH_OP_V_MOVRELD_B32 || p->ops[i] == SH_OP_V_MOVRELSD_B32)
            relative_slots += 256 - ((q->dst - RDNA2_OP_VGPR) & 0xFF);
        bool alu=q->enc==RDNA2_ENC_SOP1 || q->enc==RDNA2_ENC_SOP2 || q->enc==RDNA2_ENC_SOPC ||
            q->enc==RDNA2_ENC_VOP1 || q->enc==RDNA2_ENC_VOP2 || q->enc==RDNA2_ENC_VOPC ||
            q->enc==RDNA2_ENC_VOP3 || q->enc==RDNA2_ENC_VOP3P;
        if(alu && (q->src[0]==239 || q->src[1]==239 || q->src[2]==239)) {
            if(info->stage!=SH_STAGE_PS || !g_pixel_interlock) {
                LOG_E(GPU,"shader 0x%llx: POPS requires native ordered pixel interlock",(unsigned long long)p->base);
                free(t);return false;
            }
            t->ordered_pixel=true;
        }
    }
    uint64_t threads=(uint64_t)info->local_size[0]*info->local_size[1]*info->local_size[2];
    bool fast_launch=info->stage==SH_STAGE_MESH && (info->mesh_indexed&SH_MESH_FAST_LAUNCH);
    if(fast_launch && (!p->wave32 || !info->mesh_compute || info->mesh_input_vertices!=1 || (info->mesh_indexed&3u))) {
        LOG_E(GPU,"fast launch requires native wave32 and one nonindexed input per compute group");free(t);return false;
    }
    if(info->stage==SH_STAGE_MESH && ((p->wave32 && !fast_launch) || (threads!=64 && threads!=128 && !(info->mesh_compute && threads==256)) || info->local_size[0]!=threads ||
        !info->mesh_input_vertices || info->mesh_input_vertices>64 || !info->mesh_max_vertices || info->mesh_max_vertices>threads ||
        !info->mesh_max_primitives || info->mesh_max_primitives>threads || info->vs_layer || info->vs_ucp_mask)) {
        LOG_E(GPU,"mesh translation requires one wave64, bounded outputs and no layered/clip-plane exports");
        free(t);return false;
    }
    if(info->stage==SH_STAGE_CS && threads==64) {
        t->full_wave64=sh_compute_auto_wave64(p,info);
        t->wave_threads=(uint32_t)threads;
        if(t->full_wave64) LOG_I(GPU,"shader 0x%llx: complete wave64 masks and cross-lane reads (%u workgroup invocations)",
            (unsigned long long)p->base,t->wave_threads);
    }
    if(info->stage==SH_STAGE_CS && info->cs_wave_mode==1) {
        if(p->wave32 || threads<64 || threads>256 || threads%64) {
            LOG_E(GPU,"paired compute waves require bounded complete wave64 groups");
            free(t);return false;
        }
        t->full_wave64=true;t->wave_threads=64;t->wave_groups=threads>64;
    }
    if(info->stage==SH_STAGE_CS && info->cs_wave_mode==2) {
        if(!p->wave32 || threads<32 || threads>1024 || threads%32) {
            LOG_E(GPU,"native32 compute requires complete bounded wave32 groups");free(t);return false;
        }
        t->full_wave64=false;t->full_wave32=true;t->wave_groups=false;t->wave_threads=32;
        LOG_I(GPU,"shader 0x%llx: native32 masks and scalar lane exchange (%u invocations)",
            (unsigned long long)p->base,(unsigned)threads);
    }
    if(info->stage==SH_STAGE_MESH) {
        t->mesh_ballot32=(info->mesh_indexed&SH_MESH_BALLOT32)!=0;
        t->full_wave64=!fast_launch;t->full_wave32=fast_launch;
        t->wave_threads=fast_launch?32:64;t->wave_groups=!fast_launch && threads>64;
    }
    spv_builder *b = &t->b;
    spv_init(b);
    /* The guest rounds after every multiply and add; a host compiler fusing them into fma
     * (Intel does) gives a depth prepass and the base pass slightly different positions,
     * and the base pass's EQUAL/GEQUAL test then fails (Dragon Ball's empty G-buffer).
     * Every stage keeps each operation; explicit FMA remains fused. The builder
     * owns NoContraction so vertex operations never get duplicate decorations.
     * PS5C_SHADER_EXACT=0 off, =1 vertex stages only, =2 every stage (default). */
    b->exact = (t->translation_flags & SH_TRANSLATE_EXACT) != 0;

    /* Opt-in diagnostic only. Never mix this translation with persistent
     * recipes: host subgroup width is not necessarily the guest wave width. */
    t->ps_quad_flow=info->stage==SH_STAGE_PS && getenv("PS5C_PS_QUAD_FLOW") && getenv("PS5C_NO_SHADER_CACHE");
    t->ps_wave_exec=info->stage==SH_STAGE_PS && (getenv("PS5C_PS_SUBGROUP_EXEC") || t->ps_quad_flow) && getenv("PS5C_NO_SHADER_CACHE");
    uint32_t cap = SpvCapabilityShader;
    spv_emit(b, SPV_SEC_CAPABILITY, SpvOpCapability, &cap, 1);
    if(t->ordered_pixel) {
        cap=SpvCapabilityFragmentShaderPixelInterlockEXT;
        spv_emit(b,SPV_SEC_CAPABILITY,SpvOpCapability,&cap,1);
        uint32_t w[16],n=spv_string("SPV_EXT_fragment_shader_interlock",w,16);
        spv_emit(b,SPV_SEC_EXTENSION,10,w,n);
    }
    if (info->stage == SH_STAGE_PS && (info->ps_quad_disable & 15u) && (info->ps_quad_disable & SH_PS_QUAD_DEMOTE)) {
        cap = 5379; /* DemoteToHelperInvocationEXT */
        spv_emit(b, SPV_SEC_CAPABILITY, SpvOpCapability, &cap, 1);
        uint32_t w[16];
        uint32_t n = spv_string("SPV_EXT_demote_to_helper_invocation", w, 16);
        spv_emit(b, SPV_SEC_EXTENSION, 10 /* OpExtension */, w, n);
    }
    if (g_preserve_inf_nan) {
        cap = 4466; /* SignedZeroInfNanPreserve */
        spv_emit(b, SPV_SEC_CAPABILITY, SpvOpCapability, &cap, 1);
        uint32_t w[8];
        uint32_t n = spv_string("SPV_KHR_float_controls", w, 8);
        spv_emit(b, SPV_SEC_EXTENSION, 10 /* OpExtension */, w, n);
    }
    if (info->stage == SH_STAGE_VS && info->vs_layer && g_vs_layer) {
        cap = 5254; /* ShaderViewportIndexLayerEXT */
        spv_emit(b, SPV_SEC_CAPABILITY, SpvOpCapability, &cap, 1);
        uint32_t w[12];
        uint32_t n = spv_string("SPV_EXT_shader_viewport_index_layer", w, 12);
        spv_emit(b, SPV_SEC_EXTENSION, 10 /* OpExtension */, w, n);
    }
    cap = SpvCapabilityInt64;
    spv_emit(b, SPV_SEC_CAPABILITY, SpvOpCapability, &cap, 1);
    cap = SpvCapabilityImageQuery;
    spv_emit(b, SPV_SEC_CAPABILITY, SpvOpCapability, &cap, 1);
    for (uint32_t i = 0; i < p->count; i++) {
        if (p->ops[i] == SH_OP_IMAGE_GATHER4 && strstr(p->insts[i].name, "_o")) {
            cap = SpvCapabilityImageGatherExtended;
            spv_emit(b, SPV_SEC_CAPABILITY, SpvOpCapability, &cap, 1);
            break;
        }
    }
    for (uint32_t s = 0; s < p->site_count; s++) {
        if (p->sites[s].kind == SH_SITE_IMAGE && p->sites[s].store) {
            cap = SpvCapabilityStorageImageWriteWithoutFormat;
            spv_emit(b, SPV_SEC_CAPABILITY, SpvOpCapability, &cap, 1);
            cap = SpvCapabilityStorageImageReadWithoutFormat;
            spv_emit(b, SPV_SEC_CAPABILITY, SpvOpCapability, &cap, 1);
            break;
        }
    }
    bool quad = false;
    for (uint32_t i = 0; i < p->count && info->stage != SH_STAGE_VS; i++)
        quad |= (p->insts[i].enc == RDNA2_ENC_VOP1 || p->insts[i].enc == RDNA2_ENC_VOP2 || p->insts[i].enc == RDNA2_ENC_VOPC) &&
                p->insts[i].src[0] == RDNA2_OP_DPP16;
    bool shuffle = t->full_wave32, lds_wait = false;
    for (uint32_t i = 0; i < p->count; i++) {
        const rdna2_inst *q = &p->insts[i];
        lds_wait |= info->stage == SH_STAGE_CS && sh_barrier_lds_wait(p, i);
        shuffle |= p->ops[i] == SH_OP_V_PERMLANEX16_B32 || p->ops[i] == SH_OP_V_PERMLANE16_B32;
        shuffle |= p->ops[i] == SH_OP_V_MBCNT_LO_U32_B32 || p->ops[i] == SH_OP_V_MBCNT_HI_U32_B32; /* lane index, ballot */
        shuffle |= p->ops[i] == SH_OP_DS && q->op == 0x35; /* ds_swizzle_b32 */
        shuffle |= q->name && (!strcmp(q->name, "ds_bpermute_b32") || !strcmp(q->name, "ds_permute_b32"));
        shuffle |= (q->enc == RDNA2_ENC_VOP1 || q->enc == RDNA2_ENC_VOP2 || q->enc == RDNA2_ENC_VOPC) &&
                   q->src[0] == RDNA2_OP_DPP16 && ((q->dpp_sdwa >> 8) & 0x1FF) > 0xFF;
        shuffle |=
            info->stage != SH_STAGE_VS &&
            (q->enc == RDNA2_ENC_VOP1 || q->enc == RDNA2_ENC_VOP2 || q->enc == RDNA2_ENC_VOPC) &&
            (q->src[0] == RDNA2_OP_DPP8 || q->src[0] == RDNA2_OP_DPP8FI);
    }
    if (quad || shuffle || t->ps_wave_exec || lds_wait || info->cs_wave_mode || t->mesh_ballot32) {
        cap = SpvCapabilityGroupNonUniform;
        spv_emit(b, SPV_SEC_CAPABILITY, SpvOpCapability, &cap, 1);
    }
    if (quad || t->ps_quad_flow) {
        cap = SpvCapabilityGroupNonUniformQuad;
        spv_emit(b, SPV_SEC_CAPABILITY, SpvOpCapability, &cap, 1);
    }
    if(t->ps_wave_exec) {
        cap=SpvCapabilityGroupNonUniformVote;
        spv_emit(b,SPV_SEC_CAPABILITY,SpvOpCapability,&cap,1);
    }
    if (shuffle) {
        cap = SpvCapabilityGroupNonUniformShuffle;
        spv_emit(b, SPV_SEC_CAPABILITY, SpvOpCapability, &cap, 1);
        cap = SpvCapabilityGroupNonUniformBallot; /* v_mbcnt of a comparison mask */
        spv_emit(b, SPV_SEC_CAPABILITY, SpvOpCapability, &cap, 1);
    }
    if((info->cs_wave_mode || t->mesh_ballot32) && !shuffle) {
        cap=SpvCapabilityGroupNonUniformBallot;spv_emit(b,SPV_SEC_CAPABILITY,SpvOpCapability,&cap,1);
    }
    bool bvh_address = false;
    for (uint32_t s = 0; s < p->site_count; s++)
        bvh_address |= p->sites[s].kind == SH_SITE_BVH && (shapes[s].address_map == 3 || shapes[s].address_map == 4);
    if (bvh_address) {
        uint32_t c = 5347 /* PhysicalStorageBufferAddresses */, ext[16];
        spv_emit(b, SPV_SEC_CAPABILITY, SpvOpCapability, &c, 1);
        uint32_t n = spv_string("SPV_KHR_physical_storage_buffer", ext, 16);
        spv_emit(b, SPV_SEC_EXTENSION, 10 /* OpExtension */, ext, n);
    }
    uint32_t mm[2] = {bvh_address ? 5348u /* PhysicalStorageBuffer64 */ : 0u, 1 /* GLSL450 */};
    spv_emit(b, SPV_SEC_MEMORY_MODEL, SpvOpMemoryModel, mm, 2);

    t->t_void = spv_type_void(b);
    t->t_bool = spv_type_bool(b);
    t->t_u32 = spv_type_int(b, 32, false);
    t->t_i32 = spv_type_int(b, 32, true);
    t->t_u64 = spv_type_int(b, 64, false);
    t->t_f32 = spv_type_float(b, 32);
    t->t_v2f = spv_type_vector(b, t->t_f32, 2);
    t->t_v2i = spv_type_vector(b, t->t_i32, 2);
    t->t_v3u = spv_type_vector(b, t->t_u32, 3);
    t->t_v4f = spv_type_vector(b, t->t_f32, 4);
    t->t_v4u = spv_type_vector(b, t->t_u32, 4);
    t->t_v4i = spv_type_vector(b, t->t_i32, 4);
    t->p_fn_u32 = spv_type_pointer(b, SpvStorageClassFunction, t->t_u32);
    t->p_fn_bool = spv_type_pointer(b, SpvStorageClassFunction, t->t_bool);

    declare_resources(t);
    declare_io(t);
    if(t->full_wave64) {
        uint32_t arr=spv_type_array(b,t->t_u32,cu(t,(uint32_t)(threads/64)*66));
        t->wave_scratch=spv_id(b);
        spv_emit(b,SPV_SEC_GLOBAL,SpvOpVariable,(uint32_t[]){spv_type_pointer(b,SpvStorageClassWorkgroup,arr),t->wave_scratch,SpvStorageClassWorkgroup},3);
        if(t->wave_groups) {
            arr=spv_type_array(b,t->t_u32,cu(t,(uint32_t)(threads/64)));t->wave_arrivals=spv_id(b);
            spv_emit(b,SPV_SEC_GLOBAL,SpvOpVariable,(uint32_t[]){spv_type_pointer(b,SpvStorageClassWorkgroup,arr),t->wave_arrivals,SpvStorageClassWorkgroup},3);

        }
    }
    bool needs_rendezvous=false;
    for(uint32_t i=0;i<p->count;i++)needs_rendezvous|=p->ops[i]==SH_OP_S_BARRIER;
    if(info->wave_barriers && needs_rendezvous && (info->stage==SH_STAGE_MESH ||
        (info->stage==SH_STAGE_CS && (t->full_wave32 || t->full_wave64)))) {
        t->wg_shift=t->full_wave32?5:6;t->wg_waves=(uint32_t)threads>>t->wg_shift;
        uint32_t arr=spv_type_array(b,t->t_u32,cu(t,t->wg_waves));t->wg_states=spv_id(b);
        spv_emit(b,SPV_SEC_GLOBAL,SpvOpVariable,(uint32_t[]){spv_type_pointer(b,SpvStorageClassWorkgroup,arr),t->wg_states,SpvStorageClassWorkgroup},3);
    }
    for (uint32_t i = 0; i < p->count; i++) {
        if ((p->insts[i].enc != RDNA2_ENC_FLAT || (p->insts[i].seg != 1 && !t->flat_apertures)) &&
            !mubuf_scratch(p, i)) continue;
        uint32_t arr = spv_type_array(b, t->t_u32, spv_const_u32(b, SCRATCH_DWORDS));
        t->p_pr_u32 = spv_type_pointer(b, SpvStorageClassPrivate, t->t_u32);
        t->scratch = spv_id(b);
        spv_emit(b, SPV_SEC_GLOBAL, SpvOpVariable,
                 (uint32_t[]){spv_type_pointer(b, SpvStorageClassPrivate, arr), t->scratch, SpvStorageClassPrivate}, 3);
        break;
    }
    bool uses_lds = t->flat_apertures;
    for (uint32_t i = 0; i < p->count; i++) uses_lds |= sh_inst_uses_lds(&p->insts[i]);
    if (uses_lds && (info->stage == SH_STAGE_CS || info->stage == SH_STAGE_MESH)) {
        uint32_t n = info->lds_dwords && info->lds_dwords <= 16384 ? info->lds_dwords : 16384;
        t->lds_dwords = n;
        bool wide=false;
        for(uint32_t i=0;i<p->count;i++) wide |= p->insts[i].enc==RDNA2_ENC_DS && !p->insts[i].gds && p->insts[i].op==0x4a;
        wide = wide && g_lds_atomic64 && n>=2 && !(n&1);
        uint32_t arr = spv_type_array(b, t->t_u32, spv_const_u32(b, n));
        t->p_wg_u32 = spv_type_pointer(b, SpvStorageClassWorkgroup, t->t_u32);
        t->lds = spv_id(b);
        if(wide) {
            cap=SpvCapabilityWorkgroupMemoryExplicitLayoutKHR;
            spv_emit(b,SPV_SEC_CAPABILITY,SpvOpCapability,&cap,1);
            if(!t->p_sb_u64){cap=SpvCapabilityInt64Atomics;spv_emit(b,SPV_SEC_CAPABILITY,SpvOpCapability,&cap,1);}
            uint32_t ext[16],ext_words=spv_string("SPV_KHR_workgroup_memory_explicit_layout",ext,16);
            spv_emit(b,SPV_SEC_EXTENSION,10,ext,ext_words);
            uint32_t stride=4,offset=0,block=spv_id(b);
            spv_decorate(b,arr,SpvDecorationArrayStride,&stride,1);
            spv_emit(b,SPV_SEC_GLOBAL,SpvOpTypeStruct,(uint32_t[]){block,arr},2);
            spv_decorate(b,block,SpvDecorationBlock,NULL,0);spv_member_decorate(b,block,0,SpvDecorationOffset,&offset,1);
            spv_emit(b,SPV_SEC_GLOBAL,SpvOpVariable,(uint32_t[]){spv_type_pointer(b,SpvStorageClassWorkgroup,block),t->lds,SpvStorageClassWorkgroup},3);
            spv_decorate(b,t->lds,SpvDecorationAliased,NULL,0);
            uint32_t arr64=spv_type_array(b,t->t_u64,spv_const_u32(b,n/2));stride=8;
            spv_decorate(b,arr64,SpvDecorationArrayStride,&stride,1);
            block=spv_id(b);spv_emit(b,SPV_SEC_GLOBAL,SpvOpTypeStruct,(uint32_t[]){block,arr64},2);
            spv_decorate(b,block,SpvDecorationBlock,NULL,0);spv_member_decorate(b,block,0,SpvDecorationOffset,&offset,1);
            t->lds64=spv_id(b);
            spv_emit(b,SPV_SEC_GLOBAL,SpvOpVariable,(uint32_t[]){spv_type_pointer(b,SpvStorageClassWorkgroup,block),t->lds64,SpvStorageClassWorkgroup},3);
            spv_decorate(b,t->lds64,SpvDecorationAliased,NULL,0);
        } else {
        spv_emit(b, SPV_SEC_GLOBAL, SpvOpVariable,
                 (uint32_t[]){spv_type_pointer(b, SpvStorageClassWorkgroup, arr), t->lds, SpvStorageClassWorkgroup}, 3);
        }
    } else if (uses_lds && info->stage == SH_STAGE_VS && info->lds_dwords) {
        /* NGG: a vertex thread stores its vertex in the LDS and reads it back to export it.
         * One invocation per vertex here, so a private copy holds what that lane would see
         * (reads of other lanes' slots, as culling does, see zeros). */
        uint32_t n = info->lds_dwords <= 8192 ? info->lds_dwords : 8192;
        t->lds_dwords = n;
        t->lds_private = true;
        uint32_t arr = spv_type_array(b, t->t_u32, spv_const_u32(b, n));
        t->p_wg_u32 = spv_type_pointer(b, SpvStorageClassPrivate, t->t_u32);
        t->lds = spv_id(b);
        spv_emit(b, SPV_SEC_GLOBAL, SpvOpVariable,
                 (uint32_t[]){spv_type_pointer(b, SpvStorageClassPrivate, arr), t->lds, SpvStorageClassPrivate}, 3);
    }
    if ((quad || shuffle) && !t->in_sgid) {
        t->in_sgid = input_var(t, t->t_u32, SpvBuiltInSubgroupLocalInvocationId);
        if (info->stage == SH_STAGE_PS) spv_decorate(b, t->in_sgid, SpvDecorationFlat, NULL, 0);
    }

    if(info->stage==SH_STAGE_PS && info->ps_vertex_fetch) {
        for(uint32_t attr=0;attr<32;attr++)
            if((p->attr_mask>>attr&1u) && !(info->ps_attr_location[attr]&32u))
                t->pv_mask|=1u<<(info->ps_attr_location[attr]&31u);
        if(t->pv_mask || (info->ps_input_addr&0x77u)) {
            uint32_t bary_cap=5284,ext[32];spv_emit(b,SPV_SEC_CAPABILITY,SpvOpCapability,&bary_cap,1);
            uint32_t n=spv_string("SPV_KHR_fragment_shader_barycentric",ext,32);
            spv_emit(b,SPV_SEC_EXTENSION,10,ext,n);
            t->bary_p=input_var(t,spv_type_vector(&t->b,t->t_f32,3),5286);t->bary_n=input_var(t,spv_type_vector(&t->b,t->t_f32,3),5287);
        }
    }
    uint32_t fn_type = spv_type_function(b, t->t_void, NULL, 0);
    uint32_t fn = spv_id(b);
    spv_begin_function(b, t->t_void, fn_type, fn);
    label(t);
    /* Large branch trees can exhaust the host compiler. An indexed bank keeps
     * the translation linear for these shaders, while small relative accesses
     * retain independent register variables and their lower execution cost. */
    if (relative_slots >= 4096 && info->stage != SH_STAGE_MESH) {
        uint32_t type = spv_type_array(b, t->t_u32, cu(t, 256));
        t->vgpr_array = spv_local_var(b, spv_type_pointer(b, SpvStorageClassFunction, type));
        uint32_t zero = spv_id(b);
        spv_emit(b, SPV_SEC_GLOBAL, SpvOpConstantNull, (uint32_t[]){type, zero}, 2);
        spv_prologue_void(b, SpvOpStore, (uint32_t[]){t->vgpr_array, zero}, 2);
        /* Relative reads can access every input register, even if the shader
         * never uses its corresponding constant index in the body. */
        for (uint32_t n = 0; n < 256; n++) vreg(t, n);
    }
    if(t->wave_groups)t->wave_generation=spv_local_var(b,t->p_fn_u32);
    t->scc = spv_local_var(b, t->p_fn_bool);
    if(info->stage==SH_STAGE_MESH) {
        sreg(t,2);sreg(t,3);vreg(t,0);vreg(t,1);vreg(t,5);vreg(t,8);
    }
    sreg(t, RDNA2_OP_EXEC_LO);
    for (uint32_t i = 0; i < p->count && uses_lds && !t->lds; i++) {
        const rdna2_inst *q = &p->insts[i];
        uint32_t off = (uint32_t)q->offset & 0xFFFF;
        if (p->ops[i] != SH_OP_DS || q->gds || (q->op != 0xB0 && q->op != 0xB1)) continue;
        /* Allocate host lookup metadata only for these shaders. Every
         * distinct 16-bit immediate gets its own per-invocation variable;
         * no oversized GPU LDS/private array and no sixteen-slot ceiling. */
        if (!t->spill) t->spill = calloc(0x10000, sizeof(*t->spill));
        if (!t->spill) { fail(t, "out of memory for LDS spill offsets"); break; }
        if (!t->spill[off]) {
            t->spill[off] = spv_local_var(b, t->p_fn_u32);
            t->spill_count++;
        }
    }
    if (info->stage == SH_STAGE_PS) {
        t->exported = spv_local_var(b, t->p_fn_bool);
        for (uint32_t i=0;i<p->count;i++) if (p->insts[i].enc==RDNA2_ENC_EXP && p->insts[i].exp_vm) {
            t->vm_valid=spv_local_var(b,t->p_fn_bool);
            st(t,t->vm_valid,spv_const_bool(b,true));
            break;
        }
        if (info->ps_quad_disable & 15u) t->covered = spv_local_var(b, t->p_fn_bool);
        st(t, t->exported, spv_const_bool(b, false));
    }
    if(t->ordered_pixel) spv_op_void(b,SpvOpBeginInvocationInterlockEXT,NULL,0);

    /* basic blocks */
    static const uint32_t k_none = 0xFFFFFFFFu;
    uint32_t *block_of = (uint32_t *)malloc(sizeof(uint32_t) * (p->count + 1));
    uint32_t *block_start = (uint32_t *)malloc(sizeof(uint32_t) * (p->count + 1));
    bool *starts = (bool *)calloc(p->count + 1, 1);
    bool branches = false, calls=false;
    if (!block_of || !block_start || !starts) {
        free(block_of);
        free(block_start);
        free(starts);
        spv_free(b);
        free(t->spill);
        free(t);
        return false;
    }
    starts[0] = true;
    for (uint32_t f=0;f<p->function_count;f++) starts[p->functions[f].first]=true;
    for (uint32_t i = 0; i < p->count; i++) {
        uint32_t op = p->ops[i];
        if (is_branch_op(op)) {
            branches = true;
            uint32_t target = (uint32_t)((int32_t)(p->pc[i] + p->insts[i].words) + p->insts[i].simm16);
            for (uint32_t j = 0; j < p->count; j++) {
                if (p->pc[j] == target) starts[j] = true;
            }
            starts[i + 1] = true;
        } else if (op == SH_OP_S_SWAPPC_B64) {
            calls=branches=true;
            starts[i+1]=true;
        } else if (op == SH_OP_S_ENDPGM) {
            starts[i + 1] = true;
        }
        if(t->wg_states && op==SH_OP_S_BARRIER) {
            branches=true;
            starts[i+1]=true;
        }
    }
    uint32_t barrier_prefix = 0;
    if ((info->stage == SH_STAGE_CS || info->stage == SH_STAGE_MESH) && info->barrier_prefix && t->lds && !t->lds_private) {
        if (info->stage == SH_STAGE_CS) sh_barrier_convergence(p, shapes, t->barrier_safe);
        for (uint32_t i = 0; i + 1 < p->count; i++)
            if (p->ops[i] == SH_OP_S_BARRIER && (sh_barrier_prefix(p, shapes, i, info->stage == SH_STAGE_CS) ||
                (t->full_wave32 && (info->mesh_indexed&SH_JOIN_PREFIX) && sh_barrier_join_prefix(p,i)))) {
                t->barrier_safe[i] = true;
                barrier_prefix = i + 1;
            }
        if (barrier_prefix) starts[barrier_prefix] = true;
    }
    uint32_t nblocks = 0;
    for (uint32_t i = 0; i < p->count; i++) {
        if (starts[i]) block_start[nblocks++] = i;
        block_of[i] = nblocks - 1;
    }
    block_start[nblocks] = p->count;

    { /* PS5C_PS_PROBE=<fnv of the first 32 code bytes>:<byte pc>:<vgpr>: the pixel
       * shader's color 0 becomes that VGPR after the instruction at pc (debugging aid) */
        const char *probe = info->stage == SH_STAGE_PS ? getenv("PS5C_PS_PROBE") : NULL;
        const char *capture=info->stage==SH_STAGE_PS?getenv("PS5C_PS_CAPTURE"):NULL;
        if(capture)probe=capture;
        uint64_t h = 0xCBF29CE484222325ull;
        for (uint32_t k = 0; probe && k < 32 && k < p->dwords * 4; k++) h = (h ^ ((const uint8_t *)p->code)[k]) * 0x100000001B3ull;
        char *end;
        uint64_t wanted_probe=probe?strtoull(probe,&end,16):0;
        if (probe && (wanted_probe == h || wanted_probe == p->hash) && *end == ':') { /* <hash>:<pc>:<vgpr>[,<pc>:<vgpr>...] */
            if(capture) {
                char *start=end+1;
                t->capture_x=(uint32_t)strtoul(start,&end,10);
                bool valid=end!=start && *end==':';
                if(valid) {
                    start=end+1;t->capture_y=(uint32_t)strtoul(start,&end,10);
                    valid=end!=start && *end==':' && t->capture_x<16384 && t->capture_y<16384;
                }
                if(!valid){fail(t,"PS capture requires valid x:y:pc:register");capture=NULL;end="";}
                else {
                    t->ps_capture=true;t->capture_frag=input_var(t,t->t_v4f,SpvBuiltInFragCoord);
                    const char *required=getenv("PS5C_PS_CAPTURE_REQUIRE_PC");
                    t->capture_require=required!=NULL;
                    if(required) {
                        char *e;t->capture_require_pc=(uint32_t)strtoul(required,&e,16);bool found=false;
                        for(uint32_t j=0;j<p->count;j++)if(p->pc[j]*4==t->capture_require_pc &&
                            !is_branch_op(p->ops[j]) && p->ops[j]!=SH_OP_S_ENDPGM && p->ops[j]!=SH_OP_S_CODE_END)found=true;
                        if(*e || !found)fail(t,"PS capture required PC is invalid");
                    }
                    t->capture_reached=spv_local_var(b,t->p_fn_bool);
                    st(t,t->capture_reached,spv_const_bool(b,!t->capture_require));
                }
            }
            for (const char *q = end; t->probe_count < (capture?8u:3u) && (*q == ':' || *q == ',');) {
                uint32_t k = t->probe_count++;
                char *e2;
                t->probe_pcs[k] = (uint32_t)strtoul(q + 1, &e2, 16);
                t->probe_vgprs[k]=0;
                if(*e2==':') {
                    const char *reg=e2+1;bool scalar=*reg=='s';if(scalar)reg++;
                    t->probe_vgprs[k]=(uint32_t)strtoul(reg,&e2,10)+(scalar?256:0);
                }
                t->probe_vars[k] = spv_local_var(b, t->p_fn_u32);
                st(t, t->probe_vars[k], cu(t, capture?0xDEADBEEFu:0));
                q = e2;
            }
            t->probe_var = t->probe_vars[0];
            if(t->ps_capture)for(uint32_t k=0;k<t->probe_count;k++) {
                bool found=false;
                for(uint32_t j=0;j<p->count;j++)if(p->pc[j]*4==t->probe_pcs[k]) {
                    found=true;
                    if(is_branch_op(p->ops[j]) || p->ops[j]==SH_OP_S_ENDPGM || p->ops[j]==SH_OP_S_CODE_END)
                        fail(t,"PS register capture requires a non-terminating instruction");
                    break;
                }
                if(!found || t->probe_vgprs[k]>=384)fail(t,"PS register capture has invalid PC/register");
            }
        }
        const char *solid = info->stage == SH_STAGE_PS ? getenv("PS5C_PS_SOLID") : NULL; /* <program hash> */
        for (const char *q = solid; q && *q && !t->ps_solid; q = strchr(q, ',') ? strchr(q, ',') + 1 : NULL, t->ps_solid_index += !t->ps_solid)
            t->ps_solid = strtoull(q, NULL, 16) == p->hash; /* <hash>[,<hash>...]: magenta, red, green, blue, yellow, cyan, ... */
        if(t->ps_capture && t->ps_solid)fail(t,"PS register capture cannot replace shader output");
        /* PS5C_VS_PROBE=<program hash>:<vertex index>:<byte pc>:<vgpr>[,<pc>:<vgpr>...] (up to 8): that
         * vertex's VGPRs (or sN scalar selectors) after those instructions, read back by the renderer (debugging aid) */
        const char *vprobe = info->stage != SH_STAGE_PS ? getenv("PS5C_VS_PROBE") : NULL; /* compute too */
        /* several shaders: specs separated by ';' */
        while (vprobe && strtoull(vprobe, NULL, 16) != p->hash && strchr(vprobe, ';')) vprobe = strchr(vprobe, ';') + 1;
        if (vprobe && strtoull(vprobe, &end, 16) == p->hash && *end == ':') {
            t->vs_probe = true;
            t->vs_probe_vid = (uint32_t)strtoul(end + 1, &end, 10);
            for (const char *q = end; t->probe_count < 8 && (*q == ':' || *q == ',');) {
                uint32_t k = t->probe_count++;
                char *e2;
                t->probe_pcs[k] = (uint32_t)strtoul(q + 1, &e2, 16);
                t->probe_vgprs[k]=0;
                if(*e2==':') {
                    const char *reg=e2+1;bool scalar=*reg=='s';if(scalar)reg++;
                    t->probe_vgprs[k]=(uint32_t)strtoul(reg,&e2,10)+(scalar?256:0);
                }
                t->probe_vars[k] = spv_local_var(b, t->p_fn_u32);
                st(t, t->probe_vars[k], cu(t, 0xDEADBEEFu));
                q = e2;
            }
            t->probe_var = t->probe_vars[0];
        }
    }
    const char*nan_setting=info->stage==SH_STAGE_PS?getenv("PS5C_PS_NAN_TRACE"):NULL;
    if(nan_setting && !strchr(nan_setting,':') && strtoull(nan_setting,NULL,16)==p->hash) {
        if(t->ps_capture)fail(t,"PS register capture cannot mix with nonfinite color probing");
        t->nan_trace=true;t->probe_count=3;
        for(unsigned k=0;k<3;k++){t->probe_vars[k]=spv_local_var(b,t->p_fn_u32);st(t,t->probe_vars[k],cu(t,0));}
        t->probe_var=t->probe_vars[0];t->nan_found=spv_local_var(b,t->p_fn_bool);st(t,t->nan_found,spv_const_bool(b,false));
    }
    bool leaf_inline[SH_MAX_FUNCTIONS]={0};uint32_t leaf_count=0;
    if(t->full_wave32 && (info->mesh_indexed&SH_LEAF_CALL_INLINE)) {
        for(uint32_t f=1;f<p->function_count;f++) {
            const sh_function *range=p->functions+f;
            uint32_t end=range->first+range->count;
            if(!range->count || end>p->count || range->caller>=p->count ||
               p->ops[range->caller]!=SH_OP_S_SWAPPC_B64 ||
               p->insts[range->caller].dst==RDNA2_OP_NULL ||
               p->ops[end-1]!=SH_OP_S_SWAPPC_B64 || p->insts[end-1].dst!=RDNA2_OP_NULL)continue;
            bool safe=true;
            for(uint32_t other=1;other<p->function_count;other++)
                if(other!=f && p->functions[other].caller==range->caller)safe=false;
            for(uint32_t i=range->first;i<end-1;i++)
                if(p->ops[i]==SH_OP_S_SWAPPC_B64 || p->ops[i]==SH_OP_S_BARRIER ||
                   p->ops[i]==SH_OP_S_ENDPGM || p->ops[i]==SH_OP_S_CODE_END || sh_inst_uses_lds(p->insts+i) ||
                   (is_branch_op(p->ops[i]) && p->target[i]<=i))safe=false; /* retain original VM loop budget */
            const rdna2_inst *ret=p->insts+end-1,*call=p->insts+range->caller;
            if(ret->src[0]>104 || (ret->src[0]&1) || call->src[0]>104 || (call->src[0]&1) ||
               call->dst>104 || (call->dst&1))safe=false;
            sf_state proof={t,false,false,0,NULL};
            if(safe && sf_range(&proof,range->first,end-1,NULL)){leaf_inline[f]=true;leaf_count++;}
        }
        if(leaf_count)LOG_I(GPU,"shader 0x%llx: %u structured wave32 leaf calls, runtime target/return guards",
            (unsigned long long)p->base,leaf_count);
    }
    bool structured = false;
    if (branches && !calls && !t->wg_states && !(t->translation_flags & SH_TRANSLATE_NO_STRUCTURED)) {
        bool uniform_flow = false; /* as for the dispatch loop below */
        for (uint32_t i = 0; i < p->count && t->lds && !t->lds_private; i++) uniform_flow |= p->ops[i] == SH_OP_S_BARRIER;
        bool barrier_exec = true;
        for (uint32_t i = 0; i < p->count; i++) {
            if (is_branch_op(p->ops[i]) && !(p->ops[i] == SH_OP_S_CBRANCH_EXECZ && p->insts[i].simm16 >= 0))
                barrier_exec = false;
        }
        sf_state sf = {t, false, uniform_flow && barrier_exec, 0, NULL, false};
        /* Jumps to the end from nested code (a discard test) and early ends break out
         * of the program run as a loop once. Only when the plain shape fails; then, in
         * compute shaders (no exports a lane without exec must not make), exec skips out
         * of their construct fall through (better than a dispatch loop). */
        sf_loop once = {p->count, 0};
        bool ok = sf_range(&sf, 0, p->count, NULL);
        for (uint32_t attempt = 0; !ok && attempt < (info->stage == SH_STAGE_CS ? 3u : 1u); attempt++) {
            sf.execz_out = attempt > 0;
            sf.top = attempt == 1 ? NULL : &once;
            ok = sf_range(&sf, 0, p->count, sf.top);
        }
        if (!ok) sf.top = NULL, sf.execz_out = false;
        else if (sf.execz_out) LOG_I(GPU, "shader 0x%llx: exec skips out of their construct run through", (unsigned long long)p->base);
        if (ok) {
            structured = true;
            t->barrier_exec = barrier_exec;
            if (info->stage == SH_STAGE_CS && t->lds && !t->lds_private)
                sh_barrier_convergence(p, shapes, t->barrier_safe);
            sf.emit = true;
            sf.count = spv_local_var(b, t->p_fn_u32);
            st(t, sf.count, cu(t, 0));
            uint32_t header = 0, body = 0, cont = 0;
            if (sf.top) {
                header = spv_id(b), body = spv_id(b), cont = spv_id(b), once.merge = spv_id(b);
                branch(t, header);
                spv_label(b, header);
                spv_op_void(b, SpvOpLoopMerge, (uint32_t[]){once.merge, cont, SpvLoopControlNone}, 3);
                branch(t, body);
                spv_label(b, body);
            }
            sf_range(&sf, 0, p->count, sf.top);
            if (sf.top) {
                branch(t, once.merge);
                spv_label(b, cont); /* never reached */
                branch(t, header);
                spv_label(b, once.merge);
            }
            finish(t);
        }
    }
    if (structured) {
    } else if (!branches) {
        for (uint32_t i = 0; i < p->count && t->ok; i++) translate_inst(t, i);
        finish(t);
    } else if (nblocks > MAX_BLOCKS) {
        fail(t, "too many blocks");
    } else {
        LOG_I(GPU, "shader 0x%llx (%016llx): block dispatch loop, %u blocks", (unsigned long long)p->base,
              (unsigned long long)p->hash, nblocks);
        t->pc = spv_local_var(b, t->p_fn_u32);
        st(t, t->pc, cu(t, 0));
        uint32_t barrier_resume=t->wg_states?spv_local_var(b,t->p_fn_u32):0;
        if(barrier_resume)st(t,barrier_resume,cu(t,PC_END));
        uint32_t wg_done=t->wg_states?spv_local_var(b,t->p_fn_bool):0;
        if(wg_done)st(t,wg_done,spv_const_bool(b,false));
        bool prefix_emitted = false;
        if (barrier_prefix && p->ops[p->count - 1] == SH_OP_S_ENDPGM) {
            /* Execute a proven forward prefix once, outside the divergent
             * PC loop. Its only external exit is the common terminal END.
             * A single-iteration loop supplies a legal structured escape
             * from nested selections, without putting a workgroup wait in
             * the later per-wave control flow. */
            uint32_t merge = spv_id(b), body = spv_id(b), head = spv_id(b), cont = spv_id(b);
            sf_state prefix = {t, false, false, 0, NULL};
            sf_loop escape = {p->count - 1, merge};
            if (sf_range(&prefix, 0, barrier_prefix, &escape)) {
                t->barrier_exec = false;
                st(t, t->pc, cu(t, PC_END));
                branch(t, head);
                spv_label(b, head);
                spv_op_void(b, SpvOpLoopMerge, (uint32_t[]){merge, cont, SpvLoopControlNone}, 3);
                branch(t, body);
                spv_label(b, body);
                prefix.emit = true;
                prefix.count = spv_local_var(b, t->p_fn_u32);
                st(t, prefix.count, cu(t, 0));
                sf_range(&prefix, 0, barrier_prefix, &escape);
                st(t, t->pc, cu(t, block_of[barrier_prefix]));
                branch(t, cont);
                spv_label(b, cont);
                spv_op_void(b, SpvOpBranchConditional, (uint32_t[]){spv_const_bool(b,false),head,merge}, 3);
                spv_label(b, merge);
                LOG_I(GPU, "shader 0x%llx: structured LDS prefix through PC %04x before dispatch loop",
                      (unsigned long long)p->base, p->pc[barrier_prefix - 1] * 4);
                prefix_emitted = true;
                /* sh_barrier_prefix rejects every edge back into this prefix.
                 * Its execution barriers have now completed before the PC
                 * loop. Independent guest waves can finish the suffix without
                 * repeated workgroup rendezvous if it contains no barriers.
                 * Paired64's per-wave exchanges remain unchanged. */
                bool later_barrier = false;
                for (uint32_t i = barrier_prefix; i < p->count; ++i)
                    later_barrier |= p->ops[i] == SH_OP_S_BARRIER;
                if (t->wg_states && !later_barrier) {
                    t->wg_states = 0;
                    LOG_I(GPU, "shader 0x%llx: all LDS barriers completed in prefix; independent suffix",
                          (unsigned long long)p->base);
                }
                if(!later_barrier && t->full_wave32 && !calls && (info->mesh_indexed&SH_JOIN_SUFFIX)) {
                    bool forward=true;
                    for(uint32_t i=barrier_prefix;i<p->count;i++)
                        if(is_branch_op(p->ops[i]) && p->target[i]<=i)forward=false;
                    sf_state suffix={t,false,false,0,NULL};
                    if(forward && sf_range(&suffix,barrier_prefix,p->count,NULL)) {
                        suffix.emit=true;suffix.count=spv_local_var(b,t->p_fn_u32);st(t,suffix.count,cu(t,0));
                        sf_range(&suffix,barrier_prefix,p->count,NULL);finish(t);
                        LOG_I(GPU,"shader 0x%llx: structured forward suffix, no block dispatcher",
                            (unsigned long long)p->base);
                        goto shader_flow_done;
                    }
                }
            }
        }
        /* Prefix blocks remain as empty switch cases below. They must not
         * emit their barriers again inside the divergent suffix executor. */
        memset(t->barrier_safe, 0, sizeof(t->barrier_safe));
        bool uniform_flow = false; /* workgroup barriers: lanes must not diverge on exec skips */
        for (uint32_t i = 0; i < p->count && t->lds && !t->lds_private; i++) uniform_flow |= p->ops[i] == SH_OP_S_BARRIER;
        /* execution barriers only where every branch left is a forward exec skip */
        t->barrier_exec = true;
        for (uint32_t i = 0; i < p->count; i++) {
            if (is_branch_op(p->ops[i]) && !(p->ops[i] == SH_OP_S_CBRANCH_EXECZ && p->insts[i].simm16 >= 0))
                t->barrier_exec = false;
        }
        /* with other branches (loops) an exec skip can be a loop exit: taken per lane */
        uniform_flow = uniform_flow && t->barrier_exec && !t->wg_states;
        /* a bound on executed blocks: a loop the translation gets wrong (or one
         * waiting on work that is not emulated) must not hang the host GPU */
        uint32_t steps = spv_local_var(b, t->p_fn_u32);
        st(t, steps, cu(t, 0));
        uint32_t header = spv_id(b), dispatch = spv_id(b), swm = spv_id(b), cont = spv_id(b), exit_l = spv_id(b);
        branch(t, header);
        spv_label(b, header);
        spv_op_void(b, SpvOpLoopMerge, (uint32_t[]){exit_l, cont, SpvLoopControlNone}, 3);
        branch(t, dispatch);
        spv_label(b, dispatch);
        uint32_t pcv = ld(t, t->t_u32, t->pc);
        spv_op_void(b, SpvOpSelectionMerge, (uint32_t[]){swm, SpvSelectionControlNone}, 2);
        uint32_t *sw = (uint32_t *)malloc(sizeof(uint32_t) * (2 + nblocks * 2));
        uint32_t *labels = (uint32_t *)malloc(sizeof(uint32_t) * nblocks);
        if (!sw || !labels) {
            fail(t, "out of memory");
        } else {
            sw[0] = pcv;
            sw[1] = swm;
            for (uint32_t k = 0; k < nblocks; k++) {
                labels[k] = spv_id(b);
                sw[2 + k * 2] = k;
                sw[3 + k * 2] = labels[k];
            }
            spv_op_void(b, SpvOpSwitch, sw, 2 + nblocks * 2);
            for (uint32_t k = 0; k < nblocks; k++) {
                spv_label(b, labels[k]);
                uint32_t first = block_start[k], end = block_start[k + 1];
                bool inlined_body=false;
                for(uint32_t f=1;f<p->function_count;f++)if(leaf_inline[f] && first>=p->functions[f].first &&
                    first<p->functions[f].first+p->functions[f].count)inlined_body=true;
                if(inlined_body){st(t,t->pc,cu(t,PC_END));branch(t,swm);continue;}
                if (prefix_emitted && first < barrier_prefix) {
                    st(t, t->pc, cu(t, PC_END));
                    branch(t, swm);
                    continue;
                }
                for (uint32_t i = first; i < end && t->ok; i++) translate_inst(t, i);
                uint32_t last = end - 1, op = p->ops[last];
                uint32_t next = k + 1 < nblocks ? k + 1 : PC_END;
                if (op == SH_OP_S_ENDPGM) {
                    st(t, t->pc, cu(t, PC_END));
                } else if (op==SH_OP_S_SWAPPC_B64) {
                    const rdna2_inst *in=&p->insts[last];
                    uint32_t inline_f=0;
                    for(uint32_t f=1;f<p->function_count;f++)if(leaf_inline[f] && p->functions[f].caller==last)inline_f=f;
                    if(inline_f) {
                        const sh_function *range=p->functions+inline_f;
                        /* Load the real target before writing an overlapping link. */
                        uint32_t lo=get_s(t,in->src[0]),hi=get_s(t,in->src[0]+1);
                        guest_addr ret=sh_inst_address(p,last)+(uint64_t)in->words*4;
                        uint32_t matches=op2(t,SpvOpLogicalAnd,t->t_bool,
                            op2(t,SpvOpIEqual,t->t_bool,lo,cu(t,(uint32_t)range->base)),
                            op2(t,SpvOpIEqual,t->t_bool,hi,cu(t,(uint32_t)(range->base>>32))));
                        set_s(t,in->dst,cu(t,(uint32_t)ret));set_s(t,in->dst+1,cu(t,(uint32_t)(ret>>32)));
                        st(t,t->pc,cu(t,PC_END));uint32_t join=begin_if(t,matches);
                        sf_state body={t,true,false,spv_local_var(b,t->p_fn_u32),NULL};st(t,body.count,cu(t,0));
                        sf_range(&body,range->first,range->first+range->count-1,NULL);
                        const rdna2_inst *exit=p->insts+range->first+range->count-1;
                        matches=op2(t,SpvOpLogicalAnd,t->t_bool,
                            op2(t,SpvOpIEqual,t->t_bool,get_s(t,exit->src[0]),cu(t,(uint32_t)ret)),
                            op2(t,SpvOpIEqual,t->t_bool,get_s(t,exit->src[0]+1),cu(t,(uint32_t)(ret>>32))));
                        st(t,t->pc,sel(t,t->t_u32,matches,cu(t,block_of[last+1]),cu(t,PC_END)));end_if(t,join);
                    } else
                    if (in->src[0]>104 || (in->src[0]&1) ||
                        (in->dst!=RDNA2_OP_NULL && (in->dst>104 || (in->dst&1)))) {
                        fail(t,"unsupported scalar call register pair");
                    } else {
                        uint32_t lo=get_s(t,in->src[0]),hi=get_s(t,in->src[0]+1),dest=cu(t,PC_END);
                        for (uint32_t f=1;f<p->function_count;f++) {
                            const sh_function *range=&p->functions[f];
                            bool ret=in->dst==RDNA2_OP_NULL;
                            if (ret ? !(last>=range->first && last<range->first+range->count) : range->caller!=last) continue;
                            uint32_t j=ret?range->caller+1:range->first;
                            guest_addr addr=ret?sh_inst_address(p,range->caller)+(uint64_t)p->insts[range->caller].words*4:range->base;
                            uint32_t match=op2(t,SpvOpLogicalAnd,t->t_bool,
                                op2(t,SpvOpIEqual,t->t_bool,lo,cu(t,(uint32_t)addr)),
                                op2(t,SpvOpIEqual,t->t_bool,hi,cu(t,(uint32_t)(addr>>32))));
                            dest=sel(t,t->t_u32,match,cu(t,block_of[j]),dest);
                        }
                        /* Preserve target SSA values when source/link overlap. */
                        if (in->dst!=RDNA2_OP_NULL) {
                            guest_addr ret=sh_inst_address(p,last)+(uint64_t)in->words*4;
                            set_s(t,in->dst,cu(t,(uint32_t)ret));set_s(t,in->dst+1,cu(t,(uint32_t)(ret>>32)));
                        }
                        st(t,t->pc,dest);
                    }
                } else if(op==SH_OP_S_BARRIER && t->wg_states) {
                    st(t,barrier_resume,cu(t,next));
                    st(t,t->pc,cu(t,PC_BARRIER));
                } else if (is_branch_op(op) && op == SH_OP_S_CBRANCH_EXECZ && uniform_flow &&
                           (int32_t)p->insts[last].simm16 >= 0) {
                    /* a forward skip over code the lane's exec masks anyway: taken on the
                     * hardware only when the whole wave is inactive; here every lane walks
                     * the same blocks, which keeps barriers in uniform control flow */
                    st(t, t->pc, cu(t, next));
                } else if (is_branch_op(op)) {
                    uint32_t target = (uint32_t)((int32_t)(p->pc[last] + p->insts[last].words) + p->insts[last].simm16);
                    uint32_t tb = k_none;
                    for (uint32_t j = 0; j < p->count; j++) {
                        if (p->pc[j] == target) tb = block_of[j];
                    }
                    if (tb == k_none) tb = PC_END;
                    if (op == SH_OP_S_BRANCH) st(t, t->pc, cu(t, tb));
                    else st(t, t->pc, sel(t, t->t_u32, branch_cond(t, op), cu(t, tb), cu(t, next)));
                } else {
                    st(t, t->pc, cu(t, next));
                }
                branch(t, swm);
            }
        }
        free(sw);
        free(labels);
        spv_label(b, swm);
        branch(t, cont);
        spv_label(b, cont);
        if(t->wg_states) {
            /* WAIT/END waves remain host-active. Let other guest waves run
             * up to eight blocks before the next rendezvous: a parked wave
             * cannot access LDS again until all live waves have arrived.
             * The host counter is uniform, including for parked waves. */
            uint32_t rendezvous=begin_if(t,op2(t,SpvOpIEqual,t->t_bool,
                op2(t,SpvOpBitwiseAnd,t->t_u32,ld(t,t->t_u32,steps),cu(t,7)),cu(t,7)));
            /* Advance every host invocation in lockstep. Guest waves parked
             * at a barrier (or END) keep participating in the host loop, so
             * these two workgroup barriers always have uniform execution. */
            uint32_t u=t->t_u32,flat=local_flat(t),ptr_type=spv_type_pointer(b,SpvStorageClassWorkgroup,u);
            uint32_t wave=op2(t,SpvOpShiftRightLogical,u,flat,cu(t,t->wg_shift));
            uint32_t leader=begin_if(t,op2(t,SpvOpIEqual,t->t_bool,op2(t,SpvOpBitwiseAnd,u,flat,cu(t,(1u<<t->wg_shift)-1)),cu(t,0)));
            st(t,op2(t,SpvOpAccessChain,ptr_type,t->wg_states,wave),ld(t,u,t->pc));end_if(t,leader);
            spv_op_void(b,SpvOpControlBarrier,(uint32_t[]){cu(t,2),cu(t,2),cu(t,0x108)},3);
            uint32_t group_done=spv_const_bool(b,true),parked=spv_const_bool(b,true);
            for(uint32_t k=0;k<t->wg_waves;k++) {
                uint32_t state=ld(t,u,op2(t,SpvOpAccessChain,ptr_type,t->wg_states,cu(t,k)));
                uint32_t ended=op2(t,SpvOpIEqual,t->t_bool,state,cu(t,PC_END));
                group_done=op2(t,SpvOpLogicalAnd,t->t_bool,group_done,ended);
                parked=op2(t,SpvOpLogicalAnd,t->t_bool,parked,op2(t,SpvOpLogicalOr,t->t_bool,ended,
                    op2(t,SpvOpIEqual,t->t_bool,state,cu(t,PC_BARRIER))));
            }
            uint32_t pc=ld(t,u,t->pc),resume=op2(t,SpvOpLogicalAnd,t->t_bool,parked,
                op2(t,SpvOpIEqual,t->t_bool,pc,cu(t,PC_BARRIER)));
            st(t,t->pc,sel(t,u,resume,ld(t,u,barrier_resume),pc));
            st(t,wg_done,group_done);
            spv_op_void(b,SpvOpControlBarrier,(uint32_t[]){cu(t,2),cu(t,2),cu(t,0x108)},3);
            end_if(t,rendezvous);
        }
        uint32_t n = op2(t, SpvOpIAdd, t->t_u32, ld(t, t->t_u32, steps), cu(t, 1));
        st(t, steps, n);
        uint32_t exhausted=op2(t,SpvOpUGreaterThanEqual,t->t_bool,n,cu(t,1u<<14));
        uint32_t done=op2(t,SpvOpLogicalOr,t->t_bool,
            t->wg_states?ld(t,t->t_bool,wg_done):op2(t,SpvOpIEqual,t->t_bool,ld(t,t->t_u32,t->pc),cu(t,PC_END)),exhausted);
        spv_op_void(b, SpvOpBranchConditional, (uint32_t[]){done, exit_l, header}, 3);
        spv_label(b, exit_l);
        finish(t);
    }
shader_flow_done:
    free(block_of);
    free(block_start);
    free(starts);

    prologue(t);
    spv_end_function(b);

    uint32_t model = info->stage == SH_STAGE_MESH ? (info->mesh_compute?SpvExecutionModelGLCompute:5365) : info->stage == SH_STAGE_VS ? SpvExecutionModelVertex
                   : info->stage == SH_STAGE_PS ? SpvExecutionModelFragment : SpvExecutionModelGLCompute;
    if(t->lds64 || info->stage==SH_STAGE_MESH) add_all_globals(b,model,fn);
    else add_name(b, model, fn, t->iface, t->n_iface);
    if (g_preserve_inf_nan) spv_emit(b, SPV_SEC_EXEC_MODE, SpvOpExecutionMode, (uint32_t[]){fn, 4461 /* SignedZeroInfNanPreserve */, 32}, 3);
    if (info->stage == SH_STAGE_PS) {
        spv_emit(b, SPV_SEC_EXEC_MODE, SpvOpExecutionMode, (uint32_t[]){fn, SpvExecutionModeOriginUpperLeft}, 2);
        if(t->ordered_pixel) spv_emit(b,SPV_SEC_EXEC_MODE,SpvOpExecutionMode,
            (uint32_t[]){fn,SpvExecutionModePixelInterlockOrderedEXT},2);
        if (t->out_depth) spv_emit(b, SPV_SEC_EXEC_MODE, SpvOpExecutionMode, (uint32_t[]){fn, SpvExecutionModeDepthReplacing}, 2);
    } else if (info->stage == SH_STAGE_CS || info->stage == SH_STAGE_MESH) {
        const uint32_t *size=info->host_local_size[0]?info->host_local_size:info->local_size;
        uint32_t m[5] = {fn, SpvExecutionModeLocalSize, size[0] ? size[0] : 64,
                         size[1] ? size[1] : 1, size[2] ? size[2] : 1};
        spv_emit(b, SPV_SEC_EXEC_MODE, SpvOpExecutionMode, m, 5);
        if(info->stage==SH_STAGE_MESH && !info->mesh_compute) {
            spv_emit(b,SPV_SEC_EXEC_MODE,SpvOpExecutionMode,(uint32_t[]){fn,SpvExecutionModeOutputVertices,info->mesh_max_vertices},3);
            spv_emit(b,SPV_SEC_EXEC_MODE,SpvOpExecutionMode,(uint32_t[]){fn,5270,info->mesh_max_primitives},3);
            spv_emit(b,SPV_SEC_EXEC_MODE,SpvOpExecutionMode,(uint32_t[]){fn,5298},2);
        }
    }
    bool ok = t->ok && spv_finish(b, spirv, words);
    if(ok && (t->lds64 || info->stage==SH_STAGE_MESH)) (*spirv)[1]=0x00010400;
    spv_free(b);
    free(t->spill);
    free(t);
    return ok;
}

/* ---- Rectangle lists ---- */

bool sh_rectlist_gs(uint32_t param_mask, uint32_t **spirv, size_t *words) {
    spv_builder bb, *b = &bb;
    spv_init(b);
    uint32_t cap = SpvCapabilityShader;
    spv_emit(b, SPV_SEC_CAPABILITY, SpvOpCapability, &cap, 1);
    cap = SpvCapabilityGeometry;
    spv_emit(b, SPV_SEC_CAPABILITY, SpvOpCapability, &cap, 1);
    spv_emit(b, SPV_SEC_MEMORY_MODEL, SpvOpMemoryModel, (uint32_t[]){0, 1}, 2);
    uint32_t t_void = spv_type_void(b), t_f32 = spv_type_float(b, 32), t_v4f = spv_type_vector(b, t_f32, 4);
    uint32_t three = spv_const_u32(b, 3);
    uint32_t iface[70], n_iface = 0;

    /* gl_PerVertex in and out */
    uint32_t blk = spv_id(b);
    spv_emit(b, SPV_SEC_GLOBAL, SpvOpTypeStruct, (uint32_t[]){blk, t_v4f}, 2);
    spv_decorate(b, blk, SpvDecorationBlock, NULL, 0);
    uint32_t pos = SpvBuiltInPosition;
    spv_member_decorate(b, blk, 0, SpvDecorationBuiltIn, &pos, 1);
    uint32_t blk_arr = spv_type_array(b, blk, three);
    uint32_t in_pv = spv_id(b), out_pv = spv_id(b);
    spv_emit(b, SPV_SEC_GLOBAL, SpvOpVariable,
             (uint32_t[]){spv_type_pointer(b, SpvStorageClassInput, blk_arr), in_pv, SpvStorageClassInput}, 3);
    spv_emit(b, SPV_SEC_GLOBAL, SpvOpVariable,
             (uint32_t[]){spv_type_pointer(b, SpvStorageClassOutput, blk), out_pv, SpvStorageClassOutput}, 3);
    iface[n_iface++] = in_pv;
    iface[n_iface++] = out_pv;
    uint32_t v4_arr = spv_type_array(b, t_v4f, three);
    uint32_t in_p[32] = {0}, out_p[32] = {0};
    for (uint32_t k = 0; k < 32; k++) {
        if (!((param_mask >> k) & 1)) continue;
        in_p[k] = spv_id(b);
        out_p[k] = spv_id(b);
        spv_emit(b, SPV_SEC_GLOBAL, SpvOpVariable,
                 (uint32_t[]){spv_type_pointer(b, SpvStorageClassInput, v4_arr), in_p[k], SpvStorageClassInput}, 3);
        spv_emit(b, SPV_SEC_GLOBAL, SpvOpVariable,
                 (uint32_t[]){spv_type_pointer(b, SpvStorageClassOutput, t_v4f), out_p[k], SpvStorageClassOutput}, 3);
        spv_decorate(b, in_p[k], SpvDecorationLocation, &k, 1);
        spv_decorate(b, out_p[k], SpvDecorationLocation, &k, 1);
        iface[n_iface++] = in_p[k];
        iface[n_iface++] = out_p[k];
    }
    uint32_t p_in_v4 = spv_type_pointer(b, SpvStorageClassInput, t_v4f);
    uint32_t p_out_v4 = spv_type_pointer(b, SpvStorageClassOutput, t_v4f);
    uint32_t zero_i = spv_const_i32(b, 0);

    uint32_t fn = spv_id(b);
    spv_begin_function(b, t_void, spv_type_function(b, t_void, NULL, 0), fn);
    spv_label(b, spv_id(b));
    /* load the three vertices of every attribute (position = attribute 32) */
    uint32_t vals[33][4];
    for (uint32_t k = 0; k <= 32; k++) {
        if (k < 32 && !in_p[k]) continue;
        for (uint32_t v = 0; v < 3; v++) {
            uint32_t ptr = k == 32 ? spv_op(b, SpvOpAccessChain, p_in_v4, (uint32_t[]){in_pv, spv_const_i32(b, (int32_t)v), zero_i}, 3)
                                   : spv_op(b, SpvOpAccessChain, p_in_v4, (uint32_t[]){in_p[k], spv_const_i32(b, (int32_t)v)}, 2);
            vals[k][v] = spv_op(b, SpvOpLoad, t_v4f, &ptr, 1);
        }
    }
    /* the corner with the right angle is the one opposite the longest edge (the
     * diagonal); the strip goes corner, its two neighbours, then the fourth
     * corner = neighbours - corner */
    uint32_t t_bool = spv_type_bool(b), t_v2f = spv_type_vector(b, t_f32, 2), t_v4b = spv_type_vector(b, t_bool, 4);
    uint32_t len[3]; /* squared screen lengths of the edges 12, 02, 01 */
    for (uint32_t e = 0; e < 3; e++) {
        uint32_t i = e == 0 ? 1 : 0, j = e == 2 ? 1 : 2;
        uint32_t d = spv_op(b, SpvOpFSub, t_v4f, (uint32_t[]){vals[32][i], vals[32][j]}, 2);
        uint32_t xy = spv_op(b, SpvOpVectorShuffle, t_v2f, (uint32_t[]){d, d, 0, 1}, 4);
        len[e] = spv_op(b, SpvOpDot, t_f32, (uint32_t[]){xy, xy}, 2);
    }
    uint32_t c1 = spv_op(b, SpvOpLogicalAnd, t_bool,
                         (uint32_t[]){spv_op(b, SpvOpFOrdGreaterThan, t_bool, (uint32_t[]){len[1], len[0]}, 2),
                                      spv_op(b, SpvOpFOrdGreaterThanEqual, t_bool, (uint32_t[]){len[1], len[2]}, 2)},
                         2);
    uint32_t c2 = spv_op(b, SpvOpLogicalAnd, t_bool,
                         (uint32_t[]){spv_op(b, SpvOpFOrdGreaterThan, t_bool, (uint32_t[]){len[2], len[0]}, 2),
                                      spv_op(b, SpvOpFOrdGreaterThan, t_bool, (uint32_t[]){len[2], len[1]}, 2)},
                         2);
    uint32_t c12 = spv_op(b, SpvOpLogicalOr, t_bool, (uint32_t[]){c1, c2}, 2);
    uint32_t v1b = spv_op(b, SpvOpCompositeConstruct, t_v4b, (uint32_t[]){c1, c1, c1, c1}, 4);
    uint32_t v2b = spv_op(b, SpvOpCompositeConstruct, t_v4b, (uint32_t[]){c2, c2, c2, c2}, 4);
    uint32_t v12b = spv_op(b, SpvOpCompositeConstruct, t_v4b, (uint32_t[]){c12, c12, c12, c12}, 4);
    for (uint32_t k = 0; k <= 32; k++) {
        if (k < 32 && !in_p[k]) continue;
        uint32_t *x = vals[k], v0 = x[0], v1 = x[1], v2 = x[2];
        x[0] = spv_op(b, SpvOpSelect, t_v4f, (uint32_t[]){v1b, v1, spv_op(b, SpvOpSelect, t_v4f, (uint32_t[]){v2b, v2, v0}, 3)}, 3);
        x[1] = spv_op(b, SpvOpSelect, t_v4f, (uint32_t[]){v12b, v0, v1}, 3);
        x[2] = spv_op(b, SpvOpSelect, t_v4f, (uint32_t[]){v2b, v1, v2}, 3);
        uint32_t sum = spv_op(b, SpvOpFAdd, t_v4f, (uint32_t[]){x[1], x[2]}, 2);
        x[3] = spv_op(b, SpvOpFSub, t_v4f, (uint32_t[]){sum, x[0]}, 2);
    }
    uint32_t out_pos = spv_op(b, SpvOpAccessChain, p_out_v4, (uint32_t[]){out_pv, zero_i}, 2);
    for (uint32_t v = 0; v < 4; v++) {
        spv_op_void(b, SpvOpStore, (uint32_t[]){out_pos, vals[32][v]}, 2);
        for (uint32_t k = 0; k < 32; k++) {
            if (in_p[k]) spv_op_void(b, SpvOpStore, (uint32_t[]){out_p[k], vals[k][v]}, 2);
        }
        spv_op_void(b, SpvOpEmitVertex, NULL, 0);
    }
    spv_op_void(b, SpvOpEndPrimitive, NULL, 0);
    spv_op_void(b, SpvOpReturn, NULL, 0);
    spv_end_function(b);
    add_name(b, SpvExecutionModelGeometry, fn, iface, n_iface);
    spv_emit(b, SPV_SEC_EXEC_MODE, SpvOpExecutionMode, (uint32_t[]){fn, SpvExecutionModeInputTriangles}, 2);
    spv_emit(b, SPV_SEC_EXEC_MODE, SpvOpExecutionMode, (uint32_t[]){fn, SpvExecutionModeOutputTriangleStrip}, 2);
    spv_emit(b, SPV_SEC_EXEC_MODE, SpvOpExecutionMode, (uint32_t[]){fn, SpvExecutionModeOutputVertices, 4}, 3);
    spv_emit(b, SPV_SEC_EXEC_MODE, SpvOpExecutionMode, (uint32_t[]){fn, SpvExecutionModeInvocations, 1}, 3);
    bool ok = spv_finish(b, spirv, words);
    spv_free(b);
    return ok;
}

#include "gpu/shader/shader_mesh_replay_spirv.h"
