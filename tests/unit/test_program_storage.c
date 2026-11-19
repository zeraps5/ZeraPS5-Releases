/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/program_storage.h"
#include "test.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static void check_live(const sh_program *p, const sh_program *q) {
    CHECK(!memcmp(p, q, offsetof(sh_program, insts)));
#define CHECK_FIELD(f, n) CHECK(!memcmp(p->f, q->f, (size_t)(n) * sizeof(p->f[0])))
    CHECK_FIELD(insts, p->count); CHECK_FIELD(ops, p->count); CHECK_FIELD(pc, p->count);
    CHECK_FIELD(sites, p->site_count); CHECK_FIELD(site_of, p->count); CHECK_FIELD(target, p->count);
    CHECK_FIELD(next_scalar, p->count + 1u); CHECK_FIELD(iflags, p->count); CHECK_FIELD(vmem_width, p->count);
#undef CHECK_FIELD
    CHECK(p->count == q->count && p->site_count == q->site_count);
    CHECK(p->param_mask == q->param_mask && p->mrt_mask == q->mrt_mask && p->attr_mask == q->attr_mask);
    CHECK(p->has_stores == q->has_stores && p->wave32 == q->wave32);
    CHECK(p->has_flow == q->has_flow && p->back_branch == q->back_branch);
    CHECK(p->eval_forward_branches == q->eval_forward_branches);
    CHECK(p->lane_reads == q->lane_reads);
}

static void test_live_fields_and_reuse(void) {
    sh_program *reference = program_storage_alloc(false);
    sh_program *sparse = program_storage_alloc(true), *clone = program_storage_alloc(true);
    CHECK(reference && sparse && clone);
    const unsigned counts[] = {0, 1, 31, 32, 255, 256, 1023, 8192, SH_MAX_INSTS, 3};
    for (unsigned k = 0; k < PS5C_ARRAY_COUNT(counts); ++k) {
        memset(reference, (int)(k * 17 + 1), sizeof(*reference));
        reference->count = counts[k];
        reference->site_count = (k & 1) ? SH_MAX_SITES : counts[k] % SH_MAX_SITES;
        reference->has_stores = (k & 1) != 0; reference->wave32 = (k & 2) != 0;
        reference->has_flow = (k & 4) != 0; reference->back_branch = (k & 1) == 0;
        reference->lane_reads = (k & 2) != 0;
        reference->eval_forward_branches = counts[k]>=40?40:0;
        CHECK(program_storage_copy(sparse, reference, true));
        check_live(reference, sparse);
        CHECK(program_storage_copy(clone, sparse, true)); /* reading the source must skip holes */
        check_live(reference, clone);
        if (counts[k] <= 32) {
#ifdef _WIN32
            uint64_t committed = 0;
            uintptr_t at = (uintptr_t)sparse, end = at + sizeof(*sparse);
            while (at < end) {
                MEMORY_BASIC_INFORMATION info;
                CHECK(VirtualQuery((void *)at, &info, sizeof(info)) == sizeof(info));
                CHECK(info.Type == MEM_PRIVATE || info.State == MEM_RESERVE);
                if (info.State == MEM_COMMIT) committed += info.RegionSize;
                at = (uintptr_t)info.BaseAddress + info.RegionSize;
            }
            CHECK(committed < 128 * 1024);
            printf("    %u instructions: %llu committed bytes / %zu reserved\n",
                   counts[k], (unsigned long long)committed, sizeof(*sparse));
#endif
        }
    }
    reference->count = SH_MAX_INSTS + 1u;
    CHECK(!program_storage_copy(sparse, reference, true));
    program_storage_free(reference, false);
    program_storage_free(sparse, true); program_storage_free(clone, true);
    program_storage_free(NULL, true);
}

static void test_reanalysis_grows(void) {
    uint32_t *code = malloc(SH_MAX_INSTS * sizeof(*code));
    sh_program *p = program_storage_alloc(true), *reference = program_storage_alloc(false);
    CHECK(code && p && reference);
    const unsigned counts[] = {2, 8193, 7, SH_MAX_INSTS, 1};
    for (unsigned k = 0; k < PS5C_ARRAY_COUNT(counts); ++k) {
        unsigned n = counts[k];
        for (unsigned i = 0; i < n; ++i) code[i] = 0xBF800000u; /* s_nop */
        code[n - 1] = 0xBF810000u; /* s_endpgm */
        CHECK(program_storage_prepare(p, true));
        CHECK(sh_analyze(code, n, 0x10000, p));
        CHECK(sh_analyze(code, n, 0x10000, reference));
        CHECK(p->count == n);
        program_storage_trim(p, true);
        check_live(reference, p);
        CHECK(p->next_scalar[n] == reference->next_scalar[n]);
    }
    program_storage_free(p, true); program_storage_free(reference, false); free(code);
}

int main(void) {
    if (pal_init() != PS5C_OK) return 1;
    const test_case tests[] = {TEST_CASE(test_live_fields_and_reuse), TEST_CASE(test_reanalysis_grows)};
    int rc = test_run_all("program_storage", tests, PS5C_ARRAY_COUNT(tests));
    pal_shutdown();
    return rc;
}
