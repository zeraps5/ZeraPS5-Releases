/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include "gpu/vk/vk_snapshot_refresh.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1; } } while(0)
int main(void) {
    uint64_t offset=UINT64_MAX;
    CHECK(vk_snapshot_window(0x10000,8192,0x10000,8192,256,&offset)&&offset==0);
    CHECK(vk_snapshot_window(0x10000,8192,0x11000,4096,256,&offset)&&offset==4096);
    CHECK(!vk_snapshot_window(0x10000,8192,0x11001,4096,256,&offset));
    CHECK(!vk_snapshot_window(0x10000,8192,0x11000,4097,256,&offset));
    CHECK(!vk_snapshot_window(0x10000,8192,0x0ffff,1,256,&offset));
    CHECK(!vk_snapshot_window(0x10000,8192,0x12000,1,256,&offset));
    CHECK(!vk_snapshot_window(UINT64_MAX-4,16,UINT64_MAX-4,1,1,&offset));
    CHECK(!vk_snapshot_window(0,UINT64_MAX,UINT64_MAX-3,8,1,&offset));
    CHECK(!vk_snapshot_window(0,16,0,0,1,&offset));
    CHECK(!vk_snapshot_window(0,16,0,1,0,&offset));
    const size_t bytes=3u*VK_SNAPSHOT_BLOCK+17;
    uint8_t *src=malloc(bytes),*dst=malloc(bytes+32); CHECK(src&&dst);
    memset(src,0x41,bytes);memset(dst,0x41,bytes);memset(dst+bytes,0xa5,32);
    vk_snapshot_refresh_result r=vk_snapshot_refresh(dst,src,bytes,false);
    CHECK(r.ready&&r.copied==0&&r.compared==bytes);
    src[5]=0x42;src[2*VK_SNAPSHOT_BLOCK+4]=0x43;src[bytes-1]=0x44;
    r=vk_snapshot_refresh(dst,src,bytes,false);
    CHECK(!r.ready&&r.copied==0);
    for(size_t i=0;i<bytes;i++)CHECK(dst[i]==0x41); /* held version remains byte-for-byte immutable */
    r=vk_snapshot_refresh(dst,src,bytes,true);
    CHECK(r.ready&&r.compared==bytes&&r.copied==2u*VK_SNAPSHOT_BLOCK+17);
    CHECK(!memcmp(dst,src,bytes));for(size_t i=0;i<32;i++)CHECK(dst[bytes+i]==0xa5);
    r=vk_snapshot_refresh(dst,src,bytes,true);CHECK(r.ready&&r.copied==0);
    memset(src+VK_SNAPSHOT_BLOCK,0x55,VK_SNAPSHOT_BLOCK);
    r=vk_snapshot_refresh(dst,src,bytes,true);CHECK(r.copied==VK_SNAPSHOT_BLOCK);
    CHECK(!memcmp(dst,src,bytes));
    r=vk_snapshot_refresh(NULL,NULL,0,false);CHECK(r.ready&&r.copied==0&&r.compared==0);
    /* Sparse update of a much larger snapshot: verify exact copy reduction,
     * no dirty tracking required and no claim about wall-clock speed. */
    free(src);free(dst);
    size_t large=8u<<20;src=malloc(large);dst=malloc(large);CHECK(src&&dst);
    memset(src,0x11,large);memset(dst,0x11,large);
    src[3]=0x22;src[large-5]=0x33;
    r=vk_snapshot_refresh(dst,src,large,true);
    CHECK(r.ready&&r.compared==large&&r.copied==2u*VK_SNAPSHOT_BLOCK);
    CHECK(!memcmp(dst,src,large));
    printf("snapshot refresh: immutable versions, boundaries and sparse copy %zu/%zu bytes verified\n",r.copied,large);
    free(src);free(dst);return 0;
}
