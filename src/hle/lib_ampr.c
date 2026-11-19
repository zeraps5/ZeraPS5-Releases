/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceAmpr: games build APR command buffers (file reads into memory, then
 * completion writes and events) with it and submit them to libkernel's APR
 * (lib_kernel_apr.c). The records are ours (ampr_records.h); their sizes and
 * the API are as KytyPS5's libAmpr.cpp (GPL-2.0) documents them. Dumps for
 * older firmware ship a third-party stand-in (AMPR_EMU) in fakelib/; this one
 * replaces it unless PS5C_AMPR_LLE=1. The AMM half maps memory through command
 * buffers too: its records (0xEA..0xEC) run on libkernel's APR worker, in order
 * with the APR submissions, and map in a window of the address space kept for it.
 *
 * Command buffer object (the game's memory):
 *   +0x00 u32 type (0x10000: a read happened, gather/scatter may follow;
 *                   0x20000: inside MapBegin..MapEnd)
 *   +0x04 u32 current offset   +0x08 u32 command count   +0x0c u32 buffer size
 *   +0x10 u64 buffer address
 *   APR command buffers: +0x18, +0x20 reserved state words (cleared). */
#include <string.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#include "base/log.h"
#include "hle/amm.h"
#include "hle/ampr_records.h"
#include "hle/hle.h"

void hle_register_table(const hle_export *table, size_t count);

#define E_PERM ((int32_t)0x80020001)
#define E_FAULT ((int32_t)0x8002000E)
#define E_BUSY ((int32_t)0x80020010)
#define E_INVAL ((int32_t)0x80020016)

#define CB_TYPE 0x00
#define CB_OFFSET 0x04
#define CB_NUM 0x08
#define CB_SIZE 0x0C
#define CB_DATA 0x10
#define TYPE_GS_VALID 0x10000u
#define TYPE_MAP_ACTIVE 0x20000u

#define MAX_READ (1ull << 32)
#define MAX_OFFSET (1ull << 40)
#define MAX_ADDR 0x0000F00000000000ull

static uint32_t rd32(hle_call *c, guest_addr a) {
    uint32_t v = 0;
    gmem_read(c->mem, a, &v, 4);
    return v;
}
static uint64_t rd64(hle_call *c, guest_addr a) {
    uint64_t v = 0;
    gmem_read_u64(c->mem, a, &v);
    return v;
}
static void wr32(hle_call *c, guest_addr a, uint32_t v) {
    gmem_write(c->mem, a, &v, 4);
}
static void wr64(hle_call *c, guest_addr a, uint64_t v) {
    gmem_write_u64(c->mem, a, v);
}
static void ret(hle_call *c, int32_t v) {
    hle_ret_s32(c, v);
}

static bool read_ok(uint64_t dest, uint64_t size) {
    return size && size <= MAX_READ && dest <= MAX_ADDR && MAX_ADDR - dest >= size;
}
static uint32_t align4(uint64_t v) {
    return (uint32_t)((v + 3) & ~3ull);
}
static uint32_t marker_size(hle_call *c, guest_addr msg, bool color) {
    uint64_t len = 0;
    for (; msg && len < 4096; len++) {
        uint8_t ch = 0;
        if (!gmem_read(c->mem, msg + len, &ch, 1) || !ch)
            break;
    }
    return align4((color ? 8u : 4u) + (msg ? len + 1 : 0));
}

/* Appends a record; 0, or the error code when the buffer is missing or full. */
static int32_t append(hle_call *c, guest_addr cb, const void *rec, uint32_t bytes) {
    if (!cb)
        return E_INVAL;
    uint32_t off = rd32(c, cb + CB_OFFSET), size = rd32(c, cb + CB_SIZE);
    uint64_t data = rd64(c, cb + CB_DATA);
    if (!data || bytes > size || off > size - bytes)
        return E_BUSY;
    uint8_t zero[64] = {0};
    if (!gmem_write(c->mem, data + off, rec ? rec : zero, rec ? bytes : (bytes < 64 ? bytes : 4)))
        return E_FAULT;
    wr32(c, cb + CB_OFFSET, off + bytes);
    wr32(c, cb + CB_NUM, rd32(c, cb + CB_NUM) + 1);
    return 0;
}

static int32_t append_skip(hle_call *c, guest_addr cb, uint32_t bytes) {
    uint32_t w = AMPR_OP_SKIP | (align4(bytes) << 8);
    if (!cb)
        return E_INVAL;
    uint32_t off = rd32(c, cb + CB_OFFSET), size = rd32(c, cb + CB_SIZE);
    uint64_t data = rd64(c, cb + CB_DATA);
    bytes = align4(bytes);
    if (!data || bytes > size || off > size - bytes)
        return E_BUSY;
    if (!gmem_write(c->mem, data + off, &w, 4))
        return E_FAULT;
    wr32(c, cb + CB_OFFSET, off + bytes);
    wr32(c, cb + CB_NUM, rd32(c, cb + CB_NUM) + 1);
    return 0;
}

static void set_type(hle_call *c, guest_addr cb, uint32_t set, uint32_t clear) {
    wr32(c, cb + CB_TYPE, (rd32(c, cb + CB_TYPE) | set) & ~clear);
}

/* ---- Object ---- */

static void cb_constructor(hle_call *c) {
    guest_addr cb = hle_arg_ptr(c, 0);
    uint8_t z[0x18] = {0};
    if (cb && !gmem_write(c->mem, cb, z, sizeof(z))) {
        ret(c, E_FAULT);
        return;
    }
    ret(c, 0);
}

/* (cb, reserved_state0, reserved_state1): the two state words, by default after the header */
static void apr_constructor(hle_call *c) {
    guest_addr cb = hle_arg_ptr(c, 0), r0 = hle_arg_ptr(c, 1), r1 = hle_arg_ptr(c, 2);
    if (!cb) {
        ret(c, 0);
        return;
    }
    wr64(c, r0 ? r0 : cb + 0x18, 0);
    wr64(c, r1 ? r1 : cb + 0x20, 0);
    ret(c, 0);
}

static void ok(hle_call *c) {
    ret(c, 0);
}

/* (cb, buffer, size) */
static void set_buffer(hle_call *c) {
    guest_addr cb = hle_arg_ptr(c, 0), buf = hle_arg_ptr(c, 1);
    uint32_t size = hle_arg_u32(c, 2);
    if (!cb || !buf || (buf & 3) || !size || (size & 3) || size > (64u << 20)) {
        ret(c, E_INVAL);
        return;
    }
    if (rd64(c, cb + CB_DATA)) {
        ret(c, E_BUSY);
        return;
    }
    wr64(c, cb + CB_DATA, buf);
    wr32(c, cb + CB_SIZE, size);
    wr32(c, cb + CB_OFFSET, 0);
    wr32(c, cb + CB_NUM, 0);
    set_type(c, cb, 0, TYPE_GS_VALID | TYPE_MAP_ACTIVE);
    ret(c, 0);
}

static void reset(hle_call *c) {
    guest_addr cb = hle_arg_ptr(c, 0);
    if (!cb || !rd64(c, cb + CB_DATA)) {
        ret(c, E_PERM);
        return;
    }
    wr32(c, cb + CB_OFFSET, 0);
    wr32(c, cb + CB_NUM, 0);
    set_type(c, cb, 0, TYPE_GS_VALID | TYPE_MAP_ACTIVE);
    ret(c, 0);
}

/* void *ClearBuffer(cb): detaches the buffer, returning it */
static void clear_buffer(hle_call *c) {
    guest_addr cb = hle_arg_ptr(c, 0);
    uint64_t data = cb ? rd64(c, cb + CB_DATA) : 0;
    if (data)
        wr64(c, cb + CB_DATA, 0), wr32(c, cb + CB_SIZE, 0);
    hle_ret_u64(c, data);
}

static void get_size(hle_call *c) {
    hle_ret_u64(c, hle_arg_ptr(c, 0) ? rd32(c, hle_arg_ptr(c, 0) + CB_SIZE) : 0);
}
static void get_offset(hle_call *c) {
    hle_ret_u64(c, hle_arg_ptr(c, 0) ? rd32(c, hle_arg_ptr(c, 0) + CB_OFFSET) : 0);
}
static void get_type(hle_call *c) {
    hle_ret_u64(c, hle_arg_ptr(c, 0) ? rd32(c, hle_arg_ptr(c, 0) + CB_TYPE) : 0);
}
static void get_base(hle_call *c) {
    hle_ret_u64(c, hle_arg_ptr(c, 0) ? rd64(c, hle_arg_ptr(c, 0) + CB_DATA) : 0);
}
static void get_num(hle_call *c) {
    hle_ret_u64(c, hle_arg_ptr(c, 0) ? rd32(c, hle_arg_ptr(c, 0) + CB_NUM) : 0);
}

/* ---- Reads ---- */

/* (cb, -, -, file id, destination, size, file offset) */
static void read_file(hle_call *c) {
    guest_addr cb = hle_arg_ptr(c, 0), dest = hle_arg_ptr(c, 4);
    uint32_t id = hle_arg_u32(c, 3);
    uint64_t size = hle_arg(c, 5), off = hle_arg(c, 6);
    if (!cb || !read_ok(dest, size) || off >= MAX_OFFSET) {
        ret(c, E_INVAL);
        return;
    }
    uint32_t w[6] = {0};
    uint32_t bytes = off >> 32 ? 0x18 : 0x14;
    w[0] = AMPR_OP_READ | ((bytes / 4 - 1) << 8) | (uint32_t)((off & 0x3FFFF) << 12);
    w[1] = (uint32_t)(size - 1);
    w[2] = id;
    uint64_t q = (dest & 0xFFFFFFFFFFFFull) | (((off >> 18) & 0x3FFF) << 50);
    memcpy(&w[3], &q, 8);
    w[5] = (uint32_t)(off >> 32) & 0xFF;
    int32_t r = append(c, cb, w, bytes);
    if (!r)
        set_type(c, cb, TYPE_GS_VALID, 0);
    ret(c, r);
}

/* (cb, -, -, size, file offset): the last read's file, right after its destination */
static void read_gather(hle_call *c) {
    guest_addr cb = hle_arg_ptr(c, 0);
    uint64_t size = hle_arg(c, 3), off = hle_arg(c, 4);
    if (!cb || !size || size > MAX_READ || off >= MAX_OFFSET ||
        !(rd32(c, cb + CB_TYPE) & TYPE_GS_VALID)) {
        ret(c, E_INVAL);
        return;
    }
    uint32_t w[3];
    if (off <= 0x3FFFF) {
        w[0] = AMPR_OP_GATHER | (uint32_t)(off << 8), w[1] = (uint32_t)(size - 1);
        {
            ret(c, append(c, cb, w, 8));
            return;
        }
    }
    w[0] = AMPR_OP_GATHER_EXT | (uint32_t)((off >> 32) << 8), w[1] = (uint32_t)(size - 1),
    w[2] = (uint32_t)off;
    ret(c, append(c, cb, w, 12));
}

/* (cb, -, -, destination, size): the last read's file, right after its file range */
static void read_scatter(hle_call *c) {
    guest_addr cb = hle_arg_ptr(c, 0), dest = hle_arg_ptr(c, 3);
    uint64_t size = hle_arg(c, 4);
    if (!cb || !read_ok(dest, size) || !(rd32(c, cb + CB_TYPE) & TYPE_GS_VALID)) {
        ret(c, E_INVAL);
        return;
    }
    uint32_t w[3] = {AMPR_OP_SCATTER | (uint32_t)((dest >> 32) << 8), (uint32_t)(size - 1),
                     (uint32_t)dest};
    ret(c, append(c, cb, w, 12));
}

/* (cb, -, -, destination, size, file offset): the last read's file */
static void read_gather_scatter(hle_call *c) {
    guest_addr cb = hle_arg_ptr(c, 0), dest = hle_arg_ptr(c, 3);
    uint64_t size = hle_arg(c, 4), off = hle_arg(c, 5);
    if (!cb || !read_ok(dest, size) || off >= MAX_OFFSET ||
        !(rd32(c, cb + CB_TYPE) & TYPE_GS_VALID)) {
        ret(c, E_INVAL);
        return;
    }
    uint32_t w[5] = {(off >> 32 ? AMPR_OP_GATHER_SCATTER_EXT : AMPR_OP_GATHER_SCATTER) |
                         (uint32_t)((dest >> 32) << 8),
                     (uint32_t)(size - 1), (uint32_t)dest, (uint32_t)off, (uint32_t)(off >> 32)};
    ret(c, append(c, cb, w, off >> 32 ? 0x14 : 0x10));
}

static void reset_gather_scatter(hle_call *c) {
    guest_addr cb = hle_arg_ptr(c, 0);
    uint32_t w = AMPR_OP_RESET_GS;
    int32_t r = append(c, cb, &w, 4);
    if (!r)
        set_type(c, cb, 0, TYPE_GS_VALID);
    ret(c, r);
}

/* ---- Completion and synchronization ---- */

/* (cb, equeue, id, data, ...) */
static void write_event(hle_call *c) {
    uint8_t rec[0x20] = {0};
    uint32_t id = hle_arg_u32(c, 2);
    uint64_t eq = hle_arg(c, 1), data = hle_arg(c, 3);
    rec[0] = AMPR_OP_EVENT;
    memcpy(rec + 4, &id, 4), memcpy(rec + 8, &eq, 8), memcpy(rec + 16, &data, 8);
    ret(c, append(c, hle_arg_ptr(c, 0), rec, sizeof(rec)));
}

static void put_write(hle_call *c, guest_addr addr, uint64_t value, uint8_t kind) {
    if (!hle_arg_ptr(c, 0) || !addr || (addr & 7)) {
        ret(c, E_INVAL);
        return;
    }
    uint8_t rec[0x20] = {0};
    rec[0] = AMPR_OP_WRITE, rec[1] = kind;
    memcpy(rec + 8, &addr, 8), memcpy(rec + 16, &value, 8);
    ret(c, append(c, hle_arg_ptr(c, 0), rec, sizeof(rec)));
}

static void write_address(hle_call *c) {
    put_write(c, hle_arg_ptr(c, 1), hle_arg(c, 2), AMPR_WRITE_VALUE);
}
static void write_address_time(hle_call *c) {
    put_write(c, hle_arg_ptr(c, 1), 0, AMPR_WRITE_TIME);
}
static void write_address_counter(hle_call *c) {
    put_write(c, hle_arg_ptr(c, 1), 0, AMPR_WRITE_ZERO);
}

/* (cb, address, value, compare, flags) */
static void wait_on_address(hle_call *c) {
    guest_addr addr = hle_arg_ptr(c, 1);
    uint64_t value = hle_arg(c, 2);
    uint8_t cmp = (uint8_t)hle_arg(c, 3);
    if (!hle_arg_ptr(c, 0) || !addr || (addr & 7) || cmp > 6) {
        ret(c, E_INVAL);
        return;
    }
    uint8_t rec[0x20] = {0};
    rec[0] = AMPR_OP_WAIT, rec[1] = cmp;
    memcpy(rec + 8, &addr, 8), memcpy(rec + 16, &value, 8);
    ret(c, append(c, hle_arg_ptr(c, 0), rec, sizeof(rec)));
}

/* Commands without effect here: counters, markers, nops, the map brackets */
static void skip_0x20(hle_call *c) {
    ret(c, append_skip(c, hle_arg_ptr(c, 0), 0x20));
}
static void nop(hle_call *c) {
    uint32_t n = hle_arg_u32(c, 1);
    if (!n || n > 16) {
        ret(c, E_INVAL);
        return;
    }
    ret(c, append_skip(c, hle_arg_ptr(c, 0), n * 4));
}
static void nop_with_data(hle_call *c) {
    uint32_t n = hle_arg_u32(c, 1);
    if (n > 15) {
        ret(c, E_INVAL);
        return;
    }
    ret(c, append_skip(c, hle_arg_ptr(c, 0), (n + 1) * 4));
}
static void construct_nop(hle_call *c) {
    ret(c, append_skip(c, hle_arg_ptr(c, 0), 4 + hle_arg_u32(c, 3)));
}
static void construct_marker(hle_call *c) {
    ret(c, append_skip(c, hle_arg_ptr(c, 0),
                       marker_size(c, hle_arg_ptr(c, 2), hle_arg_ptr(c, 3) != 0)));
}
static void marker(hle_call *c) {
    ret(c, append_skip(c, hle_arg_ptr(c, 0), marker_size(c, hle_arg_ptr(c, 1), false)));
}
static void marker_color(hle_call *c) {
    ret(c, append_skip(c, hle_arg_ptr(c, 0), marker_size(c, hle_arg_ptr(c, 1), true)));
}
static void pop_marker(hle_call *c) {
    ret(c, append_skip(c, hle_arg_ptr(c, 0), 4));
}

static bool map_ok(uint64_t va, uint64_t size) {
    return va && size && !(va & 0x3FFF) && !(size & 0x3FFF) && va + size > va;
}
static void map_begin(hle_call *c) {
    guest_addr cb = hle_arg_ptr(c, 0);
    if (!cb || !map_ok(hle_arg(c, 1), hle_arg(c, 2))) {
        ret(c, E_INVAL);
        return;
    }
    int32_t r = append_skip(c, cb, 0x0C);
    if (!r)
        set_type(c, cb, TYPE_MAP_ACTIVE, 0);
    ret(c, r);
}
static void map_direct_begin(hle_call *c) {
    guest_addr cb = hle_arg_ptr(c, 0);
    if (!cb || !map_ok(hle_arg(c, 1), hle_arg(c, 3)) || (hle_arg(c, 2) & 0x3FFF)) {
        ret(c, E_INVAL);
        return;
    }
    int32_t r = append_skip(c, cb, 0x10);
    if (!r)
        set_type(c, cb, TYPE_MAP_ACTIVE, 0);
    ret(c, r);
}
static void map_end(hle_call *c) {
    guest_addr cb = hle_arg_ptr(c, 0);
    if (!cb || !(rd32(c, cb + CB_TYPE) & TYPE_MAP_ACTIVE)) {
        ret(c, E_PERM);
        return;
    }
    int32_t r = append_skip(c, cb, 4);
    if (!r)
        set_type(c, cb, 0, TYPE_MAP_ACTIVE);
    ret(c, r);
}

/* ---- Sizes ---- */

/* (-, destination, size, file offset) */
static void measure_read(hle_call *c) {
    uint64_t dest = hle_arg(c, 1), size = hle_arg(c, 2), off = hle_arg(c, 3);
    hle_ret_u64(c, read_ok(dest, size) && off < MAX_OFFSET ? (off >> 32 ? 0x18 : 0x14)
                                                           : (uint32_t)E_INVAL);
}
static void measure_gather(hle_call *c) {
    uint64_t size = hle_arg(c, 0), off = hle_arg(c, 1);
    hle_ret_u64(c, size && size <= MAX_READ && off < MAX_OFFSET ? (off > 0x3FFFF ? 0x0C : 0x08)
                                                                : (uint32_t)E_INVAL);
}
static void measure_scatter(hle_call *c) {
    hle_ret_u64(c, read_ok(hle_arg(c, 0), hle_arg(c, 1)) ? 0x0C : (uint32_t)E_INVAL);
}
static void measure_gather_scatter(hle_call *c) {
    uint64_t off = hle_arg(c, 2);
    hle_ret_u64(c, read_ok(hle_arg(c, 0), hle_arg(c, 1)) && off < MAX_OFFSET
                       ? (off >> 32 ? 0x14 : 0x10)
                       : (uint32_t)E_INVAL);
}
static void measure_4(hle_call *c) {
    hle_ret_u64(c, 4);
}
static void measure_0x20(hle_call *c) {
    hle_ret_u64(c, 0x20);
}
static void measure_nop(hle_call *c) {
    uint32_t n = hle_arg_u32(c, 0);
    hle_ret_u64(c, n ? align4((uint64_t)n * 4) : 4);
}
static void measure_nop_data(hle_call *c) {
    hle_ret_u64(c, align4(((uint64_t)hle_arg_u32(c, 0) + 1) * 4));
}
static void measure_marker(hle_call *c) {
    hle_ret_u64(c, marker_size(c, hle_arg_ptr(c, 0), false));
}
static void measure_marker_color(hle_call *c) {
    hle_ret_u64(c, marker_size(c, hle_arg_ptr(c, 0), true));
}
static void measure_map_begin(hle_call *c) {
    hle_ret_u64(c, map_ok(hle_arg(c, 0), hle_arg(c, 1)) ? 0x0C : (uint32_t)E_INVAL);
}
static void measure_map_direct_begin(hle_call *c) {
    hle_ret_u64(c, map_ok(hle_arg(c, 0), hle_arg(c, 2)) && !(hle_arg(c, 1) & 0x3FFF)
                       ? 0x10
                       : (uint32_t)E_INVAL);
}

/* ---- AMM: memory mapping through command buffers ---- */

/* The window games map AMM memory in (sceAmprAmmGetVirtualAddressRanges): 256 GiB below the
 * kernel's placement area (GUEST_PH_BASE), which nothing else places memory in */
#define AMM_VA_START 0x0000008000000000ull
#define AMM_VA_END 0x000000C000000000ull
#define AMM_PAGE 0x4000ull

/* AMPR and ACP access bits mean CPU access here (the copies run on the CPU) */
static uint32_t amm_prot(uint32_t prot) {
    uint32_t p = prot & 0x37;
    if (prot & 0x140) p |= 1;
    if (prot & 0x280) p |= 3;
    return p;
}

static bool amm_range_ok(uint64_t va, uint64_t size) {
    return va && size && !(va % AMM_PAGE) && !(size % AMM_PAGE) && va + size > va;
}

/* (cb, va, size, type, prot[, gpu mask id]) */
static void amm_map(hle_call *c) {
    uint64_t va = hle_arg(c, 1), size = hle_arg(c, 2);
    if (!amm_range_ok(va, size)) {
        ret(c, E_INVAL);
        return;
    }
    uint8_t rec[0x20] = {0};
    uint32_t w0 = AMPR_OP_AMM_MAP | (amm_prot(hle_arg_u32(c, 4)) & 0xFFFF) << 8, type = hle_arg_u32(c, 3);
    memcpy(rec, &w0, 4), memcpy(rec + 4, &type, 4), memcpy(rec + 8, &va, 8), memcpy(rec + 16, &size, 8);
    ret(c, append(c, hle_arg_ptr(c, 0), rec, sizeof(rec)));
}

/* (cb, va, dmem offset, size, type, prot[, gpu mask id]) */
static void amm_map_direct(hle_call *c) {
    uint64_t va = hle_arg(c, 1), phys = hle_arg(c, 2), size = hle_arg(c, 3);
    if (!amm_range_ok(va, size) || phys % AMM_PAGE) {
        ret(c, E_INVAL);
        return;
    }
    uint8_t rec[0x30] = {0};
    uint32_t w0 = AMPR_OP_AMM_MAP_DIRECT | (amm_prot(hle_arg_u32(c, 5)) & 0xFFFF) << 8, type = hle_arg_u32(c, 4);
    memcpy(rec, &w0, 4), memcpy(rec + 4, &type, 4), memcpy(rec + 8, &va, 8), memcpy(rec + 16, &size, 8);
    memcpy(rec + 24, &phys, 8);
    ret(c, append(c, hle_arg_ptr(c, 0), rec, sizeof(rec)));
}

/* (cb, va, size) */
static void amm_unmap(hle_call *c) {
    uint64_t va = hle_arg(c, 1), size = hle_arg(c, 2);
    if (!amm_range_ok(va, size)) {
        ret(c, E_INVAL);
        return;
    }
    uint8_t rec[0x20] = {AMPR_OP_AMM_UNMAP};
    memcpy(rec + 8, &va, 8), memcpy(rec + 16, &size, 8);
    ret(c, append(c, hle_arg_ptr(c, 0), rec, sizeof(rec)));
}

static void measure_0x30(hle_call *c) {
    hle_ret_u64(c, 0x30);
}

/* (search start, search end, size, align, usage, int64 *dmem offset) */
static void amm_give_direct_memory(hle_call *c) {
    uint64_t size = hle_arg(c, 2), align = hle_arg(c, 3), out = 0;
    guest_addr out_ptr = hle_arg_ptr(c, 5);
    if (!out_ptr || !size || size % 0x200000 || align % 0x200000 || hle_arg_u32(c, 4) > 1) {
        ret(c, E_INVAL);
        return;
    }
    int32_t e = kernel_amm_give_direct(hle_arg(c, 0), hle_arg(c, 1), size, align, &out);
    if (!e) wr64(c, out_ptr, out);
    ret(c, e);
}

/* (u64 *va start, *va end, *multimap va start, *multimap va end): no multimap window */
static void amm_get_virtual_address_ranges(hle_call *c) {
    if (hle_arg_ptr(c, 0)) wr64(c, hle_arg_ptr(c, 0), AMM_VA_START);
    for (int i = 1; i < 4; i++)
        if (hle_arg_ptr(c, i)) wr64(c, hle_arg_ptr(c, i), AMM_VA_END);
}

/* (stats *): size in bytes (in), 512 page table pool entries, none in use, all rings idle */
static void amm_get_usage_stats(hle_call *c) {
    guest_addr p = hle_arg_ptr(c, 0);
    uint64_t want = p ? rd64(c, p) : 0;
    if (!p || want > 0x18) {
        ret(c, E_INVAL);
        return;
    }
    uint8_t st[0x18] = {0};
    uint16_t pool = 512;
    uint32_t idle = 7;
    memcpy(st, &want, 8), memcpy(st + 8, &pool, 2), memcpy(st + 0x10, &idle, 4);
    ret(c, gmem_write(c->mem, p, st, (size_t)want) ? 0 : E_FAULT);
}

/* (cb, bytes, priority[, ...]): Submit2 adds uint32 *id, Submit3 result * and uint32 *id
 * (as KytyPS5 has them: AndGetId, AndGetResult) */
static bool amm_submit_ok(hle_call *c) {
    return hle_arg_ptr(c, 0) && hle_arg_u32(c, 1) && hle_arg_u32(c, 2) <= 2;
}
static void amm_submit(hle_call *c) {
    if (!amm_submit_ok(c)) {
        ret(c, E_INVAL);
        return;
    }
    apr_submit_amm(c, hle_arg_ptr(c, 0), hle_arg_u32(c, 1), hle_arg_u32(c, 2), 0);
    ret(c, 0);
}
static void amm_submit_and_get_id(hle_call *c) {
    if (!amm_submit_ok(c) || !hle_arg_ptr(c, 3)) {
        ret(c, E_INVAL);
        return;
    }
    wr32(c, hle_arg_ptr(c, 3), apr_submit_amm(c, hle_arg_ptr(c, 0), hle_arg_u32(c, 1), hle_arg_u32(c, 2), 0));
    ret(c, 0);
}
static void amm_submit_and_get_result(hle_call *c) {
    if (!amm_submit_ok(c)) {
        ret(c, E_INVAL);
        return;
    }
    uint32_t id = apr_submit_amm(c, hle_arg_ptr(c, 0), hle_arg_u32(c, 1), hle_arg_u32(c, 2), hle_arg_ptr(c, 3));
    if (hle_arg_ptr(c, 4)) wr32(c, hle_arg_ptr(c, 4), id);
    ret(c, 0);
}
static void amm_wait_completion(hle_call *c) {
    apr_wait(hle_arg_u32(c, 0));
    ret(c, 0);
}

#define L "libSceAmpr"
#define F(name, fn) HLE_FUNC_ENTRY(L, name, fn)

static const hle_export k_exports[] = {
    F("sceAmprAmmCommandBufferMap", amm_map),
    F("sceAmprAmmCommandBufferMapWithGpuMaskId", amm_map),
    F("sceAmprAmmCommandBufferMapDirect", amm_map_direct),
    F("sceAmprAmmCommandBufferMapDirectWithGpuMaskId", amm_map_direct),
    F("sceAmprAmmCommandBufferUnmap", amm_unmap),
    F("sceAmprAmmMeasureAmmCommandSizeMap", measure_0x20),
    F("sceAmprAmmMeasureAmmCommandSizeMapWithGpuMaskId", measure_0x20),
    F("sceAmprAmmMeasureAmmCommandSizeMapDirect", measure_0x30),
    F("sceAmprAmmMeasureAmmCommandSizeMapDirectWithGpuMaskId", measure_0x30),
    F("sceAmprAmmMeasureAmmCommandSizeUnmap", measure_0x20),
    F("sceAmprAmmGiveDirectMemory", amm_give_direct_memory),
    F("sceAmprAmmGetVirtualAddressRanges", amm_get_virtual_address_ranges),
    F("sceAmprAmmGetUsageStatsData", amm_get_usage_stats),
    F("sceAmprAmmSetPageTablePoolOccupancyNotificationThreshold", ok),
    F("sceAmprAmmSubmitCommandBuffer", amm_submit),
    F("sceAmprAmmSubmitCommandBuffer2", amm_submit_and_get_id),
    F("sceAmprAmmSubmitCommandBuffer3", amm_submit_and_get_result),
    F("sceAmprAmmWaitCommandBufferCompletion", amm_wait_completion),
    F("sceAmprCommandBufferConstructor", cb_constructor),
    F("sceAmprCommandBufferDestructor", ok),
    F("sceAmprAprCommandBufferConstructor", apr_constructor),
    F("sceAmprAprCommandBufferDestructor", ok),
    F("sceAmprAmmCommandBufferConstructor", cb_constructor),
    F("sceAmprAmmCommandBufferDestructor", ok),
    F("sceAmprCommandBufferSetBuffer", set_buffer),
    F("sceAmprCommandBufferReset", reset),
    F("sceAmprCommandBufferClearBuffer", clear_buffer),
    F("sceAmprCommandBufferGetSize", get_size),
    F("sceAmprCommandBufferGetCurrentOffset", get_offset),
    F("sceAmprCommandBufferGetType", get_type),
    F("sceAmprCommandBufferGetBufferBaseAddress", get_base),
    F("sceAmprCommandBufferGetNumCommands", get_num),
    F("sceAmprAprCommandBufferReadFile", read_file),
    F("sceAmprAprCommandBufferReadFileGather", read_gather),
    F("sceAmprAprCommandBufferReadFileScatter", read_scatter),
    F("sceAmprAprCommandBufferReadFileGatherScatter", read_gather_scatter),
    F("sceAmprAprCommandBufferResetGatherScatterState", reset_gather_scatter),
    F("sceAmprAprCommandBufferMapBegin", map_begin),
    F("sceAmprAprCommandBufferMapDirectBegin", map_direct_begin),
    F("sceAmprAprCommandBufferMapEnd", map_end),
    F("sceAmprCommandBufferWriteKernelEventQueue_04_00", write_event),
    F("sceAmprCommandBufferWriteKernelEventQueueOnCompletion", write_event),
    F("sceAmprCommandBufferWriteAddress_04_00", write_address),
    F("sceAmprCommandBufferWriteAddressOnCompletion", write_address),
    F("sceAmprCommandBufferWriteAddressFromTimeCounter_04_00", write_address_time),
    F("sceAmprCommandBufferWriteAddressFromTimeCounterOnCompletion", write_address_time),
    F("sceAmprCommandBufferWriteAddressFromCounter_04_00", write_address_counter),
    F("sceAmprCommandBufferWriteAddressFromCounterOnCompletion", write_address_counter),
    F("sceAmprCommandBufferWriteAddressFromCounterPair_04_00", write_address_counter),
    F("sceAmprCommandBufferWriteAddressFromCounterPairOnCompletion", write_address_counter),
    F("sceAmprCommandBufferWriteCounter_04_00", skip_0x20),
    F("sceAmprCommandBufferWriteCounterOnCompletion", skip_0x20),
    F("sceAmprCommandBufferWaitOnAddress", wait_on_address),
    F("sceAmprCommandBufferWaitOnAddress_04_00", wait_on_address),
    F("sceAmprCommandBufferWaitOnCounter", skip_0x20),
    F("sceAmprCommandBufferWaitOnCounter_04_00", skip_0x20),
    F("sceAmprCommandBufferNop", nop),
    F("sceAmprCommandBufferNopWithData", nop_with_data),
    F("sceAmprCommandBufferConstructNop", construct_nop),
    F("sceAmprCommandBufferConstructMarker", construct_marker),
    F("sceAmprCommandBufferSetMarker", marker),
    F("sceAmprCommandBufferSetMarkerWithColor", marker_color),
    F("sceAmprCommandBufferPushMarker", marker),
    F("sceAmprCommandBufferPushMarkerWithColor", marker_color),
    F("sceAmprCommandBufferPopMarker", pop_marker),
    F("sceAmprMeasureCommandSizeReadFile", measure_read),
    F("sceAmprMeasureCommandSizeReadFileGather", measure_gather),
    F("sceAmprMeasureCommandSizeReadFileScatter", measure_scatter),
    F("sceAmprMeasureCommandSizeReadFileGatherScatter", measure_gather_scatter),
    F("sceAmprMeasureCommandSizeResetGatherScatterState", measure_4),
    F("sceAmprMeasureCommandSizeMapBegin", measure_map_begin),
    F("sceAmprMeasureCommandSizeMapDirectBegin", measure_map_direct_begin),
    F("sceAmprMeasureCommandSizeMapEnd", measure_4),
    F("sceAmprMeasureCommandSizeNop", measure_nop),
    F("sceAmprMeasureCommandSizeNopWithData", measure_nop_data),
    F("sceAmprMeasureCommandSizeSetMarker", measure_marker),
    F("sceAmprMeasureCommandSizeSetMarkerWithColor", measure_marker_color),
    F("sceAmprMeasureCommandSizePushMarker", measure_marker),
    F("sceAmprMeasureCommandSizePushMarkerWithColor", measure_marker_color),
    F("sceAmprMeasureCommandSizePopMarker", measure_4),
    F("sceAmprMeasureCommandSizeWriteKernelEventQueue_04_00", measure_0x20),
    F("sceAmprMeasureCommandSizeWriteKernelEventQueueOnCompletion", measure_0x20),
    F("sceAmprMeasureCommandSizeWriteAddress_04_00", measure_0x20),
    F("sceAmprMeasureCommandSizeWriteAddressOnCompletion", measure_0x20),
    F("sceAmprMeasureCommandSizeWriteAddressFromTimeCounter_04_00", measure_0x20),
    F("sceAmprMeasureCommandSizeWriteAddressFromTimeCounterOnCompletion", measure_0x20),
    F("sceAmprMeasureCommandSizeWriteAddressFromCounter_04_00", measure_0x20),
    F("sceAmprMeasureCommandSizeWriteAddressFromCounterOnCompletion", measure_0x20),
    F("sceAmprMeasureCommandSizeWriteAddressFromCounterPair_04_00", measure_0x20),
    F("sceAmprMeasureCommandSizeWriteAddressFromCounterPairOnCompletion", measure_0x20),
    F("sceAmprMeasureCommandSizeWriteCounter_04_00", measure_0x20),
    F("sceAmprMeasureCommandSizeWriteCounterOnCompletion", measure_0x20),
    F("sceAmprMeasureCommandSizeWaitOnAddress", measure_0x20),
    F("sceAmprMeasureCommandSizeWaitOnAddress_04_00", measure_0x20),
    F("sceAmprMeasureCommandSizeWaitOnCounter", measure_0x20),
    F("sceAmprMeasureCommandSizeWaitOnCounter_04_00", measure_0x20),
};

void hle_register_ampr(void) {
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}

uint64_t ampr_time_counter(void) {
    return __rdtsc();
}
