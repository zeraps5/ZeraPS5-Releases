/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GPU command processor (PM4, AMD RDNA2 as used by the PS5).
 *
 * Current stage: "null GPU". Command buffers are walked and only the
 * packets that talk back to the CPU are executed: memory writes and end-of-
 * pipe releases (labels/fences), interrupts, DMA, indirect buffers and the
 * driver's flip packets. Draws and dispatches are counted, not rendered.
 *
 * The processor knows nothing about HLE: flips and interrupts go out through
 * callbacks. Guest memory is reached through gmem. */
#ifndef PS5C_GPU_H
#define PS5C_GPU_H

#include "base/base.h"
#include "mem/gmem.h"
#include "pal/pal.h"

/* Driver-private packet: a PM4 NOP whose first payload dword is this tag.
 * Emitted by our libSceAgcDriver (flip, wait-for-display, ...). */
#define GPU_DRIVER_TAG 0x43355350u /* "PS5C" */
enum { GPU_DRIVER_FLIP = 1, GPU_DRIVER_WAIT_DISPLAY = 2 };
#define GPU_FLIP_PACKET_DWORDS 8 /* header, tag, kind, handle, index, mode, arg lo, arg hi */
#define GPU_WAIT_DISPLAY_PACKET_DWORDS 5 /* header, tag, kind, handle, index */

typedef struct gpu gpu;

#define GPU_IB_FRAMES 5
typedef enum gpu_exec_result { GPU_EXEC_DONE, GPU_EXEC_BLOCKED, GPU_EXEC_YIELD, GPU_EXEC_FAULT } gpu_exec_result;
typedef struct gpu_submission {
    struct { guest_addr addr; uint32_t dwords, pos; } frame[GPU_IB_FRAMES];
    uint32_t depth;
    bool initialized, done, blocked, yielded, fault, had_interrupt;
    uint32_t budget;
    uint64_t executed;
    guest_addr wait_packet;
    uint32_t wait_opcode;
} gpu_submission;

typedef struct gpu_draw {
    bool indexed;
    uint32_t count;        /* vertices or indices */
    uint32_t instances;
    guest_addr index_addr; /* indexed draws */
    uint32_t index_size;   /* bytes per index: 2 or 4 */
} gpu_draw;

/* End of pipe: a label write and/or an interrupt the CPU waits on (RELEASE_MEM,
 * EVENT_WRITE_EOP, the driver's end of a graphics submission). */
typedef struct gpu_release {
    guest_addr addr;
    uint64_t data;
    uint32_t data_sel; /* 0 none, 1 data 32-bit, 2 data 64-bit, 3/4 GPU clock 64-bit */
    bool interrupt;
    int32_t queue_event;
    uint32_t context_id;
} gpu_release;

typedef struct gpu_callbacks {
    void *user;
    void (*flip)(void *user, int32_t handle, int32_t index, int32_t mode, int64_t arg);
    /* End-of-pipe interrupt on a queue (0 = graphics). */
    void (*interrupt)(void *user, int32_t queue_event, uint32_t context_id);
    /* Rendering backend (optional): called with the register state current. */
    void (*draw)(void *user, const gpu *g, const gpu_draw *d);
    void (*dispatch)(void *user, const gpu *g, uint32_t x, uint32_t y, uint32_t z);
    /* Guest memory the command processor wrote (DMA_DATA, WRITE_DATA, COPY_DATA, ATOMIC_MEM). */
    void (*mem_written)(void *user, guest_addr addr, uint64_t size);
    /* The command processor is about to read guest memory that earlier GPU work may still hold (indirect arguments,
     * DMA sources, labels): results there land first, and so do the releases the backend holds that write there. */
    void (*sync_mem)(void *user, guest_addr addr, uint64_t size);
    /* End of pipe (optional): the backend may hold the release until the GPU work recorded so far completes and
     * perform it then with gpu_perform_release (true when it took it); else it happens now. */
    bool (*release)(void *user, gpu *g, const gpu_release *rel);
    /* The command processor is about to write guest memory now (WRITE_DATA, DMA_DATA, COPY_DATA, ATOMIC_MEM): the
     * releases the backend holds came first and must land first. A game that sees the later write may reuse the
     * memory a held label goes to (Sonic Frontiers recycles its command buffers, and the late label broke them). */
    void (*cp_write)(void *user);
    /* Optional conservative binding feedback. NULL/0/0 queries enablement;
     * report/reset calls are serialized with rendering by the backend. */
    bool (*lod_stats)(void *user,void *out,uint32_t bytes,uint32_t flags);
    /* NULL queries support. Otherwise completes older draws and returns the
     * cumulative depth-passing sample count; never an always-visible answer. */
    bool (*sample_counter)(void *user,uint64_t *value);
    /* Optional: returns once buffer `index` of display `handle` is neither waiting to be shown nor being shown
     * (sceAgcDriverWaitUntilSafeForRendering), so the frame drawn into it next does not reach the screen half done. */
    void (*wait_display)(void *user, int32_t handle, int32_t index);
} gpu_callbacks;

typedef struct gpu_stats {
    uint64_t submits, packets, draws, dispatches, releases, interrupts, flips, unknown;
} gpu_stats;

struct gpu {
    gmem *mem;
    pal_mutex *lock; /* queues submit from several threads */
    uint32_t *regs;  /* GPU_REG_COUNT dwords, indexed by absolute offset */
    /* Draw state set by packets rather than registers */
    guest_addr index_base;
    uint32_t index_buffer_size; /* in indices */
    uint32_t index_type;        /* 0: 16-bit, 1: 32-bit */
    uint32_t num_instances;
    guest_addr indirect_base[2]; /* SET_BASE: indirect arguments of draws [0] and dispatches [1] */
    gpu_callbacks cb;
    gpu_stats stats;
    uint8_t seen_unknown[256]; /* opcodes already reported */
    int64_t dump_frame; /* PS5C_GPU_DUMP: log draws (and packets) of this frame (-1: off) */
    /* CLEAR_STATE's PUSH_STATE / POP_STATE: saved context registers */
    uint32_t *ctx_stack;
    uint32_t ctx_depth;
    uint64_t interrupts_issued; /* releases with an interrupt the stream asked for, held or not (GPU lock) */
    /* the command buffers being run, outermost first (labels inside them: cp_label_write) */
    guest_addr run_lo[8], run_hi[8];
    uint32_t run_depth;
    gpu_submission *active_submission; /* borrowed only while resuming under the GPU lock */
    bool gcn; /* a PS4 game's streams (Gnm): its draws use the VS stage and GCN shaders */
};

ps5c_err gpu_init(gpu *g, gmem *mem, const gpu_callbacks *cb);
static inline uint32_t gpu_reg(const gpu *g, uint32_t offset) { return g->regs[offset & 0xFFFF]; }
/* 40-bit address from a LO/HI register pair holding address >> 8. */
static inline guest_addr gpu_reg_addr256(const gpu *g, uint32_t lo) {
    return ((guest_addr)gpu_reg(g, lo) << 8) | ((guest_addr)(gpu_reg(g, lo + 1) & 0xFF) << 40);
}
/* Processes `dwords` dwords at guest address `addr`. queue_event: the event
 * id end-of-pipe interrupts raise (0 for the graphics queue). */
void gpu_submit(gpu *g, guest_addr addr, uint32_t dwords, int32_t queue_event);
/* A zero-initialized submission retains its IB stack at a pending wait.
 * The caller must restore the queue's register state before each resume. */
gpu_exec_result gpu_submit_resume(gpu *g, gpu_submission *s, guest_addr addr, uint32_t dwords, int32_t queue_event);

/* Writes the label and raises the interrupt of a release (any thread; does not take the GPU lock). */
void gpu_perform_release(gpu *g, const gpu_release *rel);
/* The last command-processor write to a label (hashed, may be forgotten): false if none seen. */
bool gpu_last_write(guest_addr a, char *out, size_t size);

/* Length in dwords of the shader program at `addr` (through s_endpgm), 0 if unknown. */
uint32_t gpu_shader_size(const gpu *g, guest_addr addr);

/* Writes a driver packet (NOP + tag) of `dwords` dwords at `dst`. */
void gpu_write_driver_packet(uint32_t *dst, uint32_t dwords, const uint32_t *payload, uint32_t payload_dwords);

#endif
