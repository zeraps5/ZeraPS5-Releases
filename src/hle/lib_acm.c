/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ACM context ABI/reference: boykopovar/AnyPS5 bf6e25a1,
 * core/libs/prx/libSceAcm/Export.cpp and core/libs/SceTypes.hpp.
 * IDs are u32 outputs, not pointers to host objects. Contexts belong to the
 * runtime. DSP commands are unsupported: never report an unexecuted batch
 * as completed. This implements lifecycle, not the audio coprocessor. */
#include <stdlib.h>
#include <string.h>
#include "core/runtime.h"
#include "hle/acm.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);
#define ACM_CONTEXTS 64
struct acm_state { uint32_t next_id, contexts[ACM_CONTEXTS]; };

/* Check all four writable bytes under the mapping lock, including a split
 * across adjacent VMAs. A failed probe/write leaves guest memory untouched. */
static bool output_id(hle_call *c, guest_addr addr, const uint32_t *id) {
    if (!addr || addr > UINT64_MAX - 4) return false;
    gmem *m = c->mem;
    pal_mutex_lock(m->lock);
    guest_addr cursor = addr, end = addr + 4;
    for (size_t i = 0; i < m->vmas.len && cursor < end; i++) {
        const gmem_vma *v = &m->vmas.data[i];
        if (v->start + v->size <= cursor) continue;
        if (v->start > cursor || v->reserved || !(v->prot & GPROT_WRITE)) break;
        cursor = PS5C_MIN(end, v->start + v->size);
    }
    bool ok = cursor == end;
    if (ok && id) {
        void *p = m->identity ? (void *)(uintptr_t)addr : m->host_base + addr;
        memcpy(p, id, 4);
    }
    pal_mutex_unlock(m->lock);
    return ok;
}
static bool has_context(const struct acm_state *s, uint32_t id) {
    if (s && id) for (size_t i = 0; i < ACM_CONTEXTS; i++)
        if (s->contexts[i] == id) return true;
    return false;
}
static void context_create(hle_call *c) {
    guest_addr out = hle_arg_ptr(c, 0);
    if (!c->rt || !c->rt->threads_lock || !output_id(c, out, NULL)) {
        hle_ret_s32(c, ACM_INVALID_PARAMETER); return;
    }
    pal_mutex_lock(c->rt->threads_lock);
    struct acm_state *s = c->rt->acm;
    if (!s) c->rt->acm = s = calloc(1, sizeof(*s));
    int32_t result = ACM_NOMEM;
    if (s && s->next_id != UINT32_MAX) {
        for (size_t i = 0; i < ACM_CONTEXTS; i++) if (!s->contexts[i]) {
            uint32_t id = ++s->next_id;
            if (output_id(c, out, &id)) { s->contexts[i] = id; result = 0; }
            else result = ACM_INVALID_PARAMETER;
            break;
        }
    }
    pal_mutex_unlock(c->rt->threads_lock);
    hle_ret_s32(c, result);
}
static void context_destroy(hle_call *c) {
    uint32_t id = hle_arg_u32(c, 0);
    int32_t result = ACM_INVALID_PARAMETER;
    if (c->rt && c->rt->threads_lock) {
        pal_mutex_lock(c->rt->threads_lock);
        struct acm_state *s = c->rt->acm;
        if (s && id) for (size_t i = 0; i < ACM_CONTEXTS; i++) if (s->contexts[i] == id) {
            s->contexts[i] = 0; result = 0; break;
        }
        pal_mutex_unlock(c->rt->threads_lock);
    }
    hle_ret_s32(c, result);
}
static bool context_valid(hle_call *c) {
    if (!c->rt || !c->rt->threads_lock) return false;
    pal_mutex_lock(c->rt->threads_lock);
    bool valid = has_context(c->rt->acm, hle_arg_u32(c, 0));
    pal_mutex_unlock(c->rt->threads_lock);
    return valid;
}
static void batch_start(hle_call *c) {
    /* Both start APIs place the u32 batch output in argument four. The
     * command/error buffer layouts are not implemented or modified. */
    hle_ret_s32(c, context_valid(c) && output_id(c, hle_arg_ptr(c, 4), NULL)
                      ? ACM_UNSUPPORTED : ACM_INVALID_PARAMETER);
}
static void batch_wait(hle_call *c) {
    hle_ret_s32(c, context_valid(c) ? ACM_UNSUPPORTED : ACM_INVALID_PARAMETER);
}
static void command_unsupported(hle_call *c) { hle_ret_s32(c, ACM_UNSUPPORTED); }
void hle_acm_reset(ps5_runtime *rt) {
    free(rt->acm); rt->acm = NULL; /* runtime teardown: no live callers */
}
void hle_register_acm(void) {
    static const hle_export table[] = {
        HLE_FUNC_ENTRY("libSceAcm", "sceAcmContextCreate", context_create),
        HLE_FUNC_ENTRY("libSceAcm", "sceAcmContextDestroy", context_destroy),
        HLE_FUNC_ENTRY("libSceAcm", "sceAcmBatchStartBuffer", batch_start),
        HLE_FUNC_ENTRY("libSceAcm", "sceAcmBatchStartBuffers", batch_start),
        HLE_FUNC_ENTRY("libSceAcm", "sceAcmBatchWait", batch_wait),
        /* commands appended to a batch: batches are not executed, so they are refused too */
        HLE_FUNC_ENTRY("libSceAcm", "sceAcm_ConvReverb_SharedInput", command_unsupported),
        HLE_FUNC_ENTRY("libSceAcm", "sceAcm_FFT", command_unsupported),
    };
    hle_register_table(table, PS5C_ARRAY_COUNT(table));
}
