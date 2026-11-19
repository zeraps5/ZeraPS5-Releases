/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pal_win32/report_archive.h"
#include <stdio.h>
#include <string.h>
static uint32_t crc32(const unsigned char *data,size_t size,uint32_t crc) {
    for(size_t n=0;n<size;n++) {
        crc^=data[n];
        for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^(0xedb88320u&((uint32_t)0-(crc&1)));
    }
    return crc;
}
static void put16(unsigned char *p,unsigned n){p[0]=(unsigned char)n;
    p[1]=(unsigned char)(n>>8);
}
static void put32(unsigned char *p,uint32_t n){put16(p,n&65535u);
    put16(p+2,n>>16);
}
static bool valid(const report_archive_entry *e,unsigned count) {
    if(!count||count>REPORT_ARCHIVE_FILES)return false;
    for(unsigned i=0;i<count;i++) {
        if(!memchr(e[i].name,0,sizeof(e[i].name))||!e[i].name[0]||e[i].name[0]=='.'||e[i].size>64u*1024u*1024u)return false;
        for(const char *p=e[i].name;*p;p++)if(!((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||strchr("._-",*p)))return false;
        for(unsigned j=0;j<i;j++)if(!strcmp(e[j].name,e[i].name))return false;
    }
    return true;
}
bool report_archive_prepare(report_archive_entry *e,unsigned count) {
    if(!valid(e,count))return false;
    unsigned char data[32768];
    for(unsigned i=0;i<count;i++) {
        FILE *f=_wfopen(e[i].path,L"rb");
        if(!f)return false;
        uint64_t size=0;
        uint32_t crc=UINT32_MAX;
        size_t n;
        while((n=fread(data,1,sizeof(data),f))!=0) {
            size+=n;
            if(size>64u*1024u*1024u){fclose(f);
                return false;
            }
            crc=crc32(data,n,crc);
        }
        bool ok=!ferror(f);
        if(fclose(f))ok=false;
        if(!ok)return false;
        e[i].size=(uint32_t)size;
        e[i].crc=crc^UINT32_MAX;
    }
    return true;
}
static bool write_bytes(FILE *f,const void *data,size_t n){return fwrite(data,1,n,f)==n;
}
static void local_header(unsigned char *h,const report_archive_entry *e) {
    memset(h,0,30);
    put32(h,0x04034b50);
    put16(h+4,20);
    put16(h+12,0x21);
    put32(h+14,e->crc);
    put32(h+18,e->size);
    put32(h+22,e->size);
    put16(h+26,(unsigned)strlen(e->name));
}
static void central_header(unsigned char *h,const report_archive_entry *e) {
    memset(h,0,46);
    put32(h,0x02014b50);
    put16(h+4,20);
    put16(h+6,20);
    put16(h+14,0x21);
    put32(h+16,e->crc);
    put32(h+20,e->size);
    put32(h+24,e->size);
    put16(h+28,(unsigned)strlen(e->name));
    put32(h+42,e->offset);
}
bool report_archive_write(const wchar_t *path,report_archive_entry *e,unsigned count) {
    if(!valid(e,count))return false;
    FILE *out=_wfopen(path,L"wb");
    if(!out)return false;
    unsigned char header[46],data[32768];
    bool ok=true;
    for(unsigned i=0;i<count&&ok;i++) {
        long offset=ftell(out);
        if(offset<0){ok=false;
            break;
        }e[i].offset=(uint32_t)offset;
        local_header(header,&e[i]);
        ok=write_bytes(out,header,30)&&write_bytes(out,e[i].name,strlen(e[i].name));
        FILE *in=_wfopen(e[i].path,L"rb");
        if(!in){ok=false;
            break;
        }
        uint32_t size=0,crc=UINT32_MAX;
        size_t n;
        while(ok&&(n=fread(data,1,sizeof(data),in))!=0) {
            if(n>e[i].size-size){ok=false;
                break;
            }
            size+=(uint32_t)n;
            crc=crc32(data,n,crc);
            ok=write_bytes(out,data,n);
        }
        if(ferror(in)||size!=e[i].size||(crc^UINT32_MAX)!=e[i].crc)ok=false;
        if(fclose(in))ok=false;
    }
    long center=ftell(out);
    if(center<0)ok=false;
    for(unsigned i=0;i<count&&ok;i++) {
        central_header(header,&e[i]);
        ok=write_bytes(out,header,46)&&write_bytes(out,e[i].name,strlen(e[i].name));
    }
    long end=ftell(out);
    if(end<0)ok=false;
    if(ok) {
        memset(header,0,22);
        put32(header,0x06054b50);
        put16(header+8,count);
        put16(header+10,count);
        put32(header+12,(uint32_t)(end-center));
        put32(header+16,(uint32_t)center);
        ok=write_bytes(out,header,22);
    }
    if(fflush(out)||ferror(out))ok=false;
    if(fclose(out))ok=false;
    return ok;
}
bool report_archive_verify(const wchar_t *path,const report_archive_entry *e,unsigned count) {
    if(!valid(e,count))return false;
    FILE *f=_wfopen(path,L"rb");
    if(!f)return false;
    unsigned char actual[46],expected[46],data[32768];
    char name[64];
    bool ok=true;
    for(unsigned i=0;i<count&&ok;i++) {
        if(ftell(f)!=(long)e[i].offset){ok=false;
            break;
        }
        local_header(expected,&e[i]);
        size_t len=strlen(e[i].name);
        ok=fread(actual,1,30,f)==30&&!memcmp(actual,expected,30)&&fread(name,1,len,f)==len&&!memcmp(name,e[i].name,len);
        uint32_t left=e[i].size,crc=UINT32_MAX;
        while(ok&&left){size_t n=left<sizeof(data)?left:sizeof(data);
            ok=fread(data,1,n,f)==n;
            if(ok){crc=crc32(data,n,crc);
                left-=(uint32_t)n;
            }}
        if((crc^UINT32_MAX)!=e[i].crc)ok=false;
    }
    long center=ftell(f);
    if(center<0)ok=false;
    for(unsigned i=0;i<count&&ok;i++) {
        central_header(expected,&e[i]);
        size_t len=strlen(e[i].name);
        ok=fread(actual,1,46,f)==46&&!memcmp(actual,expected,46)&&fread(name,1,len,f)==len&&!memcmp(name,e[i].name,len);
    }
    long end=ftell(f);
    if(end<0)ok=false;
    if(ok) {
        memset(expected,0,22);
        put32(expected,0x06054b50);
        put16(expected+8,count);
        put16(expected+10,count);
        put32(expected+12,(uint32_t)(end-center));
        put32(expected+16,(uint32_t)center);
        ok=fread(actual,1,22,f)==22&&!memcmp(actual,expected,22)&&fgetc(f)==EOF&&!ferror(f);
    }
    if(fclose(f))ok=false;
    return ok;
}
