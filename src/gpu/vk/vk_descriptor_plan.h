/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_VK_DESCRIPTOR_PLAN_H
#define PS5C_VK_DESCRIPTOR_PLAN_H
#include "gpu/shader/shader.h"
#include "gpu/vk/vk_api.h"

#define VK_DESCRIPTOR_BINDINGS (2u * (SH_MAX_SITES + 1u))
typedef struct vk_descriptor_plan {
    uint16_t binding[VK_DESCRIPTOR_BINDINGS]; /* logical binding -> earliest equivalent binding */
    uint32_t count;
    bool compact; /* a stage was planned with equal images sharing a binding */
} vk_descriptor_plan;

void vk_descriptor_plan_init(vk_descriptor_plan *plan);
bool vk_descriptor_plan_append(vk_descriptor_plan *plan);
bool vk_descriptor_plan_stage(vk_descriptor_plan *plan, const sh_program *program,
                              const sh_eval *eval, const sh_site_shape *shapes, bool compact);
void vk_descriptor_plan_identity(vk_descriptor_plan *plan);
bool vk_descriptor_plan_valid(const vk_descriptor_plan *plan);
uint32_t vk_descriptor_plan_unique(const vk_descriptor_plan *plan);
bool vk_descriptor_plan_images_match(const vk_descriptor_plan *plan, const VkWriteDescriptorSet *writes,
                                     const VkDescriptorImageInfo *images, uint32_t count);
#endif
