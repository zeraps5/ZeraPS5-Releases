/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PlayGo contracts verified against PS5 guest callers. PLGX describes required
 * data, not installation. Without a verified inode/installation map, nonempty
 * chunks have unknown availability; zero-payload chunks need no download.
 * A game run from a .pkg has one: the package reader, which checks that each
 * chunk's mchunk ranges (package offsets) are in the file and read. */
#include "base/log.h"
#include "core/runtime.h"
#include "hle/hle.h"
#include "hle/playgo_metadata.h"
#include "pal/pal.h"
#include "pal/pal_exfat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hle_register_table(const hle_export *table, size_t count);
int kernel_fs_resolve(hle_call *c, const char *guest, char *host, size_t size);
#define PG_ERROR(n) ((int32_t)(0x80b20000u + (n)))
#define PG_ARGUMENT PG_ERROR(4)
#define PG_UNINITIALIZED PG_ERROR(5)
#define PG_INITIALIZED PG_ERROR(6)
#define PG_HANDLE PG_ERROR(9)
#define PG_POINTER PG_ERROR(10)
#define PG_SIZE PG_ERROR(11)
#define PG_CHUNK PG_ERROR(12)
#define PG_UNSUPPORTED PG_ERROR(14)
#define PG_LOCUS PG_ERROR(16)
#define PG_NOMEM PG_ERROR(3)

typedef struct playgo_todo {
    uint16_t id;
    uint8_t locus, reserved;
} playgo_todo;
_Static_assert(sizeof(playgo_todo) == 4, "guest PlayGo ToDo ABI");
struct playgo_state {
    bool initialized;
    uint32_t handle, next_handle;
    playgo_metadata metadata;
    uint8_t *loci;
    playgo_todo *todo;
    uint32_t todo_count;
    char pkg_path[1024]; /* playgo-chunk.dat inside a .pkg volume, else empty */
    uint8_t *pkg_loci;   /* the package's answers so far; 0xff not asked yet */
};

/* The .pkg installation backend: a chunk is local (3) when the reader finds
 * every one of its ranges in the package file, not downloaded (0) when one
 * lies past its end; a read error leaves it unknown. The package is
 * read-only, so a local chunk stays local. */
static bool pkg_query_locus(void *user, uint16_t chunk_id, uint8_t *locus) {
    struct playgo_state *s = user;
    if (chunk_id >= s->metadata.count || !s->pkg_path[0])
        return false;
    if (s->pkg_loci && s->pkg_loci[chunk_id] != 0xff) {
        *locus = s->pkg_loci[chunk_id];
        return true;
    }
    const playgo_chunk *ch = &s->metadata.chunks[chunk_id];
    uint8_t value = 3;
    for (uint32_t i = 0; i < ch->nranges && value; i++) {
        const playgo_range *r = &s->metadata.ranges[ch->first_range + i];
        bool present = false;
        ps5c_err err = exfat_pkg_range_present(s->pkg_path, r->offset, r->length, &present);
        if (err) {
            LOG_W(HLE, "PlayGo: chunk %u range 0x%llx+0x%llx did not read from the package (%d)", chunk_id,
                  (unsigned long long)r->offset, (unsigned long long)r->length, (int)err);
            return false;
        }
        if (!present)
            value = 0;
    }
    if (!ch->nranges && ch->size)
        return false;
    if (s->pkg_loci)
        s->pkg_loci[chunk_id] = value;
    *locus = value;
    return true;
}

/* Hold the mapping lock across protection checks and copies: gmem_read/write
 * check mapping but do not check CPU permissions. Probe-only calls validate
 * every output before any output is changed. */
static bool transfer(hle_call *c, guest_addr addr, void *bytes, uint64_t size, bool write) {
    if (!size)
        return true;
    if (!addr || size > UINT64_MAX - addr)
        return false;
    gmem *m = c->mem;
    pal_mutex_lock(m->lock);
    uint64_t end = addr + size, cursor = addr;
    for (size_t i = 0; i < m->vmas.len && cursor < end; i++) {
        const gmem_vma *v = &m->vmas.data[i];
        if (v->start + v->size <= cursor)
            continue;
        if (v->start > cursor || v->reserved || !(v->prot & (write ? GPROT_WRITE : GPROT_READ)))
            break;
        cursor = PS5C_MIN(end, v->start + v->size);
    }
    bool ok = cursor == end;
    if (ok && bytes) {
        void *host = m->identity ? (void *)(uintptr_t)addr : m->host_base + addr;
        if (write)
            memcpy(host, bytes, (size_t)size);
        else
            memcpy(bytes, host, (size_t)size);
    }
    pal_mutex_unlock(m->lock);
    return ok;
}
static bool refresh_loci(hle_call *c) {
    struct playgo_state *s = c->rt->playgo;
    bool (*query)(void *, uint16_t, uint8_t *) = c->rt->opts.playgo_query_locus;
    void *user = c->rt->opts.playgo_query_user;
    if (!query && s->pkg_path[0])
        query = pkg_query_locus, user = s;
    for (uint32_t i = 0; i < s->metadata.count; i++) {
        uint8_t value = 3;
        if (s->metadata.chunks[i].size && (!query || !query(user, (uint16_t)i, &value)))
            return false;
        if (value != 0 && value != 2 && value != 3)
            return false;
        s->loci[i] = value;
    }
    return true;
}
static int32_t ready(hle_call *c) {
    struct playgo_state *s = c->rt->playgo;
    if (!s || !s->initialized)
        return PG_UNINITIALIZED;
    if (!s->handle || hle_arg_u32(c, 0) != s->handle)
        return PG_HANDLE;
    return 0;
}
static bool load_metadata(hle_call *c, playgo_metadata *out, char *pkg_path, size_t pkg_cap) {
    char path[1024];
    pkg_path[0] = 0;
    if (kernel_fs_resolve(c, "/app0/sce_sys/playgo-chunk.dat", path, sizeof(path)))
        return false;
    pal_file *f = NULL;
    exfat_file *xf = NULL;
    uint64_t size = 0, got = 0;
    bool virtual = exfat_path(path), ok = false;
    if (virtual) {
        if (exfat_open(path, PAL_FILE_READ, &xf) != PS5C_OK)
            return false;
        size = exfat_size(xf);
    } else {
        if (pal_file_open(path, PAL_FILE_READ, &f) != PS5C_OK)
            return false;
        if (pal_file_size(f, &size) != PS5C_OK)
            goto done;
    }
    if (size < 256 || size > PLAYGO_METADATA_MAX_SIZE)
        goto done;
    uint8_t *data = malloc((size_t)size);
    if (!data)
        goto done;
    ps5c_err err =
        virtual ? exfat_read(xf, data, size, 0, &got) : pal_file_read(f, data, size, &got);
    ok = err == PS5C_OK && got == size && playgo_metadata_parse(data, (size_t)size, out);
    free(data);
    bool in_pkg = false;
    if (ok && virtual && exfat_pkg_range_present(path, 0, 0, &in_pkg) == PS5C_OK)
        snprintf(pkg_path, pkg_cap, "%s", path);
done:
    if (xf)
        exfat_close(xf);
    if (f)
        pal_file_close(f);
    return ok;
}
/* A dump without sce_sys/playgo-chunk.dat at all (Wolverine's has only playgo-scenario.json):
 * the game is on the disk in full, so it is reported as chunks with nothing left to download,
 * the console's answer for an installed game. Without this, Open failed and the game took its
 * data as missing. PS5C_PLAYGO_STRICT=1: Open fails as before. */
#define PLAYGO_INSTALLED_CHUNKS 1000
static bool installed_without_metadata(hle_call *c, playgo_metadata *out) {
    char path[1024];
    pal_path_info info;
    if (getenv("PS5C_PLAYGO_STRICT") || kernel_fs_resolve(c, "/app0/sce_sys/playgo-chunk.dat", path, sizeof(path)) ||
        (exfat_path(path) ? exfat_stat(path, &info) == PS5C_OK : pal_path_stat(path, &info) == PS5C_OK))
        return false;
    out->chunks = calloc(PLAYGO_INSTALLED_CHUNKS, sizeof(*out->chunks));
    if (!out->chunks)
        return false;
    out->count = PLAYGO_INSTALLED_CHUNKS;
    for (uint32_t i = 0; i < out->count; i++) out->chunks[i].language_mask = ~0ull;
    LOG_I(HLE, "PlayGo: no playgo-chunk.dat; the game is reported installed (%u chunks)", out->count);
    return true;
}

static int32_t initialize(hle_call *c) {
    struct {
        uint64_t buffer;
        uint32_t size, reserved;
    } p;
    if (!transfer(c, hle_arg(c, 0), &p, sizeof(p), false))
        return PG_POINTER;
    if (p.reserved)
        return PG_ARGUMENT;
    if (p.size < (2u << 20))
        return PG_SIZE;
    if (!transfer(c, p.buffer, NULL, p.size, true))
        return PG_POINTER;
    struct playgo_state *s = c->rt->playgo;
    if (s && s->initialized)
        return PG_INITIALIZED;
    if (!s) {
        s = calloc(1, sizeof(*s));
        if (!s)
            return PG_NOMEM;
        c->rt->playgo = s;
    }
    s->initialized = true;
    return 0;
}
static int32_t open_playgo(hle_call *c) {
    struct playgo_state *s = c->rt->playgo;
    if (!s || !s->initialized)
        return PG_UNINITIALIZED;
    if (hle_arg(c, 1))
        return PG_ARGUMENT;
    guest_addr output = hle_arg(c, 0);
    if (!transfer(c, output, NULL, 4, true))
        return PG_POINTER;
    if (!s->handle) {
        if (!load_metadata(c, &s->metadata, s->pkg_path, sizeof(s->pkg_path)) &&
            !installed_without_metadata(c, &s->metadata))
            return PG_UNSUPPORTED;
        s->loci = malloc(s->metadata.count);
        s->pkg_loci = s->pkg_path[0] ? malloc(s->metadata.count) : NULL;
        if (!s->loci || (s->pkg_path[0] && !s->pkg_loci)) {
            free(s->loci);
            free(s->pkg_loci);
            s->loci = s->pkg_loci = NULL;
            playgo_metadata_free(&s->metadata);
            return PG_NOMEM;
        }
        if (s->pkg_loci)
            memset(s->pkg_loci, 0xff, s->metadata.count);
        if (!refresh_loci(c)) {
            LOG_I(HLE, "PlayGo: installation unknown; Open returns NOT_SUPPORT_PLAYGO");
            free(s->loci);
            free(s->pkg_loci);
            s->loci = s->pkg_loci = NULL;
            s->pkg_path[0] = 0;
            playgo_metadata_free(&s->metadata);
            return PG_UNSUPPORTED;
        }
        s->handle = ++s->next_handle;
        if (!s->handle)
            s->handle = ++s->next_handle;
        uint32_t local = 0;
        for (uint32_t i = 0; i < s->metadata.count; i++)
            local += s->loci[i] == 3;
        LOG_I(HLE, "PlayGo: PLGX loaded, %u chunks (%u local); installation backend: %s", s->metadata.count, local,
              c->rt->opts.playgo_query_locus ? "frontend" : s->pkg_path[0] ? "package reader" : "none needed");
    }
    return transfer(c, output, &s->handle, 4, true) ? 0 : PG_POINTER;
}
static int32_t get_chunks(hle_call *c) {
    int32_t err = ready(c);
    if (err)
        return err;
    struct playgo_state *s = c->rt->playgo;
    guest_addr ids = hle_arg(c, 1), output = hle_arg(c, 3);
    uint32_t capacity = hle_arg_u32(c, 2),
             count = ids ? PS5C_MIN(capacity, s->metadata.count) : s->metadata.count;
    if (ids && !capacity)
        return PG_SIZE;
    if (!transfer(c, output, NULL, 4, true) || (ids && !transfer(c, ids, NULL, count * 2ull, true)))
        return PG_POINTER;
    if (ids) {
        uint16_t *list = malloc(count * sizeof(*list));
        if (!list)
            return PG_NOMEM;
        for (uint32_t i = 0; i < count; i++)
            list[i] = (uint16_t)i;
        bool ok = transfer(c, ids, list, count * 2ull, true);
        free(list);
        if (!ok)
            return PG_POINTER;
    }
    return transfer(c, output, &count, 4, true) ? 0 : PG_POINTER;
}
static int32_t get_locus_or_progress(hle_call *c, bool progress) {
    int32_t err = ready(c);
    if (err)
        return err;
    struct playgo_state *s = c->rt->playgo;
    uint32_t count = hle_arg_u32(c, 2);
    if (!count || count > UINT16_MAX)
        return PG_SIZE;
    guest_addr ids = hle_arg(c, 1), output = hle_arg(c, 3);
    if (!transfer(c, ids, NULL, count * 2ull, false) ||
        !transfer(c, output, NULL, progress ? 16 : count, true))
        return PG_POINTER;
    if (!refresh_loci(c))
        return PG_UNSUPPORTED;
    uint16_t *list = malloc(count * sizeof(*list));
    uint8_t *loci = malloc(count);
    if (!list || !loci) {
        free(list);
        free(loci);
        return PG_NOMEM;
    }
    uint64_t sizes[2] = {0, 0};
    if (!transfer(c, ids, list, count * 2ull, false))
        err = PG_POINTER;
    for (uint32_t i = 0; !err && i < count; i++) {
        if (list[i] >= s->metadata.count) {
            err = PG_CHUNK;
            break;
        }
        playgo_chunk *chunk = &s->metadata.chunks[list[i]];
        loci[i] = s->loci[list[i]];
        if (sizes[1] > UINT64_MAX - chunk->size) {
            err = PG_SIZE;
            break;
        }
        sizes[1] += chunk->size;
        if (loci[i] >= 2)
            sizes[0] += chunk->size;
    }
    if (!err &&
        !transfer(c, output, progress ? (void *)sizes : (void *)loci, progress ? 16 : count, true))
        err = PG_POINTER;
    free(list);
    free(loci);
    return err;
}
static int32_t set_todo(hle_call *c) {
    int32_t err = ready(c);
    if (err)
        return err;
    struct playgo_state *s = c->rt->playgo;
    uint32_t count = hle_arg_u32(c, 2);
    if (count > UINT16_MAX)
        return PG_SIZE;
    if (!refresh_loci(c))
        return PG_UNSUPPORTED;
    playgo_todo *list = count ? malloc(count * sizeof(*list)) : NULL;
    if (count && !list)
        return PG_NOMEM;
    if (!transfer(c, hle_arg(c, 1), list, count * 4ull, false)) {
        free(list);
        return PG_POINTER;
    }
    uint32_t pending = 0;
    for (uint32_t i = 0; i < count; i++) {
        playgo_todo item = list[i];
        if (item.id >= s->metadata.count) {
            err = PG_CHUNK;
            break;
        }
        if (item.locus != 0 && item.locus != 2 && item.locus != 3) {
            err = PG_LOCUS;
            break;
        }
        if (item.reserved) {
            err = PG_ARGUMENT;
            break;
        }
        if (item.locus > s->loci[item.id])
            list[pending++] = item;
    }
    if (err) {
        free(list);
        return err;
    }
    free(s->todo);
    s->todo = list;
    s->todo_count = pending;
    return 0;
}
static int32_t get_todo(hle_call *c) {
    int32_t err = ready(c);
    if (err)
        return err;
    struct playgo_state *s = c->rt->playgo;
    if (!refresh_loci(c))
        return PG_UNSUPPORTED;
    uint32_t pending = 0;
    for (uint32_t i = 0; i < s->todo_count; i++) {
        if (s->todo[i].locus > s->loci[s->todo[i].id])
            s->todo[pending++] = s->todo[i];
    }
    s->todo_count = pending;
    guest_addr list = hle_arg(c, 1), output = hle_arg(c, 3);
    uint32_t capacity = hle_arg_u32(c, 2),
             count = list ? PS5C_MIN(capacity, s->todo_count) : s->todo_count;
    if (list && !capacity)
        return PG_SIZE;
    if (!transfer(c, output, NULL, 4, true) ||
        (list && !transfer(c, list, NULL, count * 4ull, true)))
        return PG_POINTER;
    if (list && !transfer(c, list, s->todo, count * 4ull, true))
        return PG_POINTER;
    return transfer(c, output, &count, 4, true) ? 0 : PG_POINTER;
}
static void close_state(struct playgo_state *s) {
    playgo_metadata_free(&s->metadata);
    free(s->loci);
    free(s->pkg_loci);
    s->loci = s->pkg_loci = NULL;
    s->pkg_path[0] = 0;
    free(s->todo);
    s->todo = NULL;
    s->todo_count = 0;
    s->handle = 0;
}
void hle_playgo_reset(ps5_runtime *rt) {
    if (rt->playgo) {
        close_state(rt->playgo);
        free(rt->playgo);
        rt->playgo = NULL;
    }
}
static int32_t close_playgo(hle_call *c) {
    int32_t err = ready(c);
    if (err)
        return err;
    close_state(c->rt->playgo);
    return 0;
}
static int32_t terminate(hle_call *c) {
    struct playgo_state *s = c->rt->playgo;
    if (!s || !s->initialized)
        return PG_UNINITIALIZED;
    close_state(s);
    s->initialized = false;
    return 0;
}
/* (handle, uint64 *mask): every language */
static int32_t get_language_mask(hle_call *c) {
    int32_t err = ready(c);
    uint64_t all = ~0ull;
    if (err)
        return err;
    return transfer(c, hle_arg(c, 1), &all, 8, true) ? 0 : PG_POINTER;
}
/* (handle, type, uint64 *mask): 0 languages (all), 1 scenarios (as KytyPS5: 0x1f) */
static int32_t get_supported_optional_chunk(hle_call *c) {
    int32_t err = ready(c);
    if (err)
        return err;
    uint32_t type = hle_arg_u32(c, 1);
    uint64_t mask = type == 0 ? ~0ull : 0x1f;
    if (type > 1)
        return PG_ARGUMENT;
    return transfer(c, hle_arg(c, 2), &mask, 8, true) ? 0 : PG_POINTER;
}
/* (handle, const uint16 *ids, count, minimum locus): everything is local already */
static int32_t prefetch(hle_call *c) {
    int32_t err = ready(c);
    if (err)
        return err;
    uint32_t count = hle_arg_u32(c, 2);
    uint8_t locus = (uint8_t)hle_arg(c, 3);
    if (!count || count > UINT16_MAX)
        return PG_SIZE;
    if (locus != 0 && locus != 2 && locus != 3)
        return PG_LOCUS;
    return transfer(c, hle_arg(c, 1), NULL, count * 2ull, false) ? 0 : PG_POINTER;
}
/* (handle, int32 *speed): the install is done, the speed stays at its default (2, "trickle") */
static int32_t get_install_speed(hle_call *c) {
    int32_t err = ready(c);
    if (err)
        return err;
    int32_t speed = 2;
    return transfer(c, hle_arg(c, 1), &speed, 4, true) ? 0 : PG_POINTER;
}
/* (handle, const uint16 *ids, count, int64 *eta): nothing left to install */
static int32_t get_eta(hle_call *c) {
    int32_t err = ready(c);
    if (err)
        return err;
    int64_t eta = 0;
    return transfer(c, hle_arg(c, 3), &eta, 8, true) ? 0 : PG_POINTER;
}
static int32_t handle_only(hle_call *c) { return ready(c); }
#define WRAP(name, impl)                                                                           \
    static void name(hle_call *c) {                                                                \
        if (!c->rt || !c->rt->threads_lock || !c->mem) {                                           \
            hle_ret_s32(c, PG_ARGUMENT);                                                           \
            return;                                                                                \
        }                                                                                          \
        pal_mutex_lock(c->rt->threads_lock);                                                       \
        int32_t result = (impl);                                                                   \
        pal_mutex_unlock(c->rt->threads_lock);                                                     \
        hle_ret_s32(c, result);                                                                    \
    }
WRAP(pg_initialize, initialize(c))
WRAP(pg_open, open_playgo(c))
WRAP(pg_chunks, get_chunks(c))
WRAP(pg_locus, get_locus_or_progress(c, false))
WRAP(pg_progress, get_locus_or_progress(c, true))
WRAP(pg_set_todo, set_todo(c))
WRAP(pg_get_todo, get_todo(c))
WRAP(pg_close, close_playgo(c))
WRAP(pg_terminate, terminate(c))
WRAP(pg_language_mask, get_language_mask(c))
WRAP(pg_optional_chunk, get_supported_optional_chunk(c))
WRAP(pg_prefetch, prefetch(c))
WRAP(pg_install_speed, get_install_speed(c))
WRAP(pg_eta, get_eta(c))
WRAP(pg_handle_only, handle_only(c))
void hle_register_playgo(void) {
    static const hle_export exports[] = {
        HLE_FUNC_ENTRY("libScePlayGo", "scePlayGoInitialize", pg_initialize),
        HLE_FUNC_ENTRY("libScePlayGo", "scePlayGoOpen", pg_open),
        HLE_FUNC_ENTRY("libScePlayGo", "scePlayGoGetChunkId", pg_chunks),
        HLE_FUNC_ENTRY("libScePlayGo", "scePlayGoGetInstallChunkId", pg_chunks),
        HLE_FUNC_ENTRY("libScePlayGo", "scePlayGoGetLanguageMask", pg_language_mask),
        HLE_FUNC_ENTRY("libScePlayGo", "scePlayGoGetSupportedOptionalChunk", pg_optional_chunk),
        HLE_FUNC_ENTRY("libScePlayGo", "scePlayGoPrefetch", pg_prefetch),
        HLE_FUNC_ENTRY("libScePlayGo", "scePlayGoGetLocus", pg_locus),
        HLE_FUNC_ENTRY("libScePlayGo", "scePlayGoGetProgress", pg_progress),
        HLE_FUNC_ENTRY("libScePlayGo", "scePlayGoSetToDoList", pg_set_todo),
        HLE_FUNC_ENTRY("libScePlayGo", "scePlayGoGetToDoList", pg_get_todo),
        HLE_FUNC_ENTRY("libScePlayGo", "scePlayGoClose", pg_close),
        HLE_FUNC_ENTRY("libScePlayGo", "scePlayGoTerminate", pg_terminate),
        HLE_FUNC_ENTRY("libScePlayGo", "scePlayGoGetInstallSpeed", pg_install_speed),
        HLE_FUNC_ENTRY("libScePlayGo", "scePlayGoSetInstallSpeed", pg_handle_only),
        HLE_FUNC_ENTRY("libScePlayGo", "scePlayGoGetEta", pg_eta),
        HLE_FUNC_ENTRY("libScePlayGo", "scePlayGoGetOptionalChunk", pg_optional_chunk),
        HLE_FUNC_ENTRY("libScePlayGo", "scePlayGoPrefetchOptionalChunk", pg_handle_only),
    };
    hle_register_table(exports, PS5C_ARRAY_COUNT(exports));
}
