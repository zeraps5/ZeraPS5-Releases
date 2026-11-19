/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shader corpus files: round trip, rejected files, and the offline check
 * path (analysis and translation of what was read). Synthetic programs only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/shader/shader_corpus.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static const char *const k_path = "test_shader_corpus.tmp.shd";

int main(void) {
    /* Persisted shapes stay 16 bytes when image channel metadata and optional
     * buffer lengths coexist; each site kind owns the union's final byte. */
    CHECK(sizeof(sh_site_shape) == 16);
    static const uint32_t endpgm[] = {0xBF810000u}; /* s_endpgm */
    sh_corpus_entry *e = calloc(1, sizeof(*e)), *r = calloc(1, sizeof(*r));
    sh_program *p = calloc(1, sizeof(*p));
    CHECK(e && r && p);

    /* round trip of a translation input */
    e->kind = SH_CORPUS_TRANSLATION;
    e->base = 0x1234567800ull;
    e->info.stage = SH_STAGE_CS;
    e->info.local_size[0] = 64;
    e->info.local_size[1] = e->info.local_size[2] = 1;
    e->info.cs_tid_components = 1;
    e->info.fp16_overflow = 1;
    e->info.barrier_prefix = true;
    e->info.ps_quad_disable = 13u | SH_PS_QUAD_DEMOTE;
    e->binding_base = 3;
    e->site_count = 2;
    e->shapes[0].buf_stride = 16;
    e->shapes[0].address_map = 1;
    e->shapes[0].buffer_length = 1;
    e->shapes[1].img_type = 9;
    e->shapes[1].img_components = 4;
    e->shapes[1].dst_sel[2] = 6;
    e->shapes[1].ordered_load = 1;
    e->dwords = 1;
    e->code = (uint32_t *)endpgm;
    CHECK(sh_corpus_write(k_path, e));
    CHECK(sh_corpus_read(k_path, r));
    CHECK(r->kind == SH_CORPUS_TRANSLATION && r->base == e->base && r->binding_base == 3);
    CHECK(!memcmp(&r->info, &e->info, sizeof(e->info)));
    CHECK(r->site_count == 2 && !memcmp(r->shapes, e->shapes, sizeof(sh_site_shape) * 2));
    CHECK(r->dwords == 1 && r->code[0] == endpgm[0]);

    /* the offline check: what was read analyzes and translates */
    CHECK(sh_corpus_analyze(r, p));
    uint32_t *spv = NULL;
    size_t words = 0;
    CHECK(sh_translate(p, &r->info, r->shapes, r->binding_base, &spv, &words));
    CHECK(words > 5 && spv[0] == 0x07230203u); /* SPIR-V magic */
    free(spv);
    sh_corpus_free(r);

    /* The packed GPU uniform layout is recorded, while old stage prefixes
     * default to legacy layout even when the destination was not zeroed. */
    {
        e->info.uniform_shift=sh_uniform_compact_shift(p->site_count);
        CHECK(e->info.uniform_shift && sh_corpus_write(k_path,e) && sh_corpus_read(k_path,r));
        CHECK(r->info.uniform_shift==e->info.uniform_shift && !memcmp(&r->info,&e->info,sizeof(e->info)));
        CHECK(sh_corpus_analyze(r,p) && sh_translate(p,&r->info,r->shapes,r->binding_base,&spv,&words));
        free(spv);sh_corpus_free(r);

        const uint64_t magic=0x32304448535a5350ull;
        uint32_t prefix=(uint32_t)offsetof(sh_stage_info,uniform_shift);
        uint32_t head[]={prefix,sizeof(sh_site_shape),e->kind,e->binding_base};
        FILE *old=fopen(k_path,"wb");
        CHECK(old && fwrite(&magic,8,1,old)==1 && fwrite(head,sizeof(head),1,old)==1 &&
            fwrite(&e->base,8,1,old)==1 && fwrite(&e->info,prefix,1,old)==1 &&
            fwrite(&e->site_count,4,1,old)==1 && fwrite(e->shapes,sizeof(sh_site_shape),e->site_count,old)==e->site_count &&
            fwrite(&e->dwords,4,1,old)==1 && fwrite(e->code,4,e->dwords,old)==e->dwords && !fclose(old));
        memset(r,0xa5,sizeof(*r));
        CHECK(sh_corpus_read(k_path,r) && !r->info.uniform_shift && !memcmp(&r->info,&e->info,prefix));
        CHECK(r->site_count==e->site_count && !memcmp(r->shapes,e->shapes,e->site_count*sizeof(sh_site_shape)));
        CHECK(sh_corpus_analyze(r,p) && sh_translate(p,&r->info,r->shapes,r->binding_base,&spv,&words));
        free(spv);sh_corpus_free(r);e->info.uniform_shift=0;
    }

    /* First mesh captures predate indexed/triangle input fields. Keep the
     * original output limits and default the appended contract to point. */
    {
        const uint64_t magic=0x32304448535a5350ull;
        uint32_t prefix=(uint32_t)offsetof(sh_stage_info,mesh_primitive_vertices);
        uint32_t head[]={prefix,sizeof(sh_site_shape),e->kind,e->binding_base};
        e->info.mesh_input_vertices=3;e->info.mesh_max_vertices=3;e->info.mesh_max_primitives=1;
        FILE *old=fopen(k_path,"wb");
        CHECK(old && fwrite(&magic,8,1,old)==1 && fwrite(head,sizeof(head),1,old)==1 &&
            fwrite(&e->base,8,1,old)==1 && fwrite(&e->info,prefix,1,old)==1 &&
            fwrite(&e->site_count,4,1,old)==1 && fwrite(e->shapes,sizeof(sh_site_shape),e->site_count,old)==e->site_count &&
            fwrite(&e->dwords,4,1,old)==1 && fwrite(e->code,4,e->dwords,old)==e->dwords && !fclose(old));
        CHECK(sh_corpus_read(k_path,r) && r->info.mesh_input_vertices==3 && r->info.mesh_max_vertices==3 &&
            r->info.mesh_max_primitives==1 && !r->info.mesh_indexed && !r->info.mesh_primitive_vertices);
        sh_corpus_free(r);
    }

    /* Captures from before POPS specialization have 12-byte site shapes. */
    {
        const uint64_t magic=0x31304448535a5350ull;
        uint32_t old_shape=(uint32_t)offsetof(sh_site_shape,ordered_load);
        uint32_t head[]={sizeof(sh_stage_info),old_shape,e->kind,e->binding_base};
        FILE *old=fopen(k_path,"wb");
        CHECK(old && fwrite(&magic,8,1,old)==1 && fwrite(head,sizeof(head),1,old)==1 &&
            fwrite(&e->base,8,1,old)==1 && fwrite(&e->info,sizeof(e->info),1,old)==1 && fwrite(&e->site_count,4,1,old)==1);
        for(unsigned s=0;s<e->site_count;s++)CHECK(fwrite(&e->shapes[s],old_shape,1,old)==1);
        CHECK(fwrite(&e->dwords,4,1,old)==1 && fwrite(e->code,4,e->dwords,old)==e->dwords && !fclose(old));
        CHECK(sh_corpus_read(k_path,r) && r->shapes[1].dst_sel[2]==6 && !r->shapes[1].ordered_load);
        CHECK(r->code[0]==endpgm[0]);sh_corpus_free(r);
    }

    /* Old captures omit PS export specialization. Keep their original
     * fields and code readable, defaulting the newly added fields to zero. */
    {
        const uint64_t magic = 0x31304448535a5350ull;
        uint32_t old_size = (uint32_t)offsetof(sh_stage_info, ps_col_format);
        uint32_t head[] = {old_size, sizeof(sh_site_shape), e->kind, e->binding_base};
        FILE *old = fopen(k_path, "wb");
        CHECK(old && fwrite(&magic, 8, 1, old) == 1 && fwrite(head, sizeof(head), 1, old) == 1 &&
              fwrite(&e->base, 8, 1, old) == 1 && fwrite(&e->info, old_size, 1, old) == 1 &&
              fwrite(&e->site_count, 4, 1, old) == 1 &&
              fwrite(e->shapes, sizeof(sh_site_shape), e->site_count, old) == e->site_count &&
              fwrite(&e->dwords, 4, 1, old) == 1 && fwrite(e->code, 4, e->dwords, old) == e->dwords);
        CHECK(!fclose(old) && sh_corpus_read(k_path, r));
        CHECK(!r->shapes[0].address_map); /* arbitrary old padding is not a mapping flag */
        CHECK(!memcmp(&r->info, &e->info, old_size) && !r->info.ps_col_format && !r->info.ps_mrt_kind);
        CHECK(r->dwords == 1 && r->code[0] == endpgm[0]);
        sh_corpus_free(r);
        e->info.ps_col_format = 0x27;
        e->info.ps_mrt_kind = 5;
        CHECK(sh_corpus_write(k_path, e) && sh_corpus_read(k_path, r));
        CHECK(r->info.ps_col_format == 0x27 && r->info.ps_mrt_kind == 5);
        sh_corpus_free(r);
    }

    /* a truncated file and a foreign file are rejected */
    FILE *f = fopen(k_path, "r+b");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    char *bytes = malloc((size_t)size);
    CHECK(bytes);
    f = fopen(k_path, "rb");
    CHECK(f && fread(bytes, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    /* Legacy corpus has no FP16_OVFL field. Read its prefix with mode zero,
     * while new files round-trip the mode and cannot alias its cache entry. */
    uint32_t legacy_size = (uint32_t)offsetof(sh_stage_info, fp16_overflow);
    char *legacy = malloc((size_t)size); CHECK(legacy);
    memcpy(legacy, bytes, (size_t)size); memcpy(legacy + 8, &legacy_size, 4);
    f = fopen(k_path, "wb"); CHECK(f);
    CHECK(fwrite(legacy, 1, 32 + legacy_size, f) == 32 + legacy_size);
    CHECK(fwrite(bytes + 32 + sizeof(sh_stage_info), 1, (size_t)size - 32 - sizeof(sh_stage_info), f)
          == (size_t)size - 32 - sizeof(sh_stage_info));
    CHECK(fclose(f) == 0); free(legacy);
    CHECK(sh_corpus_read(k_path, r) && r->info.fp16_overflow == 0 && r->info.local_size[0] == 64);
    CHECK(r->dwords == 1 && r->code[0] == endpgm[0]); sh_corpus_free(r);
    /* The immediately preceding layout has FP16 mode but no coverage mask. */
    legacy_size = (uint32_t)offsetof(sh_stage_info, ps_quad_disable);
    f = fopen(k_path, "wb"); CHECK(f);
    CHECK(fwrite(bytes, 1, 8, f) == 8 && fwrite(&legacy_size, 1, 4, f) == 4);
    CHECK(fwrite(bytes + 12, 1, 20 + legacy_size, f) == 20 + legacy_size);
    CHECK(fwrite(bytes + 32 + sizeof(sh_stage_info), 1, (size_t)size - 32 - sizeof(sh_stage_info), f)
          == (size_t)size - 32 - sizeof(sh_stage_info));
    CHECK(fclose(f) == 0);
    CHECK(sh_corpus_read(k_path, r) && r->info.fp16_overflow == 1 && r->info.ps_quad_disable == 0);
    sh_corpus_free(r);
    f = fopen(k_path, "wb");
    CHECK(f && fwrite(bytes, 1, (size_t)size - 2, f) == (size_t)size - 2);
    fclose(f);
    CHECK(!sh_corpus_read(k_path, r) && !r->code);
    bytes[0] ^= 0xFF;
    f = fopen(k_path, "wb");
    CHECK(f && fwrite(bytes, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    CHECK(!sh_corpus_read(k_path, r));
    free(bytes);

    /* Opcode inference selects wave32 here, while the captured dispatch can
     * select either mode. The offline translator must reproduce that choice. */
    static const uint32_t wave_code[] = {0xbeeb03ff, 0x13572468, 0x7d840080, 0x7e02026b, 0xbf810000};
    memset(e, 0, sizeof(*e));
    e->kind = SH_CORPUS_TRANSLATION;
    e->info.stage = SH_STAGE_CS;
    e->info.local_size[0] = 64; e->info.local_size[1] = e->info.local_size[2] = 1;
    e->info.cs_tid_components = 1;
    e->code = (uint32_t *)wave_code; e->dwords = sizeof(wave_code) / 4;
    CHECK(sh_analyze(e->code, e->dwords, 0, p) && p->wave32);
    for (unsigned mode = 0; mode < 3; mode++) {
        e->info.cs_wave_mode = mode;
        CHECK(sh_corpus_write(k_path, e) && sh_corpus_read(k_path, r));
        CHECK(sh_corpus_analyze(r, p) && p->wave32 == (mode != 1));
        CHECK(sh_translate(p, &r->info, r->shapes, r->binding_base, &spv, &words));
        free(spv); sh_corpus_free(r);
    }
    e->info.stage = SH_STAGE_MESH;
    CHECK(sh_corpus_analyze(e, p) && !p->wave32);
    e->info.mesh_indexed=SH_MESH_FAST_LAUNCH;
    e->info.mesh_compute=1;e->info.mesh_input_vertices=1;
    e->info.mesh_max_vertices=32;e->info.mesh_max_primitives=64;
    CHECK(sh_corpus_write(k_path,e) && sh_corpus_read(k_path,r));
    CHECK(sh_corpus_analyze(r,p) && p->wave32);
    CHECK(sh_translate(p,&r->info,r->shapes,r->binding_base,&spv,&words));
    free(spv);sh_corpus_free(r);
    e->kind = SH_CORPUS_CODE;
    CHECK(sh_corpus_analyze(e, p) && p->wave32);

    /* code whose analysis fails keeps failing offline */
    static const uint32_t bad[] = {0xFFFFFFFFu, 0, 0, 0, 0xBF810000u}; /* unknown encoding */
    memset(e, 0, sizeof(*e));
    e->kind = SH_CORPUS_CODE;
    e->dwords = 5;
    e->code = (uint32_t *)bad;
    CHECK(sh_corpus_write(k_path, e));
    CHECK(sh_corpus_read(k_path, r) && r->kind == SH_CORPUS_CODE && r->site_count == 0);
    CHECK(!sh_corpus_analyze(r, p));
    sh_corpus_free(r);

    char name[32];
    sh_corpus_name(0xABCDEFull, name, sizeof(name));
    CHECK(!strcmp(name, "0000000000abcdef.shd"));
    remove(k_path);
    free(e);
    free(r);
    free(p);
    printf("shader corpus: ok\n");
    return 0;
}
