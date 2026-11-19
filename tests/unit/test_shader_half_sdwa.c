/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Half SDWA float modifiers apply after selecting the source half. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/gpu_regs.h"
#include "gpu/renderer.h"
#include "gpu/shader/shader.h"
#include "gpu/shader/shader_ops.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
/* Hand-encoded synthetic multiplication, no commercial shader bytes. */
static uint32_t code[] = {
    0x7e000200, 0x7e020201, 0x7e040202, /* v0=s0, v1=s1, v2=s2 */
    0x6a0402f9, 0x14041400, /* v_mul_f16 v2, v0, -v1 (SDWA, preserve high) */
    0xe0700000, 0x80030200, /* buffer_store_dword v2, s[12:15] */
    0xbf810000
};
static const struct { uint32_t control, expected; } cases[] = {
    {0x04041400, 0xc000}, /* -1 * +2 = -2 */
    {0x14041400, 0x4000}, /* -1 * -(+2) = +2 */
    {0x04141400, 0x4000}, /* -(-1) * +2 = +2 */
    {0x14141400, 0xc000}, /* -(-1) * -(+2) = -2 */
    {0x04241400, 0x4000}, /* abs(-1) * +2 = +2 */
    {0x04341400, 0xc000}, /* -abs(-1) * +2 = -2 */
    {0x25041400, 0xc000}, /* -1 * abs(high=-2) = -2 */
    {0x35041400, 0x4000}, /* -1 * -abs(high=-2) = +2 */
};
static int gpu_test(void) {
    CHECK(pal_init() == PS5C_OK);
    gmem m; gmem_config cfg = gmem_default_config(); cfg.identity = false;
    CHECK(gmem_init(&m, &cfg) == PS5C_OK);
    guest_addr a;
    CHECK(gmem_map(&m, 0, 4 * GUEST_PAGE_SIZE, GPROT_RW, 0, "half SDWA", &a) == PS5C_OK);
    uint32_t *output = gmem_host(&m, a + 0x1000, 4); CHECK(output);
    pal_window *window=NULL;
    CHECK(pal_window_open("ZeraPS5 half SDWA regression",320,180,&window)==PS5C_OK);
    renderer *r=renderer_create(window,&m); CHECK(r);
    gpu g; CHECK(gpu_init(&g,&m,NULL)==PS5C_OK);
    g.regs[mmCOMPUTE_PGM_RSRC2]=16u<<1;
    g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
    uint32_t *u=g.regs+mmCOMPUTE_USER_DATA_0;
    u[0]=0x3c00bc00; u[1]=0xc0004000; u[2]=0x35553a00;
    u[12]=(uint32_t)(a+0x1000);u[13]=(uint32_t)((a+0x1000)>>32);u[14]=4;u[15]=0x00016204;
    for (unsigned i=0;i<PS5C_ARRAY_COUNT(cases);i++) {
        code[4]=cases[i].control;
        guest_addr pc=a+i*0x100;
        CHECK(gmem_write(&m,pc,code,sizeof(code)));
        g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)(pc>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)(pc>>40);
        *output=0xdeadbeef;
        renderer_submit_begin(r);renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,a+0x1000,4);
        uint32_t expected=0x35550000|cases[i].expected;
        if (*output!=expected) fprintf(stderr,"case %u control %08x: %08x expected %08x\n",i,cases[i].control,*output,expected);
        CHECK(*output==expected);
    }
    renderer_destroy(r); free(g.regs);pal_mutex_destroy(g.lock);pal_window_destroy(window);gmem_free(&m);pal_shutdown();
    puts("half SDWA GPU: eight sign/abs combinations and untouched high half verified");return 0;
}
int main(void) {
    if (getenv("PS5C_TEST_GPU")) CHECK(gpu_test()==0);
    sh_program *p=calloc(1,sizeof(*p));CHECK(p);
    sh_stage_info info={.stage=SH_STAGE_CS,.cs_tgid_sgpr=16,.local_size={1,1,1}};
    sh_site_shape shape={0};uint32_t *spv=NULL;size_t words;
    code[4]=0x14041400;
    CHECK(sh_analyze(code,PS5C_ARRAY_COUNT(code),0x100000,p));
    CHECK(sh_translate(p,&info,&shape,0,&spv,&words));
    unsigned neg=0;
    for(size_t i=5;i<words;i+=spv[i]>>16) neg+=(spv[i]&0xffff)==127; /* OpFNegate */
    CHECK(neg>0);
    free(spv);free(p);puts("half SDWA: ok");return 0;
}
