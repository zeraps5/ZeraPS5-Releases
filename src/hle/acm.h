/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_ACM_H
#define PS5C_ACM_H
#include <stdint.h>
struct ps5_runtime;
#define ACM_INVALID_PARAMETER ((int32_t)0x80e30002u)
/* Generic kernel failures until the proprietary ACM equivalents are known. */
#define ACM_UNSUPPORTED ((int32_t)0x8002004eu) /* FreeBSD ENOSYS */
#define ACM_NOMEM ((int32_t)0x8002000cu)
void hle_acm_reset(struct ps5_runtime *rt);
#endif
