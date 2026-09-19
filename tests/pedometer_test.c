#include "pedometer.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static void walking(pedometer *p,uint32_t day) {
    for(int i=0;i<30000;i+=20) {
        hal_imu m={.az=1+.25f*sinf(i*.001f*2*6.2831853f),.gx=30};
        pedometer_update(p,day,(uint32_t)i,&m,1);
    }
}
int main(void) {
    pedometer p;pedometer_init(&p,NULL);
    walking(&p,20260915);
    hal_activity a;pedometer_get(&p,&a);
    assert(a.steps>=50 && a.steps<=61 && a.active_seconds>=24);
    pedometer_saved saved=p.today;
    pedometer_init(&p,&saved); /* Reboot restores totals, not an in-flight bout. */
    hal_imu still={.az=1};pedometer_update(&p,0,0,&still,1);
    assert(p.today.steps==saved.steps && !p.walk.walking);
    walking(&p,20260915);assert(p.today.steps>=saved.steps+50);
    pedometer_update(&p,20260916,30000,&still,1);
    assert(p.today.steps==0 && p.today.active_ms==0 && !p.walk.walking);
    saved.version=999;pedometer_init(&p,&saved);assert(p.today.steps==0);
    pedometer_init(&p,NULL);walking(&p,0);
    uint32_t steps=p.today.steps;pedometer_update(&p,20260916,30000,&still,1);
    assert(p.today.steps==steps); /* First time sync retains current boot steps. */
    puts("pedometer: cadence, reboot, unknown clock, midnight and corrupt version passed");
}
