/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
#include <string.h>
#include "cpu/x64_compat.h"
#include "test.h"
static bool compat_off;
static bool compat_auto;
static bool guest_tsc;

/* Assemble native reference instructions without enabling AVX2 for the host
 * binary. Both runs execute the same bytes, before/after the guest patcher.
 * Only volatile Win64 registers are used, including in the vector fixtures. */
#if defined(__GNUC__) && defined(__x86_64__) && defined(_WIN32)
#define FIXTURE(name, body) \
    extern const unsigned char name[], name##_end[]; \
    __asm__(".text\n.p2align 4\n.global " #name "\n" #name ":\n" \
            body "\nret\n.global " #name "_end\n" #name "_end:\n")
#define SCALAR(name, insn, flags) FIXTURE(name, \
    "mov %rcx,%r8\nmov (%r8),%rcx\nmov 8(%r8),%rdx\n" \
    insn "\nmov %rax,16(%r8)\npushfq\npop %r9\nand $" #flags ",%r9\nmov %r9,24(%r8)")
SCALAR(f_andn, "andn %rdx,%rcx,%rax", 2241);
SCALAR(f_blsi, "blsi %rcx,%rax", 2241);
SCALAR(f_blsr, "blsr %rcx,%rax", 2241);
SCALAR(f_blsmsk, "blsmsk %rcx,%rax", 2241);
SCALAR(f_bextr, "bextr %rdx,%rcx,%rax", 2113);
SCALAR(f_bzhi, "bzhi %rdx,%rcx,%rax", 2241);
SCALAR(f_pext, "pext %rdx,%rcx,%rax", 0);
SCALAR(f_pdep, "pdep %rdx,%rcx,%rax", 0);
SCALAR(f_shlx, "shlx %rdx,%rcx,%rax", 0);
SCALAR(f_shrx, "shrx %rdx,%rcx,%rax", 0);
SCALAR(f_sarx, "sarx %rdx,%rcx,%rax", 0);
SCALAR(f_rorx, "rorx $13,%rcx,%rax", 0);
SCALAR(f_lzcnt, "lzcnt %rcx,%rax", 65);
SCALAR(f_tzcnt, "tzcnt %rcx,%rax", 65);
SCALAR(f_lzcnt32, "lzcnt %ecx,%eax", 65);
SCALAR(f_tzcnt32, "tzcnt %ecx,%eax", 65);
SCALAR(f_lzcnt16, "mov $-1,%rax\nlzcnt %cx,%ax", 65);
SCALAR(f_tzcnt16, "mov $-1,%rax\ntzcnt %cx,%ax", 65);
SCALAR(f_mulx, "mulx %rcx,%r10,%rax\nxor %r10,%rax", 0);
FIXTURE(f_movbe, "movbe (%rcx),%rax\nmov %rax,16(%rcx)\nmovbe %rax,24(%rcx)");
FIXTURE(f_short, "mov (%rcx),%eax\nlzcnt %eax,%eax\nadd $7,%eax\nmov %eax,16(%rcx)");
/* Branch into the would-be borrowed instruction: it must remain a UD2 site. */
FIXTURE(f_target, "mov (%rcx),%eax\ncmpl $0,8(%rcx)\njnz 1f\nlzcnt %eax,%eax\n1: add $7,%eax\nmov %eax,16(%rcx)");
/* RIP-relative read borrowed after a four-byte LZCNT. */
FIXTURE(f_rip, "mov (%rcx),%eax\nlzcnt %eax,%eax\nadd 1f(%rip),%eax\nmov %eax,16(%rcx)\nret\n1: .long 7");
#define VECTOR(name, insn) FIXTURE(name, \
    "vmovdqu (%rcx),%ymm0\nvmovdqu 32(%rcx),%ymm1\nvmovdqu 64(%rcx),%ymm2\n" \
    insn "\nvmovdqu %ymm0,96(%rcx)\nvmovdqu %ymm1,128(%rcx)\nvmovdqu %ymm2,160(%rcx)\nvzeroupper")
VECTOR(f_add, "vpaddd %ymm1,%ymm0,%ymm2");
VECTOR(f_shuffle, "vpshufb %ymm1,%ymm0,%ymm2");
VECTOR(f_shift, "vpsllvd %ymm1,%ymm0,%ymm2");
VECTOR(f_perm, "vpermd %ymm1,%ymm0,%ymm2");
VECTOR(f_broadcast, "vpbroadcastb %xmm0,%ymm2");
VECTOR(f_fma, "vfmadd213ps %ymm0,%ymm1,%ymm2");
VECTOR(f_extract, "vextracti128 $1,%ymm0,%xmm2");
VECTOR(f_gather, "vpgatherdd %ymm1,(%rcx,%ymm0,4),%ymm2");

#define TSC_FIXTURE(name, insn) FIXTURE(name, \
    "mov %rcx,%r8\nmov $-1,%rax\nmov %rax,%rcx\nmov %rax,%rdx\nstc\npushfq\npop %r9\n" \
    insn "\nmov %rax,(%r8)\nmov %rdx,8(%r8)\nmov %rcx,16(%r8)\npushfq\npop %r10\n" \
    "mov %r9,24(%r8)\nmov %r10,32(%r8)")
TSC_FIXTURE(f_rdtsc, "rdtsc");
TSC_FIXTURE(f_rdtscp, "rdtscp");

typedef void (*fixture_fn)(uint64_t *);
typedef struct fixture { const char *name; const unsigned char *start, *end; } fixture;
#define ENTRY(name) {#name, name, name##_end}
static const fixture fixtures[] = {
    ENTRY(f_andn), ENTRY(f_blsi), ENTRY(f_blsr), ENTRY(f_blsmsk), ENTRY(f_bextr),
    ENTRY(f_bzhi), ENTRY(f_pext), ENTRY(f_pdep), ENTRY(f_shlx), ENTRY(f_shrx),
    ENTRY(f_sarx), ENTRY(f_rorx), ENTRY(f_lzcnt), ENTRY(f_tzcnt), ENTRY(f_lzcnt32),
    ENTRY(f_tzcnt32), ENTRY(f_lzcnt16), ENTRY(f_tzcnt16), ENTRY(f_mulx), ENTRY(f_movbe),
    ENTRY(f_short), ENTRY(f_target), ENTRY(f_rip), ENTRY(f_add), ENTRY(f_shuffle),
    ENTRY(f_shift), ENTRY(f_perm), ENTRY(f_broadcast), ENTRY(f_fma), ENTRY(f_extract), ENTRY(f_gather)
};
static uint64_t seed = 0x123456789abcdef0ull;
static uint64_t random64(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }

static bool fault(void *unused, pal_fault_info *info) { (void)unused; return x64_compat_fault(info); }

static void guest_tsc_instructions(void) {
    gmem m;
    gmem_config cfg = gmem_default_config();
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr code = 0;
    CHECK(gmem_map(&m, 0, GUEST_PAGE_SIZE, GPROT_RW | GPROT_EXEC, 0, "TSC fixtures", &code) == PS5C_OK);
    uint8_t *p = gmem_host(&m, code, GUEST_PAGE_SIZE);
    const fixture tsc[] = {ENTRY(f_rdtsc), ENTRY(f_rdtscp)};
    enum { STRIDE = 64 };
    memset(p, 0x90, GUEST_PAGE_SIZE);
    CHECK(pal_fault_handler_install(fault, NULL) == PS5C_OK);
    for (unsigned i = 0; i < PS5C_ARRAY_COUNT(tsc); i++) {
        size_t size = (size_t)(tsc[i].end - tsc[i].start);
        CHECK(size <= STRIDE);
        void *entry = p + i * STRIDE;
        memcpy(entry, tsc[i].start, size);
        x64_compat_patch(&m, tsc[i].name, code + i * STRIDE, STRIDE, code + GUEST_PAGE_SIZE);
        CHECK(memcmp(entry, tsc[i].start, size) != 0);
        fixture_fn fn;
        memcpy(&fn, &entry, sizeof(fn));
        uint64_t out[5] = {0};
        uint64_t before = x64_guest_tsc_read();
        fn(out);
        uint64_t after = x64_guest_tsc_read();
        CHECK(out[0] <= UINT32_MAX && out[1] <= UINT32_MAX);
        uint64_t ticks = out[0] | (out[1] << 32);
        CHECK(before <= ticks && ticks <= after);
        CHECK(i == 0 ? out[2] == UINT64_MAX : out[2] <= UINT32_MAX);
        CHECK(out[3] == out[4] && (out[4] & 1));
    }
    CHECK(pal_fault_handler_install(NULL, NULL) == PS5C_OK);
    gmem_free(&m);
}

static void differential(bool off) {
    __builtin_cpu_init();
    if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("bmi") || !__builtin_cpu_supports("bmi2") ||
        !__builtin_cpu_supports("lzcnt") || !__builtin_cpu_supports("movbe") || !__builtin_cpu_supports("fma")) {
        puts("Native differential reference skipped: host lacks reference ISA (decoder still tested)");
        return;
    }
    if (compat_auto) CHECK(x64_compat_missing() == 0);
    gmem m;
    gmem_config cfg = gmem_default_config();
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    CHECK(pal_fault_handler_install(fault, NULL) == PS5C_OK);
    enum { CASES = 64, WORDS = 32, STRIDE = 256 };
    uint64_t input[CASES][WORDS], expected[CASES][WORDS];
    guest_addr code = 0;
    CHECK(gmem_map(&m, 0, GUEST_PAGE_SIZE, GPROT_RW | GPROT_EXEC, 0, "compat fixtures", &code) == PS5C_OK);
    uint8_t *p = gmem_host(&m, code, GUEST_PAGE_SIZE);
    memset(p, 0x90, GUEST_PAGE_SIZE);
    for (unsigned f = 0; f < PS5C_ARRAY_COUNT(fixtures); f++) {
        const fixture *fx = &fixtures[f];
        size_t len = (size_t)(fx->end - fx->start);
        CHECK(len < STRIDE);
        memcpy(p + f * STRIDE, fx->start, len);
    }
    pal_icache_flush(p, GUEST_PAGE_SIZE);
    /* Independent segments exercise metadata lookup across many stub areas. */
    uint8_t original[GUEST_PAGE_SIZE];
    memcpy(original, p, sizeof(original));
    for (unsigned f = 0; f < PS5C_ARRAY_COUNT(fixtures); f++) {
        fixture_fn fn;
        void *entry = p + f * STRIDE;
        memcpy(&fn, &entry, sizeof(fn));
        for (unsigned n = 0; n < CASES; n++) {
            for (unsigned w = 0; w < WORDS; w++) input[n][w] = random64();
            /* Finite exact floats for FMA and bounded VSIB addresses. */
            if (fixtures[f].start == f_fma) {
                float *v = (float *)input[n];
                for (int i = 0; i < 24; i++) v[i] = (float)(i - 11) * 0.25f;
            }
            if (fixtures[f].start == f_gather) {
                int32_t *ix = (int32_t *)input[n];
                for (int i = 0; i < 8; i++) ix[i] = (i * 3) % 24;
            }
            if (n < 4 && f < 23) { input[n][0] = n == 0 ? 0 : n == 1 ? 1 : n == 2 ? UINT64_MAX : 1ull << 63; }
            if (fixtures[f].start == f_target) input[n][1] = n & 1;
            memcpy(expected[n], input[n], sizeof(input[n]));
            fn(expected[n]);
        }
        x64_compat_patch(&m, fixtures[f].name, code + f * STRIDE, STRIDE, code + GUEST_PAGE_SIZE);
        for (unsigned n = 0; n < CASES; n++) {
            uint64_t actual[WORDS];
            memcpy(actual, input[n], sizeof(actual));
            fn(actual);
            if (memcmp(actual, expected[n], sizeof(actual))) {
                fprintf(stderr, "%s differs at input %u\n", fixtures[f].name, n);
                CHECK(false);
            }
        }
    }
    CHECK(off || compat_auto ? !memcmp(original, p, sizeof(original)) : memcmp(original, p, sizeof(original)) != 0);
    CHECK(pal_fault_handler_install(NULL, NULL) == PS5C_OK);
    gmem_free(&m);
}
#endif

static void decoder(void) {
    x64_insn in;
    const uint8_t lzcnt[] = {0xf3,0x48,0x0f,0xbd,0xc1};
    CHECK(x64_decode(lzcnt, sizeof(lzcnt), &in));
    CHECK(in.len == 5 && in.reg == 0 && in.rm == 1 && x64_compat_set(&in) == X64_SET_LZCNT);
    for (size_t n = 0; n < sizeof(lzcnt); n++) CHECK(!x64_decode(lzcnt,n,&in));
    const uint8_t rip[] = {0x48,0x8b,0x05,0x10,0,0,0};
    CHECK(x64_decode(rip,sizeof(rip),&in));
    CHECK(in.rip_rel && in.disp_off == 3 && in.disp == 16 && in.len == 7);
    const uint8_t rdtsc[] = {0x0f, 0x31};
    const uint8_t rdtscp[] = {0x0f, 0x01, 0xf9};
    CHECK(x64_decode(rdtsc, sizeof(rdtsc), &in) && x64_compat_set(&in) == X64_SET_TSC);
    CHECK(x64_decode(rdtscp, sizeof(rdtscp), &in) && x64_compat_set(&in) == X64_SET_TSC);
}

static void virtual_clock(void) {
    CHECK(pal_init() == PS5C_OK);
    CHECK(x64_guest_tsc_enabled());
    uint64_t before = pal_time_ns();
    uint64_t ticks = x64_guest_tsc_read();
    uint64_t after = pal_time_ns();
    CHECK(before * 8 / 5 <= ticks && ticks <= after * 8 / 5);
#if defined(__GNUC__) && defined(__x86_64__) && defined(_WIN32)
    guest_tsc_instructions();
#endif
}

static void execution(void) {
#if defined(__GNUC__) && defined(__x86_64__) && defined(_WIN32)
    if (!compat_auto) CHECK(compat_off ? x64_compat_missing() == 0 : x64_compat_missing() == 63);
    differential(compat_off);
#endif
}

int main(int argc, char **argv) {
    guest_tsc = argc > 1 && !strcmp(argv[1], "--guest-tsc");
    compat_off = guest_tsc || (argc > 1 && !strcmp(argv[1], "--off"));
    compat_auto = argc > 1 && !strcmp(argv[1], "--auto");
#ifdef _WIN32
    _putenv_s("PS5C_CPU_COMPAT", compat_auto ? "" : compat_off ? "off" : "ivb");
    _putenv_s("PS5C_CPU_COMPAT_TRAPALL", argc > 1 && !strcmp(argv[1], "--trap") ? "1" : "");
#endif
    if (guest_tsc) {
        const test_case clock_tests[] = {{"guest TSC rate and instruction state", virtual_clock}};
        return test_run_all("guest_tsc", clock_tests, PS5C_ARRAY_COUNT(clock_tests));
    }
    const test_case tests[] = {{"decoder bounds and operands", decoder}, {"native/compatible execution", execution}};
    return test_run_all("cpu_compat", tests, PS5C_ARRAY_COUNT(tests));
}
