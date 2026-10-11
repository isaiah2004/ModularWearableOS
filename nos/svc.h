/* svc.h — NOS data services: the single source of truth apps read from.
 *
 * Services run in the UI task: svc_tick() is called once per frame (before screens
 * tick) via the engine's background hook. They poll HAL caches, parse phone traffic,
 * fire alarms/timers (which push their ring screens), and post banners. Apps never
 * talk to radios or sensors for shared state; they read these structs.
 *
 * Phone link: Gadgetbridge's Bangle.js protocol over the Nordic UART Service (see
 * docs/research/ZSWATCH_NOTES.md section 4). The HAL only moves bytes; all parsing
 * lives here in portable C so the simulator can feed the same lines. */
#ifndef NOS_SVC_H
#define NOS_SVC_H
#include <stdint.h>

void svc_init(void);
void svc_tick(float dt);

/* ---- phone transport (implemented by the HAL: device = NUS, sim = injected) ---- */
int  hal_phone_connected(void);                 /* a central is subscribed to NUS TX */
int  hal_phone_read_line(char *buf, int n);     /* pop one complete received line, 0 = none */
void hal_phone_write_line(const char *line);    /* send one line to the phone ('\n' added) */
/* Sim/USB: feed a line as if the phone sent it (e.g. GB({"t":"notify",...})). */
void svc_phone_inject(const char *line);

/* ---- notifications ---- */
#define SVC_NOTIF_MAX 20
typedef struct {
    int      id;              /* phone id (dismissal sync) or local id */
    char     app[20];         /* "WhatsApp", "Gmail", "Calendar", "System" */
    char     title[40];       /* sender / title */
    char     body[160];
    uint32_t t_epoch;         /* when received (0 = unknown) */
    uint8_t  read;
    uint8_t  icon;            /* DOT_ICON_* derived from app */
} svc_notif;
int  svc_notif_count(void);
int  svc_notif_unread(void);
const svc_notif *svc_notif_get(int i);          /* 0 = newest */
void svc_notif_dismiss(int i);                  /* syncs DISMISS to the phone */
void svc_notif_clear(void);
void svc_notif_mark_all_read(void);
void svc_notif_add_local(const char *app, const char *title, const char *body);
/* Quick reply is unsupported by Gadgetbridge for most apps; OPEN asks the phone. */
void svc_notif_open_on_phone(int i);

/* ---- music (remote control of the phone's player) ---- */
typedef struct {
    int  have;                /* a player reported something */
    char track[64], artist[48], album[48];
    int  playing;
    uint32_t pos_ms, dur_ms;  /* pos interpolated locally while playing */
    int  volume;              /* 0-100 if known, else -1 */
} svc_music_t;
const svc_music_t *svc_music(void);
enum { MUSIC_PLAYPAUSE = 0, MUSIC_NEXT, MUSIC_PREV, MUSIC_VOLUP, MUSIC_VOLDOWN };
void svc_music_cmd(int cmd);

/* ---- weather (phone via Gadgetbridge, else Wi-Fi Open-Meteo, else none) ---- */
enum { WX_CLEAR = 0, WX_PARTLY, WX_CLOUDY, WX_RAIN, WX_STORM, WX_SNOW, WX_FOG, WX_WIND, WX_NIGHT };
typedef struct {
    int   valid;
    float temp_c, feels_c, hi_c, lo_c;
    int   cond;               /* WX_* */
    char  text[24];           /* "Light rain" */
    int   humidity, rain_pct, uv;
    float wind_kph; int wind_dir;
    char  loc[24];
    uint32_t updated_epoch;
    int   nhours;             /* hourly temps for the next hours */
    float hours_c[12];
    int   hours_cond[12];
    int   ndays;
    struct { float hi, lo; int cond; } days[5];
} svc_weather_t;
const svc_weather_t *svc_weather(void);

/* ---- activity ---- */
typedef struct {
    int   steps, goal;
    float km, kcal;
    int   active_min;
    uint16_t hourly[24];      /* steps per hour today */
    uint32_t week[7];         /* daily totals, [6] = today */
    int   stand_hours;        /* hours with > 250 steps */
} svc_activity_t;
const svc_activity_t *svc_activity(void);

/* ---- heart ---- */
typedef struct {
    int bpm_last;             /* last good reading, 0 none */
    uint32_t bpm_last_epoch;
    int resting, min_today, max_today;
    uint8_t hist[144];        /* 10-min samples for 24 h (hal_hr_history) */
    int nhist;
    int spo2_last;
} svc_heart_t;
const svc_heart_t *svc_heart(void);
void svc_heart_record(int bpm, int spo2);      /* app reports a measurement */

/* ---- alarms, timer, stopwatch (keep running while their apps are closed) ---- */
#define SVC_ALARM_MAX 8
typedef struct { uint8_t h, m, days, on; char label[16]; } svc_alarm;   /* days bit0 = Sun; 0 = once */
int  svc_alarm_count(void);
svc_alarm *svc_alarm_get(int i);
int  svc_alarm_add(int h, int m);               /* returns index or -1 */
void svc_alarm_remove(int i);
void svc_alarm_save(void);
int  svc_alarm_next(int *h, int *m, int *in_min); /* 1 if an alarm is set */
int  svc_alarm_ringing(void);                   /* index or -1 */
void svc_alarm_snooze(void);                    /* +9 min */
int  svc_alarm_snoozed(int *idx, uint32_t *at_ms);   /* 1 if snoozing: which alarm, when (hal_millis) */
void svc_alarm_snooze_cancel(void);
void svc_alarm_stop(void);

typedef struct {
    int      state;           /* 0 idle, 1 running, 2 paused, 3 ringing */
    uint32_t total_ms;
    uint32_t left_ms;         /* updated every tick */
} svc_timer_t;
const svc_timer_t *svc_timer(void);
void svc_timer_start(uint32_t ms);
void svc_timer_pause(void);
void svc_timer_resume(void);
void svc_timer_cancel(void);                    /* also stops ringing */
void svc_timer_add(uint32_t ms);                /* +1 min while running */

#define SVC_LAPS 30
typedef struct {
    int      running;
    uint32_t elapsed_ms;      /* updated every tick */
    int      nlaps;
    uint32_t laps[SVC_LAPS];  /* split times (absolute elapsed at each lap) */
} svc_stopwatch_t;
const svc_stopwatch_t *svc_stopwatch(void);
void svc_sw_start(void);
void svc_sw_stop(void);
void svc_sw_lap(void);
void svc_sw_reset(void);

/* ---- calls (Gadgetbridge "call" messages) ---- */
typedef struct { int state; char name[32]; char number[24]; uint32_t t0_ms; } svc_call_t;  /* 0 none, 1 incoming, 2 active */
const svc_call_t *svc_call(void);
void svc_call_accept(void);
void svc_call_reject(void);
void svc_call_hangup(void);
/* Silence the current ringer (side button on a ringing screen); the alert keeps
 * showing. Clears itself when the ringing source changes. */
void svc_ring_silence(void);

/* ---- find ---- */
void svc_find_phone(int on);                    /* ring the phone */
int  svc_find_phone_active(void);
int  svc_find_watch_active(void);               /* the phone is ringing us (screen pushed) */
void svc_find_watch_stop(void);

/* ---- system ---- */
typedef struct {
    int battery_pct, charging, rail_mv, battery_valid;
    int phone_connected;
    int wifi_up;
    int steps_today;
} svc_sys_t;
const svc_sys_t *svc_sys(void);

/* Ring screens the services push (defined by apps): */
extern const struct nos_screen scr_alarm_ring, scr_timer_ring, scr_call, scr_find_watch;

/* Voice memo recording, which keeps running when the Recorder is left (con_memo.c).
 * The system REC chip shows while svc_memo_active(). */
int      svc_memo_active(void);
uint32_t svc_memo_elapsed_ms(void);
int      svc_memo_number(void);          /* the "Memo n" it will be saved as */
int      svc_memo_stop(void);            /* stop and save; 1 if it was recording */
void     svc_memo_tick(float dt);        /* svc_tick runs it every frame */
#endif
