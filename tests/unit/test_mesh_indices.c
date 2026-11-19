/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/mesh_indices.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1; } } while (0)
int main(void) {
    uint8_t source[4*513+3]; uint32_t result[515];
    const uint32_t values[]={0,1,0x7fff,0x8000,0xffff,0x80000000u,0xffffffffu};
    for (uint32_t width=2;width<=4;width+=2) {
        for (uint32_t offset=0;offset<3;offset++) {
            memset(source,0x57,sizeof(source));
            for (uint32_t i=0;i<513;i++) {
                uint32_t value=values[i%7];memcpy(source+offset+i*width,&value,width);
            }
            for (uint32_t count=0;count<=513;count++) {
                for(unsigned i=0;i<515;i++) result[i]=0x5aa55aa5;
                CHECK(mesh_indices_expand(result+1,source+offset,count,width));
                CHECK(result[0]==0x5aa55aa5 && result[count+1]==0x5aa55aa5);
                for(uint32_t i=0;i<count;i++)CHECK(result[i+1]==(values[i%7]&(width==2?0xffffu:0xffffffffu)));
            }
        }
    }
    result[0]=0x1234;
    CHECK(!mesh_indices_expand(result,source,1,3) && result[0]==0x1234);
    CHECK(!mesh_indices_expand(NULL,source,1,2));
    CHECK(!mesh_indices_expand(result,NULL,1,4));
    CHECK(mesh_indices_expand(NULL,NULL,0,2));
    puts("mesh indices: 16/32-bit values, unaligned input, tails and sentinels verified");
    return 0;
}
