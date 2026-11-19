/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Persistent shader cache, per game in <user dir>/cache/ (user/<game folder>/cache):
 *   spirv.bin   the SPIR-V of each translation, under a key over everything the
 *               translator's output depends on (code hash and size, the load
 *               address when the code reads its PC, stage info, site shapes,
 *               binding base, Inf/NaN variant). Records are appended as
 *               translations happen; a damaged tail is dropped at load. Bodies
 *               are indexed by disk offset, with a bounded resident LRU.
 *   pipelines-<vendor>-<device>.bin  the driver's VkPipelineCache data, saved once
 *               new pipelines have settled and before exiting.
 *   sources.bin  each translation's inputs (shader_corpus.h encoding, code included).
 * SPIR-V starts over when translator sources change (PS5C_SHADER_TRANSLATOR_HASH,
 * computed at build time); the shaders in sources.bin are then translated again at
 * start and vk_pipeline_async.c carries the pipeline list over to the new keys, so
 * an update does not turn every pipeline into a draw-time driver compile
 * (PS5C_NO_SHADER_MIGRATION=1 starts over as before; PS5C_SHADER_SOURCES_MB caps the file). Opaque Vulkan data is compatible by its device/UUID,
 * independently of the translator: the driver keys complete pipeline inputs.
 * PS5C_NO_SHADER_CACHE=1 turns both caches off. */
#include "gpu/vk/vk_shader_cache.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/hashmap.h"
#include "base/log.h"
#include "gpu/shader/shader_corpus.h"
#include "gpu/shader/shader_ops.h"
#include "gpu/vk/vk_pipeline_async.h"
#include "gpu/vk/vk_cache_budget.h"
#include "pal/pal.h"
#include "ps5c/shader_cache_version.h"

#define FILE_MAGIC 0x31565053355A5350ull /* "PSZ5SPV1" */
#define PIPE_MAGIC 0x31455049505A5350ull /* "PSZPIPE1" */
#define REC_MAGIC 0x52565053u            /* "SPVR" */
#define SRC_MAGIC 0x31435253355A5350ull /* "PSZ5SRC1" */
#define SRC_REC_MAGIC 0x52435253u        /* "SRCR" */
#define SRC_MAX_BYTES (64u << 20)
#define SRC_WAVE32 1u
#define MAX_WORDS (1u << 22)
#define SETTLE_NS 2000000000ull  /* save the pipeline cache 2 s after the last new pipeline */
#define MAX_UNSAVED_NS 60000000000ull /* or at least once a minute while they keep coming */
#define NO_ENTRY UINT32_MAX
#define NO_OFFSET UINT64_MAX
#define SCAN_BYTES 65536u

typedef struct file_head {
    uint64_t magic, translator;
} file_head;

typedef struct rec_head {
    uint32_t magic, words;
    uint64_t key, check; /* check: hash of the code (seeded with the key) */
} rec_head;

/* sources.bin record: translation inputs (u32 flags + shader_corpus.h encoding)
 * under the key their translation had when stored; check: hash of the payload. */
typedef struct src_head {
    uint32_t magic, bytes;
    uint64_t key, check;
} src_head;

/* After a translator change: a shader translated again from its sources. */
typedef struct mig_entry {
    uint64_t key;    /* its key now */
    uint8_t *types;  /* binding types (vk_pipeline_binding_types) */
    uint32_t n;
} mig_entry;

typedef struct entry {
    uint64_t key, offset, check;
    uint32_t *code;
    uint32_t words, prev, next; /* LRU links, only resident code is on this list */
} entry;

static struct {
    char dir[320], cache_dir[340];
    bool off, preserve, buffer_atomic64, lds_atomic64, pixel_interlock;
    pal_mutex *lock;
    pal_mutex *save_lock; /* snapshots and the temporary file have one writer */
    renderer *owner;
    VkDevice device;
    VkPipelineCache pipes;
    char pipe_path[400];
    hmap64 index; /* key -> entries[] */
    entry *entries;
    uint32_t count, cap;
    pal_file *file;
    bool writable;
    uint64_t end; /* where the next record goes */
    uint64_t resident, ram_limit, disk_limit, driver_limit;
    uint32_t entry_limit, first, last;
    /* this run */
    uint32_t loaded, hits, misses, pipelines;
    bool unsaved;
    uint64_t translate_ns, pipeline_ns, last_pipeline, last_save, retry_after;
    /* sources.bin: what each translation was made from, kept across translator
     * changes so a new translator can translate the game's shaders again at start */
    pal_file *src;
    bool src_writable;
    uint64_t src_end, src_limit;
    hmap64 src_keys;
    hmap64 mig_index; /* old key -> mig[] */
    mig_entry *mig;
    uint32_t mig_count, mig_cap;
    bool mig_on;
} g;

static uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v * 0x9E3779B97F4A7C15ull;
    h = (h << 27 | h >> 37) * 0xC2B2AE3D27D4EB4Full;
    return h;
}

static uint64_t finish(uint64_t h) {
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    return h ^ (h >> 33);
}

static uint64_t hash_bytes(const void *data, size_t size, uint64_t h) {
    const uint8_t *p = (const uint8_t *)data;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = mix(h, w);
    }
    uint64_t tail = 0;
    if (size > i) memcpy(&tail, p + i, size - i);
    return finish(mix(h, tail ^ ((uint64_t)size << 56)));
}

void renderer_set_cache_dir(const char *game_user_dir) {
    snprintf(g.dir, sizeof(g.dir), "%s", game_user_dir ? game_user_dir : "");
}

void vk_shader_cache_device(bool preserve_inf_nan, bool buffer_atomic64, bool lds_atomic64, bool pixel_interlock) {
    g.preserve = preserve_inf_nan;
    g.buffer_atomic64 = buffer_atomic64;
    g.lds_atomic64 = lds_atomic64;
    g.pixel_interlock = pixel_interlock;
}

VkPipelineCache vk_shader_cache_pipelines(void) { return g.pipes; }

/* Pipeline creations share the cache; a save reads it alone. vkGetPipelineCacheData
 * needs no external synchronization by the spec, but igvk64.dll read a null pointer
 * when a pipeline was created meanwhile (Sonic Frontiers, Dragon Ball). */
static pal_mutex *g_use_lock;
static pal_cond *g_use_cond;
static uint32_t g_users;
static bool g_saving;

static bool use_init(void) {
    if (g_use_lock && g_use_cond) return true;
    if (pal_mutex_create(&g_use_lock) != PS5C_OK) return false;
    if (pal_cond_create(&g_use_cond) != PS5C_OK) {
        pal_mutex_destroy(g_use_lock);
        g_use_lock = NULL;
        return false;
    }
    return true;
}

void vk_shader_cache_use_begin(void) {
    if (!g_use_lock) return;
    pal_mutex_lock(g_use_lock);
    while (g_saving) pal_cond_wait(g_use_cond, g_use_lock);
    g_users++;
    pal_mutex_unlock(g_use_lock);
}

void vk_shader_cache_use_end(void) {
    if (!g_use_lock) return;
    pal_mutex_lock(g_use_lock);
    if (g_users && !--g_users) pal_cond_broadcast(g_use_cond);
    pal_mutex_unlock(g_use_lock);
}

uint64_t vk_shader_cache_key(const sh_program *p, const sh_stage_info *info, const sh_site_shape *shapes,
                             uint32_t binding_base) {
    bool reads_pc = false; /* s_getpc_b64 makes the load address a constant of the code */
    for (uint32_t i = 0; i < p->count; i++) reads_pc |= p->ops[i] == SH_OP_S_GETPC_B64;
    uint64_t h = mix(PS5C_SHADER_TRANSLATOR_HASH, p->hash);
    h = mix(h, p->dwords);
    h = mix(h, p->wave32);
    h = mix(h, sh_translation_flags(info->stage));
    h = mix(h, sh_translation_wave64_lds(p->hash));
    h = mix(h, reads_pc && !sh_pc_uniform(p) ? p->base : 0);
    h = hash_bytes(info, sizeof(*info), h);
    h = hash_bytes(shapes, sizeof(*shapes) * p->site_count, h);
    h = mix(h, binding_base);
    return finish(mix(h, (uint32_t)g.preserve | ((uint32_t)g.buffer_atomic64 << 1) |
        ((uint32_t)g.lds_atomic64 << 2) | ((uint32_t)g.pixel_interlock << 3)));
}

/* g.lock held (or loading); the disk index does not retain shader bodies. */
static entry *add_entry(uint64_t key, uint64_t offset, uint32_t words, uint64_t check) {
    if (g.count >= g.entry_limit) return NULL;
    if (g.count == g.cap) {
        uint32_t cap = g.cap ? g.cap * 2 : 256;
        if (cap > g.entry_limit) cap = g.entry_limit;
        entry *e = (entry *)realloc(g.entries, sizeof(entry) * cap);
        if (!e) return NULL;
        g.entries = e;
        g.cap = cap;
    }
    if (!hmap64_put(&g.index, key, g.count)) return NULL;
    entry *e = &g.entries[g.count++];
    *e = (entry){.key=key, .offset=offset, .check=check, .words=words, .prev=NO_ENTRY, .next=NO_ENTRY};
    return e;
}

static void unlink_resident(uint32_t i) {
    entry *e = &g.entries[i];
    if (e->prev != NO_ENTRY) g.entries[e->prev].next = e->next;
    else g.first = e->next;
    if (e->next != NO_ENTRY) g.entries[e->next].prev = e->prev;
    else g.last = e->prev;
}

static void touch(uint32_t i) {
    entry *e = &g.entries[i];
    if (g.first == i) return;
    if (e->code) unlink_resident(i);
    e->prev = NO_ENTRY;
    e->next = g.first;
    if (g.first != NO_ENTRY) g.entries[g.first].prev = i;
    else g.last = i;
    g.first = i;
}

/* A hot copy is optional: a miss in this LRU still reads the verified disk record. */
static void retain_code(uint32_t i, const uint32_t *code) {
    entry *e = &g.entries[i];
    uint64_t bytes = (uint64_t)e->words * 4;
    if (e->code) { touch(i); return; }
    if (bytes > g.ram_limit) return;
    while (g.resident > g.ram_limit - bytes) {
        entry *old = &g.entries[g.last];
        unlink_resident(g.last);
        g.resident -= (uint64_t)old->words * 4;
        free(old->code);
        old->code = NULL;
        old->prev = old->next = NO_ENTRY;
    }
    uint32_t *copy = malloc((size_t)bytes);
    if (!copy) return;
    memcpy(copy, code, (size_t)bytes);
    touch(i);
    e->code = copy;
    g.resident += bytes;
}

static bool read_exact(pal_file *file, void *data, uint64_t bytes, uint64_t offset) {
    uint64_t done = 0;
    return pal_file_pread(file, data, bytes, offset, &done) == PS5C_OK && done == bytes;
}

/* Same checksum as hash_bytes, using a fixed scratch allocation while scanning. */
static bool check_record(const rec_head *rec, uint64_t offset, uint8_t *scratch, bool *read_error) {
    uint64_t bytes = (uint64_t)rec->words * 4, left = bytes, h = rec->key;
    while (left >= 8) {
        size_t n = left >= SCAN_BYTES ? SCAN_BYTES : (size_t)(left & ~7ull);
        if (!read_exact(g.file, scratch, n, offset)) { *read_error = true; return false; }
        for (size_t i = 0; i < n; i += 8) {
            uint64_t word;
            memcpy(&word, scratch + i, 8);
            h = mix(h, word);
        }
        offset += n;
        left -= n;
    }
    uint64_t tail = 0;
    if (left && !read_exact(g.file, &tail, left, offset)) { *read_error = true; return false; }
    return finish(mix(h, tail ^ (bytes << 56))) == rec->check;
}

static uint64_t write_record(const rec_head *h, const uint32_t *code) {
    uint64_t bytes = (uint64_t)h->words * 4;
    if (!g.writable || g.end > g.disk_limit || sizeof(*h) + bytes > g.disk_limit - g.end) return NO_OFFSET;
    uint64_t done = 0, offset = g.end + sizeof(*h);
    bool ok = pal_file_pwrite(g.file, h, sizeof(*h), g.end, &done) == PS5C_OK && done == sizeof(*h) &&
              pal_file_pwrite(g.file, code, bytes, offset, &done) == PS5C_OK && done == bytes;
    if (ok) { g.end = offset + bytes; return offset; }
    LOG_W(GPU, "shader cache: cannot write %s/cache/spirv.bin", g.dir);
    /* Keep the handle for reads of earlier records. A damaged tail is repaired
     * at next open; do not keep appending after a short write. */
    g.writable = false;
    return NO_OFFSET;
}

/* Index valid records with bounded scratch space. Repair only a damaged tail;
 * reaching a memory/disk admission limit or an I/O failure never truncates it. */
static void load_spirv(const char *path) {
    g.writable = pal_file_open(path, PAL_FILE_READ | PAL_FILE_WRITE | PAL_FILE_CREATE, &g.file) == PS5C_OK;
    if (!g.writable && pal_file_open(path, PAL_FILE_READ, &g.file) != PS5C_OK) return;
    uint64_t size = 0;
    if (pal_file_size(g.file, &size) != PS5C_OK) { g.writable = false; return; }
    file_head fh = {0}, want = {FILE_MAGIC, PS5C_SHADER_TRANSLATOR_HASH};
    if (size >= sizeof(fh) && !read_exact(g.file, &fh, sizeof(fh), 0)) { g.writable = false; return; }
    if (size < sizeof(fh) || memcmp(&fh, &want, sizeof(fh))) {
        if (size) LOG_I(GPU, "shader cache: the shader translator changed, starting over");
        uint64_t done = 0;
        if (!g.writable || g.disk_limit < sizeof(want) || pal_file_truncate(g.file, 0) != PS5C_OK ||
            pal_file_pwrite(g.file, &want, sizeof(want), 0, &done) != PS5C_OK || done != sizeof(want)) {
            g.writable = false;
            return;
        }
        g.end = sizeof(want);
        return;
    }
    uint8_t *scratch = malloc(SCAN_BYTES);
    if (!scratch) { g.writable = false; return; }
    uint64_t good = sizeof(fh);
    bool read_error = false, limited = false;
    while (size - good >= sizeof(rec_head)) {
        if (g.count >= g.entry_limit || good >= g.disk_limit) { limited = true; break; }
        rec_head h;
        if (!read_exact(g.file, &h, sizeof(h), good)) { read_error = true; break; }
        uint64_t bytes = (uint64_t)h.words * 4;
        if (h.magic != REC_MAGIC || !h.words || h.words > MAX_WORDS || bytes > size - good - sizeof(h)) break;
        if (sizeof(h) + bytes > g.disk_limit - good) { limited = true; break; }
        if (!check_record(&h, good + sizeof(h), scratch, &read_error)) break;
        if (!hmap64_get(&g.index, h.key, &(uint64_t){0}) && !add_entry(h.key, good + sizeof(h), h.words, h.check)) {
            limited = true;
            break;
        }
        good += sizeof(h) + bytes;
    }
    free(scratch);
    g.loaded = g.count;
    g.end = good;
    if (read_error || limited) g.writable = false;
    else if (good != size && g.writable && pal_file_truncate(g.file, good) != PS5C_OK) g.writable = false;
}

/* The driver's data after our unchanged file header. The translator tag is
 * provenance only here; SPIR-V/recipe compatibility is checked separately. */
static VkPipelineCache load_pipelines(renderer *r) {
    uint8_t *data = NULL;
    uint64_t size = 0;
    pal_file *file = NULL;
    VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    if (g.driver_limit && pal_file_open(g.pipe_path, PAL_FILE_READ, &file) == PS5C_OK &&
        pal_file_size(file, &size) == PS5C_OK && size >= sizeof(file_head) + 32 && size - sizeof(file_head) <= g.driver_limit) {
        file_head fh;
        uint8_t d[32];
        uint32_t hsize, hversion, vendor, device;
        if (!read_exact(file, &fh, sizeof(fh), 0) || !read_exact(file, d, sizeof(d), sizeof(fh))) goto no_initial;
        memcpy(&hsize, d, 4);
        memcpy(&hversion, d + 4, 4);
        memcpy(&vendor, d + 8, 4);
        memcpy(&device, d + 12, 4);
        /* drivers check this header too; some crash on data that is not theirs */
        if (fh.magic == PIPE_MAGIC && hsize == 32 && hsize <= size - sizeof(fh) &&
            hversion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && vendor == r->props.vendorID &&
            device == r->props.deviceID && !memcmp(d + 16, r->props.pipelineCacheUUID, VK_UUID_SIZE)) {
            data = malloc((size_t)(size - sizeof(fh)));
            if (data && read_exact(file, data, size - sizeof(fh), sizeof(fh))) {
                ci.initialDataSize = (size_t)(size - sizeof(fh));
                ci.pInitialData = data;
            }
        }
    }
no_initial:
    if (file) pal_file_close(file);
    VkPipelineCache c = VK_NULL_HANDLE;
    if (vk_check_device(r, vkCreatePipelineCache(r->device, &ci, NULL, &c), "vkCreatePipelineCache") != VK_SUCCESS && ci.initialDataSize) {
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        vk_check_device(r, vkCreatePipelineCache(r->device, &ci, NULL, &c), "vkCreatePipelineCache");
    }
    LOG_I(GPU, "shader cache: %u translations, driver pipeline cache %llu KB%s", g.count,
          (unsigned long long)ci.initialDataSize / 1024, ci.initialDataSize || !data ? "" : " (initial data not used)");
    free(data);
    return c;
}

/* ---- Sources ---- */

static bool src_append(pal_file *f, uint64_t *end, uint64_t key, uint32_t flags, const sh_corpus_entry *e) {
    size_t enc = sh_corpus_encoded_size(e), bytes = 4 + enc;
    if (bytes > SRC_MAX_BYTES || *end > g.src_limit || sizeof(src_head) + bytes > g.src_limit - *end) return false;
    uint8_t *payload = (uint8_t *)malloc(bytes);
    if (!payload) return false;
    memcpy(payload, &flags, 4);
    bool ok = sh_corpus_encode(e, payload + 4, enc);
    src_head h = {SRC_REC_MAGIC, (uint32_t)bytes, key, ok ? hash_bytes(payload, bytes, key) : 0};
    uint64_t done = 0;
    ok = ok && pal_file_pwrite(f, &h, sizeof(h), *end, &done) == PS5C_OK && done == sizeof(h) &&
         pal_file_pwrite(f, payload, bytes, *end + sizeof(h), &done) == PS5C_OK && done == bytes;
    if (ok) *end += sizeof(h) + bytes;
    free(payload);
    return ok;
}

/* Reads the record at *at (advanced past it); NULL at the end or on damage. */
static uint8_t *src_read(pal_file *f, uint64_t size, uint64_t *at, src_head *h) {
    if (size - *at < sizeof(*h) || !read_exact(f, h, sizeof(*h), *at)) return NULL;
    if (h->magic != SRC_REC_MAGIC || h->bytes < 4 || h->bytes > SRC_MAX_BYTES || h->bytes > size - *at - sizeof(*h))
        return NULL;
    uint8_t *payload = (uint8_t *)malloc(h->bytes);
    if (!payload || !read_exact(f, payload, h->bytes, *at + sizeof(*h)) || hash_bytes(payload, h->bytes, h->key) != h->check) {
        free(payload);
        return NULL;
    }
    *at += sizeof(*h) + h->bytes;
    return payload;
}

static void mig_add(uint64_t old_key, uint64_t key, const uint8_t *types, uint32_t n) {
    if (hmap64_get(&g.mig_index, old_key, &(uint64_t){0})) return;
    if (g.mig_count == g.mig_cap) {
        uint32_t cap = g.mig_cap ? g.mig_cap * 2 : 256;
        mig_entry *m = (mig_entry *)realloc(g.mig, sizeof(*m) * cap);
        if (!m) return;
        g.mig = m;
        g.mig_cap = cap;
    }
    uint8_t *copy = (uint8_t *)malloc(n ? n : 1);
    if (!copy) return;
    memcpy(copy, types, n);
    if (!hmap64_put(&g.mig_index, old_key, g.mig_count)) {
        free(copy);
        return;
    }
    g.mig[g.mig_count++] = (mig_entry){key, copy, n};
}

/* The translator changed: translate the recorded shaders again (their SPIR-V
 * goes to spirv.bin under their new keys), note old key -> new key for the
 * pipeline list (vk_pipeline_async.c), and rewrite sources.bin with new keys.
 * Code that no longer analyzes or translates is dropped. */
static void migrate_sources(pal_file *old, uint64_t size, const char *path) {
    char tmp[420];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    pal_file *nf = NULL;
    file_head want = {SRC_MAGIC, PS5C_SHADER_TRANSLATOR_HASH};
    uint64_t done = 0, end = sizeof(want), t0 = pal_time_ns();
    bool fresh = pal_file_open(tmp, PAL_FILE_READ | PAL_FILE_WRITE | PAL_FILE_CREATE, &nf) == PS5C_OK &&
                 pal_file_truncate(nf, 0) == PS5C_OK && pal_file_pwrite(nf, &want, sizeof(want), 0, &done) == PS5C_OK &&
                 done == sizeof(want);
    sh_program *p = (sh_program *)malloc(sizeof(sh_program));
    uint8_t *types = (uint8_t *)malloc(SH_MAX_SITES + 1);
    sh_corpus_entry *ep = (sh_corpus_entry *)malloc(sizeof(sh_corpus_entry)); /* shapes[]: too big for a stack */
    uint32_t total = 0, kept = 0, translated = 0;
    hmap64_init(&g.mig_index);
    g.mig_on = true;
    for (uint64_t at = sizeof(file_head); p && types && ep;) {
        src_head h;
        uint8_t *payload = src_read(old, size, &at, &h);
        if (!payload) break;
        total++;
        uint32_t flags;
        memcpy(&flags, payload, 4);
        if (sh_corpus_decode(payload + 4, h.bytes - 4, ep)) {
            sh_corpus_entry *e = ep;
            bool ok = e->kind == SH_CORPUS_TRANSLATION && sh_corpus_analyze(e, p);
            if (ok) {
                p->wave32 = (flags & SRC_WAVE32) != 0;
                ok = sh_aperture_allowed(p);
                for (uint32_t i = 0; ok && i < p->site_count; i++)
                    if (e->shapes[i].buf_format == SH_IMG_ATOMIC_UINT64 && !sh_image_atomic64_supported()) ok = false;
            }
            uint64_t key = ok ? vk_shader_cache_key(p, &e->info, e->shapes, e->binding_base) : 0;
            if (ok && !vk_shader_cache_size(key)) {
                uint32_t *spv = NULL;
                size_t words = 0;
                ok = sh_translate(p, &e->info, e->shapes, e->binding_base, &spv, &words);
                if (ok) {
                    vk_shader_cache_store(key, spv, words);
                    translated++;
                }
                free(spv);
                ok = ok && vk_shader_cache_size(key);
            }
            if (ok) {
                kept++;
                mig_add(h.key, key, types, vk_pipeline_binding_types(p, e->shapes, types));
                if (fresh && !hmap64_get(&g.src_keys, key, &(uint64_t){0}) && src_append(nf, &end, key, flags, e))
                    hmap64_put(&g.src_keys, key, 1);
            }
            sh_corpus_free(e);
        }
        free(payload);
    }
    free(p);
    free(types);
    free(ep);
    pal_file_close(old);
    if (nf) pal_file_close(nf);
    bool replaced = fresh && pal_file_rename(tmp, path) == PS5C_OK;
    if (!replaced) {
        pal_file_remove(tmp);
        hmap64_free(&g.src_keys);
        hmap64_init(&g.src_keys);
    }
    if (pal_file_open(path, PAL_FILE_READ | PAL_FILE_WRITE, &g.src) == PS5C_OK) {
        g.src_writable = replaced;
        g.src_end = replaced ? end : 0;
    }
    LOG_I(GPU, "shader cache: the translator changed; %u of %u recorded shaders translated again (%u new) in %.0f ms",
          kept, total, translated, (pal_time_ns() - t0) / 1e6);
}

/* sources.bin of this translator: index its keys (records are appended once each). */
static void open_sources(const char *dir) {
    char path[400];
    snprintf(path, sizeof(path), "%s/sources.bin", dir);
    hmap64_init(&g.src_keys);
    g.src_limit = vk_cache_budget_mb("PS5C_SHADER_SOURCES_MB", 256);
    pal_file *f = NULL;
    if (pal_file_open(path, PAL_FILE_READ | PAL_FILE_WRITE | PAL_FILE_CREATE, &f) != PS5C_OK) return;
    uint64_t size = 0, done = 0;
    file_head fh = {0}, want = {SRC_MAGIC, PS5C_SHADER_TRANSLATOR_HASH};
    if (pal_file_size(f, &size) != PS5C_OK) size = 0;
    bool head = size >= sizeof(fh) && read_exact(f, &fh, sizeof(fh), 0) && fh.magic == SRC_MAGIC;
    if (head && fh.translator != want.translator && !getenv("PS5C_NO_SHADER_MIGRATION")) {
        migrate_sources(f, size, path);
        return;
    }
    g.src = f;
    if (!head || fh.translator != want.translator) {
        g.src_writable = pal_file_truncate(f, 0) == PS5C_OK && pal_file_pwrite(f, &want, sizeof(want), 0, &done) == PS5C_OK &&
                         done == sizeof(want);
        g.src_end = sizeof(want);
        return;
    }
    uint64_t at = sizeof(fh);
    for (;;) {
        src_head h;
        uint64_t before = at;
        uint8_t *payload = src_read(f, size, &at, &h);
        if (!payload) { at = before; break; }
        free(payload);
        if (!hmap64_put(&g.src_keys, h.key, 1)) break;
    }
    g.src_end = at;
    g.src_writable = at == size || pal_file_truncate(f, at) == PS5C_OK;
}

void vk_shader_cache_note_source(uint64_t key, const sh_program *p, const sh_stage_info *info, const sh_site_shape *shapes,
                                 uint32_t binding_base) {
    if (g.off || !g.lock || !p || !p->code || p->function_count > 1) return;
    pal_mutex_lock(g.lock);
    bool known = !g.src || !g.src_writable || hmap64_get(&g.src_keys, key, &(uint64_t){0});
    sh_corpus_entry *e = known ? NULL : (sh_corpus_entry *)malloc(sizeof(sh_corpus_entry));
    if (e) {
        memset(e, 0, offsetof(sh_corpus_entry, shapes)); /* no compound literal: it would be a stack copy */
        e->kind = SH_CORPUS_TRANSLATION;
        e->base = p->base;
        e->info = *info;
        e->binding_base = binding_base;
        e->site_count = p->site_count;
        e->dwords = p->dwords;
        e->code = (uint32_t *)p->code;
        memcpy(e->shapes, shapes, sizeof(sh_site_shape) * p->site_count);
        if (src_append(g.src, &g.src_end, key, p->wave32 ? SRC_WAVE32 : 0, e)) hmap64_put(&g.src_keys, key, 1);
        else g.src_writable = false; /* full or failing: stop trying */
        free(e);
    }
    pal_mutex_unlock(g.lock);
}

bool vk_shader_cache_migrating(void) { return !g.off && g.mig_on; }

bool vk_shader_cache_migrated(uint64_t old_key, uint64_t *key, const uint8_t **types, uint32_t *n) {
    uint64_t i = 0;
    if (!g.mig_on || !hmap64_get(&g.mig_index, old_key, &i)) return false;
    *key = g.mig[i].key;
    *types = g.mig[i].types;
    *n = g.mig[i].n;
    return true;
}

void vk_shader_cache_migration_end(void) {
    if (!g.mig_on) return;
    for (uint32_t i = 0; i < g.mig_count; i++) free(g.mig[i].types);
    free(g.mig);
    g.mig = NULL;
    g.mig_count = g.mig_cap = 0;
    hmap64_free(&g.mig_index);
    g.mig_on = false;
}

void vk_shader_cache_open(renderer *r) {
    /* The previous session has closed. Preserve only its selected directory
     * and the device's translation capabilities; off/dirty/statistics are local. */
    g.off = false;
    g.loaded = g.hits = g.misses = g.pipelines = 0;
    g.unsaved = false;
    g.translate_ns = g.pipeline_ns = g.last_pipeline = g.retry_after = g.end = 0;
    g.owner = NULL;
    g.resident = 0;
    g.first = g.last = NO_ENTRY;
    g.writable = false;
    g.ram_limit = vk_cache_budget_mb("PS5C_SHADER_CACHE_RAM_MB", 64);
    g.disk_limit = vk_cache_budget_mb("PS5C_SHADER_CACHE_DISK_MB", 1024);
    g.driver_limit = vk_cache_budget_mb("PS5C_DRIVER_CACHE_MB", 256);
    g.entry_limit = (uint32_t)vk_cache_budget("PS5C_SHADER_CACHE_ENTRIES", 65536, 262144);
    /* probes change translations without changing their keys: no cache while probing */
    if (getenv("PS5C_NO_SHADER_CACHE") || getenv("PS5C_PS_PROBE") || getenv("PS5C_VS_PROBE") || getenv("PS5C_PS_SOLID") || getenv("PS5C_PS_NAN_TRACE") || getenv("PS5C_PS_CAPTURE") || !g.dir[0]) {
        g.off = true;
        LOG_I(GPU, "shader cache: off");
        return;
    }
    if (pal_mutex_create(&g.lock) != PS5C_OK) {
        g.off = true;
        return;
    }
    if (pal_mutex_create(&g.save_lock) != PS5C_OK) {
        pal_mutex_destroy(g.lock);
        g.lock = NULL;
        g.off = true;
        return;
    }
    hmap64_init(&g.index);
    g.device = r->device;
    g.owner = r;
    g.last_save = pal_time_ns();
    char *dir = g.cache_dir, path[360];
    snprintf(g.cache_dir, sizeof(g.cache_dir), "%s/cache", g.dir);
    pal_mkdir(dir);
    snprintf(path, sizeof(path), "%s/spirv.bin", dir);
    load_spirv(path);
    open_sources(dir);
    snprintf(g.pipe_path, sizeof(g.pipe_path), "%s/pipelines-%04x-%04x.bin", dir, r->props.vendorID, r->props.deviceID);
    /* Without the guard, retain only SPIR-V caching. Driver snapshots must
     * never race a pipeline creation when synchronization allocation failed. */
    g.pipes = g.driver_limit && use_init() ? load_pipelines(r) : VK_NULL_HANDLE;
}

static bool get_entry(uint64_t key, uint32_t **spirv, size_t *words, bool count) {
    if (g.off || !g.lock) return false;
    pal_mutex_lock(g.lock);
    uint64_t i = 0;
    bool hit = hmap64_get(&g.index, key, &i);
    if (hit) {
        entry *e = &g.entries[i];
        *spirv = (uint32_t *)malloc((size_t)e->words * 4);
        hit = *spirv != NULL;
        if (hit) {
            size_t bytes = (size_t)e->words * 4;
            if (e->code) { memcpy(*spirv, e->code, bytes); touch((uint32_t)i); }
            else {
                hit = e->offset != NO_OFFSET && g.file && read_exact(g.file, *spirv, bytes, e->offset);
                if (hit) hit = hash_bytes(*spirv, bytes, e->key) == e->check;
                if (hit) retain_code((uint32_t)i, *spirv);
            }
            if (hit) *words = e->words;
            else { free(*spirv); *spirv = NULL; }
        }
    }
    if (count && hit) g.hits++;
    else if (count) g.misses++;
    pal_mutex_unlock(g.lock);
    return hit;
}

bool vk_shader_cache_find(uint64_t key, uint32_t **spirv, size_t *words) { return get_entry(key, spirv, words, true); }

bool vk_shader_cache_lookup(uint64_t key, uint32_t **spirv, size_t *words) { return get_entry(key, spirv, words, false); }

uint64_t vk_shader_cache_size(uint64_t key) {
    if (g.off || !g.lock) return 0;
    pal_mutex_lock(g.lock);
    uint64_t i, size = 0;
    if (hmap64_get(&g.index, key, &i) && (g.entries[i].code || g.entries[i].offset != NO_OFFSET))
        size = (uint64_t)g.entries[i].words * 4;
    pal_mutex_unlock(g.lock);
    return size;
}

vk_shader_cache_usage vk_shader_cache_memory(void) {
    vk_shader_cache_usage usage = {0};
    if (g.off || !g.lock) return usage;
    pal_mutex_lock(g.lock);
    usage = (vk_shader_cache_usage){g.resident, g.ram_limit, g.count};
    pal_mutex_unlock(g.lock);
    return usage;
}

const char *vk_shader_cache_dir(void) { return g.off || !g.lock ? NULL : g.cache_dir; }

const char *vk_shader_cache_game_dir(void) { return g.dir; }

void vk_shader_cache_store(uint64_t key, const uint32_t *spirv, size_t words) {
    if (g.off || !g.lock || !words || words > MAX_WORDS) return;
    pal_mutex_lock(g.lock);
    uint64_t i = 0;
    bool exists = hmap64_get(&g.index, key, &i);
    if (!exists && g.count < g.entry_limit) {
        uint64_t check = hash_bytes(spirv, words * 4, key);
        entry *e = add_entry(key, NO_OFFSET, (uint32_t)words, check);
        if (e) {
            rec_head h = {REC_MAGIC, (uint32_t)words, key, check};
            i = g.count - 1;
            e->offset = write_record(&h, spirv);
            retain_code((uint32_t)i, spirv);
        }
    } else if (exists && !g.entries[i].code && g.entries[i].offset == NO_OFFSET) {
        /* A nonpersistent entry may have been evicted. The caller translated
         * it again; retain only an exact body for this translation key. */
        if (g.entries[i].words == words && hash_bytes(spirv, words * 4, key) == g.entries[i].check)
            retain_code((uint32_t)i, spirv);
    }
    pal_mutex_unlock(g.lock);
}

void vk_shader_cache_note_translate(uint64_t ns) {
    if (!g.lock) return;
    pal_mutex_lock(g.lock);
    g.translate_ns += ns;
    pal_mutex_unlock(g.lock);
}

void vk_shader_cache_note_pipeline(uint64_t ns) {
    if (!g.lock) return;
    pal_mutex_lock(g.lock);
    g.pipelines++;
    if (g.pipes) g.unsaved = true;
    g.pipeline_ns += ns;
    g.last_pipeline = pal_time_ns();
    pal_mutex_unlock(g.lock);
}

/* Writes the driver's pipeline cache (to a temporary file, then renamed) and
 * logs this run's numbers. */
static void retry_save(void) {
    pal_mutex_lock(g.lock);
    g.unsaved = true;
    g.retry_after = pal_time_ns() + SETTLE_NS;
    pal_mutex_unlock(g.lock);
}

/* save_lock held; pipeline builds may still note new work while disk I/O runs. */
static void save_pipelines_locked(void) {
    pal_mutex_lock(g.lock);
    bool due = g.unsaved && g.pipes;
    if (due) g.unsaved = false;
    LOG_I(GPU, "shader cache: %u pipelines created in %.0f ms; translations %u from the cache, %u new in %.0f ms (%u stored)",
          g.pipelines, (double)g.pipeline_ns / 1e6, g.hits, g.misses, (double)g.translate_ns / 1e6, g.count);
    pal_mutex_unlock(g.lock);
    if (!due) return;
    size_t size = 0;
    if (g_use_lock) { /* alone with the cache */
        pal_mutex_lock(g_use_lock);
        g_saving = true;
        /* not behind a build that does not end (Intel's compiler on one of No Man's Sky's
         * shaders): the caller presents frames. Saved at a later turn instead. */
        uint64_t until = pal_time_ns() + 200000000ull;
        while (g_users && pal_time_ns() < until) pal_cond_wait_ns(g_use_cond, g_use_lock, 20000000);
        if (g_users) {
            g_saving = false;
            pal_cond_broadcast(g_use_cond);
            pal_mutex_unlock(g_use_lock);
            retry_save();
            return;
        }
        pal_mutex_unlock(g_use_lock);
    }
    VkResult result = vk_check_device(g.owner, vkGetPipelineCacheData(g.device, g.pipes, &size, NULL), "vkGetPipelineCacheData");
    bool got = result == VK_SUCCESS && size && size <= g.driver_limit;
    uint8_t *data = got ? (uint8_t *)malloc(size) : NULL;
    size_t capacity = size;
    if (data) result = vk_check_device(g.owner, vkGetPipelineCacheData(g.device, g.pipes, &size, data), "vkGetPipelineCacheData");
    got = data && result == VK_SUCCESS && size && size <= capacity;
    if (g_use_lock) {
        pal_mutex_lock(g_use_lock);
        g_saving = false;
        pal_cond_broadcast(g_use_cond);
        pal_mutex_unlock(g_use_lock);
    }
    if (!got) {
        free(data);
        retry_save();
        return;
    }
    file_head fh = {PIPE_MAGIC, PS5C_SHADER_TRANSLATOR_HASH};
    char tmp[420];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g.pipe_path);
    pal_file *f = NULL;
    uint64_t done = 0, done2 = 0;
    bool ok = pal_file_open(tmp, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE, &f) == PS5C_OK &&
              pal_file_write(f, &fh, sizeof(fh), &done) == PS5C_OK && done == sizeof(fh) &&
              pal_file_write(f, data, size, &done2) == PS5C_OK && done2 == size;
    if (f) pal_file_close(f);
    free(data);
    if (ok) ok = pal_file_rename(tmp, g.pipe_path) == PS5C_OK;
    if (!ok) {
        LOG_W(GPU, "shader cache: cannot write %s", g.pipe_path);
        retry_save();
    } else {
        pal_mutex_lock(g.lock);
        g.last_save = pal_time_ns();
        g.retry_after = 0;
        pal_mutex_unlock(g.lock);
    }
}

static void save_pipelines(void) {
    pal_mutex_lock(g.save_lock);
    save_pipelines_locked();
    pal_mutex_unlock(g.save_lock);
}

void vk_shader_cache_tick(void) {
    if (g.off || !g.lock) return;
    uint64_t now = pal_time_ns();
    pal_mutex_lock(g.lock); /* pipeline builds on worker threads update these */
    bool due = g.pipes && g.unsaved && now >= g.retry_after &&
               (now - g.last_pipeline >= SETTLE_NS || now - g.last_save >= MAX_UNSAVED_NS);
    pal_mutex_unlock(g.lock);
    if (due) save_pipelines();
}

void renderer_save_caches(renderer *r) {
    PS5C_UNUSED(r);
    if (g.off || !g.lock) return;
    save_pipelines();
}

void vk_shader_cache_close(renderer *r) {
    if (g.off || !g.lock) return;
    save_pipelines();
    if (g.pipes) vkDestroyPipelineCache(r->device, g.pipes, NULL);
    g.pipes = VK_NULL_HANDLE;
    if (g.file) pal_file_close(g.file);
    g.file = NULL;
    vk_shader_cache_migration_end();
    if (g.src) pal_file_close(g.src);
    g.src = NULL;
    g.src_writable = false;
    hmap64_free(&g.src_keys);
    for (uint32_t i = 0; i < g.count; i++) free(g.entries[i].code);
    free(g.entries);
    g.entries = NULL;
    g.count = g.cap = 0;
    g.resident = 0;
    g.first = g.last = NO_ENTRY;
    hmap64_free(&g.index);
    pal_mutex_destroy(g.lock);
    g.lock = NULL;
    pal_mutex_destroy(g.save_lock);
    g.save_lock = NULL;
    g.owner = NULL;
}
