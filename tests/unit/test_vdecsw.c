/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The fixture is a generated solid-blue AVC frame, not console/game data. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/runtime.h"
#include "hle/hle.h"
#include "loader/nid.h"
#include "pal/pal.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int32_t call(ps5_runtime *rt,const char *name,uint64_t a,uint64_t b,uint64_t d) {
    const hle_export *e=hle_find("libSceVdecsw",nid_hash_name(name));
    if(!e || !e->fn)return INT32_MIN;
    hle_args args={.gpr={a,b,d}};hle_call c={.rt=rt,.mem=&rt->mem,.a=&args};e->fn(&c);return (int32_t)args.ret[0];
}
static void put64(uint8_t *p,uint64_t v){memcpy(p,&v,8);}
static void put32(uint8_t *p,uint32_t v){memcpy(p,&v,4);}
static uint64_t get64(const uint8_t *p){uint64_t v;memcpy(&v,p,8);return v;}
static uint32_t get32(const uint8_t *p){uint32_t v;memcpy(&v,p,4);return v;}
static bool init(ps5_runtime *rt,guest_addr *a,uint64_t size) {
    memset(rt,0,sizeof(*rt));gmem_config cfg=gmem_default_config();cfg.identity=false;
    return gmem_init(&rt->mem,&cfg)==PS5C_OK && gmem_map(&rt->mem,0,size,GPROT_RW,0,"Vdecsw regression",a)==PS5C_OK;
}
int main(int argc,char **argv) {
    CHECK(argc==3 && pal_init()==PS5C_OK && hle_registry_init()==PS5C_OK);
    ps5_runtime rt,other;guest_addr a,b;
    CHECK(init(&rt,&a,64ull<<20) && init(&other,&b,GUEST_PAGE_SIZE));
    uint8_t *p=gmem_host(&rt.mem,a,64ull<<20),*q=gmem_host(&other.mem,b,GUEST_PAGE_SIZE);CHECK(p && q);
    put64(p,16);put32(p+8,0x20002);p[12]=1; /* compute config */
    put64(p+0x100,24);p[0x118]=0xa5;
    CHECK(call(&rt,"sceVdecswQueryComputeMemoryInfo",a+0x100,0,0)==0);
    CHECK(get64(p+0x108)==16ull<<20 && p[0x118]==0xa5);
    put64(p+0x110,a+0x10000);
    CHECK(call(&rt,"sceVdecswAllocateComputeQueue",a,a+0x100,a+0x200)==0);
    uint64_t queue=get64(p+0x200);CHECK(queue);
    put64(p+0x300,80);put32(p+0x308,1);put32(p+0x30c,1);put32(p+0x310,100);
    put32(p+0x314,42);put32(p+0x318,16);put32(p+0x31c,18);put32(p+0x320,4);put32(p+0x324,3);put64(p+0x328,queue);
    put64(p+0x400,72);p[0x448]=0xa5;
    put64(p+0x300,72);
    CHECK(call(&rt,"sceVdecswQueryDecoderMemoryInfo",a+0x300,a+0x400,0)!=0);
    CHECK(get64(p+0x408)==0 && p[0x448]==0xa5);
    put64(p+0x300,80);
    CHECK(call(&rt,"sceVdecswQueryDecoderMemoryInfo",a+0x300,a+0x400,0)==0);
    CHECK(get64(p+0x438)>=16ull<<20 && get32(p+0x440)==256 && p[0x448]==0xa5);
    put64(p+0x410,a+0x10000);put64(p+0x420,a+0x10000);put64(p+0x430,a+0x10000);
    int32_t created=call(&rt,"sceVdecswCreateDecoder",a+0x300,a+0x400,a+0x500);
#ifndef PS5C_HAVE_FFMPEG
    CHECK(created!=0);puts("Vdecsw ABI checks passed; no FFmpeg decoder in this build");
#else
    CHECK(created==0);uint64_t handle=get64(p+0x500);CHECK(handle);
    put64(p+0x600,24);put64(p+0x608,a+0x3000000);put64(p+0x610,0x10000);
    put64(p+0x700,48);put64(p+0x708,a+0x1000);
    FILE *f=fopen(argv[1],"rb");CHECK(f);uint8_t fixture[4096];size_t n=fread(fixture,1,sizeof(fixture),f);fclose(f);
    CHECK(n>0 && n<sizeof(fixture));memcpy(p+0x1000,fixture,n);put64(p+0x710,n);
    put64(p+0x718,111);put64(p+0x720,100);put64(p+0x728,77);
    put64(p+0x800,56);put64(q+0x800,56);put64(p+0xb00,24);
    memset(p+0x3000000,0xa5,0x10000);
    /* Input precedes output; its guest bytes can be recycled after submission. */
    CHECK(call(&rt,"sceVdecswSetDecodeInput",handle,a+0x700,0)==0);memset(p+0x1000,0,n);
    CHECK(call(&rt,"sceVdecswTrySyncDecodeInput",handle,a+0xb00,0)==0 && get64(p+0xb08)==a+0x1000 && get32(p+0xb10)==1);
    CHECK(call(&rt,"sceVdecswTrySyncDecodeInput",handle,a+0xb00,0)==(int32_t)0x81510116 && get32(p+0xb10)==0);
    CHECK(call(&rt,"sceVdecswSetDecodeOutput",handle,a+0x600,0)==0);
    CHECK(call(&rt,"sceVdecswSetDecodeOutput",handle,a+0x600,0)!=0); /* duplicate live output */
    CHECK(call(&other,"sceVdecswTrySyncDecodeOutput",handle,b+0x800,0)!=0);
    int32_t result=call(&rt,"sceVdecswTrySyncDecodeOutput",handle,a+0x800,0);
    if(result==(int32_t)0x81510117) {
        CHECK(call(&rt,"sceVdecswFinalizeDecodeSequence",handle,0,0)==0);
        result=call(&rt,"sceVdecswTrySyncDecodeOutput",handle,a+0x800,0);
    }
    CHECK(result==0 && p[0x808]==1 && p[0x80a]==0 && p[0x80b]==1);
    CHECK(get32(p+0x80c)==1 && get32(p+0x810)==16 && get32(p+0x814)==256 && get32(p+0x818)==32);
    CHECK(get64(p+0x820)==a+0x3000000);
    CHECK(p[0x3000000]==41 && p[0x3000000+256*32]==240 && p[0x3000000+256*32+1]==110);
    CHECK(p[0x3000000+256*48]==0xa5); /* no write beyond coded NV12 */
    put64(p+0x900,120);put64(p+0xa00,120);
    CHECK(call(&rt,"sceVdecswGetAvcPictureInfo",a+0x800,a+0x900,a+0xa00)==0);
    CHECK(p[0x908]==1 && p[0xa08]==0 && get64(p+0x910)==111 && get64(p+0x920)==77);
    CHECK(get32(p+0x92c)==0 && get32(p+0x930)==1 && p[0x935]==1 && get32(p+0x944)==14);
    /* Independently exercise the consumer's frame getters: coded height
     * selects UV start; height minus bottom cropping gives visible rows. */
    CHECK(get32(p+0x818)-get32(p+0x944)==18);
    CHECK(p[0x3000000+(uint64_t)get32(p+0x814)*get32(p+0x818)]==240);
    CHECK(call(&rt,"sceVdecswSetDecodeOutput",handle,a+0x600,0)==0);
    CHECK(call(&rt,"sceVdecswFinalizeDecodeSequence",handle,0,0)==0);
    CHECK(call(&rt,"sceVdecswTrySyncDecodeOutput",handle,a+0x800,0)==(int32_t)0x81510117 && !p[0x808]);
    CHECK(call(&rt,"sceVdecswTrySyncDecodeInput",handle,a+0xb00,0)==(int32_t)0x81510116 && get32(p+0xb10)==0);
    CHECK(call(&rt,"sceVdecswResetDecoder",handle,0,0)==0);
    CHECK(call(&rt,"sceVdecswTrySyncDecodeOutput",handle,a+0x800,0)==(int32_t)0x81510118);
    memcpy(p+0x1000,fixture,n);
    /* One picture held in scratch, then three pending inputs. */
    for(unsigned i=0;i<4;i++)CHECK(call(&rt,"sceVdecswSetDecodeInput",handle,a+0x700,0)==0);
    CHECK(call(&rt,"sceVdecswSetDecodeInput",handle,a+0x700,0)==(int32_t)0x81510114);
    CHECK(call(&rt,"sceVdecswTrySyncDecodeInput",handle,a+0xb00,0)==0 && get32(p+0xb10)==1);
    CHECK(call(&rt,"sceVdecswSetDecodeOutput",handle,a+0x600,0)==0);
    CHECK(call(&rt,"sceVdecswSetDecodeInput",handle,a+0x700,0)==0); /* retry accepted */
    CHECK(call(&rt,"sceVdecswResetDecoder",handle,0,0)==0);
    /* Eight synthetic AUs with two B-frames between references. A decoded
     * picture can arrive after a later input; EOS must drain every picture,
     * preserving that picture's original packet metadata. */
    f=fopen(argv[2],"rb");CHECK(f);n=fread(fixture,1,sizeof(fixture),f);fclose(f);
    CHECK(n>0 && n<sizeof(fixture));size_t boundary[9];unsigned count=0;
    for(size_t i=0;i+5<n;i++) {
        size_t prefix=!memcmp(fixture+i,"\0\0\0\1",4)?4:!memcmp(fixture+i,"\0\0\1",3)?3:0;
        if(prefix && (fixture[i+prefix]&31)==9){CHECK(count<8);boundary[count++]=i;i+=prefix;}
    }
    CHECK(count==8 && boundary[0]==0);boundary[8]=n;
    for(unsigned i=0;i<8;i++) {
        put64(p+0x608,a+0x3000000+i*0x10000);
        CHECK(call(&rt,"sceVdecswSetDecodeOutput",handle,a+0x600,0)==0);
        size_t size=boundary[i+1]-boundary[i];memcpy(p+0x1000+i*0x1000,fixture+boundary[i],size);
        put64(p+0x708,a+0x1000+i*0x1000);
        put64(p+0x710,size);put64(p+0x718,1000+i);put64(p+0x720,900+i);put64(p+0x728,2000+i);
        CHECK(call(&rt,"sceVdecswSetDecodeInput",handle,a+0x700,0)==0);
    }
    /* The producer waits for INPUT_END before Finalize: consume the last
     * completion, observe the idle queue while output is not finalized,
     * then release the two delayed B-frame pictures through Finalize. */
    unsigned produced=0;
    for(unsigned i=0;i<8;i++) {
        CHECK(call(&rt,"sceVdecswTrySyncDecodeInput",handle,a+0xb00,0)==0 && get64(p+0xb08)==a+0x1000+i*0x1000);
        CHECK(get32(p+0xb14)==0 && get32(p+0xb10)==(i<2?0u:1u));produced+=get32(p+0xb10);
    }
    CHECK(produced==6);
    CHECK(call(&rt,"sceVdecswTrySyncDecodeInput",handle,a+0xb00,0)==(int32_t)0x81510116 && get32(p+0xb10)==0);
    unsigned pictures=0,metadata=0;
    for(unsigned phase=0;phase<2;phase++) {
        if(phase){CHECK(pictures<8);CHECK(call(&rt,"sceVdecswFinalizeDecodeSequence",handle,0,0)==0);}
        for(;;) {
            int32_t status=call(&rt,"sceVdecswTrySyncDecodeOutput",handle,a+0x800,0);
            if(status){CHECK(status==(int32_t)(phase?0x81510118:0x81510117));break;}
            CHECK(!p[0x80a] && p[0x80b]==1 && get32(p+0x80c)==1);
            CHECK(p[0x809]==(phase && pictures==7)); /* only last real picture ends the sequence */
            CHECK(call(&rt,"sceVdecswGetAvcPictureInfo",a+0x800,a+0x900,a+0xa00)==0);
            uint64_t pts=get64(p+0x910),dts=get64(p+0x918),attached=get64(p+0x920);
            CHECK(pts>=1000 && pts<1008 && dts==pts-100 && attached==pts+1000);
            unsigned bit=1u<<(unsigned)(pts-1000);CHECK(!(metadata&bit));metadata|=bit;pictures++;
        }
    }
    CHECK(pictures==8 && metadata==255);
    CHECK(call(&rt,"sceVdecswResetDecoder",handle,0,0)==0);
    hle_vdecsw_reset(&other); /* another runtime cannot close this decoder */
    CHECK(call(&rt,"sceVdecswResetDecoder",handle,0,0)==0);
    CHECK(call(&rt,"sceVdecswDeleteDecoder",handle,0,0)==0);
    CHECK(call(&rt,"sceVdecswCreateDecoder",a+0x300,a+0x400,a+0x500)==0);
    handle=get64(p+0x500);
    f=fopen(argv[1],"rb");CHECK(f);n=fread(fixture,1,sizeof(fixture),f);fclose(f);
    CHECK(n>0 && n<sizeof(fixture));memcpy(p+0x1000,fixture,n);put64(p+0x708,a+0x1000);put64(p+0x710,n);
    for(unsigned i=0;i<4;i++)CHECK(call(&rt,"sceVdecswSetDecodeInput",handle,a+0x700,0)==0);
    CHECK(call(&rt,"sceVdecswSetDecodeInput",handle,a+0x700,0)==(int32_t)0x81510114);
    CHECK(call(&rt,"sceVdecswDeleteDecoder",handle,0,0)==0);
    CHECK(call(&rt,"sceVdecswDeleteDecoder",handle,0,0)!=0);
    CHECK(call(&rt,"sceVdecswCreateDecoder",a+0x300,a+0x400,a+0x500)==0);
    CHECK(get64(p+0x500)!=handle); /* stale handles cannot alias a new decoder */
    hle_vdecsw_reset(&rt);
    CHECK(call(&rt,"sceVdecswResetDecoder",get64(p+0x500),0,0)!=0);
    puts("Vdecsw: real AVC/NV12, queues, timestamps, SPS crop, EOS, reset, ownership and lifetime passed");
#endif
    hle_vdecsw_reset(&rt);hle_vdecsw_reset(&other);gmem_free(&rt.mem);gmem_free(&other.mem);pal_shutdown();return 0;
}
