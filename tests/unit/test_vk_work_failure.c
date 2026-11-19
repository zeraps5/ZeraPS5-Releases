/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Run the real work paths in children: failed Vulkan calls must stop before
 * fence reuse, writeback or an end-of-pipe interrupt. No GPU is needed for the
 * recording guards; PS5C_TEST_GPU adds real pending work and presentation. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "gpu/gpu_regs.h"
#include "gpu/vk/vk_internal.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line%d: %s\n",__LINE__,#x); return 100; } } while (0)
enum { HEALTHY, WAIT, RESET_FENCE, RESET_COMMAND, BEGIN, PRESENT_WAIT,
       GPU_SYNC=10, GPU_SUBMIT, GPU_END, GPU_WRITEBACK, GPU_IDLE,
       GPU_PRESENT_SUBMIT, GPU_ACQUIRE, GPU_HEALTHY, GPU_OUT_OF_DATE, GPU_SUBOPTIMAL,
       GPU_PIPELINE_LOST, GPU_MODULE_LOST };
static int mode;
static bool failed, once;
static unsigned submits, interrupts;
static PFN_vkQueueSubmit real_submit;
static PFN_vkAcquireNextImageKHR real_acquire;
static PFN_vkQueuePresentKHR real_present;
static VKAPI_ATTR VkResult VKAPI_CALL pipeline_stub(VkDevice d,VkPipelineCache c,uint32_t n,const VkComputePipelineCreateInfo*i,const VkAllocationCallbacks*a,VkPipeline*p) {
    (void)d;(void)c;(void)n;(void)i;(void)a;(void)p;return VK_ERROR_DEVICE_LOST;
}
static VKAPI_ATTR VkResult VKAPI_CALL module_stub(VkDevice d,const VkShaderModuleCreateInfo*i,const VkAllocationCallbacks*a,VkShaderModule*p) {
    (void)d;(void)i;(void)a;(void)p;return VK_ERROR_DEVICE_LOST;
}

static VkResult step(int at) {
    if (failed) pal_exit_process(92); /* the failed result was ignored */
    if (mode==at) { failed=true; return VK_ERROR_DEVICE_LOST; }
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL wait_stub(VkDevice d,uint32_t n,const VkFence*f,VkBool32 all,uint64_t ns) {
    (void)d;(void)n;(void)f;(void)all;(void)ns;
    if(mode>=GPU_SYNC) { failed=true;return VK_ERROR_DEVICE_LOST; }
    return step(mode==PRESENT_WAIT?PRESENT_WAIT:WAIT);
}
static VKAPI_ATTR VkResult VKAPI_CALL reset_fence_stub(VkDevice d,uint32_t n,const VkFence*f) {
    (void)d;(void)n;(void)f;return step(RESET_FENCE);
}
static VKAPI_ATTR VkResult VKAPI_CALL reset_command_stub(VkCommandBuffer c,VkCommandBufferResetFlags f) {
    (void)c;(void)f;return step(RESET_COMMAND);
}
static VKAPI_ATTR VkResult VKAPI_CALL begin_stub(VkCommandBuffer c,const VkCommandBufferBeginInfo*i) {
    (void)c;(void)i;return step(BEGIN);
}
static VKAPI_ATTR VkResult VKAPI_CALL end_stub(VkCommandBuffer c) {
    (void)c;failed=true;return VK_ERROR_OUT_OF_HOST_MEMORY;
}
static VKAPI_ATTR VkResult VKAPI_CALL submit_stub(VkQueue q,uint32_t n,const VkSubmitInfo*i,VkFence f) {
    if(failed)pal_exit_process(92);
    if(++submits==(mode==GPU_PRESENT_SUBMIT?2u:1u)) {
        failed=true;return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    return real_submit(q,n,i,f);
}
static VKAPI_ATTR VkResult VKAPI_CALL acquire_stub(VkDevice d,VkSwapchainKHR s,uint64_t ns,VkSemaphore a,VkFence f,uint32_t*i) {
    if(mode==GPU_ACQUIRE){failed=true;return VK_ERROR_DEVICE_LOST;}
    if(!once){once=true;return VK_ERROR_OUT_OF_DATE_KHR;}
    return real_acquire(d,s,ns,a,f,i);
}
static VKAPI_ATTR VkResult VKAPI_CALL present_stub(VkQueue q,const VkPresentInfoKHR*i) {
    if(failed)pal_exit_process(92); /* failed present submission reached presentation */
    VkResult result=real_present(q,i);
    if(!once && result==VK_SUCCESS){once=true;return VK_SUBOPTIMAL_KHR;}
    return result;
}
static void interrupt(void *user,int32_t event,uint32_t context) {
    (void)user;(void)event;(void)context;
    if(mode<GPU_HEALTHY)pal_exit_process(91); /* failed work published a completion */
    interrupts++;
}

static int recording_child(void) {
    renderer *r=calloc(1,sizeof(*r));CHECK(r);
    r->device=(VkDevice)(uintptr_t)1;
    r->work_fences[0]=(VkFence)(uintptr_t)2;
    r->work_cmds[0]=r->cmd=(VkCommandBuffer)(uintptr_t)3;
    vkWaitForFences=wait_stub;vkResetFences=reset_fence_stub;
    vkResetCommandBuffer=reset_command_stub;vkBeginCommandBuffer=begin_stub;
    if(mode==HEALTHY) {
        vk_check_work(r,VK_SUCCESS,"healthy control");
        CHECK(vk_check_device(r,VK_ERROR_FORMAT_NOT_SUPPORTED,"format fallback")==VK_ERROR_FORMAT_NOT_SUPPORTED);
        CHECK(vk_check_device(r,VK_ERROR_OUT_OF_DEVICE_MEMORY,"allocation fallback")==VK_ERROR_OUT_OF_DEVICE_MEMORY);
        r->recording=true;CHECK(vk_begin(r)==r->cmd);free(r);return 0;
    }
    if(mode==PRESENT_WAIT)renderer_present(r,NULL);
    else vk_begin(r);
    return 93; /* a failed operation returned to its caller */
}

static int gpu_child(void) {
    /* The S_BUFFER input and output are different resources. This is the
     * scalar-snapshot fixture, with real staged writeback and a held release. */
    static const uint32_t reader[]={0xf4200400,0xfa000000,0xbf8cc07f,
        0x7e080210,0xe0700000,0x80030400,0xbf810000};
    gmem mem;gmem_config cfg=gmem_default_config();cfg.identity=false;
    CHECK(gmem_init(&mem,&cfg)==PS5C_OK);guest_addr a;
    CHECK(gmem_map(&mem,0,8*GUEST_PAGE_SIZE,GPROT_RW,0,"failure fixture",&a)==PS5C_OK);
    CHECK(gmem_write(&mem,a,reader,sizeof(reader)));
    guest_addr input=a+0x1000,out=a+0x2000,label=a+0x3000;
    uint32_t one=1,seen=0;CHECK(gmem_write(&mem,input,&one,4));
    pal_window *window=NULL;CHECK(pal_window_open("ZeraPS5 work failure regression",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&mem);CHECK(r);
    gpu g;gpu_callbacks cb={.interrupt=interrupt};CHECK(gpu_init(&g,&mem,&cb)==PS5C_OK);
    g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(a>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(a>>40);
    g.regs[mmCOMPUTE_PGM_RSRC2]=16u<<1;
    g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    u[0]=(uint32_t)input;u[1]=(uint32_t)(input>>32);u[2]=4;u[3]=0x16204;
    u[12]=(uint32_t)out;u[13]=(uint32_t)(out>>32);u[14]=4;u[15]=0x16204;
    if(mode==GPU_PIPELINE_LOST)vkCreateComputePipelines=pipeline_stub;
    if(mode==GPU_MODULE_LOST)vkCreateShaderModule=module_stub;
    renderer_dispatch(r,&g,1,1,1);
    gpu_release release={.addr=label,.data=77,.data_sel=1,.interrupt=true,.queue_event=3};
    CHECK(renderer_release(r,&g,&release));
    CHECK(gmem_read(&mem,label,&seen,4) && !seen && !interrupts);
    real_submit=vkQueueSubmit;real_acquire=vkAcquireNextImageKHR;real_present=vkQueuePresentKHR;
    if(mode>=GPU_HEALTHY) {
        renderer_sync_mem(r,out,4);
        CHECK(gmem_read(&mem,out,&seen,4) && seen==1);
        CHECK(gmem_read(&mem,label,&seen,4) && seen==77 && interrupts==1);
        if(mode==GPU_OUT_OF_DATE)vkAcquireNextImageKHR=acquire_stub;
        if(mode==GPU_SUBOPTIMAL)vkQueuePresentKHR=present_stub;
        if(mode!=GPU_HEALTHY){renderer_present(r,NULL);renderer_present(r,NULL);CHECK(once);}
        renderer_destroy(r);free(g.regs);pal_mutex_destroy(g.lock);
        pal_window_destroy(window);gmem_free(&mem);return 0;
    }
    if(mode==GPU_WRITEBACK)vk_flush(r); /* work submitted; inject completion only */
    if(mode==GPU_SYNC || mode==GPU_WRITEBACK || mode==GPU_IDLE)vkWaitForFences=wait_stub;
    if(mode==GPU_SUBMIT || mode==GPU_PRESENT_SUBMIT)vkQueueSubmit=submit_stub;
    if(mode==GPU_PRESENT_SUBMIT)vkQueuePresentKHR=present_stub;
    if(mode==GPU_END)vkEndCommandBuffer=end_stub;
    if(mode==GPU_ACQUIRE)vkAcquireNextImageKHR=acquire_stub;
    if(mode==GPU_PRESENT_SUBMIT || mode==GPU_ACQUIRE)renderer_present(r,NULL);
    else if(mode==GPU_WRITEBACK)vk_writebacks_complete(r);
    else if(mode==GPU_IDLE)renderer_wait_idle(r);
    else renderer_sync_mem(r,out,4);
    return 93;
}

static int child(int which) {
    mode=which;SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    CHECK(pal_init()==PS5C_OK);
    int result=mode>=GPU_SYNC?gpu_child():recording_child();
    pal_shutdown();return result;
}

static int check_child(unsigned which,bool failure) {
    wchar_t path[32768],cmd[32832],log[100];
    CHECK(GetModuleFileNameW(NULL,path,PS5C_ARRAY_COUNT(path))>0);
    CHECK(swprintf(cmd,PS5C_ARRAY_COUNT(cmd),L"\"%ls\" --child %u",path,which)>0);
    CHECK(swprintf(log,PS5C_ARRAY_COUNT(log),L"vk-work-failure-%lu-%u.log",GetCurrentProcessId(),which)>0);
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
    else TerminateProcess(pi.hProcess,result);
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
    FILE *f=_wfopen(log,L"r");CHECK(f);char line[2048];bool logged=false;
    while(fgets(line,sizeof(line),f))logged|=strstr(line,"Vulkan:") && strstr(line,"failed");
    fclose(f);
    fprintf(stderr,"Vulkan work case%u: exit%lu, error logged%d\n",which,result,logged);
    CHECK(result==(failure?1u:0u) && logged==failure);
    return 0;
}

int main(int argc,char **argv) {
    if(argc==3 && !strcmp(argv[1],"--child"))return child(atoi(argv[2]));
    for(unsigned which=HEALTHY;which<=PRESENT_WAIT;which++)CHECK(check_child(which,which!=HEALTHY)==0);
    if(getenv("PS5C_TEST_GPU"))
        for(unsigned which=GPU_SYNC;which<=GPU_MODULE_LOST;which++)CHECK(check_child(which,which<GPU_HEALTHY || which>=GPU_PIPELINE_LOST)==0);
    puts("Vulkan failure guards: no fence reuse or false completion; success/presentation controls retained");
    return 0;
}
