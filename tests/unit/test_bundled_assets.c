/* SPDX-License-Identifier: GPL-2.0-or-later */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pal/pal.h"
#include "pal/bundled_assets.h"
#include "hle/font_raster.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static const void *image_asset(HMODULE image, unsigned id, size_t *size) {
    HRSRC r = FindResourceW(image, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    *size = r ? SizeofResource(image, r) : 0;
    HGLOBAL loaded = r ? LoadResource(image, r) : NULL;
    return loaded ? LockResource(loaded) : NULL;
}

static int check_bytes(HMODULE image, unsigned id, const char *file) {
    size_t size = 0;
    const void *data = image ? image_asset(image, id, &size) : pal_bundled_asset(id, &size);
    uint8_t *reference = NULL;
    uint64_t length = 0;
    CHECK(data && size);
    CHECK(pal_file_read_all(file, &reference, &length) == PS5C_OK);
    CHECK(length == size && !memcmp(data, reference, size));
    free(reference);
    return 0;
}

static int check_movie(HMODULE image, const char *file) {
    size_t size = 0;
    const void *data = image ? image_asset(image, PS5C_BUNDLED_INTRO, &size)
                             : pal_bundled_asset(PS5C_BUNDLED_INTRO, &size);
    CHECK(data && size);
    pal_media *memory = pal_media_open_memory(data, size), *disk = pal_media_open_file(file);
    CHECK(memory && disk);
    pal_media_info a = {0}, b = {0};
    pal_media_get_info(memory, &a); pal_media_get_info(disk, &b);
    CHECK(a.width == 848 && a.height == 478 && a.channels == 1 && a.sample_rate == 44100);
    CHECK(a.width == b.width && a.height == b.height && a.channels == b.channels && a.sample_rate == b.sample_rate);
    size_t bytes = (size_t)a.width * a.height * 3 / 2;
    uint8_t *ma = malloc(bytes), *mb = malloc(bytes);
    CHECK(ma && mb);
    unsigned frames = 0;
    for (;;) {
        uint64_t ta = 0, tb = 0;
        int ra = pal_media_next_video(memory, ma, &ta), rb = pal_media_next_video(disk, mb, &tb);
        CHECK(ra == rb && ra >= 0);
        if (!ra) break;
        CHECK(ta == tb && !memcmp(ma, mb, bytes));
        frames++;
    }
    CHECK(frames == 150);
    free(ma); free(mb);
    pal_media_close(memory); pal_media_close(disk);
    memory = pal_media_open_memory(data, size); disk = pal_media_open_file(file);
    CHECK(memory && disk);
    static int16_t pa[8192], pb[8192];
    uint64_t samples = 0, nonzero = 0;
    for (;;) {
        uint64_t ta = 0, tb = 0;
        int ra = pal_media_next_audio(memory, pa, 4096, &ta), rb = pal_media_next_audio(disk, pb, 4096, &tb);
        CHECK(ra == rb && ra >= 0);
        if (!ra) break;
        CHECK(ta == tb && !memcmp(pa, pb, (size_t)ra * a.channels * sizeof(*pa)));
        samples += (unsigned)ra;
        for (int i = 0; i < ra; i++) nonzero += pa[i] != 0;
    }
    CHECK(samples > 200000 && nonzero > 1000);
    pal_media_close(memory); pal_media_close(disk);
    printf("embedded intro: %u identical video frames, %llu identical audio frames\n",
           frames, (unsigned long long)samples);
    return 0;
}

int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 4);
    CHECK(pal_init() == PS5C_OK);
    HMODULE image = argc == 4 ? LoadLibraryExA(argv[3], NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE) : NULL;
    CHECK(argc != 4 || image);
    size_t size = 123;
    CHECK(!pal_bundled_asset(999, &size) && size == 0);
    CHECK(check_bytes(image, PS5C_BUNDLED_FONT, argv[1]) == 0);
    CHECK(check_bytes(image, PS5C_BUNDLED_INTRO, argv[2]) == 0);
    /* This fixture's working directory deliberately has no data/fonts/ files.
     * The old disk-backed font_raster fixture still covers the file path. */
    FILE *f = fopen("data/fonts/NotoSans-Regular.ttf", "rb");
    CHECK(!f);
    const font_face *face = font_face_default();
    CHECK(face);
    static uint8_t pixels[64 * 64];
    font_surface surface = {pixels, 64, 1, 64, 64, {0, 0, 64, 64}};
    CHECK(font_draw_glyph(face, 'A', 32, 32, 8, 40, &surface));
#if defined(PS5C_HAVE_FFMPEG)
    CHECK(check_movie(image, argv[2]) == 0);
#endif
    if (image) FreeLibrary(image);
    pal_shutdown();
    puts("bundled_assets: font and movie usable without external runtime assets");
    return 0;
}
