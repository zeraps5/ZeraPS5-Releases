/* SPDX-License-Identifier: GPL-2.0-only */
/* Codec fixtures: AnyPS5 core/libs/tests/GuestAjm.cpp, bf6e25a1.
 * Tests use our HLE dispatch and FFmpeg decoders, not the upstream engine. */
#include "hle/ajm_resample.h"
#include "hle/hle.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include "test.h"
#include <float.h>

static const uint8_t MP3_MONO[] = {
    0xFF, 0xFB, 0x14, 0xC4, 0x00, 0x00, 0x03, 0xB0, 0x21, 0x5E, 0xF4, 0x30, 0x80, 0x30, 0x98, 0x88, 0xA9, 0x83, 0x34, 0x70, 0x00, 0x18, 0x89, 0xCB,
    0x20, 0x00, 0x3B, 0xBB, 0xB9, 0x9F, 0xD7, 0x0E, 0x06, 0x06, 0x06, 0x2C, 0x3E, 0xEF, 0x83, 0xE7, 0xF1, 0x00, 0x63, 0x83, 0xFD, 0x4E, 0xFC, 0xFF,
    0x29, 0xE1, 0xF7, 0xD9, 0x87, 0x7E, 0xFF, 0x65, 0x9E, 0x21, 0xAF, 0x46, 0x58, 0xBB, 0x46, 0x58, 0xBA, 0x64, 0x40, 0x21, 0xB2, 0x6C, 0xFA, 0x6C,
    0xF8, 0x42, 0x0E, 0x7C, 0x1E, 0x8F, 0x0D, 0xBF, 0x1B, 0x02, 0xBC, 0x24, 0x0D, 0x37, 0xCA, 0x9D, 0x52, 0xFD, 0x77, 0xE0, 0x16, 0xBA, 0x02, 0x04,
    0xFF, 0xFB, 0x14, 0xC4, 0x03, 0x83, 0xC4, 0xE0, 0x2B, 0x28, 0x1D, 0xE4, 0x80, 0x20, 0x5D, 0x95, 0x60, 0x41, 0x70, 0x16, 0xC8, 0xD3, 0x02, 0x00,
    0x2F, 0x12, 0x02, 0xD0, 0x40, 0x1D, 0x98, 0x4D, 0x06, 0x99, 0x84, 0xF2, 0x68, 0x98, 0xBF, 0x86, 0xD1, 0x84, 0xC8, 0x18, 0x2A, 0xA9, 0x79, 0x50,
    0x85, 0xDE, 0x14, 0x88, 0x9A, 0xF8, 0x44, 0x02, 0xB0, 0x60, 0x62, 0x5E, 0x11, 0xE9, 0xDC, 0x18, 0x65, 0x8F, 0xFF, 0x7F, 0xFC, 0xB3, 0xF8, 0x58,
    0xEF, 0xFF, 0xFF, 0xE1, 0x95, 0xF8, 0x44, 0x02, 0xB0, 0x60, 0x62, 0x5E, 0x11, 0xE9, 0xDC, 0x18, 0x65, 0x8F, 0xFF, 0x7F, 0xFC, 0xB3, 0xF8, 0x58,
    0xFF, 0xFB, 0x14, 0xC4, 0x09, 0x83, 0xC2, 0xEC, 0xAB, 0x02, 0x0B, 0x80, 0xB6, 0x40, 0x5D, 0x95, 0x60, 0x41, 0x70, 0x16, 0xC8, 0xEF, 0xFF, 0xFF,
    0xE1, 0x9F, 0x08, 0x80, 0x56, 0x0C, 0x0C, 0x4B, 0xC2, 0x3D, 0x3B, 0x83, 0x0C, 0xB1, 0xFF, 0xEF, 0xFF, 0x96, 0x7F, 0x0B, 0x1D, 0xFF, 0xFF, 0xFC,
    0x31, 0xF8, 0x44, 0x02, 0xB0, 0x60, 0x62, 0x5E, 0x11, 0xE9, 0xDC, 0x18, 0x65, 0x8F, 0xFF, 0x7F, 0xFC, 0xB3, 0xF8, 0x58, 0xEF, 0xFF, 0xFF, 0xE1,
    0x9F, 0x08, 0x80, 0x56, 0x0C, 0x0C, 0x4B, 0xC2, 0x3D, 0x3B, 0x83, 0x0C, 0xB1, 0xFF, 0xEF, 0xFF, 0x96, 0x7F, 0x0B, 0x1D, 0xFF, 0xFF, 0xFC, 0x31,
    0xFF, 0xFB, 0x14, 0xC4, 0x17, 0x83, 0xC2, 0xEC, 0xAB, 0x02, 0x0B, 0x80, 0xB6, 0x40, 0x5D, 0x95, 0x60, 0x41, 0x70, 0x16, 0xC8, 0x96, 0x55, 0x8D,
    0xBF, 0xE9, 0xD8, 0x50, 0x01, 0xA6, 0x17, 0x80, 0x34, 0x61, 0x00, 0x17, 0xC6, 0x15, 0x40, 0xD4, 0x61, 0x56, 0x25, 0x66, 0x3E, 0xA4, 0x92, 0x62,
    0xC3, 0xED, 0x26, 0x23, 0xA5, 0x5C, 0x63, 0x10, 0x01, 0xC6, 0x0F, 0x20, 0x9E, 0x60, 0x62, 0x08, 0x26, 0x06, 0xE0, 0x12, 0xD6, 0x22, 0x02, 0x6C,
    0x93, 0xC7, 0x04, 0x3E, 0xBF, 0xFE, 0xBF, 0xFF, 0x72, 0x62, 0x28, 0x22, 0xD1, 0x2F, 0xEF, 0xFD, 0xF1, 0x15, 0x09, 0x83, 0x08, 0x8C, 0x00, 0xFE,
    0xFF, 0xFB, 0x14, 0xC4, 0x25, 0x80, 0x07, 0x74, 0x2D, 0x16, 0x15, 0xE7, 0x80, 0x00, 0xF2, 0x0C, 0xE8, 0x83, 0x36, 0xB0, 0x00, 0xC4, 0x42, 0x82,
    0xC0, 0x86, 0x24, 0x2A, 0x02, 0x11, 0xFF, 0x02, 0x70, 0xF6, 0x07, 0x80, 0x9B, 0xCB, 0xCD, 0x0D, 0x00, 0x7C, 0x49, 0x1D, 0xBF, 0xD3, 0xDE, 0x3B,
    0x52, 0x36, 0xFE, 0xFA, 0xF3, 0x66, 0xB5, 0x13, 0xDF, 0x06, 0x84, 0xA1, 0x2F, 0xCB, 0x05, 0x41, 0x5F, 0xF5, 0x2A, 0x4C, 0x41, 0x4D, 0x45, 0x34,
    0x2E, 0x30, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
    0xFF, 0xFB, 0x14, 0xC4, 0x0E, 0x83, 0xC0, 0x00, 0x01, 0xA4, 0x1C, 0x00, 0x00, 0x20, 0x00, 0x00, 0x34, 0x80, 0x00, 0x00, 0x04, 0xAA, 0xAA, 0xAA,
    0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
    0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
    0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
};

static const uint8_t OPUS_STEREO[] = {
    0x46, 0x00, 0x7C, 0x87, 0xFC, 0xB1, 0x1D, 0xC0, 0xE3, 0x07, 0xD5, 0x9C, 0x4D, 0x91, 0x62, 0x6B, 0x73, 0x86, 0x05, 0xE8, 0xDB, 0xC6, 0x23, 0x6D,
    0x5E, 0x6F, 0x73, 0xF8, 0xF2, 0x47, 0xD9, 0x5E, 0xEA, 0xB3, 0xD2, 0x74, 0x6C, 0xE0, 0xCE, 0xE2, 0x3C, 0xFA, 0x59, 0x4A, 0xBA, 0x8F, 0x76, 0x0F,
    0x15, 0xE1, 0x3E, 0x60, 0xDF, 0x80, 0xC5, 0x44, 0xB1, 0x1B, 0xEF, 0x43, 0x03, 0x4F, 0xF2, 0xDE, 0xE3, 0xAD, 0x8B, 0x20, 0xFF, 0x68, 0x0C, 0x0A,
    0x3F, 0x00, 0x7C, 0x87, 0xFD, 0x45, 0xBD, 0x12, 0x00, 0xA5, 0xB1, 0xA0, 0x0A, 0x62, 0x24, 0x6F, 0x63, 0x70, 0xA5, 0x35, 0x13, 0x03, 0x74, 0x0D,
    0x83, 0xC9, 0x74, 0x1B, 0x79, 0xED, 0xA3, 0x09, 0x83, 0xA5, 0x02, 0xC4, 0x60, 0x49, 0xC9, 0xB6, 0x9A, 0x60, 0xAD, 0x6C, 0x77, 0x84, 0xE5, 0xC2,
    0xCE, 0x2E, 0x71, 0x07, 0x7E, 0x40, 0xCB, 0xDC, 0x13, 0xDF, 0xCF, 0x84, 0x5D, 0x55, 0x3B, 0x6B, 0x6F, 0x44, 0x00, 0x7C, 0x88, 0x01, 0xE8, 0xA1,
    0x2A, 0x12, 0x7D, 0x5E, 0xF6, 0x74, 0x78, 0xB1, 0x27, 0x2B, 0x8C, 0xF0, 0x7F, 0x95, 0x51, 0x71, 0x28, 0x18, 0xEA, 0x66, 0xEB, 0xCA, 0xAC, 0xDF,
    0x6C, 0x9C, 0xFD, 0xED, 0x88, 0x8E, 0x66, 0xD7, 0xDA, 0x36, 0xD7, 0xEB, 0x5F, 0xA6, 0x05, 0x72, 0xDA, 0x52, 0x14, 0x7F, 0xF5, 0x84, 0x54, 0xA1,
    0xA9, 0xF1, 0xAA, 0xA8, 0x73, 0x9A, 0xCC, 0x91, 0x83, 0x92, 0xD0, 0x7F, 0x3B, 0x6B, 0x65, 0x43, 0x00, 0x7C, 0x88, 0x01, 0xE8, 0xA1, 0x2A, 0x12,
    0x7D, 0x5F, 0x04, 0xF8, 0xF5, 0x1F, 0xA0, 0x85, 0x89, 0xED, 0xBA, 0x7C, 0x5F, 0x63, 0x2A, 0x9E, 0x05, 0xE8, 0x63, 0xD1, 0x73, 0x0C, 0xAF, 0xC8,
    0xC9, 0x22, 0xF7, 0x11, 0x1D, 0x8B, 0xA9, 0x9A, 0x90, 0x77, 0xF2, 0x86, 0x49, 0x15, 0x6E, 0xD6, 0x34, 0x1F, 0x50, 0x15, 0xEC, 0x85, 0xC7, 0xF5,
    0xA7, 0xFA, 0x99, 0xF9, 0x47, 0x32, 0x07, 0xAF, 0x24, 0xBF, 0x14, 0xC5,
};

static const uint8_t AT9_MONO_SILENT_SUPERFRAME[] = {
    0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static gmem mem;
static guest_addr page;
static uint8_t *host;
enum { INFO = 0x100, BATCH = 0x200, RESULT = 0x1400, PARAM = 0x1500,
       INPUT = 0x1800, OUTPUT = 0x4000, STACK = 0x70, ID = 0x80 };
typedef struct sideband { int32_t result, internal, consumed, written; uint64_t total; uint32_t frames, reserved; } sideband;

static int32_t invoke(const char *name, const uint64_t v[7], float ratio, float ramp) {
    const hle_export *e = hle_find("libSceAjm", nid_hash_name(name));
    if (!e || !e->fn) return INT32_MIN;
    hle_args a = {0};
    memcpy(a.gpr, v, sizeof(a.gpr));
    memcpy(&a.xmm[0][0], &ratio, 4); memcpy(&a.xmm[1][0], &ramp, 4);
    a.stack = page + STACK;
    gmem_write(&mem, a.stack, &v[6], 8);
    hle_call c = {.mem = &mem, .a = &a};
    e->fn(&c);
    return (int32_t)a.ret[0];
}
#define CALL(name, ...) invoke(name, (uint64_t[7]){__VA_ARGS__}, 0, 0)
static void batch(void) { CALL("sceAjmBatchInitialize", page+BATCH, 4096, page+INFO); }
static int32_t set_ratio(uint32_t id, float ratio, float ramp, bool ex) {
    return invoke(ex ? "sceAjmBatchJobSetResampleParametersEx" : "sceAjmBatchJobSetResampleParameters",
                  (uint64_t[7]){page+INFO, id, 1, page+RESULT}, ratio, ramp);
}
static uint32_t create(uint32_t codec, uint32_t encoding) {
    batch();
    if (CALL("sceAjmInstanceCreate", 1, codec, (uint64_t)encoding << 7, page+ID)) return 0;
    uint32_t id; memcpy(&id, host+ID, 4);
    uint8_t cfg[8] = {0xfe, 0x70, 0x1f, 0xf0};
    uint32_t opus[2] = {2, 48000};
    if (codec == 1 || codec == 24) {
        memcpy(host+PARAM, codec == 1 ? (void*)cfg : (void*)opus, 8);
        if (CALL("sceAjmBatchJobInitialize", page+INFO, id, page+PARAM, 8, page+RESULT) || *(int32_t*)(host+RESULT)) return 0;
    }
    return id;
}
static bool decode_stream(uint32_t id, const uint8_t *input, size_t size,
                           uint8_t *pcm, size_t *bytes, size_t room, uint64_t *samples) {
    memcpy(host+INPUT, input, size);
    size_t consumed = 0; *bytes = 0; *samples = 0;
    for (unsigned jobs = 0; jobs < 4096; ++jobs) {
        batch(); memset(host+OUTPUT, 0xa5, room+8); memset(host+RESULT, 0xa5, 40);
        if (CALL("sceAjmBatchJobDecode", page+INFO, id, page+INPUT+consumed, size-consumed,
                 page+OUTPUT, room, page+RESULT)) return false;
        sideband r; memcpy(&r, host+RESULT, sizeof(r));
        if ((r.result & ~(8|16)) || r.consumed < 0 || r.written < 0 ||
            (size_t)r.consumed > size-consumed || (size_t)r.written > room || *bytes+(size_t)r.written > 65536) return false;
        for (size_t i = room; i < room+8; ++i) if (host[OUTPUT+i] != 0xa5) return false;
        if (host[RESULT+32] != 0xa5) return false;
        memcpy(pcm+*bytes, host+OUTPUT, (size_t)r.written);
        *bytes += (size_t)r.written; consumed += (size_t)r.consumed; *samples = r.total;
        if (!r.written && !r.consumed) return consumed == size;
    }
    return false;
}

static void test_hle_contract(void) {
    uint32_t id = create(0, 0); CHECK(id);
    uint32_t other = create(0, 0); CHECK(other);
    batch(); CHECK(set_ratio(id, 2.0f, 0, true) == 0);
    CHECK(CALL("sceAjmBatchJobGetResampleInfo", page+INFO, id, page+RESULT) == 0);
    float ratio; memcpy(&ratio, host+RESULT+8, 4); CHECK(ratio == 2.0f);
    CHECK(CALL("sceAjmBatchJobGetResampleInfo", page+INFO, other, page+RESULT) == 0);
    memcpy(&ratio, host+RESULT+8, 4); CHECK(ratio == 1.0f);
    const float invalid[] = {0, -1, NAN, INFINITY};
    for (unsigned i=0; i<4; ++i) {
        uint64_t offset; memcpy(&offset, host+INFO+8, 8);
        CHECK((uint32_t)set_ratio(id, invalid[i], 0, true) == 0x80930005);
        uint64_t after; memcpy(&after, host+INFO+8, 8); CHECK(after == offset);
    }
    CHECK((uint32_t)set_ratio(id, 1, 0.001f, true) == 0x80930005);
    CHECK((uint32_t)set_ratio(id, 1, NAN, true) == 0x80930005);
    CHECK(CALL("sceAjmBatchJobClearContext", page+INFO, id, page+RESULT) == 0);
    CHECK(CALL("sceAjmBatchJobGetResampleInfo", page+INFO, id, page+RESULT) == 0);
    memcpy(&ratio, host+RESULT+8, 4); CHECK(ratio == 2.0f);
    uint64_t full = 4096; memcpy(host+INFO+8, &full, 8);
    CHECK((uint32_t)set_ratio(id, 0.5f, 0, false) == 0x80930012);
    batch(); CHECK(CALL("sceAjmBatchJobGetResampleInfo", page+INFO, id, page+RESULT) == 0);
    memcpy(&ratio, host+RESULT+8, 4); CHECK(ratio == 2.0f);
    CHECK(set_ratio(0xffffffff, 1, 0, true) == 0); CHECK(*(int32_t*)(host+RESULT) == 4);
    CHECK(CALL("sceAjmInstanceDestroy", 1, id) == 0);
    CHECK(CALL("sceAjmInstanceDestroy", 1, other) == 0);
}

static void test_codec_streams(void) {
    uint8_t at9[3072] = {0};
    for (unsigned i=0; i<3; ++i) memcpy(at9+i*1024, AT9_MONO_SILENT_SUPERFRAME, sizeof(AT9_MONO_SILENT_SUPERFRAME));
    const uint8_t *inputs[] = {MP3_MONO, OPUS_STEREO, at9};
    size_t sizes[] = {sizeof(MP3_MONO), sizeof(OPUS_STEREO), sizeof(at9)};
    uint32_t codecs[] = {0, 24, 1}, channels[] = {1, 2, 1};
    uint8_t *plain=malloc(65536), *large=malloc(65536), *small=malloc(65536);
    CHECK(plain && large && small);
    for (unsigned codec=0; codec<3; ++codec) for (uint32_t enc=0; enc<3; ++enc) {
        size_t stride = channels[codec] * (enc == 0 ? 2 : 4);
        uint32_t ref = create(codecs[codec], enc); CHECK(ref);
        size_t reference_bytes; uint64_t reference_samples;
        CHECK(decode_stream(ref, inputs[codec], sizes[codec], plain, &reference_bytes, 65536, &reference_samples));
        CHECK(reference_bytes > 0 && reference_samples == reference_bytes/stride);
        CHECK(CALL("sceAjmInstanceDestroy", 1, ref) == 0);
        const float ratios[] = {2.0f, 0.5f, 1.5f, 3.25f};
        const unsigned integer_step[] = {1, 2, 2, 4};
        for (unsigned mode=0; mode<4; ++mode) {
            float ratio = ratios[mode];
            uint32_t a=create(codecs[codec], enc), b=create(codecs[codec], enc); CHECK(a && b);
            batch(); CHECK(set_ratio(a, ratio, 0, true) == 0);
            batch(); CHECK(set_ratio(b, ratio, 0, false) == 0);
            size_t large_bytes, small_bytes; uint64_t a_samples, b_samples;
            CHECK(decode_stream(a, inputs[codec], sizes[codec], large, &large_bytes, 16384, &a_samples));
            CHECK(decode_stream(b, inputs[codec], sizes[codec], small, &small_bytes, 17*stride, &b_samples));
            CHECK(large_bytes == small_bytes && !memcmp(large, small, large_bytes));
            printf("codec %u enc %u ratio %g: ref %llu (%zu bytes), large %llu/%zu, small %llu/%zu\n",
                   codecs[codec], enc, (double)ratio, (unsigned long long)reference_samples,
                   reference_bytes, (unsigned long long)a_samples, large_bytes, (unsigned long long)b_samples, small_bytes);
            CHECK(a_samples == reference_samples && b_samples == reference_samples);
            size_t frames=large_bytes/stride, expected=(size_t)ceil((double)(reference_samples-2)/ratio);
            CHECK(frames == expected);
            for (size_t n=0; n<frames; n += integer_step[mode])
                CHECK(!memcmp(large+n*stride, plain+(size_t)(n*(double)ratio)*stride, stride));
            batch(); CHECK(CALL("sceAjmBatchJobGetResampleInfo", page+INFO, a, page+RESULT) == 0);
            CHECK(*(int32_t*)(host+RESULT+12) <= 2);
            CHECK(CALL("sceAjmBatchJobClearContext", page+INFO, a, page+RESULT) == 0);
            CHECK(CALL("sceAjmBatchJobGetResampleInfo", page+INFO, a, page+RESULT) == 0);
            CHECK(*(int32_t*)(host+RESULT+12) == 0);
            CHECK(decode_stream(a, inputs[codec], sizes[codec], small, &small_bytes, 91*stride, &a_samples));
            CHECK(small_bytes == large_bytes && !memcmp(small, large, large_bytes));
            CHECK(CALL("sceAjmInstanceDestroy", 1, a) == 0); CHECK(CALL("sceAjmInstanceDestroy", 1, b) == 0);
        }
    }
    free(plain); free(large); free(small);
    puts("AJM: MP3/Opus/ATRAC9, S16/S32/float, 4 ratios, small output and context reset: 36 scenarios");
}

static int32_t run_split(uint32_t id,uint64_t flags,uint64_t ni,uint64_t no,uint64_t capacity) {
    const hle_export *e=hle_find("libSceAjm",nid_hash_name("sceAjmBatchJobRunSplit"));
    if(!e || !e->fn)return INT32_MIN;
    hle_args a={.gpr={page+INFO,id,flags,page+0x1600,ni,page+0x1640},.stack=page+0x1780};
    uint64_t extra[]={no,page+RESULT,capacity};if(!gmem_write(&mem,a.stack,extra,sizeof(extra)))return INT32_MIN;
    hle_call c={.mem=&mem,.a=&a};e->fn(&c);return (int32_t)a.ret[0];
}

static void test_run_split_and_statistics(void) {
    uint32_t reference=create(0,0),candidate=create(0,0);CHECK(reference && candidate);
    memcpy(host+INPUT,MP3_MONO,sizeof(MP3_MONO));batch();
    CHECK(CALL("sceAjmBatchJobDecode",page+INFO,reference,page+INPUT,sizeof(MP3_MONO),page+OUTPUT,16384,page+RESULT)==0);
    sideband expected;memcpy(&expected,host+RESULT,sizeof(expected));
    CHECK(expected.written>512 && expected.written<=16384);
    uint8_t *pcm=malloc((size_t)expected.written);CHECK(pcm);memcpy(pcm,host+OUTPUT,(size_t)expected.written);
    const uint64_t inputs[]={page+INPUT,3,page+INPUT+3,sizeof(MP3_MONO)-3};
    const uint64_t outputs[]={page+OUTPUT,512,page+0xc000,16384-512};
    memcpy(host+0x1600,inputs,sizeof(inputs));memcpy(host+0x1640,outputs,sizeof(outputs));
    const uint64_t flags=(1ull<<47)|(1ull<<12);batch();
    memset(host+RESULT,0xa5,64);memset(host+OUTPUT,0xa5,520);memset(host+0xc000,0xa5,16384-512+8);
    uint64_t before,after;memcpy(&before,host+INFO+8,8);
    CHECK((uint32_t)run_split(candidate,flags,65,2,32)==0x80930005);
    CHECK((uint32_t)run_split(candidate,flags,2,2,31)==0x80930005);
    uint64_t huge=UINT64_MAX;memcpy(host+0x1608,&huge,8);
    CHECK((uint32_t)run_split(candidate,flags,2,2,32)==0x80930005);
    memcpy(host+0x1600,inputs,sizeof(inputs));memcpy(&after,host+INFO+8,8);CHECK(before==after);
    uint64_t no_room=0;memcpy(host+INFO+16,&no_room,8);
    CHECK((uint32_t)run_split(candidate,flags,2,2,32)==0x80930012);
    CHECK(host[RESULT]==0xa5 && host[OUTPUT]==0xa5); /* failure must not decode */
    batch();CHECK(run_split(candidate,flags,2,2,32)==0);
    sideband actual;memcpy(&actual,host+RESULT,sizeof(actual));CHECK(!memcmp(&actual,&expected,sizeof(actual)));
    CHECK(!memcmp(host+OUTPUT,pcm,512) && !memcmp(host+0xc000,pcm+512,(size_t)actual.written-512));
    CHECK(host[OUTPUT+512]==0xa5 && host[0xc000+16384-512]==0xa5 && host[RESULT+32]==0xa5);
    memcpy(&after,host+INFO+8,8);CHECK(after==96);
    batch();memset(host+RESULT,0xa5,64);
    CHECK(invoke("sceAjmBatchJobGetStatistics",(uint64_t[7]){page+INFO,page+RESULT},0.016f,0)==0);
    for(unsigned i=0;i<48;i++)CHECK(host[RESULT+i]==0);
    CHECK(host[RESULT+48]==0xa5);memcpy(&after,host+INFO+8,8);CHECK(after==88);
    CHECK(CALL("sceAjmInstanceDestroy",1,reference)==0 && CALL("sceAjmInstanceDestroy",1,candidate)==0);
    free(pcm);puts("AJM RunSplit: nine SysV arguments, split MP3 PCM matches decoder, stream counters, bounds, batch failure and statistics passed");
}

static void test_filter_and_extreme_ratio(void) {
    float in[64], out[128]; for (unsigned i=0; i<64; ++i) in[i]=(float)i;
    ajm_resampler s = {.ratio=0.5f, .position=1};
    CHECK(ajm_resample_push(&s, (uint8_t*)in, 64, 1, 2));
    CHECK(ajm_resample_produce(&s, (uint8_t*)out, sizeof(out), 2) == 124*4);
    /* First interpolation uses duplicated boundary; subsequent ramp samples
     * have an independent analytical value. */
    for (unsigned i=2; i<124; ++i) CHECK(fabsf(out[i] - (float)i/2) < 1e-5f);
    ajm_resample_clear(&s); s.ratio = FLT_MAX;
    CHECK(ajm_resample_push(&s, (uint8_t*)in, 64, 1, 2));
    CHECK(ajm_resample_produce(&s, (uint8_t*)out, sizeof(out), 2) == 4);
    CHECK(ajm_resample_held(&s) == 0);
    CHECK(ajm_resample_push(&s, (uint8_t*)in, 64, 1, 2));
    CHECK(ajm_resample_produce(&s, (uint8_t*)out, sizeof(out), 2) == 0);
    ajm_resample_free(&s);
}

int main(void) {
    if (pal_init() != PS5C_OK || hle_registry_init() != PS5C_OK) return 2;
    gmem_config cfg = gmem_default_config(); cfg.identity = false;
    if (gmem_init(&mem, &cfg) != PS5C_OK || gmem_map(&mem, 0, 0x20000, GPROT_RW, 0, "ajm-test", &page) != PS5C_OK) return 2;
    host=gmem_host(&mem, page, 0x20000);
    bool codecs = CALL("sceAjmModuleRegister", 1, 0, 0) == 0;
    const test_case tests[] = {TEST_CASE(test_filter_and_extreme_ratio), TEST_CASE(test_hle_contract), TEST_CASE(test_codec_streams),TEST_CASE(test_run_split_and_statistics)};
    int result = test_run_all("ajm_resample", tests, codecs ? PS5C_ARRAY_COUNT(tests) : 1);
    if (!codecs) puts("FFmpeg unavailable: codec/HLE resampling integration skipped");
    gmem_free(&mem); hle_registry_free(); pal_shutdown();
    return result;
}
