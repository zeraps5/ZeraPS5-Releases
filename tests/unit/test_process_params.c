/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/process_params.h"
#include <stdio.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static uint64_t ptr(const uint8_t *p,unsigned i){uint64_t v;memcpy(&v,p+8+i*8,8);return v;}
int main(void) {
    uint8_t page[16384];const uint64_t base=0x900000000ull;
    const char *args[PROCESS_ARG_MAX+1]={"eboot.bin"};uint32_t count=1;
    CHECK(process_params_pack(page,sizeof(page),base,args,count));
    CHECK(page[0]==1 && ptr(page,0)==base+64 && ptr(page,1)==0 && ptr(page,2)==0);
    CHECK(!strcmp((char*)page+64,"eboot.bin"));
    char text[]="-lvl\r\nhub_crashsite\r\n\narg with spaces\nportuguês\n";
    CHECK(process_args_lines(text,sizeof(text)-1,args,&count));
    CHECK(count==6 && !strcmp(args[1],"-lvl") && !strcmp(args[2],"hub_crashsite"));
    CHECK(!*args[3] && !strcmp(args[4],"arg with spaces") && !strcmp(args[5],"português"));
    CHECK(process_params_pack(page,sizeof(page),base,args,count));
    for(unsigned i=0;i<count;i++) {
        CHECK(ptr(page,i)>=base+64 && ptr(page,i)<base+sizeof(page));
        CHECK(!strcmp((char*)page+(ptr(page,i)-base),args[i]));
    }
    CHECK(ptr(page,count)==0 && ptr(page,count+1)==0);
    for(unsigned i=1;i<=PROCESS_ARG_MAX;i++)args[i]="x";
    CHECK(process_params_pack(page,sizeof(page),base,args,PROCESS_ARG_MAX));
    CHECK(ptr(page,0)>=base+8+(PROCESS_ARG_MAX+2)*8 && ptr(page,PROCESS_ARG_MAX)==0);
    page[0]=0x5a;
    CHECK(!process_params_pack(page,sizeof(page),base,args,PROCESS_ARG_MAX+1) && page[0]==0x5a);
    CHECK(!process_params_pack(page,sizeof(page),UINT64_MAX-8,args,1) && page[0]==0x5a);
    char huge[16384];memset(huge,'x',sizeof(huge)-1);huge[sizeof(huge)-1]=0;args[1]=huge;
    CHECK(!process_params_pack(page,sizeof(page),base,args,2) && page[0]==0x5a);
    CHECK(!process_params_pack(page,sizeof(page),base,args,0));
    char nul[]={'a',0,'b','\n',0};count=1;
    CHECK(!process_args_lines(nul,4,args,&count));
    char last[]="tail";CHECK(process_args_lines(last,4,args,&count) && count==2 && !strcmp(args[1],"tail"));
    char extra[]="one\n";count=PROCESS_ARG_MAX;
    CHECK(!process_args_lines(extra,sizeof(extra)-1,args,&count));
    char legacy[]="  -verbose  -lobby   one two three ignored ";count=1;
    CHECK(process_args_words(legacy,args,&count,6) && count==6);
    CHECK(!strcmp(args[1],"-verbose") && !strcmp(args[2],"-lobby") && !strcmp(args[5],"three"));
    CHECK(process_params_pack(page,sizeof(page),base,args,count) && ptr(page,count)==0);
    char spaces[]="   ";count=1;CHECK(process_args_words(spaces,args,&count,6) && count==1);
    char quotes[]="\"a b\"";CHECK(process_args_words(quotes,args,&count,6) && count==3);
    CHECK(!strcmp(args[1],"\"a") && !strcmp(args[2],"b\""));
    CHECK(!process_args_words(spaces,args,&count,2));
    puts("guest argv: ABI pointers, empty environment, CRLF/UTF-8, empty args and bounds verified");
    return 0;
}
