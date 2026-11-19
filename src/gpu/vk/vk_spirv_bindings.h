/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_VK_SPIRV_BINDINGS_H
#define PS5C_VK_SPIRV_BINDINGS_H
#include "gpu/vk/vk_descriptor_plan.h"

/* A copy with Binding remapped and Aliased on shared variables. The input is
 * immutable. NULL output means no rewrite was needed. False rejects malformed
 * resource metadata or allocation failure; this is not a full SPIR-V validator.
 * No instruction accessing a resource is discarded. */
bool vk_spirv_bindings(const uint32_t *code, size_t words, const vk_descriptor_plan *plan,
                       uint32_t **out, size_t *out_words);
#endif
