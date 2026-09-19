#include "weather_parse.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void rejected(cJSON *root) {
    char *json=cJSON_PrintUnformatted(root);
    hal_weather w,original;
    memset(&w,0x5a,sizeof w);original=w;
    assert(!weather_parse(json,&w));
    assert(!memcmp(&w,&original,sizeof w));
    free(json);
}
int main(int argc,char **argv) {
    assert(argc==2);
    FILE *f=fopen(argv[1],"rb");assert(f);
    char body[16384]={0};size_t n=fread(body,1,sizeof body-1,f);fclose(f);assert(n>0);
    hal_weather w;assert(weather_parse(body,&w));
    assert(w.valid && w.utc_offset_seconds==19800);
    assert(w.temp==31 && w.feels==34 && w.code==53);
    assert(w.hours[4].epoch>w.hours[0].epoch && w.days[3].epoch>w.days[0].epoch);
    cJSON *root=cJSON_Parse(body);assert(root);
    cJSON *cur=cJSON_GetObjectItemCaseSensitive(root,"current");
    cJSON_ReplaceItemInObject(cur,"temperature_2m",cJSON_CreateNull());rejected(root);
    cJSON_ReplaceItemInObject(cur,"temperature_2m",cJSON_CreateNumber(-12.6));
    char *json=cJSON_PrintUnformatted(root);assert(weather_parse(json,&w) && w.temp==-13);free(json);
    cJSON_ReplaceItemInObject(cur,"temperature_2m",cJSON_CreateNumber(500));rejected(root);
    cJSON_Delete(root);root=cJSON_Parse(body);
    cJSON *hour=cJSON_GetObjectItemCaseSensitive(root,"hourly");
    cJSON_DeleteItemFromArray(cJSON_GetObjectItemCaseSensitive(hour,"time"),0);
    /* Removing several entries must never turn missing forecasts into zero C. */
    for(int i=0;i<4;i++)cJSON_DeleteItemFromArray(cJSON_GetObjectItemCaseSensitive(hour,"time"),0);
    rejected(root);cJSON_Delete(root);
    root=cJSON_Parse(body);cJSON_DeleteItemFromObject(root,"daily");rejected(root);cJSON_Delete(root);
    assert(!weather_parse("{",&w));
    assert(weather_condition(0)==0 && weather_condition(2)==1 && weather_condition(45)==2);
    assert(weather_condition(61)==3 && weather_condition(75)==4 && weather_condition(95)==5);
    puts("weather: live schema, units, forecasts, null/range/truncation rejection, atomic publication, WMO categories passed");
}
