/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GCN (PS4) shaders re-encoded as RDNA2: Cloudberry Kingdom's sprite VS (with its fetch shader)
 * and a lit PS, checked instruction by instruction through the RDNA2 decoder. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu/gcn.h"
#include "gpu/rdna2.h"
#include "test.h"

static const uint32_t k_vs[] = {
    0xbeeb03ffu, 0x00000017u, 0xbe802100u, 0x7e000280u, 0xc2c40500u, 0xc2400508u, 0x7e0202f2u, 0xbf8c007fu,
    0x7e06020eu, 0x7e04020fu, 0xd23e0003u, 0x2002080cu, 0x7e08020cu, 0x3e06080eu, 0x7e08020du, 0xd23e0002u,
    0x20020a0du, 0x0a060608u, 0x7e0a5400u, 0x3e04080fu, 0x10060705u, 0x0a080409u, 0x1004060au, 0x1006080bu,
    0xf80008cfu, 0x01000302u, 0x100c1801u, 0x10081c01u, 0x100a1a01u, 0xf800022fu, 0x0f040506u, 0xf800021fu,
    0x00000908u, 0xf800020fu, 0x00000302u, 0xbf810000u,
};
static const uint32_t k_fetch[] = {
    0xc0840300u, 0xc0860304u, 0xc0880308u, 0xbf8c007fu, 0xe0042000u, 0x80020400u,
    0xe0042000u, 0x80030800u, 0xe00c2000u, 0x80040c00u, 0xbf8c0000u, 0xbe802000u,
};
static const uint32_t k_ps[] = {
    0xbeeb03ffu, 0x00000022u, 0xbefc0310u, 0xbe90047eu, 0xbefe0a7eu, 0xc8080000u, 0xc80c0100u, 0xc8090001u,
    0xc80d0101u, 0xf0800f00u, 0x00400202u, 0xc8180400u, 0xc8240500u, 0xc81c0600u, 0xc8200700u, 0xc8190401u,
    0xc8250501u, 0xc81d0601u, 0xc8210701u, 0xbf8c0f70u, 0x20000902u, 0x060000ffu, 0x3ccccccdu, 0x7c080103u,
    0x100c0d02u, 0x10020709u, 0x10000907u, 0x10040b08u, 0xbeea246au, 0xbf880018u, 0xc3000d0cu, 0xbf8c007fu,
    0x100a0c00u, 0x100e0c04u, 0x10060c08u, 0x100c0c0cu, 0xd2820004u, 0x04160201u, 0x3e0e0205u, 0x3e060209u,
    0x3e0c020du, 0xd2820001u, 0x041e0006u, 0xd2820007u, 0x041a000eu, 0x3e080002u, 0x3e06000au, 0xd2820006u,
    0x04120403u, 0xd2820000u, 0x040e040bu, 0x3e020407u, 0xd2820002u, 0x041e040fu, 0xbefe046au, 0x5e020306u,
    0x5e000500u, 0xbefe0410u, 0xf8001c0fu, 0x00000001u, 0xbf810000u,
};

/* Disassembles out[0..n) into lines; false if an instruction does not decode */
static bool listing(const uint32_t *out, uint32_t n, char lines[][96], uint32_t *count, rdna2_inst *insts) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; k++) {
        rdna2_inst in;
        if (!rdna2_decode(out + i, n - i, &in)) {
            printf("    no RDNA2 instruction at %u: %08x\n", i, out[i]);
            return false;
        }
        if (!rdna2_disasm(&in, lines[k], 96)) snprintf(lines[k], 96, "%s", in.name);
        insts[k] = in;
        if (getenv("GCN_LIST")) printf("      %3u %s\n", i, lines[k]);
        i += in.words;
    }
    *count = k;
    return true;
}

static bool starts(const char *line, const char *prefix) { return !strncmp(line, prefix, strlen(prefix)); }

static void test_vs_with_fetch(void) {
    uint32_t out[256];
    gcn_error err = {0};
    CHECK_EQ_U64(gcn_fetch_size(k_fetch, PS5C_ARRAY_COUNT(k_fetch)), 12);
    uint32_t n = gcn_to_rdna2(k_vs, PS5C_ARRAY_COUNT(k_vs), k_fetch, 12, out, 256, &err);
    if (!n) printf("    %s at %u (%08x)\n", err.why, err.pos, err.word);
    CHECK(n > 0);
    static char lines[128][96];
    static rdna2_inst in[128];
    uint32_t count = 0;
    CHECK(listing(out, n, lines, &count, in));
    /* the fetch shader is inlined where the s_swappc_b64 was: three SMEM loads (byte offsets) */
    CHECK(starts(lines[0], "s_mov_b32 vcc_hi"));
    CHECK(starts(lines[1], "s_load_dwordx4 s[8:11], s[2:3], 0x0"));
    CHECK(starts(lines[2], "s_load_dwordx4 s[12:15], s[2:3], 0x10"));
    CHECK(starts(lines[3], "s_load_dwordx4 s[16:19], s[2:3], 0x20"));
    CHECK(in[4].enc == RDNA2_ENC_SOPP && !strcmp(in[4].name, "s_waitcnt"));
    CHECK(starts(lines[5], "buffer_load_format_xy v[4:5], v0, s[8:11], 0 idxen"));
    CHECK(in[7].enc == RDNA2_ENC_MUBUF && !strcmp(in[7].name, "buffer_load_format_xyzw") && in[7].idxen);
    CHECK(!strcmp(in[8].name, "s_waitcnt"));
    /* the VS goes on: v_mov_b32, s_buffer_load (byte offsets), v_mac_f32 in VOP3 form */
    CHECK(!strcmp(in[9].name, "v_mov_b32"));
    CHECK(starts(lines[10], "s_buffer_load_dwordx8 s[8:15], s[4:7], 0x0"));
    CHECK(starts(lines[11], "s_buffer_load_dwordx2 s[0:1], s[4:7], 0x20"));
    bool mac3 = false, endpgm = false;
    uint32_t exports = 0;
    for (uint32_t i = 0; i < count; i++) {
        mac3 |= in[i].enc == RDNA2_ENC_VOP3 && !strcmp(in[i].name, "v_mac_f32");
        exports += in[i].enc == RDNA2_ENC_EXP;
        endpgm |= !strcmp(in[i].name, "s_endpgm");
        CHECK(strcmp(in[i].name, "s_swappc_b64") && strcmp(in[i].name, "s_setpc_b64"));
    }
    CHECK(mac3 && endpgm);
    CHECK_EQ_U64(exports, 4);
}

static void test_ps_branch_and_sample(void) {
    uint32_t out[256];
    gcn_error err = {0};
    uint32_t n = gcn_to_rdna2(k_ps, PS5C_ARRAY_COUNT(k_ps), NULL, 0, out, 256, &err);
    if (!n) printf("    %s at %u (%08x)\n", err.why, err.pos, err.word);
    CHECK(n > 0);
    static char lines[128][96];
    static rdna2_inst in[128];
    uint32_t count = 0;
    CHECK(listing(out, n, lines, &count, in));
    int branch = -1, sample = -1, target = -1;
    uint32_t at = 0, branch_at = 0, pos = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!strcmp(in[i].name, "s_cbranch_execz")) { branch = (int)i; branch_at = pos; }
        if (in[i].enc == RDNA2_ENC_MIMG) sample = (int)i;
        if (branch >= 0 && (uint32_t)branch < i && !strcmp(in[i].name, "s_mov_b64") && in[i].src[0] == 0x6a) target = (int)pos;
        pos += in[i].words;
        (void)at;
    }
    CHECK(sample >= 0 && !strcmp(in[sample].name, "image_sample") && in[sample].dim == 1 && in[sample].dmask == 0xF);
    CHECK(branch >= 0 && target >= 0);
    /* s_cbranch_execz skips to s_mov_b64 exec, s[106:107] past the SMEM load, now one dword longer */
    CHECK_EQ_U64((int32_t)branch_at + 1 + in[branch].simm16, target);
    CHECK(!strcmp(in[branch + 1].name, "s_buffer_load_dwordx16"));
    bool vopc = false, mad = false;
    for (uint32_t i = 0; i < count; i++) {
        vopc |= !strcmp(in[i].name, "v_cmp_gt_f32");
        mad |= !strcmp(in[i].name, "v_mad_f32");
    }
    CHECK(vopc && mad);
}

static void test_waitcnt_and_vop2_changes(void) {
    /* s_waitcnt lgkmcnt(0) (vmcnt and expcnt idle): RDNA2 keeps vmcnt idle through its high bits */
    const uint32_t wait[] = {0xbf8c007fu, 0xbf810000u};
    uint32_t out[8];
    CHECK_EQ_U64(gcn_to_rdna2(wait, 2, NULL, 0, out, 8, NULL), 2);
    CHECK_EQ_U64(out[0], 0xbf8cc07fu);
    /* v_cndmask_b32 v0, v1, v2 (GCN VOP2 0) is RDNA2 VOP2 1 */
    const uint32_t cnd[] = {(0u << 25) | (0u << 17) | (2u << 9) | 0x101u, 0xbf810000u};
    CHECK_EQ_U64(gcn_to_rdna2(cnd, 2, NULL, 0, out, 8, NULL), 2);
    CHECK_EQ_U64(out[0], (1u << 25) | (2u << 9) | 0x101u);
    /* v_lshr_b32 v0, s4, v2 -> v_lshrrev_b32 v0, v2, s4 (VOP3) */
    const uint32_t lshr[] = {(21u << 25) | (2u << 9) | 4u, 0xbf810000u};
    CHECK_EQ_U64(gcn_to_rdna2(lshr, 2, NULL, 0, out, 8, NULL), 3);
    rdna2_inst in;
    CHECK(rdna2_decode(out, 3, &in) && in.enc == RDNA2_ENC_VOP3 && !strcmp(in.name, "v_lshrrev_b32"));
    CHECK(in.src[0] == 0x102 && in.src[1] == 4);
    /* v_add_i32 v0, vcc, v1, v2 -> v_add_co_u32 v0, vcc_lo... (VOP3B, carry to vcc) */
    const uint32_t add[] = {(37u << 25) | (2u << 9) | 0x101u, 0xbf810000u};
    CHECK_EQ_U64(gcn_to_rdna2(add, 2, NULL, 0, out, 8, NULL), 3);
    CHECK(rdna2_decode(out, 3, &in) && !strcmp(in.name, "v_add_co_u32") && in.sdst == 106);
    /* a call without its fetch shader is refused */
    gcn_error err = {0};
    const uint32_t call[] = {0xbe802100u, 0xbf810000u};
    CHECK_EQ_U64(gcn_to_rdna2(call, 2, NULL, 0, out, 8, &err), 0);
    CHECK(err.why != NULL && err.pos == 0);
}

static void test_code_after_early_endpgm(void) {
    /* s_cbranch_scc0 over an early s_endpgm: the code it jumps to is translated too */
    const uint32_t ps[] = {0xbf840001u, 0xbf810000u, 0xbf810000u};
    uint32_t out[8];
    gcn_error err = {0};
    CHECK_EQ_U64(gcn_to_rdna2(ps, 3, NULL, 0, out, 8, &err), 3);
    CHECK_EQ_U64(out[0], 0xbf840001u);
    CHECK_EQ_U64(out[1], 0xbf810000u);
    CHECK_EQ_U64(out[2], 0xbf810000u);
}

int main(void) {
    static const test_case tests[] = {
        TEST_CASE(test_vs_with_fetch),
        TEST_CASE(test_ps_branch_and_sample),
        TEST_CASE(test_waitcnt_and_vop2_changes),
        TEST_CASE(test_code_after_early_endpgm),
    };
    return test_run_all("gcn", tests, PS5C_ARRAY_COUNT(tests));
}
