/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/runtime.h"
#include "core/user_config.h"
#include "base/ini.h"
#include "hle/hle.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include "pal/native_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"native input line %d: %s\n",__LINE__,#x); exit(1); } } while (0)

/* GNU's link-time wrapper isolates the device gate, leaving the real HLE,
 * queues, guest stack and nested callback machinery under test. */
static bool test_pad;
bool __wrap_pal_input_has_controller(void);
bool __real_pal_input_has_controller(void);
bool __wrap_pal_input_has_controller(void) { native_input_controller(test_pad); return test_pad; }
static ps5_runtime *rt;
static guest_addr page;
static int32_t call(const char *lib,const char *name,uint64_t a,uint64_t b,uint64_t d,uint64_t e) {
    const hle_export *exp=hle_find(lib,nid_hash_name(name)); CHECK(exp && exp->fn);
    hle_args args={.gpr={a,b,d,e}}; hle_call c={.rt=rt,.mem=&rt->mem,.a=&args};
    exp->fn(&c); return (int32_t)args.ret[0];
}
#define M(name,a,b,c,d) call("libSceMouse",name,a,b,c,d)
#define K(name,a,b,c,d) call("libSceIme",name,a,b,c,d)
#define U(name,a,b,c,d) call("libSceKeyboard",name,a,b,c,d)

static void queues(void) {
    native_input_configure(false,false); native_input_controller(true);
    CHECK(!native_keyboard_enabled() && !native_mouse_enabled());
    native_key key={.code=26,.character='w',.status=3,.type=4,.event=257};
    native_input_key(&key); CHECK(!native_input_read_key(&key));
    native_input_configure(true,true); CHECK(!native_keyboard_enabled());
    native_input_controller(true); CHECK(native_keyboard_enabled());
    native_input_key(&key); native_input_release();
    CHECK(native_input_read_key(&key) && key.event==258 && key.code==26 && key.character=='w' && (key.status&2));
    CHECK(!native_input_read_key(&key));
    native_mouse mouse={.x=1,.y=-2,.wheel=120};
    for (unsigned i=0;i<1000;i++) native_input_mouse(&mouse);
    CHECK(native_input_read_mouse(&mouse) && mouse.x==1000 && mouse.y==-2000 && mouse.wheel==120000);
    CHECK(!native_input_read_mouse(&mouse) && !mouse.x && !mouse.y && !mouse.wheel);
    mouse=(native_mouse){.buttons=1}; native_input_mouse(&mouse); mouse.buttons=0; native_input_mouse(&mouse);
    CHECK(native_input_read_mouse(&mouse) && mouse.buttons==1);
    CHECK(native_input_read_mouse(&mouse) && mouse.buttons==0);
    mouse=(native_mouse){.x=INT32_MAX}; native_input_mouse(&mouse); native_input_mouse(&mouse);
    CHECK(native_input_read_mouse(&mouse) && mouse.x==INT32_MAX);
    key=(native_key){.code=26,.event=257,.status=1}; native_input_key(&key);
    native_input_controller(false); CHECK(!native_keyboard_enabled());
    CHECK(native_input_read_key(&key) && key.event==258);
    native_input_controller(true);
    for (unsigned i=0;i<5000;i++) { key.event=257; native_input_key(&key); }
    native_input_release(); CHECK(native_input_read_key(&key) && key.event==258);
    CHECK(!native_input_read_key(&key));
}
/* Called on an actual guest stack by a small ABI adapter below. */
static int guest_driver(void) {
    CHECK(K("sceImeUpdate",page+0x200,0,0,0)==0); /* open */
    native_key key={.code=26,.character='w',.status=3,.type=4,.event=257,.time_us=123};
    native_input_key(&key);
    CHECK(K("sceImeUpdate",page+0x200,0,0,0)==0);
    native_input_release(); CHECK(K("sceImeUpdate",page+0x200,0,0,0)==0);
    test_pad=false; CHECK(K("sceImeUpdate",page+0x200,0,0,0)==0);
    test_pad=true; CHECK(K("sceImeUpdate",page+0x200,0,0,0)==0);
    return 7;
}
int main(int argc,char **argv) {
    CHECK(pal_init()==PS5C_OK);
    if (argc==2 && !strcmp(argv[1],"--probe")) {
        printf("physical_controller=%d\n",__real_pal_input_has_controller()); pal_shutdown(); return 0;
    }
    ini *defaults=ini_parse(user_config_default_text,strlen(user_config_default_text)); CHECK(defaults);
    CHECK(ini_get_int(defaults,"entrada","mouse_nativo",-1)==0);
    CHECK(ini_get_int(defaults,"entrada","teclado_nativo",-1)==0); ini_free(defaults);
    queues();
    module_set ms; module_set_init(&ms); rt=calloc(1,sizeof(*rt)); CHECK(rt);
    runtime_options options={0}; CHECK(runtime_init(rt,&ms,&options)==PS5C_OK);
    CHECK(gmem_map(&rt->mem,0,GUEST_PAGE_SIZE,GPROT_RW|GPROT_EXEC,0,"native input fixture",&page)==PS5C_OK);
    native_input_configure(false,false);
    CHECK(M("sceMouseInit",0,0,0,0)==0 && (uint32_t)M("sceMouseOpen",1,0,0,0)==0x80DF0003u);
    CHECK(K("sceImeKeyboardOpen",0,0,0,0)==0); /* previous disabled behavior */
    native_input_configure(true,true);
    CHECK(M("sceMouseOpen",2,0,0,0)==(int32_t)0x80DF0001);
    uint8_t mouse_param[8]; memset(mouse_param,0xA5,sizeof(mouse_param)); mouse_param[0]=1;
    CHECK(gmem_write(&rt->mem,page+0x700,mouse_param,sizeof(mouse_param)));
    CHECK(M("sceMouseOpen",1,0,0,page+0x700)==1);
    CHECK((uint32_t)M("sceMouseOpen",1,0,0,0)==0x80DF0004u);
    CHECK((uint32_t)M("sceMouseRead",99,page,1,0)==0x80DF0003u);
    CHECK((uint32_t)M("sceMouseRead",1,page,65,0)==0x80DF0001u);
    CHECK((uint32_t)M("sceMouseRead",1,UINT64_MAX,1,0)==0x80DF0001u);
    uint8_t sentinel[80]; memset(sentinel,0xA5,sizeof(sentinel)); CHECK(gmem_write(&rt->mem,page+0x500,sentinel,sizeof(sentinel)));
    native_mouse mouse={.buttons=5,.x=12,.y=-7,.wheel=120,.time_us=456}; native_input_mouse(&mouse);
    CHECK(M("sceMouseRead",1,page+0x510,1,0)==1);
    uint8_t bytes[80]; CHECK(gmem_read(&rt->mem,page+0x500,bytes,sizeof(bytes)));
    CHECK(!memcmp(bytes,sentinel,16) && !memcmp(bytes+56,sentinel+56,24));
    CHECK(bytes[24]==1 && bytes[28]==5 && bytes[32]==12 && bytes[36]==249);
    CHECK(M("sceMouseClose",1,0,0,0)==0 && M("sceMouseOpen",1,0,0,0)==1);
    uint64_t param[4]={0,page+0x800,page+0x200,0}; CHECK(gmem_write(&rt->mem,page+0x600,param,sizeof(param)));
    test_pad=false;
    CHECK(K("sceImeKeyboardOpen",1,page+0x600,0,0)==0);
    CHECK((uint32_t)K("sceImeKeyboardGetResourceId",1,page+0x700,0,0)==0x80BC0101u);
    test_pad=true; CHECK(K("sceImeKeyboardGetResourceId",1,page+0x700,0,0)==0);
    CHECK(K("sceImeKeyboardGetInfo",1,page+0x740,0,0)==0);
    uint32_t info[9]; CHECK(gmem_read(&rt->mem,page+0x740,info,sizeof(info)) && info[5]==1);
    /* Callback: store successive 72-byte events after a 64-bit count. */
    const uint8_t callback[]={0x48,0x8b,0x07,0x48,0x6b,0xc0,0x48,0x48,0x8d,0x54,0x07,0x08,
        0x48,0xff,0x07,0x48,0x89,0xd7,0xb9,9,0,0,0,0xf3,0x48,0xa5,0xc3};
    CHECK(gmem_write(&rt->mem,page+0x200,callback,sizeof(callback)));
    uint8_t adapter[]={0x48,0x83,0xec,0x28,0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xd0,0x48,0x83,0xc4,0x28,0xc3};
    uint64_t driver=(uint64_t)(uintptr_t)guest_driver; memcpy(adapter+6,&driver,8);
    CHECK(gmem_write(&rt->mem,page+0x100,adapter,sizeof(adapter)));
    pal_icache_flush(gmem_host(&rt->mem,page,GUEST_PAGE_SIZE),GUEST_PAGE_SIZE);
    guest_thread *thread; CHECK(runtime_start_thread(rt,page+0x100,0,0,0x200000,false,"native keyboard fixture",&thread)==0);
    uint64_t handle=(uint64_t)(uintptr_t)thread,result;
    runtime_release_thread(rt,thread); CHECK(runtime_join_thread(rt,handle,&result)==0 && result==7);
    uint64_t count; CHECK(gmem_read_u64(&rt->mem,page+0x800,&count) && count==5);
    const uint32_t expected[]={256,257,258,261,260};
    for (unsigned i=0;i<5;i++) { uint32_t id; CHECK(gmem_read(&rt->mem,page+0x808+i*72,&id,4) && id==expected[i]); }
    uint16_t code; CHECK(gmem_read(&rt->mem,page+0x808+72+8,&code,2) && code==26);
    CHECK(gmem_read(&rt->mem,page+0x808+144+10,&code,2) && code=='w');
    CHECK(K("sceImeKeyboardClose",1,0,0,0)==0);
    /* PS5's polling keyboard reports every transition as a 64-byte snapshot,
     * retaining a down/up pair read in one frame. The device gate is shared. */
    CHECK(U("sceKeyboardInit",0,0,0,0)==0 && U("sceKeyboardOpen",1,0,0,0)==1);
    test_pad=false;CHECK(U("sceKeyboardRead",1,page+0xa00,1,0)==1);
    uint8_t sample[64];CHECK(gmem_read(&rt->mem,page+0xa00,sample,64)&&sample[16]==0);
    test_pad=true;CHECK(U("sceKeyboardRead",1,page+0xa00,1,0)==1);
    CHECK(gmem_read(&rt->mem,page+0xa00,sample,64)&&sample[16]==1);
    native_key usb={.code=4,.event=257,.status=0x201,.time_us=100};native_input_key(&usb);
    usb.event=258;usb.status=1;usb.time_us=200;native_input_key(&usb);
    CHECK(U("sceKeyboardRead",1,page+0xa00,2,0)==2);
    uint16_t usage;uint32_t modifiers;
    CHECK(gmem_read(&rt->mem,page+0xa20,&usage,2)&&usage==4);
    CHECK(gmem_read(&rt->mem,page+0xa1c,&modifiers,4)&&modifiers==2);
    CHECK(gmem_read(&rt->mem,page+0xa60,&usage,2)&&usage==0);
    usb.event=257;native_input_key(&usb);CHECK(U("sceKeyboardRead",1,page+0xa00,1,0)==1);
    test_pad=false;CHECK(U("sceKeyboardRead",1,page+0xa00,1,0)==1);
    CHECK(gmem_read(&rt->mem,page+0xa00,sample,64)&&sample[16]==0);
    test_pad=true;CHECK(U("sceKeyboardRead",1,page+0xa00,1,0)==1);
    CHECK(gmem_read(&rt->mem,page+0xa20,&usage,2)&&usage==0);
    CHECK(U("sceKeyboardClose",1,0,0,0)==0);
    runtime_free(rt); free(rt); module_set_free(&ms); pal_shutdown();
    puts("native input: defaults, physical controller gate, queues, mouse ABI and real guest callbacks verified"); return 0;
}
