/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the real player with synthetic queues and a recorded guest callback. */
#define runtime_call_nested4 avp_test_nested4
#define hle_register_av_player avp_test_register
#include "../../src/hle/lib_av_player.c"
#undef runtime_call_nested4
#undef hle_register_av_player
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static unsigned callback_count;
static uint64_t last_object, last_event;
bool avp_test_nested4(ps5_runtime *rt, guest_addr fn, uint64_t obj, uint64_t event,
                     uint64_t a, uint64_t b, uint64_t *rax) {
    (void)rt; (void)fn; (void)a; (void)b; *rax=0;
    callback_count++; last_object=obj; last_event=event; return true;
}
int main(int argc, char **argv) {
    bool deferred = argc == 2 && !strcmp(argv[1], "--async");
    CHECK(pal_init()==PS5C_OK);
    CHECK(pal_mutex_create(&g_avp.lock)==PS5C_OK);
    CHECK(pal_cond_create(&g_avp.events)==PS5C_OK);
    ps5_runtime rt; memset(&rt,0,sizeof(rt));
    gmem_config cfg=gmem_default_config(); cfg.identity=false;
    CHECK(gmem_init(&rt.mem,&cfg)==PS5C_OK);
    guest_addr page; CHECK(gmem_map(&rt.mem,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"synthetic movie",&page)==PS5C_OK);
    hle_args args={.gpr={HANDLE_BASE,0,0}};
    hle_call c={.rt=&rt,.mem=&rt.mem,.a=&args};
    av_player *p=&g_avp.players[0]; memset(p,0,sizeof(*p));
    p->used=true; p->has_source=true; p->event_cb=0x1234; p->event_obj=0x5678;
    CHECK(pal_mutex_create(&p->lock)==PS5C_OK); CHECK(pal_cond_create(&p->cond)==PS5C_OK);
    /* READY must arrive now: the guest is allowed to wait without another API call. */
    send_event(&c,p,EV_STATE_READY);
    if (deferred) { CHECK(callback_count==0); avp_current_time(&c); }
    CHECK(callback_count==1 && last_event==EV_STATE_READY && last_object==p->event_obj);
    avp_start(&c); CHECK(args.ret[0]==0);
    if (deferred) { CHECK(callback_count==1); avp_current_time(&c); }
    CHECK(callback_count==2 && last_event==EV_STATE_PLAY);
    avp_pause(&c);
    if (deferred) { CHECK(callback_count==2); avp_current_time(&c); }
    CHECK(p->paused && callback_count==3 && last_event==EV_STATE_PAUSE);
    p->info.width=1920;p->info.height=1080;p->info.frame_rate=30.0;
    p->info.duration_us=2500000;p->info.channels=2;p->info.sample_rate=48000;
    uint8_t *mem=gmem_host(&rt.mem,page,8*GUEST_PAGE_SIZE); CHECK(mem);
    memset(mem,0xa5,128);args.gpr[1]=0;args.gpr[2]=page;
    stream_info(&c,false);CHECK(args.ret[0]==0);
    uint32_t type;memcpy(&type,mem,4);CHECK(type==1);
    uint64_t dur;memcpy(&dur,mem+0x18,8);CHECK(dur==2500);
    for(unsigned i=0x20;i<128;i++)CHECK(mem[i]==0xa5); /* preserve stack canary */
    memset(mem,0xa5,128);stream_info(&c,true);CHECK(args.ret[0]==0);
    double fps;memcpy(&fps,mem+0x40,8);CHECK(fps==30.0);
    for(unsigned i=0;i<8;i++)CHECK(mem[i]==0xa5); /* caller's size prefix */
    for(unsigned i=0x68;i<128;i++)CHECK(mem[i]==0xa5);
    /* File positions reset per loop; scheduling still uses a continuous clock. */
    p->info.width=2;p->info.height=2;p->pitch=256;p->plane_h=16;
    p->paused_us=1100000;p->vbuf=page+GUEST_PAGE_SIZE;p->vbuf_size=GUEST_PAGE_SIZE;
    memset(mem+GUEST_PAGE_SIZE,16,256*16);
    memset(mem+GUEST_PAGE_SIZE+256*16,128,256*16/2);
    uint8_t nv12[6]={20,30,40,50,120,130};p->vq[0]=(avp_picture){nv12,1100000,100000};p->vq_count=1;
    guest_addr frame=0;uint64_t ts=0;CHECK(take_picture(&c,p,&frame,&ts));
    CHECK(ts==100000 && p->shown_base_us==1000000 && frame==p->vbuf);
    uint8_t *pixels=gmem_host(&rt.mem,frame,p->vbuf_size);CHECK(pixels && pixels[0]==20 && pixels[256]==40);
    CHECK(pixels[2]==16 && pixels[256*16]==120);
    avp_current_time(&c);CHECK(args.ret[0]==100);
    /* A paused player must release the first due sound block. */
    p->abuf=page+5*GUEST_PAGE_SIZE;p->abuf_size=GUEST_PAGE_SIZE;
    p->aq[0].frames=2;p->aq[0].ts_us=1100000;p->aq[0].pos_us=100000;
    p->aq[0].pcm[0]=1234;p->aq_count=1;args.gpr[1]=page;
    avp_get_audio_data(&c);CHECK(args.ret[0]==1 && p->aq_count==0 && p->paused);
    int16_t sample;CHECK(gmem_read(&rt.mem,p->abuf,&sample,2));CHECK(sample==1234);
    uint64_t ms;memcpy(&ms,mem+0x10,8);CHECK(ms==100);
    p->event_cb=0;send_event(&c,p,EV_STATE_READY);CHECK(callback_count==3);
    pal_cond_destroy(p->cond);pal_mutex_destroy(p->lock);memset(p,0,sizeof(*p));
    gmem_free(&rt.mem);pal_cond_destroy(g_avp.events);pal_mutex_destroy(g_avp.lock);pal_shutdown();
    puts("AvPlayer: immediate READY, play/pause callbacks, ABI bounds, frame rate, NV12 pitch, loop position and paused sound pass");
    return 0;
}
