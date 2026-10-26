/* GCC: gcc -std=c99 -Wall -Wextra -Werror -Icore -Inos tests/workout_test.c
 *           -lm -o build/workout_test.exe
 * Run in an isolated directory. Optional --write-recovery/--read-recovery DIR
 * modes verify checkpoints across separate OS processes. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#include <io.h>
static int checked_commit(int fd);
#define _commit checked_commit
#else
#include <unistd.h>
static int checked_commit(int fd);
#define fsync checked_commit
#endif
static FILE *checked_fopen(const char *path,const char *mode);
#define fopen checked_fopen
#include "../nos/workout_motion.c"
#include "../nos/workout.c"
void diag_log_event(uint16_t code,int32_t a,int32_t b,int32_t c) { (void)code;(void)a;(void)b;(void)c; }
#undef fopen
#ifdef _WIN32
#undef _commit
#else
#undef fsync
#endif

static uint32_t test_ms=10000;
static hal_activity input_activity={1000,0,0,1};
static hal_gps input_gps;
static hal_hr input_hr;
static int gps_present=1,hr_present=1,card_present=1,sd_locked,lock_allowed=1;
static int commit_failure,commit_corruption,gps_reads,hr_reads;
static int readback_failure,readback_pending;
static int unreadable_bank=-1,write_opens;
static unsigned hr_owners;
uint32_t hal_millis(void) { return test_ms; }
uint64_t hal_epoch_seconds(void) { return 1792497600ULL+test_ms/1000u; }
void hal_activity_get(hal_activity *out) { *out=input_activity; }
void hal_storage_get(hal_storage *out) { memset(out,0,sizeof *out);out->mounted=card_present; }
int hal_sports_sd_lock(void) { assert(!sd_locked);if(!lock_allowed)return 0;sd_locked=1;return 1; }
void hal_sports_sd_unlock(void) { assert(sd_locked);sd_locked=0; }
void hal_gps_enable(int on) { (void)on; }
void hal_hr_enable(int on) { (void)on; }
void hal_hr_request(unsigned owner,int on) {if(on)hr_owners|=owner;else hr_owners&=~owner;}
int hal_gps_get(hal_gps *out) { *out=input_gps;gps_reads++;return gps_present; }
int hal_hr_get(hal_hr *out) { *out=input_hr;hr_reads++;return hr_present; }
static FILE *checked_fopen(const char *path,const char *mode)
{
    if(!strcmp(mode,"wb"))write_opens++;
    if(unreadable_bank>=0 && !strcmp(mode,"rb")) {
        char needle[32];snprintf(needle,sizeof needle,"workouts.%d.bin",unreadable_bank);
        if(strstr(path,needle)) {errno=EIO;return NULL;}
    }
    if(readback_pending && !strcmp(mode,"rb")) {
        readback_pending=0;errno=EIO;return NULL;
    }
    return fopen(path,mode);
}
static int checked_commit(int fd)
{
    assert(sd_locked);
    if(commit_failure)return -1;
    if(readback_failure)readback_pending=1;
#ifdef _WIN32
    if(commit_corruption) {
        assert(_lseek(fd,0,SEEK_SET)==0);assert(_write(fd,"bad!",4)==4);
    }
    return _commit(fd);
#else
    if(commit_corruption) {
        assert(lseek(fd,0,SEEK_SET)==0);assert(write(fd,"bad!",4)==4);
    }
    return fsync(fd);
#endif
}
static void valid_inputs(void)
{
    input_gps=(hal_gps){0};input_gps.valid=1;input_gps.fix3d=1;
    input_gps.sats_used=9;input_gps.lat=12.9;input_gps.lon=77.6;
    input_gps.hdop=1.1f;input_gps.speed_mps=2;
    input_hr=(hal_hr){0};input_hr.bpm=100;input_hr.quality=.95f;
    input_hr.on_wrist=1;input_hr.sample_ms=test_ms;
}
static void clock_tick(uint32_t dt,int steps,float metres)
{
    test_ms+=dt;input_activity.steps+=(uint32_t)steps;
    input_gps.lat+=(double)metres/111194.9266;input_hr.sample_ms=test_ms;
    workout_tick();
}
static void save(void)
{ workout_storage_service();workout_tick(); }
/* Reset volatile state only; real checkpoint files remain untouched. */
static void reboot(void)
{
    memset(&current,0,sizeof current);memset(history,0,sizeof history);
    memset(&evicted,0,sizeof evicted);have_evicted=0;
    memset(&storage,0,sizeof storage);memset(&host_writer,0,sizeof host_writer);
    history_count=0;next_id=1;revision=saved_revision=ack_revision=0;
    last_attempt_ms=last_checkpoint_ms=0;
    initialized=load_pending=loaded=force_write=attempted=ack_success=ack_pending=0;
    failed_pristine_slot=-1;sample_started=0;prepared_sport=-1;hr_integral=0;
    clear_anchors();workout_init();save();
}
static void new_area(const char *name)
{
    char path[224];snprintf(path,sizeof path,"build/workout-tests/%s",name);
    initialized=0;assert(workout_storage_set_directory(path));
    /* Tests may be rerun without preserving their own fixture files. */
    char file[256];for(int i=0;i<2;i++) {
        snprintf(file,sizeof file,"%s/workouts.%d.bin",path,i);remove(file);
    }
    card_present=lock_allowed=1;commit_failure=commit_corruption=0;
    readback_failure=readback_pending=0;
    unreadable_bank=-1;hr_owners=0;
    gps_present=hr_present=1;valid_inputs();reboot();
    assert(storage.ready && storage.available && !storage.corrupt);
}
static void close_saved(void)
{
    assert(workout_finish());save();assert(current.save_state==WORKOUT_SAVE_SAVED);
    assert(workout_discard());save();assert(current.state==WORKOUT_IDLE);
}
static void timing_and_steps(void)
{
    new_area("timing");assert(workout_start(WORKOUT_TREADMILL));
    assert(!workout_start(WORKOUT_WALK));assert(!workout_start(-1));
    clock_tick(1000,2,0);clock_tick(1000,2,0);assert(current.elapsed_ms==2000);
    assert(current.steps==4 && !current.distance_valid && current.distance_m==0);
    assert(workout_pause());clock_tick(7000,80,100);
    assert(current.elapsed_ms==2000 && current.total_ms==9000 && current.steps==4);
    assert(!workout_lap_add());assert(workout_resume());clock_tick(1000,2,0);
    assert(current.elapsed_ms==3000 && current.total_ms==10000 && current.steps==6);
    input_activity.steps=0;clock_tick(1000,1,0);assert(current.steps==6);
    clock_tick(1000,2,0);assert(current.steps==8);
    input_activity.valid=0;clock_tick(1000,30,0);assert(current.steps==8);
    input_activity.valid=1;clock_tick(1000,30,0);assert(current.steps==8);
    clock_tick(1000,2,0);assert(current.steps==10);
    clock_tick(1000,10000,0);assert(current.steps==10);
    for(int i=0;i<6;i++)clock_tick(1000,2,0);
    assert(current.cadence_valid && current.cadence_spm>0 && current.cadence_spm<=300);
    assert(workout_lap_add());assert(!workout_lap_add());
    close_saved();
    test_ms=UINT32_MAX-500u;assert(workout_start(WORKOUT_GENERAL));
    clock_tick(1000,0,0);assert(current.elapsed_ms==1000);
    assert(workout_pause());clock_tick(2000,0,0);assert(workout_resume());
    clock_tick(1000,0,0);assert(current.elapsed_ms==2000 && current.total_ms==4000);
    close_saved();
    /* A long segment crosses the signed uint32 half-range without rejecting
     * current acquisition stamps or bridging the long sensor absence. */
    new_area("long-segment");assert(workout_start(WORKOUT_RUN));
    clock_tick(1000,2,2);clock_tick(1000,2,2);
    float distance=current.distance_m;
    clock_tick((uint32_t)INT32_MAX+1000u,0,0);
    assert(current.hr_valid && current.distance_m==distance);
    clock_tick(1000,2,2);assert(fabsf(current.distance_m-distance-2)<.03f);
    close_saved();
}
static void gps_metrics(void)
{
    new_area("gps");assert(workout_start(WORKOUT_RUN));
    clock_tick(1000,2,2);assert(!current.distance_valid);
    clock_tick(1000,2,2);assert(current.distance_valid && fabsf(current.distance_m-2)<.03f);
    assert(current.speed_valid && current.pace_valid && current.gps_active_ms==1000);
    assert(fabsf(current.pace_s_km-500)<.001f);
    float before=current.distance_m;
    /* The same timestamp is ignored even if a newer render sees a changed fix. */
    test_ms+=1000;input_gps.age_ms=1000;workout_tick();assert(current.distance_m==before);
    input_gps.age_ms=0;clock_tick(1000,2,2);
    assert(current.distance_m>before);before=current.distance_m;
    input_gps.valid=0;clock_tick(1000,2,50);
    assert(current.gps_status==WORKOUT_GPS_WAITING && !current.speed_valid);
    input_gps.valid=1;clock_tick(1000,2,50);assert(current.distance_m==before);
    clock_tick(1000,2,2);assert(fabsf(current.distance_m-before-2)<.03f);
    before=current.distance_m;
    input_gps.hdop=9;clock_tick(1000,2,2);assert(current.gps_status==WORKOUT_GPS_POOR);
    input_gps.hdop=1;clock_tick(1000,2,50);assert(current.distance_m==before);
    clock_tick(1000,2,1000);assert(current.gps_status==WORKOUT_GPS_REJECTED);
    clock_tick(1000,2,-1000);assert(current.distance_m==before);
    clock_tick(1000,2,2);assert(fabsf(current.distance_m-before-2)<.03f);
    before=current.distance_m;
    input_gps.age_ms=9000;clock_tick(1000,2,2);assert(current.gps_status==WORKOUT_GPS_STALE);
    input_gps.age_ms=0;clock_tick(1000,2,50);assert(current.distance_m==before);
    input_gps.lat=NAN;clock_tick(1000,2,0);assert(current.gps_status==WORKOUT_GPS_POOR);
    input_gps.lat=12.9;input_gps.lon=181;clock_tick(1000,2,0);assert(current.gps_status==WORKOUT_GPS_POOR);
    input_gps.lon=77.6;input_gps.speed_mps=INFINITY;clock_tick(1000,2,0);
    assert(current.gps_status==WORKOUT_GPS_REJECTED);
    input_gps.speed_mps=.1f;clock_tick(1000,2,0);
    for(int i=0;i<60;i++)clock_tick(1000,0,i%2?-.8f:.8f);
    assert(current.distance_m==before); /* standing coordinate jitter */
    input_gps.speed_mps=2;assert(workout_pause());clock_tick(10000,50,2000);
    assert(workout_resume());clock_tick(1000,2,2);assert(current.distance_m==before);
    clock_tick(1000,2,2);assert(fabsf(current.distance_m-before-2)<.03f);
    before=current.distance_m;clock_tick(12000,20,24);assert(current.distance_m==before);
    clock_tick(1000,2,2);assert(fabsf(current.distance_m-before-2)<.03f);
    input_gps.speed_mps=6;clock_tick(1000,2,6);
    assert(current.speed_mps>2.9f && current.speed_mps<3.1f); /* display-only 3s EMA */
    input_gps.valid=0;clock_tick(1000,2,6);assert(!current.speed_valid);
    input_gps.valid=1;clock_tick(1000,2,6);clock_tick(1000,2,6);
    assert(fabsf(current.speed_mps-6)<.001f); /* gap resets smoothing */
    close_saved();
    assert(workout_start(WORKOUT_TREADMILL));
    for(int i=0;i<8;i++)clock_tick(1000,2,5);
    assert(!current.distance_valid && !current.speed_valid && current.distance_m==0);
    close_saved();
}
static void expanded_gps_bounds(void)
{
    const int sports[]={WORKOUT_FOOTBALL,WORKOUT_CRICKET,WORKOUT_RUGBY,WORKOUT_HOCKEY,
                        WORKOUT_TRAIL_RUN,WORKOUT_MTB,WORKOUT_GRAVEL,WORKOUT_RACE_WALK};
    const float speeds[]={8,8,8,8,8,15,15,7};
    new_area("expanded-gps-bounds");
    assert(gps_speed_limit(WORKOUT_WALK)==5 && gps_speed_limit(WORKOUT_HIKE)==5);
    assert(gps_speed_limit(WORKOUT_RUN)==12 && gps_speed_limit(WORKOUT_CYCLE)==35);
    for(unsigned i=0;i<sizeof sports/sizeof sports[0];i++) {
        valid_inputs();assert(workout_start(sports[i]));input_gps.speed_mps=speeds[i];
        clock_tick(1000,0,speeds[i]);clock_tick(1000,0,speeds[i]);
        assert(current.gps_status==WORKOUT_GPS_GOOD && current.distance_valid);
        assert(fabsf(current.distance_m-speeds[i])<.04f);
        assert(current.speed_valid && fabsf(current.speed_mps-speeds[i])<.01f);
        float distance=current.distance_m;
        input_gps.speed_mps=gps_speed_limit(sports[i])+1;
        clock_tick(1000,0,1);assert(current.gps_status==WORKOUT_GPS_REJECTED);
        assert(current.distance_m==distance && !current.speed_valid);
        /* Accepted reported speed cannot excuse an impossible coordinate jump. */
        input_gps.speed_mps=speeds[i];clock_tick(1000,0,speeds[i]);
        clock_tick(1000,0,1000);assert(current.gps_status==WORKOUT_GPS_REJECTED);
        assert(current.distance_m==distance);close_saved();
    }
    /* Existing walking bound remains strict even at a plausible sprint speed. */
    valid_inputs();assert(workout_start(WORKOUT_WALK));input_gps.speed_mps=8;
    clock_tick(1000,0,8);assert(current.gps_status==WORKOUT_GPS_REJECTED);close_saved();
}
static void heart_metrics(void)
{
    new_area("heart");assert(workout_start(WORKOUT_GENERAL));
    clock_tick(1000,0,0);assert(current.hr_valid && current.hr_active_ms==0);
    clock_tick(1000,0,0);assert(current.hr_active_ms==1000 && current.avg_bpm==100);
    input_hr.bpm=120;clock_tick(1000,0,0);
    assert(current.hr_active_ms==2000 && current.avg_bpm==105 && current.peak_bpm==120);
    uint32_t coverage=current.hr_active_ms;
    input_hr.quality=.2f;clock_tick(1000,0,0);assert(!current.hr_valid && current.bpm==0);
    input_hr.quality=.95f;input_hr.on_wrist=0;clock_tick(1000,0,0);assert(!current.hr_valid);
    input_hr.on_wrist=1;input_hr.bpm=250;clock_tick(1000,0,0);assert(!current.hr_valid);
    input_hr.bpm=120;input_hr.quality=NAN;clock_tick(1000,0,0);assert(!current.hr_valid);
    input_hr.quality=.95f;test_ms+=5000;workout_tick();assert(!current.hr_valid);
    clock_tick(1000,0,0);assert(current.hr_active_ms==coverage);
    clock_tick(1000,0,0);assert(current.hr_active_ms==coverage+1000);
    input_hr.bpm=210;clock_tick(1000,0,0);assert(!current.hr_valid && current.peak_bpm==120);
    input_hr.bpm=120;clock_tick(1000,0,0);assert(current.hr_valid);
    input_hr.bpm=180;clock_tick(1000,0,0);assert(!current.hr_valid);
    clock_tick(1000,0,0);assert(!current.hr_valid);
    clock_tick(1000,0,0);assert(current.hr_valid && current.peak_bpm==180);
    coverage=current.hr_active_ms;
    assert(workout_pause());clock_tick(10000,0,0);assert(workout_resume());
    clock_tick(1000,0,0);assert(current.hr_active_ms==coverage);
    clock_tick(1000,0,0);assert(current.hr_active_ms==coverage+1000);
    test_ms+=1000;workout_tick(); /* fresh duplicate retains display, adds no coverage */
    assert(current.hr_valid && current.hr_active_ms==coverage+1000);
    hr_present=0;clock_tick(1000,0,0);assert(!current.hr_valid);
    close_saved();
    new_area("heart-acquisition-time");assert(workout_start(WORKOUT_GENERAL));
    clock_tick(1000,0,0);
    test_ms+=1000;input_hr.sample_ms=test_ms-500;input_hr.bpm=120;workout_tick();
    assert(current.hr_active_ms==500 && current.avg_bpm==110);
    test_ms+=1000;input_hr.sample_ms=test_ms;workout_tick();
    assert(current.hr_active_ms==2000 && current.avg_bpm==118);
    test_ms+=1000;input_hr.sample_ms=test_ms-2500;workout_tick();
    assert(!current.hr_valid); /* backward acquisition stamp */
    close_saved();
}
static void boundary_duplicate_samples(void)
{
    new_area("boundary-duplicates");assert(workout_start(WORKOUT_RUN));
    clock_tick(1000,2,2);clock_tick(1000,2,2);
    workout_snapshot before=*workout_get();
    assert(before.hr_valid && before.speed_valid && before.pace_valid);
    /* A lap arrives 50ms after acquisition, before either cached sensor has
     * published another observation. Its boundary poll must not blink data. */
    test_ms+=50;input_gps.age_ms=50;assert(workout_lap_add());
    const workout_snapshot *s=workout_get();
    assert(s->hr_valid && s->bpm==before.bpm);
    assert(s->speed_valid && s->pace_valid && s->speed_mps==before.speed_mps &&
           s->pace_s_km==before.pace_s_km);
    assert(s->hr_active_ms==before.hr_active_ms && s->gps_active_ms==before.gps_active_ms &&
           s->distance_m==before.distance_m && s->avg_bpm==before.avg_bpm);
    test_ms+=50;input_gps.age_ms=100;assert(workout_lap_add());
    assert(s->hr_valid && s->speed_valid && s->pace_valid && s->distance_m==before.distance_m);
    assert(s->hr_active_ms==before.hr_active_ms && s->gps_active_ms==before.gps_active_ms);
    /* The next distinct observation integrates exactly once from the preserved
     * acquisition baseline, including the time around both lap button presses. */
    input_gps.age_ms=0;clock_tick(1000,2,2);
    assert(s->hr_active_ms==before.hr_active_ms+1100);
    assert(s->gps_active_ms==before.gps_active_ms+1100);
    assert(fabsf(s->distance_m-before.distance_m-2)<.03f);
    before=*s;test_ms+=50;input_gps.age_ms=50;assert(workout_finish());
    assert(s->hr_active_ms==before.hr_active_ms && s->gps_active_ms==before.gps_active_ms &&
           s->distance_m==before.distance_m && s->avg_bpm==before.avg_bpm);
    save();assert(s->save_state==WORKOUT_SAVE_SAVED);

    new_area("boundary-invalid-cache");assert(workout_start(WORKOUT_RUN));
    clock_tick(1000,2,2);clock_tick(1000,2,2);before=*workout_get();s=workout_get();
    /* A duplicate timestamp is no excuse to retain an off-wrist/poor-quality
     * observation. Once invalidated, it cannot restore an interval baseline. */
    test_ms+=50;input_gps.age_ms=50;input_hr.quality=.2f;input_gps.hdop=8;
    assert(workout_lap_add());assert(!s->hr_valid && !s->speed_valid && !s->pace_valid);
    test_ms+=50;input_gps.age_ms=100;input_hr.quality=.95f;input_gps.hdop=1;
    assert(workout_lap_add());assert(!s->hr_valid && !s->speed_valid && !s->pace_valid);
    input_gps.age_ms=0;clock_tick(1000,2,50);
    assert(s->hr_active_ms==before.hr_active_ms && s->gps_active_ms==before.gps_active_ms &&
           s->distance_m==before.distance_m);
    clock_tick(1000,2,2);
    assert(s->hr_active_ms==before.hr_active_ms+1000 &&
           s->gps_active_ms==before.gps_active_ms+1000);
    assert(fabsf(s->distance_m-before.distance_m-2)<.03f);
    /* Still-cached observations expire normally, and fresh return points cannot
     * bridge the stale interval or add fabricated heart-rate coverage. */
    before=*s;test_ms+=3100;input_gps.age_ms=3100;assert(workout_lap_add());
    assert(!s->hr_valid && !s->speed_valid && !s->pace_valid && s->gps_status==WORKOUT_GPS_STALE);
    input_gps.age_ms=0;clock_tick(1000,2,50);
    assert(s->hr_active_ms==before.hr_active_ms && s->distance_m==before.distance_m);
    clock_tick(1000,2,2);assert(s->hr_active_ms==before.hr_active_ms+1000);
    assert(fabsf(s->distance_m-before.distance_m-2)<.03f);
    close_saved();
}
static void storage_and_bounds(void)
{
    new_area("history");assert(workout_start(WORKOUT_WALK));
    for(int i=0;i<WORKOUT_LAPS_MAX;i++) {clock_tick(1000,2,2);assert(workout_lap_add());}
    clock_tick(1000,2,2);assert(!workout_lap_add());
    assert(workout_finish() && current.nlaps==WORKOUT_LAPS_MAX);
    card_present=0;save();assert(current.save_state==WORKOUT_SAVE_FAILED && storage.dirty);
    assert(!workout_start(WORKOUT_RUN));
    uint32_t id=current.id;card_present=1;workout_retry_save();save();
    assert(current.save_state==WORKOUT_SAVE_SAVED && !storage.dirty && history[0].id==id);
    reboot();assert(current.id==id && current.save_state==WORKOUT_SAVE_SAVED);
    assert(history_count==1 && history[0].nlaps==WORKOUT_LAPS_MAX);
    for(int i=0;i<16;i++) {
        assert(workout_start(WORKOUT_GENERAL));clock_tick(1000,0,0);close_saved();
    }
    assert(history_count==WORKOUT_HISTORY_MAX);
    workout_snapshot oldest=history[WORKOUT_HISTORY_MAX-1];
    assert(workout_start(WORKOUT_RUN));clock_tick(1000,2,2);assert(workout_finish());
    card_present=0;save();assert(current.save_state==WORKOUT_SAVE_FAILED);
    assert(workout_discard());assert(history_count==WORKOUT_HISTORY_MAX);
    assert(history[WORKOUT_HISTORY_MAX-1].id==oldest.id);
    card_present=1;save();reboot();assert(history_count==WORKOUT_HISTORY_MAX);
    /* Simulated fsync failure must not confirm a save. */
    assert(workout_start(WORKOUT_GENERAL));clock_tick(1000,0,0);assert(workout_finish());
    commit_failure=1;save();assert(current.save_state==WORKOUT_SAVE_FAILED && storage.dirty);
    commit_failure=0;workout_retry_save();save();assert(current.save_state==WORKOUT_SAVE_SAVED);
    /* A successful write+fsync whose readback is corrupt is still a failure. */
    assert(workout_start(WORKOUT_GENERAL));clock_tick(1000,0,0);assert(workout_finish());
    commit_corruption=1;save();assert(current.save_state==WORKOUT_SAVE_FAILED && storage.dirty);
    commit_corruption=0;workout_retry_save();save();assert(current.save_state==WORKOUT_SAVE_SAVED);
    assert(!workout_history_get(-1,&oldest));assert(!workout_history_get(99,&oldest));
    assert(!workout_history_get(0,NULL));assert(!workout_profile_get(99));
    /* Explicit no-auto-resume restores paused, with no invented downtime. */
    workout_setup opt=next_setup;opt.auto_resume=0;workout_setup_set(&opt);
    assert(workout_start(WORKOUT_RUN));clock_tick(1000,2,2);clock_tick(1000,2,2);save();
    uint32_t elapsed=current.elapsed_ms;test_ms+=60000;reboot();
    assert(current.state==WORKOUT_PAUSED && current.recovered && current.elapsed_ms<=elapsed);
    uint32_t restored=current.elapsed_ms;clock_tick(10000,50,500);assert(current.elapsed_ms==restored);
    assert(workout_resume());clock_tick(1000,2,2);assert(current.steps==2);
    close_saved();
}
static void corruption_and_startup(void)
{
    new_area("corruption");assert(workout_start(WORKOUT_GENERAL));
    clock_tick(1000,0,0);close_saved();
    disk_record a,b;int latest=newest_slot(&a,&b,NULL);assert(latest>=0);
    char path[256];assert(file_path(path,sizeof path,latest));
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite("bad",1,3,f)==3);fclose(f);
    reboot();assert(storage.ready && history_count==1); /* intact older bank */
    for(int i=0;i<2;i++) {
        assert(file_path(path,sizeof path,i));f=fopen(path,"wb");assert(f);
        assert(fwrite("bad",1,3,f)==3);fclose(f);
    }
    reboot();assert(!storage.ready && storage.corrupt && storage.errors);
    assert(!workout_start(WORKOUT_GENERAL));
    workout_retry_save();save();assert(!storage.ready && storage.corrupt);
    for(int i=0;i<2;i++) {
        assert(file_path(path,sizeof path,i));f=fopen(path,"rb");assert(f);
        assert(fseek(f,0,SEEK_END)==0 && ftell(f)==3);fclose(f);
    }
    new_area("boot-without-card");card_present=0;reboot();
    assert(!storage.ready && !storage.available && storage.errors);
    assert(!workout_start(WORKOUT_WALK));card_present=1;workout_retry_save();save();
    assert(storage.ready && workout_start(WORKOUT_WALK));assert(workout_discard());save();
    /* Fields with a matching checksum still need structural validation. */
    assert(newest_slot(&a,&b,NULL)>=0);
    a=io->pending;a.magic=DISK_MAGIC;a.version=DISK_VERSION;a.bytes=sizeof a;
    a.next_id=1;a.count=WORKOUT_HISTORY_MAX+1;a.checksum=disk_checksum(&a);assert(!valid_disk(&a));
    a.count=0;a.session.distance_m=NAN;a.checksum=disk_checksum(&a);assert(!valid_disk(&a));
    a.session.distance_m=0;a.session.nlaps=WORKOUT_LAPS_MAX+1;
    a.checksum=disk_checksum(&a);assert(!valid_disk(&a));
    a.session.nlaps=0;a.session.hr_valid=2;
    a.checksum=disk_checksum(&a);assert(!valid_disk(&a));
}
static void capture_banks(disk_record banks[2])
{ for(int i=0;i<2;i++)assert(read_slot(i,&banks[i])); }
static void replace_banks(const disk_record banks[2])
{
    char path[256];
    for(int i=0;i<2;i++) {
        assert(file_path(path,sizeof path,i));
        if(!banks) {remove(path);continue;}
        FILE *f=fopen(path,"wb");assert(f);
        assert(fwrite(&banks[i],1,sizeof banks[i],f)==sizeof banks[i]);assert(!fclose(f));
    }
}
static void expect_banks(const disk_record banks[2])
{
    disk_record actual;
    for(int i=0;i<2;i++) {
        int state=read_slot_status(i,&actual);
        if(banks)assert(state==1 && !memcmp(&actual,&banks[i],sizeof actual));
        else assert(state==0);
    }
}
static void card_lineage(void)
{
    disk_record other[2],original[2],damaged[2];
    new_area("replacement-card");
    for(int i=0;i<3;i++) {
        assert(workout_start(WORKOUT_GENERAL));clock_tick(1000,0,0);close_saved();
    }
    capture_banks(other);assert(history_count==3);
    new_area("original-card");
    assert(workout_start(WORKOUT_GENERAL));clock_tick(1000,0,0);close_saved();
    capture_banks(original);
    assert(workout_start(WORKOUT_RUN));clock_tick(1000,2,2);assert(workout_finish());
    uint32_t id=current.id;replace_banks(other);save();
    assert(storage.foreign && storage.dirty && current.save_state==WORKOUT_SAVE_FAILED);
    expect_banks(other); /* Never replace another card's three saved sessions. */
    replace_banks(original);workout_retry_save();save();
    assert(!storage.foreign && !storage.dirty && current.save_state==WORKOUT_SAVE_SAVED);
    assert(current.id==id && history_count==2);reboot();
    assert(history_count==2 && current.id==id && current.save_state==WORKOUT_SAVE_SAVED);
    capture_banks(original);
    assert(workout_start(WORKOUT_GENERAL));clock_tick(1000,0,0);assert(workout_finish());
    replace_banks(NULL);save();
    assert(storage.foreign && storage.dirty && current.save_state==WORKOUT_SAVE_FAILED);
    expect_banks(NULL); /* A blank replacement is not the original empty bank. */
    replace_banks(original);workout_retry_save();save();
    assert(!storage.foreign && current.save_state==WORKOUT_SAVE_SAVED);
    capture_banks(original);
    /* Even one known bank must not authorize overwriting an unknown, valid
     * other bank: a card cloned from an old checkpoint can have diverged. */
    damaged[0]=other[0];damaged[1]=original[1];replace_banks(damaged);
    assert(workout_start(WORKOUT_GENERAL));clock_tick(1000,0,0);assert(workout_finish());save();
    assert(storage.foreign && storage.dirty);expect_banks(damaged);
    replace_banks(original);workout_retry_save();save();
    assert(!storage.foreign && current.save_state==WORKOUT_SAVE_SAVED);
    /* A damaged newest original bank can still be repaired from the known
     * older bank. A checksummed unrelated record is the refusal condition. */
    disk_record a,b;int latest=newest_slot(&a,&b,NULL);assert(latest>=0);
    char path[256];assert(file_path(path,sizeof path,latest));
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite("bad",1,3,f)==3);assert(!fclose(f));
    assert(workout_start(WORKOUT_GENERAL));clock_tick(1000,0,0);assert(workout_finish());save();
    assert(!storage.foreign && current.save_state==WORKOUT_SAVE_SAVED);
    /* A card appearing before the first write is foreign too; returning to
     * the original pristine directory still permits its initial checkpoint. */
    new_area("pristine-replacement");replace_banks(other);
    assert(workout_start(WORKOUT_GENERAL));clock_tick(1000,0,0);assert(workout_finish());save();
    assert(storage.foreign && storage.dirty);expect_banks(other);
    replace_banks(NULL);workout_retry_save();save();
    assert(!storage.foreign && current.save_state==WORKOUT_SAVE_SAVED);
    new_area("pristine-fsync-failure");assert(workout_start(WORKOUT_GENERAL));
    clock_tick(1000,0,0);assert(workout_finish());commit_failure=1;save();
    assert(storage.dirty && !storage.foreign && current.save_state==WORKOUT_SAVE_FAILED);
    commit_failure=0;workout_retry_save();save();
    assert(!storage.dirty && !storage.foreign && current.save_state==WORKOUT_SAVE_SAVED);
    new_area("pristine-readback-corruption");assert(workout_start(WORKOUT_GENERAL));
    clock_tick(1000,0,0);assert(workout_finish());commit_corruption=1;save();
    assert(storage.dirty && !storage.foreign && current.save_state==WORKOUT_SAVE_FAILED);
    commit_corruption=0;workout_retry_save();save();
    assert(!storage.dirty && !storage.foreign && current.save_state==WORKOUT_SAVE_SAVED);
    new_area("pristine-transient-readback");assert(workout_start(WORKOUT_GENERAL));
    clock_tick(1000,0,0);assert(workout_finish());readback_failure=1;save();
    assert(storage.dirty && !storage.foreign && current.save_state==WORKOUT_SAVE_FAILED);
    /* The complete write is now a valid bank, but readback did not acknowledge
     * it. Its in-flight identity must be accepted as our own on the retry. */
    readback_failure=0;workout_retry_save();save();
    assert(!storage.dirty && !storage.foreign && current.save_state==WORKOUT_SAVE_SAVED);
}
static int process_mode(const char *mode,const char *directory)
{
    workout_setup opt={0,0,0,0,0,0,0};workout_setup_set(&opt);
    assert(workout_storage_set_directory(directory));valid_inputs();workout_init();save();
    assert(storage.ready);
    if(!strcmp(mode,"--write-recovery")) {
        assert(workout_start(WORKOUT_RUN));
        clock_tick(1000,2,2);clock_tick(1000,2,2);assert(workout_lap_add());save();
        assert(!storage.dirty);puts("PASS wrote running checkpoint for process restart");return 0;
    }
    assert(!strcmp(mode,"--read-recovery"));
    assert(current.state==WORKOUT_PAUSED && current.recovered);
    assert(current.elapsed_ms==2000 && current.steps==4 && current.nlaps==1);
    clock_tick(5000,10,500);assert(current.elapsed_ms==2000);
    assert(workout_resume());clock_tick(1000,2,2);
    assert(current.elapsed_ms==3000 && current.steps==6);
    close_saved();puts("PASS resumed and saved checkpoint in a separate process");return 0;
}
static void unreadable_bank_recovery(void)
{
    new_area("read-io-recovery");assert(workout_start(WORKOUT_WALK));save();
    clock_tick(1000,2,0);assert(workout_pause());save();
    assert(workout_resume());clock_tick(1000,2,0);assert(workout_pause());save();
    disk_record a,b;int states[2],latest=newest_slot(&a,&b,states);
    assert(latest==0 && a.session.elapsed_ms==2000 && b.session.elapsed_ms==1000);
    uint64_t hashes[2]={bank_hash(&a),bank_hash(&b)};int writes=write_opens;
    unreadable_bank=latest;reboot();
    assert(!storage.ready && !storage.available && storage.uncertain && !storage.corrupt);
    assert(storage.errors && storage.last_operation==WORKOUT_IO_READ_OPEN && storage.last_slot==latest && storage.last_errno==EIO);
    assert(write_opens==writes && !loaded && current.state==WORKOUT_IDLE);
    workout_retry_save();save();assert(!storage.ready && write_opens==writes);
    unreadable_bank=-1;
    assert(newest_slot(&a,&b,states)==latest && bank_hash(&a)==hashes[0] && bank_hash(&b)==hashes[1]);
    workout_retry_save();save();assert(storage.ready && current.elapsed_ms==2000 && storage.dirty);
    /* Also protect preflight after a good authoritative recovery. */
    unreadable_bank=latest;workout_retry_save();save();
    assert(storage.uncertain && storage.dirty && !storage.available && write_opens==writes);
    unreadable_bank=-1;
    assert(newest_slot(&a,&b,states)==latest && bank_hash(&a)==hashes[0] && bank_hash(&b)==hashes[1]);
    workout_retry_save();save();assert(!storage.uncertain && !storage.dirty);
    assert(current.elapsed_ms==2000);close_saved();
}
static void quality_and_placement_persistence(void)
{
    new_area("quality-summary");workout_setup setup={HAND_LEFT,HAND_UNKNOWN,HAND_RIGHT,HAND_UNKNOWN,0,0,PLACEMENT_CALIBRATED};
    workout_setup_set(&setup);workout_placement_revision_set(19);
    assert(workout_start(WORKOUT_BADMINTON));assert(hr_owners&HAL_HR_OWNER_WORKOUT);
    hal_hr_request(HAL_HR_OWNER_HEART,1);hal_hr_request(HAL_HR_OWNER_HEART,0);
    hal_hr_request(HAL_HR_OWNER_BREATHE,1);hal_hr_request(HAL_HR_OWNER_BREATHE,0);
    assert(hr_owners==HAL_HR_OWNER_WORKOUT);
    input_hr.quality=.1f;clock_tick(1000,0,0);
    assert(current.quality.rejected[WORKOUT_REJECT_HR_QUALITY]==1);
    hal_imu v={NAN,0,1,0,0,0};test_ms+=20;workout_motion_sample(&v,test_ms);workout_tick();
    assert(current.quality.rejected[WORKOUT_REJECT_MOTION_INVALID]==1);
    input_hr.quality=.95f;clock_tick(1000,0,0);assert(current.quality.hr_sample_ms==test_ms);
    assert(workout_pause());assert(!(hr_owners&HAL_HR_OWNER_WORKOUT));save();
    reboot();assert(current.placement_revision==19 && current.setup.worn_wrist==HAND_LEFT && current.setup.dominant_hand==HAND_UNKNOWN && current.setup.playing_hand==HAND_RIGHT);
    assert(current.quality.rejected[WORKOUT_REJECT_HR_QUALITY]==1 && current.quality.rejected[WORKOUT_REJECT_MOTION_INVALID]==1);
    assert(workout_resume() && (hr_owners&HAL_HR_OWNER_WORKOUT));
    clock_tick(1000,0,0);close_saved();assert(!hr_owners);
}
static void category_energy(void)
{
    new_area("category-energy");workout_setup setup={HAND_LEFT,HAND_RIGHT,HAND_RIGHT,0,1,0,PLACEMENT_USER};
    hr_present=0; /* no heart rate: the category MET path */
    workout_setup_set(&setup);assert(workout_start(WORKOUT_BADMINTON));
    /* Unset mass is estimated for an assumed 70 kg body and says so. */
    for(int i=0;i<10;i++)clock_tick(1000,0,0);
    assert(current.energy_kcal_est>0 && (current.energy_basis&ENERGY_BASIS_ASSUMED_MASS) && (current.energy_basis&ENERGY_BASIS_ASSUMED_BODY));
    assert(!(current.energy_basis&ENERGY_BASIS_HR));close_saved();
    setup.mass_kg=70;workout_setup_set(&setup);assert(workout_start(WORKOUT_BADMINTON));
    assert(current.met_tenths==55);
    for(int i=0;i<60;i++)clock_tick(1000,0,0);
    assert(!(current.energy_basis&ENERGY_BASIS_ASSUMED_MASS));
    /* No valid motion: the unscaled category MET (5.5 x 3.5 x 70 / 200 per minute). */
    assert(fabsf(current.energy_kcal_est-6.7375f)<.001f);
    assert(current.movement_ms==0); /* category estimate does not use movement */
    assert(workout_pause());float energy=current.energy_kcal_est;
    clock_tick(60000,0,0);assert(current.energy_kcal_est==energy);
    setup.mass_kg=90;workout_setup_set(&setup); /* current mass is frozen */
    assert(current.setup.mass_kg==70);assert(workout_resume());
    for(int i=0;i<60;i++)clock_tick(1000,0,0);
    assert(fabsf(current.energy_kcal_est-13.475f)<.001f);
    assert(workout_finish());save();float frozen=current.energy_kcal_est;reboot();
    assert(current.met_tenths==55 && current.setup.mass_kg==70);
    assert(fabsf(current.energy_kcal_est-frozen)<.001f);
    /* Valid heart rate switches to Keytel (unset sex/age: male/female midpoint = 5.457 kcal/min at
     * 100 bpm, 70 kg). Net energy excludes the resting rate. */
    hr_present=1;valid_inputs();assert(workout_discard());save();
    setup.mass_kg=70;workout_setup_set(&setup);assert(workout_start(WORKOUT_BADMINTON));
    for(int i=0;i<60;i++)clock_tick(1000,0,0);
    assert(current.energy_basis&ENERGY_BASIS_HR);
    assert(fabsf(current.energy_kcal_est-5.457f)<.12f);
    assert(current.energy_net_kcal>0 && current.energy_net_kcal<current.energy_kcal_est);
    close_saved();
}
static void energy_series_and_v3(void)
{
    /* Movement-scaled MET (no HR): constant strong wrist motion reads above the flat category
     * value, and the series records it per minute. Steps are counted for every sport. */
    new_area("energy-move");hr_present=0;workout_setup setup={HAND_LEFT,HAND_RIGHT,HAND_RIGHT,0,1,70,PLACEMENT_USER};
    workout_setup_set(&setup);assert(workout_start(WORKOUT_BADMINTON));
    hal_imu v={0,0,1,100,0,0};
    for(int s=0;s<180;s++) {
        for(int k=0;k<50;k++) {test_ms+=20;workout_motion_sample(&v,test_ms);}
        input_activity.steps+=2;input_hr.sample_ms=test_ms;workout_tick();
    }
    assert(current.energy_basis&ENERGY_BASIS_MOVE);
    assert(current.energy_kcal_est>3*6.7375f*1.15f && current.energy_kcal_est<3*30.0f);
    assert(current.energy_net_kcal>0 && current.energy_net_kcal<current.energy_kcal_est);
    assert(current.ped_steps>=300 && current.ped_steps<=370);
    assert(current.series_bin_s==60 && current.series_n>=2 && current.series_n<=3);
    for(unsigned i=0;i<current.series_n;i++)assert(current.series_move[i]>=60 && current.series_move[i]<=100 && !current.series_bpm[i]);
    uint32_t ped=current.ped_steps;float net=current.energy_net_kcal;unsigned n=current.series_n;
    assert(workout_finish());save();assert(current.save_state==WORKOUT_SAVE_SAVED);
    assert(current.series_n>=n);n=current.series_n;
    reboot();assert(current.ped_steps==ped && current.series_n==n && fabsf(current.energy_net_kcal-net)<.001f);
    assert(history_count==1 && history[0].series_n==n && history[0].ped_steps==ped);
    /* Daily merge input: the finished session (also still `current`) counts exactly once. */
    {uint32_t ds=0;float dn=0;
     assert(workout_day_energy(0,&ds,&dn)==1 && ds==ped && fabsf(dn-net)<.001f);
     assert(workout_day_energy(history[0].started_epoch,&ds,&dn)==1);
     assert(workout_day_energy(history[0].started_epoch+1,&ds,&dn)==0 && ds==0 && dn==0);}
    assert(workout_discard());save();

    /* Rest-only motion: 1.3 MET, far below the flat category MET. */
    new_area("energy-rest");hr_present=0;setup.mass_kg=70;workout_setup_set(&setup);assert(workout_start(WORKOUT_BADMINTON));
    hal_imu rest={0,0,1,0,0,0};
    for(int s=0;s<60;s++) {
        for(int k=0;k<50;k++) {test_ms+=20;workout_motion_sample(&rest,test_ms);}
        input_hr.sample_ms=test_ms;workout_tick();
    }
    assert(current.energy_kcal_est>1.4f && current.energy_kcal_est<2.2f);
    assert(current.series_n==1 && current.series_move[0]<=5);
    assert(workout_finish());save();assert(workout_discard());save();

    /* Heart rate and movement land in the same bins; a long session halves resolution, not data. */
    new_area("series-compact");hr_present=1;valid_inputs();setup.mass_kg=0;workout_setup_set(&setup);
    assert(workout_start(WORKOUT_RUN));
    for(int s=0;s<125*60;s++) {input_hr.bpm=100+(s/600)%5;clock_tick(1000,1,0);}
    assert(current.series_bin_s==120 && current.series_n>=61 && current.series_n<=64);
    assert(current.series_bpm[0]>=100 && current.series_bpm[0]<=104);
    assert(current.series_move[0]==255); /* no motion feed: unknown, not zero */
    assert(current.ped_steps>=7400 && current.ped_steps<=7500);
    assert((current.energy_basis&ENERGY_BASIS_HR) && (current.energy_basis&ENERGY_BASIS_ASSUMED_MASS));
    assert(workout_finish());save();assert(workout_discard());save();

    /* A V3 bank (no body/series) migrates in place to V4 and keeps its energy figure. */
    hr_present=1;new_area("migration-v3");unsigned char raw3[WORKOUT_DISK_V3_BYTES]={0};
    uint32_t head3[6]={DISK_MAGIC,3,WORKOUT_DISK_V3_BYTES,12,1,8};memcpy(raw3,head3,sizeof head3);
    workout_snapshot old={0};old.id=9;old.sport=WORKOUT_BADMINTON;old.state=WORKOUT_RUNNING;
    old.elapsed_ms=old.total_ms=61000;old.setup.mass_kg=70;old.met_tenths=55;old.energy_kcal_est=6.5f;
    memcpy(raw3+32,&old,WORKOUT_SNAPSHOT_V3_BYTES);
    old.state=WORKOUT_FINISHED;old.save_state=WORKOUT_SAVE_SAVED;
    memcpy(raw3+32+WORKOUT_SNAPSHOT_V3_BYTES,&old,WORKOUT_SNAPSHOT_V3_BYTES);
    uint32_t sum=2166136261u;
    for(unsigned i=0;i<WORKOUT_DISK_V3_CHECKSUM_OFFSET;i++)sum=(sum^raw3[i])*16777619u;
    memcpy(raw3+WORKOUT_DISK_V3_CHECKSUM_OFFSET,&sum,4);
    char path[256];assert(file_path(path,sizeof path,0));FILE *f=fopen(path,"wb");assert(f);
    assert(fwrite(raw3,1,sizeof raw3,f)==sizeof raw3);assert(!fclose(f));
    reboot();assert(current.id==9 && fabsf(current.energy_kcal_est-6.5f)<.001f);
    assert(current.series_n==0 && current.ped_steps==0 && current.energy_net_kcal==0 && current.energy_basis==0);
    assert(history_count==1 && history[0].id==9 && history[0].series_n==0);
    save();assert(!storage.dirty && !storage.foreign);
    unsigned char retained3[WORKOUT_DISK_V3_BYTES];f=fopen(path,"rb");assert(f);
    assert(fread(retained3,1,sizeof retained3,f)==sizeof retained3);assert(!fclose(f));
    assert(!memcmp(raw3,retained3,sizeof raw3));
    hr_present=1;
}
static void expansion_metrics(void)
{
    /* Isolated spikes cannot become episodes; rotations preserve magnitudes. */
    workout_motion a,b;workout_motion_reset(&a);workout_motion_reset(&b);
    for(unsigned i=0;i<200;i++) {
        float g=(i>=20 && i<40)?100.0f:0;
        if(i==70)g=200;
        workout_motion_feed(&a,i*20,0,0,1,g,0,0);
        workout_motion_feed(&b,i*20,1,0,0,0,-g,0);
    }
    assert(a.bursts==1 && a.movement_ms>=300 && a.movement_ms<600);
    assert(a.valid_ms==3980 && a.bursts==b.bursts && a.movement_ms==b.movement_ms);
    uint32_t covered=a.valid_ms;
    workout_motion_feed(&a,9000,0,0,1,100,0,0);
    assert(a.valid_ms==covered); /* unknown gap is not rest or active */
    workout_motion_feed(&a,9020,NAN,0,1,100,0,0);
    assert(!a.have);

    new_area("expansion");workout_setup setup={HAND_LEFT,HAND_RIGHT,HAND_RIGHT,HAND_LEFT,1,70,PLACEMENT_USER};
    workout_setup_set(&setup);assert(WORKOUT_SPORT_COUNT==40);
    for(int i=0;i<WORKOUT_SPORT_COUNT;i++)assert(workout_profile_get(i));
    assert(workout_start(WORKOUT_BADMINTON));
    setup.worn_wrist=HAND_RIGHT;workout_setup_set(&setup);
    assert(current.setup.worn_wrist==HAND_LEFT && current.setup.playing_hand==HAND_RIGHT);
    hal_imu v={0,0,1,100,0,0};
    for(int i=0;i<100;i++) {test_ms+=20;workout_motion_sample(&v,test_ms);workout_tick();}
    assert(current.movement_bursts==1 && current.movement_ms>1500);
    assert(current.motion_valid_ms<=current.elapsed_ms);
    assert(workout_lap_add());save();uint32_t elapsed=current.elapsed_ms,moved=current.movement_ms;
    test_ms+=60000;reboot();assert(current.state==WORKOUT_RUNNING);
    assert(current.recovery_count==1 && current.elapsed_ms==elapsed && current.movement_ms==moved);
    assert(current.setup.worn_wrist==HAND_LEFT);
    assert(workout_pause());save();test_ms+=60000;reboot();
    assert(current.state==WORKOUT_PAUSED && current.recovery_count==2);
    assert(current.elapsed_ms==elapsed);close_saved();

    /* Original V1 bytes migrate without replacing or losing the source bank. */
    new_area("migration");unsigned char raw[WORKOUT_DISK_V1_BYTES]={0};
    uint32_t head[6]={DISK_MAGIC,1,WORKOUT_DISK_V1_BYTES,5,1,8};memcpy(raw,head,sizeof head);
    workout_snapshot old={0};old.id=7;old.sport=WORKOUT_RUN;old.state=WORKOUT_RUNNING;
    old.elapsed_ms=old.total_ms=1234;old.steps=9;
    memcpy(raw+32,&old,WORKOUT_SNAPSHOT_V1_BYTES);
    old.state=WORKOUT_FINISHED;old.save_state=WORKOUT_SAVE_SAVED;
    memcpy(raw+268,&old,WORKOUT_SNAPSHOT_V1_BYTES);
    uint32_t checksum=2166136261u;
    for(unsigned i=0;i<WORKOUT_DISK_V1_CHECKSUM_OFFSET;i++)checksum=(checksum^raw[i])*16777619u;
    memcpy(raw+WORKOUT_DISK_V1_CHECKSUM_OFFSET,&checksum,4);
    char path[256];assert(file_path(path,sizeof path,0));FILE *f=fopen(path,"wb");assert(f);
    assert(fwrite(raw,1,sizeof raw,f)==sizeof raw);assert(!fclose(f));
    reboot();assert(current.id==7 && current.steps==9 && current.elapsed_ms==1234);
    assert(current.state==WORKOUT_PAUSED && !current.setup.auto_resume);
    assert(history_count==1 && history[0].id==7 && history[0].steps==9);
    assert(!storage.foreign);save();assert(!storage.foreign && !storage.dirty);
    /* Migration writes an alternate V2 bank, preserving original V1 bytes. */
    f=fopen(path,"rb");assert(f);unsigned char retained[WORKOUT_DISK_V1_BYTES];
    assert(fread(retained,1,sizeof retained,f)==sizeof retained);assert(!fclose(f));
    assert(!memcmp(raw,retained,sizeof raw));
    /* The recovered V2 bank also migrates without disturbing either its frozen
     * hands or its original bytes; unknown provenance revision stays unknown. */
    new_area("migration-v2");unsigned char raw2[WORKOUT_DISK_V2_BYTES]={0};
    uint32_t head2[6]={DISK_MAGIC,2,WORKOUT_DISK_V2_BYTES,12,1,8};memcpy(raw2,head2,sizeof head2);
    memset(&old,0,sizeof old);old.id=7;old.sport=WORKOUT_BADMINTON;old.state=WORKOUT_RUNNING;
    old.elapsed_ms=old.total_ms=1234;old.setup=(workout_setup){HAND_LEFT,HAND_UNKNOWN,HAND_RIGHT,0,0,70,PLACEMENT_CALIBRATED};
    old.met_tenths=55;memcpy(raw2+32,&old,WORKOUT_SNAPSHOT_V2_BYTES);
    old.state=WORKOUT_FINISHED;old.save_state=WORKOUT_SAVE_SAVED;
    memcpy(raw2+32+WORKOUT_SNAPSHOT_V2_BYTES,&old,WORKOUT_SNAPSHOT_V2_BYTES);
    checksum=2166136261u;
    for(unsigned i=0;i<WORKOUT_DISK_V2_CHECKSUM_OFFSET;i++)checksum=(checksum^raw2[i])*16777619u;
    memcpy(raw2+WORKOUT_DISK_V2_CHECKSUM_OFFSET,&checksum,4);
    assert(file_path(path,sizeof path,0));f=fopen(path,"wb");assert(f);
    assert(fwrite(raw2,1,sizeof raw2,f)==sizeof raw2);assert(!fclose(f));
    reboot();assert(current.state==WORKOUT_PAUSED && current.setup.worn_wrist==HAND_LEFT && current.setup.playing_hand==HAND_RIGHT);
    assert(current.setup.dominant_hand==HAND_UNKNOWN && current.placement_revision==0 && !current.quality.last_reason);
    save();assert(!storage.dirty && !storage.foreign);
    unsigned char retained2[WORKOUT_DISK_V2_BYTES];f=fopen(path,"rb");assert(f);
    assert(fread(retained2,1,sizeof retained2,f)==sizeof retained2);assert(!fclose(f));
    assert(!memcmp(raw2,retained2,sizeof raw2));
}
int main(int argc,char **argv)
{
    if(argc==3)return process_mode(argv[1],argv[2]);
    unreadable_bank_recovery();quality_and_placement_persistence();category_energy();energy_series_and_v3();expansion_metrics();timing_and_steps();gps_metrics();expanded_gps_bounds();heart_metrics();boundary_duplicate_samples();storage_and_bounds();corruption_and_startup();card_lineage();
    /* Frame-rate polling must not raise sensor acquisition beyond 1Hz. */
    new_area("poll-rate");assert(workout_start(WORKOUT_RUN));
    int gr=gps_reads,hr=hr_reads;
    for(int i=0;i<300;i++) {test_ms+=10;input_hr.sample_ms=test_ms;workout_tick();}
    assert(gps_reads-gr==3 && hr_reads-hr==3);
    puts("PASS workout: pause/resume, wrap, steps/reset/gap/cadence, GNSS quality/jumps/jitter/gaps, HR freshness/quality/spikes/coverage, duplicate LAP/FINISH boundaries, bounded laps/history, pending/failed/retry, fsync/readback, recovery, corruption, card lineage and 1Hz work");
    return 0;
}

