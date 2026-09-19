#include "pedometer.h"
#include <string.h>
void pedometer_init(pedometer *p,const pedometer_saved *saved)
{
    memset(p,0,sizeof *p);
    p->today.version=1;
    if(saved && saved->version==1 && (!saved->day || (saved->day>=20200101 && saved->day<=20991231))) p->today=*saved;
}
void pedometer_update(pedometer *p,uint32_t day,uint32_t ms,const hal_imu *imu,int valid)
{
    if(day && p->today.day && day!=p->today.day) {
        p->today=(pedometer_saved){.version=1,.day=day};
        walk_init(&p->walk); /* Do not back-credit yesterday's candidate peaks. */
    }
    if(day) p->today.day=day;
    uint32_t old=p->walk.steps;
    uint64_t active=p->walk.active_ms;
    walk_update(&p->walk,ms,imu,valid);
    uint32_t delta=p->walk.steps-old;
    p->today.steps=UINT32_MAX-p->today.steps<delta ? UINT32_MAX : p->today.steps+delta;
    p->today.active_ms+=p->walk.active_ms-active;
}
void pedometer_get(const pedometer *p,hal_activity *out)
{
    uint64_t seconds=p->today.active_ms/1000;
    *out=(hal_activity){.steps=p->today.steps,.active_seconds=seconds>UINT32_MAX ? UINT32_MAX : (uint32_t)seconds,
        .walking=p->walk.walking,.valid=p->walk.valid};
}
