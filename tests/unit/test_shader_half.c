/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Compiler regression: a half write must carry the untouched half through
 * the SPIR-V expression, including a high write followed by a low write. */
#include <stdio.h>
#include <stdlib.h>
#include "gpu/shader/shader.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static uint32_t merge_count(const uint32_t *spv, size_t words, uint32_t mask) {
    uint32_t mask_id = 0, count = 0;
    for (size_t i = 5; i < words; i += spv[i] >> 16)
        if ((spv[i] & 0xFFFF) == 43 && (spv[i] >> 16) == 4 && spv[i + 3] == mask)
            mask_id = spv[i + 2]; /* OpConstant */
    if (!mask_id) return 0;
    for (size_t i = 5; i < words; i += spv[i] >> 16) {
        if ((spv[i] & 0xFFFF) != 199 || spv[i + 4] != mask_id) continue; /* OpBitwiseAnd */
        uint32_t kept = spv[i + 2];
        for (size_t j = i + (spv[i] >> 16); j < words; j += spv[j] >> 16)
            if ((spv[j] & 0xFFFF) == 197 && (spv[j + 3] == kept || spv[j + 4] == kept)) {
                count++;
                break; /* OpBitwiseOr combines old half and new result */
            }
    }
    return count;
}

int main(void) {
    sh_program *p = calloc(1, sizeof(*p));
    sh_stage_info info = {.stage = SH_STAGE_CS, .local_size = {1, 1, 1}};
    CHECK(p);
    const uint32_t op[] = {0xD7540000u, 0xD7510000u, 0xD74B0000u}; /* MAX3/MIN3/FMA_F16 */
    for (uint32_t k = 0; k < 3; k++) {
        /* v0 starts with two distinct nonzero halves. Update high then low. */
        uint32_t code[] = {0x7E0002FFu, 0x3C003800u, op[k] | 0x4000u,
                           242u | (240u << 9) | (244u << 18), op[k],
                           242u | (240u << 9) | (244u << 18), 0xBF810000u};
        uint32_t *spv = NULL;
        size_t words;
        CHECK(sh_analyze(code, 7, 0x100000, p));
        CHECK(sh_translate(p, &info, NULL, 0, &spv, &words));
        CHECK(merge_count(spv, words, 0xFFFFu) >= 1); /* high write keeps low */
        CHECK(merge_count(spv, words, 0xFFFF0000u) >= 1); /* low keeps high */
        const char *dir = getenv("PS5C_TEST_SPIRV");
        if (dir) {
            char path[512];
            snprintf(path, sizeof(path), "%s/half%u.spv", dir, k);
            FILE *f = fopen(path, "wb");
            CHECK(f);
            CHECK(fwrite(spv, 4, words, f) == words);
            CHECK(fclose(f) == 0);
        }
        free(spv);
    }
    free(p);
    puts("shader half preservation: ok");
    return 0;
}
