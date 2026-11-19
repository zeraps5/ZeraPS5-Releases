/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/gpu.h"
#include "gpu/lod_stats.h"
#include "gpu/gpu_regs.h"
#include "pal/pal.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"LOD failed line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static gpu_lod_stats stats;
static bool enabled=true;
static unsigned writes, samples;
static bool sample(void*u,uint64_t*v){(void)u;if(!enabled)return false;if(v)*v=123+samples++;return true;}
static bool report(void*u,void*out,uint32_t bytes,uint32_t flags){(void)u;return enabled && gpu_lod_report(&stats,out,bytes,flags);}
static void written(void*u,guest_addr addr,uint64_t bytes){(void)u;(void)addr;(void)bytes;writes++;}
static uint64_t word(const uint8_t*p,unsigned id){uint64_t v;memcpy(&v,p+64+id*8,8);return v;}
int main(void){
 uint32_t desc[8]={0};uint8_t out[GPU_LOD_REPORT_BYTES+16];
 desc[6]=255;gpu_lod_note(&stats,desc);CHECK(stats.bound[3]==0);
 desc[5]=1u<<25;gpu_lod_note(&stats,desc);desc[6]=0;gpu_lod_note(&stats,desc);
 memset(out,0xa5,sizeof(out));CHECK(gpu_lod_report(&stats,out+8,GPU_LOD_REPORT_BYTES,0));
 CHECK(out[0]==0xa5 && out[7]==0xa5 && out[sizeof(out)-1]==0xa5);
 CHECK(word(out+8,0)==1 && word(out+8,255)==1 && word(out+8,1)==(UINT64_C(15)<<56));
 CHECK(stats.bound[0]==1 && stats.bound[3]==(UINT64_C(1)<<63));
 CHECK(!gpu_lod_report(&stats,NULL,8,GPU_LOD_REPORT_RESET) && stats.bound[0]==1);
 CHECK(!gpu_lod_report(&stats,out,GPU_LOD_REPORT_BYTES+1,GPU_LOD_REPORT_RESET) && stats.bound[0]==1);
 CHECK(gpu_lod_report(&stats,out,67,0) && stats.bound[0]==1);
 CHECK(gpu_lod_report(&stats,out,GPU_LOD_REPORT_BYTES,GPU_LOD_REPORT_RESET) && word(out,0)==1 && stats.bound[0]==0);
 gpu_lod_note(&stats,desc);CHECK(gpu_lod_report(&stats,NULL,0,GPU_LOD_FORCE_RESET) && stats.bound[0]==0);
 CHECK(pal_init()==PS5C_OK);gmem mem;gmem_config cfg=gmem_default_config();cfg.identity=false;CHECK(gmem_init(&mem,&cfg)==PS5C_OK);
 guest_addr a;CHECK(gmem_map(&mem,0,GUEST_PAGE_SIZE,GPROT_RW,0,"LOD fixture",&a)==PS5C_OK);
 gpu g;gpu_callbacks cb={.mem_written=written,.lod_stats=report,.sample_counter=sample};CHECK(gpu_init(&g,&mem,&cb)==PS5C_OK);
 uint8_t poison[GPU_LOD_REPORT_BYTES+16];memset(poison,0xa5,sizeof(poison));CHECK(gmem_write(&mem,a+0x100,poison,sizeof(poison)));
 const uint32_t packet[]={0xc0038e00,GPU_LOD_REPORT_BYTES,(uint32_t)(a+0x140),(uint32_t)((a+0x140)>>32),GPU_LOD_REPORT_RESET};
 CHECK(gmem_write(&mem,a,packet,sizeof(packet)));gpu_lod_note(&stats,desc);gpu_submit(&g,a,5,0);
 CHECK(gmem_read(&mem,a+0x140,out,GPU_LOD_REPORT_BYTES));uint32_t ready;memcpy(&ready,out,4);
 CHECK(ready==1 && word(out,0)==1 && word(out,255)==(UINT64_C(15)<<56) && writes==1 && stats.bound[0]==0);
 CHECK(gmem_read(&mem,a+0x13f,out,1) && out[0]==0xa5);
 enabled=false;memset(out,0xa5,GPU_LOD_REPORT_BYTES);CHECK(gmem_write(&mem,a+0x140,out,GPU_LOD_REPORT_BYTES));
 gpu_submit(&g,a,5,0);CHECK(gmem_read(&mem,a+0x140,out,4) && out[0]==0xa5 && writes==1);
 enabled=true;uint32_t bad[]={0xc0038e00,GPU_LOD_REPORT_BYTES,0,0xffff, GPU_LOD_REPORT_RESET};gpu_lod_note(&stats,desc);
 CHECK(gmem_write(&mem,a,bad,sizeof(bad)));gpu_submit(&g,a,5,0);CHECK(stats.bound[0]==1 && writes==1);
 uint8_t block[272];memset(block,0xa5,sizeof(block));CHECK(gmem_write(&mem,a+0x1000,block,sizeof(block)));
 uint32_t dump[]={0xc0024600,0x139,(uint32_t)(a+0x1000),(uint32_t)((a+0x1000)>>32)};
 CHECK(gmem_write(&mem,a,dump,sizeof(dump)));gpu_submit(&g,a,4,0);
 dump[2]+=8;CHECK(gmem_write(&mem,a,dump,sizeof(dump)));gpu_submit(&g,a,4,0);
 CHECK(gmem_read(&mem,a+0x1000,block,sizeof(block)));
 for(unsigned db=0;db<16;db++) {
  uint64_t begin,end;memcpy(&begin,block+db*16,8);memcpy(&end,block+db*16+8,8);
  CHECK((begin>>63)==1 && (end>>63)==1 && (end-begin)==(db?0:1));
 }
 CHECK(samples==2 && block[256]==0xa5 && block[271]==0xa5);
 enabled=false;gpu_submit(&g,a,4,0);CHECK(samples==2);
 enabled=true;dump[2]++;CHECK(gmem_write(&mem,a,dump,sizeof(dump)));gpu_submit(&g,a,4,0);CHECK(samples==2);
 puts("Occlusion: real callback counts, 16 interleaved DB pairs, ready bits, label boundary, disabled/alignment checks passed");
 puts("LOD: bounds, texture flags, report/reset, CP publication, invalid address and disabled mode passed");return 0;
}
