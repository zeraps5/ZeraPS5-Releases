/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_PROCESS_PARAMS_H
#define PS5C_PROCESS_PARAMS_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define PROCESS_ARG_MAX 64u

/* Legacy GUEST_ARGS: spaces separate words; quotes are ordinary bytes and
 * extra words beyond the legacy limit are ignored, as on main. */
static inline bool process_args_words(char *text,const char **args,uint32_t *count,uint32_t limit) {
    if(!text || !args || !count || limit>PROCESS_ARG_MAX || *count>limit)return false;
    while(*text && *count<limit) {
        while(*text==' ')text++;
        if(!*text)break;
        args[(*count)++]=text;
        while(*text && *text!=' ')text++;
        if(*text)*text++=0;
    }
    return true;
}

/* Append one UTF-8 argument per line. A final newline is only a terminator;
 * empty interior lines represent empty arguments. No shell expansion. */
static inline bool process_args_lines(char *text,size_t bytes,const char **args,uint32_t *count) {
    if(memchr(text,0,bytes))return false;
    size_t start=0;
    while(start<bytes) {
        if(*count>=PROCESS_ARG_MAX)return false;
        size_t end=start;while(end<bytes && text[end]!='\n')end++;
        size_t len=end-start;
        if(len && text[start+len-1]=='\r')text[start+--len]=0;
        text[end]=0;args[(*count)++]=text+start;start=end+1;
    }
    return true;
}

/* EntryParams: argc, padding, inline argv pointers, argv NULL and empty envp.
 * Keep the original one-argument string at offset64. */
static inline bool process_params_pack(void *buffer,size_t bytes,uint64_t base,
                                       const char *const *args,uint32_t count) {
    if(!buffer || !args || !count || count>PROCESS_ARG_MAX || bytes<64 || base>UINT64_MAX-bytes)return false;
    size_t strings=(8+(size_t)(count+2)*8+63)&~(size_t)63;
    if(strings>bytes)return false;
    size_t total=strings;
    for(uint32_t i=0;i<count;i++) {
        if(!args[i])return false;
        size_t len=strlen(args[i])+1;if(len>bytes-total)return false;total+=len;
    }
    uint8_t *out=buffer;memset(out,0,bytes);memcpy(out,&count,4);
    for(uint32_t i=0;i<count;i++) {
        uint64_t address=base+strings;size_t len=strlen(args[i])+1;
        memcpy(out+8+(size_t)i*8,&address,8);memcpy(out+strings,args[i],len);strings+=len;
    }
    return true;
}
#endif
