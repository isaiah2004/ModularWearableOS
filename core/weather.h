#ifndef WATCH_WEATHER_H
#define WATCH_WEATHER_H
#include <stdint.h>
#define WEATHER_HOURS 5
#define WEATHER_DAYS 4
typedef struct { int64_t epoch; int temp, code; } weather_hour;
typedef struct { int64_t epoch; int hi, lo, code; } weather_day;
typedef struct {
    int valid, stale, fetching;
    uint32_t generation, age_seconds;
    int temp, feels, code, utc_offset_seconds;
    int64_t observed_epoch;
    char city[32], status[48];
    weather_hour hours[WEATHER_HOURS];
    weather_day days[WEATHER_DAYS];
} hal_weather;
/* Cached snapshots only; rendering never performs network I/O. */
void hal_weather_get(hal_weather *out);
void hal_weather_refresh(void);
int hal_weather_set_location(const char *city, float lat, float lon);
int hal_weather_location(char *city, int n, float *lat, float *lon);
/* WMO codes: clear, partly cloudy, cloudy/fog, rain, snow, thunderstorm. */
static inline int weather_condition(int code) {
    if(code==0) return 0;
    if(code==1 || code==2) return 1;
    if(code==3 || code==45 || code==48) return 2;
    if((code>=71 && code<=77) || code==85 || code==86) return 4;
    if(code>=95) return 5;
    return 3;
}
#endif
