/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/runtime.h"
#include "hle/hle.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"save convert line%d: %s\n",__LINE__,#x);return 1;}}while(0)
static int32_t call(ps5_runtime *rt,guest_addr request) {
 const hle_export *e=hle_find("libSceSaveData_native",nid_hash_name("sceSaveDataConvert"));
 if(!e || !e->fn)return INT32_MIN;
 hle_args args={.gpr={request}};hle_call c={.rt=rt,.mem=&rt->mem,.a=&args};e->fn(&c);return (int32_t)args.ret[0];
}
int main(void) {
 CHECK(pal_init()==PS5C_OK && hle_registry_init()==PS5C_OK);
 ps5_runtime *rt=calloc(1,sizeof(*rt));CHECK(rt);module_set modules;module_set_init(&modules);rt->ms=&modules;
 gmem_config config=gmem_default_config();config.identity=false;CHECK(gmem_init(&rt->mem,&config)==PS5C_OK);
 guest_addr page;CHECK(gmem_map(&rt->mem,0,GUEST_PAGE_SIZE,GPROT_RW,0,"save conversion fixture",&page)==PS5C_OK);
 uint8_t request[64];memset(request,0xa5,sizeof(request));uint32_t uid=1;memcpy(request,&uid,4);
 guest_addr name_at=page+256;memcpy(request+8,&name_at,8);guest_addr req=page+1;
 CHECK(gmem_write(&rt->mem,req,request,sizeof(request)));
 char name[32]="missing-convert-fixture";CHECK(gmem_write(&rt->mem,name_at,name,sizeof(name)));
 CHECK((uint32_t)call(rt,0)==0x809f0000u && (uint32_t)call(rt,page+GUEST_PAGE_SIZE-8)==0x809f0000u);
 CHECK((uint32_t)call(rt,req)==0x809f0008u);
 char path[400];snprintf(path,sizeof(path),"%s/savedata/%s",hle_user_dir(rt),name);pal_path_info missing;
 CHECK(pal_path_stat(path,&missing)==PS5C_ERR_NOT_FOUND); /* lookup does not create the requested save */
 const char *bad[]={"",".","..","bad/name","bad\\name","bad:name"};
 for(unsigned i=0;i<PS5C_ARRAY_COUNT(bad);i++) {
  memset(name,0,sizeof(name));memcpy(name,bad[i],strlen(bad[i]));CHECK(gmem_write(&rt->mem,name_at,name,sizeof(name)));
  CHECK((uint32_t)call(rt,req)==0x809f0000u);
 }
 memset(name,'x',sizeof(name));CHECK(gmem_write(&rt->mem,name_at,name,sizeof(name)) && (uint32_t)call(rt,req)==0x809f0000u);
 uint64_t invalid=UINT64_MAX;memcpy(request+8,&invalid,8);CHECK(gmem_write(&rt->mem,req,request,sizeof(request)) && (uint32_t)call(rt,req)==0x809f0000u);
 memcpy(request+8,&name_at,8);CHECK(gmem_write(&rt->mem,req,request,sizeof(request)));
 memset(name,0,sizeof(name));strcpy(name,"existing-convert-fixture");CHECK(gmem_write(&rt->mem,name_at,name,sizeof(name)));
 snprintf(path,sizeof(path),"%s/savedata/%s",hle_user_dir(rt),name);
 ps5c_err made=pal_mkdir(path);CHECK(made==PS5C_OK || (made==PS5C_ERR_BUSY && pal_path_stat(path,&missing)==PS5C_OK && missing.is_dir));
 char file[440];snprintf(file,sizeof(file),"%s/canary.bin",path);pal_file *f;
 const uint8_t canary[]={0x91,0,0xff,0x42,0x17};uint64_t bytes=0;
 CHECK(pal_file_open(file,PAL_FILE_WRITE|PAL_FILE_CREATE|PAL_FILE_TRUNCATE,&f)==PS5C_OK);
 CHECK(pal_file_write(f,canary,sizeof(canary),&bytes)==PS5C_OK && bytes==sizeof(canary));pal_file_close(f);
 pal_path_info before,after;CHECK(pal_path_stat(file,&before)==PS5C_OK && (uint32_t)call(rt,req)==0x809f001eu && pal_path_stat(file,&after)==PS5C_OK);
 CHECK((uint32_t)call(rt,req)==0x809f001eu); /* reopening must not start a phantom conversion */
 CHECK(before.size==after.size && before.mtime_sec==after.mtime_sec && before.mtime_nsec==after.mtime_nsec);
 uint8_t readback[sizeof(canary)];CHECK(pal_file_open(file,PAL_FILE_READ,&f)==PS5C_OK);
 CHECK(pal_file_read(f,readback,sizeof(readback),&bytes)==PS5C_OK && bytes==sizeof(readback));pal_file_close(f);CHECK(!memcmp(canary,readback,sizeof(canary)));
 uint8_t unchanged[64];CHECK(gmem_read(&rt->mem,req,unchanged,sizeof(unchanged)) && !memcmp(request,unchanged,sizeof(request)));
 memset(name,0,sizeof(name));strcpy(name,"convert-is-a-file");CHECK(gmem_write(&rt->mem,name_at,name,sizeof(name)));
 snprintf(file,sizeof(file),"%s/savedata/%s",hle_user_dir(rt),name);
 CHECK(pal_file_open(file,PAL_FILE_WRITE|PAL_FILE_CREATE|PAL_FILE_TRUNCATE,&f)==PS5C_OK);
 CHECK(pal_file_write(f,canary,sizeof(canary),&bytes)==PS5C_OK && bytes==sizeof(canary));pal_file_close(f);
 CHECK((uint32_t)call(rt,req)==0x809f0008u && pal_path_stat(file,&after)==PS5C_OK && after.size==sizeof(canary) && !after.is_dir);
 memset(name,0,sizeof(name));strcpy(name,"existing-convert-fixture/canary");CHECK(gmem_write(&rt->mem,name_at,name,sizeof(name)) && (uint32_t)call(rt,req)==0x809f0000u);
 gmem_free(&rt->mem);module_set_free(&modules);free(rt);hle_registry_free();pal_shutdown();
 puts("save conversion: missing/invalid requests report errors, existing host data and request bytes preserved");return 0;
}
