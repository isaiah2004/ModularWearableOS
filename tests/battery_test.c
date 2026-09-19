#include "battery.h"
#include "hal.h"
#include <assert.h>
#include <stdio.h>

static hal_power observation={.external_power=-1};
void hal_power_get(hal_power *out) { *out=observation; }

int main(void)
{
    assert(BATT_DISCHARGING==0 && BATT_CHARGING==1 && BATT_FULL==2);
    battery_update();
    assert(!battery_valid() && battery_state()==BATT_UNKNOWN && battery_pct()==50);
    observation=(hal_power){.rail_mv=3830,.battery_percent=59,.battery_valid=1,
                           .external_power=0,.sample_valid=1,.age_ms=200};
    battery_update();
    assert(battery_valid() && battery_pct()==59 && battery_mv()==3830);
    assert(battery_state()==BATT_DISCHARGING);
    /* Elevated USB voltage must never become a full or charging battery. */
    observation=(hal_power){.rail_mv=4900,.battery_percent=100,.battery_valid=0,
                           .external_power=1,.sample_valid=1};
    for(int i=0;i<1000;i++) battery_update();
    assert(!battery_valid() && battery_pct()==59 && battery_mv()==4900);
    assert(battery_state()==BATT_EXTERNAL);
    /* Even contradictory input cannot make external power a valid battery. */
    observation.battery_valid=1; battery_update();
    assert(!battery_valid() && battery_pct()==59);
    observation.sample_valid=0; battery_update();
    assert(battery_state()==BATT_EXTERNAL && battery_mv()==0);
    observation.age_ms=3001; battery_update();
    assert(battery_state()==BATT_UNKNOWN && !battery_valid());
    observation=(hal_power){.rail_mv=3900,.battery_percent=66,.battery_valid=1,
                           .external_power=0,.sample_valid=1,.age_ms=100};
    battery_update();
    assert(battery_valid() && battery_pct()==66 && battery_mv()==3900);
    for(int i=0;i<10000;i++) battery_update();
    assert(battery_pct()==66 && battery_mv()==3900); /* Frame rate is irrelevant. */
    observation.battery_percent=150; battery_update();
    assert(!battery_valid() && battery_pct()==66 && battery_state()==BATT_UNKNOWN);
    observation=(hal_power){.rail_mv=4190,.battery_percent=99,.battery_valid=1,
                           .external_power=0,.sample_valid=1};
    battery_update();
    assert(battery_pct()==99 && battery_state()==BATT_DISCHARGING);
    puts("battery: qualified estimates, external/unknown/stale power, enum compatibility, frame invariance passed");
    return 0;
}
