/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "pal/module_paths.h"
#include "pal/pal.h"
#include "guest/elf_sce.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void path(char *out,const char *a,const char *b) {
    int n=snprintf(out,MODULE_PATH_CAP,"%s/%s",a,b);assert(n>0 && n<MODULE_PATH_CAP);
}
static void mkdir_at(char *out,const char *parent,const char *name) {path(out,parent,name);assert(pal_mkdir(out)==PS5C_OK);}
static void write_bytes(const char *name,const void *data,size_t size) {
    pal_file *f=NULL;uint64_t put=0;
    assert(pal_file_open(name,PAL_FILE_WRITE|PAL_FILE_CREATE|PAL_FILE_TRUNCATE,&f)==PS5C_OK);
    assert(pal_file_write(f,data,size,&put)==PS5C_OK && put==size);pal_file_close(f);
}
static void image(const char *directory,const char *name,bool self,bool valid) {
    unsigned char data[320]={0};uint64_t offset=0;
    if(self) {sce_self_header h={0};h.magic=SELF_MAGIC_PS5;h.num_segments=2;h.file_size=sizeof(data);memcpy(data,&h,sizeof(h));offset=sizeof(h)+2*sizeof(sce_self_segment);}
    elf64_ehdr h={0};memcpy(h.e_ident,"\177ELF",4);h.e_ident[4]=ELF_CLASS64;h.e_ident[5]=ELF_DATA_LSB;
    h.e_machine=valid?EM_X86_64:183;h.e_type=0xfe18;h.e_phoff=sizeof(h);h.e_phentsize=sizeof(elf64_phdr);h.e_phnum=1;
    memcpy(data+offset,&h,sizeof(h));char file[MODULE_PATH_CAP];path(file,directory,name);write_bytes(file,data,sizeof(data));
}
int main(int argc,char **argv) {
    assert(argc==2 && pal_init()==PS5C_OK);
    char root[MODULE_PATH_CAP],name[80];snprintf(name,sizeof(name),"case-%llx",(unsigned long long)pal_time_ns());mkdir_at(root,argv[1],name);
    char install[MODULE_PATH_CAP],peers[MODULE_PATH_CAP],peer[MODULE_PATH_CAP],system[MODULE_PATH_CAP],modules[MODULE_PATH_CAP];
    mkdir_at(install,root,"current");mkdir_at(peers,root,"nearby");mkdir_at(peer,peers,"ZeraPS5-older");
    mkdir_at(system,peer,"system");mkdir_at(modules,system,"modules");
    image(modules,"libSceAgc.sprx",true,true);image(modules,"libScePsml.prx",false,true);
    assert(module_path_probe(modules)==(MODULE_AGC|MODULE_PSML));
    module_path_options *o=calloc(1,sizeof(*o));assert(o);strcpy(o->install,install);strcpy(o->configured,"system/modules");
    strcpy(o->roots[0],peers);o->root_count=1;o->directory_limit=128;
    module_path_result r;
    assert(module_path_find(o,&r) && r.origin==MODULE_PATH_DISCOVERED && !strcmp(r.directory,modules));
    char hint[MODULE_PATH_CAP];path(hint,root,"location.path");strcpy(o->hint_file,hint);
    assert(module_path_remember(o,&r));o->root_count=0;
    assert(module_path_find(o,&r) && r.origin==MODULE_PATH_REMEMBERED && !strcmp(r.directory,modules));
    char custom[MODULE_PATH_CAP];mkdir_at(custom,root,"módulos");image(custom,"libSceAmpr.sprx",false,true);
    strcpy(o->configured,custom);
    assert(module_path_find(o,&r) && r.origin==MODULE_PATH_CONFIGURED && r.modules==MODULE_AMPR && !strcmp(r.directory,custom));
    assert(!module_path_remember(o,&r));
    char empty[MODULE_PATH_CAP];mkdir_at(empty,root,"intentional-empty");strcpy(o->configured,empty);
    assert(module_path_find(o,&r) && r.origin==MODULE_PATH_CONFIGURED && r.modules==0);
    strcpy(o->configured,"system/modules");o->directory_limit=1;
    assert(!module_path_find(o,&r) && r.directories_checked==1);
    o->directory_limit=128;
    char local_system[MODULE_PATH_CAP],local[MODULE_PATH_CAP];mkdir_at(local_system,install,"system");mkdir_at(local,local_system,"modules");
    image(local,"libSceAgc.sprx",false,false);
    assert(module_path_probe(local)==0);
    assert(module_path_find(o,&r) && r.origin==MODULE_PATH_REMEMBERED);
#ifdef _WIN32
    strcpy(o->configured,"SYSTEM\\MODULES");
    assert(module_path_find(o,&r) && r.origin==MODULE_PATH_REMEMBERED);
    strcpy(o->configured,"system/modules");
#endif
    image(local,"libSceAgc.sprx",false,true);
    assert(module_path_find(o,&r) && r.origin==MODULE_PATH_LOCAL && !strcmp(r.directory,local));
    char file[MODULE_PATH_CAP];path(file,local,"libSceAgc.sprx");assert(pal_file_remove(file)==PS5C_OK);
    const char bad_hint[]="//unavailable-server/private";write_bytes(hint,bad_hint,sizeof(bad_hint)-1);
    o->root_count=1;
    assert(module_path_find(o,&r) && r.origin==MODULE_PATH_DISCOVERED);
    char oversized[2048];memset(oversized,'x',sizeof(oversized));write_bytes(hint,oversized,sizeof(oversized));
    assert(module_path_find(o,&r) && r.origin==MODULE_PATH_DISCOVERED);
    const unsigned char truncated[]={0x54,0x14,0xf5,0xee};write_bytes(file,truncated,sizeof(truncated));
    assert(module_path_probe(local)==0);
    /* No broad scan of unrelated folders and no copying a set into current. */
    char unrelated[MODULE_PATH_CAP],usys[MODULE_PATH_CAP],umods[MODULE_PATH_CAP];mkdir_at(unrelated,peers,"Unrelated");
    mkdir_at(usys,unrelated,"system");mkdir_at(umods,usys,"modules");image(umods,"libSceAgc.sprx",false,true);
    path(file,modules,"libSceAgc.sprx");assert(pal_file_remove(file)==PS5C_OK);
    assert(!module_path_find(o,&r));
    /* A previous install's configuration can refer to a separate module set. */
    char user[MODULE_PATH_CAP],config[MODULE_PATH_CAP];mkdir_at(user,peer,"user");mkdir_at(config,user,"config");
    path(file,config,"zeraps5.ini");char text[MODULE_PATH_CAP+80];snprintf(text,sizeof(text),"[pastas]\nsistema=%s\n",umods);write_bytes(file,text,strlen(text));
    assert(module_path_find(o,&r) && !strcmp(r.directory,umods));
    path(file,local,"libScePsml.prx");pal_path_info info;assert(pal_path_stat(file,&info)==PS5C_ERR_NOT_FOUND);
    /* Local accumulation keeps complete, immutable sets instead of mixing
     * modules from different installations or replacing a working version. */
    path(o->collection_dir,install,"system/modules/collections");
    module_path_result source=r,collected;
    image(umods,"libSceAmpr.prx",false,true);image(umods,"libUnrelated.sprx",false,true);
    source.modules=module_path_probe(umods);
    assert(module_path_collect(o,&source,&collected) && collected.origin==MODULE_PATH_COLLECTION);
    assert(collected.modules==(MODULE_AGC|MODULE_AMPR));
    char first[MODULE_PATH_CAP];strcpy(first,collected.directory);
    path(file,first,"libUnrelated.sprx");assert(pal_path_stat(file,&info)==PS5C_ERR_NOT_FOUND);
    assert(module_path_collect(o,&source,&collected) && !strcmp(collected.directory,first));
    char original[MODULE_PATH_CAP];path(original,first,"libSceAgc.sprx");
    pal_file *f=NULL;uint8_t before[320],after[320];uint64_t got=0;
    assert(pal_file_open(original,PAL_FILE_READ,&f)==PS5C_OK);
    assert(pal_file_read(f,before,sizeof(before),&got)==PS5C_OK && got==sizeof(before));pal_file_close(f);
    memcpy(after,before,sizeof(after));after[319]=1;path(file,umods,"libSceAgc.sprx");write_bytes(file,after,sizeof(after));
    assert(module_path_collect(o,&source,&collected) && strcmp(collected.directory,first));
    assert(pal_file_open(original,PAL_FILE_READ,&f)==PS5C_OK);
    assert(pal_file_read(f,after,sizeof(after),&got)==PS5C_OK && got==sizeof(after));pal_file_close(f);
    assert(!memcmp(before,after,sizeof(before)));
    image(umods,"libScePsml.sprx",true,true);source.modules=module_path_probe(umods);
    assert(module_path_collect(o,&source,&collected) && collected.modules==(MODULE_AGC|MODULE_AMPR|MODULE_PSML));
    char last[MODULE_PATH_CAP];strcpy(last,collected.directory);
    o->root_count=0;
    assert(module_path_find(o,&r) && r.origin==MODULE_PATH_COLLECTION && !strcmp(r.directory,last));
    /* Another installation can discover this retained copy even when it has
     * no own collection or remembered source, using only a nearby install. */
    module_path_options *other=calloc(1,sizeof(*other));assert(other);
    strcpy(other->install,empty);strcpy(other->roots[0],root);other->root_count=1;
    char retained[MODULE_PATH_CAP],rsys[MODULE_PATH_CAP],rmods[MODULE_PATH_CAP];
    mkdir_at(retained,root,"ZeraPS5-retained");mkdir_at(rsys,retained,"system");mkdir_at(rmods,rsys,"modules");
    path(other->collection_dir,rmods,"collections");module_path_result retained_copy;
    assert(module_path_collect(other,&source,&retained_copy));
    other->collection_dir[0]=0;
    assert(module_path_find(other,&r) && r.origin==MODULE_PATH_DISCOVERED && !strcmp(r.directory,retained_copy.directory));
    free(other);
    /* Corrupt collections are not selected or silently repaired over user data. */
    path(file,last,"libSceAgc.sprx");write_bytes(file,truncated,sizeof(truncated));
    assert(!module_path_collect(o,&source,&collected));
    assert(pal_path_stat(file,&info)==PS5C_OK && info.size==sizeof(truncated));
    assert(!module_path_find(o,&r));
    path(o->collection_dir,root,"blocked-storage");write_bytes(o->collection_dir,"blocked",7);
    assert(!module_path_collect(o,&source,&collected));
    assert(!strcmp(collected.directory,source.directory));
    free(o);pal_shutdown();puts("Module discovery: explicit paths, ELF/SELF validation, shared hint, bounded peers and missing/corrupt candidates passed");return 0;
}
