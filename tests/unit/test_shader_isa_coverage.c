/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ISA coverage: every opcode of the decoder's tables, in one operand form
 * (tools/gen_isa_tests.py --coverage), as a one-instruction compute program
 * that must translate. Prints the count per encoding and the opcodes that do
 * not; fails when fewer translate than COVERAGE_MIN (raise it as gaps close).
 * PS5C_ISA_LIST=1 lists every missing opcode. */
#include "gpu/shader/shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COVERAGE_MIN 969u

typedef struct isa_op {
    const char *table, *name;
    uint32_t n;
    uint32_t words[4];
} isa_op;

static const isa_op k_ops[] = {
#include "test_shader_isa_coverage.inc"
};

int main(void) {
    sh_program *p = calloc(1, sizeof(*p));
    if (!p) return 1;
    sh_stage_info info = {.stage = SH_STAGE_CS, .cs_tid_components = 1, .cs_tgid_sgpr = 16, .local_size = {64, 1, 1}};
    sh_site_shape shapes[4];
    memset(shapes, 0, sizeof(shapes));
    bool list = getenv("PS5C_ISA_LIST") != NULL;
    uint32_t total = 0, ok = 0, t_total = 0, t_ok = 0;
    const char *table = NULL;
    char missing[2048] = "";
    for (size_t i = 0; i <= PS5C_ARRAY_COUNT(k_ops); i++) {
        const isa_op *o = i < PS5C_ARRAY_COUNT(k_ops) ? &k_ops[i] : NULL;
        if (table && (!o || strcmp(o->table, table))) {
            printf("  %-7s %4u/%4u%s%s\n", table, t_ok, t_total, missing[0] ? "  missing:" : "", missing);
            t_total = t_ok = 0;
            missing[0] = 0;
        }
        if (!o) break;
        table = o->table;
        uint32_t code[8] = {0};
        memcpy(code, o->words, o->n * 4);
        code[o->n] = 0xBF810000u; /* s_endpgm */
        uint32_t *spv = NULL;
        size_t words = 0;
        /* image sites: a 2D descriptor, an unsigned integer one for atomics */
        for (int k = 0; k < 4; k++) {
            shapes[k].img_type = 9;
            shapes[k].img_kind = strstr(o->name, "image_atomic") ? 1 : 0;
        }
        bool done = o->n && sh_analyze(code, o->n + 1, 0x100000, p) && sh_translate(p, &info, shapes, 0, &spv, &words);
        if (!done && o->n && !strcmp(o->table, "mimg")) { /* implicit derivatives: pixel shaders only */
            sh_stage_info ps = {.stage = SH_STAGE_PS};
            free(spv);
            spv = NULL;
            done = sh_analyze(code, o->n + 1, 0x100000, p) && sh_translate(p, &ps, shapes, 0, &spv, &words);
        }
        const char *dir = getenv("PS5C_TEST_SPIRV"); /* for spirv-val --target-env vulkan1.1 */
        if (done && dir) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s.spv", dir, o->name);
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(spv, 4, words, f);
                fclose(f);
            }
        }
        free(spv);
        total++, t_total++;
        if (done) {
            ok++, t_ok++;
        } else if (list || strlen(missing) + strlen(o->name) + 2 < 120) {
            strcat(missing, " ");
            strcat(missing, o->name);
        } else if (!strstr(missing, "...")) {
            strcat(missing, " ...");
        }
        if (!done && list) fprintf(stderr, "MISSING %s %s\n", table, o->name);
        if (!done && list) {
            printf("  %-7s %4u/%4u  missing: %s\n", table, t_ok, t_total, missing);
            missing[0] = 0;
        }
    }
    free(p);
    printf("ISA coverage: %u of %u opcodes translate (%.1f%%)\n", ok, total, 100.0 * ok / total);
    return ok >= COVERAGE_MIN ? 0 : 1;
}
