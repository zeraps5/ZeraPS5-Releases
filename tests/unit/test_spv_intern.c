/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Compare emitted IDs/words with the former linear lookup, including growth
 * and repeated operands. The oracle has no hash table or builder intern calls. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/shader/spv.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
typedef struct reference {
    spv_builder b;
    spv_intern *entries;
    size_t len, cap;
} reference;

static uint32_t ref(reference *r,uint32_t op,const uint32_t *args,uint32_t n,bool first) {
    uint32_t key[8]={op};
    if(n)memcpy(key+1,args,n*4);
    for(size_t i=0;i<r->len;i++)
        if(r->entries[i].len==n+1 && !memcmp(r->entries[i].key,key,(n+1)*4))return r->entries[i].id;
    if(r->len==r->cap) {
        size_t cap=r->cap?r->cap*2:64;
        spv_intern *p=realloc(r->entries,cap*sizeof(*p));
        if(!p){r->b.failed=true;return 0;}
        r->entries=p;r->cap=cap;
    }
    uint32_t id=spv_id(&r->b),words[8];
    if(first){words[0]=id;if(n)memcpy(words+1,args,n*4);}
    else {words[0]=args[0];words[1]=id;if(n>1)memcpy(words+2,args+1,(n-1)*4);}
    spv_emit(&r->b,SPV_SEC_GLOBAL,op,words,n+1);
    spv_intern *e=r->entries+r->len++;
    memcpy(e->key,key,sizeof(key));e->len=n+1;e->id=id;
    return id;
}

int main(void) {
    spv_builder b;reference r={0};spv_init(&b);spv_init(&r.b);
    uint32_t a[7],u,s,f,bo;
    CHECK(spv_type_void(&b)==ref(&r,SpvOpTypeVoid,NULL,0,true));
    bo=spv_type_bool(&b);CHECK(bo==ref(&r,SpvOpTypeBool,NULL,0,true));
    a[0]=32;a[1]=0;u=spv_type_int(&b,32,false);CHECK(u==ref(&r,SpvOpTypeInt,a,2,true));
    a[1]=1;s=spv_type_int(&b,32,true);CHECK(s==ref(&r,SpvOpTypeInt,a,2,true) && s!=u);
    a[0]=32;f=spv_type_float(&b,32);CHECK(f==ref(&r,SpvOpTypeFloat,a,1,true));
    for(unsigned pass=0;pass<6;pass++)for(uint32_t i=0;i<2048;i++) {
        uint32_t v=((i*1031u+pass*19u)&2047u)*0x9e3779b9u;
        if(!(i%127))CHECK(spv_id(&b)==spv_id(&r.b)); /* IDs outside the intern table */
        a[0]=u;a[1]=v;
        uint32_t c=spv_const_u32(&b,v);CHECK(c==ref(&r,SpvOpConstant,a,2,false));
        uint32_t recovered=0;CHECK(spv_const_u32_value(&b,c,&recovered) && recovered==v);
        a[0]=s;uint32_t signed_id=spv_const_i32(&b,(int32_t)v);CHECK(signed_id==ref(&r,SpvOpConstant,a,2,false));
        CHECK(!spv_const_u32_value(&b,signed_id,&recovered));
        float bits;memcpy(&bits,&v,4);a[0]=f;
        uint32_t float_id=spv_const_f32(&b,bits);CHECK(float_id==ref(&r,SpvOpConstant,a,2,false));
        CHECK(!spv_const_u32_value(&b,float_id,&recovered));
        a[0]=bo;CHECK(spv_const_bool(&b,(v&1)!=0)==ref(&r,(v&1)?SpvOpConstantTrue:SpvOpConstantFalse,a,1,false));
        a[0]=u;a[1]=2+(v&1);
        uint32_t vec=spv_type_vector(&b,u,a[1]);CHECK(vec==ref(&r,SpvOpTypeVector,a,2,true));
        a[0]=vec;a[1]=a[2]=a[3]=c;
        uint32_t n=2+(v&1);CHECK(spv_const_composite(&b,vec,a+1,n)==ref(&r,SpvOpConstantComposite,a,n+1,false));
        a[0]=SpvStorageClassStorageBuffer;a[1]=vec;
        CHECK(spv_type_pointer(&b,a[0],a[1])==ref(&r,SpvOpTypePointer,a,2,true));
        a[0]=u;a[1]=c;CHECK(spv_type_array(&b,u,c)==ref(&r,SpvOpTypeArray,a,2,true));
        a[0]=u;a[1]=u;a[2]=vec;
        CHECK(spv_type_function(&b,u,a+1,2)==ref(&r,SpvOpTypeFunction,a,3,true));
        a[0]=f;a[1]=SpvDimDim2D;a[2]=0;a[3]=v&1;a[4]=0;a[5]=1;a[6]=SpvImageFormatUnknown;
        uint32_t img=spv_type_image(&b,a);CHECK(img==ref(&r,SpvOpTypeImage,a,7,true));
        a[0]=img;CHECK(spv_type_sampled_image(&b,img)==ref(&r,SpvOpTypeSampledImage,a,1,true));
    }
    /* Float bit identity: both zero signs and NaN payloads must stay distinct. */
    const uint32_t special[]={0,0x80000000u,0x7f800000u,0xff800000u,0x7fc00001u,0x7fc00002u};
    for(unsigned i=0;i<PS5C_ARRAY_COUNT(special);i++) {
        float value;memcpy(&value,special+i,4);a[0]=f;a[1]=special[i];
        CHECK(spv_const_f32(&b,value)==ref(&r,SpvOpConstant,a,2,false));
    }
    uint32_t *actual=NULL,*expected=NULL;size_t actual_words=0,expected_words=0;
    uint32_t absent=0;CHECK(!spv_const_u32_value(&b,0,&absent) && !spv_const_u32_value(&b,u,&absent));
    CHECK(!spv_const_u32_value(&b,u,NULL) && !spv_const_u32_value(&b,spv_id(&b),&absent));
    CHECK(spv_id(&r.b)==b.next_id-1); /* mirror the extra external ID */
    CHECK(!b.failed && !r.b.failed && b.intern_len==r.len);
    CHECK(b.intern_slot_cap>=b.intern_len*2 && PS5C_IS_POW2(b.intern_slot_cap));
    CHECK(spv_finish(&b,&actual,&actual_words) && spv_finish(&r.b,&expected,&expected_words));
    CHECK(actual_words==expected_words && !memcmp(actual,expected,actual_words*4));
    b.failed=true;uint32_t next=b.next_id;
    CHECK(spv_const_u32(&b,0x11223344)==0 && b.next_id==next && !spv_finish(&b,&actual,&actual_words));
    printf("SPIR-V intern: %zu unique entries, IDs and %zu words match linear reference\n",r.len,expected_words);
    free(actual);free(expected);free(r.entries);spv_free(&r.b);spv_free(&b);
    CHECK(!b.intern_slots && !b.intern_slot_cap && !b.interned);
    spv_init(&b);CHECK(spv_const_u32(&b,17)==2);spv_free(&b);
    return 0;
}
