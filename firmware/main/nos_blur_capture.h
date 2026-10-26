/* Quiet, UI-task-only counters. No allocation, printing, or renderer control.
 * Add only where the existing quiet capture is ACTIVE. Prime on its excluded
 * ARMED frame, so command/report frames never enter these totals. */
#ifndef NOS_BLUR_CAPTURE_H
#define NOS_BLUR_CAPTURE_H
#include <stdint.h>
#include <string.h>
typedef struct {
    uint32_t h_skip,h_run,v_skip,v_run,h_reduced,v_reduced;
    uint32_t ph_hit,ph_miss,pv_hit,pv_miss;
} nos_blur_counts;
typedef struct { int app,axis; int32_t offset_q8; } nos_blur_pose;
typedef struct {
    uint32_t frames,painted,pose_samples,pose_changed,drag_frames,foreign_frames;
    uint32_t history_frames,blend_frames,allocation_failures;
    uint64_t history_pixels,history_blended,history_uniform_pixels;
    uint64_t h_filtered_pixels,v_filtered_pixels;
    uint32_t h_filter_frames,v_filter_frames;
    uint64_t h_admitted,h_skipped,v_admitted,v_skipped,h_reduced,v_reduced;
    uint64_t pblur_h_hits,pblur_h_misses,pblur_v_hits,pblur_v_misses;
    nos_blur_pose first,last;
    int32_t min_q8,max_q8;
} nos_blur_capture;
static inline void nos_blur_capture_begin(nos_blur_capture *c)
{
    memset(c,0,sizeof *c);
}
static inline void nos_blur_capture_prime(nos_blur_capture *c,nos_blur_pose p)
{
    c->first=c->last=p;c->min_q8=c->max_q8=p.offset_q8;
}
static inline void nos_blur_capture_add(nos_blur_capture *c,
    const nos_blur_counts *before,const nos_blur_counts *after,nos_blur_pose pose,
    int dragging,int painted,uint32_t history_pixels,uint32_t history_blended,
    uint32_t history_uniform_pixels,uint32_t allocation_failures,
    uint32_t h_filtered_pixels,uint32_t v_filtered_pixels)
{
    c->frames++;c->painted+=painted!=0;
    c->foreign_frames+=pose.app!=c->first.app;
    if(pose.app) {
        c->pose_samples++;c->pose_changed+=pose.offset_q8!=c->last.offset_q8;
        c->drag_frames+=dragging!=0;
        if(pose.offset_q8<c->min_q8)c->min_q8=pose.offset_q8;
        if(pose.offset_q8>c->max_q8)c->max_q8=pose.offset_q8;
    }
    c->last=pose;
    c->history_frames+=history_pixels!=0;c->blend_frames+=history_blended!=0;
    c->history_pixels+=history_pixels;c->history_blended+=history_blended;
    c->history_uniform_pixels+=history_uniform_pixels;
    c->h_filtered_pixels+=h_filtered_pixels;c->v_filtered_pixels+=v_filtered_pixels;
    c->h_filter_frames+=h_filtered_pixels!=0;c->v_filter_frames+=v_filtered_pixels!=0;
    if(allocation_failures>c->allocation_failures)c->allocation_failures=allocation_failures;
#define NOS_BC_DELTA(dst,src) c->dst+=(uint32_t)(after->src-before->src)
    NOS_BC_DELTA(h_admitted,h_run);NOS_BC_DELTA(h_skipped,h_skip);
    NOS_BC_DELTA(v_admitted,v_run);NOS_BC_DELTA(v_skipped,v_skip);
    NOS_BC_DELTA(h_reduced,h_reduced);NOS_BC_DELTA(v_reduced,v_reduced);
    NOS_BC_DELTA(pblur_h_hits,ph_hit);NOS_BC_DELTA(pblur_h_misses,ph_miss);
    NOS_BC_DELTA(pblur_v_hits,pv_hit);NOS_BC_DELTA(pblur_v_misses,pv_miss);
#undef NOS_BC_DELTA
}
#endif
