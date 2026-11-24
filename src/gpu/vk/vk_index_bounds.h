/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_VK_INDEX_BOUNDS_H
#define PS5C_VK_INDEX_BOUNDS_H
#include <stdint.h>
#include <stddef.h>

#define VK_INDEX_BOUNDS_ENTRIES 64
#define VK_INDEX_BOUNDS_MIN_BYTES (4u << 10)
typedef struct vk_index_bounds {
    struct {
        const void *snapshot;
        uint64_t epoch;
        uint32_t count, size, max;
    } entries[VK_INDEX_BOUNDS_ENTRIES];
    uint32_t next;
} vk_index_bounds;

static inline uint32_t vk_index_scan(const void *snapshot, uint32_t count, uint32_t size) {
    uint32_t max = 0;
    if (size == 1) {
        const uint8_t *ix = snapshot;
        uint8_t m = 0;
        for (uint32_t i = 0; i < count; i++) m = ix[i] > m ? ix[i] : m;
        max = m;
    } else if (size == 2) {
        const uint16_t *ix = snapshot;
        uint16_t m = 0;
        for (uint32_t i = 0; i < count; i++) m = ix[i] > m ? ix[i] : m;
        max = m;
    } else {
        const uint32_t *ix = snapshot;
        for (uint32_t i = 0; i < count; i++) max = ix[i] > max ? ix[i] : max;
    }
    return max;
}

/* Snapshots remain immutable until their work-slot fence. Guest VA is not an
 * identity: another copy at that VA in the SAME epoch can contain new indices.
 * Tiny inputs always receive new copies, so avoid 64 misses and cache churn
 * for them. Epoch disambiguates storage reused after a slot completes. */
static inline uint32_t vk_index_maximum(vk_index_bounds *cache, const void *snapshot,
                                        uint32_t count, uint32_t size, uint64_t epoch) {
    if ((size != 1 && size != 2 && size != 4) || (!snapshot && count)) return UINT32_MAX;
    int reusable = (uint64_t)count * size >= VK_INDEX_BOUNDS_MIN_BYTES;
    if (reusable) for (uint32_t i = 0; i < VK_INDEX_BOUNDS_ENTRIES; i++) {
        if (cache->entries[i].snapshot == snapshot && cache->entries[i].epoch == epoch &&
            cache->entries[i].count == count && cache->entries[i].size == size)
            return cache->entries[i].max;
    }
    uint32_t max = vk_index_scan(snapshot, count, size);
    if (reusable) {
        uint32_t k = cache->next++ % VK_INDEX_BOUNDS_ENTRIES;
        cache->entries[k].snapshot = snapshot;
        cache->entries[k].epoch = epoch;
        cache->entries[k].count = count;
        cache->entries[k].size = size;
        cache->entries[k].max = max;
    }
    return max;
}
#endif
