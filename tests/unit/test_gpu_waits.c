/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
#include "gpu/gpu.h"
#include "gpu/gpu_regs.h"
#include "test.h"

static gmem mem;
static gpu g;
static guest_addr base;
static uint32_t interrupts;
static guest_addr sync_label;

static void irq(void *user, int32_t queue, uint32_t context) {
    PS5C_UNUSED(user); PS5C_UNUSED(queue); PS5C_UNUSED(context); interrupts++;
}
static void sync_read(void *user, guest_addr addr, uint64_t size) {
    PS5C_UNUSED(user); PS5C_UNUSED(size);
    if (sync_label && addr == sync_label) {
        uint32_t one = 1;
        gmem_write(&mem, addr, &one, 4);
        sync_label = 0;
    }
}
static void reset(void) {
    memset(gmem_host(&mem, base, GUEST_PAGE_SIZE), 0, GUEST_PAGE_SIZE);
    memset(g.regs, 0, GPU_REG_COUNT * 4);
    memset(&g.stats, 0, sizeof(g.stats));
    interrupts = 0; sync_label = 0;
}
static void put(guest_addr at, const uint32_t *p, uint32_t n) { gmem_write(&mem, at, p, n * 4); }
static uint32_t value(guest_addr at) { uint32_t v = ~0u; gmem_read(&mem, at, &v, 4); return v; }
static void atomic_add(uint32_t *p, guest_addr at) {
    uint32_t a[] = {0xc0061e00, 9, (uint32_t)at, (uint32_t)(at >> 32), 1, 0, 0, 0};
    memcpy(p, a, sizeof(a));
}
static void test_nested_wait_no_replay_or_early_irq(void) {
    reset();
    guest_addr root = base + 256, child = base + 1024, count = base + 8, label = base + 16;
    uint32_t r[20] = {0}, c[23] = {0};
    atomic_add(r, count); atomic_add(r + 12, count);
    r[8] = 0xc0023f00; r[9] = (uint32_t)child; r[10] = (uint32_t)(child >> 32); r[11] = 23;
    atomic_add(c, count); atomic_add(c + 15, count);
    uint32_t wait[] = {0xc0053c00, 0x13, (uint32_t)label, (uint32_t)(label >> 32), 1, ~0u, 4};
    memcpy(c + 8, wait, sizeof(wait)); put(root, r, 20); put(child, c, 23);
    gpu_submission s = {0};
    CHECK(gpu_submit_resume(&g, &s, root, 20, 0) == GPU_EXEC_BLOCKED);
    CHECK(value(count) == 2 && interrupts == 0);
    CHECK(gpu_submit_resume(&g, &s, root, 20, 0) == GPU_EXEC_BLOCKED);
    CHECK(value(count) == 2 && interrupts == 0);
    uint32_t one = 1; put(label, &one, 1);
    CHECK(gpu_submit_resume(&g, &s, root, 20, 0) == GPU_EXEC_DONE);
    CHECK(value(count) == 4 && interrupts == 1 && g.stats.submits == 1);
    CHECK(gpu_submit_resume(&g, &s, root, 20, 0) == GPU_EXEC_DONE);
    CHECK(value(count) == 4 && interrupts == 1);
}
static void test_chain_rewind_preserves_tail_jump(void) {
    reset();
    guest_addr root = base + 256, child = base + 1024, count = base + 8;
    uint32_t r[12] = {0xc0023f00, (uint32_t)child, (uint32_t)(child >> 32), 0x100000 | 10};
    atomic_add(r + 4, count);
    uint32_t c[10] = {0xc0005900, 0}; atomic_add(c + 2, count);
    put(root, r, 12); put(child, c, 10);
    gpu_submission s = {0};
    CHECK(gpu_submit_resume(&g, &s, root, 12, 0) == GPU_EXEC_BLOCKED);
    CHECK(value(count) == 0 && interrupts == 0 && s.wait_packet == child);
    uint32_t valid = 0x80000000; put(child + 4, &valid, 1);
    CHECK(gpu_submit_resume(&g, &s, root, 12, 0) == GPU_EXEC_DONE);
    CHECK(value(count) == 1 && interrupts == 1);
}
static void test_wait64_mask_and_high_word(void) {
    reset();
    guest_addr root = base + 256, label = base + 16, count = base + 8;
    uint32_t r[17] = {0xc0079300, 0x15, (uint32_t)label, (uint32_t)(label >> 32), 3, 1, 0xff, 1, 4};
    atomic_add(r + 9, count); put(root, r, 17);
    CHECK(gmem_write_u64(&mem, label, 0x100000002ull));
    gpu_submission s = {0};
    CHECK(gpu_submit_resume(&g, &s, root, 17, 0) == GPU_EXEC_BLOCKED);
    CHECK(value(count) == 0 && interrupts == 0);
    CHECK(gmem_write_u64(&mem, label, 0xabcdef010000ff03ull));
    CHECK(gpu_submit_resume(&g, &s, root, 17, 0) == GPU_EXEC_DONE);
    CHECK(value(count) == 1);
}
static void test_wait_reads_prior_gpu_results(void) {
    reset();
    guest_addr root = base + 256, label = base + 16;
    uint32_t r[] = {0xc0053c00, 0x13, (uint32_t)label, (uint32_t)(label >> 32), 1, ~0u, 4};
    put(root, r, 7); sync_label = label;
    gpu_submission s = {0};
    CHECK(gpu_submit_resume(&g, &s, root, 7, 0) == GPU_EXEC_DONE);
    CHECK(value(label) == 1 && !sync_label);
}
static void test_mem_semaphore_blocks_signals_and_consumes_once(void) {
    reset();
    guest_addr root=base+256,signal=base+512,counter=base+16,count=base+8;
    uint32_t wait[12]={0xc0023900,(uint32_t)counter,(uint32_t)(counter>>32),0xe0000000};
    atomic_add(wait+4,count);put(root,wait,12);
    gpu_submission parked={0};
    CHECK(gpu_submit_resume(&g,&parked,root,12,0)==GPU_EXEC_BLOCKED);
    CHECK(parked.wait_opcode==0x39 && parked.wait_packet==root && value(count)==0 && interrupts==0);
    CHECK(gpu_submit_resume(&g,&parked,root,12,0)==GPU_EXEC_BLOCKED && value(counter)==0);
    uint32_t notify[]={0xc0023900,(uint32_t)counter,(uint32_t)(counter>>32),0xc0010000};
    put(signal,notify,4);gpu_submission producer={0};
    CHECK(gpu_submit_resume(&g,&producer,signal,4,0)==GPU_EXEC_DONE && value(counter)==1);
    CHECK(gpu_submit_resume(&g,&parked,root,12,0)==GPU_EXEC_DONE);
    CHECK(value(counter)==0 && value(count)==1 && interrupts==2);
    CHECK(gpu_submit_resume(&g,&parked,root,12,0)==GPU_EXEC_DONE && value(count)==1);
    CHECK(gmem_write_u64(&mem,counter,UINT64_C(0x100000000)));
    parked=(gpu_submission){0};
    CHECK(gpu_submit_resume(&g,&parked,root,12,0)==GPU_EXEC_DONE);
    uint64_t wide=0;CHECK(gmem_read_u64(&mem,counter,&wide) && wide==UINT64_C(0xffffffff));
    notify[3]=0xc0100000;put(signal,notify,4);producer=(gpu_submission){0};
    CHECK(gpu_submit_resume(&g,&producer,signal,4,0)==GPU_EXEC_DONE);
    CHECK(gmem_read_u64(&mem,counter,&wide) && wide==1);
    CHECK(gmem_write_u64(&mem,counter,0));sync_label=counter;parked=(gpu_submission){0};
    CHECK(gpu_submit_resume(&g,&parked,root,12,0)==GPU_EXEC_DONE && !sync_label && value(counter)==0);
}
static void test_mem_semaphore_refuses_invalid_packets_and_memory(void) {
    reset();
    uint32_t packet[]={0xc0023900,(uint32_t)(base+16),(uint32_t)((base+16)>>32),0xc0000001};
    put(base+256,packet,4);gpu_submission s={0};
    CHECK(gpu_submit_resume(&g,&s,base+256,4,0)==GPU_EXEC_FAULT && interrupts==0);
    packet[3]=0;put(base+256,packet,4);s=(gpu_submission){0};
    CHECK(gpu_submit_resume(&g,&s,base+256,4,0)==GPU_EXEC_FAULT && interrupts==0);
    guest_addr missing=base+(1u<<20);CHECK(!gmem_is_mapped(&mem,missing,8));
    packet[1]=(uint32_t)missing;packet[2]=(uint32_t)(missing>>32);packet[3]=0xc0000000;
    put(base+256,packet,4);s=(gpu_submission){0};
    CHECK(gpu_submit_resume(&g,&s,base+256,4,0)==GPU_EXEC_FAULT && interrupts==0);
}
static int32_t cpu_signal_counter(void *arg) {
    uint64_t *counter = arg;
    for (unsigned i = 0; i < 4096; i++) __atomic_fetch_add(counter, 1, __ATOMIC_SEQ_CST);
    return 0;
}
static void test_mem_semaphore_keeps_concurrent_cpu_signals(void) {
    reset();
    guest_addr counter = base + 16, root = base + 256;
    uint32_t packet[] = {0xc0023900, (uint32_t)counter, (uint32_t)(counter >> 32), 0xc0000000};
    put(root, packet, 4);
    pal_thread *thread = NULL;
    CHECK(pal_thread_create(&thread, cpu_signal_counter, gmem_host(&mem, counter, 8), "CPU semaphore", 0) == PS5C_OK);
    bool ok = true;
    for (unsigned i = 0; i < 4096; i++) {
        gpu_submission s = {0};
        if (gpu_submit_resume(&g, &s, root, 4, 0) != GPU_EXEC_DONE) ok = false;
    }
    int32_t ret = -1;
    CHECK(pal_thread_join(thread, &ret) == PS5C_OK && ret == 0 && ok);
    uint64_t wide = 0;
    CHECK(gmem_read_u64(&mem, counter, &wide) && wide == 8192);
    CHECK(gmem_write_u64(&mem, counter, UINT64_MAX));
    gpu_submission s = {0};
    CHECK(gpu_submit_resume(&g, &s, root, 4, 0) == GPU_EXEC_DONE);
    CHECK(gmem_read_u64(&mem, counter, &wide) && wide == 0);
}
static void test_atomic_counter_validates_mapping_and_outputs(void) {
    reset();
    uint64_t expected = 7;
    bool exchanged = true;
    CHECK(gmem_write_u64(&mem, base + 16, 5));
    CHECK(gmem_compare_exchange_u64(&mem, base + 16, &expected, 9, &exchanged));
    CHECK(!exchanged && expected == 5 && value(base + 16) == 5);
    CHECK(gmem_compare_exchange_u64(&mem, base + 16, &expected, 9, &exchanged));
    CHECK(exchanged && expected == 5 && value(base + 16) == 9);
    expected = 17; exchanged = true;
    CHECK(!gmem_compare_exchange_u64(&mem, base + 17, &expected, 0, &exchanged));
    CHECK(expected == 17 && exchanged);
    CHECK(!gmem_compare_exchange_u64(&mem, base + (1u << 20), &expected, 0, &exchanged));
    CHECK(expected == 17 && exchanged);
    guest_addr page = 0;
    CHECK(gmem_map(&mem, 0, GUEST_PAGE_SIZE, GPROT_READ, 0, "readonly counter", &page) == PS5C_OK);
    CHECK(!gmem_compare_exchange_u64(&mem, page, &expected, 0, &exchanged));
    CHECK(expected == 17 && exchanged);
    CHECK(gmem_protect(&mem, page, GUEST_PAGE_SIZE, GPROT_NONE) == PS5C_OK);
    CHECK(!gmem_compare_exchange_u64(&mem, page, &expected, 0, &exchanged));
    CHECK(expected == 17 && exchanged);
    CHECK(gmem_unmap(&mem, page, GUEST_PAGE_SIZE) == PS5C_OK);
}
static void test_unsupported_wait_fails_without_completion(void) {
    reset();
    uint32_t r[] = {0xc0053c00, 0x17, (uint32_t)(base + 16), (uint32_t)((base + 16) >> 32), 1, ~0u, 4};
    put(base + 256, r, 7); gpu_submission s = {0};
    CHECK(gpu_submit_resume(&g, &s, base + 256, 7, 0) == GPU_EXEC_FAULT);
    CHECK(interrupts == 0 && !s.done);
}
static void conditional_ib(uint32_t *p, uint32_t control, guest_addr compare, uint64_t mask,
                           uint64_t reference, guest_addr pass, uint32_t pass_size,
                           guest_addr fail, uint32_t fail_size) {
    uint32_t words[] = {0xc00c3f00, control, (uint32_t)compare, (uint32_t)(compare >> 32),
        (uint32_t)mask, (uint32_t)(mask >> 32), (uint32_t)reference, (uint32_t)(reference >> 32),
        (uint32_t)pass, (uint32_t)(pass >> 32), pass_size,
        (uint32_t)fail, (uint32_t)(fail >> 32), fail_size};
    memcpy(p, words, sizeof(words));
}
static void conditional_chain(guest_addr first, guest_addr count) {
    for (uint32_t i = 0; i < 12; i++) {
        uint32_t p[22];
        conditional_ib(p, 1, base + 16, UINT64_MAX, 0, first + (i+1)*128, i==11 ? 8 : 22, 0, 0);
        atomic_add(p+14,count); /* a tail jump must not return here */
        put(first+i*128,p,22);
    }
    uint32_t end[8];atomic_add(end,count);put(first+12*128,end,8);
}
static void test_conditional_chain_keeps_real_call_depth(void) {
    reset();
    guest_addr root=base+256,first=base+1024,count=base+8;
    conditional_chain(first,count);
    uint32_t p[20]={0};atomic_add(p,count);atomic_add(p+12,count);
    p[8]=0xc0023f00;p[9]=(uint32_t)first;p[10]=(uint32_t)(first>>32);p[11]=22;
    put(root,p,20);
    sync_label=base+16; /* ALWAYS must not synchronize or fetch a predicate */
    gpu_submission s={0};
    CHECK(gpu_submit_resume(&g,&s,root,20,0)==GPU_EXEC_DONE);
    CHECK(value(count)==3 && interrupts==1 && sync_label==base+16);
    CHECK(!s.depth && s.done);
    reset();conditional_chain(first,count);
    gpu_submit(&g,first,22,0);
    CHECK(value(count)==1 && interrupts==1);
}
static void test_conditional_chain_wait_resumes_only_selected_path(void) {
    reset();
    guest_addr root=base+256,branch=base+512,target=base+1024,label=base+16,count=base+8;
    uint32_t p[20]={0};atomic_add(p,count);atomic_add(p+12,count);
    p[8]=0xc0023f00;p[9]=(uint32_t)branch;p[10]=(uint32_t)(branch>>32);p[11]=22;
    uint32_t b[22];conditional_ib(b,1,0,0,0,target,15,0,0);atomic_add(b+14,count);
    uint32_t t[15]={0xc0053c00,0x13,(uint32_t)label,(uint32_t)(label>>32),1,~0u,4};
    atomic_add(t+7,count);put(root,p,20);put(branch,b,22);put(target,t,15);
    gpu_submission s={0};
    CHECK(gpu_submit_resume(&g,&s,root,20,0)==GPU_EXEC_BLOCKED);
    CHECK(value(count)==1 && interrupts==0 && s.depth==2 && s.frame[1].addr==target);
    CHECK(gpu_submit_resume(&g,&s,root,20,0)==GPU_EXEC_BLOCKED);
    CHECK(value(count)==1 && interrupts==0);
    uint32_t ready=1;put(label,&ready,1);
    CHECK(gpu_submit_resume(&g,&s,root,20,0)==GPU_EXEC_DONE);
    CHECK(value(count)==3 && interrupts==1);
}
static void test_conditional_four_byte_predicate_and_else(void) {
    reset();
    guest_addr root=base+256,pass=base+512,fail=base+768,yes=base+8,no=base+12;
    guest_addr predicate=base+20; /* four-byte aligned, not eight */
    const uint64_t data=UINT64_C(0x1234567887654321);
    uint32_t pad=0xdeadbeef;put(base+16,&pad,1);
    CHECK(gmem_write_u64(&mem,predicate,data));
    uint32_t a[8],b[8],p[14];atomic_add(a,yes);atomic_add(b,no);put(pass,a,8);put(fail,b,8);
    conditional_ib(p,2|(3u<<8),predicate,UINT64_MAX,data,pass,8,fail,8);put(root,p,14);
    gpu_submission s={0};CHECK(gpu_submit_resume(&g,&s,root,14,0)==GPU_EXEC_DONE);
    CHECK(value(yes)==1 && value(no)==0);
    conditional_ib(p,2|(3u<<8),predicate,UINT64_MAX,data+1,pass,8,fail,8);put(root,p,14);
    s=(gpu_submission){0};CHECK(gpu_submit_resume(&g,&s,root,14,0)==GPU_EXEC_DONE);
    CHECK(value(yes)==1 && value(no)==1);
}
static void test_conditional_cycle_yields_without_false_call_depth(void) {
    reset();guest_addr root=base+256,count=base+8;
    uint32_t p[22];conditional_ib(p,1,0,0,0,root,22,0,0);atomic_add(p+14,count);put(root,p,22);
    gpu_submission s={0};CHECK(gpu_submit_resume(&g,&s,root,22,0)==GPU_EXEC_YIELD);
    CHECK(!s.fault && s.depth==1 && value(count)==0 && interrupts==0);
    reset();put(root,p,22);gpu_submit(&g,root,22,0);
    CHECK(value(count)==0 && interrupts==0);
}
static void test_cyclic_chain_yields(void) {
    reset();
    guest_addr root = base + 256;
    uint32_t r[] = {0xc0023f00, (uint32_t)root, (uint32_t)(root >> 32), 0x100004};
    put(root, r, 4); gpu_submission s = {0};
    CHECK(gpu_submit_resume(&g, &s, root, 4, 0) == GPU_EXEC_YIELD);
    CHECK(interrupts == 0 && !s.done);
}
static void legacy_chain(uint32_t opcode, guest_addr first, guest_addr count) {
    /* More links than the real CALL nesting limit. Every abandoned tail has
     * an observable side effect, so treating CHAIN as CALL is also detected. */
    for (uint32_t i = 0; i < 12; i++) {
        guest_addr next = first + (i + 1) * 128;
        uint32_t p[12] = {0xc0020000 | (opcode << 8), (uint32_t)next,
                         (uint32_t)(next >> 32), 0x100000 | (i == 11 ? 8 : 12)};
        atomic_add(p + 4, count);
        put(first + i * 128, p, 12);
    }
    uint32_t end[8]; atomic_add(end, count); put(first + 12 * 128, end, 8);
}
static void test_legacy_chain_reaches_completion_without_tail_replay(void) {
    reset();
    guest_addr first = base + 1024, count = base + 8;
    legacy_chain(0x3f, first, count);
    gpu_submit(&g, first, 12, 0);
    CHECK(value(count) == 1 && interrupts == 1);
    reset();
    legacy_chain(0x33, first, count);
    gpu_submit(&g, first, 12, 0);
    CHECK(value(count) == 1 && interrupts == 1);
}
static void test_legacy_chain_returns_to_real_caller(void) {
    reset();
    guest_addr root = base + 256, first = base + 1024, count = base + 8;
    legacy_chain(0x3f, first, count);
    uint32_t p[20] = {0}; atomic_add(p, count); atomic_add(p + 12, count);
    p[8] = 0xc0023f00; p[9] = (uint32_t)first; p[10] = (uint32_t)(first >> 32); p[11] = 12;
    put(root, p, 20); gpu_submit(&g, root, 20, 0);
    CHECK(value(count) == 3 && interrupts == 1);
}
static void test_legacy_cycle_is_bounded(void) {
    reset();
    guest_addr root = base + 256, count = base + 8;
    uint32_t p[12] = {0xc0023f00, (uint32_t)root, (uint32_t)(root >> 32), 0x10000c};
    atomic_add(p + 4, count); put(root, p, 12);
    gpu_submit(&g, root, 12, 0);
    CHECK(value(count) == 0 && interrupts == 0);
    /* A fault inside a genuine CALL must suppress its parent's tail/IRQ. */
    reset();
    guest_addr parent = base + 512;
    uint32_t call[12] = {0xc0023f00, (uint32_t)root, (uint32_t)(root >> 32), 12};
    atomic_add(call + 4, count); put(root, p, 12); put(parent, call, 12);
    gpu_submit(&g, parent, 12, 0);
    CHECK(value(count) == 0 && interrupts == 0);
}
static void test_legacy_chain_unmapped_target_has_no_completion(void) {
    reset();
    guest_addr root = base + 256, missing = base + (1u << 20), count = base + 8;
    CHECK(!gmem_is_mapped(&mem, missing, 16));
    uint32_t p[12] = {0xc0023f00, (uint32_t)missing, (uint32_t)(missing >> 32), 0x100004};
    atomic_add(p + 4, count); put(root, p, 12);
    gpu_submit(&g, root, 12, 0);
    CHECK(value(count) == 0 && interrupts == 0);
}
static void test_dispatch_wave_initiator(void) {
    reset();
    const uint32_t direct[] = {0xc0031500,1,1,1,0x8041};
    put(base+256,direct,5); gpu_submit(&g,base+256,5,0);
    CHECK(g.regs[mmCOMPUTE_DISPATCH_INITIATOR]==0x8041 && g.stats.dispatches==1);
    const uint32_t dims[] = {1,1,1}; put(base+64,dims,3);
    const uint32_t indirect[] = {0xc0021602,(uint32_t)(base+64),(uint32_t)((base+64)>>32),0x41};
    put(base+256,indirect,4); gpu_submit(&g,base+256,4,0);
    CHECK(g.regs[mmCOMPUTE_DISPATCH_INITIATOR]==0x41 && g.stats.dispatches==2);
    g.indirect_base[1]=base+64;
    const uint32_t graphics[] = {0xc0011600,0,0x8041};
    put(base+256,graphics,3); gpu_submit(&g,base+256,3,0);
    CHECK(g.regs[mmCOMPUTE_DISPATCH_INITIATOR]==0x8041 && g.stats.dispatches==3);
}
int main(void) {
    pal_init(); gmem_config cfg = gmem_default_config(); cfg.identity = false;
    if (gmem_init(&mem, &cfg) != PS5C_OK ||
        gmem_map(&mem, GUEST_MODULE_BASE, GUEST_PAGE_SIZE, GPROT_RW, GMAP_FIXED, "CP waits test", &base) != PS5C_OK) return 2;
    gpu_callbacks cb = {.interrupt=irq, .sync_mem=sync_read};
    if (gpu_init(&g, &mem, &cb) != PS5C_OK) return 2;
    const test_case cases[] = {TEST_CASE(test_dispatch_wave_initiator), TEST_CASE(test_nested_wait_no_replay_or_early_irq), TEST_CASE(test_chain_rewind_preserves_tail_jump),
        TEST_CASE(test_conditional_chain_keeps_real_call_depth),
        TEST_CASE(test_conditional_chain_wait_resumes_only_selected_path),
        TEST_CASE(test_conditional_four_byte_predicate_and_else),
        TEST_CASE(test_conditional_cycle_yields_without_false_call_depth),
        TEST_CASE(test_wait64_mask_and_high_word), TEST_CASE(test_wait_reads_prior_gpu_results),
        TEST_CASE(test_mem_semaphore_blocks_signals_and_consumes_once),
        TEST_CASE(test_mem_semaphore_refuses_invalid_packets_and_memory),
        TEST_CASE(test_mem_semaphore_keeps_concurrent_cpu_signals),
        TEST_CASE(test_atomic_counter_validates_mapping_and_outputs),
        TEST_CASE(test_unsupported_wait_fails_without_completion), TEST_CASE(test_cyclic_chain_yields),
        TEST_CASE(test_legacy_chain_reaches_completion_without_tail_replay),
        TEST_CASE(test_legacy_chain_returns_to_real_caller), TEST_CASE(test_legacy_cycle_is_bounded),
        TEST_CASE(test_legacy_chain_unmapped_target_has_no_completion)};
    return test_run_all("gpu_waits", cases, PS5C_ARRAY_COUNT(cases));
}
