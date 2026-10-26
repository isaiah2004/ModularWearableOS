/* Background workout recorder. Public getters never perform filesystem I/O.
 * Call init once and tick from the services loop, including display-off ticks.
 * Times and sensor heuristics are engineering estimates, not clinical metrics. */
#ifndef NOS_WORKOUT_H
#define NOS_WORKOUT_H
#include <stdint.h>
#include "hal.h"

#define WORKOUT_LAPS_MAX 20
#define WORKOUT_HISTORY_MAX 12
/* Versioned SD ABI (little endian, IEEE-754 float32). A format or snapshot
 * layout change must bump the disk version; checks in workout.c enforce it. */
#define WORKOUT_DISK_VERSION 4u
#define WORKOUT_SNAPSHOT_V1_BYTES 236u
#define WORKOUT_SNAPSHOT_V2_BYTES 264u
#define WORKOUT_DISK_V2_BYTES 3472u
#define WORKOUT_DISK_V2_CHECKSUM_OFFSET 3464u
#define WORKOUT_DISK_V1_BYTES 3104u
#define WORKOUT_DISK_V1_CHECKSUM_OFFSET 3100u
#define WORKOUT_SNAPSHOT_V3_BYTES 328u
#define WORKOUT_DISK_V3_BYTES 4304u
#define WORKOUT_DISK_V3_CHECKSUM_OFFSET 4296u
/* V4 appends the body/energy basis and a movement series to every snapshot. A V3 bank
 * is expanded on read (zeroed extension) and the original bytes stay on the card. */
#define WORKOUT_SERIES_MAX 120
#define WORKOUT_SNAPSHOT_V4_BYTES 584u
#define WORKOUT_DISK_V4_BYTES 7632u
#define WORKOUT_DISK_V4_CHECKSUM_OFFSET 7624u
enum { WORKOUT_WALK, WORKOUT_RUN, WORKOUT_CYCLE, WORKOUT_HIKE,
       WORKOUT_TREADMILL, WORKOUT_GENERAL, WORKOUT_FOOTBALL, WORKOUT_CRICKET,
       WORKOUT_BADMINTON, WORKOUT_TRAIL_RUN, WORKOUT_TREK, WORKOUT_RACE_WALK,
       WORKOUT_NORDIC_WALK, WORKOUT_INDOOR_WALK, WORKOUT_STAIRS, WORKOUT_MTB,
       WORKOUT_GRAVEL, WORKOUT_INDOOR_CYCLE, WORKOUT_TENNIS, WORKOUT_TABLE_TENNIS,
       WORKOUT_SQUASH, WORKOUT_PICKLEBALL, WORKOUT_PADEL, WORKOUT_BASKETBALL,
       WORKOUT_VOLLEYBALL, WORKOUT_HANDBALL, WORKOUT_HOCKEY, WORKOUT_RUGBY,
       WORKOUT_STRENGTH, WORKOUT_HIIT, WORKOUT_CIRCUIT, WORKOUT_AEROBICS,
       WORKOUT_DANCE, WORKOUT_JUMP_ROPE, WORKOUT_BOXING, WORKOUT_YOGA,
       WORKOUT_PILATES, WORKOUT_STRETCHING, WORKOUT_ROWING, WORKOUT_KAYAK,
       WORKOUT_SPORT_COUNT };
enum { WORKOUT_IDLE, WORKOUT_RUNNING, WORKOUT_PAUSED, WORKOUT_FINISHED };
enum { WORKOUT_SAVE_NONE, WORKOUT_SAVE_PENDING, WORKOUT_SAVE_SAVED, WORKOUT_SAVE_FAILED };
enum { WORKOUT_GPS_OFF, WORKOUT_GPS_WAITING, WORKOUT_GPS_GOOD,
       WORKOUT_GPS_STALE, WORKOUT_GPS_POOR, WORKOUT_GPS_REJECTED };
typedef struct {
    const char *name;
    const char *subtitle;
    uint8_t outdoor, uses_steps, show_speed;
    uint8_t family, met_tenths; /* reference category only; zero means unspecified */
} workout_profile;
enum { HAND_UNKNOWN, HAND_LEFT, HAND_RIGHT, HAND_BOTH };
enum { PLACEMENT_UNKNOWN, PLACEMENT_USER, PLACEMENT_CALIBRATED };
enum { WORKOUT_FAMILY_LOCOMOTION, WORKOUT_FAMILY_INDOOR, WORKOUT_FAMILY_CYCLE,
       WORKOUT_FAMILY_RACKET, WORKOUT_FAMILY_TEAM, WORKOUT_FAMILY_CONDITIONING,
       WORKOUT_FAMILY_CONTROLLED, WORKOUT_FAMILY_PADDLE };
typedef struct {
    uint8_t worn_wrist, dominant_hand, playing_hand, bowling_hand;
    uint8_t auto_resume, mass_kg, placement_source;
} workout_setup;
void workout_setup_set(const workout_setup *setup);
void workout_setup_get(workout_setup *out);
/* Body profile for the energy estimate (weight is setup.mass_kg). Frozen at Start like the
 * setup; 0 = unset, which the estimate replaces with a default and flags as assumed. */
typedef struct { uint8_t height_cm, age_years, sex /* 0 unset, 1 male, 2 female */, reserved; } workout_body;
void workout_body_set(const workout_body *body);
void workout_body_get(workout_body *out);
void workout_placement_revision_set(uint32_t revision);

enum { WORKOUT_REJECT_NONE, WORKOUT_REJECT_HR_MISSING, WORKOUT_REJECT_HR_CONTACT,
       WORKOUT_REJECT_HR_QUALITY, WORKOUT_REJECT_HR_RANGE, WORKOUT_REJECT_HR_STALE,
       WORKOUT_REJECT_HR_BEFORE, WORKOUT_REJECT_HR_ORDER, WORKOUT_REJECT_HR_SPIKE,
       WORKOUT_REJECT_GPS_MISSING, WORKOUT_REJECT_GPS_FIX, WORKOUT_REJECT_GPS_STALE,
       WORKOUT_REJECT_GPS_QUALITY, WORKOUT_REJECT_GPS_SPEED, WORKOUT_REJECT_GPS_BEFORE,
       WORKOUT_REJECT_GPS_ORDER, WORKOUT_REJECT_GPS_JUMP, WORKOUT_REJECT_MOTION_INVALID,
       WORKOUT_REJECT_MOTION_SATURATED, WORKOUT_REJECT_MOTION_GAP, WORKOUT_REJECT_COUNT };
typedef struct {
    uint16_t rejected[WORKOUT_REJECT_COUNT]; /* saturating acquisition/poll counts */
    uint32_t hr_sample_ms, gps_sample_ms, motion_sample_ms, last_reject_ms;
    uint8_t last_reason, reserved[3];
} workout_quality;

typedef struct { uint32_t elapsed_ms; float distance_m; } workout_lap;
typedef struct {
    uint32_t id, started_epoch, ended_epoch;
    uint32_t elapsed_ms;              /* active time; paused time excluded */
    uint32_t total_ms;                /* observed duration; reboot downtime excluded */
    uint32_t steps, hr_active_ms, gps_active_ms;
    float distance_m;
    float speed_mps, avg_speed_mps;   /* 0 with speed_valid = 0 means missing */
    float pace_s_km, avg_pace_s_km;   /* 0 with pace_valid = 0 means missing */
    float cadence_spm;
    uint16_t bpm, avg_bpm, peak_bpm;
    uint8_t sport, state, save_state, gps_status;
    uint8_t distance_valid, speed_valid, pace_valid, hr_valid, cadence_valid;
    uint8_t recovered;               /* reboot checkpoint recovered; see frozen setup */
    uint8_t nlaps;
    workout_lap laps[WORKOUT_LAPS_MAX]; /* cumulative splits; bounded */
    /* V2 append-only extension; first 236 bytes retain the V1 ABI. */
    workout_setup setup;
    uint8_t met_tenths; /* frozen category assumption; zero means unspecified */
    uint32_t motion_valid_ms, movement_ms, movement_bursts;
    float energy_kcal_est; /* gross session energy: heart rate (Keytel) or movement-scaled MET */
    uint32_t recovery_count;
    /* V3 append-only extension. Sample stamps are boot-relative; zero is unknown
     * after recovery. These are diagnostic gates, never accuracy claims. */
    uint32_t placement_revision;
    workout_quality quality;
    /* V4 append-only extension. */
    workout_body body;               /* frozen at Start */
    uint32_t ped_steps;              /* pedometer steps while RUNNING, any sport: the daily merge
                                        subtracts them so workout energy is not added twice */
    float energy_net_kcal;           /* energy above resting metabolism (the daily ACTIVE share) */
    uint8_t energy_basis;            /* ENERGY_BASIS_* bits in energy.h: what the estimate used */
    uint8_t series_n;                /* bins used */
    uint16_t series_bin_s;           /* active seconds per bin; doubles (bins merge) when full */
    uint8_t series_move[WORKOUT_SERIES_MAX]; /* wrist-movement %, 255 = no valid motion */
    uint8_t series_bpm[WORKOUT_SERIES_MAX];  /* mean valid bpm, 0 = none */
} workout_snapshot;

enum { WORKOUT_IO_NONE, WORKOUT_IO_LOCK, WORKOUT_IO_DIRECTORY, WORKOUT_IO_READ_OPEN,
       WORKOUT_IO_READ, WORKOUT_IO_READ_CLOSE, WORKOUT_IO_CORRUPT,
       WORKOUT_IO_WRITE_OPEN, WORKOUT_IO_WRITE, WORKOUT_IO_FLUSH, WORKOUT_IO_SYNC,
       WORKOUT_IO_WRITE_CLOSE, WORKOUT_IO_VERIFY, WORKOUT_IO_ALLOC, WORKOUT_IO_TASK,
       WORKOUT_IO_COUNT };
typedef struct {
    uint8_t ready;                    /* async history load has completed */
    uint8_t available;                /* last storage attempt succeeded */
    uint8_t dirty;                    /* checkpoint still needs saving */
    uint8_t busy;
    uint8_t corrupt;                  /* existing banks both invalid; preserved */
    uint8_t foreign;                  /* journal changed since load; original card needed */
    uint32_t errors;
    uint8_t uncertain;                /* unresolved read I/O; authoritative load deferred */
    uint8_t last_operation;
    int8_t last_slot;                 /* -1 for non-bank operations */
    uint8_t worker_failed;
    int32_t last_errno;
    uint32_t last_error_ms;
    uint16_t operation_errors[WORKOUT_IO_COUNT];
} workout_storage_status;

void workout_init(void);
/* Host automation may isolate history before init. Device always uses SD.
 * Host calls storage_service between frames; only the device starts a worker. */
int workout_storage_set_directory(const char *directory);
/* Thread-safe producer entry, g and deg/s; fresh samples only. No I/O. */
void workout_motion_sample(const hal_imu *sample, uint32_t now);
void workout_tick(void);              /* O(1), sensor cache sampling <=1Hz */
const workout_snapshot *workout_get(void); /* service/UI thread only */
const workout_profile *workout_profile_get(int sport);
/* Ready screen refreshes this lease in tick; idle GPS status then reflects the
 * selected outdoor profile. No explicit disable: other screens may use GPS. */
void workout_prepare(int sport);
void workout_unprepare(void);
int workout_start(int sport);         /* 0 if busy or unsaved result exists */
int workout_pause(void);
int workout_resume(void);
int workout_lap_add(void);            /* 0 if paused or 20 splits reached */
int workout_finish(void);             /* stops, then requests checked save */
int workout_discard(void);            /* explicit discard; removes checkpoint */
void workout_retry_save(void);        /* requests immediate background retry */
int workout_history_count(void);
/* Steps and net kcal of sessions started since `since_epoch`; returns the session count. */
int workout_day_energy(uint32_t since_epoch, uint32_t *ped_steps, float *net_kcal);
int workout_history_get(int newest_offset, workout_snapshot *out);
void workout_storage_get(workout_storage_status *out);
/* Background writer entry, also used by deterministic host tests. UI must
 * never call this: it acquires the SD lease/lock and checks fsync/readback. */
void workout_storage_service(void);
#endif
