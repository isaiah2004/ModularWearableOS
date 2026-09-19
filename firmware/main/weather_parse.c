#include "weather_parse.h"
#include "cJSON.h"
#include <math.h>
#include <string.h>

static int number(const cJSON *obj,const char *key,int index,double lo,double hi,double *out)
{
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(obj,key);
    if(index>=0) v=cJSON_GetArrayItem(v,index);
    if(!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble<lo || v->valuedouble>hi) return 0;
    *out=v->valuedouble; return 1;
}
int weather_parse(const char *json,hal_weather *out)
{
    if(!json || !out) return 0;
    const char *end=NULL;
    cJSON *root=cJSON_ParseWithOpts(json,&end,1);
    if(!root) return 0;
    hal_weather w={0};
    const cJSON *cur=cJSON_GetObjectItemCaseSensitive(root,"current");
    const cJSON *hour=cJSON_GetObjectItemCaseSensitive(root,"hourly");
    const cJSON *day=cJSON_GetObjectItemCaseSensitive(root,"daily");
    double t,f,c,e,off;
    int ok=number(root,"utc_offset_seconds",-1,-43200,50400,&off) &&
        number(cur,"temperature_2m",-1,-100,70,&t) &&
        number(cur,"apparent_temperature",-1,-120,100,&f) &&
        number(cur,"weather_code",-1,0,99,&c) &&
        number(cur,"time",-1,1577836800,4102444800.0,&e);
    if(ok) {
        w.temp=(int)lround(t); w.feels=(int)lround(f); w.code=(int)c;
        w.observed_epoch=(int64_t)e; w.utc_offset_seconds=(int)off;
    }
    /* The endpoint may include a preceding hour (notably half-hour zones).
     * Anchor to the observation rather than assuming array index zero. */
    int first=0;
    for(;ok && first<24;first++) {
        ok=number(hour,"time",first,1577836800,4102444800.0,&e);
        if(ok && e>=w.observed_epoch-3600) break;
    }
    for(int i=0;ok && i<WEATHER_HOURS;i++) {
        int index=first+i*2;
        ok=number(hour,"time",index,1577836800,4102444800.0,&e) &&
           number(hour,"temperature_2m",index,-100,70,&t) &&
           number(hour,"weather_code",index,0,99,&c);
        if(ok) {
            w.hours[i]=(weather_hour){(int64_t)e,(int)lround(t),(int)c};
            if((i && w.hours[i].epoch<=w.hours[i-1].epoch) ||
               e<w.observed_epoch-3600 || e>w.observed_epoch+43200) ok=0;
        }
    }
    for(int i=0;ok && i<WEATHER_DAYS;i++) {
        ok=number(day,"time",i,1577836800,4102444800.0,&e) &&
           number(day,"temperature_2m_max",i,-100,70,&t) &&
           number(day,"temperature_2m_min",i,-100,70,&f) &&
           number(day,"weather_code",i,0,99,&c) && t>=f;
        if(ok) {
            w.days[i]=(weather_day){(int64_t)e,(int)lround(t),(int)lround(f),(int)c};
            if(i && w.days[i].epoch<=w.days[i-1].epoch) ok=0;
        }
    }
    cJSON_Delete(root);
    if(ok) {w.valid=1; *out=w;}
    return ok;
}
