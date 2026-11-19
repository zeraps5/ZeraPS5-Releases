/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_AVPLAYER_TIMING_H
#define PS5C_AVPLAYER_TIMING_H
#include <stdint.h>
/* Keep the newest queued frame due now, plus every future frame. The
 * producer also uses this: a slow consumer must not slow the media clock. */
static inline uint32_t avp_late_video_frames(const uint64_t *timestamps,uint32_t capacity,
                                            uint32_t head,uint32_t count,uint64_t now) {
    uint32_t discard=0;
    while(count>1 && timestamps[(head+1)%capacity]<=now) {
        head=(head+1)%capacity;count--;discard++;
    }
    return discard;
}
#endif
