/* Optional 16/8 pipeline observer. No timers, allocation or output in helpers.
 * Worker and ISR own distinct 32-bit fields. UI resets before publishing active,
 * and copies only after the ordered final DMA fence. Offsets wrap modulo 2^32;
 * individual frame measurements must be shorter than 71 minutes. */
#ifndef NOS_PIPELINE_PROBE_H
#define NOS_PIPELINE_PROBE_H
#include <stdint.h>
#include <string.h>
#include "../../core/cost_profile.h"
#define NOS_PIPE_INLINE static inline __attribute__((always_inline))
NOS_PIPE_INLINE void nos_pipeline_barrier(void) {
#ifdef ESP_PLATFORM
    __asm__ volatile("memw" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
typedef struct {
    /* begin_us is UI-only until after fencing; ISR/worker use origin_low. */
    uint64_t begin_us;
    uint32_t frame_id,origin_low,rawjobs,chunks,bytes;
    uint32_t first_enter,first_return,last_return,api_sum,api_max;
    uint32_t dmawait_sum,dmawait_max,rotation_sum,rotation_max;
    uint32_t queuewait_sum,queuewait_max;
    uint32_t done,first_done,last_done,done_gap_max;
    uint32_t render_done,drain_enter,completed,e2e;
    uint32_t tick,history,raster,ui_wait;
    uint32_t record,compare,layer_record,layer_raster,layer_bands,layer_reused;
    uint32_t submit_sum,submit_max;
    nos_cost_frame cost;
} nos_pipeline_frame;
typedef struct {
    volatile uint32_t active;
    nos_pipeline_frame frame;
} nos_pipeline_live;
typedef struct {
    uint32_t frames,painted,rawjobs,chunks,bytes,done,mismatch;
    uint64_t api_sum,dmawait_sum,rotation_sum,queuewait_sum;
    uint32_t api_max,dmawait_max,rotation_max,queuewait_max,done_gap_max;
    uint64_t worst_interval;
    int previous_valid,worst_valid;
    nos_pipeline_frame previous,worst_previous,worst_current;
} nos_pipeline_capture;
NOS_PIPE_INLINE void nos_pipeline_max(uint32_t *p,uint32_t v) {if(v>*p)*p=v;}
/* UI producer only. This is distinct from the worker's receive-queue idle time. */
NOS_PIPE_INLINE void nos_pipeline_submit(nos_pipeline_live *p,uint32_t elapsed) {
    p->frame.submit_sum+=elapsed;nos_pipeline_max(&p->frame.submit_max,elapsed);
}
/* Caller has already fenced the previous frame. active is immutable until the
 * final fence; a new frame may start while worker waits for its first raw job. */
NOS_PIPE_INLINE void nos_pipeline_begin(nos_pipeline_live *p,uint64_t begin) {
    p->active=0;
    memset(&p->frame,0,sizeof p->frame);
    p->frame.begin_us=begin;p->frame.origin_low=(uint32_t)begin;
    nos_pipeline_barrier();p->active=1;
}
NOS_PIPE_INLINE uint32_t nos_pipeline_offset(const nos_pipeline_live *p,uint64_t now) {
    return (uint32_t)now-p->frame.origin_low;
}
NOS_PIPE_INLINE void nos_pipeline_raw(nos_pipeline_live *p,uint32_t queuewait) {
    if(p->frame.rawjobs) {
        p->frame.queuewait_sum+=queuewait;
        nos_pipeline_max(&p->frame.queuewait_max,queuewait);
    }
    ++p->frame.rawjobs;
}
NOS_PIPE_INLINE void nos_pipeline_dmawait(nos_pipeline_live *p,uint32_t elapsed) {
    p->frame.dmawait_sum+=elapsed;nos_pipeline_max(&p->frame.dmawait_max,elapsed);
}
NOS_PIPE_INLINE void nos_pipeline_rotation(nos_pipeline_live *p,uint32_t elapsed) {
    p->frame.rotation_sum+=elapsed;nos_pipeline_max(&p->frame.rotation_max,elapsed);
}
NOS_PIPE_INLINE void nos_pipeline_enter(nos_pipeline_live *p,uint64_t now,uint32_t bytes) {
    if(!p->frame.chunks)p->frame.first_enter=nos_pipeline_offset(p,now);
    ++p->frame.chunks;p->frame.bytes+=bytes;
}
NOS_PIPE_INLINE void nos_pipeline_return(nos_pipeline_live *p,uint64_t enter,uint64_t now) {
    uint32_t elapsed=(uint32_t)(now-enter);
    if(p->frame.chunks==1)p->frame.first_return=nos_pipeline_offset(p,now);
    p->frame.last_return=nos_pipeline_offset(p,now);
    p->frame.api_sum+=elapsed;nos_pipeline_max(&p->frame.api_max,elapsed);
}
/* ISR writes all completion data BEFORE returning the DMA credit. A callback
 * is allowed before the corresponding bitmap API returns. No worker fields. */
NOS_PIPE_INLINE void nos_pipeline_done(nos_pipeline_live *p,uint64_t now) {
    uint32_t offset=nos_pipeline_offset(p,now);
    if(!p->frame.done)p->frame.first_done=offset;
    else nos_pipeline_max(&p->frame.done_gap_max,offset-p->frame.last_done);
    p->frame.last_done=offset;++p->frame.done;
    nos_pipeline_barrier();
}
/* UI only, after final DMA fence. No active writer can remain. */
NOS_PIPE_INLINE nos_pipeline_frame nos_pipeline_finish(nos_pipeline_live *p,
    uint32_t frame_id,uint64_t render_done,uint64_t drain_enter,uint64_t completed,
    uint32_t tick,uint32_t history,uint32_t raster,uint32_t wait) {
    nos_pipeline_barrier();p->active=0;
    p->frame.frame_id=frame_id;
    p->frame.render_done=nos_pipeline_offset(p,render_done);
    p->frame.drain_enter=nos_pipeline_offset(p,drain_enter);
    p->frame.completed=nos_pipeline_offset(p,completed);p->frame.e2e=p->frame.completed;
    p->frame.tick=tick;p->frame.history=history;p->frame.raster=raster;p->frame.ui_wait=wait;
    return p->frame;
}
NOS_PIPE_INLINE void nos_pipeline_capture_add(nos_pipeline_capture *c,
                                              const nos_pipeline_frame *f,int painted) {
    ++c->frames;c->painted+=painted!=0;c->rawjobs+=f->rawjobs;c->chunks+=f->chunks;
    c->bytes+=f->bytes;c->done+=f->done;c->mismatch+=f->chunks!=f->done;
    c->api_sum+=f->api_sum;c->dmawait_sum+=f->dmawait_sum;
    c->rotation_sum+=f->rotation_sum;c->queuewait_sum+=f->queuewait_sum;
    nos_pipeline_max(&c->api_max,f->api_max);nos_pipeline_max(&c->dmawait_max,f->dmawait_max);
    nos_pipeline_max(&c->rotation_max,f->rotation_max);nos_pipeline_max(&c->queuewait_max,f->queuewait_max);
    nos_pipeline_max(&c->done_gap_max,f->done_gap_max);
    if(painted) {
        if(c->previous_valid) {
            uint64_t interval=(f->begin_us+f->completed)-(c->previous.begin_us+c->previous.completed);
            if(!c->worst_valid || interval>c->worst_interval) {
                c->worst_interval=interval;c->worst_previous=c->previous;
                c->worst_current=*f;c->worst_valid=1;
            }
        }
        c->previous=*f;c->previous_valid=1;
    } else c->previous_valid=0; /* same idle-break policy as NOSCAPTURE */
}
#undef NOS_PIPE_INLINE
#endif
