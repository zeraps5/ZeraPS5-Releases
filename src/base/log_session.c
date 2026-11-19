/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "base/log_session.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
bool log_session_id_valid(const char *id) {
    if(!id || strlen(id)!=32)return false;
    for(unsigned i=0;i<32;i++)if(!((id[i]>='0'&&id[i]<='9')||(id[i]>='a'&&id[i]<='f')))return false;
    return true;
}
bool log_session_meta_valid(const log_session_meta *m) {
    if(!log_session_id_valid(m->id) || !memchr(m->version,0,sizeof(m->version)))return false;
    if(m->version[0]<'0'||m->version[0]>'9')return false;
    for(const char *p=m->version;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||strchr(".+_-",*p)))return false;
    if(m->role<1||m->role>2||m->stage<1||m->stage>6||m->creation_known>1||m->creation_ok>m->creation_known||m->exit_known>1||m->guest_known>1)return false;
    if(m->role==1 && (m->guest_known || (m->exit_known && !m->creation_ok)))return false;
    if(m->role==2 && (m->creation_known||m->exit_known))return false;
    return true;
}
bool log_session_header_write(FILE *f,const char *id) {
    return log_session_id_valid(id) && fprintf(f,"ZSID1 %s\n",id)==39 && !fflush(f);
}
int log_session_header_read(FILE *f,char *id) {
    long start=ftell(f);
    if(start<0)return -1;
    char header[40]={0};
    size_t n=fread(header,1,39,f);
    if(ferror(f))return -1;
    if(n<6||memcmp(header,"ZSID1 ",6))return fseek(f,start,SEEK_SET)?-1:0;
    if(n!=39||header[38]!='\n')return -1;
    header[38]=0;
    if(!log_session_id_valid(header+6))return -1;
    memcpy(id,header+6,33);
    return 1;
}
static bool line(FILE *f,const char *key,char *value,size_t cap) {
    char text[192];
    if(!fgets(text,sizeof(text),f))return false;
    size_t k=strlen(key),n=strlen(text);
    if(n<k+1||text[n-1]!='\n'||strncmp(text,key,k)||n-k>cap)return false;
    memcpy(value,text+k,n-k-1);
    value[n-k-1]=0;
    return true;
}
static bool number(const char *text,uint32_t *out) {
    if(!*text)return false;
    uint64_t n=0;
    for(const char *p=text;*p;p++) {if(*p<'0'||*p>'9')return false;
        n=n*10+(unsigned)(*p-'0');
        if(n>UINT32_MAX)return false;
    }
    *out=(uint32_t)n;
    return true;
}
bool log_session_meta_write(FILE *f,const log_session_meta *m) {
    if(!log_session_meta_valid(m))return false;
    char process[24]="unknown",guest[32]="unknown";
    if(m->exit_known)snprintf(process,sizeof(process),"%" PRIu32,m->exit_code);
    if(m->guest_known)snprintf(guest,sizeof(guest),"%" PRId64,m->guest_code);
    fprintf(f,"ZSM1\nrole=%u\nid=%s\nversion=%s\nbuild=%u\nstage=%u\ncreation=%s\ncreation_error=%u\nprocess_exit=%s\nguest_exit=%s\nEND\n",
        m->role,m->id,m->version,m->build,m->stage,!m->creation_known?"unknown":(m->creation_ok?"ok":"failed"),m->creation_error,process,guest);
    return !ferror(f);
}
bool log_session_meta_read(FILE *f,log_session_meta *out) {
    log_session_meta m={0};
    char v[96];
    if(!fgets(v,sizeof(v),f)||strcmp(v,"ZSM1\n"))return false;
    if(!line(f,"role=",v,sizeof(v))||!number(v,&m.role)||!line(f,"id=",m.id,sizeof(m.id))||!line(f,"version=",m.version,sizeof(m.version)))return false;
    if(!line(f,"build=",v,sizeof(v))||!number(v,&m.build)||!line(f,"stage=",v,sizeof(v))||!number(v,&m.stage)||!line(f,"creation=",v,sizeof(v)))return false;
    if(!strcmp(v,"ok")){m.creation_known=1;
        m.creation_ok=1;
    }else if(!strcmp(v,"failed"))m.creation_known=1;
    else if(strcmp(v,"unknown"))return false;
    if(!line(f,"creation_error=",v,sizeof(v))||!number(v,&m.creation_error)||!line(f,"process_exit=",v,sizeof(v)))return false;
    if(strcmp(v,"unknown")){if(!number(v,&m.exit_code))return false;
        m.exit_known=1;
    }
    if(!line(f,"guest_exit=",v,sizeof(v)))return false;
    if(strcmp(v,"unknown")) {
        const char *p=v;
        if(*p=='-')p++;
        if(!*p)return false;
        for(;*p;p++)if(*p<'0'||*p>'9')return false;
        errno=0;
        char *end=NULL;
        intmax_t code=strtoimax(v,&end,10);
        if(errno||!end||*end||code<INT64_MIN||code>INT64_MAX)return false;
        m.guest_known=1;
        m.guest_code=(int64_t)code;
    }
    if(!fgets(v,sizeof(v),f)||strcmp(v,"END\n")||fgetc(f)!=EOF||ferror(f)||!log_session_meta_valid(&m))return false;
    *out=m;
    return true;
}
