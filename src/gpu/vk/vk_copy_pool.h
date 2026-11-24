/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_VK_COPY_POOL_H
#define PS5C_VK_COPY_POOL_H
/* Copies of guest memory into the arena made by helper threads (PS5C_COPY_THREADS, default 2;
 * 0 = the command processor copies itself, as before).
 *
 * The command processor records a draw's buffers with their arena offsets as usual; the bytes
 * themselves are copied by the helpers while it goes on with the next draws. Everything queued
 * is complete before the batch is submitted (copy_pool_drain in vk_flush) and before the command
 * processor reads the arena or writes guest memory itself (writebacks, CP writes).
 *
 * Source pointers stay valid: while copies are queued the pool holds a pin of the host mappings
 * (joined from the command processor's own pin), so an unmap waits for them. */

#include <stdatomic.h>

#define COPY_POOL_RING 8192u
#define COPY_POOL_CHUNK (256u << 10)
#define COPY_POOL_MAX_THREADS 8u

typedef struct copy_task {
    uint8_t *dst;
    const uint8_t *src;
    size_t n;
} copy_task;

static struct {
    int threads; /* -1 unread */
    size_t min_bytes;
    gmem *mem;
    pal_mutex *lock;
    pal_cond *work, *idle;
    pal_thread *thread[COPY_POOL_MAX_THREADS];
    copy_task ring[COPY_POOL_RING];
    uint32_t head, tail;     /* lock: queued tasks [tail, head) */
    uint32_t running;        /* lock: tasks taken but not finished */
    bool pinned, stop;       /* lock */
    atomic_uint pending;     /* queued + running, read without the lock */
    uint64_t tasks, bytes, inline_bytes, drains, drain_ns; /* per second (stats) */
} g_cpool = {.threads = -1};

static int32_t copy_pool_main(void *arg);

/* Lock held: the last task finished, the pin goes */
static void copy_pool_unpin_locked(void) {
    if (g_cpool.pinned && g_cpool.head == g_cpool.tail && !g_cpool.running) {
        gmem_unpin_mappings(g_cpool.mem);
        g_cpool.pinned = false;
        pal_cond_broadcast(g_cpool.idle);
    }
}

/* Lock held: runs one queued task with the lock released; false when none */
static bool copy_pool_run_one_locked(void) {
    if (g_cpool.head == g_cpool.tail) return false;
    copy_task t = g_cpool.ring[g_cpool.tail++ % COPY_POOL_RING];
    g_cpool.running++;
    pal_mutex_unlock(g_cpool.lock);
    memcpy(t.dst, t.src, t.n);
    pal_mutex_lock(g_cpool.lock);
    g_cpool.running--;
    atomic_fetch_sub_explicit(&g_cpool.pending, 1, memory_order_release);
    copy_pool_unpin_locked();
    return true;
}

static int32_t copy_pool_main(void *arg) {
    PS5C_UNUSED(arg);
    pal_mutex_lock(g_cpool.lock);
    while (!g_cpool.stop) {
        if (!copy_pool_run_one_locked()) pal_cond_wait(g_cpool.work, g_cpool.lock);
    }
    pal_mutex_unlock(g_cpool.lock);
    return 0;
}

static bool copy_pool_enabled(renderer *r) {
    if (g_cpool.threads < 0) {
        const char *e = getenv("PS5C_COPY_THREADS"), *m = getenv("PS5C_COPY_MIN");
        int n = e ? atoi(e) : 2;
        if (n > (int)COPY_POOL_MAX_THREADS) n = COPY_POOL_MAX_THREADS;
        g_cpool.min_bytes = m ? (size_t)strtoull(m, NULL, 10) : 16384;
        g_cpool.threads = 0;
        g_cpool.mem = r->mem;
        if (n > 0 && r->mem && pal_mutex_create(&g_cpool.lock) == PS5C_OK) {
            if (pal_cond_create(&g_cpool.work) == PS5C_OK && pal_cond_create(&g_cpool.idle) == PS5C_OK) {
                for (int i = 0; i < n; i++)
                    if (pal_thread_create(&g_cpool.thread[i], copy_pool_main, NULL, "gpu copies", 0) == PS5C_OK)
                        g_cpool.threads++;
            }
        }
        LOG_I(GPU, "buffer copies: %d helper threads (copies of %zu bytes or more)", g_cpool.threads, g_cpool.min_bytes);
    }
    return g_cpool.threads > 0;
}

/* Queues dst <- src (n bytes); false when the caller must copy itself. The caller holds a
 * mapping pin (guest_host) while it calls this. */
static bool copy_pool_push(renderer *r, uint8_t *dst, const uint8_t *src, size_t n) {
    if (n < g_cpool.min_bytes || !copy_pool_enabled(r)) return false;
    uint32_t chunks = (uint32_t)((n + COPY_POOL_CHUNK - 1) / COPY_POOL_CHUNK);
    pal_mutex_lock(g_cpool.lock);
    if (g_cpool.head - g_cpool.tail + chunks > COPY_POOL_RING) {
        pal_mutex_unlock(g_cpool.lock);
        g_cpool.inline_bytes += n;
        return false;
    }
    if (!g_cpool.pinned) {
        if (!gmem_pin_mappings_version(g_cpool.mem, true, NULL)) {
            pal_mutex_unlock(g_cpool.lock);
            g_cpool.inline_bytes += n;
            return false;
        }
        g_cpool.pinned = true;
    }
    for (size_t at = 0; at < n; at += COPY_POOL_CHUNK) {
        size_t len = n - at < COPY_POOL_CHUNK ? n - at : COPY_POOL_CHUNK;
        g_cpool.ring[g_cpool.head++ % COPY_POOL_RING] = (copy_task){dst + at, src + at, len};
    }
    atomic_fetch_add_explicit(&g_cpool.pending, chunks, memory_order_relaxed);
    if (chunks > 1) pal_cond_broadcast(g_cpool.work);
    else pal_cond_signal(g_cpool.work);
    pal_mutex_unlock(g_cpool.lock);
    g_cpool.tasks += chunks;
    g_cpool.bytes += n;
    return true;
}

/* Every queued copy is done; the caller helps with what is still queued. */
static void copy_pool_drain_now(void) {
    if (!atomic_load_explicit(&g_cpool.pending, memory_order_acquire)) return;
    uint64_t t0 = pal_time_ns();
    pal_mutex_lock(g_cpool.lock);
    while (copy_pool_run_one_locked()) {}
    while (g_cpool.running || g_cpool.head != g_cpool.tail) pal_cond_wait(g_cpool.idle, g_cpool.lock);
    pal_mutex_unlock(g_cpool.lock);
    atomic_thread_fence(memory_order_acquire);
    g_cpool.drains++;
    g_cpool.drain_ns += pal_time_ns() - t0;
}

static void copy_pool_stats(void) {
    if (g_cpool.threads <= 0) return;
    LOG_I(GPU, "buffer copies by helpers: %llu tasks, %.1f MiB; copied in place when full: %.1f MiB; %llu waits %.2f ms",
          (unsigned long long)g_cpool.tasks, g_cpool.bytes / 1048576.0, g_cpool.inline_bytes / 1048576.0,
          (unsigned long long)g_cpool.drains, g_cpool.drain_ns / 1e6);
    g_cpool.tasks = g_cpool.bytes = g_cpool.inline_bytes = g_cpool.drains = g_cpool.drain_ns = 0;
}

static void copy_pool_shutdown(void) {
    if (g_cpool.threads <= 0) return;
    copy_pool_drain_now();
    pal_mutex_lock(g_cpool.lock);
    g_cpool.stop = true;
    pal_cond_broadcast(g_cpool.work);
    pal_mutex_unlock(g_cpool.lock);
    for (int i = 0; i < g_cpool.threads; i++) pal_thread_join(g_cpool.thread[i], NULL);
    g_cpool.threads = 0;
}

#endif
