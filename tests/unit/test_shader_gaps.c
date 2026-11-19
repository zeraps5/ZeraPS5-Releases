/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Instructions the shader corpus found untranslated: LDS spills in pixel
 * shaders, image atomics, 64-bit scalar bit ops, packed/literal f16 FMAs and
 * ds_swizzle. Each program must analyze and translate to SPIR-V that uses the
 * expected instructions. Hand-encoded programs (checked with `ps5c disasm`). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/shader/shader.h"
#include "gpu/shader/shader_ops.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

enum { OP_VARIABLE = 59, OP_IMAGE_TEXEL_POINTER = 60, OP_ATOMIC_IADD = 234, OP_SHUFFLE = 345 };
enum { STORAGE_WORKGROUP = 4 };

/* count of `opcode` instructions; with `storage`, OpVariables of that storage class only */
static uint32_t count_op(const uint32_t *spv, size_t words, uint32_t opcode, int storage) {
    uint32_t n = 0;
    for (size_t i = 5; i < words;) {
        uint32_t len = spv[i] >> 16;
        if ((spv[i] & 0xFFFF) == opcode && (storage < 0 || (len > 3 && spv[i + 3] == (uint32_t)storage))) n++;
        i += len ? len : 1;
    }
    return n;
}

/* PS5C_TEST_SPIRV=<dir>: the modules are also written there (for spirv-val --target-env vulkan1.1) */
static bool translate(const uint32_t *code, uint32_t dwords, const sh_stage_info *info, const sh_site_shape *shapes,
                      sh_program *p, uint32_t **spv, size_t *words) {
    static int n;
    *spv = NULL;
    *words = 0;
    if (!sh_analyze(code, dwords, 0x100000, p) || !sh_translate(p, info, shapes, 0, spv, words)) return false;
    const char *dir = getenv("PS5C_TEST_SPIRV");
    if (dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/gaps%d.spv", dir, n++);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(*spv, 4, *words, f);
            fclose(f);
        }
    }
    return true;
}

int main(void) {
    sh_program *p = calloc(1, sizeof(*p));
    sh_site_shape shapes[4];
    sh_stage_info ps, cs;
    uint32_t *spv;
    size_t words;
    CHECK(p);
    memset(shapes, 0, sizeof(shapes));
    memset(&ps, 0, sizeof(ps));
    memset(&cs, 0, sizeof(cs));
    ps.stage = SH_STAGE_PS;
    cs.stage = SH_STAGE_CS;
    cs.local_size[0] = 64;
    cs.local_size[1] = cs.local_size[2] = 1;
    cs.cs_tid_components = 1;

    /* Mesa's NGG vertex shaders spill subword data into invocation-private
     * LDS. Atomic operations on Private pointers violate SPIR-V rules. The
     * same DS operations on compute Workgroup memory must remain atomic. */
    static const uint32_t subword[]={0xd8780000u,0x00000100u,0xd87c0002u,0x00000100u,0xbf810000u};
    sh_stage_info private_vs={.stage=SH_STAGE_VS,.lds_dwords=1024};
    CHECK(translate(subword,PS5C_ARRAY_COUNT(subword),&private_vs,shapes,p,&spv,&words));
    CHECK(count_op(spv,words,240,-1)==0 && count_op(spv,words,241,-1)==0);
    free(spv);
    CHECK(translate(subword,PS5C_ARRAY_COUNT(subword),&cs,shapes,p,&spv,&words));
    CHECK(count_op(spv,words,240,-1)==2 && count_op(spv,words,241,-1)==2);
    free(spv);

    /* pixel shader: a register spilled to LDS by lane and read back */
    static const uint32_t spill[] = {
        0xB07C0000u,              /* s_movk_i32 m0, 0 */
        0xDAC00300u, 0x00000700u, /* ds_write_addtid_b32 v7 offset:768 */
        0xDAC40300u, 0x07000000u, /* ds_read_addtid_b32 v7 offset:768 */
        0xF800180Fu, 0x07070707u, /* exp mrt0 v7, v7, v7, v7 done vm */
        0xBF810000u,              /* s_endpgm */
    };
    CHECK(translate(spill, PS5C_ARRAY_COUNT(spill), &ps, shapes, p, &spv, &words));
    CHECK(count_op(spv, words, OP_VARIABLE, STORAGE_WORKGROUP) == 0); /* no LDS array outside compute */
    free(spv);

    /* A material PS can spill more than sixteen registers. Keep every byte
     * offset distinct, including the end of the 16-bit immediate range. */
    uint32_t many_spills[2+32*4+3]={0xB07C0000u};
    uint32_t at=1;
    for(uint32_t i=0;i<32;i++) {
        uint32_t off=i==31?65532u:i*256u;
        many_spills[at++]=0xDAC00000u|off;
        many_spills[at++]=0x00000700u;
    }
    for(uint32_t i=32;i-->0;) {
        uint32_t off=i==31?65532u:i*256u;
        many_spills[at++]=0xDAC40000u|off;
        many_spills[at++]=0x07000000u;
    }
    many_spills[at++]=0xF800180Fu;many_spills[at++]=0x07070707u;
    many_spills[at++]=0xBF810000u;
    CHECK(translate(many_spills,at,&ps,shapes,p,&spv,&words));
    CHECK(count_op(spv,words,OP_VARIABLE,STORAGE_WORKGROUP)==0);
    CHECK(count_op(spv,words,OP_VARIABLE,7 /* Function */)>=32);
    free(spv);

    /* image_atomic_add with glc on a 32-bit unsigned storage image */
    static const uint32_t atomic[] = {
        0xF0442108u, 0x00000200u, /* image_atomic_add v2, v[0:1], s[0:7] dmask:0x1 dim:2D glc */
        0xBF810000u,
    };
    shapes[0].img_kind = 1;
    shapes[0].img_type = 9; /* 2D */
    CHECK(translate(atomic, PS5C_ARRAY_COUNT(atomic), &cs, shapes, p, &spv, &words));
    CHECK(count_op(spv, words, OP_IMAGE_TEXEL_POINTER, -1) == 1 && count_op(spv, words, OP_ATOMIC_IADD, -1) == 1);
    free(spv);
    shapes[0].img_kind = 0; /* float images have no integer atomics */
    CHECK(!translate(atomic, PS5C_ARRAY_COUNT(atomic), &cs, shapes, p, &spv, &words));
    free(spv);
    memset(shapes, 0, sizeof(shapes));

    /* scalar 64-bit bit ops, f16 FMAs and a lane swizzle */
    static const uint32_t misc[] = {
        0xBE841602u,              /* s_flbit_i32_b64 s4, s[2:3] */
        0xBE843B02u,              /* s_bitreplicate_b64_b32 s[4:5], s2 */
        0x78020702u,              /* v_pk_fmac_f16 v1, v2, v3 */
        0x6E020702u, 0x00003C00u, /* v_fmamk_f16 v1, v2, 1.0, v3 */
        0xD8D4041Fu, 0x01000000u, /* ds_swizzle_b32 v1, v0 offset:0x41f (lane ^ 1) */
        0xBF810000u,
    };
    CHECK(translate(misc, PS5C_ARRAY_COUNT(misc), &cs, shapes, p, &spv, &words));
    /* EXEC masking may require another shuffle. The DS_SWIZZLE GPU fixture
     * checks actual lane values rather than counting implementation details. */
    CHECK(count_op(spv, words, OP_SHUFFLE, -1) >= 1);
    free(spv);

    /* ds_bpermute_b32 outside compute: a subgroup shuffle, no LDS array */
    static const uint32_t bperm_ps[] = {
        0xDACC0000u, 0x01000200u, /* ds_bpermute_b32 v1, v0, v2 */
        0xF800180Fu, 0x01010101u, /* exp mrt0 v1, v1, v1, v1 done vm */
        0xBF810000u,
    };
    CHECK(translate(bperm_ps, PS5C_ARRAY_COUNT(bperm_ps), &ps, shapes, p, &spv, &words));
    CHECK(count_op(spv, words, OP_SHUFFLE, -1) == 1 && count_op(spv, words, OP_VARIABLE, STORAGE_WORKGROUP) == 0);
    free(spv);
    sh_stage_info vs;
    memset(&vs, 0, sizeof(vs));
    vs.stage = SH_STAGE_VS;
    static const uint32_t bperm_vs[] = {
        0xDACC0004u, 0x01000200u, /* ds_bpermute_b32 v1, v0, v2 offset:4 */
        0xF80008CFu, 0x01010101u, /* exp pos0 v1, v1, v1, v1 done */
        0xBF810000u,
    };
    CHECK(translate(bperm_vs, PS5C_ARRAY_COUNT(bperm_vs), &vs, shapes, p, &spv, &words));
    CHECK(count_op(spv, words, OP_SHUFFLE, -1) == 1 && count_op(spv, words, OP_VARIABLE, STORAGE_WORKGROUP) == 0);
    free(spv);

    /* s_setpc_b64 to an address computed from s_getpc_b64: a jump over the v_mov */
    static const uint32_t jump[] = {
        0xBE801F00u,              /* s_getpc_b64 s[0:1] (the address of the next instruction: dword 1) */
        0x8000FF00u, 0x00000014u, /* s_add_u32 s0, s0, 20 (literal) -> dword 6 */
        0x82018001u,              /* s_addc_u32 s1, s1, 0 */
        0xBE802000u,              /* s_setpc_b64 s[0:1] */
        0x7E0202F2u,              /* v_mov_b32 v1, 1.0 (skipped) */
        0xBF810000u,              /* s_endpgm */
    };
    CHECK(translate(jump, PS5C_ARRAY_COUNT(jump), &cs, shapes, p, &spv, &words));
    CHECK(p->count == 6 && p->ops[3] == SH_OP_S_BRANCH && p->target[3] == 5);
    CHECK(p->insts[3].simm16 == 1 && p->has_flow);
    free(spv);
    /* the same with s_sub_u32 / s_subb_u32 and an unrelated instruction between: a jump back to the v_mov */
    static const uint32_t back[] = {
        0x7E0202F2u,              /* v_mov_b32 v1, 1.0 */
        0xBF880007u,              /* s_cbranch_execz 7 -> s_endpgm */
        0xBE801F00u,              /* s_getpc_b64 s[0:1] (= dword 3) */
        0x8080FF00u, 0x0000000Cu, /* s_sub_u32 s0, s0, 12 -> dword 0 */
        0xBF060302u,              /* s_cmp_eq_u32 s2, s3 */
        0x8281FF01u, 0x00000000u, /* s_subb_u32 s1, s1, 0 (literal) */
        0xBE802000u,              /* s_setpc_b64 s[0:1] */
        0xBF810000u,
    };
    /* the compare replaces SCC, the borrow s_subb_u32 needs: rejected */
    CHECK(!translate(back, PS5C_ARRAY_COUNT(back), &cs, shapes, p, &spv, &words));
    static const uint32_t back2[] = {
        0x7E0202F2u,              /* v_mov_b32 v1, 1.0 */
        0xBF880007u,              /* s_cbranch_execz 7 -> s_endpgm */
        0xBE801F00u,              /* s_getpc_b64 s[0:1] (= dword 3) */
        0x7E0402F2u,              /* v_mov_b32 v2, 1.0 */
        0x8080FF00u, 0x0000000Cu, /* s_sub_u32 s0, s0, 12 -> dword 0 */
        0x8281FF01u, 0x00000000u, /* s_subb_u32 s1, s1, 0 (literal) */
        0xBE802000u,              /* s_setpc_b64 s[0:1] */
        0xBF810000u,
    };
    CHECK(translate(back2, PS5C_ARRAY_COUNT(back2), &cs, shapes, p, &spv, &words));
    CHECK(p->ops[6] == SH_OP_S_BRANCH && p->target[6] == 0 && p->back_branch);
    free(spv);
    /* unknown targets still fail: a return through s[6:7], a pair overwritten before the jump */
    static const uint32_t ret[] = {0xBE802006u, 0xBF810000u}; /* s_setpc_b64 s[6:7] */
    CHECK(!translate(ret, PS5C_ARRAY_COUNT(ret), &cs, shapes, p, &spv, &words));
    static const uint32_t clobber[] = {
        0xBE801F00u,              /* s_getpc_b64 s[0:1] */
        0x8000FF00u, 0x00000010u, /* s_add_u32 s0, s0, 16 */
        0xBE800380u,              /* s_mov_b32 s0, 0 */
        0xBE802000u,              /* s_setpc_b64 s[0:1] */
        0xBF810000u,
    };
    CHECK(!translate(clobber, PS5C_ARRAY_COUNT(clobber), &cs, shapes, p, &spv, &words));

    free(p);
    printf("shader gaps: ok\n");
    return 0;
}
