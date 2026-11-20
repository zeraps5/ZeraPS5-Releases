/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "base/base.h"
#include "hle/hle.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int32_t call3(gmem *m,const char *name,uint64_t a,uint64_t b,uint64_t d) {
    const hle_export *exp=hle_find("libScePad",nid_hash_name(name));
    if(!exp || !exp->fn)return INT32_MIN;
    hle_args args={.gpr={a,b,d}};hle_call c={.a=&args,.mem=m};exp->fn(&c);return (int32_t)args.ret[0];
}
static int32_t call(gmem *m,const char *name,uint64_t a,uint64_t b){return call3(m,name,a,b,0);}
static float f32(const uint8_t *p,unsigned at){float f;memcpy(&f,p+at,4);return f;}
int main(void) {
#ifdef _WIN32
    _putenv_s("PS5C_PAD_SCRIPT","f0:shake/300,f0:l2/300,f0:r2/300"); ps5c_env_reset();
    _putenv_s("PS5C_PAD_SCRIPT_ONLY","1"); ps5c_env_reset();
#else
    setenv("PS5C_PAD_SCRIPT","f0:shake/300,f0:l2/300,f0:r2/300",1); ps5c_env_reset();
    setenv("PS5C_PAD_SCRIPT_ONLY","1",1); ps5c_env_reset();
#endif
    CHECK(pal_init()==PS5C_OK);CHECK(hle_registry_init()==PS5C_OK);
    gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;CHECK(gmem_init(&m,&cfg)==PS5C_OK);
    guest_addr addr;CHECK(gmem_map(&m,0,GUEST_PAGE_SIZE,GPROT_RW,0,"pad output",&addr)==PS5C_OK);
    uint8_t *data=gmem_host(&m,addr,120);CHECK(data);
    CHECK(call(&m,"scePadInit",0,0)==0);
    /* user 1: the standard port opens; special (2) and remote (16) ports have no device */
    CHECK(call3(&m,"scePadOpen",1,0,0)==1 && call3(&m,"scePadGetHandle",1,0,0)==1);
    CHECK((uint32_t)call3(&m,"scePadOpen",1,2,0)==0x80920007u);
    CHECK((uint32_t)call3(&m,"scePadOpen",1,16,0)==0x80920007u);
    CHECK((uint32_t)call3(&m,"scePadGetHandle",1,2,0)==0x80920007u);
    CHECK((uint32_t)call3(&m,"scePadOpen",99,0,0)==0x80920001u);
    CHECK(call(&m,"scePadReadState",1,addr)==0);
    CHECK(fabsf(f32(data,28))==3 && f32(data,32)==1 && fabsf(f32(data,40))==8);
    CHECK(data[8]==255 && data[9]==255 && data[76]==1 && f32(data,24)==1);
    CHECK(call(&m,"scePadSetMotionSensorState",1,0)==0);
    CHECK(call(&m,"scePadReadState",1,addr)==0);
    CHECK(f32(data,28)==0 && f32(data,32)==1 && f32(data,36)==0);
    CHECK(f32(data,40)==0 && f32(data,44)==0 && f32(data,48)==0);
    CHECK((uint32_t)call(&m,"scePadSetMotionSensorState",999,1)==0x80920003u);
    CHECK(call(&m,"scePadReadState",1,addr)==0 && f32(data,28)==0);
    CHECK(call(&m,"scePadSetMotionSensorState",1,1)==0);
    CHECK(call(&m,"scePadReadState",1,addr)==0 && fabsf(f32(data,28))==3);
    /* Unknown query must not report success with an untouched stack output.
     * ABI remains unsupported: even invalid pointers must never be accessed. */
    uint64_t unknown_nid;CHECK(nid_from_str("zBsgF0q8DIM",NID_STR_LEN,&unknown_nid));
    const hle_export *unknown=hle_find("libScePad",unknown_nid);CHECK(unknown && unknown->fn);
    uint8_t before[120];memcpy(before,data,sizeof(before));
    for(unsigned variant=0;variant<4;variant++) {
        hle_args args={.gpr={variant==1?999:1,variant==2?0:variant==3?UINT64_MAX:addr}};
        hle_call c={.mem=&m,.a=&args};unknown->fn(&c);
        CHECK((int32_t)args.ret[0]==SCE_KERNEL_ERROR_ENOSYS);
        CHECK(!memcmp(before,data,sizeof(before)));
    }
    gmem_free(&m);hle_registry_free();pal_shutdown();
    puts("pad HLE: port types, motion initially on, disable/re-enable, invalid handles, rest pose and analog triggers verified");return 0;
}
