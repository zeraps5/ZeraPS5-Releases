/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Conservative mip feedback from bound textures, experimental and opt-in.
 * Report layout documented by KytyPS5 PR1631 (devklex, b5e855b), GPL-2.0:
 * https://github.com/KytyPS5/KytyPS5/pull/1631
 * This observes bindings, not individual shader samples or their actual LOD.
 * A bound texture asks for mip0; unused counters retain the hardware sentinel.
 */
#ifndef PS5C_GPU_LOD_STATS_H
#define PS5C_GPU_LOD_STATS_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#define GPU_LOD_COUNTERS 256u
#define GPU_LOD_HEADER_BYTES 64u
#define GPU_LOD_REPORT_BYTES (GPU_LOD_HEADER_BYTES + GPU_LOD_COUNTERS * 8u)
#define GPU_LOD_FORCE_RESET (1u << 18)
#define GPU_LOD_REPORT_RESET (1u << 19)
typedef struct gpu_lod_stats { uint64_t bound[4]; } gpu_lod_stats;
static inline void gpu_lod_note(gpu_lod_stats *s,const uint32_t desc[8]) {
    if(!(desc[5] & (1u<<25)))return;
    uint32_t id=desc[6]&255u;
    s->bound[id/64]|=UINT64_C(1)<<(id%64);
}
static inline bool gpu_lod_report(gpu_lod_stats *s,void *out,uint32_t bytes,uint32_t flags) {
    if(bytes>GPU_LOD_REPORT_BYTES || (bytes && !out))return false;
    if(bytes)memset(out,0,bytes);
    uint8_t *p=out;
    for(uint32_t i=0;bytes>=GPU_LOD_HEADER_BYTES && i<(bytes-GPU_LOD_HEADER_BYTES)/8;i++) {
        uint64_t word=(s->bound[i/64]>>(i%64))&1u?UINT64_C(1):UINT64_C(15)<<56;
        memcpy(p+GPU_LOD_HEADER_BYTES+8*i,&word,8);
    }
    if(bytes>=4){uint32_t ready=1;memcpy(p,&ready,4);}
    if(flags&(GPU_LOD_FORCE_RESET|GPU_LOD_REPORT_RESET))memset(s,0,sizeof(*s));
    return true;
}
#endif
