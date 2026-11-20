/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shader corpus: the translation inputs of the shaders a game ran, one file
 * per distinct input (PS5C_SHADER_CORPUS=1 writes them in user/<game>/corpus),
 * for offline analysis with `ps5c shaders <dir>`. The files hold game code:
 * they stay on the machine, never in the repository. */
#ifndef PS5C_SHADER_CORPUS_H
#define PS5C_SHADER_CORPUS_H

#include "gpu/shader/shader.h"

typedef enum sh_corpus_kind {
    SH_CORPUS_TRANSLATION = 1, /* analyzed code + everything sh_translate takes */
    SH_CORPUS_CODE = 2,        /* code whose analysis failed (no stage inputs) */
} sh_corpus_kind;

typedef struct sh_corpus_entry {
    sh_corpus_kind kind;
    guest_addr base;
    sh_stage_info info;
    uint32_t binding_base;
    uint32_t site_count;
    sh_site_shape shapes[SH_MAX_SITES];
    uint32_t dwords;
    uint32_t *code; /* sh_corpus_read: malloc'ed (sh_corpus_free) */
} sh_corpus_entry;

/* The file name of an entry: <key as 16 hex digits>.shd */
void sh_corpus_name(uint64_t key, char *out, size_t size);
/* Writes `e` to `path` (replacing it). False on I/O errors. */
bool sh_corpus_write(const char *path, const sh_corpus_entry *e);
/* Reads an entry. False when the file is not one, or was written by a build
 * whose structures differ (sizes are checked). */
bool sh_corpus_read(const char *path, sh_corpus_entry *e);
/* Reanalyze code, then restore the recorded stage's explicit guest wave
 * contract. Old/code-only captures keep opcode inference. */
bool sh_corpus_analyze(const sh_corpus_entry *e, sh_program *p);
void sh_corpus_free(sh_corpus_entry *e);
/* The same layout in memory (vk_shader_cache.c keeps entries in one file):
 * the encoded size, encoding into `out` (cap bytes) and decoding (code is
 * malloc'ed, sh_corpus_free). Decoding accepts the sizes sh_corpus_read does. */
size_t sh_corpus_encoded_size(const sh_corpus_entry *e);
bool sh_corpus_encode(const sh_corpus_entry *e, uint8_t *out, size_t cap);
bool sh_corpus_decode(const uint8_t *data, size_t size, sh_corpus_entry *e);

#endif
