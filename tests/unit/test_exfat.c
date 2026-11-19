/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "pal/pal_exfat.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

enum { SECTOR = 512, HEAP = 27, CLUSTERS = 320, IMAGE_BYTES = (HEAP+CLUSTERS)*SECTOR };
static uint8_t image[IMAGE_BYTES];
static void w16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void w32(uint8_t *p, uint32_t v) { for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(i*8)); }
static void w64(uint8_t *p, uint64_t v) { for(unsigned i=0;i<8;i++) p[i]=(uint8_t)(v>>(i*8)); }
static uint8_t *cluster(unsigned c) { return image+(HEAP+c-2)*SECTOR; }
static void fat(unsigned c, unsigned next) { w32(image+24*SECTOR+c*4,next); }
static void entry(uint8_t *e, const uint16_t *name, unsigned n, unsigned first, uint64_t size, uint64_t valid, unsigned flags, bool dir) {
    unsigned names=(n+14)/15, total=(2+names)*32;
    memset(e,0,total); e[0]=0x85; e[1]=(uint8_t)(names+1); w16(e+4,dir?16:32);
    e[32]=0xc0; e[33]=(uint8_t)flags; e[35]=(uint8_t)n;
    w64(e+40,valid); w32(e+52,first); w64(e+56,size);
    for(unsigned k=0;k<names;k++) { e[64+k*32]=0xc1; for(unsigned j=0;j<15 && k*15+j<n;j++) w16(e+66+k*32+j*2,name[k*15+j]); }
    uint16_t sum=0;
    for(unsigned i=0;i<total;i++) if(i!=2 && i!=3) sum=(uint16_t)((sum>>1|sum<<15)+e[i]);
    w16(e+2,sum);
}
static void ascii_entry(uint8_t *e,const char *name,unsigned c,uint64_t size,uint64_t valid,unsigned flags,bool dir) {
    uint16_t w[256]={0}; size_t n=strlen(name); for(size_t i=0;i<n;i++) w[i]=(uint8_t)name[i];
    entry(e,w,(unsigned)n,c,size,valid,flags,dir);
}
static void make_image(void) {
    memset(image,0,sizeof(image));
    image[0]=0xeb; image[1]=0x76; image[2]=0x90; memcpy(image+3,"EXFAT   ",8);
    w64(image+72,HEAP+CLUSTERS); w32(image+80,24); w32(image+84,3); w32(image+88,HEAP);
    w32(image+92,CLUSTERS); w32(image+96,2); w16(image+104,0x100); image[108]=9; image[110]=1; w16(image+510,0xaa55);
    uint32_t sum=0;
    for(unsigned i=0;i<11*SECTOR;i++) if(i!=106 && i!=107 && i!=112) sum=(sum>>1|sum<<31)+image[i];
    for(unsigned i=11*SECTOR;i<12*SECTOR;i+=4) w32(image+i,sum);
    fat(2,267); fat(267,0xffffffff); fat(259,0xffffffff); fat(260,262); fat(262,0xffffffff); fat(266,0xffffffff);
    for(unsigned c=3;c<=258;c++) fat(c,c==258?0xffffffff:c+1);
    uint8_t *table=cluster(3); sum=0;
    for(unsigned i=0;i<65536;i++) { unsigned upper=i>='a'&&i<='z'?i-32:i; if(i==0xe4) upper=0xc4; w16(table+i*2,(uint16_t)upper); }
    for(unsigned i=0;i<131072;i++) sum=(sum>>1|sum<<31)+table[i];
    uint8_t root[1024]={0}; root[0]=0x82; w32(root+4,sum); w32(root+20,3); w64(root+24,131072);
    ascii_entry(root+32,"sce_sys",259,512,512,3,true);
    ascii_entry(root+128,"fragment.bin",260,900,800,1,false);
    ascii_entry(root+224,"eboot.bin",263,700,700,3,false);
    uint16_t name[]={0x00e4,0xd83d,0xde00,'.','b','i','n'};
    entry(root+320,name,7,265,4,4,3,false);
    ascii_entry(root+416,"empty-directory-long",266,512,512,3,true); /* secondary entries cross root cluster boundary */
    ascii_entry(root+544,"zero.bin",0,0,0,1,false);
    memcpy(cluster(2),root,512); memcpy(cluster(267),root+512,512);
    ascii_entry(cluster(259),"param.json",268,2,2,3,false); memcpy(cluster(268),"{}",2);
    memset(cluster(260),'A',512); memset(cluster(262),'B',512);
    memset(cluster(263),'E',512); memset(cluster(264),'F',512); memcpy(cluster(265),"UTF8",4);
}
static void make_overlay(void) {
    make_image();
    uint8_t root[1024]; memcpy(root,cluster(2),512); memcpy(root+512,cluster(267),512);
    ascii_entry(root+640,"decrypted",269,512,512,3,true);
    ascii_entry(root+736,"Media",273,512,512,3,true);
    memcpy(cluster(2),root,512); memcpy(cluster(267),root+512,512);
    ascii_entry(cluster(269),"eboot.bin",270,16,16,3,false);
    ascii_entry(cluster(269)+96,"Media",271,512,512,3,true);
    ascii_entry(cluster(271),"only.prx",272,1,1,3,false);
    ascii_entry(cluster(271)+96,"same.prx",276,1,1,3,false);
    ascii_entry(cluster(273),"asset.dat",274,1,1,3,false);
    ascii_entry(cluster(273)+96,"same.prx",275,1,1,3,false);
    memset(cluster(270),'D',16); cluster(272)[0]='P'; cluster(274)[0]='A'; cluster(275)[0]='O'; cluster(276)[0]='Z';
}
static void write_image(const char *path, uint64_t bytes) {
    pal_file *f=NULL; CHECK(pal_file_open(path,PAL_FILE_WRITE|PAL_FILE_CREATE|PAL_FILE_TRUNCATE,&f)==PS5C_OK);
    CHECK(pal_file_write(f,image,bytes,NULL)==PS5C_OK); pal_file_close(f);
}
static bool count_entry(void *user,const char *name,bool dir) { (void)name;(void)dir; (*(unsigned *)user)++; return true; }
static int32_t reader(void *arg) {
    pal_file *f=(pal_file *)arg;
    for(unsigned i=0;i<200;i++) { uint8_t b[230]; uint64_t got=0,off=450+i%50;
        if(pal_file_pread(f,b,sizeof(b),off,&got)!=PS5C_OK || got!=sizeof(b)) return 1;
        for(unsigned j=0;j<sizeof(b);j++) if(b[j]!=(off+j<512?'A':'B')) return 2;
    }
    return 0;
}
int main(int argc,char **argv) {
    CHECK(pal_init()==PS5C_OK);
    /* Optional real-image probe: find the virtual game executable and read its header. */
    if(argc==2) { char eboot[1024]; CHECK(exfat_game_path(argv[1],eboot,sizeof(eboot))==PS5C_OK);
        pal_file *f=NULL; uint64_t bytes=0; CHECK(pal_file_open(eboot,PAL_FILE_READ,&f)==PS5C_OK);
        CHECK(pal_file_size(f,&bytes)==PS5C_OK); printf("direct exFAT eboot: %s (%llu bytes)\n",eboot,(unsigned long long)bytes);
        uint8_t b[4]; CHECK(pal_file_pread(f,b,4,0,NULL)==PS5C_OK); printf("magic %02x %02x %02x %02x\n",b[0],b[1],b[2],b[3]);
        pal_file_close(f); pal_shutdown(); return 0; }
    make_image(); write_image("unit.exfat",sizeof(image));
    char eboot[512]; CHECK(exfat_game_path("unit.exfat",eboot,sizeof(eboot))==PS5C_OK); CHECK(!strcmp(eboot,"unit.exfat/@game/eboot.bin"));
    unsigned count=0; CHECK(pal_dir_list("unit.exfat/",count_entry,&count)==PS5C_OK); CHECK(count==6);
    pal_path_info st; CHECK(pal_path_stat("unit.exfat/sce_sys/../EMPTY-directory-long",&st)==PS5C_OK && st.is_dir);
    CHECK(pal_path_stat("unit.exfat/../../missing",&st)==PS5C_ERR_NOT_FOUND);
    CHECK(pal_path_stat("unit.exfat/\xc3\x84\xf0\x9f\x98\x80.bin",&st)==PS5C_OK && st.size==4);
    pal_file *f=NULL; CHECK(pal_file_open("unit.exfat/FRAGMENT.bin",PAL_FILE_READ,&f)==PS5C_OK);
    uint8_t b[1000]; uint64_t got=0; CHECK(pal_file_read(f,b,sizeof(b),&got)==PS5C_OK && got==900);
    for(unsigned i=0;i<900;i++) CHECK(b[i]==(i<512?'A':i<800?'B':0));
    CHECK(pal_file_read(f,b,1,&got)==PS5C_OK && got==0);
    CHECK(pal_file_seek(f,-388,PAL_SEEK_END,NULL)==PS5C_OK); CHECK(pal_file_read(f,b,1,&got)==PS5C_OK && b[0]=='B');
    CHECK(pal_file_seek(f,INT64_MIN,PAL_SEEK_CUR,NULL)==PS5C_ERR_INVALID);
    CHECK(pal_file_write(f,b,1,&got)==PS5C_ERR_BUSY && got==0);
    pal_thread *threads[4]; for(unsigned i=0;i<4;i++) CHECK(pal_thread_create(&threads[i],reader,f,"exfat",0)==PS5C_OK);
    for(unsigned i=0;i<4;i++) {int32_t result=-1; CHECK(pal_thread_join(threads[i],&result)==PS5C_OK && result==0);}
    pal_file_close(f);
    CHECK(pal_file_open("unit.exfat/FRAGMENT.bin",PAL_FILE_READ|PAL_FILE_RECOVER_READ,&f)==PS5C_OK);
    CHECK(pal_file_pread(f,b,32,500,&got)==PS5C_OK && got==32);
    for(unsigned i=0;i<32;i++)CHECK(b[i]==(i<12?'A':'B'));
    pal_file_close(f);
    CHECK(pal_file_open("unit.exfat/eboot.bin",PAL_FILE_READ|PAL_FILE_RECOVER_READ|PAL_FILE_WRITE,&f)==PS5C_ERR_BUSY);
    CHECK(pal_file_open("unit.exfat/eboot.bin",PAL_FILE_READ|PAL_FILE_RECOVER_READ|PAL_FILE_CREATE,&f)==PS5C_ERR_BUSY);
    CHECK(pal_file_open("unit.exfat/eboot.bin",PAL_FILE_READ|PAL_FILE_RECOVER_READ|PAL_FILE_TRUNCATE,&f)==PS5C_ERR_BUSY);
    CHECK(pal_file_open("unit.exfat/eboot.bin",PAL_FILE_WRITE|PAL_FILE_TRUNCATE,&f)==PS5C_ERR_BUSY);
    CHECK(pal_file_remove("unit.exfat/eboot.bin")==PS5C_ERR_BUSY);
    CHECK(pal_file_rename("unit.exfat/eboot.bin","oops")==PS5C_ERR_BUSY);
    CHECK(pal_mkdir("unit.exfat/new")==PS5C_ERR_BUSY);
    CHECK(pal_file_open("unit.exfat/zero.bin",PAL_FILE_READ,&f)==PS5C_OK);
    CHECK(pal_file_read(f,b,1,&got)==PS5C_OK && got==0); pal_file_close(f);
    uint8_t *unchanged=NULL; uint64_t bytes=0; CHECK(pal_file_read_all("unit.exfat",&unchanged,&bytes)==PS5C_OK);
    CHECK(bytes==sizeof(image) && !memcmp(unchanged,image,sizeof(image))); free(unchanged);
    make_overlay(); write_image("overlay.exfat",sizeof(image));
    CHECK(exfat_game_path("overlay.exfat",eboot,sizeof(eboot))==PS5C_OK);
    CHECK(pal_file_open(eboot,PAL_FILE_READ,&f)==PS5C_OK); CHECK(pal_file_read(f,b,1,&got)==PS5C_OK && b[0]=='D'); pal_file_close(f);
    CHECK(pal_file_open("overlay.exfat/eboot.bin",PAL_FILE_READ,&f)==PS5C_OK); CHECK(pal_file_read(f,b,1,&got)==PS5C_OK && b[0]=='E'); pal_file_close(f);
    CHECK(pal_file_open("overlay.exfat/@game/Media/asset.dat",PAL_FILE_READ,&f)==PS5C_OK); CHECK(pal_file_read(f,b,1,&got)==PS5C_OK && b[0]=='A'); pal_file_close(f);
    CHECK(pal_file_open("overlay.exfat/@game/Media/same.prx",PAL_FILE_READ,&f)==PS5C_OK); CHECK(pal_file_read(f,b,1,&got)==PS5C_OK && b[0]=='Z'); pal_file_close(f);
    count=0; CHECK(pal_dir_list("overlay.exfat/@game/Media",count_entry,&count)==PS5C_OK && count==3);
    count=0; CHECK(pal_dir_list("overlay.exfat/@game",count_entry,&count)==PS5C_OK && count==7);
    CHECK(pal_path_stat("overlay.exfat/@game/../eboot.bin",&st)==PS5C_OK && st.size==16);
    CHECK(pal_path_stat("overlay.exfat/@game/sce_sys/param.json",&st)==PS5C_OK && st.size==2);
    make_image();
    write_image("short.exfat",100); CHECK(exfat_game_path("short.exfat",eboot,sizeof(eboot))!=PS5C_OK);
    fat(260,260); write_image("cycle.exfat",sizeof(image)); CHECK(exfat_game_path("cycle.exfat",eboot,sizeof(eboot))==PS5C_ERR_INVALID);
    make_image(); image[3]='?'; write_image("bad.exfat",sizeof(image)); CHECK(exfat_game_path("bad.exfat",eboot,sizeof(eboot))==PS5C_ERR_INVALID);
    make_image(); image[120]=1; write_image("checksum.exfat",sizeof(image)); CHECK(exfat_game_path("checksum.exfat",eboot,sizeof(eboot))==PS5C_ERR_INVALID);
    make_image(); cluster(2)[34] ^= 1; write_image("entry-checksum.exfat",sizeof(image)); CHECK(exfat_game_path("entry-checksum.exfat",eboot,sizeof(eboot))==PS5C_ERR_INVALID);
    pal_shutdown(); CHECK(pal_file_remove("unit.exfat")==PS5C_OK); CHECK(pal_file_remove("short.exfat")==PS5C_OK);
    CHECK(pal_file_remove("cycle.exfat")==PS5C_OK); CHECK(pal_file_remove("bad.exfat")==PS5C_OK);
    CHECK(pal_file_remove("overlay.exfat")==PS5C_OK); CHECK(pal_file_remove("checksum.exfat")==PS5C_OK); CHECK(pal_file_remove("entry-checksum.exfat")==PS5C_OK);
    puts("exFAT: fragmented/contiguous reads, Unicode, zero-fill, seek, concurrent reads, overlays, immutable image and malformed volumes passed"); return 0;
}
