/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Guest shader programs: analysis, per-draw scalar evaluation and
 * translation to SPIR-V.
 *
 * Resource model. Guest shaders build their buffer, texture and sampler
 * descriptors in scalar registers, from user data or from memory. Every
 * memory instruction is a "site". Before each draw the scalar part of the
 * program runs on the CPU (sh_evaluate) with the real user data and guest
 * memory, which yields the concrete descriptor of every site. The
 * translated shader then reads:
 *   - scalar memory (s_load / s_buffer_load): values precomputed by
 *     sh_evaluate, from the "uniforms" buffer (first binding), which also
 *     holds the initial SGPR values;
 *   - vector buffer memory: a storage buffer per site holding a copy of the
 *     guest range the descriptor covers;
 *   - images: a combined image sampler per site.
 * The descriptor properties the code depends on (formats, strides) are part
 * of the pipeline key ("site shape"); addresses and sizes are not.
 *
 * Execution model. One host invocation runs one guest lane. EXEC, VCC and
 * compare results are 64-bit masks in SGPRs where only bit 0 (the lane's own
 * bit) is meaningful; scalar code runs redundantly in every invocation. */
#ifndef PS5C_SHADER_H
#define PS5C_SHADER_H

#include "base/base.h"
#include "gpu/rdna2.h"
#include "mem/gmem.h"

typedef enum sh_stage { SH_STAGE_VS, SH_STAGE_PS, SH_STAGE_CS, SH_STAGE_MESH } sh_stage;

#define SH_MAX_INSTS 32768 /* Large materials exceed 21K; target/next_scalar indices remain uint16_t. */
#define SH_MAX_SITES 4096 /* Large materials exceed 3K memory instructions; backend still checks device limits. */
#define SH_MAX_CODE_DWORDS 0x20000u /* Bounded readable code window, not the instruction count. */
/* Internal capacity, not a device limit. The Vulkan backend checks the real
 * per-type, per-stage and pipeline limits, including its uniforms binding. */
#define SH_MAX_BINDING_SITES SH_MAX_SITES
#define SH_SGPRS 128
#define SH_MAX_FUNCTIONS 16
typedef struct sh_function {
    guest_addr base;
    uint32_t first, count, pc_start, dwords, caller;
} sh_function;

/* Uniforms buffer layout (dwords) */
#define SH_U_SGPR 0            /* initial SGPR values */
#define SH_U_SITE (SH_U_SGPR + SH_SGPRS) /* per-site metadata; buffer_length: actual bound dwords */
/* VS: the guest viewport transform, done in the shader (floats): x scale,
 * x offset, y scale, y offset, 2 / target width, 2 / target height */
#define SH_U_VPORT (SH_U_SITE + SH_MAX_SITES)
/* VS: the user clip planes PA_CL_UCP_0..5 (x, y, z, w floats each) */
#define SH_U_UCP (SH_U_VPORT + 8)
#define SH_U_MESH (SH_U_UCP + 24) /* draw count, base vertex, base instance; remainder reserved */
#define SH_U_DATA (SH_U_MESH + 8)
/* The program's load address (low, high), set by sh_evaluate: s_getpc_b64 adds its offset to
 * it, so the translation does not depend on where the code was loaded and the driver's
 * pipeline cache serves the next run (sh_pc_uniform). */
#define SH_U_PC (SH_U_MESH + 6)

/* Private GPU layout: omit unused site words, preserving 16-byte alignment.
 * CPU metadata and payload offsets always remain logical. The removed gap
 * reads as zero through the last retained padding word. Zero shift is legacy. */
uint32_t sh_uniform_compact_shift(uint32_t site_count);
bool sh_uniform_shift_valid(uint32_t site_count, uint32_t shift);
/* shift must first pass sh_uniform_shift_valid for the program. */
uint32_t sh_uniform_index(uint32_t shift, uint32_t logical);
/* Source/destination must be disjoint or identical. Includes no spare/probe
 * words beyond logical_words; callers append them after the packed result. */
bool sh_uniform_pack(uint32_t *dst, size_t capacity_words, const uint32_t *src,
                     uint32_t logical_words, uint32_t site_count, uint32_t shift);

typedef enum sh_site_kind { SH_SITE_SMEM, SH_SITE_BUFFER, SH_SITE_IMAGE, SH_SITE_SBUFFER, SH_SITE_BVH } sh_site_kind;

/* GDS (the GPU's global data share) lives in guest memory at sh_gds_base:
 * DS instructions with the gds bit are buffer sites over it (M0[15:0] is
 * their base), and the command processor's DMA reaches it there too. */
#define SH_GDS_SIZE 0x10000u
extern guest_addr sh_gds_base;

typedef struct sh_site {
    uint32_t inst; /* instruction index */
    sh_site_kind kind;
    bool store;    /* BUFFER/IMAGE: written by the shader */
    bool pops_load; /* candidate: specialize coherent access using evaluated descriptors */
} sh_site;


typedef struct sh_program {
    guest_addr base;
    const uint32_t *code;
    uint32_t dwords;
    uint32_t eval_sgpr_lo; /* SGPRs 0..31 read by scalar/uniform evaluation */
    uint64_t hash;
    uint8_t aperture_flags;
    uint16_t eval_sgpr_hi; /* SGPRs 32..47 read by scalar/uniform evaluation */
    /* Of those, SGPRs read only as word 0 (the base address) of S_BUFFER_LOAD
     * descriptors and never written: another draw with the same constants at
     * another address evaluates the same way (the renderer reuses it). */
    uint64_t eval_reloc_sgprs;
    /* Finite linked code ranges. pc[] remains a unique virtual offset;
     * GETPC and return links use the original guest addresses. */
    uint32_t function_count;
    sh_function functions[SH_MAX_FUNCTIONS];
    rdna2_inst insts[SH_MAX_INSTS];
    uint16_t ops[SH_MAX_INSTS]; /* sh_op per instruction */
    uint32_t pc[SH_MAX_INSTS];  /* dword offset of each instruction */
    uint32_t count;
    sh_site sites[SH_MAX_SITES];
    uint32_t site_count;
    int16_t site_of[SH_MAX_INSTS]; /* site index per instruction, -1 if none */
    /* scalar evaluation: branch target index per instruction, and the next
     * instruction at or after each one that the evaluation looks at (vector
     * ALU, LDS and export instructions are skipped) */
    uint16_t target[SH_MAX_INSTS];
    uint16_t next_scalar[SH_MAX_INSTS + 1];
    uint32_t param_mask; /* VS: exported parameters */
    uint32_t mrt_mask;   /* PS: exported color targets; bit 8: depth (mrtz) */
    uint32_t attr_mask;  /* PS: interpolated attributes read */
    bool has_stores;
    /* wave32 code: compares write VCC_LO / EXEC_LO only, and the compiler keeps
     * scalar temporaries in VCC_HI */
    bool wave32;
    /* per instruction, from its name (sh_program_flags): SH_IF_* bits, and the
     * VGPRs a memory instruction may write from vdata on (16: unknown) */
    uint8_t iflags[SH_MAX_INSTS];
    uint8_t vmem_width[SH_MAX_INSTS];
    bool has_flow; /* branches, s_setpc/s_swappc or movrel */
    uint32_t eval_forward_branches; /* bounded reference fork storage, counted during analysis */
    bool back_branch; /* a branch to an earlier instruction (a loop) */
    bool lane_reads; /* readlane/readfirstlane of a VGPR: follow uniform VGPR writes and scalar spills */
} sh_program;

enum {
    SH_IF_64 = 1,       /* 64-bit destination (b64/i64/u64 in the name) */
    SH_IF_CARRY = 2,    /* writes a carry mask (co_ in the name) */
    SH_IF_SAVEEXEC = 4, /* saveexec */
    SH_IF_FLOW = 8,     /* branch, setpc/swappc or movrel */
    SH_IF_JOIN = 16,    /* branch target join point */
    SH_IF_HAS_SWAPPC = 32, /* program contains s_swappc_b64 */
    SH_IF_ANALYZED = 64,   /* sh_program_flags has populated iflags */
};

/* Fills iflags, vmem_width and has_flow from the decoded instructions (sh_analyze does it). */
void sh_program_flags(sh_program *p);
/* MOV-only aperture queries; generic FLAT DWORD routing requires a second opt-in. */
bool sh_aperture_allowed(const sh_program *p);
bool sh_flat_apertures(const sh_program *p);

/* Decodes the program at `code` (at most `max_dwords`), loaded at guest `base`.
 * False (with a log line) on instructions the translator does not know. */
bool sh_analyze(const uint32_t *code, uint32_t max_dwords, guest_addr base, sh_program *p);
bool sh_append_function(sh_program *p, const uint32_t *code, uint32_t max_dwords, guest_addr base, uint32_t caller);
guest_addr sh_inst_address(const sh_program *p, uint32_t i);
/* s_getpc_b64 reads SH_U_PC (one piece of code, no calls or jumps by address): the
 * translation then holds no address. PS5C_PC_CONSTANT=1 keeps it a constant. */
bool sh_pc_uniform(const sh_program *p);
/* Legal indirect destinations: function entries and call continuations. */
uint32_t sh_call_destination(const sh_program *p, guest_addr addr, uint32_t caller);
/* Merged-stage programs (NGG ES+GS) can come in two halves: the first ends in
 * s_setpc_b64 s[6:7], a jump to the second (SPI_SHADER_PGM_LO_GS). The first
 * half's length in dwords, the jump included; 0 when the code is not one. */
uint32_t sh_front_dwords(const uint32_t *code, uint32_t max_dwords);
/* The device lets vertex shaders write the target layer (gl_Layer). */
void sh_set_vs_layer(bool on);
/* gl_Layer readable by pixel shaders (the Geometry capability: geometryShader enabled) */
void sh_set_ps_layer(bool on);

/* Per-draw values of one site */
typedef struct sh_site_value {
    bool valid;
    uint32_t desc[8];    /* BUFFER: V# (4 dwords); IMAGE: T# (8) */
    uint32_t sampler[4]; /* IMAGE: S# */
    uint32_t smem_offset; /* SMEM: dword offset of the values in the uniforms buffer */
    /* BUFFER: the scalar offset (soffset) of the access, when every evaluated
     * path reaching it agrees on a value that does not depend on vector data */
    uint32_t soffset;
    bool soffset_known;
    /* BUFFER/BVH: the descriptor came from vector data, or evaluated BVH
     * paths disagree: the scalar evaluation only has a stand-in. */
    bool desc_unknown;
    /* BUFFER: only NUM_RECORDS came from vector data (a count the shader
     * loaded); the shader checks the real one, the binding covers the mapping */
    bool records_unknown;
} sh_site_value;

typedef struct sh_eval {
    uint32_t sgpr_init[SH_SGPRS];
    sh_site_value site[SH_MAX_SITES];
    uint32_t *smem_data; /* uniforms buffer contents; sh_eval_free releases */
    uint32_t site_dirty; /* highest SH_U_SITE slot written since allocation */
    uint32_t smem_words, smem_cap;
    /* optional: host pointer of mapped guest memory, NULL if not mapped (the
     * renderer answers from ranges it already checked); NULL: gmem_host */
    void *(*host)(void *user, guest_addr addr, uint64_t size);
    void *host_user;
    /* optional: true when an S_BUFFER_LOAD range holds results of earlier GPU work
     * not yet back in guest memory. The translated shader reads such loads from its
     * bound buffer, so the evaluation goes on with unknown values instead of waiting
     * (deferred); guessed: a value from such a load set a descriptor, a scalar
     * load address, a global base or a branch.
     * The caller evaluates again without `defer` when the guess could matter. */
    bool (*defer)(void *user, guest_addr addr, uint64_t size);
    bool deferred, guessed;
    /* optional (sh_store_range): per instruction and source operand, the value
     * of an SGPR source of a vector instruction; vsrc_state 1 = known (every
     * evaluated path agrees), 2 = unknown, 0 = not reached. SH_MAX_INSTS each. */
    uint32_t (*vsrc)[3];
    uint8_t (*vsrc_state)[3];
    /* compute: the workgroup id SGPRs (first, count), which differ between the
     * groups of one dispatch; sgpr_init holds group 0 */
    uint8_t group_sgpr, group_sgprs;
    /* Unresolved scalar call: never skip it. The renderer may provide a
     * bounded callee and restart evaluation before translating the program. */
    bool call_pending, call_known;
    uint32_t call_inst;
    guest_addr call_target;
    /* during a `host` read: the base SGPR of the S_BUFFER_LOAD reading, else 0xFF */
    uint8_t host_sbase;
} sh_eval;

/* Runs the scalar part of `p` from e->sgpr_init; fills the sites and the
 * uniforms buffer image (e->smem_data, SH_U_DATA dwords of header first). */
bool sh_evaluate(const sh_program *p, gmem *mem, sh_eval *e);
void sh_eval_free(sh_eval *e);

/* Shape of a site: what the translated code depends on */
typedef struct sh_site_shape {
    uint32_t buf_stride; /* address_map=2: number of compact read-only ranges */
    uint8_t buf_format; /* BUF_FMT_* (GFX10) */
    uint8_t dst_sel[4];
    uint8_t img_type;   /* image descriptor TYPE */
    uint8_t img_kind;   /* numeric kind: 0 float, 1 uint, 2 sint */
    uint8_t cmp_func;   /* image_sample_c: sampler DEPTH_COMPARE_FUNC */
    uint8_t ordered_load; /* POPS load proven to use the same descriptor as a storage write */
    uint8_t address_map; /* buffer 1: window, 2: compact ranges; BVH 3: checked VA/device-address table; image: live T#/S# guard */
    uint8_t cmp_filter; /* comparison sample: bit 0 linear mag, bit 1 linear min */
    /* Site kinds are disjoint: share the old padding byte without changing
     * the 16-byte corpus/cache shape ABI. Read either member only for its kind. */
    union {
        uint8_t img_components; /* image: physical uncompressed channels (0: unknown) */
        uint8_t buffer_length; /* buffer: what SH_U_SITE holds (SH_BUFFER_LENGTH, SH_BUFFER_SHIFTED) */
    };
} sh_site_shape;

#define SH_BUFFER_LENGTH 1  /* the actual bound dwords (opt-in) */
#define SH_BUFFER_SHIFTED 2 /* bytes from the binding's start to the descriptor base, added to every offset */

static inline bool sh_site_storage(const sh_site *site, const sh_site_shape *shape) {
    return site->store || shape->ordered_load;
}

void sh_site_shape_of(const sh_program *p, const sh_eval *e, uint32_t site, sh_site_shape *out);

/* buf_format of an image site written as IMG_FMT 10_11_11_FLOAT: hosts may not write that packed
 * format through a storage image without a declared format (the Iris Xe wrote garbage), so the
 * translation packs R11 G11 B10 itself into an R32_UINT view the renderer binds. */
#define SH_IMG_PACKED_R11G11B10 36
#define SH_IMG_ATOMIC_FLOAT32 254 /* float min/max through a raw R32_UINT CAS view */
#define SH_IMG_ATOMIC_UINT32 253 /* integer UMIN/UMAX over the bits of an R32_FLOAT resource */
#define SH_IMG_ATOMIC_UINT64 252 /* 64-bit unsigned min/max over an R32G32_UINT texel */

/* Image sites the shader addresses by layer, as a 2D array: sampled cube and array
 * descriptors; written (or atomically updated) ones when the instruction's dim has
 * a slice too (2D array or cube: Sonic Frontiers writes its environment cube maps
 * face by face). Others are plain 2D (their descriptor's first layer). */
static inline bool sh_site_arrayed(const sh_program *p, uint32_t site, uint32_t img_type) {
    bool array_type = img_type == 11 || img_type == 12 || img_type == 13 || img_type == 15;
    if (!array_type) return false;
    if (!p->sites[site].store) return true;
    uint32_t dim = p->insts[p->sites[site].inst].dim;
    return (img_type == 11 || img_type == 13) && (dim == 3 || dim == 5); /* CUBE, 2D_ARRAY */
}

/* Stage-specific inputs of the translation */
typedef struct sh_stage_info {
    sh_stage stage;
    /* VS */
    uint32_t vertex_id_vgpr, instance_id_vgpr;
    /* PS: packed SPI_PS_INPUT_CNTL: bits 0..4 = parameter location,
     * bit 5 = DEFAULT_VAL instead of parameter memory, bits 6..7 = DEFAULT_VAL.
     * Keeping the byte layout preserves old corpus files (locations 0..31). */
    uint8_t ps_attr_location[32];
    uint32_t ps_flat_mask;
    /* CS */
    uint32_t local_size[3];
    uint32_t cs_tgid_sgpr;     /* first workgroup id SGPR */
    uint8_t cs_tgid_enable;    /* bit per dimension; bit 3: TG_SIZE_EN (an SGPR after the ids: wave count, wave index << 6) */
    uint8_t cs_tid_components; /* local id VGPRs: 1..3 */
    /* PS: SPI_PS_INPUT_ADDR, the layout of the input VGPRs (barycentrics, then the
     * position, face, ...); 0 = none (fits the padding: corpus files keep their size) */
    bool ps_vertex_fetch; /* native per-vertex fragment inputs available */
    uint8_t cs_wave_mode; /* opt-in: 0 legacy, 1 paired64, 2 native32; replaces padding */
    uint16_t ps_input_addr;
    bool barrier_prefix; /* opt-in structured LDS prefix before a CS/NGG dispatch loop; padding */
    bool wave_barriers; /* opt-in uniform CS/NGG LDS phases around PC loops; padding */
    uint32_t lds_dwords;       /* local data share of a workgroup (0: unknown) */
    /* VS: PA_CL_VS_OUT_CNTL USE_VTX_RENDER_TARGET_INDX: pos1.z[10:0] is the target layer */
    uint32_t vs_layer;
    /* PS: SPI_SHADER_COL_FORMAT, four bits per MRT; numeric output kind
     * uses two bits per MRT (0 float/normalized, 1 unsigned, 2 signed). */
    uint32_t ps_col_format, ps_mrt_kind;
    uint32_t ps_mrt_rb_swap; /* bit per MRT: BGRA storage without a native BGRA view */
    uint32_t fp16_overflow; /* RSRC1 FP16_OVFL: saturate computed f16 overflow */
    /* Single sample: disabled positions of the 2x2 pixel quad (zero: all
     * enabled). SH_PS_QUAD_DEMOTE records the enabled host helper feature. */
    uint32_t ps_quad_disable;
    /* VS of a point list: gl_PointSize as float bits (PA_SU_POINT_SIZE); 0 for other primitives */
    uint32_t vs_point_size;
    /* VS: PA_CL_CLIP_CNTL UCP_ENA [5:0], the user clip planes it clips against (gl_ClipDistance) */
    uint32_t vs_ucp_mask;
    /* CS: physical Vulkan group shape; zeros keep local_size. Guest IDs and
     * wave ordering still use local_size when a host axis has a lower limit. */
    uint32_t host_local_size[3];
    /* VS: duplicate a parameter into an unused host location when the PS reads
     * the same guest parameter both flat and interpolated. Vulkan requires
     * equal interpolation decorations for all inputs at one location. */
    uint32_t vs_param_alias_mask;
    uint8_t vs_param_alias[32];
    /* NGG, one complete wave64 per host workgroup. Inputs are partitioned
     * so all generated vertices fit that wave. Zero primitive size is the
     * original point-list contract (one); indexed inputs use a staged table. */
    uint32_t mesh_input_vertices, mesh_max_vertices, mesh_max_primitives;
    uint32_t mesh_primitive_vertices;
    uint32_t mesh_indexed; /* bit0 index fetch, bit1 strip, bit2 ballot, bit3 fast launch, bit4 leaf inlining (CS too) */
    uint32_t mesh_compute, mesh_output_binding; /* experimental full GS -> storage -> vertex replay */
    uint32_t uniform_shift; /* actual omitted GPU words; logical CPU offsets unchanged (corpus append) */
} sh_stage_info;

#define SH_MESH_BALLOT32 4u /* fixed32 host subgroups; part of stage/cache/corpus identity */
#define SH_MESH_FAST_LAUNCH 8u /* one point/group, native guest wave32; compute/replay */
#define SH_LEAF_CALL_INLINE 16u /* opt-in wave32 leaf inlining; stage/cache/corpus identity */
#define SH_JOIN_PREFIX 32u /* opt-in acyclic first-barrier prefix; stage/cache/corpus identity */
#define SH_JOIN_SUFFIX 64u /* emit a proven forward-only suffix directly after that barrier */
#define SH_MESH_NGG_CULL 128u /* NGG no-GS culling ABI: three 16-bit input vertex indices */

/* Compact output buffer: eight header dwords, per-vertex position/parameters,
 * then packed primitive words. Header starts with VkDrawIndirectCommand. */
uint32_t sh_mesh_output_words(const sh_program *p,const sh_stage_info *info);
bool sh_mesh_replay(const sh_program *p,const sh_stage_info *info,uint32_t **spirv,size_t *words);

/* Link the two stage interfaces, relocating mixed-interpolation PS aliases
 * and recording matching VS exports. False if no free parameter location. */
bool sh_link_graphics_inputs(const sh_program *vs, const sh_program *ps,
                             sh_stage_info *vs_info, sh_stage_info *ps_info);

static inline bool sh_inst_uses_lds(const rdna2_inst *in) {
    /* Permutes use the LDS crossbar, not storage. GDS is a separate buffer. */
    return in->enc == RDNA2_ENC_DS && !in->gds && in->op != 0x14 && in->op != 0x35 && in->op != 0xb3;
}

#define SH_PS_QUAD_DEMOTE 0x100u
static inline uint32_t sh_ps_quad_disabled(uint32_t lo, uint32_t hi) {
    uint32_t enabled = (lo & 1u) | ((lo >> 15) & 2u) | ((hi << 2) & 4u) | ((hi >> 13) & 8u);
    return enabled ^ 15u;
}

/* Our VS executes the merged GS stage. Bit positions match AnyPS5's
 * RegisteredFloatMode (bf6e25a1); do not use the compute bit for graphics. */
static inline uint32_t sh_fp16_overflow_bit(sh_stage stage, uint32_t rsrc1) {
    return (rsrc1 >> (stage == SH_STAGE_CS ? 26 : stage == SH_STAGE_PS ? 29 : 31)) & 1u;
}

/* GFX10.3 SPI_SHADER_PGM_RSRC2_GS: LDS_SIZE [26:19], units of 128 dwords.
 * Bit 27 is USER_SGPR_MSB, not part of the LDS allocation. */
static inline uint32_t sh_graphics_lds_dwords(uint32_t rsrc2) {
    return ((rsrc2 >> 19) & 0xFFu) * 128u;
}

/* A 2D load/store mip address proven to be an unmodified literal or initial
 * user SGPR. Only a small straight-line copy program is recognized. The
 * renderer binds that mip as a one-level image; the translator uses LOD 0.
 * source == RDNA2_OP_LITERAL selects literal, otherwise an initial SGPR. */
bool sh_image_mip_source(const sh_program *p, const sh_stage_info *info, uint32_t site,
                         uint32_t *source, uint32_t *literal);
/* Select the descriptor's relative mip, preserving its full layout/last level.
 * False for out-of-range levels or dimensions not represented by 2D images. */
bool sh_image_mip_desc(const uint32_t desc[8], uint32_t lod, uint32_t out[8]);

static inline uint8_t sh_ps_input_pack(uint32_t control) {
    /* OFFSET[5] with FLAT_SHADE selects raw P0/P1/P2, not DEFAULT_VAL.
     * AMD CIK/SI SPI_PS_INPUT_CNTL: passthrough parameter-cache mode. */
    if((control&0x420u)==0x420u)return (uint8_t)(control&31u);
    return (uint8_t)((control & 0x3Fu) | ((control >> 2) & 0xC0u));
}

/* Byte range [*lo, *hi) relative to the descriptor base that buffer site
 * `site` of a compute shader may write, for a dispatch of `groups` workgroups
 * (desc: the site's V#, its stride in desc[1]). The index comes from the
 * local and workgroup ids through simple integer arithmetic, with the scalar
 * operands of the evaluation (e->vsrc). False when that is not proven: loops,
 * unconditional jumps, writes that may widen EXEC, unknown operations. */
bool sh_store_range(const sh_program *p, const sh_eval *e, const sh_stage_info *info, const uint32_t groups[3],
                    uint32_t site, uint64_t *lo, uint64_t *hi);

/* Translation environment is configured before a session, not per draw.
 * Use the effective flags both in SPIR-V generation and cache identity. */
enum {
    SH_TRANSLATE_EXACT = 1u,
    SH_TRANSLATE_REAL_LANES = 2u,
    SH_TRANSLATE_NO_BARRIER = 4u,
    SH_TRANSLATE_NO_STRUCTURED = 8u,
    SH_TRANSLATE_RT_SCALE = 16u
};
uint32_t sh_translation_flags(sh_stage stage);
/* Translations for render targets drawn smaller than the guest's size (set before any) */
void sh_set_rt_scale(bool on);
bool sh_translation_wave64_lds(uint64_t hash);
/* Automatic single-wave translation also needs 66 extra LDS dwords. */
bool sh_compute_auto_wave64(const sh_program *p, const sh_stage_info *info);

/* Bindings (set 0): the uniforms buffer at `binding_base`, then one binding
 * per BUFFER/IMAGE site in site order. Returns the SPIR-V module. */
bool sh_translate(const sh_program *p, const sh_stage_info *info, const sh_site_shape *shapes, uint32_t binding_base,
                  uint32_t **spirv, size_t *words);
/* Bindings a stage uses (uniforms included). */
uint32_t sh_binding_count(const sh_program *p);
uint32_t sh_binding_count_shapes(const sh_program *p, const sh_site_shape *shapes);

/* Translations declare that Inf, NaN and signed zeros are preserved
 * (SPV_KHR_float_controls): guest code relies on them, as in sign(x) computed
 * as clamp(x * inf). Only when the device supports it. */
void sh_set_preserve_inf_nan(bool on);
/* Set once with the Vulkan device, before starting pipeline workers. */
void sh_set_buffer_atomic64(bool on);
void sh_set_image_atomic64(bool on);
bool sh_image_atomic64_supported(void);
void sh_set_lds_atomic64(bool on);
/* Native ordered pixel interlock; POPS translation must reject when unavailable. */
void sh_set_pixel_interlock(bool on);

/* Rectangle-list expansion: geometry shader turning each 3-vertex primitive
 * into a quad, forwarding the parameters in `param_mask`. */
bool sh_rectlist_gs(uint32_t param_mask, uint32_t **spirv, size_t *words);

/* Buffer format table: components, bits per component and number kind. */
typedef struct sh_buf_format {
    uint8_t comps;
    uint8_t bits[4];
    uint8_t kind; /* 0 unorm, 1 snorm, 2 uscaled, 3 sscaled, 4 uint, 5 sint, 6 float */
} sh_buf_format;
bool sh_buffer_format(uint32_t fmt, sh_buf_format *out);

/* Value of an inline-constant operand code (128..208, 240..248, 255 = literal). */
bool sh_inline_const(uint32_t code, uint32_t literal, uint32_t *out);

#endif
