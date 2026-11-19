/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Late local declarations must precede initialization without moving stores
 * relative to loads. Exercise independent prologues of two functions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/shader/spv.h"
#define CHECK(x) do { if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;} } while(0)

int main(int argc,char **argv) {
    spv_builder b;spv_init(&b);
    uint32_t cap=SpvCapabilityShader,mm[]={0,1};
    spv_emit(&b,SPV_SEC_CAPABILITY,SpvOpCapability,&cap,1);
    spv_emit(&b,SPV_SEC_MEMORY_MODEL,SpvOpMemoryModel,mm,2);
    uint32_t v=spv_type_void(&b),u=spv_type_int(&b,32,false);
    uint32_t ptr=spv_type_pointer(&b,SpvStorageClassFunction,u),type=spv_type_function(&b,v,NULL,0);
    uint32_t seven=spv_const_u32(&b,7),one=spv_const_u32(&b,1),functions[2],locals[2][3],loads[2],adds[2];
    for(unsigned k=0;k<2;k++) {
        functions[k]=spv_id(&b);spv_begin_function(&b,v,type,functions[k]);spv_label(&b,spv_id(&b));
        locals[k][0]=spv_local_var(&b,ptr);
        spv_prologue_void(&b,SpvOpStore,(uint32_t[]){locals[k][0],seven},2);
        loads[k]=spv_prologue_op(&b,SpvOpLoad,u,locals[k],1);
        locals[k][1]=spv_local_var(&b,ptr); /* discovered after a store and load */
        adds[k]=spv_prologue_op(&b,SpvOpIAdd,u,(uint32_t[]){loads[k],one},2);
        spv_prologue_void(&b,SpvOpStore,(uint32_t[]){locals[k][1],adds[k]},2);
        locals[k][2]=spv_local_var(&b,ptr);
        spv_prologue_void(&b,SpvOpStore,(uint32_t[]){locals[k][2],adds[k]},2);
        spv_op(&b,SpvOpLoad,u,locals[k]+2,1);
        spv_op_void(&b,SpvOpReturn,NULL,0);spv_end_function(&b);
    }
    uint32_t entry[8]={SpvExecutionModelGLCompute,functions[1]};
    uint32_t n=spv_string("main",entry+2,6);CHECK(n);
    spv_emit(&b,SPV_SEC_ENTRY,SpvOpEntryPoint,entry,n+2);
    spv_emit(&b,SPV_SEC_EXEC_MODE,SpvOpExecutionMode,(uint32_t[]){functions[1],SpvExecutionModeLocalSize,1,1,1},5);
    uint32_t *code=NULL;size_t words=0;CHECK(spv_finish(&b,&code,&words));
    if(argc==3 && !strcmp(argv[1],"--dump")) {
        FILE *f=fopen(argv[2],"wb");CHECK(f && fwrite(code,4,words,f)==words);CHECK(!fclose(f));
    }
    unsigned function=0,at=0;bool body=false;
    const uint32_t expected[]={SpvOpVariable,SpvOpVariable,SpvOpVariable,SpvOpStore,SpvOpLoad,
        SpvOpIAdd,SpvOpStore,SpvOpStore,SpvOpLoad,SpvOpReturn,SpvOpFunctionEnd};
    for(size_t i=5;i<words;) {
        uint32_t count=code[i]>>16,op=code[i]&65535;CHECK(count && count<=words-i);
        if(op==SpvOpFunction){CHECK(function<2 && code[i+2]==functions[function]);at=0;body=false;}
        else if(op==SpvOpLabel)body=true;
        else if(body) {
            CHECK(at<PS5C_ARRAY_COUNT(expected) && op==expected[at]);
            if(at<3)CHECK(count==4 && code[i+2]==locals[function][at] && code[i+3]==SpvStorageClassFunction);
            if(at==3)CHECK(count==3 && code[i+1]==locals[function][0] && code[i+2]==seven);
            if(at==4)CHECK(count==4 && code[i+2]==loads[function] && code[i+3]==locals[function][0]);
            if(at==5)CHECK(count==5 && code[i+2]==adds[function] && code[i+3]==loads[function] && code[i+4]==one);
            if(at==6 || at==7)CHECK(code[i+1]==locals[function][at-5] && code[i+2]==adds[function]);
            if(op==SpvOpFunctionEnd){CHECK(at+1==PS5C_ARRAY_COUNT(expected));function++;body=false;}
            at++;
        }
        i+=count;
    }
    CHECK(function==2);free(code);spv_free(&b);CHECK(!b.fn_prologue.data && !b.fn_vars.data);
    /* Also exercise a function without locals or prologue. */
    spv_init(&b);v=spv_type_void(&b);type=spv_type_function(&b,v,NULL,0);
    spv_begin_function(&b,v,type,spv_id(&b));spv_label(&b,spv_id(&b));spv_op_void(&b,SpvOpReturn,NULL,0);spv_end_function(&b);
    CHECK(b.fn_vars.len==0 && b.fn_prologue.len==0);spv_free(&b);
    puts("SPIR-V prologue: late locals lead both functions; initialization order and IDs preserved");return 0;
}
