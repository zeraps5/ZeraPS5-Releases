/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Legal zero-base host dispatches covering the guest grid exactly once. */
#ifndef PS5C_VK_COMPUTE_DISPATCH_H
#define PS5C_VK_COMPUTE_DISPATCH_H
#include <stdbool.h>
#include <stdint.h>

typedef struct vk_compute_part {
    uint32_t base[3], count[3];
} vk_compute_part;
typedef struct vk_compute_partition {
    uint32_t total[3], limit[3], cursor[3];
    bool done;
} vk_compute_partition;

static inline void vk_compute_partition_init(vk_compute_partition *p, const uint32_t total[3],
                                             const uint32_t limit[3]) {
    p->done = false;
    for (unsigned k = 0; k < 3; k++) {
        p->total[k] = total[k];
        p->limit[k] = limit[k];
        p->cursor[k] = 0;
        if (!total[k] || !limit[k])
            p->done = true;
    }
}
static inline bool vk_compute_partition_next(vk_compute_partition *p, vk_compute_part *part) {
    if (p->done)
        return false;
    for (unsigned k = 0; k < 3; k++) {
        part->base[k] = p->cursor[k];
        uint32_t left = p->total[k] - p->cursor[k];
        part->count[k] = left < p->limit[k] ? left : p->limit[k];
    }
    for (unsigned k = 0; k < 3; k++) {
        /* count <= remaining: this addition cannot wrap, even at UINT32_MAX. */
        p->cursor[k] += part->count[k];
        if (p->cursor[k] < p->total[k])
            return true;
        p->cursor[k] = 0;
    }
    p->done = true;
    return true;
}
#endif
