/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hle/pad_script.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(void) {
    double duration;
    CHECK(pad_script_parse("cross",5,&duration)==PAL_PAD_CROSS && duration==0.15);
    CHECK(pad_script_parse("shake/4",7,&duration)==SCRIPT_SHAKE && duration==4);
    CHECK(pad_script_parse("ls_up/300",9,&duration)==SCRIPT_LS_UP && duration==300);
    const char *bad[]={"shake/", "shake/0", "shake/-1", "shake/nan", "shake/inf", "shake/301", "shake/4x", "missing", "cross+up"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) CHECK(!pad_script_parse(bad[i],strlen(bad[i]),&duration));
    uint32_t flips;
    CHECK(pad_script_parse_hold("cross/2f",8,&duration,&flips)==PAL_PAD_CROSS && flips==2 && duration==300);
    CHECK(pad_script_parse_hold("rs_right/120f",13,&duration,&flips)==SCRIPT_RS_RIGHT && flips==120);
    CHECK(!pad_script_parse("cross/2f",8,&duration));
    const char *bad_frames[]={"cross/f","cross/0f","cross/-1f","cross/+1f","cross/1.5f","cross/121f","cross/99999999999999999999f","cross/2ff"};
    for (unsigned i=0;i<sizeof(bad_frames)/sizeof(bad_frames[0]);i++)
        CHECK(!pad_script_parse_hold(bad_frames[i],strlen(bad_frames[i]),&duration,&flips));
    /* Repeated input sampling during one slow frame must not consume the
     * frame hold. Two presentations end it; a stall still has a deadline. */
    CHECK(pad_script_held(50,10,300,2,100,100,255));
    CHECK(pad_script_held(60,10,300,2,100,101,255));
    CHECK(!pad_script_held(61,10,300,2,100,102,255));
    CHECK(!pad_script_held(310,10,300,2,100,100,0));
    CHECK(!pad_script_held(9,10,300,2,100,100,0));
    CHECK(pad_script_held(11,10,300,2,UINT64_MAX-1,UINT64_MAX,255));
    CHECK(pad_script_held(20,10,0.15,0,0,100,1));
    CHECK(!pad_script_held(20,10,0.15,0,0,100,2));
    /* touch_l/touch_r: the touch pad click plus a finger on that half; the script bits stay internal */
    CHECK(pad_script_parse("touch_r",7,&duration)==SCRIPT_TOUCH_RIGHT);
    {
        pal_pad_state touch={0};
        pad_script_apply(&touch,SCRIPT_TOUCH_RIGHT,0);
        CHECK(touch.buttons==PAL_PAD_TOUCH_PAD && touch.touch_count==1 && touch.touch[0].x>=960);
        pal_pad_state left={0};
        pad_script_apply(&left,SCRIPT_TOUCH_LEFT,0);
        CHECK(left.buttons==PAL_PAD_TOUCH_PAD && left.touch_count==1 && left.touch[0].x<960);
    }
    pal_pad_state state={0};state.lx=state.ly=state.rx=state.ry=128;
    pal_pad_state old=state;pad_script_apply(&state,0,0);CHECK(!memcmp(&old,&state,sizeof(state)));
    pad_script_apply(&state,PAL_PAD_L2|PAL_PAD_R2|SCRIPT_LS_UP|SCRIPT_RS_RIGHT,0);
    CHECK(state.l2==255 && state.r2==255 && state.ly==0 && state.rx==255);
    CHECK(state.buttons==(PAL_PAD_L2|PAL_PAD_R2) && !state.motion);
    pad_script_apply(&state,SCRIPT_SHAKE,0);
    CHECK(state.motion && state.accel[0]==-3 && state.accel[1]==1 && state.gyro[0]==-8);
    CHECK(!(state.buttons & SCRIPT_SHAKE));
    pad_script_apply(&state,SCRIPT_SHAKE,100000000ull);
    CHECK(state.accel[0]==3 && state.accel[2]==-2 && state.gyro[0]==8);
    puts("scripted pad: bounded holds, analog triggers, sticks and motion without guest bit leakage");
    return 0;
}
