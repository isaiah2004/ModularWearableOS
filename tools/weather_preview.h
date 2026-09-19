/* Explicit synthetic weather for host screenshots only. */
#ifndef WEATHER_PREVIEW_H
#define WEATHER_PREVIEW_H
#include "../core/weather.h"
#include <string.h>
void hal_weather_get(hal_weather *out) {
    *out=(hal_weather){.valid=1,.generation=1,.temp=24,.feels=26,.code=2,.utc_offset_seconds=19800,
        .observed_epoch=1789468200,.city="Weather demo",.status="Simulator data"};
    for(int i=0;i<WEATHER_HOURS;i++) out->hours[i]=(weather_hour){1789468200LL+i*7200,24+i,i==2?61:2};
    for(int i=0;i<WEATHER_DAYS;i++) out->days[i]=(weather_day){1789410600LL+i*86400,27+i,20+i,2};
}
void hal_weather_refresh(void) {}
int hal_weather_set_location(const char *city,float lat,float lon) {(void)city;(void)lat;(void)lon;return 0;}
int hal_weather_location(char *city,int n,float *lat,float *lon) {(void)city;(void)n;(void)lat;(void)lon;return 0;}
#endif
