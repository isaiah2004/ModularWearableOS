#include "wake_service.h"
#include "hal.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int g_setting_tilt_wake;
static unsigned char stored[128];
static int stored_size,fail_save,saves,sensor_valid;
static uint32_t clock_ms,sample_ms;
static hal_imu sensor;
uint32_t hal_millis(void) { return clock_ms; }
int hal_imu_sample(hal_imu *out,uint32_t *ms)
{ *out=sensor;*ms=sample_ms;return sensor_valid; }
int hal_store_load(const char *key,void *out,int size)
{
    assert(!strcmp(key,"wake_cfg"));
    if(size!=stored_size)return 0;
    memcpy(out,stored,(size_t)size);return 1;
}
int hal_store_save_checked(const char *key,const void *data,int size)
{
    assert(!strcmp(key,"wake_cfg") && size<=(int)sizeof stored);
    if(fail_save)return 0;
    memcpy(stored,data,(size_t)size);stored_size=size;saves++;return 1;
}
static wake_info info(void) { wake_info i;wake_service_get(&i);return i; }
static void reset(int legacy)
{
    stored_size=fail_save=saves=0;clock_ms=sample_ms=1000;sensor_valid=1;
    sensor=(hal_imu){0,0,-1,4,3,-.5f};wake_service_init(legacy);
}
static void run(int ms,int fresh)
{
    for(int i=0;i<ms;i+=20) {
        clock_ms+=20;if(fresh)sample_ms=clock_ms;
        assert(wake_service_tick(clock_ms,0)==0);
    }
}
static void persistence(void)
{
    reset(1);assert(info().wrist==0 && info().pickup && info().turn && !info().learned);
    assert(saves==0); /* No flash writes on boot or normal sensor ticks. */
    run(2000,1);assert(saves==0);
    assert(wake_service_set("wrist",1));assert(wake_service_set("pickup",0));
    wake_service_init(1);assert(info().wrist==1 && !info().pickup && info().turn);
    assert(wake_service_set("turn",0));wake_service_init(1);
    assert(!info().pickup && !info().turn && !g_setting_tilt_wake);
    assert(!wake_service_set("wrist",2) && !wake_service_set("turn",-1));
    assert(!wake_service_set("unknown",0));assert(info().wrist==1);
    fail_save=1;assert(!wake_service_set("wrist",0));assert(info().wrist==1);
    assert(info().learn_result==-2);wake_service_init(1);assert(info().wrist==1);
    stored[0]^=1;wake_service_init(1);assert(info().wrist==0 && info().pickup && info().turn);
    reset(0);assert(!info().pickup && !info().turn && !g_setting_tilt_wake);
}
static void learning(void)
{
    reset(1);run(200,1);wake_service_learn();run(1000,1);
    assert(info().learned && !info().learning && info().learn_result==1);
    assert(saves==1);run(2000,1);assert(saves==1);
    assert(wake_service_set("wrist",1));assert(!info().learned);
    sensor=(hal_imu){.642788f,0,-.766044f,0,0,0};
    wake_service_learn();run(1200,1);assert(info().learned && info().learn_result==1);
    wake_service_init(1);run(1000,1);
    assert(info().wrist==1 && info().learned && info().view_dot>.999f);
    assert(wake_service_set("wrist",0));run(1000,1);
    assert(info().learned && fabsf(info().view_dot-.766044f)<.002f);
    /* Each wrist keeps its own reading pose across reboot and profile changes. */
    sensor=(hal_imu){0,0,-1,0,0,0};run(1000,1);assert(info().view_dot>.999f);
    assert(wake_service_set("wrist",1));run(1000,1);
    assert(fabsf(info().view_dot-.766044f)<.002f);
    fail_save=1;wake_service_learn();run(1000,1);
    assert(!info().learning && info().learn_result==-2);
    wake_service_init(1);run(1000,1);assert(fabsf(info().view_dot-.766044f)<.002f);
}
static void bad_learning(void)
{
    reset(1);wake_service_learn();run(11000,0);
    assert(!info().learning && info().learn_result==-1 && !saves);
    wake_service_learn();sensor_valid=0;run(11000,1);
    assert(!info().learning && info().learn_result==-1 && !saves);
    sensor_valid=1;sensor.gx=30;wake_service_learn();run(11000,1);
    assert(!info().learning && info().learn_result==-1 && !saves);
    sensor.gx=0;sensor.ax=NAN;wake_service_learn();run(11000,1);
    assert(!info().learning && info().learn_result==-1 && !saves);
    sensor=(hal_imu){0,0,-1,0,0,0};wake_service_learn();run(500,1);
    sensor_valid=0;run(200,1);sensor_valid=1;run(500,1);
    assert(info().learning && !saves);run(500,1);assert(info().learned && saves==1);
    wake_service_learn();assert(wake_service_set("wrist",1));
    assert(!info().learning && !info().learned);
    clock_ms=sample_ms=UINT32_MAX-500;wake_service_learn();run(1200,1);
    assert(info().learned && info().learn_result==1);
}
int main(void)
{
    persistence();learning();bad_learning();
    puts("PASS wake service: persistence, failed writes, independent wrist poses, stale/moving/invalid learning, timeout and clock wrap");
    return 0;
}
