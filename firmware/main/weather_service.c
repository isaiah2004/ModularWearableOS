/* HTTPS runs on its own task. A bounded body and atomic snapshots keep the
 * renderer and 50 Hz IMU sampler independent of network latency. */
#include "hal.h"
#include "weather_parse.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { uint32_t version; float lat,lon; char city[32]; } weather_config;
static weather_config s_config;
static hal_weather s_weather={.status="Set weather location over USB"};
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_request;
static int64_t s_received_us;
static int location_valid(const weather_config *c) {
    return c->version==1 && isfinite(c->lat) && isfinite(c->lon) &&
        c->lat>=-90 && c->lat<=90 && c->lon>=-180 && c->lon<=180 &&
        c->city[0] && memchr(c->city,0,sizeof c->city);
}
int hal_weather_set_location(const char *city,float lat,float lon)
{
    if(!city || !city[0] || strlen(city)>=32) return 0;
    for(const unsigned char *p=(const unsigned char*)city;*p;p++) if(*p<32 || *p>=127) return 0;
    weather_config c={.version=1,.lat=lat,.lon=lon};
    snprintf(c.city,sizeof c.city,"%s",city);
    if(!location_valid(&c) || !hal_store_save_checked("weather_loc",&c,sizeof c)) return 0;
    portENTER_CRITICAL(&s_lock);
    s_config=c; s_request++;
    s_weather.valid=0; s_weather.generation++;
    snprintf(s_weather.city,sizeof s_weather.city,"%s",city);
    snprintf(s_weather.status,sizeof s_weather.status,"Waiting for Wi-Fi");
    portEXIT_CRITICAL(&s_lock);
    return 1;
}
int hal_weather_location(char *city,int n,float *lat,float *lon)
{
    portENTER_CRITICAL(&s_lock); weather_config c=s_config; portEXIT_CRITICAL(&s_lock);
    if(!location_valid(&c)) return 0;
    if(city && n>0) snprintf(city,n,"%s",c.city);
    if(lat) *lat=c.lat;
    if(lon) *lon=c.lon;
    return 1;
}
void hal_weather_refresh(void) {
    portENTER_CRITICAL(&s_lock); s_request++; portEXIT_CRITICAL(&s_lock);
}
void hal_weather_get(hal_weather *out)
{
    if(!out) return;
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    *out=s_weather;
    out->age_seconds=out->valid ? (uint32_t)((now-s_received_us)/1000000) : 0;
    portEXIT_CRITICAL(&s_lock);
    time_t epoch=time(NULL);
    if(out->valid && epoch>out->observed_epoch && (uint64_t)(epoch-out->observed_epoch)>out->age_seconds)
        out->age_seconds=(uint32_t)(epoch-out->observed_epoch);
    out->stale=out->valid && out->age_seconds>=3600;
}
typedef struct {char *body; size_t used; int overflow;} response;
#define BODY_MAX 16384
static esp_err_t http_event(esp_http_client_event_t *event)
{
    response *r=event->user_data;
    if(event->event_id==HTTP_EVENT_ON_DATA && event->data_len>0) {
        if(r->used+(size_t)event->data_len>=BODY_MAX) {r->overflow=1; return ESP_FAIL;}
        memcpy(r->body+r->used,event->data,event->data_len); r->used+=event->data_len;
        r->body[r->used]=0;
    }
    return ESP_OK;
}
static void weather_task(void *arg)
{
    (void)arg;
    uint32_t handled=UINT32_MAX;
    int64_t due=0,last_attempt=0;
    for(;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        portENTER_CRITICAL(&s_lock);
        weather_config c=s_config; uint32_t request=s_request;
        portEXIT_CRITICAL(&s_lock);
        int64_t now=esp_timer_get_time();
        if(!location_valid(&c) || hal_status(HAL_FEAT_WIFI)!=HAL_CONN_UP) continue;
        if((request==handled && now<due) || (last_attempt && now-last_attempt<10000000)) continue;
        /* Certificate date checks require the USB/RTC/NTP clock first. */
        if(!hal_epoch_seconds()) continue;
        last_attempt=now; handled=request;
        portENTER_CRITICAL(&s_lock);
        s_weather.fetching=1;
        snprintf(s_weather.status,sizeof s_weather.status,"Updating weather");
        portEXIT_CRITICAL(&s_lock);
        char url[640];
        snprintf(url,sizeof url,"https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
            "&current=temperature_2m,apparent_temperature,weather_code"
            "&hourly=temperature_2m,weather_code"
            "&daily=temperature_2m_max,temperature_2m_min,weather_code"
            "&forecast_hours=12&forecast_days=4&timezone=auto&timeformat=unixtime",c.lat,c.lon);
        response r={.body=calloc(1,BODY_MAX)};
        hal_weather result={0};
        int ok=0,status=0;
        esp_err_t err=ESP_ERR_NO_MEM;
        if(r.body) {
            esp_http_client_config_t cfg={.url=url,.timeout_ms=12000,
                .crt_bundle_attach=esp_crt_bundle_attach,.event_handler=http_event,.user_data=&r,
                .buffer_size=1024,.buffer_size_tx=1024};
            esp_http_client_handle_t http=esp_http_client_init(&cfg);
            if(http) {
                err=esp_http_client_perform(http);
                status=esp_http_client_get_status_code(http);
                ok=err==ESP_OK && status==200 && !r.overflow && weather_parse(r.body,&result);
                esp_http_client_cleanup(http);
            }
            free(r.body);
        }
        portENTER_CRITICAL(&s_lock);
        /* A location changed while HTTP was in flight: discard old-city data. */
        if(!memcmp(&c,&s_config,sizeof c)) {
            if(ok) {
                result.generation=s_weather.generation+1;
                snprintf(result.city,sizeof result.city,"%s",c.city);
                snprintf(result.status,sizeof result.status,"Open-Meteo");
                s_weather=result; s_received_us=esp_timer_get_time();
            } else snprintf(s_weather.status,sizeof s_weather.status,"Weather retry in 1 min");
        }
        s_weather.fetching=0;
        portEXIT_CRITICAL(&s_lock);
        if(ok) ESP_LOGI("weather","Updated %s: %d C, feels %d C, code %d",c.city,result.temp,result.feels,result.code);
        else ESP_LOGW("weather","Fetch failed: %s HTTP %d overflow=%d",esp_err_to_name(err),status,r.overflow);
        due=esp_timer_get_time()+(ok ? 1800LL : 60LL)*1000000;
    }
}
void weather_service_start(void)
{
    weather_config c={0};
    if(hal_store_load("weather_loc",&c,sizeof c) && location_valid(&c)) {
        s_config=c; snprintf(s_weather.city,sizeof s_weather.city,"%s",c.city);
        snprintf(s_weather.status,sizeof s_weather.status,"Waiting for Wi-Fi");
    }
    if(xTaskCreate(weather_task,"weather",8192,NULL,2,NULL)!=pdPASS)
        ESP_LOGE("weather","Could not start worker");
}
