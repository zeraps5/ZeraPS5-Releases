/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libkernel APR: the PS5 asynchronous file read path.
 *
 * Games resolve file paths to ids once, then submit command buffers of reads
 * (built by libSceAmpr) that the kernel executes straight into memory.
 *
 * Command buffer header (built by libSceAmpr, read here):
 *   +0x00 u32 type/flags   +0x04 u32 write offset   +0x08 u32 command count
 *   +0x0c u32 buffer size  +0x10 u64 record buffer address */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/amm.h"
#include "hle/ampr_records.h"
#include "hle/equeue.h"
#include "hle/hle.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

/* Guest path -> host path and struct stat, from lib_kernel_fs.c */
int kernel_fs_resolve(hle_call *c, const char *guest, char *host, size_t host_size);
int64_t kernel_fs_stat_host(hle_call *c, const char *host, guest_addr out_addr);

#define E_NOENT 2
#define E_FAULT 14
#define E_INVAL 22
#define E_NOMEM 12
#define MAX_PATH_ 1024

/* Add/DeleteAmprEvent take int32_t ids, but records store their raw 32 bits.
 * Completions must use the same sign-extended key as registration. */
static uint64_t ampr_event_ident(uint32_t id) { return (uint64_t)(int32_t)id; }

typedef struct apr_file {
    uint32_t id;
    uint64_t size;
    char *host_path;
    pal_file *handle; /* opened on the first read, kept open: games read thousands of times */
} apr_file;

/* Files by id: an array, and an open-addressing index into it (Astro Bot
 * registers tens of thousands of files; a linear search per read made its
 * loads take minutes) */
static apr_file *g_files;
static uint32_t g_file_count, g_file_cap;
static uint32_t *g_index; /* g_index_cap slots: file index + 1, 0 = empty */
static uint32_t g_index_cap;
static pal_mutex *g_lock;
static uint32_t g_next_submission = 1;

static void set_errno_ret(hle_call *c, int e) {
    if (c->thread && c->thread->errno_addr)
        gmem_write(c->mem, c->thread->errno_addr, &e, 4);
    hle_ret_s32(c, -1);
}

/* FNV-1a of the guest path: stable ids across runs, like the console's. */
static uint32_t path_id(const char *path) {
    uint32_t h = 2166136261u;
    for (const uint8_t *p = (const uint8_t *)path; *p; p++)
        h = (h ^ *p) * 16777619u;
    return h & 0x7FFFFFFFu;
}

static apr_file *find_file(uint32_t id) {
    if (!g_index_cap)
        return NULL;
    for (uint32_t k = (id * 2654435761u) & (g_index_cap - 1);; k = (k + 1) & (g_index_cap - 1)) {
        uint32_t i = g_index[k];
        if (!i)
            return NULL;
        if (g_files[i - 1].id == id)
            return &g_files[i - 1];
    }
}

static void index_put(uint32_t file) {
    uint32_t k = (g_files[file].id * 2654435761u) & (g_index_cap - 1);
    while (g_index[k])
        k = (k + 1) & (g_index_cap - 1);
    g_index[k] = file + 1;
}

/* Keeps the index at most half full. Caller holds g_lock. */
static bool index_grow(void) {
    if ((g_file_count + 1) * 2 <= g_index_cap)
        return true;
    uint32_t cap = g_index_cap ? g_index_cap * 2 : 1024;
    uint32_t *n = (uint32_t *)calloc(cap, sizeof(uint32_t));
    if (!n)
        return false;
    free(g_index);
    g_index = n;
    g_index_cap = cap;
    for (uint32_t i = 0; i < g_file_count; i++)
        index_put(i);
    return true;
}

/* Registers `guest` and returns 0 or an errno. */
static int resolve_one(hle_call *c, const char *guest, uint32_t *id_out, uint64_t *size_out) {
    uint32_t id = path_id(guest);
    pal_mutex_lock(g_lock);
    const apr_file *known = find_file(id);
    uint64_t known_size = known ? known->size : 0;
    pal_mutex_unlock(g_lock);
    if (known) { /* already resolved: no host lookup (a network stat costs milliseconds) */
        *id_out = id;
        *size_out = known_size;
        return 0;
    }
    char host[MAX_PATH_];
    int err = kernel_fs_resolve(c, guest, host, sizeof(host));
    if (err)
        return err;
    pal_path_info info;
    if (pal_path_stat(host, &info) != PS5C_OK) {
        LOG_D(KERNEL, "apr: '%s' not found", guest);
        return E_NOENT;
    }
    pal_mutex_lock(g_lock);
    if (!find_file(id)) {
        if (!index_grow()) {
            pal_mutex_unlock(g_lock);
            return E_NOMEM;
        }
        if (g_file_count == g_file_cap) {
            uint32_t cap = g_file_cap ? g_file_cap * 2 : 64;
            apr_file *n = (apr_file *)realloc(g_files, cap * sizeof(apr_file));
            if (!n) {
                pal_mutex_unlock(g_lock);
                return E_NOMEM;
            }
            g_files = n;
            g_file_cap = cap;
        }
        size_t len = strlen(host) + 1;
        char *copy = (char *)malloc(len);
        if (!copy) {
            pal_mutex_unlock(g_lock);
            return E_NOMEM;
        }
        memcpy(copy, host, len);
        g_files[g_file_count].id = id;
        g_files[g_file_count].size = info.size;
        g_files[g_file_count].host_path = copy;
        g_files[g_file_count].handle = NULL;
        index_put(g_file_count);
        g_file_count++;
    }
    pal_mutex_unlock(g_lock);
    if (getenv("PS5C_LOG_APR"))
        LOG_I(KERNEL, "apr: '%s' -> id 0x%08x, %llu bytes", guest, id,
              (unsigned long long)info.size);
    *id_out = id;
    *size_out = info.size;
    return 0;
}

/* resolve(prefix?, paths, count, ids, sizes?, error_index? | results?) */
static void resolve_common(hle_call *c, guest_addr prefix, guest_addr paths, uint32_t count,
                           guest_addr ids, guest_addr sizes, guest_addr error_index,
                           guest_addr results) {
    char pre[MAX_PATH_] = "";
    if (prefix && !hle_read_str(c, prefix, pre, sizeof(pre))) {
        set_errno_ret(c, E_FAULT);
        return;
    }
    int first_error = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t p = 0;
        char rel[MAX_PATH_], full[MAX_PATH_ * 2];
        int err = 0;
        uint32_t id = 0xFFFFFFFFu;
        uint64_t size = 0;
        if (!gmem_read_u64(c->mem, paths + (uint64_t)i * 8, &p) ||
            !hle_read_str(c, p, rel, sizeof(rel))) {
            err = E_FAULT;
        } else {
            snprintf(full, sizeof(full), "%s%s", pre, rel);
            err = resolve_one(c, full, &id, &size);
        }
        if (ids)
            gmem_write(c->mem, ids + (uint64_t)i * 4, &id, 4);
        if (sizes)
            gmem_write_u64(c->mem, sizes + (uint64_t)i * 8, size);
        if (results) {
            int32_t r = err ? (int32_t)(0x80020000u | (uint32_t)err) : 0;
            gmem_write(c->mem, results + (uint64_t)i * 4, &r, 4);
        } else if (err) {
            if (error_index)
                gmem_write(c->mem, error_index, &i, 4);
            first_error = err;
            break;
        }
    }
    if (first_error)
        set_errno_ret(c, first_error);
    else
        hle_ret_s32(c, 0);
}

static void resolve_to_ids(hle_call *c) {
    resolve_common(c, 0, hle_arg_ptr(c, 0), hle_arg_u32(c, 1), hle_arg_ptr(c, 2), 0,
                   hle_arg_ptr(c, 3), 0);
}
static void resolve_to_ids_and_sizes(hle_call *c) {
    resolve_common(c, 0, hle_arg_ptr(c, 0), hle_arg_u32(c, 1), hle_arg_ptr(c, 2), hle_arg_ptr(c, 3),
                   hle_arg_ptr(c, 4), 0);
}
static void resolve_prefix_to_ids(hle_call *c) {
    resolve_common(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1), hle_arg_u32(c, 2), hle_arg_ptr(c, 3), 0,
                   hle_arg_ptr(c, 4), 0);
}
static void resolve_prefix_to_ids_and_sizes(hle_call *c) {
    resolve_common(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1), hle_arg_u32(c, 2), hle_arg_ptr(c, 3),
                   hle_arg_ptr(c, 4), hle_arg_ptr(c, 5), 0);
}
static void resolve_to_ids_for_each(hle_call *c) {
    resolve_common(c, 0, hle_arg_ptr(c, 0), hle_arg_u32(c, 1), hle_arg_ptr(c, 2), 0, 0,
                   hle_arg_ptr(c, 3));
}
/* ...WithPrefixToIdsForEach(prefix, paths, count, ids, results) and
 * ...WithPrefixToIdsAndFileSizesForEach(prefix, paths, count, ids, sizes, results) */
static void resolve_prefix_to_ids_for_each(hle_call *c) {
    resolve_common(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1), hle_arg_u32(c, 2), hle_arg_ptr(c, 3), 0, 0,
                   hle_arg_ptr(c, 4));
}
static void resolve_prefix_to_ids_and_sizes_for_each(hle_call *c) {
    resolve_common(c, hle_arg_ptr(c, 0), hle_arg_ptr(c, 1), hle_arg_u32(c, 2), hle_arg_ptr(c, 3),
                   hle_arg_ptr(c, 4), 0, hle_arg_ptr(c, 5));
}
static void resolve_to_ids_and_sizes_for_each(hle_call *c) {
    resolve_common(c, 0, hle_arg_ptr(c, 0), hle_arg_u32(c, 1), hle_arg_ptr(c, 2), hle_arg_ptr(c, 3),
                   0, hle_arg_ptr(c, 4));
}

/* int sceKernelAprGetFileSize(uint32_t id, uint64_t *size) */
static void get_file_size(hle_call *c) {
    pal_mutex_lock(g_lock);
    const apr_file *f = find_file(hle_arg_u32(c, 0));
    uint64_t size = f ? f->size : 0;
    pal_mutex_unlock(g_lock);
    if (!f) {
        set_errno_ret(c, E_NOENT);
        return;
    }
    if (!gmem_write_u64(c->mem, hle_arg_ptr(c, 1), size)) {
        set_errno_ret(c, E_FAULT);
        return;
    }
    hle_ret_s32(c, 0);
}

/* int sceKernelAprGetFileStat(uint32_t id, struct stat *st) */
static void get_file_stat(hle_call *c) {
    char path[MAX_PATH_];
    pal_mutex_lock(g_lock);
    const apr_file *f = find_file(hle_arg_u32(c, 0));
    if (f)
        snprintf(path, sizeof(path), "%s", f->host_path);
    pal_mutex_unlock(g_lock);
    if (!f) {
        set_errno_ret(c, E_NOENT);
        return;
    }
    int64_t r = kernel_fs_stat_host(c, path, hle_arg_ptr(c, 1));
    if (r < 0) {
        set_errno_ret(c, (int)-r);
        return;
    }
    hle_ret_s32(c, 0);
}

/* ---- Command buffers ---- */

static void dump_command_buffer(hle_call *c, guest_addr cb) {
    uint32_t hdr[4] = {0};
    uint64_t data = 0;
    gmem_read(c->mem, cb, hdr, sizeof(hdr));
    gmem_read_u64(c->mem, cb + 0x10, &data);
    LOG_I(KERNEL, "apr cb 0x%llx: type 0x%x offset 0x%x count %u size 0x%x data 0x%llx",
          (unsigned long long)cb, hdr[0], hdr[1], hdr[2], hdr[3], (unsigned long long)data);
    uint32_t len = hdr[1] < 0x100 ? hdr[1] : 0x100;
    const uint8_t *p = len ? (const uint8_t *)gmem_host(c->mem, data, len) : NULL;
    for (uint32_t off = 0; p && off < len; off += 16) {
        char line[80];
        size_t n = 0;
        for (uint32_t k = 0; k < 16 && off + k < len; k++)
            n += (size_t)snprintf(line + n, sizeof(line) - n, "%02x ", p[off + k]);
        LOG_I(KERNEL, "  +%03x: %s", off, line);
    }
}

/* Reads [offset, offset + size) of a registered file into guest memory. */
static int read_into_guest(hle_call *c, uint32_t id, uint64_t offset, uint64_t size,
                           guest_addr dest) {
    void *dst = gmem_host(c->mem, dest, size);
    if (!dst)
        return E_FAULT;
    pal_mutex_lock(g_lock);
    apr_file *f = find_file(id);
    pal_file *file = f ? f->handle : NULL;
    if (f && !file && pal_file_open(f->host_path, PAL_FILE_READ|PAL_FILE_RECOVER_READ, &file) == PS5C_OK)
        f->handle = file;
    pal_mutex_unlock(g_lock);
    if (!file)
        return E_NOENT;
    /* positional reads: threads share the handle */
    uint64_t got = 0, t0 = pal_time_ns();
    ps5c_err err = pal_file_pread(file, dst, size, offset, &got);
    { /* PS5C_LOG_APR_STATS: read volume and time every 10 s (debugging aid) */
        static int on = -1;
        static uint64_t reads, bytes, ns, last;
        if (on < 0)
            on = getenv("PS5C_LOG_APR_STATS") != NULL;
        if (on) {
            uint64_t now = pal_time_ns();
            pal_mutex_lock(g_lock);
            reads++;
            bytes += got;
            ns += now - t0;
            if (now - last > 10000000000ull) {
                LOG_I(KERNEL, "apr: %llu reads, %.1f MB, %.1f s reading, %u files",
                      (unsigned long long)reads, bytes / 1048576.0, ns / 1e9, g_file_count);
                last = now;
            }
            pal_mutex_unlock(g_lock);
        }
    }
    if (err != PS5C_OK) {
        char path[MAX_PATH_] = "";
        pal_mutex_lock(g_lock);
        f = find_file(id);
        if (f) snprintf(path, sizeof(path), "%s", f->host_path);
        pal_mutex_unlock(g_lock);
        LOG_W(KERNEL, "apr: read failed for '%s': id %08x offset 0x%llx size 0x%llx destination 0x%llx, %s",
              path, id, (unsigned long long)offset, (unsigned long long)size,
              (unsigned long long)dest, ps5c_err_str(err));
        return 5; /* EIO */
    }
    if (got < size)
        memset((uint8_t *)dst + got, 0, (size_t)(size - got));
    if (getenv("PS5C_LOG_APR")) {
        const uint8_t *b = (const uint8_t *)dst;
        LOG_I(KERNEL,
              "apr: read id 0x%08x [0x%llx, +0x%llx) got 0x%llx: %02x %02x %02x %02x %02x %02x "
              "%02x %02x",
              id, (unsigned long long)offset, (unsigned long long)size, (unsigned long long)got,
              b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    }
    return 0;
}

/* Record formats (decoded from buffers libSceAmpr builds):
 *   0x28 read file, 5 dwords (6 with a 40-bit offset), as libSceAmpr encodes it:
 *     +0x00 u32  op 0x28 | (dwords - 1) << 8 | offset[17:0] << 12
 *     +0x04 u32  size - 1
 *     +0x08 u32  file id
 *     +0x0c u64  destination[47:0] | offset[31:18] << 50
 *     +0x14 u32  offset[39:32]                 (6-dword form only)
 *   0x08 / 0x78 write kernel event queue, 5 dwords:
 *     +0x00 u32  op | equeue[47:32] << 16
 *     +0x04 u32  equeue[31:0]
 *     +0x08 u32  event id (registered with sceKernelAddAmprEvent)
 *     +0x0c u64  event data
 *   0x05 write label, 2 dwords:
 *     +0x00 u32  op | 1 << 8 | value[3:0] << 12 | address[47:32] << 16
 *     +0x04 u32  address[31:0]
 *   0x05 write u64, 4 dwords / 0x05 or 0x01 write u32, 3 dwords:
 *     +0x00 u32  op | (dwords - 1) << 8 | flags << 12 | address[47:32] << 16
 *     +0x04 u32  address[31:0]
 *     +0x08      the value
 * Unknown records stop execution and are logged with their bytes. */
static void execute_records(hle_call *c, guest_addr cb, const uint8_t *rec, uint32_t end,
                            int32_t *exec, uint32_t *error_offset);

/* The records of a command buffer as it is now (games may reset or refill the
 * buffer object once it is submitted); NULL with *exec set on a bad buffer. */
static const uint8_t *records(hle_call *c, guest_addr cb, uint32_t *end, int32_t *exec) {
    uint32_t hdr[4] = {0};
    uint64_t data = 0;
    *end = 0;
    if (!gmem_read(c->mem, cb, hdr, sizeof(hdr)) || !gmem_read_u64(c->mem, cb + 0x10, &data)) {
        *exec = (int32_t)(0x80020000u | E_FAULT);
        return NULL;
    }
    const uint8_t *rec = hdr[1] ? (const uint8_t *)gmem_host(c->mem, data, hdr[1]) : NULL;
    if (hdr[1] && !rec)
        *exec = (int32_t)(0x80020000u | E_FAULT);
    *end = rec ? hdr[1] : 0;
    return rec;
}

/* Where gather/scatter reads continue, per command buffer: the last read's file,
 * the end of its destination and of its file range. */
typedef struct gs_state {
    guest_addr cb;
    uint32_t file;
    uint64_t dest, off;
} gs_state;
static gs_state g_gs[32];
static uint32_t g_gs_next;

static gs_state gs_get(guest_addr cb) {
    gs_state s = {cb, 0, 0, 0};
    pal_mutex_lock(g_lock);
    for (uint32_t i = 0; i < 32; i++)
        if (g_gs[i].cb == cb)
            s = g_gs[i];
    pal_mutex_unlock(g_lock);
    return s;
}

static void gs_set(guest_addr cb, uint32_t file, uint64_t dest_end, uint64_t off_end) {
    pal_mutex_lock(g_lock);
    gs_state *e = NULL;
    for (uint32_t i = 0; i < 32 && !e; i++)
        if (g_gs[i].cb == cb)
            e = &g_gs[i];
    if (!e)
        e = &g_gs[g_gs_next++ % 32];
    *e = (gs_state){cb, file, dest_end, off_end};
    pal_mutex_unlock(g_lock);
}

static bool wait_satisfied(uint64_t v, uint64_t want, uint8_t cmp) {
    switch (cmp) {
    case 0: return v == want;
    case 1: return v > want;
    case 2: return v < want;
    case 3: return v != want;
    case 4: return (int64_t)(v - want) >= 0;
    case 5: return (int64_t)v > (int64_t)want;
    default: return (int64_t)v < (int64_t)want;
    }
}

/* The AMPR counters: 256 32-bit words, shared by every queue (an AMM buffer may write one that an
 * APR buffer waits on). Accesses: 0 = 64 bits (n and n + 1), 1 = 32, 2..3 = 16-bit halves, 4..7 bytes. */
static _Atomic uint32_t g_counters[256];
static atomic_flag g_counter_writing = ATOMIC_FLAG_INIT;

static void counter_field(uint8_t access, uint32_t *bits, uint32_t *shift) {
    *bits = access == 0 ? 64 : access == 1 ? 32 : access < 4 ? 16 : 8;
    *shift = access < 2 ? 0 : access < 4 ? (access - 2u) * 16u : (access - 4u) * 8u;
}
static uint64_t field_mask(uint32_t bits) { return bits == 64 ? ~0ull : (1ull << bits) - 1u; }

static uint64_t counter_read(uint8_t n, uint8_t access) {
    uint32_t bits, shift;
    counter_field(access, &bits, &shift);
    uint64_t lo = atomic_load(&g_counters[n]);
    if (bits == 64) return lo | (uint64_t)atomic_load(&g_counters[(uint8_t)(n + 1)]) << 32;
    return (lo >> shift) & field_mask(bits);
}

static void counter_write(uint8_t n, uint8_t access, uint8_t op, uint64_t value) {
    uint32_t bits, shift;
    counter_field(access, &bits, &shift);
    while (atomic_flag_test_and_set(&g_counter_writing)) {}
    uint64_t cur = counter_read(n, access), next;
    switch (op) {
    case 1: next = cur | value; break;
    case 2: next = cur & ~value; break;
    case 3: next = cur ^ value; break;
    case 4: next = cur + value; break;
    default: next = value; break;
    }
    next &= field_mask(bits);
    if (bits == 64) {
        atomic_store(&g_counters[(uint8_t)(n + 1)], (uint32_t)(next >> 32));
        atomic_store(&g_counters[n], (uint32_t)next);
    } else {
        uint32_t m = (uint32_t)(field_mask(bits) << shift);
        atomic_store(&g_counters[n], (atomic_load(&g_counters[n]) & ~m) | (uint32_t)(next << shift));
    }
    atomic_flag_clear(&g_counter_writing);
}

/* One of libSceAmpr's records (ampr_records.h); false with *exec set on a failure. */
static bool execute_ampr(hle_call *c, guest_addr cb, const uint8_t *r, int32_t *exec) {
    uint32_t w[5] = {0};
    memcpy(w, r,
           r[0] == AMPR_OP_GATHER_SCATTER_EXT ? 20
           : r[0] == AMPR_OP_GATHER_SCATTER   ? 16
                                              : 12);
    uint64_t q1 = 0, q2 = 0;
    if (r[0] == AMPR_OP_EVENT || r[0] == AMPR_OP_WRITE || r[0] == AMPR_OP_WAIT ||
        (r[0] >= AMPR_OP_AMM_MAP && r[0] != AMPR_OP_SKIP))
        memcpy(&q1, r + 8, 8), memcpy(&q2, r + 16, 8);
    gs_state gs = {cb, 0, 0, 0};
    uint64_t dest = 0, off = 0, size = (uint64_t)w[1] + 1;
    switch (r[0]) {
    case AMPR_OP_GATHER:
    case AMPR_OP_GATHER_EXT:
        gs = gs_get(cb);
        dest = gs.dest;
        off = r[0] == AMPR_OP_GATHER ? w[0] >> 8 : w[2] | (uint64_t)((w[0] >> 8) & 0xFF) << 32;
        break;
    case AMPR_OP_SCATTER:
        gs = gs_get(cb);
        dest = w[2] | (uint64_t)((w[0] >> 8) & 0xFFFF) << 32;
        off = gs.off;
        break;
    case AMPR_OP_GATHER_SCATTER:
    case AMPR_OP_GATHER_SCATTER_EXT:
        gs = gs_get(cb);
        dest = w[2] | (uint64_t)((w[0] >> 8) & 0xFFFF) << 32;
        off = w[3] | (r[0] == AMPR_OP_GATHER_SCATTER_EXT ? (uint64_t)(w[4] & 0xFF) << 32 : 0);
        break;
    case AMPR_OP_RESET_GS: gs_set(cb, 0, 0, 0); return true;
    case AMPR_OP_EVENT:
        if (!equeue_push(q1, ampr_event_ident(w[1]), EVFILT_AMPR_, (int64_t)q2))
            LOG_W(KERNEL, "apr: event %u on queue 0x%llx is not registered", w[1],
                  (unsigned long long)q1);
        return true;
    case AMPR_OP_WRITE: {
        uint64_t v = r[1] == AMPR_WRITE_TIME           ? ampr_time_counter()
                     : r[1] == AMPR_WRITE_ZERO         ? 0
                     : r[1] == AMPR_WRITE_COUNTER      ? counter_read((uint8_t)q2, 1)
                     : r[1] == AMPR_WRITE_COUNTER_PAIR ? counter_read((uint8_t)q2, 0)
                                                       : q2;
        if (!gmem_write_u64(c->mem, q1, v)) {
            *exec = (int32_t)(0x80020000u | E_FAULT);
            return false;
        }
        return true;
    }
    case AMPR_OP_WAIT: {
        uint64_t t0 = pal_time_ns(), v = 0;
        bool warned = false;
        while (gmem_read_u64(c->mem, q1, &v) && !wait_satisfied(v, q2, r[1])) {
            pal_sleep_ns(100000);
            if (!warned && pal_time_ns() - t0 > 5000000000ull) {
                warned = true;
                LOG_W(KERNEL, "apr: waiting 5 s on 0x%llx (0x%llx, want 0x%llx compare %u)",
                      (unsigned long long)q1, (unsigned long long)v, (unsigned long long)q2, r[1]);
            }
        }
        if (warned)
            LOG_I(KERNEL, "apr: the wait on 0x%llx ended after %.1f s", (unsigned long long)q1, (pal_time_ns() - t0) / 1e9);
        return true;
    }
    case AMPR_OP_AMM_MAP:
    case AMPR_OP_AMM_MAP_DIRECT:
    case AMPR_OP_AMM_UNMAP: {
        uint64_t phys = 0;
        if (r[0] == AMPR_OP_AMM_MAP_DIRECT) memcpy(&phys, r + 24, 8);
        int32_t e = r[0] == AMPR_OP_AMM_UNMAP ? kernel_amm_unmap(c, q1, q2)
                                              : kernel_amm_map(c, q1, q2, (w[0] >> 8) & 0xFFFF, r[0] == AMPR_OP_AMM_MAP_DIRECT, phys);
        static int logging = -1;
        if (logging < 0) logging = getenv("PS5C_LOG_AMM") != NULL;
        if (logging || e)
            LOG_I(KERNEL, "amm: %s 0x%llx +0x%llx prot 0x%x%s -> 0x%x", r[0] == AMPR_OP_AMM_UNMAP ? "unmap" : "map",
                  (unsigned long long)q1, (unsigned long long)q2, (w[0] >> 8) & 0xFFFF,
                  r[0] == AMPR_OP_AMM_MAP_DIRECT ? " (direct)" : "", (uint32_t)e);
        if (e) {
            *exec = e;
            return false;
        }
        return true;
    }
    case AMPR_OP_COUNTER_WRITE: counter_write(r[1], r[2], r[3], q1); return true;
    case AMPR_OP_COUNTER_WAIT: {
        /* narrow fields compare in the top bits, so the signed compares see their sign */
        uint32_t bits, shift;
        counter_field(r[2], &bits, &shift);
        uint32_t up = 64 - bits;
        uint64_t want = (q1 & q2) << up, t0 = pal_time_ns();
        bool warned = false;
        while (!wait_satisfied((counter_read(r[1], r[2]) & q2) << up, want, r[3])) {
            pal_sleep_ns(50000);
            if (!warned && pal_time_ns() - t0 > 5000000000ull) {
                warned = true;
                LOG_W(KERNEL, "apr: waiting 5 s on counter %u (0x%llx, want 0x%llx compare %u)", r[1],
                      (unsigned long long)counter_read(r[1], r[2]), (unsigned long long)q1, r[3]);
            }
        }
        if (warned) LOG_I(KERNEL, "apr: the wait on counter %u ended after %.1f s", r[1], (pal_time_ns() - t0) / 1e9);
        return true;
    }
    case AMPR_OP_AMM_REMAP:
    case AMPR_OP_AMM_PROTECT:
    case AMPR_OP_AMM_PRT: {
        uint32_t prot = (w[0] >> 8) & 0xFFFF;
        uint64_t src = 0;
        int32_t e;
        const char *what;
        if (r[0] == AMPR_OP_AMM_REMAP) {
            memcpy(&src, r + 24, 8);
            e = kernel_amm_remap(c, q1, src, q2, prot, w[1]);
            what = w[1] == AMM_REMAP_ALIAS ? "multimap" : w[1] == AMM_REMAP_INTO_PRT ? "remap into prt" : "remap";
        } else if (r[0] == AMPR_OP_AMM_PROTECT) {
            e = kernel_amm_protect(c, q1, q2, prot, w[1]);
            what = "protect";
        } else {
            e = kernel_amm_prt(c, q1, q2, w[1], prot);
            what = w[1] == AMM_PRT_MAP ? "map as prt" : w[1] == AMM_PRT_UNMAP ? "unmap to prt" : "allocate for prt";
        }
        static int logging = -1;
        if (logging < 0) logging = getenv("PS5C_LOG_AMM") != NULL;
        if (logging || e)
            LOG_I(KERNEL, "amm: %s 0x%llx +0x%llx prot 0x%x from 0x%llx -> 0x%x", what, (unsigned long long)q1,
                  (unsigned long long)q2, prot, (unsigned long long)src, (uint32_t)e);
        if (e) {
            *exec = e;
            return false;
        }
        return true;
    }
    default: return true; /* AMPR_OP_SKIP */
    }
    int err = read_into_guest(c, gs.file, off, size, dest);
    runtime_watch_arm(c->rt);
    if (err) {
        *exec = (int32_t)(0x80020000u | (uint32_t)err);
        return false;
    }
    gs_set(cb, gs.file, dest + size, off + size);
    return true;
}

static void execute_records(hle_call *c, guest_addr cb, const uint8_t *rec, uint32_t end,
                            int32_t *exec, uint32_t *error_offset) {
    *exec = 0;
    *error_offset = 0;
    for (uint32_t off = 0; off < end;) {
        uint8_t op = rec[off];
        uint32_t rec_bytes = (((uint32_t)rec[off + 1] & 7u) + 1u) * 4u;
        uint32_t ours = op >= 0xE1 ? ampr_record_size(rec + off) : 0;
        if (ours >= 4 && off + ours <= end) {
            if (!execute_ampr(c, cb, rec + off, exec)) {
                *error_offset = off;
                return;
            }
            off += ours;
            continue;
        }
        if (op == 0x28 && (rec_bytes == 0x14 || rec_bytes == 0x18) && off + rec_bytes <= end) {
            uint32_t word0, size_m1, id;
            uint64_t dest_q;
            memcpy(&word0, rec + off, 4);
            memcpy(&size_m1, rec + off + 4, 4);
            memcpy(&id, rec + off + 8, 4);
            memcpy(&dest_q, rec + off + 12, 8);
            uint64_t file_offset = ((word0 >> 12) & 0x3FFFFu) | ((dest_q >> 32) & 0xFFFC0000u);
            if (rec_bytes == 0x18)
                file_offset |= (uint64_t)rec[off + 20] << 32;
            guest_addr dest = dest_q & 0x0000FFFFFFFFFFFFull;
            int err = read_into_guest(c, id, file_offset, (uint64_t)size_m1 + 1, dest);
            runtime_watch_arm(c->rt);
            if (!err)
                gs_set(cb, id, dest + (uint64_t)size_m1 + 1, file_offset + (uint64_t)size_m1 + 1);
            LOG_D(KERNEL, "apr: read id 0x%08x, %u bytes at 0x%llx -> 0x%llx: %d", id, size_m1 + 1,
                  (unsigned long long)file_offset, (unsigned long long)dest, err);
            if (err) {
                *exec = (int32_t)(0x80020000u | (uint32_t)err);
                *error_offset = off;
                return;
            }
            off += rec_bytes;
            continue;
        }
        if ((op == 0x08 || op == 0x78) && rec_bytes == 0x14 && off + rec_bytes <= end) {
            uint32_t w[3];
            int64_t ev_data;
            memcpy(w, rec + off, 12);
            memcpy(&ev_data, rec + off + 12, 8);
            uint64_t eq = w[1] | (uint64_t)(w[0] >> 16) << 32;
            if (!equeue_push(eq, ampr_event_ident(w[2]), EVFILT_AMPR_, ev_data)) {
                LOG_W(KERNEL, "apr: event %u on queue 0x%llx is not registered", w[2],
                      (unsigned long long)eq);
            }
            off += rec_bytes;
            continue;
        }
        if (op == 0x05 && rec_bytes == 0x08 && off + rec_bytes <= end) {
            /* write a completion label: value in [15:12], address[47:32] in [31:16], then
             * address[31:0] (Resident Evil 2 spins on the 64-bit label it zeroed before submitting)
             */
            uint32_t w[2];
            memcpy(w, rec + off, 8);
            guest_addr label = w[1] | (guest_addr)(w[0] >> 16) << 32;
            if (!gmem_write_u64(c->mem, label, (w[0] >> 12) & 0xF)) {
                *exec = (int32_t)(0x80020000u | E_FAULT);
                *error_offset = off;
                return;
            }
            off += rec_bytes;
            continue;
        }
        if ((op == 0x05 || op == 0x01) &&
            (rec_bytes == 0x0C || (op == 0x05 && rec_bytes == 0x10)) && off + rec_bytes <= end) {
            /* write a 64-bit (0x05, 4 dwords) or 32-bit (3 dwords) value:
             * address[47:32] in [31:16], address[31:0], then the value (Sonic
             * Frontiers clears a status word, stores to the next one and marks
             * the first 0x3fffffffffffffff around a label write) */
            uint32_t w[4] = {0};
            memcpy(w, rec + off, rec_bytes);
            guest_addr addr = w[1] | (guest_addr)(w[0] >> 16) << 32;
            uint64_t before = 0;
            gmem_read_u64(c->mem, addr, &before);
            bool ok = rec_bytes == 0x10 ? gmem_write_u64(c->mem, addr, w[2] | (uint64_t)w[3] << 32)
                                        : gmem_write(c->mem, addr, &w[2], 4);
            if (getenv("PS5C_LOG_APR"))
                LOG_I(KERNEL, "apr: write op 0x%02x flags %u at 0x%llx: 0x%llx -> 0x%x", op,
                      (w[0] >> 12) & 0xF, (unsigned long long)addr, (unsigned long long)before,
                      w[2]);
            if (!ok) {
                *exec = (int32_t)(0x80020000u | E_FAULT);
                *error_offset = off;
                return;
            }
            off += rec_bytes;
            continue;
        }
        dump_command_buffer(c, cb);
        LOG_W(KERNEL, "apr: unknown record 0x%02x at +0x%x", op, off);
        *exec = (int32_t)(0x80020000u | E_INVAL);
        *error_offset = off;
        return;
    }
}

static void write_result(hle_call *c, guest_addr result, int32_t exec, uint32_t error_offset) {
    if (!result)
        return;
    gmem_write(c->mem, result, &exec, 4);
    gmem_write(c->mem, result + 4, &error_offset, 4);
}

/* PS5C_LOG_APR: every submitted command buffer is logged with its records */
static void log_submit(hle_call *c, const char *how, guest_addr cb, int32_t exec, uint32_t off) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("PS5C_LOG_APR") != NULL;
    if (!enabled)
        return;
    LOG_I(KERNEL, "apr %s: result 0x%x at +0x%x", how, (uint32_t)exec, off);
    dump_command_buffer(c, cb);
}

/* Asynchronous submissions run on a host worker, in order, a moment after the
 * call returns, as on the console: games mark a request pending only after
 * submitting it, so completing it inside the call let them overwrite the
 * completion (Sonic Frontiers' CRI movie reader then waited forever). */
typedef struct apr_pending {
    struct apr_pending *next;
    uint64_t serial;
    uint32_t id;
} apr_pending;

typedef struct apr_job {
    guest_addr cb, result; /* result: SubmitCommandBufferAndGetResult's, written when done */
    uint8_t *rec;          /* the records, copied at submission */
    uint32_t end, id;
    apr_pending pending; /* lives in the queue slot until its result is written */
} apr_job;

#define MAX_JOBS 256
/* One queue per engine (APR, AMM) and priority (0..2), each with its worker, as on the
 * console: a buffer waiting on an address holds up only its own queue (Wolverine waits in
 * one for a counter that submissions of another queue advance). */
#define QUEUES 6
typedef struct apr_queue {
    apr_job jobs[MAX_JOBS];
    uint32_t head, count;
    pal_cond *cond;
    pal_thread *worker;
} apr_queue;
static apr_queue g_q[QUEUES];
static pal_cond *g_done_cond;
static apr_pending *g_pending;
static uint64_t g_submission_serial;
static bool g_async_tried, g_async;
static hle_call g_worker_call; /* memory and runtime of the submitting game */

/* All pending-list and id operations hold g_lock. Completed ids need no
 * history: another queue can advance arbitrarily far while an old id waits.
 * Inline fallback nodes live on their submitting thread's stack, so tracking
 * completion itself cannot fail when the records allocation fails. */
static bool pending_matches(uint32_t id, uint64_t cutoff) {
    for (const apr_pending *p = g_pending; p; p = p->next)
        if (id ? p->id == id : p->serial <= cutoff) return true;
    return false;
}

static uint32_t submission_id_locked(void) {
    uint32_t id;
    do {
        id = g_next_submission++;
    } while (!id || pending_matches(id, 0)); /* zero means wait-all; do not reuse a live id */
    g_submission_serial++;
    return id;
}

static uint32_t next_submission(void) {
    pal_mutex_lock(g_lock);
    uint32_t id = submission_id_locked();
    pal_mutex_unlock(g_lock);
    return id;
}

static void pending_begin(apr_pending *p, uint32_t id) {
    *p = (apr_pending){g_pending, g_submission_serial, id};
    g_pending = p;
}

static void pending_end(apr_pending *p) {
    apr_pending **at = &g_pending;
    while (*at && *at != p) at = &(*at)->next;
    if (*at) *at = p->next;
    if (g_done_cond) pal_cond_broadcast(g_done_cond);
}

static void pending_wait(void) {
    if (g_done_cond) {
        pal_cond_wait(g_done_cond, g_lock);
    } else {
        /* Explicit sync mode, or failure to create the condition variable.
         * A concurrent inline submitter must be able to acquire g_lock. */
        pal_mutex_unlock(g_lock);
        pal_sleep_ns(100000);
        pal_mutex_lock(g_lock);
    }
}

static int32_t worker_main(void *arg) {
    apr_queue *q = (apr_queue *)arg;
    static int latency_us = -1;
    if (latency_us < 0)
        latency_us = getenv("PS5C_APR_LATENCY_US") ? atoi(getenv("PS5C_APR_LATENCY_US")) : 200;
    for (;;) {
        pal_mutex_lock(g_lock);
        while (!q->count)
            pal_cond_wait(q->cond, g_lock);
        apr_job *slot = &q->jobs[q->head];
        apr_job job = *slot;
        pal_mutex_unlock(g_lock);
        if (latency_us)
            pal_sleep_ns((uint64_t)latency_us * 1000);
        int32_t exec;
        uint32_t off;
        execute_records(&g_worker_call, job.cb, job.rec, job.end, &exec, &off);
        if (exec)
            LOG_W(KERNEL, "apr: submission %u failed: 0x%x at +0x%x", job.id, (uint32_t)exec, off);
        write_result(&g_worker_call, job.result, exec, off);
        free(job.rec);
        pal_mutex_lock(g_lock);
        q->head = (q->head + 1) % MAX_JOBS;
        q->count--;
        pending_end(&slot->pending);
        pal_mutex_unlock(g_lock);
    }
    return 0;
}

/* g_lock held: the workers, made at the first submission (PS5C_APR_SYNC=1: none, all run in the call) */
static bool start_workers(hle_call *c) {
    if (g_async_tried) return g_async;
    g_async_tried = true;
    if (getenv("PS5C_APR_SYNC") || pal_cond_create(&g_done_cond) != PS5C_OK) return false;
    g_worker_call.mem = c->mem, g_worker_call.rt = c->rt;
    for (uint32_t i = 0; i < QUEUES; i++) {
        char name[16];
        snprintf(name, sizeof(name), "%s %u", i < 3 ? "apr" : "amm", i % 3);
        if (pal_cond_create(&g_q[i].cond) != PS5C_OK ||
            pal_thread_create(&g_q[i].worker, worker_main, &g_q[i], name, 0) != PS5C_OK)
            return false;
    }
    return g_async = true;
}

/* Queues records (copied) on an engine's queue; the submission id. Run in the call when
 * there is no worker or no room. */
static uint32_t queue_records(hle_call *c, bool amm, uint32_t priority, guest_addr cb, const uint8_t *rec,
                              uint32_t end, guest_addr result) {
    uint8_t *copy = rec && end ? (uint8_t *)malloc(end) : NULL;
    if (copy) memcpy(copy, rec, end);
    apr_queue *q = &g_q[(amm ? 3 : 0) + (priority > 2 ? 2 : priority)];
    pal_mutex_lock(g_lock);
    uint32_t id = submission_id_locked();
    if (!copy || !start_workers(c) || q->count == MAX_JOBS) {
        apr_pending pending;
        pending_begin(&pending, id);
        pal_mutex_unlock(g_lock);
        free(copy);
        int32_t exec = 0;
        uint32_t off = 0;
        if (rec) execute_records(c, cb, rec, end, &exec, &off);
        write_result(c, result, exec, off);
        pal_mutex_lock(g_lock);
        pending_end(&pending);
        pal_mutex_unlock(g_lock);
        return id;
    }
    apr_job *slot = &q->jobs[(q->head + q->count++) % MAX_JOBS];
    *slot = (apr_job){.cb = cb, .result = result, .rec = copy, .end = end, .id = id};
    pending_begin(&slot->pending, id);
    pal_cond_signal(q->cond);
    pal_mutex_unlock(g_lock);
    return id;
}

/* Queues an APR command buffer; its submission id. */
static uint32_t submit_async(hle_call *c, guest_addr cb, uint32_t priority, guest_addr result) {
    int32_t exec = 0;
    uint32_t end;
    const uint8_t *rec = records(c, cb, &end, &exec);
    if (!rec) {
        write_result(c, result, exec, 0);
        return next_submission();
    }
    return queue_records(c, false, priority, cb, rec, end, result);
}

/* Waits until submission `id` completed (0: everything queued so far). */
static void wait_done(uint32_t id) {
    pal_mutex_lock(g_lock);
    uint64_t cutoff = g_submission_serial;
    while (pending_matches(id, cutoff)) pending_wait();
    pal_mutex_unlock(g_lock);
}

/* libSceAmpr's AMM submissions (lib_ampr.c): the `bytes` first bytes of a command buffer's
 * records, on the AMM queues. `cb` is the command buffer object or its records. */
uint32_t apr_submit_amm(hle_call *c, guest_addr cb, uint32_t bytes, uint32_t priority, guest_addr result) {
    uint32_t hdr[4] = {0};
    uint64_t data = 0;
    /* an object: offset <= size and a mapped records address (else cb is the records themselves) */
    if (!(gmem_read(c->mem, cb, hdr, sizeof(hdr)) && gmem_read_u64(c->mem, cb + 0x10, &data) && data &&
          hdr[1] <= hdr[3] && bytes <= hdr[1] && gmem_host(c->mem, data, bytes ? bytes : 1)))
        data = cb;
    const uint8_t *rec = bytes ? (const uint8_t *)gmem_host(c->mem, data, bytes) : NULL;
    return queue_records(c, true, priority, cb, rec, rec ? bytes : 0, result);
}

void apr_wait(uint32_t id) {
    wait_done(id);
}

/* int sceKernelAprSubmitCommandBuffer(void *cb, uint64_t priority) */
static void submit(hle_call *c) {
    submit_async(c, hle_arg_ptr(c, 0), hle_arg_u32(c, 1), 0);
    log_submit(c, "submit", hle_arg_ptr(c, 0), 0, 0);
    hle_ret_s32(c, 0);
}

/* int sceKernelAprSubmitCommandBufferAndGetId(void *cb, uint64_t priority, uint32_t *id) */
static void submit_and_get_id(hle_call *c) {
    uint32_t id = submit_async(c, hle_arg_ptr(c, 0), hle_arg_u32(c, 1), 0);
    log_submit(c, "submit+id", hle_arg_ptr(c, 0), 0, 0);
    gmem_write(c->mem, hle_arg_ptr(c, 2), &id, 4);
    hle_ret_s32(c, 0);
}

/* int sceKernelAprSubmitCommandBufferAndGetResult(void *cb, uint64_t priority, result *r, uint32_t
 * *id): queued like the others; the result is written when it completed */
static void submit_and_get_result(hle_call *c) {
    uint32_t id = submit_async(c, hle_arg_ptr(c, 0), hle_arg_u32(c, 1), hle_arg_ptr(c, 2));
    log_submit(c, "submit+result", hle_arg_ptr(c, 0), 0, 0);
    if (hle_arg_ptr(c, 3))
        gmem_write(c->mem, hle_arg_ptr(c, 3), &id, 4);
    hle_ret_s32(c, 0);
}

/* int sceKernelAddAmprEvent(SceKernelEqueue eq, int id, void *udata): completion
 * events that command buffers raise (record 0x08). */
static void add_ampr_event(hle_call *c) {
    bool ok = equeue_add(hle_arg(c, 0), ampr_event_ident(hle_arg_u32(c, 1)), EVFILT_AMPR_, hle_arg(c, 2));
    hle_ret_s32(c, ok ? 0 : (int32_t)0x80020009); /* EBADF */
}

static void delete_ampr_event(hle_call *c) {
    bool ok = equeue_remove(hle_arg(c, 0), ampr_event_ident(hle_arg_u32(c, 1)), EVFILT_AMPR_);
    hle_ret_s32(c, ok ? 0 : (int32_t)0x80020002); /* ENOENT */
}

/* int sceKernelAprWaitCommandBuffer(uint32_t id) */
static void wait_command_buffer(hle_call *c) {
    wait_done(hle_arg_u32(c, 0));
    hle_ret_s32(c, 0);
}

#define K "libkernel"

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(K, "sceKernelAprResolveFilepathsToIds", resolve_to_ids),
    HLE_FUNC_ENTRY(K, "sceKernelAprResolveFilepathsToIdsAndFileSizes", resolve_to_ids_and_sizes),
    HLE_FUNC_ENTRY(K, "sceKernelAprResolveFilepathsWithPrefixToIds", resolve_prefix_to_ids),
    HLE_FUNC_ENTRY(K, "sceKernelAprResolveFilepathsWithPrefixToIdsAndFileSizes",
                   resolve_prefix_to_ids_and_sizes),
    HLE_FUNC_ENTRY(K, "sceKernelAprResolveFilepathsToIdsForEach", resolve_to_ids_for_each),
    HLE_FUNC_ENTRY(K, "sceKernelAprResolveFilepathsToIdsAndFileSizesForEach",
                   resolve_to_ids_and_sizes_for_each),
    HLE_FUNC_ENTRY(K, "sceKernelAprResolveFilepathsWithPrefixToIdsForEach", resolve_prefix_to_ids_for_each),
    HLE_FUNC_ENTRY(K, "sceKernelAprResolveFilepathsWithPrefixToIdsAndFileSizesForEach",
                   resolve_prefix_to_ids_and_sizes_for_each),
    HLE_FUNC_ENTRY(K, "sceKernelAprGetFileSize", get_file_size),
    HLE_FUNC_ENTRY(K, "sceKernelAprGetFileStat", get_file_stat),
    HLE_FUNC_ENTRY(K, "sceKernelAprSubmitCommandBuffer", submit),
    HLE_FUNC_ENTRY(K, "sceKernelAprSubmitCommandBufferAndGetId", submit_and_get_id),
    HLE_FUNC_ENTRY(K, "sceKernelAprSubmitCommandBufferAndGetResult", submit_and_get_result),
    HLE_FUNC_ENTRY(K, "sceKernelAprWaitCommandBuffer", wait_command_buffer),
    HLE_FUNC_ENTRY(K, "sceKernelAddAmprEvent", add_ampr_event),
    HLE_FUNC_ENTRY(K, "sceKernelAddAmprSystemEvent", add_ampr_event),
    HLE_FUNC_ENTRY(K, "sceKernelDeleteAmprEvent", delete_ampr_event),
    HLE_FUNC_ENTRY(K, "sceKernelDeleteAmprSystemEvent", delete_ampr_event),
};

void hle_register_libkernel_apr(void) {
    if (!g_lock && pal_mutex_create(&g_lock) != PS5C_OK) {
        LOG_E(KERNEL, "cannot create the APR lock");
        return;
    }
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}
