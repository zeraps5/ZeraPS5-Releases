/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceNpTrophy2 and libSceNpUniversalDataSystem: trophies and activity
 * events. Nothing is unlocked or posted: contexts and handles are ids the game
 * gets back (left unwritten, GTA V registered context -1), and trophy queries
 * report no trophies and none unlocked. Activity events go nowhere. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "core/settings.h"
#include "gpu/renderer.h"
#include "hle/hle.h"
#include "hle/video_out.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

static volatile int32_t g_next_id = 1;

/* Writes a new id through argument 0 (int32_t *out) */
static void create_id(hle_call *c) {
    int32_t *out = (int32_t *)hle_mem(c, hle_arg_ptr(c, 0), 4);
    if (!out) {
        hle_ret_s32(c, (int32_t)0x80551604); /* invalid argument */
        return;
    }
    *out = __atomic_fetch_add(&g_next_id, 1, __ATOMIC_SEQ_CST);
    hle_ret_s32(c, 0);
}

static void np_ok(hle_call *c) { hle_ret_s32(c, 0); }

/* Trophy2 structures have no size field (Trophy v1's do): GameDetails 152 bytes
 * {u32 groups, trophies, platinum, gold, silver, bronze; char title[128]}, GameData 24,
 * GroupDetails 152 and GroupData 32 (both start with the group id), TrophyDetails 1312 and
 * TrophyData 32 (both start with the trophy id). Left as the game's stack had them, the
 * trophy count of GameDetails made Tetris Effect grow an array of 0x520-byte records to
 * 11 GiB. */
static void zero_out(hle_call *c, uint32_t arg, uint32_t size, bool id, int32_t value) {
    guest_addr p = hle_arg_ptr(c, arg);
    uint8_t *d = p ? (uint8_t *)hle_mem(c, p, size) : NULL;
    if (!d) return;
    memset(d, 0, size);
    if (id) memcpy(d, &value, 4);
}

/* int sceNpTrophy2GetGameInfo(context, handle, details, data): no trophies */
static void trophy_get_game_info(hle_call *c) {
    zero_out(c, 2, 152, false, 0);
    zero_out(c, 3, 24, false, 0);
    hle_ret_s32(c, 0);
}

/* int sceNpTrophy2GetTrophyInfo(context, handle, trophyId, details, data) */
static void trophy_get_trophy_info(hle_call *c) {
    int32_t id = hle_arg_s32(c, 2);
    zero_out(c, 3, 1312, true, id);
    zero_out(c, 4, 32, true, id);
    hle_ret_s32(c, 0);
}

/* int sceNpTrophy2GetGroupInfo(context, handle, groupId, details, data) */
static void trophy_get_group_info(hle_call *c) {
    int32_t id = hle_arg_s32(c, 2);
    zero_out(c, 3, 152, true, id);
    zero_out(c, 4, 32, true, id);
    hle_ret_s32(c, 0);
}

/* int sceNpTrophy2GetTrophyInfoArray and sceNpTrophy2GetGroupInfoArray(context, handle,
 * u32 offset, u32 limit, details[], data[], u32 *count): none */
static void trophy_get_trophy_info_array(hle_call *c) {
    uint32_t *count = (uint32_t *)hle_mem(c, hle_arg_ptr(c, 6), 4);
    if (!count) {
        hle_ret_s32(c, (int32_t)0x80551604);
        return;
    }
    *count = 0;
    hle_ret_s32(c, 0);
}

/* Activity events are built and posted to nowhere. Events and their property
 * objects and arrays are opaque: the game gets distinct non-null addresses in
 * a guest page nobody reads. */
#define UDS_ERR_INVALID_ARGUMENT ((int32_t)0x80553002)
static guest_addr g_uds_page;
static volatile uint32_t g_uds_next;

static bool uds_new_object(hle_call *c, guest_addr out) {
    if (!out) return false;
    if (!__atomic_load_n(&g_uds_page, __ATOMIC_ACQUIRE)) {
        guest_addr page = 0;
        if (gmem_map(c->mem, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "np activity objects", &page) != PS5C_OK) return false;
        guest_addr none = 0;
        if (!__atomic_compare_exchange_n(&g_uds_page, &none, page, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            gmem_unmap(c->mem, page, GUEST_PAGE_SIZE);
    }
    uint32_t slot = __atomic_fetch_add(&g_uds_next, 1, __ATOMIC_RELAXED) % (GUEST_PAGE_SIZE / 64);
    uint64_t obj = g_uds_page + (guest_addr)slot * 64;
    return gmem_write(c->mem, out, &obj, 8);
}

/* What trophy unlocks need of the objects: an event's name and property object, and a
 * property object's "_trophy_id" (PS5 games unlock with an "_UnlockTrophy" event). */
#define UDS_SLOTS (GUEST_PAGE_SIZE / 64)
static struct {
    char name[32];
    guest_addr prop;
    int64_t trophy_id; /* -1: not set */
} g_uds_obj[UDS_SLOTS];
static pal_mutex *g_uds_lock;

static int32_t uds_slot(guest_addr obj) {
    guest_addr page = __atomic_load_n(&g_uds_page, __ATOMIC_ACQUIRE);
    return page && obj >= page && obj < page + GUEST_PAGE_SIZE && !((obj - page) % 64) ? (int32_t)((obj - page) / 64) : -1;
}

static void uds_lock(void) {
    if (!g_uds_lock) {
        pal_mutex *m = NULL;
        if (pal_mutex_create(&m) != PS5C_OK) return;
        pal_mutex *none = NULL;
        if (!__atomic_compare_exchange_n(&g_uds_lock, &none, m, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) pal_mutex_destroy(m);
    }
    pal_mutex_lock(g_uds_lock);
}
static void uds_unlock(void) { pal_mutex_unlock(g_uds_lock); }

/* a fresh object: its slot's records are cleared */
static void uds_reset(hle_call *c, guest_addr out) {
    uint64_t obj = 0;
    int32_t slot = out && gmem_read_u64(c->mem, out, &obj) ? uds_slot(obj) : -1;
    if (slot < 0) return;
    uds_lock();
    memset(&g_uds_obj[slot], 0, sizeof(g_uds_obj[slot]));
    g_uds_obj[slot].trophy_id = -1;
    uds_unlock();
}

/* int sceNpUniversalDataSystemCreateEvent(const char *name, const object *prop, event **out,
 * object **prop_out) */
static void uds_create_event(hle_call *c) {
    guest_addr prop = hle_arg_ptr(c, 1), prop_out = hle_arg_ptr(c, 3), out = hle_arg_ptr(c, 2);
    char name[32];
    if (!hle_arg_ptr(c, 0) || !hle_read_str(c, hle_arg_ptr(c, 0), name, sizeof(name)) || !uds_new_object(c, out)) {
        hle_ret_s32(c, UDS_ERR_INVALID_ARGUMENT);
        return;
    }
    uds_reset(c, out);
    if (prop_out && prop) gmem_write(c->mem, prop_out, &prop, 8);
    else if (prop_out && uds_new_object(c, prop_out)) uds_reset(c, prop_out);
    uint64_t ev = 0, pr = prop;
    if (!pr && prop_out) gmem_read_u64(c->mem, prop_out, &pr);
    int32_t slot = gmem_read_u64(c->mem, out, &ev) ? uds_slot(ev) : -1;
    if (slot >= 0) {
        uds_lock();
        snprintf(g_uds_obj[slot].name, sizeof(g_uds_obj[slot].name), "%s", name);
        g_uds_obj[slot].prop = pr;
        uds_unlock();
    }
    hle_ret_s32(c, 0);
}

/* int sceNpUniversalDataSystemEventPropertyObjectSet(U)Int32/64(object *obj, const char *key, value) */
static void uds_set_integer(hle_call *c) {
    char key[32];
    int32_t slot = uds_slot(hle_arg_ptr(c, 0));
    if (slot >= 0 && hle_arg_ptr(c, 1) && hle_read_str(c, hle_arg_ptr(c, 1), key, sizeof(key)) && !strcmp(key, "_trophy_id")) {
        uds_lock();
        g_uds_obj[slot].trophy_id = (int64_t)(int32_t)hle_arg_u32(c, 2);
        uds_unlock();
    }
    hle_ret_s32(c, 0);
}

/* A trophy the game unlocked: logged, kept in user/<game>/trophies.txt, shown on screen */
static void trophy_unlocked(hle_call *c, int64_t id) {
    LOG_I(HLE, "trophy %lld unlocked", (long long)id);
    char path[600];
    snprintf(path, sizeof(path), "%s/trophies.txt", hle_user_dir(c->rt));
    uint8_t *data = NULL;
    uint64_t size = 0;
    char line[32];
    snprintf(line, sizeof(line), "%lld\n", (long long)id);
    bool known = false;
    if (pal_file_read_all(path, &data, &size) == PS5C_OK) {
        for (char *p = (char *)data, *end = p + size; p < end && !known;) {
            char *eol = memchr(p, '\n', (size_t)(end - p));
            size_t n = eol ? (size_t)(eol - p) + 1 : (size_t)(end - p);
            known = n == strlen(line) && !memcmp(p, line, n);
            p += n;
        }
        free(data);
    }
    if (!known) {
        FILE *f = fopen(path, "ab");
        if (f) {
            fputs(line, f);
            fclose(f);
        }
    }
    renderer *r = video_out_renderer();
    if (r) {
        char text[48];
        snprintf(text, sizeof(text), "TROFEU DESBLOQUEADO (#%lld)", (long long)id);
        int secs = settings_get_int("geral", "trofeu_segundos", 6);
        renderer_notify(r, text, (uint32_t)(secs < 1 ? 1 : secs > 30 ? 30 : secs) * 1000u);
    }
}

/* int sceNpUniversalDataSystemPostEvent(context, handle, const event *ev, options) */
static void uds_post_event(hle_call *c) {
    int32_t slot = uds_slot(hle_arg_ptr(c, 2));
    int64_t id = -1;
    if (slot >= 0) {
        uds_lock();
        if (!strcmp(g_uds_obj[slot].name, "_UnlockTrophy")) {
            int32_t ps = uds_slot(g_uds_obj[slot].prop);
            if (ps >= 0) id = g_uds_obj[ps].trophy_id;
        }
        uds_unlock();
    }
    if (id >= 0) trophy_unlocked(c, id);
    hle_ret_s32(c, 0);
}

/* int sceNpUniversalDataSystemCreateEventPropertyObject/Array(T **out) */
static void uds_create_property(hle_call *c) {
    bool ok = uds_new_object(c, hle_arg_ptr(c, 0));
    if (ok) uds_reset(c, hle_arg_ptr(c, 0));
    hle_ret_s32(c, ok ? 0 : UDS_ERR_INVALID_ARGUMENT);
}

/* int sceNpUniversalDataSystemGetMemoryStat({size_t pool, max_inuse, inuse} *stat) */
static void uds_get_memory_stat(hle_call *c) {
    uint8_t *stat = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 24);
    if (stat) memset(stat, 0, 24);
    hle_ret_s32(c, stat ? 0 : UDS_ERR_INVALID_ARGUMENT);
}

/* int sceNpUniversalDataSystemEventEstimateSize(const event *ev, size_t *size) */
static void uds_event_estimate_size(hle_call *c) {
    uint64_t *size = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 1), 8);
    if (size) *size = 256;
    hle_ret_s32(c, size ? 0 : UDS_ERR_INVALID_ARGUMENT);
}

/* int sceNpUniversalDataSystemEventToString(const event *ev, char *buf, size_t size, size_t *length):
 * the events go nowhere, an empty JSON object stands for them */
static void uds_event_to_string(hle_call *c) {
    uint64_t cap = hle_arg(c, 2);
    char *buf = cap >= 3 ? (char *)hle_mem(c, hle_arg_ptr(c, 1), 3) : NULL;
    uint64_t *length = hle_arg_ptr(c, 3) ? (uint64_t *)hle_mem(c, hle_arg_ptr(c, 3), 8) : NULL;
    if (buf) memcpy(buf, "{}", 3);
    if (length) *length = 2;
    hle_ret_s32(c, buf || length ? 0 : UDS_ERR_INVALID_ARGUMENT);
}

/* PS4 int sceNpTrophyUnlockTrophy(ctx, handle, trophyId, int *platinumId): nothing is
 * unlocked, so no platinum either (SCE_NP_TROPHY_INVALID_TROPHY_ID) */
static void trophy_unlock_ps4(hle_call *c) {
    int32_t *plat = (int32_t *)hle_mem(c, hle_arg_ptr(c, 3), 4);
    if (plat) *plat = -1;
    hle_ret_s32(c, 0);
}

/* PS4 int sceNpTrophyGetTrophyUnlockState(ctx, handle, SceNpTrophyFlagArray *flags, u32 *count):
 * 128 bits (SCE_NP_TROPHY_FLAG_SETSIZE) in four 32-bit words, 16 bytes; none unlocked.
 * Clearing 128 bytes ran past LEGO City Undercover's array on the TrophyInstall
 * stack, and its stack check ended the game at 19 s. */
static void trophy_unlock_state_ps4(hle_call *c) {
    uint8_t *flags = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 2), 16);
    uint32_t *count = (uint32_t *)hle_mem(c, hle_arg_ptr(c, 3), 4);
    if (flags) memset(flags, 0, 16);
    if (count) *count = 0;
    hle_ret_s32(c, flags ? 0 : (int32_t)0x80551604);
}

/* PS4 structures start with a size the game fills in (kept); the rest reads as no trophies
 * unlocked, with the asked id in its field: GameDetails 1184 bytes, GameData 32, GroupDetails
 * 1184 (groupId at 8), GroupData 40 (groupId at 8), Details 1176 (trophyId at 8), Data 24 (trophyId at 8). */
static void sized_out(hle_call *c, uint32_t arg, uint32_t size, bool id, int32_t value) {
    guest_addr p = hle_arg_ptr(c, arg);
    uint8_t *d = p ? (uint8_t *)hle_mem(c, p, size) : NULL;
    if (!d) return;
    memset(d + 8, 0, size - 8);
    if (id) memcpy(d + 8, &value, 4);
}

static void trophy_get_game_info_ps4(hle_call *c) {
    sized_out(c, 2, 1184, false, 0);
    sized_out(c, 3, 32, false, 0);
    hle_ret_s32(c, 0);
}

static void trophy_get_group_info_ps4(hle_call *c) {
    int32_t id = hle_arg_s32(c, 2);
    sized_out(c, 3, 1184, true, id);
    sized_out(c, 4, 40, true, id);
    hle_ret_s32(c, 0);
}

static void trophy_get_trophy_info_ps4(hle_call *c) {
    int32_t id = hle_arg_s32(c, 2);
    sized_out(c, 3, 1176, true, id);
    sized_out(c, 4, 24, true, id);
    hle_ret_s32(c, 0);
}

#define T "libSceNpTrophy2"
#define T1 "libSceNpTrophy" /* PS4 games */
#define U "libSceNpUniversalDataSystem"

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(T1, "sceNpTrophyCreateContext", create_id),
    HLE_FUNC_ENTRY(T1, "sceNpTrophyCreateHandle", create_id),
    HLE_FUNC_ENTRY(T1, "sceNpTrophyRegisterContext", np_ok),
    HLE_FUNC_ENTRY(T1, "sceNpTrophyAbortHandle", np_ok),
    HLE_FUNC_ENTRY(T1, "sceNpTrophyDestroyHandle", np_ok),
    HLE_FUNC_ENTRY(T1, "sceNpTrophyDestroyContext", np_ok),
    HLE_FUNC_ENTRY(T1, "sceNpTrophyUnlockTrophy", trophy_unlock_ps4),
    HLE_FUNC_ENTRY(T1, "sceNpTrophyGetTrophyUnlockState", trophy_unlock_state_ps4),
    HLE_FUNC_ENTRY(T1, "sceNpTrophyGetGameInfo", trophy_get_game_info_ps4),
    HLE_FUNC_ENTRY(T1, "sceNpTrophyGetGroupInfo", trophy_get_group_info_ps4),
    HLE_FUNC_ENTRY(T1, "sceNpTrophyGetTrophyInfo", trophy_get_trophy_info_ps4),
    HLE_FUNC_ENTRY(T, "sceNpTrophy2CreateContext", create_id),
    HLE_FUNC_ENTRY(T, "sceNpTrophy2CreateHandle", create_id),
    HLE_FUNC_ENTRY(T, "sceNpTrophy2RegisterContext", np_ok),
    HLE_FUNC_ENTRY(T, "sceNpTrophy2GetGameInfo", trophy_get_game_info),
    HLE_FUNC_ENTRY(T, "sceNpTrophy2GetTrophyInfo", trophy_get_trophy_info),
    HLE_FUNC_ENTRY(T, "sceNpTrophy2AbortHandle", np_ok),
    HLE_FUNC_ENTRY(T, "sceNpTrophy2DestroyHandle", np_ok),
    HLE_FUNC_ENTRY(T, "sceNpTrophy2DestroyContext", np_ok),
    HLE_FUNC_ENTRY(T, "sceNpTrophy2GetTrophyInfoArray", trophy_get_trophy_info_array),
    HLE_FUNC_ENTRY(T, "sceNpTrophy2GetGroupInfoArray", trophy_get_trophy_info_array),
    HLE_FUNC_ENTRY(T, "sceNpTrophy2GetGroupInfo", trophy_get_group_info),
    HLE_FUNC_ENTRY(T, "sceNpTrophy2RegisterUnlockCallback", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemInitialize", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemTerminate", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemCreateContext", create_id),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemCreateHandle", create_id),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemRegisterContext", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemAbortHandle", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemDestroyHandle", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemDestroyContext", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemCreateEvent", uds_create_event),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemCreateEventPropertyObject", uds_create_property),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemCreateEventPropertyArray", uds_create_property),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemGetMemoryStat", uds_get_memory_stat),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemDestroyEvent", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemPostEvent", uds_post_event),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventEstimateSize", uds_event_estimate_size),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventToString", uds_event_to_string),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemDestroyEventPropertyObject", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemDestroyEventPropertyArray", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyObjectSetString", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyObjectSetInt32", uds_set_integer),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyObjectSetUInt32", uds_set_integer),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyObjectSetInt64", uds_set_integer),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyObjectSetUInt64", uds_set_integer),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyObjectSetFloat32", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyObjectSetFloat64", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyObjectSetBool", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyObjectSetObject", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyObjectSetArray", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyObjectSetBinary", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyArraySetString", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyArraySetInt32", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyArraySetUInt32", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyArraySetInt64", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyArraySetUInt64", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyArraySetFloat32", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyArraySetFloat64", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyArraySetBool", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyArraySetObject", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyArraySetArray", np_ok),
    HLE_FUNC_ENTRY(U, "sceNpUniversalDataSystemEventPropertyArraySetBinary", np_ok),
};

void hle_register_np_trophy(void) { hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports)); }
