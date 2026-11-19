/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real GPU stores, synthetic guest bytes, allocation faults in this executable
 * only. Child processes distinguish recovery from an unsafe false success. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "gpu/gpu_regs.h"
#include "gpu/vk/vk_internal.h"

#define CHECK(x) do { if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);return 100;} } while(0)
#define BUFFER_BYTES ((3u<<20)+19u)
#define POOL_BYTES (128u<<20)
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size);
void __real_pal_exit_process(int code);
void __wrap_pal_exit_process(int code);
static _Thread_local bool armed;
static _Thread_local unsigned mode,original_failures,pool_failures;
static bool pool_enabled;
static renderer *active_renderer;
static gmem *active_mem;
static guest_addr active_input,active_label;
static uint64_t dispatches_before;
static bool child_not_stopped;

void *__wrap_malloc(size_t size) {
    if(armed && mode && size==BUFFER_BYTES && (mode==2 || !original_failures)) {
        original_failures++;return NULL;
    }
    if(armed && mode && pool_enabled && size==POOL_BYTES && (mode==2 || !pool_failures)) {
        pool_failures++;return NULL;
    }
    return __real_malloc(size);
}

void __wrap_pal_exit_process(int code) {
    if(armed && mode==2 && active_renderer) {
        uint64_t draws,dispatches;uint32_t label=99,canary=0;
        vk_pipeline_counts(active_renderer,&draws,&dispatches);
        bool unchanged=gmem_read(active_mem,active_label,&label,4) && label==0 &&
            gmem_read(active_mem,active_input+8,&canary,4) && canary==0x5a5a5a5a;
        bool injections=original_failures==2 && pool_failures==(pool_enabled?2u:0u);
        fprintf(stderr,"WB_OOM_FATAL_GUARD %s: code%d dispatches%llu originals%u pool%u label%u\n",
            unchanged && injections && dispatches==dispatches_before?"PASS":"FAIL",code,
            (unsigned long long)(dispatches-dispatches_before),original_failures,pool_failures,label);
        fflush(stderr);
    }
    __real_pal_exit_process(code);
}

static int child(unsigned which) {
    mode=which;SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    CHECK(pal_init()==PS5C_OK);
    const char *pool=getenv("PS5C_WB_POOL");pool_enabled=pool && !strcmp(pool,"1");
    static const uint32_t writer[]={0x7e080208,0xe0700000,0x80010400,0xbf810000};
    static const uint32_t multi[]={0x7e080208,0xe0700000,0x80010400,0x7e080209,0xe0700040,0x80010400,0xbf810000};
    static const uint32_t unbounded[]={0xbf820000,0x7e08020a,0xe0700080,0x80010400,0xbf810000};
    bool scope=getenv("PS5C_TEST_SCOPE_MULTI")!=NULL;
    gmem mem;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&mem,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&mem,0,8u<<20,GPROT_RW,0,"writeback allocation fixture",&a)==PS5C_OK);
    CHECK(gmem_write(&mem,a,scope?multi:writer,scope?sizeof(multi):sizeof(writer)));
    if(scope)CHECK(gmem_write(&mem,a+0x200,unbounded,sizeof(unbounded)));
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 writeback allocation",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&mem);CHECK(r);gpu g;CHECK(gpu_init(&g,&mem,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(a>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(a>>40);
    g.regs[mmCOMPUTE_PGM_RSRC2]=16u<<1;
    g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    guest_addr input=a+0x4000,label=a+0x700000;uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    u[4]=(uint32_t)input;u[5]=(uint32_t)(input>>32);u[6]=BUFFER_BYTES;u[7]=0x16204;u[8]=0x5a5aaabb;
    u[9]=0x10203040;u[10]=0x61728394;
    uint8_t *bytes=gmem_host(&mem,input,BUFFER_BYTES+16);CHECK(bytes);memset(bytes,0x5a,BUFFER_BYTES+16);
    CHECK(gmem_write_u64(&mem,label,0));vk_begin(r);
    uint64_t draws,batch_before=r->use_seq;vk_pipeline_counts(r,&draws,&dispatches_before);
    active_renderer=r;active_mem=&mem;active_input=input;active_label=label;
    armed=true;renderer_dispatch(r,&g,1,1,1);armed=false;
    if(mode==2){fputs("WB_PERSISTENT_OOM_RETURNED\n",stderr);return 88;}
    uint64_t dispatches;vk_pipeline_counts(r,&draws,&dispatches);
    CHECK(dispatches==dispatches_before+1);
    CHECK(original_failures==(mode?1u:0u) && pool_failures==(mode && pool_enabled?1u:0u));
    bool retried=!mode || r->use_seq>batch_before;
    bytes[3]=0x99;bytes[17]=0x71;bytes[BUFFER_BYTES-1]=0x83;
    gpu_release release={.addr=label,.data=77,.data_sel=1};CHECK(renderer_release(r,&g,&release));
    uint32_t seen=99;CHECK(gmem_read(&mem,label,&seen,4) && seen==0);
    if(scope) {
        uint64_t active_batch=r->use_seq;
        renderer_sync_mem(r,input+0x1000,4);
        CHECK(r->recording && r->use_seq==active_batch && gmem_read(&mem,label,&seen,4) && seen==0);
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x200)>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)((a+0x200)>>40);
        renderer_dispatch(r,&g,1,1,1);
        renderer_sync_mem(r,input+0x1000,4); /* the unproved writer must restore the full dependency */
        CHECK(!r->recording && gmem_read(&mem,label,&seen,4) && seen==77);
        vk_pipeline_counts(r,&draws,&dispatches);CHECK(dispatches==dispatches_before+2);
    }
    renderer_sync_mem(r,input,BUFFER_BYTES);
    uint32_t first;memcpy(&first,bytes,4);unsigned bad=first!=0x995aaabb;
    for(uint32_t i=4;i<BUFFER_BYTES+16;i++) {
        uint8_t expected=i==17?0x71:i==BUFFER_BYTES-1?0x83:0x5a;
        if(scope && i>=64 && i<68)expected=(uint8_t)(u[9]>>((i-64)*8));
        if(scope && i>=128 && i<132)expected=(uint8_t)(u[10]>>((i-128)*8));
        bad+=bytes[i]!=expected;
    }
    CHECK(gmem_read(&mem,label,&seen,4) && seen==77);
    fprintf(stderr,"WB_RECOVERY mode%u pool%d: incorrect%u originals%u pool-faults%u dispatches%llu retried%d\n",
        mode,pool_enabled,bad,original_failures,pool_failures,(unsigned long long)(dispatches-dispatches_before),retried);
    CHECK(!bad && retried);
    renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&mem);pal_shutdown();
    return 0;
}

static int check_child(unsigned which) {
    wchar_t path[32768],cmd[32832],log[100];
    CHECK(GetModuleFileNameW(NULL,path,PS5C_ARRAY_COUNT(path))>0);
    CHECK(swprintf(cmd,PS5C_ARRAY_COUNT(cmd),L"\"%ls\" --child %u",path,which)>0);
    CHECK(swprintf(log,PS5C_ARRAY_COUNT(log),L"writeback-oom-%lu-%u.log",GetCurrentProcessId(),which)>0);
    SECURITY_ATTRIBUTES sa={.nLength=sizeof(sa),.bInheritHandle=TRUE};
    HANDLE output=CreateFileW(log,GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    CHECK(output!=INVALID_HANDLE_VALUE);
    STARTUPINFOW si={.cb=sizeof(si),.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW,.wShowWindow=SW_HIDE,
                    .hStdInput=NULL,.hStdOutput=output,.hStdError=output};
    PROCESS_INFORMATION pi={0};
    BOOL started=CreateProcessW(path,cmd,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi);
    CloseHandle(output);CHECK(started);
    DWORD result=99;
    if(WaitForSingleObject(pi.hProcess,15000)==WAIT_OBJECT_0)GetExitCodeProcess(pi.hProcess,&result);
    else {
        TerminateProcess(pi.hProcess,result);
        child_not_stopped=WaitForSingleObject(pi.hProcess,5000)!=WAIT_OBJECT_0;
        if(child_not_stopped)fprintf(stderr,"owned GPU child PID%lu did not stop; aborting remaining cases\n",pi.dwProcessId);
    }
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
    FILE *f=_wfopen(log,L"r");CHECK(f);char line[2048];bool fatal=false,guard=false,recovered=false,returned=false;
    while(fgets(line,sizeof(line),f)) {
        fputs(line,stderr);
        fatal|=strstr(line,"writeback snapshot allocation failed")!=NULL;
        guard|=strstr(line,"WB_OOM_FATAL_GUARD PASS")!=NULL;
        recovered|=strstr(line,"WB_RECOVERY") && strstr(line,"incorrect0");
        returned|=strstr(line,"WB_PERSISTENT_OOM_RETURNED")!=NULL;
    }
    fclose(f);
    fprintf(stderr,"writeback child%u: exit%lu fatal%d guard%d recovery%d returned%d\n",which,result,fatal,guard,recovered,returned);
    CHECK(which==2 ? result==1 && fatal && guard && !returned : result==0 && recovered && !fatal);
    return 0;
}

int main(int argc,char **argv) {
    if(argc==3 && !strcmp(argv[1],"--child"))return child((unsigned)atoi(argv[2]));
    unsigned failures=0;
    for(unsigned which=0;which<3;which++) {
        failures+=check_child(which)!=0;
        if(child_not_stopped)break;
    }
    return failures?1:0;
}
