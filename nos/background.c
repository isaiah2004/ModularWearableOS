#include "background.h"
#include "hal.h"
#include "hal_ext.h"
#include "svc.h"
#include "sports.h"
#include "diag_log.h"
#include "workout.h"
#include "psram.h"
#include <math.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE bg_lock=portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&bg_lock)
#define UNLOCK() portEXIT_CRITICAL(&bg_lock)
#else
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#endif

static const background_info jobs[BG_COUNT]={
    {"Motion sensor","MOTION SENSOR",{"Acceleration","Rotation","Z acceleration"},{"g","dps","g"},"Raw motion / always sampled",0,2000,0},
    {"Step tracking","STEP TRACKING",{"Steps today","Active minutes","Walking"},{"steps","min","0 / 1"},"Pedometer / fresh motion required",0,3000,0},
    {"Calorie estimate","CALORIE ESTIMATE",{"Activity energy","Steps today","Active minutes"},{"kcal","steps","min"},"Step-based estimate / 0.04 kcal per step",0,3000,0},
    {"Battery sampling","BATTERY SAMPLING",{"Supply voltage","Battery level","External power"},{"mV","%","0 / 1"},"Supply voltage includes USB power",0,4000,0},
    {"SD sensor journal","SD SENSOR JOURNAL",{"Pending records","Written records","Write errors"},{"records","records","errors"},"Counts from the actual SD writer",0,10000,0},
    {"System trace","SYSTEM TRACE",{"Pending events","Written events","Write errors"},{"events","events","errors"},"Checked diagnostic log / SD",0,0,0},
    {"IMU recording","IMU RECORDING",{"Recorded samples","Elapsed time","Missed samples"},{"samples","s","samples"},"Raw recording starts in the IMU app",0,3000,1},
    {"Sports session","SPORTS SESSION",{"Active time","Session steps","Energy estimate"},{"s","steps","kcal"},"Session tracking starts in Sports",0,3000,1},
    {"Heart sampling","HEART SAMPLING",{"Heart rate","Signal quality","Oxygen estimate"},{"bpm","%","%"},"Periodic sampling / streams on demand",EXT_HR,4000,1},
    {"GPS receiver","GPS RECEIVER",{"Satellites used","Speed","HDOP"},{"sat","m/s","HDOP"},"Runs while requested / waiting means no fix",EXT_GPS,5000,1},
    {"Climate sensor","CLIMATE SENSOR",{"Temperature","Humidity","Pressure"},{"C","%","hPa"},"Optional climate module / acquisition events",EXT_ENV,7000,0},
    {"Light sensor","LIGHT SENSOR",{"Illuminance","Proximity",""},{"lux","0-255",""},"Optional light module / acquisition events",EXT_LIGHT,3000,0},
    {"Compass sensor","COMPASS SENSOR",{"Heading","Magnetic field","Calibrated"},{"deg","uT","0 / 1"},"Optional compass / streams on demand",EXT_MAG,3000,1}
};
typedef struct {
    background_point points[BG_HISTORY];
    background_event events[BG_EVENTS];
    uint32_t last_ms, updates, errors, dropped;
    float v[3], axes[6];
    unsigned head,count,event_head,event_count;
    uint8_t state,valid,have_sample,have_state;
} job_data;
static job_data *data;
static uint32_t last_tick;
static int ticked;
static int recorder_state=-1;
const background_info *background_job(int id) {return id>=0&&id<BG_COUNT?&jobs[id]:0;}
const char *background_state_name(int state)
{
    static const char *const names[]={"WAITING","RUNNING","IDLE","NO MODULE","STALE","ERROR","PAUSED","SAVING"};
    return state>=0&&state<8?names[state]:"UNKNOWN";
}
/* Caller holds the lock. Store at most one point per second, preserving actual
 * timestamps and invalid gaps. A static value remains a genuine observation. */
static void point(job_data *d,uint32_t ms,int valid)
{
    unsigned k=(d->head+BG_HISTORY-1)%BG_HISTORY;
    if(d->count && (int32_t)(ms-d->points[k].ms)<0)return;
    if(!d->count || d->points[k].ms/1000u!=ms/1000u) {
        k=d->head;d->head=(d->head+1)%BG_HISTORY;
        if(d->count<BG_HISTORY)d->count++;
    }
    d->points[k]=(background_point){.ms=ms,.valid=(uint8_t)valid,.state=d->state};
    memcpy(d->points[k].v,d->v,sizeof d->v);
}
static void state(job_data *d,int st,uint32_t now)
{
    unsigned previous=(d->event_head+BG_EVENTS-1)%BG_EVENTS;
    if(d->have_state && d->state==st && d->events[previous].errors==d->errors && d->events[previous].dropped==d->dropped)return;
    d->state=(uint8_t)st;d->have_state=1;
    d->events[d->event_head]=(background_event){.ms=now,.updates=d->updates,.errors=d->errors,.dropped=d->dropped,.state=(uint8_t)st};
    d->event_head=(d->event_head+1)%BG_EVENTS;
    if(d->event_count<BG_EVENTS)d->event_count++;
}
static void publish(int id,uint32_t ms,const float v[3],int valid,int st,int unique)
{
    if(!data)return;
    for(int i=0;i<3;i++)if(!isfinite(v[i]))valid&=~(1<<i);
    if(!(valid&1) && st==BG_RUNNING)st=BG_WAITING;
    LOCK();job_data *d=&data[id];
    if(d->have_sample && ((int32_t)(ms-d->last_ms)<0 || (unique&&ms==d->last_ms))) {UNLOCK();return;}
    d->last_ms=ms;d->have_sample=1;
    if(d->updates<UINT32_MAX)d->updates++;
    memcpy(d->v,v,sizeof d->v);d->valid=(uint8_t)valid;
    state(d,st,ms);point(d,ms,valid);UNLOCK();
}
static void counters(int id,uint32_t errors,uint32_t dropped)
{ LOCK();data[id].errors=errors;data[id].dropped=dropped;UNLOCK(); }
static void observe(const sports_record *r)
{
    int id=-1,valid=7,st=BG_RUNNING;float v[3]={0};
    switch(r->type) {
    case SP_IMU:
        id=BG_MOTION;v[0]=sqrtf(r->v[0]*r->v[0]+r->v[1]*r->v[1]+r->v[2]*r->v[2]);
        v[1]=sqrtf(r->v[3]*r->v[3]+r->v[4]*r->v[4]+r->v[5]*r->v[5]);v[2]=r->v[2];break;
    case SP_ENV: id=BG_CLIMATE;memcpy(v,r->v+1,sizeof v);if(!r->v[0])valid=0;break;
    case SP_LIGHT: id=BG_LIGHT;v[0]=r->v[1];v[1]=r->v[2];valid=r->v[0]?3:0;break;
    case SP_MAG: id=BG_COMPASS;v[0]=r->v[1];v[1]=r->v[2];v[2]=r->v[3];if(!r->v[0])valid=0;break;
    case SP_HEART:
        id=BG_HEART;v[0]=r->v[1];v[1]=r->v[3]*100;v[2]=r->v[2];valid=2;
        if(r->v[0]>0&&v[0]>0)valid|=1;
        if(r->v[0]>0&&v[2]>0)valid|=4;
        if(!(valid&1))st=BG_WAITING;
        break;
    case SP_GPS:
        id=BG_GPS;v[0]=r->v[6];v[1]=r->v[4];v[2]=r->v[5];
        if(!r->v[0]) {valid=1;st=BG_WAITING;}break;
    default:return;
    }
    if(!valid)st=BG_WAITING;
    publish(id,r->uptime,v,valid,st,0);
    if(id==BG_MOTION) {
        LOCK();if(data && data[id].last_ms==r->uptime)memcpy(data[id].axes,r->v,sizeof data[id].axes);UNLOCK();
    }
}
void background_init(void)
{
    if(data)return;
    job_data *allocated=nos_mem_alloc(sizeof(job_data)*BG_COUNT,"background.history");
    if(!allocated)return;
    memset(allocated,0,sizeof(job_data)*BG_COUNT);
    LOCK();data=allocated;UNLOCK();
    sports_observe(observe);
    background_tick();
}
static void sample_services(uint32_t now)
{
    hal_activity a={0};hal_activity_get(&a);
    const svc_activity_t *act=svc_activity();
    float steps[]={ (float)act->steps,(float)act->active_min,(float)a.walking };
    float energy[]={ act->kcal,(float)act->steps,(float)act->active_min };
    publish(BG_STEPS,now,steps,a.valid?7:0,a.valid?BG_RUNNING:BG_WAITING,1);
    publish(BG_CALORIES,now,energy,a.valid?7:0,a.valid?BG_RUNNING:BG_WAITING,1);
    hal_power p={0};hal_power_get(&p);
    float power[]={ (float)p.rail_mv,(float)p.battery_percent,(float)p.external_power };
    if(p.sample_valid)publish(BG_BATTERY,now-p.age_ms,power,1|(p.battery_valid?2:0)|(p.external_power>=0?4:0),BG_RUNNING,1);
    sports_status sd;sports_get_status(&sd);
    float journal[]={ (float)sd.queued,(float)sd.written,(float)sd.errors };
    counters(BG_JOURNAL,sd.errors,sd.dropped);
    if(sd.last_service_ms)publish(BG_JOURNAL,sd.last_service_ms,journal,7,sd.available?BG_RUNNING:sd.errors?BG_ERROR:BG_WAITING,1);
    diag_log_status log;diag_log_get(&log);
    float trace[]={ (float)log.queued,(float)log.written,(float)log.errors };
    counters(BG_TRACE,log.errors,log.dropped);
    publish(BG_TRACE,now,trace,7,!log.ready?BG_WAITING:!log.available?(log.errors?BG_ERROR:BG_WAITING):log.queued?BG_SAVING:BG_IDLE,1);
    hal_imu_record rec;hal_imu_record_get(&rec);
    float recording[]={ (float)rec.samples,rec.elapsed_ms/1000.0f,(float)rec.missed };
    counters(BG_IMU_REC,rec.errors,rec.dropped);
    int st=rec.state==HAL_IMU_ERROR?BG_ERROR:rec.state==HAL_IMU_IDLE?BG_IDLE:
        rec.state==HAL_IMU_STOPPING?BG_SAVING:rec.state==HAL_IMU_STARTING?BG_WAITING:BG_RUNNING;
    LOCK();int changed=!data[BG_IMU_REC].have_sample||data[BG_IMU_REC].v[0]!=(float)rec.samples||recorder_state!=rec.state;UNLOCK();
    recorder_state=rec.state;
    if(changed || st!=BG_RUNNING)publish(BG_IMU_REC,now,recording,st==BG_IDLE?0:7,st,1);
    const workout_snapshot *w=workout_get();
    float session[]={w->elapsed_ms/1000.0f,(float)w->steps,w->energy_kcal_est};
    st=w->state==WORKOUT_RUNNING?BG_RUNNING:w->state==WORKOUT_PAUSED?BG_PAUSED:
        w->save_state==WORKOUT_SAVE_PENDING?BG_SAVING:w->save_state==WORKOUT_SAVE_FAILED?BG_ERROR:BG_IDLE;
    publish(BG_WORKOUT,now,session,st==BG_IDLE?0:7,st,1);
}
void background_tick(void)
{
    uint32_t now=hal_millis();
    if(!data || (ticked && now-last_tick<1000u))return;
    ticked=1;last_tick=now;sample_services(now);
#ifndef ESP_PLATFORM
    /* The simulator synthesizes motion; device samples arrive via the producer. */
    hal_imu imu;uint32_t ms;
    if(hal_imu_sample(&imu,&ms)) {sports_record r={.type=SP_IMU,.uptime=ms};memcpy(r.v,&imu,sizeof imu);observe(&r);}
#endif
    uint32_t modules=hal_ext_present();
    LOCK();
    for(int id=0;id<BG_COUNT;id++) {
        job_data *d=&data[id];int st=d->state,valid=d->valid;
        if(jobs[id].module && !(modules&jobs[id].module)) {st=BG_ABSENT;valid=0;}
        else if(!d->have_sample) {st=jobs[id].on_demand?BG_IDLE:BG_WAITING;valid=0;}
        else if(jobs[id].stale_ms && (int32_t)(now-d->last_ms)>(int32_t)jobs[id].stale_ms &&
                (st==BG_RUNNING || st==BG_WAITING || st==BG_STALE || st==BG_ABSENT || (jobs[id].module&&st==BG_IDLE))) {
            st=jobs[id].on_demand && jobs[id].module?BG_IDLE:BG_STALE;valid=0;
        }
        if(st==BG_ABSENT && (modules&jobs[id].module))st=BG_WAITING;
        d->valid=(uint8_t)valid;
        state(d,st,now);
        if(!valid)point(d,now,0);
    }
    UNLOCK();
}
int background_get(int id,background_snapshot *out)
{
    if(!data || !out || id<0 || id>=BG_COUNT)return 0;
    memset(out,0,sizeof *out);out->now=hal_millis();
    LOCK();const job_data *d=&data[id];
    out->last_ms=d->last_ms;out->updates=d->updates;out->errors=d->errors;out->dropped=d->dropped;
    out->state=d->state;out->valid=d->valid;out->have_sample=d->have_sample;
    memcpy(out->v,d->v,sizeof out->v);memcpy(out->axes,d->axes,sizeof out->axes);
    for(unsigned i=0;i<d->count;i++) {
        background_point p=d->points[(d->head+BG_HISTORY-d->count+i)%BG_HISTORY];
        if((int32_t)(out->now-p.ms)<(int32_t)BG_WINDOW_MS)out->points[out->count++]=p;
    }
    out->event_count=(int)d->event_count;
    for(unsigned i=0;i<d->event_count;i++)out->events[i]=d->events[(d->event_head+BG_EVENTS-1-i)%BG_EVENTS];
    UNLOCK();return 1;
}
int background_summary(int states[BG_COUNT])
{
    if(!data)return 0;
    LOCK();for(int i=0;i<BG_COUNT;i++)states[i]=data[i].state;UNLOCK();return 1;
}
int background_status_get(int id,background_status *out)
{
    if(!data || !out || id<0 || id>=BG_COUNT)return 0;
    LOCK();const job_data *d=&data[id];
    *out=(background_status){.last_ms=d->last_ms,.updates=d->updates,.errors=d->errors,.dropped=d->dropped,.state=d->state,.valid=d->valid};
    memcpy(out->v,d->v,sizeof out->v);UNLOCK();return 1;
}
