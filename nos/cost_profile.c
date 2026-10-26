#include "cost_profile.h"
#if NOS_RENDER_PROFILING
#include "hal.h"
#include <stdio.h>
#include <string.h>
/* UI-thread only. Scope nesting makes exclusive categories disjoint even when
 * live text uses sprites or a shape calls another shape. Timings are elapsed
 * wall durations, including interrupts/cache stalls, never pure CPU counters. */
int nos_cost_active;
static int enabled;
static nos_cost_scope *top;
static nos_cost_frame frame;
static uint64_t inclusive[NOS_COST_COUNT], exclusive[NOS_COST_COUNT], calls[NOS_COST_COUNT];
static uint32_t frames,painted_frames,errors;
void nos_cost_enter(nos_cost_scope *s,unsigned bucket) {
    if(!nos_cost_active || bucket>=NOS_COST_COUNT)return;
    s->parent=top;s->bucket=bucket;s->children=0;s->live=1;
    top=s;s->start=hal_micros();
}
void nos_cost_leave(nos_cost_scope *s) {
    if(!s->live)return;
    uint32_t duration=hal_micros()-s->start;
    if(top!=s || s->children>duration) {errors++;nos_cost_active=0;top=0;return;}
    frame.inclusive[s->bucket]+=duration;
    frame.exclusive[s->bucket]+=duration-s->children;frame.calls[s->bucket]++;
    top=s->parent;if(top)top->children+=duration;s->live=0;
}
void nos_cost_set(int value) {enabled=!!value;nos_cost_active=0;top=0;}
int nos_cost_enabled(void) {return enabled;}
void nos_cost_begin(void) {memset(&frame,0,sizeof frame);top=0;nos_cost_active=enabled;}
void nos_cost_end(void) {if(top){errors++;top=0;}nos_cost_active=0;}
nos_cost_frame nos_cost_last(void) {return frame;}
void nos_cost_capture_reset(void) {
    memset(inclusive,0,sizeof inclusive);memset(exclusive,0,sizeof exclusive);
    memset(calls,0,sizeof calls);frames=painted_frames=errors=0;
}
void nos_cost_capture_add(int painted) {
    if(!enabled)return;
    frames++;painted_frames+=!!painted;
    for(unsigned b=0;b<NOS_COST_COUNT;b++) {
        inclusive[b]+=frame.inclusive[b];exclusive[b]+=frame.exclusive[b];calls[b]+=frame.calls[b];
    }
}
void nos_cost_report(void) {
    static const char *names[]={"scene_other","clear","sprite8","sprite565","text","shape"};
    printf("NOSCOST v=1 enabled=%d frames=%u painted=%u errors=%u elapsed_wall=1 exclusive_disjoint=1\n",enabled,(unsigned)frames,(unsigned)painted_frames,(unsigned)errors);
    for(unsigned b=0;b<NOS_COST_COUNT;b++)printf("NOSCOSTBUCKET bucket=%s inclusive_us=%llu exclusive_us=%llu calls=%llu\n",
        names[b],(unsigned long long)inclusive[b],(unsigned long long)exclusive[b],(unsigned long long)calls[b]);
}
void nos_cost_calibrate(void) {
    int saved=enabled;enabled=1;nos_cost_begin();
    uint32_t start=hal_micros();
    for(unsigned i=0;i<1024;i++) {NOS_COST_SCOPE(NOS_COST_SCENE,1);}
    uint32_t elapsed=hal_micros()-start;nos_cost_end();enabled=saved;
    printf("NOSCOSTCAL v=1 scopes=1024 elapsed_us=%u average_ns=%u elapsed_wall=1\n",(unsigned)elapsed,(unsigned)((uint64_t)elapsed*1000/1024));
}
#endif
