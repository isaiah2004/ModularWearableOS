#ifndef NOS_POWER_POLICY_H
#define NOS_POWER_POLICY_H
#include "hal.h"
/* Runtime policy only: never overwrite the user's AOD/sensor preferences. */
enum { NOS_PWR_ON, NOS_PWR_AOD, NOS_PWR_OFF, NOS_PWR_SENSOR };
static inline int power_low_update(int low,const hal_power *p)
{
    if(p->external_power==1)return 0;
    if(p->external_power!=0 || !p->sample_valid || !p->battery_valid ||
       p->age_ms>120000 || p->battery_percent<0 || p->battery_percent>100)return low;
    return low ? p->battery_percent<20 : p->battery_percent<=15;
}
static inline int power_idle_target(int low,int sensor_idle,int aod)
{ return low || !aod?NOS_PWR_OFF:sensor_idle?NOS_PWR_SENSOR:NOS_PWR_AOD; } /* sensor idle is an AOD variant: AOD off means panel off */
/* Walking/recording are background services, never display wake sources. */
static inline int power_wake_requested(int low,int touch,int button,int raised,int knock)
{ return touch || button || (!low && (raised || knock)); }
static inline int power_timeout_due(int no_sleep,int keep,int down,unsigned awake,unsigned timeout)
{ return !no_sleep && !keep && !down && awake>timeout; }
/* Critical battery (G14). Same validity rules as the low latch: only a fresh, qualified on-battery
 * reading counts. The rail ADC can be ~150 mV off and sags under load, so nothing acts on one sample:
 * WARN needs <=5 % for 30 s, SHUTDOWN needs <=2 % for 60 s; both clear at >=8 % or on external power
 * (hysteresis). dt_ms is the time since the previous call (a long gap counts as at most 2 s). */
enum { NOS_CRIT_NONE, NOS_CRIT_WARN, NOS_CRIT_SHUTDOWN };
#define NOS_CRIT_WARN_PCT 5
#define NOS_CRIT_OFF_PCT 2
#define NOS_CRIT_CLEAR_PCT 8
#define NOS_CRIT_WARN_MS 30000u
#define NOS_CRIT_OFF_MS 60000u
typedef struct { int level; unsigned warn_ms, off_ms; } power_crit;
static inline int power_critical_update(power_crit *c,const hal_power *p,unsigned dt_ms)
{
    if(dt_ms>2000u)dt_ms=2000u;
    if(p->external_power==1){c->level=NOS_CRIT_NONE;c->warn_ms=c->off_ms=0;return c->level;}
    if(p->external_power!=0 || !p->sample_valid || !p->battery_valid ||
       p->age_ms>120000 || p->battery_percent<0 || p->battery_percent>100)return c->level;
    if(p->battery_percent>=NOS_CRIT_CLEAR_PCT){c->level=NOS_CRIT_NONE;c->warn_ms=c->off_ms=0;return c->level;}
    if(p->battery_percent<=NOS_CRIT_WARN_PCT){c->warn_ms+=dt_ms;if(c->warn_ms>NOS_CRIT_WARN_MS)c->warn_ms=NOS_CRIT_WARN_MS;}
    else if(c->level==NOS_CRIT_NONE)c->warn_ms=0; /* 6..7 %: not yet latched, so the 30 s must be consecutive */
    if(p->battery_percent<=NOS_CRIT_OFF_PCT){c->off_ms+=dt_ms;if(c->off_ms>NOS_CRIT_OFF_MS)c->off_ms=NOS_CRIT_OFF_MS;}
    else c->off_ms=0;
    if(c->off_ms>=NOS_CRIT_OFF_MS)c->level=NOS_CRIT_SHUTDOWN;
    else if(c->level==NOS_CRIT_NONE && c->warn_ms>=NOS_CRIT_WARN_MS)c->level=NOS_CRIT_WARN;
    return c->level;
}
/* Screens flagged SCR_KEEPAWAKE (compass, navigate, chat...) hold the display only while the user is
 * around (G99): released after 5 min without a touch or button, or after 1 min once the wrist is down
 * (screen normal near horizontal or pointing down). NOBACK rings and SCR_SELFCAP screens end themselves. */
#define NOS_KEEP_IDLE_MS 300000u
#define NOS_KEEP_DOWN_MS 60000u
static inline int power_wrist_down(int valid,float az_g){return valid && az_g<0.3f;}
static inline int power_keep_expired(unsigned user_idle_ms,int wrist_down)
{ return user_idle_ms>=NOS_KEEP_IDLE_MS || (wrist_down && user_idle_ms>=NOS_KEEP_DOWN_MS); }
/* Torch (G29): closes itself after 5 min without a touch; on low battery it dims and closes after 1 min,
 * and at the critical level (qualified <=5 %) it closes at once. low is the caller's power_low_update latch. */
enum { NOS_TORCH_OK, NOS_TORCH_DIM, NOS_TORCH_CLOSE };
#define NOS_TORCH_IDLE_MS 300000u
#define NOS_TORCH_LOW_IDLE_MS 60000u
static inline int power_torch_state(int low,const hal_power *p,unsigned idle_ms)
{
    if(p->external_power==0 && p->sample_valid && p->battery_valid && p->age_ms<=120000 &&
       p->battery_percent>=0 && p->battery_percent<=NOS_CRIT_WARN_PCT)return NOS_TORCH_CLOSE;
    if(idle_ms>=(low?NOS_TORCH_LOW_IDLE_MS:NOS_TORCH_IDLE_MS))return NOS_TORCH_CLOSE;
    return low?NOS_TORCH_DIM:NOS_TORCH_OK;
}
#endif
