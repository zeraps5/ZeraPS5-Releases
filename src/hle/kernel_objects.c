/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hle/kernel_objects.h"

#include <stdlib.h>

#include "base/hashmap.h"
#include "pal/pal.h"

static pal_mutex *g_lock;
static hmap64 g_objects; /* host pointer -> obj_kind */

bool kobj_init(void) {
    if (g_lock) return true;
    if (pal_mutex_create(&g_lock) != PS5C_OK) return false;
    hmap64_init(&g_objects);
    return true;
}

void kobj_lock(void) { pal_mutex_lock(g_lock); }
void kobj_unlock(void) { pal_mutex_unlock(g_lock); }

void *kobj_new(obj_kind kind, size_t size) {
    obj_kind *o = (obj_kind *)calloc(1, size);
    if (!o) return NULL;
    *o = kind;
    kobj_lock();
    bool ok = hmap64_put(&g_objects, (uint64_t)(uintptr_t)o, kind);
    kobj_unlock();
    if (!ok) {
        free(o);
        return NULL;
    }
    return o;
}

void kobj_delete(void *obj) {
    kobj_lock();
    hmap64_remove(&g_objects, (uint64_t)(uintptr_t)obj);
    kobj_unlock();
    free(obj);
}

void kobj_forget(void *obj) {
    kobj_lock();
    hmap64_remove(&g_objects, (uint64_t)(uintptr_t)obj);
    kobj_unlock();
}

void *kobj_check(uint64_t handle, obj_kind kind) {
    uint64_t k = 0;
    kobj_lock();
    bool found = hmap64_get(&g_objects, handle, &k);
    kobj_unlock();
    return found && k == (uint64_t)kind ? (void *)(uintptr_t)handle : NULL;
}
