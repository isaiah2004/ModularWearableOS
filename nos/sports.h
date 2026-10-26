/* Sports journal v1. UTC + boot-relative time; local day/minute for browsing.
 * Sensor producers never touch SD. The device writer drains a bounded queue.
 */
#ifndef NOS_SPORTS_H
#define NOS_SPORTS_H
#include "hal.h"
#define SPORTS_MAGIC 0x53505431u
enum { SP_BOOT=1, SP_POWER, SP_BATTERY, SP_ACTIVITY, SP_WALK, SP_KNOCK,
       SP_WAKE, SP_GESTURE, SP_REPORT, SP_LABEL, SP_TUNE, SP_FOOD, SP_WATER,
       SP_IMU, SP_ENV, SP_LIGHT, SP_MAG, SP_HEART, SP_GPS, SP_ROUTINE, SP_PPG,
       SP_RAIL, SP_LIGHT_RAW, SP_MAG_RAW, SP_MIC, SP_TOUCH, SP_LOCK };
enum { SP_ACTIVE, SP_SENSOR, SP_DISPLAY_OFF, SP_DEEP_SLEEP, SP_AOD };
typedef struct {
    uint32_t magic, boot, seq, uptime, epoch, day;
    uint16_t minute, type;
    float v[8];
    uint32_t check;
} sports_record;
typedef struct {
    uint32_t written, dropped, errors, queued;
    int available;
    uint32_t suppressed, last_service_ms, last_write_ms;
    uint32_t gated; /* raw samples kept off the SD journal by the raw gate */
} sports_status;
typedef struct {
    float burn, food, water, battery, mv;
    unsigned samples, voltage_samples, modes, external_samples;
} sports_bin;
void sports_init(void);
void sports_tick(void); /* UI service, once/s internally */
void sports_emit(int type, const float *v, int n); /* thread safe */
/* Observers see every sample; the journal gets it only while the raw gate is open
 * (a workout or explicit raw logging, never in low battery), decimated to ~25 Hz. */
void sports_imu(const hal_imu *m);
enum { SPORTS_RAW_WORKOUT=1, SPORTS_RAW_MANUAL=2 };
void sports_raw_set(int source,int on);  /* hold/release the raw journal gate */
int sports_raw_active(void);             /* gate open: a source holds it and battery is not low */
int sports_raw_sources(void);
int sports_prune_raw(uint32_t cutoff_day); /* delete sensors files older than yyyymmdd; count removed */
void sports_power(int mode, int mhz, int brightness);
int sports_mode(void);
int sports_report(int feature, int outcome, int seconds); /* checked look-back label */
const char *sports_name(int type);
const char *sports_field(int type,int field);
int sports_recent(int newest_offset, sports_record *out);
int sports_archived(uint32_t day,int newest_offset,sports_record *out);
int sports_raw(uint32_t day,int newest_offset,sports_record *out);
void sports_get_status(sports_status *out);
/* Passive acquisition observer: called outside the journal lock, even if the
 * disk queue is full. Register once at boot. No I/O or sensor leases in callback. */
void sports_observe(void (*observer)(const sports_record *record));
int sports_intake(int water, int amount); /* checked NVS commit before confirmation */
int sports_undo(void); /* latest entry; persists a correction */
/* Query one local date, inclusive start / exclusive end minutes, 24 equal bins.
 * File history plus unexported NVS entries; missing sensor samples stay missing. */
int sports_query(uint32_t day, int start, int end, sports_bin bins[24]);
/* UI-safe asynchronous query. A newer request cancels the older token.
 * Poll: 0 pending, 1 snapshot copied, -1 cancelled/unavailable. No SD on caller. */
uint32_t sports_query_request(uint32_t day,int start,int end);
int sports_query_poll(uint32_t token,sports_bin bins[24]);
uint32_t sports_day(int offset);
void sports_flush(void);
#define SPORTS_QUIESCE_API 1
/* Pause routine producers before shutdown's final drain. Explicit transitions,
 * retained intake/reports and failed power observations remain eligible. This
 * is not a flush acknowledgement: the coordinator must recheck queued==0 and
 * all real owners before unmount, and resume on an aborted shutdown. */
void sports_quiesce(int pause);
int sports_quiesced(void);
/* Shared SD lock with mount/eject, implemented by the host. */
int hal_sports_sd_lock(void);
void hal_sports_sd_unlock(void);
#endif
