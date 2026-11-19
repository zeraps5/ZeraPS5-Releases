/* SPDX-License-Identifier: GPL-2.0-or-later */
/* LOD queries: return ordering, masks, array coordinates and stage restrictions. */
#include <stdio.h>
#include <stdlib.h>
#include "gpu/shader/shader.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static bool query_result(const uint32_t *spv, size_t words, uint32_t mask, uint32_t dimensions) {
    uint32_t query = 0, coord = 0, extracted = 0, count = 0;
    for (size_t i = 5; i < words; i += spv[i] >> 16) {
        if ((spv[i] & 0xFFFF) == 105) { /* OpImageQueryLod */
            query = spv[i + 2];
            coord = spv[i + 4];
            count++;
        }
    }
    if (count != 1) return false;
    bool coord_ok = false;
    for (size_t i = 5; i < words; i += spv[i] >> 16) {
        uint32_t op = spv[i] & 0xFFFF;
        if (op == 80 && spv[i + 2] == coord) /* OpCompositeConstruct */
            coord_ok = (spv[i] >> 16) == 3 + dimensions;
        if (op == 81 && spv[i + 3] == query) { /* OpCompositeExtract */
            uint32_t component = spv[i + 4];
            if (component > 1 || !(mask & (1u << component))) return false;
            /* No duplicate/swapped components: extracts follow enabled DMASK bits. */
            if (extracted && component == 0) return false;
            extracted |= 1u << component;
        }
    }
    return coord_ok && extracted == mask;
}

int main(void) {
    sh_program *p = calloc(1, sizeof(*p));
    sh_stage_info info = {.stage = SH_STAGE_PS};
    sh_site_shape shape = {.img_type = 9};
    uint32_t code[] = {0xF1800308u, 0x00400809u, 0xBF810000u};
    uint32_t *spv = NULL;
    size_t words;
    CHECK(p);
    const uint32_t dims[] = {1, 5, 2}, types[] = {9, 13, 10};
    unsigned saved = 0;
    for (unsigned d = 0; d < 3; d++) {
        shape.img_type = (uint8_t)types[d];
        for (uint32_t mode = 0; mode < 6; mode++) {
            uint32_t mask = 1 + mode % 3;
            code[0] = 0xF1800000u | (mask << 8) | (dims[d] << 3);
            code[1] = 0x00400809u | (mode >= 3 ? 1u << 30 : 0); /* A16 coordinates are floats */
            CHECK(sh_analyze(code, 3, 0x100000, p));
            CHECK(p->site_count == 1 && !p->sites[0].store);
            CHECK(sh_translate(p, &info, &shape, 0, &spv, &words));
            CHECK(query_result(spv, words, mask, dims[d] == 2 ? 3 : 2));
            const char *dir = getenv("PS5C_TEST_SPIRV");
            if (dir) {
                char path[512];
                snprintf(path, sizeof(path), "%s/lod%u.spv", dir, saved++);
                FILE *f = fopen(path, "wb");
                CHECK(f);
                CHECK(fwrite(spv, 4, words, f) == words);
                CHECK(fclose(f) == 0);
            }
            free(spv);
            spv = NULL;
        }
    }
    code[0] = 0xF1800308u;
    code[1] = 0x00400809u;
    shape.img_type = 9;
    CHECK(sh_analyze(code, 3, 0x100000, p));
    info.stage = SH_STAGE_CS;
    info.local_size[0] = info.local_size[1] = info.local_size[2] = 1;
    CHECK(!sh_translate(p, &info, &shape, 0, &spv, &words));
    free(spv);
    spv = NULL;
    info.stage = SH_STAGE_VS;
    CHECK(!sh_translate(p, &info, &shape, 0, &spv, &words));
    free(spv);
    spv = NULL;
    info.stage = SH_STAGE_PS;
    const uint32_t invalid[] = {0xF1800008u, 0xF1800408u, 0xF1801308u, 0xF1810308u, 0xF1820308u};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        code[0] = invalid[i];
        CHECK(sh_analyze(code, 3, 0x100000, p));
        CHECK(!sh_translate(p, &info, &shape, 0, &spv, &words));
        free(spv);
        spv = NULL;
    }
    shape.img_type = 11; /* cube faces currently represented as a 2D array */
    code[0] = 0xF1800318u;
    CHECK(sh_analyze(code, 3, 0x100000, p));
    CHECK(!sh_translate(p, &info, &shape, 0, &spv, &words));
    free(spv);
    free(p);
    puts("shader LOD: ok");
    return 0;
}
