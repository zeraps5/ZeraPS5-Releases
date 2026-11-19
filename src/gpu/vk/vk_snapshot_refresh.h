/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Block refresh adapted from AnyPS5 7c1b60c5. Our guest's native writes are
 * not completely tracked, so compare every block. The owner may permit
 * writes only after every GPU user of the snapshot has completed. */
#ifndef PS5C_VK_SNAPSHOT_REFRESH_H
#define PS5C_VK_SNAPSHOT_REFRESH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define VK_SNAPSHOT_BLOCK 65536u
typedef struct vk_snapshot_refresh_result {
    bool ready;
    size_t compared, copied;
} vk_snapshot_refresh_result;
/* Never bind a cached subrange at an offset the device cannot represent. */
static inline bool vk_snapshot_window(uint64_t base, uint64_t bytes, uint64_t address,
                                      uint64_t size, uint64_t alignment, uint64_t *offset) {
    if (!size || address < base || !alignment || bytes > UINT64_MAX-base || size > UINT64_MAX-address) return false;
    uint64_t at=address-base;
    if (at > bytes || size > bytes-at || at%alignment) return false;
    *offset=at;
    return true;
}
static inline vk_snapshot_refresh_result vk_snapshot_refresh(void *dst, const void *src,
                                                              size_t bytes, bool writable) {
    vk_snapshot_refresh_result result = {.ready=true};
    uint8_t *d=dst; const uint8_t *s=src;
    for(size_t at=0;at<bytes;) {
        size_t len=bytes-at < VK_SNAPSHOT_BLOCK ? bytes-at : VK_SNAPSHOT_BLOCK;
        result.compared+=len;
        if(memcmp(d+at,s+at,len)) {
            if(!writable) { result.ready=false; break; }
            memcpy(d+at,s+at,len); result.copied+=len;
        }
        at+=len;
    }
    return result;
}
#endif
