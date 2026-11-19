/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Published test vectors: FIPS 197 (AES), IEEE 1619 (XTS), FIPS 180 and
 * RFC 4231 (SHA-256, HMAC), FIPS 202 examples (SHA3-256). */
#include <stdlib.h>
#include <string.h>

#include "base/crypto.h"
#include "test.h"

static void unhex(const char *s, uint8_t *out) {
    for (size_t i = 0; s[2 * i]; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static bool same(const uint8_t *d, const char *hex) {
    uint8_t want[256];
    size_t n = strlen(hex) / 2;
    unhex(hex, want);
    return memcmp(d, want, n) == 0;
}

static void test_aes128(void) {
    uint8_t key[16], pt[16], out[16];
    unhex("000102030405060708090a0b0c0d0e0f", key);
    unhex("00112233445566778899aabbccddeeff", pt);
    aes128 a;
    aes128_init(&a, key);
    aes128_encrypt(&a, pt, out);
    CHECK(same(out, "69c4e0d86a7b0430d8cdb78070b4c55a"));
    aes128_decrypt(&a, out, out);
    CHECK(memcmp(out, pt, 16) == 0);
}

static void test_xts(void) {
    uint8_t k1[16] = {0}, k2[16] = {0}, buf[32] = {0};
    aes_xts x;
    aes_xts_init(&x, k1, k2);
    aes_xts_encrypt(&x, buf, 32, 0);
    CHECK(same(buf, "917cf69ebd68b2ec9b9fe9a3eadda692cd43d2f59598ed858c02c2652fbf922e"));
    aes_xts_decrypt(&x, buf, 32, 0);
    for (int i = 0; i < 32; i++) CHECK(buf[i] == 0);

    memset(k1, 0x11, 16);
    memset(k2, 0x22, 16);
    memset(buf, 0x44, 32);
    aes_xts_init(&x, k1, k2);
    aes_xts_encrypt(&x, buf, 32, 0x3333333333ull);
    CHECK(same(buf, "c454185e6a16936e39334038acef838bfb186fff7480adc4289382ecd6d394f0"));
    aes_xts_decrypt(&x, buf, 32, 0x3333333333ull);
    for (int i = 0; i < 32; i++) CHECK(buf[i] == 0x44);
}

static void test_sha256(void) {
    uint8_t d[32];
    sha256("abc", 3, d);
    CHECK(same(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    sha256("", 0, d);
    CHECK(same(d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(two, strlen(two), d);
    CHECK(same(d, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    uint8_t key[20];
    memset(key, 0x0b, sizeof key);
    hmac_sha256(key, sizeof key, "Hi There", 8, d);
    CHECK(same(d, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    const char *msg = "what do ya want for nothing?";
    hmac_sha256("Jefe", 4, msg, strlen(msg), d);
    CHECK(same(d, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
}

static void test_sha3(void) {
    uint8_t d[32], a3[200];
    sha3_256("", 0, d);
    CHECK(same(d, "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a"));
    sha3_256("abc", 3, d);
    CHECK(same(d, "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532"));
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha3_256(two, strlen(two), d);
    CHECK(same(d, "41c0dba2a9d6240849100376a8235e2c82e1b9998a999e21db32dd97496d3376"));
    memset(a3, 0xa3, sizeof a3); /* longer than one 136-byte block */
    sha3_256(a3, sizeof a3, d);
    CHECK(same(d, "79f38adec5c20307a98ef76e8324afbfd46cfd81b22e3973c65fa1bd9de31787"));
}

int main(void) {
    static const test_case tests[] = {
        TEST_CASE(test_aes128),
        TEST_CASE(test_xts),
        TEST_CASE(test_sha256),
        TEST_CASE(test_sha3),
    };
    return test_run_all("crypto", tests, PS5C_ARRAY_COUNT(tests));
}
