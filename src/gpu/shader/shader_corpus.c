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

static bool put(FILE *f, const void *data, size_t size) { return fwrite(data, 1, size, f) == size; }
static bool get(FILE *f, void *data, size_t size) { return fread(data, 1, size, f) == size; }

bool sh_corpus_write(const char *path, const sh_corpus_entry *e) {
    if (e->site_count > SH_MAX_SITES || e->dwords > MAX_CODE_DWORDS) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint64_t magic = CORPUS_MAGIC, base = e->base;
    uint32_t head[4] = {(uint32_t)sizeof(sh_stage_info), (uint32_t)sizeof(sh_site_shape), (uint32_t)e->kind,
                        e->binding_base};
    bool ok = put(f, &magic, 8) && put(f, head, sizeof(head)) && put(f, &base, 8) &&
              put(f, &e->info, sizeof(e->info)) && put(f, &e->site_count, 4) &&
              put(f, e->shapes, sizeof(sh_site_shape) * e->site_count) && put(f, &e->dwords, 4) &&
              put(f, e->code, (size_t)e->dwords * 4);
    if (fclose(f) != 0) ok = false;
    if (!ok) remove(path);
    return ok;
}

bool sh_corpus_read(const char *path, sh_corpus_entry *e) {
    memset(e, 0, sizeof(*e));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
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
    fclose(f);
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
