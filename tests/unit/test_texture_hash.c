/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include "gpu/vk/texture_hash.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static uint64_t old_hash(const uint8_t *p, uint64_t size) {
    uint64_t h = 0xCBF29CE484222325ull;
    if (size <= 16384) return texture_hash_words(p, (size_t)size, h) | 1;
    uint64_t step = (size - 64) / 255;
    for (uint32_t i = 0; i < 256; i++) h = texture_hash_words(p + i * step, 64, h);
    return h | 1;
}

int main(void) {
    if (texture_xxh64(NULL, 0, 0) != UINT64_C(0xef46db3751d8e999) ||
        texture_xxh64((const uint8_t *)"a", 1, 0) != UINT64_C(0xd24ec4f1a98c6e5b) ||
        texture_xxh64((const uint8_t *)"abc", 3, 0) != UINT64_C(0x44bc2cf5ad770999)) return 6;
    uint8_t *p = (uint8_t *)calloc(65537, 1);
    if (!p) return 1;
    /* In the old 32 KiB detector, byte 64 falls between two samples. */
    uint64_t old = old_hash(p, 32768), before = texture_content_hash(p, 32768);
    p[64] = 1;
    if (old_hash(p, 32768) != old || texture_content_hash(p, 32768) == before) return 2;
    p[64] = 0;
    const size_t sizes[] = {1, 7, 8, 16384, 16385, 32768, 65536};
    for (size_t j = 0; j < sizeof(sizes) / sizeof(sizes[0]); j++) {
        size_t size = sizes[j];
        before = texture_content_hash(p, size);
        uint64_t fast_before = texture_content_hash_fast(p, size);
        if (!before || texture_content_hash(p, size) != before) return 3;
        /* Check every byte of every small size, including the threshold. */
        for (size_t i = 0; i < size; i++) {
            p[i] = 1;
            if (texture_content_hash(p, size) == before || texture_content_hash_fast(p, size) == fast_before) return 4;
            p[i] = 0;
        }
    }
    /* Just above the threshold, integer division used to miss the tail. */
    before = texture_content_hash(p, 65537);
    old = old_hash(p, 65537);
    p[65536] = 1;
    if (old_hash(p, 65537) != old || texture_content_hash(p, 65537) == before) return 5;
    p[65536] = 0;
    uint64_t fast_before = texture_content_hash_fast(p, 65537);
    p[65536] = 1;
    if (texture_content_hash_fast(p, 65537) == fast_before) return 7;
    /* Reference vectors generated with official xxHash v0.8.3 XXH64(). */
    const size_t vector_sizes[] = {1024, 4096, 16384, 65536};
    const uint64_t vectors[] = {UINT64_C(0xd8b28b2a9bb53213), UINT64_C(0xc408a948390444f6),
                                UINT64_C(0xf2b4a708842d3608), UINT64_C(0xf0940af17e3ccaa7)};
    for (size_t i = 0; i < 65537; ++i) p[i] = (uint8_t)(i * 37 + 11);
    for (unsigned k = 0; k < 4; ++k)
        if (texture_xxh64(p, vector_sizes[k], UINT64_C(0xCBF29CE484222325)) != vectors[k]) return 8;
#ifdef _WIN32
    SYSTEM_INFO info; GetSystemInfo(&info); size_t page = info.dwPageSize;
    uint8_t *guard = VirtualAlloc(NULL, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!guard) return 9;
    DWORD prior;
    if (!VirtualProtect(guard + page, page, PAGE_NOACCESS, &prior)) return 10;
    for (size_t n = 0; n <= 512; ++n) {
        uint8_t *tail = guard + page - n;
        for (size_t i = 0; i < n; ++i) tail[i] = p[i];
        if (texture_xxh64(tail, n, 0) != texture_xxh64(p, n, 0) ||
            texture_content_hash_fast(tail, n) != texture_content_hash_fast(p, n)) return 11;
    }
    VirtualFree(guard, 0, MEM_RELEASE);
#endif
    free(p);
    puts("PASS: legacy and XXH64 content detectors, reference vectors, every-byte edits and sampled tail verified");
    return 0;
}
