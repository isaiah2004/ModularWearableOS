/* Passive background observability. History is bounded RAM, independent of the
 * foreground app; durable records remain owned by the existing SD journals. */
#ifndef NOS_BACKGROUND_H
#define NOS_BACKGROUND_H
#include <stdint.h>

enum {
    BG_MOTION, BG_STEPS, BG_CALORIES, BG_BATTERY, BG_JOURNAL, BG_TRACE,
    BG_IMU_REC, BG_WORKOUT, BG_HEART, BG_GPS, BG_CLIMATE, BG_LIGHT,
    BG_COMPASS, BG_COUNT
};
enum { BG_WAITING, BG_RUNNING, BG_IDLE, BG_ABSENT, BG_STALE, BG_ERROR,
       BG_PAUSED, BG_SAVING };
#define BG_HISTORY 120
#define BG_EVENTS 16
#define BG_WINDOW_MS 120000u
typedef struct {
    const char *name, *title, *metric[3], *unit[3], *about;
    uint32_t module, stale_ms;
    int on_demand;
} background_info;
typedef struct {
    uint32_t ms;
    float v[3];
    uint8_t valid, state;
    uint16_t pad;
} background_point;
typedef struct {
    uint32_t ms, updates, errors, dropped;
    uint8_t state, pad[3];
} background_event;
typedef struct {
    uint32_t last_ms, updates, errors, dropped;
    float v[3];
    uint8_t state, valid;
} background_status;
typedef struct {
    uint32_t now, last_ms, updates, errors, dropped;
    uint8_t state, valid, have_sample, reserved;
    float v[3], axes[6];
    int count, event_count;
    background_point points[BG_HISTORY];       /* oldest first */
    background_event events[BG_EVENTS];       /* newest first */
} background_snapshot;
void background_init(void);                   /* boot, PSRAM allocation only */
void background_tick(void);                   /* services, including display off */
const background_info *background_job(int id);
const char *background_state_name(int state);
int background_get(int id, background_snapshot *out); /* cached only, no I/O */
int background_summary(int states[BG_COUNT]);
int background_status_get(int id,background_status *out);
#endif
