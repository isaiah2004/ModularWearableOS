#include "walk.h"
#include <math.h>
#include <string.h>
#include <limits.h>

/* Tunable first-field-trial thresholds; see PET_HARDWARE_FEASIBILITY.md.
 * Eight coherent peaks avoid crediting isolated wrist gestures. Vigorous
 * spinning and implausible magnitudes invalidate the current bout. */
#define ENTRY_PEAKS 8
#define MIN_INTERVAL_MS 300u
#define MAX_INTERVAL_MS 1500u
#define GAP_MS 100u
#define END_BOUT_MS 2500u

static void end_bout(walk_state *s)
{
    s->walking=0; s->candidates=0; s->interval_count=0;
    s->high=0; s->peak=0; s->valley=0;
}

void walk_init(walk_state *s) { memset(s,0,sizeof *s); }

static void add_time(walk_state *s,uint32_t elapsed)
{
    s->active_ms+=elapsed;
    uint64_t seconds=s->active_ms/1000u;
    s->active_seconds=seconds>UINT32_MAX ? UINT32_MAX : (uint32_t)seconds;
}

static void candidate(walk_state *s,uint32_t now)
{
    unsigned entry=s->entry_peaks>=4 && s->entry_peaks<=12?(unsigned)s->entry_peaks:ENTRY_PEAKS;
    if(!s->candidates) {
        s->candidates=1; s->first_peak_ms=now; s->peak_ms=now; return;
    }
    uint32_t interval=now-s->peak_ms;
    if(interval<MIN_INTERVAL_MS || interval>MAX_INTERVAL_MS) {
        end_bout(s); s->candidates=1;
        s->first_peak_ms=now; s->peak_ms=now; return;
    }
    if(s->interval_count==7) {
        memmove(s->intervals,s->intervals+1,6*sizeof s->intervals[0]);
        s->interval_count=6;
    }
    s->intervals[s->interval_count++]=interval;
    float sum=0,variance=0;
    for(int i=0;i<s->interval_count;i++) sum+=(float)s->intervals[i];
    float mean=sum/(float)s->interval_count;
    for(int i=0;i<s->interval_count;i++) {
        float difference=(float)s->intervals[i]-mean;
        variance+=difference*difference;
    }
    /* CV <=0.25, expressed without an extra square root. */
    if(s->interval_count>=3 && variance/(float)s->interval_count>mean*mean*.0625f) {
        end_bout(s); s->candidates=1;
        s->first_peak_ms=now; s->peak_ms=now; return;
    }
    s->peak_ms=now;
    if(s->walking) {
        if(s->steps<UINT32_MAX) s->steps++;
        add_time(s,interval);
    } else if((unsigned)++s->candidates>=entry) {
        s->walking=1;
        s->steps=UINT32_MAX-s->steps<entry ? UINT32_MAX : s->steps+entry;
        add_time(s,now-s->first_peak_ms);
    }
}

void walk_update(walk_state *s,uint32_t now,const hal_imu *m,int valid)
{
    if(!s || !m) return;
    if(!valid || !isfinite(m->ax) || !isfinite(m->ay) || !isfinite(m->az) ||
       !isfinite(m->gx) || !isfinite(m->gy) || !isfinite(m->gz)) {
        end_bout(s); s->ready=0; s->valid=0; return;
    }
    float magnitude2=m->ax*m->ax+m->ay*m->ay+m->az*m->az;
    float rotation2=m->gx*m->gx+m->gy*m->gy+m->gz*m->gz;
    if(magnitude2<.04f || magnitude2>9 || rotation2>250.0f*250.0f) {
        end_bout(s); s->ready=0; s->valid=0; return;
    }
    float magnitude=sqrtf(magnitude2);
    uint32_t elapsed=now-s->last_ms;
    if(!s->ready || elapsed>GAP_MS) {
        end_bout(s); s->baseline=magnitude; s->filtered=0;
        s->last_ms=now; s->ready=1; s->valid=1; return;
    }
    if(!elapsed) return; /* A repeated cached sample is not another observation. */
    s->last_ms=now; s->valid=1;
    if(s->candidates && now-s->peak_ms>END_BOUT_MS) end_bout(s);
    float dt=(float)elapsed*.001f;
    s->baseline+=(magnitude-s->baseline)*dt/(.7f+dt);
    s->filtered+=(magnitude-s->baseline-s->filtered)*dt/(.05f+dt);
    if(s->filtered<s->valley) s->valley=s->filtered;
    float threshold=s->threshold_g>=.04f && s->threshold_g<=.20f?s->threshold_g:.10f;
    if(!s->high && s->filtered>threshold) {
        s->high=1; s->high_ms=now; s->peak=s->filtered;
    }
    if(s->high && s->filtered>s->peak) s->peak=s->filtered;
    if(s->high && s->filtered<.03f) {
        int plausible=now-s->high_ms>=80u && now-s->high_ms<=650u &&
                      s->peak<=1.0f && s->valley<=-.04f && s->peak-s->valley>=.18f;
        s->high=0;
        if(plausible) candidate(s,now);
        s->peak=0; s->valley=s->filtered;
    }
}
