/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hle/avplayer_timing.h"
#include <stdio.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void) {
    uint64_t times[3]={0,16667,33334};
    CHECK(avp_late_video_frames(times,3,0,0,1000000)==0);
    CHECK(avp_late_video_frames(times,3,0,1,1000000)==0);
    CHECK(avp_late_video_frames(times,3,0,3,0)==0);
    CHECK(avp_late_video_frames(times,3,0,3,16667)==1);
    CHECK(avp_late_video_frames(times,3,0,3,33334)==2);
    uint64_t wrapped[3]={33334,50001,16667};
    CHECK(avp_late_video_frames(wrapped,3,2,3,33334)==1);
    /* Simulate a 60-fps, 8.5-second clip with a consumer polling only twice
     * per second. Producer gets clock ticks every 5 ms. Returned timestamps
     * must follow wall time, with future pictures still queued. */
    uint32_t head=0,count=0,next=0,consumed=0;
    uint64_t newest=0;
    for(uint64_t now=0;now<=9000000;now+=5000) {
        for(unsigned step=0;step<4;step++) {
            uint32_t discard=avp_late_video_frames(times,3,head,count,now);
            head=(head+discard)%3;count-=discard;
            if(count==3 || next==510)break;
            times[(head+count)%3]=(uint64_t)next++*1000000/60;count++;
        }
        if(now%500000==0 && count && times[head]<=now) {
            uint32_t discard=avp_late_video_frames(times,3,head,count,now);
            head=(head+discard)%3;count-=discard;newest=times[head];
            CHECK(now>=8500000 || now-newest<=16667);
            head=(head+1)%3;count--;consumed++;
        }
    }
    CHECK(next==510 && !count && consumed>=17 && newest>=8480000);
    puts("AvPlayer: late frames catch up at 2 consumer polls/sec, future frames retained");return 0;
}
