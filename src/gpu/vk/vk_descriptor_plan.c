/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/vk/vk_descriptor_plan.h"
#include "gpu/shader/shader_ops.h"
#include <string.h>

void vk_descriptor_plan_init(vk_descriptor_plan *p) { p->count = 0; p->compact = false; }

bool vk_descriptor_plan_append(vk_descriptor_plan *p) {
    if (p->count >= VK_DESCRIPTOR_BINDINGS) return false;
    p->binding[p->count] = (uint16_t)p->count;
    p->count++;
    return true;
}

void vk_descriptor_plan_identity(vk_descriptor_plan *p) {
    for (uint32_t i = 0; i < p->count; i++) p->binding[i] = (uint16_t)i;
}

bool vk_descriptor_plan_valid(const vk_descriptor_plan *p) {
    if (p->count > VK_DESCRIPTOR_BINDINGS) return false;
    for (uint32_t i = 0; i < p->count; i++)
        if (p->binding[i] > i || p->binding[p->binding[i]] != p->binding[i]) return false;
    return true;
}

uint32_t vk_descriptor_plan_unique(const vk_descriptor_plan *p) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < p->count; i++) n += p->binding[i] == i;
    return n;
}

static bool image_known(const sh_program *p, const sh_eval *e, const sh_site_shape *shape, uint32_t s) {
    return p->sites[s].kind == SH_SITE_IMAGE && p->sites[s].inst < p->count &&
           e->site[s].valid && !e->site[s].desc_unknown && !e->site[s].records_unknown && !shape[s].address_map;
}

static bool image_equal(const sh_program *p, const sh_eval *e, const sh_site_shape *shape, uint32_t a, uint32_t b) {
    return p->sites[a].store == p->sites[b].store &&
           (p->ops[p->sites[a].inst] == SH_OP_IMAGE_ATOMIC) == (p->ops[p->sites[b].inst] == SH_OP_IMAGE_ATOMIC) &&
           sh_site_arrayed(p, a, shape[a].img_type) == sh_site_arrayed(p, b, shape[b].img_type) &&
           !memcmp(&shape[a], &shape[b], sizeof(*shape)) &&
           !memcmp(e->site[a].desc, e->site[b].desc, sizeof(e->site[a].desc)) &&
           (sh_site_storage(&p->sites[a], &shape[a]) ||
            !memcmp(e->site[a].sampler, e->site[b].sampler, sizeof(e->site[a].sampler)));
}

static uint64_t image_hash(const sh_site_value *v, const sh_site_shape *shape, bool storage) {
    /* A collision is only a candidate: image_equal always checks all fields.
     * Storage images ignore unused sampler bits. */
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *parts[] = {(const uint8_t *)v->desc, (const uint8_t *)shape, (const uint8_t *)v->sampler};
    const size_t sizes[] = {sizeof(v->desc), sizeof(*shape), storage ? 0 : sizeof(v->sampler)};
    for (unsigned k = 0; k < 3; k++)
        for (size_t i = 0; i < sizes[k]; i++) h = (h ^ parts[k][i]) * 0x100000001b3ull;
    return h;
}

bool vk_descriptor_plan_stage(vk_descriptor_plan *out, const sh_program *p,
                              const sh_eval *e, const sh_site_shape *shape, bool compact) {
    if (p->site_count > SH_MAX_SITES || !vk_descriptor_plan_append(out)) return false; /* uniforms */
    if (!compact) {
        for (uint32_t s = 0; s < p->site_count; s++)
            if ((p->sites[s].kind != SH_SITE_SMEM || shape[s].address_map) && !vk_descriptor_plan_append(out)) return false;
        return true;
    }
    out->compact = true;
    uint16_t table[2 * SH_MAX_SITES], binding[SH_MAX_SITES];
    uint64_t hashes[SH_MAX_SITES];
    uint32_t slots = 1;
    while (slots < 2 * p->site_count) slots *= 2;
    memset(table, 0xff, slots * sizeof(*table));
    for (uint32_t s = 0; s < p->site_count; s++) {
        if (p->sites[s].kind == SH_SITE_SMEM && !shape[s].address_map) continue;
        uint32_t b = out->count;
        if (!vk_descriptor_plan_append(out)) return false;
        binding[s] = (uint16_t)b;
        if (!image_known(p, e, shape, s)) continue;
        uint64_t hash = image_hash(&e->site[s], &shape[s], sh_site_storage(&p->sites[s], &shape[s]));
        hashes[s] = hash;
        uint32_t slot = (uint32_t)(hash ^ (hash >> 32)) & (slots - 1);
        while (table[slot] != UINT16_MAX) {
            uint32_t old = table[slot];
            if (hashes[old] == hash && image_equal(p, e, shape, s, old)) {
                out->binding[b] = binding[old];
                break;
            }
            slot = (slot + 1) & (slots - 1);
        }
        if (table[slot] == UINT16_MAX) table[slot] = (uint16_t)s;
    }
    return true;
}

bool vk_descriptor_plan_images_match(const vk_descriptor_plan *p, const VkWriteDescriptorSet *w,
                                     const VkDescriptorImageInfo *images, uint32_t count) {
    if (p->count != count || !vk_descriptor_plan_valid(p)) return false;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t root = p->binding[i];
        if (root == i) continue;
        if (w[i].descriptorType != w[root].descriptorType || w[i].descriptorCount != 1 || w[root].descriptorCount != 1 ||
            (w[i].descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && w[i].descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) ||
            images[i].imageView != images[root].imageView || images[i].imageLayout != images[root].imageLayout ||
            (w[i].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && images[i].sampler != images[root].sampler))
            return false;
    }
    return true;
}
