/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pal/module_paths.h"
#include "pal/pal.h"
#include "base/ini.h"
#include "base/sha1.h"
#include "guest/elf_sce.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#endif

static bool copy_path(char *out,size_t cap,const char *path) {
    if(!path || strlen(path)>=cap || strpbrk(path,"\r\n"))return false;
    memmove(out,path,strlen(path)+1);
    for(char *p=out;*p;p++)if(*p=='\\')*p='/';
    size_t n=strlen(out);while(n>1 && out[n-1]=='/')out[--n]=0;
    return true;
}
static bool join_path(char *out,size_t cap,const char *root,const char *suffix) {
    int n=snprintf(out,cap,"%s%s%s",root,*root && root[strlen(root)-1]!='/'?"/":"",suffix);
    return n>=0 && (size_t)n<cap;
}
static bool absolute_path(const char *path) {
    return path && (path[0]=='/' || (isalpha((unsigned char)path[0]) && path[1]==':'));
}
static bool directory_exists(const char *path) {
    pal_path_info info;return pal_path_stat(path,&info)==PS5C_OK && info.is_dir;
}
static char *read_limited(const char *path,uint64_t limit) {
    pal_file *file=NULL;uint64_t size=0,got=0;char *text=NULL;
    if(pal_file_open(path,PAL_FILE_READ,&file)!=PS5C_OK)return NULL;
    if(pal_file_size(file,&size)==PS5C_OK && size>0 && size<=limit) {
        text=malloc((size_t)size+1);
        if(text && (pal_file_read(file,text,size,&got)!=PS5C_OK || got!=size)){free(text);text=NULL;}
        if(text)text[size]=0;
    }
    pal_file_close(file);return text;
}
static bool image_header(const char *path) {
    pal_file *file=NULL;uint64_t size=0,got=0,offset=0;
    if(pal_file_open(path,PAL_FILE_READ,&file)!=PS5C_OK)return false;
    sce_self_header self;elf64_ehdr elf;
    bool ok=pal_file_size(file,&size)==PS5C_OK && size>=sizeof(elf) && size<=64u*1024u*1024u &&
            pal_file_pread(file,&self,sizeof(self),0,&got)==PS5C_OK && got==sizeof(self);
    if(ok && (self.magic==SELF_MAGIC_PS4 || self.magic==SELF_MAGIC_PS5)) {
        ok=self.num_segments>0 && self.num_segments<=1024;
        offset=sizeof(self)+(uint64_t)self.num_segments*sizeof(sce_self_segment);
    }
    ok=ok && offset<=size && sizeof(elf)<=size-offset &&
       pal_file_pread(file,&elf,sizeof(elf),offset,&got)==PS5C_OK && got==sizeof(elf);
    if(ok) {
        ok=!memcmp(elf.e_ident,"\177ELF",4) && elf.e_ident[4]==ELF_CLASS64 && elf.e_ident[5]==ELF_DATA_LSB &&
           elf.e_machine==EM_X86_64 && elf.e_phentsize==sizeof(elf64_phdr) && elf.e_phnum>0 && elf.e_phnum<=1024 &&
           elf.e_phoff<=size-offset && (uint64_t)elf.e_phnum*sizeof(elf64_phdr)<=size-offset-elf.e_phoff;
    }
    pal_file_close(file);return ok;
}
uint32_t module_path_probe(const char *directory) {
    static const char *names[]={"libSceAgc","libSceAgcDriver","libSceAmpr","libScePsml"};
    uint32_t mask=0;char path[MODULE_PATH_CAP+64];
    for(unsigned n=0;n<4;n++)for(unsigned ext=0;ext<2;ext++) {
        int len=snprintf(path,sizeof(path),"%s/%s.%s",directory,names[n],ext?"prx":"sprx");
        if(len>0 && (size_t)len<sizeof(path) && image_header(path)){mask|=1u<<n;break;}
    }
    return mask;
}

/* SHA-1 is a cache identity, never a signature or an authenticity check. Files
 * copied in this transaction are also compared byte-for-byte with the source. */
typedef struct module_snapshot {char name[32];uint8_t digest[20];uint64_t size;} module_snapshot;
static void hex_digest(const uint8_t *bytes,char *out) {
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<20;i++){out[2*i]=hex[bytes[i]>>4];out[2*i+1]=hex[bytes[i]&15];}out[40]=0;
}
static bool file_digest(const char *path,uint8_t *digest,uint64_t *size) {
    pal_file *file=NULL;if(pal_file_open(path,PAL_FILE_READ,&file)!=PS5C_OK)return false;
    bool ok=pal_file_size(file,size)==PS5C_OK && *size && *size<=64u*1024u*1024u;
    uint8_t *buffer=ok?malloc(65536):NULL;ok=ok && buffer;
    sha1_ctx hash;sha1_init(&hash);uint64_t offset=0;
    while(ok && offset<*size) {
        uint64_t want=PS5C_MIN(*size-offset,65536),got=0;
        ok=pal_file_read(file,buffer,want,&got)==PS5C_OK && got==want;
        if(ok){sha1_update(&hash,buffer,(size_t)got);offset+=got;}
    }
    uint64_t after=0;if(ok)ok=pal_file_size(file,&after)==PS5C_OK && after==*size;
    if(ok)sha1_final(&hash,digest);
    free(buffer);pal_file_close(file);return ok;
}
static unsigned snapshot(const char *directory,module_snapshot *files,char *id) {
    static const char *names[]={"libSceAgc","libSceAgcDriver","libSceAmpr","libScePsml"};
    unsigned count=0;sha1_ctx hash;sha1_init(&hash);
    for(unsigned n=0;n<4;n++)for(unsigned ext=0;ext<2;ext++) {
        char path[MODULE_PATH_CAP];module_snapshot *f=&files[count];
        snprintf(f->name,sizeof(f->name),"%s.%s",names[n],ext?"prx":"sprx");
        if(!join_path(path,sizeof(path),directory,f->name) || !image_header(path))continue;
        if(!file_digest(path,f->digest,&f->size))return 0;
        sha1_update(&hash,f->name,strlen(f->name)+1);sha1_update(&hash,f->digest,20);
        char size[32];snprintf(size,sizeof(size),"%llu",(unsigned long long)f->size);
        sha1_update(&hash,size,strlen(size)+1);count++;break;
    }
    uint8_t digest[20];sha1_final(&hash,digest);hex_digest(digest,id);return count;
}
static bool collection_id(const char *id) {
    if(strlen(id)!=40)return false;
    for(unsigned n=0;n<40;n++)if(!strchr("0123456789abcdef",id[n]))return false;
    return true;
}
static bool collection_valid(const char *directory,const char *id) {
    if(!collection_id(id))return false;
    char path[MODULE_PATH_CAP];if(!join_path(path,sizeof(path),directory,"collection.id"))return false;
    char *marker=read_limited(path,40);bool ok=marker && !strcmp(marker,id);free(marker);
    if(!ok)return false;
    module_snapshot files[4];char actual[41];
    return snapshot(directory,files,actual)>0 && !strcmp(actual,id);
}
static bool write_small(const char *path,const char *text) {
    pal_file *file=NULL;if(pal_file_open(path,PAL_FILE_WRITE|PAL_FILE_CREATE|PAL_FILE_TRUNCATE,&file)!=PS5C_OK)return false;
    uint64_t put=0;size_t size=strlen(text);
    bool ok=pal_file_write(file,text,size,&put)==PS5C_OK && put==size;pal_file_close(file);return ok;
}
static bool mkdir_parents(const char *directory) {
    char path[MODULE_PATH_CAP];if(!copy_path(path,sizeof(path),directory) || !*path || !strncmp(path,"//",2))return false;
    for(char *c=path+1;*c;c++)if(*c=='/' && c[-1]!=':') {
        *c=0;pal_mkdir(path);bool ok=directory_exists(path);*c='/';if(!ok)return false;
    }
    pal_mkdir(path);return directory_exists(path);
}
static bool copy_verified(const char *source,const char *target,const module_snapshot *expected) {
    pal_file *in=NULL,*out=NULL;
    if(pal_file_open(source,PAL_FILE_READ,&in)!=PS5C_OK)return false;
    uint8_t *buffer=malloc(65536);bool ok=buffer && pal_file_open(target,PAL_FILE_WRITE|PAL_FILE_CREATE|PAL_FILE_TRUNCATE,&out)==PS5C_OK;
    uint64_t offset=0;sha1_ctx hash;sha1_init(&hash);
    while(ok && offset<expected->size) {
        uint64_t want=PS5C_MIN(expected->size-offset,65536),got=0,put=0;
        ok=pal_file_read(in,buffer,want,&got)==PS5C_OK && got==want &&
           pal_file_write(out,buffer,got,&put)==PS5C_OK && put==got;
        if(ok){sha1_update(&hash,buffer,(size_t)got);offset+=got;}
    }
    uint8_t digest[20];sha1_final(&hash,digest);uint64_t size=0;
    ok=ok && pal_file_size(in,&size)==PS5C_OK && size==expected->size && !memcmp(digest,expected->digest,20);
    if(out)pal_file_close(out);
    pal_file_close(in);free(buffer);return ok;
}
static bool files_equal(const char *source,const char *target) {
    pal_file *a=NULL,*b=NULL;uint64_t sa=0,sb=0;
    bool ok=pal_file_open(source,PAL_FILE_READ,&a)==PS5C_OK && pal_file_open(target,PAL_FILE_READ,&b)==PS5C_OK &&
            pal_file_size(a,&sa)==PS5C_OK && pal_file_size(b,&sb)==PS5C_OK && sa==sb;
    uint8_t *bytes=ok?malloc(131072):NULL;ok=ok && bytes;
    for(uint64_t offset=0;ok && offset<sa;) {
        uint64_t want=PS5C_MIN(sa-offset,65536),ga=0,gb=0;
        ok=pal_file_read(a,bytes,want,&ga)==PS5C_OK && ga==want &&
           pal_file_read(b,bytes+65536,want,&gb)==PS5C_OK && gb==want && !memcmp(bytes,bytes+65536,(size_t)want);
        offset+=want;
    }
    if(a)pal_file_close(a);
    if(b)pal_file_close(b);
    free(bytes);return ok;
}
static bool same_snapshot(const char *source,const char *target,const module_snapshot *files,unsigned count) {
    for(unsigned n=0;n<count;n++) {
        char a[MODULE_PATH_CAP],b[MODULE_PATH_CAP];
        if(!join_path(a,sizeof(a),source,files[n].name) || !join_path(b,sizeof(b),target,files[n].name) || !files_equal(a,b))return false;
    }
    return true;
}
bool module_path_collect(const module_path_options *o,const module_path_result *source,module_path_result *result) {
    *result=*source;
    if(!*o->collection_dir || !source->modules)return false;
    module_snapshot files[4];char id[41],target[MODULE_PATH_CAP];unsigned count=snapshot(source->directory,files,id);
    if(!count || !join_path(target,sizeof(target),o->collection_dir,id))return false;
    if(!collection_valid(target,id)) {
        /* An existing path is never repaired or overwritten automatically. */
        pal_path_info info;if(pal_path_stat(target,&info)!=PS5C_ERR_NOT_FOUND || !mkdir_parents(o->collection_dir))return false;
        char temp[MODULE_PATH_CAP],suffix[80];snprintf(suffix,sizeof(suffix),".import-%llx",(unsigned long long)pal_time_ns());
        if(!join_path(temp,sizeof(temp),o->collection_dir,suffix) || pal_mkdir(temp)!=PS5C_OK)return false;
        bool ok=true;
        for(unsigned n=0;n<count && ok;n++) {
            char a[MODULE_PATH_CAP],b[MODULE_PATH_CAP];
            ok=join_path(a,sizeof(a),source->directory,files[n].name) && join_path(b,sizeof(b),temp,files[n].name) && copy_verified(a,b,&files[n]);
        }
        char marker[MODULE_PATH_CAP];
        ok=ok && same_snapshot(source->directory,temp,files,count) && join_path(marker,sizeof(marker),temp,"collection.id") && write_small(marker,id) && collection_valid(temp,id);
        if(ok) {
            /* A concurrent collector may have published the same complete set. */
            if(pal_file_rename(temp,target)!=PS5C_OK)ok=collection_valid(target,id) && same_snapshot(source->directory,target,files,count);
        }
        /* Only transaction-owned files are removed; no recursive deletion. */
        for(unsigned n=0;n<count;n++)if(join_path(marker,sizeof(marker),temp,files[n].name))pal_file_remove(marker);
        if(join_path(marker,sizeof(marker),temp,"collection.id"))pal_file_remove(marker);
        pal_rmdir(temp);if(!ok)return false;
    }
    if(!same_snapshot(source->directory,target,files,count))return false;
    char current[MODULE_PATH_CAP],temporary[MODULE_PATH_CAP],suffix[80];
    snprintf(suffix,sizeof(suffix),".current-%llx",(unsigned long long)pal_time_ns());
    if(join_path(current,sizeof(current),o->collection_dir,"current.path") && join_path(temporary,sizeof(temporary),o->collection_dir,suffix)) {
        if(write_small(temporary,id))pal_file_rename(temporary,current);
        pal_file_remove(temporary);
    }
    copy_path(result->directory,sizeof(result->directory),target);result->modules=module_path_probe(target);result->origin=MODULE_PATH_COLLECTION;
    return true;
}
static bool find_collection_dir(const char *directory,module_path_result *result) {
    char pointer[MODULE_PATH_CAP],path[MODULE_PATH_CAP];
    if(!*directory || !join_path(pointer,sizeof(pointer),directory,"current.path"))return false;
    char *id=read_limited(pointer,40);bool ok=id && collection_id(id) && join_path(path,sizeof(path),directory,id) && collection_valid(path,id);free(id);
    if(!ok)return false;
    copy_path(result->directory,sizeof(result->directory),path);result->modules=module_path_probe(path);result->origin=MODULE_PATH_COLLECTION;return true;
}
typedef struct search_state {
    const module_path_options *options;
    module_path_result *result;
    uint32_t checked,limit;
} search_state;
static bool accept(search_state *s,const char *path,module_path_origin origin,bool explicit_folder) {
    if(s->checked>=s->limit || !*path)return false;
    s->checked++;
    uint32_t mask=module_path_probe(path);
    if(!(mask&MODULE_AGC) && !(explicit_folder && directory_exists(path)))return false;
    if(!copy_path(s->result->directory,sizeof(s->result->directory),path))return false;
    s->result->origin=origin;s->result->modules=mask;return true;
}
static bool candidate_install(search_state *s,const char *folder) {
    char path[MODULE_PATH_CAP];
    if(join_path(path,sizeof(path),folder,"system/modules") && accept(s,path,MODULE_PATH_DISCOVERED,false))return true;
    /* A previous installation may have retained its own complete collection
     * after the original disk was removed. Never inspect transaction folders. */
    if(strcmp(folder,s->options->install) && join_path(path,sizeof(path),folder,"system/modules/collections")) {
        module_path_result cached={0};
        if(find_collection_dir(path,&cached) && accept(s,cached.directory,MODULE_PATH_DISCOVERED,false))return true;
    }
    /* Existing installations can point to a separate local module directory.
     * Read their preference only; never change that installation or its files. */
    if(s->checked>=s->limit || !join_path(path,sizeof(path),folder,"user/config/zeraps5.ini"))return false;
    pal_path_info info;if(pal_path_stat(path,&info)!=PS5C_OK || info.is_dir || info.size>65536)return false;
    char *bytes=read_limited(path,65536);if(!bytes)return false;
    ini *doc=ini_parse(bytes,strlen(bytes));free(bytes);
    const char *value=ini_get(doc,"pastas","sistema");char normalized[MODULE_PATH_CAP];bool found=false;
    if(value && copy_path(normalized,sizeof(normalized),value) && strncmp(normalized,"//",2)) {
        if(absolute_path(normalized))found=accept(s,normalized,MODULE_PATH_DISCOVERED,false);
        else if(join_path(path,sizeof(path),folder,normalized))found=accept(s,path,MODULE_PATH_DISCOVERED,false);
    }
    ini_free(doc);return found;
}
typedef struct walk_state { search_state *search;const char *root;unsigned depth;unsigned entries; } walk_state;
static bool install_name(const char *name) {
    const char *prefix="zeraps5";
    for(unsigned i=0;prefix[i];i++)if(tolower((unsigned char)name[i])!=prefix[i])return false;
    return true;
}
static bool walk_installs(void *user,const char *name,bool is_dir) {
    walk_state *w=user;search_state *s=w->search;
    if(++w->entries>256 || s->checked>=s->limit || s->result->origin!=MODULE_PATH_NONE)return false;
    if(!is_dir || !install_name(name))return true;
    char path[MODULE_PATH_CAP];if(!join_path(path,sizeof(path),w->root,name))return true;
    if(candidate_install(s,path))return false;
    if(w->depth) {walk_state child={s,path,w->depth-1,0};pal_dir_list(path,walk_installs,&child);}
    return s->result->origin==MODULE_PATH_NONE;
}
bool module_path_find(const module_path_options *o,module_path_result *result) {
    memset(result,0,sizeof(*result));search_state s={o,result,0,o->directory_limit?o->directory_limit:128};
    char path[MODULE_PATH_CAP],configured[MODULE_PATH_CAP];
    if(!copy_path(configured,sizeof(configured),*o->configured?o->configured:"system/modules"))return false;
#ifdef _WIN32
    bool local=!_stricmp(configured,"system/modules") || !_stricmp(configured,"./system/modules");
#else
    bool local=!strcmp(configured,"system/modules") || !strcmp(configured,"./system/modules");
#endif
    if(absolute_path(configured))copy_path(path,sizeof(path),configured);
    else if(!join_path(path,sizeof(path),o->install,configured))return false;
    if(accept(&s,path,local?MODULE_PATH_LOCAL:MODULE_PATH_CONFIGURED,!local))goto done;
    if(*o->hint_file) {
        pal_path_info info;
        if(pal_path_stat(o->hint_file,&info)==PS5C_OK && !info.is_dir && info.size>0 && info.size<MODULE_PATH_CAP) {
            char *hint=read_limited(o->hint_file,MODULE_PATH_CAP-1);
            if(hint) {
                hint[strcspn(hint,"\r\n")]=0;
                bool valid=copy_path(path,sizeof(path),hint);free(hint);
                if(valid && absolute_path(path) && strncmp(path,"//",2) &&
                   accept(&s,path,MODULE_PATH_REMEMBERED,false))goto done;
            }
        }
    }
    for(uint32_t i=0;i<o->root_count && i<MODULE_SEARCH_ROOTS && s.checked<s.limit;i++) {
        if(!*o->roots[i] || !strncmp(o->roots[i],"//",2))continue;
        if(candidate_install(&s,o->roots[i]))break;
        walk_state walk={&s,o->roots[i],1,0};pal_dir_list(o->roots[i],walk_installs,&walk);
        if(result->origin!=MODULE_PATH_NONE)break;
    }
    if(result->origin==MODULE_PATH_NONE)find_collection_dir(o->collection_dir,result);
done:
    result->directories_checked=s.checked;return result->origin!=MODULE_PATH_NONE;
}
static void add_root(module_path_options *o,const char *root) {
    if(o->root_count>=MODULE_SEARCH_ROOTS)return;
    char normalized[MODULE_PATH_CAP];if(!copy_path(normalized,sizeof(normalized),root))return;
    for(uint32_t i=0;i<o->root_count;i++)if(!strcmp(o->roots[i],normalized))return;
    copy_path(o->roots[o->root_count++],MODULE_PATH_CAP,normalized);
}
void module_path_host_options(const char *install,const char *configured,module_path_options *o) {
    memset(o,0,sizeof(*o));o->directory_limit=128;
    char cwd[MODULE_PATH_CAP]="",profile[MODULE_PATH_CAP]="";
#ifdef _WIN32
    wchar_t wide[MODULE_PATH_CAP];
    DWORD cwd_length=GetCurrentDirectoryW(MODULE_PATH_CAP,wide);
    if(cwd_length && cwd_length<MODULE_PATH_CAP)WideCharToMultiByte(CP_UTF8,0,wide,-1,cwd,sizeof(cwd),NULL,NULL);
    if(SUCCEEDED(SHGetFolderPathW(NULL,CSIDL_LOCAL_APPDATA,NULL,SHGFP_TYPE_CURRENT,wide)))
        WideCharToMultiByte(CP_UTF8,0,wide,-1,profile,sizeof(profile),NULL,NULL);
#else
    if(!getcwd(cwd,sizeof(cwd)))cwd[0]=0;
    const char *home=getenv("XDG_DATA_HOME");
    if(home && *home)copy_path(profile,sizeof(profile),home);
    else if((home=getenv("HOME"))!=NULL)join_path(profile,sizeof(profile),home,".local/share");
#endif
    copy_path(o->install,sizeof(o->install),install && *install?install:cwd);
    copy_path(o->configured,sizeof(o->configured),configured && *configured?configured:"system/modules");
    join_path(o->collection_dir,sizeof(o->collection_dir),o->install,"system/modules/collections");
    char path[MODULE_PATH_CAP];
    if(*profile && join_path(path,sizeof(path),profile,"ZeraPS5/system-modules.path"))
        copy_path(o->hint_file,sizeof(o->hint_file),path);
    copy_path(path,sizeof(path),o->install);
    for(unsigned parent=0;parent<3;parent++) {
        add_root(o,path);char *slash=strrchr(path,'/');if(!slash || slash==path)break;*slash=0;
    }
#ifdef _WIN32
    wchar_t drives[256];DWORD count=GetLogicalDriveStringsW(256,drives);
    if(count && count<256)for(const wchar_t *d=drives;*d;d+=wcslen(d)+1) {
        if(GetDriveTypeW(d)!=DRIVE_FIXED)continue;
        char drive[8];if(!WideCharToMultiByte(CP_UTF8,0,d,-1,drive,sizeof(drive),NULL,NULL))continue;
        for(char *c=drive;*c;c++)if(*c=='\\')*c='/';
        if(join_path(path,sizeof(path),drive,"ps5"))add_root(o,path);
        if(join_path(path,sizeof(path),drive,"ZeraPS5"))add_root(o,path);
    }
#endif
}
bool module_path_remember(const module_path_options *o,const module_path_result *result) {
    if(!*o->hint_file || !(result->modules&MODULE_AGC) || !absolute_path(result->directory) || !strncmp(result->directory,"//",2))return false;
    char parent[MODULE_PATH_CAP];if(!copy_path(parent,sizeof(parent),o->hint_file))return false;
    char *slash=strrchr(parent,'/');if(!slash)return false;*slash=0;pal_mkdir(parent);
    char temporary[MODULE_PATH_CAP];int n=snprintf(temporary,sizeof(temporary),"%s.tmp-%llx",o->hint_file,(unsigned long long)pal_time_ns());
    if(n<0 || (size_t)n>=sizeof(temporary))return false;
    pal_file *file=NULL;if(pal_file_open(temporary,PAL_FILE_WRITE|PAL_FILE_CREATE|PAL_FILE_TRUNCATE,&file)!=PS5C_OK)return false;
    uint64_t put=0;size_t len=strlen(result->directory);
    bool ok=pal_file_write(file,result->directory,len,&put)==PS5C_OK && put==len;pal_file_close(file);
    if(ok)ok=pal_file_rename(temporary,o->hint_file)==PS5C_OK;
    if(!ok)pal_file_remove(temporary);
    return ok;
}
bool module_path_resolve(const char *install,const char *configured,bool remember,module_path_result *result) {
    module_path_options *o=malloc(sizeof(*o));if(!o){memset(result,0,sizeof(*result));return false;}
    module_path_host_options(install,configured,o);bool found=module_path_find(o,result);
    if(found && remember) {
        if(result->origin!=MODULE_PATH_COLLECTION)(void)module_path_remember(o,result);
        if(result->origin!=MODULE_PATH_COLLECTION && result->modules) {
            module_path_result local;
            if(module_path_collect(o,result,&local))*result=local;
            else result->copy_failed=true;
        }
    }
    free(o);return found;
}
