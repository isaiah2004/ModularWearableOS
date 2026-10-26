#include "power_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    hal_power p={3500,15,1,0,1,0};
    assert(power_low_update(0,&p));
    p.battery_percent=19;assert(power_low_update(1,&p));assert(!power_low_update(0,&p));
    p.battery_percent=20;assert(!power_low_update(1,&p));
    p.battery_percent=0;assert(power_low_update(0,&p));
    p.battery_valid=0;assert(!power_low_update(0,&p));assert(power_low_update(1,&p));
    p.battery_valid=1;p.sample_valid=0;assert(!power_low_update(0,&p));
    p.sample_valid=1;p.external_power=-1;assert(!power_low_update(0,&p));
    p.external_power=1;assert(!power_low_update(1,&p));
    p.external_power=0;p.age_ms=120001;assert(!power_low_update(0,&p));
    for(int recording=0;recording<2;recording++)for(int walking=0;walking<2;walking++) {
        (void)recording;(void)walking;
        assert(power_idle_target(0,1,1)==NOS_PWR_SENSOR);
        assert(power_idle_target(0,0,1)==NOS_PWR_AOD);
        assert(power_idle_target(0,0,0)==NOS_PWR_OFF);
        assert(power_idle_target(0,1,0)==NOS_PWR_OFF);
        assert(power_idle_target(1,1,1)==NOS_PWR_OFF);
        assert(!power_wake_requested(0,0,0,0,0));
        assert(power_wake_requested(0,0,0,1,0));
        assert(power_wake_requested(0,0,0,0,1));
        assert(!power_wake_requested(1,0,0,1,1));
        assert(power_wake_requested(1,1,0,0,0));assert(power_wake_requested(1,0,1,0,0));
        assert(power_timeout_due(0,0,0,10001,10000));
    }
    assert(!power_timeout_due(0,0,0,10000,10000));
    assert(!power_timeout_due(1,0,0,10001,10000));
    assert(!power_timeout_due(0,1,0,10001,10000));
    assert(!power_timeout_due(0,0,1,10001,10000));
    /* G14 critical battery: sustained, qualified, hysteretic; one noisy sample never acts. */
    {
        power_crit c={0};hal_power q={3500,5,1,0,1,0};
        assert(power_critical_update(&c,&q,1000)==NOS_CRIT_NONE);
        for(int i=0;i<28;i++)assert(power_critical_update(&c,&q,1000)==NOS_CRIT_NONE);
        assert(power_critical_update(&c,&q,1000)==NOS_CRIT_NONE || c.warn_ms==NOS_CRIT_WARN_MS);
        assert(power_critical_update(&c,&q,1000)==NOS_CRIT_WARN);
        q.battery_percent=7;assert(power_critical_update(&c,&q,1000)==NOS_CRIT_WARN); /* hysteresis */
        q.battery_valid=0;q.battery_percent=0;assert(power_critical_update(&c,&q,60000)==NOS_CRIT_WARN); /* unqualified: hold */
        q.battery_valid=1;q.age_ms=120001;assert(power_critical_update(&c,&q,60000)==NOS_CRIT_WARN);q.age_ms=0;
        q.battery_percent=2;
        for(int i=0;i<59;i++)assert(power_critical_update(&c,&q,1000)==NOS_CRIT_WARN);
        assert(power_critical_update(&c,&q,1000)==NOS_CRIT_SHUTDOWN);
        q.battery_percent=4;assert(power_critical_update(&c,&q,1000)==NOS_CRIT_SHUTDOWN); /* latched */
        q.external_power=1;assert(power_critical_update(&c,&q,1000)==NOS_CRIT_NONE && !c.warn_ms && !c.off_ms);
        q.external_power=0;q.battery_percent=5;
        assert(power_critical_update(&c,&q,500000)==NOS_CRIT_NONE); /* a long gap counts as 2 s */
        q.battery_percent=6;for(int i=0;i<100;i++){power_critical_update(&c,&q,1000);}assert(c.level==NOS_CRIT_NONE && !c.warn_ms);
        q.battery_percent=8;assert(power_critical_update(&c,&q,1000)==NOS_CRIT_NONE);
        q.battery_percent=1;c=(power_crit){0};
        for(int i=0;i<30;i++){power_critical_update(&c,&q,1000);}assert(c.level==NOS_CRIT_WARN && c.off_ms==30000u);
        q.battery_percent=3;power_critical_update(&c,&q,1000);assert(c.off_ms==0 && c.level==NOS_CRIT_WARN); /* off timer must be consecutive */
    }
    /* G99 screen inactivity cap: idle or wrist-down, never earlier. */
    assert(!power_keep_expired(0,0) && !power_keep_expired(NOS_KEEP_IDLE_MS-1,0));
    assert(power_keep_expired(NOS_KEEP_IDLE_MS,0));
    assert(!power_keep_expired(NOS_KEEP_DOWN_MS-1,1) && power_keep_expired(NOS_KEEP_DOWN_MS,1));
    assert(power_wrist_down(1,0.1f) && power_wrist_down(1,-0.9f) && !power_wrist_down(1,0.8f) && !power_wrist_down(0,-1.0f));
    /* G29 torch: auto-off, dim and shorter on low battery, closed at the critical level, USB exempt. */
    {
        hal_power t={3900,80,1,0,1,0};
        assert(power_torch_state(0,&t,0)==NOS_TORCH_OK && power_torch_state(0,&t,NOS_TORCH_IDLE_MS-1)==NOS_TORCH_OK);
        assert(power_torch_state(0,&t,NOS_TORCH_IDLE_MS)==NOS_TORCH_CLOSE);
        assert(power_torch_state(1,&t,0)==NOS_TORCH_DIM && power_torch_state(1,&t,NOS_TORCH_LOW_IDLE_MS)==NOS_TORCH_CLOSE);
        t.battery_percent=NOS_CRIT_WARN_PCT;assert(power_torch_state(0,&t,0)==NOS_TORCH_CLOSE);
        t.battery_percent=NOS_CRIT_WARN_PCT+1;assert(power_torch_state(1,&t,0)==NOS_TORCH_DIM);
        t.battery_percent=1;t.external_power=1;assert(power_torch_state(0,&t,0)==NOS_TORCH_OK);
        t.external_power=0;t.battery_valid=0;assert(power_torch_state(0,&t,0)==NOS_TORCH_OK);
    }
    puts("PASS power policy: validity, external power, hysteresis, wake, background activity, critical battery, keep-awake cap, torch");
}
