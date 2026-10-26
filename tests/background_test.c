/* Run real observer/history logic with acquisition and worker faults. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../nos/background.c"
static uint32_t now_ms=10000,modules;
static int allocation_failure,imu_valid;
static svc_activity_t activity;
static hal_activity steps_cache;
static hal_power power_cache;
static sports_status journal;
static diag_log_status trace;
static hal_imu_record recording;
static workout_snapshot workout;
static void (*observer)(const sports_record *);
void *nos_mem_alloc(uint32_t n,const char *owner) {(void)owner;return allocation_failure?NULL:calloc(1,n);}
uint32_t hal_millis(void) {return now_ms;}
void sports_observe(void (*fn)(const sports_record *)) {observer=fn;}
void hal_activity_get(hal_activity *out) {*out=steps_cache;}
const svc_activity_t *svc_activity(void) {return &activity;}
void hal_power_get(hal_power *out) {*out=power_cache;}
void sports_get_status(sports_status *out) {*out=journal;}
void diag_log_get(diag_log_status *out) {*out=trace;}
void hal_imu_record_get(hal_imu_record *out) {*out=recording;}
const workout_snapshot *workout_get(void) {return &workout;}
uint32_t hal_ext_present(void) {return modules;}
int hal_imu_sample(hal_imu *out,uint32_t *ms) {*out=(hal_imu){.az=1};*ms=now_ms;return imu_valid;}
static background_snapshot snapshot(int id)
{background_snapshot out;assert(background_get(id,&out));return out;}
static void motion(uint32_t ms,float x)
{sports_record r={.type=SP_IMU,.uptime=ms,.v={x,0,1,0,2,0}};observer(&r);}
int main(void)
{
    allocation_failure=1;background_init();assert(!data&&!observer);
    assert(!background_get(0,NULL));allocation_failure=0;background_init();assert(data&&observer);
    assert(sizeof(job_data)*BG_COUNT<40000);
    assert(snapshot(BG_HEART).state==BG_ABSENT && snapshot(BG_MOTION).state==BG_WAITING);
    steps_cache.valid=1;activity.steps=100;activity.kcal=4;
    power_cache=(hal_power){.sample_valid=1,.battery_valid=0,.rail_mv=5020,.external_power=1};
    journal=(sports_status){.written=30,.available=1,.last_service_ms=now_ms};
    motion(now_ms,0);motion(now_ms+1,.5f);background_snapshot m=snapshot(BG_MOTION);
    assert(m.updates==2&&m.count==1&&m.state==BG_RUNNING&&m.v[0]>1);
    motion(now_ms-50,9);assert(snapshot(BG_MOTION).updates==2); /* out of order */
    now_ms+=1000;background_tick();
    assert(snapshot(BG_STEPS).v[0]==100&&snapshot(BG_CALORIES).v[0]==4);
    assert(snapshot(BG_BATTERY).valid==5); /* USB voltage isn't battery percentage */
    assert(snapshot(BG_JOURNAL).state==BG_RUNNING);
    uint32_t battery_updates=snapshot(BG_BATTERY).updates;
    for(int i=0;i<200;i++)background_tick();
    assert(snapshot(BG_BATTERY).updates==battery_updates);
    /* Stopped acquisition must age out even if the foreground keeps ticking. */
    now_ms+=5000;power_cache.age_ms=5000;background_tick();m=snapshot(BG_MOTION);
    assert(m.state==BG_STALE&&m.updates==2&&m.points[m.count-1].valid==0);
    assert(snapshot(BG_BATTERY).state==BG_STALE);
    motion(now_ms,.7f);assert(snapshot(BG_MOTION).state==BG_RUNNING);
    modules=EXT_ENV|EXT_HR;now_ms+=1000;background_tick();
    assert(snapshot(BG_CLIMATE).state==BG_WAITING&&snapshot(BG_HEART).state==BG_IDLE);
    sports_record env={.type=SP_ENV,.uptime=now_ms,.v={1,25,45,1005}};observer(&env);
    assert(snapshot(BG_CLIMATE).v[0]==25);
    sports_record hr={.type=SP_HEART,.uptime=now_ms,.v={0,0,0,.15f}};observer(&hr);
    assert(snapshot(BG_HEART).state==BG_WAITING&&snapshot(BG_HEART).valid==2);
    now_ms+=9000;background_tick();assert(snapshot(BG_HEART).state==BG_IDLE);
    assert(snapshot(BG_CLIMATE).state==BG_STALE);
    modules=0;now_ms+=1000;background_tick();assert(snapshot(BG_CLIMATE).state==BG_ABSENT);
    journal.errors=4;journal.dropped=17;journal.available=0;journal.last_service_ms=now_ms;
    now_ms+=1000;background_tick();
    assert(snapshot(BG_JOURNAL).state==BG_ERROR&&snapshot(BG_JOURNAL).dropped==17);
    journal.available=1;journal.last_service_ms=now_ms;now_ms+=1000;background_tick();
    assert(snapshot(BG_JOURNAL).state==BG_RUNNING);
    now_ms+=12000;background_tick();assert(snapshot(BG_JOURNAL).state==BG_STALE);
    recording.state=HAL_IMU_RECORDING;recording.samples=10;
    now_ms+=1000;background_tick();assert(snapshot(BG_IMU_REC).state==BG_RUNNING);
    now_ms+=5000;background_tick();assert(snapshot(BG_IMU_REC).state==BG_STALE);
    now_ms+=1000;background_tick();assert(snapshot(BG_IMU_REC).state==BG_STALE);
    recording.samples++;now_ms+=1000;background_tick();assert(snapshot(BG_IMU_REC).state==BG_RUNNING);
    recording.state=HAL_IMU_STOPPING;now_ms+=1000;background_tick();assert(snapshot(BG_IMU_REC).state==BG_SAVING);
    /* History runs without a screen, remains bounded, and crosses uptime wrap. */
    for(int i=0;i<180;i++){now_ms+=1000;motion(now_ms,(float)(i%5));background_tick();}
    m=snapshot(BG_MOTION);assert(m.count==BG_HISTORY&&m.updates==183);
    for(int i=1;i<m.count;i++)assert((int32_t)(m.points[i].ms-m.points[i-1].ms)>0);
    now_ms=UINT32_MAX-2000;memset(&data[BG_MOTION],0,sizeof data[BG_MOTION]);
    motion(now_ms,0);now_ms+=1000;motion(now_ms,.1f);now_ms+=2000;motion(now_ms,.2f);
    m=snapshot(BG_MOTION);assert(m.count==3&&m.updates==3);
    sports_record bad={.type=SP_IMU,.uptime=now_ms+1,.v={NAN,0,0}};observer(&bad);
    m=snapshot(BG_MOTION);assert(!(m.valid&1));
    puts("PASS background: acquisition provenance, unchanged values, stale/recovery, missing modules, leases, USB validity, writer errors, recorder stalls, bounded closed-app history and uptime wrap");
    free(data);return 0;
}
