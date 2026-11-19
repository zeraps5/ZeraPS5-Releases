/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/vk/vk_ngg_group.h"
#include "gpu/vk/vk_ngg_fast_launch.h"
#include "base/base.h"
#include "test.h"
static void test_output_and_partition_coverage(void) {
    CHECK(vk_ngg_group_threads(80,4,20)==128); /* observed Astro GS */
    CHECK(vk_ngg_group_threads(216,72,3)==256); /* observed UI and vertex192 */
    CHECK(vk_ngg_group_threads(80,1,3)==128); /* compiled limit exceeds actual draw */
    CHECK(vk_ngg_group_threads(80,4,64)==256); /* never repartition the inputs */
    CHECK(vk_ngg_group_threads(64,1,64)==64);
    CHECK(vk_ngg_group_threads(65,1,1)==128);
    CHECK(vk_ngg_group_threads(128,1,1)==128);
    CHECK(vk_ngg_group_threads(129,1,1)==256);
    for(uint32_t out=1;out<=256;out++)for(uint32_t max=1;max<=256;max++)
        for(uint32_t in=1;in<=64;in++) {
            uint32_t n=vk_ngg_group_threads(out,max,in);
            CHECK(n==64 || n==128 || n==256);
            if((uint64_t)max*in<=256)CHECK(n>=out && n>=max*in);
            else CHECK(n==256);
        }
}
static void test_unknown_limits_keep_full_group(void) {
    CHECK(vk_ngg_group_threads(0,4,20)==256);
    CHECK(vk_ngg_group_threads(257,4,20)==256);
    CHECK(vk_ngg_group_threads(80,0,20)==256);
    CHECK(vk_ngg_group_threads(80,4,0)==256);
    CHECK(vk_ngg_group_threads(1,1,65)==256);
    CHECK(vk_ngg_group_threads(UINT32_MAX,UINT32_MAX,UINT32_MAX)==256);
}
static void test_fast_launch_contract(void) {
    CHECK(vk_ngg_fast_launch_threads(0x482030,0x201,1,32,32,64,false)==64);
    CHECK(vk_ngg_fast_launch_threads(0x482030,0x201,1,64,64,128,false)==128);
    CHECK(vk_ngg_fast_launch_threads(0x482030,0x201,1,64,64,192,false)==256);
    CHECK(!vk_ngg_fast_launch_threads(0x502030,0x201,1,32,32,64,false)); /* mode2 */
    CHECK(!vk_ngg_fast_launch_threads(0x082030,0x201,1,32,32,64,false)); /* wave64 */
    CHECK(!vk_ngg_fast_launch_threads(0x482030,0x402,1,32,32,64,false));
    CHECK(!vk_ngg_fast_launch_threads(0x482030,0x201,4,32,32,64,false));
    CHECK(!vk_ngg_fast_launch_threads(0x482030,0x201,1,32,32,64,true));
    CHECK(!vk_ngg_fast_launch_threads(0x482030,0x201,1,33,32,64,false));
    CHECK(!vk_ngg_fast_launch_threads(0x482030,0x201,1,32,32,0,false));
}
static void test_culling_stage_contract(void) {
    CHECK(vk_ngg_culling_triangle(0x00012010)); /* RADV NGG VS, stale GS limit is irrelevant */
    CHECK(!vk_ngg_culling_triangle(0x02012010)); /* passthrough uses another input ABI */
    CHECK(!vk_ngg_culling_triangle(0x00012030)); /* real GS */
    CHECK(!vk_ngg_culling_triangle(0x00012014)); /* tessellation */
    CHECK(!vk_ngg_culling_triangle(0x00010010)); /* legacy ES */
    CHECK(!vk_ngg_culling_triangle(0x00012008)); /* ES is a TES */
}
int main(void) {
    const test_case tests[]={TEST_CASE(test_output_and_partition_coverage),TEST_CASE(test_unknown_limits_keep_full_group),TEST_CASE(test_culling_stage_contract)};
    test_fast_launch_contract();return test_run_all("ngg_group",tests,PS5C_ARRAY_COUNT(tests));
}
