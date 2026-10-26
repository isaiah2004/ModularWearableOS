/* Optional bounded per-frame list trace. UI owns this PSRAM allocation; no
 * allocation, printing, or extra clocks in the measured frame path. DMA times
 * describe writes to GRAM, not the panel's optical scanout. */
#ifndef NOS_LIST_PROFILE_H
#define NOS_LIST_PROFILE_H
#include "nos_pipeline_probe.h"
#define NOS_LIST_PROFILE_CAPACITY 512
typedef struct {
    uint64_t begin_us;
    uint32_t frame,e2e_us,tick_us,record_us,scene_us,raster_us,wait_us;
    uint32_t first_write_us,first_done_us,last_done_us,gap_us,rotation_us,api_us;
    uint32_t pixels,chunks,rawjobs,scene_other_us,shape_us,sprite_us;
    int32_t offset_q8;
    uint16_t app,dragging;
} nos_list_sample;
typedef struct {
    uint32_t count,dropped;
    nos_list_sample samples[NOS_LIST_PROFILE_CAPACITY];
} nos_list_profile;
static inline void nos_list_profile_reset(nos_list_profile *p)
{
    if(p)p->count=p->dropped=0;
}
static inline void nos_list_profile_add(nos_list_profile *p,const nos_pipeline_frame *f,
    int app,int dragging,int32_t offset_q8)
{
    if(!p)return;
    if(p->count==NOS_LIST_PROFILE_CAPACITY) {++p->dropped;return;}
    nos_list_sample *s=&p->samples[p->count++];
    *s=(nos_list_sample){.begin_us=f->begin_us,.frame=f->frame_id,.e2e_us=f->e2e,
        .tick_us=f->tick,.record_us=f->record,.raster_us=f->raster,.wait_us=f->ui_wait,
        .first_write_us=f->first_enter,.first_done_us=f->first_done,.last_done_us=f->last_done,
        .gap_us=f->done_gap_max,.rotation_us=f->rotation_sum,.api_us=f->api_sum,
        .pixels=f->bytes/2,.chunks=f->chunks,.rawjobs=f->rawjobs,
        .offset_q8=offset_q8,.app=(uint16_t)app,.dragging=(uint16_t)dragging};
    s->scene_us=f->cost.inclusive[NOS_COST_SCENE];
    s->scene_other_us=f->cost.exclusive[NOS_COST_SCENE];
    s->shape_us=f->cost.exclusive[NOS_COST_SHAPE];
    s->sprite_us=f->cost.exclusive[NOS_COST_SPRITE8];
}
#endif
