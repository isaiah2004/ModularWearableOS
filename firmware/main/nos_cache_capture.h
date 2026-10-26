#ifndef NOS_CACHE_CAPTURE_H
#define NOS_CACHE_CAPTURE_H
#include <stdint.h>
#include <string.h>
#include "ui_blur_observe.h"
/* Accumulate only under the SAME ACTIVE frame gate as NOSBLURCAP.
 * Areas count cache draw-call writes/requests, including overlap and later
 * covered pixels; they are not unique final-visible or optical coverage. */
typedef struct {
    uint32_t frames,painted,requested_frames,sharp_frames,blurred_frames,stale_frames;
    uint32_t native_frames,native_blurred_frames;
    uint64_t requested_processed_area,sharp_processed_area,blurred_processed_area,unknown_requests;
    uint64_t native_processed_area,native_blurred_processed_area;
} nos_cache_capture;
static inline void nos_cache_capture_begin(nos_cache_capture *c) {memset(c,0,sizeof *c);}
static inline void nos_cache_capture_add(nos_cache_capture *c,const ui_fblur_observation *o,
                                       unsigned frame,int painted)
{
    c->frames++;c->painted+=painted!=0;
    if(o->frame!=frame) {c->stale_frames++;return;}
    c->requested_frames+=o->requested_processed_area!=0;
    c->sharp_frames+=o->sharp_processed_area!=0;
    c->blurred_frames+=o->blurred_processed_area!=0;
    c->requested_processed_area+=o->requested_processed_area;
    c->sharp_processed_area+=o->sharp_processed_area;
    c->blurred_processed_area+=o->blurred_processed_area;
    c->unknown_requests+=o->unknown_requests;
    c->native_frames+=o->native_processed_area!=0;
    c->native_blurred_frames+=o->native_blurred_processed_area!=0;
    c->native_processed_area+=o->native_processed_area;
    c->native_blurred_processed_area+=o->native_blurred_processed_area;
}
#endif
