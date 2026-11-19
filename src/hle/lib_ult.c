/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceUlt: the synchronization objects of user-level threads, used here from
 * ordinary threads. No Man's Sky's job system takes its ULT mutexes thousands
 * of times a second; with the calls left unimplemented nothing excluded
 * anything, and its PlayFab client later locked a mutex that was not one.
 *
 * SceUltMutex and SceUltSemaphore are opaque guest structures (256 bytes): the
 * first 8 bytes hold the host object, the next 8 a tag, and every live object
 * is in a registry, so a garbage structure is refused instead of dereferenced.
 * Waiting queue resource pools only size the work area: the host objects wait
 * on their own. */
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/hle.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

#define ULT_ERROR_NULL ((int32_t)0x80810001)
#define ULT_ERROR_INVALID ((int32_t)0x80810004)
#define ULT_ERROR_BUSY ((int32_t)0x80810007)
#define ULT_ERROR_PERMISSION ((int32_t)0x80810005)
#define ULT_TAG_MUTEX 0x58544d544c55ull /* "ULTMTX" */
#define ULT_TAG_SEMA 0x414d53544c55ull  /* "ULTSMA" */

typedef struct ult_obj {
    uint64_t tag;
    pal_mutex *lock;
    pal_cond *changed;
    int64_t count;  /* semaphore: resources; mutex: 1 when held */
    uint64_t owner; /* mutex holder */
} ult_obj;

#define MAX_ULT_OBJECTS 1024
static ult_obj *g_objs[MAX_ULT_OBJECTS];
static volatile int32_t g_reg_lock;
static void reg_lock(void) { while (__atomic_test_and_set(&g_reg_lock, __ATOMIC_ACQUIRE)) pal_sleep_ns(0); }
static void reg_unlock(void) { __atomic_clear(&g_reg_lock, __ATOMIC_RELEASE); }

static uint64_t self_id(hle_call *c) {
    return c->thread ? (uint64_t)c->thread->id : 0x100000000ull + pal_thread_current_id();
}

static int32_t create(hle_call *c, guest_addr obj, uint64_t tag, int64_t count) {
    if (!obj) return ULT_ERROR_NULL;
    ult_obj *o = (ult_obj *)calloc(1, sizeof(*o));
    if (!o || pal_mutex_create(&o->lock) != PS5C_OK || pal_cond_create(&o->changed) != PS5C_OK) {
        free(o);
        return ULT_ERROR_INVALID;
    }
    o->tag = tag, o->count = count;
    reg_lock();
    uint32_t i = 0;
    while (i < MAX_ULT_OBJECTS && g_objs[i]) i++;
    if (i < MAX_ULT_OBJECTS) g_objs[i] = o;
    reg_unlock();
    uint64_t words[2] = {(uint64_t)(uintptr_t)o, tag};
    if (i == MAX_ULT_OBJECTS || !gmem_write(c->mem, obj, words, sizeof(words))) {
        reg_lock();
        if (i < MAX_ULT_OBJECTS) g_objs[i] = NULL;
        reg_unlock();
        pal_cond_destroy(o->changed);
        pal_mutex_destroy(o->lock);
        free(o);
        return ULT_ERROR_INVALID;
    }
    return 0;
}

static ult_obj *find(hle_call *c, guest_addr obj, uint64_t tag) {
    uint64_t words[2];
    if (!obj || !gmem_read(c->mem, obj, words, sizeof(words)) || words[1] != tag) return NULL;
    ult_obj *o = (ult_obj *)(uintptr_t)words[0], *found = NULL;
    reg_lock();
    for (uint32_t i = 0; i < MAX_ULT_OBJECTS && !found; i++)
        if (g_objs[i] == o && o->tag == tag) found = o;
    reg_unlock();
    return found;
}

static int32_t destroy(hle_call *c, guest_addr obj, uint64_t tag) {
    ult_obj *o = find(c, obj, tag);
    if (!o) return ULT_ERROR_INVALID;
    reg_lock();
    for (uint32_t i = 0; i < MAX_ULT_OBJECTS; i++)
        if (g_objs[i] == o) g_objs[i] = NULL;
    reg_unlock();
    uint64_t zero[2] = {0, 0};
    gmem_write(c->mem, obj, zero, sizeof(zero));
    pal_cond_destroy(o->changed);
    pal_mutex_destroy(o->lock);
    free(o);
    return 0;
}

static void ok(hle_call *c) { hle_ret_s32(c, 0); }

/* size_t sceUltWaitingQueueResourcePoolGetWorkAreaSize(uint32_t numThreads, uint32_t numSyncObjects) */
static void pool_work_area_size(hle_call *c) {
    hle_ret_u64(c, 64ull * ((uint64_t)hle_arg_u32(c, 0) + hle_arg_u32(c, 1)) + 64);
}

/* int _sceUltMutexCreate(SceUltMutex *mutex, const char *name, pool *, const SceUltMutexOptParam *) */
static void mutex_create(hle_call *c) { hle_ret_s32(c, create(c, hle_arg_ptr(c, 0), ULT_TAG_MUTEX, 0)); }
static void mutex_destroy(hle_call *c) { hle_ret_s32(c, destroy(c, hle_arg_ptr(c, 0), ULT_TAG_MUTEX)); }

static void mutex_lock_common(hle_call *c, bool try_only) {
    ult_obj *o = find(c, hle_arg_ptr(c, 0), ULT_TAG_MUTEX);
    if (!o) {
        hle_ret_s32(c, ULT_ERROR_INVALID);
        return;
    }
    uint64_t me = self_id(c);
    pal_mutex_lock(o->lock);
    if (o->count && o->owner == me) { /* not recursive */
        pal_mutex_unlock(o->lock);
        hle_ret_s32(c, ULT_ERROR_BUSY);
        return;
    }
    while (o->count) {
        if (try_only) {
            pal_mutex_unlock(o->lock);
            hle_ret_s32(c, ULT_ERROR_BUSY);
            return;
        }
        pal_cond_wait(o->changed, o->lock);
    }
    o->count = 1, o->owner = me;
    pal_mutex_unlock(o->lock);
    hle_ret_s32(c, 0);
}
static void mutex_lock(hle_call *c) { mutex_lock_common(c, false); }
static void mutex_trylock(hle_call *c) { mutex_lock_common(c, true); }

static void mutex_unlock(hle_call *c) {
    ult_obj *o = find(c, hle_arg_ptr(c, 0), ULT_TAG_MUTEX);
    if (!o) {
        hle_ret_s32(c, ULT_ERROR_INVALID);
        return;
    }
    pal_mutex_lock(o->lock);
    int32_t r = 0;
    if (!o->count || o->owner != self_id(c)) r = ULT_ERROR_PERMISSION;
    else o->count = 0, o->owner = 0, pal_cond_signal(o->changed);
    pal_mutex_unlock(o->lock);
    hle_ret_s32(c, r);
}

/* int _sceUltSemaphoreCreate(SceUltSemaphore *sem, const char *name, int32_t numInitialResource,
 *                            pool *, const SceUltSemaphoreOptParam *) */
static void sema_create(hle_call *c) {
    int32_t initial = hle_arg_s32(c, 2);
    hle_ret_s32(c, initial < 0 ? ULT_ERROR_INVALID : create(c, hle_arg_ptr(c, 0), ULT_TAG_SEMA, initial));
}
static void sema_destroy(hle_call *c) { hle_ret_s32(c, destroy(c, hle_arg_ptr(c, 0), ULT_TAG_SEMA)); }

static void sema_acquire_common(hle_call *c, bool try_only) {
    ult_obj *o = find(c, hle_arg_ptr(c, 0), ULT_TAG_SEMA);
    int32_t n = hle_arg_s32(c, 1);
    if (!o || n <= 0) {
        hle_ret_s32(c, ULT_ERROR_INVALID);
        return;
    }
    pal_mutex_lock(o->lock);
    while (o->count < n) {
        if (try_only) {
            pal_mutex_unlock(o->lock);
            hle_ret_s32(c, ULT_ERROR_BUSY);
            return;
        }
        pal_cond_wait(o->changed, o->lock);
    }
    o->count -= n;
    pal_mutex_unlock(o->lock);
    hle_ret_s32(c, 0);
}
static void sema_acquire(hle_call *c) { sema_acquire_common(c, false); }
static void sema_tryacquire(hle_call *c) { sema_acquire_common(c, true); }

static void sema_release(hle_call *c) {
    ult_obj *o = find(c, hle_arg_ptr(c, 0), ULT_TAG_SEMA);
    int32_t n = hle_arg_s32(c, 1);
    if (!o || n <= 0) {
        hle_ret_s32(c, ULT_ERROR_INVALID);
        return;
    }
    pal_mutex_lock(o->lock);
    o->count += n;
    pal_cond_broadcast(o->changed);
    pal_mutex_unlock(o->lock);
    hle_ret_s32(c, 0);
}

#define U "libSceUlt"
static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(U, "sceUltInitialize", ok),
    HLE_FUNC_ENTRY(U, "sceUltFinalize", ok),
    HLE_FUNC_ENTRY(U, "sceUltWaitingQueueResourcePoolGetWorkAreaSize", pool_work_area_size),
    HLE_FUNC_ENTRY(U, "_sceUltWaitingQueueResourcePoolCreate", ok),
    HLE_FUNC_ENTRY(U, "sceUltWaitingQueueResourcePoolDestroy", ok),
    HLE_FUNC_ENTRY(U, "_sceUltWaitingQueueResourcePoolOptParamInitialize", ok),
    HLE_FUNC_ENTRY(U, "_sceUltMutexOptParamInitialize", ok),
    HLE_FUNC_ENTRY(U, "_sceUltMutexCreate", mutex_create),
    HLE_FUNC_ENTRY(U, "sceUltMutexDestroy", mutex_destroy),
    HLE_FUNC_ENTRY(U, "sceUltMutexLock", mutex_lock),
    HLE_FUNC_ENTRY(U, "sceUltMutexTryLock", mutex_trylock),
    HLE_FUNC_ENTRY(U, "sceUltMutexUnlock", mutex_unlock),
    HLE_FUNC_ENTRY(U, "_sceUltSemaphoreOptParamInitialize", ok),
    HLE_FUNC_ENTRY(U, "_sceUltSemaphoreCreate", sema_create),
    HLE_FUNC_ENTRY(U, "sceUltSemaphoreDestroy", sema_destroy),
    HLE_FUNC_ENTRY(U, "sceUltSemaphoreAcquire", sema_acquire),
    HLE_FUNC_ENTRY(U, "sceUltSemaphoreTryAcquire", sema_tryacquire),
    HLE_FUNC_ENTRY(U, "sceUltSemaphoreRelease", sema_release),
};

void hle_register_ult(void) { hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports)); }
