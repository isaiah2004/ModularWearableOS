/* Public-API state-machine stress test. Link with nos/workout.c; no private
 * implementation inclusion, private symbols, or disk-layout assumptions. */
#include "workout.h"
#include "hal.h"
#include "hal_ext.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void diag_log_event(uint16_t code,int32_t a,int32_t b,int32_t c) { (void)code;(void)a;(void)b;(void)c; }

static uint32_t clock_ms, rng, seed, action_index;
static uint64_t uptime_ms;
static int card=1, locked, sensor_mode;
static hal_activity activity={1000,0,0,1};
static double latitude=12.9;
static uint32_t reads_hr, reads_gps, lock_count;

#define CHECK(c) do { if(!(c)) { fprintf(stderr,"FAIL seed=%u action=%u line=%d: %s\n",seed,action_index,__LINE__,#c);exit(1); } } while(0)
static uint32_t random_u32(void) { rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng; }
uint32_t hal_millis(void) { return clock_ms; }
uint64_t hal_epoch_seconds(void) { return 1792497600ULL+uptime_ms/1000u; }
void hal_activity_get(hal_activity *out) { *out=activity; }
void hal_storage_get(hal_storage *out) { memset(out,0,sizeof *out);out->mounted=card; }
int hal_sports_sd_lock(void) { CHECK(!locked);if(!card)return 0;locked=1;lock_count++;return 1; }
void hal_sports_sd_unlock(void) { CHECK(locked);locked=0; }
void hal_gps_enable(int on) { (void)on; }
void hal_hr_enable(int on) { (void)on; }
void hal_hr_request(unsigned owner,int on) { (void)owner;(void)on; }
int hal_hr_get(hal_hr *out)
{
    reads_hr++;memset(out,0,sizeof *out);
    out->bpm=100+(int)(seed%40);out->quality=.95f;out->on_wrist=1;out->sample_ms=clock_ms;
    if(sensor_mode==1)return 0;
    if(sensor_mode==2)out->quality=.1f;
    if(sensor_mode==3)out->on_wrist=0;
    if(sensor_mode==4)out->bpm=300;
    if(sensor_mode==5)out->sample_ms=clock_ms-10000u;
    if(sensor_mode==6)out->quality=NAN;
    return 1;
}
int hal_gps_get(hal_gps *out)
{
    reads_gps++;memset(out,0,sizeof *out);
    out->valid=out->fix3d=1;out->sats_used=9;out->lat=latitude;out->lon=77.6;
    out->hdop=1;out->speed_mps=2;
    if(sensor_mode==7)return 0;
    if(sensor_mode==8)out->valid=0;
    if(sensor_mode==9)out->lat=NAN;
    if(sensor_mode==10)out->lon=181;
    if(sensor_mode==11)out->hdop=9;
    if(sensor_mode==12)out->lat+=10;
    if(sensor_mode==13)out->age_ms=10000;
    if(sensor_mode==14)out->speed_mps=0;
    return 1;
}

typedef struct {
    unsigned state,sport,nlaps,n_history;
    uint32_t id,next_id,elapsed,total,lap_times[WORKOUT_LAPS_MAX];
    uint32_t history_ids[WORKOUT_HISTORY_MAX],evicted;
    int have_evicted;
} model_state;
static model_state model={.next_id=1};

static void stable_fields(const workout_snapshot *a,const workout_snapshot *b)
{
    CHECK(a->elapsed_ms==b->elapsed_ms);CHECK(a->distance_m==b->distance_m);
    CHECK(a->steps==b->steps);CHECK(a->hr_active_ms==b->hr_active_ms);
    CHECK(a->gps_active_ms==b->gps_active_ms);CHECK(a->nlaps==b->nlaps);
}
static void invariants(void)
{
    const workout_snapshot *s=workout_get();
    CHECK(s->state==model.state);CHECK(s->sport==model.sport);CHECK(s->id==model.id);
    CHECK(s->elapsed_ms==model.elapsed);CHECK(s->total_ms==model.total);
    CHECK(s->elapsed_ms<=s->total_ms);CHECK(s->nlaps==model.nlaps);
    CHECK(s->nlaps<=WORKOUT_LAPS_MAX);CHECK(s->hr_active_ms<=s->elapsed_ms);
    CHECK(s->gps_active_ms<=s->elapsed_ms);
    CHECK(isfinite(s->distance_m) && s->distance_m>=0);
    CHECK(isfinite(s->speed_mps) && s->speed_mps>=0 && s->speed_mps<=35);
    CHECK(isfinite(s->avg_speed_mps) && s->avg_speed_mps>=0 && s->avg_speed_mps<=35.01f);
    CHECK(isfinite(s->pace_s_km) && s->pace_s_km>=0);
    CHECK(isfinite(s->avg_pace_s_km) && s->avg_pace_s_km>=0);
    CHECK(isfinite(s->cadence_spm) && s->cadence_spm>=0 && s->cadence_spm<=500);
    CHECK(!s->hr_valid || (s->bpm>=35 && s->bpm<=230));
    CHECK(s->hr_valid || s->bpm==0);CHECK(s->peak_bpm<=230);CHECK(s->avg_bpm<=s->peak_bpm);
    CHECK(s->hr_active_ms || s->avg_bpm==0);
    CHECK(s->speed_valid || s->speed_mps==0);CHECK(s->pace_valid || s->pace_s_km==0);
    CHECK(s->distance_valid || s->distance_m==0);
    if(s->state!=WORKOUT_IDLE) {
        const workout_profile *p=workout_profile_get(s->sport);CHECK(p);
        if(!p->uses_steps)CHECK(s->steps==0 && !s->cadence_valid);
        if(!p->outdoor)CHECK(s->gps_status==WORKOUT_GPS_OFF && !s->distance_valid);
    }
    for(unsigned i=0;i<s->nlaps;i++) {
        CHECK(s->laps[i].elapsed_ms==model.lap_times[i]);
        CHECK(s->laps[i].elapsed_ms<=s->elapsed_ms);
        CHECK(isfinite(s->laps[i].distance_m) && s->laps[i].distance_m>=0 && s->laps[i].distance_m<=s->distance_m);
        if(i)CHECK(s->laps[i].elapsed_ms>s->laps[i-1].elapsed_ms && s->laps[i].distance_m>=s->laps[i-1].distance_m);
    }
    CHECK(workout_history_count()==(int)model.n_history);
    for(unsigned i=0;i<model.n_history;i++) {
        workout_snapshot record;CHECK(workout_history_get((int)i,&record));
        CHECK(record.state==WORKOUT_FINISHED && record.id==model.history_ids[i]);
        CHECK(record.nlaps<=WORKOUT_LAPS_MAX);
        for(unsigned j=0;j<i;j++)CHECK(model.history_ids[j]!=record.id);
    }
    workout_snapshot dummy;
    CHECK(!workout_history_get(-1,&dummy));CHECK(!workout_history_get((int)model.n_history,&dummy));
    CHECK(!workout_history_get(0,NULL));CHECK(!locked);
    /* Public getters must leave the complete published snapshot untouched. */
    workout_snapshot copy=*s;workout_storage_status status;workout_storage_get(&status);
    CHECK(memcmp(&copy,workout_get(),sizeof copy)==0);
}
static void service(void)
{
    workout_storage_service();workout_tick();
    if(workout_get()->save_state==WORKOUT_SAVE_SAVED)model.have_evicted=0;
    invariants();
}
static void advance(uint32_t dt)
{
    workout_snapshot before=*workout_get();
    if(model.state==WORKOUT_RUNNING)model.elapsed+=dt;
    if(model.state==WORKOUT_RUNNING || model.state==WORKOUT_PAUSED)model.total+=dt;
    clock_ms+=dt;uptime_ms+=dt;
    activity.steps+=(uint32_t)((uint64_t)dt*2u/1000u);
    latitude+=(double)dt*.002/111194.9266;
    workout_tick();
    if(model.state==WORKOUT_PAUSED || model.state==WORKOUT_FINISHED)stable_fields(&before,workout_get());
    if(model.state==WORKOUT_FINISHED)CHECK(before.total_ms==workout_get()->total_ms);
    if(model.state==WORKOUT_RUNNING && dt>=1000) {
        const workout_snapshot *s=workout_get();
        CHECK(s->distance_m>=before.distance_m && s->steps>=before.steps);
        CHECK(s->hr_active_ms>=before.hr_active_ms && s->gps_active_ms>=before.gps_active_ms);
        if(sensor_mode>=1 && sensor_mode<=6) {
            CHECK(!s->hr_valid && !s->bpm);CHECK(s->hr_active_ms==before.hr_active_ms);
        }
        if(workout_profile_get(model.sport)->outdoor && sensor_mode>=7 && sensor_mode<=13 && sensor_mode!=12) {
            CHECK(!s->speed_valid && !s->pace_valid);
            CHECK(s->distance_m==before.distance_m && s->gps_active_ms==before.gps_active_ms);
        }
        if(dt>=60000) {
            CHECK(s->distance_m==before.distance_m && s->steps==before.steps);
            CHECK(s->hr_active_ms==before.hr_active_ms && s->gps_active_ms==before.gps_active_ms);
        }
    }
    invariants();
}
static void record_lap(void)
{
    if(model.nlaps<WORKOUT_LAPS_MAX && model.elapsed &&
       (!model.nlaps || model.lap_times[model.nlaps-1]!=model.elapsed))
        model.lap_times[model.nlaps++]=model.elapsed;
}
static void action(unsigned kind,int sport)
{
    workout_snapshot before=*workout_get();int result=0,expected=0;
    switch(kind) {
    case 0:
        expected=sport>=0 && sport<WORKOUT_SPORT_COUNT &&
            (model.state==WORKOUT_IDLE || (model.state==WORKOUT_FINISHED && before.save_state==WORKOUT_SAVE_SAVED));
        result=workout_start(sport);
        if(result) {
            uint32_t next=model.next_id;model.state=WORKOUT_RUNNING;model.sport=(unsigned)sport;
            model.id=next;model.next_id=next+1;model.elapsed=model.total=model.nlaps=0;model.have_evicted=0;
        }
        break;
    case 1:
        expected=model.state==WORKOUT_RUNNING;result=workout_pause();if(result)model.state=WORKOUT_PAUSED;break;
    case 2:
        expected=model.state==WORKOUT_PAUSED;result=workout_resume();if(result)model.state=WORKOUT_RUNNING;break;
    case 3:
        expected=model.state==WORKOUT_RUNNING && model.nlaps<WORKOUT_LAPS_MAX && model.elapsed &&
            (!model.nlaps || model.lap_times[model.nlaps-1]!=model.elapsed);
        result=workout_lap_add();if(result)record_lap();break;
    case 4:
        expected=model.state==WORKOUT_RUNNING || model.state==WORKOUT_PAUSED;result=workout_finish();
        if(result) {
            record_lap();model.state=WORKOUT_FINISHED;
            if(model.n_history==WORKOUT_HISTORY_MAX) {
                model.evicted=model.history_ids[WORKOUT_HISTORY_MAX-1];model.have_evicted=1;
            } else {model.n_history++;model.have_evicted=0;}
            memmove(model.history_ids+1,model.history_ids,(model.n_history-1u)*sizeof model.history_ids[0]);
            model.history_ids[0]=model.id;
        }
        break;
    case 5:
        expected=model.state!=WORKOUT_IDLE;result=workout_discard();
        if(result) {
            if(model.state==WORKOUT_FINISHED && before.save_state!=WORKOUT_SAVE_SAVED) {
                model.n_history--;
                memmove(model.history_ids,model.history_ids+1,model.n_history*sizeof model.history_ids[0]);
                if(model.have_evicted)model.history_ids[model.n_history++]=model.evicted;
            }
            model.state=WORKOUT_IDLE;model.sport=model.id=model.elapsed=model.total=model.nlaps=0;model.have_evicted=0;
        }
        break;
    default:CHECK(0);
    }
    CHECK(result==expected);
    if(!result)CHECK(memcmp(&before,workout_get(),sizeof before)==0);
    invariants();
}
int main(int argc,char **argv)
{
    if(argc<3) {fprintf(stderr,"usage: workout_fuzz_test SEED NEW_STORE_DIR [ACTIONS]\n");return 2;}
    seed=(uint32_t)strtoul(argv[1],NULL,0);rng=seed?seed:1;
    unsigned count=argc>3?(unsigned)strtoul(argv[3],NULL,0):1000;
    clock_ms=seed%3u==0?UINT32_MAX-5000u:10000u;
    CHECK(workout_storage_set_directory(argv[2]));workout_init();service();
    static const uint32_t durations[]={0,1,17,250,999,1000,1001,3000,5001,60000};
    for(action_index=0;action_index<count;action_index++) {
        uint32_t draw=random_u32();
        if((draw&7u)==0)sensor_mode=(int)(random_u32()%15u);
        if((draw&63u)==0)activity.steps=0;
        if((draw&127u)==1)activity.steps+=1000000;
        activity.valid=(draw&31u)!=2;
        advance(durations[random_u32()%(sizeof durations/sizeof durations[0])]);
        switch(random_u32()%10u) {
        case 0:case 1:action(0,(int)(random_u32()%8u)-1);break;
        case 2:action(1,0);break;
        case 3:action(2,0);break;
        case 4:action(3,0);break;
        case 5:action(4,0);break;
        case 6:action(5,0);break;
        case 7:card=!card;break;
        case 8:workout_retry_save();service();break;
        case 9:service();break;
        }
    }
    /* Every seed must still be able to save a subsequent valid session. */
    card=1;sensor_mode=0;activity.valid=1;
    if(model.state==WORKOUT_RUNNING || model.state==WORKOUT_PAUSED)action(4,0);
    workout_retry_save();service();
    workout_storage_status status;workout_storage_get(&status);CHECK(!status.dirty && status.available);
    action(0,WORKOUT_GENERAL);advance(1000);action(4,0);workout_retry_save();service();
    CHECK(workout_get()->save_state==WORKOUT_SAVE_SAVED);
    printf("PASS seed=%u actions=%u clock_wrap=%u history=%u locks=%u hr_reads=%u gps_reads=%u\n",
           seed,count,seed%3u==0,model.n_history,lock_count,reads_hr,reads_gps);
    return 0;
}
