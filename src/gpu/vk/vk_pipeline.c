/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Draws and dispatches: guest register state -> Vulkan pipelines.
 *
 * Per draw: the guest programs are analyzed (cached by address and code
 * hash), their scalar part is evaluated with the current user data to find
 * the descriptors, and a pipeline is looked up by everything the translated
 * code and the fixed-function state depend on. Scalar-memory values and
 * copies of the buffers the shaders read go into a per-batch host-visible
 * arena; images are the renderer's guest surface cache.
 *
 * Depth: DB_Z_INFO's surface is a host depth image (test, write, clears);
 * stencil is emulated for 32-bit float depth with an 8-bit stencil (D32_SFLOAT_S8_UINT).
 *
 * Not yet: stencil, MSAA (targets are single-sampled; a CB resolve is
 * a copy), tiled texture uploads, blending constants, streamout. */
#include <emmintrin.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stddef.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/hashmap.h"
#include "base/log.h"
#include "gpu/gcn.h"
#include "gpu/gpu_regs.h"
#include "gpu/gpu_watch.h"
#include "gpu/mesh_indices.h"
#include "gpu/program_storage.h"
#include "gpu/shader/shader.h"
#include "gpu/shader/shader_corpus.h"
#include "gpu/shader/shader_ops.h"
#include "gpu/shader/shader_dynamic.h"
#include "gpu/vk/vk_vertex_ranges.h"
#include "gpu/vk/vk_texture_layout.h"
#include "gpu/vk/vk_texture_tiling.h"
#include "gpu/vk/vk_texture_3d.h"
#include "gpu/vk/vk_writeback_merge.h"
#include "gpu/vk/vk_internal.h"
#include "gpu/vk/vk_guest_alias.h"
#include "gpu/vk/vk_stencil.h"
#include "gpu/vk/vk_resource_limits.h"
#include "gpu/vk/vk_descriptor_plan.h"
#include "gpu/vk/vk_spirv_bindings.h"
#include "gpu/vk/vk_image_formats.h"
#include "gpu/vk/vk_phys_import.h"
#include "gpu/vk/vk_packed_formats.h"
#include "gpu/vk/vk_depth_policy.h"
#include "gpu/vk/vk_color_target.h"
#include "gpu/vk/vk_dcc_metadata.h"
#include "gpu/vk/vk_fill_clear.h"
#include "gpu/vk/vk_pipeline_async.h"
#include "gpu/vk/vk_shader_cache.h"
#include "gpu/vk/vk_shader_code.h"
#include "gpu/vk/vk_spirv_interface.h"
#include "gpu/vk/vk_compute_dispatch.h"
#include "gpu/vk/vk_compute_shape.h"
#include "gpu/vk/vk_ngg_group.h"
#include "gpu/vk/vk_ngg_fast_launch.h"
#include "gpu/vk/texture_hash.h"
#include "gpu/vk/vk_snapshot_refresh.h"
#include "gpu/vk/vk_spirv_interface.h"
#include "pal/pal.h"
#include "gpu/vk/vk_ngg_large_reads.h"

void runtime_watch_move(gmem *m, guest_addr addr); /* core/runtime.c (its header clashes with names here) */
/* PS5C_LOG_STORE_ADDR: the address and the buffers the shaders storing there read (chased in turn) */
static guest_addr g_store_chase[5];
static uint32_t g_store_chased;
guest_addr runtime_debug_addr(void);

/* PS5C_GPU_STATS: time spent per step, logged every second. */
enum { ST_DRAW, ST_DISPATCH, ST_PROGRAM, ST_EVAL, ST_BUFFERS, ST_TEXHASH, ST_UPLOAD, ST_PIPE, ST_SYNC, ST_GPU_WAIT, ST_COUNT };
static const char *const k_stat_names[ST_COUNT] = {"draw", "dispatch", "program", "eval", "buffers", "texhash", "upload", "pipeline", "sync", "gpuwait"};
static int g_stats_on = -1;
static uint64_t g_trace_flip; /* current guest flip; also gates optional profiling */
static uint64_t g_stat_ns[ST_COUNT], g_stat_n[ST_COUNT], g_stat_bytes, g_stat_last;
static uint64_t g_stat_saved_bytes, g_stat_trimmed;
static uint64_t g_stat_held, g_stat_held_forced; /* releases held; batches a held release had to complete early */
static uint64_t g_stat_held_ordered; /* of those, before a command processor write (renderer_cp_write) */
static uint64_t g_stat_partial; /* reads that overlapped pending results in part (batch completed first) */
static uint64_t g_stat_patched; /* such reads copied from guest memory with the pending results put in on the GPU */
static uint64_t g_stat_texskip; /* texture comparisons gpu_watch made unnecessary */
static uint64_t g_stat_pins, g_stat_pin_ns, g_stat_map_epochs; /* CPU access scopes: count, time to pin, mapping changes seen */
/* resident buffers (resident_buf): statistics per second, and what is resident now */
static uint64_t g_res_binds, g_res_upload, g_res_created, g_res_applied, g_res_merged, g_res_fallbacks;
static _Atomic uint64_t g_res_saved, g_res_lost;
static uint64_t g_res_live_bytes;
static uint32_t g_res_live;
static bool g_stencil; /* renderer->stencil: guest stencil emulated (vk_pipeline_init) */
static bool g_gcn;     /* the current draw is a PS4 one: its T# word 7 holds the GCN tiling index */

/* A T#'s swizzle mode; for a PS4 draw, the renderer's value for its GCN tiling index */
static uint32_t tex_sw(const uint32_t *t) {
    if (!g_gcn) return (t[3] >> 20) & 0x1F;
    switch (t[7] & 0x1F) {
    case 8: return GCN_SW_LINEAR;      /* linear aligned */
    case 9: return GCN_SW_1D_DISPLAY;  /* display 1D thin */
    case 10: return GCN_SW_2D_DISPLAY; /* display 2D thin */
    case 13: return GCN_SW_1D_THIN;    /* thin 1D thin */
    case 14: return GCN_SW_2D_THIN;    /* thin 2D thin */
    default: return GCN_SW_UNSUPPORTED; /* depth, PRT, thick and linear general modes: not yet */
    }
}
static uint64_t g_stat_stencil_draws, g_stat_stencil_info; /* draws with STENCIL_ENABLE; last DB_STENCIL_INFO */
static uint64_t g_stat_patch_shared;
static uint64_t g_stat_copy_by[5]; /* other, patched, shared read, store, index */
static int g_copy_tag;
static uint64_t g_stat_vc_hits, g_stat_vc_copies, g_stat_vc_skips, g_stat_vc_resets, g_stat_vc_arm_ns, g_stat_vc_copy_ns, g_stat_vc_elig_ns, g_stat_vc_bytes;
static uint64_t g_stat_ro_hits, g_stat_ro_misses, g_stat_ro_compared, g_stat_ro_compare_ns, g_stat_ro_fallback;
/* evaluations: cache hits, skipped lookups (vector sources), misses and the programs that cost the most */
static uint64_t g_stat_eval_relocated, g_stat_eval_hits, g_stat_eval_vsrc, g_stat_eval_misses, g_stat_eval_deferred, g_stat_eval_redo;
#define EVAL_STAT_PROGRAMS 64
typedef struct prog_stat { uint64_t hash, base, ns, n; uint32_t insts; } prog_stat;
static prog_stat g_stat_eval_prog[EVAL_STAT_PROGRAMS], g_stat_cs_prog[EVAL_STAT_PROGRAMS]; /* evaluations, dispatches */
/* texture uploads: the guest textures whose untiling cost the most */
static struct { uint64_t addr, bytes, ns, n; uint32_t w, h, layers, sw; } g_stat_upload[EVAL_STAT_PROGRAMS];
static void upload_stat(uint64_t addr, uint32_t w, uint32_t h, uint32_t layers, uint32_t sw, uint64_t bytes, uint64_t ns) {
    uint32_t i = 0, low = 0;
    for (; i < EVAL_STAT_PROGRAMS && g_stat_upload[i].n && g_stat_upload[i].addr != addr; i++)
        if (g_stat_upload[i].ns < g_stat_upload[low].ns) low = i;
    if (i == EVAL_STAT_PROGRAMS) i = low;
    if (g_stat_upload[i].addr != addr || !g_stat_upload[i].n) {
        memset(&g_stat_upload[i], 0, sizeof(g_stat_upload[i]));
        g_stat_upload[i].addr = addr;
    }
    g_stat_upload[i].w = w; g_stat_upload[i].h = h; g_stat_upload[i].layers = layers; g_stat_upload[i].sw = sw;
    g_stat_upload[i].bytes += bytes; g_stat_upload[i].ns += ns; g_stat_upload[i].n++;
}
static void prog_stat_add(prog_stat *t, const sh_program *p, uint64_t ns) {
    uint32_t i = 0, low = 0;
    for (; i < EVAL_STAT_PROGRAMS && t[i].hash && t[i].hash != p->hash; i++)
        if (t[i].ns < t[low].ns) low = i;
    if (i == EVAL_STAT_PROGRAMS) i = low; /* replace the cheapest */
    if (t[i].hash != p->hash) t[i].hash = p->hash, t[i].ns = t[i].n = 0;
    t[i].base = p->base; t[i].insts = p->count;
    t[i].ns += ns; t[i].n++;
}
/* the four programs that cost the most, then the table is cleared */
static int prog_stat_top(prog_stat *t, char *line, size_t cap) {
    int n = 0;
    for (int k = 0; k < 4; k++) {
        int best = -1;
        for (int i = 0; i < EVAL_STAT_PROGRAMS; i++)
            if (t[i].ns && (best < 0 || t[i].ns > t[best].ns)) best = i;
        if (best < 0) break;
        n += snprintf(line + n, cap - (size_t)n, " %llx(%016llx,%u insts) %llu/%.1fms", (unsigned long long)t[best].base,
                      (unsigned long long)t[best].hash, t[best].insts, (unsigned long long)t[best].n, t[best].ns / 1e6);
        t[best].ns = 0;
    }
    memset(t, 0, sizeof(prog_stat) * EVAL_STAT_PROGRAMS);
    return n;
}
static uint64_t g_stat_ro_patches, g_stat_ro_patch_bytes, g_stat_ro_allocs;
static uint64_t g_stat_arena_full, g_stat_arena_at, g_stat_arena_request, g_stat_pool_failed;
static uint64_t g_stat_work_rotated, g_stat_work_serial;
static uint64_t g_stat_uniform_count, g_stat_uniform_logical, g_stat_uniform_uploaded;
static int g_stat_pool_result;
/* sync_batch callers (source line), with the time each spent waiting */
#define SYNC_SITES 24
static struct { uint32_t line; uint64_t n, ns; } g_stat_sync[SYNC_SITES];
static struct { uint32_t line; uint64_t n; } g_stat_draw_fail[8];
static uint64_t g_stat_fb_created;
static uint64_t g_stat_sync_submit_ns, g_stat_sync_fence_ns;
/* draw phases (source line of the end of each), with the time they took */
#define PHASE_SITES 32
static struct { uint32_t line; uint64_t n, ns; } g_stat_phase[PHASE_SITES];

/* PS5C_BUFFER_TRAFFIC: attribute actual guest-copy bytes to shader sites.
 * The descriptor span alone overcounts copies shared by earlier bindings. */
#define BUFFER_TRAFFIC_COUNT 2048
typedef struct buffer_traffic {
    uint64_t hash, addr, calls, requested, copied, min_size, max_size;
    uint32_t stage, site, stride, idxen, offen, soffset;
    bool store;
    char instruction[40];
} buffer_traffic;
static buffer_traffic g_traffic[BUFFER_TRAFFIC_COUNT];
static uint32_t g_traffic_count;
static uint64_t g_traffic_unattributed;
static int g_traffic_on = -1;
static bool g_traffic_compute;
static uint64_t g_scope_checks,g_scope_missed,g_scope_saved,g_scope_reads;

static void traffic_record(const sh_program *p, uint32_t site, sh_stage stage,
                            uint32_t stride, uint64_t size, uint64_t copied) {
    if (g_traffic_on < 0) {
        const char *value=getenv("PS5C_BUFFER_TRAFFIC");
        g_traffic_on=value!=NULL;g_traffic_compute=value && !strcmp(value,"cs");
    }
    if (!g_traffic_on) return;
    if(g_traffic_compute && stage!=SH_STAGE_CS)return;
    const rdna2_inst *in = &p->insts[p->sites[site].inst];
    buffer_traffic *t = NULL;
    for (uint32_t i = 0; i < g_traffic_count; i++)
        if (g_traffic[i].hash == p->hash && g_traffic[i].site == site && g_traffic[i].stage == (uint32_t)stage) {
            t = &g_traffic[i]; break;
        }
    if (!t) {
        if (g_traffic_count == BUFFER_TRAFFIC_COUNT) { g_traffic_unattributed += copied; return; }
        t = &g_traffic[g_traffic_count++];
        memset(t, 0, sizeof(*t));
        t->hash = p->hash; t->addr = p->base; t->site = site; t->stage = stage;
        t->idxen = in->idxen; t->offen = in->offen; t->soffset = in->soffset;
        t->store = p->sites[site].store;
        t->min_size = UINT64_MAX;
        snprintf(t->instruction, sizeof(t->instruction), "%s", in->name ? in->name : "?");
    }
    t->calls++; t->requested += size; t->copied += copied; t->stride = stride;
    if (size < t->min_size) t->min_size = size;
    if (size > t->max_size) t->max_size = size;
}

static void traffic_report(void) {
    if (g_traffic_on != 1) return;
    bool printed[BUFFER_TRAFFIC_COUNT] = {false};
    for (uint32_t n = 0; n < 8; n++) {
        uint32_t best = BUFFER_TRAFFIC_COUNT;
        for (uint32_t i = 0; i < g_traffic_count; i++)
            if (!printed[i] && g_traffic[i].copied &&
                (best == BUFFER_TRAFFIC_COUNT || g_traffic[i].copied > g_traffic[best].copied)) best = i;
        if (best == BUFFER_TRAFFIC_COUNT) break;
        printed[best] = true;
        const buffer_traffic *t = &g_traffic[best];
        LOG_I(GPU, "buffer traffic: %s 0x%llx hash %016llx site %u %s %s stride %u idxen %u offen %u soffset %u: %llu calls, %.1f MiB requested %.1f MiB copied, size 0x%llx-0x%llx",
              t->stage == SH_STAGE_VS ? "VS" : t->stage == SH_STAGE_PS ? "PS" : "CS",
              (unsigned long long)t->addr, (unsigned long long)t->hash, t->site, t->instruction,
              t->store ? "write" : "read", t->stride, t->idxen, t->offen, t->soffset,
              (unsigned long long)t->calls, t->requested / 1048576.0, t->copied / 1048576.0,
              (unsigned long long)t->min_size, (unsigned long long)t->max_size);
    }
    if (g_traffic_unattributed) LOG_I(GPU, "buffer traffic: %.1f MiB copied outside tracked sites", g_traffic_unattributed / 1048576.0);
    memset(g_traffic, 0, sizeof(g_traffic));
    g_traffic_count = 0; g_traffic_unattributed = 0;
}
static uint64_t stat_now(void) {
    if (g_stats_on < 0) g_stats_on = getenv("PS5C_GPU_STATS") != NULL;
    return g_stats_on ? pal_time_ns() : 0;
}
static void stat_add(int k, uint64_t t0) {
    if (!g_stats_on) return;
    g_stat_ns[k] += pal_time_ns() - t0;
    g_stat_n[k]++;
}
/* stat_phase(t): charge the time since t to this line and return now (0 when stats are off) */
#define stat_phase(t0) stat_phase_at(__LINE__, (t0))
static uint64_t stat_phase_at(uint32_t line, uint64_t t0) {
    if (!g_stats_on) return 0;
    uint64_t now = pal_time_ns();
    for (int i = 0; i < PHASE_SITES; i++) {
        if (g_stat_phase[i].line && g_stat_phase[i].line != line) continue;
        g_stat_phase[i].line = line;
        g_stat_phase[i].n++;
        g_stat_phase[i].ns += now - t0;
        break;
    }
    return now;
}
/* Time the CPU spent waiting for the GPU to finish earlier work (vk_renderer.c). */
void vk_stat_gpu_wait(uint64_t t0) { stat_add(ST_GPU_WAIT, t0); }
void vk_stat_work_slot(bool rotated) {
    if(!g_stats_on)return;
    if(rotated)g_stat_work_rotated++;else g_stat_work_serial++;
}
uint64_t vk_stat_now(void) { return stat_now(); }

/* PS5C_SCALE_CENSUS=1 (with PS5C_GPU_STATS=1): each second, how shaders and copies touch
 * large images (at least 960x540), split by whether they were drawn to as attachments.
 * Groundwork for a generic internal resolution: what would have to follow a smaller image. */
static int g_census = -1;
static uint32_t g_census_op[2][3][6]; /* [drawn to][PS, CS, other][sample, gather, load, store, resinfo, other] */
static uint32_t g_census_draws, g_census_pos, g_census_copy[6];
static const char *const k_census_copy[6] = {"upload", "copy_from", "copy_to_buffer", "blit", "mip_chain", "linear"};
static bool census_on(void) {
    if (g_census < 0) {
        const char *e = getenv("PS5C_SCALE_CENSUS");
        g_census = e && *e == '1';
    }
    return g_census > 0;
}
static bool census_large(const vk_image *img) { return img && img->width >= 960 && img->height >= 540; }
static void census_image(const vk_image *img, uint32_t stage, uint32_t op) {
    if (!census_on() || !census_large(img)) return;
    int c = op == SH_OP_IMAGE_SAMPLE ? 0 : op == SH_OP_IMAGE_GATHER4 ? 1 : op == SH_OP_IMAGE_LOAD ? 2
          : op == SH_OP_IMAGE_STORE ? 3 : op == SH_OP_IMAGE_GET_RESINFO ? 4 : 5;
    g_census_op[img->drawn_to][stage == SH_STAGE_PS ? 0 : stage == SH_STAGE_CS ? 1 : 2][c]++;
}
static void census_copy(const vk_image *img, int where) {
    if (census_on() && census_large(img) && img->drawn_to) g_census_copy[where]++;
}
static void census_report(void) {
    if (!census_on()) return;
    char line[700];
    int n = snprintf(line, sizeof(line), "draws to large targets %u (%u read the pixel position)", g_census_draws, g_census_pos);
    static const char *const stages[3] = {"PS", "CS", "other"};
    for (int d = 1; d >= 0; d--)
        for (int s = 0; s < 3; s++) {
            const uint32_t *o = g_census_op[d][s];
            if (!(o[0] | o[1] | o[2] | o[3] | o[4] | o[5])) continue;
            n += snprintf(line + n, sizeof(line) - (size_t)n, "; %s %s: sample %u gather %u load %u store %u resinfo %u other %u",
                          d ? "targets" : "images", stages[s], o[0], o[1], o[2], o[3], o[4], o[5]);
        }
    for (int i = 0; i < 6; i++)
        if (g_census_copy[i]) n += snprintf(line + n, sizeof(line) - (size_t)n, "; %s %u", k_census_copy[i], g_census_copy[i]);
    LOG_I(GPU, "scale census: %s", line);
    memset(g_census_op, 0, sizeof(g_census_op));
    memset(g_census_copy, 0, sizeof(g_census_copy));
    g_census_draws = g_census_pos = 0;
}

static void stat_report(renderer *r) {
    if (!g_stats_on) return;
    uint64_t now = pal_time_ns();
    if (!g_stat_last) g_stat_last = now;
    if (now - g_stat_last < 1000000000ull) return;
    char line[512];
    int n = 0;
    for (int i = 0; i < ST_COUNT; i++)
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %s %llu/%.1fms", k_stat_names[i], (unsigned long long)g_stat_n[i],
                      (double)g_stat_ns[i] / 1e6);
    LOG_I(GPU, "stats/s:%s copied %.1f MB, releases held %llu (completed early %llu, %llu for CP writes), partial overlaps %llu (%llu patched)",
          line, (double)g_stat_bytes / 1048576.0, (unsigned long long)g_stat_held, (unsigned long long)g_stat_held_forced,
          (unsigned long long)g_stat_held_ordered, (unsigned long long)g_stat_partial, (unsigned long long)g_stat_patched);
    g_stat_held = g_stat_held_forced = g_stat_held_ordered = g_stat_partial = g_stat_patched = 0;
    census_report();
    LOG_I(GPU, "access scopes: %llu, pinning %.1f ms, %llu after a mapping change", (unsigned long long)g_stat_pins,
          (double)g_stat_pin_ns / 1e6, (unsigned long long)g_stat_map_epochs);
    g_stat_pins = g_stat_pin_ns = g_stat_map_epochs = 0;
    if (g_vk_feedback_copies)
        LOG_I(GPU, "feedback copies: %llu, %.1f MB", (unsigned long long)g_vk_feedback_copies, (double)g_vk_feedback_bytes / 1048576.0);
    g_vk_feedback_copies = g_vk_feedback_bytes = 0;
    if (g_stat_stencil_draws)
        LOG_I(GPU, "stencil: %llu draws test or write it, DB_STENCIL_INFO %08llx",
              (unsigned long long)g_stat_stencil_draws, (unsigned long long)g_stat_stencil_info);
    g_stat_stencil_draws = 0;
    if (gpu_watch_enabled()) {
        uint64_t faults = 0, prot = 0;
        gpu_watch_counts(&faults, &prot);
        LOG_I(GPU, "page watch: %llu texture hashes skipped, %llu pages protected, %llu write faults",
              (unsigned long long)g_stat_texskip, (unsigned long long)prot, (unsigned long long)faults);
        g_stat_texskip = 0;
        if (g_res_live || g_res_fallbacks) {
            LOG_I(GPU, "resident buffers: %llu binds, %.1f MiB copied in, %llu created, %llu fallbacks; %llu writebacks, "
                       "%llu pages merged, %llu originals saved (%llu lost); %u slots, %.1f MiB",
                  (unsigned long long)g_res_binds, g_res_upload / 1048576.0, (unsigned long long)g_res_created,
                  (unsigned long long)g_res_fallbacks, (unsigned long long)g_res_applied, (unsigned long long)g_res_merged,
                  (unsigned long long)atomic_exchange(&g_res_saved, 0), (unsigned long long)atomic_exchange(&g_res_lost, 0),
                  g_res_live, g_res_live_bytes / 1048576.0);
            g_res_binds = g_res_upload = g_res_created = g_res_fallbacks = g_res_applied = g_res_merged = 0;
        }
    }
    if (g_stat_draw_fail[0].n) {
        n = 0;
        for (int i = 0; i < 8 && g_stat_draw_fail[i].n; i++)
            n += snprintf(line + n, sizeof(line) - (size_t)n, " L%u %llu", g_stat_draw_fail[i].line,
                          (unsigned long long)g_stat_draw_fail[i].n);
        LOG_I(GPU, "draws retried in a fresh batch, by line:%s; %llu framebuffers created", line,
              (unsigned long long)g_stat_fb_created);
        g_stat_fb_created = 0;
        memset(g_stat_draw_fail, 0, sizeof(g_stat_draw_fail));
    }
    if (g_stat_sync[0].n) {
        n = 0;
        for (int i = 0; i < SYNC_SITES && g_stat_sync[i].n; i++)
            n += snprintf(line + n, sizeof(line) - (size_t)n, " L%u %llu/%.1fms", g_stat_sync[i].line,
                          (unsigned long long)g_stat_sync[i].n, (double)g_stat_sync[i].ns / 1e6);
        LOG_I(GPU, "syncs by line:%s", line);
        LOG_I(GPU, "sync split: flush/submit %.2f ms, fence wait %.2f ms (included in sync)",
              g_stat_sync_submit_ns / 1e6, g_stat_sync_fence_ns / 1e6);
        g_stat_sync_submit_ns = g_stat_sync_fence_ns = 0;
        memset(g_stat_sync, 0, sizeof(g_stat_sync));
    }
    if (g_stat_phase[0].n) {
        n = 0;
        for (int i = 0; i < PHASE_SITES && g_stat_phase[i].n && n < 400; i++)
            n += snprintf(line + n, sizeof(line) - (size_t)n, " L%u %llu/%.1fms", g_stat_phase[i].line,
                          (unsigned long long)g_stat_phase[i].n, (double)g_stat_phase[i].ns / 1e6);
        LOG_I(GPU, "draw phases by line:%s", line);
        memset(g_stat_phase, 0, sizeof(g_stat_phase));
    }
    traffic_report();
    if(g_scope_checks || g_scope_missed || g_scope_saved || g_scope_reads) {
        LOG_I(GPU,"writeback footprints: %llu checked, %llu missed bytes, %.1f MiB comparison omitted, %llu reads outside proved writes",
            (unsigned long long)g_scope_checks,(unsigned long long)g_scope_missed,g_scope_saved/1048576.0,(unsigned long long)g_scope_reads);
        g_scope_checks=g_scope_missed=g_scope_saved=g_scope_reads=0;
    }
    if(g_stat_uniform_count) {
        LOG_I(GPU,"uniform data: %llu uploads, %.1f MB logical, %.1f MB uploaded, %.1f MB omitted",
            (unsigned long long)g_stat_uniform_count,g_stat_uniform_logical/1048576.0,
            g_stat_uniform_uploaded/1048576.0,(g_stat_uniform_logical-g_stat_uniform_uploaded)/1048576.0);
        g_stat_uniform_count=g_stat_uniform_logical=g_stat_uniform_uploaded=0;
    }
    if(g_stat_arena_full || g_stat_pool_failed)
        LOG_I(GPU,"batch capacity: %llu arena full at %.1f MiB (request %.1f KiB), %llu descriptor allocation failures (result %d)",
              (unsigned long long)g_stat_arena_full,g_stat_arena_at/1048576.0,g_stat_arena_request/1024.0,
              (unsigned long long)g_stat_pool_failed,g_stat_pool_result);
    g_stat_arena_full=g_stat_pool_failed=0;
    LOG_I(GPU,"work slots: %llu rotated, %llu serial",(unsigned long long)g_stat_work_rotated,
          (unsigned long long)g_stat_work_serial);
    g_stat_work_rotated=g_stat_work_serial=0;
    if (g_stat_trimmed)
        LOG_I(GPU, "vertex ranges: %llu bindings reduced, %.1f MiB descriptor span saved",
              (unsigned long long)g_stat_trimmed, (double)g_stat_saved_bytes / 1048576.0);
    LOG_I(GPU, "arena copies: other %.1f MiB, patched reads %.1f, shared reads %.1f, stores %.1f, indexes %.1f",
          g_stat_copy_by[0] / 1048576.0, g_stat_copy_by[1] / 1048576.0, g_stat_copy_by[2] / 1048576.0,
          g_stat_copy_by[3] / 1048576.0, g_stat_copy_by[4] / 1048576.0);
    if (g_stat_patch_shared) LOG_I(GPU, "patched reads shared: %llu", (unsigned long long)g_stat_patch_shared);
    phys_import_stats();
    g_stat_patch_shared = 0;
    memset(g_stat_copy_by, 0, sizeof(g_stat_copy_by));
    if (g_stat_vc_hits || g_stat_vc_copies || g_stat_vc_skips)
        LOG_I(GPU, "vertex cache: %llu hits, %llu copies (%.1f MiB), %llu left to the arena, %llu resets; eligible %.1f ms, arm %.1f ms, copy %.1f ms",
              (unsigned long long)g_stat_vc_hits, (unsigned long long)g_stat_vc_copies, g_stat_vc_bytes / 1048576.0,
              (unsigned long long)g_stat_vc_skips, (unsigned long long)g_stat_vc_resets, g_stat_vc_elig_ns / 1e6,
              g_stat_vc_arm_ns / 1e6, g_stat_vc_copy_ns / 1e6);
    g_stat_vc_hits = g_stat_vc_copies = g_stat_vc_skips = g_stat_vc_resets = 0;
    g_stat_vc_arm_ns = g_stat_vc_copy_ns = g_stat_vc_elig_ns = g_stat_vc_bytes = 0;
    if (g_stat_ro_hits || g_stat_ro_misses || g_stat_ro_fallback || g_stat_ro_patches)
        LOG_I(GPU, "readonly cache: %llu hits %llu misses %llu fallback, %.1f MiB comparison span in %.1f ms; %llu patched, %.1f MiB copied; %llu new Vulkan buffers",
              (unsigned long long)g_stat_ro_hits, (unsigned long long)g_stat_ro_misses,
              (unsigned long long)g_stat_ro_fallback, g_stat_ro_compared / 1048576.0, g_stat_ro_compare_ns / 1e6,
              (unsigned long long)g_stat_ro_patches, g_stat_ro_patch_bytes / 1048576.0,
              (unsigned long long)g_stat_ro_allocs);
    uint64_t import_builds,import_reuses;vk_large_import_cache_counts(r,&import_builds,&import_reuses);
    if(import_builds || import_reuses)
        LOG_I(GPU,"large import objects, cumulative: %llu fresh pools, %llu reused draws",
              (unsigned long long)import_builds,(unsigned long long)import_reuses);
    if (g_stat_eval_hits || g_stat_eval_misses || g_stat_eval_vsrc) {
        n = prog_stat_top(g_stat_eval_prog, line, sizeof(line));
        LOG_I(GPU, "evaluations: %llu cache hits (%llu relocated), %llu misses, %llu not cached (vector sources), %llu without waiting "
              "for the GPU, %llu evaluated again; top:%s",
              (unsigned long long)g_stat_eval_hits, (unsigned long long)g_stat_eval_relocated,
              (unsigned long long)g_stat_eval_misses, (unsigned long long)g_stat_eval_vsrc, (unsigned long long)g_stat_eval_deferred,
              (unsigned long long)g_stat_eval_redo, n ? line : " -");
        g_stat_eval_relocated = g_stat_eval_hits = g_stat_eval_misses = g_stat_eval_vsrc = g_stat_eval_deferred = g_stat_eval_redo = 0;
    }
    if (g_stat_cs_prog[0].n) {
        n = prog_stat_top(g_stat_cs_prog, line, sizeof(line));
        LOG_I(GPU, "dispatches, costliest:%s", line);
    }
    if (g_stat_upload[0].n) {
        n = 0;
        for (int k = 0; k < 4; k++) {
            int best = -1;
            for (int i = 0; i < EVAL_STAT_PROGRAMS; i++)
                if (g_stat_upload[i].ns && (best < 0 || g_stat_upload[i].ns > g_stat_upload[best].ns)) best = i;
            if (best < 0) break;
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %llx %ux%ux%u sw%u %llu/%.1fMiB/%.1fms",
                          (unsigned long long)g_stat_upload[best].addr, g_stat_upload[best].w, g_stat_upload[best].h,
                          g_stat_upload[best].layers, g_stat_upload[best].sw, (unsigned long long)g_stat_upload[best].n,
                          g_stat_upload[best].bytes / 1048576.0, g_stat_upload[best].ns / 1e6);
            g_stat_upload[best].ns = 0;
        }
        LOG_I(GPU, "uploads, costliest textures:%s", line);
        memset(g_stat_upload, 0, sizeof(g_stat_upload));
    }
    memset(g_stat_ns, 0, sizeof(g_stat_ns));
    memset(g_stat_n, 0, sizeof(g_stat_n));
    g_stat_bytes = 0;
    g_stat_saved_bytes = g_stat_trimmed = 0;
    g_stat_ro_hits = g_stat_ro_misses = g_stat_ro_compared = g_stat_ro_compare_ns = g_stat_ro_fallback = 0;
    g_stat_ro_patches = g_stat_ro_patch_bytes = g_stat_ro_allocs = 0;
    g_stat_last = now;
}

#define MAX_SHADERS 768 /* analyzed programs are large: least recently used ones are dropped */
#define MAX_PIPES 8192
#define MAX_RPS 64
#define MAX_FBS 256
#define MAX_SAMPLERS 2048 /* address and filter modes, anisotropy, LOD bias, border, reduction */
#define MAX_WRITEBACKS 64
#define ARENA_SIZE (128ull << 20)
#define MAX_BUFFER_COPY (32ull << 20)
#define SMALL_TEXTURE TEXTURE_FULL_HASH_LIMIT /* textures compared with guest memory at every use */

typedef struct guest_shader {
    guest_addr addr;
    uint64_t hash;
    uint64_t checked_epoch; /* code compared with the analyzed copy in this epoch */
    sh_program *prog;
    bool ok;
} guest_shader;

/* Merged-stage programs whose first half jumps to the second (s_setpc_b64 s[6:7]):
 * both halves joined into one program, the jump made a no-op. */
#define MAX_FUSED 64
typedef struct fused_shader {
    guest_addr es, gs;
    uint32_t *code, dwords; /* the joined code (front, then back) */
    uint32_t front_words; /* nonzero for a separately linked pixel epilog */
    uint64_t checked_epoch;
    sh_program *prog;
    bool ok;
} fused_shader;

typedef struct pipe_entry {
    uint64_t key;
    VkPipeline pipe,prepass;
    bool prepass_shared;
    VkPipelineLayout layout;
    VkDescriptorSetLayout dsl;
    bool update_after_bind; /* dsl is allocated from the work's update-after-bind pool */
    bool push; /* push-descriptor layout: bind_set pushes the writes, no set is allocated */
    bool failed;
    bool pending; /* built in the background past the wait budget: its draws are skipped meanwhile */
    bool shared; /* owned by vk_pipeline_async.c (built once per content) */
} pipe_entry;

typedef struct rp_entry {
    uint32_t count;
    VkFormat fmt[GPU_MAX_RENDER_TARGETS]; /* VK_FORMAT_UNDEFINED: unused slot */
    VkFormat depth;                       /* VK_FORMAT_UNDEFINED: none */
    bool depth_read_only;
    VkRenderPass rp;
} rp_entry;

typedef struct fb_entry {
    VkRenderPass rp;
    VkImageView views[GPU_MAX_RENDER_TARGETS + 1];
    uint32_t count, width, height;
    VkFramebuffer fb;
    uint32_t layers;
} fb_entry;

typedef struct sampler_entry {
    uint64_t key;
    VkSampler sampler;
    uint64_t last_use;
} sampler_entry;

typedef struct writeback {
    VkDeviceSize offset;
    guest_addr addr;
    uint64_t size;
    uint8_t *orig; /* the contents before the GPU ran: only what it changed goes back */
    bool orig_pooled;
    uint32_t res; /* 1 + the resident buffer that holds the results (offset unused), 0: the arena */
    /* PS5C_STORE_CHECK: the proven store window [check_lo, check_hi) within the copy, and its shader */
    uint64_t check_lo, check_hi, check_hash;
    uint32_t check_site;
    /* Full GPU binding remains intact. Only proved writes may restrict CPU
     * dependencies/completion; mode 2 verifies the proof without pruning. */
    uint64_t scope_lo,scope_hi;
    uint8_t scope_mode;
    uint8_t slot; /* the work slot whose batch produces the results (its arena holds them) */
} writeback;

static bool wb_scope_valid(const writeback *w) {
    return w->scope_mode && !w->res && w->scope_lo<w->scope_hi && w->scope_hi<=w->size;
}

static void wb_scope_add(writeback *w,guest_addr base,uint64_t lo,uint64_t hi,bool proved) {
    if(!wb_scope_valid(w))return; /* an earlier unbounded writer cannot be forgotten */
    if(!proved || base<w->addr || hi<=lo || base-w->addr>w->size || hi>w->size-(base-w->addr)) {
        w->scope_mode=0;return;
    }
    lo+=base-w->addr;hi+=base-w->addr;
    if(lo<w->scope_lo)w->scope_lo=lo;
    if(hi>w->scope_hi)w->scope_hi=hi;
}

static bool wb_pending(const writeback *w,guest_addr addr,uint64_t size) {
    if(!size)return false;
    if(size>UINT64_MAX-addr || w->size>UINT64_MAX-w->addr)return true;
    if(addr>=w->addr+w->size || w->addr>=addr+size)return false;
    if(wb_scope_valid(w) && (addr>=w->addr+w->scope_hi || w->addr+w->scope_lo>=addr+size)) {
        if(g_stats_on>0)g_scope_reads++;
        return w->scope_mode!=1;
    }
    return true;
}

/* Resident buffers (PS5C_RESIDENT_BUFFERS=1 with PS5C_GPU_WATCH=1): the buffer
 * cache of KytyPS5 and shadPS4. Large compute buffers keep a host buffer of
 * their own across batches instead of a copy in the arena per batch. Before a
 * use, only the guest pages the CPU wrote since they were read in are copied
 * in again (gpu_watch). Results the shaders store go back to guest memory
 * when the batch completes: a page is compared with guest memory, still what
 * the GPU read while nobody wrote it, or else with a copy-on-write original
 * saved at the CPU's first write (resident_save). */
#define RESIDENT_MIN (256u << 10)
#define MAX_RESIDENT 48
#define RESIDENT_BUDGET (768ull << 20)
#define RESIDENT_PAGE GPU_WATCH_PAGE

typedef struct resident_buf {
    guest_addr addr;
    uint64_t size; /* guest bytes mirrored, whole pages; the buffer has one page of zeros more */
    vk_buffer buf;
    uint64_t used_batch, last_use;
    uint64_t wb_batch; /* the batch whose pending writeback (for the whole buffer) holds GPU results */
    uint64_t generation; /* gpu_watch_generation when it was last brought up to date: unchanged, nothing to copy */
    /* copy-on-write originals while that writeback is pending: written by resident_save on any thread */
    _Atomic bool cow;
    uint8_t *orig;
    uint8_t *saved; /* a bit per page */
} resident_buf;

/* Large read-only guest ranges copied into the arena during the batch being
 * recorded: later draws and dispatches reading the same memory share the copy
 * (compute passes bind whole heaps of many MB, every dispatch). Dropped with
 * the arena, and where the command processor writes memory. */
#define COPY_CACHE_MIN (4u << 10)
#define MAX_COPY_CACHE 256

typedef struct copy_entry {
    guest_addr addr;
    uint64_t size;
    VkDeviceSize offset;
    uint32_t patched; /* a patched read: 1 + the batch's writebacks it has the results of; 0: plain guest memory */
} copy_entry;

/* Read-only snapshots survive arena resets. A snapshot used by commands in
 * this batch is immutable until vk_work_begin observes the completed fence.
 * CPU changes therefore create a different version rather than modifying
 * bytes an earlier draw has already bound. Reuse in a new submission epoch
 * compares the requested bytes; CP writes invalidate checked ranges too. */
#define MAX_READONLY_CACHE 256
#define READONLY_CACHE_BUDGET (256ull << 20)
#define READONLY_CACHE_MAX ((32ull << 20) + 4096)
#define READONLY_CACHE_MIN (64ull << 10)
/* Small dynamic chunks must not evict the large retained vertex streams.
 * They can still borrow an aligned subrange of an existing large version. */
#define READONLY_CACHE_ADMIT READONLY_CACHE_MIN
/* Vertex cache (PS5C_VERTEX_CACHE=1 with PS5C_GPU_WATCH=1): copies of the small
 * read-only buffers of draws stay on the GPU side across frames, valid while
 * page write tracking says the guest has not written them. Minecraft draws
 * ~4,800 chunk buffers a frame (~240 MiB): copying them each frame made the
 * GPU thread the limit on any graphics card. A changed buffer gets a new
 * region (earlier draws of a batch in flight may still read the old one);
 * full slabs reset the whole cache at a batch boundary. Buffers that change
 * every time are left to the arena for a while. */
#define VC_SLAB (64ull << 20)
#define VC_SLABS 12
#define VC_ENTRIES 32768u
#define VC_MAX_SIZE (4ull << 20)
#define VC_MIN_SIZE 4096u
#define VC_PAGES (1u << 22)
typedef struct vc_entry {
    guest_addr addr;
    uint64_t size, skip_until;
    VkDeviceSize off;
    uint32_t slab, churn;
    uint32_t index_max; /* of an index buffer, once computed for this copy */
    uint32_t versions;  /* sum of its pages' versions when copied */
    bool armed, has_max;
} vc_entry;
typedef struct readonly_entry {
    guest_addr addr;
    uint64_t size, used_batch, last_use, checked_epoch, checked_revision, checked_begin, checked_end;
    vk_buffer buffer;
} readonly_entry;

/* Depth subresources with HTILE: metadata writes and clear consumption are
 * per layer. One cascade must not consume the other cascades' clears. */
#define MAX_HTILES 64
typedef struct htile_entry {
    guest_addr start, end; /* metadata range */
    guest_addr zaddr;      /* depth surface */
    uint32_t slice;
    uint64_t used;
    bool cleared;          /* metadata written since the surface was last used */
    bool has_stencil;
    uint64_t writer;       /* hash of the compute shader that wrote it last (0: DMA or fill) */
} htile_entry;

/* Whether a compute shader's metadata write is a fast clear, learned by
 * reading its results back (a wait for the GPU). Once two readings agree the
 * verdict is used without waiting and checked when the batch completes; a
 * shader that ever disagrees is read back every time (unstable).
 * PS5C_HTILE_PREDICT=0: always read back. */
#define MAX_HTILE_VERDICTS 32
typedef struct htile_verdict {
    uint64_t writer;
    uint32_t seen;
    bool cleared, unstable;
} htile_verdict;
#define MAX_HTILE_CHECKS 32
typedef struct htile_check {
    guest_addr base;
    uint64_t size, writer;
    bool predicted;
} htile_check;

/* Color targets with DCC: their metadata range, written by the game's fast clears
 * (a compute or DMA fills it with clear codes); the image takes the clear at its next
 * use, including texture reads. Without it surfaces keep the last frame: Unreal adds its lighting
 * on top of a fast-cleared scene color, which grew frame after frame to inf/NaN. */
#define MAX_DCC 64
typedef struct dcc_entry {
    guest_addr img, start, end;
    uint32_t width, height, layers;
    VkFormat format;
    uint32_t clear[2]; /* CB_COLOR_CLEAR_WORD0/1 (code 0x20) */
    bool written;
} dcc_entry;

/* DCC keys a compute shader wrote in the batch: the uniform clear code they hold,
 * learned per (writer, keys address) by reading them back (a wait for the GPU), as
 * for HTILE above. Once two readings agree the code is used without waiting and
 * checked when the batch completes; a pair that ever disagrees is read back every
 * time. PS5C_DCC_PREDICT=0: always read back. */
#define MAX_DCC_VERDICTS 64
typedef struct dcc_verdict {
    uint64_t writer;
    guest_addr start;
    uint32_t seen;
    int code; /* -1: not a uniform clear */
    bool unstable;
} dcc_verdict;
#define MAX_DCC_CHECKS 32
typedef struct dcc_check {
    guest_addr start;
    uint64_t size, writer;
} dcc_check;

#define MAX_HELD_RELEASES 256
#define EVAL_CACHE 2048
#define EVAL_WAYS 8
#define EVAL_RELOC 1024     /* entries found by their key without the relocatable SGPRs */
#define EVAL_RELOC_SGPRS 8  /* at most that many of them a program */
#define INDEX_MAX_CACHE 64

struct stage_state;
static void free_draw_stages(struct stage_state *st);
static void free_dispatch_stage(struct stage_state *st);

/* A scalar evaluation kept for the rest of its epoch (setup_stage), with the
 * guest memory it read: a write of the command processor there drops it. */
#define EVAL_RANGES 32
#define EVAL_PAGE_BITS 65536u
#define EVAL_OVERFLOW 128u
typedef struct eval_ranges {
    guest_addr start[EVAL_RANGES], end[EVAL_RANGES];
    guest_addr min_start, max_end;
    uint8_t sbase[EVAL_RANGES]; /* the S_BUFFER_LOAD base SGPR that read it (sh_eval.host_sbase) */
    uint32_t count;
    bool overflow; /* read more ranges than kept: any write drops the evaluation */
} eval_ranges;

typedef struct eval_entry {
    uint64_t key, epoch;
    uint64_t source_hash;
    bool source_hash_valid;
    const sh_program *prog;
    bool ok;
    eval_ranges read;
    sh_site_value *site; /* site_cap entries each, at least prog->site_count */
    sh_site_shape *shapes;
    uint32_t site_cap;
    uint32_t *smem_data;
    uint32_t smem_words, smem_cap, site_dirty;
    /* sh_program.eval_reloc_sgprs: the key without them and their values */
    uint64_t reloc_key;
    uint32_t reloc_value[EVAL_RELOC_SGPRS];
} eval_entry;
typedef struct texture_cpu_source {
    guest_addr addr;
    uint64_t size, slice;
    uint32_t sw, w, h, bpe, ox, oy, layers, fmt; /* fmt: a packed layout (the same bytes read otherwise) */
} texture_cpu_source;
typedef struct held_release {
    gpu *g;
    gpu_release rel;
    uint8_t slot; /* performed once that work slot's batch completes */
} held_release;

#define WORK_EXTRA_POOLS 15
/* These are the only CPU-written resources carried by an in-flight batch.
 * Shader/pipeline/image caches stay shared and follow the single queue order. */
typedef struct work_resources {
    VkDescriptorPool pool, pool_base, pool_wide;
    /* More pools for a batch that fills the first one: a draw-heavy frame then
     * goes on in the same batch instead of waiting for the GPU to get a fresh
     * pool (Crash Bandicoot 4's level: six such waits a second, ~70 ms each). */
    VkDescriptorPool pool_extra[WORK_EXTRA_POOLS];
    uint32_t extra_count, extra_used;
    VkDescriptorPool pool_uab; /* made at the first update-after-bind layout, reset with the others */
    vk_buffer arena;
    vk_buffer other_arena; /* immutable read-only graphics snapshots, never CPU readbacks */
    struct {VkDeviceSize offset, size;} local_copies[1024];
    uint32_t local_copy_count;
    const char *local_setting;
    bool arena_local;
    vk_buffer uploads[256];
    uint32_t upload_count;
} work_resources;

/* r->dyn_state groups in use (see gfx_dyn); PS5C_VK_DYNSTATE=<hex mask> picks them */
enum { DYN_CULL = 1, DYN_DEPTH = 2, DYN_BIAS = 4, DYN_STENCIL = 8, DYN_ALL = 15 };
static uint32_t g_dyn_state;

typedef struct vk_pipelines {
    struct {uint64_t key;VkPipeline pipe;} geometry_cache[128];
    uint32_t geometry_cache_count;
    VkQueryPool occlusion_pool;
    uint32_t occlusion_count;
    uint64_t occlusion_hash[1024],occlusion_flip[1024],occlusion_ps[1024];
    uint32_t occlusion_depth[1024];
    uint64_t occlusion_total;
    bool occlusion_failed;
    bool occlusion_scope; /* count only after the guest's first dump */
    struct bvh_replay_state *bvh_replay; /* transaction-local image backups */
    vk_buffer bvh_paged_table; /* reused only after the isolated dispatch fence */
    vk_buffer *bvh_table; /* borrowed, pinned until isolated dispatch completes */
    sh_program *depth_clear_ps;
    guest_shader shaders[MAX_SHADERS];
    vr_flow_cache vertex_flow;
    uint64_t vertex_flow_flip;
    bool vertex_flow_seen, vertex_flow_enabled;
    fused_shader fused[MAX_FUSED];
    vk_image *promote; /* an image a descriptor must reinterpret: made mutable before the retry */
    uint32_t fused_count, fused_next;
    uint32_t shader_count;
    pipe_entry pipes[MAX_PIPES];
    uint16_t pipe_index[2 * MAX_PIPES]; /* open addressing by key: pipe number + 1, 0 = free */
    uint32_t pipe_count;
    rp_entry rps[MAX_RPS];
    uint32_t rp_count;
    fb_entry fbs[MAX_FBS];
    uint32_t fb_count;
    sampler_entry samplers[MAX_SAMPLERS];
    uint32_t sampler_count;
    uint8_t filter_support[256]; /* 0 unknown, else 1 | 2 linear filtering | 4 min/max reductions */
    work_resources work_slots[2], *work;
    const char *overlap_setting;
    bool overlap_ready;
    const char *batch_setting;
    VkWriteDescriptorSet descriptor_writes[VK_DESCRIPTOR_BINDINGS]; /* renderer lock held */
    VkDeviceSize arena_limit;
    bool wide_batch, batch_mode_seen;
    VkDeviceSize arena_used, align;
    VkFramebuffer cur_fb;
    VkRenderPass cur_rp;
    VkPipeline cur_gfx_pipe;
    VkViewport cur_vp;
    VkRect2D cur_sc;
    float cur_bias_constant, cur_bias_clamp, cur_bias_slope;
    vk_stencil_face cur_stencil[2];
    bool cur_vp_valid, cur_sc_valid, cur_bias_valid, cur_stencil_valid;
    uint32_t cur_dyn[8]; /* gfx_dyn last set (r->dyn_state) */
    bool cur_dyn_valid;
    writeback wb[MAX_WRITEBACKS];
    uint8_t *wb_originals[2]; /* optional, bounded CPU scratch per work slot; never bound to the GPU */
    uint64_t wb_original_used[2];
    bool cross_slot; /* the batch reads results in the other slot's arena: that slot must not be reused under it */
    bool writeback_oom; /* no GPU store may be published without its CPU snapshot */
    uint32_t wb_count;
    uint64_t batch;
    /* Guest memory the CPU writes is taken as unchanged within an epoch: copies
     * are shared, textures and shader code compared once. A new batch and a
     * new guest submission start one (the CPU writes between submissions;
     * batches span several since end-of-pipe releases are held). */
    uint64_t epoch;
    uint64_t tex_hash[VK_MAX_IMAGES]; /* guest contents hash of the last upload, per cached image (0: none) */
    uint64_t tex_checked[VK_MAX_IMAGES]; /* epoch in which the guest contents were last compared */
    bool tex_dynamic[VK_MAX_IMAGES]; /* large texture the CPU rewrote after its upload: hashed whole */
    uint16_t tex_dynamic_still[VK_MAX_IMAGES]; /* whole hashes in a row that found a dynamic texture unchanged */
    uint64_t *tex_scan[VK_MAX_IMAGES]; /* texture_scan: hash per 64 KiB slice of a large texture's memory */
    uint32_t tex_scan_count[VK_MAX_IMAGES], tex_scan_next[VK_MAX_IMAGES];
    texture_cpu_source tex_cpu[VK_MAX_IMAGES];
    uint64_t tex_fill_hash[VK_MAX_IMAGES]; /* cached texture_cpu_hash for a constant cpu_fill baseline */
    uint32_t tex_fill_data[VK_MAX_IMAGES][4];
    uint8_t tex_fill_phase[VK_MAX_IMAGES];
    bool tex_fill_dynamic[VK_MAX_IMAGES];
    resident_buf res[MAX_RESIDENT]; /* slots never move: writebacks refer to them */
    uint32_t res_count; /* slots in use or free below this */
    uint64_t res_bytes, res_seq;
    guest_addr res_failed[16]; /* ranges that cannot be watched, not retried for a while */
    uint64_t res_failed_batch[16];
    uint32_t res_failed_next;
    /* gpu_watch: tex_hash describes guest memory read after the pages were armed */
    bool tex_armed[VK_MAX_IMAGES];
    uint8_t tex_watch_streak[VK_MAX_IMAGES], tex_watch_wait[VK_MAX_IMAGES];
    uint8_t tex_hash_scratch[SMALL_TEXTURE]; /* packed active texels, never surface padding */
    vk_buffer spare_uploads[64]; /* upload buffers of completed batches, for reuse (staging_get) */
    uint32_t spare_count;
    uint64_t spare_bytes;
    uint64_t draws, dispatches, skipped;
    ngg_large_reads *large_import_cache; /* completed draws only; owns one map pin */
    ngg_large_key large_import_keys[16];
    uint32_t large_import_key_count,large_import_draws;
    bool large_import_pending;
    uint64_t large_import_builds,large_import_reuses;
    struct bind_list *cs_bl[2];
    uint32_t cs_bl_depth;
    struct prof *prof; /* PS5C_GPU_PROFILE */
    htile_entry htiles[MAX_HTILES];
    uint64_t htile_sequence;
    htile_verdict htile_verdicts[MAX_HTILE_VERDICTS];
    uint32_t htile_verdict_count;
    htile_check htile_checks[MAX_HTILE_CHECKS]; /* predictions to check when the batch completes */
    uint32_t htile_check_count;
    dcc_entry dccs[MAX_DCC];
    uint32_t dcc_count, dcc_next;
    dcc_verdict dcc_verdicts[MAX_DCC_VERDICTS];
    uint32_t dcc_verdict_count, dcc_verdict_next;
    dcc_check dcc_checks[MAX_DCC_CHECKS]; /* predictions to check when the batch completes */
    uint32_t dcc_check_count;
    uint32_t htile_count;
    copy_entry copies[MAX_COPY_CACHE];
    uint32_t copy_count, copy_next;
    readonly_entry readonly[MAX_READONLY_CACHE];
    /* vertex cache, see vertex_cache_bind */
    vk_buffer vc_slabs[VC_SLABS];
    uint32_t vc_slab_count, vc_slab; /* created, and the one being filled */
    VkDeviceSize vc_used;
    vc_entry *vc;
    uint32_t *vc_page; /* a version per guest page (hashed): bumped when the page is seen written */
    bool vc_reset;
    int vc_on;
    uint64_t readonly_bytes, readonly_sequence;
    uint32_t readonly_count;
    uint64_t readonly_admit;
    bool readonly_enabled;
    const char *readonly_setting;
    bool uniform_compact; /* renderer configuration; stage info records the effective layout */
    uint64_t readonly_poll;
    uint64_t readonly_revision;
    /* end-of-pipe releases held until the batch's writebacks land (renderer_release) */
    held_release held[MAX_HELD_RELEASES];
    uint32_t held_count;
    eval_entry *evals; /* EVAL_CACHE entries, allocated on first use */
    /* evals_written's filter: the 64 KiB pages the cached evaluations read
     * (hashed into EVAL_PAGE_BITS bits; rebuilt from the live entries each
     * epoch), and the entries that read too many ranges to list (any write
     * drops them). eval_pages_all: no filter (too many of those). */
    uint64_t eval_pages[EVAL_PAGE_BITS / 64];
    uint16_t eval_overflow[EVAL_OVERFLOW];
    uint32_t eval_overflow_count;
    uint64_t eval_pages_epoch;
    bool eval_pages_all;
    uint32_t eval_next;
    eval_ranges *eval_reading; /* the evaluation running now records what it reads here */
    const sh_eval *eval_current; /* and its scalar state (the S_BUFFER_LOAD base of a read) */
    uint16_t eval_reloc[EVAL_RELOC]; /* P->evals index + 1 by reloc_key */
    struct stage_state *draw_stages; /* optional renderer-owned scratch; never guest/GPU storage */
    struct stage_state *dispatch_stage; /* the same for compute */
    uint64_t draw_scratch_flip;
    bool draw_scratch_seen, draw_scratch_enabled;
    /* largest index of an index buffer (address, count, index size) in this epoch: draws share them */
    struct {
        guest_addr addr;
        uint64_t epoch;
        uint32_t count, size, max;
    } index_max[INDEX_MAX_CACHE];
    uint32_t index_max_next;
    struct {
        const sh_program *source;
        sh_program *program;
        uint64_t operation;
    } wave_variants[64]; /* bounded (~135 MiB); variants borrowed only within this draw/dispatch */
    uint64_t wave_operation, wave_control_flip;
    bool wave_control_seen, wave_control_enabled;
    uint64_t resident_hash_flip;
    bool resident_hash_seen, resident_hash_fast;
    uint64_t ngg_ballot_flip;
    bool ngg_ballot_seen, ngg_ballot_enabled;
    uint64_t ngg_compact_flip;
    bool ngg_compact_seen, ngg_compact_enabled;
} vk_pipelines;

static bool sparse_programs(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *s = getenv("PS5C_SPARSE_PROGRAMS");
        enabled = s && !strcmp(s, "1");
        if (enabled) LOG_I(GPU, "sparse analyzed programs: enabled (experimental)");
    }
    return enabled != 0;
}

static bool analyze_cached_program(const uint32_t *code, uint32_t dwords, guest_addr addr, sh_program *p) {
    if (!program_storage_prepare(p, sparse_programs()) || !sh_analyze(code, dwords, addr, p)) return false;
    uint64_t saved = program_storage_trim(p, sparse_programs());
    static uint32_t logged;
    if (saved && logged++ < 16)
        LOG_I(GPU, "program 0x%llx: %u instructions, released %llu unused bytes",
              (unsigned long long)addr, p->count, (unsigned long long)saved);
    return true;
}

static void forget_wave_evaluations(vk_pipelines *P,const sh_program *program) {
    for(uint32_t i=0;P->evals && i<EVAL_CACHE;i++)
        if(P->evals[i].prog==program)P->evals[i].prog=NULL;
    vr_flow_invalidate(&P->vertex_flow);
}

static void forget_wave_variants(vk_pipelines *P,const sh_program *source) {
    if(!source)return;
    bool changed=false;
    for(uint32_t i=0;i<PS5C_ARRAY_COUNT(P->wave_variants);i++)if(P->wave_variants[i].source==source) {
        forget_wave_evaluations(P,P->wave_variants[i].program);
        program_storage_free(P->wave_variants[i].program,sparse_programs());memset(&P->wave_variants[i],0,sizeof(P->wave_variants[i]));changed=true;
    }
    if(changed)vr_flow_invalidate(&P->vertex_flow);
}

/* On by default since 2026-11-02: without it every NGG draw whose wave size changes copied
 * its 2.2 MB program (PSPHD's wave32 sprite GS run as wave64: Ratchet & Clank's gameplay
 * 8 -> 11 FPS). Variants are allocated as used. PS5C_WAVE_VARIANT_CACHE=0 turns it off. */
static bool wave_variant_cache(vk_pipelines *P,uint64_t flip) {
    static bool init;static const char *setting;
    if(!init){setting=getenv("PS5C_WAVE_VARIANT_CACHE");if(!setting)setting="1";init=true;}
    if(!P->wave_control_seen || P->wave_control_flip!=flip) {
        bool enabled=!strcmp(setting,"1");
        if(setting[0]=='@') {
            FILE*f=fopen(setting+1,"r");int n=0;
            if(f){enabled=fscanf(f,"%d",&n)==1 && n==1;fclose(f);}
        }
        if(!P->wave_control_seen || enabled!=P->wave_control_enabled)
            LOG_I(GPU,"wave variant cache: %s (experimental)",enabled?"enabled":"disabled");
        P->wave_control_seen=true;P->wave_control_flip=flip;P->wave_control_enabled=enabled;
    }
    return P->wave_control_enabled;
}

/* Optional live comparison, polled once per flip. The selected bit is part
 * of sh_stage_info, so cached SPIR-V/pipelines never cross the two modes. */
static bool ngg_ballot32(vk_pipelines *P,uint64_t flip) {
    static bool init;static const char *setting;
    if(!init){setting=getenv("PS5C_NGG_BALLOT32");init=true;}
    if(!setting)return false;
    if(!P->ngg_ballot_seen || P->ngg_ballot_flip!=flip) {
        bool enabled=!strcmp(setting,"1");
        if(setting[0]=='@') {
            FILE*f=fopen(setting+1,"r");int value=0;
            enabled=f && fscanf(f,"%d",&value)==1 && value==1;
            if(f)fclose(f);
        }
        if(!P->ngg_ballot_seen || enabled!=P->ngg_ballot_enabled)
            LOG_I(GPU,"NGG native32 ballots: %s (experimental)",enabled?"requested":"disabled");
        P->ngg_ballot_seen=true;P->ngg_ballot_flip=flip;P->ngg_ballot_enabled=enabled;
    }
    return P->ngg_ballot_enabled;
}

static uint32_t ngg_multi_draw_limit(renderer *r,uint64_t flip) {
    static bool init,seen,enabled;static const char *setting;static uint64_t last_flip;
    if(!init){setting=getenv("PS5C_NGG_MULTI_DRAW");init=true;}
    if(!setting || !r->features.multiDrawIndirect)return 1;
    if(!seen || last_flip!=flip) {
        bool on=!strcmp(setting,"1");
        if(setting[0]=='@') {
            FILE*f=fopen(setting+1,"r");int value=0;
            on=f && fscanf(f,"%d",&value)==1 && value==1;
            if(f)fclose(f);
        }
        if(!seen || on!=enabled)LOG_I(GPU,"NGG indirect replay batching: %s (experimental)",on?"enabled":"disabled");
        enabled=on;seen=true;last_flip=flip;
    }
    uint32_t limit=r->props.limits.maxDrawIndirectCount;
    return enabled && limit>1?limit:1;
}

static bool ngg_compact_groups(vk_pipelines *P,uint64_t flip) {
    static bool init;static const char *setting;
    if(!init){setting=getenv("PS5C_NGG_COMPACT_GROUPS");init=true;}
    if(!setting)return false;
    if(!P->ngg_compact_seen || P->ngg_compact_flip!=flip) {
        bool enabled=!strcmp(setting,"1");
        if(setting[0]=='@') {
            FILE*f=fopen(setting+1,"r");int value=0;
            enabled=f && fscanf(f,"%d",&value)==1 && value==1;
            if(f)fclose(f);
        }
        if(!P->ngg_compact_seen || enabled!=P->ngg_compact_enabled)
            LOG_I(GPU,"compact compute GS groups: %s (experimental)",enabled?"enabled":"disabled");
        P->ngg_compact_seen=true;P->ngg_compact_flip=flip;P->ngg_compact_enabled=enabled;
    }
    return P->ngg_compact_enabled;
}

/* Only the wave flag changes; never mutate an analyzed program shared with
 * other stages or pipeline workers. Source replacement explicitly invalidates
 * its variants. Keep both stages of the current draw pinned until cleanup. */
static const sh_program *program_with_wave(renderer*r,const sh_program*source,bool wave32,
                                          uint64_t flip,sh_program **temporary) {
    vk_pipelines *P=r->pipes;
    /* The same wave size needs no copy (PSPHD's sprite draws copied their 540-instruction
     * program every draw, ~1.6 ms each, and lost its cached evaluations with it). */
    if(source->wave32==wave32)return source;
    if(wave_variant_cache(P,flip)) {
        uint32_t slot=UINT32_MAX;
        for(uint32_t i=0;i<PS5C_ARRAY_COUNT(P->wave_variants);i++) {
            if(P->wave_variants[i].source==source && P->wave_variants[i].program->wave32==wave32) {
                P->wave_variants[i].operation=P->wave_operation;return P->wave_variants[i].program;
            }
            if(P->wave_variants[i].operation==P->wave_operation)continue;
            if(slot==UINT32_MAX || P->wave_variants[i].operation<P->wave_variants[slot].operation)slot=i;
        }
        if(slot!=UINT32_MAX) {
            if(!P->wave_variants[slot].program)P->wave_variants[slot].program=program_storage_alloc(sparse_programs());
            if(P->wave_variants[slot].program) {
                forget_wave_evaluations(P,P->wave_variants[slot].program);
                if(program_storage_copy(P->wave_variants[slot].program,source,sparse_programs())) {
                    P->wave_variants[slot].program->wave32=wave32;
                    P->wave_variants[slot].source=source;P->wave_variants[slot].operation=P->wave_operation;
                    return P->wave_variants[slot].program;
                }
            }
        }
    }
    *temporary=program_storage_alloc(sparse_programs());
    if(!*temporary)return NULL;
    if(!program_storage_copy(*temporary,source,sparse_programs())) {
        program_storage_free(*temporary,sparse_programs());*temporary=NULL;return NULL;
    }
    (*temporary)->wave32=wave32;return *temporary;
}

/* ---- PS5C_GPU_PROFILE: GPU time per shader pair, from timestamps around each
 * draw and dispatch, logged every second (slowest first). ---- */

#define PROF_MAX 2048 /* draws and dispatches timed per batch */
#define PROF_AGG 512

typedef struct prof_key {
    uint64_t a, b; /* VS/PS or CS (b = 0) shader addresses */
} prof_key;

typedef struct prof_agg {
    prof_key k;
    double ns, cpu_ns; /* GPU time, host wall time including waits and compilation */
    uint64_t calls, cpu_calls, work, max_ns;
} prof_agg;

typedef struct prof {
    VkQueryPool pool;
    VkQueryPool stats; /* per draw: vertex invocations, primitives out of clipping, fragment invocations */
    VkQueryPool mesh_stats; /* mesh draws forbid the legacy vertex/clipping counters */
    bool mesh_query[PROF_MAX];
    uint32_t count;
    prof_key rec[PROF_MAX];
    uint64_t work[PROF_MAX];
    char note[PROF_MAX][160]; /* draw parameters, logged for slow draws */
    prof_agg agg[PROF_AGG];
    uint32_t agg_count;
    uint64_t last;
    double batch_max_ns;
    double covered_ns, covered_max_ns;
    uint64_t timed_batches;
    bool batch_timed;
    double whole_ns, whole_max_ns;
    uint64_t whole_batches, dropped;
} prof;

static void prof_init(renderer *r) {
    if (!getenv("PS5C_GPU_PROFILE")) return;
    prof *f = (prof *)calloc(1, sizeof(prof));
    if (!f) return;
    VkQueryPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = 2 * PROF_MAX + 2; /* independent whole-batch endpoints */
    if (vkCreateQueryPool(r->device, &ci, NULL, &f->pool) != VK_SUCCESS) {
        free(f);
        return;
    }
    if (r->features.pipelineStatisticsQuery) {
        ci.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        ci.queryCount = PROF_MAX;
        ci.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                                VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
                                VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        if (vkCreateQueryPool(r->device, &ci, NULL, &f->stats) != VK_SUCCESS) f->stats = VK_NULL_HANDLE;
        ci.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        if (vkCreateQueryPool(r->device, &ci, NULL, &f->mesh_stats) != VK_SUCCESS) f->mesh_stats = VK_NULL_HANDLE;
    }
    r->pipes->prof = f;
    LOG_I(GPU, "GPU profile on (timestamp period %.2f ns)", (double)r->props.limits.timestampPeriod);
}

static void prof_shutdown(renderer *r) {
    prof *f = r->pipes->prof;
    if (!f) return;
    vkDestroyQueryPool(r->device, f->pool, NULL);
    if (f->stats) vkDestroyQueryPool(r->device, f->stats, NULL);
    if (f->mesh_stats) vkDestroyQueryPool(r->device, f->mesh_stats, NULL);
    free(f);
}

/* Bound expensive GPU timing to selected flips. The optional file contains
 * an inclusive first/last flip; an empty or reversed range disables timing.
 * Read it once per flip, rather than adding file I/O to every draw. */
static bool prof_window(renderer *r) {
    if (!r->pipes->prof) return false;
    static bool configured, enabled;
    static const char *path;
    static uint64_t flip = UINT64_MAX;
    if (!configured) { path = getenv("PS5C_GPU_PROFILE_WINDOW"); configured = true; }
    if (!path || path[0] != '@') return true;
    if (flip != g_trace_flip) {
        flip = g_trace_flip;
        unsigned long long first = 1, last = 0;
        FILE *f = fopen(path + 1, "r");
        enabled = false;
        if (f) {
            if (fscanf(f, "%llu %llu", &first, &last) == 2)
                enabled = first <= flip && flip <= last;
            fclose(f);
        }
    }
    return enabled;
}

static int prof_start(renderer *r) {
    prof *f = r->pipes->prof;
    static bool configured;static uint64_t from;
    if(!configured){const char*v=getenv("PS5C_GPU_PROFILE_FROM");from=v?strtoull(v,NULL,10):0;configured=true;}
    if (!prof_window(r) || g_trace_flip<from) return -1;
    if (f->count >= PROF_MAX) { f->dropped++; return -1; }
    if (!r->pipes->cur_fb) {
        /* outside a render pass the work before finishes first: it is not
         * counted in this one (draws in a pass still overlap) */
        VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        vkCmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                             NULL, 0, NULL);
    }
    vkCmdWriteTimestamp(r->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, f->pool, 2 * f->count);
    f->mesh_query[f->count] = false;
    return (int)f->count++;
}

static void prof_stop(renderer *r, int i, uint64_t a, uint64_t b, uint64_t work) {
    prof *f = r->pipes->prof;
    if (i < 0) return;
    vkCmdWriteTimestamp(r->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f->pool, 2 * (uint32_t)i + 1);
    f->rec[i].a = a;
    f->rec[i].b = b;
    f->work[i] = work;
    f->note[i][0] = 0;
}

static prof_agg *prof_find(prof *f, prof_key k) {
    for (uint32_t j = 0; j < f->agg_count; j++) {
        if (f->agg[j].k.a == k.a && f->agg[j].k.b == k.b) return &f->agg[j];
    }
    if (f->agg_count >= PROF_AGG) return NULL;
    prof_agg *g = &f->agg[f->agg_count++];
    memset(g, 0, sizeof(*g));
    g->k = k;
    return g;
}

static uint64_t prof_now(renderer *r) { return prof_window(r) ? pal_time_ns() : 0; }

/* Host wall time of a draw/dispatch, including waits and skipped work. */
static void prof_cpu(renderer *r, uint64_t a, uint64_t b, uint64_t t0) {
    prof *f = r->pipes->prof;
    if (!f || !t0) return;
    prof_agg *g = prof_find(f, (prof_key){a, b});
    if (!g) return;
    g->cpu_ns += (double)(pal_time_ns() - t0);
    g->cpu_calls++;
}

static int prof_cmp(const void *x, const void *y) {
    const prof_agg *p = (const prof_agg *)x, *q = (const prof_agg *)y;
    double a = p->ns + p->cpu_ns, b = q->ns + q->cpu_ns;
    return a < b ? 1 : a > b ? -1 : 0;
}

/* The batch recorded before has completed: collect its timestamps, reset the pool. */
static void prof_collect(renderer *r) {
    prof *f = r->pipes->prof;
    if (!f) return;
    if (f->batch_timed) {
        uint64_t endpoints[2];
        if (vkGetQueryPoolResults(r->device,f->pool,2*PROF_MAX,2,sizeof(endpoints),endpoints,8,
                                 VK_QUERY_RESULT_64_BIT)==VK_SUCCESS && endpoints[1]>=endpoints[0]) {
            double ns=(double)(endpoints[1]-endpoints[0])*r->props.limits.timestampPeriod;
            f->whole_ns+=ns;
            if(ns>f->whole_max_ns)f->whole_max_ns=ns;
            f->whole_batches++;
        }
        f->batch_timed=false;
    }
    static uint64_t ts[2 * PROF_MAX], ps_stats[4 * PROF_MAX], mesh_stats[2 * PROF_MAX];
    memset(ps_stats, 0, sizeof(uint64_t) * 4 * f->count);
    if (f->stats && f->count) {
        /* Dispatches also consume timestamp slots, but do not issue pipeline
         * statistics queries. Availability distinguishes those slots from
         * completed draws that really generated zero fragments. Never WAIT
         * on the unused slots. */
        VkResult result=vkGetQueryPoolResults(r->device,f->stats,0,f->count,sizeof(ps_stats),ps_stats,32,
                                             VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if(result!=VK_SUCCESS && result!=VK_NOT_READY)memset(ps_stats,0,sizeof(uint64_t)*4*f->count);
    }
    if (f->mesh_stats && f->count) {
        memset(mesh_stats, 0, sizeof(uint64_t) * 2 * f->count);
        VkResult result=vkGetQueryPoolResults(r->device,f->mesh_stats,0,f->count,sizeof(mesh_stats),mesh_stats,16,
                                             VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if(result==VK_SUCCESS || result==VK_NOT_READY)
            for(uint32_t i=0;i<f->count;i++)if(f->mesh_query[i]) {
                ps_stats[4*i]=ps_stats[4*i+1]=0;
                ps_stats[4*i+2]=mesh_stats[2*i];ps_stats[4*i+3]=mesh_stats[2*i+1];
            }
    }
    double period = r->props.limits.timestampPeriod;
    if (f->count && vkGetQueryPoolResults(r->device, f->pool, 0, 2 * f->count, sizeof(ts), ts, 8,
                                          VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        double batch_ns = 0;
        uint64_t first = UINT64_MAX, last = 0;
        for (uint32_t i = 0; i < f->count; i++) {
            double ns = ts[2 * i + 1] > ts[2 * i] ? (double)(ts[2 * i + 1] - ts[2 * i]) * period : 0;
            if (ts[2 * i + 1] >= ts[2 * i]) {
                if (ts[2 * i] < first) first = ts[2 * i];
                if (ts[2 * i + 1] > last) last = ts[2 * i + 1];
            }
            batch_ns += ns;
            { /* PS5C_RASTER_STATS=<VS address>[,...]: completed geometry queries, first 24 per shader. */
                static int configured;
                static uint64_t wants[8],want_ps;static uint32_t nwant,logged[8];
                if(!configured) {
                    configured=1;
                    const char *ps=getenv("PS5C_RASTER_STATS_PS");if(ps)want_ps=strtoull(ps,NULL,16);
                    for(const char *q=getenv("PS5C_RASTER_STATS");q && *q && nwant<8;q=strchr(q,',')?strchr(q,',')+1:NULL)
                        wants[nwant++]=strtoull(q,NULL,16);
                }
                for(uint32_t k=0;ps_stats[4*i+3] && k<nwant;k++)if(f->rec[i].b && (!want_ps || f->rec[i].b==want_ps) && wants[k]==f->rec[i].a && logged[k]<24) {
                    logged[k]++;
                    if(f->mesh_query[i])LOG_I(GPU,"raster stats MESH 0x%llx PS 0x%llx %.3f ms: %llu fragments (vertex/clipping counters unavailable); %s",
                          (unsigned long long)f->rec[i].a,(unsigned long long)f->rec[i].b,ns/1e6,
                          (unsigned long long)ps_stats[4*i+2],f->note[i]);
                    else LOG_I(GPU,"raster stats VS 0x%llx PS 0x%llx %.3f ms: %llu vertices, %llu primitives, %llu fragments; %s",
                          (unsigned long long)f->rec[i].a,(unsigned long long)f->rec[i].b,ns/1e6,
                          (unsigned long long)ps_stats[4*i],(unsigned long long)ps_stats[4*i+1],
                          (unsigned long long)ps_stats[4*i+2],f->note[i]);
                }
            }
            prof_agg *g = prof_find(f, f->rec[i]);
            if (!g) continue;
            g->ns += ns;
            g->calls++;
            g->work += f->work[i];
            if ((uint64_t)ns > g->max_ns) g->max_ns = (uint64_t)ns;
            static int slow_logged;
            if (ns > 50e6 && f->note[i][0] && ps_stats[4*i+3] && slow_logged++ < 32)
                LOG_I(GPU, "slow draw %.1f ms: VS 0x%llx PS 0x%llx %s; %llu vertices, %llu primitives, %llu fragments",
                      ns / 1e6, (unsigned long long)f->rec[i].a, (unsigned long long)f->rec[i].b, f->note[i],
                      (unsigned long long)ps_stats[4 * i], (unsigned long long)ps_stats[4 * i + 1],
                      (unsigned long long)ps_stats[4 * i + 2]);
        }
        if (batch_ns > f->batch_max_ns) f->batch_max_ns = batch_ns;
        /* Draw intervals can overlap: their sum is not elapsed device time.
         * This span includes GPU barriers between the timed calls, but excludes
         * uninstrumented work before/after them and is not whole-frame time. */
        if (first != UINT64_MAX && last >= first) {
            double span = (double)(last - first) * period;
            f->covered_ns += span;
            if (span > f->covered_max_ns) f->covered_max_ns = span;
            f->timed_batches++;
        }
    }
    f->count = 0;
    vkCmdResetQueryPool(r->cmd, f->pool, 0, 2 * PROF_MAX + 2);
    if (f->stats) vkCmdResetQueryPool(r->cmd, f->stats, 0, PROF_MAX);
    if (f->mesh_stats) vkCmdResetQueryPool(r->cmd, f->mesh_stats, 0, PROF_MAX);
    f->batch_timed=prof_window(r);
    if(f->batch_timed)vkCmdWriteTimestamp(r->cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,f->pool,2*PROF_MAX);
    uint64_t now = pal_time_ns();
    if (!f->last) f->last = now;
    if (now - f->last < 1000000000ull || !f->agg_count) return;
    qsort(f->agg, f->agg_count, sizeof(prof_agg), prof_cmp);
    double total = 0, cpu = 0;
    for (uint32_t j = 0; j < f->agg_count; j++) {
        total += f->agg[j].ns;
        cpu += f->agg[j].cpu_ns;
    }
    LOG_I(GPU, "profile/s: GPU %.1f ms, CPU %.1f ms (wall) in %u shaders over %.1f ms wall, slowest batch %.1f ms",
          total / 1e6, cpu / 1e6, f->agg_count, (double)(now-f->last)/1e6, f->batch_max_ns / 1e6);
    LOG_I(GPU, "profile coverage: %llu batches, GPU spans %.2f ms, largest span %.2f ms; draw intervals may overlap",
          (unsigned long long)f->timed_batches, f->covered_ns / 1e6, f->covered_max_ns / 1e6);
    LOG_I(GPU,"profile whole work: %llu batches, GPU %.2f ms, largest %.2f ms; %llu calls beyond timing capacity (presentation excluded)",
          (unsigned long long)f->whole_batches,f->whole_ns/1e6,f->whole_max_ns/1e6,(unsigned long long)f->dropped);
    for (uint32_t j = 0; j < f->agg_count && j < 10; j++) {
        const prof_agg *g = &f->agg[j];
        char name[64];
        if (g->k.b) snprintf(name, sizeof(name), "VS 0x%llx PS 0x%llx", (unsigned long long)g->k.a, (unsigned long long)g->k.b);
        else snprintf(name, sizeof(name), "CS 0x%llx", (unsigned long long)g->k.a);
        LOG_I(GPU, "  GPU %7.2f ms (%llu, max %.2f) CPU %7.2f ms (%llu) work %llu  %s", g->ns / 1e6,
              (unsigned long long)g->calls, (double)g->max_ns / 1e6, g->cpu_ns / 1e6, (unsigned long long)g->cpu_calls,
              (unsigned long long)g->work, name);
    }
    f->agg_count = 0;
    f->batch_max_ns = 0;
    f->covered_ns = f->covered_max_ns = 0;
    f->timed_batches = 0;
    f->whole_ns=f->whole_max_ns=0;
    f->whole_batches=f->dropped=0;
    f->last = now;
}

/* Object reuse keeps per-draw fences by default. The independent batch opt-in
 * holds EOPs and mappings until completion; CPU snapshots remain synchronous. */
static bool large_import_cache_enabled(void) {
    static int enabled=-1;
    if(enabled<0){const char *s=getenv("PS5C_LARGE_IMPORT_CACHE");enabled=s && !strcmp(s,"1");}
    return enabled!=0;
}
static bool large_import_batch_enabled(void) {
    static int enabled=-1;
    if(enabled<0){const char *s=getenv("PS5C_LARGE_IMPORT_BATCH");enabled=s && !strcmp(s,"1");}
    return enabled && large_import_cache_enabled();
}
/* Optional bounded object lifetime; no submission, dependency or mapping
 * invalidation is omitted. A live reduction retires the pool after the next
 * draw's real completion, even if it already exceeded the new ceiling. */
static uint32_t large_import_draw_limit(uint64_t flip) {
    static bool initialized,seen;static const char *setting;static uint64_t checked;
    static uint32_t limit=8;
    if(!initialized){setting=getenv("PS5C_LARGE_IMPORT_DRAWS");initialized=true;}
    if(!setting)return 8;
    if(!seen || checked!=flip) {
        uint32_t next=8;
        if(setting[0]=='@') {
            FILE*f=fopen(setting+1,"r");unsigned value=0;
            if(f && fscanf(f,"%u",&value)==1 && (value==8 || value==16 || value==32))next=value;
            if(f)fclose(f);
        } else if(!strcmp(setting,"16"))next=16;
        else if(!strcmp(setting,"32"))next=32;
        if(!seen || limit!=next)LOG_I(GPU,"readonly import lifetime: at most %u draws (experimental)",next);
        limit=next;seen=true;checked=flip;
    }
    return limit;
}
static void end_pass(renderer *r);
static void sync_batch_at(renderer *r,uint32_t line);
static void writeback_snapshot_failure(const char *where) {
    LOG_E(GPU,"writeback snapshot allocation failed %s",where);
    pal_exit_process(1);
}

static void apply_writebacks(renderer *r);
static void large_import_cache_drop(renderer *r) {
    vk_pipelines *P=r->pipes;
    if(!P || !P->large_import_cache)return;
    if(P->large_import_pending) {
        end_pass(r);sync_batch_at(r,__LINE__);apply_writebacks(r);vk_begin(r);
        if(P->large_import_pending)abort(); /* never free an import while the device may still use it */
    }
    ngg_large_reads *pool=P->large_import_cache;
    P->large_import_cache=NULL;P->large_import_draws=P->large_import_key_count=0;
    /* All borrowing commands completed before destroying imports/unpinning. */
    ngg_large_free(r,pool);gmem_unpin_mappings(r->mem);
}
void vk_large_import_cache_counts(renderer *r,uint64_t *builds,uint64_t *reuses) {
    *builds=r->pipes?r->pipes->large_import_builds:0;
    *reuses=r->pipes?r->pipes->large_import_reuses:0;
}

/* ---- Lifetime ---- */

static bool create_pool_flags(renderer *r, uint32_t scale, VkDescriptorPoolCreateFlags flags, VkDescriptorPool *out) {
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16384*scale},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192*scale},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2048*scale}};
    VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.flags = flags;
    ci.maxSets = 4096*scale;
    ci.poolSizeCount = 3;
    ci.pPoolSizes = sizes;
    return vkCreateDescriptorPool(r->device, &ci, NULL, out) == VK_SUCCESS;
}

static bool create_pool(renderer *r, uint32_t scale, VkDescriptorPool *out) { return create_pool_flags(r, scale, 0, out); }

static void work_resources_destroy(renderer *r, work_resources *w) {
    if (w->pool_base) vkDestroyDescriptorPool(r->device,w->pool_base,NULL);
    if (w->pool_wide) vkDestroyDescriptorPool(r->device,w->pool_wide,NULL);
    for(uint32_t i=0;i<w->extra_count;i++) vkDestroyDescriptorPool(r->device,w->pool_extra[i],NULL);
    if (w->pool_uab) vkDestroyDescriptorPool(r->device,w->pool_uab,NULL);
    vk_buffer_destroy(r,&w->arena);
    vk_buffer_destroy(r,&w->other_arena);
    for(uint32_t i=0;i<w->upload_count;i++) vk_buffer_destroy(r,&w->uploads[i]);
    memset(w,0,sizeof(*w));
}

/* Small BAR heaps cannot fit a whole 256 MiB arena. Give all work slots
 * together at most half of an eligible heap; offsets beyond this bounded
 * mirror keep their original host binding. Allocation failure still falls
 * back. The remaining heap space belongs to the driver and other resources. */
static VkDeviceSize local_arena_capacity(const VkPhysicalDeviceMemoryProperties *mem,VkDeviceSize requested,uint32_t slots) {
    if(!slots)return 0;
    VkDeviceSize available=0;
    VkMemoryPropertyFlags need=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for(uint32_t i=0;i<mem->memoryTypeCount;i++) {
        uint32_t heap=mem->memoryTypes[i].heapIndex;
        if(heap>=mem->memoryHeapCount || (mem->memoryTypes[i].propertyFlags&need)!=need)continue;
        VkDeviceSize share=(mem->memoryHeaps[heap].size/2/slots)&~(VkDeviceSize)65535;
        if(share>available)available=share;
    }
    return available<requested?available:requested;
}

static bool work_resources_create(renderer *r, work_resources *w, bool wide) {
    if(!create_pool(r,1,&w->pool_base))return false;
    w->pool=w->pool_base;
    if(wide && !create_pool(r,2,&w->pool_wide))
        LOG_W(GPU,"large draw batches unavailable: keeping the original pool");
    VkBufferUsageFlags usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if(!vk_buffer_create(r,w->pool_wide?2*ARENA_SIZE:ARENA_SIZE,usage,true,&w->arena)) {
        vk_buffer_destroy(r,&w->arena);
        if(!w->pool_wide)return false;
        vkDestroyDescriptorPool(r->device,w->pool_wide,NULL);w->pool_wide=VK_NULL_HANDLE;
        LOG_W(GPU,"large draw arena unavailable: retrying the original 128 MiB");
        if(!vk_buffer_create(r,ARENA_SIZE,usage,true,&w->arena))return false;
    }
    w->local_setting=getenv("PS5C_LOCAL_READONLY");
    if(w->local_setting && (w->local_setting[0]=='@' || !strcmp(w->local_setting,"1"))) {
        VkDeviceSize capacity=local_arena_capacity(&r->mem_props,w->arena.size,PS5C_ARRAY_COUNT(r->pipes->work_slots));
        if(!capacity || !vk_buffer_create_local_host(r,capacity,usage,&w->other_arena))
            LOG_W(GPU,"local work arena unavailable: retaining cached host memory");
    }
    return true;
}

bool vk_pipeline_init(renderer *r) {
    r->pipes = (vk_pipelines *)calloc(1, sizeof(vk_pipelines));
    if (!r->pipes) return false;
    vk_pipelines *P = r->pipes;
    P->align = r->props.limits.minStorageBufferOffsetAlignment;
    g_dyn_state = 0;
    if (r->dyn_state) {
        const char *e = getenv("PS5C_VK_DYNSTATE");
        g_dyn_state = e && *e ? (uint32_t)strtoul(e, NULL, 16) & DYN_ALL : DYN_ALL;
    }
    /* Compact uniform layouts and the 256 MiB batch are on by default since 2026-11-11: Minecraft's world
     * 5.5 -> 8-9 FPS (uniform uploads 547 -> 28 MB/s, no arena-full syncs), LN2/Wobbly faster, same pictures
     * in 13 games. PS5C_UNIFORM_COMPACT=0 / PS5C_LARGE_DRAW_BATCH=0 turn them off. */
    const char *uniform_mode=getenv("PS5C_UNIFORM_COMPACT");
    P->uniform_compact=!uniform_mode || !strcmp(uniform_mode,"1");
    g_stencil = r->stencil;
    LOG_I(GPU, "stencil: %s", g_stencil ? "emulated (D32_SFLOAT_S8_UINT)" : "not emulated");
    P->readonly_setting = getenv("PS5C_READONLY_CACHE");
    P->readonly_admit = READONLY_CACHE_ADMIT;
    P->readonly_enabled = P->readonly_setting && P->readonly_setting[0] != '@' && atoi(P->readonly_setting) != 0;
    if (P->readonly_setting) LOG_I(GPU, "persistent read snapshots configured (128 MiB, compared epochs, immutable batches)");
    if (P->align < 16) P->align = 16;
    P->work=&P->work_slots[0];P->arena_limit=ARENA_SIZE;
    P->batch_setting=getenv("PS5C_LARGE_DRAW_BATCH");
    if(!P->batch_setting)P->batch_setting="1";
    bool wide=P->batch_setting && (P->batch_setting[0]=='@' || !strcmp(P->batch_setting,"1"));
    if(!work_resources_create(r,P->work,wide))return false;
    /* On by default since 2026-11-01 (Dead Cells' menu +13-30%, KAGE +11%, Rugrats +7%, same pictures in six
     * games); PS5C_WORK_OVERLAP=0 records and runs each batch serially again. */
    P->overlap_setting=getenv("PS5C_WORK_OVERLAP");
    if(!P->overlap_setting)P->overlap_setting="1";
    if((P->overlap_setting[0]=='@' || !strcmp(P->overlap_setting,"1")) && r->work_fences[1]) {
        P->overlap_ready=work_resources_create(r,&P->work_slots[1],wide);
        if(!P->overlap_ready)work_resources_destroy(r,&P->work_slots[1]);
        LOG_I(GPU,"work overlap: %s",P->overlap_ready?"two immutable resource slots":"unavailable, serial fallback");
    }
    const char *occlusion=getenv("PS5C_GPU_OCCLUSION");
    if(occlusion && !strcmp(occlusion,"1") && r->features.occlusionQueryPrecise) {
        VkQueryPoolCreateInfo qi={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType=VK_QUERY_TYPE_OCCLUSION,.queryCount=1024};
        if(vkCreateQueryPool(r->device,&qi,NULL,&P->occlusion_pool)==VK_SUCCESS)
            LOG_I(GPU,"occlusion: precise GPU samples, synchronous cumulative dumps (experimental)");
    }
    prof_init(r);
    return true;
}

static void resident_shutdown(renderer *r);
void vk_pipeline_shutdown(renderer *r) {
    vk_pipelines *P = r->pipes;
    if (!P) return;
    large_import_cache_drop(r);
    phys_import_drop(r);
    for (uint32_t i = 0; i < P->pipe_count; i++) {
        if (P->pipes[i].shared) continue;
        if (P->pipes[i].pipe) vkDestroyPipeline(r->device, P->pipes[i].pipe, NULL);
        if(P->pipes[i].prepass && !P->pipes[i].prepass_shared)vkDestroyPipeline(r->device,P->pipes[i].prepass,NULL);
        if (P->pipes[i].layout) vkDestroyPipelineLayout(r->device, P->pipes[i].layout, NULL);
        if (P->pipes[i].dsl) vkDestroyDescriptorSetLayout(r->device, P->pipes[i].dsl, NULL);
    }
    for (uint32_t i = 0; i < P->fb_count && !r->dyn_render; i++) vkDestroyFramebuffer(r->device, P->fbs[i].fb, NULL);
    for(uint32_t i=0;i<P->geometry_cache_count;i++)vkDestroyPipeline(r->device,P->geometry_cache[i].pipe,NULL);
    for (uint32_t i = 0; i < P->rp_count && !r->dyn_render; i++) vkDestroyRenderPass(r->device, P->rps[i].rp, NULL);
    for (uint32_t i = 0; i < P->sampler_count; i++) vkDestroySampler(r->device, P->samplers[i].sampler, NULL);
    prof_shutdown(r);
    if(P->occlusion_pool)vkDestroyQueryPool(r->device,P->occlusion_pool,NULL);
    vk_buffer_destroy(r,&P->bvh_paged_table);
    for (uint32_t i = 0; i < MAX_READONLY_CACHE; i++) vk_buffer_destroy(r, &P->readonly[i].buffer);
    for(uint32_t i=0;i<2;i++)work_resources_destroy(r,&P->work_slots[i]);
    for (uint32_t i = 0; i < P->spare_count; i++) vk_buffer_destroy(r, &P->spare_uploads[i]);
    for (uint32_t i = 0; i < P->shader_count; i++) program_storage_free(P->shaders[i].prog,sparse_programs());
    for(uint32_t i=0;i<PS5C_ARRAY_COUNT(P->wave_variants);i++)program_storage_free(P->wave_variants[i].program,sparse_programs());
    for (uint32_t i = 0; i < P->fused_count; i++) program_storage_free(P->fused[i].prog,sparse_programs()), free(P->fused[i].code);
    for (uint32_t i = 0; P->evals && i < EVAL_CACHE; i++) {
        free(P->evals[i].smem_data);
        free(P->evals[i].site);
        free(P->evals[i].shapes);
    }
    free(P->evals);
    free_draw_stages(P->draw_stages);
    free_dispatch_stage(P->dispatch_stage);
    free(P->depth_clear_ps);
    for(uint32_t i=0;i<P->wb_count;i++)if(!P->wb[i].orig_pooled)free(P->wb[i].orig);
    free(P->wb_originals[0]);
    free(P->wb_originals[1]);
    for (uint32_t i = 0; i < VK_MAX_IMAGES; i++) free(P->tex_scan[i]);
    for (uint32_t i = 0; i < PS5C_ARRAY_COUNT(P->cs_bl); i++) free(P->cs_bl[i]);
    resident_shutdown(r);
    free(P);
    r->pipes = NULL;
}

static void *guest_host(renderer *r, guest_addr addr, uint64_t size);

/* An HTILE dword's ZMASK (bits 3:0) is 0 for a cleared 8x8 tile, 0xF for one
 * whose depth is in memory: most sampled tiles cleared = a fast clear. */
static bool htile_mostly_cleared(const uint32_t *t, uint64_t size, uint64_t *cleared_out, uint64_t *seen_out) {
    uint64_t n = size / 4, cleared = 0, step = n > 4096 ? n / 4096 : 1, seen = 0;
    for (uint64_t i = 0; i < n; i += step, seen++) cleared += (t[i] & 0xF) == 0;
    if (cleared_out) *cleared_out = cleared;
    if (seen_out) *seen_out = seen;
    return cleared * 2 >= seen;
}

static htile_verdict *htile_verdict_find(vk_pipelines *P, uint64_t writer) {
    for (uint32_t i = 0; i < P->htile_verdict_count; i++)
        if (P->htile_verdicts[i].writer == writer) return &P->htile_verdicts[i];
    return NULL;
}

/* A verdict read back from guest memory: learn it, or mark the shader unstable. */
static void htile_verdict_learn(vk_pipelines *P, uint64_t writer, bool cleared) {
    if (!writer) return;
    htile_verdict *v = htile_verdict_find(P, writer);
    if (!v) {
        if (P->htile_verdict_count == MAX_HTILE_VERDICTS) return;
        v = &P->htile_verdicts[P->htile_verdict_count++];
        *v = (htile_verdict){.writer = writer, .cleared = cleared};
    }
    if (v->cleared != cleared && !v->unstable) {
        v->unstable = true;
        LOG_I(GPU, "depth metadata writer %016llx: fast clear %s, earlier %s; read back from now on",
              (unsigned long long)writer, cleared ? "yes" : "no", v->cleared ? "yes" : "no");
    }
    v->seen++;
}

static dcc_verdict *dcc_verdict_find(vk_pipelines *P, uint64_t writer, guest_addr start) {
    for (uint32_t i = 0; i < P->dcc_verdict_count; i++)
        if (P->dcc_verdicts[i].writer == writer && P->dcc_verdicts[i].start == start) return &P->dcc_verdicts[i];
    return NULL;
}

/* A code read back from guest memory: learn it, or mark the pair unstable. */
static void dcc_verdict_learn(vk_pipelines *P, uint64_t writer, guest_addr start, int code) {
    if (!writer) return;
    dcc_verdict *v = dcc_verdict_find(P, writer, start);
    if (!v) {
        v = &P->dcc_verdicts[P->dcc_verdict_count < MAX_DCC_VERDICTS ? P->dcc_verdict_count++
                                                                      : P->dcc_verdict_next++ % MAX_DCC_VERDICTS];
        *v = (dcc_verdict){.writer = writer, .start = start, .code = code};
    }
    if (v->code != code && !v->unstable) {
        v->unstable = true;
        LOG_I(GPU, "DCC keys 0x%llx by %016llx: code %d, earlier %d; read back from now on", (unsigned long long)start,
              (unsigned long long)writer, code, v->code);
    }
    v->seen++;
}

static void dcc_checks_verify(renderer *r) {
    vk_pipelines *P = r->pipes;
    for (uint32_t i = 0; i < P->dcc_check_count; i++) {
        const dcc_check *c = &P->dcc_checks[i];
        const uint8_t *m = (const uint8_t *)guest_host(r, c->start, c->size);
        if (m) dcc_verdict_learn(P, c->writer, c->start, vk_dcc_uniform_clear(m, (size_t)c->size));
    }
    P->dcc_check_count = 0;
}

/* The batch completed and its results are in guest memory: check the predictions made for it. */
static void htile_checks_verify(renderer *r) {
    vk_pipelines *P = r->pipes;
    for (uint32_t i = 0; i < P->htile_check_count; i++) {
        const htile_check *c = &P->htile_checks[i];
        const uint32_t *t = (const uint32_t *)guest_host(r, c->base, c->size);
        if (t) htile_verdict_learn(P, c->writer, htile_mostly_cleared(t, c->size, NULL, NULL));
    }
    P->htile_check_count = 0;
}

/* Buffers compute shaders wrote in the finished batch go back to guest memory:
 * the bytes they changed only, since the CPU (or the command
 * processor) may have written the rest of the range in the meantime. */
static void *guest_host(renderer *r, guest_addr addr, uint64_t size);

/* The bytes the GPU changed (src differs from orig, the contents it read) go to dst. */
static void wb_merge_legacy(uint8_t *dst, const uint8_t *src, const uint8_t *orig, uint64_t size) {
    for (uint64_t o = 0; o < size;) {
        /* unchanged blocks are skipped whole: most of a buffer usually is */
        uint64_t blk = size - o < 256 ? size - o : 256;
        if (!memcmp(src + o, orig + o, blk)) {
            o += blk;
            continue;
        }
        for (uint64_t end = o + blk; o < end; o += 8) {
            uint64_t n = end - o < 8 ? end - o : 8;
            if (!memcmp(src + o, orig + o, n)) continue;
            if (n == 8 && !memcmp(dst + o, orig + o, 8)) { /* the CPU left this word alone */
                memcpy(dst + o, src + o, 8);
                continue;
            }
            /* bytes only: the CPU may have written the rest of the word since */
            for (uint64_t k = o; k < o + n; k++) {
                if (src[k] != orig[k]) dst[k] = src[k];
            }
        }
    }
}

/* A/B diagnostic: PS5C_WB_MERGE=0 retains the old comparator; @file reads
 * 0/1 at most once per guest flip. Both paths preserve byte-level merging. */
static void wb_merge(uint8_t *dst, const uint8_t *src, const uint8_t *orig, uint64_t size) {
    static bool initialized, seen, enabled = true;
    static const char *setting;
    static uint64_t flip;
    if (!initialized) { setting = getenv("PS5C_WB_MERGE"); initialized = true; }
    if (setting && (!seen || flip != g_trace_flip)) {
        bool next = enabled;
        if (setting[0] == '@') {
            FILE *f = fopen(setting + 1, "r"); unsigned value;
            if (f) { if (fscanf(f, "%u", &value) == 1 && value <= 1) next = value != 0; fclose(f); }
        } else next = strcmp(setting, "0") != 0;
        if (!seen || next != enabled) LOG_I(GPU, "writeback merge: %s", next ? "bounded vector comparison" : "legacy comparison");
        enabled = next; seen = true; flip = g_trace_flip;
    }
    if (enabled) vk_writeback_merge(dst, src, orig, size);
    else wb_merge_legacy(dst, src, orig, size);
}

static void resident_apply(renderer *r, writeback *w);
static void resident_release_wb(vk_pipelines *P, writeback *w);
static uint32_t work_slot(const vk_pipelines *P) { return (uint32_t)(P->work - P->work_slots); }

/* Results of one work slot (`slot`), or of every batch (-1). A slot's entries
 * come first in the lists: its batch was submitted before the other one. */
static void apply_writebacks_of(renderer *r, int slot) {
    vk_pipelines *P = r->pipes;
    uint32_t wb_n = 0, held_n = 0;
    if (slot < 0) {
        wb_n = P->wb_count;
        held_n = P->held_count;
        /* the caller waited for the current slot; results of the other one come from an earlier submit */
        uint32_t other = work_slot(P) ^ 1;
        bool older = false;
        for (uint32_t i = 0; i < wb_n && !older; i++) older = P->wb[i].slot == other;
        for (uint32_t i = 0; i < held_n && !older; i++) older = P->held[i].slot == other;
        if (older) vk_check_work(r, vk_work_wait_slot(r, other, UINT64_MAX), "vkWaitForFences (older writebacks)");
    } else {
        while (wb_n < P->wb_count && P->wb[wb_n].slot == (uint8_t)slot) wb_n++;
        while (held_n < P->held_count && P->held[held_n].slot == (uint8_t)slot) held_n++;
    }
    bool own_access = !r->guest_access.pinned && (wb_n || P->htile_check_count || P->dcc_check_count);
    if (own_access && !vk_guest_access_begin(r)) return;
    static int have = -1;
    static guest_addr lo, hi;
    if (have < 0) { /* PS5C_WB_LOG=<lo>-<hi>: writebacks that change bytes in that range (debugging aid) */
        const char *e = getenv("PS5C_WB_LOG");
        have = e != NULL;
        if (e) {
            char *end;
            lo = strtoull(e, &end, 16);
            hi = *end == '-' ? strtoull(end + 1, NULL, 16) : lo + 1;
        }
    }
    static int low = -1; /* PS5C_LOG_GPU_LOW16=<hex>: writebacks covering an address with those low 16 bits */
    static uint32_t low16;
    if (low < 0) {
        low = getenv("PS5C_LOG_GPU_LOW16") != NULL;
        low16 = low ? (uint32_t)strtoul(getenv("PS5C_LOG_GPU_LOW16"), NULL, 16) & 0xFFFF : 0;
    }
    for (uint32_t i = 0; i < wb_n; i++) {
        writeback *w = &P->wb[i];
        if (w->res) {
            resident_apply(r, w);
            continue;
        }
        uint8_t *dst = (uint8_t *)guest_host(r, w->addr, w->size);
        const uint8_t *src = (const uint8_t *)P->work_slots[w->slot].arena.mapped + w->offset;
        { /* PS5C_LOG_STORE_ADDR=break: writebacks over the address the guest breakpoint published */
            static int published = -1;
            static uint32_t logged;
            if (published < 0) published = getenv("PS5C_LOG_STORE_ADDR") && !strcmp(getenv("PS5C_LOG_STORE_ADDR"), "break");
            guest_addr a = 0;
            for (uint32_t c = 0; published && c <= g_store_chased && c < 5 && !a; c++) { /* the chased buffers too */
                guest_addr x = c ? g_store_chase[c] : runtime_debug_addr();
                if (x && x >= w->addr && x + 16 <= w->addr + w->size) a = x;
            }
            if (a && logged++ < 64) {
                const uint32_t *n = (const uint32_t *)(src + (a - w->addr)), *o = w->orig ? (const uint32_t *)(w->orig + (a - w->addr)) : NULL;
                const uint32_t *g = dst ? (const uint32_t *)(dst + (a - w->addr)) : NULL;
                LOG_I(GPU, "writeback 0x%llx+0x%llx over 0x%llx (shader %016llx site %u): new %08x %08x %08x %08x, before %08x %08x, guest %08x %08x",
                      (unsigned long long)w->addr, (unsigned long long)w->size, (unsigned long long)a,
                      (unsigned long long)w->check_hash, w->check_site, n[0], n[1], n[2], n[3], o ? o[0] : 0, o ? o[1] : 0,
                      g ? g[0] : 0, g ? g[1] : 0);
            }
        }
        if (low && w->size) {
            guest_addr c = (w->addr & ~(guest_addr)0xFFFF) | low16;
            if (c < w->addr) c += 0x10000;
            if (c < w->addr + w->size && c >= 0xc000000000ull && c < 0xc010000000ull)
                LOG_I(GPU, "writeback 0x%llx+0x%llx covers 0x%llx: %08x %08x %08x %08x", (unsigned long long)w->addr,
                      (unsigned long long)w->size, (unsigned long long)c, ((const uint32_t *)(src + (c - w->addr)))[0],
                      c + 16 <= w->addr + w->size ? ((const uint32_t *)(src + (c - w->addr)))[1] : 0,
                      c + 16 <= w->addr + w->size ? ((const uint32_t *)(src + (c - w->addr)))[2] : 0,
                      c + 16 <= w->addr + w->size ? ((const uint32_t *)(src + (c - w->addr)))[3] : 0);
        }
        if (have && dst && w->addr < hi && lo < w->addr + w->size) {
            uint64_t a = w->addr < lo ? lo - w->addr : 0, b = w->addr + w->size > hi ? hi - w->addr : w->size;
            uint64_t changed = 0, first = 0;
            for (uint64_t k = a; k < b; k++)
                if (!w->orig || src[k] != w->orig[k]) first = changed++ ? first : k;
            if (changed)
                LOG_I(GPU, "writeback 0x%llx+0x%llx changes %llu bytes in range, first at 0x%llx (%s; shader %016llx site %u)",
                      (unsigned long long)w->addr, (unsigned long long)w->size, (unsigned long long)changed,
                      (unsigned long long)(w->addr + first), w->orig ? "diff" : "whole copy",
                      (unsigned long long)w->check_hash, w->check_site);
        }
        if (!w->orig) writeback_snapshot_failure("at completion");
        uint64_t merge_lo=0,merge_hi=w->size;
        if(wb_scope_valid(w)) {
            if(w->scope_mode==1) {
                merge_lo=w->scope_lo;merge_hi=w->scope_hi;
                if(g_stats_on>0)g_scope_saved+=w->size-(merge_hi-merge_lo);
            } else {
                uint64_t missed=0;
                for(uint64_t k=0;k<w->scope_lo;k++)missed+=src[k]!=w->orig[k];
                for(uint64_t k=w->scope_hi;k<w->size;k++)missed+=src[k]!=w->orig[k];
                g_scope_checks++;g_scope_missed+=missed;
                if(missed)LOG_E(GPU,"writeback footprint missed %llu bytes: shader %016llx, [0x%llx,0x%llx) of 0x%llx",
                    (unsigned long long)missed,(unsigned long long)w->check_hash,(unsigned long long)w->scope_lo,
                    (unsigned long long)w->scope_hi,(unsigned long long)w->size);
            }
        }
        if (dst && w->orig) wb_merge(dst+merge_lo, src+merge_lo, w->orig+merge_lo, merge_hi-merge_lo);
        if (w->check_hi && w->orig) { /* PS5C_STORE_CHECK: the GPU changed bytes outside the proven window */
            static uint32_t reported;
            uint64_t outside = 0;
            for (uint64_t k = 0; k < w->size; k++)
                outside += (k < w->check_lo || k >= w->check_hi) && src[k] != w->orig[k];
            if (outside && reported++ < 16)
                LOG_E(GPU, "store window missed %llu changed bytes: shader %016llx site %u, window 0x%llx-0x%llx of 0x%llx",
                      (unsigned long long)outside, (unsigned long long)w->check_hash, w->check_site,
                      (unsigned long long)w->check_lo, (unsigned long long)w->check_hi, (unsigned long long)w->size);
        }
        if(!w->orig_pooled)free(w->orig);
        w->orig = NULL;
    }
    if (wb_n < P->wb_count) memmove(P->wb, P->wb + wb_n, (P->wb_count - wb_n) * sizeof(P->wb[0]));
    P->wb_count -= wb_n;
    if (slot < 0) P->wb_original_used[0] = P->wb_original_used[1] = 0;
    else P->wb_original_used[slot] = 0; /* every entry of that slot is applied */
    if (slot < 0) { /* the predictions belong to the batch being recorded: not before it completes */
        htile_checks_verify(r);
        dcc_checks_verify(r);
    }
    if (own_access) vk_guest_access_end(r);
    /* the batch is complete: labels and interrupts behind its work, in order */
    for (uint32_t i = 0; i < held_n; i++) gpu_perform_release(P->held[i].g, &P->held[i].rel);
    if (held_n < P->held_count) memmove(P->held, P->held + held_n, (P->held_count - held_n) * sizeof(P->held[0]));
    P->held_count -= held_n;
}

static void apply_writebacks(renderer *r) { apply_writebacks_of(r, -1); }

/* Results and labels of the other slot's batch reach guest memory as soon as
 * it has completed (the game may be waiting for its labels), not when the slot
 * is reused. Not while this batch reads them in place from that slot's arena. */
static void retired_poll(renderer *r) {
    vk_pipelines *P = r->pipes;
    if (!P || P->cross_slot || (!P->wb_count && !P->held_count)) return;
    uint32_t other = work_slot(P) ^ 1;
    if (!(P->wb_count && P->wb[0].slot == other) && !(P->held_count && P->held[0].slot == other)) return;
    if (!vk_work_slot_done(r, other)) return;
    uint32_t n = 0;
    while (n < P->wb_count && P->wb[n].slot == other) n++;
    apply_writebacks_of(r, (int)other);
    /* patched copies count the batch's writebacks from the start of the list */
    uint32_t copies = P->copy_count < MAX_COPY_CACHE ? P->copy_count : MAX_COPY_CACHE;
    for (uint32_t q = 0; n && q < copies; q++)
        if (P->copies[q].patched) P->copies[q].patched = P->copies[q].patched > n ? P->copies[q].patched - n : 1;
}

/* A held release writes inside [addr, addr + size). */
static bool held_overlaps(const vk_pipelines *P, guest_addr addr, uint64_t size) {
    for (uint32_t i = 0; i < P->held_count; i++) {
        const gpu_release *h = &P->held[i].rel;
        if (h->data_sel && h->addr < addr + size && addr < h->addr + 8) return true;
    }
    return false;
}

/* PS5C_NO_HELD_PATCH=1 turns off held_patch (reads over held labels complete the batch). */
static bool held_patch_on(void) {
    static int off = -1;
    if (off < 0) off = getenv("PS5C_NO_HELD_PATCH") != NULL;
    return !off;
}

/* [addr, addr + size) can be read with the held labels written in by the CPU: every held
 * release landing there writes immediate data, and no earlier result of the batch is there. */
static bool held_patchable(const renderer *r,const vk_pipelines *P, guest_addr addr, uint64_t size) {
    for (uint32_t i = 0; i < P->held_count; i++) {
        const gpu_release *h = &P->held[i].rel;
        if (h->data_sel && h->addr < addr + size && addr < h->addr + 8 && h->data_sel != 1 && h->data_sel != 2)
            return false;
    }
    for (uint32_t i = 0; i < P->wb_count; i++)
        if (!vk_guest_ranges_disjoint(r->mem,addr,size,P->wb[i].addr,P->wb[i].size)) return false;
    return true;
}

/* Writes the held labels' values over dst, a copy of [addr, addr + size), in release order
 * (as gpu_perform_release will write them to guest memory). */
static void held_patch(const vk_pipelines *P, uint8_t *dst, guest_addr addr, uint64_t size) {
    for (uint32_t i = 0; i < P->held_count; i++) {
        const gpu_release *h = &P->held[i].rel;
        if (h->data_sel != 1 && h->data_sel != 2) continue;
        uint32_t n = h->data_sel == 1 ? 4 : 8;
        guest_addr at = h->addr & ~(guest_addr)(n - 1);
        uint8_t v[8];
        memcpy(v, &h->data, 8); /* little endian: the low 4 bytes for a 32-bit label */
        for (uint32_t k = 0; k < n; k++)
            if (at + k >= addr && at + k < addr + size) dst[at + k - addr] = v[k];
    }
}

static void drop_writebacks(vk_pipelines *P, uint32_t from) {
    /* from 0: the batch changed under the caller. Entries of the other slot (a
     * submitted batch whose results vk_work_begin left pending) stay. */
    if (!from)
        while (from < P->wb_count && P->wb[from].slot != work_slot(P)) from++;
    for (uint32_t i = from; i < P->wb_count; i++) {
        if (P->wb[i].res) resident_release_wb(P, &P->wb[i]);
        if(!P->wb[i].orig_pooled)free(P->wb[i].orig);
        P->wb[i].orig = NULL;
    }
    P->wb_count = from;
    if(!from)P->wb_original_used[0] = P->wb_original_used[1] = 0;
    else if(P->wb[from-1].slot != work_slot(P))P->wb_original_used[work_slot(P)] = 0; /* none of this slot left */
}

/* Retain at most one ordinary arena's worth of CPU originals. Reuse starts
 * only after every live writeback has been applied or discarded. Partial
 * rollback deliberately leaves dead space rather than reusing live bytes.
 * Allocation failure or a full pool retains the original malloc/free path. */
static uint8_t *writeback_original(vk_pipelines *P, uint64_t size, bool *pooled) {
    static int enabled = -1;
    if(enabled<0) {
        /* On by default: a fresh malloc per writeback faulted in its pages at
         * every copy (Minecraft loading a world: ~9% of the GPU thread -> ~3.5%).
         * PS5C_WB_POOL=0: malloc each one, as before. */
        const char *e=getenv("PS5C_WB_POOL");enabled=!e || strcmp(e,"0");
    }
    *pooled=false;
    uint32_t s=work_slot(P);
    if(enabled && size<=ARENA_SIZE-P->wb_original_used[s]) {
        if(!P->wb_originals[s]) {
            P->wb_originals[s]=(uint8_t *)malloc(ARENA_SIZE);
            if(P->wb_originals[s])LOG_I(GPU,"writeback originals: reusable CPU pool, limit 128 MiB");
        }
        if(P->wb_originals[s]) {
            uint8_t *p=P->wb_originals[s]+P->wb_original_used[s];
            P->wb_original_used[s]+=(size+63)&~63ull;*pooled=true;return p;
        }
    }
    return (uint8_t *)malloc(size);
}

static vk_image *find_depth32(renderer *r, guest_addr addr, uint32_t w, uint32_t h, bool array);
static VkImageView stencil_view(renderer *r, vk_image *z);
static void end_pass(renderer *r);
static void sync_batch_at(renderer *r, uint32_t line);
#define sync_batch(r) sync_batch_at(r, __LINE__)

/* At a flip (renderer lock held, batch just submitted): results of the work
 * so far reach guest memory now. The game may reuse those buffers once it
 * sees the frame done; a later writeback would clobber its new data. */
/* Draws and dispatches recorded so far (the performance panel) */
void vk_pipeline_counts(renderer *r, uint64_t *draws, uint64_t *dispatches) {
    *draws = r->pipes ? r->pipes->draws : 0;
    *dispatches = r->pipes ? r->pipes->dispatches : 0;
}

void vk_readonly_cache_counts(renderer *r, uint64_t *bytes, uint64_t *uses) {
    *bytes = r->pipes ? r->pipes->readonly_bytes : 0;
    *uses = r->pipes ? r->pipes->readonly_sequence : 0;
}

void vk_writebacks_complete(renderer *r) {
    vk_pipelines *P = r->pipes;
    if (!P || (!P->wb_count && !P->held_count)) return;
    vk_check_work(r, vk_work_wait(r, UINT64_MAX), "vkWaitForFences (writeback)");
    apply_writebacks(r);
}

/* End of pipe. Its label or interrupt must not reach the CPU before the
 * results of the work ahead of it: with writebacks pending, it waits for the
 * batch to complete (apply_writebacks) instead of the command processor
 * waiting for the GPU now. That keeps the following work in the same batch
 * (a game that releases after every small dispatch otherwise turns each one
 * into a submit, a wait and a full copy of the buffers it writes). */
bool renderer_release(renderer *r, gpu *g, const gpu_release *rel) {
    static int off = -1;
    if (off < 0) off = getenv("PS5C_HOLD_RELEASES") && !atoi(getenv("PS5C_HOLD_RELEASES"));
    pal_mutex_lock(r->lock);
    vk_pipelines *P = r->pipes;
    if (P) retired_poll(r);
    bool hold = P && (P->large_import_pending || (!off && (P->wb_count || P->held_count)));
    if (hold && P->held_count == MAX_HELD_RELEASES) { /* complete the batch, then this one happens now */
        end_pass(r);
        sync_batch(r);
        apply_writebacks(r);
        g_stat_held_forced++;
        hold = P->wb_count != 0;
    }
    if (hold) {
        P->held[P->held_count].g = g;
        P->held[P->held_count].rel = *rel;
        P->held[P->held_count].slot = (uint8_t)work_slot(P);
        P->held_count++;
        g_stat_held++;
    } else if (off && P && P->wb_count) { /* the old way: wait for the GPU now */
        end_pass(r);
        sync_batch(r);
        apply_writebacks(r);
    }
    pal_mutex_unlock(r->lock);
    return hold;
}

/* A guest command buffer starts: memory the CPU wrote since the last one is
 * read again (new epoch). */
void renderer_submit_begin(renderer *r) {
    pal_mutex_lock(r->lock);
    vk_pipelines *P = r->pipes;
    large_import_cache_drop(r);
    if (P) {
        P->epoch++;
        P->copy_count = P->copy_next = 0;
    }
    pal_mutex_unlock(r->lock);
}

/* The command processor ran out of work: what it holds completes now (the
 * game may be waiting for it before it submits more). */
void renderer_gpu_idle(renderer *r) {
    pal_mutex_lock(r->lock);
    large_import_cache_drop(r);
    vk_pipelines *P = r->pipes;
    if (P && (P->held_count || P->wb_count)) {
        end_pass(r);
        sync_batch(r);
        apply_writebacks(r); /* sync_batch leaves them to the next batch, which the game may be waiting to submit */
    }
    pal_mutex_unlock(r->lock);
}

/* Unlike the queue-idle hint, an explicit suspension also waits for batches
 * that contain only draws and have no writeback/held label to force a flush. */
void renderer_wait_idle(renderer *r) {
    pal_mutex_lock(r->lock);
    large_import_cache_drop(r);
    if (r->pipes) {
        end_pass(r);
        sync_batch(r);
        apply_writebacks(r);
    }
    pal_mutex_unlock(r->lock);
}

/* Recording copied every guest byte the commands read (arena, read-only
 * snapshots, texture uploads), so the CPU may free and reuse that memory
 * without the host GPU finishing. Only results owed to guest memory make it
 * wait. PS5C_SUSPEND_GPU_WAIT=1: always wait for the host GPU (the old way). */
void renderer_suspend_point(renderer *r) {
    static int always = -1;
    if (always < 0) always = getenv("PS5C_SUSPEND_GPU_WAIT") && atoi(getenv("PS5C_SUSPEND_GPU_WAIT"));
    if (always) {
        renderer_wait_idle(r);
        return;
    }
    pal_mutex_lock(r->lock);
    large_import_cache_drop(r);
    vk_pipelines *P = r->pipes;
    if (P && (P->wb_count || P->held_count)) {
        end_pass(r);
        sync_batch(r);
        apply_writebacks(r);
    }
    pal_mutex_unlock(r->lock);
}

/* A write the command processor makes now (WRITE_DATA, DMA_DATA, COPY_DATA,
 * ATOMIC_MEM) comes after the releases held so far: a game that sees it may
 * take the earlier work as done and reuse the memory a held label still goes
 * to (Sonic Frontiers recycles its command buffers; the late labels landed in
 * the new ones and broke their packets). PS5C_HOLD_ORDER=0: the old way. */
void renderer_cp_write(renderer *r) {
    static int off = -1;
    if (off < 0) off = getenv("PS5C_HOLD_ORDER") && !atoi(getenv("PS5C_HOLD_ORDER"));
    pal_mutex_lock(r->lock);
    if(r->pipes && r->pipes->large_import_pending)large_import_cache_drop(r);
    if(off){pal_mutex_unlock(r->lock);return;}
    vk_pipelines *P = r->pipes;
    if (P && P->held_count) {
        static int logging = -1;
        if (logging < 0) logging = getenv("PS5C_LOG_CP_WRITES") != NULL;
        uint32_t held = P->held_count, wbs = P->wb_count;
        uint64_t t0 = pal_time_ns();
        g_stat_held_forced++;
        g_stat_held_ordered++;
        end_pass(r);
        sync_batch(r);
        apply_writebacks(r);
        if (logging) LOG_I(GPU, "CP write waited for %u held releases, %u writebacks: %.1f ms", held, wbs, (pal_time_ns() - t0) / 1e6);
    }
    pal_mutex_unlock(r->lock);
}

void renderer_sync_mem(renderer *r, guest_addr addr, uint64_t size) {
    pal_mutex_lock(r->lock);
    vk_pipelines *P = r->pipes;
    /* a held label is read, or (the whole range) everything held must land */
    bool pending = P && P->held_count && (size == ~0ull || held_overlaps(P, addr, size));
    if (pending) g_stat_held_forced++;
    for (uint32_t i = 0; P && i < P->wb_count && !pending; i++) {
        pending = wb_pending(&P->wb[i],addr,size);
    }
    if (pending) {
        static int logging = -1;
        if (logging < 0) logging = getenv("PS5C_LOG_SUBMITS") != NULL;
        uint64_t t0 = pal_time_ns();
        end_pass(r);
        vk_flush(r);
        uint64_t t1 = pal_time_ns();
        vk_check_work(r, vk_work_wait(r, UINT64_MAX), "vkWaitForFences (writeback)");
        uint64_t t2 = pal_time_ns(), bytes = 0;
        for (uint32_t i = 0; i < P->wb_count; i++) bytes += P->wb[i].size;
        uint32_t n = P->wb_count;
        apply_writebacks(r);
        if (logging && pal_time_ns() - t0 > 20000000ull)
            LOG_I(GPU, "sync for GPU results: submit %.1f ms, wait %.1f ms, %u writebacks (%.1f MB) %.1f ms", (t1 - t0) / 1e6,
                  (t2 - t1) / 1e6, n, bytes / 1048576.0, (pal_time_ns() - t2) / 1e6);
    }
    pal_mutex_unlock(r->lock);
}

/* Texture upload buffers are kept for later batches instead of freed: Intel's
 * driver (igvk64) can deadlock when memory is freed while its own thread
 * presents a frame (Sonic Frontiers stopped at its first frame now and then,
 * vkFreeMemory and the driver's present thread each waiting on the other), and
 * an allocation per upload costs anyway. */
#define SPARE_UPLOAD_BYTES (256ull << 20)
#define STAGING_BATCH_BYTES (384ull << 20) /* texture uploads one batch stages (texture) */

static bool staging_get(renderer *r, vk_pipelines *P, uint64_t size, vk_buffer *out) {
    uint32_t best = P->spare_count;
    for (uint32_t i = 0; i < P->spare_count; i++) {
        uint64_t have = P->spare_uploads[i].size;
        if (have >= size && have <= size * 4 && (best == P->spare_count || have < P->spare_uploads[best].size)) best = i;
    }
    if (best < P->spare_count) {
        *out = P->spare_uploads[best];
        P->spare_bytes -= out->size;
        P->spare_uploads[best] = P->spare_uploads[--P->spare_count];
        return true;
    }
    uint64_t rounded = (size + 0xFFFFull) & ~0xFFFFull; /* 64 KB steps: close sizes share buffers */
    return vk_buffer_create(r, rounded, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, out);
}

static void staging_put(renderer *r, vk_pipelines *P, vk_buffer *b) {
    if (P->spare_count < PS5C_ARRAY_COUNT(P->spare_uploads) && P->spare_bytes + b->size <= SPARE_UPLOAD_BYTES) {
        P->spare_bytes += b->size;
        P->spare_uploads[P->spare_count++] = *b;
        memset(b, 0, sizeof(*b));
    } else {
        vk_buffer_destroy(r, b);
    }
}

/* Called only after this slot's fence. Queries are never reset while the
 * device still owns them, and an error does not fabricate a ready result. */
static void occlusion_collect(renderer *r) {
    vk_pipelines *P=r->pipes;
    if(!P->occlusion_count)return;
    uint64_t values[1024];
    VkResult result=vkGetQueryPoolResults(r->device,P->occlusion_pool,0,P->occlusion_count,
        sizeof(values),values,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT);
    if(result==VK_SUCCESS)for(uint32_t i=0;i<P->occlusion_count;i++)
        P->occlusion_total=(P->occlusion_total+values[i])&INT64_MAX;
    if(result==VK_SUCCESS) {
        static unsigned logged;static uint64_t previous;static unsigned previous_depth=8;
        const char *trace=getenv("PS5C_OCCLUSION_TRACE");
        uint64_t want=trace?strtoull(trace,NULL,16):0;unsigned depth_filter=8;
        if(trace && trace[0]=='@') {
            FILE*f=fopen(trace+1,"r");unsigned long long value=0;
            if(f){if(fscanf(f,"%llx %u",&value,&depth_filter)>=1)want=value;fclose(f);}
        }
        if(previous!=want || previous_depth!=depth_filter){previous=want;previous_depth=depth_filter;logged=0;}
        for(uint32_t i=0;want && i<P->occlusion_count && logged<48;i++)
            if(P->occlusion_hash[i]==want && P->occlusion_flip[i]>=330 &&
               (depth_filter>=8 || ((P->occlusion_depth[i]>>4)&7u)==depth_filter)) {
                logged++;LOG_I(GPU,"occlusion draw: flip %llu VS %016llx PS %016llx depth %08x passed %llu actual samples",
                    (unsigned long long)P->occlusion_flip[i],(unsigned long long)want,
                    (unsigned long long)P->occlusion_ps[i],P->occlusion_depth[i],(unsigned long long)values[i]);
            }
    }
    if(result!=VK_SUCCESS) {P->occlusion_failed=true;LOG_E(GPU,"occlusion query read failed (%d)",result);}
    P->occlusion_count=0;
}

/* PS5C_VK_RETIRE=0: a batch with results or labels still owed to guest memory
 * keeps the next one in its slot (the CPU records only after the GPU is done).
 * By default the next batch goes to the other slot; the results come in when
 * their slot is reused, or sooner when something needs them (needs timeline semaphores). */
static bool retire_on(renderer *r) {
    static int on = -1;
    if (on < 0) on = !getenv("PS5C_VK_RETIRE") || atoi(getenv("PS5C_VK_RETIRE"));
    return on && r->work_timeline;
}

bool vk_work_can_overlap(renderer *r) {
    vk_pipelines *P=r->pipes;
    /* CPU-visible results and held releases always complete in their owning
     * slot. The optional timestamp and persistent snapshot caches currently
     * have single-batch lifetimes too, so keep those combinations serial. */
    if(!P || !P->overlap_ready || ((P->wb_count || P->held_count) && !retire_on(r)) || P->cross_slot ||
       P->htile_check_count || P->dcc_check_count ||
       P->prof || P->readonly_count || P->occlusion_pool || P->large_import_cache || P->res_count)return false;
    if(P->overlap_setting[0]!='@')return !strcmp(P->overlap_setting,"1");
    FILE *f=fopen(P->overlap_setting+1,"r");int value=0;
    bool on=f && fscanf(f,"%d",&value)==1 && value==1;
    if(f)fclose(f);
    return on;
}

void vk_work_begin(renderer *r) {
    vk_pipelines *P = r->pipes;
    /* A switch is allowed only with no pending CPU results in the old slot;
     * otherwise vk_begin waited on the active slot, which still owns them. */
    bool rotated=P->work!=&P->work_slots[r->work_index];
    P->work=&P->work_slots[r->work_index];
    P->cross_slot=false;
    occlusion_collect(r);
    if(P->occlusion_pool)vkCmdResetQueryPool(r->cmd,P->occlusion_pool,0,1024);
    /* the batch this slot ran before has completed; the other slot's (submitted
     * after it) stays pending unless it has completed as well */
    if(rotated && (P->wb_count || P->held_count) && !vk_work_slot_done(r,r->work_index^1)) {
        apply_writebacks_of(r,(int)r->work_index);
    } else apply_writebacks(r);
    prof_collect(r);
    if(P->work->other_arena.buffer) {
        work_resources *w=P->work;bool local=!strcmp(w->local_setting,"1");
        if(w->local_setting[0]=='@') {
            FILE*f=fopen(w->local_setting+1,"r");int value=0;
            local=f && fscanf(f,"%d",&value)==1 && value==1;if(f)fclose(f);
        }
        if(local!=w->arena_local) {
            w->arena_local=local;
            LOG_I(GPU,"read-only graphics residency: %s (experimental)",local?"device-local coherent":"original cached host");
        }
    }
    /* vk_begin has waited for the previous work fence. Both pools and the
     * arena may be reset/reselected only here; never change limits under
     * descriptors or snapshots already owned by recorded GPU commands. */
    bool wide=false;
    if(P->work->pool_wide) {
        if(P->batch_setting[0]=='@') {
            FILE *f=fopen(P->batch_setting+1,"r");int value=0;
            if(f){wide=fscanf(f,"%d",&value)==1 && value==1;fclose(f);}
        } else wide=!strcmp(P->batch_setting,"1");
    }
    if(!P->batch_mode_seen || wide!=P->wide_batch)
        LOG_I(GPU,"draw batch capacity: %s",wide?"256 MiB, doubled descriptor pool (experimental)":"128 MiB, original descriptor pool");
    P->wide_batch=wide;P->batch_mode_seen=true;
    P->work->pool=wide?P->work->pool_wide:P->work->pool_base;
    P->arena_limit=wide?P->work->arena.size:ARENA_SIZE;
    vkResetDescriptorPool(r->device, P->work->pool, 0);
    for(uint32_t i=0;i<P->work->extra_used;i++) vkResetDescriptorPool(r->device, P->work->pool_extra[i], 0);
    P->work->extra_used = 0;
    if (P->work->pool_uab) vkResetDescriptorPool(r->device, P->work->pool_uab, 0);
    P->arena_used = 0;
    P->work->local_copy_count = 0;
    P->copy_count = P->copy_next = 0;
    P->cur_fb = VK_NULL_HANDLE;
    P->cur_gfx_pipe = VK_NULL_HANDLE;
    P->cur_vp_valid = P->cur_sc_valid = P->cur_bias_valid = P->cur_stencil_valid = P->cur_dyn_valid = false;
    if (P->vc_reset) {
        /* both work slots may still read the slabs */
        vk_check_work(r, vkQueueWaitIdle(r->queue), "vkQueueWaitIdle (vertex cache)");
        memset(P->vc, 0, VC_ENTRIES * sizeof(vc_entry));
        P->vc_slab = 0;
        P->vc_used = 0;
        P->vc_reset = false;
        g_stat_vc_resets++;
    }
    if (P->fb_count == MAX_FBS) {
        /* The cache is shared by both work slots. The selected slot's fence
         * alone cannot protect framebuffers used by the other slot. */
        /* Dynamic rendering: entries are ids only, no Vulkan object to wait for. */
        if(P->overlap_ready && !r->dyn_render)vk_check_work(r, vkQueueWaitIdle(r->queue), "vkQueueWaitIdle (work slots)");
        for (uint32_t i = 0; i < P->fb_count && !r->dyn_render; i++) vkDestroyFramebuffer(r->device, P->fbs[i].fb, NULL);
        P->fb_count = 0;
    }
    P->batch++;
    P->epoch++;
    for (uint32_t i = 0; i < P->work->upload_count; i++) staging_put(r, P, &P->work->uploads[i]);
    P->work->upload_count = 0;
}

static void end_pass(renderer *r) {
    vk_pipelines *P = r->pipes;
    if (P->cur_fb) {
        if (r->dyn_render) r->end_rendering(r->cmd);
        else vkCmdEndRenderPass(r->cmd);
        P->cur_fb = VK_NULL_HANDLE;
        P->cur_rp = VK_NULL_HANDLE;
        P->cur_gfx_pipe = VK_NULL_HANDLE;
        P->cur_vp_valid = P->cur_sc_valid = P->cur_bias_valid = P->cur_stencil_valid = P->cur_dyn_valid = false;
    }
}

void vk_work_end(renderer *r) { end_pass(r); }

/* vk_work_end also ends passes for feedback copies without submitting.
 * The endpoint belongs only to the final command-buffer submission. */
void vk_work_profile_end(renderer *r) {
    prof *f=r->pipes->prof;
    if(f && f->batch_timed)
        vkCmdWriteTimestamp(r->cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,f->pool,2*PROF_MAX+1);
}

/* A depth image cannot expose raw integer bits through a color view. These
 * pairs represent the same guest bytes using distinct Vulkan images. */
static VkFormat depth_integer_pair(VkFormat format) {
    switch (format) {
    case VK_FORMAT_D16_UNORM: return VK_FORMAT_R16_UINT;
    case VK_FORMAT_R16_UINT: return VK_FORMAT_D16_UNORM;
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_FORMAT_R32_UINT;
    case VK_FORMAT_R32_UINT: return VK_FORMAT_D32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static bool sync_depth_alias(renderer *r, vk_image *dst) {
    VkFormat other = depth_integer_pair(dst->format);
    if (other == VK_FORMAT_UNDEFINED || dst->depth) return true;
    vk_image *src = NULL;
    for (uint32_t i = 0; i < r->image_count; i++) {
        vk_image *img = &r->images[i];
        if ((img->format == other || (other==VK_FORMAT_D32_SFLOAT && img->format==VK_FORMAT_D32_SFLOAT_S8_UINT)) && img->addr == dst->addr && img->width == dst->width &&
            img->height == dst->height && img->layers == dst->layers && !img->depth && img->gpu_written &&
            img->layout != VK_IMAGE_LAYOUT_UNDEFINED && img->written_seq > dst->written_seq &&
            (!src || img->written_seq > src->written_seq)) src = img;
    }
    if (!src && vk_image_backing_newer(r, dst->addr, dst->width, dst->height, dst->layers, other, dst->written_seq)) {
        dst->last_use = r->use_seq;
        src = vk_image_get_layers(r, dst->addr, dst->width, dst->height, other, dst->layers);
        if (!src) return false;
    }
    if (!src) return true;
    if (vk_img_w(src) != vk_img_w(dst) || vk_img_h(src) != vk_img_h(dst)) { /* PS5C_RT_SCALE: one is smaller */
        static uint32_t mismatch;
        if (mismatch++ < 8) LOG_W(GPU, "depth alias 0x%llx: scaled and full-size formats (PS5C_RT_SCALE), not copied",
                                  (unsigned long long)dst->addr);
        return true;
    }
    src->last_use = dst->last_use = r->use_seq;
    vk_pipelines *P = r->pipes;
    if (P->work->upload_count == PS5C_ARRAY_COUNT(P->work->uploads)) return false;
    uint64_t bytes = (uint64_t)vk_img_w(dst) * vk_img_h(dst) * dst->layers *
                     (other == VK_FORMAT_R16_UINT || other == VK_FORMAT_D16_UNORM ? 2u : 4u);
    vk_buffer *scratch = &P->work->uploads[P->work->upload_count];
    if (!staging_get(r, P, bytes, scratch)) return false;
    P->work->upload_count++;
    end_pass(r);
    vk_barrier(r->cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);
    vk_barrier(r->cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {0};
    region.imageSubresource.aspectMask = src->aspect&VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_ASPECT_DEPTH_BIT:src->aspect;
    region.imageSubresource.layerCount = src->layers;
    region.imageExtent.width = vk_img_w(src);
    region.imageExtent.height = vk_img_h(src);
    region.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(r->cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scratch->buffer, 1, &region);
    VkBufferMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = scratch->buffer;
    barrier.size = bytes;
    vkCmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 1, &barrier, 0, NULL);
    region.imageSubresource.aspectMask = dst->aspect&VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_ASPECT_DEPTH_BIT:dst->aspect;
    vkCmdCopyBufferToImage(r->cmd, scratch->buffer, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    dst->gpu_written = true;
    dst->written_seq = src->written_seq; /* synchronization, not a new guest write */
    vk_pipeline_forget_image(r, (uint32_t)(dst - r->images));
    static int trace = -1;
    static uint32_t logged;
    if (trace < 0) trace = getenv("PS5C_LOG_DEPTH_ALIAS") != NULL;
    if (trace && logged++ < 64)
        LOG_I(GPU, "depth alias 0x%llx %ux%u: format %d -> %d seq %llu (%llu bytes)",
              (unsigned long long)dst->addr, dst->width, dst->height, (int)src->format, (int)dst->format,
              (unsigned long long)dst->written_seq, (unsigned long long)bytes);
    return true;
}


/* Submits the current batch and waits for it (arena and pool become free). */
static void sync_batch_at(renderer *r, uint32_t line) {
    uint64_t t0 = stat_now();
    vk_flush(r);
    uint64_t submitted = g_stats_on ? pal_time_ns() : 0;
    vk_check_work(r, vk_work_wait(r, UINT64_MAX), "vkWaitForFences (batch)");
    if(r->pipes)r->pipes->large_import_pending=false;
    stat_add(ST_SYNC, t0);
    if (!g_stats_on) return;
    g_stat_sync_submit_ns += submitted - t0;
    g_stat_sync_fence_ns += pal_time_ns() - submitted;
    for (int i = 0; i < SYNC_SITES; i++)
        if (g_stat_sync[i].line == line || !g_stat_sync[i].n) {
            g_stat_sync[i].line = line;
            g_stat_sync[i].n++;
            g_stat_sync[i].ns += pal_time_ns() - t0;
            break;
        }
}

/* Copies a linear 32-bit guest surface into `img` (presentation of CPU-drawn frames). */
void vk_upload_linear(renderer *r, VkCommandBuffer cmd, vk_buffer *staging, vk_image *img, guest_addr addr,
                      uint32_t pitch_bytes) {
    uint64_t row = (uint64_t)img->width * 4, size = row * img->height;
    if (staging->size < size) {
        vk_buffer_destroy(r, staging);
        if (!vk_buffer_create(r, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging)) return;
    }
    bool own_access = !r->guest_access.pinned;
    if (own_access && !vk_guest_access_begin(r)) return;
    const uint8_t *src = guest_host(r, addr, (uint64_t)pitch_bytes * img->height);
    if (src) for (uint32_t y = 0; y < img->height; y++)
        memcpy((uint8_t *)staging->mapped + y * row, src + (uint64_t)y * pitch_bytes, row);
    if (own_access) vk_guest_access_end(r);
    if (!src) return;
    vk_barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = {0};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = img->width;
    copy.imageExtent.height = img->height;
    copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, staging->buffer, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

/* ---- Arena ---- */

static bool arena_alloc(renderer *r, VkDeviceSize size, VkDeviceSize *offset) {
    vk_pipelines *P = r->pipes;
    VkDeviceSize at = (P->arena_used + P->align - 1) & ~(P->align - 1);
    if (size>P->arena_limit || at>P->arena_limit-size) {
        g_stat_arena_full++;g_stat_arena_at=P->arena_used;g_stat_arena_request=size;
        return false;
    }
    *offset = at;
    P->arena_used = at + size;
    return true;
}

static void *arena_ptr(renderer *r, VkDeviceSize offset) { return (uint8_t *)r->pipes->work->arena.mapped + offset; }

/* The renderer lock serializes CPU access scopes. Device imports may outlive
 * one scope; retaining their existing pin must not wait behind a writer that
 * needs that same import to retire. Nested imports retain the CPU scope too. */
bool vk_guest_access_begin(renderer *r) {
    vk_pipelines *P = r->pipes;
    uint64_t generation = r->guest_access.generation;
    gmem *prior = r->guest_access.mem;
    bool was_pinned = r->guest_access.pinned;
    uint64_t t0 = g_stats_on > 0 ? pal_time_ns() : 0;
    if (!gmem_access_begin(&r->guest_access, r->mem, P && P->large_import_cache)) return false;
    if (t0 && !was_pinned) g_stat_pins++, g_stat_pin_ns += pal_time_ns() - t0;
    if (prior != r->mem || generation != r->guest_access.generation) {
        r->phys_cached = 0;
        r->phys_cache_seq = r->use_seq;
    }
    if (P && (prior != r->mem || generation != r->guest_access.generation)) {
        g_stat_map_epochs++;
        P->epoch++;
        P->copy_count = P->copy_next = 0;
    }
    return true;
}

void vk_guest_access_end(renderer *r) { gmem_access_end(&r->guest_access); }

/* Cached validations survive a CPU scope only while its mapping generation
 * stays unchanged. Every returned pointer is consumed before scope end. */
static void *guest_host(renderer *r, guest_addr addr, uint64_t size) {
    static int off = -1; /* PS5C_NO_SPAN_CACHE=1: the lock for every access, as before (comparison) */
    if (off < 0) off = getenv("PS5C_NO_SPAN_CACHE") != NULL;
    if (!r->guest_access.pinned) return NULL;
    if (off) return gmem_host(r->mem, addr, size);
    return gmem_access_host(&r->guest_access, addr, size);
}

/* Scalar evaluation runs on the CPU. It cannot read a recorded GPU store
 * or held label until those bytes have actually reached guest memory. */
static bool eval_read_pending(const vk_pipelines *P, guest_addr addr, uint64_t size) {
    if (P->held_count && held_overlaps(P, addr, size)) return true;
    for (uint32_t i = 0; i < P->wb_count; i++)
        if (wb_pending(&P->wb[i],addr,size)) return true;
    return false;
}

static guest_addr g_eval_base; /* the program being evaluated (PS5C_LOG_EVAL_SYNC) */

/* S_BUFFER_LOADs over results of earlier GPU work: the evaluation goes on without them
 * (sh_eval.defer). PS5C_EVAL_NO_DEFER=1: it waits for the GPU, as before. */
static bool eval_defer_cb(void *user, guest_addr addr, uint64_t size) {
    return eval_read_pending(((renderer *)user)->pipes, addr, size);
}

static void *guest_host_cb(void *user, guest_addr addr, uint64_t size) {
    renderer *r = (renderer *)user;
    if (eval_read_pending(r->pipes, addr, size)) {
        /* PS5C_LOG_EVAL_SYNC[=<count>]: reads that wait for the GPU, the first 200 or <count> (debugging aid) */
        static int log_sync = -1;
        static uint32_t logged;
        if (log_sync < 0) log_sync = getenv("PS5C_LOG_EVAL_SYNC") ? (atoi(getenv("PS5C_LOG_EVAL_SYNC")) > 1 ? atoi(getenv("PS5C_LOG_EVAL_SYNC")) : 200) : 0;
        if (log_sync && logged++ < (uint32_t)log_sync) {
            const vk_pipelines *P = r->pipes;
            guest_addr wa = 0;
            uint64_t ws = 0;
            for (uint32_t i = 0; i < P->wb_count && !ws; i++)
                if (addr < P->wb[i].addr + P->wb[i].size && P->wb[i].addr < addr + size) wa = P->wb[i].addr, ws = P->wb[i].size;
            LOG_I(GPU, "evaluation of 0x%llx waits for the GPU: read 0x%llx+0x%llx, %s 0x%llx+0x%llx",
                  (unsigned long long)g_eval_base, (unsigned long long)addr, (unsigned long long)size,
                  ws ? "store" : "held label", (unsigned long long)wa, (unsigned long long)ws);
        }
        end_pass(r);
        sync_batch(r);
        apply_writebacks(r);
        vk_begin(r);
    }
    eval_ranges *rd = r->pipes->eval_reading;
    if (rd && !rd->overflow) { /* extend a range of the same base it touches, else add one */
        uint8_t sbase = r->pipes->eval_current ? r->pipes->eval_current->host_sbase : 0xFF;
        guest_addr end = addr + size;
        if (!rd->count || addr < rd->min_start) rd->min_start = addr;
        if (!rd->count || end > rd->max_end) rd->max_end = end;
        uint32_t i = 0;
        while (i < rd->count && (rd->sbase[i] != sbase || addr > rd->end[i] || end < rd->start[i])) i++;
        if (i < rd->count) {
            if (addr < rd->start[i]) rd->start[i] = addr;
            if (end > rd->end[i]) rd->end[i] = end;
            for (uint32_t j = i + 1; j < rd->count;) {
                if (rd->sbase[j] == sbase && rd->start[i] <= rd->end[j] && rd->end[i] >= rd->start[j]) {
                    if (rd->start[j] < rd->start[i]) rd->start[i] = rd->start[j];
                    if (rd->end[j] > rd->end[i]) rd->end[i] = rd->end[j];
                    rd->count--;
                    rd->sbase[j] = rd->sbase[rd->count];
                    rd->start[j] = rd->start[rd->count];
                    rd->end[j] = rd->end[rd->count];
                } else {
                    j++;
                }
            }
        } else if (rd->count < EVAL_RANGES) {
            rd->sbase[rd->count] = sbase;
            rd->start[rd->count] = addr;
            rd->end[rd->count++] = end;
        } else {
            rd->overflow = true;
        }
    }
    return guest_host(r, addr, size);
}

/* Arming clears the written bits of the pages for the whole cache, so another
 * entry on the same pages would miss the write: every write seen bumps the
 * pages' versions first, and an entry is current only while the versions of
 * its pages add up to what they did when it was copied. */
static uint32_t vc_versions(vk_pipelines *P, guest_addr addr, uint64_t size) {
    uint32_t sum = 0;
    for (guest_addr pg = addr >> 12; pg <= (addr + size - 1) >> 12; pg++)
        sum += P->vc_page[(uint32_t)(pg * 0x9E3779B1u) & (VC_PAGES - 1)];
    return sum;
}
static void vc_note_writes(vk_pipelines *P, guest_addr addr, uint64_t size) {
    guest_addr at = addr, end = addr + size, run;
    uint64_t len;
    while (at < end && gpu_watch_find_written(GPU_WATCH_VERTICES, at, end - at, &run, &len)) {
        for (guest_addr pg = run >> 12; pg <= (run + len - 1) >> 12; pg++)
            P->vc_page[(uint32_t)(pg * 0x9E3779B1u) & (VC_PAGES - 1)]++;
        at = run + len;
    }
}

/* The cached copy of [addr, addr + copy) bound with `range` bytes: its buffer and offset. False: use the arena. */
static bool vertex_cache_bind(renderer *r, guest_addr addr, uint64_t copy, VkDeviceSize range, VkBuffer *buffer,
                              VkDeviceSize *offset, vc_entry **entry) {
    vk_pipelines *P = r->pipes;
    if (!P->vc_on) {
        const char *e = getenv("PS5C_VERTEX_CACHE");
        /* on by default since 2026-11-14 (Minecraft's world: draw cost 206 -> 111 us); PS5C_VERTEX_CACHE=0 = off */
        P->vc_on = (!e || e[0] == '@' || atoi(e)) && gpu_watch_enabled() ? 1 : -1;
        if (P->vc_on > 0) {
            P->vc = (vc_entry *)calloc(VC_ENTRIES, sizeof(vc_entry));
            P->vc_page = (uint32_t *)calloc(VC_PAGES, sizeof(uint32_t));
            if (!P->vc || !P->vc_page) P->vc_on = -1;
            else LOG_I(GPU, "vertex cache on: draw buffers stay on the GPU side until the guest writes them");
        }
    }
    if (P->vc_on > 0) { /* PS5C_VERTEX_CACHE=@file: 1/0 in the file turns it on and off while running (A/B) */
        static const char *setting;
        static uint64_t poll;
        static bool live = true;
        if (!setting) setting = getenv("PS5C_VERTEX_CACHE");
        uint64_t now = pal_time_ns();
        if (setting && setting[0] == '@' && now - poll > 200000000ull) {
            poll = now;
            FILE *f = fopen(setting + 1, "r");
            int v = 1;
            if (f) { if (fscanf(f, "%d", &v) != 1) v = 1; fclose(f); }
            if ((v != 0) != live) LOG_I(GPU, "vertex cache: %s", v ? "on" : "off");
            live = v != 0;
        }
        if (!live) return false;
    }
    if (copy < VC_MIN_SIZE) return false; /* cheaper to copy than to protect */
    if (P->vc_on < 0 || P->vc_reset || !copy || copy > VC_MAX_SIZE || range < copy || range > VC_MAX_SIZE + 64) return false;
    uint64_t h = (addr >> 4) * 0x9E3779B97F4A7C15ull ^ copy * 0xC2B2AE3D27D4EB4Full;
    vc_entry *e = NULL, *spare = NULL;
    for (uint32_t k = 0; k < 8; k++) {
        vc_entry *c = &P->vc[(uint32_t)((h >> 40) + k) & (VC_ENTRIES - 1)];
        if (c->addr == addr && c->size == copy) { e = c; break; }
        if (!c->size && !spare) spare = c;
    }
    if (e) {
        if (e->skip_until > P->batch) { g_stat_vc_skips++; return false; }
        if (e->armed && !gpu_watch_written(GPU_WATCH_VERTICES, addr, copy) && vc_versions(P, addr, copy) == e->versions) {
            e->churn = 0;
            *buffer = P->vc_slabs[e->slab].buffer;
            *offset = e->off;
            if (entry) *entry = e;
            g_stat_vc_hits++;
            return true;
        }
        if (e->armed && ++e->churn >= 4) { /* rewritten every time it is drawn: not worth protecting */
            e->armed = false;
            e->churn = 0;
            e->skip_until = P->batch + 2000;
            g_stat_vc_skips++;
            return false;
        }
    } else {
        e = spare ? spare : &P->vc[(uint32_t)(h >> 40) & (VC_ENTRIES - 1)];
        /* arming below checks that the range can be watched; a refusal is remembered */
        memset(e, 0, sizeof(*e));
        e->addr = addr;
        e->size = copy;
    }
    VkDeviceSize need = (range + 63) & ~63ull;
    if (P->vc_slab_count && VC_SLAB - P->vc_used < need && P->vc_slab + 1 < P->vc_slab_count) {
        P->vc_slab++;
        P->vc_used = 0;
    }
    if (!P->vc_slab_count || VC_SLAB - P->vc_used < need) {
        if (P->vc_slab_count == VC_SLABS ||
            !vk_buffer_create(r, VC_SLAB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true,
                              &P->vc_slabs[P->vc_slab_count])) {
            if (P->vc_slab_count < VC_SLABS) vk_buffer_destroy(r, &P->vc_slabs[P->vc_slab_count]);
            P->vc_reset = P->vc_slab_count != 0; /* every slab is full: start over at the next batch */
            e->armed = false;
            g_stat_vc_skips++;
            return false;
        }
        P->vc_slab = P->vc_slab_count++;
        P->vc_used = 0;
    }
    /* arm, then read: a write racing with the copy marks the pages again */
    vc_note_writes(P, addr, copy);
    uint64_t ta = pal_time_ns();
    const void *src = gpu_watch_arm(GPU_WATCH_VERTICES, addr, copy) ? gmem_host(r->mem, addr, copy) : NULL;
    g_stat_vc_arm_ns += pal_time_ns() - ta;
    ta = pal_time_ns();
    if (!src) {
        static uint32_t refused;
        if (copy >= 32768 && refused < 6) {
            refused++;
            LOG_I(GPU, "vertex cache: [0x%llx +0x%llx) cannot be watched", (unsigned long long)addr, (unsigned long long)copy);
            gpu_watch_log_page(addr);
        }
        e->armed = false;
        e->skip_until = P->batch + 2000;
        g_stat_vc_skips++;
        return false;
    }
    uint8_t *dst = (uint8_t *)P->vc_slabs[P->vc_slab].mapped + P->vc_used;
    memcpy(dst, src, (size_t)copy);
    memset(dst + copy, 0, (size_t)(need - copy));
    e->slab = P->vc_slab;
    e->off = P->vc_used;
    e->armed = true;
    e->versions = vc_versions(P, addr, copy);
    e->has_max = false;
    if (entry) *entry = e;
    P->vc_used += need;
    g_stat_vc_copy_ns += pal_time_ns() - ta;
    g_stat_vc_bytes += copy;
    g_stat_bytes += copy;
    g_stat_vc_copies++;
    *buffer = P->vc_slabs[e->slab].buffer;
    *offset = e->off;
    return true;
}

static bool readonly_cache_enabled(vk_pipelines *P) {
    if (!P->readonly_setting || P->readonly_setting[0]!='@') return P->readonly_enabled;
    uint64_t now=pal_time_ns();
    if (P->readonly_setting[0]=='@' && (!P->readonly_poll || now-P->readonly_poll>=100000000ull)) {
        P->readonly_poll=now;
        FILE *f=fopen(P->readonly_setting+1,"r");
        int mode=P->readonly_enabled?1:0;
        unsigned long long admit=P->readonly_admit;
        if(f) { if(fscanf(f,"%d %llu",&mode,&admit)<1)mode=P->readonly_enabled?1:0;fclose(f); }
        if(admit<READONLY_CACHE_MIN || admit>READONLY_CACHE_MAX)admit=P->readonly_admit;
        if ((mode==0 || mode==1) && ((mode!=0)!=P->readonly_enabled || admit!=P->readonly_admit)) {
            P->readonly_enabled=mode!=0;P->readonly_admit=admit;P->readonly_revision++;
            LOG_I(GPU,"persistent read snapshots: %s, admission %llu KiB",P->readonly_enabled?"on":"off",admit/1024);
        }
    }
    return P->readonly_enabled;
}

/* No pending compute writes: even a physical alias of a recorded writeback
 * could make guest RAM stale. Stay on the original path in that case. */
static VkBuffer readonly_snapshot(renderer *r, guest_addr addr, uint64_t size, VkDeviceSize *offset) {
    vk_pipelines *P = r->pipes;
    if (size < READONLY_CACHE_MIN || size > READONLY_CACHE_MAX || P->wb_count) {
        g_stat_ro_fallback++;
        return VK_NULL_HANDLE;
    }
    const void *src = NULL;
    readonly_entry *replace = NULL;
    /* Prefer an exact version before looking for an aligned containing one. */
    for (uint32_t pass=0;pass<2;pass++) for (uint32_t i = 0; i < P->readonly_count; i++) {
        readonly_entry *e = &P->readonly[i];
        uint64_t at=0;
        bool exact=e->addr==addr && e->size==size;
        if (e->buffer.buffer && exact==(pass==0) &&
            vk_snapshot_window(e->addr,e->size,addr,size,P->align,&at)) {
            /* Same lifetime as the existing per-epoch arena copies. A new
             * submission or CP write revalidates the requested bytes. */
            if(e->checked_epoch==P->epoch && e->checked_revision==P->readonly_revision &&
               at>=e->checked_begin && at<=e->checked_end && size<=e->checked_end-at) {
                e->used_batch=P->batch;e->last_use=++P->readonly_sequence;
                *offset=at;g_stat_ro_hits++;return e->buffer.buffer;
            }
            if(!src)src=gmem_host(r->mem,addr,size);
            if(!src) {g_stat_ro_fallback++;return VK_NULL_HANDLE;}
            uint64_t t0 = stat_now();
            vk_snapshot_refresh_result refresh = vk_snapshot_refresh((uint8_t*)e->buffer.mapped+at, src,
                (size_t)size, e->used_batch != P->batch);
            if (g_stats_on) {
                g_stat_ro_compared += refresh.compared;
                g_stat_ro_compare_ns += pal_time_ns() - t0;
            }
            if (refresh.ready) {
                e->used_batch = P->batch;
                e->last_use = ++P->readonly_sequence;
                e->checked_epoch=P->epoch;e->checked_revision=P->readonly_revision;
                e->checked_begin=at;e->checked_end=at+size;*offset=at;
                if (refresh.copied) {
                    g_stat_ro_patches++; g_stat_ro_patch_bytes += refresh.copied;
                    g_stat_bytes += refresh.copied;
                } else g_stat_ro_hits++;
                return e->buffer.buffer;
            }
        }
    }
    if(size<P->readonly_admit) {g_stat_ro_fallback++;return VK_NULL_HANDLE;}
    if(!src)src=gmem_host(r->mem,addr,size);
    if(!src) {g_stat_ro_fallback++;return VK_NULL_HANDLE;}
    /* Recycle allocations across snapshot sizes. Creating/freeing a Vulkan
     * allocation for every changed chunk would move the bottleneck to the
     * driver. Descriptor range remains the exact logical size plus padding. */
    VkDeviceSize needed = ((size + 3) & ~3ull) + 16;
    VkDeviceSize alloc = (needed + 65535) & ~65535ull;
    readonly_entry *empty=NULL,*fit=NULL;
    for (uint32_t i = 0; i < MAX_READONLY_CACHE; i++) {
        readonly_entry *e = &P->readonly[i];
        if (!e->buffer.buffer) {if(!empty)empty=e;continue;}
        if (e->used_batch == P->batch) continue;
        if (!replace || e->last_use < replace->last_use) replace=e;
        if (e->buffer.size>=needed && (!fit || e->buffer.size<fit->buffer.size ||
            (e->buffer.size==fit->buffer.size && e->last_use<fit->last_use))) fit=e;
    }
    /* Fill unused capacity while it fits. Under pressure, rebind a completed
     * allocation of suitable size instead of freeing it to allocate an empty
     * slot; that churn can cost more CPU time than the memcpy it replaces. */
    if(empty && P->readonly_bytes+alloc<=READONLY_CACHE_BUDGET)replace=empty;
    else if(fit)replace=fit;
    if (!replace) { g_stat_ro_fallback++; return VK_NULL_HANDLE; }
    /* Evict completed versions only. Never force a submit for cache space. */
    while (P->readonly_bytes - replace->buffer.size + alloc > READONLY_CACHE_BUDGET) {
        readonly_entry *old = NULL;
        for (uint32_t i = 0; i < MAX_READONLY_CACHE; i++) {
            readonly_entry *e = &P->readonly[i];
            if (e != replace && e->buffer.buffer && e->used_batch != P->batch &&
                (!old || e->last_use < old->last_use)) old = e;
        }
        if (!old) { g_stat_ro_fallback++; return VK_NULL_HANDLE; }
        P->readonly_bytes -= old->buffer.size;
        vk_buffer_destroy(r, &old->buffer);
        memset(old, 0, sizeof(*old));
    }
    if (replace->buffer.size < needed) {
        P->readonly_bytes -= replace->buffer.size;
        vk_buffer_destroy(r, &replace->buffer);
        if (!vk_buffer_create(r, alloc, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, &replace->buffer)) {
            vk_buffer_destroy(r, &replace->buffer); /* helper may have partially allocated */
            memset(replace, 0, sizeof(*replace));
            g_stat_ro_fallback++;
            return VK_NULL_HANDLE;
        }
        P->readonly_bytes += alloc;
        g_stat_ro_allocs++;
    }
    memcpy(replace->buffer.mapped, src, (size_t)size);
    memset((uint8_t *)replace->buffer.mapped + size, 0, (size_t)(needed - size));
    replace->addr = addr;
    uint32_t index=(uint32_t)(replace-P->readonly);
    if(index>=P->readonly_count)P->readonly_count=index+1;
    replace->size = size;
    replace->used_batch = P->batch;
    replace->last_use = ++P->readonly_sequence;
    replace->checked_epoch=P->epoch;replace->checked_revision=P->readonly_revision;
    replace->checked_begin=0;replace->checked_end=size;*offset=0;
    g_stat_bytes += size;
    g_stat_ro_misses++;
    return replace->buffer.buffer;
}

/* Copies guest memory into the arena (unmapped pages read as zero). */
static bool arena_copy_guest(renderer *r, guest_addr addr, uint64_t size, VkDeviceSize *offset) {
    VkDeviceSize alloc = (size + 16 + 3) & ~3ull;
    if (!arena_alloc(r, alloc, offset)) return false;
    g_stat_bytes += size;
    g_stat_copy_by[g_copy_tag] += size;
    uint8_t *dst = (uint8_t *)arena_ptr(r, *offset);
    memset(dst + size, 0, alloc - size);
    const void *src = guest_host(r, addr, size);
    if (src) {
        memcpy(dst, src, size);
    } else {
        for (uint64_t o = 0; o < size; o += 4096) {
            uint64_t n = size - o < 4096 ? size - o : 4096;
            const void *s = guest_host(r, addr + o, n);
            if (s) memcpy(dst + o, s, n);
            else memset(dst + o, 0, n);
        }
    }
    return true;
}

/* A copy of [addr, addr + size) for a read (compute or draw) that earlier results of this batch
 * (the first `earlier` writebacks) cover in part: guest memory, with those results
 * copied in on the GPU. False when the arena is full or the results overlap each other.
 * PS5C_NO_PARTIAL_PATCH=1: the caller completes the batch first, as before. */
static bool arena_copy_patched(renderer *r, guest_addr addr, uint64_t size, uint32_t earlier, VkDeviceSize *offset) {
    static int off = -1;
    if (off < 0) off = getenv("PS5C_NO_PARTIAL_PATCH") != NULL;
    if (off) return false;
    vk_pipelines *P = r->pipes;
    VkBufferCopy regions[8];
    VkBuffer sources[8];
    uint32_t n = 0;
    for (uint32_t k = 0; k < earlier; k++) {
        const writeback *w = &P->wb[k];
        if (addr >= w->addr + w->size || w->addr >= addr + size) continue;
        guest_addr lo = addr > w->addr ? addr : w->addr;
        guest_addr hi = addr + size < w->addr + w->size ? addr + size : w->addr + w->size;
        for (uint32_t q = 0; q < n; q++) /* destinations of one copy must not overlap */
            if (lo - addr < regions[q].dstOffset + regions[q].size && regions[q].dstOffset < hi - addr) return false;
        if (n == 8) return false;
        sources[n] = w->res ? P->res[w->res - 1].buf.buffer : P->work_slots[w->slot].arena.buffer;
        if (!w->res && w->slot != work_slot(P)) P->cross_slot = true;
        if (w->res) P->res[w->res - 1].used_batch = P->batch;
        regions[n++] = (VkBufferCopy){(w->res ? w->addr - P->res[w->res - 1].addr : w->offset) + (lo - w->addr), lo - addr, hi - lo};
    }
    g_copy_tag = 1;
    bool copied_ = n && arena_copy_guest(r, addr, size, offset);
    g_copy_tag = 0;
    if (!copied_) return false;
    for (uint32_t q = 0; q < n; q++) regions[q].dstOffset += *offset;
    end_pass(r);
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    VkBuffer arena = P->work->arena.buffer;
    for (uint32_t q = 0; q < n; q++) vkCmdCopyBuffer(r->cmd, sources[q], arena, 1, &regions[q]);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    /* the reader is a compute shader or, in Crash Bandicoot 4's level, a draw */
    vkCmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    g_stat_patched++;
    return true;
}

/* A copy of [addr, addr + size) made earlier in this batch, or a new one. */
static bool arena_copy_guest_shared(renderer *r, guest_addr addr, uint64_t size, VkDeviceSize *offset) {
    vk_pipelines *P = r->pipes;
    /* Diagnostic only: @file selects normal reuse (1), fresh copies (0), or
     * bounded comparison without changing reuse (2). No setting keeps the
     * existing path; never update a snapshot already used by GPU commands. */
    static bool initialized;
    static const char *setting;
    static int mode = 1;
    static uint64_t poll_at, check_at, compared, mismatches;
    static uint32_t checks;
    if (!initialized) { setting = getenv("PS5C_COPY_CACHE_MODE"); initialized = true; }
    uint64_t now = setting ? pal_time_ns() : 0;
    if (setting && (!poll_at || now - poll_at >= 100000000ull)) {
        poll_at = now;
        int next = mode;
        if (setting[0] == '@') {
            FILE *f = fopen(setting + 1, "r");
            if (f) { if (fscanf(f, "%d", &next) != 1) next = mode; fclose(f); }
        } else next = atoi(setting);
        if (next >= 0 && next <= 2 && next != mode) {
            mode = next;
            LOG_I(GPU, "copy cache diagnostic: mode %d (0 fresh, 1 normal, 2 compare)", mode);
        }
    }
    if (!mode) return arena_copy_guest(r, addr, size, offset);
    if (size < COPY_CACHE_MIN) return arena_copy_guest(r, addr, size, offset);
    for (uint32_t i = 0; i < P->copy_count; i++) {
        const copy_entry *c = &P->copies[i];
        if (!c->patched && addr >= c->addr && addr + size <= c->addr + c->size && ((addr - c->addr) & (P->align - 1)) == 0) {
            *offset = c->offset + (addr - c->addr);
            if (mode == 2 && size <= (4u << 20)) {
                if (!check_at || now - check_at >= 1000000000ull) { check_at = now; checks = 0; }
                const void *src = checks < 32 ? guest_host(r, addr, size) : NULL;
                if (src) {
                    checks++;
                    bool changed = memcmp(src, arena_ptr(r, *offset), (size_t)size) != 0;
                    compared++;
                    if (changed && mismatches++ < 16)
                        LOG_W(GPU, "copy cache diagnostic: changed guest buffer 0x%llx +0x%llx batch %llu",
                              (unsigned long long)addr, (unsigned long long)size, (unsigned long long)P->batch);
                    if (compared == 1 || !(compared % 256))
                        LOG_I(GPU, "copy cache diagnostic: %llu checked, %llu changed (max 32/s, 4 MiB each)",
                              (unsigned long long)compared, (unsigned long long)mismatches);
                }
            }
            return true;
        }
    }
    if (!arena_copy_guest(r, addr, size, offset)) return false;
    copy_entry *c = &P->copies[P->copy_count < MAX_COPY_CACHE ? P->copy_count++ : P->copy_next++ % MAX_COPY_CACHE];
    c->addr = addr;
    c->size = size;
    c->offset = *offset;
    c->patched = 0;
    return true;
}

/* ---- Resident buffers (see resident_buf) ---- */

static vk_pipelines *_Atomic g_res_pipes; /* for resident_save, which runs on any thread */
static atomic_flag g_res_lock = ATOMIC_FLAG_INIT; /* the slots' metadata and originals against resident_save */

static void res_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_res_lock, memory_order_acquire)) {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#endif
    }
}
static void res_unlock(void) { atomic_flag_clear_explicit(&g_res_lock, memory_order_release); }

/* gpu_watch saver: a page some resident buffer has results pending for is
 * about to be written. Keep what it holds now, which is what the GPU read. */
static void resident_save(guest_addr page) {
    vk_pipelines *P = atomic_load_explicit(&g_res_pipes, memory_order_acquire);
    if (!P) return;
    res_lock();
    for (uint32_t i = 0; i < P->res_count; i++) {
        resident_buf *e = &P->res[i];
        if (!e->buf.buffer || !atomic_load_explicit(&e->cow, memory_order_relaxed) || page < e->addr ||
            page >= e->addr + e->size)
            continue;
        uint64_t k = (page - e->addr) / RESIDENT_PAGE;
        if (e->saved[k >> 3] & (1u << (k & 7))) continue;
        if (!e->orig) e->orig = (uint8_t *)malloc((size_t)e->size);
        if (!e->orig) {
            atomic_fetch_add(&g_res_lost, 1);
            continue;
        }
        memcpy(e->orig + k * RESIDENT_PAGE, (const void *)(uintptr_t)page, RESIDENT_PAGE);
        e->saved[k >> 3] |= (uint8_t)(1u << (k & 7));
        atomic_fetch_add_explicit(&g_res_saved, 1, memory_order_relaxed);
    }
    res_unlock();
}

static bool resident_on(vk_pipelines *P) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("PS5C_RESIDENT_BUFFERS");
        on = e && (e[0] == '@' || atoi(e)) && gpu_watch_enabled();
        if (e && atoi(e) && !on) LOG_W(GPU, "resident buffers need PS5C_GPU_WATCH=1: off");
        if (on) LOG_I(GPU, "resident buffers: on (experimental), budget %llu MiB", RESIDENT_BUDGET >> 20);
    }
    if (on) { /* PS5C_RESIDENT_BUFFERS=@file: off until the file says 1 (to start them after a level has loaded) */
        static const char *setting;
        static uint64_t poll;
        static bool live;
        static int file_mode = -1;
        if (file_mode < 0) {
            setting = getenv("PS5C_RESIDENT_BUFFERS");
            file_mode = setting && setting[0] == '@';
            live = !file_mode;
        }
        uint64_t now = file_mode && !live ? pal_time_ns() : 0;
        if (file_mode && !live && now - poll > 200000000ull) {
            poll = now;
            FILE *f = fopen(setting + 1, "r");
            int v = 0;
            if (f) { if (fscanf(f, "%d", &v) != 1) v = 0; fclose(f); }
            if (v) { live = true; LOG_I(GPU, "resident buffers: started"); }
        }
        if (!live) return false;
    }
    if (on && atomic_load_explicit(&g_res_pipes, memory_order_relaxed) != P) {
        atomic_store(&g_res_pipes, P);
        gpu_watch_set_saver(GPU_WATCH_BUFFERS, resident_save);
    }
    return on != 0;
}

/* Copy-on-write ends: the writeback was applied or dropped. */
static void resident_cow_end(resident_buf *e) {
    res_lock();
    atomic_store_explicit(&e->cow, false, memory_order_relaxed);
    memset(e->saved, 0, (size_t)(e->size / RESIDENT_PAGE / 8 + 1)); /* e->orig stays for the next writeback */
    res_unlock();
}

static void resident_release_wb(vk_pipelines *P, writeback *w) {
    resident_buf *e = &P->res[w->res - 1];
    e->wb_batch = 0;
    resident_cow_end(e);
    w->res = 0;
}

/* Only when no recorded command uses it and no results are pending in it. */
static void resident_drop(renderer *r, uint32_t i) {
    vk_pipelines *P = r->pipes;
    resident_buf *e = &P->res[i];
    vk_buffer buf = e->buf;
    uint8_t *saved = e->saved, *orig = e->orig;
    res_lock();
    P->res_bytes -= e->size;
    g_res_live--;
    g_res_live_bytes -= e->size;
    memset(e, 0, sizeof(*e));
    res_unlock();
    free(saved);
    free(orig);
    vk_buffer_destroy(r, &buf);
}

static bool resident_evictable(const vk_pipelines *P, const resident_buf *e) {
    return e->buf.buffer && e->used_batch != P->batch && e->wb_batch != P->batch;
}

static void resident_shutdown(renderer *r) {
    vk_pipelines *P = r->pipes;
    if (atomic_load(&g_res_pipes) == P) {
        gpu_watch_set_saver(GPU_WATCH_BUFFERS, NULL);
        atomic_store(&g_res_pipes, NULL);
    }
    for (uint32_t i = 0; i < P->res_count; i++)
        if (P->res[i].buf.buffer) resident_drop(r, i);
    P->res_count = 0;
}

/* Copies in the pages of e the CPU wrote since they were last read: straight
 * into the buffer when no recorded command uses it (earlier batches have
 * completed: no work overlap with resident buffers), else through the arena
 * in command order. Arm, then read: a write racing the copy faults and marks
 * the page again. */
static bool resident_refresh(renderer *r, resident_buf *e) {
    vk_pipelines *P = r->pipes;
    uint64_t generation = gpu_watch_generation();
    if (e->generation && e->generation == generation) return true;
    bool in_stream = e->used_batch == P->batch;
    guest_addr at = e->addr, end = e->addr + e->size, run;
    uint64_t len;
    VkBufferCopy regions[16];
    uint32_t n = 0;
    bool ok = true;
    while (at < end && gpu_watch_find_written(GPU_WATCH_BUFFERS, at, end - at, &run, &len)) {
        const void *src = guest_host(r, run, len);
        if (!src || !gpu_watch_arm(GPU_WATCH_BUFFERS, run, len)) {
            ok = false;
            break;
        }
        if (!in_stream) {
            memcpy((uint8_t *)e->buf.mapped + (run - e->addr), src, (size_t)len);
        } else {
            VkDeviceSize off;
            if (n == PS5C_ARRAY_COUNT(regions) || !arena_alloc(r, len, &off)) {
                ok = false;
                gpu_watch_prepare_write(run, len); /* armed but not copied: written again */
                break;
            }
            memcpy(arena_ptr(r, off), src, (size_t)len);
            regions[n++] = (VkBufferCopy){off, run - e->addr, len};
        }
        g_res_upload += len;
        g_stat_bytes += len;
        at = run + len;
    }
    if (n) {
        end_pass(r);
        VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
        vkCmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL,
                             0, NULL);
        vkCmdCopyBuffer(r->cmd, P->work->arena.buffer, e->buf.buffer, n, regions);
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL,
                             0, NULL);
    }
    /* the generation read before the scan: a write since then shows in the next one */
    if (ok) e->generation = generation;
    return ok;
}

static int resident_find(const vk_pipelines *P, guest_addr base, uint64_t size) {
    for (uint32_t i = 0; i < P->res_count; i++) {
        const resident_buf *e = &P->res[i];
        if (e->buf.buffer && base >= e->addr && base + size <= e->addr + e->size) return (int)i;
    }
    return -1;
}

/* A new resident buffer for [base, base + size): it takes over the ranges of
 * evictable buffers it overlaps (older views of the same memory). */
static int resident_create(renderer *r, guest_addr base, uint64_t size) {
    vk_pipelines *P = r->pipes;
    guest_addr a = base & ~(RESIDENT_PAGE - 1), z = (base + size + RESIDENT_PAGE - 1) & ~(RESIDENT_PAGE - 1);
    for (uint32_t i = 0; i < P->res_count; i++) {
        resident_buf *e = &P->res[i];
        if (!resident_evictable(P, e) || e->addr >= z || a >= e->addr + e->size) continue;
        if (e->addr < a) a = e->addr;
        if (e->addr + e->size > z) z = e->addr + e->size;
    }
    uint64_t n = z - a;
    if (n > RESIDENT_BUDGET / 2 || n + RESIDENT_PAGE > r->props.limits.maxStorageBufferRange) return -1;
    for (uint32_t f = 0; f < PS5C_ARRAY_COUNT(P->res_failed); f++)
        if (P->res_failed[f] == base && P->batch - P->res_failed_batch[f] < 256) return -1;
    if (!gpu_watch_eligible(a, n)) {
        uint32_t f = P->res_failed_next++ % PS5C_ARRAY_COUNT(P->res_failed);
        P->res_failed[f] = base;
        P->res_failed_batch[f] = P->batch;
        return -1;
    }
    for (uint32_t i = 0; i < P->res_count; i++) {
        resident_buf *e = &P->res[i];
        if (resident_evictable(P, e) && e->addr < z && a < e->addr + e->size) resident_drop(r, i);
    }
    int slot;
    for (;;) { /* a free slot within the budget, evicting the least recently used */
        slot = -1;
        for (uint32_t i = 0; i < P->res_count && slot < 0; i++)
            if (!P->res[i].buf.buffer) slot = (int)i;
        if (slot < 0 && P->res_count < MAX_RESIDENT) slot = (int)P->res_count;
        if (slot >= 0 && P->res_bytes + n <= RESIDENT_BUDGET) break;
        int lru = -1;
        for (uint32_t i = 0; i < P->res_count; i++)
            if (resident_evictable(P, &P->res[i]) && (lru < 0 || P->res[i].last_use < P->res[lru].last_use)) lru = (int)i;
        if (lru < 0) return -1;
        resident_drop(r, (uint32_t)lru);
    }
    vk_buffer buf;
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!vk_buffer_create(r, n + RESIDENT_PAGE, usage, true, &buf)) {
        vk_buffer_destroy(r, &buf);
        return -1;
    }
    uint8_t *saved = (uint8_t *)calloc((size_t)(n / RESIDENT_PAGE / 8 + 1), 1);
    if (!saved) {
        vk_buffer_destroy(r, &buf);
        return -1;
    }
    memset((uint8_t *)buf.mapped + n, 0, RESIDENT_PAGE);
    res_lock();
    resident_buf *e = &P->res[slot];
    memset(e, 0, sizeof(*e));
    e->addr = a;
    e->size = n;
    e->buf = buf;
    e->saved = saved;
    if ((uint32_t)slot == P->res_count) P->res_count++;
    P->res_bytes += n;
    g_res_live++;
    g_res_live_bytes += n;
    res_unlock();
    g_res_created++;
    /* never armed: every page reads as written, the first refresh copies them all */
    return slot;
}

/* The resident buffer holding [base, base + size) for a compute binding, its
 * contents current, at *offset. A store makes it a writeback of the batch.
 * -1: use the arena (results of other buffers pending over the range, or
 * memory that cannot be watched). */
static int resident_bind(renderer *r, guest_addr base, uint64_t size, bool store, VkDeviceSize *offset) {
    vk_pipelines *P = r->pipes;
    int k = resident_find(P, base, size);
    for (uint32_t i = 0; i < P->wb_count; i++) {
        const writeback *w = &P->wb[i];
        if (base < w->addr + w->size && w->addr < base + size && !(k >= 0 && w->res == (uint32_t)k + 1)) {
            g_res_fallbacks++;
            return -1;
        }
    }
    if (k < 0 && (k = resident_create(r, base, size)) < 0) {
        g_res_fallbacks++;
        return -1;
    }
    resident_buf *e = &P->res[k];
    if (e->wb_batch != P->batch) { /* no results pending in it: bring in what the CPU wrote */
        if (store) {
            if (P->wb_count == MAX_WRITEBACKS) return -1;
            res_lock(); /* originals from now on: before the refresh arms the pages */
            atomic_store_explicit(&e->cow, true, memory_order_relaxed);
            res_unlock();
        }
        if (!resident_refresh(r, e)) {
            if (store) resident_cow_end(e);
            if (resident_evictable(P, e)) resident_drop(r, (uint32_t)k);
            g_res_fallbacks++;
            return -1;
        }
        if (store) {
            writeback *w = &P->wb[P->wb_count++];
            memset(w, 0, sizeof(*w));
            w->addr = e->addr;
            w->size = e->size;
            w->res = (uint32_t)k + 1;
            w->slot = (uint8_t)work_slot(P);
            e->wb_batch = P->batch;
        }
    }
    e->used_batch = P->batch;
    e->last_use = ++P->res_seq;
    *offset = base - e->addr;
    g_res_binds++;
    return k;
}

/* The batch completed: the bytes the GPU changed go to guest memory. */
static void resident_apply(renderer *r, writeback *w) {
    vk_pipelines *P = r->pipes;
    resident_buf *e = &P->res[w->res - 1];
    uint8_t *dst = (uint8_t *)guest_host(r, w->addr, w->size);
    const uint8_t *src = (const uint8_t *)e->buf.mapped;
    for (uint64_t pg = 0; dst && pg < w->size; pg += RESIDENT_PAGE) {
        uint64_t k = pg / RESIDENT_PAGE;
        res_lock();
        bool saved = e->orig && (e->saved[k >> 3] & (1u << (k & 7)));
        res_unlock();
        if (!saved) {
            /* nobody wrote the page: it still holds what the GPU read */
            if (!memcmp(src + pg, dst + pg, RESIDENT_PAGE)) continue;
            /* writable now, its original saved first: a CPU write racing the merge is kept */
            gpu_watch_prepare_write(w->addr + pg, RESIDENT_PAGE);
            res_lock();
            saved = e->orig && (e->saved[k >> 3] & (1u << (k & 7)));
            res_unlock();
        }
        g_res_merged++;
        wb_merge(dst + pg, src + pg, saved ? e->orig + pg : dst + pg, RESIDENT_PAGE);
    }
    g_res_applied++;
    resident_release_wb(P, w);
}

static void copies_written(vk_pipelines *P, guest_addr addr, uint64_t size) {
    /* The persistent cache must recheck even a physical alias of this write. */
    P->readonly_revision++;
    for (uint32_t i = P->copy_count; i-- > 0;) {
        if (addr < P->copies[i].addr + P->copies[i].size && P->copies[i].addr < addr + size)
            P->copies[i] = P->copies[--P->copy_count];
    }
    P->copy_next = 0;
}

/* ---- Guest programs ---- */

static uint64_t fnv(const void *data, size_t size, uint64_t h) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < size; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

#define FNV_INIT 0xCBF29CE484222325ull

/* PS5C_SHADER_CORPUS=1: the translation inputs of every shader (and the code
 * of those whose analysis fails) go to user/<game>/corpus, one file per
 * distinct input, for `ps5c shaders` (shader_corpus.h). Game code: the
 * folder stays local. Called on the GPU thread (renderer lock). */
static void corpus_add(uint64_t key, const sh_program *p, const sh_stage_info *info, const sh_site_shape *shapes,
                       uint32_t binding_base, guest_addr base, const uint32_t *code, uint32_t dwords) {
    static int on = -1;
    static char dir[360];
    static hmap64 seen;
    if (on < 0) {
        on = getenv("PS5C_SHADER_CORPUS") && vk_shader_cache_game_dir()[0];
        if (on) {
            snprintf(dir, sizeof(dir), "%s/corpus", vk_shader_cache_game_dir());
            pal_mkdir(dir);
            hmap64_init(&seen);
            LOG_I(GPU, "shader corpus: %s", dir);
        }
    }
    if (!on || !code || hmap64_get(&seen, key, &(uint64_t){0})) return;
    hmap64_put(&seen, key, 1);
    char name[32], path[400];
    sh_corpus_name(key, name, sizeof(name));
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    pal_path_info pi;
    if (pal_path_stat(path, &pi) == PS5C_OK) return; /* from an earlier session */
    sh_corpus_entry *e = (sh_corpus_entry *)calloc(1, sizeof(sh_corpus_entry));
    if (!e) return;
    e->kind = info ? SH_CORPUS_TRANSLATION : SH_CORPUS_CODE;
    e->base = base;
    if (info) {
        e->info = *info;
        e->binding_base = binding_base;
        e->site_count = p->site_count;
        memcpy(e->shapes, shapes, sizeof(sh_site_shape) * p->site_count);
    }
    e->dwords = dwords;
    e->code = (uint32_t *)code;
    if (!sh_corpus_write(path, e)) LOG_W(GPU, "shader corpus: cannot write %s", path);
    free(e);
}

/* The code of a shader whose analysis failed: up to its s_endpgm, else a bounded prefix. */
static void corpus_add_code(guest_addr addr, const uint32_t *code, uint32_t avail) {
    if (!code) return;
    uint32_t n = 0, cap = avail < SH_MAX_CODE_DWORDS ? avail : SH_MAX_CODE_DWORDS;
    while (n < cap && code[n] != 0xBF810000u) n++; /* s_endpgm */
    n = n < cap ? n + 1 : cap;
    corpus_add(texture_hash_words(code, (size_t)n * 4, FNV_INIT) ^ 0xC0DEull, NULL, NULL, NULL, 0, addr, code, n);
}

static const sh_program *get_program(renderer *r, guest_addr addr) {
    vk_pipelines *P = r->pipes;
    if (!addr) return NULL;
    for (uint32_t i = 0; i < P->shader_count; i++) {
        guest_shader *s = &P->shaders[i];
        if (s->addr != addr) continue;
        if (s->checked_epoch == P->epoch) return s->ok ? s->prog : NULL;
        s->checked_epoch = P->epoch;
        if (!s->ok) return NULL;
        const uint32_t *code = (const uint32_t *)guest_host(r, addr, (uint64_t)s->prog->dwords * 4);
        if (code && fnv(code, (size_t)s->prog->dwords * 4, FNV_INIT) == s->prog->hash) return s->prog;
        /* code changed (the memory holds another program now): analyze it again */
        uint32_t avail = 0;
        code = vk_shader_code(r->mem, addr, &avail);
        vr_flow_invalidate(&P->vertex_flow);
        forget_wave_variants(P,s->prog);
        bool front_half = code && sh_front_dwords(code, avail) != 0; /* as below: get_fused_program's */
        s->ok = code && !front_half && analyze_cached_program(code, avail, addr, s->prog);
        if (!s->ok && !front_half) LOG_W(GPU, "shader 0x%llx: analysis failed (code replaced)", (unsigned long long)addr);
        if (!s->ok && !front_half) corpus_add_code(addr, code, avail);
        return s->ok ? s->prog : NULL;
    }
    guest_shader *s = NULL;
    if (P->shader_count == MAX_SHADERS) {
        /* reuse the least recently used entry; programs of this epoch stay (a draw holds them) */
        for (uint32_t i = 0; i < P->shader_count; i++) {
            guest_shader *c = &P->shaders[i];
            if (c->checked_epoch != P->epoch && (!s || c->checked_epoch < s->checked_epoch)) s = c;
        }
        if (!s) return NULL;
        vr_flow_invalidate(&P->vertex_flow);
        forget_wave_variants(P,s->prog);
        program_storage_free(s->prog,sparse_programs());
    } else {
        s = &P->shaders[P->shader_count++];
    }
    uint32_t avail = 0;
    const uint32_t *code = vk_shader_code(r->mem, addr, &avail);
    s->addr = addr;
    s->checked_epoch = P->epoch;
    vr_flow_invalidate(&P->vertex_flow);
    /* The first half of a merged-stage program returns through s_setpc_b64 s[6:7]:
     * not a program of its own (get_fused_program joins it with the second) */
    bool front_half = code && sh_front_dwords(code, avail) != 0;
    s->prog = code && !front_half ? program_storage_alloc(sparse_programs()) : NULL;
    s->ok = code && s->prog && !front_half && analyze_cached_program(code, avail, addr, s->prog);
    if (front_half) LOG_D(GPU, "shader 0x%llx: first half of a merged program", (unsigned long long)addr);
    else if (!s->ok) LOG_W(GPU, "shader 0x%llx: analysis failed", (unsigned long long)addr);
    if (!s->ok && !front_half) corpus_add_code(addr, code, avail);
    else if (s->ok && getenv("PS5C_LOG_SHADERS")) {
        LOG_I(GPU, "shader 0x%llx: %u dwords, hash %016llx", (unsigned long long)addr, s->prog->dwords,
              (unsigned long long)s->prog->hash);
        /* PS5C_LOG_SHADERS=<hash>[,<hash>...]: those programs' instructions too (debugging aid) */
        char hash[17];
        snprintf(hash, sizeof(hash), "%016llx", (unsigned long long)s->prog->hash);
        if (strstr(getenv("PS5C_LOG_SHADERS"), hash))
            for (uint32_t j = 0; j < s->prog->count; j++) {
                char text[128];
                rdna2_disasm(&s->prog->insts[j], text, sizeof(text));
                LOG_I(GPU, "  +0x%x %s", s->prog->pc[j] * 4, text);
            }
    }
    return s->ok ? s->prog : NULL;
}

/* Fixed-function clears and opaque depth-only draws need no guest PS. Keep
 * fragments alive with a null export: a bare s_endpgm would discard them.
 * Scissored clears themselves still use the existing clear-attachment path. */
static const sh_program *depth_clear_program(renderer *r) {
    static const uint32_t code[] = {0xF8001890u,0,0xBF810000u}; /* exp null done vm; s_endpgm */
    sh_program **p = &r->pipes->depth_clear_ps;
    if (!*p) {
        *p = (sh_program *)malloc(sizeof(**p));
        if (*p && !sh_analyze(code, PS5C_ARRAY_COUNT(code), 0, *p)) {
            free(*p);
            *p = NULL;
        }
    }
    return *p;
}

/* The two halves of a merged-stage program or a RADV pixel shader. The
 * optional null export after a pixel shader's indirect jump keeps its
 * original address for branches that skip the color export. */
static uint32_t *join_halves(renderer *r, guest_addr es, guest_addr gs, uint32_t front_words, uint32_t *dwords) {
    uint32_t avail = 0, back = 0;
    const uint32_t *front = vk_shader_code(r->mem, es, &avail);
    const uint32_t *bcode = vk_shader_code(r->mem, gs, &back);
    if (!front || !bcode) return NULL;
    uint32_t n = front_words ? front_words : sh_front_dwords(front, avail);
    if (!n || n > avail) return NULL;
    uint32_t null_export_words = 0;
    if (front_words) {
        if (front[n - 1] != 0xBE802000u && front[n - 1] != 0xBE802002u) return NULL;
        if (avail - n >= 3 && front[n] == 0xF8001890u &&
            front[n + 1] == 0x80808080u && front[n + 2] == 0xBF810000u)
            null_export_words = 3;
        else if (n == avail || front[n] != 0xBF9F0000u)
            return NULL;
    }
    uint32_t epilog_start = n + null_export_words;
    uint32_t *code = (uint32_t *)malloc(((size_t)epilog_start + back) * 4);
    if (!code) return NULL;
    memcpy(code, front, (size_t)epilog_start * 4);
    code[n - 1] = null_export_words ? 0xBF820000u | null_export_words : 0xBF800000u;
    memcpy(code + epilog_start, bcode, (size_t)back * 4);
    *dwords = epilog_start + back;
    return code;
}

static const sh_program *get_fused_program(renderer *r, guest_addr es, guest_addr gs, uint32_t front_words) {
    vk_pipelines *P = r->pipes;
    if (!es || !gs) return NULL;
    fused_shader *f = NULL;
    for (uint32_t i = 0; i < P->fused_count && !f; i++)
        if (P->fused[i].es == es && P->fused[i].gs == gs && P->fused[i].front_words == front_words) f = &P->fused[i];
    if (f && f->checked_epoch == P->epoch) return f->ok ? f->prog : NULL;
    uint32_t dwords = 0;
    uint32_t *code = join_halves(r, es, gs, front_words, &dwords);
    if (f && code && f->code && f->prog && f->ok && f->prog->dwords <= dwords &&
        !memcmp(code, f->code, (size_t)f->prog->dwords * 4)) {
        free(code); /* unchanged */
        f->checked_epoch = P->epoch;
        return f->prog;
    }
    if (!f) {
        f = &P->fused[P->fused_count < MAX_FUSED ? P->fused_count++ : P->fused_next++ % MAX_FUSED];
        vr_flow_invalidate(&P->vertex_flow);
        forget_wave_variants(P,f->prog);
        program_storage_free(f->prog,sparse_programs()), free(f->code);
        memset(f, 0, sizeof(*f));
    }
    forget_wave_variants(P,f->prog);
    free(f->code);
    f->es = es, f->gs = gs, f->code = code, f->dwords = dwords, f->checked_epoch = P->epoch;
    f->front_words = front_words;
    if (!f->prog) f->prog = program_storage_alloc(sparse_programs());
    vr_flow_invalidate(&P->vertex_flow);
    f->ok = code && f->prog && analyze_cached_program(code, dwords, es, f->prog);
    static uint32_t logged;
    if (logged++ < 16)
        LOG_I(GPU, "merged program 0x%llx + 0x%llx: %s", (unsigned long long)es, (unsigned long long)gs,
              f->ok ? "joined" : "analysis failed");
    return f->ok ? f->prog : NULL;
}

/* PS5 RADV's separate pixel epilog: ACO saves USER_DATA_3 in s0 or s1,
 * then jumps through s[0:1] or s[2:3] with a high address word of 2. */
static const sh_program *get_radv_pixel_program(renderer *r, guest_addr addr, uint32_t lo) {
    if (!getenv("PS5C_RADV_PS_EPILOG")) return NULL;
    if (!lo || (lo & 3)) return NULL;
    uint32_t avail = 0;
    const uint32_t *code = vk_shader_code(r->mem, addr, &avail);
    if (!code || avail < 4) return NULL;
    uint32_t start = code[0] == 0xBE80037Eu ? 1 : 0; /* optional save of exec_lo */
    if ((code[start] != 0xBE800303u && code[start] != 0xBE810303u) ||
        code[start + 1] != 0xBE830382u)
        return NULL;
    bool low_in_s0 = code[start] == 0xBE800303u, low_in_s2 = false, high_in_s1 = false;
    for (uint32_t i = start + 2; i < avail && i < SH_MAX_CODE_DWORDS;) {
        rdna2_inst inst;
        uint32_t word = code[i];
        if (word == 0xBE800301u) low_in_s0 = true;
        if (word == 0xBE820301u) low_in_s2 = true;
        if (word == 0xBE810382u) high_in_s1 = true;
        if (word == 0xBF810000u || word == 0xBF9F0000u) return NULL;
        if ((word == 0xBE802000u && low_in_s0 && high_in_s1) ||
            (word == 0xBE802002u && low_in_s2)) {
            return get_fused_program(r, addr, 0x200000000ull | lo, i + 1);
        }
        if (!rdna2_decode(code + i, avail - i, &inst)) return NULL;
        i += inst.words;
    }
    return NULL;
}

/* ---- Formats ---- */

/* PS5C_NO_ALIAS_VIEWS=1: images are not viewed in another format (read once: getenv locks) */
static bool no_alias_views(void) {
    static int off = -1;
    if (off < 0) off = getenv("PS5C_NO_ALIAS_VIEWS") != NULL;
    return off != 0;
}

static VkFormat cb_format(uint32_t info) {
    uint32_t fmt = (info >> 2) & 0x1F, num = (info >> 8) & 7, swap = (info >> 11) & 3;
    switch (fmt) {
    case 1: return num == 4 ? VK_FORMAT_R8_UINT : VK_FORMAT_R8_UNORM;
    case 2: return num == 7 ? VK_FORMAT_R16_SFLOAT : num == 4 ? VK_FORMAT_R16_UINT : VK_FORMAT_R16_UNORM;
    case 3: return num == 4 ? VK_FORMAT_R8G8_UINT : VK_FORMAT_R8G8_UNORM;
    case 4: return num == 7 ? VK_FORMAT_R32_SFLOAT : num == 5 ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT;
    case 5: return num == 7 ? VK_FORMAT_R16G16_SFLOAT : num == 4 ? VK_FORMAT_R16G16_UINT : VK_FORMAT_R16G16_UNORM;
    case 6: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 9: return swap == 1 ? VK_FORMAT_A2R10G10B10_UNORM_PACK32 : VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 10:
        if (num == 6) return swap == 1 ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_R8G8B8A8_SRGB;
        if (num == 4) return VK_FORMAT_R8G8B8A8_UINT;
        if (num == 1) return VK_FORMAT_R8G8B8A8_SNORM;
        return swap == 1 ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    case 11: return num == 7 ? VK_FORMAT_R32G32_SFLOAT : VK_FORMAT_R32G32_UINT;
    case 12: return num == 7 ? VK_FORMAT_R16G16B16A16_SFLOAT : num == 4 ? VK_FORMAT_R16G16B16A16_UINT : VK_FORMAT_R16G16B16A16_UNORM;
    case 14: return num == 7 ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R32G32B32A32_UINT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

/* Writes to absent components do not access a color attachment on the guest.
 * An RG target with only BA enabled may still be a storage resource in that
 * draw; binding it as an attachment would create a forbidden Vulkan alias. */
static uint32_t color_component_mask(VkFormat format) {
    switch(format) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_UINT:
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_SFLOAT: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SFLOAT: return 3;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return 7;
    default: return 15;
    }
}

/* SPIR-V fragment outputs must have the attachment's numeric type. The
 * 32-bit export registers contain raw integers, not float representations. */
static uint32_t color_output_kind(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32A32_UINT:
        return 1;
    case VK_FORMAT_R32_SINT: return 2;
    default: return 0;
    }
}

/* Image format (GFX10 numbering) -> host format, bytes per element and
 * element size in texels (4 for block-compressed formats). */
static VkFormat img_format(uint32_t fmt, uint32_t *bpe, uint32_t *block) {
    *bpe = 4;
    *block = 1;
    switch (fmt) {
    case 1: *bpe = 1; return VK_FORMAT_R8_UNORM;
    case 5: *bpe = 1; return VK_FORMAT_R8_UINT;
    case 6: *bpe = 1; return VK_FORMAT_R8_SINT;
    case 7: *bpe = 2; return VK_FORMAT_R16_UNORM;
    case 8: *bpe = 2; return VK_FORMAT_R16_SNORM;
    case 11: *bpe = 2; return VK_FORMAT_R16_UINT;
    case 12: *bpe = 2; return VK_FORMAT_R16_SINT;
    case 13: *bpe = 2; return VK_FORMAT_R16_SFLOAT;
    case 14: *bpe = 2; return VK_FORMAT_R8G8_UNORM;
    case 15: *bpe = 2; return VK_FORMAT_R8G8_SNORM;
    case 18: *bpe = 2; return VK_FORMAT_R8G8_UINT;
    case 19: *bpe = 2; return VK_FORMAT_R8G8_SINT;
    case 20: return VK_FORMAT_R32_UINT;
    case 21: return VK_FORMAT_R32_SINT;
    case 22: return VK_FORMAT_R32_SFLOAT;
    case 23: return VK_FORMAT_R16G16_UNORM;
    case 24: return VK_FORMAT_R16G16_SNORM;
    case 27: return VK_FORMAT_R16G16_UINT;
    case 28: return VK_FORMAT_R16G16_SINT;
    case 29: return VK_FORMAT_R16G16_SFLOAT;
    case 36: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 50: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 56: return VK_FORMAT_R8G8B8A8_UNORM;
    case 57: return VK_FORMAT_R8G8B8A8_SNORM;
    case 60: return VK_FORMAT_R8G8B8A8_UINT;
    case 61: return VK_FORMAT_R8G8B8A8_SINT;
    case 62: *bpe = 8; return VK_FORMAT_R32G32_UINT;
    case 63: *bpe = 8; return VK_FORMAT_R32G32_SINT;
    case 64: *bpe = 8; return VK_FORMAT_R32G32_SFLOAT;
    case 65: *bpe = 8; return VK_FORMAT_R16G16B16A16_UNORM;
    case 66: *bpe = 8; return VK_FORMAT_R16G16B16A16_SNORM;
    case 69: *bpe = 8; return VK_FORMAT_R16G16B16A16_UINT;
    case 70: *bpe = 8; return VK_FORMAT_R16G16B16A16_SINT;
    case 71: *bpe = 8; return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 77: *bpe = 16; return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 75: *bpe = 16; return VK_FORMAT_R32G32B32A32_UINT;
    case 76: *bpe = 16; return VK_FORMAT_R32G32B32A32_SINT;
    case 128: *bpe = 1; return VK_FORMAT_R8_SRGB;
    case 129: *bpe = 2; return VK_FORMAT_R8G8_SRGB;
    case 130: return VK_FORMAT_R8G8B8A8_SRGB;
    case 133: case 134: case 135: case 136: case 137: /* bpe: the guest texel; RGBA8 copy (vk_packed_expand) */
        *bpe = vk_packed_bytes(fmt); return VK_FORMAT_R8G8B8A8_UNORM;
    default: break;
    }
    static const VkFormat k_bc[14] = {
        VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK,
        VK_FORMAT_BC2_SRGB_BLOCK,       VK_FORMAT_BC3_UNORM_BLOCK,      VK_FORMAT_BC3_SRGB_BLOCK,
        VK_FORMAT_BC4_UNORM_BLOCK,      VK_FORMAT_BC4_SNORM_BLOCK,      VK_FORMAT_BC5_UNORM_BLOCK,
        VK_FORMAT_BC5_SNORM_BLOCK,      VK_FORMAT_BC6H_UFLOAT_BLOCK,    VK_FORMAT_BC6H_SFLOAT_BLOCK,
        VK_FORMAT_BC7_UNORM_BLOCK,      VK_FORMAT_BC7_SRGB_BLOCK};
    if (fmt >= 169 && fmt <= 182) {
        uint32_t i = fmt - 169;
        *block = 4;
        *bpe = i < 2 || i == 6 || i == 7 ? 8 : 16;
        return k_bc[i];
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

/* ---- Render passes, framebuffers, samplers ---- */

/* A render pass for these targets (pipeline builds on other threads make their own, compatible one). */
static VkRenderPass make_render_pass(VkDevice device, const VkFormat *fmt, uint32_t count, VkFormat depth, bool depth_read_only) {
    VkAttachmentDescription att[GPU_MAX_RENDER_TARGETS + 1];
    VkAttachmentReference ref[GPU_MAX_RENDER_TARGETS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (fmt[i] == VK_FORMAT_UNDEFINED) {
            ref[i].attachment = VK_ATTACHMENT_UNUSED;
            ref[i].layout = VK_IMAGE_LAYOUT_UNDEFINED;
            continue;
        }
        memset(&att[n], 0, sizeof(att[n]));
        att[n].format = fmt[i];
        att[n].samples = VK_SAMPLE_COUNT_1_BIT;
        att[n].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[n].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[n].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ref[i].attachment = n++;
        ref[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkImageLayout depth_layout = depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL :
                                                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference dref = {VK_ATTACHMENT_UNUSED, depth_layout};
    if (depth != VK_FORMAT_UNDEFINED) {
        memset(&att[n], 0, sizeof(att[n]));
        att[n].format = depth;
        att[n].samples = VK_SAMPLE_COUNT_1_BIT;
        att[n].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        bool stencil = vk_format_has_stencil(depth);
        att[n].stencilLoadOp = stencil ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[n].stencilStoreOp = stencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[n].initialLayout = depth_layout;
        att[n].finalLayout = depth_layout;
        dref.attachment = n++;
    }
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = count;
    sub.pColorAttachments = ref;
    if (depth != VK_FORMAT_UNDEFINED) sub.pDepthStencilAttachment = &dref;
    VkSubpassDependency dep = {0};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = n;
    ci.pAttachments = att;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;
    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(device, &ci, NULL, &rp) != VK_SUCCESS) return VK_NULL_HANDLE;
    return rp;
}

static VkRenderPass get_render_pass(renderer *r, const VkFormat *fmt, uint32_t count, VkFormat depth, bool depth_read_only) {
    vk_pipelines *P = r->pipes;
    for (uint32_t i = 0; i < P->rp_count; i++) {
        if (P->rps[i].count == count && P->rps[i].depth == depth &&
            P->rps[i].depth_read_only == depth_read_only &&
            !memcmp(P->rps[i].fmt, fmt, count * sizeof(VkFormat)))
            return P->rps[i].rp;
    }
    if (P->rp_count == MAX_RPS) return VK_NULL_HANDLE;
    rp_entry *e = &P->rps[P->rp_count];
    /* dynamic rendering: the entry only names the attachment formats (no Vulkan object) */
    e->rp = r->dyn_render ? (VkRenderPass)(uintptr_t)(P->rp_count + 1) : make_render_pass(r->device, fmt, count, depth, depth_read_only);
    if (!e->rp) return VK_NULL_HANDLE;
    e->count = count;
    e->depth = depth;
    e->depth_read_only = depth_read_only;
    memcpy(e->fmt, fmt, count * sizeof(VkFormat));
    P->rp_count++;
    return e->rp;
}

static VkFramebuffer get_framebuffer(renderer *r, VkRenderPass rp, const VkImageView *views, uint32_t count,
                                     uint32_t w, uint32_t h, uint32_t layers) {
    vk_pipelines *P = r->pipes;
    for (uint32_t i = 0; i < P->fb_count; i++) {
        fb_entry *e = &P->fbs[i];
        if (e->rp == rp && e->count == count && e->width == w && e->height == h && e->layers == layers &&
            !memcmp(e->views, views, count * sizeof(VkImageView))) {
            return e->fb;
        }
    }
    if (P->fb_count == MAX_FBS) {
        /* Dynamic rendering: the entries only name sets of views with ids that
         * are never reused, so the cache simply starts over (an open pass gets
         * a new id at worst, which ends and reopens it). Before, the full cache
         * failed the draw and the batch was flushed and waited for. */
        if (r->dyn_render) P->fb_count = 0;
        /* The caller must retry the whole draw in a new batch. Flushing here
         * would leave its already-selected targets unprotected from eviction. */
        else return VK_NULL_HANDLE;
    }
    fb_entry *e = &P->fbs[P->fb_count];
    VkFramebufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    ci.renderPass = rp;
    ci.attachmentCount = count;
    ci.pAttachments = views;
    ci.width = w;
    ci.height = h;
    ci.layers = layers;
    if (r->dyn_render) { /* the entry only names the set of views a pass is open on */
        static uint64_t next_id;
        e->fb = (VkFramebuffer)(uintptr_t)++next_id;
    } else if (vkCreateFramebuffer(r->device, &ci, NULL, &e->fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    g_stat_fb_created++;
    e->layers = layers;
    e->rp = rp;
    memcpy(e->views, views, count * sizeof(VkImageView));
    e->count = count;
    e->width = w;
    e->height = h;
    P->fb_count++;
    return e->fb;
}

static VkSamplerAddressMode clamp_mode(uint32_t c) {
    switch (c) {
    case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4:
    case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

static VkSampler get_sampler(renderer *r, const uint32_t *s, VkFormat format) {
    vk_pipelines *P = r->pipes;
    uint8_t support = (uint32_t)format < PS5C_ARRAY_COUNT(P->filter_support) ? P->filter_support[format] : 0;
    if (!support) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(r->phys, format, &props);
        support = 1 | (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ? 2 : 0) |
                  (props.optimalTilingFeatures & 0x00010000u /* SAMPLED_IMAGE_FILTER_MINMAX */ ? 4 : 0);
        if ((uint32_t)format < PS5C_ARRAY_COUNT(P->filter_support)) P->filter_support[format] = support;
    }
    bool linear_ok = support & 2;
    VkSamplerCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    bool mip_disabled = ((s[2] >> 26) & 3) == 0;
    ci.magFilter = linear_ok && ((s[2] >> 20) & 3) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = linear_ok && ((s[2] >> 22) & 3) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = linear_ok && ((s[2] >> 26) & 3) == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    /* S# (GFX10 and GCN alike): word 0 [11:9] MAX_ANISO_RATIO (1x..16x), [30:29] FILTER_MODE
     * (blend, min, max); word 2 [13:0] LOD_BIAS (s5.8), [21:20]/[23:22] XY_MAG/MIN_FILTER (2 and 3
     * anisotropic); word 3 [31:30] BORDER_COLOR_TYPE (transparent black, opaque black, opaque
     * white, register table) */
    uint32_t aniso_log2 = (s[0] >> 9) & 7, reduction = (s[0] >> 29) & 3, border = s[3] >> 30;
    int32_t bias = (int32_t)((s[2] & 0x3FFFu) << 18) >> 18;
    static int old = -1; /* PS5C_SAMPLER_OLD=1: address and filter modes only, as before (A/B) */
    if (old < 0) old = getenv("PS5C_SAMPLER_OLD") != NULL;
    if (old) aniso_log2 = reduction = border = 0, bias = 0;
    bool aniso_filter = ((s[2] >> 22) & 3) >= 2 || ((s[2] >> 20) & 3) >= 2;
    if (aniso_log2 > 4) aniso_log2 = 4; /* reserved ratios 5-7: 16x (as Kyty) */
    if (!aniso_filter || !linear_ok || r->max_aniso < 2.0f) aniso_log2 = 0;
    if (aniso_log2) {
        float ratio = (float)(1u << aniso_log2);
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = ratio < r->max_aniso ? ratio : r->max_aniso;
    }
    if (reduction > 2 || !r->sampler_minmax || !(support & 4)) reduction = 0;
    VkSamplerReductionModeCreateInfoEXT red = {.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO_EXT,
                                               .reductionMode = (VkSamplerReductionModeEXT)reduction};
    if (reduction) ci.pNext = &red;
    float lod_bias = (float)bias / 256.0f, bias_max = r->props.limits.maxSamplerLodBias;
    ci.mipLodBias = lod_bias > bias_max ? bias_max : lod_bias < -bias_max ? -bias_max : lod_bias;
    ci.addressModeU = clamp_mode(s[0] & 7);
    ci.addressModeV = clamp_mode((s[0] >> 3) & 7);
    ci.addressModeW = clamp_mode((s[0] >> 6) & 7);
    /* MIP_FILTER=0 samples the view's base level. A zero maxLod would
     * incorrectly force MAG even when the guest requests minification.
     * NEAREST + .25 keeps mip0 while preserving the MIN/MAG decision. */
    ci.maxLod = mip_disabled ? .25f : 16.0f;
    ci.borderColor = border == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                     : border == 2 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                   : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK; /* 3: a table entry, not modelled */
    /* Cache the effective state, not unused or equivalent guest bits. Never
     * substitute a different sampler when the cache fills: even its numeric
     * filtering can be invalid. */
    uint64_t key = (uint64_t)ci.addressModeU | ((uint64_t)ci.addressModeV << 2) |
                   ((uint64_t)ci.addressModeW << 4) | ((uint64_t)ci.magFilter << 6) |
                   ((uint64_t)ci.minFilter << 7) | ((uint64_t)ci.mipmapMode << 8) |
                   ((uint64_t)mip_disabled << 9) | ((uint64_t)aniso_log2 << 10) | ((uint64_t)reduction << 13) |
                   ((uint64_t)(border == 1 || border == 2 ? border : 0) << 15) |
                   ((uint64_t)(uint32_t)(int32_t)(ci.mipLodBias * 256.0f) & 0x3FFFu) << 17;
    for (uint32_t i = 0; i < P->sampler_count; i++)
        if (P->samplers[i].key == key) {
            P->samplers[i].last_use=r->use_seq;
            return P->samplers[i].sampler;
        }
    if (P->sampler_count == MAX_SAMPLERS) {
        uint32_t unused=0;
        for(uint32_t i=0;i<P->sampler_count;i++)unused+=P->samplers[i].last_use<r->use_seq;
        if(!unused) {
            /* All handles are referenced by this recording batch. Complete it
             * and retry resources rather than replacing a live sampler. */
            r->image_cache_pressure=true;
            return VK_NULL_HANDLE;
        }
        /* Earlier work can still be in the other in-flight slot. Wait before
         * destroying any of its samplers; current-batch entries stay pinned.
         * Reclaim together so this wait occurs once per cache-sized burst. */
        if(vkQueueWaitIdle(r->queue)!=VK_SUCCESS)return VK_NULL_HANDLE;
        uint32_t kept=0;
        for(uint32_t i=0;i<P->sampler_count;i++) {
            if(P->samplers[i].last_use<r->use_seq)vkDestroySampler(r->device,P->samplers[i].sampler,NULL);
            else P->samplers[kept++]=P->samplers[i];
        }
        P->sampler_count=kept;
        static uint32_t reports;
        if(reports++<8)LOG_I(GPU,"sampler cache: reclaimed %u completed entries, %u pinned by the current batch",unused,kept);
    }

    sampler_entry *e = &P->samplers[P->sampler_count];
    if (vkCreateSampler(r->device, &ci, NULL, &e->sampler) != VK_SUCCESS) return VK_NULL_HANDLE;
    static uint32_t seen; /* each sampler feature's first use (debugging aid) */
    uint32_t now = (aniso_log2 ? 1u : 0u) | (reduction ? 2u : 0u) | (bias ? 4u : 0u) | (border == 1 || border == 2 ? 8u : 0u);
    if (now & ~seen) {
        seen |= now;
        LOG_I(GPU, "sampler: anisotropy %ux, reduction %u, LOD bias %.2f, border %u (sampler %u, S# %08x %08x %08x %08x)",
              1u << aniso_log2, reduction, (double)ci.mipLodBias, border, P->sampler_count + 1, s[0], s[1], s[2], s[3]);
    }
    e->key = key;
    e->last_use=r->use_seq;
    P->sampler_count++;
    return e->sampler;
}

/* ---- Pipelines ---- */

typedef struct stage_state {
    const sh_program *prog;
    sh_program *linked_program;
    sh_eval eval;
    sh_site_shape shapes[SH_MAX_SITES];
    sh_stage_info info;
    bool wave_locked, wave32_expected; /* authoritative opt-in stage state survives linking */
    bool trim_vertex_buffers;
    bool gcn; /* PS4 shaders: their descriptors are GFX7 ones */
    bool images_only; /* reuse image staging for an isolated finite-table dispatch */
    ngg_large_reads *large_reads; /* per-draw, pinned readonly imports; not cached */
    uint32_t vertex_max, instance_max;
    uint32_t groups[3]; /* compute: workgroups of the dispatch (store windows) */
    eval_ranges footprint_reads;
    uint64_t footprint_hash;
    bool footprint_hash_valid;
    uint32_t footprint_count;
    struct {guest_addr base;uint64_t size,lo,hi;} footprint[16];
    uint32_t dispatch_base[3]; /* guest WGID origin of this legal host part */
    VkDeviceSize u_off, u_bytes; /* its uniform buffer in the arena (PS5C_VS_PROBE reads past the end) */
    VkDeviceSize mesh_index_off;
    uint32_t mesh_index_count, mesh_index_size;
    VkDeviceSize mesh_output_off;uint32_t mesh_output_groups;
} stage_state;

/* Exact sorted descriptor spans, not a hash: changing bindings starts a new
 * pool rather than filling the old pool and silently clipping a new range. */
static bool large_import_signature(const renderer *r,const stage_state *st,ngg_large_key keys[16],uint32_t *count) {
    *count=0;
    for(uint32_t stage=0;stage<2;stage++)for(uint32_t i=0;i<st[stage].prog->site_count;i++) {
        const sh_site_value *v=st[stage].eval.site+i;sh_site_kind kind=st[stage].prog->sites[i].kind;
        if(kind!=SH_SITE_BUFFER && kind!=SH_SITE_SBUFFER)continue;
        uint32_t stride=(v->desc[1]>>16)&0x3fff;
        uint64_t bytes=stride?(uint64_t)v->desc[2]*stride:v->desc[2];
        if(!v->valid || v->desc_unknown || bytes<=MAX_BUFFER_COPY || bytes>r->props.limits.maxStorageBufferRange)continue;
        if(v->records_unknown)return false;
        guest_addr base=(guest_addr)v->desc[0]|((guest_addr)(v->desc[1]&65535)<<32);
        uint32_t at=0;while(at<*count && (keys[at].base<base || (keys[at].base==base && keys[at].bytes<bytes)))at++;
        if(at<*count && keys[at].base==base && keys[at].bytes==bytes)continue;
        if(*count==16)return false;
        memmove(keys+at+1,keys+at,(*count-at)*sizeof(*keys));keys[at]=(ngg_large_key){base,bytes};(*count)++;
    }
    return *count!=0;
}

/* Reuse objects, never the copied contents of a snapshot. A smaller binding
 * can borrow the same pinned allocation; ngg_large_bind still validates its
 * current VMA, image aliases, physical overlap and actual descriptor range.
 * The bounded draw retirement and every existing synchronization stay intact. */
static bool large_import_signature_reusable(const vk_pipelines *P,const ngg_large_key *keys,uint32_t count,uint64_t flip) {
    if(P->large_import_key_count==count &&
       !memcmp(P->large_import_keys,keys,count*sizeof(*keys)))return true;
    static bool initialized,contained,seen;static const char *setting;static uint64_t last_flip;
    if(!initialized){setting=getenv("PS5C_LARGE_IMPORT_CONTAINED");initialized=true;}
    if(!seen || last_flip!=flip) {
        bool enabled=setting && !strcmp(setting,"1");
        if(setting && setting[0]=='@') {
            FILE*f=fopen(setting+1,"r");int value=0;
            enabled=f && fscanf(f,"%d",&value)==1 && value==1;if(f)fclose(f);
        }
        if(setting && (!seen || contained!=enabled))
            LOG_I(GPU,"contained readonly import reuse: %s (experimental)",enabled?"enabled":"disabled");
        contained=enabled;seen=true;last_flip=flip;
    }
    if(!contained || !P->large_import_cache || P->large_import_cache->has_snapshot ||
       P->large_import_cache->write_count || P->large_import_cache->excluded_count)return false;
    for(uint32_t i=0;i<count;i++) {
        if(!keys[i].bytes || keys[i].base>UINT64_MAX-keys[i].bytes)return false;
        bool covered=false;
        for(uint32_t j=0;j<P->large_import_key_count;j++) {
            const ngg_large_key *old=P->large_import_keys+j;
            if(old->base<=UINT64_MAX-old->bytes && keys[i].base>=old->base &&
               keys[i].bytes<=old->bytes && keys[i].base-old->base<=old->bytes-keys[i].bytes) {
                covered=true;break;
            }
        }
        if(!covered)return false;
    }
    return count!=0;
}

/* Optional file permits a one-frame correctness comparison without rebooting
 * the title. Every draw of a flip uses the same value. */
static bool ngg_large_reads_on(uint64_t flip) {
    static bool initialized,enabled;static const char *setting;
    static uint64_t checked=UINT64_MAX;
    if(!initialized){setting=getenv("PS5C_NGG_LARGE_READS");initialized=true;}
    if(!setting)return false;
    if(setting[0]!='@')return !strcmp(setting,"1");
    if(checked!=flip) {
        checked=flip;enabled=false;FILE *f=fopen(setting+1,"r");int value=0;
        if(f){if(fscanf(f,"%d",&value)==1)enabled=value==1;fclose(f);}
    }
    return enabled;
}

static void free_draw_stages(stage_state *st) {
    if(!st)return;
    sh_eval_free(&st[0].eval);sh_eval_free(&st[1].eval);
    program_storage_free(st[0].linked_program,sparse_programs());
    program_storage_free(st[1].linked_program,sparse_programs());free(st);
}

/* Independent opt-in switch permits comparison without changing stage reuse.
 * Renderer lock protects the metadata; poll an optional control file per flip. */
static bool cache_vertex_flow(vk_pipelines *P, uint64_t flip) {
    static const char *setting;
    static bool initialized;
    if (!initialized) { setting = getenv("PS5C_CACHE_VERTEX_FLOW"); initialized = true; }
    if (!setting) return false;
    if (!P->vertex_flow_seen || P->vertex_flow_flip != flip) {
        bool enabled = false;
        if (setting[0] == '@') {
            FILE *f = fopen(setting + 1, "r");
            int value = 0;
            if (f) { if (fscanf(f, "%d", &value) == 1) enabled = value == 1; fclose(f); }
        } else enabled = !strcmp(setting, "1");
        if (!P->vertex_flow_seen || enabled != P->vertex_flow_enabled)
            LOG_I(GPU, "vertex flow cache: %s (experimental)", enabled ? "enabled" : "disabled");
        P->vertex_flow_seen = true;
        P->vertex_flow_flip = flip;
        P->vertex_flow_enabled = enabled;
    }
    return P->vertex_flow_enabled;
}

/* Reuse only storage: setup_stage writes the new program's entire observable
 * site/shape prefix, on both cache hits and misses. Its inactive capacity is
 * never consulted. Reset stage metadata and optional pointers on every draw.
 * The renderer lock serializes users; commands already own uniform copies. */
static bool reuse_draw_stages(vk_pipelines *P,uint64_t flip) {
    static const char *setting;
    static bool initialized;
    /* On by default: the two stages hold 4096 sites each, ~660 KiB that a
     * fresh calloc zeroes on every draw (17 us of a 45 us draw). =0 restores it. */
    if(!initialized){setting=getenv("PS5C_REUSE_DRAW_STAGE");if(!setting)setting="1";initialized=true;}
    if(!P->draw_scratch_seen || P->draw_scratch_flip!=flip) {
        bool enabled=false;
        if(setting[0]=='@') {
            FILE *f=fopen(setting+1,"r");int value=0;
            if(f){if(fscanf(f,"%d",&value)==1)enabled=value==1;fclose(f);}
        } else enabled=!strcmp(setting,"1");
        if(!P->draw_scratch_seen || enabled!=P->draw_scratch_enabled)
            LOG_I(GPU,"draw scratch reuse: %s",enabled?"enabled":"disabled");
        P->draw_scratch_seen=true;P->draw_scratch_flip=flip;P->draw_scratch_enabled=enabled;
    }
    return P->draw_scratch_enabled;
}

static void free_dispatch_stage(stage_state *st) {
    if(!st)return;
    sh_eval_free(&st->eval);program_storage_free(st->linked_program,sparse_programs());free(st);
}
static void reset_stage(vk_pipelines *P,stage_state *st) {
    uint32_t *data=st->eval.smem_data,capacity=st->eval.smem_cap;
    if(st->linked_program)vr_flow_invalidate(&P->vertex_flow);
    program_storage_free(st->linked_program,sparse_programs());st->linked_program=NULL;
    st->prog=NULL;
    memset(st->eval.sgpr_init,0,sizeof(st->eval.sgpr_init));
    memset(&st->eval.smem_words,0,sizeof(st->eval)-offsetof(sh_eval,smem_words));
    memset(&st->info,0,sizeof(*st)-offsetof(stage_state,info));
    st->eval.smem_data=data;st->eval.smem_cap=capacity;
}
static stage_state *get_draw_stages(vk_pipelines *P,bool reuse) {
    if(!reuse)return calloc(2,sizeof(stage_state));
    if(!P->draw_stages){P->draw_stages=calloc(2,sizeof(stage_state));return P->draw_stages;}
    for(uint32_t i=0;i<2;i++)reset_stage(P,P->draw_stages+i);
    return P->draw_stages;
}
static stage_state *get_dispatch_stage(vk_pipelines *P,bool reuse) {
    if(!reuse)return calloc(1,sizeof(stage_state));
    if(!P->dispatch_stage){P->dispatch_stage=calloc(1,sizeof(stage_state));return P->dispatch_stage;}
    reset_stage(P,P->dispatch_stage);
    return P->dispatch_stage;
}

static VkShaderModule make_module(renderer *r, const uint32_t *code, size_t words) {
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = words * 4;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vk_check_device(r, vkCreateShaderModule(r->device, &ci, NULL, &m), "vkCreateShaderModule");
    return m;
}

/* The SPIR-V of a stage (malloc'ed), from the shader cache or translated now, and its cache key. */
static bool stage_spirv(const stage_state *st, uint32_t binding_base, uint64_t *key_out, uint32_t **spirv,
                        size_t *count) {
    if(!sh_aperture_allowed(st->prog))return false; /* gate cached SPIR-V too */
    for(uint32_t i=0;i<st->prog->site_count;i++)
        if(st->shapes[i].buf_format==SH_IMG_ATOMIC_UINT64 && !sh_image_atomic64_supported())return false;
    uint32_t *spv = NULL;
    size_t words = 0;
    uint64_t key = vk_shader_cache_key(st->prog, &st->info, st->shapes, binding_base);
    *key_out = key;
    vk_shader_cache_note_source(key, st->prog, &st->info, st->shapes, binding_base);
    /* The corpus's contiguous-code format cannot describe linked ranges. */
    if(st->prog->function_count<=1)
        corpus_add(key, st->prog, &st->info, st->shapes, binding_base, st->prog->base, st->prog->code, st->prog->dwords);
    if (!vk_shader_cache_find(key, &spv, &words)) {
        uint64_t t0 = pal_time_ns();
        if (!sh_translate(st->prog, &st->info, st->shapes, binding_base, &spv, &words)) return false;
        vk_shader_cache_note_translate(pal_time_ns() - t0);
        vk_shader_cache_store(key, spv, words);
    } else if (getenv("PS5C_SHADER_CACHE_CHECK")) { /* debugging aid: a cached translation equals a new one */
        uint32_t *fresh = NULL;
        size_t n = 0;
        if (sh_translate(st->prog, &st->info, st->shapes, binding_base, &fresh, &n) &&
            (n != words || memcmp(fresh, spv, words * 4)))
            LOG_E(GPU, "shader cache: shader 0x%llx stage %u differs from its new translation",
                  (unsigned long long)st->prog->base, (unsigned)st->info.stage);
        free(fresh);
    }
    if (getenv("PS5C_DUMP_SPIRV")) {
        char path[256];
        snprintf(path, sizeof(path), "user/shaders/%010llx_%u.spv", (unsigned long long)st->prog->base, (unsigned)st->info.stage);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(spv, 4, words, f);
            fclose(f);
        }
    }
    *spirv = spv;
    *count = words;
    return true;
}

static pipe_entry *find_pipe(renderer *r, uint64_t key) {
    vk_pipelines *P = r->pipes;
    uint32_t mask = 2 * MAX_PIPES - 1, slot = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 40) & mask;
    for (;; slot = (slot + 1) & mask) {
        uint16_t i = P->pipe_index[slot];
        if (!i) break;
        if (P->pipes[i - 1].key == key) return &P->pipes[i - 1];
    }
    if (P->pipe_count == MAX_PIPES) {
        static bool warned;
        if (!warned) LOG_W(GPU, "pipeline cache full (%u)", MAX_PIPES);
        warned = true;
        return NULL;
    }
    P->pipe_index[slot] = (uint16_t)(P->pipe_count + 1);
    pipe_entry *e = &P->pipes[P->pipe_count++];
    memset(e, 0, sizeof(*e));
    e->key = key;
    return e;
}

/* Hash of a key whose site shapes are used up to the program's site count
 * (the rest is zero and the code hash fixes the count): a few hundred bytes
 * instead of the whole key. */
static uint64_t hash_with_shapes(const void *key, size_t size, const void *shapes, size_t used, size_t shapes_size) {
    const uint8_t *k = (const uint8_t *)key, *sh = (const uint8_t *)shapes;
    uint64_t h = texture_hash_words(k, (size_t)(sh - k), FNV_INIT);
    h = texture_hash_words(sh, used, h);
    return texture_hash_words(sh + shapes_size, size - (size_t)(sh - k) - shapes_size, h);
}

static const VkBlendFactor k_blend_factor[] = {
    VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
    VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
    VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_CONSTANT_COLOR,
    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR,
    VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, VK_BLEND_FACTOR_CONSTANT_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA};

static VkBlendFactor blend_factor(uint32_t f) {
    return f < PS5C_ARRAY_COUNT(k_blend_factor) ? k_blend_factor[f] : VK_BLEND_FACTOR_ONE;
}

static VkBlendOp blend_op(uint32_t c) {
    switch (c) {
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
    }
}

/* DB_STENCIL_CONTROL op codes: KEEP, ZERO, ONES, REPLACE_TEST, REPLACE_OP, ADD_CLAMP, SUB_CLAMP,
 * INVERT, ADD_WRAP, SUB_WRAP, then bitwise ops with STENCILOPVAL. Vulkan replaces with one
 * reference and steps by one: ONES/REPLACE_OP use the reference vk_stencil_face_state picks
 * (or become inversions), the adds assume an op value of 1, the bitwise ops keep. */
static VkStencilOp stencil_op(uint32_t op) {
    switch (op) {
    case 1: return VK_STENCIL_OP_ZERO;
    case 2: case 3: case 4: return VK_STENCIL_OP_REPLACE;
    case 5: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 7: return VK_STENCIL_OP_INVERT;
    case 8: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 9: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static void stencil_face(uint32_t refmask, uint32_t func, uint32_t *ops,
                         vk_stencil_face *face, uint32_t shader_control) {
    static int disabled = -1;
    static unsigned reported;
    if (disabled < 0) disabled = getenv("PS5C_NO_STENCIL_LOGIC") != NULL;
    uint32_t original = *ops;
    /* Exported stencil values vary per pixel, so a fixed mask is not enough.
     * Conservatively exclude all depth/stencil export modes. */
    if (!disabled && !(shader_control & 7u) && vk_stencil_logic_state(refmask, func, ops, face)) {
        if (reported++ < 8)
            LOG_I(GPU, "stencil logic: ops %03x -> %03x, refmask %08x -> reference %02x mask %02x",
                  original, *ops, refmask, face->ref, face->wr);
    } else vk_stencil_face_state(refmask, func, ops, face);
}

typedef struct gfx_key {
    guest_addr vs, ps;
    uint64_t vs_hash, ps_hash;
    sh_site_shape vs_shapes[SH_MAX_SITES], ps_shapes[SH_MAX_SITES];
    uint32_t prim;
    uint32_t ps_vertex_fetch;
    uint32_t ps_wave32;
    uint32_t mesh, mesh_inputs, mesh_vertices, mesh_primitives, mesh_primitive_vertices, mesh_indexed, mesh_threads;
    VkFormat rt_fmt[GPU_MAX_RENDER_TARGETS];
    uint32_t rt_count;
    uint32_t target_mask;
    uint32_t blend[GPU_MAX_RENDER_TARGETS];
    uint32_t ps_cntl[32];
    uint32_t ps_col_format;
    uint32_t ps_mrt_rb_swap;
    uint32_t cull;
    VkFormat depth_fmt;
    uint32_t depth_test, depth_write, depth_func;
    uint32_t depth_bias; /* polygon offset on (its amounts are dynamic state) */
    uint32_t ps_quad_disable;
    /* Stencil test: DB_DEPTH_CONTROL's stencil bits (enable, backface, both funcs; 0: off) and
     * DB_STENCIL_CONTROL's ops. Reference and masks are dynamic state (vk_stencil_face_state). */
    uint32_t stencil_func, stencil_ops;
    uint32_t point_size; /* VS gl_PointSize of point lists (float bits) */
    uint32_t alpha_to_mask; /* DB_ALPHA_TO_MASK ALPHA_TO_MASK_ENABLE: MRT0's alpha gives the coverage */
    uint32_t rop3; /* CB_COLOR_CONTROL ROP3 [23:16] | 0x100 when not copy (0xCC); 0: no logic op */
} gfx_key;

/* Inactive shape capacity is not part of the live key. The compact path
 * hashes the current stage arrays directly; never read its untouched slots. */
static void gfx_key_reset(gfx_key *k, bool compact) {
    if (!compact) {
        memset(k, 0, sizeof(*k));
        return;
    }
    memset(k, 0, offsetof(gfx_key, vs_shapes));
    memset(&k->prim, 0, sizeof(*k) - offsetof(gfx_key, prim));
}

static uint64_t gfx_key_hash(const gfx_key *k, const sh_site_shape *vs, uint32_t nv,
                            const sh_site_shape *ps, uint32_t np, bool compact) {
    uint64_t h = texture_hash_words(k, offsetof(gfx_key, vs_shapes), FNV_INIT);
    h = texture_hash_words(compact ? vs : k->vs_shapes, sizeof(*vs) * nv, h);
    h = texture_hash_words(compact ? ps : k->ps_shapes, sizeof(*ps) * np, h);
    return texture_hash_words(&k->prim, sizeof(*k) - offsetof(gfx_key, prim), h);
}

/* The recipe was zeroed by its caller. Preserve its existing byte layout
 * and zero shape capacity without copying uninitialized live-key capacity. */
static void gfx_recipe_state(gfx_key *dst, const gfx_key *src) {
    memcpy(dst, src, offsetof(gfx_key, vs_shapes));
    memcpy(&dst->prim, &src->prim, sizeof(*src) - offsetof(gfx_key, prim));
    dst->vs = dst->ps = 0;
    dst->vs_hash = dst->ps_hash = 0;
}

/* Dynamic state (r->dyn_state, Vulkan 1.3): these key fields are set per draw with vkCmdSet*
 * instead of being compiled into the pipeline. They are cleared from the key while it is hashed
 * and while its pipeline is created, so draws differing only in them share one pipeline. */
typedef struct gfx_dyn {
    uint32_t cull, depth_test, depth_write, depth_func, depth_bias, stencil_func, stencil_ops, pad;
} gfx_dyn;

static void gfx_dyn_strip(gfx_key *k, gfx_dyn *save) {
    if (!g_dyn_state) return;
    *save = (gfx_dyn){k->cull, k->depth_test, k->depth_write, k->depth_func, k->depth_bias, k->stencil_func,
                      k->stencil_ops, 0};
    if (g_dyn_state & DYN_CULL) k->cull = 0;
    if (g_dyn_state & DYN_DEPTH) k->depth_test = k->depth_write = k->depth_func = 0;
    if (g_dyn_state & DYN_BIAS) k->depth_bias = 0;
    if (g_dyn_state & DYN_STENCIL) k->stencil_func = k->stencil_ops = 0;
}

static void gfx_dyn_restore(gfx_key *k, const gfx_dyn *save) {
    if (!g_dyn_state) return;
    k->cull = save->cull;
    k->depth_test = save->depth_test;
    k->depth_write = save->depth_write;
    k->depth_func = save->depth_func;
    k->depth_bias = save->depth_bias;
    k->stencil_func = save->stencil_func;
    k->stencil_ops = save->stencil_ops;
}

/* The state create_graphics would have compiled from these key fields, set on the command buffer. */
static void set_dyn_state(renderer *r, struct vk_pipelines *P, VkCommandBuffer cmd, const gfx_key *k) {
    gfx_dyn d = {k->cull, k->depth_test, k->depth_write, k->depth_func, k->depth_bias, k->stencil_func,
                 k->stencil_ops, 0};
    /* rectangle lists are expanded to triangles but not culled as triangles on the guest GPU */
    if (!k->mesh && (k->prim == 7 || k->prim == GPU_PRIM_RECTLIST)) d.cull &= ~3u;
    { /* PS5C_CULL_DEBUG=none|flip and PS5C_DEPTH_SLACK<0: as in create_graphics */
        static int cull_debug = -1, slack = -2;
        if (cull_debug < 0) {
            const char *cd = getenv("PS5C_CULL_DEBUG");
            cull_debug = cd && !strcmp(cd, "none") ? 1 : cd && !strcmp(cd, "flip") ? 2 : 0;
        }
        if (slack == -2) slack = getenv("PS5C_DEPTH_SLACK") ? atoi(getenv("PS5C_DEPTH_SLACK")) : 0;
        if (cull_debug == 1) d.cull &= ~3u;
        if (cull_debug == 2) d.cull ^= 4u;
        uint32_t f = d.depth_func;
        if (slack < 0 && d.depth_test && k->rt_count && (f == 2 || f == 3 || f == 6)) d.depth_func = VK_COMPARE_OP_ALWAYS;
    }
    if (!(g_dyn_state & DYN_CULL)) d.cull = 0;
    if (!(g_dyn_state & DYN_DEPTH)) d.depth_test = d.depth_write = d.depth_func = 0;
    if (!(g_dyn_state & DYN_BIAS)) d.depth_bias = 0;
    if (!(g_dyn_state & DYN_STENCIL)) d.stencil_func = d.stencil_ops = 0;
    if (P->cur_dyn_valid && !memcmp(P->cur_dyn, &d, sizeof(d))) return;
    gfx_dyn was = {0};
    if (P->cur_dyn_valid) memcpy(&was, P->cur_dyn, sizeof(was));
    bool all = !P->cur_dyn_valid;
    if ((g_dyn_state & DYN_CULL) && (all || was.cull != d.cull)) {
        r->set_cull_mode(cmd, (VkCullModeFlags)(d.cull & 3u));
        r->set_front_face(cmd, (d.cull & 4u) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    }
    if ((g_dyn_state & DYN_DEPTH) && (all || was.depth_test != d.depth_test)) r->set_depth_test(cmd, d.depth_test ? VK_TRUE : VK_FALSE);
    if ((g_dyn_state & DYN_DEPTH) && (all || was.depth_write != d.depth_write)) r->set_depth_write(cmd, d.depth_write ? VK_TRUE : VK_FALSE);
    if ((g_dyn_state & DYN_DEPTH) && (all || was.depth_func != d.depth_func))
        r->set_depth_compare(cmd, (VkCompareOp)d.depth_func); /* ZFUNC has Vulkan's order */
    if ((g_dyn_state & DYN_BIAS) && (all || was.depth_bias != d.depth_bias))
        r->set_depth_bias_enable(cmd, d.depth_bias ? VK_TRUE : VK_FALSE);
    if (!(g_dyn_state & DYN_STENCIL) || (!all && was.stencil_func == d.stencil_func && was.stencil_ops == d.stencil_ops)) {
        memcpy(P->cur_dyn, &d, sizeof(d));
        P->cur_dyn_valid = true;
        return;
    }
    r->set_stencil_test(cmd, d.stencil_func ? VK_TRUE : VK_FALSE);
    uint32_t ops = d.stencil_ops;
    VkCompareOp front = (VkCompareOp)((d.stencil_func >> 8) & 7); /* STENCILFUNC has Vulkan's order */
    if (!d.stencil_func) {
        r->set_stencil_op(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                          VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
    } else if ((d.stencil_func >> 7) & 1) {
        r->set_stencil_op(cmd, VK_STENCIL_FACE_FRONT_BIT, stencil_op(ops & 15), stencil_op((ops >> 4) & 15),
                          stencil_op((ops >> 8) & 15), front);
        r->set_stencil_op(cmd, VK_STENCIL_FACE_BACK_BIT, stencil_op((ops >> 12) & 15), stencil_op((ops >> 16) & 15),
                          stencil_op((ops >> 20) & 15), (VkCompareOp)((d.stencil_func >> 20) & 7));
    } else {
        r->set_stencil_op(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_op(ops & 15), stencil_op((ops >> 4) & 15),
                          stencil_op((ops >> 8) & 15), front);
    }
    memcpy(P->cur_dyn, &d, sizeof(d));
    P->cur_dyn_valid = true;
}

/* CPU regression: compare live hashes and serialized recipe state against
 * the original full-zero/full-copy path, including maximum site counts and
 * poisoned unused capacity. No game bytes or device are needed. */
bool vk_pipeline_test_draw_keys(void) {
    gfx_key *keys = malloc(5 * sizeof(*keys));
    sh_site_shape *shapes = malloc(2 * SH_MAX_SITES * sizeof(*shapes));
    if (!keys || !shapes) { free(keys); free(shapes); return false; }
    uint32_t random = 0x931fa427u;
    static const uint32_t counts[] = {0, 1, 7, 31, SH_MAX_SITES - 1, SH_MAX_SITES};
    bool correct = true;
    for (uint32_t round = 0; round < 128; round++) {
        for (size_t i = 0; i < sizeof(*keys); i++) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            ((uint8_t *)&keys[0])[i] = (uint8_t)random;
        }
        memcpy(shapes, keys[0].vs_shapes, sizeof(keys[0].vs_shapes));
        memcpy(shapes + SH_MAX_SITES, keys[0].ps_shapes, sizeof(keys[0].ps_shapes));
        uint32_t nv = counts[round % PS5C_ARRAY_COUNT(counts)];
        uint32_t np = counts[(round / PS5C_ARRAY_COUNT(counts)) % PS5C_ARRAY_COUNT(counts)];
        gfx_key_reset(&keys[1], false);
        memset(&keys[2], 0xA5, sizeof(*keys)); gfx_key_reset(&keys[2], true);
        for (uint32_t k = 1; k <= 2; k++) {
            memcpy(&keys[k], &keys[0], offsetof(gfx_key, vs_shapes));
            memcpy(&keys[k].prim, &keys[0].prim, sizeof(*keys) - offsetof(gfx_key, prim));
        }
        memcpy(keys[1].vs_shapes, shapes, nv * sizeof(*shapes));
        memcpy(keys[1].ps_shapes, shapes + SH_MAX_SITES, np * sizeof(*shapes));
        correct &= gfx_key_hash(&keys[1], shapes, nv, shapes + SH_MAX_SITES, np, false) ==
                   gfx_key_hash(&keys[2], shapes, nv, shapes + SH_MAX_SITES, np, true);
        keys[3] = keys[1]; keys[3].vs = keys[3].ps = keys[3].vs_hash = keys[3].ps_hash = 0;
        memset(keys[3].vs_shapes, 0, sizeof(keys[3].vs_shapes));
        memset(keys[3].ps_shapes, 0, sizeof(keys[3].ps_shapes));
        memset(&keys[4], 0, sizeof(*keys)); gfx_recipe_state(&keys[4], &keys[2]);
        correct &= !memcmp(&keys[3], &keys[4], sizeof(*keys));
    }
    free(keys); free(shapes); return correct;
}

/* A pipeline's recipe: everything its build depends on, without guest
 * addresses (two draws whose shaders sit at other addresses but translate the
 * same share one pipeline, also across sessions: vk_pipeline_async.c). */
#define RECIPE_VERSION 18
/* graphics recipe built with the dynamic-state groups g_dyn_state: RECIPE_VERSION + 1000 * groups */
#define RECIPE_VERSION_DYN (RECIPE_VERSION + 1000 * g_dyn_state)
#define RECIPE_BINDINGS (2 * (SH_MAX_SITES + 1))
typedef struct pipe_recipe {
    uint32_t version, compute, mesh;
    uint64_t spv[2];                 /* shader cache keys: VS and PS, or the CS */
    uint32_t rect, gs_params;        /* rectangle lists: geometry shader forwarding these parameters */
    uint32_t nbind;
    uint8_t btype[RECIPE_BINDINGS];  /* VkDescriptorType of each binding, in stage order */
    uint8_t bstage[RECIPE_BINDINGS]; /* 0 vertex, 1 fragment, 2 compute */
    uint16_t aliases[RECIPE_BINDINGS]; /* original binding -> compatible canonical binding */
    gfx_key k; /* graphics state; shader addresses, code hashes and site shapes cleared (spv[] has them) */
} pipe_recipe;

size_t vk_pipeline_recipe_size(void) { return sizeof(pipe_recipe); }

/* Recipes of the other dynamic-state mode are skipped: their keys are hashed differently. */
static bool recipe_version_ok(const pipe_recipe *rc) {
    if (rc->compute) return rc->version == RECIPE_VERSION;
    return rc->version == RECIPE_VERSION_DYN; /* RECIPE_VERSION when g_dyn_state is 0 */
}

bool vk_pipeline_prewarm_size(const void *recipe, uint64_t *bytes) {
    const pipe_recipe *rc = recipe;
    if (!recipe_version_ok(rc) || rc->compute > 1 || rc->nbind > RECIPE_BINDINGS) return false;
    uint64_t size = 0;
    for (uint32_t i = 0; i < (rc->compute ? 1u : 2u); i++) {
        uint64_t stage = vk_shader_cache_size(rc->spv[i]);
        if (!stage) return false;
        size += stage;
    }
    *bytes = size;
    return true;
}

/* Bindings of a stage: its uniforms, then one per BUFFER/IMAGE site (at most SH_MAX_SITES + 1) */
uint32_t vk_pipeline_binding_types(const sh_program *p, const sh_site_shape *shapes, uint8_t *types) {
    uint32_t n = 0;
    types[n++] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    for (uint32_t i = 0; i < p->site_count; i++) {
        if (p->sites[i].kind == SH_SITE_SMEM && !shapes[i].address_map) continue;
        types[n++] = (uint8_t)(p->sites[i].kind != SH_SITE_IMAGE ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                               : sh_site_storage(&p->sites[i],&shapes[i]) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                                          : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    }
    return n;
}

static void recipe_bindings(pipe_recipe *rc, const sh_program *p, const sh_site_shape *shapes, uint8_t stage) {
    uint32_t n = vk_pipeline_binding_types(p, shapes, rc->btype + rc->nbind);
    memset(rc->bstage + rc->nbind, stage, n);
    rc->nbind += n;
}

/* After a translator change: a recipe of the earlier translator with its
 * shaders' new keys, if every stage was translated again with the same
 * bindings (else a pipeline layout could disagree with its SPIR-V). Mesh
 * replay recipes (mesh 2) key a generated stage and are not carried over. */
bool vk_pipeline_recipe_migrate(void *recipe) {
    pipe_recipe *rc = (pipe_recipe *)recipe;
    if (!recipe_version_ok(rc) || rc->compute > 1 || rc->nbind > RECIPE_BINDINGS || rc->mesh == 2) return false;
    uint32_t at = 0;
    uint64_t keys[2] = {0, 0};
    for (uint32_t i = 0; i < (rc->compute ? 1u : 2u); i++) {
        const uint8_t *types = NULL;
        uint32_t n = 0;
        uint8_t stage = rc->compute ? 2 : (uint8_t)i;
        if (!vk_shader_cache_migrated(rc->spv[i], &keys[i], &types, &n) || n > rc->nbind - at ||
            memcmp(rc->btype + at, types, n))
            return false;
        for (uint32_t j = 0; j < n; j++)
            if (rc->bstage[at + j] != stage) return false;
        at += n;
    }
    if (at != rc->nbind) return false;
    rc->spv[0] = keys[0];
    rc->spv[1] = keys[1];
    return true;
}

/* Disk recipes can travel between devices. Guard prewarming as well as fresh
 * translations before handing extension-dependent SPIR-V to the driver. */
static bool spirv_features_allowed(const uint32_t *code,size_t words,bool physical_addresses,bool image64) {
    if(!code || words<5)return false;
    for(size_t at=5;at<words;) {
        uint32_t count=code[at]>>16,op=code[at]&65535;
        if(!count || count>words-at)return false;
        if(op==SpvOpCapability && count==2 &&
           ((code[at+1]==5347 && !physical_addresses) || (code[at+1]==5016 && !image64)))return false;
        at+=count;
    }
    return true;
}

bool vk_pipeline_build(renderer *r, const void *recipe, const uint32_t *const words[2], const size_t n[2],
                       vk_built_pipeline *out) {
    const pipe_recipe *rc = (const pipe_recipe *)recipe;
    memset(out, 0, sizeof(*out));
    if (!recipe_version_ok(rc) || rc->compute > 1 || rc->nbind > RECIPE_BINDINGS ||
        rc->nbind < (rc->compute ? 1u : 2u) + (rc->mesh == 2 ? 1u : 0u)) return false;
    vk_descriptor_plan plan;
    plan.count = rc->nbind;
    plan.compact = false;
    memcpy(plan.binding, rc->aliases, rc->nbind * sizeof(*rc->aliases));
    if (!vk_descriptor_plan_valid(&plan)) return false;
    uint8_t types[RECIPE_BINDINGS], visibility[RECIPE_BINDINGS];
    uint32_t unique = 0;
    for (uint32_t i = 0; i < rc->nbind; i++) {
        uint32_t root = plan.binding[i];
        if (rc->bstage[i] >= 3 || (rc->btype[i] != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
            rc->btype[i] != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && rc->btype[i] != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)) return false;
        if (rc->btype[i] != rc->btype[root] || rc->bstage[i] != rc->bstage[root]) return false;
        if (root == i) { types[unique] = rc->btype[i]; visibility[unique++] = rc->bstage[i]; }
    }
    if(rc->mesh==1 && (!r->mesh_shader || rc->compute || rc->rect))return false;
    /* Samplers and storage images: Intel's driver takes more per stage than the 64 and 16 it
     * reports (Sonic Frontiers' screen-space passes use ~70 samplers, a Dragon Ball Sparking
     * Zero pass ~20 storage images; both ran before this check). The sampled image and total
     * resource limits stay. PS5C_STRICT_DESCRIPTOR_LIMITS=1 enforces the reported ones. */
    static int strict = -1;
    if (strict < 0) strict = getenv("PS5C_STRICT_DESCRIPTOR_LIMITS") != NULL;
    VkPhysicalDeviceLimits lim = r->props.limits;
    if (!strict) {
        lim.maxPerStageDescriptorSamplers = lim.maxPerStageDescriptorSampledImages;
        lim.maxDescriptorSetSamplers = lim.maxDescriptorSetSampledImages;
        lim.maxPerStageDescriptorStorageImages = lim.maxPerStageDescriptorSampledImages;
        lim.maxDescriptorSetStorageImages = lim.maxDescriptorSetSampledImages;
    }
    const char *limit = vk_pipeline_descriptor_error(&lim, types, visibility, unique, rc->compute ? 0 : rc->k.rt_count);
    if(!limit && rc->mesh==2 && rc->nbind) {
        uint8_t stage_visibility[RECIPE_BINDINGS];memcpy(stage_visibility,visibility,unique);stage_visibility[unique-1]=2;
        limit=vk_pipeline_descriptor_error(&lim,types,stage_visibility,unique,rc->k.rt_count);
    }
    if (!limit && !strict) {
        const char *reported = vk_pipeline_descriptor_error(&r->props.limits, types, visibility, unique,
                                                            rc->compute ? 0 : rc->k.rt_count);
        static uint32_t warned;
        if (reported && warned++ < 4)
            LOG_W(GPU, "pipeline layout (%u descriptors) exceeds the reported %s on %s: built anyway", unique, reported,
                  r->props.deviceName);
    }
    /* PS5C_NO_DESCRIPTOR_LIMITS=1: try the layout anyway (experiment: does the driver take more
     * storage buffers than it reports? No Man's Sky's loading compute needs ~500-600) */
    static int no_limits = -1;
    if (no_limits < 0) no_limits = getenv("PS5C_NO_DESCRIPTOR_LIMITS") != NULL;
    if (limit && no_limits) {
        static uint32_t tried;
        if (tried++ < 4) LOG_W(GPU, "pipeline layout (%u descriptors) exceeds %s on %s: tried anyway", unique, limit, r->props.deviceName);
        limit = NULL;
    }
    bool update_after_bind = false;
    if (limit && r->descriptor_uab &&
        !vk_pipeline_descriptor_error(&r->uab_limits, types, visibility, unique, rc->compute ? 0 : rc->k.rt_count)) {
        static uint32_t told;
        if (told++ < 4)
            LOG_I(GPU, "pipeline layout (%u descriptors) exceeds %s: built with update-after-bind limits", unique, limit);
        limit = NULL;
        update_after_bind = true;
    }
    if (limit) {
        LOG_E(GPU, "pipeline layout (%u descriptors) exceeds %s on %s", unique, limit, r->props.deviceName);
        return false;
    }
    uint32_t stages = rc->compute ? 1 : 2, *own[2] = {NULL, NULL};
    const uint32_t *code[2] = {NULL, NULL};
    size_t cn[2] = {0, 0};
    for (uint32_t i = 0; i < stages; i++) {
        if (words && words[i]) {
            code[i] = words[i];
            cn[i] = n[i];
        } else if (vk_shader_cache_lookup(rc->spv[i], &own[i], &cn[i])) {
            code[i] = own[i];
        }
    }
    bool ok = code[0] && (rc->compute || code[1]);
    uint32_t *remapped[2] = {NULL, NULL};
    if (unique != rc->nbind) for (uint32_t i = 0; ok && i < stages; i++) {
        size_t size = cn[i];
        ok = vk_spirv_bindings(code[i], cn[i], &plan, &remapped[i], &size);
        if (ok && remapped[i]) { code[i] = remapped[i]; cn[i] = size; }
    }
    for(uint32_t i=0;ok && i<stages;i++)
        ok=spirv_features_allowed(code[i],cn[i],r->bvh_address,sh_image_atomic64_supported());
    if (ok && !rc->compute) {
        uint32_t outputs = 0, inputs = 0;
        ok = vk_spirv_locations(code[0], cn[0], SpvStorageClassOutput, &outputs) &&
             vk_spirv_locations(code[1], cn[1], SpvStorageClassInput, &inputs);
        if (rc->rect) outputs &= rc->gs_params;
        if (!ok || (inputs & ~outputs)) {
            LOG_W(GPU, "pipeline interface rejected: VS %016llx PS %016llx, missing locations %08x",
                  (unsigned long long)rc->spv[0], (unsigned long long)rc->spv[1], inputs & ~outputs);
            ok = false;
        }
    }
    static const VkShaderStageFlags k_flags[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
                                                  VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutBinding bind[RECIPE_BINDINGS];
    uint32_t nbind = 0;
    for (uint32_t i = 0; ok && i < rc->nbind; i++) {
        if (plan.binding[i] != i) continue;
        VkDescriptorSetLayoutBinding *b = &bind[nbind++];
        *b = (VkDescriptorSetLayoutBinding){.binding=i, .descriptorType=(VkDescriptorType)rc->btype[i],
            .descriptorCount=1, .stageFlags=k_flags[rc->bstage[i] < 3 ? rc->bstage[i] : 0]};
        if(rc->mesh==1 && rc->bstage[i]==0)b->stageFlags=(VkShaderStageFlags)0x80;
        if(rc->mesh==2 && i==rc->nbind-1)b->stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    if (update_after_bind) dci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    out->update_after_bind = update_after_bind;
    out->push = !update_after_bind && nbind && nbind <= r->push_max;
    if (out->push) dci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dci.bindingCount = nbind;
    dci.pBindings = bind;
    ok = ok && vkCreateDescriptorSetLayout(r->device, &dci, NULL, &out->dsl) == VK_SUCCESS;
    VkPipelineLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &out->dsl;
    ok = ok && vkCreatePipelineLayout(r->device, &lci, NULL, &out->layout) == VK_SUCCESS;
    VkShaderModule vm = ok ? make_module(r, code[0], cn[0]) : VK_NULL_HANDLE;
    VkShaderModule fm = ok && !rc->compute ? make_module(r, code[1], cn[1]) : VK_NULL_HANDLE;
    VkShaderModule gm = VK_NULL_HANDLE;
    if (ok && rc->rect) {
        uint32_t *spv = NULL;
        size_t gs_words = 0;
        if (sh_rectlist_gs(rc->gs_params, &spv, &gs_words)) gm = make_module(r, spv, gs_words);
        free(spv);
    }
    ok = ok && vm && (rc->compute || fm) && (!rc->rect || gm);
    uint64_t t0 = pal_time_ns();
    if (ok && rc->compute) {
        VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = vm;
        ci.stage.pName = "main";
        ci.layout = out->layout;
        ci.flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT; /* large dispatches go in pieces (do_dispatch) */
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT sg = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT, .requiredSubgroupSize = r->cs_subgroup};
        if (r->cs_subgroup) ci.stage.pNext = &sg; /* the guest's wave width (create_device) */
        vk_shader_cache_use_begin();
        ok = vk_check_device(r, vkCreateComputePipelines(r->device, vk_shader_cache_pipelines(), 1, &ci, NULL, &out->pipe), "vkCreateComputePipelines") == VK_SUCCESS;
        vk_shader_cache_use_end();
    } else if (ok) {
        const gfx_key *k = &rc->k;
        VkRenderPass rp = r->dyn_render ? VK_NULL_HANDLE : make_render_pass(r->device, k->rt_fmt, k->rt_count, k->depth_fmt, false);
        VkPipelineRenderingCreateInfo rendering = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = k->rt_count;
        rendering.pColorAttachmentFormats = k->rt_fmt;
        if (k->depth_fmt != VK_FORMAT_UNDEFINED) {
            if (k->depth_fmt != VK_FORMAT_S8_UINT) rendering.depthAttachmentFormat = k->depth_fmt;
            if (vk_format_has_stencil(k->depth_fmt)) rendering.stencilAttachmentFormat = k->depth_fmt;
        }
        VkPipelineShaderStageCreateInfo st[3];
        uint32_t ns = 0;
        VkShaderModule mods[3] = {vm, gm, fm};
        VkShaderStageFlagBits bits[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
                                         VK_SHADER_STAGE_FRAGMENT_BIT};
        if(rc->mesh==1)bits[0]=(VkShaderStageFlagBits)0x80;
        for (int i = 0; i < 3; i++) {
            if (!mods[i]) continue;
            memset(&st[ns], 0, sizeof(st[ns]));
            st[ns].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            st[ns].stage = bits[i];
            st[ns].module = mods[i];
            st[ns].pName = "main";
            ns++;
        }
        VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        switch (k->prim) {
        case GPU_PRIM_POINTLIST: ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case GPU_PRIM_LINELIST: ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case GPU_PRIM_LINESTRIP: ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
        case GPU_PRIM_TRIFAN: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
        case GPU_PRIM_TRISTRIP: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
        default: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        }
        /* strips and fans restart at index 0xFFFF / 0xFFFFFFFF (games enable the reset index for them) */
        ia.primitiveRestartEnable = k->prim == GPU_PRIM_LINESTRIP || k->prim == GPU_PRIM_TRIFAN || k->prim == GPU_PRIM_TRISTRIP;
        if(rc->mesh==2){ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;ia.primitiveRestartEnable=VK_FALSE;}
        VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        /* PA_SU_SC_MODE_CNTL: front/back cull in bits 0/1, FACE in bit 2.
         * Rectangle lists are expanded to triangles here but are not culled
         * as triangles on the guest GPU. Keep their previous coverage. */
        rs.cullMode = rc->rect ? VK_CULL_MODE_NONE : (VkCullModeFlags)(k->cull & 3u);
        rs.frontFace = (k->cull & 4u) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        { /* PS5C_CULL_DEBUG=none|flip: no culling, or the other orientation (debugging aid) */
            const char *cd = getenv("PS5C_CULL_DEBUG");
            if (cd && !strcmp(cd, "none")) rs.cullMode = VK_CULL_MODE_NONE;
            if (cd && !strcmp(cd, "flip"))
                rs.frontFace = rs.frontFace == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        }
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        /* one sample: alpha decides whether the pixel is written at all (foliage, hair, fences) */
        ms.alphaToCoverageEnable = k->alpha_to_mask ? VK_TRUE : VK_FALSE;
        VkPipelineColorBlendAttachmentState cb[GPU_MAX_RENDER_TARGETS];
        for (uint32_t i = 0; i < k->rt_count; i++) {
            uint32_t b = k->blend[i];
            memset(&cb[i], 0, sizeof(cb[i]));
            cb[i].colorWriteMask = (k->target_mask >> (4 * i)) & 0xF;
            if ((k->ps_mrt_rb_swap >> i) & 1u) {
                uint32_t mask = cb[i].colorWriteMask;
                cb[i].colorWriteMask = (mask & 0xAu) | ((mask & 1u) << 2) | ((mask & 4u) >> 2);
            }
            cb[i].blendEnable = (b >> 30) & 1;
            cb[i].srcColorBlendFactor = blend_factor(b & 0x1F);
            cb[i].colorBlendOp = blend_op((b >> 5) & 7);
            cb[i].dstColorBlendFactor = blend_factor((b >> 8) & 0x1F);
            bool separate = (b >> 29) & 1;
            cb[i].srcAlphaBlendFactor = separate ? blend_factor((b >> 16) & 0x1F) : cb[i].srcColorBlendFactor;
            cb[i].alphaBlendOp = separate ? blend_op((b >> 21) & 7) : cb[i].colorBlendOp;
            cb[i].dstAlphaBlendFactor = separate ? blend_factor((b >> 24) & 0x1F) : cb[i].dstColorBlendFactor;
        }
        VkPipelineDepthStencilStateCreateInfo dss = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        dss.depthTestEnable = k->depth_test;
        dss.depthWriteEnable = k->depth_write;
        dss.depthCompareOp = (VkCompareOp)k->depth_func; /* ZFUNC has Vulkan's order */
        { /* PS5C_DEPTH_SLACK=<units>: debugging aid, color draws testing EQUAL/GEQUAL/LEQUAL against a
           * prepass get that much depth bias in their favor (how far apart the two passes' depths are) */
            static int slack = -2;
            if (slack == -2) slack = getenv("PS5C_DEPTH_SLACK") ? atoi(getenv("PS5C_DEPTH_SLACK")) : 0;
            uint32_t f = k->depth_func;
            if (slack < 0 && k->depth_test && k->rt_count && (f == 2 || f == 3 || f == 6)) {
                dss.depthCompareOp = VK_COMPARE_OP_ALWAYS; /* -1: what these draws would write */
            }
        }
        rs.depthBiasEnable = k->depth_bias != 0;
        dss.maxDepthBounds = 1.0f;
        if (k->stencil_func) {
            bool bf = (k->stencil_func >> 7) & 1;
            uint32_t ops = k->stencil_ops;
            dss.stencilTestEnable = VK_TRUE;
            dss.front.failOp = stencil_op(ops & 15);
            dss.front.passOp = stencil_op((ops >> 4) & 15);
            dss.front.depthFailOp = stencil_op((ops >> 8) & 15);
            dss.front.compareOp = (VkCompareOp)((k->stencil_func >> 8) & 7); /* STENCILFUNC has Vulkan's order */
            dss.back = dss.front;
            if (bf) {
                dss.back.failOp = stencil_op((ops >> 12) & 15);
                dss.back.passOp = stencil_op((ops >> 16) & 15);
                dss.back.depthFailOp = stencil_op((ops >> 20) & 15);
                dss.back.compareOp = (VkCompareOp)((k->stencil_func >> 20) & 7);
            }
        }
        VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cbs.attachmentCount = k->rt_count;
        cbs.pAttachments = cb;
        if (k->rop3) { /* ROP3 truth tables over source 0xCC and destination 0xAA; the 16 that are
                        * two-operand functions. Vulkan applies it to integer and normalized targets
                        * (float and sRGB ones are written as they come) and turns blending off. */
            static const struct { uint8_t rop; VkLogicOp op; } k_rop[16] = {
                {0x00, VK_LOGIC_OP_CLEAR},  {0x88, VK_LOGIC_OP_AND},   {0x44, VK_LOGIC_OP_AND_REVERSE},
                {0xCC, VK_LOGIC_OP_COPY},   {0x22, VK_LOGIC_OP_AND_INVERTED}, {0xAA, VK_LOGIC_OP_NO_OP},
                {0x66, VK_LOGIC_OP_XOR},    {0xEE, VK_LOGIC_OP_OR},    {0x11, VK_LOGIC_OP_NOR},
                {0x99, VK_LOGIC_OP_EQUIVALENT}, {0x55, VK_LOGIC_OP_INVERT}, {0xDD, VK_LOGIC_OP_OR_REVERSE},
                {0x33, VK_LOGIC_OP_COPY_INVERTED}, {0xBB, VK_LOGIC_OP_OR_INVERTED}, {0x77, VK_LOGIC_OP_NAND},
                {0xFF, VK_LOGIC_OP_SET}};
            for (uint32_t i = 0; i < 16; i++)
                if (k_rop[i].rop == (k->rop3 & 0xFF)) cbs.logicOpEnable = VK_TRUE, cbs.logicOp = k_rop[i].op;
        }
        VkDynamicState dyn[15] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
                                  VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                  VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                  VK_DYNAMIC_STATE_STENCIL_REFERENCE}; /* + the dynamic-state groups below */
        VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        uint32_t dm = rc->version > RECIPE_VERSION ? (rc->version - RECIPE_VERSION) / 1000 : 0;
        uint32_t nd = 4;
        if ((dm & DYN_STENCIL) || k->stencil_func) nd = 7; /* compare/write masks and reference */
        if (dm & DYN_CULL) dyn[nd++] = VK_DYNAMIC_STATE_CULL_MODE, dyn[nd++] = VK_DYNAMIC_STATE_FRONT_FACE;
        if (dm & DYN_DEPTH) {
            dyn[nd++] = VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE;
            dyn[nd++] = VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE;
            dyn[nd++] = VK_DYNAMIC_STATE_DEPTH_COMPARE_OP;
        }
        if (dm & DYN_BIAS) dyn[nd++] = VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE;
        if (dm & DYN_STENCIL) dyn[nd++] = VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, dyn[nd++] = VK_DYNAMIC_STATE_STENCIL_OP;
        ds.dynamicStateCount = nd;
        ds.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.stageCount = ns;
        ci.pStages = st;
        ci.pVertexInputState = rc->mesh==1 ? NULL : &vi;
        ci.pInputAssemblyState = rc->mesh==1 ? NULL : &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pColorBlendState = &cbs;
        if (k->depth_fmt != VK_FORMAT_UNDEFINED) ci.pDepthStencilState = &dss;
        ci.pDynamicState = &ds;
        ci.layout = out->layout;
        ci.renderPass = rp;
        if (r->dyn_render) ci.pNext = &rendering;
        vk_shader_cache_use_begin();
        ok = (rp || r->dyn_render) && vk_check_device(r, vkCreateGraphicsPipelines(r->device, vk_shader_cache_pipelines(), 1, &ci, NULL, &out->pipe), "vkCreateGraphicsPipelines") == VK_SUCCESS;
        vk_shader_cache_use_end();
        if (rp) vkDestroyRenderPass(r->device, rp, NULL);
    }
    if (out->pipe) vk_shader_cache_note_pipeline(pal_time_ns() - t0);
    if (vm) vkDestroyShaderModule(r->device, vm, NULL);
    if (fm) vkDestroyShaderModule(r->device, fm, NULL);
    if (gm) vkDestroyShaderModule(r->device, gm, NULL);
    free(own[0]);
    free(own[1]);
    free(remapped[0]);
    free(remapped[1]);
    if (!ok) {
        if (out->pipe) vkDestroyPipeline(r->device, out->pipe, NULL);
        if (out->layout) vkDestroyPipelineLayout(r->device, out->layout, NULL);
        if (out->dsl) vkDestroyDescriptorSetLayout(r->device, out->dsl, NULL);
        memset(out, 0, sizeof(*out));
    }
    return ok;
}

/* The pipeline of a recipe for a draw or dispatch: shared by content
 * (vk_pipeline_async.c), or built here as before with PS5C_PIPELINES_ASYNC=0. */
static bool pipeline_for(renderer *r, pipe_entry *e, const pipe_recipe *rc, uint32_t *const w[2], const size_t n[2]) {
    const uint32_t *const words[2] = {w[0], w[1]};
    vk_built_pipeline b;
    bool ok;
    if (vk_pipeline_async_on() && rc->mesh!=2) {
        bool shared = false, pending = false;
        /* Compute side effects may be observed only once (culling, counters,
         * indirect arguments). Returning "pending" here would lose the work. */
        ok = vk_pipeline_async_get(r, rc, words, n, &b, &shared, &pending, rc->compute != 0);
        e->shared = ok && shared;
        e->pending = pending;
    } else {
        uint64_t t0 = pal_time_ns();
        ok = vk_pipeline_build(r, rc, words, n, &b);
        vk_pipeline_async_note_wait(pal_time_ns() - t0, true);
    }
    if (ok) {
        e->pipe = b.pipe;
        e->layout = b.layout;
        e->dsl = b.dsl;
        e->update_after_bind = b.update_after_bind;
        e->push = b.push;
    }
    return ok;
}

static bool compact_descriptors(void) {
    static int on = -1; /* read once: called several times a draw */
    if (on < 0) { const char *value = getenv("PS5C_DESCRIPTOR_COMPACT"); on = value && !strcmp(value, "1"); }
    return on; /* opt-in until image/game regression gates pass */
}

/* Equal images share a binding when PS5C_DESCRIPTOR_COMPACT=1, and always in a stage
 * with more image sites than the device's per-stage sampler limit: Alone in the Dark
 * (2024) samples one image through 175 sites in a compute pass, and Intel's driver
 * crashes recording a dispatch with that many separate samplers. */
static bool compact_stage(const renderer *r, const sh_program *p) {
    if (compact_descriptors()) return true;
    uint32_t images = 0;
    for (uint32_t s = 0; s < p->site_count; s++) images += p->sites[s].kind == SH_SITE_IMAGE;
    return images > r->props.limits.maxPerStageDescriptorSamplers;
}

static uint64_t descriptor_plan_key(uint64_t key, const vk_descriptor_plan *plan) {
    if (vk_descriptor_plan_unique(plan) == plan->count) return key;
    return texture_hash_words(plan->binding, plan->count * sizeof(*plan->binding), key ^ 0x44455343504c414eull);
}

static bool recipe_plan(pipe_recipe *rc, const vk_descriptor_plan *plan) {
    if (rc->nbind != plan->count || !vk_descriptor_plan_valid(plan)) return false;
    memcpy(rc->aliases, plan->binding, plan->count * sizeof(*plan->binding));
    return true;
}

static bool create_graphics(renderer *r, pipe_entry *e, const gfx_key *k, stage_state *vs, stage_state *ps,
                             const vk_descriptor_plan *plan) {
    if (getenv("PS5C_LOG_PS_EXPORTS")) {
        for (uint32_t i = 0; i < k->rt_count; i++) {
            if (!color_output_kind(k->rt_fmt[i])) continue;
            LOG_I(GPU, "integer RT%u fmt %d: PS 0x%llx hash %016llx col_format %08x", i, k->rt_fmt[i],
                  (unsigned long long)ps->prog->base, (unsigned long long)ps->prog->hash, ps->info.ps_col_format);
            for (uint32_t j = 0; j < ps->prog->count; j++) {
                const rdna2_inst *in = &ps->prog->insts[j];
                if (in->enc == RDNA2_ENC_EXP)
                    LOG_I(GPU, "  export target %u compressed %u en %x v%u v%u v%u v%u", in->exp_target,
                          in->exp_compr, in->exp_en, in->exp_src[0], in->exp_src[1], in->exp_src[2], in->exp_src[3]);
            }
        }
    }
    uint32_t *w[2] = {NULL, NULL};
    size_t n[2] = {0, 0};
    pipe_recipe rc;
    memset(&rc, 0, sizeof(rc));
    /* PS5C_LOG_PIPELINE_START: name each pipeline before the driver compiles it (one that never comes back) */
    uint64_t t0 = pal_time_ns();
    if (getenv("PS5C_LOG_PIPELINE_START"))
        LOG_I(GPU, "pipeline start: VS 0x%llx PS 0x%llx", (unsigned long long)k->vs, (unsigned long long)k->ps);
    uint32_t *geometry=NULL;size_t geometry_words=0;uint64_t geometry_key=0;
    bool ok;
    if(k->mesh==2) {
        ok=stage_spirv(vs,0,&geometry_key,&geometry,&geometry_words) &&
            sh_mesh_replay(vs->prog,&vs->info,&w[0],&n[0]) &&
            stage_spirv(ps,sh_binding_count_shapes(vs->prog,vs->shapes),&rc.spv[1],&w[1],&n[1]);
        if(ok)rc.spv[0]=texture_hash_words(w[0],n[0]*4,FNV_INIT);
        if(ok && getenv("PS5C_DUMP_SPIRV")) {
            char path[256];snprintf(path,sizeof(path),"user/shaders/%010llx_replay.spv",(unsigned long long)vs->prog->base);
            FILE*f=fopen(path,"wb");if(f){fwrite(w[0],4,n[0],f);fclose(f);}
        }
    } else ok = stage_spirv(vs, 0, &rc.spv[0], &w[0], &n[0]) &&
              stage_spirv(ps, sh_binding_count_shapes(vs->prog, vs->shapes), &rc.spv[1], &w[1], &n[1]);
    if (ok) {
        rc.version = RECIPE_VERSION_DYN;
        rc.mesh = k->mesh;
        rc.rect = !rc.mesh && (k->prim == 7 || k->prim == GPU_PRIM_RECTLIST);
        rc.gs_params = rc.rect ? vs->prog->param_mask : 0;
        if (rc.rect) for (uint32_t bits = vs->info.vs_param_alias_mask & vs->prog->param_mask; bits; bits &= bits - 1)
            rc.gs_params |= 1u << vs->info.vs_param_alias[__builtin_ctz(bits)];
        recipe_bindings(&rc, vs->prog, vs->shapes, k->mesh==2?2:0);
        recipe_bindings(&rc, ps->prog, ps->shapes, 1);
        if(k->mesh==2) {
            if(rc.nbind>=RECIPE_BINDINGS)ok=false;
            else {rc.btype[rc.nbind]=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;rc.bstage[rc.nbind++]=0;}
        }
        gfx_recipe_state(&rc.k, k);
        ok = ok && recipe_plan(&rc, plan) && pipeline_for(r, e, &rc, w, n);
        if(ok && k->mesh==2) {
            uint64_t key=texture_hash_words(rc.btype,rc.nbind,geometry_key);
            key=texture_hash_words(rc.bstage,rc.nbind,key);
            key=texture_hash_words(rc.aliases,rc.nbind*sizeof(*rc.aliases),key);
            uint32_t *replacement=NULL;size_t replacement_words=geometry_words;
            if(vk_descriptor_plan_unique(plan)!=plan->count)
                ok=vk_spirv_bindings(geometry,geometry_words,plan,&replacement,&replacement_words);
            if(ok && replacement){free(geometry);geometry=replacement;geometry_words=replacement_words;}
            if(!ok){free(replacement);free(geometry);geometry=NULL;}
            if(ok) {
            for(uint32_t i=0;i<r->pipes->geometry_cache_count;i++)if(r->pipes->geometry_cache[i].key==key) {
                e->prepass=r->pipes->geometry_cache[i].pipe;e->prepass_shared=true;break;
            }
            if(e->prepass) {
                static uint32_t hits;if(hits++<16)LOG_I(GPU,"GS compute pipeline reused across raster state (%016llx)",(unsigned long long)geometry_key);
            } else {
            VkShaderModule module=make_module(r,geometry,geometry_words);
            VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.stage=(VkPipelineShaderStageCreateInfo){.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"};
            VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT sg={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT,.requiredSubgroupSize=32};
            if(r->cs_subgroup==32)ci.stage.pNext=&sg;
            ci.layout=e->layout;vk_shader_cache_use_begin();
            ok=module && vk_check_device(r,vkCreateComputePipelines(r->device,vk_shader_cache_pipelines(),1,&ci,NULL,&e->prepass),"vkCreateComputePipelines (prepass)")==VK_SUCCESS;
            vk_shader_cache_use_end();if(module)vkDestroyShaderModule(r->device,module,NULL);
            if(ok && r->pipes->geometry_cache_count<PS5C_ARRAY_COUNT(r->pipes->geometry_cache)) {
                uint32_t at=r->pipes->geometry_cache_count++;
                r->pipes->geometry_cache[at].key=key;r->pipes->geometry_cache[at].pipe=e->prepass;e->prepass_shared=true;
            }
            }
            }
            if(!ok) {
                if(e->pipe)vkDestroyPipeline(r->device,e->pipe,NULL);
                if(e->layout)vkDestroyPipelineLayout(r->device,e->layout,NULL);
                if(e->dsl)vkDestroyDescriptorSetLayout(r->device,e->dsl,NULL);
                e->pipe=VK_NULL_HANDLE;e->layout=VK_NULL_HANDLE;e->dsl=VK_NULL_HANDLE;
            }
        }
    }
    free(geometry);
    free(w[0]);
    free(w[1]);
    if (!ok && e->pending) return false; /* logged when it is built */
    LOG_I(GPU, "pipeline %s: %s 0x%llx PS 0x%llx prim %u, %u targets (%u pipelines, %.0f ms)", ok ? "created" : "FAILED",
          k->mesh==2?"GS compute/replay":k->mesh==1?"MESH":"VS", (unsigned long long)k->vs, (unsigned long long)k->ps, k->prim, k->rt_count, r->pipes->pipe_count,
          (pal_time_ns() - t0) / 1e6);
    return ok;
}

static bool create_compute(renderer *r, pipe_entry *e, stage_state *cs, const vk_descriptor_plan *plan) {
    uint32_t *w[2] = {NULL, NULL};
    size_t n[2] = {0, 0};
    pipe_recipe rc;
    memset(&rc, 0, sizeof(rc));
    uint64_t t0 = pal_time_ns();
    if (getenv("PS5C_LOG_PIPELINE_START")) LOG_I(GPU, "compute pipeline start: CS 0x%llx", (unsigned long long)cs->prog->base);
    bool ok = stage_spirv(cs, 0, &rc.spv[0], &w[0], &n[0]);
    if (ok) {
        rc.version = RECIPE_VERSION;
        rc.compute = 1;
        recipe_bindings(&rc, cs->prog, cs->shapes, 2);
        ok = recipe_plan(&rc, plan) && pipeline_for(r, e, &rc, w, n);
    }
    free(w[0]);
    if (!ok && e->pending) return false;
    LOG_I(GPU, "compute pipeline %s: CS 0x%llx, local size %ux%ux%u (%.0f ms)", ok ? "created" : "FAILED",
          (unsigned long long)cs->prog->base, cs->info.local_size[0], cs->info.local_size[1], cs->info.local_size[2],
          (pal_time_ns() - t0) / 1e6);
    return ok;
}

/* ---- Resources ---- */

typedef struct bind_list {
    VkWriteDescriptorSet w[2 * (SH_MAX_SITES + 1)];
    VkDescriptorBufferInfo buf[2 * (SH_MAX_SITES + 1)];
    VkDescriptorImageInfo img[2 * (SH_MAX_SITES + 1)];
    vk_image *bound_image[2 * (SH_MAX_SITES + 1)];
    uint32_t count;
    vk_image *sampled[2 * SH_MAX_SITES];
    uint32_t sampled_count;
    vk_image *written[2 * SH_MAX_SITES]; /* storage images (image_store) */
    uint32_t written_count;
    bool missing; /* an image could not be bound: skip the work (not a full arena) */
} bind_list;

static bool bvh_save_targets(renderer *r,const bind_list *bl);

static bool image_written(const bind_list *bl, const vk_image *img) {
    for (uint32_t i = 0; i < bl->written_count; i++)
        if (bl->written[i] == img) return true;
    return false;
}

/* A sampled descriptor of a storage image must name the layout used by both
 * bindings during the command. GENERAL permits sampling and shader writes. */
static void shared_image_layouts(bind_list *bl) {
    for (uint32_t i = 0; i < bl->count; i++)
        if (bl->w[i].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
            image_written(bl, bl->bound_image[i])) bl->img[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
}

static void update_descriptor_set(renderer *r, VkDescriptorSet set, bind_list *bl,
                                   const vk_descriptor_plan *plan) {
    if (!plan->compact) {
        for (uint32_t i = 0; i < bl->count; i++) bl->w[i].dstSet = set;
        vkUpdateDescriptorSets(r->device, bl->count, bl->w, 0, NULL);
        return;
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < bl->count; i++) if (plan->binding[i] == i) {
        VkWriteDescriptorSet *w = &r->pipes->descriptor_writes[count++];
        *w = bl->w[i];
        w->dstSet = set;
        w->dstBinding = i; /* binding numbers may have gaps */
    }
    vkUpdateDescriptorSets(r->device, count, r->pipes->descriptor_writes, 0, NULL);
}

/* Binds the draw's descriptors: the set update_descriptor_set filled, or (push layout) the same
 * writes pushed into the command buffer. */
static void bind_set(renderer *r, VkCommandBuffer cmd, VkPipelineBindPoint point, const pipe_entry *e, VkDescriptorSet set,
                     bind_list *bl, const vk_descriptor_plan *plan) {
    if (!e->push) {
        vkCmdBindDescriptorSets(cmd, point, e->layout, 0, 1, &set, 0, NULL);
        return;
    }
    if (!plan->compact) {
        r->push_descriptor_set(cmd, point, e->layout, 0, bl->count, bl->w);
        return;
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < bl->count; i++) if (plan->binding[i] == i) {
        VkWriteDescriptorSet *w = &r->pipes->descriptor_writes[count++];
        *w = bl->w[i];
        w->dstBinding = i;
    }
    r->push_descriptor_set(cmd, point, e->layout, 0, count, r->pipes->descriptor_writes);
}

static bool descriptor_plan_confirm(vk_descriptor_plan *plan, const bind_list *bl) {
    if (!plan->compact) return plan->count == bl->count; /* producer made an identity plan */
    if (vk_descriptor_plan_images_match(plan, bl->w, bl->img, bl->count)) return true;
    static uint32_t reported;
    if (reported++ < 8) LOG_W(GPU, "descriptor aliases differ from the bound views: rebuilding with separate bindings");
    vk_descriptor_plan_identity(plan);
    return false;
}

static void add_buffer(renderer *r, bind_list *bl, VkDeviceSize off, VkDeviceSize size) {
    uint32_t i = bl->count++;
    bl->buf[i].buffer = r->pipes->work->arena.buffer;
    bl->buf[i].offset = off;
    bl->buf[i].range = size;
    memset(&bl->w[i], 0, sizeof(bl->w[i]));
    bl->w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    bl->w[i].dstBinding = i;
    bl->w[i].descriptorCount = 1;
    bl->w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bl->w[i].pBufferInfo = &bl->buf[i];
}

/* The sampled hash of a large texture reads 16 KiB of it, so a CPU edit between the
 * samples went unseen: Return to Monkey Island draws accented letters into a text
 * strip after its upload, and "m scara" stayed without its a-acute. Each upload keeps
 * a hash per 64 KiB slice of the guest memory; the checks after it hash one slice in
 * turn and compare it, twice round the texture. Only then: reading every texture's
 * memory all the time cost Dragon Ball Sparking Zero two thirds of its frames. */
#define TEX_SCAN_SLICE (64u << 10)
#define TEX_DYNAMIC_SETTLE 64 /* unchanged whole hashes before a dynamic texture is sampled again */
static void texture_scan_drop(vk_pipelines *P, uint32_t idx) {
    free(P->tex_scan[idx]);
    P->tex_scan[idx] = NULL;
    P->tex_scan_count[idx] = P->tex_scan_next[idx] = 0;
}
static void texture_scan_baseline(vk_pipelines *P, uint32_t idx, const uint8_t *src, uint64_t size) {
    static int off = -1; /* PS5C_NO_TEX_SCAN=1: sampled detector only */
    if (off < 0) off = getenv("PS5C_NO_TEX_SCAN") != NULL;
    uint64_t n = (size + TEX_SCAN_SLICE - 1) / TEX_SCAN_SLICE;
    if (off || !n || n > 65536) return;
    if (P->tex_scan_count[idx] != n) {
        texture_scan_drop(P, idx);
        if (!(P->tex_scan[idx] = (uint64_t *)malloc(n * sizeof(uint64_t)))) return;
        P->tex_scan_count[idx] = (uint32_t)n;
    }
    for (uint64_t k = 0; k < n; k++) {
        uint64_t at = k * TEX_SCAN_SLICE, len = size - at < TEX_SCAN_SLICE ? size - at : TEX_SCAN_SLICE;
        P->tex_scan[idx][k] = texture_xxh64(src + at, (size_t)len, 0) | 1;
    }
    P->tex_scan_next[idx] = 0;
}
static bool texture_scan(vk_pipelines *P, uint32_t idx, const uint8_t *src, uint64_t size) {
    uint32_t n = P->tex_scan_count[idx];
    if (!P->tex_scan[idx] || (size + TEX_SCAN_SLICE - 1) / TEX_SCAN_SLICE != n) return false;
    if (P->tex_scan_next[idx] >= 2 * n) {
        texture_scan_drop(P, idx); /* settled */
        return false;
    }
    uint32_t k = P->tex_scan_next[idx]++ % n;
    uint64_t at = (uint64_t)k * TEX_SCAN_SLICE, len = size - at < TEX_SCAN_SLICE ? size - at : TEX_SCAN_SLICE;
    uint64_t h = texture_xxh64(src + at, (size_t)len, 0) | 1;
    bool changed = P->tex_scan[idx][k] != h;
    P->tex_scan[idx][k] = h;
    return changed;
}

void vk_pipeline_forget_image(renderer *r, uint32_t index) {
    vk_pipelines *P = r->pipes;
    if (!P || index >= VK_MAX_IMAGES) return;
    P->tex_hash[index] = 0;
    P->tex_checked[index] = 0;
    P->tex_dynamic[index] = false;
    P->tex_dynamic_still[index] = 0;
    P->tex_fill_hash[index] = 0;
    P->tex_armed[index] = false;
    P->tex_watch_streak[index] = P->tex_watch_wait[index] = 0;
    memset(&P->tex_cpu[index],0,sizeof(P->tex_cpu[index]));
    texture_scan_drop(P, index);
}

/* Whether guest bytes are all zero: a texture there untiles to zeros in every swizzle mode. */
static bool bytes_zero(const uint8_t *p, uint64_t n) {
    for (; n && ((uintptr_t)p & 7); n--, p++) if (*p) return false;
    const uint64_t *q = (const uint64_t *)p;
    for (; n >= 64; n -= 64, q += 8)
        if (q[0] | q[1] | q[2] | q[3] | q[4] | q[5] | q[6] | q[7]) return false;
    for (p = (const uint8_t *)q; n; n--, p++) if (*p) return false;
    return true;
}

/* Render targets can also be CPU textures. Compare the texels the T#
 * actually exposes: linear row padding and unused tiled texels may belong
 * to other resources. A GPU write establishes a new CPU baseline, otherwise
 * a CPU edit made BEFORE that write can restore an obsolete CPU image. */
static uint64_t texture_cpu_hash(renderer *r,const texture_cpu_source *s,bool full) {
    const uint8_t *src=guest_host(r,s->addr,s->size);
    uint64_t packed=(uint64_t)s->w*s->h*s->bpe;
    if(!src || !packed || !s->layers)return 0;
    uint64_t hash=FNV_INIT;
    if(packed*s->layers<=SMALL_TEXTURE) {
        for(uint32_t l=0;l<s->layers;l++) {
            untile_layer(src+s->slice*l,r->pipes->tex_hash_scratch,s->sw,s->w,s->h,s->bpe,s->ox,s->oy,
                         gcn_layer_xor(s->sw,s->bpe,l));
            hash=texture_hash_words(r->pipes->tex_hash_scratch,(size_t)packed,hash);
        }
        return hash|1;
    }
    /* Fully occupied allocations have no padding to exclude. Their native
     * byte order is an equally stable fingerprint, and needs no detiling. */
    if(!s->ox && !s->oy && s->size==packed*s->layers)
        return full?texture_xxh64(src,(size_t)s->size,hash)|1:texture_content_hash_fast(src,s->size);
    /* Hash logical texels, not allocation padding shared with other surfaces.
     * Large static images retain a bounded detector; CPU-rewritten images use
     * every texel. Both modes use the SAME layout for upload and GPU baselines. */
    uint32_t bw=0,bh=0,block=0;
    bool linear=tex_linear(s->sw);
    if(!linear)std_block(s->sw,s->bpe,&bw,&bh,&block);
    uint64_t total=packed*s->layers;
    uint64_t pitch=tex_linear_pitch(s->sw,s->w,s->h,s->bpe);
    if(full && linear) {
        for(uint32_t l=0;l<s->layers;l++)for(uint32_t y=0;y<s->h;y++)
            hash=texture_hash_words(src+s->slice*l+((uint64_t)y+s->oy)*pitch+(uint64_t)s->ox*s->bpe,
                                    (size_t)s->w*s->bpe,hash);
        return hash|1;
    }
    uint64_t blocks=bw?std_blocks_per_row(s->sw,s->w+s->ox,s->h+s->oy,s->bpe):0;
    uint32_t bw_shift=bw?(uint32_t)__builtin_ctz(bw):0;
    uint32_t bh_shift=bh?(uint32_t)__builtin_ctz(bh):0;
    uint32_t block_shift=block?(uint32_t)__builtin_ctz(block):0;
    uint32_t gx_end=s->ox+s->w;
    if(full && s->sw) {
        uint32_t step=std_contig_elements(s->sw,s->bpe,0,16);
        uint32_t step_bytes=step*s->bpe;
        if(!s->ox && !(s->w&(step-1)) && (step_bytes==16 || step_bytes==8)) {
            for(uint32_t l=0;l<s->layers;l++) {
                const uint8_t *lsrc=src+(uint64_t)l*s->slice;
                for(uint32_t y=0;y<s->h;y++) {
                    uint32_t gy=y+s->oy;
                    const uint8_t *row_src=lsrc+(((uint64_t)(gy>>bh_shift)*blocks)<<block_shift);
                    uint32_t y_off=std_offset(s->sw,0,gy,s->bpe)^gcn_layer_xor(s->sw,s->bpe,l);
                    if(step_bytes==16) {
                        for(uint32_t gx=0;gx<gx_end;gx+=step) {
                            const uint8_t *p=row_src+(((uint64_t)(gx>>bw_shift)<<block_shift)+(y_off^std_offset(s->sw,gx,0,s->bpe)));
                            hash=texture_hash_words(p,16,hash);
                        }
                    } else {
                        for(uint32_t gx=0;gx<gx_end;gx+=step) {
                            const uint8_t *p=row_src+(((uint64_t)(gx>>bw_shift)<<block_shift)+(y_off^std_offset(s->sw,gx,0,s->bpe)));
                            hash=texture_hash_words(p,8,hash);
                        }
                    }
                }
            }
            return hash|1;
        }
        uint8_t tail[8];
        uint32_t tail_len=0;
        for(uint32_t l=0;l<s->layers;l++) {
            const uint8_t *lsrc=src+(uint64_t)l*s->slice;
            for(uint32_t y=0;y<s->h;y++) {
                uint32_t gy=y+s->oy;
                const uint8_t *row_src=lsrc+(((uint64_t)(gy>>bh_shift)*blocks)<<block_shift);
                uint32_t y_off=std_offset(s->sw,0,gy,s->bpe)^gcn_layer_xor(s->sw,s->bpe,l);
                for(uint32_t gx=s->ox;gx<gx_end;) {
                    uint32_t span=std_contig_elements(s->sw,s->bpe,gx,gx_end);
                    const uint8_t *p=row_src+(((uint64_t)(gx>>bw_shift)<<block_shift)+(y_off^std_offset(s->sw,gx,0,s->bpe)));
                    size_t run=(size_t)span*s->bpe;
                    gx+=span;
                    if(tail_len) {
                        size_t need=8-tail_len;
                        if(run<need) {
                            memcpy(tail+tail_len,p,run);
                            tail_len+=(uint32_t)run;
                            continue;
                        }
                        memcpy(tail+tail_len,p,need);
                        hash=texture_hash_words(tail,8,hash);
                        p+=need;
                        run-=need;
                        tail_len=0;
                    }
                    size_t aligned=run&~(size_t)7;
                    if(aligned) {
                        hash=texture_hash_words(p,aligned,hash);
                        p+=aligned;
                        run-=aligned;
                    }
                    if(run) {
                        memcpy(tail,p,run);
                        tail_len=(uint32_t)run;
                    }
                }
            }
        }
        if(tail_len)hash=texture_hash_words(tail,tail_len,hash);
        return hash|1;
    }
    uint32_t bpe_shift=(uint32_t)__builtin_ctz(s->bpe);
    uint32_t bpe_mask=s->bpe-1u;
    uint64_t count=full?total:256*64;
    uint64_t cur_logical=UINT64_MAX,layer=0,byte_in_pixel=0;
    uint32_t x=s->ox,y=s->oy;
    for(uint64_t i=0;i<count;) {
        uint64_t logical=full?i:((total-64)/255)*(i/64)+(((total-64)%255)*(i/64))/255;
        uint64_t chunk=full?(total-i<64?total-i:64):64;
        if(logical!=cur_logical) {
            uint64_t in_layer;
            if(s->layers==1) {
                layer=0;
                in_layer=logical;
            } else {
                layer=logical/packed;
                in_layer=logical%packed;
            }
            uint64_t pixel=in_layer>>bpe_shift;
            byte_in_pixel=in_layer&bpe_mask;
            x=(uint32_t)(pixel%s->w)+s->ox;
            y=(uint32_t)(pixel/s->w)+s->oy;
        }
        uint8_t bytes[64];
        for(uint64_t k=0;k<chunk;) {
            uint64_t offset=!linear?((((uint64_t)(y>>bh_shift)*blocks+(x>>bw_shift))<<block_shift)+std_offset(s->sw,x,y,s->bpe))
                                     ^gcn_layer_xor(s->sw,s->bpe,(uint32_t)layer)
                                 :(uint64_t)y*pitch+(uint64_t)x*s->bpe;
            uint32_t span=!linear?std_contig_elements(s->sw,s->bpe,x,gx_end):(gx_end-x);
            uint64_t run=(uint64_t)span*s->bpe-byte_in_pixel;
            uint64_t rem=chunk-k;
            if(run<=rem) {
                memcpy(bytes+k,src+layer*s->slice+offset+byte_in_pixel,(size_t)run);
                k+=run;
                byte_in_pixel=0;
                x+=span;
                if(x==gx_end) {
                    x=s->ox;
                    if(++y==s->oy+s->h){y=s->oy;layer++;}
                }
            } else {
                memcpy(bytes+k,src+layer*s->slice+offset+byte_in_pixel,(size_t)rem);
                byte_in_pixel+=rem;
                x+=(uint32_t)(byte_in_pixel>>bpe_shift);
                byte_in_pixel&=bpe_mask;
                k=chunk;
            }
        }
        cur_logical=logical+chunk;
        hash=texture_hash_words(bytes,(size_t)chunk,hash);i+=chunk;
    }
    return hash|1;
}
/* PS5C_GPU_WATCH=1 (gpu_watch.h): a large texture whose pages the CPU has not
 * written since they were armed and hashed is not hashed again. One the CPU
 * keeps rewriting is not armed for a while: every arm costs a protection
 * change, and every page written afterwards a fault. */
static bool texture_unwritten(vk_pipelines *P,uint32_t idx) {
    if(!P->tex_armed[idx] || !P->tex_hash[idx])return false;
    const texture_cpu_source *s=&P->tex_cpu[idx];
    if(!gpu_watch_written(GPU_WATCH_TEXTURES,s->addr,s->size)) {
        P->tex_watch_streak[idx]=0;
        g_stat_texskip++;
        return true;
    }
    P->tex_armed[idx]=false;
    if(++P->tex_watch_streak[idx]>=4) {P->tex_watch_streak[idx]=0;P->tex_watch_wait[idx]=64;}
    return false;
}
/* Before the guest memory of the texture is read (hashed or uploaded). */
static void texture_arm(vk_pipelines *P,uint32_t idx) {
    P->tex_armed[idx]=false;
    const texture_cpu_source *s=&P->tex_cpu[idx];
    if(!gpu_watch_enabled() || s->size<=SMALL_TEXTURE)return;
    if(P->tex_watch_wait[idx]) {P->tex_watch_wait[idx]--;return;}
    P->tex_armed[idx]=gpu_watch_arm(GPU_WATCH_TEXTURES,s->addr,s->size);
}
static void texture_gpu_write(renderer *r,const vk_image *img) {
    uint32_t idx=(uint32_t)(img-r->images);
    if(idx>=VK_MAX_IMAGES)return;
    texture_cpu_source *s=&r->pipes->tex_cpu[idx];
    if(s->size) {
        /* Large sources reuse the checked submission baseline. Small ring
         * textures retain their stricter write-time check: DMA publication
         * does not currently invalidate tex_checked. */
        bool small=s->size<=SMALL_TEXTURE;
        if(!small && r->pipes->tex_checked[idx]==r->pipes->epoch && r->pipes->tex_hash[idx])return;
        if(texture_unwritten(r->pipes,idx))return; /* the baseline is still the memory's hash */
        uint64_t t0=stat_now();
        texture_arm(r->pipes,idx);
        r->pipes->tex_hash[idx]=texture_cpu_hash(r,s,r->pipes->tex_dynamic[idx]);
        r->pipes->tex_checked[idx]=small?0:r->pipes->epoch;
        stat_add(ST_TEXHASH,t0);
    }
}

void vk_pipeline_forget_view(renderer *r, VkImageView view) {
    vk_pipelines *P = r->pipes;
    if (!P || !view) return;
    for (uint32_t i = 0; i < P->fb_count;) {
        fb_entry *e = &P->fbs[i];
        bool found = false;
        for (uint32_t j = 0; j < e->count; j++) found |= e->views[j] == view;
        if (!found) {
            i++;
            continue;
        }
        /* Called only for retired views after the fence/queue has completed.
         * Keeping the framebuffer would also alias a recycled view handle. */
        if (!r->dyn_render) vkDestroyFramebuffer(r->device, e->fb, NULL);
        *e = P->fbs[--P->fb_count];
    }
}

/* Host image for a T#. Surfaces the GPU rendered are used as they are; others
 * are uploaded (untiled) when their guest contents change. */
/* An image with more layers in place of `old`: what the GPU wrote in the
 * old one is copied over, the rest comes from guest memory. */
static vk_image *grow_layers(renderer *r, vk_image *old, VkFormat vf, uint32_t layers) {
    guest_addr addr = old->addr;
    uint32_t w = old->width, h = old->height;
    /* `old` was used in this batch: the cache does not replace it */
    vk_image *img = vk_image_get_layers(r, addr, w, h, vf, layers);
    if (!img || img == old) return img;
    if (old->gpu_written && old->format == img->format && old->layers < layers && !old->ext_w) {
        end_pass(r);
        vk_barrier(r->cmd, old, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
        vk_barrier(r->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy c = {0};
        c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = img->aspect;
        c.srcSubresource.layerCount = c.dstSubresource.layerCount = old->layers;
        c.extent.width = w;
        c.extent.height = h;
        c.extent.depth = 1;
        vkCmdCopyImage(r->cmd, old->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        img->gpu_written = true;
        img->written_seq = old->written_seq;
    }
    return img;
}

/* The view of a descriptor's slices [base, last]: the shader declares
 * cube and array descriptors as 2D arrays, anything else as 2D (layer base). */
/* A view of the layers the descriptor shows, in `format` (the image's own, or a same-size
 * format its bits are reinterpreted as: vk_alias_formats). */
static VkImageView layer_view(renderer *r, vk_image *img, const uint32_t *t, bool array, VkFormat format, bool store) {
    uint32_t type = t[3] >> 28, base = 0, last = 0;
    if (type == 11 || type == 12 || type == 13 || type == 15) base = (t[4] >> 16) & 0x1FFF, last = t[4] & 0x1FFF;
    if (base >= img->layers) base = img->layers - 1;
    if (last < base) last = base;
    uint32_t count = array ? (last - base + 1 < img->layers - base ? last - base + 1 : img->layers - base) : 1;
    if (format == img->format && base == 0 && !array && img->levels <= 1) return img->view;
    if (format == img->format && base == 0 && count == img->layers && img->levels <= 1) return img->array_view;
    /* A reinterpreting view is restricted to one usage below. A storage
     * producer's view cannot be bound later as a sampled image (or vice versa). */
    VkImageUsageFlags view_usage = format != img->format ?
        (store ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT) : 0;
    uint32_t min_lod=0;
    if(r->resident_mips && !store && img->levels>1) {
        uint32_t resource_min=(t[1]>>8)&0xfff,base_level=((t[3]>>12)&15)*256;
        min_lod=resource_min>base_level?resource_min-base_level:0;
        if(min_lod>(img->levels-1)*256)min_lod=(img->levels-1)*256;
    }
    for (uint32_t i = 0; i < img->sub_count; i++) {
        if (img->sub[i].base == base && img->sub[i].count == count && img->sub[i].array == array &&
            img->sub[i].format == format && img->sub[i].usage == view_usage && img->sub[i].min_lod == min_lod)
            return img->sub[i].view;
    }
    if (img->sub_count == img->sub_cap) {
        uint32_t cap = img->sub_cap ? img->sub_cap * 2 : 8;
        struct vk_image_sub *n = cap <= 4096 ? (struct vk_image_sub *)realloc(img->sub, cap * sizeof(*n)) : NULL;
        if (!n) return VK_NULL_HANDLE;
        img->sub = n;
        img->sub_cap = cap;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img->image;
    vi.viewType = img->depth ? VK_IMAGE_VIEW_TYPE_3D : array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = img->aspect;
    vi.subresourceRange.levelCount = store || !img->levels ? 1 : img->levels; /* chains: every level */
    vi.subresourceRange.baseArrayLayer = base;
    vi.subresourceRange.layerCount = count;
    /* a reinterpreting view is used one way only: its format need not support the rest */
    VkImageViewUsageCreateInfo usage = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    usage.usage = view_usage;
    if (format != img->format) vi.pNext = &usage;
    VkImageViewMinLodCreateInfoEXT clamp={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_MIN_LOD_CREATE_INFO_EXT,
        .pNext=vi.pNext,.minLod=(float)min_lod/256.0f};
    if(min_lod)vi.pNext=&clamp;
    VkImageView v;
    if (vkCreateImageView(r->device, &vi, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    img->sub[img->sub_count].base = (uint16_t)base;
    img->sub[img->sub_count].count = (uint16_t)count;
    img->sub[img->sub_count].array = array;
    img->sub[img->sub_count].format = format;
    img->sub[img->sub_count].usage = view_usage;
    img->sub[img->sub_count].min_lod = (uint16_t)min_lod;
    img->sub[img->sub_count++].view = v;
    return v;
}

/* Without a native attachment feedback loop, sample a private copy made before
 * the attachment-writing draw. Keep exactly the view range the descriptor selected. */
static bool depth_feedback(renderer *r, bind_list *bl, vk_image *source) {
    vk_image *copy = NULL;
    for (uint32_t i = 0; i < bl->count; i++) {
        if (bl->bound_image[i] != source ||
            bl->w[i].descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        if (!copy && !(copy = vk_image_feedback(r, source))) return false;
        VkImageView old = bl->img[i].imageView, view = VK_NULL_HANDLE;
        if (old == source->view) view = copy->view;
        else if (old && old == source->stencil_view) view = stencil_view(r, copy);
        else if (old == source->array_view) view = copy->array_view;
        else for (uint32_t k = 0; k < source->sub_count; k++) {
            const struct vk_image_sub *sub = &source->sub[k];
            if (sub->view != old) continue;
            uint32_t t[8] = {0, 0, 0, 13u << 28,
                             ((uint32_t)sub->base << 16) | (sub->base + sub->count - 1u), 0, 0, 0};
            t[1]=(uint32_t)sub->min_lod<<8;
            view = layer_view(r, copy, t, sub->array, sub->format, false);
            break;
        }
        if (!view) return false;
        bl->img[i].imageView = view;
        bl->bound_image[i] = copy;
    }
    if (copy) for (uint32_t i = 0; i < bl->sampled_count; i++)
        if (bl->sampled[i] == source) bl->sampled[i] = copy;
    return true;
}

/* A 3D render target may select one slice without a vertex layer export. */
static VkImageView color_slice_view(renderer *r, vk_image *img, uint32_t slice) {
    if (slice >= (img->depth ? img->depth : img->layers)) return VK_NULL_HANDLE; /* 3D slices or array layers */
    for (uint32_t i = 0; i < img->sub_count; i++)
        if (img->sub[i].base == slice && img->sub[i].count == 1 && !img->sub[i].array && img->sub[i].format == img->format &&
            !img->sub[i].usage && !img->sub[i].min_lod)
            return img->sub[i].view;
    if (img->sub_count == img->sub_cap) {
        uint32_t cap = img->sub_cap ? img->sub_cap * 2 : 8;
        struct vk_image_sub *views = cap <= 4096 ? realloc(img->sub, cap * sizeof(*views)) : NULL;
        if (!views) return VK_NULL_HANDLE;
        img->sub = views;
        img->sub_cap = cap;
    }
    VkImageViewUsageCreateInfo usage = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    usage.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageViewCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &usage};
    info.image = img->image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = img->format;
    info.subresourceRange = (VkImageSubresourceRange){img->aspect, 0, 1, slice, 1};
    VkImageView view;
    if (vkCreateImageView(r->device, &info, NULL, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    img->sub[img->sub_count++] = (struct vk_image_sub){(uint16_t)slice, 1, false, img->format, 0, 0, view};
    return view;
}

static uint64_t g_flow_flip;

static void depth_flow(guest_addr addr, uint32_t w, const char *how);
static vk_image *texture(renderer *r, const uint32_t *t);

/* A sampled descriptor spanning several mip levels [BASE_LEVEL, LAST_LEVEL] whose
 * smaller levels the GPU wrote (each level is its own image here: compute shaders
 * write them one by one, as Sonic Frontiers does with its environment cube maps).
 * Level `img` alone would leave sampling with a LOD (roughness, diffuse) on level 0:
 * a chain image takes every level, copied again when one of them was written since.
 * PS5C_GUEST_MIPS=1 (experiment): levels only in guest memory make a chain too, built
 * again when the base level's contents change (No Man's Sky's terrain BC7 arrays have 9
 * levels; its noise did not change with them, and the planet lost ~8%).
 * PS5C_NO_MIP_CHAINS=1: the base level alone (the old way). */
/* Levels [base, base + n) of a guest texture, untiled straight into the chain image
 * (one level image each would multiply the cached images: No Man's Sky's thousands
 * of material arrays then evicted each other every frame). */
static bool chain_upload(renderer *r, vk_image *c, const uint32_t *t, uint32_t base, uint32_t n) {
    vk_pipelines *P = r->pipes;
    guest_addr addr = ((guest_addr)t[0] << 8) | ((guest_addr)(t[1] & 0xFF) << 40);
    uint32_t w0 = ((t[1] >> 30) | ((t[2] & 0x3FFF) << 2)) + 1, h0 = ((t[2] >> 14) & 0xFFFF) + 1;
    uint32_t fmt = (t[1] >> 20) & 0x1FF, sw = tex_sw(t), last = (t[3] >> 16) & 0xF, bpe = 0, block = 0;
    img_format(fmt, &bpe, &block);
    if (!std_supported(sw) || !bpe || !block || n > 16 || n > c->levels || vk_packed_bytes(fmt)) return false;
    mip_place m0, mp[16];
    mip_locate(sw, w0, h0, block, bpe, last + 1, 0, &m0);
    uint64_t slice = c->layers > 1 ? m0.offset + surface_bytes(sw, (w0 + block - 1) / block, (h0 + block - 1) / block, bpe) : 0;
    uint32_t we[16], he[16], swk[16];
    uint64_t at[16], total = 0, span = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t lv = base + k, lw = w0 >> lv ? w0 >> lv : 1, lh = h0 >> lv ? h0 >> lv : 1;
        we[k] = (lw + block - 1) / block, he[k] = (lh + block - 1) / block;
        swk[k] = gcn_level_sw(sw, lv, we[k], he[k], bpe);
        mip_locate(sw, w0, h0, block, bpe, last + 1, lv, &mp[k]);
        uint64_t end = mp[k].offset + surface_bytes(swk[k], we[k] + mp[k].ox, he[k] + mp[k].oy, bpe) +
                       tex_layer_offset(sw, w0, h0, block, bpe, last + 1, lv, c->layers, c->layers - 1, slice);
        if (end > span) span = end;
        at[k] = total;
        total += (((uint64_t)we[k] * he[k] * bpe * c->layers) + 15) & ~15ull;
    }
    const uint8_t *src = (const uint8_t *)guest_host(r, addr, span);
    if (!src || P->work->upload_count == PS5C_ARRAY_COUNT(P->work->uploads)) return false;
    vk_buffer *staging = &P->work->uploads[P->work->upload_count];
    if (!staging_get(r, P, total, staging)) return false;
    P->work->upload_count++;
    uint64_t t0 = stat_now();
    VkBufferImageCopy copies[16];
    memset(copies, 0, sizeof(copies));
    for (uint32_t k = 0; k < n; k++) {
        uint64_t size = (uint64_t)we[k] * he[k] * bpe;
        for (uint32_t l = 0; l < c->layers; l++)
            untile_layer(src + tex_layer_offset(sw, w0, h0, block, bpe, last + 1, base + k, c->layers, l, slice) +
                             mp[k].offset, (uint8_t *)staging->mapped + at[k] + size * l, swk[k], we[k], he[k], bpe,
                         mp[k].ox, mp[k].oy, gcn_layer_xor(swk[k], bpe, l));
        copies[k].bufferOffset = at[k];
        copies[k].bufferRowLength = we[k] * block;
        copies[k].imageSubresource.aspectMask = c->aspect;
        copies[k].imageSubresource.mipLevel = k;
        copies[k].imageSubresource.layerCount = c->layers;
        copies[k].imageExtent.width = c->width >> k ? c->width >> k : 1;
        copies[k].imageExtent.height = c->height >> k ? c->height >> k : 1;
        copies[k].imageExtent.depth = 1;
    }
    if (g_stats_on) upload_stat(addr, c->width, c->height, c->layers, sw, total, pal_time_ns() - t0);
    stat_add(ST_UPLOAD, t0);
    end_pass(r);
    vk_barrier(r->cmd, c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(r->cmd, staging->buffer, c->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, copies);
    return true;
}

/* CPU-streamed 2D resources retain the descriptor's view dimensions. MIN_LOD
 * is a residency clamp, not BASE_LEVEL: rebasing the image changes resinfo,
 * derivatives and the game's streaming decisions. Query each resident level
 * separately; an unmapped high-resolution mip must not hide the mapped tail.
 * GPU-written subresources keep the existing per-level coherence path. */
static bool resident_hash_fast(vk_pipelines *P) {
    static bool initialized; static const char *setting;
    if (!initialized) { setting = getenv("PS5C_RESIDENT_HASH"); initialized = true; }
    if (!setting) return false;
    if (!P->resident_hash_seen || P->resident_hash_flip != g_trace_flip) {
        bool enabled = !strcmp(setting, "1");
        if (setting[0] == '@') {
            FILE *f = fopen(setting + 1, "r"); int value = 0;
            enabled = f && fscanf(f, "%d", &value) == 1 && value == 1;
            if (f) fclose(f);
        }
        if (!P->resident_hash_seen || P->resident_hash_fast != enabled)
            LOG_I(GPU, "resident content hash: %s (experimental)", enabled ? "XXH64" : "legacy");
        P->resident_hash_seen = true; P->resident_hash_flip = g_trace_flip; P->resident_hash_fast = enabled;
    }
    return P->resident_hash_fast;
}

static vk_image *resident_mips(renderer *r, const uint32_t *t) {
    if(!r->resident_mips || t[3]>>28!=9) return NULL;
    uint32_t base=(t[3]>>12)&15,last=(t[3]>>16)&15;
    uint32_t w0=((t[1]>>30)|((t[2]&0x3fff)<<2))+1,h0=((t[2]>>14)&0xffff)+1;
    uint32_t max_dim=w0>h0?w0:h0,max_level=0;
    while(max_dim>1){max_dim>>=1;max_level++;}
    if(base>=last || last>max_level) return NULL;
    uint32_t sw=tex_sw(t),bpe=0,block=0;
    VkFormat vf=img_format((t[1]>>20)&511,&bpe,&block);
    if(!std_supported(sw) || !bpe || !block || vf==VK_FORMAT_UNDEFINED ||
       vf==VK_FORMAT_R8_SRGB || vf==VK_FORMAT_R8G8_SRGB || vk_packed_bytes((t[1]>>20)&511)) return NULL;
    guest_addr addr=((guest_addr)t[0]<<8)|((guest_addr)(t[1]&255)<<40);
    for(uint32_t lv=base;lv<=last;lv++) {
        vk_image *old=vk_image_find_any(r,addr,w0>>lv?w0>>lv:1,h0>>lv?h0>>lv:1);
        if(old && (old->gpu_written || old->aspect!=VK_IMAGE_ASPECT_COLOR_BIT)) return NULL;
    }
    uint32_t first=((t[1]>>8)&0xfff)>>8;
    if(first<base)first=base;
    if(first>last)first=last;
    vk_image *c=vk_image_get_resident_levels(r,addr,w0>>base?w0>>base:1,h0>>base?h0>>base:1,vf,last-base+1,first-base);
    if(!c) return NULL;
    vk_pipelines *P=r->pipes;
    uint32_t idx=(uint32_t)(c-r->images);
    bool fast = resident_hash_fast(P);
    /* A mode change must not reuse the previous mode's epoch shortcut/hash.
     * The first visit recomputes content and uploads, then normal reuse resumes. */
    uint64_t key=texture_hash_words(t,8*sizeof(*t),FNV_INIT ^ (fast ? UINT64_C(0x9E3779B185EBCA87) : 0))|1;
    if(P->tex_checked[idx]==P->epoch && P->tex_hash[idx]==key && c->chain_hash) return c;
    mip_place mp[16];const uint8_t *src[16];
    uint32_t we[16],he[16],swl[16];uint64_t at[16],total=0,hash=key;
    for(uint32_t lv=first;lv<=last;lv++) {
        uint32_t lw=w0>>lv?w0>>lv:1,lh=h0>>lv?h0>>lv:1;
        we[lv]=(lw+block-1)/block;he[lv]=(lh+block-1)/block;
        swl[lv]=gcn_level_sw(sw,lv,we[lv],he[lv],bpe);
        mip_locate(sw,w0,h0,block,bpe,last+1,lv,&mp[lv]);
        uint64_t bytes=surface_bytes(swl[lv],we[lv]+mp[lv].ox,he[lv]+mp[lv].oy,bpe);
        src[lv]=guest_host(r,addr+mp[lv].offset,bytes);
        if(!src[lv]) return NULL;
        uint64_t part=fast?texture_content_hash_fast(src[lv],bytes):texture_content_hash(src[lv],bytes);
        hash=texture_hash_words(&part,sizeof(part),hash);
        at[lv]=total;total+=((uint64_t)we[lv]*he[lv]*bpe+15)&~15ull;
    }
    hash|=1;
    if(c->chain_hash==hash) {
        P->tex_checked[idx]=P->epoch;P->tex_hash[idx]=key;P->tex_armed[idx]=false;return c;
    }
    if(P->work->upload_count==PS5C_ARRAY_COUNT(P->work->uploads)) return NULL;
    vk_buffer *staging=&P->work->uploads[P->work->upload_count];
    if(!staging_get(r,P,total,staging)) return NULL;
    P->work->upload_count++;
    VkBufferImageCopy copies[16]={0};
    for(uint32_t lv=first;lv<=last;lv++) {
        untile(src[lv],(uint8_t*)staging->mapped+at[lv],swl[lv],we[lv],he[lv],bpe,mp[lv].ox,mp[lv].oy);
        VkBufferImageCopy *copy=&copies[lv-first];
        copy->bufferOffset=at[lv];copy->bufferRowLength=we[lv]*block;
        copy->imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
        copy->imageSubresource.mipLevel=lv-base;copy->imageSubresource.layerCount=1;
        copy->imageExtent=(VkExtent3D){w0>>lv?w0>>lv:1,h0>>lv?h0>>lv:1,1};
    }
    end_pass(r);
    vk_barrier(r->cmd,c,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(r->cmd,staging->buffer,c->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,last-first+1,copies);
    c->gpu_written=true;c->written_seq=c->chain_seq=0;c->chain_hash=hash;
    P->tex_checked[idx]=P->epoch;P->tex_hash[idx]=key;P->tex_armed[idx]=false;
    return c;
}

static vk_image *mip_chain(renderer *r, vk_image *img, const uint32_t *t) {
    static int off = -1, guest = -1;
    if (off < 0) off = getenv("PS5C_NO_MIP_CHAINS") != NULL;
    if (guest < 0) guest = getenv("PS5C_GUEST_MIPS") != NULL;
    uint32_t base = (t[3] >> 12) & 15, last = (t[3] >> 16) & 15;
    if (off || !img || last <= base || img->depth || img->aspect != VK_IMAGE_ASPECT_COLOR_BIT || img->levels > 1) return img;
    uint32_t n = last - base + 1;
    vk_image *lv[16] = {img};
    uint64_t newest = img->gpu_written ? img->written_seq : 0;
    bool gpu_levels = false;
    for (uint32_t k = 1; k < n; k++) {
        uint32_t w = img->width >> k ? img->width >> k : 1, h = img->height >> k ? img->height >> k : 1;
        lv[k] = vk_image_find_layers(r, img->addr, w, h, img->format, img->layers);
        if (lv[k] && lv[k]->gpu_written) {
            gpu_levels = true;
            if (lv[k]->written_seq > newest) newest = lv[k]->written_seq;
        }
    }
    uint32_t idx = (uint32_t)(img - r->images);
    vk_pipelines *P = r->pipes;
    /* levels only in guest memory: a chain of the uploaded levels, while the base is a texture */
    uint64_t base_hash = !gpu_levels && idx < VK_MAX_IMAGES ? P->tex_hash[idx] : 0;
    if (!gpu_levels && (!guest || img->gpu_written || !base_hash)) return img;
    vk_image *c = vk_image_get_levels(r, img->addr, img->width, img->height, img->format, img->layers, n);
    if (!c) return img;
    img = &r->images[idx]; /* still there: used in this batch */
    lv[0] = img;
    if (gpu_levels && c->gpu_written && c->chain_seq >= newest) return c;
    if (!gpu_levels && c->gpu_written && c->chain_hash == base_hash) return c;
    if (!gpu_levels) {
        if (!chain_upload(r, c, t, base, n)) return img;
        c->gpu_written = true;
        c->written_seq = c->chain_seq = 0;
        c->chain_hash = base_hash;
        return c;
    }
    for (uint32_t k = 1; k < n; k++)
        if (!lv[k]) { /* a level the GPU did not write: from guest memory, as a texture of that level */
            uint32_t mt[8];
            memcpy(mt, t, sizeof(mt));
            mt[3] = (mt[3] & ~0xFF000u) | ((base + k) << 12) | ((base + k) << 16);
            lv[k] = texture(r, mt);
            if (lv[k] && (lv[k]->format != img->format || lv[k]->layers != img->layers)) lv[k] = NULL;
        }
    end_pass(r);
    vk_barrier(r->cmd, c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    for (uint32_t k = 0; k < n; k++) {
        if (!lv[k] || lv[k] == c) continue;
        vk_barrier(r->cmd, lv[k], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy cp = {0};
        cp.srcSubresource.aspectMask = cp.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        cp.srcSubresource.layerCount = cp.dstSubresource.layerCount = img->layers;
        cp.dstSubresource.mipLevel = k;
        cp.extent.width = lv[k]->width;
        cp.extent.height = lv[k]->height;
        cp.extent.depth = 1;
        census_copy(lv[k], 4);
        if (lv[k]->ext_w) { /* PS5C_RT_SCALE: a smaller level fills the chain's level by a blit */
            VkImageBlit b = {0};
            b.srcSubresource = cp.srcSubresource;
            b.dstSubresource = cp.dstSubresource;
            b.srcOffsets[1] = (VkOffset3D){(int32_t)lv[k]->ext_w, (int32_t)lv[k]->ext_h, 1};
            b.dstOffsets[1] = (VkOffset3D){(int32_t)lv[k]->width, (int32_t)lv[k]->height, 1};
            vkCmdBlitImage(r->cmd, lv[k]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &b, VK_FILTER_LINEAR);
            continue;
        }
        vkCmdCopyImage(r->cmd, lv[k]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &cp);
    }
    c->gpu_written = true;
    c->written_seq = c->chain_seq = newest;
    c->chain_hash = base_hash;
    { /* PS5C_LOG_MIP_CHAINS=<flip>: from that frame (6 chains), where each level of a cube's chain came from (debugging aid) */
        static uint32_t logged;
        static int64_t frame = -2;
        if (frame == -2) frame = getenv("PS5C_LOG_MIP_CHAINS") ? atoll(getenv("PS5C_LOG_MIP_CHAINS")) : -1;
        if (frame >= 0 && (int64_t)g_trace_flip >= frame && img->layers == 6 && logged < 6) {
            logged++;
            sync_batch(r);
            for (uint32_t k = 0; k < n; k++) {
                char path[128];
                snprintf(path, sizeof(path), "user/frames/chain_%010llx_%u_l%u.bmp", (unsigned long long)img->addr, logged, k);
                LOG_I(GPU, "mip chain 0x%llx %ux%u level %u: %s", (unsigned long long)img->addr, img->width, img->height, k,
                      !lv[k] ? "missing" : lv[k]->gpu_written ? "GPU written" : "guest memory");
                if (lv[k]) vk_save_image(r, lv[k], path);
            }
            vk_begin(r);
        }
    }
    return c;
}

/* A shader reading or writing a depth surface's bits as a color format (Sonic Frontiers copies
 * its depth with a compute shader: 32_UINT in, 32_UINT out), which a depth image cannot be
 * viewed as: a color image of that format at the same address, holding the depth's bits.
 * It takes them again whenever the depth was drawn since. */
static vk_image *depth_as_color(renderer *r, vk_image *z, VkFormat vf) {
    static int off = -1; /* PS5C_NO_DEPTH_AS_COLOR: debugging aid */
    if (off < 0) off = getenv("PS5C_NO_DEPTH_AS_COLOR") != NULL;
    if (off || z->layers != 1 || z->depth || vk_format_bytes(vf) != (z->format == VK_FORMAT_D16_UNORM ? 2u : 4u) ||
        (z->format != VK_FORMAT_D32_SFLOAT && z->format != VK_FORMAT_D32_SFLOAT_S8_UINT && z->format != VK_FORMAT_D16_UNORM))
        return z;
    vk_pipelines *P = r->pipes;
    vk_image *c = vk_image_find(r, z->addr, z->width, z->height, vf);
    if (!c) {
        r->scale_new = z->ext_w != 0; /* PS5C_RT_SCALE: drawn at the depth's size */
        c = vk_image_get(r, z->addr, z->width, z->height, vf); /* z was used in this batch: not evicted */
        r->scale_new = false;
    }
    if (!c) return NULL;
    if (vk_img_w(c) != vk_img_w(z) || vk_img_h(c) != vk_img_h(z)) return z;
    if (c->gpu_written && c->written_seq >= z->written_seq) return c;
    if (!z->gpu_written) return c;
    uint64_t size = (uint64_t)z->width * z->height * vk_format_bytes(vf);
    if (P->work->upload_count == PS5C_ARRAY_COUNT(P->work->uploads)) {r->image_cache_pressure=true;return NULL;}
    vk_buffer *tmp = &P->work->uploads[P->work->upload_count];
    if (!staging_get(r, P, size, tmp)) {r->image_cache_pressure=true;return NULL;}
    P->work->upload_count++;
    end_pass(r);
    vk_barrier(r->cmd, z, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = {0};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = vk_img_w(z);
    copy.imageExtent.height = vk_img_h(z);
    copy.imageExtent.depth = 1;
    census_copy(z, 2);
    vkCmdCopyImageToBuffer(r->cmd, z->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp->buffer, 1, &copy);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vk_barrier(r->cmd, c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vkCmdCopyBufferToImage(r->cmd, tmp->buffer, c->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    c->gpu_written = true;
    c->written_seq = z->written_seq;
    static uint32_t logged;
    if (logged < 8 && getenv("PS5C_LOG_DEPTH_AS_COLOR")) {
        logged++;
        LOG_I(GPU, "depth 0x%llx %ux%u: bits copied to a color image (format %d)", (unsigned long long)z->addr, z->width,
              z->height, (int)vf);
    }
    return c;
}

static void dcc_sample(renderer *r, vk_image *img);
static bool dcc_texture_clear(renderer *r, vk_image *img, const uint32_t *t, uint64_t bytes);
static int dcc_keys(renderer *r, guest_addr img_addr, guest_addr start, uint64_t size, bool log_on, bool *pending_out,
                    uint8_t *first);
static void dcc_clear_image(renderer *r, vk_image *img, int code, const uint32_t clear[2]);

/* The depth/stencil image whose stencil lives at `addr` (Unreal reads it in its lighting and
 * post-processing: shading models, custom stencil). Its texels are the stencil_view's. */
static vk_image *stencil_source(renderer *r, guest_addr addr, uint32_t w, uint32_t h) {
    vk_image *best = NULL;
    for (uint32_t i = 0; i < r->image_count; i++) {
        vk_image *z = &r->images[i];
        if (z->image && z->stencil_addr == addr && vk_format_has_stencil(z->format) && z->width == w && z->height == h &&
            z->layers == 1 && (!best || z->written_seq > best->written_seq))
            best = z;
    }
    return best;
}

static VkImageView stencil_view(renderer *r, vk_image *z) {
    if (!z->stencil_view) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = z->image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = z->format;
        vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(r->device, &vi, NULL, &z->stencil_view) != VK_SUCCESS) z->stencil_view = VK_NULL_HANDLE;
    }
    return z->stencil_view;
}

static vk_image *texture(renderer *r, const uint32_t *t) {
    vk_pipelines *P = r->pipes;
    guest_addr addr = ((guest_addr)t[0] << 8) | ((guest_addr)(t[1] & 0xFF) << 40);
    uint32_t w0 = ((t[1] >> 30) | ((t[2] & 0x3FFF) << 2)) + 1, h0 = ((t[2] >> 14) & 0xFFFF) + 1;
    uint32_t fmt = (t[1] >> 20) & 0x1FF, sw_mode = tex_sw(t), bpe, block;
    VkFormat vf = img_format(fmt, &bpe, &block);
    if(vf==VK_FORMAT_R8_SRGB || vf==VK_FORMAT_R8G8_SRGB) {
        VkFormatProperties fp;vkGetPhysicalDeviceFormatProperties(r->phys,vf,&fp);
        if(!(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
            vf=vf==VK_FORMAT_R8_SRGB?VK_FORMAT_R8_UNORM:VK_FORMAT_R8G8_UNORM;
    }
    /* the view starts at BASE_LEVEL: that level is the image here */
    uint32_t base_level = (t[3] >> 12) & 0xF, last_level = (t[3] >> 16) & 0xF;
    if (base_level > last_level) base_level = 0;
    uint32_t w = w0 >> base_level ? w0 >> base_level : 1, h = h0 >> base_level ? h0 >> base_level : 1;
    if (vf == VK_FORMAT_R8_UINT && g_stencil) { /* a stencil drawn with a depth image, read as a texture */
        vk_image *z = stencil_source(r, addr, w, h);
        if (z) return z;
    }
    /* cube (its faces), 1D/2D arrays: layers of one image, each slice
     * holding its whole mip chain. DEPTH is the last slice the descriptor
     * shows (BASE_ARRAY the first): the image grows to the most seen. */
    uint32_t type = t[3] >> 28, layers = 1;
    if (type == 10) { /* 3D: DEPTH [12:0] is the last slice */
        uint32_t d = (t[4] & 0x1FFF) + 1;
        vk_image *img3 = vk_image_find_3d(r, addr, w, h, d, vf);
        if (img3 && img3->gpu_written) return img3; /* drawn slice by slice (Unreal's color LUT) */
        if (!img3) img3 = vk_image_get_3d(r, addr, w, h, d, vf);
        if (!img3) return NULL;
        uint32_t idx3 = (uint32_t)(img3 - r->images);
        if (idx3 < PS5C_ARRAY_COUNT(P->tex_checked) && P->tex_checked[idx3] == P->epoch && P->tex_hash[idx3]) return img3;
        if (idx3 < PS5C_ARRAY_COUNT(P->tex_checked)) P->tex_checked[idx3] = P->epoch;
        /* Standard 3D blocks interleave Z with X/Y. Handle single-level S3
         * surfaces here; retain the existing path for other modes/mip chains. */
        uint32_t we = (w + block - 1) / block, he = (h + block - 1) / block;
        vk_3d_layout layout3 = {0};
        bool standard3 = !last_level && vk_std_3d_layout(sw_mode, bpe, &layout3);
        uint64_t slice3 = surface_bytes(sw_mode, we, he, bpe), size3 = (uint64_t)we * he * bpe;
        uint64_t guest_size3 = standard3 ? vk_std_3d_bytes(&layout3, we, he, d) : slice3 * d;
        const uint8_t *src3 = (const uint8_t *)guest_host(r, addr, guest_size3);
        if (!src3 || !std_supported(sw_mode)) return img3;
        uint64_t hash3 = texture_hash_words(&fmt, sizeof(fmt), texture_content_hash(src3, guest_size3));
        if (idx3 < PS5C_ARRAY_COUNT(P->tex_hash)) {
            if (P->tex_hash[idx3] == hash3) return img3;
            P->tex_hash[idx3] = hash3;
        }
        if (P->work->upload_count == PS5C_ARRAY_COUNT(P->work->uploads)) return img3;
        vk_buffer *staging = &P->work->uploads[P->work->upload_count];
        uint32_t packed3 = vk_packed_bytes(fmt);
        uint64_t upload3 = (uint64_t)we * he * d * (packed3 ? 4 : bpe);
        if (!staging_get(r, P, upload3, staging)) return img3;
        P->work->upload_count++;
        if (standard3) vk_std_3d_unpack(src3, staging->mapped, &layout3, we, he, d);
        else for (uint32_t z = 0; z < d; z++)
            untile(src3 + slice3 * z, (uint8_t *)staging->mapped + size3 * z, sw_mode, we, he, bpe, 0, 0);
        if (packed3) vk_packed_expand(fmt, staging->mapped, (uint64_t)we * he * d);
        end_pass(r);
        vk_barrier(r->cmd, img3, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy3 = {0};
        copy3.bufferRowLength = we * block;
        copy3.imageSubresource.aspectMask = img3->aspect;
        copy3.imageSubresource.layerCount = 1;
        copy3.imageExtent.width = w;
        copy3.imageExtent.height = h;
        copy3.imageExtent.depth = d;
        vkCmdCopyBufferToImage(r->cmd, staging->buffer, img3->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy3);
        return img3;
    }
    bool array = type == 11 || type == 12 || type == 13 || type == 15;
    if (array) layers = (t[4] & 0x1FFF) + 1;
    if (layers > 256) layers = 256;
    vk_image *img = array ? vk_image_find_array(r, addr, w, h, vf) : NULL;
    if (array) { /* a depth array drawn slice by slice (depth_image), sampled as floats: newer than a float array */
        vk_image *z = vf == VK_FORMAT_R32_SFLOAT ? find_depth32(r, addr, w, h, true)
                    : vf == VK_FORMAT_R16_UNORM ? vk_image_find_array(r, addr, w, h, VK_FORMAT_D16_UNORM) : NULL;
        if (z && z->gpu_written && (!img || z->written_seq > img->written_seq)) img = z;
        static const char *dbg = (const char *)1; /* PS5C_DEBUG_DEPTH_ARRAY_CLEAR=<value>: debugging aid, the depth array
                                                   * sampled holds that value (is the shadow test the culprit?) */
        if (dbg == (const char *)1) dbg = getenv("PS5C_DEBUG_DEPTH_ARRAY_CLEAR");
        if (dbg && img && img == z) {
            end_pass(r);
            vk_barrier(r->cmd, z, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkClearDepthStencilValue cv = {(float)atof(dbg), 0};
            VkImageSubresourceRange range = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, z->layers};
            vkCmdClearDepthStencilImage(r->cmd, z->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range);
        }
    }
    if (!img) img = vk_image_find_any(r, addr, w, h);
    if (img && img->aspect == VK_IMAGE_ASPECT_DEPTH_BIT && depth_integer_pair(img->format) == vf) {
        /* Copy shaders bind an integer image, never a depth-format storage view
         * (kept in step with the depth image by sync_depth_alias below). */
        depth_flow(addr, w, "as integer");
        img = vk_image_get_layers(r, addr, w, h, vf, layers);
        if (!img) return NULL;
    } else if (img && img->aspect != VK_IMAGE_ASPECT_COLOR_BIT && vf != VK_FORMAT_UNDEFINED && vf != img->format &&
               !((img->format == VK_FORMAT_D32_SFLOAT || img->format == VK_FORMAT_D32_SFLOAT_S8_UINT) &&
                 vf == VK_FORMAT_R32_SFLOAT) &&
               !(img->format == VK_FORMAT_D16_UNORM && vf == VK_FORMAT_R16_UNORM)) {
        depth_flow(addr, w, "as color");
        img = depth_as_color(r, img, vf); /* its bits in a format a depth image cannot be viewed as */
        if (!img) return NULL;
    }

    /* Another texel size at the same address is another resource sharing the memory
     * (Unreal's transient surfaces): this one has its own image. Same-size formats
     * reinterpret the bits (views in the descriptor's format, see image_view_format). */
    if (img && img->format != vf && img->aspect == VK_IMAGE_ASPECT_COLOR_BIT &&
        (!vk_format_bytes(img->format) || !vk_format_bytes(vf) ||
         (vk_format_bytes(img->format) != vk_format_bytes(vf) && !no_alias_views())))
        /* Compressed formats have no bytes-per-texel value or mutable alias
         * views. BC1 and BC3 at the same address need distinct host images:
         * uploading BC1 bytes into BC3 otherwise reads past the staging buffer. */
        img = vk_image_find(r, addr, w, h, vf);
    if (img && layers > img->layers) {
        img = grow_layers(r, img, vf, layers);
        if (!img) return NULL;
    }
    if (img) layers = img->layers;
    if (img && !sync_depth_alias(r, img)) {
        r->image_cache_pressure = true; /* complete this batch and retry the copy */
        return NULL;
    }

    uint32_t w_el = (w + block - 1) / block, h_el = (h + block - 1) / block;
    mip_place mip = {0};
    if (std_supported(sw_mode)) mip_locate(sw_mode, w0, h0, block, bpe, last_level + 1, base_level, &mip);
    uint32_t chain_sw = sw_mode; /* the whole chain's mode, for level 0 and the slice size */
    if (g_gcn && gcn_sw(chain_sw) && chain_sw != GCN_SW_UNSUPPORTED) {
        /* PS4: the layout derives the pitch from the size (addrlib SI rules); the T# states it.
         * A difference means a padding rule is missing here (debugging aid, a few lines). */
        static uint32_t reported;
        uint32_t tp = (((t[7] >> 6) & 0x3FFF) + 1 + block - 1) / block, w0e = (w0 + block - 1) / block,
                 h0e = (h0 + block - 1) / block, ours = 0;
        if (chain_sw == GCN_SW_LINEAR) ours = gcn_linear_pitch(w0e, h0e, bpe);
        else if (gcn_macro_sw(chain_sw)) ours = (w0e + gcn_macro(chain_sw, bpe)->bw - 1) / gcn_macro(chain_sw, bpe)->bw * gcn_macro(chain_sw, bpe)->bw;
        else ours = gcn_tiles_per_row(w0e, h0e, bpe) * 8;
        bool pow2 = (t[7] >> 5) & 1 && last_level > 0;
        if ((tp != ours || pow2) && reported < 16) {
            reported++;
            LOG_W(GPU, "PS4 texture 0x%llx %ux%u tiling %u: T# pitch %u, layout pitch %u%s", (unsigned long long)addr, w0, h0,
                  t[7] & 0x1F, tp, ours, pow2 ? ", pow2-padded mips (not applied)" : "");
        }
    }
    sw_mode = gcn_level_sw(sw_mode, base_level, w_el, h_el, bpe);
    uint64_t gsize = surface_bytes(sw_mode, w_el + mip.ox, h_el + mip.oy, bpe), slice = 0;
    if (layers > 1) {
        mip_place m0 = {0};
        if (std_supported(chain_sw)) mip_locate(chain_sw, w0, h0, block, bpe, last_level + 1, 0, &m0);
        slice = m0.offset + surface_bytes(chain_sw, (w0 + block - 1) / block, (h0 + block - 1) / block, bpe);
        /* a PS4 array with mips is level-major: its slices start further on, a level apart */
        uint64_t l0 = tex_layer_offset(chain_sw, w0, h0, block, bpe, last_level + 1, base_level, layers, 0, slice);
        uint64_t l1 = tex_layer_offset(chain_sw, w0, h0, block, bpe, last_level + 1, base_level, layers, 1, slice);
        mip.offset += l0;
        slice = l1 - l0;
        gsize += slice * (layers - 1);
    }
    /* Small textures (tiles uploaded through a ring of slots, several per batch,
     * some also written by copy shaders) are compared at every use; for them,
     * guest memory the CPU changed after the GPU wrote the image wins. */
    bool small = gsize <= SMALL_TEXTURE;
    /* Large textures the GPU wrote (a fill turned into an image clear) are still
     * compared with guest memory: the CPU may decode into them afterwards (Sonic
     * Frontiers' CRI video planes stayed on an old frame, in blocks).
     * PS5C_NO_TEX_RECHECK=1: keep GPU-written large textures as before. */
    static int recheck = -1;
    if (recheck < 0) recheck = getenv("PS5C_NO_TEX_RECHECK") == NULL;
    if (img && img->aspect != VK_IMAGE_ASPECT_COLOR_BIT) depth_flow(addr, w, "sampled");
    if (img) dcc_sample(r, img);
    if (img && img->gpu_written && !small && !recheck) return img;
    if (!img) {
        img = vk_image_get_layers(r, addr, w, h, vf, layers);
        if (!img) return NULL;
        static int log_tex = -1; /* PS5C_LOG_TEX: every new texture's descriptor (debugging aid) */
        if (log_tex < 0) log_tex = getenv("PS5C_LOG_TEX") != NULL;
        if (log_tex)
            LOG_I(GPU, "flip %llu texture 0x%llx %ux%u (level %u of %ux%u, %u levels) fmt %u sw_mode %u type %u depth %u pitch %u T# %08x %08x %08x %08x %08x %08x %08x %08x",
                  (unsigned long long)g_trace_flip, (unsigned long long)addr, w, h, base_level, w0, h0, last_level + 1, fmt, sw_mode, t[3] >> 28,
                  (t[4] & 0x1FFF) + 1, ((t[4] >> 13) & 0x3FFF) + 1, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]);
        dcc_sample(r, img);
        if (img->gpu_written && !small && !recheck) return img;
    }
    uint32_t idx = (uint32_t)(img - r->images);
    if(std_supported(sw_mode) && idx<VK_MAX_IMAGES) {
        texture_cpu_source source;memset(&source,0,sizeof(source));
        source.addr=addr+mip.offset;source.size=gsize;source.slice=slice;
        source.sw=sw_mode;source.w=w_el;source.h=h_el;source.bpe=bpe;source.ox=mip.ox;source.oy=mip.oy;source.layers=layers;source.fmt=vk_packed_bytes(fmt)?fmt:0;
        if(memcmp(&source,&P->tex_cpu[idx],sizeof(source))) {
            P->tex_hash[idx]=P->tex_checked[idx]=P->tex_fill_hash[idx]=0;P->tex_dynamic[idx]=false;P->tex_dynamic_still[idx]=0;P->tex_cpu[idx]=source;
            texture_scan_drop(P,idx);
            P->tex_armed[idx]=false;P->tex_watch_streak[idx]=P->tex_watch_wait[idx]=0;
        }
    }
    if (idx < PS5C_ARRAY_COUNT(P->tex_checked)) {
        /* once per epoch, small ones too: the CPU rewrites guest memory between submissions
         * (Minecraft's ring of 16x16 tile slots), each of which starts an epoch */
        if (P->tex_checked[idx] == P->epoch && P->tex_hash[idx]) return img;
        P->tex_checked[idx] = P->epoch;
        if (std_supported(sw_mode) && texture_unwritten(P, idx)) return img;
    }
    const uint8_t *src = (const uint8_t *)guest_host(r, addr, mip.offset + gsize);
    if (src) src += mip.offset;
    bool supported = std_supported(sw_mode);
    uint64_t t0 = stat_now();
    /* A large texture the CPU keeps rewriting (video planes) is hashed whole: the
     * sampled detector misses most changes of smooth chroma planes, which then
     * kept the colors of an older scene. */
    bool dynamic = !small && idx < VK_MAX_IMAGES && P->tex_dynamic[idx];
    if (src && supported && idx < VK_MAX_IMAGES) texture_arm(P, idx);
    uint64_t hash = src && supported ? texture_cpu_hash(r,&P->tex_cpu[idx],dynamic) : 1;
    bool scanned = !small && !dynamic && !img->gpu_written && src && supported && idx < VK_MAX_IMAGES &&
                   texture_scan(P, idx, src, gsize);
    stat_add(ST_TEXHASH, t0);
    if (img->gpu_written) {
        /* GPU-written: keep it until the CPU rewrites its memory */
        if (idx >= PS5C_ARRAY_COUNT(P->tex_hash) || !src || !supported || !hash) return img;
        if (!P->tex_hash[idx] || P->tex_hash[idx] == hash) {
            P->tex_hash[idx] = hash;
            return img;
        }
        static unsigned invalidation_logs;
        const char *watch = getenv("PS5C_LOG_TEX_INVALIDATE");
        if(watch && strtoull(watch,NULL,16)==addr && invalidation_logs++<32)
            LOG_I(GPU,"texture CPU invalidation flip %llu addr 0x%llx seq %llu old hash %llx new hash %llx size %llu",
                (unsigned long long)g_trace_flip,(unsigned long long)addr,(unsigned long long)img->written_seq,
                (unsigned long long)P->tex_hash[idx],(unsigned long long)hash,(unsigned long long)gsize);
        img->gpu_written = false;
    }
    if (layers == 1 && !mip.offset && dcc_texture_clear(r, img, t, (uint64_t)w_el * h_el * bpe)) {
        if (idx < PS5C_ARRAY_COUNT(P->tex_hash)) P->tex_hash[idx] = hash; /* kept until the CPU rewrites it */
        return img;
    }
    /* A dynamic texture that stopped changing goes back to the sampled hash and the
     * slice scan: Tomb Raider I-III's 90 MB array (512x512, 256 layers), filled
     * layer by layer while a level loads, was hashed whole at every frame of the
     * level afterwards (~38 ms each). Video planes change at every frame and stay
     * dynamic. PS5C_TEX_DYNAMIC_KEEP=1: a dynamic texture stays dynamic. */
    if (dynamic && src && supported && hash && P->tex_hash[idx] == hash) {
        static int keep = -1;
        if (keep < 0) keep = getenv("PS5C_TEX_DYNAMIC_KEEP") != NULL;
        if (!keep && ++P->tex_dynamic_still[idx] >= TEX_DYNAMIC_SETTLE) {
            static unsigned settle_logs;
            if (settle_logs++ < 8)
                LOG_I(GPU, "texture 0x%llx %ux%u x%u (%llu MB) unchanged for %u whole hashes: sampled again",
                      (unsigned long long)addr, w, h, layers, (unsigned long long)(gsize >> 20), TEX_DYNAMIC_SETTLE);
            P->tex_dynamic[idx] = false;
            P->tex_dynamic_still[idx] = 0;
            P->tex_hash[idx] = texture_cpu_hash(r, &P->tex_cpu[idx], false);
            texture_scan_baseline(P, idx, src, gsize);
        }
        return img;
    }
    if (idx < VK_MAX_IMAGES) P->tex_dynamic_still[idx] = 0;
    if (idx < PS5C_ARRAY_COUNT(P->tex_hash) && P->tex_hash[idx] == hash && !scanned) return img;
    if (idx < PS5C_ARRAY_COUNT(P->tex_hash)) {
        /* the sampled detector saw the change: hashed whole from now on (a slice scan
         * alone does not, a text strip gaining a letter is no video plane) */
        if (!small && P->tex_hash[idx] && P->tex_hash[idx] != hash && src && supported && !dynamic) {
            P->tex_dynamic[idx] = true;
            hash=texture_cpu_hash(r,&P->tex_cpu[idx],true);
        }
        P->tex_hash[idx] = hash;
        if (!small && src && supported && !P->tex_dynamic[idx]) texture_scan_baseline(P, idx, src, gsize);
    }

    end_pass(r);
    static int log_uploads = -1; /* PS5C_LOG_TEX_UPLOADS: every upload of a texture from guest memory (debugging aid) */
    if (log_uploads < 0) log_uploads = getenv("PS5C_LOG_TEX_UPLOADS") != NULL;
    if (log_uploads && src)
        LOG_I(GPU, "flip %llu upload 0x%llx %ux%u fmt %u sw %u: first bytes %02x %02x %02x %02x, hash %016llx",
              (unsigned long long)g_trace_flip, (unsigned long long)addr, w, h, fmt, sw_mode, src[0], src[1], src[2], src[3],
              (unsigned long long)hash);
    if (src && supported) {
        uint32_t packed = vk_packed_bytes(fmt); /* untiled first, then grown to RGBA8 in place */
        uint64_t size = (uint64_t)w_el * h_el * (packed ? 4 : bpe);
        /* Zeroed memory is a clear: Unreal samples its 4K transient surfaces at fresh
         * addresses, and untiling 63 MB of zeros took ~100 ms per surface. */
        if (!small && !packed && block == 1 && img->aspect == VK_IMAGE_ASPECT_COLOR_BIT && bytes_zero(src, gsize)) {
            vk_barrier(r->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkClearColorValue zero = {{0, 0, 0, 0}};
            VkImageSubresourceRange range = {img->aspect, 0, 1, 0, layers};
            vkCmdClearColorImage(r->cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
            if (g_stats_on) upload_stat(addr, w, h, layers, sw_mode, 0, pal_time_ns() - t0);
            return img;
        }
        /* A batch stages at most STAGING_BATCH_BYTES: then it is submitted and the copy retried.
         * Commandos: Origins' 63 MB 4K uploads otherwise held gigabytes of host-visible buffers
         * per batch, past the integrated GPU's memory budget. */
        uint64_t staged = 0;
        for (uint32_t i = 0; i < P->work->upload_count; i++) staged += P->work->uploads[i].size;
        if (P->work->upload_count && staged + size * layers > STAGING_BATCH_BYTES) {
            if (idx < PS5C_ARRAY_COUNT(P->tex_hash)) P->tex_hash[idx] = 0;
            r->image_cache_pressure = true;
            return NULL;
        }
        /* No staging slot left in this batch (a level loading hundreds of textures):
         * the draw is retried after the batch is submitted. Sampling the image as it
         * is gave a draw done once an empty input: TMNT: Splintered Fate's colour
         * grading table came out black, and the whole tutorial scene very dark. */
        vk_buffer *staging = &P->work->uploads[P->work->upload_count];
        if (P->work->upload_count == PS5C_ARRAY_COUNT(P->work->uploads) || !staging_get(r, P, size * layers, staging)) {
            if (idx < PS5C_ARRAY_COUNT(P->tex_hash)) P->tex_hash[idx] = 0; /* uploaded on the retry */
            static unsigned full_logs;
            if (full_logs++ < 8)
                LOG_I(GPU, "texture 0x%llx %ux%u: no upload slot left in this batch, the draw is retried after it",
                      (unsigned long long)addr, w, h);
            r->image_cache_pressure = true;
            return NULL;
        }
        P->work->upload_count++;
        t0 = stat_now();
        for (uint32_t l = 0; l < layers; l++)
        {
            untile_layer(src + slice * l, (uint8_t *)staging->mapped + size * l, sw_mode, w_el, h_el, bpe, mip.ox, mip.oy,
                         gcn_layer_xor(sw_mode, bpe, l));
            if (packed) vk_packed_expand(fmt, (uint8_t *)staging->mapped + size * l, (uint64_t)w_el * h_el);
        }
        if (g_stats_on) upload_stat(addr, w, h, layers, sw_mode, size * layers, pal_time_ns() - t0);
        stat_add(ST_UPLOAD, t0);
        vk_barrier(r->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
        if (img->ext_w) { /* PS5C_RT_SCALE: guest memory is not copied into a smaller target */
            static uint32_t logged;
            if (logged++ < 8)
                LOG_I(GPU, "texture 0x%llx %ux%u: guest contents not copied into a scaled target", (unsigned long long)addr, w, h);
            if (img->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
                VkClearColorValue zero = {{0, 0, 0, 0}};
                VkImageSubresourceRange all = {img->aspect, 0, 1, 0, layers};
                vkCmdClearColorImage(r->cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
            }
            return img;
        }
        VkBufferImageCopy copy = {0};
        copy.bufferRowLength = w_el * block;
        copy.imageSubresource.aspectMask = img->aspect;
        copy.imageSubresource.layerCount = layers;
        copy.imageExtent.width = w;
        copy.imageExtent.height = h;
        copy.imageExtent.depth = 1;
        census_copy(img, 0);
        vkCmdCopyBufferToImage(r->cmd, staging->buffer, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        return img;
    }
    static uint32_t warned;
    if (!(warned & (1u << (sw_mode & 31)))) {
        warned |= 1u << (sw_mode & 31);
        if(!supported)LOG_W(GPU, "texture swizzle mode %u not supported yet (shown black)", sw_mode);
        else LOG_W(GPU,"texture 0x%llx %ux%u swizzle %u: guest range of %llu bytes is unavailable (shown black)",
                   (unsigned long long)addr,w,h,sw_mode,(unsigned long long)(mip.offset+gsize));
    }
    vk_barrier(r->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue c = {{0, 0, 0, 0}};
    VkImageSubresourceRange range = {img->aspect, 0, 1, 0, 1};
    if (block == 1) vkCmdClearColorImage(r->cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
    return img;
}

/* A buffer written by an earlier dispatch of this batch that holds [addr, addr + size). */
/* PS5C_WB_TAIL=1: a read up to 16 bytes past a writeback binds it (the tail reads as zeros, not guest bytes) */
static uint64_t wb_tail(void) {
    static int on = -1;
    if (on < 0) on = getenv("PS5C_WB_TAIL") != NULL;
    return on ? 16 : 0;
}

static writeback *find_writeback(vk_pipelines *P, guest_addr addr, uint64_t size) {
    for (uint32_t i = P->wb_count; i-- > 0;) {
        writeback *w = &P->wb[i];
        /* up to 16 bytes past its end still count as inside: bindings carry that much padding, and every
         * copy of a writeback has it (Minecraft reads 1 MiB + 16 of the 1 MiB a compute shader just wrote) */
        if (addr >= w->addr && addr + size <= w->addr + w->size + wb_tail() && addr < w->addr + w->size &&
            (addr - w->addr) % P->align == 0) return w;
    }
    return NULL;
}

/* Resources of one stage: uniforms, buffer copies, images. False when the arena
 * is full. Buffers a compute shader writes stay in the arena for the rest of
 * the batch and go back to guest memory when it completes; `stored` is set
 * when the stage writes any, `overflow` when one could not be tracked. */
/* Black 1x1 stand-in for a texture the shader reads through a null (or
 * unknown) descriptor: the hardware returns zeros there. Stores get their
 * own image so they never show up in reads. */
#define NULL_TEXTURE_ADDR 0x1
#define NULL_STORAGE_ADDR 0x2
static vk_image *null_image(renderer *r, bool store, const sh_site_shape *shape, bool atomic) {
    bool raw=shape->buf_format==SH_IMG_PACKED_R11G11B10 || shape->buf_format==SH_IMG_ATOMIC_FLOAT32 ||
             shape->buf_format==SH_IMG_ATOMIC_UINT32;
    VkFormat format=shape->buf_format==SH_IMG_ATOMIC_UINT64 ? VK_FORMAT_R64_UINT
                  :raw || atomic ? (shape->img_kind==2 && !raw?VK_FORMAT_R32_SINT:VK_FORMAT_R32_UINT)
                  :shape->img_kind==1?VK_FORMAT_R32G32B32A32_UINT
                  :shape->img_kind==2?VK_FORMAT_R32G32B32A32_SINT:VK_FORMAT_R8G8B8A8_UNORM;
    bool volume=shape->img_type==10;
    guest_addr addr=(store?NULL_STORAGE_ADDR:NULL_TEXTURE_ADDR)+((guest_addr)format<<8)+(volume?0x1000000:0);
    vk_image *img = volume?vk_image_get_3d(r,addr,1,1,1,format):vk_image_get(r,addr,1,1,format);
    if (!img) return NULL;
    img->last_use = r->use_seq;
    if (img->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        end_pass(r);
        vk_barrier(r->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue zero = {{0, 0, 0, 0}};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(r->cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        img->gpu_written = true;
    }
    return img;
}

/* Compute stores copy only the window the analysis proves (sh_store_range). Experimental, off by
 * default: PS5C_STORE_WINDOWS=1 turns it on; =check copies whole descriptors and reports changes
 * outside the window (2). The check flagged Minecraft's copy shader (possibly later dispatches of
 * the batch writing into the same copy), not yet settled. */
static int store_windows_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("PS5C_STORE_WINDOWS");
        on = !e ? 0 : !strcmp(e, "check") ? 2 : atoi(e) != 0;
    }
    return on;
}

static bool dynamic_ranges_alias(gmem *mem,guest_addr a,uint64_t an,guest_addr b,uint64_t bn);
static bool eval_source_hash(renderer *r,const eval_ranges *read,uint64_t *hash);

static bool footprint_record(const sh_program *p) {
    static bool init,configured;static uint64_t selected;
    if(!init) {
        configured=getenv("PS5C_STORE_FOOTPRINT")!=NULL;
        const char *value=getenv("PS5C_STORE_FOOTPRINT_SHADER");
        selected=value?strtoull(value,NULL,16):0;init=true;
    }
    return configured && p->has_stores && (!selected || selected==p->hash);
}

/* 0 retains the complete binding/writeback, 1 prunes only proved CPU-visible
 * writes, 2 verifies those intervals against the completed full GPU buffer.
 * Operand recording stays identical across @file A/B modes. */
static unsigned footprint_mode(void) {
    static bool init,seen;static const char *value;static uint64_t flip;static unsigned mode;
    if(!init){value=getenv("PS5C_STORE_FOOTPRINT");init=true;}
    if(value && (!seen || flip!=g_trace_flip)) {
        unsigned next=0;
        if(value[0]=='@') {FILE*f=fopen(value+1,"r");if(f){if(fscanf(f,"%u",&next)!=1)next=0;fclose(f);}}
        else next=!strcmp(value,"check")?2:(unsigned)strtoul(value,NULL,10);
        if(next>2)next=0;
        if(!seen || next!=mode)LOG_I(GPU,"writeback footprint mode: %u (experimental)",next);
        mode=next;flip=g_trace_flip;seen=true;
    }
    return mode;
}

/* The range evaluator records scalar values from a representative workgroup.
 * Accept only forward-flow raw-buffer programs whose scalar memory addresses
 * do not depend on that group. Direct, untouched TGID vector operands are
 * bounded by sh_store_range; a later read of a potentially overwritten TGID
 * is refused rather than mistaking group zero for a uniform constant. */
static bool footprint_linear(const stage_state *st) {
    const sh_program *p=st->prog;
    if(st->gcn || st->large_reads || p->count>2048 || !st->eval.vsrc)return false;
    uint32_t group[3],ng=0;bool live[3]={true,true,true},written[3]={false,false,false};
    for(unsigned d=0;d<3;d++)if(st->info.cs_tgid_enable>>d&1)group[ng]=st->info.cs_tgid_sgpr+ng,ng++;
    for(uint32_t i=0;i<p->count;i++) {
        const rdna2_inst *q=&p->insts[i];unsigned op=p->ops[i];const char *name=q->name?q->name:"";
        if((p->iflags[i]&SH_IF_FLOW) && (p->target[i]<=i || !strstr(name,"cbranch")))return false;
        bool scalar=q->enc==RDNA2_ENC_SOP1 || q->enc==RDNA2_ENC_SOP2 || q->enc==RDNA2_ENC_SOPK || q->enc==RDNA2_ENC_SOPC;
        bool smem=q->enc==RDNA2_ENC_SMEM;
        bool vector=q->enc==RDNA2_ENC_VOP1 || q->enc==RDNA2_ENC_VOP2 || q->enc==RDNA2_ENC_VOP3 || q->enc==RDNA2_ENC_VOPC;
        if(!scalar && !smem && !vector && q->enc!=RDNA2_ENC_SOPP && q->enc!=RDNA2_ENC_MUBUF)return false;
        if(vector && (((p->iflags[i]&SH_IF_64) && op!=SH_OP_V_CMP && op!=SH_OP_V_CMPX) ||
           op==SH_OP_V_SWAP_B32 || op==SH_OP_V_WRITELANE_B32 ||
           op==SH_OP_V_MOVRELD_B32 || op==SH_OP_V_MOVRELSD_B32))return false;
        if(strstr(name,"getreg") || strstr(name,"setreg") || strstr(name,"saveexec") || strstr(name,"wrexec") ||
           (scalar && strstr(name,"movrel")))return false;
        unsigned width=(p->iflags[i]&SH_IF_64)?2:1,load_width=0;
        if(smem) {
            if(strncmp(name,"s_load_dword",12) && strncmp(name,"s_buffer_load_dword",18))return false;
            load_width=strstr(name,"dwordx16")?16:strstr(name,"dwordx8")?8:strstr(name,"dwordx4")?4:strstr(name,"dwordx2")?2:1;
        }
        for(unsigned k=0;k<ng;k++) {
            uint32_t g=group[k];if(g>=SH_SGPRS)return false;
            if(vector)for(unsigned s=0;s<3;s++)if(q->src[s]==g && written[k])return false;
            if(live[k] && scalar) {
                for(unsigned s=0;s<2;s++)if(q->src[s]<=g && g<q->src[s]+width)return false;
                if(q->dst<=g && g<q->dst+width &&
                   (q->enc==RDNA2_ENC_SOPK || strstr(name,"cmov") || strstr(name,"bitset")))return false;
            }
            if(live[k] && smem) {
                unsigned n=!strncmp(name,"s_buffer",8)?4:2;
                if((q->sbase<=g && g<q->sbase+n) || q->soffset==g)return false;
            }
            if(live[k] && q->enc==RDNA2_ENC_MUBUF &&
               ((q->srsrc<=g && g<q->srsrc+4u) || q->soffset==g))return false;
            if((q->enc==RDNA2_ENC_SOP1 || q->enc==RDNA2_ENC_SOP2 || q->enc==RDNA2_ENC_SOPK) && q->dst<=g && g<q->dst+width)live[k]=false;
            if(smem && q->dst<=g && g<q->dst+load_width)live[k]=false;
            /* Match the conservative definition used by sgpr_written_before. */
            unsigned may_width=smem?16:width;
            if(q->enc!=RDNA2_ENC_SOPP && q->dst<SH_SGPRS && q->dst<=g && g<q->dst+may_width)written[k]=true;
            if((q->enc==RDNA2_ENC_VOP3 || q->enc==RDNA2_ENC_VOPC) && (q->sdst==g || q->sdst+1u==g))written[k]=true;
            if(q->enc==RDNA2_ENC_VOPC && (g==RDNA2_OP_VCC_LO || g==RDNA2_OP_VCC_HI))written[k]=true;
            if((op==SH_OP_V_READFIRSTLANE_B32 || op==SH_OP_V_READLANE_B32) && (q->dst&0x7f)==g)written[k]=true;
        }
    }
    return true;
}

static bool footprint_sources_current(renderer *r,const stage_state *st) {
    uint64_t hash;
    return st->footprint_hash_valid && eval_source_hash(r,&st->footprint_reads,&hash) && hash==st->footprint_hash;
}

static void footprint_build(renderer *r,stage_state *st) {
    st->footprint_count=0;
    const sh_program *p=st->prog;
    if(!footprint_linear(st) || !footprint_sources_current(r,st))return;
    for(uint32_t i=0;i<p->site_count;i++)if(p->sites[i].store) {
        const sh_site_value *v=&st->eval.site[i];const rdna2_inst *in=&p->insts[p->sites[i].inst];
        if(p->sites[i].kind!=SH_SITE_BUFFER || p->ops[p->sites[i].inst]!=SH_OP_BUFFER_STORE_DWORD ||
           in->enc!=RDNA2_ENC_MUBUF || !v->valid || v->desc_unknown || st->shapes[i].address_map)goto reject;
        guest_addr base=(guest_addr)v->desc[0]|((guest_addr)(v->desc[1]&0xffff)<<32);
        uint32_t stride=(v->desc[1]>>16)&0x3fff;uint64_t size=(uint64_t)v->desc[2]*(stride?stride:1),lo,hi;
        if(v->records_unknown) {
            gmem_vma vm;
            if(!gmem_query_vma(r->mem,base,false,&vm) || vm.reserved)goto reject;
            size=vm.start+vm.size-base;if(size>MAX_BUFFER_COPY)size=MAX_BUFFER_COPY;
        }
        if(!base || !size || size>MAX_BUFFER_COPY || !sh_store_range(p,&st->eval,&st->info,st->groups,i,&lo,&hi) ||
           lo>=hi || hi>size)goto reject;
        uint32_t j=0;while(j<st->footprint_count && st->footprint[j].base!=base)j++;
        if(j==st->footprint_count) {
            if(j==PS5C_ARRAY_COUNT(st->footprint))goto reject;
            st->footprint[j].base=base;st->footprint[j].size=size;st->footprint[j].lo=lo;st->footprint[j].hi=hi;
            st->footprint_count++;
        } else {
            if(st->footprint[j].size!=size)goto reject;
            if(lo<st->footprint[j].lo)st->footprint[j].lo=lo;
            if(hi>st->footprint[j].hi)st->footprint[j].hi=hi;
        }
    }
    /* Scalar snapshots must stay uniform while this dispatch runs. */
    for(uint32_t i=0;i<st->footprint_count;i++)for(uint32_t j=0;j<st->footprint_reads.count;j++)
        if(dynamic_ranges_alias(r->mem,st->footprint[i].base+st->footprint[i].lo,st->footprint[i].hi-st->footprint[i].lo,
                               st->footprint_reads.start[j],st->footprint_reads.end[j]-st->footprint_reads.start[j]))goto reject;
    return;
reject:st->footprint_count=0;
}

static void footprint_prepare(renderer *r,stage_state *st) {
    st->footprint_count=0;
    if(footprint_record(st->prog) && footprint_mode() && !store_windows_on()) {
        footprint_build(r,st);
        static uint64_t reported[16];static uint32_t count;
        bool seen=false;for(uint32_t i=0;i<count;i++)seen|=reported[i]==st->prog->hash;
        if(!seen && count<PS5C_ARRAY_COUNT(reported)) {
            reported[count++]=st->prog->hash;
            LOG_I(GPU,"writeback footprint shader %016llx: %u proved ranges, linear %u, scalar snapshot %u",
                (unsigned long long)st->prog->hash,st->footprint_count,footprint_linear(st),st->footprint_hash_valid);
            for(uint32_t i=0;i<st->footprint_count && i<4;i++)LOG_I(GPU,"writeback footprint range: base 0x%llx, [0x%llx,0x%llx) of 0x%llx",
                (unsigned long long)st->footprint[i].base,(unsigned long long)st->footprint[i].lo,
                (unsigned long long)st->footprint[i].hi,(unsigned long long)st->footprint[i].size);
        }
    }
}

/* An immutable input may use the local read-only mirror even when this CS
 * writes other buffers. Reject uncertain descriptors and both virtual and
 * physical aliases of its outputs; copying such an alias would split one
 * mutable resource into two different GPU allocations. */
static bool local_compute_read_safe(renderer *r,const stage_state *st,uint32_t site,
                                    const guest_addr *bases,const uint64_t *sizes,uint64_t copy_size) {
    const sh_program *p=st->prog;
    if(site>=p->site_count || !bases[site] || !copy_size || p->sites[site].store ||
       !st->eval.site[site].valid || st->eval.site[site].desc_unknown)return false;
    for(uint32_t i=0;i<p->site_count;i++) {
        sh_site_kind kind=p->sites[i].kind;
        if(kind!=SH_SITE_SMEM && kind!=SH_SITE_BUFFER && kind!=SH_SITE_SBUFFER)return false;
        if(!p->sites[i].store)continue;
        if(!st->eval.site[i].valid || st->eval.site[i].desc_unknown || !bases[i] || !sizes[i] ||
           dynamic_ranges_alias(r->mem,bases[site],copy_size,bases[i],sizes[i]))return false;
    }
    return true;
}

/* PS5C_LOCAL_COMPUTE=1 extends the optional local read-only arena to proved
 * immutable CS inputs. @file supports an A/B without restarting the game. */
static bool local_compute_enabled(void) {
    static bool initialized,seen,enabled;
    static const char *setting;
    static uint64_t flip;
    if(!initialized){setting=getenv("PS5C_LOCAL_COMPUTE");initialized=true;}
    if(setting && (!seen || flip!=g_trace_flip)) {
        bool next=false;
        if(setting[0]=='@') {
            FILE*f=fopen(setting+1,"r");unsigned value;
            if(f){if(fscanf(f,"%u",&value)==1 && value<=1)next=value!=0;fclose(f);}
        } else next=!strcmp(setting,"1");
        if(!seen || next!=enabled)LOG_I(GPU,"immutable compute inputs in local arena: %s (experimental)",next?"enabled":"disabled");
        enabled=next;seen=true;flip=g_trace_flip;
    }
    return enabled;
}

/* The descriptor selects byte order and numeric interpretation before shader DST_SEL.
 * Integer/float reinterpretation and stores retain their existing alias policy. */
static VkFormat image_view_format(const vk_image *img, const uint32_t *t, bool store, uint32_t kind) {
    uint32_t bpe, block;
    VkFormat vf = img_format((t[1] >> 20) & 0x1FF, &bpe, &block);
    if(img->expanded_srgb && !store) return VK_FORMAT_R8G8B8A8_SRGB;
    if (no_alias_views()) return img->format;
    VkFormat list[VK_IMAGE_ALIAS_CAPACITY];
    uint32_t n = vk_alias_formats(img->format, list);
    return vk_image_descriptor_view_format(img->format, vf, store, kind, list, n);
}

/* PS5C_TRACE_FRAME / PS5C_SAVE_AFTER_FRAME = <flip> or @<file>: with a file, the frame after
 * it appears (it is removed then). Frame numbers drift between runs; a file is created when
 * the game is where it should be. Returns the frame, -1 not yet, -2 not set. */
typedef struct frame_trig {
    bool init;
    const char *env; /* read once: getenv locks */
    int64_t frame;
    uint64_t checked;
} frame_trig;

static int64_t frame_trigger(frame_trig *t, const char *name, uint64_t flips) {
    if (!t->init) {
        t->init = true;
        t->env = getenv(name);
        t->frame = t->env && t->env[0] != '@' ? strtoll(t->env, NULL, 0) : -1;
    }
    if (!t->env) return -2;
    if (t->env[0] != '@' || t->checked == flips + 1) return t->frame;
    /* A fresh file rearms a later frame. Numeric settings remain one-shot;
     * file diagnostics can inspect another scene without relaunching a game. */
    t->checked = flips + 1;
    pal_path_info pi;
    if (pal_path_stat(t->env + 1, &pi) != PS5C_OK) return t->frame;
    pal_file_remove(t->env + 1);
    t->frame = (int64_t)flips + 1;
    LOG_I(GPU, "%s: frame %lld", name, (long long)t->frame);
    return t->frame;
}

/* PS5C_LOG_DEPTH_FLOW=@<file> or <flip>: for 8 frames from then, each 4K depth surface drawn
 * and sampled, once per frame and address (debugging aid: which depth the lighting reads) */
static void depth_flow(guest_addr addr, uint32_t w, const char *how) {
    static frame_trig trig;
    int64_t f = frame_trigger(&trig, "PS5C_LOG_DEPTH_FLOW", g_flow_flip);
    if (f < 0 || g_flow_flip < (uint64_t)f || g_flow_flip >= (uint64_t)f + 8 || w < 2048) return;
    static guest_addr seen[32];
    static char seen_how[32];
    static uint32_t n;
    static uint64_t flip;
    if (flip != g_flow_flip) flip = g_flow_flip, n = 0;
    for (uint32_t i = 0; i < n; i++)
        if (seen[i] == addr && seen_how[i] == how[0]) return;
    if (n < 32) seen[n] = addr, seen_how[n++] = how[0];
    LOG_I(GPU, "depth flow frame %llu: %s 0x%llx", (unsigned long long)g_flow_flip, how, (unsigned long long)addr);
}

/* PS5C_TRACE_IMAGES=<w>x<h> or all (+ PS5C_TRACE_FRAME=<flip> or @<file>): every draw and dispatch of that
 * frame that writes or reads an image of that size is logged with its shaders (debugging
 * aid: which passes feed an image that comes out black). */
static uint64_t g_trace_flip;
static bool trace_image(const vk_image *img) {
    static int on = -1;
    static uint32_t tw[8], th[8], n;
    static uint64_t frame;
    static bool all;
    if (on < 0) { /* sizes separated by commas */
        const char *e = getenv("PS5C_TRACE_IMAGES");
        all = e && !strcmp(e, "all");
        for (const char *p = e; p && *p && n < 8; p = strchr(p, ',') ? strchr(p, ',') + 1 : "")
            if (sscanf(p, "%ux%u", &tw[n], &th[n]) == 2) n++;
        on = all || n > 0;
    }
    if (!on) return false;
    /* PS5C_TRACE_FORMAT=<VkFormat>: images of that format (and size) in any frame, the first 300 */
    static int tfmt = -2;
    static uint32_t tcount;
    static uint64_t format_from;
    if (tfmt == -2) {
        tfmt = getenv("PS5C_TRACE_FORMAT") ? atoi(getenv("PS5C_TRACE_FORMAT")) : -1;
        const char *from = getenv("PS5C_TRACE_FORMAT_FROM");
        format_from = from ? strtoull(from, NULL, 10) : 0;
    }
    if (tfmt >= 0) {
        if (g_trace_flip < format_from) return false;
        if (!img || (int)img->format != tfmt || tcount >= 300) return false;
        for (uint32_t i = 0; i < n; i++)
            if (img->width == tw[i] && img->height == th[i]) return ++tcount, true;
        return all ? (++tcount, true) : false;
    }
    static frame_trig trig;
    int64_t f = frame_trigger(&trig, "PS5C_TRACE_FRAME", g_trace_flip);
    frame = f == -2 ? 0 : f < 0 ? UINT64_MAX : (uint64_t)f;
    if (g_trace_flip != frame) return false;
    if (all) return true;
    if (!img) return false;
    for (uint32_t i = 0; i < n; i++)
        if (img->width == tw[i] && img->height == th[i]) return true;
    return false;
}

/* Interleaved vertex attributes: one V# per attribute, each base a few bytes into the
 * same vertex (TMNT: Splintered Fate: offsets 0, 12, 24 and 40 of a 44 or 48-byte
 * vertex). Each site copied its own range of the buffer, four copies of it a draw.
 * A vertex fetch whose base lies less than a stride after another one's (same stride
 * and record count) binds that one's copy, the shader adding the distance
 * (SH_BUFFER_SHIFTED). Returns the distance, 0 when the site has none (or cannot);
 * *first: the site it binds the copy of. PS5C_NO_VERTEX_SHARE=1 turns it off. */
static bool vertex_share_eligible(const sh_program *p, const sh_eval *e, uint32_t i) {
    const sh_site *site = &p->sites[i];
    if (site->kind != SH_SITE_BUFFER || site->store) return false;
    const rdna2_inst *in = &p->insts[site->inst];
    if ((in->enc != RDNA2_ENC_MUBUF && in->enc != RDNA2_ENC_MTBUF) || !in->idxen) return false;
    const sh_site_value *v = &e->site[i];
    return v->valid && !v->desc_unknown && ((v->desc[1] >> 16) & 0x3FFF) != 0;
}

static uint64_t vertex_share_shift(const sh_program *p, const sh_eval *e, uint32_t i, uint32_t *first) {
    if (first) *first = i;
    if (!vertex_share_eligible(p, e, i)) return 0;
    const uint32_t *d = e->site[i].desc;
    guest_addr base = (guest_addr)d[0] | ((guest_addr)(d[1] & 0xFFFF) << 32), low = base;
    uint32_t stride = (d[1] >> 16) & 0x3FFF, low_site = i;
    for (uint32_t j = 0; j < p->site_count; j++) {
        if (j == i || !vertex_share_eligible(p, e, j)) continue;
        const uint32_t *o = e->site[j].desc;
        guest_addr b = (guest_addr)o[0] | ((guest_addr)(o[1] & 0xFFFF) << 32);
        if (((o[1] >> 16) & 0x3FFF) == stride && o[2] == d[2] && b < low && base - b < stride) low = b, low_site = j;
    }
    if ((base - low) & 3) return 0; /* the shader indexes dwords */
    if (first) *first = low_site;
    return base - low;
}

static bool draw_fail(uint32_t line);
static bool stage_resources(renderer *r, stage_state *st, bind_list *bl, bool writeback_stores, bool *stored,
                            bool *overflow) {
    uint32_t first_binding=bl->count;
    vk_pipelines *P = r->pipes;
    if (P->promote) { /* the retry after a batch break: the image is not in use any more */
        end_pass(r);
        vk_image_promote(r, P->promote);
        P->promote = NULL;
    }
    uint32_t wb_earlier = P->wb_count; /* results of earlier work (not this stage's own stores) */
    const sh_program *p = st->prog;
    st->footprint_count=0;
    if (g_pimp.on > 0) memset(g_pimp_extra, 0, p->site_count * sizeof(g_pimp_extra[0]));
    if(st->info.stage==SH_STAGE_CS)footprint_prepare(r,st);
    VkDeviceSize off;
    uint32_t uniform_shift=st->info.uniform_shift;
    if(!sh_uniform_shift_valid(p->site_count,uniform_shift) || st->eval.smem_words<SH_U_DATA)return false;
    uint32_t packed_words=st->eval.smem_words-uniform_shift;
    uint64_t smem_size = (uint64_t)packed_words * 4;
    uint64_t index_size = st->info.stage==SH_STAGE_MESH && (st->info.mesh_indexed&1u) ? (uint64_t)st->mesh_index_count*4 : 0;
    uint64_t usize = smem_size + index_size;
    static int probing = -1; /* PS5C_VS_PROBE: 16 more words, where the probed vertex writes */
    if (probing < 0) probing = getenv("PS5C_VS_PROBE") != NULL || getenv("PS5C_PS_CAPTURE") != NULL;
    uint64_t spare = probing ? (st->info.stage==SH_STAGE_MESH?128:64) : 16;
    /* Mesh index starts and sidecar addresses are logical u32 word offsets.
     * Keep start + vertex and ArrayLength + shift representable before the
     * prologue maps the appended index table into the physical layout. */
    if((uint64_t)st->eval.smem_words+index_size/4+spare/4>UINT32_MAX)return false;
    if(!st->images_only) {
    if(usize+spare>r->props.limits.maxStorageBufferRange)return false;
    if (!arena_alloc(r, usize + spare, &off)) return false;
    if(uniform_shift) {
        if(!sh_uniform_pack(arena_ptr(r,off),packed_words,st->eval.smem_data,
                            st->eval.smem_words,p->site_count,uniform_shift))return false;
    } else memcpy(arena_ptr(r, off), st->eval.smem_data, smem_size);
    if(g_stats_on>0) {
        g_stat_uniform_count++;
        g_stat_uniform_logical+=(uint64_t)st->eval.smem_words*4+index_size;
        g_stat_uniform_uploaded+=usize;
    }
    if(index_size) {
        uint32_t *uniforms=arena_ptr(r,off);
        const uint8_t *indices=arena_ptr(r,st->mesh_index_off);
        uniforms[sh_uniform_index(uniform_shift,SH_U_MESH+3)]=st->eval.smem_words;
        if(!mesh_indices_expand(uniforms+packed_words,indices,
                                st->mesh_index_count,st->mesh_index_size))return false;
    }
    memset((uint8_t *)arena_ptr(r, off) + usize, 0xEE, spare);
    if(probing && st->info.stage==SH_STAGE_PS) {
        uint32_t *sidecar=(uint32_t *)((uint8_t *)arena_ptr(r,off)+usize);sidecar[12]=0;
        static bool checked;static const char *point_file;static uint64_t point_flip=UINT64_MAX;
        static uint32_t point_x=16384,point_y=16384;
        if(!checked){const char *s=getenv("PS5C_PS_CAPTURE_PIXEL");if(s && *s=='@')point_file=s+1;checked=true;}
        if(point_file) {
            if(point_flip!=g_trace_flip) {
                point_flip=g_trace_flip;point_x=point_y=16384;
                char line[64]={0};FILE *f=fopen(point_file,"r");
                if(f){if(!fgets(line,sizeof(line),f))line[0]=0;fclose(f);}
                unsigned x,y;
                if(!strncmp(line,"any",3))point_x=point_y=UINT32_MAX;
                else if(sscanf(line,"%u %u",&x,&y)==2 && x<16384 && y<16384){point_x=x;point_y=y;}
            }
            sidecar[8]=point_x;sidecar[9]=point_y;
        }
    }
    if(probing && st->info.stage==SH_STAGE_MESH)
        memset((uint8_t *)arena_ptr(r,off)+usize+8*4,0,7*4);
    if(probing && st->info.stage==SH_STAGE_MESH)
        memset((uint8_t *)arena_ptr(r,off)+usize+16*4,0,7*4);
    st->u_off = off;
    st->u_bytes = usize;
    if (st->info.stage == SH_STAGE_CS) {
        uint32_t *uniforms = arena_ptr(r, off), n = st->info.cs_tgid_sgpr;
        for (uint32_t k = 0; k < 3; k++) {
            if (!((st->info.cs_tgid_enable >> k) & 1))
                continue;
            if (n < SH_SGPRS && SH_U_SGPR + n < st->eval.smem_words)
                uniforms[SH_U_SGPR + n] = st->dispatch_base[k];
            n++;
        }
    }
    add_buffer(r, bl, off, usize + spare);
    }
    /* the range each buffer site reads or writes */
    guest_addr site_base[SH_MAX_SITES];
    uint64_t site_size[SH_MAX_SITES];
    bool site_trimmed[SH_MAX_SITES];
    for (uint32_t i = 0; i < p->site_count; i++) {
        const sh_site *site = &p->sites[i];
        const sh_site_value *v = &st->eval.site[i];
        site_base[i] = 0;
        site_size[i] = 0;
        site_trimmed[i] = false;
        if(st->images_only)continue;
        if (site->kind == SH_SITE_BVH) {
            if(P->bvh_table && (st->shapes[i].address_map==3 || st->shapes[i].address_map==4))continue;
            guest_addr base = ((guest_addr)v->desc[0] | ((guest_addr)(v->desc[1] & 255) << 32)) << 8;
            uint64_t units = (uint64_t)v->desc[2] | ((uint64_t)(v->desc[3] & 1023) << 32);
            uint64_t bytes = (units + 1) * 64;
            bool descriptor = (v->desc[3] >> 28) == 8;
            if (v->desc_unknown || (descriptor && (!base || bytes > MAX_BUFFER_COPY || !guest_host(r, base, bytes)))) {
                static uint32_t reports;
                if (reports++ < 24)
                    LOG_W(GPU, "BVH requires dynamic address mapping: shader %016llx site %u desc %08x %08x %08x %08x unknown %d base 0x%llx bytes 0x%llx",
                        (unsigned long long)p->hash, i, v->desc[0], v->desc[1], v->desc[2], v->desc[3],
                        (int)v->desc_unknown, (unsigned long long)base, (unsigned long long)bytes);
                bl->missing = true;
                return false;
            }
            site_base[i] = descriptor && v->valid ? base : 0;
            site_size[i] = descriptor && v->valid ? bytes : 0;
            static int trace = -1;
            static uint32_t traced;
            if (trace < 0) trace = getenv("PS5C_LOG_BVH") != NULL;
            if (trace && traced++ < 16)
                LOG_I(GPU, "BVH bounded span: shader %016llx site %u base 0x%llx bytes 0x%llx type %u valid %d",
                      (unsigned long long)p->hash, i, (unsigned long long)base, (unsigned long long)bytes,
                      v->desc[3] >> 28, (int)v->valid);
            continue;
        }
        if (site->kind != SH_SITE_BUFFER && site->kind != SH_SITE_SBUFFER) continue;
        guest_addr base = (guest_addr)v->desc[0] | ((guest_addr)(v->desc[1] & 0xFFFF) << 32);
        uint32_t stride = (v->desc[1] >> 16) & 0x3FFF, records = v->desc[2];
        uint64_t size = stride ? (uint64_t)records * stride : records;
        /* The hardware range check looks at where an access starts (the offset, or the index
         * when strided), then reads the whole access: a load at the last valid offset still
         * gets all its bytes. Dragon Ball's per-draw instance index sits in a buffer of
         * NUM_RECORDS 1 read as one dword; bound to 1 byte, the index lost its high bits and
         * the scenery was drawn around the camera. Loads get up to 16 more bytes (dwordx4)
         * when that memory is there; stores keep the exact range. */
        if (site->kind == SH_SITE_BUFFER && p->insts[site->inst].enc != RDNA2_ENC_DS &&
            !site->store && size && size < MAX_BUFFER_COPY && v->valid && base) {
            /* V# OOB_SELECT 2 (raw: only NUM_RECORDS != 0 is checked) lets accesses go well past
             * NUM_RECORDS: a page more when there is one */
            uint64_t more = !stride && ((v->desc[3] >> 28) & 3) == 2 ? 4096 : 16;
            if (more > 16 && !guest_host(r, base, size + more)) more = 16;
            if (guest_host(r, base, size + more)) size += more;
        }
        gmem_vma vma;
        if (site->kind == SH_SITE_BUFFER && v->records_unknown && !v->desc_unknown && base &&
            gmem_query_vma(r->mem, base, false, &vma) && !vma.reserved)
            size = vma.start + vma.size - base; /* the shader checks the count it computed */
        if (size > MAX_BUFFER_COPY) size = MAX_BUFFER_COPY;
        if (!v->valid || !base) size = 0;
        /* Unknown NUM_RECORDS used to copy the whole VMA (up to 32 MiB),
         * even when the dispatch provably writes only a small prefix. Keep
         * descriptor checks in the shader; reduce only its host backing.
         * Check mode retains the complete backing to verify every GPU byte. */
        if(site->store && v->records_unknown && footprint_mode()==1)
            for(uint32_t k=0;k<st->footprint_count;k++)
                if(st->footprint[k].base==base && st->footprint[k].hi<size) {
                    size=st->footprint[k].hi;break;
                }
        if (site->kind == SH_SITE_BUFFER && st->trim_vertex_buffers && !site->store && size) {
            uint64_t prefix = vr_site_prefix(p, i, &st->info, v, st->vertex_max, st->instance_max, stride);
            if (prefix < size) {
                if (g_stats_on) {
                    g_stat_saved_bytes += size - prefix;
                    g_stat_trimmed++;
                }
                size = prefix;
                site_trimmed[i] = true;
            }
        }
        site_base[i] = base;
        site_size[i] = size;
    }
    /* One dispatch must see one mutable resource, even when a reader's
     * permitted load tail is wider than the store descriptor. Separate arena
     * snapshots make a lookback load miss the atomic/store at the same guest
     * base. Plan the full bounded union before binding the first reader.
     * Different bases/unknown descriptors keep the existing overlap path. */
    /* shared vertex buffer copies: bind from the first attribute's base, wide enough */
    for (uint32_t i = 0; i < p->site_count; i++)
        if (st->shapes[i].buffer_length == SH_BUFFER_SHIFTED && !st->shapes[i].address_map && site_size[i]) {
            uint64_t shift = vertex_share_shift(p, &st->eval, i, NULL);
            site_base[i] -= shift;
            site_size[i] += shift;
        }
    uint64_t coherent_size[SH_MAX_SITES] = {0};
    uint64_t site_copy_size[SH_MAX_SITES] = {0};
    for(uint32_t i=0;i<p->site_count;i++) {
        if(!site_copy_size[i] && !p->sites[i].store && site_size[i] && site_base[i]) {
            uint64_t widest_read=site_size[i];
            for(uint32_t j=i+1;j<p->site_count;j++)
                if(site_base[j]==site_base[i] && !p->sites[j].store && site_size[j]>widest_read)widest_read=site_size[j];
            for(uint32_t j=i;j<p->site_count;j++)
                if(site_base[j]==site_base[i] && !p->sites[j].store && site_size[j])site_copy_size[j]=widest_read;
        }
        if(!writeback_stores || coherent_size[i] || !site_base[i] || !site_size[i] || st->eval.site[i].desc_unknown ||
           (p->sites[i].kind!=SH_SITE_BUFFER && p->sites[i].kind!=SH_SITE_SBUFFER))continue;
        bool reader=false,writer=false;uint64_t widest=site_size[i];
        for(uint32_t j=i;j<p->site_count;j++) {
            if(site_base[j]!=site_base[i] || !site_size[j] || st->eval.site[j].desc_unknown ||
               (p->sites[j].kind!=SH_SITE_BUFFER && p->sites[j].kind!=SH_SITE_SBUFFER))continue;
            reader|=!p->sites[j].store;writer|=p->sites[j].store;
            if(site_size[j]>widest)widest=site_size[j];
        }
        /* Mark even immutable groups as visited. Large materials refer to a
         * few resources from thousands of sites; scan once per distinct base. */
        uint64_t extent=reader && writer?widest:UINT64_MAX;
        for(uint32_t j=i;j<p->site_count;j++)
            if(site_base[j]==site_base[i] && site_size[j] && !st->eval.site[j].desc_unknown &&
               (p->sites[j].kind==SH_SITE_BUFFER || p->sites[j].kind==SH_SITE_SBUFFER))coherent_size[j]=extent;
    }
    for (uint32_t i = 0; i < p->site_count; i++) {
        const sh_site *site = &p->sites[i];
        const sh_site_value *v = &st->eval.site[i];
        if(st->images_only && site->kind!=SH_SITE_IMAGE)continue;
        if (site->kind == SH_SITE_SMEM) continue;
        if(site->kind==SH_SITE_BVH && P->bvh_table && (st->shapes[i].address_map==3 || st->shapes[i].address_map==4)) {
            add_buffer(r,bl,0,P->bvh_table->size);
            bl->buf[bl->count-1].buffer=P->bvh_table->buffer;
            continue;
        }
        if (site->kind == SH_SITE_BUFFER || site->kind == SH_SITE_SBUFFER || site->kind == SH_SITE_BVH) {
            guest_addr base = site_base[i];
            uint32_t stride = (v->desc[1] >> 16) & 0x3FFF;
            uint64_t size = site_size[i];
            uint64_t coherent_extent=coherent_size[i]==UINT64_MAX?0:coherent_size[i];
            bool coherent=coherent_extent!=0;
            uint64_t full=stride?(uint64_t)v->desc[2]*stride:v->desc[2];
            VkDescriptorBufferInfo imported;
            if(st->large_reads && st->large_reads->write_count && !v->desc_unknown && v->valid &&
               (site->kind==SH_SITE_BUFFER || site->kind==SH_SITE_SBUFFER)) {
                int matched=large_write_bind(r,st->large_reads,base,full,&imported);
                if(matched<0){bl->missing=true;return false;}
                if(matched>0){add_buffer(r,bl,0,16);bl->buf[bl->count-1]=imported;
                    if(site->store && stored)*stored=true;
                    continue;
                }
            }
            if(st->large_reads && !site->store && !v->desc_unknown && v->valid &&
               (site->kind==SH_SITE_BUFFER || site->kind==SH_SITE_SBUFFER) && full>MAX_BUFFER_COPY &&
               ngg_large_bind(r,st->large_reads,base,full,&imported)) {
                add_buffer(r,bl,0,16);bl->buf[bl->count-1]=imported;continue;
            }
            if(st->large_reads && st->info.stage!=SH_STAGE_CS && !p->has_stores && !site->store &&
               !v->desc_unknown && v->valid && (site->kind==SH_SITE_BUFFER || site->kind==SH_SITE_SBUFFER) &&
               full>MAX_BUFFER_COPY && ngg_large_snapshot_bind(r,st->large_reads,base,full,g_trace_flip,&imported)) {
                add_buffer(r,bl,0,16);bl->buf[bl->count-1]=imported;continue;
            }
            /* Metadata only, once per shader/site: a failed full binding still
             * falls back to the existing bounded copy. Expose those losses for
             * producer/consumer diagnosis without changing access or guards. */
            if(full>MAX_BUFFER_COPY && size<full && v->valid && !v->desc_unknown &&
               (site->kind==SH_SITE_BUFFER || site->kind==SH_SITE_SBUFFER)) {
                static int audit=-1;
                static struct {uint64_t hash;uint32_t site;} seen[64];
                static uint32_t nseen;
                if(audit<0){const char*s=getenv("PS5C_LOG_BUFFER_CLIPS");audit=s && !strcmp(s,"1");}
                bool known=false;
                for(uint32_t k=0;audit && k<nseen;k++)known|=seen[k].hash==p->hash && seen[k].site==i;
                if(audit && !known && nseen<PS5C_ARRAY_COUNT(seen)) {
                    seen[nseen].hash=p->hash;seen[nseen++].site=i;
                    LOG_I(GPU,"buffer clip: shader %llx hash %016llx stage %u site %u PC %x %s base %llx requested %llu bound %llu bytes",
                        (unsigned long long)p->base,(unsigned long long)p->hash,st->info.stage,i,p->pc[site->inst]*4,
                        site->store?"store":"read",(unsigned long long)base,(unsigned long long)full,(unsigned long long)size);
                }
            }
            { /* PS5C_LOG_STORE_ADDR=<hex>: shader stores whose buffer covers that address (debugging aid) */
                static int have = -1;
                static guest_addr want;
                static uint32_t logged;
                if (have < 0) {
                    have = getenv("PS5C_LOG_STORE_ADDR") != NULL;
                    if (have) want = strtoull(getenv("PS5C_LOG_STORE_ADDR"), NULL, 16);
                }
                static int published = -1;
                if (published < 0) published = have && !strcmp(getenv("PS5C_LOG_STORE_ADDR"), "break");
                if (published) want = runtime_debug_addr(); /* PS5C_BREAK_PUBLISH */
                guest_addr raw = (guest_addr)v->desc[0] | ((guest_addr)(v->desc[1] & 0xFFFF) << 32);
                /* the buffers a matching shader reads are chased the same way (up to 4 steps) */
                guest_addr *chase = g_store_chase;
                uint32_t chased = g_store_chased;
                chase[0] = want;
                static uint32_t per_step[5];
                for (uint32_t c = 0; have && site->store && logged < 256 && c <= chased && c < 5; c++) {
                    guest_addr w = chase[c];
                    if (!w || !((w >= base && w < base + (size ? size : 1)) || raw == w) || per_step[c] >= 12) continue;
                    logged++;
                    per_step[c]++;
                    LOG_I(GPU, "store to 0x%llx (step %u): %s 0x%llx (%016llx) site %u, buffer 0x%llx+0x%llx (descriptor 0x%llx, %s)",
                          (unsigned long long)w, c, st->info.stage == SH_STAGE_CS ? "CS" : "PS", (unsigned long long)p->base,
                          (unsigned long long)p->hash, i, (unsigned long long)base, (unsigned long long)size,
                          (unsigned long long)raw, writeback_stores ? "written back" : "not written back");
                    for (uint32_t k = 0; k < p->site_count; k++) { /* the constants it loaded (offsets, counts) */
                        const sh_site_value *sv = &st->eval.site[k];
                        if (p->sites[k].kind == SH_SITE_SMEM && sv->smem_offset + 4 <= st->eval.smem_words)
                            LOG_I(GPU, "  constants site %u +0x%x: %08x %08x %08x %08x", k, p->pc[p->sites[k].inst] * 4,
                                  st->eval.smem_data[sv->smem_offset], st->eval.smem_data[sv->smem_offset + 1],
                                  st->eval.smem_data[sv->smem_offset + 2], st->eval.smem_data[sv->smem_offset + 3]);
                    }
                    for (uint32_t k = 0; k < p->site_count; k++) {
                        if ((p->sites[k].kind != SH_SITE_BUFFER && p->sites[k].kind != SH_SITE_IMAGE) || p->sites[k].store) continue;
                        const sh_site_value *src = &st->eval.site[k];
                        guest_addr sb = p->sites[k].kind == SH_SITE_IMAGE
                                            ? ((guest_addr)src->desc[0] << 8) | ((guest_addr)(src->desc[1] & 0xFF) << 40)
                                            : (guest_addr)src->desc[0] | ((guest_addr)(src->desc[1] & 0xFFFF) << 32);
                        const uint32_t *gd = sb ? (const uint32_t *)gmem_host(r->mem, sb, 16) : NULL;
                        LOG_I(GPU, "  it reads site %u (%s): 0x%llx (%08x %08x %08x %08x), guest %08x %08x %08x %08x", k,
                              p->sites[k].kind == SH_SITE_IMAGE ? "image" : "buffer", (unsigned long long)sb, src->desc[0],
                              src->desc[1], src->desc[2], src->desc[3], gd ? gd[0] : 0, gd ? gd[1] : 0, gd ? gd[2] : 0,
                              gd ? gd[3] : 0);
                        bool known = false;
                        for (uint32_t q = 0; q <= chased && q < 5; q++) known |= chase[q] == sb;
                        if (!known && sb && chased < 4) chase[++chased] = sb, g_store_chased = chased;
                    }
                }
            }
            /* sites that read the same buffer (a fetch per vertex attribute) share one copy of
             * the widest of their ranges: the others find it in the copy cache */
            uint64_t copy_size = site_copy_size[i] ? site_copy_size[i] : size;
            /* The part of the buffer this site uses: [lo, hi) from base. A compute store the
             * analysis bounds (sh_store_range) copies, compares and writes back that window
             * only, and the binding starts lo bytes before it so the shader's own addressing
             * lands there. A trimmed vertex read never touches its last 48 bytes (padding). */
            uint64_t lo = 0, hi = size, need = site_trimmed[i] && size > 48 ? size - 48 : size;
            bool windowed = false, checking = false;
            if (!coherent && site->store && writeback_stores && size && st->eval.vsrc && store_windows_on()) {
                uint64_t slo, shi;
                if (sh_store_range(p, &st->eval, &st->info, st->groups, i, &slo, &shi) && slo < shi) {
                    slo &= ~(uint64_t)(P->align - 1);
                    if (shi > size) shi = size;
                    if (slo < shi && shi - slo < size) {
                        checking = store_windows_on() == 2;
                        if (!checking) lo = slo, hi = need = shi, windowed = true;
                        else lo = slo, hi = shi; /* checked at apply_writebacks, copied whole */
                    }
                }
            }
            guest_addr wbase = base + (windowed ? lo : 0);
            uint64_t wsize = windowed ? hi - lo : size, wneed = windowed ? hi - lo : need;
            if(coherent)wsize=wneed=coherent_extent;
            if (size && P->held_count && held_overlaps(P, wbase, wsize)) {
                /* A read of a range a held label (immediate data) writes into, with no earlier
                 * GPU results in it: a copy of guest memory with the labels' values written in,
                 * as the GPU would see them after the release (Wobbly Life, The Mortuary
                 * Assistant: a small compute reads a 16-byte record whose upper half is the label
                 * just released, ~200 times a second; completing the batch for each one cost
                 * 0.5-0.8 s/s). PS5C_NO_HELD_PATCH=1: the caller completes the batch, as before. */
                if (!site->store && !coherent && !windowed && !checking && held_patch_on() &&
                    held_patchable(r, P, base, size)) {
                    uint64_t copy_before = g_stat_bytes;
                    if (!arena_copy_guest(r, base, size, &off)) return false;
                    held_patch(P, (uint8_t *)arena_ptr(r, off), base, size);
                    add_buffer(r, bl, off, ((size + 3) & ~3ull) + 16);
                    traffic_record(p, i, st->info.stage, stride, size, g_stat_bytes - copy_before);
                    continue;
                }
                g_stat_held_forced++;
                return draw_fail(__LINE__); /* a held label lands there first: the caller completes the batch and retries */
            }
            if (size >= RESIDENT_MIN && st->info.stage == SH_STAGE_CS && !coherent && !windowed && !checking &&
                base % P->align == 0 && (!site->store || writeback_stores) && resident_on(P)) {
                VkDeviceSize roff;
                int k = resident_bind(r, base, site->store ? size : copy_size, site->store, &roff);
                if (k >= 0) {
                    add_buffer(r, bl, 0, ((size + 3) & ~3ull) + 16);
                    bl->buf[bl->count - 1].buffer = P->res[k].buf.buffer;
                    bl->buf[bl->count - 1].offset = roff;
                    if (site->store && stored) *stored = true;
                    traffic_record(p, i, st->info.stage, stride, size, 0);
                    continue;
                }
            }
            uint64_t copy_before = g_stat_bytes;
            writeback *w = wsize ? find_writeback(P, wbase, wneed) : NULL;
            uint64_t scope_lo=0,scope_hi=0;
            bool scope_proved=false;
            for(uint32_t j=0;site->store && !windowed && j<st->footprint_count;j++)
                if(st->footprint[j].base==base && st->footprint[j].hi<=wsize) {
                    scope_lo=st->footprint[j].lo;scope_hi=st->footprint[j].hi;scope_proved=true;break;
                }
            if(w && site->store)wb_scope_add(w,base,scope_lo,scope_hi,scope_proved);
            if (w && w->res) { /* in a resident buffer that holds earlier results of the batch */
                resident_buf *e = &P->res[w->res - 1];
                VkDeviceSize range = ((windowed ? hi : size) + 3) & ~3ull;
                if (wbase - e->addr < (windowed ? lo : 0) || wbase - e->addr - (windowed ? lo : 0) + range + 16 > e->buf.size) {
                    g_stat_partial++;
                    return draw_fail(__LINE__);
                }
                add_buffer(r, bl, 0, range + 16);
                bl->buf[bl->count - 1].buffer = e->buf.buffer;
                bl->buf[bl->count - 1].offset = wbase - e->addr - (windowed ? lo : 0);
                e->used_batch = P->batch;
                if ((site->store || coherent) && stored) *stored = true;
                traffic_record(p, i, st->info.stage, stride, size, 0);
                continue;
            }
            if (w && w->offset + (wbase - w->addr) >= (windowed ? lo : 0)) {
                VkDeviceSize at = w->offset + (wbase - w->addr) - (windowed ? lo : 0);
                VkDeviceSize range = ((windowed ? hi : size) + 3) & ~3ull;
                /* The A/B control may reserve 256 MiB while this batch is
                 * limited to 128 MiB. Preserve the old binding boundary. */
                if(at>P->arena_limit || P->arena_limit-at<16)return false;
                if(range>P->arena_limit-at-16)range=P->arena_limit-at-16;
                add_buffer(r, bl, at, range + 16);
                if (w->slot != work_slot(P)) { /* results of the batch before, in its own arena */
                    bl->buf[bl->count - 1].buffer = P->work_slots[w->slot].arena.buffer;
                    P->cross_slot = true;
                }
                if ((site->store || coherent) && stored) *stored = true;
                traffic_record(p, i, st->info.stage, stride, size, 0);
                continue;
            }
            if (w) { /* inside earlier results, but bound in place it would start before the copy */
                g_stat_partial++;
                return draw_fail(__LINE__);
            }
            bool partial = false;
            for (uint32_t k = 0; wsize && k < wb_earlier; k++)
                partial |= wbase < P->wb[k].addr + P->wb[k].size && P->wb[k].addr < wbase + wneed;
            if (partial) {
                static int log_partial = -1;
                static uint32_t logged_partial;
                if (log_partial < 0) log_partial = getenv("PS5C_LOG_PARTIAL") != NULL;
                if (log_partial && logged_partial < 60 && wsize >= 4096 && !(g_stat_partial % 37)) {
                    logged_partial++;
                    for (uint32_t k = 0; k < wb_earlier; k++)
                        if (wbase < P->wb[k].addr + P->wb[k].size && P->wb[k].addr < wbase + wneed)
                            LOG_I(GPU, "partial read: stage %d site %u [0x%llx +0x%llx) need 0x%llx stride %u over writeback %u/%u [0x%llx +0x%llx)%s",
                                  (int)st->info.stage, i, (unsigned long long)wbase, (unsigned long long)wsize,
                                  (unsigned long long)wneed, stride, k, wb_earlier, (unsigned long long)P->wb[k].addr,
                                  (unsigned long long)P->wb[k].size, P->wb[k].res ? " resident" : "");
                }
                /* earlier GPU results cover part of the range: guest memory there is not current yet.
                 * A read takes them from the arena; a store or a window has the caller complete the batch and retry */
                g_stat_partial++;
                /* A draw waits too: the copy from the arena carries every byte of the earlier binding, also
                 * those the shader left alone, over what the CPU wrote since (Minecraft's interface vertices
                 * share a pool with a compute output: text and buttons came out scrambled).
                 * PS5C_DRAW_PATCH=1 = patched copies for draws again (Crash Bandicoot 4: fewer waits). */
                static int draw_patch = -1;
                if (draw_patch < 0) draw_patch = getenv("PS5C_DRAW_PATCH") != NULL;
                if (site->store || coherent || windowed || (st->info.stage != SH_STAGE_CS && !draw_patch)) {
                    static uint32_t logged_sync;
                    if (log_partial && logged_sync < 40) {
                        logged_sync++;
                        for (uint32_t k = 0; k < wb_earlier; k++)
                            if (wbase < P->wb[k].addr + P->wb[k].size && P->wb[k].addr < wbase + wneed)
                                LOG_I(GPU, "sync for a partial %s%s%s: stage %d site %u [0x%llx +0x%llx) size 0x%llx over writeback %u/%u [0x%llx +0x%llx)%s",
                                      site->store ? "store" : "read", coherent ? " coherent" : "", windowed ? " windowed" : "",
                                      (int)st->info.stage, i, (unsigned long long)wbase, (unsigned long long)wsize,
                                      (unsigned long long)size, k, wb_earlier, (unsigned long long)P->wb[k].addr,
                                      (unsigned long long)P->wb[k].size, P->wb[k].res ? " resident" : "");
                    }
                    return draw_fail(__LINE__);
                }
                /* the sites of a draw read the same vertex slice (Minecraft: four or five each): one patched
                 * copy serves them while no later result of the batch lands in the range */
                bool shared_patch = false;
                uint32_t copies = P->copy_count < MAX_COPY_CACHE ? P->copy_count : MAX_COPY_CACHE;
                for (uint32_t q = 0; q < copies && !shared_patch; q++) {
                    const copy_entry *c = &P->copies[q];
                    if (!c->patched || base < c->addr || base + size > c->addr + c->size ||
                        ((base - c->addr) & (P->align - 1)))
                        continue;
                    bool later = false;
                    for (uint32_t k = c->patched - 1; k < wb_earlier; k++)
                        later |= base < P->wb[k].addr + P->wb[k].size && P->wb[k].addr < base + size;
                    if (later) continue;
                    off = c->offset + (base - c->addr);
                    shared_patch = true;
                    g_stat_patch_shared++;
                }
                if (!shared_patch) {
                    if (!arena_copy_patched(r, base, size, wb_earlier, &off)) {
                        static uint32_t logged_patch;
                        if (log_partial && logged_patch < 40) {
                            logged_patch++;
                            uint32_t over = 0;
                            for (uint32_t k = 0; k < wb_earlier; k++)
                                over += base < P->wb[k].addr + P->wb[k].size && P->wb[k].addr < base + size;
                            LOG_I(GPU, "sync for a patched read that failed: stage %d site %u [0x%llx +0x%llx), %u of %u writebacks overlap, arena %llu/%llu MiB",
                                  (int)st->info.stage, i, (unsigned long long)base, (unsigned long long)size, over, wb_earlier,
                                  (unsigned long long)(P->arena_used >> 20), (unsigned long long)(P->arena_limit >> 20));
                            for (uint32_t k = 0, shown = 0; k < wb_earlier && shown < 10; k++)
                                if (base < P->wb[k].addr + P->wb[k].size && P->wb[k].addr < base + size) {
                                    shown++;
                                    LOG_I(GPU, "  writeback %u [0x%llx +0x%llx)%s", k, (unsigned long long)P->wb[k].addr,
                                          (unsigned long long)P->wb[k].size, P->wb[k].res ? " resident" : "");
                                }
                        }
                        return draw_fail(__LINE__);
                    }
                    copy_entry *c = &P->copies[P->copy_count < MAX_COPY_CACHE ? P->copy_count++ : P->copy_next++ % MAX_COPY_CACHE];
                    c->addr = base;
                    c->size = size;
                    c->offset = off;
                    c->patched = wb_earlier + 1;
                }
                add_buffer(r, bl, off, ((size + 3) & ~3ull) + 16);
                traffic_record(p, i, st->info.stage, stride, size, g_stat_bytes - copy_before);
                continue;
            }
            if (!site->store && !coherent && !windowed && size && phys_import_enabled(r)) {
                VkDeviceSize ioff, irange = ((size + 3) & ~3ull) + 16;
                bool shiftable = st->shapes[i].buffer_length == SH_BUFFER_SHIFTED && !st->shapes[i].address_map;
                VkBuffer ibuf = phys_import_bind(r, base, irange, P->align, shiftable, &ioff, &g_pimp_extra[i]);
                if (ibuf) {
                    add_buffer(r, bl, 0, irange + g_pimp_extra[i]);
                    bl->buf[bl->count - 1].buffer = ibuf;
                    bl->buf[bl->count - 1].offset = ioff;
                    traffic_record(p, i, st->info.stage, stride, size, 0);
                    continue;
                }
            }
            if (st->info.stage != SH_STAGE_CS && !p->has_stores && !site->store && !coherent && !windowed && size &&
                P->vc_on >= 0) {
                /* sites of one buffer share the widest read's copy (site_copy_size): a narrower site binds
                 * its own range of that same entry (Minecraft's chunk positions: 117 MiB/s copied otherwise) */
                VkBuffer vbuf;
                uint64_t widest = copy_size > size ? copy_size : size;
                VkDeviceSize voff, vrange = ((size + 3) & ~3ull) + 16;
                if (vertex_cache_bind(r, base, widest, ((widest + 3) & ~3ull) + 16, &vbuf, &voff, NULL)) {
                    add_buffer(r, bl, 0, vrange);
                    bl->buf[bl->count - 1].buffer = vbuf;
                    bl->buf[bl->count - 1].offset = voff;
                    traffic_record(p, i, st->info.stage, stride, size, g_stat_bytes - copy_before);
                    continue;
                }
            }
            if (size >= READONLY_CACHE_MIN && copy_size <= READONLY_CACHE_MAX &&
                st->info.stage != SH_STAGE_CS && !p->has_stores && !site->store && readonly_cache_enabled(P)) {
                VkDeviceSize cached_offset=0;
                VkBuffer cached = readonly_snapshot(r, base, copy_size, &cached_offset);
                if (cached) {
                    add_buffer(r, bl, 0, ((size + 3) & ~3ull) + 16);
                    bl->buf[bl->count - 1].buffer = cached;
                    bl->buf[bl->count - 1].offset = cached_offset;
                    traffic_record(p, i, st->info.stage, stride, size, g_stat_bytes - copy_before);
                    continue;
                }
            }
            if (windowed && P->arena_used < lo) P->arena_used = lo; /* the binding starts lo bytes before the window */
            g_copy_tag = (site->store || coherent) ? 3 : 2;
            bool copied_ = (site->store || coherent) ? arena_copy_guest(r, wbase, wsize, &off) : arena_copy_guest_shared(r, base, copy_size, &off);
            g_copy_tag = 0;
            if (!copied_) return false;
            add_buffer(r, bl, off - (windowed ? lo : 0), (((windowed ? hi : size) + 3) & ~3ull) + 16);
            /* This path just made an immutable CPU snapshot. Mutable inputs,
             * GPU results and reduced windows retain the cached host arena.
             * Never compare/read back from the device-local host mapping. */
            bool local_read=P->work->arena_local && !site->store && !coherent && !windowed && copy_size==size;
            if(local_read) {
                if(st->info.stage==SH_STAGE_VS)local_read=!p->has_stores;
                else local_read=st->info.stage==SH_STAGE_CS && local_compute_enabled() &&
                    local_compute_read_safe(r,st,i,site_base,site_size,copy_size);
            }
            if(local_read) {
                work_resources *wr=P->work;
                VkDeviceSize bytes=((copy_size+3)&~3ull)+16;
                bool copied=false;
                for(uint32_t c=0;c<wr->local_copy_count;c++)
                    if(wr->local_copies[c].offset==off && wr->local_copies[c].size==bytes){copied=true;break;}
                if(!copied && wr->local_copy_count<PS5C_ARRAY_COUNT(wr->local_copies) &&
                   off<=wr->other_arena.size && bytes<=wr->other_arena.size-off) {
                    memcpy((uint8_t*)wr->other_arena.mapped+off,arena_ptr(r,off),(size_t)bytes);
                    wr->local_copies[wr->local_copy_count].offset=off;
                    wr->local_copies[wr->local_copy_count++].size=bytes;copied=true;
                }
                if(copied) {
                    bl->buf[bl->count-1].buffer=wr->other_arena.buffer;
                    if(st->info.stage==SH_STAGE_CS) {
                        static uint32_t reports;
                        if(reports++<8)LOG_I(GPU,"immutable compute input in local arena: shader %016llx site %u, %llu bytes",
                            (unsigned long long)p->hash,i,(unsigned long long)bytes);
                    }
                }
            }
            traffic_record(p, i, st->info.stage, stride, wsize, g_stat_bytes - copy_before);
            if ((site->store || coherent) && writeback_stores && size) {
                if (stored) *stored = true;
                if (P->wb_count < MAX_WRITEBACKS) {
                    writeback *nw = &P->wb[P->wb_count];
                    memset(nw,0,sizeof(*nw));
                    nw->offset = off;
                    nw->slot = (uint8_t)work_slot(P);
                    nw->addr = wbase;
                    nw->size = wsize;
                    nw->orig = writeback_original(P,wsize,&nw->orig_pooled);
                    if (!nw->orig) { P->writeback_oom = true; return false; }
                    memcpy(nw->orig, arena_ptr(r, off), wsize);
                    nw->check_lo = checking ? lo : 0;
                    nw->check_hi = checking ? hi : 0;
                    nw->check_hash = p->hash;
                    nw->check_site = i;
                    if(scope_proved) {
                        nw->scope_lo=scope_lo;nw->scope_hi=scope_hi;nw->scope_mode=(uint8_t)footprint_mode();
                    }
                    P->wb_count++;
                } else if (overflow) {
                    *overflow = true;
                }
            }
        } else {
            static const char *log_store = (const char *)1; /* PS5C_LOG_STORE_ADDR, read once */
            /* Read-only POPS image_load also requires a storage binding. Treat
             * its image conservatively as writable for GENERAL layout/barriers;
             * the translated instruction remains a read. */
            sh_site storage_site=*site;
            storage_site.store=sh_site_storage(site,&st->shapes[i]);
            site=&storage_site;
            if (log_store == (const char *)1) log_store = getenv("PS5C_LOG_STORE_ADDR");
            if (site->store && log_store) { /* image stores near that address too */
                static uint32_t logged;
                const char *e = log_store;
                guest_addr want = !strcmp(e, "break") ? runtime_debug_addr() : strtoull(e, NULL, 16);
                guest_addr ib = ((guest_addr)v->desc[0] << 8) | ((guest_addr)(v->desc[1] & 0xFF) << 40);
                for (uint32_t c = 1; c <= g_store_chased && c < 5; c++) /* or a buffer an earlier match reads */
                    if (ib <= g_store_chase[c] && g_store_chase[c] - ib < 0x100000) want = g_store_chase[c];
                if (want && ib <= want && want - ib < 0x100000 && logged++ < 64)
                    LOG_I(GPU, "image store near 0x%llx: shader 0x%llx (%016llx) site %u, image 0x%llx (T# %08x %08x %08x %08x)",
                          (unsigned long long)want, (unsigned long long)p->base, (unsigned long long)p->hash, i,
                          (unsigned long long)ib, v->desc[0], v->desc[1], v->desc[2], v->desc[3]);
            }
            if(r->lod_stats_enabled && v->valid && !site->store)
                gpu_lod_note(&r->lod_stats,v->desc);
            uint32_t mip_desc[8], mip_source, mip_literal;
            const uint32_t *desc = v->desc;
            if (v->valid && st->shapes[i].img_type == 9 &&
                sh_image_mip_source(p, &st->info, i, &mip_source, &mip_literal)) {
                uint32_t lod = mip_source == RDNA2_OP_LITERAL ? mip_literal : st->eval.sgpr_init[mip_source];
                if (!sh_image_mip_desc(v->desc, lod, mip_desc)) {
                    static uint32_t logged;
                    if (logged++ < 8)
                        LOG_W(GPU, "shader 0x%llx site %u: mip %u outside the 2D descriptor, skipping",
                              (unsigned long long)p->base, i, lod);
                    bl->missing = true;
                    return false;
                }
                desc = mip_desc;
                static int trace_mip = -1;
                if (trace_mip < 0) trace_mip = getenv("PS5C_TRACE_IMAGE_MIP") != NULL;
                if (trace_mip)
                    LOG_I(GPU, "shader 0x%llx site %u: binds mip %u (relative %u), T# %08x %08x %08x %08x",
                          (unsigned long long)p->base, i, (desc[3] >> 12) & 15, lod,
                          desc[0], desc[1], desc[2], desc[3]);
            }
            /* MIN_LOD (T# word 1, bits 19:8, 4.8 fixed point): levels below it are not resident yet
             * (No Man's Sky streams its material arrays: only the small levels are in memory, the
             * rest unmapped or zero). PS5C_MIN_LOD=1 (experiment): the first level it allows is the
             * image, as BASE_LEVEL would be. With it the save's load stayed in the galaxy jump for
             * 10 minutes in one run (it loads in ~3 without), so it stays off until understood. */
            uint32_t lod_desc[8];
            static int no_min_lod = -1;
            if (no_min_lod < 0) no_min_lod = getenv("PS5C_MIN_LOD") == NULL;
            if (v->valid && !site->store && !no_min_lod && desc != lod_desc) {
                uint32_t min_level = ((desc[1] >> 8) & 0xFFF) >> 8, base = (desc[3] >> 12) & 15, last = (desc[3] >> 16) & 15;
                if (min_level > last) min_level = last;
                if (min_level > base && base <= last) {
                    memcpy(lod_desc, desc, sizeof(lod_desc));
                    lod_desc[3] = (lod_desc[3] & ~0xF000u) | (min_level << 12);
                    desc = lod_desc;
                }
            }
            vk_image *img = v->valid && !site->store ? resident_mips(r,desc) : NULL;
            if(!img && v->valid) img=texture(r,desc);
            if(img && site->store && (img->aspect&VK_IMAGE_ASPECT_DEPTH_BIT)) {
                VkFormat pair=depth_integer_pair(img->format);
                if(pair!=VK_FORMAT_UNDEFINED) img=depth_as_color(r,img,pair);
            }
            if (img && !site->store) img = mip_chain(r, img, desc);
            /* The guest can leave an incompatible placeholder T# in an
             * inactive 64-bit atomic branch. Keep the module well typed, but
             * never bind that 32-bit image to an R64ui declaration. Use the
             * existing typed missing-resource path; valid 64-bit sites remain
             * bound to their real images. An active unsupported access is
             * diagnosed rather than corrupting the unrelated placeholder. */
            if(img && site->store && st->shapes[i].buf_format==SH_IMG_ATOMIC_UINT64 && vk_format_bytes(img->format)!=8)
                img=NULL;
            if(img && !site->store && (((desc[1]>>20)&0x1ff)==128 || ((desc[1]>>20)&0x1ff)==129) &&
               (img->format==VK_FORMAT_R8_UNORM || img->format==VK_FORMAT_R8G8_UNORM)) img=vk_image_srgb(r,img);
            if (r->image_cache_pressure) return false; /* retry after submitting, not a black stand-in */
            VkFormat view_fmt = img ? image_view_format(img, desc, site->store, st->shapes[i].img_kind) : VK_FORMAT_UNDEFINED;
            /* the translation packs this format itself (SH_IMG_PACKED_R11G11B10) */
            if (img && site->store && (st->shapes[i].buf_format == SH_IMG_PACKED_R11G11B10 ||
                                      st->shapes[i].buf_format == SH_IMG_ATOMIC_FLOAT32 ||
                                      st->shapes[i].buf_format == SH_IMG_ATOMIC_UINT32) && vk_format_bytes(img->format) == 4)
                view_fmt = VK_FORMAT_R32_UINT;
            if(img && site->store && st->shapes[i].buf_format==SH_IMG_ATOMIC_UINT64 && vk_format_bytes(img->format)==8)
                view_fmt=VK_FORMAT_R64_UINT;
            static int log_image_types = -1;
            if (log_image_types < 0) log_image_types = getenv("PS5C_LOG_IMAGE_TYPES") != NULL;
            if (log_image_types) {
                static uint32_t logged;
                bool uint_view = view_fmt == VK_FORMAT_R8_UINT || view_fmt == VK_FORMAT_R16_UINT ||
                                 view_fmt == VK_FORMAT_R32_UINT || view_fmt == VK_FORMAT_R8G8B8A8_UINT ||
                                 view_fmt == VK_FORMAT_R32G32_UINT || view_fmt == VK_FORMAT_R16G16B16A16_UINT ||
                                 view_fmt == VK_FORMAT_R32G32B32A32_UINT;
                if (st->shapes[i].img_kind == 1 && !uint_view && logged++ < 24)
                    LOG_I(GPU, "image numeric mismatch: stage %u shader 0x%llx site %u store %d guest 0x%llx "
                               "image fmt %d view fmt %d T# %08x %08x %08x %08x",
                          st->info.stage, (unsigned long long)p->base, i, (int)site->store,
                          (unsigned long long)(img ? img->addr : (guest_addr)desc[0] << 8),
                          img ? (int)img->format : 0, (int)view_fmt, desc[0], desc[1], desc[2], desc[3]);
            }
            bool stencil_read = img && !site->store && vk_format_has_stencil(img->format) &&
                                img_format((desc[1] >> 20) & 0x1FF, &(uint32_t){0}, &(uint32_t){0}) == VK_FORMAT_R8_UINT;
            if (stencil_read) view_fmt = img->format;
            if (img && view_fmt != img->format && !img->mutable_fmt) {
                if (img->aspect == VK_IMAGE_ASPECT_COLOR_BIT && !P->promote) {
                    P->promote = img; /* the caller completes the batch and retries */
                    return false;
                }
                view_fmt = img->format; /* already one pending: as before, once */
            }
            if (img && site->store && !img->storage && view_fmt == img->format) img = NULL; /* format not writable here */
            if (trace_image(img)) {
                LOG_I(GPU, "trace: %s 0x%llx (%016llx) site %u %s image 0x%llx fmt %d (T# fmt %u, type %u)%s "
                           "[#%u %ux%u x%u, seq %llu; T# %08x %08x %08x %08x %08x]",
                      st->info.stage == SH_STAGE_CS ? "CS" : st->info.stage == SH_STAGE_PS ? "PS" : "VS",
                      (unsigned long long)p->base, (unsigned long long)p->hash, i, site->store ? "WRITES" : "reads",
                      (unsigned long long)(img ? img->addr : (guest_addr)v->desc[0] << 8), img ? (int)img->format : 0,
                      (v->desc[1] >> 20) & 0x1FF, v->desc[3] >> 28, img ? "" : " MISSING",
                      img ? (uint32_t)(img - r->images) : 0, img ? img->width : 0, img ? img->height : 0, img ? img->layers : 0,
                      img ? (unsigned long long)img->written_seq : 0ull, v->desc[1], v->desc[2], v->desc[3], v->desc[4], v->desc[5]);
                if (img && img->width == 1 && img->height == 1 && vk_format_bytes(img->format) == 4) {
                    uint32_t pixel = 0;
                    bool readable = gmem_read(r->mem,img->addr,&pixel,sizeof(pixel));
                    LOG_I(GPU,"trace: single-pixel source 0x%llx readable %u bytes %08x",
                          (unsigned long long)img->addr,(unsigned)readable,pixel);
                }
            }
            bool placeholder=false;
            if (!img) {
                /* once per shader and site (a cap over all of them hid most) */
                static uint64_t seen[256];
                static uint32_t seen_count;
                uint64_t key = p->hash ^ ((uint64_t)i << 56);
                bool known = false;
                for (uint32_t k = 0; k < seen_count && !known; k++) known = seen[k] == key;
                if (!known && seen_count < PS5C_ARRAY_COUNT(seen)) {
                    seen[seen_count++] = key;
                    LOG_W(GPU, "shader 0x%llx (%016llx) site %u: no image, black stand-in (valid %d, T# %08x %08x %08x %08x)",
                          (unsigned long long)p->base, (unsigned long long)p->hash, i, (int)v->valid, v->desc[0], v->desc[1],
                          v->desc[2], v->desc[3]);
                }
                img = null_image(r,site->store,&st->shapes[i],p->ops[site->inst]==SH_OP_IMAGE_ATOMIC);
                /* The original descriptor may still describe depth, a mip,
                 * or a different number type. It must not create a view of
                 * that format on the private zero/discard image. */
                placeholder=true;stencil_read=false;view_fmt=img?img->format:VK_FORMAT_UNDEFINED;
            }
            if (!placeholder) census_image(img, st->info.stage, p->ops[site->inst]);
            if (census_on() && img && !placeholder && census_large(img)) { /* which shaders read large images, how */
                static uint64_t seen[512];
                static uint32_t nseen;
                uint64_t key = p->hash ^ ((uint64_t)i << 56);
                bool known = false;
                for (uint32_t q = 0; q < nseen; q++) known |= seen[q] == key;
                if (!known && nseen < 512) {
                    seen[nseen++] = key;
                    LOG_I(GPU, "scale census: stage %u shader %016llx site %u %s %s image 0x%llx %ux%u host %ux%u map %u type %u",
                          st->info.stage, (unsigned long long)p->hash, i, p->insts[site->inst].name,
                          site->store ? "store" : "read", (unsigned long long)img->addr, img->width, img->height, vk_img_w(img),
                          vk_img_h(img), st->shapes[i].address_map, st->shapes[i].img_type);
                }
            }
            if (r->rt_scale && !st->shapes[i].address_map && SH_U_SITE + i < st->eval.smem_words) {
                /* PS5C_RT_SCALE: host / guest of a target drawn smaller, for its integer coordinates */
                float f = img && !placeholder && img->ext_w ? (float)img->ext_w / (float)img->width : 0.0f;
                memcpy(&st->eval.smem_data[SH_U_SITE + i], &f, 4);
                if (!st->images_only) memcpy((uint32_t *)arena_ptr(r, st->u_off) + SH_U_SITE + i, &f, 4); /* the uploaded copy */
            }
            uint32_t k = bl->count++;
            memset(&bl->w[k], 0, sizeof(bl->w[k]));
            bl->w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            bl->w[k].dstBinding = k;
            bl->w[k].descriptorCount = 1;
            bl->w[k].descriptorType = site->store ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bl->bound_image[k] = img;
            bl->img[k].sampler = site->store ? VK_NULL_HANDLE :
                get_sampler(r, v->sampler, stencil_read ? VK_FORMAT_R8_UINT : view_fmt ? view_fmt : img ? img->format : VK_FORMAT_R8G8B8A8_UNORM);
            if(!site->store && !bl->img[k].sampler) {
                if(r->image_cache_pressure)return false;
                static uint32_t reported;
                if(reported++<8)LOG_E(GPU,"shader 0x%llx site %u: sampler unavailable (%u/%u cached); resource binding stopped",
                    (unsigned long long)p->base,i,P->sampler_count,MAX_SAMPLERS);
                bl->missing=true;
                return false;
            }
            /* the shader declares cube and array descriptors as 2D arrays */
            bool arrayed = sh_site_arrayed(p, i, st->shapes[i].img_type);
            bl->img[k].imageView = !img      ? VK_NULL_HANDLE
                                   : stencil_read && !arrayed ? stencil_view(r, img)
                                   : v->valid && !placeholder ? layer_view(r, img, desc, arrayed, view_fmt ? view_fmt : img->format, site->store)
                                   : arrayed  ? img->array_view
                                              : img->view;
            bl->img[k].imageLayout = site->store ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if(!bl->img[k].imageView) {bl->missing=true;return false;}
            { /* Inspect the actual sampled image/view, rather than only its guest address. */
                static int have = -1;
                static uint64_t want;
                static uint32_t logged;
                if (have < 0) {
                    const char *setting = getenv("PS5C_LOG_IMAGE_PS");
                    have = setting != NULL;
                    want = setting ? strtoull(setting, NULL, 16) : 0;
                }
                if (have && st->info.stage == SH_STAGE_PS && !site->store && img && logged < 96 &&
                    (p->base == want || p->hash == want)) {
                    logged++;
                    LOG_I(GPU, "sample binding PS 0x%llx site %u binding %u: guest 0x%llx %ux%u layers %u fmt %d view fmt %d"
                               " image %llx view %llx seq %llu gpu %d sampler %08x %08x %08x %08x sel %u%u%u%u T4-7 %08x %08x %08x %08x",
                          (unsigned long long)p->base, i, k, (unsigned long long)img->addr, img->width, img->height,
                          img->layers, (int)img->format, (int)view_fmt, (unsigned long long)(uintptr_t)img->image,
                          (unsigned long long)(uintptr_t)bl->img[k].imageView, (unsigned long long)img->written_seq,
                          (int)img->gpu_written, v->sampler[0], v->sampler[1], v->sampler[2], v->sampler[3],
                          st->shapes[i].dst_sel[0], st->shapes[i].dst_sel[1], st->shapes[i].dst_sel[2], st->shapes[i].dst_sel[3],
                          desc[4], desc[5], desc[6], desc[7]);
                }
            }
            bl->w[k].pImageInfo = &bl->img[k];
            if (img && site->store) {
                bl->written[bl->written_count++] = img;
                continue;
            }
            if (!img) {
                bl->missing = true;
                return false;
            }
            bl->sampled[bl->sampled_count++] = img;
        }
    }
    if(!st->images_only) {
        uint32_t binding=first_binding+1; /* stage uniform buffer */
        uint32_t *uniforms=arena_ptr(r,st->u_off);
        for(uint32_t s=0;s<p->site_count;s++) {
            if(p->sites[s].kind==SH_SITE_SMEM && !st->shapes[s].address_map)continue;
            if(st->shapes[s].buffer_length==SH_BUFFER_SHIFTED && !st->shapes[s].address_map && p->sites[s].kind==SH_SITE_BUFFER) {
                if(SH_U_SITE+s>=st->eval.smem_words)return false;
                uniforms[SH_U_SITE+s]=(uint32_t)vertex_share_shift(p,&st->eval,s,NULL)+(g_pimp.on>0?g_pimp_extra[s]:0);
            } else if(st->shapes[s].buffer_length==SH_BUFFER_LENGTH && !st->shapes[s].address_map &&
               (p->sites[s].kind==SH_SITE_BUFFER || p->sites[s].kind==SH_SITE_SBUFFER)) {
                if(binding>=bl->count || SH_U_SITE+s>=st->eval.smem_words)return false;
                uint64_t words=bl->buf[binding].range/4;
                if(words>UINT32_MAX)return false;
                uniforms[SH_U_SITE+s]=(uint32_t)words;
            }
            binding++;
        }
    }
    return true;
}

static VkDescriptorSet alloc_set(renderer *r, VkDescriptorSetLayout dsl, bool update_after_bind) {
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    work_resources *w = r->pipes->work;
    if (update_after_bind && !w->pool_uab &&
        !create_pool_flags(r, 1, VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, &w->pool_uab)) {
        w->pool_uab = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    ai.descriptorPool = update_after_bind ? w->pool_uab : w->pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &dsl;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult result=vkAllocateDescriptorSets(r->device, &ai, &set);
    static int extra_pools = -1;
    if (extra_pools < 0) extra_pools = getenv("PS5C_EXTRA_POOLS") != NULL;
    if (extra_pools && !update_after_bind && (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL)) {
        /* the pool is full: the batch continues in the next one */
        work_resources *w=r->pipes->work;
        if(w->extra_used==w->extra_count && w->extra_count<WORK_EXTRA_POOLS &&
           create_pool(r,1,&w->pool_extra[w->extra_count])) {
            w->extra_count++;
            LOG_I(GPU,"descriptor pools of a batch: %u",w->extra_count+1);
        }
        if(w->extra_used<w->extra_count) {
            w->pool=ai.descriptorPool=w->pool_extra[w->extra_used++];
            result=vkAllocateDescriptorSets(r->device, &ai, &set);
        }
    }
    if (result != VK_SUCCESS) {g_stat_pool_failed++;g_stat_pool_result=result;return VK_NULL_HANDLE;}
    return set;
}

/* ---- Stage setup ---- */

typedef struct linear_image_copy {
    vk_image *image;
    guest_addr addr;
    VkDeviceSize offset;
} linear_image_copy;
typedef struct linear_image_plan {
    linear_image_copy copies[MAX_WRITEBACKS];
    uint32_t count;
} linear_image_plan;

/* Native guests can poll a linear UAV through ordinary CPU loads. Keep an
 * arena readback in the same batch as the store, using the existing originals
 * and completion/label machinery. Do not publish speculative BVH replays. */
static bool prepare_linear_image_copies(renderer *r,const stage_state *st,const bind_list *bl,linear_image_plan *plan) {
    plan->count=0;
    static int enabled=-1;
    if(enabled<0){const char *s=getenv("PS5C_LINEAR_IMAGE_WB");enabled=s && !strcmp(s,"1");}
    vk_pipelines *P=r->pipes;
    if(!enabled || P->bvh_replay || P->bvh_table)return true;
    uint32_t binding=1; /* uniform storage buffer */
    for(uint32_t s=0;s<st->prog->site_count;s++) {
        const sh_site *site=&st->prog->sites[s];
        if(site->kind==SH_SITE_SMEM && !st->shapes[s].address_map)continue;
        uint32_t b=binding++;
        const sh_site_value *v=&st->eval.site[s];
        if(site->kind!=SH_SITE_IMAGE || !site->store || !v->valid || v->desc_unknown)continue;
        const uint32_t *d=v->desc;
        uint32_t fmt=(d[1]>>20)&0x1ff;
        uint32_t type=d[3]>>28;
        /* A single-layer 2D array has the same tightly packed CPU layout.
         * The host-image layer check below still excludes multi-layer views. */
        if((type!=9 && type!=13) || ((d[3]>>20)&31) || ((d[3]>>12)&255) ||
           (fmt!=20 && fmt!=21 && fmt!=22))continue;
        if(b>=bl->count)return false;
        vk_image *image=bl->bound_image[b];
        if(!image || image->depth || image->layers!=1 || image->levels!=1 ||
           image->aspect!=VK_IMAGE_ASPECT_COLOR_BIT ||
           (image->format!=VK_FORMAT_R32_SFLOAT && image->format!=VK_FORMAT_R32_UINT && image->format!=VK_FORMAT_R32_SINT))continue;
        uint64_t bytes=(uint64_t)image->width*image->height*4;
        if(!bytes || bytes>(4ull<<20))continue; /* bounded prototype, single tightly packed level */
        guest_addr addr=((guest_addr)d[0]<<8)|((guest_addr)(d[1]&255)<<40);
        bool seen=false;
        for(uint32_t i=0;i<plan->count;i++)seen|=plan->copies[i].image==image && plan->copies[i].addr==addr;
        if(seen)continue;
        if(plan->count==MAX_WRITEBACKS)return false;
        writeback *prior=find_writeback(P,addr,bytes);
        if(prior && (prior->res || prior->slot!=work_slot(P)))return false; /* resident or another slot's arena: not this bounded path */
        VkDeviceSize offset;
        if(prior) {
            wb_scope_add(prior,addr,0,bytes,true);
            offset=prior->offset+(addr-prior->addr);
        }
        else {
            if(P->wb_count==MAX_WRITEBACKS || !arena_alloc(r,bytes,&offset))return false;
            /* gmem_read holds the mapping lock for the copy, not just lookup. */
            if(!gmem_read(r->mem,addr,arena_ptr(r,offset),bytes))return false;
            writeback *w=&P->wb[P->wb_count];memset(w,0,sizeof(*w));
            w->orig=writeback_original(P,bytes,&w->orig_pooled);
            if(!w->orig){P->writeback_oom=true;return false;}
            memcpy(w->orig,arena_ptr(r,offset),(size_t)bytes);
            w->offset=offset;w->addr=addr;w->size=bytes;w->slot=(uint8_t)work_slot(P);P->wb_count++;
        }
        plan->copies[plan->count++]=(linear_image_copy){image,addr,offset};
        static guest_addr reported[64];static uint32_t reports;
        bool logged=false;for(uint32_t i=0;i<reports;i++)logged|=reported[i]==addr;
        if(!logged && reports<64) {
            reported[reports++]=addr;
            LOG_I(GPU,"linear image writeback: CS 0x%llx -> 0x%llx %ux%u (%llu bytes)",
                (unsigned long long)st->prog->base,(unsigned long long)addr,image->width,image->height,(unsigned long long)bytes);
        }
    }
    return true;
}

static void record_linear_image_copies(renderer *r,const linear_image_plan *plan) {
    for(uint32_t i=0;i<plan->count;i++) {
        const linear_image_copy *copy=&plan->copies[i];vk_image *image=copy->image;
        if(image->ext_w)continue; /* PS5C_RT_SCALE: a smaller target has no linear guest copy */
        vk_barrier(r->cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region={.bufferOffset=copy->offset,
            .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={image->width,image->height,1}};
        census_copy(image, 5);
        vkCmdCopyImageToBuffer(r->cmd,image->image,image->layout,r->pipes->work->arena.buffer,1,&region);
        vk_barrier(r->cmd,image,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    if(plan->count) {
        VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask=VK_ACCESS_HOST_READ_BIT|VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
        vkCmdPipelineBarrier(r->cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0,1,&barrier,0,NULL,0,NULL);
    }
}

static void init_sgprs(sh_eval *e, const gpu *g, uint32_t first_sgpr, uint32_t user_reg, uint32_t count) {
    memset(e->sgpr_init, 0, sizeof(e->sgpr_init));
    for (uint32_t i = 0; i < count && first_sgpr + i < SH_SGPRS; i++) e->sgpr_init[first_sgpr + i] = gpu_reg(g, user_reg + i);
    e->sgpr_init[126] = e->sgpr_init[127] = 0xFFFFFFFFu; /* exec */
}

/* An evaluation of the same program from the same initial SGPRs in this epoch
 * (memory the CPU writes holds still within one): draws of a pass often
 * repeat a shader with the same user data. */
/* Global loads (eval_global_site) find their window through the mapping list, not guest_host. */
static bool eval_reads_globals(const sh_program *p) {
    for (uint32_t i = 0; i < p->site_count; i++) {
        uint16_t op = p->ops[p->sites[i].inst];
        if (op == SH_OP_GLOBAL_LOAD || op == SH_OP_GLOBAL_STORE || op == SH_OP_GLOBAL_ATOMIC) return true;
    }
    return false;
}

static inline uint32_t eval_page_bit(guest_addr page) { return (uint32_t)((page ^ (page >> 16)) & (EVAL_PAGE_BITS - 1u)); }

/* Records what cached entry `c` (index `i`) read in the evaluation page filter. */
static void eval_pages_mark(vk_pipelines *P, const eval_entry *c, uint32_t i) {
    if (P->eval_pages_all) return;
    if (c->read.overflow) {
        if (P->eval_overflow_count < EVAL_OVERFLOW) P->eval_overflow[P->eval_overflow_count++] = (uint16_t)i;
        else P->eval_pages_all = true;
        return;
    }
    for (uint32_t k = 0; k < c->read.count; k++) {
        guest_addr lo = c->read.start[k] >> 16, hi = (c->read.end[k] + 0xFFFF) >> 16;
        if (hi - lo > 64) { P->eval_pages_all = true; return; }
        for (guest_addr pg = lo; pg < hi; pg++) {
            uint32_t b = eval_page_bit(pg);
            P->eval_pages[b / 64] |= 1ull << (b % 64);
        }
    }
}

/* The filter again from the live entries (dropped ones leave stale bits). */
static void eval_pages_rebuild(vk_pipelines *P) {
    memset(P->eval_pages, 0, sizeof(P->eval_pages));
    P->eval_overflow_count = 0;
    P->eval_pages_all = false;
    P->eval_pages_epoch = P->epoch;
    for (uint32_t i = 0; P->evals && i < EVAL_CACHE && !P->eval_pages_all; i++)
        if (P->evals[i].key) eval_pages_mark(P, &P->evals[i], i);
}

/* The command processor wrote guest memory: evaluations that read it go.
 * The page filter skips the scan of the whole cache for writes elsewhere
 * (TMNT: ~8% of the GPU thread). PS5C_EVAL_PAGE_FILTER=0: always scan. */
static void evals_written(vk_pipelines *P, guest_addr addr, uint64_t size) {
    static int no_filter = -1;
    if (no_filter < 0) no_filter = getenv("PS5C_EVAL_PAGE_FILTER") && !atoi(getenv("PS5C_EVAL_PAGE_FILTER"));
    if (!P->evals) return;
    if (P->eval_pages_epoch != P->epoch) eval_pages_rebuild(P);
    if (!no_filter && !P->eval_pages_all && size) {
        guest_addr lo = addr >> 16, hi = (addr + size + 0xFFFF) >> 16;
        if (hi - lo <= 64) {
            for (uint32_t k = 0; k < P->eval_overflow_count; k++) P->evals[P->eval_overflow[k]].key = 0; /* any write */
            P->eval_overflow_count = 0;
            bool any = false;
            for (guest_addr pg = lo; pg < hi && !any; pg++) {
                uint32_t b = eval_page_bit(pg);
                any = (P->eval_pages[b / 64] >> (b % 64)) & 1;
            }
            if (!any) return;
        }
    }
    guest_addr end = addr + size;
    for (uint32_t i = 0; P->evals && i < EVAL_CACHE; i++) {
        eval_entry *c = &P->evals[i];
        if (!c->key) continue;
        if (!c->read.overflow && (!c->read.count || end <= c->read.min_start || addr >= c->read.max_end)) continue;
        bool hit = c->read.overflow;
        for (uint32_t k = 0; k < c->read.count && !hit; k++) hit = addr < c->read.end[k] && c->read.start[k] < end;
        if (hit) c->key = 0;
    }
}

/* Opt-in content check for the assumption that CPU constants stay unchanged
 * within a submission. @file: 0 control, 1 report only, 2 reject stale hits.
 * It reads bounded CPU ranges and never inserts GPU synchronization. */
static unsigned eval_check_mode(bool *configured) {
    static bool init;
    static const char *file;
    static unsigned mode;
    static bool present;
    static uint64_t next;
    if (!init) {
        const char *e = getenv("PS5C_EVAL_CACHE_CHECK");
        present = e != NULL;
        if (e && *e == '@') file = e + 1;
        else if (e) mode = (unsigned)strtoul(e, NULL, 10);
        init = true;
    }
    *configured = present;
    if (file && pal_time_ns() >= next) {
        next = pal_time_ns() + 100000000ull;
        FILE *f = fopen(file, "r");unsigned n;
        if (f) { if (fscanf(f, "%u", &n) == 1 && n <= 2) mode = n; fclose(f); }
    }
    return mode <= 2 ? mode : 0;
}

static bool eval_source_hash(renderer *r, const eval_ranges *read, uint64_t *hash) {
    if (read->overflow) return false;
    uint64_t bytes = 0, h = FNV_INIT;
    for (uint32_t i = 0; i < read->count; i++) {
        if (read->end[i] < read->start[i]) return false;
        uint64_t size = read->end[i] - read->start[i];
        if (size > 65536 - bytes) return false;
        bytes += size;
        const void *p = size ? guest_host(r, read->start[i], size) : NULL;
        if (size && !p) return false;
        if (size) h = texture_hash_words(p, (size_t)size, h);
    }
    *hash = h;
    return true;
}

static inline uint32_t eval_set_base(uint64_t key) {
    return (uint32_t)(((key >> 1) ^ (key >> 13)) & ((EVAL_CACHE / EVAL_WAYS) - 1u)) * EVAL_WAYS;
}

static eval_entry *eval_lookup(renderer *r, const stage_state *st, uint64_t key) {
    vk_pipelines *P = r->pipes;
    static int off = -1; /* PS5C_NO_EVAL_CACHE=1: every draw evaluates (comparison) */
    if (off < 0) off = getenv("PS5C_NO_EVAL_CACHE") != NULL;
    if (!P->evals || off) return NULL;
    uint32_t base = eval_set_base(key);
    for (uint32_t w = 0; w < EVAL_WAYS; w++) {
        eval_entry *c = &P->evals[base + w];
        if (c->key == key && c->prog == st->prog) {
            /* A compute store can reuse an existing writeback allocation.
             * Checking the live ranges also covers that case; the SGPR key
             * and submission epoch alone do not make its snapshot current. */
            if (c->read.overflow && (P->wb_count || P->held_count)) return NULL;
            for (uint32_t k = 0; k < c->read.count; k++)
                if (eval_read_pending(P, c->read.start[k], c->read.end[k] - c->read.start[k])) return NULL;
            if (c->epoch != P->epoch) {
                uint64_t h;
                if (!c->ok || c->read.overflow || !c->source_hash_valid || !eval_source_hash(r, &c->read, &h)) {
                    c->key = 0;
                    return NULL;
                }
                if (h != c->source_hash) {
                    for (uint32_t k = 0; k < c->read.count; k++)
                        copies_written(P, c->read.start[k], c->read.end[k] - c->read.start[k]);
                    c->key = 0;
                    return NULL;
                }
                c->epoch = P->epoch;
                return c;
            }
            bool configured;
            unsigned mode = eval_check_mode(&configured);
            if (configured && mode) {
                uint64_t h;
                bool valid = c->source_hash_valid && eval_source_hash(r, &c->read, &h);
                bool changed = valid && h != c->source_hash;
                static uint64_t checks, mismatches, unchecked, next_report;
                checks++;mismatches += changed;unchecked += !valid;
                if (changed && mismatches <= 32)
                    LOG_W(GPU, "eval cache content changed: stage %u shader 0x%llx (%016llx), epoch %llu, mode %u",
                          st->info.stage,(unsigned long long)st->prog->base,(unsigned long long)st->prog->hash,
                          (unsigned long long)P->epoch,mode);
                if (pal_time_ns() >= next_report) {
                    next_report = pal_time_ns() + 10000000000ull;
                    LOG_I(GPU, "eval cache content: %llu checked, %llu changed, %llu unchecked; mode %u",
                          (unsigned long long)checks,(unsigned long long)mismatches,(unsigned long long)unchecked,mode);
                }
                if (mode == 2 && (!valid || changed)) {
                    /* S_BUFFER uses staged data, while the evaluator's scalar
                     * snapshot discovers its descriptors. Refresh both paths
                     * for a changed source; never overwrite an in-use copy. */
                    if (changed) for (uint32_t k = 0; k < c->read.count; k++)
                        copies_written(P,c->read.start[k],c->read.end[k]-c->read.start[k]);
                    c->key = 0;
                    return NULL;
                }
            }
            return c;
        }
    }
    return NULL;
}

/* The evaluation of another draw of the program whose relocatable SGPRs (constant
 * buffer bases) differ: it applies when the memory read through each base, moved by
 * its change, and every other read still hash as stored. The same constants at
 * another address evaluate the same way, but for the descriptors' base words.
 * PS5C_EVAL_RELOCATE=0 turns it off. PSPHD writes each draw's constants anew. */
static const eval_entry *eval_relocated(renderer *r, const stage_state *st, uint64_t reloc_key) {
    vk_pipelines *P = r->pipes;
    static int off = -1;
    if (off < 0) { const char *e = getenv("PS5C_EVAL_RELOCATE"); off = (e && !strcmp(e, "0")) || getenv("PS5C_NO_EVAL_CACHE"); }
    uint16_t index = P->evals && !off ? P->eval_reloc[(reloc_key >> 7) & (EVAL_RELOC - 1u)] : 0;
    if (!index) return NULL;
    eval_entry *c = &P->evals[index - 1u];
    if (!c->key || c->reloc_key != reloc_key || c->prog != st->prog || !c->ok || c->read.overflow || !c->source_hash_valid)
        return NULL;
    eval_ranges moved = c->read;
    for (uint32_t k = 0; k < moved.count; k++) {
        uint32_t b = moved.sbase[k], slot = 0;
        if (b >= 64 || !(st->prog->eval_reloc_sgprs >> b & 1)) continue;
        for (uint64_t m = st->prog->eval_reloc_sgprs & ((1ull << b) - 1u); m; m &= m - 1) slot++;
        int64_t delta = (int64_t)st->eval.sgpr_init[b] - (int64_t)c->reloc_value[slot]; /* the high word is keyed */
        moved.start[k] += (guest_addr)delta;
        moved.end[k] += (guest_addr)delta;
    }
    for (uint32_t k = 0; k < moved.count; k++)
        if (eval_read_pending(P, moved.start[k], moved.end[k] - moved.start[k])) return NULL;
    uint64_t h;
    if (!eval_source_hash(r, &moved, &h) || h != c->source_hash) return NULL;
    return c;
}

static bool eval_copy(uint32_t **data, uint32_t *words, uint32_t *cap, const uint32_t *src, uint32_t n) {
    if (*cap < n) {
        uint32_t *d = (uint32_t *)realloc(*data, (size_t)n * 4);
        if (!d) return false;
        *data = d;
        *cap = n;
    }
    memcpy(*data, src, (size_t)n * 4);
    *words = n;
    return true;
}

/* Copy only active uniform header regions ([0 .. SH_U_SITE + site_count) and
 * [SH_U_VPORT .. n)), avoiding the ~16 KiB unused gap in [SH_U_SITE + site_count .. SH_U_VPORT). */
static bool eval_copy_sparse(uint32_t **data, uint32_t *words, uint32_t *cap, uint32_t *dirty,
                             const uint32_t *src, uint32_t n, uint32_t site_count, const uint32_t *sgpr_override) {
    if (site_count > SH_MAX_SITES) site_count = SH_MAX_SITES;
    if (n <= SH_U_VPORT) {
        if (!eval_copy(data, words, cap, src, n)) return false;
        if (sgpr_override && n >= SH_U_SITE) memcpy(*data, sgpr_override, SH_U_SITE * 4);
        if (dirty) *dirty = site_count;
        return true;
    }
    if (*cap < n) {
        uint32_t old_cap = *cap;
        uint32_t *d = (uint32_t *)realloc(*data, (size_t)n * 4);
        if (!d) return false;
        uint32_t zero_lo = old_cap < SH_U_SITE + site_count ? SH_U_SITE + site_count : old_cap;
        if (zero_lo < SH_U_VPORT)
            memset(d + zero_lo, 0, (size_t)(SH_U_VPORT - zero_lo) * 4);
        *data = d;
        *cap = n;
    }
    if (sgpr_override) {
        memcpy(*data, sgpr_override, SH_U_SITE * 4);
        if (site_count)
            memcpy(*data + SH_U_SITE, src + SH_U_SITE, (size_t)site_count * 4);
    } else {
        memcpy(*data, src, (size_t)(SH_U_SITE + site_count) * 4);
    }
    if (dirty) {
        if (*dirty > site_count) {
            uint32_t dirty_end = *dirty <= SH_MAX_SITES ? *dirty : SH_MAX_SITES;
            memset(*data + SH_U_SITE + site_count, 0, (size_t)(dirty_end - site_count) * 4);
        }
        *dirty = site_count;
    }
    memcpy(*data + SH_U_VPORT, src + SH_U_VPORT, (size_t)(n - SH_U_VPORT) * 4);
    *words = n;
    return true;
}

static void eval_store(renderer *r, const stage_state *st, uint64_t key, uint64_t reloc_key, bool ok, const eval_ranges *read) {
    vk_pipelines *P = r->pipes;
    if (!P->evals && !(P->evals = (eval_entry *)calloc(EVAL_CACHE, sizeof(eval_entry)))) return;
    uint32_t base = eval_set_base(key);
    eval_entry *c = NULL;
    for (uint32_t w = 0; w < EVAL_WAYS; w++) {
        if (P->evals[base + w].key == key && P->evals[base + w].prog == st->prog) {
            c = &P->evals[base + w];
            break;
        }
    }
    for (uint32_t w = 0; !c && w < EVAL_WAYS; w++) {
        if (!P->evals[base + w].key) {
            c = &P->evals[base + w];
            break;
        }
    }
    if (!c) c = &P->evals[base + (P->eval_next++ & (EVAL_WAYS - 1u))];
    uint32_t n = st->prog->site_count;
    if (n > c->site_cap) {
        sh_site_value *site = (sh_site_value *)realloc(c->site, sizeof(sh_site_value) * n);
        if (site) c->site = site;
        sh_site_shape *shapes = site ? (sh_site_shape *)realloc(c->shapes, sizeof(sh_site_shape) * n) : NULL;
        if (shapes) c->shapes = shapes;
        if (!site || !shapes) {
            c->key = 0;
            return;
        }
        c->site_cap = n;
    }
    if (!eval_copy_sparse(&c->smem_data, &c->smem_words, &c->smem_cap, &c->site_dirty,
                          st->eval.smem_data, st->eval.smem_words, n, NULL)) {
        c->key = 0;
        return;
    }
    c->key = key;
    c->epoch = P->epoch;
    c->prog = st->prog;
    c->ok = ok;
    c->read = *read;
    if (P->eval_pages_epoch == P->epoch) eval_pages_mark(P, c, (uint32_t)(c - P->evals)); /* else rebuilt at the next write */
    c->source_hash_valid = !st->eval.deferred && eval_source_hash(r, read, &c->source_hash);
    memcpy(c->site, st->eval.site, sizeof(sh_site_value) * n);
    memcpy(c->shapes, st->shapes, sizeof(sh_site_shape) * n);
    c->reloc_key = reloc_key;
    if (reloc_key && ok && c->source_hash_valid && !read->overflow) {
        uint32_t slot = 0;
        for (uint64_t m = st->prog->eval_reloc_sgprs; m; m &= m - 1)
            c->reloc_value[slot++] = st->eval.sgpr_init[__builtin_ctzll(m)];
        P->eval_reloc[(reloc_key >> 7) & (EVAL_RELOC - 1u)] = (uint16_t)(c - P->evals + 1);
    }
}

static bool evaluate_stage_calls(renderer *r, stage_state *st) {
    for (uint32_t attempt=0;attempt<SH_MAX_FUNCTIONS;attempt++) {
        if(st->wave_locked && st->prog->wave32!=st->wave32_expected)return false;
        if (sh_evaluate(st->prog,r->mem,&st->eval)) return true;
        if (!st->eval.call_pending || !st->eval.call_known || !getenv("PS5C_SHADER_CALLS") ||
            strcmp(getenv("PS5C_SHADER_CALLS"),"1")) return false;
        const rdna2_inst *call=&st->prog->insts[st->eval.call_inst];
        if (call->dst==RDNA2_OP_NULL) return false;
        guest_addr addr=st->eval.call_target;
        gmem_vma v;
        if (!addr || (addr&3) || !gmem_query_vma(r->mem,addr,false,&v) || addr<v.start ||
            addr-v.start>=v.size || v.reserved || v.prot==GPROT_NONE) return false;
        uint64_t bytes=v.size-(addr-v.start);
        if(bytes>65536)bytes=65536;
        const uint32_t *code=gmem_host(r->mem,addr,bytes);
        if (!code) return false;
        if (!st->linked_program) {
            st->linked_program=program_storage_alloc(sparse_programs());
            if (!st->linked_program) return false;
            if(!program_storage_copy(st->linked_program,st->prog,sparse_programs()))return false;
            st->prog=st->linked_program;
        }
        if(!program_storage_prepare(st->linked_program,sparse_programs()))return false;
        if (!sh_append_function(st->linked_program,code,(uint32_t)(bytes/4),addr,st->eval.call_inst)) return false;
        if(st->wave_locked) {
            /* A callee's opcode heuristic cannot change the caller's physical
             * wave mode. The linked copy is private to this stage operation. */
            if(st->linked_program->wave32!=st->wave32_expected) {
                static uint32_t corrected;
                if(corrected++<24)LOG_I(GPU,"shader %llx: kept authoritative wave%u after linking %llx (callee heuristic differed)",
                    (unsigned long long)st->prog->base,st->wave32_expected?32:64,(unsigned long long)addr);
            }
            st->linked_program->wave32=st->wave32_expected;
        }
        static uint32_t reports;
        if (reports++<24) LOG_I(GPU,"shader 0x%llx: linked function 0x%llx (%u functions, %u instructions, %u sites)",
            (unsigned long long)st->prog->base,(unsigned long long)addr,st->prog->function_count,
            st->prog->count,st->prog->site_count);
    }
    return false;
}

/* A PS4 stage's evaluated descriptors, GFX7 ones, as the GFX10 ones the rest reads */
static void gcn_descriptors(stage_state *st) {
    for (uint32_t i = 0; i < st->prog->site_count; i++) {
        sh_site_value *v = &st->eval.site[i];
        if (!v->valid) continue;
        switch (st->prog->sites[i].kind) {
        case SH_SITE_BUFFER: case SH_SITE_SBUFFER: gcn_buffer_desc_to_rdna2(v->desc); break;
        case SH_SITE_IMAGE: gcn_image_desc_to_rdna2(v->desc); break;
        default: break;
        }
    }
}

static bool setup_stage(renderer *r, stage_state *st) {
    st->footprint_hash_valid=false;st->footprint_count=0;
    for (uint32_t i = 0; i < st->prog->site_count; i++) {
        if (st->prog->sites[i].kind != SH_SITE_BVH) continue;
        static int enabled = -1;
        if (enabled < 0) { const char *value = getenv("PS5C_BVH"); enabled = value && !strcmp(value, "1"); }
        if (!enabled) {
            static uint32_t reports;
            if (reports++ < 8) LOG_W(GPU, "BVH prototype disabled: PS5C_BVH=1 enables bounded node reads");
            return false;
        }
    }
    uint64_t t0 = stat_now();
    vk_pipelines *P = r->pipes;
    uint64_t key, reloc_key = 0, reloc = 0;
    if (st->prog->count && (st->prog->iflags[0] & SH_IF_ANALYZED)) {
        uint64_t sgpr_mask = (uint64_t)st->prog->eval_sgpr_lo | ((uint64_t)st->prog->eval_sgpr_hi << 32);
        if (__builtin_popcountll(st->prog->eval_reloc_sgprs) <= EVAL_RELOC_SGPRS) reloc = st->prog->eval_reloc_sgprs;
        uint32_t masked_sgprs[48], kept_sgprs[48];
        uint32_t masked_count = 0;
        while (sgpr_mask) {
            uint32_t idx = (uint32_t)__builtin_ctzll(sgpr_mask);
            kept_sgprs[masked_count] = reloc >> idx & 1 ? 0 : st->eval.sgpr_init[idx];
            masked_sgprs[masked_count++] = st->eval.sgpr_init[idx];
            sgpr_mask &= sgpr_mask - 1;
        }
        key = texture_hash_words(masked_sgprs, (size_t)masked_count * sizeof(uint32_t), st->prog->hash);
        if (reloc) reloc_key = texture_hash_words(kept_sgprs, (size_t)masked_count * sizeof(uint32_t), st->prog->hash ^ 1);
    } else {
        key = texture_hash_words(st->eval.sgpr_init, sizeof(st->eval.sgpr_init), st->prog->hash);
    }
    st->info.uniform_shift=P->uniform_compact?sh_uniform_compact_shift(st->prog->site_count):0;
    /* Equal SGPR bits can mean user data in one dispatch and a non-uniform
     * workgroup ID in another. Source-content hashes cannot distinguish it. */
    uint32_t eval_context = st->eval.group_sgpr | ((uint32_t)st->eval.group_sgprs << 8);
    key = texture_hash_words(&st->prog->wave32,sizeof(st->prog->wave32),key) | 1;
    key = texture_hash_words(&eval_context, sizeof(eval_context), key) | 1;
    if (reloc) {
        reloc_key = texture_hash_words(&st->prog->wave32,sizeof(st->prog->wave32),reloc_key) | 1;
        reloc_key = texture_hash_words(&eval_context, sizeof(eval_context), reloc_key) | 1;
    }
    bool calls = st->prog->count && (st->prog->iflags[0] & SH_IF_ANALYZED)
                     ? (st->prog->iflags[0] & SH_IF_HAS_SWAPPC) != 0
                     : false;
    if (st->prog->count && !(st->prog->iflags[0] & SH_IF_ANALYZED))
        for(uint32_t i=0;i<st->prog->count;i++) calls|=st->prog->ops[i]==SH_OP_S_SWAPPC_B64;
    const eval_entry *c = st->eval.vsrc || calls ? NULL : eval_lookup(r, st, key); /* records are not kept */
    bool relocated = false;
    if (!c && reloc_key && !st->eval.vsrc && !calls) relocated = (c = eval_relocated(r, st, reloc_key)) != NULL;
    uint32_t n = st->prog->site_count;
    if (c && c->site_cap >= n && eval_copy_sparse(&st->eval.smem_data, &st->eval.smem_words, &st->eval.smem_cap, &st->eval.site_dirty,
                              c->smem_data, c->smem_words, n, st->eval.sgpr_init)) {
        memcpy(st->eval.site, c->site, sizeof(sh_site_value) * n);
        memcpy(st->shapes, c->shapes, sizeof(sh_site_shape) * n);
        for (uint32_t i = 0; relocated && i < n; i++) { /* the V# base words of this draw */
            uint32_t b = st->prog->insts[st->prog->sites[i].inst].sbase;
            if (st->prog->sites[i].kind == SH_SITE_SBUFFER && b < 64 && (reloc >> b & 1) && st->eval.site[i].valid)
                st->eval.site[i].desc[0] = st->eval.sgpr_init[b];
        }
        stat_add(ST_EVAL, t0);
        g_stat_eval_hits++;
        g_stat_eval_relocated += relocated;
        return c->ok;
    }
    if (g_stats_on) {
        if (st->eval.vsrc) g_stat_eval_vsrc++;
        else g_stat_eval_misses++;
    }
    st->eval.host = guest_host_cb;
    st->eval.host_user = r;
    eval_ranges read;
    memset(&read, 0, sizeof(read));
    P->eval_reading = &read;
    P->eval_current = &st->eval;
    st->eval.host_sbase = 0xFF;
    g_eval_base = st->prog->base;
    static int no_defer = -1;
    if (no_defer < 0) no_defer = getenv("PS5C_EVAL_NO_DEFER") != NULL;
    st->eval.defer = no_defer || st->eval.vsrc ? NULL : eval_defer_cb;
    bool ok = evaluate_stage_calls(r, st);
    if (st->eval.deferred) {
        /* again when the unknown values reached a descriptor, a scalar load address or a branch */
        if (!ok || st->eval.guessed) {
            g_stat_eval_redo++;
            st->eval.defer = NULL;
            memset(&read, 0, sizeof(read));
            ok = evaluate_stage_calls(r, st);
        } else g_stat_eval_deferred++;
    }
    st->eval.defer = NULL;
    P->eval_reading = NULL;
    P->eval_current = NULL;
    if (st->eval.call_pending && getenv("PS5C_TRACE_CALLS")) {
        static uint32_t reported;
        if (reported++ < 16) {
            uint32_t i = st->eval.call_inst;
            guest_addr target = st->eval.call_target;
            LOG_W(GPU, "shader call 0x%llx +0x%x: target 0x%llx known %u link s%u",
                (unsigned long long)st->prog->base, st->prog->pc[i] * 4,
                (unsigned long long)target, st->eval.call_known, st->prog->insts[i].dst);
            if (st->eval.call_known && target && !(target & 3)) {
                for (uint32_t pos = 0; pos < 256;) {
                    const uint32_t *code = gmem_host(r->mem, target + pos * 4, 16);
                    rdna2_inst in;
                    if (!code || !rdna2_decode(code, 4, &in)) break;
                    char text[160];
                    rdna2_disasm(&in, text, sizeof(text));
                    LOG_W(GPU, "  callee +0x%x %s", pos * 4, text);
                    pos += in.words;
                    if (in.name && (!strcmp(in.name,"s_setpc_b64") || !strcmp(in.name,"s_endpgm"))) break;
                }
            }
        }
    }
    read.overflow |= ok && eval_reads_globals(st->prog); /* global loads read through their own lookups */
    if(ok && st->eval.vsrc && footprint_record(st->prog)) {
        st->footprint_reads=read;
        st->footprint_hash_valid=!st->eval.deferred && !st->eval.guessed && eval_source_hash(r,&read,&st->footprint_hash);
    }
    if (g_stats_on && pal_time_ns() - t0 > 1000000) {
        static int slow;
        if (slow++ < 8)
            LOG_W(GPU, "slow evaluation of shader 0x%llx (hash %016llx): %.2f ms, %u instructions, %u uniform dwords",
                  (unsigned long long)st->prog->base, (unsigned long long)st->prog->hash,
                  (double)(pal_time_ns() - t0) / 1e6, st->prog->count, st->eval.smem_words);
    }
    if (ok && st->gcn) gcn_descriptors(st);
    if (ok) {
        st->info.uniform_shift=P->uniform_compact?sh_uniform_compact_shift(st->prog->site_count):0;
        static int explicit_lengths=-1;
        if(explicit_lengths<0){const char *s=getenv("PS5C_EXPLICIT_BUFFER_LENGTHS");explicit_lengths=s && !strcmp(s,"1");}
        for (uint32_t i = 0; i < st->prog->site_count; i++) {
            sh_site_shape_of(st->prog, &st->eval, i, &st->shapes[i]);
            sh_site_kind kind=st->prog->sites[i].kind;
            uint32_t enc=st->prog->insts[st->prog->sites[i].inst].enc;
            if(explicit_lengths && !st->shapes[i].address_map &&
               (kind==SH_SITE_SBUFFER || (kind==SH_SITE_BUFFER && (enc==RDNA2_ENC_MUBUF || enc==RDNA2_ENC_MTBUF))))
                st->shapes[i].buffer_length=SH_BUFFER_LENGTH;
        }
        static int share=-1;
        if(share<0)share=getenv("PS5C_NO_VERTEX_SHARE")==NULL;
        if(share && st->info.stage==SH_STAGE_VS && !st->gcn && st->prog->site_count<=256)
            for (uint32_t i = 0; i < st->prog->site_count; i++) {
                uint32_t first;
                if (st->shapes[i].address_map || st->shapes[i].buffer_length || SH_U_SITE + i >= st->eval.smem_words ||
                    !vertex_share_shift(st->prog, &st->eval, i, &first) || st->shapes[first].address_map ||
                    (st->shapes[first].buffer_length && st->shapes[first].buffer_length != SH_BUFFER_SHIFTED) ||
                    SH_U_SITE + first >= st->eval.smem_words) continue;
                st->shapes[i].buffer_length = st->shapes[first].buffer_length = SH_BUFFER_SHIFTED;
            }
        /* direct memory reads (vk_phys_import.h): a vertex fetch may bind from below its base */
        if(st->info.stage==SH_STAGE_VS && !st->gcn && phys_import_enabled(r))
            for (uint32_t i = 0; i < st->prog->site_count; i++)
                if (!st->shapes[i].address_map && !st->shapes[i].buffer_length && SH_U_SITE + i < st->eval.smem_words &&
                    vertex_share_eligible(st->prog, &st->eval, i))
                    st->shapes[i].buffer_length = SH_BUFFER_SHIFTED;
    }
    if (!st->eval.vsrc && !calls) eval_store(r, st, key, reloc_key, ok, &read);
    if (g_stats_on) prog_stat_add(g_stat_eval_prog, st->prog, pal_time_ns() - t0);
    stat_add(ST_EVAL, t0);
    { /* PS5C_LOG_SITES=<address>: the descriptors found for one program (debugging aid) */
        static uint64_t want;
        static int have = -1;
        static int logged;
        static const char *want_file;
        static uint64_t want_flip=UINT64_MAX;
        if (have < 0) {
            const char *setting=getenv("PS5C_LOG_SITES");
            have = setting != NULL;
            if(setting && setting[0]=='@')want_file=setting+1;
            else if(setting)want=strtoull(setting,NULL,16);
        }
        if(want_file && want_flip!=g_trace_flip) {
            want_flip=g_trace_flip;unsigned long long next=0;
            FILE *f=fopen(want_file,"r");
            if(f){if(fscanf(f,"%llx",&next)!=1)next=0;fclose(f);}
            if(want!=next){want=next;logged=0;}
        }
        /* also by the hash of the first 32 bytes, stable across runs when the full hash is not */
        /* PS5C_LOG_SITES_FRAME=<flip>: from that frame on */
        static int64_t from = -2;
        if (from == -2) from = getenv("PS5C_LOG_SITES_FRAME") ? atoll(getenv("PS5C_LOG_SITES_FRAME")) : -1;
        /* with PS5C_LOG_SITES_OFFSET, the first logged buffer is read again 1, 2 and 3 frames later
         * (constants the CPU fills after the draw was recorded) */
        static guest_addr again;
        static uint64_t again_flip;
        static uint32_t again_n;
        if (again && again_n < 3 && g_trace_flip > again_flip + again_n) {
            const uint32_t *w = (const uint32_t *)gmem_host(r->mem, again, 192);
            again_n++;
            for (uint32_t k = 0; w && k < 48; k += 8)
                LOG_I(GPU, "  0x%llx+%02x %u frames later: %08x %08x %08x %08x %08x %08x %08x %08x", (unsigned long long)again, k * 4,
                      again_n, w[k], w[k + 1], w[k + 2], w[k + 3], w[k + 4], w[k + 5], w[k + 6], w[k + 7]);
        }
        if (have && (st->prog->base == want || st->prog->hash == want ||
                     fnv(st->prog->code, 32, 0xCBF29CE484222325ull) == want) && logged < (getenv("PS5C_LOG_SITES_MAX") ? atoi(getenv("PS5C_LOG_SITES_MAX")) : 3) &&
            (from < 0 || (int64_t)g_trace_flip >= from)) {
            logged++;
            for (uint32_t i = 0; i < st->prog->site_count; i++) {
                const sh_site_value *v = &st->eval.site[i];
                LOG_I(GPU, "site %u kind %d inst 0x%x valid %d unknown %d: %08x %08x %08x %08x", i, (int)st->prog->sites[i].kind,
                      st->prog->pc[st->prog->sites[i].inst] * 4, (int)v->valid, (int)v->desc_unknown,
                      v->desc[0], v->desc[1], v->desc[2], v->desc[3]);
                guest_addr base = (guest_addr)v->desc[0] | ((guest_addr)(v->desc[1] & 0xFFFF) << 32);
                if (st->prog->sites[i].kind == SH_SITE_IMAGE) base = ((guest_addr)v->desc[0] << 8) | ((guest_addr)(v->desc[1] & 0xFF) << 40);
                /* PS5C_LOG_SITES_OFFSET=<hex bytes>: the data from there on */
                if (base && getenv("PS5C_LOG_SITES_OFFSET")) base += strtoull(getenv("PS5C_LOG_SITES_OFFSET"), NULL, 16);
                const uint32_t *w = st->prog->sites[i].kind != SH_SITE_SMEM && base ? (const uint32_t *)gmem_host(r->mem, base, 192) : NULL;
                if (w && getenv("PS5C_LOG_SITES_DUMP") && (st->prog->sites[i].kind == SH_SITE_BUFFER || st->prog->sites[i].kind == SH_SITE_SBUFFER)) {
                    /* PS5C_LOG_SITES_DUMP: the whole buffer (records x stride) to user/frames/site_<n>_<i>_<base>.bin */
                    uint32_t stride = (v->desc[1] >> 16) & 0x3FFF;
                    uint64_t size = stride ? (uint64_t)v->desc[2] * stride : v->desc[2];
                    if (size > (1u << 20)) size = 1u << 20;
                    const void *all = size ? gmem_host(r->mem, base, size) : NULL;
                    char path[96];
                    snprintf(path, sizeof(path), "user/frames/site_%u_%u_%010llx.bin", logged, i, (unsigned long long)base);
                    FILE *f = all ? fopen(path, "wb") : NULL;
                    if (f) {
                        fwrite(all, 1, (size_t)size, f);
                        fclose(f);
                        LOG_I(GPU, "  dumped %llu bytes to %s", (unsigned long long)size, path);
                    }
                }
                if (w)
                    LOG_I(GPU, "  data: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
                          w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15],
                          w[16], w[17], w[18], w[19], w[20], w[21], w[22], w[23]);
                if (w && getenv("PS5C_LOG_SITES_OFFSET") && !again) {
                    again = base;
                    again_flip = g_trace_flip;
                    if (getenv("PS5C_LOG_SITES_WATCH")) runtime_watch_move(r->mem, base); /* who writes it next */
                }
                if (w && getenv("PS5C_LOG_SITES_OFFSET"))
                    for (uint32_t k = 24; k < 48; k += 8)
                        LOG_I(GPU, "  +%02x:  %08x %08x %08x %08x %08x %08x %08x %08x", k * 4, w[k], w[k + 1], w[k + 2], w[k + 3],
                              w[k + 4], w[k + 5], w[k + 6], w[k + 7]);
                if (v->valid && (st->prog->sites[i].kind == SH_SITE_SMEM || st->prog->sites[i].kind == SH_SITE_SBUFFER) &&
                    v->smem_offset < st->eval.smem_words) {
                    uint32_t first = st->eval.smem_data[v->smem_offset];
                    float scalar;
                    memcpy(&scalar, &first, sizeof(scalar));
                    LOG_I(GPU, "  evaluated first: %08x (float %.9g)", first, (double)scalar);
                }
                if (st->prog->sites[i].kind == SH_SITE_SMEM && v->smem_offset + 8 <= st->eval.smem_words) {
                    const uint32_t *l = st->eval.smem_data + v->smem_offset;
                    LOG_I(GPU, "  loaded: %08x %08x %08x %08x %08x %08x %08x %08x", l[0], l[1], l[2], l[3], l[4], l[5], l[6], l[7]);
                    if (v->smem_offset + 16 <= st->eval.smem_words)
                        LOG_I(GPU, "  then:   %08x %08x %08x %08x %08x %08x %08x %08x", l[8], l[9], l[10], l[11], l[12], l[13], l[14], l[15]);
                }
            }
        }
    }
    return ok;
}

/* CB resolve (MSAA -> single sample): targets are single-sampled here, so it is a copy. */
static void resolve(renderer *r, const gpu *g) {
    uint32_t info0 = gpu_reg(g, mmCB_COLOR0_INFO), info1 = gpu_reg(g, mmCB_COLOR0_INFO + GPU_CB_STRIDE);
    uint32_t a0 = gpu_reg(g, mmCB_COLOR0_ATTRIB2), a1 = gpu_reg(g, mmCB_COLOR0_ATTRIB2 + 1);
    guest_addr src_addr = (guest_addr)gpu_reg(g, mmCB_COLOR0_BASE) << 8;
    guest_addr dst_addr = (guest_addr)gpu_reg(g, mmCB_COLOR0_BASE + GPU_CB_STRIDE) << 8;
    uint32_t w = ((a0 >> 14) & 0x3FFF) + 1, h = (a0 & 0x3FFF) + 1;
    uint32_t w1 = ((a1 >> 14) & 0x3FFF) + 1, h1 = (a1 & 0x3FFF) + 1;
    vk_image *src = vk_image_find(r, src_addr, w, h, cb_format(info0));
    if (!src || !dst_addr) return;
    vk_image *dst = vk_image_get(r, dst_addr, w1, h1, cb_format(info1));
    if (!dst || dst == src) return;
    end_pass(r);
    vk_barrier(r->cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vk_barrier(r->cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit b = {0};
    b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.srcSubresource.layerCount = 1;
    b.srcOffsets[1].x = (int32_t)(src->ext_w ? src->ext_w : w);
    b.srcOffsets[1].y = (int32_t)(src->ext_h ? src->ext_h : h);
    b.srcOffsets[1].z = 1;
    b.dstSubresource = b.srcSubresource;
    b.dstOffsets[1].x = (int32_t)(dst->ext_w ? dst->ext_w : w1);
    b.dstOffsets[1].y = (int32_t)(dst->ext_h ? dst->ext_h : h1);
    b.dstOffsets[1].z = 1;
    census_copy(src, 3);
    vkCmdBlitImage(r->cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    dst->gpu_written = true;
    dst->written_seq = ++r->write_seq;
}

static float f32_of(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

/* ---- Draw ---- */

/* The depth surface a draw uses: DB_Z_INFO format and address, when the
 * depth test is on or the draw clears depth. */
/* The host format of the depth surface: 16- or 32-bit float depth; with an 8-bit stencil
 * (DB_STENCIL_INFO FORMAT) 32-bit float depth becomes D32_SFLOAT_S8_UINT. */
static VkFormat db_format(const gpu *g) {
    uint32_t zfmt = gpu_reg(g, mmDB_Z_INFO) & 3;
    if (zfmt == 1) return VK_FORMAT_D16_UNORM;
    if (zfmt != 3) return VK_FORMAT_UNDEFINED;
    return g_stencil && (gpu_reg(g, mmDB_STENCIL_INFO) & 1) ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT;
}

/* A depth image of either 32-bit float depth format at that address (a sampled depth). */
static vk_image *find_depth32(renderer *r, guest_addr addr, uint32_t w, uint32_t h, bool array) {
    vk_image *z = array ? vk_image_find_array(r, addr, w, h, VK_FORMAT_D32_SFLOAT) : vk_image_find(r, addr, w, h, VK_FORMAT_D32_SFLOAT);
    vk_image *s = !g_stencil ? NULL : array ? vk_image_find_array(r, addr, w, h, VK_FORMAT_D32_SFLOAT_S8_UINT)
                                            : vk_image_find(r, addr, w, h, VK_FORMAT_D32_SFLOAT_S8_UINT);
    if (!z || (s && s->written_seq > z->written_seq)) return s;
    return z;
}

static bool depth_target(const gpu *g, guest_addr *addr, uint32_t *w, uint32_t *h, VkFormat *fmt) {
    uint32_t zinfo = gpu_reg(g, mmDB_Z_INFO), ctl = gpu_reg(g, mmDB_DEPTH_CONTROL);
    uint32_t zfmt = zinfo & 3, size = gpu_reg(g, mmDB_DEPTH_SIZE_XY);
    bool clear = gpu_reg(g, mmDB_RENDER_CONTROL) & 1;
    VkFormat f = db_format(g);
    /* stencil test, or a stencil clear, without depth test still needs the surface */
    if (vk_format_has_stencil(f) && ((ctl & 1) || ((gpu_reg(g, mmDB_RENDER_CONTROL) >> 1) & 1))) clear = true;
    static int off = -1; /* PS5C_NO_DEPTH: debugging aid, draw without depth */
    if (off < 0) off = getenv("PS5C_NO_DEPTH") != NULL;
    if (off) return false;
    *addr = (guest_addr)gpu_reg(g, mmDB_Z_WRITE_BASE) << 8;
    if (!*addr) *addr = (guest_addr)gpu_reg(g, mmDB_Z_READ_BASE) << 8;
    if (!(zfmt == 1 || zfmt == 3) || !*addr || !size || !(((ctl >> 1) & 1) || clear)) return false;
    *w = (size & 0x3FFF) + 1;
    *h = ((size >> 16) & 0x3FFF) + 1;
    *fmt = f;
    return true;
}

typedef struct depth_bias_state {
    bool on;
    float constant, clamp, slope;
} depth_bias_state;

/* PA_SU_SC_MODE_CNTL POLY_OFFSET_FRONT_ENABLE [11] / BACK_ENABLE [12] turn on the
 * PA_SU_POLY_OFFSET_* amounts (shadow maps). The slope scale register holds 16x
 * Vulkan's factor; the constant is in Vulkan's units for float depth, 4x them for
 * 16-bit depth (radv converts the other way). */
static depth_bias_state depth_bias(const gpu *g, VkFormat zf, uint32_t depth_test, uint32_t depth_func, uint32_t rt_count) {
    depth_bias_state b = {0};
    uint32_t mode = gpu_reg(g, mmPA_SU_SC_MODE_CNTL);
    bool front = (mode >> 11) & 1, back = (mode >> 12) & 1;
    if (front || back) {
        uint32_t reg = front ? 0xA2E0u /* PA_SU_POLY_OFFSET_FRONT_SCALE */ : 0xA2E2u /* BACK_SCALE */;
        b.slope = f32_of(gpu_reg(g, reg)) / 16.0f;
        b.constant = f32_of(gpu_reg(g, reg + 1)) / (zf == VK_FORMAT_D16_UNORM ? 4.0f : 1.0f);
        b.clamp = f32_of(gpu_reg(g, 0xA2DFu /* PA_SU_POLY_OFFSET_CLAMP */));
        b.on = b.slope != 0.0f || b.constant != 0.0f;
    }
    /* PS5C_DEPTH_SLACK=<units>: debugging aid, color draws testing EQUAL/GEQUAL/LEQUAL against a
     * prepass get that much depth bias in their favor (how far apart the two passes' depths are) */
    static int slack = -2;
    if (slack == -2) slack = getenv("PS5C_DEPTH_SLACK") ? atoi(getenv("PS5C_DEPTH_SLACK")) : 0;
    uint32_t f = depth_func;
    if (slack > 0 && depth_test && rt_count && (f == 2 || f == 3 || f == 6)) {
        b.on = true;
        b.constant += (float)(f == 3 ? -slack : slack);
    }
    static int off = -1; /* PS5C_NO_DEPTH_BIAS: debugging aid */
    if (off < 0) off = getenv("PS5C_NO_DEPTH_BIAS") != NULL;
    if (off || !b.on) b = (depth_bias_state){0};
    return b;
}

/* The depth image and the attachment view of its slice. DB_DEPTH_VIEW: SLICE_START [10:0],
 * SLICE_MAX [23:13]. A depth array (Sonic's cascaded shadow map: four slices, one draw pass
 * each, sampled as one array texture) is one image with a layer per slice. */
static vk_image *depth_image(renderer *r, const gpu *g, guest_addr za, uint32_t zw, uint32_t zh, VkFormat zf,
                             uint32_t *slice, VkImageView *view) {
    static int log_bases=-1,log_slices=-1;
    if(log_bases<0) {
        log_bases=getenv("PS5C_LOG_DEPTH_BASES")!=NULL;
        log_slices=getenv("PS5C_LOG_DEPTH_SLICES")!=NULL;
    }
    uint32_t v = gpu_reg(g, mmDB_DEPTH_VIEW), start = v & 0x7FF, max = (v >> 13) & 0x7FF;
    uint32_t layers = (max > start ? max : start) + 1;
    static int off = -1; /* PS5C_NO_DEPTH_SLICES: every slice in the same image (the old way) */
    if (off < 0) off = getenv("PS5C_NO_DEPTH_SLICES") != NULL;
    if (off || layers > 256) layers = 1, start = 0;
    if (layers > 1 && vk_format_has_stencil(zf)) zf = VK_FORMAT_D32_SFLOAT; /* layered stencil: depth only */
    vk_image *img;
    if (layers == 1) {
        img = vk_image_find_array(r, za, zw, zh, zf); /* a slice 0 draw into a known array */
        if (!img) {
            r->scale_new = true; /* PS5C_RT_SCALE */
            img = vk_image_get(r, za, zw, zh, zf);
            r->scale_new = false;
        }
    } else {
        img = vk_image_find_array(r, za, zw, zh, zf);
        if (!img || img->layers < layers) {
            vk_image *old = img ? img : vk_image_find(r, za, zw, zh, zf);
            img = old ? grow_layers(r, old, zf, layers) : vk_image_get_layers(r, za, zw, zh, zf, layers);
        }
    }
    *slice = img && start < img->layers ? start : 0;
    if (!img) return NULL;
    if (vk_format_has_stencil(img->format)) {
        guest_addr sa = (guest_addr)gpu_reg(g, mmDB_STENCIL_WRITE_BASE) << 8;
        img->stencil_addr = sa ? sa : (guest_addr)gpu_reg(g, mmDB_STENCIL_READ_BASE) << 8;
    }
    depth_flow(za, zw, (gpu_reg(g, mmDB_DEPTH_CONTROL) >> 2) & 1 ? "written" : "tested");
    if (r->cpu_prepare_enabled?log_bases:getenv("PS5C_LOG_DEPTH_BASES")!=NULL) { /* each distinct pair once */
        static guest_addr seen[64][2];
        static uint32_t n;
        guest_addr rd = (guest_addr)gpu_reg(g, mmDB_Z_READ_BASE) << 8, wr = (guest_addr)gpu_reg(g, mmDB_Z_WRITE_BASE) << 8;
        bool known = false;
        for (uint32_t i = 0; i < n && !known; i++) known = seen[i][0] == rd && seen[i][1] == wr;
        if (!known && n < 64) {
            seen[n][0] = rd, seen[n++][1] = wr;
            gmem_vma vma = {0};
            bool q = gmem_query_vma(r->mem, wr, false, &vma);
            LOG_I(GPU, "depth bases: read 0x%llx write 0x%llx %ux%u, render control %08x override %08x control %08x; phys 0x%llx "
                       "(vma %d: 0x%llx+0x%llx kind %u os 0x%llx phys 0x%llx %s)",
                  (unsigned long long)rd, (unsigned long long)wr, zw, zh, gpu_reg(g, mmDB_RENDER_CONTROL), gpu_reg(g, 0xA003u),
                  gpu_reg(g, mmDB_DEPTH_CONTROL), (unsigned long long)vk_phys_of(r, wr), (int)q, (unsigned long long)vma.start,
                  (unsigned long long)vma.size, vma.kind, (unsigned long long)vma.os_base, (unsigned long long)vma.phys, vma.name);
        }
    }
    if ((r->cpu_prepare_enabled?log_slices:getenv("PS5C_LOG_DEPTH_SLICES")!=NULL) && layers > 1) {
        static uint32_t logged;
        if (logged++ < 64)
            LOG_I(GPU, "depth 0x%llx %ux%u: view %08x -> slice %u of %u, z_info %08x, poly offset mode %08x %08x %08x %08x",
                  (unsigned long long)za, zw, zh, v, *slice, img->layers, gpu_reg(g, mmDB_Z_INFO), gpu_reg(g, mmPA_SU_SC_MODE_CNTL),
                  gpu_reg(g, 0xA2DEu), gpu_reg(g, 0xA2E0u), gpu_reg(g, 0xA2E1u));
    }
    if (img->layers == 1) {
        *view = img->ds_view ? img->ds_view : img->view;
    } else {
        uint32_t t[8] = {0, 0, 0, 13u << 28, (*slice << 16) | *slice, 0, 0, 0}; /* a 2D array descriptor: that slice */
        *view = layer_view(r, img, t, false, img->format, false);
    }
    return img;
}

static void dcc_written(vk_pipelines *P, guest_addr addr, uint64_t size) {
    for (uint32_t i = 0; i < P->dcc_count; i++)
        if (addr < P->dccs[i].end && addr + size > P->dccs[i].start) P->dccs[i].written = true;
}

static bool log_memory_range(guest_addr addr, uint64_t size, const char *how) {
    /* Track an alias between buffer writes and a color image (debugging aid). */
    static bool checked;
    static uint64_t watch_start, watch_size;
    static uint32_t watch_logged;
    if (!checked) {
        const char *s = getenv("PS5C_LOG_MEM_RANGE");
        if (s) {
            char *end;
            watch_start = strtoull(s, &end, 16);
            if (*end == ':') watch_size = strtoull(end + 1, NULL, 16);
        }
        checked = true;
    }
    if (watch_size && size && addr < watch_start + watch_size && watch_start < addr + size && watch_logged++ < 256) {
        LOG_I(GPU, "memory range write by %s: 0x%llx +0x%llx flip %llu", how,
              (unsigned long long)addr, (unsigned long long)size, (unsigned long long)g_trace_flip);
        return true;
    }
    return false;
}

/* A metadata maintenance pass can read a clear code and leave it unchanged.
 * Registering its entire descriptor as another depth clear erases the depth
 * just rendered (GTA's conditional HTILE expansion). A pass that reads what it
 * writes (or uses atomics) is such an update; one that fills from elsewhere
 * (Joe & Mac copies a clear pattern buffer into HTILE) may still clear depth.
 * RMW still updates DCC. */
static uint32_t htile_fill_candidate(const vk_pipelines *P, const stage_state *cs, uint32_t wb_before) {
    const sh_program *p = cs->prog;
    if (vk_htile_masked_depth_clear(p, &cs->eval, &cs->info)) return 2; /* Z/S masked depth clear */
    for (uint32_t i = 0; i < p->site_count; i++) {
        const sh_site *s = &p->sites[i];
        if (s->kind == SH_SITE_SMEM) continue; /* scalar descriptors/constants */
        if (strstr(p->insts[s->inst].name, "atomic")) return false;
        if (s->store || (s->kind != SH_SITE_BUFFER && s->kind != SH_SITE_IMAGE)) continue;
        const uint32_t *d = cs->eval.site[i].desc;
        guest_addr a = s->kind == SH_SITE_IMAGE ? ((guest_addr)d[0] << 8) | ((guest_addr)(d[1] & 0xFF) << 40)
                                                : (guest_addr)d[0] | ((guest_addr)(d[1] & 0xFFFF) << 32);
        for (uint32_t k = wb_before; k < P->wb_count; k++)
            if (a >= P->wb[k].addr && a < P->wb[k].addr + P->wb[k].size) return false; /* reads what it writes */
    }
    return true;
}

static bool log_depth_on(void) {
    static int on = -1; /* read once: checked on every buffer and depth write */
    if (on < 0) on = getenv("PS5C_LOG_DEPTH") != NULL;
    return on;
}

static void htile_written(vk_pipelines *P, guest_addr addr, uint64_t size, const char *how, uint64_t writer,
                          uint32_t may_clear_depth) {
    log_memory_range(addr, size, how);
    dcc_written(P, addr, size);
    for (uint32_t i = 0; !may_clear_depth && i < P->htile_count && log_depth_on(); i++) {
        static uint32_t logged;
        if (addr < P->htiles[i].end && addr + size > P->htiles[i].start && logged++ < 16)
            LOG_I(GPU, "depth 0x%llx: metadata written by %s %016llx, not taken as a clear (it reads)",
                  (unsigned long long)P->htiles[i].zaddr, how, (unsigned long long)writer);
    }
    if (!may_clear_depth) return;
    for (uint32_t i = 0; i < P->htile_count; i++) {
        htile_entry *h = &P->htiles[i];
        if (may_clear_depth == 2 && (!h->has_stencil || addr > h->start || size < h->end - addr)) continue;
        if (addr < h->end && addr + size > h->start) {
            if (log_depth_on() && (!h->cleared || getenv("PS5C_LOG_DEPTH_FRAME")))
                LOG_I(GPU, "depth 0x%llx: metadata cleared by %s (0x%llx, %llu bytes)", (unsigned long long)h->zaddr, how,
                      (unsigned long long)addr, (unsigned long long)size);
            h->cleared = true;
            h->writer = writer;
        }
    }
}

void renderer_mem_written(renderer *r, guest_addr addr, uint64_t size) {
    pal_mutex_lock(r->lock);
    if (r->pipes) {
        htile_written(r->pipes, addr, size, "DMA", 0, true);
        copies_written(r->pipes, addr, size);
        evals_written(r->pipes, addr, size);
    }
    pal_mutex_unlock(r->lock);
}

/* Registers the HTILE of the draw's depth surface; true when the surface was
 * fast-cleared since its last use (then it takes DB_DEPTH_CLEAR). */
static bool htile_take_clear(vk_pipelines *P, const gpu *g, const vk_image *z, uint32_t slice, guest_addr *metadata) {
    if (!(gpu_reg(g, mmDB_Z_INFO) & GPU_DB_Z_INFO_TILE_SURFACE_ENABLE)) return false; /* no metadata */
    guest_addr base = (guest_addr)gpu_reg(g, mmDB_HTILE_DATA_BASE) << 8;
    if (!base) return false;
    uint64_t size = (uint64_t)((z->width + 7) / 8) * ((z->height + 7) / 8) * 4; /* a dword per 8x8 tile */
    /* The register addresses the array's metadata, not the selected layer.
     * This follows the dense 8x8 HTILE layout supported by the reader below. */
    base += size * slice;
    *metadata = base;
    htile_entry *h = NULL;
    for (uint32_t i = 0; i < P->htile_count && !h; i++) {
        if (P->htiles[i].zaddr == z->addr && P->htiles[i].slice == slice) h = &P->htiles[i];
    }
    if (!h) {
        uint32_t at=P->htile_count;
        if(at==MAX_HTILES) {
            at=0;
            for(uint32_t i=1;i<MAX_HTILES;i++)if(P->htiles[i].used<P->htiles[at].used)at=i;
        } else P->htile_count++;
        h = &P->htiles[at];
        *h = (htile_entry){.zaddr=z->addr,.slice=slice};
    }
    h->used=++P->htile_sequence;
    h->has_stencil = vk_format_has_stencil(z->format);
    if(h->start!=base || h->end!=base+size){h->cleared=false;h->writer=0;}
    h->start = base;
    h->end = base + size;
    bool c = h->cleared;
    h->cleared = false;
    return c;
}

/* Metadata writes are clears only when they say so: an HTILE dword's ZMASK (bits 3:0) is 0 for a
 * cleared 8x8 tile, 0xF for one whose depth is in memory. Unreal rewrites the HTILE of its scene
 * depth every frame after the depth pass (expanded tiles): taking that for a clear wiped the depth
 * and the lighting saw only sky (DBZ's black menus). The compute results are completed first. */
static bool htile_says_cleared(renderer *r, const vk_image *z, guest_addr base) {
    static int any_write = -1;
    if (any_write < 0) any_write = getenv("PS5C_HTILE_ANY_WRITE") != NULL;
    if (any_write) return true; /* the old rule (A/B) */
    static int predict = -1;
    if (predict < 0) predict = !getenv("PS5C_HTILE_PREDICT") || atoi(getenv("PS5C_HTILE_PREDICT"));
    uint64_t size = (uint64_t)((z->width + 7) / 8) * ((z->height + 7) / 8) * 4;
    vk_pipelines *P = r->pipes;
    uint64_t writer = 0;
    for (uint32_t i = 0; i < P->htile_count; i++)
        if (P->htiles[i].zaddr == z->addr && P->htiles[i].start == base) writer = P->htiles[i].writer;
    bool pending = false;
    for (uint32_t i = 0; i < P->wb_count && !pending; i++) pending = base < P->wb[i].addr + P->wb[i].size && P->wb[i].addr < base + size;
    if (pending) {
        const htile_verdict *v = writer ? htile_verdict_find(P, writer) : NULL;
        if (predict && v && !v->unstable && v->seen >= 2 && P->htile_check_count < MAX_HTILE_CHECKS) {
            P->htile_checks[P->htile_check_count++] = (htile_check){base, size, writer, v->cleared};
            return v->cleared;
        }
        end_pass(r);
        sync_batch(r);
        apply_writebacks(r);
        vk_begin(r);
    }
    const uint32_t *t = (const uint32_t *)guest_host(r, base, size);
    if (!t) return true;
    uint64_t cleared, seen;
    bool verdict = htile_mostly_cleared(t, size, &cleared, &seen);
    if (pending) htile_verdict_learn(P, writer, verdict);
    static uint32_t logged;
    if (log_depth_on() && logged++ < 32)
        LOG_I(GPU, "depth 0x%llx: metadata write by %016llx has %llu of %llu sampled tiles cleared (first %08x)",
              (unsigned long long)z->addr, (unsigned long long)writer, (unsigned long long)cleared,
              (unsigned long long)seen, t[0]);
    return verdict;
}

/* A DCC color target: its metadata range is watched; when the game wrote it since the
 * last use, the codes there say whether that was a fast clear (and to what). */
static void dcc_apply(renderer *r, vk_image *img, dcc_entry *e, guest_addr shader, const char *how) {
    static int log_on = -1;
    static guest_addr log_image;
    static const char *log_from;
    if (log_on < 0) {
        log_on = getenv("PS5C_LOG_DCC") != NULL;
        const char *filter = getenv("PS5C_LOG_DCC_IMAGE");
        if (filter) log_image = strtoull(filter, NULL, 16);
        log_from = getenv("PS5C_LOG_DCC_FROM");
    }
    bool log_this = log_on && (!log_image || log_image == img->addr) &&
        (!log_from || g_trace_flip >= strtoull(log_from,NULL,0));
    guest_addr start = e->start;
    uint64_t size = e->end - e->start;
    if (!e->written) return;
    e->written = false;
    bool pending = false;
    uint8_t first = 0;
    /* Mixed metadata can contain valid tiles: a majority (or sparse samples)
     * cannot justify clearing every pixel of the host image. */
    int code = dcc_keys(r, img->addr, start, size, log_on, &pending, &first);
    if (log_this && code == -2) LOG_I(GPU, "DCC 0x%llx: metadata 0x%llx not readable (pending %d)", (unsigned long long)img->addr, (unsigned long long)start, (int)pending);
    if (code == -2) return;
    static uint32_t logged;
    if (log_this && logged++ < 40)
        LOG_I(GPU, "DCC 0x%llx (%ux%u fmt %d): metadata written, %s (first 0x%02x, %llu bytes), "
                   "flip %llu PS 0x%llx metadata 0x%llx pending %d prior seq %llu at %s", (unsigned long long)img->addr,
              img->width, img->height, (int)img->format, code < 0 ? "not a uniform clear" : "fast clear",
              first, (unsigned long long)size,
              (unsigned long long)g_trace_flip, (unsigned long long)shader,
              (unsigned long long)start, (int)pending, (unsigned long long)img->written_seq, how);
    if (code < 0) return;
    dcc_clear_image(r, img, code, e->clear);
}

/* The uniform clear code of the DCC keys at [start, start + size) (-1: not a
 * uniform clear, -2: not readable), once the compute results of the batch that
 * write them (fills) have landed, or as predicted for their writer (dcc_verdict).
 * *first: the first key (logs). */
static int dcc_keys(renderer *r, guest_addr img_addr, guest_addr start, uint64_t size, bool log_on, bool *pending_out,
                    uint8_t *first) {
    static int predict = -1;
    if (predict < 0) predict = !getenv("PS5C_DCC_PREDICT") || atoi(getenv("PS5C_DCC_PREDICT"));
    vk_pipelines *P = r->pipes;
    bool pending = false; /* the codes are compute results still in the batch */
    uint64_t writer = 0;
    for (uint32_t i = 0; i < P->wb_count && !pending; i++) {
        pending = start < P->wb[i].addr + P->wb[i].size && P->wb[i].addr < start + size;
        if (pending) writer = P->wb[i].check_hash;
        static uint32_t logged;
        if (pending && log_on && logged++ < 24)
            LOG_I(GPU, "DCC 0x%llx: metadata 0x%llx+0x%llx pending in writeback 0x%llx+0x%llx of shader %016llx site %u",
                  (unsigned long long)img_addr, (unsigned long long)start, (unsigned long long)size,
                  (unsigned long long)P->wb[i].addr, (unsigned long long)P->wb[i].size,
                  (unsigned long long)P->wb[i].check_hash, P->wb[i].check_site);
    }
    if (pending_out) *pending_out = pending;
    if (pending) {
        const dcc_verdict *v = writer ? dcc_verdict_find(P, writer, start) : NULL;
        if (predict && v && !v->unstable && v->seen >= 2 && P->dcc_check_count < MAX_DCC_CHECKS) {
            P->dcc_checks[P->dcc_check_count++] = (dcc_check){start, size, writer};
            if (first) *first = v->code < 0 ? 0 : (uint8_t)v->code;
            return v->code;
        }
        end_pass(r);
        sync_batch(r);
        apply_writebacks(r);
        vk_begin(r);
    }
    const uint8_t *m = (const uint8_t *)guest_host(r, start, size);
    if (!m) return -2;
    if (first) *first = m[0];
    int code = vk_dcc_uniform_clear(m, (size_t)size);
    if (pending) dcc_verdict_learn(P, writer, start, code);
    return code;
}

/* Fills img with the color of DCC fast-clear `code` (0x20: the CB clear words `clear`). */
static void dcc_clear_image(renderer *r, vk_image *img, int code, const uint32_t clear[2]) {
    VkClearColorValue cv;
    memset(&cv, 0, sizeof(cv));
    float c = code == 0x80 || code == 0xC0 ? 1.0f : 0.0f, a = code == 0x40 || code == 0xC0 ? 1.0f : 0.0f;
    cv.float32[0] = cv.float32[1] = cv.float32[2] = c, cv.float32[3] = a;
    if (code == 0x20 && clear) { /* the clear color registers hold the color in the surface's format */
        if (img->format == VK_FORMAT_R16G16B16A16_SFLOAT) {
            for (int k = 0; k < 4; k++) {
                uint16_t h = (uint16_t)(clear[k / 2] >> (16 * (k & 1)));
                uint32_t ex = (h >> 10) & 0x1F, mn = h & 0x3FF, bits = ((uint32_t)(h & 0x8000) << 16) |
                                                                   (ex ? ((ex + 112) << 23) | (mn << 13) : 0);
                memcpy(&cv.float32[k], &bits, 4);
            }
        } else if (vk_format_bytes(img->format) == 4 && img->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
            for (int k = 0; k < 4; k++) cv.float32[k] = (float)((clear[0] >> (8 * k)) & 0xFF) / 255.0f;
        }
    }
    end_pass(r);
    vk_barrier(r->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, img->layers};
    vkCmdClearColorImage(r->cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range);
    img->gpu_written = true;
    texture_gpu_write(r,img);
    img->written_seq = ++r->write_seq;
}

/* A DCC texture the GPU has not drawn here, cleared only through its metadata: Unity
 * fast-clears its camera target while a scene loads and shows it as is (The Rogue
 * Prince of Persia showed the stale memory under it, in 64 KiB blocks). The T# names
 * the keys (COMPRESSION_EN, dword6 bit 22; META_DATA_ADDRESS, dword6 [31:24] and
 * dword7): uniform clear codes give the texels, not the color memory. True when the
 * image now holds that clear. PS5C_NO_DCC_CLEAR=1 or PS5C_DCC_SAMPLE_CLEAR=0: off. */
static bool dcc_texture_clear(renderer *r, vk_image *img, const uint32_t *t, uint64_t bytes) {
    static int off = -1;
    if (off < 0) off = (getenv("PS5C_DCC_SAMPLE_CLEAR") && !atoi(getenv("PS5C_DCC_SAMPLE_CLEAR"))) ||
                       getenv("PS5C_NO_DCC_CLEAR") != NULL;
    if (off || !((t[6] >> 22) & 1) || !t[7] || img->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !bytes) return false;
    guest_addr start = ((guest_addr)t[7] << 16) | ((guest_addr)(t[6] >> 24) << 8);
    uint64_t size = (bytes + 255) / 256;
    static int log_on = -1;
    if (log_on < 0) log_on = getenv("PS5C_LOG_DCC") != NULL;
    uint8_t first = 0;
    int code = dcc_keys(r, img->addr, start, size, log_on, NULL, &first);
    static uint32_t logged;
    if (log_on && logged++ < 40)
        LOG_I(GPU, "DCC texture 0x%llx (%ux%u fmt %d): keys 0x%llx+0x%llx %s (first 0x%02x), flip %llu",
              (unsigned long long)img->addr, img->width, img->height, (int)img->format, (unsigned long long)start,
              (unsigned long long)size, code == -2 ? "not readable" : code < 0 ? "not a uniform clear" : "fast clear",
              first, (unsigned long long)g_trace_flip);
    if (code < 0 || code == 0x20) return false; /* 0x20: the CB clear words, which a texture does not carry */
    dcc_clear_image(r, img, code, NULL);
    return true;
}

static void dcc_target(renderer *r, const gpu *g, vk_image *img, uint32_t rt, uint64_t bytes) {
    vk_pipelines *P = r->pipes;
    uint32_t o = rt * GPU_CB_STRIDE;
    guest_addr start = ((guest_addr)gpu_reg(g, mmCB_COLOR0_DCC_BASE + o) << 8) |
                       ((guest_addr)(gpu_reg(g, mmCB_COLOR0_DCC_BASE_EXT + rt) & 0xFF) << 40);
    if (!start || !bytes) return;
    static int surface_keys=-1;
    if(surface_keys<0)surface_keys=getenv("PS5C_DCC_SURFACE_KEYS")!=NULL;
    dcc_entry *e = NULL;
    for (uint32_t i = 0; i < P->dcc_count && !e; i++)
        if (P->dccs[i].img == img->addr &&
            (!surface_keys ||
             (P->dccs[i].width==img->width && P->dccs[i].height==img->height &&
              P->dccs[i].layers==img->layers && P->dccs[i].format==img->format && P->dccs[i].start==start))) e = &P->dccs[i];
    if (!e) {
        e = &P->dccs[P->dcc_count < MAX_DCC ? P->dcc_count++ : P->dcc_next++ % MAX_DCC];
        memset(e, 0, sizeof(*e));
        e->img = img->addr;
        if(surface_keys)e->written=true; /* inspect initial clear metadata for this association */
    }
    e->start = start, e->end = start + (bytes + 255) / 256;
    e->width = img->width, e->height = img->height, e->layers = img->layers, e->format = img->format;
    e->clear[0] = gpu_reg(g, mmCB_COLOR0_CLEAR_WORD0 + o), e->clear[1] = gpu_reg(g, mmCB_COLOR0_CLEAR_WORD1 + o);
    dcc_apply(r, img, e, gpu_reg_addr256(g, mmSPI_SHADER_PGM_LO_PS), "target");
}

static void dcc_sample(renderer *r, vk_image *img) {
    static int off = -1; /* comparison with the former target-only handling */
    if (off < 0) off = (getenv("PS5C_DCC_SAMPLE_CLEAR") && !atoi(getenv("PS5C_DCC_SAMPLE_CLEAR"))) ||
                       getenv("PS5C_NO_DCC_CLEAR") != NULL;
    if (off) return;
    vk_pipelines *P = r->pipes;
    for (uint32_t i = 0; i < P->dcc_count; i++) {
        dcc_entry *e = &P->dccs[i];
        if (e->img != img->addr || !e->written || e->width != img->width || e->height != img->height ||
            e->layers != img->layers || e->format != img->format) continue;
        dcc_apply(r, img, e, 0, "sample");
        img->last_use = r->use_seq; /* consuming pending compute results may start a new batch */
        break;
    }
}

/* The largest index of a draw (index buffer copied to `indices`): a scan the
 * compiler vectorizes, kept per epoch for the draws that share the buffer. */
static uint32_t largest_index(vk_pipelines *P, const gpu_draw *d, const void *indices) {
    for (uint32_t i = 0; i < INDEX_MAX_CACHE; i++) {
        if (P->index_max[i].addr == d->index_addr && P->index_max[i].epoch == P->epoch &&
            P->index_max[i].count == d->count && P->index_max[i].size == d->index_size)
            return P->index_max[i].max;
    }
    uint32_t max = 0;
    if (d->index_size == 2) {
        const uint16_t *ix = (const uint16_t *)indices;
        uint16_t m = 0;
        for (uint32_t i = 0; i < d->count; i++) m = ix[i] > m ? ix[i] : m;
        max = m;
    } else {
        const uint32_t *ix = (const uint32_t *)indices;
        for (uint32_t i = 0; i < d->count; i++) max = ix[i] > max ? ix[i] : max;
    }
    uint32_t k = P->index_max_next++ % INDEX_MAX_CACHE;
    P->index_max[k].addr = d->index_addr;
    P->index_max[k].epoch = P->epoch;
    P->index_max[k].count = d->count;
    P->index_max[k].size = d->index_size;
    P->index_max[k].max = max;
    return max;
}

/* PS5C_SAVE_TEXTURES=<program hash> (+ PS5C_SAVE_TEXTURES_FRAME=<flip>|@<file>): the images the
 * first draw of that pixel shader samples go to user/frames/tex_*.bmp (debugging aid: textures that
 * come out black). do_draw remembers them, save_textures saves them after the draw is recorded. */
static vk_image *g_tex_save[2 * SH_MAX_SITES];
static uint32_t g_tex_save_count;

static bool tex_save_wanted(const gpu *g, const sh_program *ps) {
    static int have = -1;
    static uint64_t want;
    static bool done;
    if (have < 0) {
        have = getenv("PS5C_SAVE_TEXTURES") != NULL;
        want = have ? strtoull(getenv("PS5C_SAVE_TEXTURES"), NULL, 16) : 0;
    }
    if (!have || done || ps->hash != want) return false;
    static frame_trig trig;
    int64_t frame = frame_trigger(&trig, "PS5C_SAVE_TEXTURES_FRAME", g->stats.flips);
    if (frame != -2 && (int64_t)g->stats.flips != frame) return false;
    done = true;
    return true;
}

static void save_textures(renderer *r) {
    if (!g_tex_save_count) return;
    end_pass(r);
    sync_batch(r);
    for (uint32_t i = 0; i < g_tex_save_count; i++) {
        vk_image *img = g_tex_save[i];
        char path[128];
        snprintf(path, sizeof(path), "user/frames/tex_%02u_%010llx_%ux%u_f%d.bmp", i, (unsigned long long)img->addr,
                 img->width, img->height, (int)img->format);
        if (img->aspect == VK_IMAGE_ASPECT_COLOR_BIT) vk_save_image(r, img, path);
        else vk_save_depth(r, img, path);
        for (uint32_t j = 0; j < r->image_count; j++) {
            const vk_image *o = &r->images[j];
            if (o->image && o->addr == img->addr)
                LOG_I(GPU, "  image at 0x%llx: %ux%u format %d layers %u%s, written seq %llu", (unsigned long long)o->addr,
                      o->width, o->height, (int)o->format, o->layers, o->gpu_written ? " GPU written" : "",
                      (unsigned long long)o->written_seq);
        }
        const uint8_t *mem = (const uint8_t *)gmem_host(r->mem, img->addr, 65536);
        uint32_t nonzero = 0;
        for (uint32_t k = 0; mem && k < 65536; k++) nonzero += mem[k] != 0;
        LOG_I(GPU, "saved texture %u: 0x%llx %ux%u format %d, %u of the first 64 KB of its memory not zero%s", i,
              (unsigned long long)img->addr, img->width, img->height, (int)img->format, nonzero, img->gpu_written ? ", GPU written" : "");
        { /* nonzero bytes in 16 windows of 4 KiB spread over one byte per texel of every layer (streamed textures) */
            uint64_t span = (uint64_t)img->width * img->height * img->layers;
            char line[256];
            int n = 0;
            for (uint32_t k = 0; k < 16 && span >= 65536; k++) {
                const uint8_t *m = (const uint8_t *)gmem_host(r->mem, img->addr + span / 16 * k, 4096);
                uint32_t nz = 0;
                for (uint32_t b = 0; m && b < 4096; b++) nz += m[b] != 0;
                n += snprintf(line + n, sizeof(line) - (size_t)n, " %u", m ? nz : 9999);
            }
            if (n) LOG_I(GPU, "  memory spread (nonzero of 4096, %u layers):%s", img->layers, line);
        }
    }
    g_tex_save_count = 0;
    vk_begin(r);
}

#include "gpu/vk/vk_ngg_position_capture.h"

/* PS5C_VS_PROBE (shader_spirv.c): after the vertex shader's draws in PS5C_VS_PROBE_FRAME (first 8),
 * the values its probed vertex left past the end of the uniform buffer (0xeeeeeeee: not reached) */
/* Bounded readback diagnostic of the actual compute output, off by default. */
static void geometry_output_stats(renderer*r,const gpu*g,const stage_state*vs,const stage_state*ps) {
    static int enabled=-1;static uint32_t logged;static uint64_t shader;static int64_t last_from=-3;
    static const char *selection_file;static uint64_t selection_flip=UINT64_MAX;
    static frame_trig trigger;
    if(enabled<0) {
        enabled=getenv("PS5C_GS_OUTPUT_STATS")!=NULL;
        const char *s=getenv("PS5C_GS_OUTPUT_SHADER");shader=s?strtoull(s,NULL,16):0;
        if(s && s[0]=='@')selection_file=s+1;
    }
    if(enabled && selection_file && selection_flip!=g->stats.flips) {
        selection_flip=g->stats.flips;FILE*f=fopen(selection_file,"r");unsigned long long value=0;
        if(f){if(fscanf(f,"%llx",&value)!=1)value=0;fclose(f);}
        if(shader!=(uint64_t)value){shader=value;logged=0;}
    }
    if(!enabled || !vs->info.mesh_compute || (shader && shader!=vs->prog->hash && shader!=vs->prog->base))return;
    int64_t from=frame_trigger(&trigger,"PS5C_GS_OUTPUT_FROM",g->stats.flips);
    if(from!=last_from){last_from=from;logged=0;}
    if(g->stats.flips<330 || logged>=16 || from==-1 || (from>=0 && g->stats.flips<(uint64_t)from))return;
    logged++;end_pass(r);sync_batch(r);
    LOG_I(GPU,"GS diagnostic state: base %llx PS %llx max vertices %u primitives %u params %08x workgroup %u",
        (unsigned long long)vs->prog->base,(unsigned long long)gpu_reg_addr256(g,mmSPI_SHADER_PGM_LO_PS),
        vs->info.mesh_max_vertices,vs->info.mesh_max_primitives,vs->prog->param_mask,vs->info.local_size[0]);
    uint64_t largest=0;guest_addr largest_addr=0;uint32_t largest_site=UINT32_MAX;
    for(uint32_t s=0;s<vs->prog->site_count;s++) {
        const sh_site_value*v=vs->eval.site+s;sh_site_kind kind=vs->prog->sites[s].kind;
        if(!v->valid || v->desc_unknown || (kind!=SH_SITE_BUFFER && kind!=SH_SITE_SBUFFER))continue;
        uint32_t stride=(v->desc[1]>>16)&0x3fff;uint64_t bytes=stride?(uint64_t)stride*v->desc[2]:v->desc[2];
        if(bytes>largest){largest=bytes;largest_site=s;largest_addr=(uint64_t)v->desc[0]|((uint64_t)(v->desc[1]&0xffff)<<32);}
    }
    LOG_I(GPU,"GS resources: VS stores %u PS stores %u largest known buffer %llx %llu bytes imports %u",
        vs->prog->has_stores,ps->prog->has_stores,(unsigned long long)largest_addr,(unsigned long long)largest,
        vs->large_reads?vs->large_reads->count:0);
    if(largest_site!=UINT32_MAX) {
        const uint32_t*u=arena_ptr(r,vs->u_off);
        LOG_I(GPU,"GS largest binding: site %u explicit length %u actual bound dwords %u",
            largest_site,vs->shapes[largest_site].buffer_length,
            vs->shapes[largest_site].buffer_length?u[SH_U_SITE+largest_site]:0);
    }
    for(uint32_t i=0;vs->large_reads && i<vs->large_reads->count;i++)
        LOG_I(GPU,"GS imported span %u: %llx %llu bytes",i,
            (unsigned long long)vs->large_reads->entries[i].base,
            (unsigned long long)vs->large_reads->entries[i].size);
    const uint32_t*data=arena_ptr(r,vs->mesh_output_off);
    uint32_t stride=sh_mesh_output_words(vs->prog,&vs->info),record=4+4*(uint32_t)__builtin_popcount(vs->prog->param_mask);
    uint64_t vertices=0,primitives=0,valid=0,positive=0,zero=0,nonfinite=0,bad=0;
    uint64_t positive_triangles=0,projected_triangles=0;
    uint64_t outside=0,culled=0,out_of_bounds=0,repeated=0;float example[3][4]={{0}};bool have_example=false;
    for(uint32_t group=0;group<vs->mesh_output_groups;group++) {
        const uint32_t*w=data+(uint64_t)group*stride;uint32_t nv=w[4],np=w[5];
        if(nv>vs->info.mesh_max_vertices || np>vs->info.mesh_max_primitives){bad++;continue;}
        vertices+=nv;primitives+=np;
        for(uint32_t v=0;v<nv;v++) {
            float pos[4];memcpy(pos,w+8+(uint64_t)v*record,16);
            positive+=pos[3]>0;zero+=pos[3]==0;
            nonfinite+=!isfinite(pos[0]) || !isfinite(pos[1]) || !isfinite(pos[2]) || !isfinite(pos[3]);
        }
        const uint32_t*pr=w+8+(uint64_t)vs->info.mesh_max_vertices*record;
        if(group<4) {
            char sample[192];int used=0;
            for(uint32_t j=0;j<np && j<8;j++)
                used+=snprintf(sample+used,sizeof(sample)-(size_t)used," %08x",pr[j]);
            LOG_I(GPU,"GS group %u: vertices %u primitives %u words:%s",group,nv,np,used?sample:"");
        }
        for(uint32_t j=0;j<np;j++) {
            uint32_t packed=pr[j],x=packed&511,y=(packed>>10)&511,z=(packed>>20)&511;
            culled+=(packed>>31)!=0;
            out_of_bounds+=x>=nv || y>=nv || z>=nv;
            repeated+=x==y || x==z || y==z;
            bool fits=!(packed>>31) && x<nv && y<nv && z<nv && x!=y && x!=z && y!=z;
            valid+=fits;
            if(fits) {
                uint32_t indices[]={x,y,z};double ndc[3][2];bool front=true;
                uint32_t planes=63;float positions[3][4];
                for(unsigned k=0;k<3;k++) {
                    float pos[4];memcpy(pos,w+8+(uint64_t)indices[k]*record,16);
                    memcpy(positions[k],pos,16);
                    bool finite=isfinite(pos[0]) && isfinite(pos[1]) && isfinite(pos[2]) && isfinite(pos[3]);
                    front &= finite && pos[3]>0;
                    uint32_t mask=(pos[0]<-pos[3]) | ((pos[0]>pos[3])<<1) |
                        ((pos[1]<-pos[3])<<2) | ((pos[1]>pos[3])<<3) | ((pos[2]<0)<<4) | ((pos[2]>pos[3])<<5);
                    planes&=mask;
                    for(unsigned axis=0;axis<2;axis++)ndc[k][axis]=finite && pos[3]>0?(double)pos[axis]/pos[3]:0;
                }
                positive_triangles+=front;
                double area=(ndc[1][0]-ndc[0][0])*(ndc[2][1]-ndc[0][1])-(ndc[1][1]-ndc[0][1])*(ndc[2][0]-ndc[0][0]);
                projected_triangles+=front && fabs(area)>1e-12;
                outside+=front && planes!=0;
                if(front && !have_example){memcpy(example,positions,sizeof(example));have_example=true;}
            }
        }
    }
    LOG_I(GPU,"GS output %016llx flip %llu: %u groups, %llu vertices, %llu primitives, %llu index-valid triangles, positive W %llu zero W %llu nonfinite %llu bad headers %llu; positive-W triangles %llu, nondegenerate projected triangles %llu",
        (unsigned long long)vs->prog->hash,(unsigned long long)g->stats.flips,vs->mesh_output_groups,
        (unsigned long long)vertices,(unsigned long long)primitives,(unsigned long long)valid,
        (unsigned long long)positive,(unsigned long long)zero,(unsigned long long)nonfinite,(unsigned long long)bad,
        (unsigned long long)positive_triangles,(unsigned long long)projected_triangles);
    LOG_I(GPU,"GS rejected primitives (overlapping counts): culled %llu, out of bounds %llu, repeated indices %llu",
        (unsigned long long)culled,(unsigned long long)out_of_bounds,(unsigned long long)repeated);
    LOG_I(GPU,"GS clip: %llu triangles outside a common clip plane; example valid %u: [%.9g %.9g %.9g %.9g] [%.9g %.9g %.9g %.9g] [%.9g %.9g %.9g %.9g]",
        (unsigned long long)outside,have_example,example[0][0],example[0][1],example[0][2],example[0][3],
        example[1][0],example[1][1],example[1][2],example[1][3],example[2][0],example[2][1],example[2][2],example[2][3]);
    vk_begin(r);
}

static void vs_probe_read(renderer *r, const gpu *g, const gpu_draw *d, const stage_state *vs, const sh_program *ps) {
    static int have = -1;
    static uint64_t want[4], want_ps[4]; /* PS5C_VS_PROBE_PS=<hash>[,<hash>...]: only the draws with those pixel shaders */
    static uint32_t nwant, nps, max_probes = 12;
    static guest_addr want_depth;
    if (have < 0) {
        const char *e = getenv("PS5C_VS_PROBE");
        have = e != NULL;
        for (const char *q = e; q && nwant < 4; q = strchr(q, ';') ? strchr(q, ';') + 1 : NULL) want[nwant++] = strtoull(q, NULL, 16);
        for (const char *q = getenv("PS5C_VS_PROBE_PS"); q && nps < 4; q = strchr(q, ',') ? strchr(q, ',') + 1 : NULL)
            want_ps[nps++] = strtoull(q, NULL, 16);
        /* A VS can serve shadow maps as well as the main depth prepass.
         * Keep bounded diagnostics from consuming their quota on shadows. */
        const char *depth = getenv("PS5C_VS_PROBE_DEPTH"), *limit = getenv("PS5C_VS_PROBE_MAX");
        if (depth) want_depth = strtoull(depth, NULL, 16);
        if (limit) { unsigned long n = strtoul(limit, NULL, 10); if (n) max_probes = n > 256 ? 256 : (uint32_t)n; }
    }
    static uint32_t per[4]; /* 12 lines for each probed shader */
    int which = -1;
    bool ps_match = !nps;
    for (uint32_t i = 0; vs->prog && i < nwant; i++)
        if (vs->prog->hash == want[i]) which = (int)i;
    for (uint32_t i = 0; ps && i < nps; i++) ps_match |= ps->hash == want_ps[i];
    if (!have || which < 0 || !ps_match || per[which] >= max_probes) return;
    if (want_depth && ((guest_addr)gpu_reg(g, mmDB_Z_READ_BASE) << 8) != want_depth) return;
    static int64_t want_count = -2; /* PS5C_VS_PROBE_COUNT=<n>: only draws of n indices */
    if (want_count == -2) want_count = getenv("PS5C_VS_PROBE_COUNT") ? atoll(getenv("PS5C_VS_PROBE_COUNT")) : -1;
    if (want_count >= 0 && (!d || (int64_t)d->count != want_count)) return;
    static frame_trig trigger;
    int64_t frame = frame_trigger(&trigger,"PS5C_VS_PROBE_FRAME",g->stats.flips);
    if (frame != -2 && frame != (int64_t)g->stats.flips) return;
    end_pass(r);
    sync_batch(r);
    const uint32_t *w = (const uint32_t *)((const uint8_t *)arena_ptr(r, vs->u_off) + vs->u_bytes);
    if (w[15] == 0xEEEEEEEEu) { /* the vertex is not in this draw */
        vk_begin(r);
        return;
    }
    per[which]++;
    if(vs->info.stage==SH_STAGE_MESH) {
        LOG_I(GPU,"MESH probe flip %llu shader %016llx draw %u: allocation %u vertices/%u primitives, capacity %u/%u, lane %u primitive %08x; totals %u/%u in %u groups, max %u/%u, valid triangles %u",
              (unsigned long long)g->stats.flips,(unsigned long long)vs->prog->hash,d?d->count:0,
              w[0],w[1],w[2],w[3],w[15],w[4],w[8],w[9],w[10],w[11],w[12],w[13]);
        uint32_t values[]={w[5],w[6],w[7],w[14]};float f[4];memcpy(f,values,sizeof(f));
        LOG_I(GPU,"MESH values shader %016llx lane %u: %08x %08x %08x %08x = %g %g %g %g (position, or selected PC/VGPR probes)",
              (unsigned long long)vs->prog->hash,w[15],values[0],values[1],values[2],values[3],
              (double)f[0],(double)f[1],(double)f[2],(double)f[3]);
        if(w[23]!=0xEEEEEEEEu)LOG_I(GPU,"MESH extra shader %016llx: %08x %08x %08x %08x",
              (unsigned long long)vs->prog->hash,w[23],w[24],w[25],w[26]);
        if(!vs->info.mesh_compute)LOG_I(GPU,"MESH clip shader %016llx: positions %u, positive W %u, negative W %u, zero W %u, inside %u, NaN %u, Inf %u; VTE %08x CLIP %08x",
              (unsigned long long)vs->prog->hash,w[16],w[17],w[18],w[19],w[20],w[21],w[22],
              gpu_reg(g,mmPA_CL_VTE_CNTL),gpu_reg(g,mmPA_CL_CLIP_CNTL));
        vk_begin(r);return;
    }
    float f[8];
    memcpy(f, w, sizeof(f));
    if (w[15] == 7) { /* the first 4 vertices: index, x/w, y/w */
        float v[12];
        memcpy(v, w, sizeof(v));
        LOG_I(GPU, "VS probe flip %llu VS %016llx PS %016llx count %u: %u %g,%g  %u %g,%g  %u %g,%g  %u %g,%g",
              (unsigned long long)g->stats.flips, (unsigned long long)vs->prog->hash, (unsigned long long)(ps ? ps->hash : 0), d ? d->count : 0,
              w[0], (double)v[1], (double)v[2], w[3], (double)v[4], (double)v[5], w[6], (double)v[7], (double)v[8], w[9], (double)v[10],
              (double)v[11]);
        vk_begin(r);
        return;
    }
    LOG_I(GPU, "VS probe flip %llu VS %016llx PS %016llx index 0x%llx count %u vertex %u: %08x %08x %08x %08x %08x %08x %08x %08x = %g %g %g %g %g %g %g %g",
          (unsigned long long)g->stats.flips, (unsigned long long)vs->prog->hash, (unsigned long long)(ps ? ps->hash : 0),
          (unsigned long long)(d?d->index_addr:0),d?d->count:0,w[15], w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], (double)f[0], (double)f[1],
          (double)f[2], (double)f[3], (double)f[4], (double)f[5], (double)f[6], (double)f[7]);
    vk_begin(r);
}

/* Read only the selected pixel's sidecar. This never substitutes a color,
 * relaxes a discard, changes guest registers, or consumes guest resources. */
static void ps_capture_read(renderer *r,const gpu *g,const stage_state *ps) {
    static bool initialized;static uint64_t wanted;static unsigned logged;
    if(!initialized){const char *s=getenv("PS5C_PS_CAPTURE");if(s)wanted=strtoull(s,NULL,16);initialized=true;}
    if(!wanted || !ps->prog || ps->prog->hash!=wanted)return;
    static frame_trig trigger;
    int64_t frame=frame_trigger(&trigger,"PS5C_PS_CAPTURE_FRAME",g->stats.flips);
    if(frame!=-2 && frame!=(int64_t)g->stats.flips)return;
    static int64_t captured_frame=-3;
    if(captured_frame!=frame){captured_frame=frame;logged=0;}
    if(logged>=8)return;
    end_pass(r);sync_batch(r);
    const uint32_t *w=(const uint32_t *)((const uint8_t *)arena_ptr(r,ps->u_off)+ps->u_bytes);
    if(w[15]!=0xEEEEEEEEu) {
        logged++;
        LOG_I(GPU,"PS register capture flip %llu shader %016llx pixel %u,%u: %08x %08x %08x %08x %08x %08x %08x %08x; matching fragments %u",
              (unsigned long long)g->stats.flips,(unsigned long long)wanted,w[10],w[11],w[0],w[1],w[2],w[3],w[4],w[5],w[6],w[7],w[12]);
    }
    vk_begin(r);
}

/* PS5C_GPU_STATS: where do_draw gave up and asked for a fresh batch */
static bool draw_fail(uint32_t line) {
    for (int i = 0; i < 8; i++)
        if (g_stat_draw_fail[i].line == line || !g_stat_draw_fail[i].n) {
            g_stat_draw_fail[i].line = line;
            g_stat_draw_fail[i].n++;
            break;
        }
    return false;
}

static bool do_draw(renderer *r, const gpu *g, const gpu_draw *d, stage_state *vs, stage_state *ps) {
    uint64_t t_phase = stat_now();
    /* Diagnostic settings are process configuration, just like frame_trigger's
     * environment. Avoid taking the CRT environment lock for every target/draw.
     * File based triggers still read their files at runtime. */
    static bool debug_init, log_depth, log_alias, no_dcc_clear;
    static int64_t log_cb_frame, log_depth_frame, log_scissor_from;
    if (!debug_init) {
        const char *cb=getenv("PS5C_LOG_CB_FRAME"),*depth=getenv("PS5C_LOG_DEPTH_FRAME"),
                   *scissor=getenv("PS5C_LOG_SCISSOR");
        log_cb_frame=cb?atoll(cb):-1;log_depth_frame=depth?atoll(depth):-1;
        log_scissor_from=scissor?atoll(scissor):-1;
        log_depth=getenv("PS5C_LOG_DEPTH")!=NULL;
        log_alias=getenv("PS5C_LOG_IMAGE_ALIAS")!=NULL;
        no_dcc_clear=getenv("PS5C_NO_DCC_CLEAR")!=NULL;
        debug_init=true;
    }
    uint64_t target_batch = r->use_seq;
    vk_pipelines *P = r->pipes;
    /* render targets: the color outputs the pixel shader writes */
    gfx_key k;
    gfx_key_reset(&k, P->draw_scratch_enabled);
    vk_image *rts[GPU_MAX_RENDER_TARGETS] = {0};
    VkImageView color_views[GPU_MAX_RENDER_TARGETS] = {0};
    uint32_t fb_w = 0xFFFF, fb_h = 0xFFFF, fb_layers = 1;
    bool color_on = ((gpu_reg(g, mmCB_COLOR_CONTROL) >> 4) & 7) != 0;
    for (uint32_t i = 0; i < GPU_MAX_RENDER_TARGETS && color_on; i++) {
        if (!((ps->prog->mrt_mask >> i) & 1)) continue;
        uint32_t o = i * GPU_CB_STRIDE, info = gpu_reg(g, mmCB_COLOR0_INFO + o), a2 = gpu_reg(g, mmCB_COLOR0_ATTRIB2 + i);
        guest_addr base = (guest_addr)gpu_reg(g, mmCB_COLOR0_BASE + o) << 8;
        VkFormat f = cb_format(info);
        k.rt_count = i + 1;
        if (!base || f == VK_FORMAT_UNDEFINED ||
            !((gpu_reg(g, mmCB_TARGET_MASK) >> (4 * i)) & color_component_mask(f))) continue;
        uint32_t w0 = ((a2 >> 14) & 0x3FFF) + 1, h0 = (a2 & 0x3FFF) + 1;
        uint32_t level = vk_color_mip(gpu_reg(g, mmCB_COLOR0_VIEW + o));
        uint32_t w = vk_color_mip_size(w0, level), h = vk_color_mip_size(h0, level);
        /* CB_COLOR_ATTRIB3: MIP0_DEPTH [12:0], RESOURCE_TYPE [25:24] (2: 3D).
         * Keep one volume whether gl_Layer or CB_COLOR_VIEW selects its slices. */
        uint32_t a3 = gpu_reg(g, mmCB_COLOR0_ATTRIB3 + i), slices = (a3 & 0x1FFF) + 1;
        if (((a3 >> 24) & 3) == 2) slices = vk_color_mip_size(slices, level);
        if (((a3 >> 24) & 3) == 2 && slices > 1) {
            rts[i] = vk_image_get_3d(r, base, w, h, slices, f);
            if (rts[i] && vs->info.vs_layer && r->vs_layer) {
                color_views[i] = rts[i]->array_view;
                if (slices > fb_layers) fb_layers = slices;
            } else if (rts[i]) {
                color_views[i] = color_slice_view(r, rts[i], gpu_reg(g, mmCB_COLOR0_VIEW + o) & 0x1fff);
                if (!color_views[i]) return true;
            }
        } else {
            /* CB_COLOR_VIEW SLICE_START [10:0] / SLICE_MAX [23:13] on a 2D target: one layer of an
             * array (Unreal draws each face of its reflection-capture cube this way, then filters
             * the cube; every face went to one 2D image and the cube held garbage). Kept in the
             * array image at this address, grown as faces come; PS5C_NO_COLOR_SLICES=1: the old way. */
            static int no_slices = -1;
            if (no_slices < 0) no_slices = getenv("PS5C_NO_COLOR_SLICES") != NULL;
            uint32_t cv = gpu_reg(g, mmCB_COLOR0_VIEW + o), s0 = cv & 0x7FF, smax = (cv >> 13) & 0x7FF;
            vk_image *arr = no_slices ? NULL : vk_image_find_array(r, base, w, h, f);
            if (!no_slices && (s0 || smax || arr) && s0 < 64 && smax < 64) {
                uint32_t need = (smax > s0 ? smax : s0) + 1;
                if (!arr || arr->layers < need) {
                    vk_image *old = arr ? arr : vk_image_find(r, base, w, h, f);
                    arr = old ? grow_layers(r, old, f, need) : vk_image_get_layers(r, base, w, h, f, need);
                }
                if (arr) {
                    rts[i] = arr;
                    if (vs->info.vs_layer && r->vs_layer && smax > s0 && arr->array_view) { /* gl_Layer picks */
                        color_views[i] = arr->array_view;
                        if (arr->layers > fb_layers) fb_layers = arr->layers;
                    } else {
                        color_views[i] = color_slice_view(r, arr, s0);
                        if (!color_views[i]) return true;
                    }
                }
            }
            if (!rts[i]) {
                r->scale_new = true; /* PS5C_RT_SCALE */
                rts[i] = vk_image_get(r, base, w, h, f);
                r->scale_new = false;
            }
        }
        if (!rts[i]) {
            if (r->image_cache_pressure) return draw_fail(__LINE__);
            continue;
        }
        if(!rts[i]->color_attachment) {
            static uint32_t unsupported;
            if(unsupported++<8) LOG_W(GPU,"color target format %d is sampling-only on this host",(int)rts[i]->format);
            P->skipped++;return true;
        }
        if (((info >> 28) & 1) && !no_dcc_clear) /* DCC_ENABLE */
            dcc_target(r, g, rts[i], i, (uint64_t)w * h * vk_format_bytes(f));
        if (log_cb_frame >= 0 && (int64_t)g->stats.flips == log_cb_frame) {
            static guest_addr seen[64];
            static uint32_t nseen;
            bool known = false;
            for (uint32_t s = 0; s < nseen; s++) known |= seen[s] == base;
            if (!known && nseen < 64) {
                seen[nseen++] = base;
                LOG_I(GPU, "CB RT%u 0x%llx %ux%u info %08x dcc_ctl %08x dcc_base 0x%llx cmask 0x%llx clear %08x %08x",
                      i, (unsigned long long)base, w, h, info, gpu_reg(g, mmCB_COLOR0_DCC_CONTROL + o),
                      ((unsigned long long)gpu_reg(g, mmCB_COLOR0_DCC_BASE + o) << 8) |
                          ((unsigned long long)(gpu_reg(g, mmCB_COLOR0_DCC_BASE_EXT + i) & 0xFF) << 40),
                      ((unsigned long long)gpu_reg(g, mmCB_COLOR0_CMASK + o) << 8) |
                          ((unsigned long long)(gpu_reg(g, mmCB_COLOR0_CMASK_BASE_EXT + i) & 0xFF) << 40),
                      gpu_reg(g, mmCB_COLOR0_CLEAR_WORD0 + o), gpu_reg(g, mmCB_COLOR0_CLEAR_WORD1 + o));
            }
        }
        if (trace_image(rts[i]))
            LOG_I(GPU, "trace: draw PS 0x%llx (%016llx) RENDERS RT%u image 0x%llx fmt %d, blend 0x%08x, view %08x attrib %08x, "
                       "%u vertices x%u, vs_out_cntl %08x, VS %016llx, attrib3 %08x, GE_INDX_OFFSET %d, "
                       "depth_ctl %08x render_ctl %08x ZR %08x ZW %08x bounds %08x/%08x sctl %08x ref %08x/%08x",
                  (unsigned long long)ps->prog->base, (unsigned long long)ps->prog->hash, i, (unsigned long long)base, (int)f,
                  gpu_reg(g, mmCB_BLEND0_CONTROL + i), gpu_reg(g, mmCB_COLOR0_VIEW + o), gpu_reg(g, mmCB_COLOR0_ATTRIB + o), d->count,
                  d->instances, gpu_reg(g, 0xA207u), (unsigned long long)vs->prog->hash, gpu_reg(g, mmCB_COLOR0_ATTRIB3 + i),
                  (int32_t)gpu_reg(g, mmGE_INDX_OFFSET),gpu_reg(g,mmDB_DEPTH_CONTROL),gpu_reg(g,mmDB_RENDER_CONTROL),
                  gpu_reg(g,mmDB_Z_READ_BASE),gpu_reg(g,mmDB_Z_WRITE_BASE),
                  gpu_reg(g,0xA008u /* DB_DEPTH_BOUNDS_MIN */),gpu_reg(g,0xA009u /* DB_DEPTH_BOUNDS_MAX */), gpu_reg(g, mmDB_STENCIL_CONTROL), gpu_reg(g, mmDB_STENCILREFMASK), gpu_reg(g, mmDB_STENCILREFMASK_BF));
        if (!rts[i]->gpu_written && rts[i]->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            /* A new target starts with what its memory holds: draws may add to
             * contents the CPU wrote (Minecraft's texture atlas takes its
             * animated tiles by draws, the rest from the CPU). */
            static const uint8_t k_img_fmt[16] = {0, 1, 7, 14, 20, 23, 36, 0, 0, 50, 56, 64, 71, 0, 77, 0};
            uint32_t ifmt = k_img_fmt[(info >> 2) & 0xF], sw = (gpu_reg(g, mmCB_COLOR0_ATTRIB3 + i) >> 14) & 0x1F;
            if (ifmt && !((info >> 2) & 0x10)) {
                /* Keep mip-zero dimensions in T#, selecting the target's level
                 * for CPU initialization at its actual guest-memory offset. */
                uint32_t max_mip = a2 >> 28;
                uint32_t t[8] = {(uint32_t)(base >> 8), (uint32_t)((base >> 40) & 0xFF) | (ifmt << 20) | (((w0 - 1) & 3) << 30),
                                 (((w0 - 1) >> 2) & 0x3FFF) | ((h0 - 1) << 14),
                                 (sw << 20) | (max_mip << 16) | (level << 12), 0, 0, 0, 0};
                texture(r, t);
            }
        }
        k.rt_fmt[i] = f;
        if (w < fb_w) fb_w = w;
        if (h < fb_h) fb_h = h;
    }
    uint32_t bound = 0;
    for (uint32_t i = 0; i < k.rt_count; i++) bound += rts[i] != NULL;
    /* depth target */
    vk_image *zimg = NULL;
    depth_bias_state bias = {0};
    float far_depth_offset = 0.0f;
    VkImageView zview = VK_NULL_HANDLE;
    uint32_t zslice = 0;
    bool zclear = false, sclear = false;
    vk_stencil_face sface[2] = {{0}};
    {
        guest_addr za;
        uint32_t zw, zh;
        VkFormat zf;
        if (depth_target(g, &za, &zw, &zh, &zf)) zimg = depth_image(r, g, za, zw, zh, zf, &zslice, &zview);
        if (zimg && !sync_depth_alias(r, zimg)) return draw_fail(__LINE__);
        if (zimg) {
            uint32_t ctl = gpu_reg(g, mmDB_DEPTH_CONTROL);
            if (g_stats_on && (ctl & 1)) g_stat_stencil_draws++, g_stat_stencil_info = gpu_reg(g, mmDB_STENCIL_INFO);
            zclear = gpu_reg(g, mmDB_RENDER_CONTROL) & 1;
            k.depth_fmt = zimg->format;
            k.depth_test = (ctl >> 1) & 1;
            k.depth_write = k.depth_test && ((ctl >> 2) & 1) && !zclear;
            k.depth_func = (ctl >> 4) & 7;
            { /* PS5C_DEPTH_EQUAL_PROBE=@file: compare a material EQUAL pass
               * with LEQUAL (3) or GEQUAL (6) in a running diagnostic. Zero
               * restores the guest function; depth writes are never changed. */
                static const char *file;
                static bool checked;
                static uint64_t flip = UINT64_MAX;
                static unsigned mode;
                if (!checked) {
                    const char *v = getenv("PS5C_DEPTH_EQUAL_PROBE");
                    if (v && *v == '@') file = v + 1;
                    checked = true;
                }
                if (file && flip != g->stats.flips) {
                    flip = g->stats.flips;
                    unsigned value = 0;
                    FILE *f = fopen(file, "r");
                    if (f) { if (fscanf(f, "%u", &value) != 1) value = 0; fclose(f); }
                    if (value != 3 && value != 6) value = 0;
                    if (mode != value) LOG_I(GPU, "depth EQUAL probe: %u (diagnostic only)", value);
                    mode = value;
                }
                if (mode && k.depth_test && !k.depth_write && k.rt_count && k.depth_func == 2)
                    k.depth_func = mode;
            }
            if (vk_format_has_stencil(zimg->format)) {
                sclear = (gpu_reg(g, mmDB_RENDER_CONTROL) >> 1) & 1;
                if (ctl & 1) {
                    k.stencil_func = ctl & 0x00700781u; /* STENCIL_ENABLE, BACKFACE_ENABLE, STENCILFUNC, STENCILFUNC_BF */
                    uint32_t ops = gpu_reg(g, mmDB_STENCIL_CONTROL), front = ops & 0xFFFu, back = 0;
                    stencil_face(gpu_reg(g, mmDB_STENCILREFMASK), (ctl >> 8) & 7, &front, &sface[0], gpu_reg(g, mmDB_SHADER_CONTROL));
                    if ((ctl >> 7) & 1) {
                        back = (ops >> 12) & 0xFFFu;
                        stencil_face(gpu_reg(g, mmDB_STENCILREFMASK_BF), (ctl >> 20) & 7, &back, &sface[1], gpu_reg(g, mmDB_SHADER_CONTROL));
                    }
                    k.stencil_ops = front | (back << 12);
                }
            }
            bias = depth_bias(g, zimg->format, k.depth_test, k.depth_func, k.rt_count);
            uint32_t vte = gpu_reg(g, mmPA_CL_VTE_CNTL);
            if (bias.on && (zimg->format == VK_FORMAT_D32_SFLOAT || zimg->format == VK_FORMAT_D32_SFLOAT_S8_UINT) &&
                vk_far_plane_bias(vs->prog, ps->prog,
                                  (vte & 16) ? f32_of(gpu_reg(g, mmPA_CL_VPORT_ZSCALE)) : 1.0f,
                                  (vte & 32) ? f32_of(gpu_reg(g, mmPA_CL_VPORT_ZOFFSET)) : 0.0f,
                                  bias.constant, bias.clamp, &far_depth_offset)) bias = (depth_bias_state){0};
            k.depth_bias = bias.on;
            if (zimg->width < fb_w) fb_w = zimg->width;
            if (zimg->height < fb_h) fb_h = zimg->height;
            guest_addr metadata=0;
            if (htile_take_clear(P, g, zimg, zslice, &metadata) && htile_says_cleared(r, zimg, metadata)) {
                if (trace_image(zimg))
                    LOG_I(GPU,"trace: depth 0x%llx slice %u fast clear %08x HTILE %08x before VS %016llx PS %016llx, %u vertices, index 0x%llx",
                          (unsigned long long)zimg->addr,zslice,gpu_reg(g,mmDB_DEPTH_CLEAR),gpu_reg(g,mmDB_HTILE_DATA_BASE),
                          (unsigned long long)vs->prog->hash,(unsigned long long)ps->prog->hash,d->count,
                          (unsigned long long)d->index_addr);
                /* fast clear: the whole surface takes the clear value */
                end_pass(r);
                vk_barrier(r->cmd, zimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkClearDepthStencilValue cv = {f32_of(gpu_reg(g, mmDB_DEPTH_CLEAR)), 0};
                VkImageSubresourceRange range = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, zslice, 1};
                vkCmdClearDepthStencilImage(r->cmd, zimg->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range);
                if (log_depth_frame >= 0 && (int64_t)g->stats.flips == log_depth_frame)
                    LOG_I(GPU, "depth 0x%llx: fast clear to %g applied", (unsigned long long)zimg->addr, cv.depth);
                zimg->gpu_written = true;
                zimg->written_seq = ++r->write_seq;
            }
        }
    }
    /* DCC/HTILE metadata can complete a batch. Select all targets again so
     * subsequent texture allocations cannot evict an earlier target. */
    if (r->image_cache_pressure || r->use_seq != target_batch) return draw_fail(__LINE__);
    t_phase = stat_phase(t_phase); /* targets */
    if (zimg && trace_image(zimg))
        LOG_I(GPU,"trace: depth draw 0x%llx ctl %08x RTs %u VS 0x%llx (%016llx) PS 0x%llx (%016llx) index 0x%llx/%u count %u x%u offset %d cull %08x",
              (unsigned long long)zimg->addr,gpu_reg(g,mmDB_DEPTH_CONTROL),bound,
              (unsigned long long)vs->prog->base,(unsigned long long)vs->prog->hash,
              (unsigned long long)ps->prog->base,(unsigned long long)ps->prog->hash,
              (unsigned long long)d->index_addr,d->index_size,d->count,d->instances,
              (int32_t)gpu_reg(g,mmGE_INDX_OFFSET),gpu_reg(g,mmPA_SU_SC_MODE_CNTL));
    if (!bound && !zimg) return true; /* nothing visible to write */
    if (log_depth) {
        static uint32_t logged; /* PS5C_LOG_DEPTH_FRAME=<flip>: the draws of that frame */
        static int64_t frame = -2;
        static uint32_t max; /* PS5C_LOG_DEPTH_MAX: lines (400) */
        if (frame == -2) {
            frame = getenv("PS5C_LOG_DEPTH_FRAME") ? atoll(getenv("PS5C_LOG_DEPTH_FRAME")) : -1;
            max = getenv("PS5C_LOG_DEPTH_MAX") ? (uint32_t)atoi(getenv("PS5C_LOG_DEPTH_MAX")) : 400;
        }
        if ((frame < 0 || (int64_t)g->stats.flips == frame) && logged++ < max)
            LOG_I(GPU, "depth: %s 0x%llx %ux%u ctl %08x render %08x clear %08x zinfo %08x vte %x zscale %08x zoffset %08x "
                       "clip %08x colors %u",
                  zimg ? "on" : "off", zimg ? (unsigned long long)zimg->addr : 0ull, zimg ? zimg->width : 0,
                  zimg ? zimg->height : 0, gpu_reg(g, mmDB_DEPTH_CONTROL), gpu_reg(g, mmDB_RENDER_CONTROL),
                  gpu_reg(g, mmDB_DEPTH_CLEAR), gpu_reg(g, mmDB_Z_INFO), gpu_reg(g, mmPA_CL_VTE_CNTL),
                  gpu_reg(g, mmPA_CL_VPORT_ZSCALE), gpu_reg(g, mmPA_CL_VPORT_ZOFFSET), gpu_reg(g, mmPA_CL_CLIP_CNTL), bound);
    if (log_depth_frame >= 0 && (int64_t)g->stats.flips == log_depth_frame)
        LOG_I(GPU, "  draw PS %016llx, %u vertices", (unsigned long long)ps->prog->hash, d->count);
    }

    k.vs = vs->prog->base;
    k.ps = ps->prog->base;
    k.vs_hash = vs->prog->hash;
    k.ps_hash = ps->prog->hash;
    k.ps_wave32 = ps->prog->wave32;
    if (!P->draw_scratch_enabled) {
        memcpy(k.vs_shapes, vs->shapes, sizeof(sh_site_shape) * vs->prog->site_count);
        memcpy(k.ps_shapes, ps->shapes, sizeof(sh_site_shape) * ps->prog->site_count);
    }
    k.prim = gpu_reg(g, mmVGT_PRIMITIVE_TYPE);
    k.mesh=vs->info.stage==SH_STAGE_MESH?(vs->info.mesh_compute?2:1):0;
    k.mesh_threads=vs->info.stage==SH_STAGE_MESH?vs->info.local_size[0]:0;
    k.mesh_inputs=vs->info.mesh_input_vertices;
    k.mesh_vertices=vs->info.mesh_max_vertices;
    k.mesh_primitives=vs->info.mesh_max_primitives;
    k.mesh_primitive_vertices=vs->info.mesh_primitive_vertices;
    k.mesh_indexed=vs->info.mesh_indexed;
    if(vs->info.mesh_compute)vs->info.mesh_output_binding=sh_binding_count_shapes(vs->prog,vs->shapes)+sh_binding_count_shapes(ps->prog,ps->shapes);
    k.cull = gpu_reg(g, mmPA_SU_SC_MODE_CNTL) & 7u;
    { /* PS5C_NO_ALPHA_TO_MASK=1: ignore it (comparison) */
        static int no_a2m = -1;
        if (no_a2m < 0) no_a2m = getenv("PS5C_NO_ALPHA_TO_MASK") != NULL;
        uint32_t rop3 = (gpu_reg(g, mmCB_COLOR_CONTROL) >> 16) & 0xFF;
        static int no_rop = -1; /* PS5C_NO_LOGIC_OP=1: always copy (comparison) */
        if (no_rop < 0) no_rop = getenv("PS5C_NO_LOGIC_OP") != NULL;
        k.rop3 = r->logic_op && rop3 != 0xCC && !no_rop ? rop3 | 0x100u : 0;
        static bool rop_seen;
        if (k.rop3 && !rop_seen) {
            rop_seen = true;
            LOG_I(GPU, "color logic op: ROP3 0x%02x first with PS %016llx", rop3, (unsigned long long)ps->prog->hash);
        }
        k.alpha_to_mask = !no_a2m && (gpu_reg(g, 0xA2DCu /* DB_ALPHA_TO_MASK */) & 1u) &&
                          !(gpu_reg(g, mmDB_SHADER_CONTROL) & 0x800u /* ALPHA_TO_MASK_DISABLE */) &&
                          (ps->prog->mrt_mask & 1u);
        static bool seen;
        if (k.alpha_to_mask && !seen) {
            seen = true;
            LOG_I(GPU, "alpha to coverage: first draw with PS %016llx", (unsigned long long)ps->prog->hash);
        }
    }
    k.target_mask = gpu_reg(g, mmCB_TARGET_MASK);
    for (uint32_t i = 0; i < k.rt_count; i++) k.blend[i] = gpu_reg(g, mmCB_BLEND0_CONTROL + i);
    /* Diagnostic PC colors must not be scaled by the material blend. */
    static bool nan_trace_checked;static uint64_t nan_trace_hash;
    if(!nan_trace_checked){const char *setting=getenv("PS5C_PS_NAN_TRACE");if(setting)nan_trace_hash=strtoull(setting,NULL,16);nan_trace_checked=true;}
    if(nan_trace_hash && nan_trace_hash==ps->prog->hash)
        for(uint32_t i=0;i<k.rt_count;i++)k.blend[i]&=~(1u<<30);
    for (uint32_t i = 0; i < 32; i++) k.ps_cntl[i] = gpu_reg(g, mmSPI_PS_INPUT_CNTL_0 + i);
    k.ps_vertex_fetch=ps->info.ps_vertex_fetch=r->ps_vertex;
    ps->info.ps_input_addr = (uint16_t)gpu_reg(g, mmSPI_PS_INPUT_ADDR);
    ps->info.ps_quad_disable = sh_ps_quad_disabled(gpu_reg(g, mmPA_SC_AA_MASK_X0Y0_X1Y0),
                                                  gpu_reg(g, mmPA_SC_AA_MASK_X0Y1_X1Y1));
    if (ps->info.ps_quad_disable && r->demote) ps->info.ps_quad_disable |= SH_PS_QUAD_DEMOTE;
    k.ps_quad_disable = ps->info.ps_quad_disable;
    k.point_size = vs->info.vs_point_size;
    k.ps_col_format = ps->info.ps_col_format = gpu_reg(g, mmSPI_SHADER_COL_FORMAT);
    ps->info.ps_mrt_kind = 0;
    for (uint32_t i = 0; i < k.rt_count; i++) {
        ps->info.ps_mrt_kind |= color_output_kind(k.rt_fmt[i]) << (2 * i);
        uint32_t info = gpu_reg(g, mmCB_COLOR0_INFO + i * GPU_CB_STRIDE);
        /* COLOR_16_16_16_16 + COMP_SWAP_ALT stores B,G,R,A. Vulkan
         * has no BGRA16 attachment format: permute the fragment export and
         * write mask together. BGRA8 already uses a native attachment view. */
        if (((info >> 2) & 31u) == 12 && ((info >> 11) & 3u) == 1)
            k.ps_mrt_rb_swap |= 1u << i;
    }
    ps->info.ps_mrt_rb_swap = k.ps_mrt_rb_swap;
    ps->info.ps_flat_mask = 0;
    for (uint32_t i = 0; i < 32; i++) {
        ps->info.ps_attr_location[i] = sh_ps_input_pack(k.ps_cntl[i]);
        if ((k.ps_cntl[i] >> 10) & 1) ps->info.ps_flat_mask |= 1u << i;
    }
    { /* PS5C_LOG_PS_INPUTS=<PS hash>: which VS parameter each pixel attribute reads (debugging aid) */
        static uint64_t want;
        static int have = -1;
        static uint32_t logged;
        if (have < 0) {
            const char *e = getenv("PS5C_LOG_PS_INPUTS");
            have = e != NULL;
            if (e) want = strtoull(e, NULL, 16);
        }
        if (have && ps->prog->hash == want && logged++ < 4) {
            char line[512];
            int n = 0;
            for (uint32_t i = 0; i < 32 && n < (int)sizeof(line) - 24; i++)
                if ((ps->prog->attr_mask >> i) & 1) n += snprintf(line + n, sizeof(line) - n, " attr%u=%08x", i, k.ps_cntl[i]);
            LOG_I(GPU, "PS %016llx inputs (VS %016llx params %08x, ena %08x addr %08x):%s", (unsigned long long)ps->prog->hash,
                  (unsigned long long)vs->prog->hash, vs->prog->param_mask, gpu_reg(g, mmSPI_PS_INPUT_ENA),
                  gpu_reg(g, mmSPI_PS_INPUT_ADDR), line);
        }
    }
    { /* Bounded diagnostic: identify the draw behind an invalid stage interface. */
        static int inspect=-1;static uint32_t logged;
        if(inspect<0)inspect=getenv("PS5C_LOG_INPUT_LINK")!=NULL;
        if(inspect && logged<12) {
            uint32_t missing=0;
            for(uint32_t a=0;a<32;a++)if((ps->prog->attr_mask>>a&1) && !(ps->info.ps_attr_location[a]&32) &&
                !(vs->prog->param_mask>>(ps->info.ps_attr_location[a]&31)&1))missing|=1u<<a;
            if(missing) {
                logged++;
                LOG_W(GPU,"input link VS 0x%llx (%016llx) params %08x PS 0x%llx (%016llx) missing attrs %08x "
                    "dbsh %08x render %08x color %08x target %08x shader %08x cntl %08x %08x %08x %08x",
                    (unsigned long long)vs->prog->base,(unsigned long long)vs->prog->hash,vs->prog->param_mask,
                    (unsigned long long)ps->prog->base,(unsigned long long)ps->prog->hash,missing,
                    gpu_reg(g,mmDB_SHADER_CONTROL),gpu_reg(g,mmDB_RENDER_CONTROL),gpu_reg(g,mmCB_COLOR_CONTROL),
                    gpu_reg(g,mmCB_TARGET_MASK),gpu_reg(g,mmCB_SHADER_MASK),k.ps_cntl[0],k.ps_cntl[1],k.ps_cntl[2],k.ps_cntl[3]);
            }
        }
    }
    if (!sh_link_graphics_inputs(vs->prog, ps->prog, &vs->info, &ps->info)) {
        static uint32_t input_failures;
        if (input_failures++ < 8) LOG_W(GPU, "PS 0x%llx: no free location for mixed interpolation",
                                       (unsigned long long)ps->prog->base);
        return true;
    }
    /* Only each program's declared shape prefix contributes to the key. */
    gfx_dyn dyn_saved = {0};
    gfx_dyn_strip(&k, &dyn_saved);
    uint64_t key = gfx_key_hash(&k, vs->shapes, vs->prog->site_count, ps->shapes, ps->prog->site_count,
                                P->draw_scratch_enabled);
    gfx_dyn_restore(&k, &dyn_saved);
    if (g_dyn_state) key ^= 0x5D1A6E0C3B9F4721ull * g_dyn_state; /* never the same pipeline as another mode's */
    /* The live cache must distinguish every baked translation input, just as
     * the persistent SPIR-V cache does. In particular SPI_PS_INPUT_ADDR changes
     * which VGPR receives pixel X/Y/Z/W without changing the shader bytes or
     * SPI_PS_INPUT_CNTL. Reusing the previous pipeline reads the wrong inputs. */
    uint64_t stage_key = texture_hash_words(&vs->info, sizeof(vs->info), key);
    stage_key = texture_hash_words(&ps->info, sizeof(ps->info), stage_key);
    {
        /* Bounded opt-in evidence of state formerly missing from the live key. */
        static int inspect = -1;
        static struct { uint64_t legacy, stage; } seen[1024];
        static uint32_t count, reported;
        if (inspect < 0) inspect = getenv("PS5C_LOG_STAGE_CACHE") != NULL;
        if (inspect && reported < 12) {
            uint32_t i = 0;
            while (i < count && seen[i].legacy != key) i++;
            if (i < count && seen[i].stage != stage_key) {
                reported++;
                LOG_W(GPU, "stage cache state changed: VS %016llx PS %016llx input_addr %04x; distinct live pipeline required",
                      (unsigned long long)vs->prog->hash, (unsigned long long)ps->prog->hash, ps->info.ps_input_addr);
                seen[i].stage = stage_key;
            } else if (i == count && count < PS5C_ARRAY_COUNT(seen)) {
                seen[count].legacy = key; seen[count++].stage = stage_key;
            }
        }
    }
    key = stage_key;
    vk_descriptor_plan bindings;
    vk_descriptor_plan_init(&bindings);
    if (!vk_descriptor_plan_stage(&bindings, vs->prog, &vs->eval, vs->shapes, compact_stage(r, vs->prog)) ||
        !vk_descriptor_plan_stage(&bindings, ps->prog, &ps->eval, ps->shapes, compact_stage(r, ps->prog)) ||
        (k.mesh == 2 && !vk_descriptor_plan_append(&bindings))) return true;
    uint64_t descriptor_base_key = key;
    key = descriptor_plan_key(key, &bindings);
    t_phase = stat_phase(t_phase); /* pipeline key */

    /* Attachment layouts do not affect pipeline render-pass compatibility. */
    bool depth_read_only = zimg && !k.depth_write && !zclear && !sclear && !k.stencil_ops;
    VkImageLayout depth_layout = depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL :
                                                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkRenderPass rp = get_render_pass(r, k.rt_fmt, k.rt_count, k.depth_fmt, depth_read_only);
    if (!rp) return true;
    pipe_entry *e = find_pipe(r, key);
    if (!e || e->failed) return true;
    if (!e->pipe) {
        uint64_t t0 = stat_now();
        gfx_dyn_strip(&k, &dyn_saved);
        bool ok = create_graphics(r, e, &k, vs, ps, &bindings);
        gfx_dyn_restore(&k, &dyn_saved);
        stat_add(ST_PIPE, t0);
        if (!ok) {
            e->failed = !e->pending;
            return true;
        }
    }

    if (r->rt_scale) { /* PS5C_RT_SCALE: full-size color targets drawn with scaled ones become scaled */
        bool scaled = zimg && zimg->ext_w, plain = false;
        for (uint32_t i = 0; i < k.rt_count; i++)
            if (rts[i]) {
                scaled |= rts[i]->ext_w != 0;
                plain |= !rts[i]->ext_w && !color_views[i];
            }
        if (scaled && plain) {
            end_pass(r);
            for (uint32_t i = 0; i < k.rt_count; i++)
                if (rts[i] && !rts[i]->ext_w && !color_views[i]) vk_image_rescale(r, rts[i]);
        }
    }
    VkImageView views[GPU_MAX_RENDER_TARGETS + 1];
    uint32_t nviews = 0;
    for (uint32_t i = 0; i < k.rt_count; i++) {
        if (rts[i]) views[nviews++] = color_views[i] ? color_views[i] : rts[i]->view;
    }
    if (zimg) views[nviews++] = zview;
    /* PS5C_RT_SCALE: the targets may be drawn smaller than the guest's size (fb_w x fb_h,
     * which the vertex shader's viewport transform keeps using); hfb_w x hfb_h is the host's */
    uint32_t hfb_w = fb_w, hfb_h = fb_h;
    {
        bool scaled = false, plain = false;
        for (uint32_t i = 0; i < k.rt_count; i++)
            if (rts[i]) *(rts[i]->ext_w ? &scaled : &plain) = true;
        if (zimg) *(zimg->ext_w ? &scaled : &plain) = true;
        if (scaled) {
            for (uint32_t i = 0; i < k.rt_count; i++)
                if (rts[i] && rts[i]->ext_w) {
                    if (rts[i]->ext_w < hfb_w) hfb_w = rts[i]->ext_w;
                    if (rts[i]->ext_h < hfb_h) hfb_h = rts[i]->ext_h;
                }
            if (zimg && zimg->ext_w) {
                if (zimg->ext_w < hfb_w) hfb_w = zimg->ext_w;
                if (zimg->ext_h < hfb_h) hfb_h = zimg->ext_h;
            }
            if (plain) {
                static uint32_t logged;
                if (logged++ < 8) {
                    LOG_W(GPU, "draw mixes scaled and full-size targets (PS5C_RT_SCALE): %ux%u drawn at %ux%u",
                          fb_w, fb_h, hfb_w, hfb_h);
                    for (uint32_t i = 0; i <= k.rt_count; i++) {
                        vk_image *t = i < k.rt_count ? rts[i] : zimg;
                        if (t) LOG_I(GPU, "  %s 0x%llx %ux%u fmt %d host %ux%u written %d drawn %d", i < k.rt_count ? "color" : "depth",
                                     (unsigned long long)t->addr, t->width, t->height, (int)t->format, vk_img_w(t), vk_img_h(t),
                                     (int)t->gpu_written, (int)t->drawn_to);
                    }
                }
            }
        }
    }
    VkFramebuffer fb = get_framebuffer(r, rp, views, nviews, hfb_w, hfb_h, fb_layers);
    if (!fb) return P->fb_count != MAX_FBS || draw_fail(__LINE__);

    /* resources */
    bind_list bl;
    bl.count = 0;
    bl.sampled_count = 0;
    bl.written_count = 0;
    bl.missing = false;
    VkDeviceSize index_off = 0;
    /* GE adds this signed offset to both DMA indices and generated indices.
     * Passing zero repeats the first glyphs/vertices of later UI draws. */
    static int no_vertex_offset = -1; /* comparison only; read once */
    if (no_vertex_offset < 0) no_vertex_offset = getenv("PS5C_NO_VERTEX_OFFSET") != NULL;
    int32_t base_vertex = no_vertex_offset ? 0 : (int32_t)gpu_reg(g, mmGE_INDX_OFFSET);
    /* Inspect exactly the index bytes bound to this draw, before staging
     * vertex buffers. Unknown shader addressing retains the full copy. */
    static int vertex_ranges = -1; /* on by default; PS5C_VERTEX_RANGES=0 copies whole descriptors */
    if (vertex_ranges < 0) vertex_ranges = !getenv("PS5C_VERTEX_RANGES") || atoi(getenv("PS5C_VERTEX_RANGES")) != 0;
    vs->trim_vertex_buffers = vertex_ranges && (cache_vertex_flow(P, g->stats.flips)
        ? vr_flow_cached(&P->vertex_flow, vs->prog) : vr_straight_line(vs->prog));
    vs->vertex_max = d->count ? d->count - 1 : 0;
    vs->instance_max = d->instances ? d->instances - 1 : 0;
    if (d->indexed && d->index_size != 2 && d->index_size != 4) vs->trim_vertex_buffers = false;
    /* the index buffer comes from the vertex cache when it can: no copy and no scan of an unchanged one */
    VkBuffer index_buf = P->work->arena.buffer;
    const uint8_t *index_ptr = NULL;
    vc_entry *index_entry = NULL;
    if (d->indexed && vs->info.stage != SH_STAGE_MESH && P->vc_on >= 0 &&
        vertex_cache_bind(r, d->index_addr, (uint64_t)d->count * d->index_size, (uint64_t)d->count * d->index_size,
                          &index_buf, &index_off, &index_entry))
        index_ptr = (const uint8_t *)P->vc_slabs[index_entry->slab].mapped + index_off;
    else index_buf = P->work->arena.buffer;
    if (d->indexed && (vs->trim_vertex_buffers || vs->info.stage==SH_STAGE_MESH)) {
        uint64_t size = (uint64_t)d->count * d->index_size;
        if (!index_ptr) {
            g_copy_tag = 4;
            bool copied_ = arena_copy_guest_shared(r, d->index_addr, size, &index_off);
            g_copy_tag = 0;
            if (!copied_) return draw_fail(__LINE__);
            if(vs->trim_vertex_buffers)vs->vertex_max = largest_index(P, d, arena_ptr(r, index_off));
        } else if (vs->trim_vertex_buffers) {
            if (!index_entry->has_max) {
                const void *src = gmem_host(r->mem, d->index_addr, size); /* not the write-combined copy */
                index_entry->index_max = largest_index(P, d, src ? src : index_ptr);
                index_entry->has_max = true;
            }
            vs->vertex_max = index_entry->index_max;
        }
    }
    vs->mesh_index_off=index_off;vs->mesh_index_count=d->count;vs->mesh_index_size=d->index_size;
    vs->vertex_max = vr_vertex_max(vs->vertex_max, base_vertex);
    { /* the guest viewport transform, for the vertex shader (SH_U_VPORT) */
        uint32_t vte = gpu_reg(g, mmPA_CL_VTE_CNTL);
        float vpt[8] = {(vte & 1) ? f32_of(gpu_reg(g, mmPA_CL_VPORT_XSCALE)) : 1.0f,
                        (vte & 2) ? f32_of(gpu_reg(g, mmPA_CL_VPORT_XOFFSET)) : 0.0f,
                        (vte & 4) ? f32_of(gpu_reg(g, mmPA_CL_VPORT_YSCALE)) : 1.0f,
                        (vte & 8) ? f32_of(gpu_reg(g, mmPA_CL_VPORT_YOFFSET)) : 0.0f,
                        2.0f / (float)fb_w, 2.0f / (float)fb_h,
                        (vte & 16) ? f32_of(gpu_reg(g, mmPA_CL_VPORT_ZSCALE)) : 1.0f,
                        (vte & 32) ? f32_of(gpu_reg(g, mmPA_CL_VPORT_ZOFFSET)) : 0.0f};
        vpt[7] += far_depth_offset;
        memcpy(vs->eval.smem_data + SH_U_VPORT, vpt, sizeof(vpt));
        for (uint32_t i = 0; i < 24; i++) vs->eval.smem_data[SH_U_UCP + i] = gpu_reg(g, 0xA16Fu /* PA_CL_UCP_0_X */ + i);
        /* PS5C_RT_SCALE: the pixel shader's position back at the guest's scale (guest / host - 1) */
        if (r->rt_scale) {
            float frag[2] = {hfb_w != fb_w ? (float)fb_w / (float)hfb_w - 1.0f : 0.0f,
                             hfb_h != fb_h ? (float)fb_h / (float)hfb_h - 1.0f : 0.0f};
            memcpy(ps->eval.smem_data + SH_U_VPORT, frag, sizeof(frag));
        }
    }
    uint64_t t0 = stat_now();
    if(vs->info.stage==SH_STAGE_MESH) {
        vs->eval.smem_data[SH_U_MESH]=(vs->info.mesh_indexed&2u)?(d->count-2)*3:d->count;
        vs->eval.smem_data[SH_U_MESH+1]=gpu_reg(g,mmGE_INDX_OFFSET);
        vs->eval.smem_data[SH_U_MESH+2]=0;
    }
    bool res_ok = stage_resources(r, vs, &bl, false, NULL, NULL) && stage_resources(r, ps, &bl, false, NULL, NULL);
    stat_add(ST_BUFFERS, t0);
    /* Image promotion can complete a batch while binding resources.
     * Rebuild all bindings before using an earlier view or framebuffer. */
    if (r->use_seq != target_batch) return draw_fail(__LINE__);
    if (!res_ok) {
        if (bl.missing) P->skipped++;
        return bl.missing || draw_fail(__LINE__);
    }
    /* Diagnostics only: storage writes cannot overlap native attachments.
     * Record the actual resources before selecting a faithful alias strategy. */
    if(log_alias) {
        static unsigned logged;
        for(uint32_t rt=0;rt<k.rt_count && logged<48;rt++) if(rts[rt])
            for(uint32_t b=0;b<bl.count && logged<48;b++)
                if(bl.w[b].descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && bl.bound_image[b]==rts[rt]) {
                    int site=-1;uint32_t binding=sh_binding_count(vs->prog)+1;
                    for(uint32_t s=0;s<ps->prog->site_count;s++) {
                        if(ps->prog->sites[s].kind==SH_SITE_SMEM) continue;
                        if(binding++==b)site=(int)s;
                    }
                    uint32_t zero[8]={0};const uint32_t *desc=site>=0?ps->eval.site[site].desc:zero;
                    LOG_W(GPU,"storage/RT alias PS 0x%llx RT%u binding %u site %d write %d coherent_load %u addr 0x%llx "
                        "%ux%u layers %u fmt %d target_mask %08x shader_mask %08x CBview %08x descriptor "
                        "%08x %08x %08x %08x %08x %08x %08x %08x",
                        (unsigned long long)ps->prog->base,rt,b,site,site>=0?ps->prog->sites[site].store:0,
                        site>=0?ps->shapes[site].ordered_load:0,(unsigned long long)rts[rt]->addr,
                        rts[rt]->width,rts[rt]->height,rts[rt]->layers,(int)rts[rt]->format,
                        gpu_reg(g,mmCB_TARGET_MASK),gpu_reg(g,mmCB_SHADER_MASK),gpu_reg(g,mmCB_COLOR0_VIEW+rt*GPU_CB_STRIDE),
                        desc[0],desc[1],desc[2],desc[3],desc[4],desc[5],desc[6],desc[7]);
                    logged++;
                }
    }
    if (zimg && !depth_read_only && !depth_feedback(r, &bl, zimg)) return draw_fail(__LINE__);
    for(uint32_t i=0;i<k.rt_count;i++)
        if(rts[i] && !depth_feedback(r,&bl,rts[i])) return draw_fail(__LINE__);
    if (d->indexed && !vs->trim_vertex_buffers && vs->info.stage!=SH_STAGE_MESH && !index_ptr) {
        uint64_t size = (uint64_t)d->count * d->index_size;
        g_copy_tag = 4;
        bool copied_ = arena_copy_guest_shared(r, d->index_addr, size, &index_off);
        g_copy_tag = 0;
        if (!copied_) return draw_fail(__LINE__);
    }
    if (d->indexed && !index_ptr) index_ptr = (const uint8_t *)arena_ptr(r, index_off);
    /* Inspect the index bytes actually bound, not merely guest RAM before a
     * pending compute write. Debug only; no change to draw or synchronization. */
    static int log_indices = -1;
    static uint64_t index_ps;
    static uint32_t index_logged;
    if (log_indices < 0) {
        const char *setting = getenv("PS5C_LOG_INDEX_PS");
        log_indices = setting != NULL;
        index_ps = setting ? strtoull(setting, NULL, 16) : 0;
    }
    if (log_indices && index_logged < 24 && (ps->prog->hash == index_ps || ps->prog->base == index_ps)) {
        if (!d->indexed || (d->index_size != 1 && d->index_size != 2 && d->index_size != 4)) {
            LOG_I(GPU, "draw frame %llu PS %016llx: %u vertices, not indexed (index size %u), prim %u, %u instances",
                  (unsigned long long)g->stats.flips, (unsigned long long)ps->prog->hash, d->count, d->index_size,
                  gpu_reg(g, mmVGT_PRIMITIVE_TYPE), d->instances);
        } else {
            char text[256];
            uint32_t len = 0, pending = 0;
            uint64_t size = (uint64_t)d->count * d->index_size;
            for (uint32_t j = 0; j < d->count && j < 32 && len < sizeof(text) - 12; j++) {
                uint32_t ix = 0;
                memcpy(&ix, index_ptr + j * d->index_size, d->index_size);
                len += (uint32_t)snprintf(text + len, sizeof(text) - len, " %x", ix);
            }
            text[len] = 0;
            for (uint32_t j = 0; j < P->wb_count; j++)
                pending += d->index_addr < P->wb[j].addr + P->wb[j].size && P->wb[j].addr < d->index_addr + size;
            LOG_I(GPU, "bound indices frame %llu PS %016llx target %ux%u at 0x%llx x%u, %u, prim %u, reset %08x:%s; %u pending writes",
                  (unsigned long long)g->stats.flips, (unsigned long long)ps->prog->hash, fb_w, fb_h,
                  (unsigned long long)d->index_addr, d->index_size, d->count, gpu_reg(g, mmVGT_PRIMITIVE_TYPE),
                  gpu_reg(g, mmVGT_MULTI_PRIM_IB_RESET_INDX), text, pending);
        }
        index_logged++;
    }
    if (d->indexed && d->index_size == 1 && vs->info.stage != SH_STAGE_MESH) {
        /* Widen the guest indices on the Vulkan 1.3 path, which cannot
         * assume VK_KHR_index_type_uint8 is enabled. */
        if (!arena_alloc(r, (uint64_t)d->count * sizeof(uint16_t), &index_off))
            return draw_fail(__LINE__);
        uint16_t *indices = (uint16_t *)arena_ptr(r, index_off);
        uint32_t reset = gpu_reg(g, mmVGT_MULTI_PRIM_IB_RESET_INDX);
        uint32_t reset_mode = gpu_reg(g, mmVGT_MULTI_PRIM_IB_RESET_EN);
        bool strip_or_fan = k.prim == GPU_PRIM_LINESTRIP || k.prim == GPU_PRIM_TRIFAN || k.prim == GPU_PRIM_TRISTRIP;
        uint32_t restart = strip_or_fan && (reset_mode & 1u)
            ? reset & ((reset_mode & 2u) ? UINT32_MAX : UINT8_MAX) : UINT32_MAX;
        for (uint32_t i = 0; i < d->count; i++)
            indices[i] = index_ptr[i] == restart ? UINT16_MAX : index_ptr[i];
        index_buf = P->work->arena.buffer;
    }
    VkDeviceSize geometry_off=0;uint32_t geometry_groups=0,geometry_total=0,geometry_stride=0;
    if(k.mesh==2) {
        geometry_groups=d->count/vs->info.mesh_input_vertices+(d->count%vs->info.mesh_input_vertices!=0);
        uint64_t total=(uint64_t)geometry_groups*(d->instances?d->instances:1);
        geometry_stride=sh_mesh_output_words(vs->prog,&vs->info);
        uint64_t bytes=total*geometry_stride*4;
        if(total>UINT32_MAX || geometry_groups>r->props.limits.maxComputeWorkGroupCount[0] ||
           (d->instances?d->instances:1)>r->props.limits.maxComputeWorkGroupCount[1] ||
           bytes>r->props.limits.maxStorageBufferRange || bl.count>=PS5C_ARRAY_COUNT(bl.w) || !arena_alloc(r,bytes,&geometry_off))return draw_fail(__LINE__);
        geometry_total=(uint32_t)total;add_buffer(r,&bl,geometry_off,bytes);
        vs->mesh_output_off=geometry_off;vs->mesh_output_groups=geometry_total;
    }
    if(P->occlusion_pool && P->occlusion_count==1024)return draw_fail(__LINE__);
    t_phase = stat_phase(t_phase); /* pipeline + resources */
    shared_image_layouts(&bl);
    if (depth_read_only) {
        for (uint32_t i = 0; i < bl.count; i++)
            if (bl.bound_image[i] == zimg &&
                bl.w[i].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
                bl.img[i].imageLayout = depth_layout;
    }
    if (!descriptor_plan_confirm(&bindings, &bl)) {
        if (bindings.count != bl.count) return true;
        e = find_pipe(r, descriptor_base_key);
        if (!e || e->failed) return true;
        if (!e->pipe) {
            gfx_dyn_strip(&k, &dyn_saved);
            bool ok = create_graphics(r, e, &k, vs, ps, &bindings);
            gfx_dyn_restore(&k, &dyn_saved);
            if (!ok) {
                e->failed = !e->pending;
                return true;
            }
        }
    }
    VkDescriptorSet set = e->push ? VK_NULL_HANDLE : alloc_set(r, e->dsl, e->update_after_bind);
    if (!set && !e->push) return draw_fail(__LINE__);
    if (set) update_descriptor_set(r, set, &bl, &bindings);
    t_phase = stat_phase(t_phase); /* descriptor set */

    if (tex_save_wanted(g, ps->prog))
        for (uint32_t i = 0; i < bl.sampled_count && g_tex_save_count < PS5C_ARRAY_COUNT(g_tex_save); i++) {
            /* Blur/effect shaders can bind one image at many sites. Save it
             * once so a single diagnostic does not exhaust the disk budget. */
            bool seen=false;
            for(uint32_t j=0;j<g_tex_save_count;j++) seen|=g_tex_save[j]==bl.sampled[i];
            if(!seen) g_tex_save[g_tex_save_count++] = bl.sampled[i];
        }
    /* layouts: sampled images, then targets */
    VkCommandBuffer cmd = r->cmd;
    /* GENERAL-to-GENERAL storage hazards still emit barriers. They cannot be
     * recorded inside our render pass (which has no self-dependency). */
    bool need_barrier = bl.written_count != 0 || k.mesh==2;
    for (uint32_t i = 0; i < bl.sampled_count; i++)
        need_barrier |= bl.sampled[i]->layout != (depth_read_only && bl.sampled[i] == zimg ? depth_layout :
                                                    image_written(&bl, bl.sampled[i]) ? VK_IMAGE_LAYOUT_GENERAL :
                                                                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for (uint32_t i = 0; i < bl.written_count; i++) need_barrier |= bl.written[i]->layout != VK_IMAGE_LAYOUT_GENERAL;
    for (uint32_t i = 0; i < k.rt_count; i++) need_barrier |= rts[i] && rts[i]->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    need_barrier |= zimg && zimg->layout != depth_layout;
    if (need_barrier) end_pass(r);
    VkPipelineStageFlags draw_shader_stages=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        (k.mesh==1 ? VK_PIPELINE_STAGE_MESH_SHADER_BIT_NV : k.mesh==2 ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_VERTEX_SHADER_BIT : VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
    for (uint32_t i = 0; i < bl.sampled_count; i++) {
        if (image_written(&bl, bl.sampled[i]) || (depth_read_only && bl.sampled[i] == zimg)) continue;
        vk_barrier(cmd, bl.sampled[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                   draw_shader_stages);
    }
    for (uint32_t i = 0; i < bl.written_count; i++) {
        vk_barrier(cmd, bl.written[i], VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   draw_shader_stages);
        bl.written[i]->gpu_written = true;
        texture_gpu_write(r,bl.written[i]);
        bl.written[i]->written_seq = ++r->write_seq;
    }
    uint64_t rt_prev[GPU_MAX_RENDER_TARGETS] = {0}; /* for vk_image_note_draw below */
    for (uint32_t i = 0; i < k.rt_count; i++) {
        if (!rts[i]) continue;
        vk_barrier(cmd, rts[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        rts[i]->gpu_written = true;
        rts[i]->drawn_to = true;
        texture_gpu_write(r,rts[i]);
        rt_prev[i] = rts[i]->written_seq;
        rts[i]->written_seq = ++r->write_seq;
    }
    if (census_on()) {
        bool large = census_large(zimg);
        for (uint32_t i = 0; i < k.rt_count; i++) large |= census_large(rts[i]);
        if (large) {
            g_census_draws++;
            if (gpu_reg(g, mmSPI_PS_INPUT_ENA) & 0xF00u) g_census_pos++; /* POS_X/Y/Z/W_FLOAT */
        }
    }
    if (zimg) {
        /* A read-only depth attachment can also be sampled by shaders. Its
         * STORE storeOp is still a late-fragment-test write at pass end, even
         * when depthWriteEnable is false. Retain that access for the barrier
         * before a subsequent layout transition/overwrite. */
        vk_barrier(cmd, zimg, depth_layout,
                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                       (depth_read_only ? VK_ACCESS_SHADER_READ_BIT : 0),
                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                       (depth_read_only ? draw_shader_stages : 0));
        zimg->drawn_to = true;
        if (k.depth_write || zclear) {
            zimg->gpu_written = true;
            zimg->written_seq = ++r->write_seq;
        }
    }
    if(k.mesh==2) {
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,e->prepass);
        bind_set(r,cmd,VK_PIPELINE_BIND_POINT_COMPUTE,e,set,&bl,&bindings);
        /* Include geometry expansion, not just its later raster replay.
         * It is reported as CS at the guest GS address (no pixel shader). */
        int geometry_prof = prof_start(r);
        vkCmdDispatch(cmd,geometry_groups,d->instances?d->instances:1,1);
        prof_stop(r,geometry_prof,vs->prog->base,0,geometry_total);
        VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_INDIRECT_COMMAND_READ_BIT};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            0,1,&barrier,0,NULL,0,NULL);
    }
    if (P->cur_fb != fb) {
        end_pass(r);
        if (r->dyn_render) {
            VkRenderingAttachmentInfo color[GPU_MAX_RENDER_TARGETS], depth;
            for (uint32_t i = 0; i < k.rt_count; i++) {
                color[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                    .imageView = rts[i] ? (color_views[i] ? color_views[i] : rts[i]->view) : VK_NULL_HANDLE,
                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            }
            VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea.extent.width = hfb_w;
            ri.renderArea.extent.height = hfb_h;
            ri.layerCount = fb_layers;
            ri.colorAttachmentCount = k.rt_count;
            ri.pColorAttachments = color;
            if (zimg) {
                depth = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = zview,
                    .imageLayout = depth_layout, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
                if (k.depth_fmt != VK_FORMAT_S8_UINT) ri.pDepthAttachment = &depth;
                if (vk_format_has_stencil(k.depth_fmt)) ri.pStencilAttachment = &depth;
            }
            r->begin_rendering(cmd, &ri);
        } else {
        VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = rp;
        bi.framebuffer = fb;
        bi.renderArea.extent.width = hfb_w;
        bi.renderArea.extent.height = hfb_h;
        vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        }
        P->cur_fb = fb;
        P->cur_rp = rp;
        P->cur_gfx_pipe = VK_NULL_HANDLE;
        P->cur_vp_valid = P->cur_sc_valid = P->cur_bias_valid = P->cur_stencil_valid = P->cur_dyn_valid = false;
    }

    /* viewport: the whole target (the vertex shader applies the guest's) */
    VkViewport vp = {0, 0, (float)hfb_w, (float)hfb_h, 0, 1};
    /* scissor: screen, window, generic and (when enabled) viewport 0 rectangles */
    int32_t x0 = 0, y0 = 0, x1 = (int32_t)fb_w, y1 = (int32_t)fb_h;
    const uint32_t rects[4] = {mmPA_SC_SCREEN_SCISSOR_TL, mmPA_SC_WINDOW_SCISSOR_TL, mmPA_SC_GENERIC_SCISSOR_TL,
                               mmPA_SC_VPORT_SCISSOR_0_TL};
    for (int q = 0; q < 4; q++) {
        if (q == 3 && !((gpu_reg(g, mmPA_SC_MODE_CNTL_0) >> 1) & 1)) continue;
        uint32_t tl = gpu_reg(g, rects[q]), br = gpu_reg(g, rects[q] + 1);
        if (!br) continue; /* never set */
        int32_t l = (int32_t)(tl & 0x7FFF), t = (int32_t)((tl >> 16) & 0x7FFF);
        int32_t rr = (int32_t)(br & 0x7FFF), b = (int32_t)((br >> 16) & 0x7FFF);
        if (l > x0) x0 = l;
        if (t > y0) y0 = t;
        if (rr < x1) x1 = rr;
        if (b < y1) y1 = b;
    }
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    if (hfb_w != fb_w || hfb_h != fb_h) { /* PS5C_RT_SCALE: the rectangles at the host's size */
        x0 = (int32_t)((uint64_t)x0 * hfb_w / fb_w), x1 = (int32_t)(((uint64_t)x1 * hfb_w + fb_w - 1) / fb_w);
        y0 = (int32_t)((uint64_t)y0 * hfb_h / fb_h), y1 = (int32_t)(((uint64_t)y1 * hfb_h + fb_h - 1) / fb_h);
        if (x1 > (int32_t)hfb_w) x1 = (int32_t)hfb_w;
        if (y1 > (int32_t)hfb_h) y1 = (int32_t)hfb_h;
    }
    VkRect2D sc = {{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    for (uint32_t i = 0; i < k.rt_count; i++)
        if (rts[i]) vk_image_note_draw(rts[i], rt_prev[i], x0, y0, x1, y1); /* the draw writes within its scissor */
    if (log_scissor_from >= 0 && g->stats.flips >= (uint64_t)log_scissor_from && /* =<from flip> */
        (x0 || y0 || x1 != (int32_t)fb_w || y1 != (int32_t)fb_h)) {
        static uint32_t logged;
        if (logged++ < 200)
            LOG_I(GPU, "scissor %d,%d-%d,%d of %ux%u: screen %08x/%08x window %08x/%08x generic %08x/%08x vport %08x/%08x "
                       "offset %08x mode %08x",
                  x0, y0, x1, y1, fb_w, fb_h, gpu_reg(g, rects[0]), gpu_reg(g, rects[0] + 1), gpu_reg(g, rects[1]),
                  gpu_reg(g, rects[1] + 1), gpu_reg(g, rects[2]), gpu_reg(g, rects[2] + 1), gpu_reg(g, rects[3]),
                  gpu_reg(g, rects[3] + 1), gpu_reg(g, 0xA080u), gpu_reg(g, mmPA_SC_MODE_CNTL_0));
    }
    if (!P->cur_vp_valid || memcmp(&P->cur_vp, &vp, sizeof(vp)) != 0) {
        vkCmdSetViewport(cmd, 0, 1, &vp);
        P->cur_vp = vp;
        P->cur_vp_valid = true;
    }
    if (!P->cur_sc_valid || memcmp(&P->cur_sc, &sc, sizeof(sc)) != 0) {
        vkCmdSetScissor(cmd, 0, 1, &sc);
        P->cur_sc = sc;
        P->cur_sc_valid = true;
    }
    if (!P->cur_bias_valid || memcmp(&P->cur_bias_constant, &bias.constant, sizeof(float)) != 0 ||
        memcmp(&P->cur_bias_clamp, &bias.clamp, sizeof(float)) != 0 ||
        memcmp(&P->cur_bias_slope, &bias.slope, sizeof(float)) != 0) {
        vkCmdSetDepthBias(cmd, bias.constant, bias.clamp, bias.slope);
        P->cur_bias_constant = bias.constant;
        P->cur_bias_clamp = bias.clamp;
        P->cur_bias_slope = bias.slope;
        P->cur_bias_valid = true;
    }
    if (P->cur_gfx_pipe != e->pipe) P->cur_stencil_valid = false;
    if (k.stencil_func) {
        bool bf = (k.stencil_func >> 7) & 1;
        const vk_stencil_face *b = bf ? &sface[1] : &sface[0];
        if (!P->cur_stencil_valid || memcmp(&P->cur_stencil[0], &sface[0], sizeof(vk_stencil_face)) != 0 ||
            memcmp(&P->cur_stencil[1], b, sizeof(vk_stencil_face)) != 0) {
            vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, sface[0].ref);
            vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, b->ref);
            vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, sface[0].cmp);
            vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, b->cmp);
            vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, sface[0].wr);
            vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, b->wr);
            P->cur_stencil[0] = sface[0];
            P->cur_stencil[1] = *b;
            P->cur_stencil_valid = true;
        }
    } else if (g_dyn_state & DYN_STENCIL) { /* the pipeline still has them dynamic: give them defined values */
        static const vk_stencil_face zero;
        if (!P->cur_stencil_valid || memcmp(&P->cur_stencil[0], &zero, sizeof(zero)) != 0 ||
            memcmp(&P->cur_stencil[1], &zero, sizeof(zero)) != 0) {
            vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
            vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
            vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
            P->cur_stencil[0] = P->cur_stencil[1] = zero;
            P->cur_stencil_valid = true;
        }
    } else {
        P->cur_stencil_valid = false;
    }
    { /* PS5C_LOG_STENCIL=<flip>: the depth/stencil state of that frame's draws (debugging aid) */
        static frame_trig trig;
        int64_t frame = frame_trigger(&trig, "PS5C_LOG_STENCIL", g->stats.flips);
        static uint32_t logged;
        if (frame >= 0 && (int64_t)g->stats.flips == frame && logged++ < 600)
            LOG_I(GPU, "ds draw: PS %016llx z 0x%llx fmt %d ctl %08x sctl %08x ref %08x/%08x render %08x clear %08x/%u "
                       "rt0 %d mask %08x %ux%u",
                  (unsigned long long)ps->prog->hash, (unsigned long long)(zimg ? zimg->addr : 0), zimg ? (int)zimg->format : 0,
                  gpu_reg(g, mmDB_DEPTH_CONTROL), gpu_reg(g, mmDB_STENCIL_CONTROL), gpu_reg(g, mmDB_STENCILREFMASK),
                  gpu_reg(g, mmDB_STENCILREFMASK_BF), gpu_reg(g, mmDB_RENDER_CONTROL), gpu_reg(g, mmDB_DEPTH_CLEAR),
                  gpu_reg(g, mmDB_STENCIL_CLEAR), k.rt_count ? (int)k.rt_fmt[0] : 0, k.target_mask, fb_w, fb_h);
    }
    { /* PS5C_LOG_DRAW_STATE=<flip>: rasterizer state of that frame's draws (debugging aid) */
        static frame_trig trig;
        int64_t frame = frame_trigger(&trig, "PS5C_LOG_DRAW_STATE", g->stats.flips);
        static uint32_t logged;
        if (frame >= 0 && (int64_t)g->stats.flips == frame && logged++ < 8192)
            LOG_I(GPU, "draw state: VS %016llx PS %016llx count %u inst %u prim %u idx %08x clip %08x su %08x vte %08x "
                       "aa %08x/%08x quad %x shmask %08x tmask %08x colfmt %08x dbsh %08x sc0 %08x vp %d,%d %ux%u sc %d,%d %ux%u "
                       "guest vport %g %g %g %g z %g %g base vertex %d index 0x%llx",
                  (unsigned long long)vs->prog->hash, (unsigned long long)ps->prog->hash, d->count, d->instances,
                  gpu_reg(g, mmVGT_PRIMITIVE_TYPE), gpu_reg(g, mmVGT_INDEX_TYPE), gpu_reg(g, mmPA_CL_CLIP_CNTL),
                  gpu_reg(g, mmPA_SU_SC_MODE_CNTL), gpu_reg(g, mmPA_CL_VTE_CNTL), gpu_reg(g, mmPA_SC_AA_MASK_X0Y0_X1Y0),
                  gpu_reg(g, mmPA_SC_AA_MASK_X0Y0_X1Y0 + 1), ps->info.ps_quad_disable, gpu_reg(g, mmCB_SHADER_MASK),gpu_reg(g, mmCB_TARGET_MASK),
                  gpu_reg(g, mmSPI_SHADER_COL_FORMAT), gpu_reg(g, mmDB_SHADER_CONTROL), gpu_reg(g, mmPA_SC_MODE_CNTL_0),
                  (int)vp.x, (int)vp.y, (unsigned)vp.width, (unsigned)vp.height, sc.offset.x, sc.offset.y, sc.extent.width,
                  sc.extent.height, (double)f32_of(gpu_reg(g, mmPA_CL_VPORT_XSCALE)), (double)f32_of(gpu_reg(g, mmPA_CL_VPORT_XOFFSET)),
                  (double)f32_of(gpu_reg(g, mmPA_CL_VPORT_YSCALE)), (double)f32_of(gpu_reg(g, mmPA_CL_VPORT_YOFFSET)),
                  (double)f32_of(gpu_reg(g, mmPA_CL_VPORT_ZSCALE)), (double)f32_of(gpu_reg(g, mmPA_CL_VPORT_ZOFFSET)),
                  (int32_t)gpu_reg(g, mmGE_INDX_OFFSET), (unsigned long long)d->index_addr);
    }
    if ((zclear || sclear) && sc.extent.width && sc.extent.height) {
        /* DB_RENDER_CONTROL.DEPTH_CLEAR_ENABLE / STENCIL_CLEAR_ENABLE: the draw's pixels take
         * DB_DEPTH_CLEAR / DB_STENCIL_CLEAR */
        VkClearAttachment ca = {(zclear ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (sclear ? VK_IMAGE_ASPECT_STENCIL_BIT : 0), 0,
                                {.depthStencil = {f32_of(gpu_reg(g, mmDB_DEPTH_CLEAR)), gpu_reg(g, mmDB_STENCIL_CLEAR) & 0xFF}}};
        VkClearRect cr = {sc, 0, 1};
        vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
        if (!bound) return true;
    }
    t_phase = stat_phase(t_phase); /* render pass + dynamic state */
    if (P->cur_gfx_pipe != e->pipe) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, e->pipe);
        P->cur_gfx_pipe = e->pipe;
    }
    if (g_dyn_state) set_dyn_state(r, P, cmd, &k);
    {
        /* Constants belong to the draw, not the pipeline. Always set them:
         * pipeline reuse and new command buffers must not inherit old values. */
        uint32_t bits[4]={gpu_reg(g,mmCB_BLEND_RED),gpu_reg(g,mmCB_BLEND_GREEN),
                          gpu_reg(g,mmCB_BLEND_BLUE),gpu_reg(g,mmCB_BLEND_ALPHA)};
        int rb_swap=-1;
        for(uint32_t i=0;i<k.rt_count;i++) {
            uint32_t b=k.blend[i],sf=b&31u,df=(b>>8)&31u,op=(b>>5)&7u;
            if(!(b&(1u<<30)) || op==2 || op==3 || !((k.target_mask>>(4*i))&5u) ||
               !((sf==13 || sf==14) || (df==13 || df==14)))continue;
            int swap=(k.ps_mrt_rb_swap>>i)&1u;
            if(rb_swap<0)rb_swap=swap;
            else if(rb_swap!=swap && bits[0]!=bits[2]) {
                /* Vulkan has one global blend color; unlike COLOR_16 ALT,
                 * native BGRA attachments need no permutation. Mixed ALT MRTs
                 * with distinct R/B constants require an additional fallback. */
                static bool warned;
                if(!warned){warned=true;LOG_W(GPU,"constant-color blend with mixed HDR channel permutations is not fully supported");}
            }
        }
        if(rb_swap==1){uint32_t red=bits[0];bits[0]=bits[2];bits[2]=red;}
        float constants[4];memcpy(constants,bits,sizeof(constants));
        vkCmdSetBlendConstants(cmd,constants);
    }
    bind_set(r, cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, e, set, &bl, &bindings);
    uint32_t instances = d->instances ? d->instances : 1;
    int occlusion_i=-1;
    /* DB_COUNT_CONTROL at byte register 0x28004: bit0 disables increments. */
    if(P->occlusion_pool && P->occlusion_scope && !(gpu_reg(g,0xa001)&1)) {
        occlusion_i=(int)P->occlusion_count++;
        P->occlusion_hash[occlusion_i]=vs->prog->hash;
        P->occlusion_flip[occlusion_i]=g->stats.flips;
        P->occlusion_ps[occlusion_i]=ps->prog->hash;
        P->occlusion_depth[occlusion_i]=gpu_reg(g,mmDB_DEPTH_CONTROL);
        vkCmdBeginQuery(cmd,P->occlusion_pool,(uint32_t)occlusion_i,VK_QUERY_CONTROL_PRECISE_BIT);
    }
    int prof_i = prof_start(r);
    VkQueryPool stat_query=VK_NULL_HANDLE;
    if(prof_i>=0) {
        P->prof->mesh_query[prof_i]=k.mesh==1;
        stat_query=k.mesh==1?P->prof->mesh_stats:P->prof->stats;
        if(stat_query)vkCmdBeginQuery(cmd,stat_query,(uint32_t)prof_i,0);
    }
    VkDeviceSize quad_off;
    if(k.mesh==2) {
        /* The replay shader derives its group from VertexIndex/firstVertex,
         * never DrawIndex. Preserve the per-group commands and their order;
         * only reduce host calls. Each record starts with the same 16-byte
         * indirect command, followed by that group's geometry payload. */
        uint32_t limit=ngg_multi_draw_limit(r,g->stats.flips);
        for(uint32_t group=0;group<geometry_total;) {
            uint32_t count=geometry_total-group;if(count>limit)count=limit;
            vkCmdDrawIndirect(cmd,P->work->arena.buffer,geometry_off+(VkDeviceSize)group*geometry_stride*4,
                count,count>1?geometry_stride*4:sizeof(VkDrawIndirectCommand));
            group+=count;
        }
    } else if(k.mesh==1) {
        uint32_t expanded=(vs->info.mesh_indexed&2u)?(d->count-2)*3:d->count;
        uint32_t groups=expanded/vs->info.mesh_input_vertices+(expanded%vs->info.mesh_input_vertices!=0);
        if(groups && groups<=r->mesh_limits.maxMeshWorkGroupCount[0] && instances<=r->mesh_limits.maxMeshWorkGroupCount[1] &&
           (uint64_t)groups*instances<=r->mesh_limits.maxMeshWorkGroupTotalCount)r->draw_mesh_tasks(cmd,groups,instances,1);
        else LOG_E(GPU,"mesh draw exceeds host group limits (%u x %u)",groups,instances);
    } else if (k.prim == GPU_PRIM_QUADLIST && d->count >= 4 && arena_alloc(r, (VkDeviceSize)(d->count / 4) * 24, &quad_off)) {
        /* Vulkan has no quad list: each quad (v0 v1 v2 v3) becomes (v0 v1 v2) (v0 v2 v3), same winding.
         * Unreal draws its color-grading LUT this way, one quad per slice: only half of it was filled. */
        uint32_t *q = (uint32_t *)arena_ptr(r, quad_off), quads = d->count / 4;
        const uint8_t *ix = d->indexed ? index_ptr : NULL;
        for (uint32_t j = 0; j < quads; j++) {
            uint32_t v[4];
            for (uint32_t c = 0; c < 4; c++) {
                uint32_t n = j * 4 + c;
                if (!ix) v[c] = n;
                else if (d->index_size == 1) v[c] = ix[n];
                else if (d->index_size == 4) memcpy(&v[c], ix + n * 4, 4);
                else { uint16_t s; memcpy(&s, ix + n * 2, 2); v[c] = s; }
            }
            uint32_t t[6] = {v[0], v[1], v[2], v[0], v[2], v[3]};
            memcpy(q + j * 6, t, sizeof(t));
        }
        vkCmdBindIndexBuffer(cmd, P->work->arena.buffer, quad_off, VK_INDEX_TYPE_UINT32);
        vk_diagnostic_checkpoint(r, 0, vs->prog->base, ps->prog->base, vs->prog->hash, ps->prog->hash);
        vkCmdDrawIndexed(cmd, quads * 6, instances, 0, base_vertex, 0);
    } else if (d->indexed) {
        vkCmdBindIndexBuffer(cmd, index_buf, index_off, d->index_size == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        vk_diagnostic_checkpoint(r, 0, vs->prog->base, ps->prog->base, vs->prog->hash, ps->prog->hash);
        vkCmdDrawIndexed(cmd, d->count, instances, 0, base_vertex, 0);
    } else {
        vk_diagnostic_checkpoint(r, 0, vs->prog->base, ps->prog->base, vs->prog->hash, ps->prog->hash);
        vkCmdDraw(cmd, d->count, instances, (uint32_t)base_vertex, 0);
    }
    vk_diagnostic_checkpoint(r, 1, vs->prog->base, ps->prog->base, vs->prog->hash, ps->prog->hash);
    if(occlusion_i>=0)vkCmdEndQuery(cmd,P->occlusion_pool,(uint32_t)occlusion_i);
    if(stat_query)vkCmdEndQuery(cmd,stat_query,(uint32_t)prof_i);
    prof_stop(r, prof_i, vs->prog->base, ps->prog->base, (uint64_t)d->count * instances);
    stat_phase(t_phase); /* draw commands */
    if (prof_i >= 0) {
        uint32_t back[2] = {0, 0};
        for (int s = 0; s < 2; s++) {
            const sh_program *pr = s ? ps->prog : vs->prog;
            for (uint32_t j = 0; j < pr->count; j++)
                back[s] += (pr->ops[j] == SH_OP_S_BRANCH || pr->ops[j] == SH_OP_S_CBRANCH_SCC0 ||
                            pr->ops[j] == SH_OP_S_CBRANCH_SCC1 || pr->ops[j] == SH_OP_S_CBRANCH_VCCZ ||
                            pr->ops[j] == SH_OP_S_CBRANCH_VCCNZ || pr->ops[j] == SH_OP_S_CBRANCH_EXECZ ||
                            pr->ops[j] == SH_OP_S_CBRANCH_EXECNZ) && pr->insts[j].simm16 < 0;
        }
        snprintf(P->prof->note[prof_i], sizeof(P->prof->note[0]),
                 "(%u/%u insts, %u/%u loops) %s %u x %u prim %u, target %ux%u (%u), viewport %g,%g %gx%g, blend %x",
                 vs->prog->count, ps->prog->count, back[0], back[1], d->indexed ? "indexed" : "auto", d->count, instances,
                 k.prim, fb_w, fb_h, bound, vp.x, vp.y, vp.width, vp.height, k.blend[0]);
    }
    P->draws++;
    return true;
}

/* Keep diagnostic draws on the same retry path as ordinary rendering.
 * A handled pipeline failure also returns true from do_draw; only the
 * command counter proves that a draw was actually recorded. */
static bool draw_with_retry(renderer *r, const gpu *g, const gpu_draw *d, stage_state *vs, stage_state *ps) {
    uint64_t before=r->pipes->draws;
    if(!do_draw(r,g,d,vs,ps)) {
        end_pass(r);
        /* A full descriptor pool or arena only needs a fresh slot: with overlap the
         * batch is submitted and the other slot continues (PSPHD's 4,500-dword
         * uniform draws filled the pool ~11 times a second, each a full GPU wait). */
        /* results still owed by earlier batches: complete them, or the retry fails the same way */
        if(vk_work_can_overlap(r) && !r->pipes->wb_count && !r->pipes->held_count)vk_flush(r);
        else sync_batch(r);
        vk_begin(r);
        if(!do_draw(r,g,d,vs,ps))r->pipes->skipped++;
    }
    return r->pipes->draws!=before;
}

/* PS5C_SYNC_DRAWS: waits for the GPU after every draw and dispatch and names
 * the one that does not finish in 2 s (a shader hang; debugging aid). */
static void sync_check(renderer *r, const char *what, uint64_t a, uint64_t b) {
    static int strict_order = -1;
    if (strict_order < 0) {
        const char *v = getenv("PS5C_STRICT_GPU_ORDER");
        strict_order = v && !strcmp(v, "1");
        if (strict_order) LOG_I(GPU, "strict GPU memory order: enabled (diagnostic, no per-draw CPU fence)");
    }
    if (strict_order && r->recording) {
        end_pass(r);
        VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
    }
    static int on = -1;
    static uint64_t limit_ms = 200; /* PS5C_SYNC_DRAWS=<ms>: log the ones slower than that */
    static uint64_t from_flip; /* PS5C_SYNC_DRAWS_FROM=<flip>: avoid synchronizing the early boot */
    if (on < 0) {
        on = getenv("PS5C_SYNC_DRAWS") != NULL;
        if (on && atoi(getenv("PS5C_SYNC_DRAWS")) > 1) limit_ms = (uint64_t)atoi(getenv("PS5C_SYNC_DRAWS"));
        const char *from = getenv("PS5C_SYNC_DRAWS_FROM");
        if (from) from_flip = strtoull(from, NULL, 10);
    }
    if (!on || g_trace_flip < from_flip || !r->recording) return;
    end_pass(r);
    vk_flush(r);
    uint64_t t0 = pal_time_ns();
    VkResult res = vk_work_wait(r, 2000000000ull);
    uint64_t ms = (pal_time_ns() - t0) / 1000000;
    if (res != VK_SUCCESS || ms > limit_ms)
        LOG_E(GPU, "%s 0x%llx / 0x%llx took %llu ms (VkResult %d)", what, (unsigned long long)a, (unsigned long long)b,
              (unsigned long long)ms, (int)res);
    if (res != VK_SUCCESS) {
        /* Diagnostic mode only: do not flood logs with every later draw after
         * the first failed/expired wait, or keep using a lost device. */
        LOG_E(GPU, "synchronized GPU diagnostic stopped at the first failed wait");
        pal_exit_process(1);
    }
}

/* PS5C_SAVE_AFTER=<hash of a pixel shader's first 32 bytes, its address or its program hash>
 * or @<file of up to eight hex hashes, reloaded once per flip>: color target 0
 * of its first draws goes to user/frames/after_*.bmp (debugging aid).
 * PS5C_SAVE_AFTER_IMAGE=<guest addresses in hex, comma separated>: inspect other images at that point,
 * for example the G-buffer consumed by a lighting shader.
 * PS5C_SAVE_AFTER_TARGET=<guest address in hex>: only draws to color target 0
 * at that address (a copy shader can be reused for many unrelated targets). */
static void save_after(renderer *r, const gpu *g, const sh_program *ps, bool compute) {
    static uint64_t wants[8], want;
    static uint32_t nwant;
    static const char *selection_file;
    static uint64_t selection_flip = UINT64_MAX;
    static int have = -1;
    static uint32_t saved;
    static guest_addr image_addrs[8], target_addr;
    static uint32_t image_count;
    if (have < 0) { /* several, separated by commas */
        have = getenv("PS5C_SAVE_AFTER") != NULL;
        const char *selection = getenv("PS5C_SAVE_AFTER");
        if (selection && selection[0] == '@') selection_file = selection + 1;
        else for (const char *p = selection; p && *p && nwant < 8; p = strchr(p, ',') ? strchr(p, ',') + 1 : "")
            wants[nwant++] = strtoull(p, NULL, 16);
        const char *image_setting = getenv("PS5C_SAVE_AFTER_IMAGE");
        for (const char *p = image_setting; have && p && *p && image_count < 8;
             p = strchr(p, ',') ? strchr(p, ',') + 1 : "")
            image_addrs[image_count++] = strtoull(p, NULL, 16);
    }
    if (selection_file && selection_flip != g->stats.flips) {
        selection_flip = g->stats.flips;
        nwant = 0;
        FILE *f = fopen(selection_file, "r");
        if (f) {
            unsigned long long hash;
            while (nwant < PS5C_ARRAY_COUNT(wants) && fscanf(f, "%llx", &hash) == 1)
                wants[nwant++] = (uint64_t)hash;
            fclose(f);
        }
    }
    static uint32_t seen, skip; /* PS5C_SAVE_AFTER_SKIP=<n>: start at the n-th draw */
    want = 0;
    for (uint32_t i = 0; i < nwant && !want; i++)
        if (fnv(ps->code, 32, FNV_INIT) == wants[i] || ps->base == wants[i] || ps->hash == wants[i]) want = wants[i];
    if (!have || !want) return;
    static bool target_checked;
    if (!target_checked) {
        const char *target = getenv("PS5C_SAVE_AFTER_TARGET");
        target_addr = target ? strtoull(target, NULL, 16) : 0;
        target_checked = true;
    }
    if (target_addr && ((guest_addr)gpu_reg(g, mmCB_COLOR0_BASE) << 8) != target_addr) return;
    /* PS5C_SAVE_AFTER_FRAME=<flip>: only in that frame.
     * @file: changing its numeric token arms one current frame, so a late
     * scene can be inspected repeatedly without restarting the game. */
    static int64_t frame = -2;
    static const char *frame_file;
    static unsigned long long frame_token;
    static bool have_token;
    if (frame == -2) {
        const char *setting = getenv("PS5C_SAVE_AFTER_FRAME");
        if (setting && setting[0] == '@') frame_file = setting + 1;
        frame = setting && !frame_file ? atoll(setting) : -1;
    }
    if (frame_file) {
        FILE *f = fopen(frame_file, "r");
        unsigned long long token = 0;
        if (!f && frame < 0) return;
        bool valid = f && fscanf(f, "%llu", &token) == 1;
        if (f) fclose(f);
        if (f && !valid) { /* an empty file: arms this frame once, then goes away */
            pal_file_remove(frame_file);
            token = frame_token + 1;
            valid = true;
        }
        if (valid && (!have_token || token != frame_token)) {
            have_token = true;
            frame_token = token;
            /* Arm the next complete frame. A token can arrive after an early
             * requested pass in the current frame has already executed. */
            frame = (int64_t)g->stats.flips + 1;
            saved = seen = skip = 0;
        }
    }
    if (saved >= 24) return;
    if (frame >= 0 && (int64_t)g->stats.flips != frame) return;
    if (!seen++ && getenv("PS5C_SAVE_AFTER_SKIP")) skip = (uint32_t)atoi(getenv("PS5C_SAVE_AFTER_SKIP"));
    if (seen <= skip) return;
    /* PS5C_SAVE_AFTER_EVERY=<n>: every n-th draw from there (24 snapshots spread over a frame) */
    static int every = -1;
    if (every < 0) every = getenv("PS5C_SAVE_AFTER_EVERY") ? atoi(getenv("PS5C_SAVE_AFTER_EVERY")) : 1;
    if (every > 1 && (seen - skip - 1) % (uint32_t)every) return;
    bool synced = false;
    static int deferred=-1;
    if(deferred<0)deferred=getenv("PS5C_SAVE_AFTER_DEFER")!=NULL;
    if (deferred)
        LOG_I(GPU, "snapshot state PS %llx: depth %08x zbase %08x/%08x zinfo %08x su %08x offset %g/%g %g/%g clamp %g",
              (unsigned long long)ps->base, gpu_reg(g, mmDB_DEPTH_CONTROL), gpu_reg(g, mmDB_Z_READ_BASE),
              gpu_reg(g, mmDB_Z_WRITE_BASE), gpu_reg(g, mmDB_Z_INFO), gpu_reg(g, mmPA_SU_SC_MODE_CNTL),
              (double)f32_of(gpu_reg(g, 0xA2E0)), (double)f32_of(gpu_reg(g, 0xA2E1)),
              (double)f32_of(gpu_reg(g, 0xA2E2)), (double)f32_of(gpu_reg(g, 0xA2E3)),
              (double)f32_of(gpu_reg(g, 0xA2DF)));
    for (uint32_t j = 0; j < (image_count ? image_count : 1) && saved < 24; j++) {
        guest_addr base = image_count ? image_addrs[j] : (guest_addr)gpu_reg(g, mmCB_COLOR0_BASE) << 8;
        vk_image *img = NULL;
        for (uint32_t i = 0; i < r->image_count; i++) {
            vk_image *c = &r->images[i];
            if (c->addr == base && c->image && c->gpu_written && (!img || c->written_seq > img->written_seq)) img = c;
        }
        if (!img) continue;
        if (!synced && !deferred) {
            end_pass(r);
            sync_batch(r);
            synced = true;
        }
        char path[96];
        if (image_count) {
            if (frame_file)
                snprintf(path, sizeof(path), "user/frames/after_%016llx_f%llu_%u_%010llx.bmp",
                         (unsigned long long)want, (unsigned long long)g->stats.flips, saved++, (unsigned long long)base);
            else
                snprintf(path, sizeof(path), "user/frames/after_%016llx_%u_%010llx.bmp",
                         (unsigned long long)want, saved++, (unsigned long long)base);
            LOG_I(GPU, "pass image %s 0x%llx flip %llu: 0x%llx %ux%u fmt %d written seq %llu image %llx -> %s",
                  compute?"CS":"PS", (unsigned long long)ps->base, (unsigned long long)g->stats.flips,
                  (unsigned long long)base, img->width, img->height, (int)img->format,
                  (unsigned long long)img->written_seq, (unsigned long long)(uintptr_t)img->image, path);
        } else {
            if (frame_file)
                snprintf(path, sizeof(path), "user/frames/after_%016llx_f%llu_%u.bmp",
                         (unsigned long long)want, (unsigned long long)g->stats.flips, saved++);
            else
                snprintf(path, sizeof(path), "user/frames/after_%016llx_%u.bmp", (unsigned long long)want, saved++);
        }
        if(deferred) {
            char delayed[128];snprintf(delayed,sizeof(delayed),"user/frames/deferred_%s",path+12);
            if(!vk_capture_defer(r,img,delayed))LOG_W(GPU,"pass snapshot unavailable: %s",delayed);
        } else if (img->aspect == VK_IMAGE_ASPECT_COLOR_BIT) vk_save_image(r, img, path);
        else vk_save_depth(r, img, path); /* grey, min..max */
    }
    if (synced) vk_begin(r);
}

/* A draw that copies the depth surface at DB_Z_READ_BASE to the one at DB_Z_WRITE_BASE
 * instead of rasterizing: DB_RENDER_CONTROL.DEPTH_COPY [2], or (as shadPS4 detects it)
 * DB_RENDER_OVERRIDE FORCE_Z_DIRTY [27] + FORCE_Z_VALID [29] with color writes off.
 * Sonic Frontiers copies its depth into the texture the lighting samples. */
static bool depth_copy(renderer *r, const gpu *g) {
    uint32_t rc = gpu_reg(g, mmDB_RENDER_CONTROL), zinfo = gpu_reg(g, mmDB_Z_INFO), size = gpu_reg(g, mmDB_DEPTH_SIZE_XY);
    uint32_t ov = gpu_reg(g, 0xA003u /* DB_RENDER_OVERRIDE */);
    guest_addr src_addr = (guest_addr)gpu_reg(g, mmDB_Z_READ_BASE) << 8, dst_addr = (guest_addr)gpu_reg(g, mmDB_Z_WRITE_BASE) << 8;
    uint32_t zfmt = zinfo & 3;
    bool color_off = ((gpu_reg(g, mmCB_COLOR_CONTROL) >> 4) & 7) == 0 || !gpu_reg(g, mmCB_TARGET_MASK);
    bool copy = ((rc >> 2) & 1) || (((ov >> 27) & 1) && ((ov >> 29) & 1) && color_off);
    if (!copy || !src_addr || !dst_addr || src_addr == dst_addr || !(zfmt == 1 || zfmt == 3) || !size) return false;
    static int off = -1; /* PS5C_NO_DEPTH_COPY: debugging aid, draw as before */
    if (off < 0) off = getenv("PS5C_NO_DEPTH_COPY") != NULL;
    if (off) return false;
    uint32_t w = (size & 0x3FFF) + 1, h = ((size >> 16) & 0x3FFF) + 1;
    VkFormat f = db_format(g);
    vk_image *src = vk_image_find(r, src_addr, w, h, f);
    if (!src && f != VK_FORMAT_D16_UNORM) src = find_depth32(r, src_addr, w, h, false), f = src ? src->format : f;
    static uint32_t logged;
    if (logged < 16) {
        logged++;
        LOG_I(GPU, "depth copy 0x%llx -> 0x%llx %ux%u (render control %08x, override %08x)%s", (unsigned long long)src_addr,
              (unsigned long long)dst_addr, w, h, rc, ov, src ? "" : ": no source image");
    }
    if (!src) return true;
    r->scale_new = src->ext_w != 0; /* PS5C_RT_SCALE: the copy at the source's size */
    vk_image *dst = vk_image_get(r, dst_addr, w, h, f);
    r->scale_new = false;
    if (!dst || dst == src) return true;
    if (vk_img_w(dst) != vk_img_w(src) || vk_img_h(dst) != vk_img_h(src)) {
        static uint32_t mismatch;
        if (mismatch++ < 8) LOG_W(GPU, "depth copy 0x%llx: scaled and full-size depth (PS5C_RT_SCALE), not copied", (unsigned long long)dst_addr);
        return true;
    }
    w = vk_img_w(src), h = vk_img_h(src);
    end_pass(r);
    vk_barrier(r->cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vk_barrier(r->cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy c = {0};
    c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    c.srcSubresource.layerCount = c.dstSubresource.layerCount = 1;
    c.extent.width = w;
    c.extent.height = h;
    c.extent.depth = 1;
    census_copy(src, 1);
    vkCmdCopyImage(r->cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    dst->gpu_written = true;
    dst->written_seq = ++r->write_seq;
    return true;
}

void renderer_draw(renderer *r, const gpu *g, const gpu_draw *d) {
    g_trace_flip = g_flow_flip = g->stats.flips;
    pal_mutex_lock(r->lock);
    if (!vk_guest_access_begin(r)) { pal_mutex_unlock(r->lock); return; }
    vk_cpu_prepare(r,g->stats.flips);
    uint64_t t_draw = stat_now();
    vk_begin(r);
    retired_poll(r);
    uint32_t mode = (gpu_reg(g, mmCB_COLOR_CONTROL) >> 4) & 7;
    static long long cb_modes = -2; /* PS5C_LOG_CB_MODES=<flip>: each draw's CB mode and first targets (debugging aid) */
    if (cb_modes == -2) cb_modes = getenv("PS5C_LOG_CB_MODES") ? atoll(getenv("PS5C_LOG_CB_MODES")) : -1;
    if (cb_modes >= 0 && (long long)g->stats.flips == cb_modes)
        LOG_I(GPU, "CB mode %u: RT0 0x%llx info %08x, RT1 0x%llx info %08x, target mask %08x", mode,
              (unsigned long long)gpu_reg(g, mmCB_COLOR0_BASE) << 8, gpu_reg(g, mmCB_COLOR0_INFO),
              (unsigned long long)gpu_reg(g, mmCB_COLOR0_BASE + GPU_CB_STRIDE) << 8, gpu_reg(g, mmCB_COLOR0_INFO + GPU_CB_STRIDE),
              gpu_reg(g, mmCB_TARGET_MASK));
    if (mode == 3) {
        large_import_cache_drop(r);
        resolve(r, g);
        vk_guest_access_end(r);
        pal_mutex_unlock(r->lock);
        return;
    }
    if (mode == 2 || mode >= 4) {
        large_import_cache_drop(r);
        /* fast-clear eliminate, FMASK / DCC decompress: compression metadata
         * passes; surfaces are never compressed here */
        vk_guest_access_end(r);
        pal_mutex_unlock(r->lock);
        return;
    }
    if (depth_copy(r, g)) {
        large_import_cache_drop(r);
        vk_guest_access_end(r);
        pal_mutex_unlock(r->lock);
        return;
    }
    uint64_t t_phase = stat_phase(t_draw); /* mode/depth-copy checks */
    bool reuse_stages=reuse_draw_stages(r->pipes,g->stats.flips);
    stage_state *st = get_draw_stages(r->pipes,reuse_stages);
    if (!st) {
        vk_guest_access_end(r);
        pal_mutex_unlock(r->lock);
        return;
    }
    stage_state *vs = st, *ps = st + 1;
    vs->wave_locked=ps->wave_locked=false;
    ngg_large_reads *large_reads=NULL;bool large_pinned=false;
    r->pipes->wave_operation++;
    sh_program *mesh_program=NULL,*pixel_wave_program=NULL;
    uint64_t t_prof = prof_now(r);
    uint64_t t0 = stat_now();
    vs->prog = get_program(r, gpu_reg_addr256(g, mmSPI_SHADER_PGM_LO_ES));
    if (!vs->prog && !getenv("PS5C_NO_FUSED")) /* the first half of a merged-stage program */
        vs->prog = get_fused_program(r, gpu_reg_addr256(g, mmSPI_SHADER_PGM_LO_ES), gpu_reg_addr256(g, mmSPI_SHADER_PGM_LO_GS), 0);
    bool clear_only = vk_depth_stencil_clear_only(gpu_reg(g, mmDB_RENDER_CONTROL), gpu_reg(g, mmCB_COLOR_CONTROL),
                                          gpu_reg(g, mmCB_TARGET_MASK));
    /* No color output, no shader depth/stencil/kill/coverage and no requested
     * side effects: an old PS address is irrelevant. Skyrim leaves an alpha PS
     * here while its position-only VS exports no interpolants for that PS. */
    bool opaque_depth = vk_opaque_depth_only(gpu_reg(g, mmDB_SHADER_CONTROL), gpu_reg(g, mmCB_COLOR_CONTROL),
                                            gpu_reg(g, mmCB_TARGET_MASK));
    guest_addr ps_addr = gpu_reg_addr256(g, mmSPI_SHADER_PGM_LO_PS);
    const sh_program *guest_ps = clear_only || g->gcn ? NULL : get_radv_pixel_program(r, ps_addr, gpu_reg(g, mmSPI_SHADER_USER_DATA_PS_0 + 3));
    if (!clear_only && !guest_ps) guest_ps = get_program(r, ps_addr);
    /* Storage/atomic instructions are observable even without color exports.
     * Keep such a PS; DB metadata alone must not erase its memory effects. */
    opaque_depth = opaque_depth && (!guest_ps || !guest_ps->has_stores);
    ps->prog = clear_only || opaque_depth ? depth_clear_program(r) : guest_ps;
    { /* Bounded audit before any experimental register-driven variant. */
        static int audit=-1;static uint64_t seen[64];static uint32_t count;
        if(audit<0)audit=getenv("PS5C_LOG_PS_WAVE_MISMATCH")!=NULL;
        uint32_t control=gpu_reg(g,0xA1B6u /* SPI_PS_IN_CONTROL */);
        if(audit && guest_ps && (((control>>15)&1u)!=guest_ps->wave32)) {
            uint64_t key=guest_ps->hash^((uint64_t)(control&0x8000u)<<48);bool found=false;
            for(uint32_t i=0;i<count;i++)found|=seen[i]==key;
            if(!found && count<PS5C_ARRAY_COUNT(seen)) {
                seen[count++]=key;
                LOG_I(GPU,"PS wave mismatch: %llx hash %016llx inferred %u register %u control %08x flip %llu",
                    (unsigned long long)guest_ps->base,(unsigned long long)guest_ps->hash,guest_ps->wave32,
                    (control>>15)&1u,control,(unsigned long long)g->stats.flips);
            }
        }
    }
    /* Diagnostic A/B: opcode use of VCC_HI cannot determine the guest mode.
     * A file can toggle the register-driven mode only at flip boundaries. */
    const char *pixel_wave=getenv("PS5C_PS_WAVE_FROM_REGS");
    bool pixel_wave_on=pixel_wave && ps->prog && ps->prog==guest_ps &&
        (!strcmp(pixel_wave,"1") || strtoull(pixel_wave,NULL,16)==ps->prog->hash);
    if(pixel_wave && pixel_wave[0]=='@') {
        static uint64_t flip=UINT64_MAX;static unsigned enabled;
        if(flip!=g->stats.flips) {
            flip=g->stats.flips;enabled=0;FILE *f=fopen(pixel_wave+1,"r");
            if(f){if(fscanf(f,"%u",&enabled)!=1)enabled=0;fclose(f);}
        }
        pixel_wave_on=enabled==1 && ps->prog && ps->prog==guest_ps;
    }
    if(pixel_wave_on) {
        bool wave32=(gpu_reg(g,0xA1B6u /* SPI_PS_IN_CONTROL */)&0x8000u)!=0;
        ps->wave_locked=true;ps->wave32_expected=wave32;
        if(wave32!=ps->prog->wave32) {
            const sh_program *variant=program_with_wave(r,ps->prog,wave32,g->stats.flips,&pixel_wave_program);
            if(variant)ps->prog=variant;
        }
    }

    stat_add(ST_PROGRAM, t0);
    t_phase = stat_phase(t_phase); /* programs */
    { /* PS5C_SKIP_PS=<address/hash> or @<file>: opt-in draw comparison.
       * A file contains whitespace-separated hex addresses/hashes and is read
       * once per flip, allowing effects to be toggled without restarting. */
        static uint64_t skips[32], last_flip = UINT64_MAX;
        static uint32_t nskip;
        static const char *skip_file;
        static int have = -1;
        if (have < 0) {
            const char *setting = getenv("PS5C_SKIP_PS");
            have = setting != NULL;
            if (have && setting[0] == '@') skip_file = setting + 1;
            else if (have) {
                skips[0] = strtoull(setting, NULL, 16);
                nskip = 1;
            }
        }
        if (skip_file && last_flip != g->stats.flips) {
            last_flip = g->stats.flips;
            nskip = 0;
            FILE *f = fopen(skip_file, "r");
            if (f) {
                unsigned long long value;
                while (nskip < PS5C_ARRAY_COUNT(skips) && fscanf(f, "%llx", &value) == 1)
                    skips[nskip++] = (uint64_t)value;
                fclose(f);
            }
        }
        for (uint32_t i = 0; have && ps->prog && i < nskip; i++)
            if (ps->prog->base == skips[i] || ps->prog->hash == skips[i]) ps->prog = NULL;
    }
    guest_addr za;
    uint32_t zw, zh;
    VkFormat zf;
    { /* PS5C_LOG_DRAW_PATH=<n>: from the n-th draw on, 300 lines on what happens to each (debugging aid) */
        static int64_t from = -2;
        static uint64_t n;
        if (from == -2) from = getenv("PS5C_LOG_DRAW_PATH") ? atoll(getenv("PS5C_LOG_DRAW_PATH")) : -1;
        n++;
        if (from >= 0 && n >= (uint64_t)from && n < (uint64_t)from + 300) {
            bool z = depth_target(g, &za, &zw, &zh, &zf);
            LOG_I(GPU, "draw %llu: VS 0x%llx %s, PS 0x%llx %s, mode %u, depth %d, RT0 0x%llx, count %u",
                  (unsigned long long)n, (unsigned long long)gpu_reg_addr256(g, mmSPI_SHADER_PGM_LO_ES),
                  vs->prog ? "ok" : "MISSING", (unsigned long long)gpu_reg_addr256(g, mmSPI_SHADER_PGM_LO_PS),
                  ps->prog ? "ok" : "MISSING", mode, (int)z, (unsigned long long)((guest_addr)gpu_reg(g, mmCB_COLOR0_BASE) << 8),
                  d->count);
        }
    }
    if (vs->prog && ps->prog && (mode != 0 || depth_target(g, &za, &zw, &zh, &zf))) {
        {
            static int have = -1;
            static uint64_t hash;
            static struct { uint64_t hash; unsigned lines; } seen[32];
            static unsigned count;
            if(have<0){const char*s=getenv("PS5C_LOG_NGG");have=s!=NULL;if(s)hash=strtoull(s,NULL,16);}
            unsigned at=0;
            if(have && (!hash || vs->prog->hash==hash)) {
                while(at<count && seen[at].hash!=vs->prog->hash)at++;
                if(at==count && count<PS5C_ARRAY_COUNT(seen))seen[count++].hash=vs->prog->hash;
            } else at=PS5C_ARRAY_COUNT(seen);
            if(at<count && seen[at].lines++<4)
                LOG_I(GPU,"NGG state: VS %016llx count %u instances %u indexed %u prim %u GE_CNTL %08x stages %08x max_output %u max_vert %u out_prim %08x GS_RSRC1 %08x GS_RSRC2 %08x",
                      (unsigned long long)vs->prog->hash,d->count,d->instances,(unsigned)d->indexed,gpu_reg(g,mmVGT_PRIMITIVE_TYPE),
                      gpu_reg(g,mmGE_CNTL),gpu_reg(g,mmVGT_SHADER_STAGES_EN),gpu_reg(g,mmGE_MAX_OUTPUT_PER_SUBGROUP),
                      gpu_reg(g,mmVGT_GS_MAX_VERT_OUT),gpu_reg(g,mmVGT_GS_OUT_PRIM_TYPE),
                      gpu_reg(g,mmSPI_SHADER_PGM_RSRC1_GS),gpu_reg(g,mmSPI_SHADER_PGM_RSRC2_GS));
        }
        /* USER_SGPR [5:1], USER_SGPR_MSB [27]: up to 32 user data registers */
        uint32_t gs_rsrc2 = gpu_reg(g, mmSPI_SHADER_PGM_RSRC2_GS), ps_rsrc2 = gpu_reg(g, mmSPI_SHADER_PGM_RSRC2_PS);
        uint32_t vs_user = ((gs_rsrc2 >> 1) & 0x1F) | ((gs_rsrc2 >> 22) & 0x20);
        uint32_t ps_user = ((ps_rsrc2 >> 1) & 0x1F) | ((ps_rsrc2 >> 22) & 0x20);
        init_sgprs(&vs->eval, g, 8, mmSPI_SHADER_USER_DATA_GS_0, vs_user);
        vs->gcn = ps->gcn = g_gcn = g->gcn;
        if (g->gcn) {
            /* a PS4 VS (hardware VS stage): user data from s0, no merged-wave SGPRs */
            uint32_t vs_rsrc2 = gpu_reg(g, GPU_GCN_RSRC2_VS);
            init_sgprs(&vs->eval, g, 0, GPU_GCN_USER_DATA_VS_0, (vs_rsrc2 >> 1) & 0x1F);
        } else if(gpu_reg(g,mmVGT_SHADER_STAGES_EN)&0x20u) {
            /* A merged ES/GS launches the GS back half with its separate
             * user-data table in s0:s1. It is a byte address, not a PGM
             * address shifted by256. Zeroing it makes the GS load null
             * descriptors despite valid ES user data in s8 and above. */
            vs->eval.sgpr_init[0]=gpu_reg(g,mmSPI_SHADER_USER_DATA_ADDR_LO_GS);
            vs->eval.sgpr_init[1]=gpu_reg(g,mmSPI_SHADER_USER_DATA_ADDR_HI_GS);
        }
        if (!g->gcn) {
            vs->eval.sgpr_init[3] = 64u | (64u << 8); /* merged wave info: full wave of vertices and primitives */
            /* GS_TG_INFO: the subgroup's vertex [20:12] and primitive [30:22] counts. NGG shaders that
             * export only below these (No Man's Sky's v_cmpx against them) exported nothing at zero. */
            vs->eval.sgpr_init[2] = (256u << 12) | (256u << 22);
        }
        init_sgprs(&ps->eval, g, 0, mmSPI_SHADER_USER_DATA_PS_0, ps_user);
        vs->info.stage = SH_STAGE_VS;
        vs->info.mesh_input_vertices=vs->info.mesh_max_vertices=vs->info.mesh_max_primitives=0;
        vs->info.mesh_primitive_vertices=vs->info.mesh_indexed=0;
        vs->info.mesh_compute=vs->info.mesh_output_binding=0;
        vs->info.barrier_prefix=false;
        vs->info.wave_barriers=false;
        vs->info.fp16_overflow = g->gcn ? 0 : sh_fp16_overflow_bit(SH_STAGE_VS, gpu_reg(g, mmSPI_SHADER_PGM_RSRC1_GS));
        vs->info.vertex_id_vgpr = g->gcn ? 0 : 5;   /* PS4 VS: v0 vertex index, v3 instance */
        vs->info.instance_id_vgpr = g->gcn ? 3 : 8;
        vs->info.vs_layer = (gpu_reg(g, 0xA207u /* PA_CL_VS_OUT_CNTL */) >> 18) & 1;
        { /* user clip planes: PA_CL_CLIP_CNTL UCP_ENA [5:0], off with CLIP_DISABLE [16]; rectangle
           * and quad lists (expanded by a geometry shader) are not clipped by them */
            uint32_t clip = gpu_reg(g, mmPA_CL_CLIP_CNTL), prim = gpu_reg(g, mmVGT_PRIMITIVE_TYPE);
            vs->info.vs_ucp_mask = r->clip_distance && !((clip >> 16) & 1) && prim != 7 && prim != GPU_PRIM_RECTLIST ? clip & 63u : 0;
        }
        vs->info.vs_point_size = 0;
        if (gpu_reg(g, mmVGT_PRIMITIVE_TYPE) == GPU_PRIM_POINTLIST) {
            /* PA_SU_POINT_SIZE HEIGHT [15:0] and PA_SU_POINT_MINMAX MIN/MAX: eighths of a pixel
             * (radeonsi writes size * 8); unset registers draw one-pixel points */
            uint32_t size = gpu_reg(g, 0xA280u) & 0xFFFF, mm = gpu_reg(g, 0xA281u);
            uint32_t lo = mm & 0xFFFF, hi = mm >> 16;
            if (!size) size = 8;
            if (hi && size > hi) size = hi;
            if (size < lo) size = lo;
            float f = size / 8.0f;
            memcpy(&vs->info.vs_point_size, &f, 4);
        }
        vs->info.lds_dwords = sh_graphics_lds_dwords(gs_rsrc2);
        {
            /* The central environment cache avoids CRT lookups and owns these strings.
             * Reacquire them each draw because tests reset that cache between draws. */
            uint64_t wanted = 0, tri_wanted = 0;
            const char *value = getenv("PS5C_NGG_MESH_SHADER");
            if (value) wanted = strtoull(value, NULL, 16);
            value = getenv("PS5C_NGG_MESH_TRI_SHADER");
            if (value) tri_wanted = !strcmp(value, "all") ? UINT64_MAX : strtoull(value, NULL, 16);
            else if(r->ngg_culling_profile)tri_wanted=UINT64_MAX;
            const char *waves = getenv("PS5C_NGG_MESH_WAVES");
            const char *fast_setting = getenv("PS5C_NGG_FAST_LAUNCH");
            const char *compute_setting = getenv("PS5C_NGG_COMPUTE");
            const char *prefix = getenv("PS5C_NGG_BARRIER_PREFIX");
            const char *wave_barriers = getenv("PS5C_NGG_WAVE_BARRIERS");
            const char *leaf_calls = getenv("PS5C_SHADER_LEAF_INLINE");
            const char *join_prefix = getenv("PS5C_NGG_JOIN_PREFIX");
            uint32_t maxvert=gpu_reg(g,mmVGT_GS_MAX_VERT_OUT),stages=gpu_reg(g,mmVGT_SHADER_STAGES_EN);
            bool two_waves=waves && !strcmp(waves,"2") && r->mesh_native32 &&
                r->mesh_limits.maxMeshWorkGroupInvocations>=128 &&
                r->mesh_limits.maxMeshWorkGroupSize[0]>=128 &&
                r->mesh_limits.maxMeshOutputVertices>=128 && r->mesh_limits.maxMeshOutputPrimitives>=128;
            /* MAX_VERT_OUT is per GS invocation. The compiled subgroup
             * prologue also initializes/compacts GE_MAX_OUTPUT slots. A
             * four-output GS with80 total slots still needs a second wave;
             * partitioning its inputs cannot shrink those static LDS loops. */
            uint32_t group_output=gpu_reg(g,mmGE_MAX_OUTPUT_PER_SUBGROUP);
            uint32_t amp=gpu_reg(g,mmGE_NGG_SUBGRP_CNTL)&0x1ffu;
            uint32_t fast_threads=fast_setting && !strcmp(fast_setting,"1") ?
                vk_ngg_fast_launch_threads(stages,gpu_reg(g,mmGE_CNTL),gpu_reg(g,mmVGT_PRIMITIVE_TYPE),
                                          maxvert,group_output,amp,d->indexed):0;
            bool fast=fast_threads && r->cs_subgroup==32 &&
                r->props.limits.maxComputeWorkGroupInvocations>=fast_threads &&
                r->props.limits.maxComputeWorkGroupSize[0]>=fast_threads;
            bool compute_limits=maxvert && maxvert<=256 && group_output<=256 &&
                r->props.limits.maxComputeWorkGroupInvocations>=256 && r->props.limits.maxComputeWorkGroupSize[0]>=256 && r->cs_subgroup==32;
            /* A GS on line lists (PSPHD turns each PSP sprite, two corners, into a
             * four-vertex strip) has no other path: drawn as lines, only each
             * sprite's diagonal showed. Compute by default; PS5C_NGG_COMPUTE=0 turns it off. */
            bool line=gpu_reg(g,mmVGT_PRIMITIVE_TYPE)==GPU_PRIM_LINELIST && (stages&0x20u) && !(d->count&1u) &&
                !(compute_setting && !strcmp(compute_setting,"0"));
            bool compute=fast || (compute_limits && line) || (compute_setting && (!strcmp(compute_setting,"1") || strtoull(compute_setting,NULL,16)==vs->prog->hash) &&
                compute_limits && gpu_reg(g,mmVGT_PRIMITIVE_TYPE)==GPU_PRIM_POINTLIST);
            uint32_t mesh_threads=fast?fast_threads:compute?256:two_waves && maxvert && (maxvert>64 || group_output>64)?128:64;
            bool point=wanted && vs->prog->hash==wanted &&
                gpu_reg(g,mmVGT_PRIMITIVE_TYPE)==GPU_PRIM_POINTLIST && maxvert &&
                maxvert<=mesh_threads && group_output<=mesh_threads;
            /* Native NGG passthrough. Nonindexed strips are expanded with
             * alternating winding; indexed restart and fast launch remain out. */
            bool strip=gpu_reg(g,mmVGT_PRIMITIVE_TYPE)==GPU_PRIM_TRISTRIP && !d->indexed && d->count>=3 && d->count<=UINT32_MAX/3;
            bool culling_triangle=(r->ngg_culling_profile || getenv("PS5C_NGG_CULL_TRIANGLE")) && vk_ngg_culling_triangle(stages);
            bool triangle=tri_wanted && (tri_wanted==UINT64_MAX || vs->prog->hash==tri_wanted) &&
                (culling_triangle || (!maxvert && !(stages&0x30u) && (stages&0x02000000u))) &&
                ((gpu_reg(g,mmVGT_PRIMITIVE_TYPE)==GPU_PRIM_TRILIST && d->count && !(d->count%3)) || strip) &&
                ((gpu_reg(g,mmGE_CNTL)>>9)&0x1ffu)>=3;
            bool mesh_index_supported = !d->indexed || d->index_size == 2 || d->index_size == 4 ||
                (d->index_size == 1 && !(gpu_reg(g, mmVGT_MULTI_PRIM_IB_RESET_EN) & 1u));
            if(d->count && ((r->mesh_shader && (point || triangle)) || compute) &&
               (uint64_t)sh_graphics_lds_dwords(gs_rsrc2)*4+2048<=(compute?r->props.limits.maxComputeSharedMemorySize:r->mesh_limits.maxMeshSharedMemorySize) && (fast || !((stages>>19)&3u)) &&
               mesh_index_supported &&
               !vs->info.vs_layer && !vs->info.vs_ucp_mask && !vs->prog->has_stores) {
                const sh_program *variant=program_with_wave(r,vs->prog,fast,g->stats.flips,&mesh_program);
                if(variant) {
                    vs->prog=variant;
                    vs->info.stage=SH_STAGE_MESH;vs->info.vs_point_size=0;
                    vs->info.local_size[0]=mesh_threads;vs->info.local_size[1]=vs->info.local_size[2]=1;
                    memset(vs->info.host_local_size,0,sizeof(vs->info.host_local_size));
                    vs->info.mesh_compute=compute;
                    vs->info.barrier_prefix = fast || (prefix && (!strcmp(prefix,"1") || strtoull(prefix,NULL,16)==vs->prog->hash));
                    vs->info.wave_barriers = fast || (wave_barriers &&
                        (!strcmp(wave_barriers,"1") || strtoull(wave_barriers,NULL,16)==vs->prog->hash));
                    vs->info.mesh_input_vertices=fast?1:triangle?63:PS5C_MIN(64u,mesh_threads/maxvert);
                    vs->info.mesh_primitive_vertices=triangle?3:line?2:1;
                    vs->info.mesh_indexed=d->indexed?1u:strip?2u:0u;
                    if(triangle && culling_triangle && !(stages&0x02000000u))vs->info.mesh_indexed|=SH_MESH_NGG_CULL;
                    if(fast)vs->info.mesh_indexed|=SH_MESH_FAST_LAUNCH;
                    if(fast && vs->prog->function_count>1 && leaf_calls && !strcmp(leaf_calls,"1"))vs->info.mesh_indexed|=SH_LEAF_CALL_INLINE;
                    if(fast && join_prefix && (!strcmp(join_prefix,"1") || !strcmp(join_prefix,"2"))) {
                        vs->info.mesh_indexed|=SH_JOIN_PREFIX;
                        if(!strcmp(join_prefix,"2"))vs->info.mesh_indexed|=SH_JOIN_SUFFIX;
                    }
                    VkShaderStageFlagBits host_stage=compute?VK_SHADER_STAGE_COMPUTE_BIT:(VkShaderStageFlagBits)0x80; /* MESH_EXT */
                    if(ngg_ballot32(r->pipes,g->stats.flips) && (r->ballot32_stages&host_stage) &&
                       (compute?r->cs_subgroup==32:r->mesh_native32))vs->info.mesh_indexed|=SH_MESH_BALLOT32;
                    uint32_t expanded=strip?(d->count-2)*3:d->count;
                    uint32_t ge=gpu_reg(g,mmGE_CNTL),group_prims=ge&0x1ffu,group_vertices=(ge>>9)&0x1ffu;
                    if(!group_prims)group_prims=1;
                    if(!group_vertices)group_vertices=1;
                    if(triangle)group_prims*=3;
                    if(line)group_prims*=2;
                    /* Line GS groups export at most one wave64 of vertices: the exports
                     * of a second wave were lost (PSPHD's 30-sprite clears, four
                     * vertices each, covered only their first 16 sprites). */
                    if(line && maxvert && group_prims>64/maxvert*2)group_prims=maxvert<=32?64/maxvert*2:2;
                    if(vs->info.mesh_input_vertices>group_prims)vs->info.mesh_input_vertices=group_prims;
                    if(vs->info.mesh_input_vertices>group_vertices)vs->info.mesh_input_vertices=group_vertices;
                    if(triangle)vs->info.mesh_input_vertices-=vs->info.mesh_input_vertices%3;
                    if(line)vs->info.mesh_input_vertices&=~1u;
                    /* Every partition must expose the same input count to
                     * scalar evaluation and to the GPU prologue. Otherwise
                     * pre-evaluated SMEM can come from the legacy 256-lane
                     * path while the mesh shader selects another address. */
                    /* Line GS draws (PSPHD) keep one partition size: one compute
                     * pipeline per sprite shader instead of one per vertex count;
                     * the prologue reads the count and shortens the last group. */
                    while(!line && vs->info.mesh_input_vertices>vs->info.mesh_primitive_vertices && expanded%vs->info.mesh_input_vertices)
                        vs->info.mesh_input_vertices-=vs->info.mesh_primitive_vertices;
                    if(compute && !fast && ngg_compact_groups(r->pipes,g->stats.flips)) {
                        mesh_threads=vk_ngg_group_threads(group_output,maxvert,vs->info.mesh_input_vertices);
                        vs->info.local_size[0]=mesh_threads;
                    }
                    uint32_t inputs=PS5C_MIN(expanded,vs->info.mesh_input_vertices);
                    uint32_t prims=inputs/vs->info.mesh_primitive_vertices;
                    vs->eval.sgpr_init[2]=(inputs<<12)|(prims<<22);
                    vs->eval.sgpr_init[3]=inputs|(prims<<8)|((mesh_threads/(fast?32:64))<<28);
                    vs->info.mesh_max_vertices=triangle?vs->info.mesh_input_vertices:vs->info.mesh_input_vertices*maxvert;
                    vs->info.mesh_max_primitives=fast?amp:triangle?vs->info.mesh_input_vertices/3:mesh_threads;
                    vs->info.lds_dwords=sh_graphics_lds_dwords(gs_rsrc2);
                }
            }
        }
        ps->info.stage = SH_STAGE_PS;
        ps->info.fp16_overflow = sh_fp16_overflow_bit(SH_STAGE_PS, gpu_reg(g, mmSPI_SHADER_PGM_RSRC1_PS));
        t_phase = stat_phase(t_phase); /* stage info */
        bool vs_ok = setup_stage(r, vs), ps_ok = vs_ok && setup_stage(r, ps);
        t_phase = stat_phase(t_phase); /* setup_stage */
        if (!ps_ok) {
            static uint32_t warned;
            if (warned++ < 8)
                LOG_W(GPU, "draw skipped: %s 0x%llx evaluation failed", vs_ok ? "PS" : "VS",
                      (unsigned long long)(vs_ok ? ps->prog->base : vs->prog->base));
        }
        if (ps_ok) {
            bool want_large=false;
            if(ngg_large_reads_on(g->stats.flips) && r->host_import_align &&
               !vs->prog->has_stores && !ps->prog->has_stores) {
                for(uint32_t stage=0;stage<2;stage++)for(uint32_t i=0;i<st[stage].prog->site_count;i++) {
                    const sh_site_value *v=st[stage].eval.site+i;sh_site_kind kind=st[stage].prog->sites[i].kind;
                    uint32_t stride=(v->desc[1]>>16)&0x3fff;
                    uint64_t bytes=stride?(uint64_t)v->desc[2]*stride:v->desc[2];
                    want_large|=(kind==SH_SITE_BUFFER || kind==SH_SITE_SBUFFER) && v->valid &&
                                !v->desc_unknown && bytes>MAX_BUFFER_COPY && bytes<=r->props.limits.maxStorageBufferRange;
                }
            }
            ngg_large_key keys[16];uint32_t key_count=0;
            bool retain=want_large && large_import_cache_enabled() && large_import_signature(r,st,keys,&key_count);
            vk_pipelines *P=r->pipes;
            if(P->large_import_cache && (!retain || !large_import_signature_reusable(P,keys,key_count,g->stats.flips)))
                large_import_cache_drop(r);
            if(want_large) {
                if(P->large_import_cache) {
                    large_reads=P->large_import_cache;large_pinned=true;P->large_import_reuses++;
                } else large_reads=calloc(1,sizeof(*large_reads));
            }
            if(large_reads) {
                end_pass(r); /* host-memory barriers must stay outside a render pass */
                if(!P->large_import_pending){sync_batch(r);apply_writebacks(r);vk_begin(r);}
                if(!large_pinned)large_pinned=gmem_retain_mappings(r->mem);
                if(large_pinned) {
                    if(retain && !P->large_import_cache) {
                        P->large_import_cache=large_reads;P->large_import_draws=0;
                        P->large_import_key_count=key_count;memcpy(P->large_import_keys,keys,key_count*sizeof(*keys));
                        P->large_import_builds++;
                    }
                    vs->large_reads=ps->large_reads=large_reads;
                    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                        .srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT};
                    vkCmdPipelineBarrier(r->cmd,VK_PIPELINE_STAGE_HOST_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                                         0,1,&barrier,0,NULL,0,NULL);
                }
            }
            static int logged;
            static int log_smem=-1;
            if(log_smem<0)log_smem=getenv("PS5C_LOG_SMEM")!=NULL;
            if ((r->cpu_prepare_enabled?log_smem:getenv("PS5C_LOG_SMEM")!=NULL) && logged < 12) {
                logged++;
                for (int s = 0; s < 2; s++) {
                    const sh_eval *e = &st[s].eval;
                    char line[512];
                    int n = snprintf(line, sizeof(line), "%s 0x%llx smem:", s ? "PS" : "VS", (unsigned long long)st[s].prog->base);
                    for (uint32_t i = SH_U_DATA; i < e->smem_words && i < SH_U_DATA + 24 && n < 480; i++) {
                        n += snprintf(line + n, sizeof(line) - (size_t)n, " %08x", e->smem_data[i]);
                    }
                    LOG_I(GPU, "%s", line);
                }
            }
            static frame_trig depth_trig; /* PS5C_DEPTH_STATS_FRAME=<flip>|@<file>: depth after each draw of that frame */
            int64_t depth_frame = frame_trigger(&depth_trig, "PS5C_DEPTH_STATS_FRAME", g->stats.flips);
            if (depth_frame >= 0 && (int64_t)g->stats.flips == depth_frame) {
                static uint32_t n, max;
                static int64_t measured_frame = -1;
                if (measured_frame != depth_frame) { measured_frame = depth_frame; n = 0; }
                if (!max) max = getenv("PS5C_DEPTH_STATS_MAX") ? (uint32_t)atoi(getenv("PS5C_DEPTH_STATS_MAX")) : 60;
                guest_addr dza;
                uint32_t dzw, dzh;
                VkFormat dzf;
                static uint32_t min_width;
                if(!min_width){const char*s=getenv("PS5C_DEPTH_STATS_MIN_WIDTH");min_width=s?(uint32_t)strtoul(s,NULL,10):2048;if(!min_width)min_width=1;}
                static guest_addr wanted_depth;
                if(!n) {
                    const char*s=getenv("PS5C_DEPTH_STATS_ADDRESS");wanted_depth=0;
                    if(s && s[0]=='@') {FILE*f=fopen(s+1,"r");unsigned long long a=0;
                        if(f){if(fscanf(f,"%llx",&a)==1)wanted_depth=a;fclose(f);}}
                    else if(s)wanted_depth=strtoull(s,NULL,16);
                }
                if (n < max && depth_target(g, &dza, &dzw, &dzh, &dzf) && dzw >= min_width &&
                    (!wanted_depth || wanted_depth==dza) &&
                    (dzf == VK_FORMAT_D32_SFLOAT || dzf == VK_FORMAT_D32_SFLOAT_S8_UINT)) {
                    bool drew = draw_with_retry(r, g, d, vs, ps);
                    vk_image *z = vk_image_find(r, dza, dzw, dzh, dzf);
                    if (z && z->width >= min_width) {
                        n++;
                        end_pass(r);
                        sync_batch(r);
                        char what[64];
                        snprintf(what, sizeof(what), "after PS %016llx (%s)", (unsigned long long)ps->prog->hash, drew ? "drawn" : "NOT drawn");
                        float xs, xo, ys, yo;
                        uint32_t v[4] = {gpu_reg(g, mmPA_CL_VPORT_XSCALE), gpu_reg(g, mmPA_CL_VPORT_XOFFSET),
                                         gpu_reg(g, mmPA_CL_VPORT_XSCALE + 2), gpu_reg(g, mmPA_CL_VPORT_XSCALE + 3)};
                        memcpy(&xs, &v[0], 4), memcpy(&xo, &v[1], 4), memcpy(&ys, &v[2], 4), memcpy(&yo, &v[3], 4);
                        LOG_I(GPU, "  VS %016llx, %u vertices, viewport x %g+%g y %g+%g, scissor screen %08x-%08x window %08x-%08x "
                                   "generic %08x-%08x vport %08x-%08x, mode %08x shader %08x zfmt %08x",
                              (unsigned long long)vs->prog->hash, d->count, xs, xo, ys, yo, gpu_reg(g, mmPA_SC_SCREEN_SCISSOR_TL),
                              gpu_reg(g, mmPA_SC_SCREEN_SCISSOR_BR), gpu_reg(g, mmPA_SC_WINDOW_SCISSOR_TL),
                              gpu_reg(g, mmPA_SC_WINDOW_SCISSOR_BR), gpu_reg(g, mmPA_SC_GENERIC_SCISSOR_TL),
                              gpu_reg(g, mmPA_SC_GENERIC_SCISSOR_BR), gpu_reg(g, mmPA_SC_VPORT_SCISSOR_0_TL),
                              gpu_reg(g, mmPA_SC_VPORT_SCISSOR_0_BR), gpu_reg(g, mmPA_SU_SC_MODE_CNTL), gpu_reg(g, mmDB_SHADER_CONTROL),
                              gpu_reg(g, mmSPI_SHADER_Z_FORMAT));
                        uint32_t info0 = gpu_reg(g, mmCB_COLOR0_INFO), a0 = gpu_reg(g, mmCB_COLOR0_ATTRIB2);
                        guest_addr c0 = (guest_addr)gpu_reg(g, mmCB_COLOR0_BASE) << 8;
                        vk_image *rt0 = c0 && cb_format(info0) != VK_FORMAT_UNDEFINED
                                            ? vk_image_find(r, c0, ((a0 >> 14) & 0x3FFF) + 1, (a0 & 0x3FFF) + 1, cb_format(info0))
                                            : NULL;
                        LOG_I(GPU, "  index 0x%llx x%u, %u instances, depth control %08x, RT0 0x%llx covered %lld",
                              (unsigned long long)d->index_addr, d->index_size, d->instances, gpu_reg(g, mmDB_DEPTH_CONTROL),
                              (unsigned long long)c0, rt0 ? (long long)vk_color_coverage(r, rt0) : -1LL);
                        vk_depth_stats(r, z, what);
                        if (getenv("PS5C_DEPTH_STATS_SAVE")) {
                            char path[128];
                            snprintf(path,sizeof(path),"user/frames/depth_f%llu_draw%u_%016llx.bmp",
                                     (unsigned long long)g->stats.flips,n,(unsigned long long)ps->prog->hash);
                            vk_save_depth(r,z,path);
                        }
                        vk_begin(r);
                    }
                    goto drawn;
                }
            }
            t_phase = stat_phase(t_phase); /* large reads */
            draw_with_retry(r, g, d, vs, ps);
        drawn:
            t_phase = stat_phase(t_phase); /* draw_with_retry */
            geometry_output_stats(r,g,vs,ps);
            ngg_position_capture(r,g,d,vs,ps);
            vs_probe_read(r, g, d, vs, ps->prog);
            ps_capture_read(r,g,ps);
            /* Bound pass captures to complex UI draws without dumping every logo quad. */
            static bool save_count_checked;
            static uint32_t save_min_vertices;
            if (!save_count_checked) {
                const char *minimum=getenv("PS5C_SAVE_AFTER_MIN_VERTICES");
                save_min_vertices=minimum?(uint32_t)strtoul(minimum,NULL,10):0;
                save_count_checked=true;
            }
            if (d->count>=save_min_vertices) save_after(r, g, ps->prog, false);
            save_textures(r);
        }
    } else {
        r->pipes->skipped++;
    }
    prof_cpu(r, vs->prog ? vs->prog->base : 0, ps->prog ? ps->prog->base : 1, t_prof);
    sync_check(r, "draw VS/PS hashes", vs->prog ? vs->prog->hash : 0, ps->prog ? ps->prog->hash : 0);
    if(large_reads) {
        bool batch=large_import_batch_enabled() && r->pipes->large_import_cache==large_reads && !large_reads->has_snapshot;
        if(large_reads->count) {
            if(batch)r->pipes->large_import_pending=true;
            else {end_pass(r);sync_batch(r);vk_begin(r);}
        }
        vs->large_reads=ps->large_reads=NULL;
        vk_pipelines *P=r->pipes;
        if(P->large_import_cache==large_reads) {
            if(!large_reads->count || large_reads->has_snapshot ||
               ++P->large_import_draws>=large_import_draw_limit(g->stats.flips))large_import_cache_drop(r);
        } else {
            ngg_large_free(r,large_reads);
            if(large_pinned)gmem_unpin_mappings(r->mem);
        }
    }
    if(st && (st[0].linked_program || st[1].linked_program))vr_flow_invalidate(&r->pipes->vertex_flow);
    if(!reuse_stages)free_draw_stages(st);
    program_storage_free(mesh_program,sparse_programs());program_storage_free(pixel_wave_program,sparse_programs());
    stat_phase(t_phase); /* after the draw */
    stat_add(ST_DRAW, t_draw);
    stat_report(r);
    vk_guest_access_end(r);
    pal_mutex_unlock(r->lock);
}

/* ---- Dispatch ---- */

/* A read-only window may contain separate views and gaps, while each actual
 * descriptor choice was proved wholly readable by the finite-table analysis.
 * Copies must not hide a write through another VA of the same physical pages. */
static bool dynamic_ranges_alias(gmem *mem,guest_addr a,uint64_t an,guest_addr b,uint64_t bn) {
    if(!an || !bn)return false;
    guest_addr ae=a+an,be=b+bn;
    if(a<be && b<ae)return true;
    for(guest_addr ca=a;ca<ae;) {
        gmem_vma va;if(!gmem_query_vma(mem,ca,true,&va) || va.start>=ae)break;
        guest_addr al=ca>va.start?ca:va.start,ah=va.start+va.size;if(ah>ae)ah=ae;
        if(ah<=ca)return true;
        if(!va.reserved && va.kind==GVMA_VIEW)for(guest_addr cb=b;cb<be;) {
            gmem_vma vb;if(!gmem_query_vma(mem,cb,true,&vb) || vb.start>=be)break;
            guest_addr bl=cb>vb.start?cb:vb.start,bh=vb.start+vb.size;if(bh>be)bh=be;
            if(bh<=cb)return true;
            if(!vb.reserved && vb.kind==GVMA_VIEW) {
                uint64_t ap=va.phys+al-va.os_base,bp=vb.phys+bl-vb.os_base;
                if(ap<bp+bh-bl && bp<ap+ah-al)return true;
            }
            cb=bh;
        }
        ca=ah;
    }
    return false;
}

static bool dynamic_copy_read_safe(renderer *r,const sh_program *p,sh_dynamic_plan *plan,
                                   uint32_t site,guest_addr base,uint64_t size) {
    for(uint32_t s=0;s<p->site_count;s++)if(plan->spans[s].store &&
        dynamic_ranges_alias(r->mem,base,size,plan->spans[s].base,plan->spans[s].size)) {
        snprintf(plan->reason,sizeof(plan->reason),"copied input aliases a store (sites %u/%u)",site,s);return false;
    }
    for(uint32_t k=0;k<r->image_count;k++) {
        const vk_image *image=r->images+k;
        uint64_t bytes=(uint64_t)image->width*image->height*(image->layers?image->layers:1)*
                       (image->depth?image->depth:1)*vk_format_bytes(image->format);
        if(image->image && image->gpu_written && dynamic_ranges_alias(r->mem,base,size,image->addr,bytes)) {
            snprintf(plan->reason,sizeof(plan->reason),"copied input aliases a GPU image");return false;
        }
    }
    return true;
}

static bool dynamic_copy_read_window(renderer *r,const sh_program *p,sh_dynamic_plan *plan,
                                      uint32_t site,vk_buffer *out) {
    const sh_dynamic_span *span=plan->spans+site;
    if(span->store || !span->size || span->size>(32u<<20) ||
       !dynamic_copy_read_safe(r,p,plan,site,span->base,span->size))return false;
    uint64_t size=(span->size+3)&~3ull;
    if(!vk_buffer_create(r,size,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,true,out))return false;
    memset(out->mapped,0,(size_t)size);
    guest_addr end=span->base+span->size;
    for(guest_addr cursor=span->base;cursor<end;) {
        gmem_vma v;if(!gmem_query_vma(r->mem,cursor,true,&v) || v.start>=end)break;
        guest_addr low=cursor>v.start?cursor:v.start,high=v.start+v.size;if(high>end)high=end;
        if(high<=cursor)return false;
        if(!v.reserved && (v.prot&GPROT_READ) &&
           !gmem_read(r->mem,low,(uint8_t*)out->mapped+(low-span->base),high-low))return false;
        cursor=high;
    }
    static uint32_t copied;
    if(copied++<12)LOG_I(GPU,"dynamic input site %u: copied %llu KiB window across mappings (read-only)",
                         site,(unsigned long long)(span->size/1024));
    return true;
}

static bool dynamic_copy_read_ranges(renderer *r,const sh_program *p,sh_dynamic_plan *plan,
                                      uint32_t site,vk_buffer *out) {
    const sh_dynamic_span *span=plan->spans+site;
    if(span->store || !span->range_count || !span->packed_size || span->packed_size>(32u<<20))return false;
    for(uint32_t k=0;k<span->range_count;k++)
        if(!dynamic_copy_read_safe(r,p,plan,site,span->ranges[k].base,span->ranges[k].size))return false;
    if(!vk_buffer_create(r,span->packed_size,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,true,out))return false;
    memset(out->mapped,0,span->packed_size);
    for(uint32_t k=0;k<span->range_count;k++) {
        const sh_dynamic_range *range=span->ranges+k;
        if(!gmem_read(r->mem,range->base,(uint8_t*)out->mapped+range->offset,range->size))return false;
    }
    static uint32_t copied;
    if(copied++<12)LOG_I(GPU,"dynamic input site %u: packed %u read-only ranges, %u bytes instead of %llu MiB window",
                         site,span->range_count,span->packed_size,(unsigned long long)(span->size>>20));
    return true;
}

/* Finite resource-table dispatches are isolated: direct writes become visible
 * before releasing the mapping pin, and no cached arena copy survives them. */
static bool dynamic_dispatch(renderer *r,stage_state *cs,uint32_t x,uint32_t y,uint32_t z) {
    if(!r->host_import_align)return false;
    vk_pipelines *P=r->pipes;
    bool executed=false,pinned=false,recorded=false,shapes_saved=false;
    static uint32_t logged,rejected;
    struct {guest_addr addr;uint32_t site,bytes,before[4];} trace[8];
    uint32_t trace_count=0;
    sh_dynamic_plan *plan=calloc(1,sizeof(*plan));
    sh_site_shape *saved=malloc(cs->prog->site_count*sizeof(*saved));
    vk_buffer *buffers=calloc(cs->prog->site_count+1,sizeof(*buffers));
    vk_buffer *read_device=calloc(cs->prog->site_count,sizeof(*read_device));
    uint64_t read_upload_bytes=0;
    static int read_vram=-1;
    if(read_vram<0)read_vram=getenv("PS5C_DYNAMIC_READ_VRAM") && atoi(getenv("PS5C_DYNAMIC_READ_VRAM"));
    bind_list *bl=calloc(1,sizeof(*bl));
    bind_list *images=calloc(1,sizeof(*images));
    stage_state *image_stage=calloc(1,sizeof(*image_stage));
    if(!plan || !saved || !buffers || !read_device || !bl || !images || !image_stage)goto done;
    memcpy(saved,cs->shapes,cs->prog->site_count*sizeof(*saved));
    shapes_saved=true;
    end_pass(r);sync_batch(r);vk_begin(r);
    if(!(pinned=gmem_retain_mappings(r->mem)))goto done;
    if(!sh_dynamic_plan_build(cs->prog,&cs->eval,&cs->info,r->mem,plan))goto done;
    if(logged<16 && getenv("PS5C_DYNAMIC_PLAN_TRACE"))
        for(uint32_t s=0;s<cs->prog->site_count && trace_count<PS5C_ARRAY_COUNT(trace);s++) {
            const sh_dynamic_span *span=plan->spans+s;
            if(!span->store || !span->size)continue;
            uint32_t t=trace_count++,bytes=span->size<16?(uint32_t)span->size:16;
            memset(trace+t,0,sizeof(trace[t]));trace[t].addr=span->base;trace[t].site=s;trace[t].bytes=bytes;
            gmem_read(r->mem,span->base,trace[t].before,bytes);
        }
    memcpy(cs->shapes,plan->shapes,cs->prog->site_count*sizeof(*cs->shapes));
    struct {guest_addr addr;uint64_t hash;sh_site_shape shapes[SH_MAX_SITES];sh_stage_info info;uint32_t wave32;} key;
    memset(&key,0,sizeof(key));key.addr=cs->prog->base;key.hash=cs->prog->hash;key.info=cs->info;key.wave32=cs->prog->wave32;
    memcpy(key.shapes,cs->shapes,cs->prog->site_count*sizeof(*cs->shapes));
    vk_descriptor_plan bindings;
    vk_descriptor_plan_init(&bindings);
    if(!vk_descriptor_plan_stage(&bindings,cs->prog,&cs->eval,cs->shapes,compact_stage(r,cs->prog)))goto done;
    uint64_t descriptor_base_key=hash_with_shapes(&key,sizeof(key),key.shapes,cs->prog->site_count*sizeof(sh_site_shape),sizeof(key.shapes))^1;
    pipe_entry *pipe=find_pipe(r,descriptor_plan_key(descriptor_base_key,&bindings));
    if(!pipe || pipe->failed || (!pipe->pipe && !create_compute(r,pipe,cs,&bindings)))goto done;
    *image_stage=*cs;image_stage->images_only=true;
    for(uint32_t s=0;s<cs->prog->site_count;s++)if(cs->prog->sites[s].kind==SH_SITE_IMAGE)
        image_stage->eval.site[s]=plan->images[s];
    if(!stage_resources(r,image_stage,images,false,NULL,NULL))goto done;
    uint32_t image_binding=0;
    for(uint32_t s=0;s<cs->prog->site_count;s++) {
        const sh_site *site=cs->prog->sites+s;sh_dynamic_span *span=plan->spans+s;
        if(site->kind==SH_SITE_SMEM && !plan->shapes[s].address_map)continue;
        if(site->kind==SH_SITE_IMAGE) {
            uint32_t b=bl->count++,src=image_binding++;
            bl->w[b]=images->w[src];
            bl->img[b]=images->img[src];bl->bound_image[b]=images->bound_image[src];
            plan->uniforms[SH_U_SITE+s]=plan->words;
            memcpy(plan->uniforms+plan->words,plan->images[s].desc,32);plan->words+=8;
            memcpy(plan->uniforms+plan->words,plan->images[s].sampler,16);plan->words+=4;
            continue;
        }
        guest_addr base=span->base;uint64_t size=span->size;
        vk_buffer *buffer=buffers+s+1;
        if(plan->shapes[s].address_map==2) {
            if(!dynamic_copy_read_ranges(r,cs->prog,plan,s,buffer))goto done;
        } else if(span->snapshot || !size) {
            uint64_t bytes=(size+3)&~3ull;if(!bytes)bytes=16;
            if(!vk_buffer_create(r,bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true,buffer))goto done;
            memset(buffer->mapped,0,(size_t)bytes);
            if(size)memcpy(buffer->mapped,span->snapshot,(size_t)size);
        } else {
            uint64_t align=r->host_import_align;
            guest_addr low=base&~(align-1),end=(base+size+align-1)&~(align-1);
            gmem_vma vma;
            if(end<=low || !gmem_query_vma(r->mem,low,false,&vma) || vma.reserved || end>vma.start+vma.size ||
               (vma.prot&(span->store?GPROT_RW:GPROT_READ))!=(span->store?GPROT_RW:GPROT_READ)) {
                if(!dynamic_copy_read_window(r,cs->prog,plan,s,buffer)) {
                    if(!plan->reason[0])snprintf(plan->reason,sizeof(plan->reason),"unsupported import window at site %u",s);
                    goto done;
                }
            } else {
                /* Do not read stale image contents or invalidate an image silently. */
                for(uint32_t k=0;k<r->image_count;k++) {
                    const vk_image *image=r->images+k;
                    uint64_t bytes=(uint64_t)image->width*image->height*(image->layers?image->layers:1)*
                                   (image->depth?image->depth:1)*vk_format_bytes(image->format);
                    if(image->image && image->gpu_written && bytes && low<image->addr+bytes && image->addr<end) {
                        snprintf(plan->reason,sizeof(plan->reason),"import aliases GPU image");goto done;
                    }
                }
                if(!vk_buffer_import_host(r,gmem_host_unlocked(r->mem,low),end-low,buffer))goto done;
                base=low;size=end-low;
            }
        }
        if(plan->shapes[s].address_map) {
            plan->uniforms[SH_U_SITE+s]=plan->words;
            if(plan->shapes[s].address_map==2) {
                for(uint32_t k=0;k<span->range_count;k++) {
                    const sh_dynamic_range *range=span->ranges+k;
                    plan->uniforms[plan->words++]=(uint32_t)range->base;
                    plan->uniforms[plan->words++]=(uint32_t)(range->base>>32);
                    plan->uniforms[plan->words++]=range->size;
                    plan->uniforms[plan->words++]=range->offset;
                }
            } else {
                plan->uniforms[plan->words++]=(uint32_t)base;
                plan->uniforms[plan->words++]=(uint32_t)(base>>32);
            }
        }
        vk_buffer *bound=buffer;
        if(read_vram && r->props.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
           site->kind==SH_SITE_BUFFER && !span->store && !span->snapshot && buffer->mapped && size) {
            if(vk_buffer_create(r,buffer->size,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                false,read_device+s)) {
                bound=read_device+s;read_upload_bytes+=buffer->size;
            } else {
                vk_buffer_destroy(r,read_device+s); /* preserve the already-valid host copy */
                static uint32_t allocation_failures;
                if(allocation_failures++<4)LOG_W(GPU,"dynamic read staging: device allocation failed, using host copy");
            }
        }
        add_buffer(r,bl,0,bound->size);bl->buf[bl->count-1].buffer=bound->buffer;
    }
    /* The stage's initial user data and partition origin are immutable too. */
    uint32_t reg=cs->info.cs_tgid_sgpr;
    for(uint32_t k=0;k<3;k++)if((cs->info.cs_tgid_enable>>k)&1) {
        if(reg<SH_SGPRS)plan->uniforms[SH_U_SGPR+reg++]=cs->dispatch_base[k];
    }
    if(!sh_uniform_shift_valid(cs->prog->site_count,cs->info.uniform_shift) || plan->words<SH_U_DATA)goto done;
    uint32_t packed_words=plan->words-cs->info.uniform_shift;
    if((uint64_t)packed_words*4>r->props.limits.maxStorageBufferRange)goto done;
    if(!vk_buffer_create(r,(uint64_t)packed_words*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true,buffers))goto done;
    if(!sh_uniform_pack(buffers[0].mapped,packed_words,plan->uniforms,plan->words,
                        cs->prog->site_count,cs->info.uniform_shift))goto done;
    if(g_stats_on>0) {
        g_stat_uniform_count++;g_stat_uniform_logical+=(uint64_t)plan->words*4;
        g_stat_uniform_uploaded+=(uint64_t)packed_words*4;
    }
    /* Resource bindings were assembled without the uniforms at binding zero. */
    for(uint32_t b=bl->count;b>0;b--) {
        bl->buf[b]=bl->buf[b-1];bl->img[b]=bl->img[b-1];bl->w[b]=bl->w[b-1];
        bl->bound_image[b]=bl->bound_image[b-1];
    }
    bl->buf[0]=(VkDescriptorBufferInfo){buffers[0].buffer,0,buffers[0].size};
    bl->bound_image[0]=NULL;
    bl->w[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;bl->count++;
    bl->sampled_count=images->sampled_count;bl->written_count=images->written_count;
    memcpy(bl->sampled,images->sampled,images->sampled_count*sizeof(*bl->sampled));
    memcpy(bl->written,images->written,images->written_count*sizeof(*bl->written));
    for(uint32_t b=0;b<bl->count;b++) {
        VkDescriptorType type=bl->w[b].descriptorType;
        bl->w[b]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstBinding=b,
            .descriptorCount=1,.descriptorType=type,.pBufferInfo=type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER?bl->buf+b:NULL,
            .pImageInfo=type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER?NULL:bl->img+b};
    }
    shared_image_layouts(bl);
    if(!descriptor_plan_confirm(&bindings,bl)) {
        if(bindings.count!=bl->count)goto done;
        pipe=find_pipe(r,descriptor_base_key);
        if(!pipe || pipe->failed || (!pipe->pipe && !create_compute(r,pipe,cs,&bindings)))goto done;
    }
    VkDescriptorSet set=pipe->push?VK_NULL_HANDLE:alloc_set(r,pipe->dsl,pipe->update_after_bind);if(!set && !pipe->push)goto done;
    if(set)update_descriptor_set(r,set,bl,&bindings);
    end_pass(r);
    for(uint32_t i=0;i<images->sampled_count;i++)
        vk_barrier(r->cmd,images->sampled[i],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    int prof_i=prof_start(r); /* include optional upload as well as the finite-table compute */
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(r->cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,1,&barrier,0,NULL,0,NULL);
    if(read_upload_bytes) {
        /* Record only after all preparation succeeds: a rejected plan must
         * not leave commands referencing destroyed temporary allocations. */
        for(uint32_t s=0;s<cs->prog->site_count;s++)if(read_device[s].buffer) {
            VkBufferCopy copy={.size=read_device[s].size};
            vkCmdCopyBuffer(r->cmd,buffers[s+1].buffer,read_device[s].buffer,1,&copy);
        }
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(r->cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0,1,&barrier,0,NULL,0,NULL);
        static uint32_t uploads;
        if(uploads++<12)LOG_I(GPU,"dynamic read staging: %llu bytes uploaded to device-local buffers",
                             (unsigned long long)read_upload_bytes);
    }
    vkCmdBindPipeline(r->cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipe->pipe);
    bind_set(r,r->cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipe,set,bl,&bindings);
    vkCmdDispatch(r->cmd,x,y,z);recorded=true;
    prof_stop(r,prof_i,cs->prog->base,0,(uint64_t)x*y*z);
    barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT|VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(r->cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0,1,&barrier,0,NULL,0,NULL);
    vk_flush(r);
    VkResult result=vk_work_wait(r,UINT64_MAX);
    vk_check_work(r, result, "vkWaitForFences (dynamic dispatch)");
    executed=true;P->dispatches++;
    for(uint32_t t=0;t<trace_count;t++) {
        uint32_t after[4]={0};
        if(!gmem_read(r->mem,trace[t].addr,after,trace[t].bytes))continue;
        LOG_I(GPU,"dynamic store CS 0x%llx site %u at 0x%llx: %08x %08x %08x %08x -> %08x %08x %08x %08x",
              (unsigned long long)cs->prog->base,trace[t].site,(unsigned long long)trace[t].addr,
              trace[t].before[0],trace[t].before[1],trace[t].before[2],trace[t].before[3],after[0],after[1],after[2],after[3]);
    }
done:
    if(plan) {
        if(executed?logged++<16:rejected++<16)LOG_I(GPU,"dynamic dispatch CS 0x%llx: %s, %u choices %s",(unsigned long long)cs->prog->base,
                            executed?"completed":"rejected",plan->choices,plan->reason);
    }
    if(buffers)for(uint32_t s=0;s<cs->prog->site_count+1;s++)vk_buffer_destroy(r,buffers+s);
    if(read_device)for(uint32_t s=0;s<cs->prog->site_count;s++)vk_buffer_destroy(r,read_device+s);
    if(pinned)gmem_unpin_mappings(r->mem);
    if(shapes_saved)memcpy(cs->shapes,saved,cs->prog->site_count*sizeof(*saved));
    if(plan)sh_dynamic_plan_free(plan);
    free(plan);free(saved);free(buffers);free(read_device);free(bl);free(images);free(image_stage);
    if(recorded || pinned)vk_begin(r); /* completed fence; forget copies/evaluations of directly written bytes */
    return executed;
}

static bool bvh_address_dispatch(renderer *r,stage_state *cs,uint32_t x,uint32_t y,uint32_t z);
/* BVH replay re-enters dispatch. The capacity-sized descriptor list is
 * over 1 MiB; two native stack frames exceed a Windows thread's reserve.
 * Each call owns its workspace until vkUpdateDescriptorSets has copied it;
 * recorded commands retain arena buffers/images, never these host pointers. */
static bool do_dispatch_impl(renderer *r,stage_state *cs,uint32_t x,uint32_t y,uint32_t z,bind_list *bl);
static bool do_dispatch(renderer *r, stage_state *cs, uint32_t x, uint32_t y, uint32_t z) {
    vk_pipelines *P=r->pipes;
    bool outer_oom=P->writeback_oom;P->writeback_oom=false;
    uint64_t dispatches_before=P->dispatches;
    uint32_t slot=P->cs_bl_depth++;
    bind_list *bl=NULL;
    if(slot<PS5C_ARRAY_COUNT(P->cs_bl)) {
        if(!P->cs_bl[slot])P->cs_bl[slot]=malloc(sizeof(*bl));
        bl=P->cs_bl[slot];
    } else {
        bl=malloc(sizeof(*bl));
    }
    if(!bl){P->cs_bl_depth--;P->writeback_oom=outer_oom;LOG_E(GPU,"cannot allocate compute descriptor workspace");P->skipped++;return true;}
    uint64_t t_phase = stat_now();
    bool ok=do_dispatch_impl(r,cs,x,y,z,bl);
    if(!ok && P->writeback_oom) {
        if(P->dispatches!=dispatches_before)writeback_snapshot_failure("after partial compute recording");
        /* BVH replays re-enter this wrapper. Do not unwind an allocation
         * failure into their handled/skipped path or submit speculative data. */
        if(P->bvh_table || P->bvh_replay)writeback_snapshot_failure("during BVH preparation");
        end_pass(r);sync_batch(r);vk_begin(r);
        P->writeback_oom=false;
        ok=do_dispatch_impl(r,cs,x,y,z,bl);
        if(!ok || P->dispatches==dispatches_before)writeback_snapshot_failure("after compute retry");
    }
    P->writeback_oom=outer_oom;
    stat_phase(t_phase); /* do_dispatch */
    P->cs_bl_depth--;
    if(slot>=PS5C_ARRAY_COUNT(P->cs_bl))free(bl);
    return ok;
}
static bool do_dispatch_impl(renderer *r,stage_state *cs,uint32_t x,uint32_t y,uint32_t z,bind_list *bl) {
    uint64_t resource_batch = r->use_seq;
    vk_pipelines *P = r->pipes;
    if(r->bvh_address && !P->bvh_table) {
        bool has_bvh=false;
        for(uint32_t s=0;s<cs->prog->site_count;s++)has_bvh |= cs->prog->sites[s].kind==SH_SITE_BVH;
        if(has_bvh) {
            bool done=bvh_address_dispatch(r,cs,x,y,z);
            if(!done)P->skipped++;
            return true; /* failure must not retry an unbounded static BVH span */
        }
    }
    if(!P->bvh_table && r->host_import_align && cs->prog->back_branch) {
        bool unknown=false;
        for(uint32_t s=0;s<cs->prog->site_count;s++)unknown|=cs->prog->sites[s].kind==SH_SITE_BUFFER && cs->eval.site[s].desc_unknown;
        if(unknown && dynamic_dispatch(r,cs,x,y,z))return true;
    }
    { /* PS5C_LOG_CS=<program hash>[,...]: each dispatch of those, with its frame and resources (debugging aid) */
        static int have = -1;
        static uint64_t want[8];
        static uint32_t nwant, logged;
        if (have < 0) {
            const char *e = getenv("PS5C_LOG_CS");
            have = e != NULL;
            for (const char *q = e; q && *q && nwant < 8; q = strchr(q, ',') ? strchr(q, ',') + 1 : "") want[nwant++] = strtoull(q, NULL, 16);
        }
        bool match = false;
        for (uint32_t k = 0; have && k < nwant; k++) match |= cs->prog->hash == want[k];
        if (match && logged++ < 400) {
            char res[200];
            int n = 0;
            res[0] = 0;
            for (uint32_t k = 0; k < cs->prog->site_count && n < (int)sizeof(res) - 24; k++) {
                const sh_site_value *v = &cs->eval.site[k];
                if (cs->prog->sites[k].kind == SH_SITE_SMEM) continue;
                guest_addr a = cs->prog->sites[k].kind == SH_SITE_IMAGE
                                   ? ((guest_addr)v->desc[0] << 8) | ((guest_addr)(v->desc[1] & 0xFF) << 40)
                                   : (guest_addr)v->desc[0] | ((guest_addr)(v->desc[1] & 0xFFFF) << 32);
                bool seen = false; /* each resource once */
                for (uint32_t j = 0; j < k && !seen; j++) {
                    const sh_site_value *u = &cs->eval.site[j];
                    seen = cs->prog->sites[j].kind == cs->prog->sites[k].kind && u->desc[0] == v->desc[0] && u->desc[1] == v->desc[1];
                }
                if (!seen) n += snprintf(res + n, sizeof(res) - (size_t)n, " %s%llx", cs->prog->sites[k].store ? "w:" : "", (unsigned long long)a);
            }
            LOG_I(GPU, "CS %016llx flip %llu groups %u,%u,%u:%s", (unsigned long long)cs->prog->hash,
                  (unsigned long long)g_trace_flip, x, y, z, res);
        }
    }
    /* A loop over a buffer whose V# came from vector data: the evaluation has no
     * buffer for it, the loads read zeros, and a shader spinning on a flag there
     * (decoupled look-back scans) never ends: the GPU hangs. Skipped instead
     * (Little Nightmares' scan; such shaders failed to translate before). */
    static int run_unknown = -1; /* read once: getenv takes the C runtime's environment lock (96e44a5) */
    if (run_unknown < 0) run_unknown = getenv("PS5C_RUN_UNKNOWN_LOOPS") != NULL;
    for (uint32_t i = 0; cs->prog->back_branch && !run_unknown && i < cs->prog->site_count; i++) {
        const sh_site_value *v = &cs->eval.site[i];
        if (cs->prog->sites[i].kind != SH_SITE_BUFFER || !v->valid || !v->desc_unknown) continue;
        static uint32_t logged;
        if (logged++ < 8) {
            uint32_t at = cs->prog->sites[i].inst;
            const rdna2_inst *in = &cs->prog->insts[at];
            LOG_W(GPU, "dispatch CS 0x%llx skipped: loop uses buffer site %u at +0x%x (%s, s[%u:%u]); "
                       "descriptor depends on vector data (snapshot %08x %08x %08x %08x)",
                  (unsigned long long)cs->prog->base, i, cs->prog->pc[at] * 4,
                  in->name, in->srsrc, in->srsrc + 3,
                  v->desc[0], v->desc[1], v->desc[2], v->desc[3]);
            if(getenv("PS5C_DYNAMIC_PLAN_TRACE") && gmem_retain_mappings(r->mem)) {
                sh_dynamic_plan *plan=calloc(1,sizeof(*plan));
                if(plan) {
                    bool ready=sh_dynamic_plan_build(cs->prog,&cs->eval,&cs->info,r->mem,plan);
                    LOG_I(GPU,"dynamic plan CS 0x%llx: %s (%u choices) %s",(unsigned long long)cs->prog->base,
                          ready?"validated":"rejected",plan->choices,plan->reason);
                    sh_dynamic_plan_free(plan);free(plan);
                }
                gmem_unpin_mappings(r->mem);
            }
            static int trace_unknown = -1;
            if (trace_unknown < 0) trace_unknown = getenv("PS5C_TRACE_UNKNOWN_BUFFERS") != NULL;
            if(trace_unknown) {
                corpus_add_code(cs->prog->base,cs->prog->code,cs->prog->dwords);
                char path[512];snprintf(path,sizeof(path),"%s/unknown-%llx-%u.eval",
                    vk_shader_cache_game_dir(),(unsigned long long)cs->prog->base,logged);
                FILE *dump=fopen(path,"wb");
                if(dump) {
                    uint32_t head[6]={0x31425544,cs->prog->dwords,cs->eval.smem_words,cs->prog->site_count,
                        (uint32_t)sizeof(sh_site_value),(uint32_t)sizeof(sh_stage_info)};
                    bool ok=fwrite(head,sizeof(head),1,dump)==1 && fwrite(&cs->prog->base,8,1,dump)==1 &&
                        fwrite(&cs->info,sizeof(cs->info),1,dump)==1 &&
                        fwrite(cs->eval.sgpr_init,sizeof(cs->eval.sgpr_init),1,dump)==1 &&
                        fwrite(cs->eval.smem_data,4,cs->eval.smem_words,dump)==cs->eval.smem_words &&
                        fwrite(cs->eval.site,sizeof(sh_site_value),cs->prog->site_count,dump)==cs->prog->site_count &&
                        fwrite(cs->prog->code,4,cs->prog->dwords,dump)==cs->prog->dwords;
                    if(fclose(dump) || !ok)LOG_W(GPU,"unknown-buffer diagnostic incomplete: %s",path);
                }
                /* Complete, bounded source tables: one descriptor snapshot cannot
                 * show the resources chosen by other lanes/objects. Diagnostic
                 * only; these bytes never authorize an unknown GPU binding. */
                snprintf(path,sizeof(path),"%s/unknown-%llx-%u.tables",
                         vk_shader_cache_game_dir(),(unsigned long long)cs->prog->base,logged);
                dump=fopen(path,"wb");
                if(dump) {
                    guest_addr seen[16];uint32_t count=0;
                    for(uint32_t j=0;j<cs->prog->site_count && count<PS5C_ARRAY_COUNT(seen);j++) {
                        const sh_site_value *source=&cs->eval.site[j];
                        if(cs->prog->sites[j].kind!=SH_SITE_SBUFFER || !source->valid || source->desc_unknown)continue;
                        guest_addr base=(guest_addr)source->desc[0] | ((guest_addr)(source->desc[1]&0xffff)<<32);
                        uint32_t stride=(source->desc[1]>>16)&0x3fff;
                        uint64_t size=(uint64_t)source->desc[2]*(stride?stride:1);
                        bool duplicate=false;for(uint32_t k=0;k<count;k++)duplicate|=seen[k]==base;
                        if(duplicate || !base || !size || size>65536)continue;
                        uint8_t *bytes=malloc((size_t)size);
                        if(!bytes)break;
                        if(gmem_read(r->mem,base,bytes,size)) {
                            uint32_t head[2]={j,(uint32_t)size};
                            bool ok=fwrite(&base,8,1,dump)==1 && fwrite(head,sizeof(head),1,dump)==1 &&
                                    fwrite(bytes,1,(size_t)size,dump)==size;
                            seen[count++]=base;
                            if(!ok) {free(bytes);break;}
                        }
                        free(bytes);
                    }
                    fclose(dump);
                }
                snprintf(path,sizeof(path),"%s/unknown-%llx-%u.maps",
                         vk_shader_cache_game_dir(),(unsigned long long)cs->prog->base,logged);
                dump=fopen(path,"w");
                if(dump) {
                    guest_addr cursor=0;gmem_vma vma;
                    for(uint32_t j=0;j<4096 && gmem_query_vma(r->mem,cursor,true,&vma);j++) {
                        fprintf(dump,"%llx %llx %x %u %u %llx %llx %llx %s\n",
                            (unsigned long long)vma.start,(unsigned long long)vma.size,vma.prot,
                            (unsigned)vma.reserved,(unsigned)vma.kind,(unsigned long long)vma.os_base,
                            (unsigned long long)vma.os_size,(unsigned long long)vma.phys,vma.name);
                        guest_addr end=vma.start+vma.size;
                        if(end<=cursor)break;
                        cursor=end;
                    }
                    fclose(dump);
                }
            }
            for (uint32_t j = 0; trace_unknown && j < cs->prog->site_count; j++) {
                if (cs->prog->sites[j].kind != SH_SITE_SBUFFER) continue;
                const sh_site_value *source = &cs->eval.site[j];
                uint32_t si = cs->prog->sites[j].inst;
                LOG_I(GPU, "unknown-buffer source CS 0x%llx site %u +0x%x s_buffer snapshot %08x %08x %08x %08x",
                      (unsigned long long)cs->prog->base, j, cs->prog->pc[si] * 4,
                      source->desc[0], source->desc[1], source->desc[2], source->desc[3]);
            }
        }
        P->skipped++;
        return true;
    }
    struct {
        guest_addr addr;
        uint64_t hash;
        sh_site_shape shapes[SH_MAX_SITES];
        sh_stage_info info;
        uint32_t wave32; /* the same code can be dispatched in either guest mode */
    } k;
    memset(&k, 0, sizeof(k));
    k.addr = cs->prog->base;
    k.hash = cs->prog->hash;
    k.wave32 = cs->prog->wave32;
    memcpy(k.shapes, cs->shapes, sizeof(sh_site_shape) * cs->prog->site_count);
    k.info = cs->info;
    uint64_t t_phase = stat_now();
    vk_descriptor_plan bindings;
    vk_descriptor_plan_init(&bindings);
    if (!vk_descriptor_plan_stage(&bindings, cs->prog, &cs->eval, cs->shapes, compact_stage(r, cs->prog))) return true;
    uint64_t descriptor_base_key = hash_with_shapes(&k, sizeof(k), k.shapes, sizeof(sh_site_shape) * cs->prog->site_count,
                                                   sizeof(k.shapes)) ^ 1;
    pipe_entry *e = find_pipe(r, descriptor_plan_key(descriptor_base_key, &bindings));
    if (!e || e->failed) return true;
    if (!e->pipe && !create_compute(r, e, cs, &bindings)) {
        e->failed = !e->pending;
        return true;
    }
    t_phase = stat_phase(t_phase); /* compute pipeline */
    bl->count = 0;
    bl->sampled_count = 0;
    bl->written_count = 0;
    bl->missing = false;
    bool stored = false, overflow = false;
    uint32_t wb_before = P->wb_count;
    bool resources_ok = stage_resources(r, cs, bl, true, &stored, &overflow);
    if(cs->footprint_count && !footprint_sources_current(r,cs)) {
        /* A mutable source changed while staging. Discard this preparation:
         * mode 1 may already have allocated a reduced backing. The retry sees
         * the stale hash and uses complete bindings, never a clipped buffer
         * with its proof removed. */
        for(uint32_t i=0;i<P->wb_count;i++)P->wb[i].scope_mode=0;
        cs->footprint_count=0;
        resources_ok=false;
    }
    t_phase = stat_phase(t_phase); /* compute resources */
    if (!resources_ok) {
        drop_writebacks(P, r->use_seq == resource_batch ? wb_before : 0);
        if (bl->missing) P->skipped++;
        return bl->missing;
    }
    if (r->use_seq != resource_batch) {
        drop_writebacks(P, 0); /* old writebacks were applied by vk_work_begin */
        return false;
    }
    shared_image_layouts(bl);
    if (!descriptor_plan_confirm(&bindings, bl)) {
        if (bindings.count != bl->count) { drop_writebacks(P, wb_before); return true; }
        e = find_pipe(r, descriptor_base_key);
        if (!e || e->failed || (!e->pipe && !create_compute(r, e, cs, &bindings))) {
            if (e) e->failed = !e->pending;
            drop_writebacks(P, wb_before);
            return true;
        }
    }
    if(P->bvh_replay && (overflow || !bvh_save_targets(r,bl))) {
        drop_writebacks(P,wb_before);return true;
    }
    uint32_t depth_fill = htile_fill_candidate(P, cs, wb_before);
    if(!P->bvh_replay)for (uint32_t i = wb_before; i < P->wb_count; i++)
        htile_written(P, P->wb[i].addr, P->wb[i].size, "compute", cs->prog->hash, depth_fill);
    linear_image_plan linear_copies;
    if(!prepare_linear_image_copies(r,cs,bl,&linear_copies)) {
        drop_writebacks(P,wb_before);return false;
    }
    VkDescriptorSet set = e->push ? VK_NULL_HANDLE : alloc_set(r, e->dsl, e->update_after_bind);
    if (!set && !e->push) return false;
    if (set) update_descriptor_set(r, set, bl, &bindings);
    t_phase = stat_phase(t_phase); /* compute descriptors */
    end_pass(r);
    for (uint32_t i = 0; i < bl->sampled_count; i++) {
        if (image_written(bl, bl->sampled[i])) continue;
        vk_barrier(r->cmd, bl->sampled[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    for (uint32_t i = 0; i < bl->written_count; i++) {
        vk_barrier(r->cmd, bl->written[i], VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        bl->written[i]->gpu_written = true;
        texture_gpu_write(r,bl->written[i]);
        bl->written[i]->written_seq = ++r->write_seq;
    }
    vkCmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, e->pipe);
    bind_set(r, r->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, e, set, bl, &bindings);
    int prof_i = prof_start(r);
    /* Each part is within the device limits. Guest origins are in this part's
     * immutable uniforms: vkCmdDispatchBase also limits base + count. */
    vk_diagnostic_checkpoint(r, 2, cs->prog->base, 0, cs->prog->hash, 0);
    vkCmdDispatch(r->cmd, x, y, z);
    vk_diagnostic_checkpoint(r, 3, cs->prog->base, 0, cs->prog->hash, 0);
    prof_stop(r, prof_i, cs->prog->base, 0, (uint64_t)x * y * z);
    P->dispatches++;
    t_phase = stat_phase(t_phase); /* barriers + dispatch */
    record_linear_image_copies(r,&linear_copies);
    if (stored) {
        /* later draws and dispatches of the batch read the results in place */
        VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDEX_READ_BIT |
                           VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    if (overflow) {
        /* results that are not tracked must reach guest memory now */
        sync_batch(r);
        vk_begin(r);
    }
    /* Debugging aid, like PS5C_SAVE_AFTER for graphics: inspect the actual
     * outputs before a later pass changes them. Deliberately synchronizes,
     * so runs with this enabled are not performance measurements. */
    static int save_compute = -1;
    static uint64_t save_hash[8], save_from;
    static uint32_t saved_compute, save_count, save_max;
    if (save_compute < 0) { /* PS5C_SAVE_COMPUTE=<hash>[,<hash>...], PS5C_SAVE_COMPUTE_FROM=<flip>|@<file>,
                             * PS5C_SAVE_COMPUTE_MAX=<dispatches> (4) */
        const char *env = getenv("PS5C_SAVE_COMPUTE"), *f = getenv("PS5C_SAVE_COMPUTE_FROM"), *m = getenv("PS5C_SAVE_COMPUTE_MAX");
        save_compute = env != NULL;
        for (const char *q = env; q && *q && save_count < 8; q = strchr(q, ',') ? strchr(q, ',') + 1 : "")
            save_hash[save_count++] = strtoull(q, NULL, 16);
        save_from = f && f[0] != '@' ? strtoull(f, NULL, 0) : 0;
        save_max = m ? (uint32_t)atoi(m) : 4;
    }
    bool save_match = false;
    for (uint32_t h = 0; save_compute && h < save_count && !save_match; h++)
        save_match = cs->prog->hash == save_hash[h] || cs->prog->base == save_hash[h] ||
                     (cs->prog->dwords >= 8 && fnv(cs->prog->code, 32, FNV_INIT) == save_hash[h]);
    static frame_trig save_trig;
    int64_t save_frame = save_match ? frame_trigger(&save_trig, "PS5C_SAVE_COMPUTE_FROM", g_trace_flip) : -1;
    bool save_file = save_match && getenv("PS5C_SAVE_COMPUTE_FROM") && getenv("PS5C_SAVE_COMPUTE_FROM")[0] == '@';
    static uint64_t save_every = 0, save_last = UINT64_MAX; /* PS5C_SAVE_COMPUTE_EVERY=<flips>: one dispatch per n frames */
    if (!save_every) save_every = getenv("PS5C_SAVE_COMPUTE_EVERY") ? strtoull(getenv("PS5C_SAVE_COMPUTE_EVERY"), NULL, 10) : 1;
    bool save_due = save_every <= 1 || save_last == UINT64_MAX || g_trace_flip >= save_last + save_every;
    if (save_match && save_due && saved_compute < save_max && g_trace_flip >= save_from &&
        (!save_file || (save_frame >= 0 && g_trace_flip >= (uint64_t)save_frame))) {
        save_last = g_trace_flip;
        sync_batch(r);
        for (uint32_t i = 0; i < bl->written_count; i++) {
            bool duplicate = false;
            for (uint32_t j = 0; j < i; j++) duplicate |= bl->written[j] == bl->written[i];
            if (duplicate) continue;
            char path[160];
            snprintf(path, sizeof(path), "user/frames/compute_%016llx_%llu_%u_%llx.bmp",
                     (unsigned long long)cs->prog->hash, (unsigned long long)g_trace_flip,
                     saved_compute, (unsigned long long)bl->written[i]->addr);
            vk_save_image(r, bl->written[i], path);
        }
        for (uint32_t i = 0; getenv("PS5C_SAVE_COMPUTE_READS") && i < bl->sampled_count; i++) { /* its inputs too */
            bool duplicate = false;
            for (uint32_t j = 0; j < i; j++) duplicate |= bl->sampled[j] == bl->sampled[i];
            if (duplicate) continue;
            char path[160];
            snprintf(path, sizeof(path), "user/frames/compute_%016llx_%llu_%u_read_%llx_%ux%u_l%u.bmp",
                     (unsigned long long)cs->prog->hash, (unsigned long long)g_trace_flip, saved_compute,
                     (unsigned long long)bl->sampled[i]->addr, bl->sampled[i]->width, bl->sampled[i]->height,
                     bl->sampled[i]->levels);
            vk_save_image(r, bl->sampled[i], path);
        }
        saved_compute++;
        vk_begin(r);
    }
    return true;
}

/* A buffer fill: element tgid.x * 64 + tid.x of a structured buffer gets four user
 * SGPRs, and nothing else happens. Sonic Frontiers clears 16-32 MB buffers this way
 * many times a frame: on the GPU each one copied the whole buffer in and back out
 * (~640 MB/s). Done here, into guest memory, with what the GPU would have stored:
 * 32-bit components, which the format stores unconverted. PS5C_NO_CPU_FILL=1: the GPU. */
/* The bounded dword-pattern kernel reads count/mask from a parameter buffer,
 * then copies pattern[index & mask] into destination[index]. Mask zero, or a
 * small pattern whose selected words are identical, is a uniform fill.
 * Other kernels/patterns retain the normal GPU path.
 * Matching instructions and descriptor semantics (never a game/shader ID)
 * lets the existing fill publish both guest bytes and cached color images. */
#include "gpu/vk/vk_bvh_address.h"

static bool cpu_buffer_pattern(renderer *r, const gpu *g, const stage_state *cs, uint32_t x,
                               guest_addr *base, uint64_t *size, uint32_t data[4]) {
    static const uint32_t kernel[]={0xbfa00001,0xd7460000,0x04010c0c,0xf4201a84,0xfa000000,
        0xbf8cc07f,0x7da8006a,0xbf880009,0xf4201a84,0xfa000004,0xbf8cc07f,0x3602006a,
        0xe0002000,0x80000101,0xbf8c3f70,0xe0102000,0x80010100,0xbf810000};
    const sh_program *p=cs->prog;
    if(p->count!=13 || p->dwords<PS5C_ARRAY_COUNT(kernel) || cs->info.cs_tgid_sgpr!=12 ||
       cs->info.cs_tgid_enable!=1 || cs->groups[1]!=1 || cs->groups[2]!=1)
        return false;
    /* Both parameter layouts occur: {count, mask} and {mask, count}.
     * Only the two scalar load offsets may differ. */
    bool mask_first=p->code[4]==kernel[9] && p->code[9]==kernel[4];
    for(uint32_t i=0;i<PS5C_ARRAY_COUNT(kernel);i++) {
        uint32_t expected=mask_first && i==4?kernel[9]:mask_first && i==9?kernel[4]:kernel[i];
        if(p->code[i]!=expected)return false;
    }
    uint32_t d[12];for(uint32_t i=0;i<12;i++)d[i]=gpu_reg(g,mmCOMPUTE_USER_DATA_0+i);
    if((d[1]>>16)!=4 || (d[5]>>16)!=4 || (d[9]>>16)!=16 ||
       (d[3]&~0x200u)!=0x14004 || (d[7]&~0x200u)!=0x14004 ||
       d[11]!=0x4dfac || !d[2] || !d[10])return false;
    guest_addr pattern=(guest_addr)d[0]|((guest_addr)(d[1]&0xffff)<<32);
    guest_addr params=(guest_addr)d[8]|((guest_addr)(d[9]&0xffff)<<32);
    *base=(guest_addr)d[4]|((guest_addr)(d[5]&0xffff)<<32);
    /* Guest reads cannot stand in for an unpublished image write. */
    for(uint32_t i=0;i<r->image_count;i++) {
        const vk_image *im=&r->images[i];
        if(!im->image || !im->gpu_written)continue;
        uint64_t bytes=(uint64_t)im->width*im->height*vk_format_bytes(im->format)*
                       (im->layers?im->layers:1)*(im->depth?im->depth:1);
        if((pattern<im->addr+bytes && im->addr<pattern+16) ||
           (params<im->addr+bytes && im->addr<params+8))return false;
    }
    /* Pending GPU results must reach the inputs before the CPU reads them. */
    vk_pipelines *P=r->pipes;bool pending=false;
    for(uint32_t i=0;i<P->wb_count && !pending;i++) {
        const writeback *w=&P->wb[i];
        pending=(pattern<w->addr+w->size && w->addr<pattern+16) ||
                (params<w->addr+w->size && w->addr<params+8);
    }
    if(pending) {end_pass(r);sync_batch(r);apply_writebacks(r);vk_begin(r);}
    uint32_t limits[2],words[4];
    if(!gmem_read(r->mem,params,limits,sizeof(limits)))return false;
    uint32_t mask=limits[mask_first?0:1];
    /* Small uniform patterns are still a constant fill. Check every index
     * selected by the mask, within the source descriptor. Nonuniform patterns
     * and larger masks retain the original GPU path. */
    if(mask>3 || d[2]<=mask || !gmem_read(r->mem,pattern,words,(mask+1)*4))return false;
    for(uint32_t i=1;i<=mask;i++)if(!(i&~mask) && words[i]!=words[0])return false;
    uint64_t n=(uint64_t)x*64;
    uint32_t count=limits[mask_first?1:0];
    if(n>count)n=count;
    if(n>d[6])n=d[6];
    *size=n*4;
    if((pattern<*base+*size && *base<pattern+(mask+1)*4) ||
       (params<*base+*size && *base<params+8))return false;
    for(uint32_t i=0;i<4;i++)data[i]=words[0];
    return true;
}

/* in[1..2] = s_buffer_load_dword sN, s[a:a+3], 0x0; s_waitcnt: the dword it loads.
 * A pending buffer result must land first. Staying on this proven fill path
 * also updates an already-cached destination image, unlike a buffer-only
 * fallback. Current guest values do not need a submission or a wait. */
static bool cpu_fill_loaded_value(renderer *r, const gpu *g, const rdna2_inst *in, uint32_t user, uint32_t *value) {
    const rdna2_inst *ld = &in[1];
    if (ld->enc != RDNA2_ENC_SMEM || strcmp(ld->name, "s_buffer_load_dword") || ld->offset ||
        ld->soffset != RDNA2_OP_NULL || ld->sbase + 3u >= user || ld->dst < user || ld->dst >= RDNA2_OP_M0 ||
        strcmp(in[2].name, "s_waitcnt"))
        return false;
    uint32_t d[4];
    for (int k = 0; k < 4; k++) d[k] = gpu_reg(g, mmCOMPUTE_USER_DATA_0 + ld->sbase + (uint32_t)k);
    guest_addr src = (guest_addr)d[0] | ((guest_addr)(d[1] & 0xFFFF) << 32);
    uint32_t stride = (d[1] >> 16) & 0x3FFF; /* the bound the scalar evaluation uses too */
    if (!src || (stride ? (uint64_t)d[2] * stride : d[2]) < 4) return false;
    vk_pipelines *P = r->pipes;
    bool pending = eval_read_pending(P, src, 4);
    for (uint32_t i = 0; i < r->image_count; i++) {
        const vk_image *im = &r->images[i];
        if (!im->image || !im->gpu_written) continue;
        uint64_t bytes = (uint64_t)im->width * im->height * vk_format_bytes(im->format) *
                         (im->layers ? im->layers : 1) * (im->depth ? im->depth : 1);
        if (src < im->addr + bytes && im->addr < src + 4) return false;
    }
    if (pending) {
        end_pass(r);
        sync_batch(r);
        apply_writebacks(r);
        vk_begin(r);
    }
    return gmem_read(r->mem, src, value, 4);
}

/* Large fills (Sonic Origins clears ~170 MB of 4K targets every frame) with non-temporal
 * stores: they skip reading each line into the cache first, ~2.5x the speed of memset
 * here. SSE2, so every x86-64 CPU has them. PS5C_FILL_STREAM=0: memset/memcpy as before. */
static bool fill_streaming_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("PS5C_FILL_STREAM"); on = !(e && !strcmp(e, "0")); }
    return on;
}

static void fill_streaming(uint8_t *dst, uint64_t size, const uint32_t data[4]) {
    const uint8_t *pat = (const uint8_t *)data; /* byte k of the fill is pat[k % 16] */
    uint64_t head = (16u - ((uintptr_t)dst & 15u)) & 15u;
    for (uint64_t k = 0; k < head; k++) dst[k] = pat[k & 15];
    uint8_t rot[16];
    for (uint32_t j = 0; j < 16; j++) rot[j] = pat[(head + j) & 15];
    __m128i v = _mm_loadu_si128((const __m128i *)rot);
    uint8_t *p = dst + head;
    uint64_t body = (size - head) & ~(uint64_t)63;
    for (uint64_t i = 0; i < body; i += 64) {
        _mm_stream_si128((__m128i *)(p + i), v);
        _mm_stream_si128((__m128i *)(p + i + 16), v);
        _mm_stream_si128((__m128i *)(p + i + 32), v);
        _mm_stream_si128((__m128i *)(p + i + 48), v);
    }
    _mm_sfence(); /* the stores are visible before anything the fill publishes next */
    for (uint64_t k = head + body; k < size; k++) dst[k] = pat[k & 15];
}

static bool cpu_fill(renderer *r, const gpu *g, const stage_state *cs, uint32_t x) {
    static int off = -1;
    if (off < 0) off = getenv("PS5C_NO_CPU_FILL") != NULL;
    const sh_program *p = cs->prog;
    if (off || cs->info.local_size[0] != 64 || cs->info.local_size[1] != 1 ||
        cs->info.local_size[2] != 1 || !(cs->info.cs_tgid_enable & 1))
        return false;
    guest_addr base=0;uint64_t size=0;uint32_t data[4];
    if (!cpu_buffer_pattern(r,g,cs,x,&base,&size,data)) {
        /* v_lshl_add_u32 v0, tgid, 6, v0; v_mov_b32 x1 or x4; buffer_store_format_x or _xyzw; s_endpgm:
         * a fill with one dword (Little Nightmares zeroes 5 MB of metadata this way) or four.
         * The one-dword fill can also take its value from s_buffer_load_dword + s_waitcnt
         * (Little Nightmares sets DCC metadata so: on the GPU, reading it back waited for the batch). */
        const rdna2_inst *in = p->insts;
        bool loaded = p->count == 6;
        uint32_t comps = p->count == 7 ? 4 : p->count == 4 || loaded ? 1 : 0, mov_at = loaded ? 3 : 1;
        if (!comps) return false;
        uint32_t user = cs->info.cs_tgid_sgpr; /* user SGPRs come first, then the group id */
        if (strcmp(in[0].name, "v_lshl_add_u32") || in[0].src[0] != user || in[0].src[1] != 128 + 6 || in[0].src[2] != 256 ||
            in[0].dst < 256)
            return false;
        uint32_t value = 0;
        uint32_t first = 0;
        for (uint32_t k = 0; k < comps; k++) {
            const rdna2_inst *m = &in[mov_at + k];
            if (strncmp(m->name, "v_mov_b32", 9) || m->dst < 256 || m->dpp_sdwa ||
                m->abs || m->neg || m->omod || m->clamp || m->opsel) return false;
            if (k == 0) first = m->dst;
            if (m->dst != first + k) return false;
            if (loaded) {
                if (m->src[0] != in[1].dst) return false;
                data[k] = value;
            } else if (m->src[0] < user)
                data[k] = gpu_reg(g, mmCOMPUTE_USER_DATA_0 + m->src[0]);
            else if (!sh_inline_const(m->src[0], m->literal, &data[k]))
                return false;
        }
        const rdna2_inst *st = &in[mov_at + comps];
        if (strcmp(st->name, comps == 4 ? "buffer_store_format_xyzw" : "buffer_store_format_x") || !st->idxen || st->offen ||
            st->offset || st->vaddr != in[0].dst - 256 || st->vdata + 256u != first || st->srsrc + 3u >= user ||
            strcmp(in[mov_at + comps + 1].name, "s_endpgm"))
            return false;
        uint32_t soff = 0;
        if (!sh_inline_const(st->soffset, 0, &soff) || soff) return false;
        uint32_t d[4];
        for (int k = 0; k < 4; k++) d[k] = gpu_reg(g, mmCOMPUTE_USER_DATA_0 + st->srsrc + (uint32_t)k);
        base = (guest_addr)d[0] | ((guest_addr)(d[1] & 0xFFFF) << 32);
        uint32_t stride = (d[1] >> 16) & 0x3FFF, oob = (d[3] >> 28) & 3;
        sh_buf_format f;
        if (stride != 4 * comps || !sh_buffer_format((d[3] >> 12) & 0x7F, &f) || f.comps != comps || f.bits[0] != 32 ||
            f.kind < 4 || ((d[3] >> 23) & 1))
            return false;
        if (comps == 4 && (f.bits[1] != 32 || f.bits[2] != 32 || f.bits[3] != 32 || (d[3] & 0xFFF) != 0xFAC)) return false;
        if (comps == 1) {
            if ((d[3] & 7) != 4) return false; /* DST_SEL_X selects X */
            if (loaded) {
                if (!cpu_fill_loaded_value(r, g, in, user, &value)) return false;
                data[0] = value;
            }
            data[1] = data[2] = data[3] = data[0];
        }
        uint64_t n = (uint64_t)x * 64, limit = oob == 3 ? d[2] / stride : d[2];
        if (n > limit) n = limit;
        size = n * stride;
    }
    if (!size) return true;
    vk_pipelines *P = r->pipes;
    bool pending = false; /* earlier work of the batch writes there: it lands first */
    for (uint32_t i = 0; i < P->wb_count && !pending; i++) pending = base < P->wb[i].addr + P->wb[i].size && P->wb[i].addr < base + size;
    uint8_t *dst = (uint8_t *)gmem_host(r->mem, base, size);
    if (!dst) return false;
    if (pending) {
        end_pass(r);
        sync_batch(r);
        apply_writebacks(r);
        vk_begin(r);
    }
    { /* PS5C_LOG_FILLS=<n>: 60 of them from the n-th (debugging aid) */
        static int logging = -1;
        static uint32_t logged, from;
        if (logging < 0) logging = getenv("PS5C_LOG_FILLS") != NULL, from = logging ? (uint32_t)atoi(getenv("PS5C_LOG_FILLS")) : 0;
        if (logging && logged++ >= from && logged <= from + 60)
            LOG_I(GPU, "fill 0x%llx, %llu bytes (%u groups), %08x %08x %08x %08x", (unsigned long long)base,
                  (unsigned long long)size, x, data[0], data[1], data[2], data[3]);
        for (uint32_t i = 0; logging && logged > from && logged <= from + 60 && i < r->image_count; i++) {
            const vk_image *im = &r->images[i];
            if (im->image && im->addr >= base && im->addr < base + size)
                LOG_I(GPU, "  image 0x%llx %ux%u format %d layers %u%s", (unsigned long long)im->addr, im->width, im->height,
                      (int)im->format, im->layers, im->gpu_written ? " (GPU written)" : "");
        }
    }
    if (log_memory_range(base, size, "fill candidate"))
        LOG_I(GPU, "  fill pattern %08x %08x %08x %08x", data[0], data[1], data[2], data[3]);
    /* A clear of an image's memory (Sonic zeroes its G-buffer this way every frame, and fills its
     * glow target with RGBA16F (0, 0, 0, 1)): the image the GPU holds for it is what later draws
     * read, so that is cleared, to the pattern as a texel of its format. The
     * buffer fill must also publish its bytes below: later buffer loads and
     * CPU reads can use that same memory without going through an image. */
    static int no_clears = -1; /* PS5C_NO_FILL_CLEARS=1: the memory is written, the images keep what they had */
    if (no_clears < 0) no_clears = getenv("PS5C_NO_FILL_CLEARS") != NULL;
    uint64_t before_clears=r->write_seq;
    if (!no_clears) {
        for (uint32_t i = 0; i < r->image_count; i++) {
            vk_image *im = &r->images[i];
            if (!im->image || im->aspect != VK_IMAGE_ASPECT_COLOR_BIT || im->addr != base) continue;
            uint64_t bytes = (uint64_t)im->width * im->height * vk_format_bytes(im->format) * (im->layers ? im->layers : 1) *
                             (im->depth ? im->depth : 1);
            if (!bytes || bytes > size || bytes * 2 < size) continue;
            VkClearColorValue value;
            if (!vk_fill_clear_value(im->format, data, &value)) continue;
            end_pass(r);
            vk_barrier(r->cmd, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            vkCmdClearColorImage(r->cmd, im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &all);
            im->gpu_written = true;
            im->written_seq = ++r->write_seq;
            if (trace_image(im)) LOG_I(GPU, "trace: fill CLEARS image 0x%llx fmt %d", (unsigned long long)im->addr, (int)im->format);
        }
    }
    uint8_t b0 = (uint8_t)data[0];
    bool one_byte = data[0] == data[1] && data[1] == data[2] && data[2] == data[3] && data[0] == b0 * 0x01010101u;
    gpu_watch_prepare_write(base, size); /* one protection change for watched pages, not a fault each */
    if (size >= (256u << 10) && fill_streaming_on()) {
        fill_streaming(dst, size, data);
    } else if (one_byte) {
        memset(dst, b0, (size_t)size);
    } else { /* the pattern, then copies of what is there, doubling */
        memcpy(dst, data, (size_t)(size < 16 ? size : 16));
        for (uint64_t done = 16; done < size;) {
            uint64_t chunk = done < size - done ? done : size - done;
            memcpy(dst + done, dst, (size_t)chunk);
            done += chunk;
        }
    }
    /* The fill publishes guest bytes too. Its completed CPU publication is
     * the baseline for the images cleared above, rather than an apparent CPU
     * overwrite of the clear at the next sample. */
    for(uint32_t i=0;i<r->image_count;i++)
        if(r->images[i].image && r->images[i].written_seq>before_clears) {
            const texture_cpu_source *s=&P->tex_cpu[i];
            if(s->size && s->addr>=base && s->addr+s->size<=base+size) {
                uint8_t phase=(uint8_t)((s->addr-base)&15u);
                bool dyn=P->tex_dynamic[i];
                if(P->tex_fill_hash[i] && P->tex_fill_phase[i]==phase && P->tex_fill_dynamic[i]==dyn &&
                   !memcmp(P->tex_fill_data[i],data,sizeof(P->tex_fill_data[i]))) {
                    P->tex_hash[i]=P->tex_fill_hash[i];
                    P->tex_checked[i]=P->epoch;
                    continue;
                }
                P->tex_checked[i]=0;
                texture_gpu_write(r,&r->images[i]);
                P->tex_fill_hash[i]=P->tex_hash[i];
                memcpy(P->tex_fill_data[i],data,sizeof(P->tex_fill_data[i]));
                P->tex_fill_phase[i]=phase;
                P->tex_fill_dynamic[i]=dyn;
            } else {
                P->tex_checked[i]=0;
                texture_gpu_write(r,&r->images[i]);
            }
        }
    htile_written(P, base, size, "fill", 0, true);
    copies_written(P, base, size);
    evals_written(P, base, size);
    P->dispatches++;
    return true;
}

#include "gpu/vk/vk_large_compute.h"

void renderer_dispatch(renderer *r, const gpu *g, uint32_t x, uint32_t y, uint32_t z) {
    g_trace_flip = g_flow_flip = g->stats.flips;
    pal_mutex_lock(r->lock);
    if (!vk_guest_access_begin(r)) { pal_mutex_unlock(r->lock); return; }
    large_import_cache_drop(r);
    r->pipes->wave_operation++;
    uint64_t t_disp = stat_now();
    vk_begin(r);
    retired_poll(r);
    uint64_t t_prof = prof_now(r);
    static int64_t trace_from = -2;
    if (trace_from == -2) trace_from = getenv("PS5C_TRACE_DISPATCH") ? atoll(getenv("PS5C_TRACE_DISPATCH")) : -1;
    bool trace_dispatch = trace_from >= 0 && g->stats.flips >= (uint64_t)trace_from;
    guest_addr trace_addr = gpu_reg_addr256(g, mmCOMPUTE_PGM_LO);
    if (trace_dispatch)
        LOG_I(GPU, "dispatch begin flip %llu CS 0x%llx groups %u,%u,%u threads %u,%u,%u rsrc2 %08x user %08x %08x %08x %08x",
              (unsigned long long)g->stats.flips, (unsigned long long)trace_addr, x, y, z,
              gpu_reg(g, mmCOMPUTE_NUM_THREAD_X), gpu_reg(g, mmCOMPUTE_NUM_THREAD_Y),
              gpu_reg(g, mmCOMPUTE_NUM_THREAD_Z), gpu_reg(g, mmCOMPUTE_PGM_RSRC2), gpu_reg(g, mmCOMPUTE_USER_DATA_0),
              gpu_reg(g, mmCOMPUTE_USER_DATA_0 + 1), gpu_reg(g, mmCOMPUTE_USER_DATA_0 + 2), gpu_reg(g, mmCOMPUTE_USER_DATA_0 + 3));
    bool reuse_stage = reuse_draw_stages(r->pipes, g->stats.flips);
    stage_state *cs = get_dispatch_stage(r->pipes, reuse_stage);
    sh_program *wave_program = NULL;
    if (cs) cs->prog = get_program(r, gpu_reg_addr256(g, mmCOMPUTE_PGM_LO));
    /* Experimental packet-driven wave selection. Do not modify the shared
     * analyzed program: graphics and pipeline workers may still use it. */
    const char *wave_packet = getenv("PS5C_CS_WAVE_FROM_PACKET");
    if (cs && cs->prog && wave_packet &&
        (!strcmp(wave_packet,"1") || strtoull(wave_packet,NULL,16)==cs->prog->hash)) {
        bool wave32 = (gpu_reg(g, mmCOMPUTE_DISPATCH_INITIATOR) & 0x8000u) != 0;
        cs->wave_locked=true;cs->wave32_expected=wave32;
        if (wave32 != cs->prog->wave32) {
            const sh_program *variant=program_with_wave(r,cs->prog,wave32,g->stats.flips,&wave_program);
            if(variant)cs->prog=variant;
        }
    }
    if (cs && cs->prog) {
        uint32_t rsrc2 = gpu_reg(g, mmCOMPUTE_PGM_RSRC2);
        uint32_t user = (rsrc2 >> 1) & 0x1F;
        init_sgprs(&cs->eval, g, 0, mmCOMPUTE_USER_DATA_0, user);
        cs->gcn = g_gcn = g->gcn; /* a PS4 dispatch: GCN buffer/image descriptors (gcn_descriptors) */
        cs->info.stage = SH_STAGE_CS;
        cs->info.fp16_overflow = sh_fp16_overflow_bit(SH_STAGE_CS, gpu_reg(g, mmCOMPUTE_PGM_RSRC1));
        cs->info.local_size[0] = gpu_reg(g, mmCOMPUTE_NUM_THREAD_X) & 0x3FF;
        cs->info.local_size[1] = gpu_reg(g, mmCOMPUTE_NUM_THREAD_Y) & 0x3FF;
        cs->info.local_size[2] = gpu_reg(g, mmCOMPUTE_NUM_THREAD_Z) & 0x3FF;
        for (int i = 0; i < 3; i++) {
            if (!cs->info.local_size[i]) cs->info.local_size[i] = i ? 1 : 64;
        }
        cs->info.cs_tgid_sgpr = user;
        cs->info.cs_tgid_enable = (uint8_t)((rsrc2 >> 7) & 7);
        cs->eval.group_sgpr = (uint8_t)user;
        cs->eval.group_sgprs = (uint8_t)(((rsrc2 >> 7) & 1) + ((rsrc2 >> 8) & 1) + ((rsrc2 >> 9) & 1));
        cs->info.cs_tid_components = (uint8_t)(((rsrc2 >> 11) & 3) + 1);
        cs->info.lds_dwords = ((rsrc2 >> 15) & 0x1FF) * 128; /* granularity: 512 bytes */
        cs->groups[0] = x, cs->groups[1] = y, cs->groups[2] = z;
        bool uses_lds=false;
        for(uint32_t i=0;i<cs->prog->count;i++) uses_lds |= sh_inst_uses_lds(&cs->prog->insts[i]);
        uint64_t shared_bytes=uses_lds?(uint64_t)(cs->info.lds_dwords?cs->info.lds_dwords:16384)*4:0;
        if(sh_compute_auto_wave64(cs->prog,&cs->info))shared_bytes+=66*4;
        if (!vk_compute_shape(cs->info.local_size,&r->props.limits,cs->info.host_local_size) ||
            shared_bytes > r->props.limits.maxComputeSharedMemorySize) {
            static uint32_t logged;
            if(logged++<16) LOG_E(GPU,"compute shader 0x%llx exceeds host group/shared-memory limits (%u,%u,%u, LDS %u bytes)",
                (unsigned long long)cs->prog->base,cs->info.local_size[0],cs->info.local_size[1],cs->info.local_size[2],
                uses_lds?(cs->info.lds_dwords?cs->info.lds_dwords:16384)*4:0);
            r->pipes->skipped++;
            goto dispatch_cleanup;
        }
        const char *paired=getenv("PS5C_CS_WAVE64");
        uint64_t paired_values[8]={paired?strtoull(paired,NULL,16):0};uint32_t paired_count=1;
        if(paired && paired[0]=='@') {
            static uint64_t checked=UINT64_MAX,values[8];static uint32_t count;
            if(checked!=g->stats.flips) {
                checked=g->stats.flips;uint64_t next[8]={0};uint32_t n=0;
                FILE *f=fopen(paired+1,"r");unsigned long long value;
                if(f){while(n<PS5C_ARRAY_COUNT(next) && fscanf(f,"%llx",&value)==1)next[n++]=(uint64_t)value;fclose(f);}
                if(n!=count || memcmp(next,values,sizeof(values)))
                    LOG_I(GPU,"paired compute selection: %u entries, first %016llx",n,(unsigned long long)next[0]);
                count=n;memcpy(values,next,sizeof(values));
            }
            paired_count=count;memcpy(paired_values,values,sizeof(values));
        }
        bool paired_wanted=false;
        for(uint32_t i=0;i<paired_count;i++)paired_wanted|=paired_values[i]==1 || paired_values[i]==cs->prog->hash;
        if(paired && paired_wanted &&
           !cs->prog->wave32 && r->cs_subgroup==32) {
            uint64_t threads=(uint64_t)cs->info.local_size[0]*cs->info.local_size[1]*cs->info.local_size[2];
            bool safe=true;
            for(uint32_t i=0;i<cs->prog->count;i++) {
                uint32_t op=cs->prog->ops[i];
                safe &= op!=SH_OP_S_WQM_B32 && op!=SH_OP_S_WQM_B64 &&
                    op!=SH_OP_S_QUADMASK_B32 && op!=SH_OP_S_QUADMASK_B64;
            }
            if(safe && threads>=64 && threads<=256 && threads%64==0 &&
               (uint64_t)cs->info.lds_dwords*4+(threads/64)*268<=r->props.limits.maxComputeSharedMemorySize) {
                cs->info.cs_wave_mode=1;
                static uint64_t reported[16];static uint32_t reported_count;
                bool seen=false;for(uint32_t i=0;i<reported_count;i++)seen|=reported[i]==cs->prog->hash;
                if(!seen && reported_count<PS5C_ARRAY_COUNT(reported)) {
                    reported[reported_count++]=cs->prog->hash;
                    LOG_I(GPU,"paired wave64 shader %016llx: %llu threads, LDS %u bytes",
                        (unsigned long long)cs->prog->hash,(unsigned long long)threads,cs->info.lds_dwords*4);
                }
            }
        }
        const char *native=getenv("PS5C_CS_NATIVE32");
        uint64_t native_selection=native?strtoull(native,NULL,16):0;
        if(native && native[0]=='@') {
            static uint64_t checked_flip=UINT64_MAX,selected;
            if(checked_flip!=g->stats.flips) {
                checked_flip=g->stats.flips;unsigned long long value=0;
                FILE*f=fopen(native+1,"r");if(f){if(fscanf(f,"%llx",&value)!=1)value=0;fclose(f);}
                if(selected!=(uint64_t)value)LOG_I(GPU,"native32 compute selection: %016llx",value);
                selected=(uint64_t)value;
            }
            native_selection=selected;
        }
        if(native && (native_selection==1 || native_selection==cs->prog->hash) &&
           cs->prog->wave32 && r->cs_subgroup==32) {
            uint64_t threads=(uint64_t)cs->info.local_size[0]*cs->info.local_size[1]*cs->info.local_size[2];
            bool safe=true;
            for(uint32_t i=0;i<cs->prog->count;i++) {
                uint32_t op=cs->prog->ops[i];
                safe &= op!=SH_OP_S_WQM_B32 && op!=SH_OP_S_WQM_B64 &&
                    op!=SH_OP_S_QUADMASK_B32 && op!=SH_OP_S_QUADMASK_B64;
            }
            if(safe && threads>=32 && threads<=1024 && threads%32==0)cs->info.cs_wave_mode=2;
        }
        const char *prefix = getenv("PS5C_CS_BARRIER_PREFIX");
        cs->info.barrier_prefix = prefix && (!strcmp(prefix,"1") || strtoull(prefix,NULL,16)==cs->prog->hash);
        const char *leaf_calls=getenv("PS5C_SHADER_LEAF_INLINE");
        cs->info.mesh_indexed=cs->info.cs_wave_mode==2 && cs->prog->function_count>1 && leaf_calls && !strcmp(leaf_calls,"1")?SH_LEAF_CALL_INLINE:0;
        const char *join_prefix=getenv("PS5C_NGG_JOIN_PREFIX");
        if(cs->info.cs_wave_mode==2 && join_prefix && (!strcmp(join_prefix,"1") || !strcmp(join_prefix,"2"))) {
            cs->info.mesh_indexed|=SH_JOIN_PREFIX;cs->info.barrier_prefix=true;
            if(!strcmp(join_prefix,"2"))cs->info.mesh_indexed|=SH_JOIN_SUFFIX;
        }
        const char *wave_barriers=getenv("PS5C_CS_WAVE_BARRIERS");
        uint64_t wave_selection=wave_barriers?strtoull(wave_barriers,NULL,16):0;
        if(wave_barriers && wave_barriers[0]=='@') {
            static uint64_t selection_flip=UINT64_MAX,selection;
            if(selection_flip!=g->stats.flips) {
                selection_flip=g->stats.flips;unsigned long long value=0;
                FILE*f=fopen(wave_barriers+1,"r");if(f){if(fscanf(f,"%llx",&value)!=1)value=0;fclose(f);}
                if(selection!=(uint64_t)value)LOG_I(GPU,"compute workgroup PC selection: %016llx",value);
                selection=(uint64_t)value;
            }
            wave_selection=selection;
        }
        uint64_t wg_threads=(uint64_t)cs->info.local_size[0]*cs->info.local_size[1]*cs->info.local_size[2];
        /* Native32 is already opt-in. A multi-wave guest workgroup using
         * s_barrier still needs a real workgroup rendezvous outside divergent
         * PC blocks; memory ordering alone cannot implement its execution
         * barrier. Keep an explicit selection (including 0) authoritative. */
        if(!wave_barriers && cs->info.cs_wave_mode==2 && wg_threads>32)
            for(uint32_t i=0;i<cs->prog->count;i++)if(cs->prog->ops[i]==SH_OP_S_BARRIER){wave_selection=1;break;}
        uint64_t wave_count=wg_threads/(cs->info.cs_wave_mode==2?32:64);
        uint64_t lds_bytes=(uint64_t)(cs->info.lds_dwords?cs->info.lds_dwords:16384)*4;
        uint64_t auxiliary=wave_count*(cs->info.cs_wave_mode==1?272:4);
        cs->info.wave_barriers=cs->info.cs_wave_mode && wave_selection &&
            (wave_selection==1 || wave_selection==cs->prog->hash) &&
            lds_bytes+auxiliary<=r->props.limits.maxComputeSharedMemorySize;
        if(cs->info.host_local_size[0]) {
            static uint32_t reshaped;
            if(reshaped++<8) LOG_I(GPU,"compute 0x%llx: group %u,%u,%u -> host %u,%u,%u (guest local IDs preserved)",
                (unsigned long long)cs->prog->base,cs->info.local_size[0],cs->info.local_size[1],cs->info.local_size[2],
                cs->info.host_local_size[0],cs->info.host_local_size[1],cs->info.host_local_size[2]);
        }
        if (cs->prog->has_stores && (store_windows_on() || footprint_record(cs->prog))) { /* store proofs read scalar operands */
            static _Thread_local uint32_t vsrc[SH_MAX_INSTS][3];
            static _Thread_local uint8_t vsrc_state[SH_MAX_INSTS][3];
            cs->eval.vsrc = vsrc;
            cs->eval.vsrc_state = vsrc_state;
        }
        if (cpu_fill(r, g, cs, x)) {
        } else if (setup_stage(r, cs)) {
            ngg_large_reads *large_compute=large_compute_begin(r,cs,g->stats.flips);
            vk_compute_partition pieces;
            vk_compute_part part;
            uint32_t total[3] = {x, y, z};
            vk_compute_partition_init(&pieces, total, r->props.limits.maxComputeWorkGroupCount);
            while (vk_compute_partition_next(&pieces, &part)) {
                memcpy(cs->dispatch_base, part.base, sizeof(part.base));
                if (!do_dispatch(r, cs, part.count[0], part.count[1], part.count[2])) {
                    end_pass(r);
                    sync_batch(r);
                    vk_begin(r);
                    if (!do_dispatch(r, cs, part.count[0], part.count[1], part.count[2]))
                        r->pipes->skipped++;
                }
                vs_probe_read(r, g, NULL, cs, NULL); /* PS5C_VS_PROBE on a compute shader */
                save_after(r,g,cs->prog,true);
            }
            large_compute_end(r,cs,large_compute);
        }
dispatch_cleanup:
        if (!reuse_stage) sh_eval_free(&cs->eval);
    } else {
        r->pipes->skipped++;
    }
    prof_cpu(r, cs && cs->prog ? cs->prog->base : 0, 0, t_prof);
    sync_check(r, "dispatch CS hash", cs && cs->prog ? cs->prog->hash : 0, 0);
    if (trace_dispatch) {
        LOG_I(GPU, "dispatch recorded flip %llu CS 0x%llx", (unsigned long long)g->stats.flips,
              (unsigned long long)trace_addr);
        if (getenv("PS5C_TRACE_DISPATCH_SYNC")) {
            sync_batch(r);
            LOG_I(GPU, "dispatch complete flip %llu CS 0x%llx", (unsigned long long)g->stats.flips,
                  (unsigned long long)trace_addr);
            vk_begin(r);
        }
    }
    { /* PS5C_SYNC_DISPATCHES: every dispatch finishes and its stores reach guest memory before
       * the next one is evaluated (debugging aid: dumps then show GPU-written data) */
        static int each = -1;
        if (each < 0) each = getenv("PS5C_SYNC_DISPATCHES") != NULL;
        if (each) {
            end_pass(r);
            sync_batch(r);
            apply_writebacks(r);
            vk_begin(r);
        }
    }
    if (g_stats_on && cs && cs->prog) prog_stat_add(g_stat_cs_prog, cs->prog, pal_time_ns() - t_disp);
    program_storage_free(wave_program,sparse_programs());
    if (cs && !reuse_stage) { /* a retained stage is reset at the next dispatch */
        program_storage_free(cs->linked_program,sparse_programs());
        free(cs);
    }
    stat_add(ST_DISPATCH, t_disp);
    vk_guest_access_end(r);
    pal_mutex_unlock(r->lock);
}

/* Correctness-first implementation: a dump is a CPU-visible sample point.
 * Batching/deferred publication may replace the synchronous fence later. */
bool renderer_sample_counter(renderer *r,uint64_t *value) {
    if(!r || !r->pipes || !r->pipes->occlusion_pool)return false;
    if(!value)return !r->pipes->occlusion_failed;
    pal_mutex_lock(r->lock);
    vk_pipelines *P=r->pipes;
    if(r->recording){end_pass(r);sync_batch(r);}
    vk_begin(r); /* collect completed queries before resetting this pool */
    *value=P->occlusion_total;
    P->occlusion_scope=true;
    static unsigned logged;
    if(logged++<32)LOG_I(GPU,"occlusion dump: cumulative %llu actual samples",(unsigned long long)*value);
    bool ok=!P->occlusion_failed;
    pal_mutex_unlock(r->lock);
    return ok;
}
