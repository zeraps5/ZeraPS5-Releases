/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pipelines built in the background.
 *
 * Every pipeline is known by its content (vk_pipeline.c's recipe: shader cache
 * keys, bindings, fixed-function state; no guest addresses), so one built for
 * a draw serves any later draw with the same content, whatever addresses the
 * shaders were loaded at. The recipes of the pipelines a game built are kept
 * in <user dir>/cache/pipelines-list.bin; at the next start, worker threads
 * build a budgeted subset while the game boots. A draw whose pipeline is not ready hands its
 * build to the workers (first in line) and waits for it up to a budget; a build
 * that takes longer (Intel's compiler spends 5-15 s on No Man's Sky's largest
 * shaders and never returns on one) goes on in the background and its draws are
 * skipped until it is done, instead of the GPU thread stopping with it.
 * Compute callers require completion: skipping their dispatch can lose a
 * one-shot counter/write on which the guest waits indefinitely.
 *
 * PS5C_PIPELINES_ASYNC=0 builds each pipeline when a draw needs it, as before
 * (comparisons); PS5C_PIPELINE_THREADS=<n> sets the workers (default 2);
 * PS5C_PIPELINE_WAIT_MS=<ms> the budget (default 2000, 0: no limit). The
 * pause statistics (time draws waited for pipelines, frame times) are logged
 * every 10 s in both modes. */
#include "gpu/vk/vk_pipeline_async.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/hashmap.h"
#include "base/log.h"
#include "gpu/vk/vk_shader_cache.h"
#include "gpu/vk/vk_cache_budget.h"
#include "pal/pal.h"
#include "ps5c/shader_cache_version.h"

#define LIST_MAGIC 0x3154534C505A5350ull /* "PSZPLST1" */
#define MAX_WORKERS 8
#define STATS_NS 10000000000ull

enum { J_QUEUED, J_RUNNING, J_DONE, J_FAILED, J_ABSENT /* a worker could not build it: the next draw tries */ };

typedef struct job {
    uint8_t *recipe;
    int state;
    bool listed; /* in pipelines-list.bin */
    bool late;   /* asked for by a draw: its SPIR-V travels with it, a failure is final */
    bool over;   /* past the wait budget: draws are skipped until it is built */
    bool prewarm; /* counted once in the initial queue, even when a draw promotes it */
    uint32_t *words[2];
    size_t n[2];
    vk_built_pipeline b;
} job;

typedef struct list_head {
    uint64_t magic, translator;
    uint64_t recipe_size;
} list_head;

static struct {
    bool on, stop;
    renderer *r;
    size_t rsize;
    pal_mutex *lock;
    pal_cond *cond;
    hmap64 index; /* content key -> jobs[] */
    hmap64 recorded; /* all admitted disk keys, including recipes not prewarmed */
    job **jobs;
    uint32_t count, cap, queued, next;
    uint32_t *late; /* jobs[] indices of draws' builds, taken before the list's */
    uint32_t late_head, late_tail, late_cap;
    pal_thread *workers[MAX_WORKERS];
    uint32_t nworkers, running, busy, busy_late;
    uint64_t wait_budget;
    pal_file *list;
    uint64_t list_end;
    uint64_t list_limit, recipe_budget, source_budget;
    uint32_t prewarm_limit, record_limit;
    uint32_t pre_done, pre_failed, adopted, built_here;
    uint64_t pre_start, pre_ns;
    /* pauses (stat_lock) */
    pal_mutex *stat_lock;
    uint64_t win_start, last_frame, frames, frame_ns, frame_max;
    uint64_t waits, wait_ns, wait_max, builds;
    uint64_t all_waits, all_wait_ns, all_wait_max;
} g;

static uint64_t hash_recipe(const void *data, size_t size) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 0x84222325CBF29CE4ull;
    for (size_t i = 0; i < size; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    return h ^ (h >> 33);
}

bool vk_pipeline_async_on(void) { return g.on; }

/* g.lock held */
static job *add_job(uint64_t key, const void *recipe, int state) {
    if (g.count == g.cap) {
        uint32_t cap = g.cap ? g.cap * 2 : 256;
        job **j = (job **)realloc(g.jobs, sizeof(job *) * cap);
        if (!j) return NULL;
        g.jobs = j;
        g.cap = cap;
    }
    job *j = (job *)calloc(1, sizeof(job));
    if (!j || !(j->recipe = (uint8_t *)malloc(g.rsize)) || !hmap64_put(&g.index, key, g.count)) {
        if (j) free(j->recipe);
        free(j);
        return NULL;
    }
    memcpy(j->recipe, recipe, g.rsize);
    j->state = state;
    j->listed = hmap64_get(&g.recorded, key, &(uint64_t){0});
    g.jobs[g.count++] = j;
    return j;
}

/* g.lock held: a recipe built for the first time goes to the list */
static bool list_append(const void *recipe) {
    uint64_t key = hash_recipe(recipe, g.rsize);
    if (hmap64_get(&g.recorded, key, &(uint64_t){0})) return true;
    if (!g.list || g.recorded.len >= g.record_limit || g.list_end > g.list_limit || g.rsize > g.list_limit - g.list_end)
        return false;
    uint64_t done = 0;
    if (pal_file_pwrite(g.list, recipe, g.rsize, g.list_end, &done) == PS5C_OK && done == g.rsize) {
        g.list_end += g.rsize;
        if (hmap64_put(&g.recorded, key, 0)) return true;
        /* The disk record is valid, but without its index do not repeatedly
         * append it in this session after a host allocation failure. */
        pal_file_close(g.list);
        g.list = NULL;
        return true;
    } else {
        pal_file_close(g.list);
        g.list = NULL;
        return false;
    }
}

static void load_list(const char *path) {
    uint64_t size = 0;
    list_head want = {LIST_MAGIC, PS5C_SHADER_TRANSLATOR_HASH, g.rsize};
    pal_file *file = NULL;
    bool writable = pal_file_open(path, PAL_FILE_READ | PAL_FILE_WRITE | PAL_FILE_CREATE, &file) == PS5C_OK;
    if (!writable && pal_file_open(path, PAL_FILE_READ, &file) != PS5C_OK) return;
    uint64_t done = 0;
    list_head head = {0};
    if (pal_file_size(file, &size) != PS5C_OK) goto close_file;
    if (size >= sizeof(head) && (pal_file_pread(file, &head, sizeof(head), 0, &done) != PS5C_OK || done != sizeof(head)))
        goto close_file;
    if (size < sizeof(head) || memcmp(&head, &want, sizeof(head))) {
        if (!writable || g.list_limit < sizeof(want) || pal_file_truncate(file, 0) != PS5C_OK ||
            pal_file_pwrite(file, &want, sizeof(want), 0, &done) != PS5C_OK || done != sizeof(want)) goto close_file;
        g.list = file;
        g.list_end = sizeof(want);
        return;
    }
    uint8_t *recipe = malloc(g.rsize);
    if (!recipe) goto close_file;
    uint64_t good = sizeof(head), recipes = 0, sources = 0;
    bool limited = false, read_error = false;
    while (size - good >= g.rsize) {
        if (good > g.list_limit || g.rsize > g.list_limit - good || g.recorded.len >= g.record_limit) {
            limited = true;
            break;
        }
        if (pal_file_pread(file, recipe, g.rsize, good, &done) != PS5C_OK || done != g.rsize) { read_error = true; break; }
        uint64_t key = hash_recipe(recipe, g.rsize);
        if (!hmap64_get(&g.recorded, key, &(uint64_t){0})) {
            if (!hmap64_put(&g.recorded, key, 0)) { limited = true; break; }
            uint64_t bytes = 0;
            if (g.count < g.prewarm_limit && g.rsize <= g.recipe_budget - recipes &&
                vk_pipeline_prewarm_size(recipe, &bytes) && bytes <= g.source_budget - sources) {
                job *j = add_job(key, recipe, J_QUEUED);
                if (j) { j->prewarm = true; recipes += g.rsize; sources += bytes; }
            }
        }
        good += g.rsize;
    }
    free(recipe);
    g.queued = g.count;
    /* A partial last record may be dropped. Hitting a budget or an I/O error
     * preserves the full file and disables appends, rather than losing recipes. */
    if (writable && !limited && !read_error && (good == size || pal_file_truncate(file, good) == PS5C_OK)) {
        g.list = file;
        g.list_end = good;
        return;
    }
close_file:
    pal_file_close(file);
}

static int32_t worker_main(void *arg) {
    PS5C_UNUSED(arg);
    pal_mutex_lock(g.lock);
    for (;;) {
        job *j = NULL;
        while (!g.stop && !j && g.late_head != g.late_tail) {
            job *c = g.jobs[g.late[g.late_head++ % g.late_cap]];
            if (c->state == J_QUEUED) j = c; /* else the list's turn already took it */
        }
        while (!g.stop && !j && g.next < g.queued) {
            job *c = g.jobs[g.next++];
            if (c->state == J_QUEUED) j = c;
        }
        if (g.stop) break;
        if (!j) {
            pal_cond_wait(g.cond, g.lock);
            continue;
        }
        j->state = J_RUNNING;
        g.busy++;
        if (j->late) g.busy_late++;
        pal_mutex_unlock(g.lock);
        vk_built_pipeline b = {0};
        const uint32_t *const words[2] = {j->words[0], j->words[1]};
        bool ok = vk_pipeline_build(g.r, j->recipe, j->late ? words : NULL, j->late ? j->n : NULL, &b);
        pal_mutex_lock(g.lock);
        g.busy--;
        if (j->late) g.busy_late--;
        j->state = ok ? J_DONE : j->late ? J_FAILED : J_ABSENT;
        j->b = b;
        free(j->words[0]);
        free(j->words[1]);
        j->words[0] = j->words[1] = NULL;
        if (ok && !j->listed) {
            j->listed = list_append(j->recipe);
        }
        if (j->prewarm) {
            j->prewarm = false;
            if (ok) g.pre_done++;
            else g.pre_failed++;
            if (g.pre_done + g.pre_failed == g.queued) {
                g.pre_ns = pal_time_ns() - g.pre_start;
                LOG_I(GPU, "pipelines: background build finished, %u of %u in %.0f ms (%u not built: their draws build them)",
                      g.pre_done, g.queued, (double)g.pre_ns / 1e6, g.pre_failed);
            }
        }
        pal_cond_broadcast(g.cond);
    }
    g.running--;
    pal_cond_broadcast(g.cond);
    pal_mutex_unlock(g.lock);
    return 0;
}

/* g.lock held: a draw's build goes to the workers, ahead of the list. False: no memory. */
static bool hand_over(job *j, uint32_t at, const uint32_t *const words[2], const size_t n[2]) {
    if (j->late && j->state == J_QUEUED) return true; /* already owns the live words */
    if (g.late_tail - g.late_head == g.late_cap) {
        uint32_t cap = g.late_cap ? g.late_cap * 2 : 64, *q = (uint32_t *)malloc(sizeof(uint32_t) * cap);
        if (!q) return false;
        for (uint32_t i = 0; i < g.late_tail - g.late_head; i++) q[i] = g.late[(g.late_head + i) % g.late_cap];
        g.late_tail -= g.late_head;
        g.late_head = 0;
        free(g.late);
        g.late = q;
        g.late_cap = cap;
    }
    uint32_t *copies[2] = {NULL, NULL};
    for (uint32_t i = 0; i < 2; i++) {
        if (!words || !words[i]) continue;
        if (n[i] > SIZE_MAX / 4 || !(copies[i] = (uint32_t *)malloc(n[i] * 4))) {
            free(copies[0]);
            free(copies[1]);
            return false;
        }
        memcpy(copies[i], words[i], n[i] * 4);
    }
    for (uint32_t i = 0; i < 2; i++) { j->words[i] = copies[i]; j->n[i] = copies[i] ? n[i] : 0; }
    j->late = true;
    j->state = J_QUEUED;
    g.late[g.late_tail++ % g.late_cap] = at;
    pal_cond_broadcast(g.cond);
    return true;
}

void vk_pipeline_async_start(renderer *r) {
    if (g.on) {
        LOG_E(GPU, "pipelines: previous session still owns its workers and device");
        return;
    }
    /* a session starts from a clean state (an earlier one ended in vk_pipeline_async_stop) */
    memset(&g, 0, sizeof(g));
    if (pal_mutex_create(&g.stat_lock) != PS5C_OK) g.stat_lock = NULL;
    g.r = r;
    g.rsize = vk_pipeline_recipe_size();
    const char *dir = vk_shader_cache_dir();
    const char *env = getenv("PS5C_PIPELINES_ASYNC");
    if ((env && !strcmp(env, "0")) || !dir) {
        LOG_I(GPU, "pipelines: built when a draw needs them (%s)", dir ? "PS5C_PIPELINES_ASYNC=0" : "shader cache off");
        return;
    }
    if (pal_mutex_create(&g.lock) != PS5C_OK) return;
    if (pal_cond_create(&g.cond) != PS5C_OK) {
        pal_mutex_destroy(g.lock);
        g.lock = NULL;
        return;
    }
    hmap64_init(&g.index);
    hmap64_init(&g.recorded);
    g.list_limit = vk_cache_budget_mb("PS5C_PIPELINE_LIST_MB", 256);
    g.recipe_budget = vk_cache_budget_mb("PS5C_PIPELINE_PREWARM_RECIPE_MB", 8);
    g.source_budget = vk_cache_budget_mb("PS5C_PIPELINE_PREWARM_MB", 16);
    g.prewarm_limit = (uint32_t)vk_cache_budget("PS5C_PIPELINE_PREWARM_COUNT", 64, 65536);
    g.record_limit = (uint32_t)vk_cache_budget("PS5C_PIPELINE_LIST_ENTRIES", 65536, 262144);
    char path[400];
    snprintf(path, sizeof(path), "%s/pipelines-list.bin", dir);
    load_list(path);
    g.on = true;
    const char *t = getenv("PS5C_PIPELINE_THREADS");
    uint32_t n = t ? (uint32_t)atoi(t) : 2;
    if (n > MAX_WORKERS) n = MAX_WORKERS;
    if (!n) n = 1; /* draws' builds need one */
    const char *wait = getenv("PS5C_PIPELINE_WAIT_MS");
    g.wait_budget = (wait ? (uint64_t)atoll(wait) : 2000) * 1000000ull;
    g.pre_start = pal_time_ns();
    /* workers start by taking the lock: each is counted before any can finish */
    pal_mutex_lock(g.lock);
    for (uint32_t i = 0; i < n; i++) {
        char name[32];
        snprintf(name, sizeof(name), "pipelines %u", i);
        if (pal_thread_create(&g.workers[g.nworkers], worker_main, NULL, name, 0) == PS5C_OK) {
            g.nworkers++;
            g.running++;
        }
    }
    pal_mutex_unlock(g.lock);
    LOG_I(GPU, "pipelines: %u from earlier sessions, built in the background by %u threads", g.queued, g.nworkers);
}

bool vk_pipeline_async_stop(renderer *r) {
    if (g.on) {
        pal_mutex_lock(g.lock);
        g.stop = true;
        pal_cond_broadcast(g.cond);
        uint64_t until = pal_time_ns() + 3000000000ull;
        while (g.busy && pal_time_ns() < until) pal_cond_wait_ns(g.cond, g.lock, 100000000);
        uint32_t stuck = g.busy;
        pal_mutex_unlock(g.lock);
        if (stuck) { /* a build inside the driver that does not come back: leave it and what it uses */
            LOG_W(GPU, "pipelines: %u builds still in the driver at shutdown, left running", stuck);
            return false;
        }
        for (uint32_t i = 0; i < g.nworkers; i++) pal_thread_join(g.workers[i], NULL);
        g.nworkers = 0;
        for (uint32_t i = 0; i < g.count; i++) {
            job *j = g.jobs[i];
            if (j->b.pipe) vkDestroyPipeline(r->device, j->b.pipe, NULL);
            if (j->b.layout) vkDestroyPipelineLayout(r->device, j->b.layout, NULL);
            if (j->b.dsl) vkDestroyDescriptorSetLayout(r->device, j->b.dsl, NULL);
            free(j->words[0]);
            free(j->words[1]);
            free(j->recipe);
            free(j);
        }
        free(g.jobs);
        free(g.late);
        g.jobs = NULL;
        g.count = g.cap = 0;
        hmap64_free(&g.index);
        hmap64_free(&g.recorded);
        if (g.list) pal_file_close(g.list);
        g.list = NULL;
        pal_cond_destroy(g.cond);
        pal_mutex_destroy(g.lock);
    }
    if (g.stat_lock) pal_mutex_destroy(g.stat_lock);
    memset(&g, 0, sizeof(g)); /* workers joined: nothing uses the state any more */
    return true;
}

bool vk_pipeline_async_progress(uint32_t *done, uint32_t *total, uint32_t *draws) {
    *done = *total = *draws = 0;
    if (!g.on) return false;
    pal_mutex_lock(g.lock);
    *done = g.pre_done + g.pre_failed;
    *total = g.queued;
    *draws = g.late_tail - g.late_head + g.busy_late; /* the queue may hold taken entries: an upper bound */
    pal_mutex_unlock(g.lock);
    return *done < *total || *draws;
}

bool vk_pipeline_async_get(renderer *r, const void *recipe, const uint32_t *const words[2], const size_t n[2],
                           vk_built_pipeline *out, bool *shared, bool *pending, bool must_complete) {
    *shared = false;
    *pending = false;
    uint64_t t0 = pal_time_ns(), key = hash_recipe(recipe, g.rsize);
    pal_mutex_lock(g.lock);
    if (!g.nworkers) { /* thread creation failed: no job can make progress */
        pal_mutex_unlock(g.lock);
        bool ok = vk_pipeline_build(r, recipe, words, n, out);
        vk_pipeline_async_note_wait(pal_time_ns() - t0, true);
        return ok;
    }
    uint64_t at = 0;
    job *j = hmap64_get(&g.index, key, &at) ? g.jobs[at] : NULL;
    if (!must_complete && j && j->over && (j->state == J_QUEUED || j->state == J_RUNNING)) {
        /* already over its budget once: skipped again without waiting */
        pal_mutex_unlock(g.lock);
        *pending = true;
        return false;
    }
    bool handed = false;
retry_build:
    if (!j || j->state == J_QUEUED || j->state == J_ABSENT) {
        /* not built yet: the workers build it, first in line (one queued from the list too) */
        if (!j && (j = add_job(key, recipe, J_QUEUED)) != NULL) at = g.count - 1;
        if (!j || !hand_over(j, (uint32_t)at, words, n)) {
            /* no memory for the table: built here, owned by the caller */
            pal_mutex_unlock(g.lock);
            vk_built_pipeline b = {0};
            bool ok = vk_pipeline_build(r, recipe, words, n, &b);
            vk_pipeline_async_note_wait(pal_time_ns() - t0, true);
            if (ok) *out = b;
            return ok;
        }
        handed = true;
    }
    uint64_t budget = must_complete ? 0 : g.wait_budget;
    while (j->state == J_QUEUED || j->state == J_RUNNING) {
        uint64_t spent = pal_time_ns() - t0;
        if (budget && spent >= budget) {
            static uint32_t told;
            if (told++ < 16)
                LOG_W(GPU, "pipeline build over %.0f ms: its draws are skipped until it is done", (double)g.wait_budget / 1e6);
            j->over = true; /* the next draws do not wait for it */
            pal_mutex_unlock(g.lock);
            vk_pipeline_async_note_wait(spent, handed);
            *pending = true;
            return false;
        }
        if (budget) pal_cond_wait_ns(g.cond, g.lock, budget - spent);
        else pal_cond_wait(g.cond, g.lock);
    }
    /* A speculative build can lack the cached SPIR-V. The request has the
     * live words: retry once as a late job, whose failure is then terminal.
     * Otherwise a compute caller would mistake a prewarm miss for completion. */
    if (j->state == J_ABSENT && !j->late) goto retry_build;
    bool ok = j->state == J_DONE;
    if (ok) {
        *out = j->b;
        *shared = true;
        if (handed) g.built_here++;
        else g.adopted++;
    }
    pal_mutex_unlock(g.lock);
    vk_pipeline_async_note_wait(pal_time_ns() - t0, handed);
    return ok;
}

void vk_pipeline_async_note_wait(uint64_t ns, bool built_here) {
    if (!g.stat_lock) return;
    pal_mutex_lock(g.stat_lock);
    g.waits++;
    g.wait_ns += ns;
    if (ns > g.wait_max) g.wait_max = ns;
    g.builds += built_here;
    g.all_waits++;
    g.all_wait_ns += ns;
    if (ns > g.all_wait_max) g.all_wait_max = ns;
    pal_mutex_unlock(g.stat_lock);
}

void vk_pipeline_async_frame(void) {
    if (!g.stat_lock) return;
    uint64_t now = pal_time_ns();
    pal_mutex_lock(g.stat_lock);
    if (g.last_frame) {
        uint64_t d = now - g.last_frame;
        g.frames++;
        g.frame_ns += d;
        if (d > g.frame_max) g.frame_max = d;
    } else {
        g.win_start = now;
    }
    g.last_frame = now;
    if (now - g.win_start >= STATS_NS && g.frames) {
        LOG_I(GPU, "pauses: %llu frames in %.0f s (average %.0f ms, longest %.0f ms); %llu waits for pipelines, "
                   "%.0f ms (longest %.0f ms, %llu built during draws); session %llu waits, %.0f ms, longest %.0f ms",
              (unsigned long long)g.frames, (double)(now - g.win_start) / 1e9, (double)g.frame_ns / 1e6 / (double)g.frames,
              (double)g.frame_max / 1e6, (unsigned long long)g.waits, (double)g.wait_ns / 1e6, (double)g.wait_max / 1e6,
              (unsigned long long)g.builds, (unsigned long long)g.all_waits, (double)g.all_wait_ns / 1e6,
              (double)g.all_wait_max / 1e6);
        g.win_start = now;
        g.frames = g.frame_ns = g.frame_max = 0;
        g.waits = g.wait_ns = g.wait_max = g.builds = 0;
    }
    pal_mutex_unlock(g.stat_lock);
}
