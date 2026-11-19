/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/vk/vk_spirv_bindings.h"
#include "gpu/shader/spv.h"
#include "base/hashmap.h"
#include <stdlib.h>
#include <string.h>

typedef struct resource {
    uint32_t id, binding;
    size_t at;
    bool has_set, aliased, restricted, variable, image;
} resource;

bool vk_spirv_bindings(const uint32_t *code, size_t words, const vk_descriptor_plan *plan,
                       uint32_t **out, size_t *out_words) {
    *out = NULL;
    *out_words = words;
    if (!vk_descriptor_plan_valid(plan) || words < 5 || code[0] != 0x07230203 || !code[3]) return false;
    uint32_t capacity = 2 * plan->count + 1, count = 0;
    resource *vars = calloc(capacity, sizeof(*vars));
    if (!vars) return false;
    hmap64 ids;
    hmap64_init(&ids);
    bool ok = false;
    bool image_writes = false;
    size_t annotation = SIZE_MAX;
    uint16_t members[VK_DESCRIPTOR_BINDINGS] = {0};
    for (uint32_t i = 0; i < plan->count; i++) members[plan->binding[i]]++;
    for (size_t at = 5; at < words;) {
        uint32_t n = code[at] >> 16, op = code[at] & 0xffff;
        if (!n || n > words - at) goto done;
        image_writes |= op == SpvOpImageWrite || op == SpvOpImageTexelPointer;
        if (op == SpvOpDecorate) {
            if (n < 3) goto done;
            if (annotation == SIZE_MAX) annotation = at;
            if (code[at + 2] == SpvDecorationBinding) {
                if (n != 4 || count == capacity || !code[at + 1] || code[at + 1] >= code[3] ||
                    code[at + 3] >= plan->count || hmap64_get(&ids, code[at + 1], &(uint64_t){0}) ||
                    !hmap64_put(&ids, code[at + 1], count)) goto done;
                vars[count++] = (resource){.id=code[at + 1], .binding=code[at + 3], .at=at};
            }
        }
        at += n;
    }
    /* Sets/decorations can precede Binding, so resolve them in another pass. */
    for (size_t at = 5; at < words;) {
        uint32_t n = code[at] >> 16, op = code[at] & 0xffff;
        uint64_t index;
        if (op == SpvOpDecorate && hmap64_get(&ids, code[at + 1], &index)) {
            resource *v = vars + index;
            uint32_t decoration = code[at + 2];
            if (decoration == SpvDecorationDescriptorSet) {
                if (n != 4 || code[at + 3] || v->has_set) goto done;
                v->has_set = true;
            } else if (decoration == SpvDecorationAliased) v->aliased = true;
            else if (decoration == 19 /* Restrict */) v->restricted = true;
        }
        if (op == SpvOpVariable) {
            if (n < 4) goto done;
            if (hmap64_get(&ids, code[at + 2], &index)) {
                if (code[at + 3] != SpvStorageClassUniformConstant && code[at + 3] != SpvStorageClassStorageBuffer &&
                    code[at + 3] != SpvStorageClassUniform) goto done;
                vars[index].variable = true;
                vars[index].image = code[at + 3] == SpvStorageClassUniformConstant;
            }
        }
        at += n;
    }
    size_t extra = 0;
    bool changed = false;
    bool shared_images = false;
    for (uint32_t i = 0; i < count; i++)
        shared_images |= image_writes && vars[i].image && members[plan->binding[vars[i].binding]] > 1;
    for (uint32_t i = 0; i < count; i++) {
        resource *v = vars + i;
        /* An image not merged here can still be another storage/sampled view
         * of a merged image. Preserve those cross-binding memory aliases too. */
        bool shared = (members[plan->binding[v->binding]] > 1 && (!v->image || image_writes)) || (shared_images && v->image);
        if (!v->has_set || !v->variable || (shared && v->restricted)) goto done;
        if (shared && !v->aliased) extra += 3;
        changed |= plan->binding[v->binding] != v->binding;
    }
    if (!changed && !extra) { ok = true; goto done; }
    if (annotation == SIZE_MAX || words > SIZE_MAX / 4 - extra) goto done;
    uint32_t *copy = malloc((words + extra) * 4);
    if (!copy) goto done;
    memcpy(copy, code, annotation * 4);
    size_t cursor = annotation;
    for (uint32_t i = 0; i < count; i++)
        if (((members[plan->binding[vars[i].binding]] > 1 && (!vars[i].image || image_writes)) ||
             (shared_images && vars[i].image)) && !vars[i].aliased) {
            copy[cursor++] = (3u << 16) | SpvOpDecorate;
            copy[cursor++] = vars[i].id;
            copy[cursor++] = SpvDecorationAliased;
        }
    memcpy(copy + cursor, code + annotation, (words - annotation) * 4);
    for (uint32_t i = 0; i < count; i++) copy[vars[i].at + extra + 3] = plan->binding[vars[i].binding];
    *out = copy;
    *out_words = words + extra;
    ok = true;
done:
    hmap64_free(&ids);
    free(vars);
    return ok;
}
