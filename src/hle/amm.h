/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceAmpr's AMM (address memory manager): lib_ampr.c records the map commands,
 * lib_kernel_apr.c runs them with the APR submissions, lib_kernel_memory.c maps. */
#ifndef PS5C_HLE_AMM_H
#define PS5C_HLE_AMM_H

#include <stdbool.h>
#include <stdint.h>

#include "hle/hle.h"

/* lib_kernel_memory.c: direct memory for the AMM, and the mappings its commands make */
int32_t kernel_amm_give_direct(uint64_t start, uint64_t end, uint64_t size, uint64_t align, uint64_t *out);
int32_t kernel_amm_map(hle_call *c, guest_addr va, uint64_t size, uint32_t prot, bool direct, uint64_t phys);
int32_t kernel_amm_unmap(hle_call *c, guest_addr va, uint64_t size);

/* lib_kernel_apr.c: queues `bytes` bytes of AMM records (cb: the command buffer object or its
 * records); the submission id. apr_wait: until that submission completed. */
uint32_t apr_submit_amm(hle_call *c, guest_addr cb, uint32_t bytes, uint32_t priority, guest_addr result);
void apr_wait(uint32_t id);

#endif
