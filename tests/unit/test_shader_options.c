/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/shader/shader.h"
#include "gpu/shader/spv.h"
#include "gpu/vk/vk_shader_cache.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line%d: %s\n",__LINE__,#x); return 1; } } while (0)
static void option(const char *name,const char *value) {
#ifdef _WIN32
    _putenv_s(name,value?value:"");
#else
    if(value)setenv(name,value,1);else unsetenv(name);
#endif
    ps5c_env_reset();
}
static unsigned decorations(const uint32_t *code,size_t words) {
    unsigned count=0;
    for(size_t i=5;i<words;) {
        unsigned n=code[i]>>16,op=code[i]&0xffff;
        if(!n || n>words-i)return UINT32_MAX;
        if(op==SpvOpDecorate && n>=3 && code[i+2]==SpvDecorationNoContraction)count++;
        i+=n;
    }
    return count;
}
int main(void) {
    const char *diagnostics[]={"PS5C_NGG_REAL_LANES","PS5C_NO_BARRIER","PS5C_NO_STRUCTURED"};
    for(unsigned i=0;i<PS5C_ARRAY_COUNT(diagnostics);i++)option(diagnostics[i],NULL);
    option("PS5C_WAVE64_LDS",NULL);
    const uint32_t code[]={0xf4280000,0xfa000000,0xbf8cc07f,0x7e000200,0x7e020201,
        0x10040300,0x060604f3,0x7e080303,0xe0700000,0x80030400,0xbf810000};
    sh_program *p=calloc(1,sizeof(*p));CHECK(p && sh_analyze(code,PS5C_ARRAY_COUNT(code),0x100000,p));
    sh_stage_info info={.stage=SH_STAGE_CS,.local_size={1,1,1}};
    sh_site_shape shapes[SH_MAX_SITES]={0};uint64_t keys[3];
    for(unsigned mode=0;mode<3;mode++) {
        char value[2]={(char)('0'+mode),0};option("PS5C_SHADER_EXACT",value);
        keys[mode]=vk_shader_cache_key(p,&info,shapes,0);
        uint32_t *spv=NULL;size_t words=0;CHECK(sh_translate(p,&info,shapes,0,&spv,&words));
        unsigned n=decorations(spv,words);CHECK(n!=UINT32_MAX && (mode==2?n>0:n==0));
        free(spv);
    }
    CHECK(keys[0]==keys[1] && keys[0]!=keys[2]); /* effective CS precision */
    option("PS5C_SHADER_EXACT",NULL);CHECK(vk_shader_cache_key(p,&info,shapes,0)==keys[2]);
    info.stage=SH_STAGE_VS;option("PS5C_SHADER_EXACT","1");
    uint64_t vertex=vk_shader_cache_key(p,&info,shapes,0);
    option("PS5C_SHADER_EXACT","2");CHECK(vertex==vk_shader_cache_key(p,&info,shapes,0));
    option("PS5C_SHADER_EXACT","0");CHECK(vertex!=vk_shader_cache_key(p,&info,shapes,0));
    option("PS5C_SHADER_EXACT",NULL);info.stage=SH_STAGE_CS;
    for(unsigned i=0;i<PS5C_ARRAY_COUNT(diagnostics);i++) {
        option(diagnostics[i],"1");CHECK(vk_shader_cache_key(p,&info,shapes,0)!=keys[2]);
        option(diagnostics[i],NULL);CHECK(vk_shader_cache_key(p,&info,shapes,0)==keys[2]);
    }
    char hash[32];snprintf(hash,sizeof(hash),"%llx",(unsigned long long)p->hash);
    option("PS5C_WAVE64_LDS",hash);uint64_t lds=vk_shader_cache_key(p,&info,shapes,0);CHECK(lds!=keys[2]);
    option("PS5C_WAVE64_LDS","1");CHECK(vk_shader_cache_key(p,&info,shapes,0)==lds);
    option("PS5C_WAVE64_LDS",NULL);CHECK(vk_shader_cache_key(p,&info,shapes,0)==keys[2]);
    info.uniform_shift=sh_uniform_compact_shift(p->site_count);
    CHECK(info.uniform_shift && vk_shader_cache_key(p,&info,shapes,0)!=keys[2]);
    uint32_t *packed=NULL;size_t packed_words=0;CHECK(sh_translate(p,&info,shapes,0,&packed,&packed_words));free(packed);
    info.uniform_shift=1;CHECK(!sh_translate(p,&info,shapes,0,&packed,&packed_words));
    info.uniform_shift=0;CHECK(vk_shader_cache_key(p,&info,shapes,0)==keys[2]);
    free(p);puts("shader options: precision, diagnostic, LDS and uniform layout cache identity verified");return 0;
}
