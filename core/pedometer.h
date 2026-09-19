#ifndef WATCH_PEDOMETER_H
#define WATCH_PEDOMETER_H
#include "walk.h"
typedef struct {
    uint32_t version,day,steps;
    uint32_t reserved;
    uint64_t active_ms;
} pedometer_saved;
typedef struct { walk_state walk; pedometer_saved today; } pedometer;
void pedometer_init(pedometer *p,const pedometer_saved *saved);
/* day is local YYYYMMDD; zero defers rollover until the clock is trusted. */
void pedometer_update(pedometer *p,uint32_t day,uint32_t ms,const hal_imu *imu,int valid);
void pedometer_get(const pedometer *p,hal_activity *out);
#endif
