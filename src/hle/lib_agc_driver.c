/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceAgcDriver: the GPU driver interface under libSceAgc.
 *
 * Sony's libSceAgc runs LLE (hybrid policy) and builds command buffers and
 * register tables itself; this library is where those reach the GPU, so it is
 * HLE: submissions go to the command processor (src/gpu), flips to VideoOut,
 * end-of-pipe interrupts to event queues.
 *
 * Some entry points are private to libSceAgc: it asks us how many dwords a
 * driver packet needs, reserves them in the command buffer and lets us write
 * them. Those packets are ours (NOP + GPU_DRIVER_TAG), executed by src/gpu. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "core/settings.h"
#include "gpu/gpu.h"
#include "gpu/gpu_regs.h"
#include "gpu/gpu_sampler.h"
#include "gpu/renderer.h"
#include "hle/equeue.h"
#include "hle/hle.h"
#include "hle/video_out.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

#define LIB "libSceAgcDriver"

/* libSceAgc requires its private area at exactly this address ("FS Table
 * offset has shifted. This is not survivable."). */
#define AGC_RESERVED_ADDR 0xfe0040000ull
#define AGC_RESERVED_SIZE 0x100000u

#define COMPUTE_QUEUE_EVENT_BASE 0x20

typedef struct eq_registration {
    uint64_t eq;
    int32_t id;
} eq_registration;

/* Submissions run on a GPU thread, after the submit call returns, as on the
 * console: engines reset completion labels right after submitting (Unreal's
 * Agc RHI does), which a synchronous GPU would already have written. */
#define SUBMIT_RING 1024
#define CP_QUEUES 64

/* Register state belongs to a hardware queue, not the suspended CPU stack.
 * Swapping pointers avoids copying 256 KiB every time another queue runs. */
typedef struct queue_state {
    bool used;
    int32_t event;
    uint32_t *regs, *ctx_stack, ctx_depth;
    guest_addr index_base, indirect_base[2];
    uint32_t index_buffer_size, index_type, num_instances;
} queue_state;

typedef struct gpu_job {
    guest_addr addr;
    uint32_t dwords;
    int32_t queue_event;
    uint32_t *snap; /* PS5C_LOG_OVERRUN: the buffer as submitted */
    uint64_t t;
    bool suspend; /* finish this packet and all earlier work before returning */
    uint64_t ticket, retry_ns, blocked_ns;
    uint32_t queue;
    bool done;
    bool reported_stuck;
    gpu_submission stream;
} gpu_job;

static struct {
    pal_mutex *lock;
    gpu gpu;
    bool gpu_ready;
    eq_registration *events;
    uint32_t event_count, event_capacity;
    /* submission queue (lock: q_lock) */
    pal_mutex *q_lock;
    pal_cond *q_changed;
    gpu_job ring[SUBMIT_RING];
    uint32_t count;
    uint64_t flips_recorded; /* flip packets written into command buffers (sceAgcDriverSetFlip) */
    uint64_t submitted, completed;
    pal_thread *worker;
    bool cp_waits;
    queue_state queues[CP_QUEUES];
    uint32_t current_queue, next_queue;
} g_drv;

/* What the GPU thread is doing, for the log of a suspension that never
 * completes (Astro Bot: the ring stopped behind a job the worker never
 * finished). Written by the worker, read by the waiting CPU thread. */
static struct {
    _Atomic(const char *) phase;
    _Atomic uint64_t ticket, since_ns;
    _Atomic uint32_t callbacks; /* callbacks run inside the current job (progress) */
    uintptr_t handle, stack_top;
} g_worker;

static void worker_phase(const char *phase) {
    atomic_store_explicit(&g_worker.phase, phase, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_worker.callbacks, 1, memory_order_relaxed);
}

/* Logs the worker's phase and host stack three times 100 ms apart: a changing
 * pc or callback count means a loop, a fixed one a blocking wait. Executable
 * return addresses resolve with addr2line after rebasing on gpu_worker. */
static int32_t gpu_worker(void *arg);
static void log_worker_state(void) {
    for (int k = 0; k < 3; k++) {
        if (k) pal_sleep_ns(100000000ull);
        uint64_t pc = 0, sp = 0, fp = 0, ticket = atomic_load(&g_worker.ticket);
        const char *phase = atomic_load(&g_worker.phase);
        uint64_t since = atomic_load(&g_worker.since_ns);
        bool sampled = pal_thread_sample(g_worker.handle, &pc, &sp, &fp);
        LOG_W(GPU, "  GPU thread: %s, ticket %llu for %.1f s, %u callbacks, pc 0x%llx sp 0x%llx (gpu_worker at %p)",
              phase ? phase : "?", (unsigned long long)ticket, since ? (pal_time_ns() - since) / 1e9 : 0.0,
              atomic_load(&g_worker.callbacks), (unsigned long long)pc, (unsigned long long)sp, (void *)(uintptr_t)gpu_worker);
        if (!sampled || sp >= g_worker.stack_top || g_worker.stack_top - sp > (1u << 20)) continue;
        /* the stack between sp and the worker's first frame is committed: candidates in the image */
        uintptr_t self = (uintptr_t)gpu_worker;
        char line[700];
        int n = snprintf(line, sizeof(line), "    host stack:");
        for (uintptr_t a = (uintptr_t)sp & ~7u; a + 8 <= g_worker.stack_top && n < 680; a += 8) {
            uint64_t v = *(const uint64_t *)a;
            if (v + 0x4000000u > self && v < self + 0x4000000u) n += snprintf(line + n, sizeof(line) - (size_t)n, " %llx", (unsigned long long)v);
        }
        LOG_W(GPU, "%s", line);
    }
}

/* ---- GPU callbacks ---- */

static void on_flip(void *user, int32_t handle, int32_t index, int32_t mode, int64_t arg) {
    PS5C_UNUSED(user);
    worker_phase("flip");
    if (video_out_submit_flip_wait(handle, index, mode, arg, 100000000ull) < 0)
        LOG_W(GPU, "flip of buffer %d dropped: the display queue stayed full", index);
}

static void on_wait_display(void *user, int32_t handle, int32_t index) {
    PS5C_UNUSED(user);
    worker_phase("wait for display buffer");
    video_out_wait_buffer(handle, index, 100000000ull);
}

static void on_interrupt(void *user, int32_t queue_event, uint32_t context_id) {
    PS5C_UNUSED(user);
    worker_phase("interrupt");
    /* Graphics kevent payload: queue in bits 0..15, the RELEASE_MEM
     * 28-bit context in bits 16..43. ident remains the registration ID. */
    int64_t data = (int64_t)(((uint64_t)(context_id & 0x0fffffffu) << 16) |
                             ((uint32_t)queue_event & 0xffffu));
    pal_mutex_lock(g_drv.lock);
    for (uint32_t i = 0; i < g_drv.event_count; i++) {
        if (g_drv.events[i].id == queue_event) {
            /* one event per interrupt: the data (context id) of each is needed */
            equeue_push(g_drv.events[i].eq, (uint64_t)queue_event, EVFILT_GRAPHICS_, data);
        }
    }
    pal_mutex_unlock(g_drv.lock);
}

static void on_draw(void *user, const gpu *g, const gpu_draw *d) {
    PS5C_UNUSED(user);
    worker_phase("draw");
    renderer *r = video_out_renderer();
    if (r) renderer_draw(r, g, d);
}

static void on_dispatch(void *user, const gpu *g, uint32_t x, uint32_t y, uint32_t z) {
    PS5C_UNUSED(user);
    worker_phase("dispatch");
    renderer *r = video_out_renderer();
    if (r) renderer_dispatch(r, g, x, y, z);
}

static void on_mem_written(void *user, guest_addr addr, uint64_t size) {
    PS5C_UNUSED(user);
    worker_phase("mem written");
    renderer *r = video_out_renderer();
    if (r) renderer_mem_written(r, addr, size);
}

static void on_sync_mem(void *user, guest_addr addr, uint64_t size) {
    PS5C_UNUSED(user);
    worker_phase("sync mem");
    renderer *r = video_out_renderer();
    if (r) renderer_sync_mem(r, addr, size);
}

static bool on_release(void *user, gpu *g, const gpu_release *rel) {
    PS5C_UNUSED(user);
    worker_phase("release");
    renderer *r = video_out_renderer();
    return r && renderer_release(r, g, rel);
}

static bool on_lod_stats(void *user,void *out,uint32_t bytes,uint32_t flags) {
    PS5C_UNUSED(user);
    return renderer_lod_stats(video_out_renderer(),out,bytes,flags);
}

static bool on_sample_counter(void *user,uint64_t *value) {
    PS5C_UNUSED(user);
    return renderer_sample_counter(video_out_renderer(),value);
}

static void on_cp_write(void *user) {
    PS5C_UNUSED(user);
    worker_phase("cp write");
    renderer *r = video_out_renderer();
    if (r) renderer_cp_write(r);
}

/* A suspension point runs once all older work is done, except work of other
 * queues parked on a CP wait: those may wait for graphics work the CPU submits
 * only after this suspension returns (No Man's Sky: an async compute queue waits
 * for a label each frame's graphics releases). Lock: q_lock. */
static bool suspend_ready(uint64_t ticket) {
    bool parked[CP_QUEUES] = {false}, seen[CP_QUEUES] = {false};
    for (uint64_t t = g_drv.completed + 1; t < ticket; t++) {
        const gpu_job *j = &g_drv.ring[t % SUBMIT_RING];
        if (j->done) continue;
        /* the queue's oldest pending job decides: later ones of a parked queue wait behind it */
        if (!seen[j->queue]) seen[j->queue] = true, parked[j->queue] = j->blocked_ns != 0;
        if (!parked[j->queue]) return false;
    }
    return true;
}

/* the job with this ticket has run (it may stay in the ring behind an older blocked one) */
static bool ticket_done(uint64_t ticket) {
    const gpu_job *j = &g_drv.ring[ticket % SUBMIT_RING];
    return g_drv.completed >= ticket || (j->ticket == ticket && j->done);
}

static int32_t gpu_worker(void *arg) {
    PS5C_UNUSED(arg);
    volatile char top_marker = 0;
    g_worker.stack_top = (uintptr_t)&top_marker;
    g_worker.handle = pal_thread_open_current();
    gpu_sampler_start();
    /* PS5C_GPU_FAST_CORES=1: the command processor and recording run on P-cores (experiment) */
    const char *fast = getenv("PS5C_GPU_FAST_CORES");
    if (fast && atoi(fast)) LOG_I(GPU, "GPU thread on the fast cores: %s", pal_thread_prefer_fast_cores() ? "yes" : "no (one kind of core)");
    for (;;) {
        worker_phase("idle");
        pal_mutex_lock(g_drv.q_lock);
        while (!g_drv.count) pal_cond_wait(g_drv.q_changed, g_drv.q_lock);
        uint64_t selected = g_drv.completed + 1;
        if (g_drv.cp_waits) {
            uint64_t first_ticket[CP_QUEUES] = {0}, now_ns = pal_time_ns();
            for (uint64_t t = g_drv.completed + 1; t <= g_drv.submitted; t++) {
                gpu_job *j = &g_drv.ring[t % SUBMIT_RING];
                if (!j->done && !first_ticket[j->queue]) first_ticket[j->queue] = t;
            }
            selected = 0;
            for (uint32_t n = 0; n < CP_QUEUES; n++) {
                uint32_t q = (g_drv.next_queue + n) % CP_QUEUES;
                if (!first_ticket[q]) continue;
                gpu_job *j = &g_drv.ring[first_ticket[q] % SUBMIT_RING];
                if (j->blocked_ns && !j->suspend) {
                    /* An empty suspension behind work of its own queue parked on a CP wait
                     * goes first: that wait may be for graphics work the CPU submits only
                     * after the suspension returns (Halloween: the graphics queue waits for
                     * a label its later frames release). It carries no commands to reorder. */
                    for (uint64_t t = first_ticket[q] + 1; t <= g_drv.submitted; t++) {
                        gpu_job *later = &g_drv.ring[t % SUBMIT_RING];
                        if (later->done || later->queue != q) continue;
                        if (later->suspend && !later->dwords && suspend_ready(later->ticket)) j = later;
                        break;
                    }
                    if (j->suspend) {
                        selected = j->ticket;
                        g_drv.next_queue = (q + 1) % CP_QUEUES;
                        break;
                    }
                }
                if (j->retry_ns > now_ns || (j->suspend && !suspend_ready(j->ticket))) continue;
                selected = first_ticket[q];
                g_drv.next_queue = (q + 1) % CP_QUEUES;
                break;
            }
        }
        if (!selected) {
            /* A later queue may have finished while an older REWIND is
             * blocked. Its held labels/interrupts must reach the CPU even
             * though the ticket prefix cannot retire yet. */
            pal_mutex_unlock(g_drv.q_lock);
            worker_phase("renderer_gpu_idle (no runnable job)");
            renderer *idle_renderer = video_out_renderer();
            if (idle_renderer) renderer_gpu_idle(idle_renderer);
            pal_mutex_lock(g_drv.q_lock);
            /* CPU patches to REWIND/labels need not call the driver. */
            pal_cond_wait_ns(g_drv.q_changed, g_drv.q_lock, 1000000);
            pal_mutex_unlock(g_drv.q_lock);
            continue;
        }
        gpu_job job = g_drv.ring[selected % SUBMIT_RING];
        pal_mutex_unlock(g_drv.q_lock);
        atomic_store(&g_worker.ticket, job.ticket);
        atomic_store(&g_worker.since_ns, pal_time_ns());
        atomic_store(&g_worker.callbacks, 0);
        if (g_drv.cp_waits && job.queue != g_drv.current_queue) {
            gpu *g = &g_drv.gpu;
            queue_state *old = &g_drv.queues[g_drv.current_queue], *next = &g_drv.queues[job.queue];
            old->regs = g->regs; old->ctx_stack = g->ctx_stack; old->ctx_depth = g->ctx_depth;
            old->index_base = g->index_base; old->index_buffer_size = g->index_buffer_size;
            old->index_type = g->index_type; old->num_instances = g->num_instances;
            memcpy(old->indirect_base, g->indirect_base, sizeof(old->indirect_base));
            if (!next->regs) next->regs = calloc(GPU_REG_COUNT, sizeof(uint32_t));
            if (!next->regs) pal_exit_process(3);
            g->regs = next->regs; g->ctx_stack = next->ctx_stack; g->ctx_depth = next->ctx_depth;
            g->index_base = next->index_base; g->index_buffer_size = next->index_buffer_size;
            g->index_type = next->index_type; g->num_instances = next->num_instances;
            memcpy(g->indirect_base, next->indirect_base, sizeof(g->indirect_base));
            g_drv.current_queue = job.queue;
        }
        renderer *r = video_out_renderer();
        /* not for each retry of a parked wait: a new epoch makes the other queues read all memory again
         * (No Man's Sky's graphics crawled at one command buffer per ~3 s while a compute queue waited) */
        worker_phase("renderer_submit_begin");
        if (r && (!job.suspend || job.dwords) && !job.blocked_ns) renderer_submit_begin(r);
        if (job.snap) {
            const uint32_t *now = (const uint32_t *)gmem_host(g_drv.gpu.mem, job.addr, (uint64_t)job.dwords * 4);
            uint32_t first = job.dwords, last = 0, n = 0;
            for (uint32_t i = 0; now && i < job.dwords; i++)
                if (now[i] != job.snap[i]) {
                    if (first == job.dwords) first = i;
                    last = i;
                    n++;
                }
            if (n)
                LOG_W(GPU, "buffer 0x%llx (%u dwords) changed after submit: %u dwords in %u..%u (%08x -> %08x), %.1f ms later, %u queued",
                      (unsigned long long)job.addr, job.dwords, n, first, last, job.snap[first], now[first], (pal_time_ns() - job.t) / 1e6,
                      g_drv.count);
            free(job.snap);
            job.snap = NULL;
        }
        gpu_exec_result result = GPU_EXEC_DONE;
        uint64_t executed = job.stream.executed;
        worker_phase("command processor");
        if (!job.suspend || job.dwords) {
            if (g_drv.cp_waits) result = gpu_submit_resume(&g_drv.gpu, &job.stream, job.addr, job.dwords, job.queue_event);
            else gpu_submit(&g_drv.gpu, job.addr, job.dwords, job.queue_event);
        }
        if (result == GPU_EXEC_FAULT) {
            LOG_E(GPU, "invalid resumable CP stream ticket %llu queue %d at 0x%llx opcode %02x",
                  (unsigned long long)job.ticket, job.queue_event, (unsigned long long)job.stream.wait_packet,
                  job.stream.wait_opcode);
            pal_exit_process(3);
        }
        if (result == GPU_EXEC_BLOCKED || result == GPU_EXEC_YIELD) {
            if (result == GPU_EXEC_BLOCKED && !job.blocked_ns) {
                job.blocked_ns = pal_time_ns();
                static uint64_t reported;
                if (getenv("PS5C_TRACE_CP_WAIT") && ++reported <= 64)
                    LOG_I(GPU, "CP suspended ticket %llu queue %d at 0x%llx opcode %02x",
                          (unsigned long long)job.ticket, job.queue_event, (unsigned long long)job.stream.wait_packet,
                          job.stream.wait_opcode);
            }
            /* a wait still unsatisfied after 10 s is a hang: log the packet once per ticket */
            if (result == GPU_EXEC_BLOCKED && !job.reported_stuck && pal_time_ns() - job.blocked_ns > 10000000000ull) {
                job.reported_stuck = true;
                const uint32_t *w = (const uint32_t *)gmem_host(g_drv.gpu.mem, job.stream.wait_packet, 36);
                uint64_t value = 0;
                guest_addr a = 0;
                if (w && job.stream.wait_opcode == 0x39) {
                    a = (guest_addr)(w[1] & ~7u) | ((guest_addr)(w[2] & 0x3ffffu) << 32);
                    gmem_read_u64(g_drv.gpu.mem, a, &value);
                } else if (w && job.stream.wait_opcode != 0x59 && (w[1] & 0x10)) {
                    a = (guest_addr)(w[2] & ~3u) | ((guest_addr)(w[3] & 0x3ffffu) << 32);
                    gmem_read(g_drv.gpu.mem, a, &value, job.stream.wait_opcode == 0x93 ? 8 : 4);
                }
                char last[160];
                if (a && gpu_last_write(a, last, sizeof(last)))
                    LOG_W(GPU, "  label 0x%llx last written by the command processor: %s", (unsigned long long)a, last);
                else if (a)
                    LOG_W(GPU, "  label 0x%llx: no command-processor write seen (the CPU or a shader writes it)",
                          (unsigned long long)a);
                if (w)
                    LOG_W(GPU, "CP ticket %llu queue %d blocked 10 s at 0x%llx: %08x %08x %08x %08x %08x %08x %08x (memory %016llx), %u queued",
                          (unsigned long long)job.ticket, job.queue_event, (unsigned long long)job.stream.wait_packet,
                          w[0], w[1], w[2], w[3], w[4], w[5], w[6], (unsigned long long)value, g_drv.count);
            }
            if (r && job.stream.executed != executed) renderer_gpu_idle(r);
            job.retry_ns = result == GPU_EXEC_BLOCKED ? pal_time_ns() + 1000000 : 0;
            pal_mutex_lock(g_drv.q_lock);
            g_drv.ring[selected % SUBMIT_RING] = job;
            pal_cond_broadcast(g_drv.q_changed);
            pal_mutex_unlock(g_drv.q_lock);
            continue;
        }
        if (job.blocked_ns && getenv("PS5C_TRACE_CP_WAIT")) {
            static uint64_t reported;
            if (++reported <= 64)
                LOG_I(GPU, "CP resumed ticket %llu queue %d after %.3f ms",
                      (unsigned long long)job.ticket, job.queue_event, (pal_time_ns() - job.blocked_ns) / 1e6);
        }
        worker_phase("renderer_suspend_point");
        if (r && job.suspend) renderer_suspend_point(r);
        pal_mutex_lock(g_drv.q_lock);
        bool idle = g_drv.count == 1;
        pal_mutex_unlock(g_drv.q_lock);
        worker_phase("renderer_gpu_idle");
        if (r && idle) renderer_gpu_idle(r); /* releases the renderer holds: the game may wait for them */
        pal_mutex_lock(g_drv.q_lock);
        job.done = true;
        g_drv.ring[selected % SUBMIT_RING] = job;
        /* Later compute jobs may complete first. Retire only the completed
         * prefix; SuspendPoint must not mistake them for older graphics work. */
        while (g_drv.completed < g_drv.submitted && g_drv.ring[(g_drv.completed + 1) % SUBMIT_RING].done) {
            memset(&g_drv.ring[(g_drv.completed + 1) % SUBMIT_RING], 0, sizeof(gpu_job));
            g_drv.completed++;
            g_drv.count--;
        }
        pal_cond_broadcast(g_drv.q_changed);
        pal_mutex_unlock(g_drv.q_lock);
    }
    return 0;
}

static gpu *get_gpu(hle_call *c);

/* Queues a command buffer for the GPU thread (blocks while the ring is full). */
static uint64_t enqueue(hle_call *c, guest_addr addr, uint32_t dwords, int32_t queue_event, bool suspend) {
    get_gpu(c);
    static int check = -1; /* PS5C_LOG_OVERRUN: is the buffer already unparsable when submitted? */
    if (check < 0) check = getenv("PS5C_LOG_OVERRUN") != NULL;
    const uint32_t *cmd = check && dwords ? (const uint32_t *)gmem_host(c->mem, addr, (uint64_t)dwords * 4) : NULL;
    for (uint32_t pos = 0; cmd && pos < dwords;) {
        uint32_t hdr = cmd[pos], len = hdr >> 30 == 2 || hdr == 0xFFFF1000u ? 1 : ((hdr >> 16) & 0x3FFF) + 2;
        if (pos + len > dwords) {
            LOG_W(GPU, "submit: packet 0x%08x at dword %u overruns the buffer 0x%llx (%u dwords) already", hdr, pos,
                  (unsigned long long)addr, dwords);
            break;
        }
        pos += len;
    }
    pal_mutex_lock(g_drv.q_lock);
    while (g_drv.count == SUBMIT_RING) pal_cond_wait(g_drv.q_changed, g_drv.q_lock);
    uint32_t *snap = cmd && dwords <= 0x10000 ? (uint32_t *)malloc((size_t)dwords * 4) : NULL;
    if (snap) memcpy(snap, cmd, (size_t)dwords * 4);
    uint32_t queue = 0;
    if (g_drv.cp_waits) {
        for (queue = 0; queue < CP_QUEUES; queue++) {
            queue_state *q = &g_drv.queues[queue];
            if (!q->used) { q->used = true; q->event = queue_event; q->num_instances = 1; break; }
            if (q->event == queue_event) break;
        }
        if (queue == CP_QUEUES) {
            LOG_E(GPU, "too many hardware queues");
            pal_exit_process(3);
        }
    }
    uint64_t ticket = ++g_drv.submitted;
    g_drv.ring[ticket % SUBMIT_RING] = (gpu_job){.addr=addr, .dwords=dwords, .queue_event=queue_event,
        .snap=snap, .t=pal_time_ns(), .suspend=suspend, .ticket=ticket, .queue=queue};
    g_drv.count++;
    pal_cond_broadcast(g_drv.q_changed);
    pal_mutex_unlock(g_drv.q_lock);
    return ticket;
}

static void submit(hle_call *c, guest_addr addr, uint32_t dwords, int32_t queue_event) {
    enqueue(c, addr, dwords, queue_event, false);
}

static gpu *get_gpu(hle_call *c) {
    pal_mutex_lock(g_drv.lock);
    if (!g_drv.gpu_ready) {
        gpu_callbacks cb = {NULL, on_flip, on_interrupt, on_draw, on_dispatch, on_mem_written, on_sync_mem, on_release, on_cp_write, on_lod_stats, on_sample_counter, on_wait_display};
        if (gpu_init(&g_drv.gpu, c->mem, &cb) != PS5C_OK) {
            LOG_E(GPU, "out of memory for the GPU state");
            pal_exit_process(3);
        }
        /* GTA's graphics queue consumes command buffers that compute/CPU work
         * has not finished preparing. Ignoring REWIND/WAIT here runs past that
         * dependency and stalls the first story scene. Keep other titles on
         * their existing path until their WAIT modes have been validated. */
        char title[32] = {0};
        bool gta_queues = settings_param_string("titleId", title, sizeof(title)) && !strcmp(title, "PPSA04263");
        /* Unreal measures the GPU clock at boot: the CPU and a queue take turns writing 1, 2, 3...
         * to one word, the queue's turns behind WAIT_REG_MEM_64. Skipped waits run ahead of the CPU
         * and the boot spins for ever (Silent Hill Townfall, SpongeBob Titans of the Tide). */
        const loaded_module *main_module = c->rt && c->rt->ms ? module_set_main(c->rt->ms) : NULL;
        size_t mn = main_module && main_module->name ? strlen(main_module->name) : 0;
        bool unreal = mn > 13 && !strcmp(main_module->name + mn - 13, "-PS5-Shipping");
        const char *cp_waits = getenv("PS5C_CP_WAITS");
        g_drv.cp_waits = cp_waits && *cp_waits ? atoi(cp_waits) != 0 : gta_queues || unreal;
        LOG_I(GPU, "CP queues: %s (%s)", g_drv.cp_waits ? "resumable, isolated state" : "legacy FIFO",
              cp_waits && *cp_waits ? "PS5C_CP_WAITS override" : gta_queues ? "GTA V compatibility"
              : unreal ? "Unreal clock calibration" : "default");
        g_drv.queues[0] = (queue_state){.used=true, .event=0, .regs=g_drv.gpu.regs, .num_instances=1};
        if (pal_mutex_create(&g_drv.q_lock) != PS5C_OK || pal_cond_create(&g_drv.q_changed) != PS5C_OK ||
            pal_thread_create(&g_drv.worker, gpu_worker, NULL, "gpu", 0) != PS5C_OK) {
            LOG_E(GPU, "cannot start the GPU thread");
            pal_exit_process(3);
        }
        g_drv.gpu_ready = true;
    }
    pal_mutex_unlock(g_drv.lock);
    return &g_drv.gpu;
}

/* ---- Submission ---- */

/* struct { uint32_t *addr; uint32_t dwords; uint8_t flags; uint8_t pad[3]; } */
static bool read_packet(hle_call *c, guest_addr p, guest_addr *addr, uint32_t *dwords) {
    return gmem_read_u64(c->mem, p, addr) && gmem_read(c->mem, p + 8, dwords, 4);
}

static void submit_dcb(hle_call *c) {
    guest_addr addr;
    uint32_t dwords;
    if (!read_packet(c, hle_arg_ptr(c, 0), &addr, &dwords)) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    submit(c, addr, dwords, 0);
    hle_ret_s32(c, 0);
}

/* Unnamed entry used by sceAgcSuspendPoint / AndCheckStatus:
 * (const packet *pending, uint32_t *status, uint32_t flags).
 * libSceAgc can pass pending commands here (including its default state at
 * initialization). Execute that packet once. The CPU must not reuse its
 * transient resources until all work ahead of this ordered suspension has
 * been executed (renderer_suspend_point: the host GPU too when its results
 * are owed to guest memory). */
static void submit_suspend_point(hle_call *c) {
    guest_addr addr;
    uint32_t dwords;
    if (!read_packet(c, hle_arg_ptr(c, 0), &addr, &dwords)) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    uint32_t *status = hle_arg_ptr(c, 1) ? (uint32_t *)hle_mem(c, hle_arg_ptr(c, 1), 4) : NULL;
    if (hle_arg_ptr(c, 1) && !status) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    /* A/B diagnostic for the former asynchronous return. */
    static int off = -1;
    if (off < 0) off = getenv("PS5C_WAIT_SUSPEND_POINT") && !atoi(getenv("PS5C_WAIT_SUSPEND_POINT"));
    if (off) {
        if (dwords) submit(c, addr, dwords, 0);
        hle_ret_s32(c, 0);
        return;
    }
    uint64_t ticket = enqueue(c, addr, dwords, 0, true);
    pal_mutex_lock(g_drv.q_lock);
    uint64_t since = pal_time_ns();
    bool told = false;
    while (!ticket_done(ticket)) {
        pal_cond_wait_ns(g_drv.q_changed, g_drv.q_lock, 1000000000);
        if (told || pal_time_ns() - since < 10000000000ull) continue;
        told = true; /* a suspension still pending after 10 s: what it waits behind */
        LOG_W(GPU, "suspend point ticket %llu pending 10 s (completed %llu, submitted %llu)", (unsigned long long)ticket,
              (unsigned long long)g_drv.completed, (unsigned long long)g_drv.submitted);
        for (uint64_t t = g_drv.completed + 1, n = 0; t <= g_drv.submitted && n < 40; t++) {
            const gpu_job *j = &g_drv.ring[t % SUBMIT_RING];
            if (j->done) continue;
            n++;
            LOG_W(GPU, "  ticket %llu queue %u (event %d) suspend %u (%u dwords) blocked %u retry %lld ms",
                  (unsigned long long)j->ticket, j->queue, j->queue_event, j->suspend, j->dwords, j->blocked_ns != 0,
                  j->retry_ns ? (long long)(j->retry_ns - (int64_t)pal_time_ns()) / 1000000 : 0);
            /* PS5C_LOG_STUCK_PACKETS: the packets of each job still pending */
            const uint32_t *cmd = getenv("PS5C_LOG_STUCK_PACKETS") && j->dwords
                ? (const uint32_t *)gmem_host(g_drv.gpu.mem, j->addr, (uint64_t)j->dwords * 4) : NULL;
            for (uint32_t pos = 0; cmd && pos < j->dwords;) {
                uint32_t hdr = cmd[pos], len = hdr >> 30 == 2 || hdr == 0xFFFF1000u ? 1 : ((hdr >> 16) & 0x3FFF) + 2;
                LOG_W(GPU, "    +%u op %02x len %u: %08x %08x %08x %08x %08x %08x", pos, (hdr >> 8) & 0xff, len,
                      pos + 1 < j->dwords ? cmd[pos + 1] : 0, pos + 2 < j->dwords ? cmd[pos + 2] : 0,
                      pos + 3 < j->dwords ? cmd[pos + 3] : 0, pos + 4 < j->dwords ? cmd[pos + 4] : 0,
                      pos + 5 < j->dwords ? cmd[pos + 5] : 0, pos + 6 < j->dwords ? cmd[pos + 6] : 0);
                pos += len;
            }
        }
        pal_mutex_unlock(g_drv.q_lock);
        log_worker_state(); /* without q_lock: the worker may be waiting for it */
        pal_mutex_lock(g_drv.q_lock);
    }
    pal_mutex_unlock(g_drv.q_lock);
    if (status) *status = 0;
    hle_ret_s32(c, 0);
}

/* int sceAgcDriverSubmitAcb(uint32_t queue, const packet *p) */
static void submit_acb(hle_call *c) {
    guest_addr addr;
    uint32_t dwords;
    if (!read_packet(c, hle_arg_ptr(c, 1), &addr, &dwords)) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    /* compute queues are numbered from COMPUTE_QUEUE_EVENT_BASE; the queue number is its event id */
    uint32_t queue = hle_arg_u32(c, 0);
    submit(c, addr, dwords, queue >= COMPUTE_QUEUE_EVENT_BASE ? (int32_t)queue : COMPUTE_QUEUE_EVENT_BASE + (int32_t)queue);
    hle_ret_s32(c, 0);
}

/* int sceAgcDriverSubmitMultiDcbs(uint32_t *const *addrs, const uint32_t *dwords, uint32_t count) */
static void submit_multi_dcbs(hle_call *c) {
    uint32_t count = hle_arg_u32(c, 2);
    for (uint32_t i = 0; i < count; i++) {
        uint64_t addr = 0;
        uint32_t dwords = 0;
        if (!gmem_read_u64(c->mem, hle_arg_ptr(c, 0) + (uint64_t)i * 8, &addr) ||
            !gmem_read(c->mem, hle_arg_ptr(c, 1) + (uint64_t)i * 4, &dwords, 4)) {
            hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
            return;
        }
        submit(c, addr, dwords, 0);
    }
    hle_ret_s32(c, 0);
}

/* int sceAgcDriverSubmitMultiAcbs(uint32_t queue, uint32_t *const *addrs,
 *                               const uint32_t *dwords, uint32_t count)
 * Same arrays as MultiDcbs, with an explicit compute queue. Snapshot and
 * validate the entire list before publishing any work: a malformed later
 * entry must not leave a partially submitted batch behind. */
static void submit_multi_acbs(hle_call *c) {
    uint32_t count = hle_arg_u32(c, 3);
    if (!count) {
        hle_ret_s32(c, 0);
        return;
    }
    /* Bounded host metadata; this is our supported batch size, not a
     * hardware queue limit. Large command buffers themselves stay in guest
     * memory, just as with SubmitAcb. */
    if (count > 4096) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    typedef struct acb_packet {
        guest_addr addr;
        uint32_t dwords;
    } acb_packet;
    acb_packet *packets = calloc(count, sizeof(*packets));
    if (!packets) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_ENOMEM);
        return;
    }
    guest_addr addrs = hle_arg_ptr(c, 1), sizes = hle_arg_ptr(c, 2);
    if (!gmem_host(c->mem, addrs, (uint64_t)count * 8) ||
        !gmem_host(c->mem, sizes, (uint64_t)count * 4))
        goto invalid;
    for (uint32_t i = 0; i < count; i++) {
        acb_packet *p = &packets[i];
        if (!gmem_read_u64(c->mem, addrs + (uint64_t)i * 8, &p->addr) ||
            !gmem_read(c->mem, sizes + (uint64_t)i * 4, &p->dwords, 4) ||
            (p->dwords &&
             (!p->addr || (p->addr & 3) || !gmem_host(c->mem, p->addr, (uint64_t)p->dwords * 4))))
            goto invalid;
    }
    uint32_t queue = hle_arg_u32(c, 0);
    int32_t event = queue >= COMPUTE_QUEUE_EVENT_BASE ? (int32_t)queue
                                                      : COMPUTE_QUEUE_EVENT_BASE + (int32_t)queue;
    for (uint32_t i = 0; i < count; i++)
        submit(c, packets[i].addr, packets[i].dwords, event);
    free(packets);
    hle_ret_s32(c, 0);
    return;
invalid:
    free(packets);
    hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
}

/* ---- Events ---- */

/* int sceAgcDriverAddEqEvent(SceKernelEqueue eq, int id, void *udata) */
static void add_eq_event(hle_call *c) {
    uint64_t eq = hle_arg(c, 0);
    int32_t id = hle_arg_s32(c, 1);
    if (!equeue_valid(eq)) {
        hle_ret_s32(c, (int32_t)0x80020009); /* EBADF */
        return;
    }
    pal_mutex_lock(g_drv.lock);
    bool found = false;
    for (uint32_t i = 0; i < g_drv.event_count; i++) found |= g_drv.events[i].eq == eq && g_drv.events[i].id == id;
    /* Keep every accepted registration: games can register the graphics
     * events plus all compute queues before their first submission. The
     * former 32-slot table silently lost later interrupts despite success. */
    if(!found && g_drv.event_count==g_drv.event_capacity) {
        uint64_t capacity=g_drv.event_capacity?(uint64_t)g_drv.event_capacity*2:32;
        eq_registration *events=capacity<=UINT32_MAX && capacity<=SIZE_MAX/sizeof(*events)
            ?realloc(g_drv.events,(size_t)capacity*sizeof(*events)):NULL;
        if(!events){pal_mutex_unlock(g_drv.lock);hle_ret_s32(c,SCE_KERNEL_ERROR_ENOMEM);return;}
        g_drv.events=events;g_drv.event_capacity=(uint32_t)capacity;
    }
    /* Reserve host tracking first, so allocation failure cannot leave an
     * apparently registered guest event that the GPU never notifies. */
    if(!equeue_add(eq,(uint64_t)id,EVFILT_GRAPHICS_,hle_arg(c,2))) {
        pal_mutex_unlock(g_drv.lock);hle_ret_s32(c,(int32_t)0x80020009);return;
    }
    if (!found) {
        g_drv.events[g_drv.event_count].eq = eq;
        g_drv.events[g_drv.event_count].id = id;
        g_drv.event_count++;
    }
    pal_mutex_unlock(g_drv.lock);
    hle_ret_s32(c, 0);
}

static void delete_eq_event(hle_call *c) {
    uint64_t eq = hle_arg(c, 0);
    int32_t id = hle_arg_s32(c, 1);
    pal_mutex_lock(g_drv.lock);
    for (uint32_t i = 0; i < g_drv.event_count; i++) {
        if (g_drv.events[i].eq == eq && g_drv.events[i].id == id) {
            g_drv.events[i] = g_drv.events[--g_drv.event_count];
            break;
        }
    }
    pal_mutex_unlock(g_drv.lock);
    equeue_remove(eq, (uint64_t)id, EVFILT_GRAPHICS_);
    hle_ret_s32(c, 0);
}

static const guest_kevent *event_arg(hle_call *c) {
    return (const guest_kevent *)hle_mem(c, hle_arg_ptr(c, 0), sizeof(guest_kevent));
}

static void get_eq_context_id(hle_call *c) {
    const guest_kevent *ev = event_arg(c);
    hle_ret_u32(c, !ev ? 0 : ev->filter == EVFILT_GRAPHICS_ ? (uint32_t)((uint64_t)ev->data >> 16) : (uint32_t)ev->ident);
}

static void get_eq_event_type(hle_call *c) {
    const guest_kevent *ev = event_arg(c);
    hle_ret_s32(c, ev ? (int32_t)(uint32_t)ev->data : 0);
}

/* ---- Driver packets written into libSceAgc's command buffers ----
 * Calls look like f(uint32_t **cursor, uint32_t dwords, uint32_t, ...): we write
 * `dwords` dwords at *cursor and advance it. */

static uint32_t *packet_cursor(hle_call *c, uint32_t dwords, guest_addr *cursor_addr) {
    *cursor_addr = hle_arg_ptr(c, 0);
    uint64_t cursor = 0;
    if (!gmem_read_u64(c->mem, *cursor_addr, &cursor)) return NULL;
    uint32_t *dst = (uint32_t *)hle_mem(c, cursor, (uint64_t)dwords * 4);
    if (dst) gmem_write_u64(c->mem, *cursor_addr, cursor + (uint64_t)dwords * 4);
    return dst;
}

static void set_flip_packet_size(hle_call *c) { hle_ret_u32(c, GPU_FLIP_PACKET_DWORDS); }

/* int sceAgcDriverSetFlip(uint32_t **cursor, uint32_t dwords, uint32_t, int handle, int index, int mode,
 *                         int64_t arg) */
static void set_flip(hle_call *c) {
    uint32_t dwords = hle_arg_u32(c, 1);
    guest_addr cursor;
    uint32_t *dst = dwords >= GPU_FLIP_PACKET_DWORDS ? packet_cursor(c, dwords, &cursor) : NULL;
    if (!dst) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    uint64_t arg = hle_arg(c, 6);
    uint32_t payload[6] = {GPU_DRIVER_FLIP, hle_arg_u32(c, 3), hle_arg_u32(c, 4), hle_arg_u32(c, 5), (uint32_t)arg,
                           (uint32_t)(arg >> 32)};
    gpu_write_driver_packet(dst, dwords, payload, 6);
    get_gpu(c);
    pal_mutex_lock(g_drv.q_lock);
    g_drv.flips_recorded++;
    pal_mutex_unlock(g_drv.q_lock);
    hle_ret_s32(c, 0);
}

static void wait_rendering_packet_size(hle_call *c) { hle_ret_u32(c, GPU_WAIT_DISPLAY_PACKET_DWORDS); }

/* int sceAgcDriverWaitUntilSafeForRendering(uint32_t **cursor, uint32_t dwords, uint32_t, int handle, int index) */
static void wait_until_safe_for_rendering(hle_call *c) {
    uint32_t dwords = hle_arg_u32(c, 1);
    guest_addr cursor;
    uint32_t *dst = dwords >= GPU_WAIT_DISPLAY_PACKET_DWORDS ? packet_cursor(c, dwords, &cursor) : NULL;
    if (!dst) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    uint32_t payload[3] = {GPU_DRIVER_WAIT_DISPLAY, hle_arg_u32(c, 3), hle_arg_u32(c, 4)};
    gpu_write_driver_packet(dst, dwords, payload, 3);
    hle_ret_s32(c, 0);
}

/* ---- Private services to libSceAgc ---- */

/* int sceAgcDriverGetReservedDmemForAgc(void **addr, uint32_t *size) */
static void get_reserved_dmem_for_agc(hle_call *c) {
    uint64_t *addr_out = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 0), 8);
    uint32_t *size_out = (uint32_t *)hle_mem(c, hle_arg_ptr(c, 1), 4);
    if (!addr_out || !size_out) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    if (!gmem_is_mapped(c->mem, AGC_RESERVED_ADDR, AGC_RESERVED_SIZE)) {
        guest_addr at = 0;
        if (gmem_map(c->mem, AGC_RESERVED_ADDR, AGC_RESERVED_SIZE, GPROT_RW, GMAP_FIXED, "agc reserved", &at) !=
            PS5C_OK) {
            LOG_E(GPU, "cannot map the Agc reserved area at 0x%llx", (unsigned long long)AGC_RESERVED_ADDR);
            hle_ret_s32(c, SCE_KERNEL_ERROR_ENOMEM);
            return;
        }
    }
    *addr_out = AGC_RESERVED_ADDR;
    *size_out = AGC_RESERVED_SIZE;
    hle_ret_s32(c, 0);
}

/* Configuration, debug and tooling hooks with nothing to do on our GPU.
 * Debug markers take no space (their packet size is 0). */
static void ok_(hle_call *c) { hle_ret_s32(c, 0); }

/* bool sceAgcDriverIsCaptureInProgress(void): no GPU capture tool is attached */
static void capture_in_progress(hle_call *c) { hle_ret_u32(c, 0); }

/* int sceAgcDriverRegisterWorkloadStream(uint32_t id, const void *stream): ids 1..31 with a
 * 32-byte record (KytyPS5). Workload packets are not timed here, so nothing keeps it. */
static void register_workload_stream(hle_call *c) {
    uint32_t id = hle_arg_u32(c, 0);
    if (id < 1 || id > 31) hle_ret_s32(c, (int32_t)0x8a6c0033); /* SCE_AGC_DRIVER_ERROR_INVALID_VALUE */
    else if (!hle_arg_ptr(c, 1)) hle_ret_s32(c, (int32_t)0x8a6c0035); /* ..._INVALID_ARGUMENT */
    else hle_ret_s32(c, 0);
}

/* int sceAgcDriverGetResourceRegistrationMaxNameLength(uint32_t *length): names of registered
 * owners and resources (debug tools) are at most this long */
static void max_name_length(hle_call *c) {
    uint32_t *out = (uint32_t *)hle_mem(c, hle_arg_ptr(c, 0), 4);
    if (out) *out = 127;
    hle_ret_s32(c, 0);
}

/* int sceAgcDriverGetDefaultOwner(uint32_t *owner) */
static void default_owner(hle_call *c) {
    uint32_t *out = (uint32_t *)hle_mem(c, hle_arg_ptr(c, 0), 4);
    if (out) *out = 1;
    hle_ret_s32(c, 0);
}

/* Resource registration (GPU debugging tools): nothing is registered, the calls succeed */
static void resource_registration_requirements(hle_call *c) {
    uint64_t size = 0x4000; /* int sceAgcDriverQueryResourceRegistrationUserMemoryRequirements(u64 *size) */
    hle_ret_s32(c, gmem_write(c->mem, hle_arg_ptr(c, 0), &size, 8) ? 0 : (int32_t)0x80000016);
}
/* Owners and resources get handles (the callers keep them), but nothing is tracked:
 * queries about a resource answer "registration unavailable", as without a debugger. */
#define AGC_RESOURCE_REGISTRATION_UNAVAILABLE ((int32_t)0x8A6C9018u)
static void register_handle(hle_call *c) {
    static volatile uint32_t next = 1;
    uint32_t id = __atomic_add_fetch(&next, 1, __ATOMIC_RELAXED);
    if (hle_arg_ptr(c, 0)) gmem_write(c->mem, hle_arg_ptr(c, 0), &id, 4);
    hle_ret_s32(c, 0);
}
static void resource_unavailable(hle_call *c) { hle_ret_s32(c, AGC_RESOURCE_REGISTRATION_UNAVAILABLE); }

static void register_default_owner(hle_call *c) {
    uint32_t owner = 1; /* int sceAgcDriverRegisterDefaultOwner(u32 *owner) */
    gmem_write(c->mem, hle_arg_ptr(c, 0), &owner, 4);
    hle_ret_s32(c, 0);
}
static void resource_registration_ok(hle_call *c) { hle_ret_s32(c, 0); }

/* Two unnamed calls libSceAgc makes at initialization: one takes a flag (whether the video
 * mode is below 6) and returns nothing used; the other answers a yes/no question, and on no
 * libSceAgc writes one more register in its default state. No for both, as the games run. */
static void agc_unnamed_flag(hle_call *c) { hle_ret_u64(c, 0); }
static void agc_unnamed_query(hle_call *c) { hle_ret_u32(c, 0); }

/* ---- PS4: libSceGnmDriver ----
 * A PS4 game's command buffers go to the same command processor. Its flips come with the
 * submission: a flip packet of ours, in a slot of a small guest ring, queued after them. */
#define GNM_FLIP_SLOTS (2 * SUBMIT_RING) /* more than can wait in the queue */
static guest_addr g_gnm_flips;
static bool g_gnm_seen;
static uint32_t g_gnm_flip_next;

/* Each command buffer of the arrays, by its size in bytes (constant buffers are not run) */
static bool gnm_submit(hle_call *c, uint32_t count, guest_addr addrs, guest_addr sizes) {
    if (count > 4096) return false;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t addr = 0;
        uint32_t bytes = 0;
        if (!gmem_read_u64(c->mem, addrs + (uint64_t)i * 8, &addr) || !gmem_read(c->mem, sizes + (uint64_t)i * 4, &bytes, 4))
            return false;
        if (addr && bytes >= 4 && !g_gnm_seen) {
            g_gnm_seen = true;
            get_gpu(c)->gcn = true;
            LOG_I(GPU, "PS4 command buffers: GCN shaders, draws counted but not rendered yet");
        }
        if (addr && bytes >= 4) submit(c, addr, bytes / 4, 0);
    }
    return true;
}

/* int sceGnmSubmitCommandBuffers(u32 count, void **dcb, u32 *dcb_bytes, void **ccb, u32 *ccb_bytes) */
static void gnm_submit_command_buffers(hle_call *c) {
    hle_ret_s32(c, gnm_submit(c, hle_arg_u32(c, 0), hle_arg_ptr(c, 1), hle_arg_ptr(c, 2)) ? 0 : SCE_KERNEL_ERROR_EINVAL);
}

/* int sceGnmSubmitAndFlipCommandBuffers(u32 count, void **dcb, u32 *dcb_bytes, void **ccb, u32 *ccb_bytes,
 *                                       int handle, int index, int mode, int64_t arg) */
static void gnm_submit_and_flip(hle_call *c) {
    if (!gnm_submit(c, hle_arg_u32(c, 0), hle_arg_ptr(c, 1), hle_arg_ptr(c, 2))) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    pal_mutex_lock(g_drv.lock);
    if (!g_gnm_flips && gmem_map(c->mem, 0, (uint64_t)GNM_FLIP_SLOTS * GPU_FLIP_PACKET_DWORDS * 4, GPROT_RW, 0,
                                 "Gnm flip packets", &g_gnm_flips) != PS5C_OK)
        g_gnm_flips = 0;
    guest_addr slot = g_gnm_flips ? g_gnm_flips + (uint64_t)(g_gnm_flip_next++ % GNM_FLIP_SLOTS) * GPU_FLIP_PACKET_DWORDS * 4 : 0;
    pal_mutex_unlock(g_drv.lock);
    uint32_t *dst = slot ? (uint32_t *)hle_mem(c, slot, GPU_FLIP_PACKET_DWORDS * 4) : NULL;
    if (!dst) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_ENOMEM);
        return;
    }
    uint64_t arg = hle_arg(c, 8);
    uint32_t payload[6] = {GPU_DRIVER_FLIP, hle_arg_u32(c, 5), hle_arg_u32(c, 6), hle_arg_u32(c, 7), (uint32_t)arg,
                           (uint32_t)(arg >> 32)};
    gpu_write_driver_packet(dst, GPU_FLIP_PACKET_DWORDS, payload, 6);
    submit(c, slot, GPU_FLIP_PACKET_DWORDS, 0);
    pal_mutex_lock(g_drv.q_lock);
    g_drv.flips_recorded++;
    pal_mutex_unlock(g_drv.q_lock);
    hle_ret_s32(c, 0);
}

/* sceGnmInsertWaitFlipDone(cmd, 7, handle, index) fills space the game reserved: the flip runs
 * in queue order here, before the next commands, so its space is a NOP. */
/* The reserved space a Gnm call fills: PM4 packets from the start, fillers after them */
typedef struct gnm_space {
    uint32_t *p;
    uint32_t n, cap;
} gnm_space;

static bool gnm_space_open(hle_call *c, gnm_space *sp) {
    sp->cap = hle_arg_u32(c, 1);
    sp->n = 0;
    sp->p = sp->cap >= 2 && sp->cap <= 0x4000 ? (uint32_t *)hle_mem(c, hle_arg_ptr(c, 0), (uint64_t)sp->cap * 4) : NULL;
    return sp->p != NULL;
}

/* One SET_SH_REG (0x76) / SET_CONTEXT_REG (0x69) / SET_UCONFIG_REG (0x79) packet of consecutive registers */
static void gnm_set_regs(gnm_space *sp, uint32_t reg, const uint32_t *v, uint32_t count) {
    uint32_t op = reg >= 0xC000u ? 0x79u : reg >= 0xA000u ? 0x69u : 0x76u;
    uint32_t base = reg >= 0xC000u ? 0xC000u : reg >= 0xA000u ? 0xA000u : 0x2C00u;
    if (sp->n + 2 + count > sp->cap) return;
    sp->p[sp->n++] = 0xC0000000u | (count << 16) | (op << 8);
    sp->p[sp->n++] = reg - base;
    for (uint32_t i = 0; i < count; i++) sp->p[sp->n++] = v[i];
}

static void gnm_space_close(hle_call *c, gnm_space *sp) {
    uint32_t left = sp->cap - sp->n;
    if (left >= 2) {
        sp->p[sp->n] = 0xC0001000u | ((left - 2) << 16); /* type 3 NOP */
        memset(sp->p + sp->n + 1, 0, (size_t)(left - 1) * 4);
    } else if (left == 1) {
        sp->p[sp->n] = 0x80000000u; /* type 2 filler */
    }
    hle_ret_s32(c, 0);
}

/* The stage registers, as libSceGnm keeps them (sce::Gnm::VsStageRegisters, PsStageRegisters):
 * the shader modifier (from the fetch shader) replaces the low VGPR/SGPR fields of RSRC1. */
static uint32_t gnm_rsrc1(uint32_t rsrc1, uint32_t modifier) { return modifier ? (rsrc1 & 0xFCFFFC00u) | modifier : rsrc1; }

/* int sceGnmSetVsShader(u32 *cmd, u32 dwords, const VsStageRegisters *regs, u32 modifier):
 * {pgm lo, pgm hi, rsrc1, rsrc2, SPI_VS_OUT_CONFIG, SPI_SHADER_POS_FORMAT, PA_CL_VS_OUT_CNTL} */
static void gnm_set_vs_shader(hle_call *c) {
    gnm_space sp;
    const uint32_t *r = (const uint32_t *)hle_mem(c, hle_arg_ptr(c, 2), 7 * 4);
    if (!gnm_space_open(c, &sp) || !r) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    uint32_t pgm[4] = {r[0], r[1], gnm_rsrc1(r[2], hle_arg_u32(c, 3)), r[3]};
    gnm_set_regs(&sp, 0x2C48u, pgm, 4);     /* SPI_SHADER_PGM_LO_VS .. RSRC2_VS */
    gnm_set_regs(&sp, 0xA1B1u, r + 4, 1);   /* SPI_VS_OUT_CONFIG */
    gnm_set_regs(&sp, 0xA1C3u, r + 5, 1);   /* SPI_SHADER_POS_FORMAT */
    gnm_set_regs(&sp, 0xA207u, r + 6, 1);   /* PA_CL_VS_OUT_CNTL */
    gnm_space_close(c, &sp);
}

/* int sceGnmSetPsShader350(u32 *cmd, u32 dwords, const PsStageRegisters *regs):
 * {pgm lo, pgm hi, rsrc1, rsrc2, Z_FORMAT, COL_FORMAT, PS_INPUT_ENA, PS_INPUT_ADDR, PS_IN_CONTROL,
 *  BARYC_CNTL, DB_SHADER_CONTROL, CB_SHADER_MASK}; no registers: a pixel stage that does nothing */
static void gnm_set_ps_shader(hle_call *c) {
    gnm_space sp;
    if (!gnm_space_open(c, &sp)) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    const uint32_t *r = hle_arg_ptr(c, 2) ? (const uint32_t *)hle_mem(c, hle_arg_ptr(c, 2), 12 * 4) : NULL;
    static const uint32_t none[12] = {0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0};
    if (!r) r = none;
    gnm_set_regs(&sp, 0x2C08u, r, 4);       /* SPI_SHADER_PGM_LO_PS .. RSRC2_PS */
    gnm_set_regs(&sp, 0xA1C4u, r + 4, 2);   /* SPI_SHADER_Z_FORMAT, COL_FORMAT */
    gnm_set_regs(&sp, 0xA1B3u, r + 6, 2);   /* SPI_PS_INPUT_ENA, ADDR */
    gnm_set_regs(&sp, 0xA1B6u, r + 8, 1);   /* SPI_PS_IN_CONTROL */
    gnm_set_regs(&sp, 0xA1B8u, r + 9, 1);   /* SPI_BARYC_CNTL */
    gnm_set_regs(&sp, 0xA203u, r + 10, 1);  /* DB_SHADER_CONTROL */
    gnm_set_regs(&sp, 0xA08Fu, r + 11, 1);  /* CB_SHADER_MASK */
    gnm_space_close(c, &sp);
}

/* u32 sceGnmDrawInitDefaultHardwareState350(u32 *cmd, u64 dwords): the context state Gnm games
 * build on, written at cmd; returns the dwords written. Color blocks in normal mode with a copy
 * ROP, the viewport transform on, D3D clip space, scissors over the whole surface. */
static void gnm_init_default_state(hle_call *c) {
    uint64_t room = hle_arg(c, 1);
    gnm_space sp = {NULL, 0, room > 256 ? 256u : (uint32_t)room};
    sp.p = sp.cap >= 64 ? (uint32_t *)hle_mem(c, hle_arg_ptr(c, 0), (uint64_t)sp.cap * 4) : NULL;
    if (!sp.p) {
        hle_ret_u32(c, 0);
        return;
    }
    static const uint32_t full_tl = 0x80000000u, full_br = 0x40004000u; /* window offset off; 16384 x 16384 */
    const uint32_t color_control = (1u << 4) | (0xCCu << 16);           /* CB_NORMAL, ROP3 copy */
    const uint32_t clip = 1u << 19;                                     /* DX_CLIP_SPACE_DEF */
    const uint32_t vte = 0x3Fu | (1u << 10);                            /* x/y/z scale and offset, W0 format */
    const uint32_t aa_mask[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
    const uint32_t zrange[2] = {0, 0x3F800000u};                        /* 0.0, 1.0 */
    const uint32_t scissor[2] = {full_tl, full_br}, screen[2] = {0, full_br}, rule = 0xFFFFu;
    gnm_set_regs(&sp, 0xA202u, &color_control, 1); /* CB_COLOR_CONTROL */
    gnm_set_regs(&sp, 0xA204u, &clip, 1);          /* PA_CL_CLIP_CNTL */
    gnm_set_regs(&sp, 0xA206u, &vte, 1);           /* PA_CL_VTE_CNTL */
    gnm_set_regs(&sp, 0xA00Cu, screen, 2);         /* PA_SC_SCREEN_SCISSOR_TL/BR */
    gnm_set_regs(&sp, 0xA081u, scissor, 2);        /* PA_SC_WINDOW_SCISSOR_TL/BR */
    gnm_set_regs(&sp, 0xA084u, &rule, 1);          /* PA_SC_CLIPRECT_RULE */
    gnm_set_regs(&sp, 0xA090u, scissor, 2);        /* PA_SC_GENERIC_SCISSOR_TL/BR */
    gnm_set_regs(&sp, 0xA094u, scissor, 2);        /* PA_SC_VPORT_SCISSOR_0_TL/BR */
    gnm_set_regs(&sp, 0xA0B4u, zrange, 2);         /* PA_SC_VPORT_ZMIN/ZMAX_0 */
    gnm_set_regs(&sp, 0xA30Eu, aa_mask, 2);        /* PA_SC_AA_MASK */
    hle_ret_u32(c, sp.n);
}

/* u32 sceGnmDrawInitToDefaultContextState400(u32 *cmd, u64 dwords): the same default context
 * state, the rest of the block filled with a NOP; returns the dwords used (the whole block).
 * LEGO City Undercover (CUSA06549) calls it for every command buffer. */
static void gnm_init_default_context400(hle_call *c) {
    uint64_t room = hle_arg(c, 1);
    uint32_t cap = room > 256 ? 256u : (uint32_t)room;
    uint32_t *cmd = cap >= 64 ? (uint32_t *)hle_mem(c, hle_arg_ptr(c, 0), (uint64_t)cap * 4) : NULL;
    gnm_init_default_state(c);
    uint32_t used = (uint32_t)c->a->ret[0];
    if (!cmd || !used || used > cap) return; /* the 0 from above */
    if (cap - used >= 2) {
        cmd[used] = 0xC0001000u | ((cap - used - 2) << 16); /* type 3 NOP over the rest */
        memset(cmd + used + 1, 0, (size_t)(cap - used - 1) * 4);
        used = cap;
    }
    hle_ret_u32(c, used);
}

/* int sceGnmDrawIndexAuto(u32 *cmd, u32 dwords, u32 count, u32 flags): DRAW_INDEX_AUTO, auto-index source */
static void gnm_draw_index_auto(hle_call *c) {
    gnm_space sp;
    if (!gnm_space_open(c, &sp) || sp.cap < 3) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    sp.p[sp.n++] = 0xC0012D00u;
    sp.p[sp.n++] = hle_arg_u32(c, 2);
    sp.p[sp.n++] = 2;
    gnm_space_close(c, &sp);
}

static void gnm_nop_fill(hle_call *c) {
    uint32_t dwords = hle_arg_u32(c, 1);
    uint32_t *dst = dwords >= 2 && dwords <= 0x4000 ? (uint32_t *)hle_mem(c, hle_arg_ptr(c, 0), (uint64_t)dwords * 4) : NULL;
    if (!dst) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    dst[0] = 0xC0001000u | ((dwords - 2) << 16); /* type 3 NOP */
    memset(dst + 1, 0, (size_t)(dwords - 1) * 4);
    hle_ret_s32(c, 0);
}

static const hle_export k_gnm_exports[] = {
    HLE_FUNC_ENTRY("libSceGnmDriver", "sceGnmSubmitCommandBuffers", gnm_submit_command_buffers),
    HLE_FUNC_ENTRY("libSceGnmDriver", "sceGnmSubmitAndFlipCommandBuffers", gnm_submit_and_flip),
    HLE_FUNC_ENTRY("libSceGnmDriver", "sceGnmSubmitDone", ok_),
    HLE_FUNC_ENTRY("libSceGnmDriver", "sceGnmInsertWaitFlipDone", gnm_nop_fill),
    HLE_FUNC_ENTRY("libSceGnmDriver", "sceGnmSetVsShader", gnm_set_vs_shader),
    HLE_FUNC_ENTRY("libSceGnmDriver", "sceGnmSetPsShader350", gnm_set_ps_shader),
    HLE_FUNC_ENTRY("libSceGnmDriver", "sceGnmUpdateVsShader", gnm_set_vs_shader), /* same space, a later frame */
    HLE_FUNC_ENTRY("libSceGnmDriver", "sceGnmUpdatePsShader350", gnm_set_ps_shader),
    HLE_FUNC_ENTRY("libSceGnmDriver", "sceGnmDrawIndexAuto", gnm_draw_index_auto),
    HLE_FUNC_ENTRY("libSceGnmDriver", "sceGnmDrawInitDefaultHardwareState350", gnm_init_default_state),
    HLE_FUNC_ENTRY("libSceGnmDriver", "sceGnmDrawInitToDefaultContextState400", gnm_init_default_context400),
};

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverQueryResourceRegistrationUserMemoryRequirements", resource_registration_requirements),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverInitResourceRegistration", resource_registration_ok),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverRegisterDefaultOwner", register_default_owner),
    HLE_FUNC_ENTRY(LIB, "#oFb2hMcoJa4", agc_unnamed_flag),
    HLE_FUNC_ENTRY(LIB, "#SFB48L57OKQ", agc_unnamed_query),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverSubmitDcb", submit_dcb),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverSubmitAcb", submit_acb),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverSubmitMultiAcbs", submit_multi_acbs),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverSubmitMultiDcbs", submit_multi_dcbs),
    /* the AGR variants (graphics through the async-graphics ring) take the same arguments */
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverAgrSubmitDcb", submit_dcb),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverAgrSubmitMultiDcbs", submit_multi_dcbs),
    HLE_FUNC_ENTRY(LIB, "#kAPt2J4U3v8", submit_suspend_point),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverAddEqEvent", add_eq_event),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverDeleteEqEvent", delete_eq_event),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverGetEqContextId", get_eq_context_id),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverGetEqEventType", get_eq_event_type),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverGetSetFlipPacketSizeInDwords", set_flip_packet_size),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverSetFlip", set_flip),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverGetWaitRenderingPacketSizeInDwords", wait_rendering_packet_size),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverWaitUntilSafeForRendering", wait_until_safe_for_rendering),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverGetReservedDmemForAgc", get_reserved_dmem_for_agc),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverPassInfoDownward", ok_),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverSetTFRing", ok_),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverSetHsOffchipParam", ok_),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverRegisterOwner", register_handle),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverGetResourceRegistrationMaxNameLength", max_name_length),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverGetDefaultOwner", default_owner),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverRegisterResource", register_handle),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverRegisterGdsResource", register_handle),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverUnregisterAllResourcesForOwner", ok_),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverUnregisterOwnerAndResources", ok_),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverSetResourceUserData", ok_),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverGetOwnerName", resource_unavailable),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverGetResourceName", resource_unavailable),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverGetResourceType", resource_unavailable),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverGetResourceUserData", resource_unavailable),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverGetResourceShaderGuid", resource_unavailable),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverGetResourceBaseAddressAndSizeInBytes", resource_unavailable),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverFindResourcesPublic", resource_unavailable),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverRequestCaptureStart", ok_),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverRequestCaptureStop", ok_),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverUnregisterResource", ok_),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverRegisterGpuResetCallbacks", ok_),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverNotifyDefaultStates", ok_),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverTmpInitIdhs", ok_),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverIsPaDebug", ok_),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverIsCaptureInProgress", capture_in_progress),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverRegisterWorkloadStream", register_workload_stream),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverGetTraceInitiator", ok_),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverUserDataGetPacketSize", ok_),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverUserDataWritePacket", ok_),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverUserDataWriteSetMarker", ok_),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverUserDataWritePushMarker", ok_),
    HLE_FUNC_ENTRY(LIB, "sceAgcDriverUserDataWritePopMarker", ok_),
};

void hle_register_agc_driver(void) {
    if (!g_drv.lock && pal_mutex_create(&g_drv.lock) != PS5C_OK) {
        LOG_E(GPU, "cannot create the AgcDriver lock");
        return;
    }
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
    hle_register_table(k_gnm_exports, PS5C_ARRAY_COUNT(k_gnm_exports));
}

void agc_driver_log_stats(void);
void gpu_debug_check_labels(gpu *g);
void agc_driver_log_stats(void) {
    if (!g_drv.gpu_ready) return;
    const gpu_stats *s = &g_drv.gpu.stats;
    LOG_I(GPU, "GPU: %llu submits (%u queued), %llu packets, %llu draws, %llu dispatches, %llu releases, %llu interrupts "
               "(%llu asked for), %llu flips (%llu recorded)",
          (unsigned long long)s->submits, g_drv.count, (unsigned long long)s->packets, (unsigned long long)s->draws,
          (unsigned long long)s->dispatches, (unsigned long long)s->releases, (unsigned long long)s->interrupts,
          (unsigned long long)g_drv.gpu.interrupts_issued, (unsigned long long)s->flips,
          (unsigned long long)g_drv.flips_recorded);
    if (getenv("PS5C_LOG_RELEASE")) gpu_debug_check_labels(&g_drv.gpu);
}
