/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PS5 libSceVdecsw AVC queues. Unlike Videodec2's Decode, input/output are
 * submitted separately. Decode real access units through the shared PAL,
 * retaining compressed input until consumed and reporting completed pictures
 * in FIFO order. ABI sizes/offsets confirmed at the t1x import call sites;
 * unsupported codecs fail explicitly. No guest buffers are retained as host
 * pointers across calls or while FFmpeg runs. */
#include <stdlib.h>
#include <string.h>
#include "base/log.h"
#include "core/runtime.h"
#include "hle/hle.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table,size_t count);

#define BAD_API ((int32_t)0x81510100)
#define BAD_SIZE ((int32_t)0x81510101)
#define BAD_POINTER ((int32_t)0x81510102)
#define BAD_DECODER ((int32_t)0x81510103)
#define BAD_BUFFER ((int32_t)0x81510106)
#define INPUT_FULL ((int32_t)0x81510114)
#define INPUT_EMPTY ((int32_t)0x81510116)
#define BUSY ((int32_t)0x81510117)
#define OUTPUT_EMPTY ((int32_t)0x81510118)
#define BAD_CODEC ((int32_t)0x81510204)
#define BAD_INPUT ((int32_t)0x81510301)
#define QUEUE_SIZE 64u
#define MAX_DECODERS 8u
#define WORK_BYTES (16ull<<20)

typedef struct input_packet {
    uint8_t *bytes;
    uint64_t size,pts,dts,attached;
    guest_addr address;
} input_packet;
typedef struct output_buffer {guest_addr address;uint64_t size;} output_buffer;
typedef struct picture {output_buffer buffer;pal_vdec_picture info;uint64_t serial;} picture;
typedef struct decoder {
    ps5_runtime *owner;
    uint64_t handle;
    pal_vdec *host;
    uint8_t *scratch;
    uint64_t scratch_size;
    uint32_t depth,max_w,max_h;
    input_packet input[QUEUE_SIZE];uint32_t in_head,in_count;
    output_buffer output[QUEUE_SIZE];uint32_t out_head,out_count;
    picture ready[QUEUE_SIZE];uint32_t ready_head,ready_count;
    picture history[QUEUE_SIZE];uint32_t history_next;
    pal_vdec_picture pending;
    bool picture_pending;
    bool finalizing,ended,input_done;
    uint64_t last_input,inputs,pictures;
    int32_t error;
    /* Each consumed AU retains its own completion until the caller polls it.
     * InputResult contains the original AU pointer and produced frame count;
     * attached_data belongs only to the decoded picture's metadata. */
    struct {guest_addr address;uint32_t frames;} completed[QUEUE_SIZE];
    uint32_t completed_head,completed_count;
} decoder;

static decoder g_decoders[MAX_DECODERS];
static uint64_t g_serial;
static uint64_t g_picture_serial;
static pal_mutex *g_lock;

static bool structure(hle_call *c,guest_addr address,void *out,size_t size) {
    uint64_t given=0;
    if(!address || !gmem_read(c->mem,address,&given,8) || given!=size)return false;
    return gmem_read(c->mem,address,out,size);
}
static uint64_t u64(const uint8_t *p){uint64_t v;memcpy(&v,p,8);return v;}
static uint32_t u32(const uint8_t *p){uint32_t v;memcpy(&v,p,4);return v;}
static void put64(uint8_t *p,uint64_t v){memcpy(p,&v,8);}
static void put32(uint8_t *p,uint32_t v){memcpy(p,&v,4);}
static decoder *lookup(hle_call *c) {
    uint64_t handle=hle_arg(c,0);
    for(unsigned i=0;i<MAX_DECODERS;i++)
        if(g_decoders[i].owner==c->rt && g_decoders[i].host && g_decoders[i].handle==handle)return &g_decoders[i];
    return NULL;
}
static void clear_queues(decoder *d) {
    for(unsigned i=0;i<QUEUE_SIZE;i++){free(d->input[i].bytes);d->input[i].bytes=NULL;}
    d->in_head=d->in_count=d->out_head=d->out_count=d->ready_head=d->ready_count=0;
    d->history_next=0;memset(d->history,0,sizeof(d->history));
    d->completed_head=d->completed_count=0;
    d->picture_pending=false;memset(&d->pending,0,sizeof(d->pending));
    d->last_input=0;
    d->finalizing=d->ended=d->input_done=false;d->error=0;
}
static void destroy(decoder *d) {
    clear_queues(d);pal_vdec_close(d->host);free(d->scratch);memset(d,0,sizeof(*d));
}
void hle_vdecsw_reset(ps5_runtime *rt) {
    if(!g_lock)return;
    pal_mutex_lock(g_lock);
    for(unsigned i=0;i<MAX_DECODERS;i++)if(g_decoders[i].owner==rt)destroy(&g_decoders[i]);
    pal_mutex_unlock(g_lock);
}

/* Host decoding is synchronous, but the public queues preserve submission,
 * completion, B-frame reordering and end-of-sequence semantics. Lock held. */
static void pump(hle_call *c,decoder *d) {
    while(!d->error && d->ready_count<QUEUE_SIZE && !d->ended) {
        pal_vdec_picture info=d->pending;int got=1;
        if(!d->picture_pending) {
        /* Input completion does not depend on an output buffer: callers may
         * wait for the first AU to be consumed before posting an output.
         * Retain one completed picture in scratch until its buffer arrives.
         * Likewise observe EOS even after the last output was consumed. */
        if(d->in_count) {
            if(d->completed_count==QUEUE_SIZE)break;
            input_packet *p=&d->input[d->in_head];
            got=pal_vdec_decode(d->host,p->bytes,p->size,p->pts,p->dts,p->attached,
                d->scratch,d->scratch_size,&info);
            d->last_input=p->attached;d->input_done=true;d->inputs++;
            uint32_t at=(d->completed_head+d->completed_count++)%QUEUE_SIZE;
            d->completed[at].address=p->address;d->completed[at].frames=got>0?1u:0u;
            free(p->bytes);memset(p,0,sizeof(*p));d->in_head=(d->in_head+1)%QUEUE_SIZE;d->in_count--;
        } else if(d->finalizing) {
            got=pal_vdec_drain(d->host,d->scratch,d->scratch_size,&info);
            if(!got){d->ended=true;break;}
        } else break;
        if(got<0){d->error=got==-2?BAD_BUFFER:BAD_INPUT;break;}
        if(!got)continue;
        d->pending=info;d->picture_pending=true;
        }
        if(!d->out_count)break;
        output_buffer buffer=d->output[d->out_head];
        uint64_t bytes=(uint64_t)info.pitch*(info.height+(info.height+1ull)/2);
        uint64_t crop_w=(uint64_t)info.crop_left+info.crop_right,crop_h=(uint64_t)info.crop_top+info.crop_bottom;
        /* AVC can code 32 rows for a visible 18-row picture. Validate the
         * visible limits and bounded macroblock padding separately; NV12's
         * UV start and allocation still use the coded dimensions. */
        if(info.width>((d->max_w+15u)&~15u) || info.height>((d->max_h+15u)&~15u) ||
           crop_w>=info.width || crop_h>=info.height || info.width-crop_w>d->max_w ||
           info.height-crop_h>d->max_h || bytes>buffer.size || bytes>d->scratch_size) {
            d->error=BAD_BUFFER;break;
        }
        if(!gmem_write(c->mem,buffer.address,d->scratch,bytes)){d->error=BAD_POINTER;break;}
        picture *p=&d->ready[(d->ready_head+d->ready_count++)%QUEUE_SIZE];
        p->buffer=buffer;p->info=info;p->serial=++g_picture_serial;
        d->picture_pending=false;
        d->out_head=(d->out_head+1)%QUEUE_SIZE;d->out_count--;d->pictures++;
        if(d->pictures==1)LOG_I(VIDEO,"vdecsw: first real AVC picture %ux%u, pitch %u",info.width,info.height,info.pitch);
    }
}

static int32_t config(hle_call *c,guest_addr a,uint8_t out[80],uint64_t *bytes) {
    if(!structure(c,a,out,80))return BAD_SIZE;
    if(u32(out+8)!=1 || u32(out+12)!=1)return BAD_CODEC;
    int32_t w=(int32_t)u32(out+24),h=(int32_t)u32(out+28);
    if((w!=-1 && (w<1 || w>8192)) || (h!=-1 && (h<1 || h>8192)) ||
       !u32(out+36) || u32(out+36)>QUEUE_SIZE)return BAD_API;
    uint64_t pitch=((uint64_t)(w>0?w:3840)+255)&~255ull,height=((uint64_t)(h>0?h:2160)+15)&~15ull;
    *bytes=pitch*(height+(height+1)/2);
    if(*bytes<WORK_BYTES)*bytes=WORK_BYTES;
    return *bytes>(128ull<<20)?BAD_API:0;
}
static void query_compute(hle_call *c) {
    uint8_t memory[24];guest_addr a=hle_arg_ptr(c,0);
    if(!structure(c,a,memory,sizeof(memory))){hle_ret_s32(c,BAD_SIZE);return;}
    put64(memory+8,WORK_BYTES);put64(memory+16,0);
    hle_ret_s32(c,gmem_write(c->mem,a,memory,sizeof(memory))?0:BAD_POINTER);
}
static void allocate_queue(hle_call *c) {
    uint8_t cfg[16],memory[24];guest_addr out=hle_arg_ptr(c,2);
    if(!structure(c,hle_arg_ptr(c,0),cfg,sizeof(cfg)) || !structure(c,hle_arg_ptr(c,1),memory,sizeof(memory))) {
        hle_ret_s32(c,BAD_SIZE);return;
    }
    uint16_t pipe,queue;memcpy(&pipe,cfg+8,2);memcpy(&queue,cfg+10,2);
    uint64_t address=u64(memory+16);
    if(pipe>4 || queue>7 || u64(memory+8)<WORK_BYTES || !address){hle_ret_s32(c,BAD_API);return;}
    hle_ret_s32(c,gmem_write_u64(c->mem,out,address)?0:BAD_POINTER);
}
static void query_memory(hle_call *c) {
    uint8_t cfg[80],memory[72];uint64_t bytes=0;guest_addr out=hle_arg_ptr(c,1);
    int32_t error=config(c,hle_arg_ptr(c,0),cfg,&bytes);
    if(error){hle_ret_s32(c,error);return;}
    if(!structure(c,out,memory,sizeof(memory))){hle_ret_s32(c,BAD_SIZE);return;}
    memset(memory+8,0,sizeof(memory)-8);
    put64(memory+8,WORK_BYTES);put64(memory+24,WORK_BYTES);put64(memory+40,WORK_BYTES);
    put64(memory+56,bytes);put32(memory+64,256);
    hle_ret_s32(c,gmem_write(c->mem,out,memory,sizeof(memory))?0:BAD_POINTER);
}
static void create(hle_call *c) {
    uint8_t cfg[80],memory[72];uint64_t bytes=0;guest_addr out=hle_arg_ptr(c,2);
    int32_t error=config(c,hle_arg_ptr(c,0),cfg,&bytes);
    if(error){hle_ret_s32(c,error);return;}
    if(!structure(c,hle_arg_ptr(c,1),memory,sizeof(memory))){hle_ret_s32(c,BAD_SIZE);return;}
    if(!u64(cfg+40) || !u64(memory+16) || !u64(memory+32) || !u64(memory+48) ||
       u64(memory+8)<WORK_BYTES || u64(memory+24)<WORK_BYTES || u64(memory+40)<WORK_BYTES || u64(memory+56)<bytes) {
        hle_ret_s32(c,BAD_API);return;
    }
    pal_mutex_lock(g_lock);decoder *d=NULL;
    for(unsigned i=0;i<MAX_DECODERS && !d;i++)if(!g_decoders[i].owner)d=&g_decoders[i];
    if(!d){pal_mutex_unlock(g_lock);hle_ret_s32(c,BAD_API);return;}
    d->host=pal_vdec_open_ex(PAL_VDEC_H264,false);d->scratch=d->host?calloc(1,(size_t)bytes):NULL;
    if(!d->host || !d->scratch){destroy(d);pal_mutex_unlock(g_lock);hle_ret_s32(c,BAD_API);return;}
    d->owner=c->rt;d->handle=0x7d300000ull+(++g_serial<<4);d->scratch_size=bytes;d->depth=u32(cfg+36);
    d->max_w=(int32_t)u32(cfg+24)>0?u32(cfg+24):3840;d->max_h=(int32_t)u32(cfg+28)>0?u32(cfg+28):2160;
    bool ok=gmem_write_u64(c->mem,out,d->handle);
    if(!ok)destroy(d);
    else LOG_I(VIDEO,"vdecsw: AVC decoder up to %ux%u, input queue %u",d->max_w,d->max_h,d->depth);
    pal_mutex_unlock(g_lock);hle_ret_s32(c,ok?0:BAD_POINTER);
}
static void delete_decoder(hle_call *c) {
    pal_mutex_lock(g_lock);decoder *d=lookup(c);bool found=d!=NULL;
    if(d){LOG_I(VIDEO,"vdecsw: closed after %llu inputs, %llu pictures",(unsigned long long)d->inputs,(unsigned long long)d->pictures);destroy(d);}
    pal_mutex_unlock(g_lock);hle_ret_s32(c,found?0:BAD_DECODER);
}
static void set_input(hle_call *c) {
    uint8_t data[48];
    if(!structure(c,hle_arg_ptr(c,1),data,sizeof(data))){hle_ret_s32(c,BAD_SIZE);return;}
    uint64_t size=u64(data+16);if(!size || size>(64ull<<20)){hle_ret_s32(c,BAD_INPUT);return;}
    uint8_t *copy=malloc((size_t)size);
    if(!copy){hle_ret_s32(c,BAD_API);return;}
    if(!gmem_read(c->mem,u64(data+8),copy,size)){free(copy);hle_ret_s32(c,BAD_POINTER);return;}
    pal_mutex_lock(g_lock);decoder *d=lookup(c);int32_t error=0;
    if(!d)error=BAD_DECODER;
    else if(d->error)error=d->error;
    /* Full input and pending TrySync output have distinct status codes.
     * Return without accepting this AU so the caller can poll completions
     * and retry; blocking here can prevent it from starting its consumer. */
    else if(d->finalizing)error=BAD_API;
    else if(d->in_count==d->depth || d->completed_count+d->in_count==QUEUE_SIZE)error=INPUT_FULL;
    else {
        input_packet *p=&d->input[(d->in_head+d->in_count++)%QUEUE_SIZE];
        *p=(input_packet){copy,size,u64(data+24),u64(data+32),u64(data+40),u64(data+8)};copy=NULL;
        pump(c,d);error=d->error;
    }
    pal_mutex_unlock(g_lock);free(copy);hle_ret_s32(c,error);
}
static void set_output(hle_call *c) {
    uint8_t data[24];
    if(!structure(c,hle_arg_ptr(c,1),data,sizeof(data))){hle_ret_s32(c,BAD_SIZE);return;}
    output_buffer buffer={u64(data+8),u64(data+16)};
    if(!buffer.address || !buffer.size || !gmem_is_mapped(c->mem,buffer.address,buffer.size)){hle_ret_s32(c,BAD_POINTER);return;}
    pal_mutex_lock(g_lock);decoder *d=lookup(c);int32_t error=0;
    if(!d)error=BAD_DECODER;
    else if(d->out_count+d->ready_count==QUEUE_SIZE)error=BUSY;
    else {
        for(uint32_t i=0;i<d->out_count;i++)if(d->output[(d->out_head+i)%QUEUE_SIZE].address==buffer.address)error=BAD_API;
        for(uint32_t i=0;i<d->ready_count;i++)if(d->ready[(d->ready_head+i)%QUEUE_SIZE].buffer.address==buffer.address)error=BAD_API;
        if(!error){d->output[(d->out_head+d->out_count++)%QUEUE_SIZE]=buffer;pump(c,d);error=d->error;}
    }
    pal_mutex_unlock(g_lock);hle_ret_s32(c,error);
}
static void try_output(hle_call *c) {
    uint8_t out[56];guest_addr address=hle_arg_ptr(c,1);
    if(!structure(c,address,out,sizeof(out))){hle_ret_s32(c,BAD_SIZE);return;}
    memset(out+8,0,sizeof(out)-8);
    pal_mutex_lock(g_lock);decoder *d=lookup(c);int32_t error=0;
    if(!d)error=BAD_DECODER;
    else {
        pump(c,d);error=d->error;
        if(!error && d->ready_count) {
            picture p=d->ready[d->ready_head];d->ready_head=(d->ready_head+1)%QUEUE_SIZE;d->ready_count--;
            /* PS5 OutputInfo differs from Videodec2: valid/last/error/count
             * occupy bytes 8..11; codec is DWORD 12. Confirmed against the
             * consumer and KytyPS5 src/libs/libVdecsw.cpp OutputInfo. Mark
             * the last real picture only after the decoder drained. */
            out[8]=1;out[9]=d->ended && !d->ready_count;out[10]=p.info.corrupt;out[11]=1;
            put32(out+12,1);
            put32(out+16,p.info.width);put32(out+20,p.info.pitch);put32(out+24,p.info.height);
            put64(out+32,p.buffer.address);put64(out+40,p.buffer.size);put32(out+52,p.info.pitch);
            d->history[d->history_next++%QUEUE_SIZE]=p;
        } else if(!error)error=d->out_count?BUSY:OUTPUT_EMPTY;
    }
    bool ok=gmem_write(c->mem,address,out,sizeof(out));pal_mutex_unlock(g_lock);
    hle_ret_s32(c,ok?error:BAD_POINTER);
}
static void try_input(hle_call *c) {
    uint8_t out[24];guest_addr address=hle_arg_ptr(c,1);
    if(!structure(c,address,out,sizeof(out))){hle_ret_s32(c,BAD_SIZE);return;}
    memset(out+8,0,sizeof(out)-8);
    pal_mutex_lock(g_lock);decoder *d=lookup(c);int32_t error=0;
    if(!d)error=BAD_DECODER;
    else {
        pump(c,d);error=d->error;
        if(!error && d->completed_count) {
            uint32_t at=d->completed_head;
            put64(out+8,d->completed[at].address);put32(out+16,d->completed[at].frames);
            d->completed_head=(at+1)%QUEUE_SIZE;d->completed_count--;
        } else if(!error)error=INPUT_EMPTY;
        d->input_done=d->completed_count!=0;
    }
    bool ok=gmem_write(c->mem,address,out,sizeof(out));pal_mutex_unlock(g_lock);
    hle_ret_s32(c,ok?error:BAD_POINTER);
}
static void finalize(hle_call *c) {
    pal_mutex_lock(g_lock);decoder *d=lookup(c);
    if(d){d->finalizing=true;pump(c,d);}
    int32_t error=d?d->error:BAD_DECODER;pal_mutex_unlock(g_lock);hle_ret_s32(c,error);
}
static void reset(hle_call *c) {
    pal_mutex_lock(g_lock);decoder *d=lookup(c);
    if(d){clear_queues(d);pal_vdec_reset(d->host);}
    pal_mutex_unlock(g_lock);hle_ret_s32(c,d?0:BAD_DECODER);
}
static void get_avc(hle_call *c) {
    uint8_t output[56],first[120],second[120];guest_addr a=hle_arg_ptr(c,1),b=hle_arg_ptr(c,2);
    if(!structure(c,hle_arg_ptr(c,0),output,sizeof(output)) || !structure(c,a,first,sizeof(first)) ||
       (b && !structure(c,b,second,sizeof(second)))){hle_ret_s32(c,BAD_SIZE);return;}
    picture found={0};bool have=false;
    pal_mutex_lock(g_lock);
    for(unsigned i=0;i<MAX_DECODERS;i++)if(g_decoders[i].owner==c->rt)
        for(unsigned j=0;j<QUEUE_SIZE;j++)if(g_decoders[i].history[j].buffer.address==u64(output+32)) {
            const picture *p=&g_decoders[i].history[j];
            if(p->buffer.address && (!have || p->serial>found.serial)){found=*p;have=true;}
        }
    pal_mutex_unlock(g_lock);
    if(!have || !output[8] || output[11]!=1 || u32(output+12)!=1){hle_ret_s32(c,BAD_API);return;}
    const pal_vdec_picture *p=&found.info;memset(first+8,0,sizeof(first)-8);
    first[8]=1;put64(first+16,p->pts);put64(first+24,p->dts);put64(first+32,p->attached);
    first[40]=p->key;first[41]=(uint8_t)p->profile;first[42]=(uint8_t)p->level;
    put32(first+44,(p->width+15)/16-1);put32(first+48,(p->height+15)/16-1);first[52]=1;
    /* This API's consumer uses coded NV12 rows to locate the UV plane and
     * subtracts cropping in pixels for display. Keep FFmpeg's coded frame
     * and its crop metadata together instead of applying the crop twice. */
    uint32_t left=p->crop_left,right=p->crop_right;
    uint32_t top=p->crop_top,bottom=p->crop_bottom;
    first[53]=left || right || top || bottom;
    put32(first+56,left);put32(first+60,right);put32(first+64,top);put32(first+68,bottom);
    first[72]=p->sar_width && p->sar_height;first[73]=first[72]?255:0;
    memcpy(first+74,&p->sar_width,2);memcpy(first+76,&p->sar_height,2);
    first[78]=1;first[79]=5;first[80]=p->full_range;first[81]=p->primaries || p->transfer || p->matrix;
    first[82]=p->primaries;first[83]=p->transfer;first[84]=p->matrix;
    bool ok=gmem_write(c->mem,a,first,sizeof(first));
    if(b){memset(second+8,0,sizeof(second)-8);ok=gmem_write(c->mem,b,second,sizeof(second)) && ok;}
    hle_ret_s32(c,ok?0:BAD_POINTER);
}
static void release_queue(hle_call *c){hle_ret_s32(c,hle_arg(c,0)?0:BAD_POINTER);}

#define L "libSceVdecsw"
static const hle_export exports[]={
    HLE_FUNC_ENTRY(L,"sceVdecswQueryComputeMemoryInfo",query_compute),
    HLE_FUNC_ENTRY(L,"sceVdecswAllocateComputeQueue",allocate_queue),
    HLE_FUNC_ENTRY(L,"sceVdecswReleaseComputeQueue",release_queue),
    HLE_FUNC_ENTRY(L,"sceVdecswQueryDecoderMemoryInfo",query_memory),
    HLE_FUNC_ENTRY(L,"sceVdecswCreateDecoder",create),
    HLE_FUNC_ENTRY(L,"sceVdecswDeleteDecoder",delete_decoder),
    HLE_FUNC_ENTRY(L,"sceVdecswSetDecodeInput",set_input),
    HLE_FUNC_ENTRY(L,"sceVdecswSetDecodeOutput",set_output),
    HLE_FUNC_ENTRY(L,"sceVdecswTrySyncDecodeInput",try_input),
    HLE_FUNC_ENTRY(L,"sceVdecswTrySyncDecodeOutput",try_output),
    HLE_FUNC_ENTRY(L,"sceVdecswFinalizeDecodeSequence",finalize),
    HLE_FUNC_ENTRY(L,"sceVdecswResetDecoder",reset),
    HLE_FUNC_ENTRY(L,"sceVdecswGetAvcPictureInfo",get_avc),
};
void hle_register_vdecsw(void) {
    if(!g_lock && pal_mutex_create(&g_lock)!=PS5C_OK)return;
    hle_register_table(exports,PS5C_ARRAY_COUNT(exports));
}
