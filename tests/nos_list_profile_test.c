#include "nos_list_profile.h"
#include <assert.h>
#include <stdio.h>
static nos_list_profile profile;
int main(void)
{
    nos_pipeline_frame f={.begin_us=UINT64_C(0x200001234),.frame_id=42,.e2e=17000,
        .first_enter=500,.first_done=900,.last_done=16000,.bytes=24000,.chunks=8};
    f.cost.inclusive[NOS_COST_SCENE]=6000;f.cost.exclusive[NOS_COST_SHAPE]=1000;
    nos_list_profile_add(NULL,&f,8,1,1234);
    for(unsigned i=0;i<NOS_LIST_PROFILE_CAPACITY+3;i++)nos_list_profile_add(&profile,&f,8,1,1234);
    assert(profile.count==NOS_LIST_PROFILE_CAPACITY && profile.dropped==3);
    const nos_list_sample *s=&profile.samples[profile.count-1];
    assert(s->begin_us==f.begin_us && s->frame==42 && s->pixels==12000 && s->chunks==8);
    assert(s->scene_us==6000 && s->shape_us==1000 && s->app==8 && s->dragging==1 && s->offset_q8==1234);
    assert(s->first_write_us==500 && s->first_done_us==900 && s->last_done_us==16000);
    nos_list_profile_reset(&profile);assert(!profile.count && !profile.dropped);
    puts("PASS bounded list profile: frame/pose pairing, 64-bit time, pipeline fields and overflow");
}
