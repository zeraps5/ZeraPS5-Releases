/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "base/base.h"
#include "gpu/vk/vk_internal.h"
#include "gpu/shader/shader.h"
#include "gpu/shader/spv.h"
#include "gpu/gpu_regs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"mesh probe failed line %d: %s\n",__LINE__,#x);fflush(NULL);_Exit(1);}}while(0)
#include "test_mesh_native_spv.inc"
static const uint32_t guest_code[]={0xbefc03ff,0x00001003,0xbf900009,
 0xbefe0481,0x7e5002ff,0x00200400u,0xf8000941,0x00000028,
 0xbefe0487,
 0x36000a81,0x2c020a81,0x36020281,0x7e000d00,0x7e020d01,
 0x420000f6,0xbf800000,0x420202f6,0xbf800000,0x7e040280,0x7e0602f2,
 0x34120a82,0xd8340000,0x00000009,0xbf8cc07f,0xd8d80000,0x00000009,0xbf8cc07f,
 0xf80008cf,0x03020100,0xbf810000};
static const uint32_t raw_payloads[]={0xffffffff,0x80000001,0x00003c00,0x7fa12345};
static unsigned workgroup_controls(const uint32_t *spv,size_t words) {
 uint32_t *constants=calloc(spv[3],sizeof(*constants));CHECK(constants);unsigned n=0;
 for(size_t i=5;i<words;i+=spv[i]>>16)
  if((spv[i]&65535)==SpvOpConstant && (spv[i]>>16)==4)constants[spv[i+2]]=spv[i+3];
 for(size_t i=5;i<words;i+=spv[i]>>16)
  if((spv[i]&65535)==SpvOpControlBarrier && constants[spv[i+1]]==2)n++;
 free(constants);return n;
}
static int translate_guest(bool culled,bool passthrough,unsigned two_waves,uint32_t**out,size_t*words) {
 /* Synthetic NGG: allocate, emit packed triangle, then positions. The
  * X coordinate round-trips through actual shared LDS before export. */
 uint32_t code[1024];size_t count=PS5C_ARRAY_COUNT(guest_code);
 memcpy(code,guest_code,sizeof(guest_code));
 if((two_waves && two_waves<4) || two_waves==5){
  /* Wave one supplies vertex64. Its W must observe wave zero's LDS write;
   * only wave zero enters the first LDS phase, testing divergent rendezvous. */
  const uint32_t phase[]={
   /* Reconstruct the guest lane as real NGG does. Vertex32 must be distinct
    * from vertex0 even when the host uses native subgroup32. */
   0xd7650005,0x000100c1,0xd7660005,0x00020ac1,
   0x9386ff03,0x00040018,0xd7460005,0x04150c06,
   0x9384ff03,0x00080000,0x92fe8004,0xbf880006,
   0x7e0002ff,0x3e800000,0x34120a82,0xd8340000,0x00000009,0xbf8cc07f,
   0xbefe04c1,0xbf8a0000,0x7e120280,0xd8d80000,0x0a000009,0xbf8cc07f,0x101414f6};
  code[1]=0x00001043;code[5]=0x04008000;code[9]=0x2c000a85;code[10]=0x2c020a86;code[19]=0x7e06030a;
  memmove(code+8+PS5C_ARRAY_COUNT(phase),code+9,(count-9)*4);
  memcpy(code+8,phase,sizeof(phase));count+=PS5C_ARRAY_COUNT(phase)-1;
  size_t xmask=9+PS5C_ARRAY_COUNT(phase);
  memmove(code+xmask+1,code+xmask,(count-xmask)*4);code[xmask]=0x36000081;count++;
  if(two_waves==3) {
   /* Delay wave zero, then exchange LDS through four successive barrier
    * generations before the normal shader reuses the same storage. */
   unsigned at=0;while(at<count && code[at]!=0xbf880006)at++;CHECK(at<count);
   const uint32_t delay[]={0xb00a1000,0x818a810a,0xbf07800a,0xbf85fffd};
   memmove(code+at+1+PS5C_ARRAY_COUNT(delay),code+at+1,(count-at-1)*4);
   memcpy(code+at+1,delay,sizeof(delay));code[at]+=PS5C_ARRAY_COUNT(delay);count+=PS5C_ARRAY_COUNT(delay);
   at=0;while(at<count && code[at]!=0x101414f6)at++;CHECK(at<count);
   const uint32_t generations[]={0x7e16030a,0xbf8a0000,0xbf068006,0xbf840007,
    0x7e0402ff,0x3f400000,0x34120a82,0xd8340000,0x00000209,0xbf8cc07f,0x7e120280,
    0xbf8a0000,0xd8d80000,0x0a000009,0xbf8cc07f,0x0614170a,0xbf8a0000};
   memmove(code+at+PS5C_ARRAY_COUNT(generations),code+at+1,(count-at-1)*4);
   memcpy(code+at,generations,sizeof(generations));count+=PS5C_ARRAY_COUNT(generations)-1;
  }
 }
 if((two_waves>1 && two_waves<4) || two_waves==5) {
  /* Two entries into a later counted loop require the PC fallback, even
   * though the earlier LDS phase has converged across the entire group. */
  const uint32_t suffix[]={0xb00a0003,0xbf068103,0xbf840002,0xbf068000,0xbf840001,
                          0xbf800000,0x818a810a,0xbf07800a,0xbf85fffc,0xbf810000};
  memcpy(code+count-1,suffix,sizeof(suffix));count+=PS5C_ARRAY_COUNT(suffix)-1;
  if(two_waves==5) { code[count-1]=0xbf8a0000;code[count++]=0xbf810000; }
 }
 if(two_waves==4) {
  memmove(code+12,code+8,(count-8)*4);count+=4;
  code[8]=0x9386ff03;code[9]=0x00040018;code[10]=0xbf078006;
  unsigned at=0;while(at<count && code[at]!=0xd8d80000)at++;CHECK(at<count);
  memmove(code+at+1,code+at,(count-at)*4);code[at]=0xbf8a0000;count++;
  code[11]=0xbf850000u|(uint32_t)(count-13); /* other wave terminates before barrier */
 }
 bool ballot_stress=getenv("PS5C_TEST_BALLOT_LOAD")!=NULL;
 if(ballot_stress) {
  /* Rotate a single true bit through all64 lanes. Scalar OR must see both
   * complete half-masks, even in the opposite native subgroup. NAND of
   * the final halves is zero only if both are UINT32_MAX. Feed that check
   * into exported W so a missing half produces different GPU pixels. */
  const uint32_t stress[]={0xbefe04c1,0x36280abf,0xbe900480,0xb00f0040,
   0x818f810f,0x7d84280f,0x88906a10,0xbf07800f,0xbf85fffb,0x8c121110,0xbefe0481};
  memmove(code+8+PS5C_ARRAY_COUNT(stress),code+8,(count-8)*4);
  memcpy(code+8,stress,sizeof(stress));count+=PS5C_ARRAY_COUNT(stress);
  unsigned at=0;while(at<count && code[at]!=0xf80008cf)at++;CHECK(at<count);
  memmove(code+at+1,code+at,(count-at)*4);code[at]=0x3a060612;count++;
 }
 if(culled)code[5]|=0x80000000u;
 /* A real passthrough NGG shader exports VGPR0 without constructing a
  * primitive. A constant packed triangle hides an incorrect launch ABI. */
 if(passthrough)code[7]=0;
 sh_program*p=calloc(1,sizeof(*p));CHECK(p && sh_analyze(code,count,0x100000,p));
 sh_stage_info info={.stage=SH_STAGE_MESH,.local_size={64,1,1},.lds_dwords=128,
                     .mesh_input_vertices=3,.mesh_max_vertices=3,.mesh_max_primitives=1};
 info.mesh_primitive_vertices=passthrough?3:1;
 if(getenv("PS5C_TEST_NGG_BALLOT32"))info.mesh_indexed|=SH_MESH_BALLOT32;
 if(two_waves){info.local_size[0]=128;info.mesh_max_vertices=67;}
 if(two_waves>=3)info.wave_barriers=true;
 if(two_waves>=3){CHECK(_putenv_s("PS5C_NO_STRUCTURED","1")==0);ps5c_env_reset();}
 CHECK(sh_translate(p,&info,NULL,0,out,words));
 if(two_waves>=3){CHECK(_putenv_s("PS5C_NO_STRUCTURED","")==0);ps5c_env_reset();}
 if((two_waves==2 || two_waves==5) && !ballot_stress) {
  CHECK(workgroup_controls(*out,*words)==(two_waves==5?4u:2u));
  free(*out);*out=NULL;info.barrier_prefix=true;info.wave_barriers=true;
  CHECK(sh_translate(p,&info,NULL,0,out,words));
  /* All barriers in prefix: no rendezvous in the suffix (three controls).
   * A later barrier keeps the two rendezvous controls, even with a prefix. */
  unsigned controls=workgroup_controls(*out,*words), expected_controls=two_waves==5?5u:3u;
  if(controls!=expected_controls)fprintf(stderr,"prefix mesh mode %u: %u workgroup controls expected %u\n",two_waves,controls,expected_controls);
  CHECK(controls==expected_controls);
  /* s3 is per wave in NGG, unlike ordinary initial CS SGPRs. A scalar
   * branch on it that skips the LDS barrier must never enable this path. */
  memmove(code+2,code,count*4);count+=2;
  code[0]=0xbf068103;code[1]=0xbf840000u|(uint32_t)(count-3);
  CHECK(sh_analyze(code,count,0x100000,p));
  uint32_t *unsafe=NULL;size_t unsafe_words=0;
  CHECK(sh_translate(p,&info,NULL,0,&unsafe,&unsafe_words));
  CHECK(workgroup_controls(unsafe,unsafe_words)==4);free(unsafe);
 }
 if(two_waves==3 || two_waves==4)CHECK(workgroup_controls(*out,*words)==4);
 free(p);
 const char*dir=getenv("PS5C_TEST_SPIRV");
 if(dir){char path[512];snprintf(path,sizeof(path),"%s/guest_mesh_%u_%u_%u.spv",dir,(unsigned)culled,(unsigned)passthrough,(unsigned)two_waves);
 FILE*f=fopen(path,"wb");CHECK(f && fwrite(*out,4,*words,f)==*words && fclose(f)==0);}
 return 0;
}
static VkShaderModule module(renderer*r,const uint32_t*p,size_t n){
 VkShaderModule m=VK_NULL_HANDLE;VkShaderModuleCreateInfo c={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=n,.pCode=p};
 if(vkCreateShaderModule(r->device,&c,NULL,&m)!=VK_SUCCESS)return VK_NULL_HANDLE;
 return m;
}
static uint32_t float_bits(float v){uint32_t b;memcpy(&b,&v,4);return b;}
/* Observe all register bits as finite byte values. A float render target is
 * allowed to change a NaN encoding, so it cannot localize a payload failure. */
static size_t raw_byte_pixel(uint32_t code[32]) {
 size_t n=0;code[n++]=0xc8120002; /* v_interp_mov_f32 v4,P0,attr0.x */
 for(unsigned k=0;k<4;k++) {
  code[n++]=0xd5480000u|k; /* v_bfe_u32 vk,v4,k*8,8 */
  code[n++]=260u|((128u+8u*k)<<9)|(136u<<18);
  code[n++]=0x7e000c00u|(k<<17)|(256u+k); /* v_cvt_f32_u32 vk,vk */
 }
 code[n++]=0xf800080f;code[n++]=0x03020100;code[n++]=0xbf810000;return n;
}
static void translate_raw_byte_pixel(void) {
 uint32_t code[32],*spv=NULL;size_t words=0,n=raw_byte_pixel(code);
 sh_program *p=calloc(1,sizeof(*p));CHECK(p && sh_analyze(code,(uint32_t)n,0,p));
 CHECK(!strcmp(p->insts[0].name,"v_interp_mov_f32") && p->insts[0].dst==RDNA2_OP_VGPR+4);
 for(unsigned k=0;k<4;k++) {
  const rdna2_inst *extract=&p->insts[1+2*k],*convert=&p->insts[2+2*k];
  CHECK(!strcmp(extract->name,"v_bfe_u32") && extract->dst==RDNA2_OP_VGPR+k &&
        extract->src[0]==RDNA2_OP_VGPR+4 && extract->src[1]==128+8*k && extract->src[2]==136);
  CHECK(!strcmp(convert->name,"v_cvt_f32_u32") && convert->dst==RDNA2_OP_VGPR+k && convert->src[0]==RDNA2_OP_VGPR+k);
 }
 sh_stage_info info={.stage=SH_STAGE_PS,.ps_vertex_fetch=true};
 CHECK(sh_translate(p,&info,NULL,0,&spv,&words));
 const char *dir=getenv("PS5C_TEST_SPIRV");
 if(dir) {
  char path[512];snprintf(path,sizeof(path),"%s/mesh_raw_byte_observer.spv",dir);
  FILE *f=fopen(path,"wb");CHECK(f && fwrite(spv,4,words,f)==words && fclose(f)==0);
 }
 free(spv);free(p);
}
static void translate_raw_compute(uint32_t payload,uint32_t **spv,size_t *words) {
 uint32_t code[64];size_t n=PS5C_ARRAY_COUNT(guest_code)-3;
 memcpy(code,guest_code,n*4);
 const uint32_t tail[]={0x7e1402ff,payload,0xf800020f,0x0a0a0a0a,
                       0xf80008cf,0x03020100,0xbf810000};
 memcpy(code+n,tail,sizeof(tail));n+=PS5C_ARRAY_COUNT(tail);
 sh_program *p=calloc(1,sizeof(*p));CHECK(p && sh_analyze(code,(uint32_t)n,0,p) && p->param_mask==1);
 sh_stage_info info={.stage=SH_STAGE_MESH,.local_size={64,1,1},.lds_dwords=128,
                     .mesh_input_vertices=3,.mesh_max_vertices=3,.mesh_max_primitives=1,
                     .mesh_compute=1,.mesh_output_binding=1};
 CHECK(sh_mesh_output_words(p,&info)==33 && sh_translate(p,&info,NULL,0,spv,words));
 free(p);
}
static void raw_compute_test(renderer *r) {
 /* Read the exact producer record before vertex replay or rasterization. */
 vk_buffer uniform,output;
 CHECK(vk_buffer_create(r,SH_U_DATA*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true,&uniform));
 CHECK(vk_buffer_create(r,33*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true,&output));
 memset(uniform.mapped,0,SH_U_DATA*4);uint32_t *u=uniform.mapped;u[SH_U_MESH]=3;
 const float port[8]={4,4,4,4,0.25f,0.25f,1,0};memcpy(u+SH_U_VPORT,port,sizeof(port));
 VkDescriptorSetLayoutBinding bindings[2]={
  {.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
  {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
 VkDescriptorSetLayoutCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};
 VkDescriptorSetLayout dsl;CHECK(vkCreateDescriptorSetLayout(r->device,&dc,NULL,&dsl)==VK_SUCCESS);
 VkPipelineLayoutCreateInfo lc={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&dsl};
 VkPipelineLayout layout;CHECK(vkCreatePipelineLayout(r->device,&lc,NULL,&layout)==VK_SUCCESS);
 VkDescriptorPoolSize pool_size={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
 VkDescriptorPoolCreateInfo pc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&pool_size};
 VkDescriptorPool pool;CHECK(vkCreateDescriptorPool(r->device,&pc,NULL,&pool)==VK_SUCCESS);
 VkDescriptorSetAllocateInfo ac={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&dsl};
 VkDescriptorSet set;CHECK(vkAllocateDescriptorSets(r->device,&ac,&set)==VK_SUCCESS);
 VkDescriptorBufferInfo buffers[2]={{uniform.buffer,0,SH_U_DATA*4},{output.buffer,0,33*4}};
 VkWriteDescriptorSet writes[2];memset(writes,0,sizeof(writes));
 for(unsigned k=0;k<2;k++)writes[k]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,
  .dstBinding=k,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=buffers+k};
 vkUpdateDescriptorSets(r->device,2,writes,0,NULL);
 for(unsigned k=0;k<PS5C_ARRAY_COUNT(raw_payloads);k++) {
  uint32_t *spv=NULL;size_t words=0;translate_raw_compute(raw_payloads[k],&spv,&words);
  VkShaderModule shader=module(r,spv,words*4);free(spv);CHECK(shader);
  VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=layout,
   .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"}};
  VkPipeline pipeline;CHECK(vkCreateComputePipelines(r->device,VK_NULL_HANDLE,1,&ci,NULL,&pipeline)==VK_SUCCESS);
  memset(output.mapped,0xcd,33*4);VkCommandBuffer cmd=vk_begin(r);
  vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
  vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,NULL);vkCmdDispatch(cmd,1,1,1);
  VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
  renderer_wait_idle(r);const uint32_t *v=output.mapped;
  CHECK(v[0]==3 && v[4]==3 && v[5]==1 && v[32]==0x00200400);
  for(unsigned vertex=0;vertex<3;vertex++)for(unsigned c=0;c<4;c++) {
   uint32_t got=v[8+vertex*8+4+c];
   if(got!=raw_payloads[k])fprintf(stderr,"raw compute payload %u vertex %u component %u: %08x expected %08x\n",k,vertex,c,got,raw_payloads[k]);
   CHECK(got==raw_payloads[k]);
  }
  vkDestroyPipeline(r->device,pipeline,NULL);vkDestroyShaderModule(r->device,shader,NULL);
 }
 vkDestroyDescriptorPool(r->device,pool,NULL);vkDestroyPipelineLayout(r->device,layout,NULL);
 vkDestroyDescriptorSetLayout(r->device,dsl,NULL);vk_buffer_destroy(r,&output);vk_buffer_destroy(r,&uniform);
 puts("raw compute output: 48 exact parameter words before replay/rasterization; NaN sign/payload and subnormals preserved");
}
static void cache_pixels(renderer *r,vk_buffer *out,guest_addr target,float expected) {
 vk_image *image=vk_image_find(r,target,8,8,VK_FORMAT_R32G32B32A32_SFLOAT);CHECK(image);
 VkCommandBuffer command=vk_begin(r);
 vk_barrier(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
 VkBufferImageCopy region={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={8,8,1}};
 vkCmdCopyImageToBuffer(command,image->image,image->layout,out->buffer,1,&region);
 vk_flush(r);CHECK(vkWaitForFences(r->device,1,&r->work_fence,VK_TRUE,UINT64_MAX)==VK_SUCCESS);vk_begin(r);
 float *pixels=out->mapped;for(unsigned i=0;i<64;i++)
  CHECK(pixels[4*i]==expected && pixels[4*i+1]==0.5f && pixels[4*i+2]==0.75f && pixels[4*i+3]==1);
}
static void cache_irq(void *user,int32_t event,uint32_t context) {
 CHECK(event==3 && context==91);(*(uint32_t *)user)++;
}
static int renderer_mesh_test(renderer*r,gmem*m,guest_addr a,vk_buffer*out) {
 CHECK(_putenv_s("PS5C_NGG_MESH_WAVES","2")==0); ps5c_env_reset();
 const uint32_t pixel[]={0xc8020002,0x7e0202ff,0x3f000000, /* MOV P0: constant parameter */
  0x7e0402ff,0x3f400000,0x7e0602f2,0xf800080f,0x03020100,0xbf810000};
 /* Scalar loads depend on the same GS_TG_INFO that the GPU receives. A
  * legacy CPU count of 256 would preload element256 (zero), while mesh's
  * three input vertices require element3 (0.25). Exercise both stages. */
 uint32_t mesh_code[64],n=PS5C_ARRAY_COUNT(guest_code)-3;
 memcpy(mesh_code,guest_code,n*4);
 /* GS receives its separate table pointer in s0:s1. Native passthrough
  * (one primitive in this fixture) uses the ordinary user pointer instead.
  * Point GS has three primitives: never overwrite its hardware pointer. */
 const uint32_t scalar[]={0x9384ff02,0x0009000c,0x8f048204,
                         0x9385ff03,0x0004001c,0xbf068105,0xbf840001,0xbe800408,
                         /* Table element depends on both vertex count and the hardware wave
                          * count. GS expects two waves because GE reserves80
                          * outputs, even though MAX_VERT_OUT is only one. */
                         0x9386ff03,0x0004001c,0x8f068206,0x80040604,
                         0xf4000300,0x08000000,
                         0x7e14020c,0xf800020f,0x030a0a0a,0xf80008cf,0x03020100,0xbf810000};
 memcpy(mesh_code+n,scalar,sizeof(scalar));n+=PS5C_ARRAY_COUNT(scalar);
 sh_program*p=calloc(1,sizeof(*p));CHECK(p && sh_analyze(mesh_code,n,a+0x400,p));
 char hash[32];snprintf(hash,sizeof(hash),"%016llx",(unsigned long long)p->hash);free(p);
 CHECK(_putenv_s("PS5C_NGG_MESH_SHADER",hash)==0); ps5c_env_reset();
 CHECK(_putenv_s("PS5C_NGG_MESH_TRI_SHADER","all")==0); ps5c_env_reset();
 char probe[40];snprintf(probe,sizeof(probe),"%s:0:0:s3",hash);
 CHECK(_putenv_s("PS5C_VS_PROBE",probe)==0); ps5c_env_reset();
 CHECK(gmem_write(m,a+0x400,mesh_code,n*4) && gmem_write(m,a+0x500,pixel,sizeof(pixel)));
 uint32_t input[257]={0};input[4]=float_bits(0.25f);
 CHECK(gmem_write(m,a+0x1000,input,sizeof(input)));
 input[4]=0;input[5]=float_bits(0.25f);input[7]=float_bits(0.375f);
 CHECK(gmem_write(m,a+0x1800,input,sizeof(input)));
 gpu g;CHECK(gpu_init(&g,m,NULL)==PS5C_OK);
 g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)((a+0x400)>>8);g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)((a+0x400)>>40);
 g.regs[mmSPI_SHADER_PGM_LO_PS]=(uint32_t)((a+0x500)>>8);g.regs[mmSPI_SHADER_PGM_HI_PS]=(uint32_t)((a+0x500)>>40);
 g.regs[mmSPI_SHADER_PGM_RSRC2_GS]=(1u<<19)|(2u<<1);g.regs[mmVGT_GS_MAX_VERT_OUT]=1;
 g.regs[mmSPI_SHADER_USER_DATA_GS_0]=(uint32_t)(a+0x1000);
 g.regs[mmSPI_SHADER_USER_DATA_GS_0+1]=(uint32_t)((a+0x1000)>>32);
 g.regs[mmSPI_SHADER_USER_DATA_ADDR_LO_GS]=(uint32_t)(a+0x1800);
 g.regs[mmSPI_SHADER_USER_DATA_ADDR_HI_GS]=(uint32_t)((a+0x1800)>>32);
 g.regs[mmGE_MAX_OUTPUT_PER_SUBGROUP]=80;
 g.regs[mmGE_CNTL]=3u|(3u<<9);g.regs[mmVGT_GS_OUT_PRIM_TYPE]=2;
 g.regs[mmCB_COLOR0_BASE]=(uint32_t)((a+0x2000)>>8);
 g.regs[mmCB_COLOR0_INFO]=(14u<<2)|(7u<<8);g.regs[mmCB_COLOR0_ATTRIB2]=(7u<<14)|7u;
 g.regs[mmCB_COLOR_CONTROL]=0x10;g.regs[mmCB_TARGET_MASK]=15;
 g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_POINTLIST;g.regs[mmPA_CL_VTE_CNTL]=15;
 g.regs[mmPA_CL_VPORT_XSCALE]=g.regs[mmPA_CL_VPORT_XOFFSET]=float_bits(4);
 g.regs[mmPA_CL_VPORT_YSCALE]=g.regs[mmPA_CL_VPORT_YOFFSET]=float_bits(4);
 const uint16_t ix16[]={2,0,1};const uint32_t ix32[]={0xffffffffu,0,0xfffffffeu};
 CHECK(gmem_write(m,a+0x800,ix16,sizeof(ix16)) && gmem_write(m,a+0x900,ix32,sizeof(ix32)));
 const char *scalar_case=getenv("PS5C_TEST_LARGE_SCALAR");
 bool vector_case=scalar_case && !strncmp(scalar_case,"vector-",7);
 const char *sample_case=vector_case?scalar_case+7:scalar_case;
 uint64_t import_edge=getenv("PS5C_TEST_IMPORT_EDGE")?0xc000:0;
 uint32_t large_bytes=scalar_case?0xAB000000u:64u<<20;
 uint32_t large_offset=scalar_case?0xA0000000u:48u<<20;
 guest_addr cache_source=0;
 if(sample_case && !strcmp(sample_case,"end"))large_offset=large_bytes-4;
 if(sample_case && !strcmp(sample_case,"oob"))large_offset=large_bytes;
 const char *single_pass=getenv("PS5C_TEST_MESH_PASS");
 unsigned first=getenv("PS5C_TEST_RAW_TARGET")?23:getenv("PS5C_TEST_LARGE_READS")?21:getenv("PS5C_TEST_FAST_LAUNCH")?19:0;
 unsigned last=getenv("PS5C_TEST_RAW_TARGET")?27:getenv("PS5C_TEST_LARGE_READS")?23:getenv("PS5C_TEST_FAST_LAUNCH")?21:19;
 if(single_pass) {
  char *end=NULL;unsigned long selected=strtoul(single_pass,&end,10);
  CHECK(*single_pass && end && !*end && ((selected>=9 && selected<=17) || (selected>=23 && selected<=26)));
  first=(unsigned)selected;last=first+1;
 }
 bool raw_bytes=getenv("PS5C_TEST_RAW_BYTES")!=NULL;
 CHECK(!raw_bytes || (single_pass && first>=9 && first<17));
 for(unsigned pass=first;pass<last;pass++) {
 if(getenv("PS5C_TEST_LOCAL_ARENA")) {
  FILE*f=fopen("local-arena-mode.txt","w");CHECK(f);
  fprintf(f,"%u\n",pass&1u);CHECK(fclose(f)==0);
 }
 bool indexed=pass!=0 && pass!=5 && pass<6,wide=pass==2 || pass==4,triangle=(pass>=3 && pass<=5) || pass>=13;
 guest_addr target=a+0x2000+(uint64_t)pass*0x1000;
 g.regs[mmCB_COLOR0_BASE]=(uint32_t)(target>>8);
 g.regs[mmVGT_PRIMITIVE_TYPE]=pass==5?GPU_PRIM_TRISTRIP:triangle?GPU_PRIM_TRILIST:GPU_PRIM_POINTLIST;
 g.regs[mmVGT_GS_MAX_VERT_OUT]=triangle?0:1;
 g.regs[mmVGT_SHADER_STAGES_EN]=triangle?0x02002000u:0x2030u;
 g.regs[mmGE_INDX_OFFSET]=wide?2:0;
 if(pass==5) {
  /* The two strip triangles must each cover half the target. Face
   * culling makes a missing odd-triangle reversal fail half the pixels. */
  g.regs[mmPA_CL_VPORT_XSCALE]=g.regs[mmPA_CL_VPORT_XOFFSET]=float_bits(2);
  g.regs[mmPA_CL_VPORT_YSCALE]=g.regs[mmPA_CL_VPORT_YOFFSET]=float_bits(2);
  g.regs[mmPA_SU_SC_MODE_CNTL]=1;
  snprintf(probe,sizeof(probe),"%s:1",hash);CHECK(_putenv_s("PS5C_VS_PROBE",probe)==0); ps5c_env_reset();
 }
 if(pass>=6) {
  CHECK(_putenv_s("PS5C_NGG_COMPUTE",hash)==0);g.regs[mmGE_MAX_OUTPUT_PER_SUBGROUP]=216; ps5c_env_reset();
  g.regs[mmPA_SU_SC_MODE_CNTL]=0;
  g.regs[mmPA_CL_VPORT_XSCALE]=g.regs[mmPA_CL_VPORT_XOFFSET]=float_bits(4);
  g.regs[mmPA_CL_VPORT_YSCALE]=g.regs[mmPA_CL_VPORT_YOFFSET]=float_bits(4);
  snprintf(probe,sizeof(probe),"%s:0:0:s3",hash);CHECK(_putenv_s("PS5C_VS_PROBE",probe)==0); ps5c_env_reset();
 }
 if(pass==18) {
  /* Same input partition as the original compute path, but only80 output
   * slots: the optional compact path must publish two waves in both the
   * CPU resource evaluation and the GPU prologue. The vertex192 cases
   * above still require all four waves. */
  CHECK(gmem_write(m,a+0x400,mesh_code,n*4) && gmem_write(m,a+0x500,pixel,sizeof(pixel)));
  g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_POINTLIST;g.regs[mmVGT_SHADER_STAGES_EN]=0x2030u;
  g.regs[mmVGT_GS_MAX_VERT_OUT]=1;g.regs[mmGE_MAX_OUTPUT_PER_SUBGROUP]=80;
  g.regs[mmSPI_SHADER_PGM_RSRC2_GS]=(1u<<19)|(2u<<1);
 }
 if(pass>=19) {
  /* A fast-launch point expands to a triangle whose third vertex is in
   * the second native wave32. Wave index and count must be32-based, not
   * a single64 mask or four speculative waves. No commercial shader. */
  uint32_t fast[96]={0x7e1a0305,0x9386ff03,0x00040018,0x8f068506,0xd7650005,0x00000cc1};
  uint32_t at=6;
  bool object_prefix=pass==19 && getenv("PS5C_TEST_MESH_OBJECT")!=NULL;
  if(object_prefix) {
   memmove(fast+1,fast,at*4);at++;
   fast[0]=0x7e200305; /* save base vertex before MBCNT reuses v5 */
   fast[1]=0x7e1a0306; /* save Fast Launch1 instance ID, not normal GS v8 */
   /* Synthetic object table lookup: one lane publishes a b64 pair, then
    * the whole group reads it and uses the first lane's object index.
    * VCC_LO is a plain scalar temporary after the broadcast, not a mask. */
   const uint32_t lookup[]={
    0xbefe03c1,0x93eaff03,0x00040018,0x8f6a856a,0xd7650016,0x0000d4c1,
    0x7d8a2c80,0xbeea076a,0xbefe036a,0xbf880005,
    0x7e080306,0x7e0a0280,0x7e000280,0xd9340000,0x00000400,
    0xbefe03c1,0xbf8a0000,0x7e000280,0xd9d80000,0x14000000,
    0xbf8cc07f,0x7ed40514,0x98ea886a,0xf4201104,0xd4000000,0xbf8cc07f};
   memcpy(fast+at,lookup,sizeof(lookup));at+=PS5C_ARRAY_COUNT(lookup);
   uint32_t table[8]={0,0,float_bits(0.25f),0,0,0,float_bits(0.5f),0};
   CHECK(gmem_write(m,a+0x1000,table,sizeof(table)));
   g.regs[mmSPI_SHADER_USER_DATA_GS_0]=(uint32_t)(a+0x1000);
   g.regs[mmSPI_SHADER_USER_DATA_GS_0+1]=(uint32_t)((a+0x1000)>>32)|(16u<<16);
   g.regs[mmSPI_SHADER_USER_DATA_GS_0+2]=2;
   g.regs[mmSPI_SHADER_USER_DATA_GS_0+3]=0x00005204;
  }
  if(pass==20) {
   /* Only thread0 has the point ID. Broadcast it exactly as a real
    * fast-launch mesh prologue does; the other lanes' input is unused. */
   const uint32_t share[]={0xbf068006,0xbf840006,0xbefe0381,0x7e000280,
                           0xd8340100,0x00000d00,0xbf8cc07f,0xbefe03c1,
                           0xbf8a0000,0x7e000280,0xd8d80100,0x0d000000,0xbf8cc07f};
   memcpy(fast+at,share,sizeof(share));at+=PS5C_ARRAY_COUNT(share);
  }
  for(uint32_t i=0;i<PS5C_ARRAY_COUNT(guest_code)-1;i++) {
   uint32_t word=guest_code[i];
   if(i==1)word=0x1021; /* 33 vertices, one primitive */
   if(word==0x00200400)word=0x02000400; /* 0,1,32 */
   if(word==0xbefe0487)word=0xbefe04c1;
   if(word==0x2c020a81)word=0x2c020a85;
   if(pass==20 || object_prefix) {
    /* Disjoint half-screen triangles: native mesh workgroups have no
     * raster order, so overlapping groups cannot test independent IDs. */
    if(word==0x420000f6)word=0x420000f2;
    if(word==0x420202f6)word=0x420202f4;
    if(word==0xf80008cf){fast[at++]=0x7e1c0d0d;fast[at++]=0x0600010e;}
   }
   fast[at++]=word;
  }
  const uint32_t color[]={0x9384ff03,0x0004001c,0x8f048204,0xf4000300,0x08000000,
                          0x7e14020c,0xf800020f,0x030a0a0a,0xbf810000};
  memcpy(fast+at,color,sizeof(color));at+=PS5C_ARRAY_COUNT(color);
  if(object_prefix) {
   fast[at-4]=0x7e140244; /* v10=s68: the actual table result */
   /* Every lane's base vertex is0, including the triangle's vertex32.
    * A per-lane vertex index would interpolate the wrong color. */
   memmove(fast+at-1,fast+at-3,3*4);
   fast[at-3]=0x7d842080;fast[at-2]=0x02141480;at+=2;
  }
  if(pass==21) {
   /* A legitimate64MiB V# reads a color48MiB from its base. Clipping the
    * binding to MAX_BUFFER_COPY silently turns that in-bounds load into0. */
   guest_addr large=0;CHECK(gmem_map(m,a+0x10000000+import_edge,scalar_case?3ull<<30:128u<<20,GPROT_RW,GMAP_FIXED,"large mesh readonly fixture",&large)==PS5C_OK);
   if(import_edge)large+=64; /* no mapped64KiB prefix: only Vulkan alignment is required */
   cache_source=large;
   if(getenv("PS5C_TEST_LARGE_SNAPSHOT")) {
    CHECK(!scalar_case && !import_edge);
    CHECK(vk_image_get(r,large,1,1,VK_FORMAT_R32_UINT));
    large+=0x10000; /* an earlier image in the same VMA excludes host import */
    if(!getenv("PS5C_TEST_SNAPSHOT_OFF")) {
     char address[512];snprintf(address,sizeof(address),"%llx",(unsigned long long)large);
     const char *file=getenv("PS5C_TEST_SNAPSHOT_FILE");
     if(file) {
      FILE *f=fopen(file,"w");CHECK(f);
      CHECK(fprintf(f,"%s\n",address)>0 && fclose(f)==0);
      CHECK(strlen(file)+2<sizeof(address));snprintf(address,sizeof(address),"@%s",file);
     }
     CHECK(_putenv_s("PS5C_LARGE_READ_SNAPSHOT",address)==0); ps5c_env_reset();
    }
   }
   CHECK(vk_image_get(r,large+large_bytes+(16u<<20),1,1,VK_FORMAT_R32_UINT)); /* beyond the binding */
   uint32_t bits=float_bits(0.25f);CHECK(gmem_write(m,large+large_offset,&bits,4));
   const uint32_t fetch[]={0x7e0002ff,large_offset,0xe0301000,0x80040a00,0xbf8c3f70,
                          0xf800020f,0x030a0a0a,0xbf810000};
   at-=3;memcpy(fast+at,fetch,sizeof(fetch));at+=PS5C_ARRAY_COUNT(fetch);
   if(scalar_case && !vector_case) {
    /* DWORD scalar loads, including offsets beyond2GiB. Do not replace
     * the driver's incorrect length with the guest's declared capacity. */
    const uint32_t scalar_fetch[]={0xbe8403ff,large_offset,0xf4200308,0x08000000,0xbf8cc07f,0x7e14020c,
                                  0xf800020f,0x030a0a0a,0xbf810000};
    at-=PS5C_ARRAY_COUNT(fetch);memcpy(fast+at,scalar_fetch,sizeof(scalar_fetch));at+=PS5C_ARRAY_COUNT(scalar_fetch);
   }
   g.regs[mmSPI_SHADER_USER_DATA_GS_0+8]=(uint32_t)large;
   g.regs[mmSPI_SHADER_USER_DATA_GS_0+9]=(uint32_t)(large>>32);
   g.regs[mmSPI_SHADER_USER_DATA_GS_0+10]=large_bytes;
   g.regs[mmSPI_SHADER_USER_DATA_GS_0+11]=0x00016204;
   if(vector_case || (scalar_case && !strcmp(scalar_case,"structured"))) {
    g.regs[mmSPI_SHADER_USER_DATA_GS_0+9]|=1u<<16;
    g.regs[mmSPI_SHADER_USER_DATA_GS_0+11]=0x30005004;
   }
  }
  if(pass==20) {
   /* Both independent point IDs affect color and horizontal placement. */
   const uint32_t group_color[]={0x7e1c02f2,0x7d841a81,0x02141cf0,
                                0xf800020f,0x030a0a0a,0xbf810000};
   at-=3;memcpy(fast+at,group_color,sizeof(group_color));at+=PS5C_ARRAY_COUNT(group_color);
  }
  guest_addr fast_addr=a+(object_prefix?0x3000:0x400);
  CHECK(at<=PS5C_ARRAY_COUNT(fast));
  CHECK(gmem_write(m,fast_addr,fast,at*4) && gmem_write(m,a+0x500,pixel,sizeof(pixel)));
  g.regs[mmSPI_SHADER_PGM_LO_ES]=(uint32_t)(fast_addr>>8);
  g.regs[mmSPI_SHADER_PGM_HI_ES]=(uint32_t)(fast_addr>>40);
  if(pass==22) {
   /* The material PS needs the same full resource range as its geometry. */
   guest_addr large=a+0x10000000+import_edge+(import_edge?64:0);
   if(getenv("PS5C_TEST_LARGE_SNAPSHOT"))large+=0x10000;
   const uint32_t fetch_pixel[]={0x7e0002ff,large_offset,0xe0301000,0x80000000,0xbf8c3f70,
       0x7e0202ff,0x3f000000,0x7e0402ff,0x3f400000,0x7e0602f2,0xf800080f,0x03020100,0xbf810000};
   CHECK(gmem_write(m,a+0x500,fetch_pixel,sizeof(fetch_pixel)));
   if(scalar_case && !vector_case) {
    const uint32_t scalar_pixel[]={0xbe8403ff,large_offset,0xf4200300,0x08000000,0xbf8cc07f,0x7e00020c,
       0x7e0202ff,0x3f000000,0x7e0402ff,0x3f400000,0x7e0602f2,0xf800080f,0x03020100,0xbf810000};
    CHECK(gmem_write(m,a+0x500,scalar_pixel,sizeof(scalar_pixel)));
   }
   g.regs[mmSPI_SHADER_PGM_RSRC2_PS]=4u<<1;
   g.regs[mmSPI_SHADER_USER_DATA_PS_0]=(uint32_t)large;
   g.regs[mmSPI_SHADER_USER_DATA_PS_0+1]=(uint32_t)(large>>32);
   g.regs[mmSPI_SHADER_USER_DATA_PS_0+2]=large_bytes;
   g.regs[mmSPI_SHADER_USER_DATA_PS_0+3]=0x00016204;
   if(vector_case || (scalar_case && !strcmp(scalar_case,"structured"))) {
    g.regs[mmSPI_SHADER_USER_DATA_PS_0+1]|=1u<<16;
    g.regs[mmSPI_SHADER_USER_DATA_PS_0+3]=0x30005004;
   }
  }
  uint32_t color_bits=float_bits(0.25f);CHECK(gmem_write(m,a+0x1808,&color_bits,4));
  CHECK(_putenv_s("PS5C_NGG_FAST_LAUNCH","1")==0 && _putenv_s("PS5C_VS_PROBE","")==0); ps5c_env_reset();
  g.regs[mmVGT_PRIMITIVE_TYPE]=GPU_PRIM_POINTLIST;g.regs[mmVGT_SHADER_STAGES_EN]=0x482030;
  g.regs[mmGE_CNTL]=0x201;g.regs[mmGE_NGG_SUBGRP_CNTL]=64;
  g.regs[mmVGT_GS_MAX_VERT_OUT]=64;g.regs[mmGE_MAX_OUTPUT_PER_SUBGROUP]=64;
  g.regs[mmSPI_SHADER_PGM_RSRC2_GS]=(1u<<19)|(2u<<1);
  if(object_prefix)g.regs[mmSPI_SHADER_PGM_RSRC2_GS]=(1u<<19)|(4u<<1);
  if(pass==21)g.regs[mmSPI_SHADER_PGM_RSRC2_GS]=(1u<<19)|(12u<<1);
  if(pass>=23) {
   uint32_t raw_pixel[]={0x7e0002ff,raw_payloads[pass-23],0x7e0202ff,0x3f000000,
                        0x7e0402ff,0x3f400000,0x7e0602f2,0xf800080f,0x03020100,0xbf810000};
   CHECK(gmem_write(m,a+0x500,raw_pixel,sizeof(raw_pixel)));
   g.regs[mmSPI_SHADER_COL_FORMAT]=9; /* 32_ABGR register bits; no compression */
  }
 }
 if(pass>=7 && pass<13) {
  /* A triangle whose third vertex is invocation192. A 128-lane shortcut
   * cannot draw it. Repeat two groups and two instances via indirect draws. */
  uint32_t wide_code[64],at=0;
  for(uint32_t i=0;i<n;i++) {
   if(mesh_code[i]==0x36000a81)wide_code[at++]=0x2c0a0082; /* v5 = packed GS local index >> 2 */
   uint32_t word=mesh_code[i];
   if(pass>=9 && word==0x7e14020c) {wide_code[at++]=0x7e1402ff;wide_code[at++]=raw_payloads[pass-9];continue;}
   if(i==1)word=0x10c3; /* 195 vertices, one primitive */
   if(word==0x00200400u)word=0x0c000400u; /* indices0,1,192 */
   if(word==0xbefe0487)word=0xbefe04c1; /* all lanes export positions */
   if(word==0x2c020a81)word=0x2c020a86; /* Y from the upper wave bits */
   if(word==0xf80008cf)wide_code[at++]=0xbf8a0000; /* four guest waves rendezvous before export */
   wide_code[at++]=word;
  }
  CHECK(at<64 && gmem_write(m,a+0x400,wide_code,at*4));
  CHECK(_putenv_s("PS5C_NGG_WAVE_BARRIERS","1")==0); ps5c_env_reset();
  CHECK(_putenv_s("PS5C_NGG_COMPUTE","1")==0);CHECK(_putenv_s("PS5C_VS_PROBE","")==0); ps5c_env_reset();
  g.regs[mmVGT_GS_MAX_VERT_OUT]=72;g.regs[mmSPI_SHADER_PGM_RSRC2_GS]=(8u<<19)|(2u<<1);
 }
 if(pass>=13 && pass<17) {
  /* Packed attributes must retain NaN/subnormal payloads through the native
   * mesh stage too; ordinary VS tests alone do not exercise this transport. */
  uint32_t raw_code[64],at=0;
  for(uint32_t i=0;i<n;i++) {
   if(mesh_code[i]==0x7e14020c){raw_code[at++]=0x7e1402ff;raw_code[at++]=raw_payloads[pass-13];}
   else raw_code[at++]=mesh_code[i];
  }
  CHECK(at<64 && gmem_write(m,a+0x400,raw_code,at*4));
  g.regs[mmGE_MAX_OUTPUT_PER_SUBGROUP]=80;g.regs[mmSPI_SHADER_PGM_RSRC2_GS]=(1u<<19)|(2u<<1);
  g.regs[mmPA_SU_SC_MODE_CNTL]=0;
 }
 if(pass==17) {
  /* Actual Astro materials read one parameter both smoothly and through
   * passthrough MOVs. Every vertex must keep its own packed bits, including
   * on a Flat alias; equal values at all vertices cannot expose flattening. */
  uint32_t mixed_code[80],at=0;
  for(uint32_t i=0;i<n;i++) {
   if(mesh_code[i]==0x7e14020c) {
    mixed_code[at++]=0x7e1402ff;mixed_code[at++]=0x7fa12345;
    mixed_code[at++]=(0x25u<<25)|(10u<<17)|(5u<<9)|(256u+10u);
   } else mixed_code[at++]=mesh_code[i];
  }
  const uint32_t mixed_pixel[]={0xc8020002,0xc8060400,0xc80a0401,0x7e0602f2,0xf800080f,0x03020100,0xbf810000};
  CHECK(at<80 && gmem_write(m,a+0x400,mixed_code,at*4));
  CHECK(gmem_write(m,a+0x500,mixed_pixel,sizeof(mixed_pixel)));
  g.regs[mmSPI_PS_INPUT_CNTL_0]=0;g.regs[mmSPI_PS_INPUT_CNTL_0+1]=0x420;
  g.regs[mmGE_MAX_OUTPUT_PER_SUBGROUP]=80;g.regs[mmSPI_SHADER_PGM_RSRC2_GS]=(1u<<19)|(2u<<1);
  g.regs[mmPA_SU_SC_MODE_CNTL]=0;
 }
 if(pass==8)g.regs[mmPA_SU_SC_MODE_CNTL]=4; /* same compute module/layout, different raster state */
 if(raw_bytes) {
  uint32_t observer[32];size_t observer_words=raw_byte_pixel(observer);
  CHECK(gmem_write(m,a+0x500,observer,observer_words*4));
 }
 /* Alternate the packet's pixel-wave contract while reusing shader addresses.
  * The optional wave-variant cache must preserve every actual output below. */
 g.regs[0xa1b6]=(pass&1)?0x8000:0; /* SPI_PS_IN_CONTROL.PS_W32_EN */
 bool object_test=pass==19 && getenv("PS5C_TEST_MESH_OBJECT")!=NULL;
 gpu_draw draw={.count=pass>=21?1:pass>=19?pass-18:pass>=7 && pass<13?6:pass==5?4:3,.instances=(pass>=7 && pass<13) || object_test?2:1,.indexed=indexed,.index_size=wide?4:2,.index_addr=a+(wide?0x900:0x800)};
 uint64_t before,after;CHECK(renderer_sample_counter(r,&before));
 renderer_submit_begin(r);renderer_draw(r,&g,&draw);CHECK(renderer_sample_counter(r,&after));CHECK(after-before==(pass>=7 && pass<13?256:pass==20 || object_test?32:64));
 if(pass==21 && getenv("PS5C_TEST_LARGE_CACHE")) {
  CHECK(!scalar_case && !import_edge && !getenv("PS5C_TEST_LARGE_SNAPSHOT"));
  const char *setting=getenv("PS5C_LARGE_IMPORT_CACHE");
  bool enabled=setting && !strcmp(setting,"1");
  const char *subset=getenv("PS5C_LARGE_IMPORT_CONTAINED");
  bool contained=enabled && subset && !strcmp(subset,"1");
  if(enabled && subset && subset[0]=='@') {
   FILE*f=fopen(subset+1,"r");int value=0;
   contained=f && fscanf(f,"%d",&value)==1 && value==1;if(f)fclose(f);
  }
  uint32_t draw_limit=8;const char *limit=getenv("PS5C_LARGE_IMPORT_DRAWS");
  if(limit && limit[0]=='@') {
   FILE*f=fopen(limit+1,"r");unsigned value=0;
   if(f && fscanf(f,"%u",&value)==1 && (value==8 || value==16 || value==32))draw_limit=value;
   if(f)fclose(f);
  } else if(limit && !strcmp(limit,"16"))draw_limit=16;
  else if(limit && !strcmp(limit,"32"))draw_limit=32;
  uint64_t builds,reuses;
  /* Real CPU writes between completed draws must remain visible through the
   * retained import. Read the actual image without dropping the cache. */
  for(unsigned iteration=0;iteration<2;iteration++) {
   uint32_t bits=float_bits(iteration?0.25f:0.5f);
   CHECK(gmem_write(m,cache_source+large_offset,&bits,4));renderer_draw(r,&g,&draw);
   CHECK(renderer_sample_counter(r,&after) && after-before==64u*(iteration+2));
   cache_pixels(r,out,target,iteration?0.25f:0.5f);
  }
  vk_large_import_cache_counts(r,&builds,&reuses);
  CHECK(builds==(enabled?1:0) && reuses==(enabled?2:0));
  CHECK(m->mapping_pins==(enabled?1:0));
  /* A compute writer must destroy retained imports and release their map pin
   * before touching the source. Its real result is consumed by another draw. */
  const uint32_t writer[]={0x7e0002ff,0x3f400000,0xe0700000,0x80000000,0xbf810000};
  CHECK(gmem_write(m,a+0x1000,writer,sizeof(writer)));
  g.regs[mmCOMPUTE_PGM_LO]=(uint32_t)((a+0x1000)>>8);g.regs[mmCOMPUTE_PGM_HI]=(uint32_t)((a+0x1000)>>40);
  g.regs[mmCOMPUTE_PGM_RSRC2]=4u<<1;
  g.regs[mmCOMPUTE_NUM_THREAD_X]=g.regs[mmCOMPUTE_NUM_THREAD_Y]=g.regs[mmCOMPUTE_NUM_THREAD_Z]=1;
  uint32_t *user=g.regs+mmCOMPUTE_USER_DATA_0;guest_addr write_at=cache_source+large_offset;
  user[0]=(uint32_t)write_at;user[1]=(uint32_t)(write_at>>32);user[2]=4;user[3]=0x16204;
  renderer_dispatch(r,&g,1,1,1);renderer_sync_mem(r,write_at,4);CHECK(m->mapping_pins==0);
  uint32_t result=0;CHECK(gmem_read(m,write_at,&result,4) && result==float_bits(0.75f));
  renderer_draw(r,&g,&draw);CHECK(renderer_sample_counter(r,&after) && after-before==256);
  vk_large_import_cache_counts(r,&builds,&reuses);CHECK(builds==(enabled?2:0) && reuses==(enabled?2:0));
  /* The next submission releases the pin. Remapping the same VA must import
   * the new allocation rather than retaining the previous host address. */
  renderer_submit_begin(r);CHECK(m->mapping_pins==0);
  CHECK(gmem_unmap(m,cache_source,128u<<20)==PS5C_OK);
  guest_addr remapped=0;CHECK(gmem_map(m,cache_source,128u<<20,GPROT_RW,GMAP_FIXED,"remapped import fixture",&remapped)==PS5C_OK && remapped==cache_source);
  result=float_bits(0.25f);CHECK(gmem_write(m,write_at,&result,4));renderer_draw(r,&g,&draw);
  CHECK(renderer_sample_counter(r,&after) && after-before==320);
  vk_large_import_cache_counts(r,&builds,&reuses);CHECK(builds==(enabled?3:0) && reuses==(enabled?2:0));
  uint64_t recordings=r->use_seq;
  for(unsigned repeat=0;repeat<draw_limit-1;repeat++)renderer_draw(r,&g,&draw);
  const char *batch=getenv("PS5C_LARGE_IMPORT_BATCH");bool batched=batch && !strcmp(batch,"1") && enabled;
  printf("%u equal draws: %llu new command recordings\n",draw_limit-1,(unsigned long long)(r->use_seq-recordings));
  CHECK(r->use_seq-recordings==(batched?2u:2u*(draw_limit-1)));
  CHECK(m->mapping_pins==0); /* the selected bound really retired the pool */
  renderer_draw(r,&g,&draw);
  vk_large_import_cache_counts(r,&builds,&reuses);CHECK(builds==(enabled?4:0) && reuses==(enabled?draw_limit+1:0));
  g.regs[mmSPI_SHADER_USER_DATA_GS_0+10]=60u<<20;renderer_draw(r,&g,&draw);
  vk_large_import_cache_counts(r,&builds,&reuses);
  CHECK(builds==(enabled?(contained?4:5):0) && reuses==(enabled?draw_limit+1+(contained?1:0):0));
  g.regs[mmSPI_SHADER_USER_DATA_GS_0+10]=large_bytes;
  CHECK(renderer_sample_counter(r,&after) && after-before==384u+64u*draw_limit);
  renderer_wait_idle(r);CHECK(m->mapping_pins==0);
  if(batch && !strcmp(batch,"1")) {
   CHECK(enabled);uint64_t zero=0,label=0;guest_addr label_at=a+0x1f00;
   CHECK(gmem_write(m,label_at,&zero,8));uint32_t interrupts=0;
   g.cb.user=&interrupts;g.cb.interrupt=cache_irq;
   renderer_draw(r,&g,&draw); /* no sample query/fence between draw and EOP */
   gpu_release release={.addr=label_at,.data=0x123456789abcdef0ull,.data_sel=2,.interrupt=true,.queue_event=3,.context_id=91};
   CHECK(renderer_release(r,&g,&release));
   CHECK(gmem_read(m,label_at,&label,8) && label==0 && interrupts==0 && m->mapping_pins==1);
   renderer_cp_write(r);
   CHECK(gmem_read(m,label_at,&label,8) && label==release.data && interrupts==1 && m->mapping_pins==0);
   cache_pixels(r,out,target,0.25f);
   result=float_bits(0.75f);CHECK(gmem_write(m,write_at,&result,4));renderer_draw(r,&g,&draw);
   renderer_suspend_point(r);CHECK(m->mapping_pins==0);cache_pixels(r,out,target,0.75f);
   result=float_bits(0.25f);CHECK(gmem_write(m,write_at,&result,4));renderer_draw(r,&g,&draw);
   renderer_wait_idle(r);CHECK(m->mapping_pins==0);
   CHECK(renderer_sample_counter(r,&after) && after-before==576u+64u*draw_limit);
   g.cb.user=NULL;g.cb.interrupt=NULL;
   vk_large_import_cache_counts(r,&builds,&reuses);CHECK(builds==(contained?7:8) && reuses==draw_limit+1+(contained?1:0));
   puts("large import batch: EOP label/interrupt held until fence; CP_WRITE and suspend complete pixels before unpinning");
  }
  printf("large import cache %s: %llu fresh pools, %llu reused draws; fresh CPU writes, compute invalidation, remapped VA, %u-draw bound and descriptor changes correct\n",
      enabled?"on":"off",(unsigned long long)builds,(unsigned long long)reuses,draw_limit);
  if(contained) {
   renderer_submit_begin(r);CHECK(m->mapping_pins==0);
   renderer_draw(r,&g,&draw);cache_pixels(r,out,target,0.25f);
   vk_large_import_cache_counts(r,&builds,&reuses);
   uint64_t original_builds=builds,original_reuses=reuses;
   guest_addr shifted=cache_source+4096;
   result=float_bits(0.5f);CHECK(gmem_write(m,shifted+large_offset,&result,4));
   g.regs[mmSPI_SHADER_USER_DATA_GS_0+8]=(uint32_t)shifted;
   g.regs[mmSPI_SHADER_USER_DATA_GS_0+9]=(uint32_t)(shifted>>32);
   g.regs[mmSPI_SHADER_USER_DATA_GS_0+10]=60u<<20;
   renderer_draw(r,&g,&draw);cache_pixels(r,out,target,0.5f);
   vk_large_import_cache_counts(r,&builds,&reuses);
   CHECK(builds==original_builds && reuses==original_reuses+1 && m->mapping_pins==1);
   /* This expansion escapes the original import and must rebuild it. */
   g.regs[mmSPI_SHADER_USER_DATA_GS_0+10]=68u<<20;
   renderer_draw(r,&g,&draw);cache_pixels(r,out,target,0.5f);
   vk_large_import_cache_counts(r,&builds,&reuses);
   CHECK(builds==original_builds+1 && reuses==original_reuses+1);
   /* Returning below the retained start must also rebuild, not alias it. */
   g.regs[mmSPI_SHADER_USER_DATA_GS_0+8]=(uint32_t)cache_source;
   g.regs[mmSPI_SHADER_USER_DATA_GS_0+9]=(uint32_t)(cache_source>>32);
   g.regs[mmSPI_SHADER_USER_DATA_GS_0+10]=large_bytes;
   renderer_draw(r,&g,&draw);cache_pixels(r,out,target,0.25f);
   vk_large_import_cache_counts(r,&builds,&reuses);
   CHECK(builds==original_builds+2 && reuses==original_reuses+1);
   renderer_wait_idle(r);CHECK(m->mapping_pins==0);
   puts("contained import reuse: 256 additional RGBA pixels; shifted CPU data visible, expansion and lower base rebuild");
  }
  /* Identical work across lifetime settings: fewer recordings must still
   * produce every sample and the actual pixels, with no pin left behind. */
  renderer_submit_begin(r);uint64_t sample_start,sample_end;
  CHECK(renderer_sample_counter(r,&sample_start));recordings=r->use_seq;
  for(unsigned repeat=0;repeat<64;repeat++)renderer_draw(r,&g,&draw);
  uint64_t recorded=r->use_seq-recordings;
  CHECK(recorded==(batched?128u/draw_limit:128u));
  CHECK(renderer_sample_counter(r,&sample_end) && sample_end-sample_start==4096);
  cache_pixels(r,out,target,0.25f);renderer_wait_idle(r);CHECK(m->mapping_pins==0);
  printf("64 equal draws at limit%u: %llu command recordings,4096 actual samples,64 RGBA pixels and mapping release correct\n",
         draw_limit,(unsigned long long)recorded);
 }
 if(pass==0) {
  /* Disable counting without suppressing the actual draw. */
  g.regs[0xa001]=1;renderer_draw(r,&g,&draw);CHECK(renderer_sample_counter(r,&before) && before==after);g.regs[0xa001]=0;
  /* An empty scissor must produce a genuine zero, not synthetic visibility. */
  g.regs[mmPA_SC_SCREEN_SCISSOR_TL]=g.regs[mmPA_SC_SCREEN_SCISSOR_BR]=8|(8u<<16);
  renderer_draw(r,&g,&draw);CHECK(renderer_sample_counter(r,&before) && before==after);
  g.regs[mmPA_SC_SCREEN_SCISSOR_TL]=g.regs[mmPA_SC_SCREEN_SCISSOR_BR]=0;
 }
 renderer_wait_idle(r);
 vk_image*img=vk_image_find(r,target,8,8,VK_FORMAT_R32G32B32A32_SFLOAT);CHECK(img && img->gpu_written);
 VkCommandBuffer cmd=vk_begin(r);vk_barrier(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
 VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={8,8,1}};
 vkCmdCopyImageToBuffer(cmd,img->image,img->layout,out->buffer,1,&copy);renderer_wait_idle(r);
 float expected=pass>=6?0.375f:0.25f;
 if(pass==19 || pass==21 || pass==22)expected=0.25f;
 if(sample_case && (!strcmp(sample_case,"oob") || !strcmp(sample_case,"clipped")))expected=0;
 if(pass==20)expected=1.0f;
 if(pass==18 && getenv("PS5C_TEST_NGG_COMPACT_GROUPS"))expected=0.25f;
 float*v=out->mapped;for(unsigned i=0;i<64;i++) {
  if(raw_bytes) {
   uint32_t payload=raw_payloads[(pass-9)%4],observed=0;
   for(unsigned k=0;k<4;k++) {
    uint32_t byte=(payload>>(8*k))&255u;
    if(v[4*i+k]!=(float)byte)fprintf(stderr,"raw byte observer pass %u pixel %u component %u: %.9g expected %u\n",pass,i,k,v[4*i+k],byte);
    CHECK(v[4*i+k]==(float)byte);observed|=(uint32_t)v[4*i+k]<<(8*k);
   }
   CHECK(observed==payload);continue;
  }
  if(pass>=23) {
   if(float_bits(v[4*i])!=raw_payloads[pass-23])fprintf(stderr,"raw target pass%u bits=%08x expected=%08x\n",pass,float_bits(v[4*i]),raw_payloads[pass-23]);
   CHECK(float_bits(v[4*i])==raw_payloads[pass-23] && v[4*i+1]==0.5f && v[4*i+2]==0.75f && v[4*i+3]==1);continue;
  }
  if(pass==20 || object_test) {
   bool inside=2*((i%8)%4)+i/8<=6;
   float red=(i%8<4?0.5f:1.0f)*(object_test?0.5f:1.0f);
   if(object_test && (v[4*i]!=(inside?red:0) || v[4*i+3]!=(inside?1:0)))
    fprintf(stderr,"object pixel%u: %.9g %.9g %.9g %.9g expected red%.9g alpha%u\n",i,v[4*i],v[4*i+1],v[4*i+2],v[4*i+3],inside?red:0,inside?1:0);
   CHECK(v[4*i]==(inside?red:0) && v[4*i+1]==(inside?0.5f:0) &&
         v[4*i+2]==(inside?0.75f:0) && v[4*i+3]==(inside?1:0));continue;
  }
  if(pass==17) {
   for(unsigned k=0;k<3;k++) {
    if(float_bits(v[4*i+k])!=0x7fa12345u+k)fprintf(stderr,"mixed native input pixel%u component%u=%08x expected%08x\n",i,k,float_bits(v[4*i+k]),0x7fa12345u+k);
    CHECK(float_bits(v[4*i+k])==0x7fa12345u+k);
   }
   CHECK(v[4*i+3]==1);continue;
  }
  if(pass>=9 && pass<17) {
   if(float_bits(v[4*i])!=raw_payloads[(pass-9)%4] || v[4*i+1]!=0.5f || v[4*i+2]!=0.75f || v[4*i+3]!=1)
    fprintf(stderr,"raw varying pass%u pixel%u: %08x %08x %08x %08x expected %08x 3f000000 3f400000 3f800000\n",
      pass,i,float_bits(v[4*i]),float_bits(v[4*i+1]),float_bits(v[4*i+2]),float_bits(v[4*i+3]),raw_payloads[(pass-9)%4]);
   CHECK(float_bits(v[4*i])==raw_payloads[(pass-9)%4] && v[4*i+1]==0.5f && v[4*i+2]==0.75f && v[4*i+3]==1);
   continue;
  }
  if(v[4*i]!=expected || v[4*i+1]!=0.5f || v[4*i+2]!=0.75f || v[4*i+3]!=1)
   fprintf(stderr,"renderer mesh pixel %u: %g %g %g %g expected 0.25 0.5 0.75 1\n",i,v[4*i],v[4*i+1],v[4*i+2],v[4*i+3]);
  CHECK(v[4*i]==expected && v[4*i+1]==0.5f && v[4*i+2]==0.75f && v[4*i+3]==1);
 }
 }
 if(!single_pass)puts("GPU occlusion: precise 64-sample draws, disabled increments and fully clipped zero results correct");
 free(g.regs);pal_mutex_destroy(g.lock);
 if(single_pass) {
  printf("isolated mesh pass %u: 64 RGBA pixels and %u actual samples correct%s\n",first,first<13?256u:64u,
         raw_bytes?"; all 32 varying bits observed as finite bytes":"");return 0;
 }
 puts(vector_case?"large vector buffers:128 GS/PS pixels and128 actual samples correct; actual binding bounds preserved":
      scalar_case?"large scalar buffers:128 GS/PS pixels and128 actual samples correct; actual binding bounds preserved":
      getenv("PS5C_TEST_RAW_TARGET")?"float32 target:256 exact raw export pixels, NaN/subnormal payloads preserved":
      getenv("PS5C_TEST_LARGE_SNAPSHOT")?"large readonly CPU snapshot: earlier image keeps host import excluded; GS and PS read48MiB;128 RGBA pixels and128 samples correct":
      getenv("PS5C_LARGE_IMPORT_DRAWS")?"bounded readonly imports: actual GPU pixels, sample counts, mapping invalidation and completion verified":
      getenv("PS5C_LARGE_IMPORT_BATCH") && !strcmp(getenv("PS5C_LARGE_IMPORT_BATCH"),"1")?"large import batch:384 RGBA pixel readbacks and1152 actual samples correct":
      getenv("PS5C_TEST_LARGE_CACHE")?"large import cache:256 RGBA pixel readbacks and960 actual samples correct":
      getenv("PS5C_TEST_LARGE_READS")?"large readonly import: GS and PS read at48MiB in64MiB resource;128 RGBA pixels and128 actual samples correct":
      getenv("PS5C_TEST_MESH_OBJECT")?"fast launch object:128 RGBA pixels and64 actual samples correct; distinct v6 instances, v5 broadcast through lane32 and LDS b64 object lookup verified":
      getenv("PS5C_TEST_FAST_LAUNCH")?"fast launch: 128 RGBA pixels and 96 actual samples correct; vertex32, native32 wave count and disjoint point/group IDs verified":
      "renderer_draw geometry: 1216 RGBA pixels correct; mixed smooth/passthrough inputs; native mesh, strip, bounded two-wave and full four-wave compute/replay, shuffled 16/32-bit indices and signed vertex offset");return 0;
}
int main(void){
 translate_raw_byte_pixel();
 for(unsigned k=0;k<PS5C_ARRAY_COUNT(raw_payloads);k++) {
  uint32_t *spv=NULL;size_t n=0;translate_raw_compute(raw_payloads[k],&spv,&n);
  const char *dir=getenv("PS5C_TEST_SPIRV");
  if(dir) {
   char path[512];snprintf(path,sizeof(path),"%s/mesh_raw_compute_%u.spv",dir,k);
   FILE *f=fopen(path,"wb");CHECK(f && fwrite(spv,4,n,f)==n && fclose(f)==0);
  }
  free(spv);
 }
 uint32_t*guest[8]={0};size_t words[8]={0};
 CHECK(translate_guest(false,false,false,&guest[0],&words[0])==0 && translate_guest(true,false,false,&guest[1],&words[1])==0 &&
       translate_guest(false,true,false,&guest[2],&words[2])==0 && translate_guest(false,false,1,&guest[3],&words[3])==0 && translate_guest(false,false,2,&guest[4],&words[4])==0 && translate_guest(false,false,3,&guest[5],&words[5])==0 && translate_guest(false,false,4,&guest[6],&words[6])==0 && translate_guest(false,false,5,&guest[7],&words[7])==0);
 if(!getenv("PS5C_TEST_GPU")){for(unsigned k=0;k<8;k++)free(guest[k]);puts("NGG mesh translation: allocation, LDS, primitive, cull and passthrough exports compiled");return 0;}
 CHECK(pal_init()==PS5C_OK);gmem m;gmem_config cfg=gmem_default_config();cfg.identity=false;
 CHECK(gmem_init(&m,&cfg)==PS5C_OK);guest_addr a;CHECK(gmem_map(&m,0,65536,GPROT_RW,0,"native mesh fixture",&a)==PS5C_OK);
 pal_window*w=NULL;CHECK(pal_window_open("ZeraPS5 native mesh regression",320,180,&w)==PS5C_OK);
 CHECK(_putenv_s("PS5C_GPU_OCCLUSION","1")==0); ps5c_env_reset();
 char scalar_copy[64];const char *scalar_mode=getenv("PS5C_TEST_LARGE_SCALAR");
 if(scalar_mode){snprintf(scalar_copy,sizeof(scalar_copy),"%s",scalar_mode);scalar_mode=scalar_copy;}
 if(getenv("PS5C_TEST_LARGE_READS")) {
  CHECK(_putenv_s("PS5C_NGG_LARGE_READS",scalar_mode && !strcmp(scalar_mode,"clipped")?"0":"1")==0);ps5c_env_reset();
 }
 CHECK(sizeof(sh_site_shape)==16); /* buffer_length uses the old padding byte */
 if(getenv("PS5C_TEST_LOCAL_ARENA")) {
  FILE*f=fopen("local-arena-mode.txt","w");CHECK(f);fputs("0\n",f);CHECK(fclose(f)==0);
  CHECK(_putenv_s("PS5C_LOCAL_READONLY","@local-arena-mode.txt")==0); ps5c_env_reset();
 }
 renderer*r=renderer_create(w,&m);CHECK(r && r->mesh_shader && r->draw_mesh_tasks);
 if(getenv("PS5C_TEST_RAW_COMPUTE")) {
  raw_compute_test(r);for(unsigned k=0;k<8;k++)free(guest[k]);
  renderer_destroy(r);pal_window_destroy(w);gmem_free(&m);pal_shutdown();return 0;
 }
 if(getenv("PS5C_TEST_FAST_LAUNCH") || getenv("PS5C_TEST_MESH_PASS")) {
  for(unsigned k=0;k<8;k++)free(guest[k]);
  vk_buffer out;CHECK(vk_buffer_create(r,8*8*16,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&out));
  CHECK(renderer_mesh_test(r,&m,a,&out)==0);vk_buffer_destroy(r,&out);
  renderer_destroy(r);pal_window_destroy(w);gmem_free(&m);pal_shutdown();return 0;
 }
 if(getenv("PS5C_TEST_NGG_BALLOT32"))CHECK(r->mesh_native32 && (r->ballot32_stages&0x80u)); /* MESH_EXT */
 VkShaderModule ms[9]={module(r,mesh_fixture,sizeof(mesh_fixture)),module(r,guest[0],words[0]*4),module(r,guest[1],words[1]*4),module(r,guest[2],words[2]*4),module(r,guest[3],words[3]*4),module(r,guest[4],words[4]*4),module(r,guest[5],words[5]*4),module(r,guest[6],words[6]*4),module(r,guest[7],words[7]*4)};
 for(unsigned k=0;k<8;k++)free(guest[k]);
 VkShaderModule fs=module(r,fragment_fixture,sizeof(fragment_fixture));CHECK(ms[0] && ms[1] && ms[2] && ms[3] && ms[4] && ms[5] && ms[6] && ms[7] && ms[8] && fs && r->mesh_native32);
 VkAttachmentDescription att={.format=VK_FORMAT_R32G32B32A32_SFLOAT,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,
 .storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
 .initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
 VkAttachmentReference ref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&ref};
 VkRenderPassCreateInfo rc={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&att,.subpassCount=1,.pSubpasses=&sub};
 VkRenderPass rp;CHECK(vkCreateRenderPass(r->device,&rc,NULL,&rp)==VK_SUCCESS);
 VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=(VkShaderStageFlags)0x80};
 VkDescriptorSetLayoutCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
 VkDescriptorSetLayout dsl;CHECK(vkCreateDescriptorSetLayout(r->device,&dc,NULL,&dsl)==VK_SUCCESS);
 VkPipelineLayoutCreateInfo lc={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&dsl};VkPipelineLayout layout;
 CHECK(vkCreatePipelineLayout(r->device,&lc,NULL,&layout)==VK_SUCCESS);
 VkPipelineShaderStageCreateInfo st[2]={{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=(VkShaderStageFlagBits)0x80,.module=ms[0],.pName="main"},
 {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fs,.pName="main"}};
 VkViewport vp={0,0,8,8,0,1};VkRect2D rect={{0,0},{8,8}};
 VkPipelineViewportStateCreateInfo vs={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&vp,.scissorCount=1,.pScissors=&rect};
 VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.lineWidth=1};
 VkPipelineMultisampleStateCreateInfo samples={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
 VkPipelineColorBlendAttachmentState blend={.colorWriteMask=15};VkPipelineColorBlendStateCreateInfo bs={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&blend};
 VkGraphicsPipelineCreateInfo pc={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=st,.pViewportState=&vs,.pRasterizationState=&rs,.pMultisampleState=&samples,.pColorBlendState=&bs,.layout=layout,.renderPass=rp};
 VkPipeline pipe[9];for(unsigned k=0;k<9;k++){st[0].module=ms[k];CHECK(vkCreateGraphicsPipelines(r->device,VK_NULL_HANDLE,1,&pc,NULL,&pipe[k])==VK_SUCCESS);}
 vk_image*img=vk_image_get(r,a,8,8,VK_FORMAT_R32G32B32A32_SFLOAT);CHECK(img);
 VkFramebufferCreateInfo fc={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=rp,.attachmentCount=1,.pAttachments=&img->view,.width=8,.height=8,.layers=1};
 VkFramebuffer fb;CHECK(vkCreateFramebuffer(r->device,&fc,NULL,&fb)==VK_SUCCESS);
 vk_buffer out;CHECK(vk_buffer_create(r,8*8*16,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,&out));
 vk_buffer uniform;CHECK(vk_buffer_create(r,SH_U_DATA*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true,&uniform));
 memset(uniform.mapped,0,SH_U_DATA*4);uint32_t*u=uniform.mapped;u[SH_U_MESH]=3;
 float port[8]={4,4,4,4,0.25f,0.25f,1,0};memcpy(u+SH_U_VPORT,port,sizeof(port));
 VkDescriptorPoolSize pool_size={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};VkDescriptorPoolCreateInfo pool_ci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&pool_size};
 VkDescriptorPool pool;CHECK(vkCreateDescriptorPool(r->device,&pool_ci,NULL,&pool)==VK_SUCCESS);
 VkDescriptorSetAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&dsl};VkDescriptorSet set;
 CHECK(vkAllocateDescriptorSets(r->device,&alloc,&set)==VK_SUCCESS);
 VkDescriptorBufferInfo buffer={uniform.buffer,0,SH_U_DATA*4};VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffer};
 vkUpdateDescriptorSets(r->device,1,&write,0,NULL);
 for(unsigned pass=0;pass<9;pass++) {
 if(getenv("PS5C_TEST_NGG_TRACE")){fprintf(stderr,"native mesh pass %u starts\n",pass);fflush(stderr);}
 memset(out.mapped,0xcd,8*8*16);
 VkCommandBuffer cmd=vk_begin(r);vk_barrier(cmd,img,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
 VkClearValue clear={0};VkRenderPassBeginInfo begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=rp,.framebuffer=fb,.renderArea={{0,0},{8,8}},.clearValueCount=1,.pClearValues=&clear};
 vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe[pass]);
 vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,NULL);r->draw_mesh_tasks(cmd,1,1,1);vkCmdEndRenderPass(cmd);
 vk_barrier(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
 VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={8,8,1}};
 vkCmdCopyImageToBuffer(cmd,img->image,img->layout,out.buffer,1,&copy);renderer_wait_idle(r);
 float*v=out.mapped;for(unsigned i=0;i<64;i++) {
  bool correct=v[4*i]==(pass==2?0:0.25f) && v[4*i+1]==(pass==2?0:0.5f) && v[4*i+2]==(pass==2?0:0.75f) && v[4*i+3]==(pass==2?0:1);
  if(!correct){unsigned visible=0;for(unsigned j=0;j<64;j++)visible+=v[4*j+3]==1;
   fprintf(stderr,"native mesh pass %u pixel %u: %g %g %g %g; visible %u\n",pass,i,v[4*i],v[4*i+1],v[4*i+2],v[4*i+3],visible);}
  CHECK(correct);
 }
 if(getenv("PS5C_TEST_NGG_TRACE")){fprintf(stderr,"native mesh pass %u completed\n",pass);fflush(stderr);}
 }
 puts("NGG mesh: 576 RGBA pixels correct; native triangle, translated LDS/EXP_PRIM, culled primitive and passthrough VGPR0; prefix-only and later LDS barriers.");
 CHECK(renderer_mesh_test(r,&m,a,&out)==0);
 vk_buffer_destroy(r,&uniform);vk_buffer_destroy(r,&out);vkDestroyDescriptorPool(r->device,pool,NULL);vkDestroyFramebuffer(r->device,fb,NULL);
 for(unsigned k=0;k<9;k++){vkDestroyPipeline(r->device,pipe[k],NULL);vkDestroyShaderModule(r->device,ms[k],NULL);}
 vkDestroyPipelineLayout(r->device,layout,NULL);vkDestroyDescriptorSetLayout(r->device,dsl,NULL);
 vkDestroyRenderPass(r->device,rp,NULL);vkDestroyShaderModule(r->device,fs,NULL);
 renderer_destroy(r);pal_window_destroy(w);gmem_free(&m);pal_shutdown();return 0;
}
