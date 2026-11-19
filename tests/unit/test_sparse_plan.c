/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/vk/vk_sparse_plan.h"
#include "test.h"

static VkSparseImageMemoryRequirements requirements(void) {
    return (VkSparseImageMemoryRequirements){
        .formatProperties={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.imageGranularity={256,256,1}},
        .imageMipTailFirstLod=5,.imageMipTailOffset=16u<<20,.imageMipTailSize=65536};
}
static void test_complete_resident_levels(void) {
    VkMemoryRequirements mem={.size=32u<<20,.alignment=65536,.memoryTypeBits=1};
    VkSparseImageMemoryRequirements req=requirements();vk_sparse_plan p;
    CHECK(vk_sparse_plan_build(4096,4096,13,5,&mem,&req,&p));
    CHECK(!p.tile_count && p.has_tail && p.bytes==65536);
    CHECK(p.tail.resourceOffset==req.imageMipTailOffset && p.tail.size==65536 && !p.tail.memoryOffset);
    CHECK(vk_sparse_plan_build(4096,4096,13,4,&mem,&req,&p));
    CHECK(p.tile_count==1 && p.bytes==131072 && p.tail.memoryOffset==65536);
    CHECK(p.tiles[0].subresource.mipLevel==4 && p.tiles[0].extent.width==256 && p.tiles[0].extent.height==256);
    CHECK(vk_sparse_plan_build(4096,4096,13,3,&mem,&req,&p));
    CHECK(p.tile_count==2 && p.bytes==6u*65536);
    CHECK(p.tiles[1].memoryOffset==4u*65536 && p.tail.memoryOffset==5u*65536);
    req.formatProperties.imageGranularity.width=512; /* BC1 rectangular block */
    CHECK(vk_sparse_plan_build(4096,4096,13,3,&mem,&req,&p));
    CHECK(p.tile_count==2 && p.bytes==4u*65536);
}
static void test_boundaries_and_no_tail(void) {
    VkMemoryRequirements mem={.size=32u<<20,.alignment=65536};
    VkSparseImageMemoryRequirements req=requirements();vk_sparse_plan p;
    req.imageMipTailFirstLod=16;req.imageMipTailSize=0;
    CHECK(vk_sparse_plan_build(1000,513,3,1,&mem,&req,&p));
    CHECK(p.tile_count==2 && !p.has_tail && p.bytes==3u*65536);
    CHECK(p.tiles[0].extent.width==500 && p.tiles[0].extent.height==256);
    CHECK(p.tiles[1].extent.width==250 && p.tiles[1].extent.height==128);
    req=requirements();
    CHECK(!vk_sparse_plan_build(4096,4096,13,0,&mem,&req,&p));
    CHECK(!vk_sparse_plan_build(4096,4096,13,13,&mem,&req,&p));
    CHECK(!vk_sparse_plan_build(4096,4096,17,5,&mem,&req,&p));
    mem.alignment=3;CHECK(!vk_sparse_plan_build(4096,4096,13,5,&mem,&req,&p));
    mem.alignment=65536;req.formatProperties.imageGranularity.width=0;
    CHECK(!vk_sparse_plan_build(4096,4096,13,5,&mem,&req,&p));
    req=requirements();req.formatProperties.aspectMask=VK_IMAGE_ASPECT_METADATA_BIT;
    CHECK(!vk_sparse_plan_build(4096,4096,13,5,&mem,&req,&p));
    req=requirements();req.imageMipTailOffset=mem.size-1;
    CHECK(!vk_sparse_plan_build(4096,4096,13,5,&mem,&req,&p));
    req=requirements();mem.size=65536;req.imageMipTailOffset=0;
    CHECK(!vk_sparse_plan_build(4096,4096,13,5,&mem,&req,&p)); /* no saving */
    mem.size=UINT64_MAX;mem.alignment=UINT64_C(1)<<63;req.imageMipTailSize=0;
    req.imageMipTailFirstLod=16;req.formatProperties.imageGranularity=(VkExtent3D){1,1,1};
    CHECK(!vk_sparse_plan_build(UINT32_MAX,UINT32_MAX,16,1,&mem,&req,&p));
}
int main(void) {
    const test_case tests[]={TEST_CASE(test_complete_resident_levels),TEST_CASE(test_boundaries_and_no_tail)};
    return test_run_all("sparse_plan",tests,PS5C_ARRAY_COUNT(tests));
}
