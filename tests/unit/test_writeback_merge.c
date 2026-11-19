/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/vk/vk_writeback_merge.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1; } } while(0)

static uint32_t random_word(uint32_t *state) {
    *state ^= *state << 13; *state ^= *state >> 17; *state ^= *state << 5; return *state;
}

static int guard_test(void) {
#ifdef _WIN32
    SYSTEM_INFO info;GetSystemInfo(&info);size_t page=info.dwPageSize;
    uint8_t *mem=VirtualAlloc(NULL,page*6,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(mem);
    for(unsigned i=1;i<6;i+=2){DWORD old;CHECK(VirtualProtect(mem+i*page,page,PAGE_NOACCESS,&old));}
    uint8_t *s=mem+page,*o=mem+page*3,*d=mem+page*5;
    for(unsigned n=0;n<=page;n++) {
        memset(s-n,0x95,n);memset(o-n,0x53,n);memset(d-n,0x17,n);
        vk_writeback_merge(d-n,s-n,o-n,n);
        for(unsigned i=0;i<n;i++)CHECK(d[-(ptrdiff_t)n+i]==0x95);
        memcpy(o-n,s-n,n);memset(d-n,0x17,n);
        vk_writeback_merge(d-n,s-n,o-n,n);
        for(unsigned i=0;i<n;i++)CHECK(d[-(ptrdiff_t)n+i]==0x17);
    }
    CHECK(VirtualFree(mem,0,MEM_RELEASE));
#endif
    return 0;
}

int main(void) {
    CHECK(guard_test()==0);
    enum { CAP = 8192 + 64 };
    uint8_t src[CAP], orig[CAP], dst[CAP], expected[CAP]; uint32_t rng = 0x12345678;
    const unsigned lengths[] = {0,1,7,8,15,16,17,31,32,255,256,257,4095,4096,4097,8193};
    unsigned cases = 0;
    for (unsigned len = 0; len < sizeof(lengths)/sizeof(lengths[0]); len++)
    for (unsigned align = 0; align < 16; align++)
    for (unsigned mode = 0; mode < 6; mode++) {
        unsigned n = lengths[len], so = align, oo = (align*3)%16, d = (align*7)%16;
        memset(dst,0xa7,sizeof(dst));
        for (unsigned i=0;i<n;i++) {
            uint8_t b=(uint8_t)random_word(&rng);
            orig[oo+i]=b;src[so+i]=b;dst[d+i]=b;
            if(mode==1 || (mode==2 && !(i%257)) || (mode>=3 && (random_word(&rng)&1)))src[so+i]^=0xff;
            if(mode>=3 && (random_word(&rng)&1))dst[d+i]^=0x53;
        }
        memcpy(expected,dst,sizeof(dst));
        for(unsigned i=0;i<n;i++)if(src[so+i]!=orig[oo+i])expected[d+i]=src[so+i];
        vk_writeback_merge(dst+d,src+so,orig+oo,n);
        CHECK(!memcmp(dst,expected,sizeof(dst)));
        if(mode==5) {
            memcpy(dst+d,orig+oo,n);memcpy(expected,dst,sizeof(dst));memcpy(expected+d,src+so,n);
            vk_writeback_merge(dst+d,src+so,dst+d,n);CHECK(!memcmp(dst,expected,sizeof(dst)));
        }
        cases++;
    }
    printf("writeback merge: %u cases, byte conflicts, independent alignments, tails and aliased originals\n",cases);
    return 0;
}
