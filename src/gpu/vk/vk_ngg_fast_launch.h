/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_VK_NGG_FAST_LAUNCH_H
#define PS5C_VK_NGG_FAST_LAUNCH_H
#include <stdint.h>
#include <stdbool.h>

/* GFX10.3 mode1: one point (mesh workgroup id) launches one subgroup.
 * Mode2 and multiple input primitives need a different launch ABI. */
static inline uint32_t vk_ngg_fast_launch_threads(uint32_t stages,uint32_t ge,
        uint32_t prim,uint32_t vertices,uint32_t output,uint32_t amp,bool indexed) {
    if(((stages>>19)&3u)!=1 || !(stages&0x00400000u) || (stages&0x2038u)!=0x2030u ||
       (ge&0x3ffffu)!=0x201u || prim!=1 || indexed || !vertices || vertices>256 ||
       output<vertices || output>256 || !amp || amp>256)return 0;
    uint32_t need=output>amp?output:amp;
    return need<=64?64:need<=128?128:256;
}
#endif
