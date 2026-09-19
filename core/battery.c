/* Shared adapter over the board's qualified, cached power observation.
 * No new ADC reads, frame-rate-dependent smoothing, or charge-current claims. */
#include "battery.h"
#include "hal.h"

static int s_mv;
static int s_pct=50; /* Legacy faces require a number; meaningful only when valid. */
static int s_valid;
static int s_state=BATT_UNKNOWN;

void battery_update(void)
{
    hal_power p;
    hal_power_get(&p);
    int fresh=p.age_ms<=3000;
    s_mv=fresh && p.sample_valid && p.rail_mv>0 ? p.rail_mv : 0;
    s_valid=fresh && p.sample_valid && p.battery_valid && p.external_power==0 &&
            p.battery_percent>=0 && p.battery_percent<=100 && s_mv>0;
    if(s_valid) s_pct=p.battery_percent;
    if(!fresh) s_state=BATT_UNKNOWN;
    else if(p.external_power==1) s_state=BATT_EXTERNAL;
    else if(s_valid) s_state=BATT_DISCHARGING; /* Historical enum: inferred battery supply. */
    else s_state=BATT_UNKNOWN;
}

int battery_mv(void) { return s_mv; }
int battery_pct(void) { return s_pct; }
int battery_valid(void) { return s_valid; }
int battery_state(void) { return s_state; }
