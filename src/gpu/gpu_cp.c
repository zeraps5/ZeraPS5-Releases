/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "gpu/gpu.h"
#include "gpu/gpu_regs.h"
#include "gpu/gcn.h"
#include "gpu/lod_stats.h"
#include <stdatomic.h>
#include "gpu/shader/shader.h"
#include "pal/pal.h"

/* PM4 type-3 opcodes (AMD GFX10) */
#define IT_NOP 0x10
#define IT_CLEAR_STATE 0x12
#define IT_SET_BASE 0x11
#define IT_DRAW_INDIRECT_MULTI 0x2C
#define IT_DRAW_INDEX_INDIRECT_MULTI 0x38
#define IT_DISPATCH_DIRECT 0x15
#define IT_DISPATCH_INDIRECT 0x16
#define IT_DRAW_INDEX_2 0x27
#define IT_DRAW_INDEX_AUTO 0x2D
#define IT_DRAW_INDIRECT 0x24
#define IT_DRAW_INDEX_INDIRECT 0x25
#define IT_DRAW_INDEX_OFFSET_2 0x35
#define IT_INDIRECT_BUFFER_CNST 0x33
#define IT_WRITE_DATA 0x37
#define IT_DRAW_INDEX_MULTI_AUTO 0x30
#define IT_INDIRECT_BUFFER 0x3F
#define IT_EVENT_WRITE 0x46
#define IT_EVENT_WRITE_EOP 0x47
#define IT_RELEASE_MEM 0x49
#define IT_DMA_DATA 0x50
#define IT_COND_EXEC 0x22
#define IT_WAIT_REG_MEM 0x3C
#define IT_MEM_SEMAPHORE 0x39
#define IT_WAIT_REG_MEM_64 0x93
#define IT_REWIND 0x59
#define IT_ATOMIC_MEM 0x1E
#define IT_COPY_DATA 0x40
#define IT_INDEX_BASE 0x26
#define IT_INDEX_TYPE 0x2A
#define IT_INDEX_BUFFER_SIZE 0x13
#define IT_NUM_INSTANCES 0x2F
#define IT_LOAD_UCONFIG_REG 0x5E
#define IT_LOAD_SH_REG 0x5F
#define IT_LOAD_CONTEXT_REG 0x61
#define IT_LOAD_SH_REG_INDEX 0x63
#define IT_LOAD_UCONFIG_REG_INDEX 0x64
#define IT_SET_CONFIG_REG 0x68
#define IT_SET_CONTEXT_REG 0x69
#define IT_SET_CONTEXT_REG_INDEX 0x6A
#define IT_SET_SH_REG 0x76
#define IT_SET_UCONFIG_REG 0x79
#define IT_SET_UCONFIG_REG_INDEX 0x7A
#define IT_SET_SH_REG_INDEX 0x9B
#define IT_LOAD_CONTEXT_REG_INDEX 0x9F

#define MAX_IB_DEPTH 4
/* The legacy walker cannot yield: bound tail jumps so a cyclic guest stream
 * cannot monopolize its GPU worker. The resumable walker uses its own budget. */
#define MAX_LEGACY_CHAIN_HOPS 65536u

ps5c_err gpu_init(gpu *g, gmem *mem, const gpu_callbacks *cb) {
    memset(g, 0, sizeof(*g));
    g->mem = mem;
    if (cb) g->cb = *cb;
    g->regs = (uint32_t *)calloc(GPU_REG_COUNT, sizeof(uint32_t));
    if (!g->regs || pal_mutex_create(&g->lock) != PS5C_OK) {
        free(g->regs);
        g->regs = NULL;
        return PS5C_ERR_NOMEM;
    }
    g->num_instances = 1;
    g->regs[mmPA_SC_AA_MASK_X0Y0_X1Y0] = UINT32_MAX;
    g->regs[mmPA_SC_AA_MASK_X0Y1_X1Y1] = UINT32_MAX;
    if (!sh_gds_base && gmem_map(mem, 0, SH_GDS_SIZE, GPROT_RW, 0, "gds", &sh_gds_base) != PS5C_OK) sh_gds_base = 0;
    const char *dump = getenv("PS5C_GPU_DUMP");
    g->dump_frame = dump && dump[0] != '@' ? atoll(dump + (dump[0] == '+')) : -1;
    return PS5C_OK;
}

/* PS5C_GPU_DUMP=+<frame>: that frame and every later one */
/* Whether real commands follow dword `pos` of a command block (NOP packets and type-2
 * filler are padding). */
static bool packets_after(const uint32_t *cmd, uint32_t pos, uint32_t dwords) {
    while (cmd && pos < dwords) {
        uint32_t hdr = cmd[pos];
        if (hdr >> 30 == 2 || hdr == 0xFFFF1000u) {
            pos++;
            continue;
        }
        if (hdr >> 30 != 3 || ((hdr >> 8) & 0xFF) != 0x10) return true;
        pos += ((hdr >> 16) & 0x3FFF) + 2;
    }
    return false;
}

static bool dumping(const gpu *g) {
    static int from = -1;
    if (from < 0) from = getenv("PS5C_GPU_DUMP") && getenv("PS5C_GPU_DUMP")[0] == '+';
    return g->dump_frame >= 0 && (from ? (int64_t)g->stats.flips >= g->dump_frame : (int64_t)g->stats.flips == g->dump_frame);
}

/* ---- Registers ---- */

/* PS5C_WATCH_REG=<register>: logs the writes to one register (debugging aid) */
static int32_t watch_reg_id(void) {
    static int32_t watch = -1;
    if (watch == -1) {
        const char *e = getenv("PS5C_WATCH_REG");
        watch = e ? (int32_t)strtoul(e, NULL, 16) : -2;
    }
    return watch;
}

static void watch_reg(const gpu *g, uint32_t reg, uint32_t value, const char *how) {
    int32_t watch = watch_reg_id();
    /* within the frame PS5C_GPU_DUMP names, else the first writes */
    static uint32_t logged;
    bool in_dump = g->dump_frame >= 0 && (int64_t)g->stats.flips == g->dump_frame;
    if ((int32_t)reg == watch && (in_dump || (g->dump_frame < 0 && logged++ < 64)))
        LOG_I(GPU, "reg 0x%04x = 0x%08x (%s)", reg, value, how);
}

static void set_regs(gpu *g, uint32_t base, const uint32_t *p, uint32_t len) {
    if (len < 3) return;
    uint32_t reg = base + (p[1] & 0xFFFF);
    if (watch_reg_id() < 0) {
        if (reg >= GPU_REG_COUNT) return;
        uint32_t n = len - 2;
        if (n > GPU_REG_COUNT - reg) n = GPU_REG_COUNT - reg;
        memcpy(&g->regs[reg], p + 2, (size_t)n * sizeof(uint32_t));
        return;
    }
    for (uint32_t i = 2; i < len && reg < GPU_REG_COUNT; i++) {
        watch_reg(g, reg, p[i], "set");
        g->regs[reg++] = p[i];
    }
}

/* LOAD_*_REG_INDEX: dw1 address[31:2], dw2 address hi, dw3 data_format[31]
 * reg_offset[15:0], dw4 count. data_format 1: the memory holds `count`
 * (offset, value) pairs; 0: `count` consecutive registers from reg_offset. */
static void load_regs_index(gpu *g, uint32_t base, const uint32_t *p, uint32_t len) {
    if (len < 5) return;
    guest_addr a = (guest_addr)(p[1] & ~3u) | ((guest_addr)p[2] << 32);
    uint32_t n = p[4] & 0x3FFF;
    bool pairs = (p[3] & 0x80000000u) != 0;
    if (!n) return; /* empty tables (address 0) are common: nothing to load */
    const uint32_t *t = (const uint32_t *)gmem_host(g->mem, a, (uint64_t)n * (pairs ? 8 : 4));
    if (!t) {
        static uint32_t reported;
        if (reported++ < 8)
            LOG_W(GPU, "register table 0x%llx not mapped (packet %08x %08x %08x %08x %08x, %u dwords)", (unsigned long long)a,
                  p[0], p[1], p[2], p[3], p[4], len);
        return;
    }
    bool watching = watch_reg_id() >= 0;
    if (watching && g->dump_frame >= 0 && (int64_t)g->stats.flips == g->dump_frame) {
        LOG_I(GPU, "load %s: %08x %08x %08x %08x, %u entries at 0x%llx", pairs ? "pairs" : "range", p[1], p[2], p[3], p[4], n,
              (unsigned long long)a);
        for (uint32_t i = 0; pairs && i < n; i++) {
            uint32_t r = t[2 * i] & 0xFFFF;
            if ((r >= 0x80 && r < 0x98) || (r >= 0x100 && r < 0x140) || r == 0x206 || r == 0x207)
                LOG_I(GPU, "  ctx 0x%03x = %08x", r, t[2 * i + 1]);
        }
    }
    if (pairs) {
        if (!watching) {
            for (uint32_t i = 0; i < n; i++) {
                uint32_t reg = base + (t[2 * i] & 0xFFFF);
                if (reg < GPU_REG_COUNT) g->regs[reg] = t[2 * i + 1];
            }
        } else {
            for (uint32_t i = 0; i < n; i++) {
                uint32_t reg = base + (t[2 * i] & 0xFFFF);
                watch_reg(g, reg, t[2 * i + 1], "load pairs");
                if (reg < GPU_REG_COUNT) g->regs[reg] = t[2 * i + 1];
            }
        }
    } else {
        uint32_t reg = base + (p[3] & 0xFFFF);
        if (!watching) {
            if (reg < GPU_REG_COUNT) {
                uint32_t cnt = n < GPU_REG_COUNT - reg ? n : GPU_REG_COUNT - reg;
                memcpy(&g->regs[reg], t, (size_t)cnt * sizeof(uint32_t));
            }
        } else {
            for (uint32_t i = 0; i < n && reg < GPU_REG_COUNT; i++) {
                watch_reg(g, reg, t[i], "load index");
                g->regs[reg++] = t[i];
            }
        }
    }
}

/* LOAD_*_REG: dw1 address lo, dw2 address hi, then (reg_offset, count) pairs;
 * the memory holds the values back to back. */
static void load_regs(gpu *g, uint32_t base, const uint32_t *p, uint32_t len) {
    if (len < 5) return;
    guest_addr a = (guest_addr)(p[1] & ~3u) | ((guest_addr)(p[2] & 0xFFFF) << 32);
    bool watching = watch_reg_id() >= 0;
    for (uint32_t i = 3; i + 1 < len; i += 2) {
        uint32_t reg = base + (p[i] & 0xFFFF), n = p[i + 1] & 0x3FFF;
        const uint32_t *t = (const uint32_t *)gmem_host(g->mem, a, (uint64_t)n * 4);
        if (!t) return;
        if (!watching) {
            if (reg < GPU_REG_COUNT) {
                uint32_t cnt = n < GPU_REG_COUNT - reg ? n : GPU_REG_COUNT - reg;
                memcpy(&g->regs[reg], t, (size_t)cnt * sizeof(uint32_t));
            }
        } else {
            for (uint32_t k = 0; k < n && reg < GPU_REG_COUNT; k++) {
                watch_reg(g, reg, t[k], "load");
                g->regs[reg++] = t[k];
            }
        }
        a += (guest_addr)n * 4;
    }
}

/* ---- Shader capture (frame dump) ---- */

/* Shader length in dwords: up to and including s_endpgm. 0 if not found. */
uint32_t gpu_shader_size(const gpu *g, guest_addr addr) {
    /* The boundary-only fast path requires aligned instruction words. */
    if (addr & 3u) return 0;
    const uint32_t *code = (const uint32_t *)gmem_host(g->mem, addr, 4);
    if (!code) return 0;
    for (uint32_t i = 0; i < 0x10000; i++) {
        guest_addr cur = addr + (guest_addr)i * 4;
        if (i && (cur & 0xFFFu) < 4 && !gmem_host(g->mem, cur, 4)) return 0;
        if (code[i] == 0xBF810000u) return i + 1; /* s_endpgm */
    }
    return 0;
}

/* Writes the shader at `addr` to user/shaders/<addr>.bin (once per address); dwords 0: up to its s_endpgm. */
static void save_shader_words(const gpu *g, guest_addr addr, uint32_t dwords) {
    static guest_addr saved[4096];
    static uint32_t saved_count;
    if (!addr) return;
    for (uint32_t i = 0; i < saved_count; i++) {
        if (saved[i] == addr) return;
    }
    if (!dwords) dwords = gpu_shader_size(g, addr);
    if (!dwords || saved_count == 4096) return;
    saved[saved_count++] = addr;
    pal_mkdir("user");
    pal_mkdir("user/shaders");
    char path[64];
    snprintf(path, sizeof(path), "user/shaders/%010llx.bin", (unsigned long long)addr);
    pal_file *f = NULL;
    if (pal_file_open(path, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE, &f) != PS5C_OK) return;
    pal_file_write(f, gmem_host(g->mem, addr, (uint64_t)dwords * 4), (uint64_t)dwords * 4, NULL);
    pal_file_close(f);
    LOG_I(GPU, "  saved %s (%u dwords)", path, dwords);
}

static void save_shader(const gpu *g, guest_addr addr) { save_shader_words(g, addr, 0); }

/* ---- Draws ---- */

static uint32_t draw_index_size(const gpu *g) {
    uint32_t type = gpu_reg(g, mmVGT_INDEX_TYPE) & 3u;
    if (!g->gcn && type == 2u) return 1;
    return type == 1u ? 4 : 2;
}

static double f32_of(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static void log_draw(const gpu *g, const gpu_draw *d) {
    uint32_t rt_info = gpu_reg(g, mmCB_COLOR0_INFO), attrib2 = gpu_reg(g, mmCB_COLOR0_ATTRIB2);
    LOG_I(GPU, "draw: prim %u, %s %u x%u, ES 0x%llx, PS 0x%llx, RT0 0x%llx %ux%u fmt %u (info 0x%x), mask 0x%x",
          gpu_reg(g, mmVGT_PRIMITIVE_TYPE), d->indexed ? "indices" : "vertices", d->count, d->instances,
          (unsigned long long)gpu_reg_addr256(g, mmSPI_SHADER_PGM_LO_ES),
          (unsigned long long)gpu_reg_addr256(g, mmSPI_SHADER_PGM_LO_PS),
          (unsigned long long)((guest_addr)gpu_reg(g, mmCB_COLOR0_BASE) << 8), ((attrib2 >> 14) & 0x3FFF) + 1,
          (attrib2 & 0x3FFF) + 1, (rt_info >> 2) & 0x1F, rt_info, gpu_reg(g, mmCB_TARGET_MASK));
    LOG_I(GPU, "  col_format 0x%x, shader_mask 0x%x, color_control 0x%x, vte 0x%x, vport %g %g %g %g, RT1 0x%llx info 0x%x, "
          "ps rsrc2 0x%x gs rsrc2 0x%x, blend0 0x%x, depth 0x%x, ps_in 0x%x 0x%x",
          gpu_reg(g, mmSPI_SHADER_COL_FORMAT), gpu_reg(g, mmCB_SHADER_MASK), gpu_reg(g, mmCB_COLOR_CONTROL),
          gpu_reg(g, mmPA_CL_VTE_CNTL), f32_of(gpu_reg(g, mmPA_CL_VPORT_XSCALE)), f32_of(gpu_reg(g, mmPA_CL_VPORT_XOFFSET)),
          f32_of(gpu_reg(g, mmPA_CL_VPORT_YSCALE)), f32_of(gpu_reg(g, mmPA_CL_VPORT_YOFFSET)),
          (unsigned long long)((guest_addr)gpu_reg(g, mmCB_COLOR0_BASE + GPU_CB_STRIDE) << 8),
          gpu_reg(g, mmCB_COLOR0_INFO + GPU_CB_STRIDE), gpu_reg(g, mmSPI_SHADER_PGM_RSRC2_PS),
          gpu_reg(g, mmSPI_SHADER_PGM_RSRC2_GS), gpu_reg(g, mmCB_BLEND0_CONTROL), gpu_reg(g, mmDB_DEPTH_CONTROL),
          gpu_reg(g, mmSPI_PS_INPUT_ENA), gpu_reg(g, mmSPI_PS_INPUT_CNTL_0));
    LOG_I(GPU, "  depth 0x%llx z_info 0x%x size 0x%x, htile 0x%llx, clear %g", (unsigned long long)((guest_addr)gpu_reg(g, mmDB_Z_READ_BASE) << 8),
          gpu_reg(g, mmDB_Z_INFO), gpu_reg(g, mmDB_DEPTH_SIZE_XY), (unsigned long long)((guest_addr)gpu_reg(g, mmDB_HTILE_DATA_BASE) << 8),
          f32_of(gpu_reg(g, mmDB_DEPTH_CLEAR)));
    if (d->indexed) {
        const uint8_t *ix = (const uint8_t *)gmem_host(g->mem, d->index_addr, 24);
        uint16_t h[12] = {0};
        if (ix) memcpy(h, ix, sizeof(h));
        LOG_I(GPU, "  indices at 0x%llx (%u bytes each, type reg %u): u16 %u %u %u %u %u %u %u %u %u %u %u %u",
              (unsigned long long)d->index_addr, d->index_size, gpu_reg(g, mmVGT_INDEX_TYPE), h[0], h[1], h[2], h[3], h[4],
              h[5], h[6], h[7], h[8], h[9], h[10], h[11]);
    }
    const char *names[2] = {"GS", "PS"};
    const uint32_t bases[2] = {mmSPI_SHADER_USER_DATA_GS_0, mmSPI_SHADER_USER_DATA_PS_0};
    for (int s = 0; s < 2; s++) {
        char line[400];
        int n = snprintf(line, sizeof(line), "  %s user data:", names[s]);
        for (uint32_t i = 0; i < 16; i++) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %08x", gpu_reg(g, bases[s] + i));
        }
        LOG_I(GPU, "%s", line);
    }
}

static bool saving_shaders(void) {
    static int on = -1;
    if (on < 0) on = getenv("PS5C_GPU_SAVE_SHADERS") != NULL;
    return on != 0;
}

/* The GCN program at addr re-encoded as RDNA2 (fetch shader inlined, its address in the user data
 * `user_data` the s_swappc_b64 names): the RDNA2 dword count, 0 when it cannot be. Logs each new failure. */
static uint32_t gcn_program(const gpu *g, guest_addr addr, uint32_t user_data, uint32_t *out, uint32_t cap,
                            guest_addr *fetch_addr, int *fetch_reg) {
    uint32_t words = gpu_shader_size(g, addr);
    const uint32_t *code = words ? (const uint32_t *)gmem_host(g->mem, addr, (uint64_t)words * 4) : NULL;
    if (!code) return 0;
    const uint32_t *fetch = NULL;
    uint32_t fetch_words = 0;
    *fetch_addr = 0;
    *fetch_reg = -1;
    for (uint32_t i = 0; i < words; i++) {
        if ((code[i] >> 23) == 0x17Du && ((code[i] >> 8) & 0xFF) == 33) {
            uint32_t k = code[i] & 0xFF; /* s_swappc_b64 s[..], s[k:k+1] */
            if (k + 1 < GPU_USER_DATA_REGS) {
                guest_addr fa = (guest_addr)gpu_reg(g, user_data + k) | ((guest_addr)gpu_reg(g, user_data + k + 1) << 32);
                const uint32_t *f = (const uint32_t *)gmem_host(g->mem, fa, 256 * 4);
                fetch_words = f ? gcn_fetch_size(f, 256) : 0;
                if (fetch_words) {
                    fetch = f;
                    *fetch_addr = fa;
                    *fetch_reg = (int)k;
                }
            }
            break;
        }
    }
    gcn_error err = {0};
    uint32_t n = gcn_to_rdna2(code, words, fetch, fetch_words, out, cap, &err);
    if (!n) {
        static guest_addr told[64];
        static uint32_t told_count;
        bool seen = false;
        for (uint32_t i = 0; i < told_count && !seen; i++) seen = told[i] == addr;
        if (!seen && told_count < 64) {
            told[told_count++] = addr;
            LOG_W(GPU, "PS4 shader 0x%llx: %s at dword %u (0x%08x)%s", (unsigned long long)addr, err.why, err.pos,
                  err.word, err.fetch ? " of its fetch shader" : "");
        }
    }
    return n;
}

/* RDNA2 copies of PS4 programs, in guest memory the backend reads like any other shader's */
#define GCN_CODE_BYTES (4u << 20)
typedef struct gcn_copy {
    guest_addr gcn, fetch, code; /* code 0: cannot be re-encoded */
    int fetch_reg;               /* user-data register of the fetch shader's address, -1: none */
} gcn_copy;
static gcn_copy g_gcn_copies[1024];
static uint32_t g_gcn_copy_count;
static guest_addr g_gcn_code;
static uint64_t g_gcn_code_used;

static guest_addr user_addr(const gpu *g, uint32_t user_data, int k) {
    return (guest_addr)gpu_reg(g, user_data + (uint32_t)k) | ((guest_addr)gpu_reg(g, user_data + (uint32_t)k + 1) << 32);
}

/* The RDNA2 copy of the GCN program at addr (its fetch shader per the current user data); 0 if none */
static guest_addr gcn_copy_of(gpu *g, guest_addr addr, uint32_t user_data, guest_addr *fetch) {
    for (uint32_t i = 0; i < g_gcn_copy_count; i++) {
        const gcn_copy *c = &g_gcn_copies[i];
        if (c->gcn == addr && (c->fetch_reg < 0 || user_addr(g, user_data, c->fetch_reg) == c->fetch)) {
            *fetch = c->fetch;
            return c->code;
        }
    }
    static uint32_t buf[0x4000];
    int reg = -1;
    uint32_t n = gcn_program(g, addr, user_data, buf, PS5C_ARRAY_COUNT(buf), fetch, &reg);
    guest_addr code = 0;
    if (n && !g_gcn_code &&
        gmem_map(g->mem, 0, GCN_CODE_BYTES, GPROT_RW, 0, "PS4 shaders as RDNA2", &g_gcn_code) != PS5C_OK)
        g_gcn_code = 0;
    uint64_t bytes = ((uint64_t)n * 4 + 255) & ~255ull;
    if (n && g_gcn_code && g_gcn_code_used + bytes <= GCN_CODE_BYTES) {
        uint32_t *dst = (uint32_t *)gmem_host(g->mem, g_gcn_code + g_gcn_code_used, bytes);
        if (dst) {
            memcpy(dst, buf, (size_t)n * 4);
            code = g_gcn_code + g_gcn_code_used;
            g_gcn_code_used += bytes;
        }
    }
    if (g_gcn_copy_count < PS5C_ARRAY_COUNT(g_gcn_copies))
        g_gcn_copies[g_gcn_copy_count++] = (gcn_copy){addr, *fetch, code, reg};
    LOG_I(GPU, "PS4 shader 0x%llx (fetch 0x%llx): %s 0x%llx (%u dwords)", (unsigned long long)addr,
          (unsigned long long)*fetch, code ? "RDNA2 copy at" : "no RDNA2 copy", (unsigned long long)code, n);
    if (*fetch && (dumping(g) || saving_shaders()))
        save_shader_words(g, *fetch, gcn_fetch_size((const uint32_t *)gmem_host(g->mem, *fetch, 1024), 256));
    return code;
}

/* A PS4 draw: VS stage (SPI_SHADER_PGM_LO_VS) and GCN shaders. The backend draws it with the RDNA2
 * copies of its shaders, seeing the PS4 registers it reads differently as RDNA2 ones for the call:
 * - the programs: ES / PS addresses of the copies (the backend runs a VS from the ES registers)
 * - color targets: size from CB_COLORn_PITCH / SLICE (GCN has no ATTRIB2), no compression
 * - depth: size from DB_DEPTH_SIZE's tile counts, no HTILE
 * PS5C_GPU_SAVE_SHADERS saves its shaders, PS5C_GPU_DUMP logs it. */
static void draw_gcn(gpu *g, const gpu_draw *d) {
    static uint32_t told;
    guest_addr vs = gpu_reg_addr256(g, GPU_GCN_PGM_LO_VS), ps = gpu_reg_addr256(g, mmSPI_SHADER_PGM_LO_PS);
    guest_addr fetch = 0, none = 0;
    guest_addr vs_code = vs ? gcn_copy_of(g, vs, GPU_GCN_USER_DATA_VS_0, &fetch) : 0;
    guest_addr ps_code = ps ? gcn_copy_of(g, ps, mmSPI_SHADER_USER_DATA_PS_0, &none) : 0;
    if (told < 4 || dumping(g)) {
        told++;
        LOG_I(GPU, "PS4 draw: prim %u, %s %u x%u, VS 0x%llx (rsrc1 0x%x rsrc2 0x%x), PS 0x%llx, RT0 0x%llx "
              "(pitch 0x%x slice 0x%x info 0x%x attrib 0x%x), target mask 0x%x",
              gpu_reg(g, mmVGT_PRIMITIVE_TYPE), d->indexed ? "indices" : "vertices", d->count, d->instances,
              (unsigned long long)vs, gpu_reg(g, GPU_GCN_RSRC1_VS), gpu_reg(g, GPU_GCN_RSRC2_VS), (unsigned long long)ps,
              (unsigned long long)((guest_addr)gpu_reg(g, mmCB_COLOR0_BASE) << 8), gpu_reg(g, mmCB_COLOR0_BASE + 1),
              gpu_reg(g, mmCB_COLOR0_BASE + 2), gpu_reg(g, mmCB_COLOR0_INFO), gpu_reg(g, mmCB_COLOR0_ATTRIB),
              gpu_reg(g, mmCB_TARGET_MASK));
    }
    if (dumping(g) || saving_shaders()) {
        save_shader(g, vs);
        save_shader(g, ps);
    }
    if (!vs_code || (ps && !ps_code) || !g->cb.draw) return;
    static int off = -1; /* PS5C_PS4_NO_DRAWS=1: count them only */
    if (off < 0) off = getenv("PS5C_PS4_NO_DRAWS") != NULL;
    if (off) return;

    /* registers the call sees differently, and their PS4 values */
    enum { NSAVE = 6 + GPU_MAX_RENDER_TARGETS * 5 + 4 };
    uint32_t idx[NSAVE], old[NSAVE], n = 0;
#define SEE(reg, value) do { idx[n] = (reg); old[n] = g->regs[(reg)]; g->regs[(reg)] = (value); n++; } while (0)
    SEE(mmSPI_SHADER_PGM_LO_ES, (uint32_t)(vs_code >> 8));
    SEE(mmSPI_SHADER_PGM_HI_ES, (uint32_t)(vs_code >> 40) & 0xFF);
    SEE(mmSPI_SHADER_PGM_LO_PS, (uint32_t)(ps_code >> 8));
    SEE(mmSPI_SHADER_PGM_HI_PS, (uint32_t)(ps_code >> 40) & 0xFF);
    SEE(mmVGT_SHADER_STAGES_EN, 0);
    SEE(mmGE_CNTL, 0);
    for (uint32_t i = 0; i < GPU_MAX_RENDER_TARGETS; i++) {
        uint32_t base = mmCB_COLOR0_BASE + i * GPU_CB_STRIDE;
        uint32_t w = ((gpu_reg(g, base + 1) & 0x7FF) + 1) * 8;          /* CB_COLORn_PITCH TILE_MAX */
        uint32_t h = ((gpu_reg(g, base + 2) & 0x3FFFFF) + 1) * 64 / w;  /* CB_COLORn_SLICE TILE_MAX */
        if (!h) h = 1;
        SEE(mmCB_COLOR0_ATTRIB2 + i, ((h - 1) & 0x3FFF) | (((w - 1) & 0x3FFF) << 14));
        SEE(mmCB_COLOR0_ATTRIB3 + i, 0);
        SEE(mmCB_COLOR0_BASE_EXT + i, 0);
        SEE(base + (mmCB_COLOR0_INFO - mmCB_COLOR0_BASE), gpu_reg(g, base + (mmCB_COLOR0_INFO - mmCB_COLOR0_BASE)) & ~(3u << 13));
        SEE(base + (mmCB_COLOR0_DCC_CONTROL - mmCB_COLOR0_BASE), 0);
    }
    uint32_t zsize = gpu_reg(g, mmDB_DEPTH_SIZE_XY); /* GCN DB_DEPTH_SIZE: PITCH_TILE_MAX [10:0], HEIGHT_TILE_MAX [21:11] */
    uint32_t zw = ((zsize & 0x7FF) + 1) * 8, zh = (((zsize >> 11) & 0x7FF) + 1) * 8;
    SEE(mmDB_DEPTH_SIZE_XY, (zw - 1) | ((zh - 1) << 16));
    SEE(mmDB_Z_INFO, gpu_reg(g, mmDB_Z_INFO) & 0xFu);
    SEE(mmDB_STENCIL_INFO, gpu_reg(g, mmDB_STENCIL_INFO) & 0x1u);
    SEE(mmDB_HTILE_DATA_BASE, 0);
#undef SEE
    g->cb.draw(g->cb.user, g, d);
    while (n--) g->regs[idx[n]] = old[n];
}

static void draw(gpu *g, const gpu_draw *d) {
    g->stats.draws++;
    if (g->gcn) {
        draw_gcn(g, d);
        return;
    }
    { /* PS5C_LOG_RT=<address> or <width>x<height>: the first 400 draws into such a color
       * target, in any frame (debugging aid) */
        static guest_addr want;
        static uint32_t want_w, want_h;
        static int have = -1;
        static uint32_t logged;
        if (have < 0) {
            const char *e = getenv("PS5C_LOG_RT");
            have = e != NULL;
            if (e && strchr(e, 'x')) {
                want_w = (uint32_t)strtoul(e, NULL, 10);
                want_h = (uint32_t)strtoul(strchr(e, 'x') + 1, NULL, 10);
            } else if (e) {
                want = strtoull(e, NULL, 16);
            }
        }
        uint32_t a2 = gpu_reg(g, mmCB_COLOR0_ATTRIB2);
        bool match = want_w ? ((a2 >> 14) & 0x3FFF) + 1 == want_w && (a2 & 0x3FFF) + 1 == want_h
                            : (guest_addr)gpu_reg(g, mmCB_COLOR0_BASE) << 8 == want;
        if (have && !dumping(g) && logged < 400 && match) {
            logged++;
            LOG_I(GPU, "frame %llu:", (unsigned long long)g->stats.flips);
            log_draw(g, d);
        }
    }
    if (dumping(g)) log_draw(g, d);
    if (dumping(g) || saving_shaders()) {
        save_shader(g, gpu_reg_addr256(g, mmSPI_SHADER_PGM_LO_ES));
        save_shader(g, gpu_reg_addr256(g, mmSPI_SHADER_PGM_LO_PS));
    }
    if (g->cb.draw) g->cb.draw(g->cb.user, g, d);
}

static void dispatch(gpu *g, uint32_t x, uint32_t y, uint32_t z) {
    static int wave_init;
    static guest_addr wave_target;
    static unsigned wave_logged;
    if (!wave_init) {
        const char *wave_trace = getenv("PS5C_LOG_CS_WAVE");
        wave_target = wave_trace ? strtoull(wave_trace, NULL, 16) : 0;
        wave_init = 1;
    }
    if (wave_target && gpu_reg_addr256(g, mmCOMPUTE_PGM_LO) == wave_target && wave_logged++ < 10)
        LOG_I(GPU, "CS wave: shader 0x%llx initiator %08x", (unsigned long long)gpu_reg_addr256(g, mmCOMPUTE_PGM_LO), gpu_reg(g, mmCOMPUTE_DISPATCH_INITIATOR));
    g->stats.dispatches++;
    if (dumping(g)) {
        LOG_I(GPU, "dispatch %ux%ux%u, CS 0x%llx, threads %ux%ux%u", x, y, z,
              (unsigned long long)gpu_reg_addr256(g, mmCOMPUTE_PGM_LO), gpu_reg(g, mmCOMPUTE_NUM_THREAD_X),
              gpu_reg(g, mmCOMPUTE_NUM_THREAD_Y), gpu_reg(g, mmCOMPUTE_NUM_THREAD_Z));
        char line[400];
        int n = snprintf(line, sizeof(line), "  CS user data:");
        for (uint32_t i = 0; i < 16; i++) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %08x", gpu_reg(g, mmCOMPUTE_USER_DATA_0 + i));
        }
        LOG_I(GPU, "%s", line);
    }
    if (dumping(g) || saving_shaders()) save_shader(g, gpu_reg_addr256(g, mmCOMPUTE_PGM_LO));
    if (!g->cb.dispatch) return;
    if (!g->gcn) {
        g->cb.dispatch(g->cb.user, g, x, y, z);
        return;
    }
    /* A PS4 dispatch runs the RDNA2 copy of its GCN program, as draw_gcn does
     * for VS/PS; one that cannot be re-encoded is skipped (it was decoded as
     * RDNA2 bytes before). */
    guest_addr pgm = gpu_reg_addr256(g, mmCOMPUTE_PGM_LO), none = 0;
    guest_addr code = pgm ? gcn_copy_of(g, pgm, mmCOMPUTE_USER_DATA_0, &none) : 0;
    if (!code) return;
    uint32_t lo = g->regs[mmCOMPUTE_PGM_LO], hi = g->regs[mmCOMPUTE_PGM_HI];
    g->regs[mmCOMPUTE_PGM_LO] = (uint32_t)(code >> 8);
    g->regs[mmCOMPUTE_PGM_HI] = (uint32_t)(code >> 40) & 0xFF;
    g->cb.dispatch(g->cb.user, g, x, y, z);
    g->regs[mmCOMPUTE_PGM_LO] = lo;
    g->regs[mmCOMPUTE_PGM_HI] = hi;
}

/* Reads indirect arguments: earlier GPU work of the batch may still hold them. */
static bool read_args(gpu *g, guest_addr a, void *dst, uint32_t size) {
    static int trace_init;
    static guest_addr trace_addr;
    static unsigned traced;
    if (!trace_init) {
        const char *s = getenv("PS5C_TRACE_INDIRECT_ADDRESS");
        trace_addr = s ? strtoull(s, NULL, 16) : 0;
        trace_init = 1;
    }
    bool trace = trace_addr && a == trace_addr && size <= 20 && traced++ < 24;
    uint32_t before[5] = {0};
    bool before_ok = trace && gmem_read(g->mem, a, before, size);
    /* PS5C_GPU_STATS: how often indirect arguments wait for GPU results (a whole flush and wait) */
    static int stats = -1;
    static uint64_t reads, waits, wait_ns, last;
    if (stats < 0) stats = getenv("PS5C_GPU_STATS") != NULL;
    uint64_t t0 = stats ? pal_time_ns() : 0;
    if (g->cb.sync_mem) g->cb.sync_mem(g->cb.user, a, size);
    if (stats) {
        uint64_t took = pal_time_ns() - t0;
        reads++;
        if (took > 100000) waits++, wait_ns += took; /* a flush: a quick check takes microseconds */
        if (!last) last = t0;
        if (t0 - last >= 1000000000ull) {
            LOG_I(GPU, "indirect arguments: %llu read, %llu waited for GPU results (%.1f ms)", (unsigned long long)reads,
                  (unsigned long long)waits, (double)wait_ns / 1e6);
            reads = waits = wait_ns = 0;
            last = t0;
        }
    }
    bool ok = gmem_read(g->mem, a, dst, size);
    if (trace) {
        uint32_t after[5] = {0};
        if (ok) memcpy(after, dst, size);
        LOG_I(GPU, "indirect audit 0x%llx bytes %u sync %u valid %u/%u: before %08x %08x %08x %08x %08x after %08x %08x %08x %08x %08x",
              (unsigned long long)a, size, g->cb.sync_mem != NULL, before_ok, ok,
              before[0],before[1],before[2],before[3],before[4],after[0],after[1],after[2],after[3],after[4]);
    }
    return ok;
}

/* DRAW_(INDEX_)INDIRECT(_MULTI): arguments at `a`, `count` of them `stride`
 * bytes apart. The CP writes the base vertex and first instance into the user
 * SGPRs at vtx_loc / inst_loc (SH register offsets), and the draw index at
 * draw_loc when that is not ~0u. */
static void draw_indirect(gpu *g, bool indexed, guest_addr a, uint32_t count, uint32_t stride, uint32_t vtx_loc,
                          uint32_t inst_loc, uint32_t draw_loc) {
    if (count > 65536) count = 65536;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t args[5] = {0};
        if (!read_args(g, a + (guest_addr)i * stride, args, indexed ? 20 : 16)) {
            g->stats.draws++;
            return;
        }
        uint32_t n = args[0], instances = args[1];
        uint32_t base_vertex = indexed ? args[3] : args[2], first_instance = indexed ? args[4] : args[3];
        if (vtx_loc) g->regs[(GPU_REG_SH_BASE + vtx_loc) & 0xFFFF] = base_vertex;
        if (inst_loc) g->regs[(GPU_REG_SH_BASE + inst_loc) & 0xFFFF] = first_instance;
        if (draw_loc != ~0u) g->regs[(GPU_REG_SH_BASE + draw_loc) & 0xFFFF] = i;
        if (dumping(g)) {
            LOG_I(GPU, "indirect %s draw %u/%u at 0x%llx: %u x%u, first %u, base %d, first instance %u", indexed ? "indexed" : "",
                  i, count, (unsigned long long)(a + (guest_addr)i * stride), n, instances, indexed ? args[2] : args[2],
                  (int32_t)base_vertex, first_instance);
        }
        if (!n || !instances || (uint64_t)n * instances > (1u << 26)) {
            /* arguments a shader we could not run should have written */
            static uint32_t warned;
            if (n && instances && warned++ < 8) {
                LOG_W(GPU, "indirect draw at 0x%llx skipped: %u x%u", (unsigned long long)(a + (guest_addr)i * stride), n, instances);
                LOG_W(GPU, "indirect state: indexed %u stride %u entry %u, args %08x %08x %08x %08x %08x, ES 0x%llx GS 0x%llx RSRC2_GS %08x",
                      (unsigned)indexed, stride, i, args[0], args[1], args[2], args[3], args[4],
                      (unsigned long long)gpu_reg_addr256(g, mmSPI_SHADER_PGM_LO_ES),
                      (unsigned long long)gpu_reg_addr256(g, mmSPI_SHADER_PGM_LO_GS),
                      gpu_reg(g, mmSPI_SHADER_PGM_RSRC2_GS));
            }
            g->stats.draws++;
            continue;
        }
        gpu_draw d = {false, n, instances, 0, 0};
        if (indexed) {
            uint32_t isize = draw_index_size(g);
            d.indexed = true;
            d.index_addr = g->index_base + (guest_addr)args[2] * isize;
            d.index_size = isize;
        }
        draw(g, &d);
    }
}

/* Frame dump: one line per packet, register writes decoded to offsets. */
static void dump_packet(const uint32_t *p, uint32_t len, int depth) {
    uint32_t op = (p[0] >> 8) & 0xFF;
    char line[512];
    int n = snprintf(line, sizeof(line), "%*sop 0x%02x len %u:", depth * 2, "", op, len);
    uint32_t base = op == 0x69 ? 0xA000 : op == 0x76 || op == 0x9B ? 0x2C00 : op == 0x79 || op == 0x7A ? 0xC000 : 0;
    if (base && len >= 2) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, " reg 0x%04x =", base + (p[1] & 0xFFFF));
    }
    for (uint32_t i = base ? 2 : 1; i < len && n < (int)sizeof(line) - 12; i++) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %08x", p[i]);
    }
    LOG_I(GPU, "%s", line);
}

/* Frame dump of LOAD_*_REG_INDEX tables: `count` (offset, value) pairs in memory. */
static void dump_reg_table(gpu *g, const uint32_t *p, uint32_t len) {
    if (len < 5) return;
    guest_addr a = (guest_addr)(p[1] & ~3u) | ((guest_addr)p[2] << 32);
    uint32_t n = p[4];
    const uint32_t *t = (const uint32_t *)gmem_host(g->mem, a, (uint64_t)n * 8);
    if (!t) return;
    for (uint32_t i = 0; i < n; i++) LOG_I(GPU, "      [0x%04x] = %08x", t[2 * i], t[2 * i + 1]);
}

void gpu_write_driver_packet(uint32_t *dst, uint32_t dwords, const uint32_t *payload, uint32_t payload_dwords) {
    /* type 3, count = body dwords - 1, opcode NOP */
    dst[0] = 0xC0000000u | ((dwords - 2u) << 16) | (IT_NOP << 8);
    dst[1] = GPU_DRIVER_TAG;
    for (uint32_t i = 0; i + 2 < dwords; i++) dst[2 + i] = i < payload_dwords ? payload[i] : 0;
}

/* The command processor reads guest memory: GPU results and held releases
 * that write there land first ([0, ~0): everything held, interrupts too). */
static void cp_read(gpu *g, guest_addr a, uint64_t size) {
    if (g->cb.sync_mem) g->cb.sync_mem(g->cb.user, a, size);
}

/* The command processor writes guest memory now. On the hardware the end-of-pipe
 * releases ahead of it land at most a frame later; the backend may hold them
 * much longer (a whole queue behind), and a game that sees this write takes the
 * earlier work as done: the held releases land first. */
static void cp_write(gpu *g, const char *what, guest_addr addr, uint64_t size) {
    /* PS5C_LOG_CP_WRITES[=<seconds>]: the first 200 (from that many seconds into the run; debugging
     * aid: what makes held releases wait) */
    static int logging = -1;
    static uint32_t logged;
    static uint64_t from;
    if (logging < 0) {
        const char *e = getenv("PS5C_LOG_CP_WRITES");
        logging = e != NULL;
        from = e ? pal_time_ns() + strtoull(e, NULL, 10) * 1000000000ull : 0;
    }
    if (logging && logged < 200 && pal_time_ns() >= from) {
        logged++;
        LOG_I(GPU, "CP write: %s 0x%llx, %llu bytes", what, (unsigned long long)addr, (unsigned long long)size);
    }
    if (g->cb.cp_write) g->cb.cp_write(g->cb.user);
}

static void release(gpu *g, const gpu_release *rel, bool observable);

/* A small write the command processor makes (WRITE_DATA, DMA_DATA, COPY_DATA
 * up to CP_ORDERED_MAX bytes: labels, fences, indirect arguments) goes in order
 * with the releases the backend holds, as dword or qword labels, instead of the
 * whole batch completing first (Wolverine waited ~20 times a second, ~380 ms).
 * Whatever reads it before it lands waits as for a held label: the command
 * processor (cp_read), scalar evaluation and buffer bindings check them. Sonic
 * Frontiers resets the completion label it keeps inside each command buffer
 * this way. PS5C_CP_ORDERED_WRITES=0: only 4/8-byte writes inside a command
 * buffer being run (the old way); PS5C_NO_HELD_LABEL_WRITES: none. True when
 * it was taken (performed now or held). */
#define CP_ORDERED_MAX 64u
static bool cp_ordered_write(gpu *g, guest_addr addr, const void *src, uint32_t bytes) {
    static int off = -1, narrow = -1;
    if (off < 0) off = getenv("PS5C_NO_HELD_LABEL_WRITES") != NULL;
    if (narrow < 0) narrow = getenv("PS5C_CP_ORDERED_WRITES") && !atoi(getenv("PS5C_CP_ORDERED_WRITES"));
    if (off || !g->cb.release || !bytes || bytes > CP_ORDERED_MAX || (bytes & 3) || (addr & 3)) return false;
    if (narrow) {
        if ((bytes != 4 && bytes != 8) || (addr & (bytes - 1))) return false;
        bool inside = false;
        for (uint32_t i = 0; i < g->run_depth && !inside; i++) inside = addr >= g->run_lo[i] && addr + bytes <= g->run_hi[i];
        if (!inside) return false;
    }
    if (!gmem_host(g->mem, addr, bytes)) return false; /* unmapped: the caller reports it */
    /* the backend's caches of that memory read it again (after it lands: a read before waits for the batch) */
    if (g->cb.mem_written) g->cb.mem_written(g->cb.user, addr, bytes);
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t at = 0; at < bytes;) {
        bool wide = !((addr + at) & 7) && bytes - at >= 8;
        uint64_t v = 0;
        memcpy(&v, s + at, wide ? 8 : 4);
        gpu_release rel = {addr + at, v, wide ? 2u : 1u, false, 0, 0};
        release(g, &rel, true);
        at += wide ? 8 : 4;
    }
    return true;
}

static void write32(gpu *g, guest_addr a, uint32_t v) {
    if (!gmem_write(g->mem, a, &v, 4)) LOG_W(GPU, "write to unmapped 0x%llx", (unsigned long long)a);
}

static void write64(gpu *g, guest_addr a, uint64_t v) {
    if (!gmem_write(g->mem, a, &v, 8)) LOG_W(GPU, "write to unmapped 0x%llx", (unsigned long long)a);
}

static void driver_packet(gpu *g, const uint32_t *p, uint32_t len) {
    if (len < 3) return;
    switch (p[2]) {
    case GPU_DRIVER_FLIP:
        if (len >= GPU_FLIP_PACKET_DWORDS && g->cb.flip) {
            g->stats.flips++;
            /* PS5C_GPU_DUMP=@<file>: dump the frame after <file> appears (then it is removed) */
            const char *trigger = getenv("PS5C_GPU_DUMP");
            pal_path_info pi;
            if (trigger && trigger[0] == '@' && pal_path_stat(trigger + 1, &pi) == PS5C_OK) {
                pal_file_remove(trigger + 1);
                g->dump_frame = (int64_t)g->stats.flips;
                LOG_I(GPU, "dumping frame %lld", (long long)g->dump_frame);
            }
            /* the flip comes after the frame's work and its end-of-pipe events, as on the hardware */
            cp_read(g, 0, ~0ull);
            g->cb.flip(g->cb.user, (int32_t)p[3], (int32_t)p[4], (int32_t)p[5],
                       (int64_t)((uint64_t)p[6] | ((uint64_t)p[7] << 32)));
        }
        break;
    case GPU_DRIVER_WAIT_DISPLAY:
        /* the display reads the buffer at a later vblank: drawing the next frame into it before then shows a frame
         * half drawn (Dead Cells lost its menu layer every few frames). Other queues may submit meanwhile. */
        if (len >= GPU_WAIT_DISPLAY_PACKET_DWORDS && g->cb.wait_display) {
            cp_read(g, 0, ~0ull);
            pal_mutex_unlock(g->lock);
            g->cb.wait_display(g->cb.user, (int32_t)p[3], (int32_t)p[4]);
            pal_mutex_lock(g->lock);
        }
        break;
    default: break;
    }
}

/* PS5C_LOG_RELEASE: remembers timestamp labels and reports those read back as 0 */
static guest_addr g_ts_labels[4096], g_done_labels[4096];
static uint32_t g_ts_count, g_done_count;
void gpu_debug_check_labels(gpu *g);
void gpu_debug_check_labels(gpu *g) {
    uint32_t zero = 0;
    for (uint32_t i = 0; i < g_ts_count; i++) {
        uint64_t v = 1;
        gmem_read(g->mem, g_ts_labels[i], &v, 8);
        if (!v && zero++ < 8) LOG_I(GPU, "timestamp label 0x%llx is 0 again", (unsigned long long)g_ts_labels[i]);
    }
    LOG_I(GPU, "%u timestamp labels, %u read 0", g_ts_count, zero);
    uint32_t set = 0;
    for (uint32_t i = 0; i < g_done_count; i++) {
        uint64_t v = 0;
        gmem_read(g->mem, g_done_labels[i], &v, 8);
        if (v && set++ < 8) LOG_I(GPU, "completion label 0x%llx = 0x%llx", (unsigned long long)g_done_labels[i], (unsigned long long)v);
    }
    LOG_I(GPU, "%u completion labels, %u not 0", g_done_count, set);
}

/* RELEASE_MEM (GFX10): dw1 event, dw2 dst_sel[17:16] int_sel[26:24] data_sel[31:29],
 * dw3/4 address, dw5/6 data, dw7 interrupt context id. */
/* End-of-pipe: results GPU work left in host buffers reach guest memory
 * before the label the CPU waits on is written. */
static void end_of_pipe(gpu *g) {
    if (g->cb.sync_mem) g->cb.sync_mem(g->cb.user, 0, ~0ull);
}

/* The last command-processor write to each (hashed) address: a queue blocked on a
 * label reports who advanced it last (lib_agc_driver.c). */
typedef struct cp_last_write {
    guest_addr addr;
    uint64_t value, ns;
    const char *what;
    int32_t queue_event;
} cp_last_write;
static cp_last_write g_last_writes[4096];

static void note_write(guest_addr a, uint64_t value, const char *what, int32_t queue_event) {
    cp_last_write *w = &g_last_writes[(a >> 3) & 4095];
    w->addr = a & ~7ull;
    w->value = value;
    w->ns = pal_time_ns();
    w->what = what;
    w->queue_event = queue_event;
}

bool gpu_last_write(guest_addr a, char *out, size_t size) {
    const cp_last_write *w = &g_last_writes[(a >> 3) & 4095];
    if (!w->what || w->addr != (a & ~7ull)) return false;
    snprintf(out, size, "%s value 0x%llx (queue event %d) %.1f s ago", w->what, (unsigned long long)w->value,
             w->queue_event, (double)(pal_time_ns() - w->ns) / 1e9);
    return true;
}

void gpu_perform_release(gpu *g, const gpu_release *rel) {
    if (rel->data_sel >= 1 && rel->data_sel <= 4) note_write(rel->addr, rel->data, "RELEASE_MEM", rel->queue_event);
    switch (rel->data_sel) {
    case 1: write32(g, rel->addr & ~3ull, (uint32_t)rel->data); break;
    case 2: write64(g, rel->addr & ~7ull, rel->data); break;
    case 3:
    case 4: write64(g, rel->addr & ~7ull, pal_time_ns()); break;
    default: break;
    }
    if (rel->interrupt && g->cb.interrupt) {
        g->stats.interrupts++;
        g->cb.interrupt(g->cb.user, rel->queue_event, rel->context_id);
    }
}

/* An end-of-pipe release. One the CPU can observe (a label or an interrupt)
 * must not come before the results of the work ahead of it: the backend holds
 * it until that work completes, or (no backend hook) the work is waited for
 * here. Others (GPU clock stores without interrupt) happen now. */
static void release(gpu *g, const gpu_release *rel, bool observable) {
    if (rel->interrupt && g->active_submission) g->active_submission->had_interrupt = true;
    if (rel->interrupt) g->interrupts_issued++;
    if (observable && g->cb.release && g->cb.release(g->cb.user, g, rel)) return;
    if (observable && !g->cb.release) end_of_pipe(g);
    gpu_perform_release(g, rel);
}

/* PS5C_LOG_GPU_ADDR=<hex>: command-processor writes (RELEASE_MEM, WRITE_DATA,
 * DMA_DATA) that reach the 64 bytes from that address (debugging aid: who
 * advances a fence a thread waits on) */
static bool addr_logged(guest_addr a, uint64_t size) {
    static int have = -1;
    static guest_addr want;
    if (have < 0) {
        const char *e = getenv("PS5C_LOG_GPU_ADDR");
        have = e != NULL;
        want = e ? strtoull(e, NULL, 16) : 0;
    }
    if (have && a < want + 64 && want < a + (size ? size : 1)) return true;
    /* PS5C_LOG_GPU_LOW16=<hex>: writes covering an address with those low 16 bits (a ring of
     * per-frame buffers that all start at the same offset in their 64 KB) */
    static int low = -1;
    static uint32_t low16;
    if (low < 0) {
        low = getenv("PS5C_LOG_GPU_LOW16") != NULL;
        low16 = low ? (uint32_t)strtoul(getenv("PS5C_LOG_GPU_LOW16"), NULL, 16) & 0xFFFF : 0;
    }
    if (!low || !size) return false;
    guest_addr c = (a & ~(guest_addr)0xFFFF) | low16;
    if (c < a) c += 0x10000;
    return c < a + size;
}

static bool log_release_enabled(void) {
    static int on = -1;
    if (on < 0) on = getenv("PS5C_LOG_RELEASE") != NULL;
    return on != 0;
}

static void release_mem(gpu *g, const uint32_t *p, uint32_t len, int32_t queue_event) {
    if (len < 8) return;
    g->stats.releases++;
    uint32_t data_sel = p[2] >> 29, int_sel = (p[2] >> 24) & 7;
    /* The CPU only learns that work finished through a label or an interrupt:
     * a release with neither, or one that only stores the GPU clock (profiling
     * markers: Astro Bot writes thousands per second), needs no GPU results in
     * guest memory, so it does not wait for the GPU. */
    bool observable = (data_sel == 1 || data_sel == 2) || (int_sel != 0 && int_sel != 3);
    guest_addr addr = (guest_addr)p[3] | ((guest_addr)p[4] << 32);
    uint64_t data = (uint64_t)p[5] | ((uint64_t)p[6] << 32);
    if (addr_logged(addr, 8))
        LOG_I(GPU, "GPU write: RELEASE_MEM data_sel %u int_sel %u addr 0x%llx data 0x%llx (queue event %d)", data_sel, int_sel,
              (unsigned long long)addr, (unsigned long long)data, queue_event);
    if (log_release_enabled()) {
        LOG_I(GPU, "RELEASE_MEM ev 0x%x data_sel %u int_sel %u addr 0x%llx data 0x%llx ctx 0x%x qe %d", p[1], data_sel,
              int_sel, (unsigned long long)addr, (unsigned long long)data, p[7], queue_event);
        bool known = false;
        if (data_sel == 2 && data == 0) {
            for (uint32_t i = 0; i < g_done_count && !known; i++) known = g_done_labels[i] == (addr & ~7ull);
            if (!known && g_done_count < 4096) g_done_labels[g_done_count++] = addr & ~7ull;
        } else if (data_sel == 3) {
            for (uint32_t i = 0; i < g_ts_count && !known; i++) known = g_ts_labels[i] == (addr & ~7ull);
            if (!known && g_ts_count < 4096) g_ts_labels[g_ts_count++] = addr & ~7ull;
        }
    }
    if (data_sel > 4) LOG_W(GPU, "RELEASE_MEM data_sel %u not supported", data_sel);
    gpu_release rel = {addr, data, data_sel <= 4 ? data_sel : 0, int_sel != 0 && int_sel != 3, queue_event, p[7]};
    release(g, &rel, observable);
}

/* EVENT_WRITE_EOP (older form): dw1 event, dw2 addr lo, dw3 addr hi[15:0] data_sel[31:29] int_sel[25:24],
 * dw4/5 data. */
static void event_write_eop(gpu *g, const uint32_t *p, uint32_t len, int32_t queue_event) {
    if (len < 6) {
        end_of_pipe(g);
        return;
    }
    g->stats.releases++;
    uint32_t data_sel = p[3] >> 29, int_sel = (p[3] >> 24) & 3;
    guest_addr addr = (guest_addr)p[2] | ((guest_addr)(p[3] & 0xFFFF) << 32);
    uint64_t data = (uint64_t)p[4] | ((uint64_t)p[5] << 32);
    gpu_release rel = {addr, data, data_sel <= 3 ? data_sel : 0, int_sel != 0, queue_event, 0};
    release(g, &rel, true);
}

/* COPY_DATA: dw1 src_sel[3:0] dst_sel[11:8] count_sel[16] (64-bit), dw2/3 src (address or
 * immediate), dw4/5 dst. Memory, immediate and GPU clock sources to memory. */
static void copy_data(gpu *g, const uint32_t *p, uint32_t len) {
    if (len < 6) return;
    uint32_t src_sel = p[1] & 0xF, dst_sel = (p[1] >> 8) & 0xF;
    bool wide = (p[1] >> 16) & 1;
    guest_addr src = (guest_addr)p[2] | ((guest_addr)p[3] << 32);
    guest_addr dst = (guest_addr)p[4] | ((guest_addr)p[5] << 32);
    if (dst_sel != 1 && dst_sel != 2 && dst_sel != 5) return; /* registers, GDS, perf counters */
    uint64_t v = 0;
    switch (src_sel) {
    case 1:
    case 2: /* memory / TC L2 */
        cp_read(g, src, wide ? 8 : 4);
        gmem_read(g->mem, src, &v, wide ? 8 : 4);
        break;
    case 5: v = src; break;                                  /* immediate */
    case 9:
    case 10: v = pal_time_ns(); break;                       /* GPU / reference clock */
    default: return;
    }
    if (cp_ordered_write(g, dst & ~(wide ? 7ull : 3ull), &v, wide ? 8 : 4)) return;
    cp_write(g, "COPY_DATA", dst, wide ? 8 : 4);
    if (wide) write64(g, dst & ~7ull, v);
    else write32(g, dst & ~3ull, (uint32_t)v);
    if (g->cb.mem_written) g->cb.mem_written(g->cb.user, dst & ~3ull, wide ? 8 : 4);
}

/* ATOMIC_MEM: dw1 atomic[6:0] (TC op: bit 5 = 64-bit, bit 6 = no return), dw2/3 address,
 * dw4/5 source, dw6/7 compare. */
static void atomic_mem(gpu *g, const uint32_t *p, uint32_t len) {
    if (len < 8) return;
    uint32_t op = p[1] & 0x1F;
    bool wide = (p[1] >> 5) & 1;
    guest_addr a = (guest_addr)p[2] | ((guest_addr)p[3] << 32);
    uint64_t src = (uint64_t)p[4] | ((uint64_t)p[5] << 32), cmp = (uint64_t)p[6] | ((uint64_t)p[7] << 32);
    uint64_t v = 0;
    cp_read(g, a, wide ? 8 : 4);
    cp_write(g, "ATOMIC_MEM", a, wide ? 8 : 4);
    if (!gmem_read(g->mem, a, &v, wide ? 8 : 4)) return;
    switch (op) {
    case 7: v = src; break;              /* swap */
    case 8: if (v == cmp) v = src; break; /* compare-swap */
    case 9: v += src; break;             /* add */
    case 10: v -= src; break;            /* sub */
    case 15: v &= src; break;            /* and */
    case 16: v |= src; break;            /* or */
    case 17: v ^= src; break;            /* xor */
    case 18: v = v >= src ? 0 : v + 1; break; /* inc (wrap) */
    case 19: v = (v == 0 || v > src) ? src : v - 1; break; /* dec (wrap) */
    default:
        LOG_W(GPU, "ATOMIC_MEM op 0x%x not supported", p[1] & 0x7F);
        return;
    }
    gmem_write(g->mem, a, &v, wide ? 8 : 4);
    note_write(a, v, "ATOMIC_MEM", -1);
    if (g->cb.mem_written) g->cb.mem_written(g->cb.user, a, wide ? 8 : 4);
}

/* WRITE_DATA: dw1 dst_sel[11:8], dw2/3 address, dw4.. data */
static void write_data(gpu *g, const uint32_t *p, uint32_t len) {
    if (len < 5) return;
    uint32_t dst_sel = (p[1] >> 8) & 0xF;
    if (dst_sel != 1 && dst_sel != 2 && dst_sel != 5) return; /* registers, GDS, ... */
    guest_addr addr = (guest_addr)p[2] | ((guest_addr)p[3] << 32);
    if (addr_logged(addr, (uint64_t)(len - 4) * 4))
        LOG_I(GPU, "GPU write: WRITE_DATA addr 0x%llx first dword 0x%x (%u dwords)", (unsigned long long)addr, p[4], len - 4);
    note_write(addr, len >= 6 ? (uint64_t)p[4] | (uint64_t)p[5] << 32 : p[4], "WRITE_DATA", -1);
    if (cp_ordered_write(g, addr, p + 4, (len - 4) * 4)) return;
    cp_write(g, "WRITE_DATA", addr, (uint64_t)(len - 4) * 4);
    if (!gmem_write(g->mem, addr, p + 4, (uint64_t)(len - 4) * 4)) {
        LOG_W(GPU, "WRITE_DATA to unmapped 0x%llx", (unsigned long long)addr);
    } else if (g->cb.mem_written) {
        g->cb.mem_written(g->cb.user, addr, (uint64_t)(len - 4) * 4);
    }
}

/* DMA_DATA: dw1 src_sel[30:29] dst_sel[21:20], dw2/3 src (or data), dw4/5 dst, dw6 byte_count[25:0] */
static void dma_data(gpu *g, const uint32_t *p, uint32_t len) {
    if (len < 7) return;
    uint32_t src_sel = (p[1] >> 29) & 3, dst_sel = (p[1] >> 20) & 3;
    guest_addr src = (guest_addr)p[2] | ((guest_addr)p[3] << 32);
    guest_addr dst = (guest_addr)p[4] | ((guest_addr)p[5] << 32);
    uint32_t count = p[6] & 0x03FFFFFF;
    if (addr_logged(dst, count))
        LOG_I(GPU, "GPU write: DMA_DATA dst 0x%llx src 0x%llx (sel %u) %u bytes", (unsigned long long)dst, (unsigned long long)src,
              src_sel, count);
    /* sel 0 = memory, 3 = memory through the L2 cache (same memory here), 1 = GDS
     * (guest memory at sh_gds_base); source 2 = fill with the dword in dw2 */
    if (dst_sel == 2 || count == 0) return;
    note_write(dst, src_sel == 2 ? p[2] : src, src_sel == 2 ? "DMA_DATA fill" : "DMA_DATA copy from", -1);
    if (dst_sel == 1 || src_sel == 1) {
        if (!sh_gds_base) return;
        if (dst_sel == 1) dst = sh_gds_base + (dst & (SH_GDS_SIZE - 1));
        if (src_sel == 1) {
            src = sh_gds_base + (src & (SH_GDS_SIZE - 1));
        }
    }
    if (src_sel != 2) cp_read(g, src, count); /* shaders may still hold it */
    if (log_release_enabled()) LOG_I(GPU, "DMA_DATA src_sel %u 0x%llx -> 0x%llx, %u bytes", src_sel, (unsigned long long)src, (unsigned long long)dst, count);
    if (dst_sel != 1 && count <= CP_ORDERED_MAX) {
        uint32_t data[CP_ORDERED_MAX / 4];
        bool have = true;
        if (src_sel == 2) for (uint32_t i = 0; i < count / 4; i++) data[i] = p[2];
        else have = count % 4 == 0 && gmem_read(g->mem, src, data, count);
        if (have && cp_ordered_write(g, dst, data, count)) return;
    }
    uint8_t *d = (uint8_t *)gmem_host(g->mem, dst, count);
    if (!d) return;
    cp_write(g, "DMA_DATA", dst, count);
    if (src_sel != 2) {
        const uint8_t *s = (const uint8_t *)gmem_host(g->mem, src, count);
        if (s) memmove(d, s, count);
    } else if (src_sel == 2) { /* fill with a dword */
        for (uint32_t i = 0; i + 4 <= count; i += 4) memcpy(d + i, &p[2], 4);
    }
    if (g->cb.mem_written) g->cb.mem_written(g->cb.user, dst, count);
}

/* CLEAR_STATE: dw1 cmd[3:0]: 1 PUSH_STATE saves the context registers, 2
 * POP_STATE brings them back (Astro Bot wraps passes in them: without the
 * pop, a depth-only pass left color writes off for the frame's last draws).
 * 0 (back to the clear-state defaults) is not done: the defaults are the
 * kernel driver's table. */
#define CTX_STACK_DEPTH 8
#define CTX_REGS 0x400u

static void clear_state(gpu *g, const uint32_t *p, uint32_t len) {
    uint32_t cmd = len > 1 ? p[1] & 0xF : 0;
    if (cmd == 1 || cmd == 3) {
        if (!g->ctx_stack) g->ctx_stack = (uint32_t *)malloc(sizeof(uint32_t) * CTX_REGS * CTX_STACK_DEPTH);
        if (!g->ctx_stack || g->ctx_depth == CTX_STACK_DEPTH) {
            static bool warned;
            if (!warned) LOG_W(GPU, "PUSH_STATE deeper than %u", CTX_STACK_DEPTH);
            warned = true;
            return;
        }
        memcpy(g->ctx_stack + (size_t)g->ctx_depth++ * CTX_REGS, g->regs + GPU_REG_CONTEXT_BASE, CTX_REGS * 4);
    } else if ((cmd == 2 || cmd == 4) && g->ctx_depth) {
        memcpy(g->regs + GPU_REG_CONTEXT_BASE, g->ctx_stack + (size_t)--g->ctx_depth * CTX_REGS, CTX_REGS * 4);
    }
}

static int g_op_timing = -1;
static uint64_t g_op_ns[256];

/* Observe unimplemented command-stream waits without changing their ordering.
 * First eight and powers of two per opcode keep a stalled game bounded. */
static void trace_cp_wait(gpu *g, const uint32_t *p, uint32_t len, guest_addr packet) {
    static int enabled = -1;
    static uint64_t counts[3];
    if (enabled < 0) enabled = getenv("PS5C_TRACE_CP_WAIT") != NULL;
    if (!enabled) return;
    uint32_t op = (p[0] >> 8) & 255;
    uint32_t slot = op == 0x3c ? 0 : op == 0x93 ? 1 : 2;
    uint64_t n = ++counts[slot];
    static int all = -1; /* PS5C_TRACE_CP_WAIT=all: every wait */
    if (all < 0) all = !strcmp(getenv("PS5C_TRACE_CP_WAIT"), "all");
    if (!all && n > 8 && (n & (n - 1))) return;
    uint64_t value = 0, ref = 0, mask = 0;
    guest_addr a = 0;
    bool mapped = false;
    if ((op == 0x3c && len == 7) || (op == 0x93 && len == 9)) {
        bool wide = op == 0x93;
        a = (guest_addr)(p[2] & (wide ? ~7u : ~3u)) | ((guest_addr)(p[3] & 0x3ffffu) << 32);
        ref = wide ? (uint64_t)p[4] | ((uint64_t)p[5] << 32) : p[4];
        mask = wide ? (uint64_t)p[6] | ((uint64_t)p[7] << 32) : p[5];
        /* No sync_mem here: tracing must not submit, wait or copy GPU work. */
        if (p[1] & 0x10) mapped = gmem_read(g->mem, a, &value, wide ? 8 : 4);
    }
    LOG_I(GPU, "CP wait probe op=%02x n=%llu flip=%llu packet=0x%llx ctrl=%08x addr=0x%llx "
               "mapped=%u value=%016llx ref=%016llx mask=%016llx", op, (unsigned long long)n,
          (unsigned long long)g->stats.flips, (unsigned long long)packet, len > 1 ? p[1] : 0,
          (unsigned long long)a, mapped, (unsigned long long)value, (unsigned long long)ref,
          (unsigned long long)mask);
}

static void run_packets(gpu *g, guest_addr addr, uint32_t dwords, int32_t queue_event, int depth,
                        bool *legacy_fault);

static void run(gpu *g, guest_addr addr, uint32_t dwords, int32_t queue_event, int depth,
                bool *legacy_fault) {
    gpu_submission *s = g->active_submission;
    if (s) {
        if ((uint32_t)depth >= GPU_IB_FRAMES) {
            LOG_E(GPU, "CP resume depth %d exceeds %u frames at 0x%llx (%u dwords)",
                  depth, GPU_IB_FRAMES, (unsigned long long)addr, dwords);
            s->fault = true; return;
        }
        if (s->depth <= (uint32_t)depth) {
            s->frame[depth].addr = addr;
            s->frame[depth].dwords = dwords;
            s->frame[depth].pos = 0;
            s->depth = (uint32_t)depth + 1;
        }
        addr = s->frame[depth].addr;
        dwords = s->frame[depth].dwords;
    }
    uint32_t level = g->run_depth;
    if (level < PS5C_ARRAY_COUNT(g->run_lo)) {
        g->run_lo[level] = addr;
        g->run_hi[level] = addr + (guest_addr)dwords * 4;
        g->run_depth = level + 1;
    }
    /* Resume the child first; the parent already points past its call packet. */
    if (s && s->depth > (uint32_t)depth + 1) {
        run(g, s->frame[depth + 1].addr, s->frame[depth + 1].dwords, queue_event, depth + 1,
            legacy_fault);
    }
    if (!s || (!s->blocked && !s->yielded && !s->fault))
        run_packets(g, addr, dwords, queue_event, depth, legacy_fault);
    if (s && !s->blocked && !s->yielded && !s->fault) s->depth = (uint32_t)depth;
    g->run_depth = level;
}

static void run_packets(gpu *g, guest_addr addr, uint32_t dwords, int32_t queue_event, int depth,
                        bool *legacy_fault) {
    gpu_submission *s = g->active_submission;
    uint32_t chain_hops = 0;
    if (!dwords) return;
    const uint32_t *cmd = (const uint32_t *)gmem_host(g->mem, addr, (uint64_t)dwords * 4);
    if (!cmd) {
        if (s) s->fault = true;
        LOG_W(GPU, "command buffer 0x%llx (%u dwords) not mapped", (unsigned long long)addr, dwords);
        return;
    }
    for (uint32_t pos = s ? s->frame[depth].pos : 0; pos < dwords;) {
        if (legacy_fault && *legacy_fault)
            return;
        if (s) {
            s->frame[depth].pos = pos;
            if (!s->budget) { s->yielded = true; return; }
            s->budget--;
        }
        uint32_t hdr = cmd[pos];
        uint32_t type = hdr >> 30;
        if (type == 2) { /* filler */
            pos++;
            continue;
        }
        uint32_t len = ((hdr >> 16) & 0x3FFF) + 2;
        if (hdr == 0xFFFF1000u) {
            /* NOP with the maximum count: libSceAgc leaves it as a marker
             * placeholder with no body; real packets follow directly. */
            pos++;
            continue;
        }
        if (pos + len > dwords) {
            /* the FIFO path ends the buffer here; the resumable one did the same only when the
             * header is a type 3 packet (No Man's Sky leaves a type 0 word in the last dword) */
            if (s && type == 3) s->fault = true;
            else if (s) s->frame[depth].pos = dwords;
            LOG_W(GPU, "packet 0x%08x at dword %u overruns the buffer 0x%llx (%u dwords)", hdr, pos,
                  (unsigned long long)addr, dwords);
            static int all = -1; /* PS5C_LOG_OVERRUN=1: the whole buffer */
            if (all < 0) all = getenv("PS5C_LOG_OVERRUN") != NULL;
            for (uint32_t k = 0; k + 8 <= dwords && k < (all ? dwords : 24); k += 8) {
                LOG_W(GPU, "  %08x %08x %08x %08x %08x %08x %08x %08x", cmd[k], cmd[k + 1], cmd[k + 2], cmd[k + 3],
                      cmd[k + 4], cmd[k + 5], cmd[k + 6], cmd[k + 7]);
            }
            return;
        }
        const uint32_t *p = cmd + pos;
        pos += len;
        if (s) s->frame[depth].pos = pos;
        if (type != 3) continue; /* type 0: register writes */
        g->stats.packets++;
        uint32_t op = (hdr >> 8) & 0xFF;
        if (op == 0x3c || op == 0x93 || op == 0x59) trace_cp_wait(g, p, len, addr + (uint64_t)(pos - len) * 4);
        uint64_t t_op = g_op_timing ? pal_time_ns() : 0;
        if (dumping(g)) {
            static int dump_pkts = -1;
            if (dump_pkts < 0) dump_pkts = getenv("PS5C_GPU_DUMP_PACKETS") != NULL;
            if (dump_pkts) {
                dump_packet(p, len, depth);
                if (op == 0x9F || op == 0x63 || op == 0x64) dump_reg_table(g, p, len);
            }
        }
        { /* PS5C_LOG_GDS: packets that touch GDS (debugging aid) */
            static int log_gds = -1;
            static uint32_t logged;
            if (log_gds < 0) log_gds = getenv("PS5C_LOG_GDS") != NULL;
            bool gds = (op == 0x50 && (((p[1] >> 29) & 3) == 1 || ((p[1] >> 20) & 3) == 1)) ||
                       (op == 0x40 && ((p[1] & 0xF) == 3 || ((p[1] >> 8) & 0xF) == 3)) ||
                       (op == 0x37 && ((p[1] >> 8) & 0xF) == 3) || op == 0x48 || op == 0x1D || op == 0x1E;
            if (log_gds && gds && logged++ < 200)
                LOG_I(GPU, "GDS packet 0x%02x (%u dwords): %08x %08x %08x %08x %08x %08x %08x", op, len, len > 1 ? p[1] : 0,
                      len > 2 ? p[2] : 0, len > 3 ? p[3] : 0, len > 4 ? p[4] : 0, len > 5 ? p[5] : 0, len > 6 ? p[6] : 0,
                      len > 7 ? p[7] : 0);
        }
        switch (op) {
        case IT_MEM_SEMAPHORE: {
            /* Resumable queues park on zero and let the signalling queue run.
             * The FIFO executor cannot suspend a packet; preserve its policy. */
            if (!s) goto unknown_packet;
            bool ready = false;
            if (len != 4 || (p[3] & ~0xe0110000u) || (p[3] >> 29) < 6) s->fault = true;
            else {
                guest_addr a = (guest_addr)(p[1] & ~7u) | ((guest_addr)(p[2] & 0x3ffffu) << 32);
                bool signal = (p[3] >> 29) == 6, write = (p[3] & 0x100000u) != 0;
                cp_read(g, a, 8);
                cp_write(g, "MEM_SEMAPHORE", a, 8);
                /* Start with a valid candidate, then retry against the value
                 * observed by CAS. No borrowed host pointer crosses a remap,
                 * and concurrent guest CPU updates cannot be lost. */
                uint64_t value = signal ? 0 : 1;
                do {
                    uint64_t desired = signal ? (write ? 1 : value + 1) : value - 1;
                    bool exchanged;
                    if (!gmem_compare_exchange_u64(g->mem, a, &value, desired, &exchanged)) {
                        s->fault = true;
                        break;
                    }
                    if (exchanged) {
                        ready = true;
                        if (g->cb.mem_written) g->cb.mem_written(g->cb.user, a, 8);
                        break;
                    }
                } while (signal || value != 0);
            }
            if (s->fault || !ready) {
                s->blocked = !s->fault;
                s->frame[depth].pos = pos - len;
                s->wait_packet = addr + (uint64_t)(pos - len) * 4;
                s->wait_opcode = op;
                return;
            }
            break;
        }
        case IT_REWIND:
        case IT_WAIT_REG_MEM:
        case IT_WAIT_REG_MEM_64:
            if (!s) goto unknown_packet;
            if (s) {
                bool ready = false;
                if (op == IT_REWIND) {
                    if (len != 2 || (p[1] & ~0x81000000u)) s->fault = true;
                    else ready = (__atomic_load_n(p + 1, __ATOMIC_ACQUIRE) & 0x80000000u) != 0;
                } else {
                    bool wide = op == IT_WAIT_REG_MEM_64;
                    uint32_t ctrl = p[1], fn = ctrl & 7;
                    uint32_t operation = wide ? ((ctrl >> 8) & 1) | ((ctrl >> 5) & 6)
                                              : ((ctrl >> 8) & 3) | ((ctrl >> 4) & 12);
                    if (len != (wide ? 9u : 7u) || operation > 1 || fn > 6) s->fault = true;
                    else {
                        uint64_t ref = wide ? (uint64_t)p[4] | ((uint64_t)p[5] << 32) : p[4];
                        uint64_t mask = wide ? (uint64_t)p[6] | ((uint64_t)p[7] << 32) : p[5];
                        uint64_t value = 0;
                        if (ctrl & 0x10) {
                            guest_addr a = (guest_addr)(p[2] & (wide ? ~7u : ~3u)) | ((guest_addr)(p[3] & 0x3ffffu) << 32);
                            /* address 0 (No Man's Sky's first queue, Dragon Ball): nothing to poll and never
                             * filled in later (waiting hangs the queue); satisfied, as the FIFO path skips it */
                            if (!a) value = ref, fn = 3, mask = ~0ull;
                            else {
                                cp_read(g, a, wide ? 8 : 4);
                                if (!gmem_read(g->mem, a, &value, wide ? 8 : 4)) s->fault = true;
                            }
                        } else if (p[2] < GPU_REG_COUNT - (wide ? 1u : 0u)) {
                            value = g->regs[p[2]];
                            if (wide) value |= (uint64_t)g->regs[p[2] + 1] << 32;
                        } else s->fault = true;
                        value &= mask;
                        ready = fn == 0 ? true : fn == 1 ? value < ref : fn == 2 ? value <= ref : fn == 3 ? value == ref
                              : fn == 4 ? value != ref : fn == 5 ? value >= ref : value > ref;
                    }
                }
                if (s->fault) /* the packet the resumable executor refuses (debugging aid) */
                    LOG_E(GPU, "CP wait packet 0x%02x refused (%u dwords): %08x %08x %08x %08x %08x %08x %08x %08x", op, len,
                          p[0], len > 1 ? p[1] : 0, len > 2 ? p[2] : 0, len > 3 ? p[3] : 0, len > 4 ? p[4] : 0,
                          len > 5 ? p[5] : 0, len > 6 ? p[6] : 0, len > 7 ? p[7] : 0);
                if (s->fault || !ready) {
                    s->blocked = !s->fault;
                    s->frame[depth].pos = pos - len;
                    s->wait_packet = addr + (uint64_t)(pos - len) * 4;
                    s->wait_opcode = op;
                    return;
                }
            }
            break;
        case 0x8e: { /* GET_LOD_STATS: byte count, aligned destination, report/reset flags */
            if(len!=5 || !g->cb.lod_stats || !g->cb.lod_stats(g->cb.user,NULL,0,0))goto unknown_packet;
            uint32_t bytes=p[1],flags=p[4];
            guest_addr dst=(guest_addr)(p[2]&~63u)|((guest_addr)p[3]<<32);
            if(bytes>GPU_LOD_REPORT_BYTES || (dst && bytes && !gmem_host(g->mem,dst,bytes))) {
                static unsigned warned;if(warned++<8)LOG_W(GPU,"LOD report rejected: 0x%llx +%u bytes",(unsigned long long)dst,bytes);
                break;
            }
            uint8_t report[GPU_LOD_REPORT_BYTES];
            if(dst && bytes)cp_write(g,"GET_LOD_STATS",dst,bytes);
            if(!g->cb.lod_stats(g->cb.user,report,dst?bytes:0,flags))break;
            if(dst && bytes) {
                /* Publish counters before the completion word. Never expose
                 * ready=1 while the report still contains an older frame. */
                uint32_t ready=0;if(bytes>=4){memcpy(&ready,report,4);memset(report,0,4);}
                if(!gmem_write(g->mem,dst,report,bytes))break;
                if(g->cb.mem_written)g->cb.mem_written(g->cb.user,dst,bytes);
                if(bytes>=4){atomic_thread_fence(memory_order_release);write32(g,dst,ready);}
            }
            break;
        }
        case IT_NOP:
            if (len >= 3 && p[1] == GPU_DRIVER_TAG) driver_packet(g, p, len);
            break;
        case IT_CLEAR_STATE: clear_state(g, p, len); break;
        case IT_WRITE_DATA: write_data(g, p, len); break;
        case IT_RELEASE_MEM: release_mem(g, p, len, queue_event); break;
        case IT_EVENT_WRITE: {
            /* PIXEL_PIPE_STAT_DUMP: one 64-bit cumulative result per DB,
             * interleaved with the other half of the begin/end pair. The
             * last write ends at +248, never the label at +256 from a pair. */
            if(len!=4 || (p[1]&0x7ff)!=0x139 || !g->cb.sample_counter ||
               !g->cb.sample_counter(g->cb.user,NULL))goto unknown_packet;
            guest_addr dst=(guest_addr)p[2]|((guest_addr)p[3]<<32);
            if(!dst || (dst&7) || !gmem_host(g->mem,dst,248))break;
            cp_write(g,"PIXEL_PIPE_STAT_DUMP",dst,248);
            uint64_t count=0;
            if(!g->cb.sample_counter(g->cb.user,&count))break;
            atomic_thread_fence(memory_order_release);
            for(uint32_t db=0;db<16;db++) {
                /* Host Vulkan exposes a total rather than physical RBs.
                 * Put the total in RB0; the other ready counters are zero.
                 * Summed begin/end differences retain the actual result. */
                uint64_t v=(UINT64_C(1)<<63)|(db?0:count&INT64_MAX);
                if(gmem_write(g->mem,dst+16*db,&v,8) && g->cb.mem_written)
                    g->cb.mem_written(g->cb.user,dst+16*db,8);
            }
            break;
        }
        case IT_EVENT_WRITE_EOP: event_write_eop(g, p, len, queue_event); break;
        case IT_DMA_DATA: dma_data(g, p, len); break;
        case IT_COPY_DATA: copy_data(g, p, len); break;
        case IT_ATOMIC_MEM: atomic_mem(g, p, len); break;
        case IT_COND_EXEC:
            /* dw1/2 address, dw4 exec count: skip that many dwords when *address == 0 */
            if (len >= 5) {
                uint32_t v = 1;
                cp_read(g, (guest_addr)(p[1] & ~3u) | ((guest_addr)p[2] << 32), 4);
                gmem_read(g->mem, (guest_addr)(p[1] & ~3u) | ((guest_addr)p[2] << 32), &v, 4);
                if (!v) pos += PS5C_MIN(p[4] & 0x3FFF, dwords - pos);
            }
            break;
        case IT_INDIRECT_BUFFER:
        case IT_INDIRECT_BUFFER_CNST: {
            guest_addr ib;
            uint32_t ib_dwords;
            bool chained;
            if (op == IT_INDIRECT_BUFFER && len == 14) {
                /* A conditional IB branches to another command block at this
                 * IB level. Treating it as CALL grows a fictitious return
                 * stack across an if/else/loop (AMD PAL GfxCmdStream chains).
                 * ALWAYS does not fetch or synchronize a compare address. */
                uint32_t mode = p[1] & 3, fn = (p[1] >> 8) & 7;
                uint64_t v = 0, mask = (uint64_t)p[4] | ((uint64_t)p[5] << 32);
                uint64_t ref = (uint64_t)p[6] | ((uint64_t)p[7] << 32);
                if (fn) {
                    guest_addr compare = (guest_addr)(p[2] & ~3u) | ((guest_addr)p[3] << 32);
                    cp_read(g, compare, 8);
                    gmem_read(g->mem, compare, &v, 8);
                }
                v &= mask;
                bool take = fn == 0 ? true : fn == 1 ? v < ref : fn == 2 ? v <= ref : fn == 3 ? v == ref
                          : fn == 4 ? v != ref : fn == 5 ? v >= ref : fn == 6 ? v > ref : false;
                uint32_t target = take ? 8 : 11;
                if (!take && mode != 2) break;
                ib = (guest_addr)(p[target] & ~3u) | ((guest_addr)p[target + 1] << 32);
                ib_dwords = p[target + 2] & 0xFFFFF;
                if (!ib_dwords) break; /* empty conditional target: no commands */
                /* At the end of its block (padding at most) it is a tail chain; with commands
                 * after it, the selected block is called and they run when it ends (Halloween's
                 * RHI calls a second pipe's chain mid-block, then releases the frame's labels:
                 * jumping skipped those releases and its graphics queue waited for ever). */
                chained = !packets_after(cmd, pos, dwords);
            } else {
                if (len < 4) break;
                ib = (guest_addr)(p[1] & ~3u) | ((guest_addr)(p[2] & 0xFFFF) << 32);
                ib_dwords = p[3] & 0xFFFFF;
                chained = (p[3] & 0x100000u) != 0;
            }
            if (chained) {
                /* A tail jump replaces this frame; WAIT/YIELD must resume in
                 * the selected block and never replay its abandoned tail. */
                if (!s && chain_hops++ == MAX_LEGACY_CHAIN_HOPS) {
                    LOG_E(GPU, "legacy IB chain exceeded %u tail jumps at 0x%llx",
                          MAX_LEGACY_CHAIN_HOPS, (unsigned long long)addr);
                    *legacy_fault = true;
                    return;
                }
                addr = ib;
                dwords = ib_dwords;
                if (s) {
                    s->frame[depth].addr = addr;
                    s->frame[depth].dwords = dwords;
                    s->frame[depth].pos = 0;
                }
                g->run_lo[depth] = addr;
                g->run_hi[depth] = addr + (uint64_t)dwords * 4;
                if (!dwords) return;
                cmd = (const uint32_t *)gmem_host(g->mem, addr, (uint64_t)dwords * 4);
                if (!cmd) {
                    if (s) s->fault = true;
                    if (legacy_fault) *legacy_fault = true;
                    LOG_W(GPU, "IB chain target 0x%llx (%u dwords) not mapped",
                          (unsigned long long)addr, dwords);
                    return;
                }
                pos = 0;
                continue;
            }
            if (s && depth >= MAX_IB_DEPTH) {
                s->wait_packet = addr + (uint64_t)(pos-len)*4;
                s->wait_opcode = op;
                LOG_E(GPU, "CP IB depth %d at 0x%llx: header %08x target %08x:%08x control %08x",
                      depth, (unsigned long long)s->wait_packet, p[0], p[2], p[1], p[3]);
                s->fault = true; return;
            }
            if (depth < MAX_IB_DEPTH) {
                if (!gmem_host(g->mem, ib, 4)) {
                    LOG_W(GPU, "IB packet op 0x%02x len %u: %08x %08x %08x %08x %08x %08x", op, len, p[0], p[1], p[2], p[3],
                          len > 4 ? p[4] : 0, len > 5 ? p[5] : 0);
                }
                run(g, ib, ib_dwords, queue_event, depth + 1, legacy_fault);
            }
            break;
        }
        case IT_SET_CONFIG_REG: set_regs(g, 0x2000, p, len); break;
        case IT_SET_CONTEXT_REG:
        case IT_SET_CONTEXT_REG_INDEX: set_regs(g, GPU_REG_CONTEXT_BASE, p, len); break;
        case IT_SET_SH_REG:
        case IT_SET_SH_REG_INDEX: set_regs(g, GPU_REG_SH_BASE, p, len); break;
        case IT_SET_UCONFIG_REG:
        case IT_SET_UCONFIG_REG_INDEX: set_regs(g, GPU_REG_UCONFIG_BASE, p, len); break;
        case IT_LOAD_CONTEXT_REG_INDEX: load_regs_index(g, GPU_REG_CONTEXT_BASE, p, len); break;
        case IT_LOAD_SH_REG_INDEX: load_regs_index(g, GPU_REG_SH_BASE, p, len); break;
        case IT_LOAD_UCONFIG_REG_INDEX: load_regs_index(g, GPU_REG_UCONFIG_BASE, p, len); break;
        case IT_LOAD_CONTEXT_REG: load_regs(g, GPU_REG_CONTEXT_BASE, p, len); break;
        case IT_LOAD_SH_REG: load_regs(g, GPU_REG_SH_BASE, p, len); break;
        case IT_LOAD_UCONFIG_REG: load_regs(g, GPU_REG_UCONFIG_BASE, p, len); break;
        case IT_INDEX_BASE:
            if (len >= 3) g->index_base = (guest_addr)(p[1] & ~1u) | ((guest_addr)(p[2] & 0xFFFF) << 32);
            break;
        case IT_INDEX_BUFFER_SIZE:
            if (len >= 2) g->index_buffer_size = p[1];
            break;
        case IT_INDEX_TYPE: /* same state as VGT_INDEX_TYPE */
            if (len >= 2) {
                g->index_type = p[1] & 3;
                g->regs[mmVGT_INDEX_TYPE & 0xFFFF] = p[1] & 3;
            }
            break;
        case IT_NUM_INSTANCES:
            if (len >= 2) g->num_instances = p[1] ? p[1] : 1;
            break;
        case IT_DRAW_INDEX_AUTO:
            if (len >= 3) {
                gpu_draw d = {false, p[1], g->num_instances, 0, 0};
                draw(g, &d);
            }
            break;
        case IT_DRAW_INDEX_2: /* max_size, base lo, base hi, count, initiator */
            if (len >= 6) {
                uint32_t isize = draw_index_size(g);
                gpu_draw d = {true, p[4], g->num_instances, (guest_addr)p[2] | ((guest_addr)p[3] << 32), isize};
                draw(g, &d);
            }
            break;
        case IT_DRAW_INDEX_OFFSET_2: /* max_size, offset, count, initiator */
            if (len >= 5) {
                uint32_t isize = draw_index_size(g);
                gpu_draw d = {true, p[3], g->num_instances, g->index_base + (guest_addr)p[2] * isize, isize};
                draw(g, &d);
            }
            break;
        case IT_SET_BASE: /* base_index 1: indirect draw/dispatch arguments */
            /* header bit 1 (shader type) picks the graphics or the compute base */
            if (len >= 4 && (p[1] & 0xF) == 1)
                g->indirect_base[(p[0] >> 1) & 1] = (guest_addr)(p[2] & ~7u) | ((guest_addr)(p[3] & 0xFFFF) << 32);
            break;
        case IT_DRAW_INDIRECT: /* data_offset, base_vtx_loc, start_inst_loc, initiator */
        case IT_DRAW_INDEX_INDIRECT:
            if (len >= 5) draw_indirect(g, op == IT_DRAW_INDEX_INDIRECT, g->indirect_base[0] + p[1], 1, 0, p[2] & 0xFFFF,
                                        p[3] & 0xFFFF, ~0u);
            break;
        case IT_DRAW_INDIRECT_MULTI: /* GFX10: data_offset, base_vtx_loc, start_inst_loc, draw_index_loc | enables,
                                        count, count address lo/hi, stride, initiator */
        case IT_DRAW_INDEX_INDIRECT_MULTI:
            if (len >= 10) {
                uint32_t count = p[5];
                if (p[4] & 0x40000000u) { /* count_indirect_enable */
                    uint32_t c = 0;
                    if (read_args(g, (guest_addr)(p[6] & ~3u) | ((guest_addr)p[7] << 32), &c, 4) && c < count) count = c;
                }
                if (dumping(g))
                    LOG_I(GPU, "indirect multi%s: %u draws (max %u, count %s 0x%llx), args at 0x%llx stride %u",
                          op == IT_DRAW_INDEX_INDIRECT_MULTI ? " indexed" : "", count, p[5],
                          (p[4] & 0x40000000u) ? "from" : "fixed", (unsigned long long)((guest_addr)(p[6] & ~3u) | ((guest_addr)p[7] << 32)),
                          (unsigned long long)(g->indirect_base[0] + p[1]), p[8]);
                draw_indirect(g, op == IT_DRAW_INDEX_INDIRECT_MULTI, g->indirect_base[0] + p[1], count, p[8] ? p[8] : 20,
                              p[2] & 0xFFFF, p[3] & 0xFFFF, (p[4] & 0x80000000u) ? (p[4] & 0xFFFF) : ~0u);
                break;
            }
            /* fall through */
        case IT_DRAW_INDEX_MULTI_AUTO:
            g->stats.draws++;
            if (!g->seen_unknown[op]) {
                g->seen_unknown[op] = 1;
                LOG_W(GPU, "indirect draw 0x%02x not supported yet", op);
            }
            break;
        case IT_DISPATCH_DIRECT:
            if (len >= 5) g->regs[mmCOMPUTE_DISPATCH_INITIATOR] = p[4];
            if (len >= 4) dispatch(g, p[1], p[2], p[3]);
            break;
        case IT_DISPATCH_INDIRECT: { /* graphics: data_offset, initiator; compute queues: address lo, hi, initiator */
            if (len >= 3) g->regs[mmCOMPUTE_DISPATCH_INITIATOR] = p[len >= 4 ? 3 : 2];
            guest_addr a = len >= 4 ? ((guest_addr)(p[1] & ~3u) | ((guest_addr)(p[2] & 0xFFFF) << 32))
                                    : g->indirect_base[1] + (len >= 2 ? p[1] : 0);
            uint32_t dim[3] = {0};
            bool got = read_args(g, a, dim, 12);
            {
                static int log_ind = -1;
                static uint32_t logged;
                if (log_ind < 0) log_ind = getenv("PS5C_LOG_INDIRECT") != NULL;
                if (log_ind && logged++ < 40)
                    LOG_I(GPU, "dispatch indirect (len %u, hdr %08x) args 0x%llx: %ux%ux%u, CS 0x%llx", len, p[0],
                          (unsigned long long)a, dim[0], dim[1], dim[2], (unsigned long long)gpu_reg_addr256(g, mmCOMPUTE_PGM_LO));
            }
            /* garbage (arguments a shader we could not run should have written) would hang the GPU;
             * real ones reach a few million groups in X (Unreal 5's Nanite: the renderer splits them) */
            bool sane = got && dim[0] && dim[1] && dim[2] && dim[1] <= 65535 && dim[2] <= 65535 &&
                        (uint64_t)dim[0] * dim[1] * dim[2] <= (1u << 26);
            if (got && !sane && dim[0] && dim[1] && dim[2]) {
                static uint32_t warned;
                if (warned++ < 8)
                    LOG_W(GPU, "indirect dispatch %ux%ux%u skipped: args 0x%llx, len %u, "
                               "packet %08x %08x %08x %08x, bases 0x%llx/0x%llx, CS 0x%llx",
                          dim[0], dim[1], dim[2], (unsigned long long)a, len,
                          p[0], len > 1 ? p[1] : 0, len > 2 ? p[2] : 0, len > 3 ? p[3] : 0,
                          (unsigned long long)g->indirect_base[0], (unsigned long long)g->indirect_base[1],
                          (unsigned long long)gpu_reg_addr256(g, mmCOMPUTE_PGM_LO));
            }
            if (sane) dispatch(g, dim[0], dim[1], dim[2]);
            else g->stats.dispatches++;
            break;
        }
        default:
        unknown_packet: {
            static int log_pm4 = -1;
            if (log_pm4 < 0) log_pm4 = getenv("PS5C_LOG_PM4") != NULL;
            if (log_pm4 && (op == 0x40 || op == 0x1e || op == 0x3c || op == 0x22 || op == 0x58)) {
                char line[160];
                int n = snprintf(line, sizeof(line), "PM4 0x%02x:", op);
                for (uint32_t k = 1; k < len && k < 10 && n < (int)sizeof(line) - 10; k++) n += snprintf(line + n, sizeof(line) - (size_t)n, " %08x", p[k]);
                LOG_I(GPU, "%s", line);
            }
        }
            if (!g->seen_unknown[op]) {
                g->seen_unknown[op] = 1;
                LOG_I(GPU, "PM4 opcode 0x%02x (%u dwords) ignored", op, len);
            }
            g->stats.unknown++;
            break;
        }
        if (g_op_timing) g_op_ns[op] += pal_time_ns() - t_op;
        if (s) {
            if (s->blocked || s->yielded || s->fault) return;
            s->frame[depth].pos = pos;
            s->executed++;
        }
    }
}

gpu_exec_result gpu_submit_resume(gpu *g, gpu_submission *s, guest_addr addr, uint32_t dwords, int32_t queue_event) {
    pal_mutex_lock(g->lock);
    if (s->done || s->fault) {
        pal_mutex_unlock(g->lock);
        return s->fault ? GPU_EXEC_FAULT : GPU_EXEC_DONE;
    }
    if (!s->initialized) {
        s->initialized = true;
        g->stats.submits++;
    }
    s->blocked = false;
    s->yielded = false;
    s->budget = 4096;
    g->active_submission = s;
    if (g_op_timing < 0) g_op_timing = getenv("PS5C_LOG_SUBMITS") != NULL;
    run(g, addr, dwords, queue_event, 0, NULL);
    if (!s->blocked && !s->yielded && !s->fault) {
        if (!queue_event && !s->had_interrupt && g->cb.interrupt) {
            gpu_release rel = {0, 0, 0, true, 0, 0};
            release(g, &rel, true);
        }
        s->done = true;
    }
    g->active_submission = NULL;
    pal_mutex_unlock(g->lock);
    return s->fault ? GPU_EXEC_FAULT : s->blocked ? GPU_EXEC_BLOCKED : s->yielded ? GPU_EXEC_YIELD : GPU_EXEC_DONE;
}

void gpu_submit(gpu *g, guest_addr addr, uint32_t dwords, int32_t queue_event) {
    pal_mutex_lock(g->lock);
    g->stats.submits++;
    if (dumping(g)) {
        LOG_I(GPU, "submit 0x%llx, %u dwords, queue event %d", (unsigned long long)addr, dwords, queue_event);
    }
    uint64_t issued = g->interrupts_issued; /* not stats.interrupts: held ones of earlier submissions land meanwhile */
    if (g_op_timing < 0) g_op_timing = getenv("PS5C_LOG_SUBMITS") != NULL;
    uint64_t t0 = g_op_timing ? pal_time_ns() : 0;
    if (g_op_timing) memset(g_op_ns, 0, sizeof(g_op_ns));
    bool legacy_fault = false;
    run(g, addr, dwords, queue_event, 0, &legacy_fault);
    if (g_op_timing) {
        /* PS5C_LOG_SUBMITS: time per submission and its costliest packet types (nested buffers count twice) */
        char line[256];
        int n = snprintf(line, sizeof(line), "submit 0x%llx (%u dwords, queue event %d): %.1f ms;", (unsigned long long)addr,
                         dwords, queue_event, (double)(pal_time_ns() - t0) / 1e6);
        for (int k = 0; k < 5; k++) {
            uint32_t best = 0;
            for (uint32_t o = 1; o < 256; o++) {
                if (o != IT_INDIRECT_BUFFER && g_op_ns[o] > g_op_ns[best]) best = o;
            }
            if (!g_op_ns[best]) break;
            n += snprintf(line + n, sizeof(line) - (size_t)n, " 0x%02x %.1f", best, (double)g_op_ns[best] / 1e6);
            g_op_ns[best] = 0;
        }
        LOG_I(GPU, "%s", line);
    }
    /* A graphics submission that raised no interrupt of its own ends with the
     * driver's end-of-pipe event (Minecraft waits on it after each frame's
     * command buffer, then reads its completion label). */
    if (!legacy_fault && queue_event == 0 && g->interrupts_issued == issued && g->cb.interrupt) {
        /* the work is done for the game from here on: it may reuse the
         * buffers its dispatches wrote, so their results land first */
        gpu_release rel = {0, 0, 0, true, 0, 0};
        release(g, &rel, true);
    }
    pal_mutex_unlock(g->lock);
}
