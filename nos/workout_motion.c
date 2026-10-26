#include "workout_motion.h"
#include <math.h>
#include <string.h>
#include <limits.h>
static uint32_t sat(uint32_t a,uint32_t b) { return b>UINT_MAX-a?UINT_MAX:a+b; }
void workout_motion_reset(workout_motion *s) { memset(s,0,sizeof *s); }
void workout_motion_boundary(workout_motion *s)
{ s->have=s->active=0;s->candidate_ms=s->quiet_ms=0; }
void workout_motion_feed(workout_motion *s,uint32_t now,
                         float ax,float ay,float az,float gx,float gy,float gz)
{
    float a=sqrtf(ax*ax+ay*ay+az*az),g=sqrtf(gx*gx+gy*gy+gz*gz);
    if(!isfinite(a)||!isfinite(g)||a>32||g>4000) {
        int invalid=!isfinite(a)||!isfinite(g);
        uint16_t *count=invalid?&s->invalid:&s->saturated;
        if(*count<UINT16_MAX)(*count)++;
        s->reject_reason=invalid?1:2;s->rejected_ms=now;
        workout_motion_boundary(s);return;
    }
    uint32_t dt=now-s->last_ms;
    if(!s->have || dt>250) {
        if(s->have && dt>250) {
            if(s->gaps<UINT16_MAX)s->gaps++;
            s->reject_reason=3;s->rejected_ms=now;
        }
        workout_motion_boundary(s);s->have=1;s->last_ms=now;return;
    }
    s->last_ms=now;if(!dt)return;
    s->accepted_ms=now;
    s->valid_ms=sat(s->valid_ms,dt);
    /* Fixed, declared engineering thresholds. Opposite-wrist observation is
     * intentionally treated as movement; no sport or playing-hand classifier. */
    int moving=fabsf(a-1.0f)>(s->active?.10f:.16f)||g>(s->active?35.0f:55.0f);
    if(moving) {
        s->quiet_ms=0;
        if(s->active)s->movement_ms=sat(s->movement_ms,dt);
        else {
            s->candidate_ms=sat(s->candidate_ms,dt);
            if(s->candidate_ms>=300) {
                s->active=1;s->bursts=sat(s->bursts,1);
                s->movement_ms=sat(s->movement_ms,s->candidate_ms);
                s->candidate_ms=0;
            }
        }
    } else {
        s->candidate_ms=0;
        if(s->active) {
            s->quiet_ms=sat(s->quiet_ms,dt);
            if(s->quiet_ms>=600)s->active=0;
        }
    }
}
