/* Session recorder: cached sensor reads and bounded arithmetic in the service
 * loop; checksummed alternating SD checkpoints in a separate writer context.
 * Thresholds below are conservative engineering defaults. See
 * docs/research/SPORT_MODE_RESEARCH.md for evidence and limitations. */
#include "workout.h"
#include "hal.h"
#include "hal_ext.h"
#include "sports.h"
#include "workout_motion.h"
#include "energy.h"
#include "diag_log.h"
#include "runtime_admission.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define MKDIR(p) _mkdir(p)
#define SYNC(fd) _commit(fd)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p,0777)
#define SYNC(fd) fsync(fd)
#endif
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
static portMUX_TYPE writer_mux=portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&writer_mux)
#define UNLOCK() portEXIT_CRITICAL(&writer_mux)
#else
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#endif

#define DISK_MAGIC 0x574f5331u
#define DISK_VERSION WORKOUT_DISK_VERSION
#define SAMPLE_MS 1000u
#define CHECKPOINT_MS 30000u
#define RETRY_MS 5000u
#define GPS_FRESH_MS 2000u
#define GAP_MS 3000u
#define HR_FRESH_MS 3000u
#define CADENCE_N 15
/* A workout longer than 30 days is outside this on-watch recorder's format. */
#define MAX_TIME_MS (30u*24u*60u*60u*1000u)

static const workout_profile profiles[WORKOUT_SPORT_COUNT]={
    {"WALK","OUTDOOR / GNSS + TIME",1,1,0,0,38},
    {"RUN","OUTDOOR / GNSS + TIME",1,1,0,0,75},
    {"CYCLE","OUTDOOR / GNSS + TIME",1,0,1,2,70},
    {"HIKE","OUTDOOR / GNSS + TIME",1,1,0,0,53},
    {"TREADMILL","TIME / HEART + MOVEMENT",0,1,0,1,38},
    {"WORKOUT","TIME / HEART + MOVEMENT",0,0,0,5,38},
    {"FOOTBALL","OUTDOOR / GNSS + TIME",1,0,1,4,70},
    {"CRICKET","OUTDOOR / GNSS + TIME",1,0,0,4,48},
    {"BADMINTON","TIME / HEART + MOVEMENT",0,0,0,3,55},
    {"TRAIL RUN","OUTDOOR / GNSS + TIME",1,1,0,0,75},
    {"TREKKING","OUTDOOR / GNSS + TIME",1,1,0,0,53},
    {"RACE WALK","OUTDOOR / GNSS + TIME",1,1,0,0,65},
    {"NORDIC WALK","OUTDOOR / GNSS + TIME",1,1,0,0,43},
    {"INDOOR WALK","TIME / HEART + MOVEMENT",0,1,0,1,38},
    {"STAIRS","TIME / HEART + MOVEMENT",0,1,0,1,68},
    {"MOUNTAIN BIKE","OUTDOOR / GNSS + TIME",1,0,1,2,85},
    {"GRAVEL BIKE","OUTDOOR / GNSS + TIME",1,0,1,2,58},
    {"INDOOR CYCLE","TIME / HEART + MOVEMENT",0,0,0,2,68},
    {"TENNIS","TIME / HEART + MOVEMENT",0,0,0,3,68},
    {"TABLE TENNIS","TIME / HEART + MOVEMENT",0,0,0,3,40},
    {"SQUASH","TIME / HEART + MOVEMENT",0,0,0,3,73},
    {"PICKLEBALL","TIME / HEART + MOVEMENT",0,0,0,3,45},
    {"PADEL","TIME / HEART + MOVEMENT",0,0,0,3,45},
    {"BASKETBALL","TIME / HEART + MOVEMENT",0,0,0,4,60},
    {"VOLLEYBALL","TIME / HEART + MOVEMENT",0,0,0,4,30},
    {"HANDBALL","TIME / HEART + MOVEMENT",0,0,0,4,80},
    {"FIELD HOCKEY","OUTDOOR / GNSS + TIME",1,0,1,4,78},
    {"RUGBY","OUTDOOR / GNSS + TIME",1,0,1,4,63},
    {"STRENGTH","TIME / HEART + MOVEMENT",0,0,0,5,35},
    {"HIIT","TIME / HEART + MOVEMENT",0,0,0,5,70},
    {"CIRCUIT","TIME / HEART + MOVEMENT",0,0,0,5,50},
    {"AEROBICS","TIME / HEART + MOVEMENT",0,0,0,5,48},
    {"DANCE","TIME / HEART + MOVEMENT",0,0,0,5,38},
    {"JUMP ROPE","TIME / HEART + MOVEMENT",0,0,0,5,110},
    {"BOXING FITNESS","TIME / HEART + MOVEMENT",0,0,0,5,58},
    {"YOGA","TIME / HEART + MOVEMENT",0,0,0,6,23},
    {"PILATES","TIME / HEART + MOVEMENT",0,0,0,6,28},
    {"STRETCHING","TIME / HEART + MOVEMENT",0,0,0,6,23},
    {"INDOOR ROWING","TIME / HEART + MOVEMENT",0,0,0,7,50},
    {"KAYAK","OUTDOOR / GNSS + TIME",1,0,1,7,50}
};
typedef struct {
    uint32_t magic, version, bytes, sequence;
    uint32_t count, next_id;
    uint64_t hr_integral;
    workout_snapshot session;
    workout_snapshot history[WORKOUT_HISTORY_MAX]; /* newest first */
    uint32_t checksum;
} disk_record;
typedef char snapshot_v4_size_check[(sizeof(workout_snapshot)==WORKOUT_SNAPSHOT_V4_BYTES)?1:-1];
typedef char snapshot_v4_extension_check[(offsetof(workout_snapshot,body)==WORKOUT_SNAPSHOT_V3_BYTES)?1:-1];
typedef char snapshot_v1_laps_check[(offsetof(workout_snapshot,laps)==76u)?1:-1];
typedef char snapshot_v2_extension_check[(offsetof(workout_snapshot,setup)==236u)?1:-1];
typedef char snapshot_v3_extension_check[(offsetof(workout_snapshot,placement_revision)==264u)?1:-1];
typedef char disk_v4_size_check[(sizeof(disk_record)==WORKOUT_DISK_V4_BYTES)?1:-1];
typedef char disk_v4_checksum_check[(offsetof(disk_record,checksum)==WORKOUT_DISK_V4_CHECKSUM_OFFSET)?1:-1];
typedef struct { disk_record pending, work, verify; } writer_memory;
#ifdef ESP_PLATFORM
static writer_memory *io;
static char storage_directory[224]="/sdcard/watch/workouts";
#else
static writer_memory host_writer;
static writer_memory *io=&host_writer;
static char storage_directory[224]="build/nos_workouts";
#endif
static uint32_t now_ms(void);
static workout_snapshot current, history[WORKOUT_HISTORY_MAX];
static workout_setup next_setup={0,0,0,0,1,0,0};
static workout_body next_body;
static uint32_t next_placement_revision;
static workout_motion motion;
static int motion_enabled;
static uint32_t motion_start_ms;
static int setup_valid(const workout_setup *s)
{ return s->worn_wrist<=HAND_RIGHT && s->dominant_hand<=HAND_RIGHT &&
    s->playing_hand<=HAND_BOTH && s->bowling_hand<=HAND_RIGHT &&
    s->auto_resume<=1 && s->placement_source<=PLACEMENT_CALIBRATED; }
void workout_setup_set(const workout_setup *s) { if(s && setup_valid(s))next_setup=*s; }
void workout_setup_get(workout_setup *s) { if(s)*s=next_setup; }
void workout_body_set(const workout_body *b) { if(b && b->sex<=2)next_body=*b; }
void workout_body_get(workout_body *b) { if(b)*b=next_body; }
void workout_placement_revision_set(uint32_t revision) { next_placement_revision=revision; }
void workout_motion_sample(const hal_imu *v,uint32_t now)
{
    if(!v)return;
    LOCK();if(motion_enabled && (uint32_t)(now-motion_start_ms)<=MAX_TIME_MS)workout_motion_feed(&motion,now,v->ax,v->ay,v->az,v->gx,v->gy,v->gz);UNLOCK();
}
static void motion_sync(void)
{
    LOCK();current.motion_valid_ms=motion.valid_ms;current.movement_ms=motion.movement_ms;
    current.movement_bursts=motion.bursts;
    current.quality.rejected[WORKOUT_REJECT_MOTION_INVALID]=motion.invalid;
    current.quality.rejected[WORKOUT_REJECT_MOTION_SATURATED]=motion.saturated;
    current.quality.rejected[WORKOUT_REJECT_MOTION_GAP]=motion.gaps;
    current.quality.motion_sample_ms=motion.accepted_ms;
    if(motion.reject_reason && (int32_t)(motion.rejected_ms-current.quality.last_reject_ms)>0) {
        current.quality.last_reject_ms=motion.rejected_ms;
        current.quality.last_reason=(uint8_t)(WORKOUT_REJECT_MOTION_INVALID+motion.reject_reason-1);
    }
    /* Producer can advance by a sample while this service tick runs. Only
     * expose coverage within this tick's observed session duration. */
    if(current.motion_valid_ms>current.elapsed_ms)current.motion_valid_ms=current.elapsed_ms;
    if(current.movement_ms>current.motion_valid_ms)current.movement_ms=current.motion_valid_ms;
    UNLOCK();
}
static void motion_boundary(int enabled)
{ LOCK();motion_enabled=enabled;motion_start_ms=now_ms();workout_motion_boundary(&motion);UNLOCK(); }

static workout_snapshot evicted;
static int have_evicted;
static workout_storage_status storage;
static uint32_t history_count, next_id=1;
static uint32_t revision, saved_revision, ack_revision;
static uint32_t last_attempt_ms, last_checkpoint_ms;
static int initialized, load_pending, loaded, force_write, attempted;
static uint32_t worker_attempt_ms, quality_log_ms;
static int worker_attempted;
static int ack_success, ack_pending;
static int failed_pristine_slot=-1;
/* Pin the loaded journal, including both banks so a damaged newest bank may
 * still be repaired. A replacement card must never inherit our RAM history.
 * Candidate identities admit our own complete write after a failed fsync or
 * readback, without treating that failure as a successful durable save. */
typedef struct { uint64_t hash; int valid; } bank_identity;
static bank_identity known_banks[2], candidate_banks[2];
static int had_valid_bank;
static uint32_t last_clock_ms, last_sample_ms, segment_ms;
static int sample_started;
static uint32_t step_previous, step_sample_ms;
static int step_have;
/* Energy accounting baselines (active-time deltas since the last 1 Hz accounting), the
 * open series bin, and the all-sports pedometer baseline. Runtime only: after a reboot the
 * partial bin (under one bin width) is dropped and counting resumes from the checkpoint. */
static uint32_t acct_elapsed, acct_move, acct_valid, ped_previous, ped_sample_ms;
static int ped_have;
static struct { uint32_t ms, move_ms, valid_ms, hr_ms; uint64_t hr_sum; } bin;
static struct { uint32_t ms, steps; } cadence[CADENCE_N];
static unsigned cadence_index, cadence_count;
static uint32_t cadence_ms, cadence_steps;
static uint64_t hr_integral;
static uint32_t hr_previous_ms, hr_sample_ms, hr_accepted_ms;
static int hr_previous, hr_have_sample, hr_last_accepted, hr_candidate, hr_candidate_n;
static double gps_lat, gps_lon;
static uint32_t gps_anchor_ms, gps_previous_ms, gps_seen_ms;
static int gps_anchor, gps_previous, gps_seen;
static float display_speed;
static int display_speed_have;
static int prepared_sport=-1;
static uint32_t prepared_ms;

static uint32_t now_ms(void)
{
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time()/1000);
#else
    return hal_millis();
#endif
}
static uint32_t add_time(uint32_t a,uint32_t b)
{ return b>MAX_TIME_MS-a?MAX_TIME_MS:a+b; }
static uint32_t epoch(void)
{
    uint64_t t=hal_epoch_seconds();
    return t>=1577836800ULL && t<=0xffffffffULL?(uint32_t)t:0;
}
static void reject_sample(unsigned reason,uint32_t now)
{
    if(!reason || reason>=WORKOUT_REJECT_COUNT)return;
    if(current.quality.rejected[reason]<UINT16_MAX)current.quality.rejected[reason]++;
    current.quality.last_reason=(uint8_t)reason;current.quality.last_reject_ms=now;
    /* Retain every bounded count, but emit at most one summary per 30 seconds. */
    if(now-quality_log_ms>=CHECKPOINT_MS) {
        quality_log_ms=now;
        diag_log_event(DIAG_WORKOUT,(int32_t)current.id,-(int32_t)reason,current.quality.rejected[reason]);
    }
}
static void io_failure(unsigned operation,int slot,int error)
{
    LOCK();
    storage.last_operation=(uint8_t)operation;storage.last_slot=(int8_t)slot;
    storage.last_errno=error;storage.last_error_ms=now_ms();
    if(operation<WORKOUT_IO_COUNT && storage.operation_errors[operation]<UINT16_MAX)
        storage.operation_errors[operation]++;
    UNLOCK();
    /* One writer attempt is already bounded by RETRY_MS. Negative session ID
     * distinguishes storage operation/slot/errno from sensor summary events. */
    diag_log_event(DIAG_WORKOUT,-(int32_t)operation,slot,error);
}
const workout_profile *workout_profile_get(int sport)
{ return sport>=0 && sport<WORKOUT_SPORT_COUNT?&profiles[sport]:NULL; }
const workout_snapshot *workout_get(void) { return &current; }
static void clear_live(void)
{
    current.bpm=0;current.hr_valid=0;current.speed_mps=current.pace_s_km=0;
    current.speed_valid=current.pace_valid=0;
    current.cadence_spm=0;current.cadence_valid=0;
}
static void clear_gps_live(void)
{
    current.speed_valid=current.pace_valid=0;
    current.speed_mps=current.pace_s_km=0;
}
static void clear_anchors(void)
{
    gps_anchor=gps_previous=gps_seen=0;
    display_speed=0;display_speed_have=0;
    hr_previous=hr_have_sample=hr_last_accepted=hr_candidate_n=0;
    step_have=0;cadence_count=cadence_index=cadence_ms=cadence_steps=0;ped_have=0;
    memset(cadence,0,sizeof cadence);
    clear_live();
}
static void averages(void)
{
    current.avg_bpm=current.hr_active_ms?
        (uint16_t)((hr_integral+current.hr_active_ms/2u)/current.hr_active_ms):0;
    /* Only covered GNSS time contributes: missing fixes are not zero speed. */
    current.avg_speed_mps=current.gps_active_ms?
        current.distance_m*1000.0f/(float)current.gps_active_ms:0;
    current.avg_pace_s_km=current.avg_speed_mps>=0.4f?
        1000.0f/current.avg_speed_mps:0;
}
static void advance_time(uint32_t now)
{
    uint32_t dt=now-last_clock_ms;last_clock_ms=now;
    if(current.state==WORKOUT_RUNNING || current.state==WORKOUT_PAUSED)
        current.total_ms=add_time(current.total_ms,dt);
    if(current.state==WORKOUT_RUNNING)current.elapsed_ms=add_time(current.elapsed_ms,dt);
}
/* ---- energy and movement series (accounted once per ~1 s sample, in active time) ---- */
static int movement_family(int sport)
{
    uint8_t f=profiles[sport].family;
    return f==WORKOUT_FAMILY_RACKET || f==WORKOUT_FAMILY_TEAM || f==WORKOUT_FAMILY_CONDITIONING;
}
static void series_close(void)
{
    if(!bin.ms || current.series_n>=WORKOUT_SERIES_MAX)return;
    unsigned i=current.series_n++;
    current.series_move[i]=bin.valid_ms && bin.valid_ms*2u>=bin.ms?(uint8_t)((uint64_t)bin.move_ms*100u/bin.valid_ms):255;
    current.series_bpm[i]=bin.hr_ms?(uint8_t)(bin.hr_sum/bin.hr_ms):0;
    memset(&bin,0,sizeof bin);
}
/* A full series halves its resolution instead of dropping data: neighbours merge, bins double. */
static void series_compact(void)
{
    unsigned n=current.series_n/2u;
    for(unsigned i=0;i<n;i++) {
        unsigned a=current.series_move[2*i],b=current.series_move[2*i+1];
        current.series_move[i]=(uint8_t)(a==255?b:b==255?a:(a+b+1u)/2u);
        a=current.series_bpm[2*i];b=current.series_bpm[2*i+1];
        current.series_bpm[i]=(uint8_t)(!a?b:!b?a:(a+b+1u)/2u);
    }
    for(unsigned i=n;i<WORKOUT_SERIES_MAX;i++)current.series_move[i]=current.series_bpm[i]=0;
    current.series_n=(uint8_t)n;
    current.series_bin_s=(uint16_t)(current.series_bin_s>32767u?65535u:current.series_bin_s*2u);
}
static void series_add(uint32_t de,uint32_t dm,uint32_t dv,int bpm)
{
    if(!current.series_bin_s)current.series_bin_s=60;
    bin.ms=add_time(bin.ms,de);bin.move_ms+=dm;bin.valid_ms+=dv;
    if(bpm>0) {bin.hr_ms+=de;bin.hr_sum+=(uint64_t)bpm*de;}
    if(bin.ms<(uint32_t)current.series_bin_s*1000u)return;
    if(current.series_n>=WORKOUT_SERIES_MAX) {
        series_compact();
        if(bin.ms<(uint32_t)current.series_bin_s*1000u)return;
    }
    series_close();
}
static void series_flush(void)
{
    /* The final partial bin counts when it covers at least 10 s (or is all there is). */
    if(bin.ms<10000u && current.series_n)return;
    if(current.series_n>=WORKOUT_SERIES_MAX)series_compact();
    series_close();
}
static void account(void)
{
    if(current.elapsed_ms<acct_elapsed)acct_elapsed=current.elapsed_ms;
    uint32_t de=current.elapsed_ms-acct_elapsed;
    if(!de || current.state!=WORKOUT_RUNNING)return;
    uint32_t mv=current.movement_ms,va=current.motion_valid_ms;
    uint32_t dm=mv>=acct_move?mv-acct_move:0,dv=va>=acct_valid?va-acct_valid:0;
    acct_elapsed=current.elapsed_ms;acct_move=mv;acct_valid=va;
    if(dm>dv)dm=dv;
    if(dv>de)dv=de;
    if(dm>dv)dm=dv;
    /* Movement fraction only when valid motion covered at least half of the interval. */
    int pct=dv && dv*2u>=de?(int)((uint64_t)dm*100u/dv):255;
    energy_body body={current.setup.mass_kg,current.body.height_cm,current.body.age_years,current.body.sex};
    int basis=0,bpm=current.hr_valid?current.bpm:0;
    float rate=energy_session_rate(&body,bpm,current.met_tenths,movement_family(current.sport),pct,&basis);
    float minutes=(float)de/60000.0f,net=rate-energy_rest_min(&body);
    current.energy_kcal_est+=rate*minutes;
    if(net>0)current.energy_net_kcal+=net*minutes;
    current.energy_basis=(uint8_t)(current.energy_basis|basis);
    series_add(de,dm,dv,bpm);
}
static void step_baseline_ped(uint32_t now,const hal_activity *a)
{ ped_have=a->valid!=0;ped_previous=a->steps;ped_sample_ms=now; }
static void step_baseline(uint32_t now)
{
    hal_activity a={0};hal_activity_get(&a);
    step_have=a.valid!=0;step_previous=a.steps;step_sample_ms=now;step_baseline_ped(now,&a);
}
/* Pedometer steps taken during the session, for every sport: the daily total subtracts these
 * because the session's own energy already covers them. */
static void sample_pedometer(uint32_t now)
{
    hal_activity a={0};hal_activity_get(&a);
    uint32_t dt=now-ped_sample_ms;ped_sample_ms=now;
    if(!a.valid) {ped_have=0;return;}
    if(ped_have && dt<=GAP_MS && a.steps>=ped_previous) {
        uint32_t d=a.steps-ped_previous;
        if(d<=dt*5u/1000u+3u && UINT32_MAX-current.ped_steps>=d)current.ped_steps+=d;
    }
    ped_previous=a.steps;ped_have=1;
}
static void sample_steps(uint32_t now)
{
    if(!profiles[current.sport].uses_steps)return;
    hal_activity a={0};hal_activity_get(&a);
    uint32_t dt=now-step_sample_ms, delta=0;step_sample_ms=now;
    if(!a.valid) {step_have=0;current.cadence_valid=0;return;}
    /* A midnight/reset decrease establishes a new baseline, never a huge delta.
     * Gaps likewise cannot identify which steps happened inside the interval. */
    if(step_have && dt<=GAP_MS && a.steps>=step_previous) {
        uint32_t d=a.steps-step_previous;
        if(d<=dt*5u/1000u+3u)delta=d; /* reject cache resets/counter spikes */
    }
    step_previous=a.steps;
    if(!step_have || dt>GAP_MS) {
        cadence_count=cadence_index=cadence_ms=cadence_steps=0;
        memset(cadence,0,sizeof cadence);dt=0;
    }
    step_have=1;
    if(UINT32_MAX-current.steps>=delta)current.steps+=delta;
    if(!dt)return;
    if(cadence_count==CADENCE_N) {
        cadence_ms-=cadence[cadence_index].ms;cadence_steps-=cadence[cadence_index].steps;
    } else cadence_count++;
    cadence[cadence_index].ms=dt;cadence[cadence_index].steps=delta;
    cadence_ms+=dt;cadence_steps+=delta;cadence_index=(cadence_index+1u)%CADENCE_N;
    current.cadence_valid=cadence_ms>=5000u;
    current.cadence_spm=current.cadence_valid?60000.0f*(float)cadence_steps/(float)cadence_ms:0;
}
static void sample_heart(uint32_t now)
{
    hal_hr h={0};hal_hr_request(HAL_HR_OWNER_WORKOUT,1);
    unsigned reason=WORKOUT_REJECT_NONE;
    if(!hal_hr_get(&h))reason=WORKOUT_REJECT_HR_MISSING;
    else if(!h.on_wrist)reason=WORKOUT_REJECT_HR_CONTACT;
    else if(!isfinite(h.quality)||h.quality<0.65f||h.quality>1.0f)reason=WORKOUT_REJECT_HR_QUALITY;
    else if(h.bpm<35||h.bpm>230)reason=WORKOUT_REJECT_HR_RANGE;
    else if(now-h.sample_ms>HR_FRESH_MS)reason=WORKOUT_REJECT_HR_STALE;
    else if(now-h.sample_ms>now-segment_ms)reason=WORKOUT_REJECT_HR_BEFORE;
    int ok=reason==WORKOUT_REJECT_NONE;
    /* Counting the same cached acquisition again would fabricate coverage. */
    if(hr_have_sample && now-hr_sample_ms>HR_FRESH_MS)hr_have_sample=0;
    if(ok && hr_have_sample) {
        int32_t order=(int32_t)(h.sample_ms-hr_sample_ms);
        /* Boundary actions may poll between FIFO acquisitions. Preserve the
         * accepted display and interval baseline, without counting it again.
         * A preceding quality failure has already cleared that baseline. */
        if(order==0)return;
        if(order<0) {ok=0;reason=WORKOUT_REJECT_HR_ORDER;}
    }
    if(ok) {hr_sample_ms=h.sample_ms;hr_have_sample=1;}
    if(ok && hr_last_accepted && now-hr_accepted_ms<=10000u &&
       abs(h.bpm-hr_last_accepted)>30) {
        if(hr_candidate_n && abs(h.bpm-hr_candidate)<=8)hr_candidate_n++;
        else {hr_candidate=h.bpm;hr_candidate_n=1;}
        if(hr_candidate_n<3) {ok=0;reason=WORKOUT_REJECT_HR_SPIKE;}
    } else if(ok)hr_candidate_n=0;
    if(!ok) {reject_sample(reason,now);current.bpm=0;current.hr_valid=0;hr_previous=0;return;}
    current.quality.hr_sample_ms=h.sample_ms;
    uint32_t dt=h.sample_ms-hr_previous_ms;
    if(hr_previous && dt<=GAP_MS && now-hr_accepted_ms<=GAP_MS) {
        if(dt>current.elapsed_ms-current.hr_active_ms)dt=current.elapsed_ms-current.hr_active_ms;
        current.hr_active_ms+=dt;
        hr_integral+=(uint64_t)(hr_previous+h.bpm)*dt/2u;
    }
    current.bpm=(uint16_t)h.bpm;current.hr_valid=1;
    if(h.bpm>current.peak_bpm)current.peak_bpm=(uint16_t)h.bpm;
    hr_previous=h.bpm;hr_previous_ms=h.sample_ms;
    hr_last_accepted=h.bpm;hr_accepted_ms=now;
}
static int gps_quality(const hal_gps *g,uint32_t now)
{
    if(!g->valid)return WORKOUT_GPS_WAITING;
    if(g->age_ms>GPS_FRESH_MS)return WORKOUT_GPS_STALE;
    if(!isfinite(g->lat) || !isfinite(g->lon) || g->lat < -90.0 || g->lat > 90.0 ||
       g->lon < -180.0 || g->lon > 180.0 || !isfinite(g->hdop) ||
       g->hdop<0.1f || g->hdop>3.0f || g->sats_used<4)
        return WORKOUT_GPS_POOR;
    (void)now;return WORKOUT_GPS_GOOD;
}
static double separation(double a,double b,double c,double d)
{
    const double r=0.01745329251994329577;
    double x=sin((c-a)*r*0.5),y=sin((d-b)*r*0.5);
    double q=x*x+cos(a*r)*cos(c*r)*y*y;
    if(q<0)q=0;
    if(q>1)q=1;
    return 12742000.0*atan2(sqrt(q),sqrt(1.0-q));
}
static float gps_speed_limit(int sport)
{
    /* Generous sensor outlier bounds, not measured athletic limits. Keep the
     * original walk/hike 5, run 12 and cycle 35 m/s policies. Related new
     * profiles inherit family behavior rather than silently becoming walkers. */
    uint8_t family=profiles[sport].family;
    if(family==WORKOUT_FAMILY_CYCLE)return 35.0f;
    if(sport==WORKOUT_RUN || sport==WORKOUT_TRAIL_RUN ||
       family==WORKOUT_FAMILY_TEAM || family==WORKOUT_FAMILY_PADDLE)return 12.0f;
    if(sport==WORKOUT_RACE_WALK)return 8.0f;
    return 5.0f;
}
static void sample_gps(uint32_t now,int recording)
{
    uint8_t previous_status=current.gps_status;
    hal_gps g={0};hal_gps_enable(1);
    current.gps_status=hal_gps_get(&g)?(uint8_t)gps_quality(&g,now):WORKOUT_GPS_OFF;
    if(!recording)return;
    if(current.gps_status!=WORKOUT_GPS_GOOD) {
        unsigned reasons[]={WORKOUT_REJECT_GPS_MISSING,WORKOUT_REJECT_GPS_FIX,0,
                            WORKOUT_REJECT_GPS_STALE,WORKOUT_REJECT_GPS_QUALITY,WORKOUT_REJECT_GPS_JUMP};
        reject_sample(reasons[current.gps_status],now);
        clear_gps_live();
        gps_anchor=gps_previous=display_speed_have=0;return;
    }
    float limit=gps_speed_limit(current.sport);
    if(!isfinite(g.speed_mps) || g.speed_mps<0 || g.speed_mps>limit) {
        reject_sample(WORKOUT_REJECT_GPS_SPEED,now);
        clear_gps_live();
        current.gps_status=WORKOUT_GPS_REJECTED;gps_anchor=gps_previous=display_speed_have=0;return;
    }
    uint32_t stamp=now-g.age_ms;
    if(now-stamp>now-segment_ms) {reject_sample(WORKOUT_REJECT_GPS_BEFORE,now);clear_gps_live();return;}
    if(gps_seen && now-gps_seen_ms>GAP_MS) {
        gps_seen=gps_anchor=gps_previous=display_speed_have=0;
    }
    if(gps_seen) {
        int32_t order=(int32_t)(stamp-gps_seen_ms);
        if(order==0) {
            /* Same fresh fix during LAP/FINISH: retain its accepted display,
             * without repeating distance, coverage or speed-filter updates. */
            current.gps_status=previous_status;return;
        }
        if(order<0) {
            reject_sample(WORKOUT_REJECT_GPS_ORDER,now);
            clear_gps_live();gps_anchor=gps_previous=display_speed_have=0;
            current.gps_status=WORKOUT_GPS_REJECTED;return;
        }
    }
    clear_gps_live();
    gps_seen=1;gps_seen_ms=stamp;
    uint32_t dt=stamp-gps_previous_ms;
    if(!gps_anchor || !gps_previous || dt>GAP_MS) {
        current.quality.gps_sample_ms=stamp;
        gps_lat=g.lat;gps_lon=g.lon;gps_anchor_ms=stamp;
        gps_previous_ms=stamp;gps_anchor=gps_previous=1;display_speed_have=0;return;
    }
    uint32_t anchor_dt=stamp-gps_anchor_ms;
    double distance=separation(gps_lat,gps_lon,g.lat,g.lon);
    if(!anchor_dt || distance>(double)limit*anchor_dt/1000.0 ||
       distance>(double)g.speed_mps*anchor_dt*0.003+8.0) {
        reject_sample(WORKOUT_REJECT_GPS_JUMP,now);
        current.gps_status=WORKOUT_GPS_REJECTED;gps_anchor=gps_previous=display_speed_have=0;return;
    }
    current.quality.gps_sample_ms=stamp;
    if(dt>current.elapsed_ms-current.gps_active_ms)dt=current.elapsed_ms-current.gps_active_ms;
    current.gps_active_ms+=dt;
    current.distance_valid=1;gps_previous_ms=stamp;
    /* Doppler speed gates standing noise. Accumulate sub-1.5m displacements
     * against one anchor, rather than dropping every slow walking increment. */
    if(g.speed_mps<0.55f) {
        gps_lat=g.lat;gps_lon=g.lon;gps_anchor_ms=stamp;
        display_speed=0;display_speed_have=1;current.speed_valid=1;current.speed_mps=0;return;
    }
    if(distance>=1.5) {
        current.distance_m+=(float)distance;
        gps_lat=g.lat;gps_lon=g.lon;gps_anchor_ms=stamp;
    } else if(anchor_dt>=GAP_MS) {
        gps_lat=g.lat;gps_lon=g.lon;gps_anchor_ms=stamp;
    }
    /* A short (~3s) display-only low-pass prevents reciprocal pace flicker.
     * It never feeds the accumulated position distance. */
    float alpha=(float)dt/(3000.0f+(float)dt);
    display_speed=display_speed_have?display_speed+alpha*(g.speed_mps-display_speed):g.speed_mps;
    display_speed_have=1;current.speed_mps=display_speed;current.speed_valid=1;
    if(display_speed>=0.4f) {current.pace_s_km=1000.0f/display_speed;current.pace_valid=1;}
}
static void metrics(uint32_t now)
{
    sample_pedometer(now);sample_steps(now);sample_heart(now);
    if(profiles[current.sport].outdoor)sample_gps(now,1);
    else current.gps_status=WORKOUT_GPS_OFF;
    averages();last_sample_ms=now;sample_started=1;
    account();
}

static uint32_t disk_checksum(const disk_record *p)
{
    const unsigned char *s=(const unsigned char *)p;uint32_t h=2166136261u;
    for(size_t i=0;i<offsetof(disk_record,checksum);i++)h=(h^s[i])*16777619u;
    return h;
}
static uint64_t bank_hash(const disk_record *p)
{
    const unsigned char *s=(const unsigned char *)p;
    uint64_t h=UINT64_C(14695981039346656037);
    for(size_t i=0;i<sizeof *p;i++)h=(h^s[i])*UINT64_C(1099511628211);
    return h;
}
static int valid_snapshot(const workout_snapshot *s,int historical)
{
    if(s->quality.last_reason>=WORKOUT_REJECT_COUNT || !setup_valid(&s->setup) || s->motion_valid_ms>s->elapsed_ms ||
       s->movement_ms>s->motion_valid_ms || !isfinite(s->energy_kcal_est) ||
       s->energy_kcal_est<0 || s->sport>=WORKOUT_SPORT_COUNT || s->state>WORKOUT_FINISHED ||
       s->save_state>WORKOUT_SAVE_FAILED || s->gps_status>WORKOUT_GPS_REJECTED ||
       s->distance_valid>1 || s->speed_valid>1 || s->pace_valid>1 ||
       s->hr_valid>1 || s->cadence_valid>1 || s->recovered>1 ||
       s->nlaps>WORKOUT_LAPS_MAX || s->elapsed_ms>MAX_TIME_MS ||
       s->total_ms>MAX_TIME_MS || s->elapsed_ms>s->total_ms ||
       s->hr_active_ms>s->elapsed_ms || s->gps_active_ms>s->elapsed_ms ||
       s->bpm>230 || s->avg_bpm>230 || s->peak_bpm>230 ||
       !isfinite(s->distance_m) || s->distance_m<0 || s->distance_m>10000000.0f ||
       !isfinite(s->speed_mps) || s->speed_mps<0 || s->speed_mps>35 ||
       !isfinite(s->avg_speed_mps) || s->avg_speed_mps<0 || s->avg_speed_mps>35 ||
       !isfinite(s->pace_s_km) || s->pace_s_km<0 || s->pace_s_km>36000 ||
       !isfinite(s->avg_pace_s_km) || s->avg_pace_s_km<0 || s->avg_pace_s_km>36000 ||
       !isfinite(s->cadence_spm) || s->cadence_spm<0 || s->cadence_spm>500)
        return 0;
    if(!isfinite(s->energy_net_kcal) || s->energy_net_kcal<0 || s->energy_net_kcal>s->energy_kcal_est+.5f ||
       s->body.sex>2 || s->energy_basis>=32 || s->series_n>WORKOUT_SERIES_MAX ||
       (s->series_n && !s->series_bin_s))return 0;
    for(unsigned i=0;i<s->series_n;i++)
        if((s->series_move[i]>100 && s->series_move[i]!=255) || s->series_bpm[i]>230)return 0;
    if(s->state!=WORKOUT_IDLE && !s->id)return 0;
    if(historical && (s->state!=WORKOUT_FINISHED || s->save_state!=WORKOUT_SAVE_SAVED))return 0;
    for(unsigned i=0;i<s->nlaps;i++)if(s->laps[i].elapsed_ms>s->elapsed_ms ||
       (i && s->laps[i].elapsed_ms<s->laps[i-1].elapsed_ms) ||
       !isfinite(s->laps[i].distance_m) || s->laps[i].distance_m<0 ||
       s->laps[i].distance_m>s->distance_m ||
       (i && s->laps[i].distance_m<s->laps[i-1].distance_m))return 0;
    return 1;
}
static int valid_disk(const disk_record *p)
{
    if(p->magic!=DISK_MAGIC || p->version!=DISK_VERSION || p->bytes!=sizeof *p ||
       p->count>WORKOUT_HISTORY_MAX || !p->next_id || p->checksum!=disk_checksum(p) ||
       !valid_snapshot(&p->session,0) ||
       p->hr_integral>(uint64_t)230*p->session.hr_active_ms)return 0;
    for(unsigned i=0;i<p->count;i++) {
        if(!valid_snapshot(&p->history[i],1))return 0;
        for(unsigned j=0;j<i;j++)if(p->history[i].id==p->history[j].id)return 0;
    }
    return 1;
}
static int file_path(char *path,size_t n,int slot)
{ int r=snprintf(path,n,"%s/workouts.%d.bin",storage_directory,slot);return r>0 && (size_t)r<n; }
static int corrupt_bank(int slot)
{ io_failure(WORKOUT_IO_CORRUPT,slot,0);return -1; }
/* 1 valid, 0 absent, -1 confirmed corrupt, -2 unresolved I/O. An unreadable
 * bank may be the newest durable generation: never recover past or overwrite it. */
static int read_slot_status(int slot,disk_record *p)
{
    char path[256];if(!file_path(path,sizeof path,slot)) {io_failure(WORKOUT_IO_READ_OPEN,slot,ENAMETOOLONG);return -2;}
    FILE *f=fopen(path,"rb");
    if(!f) {
        int error=errno;if(error==ENOENT)return 0;
        io_failure(WORKOUT_IO_READ_OPEN,slot,error);return -2;
    }
    memset(p,0,sizeof *p);
    errno=0;size_t n=fread(p,1,sizeof *p,f);int tail=fgetc(f);
    int error=ferror(f)?(errno?errno:EIO):0;
    if(fclose(f)) {io_failure(WORKOUT_IO_READ_CLOSE,slot,errno?errno:EIO);return -2;}
    if(error) {io_failure(WORKOUT_IO_READ,slot,error);return -2;}
    if(tail!=EOF)return corrupt_bank(slot);
    if((n==WORKOUT_DISK_V1_BYTES && p->version==1) ||
       (n==WORKOUT_DISK_V2_BYTES && p->version==2) ||
       (n==WORKOUT_DISK_V3_BYTES && p->version==3)) {
        /* Validate the original generation before backwards in-place expansion.
         * The original bank remains unchanged; V1 resumes conservatively paused. */
        unsigned version=p->version;
        unsigned snapshot_bytes=version==1?WORKOUT_SNAPSHOT_V1_BYTES:version==2?WORKOUT_SNAPSHOT_V2_BYTES:WORKOUT_SNAPSHOT_V3_BYTES;
        unsigned checksum_offset=version==1?WORKOUT_DISK_V1_CHECKSUM_OFFSET:version==2?WORKOUT_DISK_V2_CHECKSUM_OFFSET:WORKOUT_DISK_V3_CHECKSUM_OFFSET;
        if(p->magic!=DISK_MAGIC || p->bytes!=n)return corrupt_bank(slot);
        unsigned char *raw=(unsigned char *)p;uint32_t h=2166136261u,old;
        memcpy(&old,raw+checksum_offset,4);
        for(size_t i=0;i<checksum_offset;i++)h=(h^raw[i])*16777619u;
        if(old!=h)return corrupt_bank(slot);
        for(int i=WORKOUT_HISTORY_MAX;i>=0;i--) {
            unsigned char old_snapshot[WORKOUT_SNAPSHOT_V3_BYTES];
            memcpy(old_snapshot,raw+32u+(unsigned)i*snapshot_bytes,snapshot_bytes);
            workout_snapshot *dest=i?&p->history[i-1]:&p->session;
            memset(dest,0,sizeof *dest);memcpy(dest,old_snapshot,snapshot_bytes);
            if(version==1 && dest->sport>=6)return corrupt_bank(slot);
        }
        p->version=DISK_VERSION;p->bytes=sizeof *p;p->checksum=disk_checksum(p);
    } else if(n!=sizeof *p)return corrupt_bank(slot);
    return valid_disk(p)?1:corrupt_bank(slot);
}
static int read_slot(int slot,disk_record *p) { return read_slot_status(slot,p)==1; }
static int newest_slot(disk_record *a,disk_record *b,int state[2])
{
    int sa=read_slot_status(0,a),sb=read_slot_status(1,b);
    if(state) {state[0]=sa;state[1]=sb;}
    if(sa==-2 || sb==-2)return -3;
    int va=sa==1,vb=sb==1;
    if(!va)return vb?1:(sa<0 || sb<0?-2:-1);
    if(!vb)return 0;
    return (int32_t)(b->sequence-a->sequence)>0?1:0;
}
static int make_directory(void)
{
    char path[sizeof storage_directory];strcpy(path,storage_directory);
    for(size_t i=1;path[i];i++)if((path[i]=='/' || path[i]=='\\') && path[i-1]!=':') {
        char c=path[i];path[i]=0;
        if(MKDIR(path)!=0 && errno!=EEXIST)return 0;
        path[i]=c;
    }
    return MKDIR(path)==0 || errno==EEXIST;
}
static int storage_lock(void)
{
#ifndef ESP_PLATFORM
    hal_storage s={0};hal_storage_get(&s);if(!s.mounted)return 0;
#endif
    return hal_sports_sd_lock();
}
static int write_checkpoint(disk_record *next,disk_record *scratch,int *foreign,int *uncertain)
{
    /* Determine the target without overwriting the intact latest bank. */
    uint32_t sequence=0;int target=0;
    int ast=read_slot_status(0,scratch),a=ast==1;uint32_t sa=a?scratch->sequence:0;
    uint64_t ha=a?bank_hash(scratch):0;
    int bst=read_slot_status(1,scratch),b=bst==1;uint32_t sb=b?scratch->sequence:0;
    uint64_t hb=b?bank_hash(scratch):0;
    if(ast==-2 || bst==-2) {*uncertain=1;return 0;}
    int valid[2]={a,b};uint64_t hashes[2]={ha,hb};
    for(int i=0;i<2;i++)if(valid[i] &&
       !(known_banks[i].valid && known_banks[i].hash==hashes[i]) &&
       !(candidate_banks[i].valid && candidate_banks[i].hash==hashes[i])) {
        *foreign=1;return 0;
    }
    if(!a && !b && had_valid_bank) { *foreign=1;return 0; }
    /* Observed candidates are ours even if their preceding fsync failed.
     * Remember them before a retry replaces the candidate for that slot. */
    for(int i=0;i<2;i++)if(valid[i]) {
        known_banks[i]=(bank_identity){hashes[i],1};candidate_banks[i].valid=0;
        had_valid_bank=1;
    }
    if(!a && !b && (ast<0 || bst<0)) {
        if(failed_pristine_slot==0 && ast<0 && bst==0)target=0;
        else if(failed_pristine_slot==1 && bst<0 && ast==0)target=1;
        else return 0;
    }
    if(a || b) {
        int latest=!a?1:!b?0:(int32_t)(sb-sa)>0?1:0;
        sequence=(latest?sb:sa);target=1-latest;
    }
    next->sequence=sequence+1u;next->checksum=disk_checksum(next);
    char path[256];if(!file_path(path,sizeof path,target)) {io_failure(WORKOUT_IO_WRITE_OPEN,target,ENAMETOOLONG);return 0;}
    if(!a && !b)failed_pristine_slot=target;
    FILE *f=fopen(path,"wb");if(!f) {io_failure(WORKOUT_IO_WRITE_OPEN,target,errno);return 0;}
    candidate_banks[target]=(bank_identity){bank_hash(next),1};
    int ok=fwrite(next,1,sizeof *next,f)==sizeof *next;
    if(!ok)io_failure(WORKOUT_IO_WRITE,target,errno?errno:EIO);
    if(fflush(f)) {io_failure(WORKOUT_IO_FLUSH,target,errno?errno:EIO);ok=0;}
    if(SYNC(fileno(f))) {io_failure(WORKOUT_IO_SYNC,target,errno?errno:EIO);ok=0;}
    if(fclose(f)) {io_failure(WORKOUT_IO_WRITE_CLOSE,target,errno?errno:EIO);ok=0;}
    int verified=read_slot(target,scratch) && !memcmp(next,scratch,sizeof *next);
    if(!verified)io_failure(WORKOUT_IO_VERIFY,target,EIO);
    if(verified) {
        known_banks[target]=candidate_banks[target];candidate_banks[target].valid=0;
        had_valid_bank=1;
    }
    ok=ok && verified;
    if(ok)failed_pristine_slot=-1;
    return ok;
}
#ifdef ESP_PLATFORM
static volatile TaskHandle_t writer_handle;
#endif
static void checkpoint_request(void)
{
    last_checkpoint_ms=now_ms();
    LOCK();revision++;storage.dirty=1;force_write=1;
    if(io) {
        disk_record *p=&io->pending;memset(p,0,sizeof *p);
        p->magic=DISK_MAGIC;p->version=DISK_VERSION;p->bytes=sizeof *p;
        p->count=history_count;p->next_id=next_id;p->hr_integral=hr_integral;
        p->session=current;memcpy(p->history,history,sizeof history);
        if(p->session.state==WORKOUT_FINISHED)p->session.save_state=WORKOUT_SAVE_SAVED;
        for(unsigned i=0;i<p->count;i++)p->history[i].save_state=WORKOUT_SAVE_SAVED;
    }
    UNLOCK();
#ifdef ESP_PLATFORM
    /* The writer sleeps between requests (light sleep); wake it for the new record. */
    TaskHandle_t writer=writer_handle;
    if(writer)xTaskNotifyGive(writer);
#endif
}
static void apply_storage(void)
{
    int recovered=0;
    LOCK();
    if(load_pending && io) {
        disk_record *p=&io->work;
        current=p->session;history_count=p->count;next_id=p->next_id;
        memcpy(history,p->history,sizeof history);hr_integral=p->hr_integral;
        if(current.state==WORKOUT_RUNNING || current.state==WORKOUT_PAUSED) {
            if(current.state==WORKOUT_RUNNING && !current.setup.auto_resume)current.state=WORKOUT_PAUSED;
            current.recovered=1;if(current.recovery_count<UINT32_MAX)current.recovery_count++;
            /* Old producer stamps belong to the previous boot. Preserve reason
             * counters, explicitly leave timing unknown until this boot samples. */
            current.quality.hr_sample_ms=current.quality.gps_sample_ms=0;
            current.quality.motion_sample_ms=current.quality.last_reject_ms=0;
            current.save_state=WORKOUT_SAVE_NONE;clear_anchors();recovered=1;
            current.gps_status=profiles[current.sport].outdoor?WORKOUT_GPS_WAITING:WORKOUT_GPS_OFF;
            workout_motion_reset(&motion);motion.valid_ms=current.motion_valid_ms;
            motion.movement_ms=current.movement_ms;motion.bursts=current.movement_bursts;
            motion.invalid=current.quality.rejected[WORKOUT_REJECT_MOTION_INVALID];
            motion.saturated=current.quality.rejected[WORKOUT_REJECT_MOTION_SATURATED];
            motion.gaps=current.quality.rejected[WORKOUT_REJECT_MOTION_GAP];
            motion_enabled=current.state==WORKOUT_RUNNING;motion_start_ms=now_ms();
            acct_elapsed=current.elapsed_ms;acct_move=current.movement_ms;acct_valid=current.motion_valid_ms;
            memset(&bin,0,sizeof bin);
        }
        last_clock_ms=now_ms();last_sample_ms=last_clock_ms;last_checkpoint_ms=last_clock_ms;
        sample_started=0;load_pending=0;storage.ready=1;
    }
    if(ack_pending) {
        if(ack_success) {
            if(ack_revision==revision) {
                storage.dirty=0;saved_revision=ack_revision;
                have_evicted=0;
                if(current.state==WORKOUT_FINISHED)current.save_state=WORKOUT_SAVE_SAVED;
                for(unsigned i=0;i<history_count;i++)history[i].save_state=WORKOUT_SAVE_SAVED;
            }
        } else {
            if(ack_revision==revision && current.state==WORKOUT_FINISHED) {
                current.save_state=WORKOUT_SAVE_FAILED;
                if(history_count && history[0].id==current.id)history[0].save_state=WORKOUT_SAVE_FAILED;
            }
        }
        ack_pending=0;
    }
    UNLOCK();
    if(recovered) {
        if(current.state==WORKOUT_RUNNING) {
            segment_ms=now_ms();step_baseline(segment_ms);hal_hr_request(HAL_HR_OWNER_WORKOUT,1);
            if(profiles[current.sport].outdoor)hal_gps_enable(1);
        }
        diag_log_event(DIAG_WORKOUT_RECOVERY,(int32_t)current.id,(int32_t)current.recovery_count,current.state);
        checkpoint_request();
    }
}
int workout_storage_set_directory(const char *directory)
{
#ifdef ESP_PLATFORM
    (void)directory;return 0;
#else
    if(initialized || !directory || !*directory || strlen(directory)>=sizeof storage_directory)return 0;
    strcpy(storage_directory,directory);return 1;
#endif
}
void workout_storage_service(void)
{
    if(!initialized || !io)return;
    uint32_t now=now_ms(),request=0;
    LOCK();
    if(storage.busy || load_pending || (loaded && !storage.dirty) ||
       (attempted && !force_write && now-last_attempt_ms<RETRY_MS)) {UNLOCK();return;}
    storage.busy=1;force_write=0;last_attempt_ms=now;attempted=1;
    int loading=!loaded;
    if(!loading) {io->work=io->pending;request=revision;}
    UNLOCK();
    int locked=storage_lock(),ok=0,corrupt=0,foreign=0,uncertain=0;
    if(locked) {
        if(make_directory()) {
            if(loading) {
                int state[2],slot=newest_slot(&io->work,&io->verify,state);
                if(slot>=0) {
                    if(state[0]==1)known_banks[0]=(bank_identity){bank_hash(&io->work),1};
                    if(state[1]==1)known_banks[1]=(bank_identity){bank_hash(&io->verify),1};
                    had_valid_bank=1;
                }
                if(slot==1)io->work=io->verify;
                if(slot==-1) {
                    memset(&io->work,0,sizeof io->work);
                    io->work.magic=DISK_MAGIC;io->work.version=DISK_VERSION;
                    io->work.bytes=sizeof io->work;io->work.next_id=1;
                }
                corrupt=slot==-2;uncertain=slot==-3;ok=slot>=-1;
                if(corrupt)io_failure(WORKOUT_IO_CORRUPT,-1,0);
            } else ok=write_checkpoint(&io->work,&io->verify,&foreign,&uncertain);
        } else io_failure(WORKOUT_IO_DIRECTORY,-1,errno);
        hal_sports_sd_unlock();
    } else io_failure(WORKOUT_IO_LOCK,-1,errno);
    LOCK();storage.busy=0;storage.available=(uint8_t)ok;
    storage.corrupt=(uint8_t)corrupt;
    storage.foreign=(uint8_t)foreign;
    storage.uncertain=(uint8_t)uncertain;
    if(!ok)storage.errors++;
    if(loading) {if(ok) {loaded=1;load_pending=1;}}
    else {ack_revision=request;ack_success=ok;ack_pending=1;}
    UNLOCK();
}
#ifdef ESP_PLATFORM
static void writer_task(void *arg)
{
    /* Event driven: checkpoint_request notifies. The 2 s timeout covers storage retry
     * pacing (RETRY_MS) and the first load without a 2 Hz wakeup while the watch idles. */
    (void)arg;
    for(;;) {workout_storage_service();ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(2000));}
}
#endif
static void writer_ensure(void)
{
#ifdef ESP_PLATFORM
    uint32_t now=now_ms();
    if(io || (worker_attempted && now-worker_attempt_ms<RETRY_MS))return;
    worker_attempted=1;worker_attempt_ms=now;
    io=heap_caps_calloc(1,sizeof *io,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!io) {
        storage.worker_failed=1;storage.errors++;
        io_failure(WORKOUT_IO_ALLOC,-1,ENOMEM);return;
    }
    if(xTaskCreatePinnedToCoreWithCaps(writer_task,"workout_sd",6144,NULL,2,(TaskHandle_t *)&writer_handle,
                                     tskNO_AFFINITY,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS) {
        heap_caps_free(io);io=NULL;storage.worker_failed=1;storage.errors++;
        io_failure(WORKOUT_IO_TASK,-1,ENOMEM);return;
    }
    storage.worker_failed=0;
#endif
}
void workout_init(void)
{
    if(initialized) {writer_ensure();return;}
    motion_enabled=0;
    memset(known_banks,0,sizeof known_banks);memset(candidate_banks,0,sizeof candidate_banks);
    had_valid_bank=0;failed_pristine_slot=-1;
    last_clock_ms=last_sample_ms=last_checkpoint_ms=now_ms();
    worker_attempted=0;worker_attempt_ms=0;
    initialized=1;
    writer_ensure();
}
void workout_prepare(int sport)
{ if(workout_profile_get(sport)) {prepared_sport=sport;prepared_ms=now_ms();} }
void workout_unprepare(void) { prepared_sport=-1; }
void workout_tick(void)
{
    workout_init();
    apply_storage();uint32_t now=now_ms();advance_time(now);
    if(current.state==WORKOUT_RUNNING) {
        motion_sync();
        if(!sample_started || now-last_sample_ms>=SAMPLE_MS)metrics(now);
        if(now-last_checkpoint_ms>=CHECKPOINT_MS)checkpoint_request();
    } else if(prepared_sport>=0 && now-prepared_ms<2000u &&
              profiles[prepared_sport].outdoor &&
              (!sample_started || now-last_sample_ms>=SAMPLE_MS)) {
        sample_gps(now,0);last_sample_ms=now;sample_started=1;
    }
}
int workout_start(int sport)
{
    if(!nos_runtime_recording_allowed()) return 0;
    workout_tick();
    if(!workout_profile_get(sport) || !storage.ready || current.state==WORKOUT_RUNNING ||
       current.state==WORKOUT_PAUSED || (current.state==WORKOUT_FINISHED &&
       current.save_state!=WORKOUT_SAVE_SAVED))return 0;
    motion_boundary(0);LOCK();workout_motion_reset(&motion);UNLOCK();
    memset(&current,0,sizeof current);current.sport=(uint8_t)sport;current.setup=next_setup;current.met_tenths=profiles[sport].met_tenths;
    current.body=next_body;current.series_bin_s=60;
    {
        energy_body eb={current.setup.mass_kg,current.body.height_cm,current.body.age_years,current.body.sex};
        int assumed=energy_assumed(&eb);
        current.energy_basis=(uint8_t)((assumed&ENERGY_ASSUME_MASS?ENERGY_BASIS_ASSUMED_MASS:0)|
                                       (assumed&ENERGY_ASSUME_BODY?ENERGY_BASIS_ASSUMED_BODY:0));
    }
    acct_elapsed=acct_move=acct_valid=0;memset(&bin,0,sizeof bin);
    current.placement_revision=next_placement_revision;
    current.id=next_id++;if(!next_id)next_id=1;
    current.state=WORKOUT_RUNNING;current.started_epoch=epoch();
    current.gps_status=profiles[sport].outdoor?WORKOUT_GPS_WAITING:WORKOUT_GPS_OFF;
    hr_integral=0;clear_anchors();
    uint32_t now=now_ms();last_clock_ms=segment_ms=last_sample_ms=now;
    sample_started=1;step_baseline(now);prepared_sport=-1;
    if(profiles[sport].outdoor)hal_gps_enable(1);
    hal_hr_request(HAL_HR_OWNER_WORKOUT,1);motion_boundary(1);diag_log_event(DIAG_WORKOUT,(int32_t)current.id,current.state,current.sport);checkpoint_request();return 1;
}
static void boundary_metrics(uint32_t now)
{
    advance_time(now);motion_sync();
    if(current.state==WORKOUT_RUNNING && (!sample_started || now-last_sample_ms>0))metrics(now);
}
int workout_pause(void)
{
    if(current.state!=WORKOUT_RUNNING)return 0;
    motion_boundary(0);boundary_metrics(now_ms());current.state=WORKOUT_PAUSED;clear_anchors();
    hal_hr_request(HAL_HR_OWNER_WORKOUT,0);
    diag_log_event(DIAG_WORKOUT,(int32_t)current.id,current.state,current.sport);
    checkpoint_request();return 1;
}
int workout_resume(void)
{
    if(current.state!=WORKOUT_PAUSED)return 0;
    uint32_t now=now_ms();advance_time(now);current.state=WORKOUT_RUNNING;
    clear_anchors();last_sample_ms=segment_ms=now;sample_started=1;step_baseline(now);
    if(profiles[current.sport].outdoor)hal_gps_enable(1);
    hal_hr_request(HAL_HR_OWNER_WORKOUT,1);motion_boundary(1);diag_log_event(DIAG_WORKOUT,(int32_t)current.id,current.state,current.sport);checkpoint_request();return 1;
}
int workout_lap_add(void)
{
    if(current.state!=WORKOUT_RUNNING || current.nlaps>=WORKOUT_LAPS_MAX)return 0;
    boundary_metrics(now_ms());
    if(!current.elapsed_ms || (current.nlaps &&
       current.laps[current.nlaps-1].elapsed_ms==current.elapsed_ms))return 0;
    current.laps[current.nlaps++]=(workout_lap){current.elapsed_ms,current.distance_m};
    checkpoint_request();return 1;
}
int workout_finish(void)
{
    if(current.state!=WORKOUT_RUNNING && current.state!=WORKOUT_PAUSED)return 0;
    motion_boundary(0);boundary_metrics(now_ms());series_flush();
    if(current.nlaps<WORKOUT_LAPS_MAX && current.elapsed_ms &&
       (!current.nlaps || current.laps[current.nlaps-1].elapsed_ms!=current.elapsed_ms))
        current.laps[current.nlaps++]=(workout_lap){current.elapsed_ms,current.distance_m};
    current.state=WORKOUT_FINISHED;current.ended_epoch=epoch();
    hal_hr_request(HAL_HR_OWNER_WORKOUT,0);
    diag_log_event(DIAG_WORKOUT,(int32_t)current.id,current.state,current.sport);
    clear_live();current.save_state=WORKOUT_SAVE_PENDING;
    have_evicted=history_count==WORKOUT_HISTORY_MAX;
    if(have_evicted)evicted=history[WORKOUT_HISTORY_MAX-1];
    if(history_count<WORKOUT_HISTORY_MAX)history_count++;
    memmove(history+1,history,(history_count-1u)*sizeof history[0]);history[0]=current;
    checkpoint_request();return 1;
}
int workout_discard(void)
{
    if(current.state==WORKOUT_IDLE)return 0;
    if(current.state==WORKOUT_FINISHED && current.save_state!=WORKOUT_SAVE_SAVED &&
       history_count && history[0].id==current.id) {
        history_count--;memmove(history,history+1,history_count*sizeof history[0]);
        memset(&history[history_count],0,sizeof history[0]);
        if(have_evicted) {history[history_count++]=evicted;have_evicted=0;}
    }
    motion_boundary(0);memset(&current,0,sizeof current);hr_integral=0;clear_anchors();
    hal_hr_request(HAL_HR_OWNER_WORKOUT,0);
    checkpoint_request();return 1;
}
void workout_retry_save(void)
{
    worker_attempted=0;workout_init();
    LOCK();force_write=1;UNLOCK();
    if(current.state==WORKOUT_FINISHED && current.save_state==WORKOUT_SAVE_FAILED)
        current.save_state=WORKOUT_SAVE_PENDING;
}
int workout_history_count(void) { return (int)history_count; }
/* Pedometer steps and net energy of every session started at or after `since_epoch` (local
 * midnight), for the daily totals: the live/unsaved session counts once even after it is also
 * in history. Sessions from older formats carry no net energy and add nothing. */
int workout_day_energy(uint32_t since_epoch,uint32_t *ped_steps,float *net_kcal)
{
    uint32_t steps=0;float net=0;int n=0;
    for(unsigned i=0;i<history_count && i<WORKOUT_HISTORY_MAX;i++) {
        const workout_snapshot *w=&history[i];
        if(!w->id || !w->started_epoch || w->started_epoch<since_epoch)continue;
        steps=UINT32_MAX-steps<w->ped_steps?UINT32_MAX:steps+w->ped_steps;net+=w->energy_net_kcal;n++;
    }
    if(current.state!=WORKOUT_IDLE && current.id && current.started_epoch && current.started_epoch>=since_epoch) {
        int dup=0;for(unsigned i=0;i<history_count && i<WORKOUT_HISTORY_MAX;i++)dup|=history[i].id==current.id;
        if(!dup) {steps=UINT32_MAX-steps<current.ped_steps?UINT32_MAX:steps+current.ped_steps;net+=current.energy_net_kcal;n++;}
    }
    if(ped_steps)*ped_steps=steps;
    if(net_kcal)*net_kcal=net;
    return n;
}int workout_history_get(int offset,workout_snapshot *out)
{
    if(!out || offset<0 || (unsigned)offset>=WORKOUT_HISTORY_MAX ||
       (unsigned)offset>=history_count)return 0;
    *out=history[offset];return 1;
}
void workout_storage_get(workout_storage_status *out)
{ if(out) {LOCK();*out=storage;UNLOCK();} }
