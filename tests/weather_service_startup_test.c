/* Compile/link the complete production weather_service.c. No task, network,
 * device or credential is accessed; SDK calls model ownership and faults. */
#include "hal.h"
#include "weather_service.h"
#include "weather_service_startup_mock.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t version;float lat,lon;char city[32]; } stored_location;
static stored_location saved={1,12.5f,34.5f,"Saved city"};
static int locked,loads,saves,attempts,live_workers,allocation_fault=-1;
static int missing,edit_during_load,edit_during_http,reenter_load,reenter_create;
static unsigned http_calls,errors,body_blocks;
static void *task_stack,*task_tcb;
static TaskFunction_t worker;
static esp_http_client_config_t http_config;
static int64_t now_us=20000000;
static jmp_buf stop_worker;
static int delay_calls;

void weather_test_enter(portMUX_TYPE *lock){assert(lock && !locked);locked=1;}
void weather_test_exit(portMUX_TYPE *lock){assert(lock && locked);locked=0;}
void weather_test_log(const char *level,const char *tag,const char *format,...)
{assert(!locked && !strcmp(tag,"weather") && format);if(!strcmp(level,"E"))errors++;}
int64_t esp_timer_get_time(void){return now_us;}
const char *esp_err_to_name(esp_err_t error){(void)error;return "mock";}
int hal_store_load(const char *key,void *out,int size)
{
    assert(!locked && !strcmp(key,"weather_loc") && size==(int)sizeof saved);loads++;
    if(reenter_load)assert(!weather_service_start() && !weather_service_ready());
    if(edit_during_load)assert(hal_weather_set_location("Selected city",21.0f,42.0f));
    if(missing)return 0;
    memcpy(out,&saved,sizeof saved);return 1;
}
int hal_store_save_checked(const char *key,const void *data,int size)
{assert(!locked && !strcmp(key,"weather_loc") && data && size==(int)sizeof saved);saves++;return 1;}
int xTaskCreate(TaskFunction_t entry,const char *name,unsigned bytes,void *arg,int priority,void *handle)
{
    assert(!locked && !strcmp(name,"weather") && bytes==8192 && !arg && priority==2 && !handle);
    assert(!live_workers && !task_stack && !task_tcb);attempts++;
    if(reenter_create)assert(!weather_service_start() && !weather_service_ready());
    /* Model either stack or TCB allocation failure from the ordinary internal
     * FreeRTOS creator, including release of any partially created task. */
    if(allocation_fault!=0)task_stack=malloc(bytes);
    if(task_stack && allocation_fault!=1)task_tcb=malloc(256);
    if(!task_stack || !task_tcb) {
        free(task_stack);free(task_tcb);task_stack=task_tcb=NULL;return 0;
    }
    worker=entry;live_workers++;return pdPASS;
}
void vTaskDelay(unsigned ticks)
{assert(!locked && ticks==1000);if(++delay_calls==2)longjmp(stop_worker,1);}
hal_conn hal_status(int feature){assert(feature==HAL_FEAT_WIFI);return HAL_CONN_UP;}
uint64_t hal_epoch_seconds(void){return 2000000000;}
void *nos_mem_alloc(uint32_t bytes,const char *owner)
{assert(!locked && bytes==16384 && !strcmp(owner,"weather_http"));body_blocks++;return malloc(bytes);}
void nos_mem_free(void *p){assert(p && body_blocks);body_blocks--;free(p);}
esp_err_t esp_crt_bundle_attach(void *config){(void)config;return ESP_OK;}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    assert(!locked && config->timeout_ms==12000 && config->buffer_size==1024 && config->buffer_size_tx==1024);
    assert(config->crt_bundle_attach==esp_crt_bundle_attach && config->event_handler && config->user_data);
    http_config=*config;return &http_config;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    assert(!locked && client==&http_config);http_calls++;
    if(edit_during_http)assert(hal_weather_set_location("Latest city",23.0f,45.0f));
    esp_http_client_event_t event={HTTP_EVENT_ON_DATA,2,"{}",http_config.user_data};
    assert(http_config.event_handler(&event)==ESP_OK);return ESP_OK;
}
int esp_http_client_get_status_code(esp_http_client_handle_t client){assert(client==&http_config);return 200;}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client){assert(client==&http_config);return ESP_OK;}
int weather_parse(const char *body,hal_weather *out)
{
    assert(!strcmp(body,"{}"));
    *out=(hal_weather){.valid=1,.temp=24,.feels=25,.code=2,.observed_epoch=2000000000,.utc_offset_seconds=20700};
    out->hours[0]=(weather_hour){2000003600,26,3};out->days[0]=(weather_day){1999987200,29,18,2};return 1;
}
static void run_one_fetch(void)
{
    assert(worker && live_workers==1);delay_calls=0;
    if(!setjmp(stop_worker))worker(NULL);
    assert(delay_calls==2 && !body_blocks && !locked);
}
static void assert_location(const char *name)
{
    char city[32];float lat,lon;assert(hal_weather_location(city,sizeof city,&lat,&lon));
    assert(!strcmp(city,name) && lat!=0 && lon!=0);
}
static void assert_repeated_ready_retains_cache(void)
{
    hal_weather before,after;hal_weather_get(&before);
    int previous_loads=loads,previous_attempts=attempts;unsigned previous_http=http_calls;
    for(int i=0;i<100;i++)assert(weather_service_ready() && weather_service_start());
    hal_weather_get(&after);
    assert(!memcmp(&before,&after,sizeof before) && loads==previous_loads && attempts==previous_attempts);
    assert(http_calls==previous_http && live_workers==1 && !locked);
}
int main(int argc,char **argv)
{
    assert(argc==2 && !weather_service_ready() && !loads && !attempts);
    reenter_load=reenter_create=1;
    if(!strcmp(argv[1],"allocation-recovery")) {
        allocation_fault=0;assert(!weather_service_start() && !weather_service_ready());
        assert(loads==1 && attempts==1 && !live_workers && !task_stack && !task_tcb);assert_location("Saved city");
        hal_weather first,after;hal_weather_get(&first);
        hal_weather_refresh();allocation_fault=1;strcpy(saved.city,"Stale durable city");
        assert(!weather_service_start() && !weather_service_ready());hal_weather_get(&after);
        assert(loads==1 && attempts==2 && !live_workers && !task_stack && !task_tcb && !memcmp(&first,&after,sizeof first));
        allocation_fault=-1;assert(weather_service_start() && weather_service_ready());
        assert(attempts==3 && errors==2 && loads==1);run_one_fetch();
        hal_weather_get(&after);assert(after.valid && after.generation==1 && after.temp==24 && !after.fetching);
        assert(after.observed_epoch==2000000000 && after.utc_offset_seconds==20700 && after.hours[0].epoch==2000003600);
        assert(after.days[0].epoch==1999987200 && !strcmp(after.city,"Saved city") && !strcmp(after.status,"Open-Meteo"));
    } else if(!strcmp(argv[1],"concurrent-selection")) {
        edit_during_load=1;allocation_fault=0;assert(!weather_service_start());
        assert_location("Selected city");assert(loads==1 && saves==1 && attempts==1);
        edit_during_load=0;allocation_fault=-1;assert(weather_service_start());
        assert(loads==1 && attempts==2);edit_during_http=1;run_one_fetch();
        hal_weather after;hal_weather_get(&after);
        assert(!after.valid && after.generation==2 && !after.fetching && !strcmp(after.city,"Latest city"));
        edit_during_http=0;now_us+=11000000;run_one_fetch();hal_weather_get(&after);
        assert(after.valid && after.generation==3 && !strcmp(after.city,"Latest city") && after.observed_epoch==2000000000);
    } else {
        assert(!strcmp(argv[1],"initial-read-recovery"));missing=1;allocation_fault=1;
        assert(!weather_service_start() && !weather_service_ready());
        assert(!hal_weather_location(NULL,0,NULL,NULL) && loads==1 && attempts==1);
        for(int i=0;i<100;i++)assert(!weather_service_ready());
        assert(loads==1 && attempts==1);missing=0;allocation_fault=-1;
        assert(weather_service_start() && weather_service_ready() && loads==2 && attempts==2);assert_location("Saved city");
    }
    assert_repeated_ready_retains_cache();assert(live_workers==1 && !body_blocks);
    free(task_stack);free(task_tcb);
    printf("PASS actual weather service %s: checked idempotent INTERNAL8KiB worker, guarded startup, retained config/forecast state\n",argv[1]);return 0;
}
