/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PNG decoding against pictures Python's zlib compressed (dynamic, fixed and
 * stored deflate blocks, split IDAT chunks, all five row filters) and the
 * RGBA Pillow decodes from them: gray 1 and 2 bits (one with tRNS), palette
 * with tRNS, RGB 8 and 16 bits, RGBA, and gray + alpha 16 bits and RGBA
 * interlaced (Adam7). Pictures without alpha get the caller's (0x80). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle/png_decode.h"

#include "test_png_decode_vectors.inc"

static uint64_t fnv(const uint8_t *p, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

int main(void) {
    int failed = 0;
    for (size_t k = 0; k < sizeof(k_cases) / sizeof(k_cases[0]); k++) {
        png_header h;
        uint32_t w = k_cases[k].w, ht = k_cases[k].h;
        size_t pitch = (size_t)w * 4 + 8;
        uint8_t *out = (uint8_t *)malloc(pitch * ht);
        bool ok = out && png_parse_header(k_cases[k].png, k_cases[k].size, &h) && h.width == w && h.height == ht &&
                  png_decode_rgba(k_cases[k].png, k_cases[k].size, out, pitch, false, 0x80);
        uint64_t hash = 0xcbf29ce484222325ull;
        for (uint32_t y = 0; ok && y < ht; y++) hash = fnv(out + y * pitch, (size_t)w * 4, hash);
        if (!ok || hash != k_cases[k].hash) {
            fprintf(stderr, "%s: %s\n", k_cases[k].name, ok ? "different pixels" : "not decoded");
            failed = 1;
        }
        /* truncated data is refused, never read past */
        if (png_decode_rgba(k_cases[k].png, k_cases[k].size / 2, out, pitch, false, 0x80)) {
            fprintf(stderr, "%s: half the file decoded\n", k_cases[k].name);
            failed = 1;
        }
        free(out);
    }
    if (!failed) printf("png_decode: %zu pictures OK\n", sizeof(k_cases) / sizeof(k_cases[0]));
    return failed;
}
