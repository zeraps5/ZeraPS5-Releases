/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shader corpus files (shader_corpus.h). Layout, little-endian:
 *   u64 magic "PSZSHD02", u32 sizeof(sh_stage_info), u32 sizeof(sh_site_shape),
 *   u32 kind, u32 binding_base, u64 base, sh_stage_info, u32 site_count,
 *   sh_site_shape[site_count], u32 dwords, u32 code[dwords]. */
#include "gpu/shader/shader_corpus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CORPUS_MAGIC_OLD 0x31304448535A5350ull /* "PSZSHD01": address_map was padding */
#define CORPUS_MAGIC 0x32304448535A5350ull /* "PSZSHD02" */
#define MAX_CODE_DWORDS 0x10000

void sh_corpus_name(uint64_t key, char *out, size_t size) {
    snprintf(out, size, "%016llx.shd", (unsigned long long)key);
}

typedef struct cursor {
    uint8_t *w;       /* encoding */
    const uint8_t *r; /* decoding */
    size_t at, size;
} cursor;

static bool put(cursor *c, const void *data, size_t size) {
    if (size > c->size - c->at) return false;
    memcpy(c->w + c->at, data, size);
    c->at += size;
    return true;
}
static bool get(cursor *c, void *data, size_t size) {
    if (size > c->size - c->at) return false;
    memcpy(data, c->r + c->at, size);
    c->at += size;
    return true;
}

size_t sh_corpus_encoded_size(const sh_corpus_entry *e) {
    return 8 + 16 + 8 + sizeof(e->info) + 4 + sizeof(sh_site_shape) * e->site_count + 4 + (size_t)e->dwords * 4;
}

bool sh_corpus_encode(const sh_corpus_entry *e, uint8_t *out, size_t cap) {
    if (e->site_count > SH_MAX_SITES || e->dwords > MAX_CODE_DWORDS || cap < sh_corpus_encoded_size(e)) return false;
    cursor c = {.w = out, .size = cap};
    uint64_t magic = CORPUS_MAGIC, base = e->base;
    uint32_t head[4] = {(uint32_t)sizeof(sh_stage_info), (uint32_t)sizeof(sh_site_shape), (uint32_t)e->kind,
                        e->binding_base};
    return put(&c, &magic, 8) && put(&c, head, sizeof(head)) && put(&c, &base, 8) &&
           put(&c, &e->info, sizeof(e->info)) && put(&c, &e->site_count, 4) &&
           put(&c, e->shapes, sizeof(sh_site_shape) * e->site_count) && put(&c, &e->dwords, 4) &&
           put(&c, e->code, (size_t)e->dwords * 4);
}

bool sh_corpus_write(const char *path, const sh_corpus_entry *e) {
    size_t size = sh_corpus_encoded_size(e);
    uint8_t *data = (uint8_t *)malloc(size);
    if (!data || !sh_corpus_encode(e, data, size)) {
        free(data);
        return false;
    }
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(data, 1, size, f) == size;
    if (f && fclose(f) != 0) ok = false;
    if (f && !ok) remove(path);
    free(data);
    return ok;
}

bool sh_corpus_read(const char *path, sh_corpus_entry *e) {
    memset(e, 0, sizeof(*e));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fseek(f, 0, SEEK_END) == 0;
    long size = ok ? ftell(f) : -1;
    uint8_t *data = size > 0 && size <= (long)(64u << 20) ? (uint8_t *)malloc((size_t)size) : NULL;
    ok = data && fseek(f, 0, SEEK_SET) == 0 && fread(data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    ok = ok && sh_corpus_decode(data, (size_t)size, e);
    free(data);
    return ok;
}

bool sh_corpus_decode(const uint8_t *data, size_t size, sh_corpus_entry *e) {
    memset(e, 0, sizeof(*e));
    cursor c = {.r = data, .size = data ? size : 0};
    cursor *f = &c;
    uint64_t magic = 0, base = 0;
    uint32_t head[4] = {0};
    bool ok = get(f, &magic, 8) && (magic == CORPUS_MAGIC || magic == CORPUS_MAGIC_OLD) && get(f, head, sizeof(head)) &&
              (head[0] == sizeof(sh_stage_info) || head[0] == offsetof(sh_stage_info, uniform_shift) || head[0] == offsetof(sh_stage_info, mesh_compute) || head[0] == offsetof(sh_stage_info, mesh_primitive_vertices) ||
               head[0] == offsetof(sh_stage_info, mesh_input_vertices) || head[0] == offsetof(sh_stage_info, vs_param_alias_mask) ||
               head[0] == offsetof(sh_stage_info, host_local_size) ||
               head[0] == offsetof(sh_stage_info, vs_ucp_mask) ||
               head[0] == offsetof(sh_stage_info, vs_point_size) ||
               head[0] == offsetof(sh_stage_info, ps_quad_disable) ||
               head[0] == offsetof(sh_stage_info, fp16_overflow) ||
               head[0] == offsetof(sh_stage_info, ps_col_format)) &&
              (head[1] == sizeof(sh_site_shape) || head[1] == offsetof(sh_site_shape, ordered_load)) &&
              (head[2] == SH_CORPUS_TRANSLATION || head[2] == SH_CORPUS_CODE) && get(f, &base, 8) &&
              get(f, &e->info, head[0]) && get(f, &e->site_count, 4) && e->site_count <= SH_MAX_SITES;
    for(uint32_t s=0;ok && s<e->site_count;s++) {
        ok=get(f,&e->shapes[s],head[1]);
        if(magic==CORPUS_MAGIC_OLD)e->shapes[s].address_map=0;
    }
    ok=ok && get(f,&e->dwords,4) && e->dwords && e->dwords<=MAX_CODE_DWORDS;
    if (ok) {
        e->kind = (sh_corpus_kind)head[2];
        e->binding_base = head[3];
        e->base = base;
        e->code = (uint32_t *)malloc((size_t)e->dwords * 4);
        ok = e->code && get(f, e->code, (size_t)e->dwords * 4);
    }
    if (!ok) sh_corpus_free(e);
    return ok;
}

bool sh_corpus_analyze(const sh_corpus_entry *e, sh_program *p) {
    if (!sh_analyze(e->code, e->dwords, e->base, p)) return false;
    if (e->kind == SH_CORPUS_TRANSLATION) {
        if (e->info.stage == SH_STAGE_MESH) p->wave32 = (e->info.mesh_indexed&SH_MESH_FAST_LAUNCH)!=0;
        else if (e->info.stage == SH_STAGE_CS && (e->info.cs_wave_mode == 1 || e->info.cs_wave_mode == 2))
            p->wave32 = e->info.cs_wave_mode == 2;
    }
    return true;
}
void sh_corpus_free(sh_corpus_entry *e) {
    free(e->code);
    memset(e, 0, sizeof(*e));
}
