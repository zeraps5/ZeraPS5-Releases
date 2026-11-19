/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hle/hle.h"
#include "mem/gmem.h"
#include "loader/nid.h"
#include "test.h"
#include <string.h>

static int32_t call(const char *name, uint64_t a0, uint64_t a1, uint64_t a2,
                    uint64_t a3, uint64_t a4, uint64_t a5) {
    const hle_export *exp = hle_find("libSceAudioOut", nid_hash_name(name));
    if (!exp || !exp->fn) return INT32_MIN;
    hle_args args = {.gpr = {a0, a1, a2, a3, a4, a5}};
    hle_call c = {.a = &args};
    exp->fn(&c);
    return (int32_t)args.ret[0];
}

/* Unity's PS5 video backend initializes AudioOut only after Open returns
 * NOT_INIT. Keep that recovery path working even when AudioOut2 is in use.
 * Controller speaker ports avoid opening a host audio device in this test. */
static void test_open_initialization_recovery_and_errors(void) {
    CHECK(hle_registry_init() == PS5C_OK);
    int32_t port = call("sceAudioOutOpen", 0xff, 4, 0, 1024, 48000, 1);
    CHECK((uint32_t)port == 0x8026000f);
    if ((uint32_t)port == 0x8026000f) {
        CHECK(call("sceAudioOutInit", 0, 0, 0, 0, 0, 0) == 0);
        port = call("sceAudioOutOpen", 0xff, 4, 0, 1024, 48000, 1);
    }
    CHECK(port >= 0);
    CHECK((uint32_t)call("sceAudioOutOpen", 0xff, 4, 0, 255, 48000, 1) == 0x80260006);
    CHECK((uint32_t)call("sceAudioOutOpen", 0xff, 4, 0, 1024, 44100, 1) == 0x80260008);
    CHECK((uint32_t)call("sceAudioOutOpen", 0xff, 4, 0, 1024, 48000, 8) == 0x80260007);
    CHECK(call("sceAudioOutClose", port, 0, 0, 0, 0, 0) == 0);
    CHECK((uint32_t)call("sceAudioOutClose", port, 0, 0, 0, 0, 0) == 0x80260003);

    int32_t ports[16];
    for (unsigned i = 0; i < 16; i++) {
        ports[i] = call("sceAudioOutOpen", 0xff, 4, 0, 1024, 48000, 1);
        CHECK(ports[i] >= 0);
        for (unsigned j = 0; j < i; j++) CHECK(ports[i] != ports[j]);
    }
    CHECK((uint32_t)call("sceAudioOutOpen", 0xff, 4, 0, 1024, 48000, 1) == 0x80260005);
    CHECK(call("sceAudioOutClose", ports[3], 0, 0, 0, 0, 0) == 0);
    ports[3] = call("sceAudioOutOpen", 0xff, 4, 0, 1024, 48000, 1);
    CHECK(ports[3] >= 0);
    for (unsigned i = 0; i < 16; i++) CHECK(call("sceAudioOutClose", ports[i], 0, 0, 0, 0, 0) == 0);
    hle_registry_free();
}

static int32_t call2(gmem *m, const char *name, uint64_t a, uint64_t b, uint64_t d) {
    const hle_export *exp = hle_find("libSceAudioOut2", nid_hash_name(name));
    if (!exp || !exp->fn) return INT32_MIN;
    hle_args args = {.gpr = {a, b, d}};
    hle_call c = {.mem = m, .a = &args};
    exp->fn(&c);
    return (int32_t)args.ret[0];
}

/* Keep each port's input layout stable across state queries. Wwise uses this
 * byte to configure its PCM buffers; reporting stereo for an eight-channel
 * port makes producer and consumer disagree on the frame stride. */
static void test_audio_out2_port_channel_state(void) {
    CHECK(hle_registry_init() == PS5C_OK);
    gmem m;
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr page;
    CHECK(gmem_map(&m, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "AudioOut2 test", &page) == PS5C_OK);
    uint8_t *p = gmem_host(&m, page, GUEST_PAGE_SIZE);
    uint64_t ports[5];
    const uint8_t channels[] = {1, 2, 6, 8, 0};
    for (unsigned i = 0; i < 5; i++) {
        memset(p, 0, 16);
        p[0] = 3; /* Controller speaker: no host audio device in unit tests. */
        p[5] = channels[i];
        CHECK(call2(&m, "sceAudioOut2PortCreate", 0, page, page + 32) == 0);
        memcpy(&ports[i], p + 32, 8);
    }
    for (unsigned repeat = 0; repeat < 2; repeat++) {
        for (unsigned i = 0; i < 5; i++) {
            memset(p + 63, 0xa5, 66);
            CHECK(call2(&m, "sceAudioOut2PortGetState", ports[i], page + 64, 0) == 0);
            CHECK(p[66] == (channels[i] ? channels[i] : 2));
            CHECK(p[63] == 0xa5 && p[128] == 0xa5);
        }
    }
    for (unsigned i = 0; i < 5; i++)
        CHECK(call2(&m, "sceAudioOut2PortDestroy", ports[i], 0, 0) == 0);
    /* A device probe must initialize its entire result. Compare successive
     * probes with different caller-stack contents, as an audio sink does
     * when deciding whether the device changed and must be reopened. */
    uint8_t first[80];
    for (unsigned repeat = 0; repeat < 2; repeat++) {
        memset(p + 127, repeat ? 0x5a : 0xa5, 82);
        CHECK(call2(&m, "sceAudioOut2GetSpeakerInfo", page + 128, 1, 0) == 0);
        uint32_t speakers;
        memcpy(&speakers, p + 132, 4);
        CHECK(speakers != 0); /* an available speaker output */
        CHECK((p[136] & 1) == 0); /* the stereo mixer is not a 3D headset */
        if (!repeat) memcpy(first, p + 128, 80);
        else CHECK(memcmp(first, p + 128, 80) == 0);
        CHECK(p[127] == (repeat ? 0x5a : 0xa5) && p[208] == (repeat ? 0x5a : 0xa5));
    }
    CHECK((uint32_t)call2(&m, "sceAudioOut2GetSpeakerInfo", page + GUEST_PAGE_SIZE - 79, 1, 0)
          == 0x80268001u);
    gmem_free(&m);
    hle_registry_free();
}

int main(void) {
    const test_case tests[] = {TEST_CASE(test_open_initialization_recovery_and_errors),
                              TEST_CASE(test_audio_out2_port_channel_state)};
    return test_run_all("audio_out", tests, PS5C_ARRAY_COUNT(tests));
}
