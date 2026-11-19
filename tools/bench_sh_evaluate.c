/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Standalone CPU pilot. Link against each unmodified Release build.
 * Synthetic resource-discovery workloads; this does not measure GPU or FPS. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/shader/shader.h"

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define DATA_BASE UINT64_C(0x900000000)
#define CODE_BASE UINT64_C(0xa00000000)
#define DATA_WORDS 16384u

typedef struct fixture {
    const char *name;
    sh_program *p;
    sh_eval *e;
    uint32_t *code, words, *data;
    bool loop, calls;
} fixture;

static volatile uint64_t observable;

static void *read_data(void *user, guest_addr addr, uint64_t size) {
    fixture *f = user;
    if (addr < DATA_BASE || addr - DATA_BASE > DATA_WORDS * 4u ||
        size > DATA_WORDS * 4u - (addr - DATA_BASE)) return NULL;
    return (uint8_t *)f->data + (addr - DATA_BASE);
}

static void descriptor(uint32_t *d, guest_addr addr, uint32_t records) {
    d[0] = (uint32_t)addr;
    d[1] = (uint32_t)(addr >> 32) | (4u << 16);
    d[2] = records;
    d[3] = 0x00016204;
}

static void emit(fixture *f, uint32_t word) {
    REQUIRE(f->words < SH_MAX_CODE_DWORDS);
    f->code[f->words++] = word;
}

/* Match observed Minecraft instruction/site scales without distributing its
 * machine code or pretending that synthetic site shapes are game replays.
 * Each resource pair loads an SGPR descriptor then uses it in vector memory. */
static void make_linear(fixture *f, uint32_t instructions, uint32_t sites, bool forks) {
    uint32_t count = 0;
    for (uint32_t site = 0; site < sites;) {
        if (forks) {
            emit(f, 0x7d820501); /* v_cmp_lt_u32: unknown VCC */
            emit(f, 0xbf860001); /* s_cbranch_vccz: skip one scalar move */
            emit(f, 0xbe940380); /* s20 = 0 on the fallthrough path */
            emit(f, 0xbe940381); /* s20 = 1: paths can join */
            count += 4;
        }
        emit(f, 0xf4080200); emit(f, 0xfa000000); /* s_load_dwordx4 s[8:11],s[0:1],0 */
        site++; count++;
        if (site < sites) {
            emit(f, 0x800a810a); /* s_add_u32 s10,s10,1: loaded descriptor dependency */
            count++;
            emit(f, 0xe0302000); emit(f, 0x80020200); /* buffer_load_dword using s[8:11] */
            site++; count++;
        }
        /* Scalar setup mixed with vector ALU which the evaluator skips. */
        emit(f, 0x80148114); /* s_add_u32 s20,s20,1 */
        count++;
        if (count + 1 < instructions) { emit(f, 0x7e020204); count++; }
    }
    REQUIRE(count < instructions);
    while (count + 1 < instructions) {
        emit(f, count % 17u ? 0x7e020204 : 0x80148114);
        count++;
    }
    emit(f, 0xbf810000);
    REQUIRE(sh_analyze(f->code, f->words, CODE_BASE, f->p));
    REQUIRE(f->p->count == instructions && f->p->site_count == sites);
}

static void initialize(fixture *f, const char *name) {
    memset(f, 0, sizeof(*f));
    f->name = name;
    f->p = calloc(1, sizeof(*f->p));
    f->e = calloc(1, sizeof(*f->e));
    f->code = calloc(SH_MAX_CODE_DWORDS, 4);
    f->data = calloc(DATA_WORDS, 4);
    REQUIRE(f->p && f->e && f->code && f->data);
    for (uint32_t k = 0; k < DATA_WORDS; k++) f->data[k] = 0x13570000u + k * 17u;
    descriptor(f->data, DATA_BASE + 0x1000, 64);
    f->e->host = read_data;
    f->e->host_user = f;
    f->e->sgpr_init[0] = (uint32_t)DATA_BASE;
    f->e->sgpr_init[1] = (uint32_t)(DATA_BASE >> 32);
    f->e->sgpr_init[126] = f->e->sgpr_init[127] = UINT32_MAX;
}

static void make_loop(fixture *f) {
    static const uint32_t code[] = { 0xbe880380, 0xf4000100, 0x10000000,
        0x80088408, 0xbf0ac008, 0xbf85fffb, 0xbf810000 };
    memcpy(f->code, code, sizeof(code)); f->words = PS5C_ARRAY_COUNT(code);
    REQUIRE(sh_analyze(f->code, f->words, CODE_BASE, f->p));
    REQUIRE(f->p->site_count == 1 && f->p->back_branch);
    f->loop = true;
}

static void make_calls(fixture *f) {
    static const uint32_t code[] = { 0xbe900308, 0xbe910309, 0xbe8e0400, 0xbe8e210e,
        0xbe90030a, 0xbe91030b, 0xbe8e0400, 0xbe8e210e, 0xbf810000 };
    static const uint32_t leaf[] = { 0xf4000588, 0xfa000000, 0xbefd210e };
    memcpy(f->code, code, sizeof(code)); f->words = PS5C_ARRAY_COUNT(code);
    REQUIRE(sh_analyze(f->code, f->words, CODE_BASE, f->p));
    REQUIRE(sh_append_function(f->p, leaf, PS5C_ARRAY_COUNT(leaf), CODE_BASE + 0x1000, 3));
    REQUIRE(sh_append_function(f->p, leaf, PS5C_ARRAY_COUNT(leaf), CODE_BASE + 0x1000, 7));
    f->e->sgpr_init[0] = (uint32_t)(CODE_BASE + 0x1000);
    f->e->sgpr_init[1] = (uint32_t)((CODE_BASE + 0x1000) >> 32);
    f->e->sgpr_init[8] = (uint32_t)(DATA_BASE + 0x100);
    f->e->sgpr_init[9] = (uint32_t)((DATA_BASE + 0x100) >> 32);
    f->e->sgpr_init[10] = (uint32_t)(DATA_BASE + 0x104);
    f->e->sgpr_init[11] = (uint32_t)((DATA_BASE + 0x104) >> 32);
    f->calls = true;
}

static void input(fixture *f, uint32_t state) {
    f->data[2] = 64u + state;
    f->data[15] = 0x24680000u + state * 1031u;
    f->data[64] = 0x31415900u + state;
    f->data[65] = 0x27182800u ^ state;
    f->e->sgpr_init[20] = state;
}

static uint64_t fold(uint64_t h, uint32_t v) { return (h ^ v) * UINT64_C(1099511628211); }

/* Serialize only defined, observable fields, never structure padding/pointers. */
static uint64_t digest(const fixture *f, uint64_t h) {
    const sh_eval *e = f->e;
    h = fold(h, e->smem_words);
    h = fold(h, e->deferred); h = fold(h, e->guessed); h = fold(h, e->call_pending);
    h = fold(h, e->call_known);
    h = fold(h, (uint32_t)e->call_target); h = fold(h, (uint32_t)(e->call_target >> 32));
    for (uint32_t k = 0; k < e->smem_words; k++) h = fold(h, e->smem_data[k]);
    for (uint32_t k = 0; k < f->p->site_count; k++) {
        const sh_site_value *s = e->site + k;
        h = fold(h, s->valid); h = fold(h, s->smem_offset); h = fold(h, s->soffset);
        h = fold(h, s->soffset_known); h = fold(h, s->desc_unknown); h = fold(h, s->records_unknown);
        for (uint32_t j = 0; j < 8; j++) h = fold(h, s->desc[j]);
        for (uint32_t j = 0; j < 4; j++) h = fold(h, s->sampler[j]);
    }
    return h;
}

static uint64_t verify(fixture *f) {
    uint64_t h = UINT64_C(14695981039346656037);
    for (uint32_t state = 0; state < 16; state++) {
        input(f, state);
        REQUIRE(sh_evaluate(f->p, NULL, f->e));
        for (uint32_t k = 0; k < f->p->site_count; k++) REQUIRE(f->e->site[k].valid);
        if (f->loop) {
            REQUIRE(f->e->smem_data[f->e->site[0].smem_offset] == f->data[15]);
        } else if (f->calls) {
            REQUIRE(f->e->smem_data[f->e->site[0].smem_offset] == f->data[64]);
            REQUIRE(f->e->smem_data[f->e->site[1].smem_offset] == f->data[65]);
        } else {
            REQUIRE(!f->e->site[1].desc_unknown);
            REQUIRE(f->e->site[1].desc[0] == (uint32_t)(DATA_BASE + 0x1000));
            REQUIRE(f->e->site[1].desc[2] == 65u + state);
            REQUIRE(f->e->smem_data[f->e->site[0].smem_offset + 2] == 64u + state);
        }
        h = digest(f, h);
    }
    return h;
}

static double milliseconds(void) {
    LARGE_INTEGER t, frequency;
    REQUIRE(QueryPerformanceCounter(&t) && QueryPerformanceFrequency(&frequency));
    return 1000.0 * (double)t.QuadPart / (double)frequency.QuadPart;
}

int main(int argc, char **argv) {
    uint32_t iterations = 0;
    if (argc == 3 && !strcmp(argv[1], "--iterations")) {
        char *end = NULL; unsigned long n = strtoul(argv[2], &end, 10);
        REQUIRE(end && !*end && n && n <= 10000000ul);
        iterations = (uint32_t)n;
    } else if (!(argc == 2 && !strcmp(argv[1], "--verify"))) {
        fprintf(stderr, "usage: %s --verify | --iterations N\n", argv[0]); return 2;
    }
    fixture cases[6];
    initialize(cases + 0, "small-44-3"); make_linear(cases + 0, 44, 3, false);
    initialize(cases + 1, "medium-166-14"); make_linear(cases + 1, 166, 14, false);
    initialize(cases + 2, "medium-184-17"); make_linear(cases + 2, 184, 17, false);
    initialize(cases + 3, "material-7700-64-forks"); make_linear(cases + 3, 7700, 64, true);
    initialize(cases + 4, "scalar-loop-16-loads"); make_loop(cases + 4);
    initialize(cases + 5, "linked-two-calls"); make_calls(cases + 5);
    for (uint32_t c = 0; c < PS5C_ARRAY_COUNT(cases); c++) {
        fixture *f = cases + c;
        uint64_t checksum = verify(f);
        double elapsed = 0;
        uint64_t sample = 0;
        if (iterations) {
            for (uint32_t k = 0; k < 128; k++) { input(f, k & 15u); REQUIRE(sh_evaluate(f->p, NULL, f->e)); }
            double start = milliseconds();
            for (uint32_t k = 0; k < iterations; k++) {
                input(f, k & 15u);
                REQUIRE(sh_evaluate(f->p, NULL, f->e));
                /* An opaque compiler memory barrier keeps every evaluation's
                 * stores observable even under whole-program LTO. No CPU
                 * fence/instruction or full-buffer hashing enters the timer. */
#ifdef _MSC_VER
                _ReadWriteBarrier();
#else
                __asm__ __volatile__("" : : "r"(f->e), "r"(f->e->smem_data) : "memory");
#endif
                sample += f->e->smem_data[f->e->smem_words - 1] + f->e->site[k % f->p->site_count].desc[2];
            }
            elapsed = milliseconds() - start;
            observable ^= sample;
        }
        printf("{\"case\":\"%s\",\"instructions\":%u,\"sites\":%u,\"checksum\":\"%016" PRIx64
               "\",\"iterations\":%u,\"milliseconds\":%.6f,\"sample\":\"%016" PRIx64 "\"}\n",
               f->name, f->p->count, f->p->site_count, checksum, iterations, elapsed, sample);
        sh_eval_free(f->e); free(f->e); free(f->p); free(f->code); free(f->data);
    }
    return 0;
}
