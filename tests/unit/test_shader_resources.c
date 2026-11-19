/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include "gpu/shader/shader.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static uint32_t program(uint32_t *code, uint32_t loads) {
    for (uint32_t i = 0; i < loads; i++) {
        code[2 * i] = 0xe0302000;     /* buffer_load_dword v2, v0, s[8:11], 0 idxen */
        code[2 * i + 1] = 0x80020200;
    }
    code[2 * loads] = 0xbf810000; /* s_endpgm */
    return 2 * loads + 1;
}

int main(void) {
    sh_program *p = calloc(1, sizeof(*p));
    uint32_t code[2 * (SH_MAX_SITES + 1) + 1];
    sh_site_shape shapes[SH_MAX_SITES] = {0};
    CHECK(p);
    /* GTA scene shaders need 136/137 resource sites. The old global cap
     * rejected them before the backend could check the actual device. */
    uint32_t count = program(code, 137);
    CHECK(sh_analyze(code, count, 0, p));
    CHECK(p->site_count == 137 && sh_binding_count(p) == 138);
    sh_stage_info info = {.stage = SH_STAGE_CS, .local_size = {1, 1, 1}};
    uint32_t *spv = NULL;
    size_t words = 0;
    CHECK(sh_translate(p, &info, shapes, 7, &spv, &words));
    bool seen[138] = {0};
    uint32_t bindings = 0;
    for (size_t i = 5; i < words;) {
        uint32_t n = spv[i] >> 16;
        CHECK(n && i + n <= words);
        if ((spv[i] & 0xffff) == 71 && n == 4 && spv[i + 2] == 33) { /* OpDecorate Binding */
            uint32_t b = spv[i + 3];
            CHECK(b >= 7 && b < 7 + 138 && !seen[b - 7]);
            seen[b - 7] = true;
            bindings++;
        }
        i += n;
    }
    CHECK(bindings == 138);
    const char *dir = getenv("PS5C_TEST_SPIRV");
    if (dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/resources.spv", dir);
        FILE *f = fopen(path, "wb");
        CHECK(f && fwrite(spv, 4, words, f) == words);
        CHECK(!fclose(f));
    }
    free(spv);
    /* The internal bound still protects all site-indexed arrays. */
    count = program(code, SH_MAX_SITES);
    CHECK(sh_analyze(code, count, 0, p));
    CHECK(p->site_count == SH_MAX_SITES && sh_binding_count(p) == SH_MAX_SITES + 1);
    CHECK(sh_translate(p, &info, shapes, 0, &spv, &words));
    bool full_seen[SH_MAX_SITES + 1] = {0};
    bindings = 0;
    for (size_t i = 5; i < words;) {
        uint32_t n = spv[i] >> 16;
        CHECK(n && i + n <= words);
        if ((spv[i] & 0xffff) == 71 && n == 4 && spv[i + 2] == 33) {
            uint32_t b = spv[i + 3];
            CHECK(b <= SH_MAX_SITES && !full_seen[b]);
            full_seen[b] = true;
            bindings++;
        }
        i += n;
    }
    CHECK(bindings == SH_MAX_SITES + 1);
    if (dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/resources-full.spv", dir);
        FILE *f = fopen(path, "wb");
        CHECK(f && fwrite(spv, 4, words, f) == words && !fclose(f));
    }
    free(spv);
    count = program(code, SH_MAX_SITES + 1);
    CHECK(!sh_analyze(code, count, 0, p));
    free(p);
    puts("shader resources: ok");
    return 0;
}
