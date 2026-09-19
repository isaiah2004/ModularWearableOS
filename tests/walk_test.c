#include "walk.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>

static hal_imu sample(float t,float hz,float amplitude,float gyro)
{
    hal_imu m={0};
    m.az=1+amplitude*sinf(t*hz*6.28318530718f);
    m.gx=gyro*sinf(t*hz*6.28318530718f);
    return m;
}
static void signal(walk_state *s,uint32_t start,int ms,float hz,float amp,float gyro)
{
    for(int i=0;i<ms;i+=20) {
        hal_imu m=sample(i*.001f,hz,amp,gyro);
        walk_update(s,start+(uint32_t)i,&m,1);
    }
}
int main(void)
{
    walk_state s; walk_init(&s);
    signal(&s,0,30000,2,.01f,3);
    assert(s.steps==0 && !s.walking && s.active_seconds==0);
    walk_init(&s); signal(&s,0,30000,2,.25f,45);
    assert(s.steps>=50 && s.steps<=61 && s.walking);
    assert(s.active_seconds>=24 && s.active_seconds<=30);
    uint32_t steps=s.steps,active=s.active_seconds;
    signal(&s,30000,4000,0,0,0);
    assert(!s.walking && s.steps==steps && s.active_seconds==active);
    walk_init(&s); signal(&s,0,30000,5,.8f,450);
    assert(!s.walking && s.steps==0);
    walk_init(&s); signal(&s,0,30000,5,.4f,0);
    assert(!s.walking && s.steps==0); /* Fast shaking without gyro evidence. */
    walk_init(&s);
    for(int i=0;i<30000;i+=20) {
        hal_imu sideways=sample(i*.001f,1,.25f,25);
        sideways.ax=sideways.az; sideways.az=0;
        walk_update(&s,(uint32_t)i,&sideways,1);
    }
    assert(s.walking && s.steps>=24 && s.steps<=31); /* Rotation-independent magnitude. */
    walk_init(&s); signal(&s,0,2500,2,.25f,45);
    assert(s.steps==0); /* A brief gesture must not earn the entry reward. */
    signal(&s,10000,2500,2,.25f,45);
    assert(s.steps==0); /* An observation gap cannot join two short gestures. */
    walk_init(&s); signal(&s,UINT32_MAX-5000u,30000,2,.25f,45);
    assert(s.steps>=50 && s.steps<=61 && s.active_seconds>=24);
    hal_imu m=sample(0,0,0,0);
    steps=s.steps; uint64_t time=s.active_ms;
    for(int i=0;i<100;i++) walk_update(&s,s.last_ms,&m,1);
    assert(s.steps==steps && s.active_ms==time);
    m.ax=NAN; walk_update(&s,s.last_ms+20,&m,1);
    assert(!s.valid && !s.walking && s.steps==steps);
    m.ax=0; walk_update(&s,s.last_ms+40,&m,0);
    assert(!s.valid && s.steps==steps);
    /* Repeated short/long alternating cadence never forms a coherent bout. */
    walk_init(&s);
    for(int i=0;i<30;i++) signal(&s,(uint32_t)i*1800u,1800,i&1 ? 1.0f : 3.5f,.25f,45);
    assert(s.steps==0);
    puts("walk: stationary, sustained cadence, shake, short bouts, gaps, rollover, duplicates, invalid input passed");
    return 0;
}
