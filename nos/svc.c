/* svc.c — NOS data services (see svc.h).
 *
 * Everything runs in the UI task from svc_tick(). Sources:
 *   phone       Gadgetbridge Bangle.js lines via hal_phone_* (gb.c parses them)
 *   companion   the existing PC/phone bridge: hal_notification_poll, hal_media_*
 *   board       hal_power_get, hal_activity_get (pedometer task), hal_weather_get
 *   modules     hal_hr_history (MAX30102), haptics + speaker for the ringers
 * Persisted: "nosalarm" (alarms), "nosact" (hourly/week activity).
 *
 * Ring screens (alarm, timer, call, find watch) are pushed by the services when
 * the event starts. When it ends the screen normally pops itself (after its own
 * exit animation); if it is still on top 2.5 s later the services pop it. Covering
 * or swiping a ring screen away (anything but another ring screen on top) counts
 * as stopping that event.
 *
 * Internal RAM is nearly full on the device: the notification store, the line
 * buffer and the JSON tokens live in one PSRAM block (hal_big_alloc). */
#include "svc.h"
#include "sports.h"
#include "tuning.h"
#include "svc_track.h"
#include "workout.h"
#include "diag_log.h"
#include "gb.h"
#include "nos.h"
#include "hal.h"
#include "hal_ext.h"
#include "prefs.h"
#include "energy.h"
#include "dot.h"
#include "apps/nos_apps.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

/* Implemented next to hal_phone_* (device: firmware/main/hal_esp32.c, sim:
 * sim/nos_phone_sim.c): set the display timezone, minutes east of UTC. */
void hal_set_tz_min(int tz_min);
#ifndef ESP_PLATFORM
int nos_phone_sim_cmd(const char *s);           /* sim/nos_phone_sim.c */
#endif

#define FW_VERSION   "NOS " NOS_VERSION
#define HW_NAME      "ESP32-S3 AMOLED 1.43"
#define RING_MAX_MS  60000u
#define SNOOZE_MS    (9u * 60u * 1000u)
#define LINES_PER_FRAME 8

/* ------------------------------------------------------------------ state */
typedef struct {
    svc_notif n[SVC_NOTIF_MAX];
    uint8_t   phone[SVC_NOTIF_MAX];    /* 1 = came from Gadgetbridge (dismissal syncs) */
    char      line[GB_LINE_MAX];
    gb_json   json;
} svc_big;
static svc_big *B;
static int s_nn;
static uint32_t s_notif_revision;
static uint32_t s_notif_next_instance, s_banner_notif_instance;
static int s_banner_notif_id;
static int s_local_id = -1;            /* local notifications count down from -1 */

static svc_music_t     s_music;
static int             s_music_src;    /* 0 none, 1 Gadgetbridge, 2 companion */
static uint32_t        s_music_pos0, s_music_t0;
static uint32_t        s_music_poll_ms;
static svc_weather_t   s_wx;
static uint32_t        s_wx_gb_ms;     /* hal_millis of the last phone weather, 0 = never */
static svc_activity_t  s_act;
static svc_heart_t     s_heart;
static int             s_hr_rec_day_min = 0, s_hr_rec_day_max = 0;
static svc_timer_t     s_timer;
static uint32_t        s_timer_end, s_timer_ring_t0;
static uint32_t        s_timer_generation;
static svc_stopwatch_t s_sw;
static uint32_t        s_sw_t0;
static svc_call_t      s_call;
static svc_sys_t       s_sys;
static svc_alarm       s_alarms[SVC_ALARM_MAX];
static int             s_nalarms;
static uint32_t        s_alarm_generation, s_alarm_next_id;
static uint32_t        s_alarm_ids[SVC_ALARM_MAX];
static uint32_t        s_call_generation;
static int             s_ring_alarm = -1;
static uint32_t        s_ring_alarm_t0;
static int             s_snooze_idx = -1;
static uint32_t        s_snooze_at;
static int             s_alarm_key = -1;
static int             s_find_phone, s_find_watch;
static uint32_t        s_find_watch_t0;
static int             s_connected;
static uint32_t        s_ver_due;      /* send ver/status shortly after connect */
static int             s_act_rt_int;   /* Gadgetbridge realtime activity interval, s (0 off) */
static int             s_act_rt_hrm, s_act_rt_stp;
static uint32_t        s_act_rt_next, s_act_rt_steps;
static uint32_t        s_slow_ms, s_hr_ms, s_wx_ms, s_status_ms;
static int             s_status_pct = -1, s_status_chg = -1;

/* ------------------------------------------------------------------ helpers */
static void action_trace(uint16_t domain,uint16_t action,int32_t target,int old_state,int result,int detail)
{
    uint32_t request=diag_action_request(domain,action,target);
    diag_action_context(request,old_state,0);
    diag_action_result(request,result,detail);
}
static void copy_ascii(char *dst, int n, const char *src) { gb_ascii(src ? src : "", -1, dst, n); }
static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}
static int icontains(const char *hay, const char *needle)
{
    int n = (int)strlen(needle);
    for (const char *p = hay; *p; p++) {
        int k = 0;
        for (; k < n && p[k]; k++) {
            int x = p[k], y = needle[k];
            if (x >= 'A' && x <= 'Z') x += 32;
            if (x != y) break;
        }
        if (k == n) return 1;
    }
    return 0;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}
static uint32_t day_key(const hal_datetime *dt) { return (uint32_t)(dt->year * 10000 + dt->month * 100 + dt->day); }

/* ------------------------------------------------------------------ phone tx */
static void txf(const char *fmt, ...)
{
    if (!hal_phone_connected()) return;
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    hal_phone_write_line(b);
}
static int send_status(void)
{
    hal_power p;
    hal_power_get(&p);
    if (p.battery_percent < 0) return 0;         /* no estimate yet: never send a placeholder level */
    int pct = p.battery_percent, chg = p.external_power == 1;
    int mv = p.rail_mv ? p.rail_mv : hal_battery_mv();
    if (mv < 0) mv = 0;                          /* "-3.-80" would not be a JSON number */
    txf("{\"t\":\"status\",\"bat\":%d,\"volt\":%d.%02d,\"chg\":%d}", pct, mv / 1000, mv % 1000 / 10, chg);
    s_status_pct = pct;
    s_status_chg = chg;
    return 1;
}

/* ------------------------------------------------------------------ notifications */
static int icon_for(const char *app)
{
    static const char *const msg[] = { "whatsapp", "message", "sms", "signal", "telegram", "messenger",
                                       "gmail", "mail", "outlook", "slack", "discord", "chat", 0 };
    for (int i = 0; msg[i]; i++) if (icontains(app, msg[i])) return DOT_ICON_MSG;
    if (icontains(app, "calendar")) return DOT_ICON_CALENDAR;
    if (icontains(app, "phone") || icontains(app, "dialer") || icontains(app, "call")) return DOT_ICON_PHONE;
    if (icontains(app, "weather")) return DOT_ICON_CLOUD;
    if (icontains(app, "music") || icontains(app, "spotify")) return DOT_ICON_MUSIC;
    if (icontains(app, "map")) return DOT_ICON_ROUTE;
    return DOT_ICON_BELL;
}
static int notif_find(int id)
{
    if (!B) return -1;
    for (int i = 0; i < s_nn; i++) if (B->n[i].id == id) return i;
    return -1;
}
static void notif_remove_at(int i)
{
    if (!B || i < 0 || i >= s_nn) return;
    memmove(&B->n[i], &B->n[i + 1], sizeof B->n[0] * (size_t)(s_nn - 1 - i));
    memmove(&B->phone[i], &B->phone[i + 1], (size_t)(s_nn - 1 - i));
    s_nn--;
    s_notif_revision++;
}
static void banner_open(void)
{
    int at=notif_find(s_banner_notif_id);
    if(at>=0 && B->n[at].instance==s_banner_notif_instance) sys_notif_open(at);
}

/* Store (newest first) and optionally announce. Text is folded to ASCII here. */
static void notif_post(const char *app, const char *title, const char *body, int id, int phone,
                       uint32_t epoch, int banner, int read)
{
    if (!B) return;
    int at = notif_find(id);
    uint32_t instance=at>=0?B->n[at].instance:0;
    if(!instance) {if(!++s_notif_next_instance) ++s_notif_next_instance;instance=s_notif_next_instance;}
    if (at >= 0) notif_remove_at(at);             /* an update moves it to the top */
    if (s_nn >= SVC_NOTIF_MAX) s_nn = SVC_NOTIF_MAX - 1;
    memmove(&B->n[1], &B->n[0], sizeof B->n[0] * (size_t)s_nn);
    memmove(&B->phone[1], &B->phone[0], (size_t)s_nn);
    s_nn++;
    svc_notif *n = &B->n[0];
    memset(n, 0, sizeof *n);
    n->id = id;
    n->instance = instance;
    copy_ascii(n->app, sizeof n->app, app && *app ? app : "Phone");
    copy_ascii(n->title, sizeof n->title, title);
    copy_ascii(n->body, sizeof n->body, body);
    n->t_epoch = epoch ? epoch : (uint32_t)hal_epoch_seconds();
    n->read = (uint8_t)read;
    n->icon = (uint8_t)icon_for(n->app);
    B->phone[0] = (uint8_t)phone;
    s_notif_revision++;
    if (banner && !nos_prefs.dnd) {
        s_banner_notif_id=id;s_banner_notif_instance=instance;
        nos_banner(n->app, n->title[0] ? n->title : n->body, n->icon, banner_open);
    }
}

int svc_notif_count(void) { return s_nn; }
uint32_t svc_notif_revision(void) { return s_notif_revision; }
int svc_notif_unread(void)
{
    int u = 0;
    for (int i = 0; i < s_nn; i++) u += !B->n[i].read;
    return u;
}
const svc_notif *svc_notif_get(int i) { return (B && i >= 0 && i < s_nn) ? &B->n[i] : 0; }
void svc_notif_dismiss(int i)
{
    if (!B || i < 0 || i >= s_nn) return;
    if (B->phone[i]) txf("{\"t\":\"notify\",\"id\":%d,\"n\":\"DISMISS\"}", B->n[i].id);
    notif_remove_at(i);
}
void svc_notif_clear(void)
{
    if (!B) return;
    for (int i = 0; i < s_nn; i++)
        if (B->phone[i]) txf("{\"t\":\"notify\",\"id\":%d,\"n\":\"DISMISS\"}", B->n[i].id);
    s_nn = 0;
    s_notif_revision++;
}
void svc_notif_mark_all_read(void)
{
    int changed = 0;
    for (int i = 0; B && i < s_nn; i++) { changed |= !B->n[i].read; B->n[i].read = 1; }
    if (changed) s_notif_revision++;
}
void svc_notif_add_local(const char *app, const char *title, const char *body)
{
    notif_post(app, title, body, s_local_id--, 0, 0, 0, 0);
}
void svc_notif_open_on_phone(int i)
{
    if (!B || i < 0 || i >= s_nn || !B->phone[i]) return;
    txf("{\"t\":\"notify\",\"id\":%d,\"n\":\"OPEN\"}", B->n[i].id);
    if (!B->n[i].read) { B->n[i].read = 1; s_notif_revision++; }
}

/* ------------------------------------------------------------------ music */
const svc_music_t *svc_music(void) { return &s_music; }
static void music_set_pos(uint32_t pos_ms)
{
    s_music_pos0 = pos_ms;
    s_music_t0 = hal_millis();
    s_music.pos_ms = pos_ms;
}
void svc_music_cmd(int cmd)
{
    if(cmd<0 || cmd>MUSIC_VOLDOWN) return;
    /* Transport APIs provide no delivery acknowledgment. Keep this pending;
     * a local optimistic player change is not proof the phone applied it. */
    action_trace(DIAG_DOMAIN_MUSIC,DIAG_ACT_SEND,cmd,s_music.playing,DIAG_RESULT_PENDING,s_music_src);
    if (hal_phone_connected() && s_music_src != 2) {
        static const char *const n[] = { "play", "next", "previous", "volumeup", "volumedown" };
        if (cmd < 0 || cmd > MUSIC_VOLDOWN) return;
        const char *v = n[cmd];
        if (cmd == MUSIC_PLAYPAUSE) {
            v = s_music.playing ? "pause" : "play";
            music_set_pos(s_music.pos_ms);       /* optimistic: the phone confirms with musicstate */
            s_music.playing = !s_music.playing;
        }
        txf("{\"t\":\"music\",\"n\":\"%s\"}", v);
    } else {
        hal_media_cmd(cmd);                      /* companion bridge: same 0-4 codes */
    }
}
static void music_tick(uint32_t now)
{
    if (s_music_src == 1) {
        if (s_music.playing) {
            uint32_t p = s_music_pos0 + (now - s_music_t0);
            if (s_music.dur_ms && p > s_music.dur_ms) p = s_music.dur_ms;
            s_music.pos_ms = p;
        }
        return;
    }
    /* companion fallback (polled at 4 Hz; position is interpolated by the HAL) */
    if (now - s_music_poll_ms < 250) return;
    s_music_poll_ms = now;
    char t[64], a[48];
    int playing = 0;
    if (hal_media_get(t, sizeof t, a, sizeof a, &playing)) {
        copy_ascii(s_music.track, sizeof s_music.track, t);
        copy_ascii(s_music.artist, sizeof s_music.artist, a);
        s_music.album[0] = 0;
        s_music.playing = playing;
        s_music.have = 1;
        s_music.volume = -1;
        int pos = 0, dur = 0;
        if (hal_media_time(&pos, &dur)) { s_music.pos_ms = (uint32_t)pos; s_music.dur_ms = (uint32_t)dur; }
        s_music_src = 2;
    } else if (s_music_src == 2) {
        memset(&s_music, 0, sizeof s_music);
        s_music.volume = -1;
        s_music_src = 0;
    }
}

/* ------------------------------------------------------------------ weather */
const svc_weather_t *svc_weather(void) { return &s_wx; }
static int local_hour(void) { hal_datetime dt; hal_get_datetime(&dt); return dt.hour; }
static int wx_from_owm(int code)
{
    if (code >= 200 && code < 300) return WX_STORM;
    if (code >= 300 && code < 600) return code == 511 ? WX_SNOW : WX_RAIN;
    if (code >= 600 && code < 700) return WX_SNOW;
    if (code == 771 || code == 781) return WX_WIND;
    if (code >= 700 && code < 800) return WX_FOG;
    if (code == 800) { int h = local_hour(); return (h < 6 || h >= 19) ? WX_NIGHT : WX_CLEAR; }
    if (code == 801 || code == 802) return WX_PARTLY;
    if (code > 802 && code < 900) return WX_CLOUDY;
    return WX_CLOUDY;
}
static int wx_from_wmo(int code)
{
    if (code == 0) { int h = local_hour(); return (h < 6 || h >= 19) ? WX_NIGHT : WX_CLEAR; }
    if (code == 1 || code == 2) return WX_PARTLY;
    if (code == 3) return WX_CLOUDY;
    if (code == 45 || code == 48) return WX_FOG;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return WX_SNOW;
    if (code >= 95) return WX_STORM;
    return WX_RAIN;
}
static const char *wx_text(int cond)
{
    static const char *const t[] = { "Clear", "Partly cloudy", "Cloudy", "Rain", "Thunderstorm",
                                     "Snow", "Fog", "Windy", "Clear night" };
    return (cond >= 0 && cond <= WX_NIGHT) ? t[cond] : "";
}
static float kelvin_c(double v) { return (float)(v > 150 ? v - 273.15 : v); }
/* Hourly/daily forecast from the Wi-Fi weather cache (core/weather.h), if it has one. */
static void wx_merge_forecast(void)
{
    hal_weather w;
    hal_weather_get(&w);
    if (!w.valid) return;
    s_wx.nhours = WEATHER_HOURS < 12 ? WEATHER_HOURS : 12;
    for (int i = 0; i < s_wx.nhours; i++) {
        s_wx.hours_epoch[i] = w.hours[i].epoch;
        s_wx.hours_c[i] = (float)w.hours[i].temp;
        s_wx.hours_cond[i] = wx_from_wmo(w.hours[i].code);
    }
    s_wx.ndays = WEATHER_DAYS < 5 ? WEATHER_DAYS : 5;
    for (int i = 0; i < s_wx.ndays; i++) {
        s_wx.days_epoch[i] = w.days[i].epoch;
        s_wx.days[i].hi = (float)w.days[i].hi;
        s_wx.days[i].lo = (float)w.days[i].lo;
        s_wx.days[i].cond = wx_from_wmo(w.days[i].code);
    }
    s_wx.utc_offset_seconds = w.utc_offset_seconds;
}
static void wx_from_hal(void)
{
    hal_weather w;
    hal_weather_get(&w);
    memset(&s_wx, 0, sizeof s_wx);
    s_wx.valid = w.valid;
    s_wx.stale = w.stale; s_wx.fetching = w.fetching; s_wx.source = 2;
    s_wx.generation = w.generation; s_wx.age_seconds = w.age_seconds;
    s_wx.utc_offset_seconds = w.utc_offset_seconds;
    snprintf(s_wx.status, sizeof s_wx.status, "%s", w.status);
    copy_ascii(s_wx.loc, sizeof s_wx.loc, w.city);
    if (!w.valid) return;                    /* location invalidation clears old feed */
    s_wx.temp_c = (float)w.temp;
    s_wx.feels_c = (float)w.feels;
    s_wx.cond = wx_from_wmo(w.code);
    snprintf(s_wx.text, sizeof s_wx.text, "%s", wx_text(s_wx.cond));
    s_wx.humidity = -1; s_wx.rain_pct = -1; s_wx.uv = -1; s_wx.wind_kph = -1; s_wx.wind_dir = -1;
    copy_ascii(s_wx.loc, sizeof s_wx.loc, w.city);
    s_wx.updated_epoch = (uint32_t)(w.observed_epoch > 0 ? w.observed_epoch : (int64_t)hal_epoch_seconds());
    s_wx.hi_c = (float)w.days[0].hi;
    s_wx.lo_c = (float)w.days[0].lo;
    wx_merge_forecast();
}
static void wx_tick(uint32_t now)
{
    if (now - s_wx_ms < 10000 && s_wx_ms) return;
    s_wx_ms = now;
    int gb_fresh = s_wx_gb_ms && now - s_wx_gb_ms < 3u * 3600u * 1000u;
    if (gb_fresh) {
        s_wx.age_seconds = (now - s_wx_gb_ms) / 1000u;
        s_wx.stale = s_wx.age_seconds >= 3600u;
        snprintf(s_wx.status, sizeof s_wx.status, "%s", s_wx.stale ? "Phone weather is stale" : "Phone weather");
        return;
    }
    wx_from_hal();
}

/* ------------------------------------------------------------------ activity */
typedef struct {
    uint8_t  version, valid_days, pad[2];
    uint32_t day;            /* YYYYMMDD the hourly/active data belongs to, 0 = unknown */
    uint32_t week[6];        /* previous six days, oldest first */
    uint16_t hourly[24];
    uint16_t active_min, pad2;
    uint32_t last_steps, max_steps;
} act_saved;
#define ACT_VERSION 2
static act_saved s_as;

const svc_activity_t *svc_activity(void) { return &s_act; }

/* Today's energy. ACTIVE = steps outside workouts x the profile's step energy + the workouts'
 * net energy (their own steps are subtracted, never counted twice); REST = Mifflin-St Jeor
 * prorated to the time of day; TOTAL = both. Weight is sports_mass_kg; height/age/sex come from
 * the profile and any that are missing fall back to defaults, flagged in kcal_assumed. */
static void act_energy(uint32_t steps)
{
    hal_datetime dt;
    hal_get_datetime(&dt);
    energy_body b = { nos_prefs.sports_mass_kg, nos_prefs.body_height_cm,
                      (uint8_t)energy_age_from_birth(nos_prefs.body_birth_year, dt.year), nos_prefs.body_sex };
    uint64_t now = hal_epoch_seconds();
    int sod = dt.hour * 3600 + dt.minute * 60 + dt.second;
    uint32_t ped = 0;
    float net = 0;
    int n = 0;
    if (now > (uint64_t)sod + 1000000u) n = workout_day_energy((uint32_t)(now - (uint64_t)sod), &ped, &net);
    float sk = energy_step_kcal(&b), stride = energy_stride_m(&b);
    s_act.km = steps * stride / 1000.0f;
    s_act.kcal_workout = net;
    s_act.workouts_today = n;
    s_act.kcal = energy_daily_active((int)steps, ped, sk, net);
    s_act.kcal_rest = now ? energy_rest_so_far(&b, sod) : 0.0f;    /* no trusted clock: no time of day */
    s_act.kcal_total = s_act.kcal + s_act.kcal_rest;
    s_act.kcal_assumed = (uint8_t)energy_assumed(&b);
}

static void act_publish(uint32_t steps)
{
    s_act.goal = nos_prefs.step_goal_100 ? nos_prefs.step_goal_100 * 100 : 8000;
    s_act.steps = (int)steps;
    act_energy(steps);
    s_act.active_min = s_as.active_min;
    memcpy(s_act.hourly, s_as.hourly, sizeof s_act.hourly);
    for (int i = 0; i < 6; i++) s_act.week[i] = s_as.week[i];
    s_act.week[6] = steps;
    s_act.week_valid = (uint8_t)(s_as.valid_days | (s_as.day ? 64u : 0u));
    int st = 0;
    for (int h = 0; h < 24; h++) st += s_as.hourly[h] > 250;
    s_act.stand_hours = st;
    s_sys.steps_today = (int)steps;
}

#ifndef ESP_PLATFORM
/* Simulator: a believable day (commute, lunch walk, evening walk; ~6.4k by 17:00). */
static const uint16_t SIM_DAY[24] = { 0, 0, 0, 0, 0, 0, 60, 520, 1480, 720, 380, 430,
                                      980, 660, 330, 420, 420, 1180, 1620, 760, 480, 260, 110, 20 };
static void act_tick(uint32_t now)
{
    (void)now;
    hal_datetime dt;
    hal_get_datetime(&dt);
    float frac = (dt.minute * 60 + dt.second) / 3600.0f;
    uint32_t steps = 0;
    memset(s_as.hourly, 0, sizeof s_as.hourly);
    for (int h = 0; h < dt.hour; h++) { s_as.hourly[h] = SIM_DAY[h]; steps += SIM_DAY[h]; }
    s_as.hourly[dt.hour] = (uint16_t)(SIM_DAY[dt.hour] * frac);
    steps += s_as.hourly[dt.hour];
    s_as.active_min = (uint16_t)(steps / 92);
    static const uint32_t wk[6] = { 7120, 9340, 5210, 8050, 10420, 6880 };
    memcpy(s_as.week, wk, sizeof wk);
    s_as.valid_days=127;
    act_publish(steps);
}
static void act_init(void) {}
#else
static int       s_as_dirty;
static uint32_t  s_as_save_ms;
static uint32_t s_as_attempt_ms;
static int s_as_retry;
static int       s_min_key = -1;
static uint32_t  s_min_steps;
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static int64_t key_days(uint32_t k) { return days_from_civil((int)(k / 10000), (int)(k / 100 % 100), (int)(k % 100)); }
static void act_save(void)
{
    s_as_attempt_ms=hal_millis();
    if(!hal_store_save_checked("nosact", &s_as, sizeof s_as)) {
        s_as_dirty=1;s_as_retry=1;s_act.checkpoint_pending=1;
        if(s_act.checkpoint_errors<UINT32_MAX)s_act.checkpoint_errors++;
        diag_log_event(DIAG_STORAGE_ERROR,(int32_t)diag_log_name_id("nosact"),(int32_t)s_as.day,(int32_t)s_act.checkpoint_errors);
        return;
    }
    s_as_dirty = 0;
    s_as_retry=0;s_act.checkpoint_pending=0;
    s_as_save_ms = hal_millis();
    s_act.checkpoint_epoch=(uint32_t)hal_epoch_seconds();
}
static void act_rollover(uint32_t day, uint32_t steps)
{
    int64_t gap = key_days(day) - key_days(s_as.day);
    if (gap < 0 || gap > 7) gap = 7;              /* clock jumped: start a clean week */
    for (int64_t k = 0; k < gap; k++) {
        memmove(&s_as.week[0], &s_as.week[1], sizeof s_as.week[0] * 5);
        s_as.week[5] = k == 0 ? s_as.max_steps : 0;
        s_as.valid_days=(uint8_t)((s_as.valid_days>>1)&31u);
        if(k==0 && s_as.day)s_as.valid_days|=32u;
    }
    memset(s_as.hourly, 0, sizeof s_as.hourly);
    s_as.active_min = 0;
    s_as.max_steps = steps;
    s_as.last_steps = steps;
    s_as.day = day;
    s_as.valid_days|=64u;s_as_dirty=1;
    s_min_key = -1;
    act_save();
}
static void act_tick(uint32_t now)
{
    hal_activity a;
    hal_activity_get(&a);
    if(!a.valid) {act_publish(s_as.max_steps);return;}
    uint32_t steps = a.steps;
    if (hal_epoch_seconds()) {                    /* day logic needs a trusted clock */
        hal_datetime dt;
        hal_get_datetime(&dt);
        uint32_t day = day_key(&dt);
        if (!s_as.day) { s_as.day = day; s_as.last_steps = steps; s_as.valid_days|=64u; s_as_dirty = 1; }
        else if (day != s_as.day) act_rollover(day, steps);
        if (steps < s_as.last_steps) s_as.last_steps = steps;   /* pedometer reset */
        uint32_t d = steps - s_as.last_steps;
        if (d) {
            uint32_t v = s_as.hourly[dt.hour] + d;
            s_as.hourly[dt.hour] = (uint16_t)(v > 65535 ? 65535 : v);
            s_as.last_steps = steps;
            s_as_dirty = 1;
        }
        if (steps > s_as.max_steps) s_as.max_steps = steps;
        int mk = dt.hour * 60 + dt.minute;
        if (steps < s_min_steps) s_min_steps = steps;
        if (mk != s_min_key) {                    /* cadence > 60 steps/min = active minute */
            if (s_min_key >= 0 && steps - s_min_steps > 60) { s_as.active_min++; s_as_dirty = 1; }
            s_min_key = mk;
            s_min_steps = steps;
        }
        s_act.checkpoint_pending=(uint8_t)s_as_dirty;
        if (s_as_dirty && (s_as_retry ? now-s_as_attempt_ms>=5000u : now-s_as_save_ms>10u*60u*1000u)) act_save();
    }
    act_publish(steps);
}
static void act_init(void)
{
    act_saved t;
    if (hal_store_load("nosact", &t, sizeof t) && (t.version == ACT_VERSION || t.version == 1)) {
        s_as=t;
        if(t.version==1) {
            /* Old zero entries have no validity provenance: keep them unknown.
             * Positive totals and the recorded current day are established. */
            s_as.valid_days=t.day?64u:0;
            for(int i=0;i<6;i++)if(t.week[i])s_as.valid_days|=(uint8_t)(1u<<i);
            s_as.version=ACT_VERSION;s_as_dirty=1;
        }
    }
    else { memset(&s_as, 0, sizeof s_as); s_as.version = ACT_VERSION; }
    s_as_save_ms = hal_millis();
}
#endif

/* ------------------------------------------------------------------ heart */
const svc_heart_t *svc_heart(void) { return &s_heart; }
static int minutes_today(void) { hal_datetime dt; hal_get_datetime(&dt); return dt.hour * 60 + dt.minute; }

static uint32_t s_hr_day;
static uint32_t s_hr_last_sample_ms;
static void heart_tick(void)
{
    hal_datetime now_dt;
    hal_get_datetime(&now_dt);
    if (day_key(&now_dt) != s_hr_day) { s_hr_day = day_key(&now_dt); s_hr_rec_day_min = s_hr_rec_day_max = 0; }
    hal_hr_sample samples[144];
    int n = hal_hr_history_samples(samples, 144);
    if (n < 0) n = 0;
    if(n>144)n=144;
    uint32_t ep=(uint32_t)hal_epoch_seconds();
    memset(s_heart.hist,0,sizeof s_heart.hist);s_heart.nhist=0;
    int rest = 0, lo = s_hr_rec_day_min, hi = s_hr_rec_day_max;
    uint32_t midnight=ep?(ep-(uint32_t)minutes_today()*60u-(uint32_t)now_dt.second):0;
    for (int i = 0; i < n; i++) {
        int v = samples[i].bpm;uint32_t at=samples[i].epoch;
        if(v<35 || v>230)continue;
        if(!s_heart.bpm_last || (at && at>s_heart.bpm_last_epoch) ||
           (!at && !s_heart.bpm_last_epoch && (int32_t)(samples[i].sample_ms-s_hr_last_sample_ms)>0)) {
            s_heart.bpm_last=v;s_heart.bpm_last_epoch=at;s_hr_last_sample_ms=samples[i].sample_ms;
        }
        if(!ep || !at || at>ep || ep-at>=86400u)continue;
        unsigned bin=143u-(ep-at)/600u;
        s_heart.hist[bin]=(uint8_t)v;s_heart.nhist=144;
        if (!rest || v < rest) rest = v;
        if (at>=midnight) {
            if (!lo || v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    s_heart.resting = rest;
    s_heart.min_today = lo;
    s_heart.max_today = hi;
}
uint32_t svc_heart_sample_epoch(uint32_t sample_ms)
{
    uint64_t ep=hal_epoch_seconds();uint32_t age=hal_millis()-sample_ms;
    if(!sample_ms || ep<1577836800ULL || ep>UINT32_MAX || age>3000u || ep<age/1000u)return 0;
    return (uint32_t)ep-age/1000u;
}
void svc_heart_record_at(int bpm, int spo2, uint32_t acquisition_epoch)
{
    uint32_t now=(uint32_t)hal_epoch_seconds();
    if(acquisition_epoch && now && acquisition_epoch>now)return;
    if (bpm>=35 && bpm<=230) {
        if(s_heart.bpm_last_epoch && (!acquisition_epoch || acquisition_epoch<s_heart.bpm_last_epoch))return;
        s_heart.bpm_last = bpm;
        s_heart.bpm_last_epoch = acquisition_epoch;
        hal_datetime dt;hal_get_datetime(&dt);
        uint32_t midnight=now-(uint32_t)(dt.hour*3600+dt.minute*60+dt.second);
        if(acquisition_epoch && acquisition_epoch>=midnight) {
            if (!s_hr_rec_day_min || bpm < s_hr_rec_day_min) s_hr_rec_day_min = bpm;
            if (bpm > s_hr_rec_day_max) s_hr_rec_day_max = bpm;
            if (!s_heart.min_today || bpm < s_heart.min_today) s_heart.min_today = bpm;
            if (bpm > s_heart.max_today) s_heart.max_today = bpm;
        }
    }
    s_heart.spo2_last=spo2>=70 && spo2<=100?spo2:0;
}
void svc_heart_record(int bpm, int spo2) {svc_heart_record_at(bpm,spo2,0);}

/* ------------------------------------------------------------------ ring screens */
typedef struct { const nos_screen *s; int pushed, ending; uint32_t end_ms; } ring_scr;
static ring_scr R_ALARM = { &scr_alarm_ring, 0, 0, 0 }, R_TIMER = { &scr_timer_ring, 0, 0, 0 },
                R_CALL = { &scr_call, 0, 0, 0 }, R_FIND = { &scr_find_watch, 0, 0, 0 };
static int is_ring(const nos_screen *s)
{
    return s == &scr_alarm_ring || s == &scr_timer_ring || s == &scr_call || s == &scr_find_watch;
}
/* Keep a ring screen on top while `want`; returns 1 if the user swiped it away.
 * When the event ends the screen gets a short grace period to play its own exit
 * (the call screen shows "ended", ring screens pop themselves); if it is still on
 * top after that, the services pop it. */
#define RING_GRACE_MS 2500u
static int ring_screen(ring_scr *r, int want)
{
    const nos_screen *top = nos_top();
    if (want) {
        r->ending = 0;
        if (!r->pushed) {
            if (top != r->s) nos_push(r->s, TX_RISE);
            r->pushed = 1;
        } else if (top != r->s && !nos_transitioning() && !is_ring(top) && !nos_contains(r->s)) {
            r->pushed = 0;
            return 1;
        }
    } else if (r->pushed) {
        uint32_t now = hal_millis();
        if (!r->ending) { r->ending = 1; r->end_ms = now; }
        if (top != r->s) { if (!nos_transitioning() && !nos_contains(r->s)) r->pushed = r->ending = 0; }
        else if (now - r->end_ms >= RING_GRACE_MS && !nos_transitioning()) {
            nos_pop();
            r->pushed = r->ending = 0;
        }
    }
    return 0;
}

enum { RING_NONE = 0, RING_TIMER, RING_ALARM, RING_CALL, RING_FIND };
static int s_ring_kind;
static uint32_t s_ring_next;
static int s_ring_silenced;
void svc_ring_silence(void) { s_ring_silenced = 1; hal_haptic_stop(); hal_sfx_stop(); }
static void ringer_tick(uint32_t now)
{
    int kind = s_find_watch ? RING_FIND : s_call.state == 1 ? RING_CALL :
               s_ring_alarm >= 0 ? RING_ALARM : s_timer.state == 3 ? RING_TIMER : RING_NONE;
    if (kind != s_ring_kind) {
        if (kind == RING_NONE) { hal_haptic_stop(); hal_sfx_stop(); }
        s_ring_kind = kind;
        s_ring_silenced = 0;
        s_ring_next = now;
    }
    if (kind && !s_ring_silenced && (int32_t)(now - s_ring_next) >= 0) {
        hal_haptic(HAPTIC_ALARM);
        int sfx = kind == RING_TIMER ? SFX_TIMER_DONE : kind == RING_CALL ? SFX_NOTIFY : SFX_ALARM;
        if (kind != RING_CALL || nos_prefs.sound) hal_sfx(sfx);
        s_ring_next = now + 2000;
    }
    if (kind) nos_keep_awake();
}

/* ------------------------------------------------------------------ alarms */
typedef struct { uint8_t version, count, pad[2]; svc_alarm a[SVC_ALARM_MAX]; } alarm_saved;
#define ALARM_VERSION 1

int svc_alarm_count(void) { return s_nalarms; }
uint32_t svc_alarm_generation(void) { return s_alarm_generation; }
uint32_t svc_alarm_id(int i) { return i >= 0 && i < s_nalarms ? s_alarm_ids[i] : 0; }
int svc_alarm_find(uint32_t id)
{
    if (id) for (int i=0; i<s_nalarms; i++) if (s_alarm_ids[i]==id) return i;
    return -1;
}
static uint32_t alarm_new_id(void) { if (!++s_alarm_next_id) ++s_alarm_next_id; return s_alarm_next_id; }
svc_alarm *svc_alarm_get(int i) { return (i >= 0 && i < SVC_ALARM_MAX) ? &s_alarms[i] : &s_alarms[0]; }
void svc_alarm_save(void)
{
    alarm_saved s;
    memset(&s, 0, sizeof s);
    s.version = ALARM_VERSION;
    s.count = (uint8_t)s_nalarms;
    memcpy(s.a, s_alarms, sizeof s.a);
    int ok=hal_store_save_checked("nosalarm", &s, sizeof s);
    action_trace(DIAG_DOMAIN_ALARM,DIAG_ACT_SAVE,(int32_t)s_alarm_generation,s_nalarms,
                 ok?DIAG_RESULT_SUCCESS:DIAG_RESULT_FAILED,ok);
}
static void alarm_load(void)
{
    alarm_saved s;
    if (hal_store_load("nosalarm", &s, sizeof s) && s.version == ALARM_VERSION && s.count <= SVC_ALARM_MAX) {
        memcpy(s_alarms, s.a, sizeof s_alarms);
        s_nalarms = s.count;
        for (int i = 0; i < s_nalarms; i++) s_alarms[i].label[sizeof s_alarms[i].label - 1] = 0;
    } else s_nalarms = 0;
    for (int i=0; i<s_nalarms; i++) s_alarm_ids[i]=alarm_new_id();
    s_alarm_generation++;
}
int svc_alarm_add(int h, int m)
{
    if (s_nalarms >= SVC_ALARM_MAX || h < 0 || h > 23 || m < 0 || m > 59) return -1;
    svc_alarm *a = &s_alarms[s_nalarms];
    memset(a, 0, sizeof *a);
    a->h = (uint8_t)h; a->m = (uint8_t)m; a->on = 1;
    int index = s_nalarms++;
    s_alarm_ids[index] = alarm_new_id(); s_alarm_generation++;
    svc_alarm_save();
    return index;
}
void svc_alarm_remove(int i)
{
    if (i < 0 || i >= s_nalarms) return;
    if (s_ring_alarm == i) svc_alarm_stop();
    else if (s_ring_alarm > i) s_ring_alarm--;
    if (s_snooze_idx == i) s_snooze_idx = -1;
    else if (s_snooze_idx > i) s_snooze_idx--;
    memmove(&s_alarms[i], &s_alarms[i + 1], sizeof s_alarms[0] * (size_t)(s_nalarms - 1 - i));
    memmove(&s_alarm_ids[i], &s_alarm_ids[i + 1], sizeof s_alarm_ids[0] * (size_t)(s_nalarms - 1 - i));
    s_nalarms--;
    s_alarm_ids[s_nalarms] = 0; s_alarm_generation++;
    memset(&s_alarms[s_nalarms], 0, sizeof s_alarms[0]);
    svc_alarm_save();
}
int svc_alarm_next(int *h, int *m, int *in_min)
{
    hal_datetime dt;
    hal_get_datetime(&dt);
    int now = dt.hour * 60 + dt.minute, best = -1, bi = -1;
    for (int i = 0; i < s_nalarms; i++) {
        const svc_alarm *a = &s_alarms[i];
        if (!a->on) continue;
        for (int d = 0; d <= 7; d++) {
            int wd = (dt.weekday + d) % 7;
            if (a->days && !(a->days & (1 << wd))) continue;
            int mins = d * 1440 + a->h * 60 + a->m - now;
            if (mins <= 0) continue;
            if (best < 0 || mins < best) { best = mins; bi = i; }
            break;
        }
    }
    if (s_snooze_idx >= 0) {
        int mins = (int)((s_snooze_at - hal_millis()) / 60000u) + 1;
        if (best < 0 || mins < best) {
            best = mins;
            int t = now + mins;
            if (h) *h = t / 60 % 24;
            if (m) *m = t % 60;
            if (in_min) *in_min = best;
            return 1;
        }
    }
    if (bi < 0) return 0;
    if (h) *h = s_alarms[bi].h;
    if (m) *m = s_alarms[bi].m;
    if (in_min) *in_min = best;
    return 1;
}
int svc_alarm_ringing(void) { return s_ring_alarm; }
static void alarm_ring(int i)
{
    action_trace(DIAG_DOMAIN_ALARM,DIAG_ACT_FIRE,(int32_t)svc_alarm_id(i),s_ring_alarm,DIAG_RESULT_SUCCESS,i);
    s_ring_alarm = i;
    s_ring_alarm_t0 = hal_millis();
}
int svc_alarm_snoozed(int *idx, uint32_t *at_ms)
{
    if (s_snooze_idx < 0) return 0;
    if (idx) *idx = s_snooze_idx;
    if (at_ms) *at_ms = s_snooze_at;
    return 1;
}
void svc_alarm_snooze_cancel(void) { s_snooze_idx = -1; }

void svc_alarm_snooze(void)
{
    if (s_ring_alarm < 0) return;
    s_snooze_idx = s_ring_alarm;
    s_snooze_at = hal_millis() + SNOOZE_MS;
    s_ring_alarm = -1;
}
void svc_alarm_stop(void) { s_ring_alarm = -1; }
static void alarm_tick(uint32_t now)
{
    hal_datetime dt;
    hal_get_datetime(&dt);
    int key = dt.day * 1440 + dt.hour * 60 + dt.minute;
    if (key != s_alarm_key) {
        s_alarm_key = key;
        int changed = 0;
        for (int i = 0; i < s_nalarms; i++) {
            svc_alarm *a = &s_alarms[i];
            if (!a->on || a->h != dt.hour || a->m != dt.minute) continue;
            if (a->days && !(a->days & (1 << dt.weekday))) continue;
            if (!a->days) { a->on = 0; changed = 1; }      /* one-shot */
            alarm_ring(i);
        }
        if (changed) svc_alarm_save();
    }
    if (s_snooze_idx >= 0 && (int32_t)(now - s_snooze_at) >= 0) {
        int i = s_snooze_idx;
        s_snooze_idx = -1;
        alarm_ring(i);
    }
    if (s_ring_alarm >= 0 && now - s_ring_alarm_t0 > RING_MAX_MS) {
        action_trace(DIAG_DOMAIN_ALARM,DIAG_ACT_TIMEOUT,(int32_t)svc_alarm_id(s_ring_alarm),1,DIAG_RESULT_SUCCESS,0);
        svc_alarm_stop();
    }
    if (ring_screen(&R_ALARM, s_ring_alarm >= 0)) svc_alarm_stop();
}

/* ------------------------------------------------------------------ timer */
const svc_timer_t *svc_timer(void) { return &s_timer; }
void svc_timer_start(uint32_t ms)
{
    if (!ms) return;
    s_timer_generation++;
    action_trace(DIAG_DOMAIN_TIMER,DIAG_ACT_START,(int32_t)s_timer_generation,s_timer.state,DIAG_RESULT_SUCCESS,(int32_t)ms);
    s_timer.total_ms = ms;
    s_timer.left_ms = ms;
    s_timer_end = hal_millis() + ms;
    s_timer.state = 1;
}
void svc_timer_pause(void)
{
    if (s_timer.state != 1) return;
    action_trace(DIAG_DOMAIN_TIMER,DIAG_ACT_PAUSE,(int32_t)s_timer_generation,1,DIAG_RESULT_SUCCESS,2);
    int32_t left = (int32_t)(s_timer_end - hal_millis());
    s_timer.left_ms = left > 0 ? (uint32_t)left : 0;
    s_timer.state = 2;
}
void svc_timer_resume(void)
{
    if (s_timer.state != 2) return;
    action_trace(DIAG_DOMAIN_TIMER,DIAG_ACT_RESUME,(int32_t)s_timer_generation,2,DIAG_RESULT_SUCCESS,1);
    s_timer_end = hal_millis() + s_timer.left_ms;
    s_timer.state = 1;
}
void svc_timer_cancel(void)
{
    if(s_timer.state) action_trace(DIAG_DOMAIN_TIMER,DIAG_ACT_CANCEL,(int32_t)s_timer_generation,s_timer.state,DIAG_RESULT_SUCCESS,0);
    s_timer.state = 0;
    s_timer.left_ms = 0;
}
void svc_timer_add(uint32_t ms)
{
    if (s_timer.state == 1) { s_timer_end += ms; s_timer.total_ms += ms; s_timer.left_ms += ms; }
    else if (s_timer.state == 2) { s_timer.left_ms += ms; s_timer.total_ms += ms; }
    else if (s_timer.state == 3) svc_timer_start(ms);   /* "+1 min" from the ring screen */
}
static void timer_tick(uint32_t now)
{
    if (s_timer.state == 1) {
        int32_t left = (int32_t)(s_timer_end - now);
        if (left <= 0) {
            action_trace(DIAG_DOMAIN_TIMER,DIAG_ACT_FIRE,(int32_t)s_timer_generation,1,DIAG_RESULT_SUCCESS,3);
            s_timer.left_ms = 0;
            s_timer.state = 3;
            s_timer_ring_t0 = now;
        } else s_timer.left_ms = (uint32_t)left;
    }
    if (s_timer.state == 3 && now - s_timer_ring_t0 > RING_MAX_MS) {
        action_trace(DIAG_DOMAIN_TIMER,DIAG_ACT_TIMEOUT,(int32_t)s_timer_generation,3,DIAG_RESULT_SUCCESS,0);
        svc_timer_cancel();
    }
    if (ring_screen(&R_TIMER, s_timer.state == 3)) svc_timer_cancel();
}

/* ------------------------------------------------------------------ stopwatch */
const svc_stopwatch_t *svc_stopwatch(void) { return &s_sw; }
void svc_sw_start(void)
{
    if (s_sw.running) return;
    s_sw_t0 = hal_millis() - s_sw.elapsed_ms;
    s_sw.running = 1;
}
void svc_sw_stop(void)
{
    if (!s_sw.running) return;
    s_sw.elapsed_ms = hal_millis() - s_sw_t0;
    s_sw.running = 0;
}
void svc_sw_lap(void)
{
    if (!s_sw.running || s_sw.nlaps >= SVC_LAPS) return;
    s_sw.elapsed_ms = hal_millis() - s_sw_t0;
    s_sw.laps[s_sw.nlaps++] = s_sw.elapsed_ms;
}
void svc_sw_reset(void)
{
    int run = s_sw.running;
    memset(&s_sw, 0, sizeof s_sw);
    s_sw.running = run;
    s_sw_t0 = hal_millis();
}

/* ------------------------------------------------------------------ calls, find */
const svc_call_t *svc_call(void) { return &s_call; }
uint32_t svc_call_generation(void) { return s_call_generation; }
void svc_call_accept(void)
{
    if (s_call.state != 1) return;
    action_trace(DIAG_DOMAIN_PHONE,DIAG_ACT_START,(int32_t)s_call_generation,s_call.state,DIAG_RESULT_PENDING,2);
    txf("{\"t\":\"call\",\"n\":\"ACCEPT\"}");
    s_call.state = 2;
    s_call.t0_ms = hal_millis();
}
void svc_call_reject(void)
{
    if (!s_call.state) return;
    action_trace(DIAG_DOMAIN_PHONE,DIAG_ACT_REJECT,(int32_t)s_call_generation,s_call.state,DIAG_RESULT_PENDING,0);
    txf("{\"t\":\"call\",\"n\":\"%s\"}", s_call.state == 1 ? "REJECT" : "END");
    s_call.state = 0;
}
void svc_call_hangup(void)
{
    if (!s_call.state) return;
    action_trace(DIAG_DOMAIN_PHONE,DIAG_ACT_STOP,(int32_t)s_call_generation,s_call.state,DIAG_RESULT_PENDING,0);
    txf("{\"t\":\"call\",\"n\":\"END\"}");
    s_call.state = 0;
}
void svc_find_phone(int on)
{
    if(s_find_phone!=(on!=0)) action_trace(DIAG_DOMAIN_FIND,on?DIAG_ACT_START:DIAG_ACT_STOP,1,s_find_phone,DIAG_RESULT_PENDING,on!=0);
    s_find_phone = on ? 1 : 0;
    if (hal_phone_connected()) txf("{\"t\":\"findPhone\",\"n\":%s}", on ? "true" : "false");
    else if (on) hal_find_phone();                 /* companion bridge rings itself */
}
int  svc_find_phone_active(void) { return s_find_phone; }
int  svc_find_watch_active(void) { return s_find_watch; }
void svc_find_watch_stop(void) { s_find_watch = 0; }
static void call_find_tick(uint32_t now)
{
    /* G99: an incoming call nobody answered must not hold the screen and haptics forever when the phone
     * never reports the end (the other rings are already bounded by RING_MAX_MS). */
    if (s_call.state == 1 && now - s_call.t0_ms > RING_MAX_MS) s_call.state = 0;
    if (ring_screen(&R_CALL, s_call.state == 1 || (s_call.state == 2 && R_CALL.pushed)) && s_call.state == 1) {
        /* swiped away while ringing: leave the call to the phone (no message), stop ringing */
        s_call.state = 0;
    }
    if (s_find_watch && now - s_find_watch_t0 > 2u * RING_MAX_MS) s_find_watch = 0;
    if (ring_screen(&R_FIND, s_find_watch)) s_find_watch = 0;
}

/* ------------------------------------------------------------------ system */
const svc_sys_t *svc_sys(void) { return &s_sys; }
static void sys_tick(void)
{
    hal_power p;
    hal_power_get(&p);
    s_sys.battery_pct = p.battery_percent < 0 ? 0 : p.battery_percent;
    s_sys.external = s_sys.charging = p.external_power == 1;
    s_sys.rail_mv = p.rail_mv;
    s_sys.battery_valid = p.battery_valid;
    s_sys.phone_connected = s_connected;
    s_sys.wifi_up = hal_status(HAL_FEAT_WIFI) == HAL_CONN_UP;
}

/* ------------------------------------------------------------------ Gadgetbridge */
static void apply_time(const gb_frame *f)
{
    int tz = nos_prefs.tz_min;
    if (f->has_tz) tz = (int)floorf(f->tz_hours * 60.0f + 0.5f);
    if (tz < -720 || tz > 840) return;
    if (tz != nos_prefs.tz_min) { nos_prefs.tz_min = (int16_t)tz; prefs_save(); }
    hal_set_tz_min(tz);
    if (!f->has_time || f->epoch < 946684800LL) return;
    int64_t local = f->epoch + (int64_t)tz * 60;
    int64_t days = local >= 0 ? local / 86400 : (local - 86399) / 86400;
    int64_t sec = local - days * 86400;
    hal_datetime dt;
    memset(&dt, 0, sizeof dt);
    civil_from_days(days, &dt.year, &dt.month, &dt.day);
    dt.weekday = (int)(((days % 7) + 11) % 7);    /* 1970-01-01 was a Thursday */
    dt.hour = (int)(sec / 3600);
    dt.minute = (int)(sec / 60 % 60);
    dt.second = (int)(sec % 60);
    hal_set_datetime(&dt);
}

static void gb_notify(const gb_json *j, int change)
{
    char app[20], title[40], body[160], tmp[40];
    int id = gb_int(j, 0, "id", 0);
    if (change) {                                  /* notify~: update fields in place */
        int i = notif_find(id);
        if (i < 0 || !B->phone[i]) return;
        if (gb_has(j, 0, "title")) gb_strk(j, 0, "title", B->n[i].title, sizeof B->n[i].title);
        if (gb_has(j, 0, "body")) gb_strk(j, 0, "body", B->n[i].body, sizeof B->n[i].body);
        B->n[i].read = 0;
        s_notif_revision++;
        return;
    }
    gb_strk(j, 0, "src", app, sizeof app);
    gb_strk(j, 0, "title", title, sizeof title);
    if (!title[0]) gb_strk(j, 0, "sender", title, sizeof title);
    gb_strk(j, 0, "body", body, sizeof body);
    gb_strk(j, 0, "subject", tmp, sizeof tmp);
    if (!title[0]) snprintf(title, sizeof title, "%s", tmp);
    else if (!body[0] && tmp[0]) snprintf(body, sizeof body, "%s", tmp);
    notif_post(app[0] ? app : "Phone", title, body, id, 1, 0, 1, 0);
}

static void gb_call(const gb_json *j)
{
    int previous=s_call.state;uint32_t generation=s_call_generation;
    char cmd[16], name[32], num[24];
    gb_strk(j, 0, "cmd", cmd, sizeof cmd);
    gb_strk(j, 0, "name", name, sizeof name);
    gb_strk(j, 0, "number", num, sizeof num);
    if (ieq(cmd, "incoming")) {
        if(s_call.state!=1 || strcmp(s_call.number,num) ||
           (name[0] && strcmp(s_call.name,name))) s_call_generation++;
        s_call.state = 1;
        snprintf(s_call.name, sizeof s_call.name, "%s", name[0] ? name : num[0] ? num : "Unknown");
        snprintf(s_call.number, sizeof s_call.number, "%s", num);
        s_call.t0_ms = hal_millis();
    } else if (ieq(cmd, "outgoing") || ieq(cmd, "start") || ieq(cmd, "accept")) {
        if (!s_call.state) s_call_generation++;
        if (s_call.state != 2) s_call.t0_ms = hal_millis();
        if (name[0] || num[0]) {
            snprintf(s_call.name, sizeof s_call.name, "%s", name[0] ? name : num);
            snprintf(s_call.number, sizeof s_call.number, "%s", num);
        }
        s_call.state = 2;
    } else if (ieq(cmd, "end") || ieq(cmd, "reject") || ieq(cmd, "missed") || ieq(cmd, "ignore")) {
        s_call.state = 0;
    }
    if(previous!=s_call.state || generation!=s_call_generation)
        action_trace(DIAG_DOMAIN_PHONE,DIAG_ACT_RECEIVE,(int32_t)s_call_generation,previous,DIAG_RESULT_SUCCESS,s_call.state);
}

static void gb_weather(const gb_json *j)
{
    uint32_t generation = s_wx.generation + 1;
    memset(&s_wx, 0, sizeof s_wx);
    s_wx.valid = 1;
    s_wx.source = 1; s_wx.generation = generation;
    s_wx.utc_offset_seconds = nos_prefs.tz_min * 60;
    snprintf(s_wx.status, sizeof s_wx.status, "Phone weather");
    s_wx.temp_c = kelvin_c(gb_numk(j, 0, "temp", 273.15));
    double feels = gb_numk(j, 0, "feels", gb_numk(j, 0, "feel", -1000));
    s_wx.feels_c = feels > -999 ? kelvin_c(feels) : s_wx.temp_c;
    s_wx.hi_c = kelvin_c(gb_numk(j, 0, "hi", gb_numk(j, 0, "temp", 273.15)));
    s_wx.lo_c = kelvin_c(gb_numk(j, 0, "lo", gb_numk(j, 0, "temp", 273.15)));
    s_wx.cond = wx_from_owm(gb_int(j, 0, "code", 803));
    char txt[sizeof s_wx.text];
    gb_strk(j, 0, "txt", txt, sizeof txt);
    if (txt[0] >= 'a' && txt[0] <= 'z') txt[0] = (char)(txt[0] - 32);   /* sentence case */
    snprintf(s_wx.text, sizeof s_wx.text, "%s", txt[0] ? txt : wx_text(s_wx.cond));
    s_wx.humidity = gb_int(j, 0, "hum", -1);
    s_wx.rain_pct = gb_int(j, 0, "rain", -1);
    s_wx.uv = gb_int(j, 0, "uv", -1);
    s_wx.wind_kph = (float)gb_numk(j, 0, "wind", -1);
    s_wx.wind_dir = gb_int(j, 0, "wdir", -1);
    gb_strk(j, 0, "loc", s_wx.loc, sizeof s_wx.loc);
    s_wx.updated_epoch = (uint32_t)hal_epoch_seconds();
    s_wx.ndays = 1;
    s_wx.days[0].hi = s_wx.hi_c;
    s_wx.days[0].lo = s_wx.lo_c;
    s_wx.days[0].cond = s_wx.cond;
    /* Do not combine forecasts from a separately configured Wi-Fi location. */
    s_wx.days_epoch[0] = (int64_t)s_wx.updated_epoch;
    if (s_wx.ndays >= 1) {                         /* today's row follows the phone */
        s_wx.days[0].hi = s_wx.hi_c;
        s_wx.days[0].lo = s_wx.lo_c;
        s_wx.days[0].cond = s_wx.cond;
    }
    s_wx_gb_ms = hal_millis() | 1;
}

static void gb_alarms(const gb_json *j)
{
    int d = gb_get(j, 0, "d");
    if (d < 0 || j->tok[d].type != GBJ_ARR) return;
    if (s_ring_alarm >= 0) svc_alarm_stop();
    s_snooze_idx = -1;
    int n = j->tok[d].count;
    if (n > SVC_ALARM_MAX) n = SVC_ALARM_MAX;
    memset(s_alarms, 0, sizeof s_alarms);
    memset(s_alarm_ids, 0, sizeof s_alarm_ids);
    s_alarm_generation++;
    s_nalarms = 0;
    for (int i = 0; i < n; i++) {
        int it = gb_item(j, d, i);
        int h = gb_int(j, it, "h", -1), m = gb_int(j, it, "m", -1);
        if (h < 0 || h > 23 || m < 0 || m > 59) continue;
        int rep = gb_int(j, it, "rep", 0);         /* Gadgetbridge: bit0 = Monday ... bit6 = Sunday */
        s_alarm_ids[s_nalarms] = alarm_new_id();
        svc_alarm *a = &s_alarms[s_nalarms++];
        a->h = (uint8_t)h;
        a->m = (uint8_t)m;
        a->days = (uint8_t)((((rep & 63) << 1) | ((rep >> 6) & 1)) & 127);   /* -> bit0 = Sunday */
        a->on = (uint8_t)gb_bool(j, it, "on", 1);
        snprintf(a->label, sizeof a->label, "Phone");
    }
    svc_alarm_save();
}

static void gb_act_send(void)
{
    uint32_t steps = (uint32_t)s_act.steps;
    uint32_t d = steps >= s_act_rt_steps ? steps - s_act_rt_steps : 0;
    s_act_rt_steps = steps;
    txf("{\"t\":\"act\",\"stp\":%u,\"hrm\":%d,\"rt\":1}", (unsigned)d, s_act_rt_hrm ? s_heart.bpm_last : 0);
}

static void gb_message(const gb_json *j)
{
    char t[20];
    gb_strk(j, 0, "t", t, sizeof t);
    if (!strcmp(t, "notify")) gb_notify(j, 0);
    else if (!strcmp(t, "notify~")) gb_notify(j, 1);
    else if (!strcmp(t, "notify-")) notif_remove_at(notif_find(gb_int(j, 0, "id", 0)));
    else if (!strcmp(t, "call")) gb_call(j);
    else if (!strcmp(t, "musicinfo")) {
        gb_strk(j, 0, "track", s_music.track, sizeof s_music.track);
        gb_strk(j, 0, "artist", s_music.artist, sizeof s_music.artist);
        gb_strk(j, 0, "album", s_music.album, sizeof s_music.album);
        int dur = gb_int(j, 0, "dur", 0);
        s_music.dur_ms = dur > 0 ? (uint32_t)dur * 1000u : 0;
        if (s_music_src != 1) { s_music.playing = 0; music_set_pos(0); s_music.volume = -1; }
        s_music.have = 1;
        s_music_src = 1;
    } else if (!strcmp(t, "musicstate")) {
        char st[12];
        gb_strk(j, 0, "state", st, sizeof st);
        s_music.playing = ieq(st, "play");
        int pos = gb_int(j, 0, "position", -1);
        if (pos >= 0) music_set_pos((uint32_t)pos * 1000u);
        else music_set_pos(s_music.pos_ms);
        if (s_music_src != 1) { s_music.volume = -1; s_music_src = 1; }
        s_music.have = 1;
    } else if (!strcmp(t, "weather")) gb_weather(j);
    else if (!strcmp(t, "find")) {
        int on = gb_bool(j, 0, "n", 0);
        if (on && !s_find_watch) s_find_watch_t0 = hal_millis();
        s_find_watch = on;
    } else if (!strcmp(t, "alarm")) gb_alarms(j);
    else if (!strcmp(t, "act")) {
        s_act_rt_hrm = gb_bool(j, 0, "hrm", 0);
        s_act_rt_stp = gb_bool(j, 0, "stp", 0);
        int iv = gb_int(j, 0, "int", 10);
        s_act_rt_int = (s_act_rt_hrm || s_act_rt_stp) ? (iv < 1 ? 1 : iv > 600 ? 600 : iv) : 0;
        s_act_rt_steps = (uint32_t)s_act.steps;
        gb_act_send();                             /* answer right away, then every int s */
        s_act_rt_next = hal_millis() + (uint32_t)s_act_rt_int * 1000u;
    } else if (!strcmp(t, "vibrate")) hal_haptic(HAPTIC_BUZZ);
    else if (!strcmp(t, "is_gps_active")) txf("{\"t\":\"gps_power\",\"status\":false}");
    /* calendar, nav, gps, http, actfetch ...: not used by NOS yet */
}

static void handle_line(const char *line, int len)
{
    gb_frame f;
    if (!B || !gb_frame_line(line, len, &f)) return;
    if (f.has_time || f.has_tz) apply_time(&f);
    if (f.json && gb_parse(&B->json, f.json, f.json_len) == 0 && B->json.tok[0].type == GBJ_OBJ)
        gb_message(&B->json);
}

void svc_phone_inject(const char *line)
{
#ifndef ESP_PLATFORM
    if (nos_phone_sim_cmd(line)) return;           /* "sim ..." commands for the fake phone */
#endif
    if (!line || !B) return;
    /* the sim script / USB console cannot type \x10, so accept the line as is */
    int n = (int)strlen(line);
    if (n >= GB_LINE_MAX) return;
    memcpy(B->line, line, (size_t)n + 1);
    handle_line(B->line, n);
}

static void phone_tick(uint32_t now)
{
    int c = hal_phone_connected();
    if (c != s_connected) {
        s_connected = c;
        s_sys.phone_connected = c;
        if (c) s_ver_due = now + 500;
        else {
            s_act_rt_int = 0;
            s_find_phone = 0;
            if (s_call.state) s_call.state = 0;
            if (s_music_src == 1) { memset(&s_music, 0, sizeof s_music); s_music.volume = -1; s_music_src = 0; }
        }
    }
    if (c && s_ver_due && (int32_t)(now - s_ver_due) >= 0) {
        s_ver_due = 0;
        txf("{\"t\":\"ver\",\"fw\":\"%s\",\"hw\":\"%s\"}", FW_VERSION, HW_NAME);
        if (send_status()) s_status_ms = now;
    }
    if (!B) return;
    for (int k = 0; k < LINES_PER_FRAME; k++) {
        int n = hal_phone_read_line(B->line, GB_LINE_MAX);
        if (n <= 0) break;
        B->line[n < GB_LINE_MAX ? n : GB_LINE_MAX - 1] = 0;
        handle_line(B->line, (int)strlen(B->line));
    }
    if (!c) return;
    if (s_act_rt_int && (int32_t)(now - s_act_rt_next) >= 0) {
        gb_act_send();
        s_act_rt_next = now + (uint32_t)s_act_rt_int * 1000u;
    }
    /* battery to the phone: every 10 min, or on a 5% / charger change */
    if (now - s_status_ms > 600000u ||
        (s_status_pct >= 0 && (s_sys.battery_pct - s_status_pct >= 5 || s_status_pct - s_sys.battery_pct >= 5)) ||
        (s_status_chg >= 0 && s_sys.charging != s_status_chg)) {
        if (send_status()) s_status_ms = now;
    }
}

static void companion_tick(void)
{
    char app[24], title[64];
    for (int k = 0; k < 4 && hal_notification_poll(app, sizeof app, title, sizeof title); k++)
        notif_post(app, title, "", s_local_id--, 0, 0, 1, 0);
}

/* ------------------------------------------------------------------ init, tick */
#ifndef ESP_PLATFORM
static void seed_sim(void)
{
    uint32_t now = (uint32_t)hal_epoch_seconds();
    notif_post("Weather", "Rain soon", "Light rain starting in 20 min", s_local_id--, 0, now - 65 * 60, 0, 1);
    notif_post("Gmail", "Figma", "Isaiah shared \xe2\x80\x98Watch OS v3\xe2\x80\x99 with you", s_local_id--, 0, now - 38 * 60, 0, 1);
    notif_post("Calendar", "Design review", "Today 15:30 \xc2\xb7 Studio 2", s_local_id--, 0, now - 14 * 60, 0, 0);
    notif_post("Messages", "Maya", "Running 10 min late, save me a seat", s_local_id--, 0, now - 2 * 60, 0, 0);
}
#endif

void svc_init(void)
{
#ifndef ESP_PLATFORM
    diag_log_init();
#endif
    if (!B) {
        B = (svc_big *)hal_big_alloc(sizeof *B);
        if (B) memset(B, 0, sizeof *B);
        else hal_log("svc: no PSRAM for notifications");
    }
    s_nn = 0;
    s_music.volume = -1;
    s_act.goal = nos_prefs.step_goal_100 ? nos_prefs.step_goal_100 * 100 : 8000;
    alarm_load();
    act_init();
#ifndef ESP_PLATFORM
    seed_sim();
#endif
    sys_tick();
    act_tick(hal_millis());
    heart_tick();
    wx_from_hal();
    s_slow_ms = s_hr_ms = s_wx_ms = hal_millis();
    tuning_init();
    sports_init();
    workout_init();
}

void svc_tick(float dt)
{
#ifndef ESP_PLATFORM
    diag_log_tick();
#endif
    svc_track_tick(dt);              /* GPS track recording runs with Navigate closed */
    svc_memo_tick(dt);               /* and voice memos with the Recorder closed */
    svc_cell_tick(dt);               /* acknowledge send outcomes even after leaving SMS */
    uint32_t now = hal_millis();
    phone_tick(now);
    companion_tick();
    music_tick(now);
    if (now - s_slow_ms >= 1000) {
        s_slow_ms = now;
        sys_tick();
        act_tick(now);
        sports_tick();
    }
    workout_tick();                 /* workout recording survives leaving Sports */
    {   /* the raw sensor journal is open only while a workout records (G11) */
        static int raw_held;
        int on = workout_get()->state == WORKOUT_RUNNING;
        if (on != raw_held) { raw_held = on; sports_raw_set(SPORTS_RAW_WORKOUT, on); }
    }
    if (now - s_hr_ms >= 60000) { s_hr_ms = now; heart_tick(); }
    wx_tick(now);
    if (s_sw.running) s_sw.elapsed_ms = now - s_sw_t0;
    timer_tick(now);
    alarm_tick(now);
    call_find_tick(now);
    ringer_tick(now);
}
