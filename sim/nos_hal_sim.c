/* nos_hal_sim.c — core/hal.h for the NOS simulator.
 *
 * Adapted from sim/main_win32.c without anything that belongs to the old UI (wos,
 * apps, flat60, pip). Differences that matter for testing:
 *  - Two clocks: real time in the interactive window, a virtual clock in scripted
 *    runs (sim_virtual), so the same script always produces the same frames.
 *  - A simulated RTC that scripts can set (`time HH:MM:SS`), advancing on the
 *    active clock.
 *  - The key/value store lives in RAM; the interactive window also persists it to
 *    files, scripted runs never read or write files (deterministic boots).
 *  - Notifications posted by the "companion" are queued and handed out by
 *    hal_notification_poll(), like the device transport.
 * hal_haptic() and the rest of nos/hal_ext.h are in nos_ext_sim.c / nos_ext_stub.c. */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hal.h"
#include "nos_sim.h"

int    sim_virtual;
double sim_vclock_ms;
int    sim_batt_pct = 78;
int    sim_charging;
int    sim_button_down;
int    sim_store_disabled;
char   sim_store_dir[260] = "nos_store";
int    sim_tz_min = 330;
int    sim_steps = 4210;
/* core/wake_service.c reads this old-UI setting (core/settings.c is not built). */
int    g_setting_tilt_wake = 1;

/* ---- clocks ------------------------------------------------------------------- */
static LARGE_INTEGER s_qpf, s_t0;
static int s_clock_init;
static void clock_init(void)
{
    if (s_clock_init) return;
    QueryPerformanceFrequency(&s_qpf);
    QueryPerformanceCounter(&s_t0);
    s_clock_init = 1;
}
uint32_t sim_real_ms(void)
{
    clock_init();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint32_t)((t.QuadPart - s_t0.QuadPart) * 1000 / s_qpf.QuadPart);
}
static double active_ms(void)
{
    if (sim_virtual) return sim_vclock_ms;
    clock_init();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - s_t0.QuadPart) * 1000.0 / (double)s_qpf.QuadPart;
}

static int s_latched;
static double s_latch_ms;
uint32_t hal_millis(void) { return (uint32_t)(s_latched ? s_latch_ms : active_ms()); }
uint32_t hal_micros(void)                       /* profiling: always real time */
{
    clock_init();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint32_t)((t.QuadPart - s_t0.QuadPart) * 1000000 / s_qpf.QuadPart);
}

/* ---- RTC ------------------------------------------------------------------------ */
static int64_t s_rtc_base_s = 0;     /* local seconds since 1970 at s_rtc_base_ms */
static double  s_rtc_base_ms;
static int     s_rtc_init;

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
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

static void rtc_default(void)
{
    if (s_rtc_init) return;
    s_rtc_init = 1;
    if (sim_virtual) {
        s_rtc_base_s = days_from_civil(2026, 10, 11) * 86400 + 10 * 3600 + 9 * 60 + 30;
        s_rtc_base_ms = active_ms();
    } else sim_rtc_from_pc();
}
static int64_t rtc_now_ms(void)       /* local ms since 1970 */
{
    rtc_default();
    double now = s_latched ? s_latch_ms : active_ms();
    return s_rtc_base_s * 1000 + (int64_t)floor(now - s_rtc_base_ms);
}
void sim_rtc_from_pc(void)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    s_rtc_init = 1;
    s_rtc_base_s = days_from_civil(st.wYear, st.wMonth, st.wDay) * 86400 +
                   st.wHour * 3600 + st.wMinute * 60 + st.wSecond;
    s_rtc_base_ms = active_ms() - st.wMilliseconds;
}
void sim_rtc_set_time(int h, int m, int s)
{
    int64_t now = rtc_now_ms() / 1000;
    int64_t day = now >= 0 ? now / 86400 : (now - 86399) / 86400;
    s_rtc_base_s = day * 86400 + h * 3600 + m * 60 + s;
    s_rtc_base_ms = s_latched ? s_latch_ms : active_ms();
}
void sim_rtc_set_date(int y, int mo, int d)
{
    int64_t now = rtc_now_ms();
    int64_t sod = ((now / 1000) % 86400 + 86400) % 86400;
    s_rtc_base_s = days_from_civil(y, mo, d) * 86400 + sod;
    s_rtc_base_ms = (s_latched ? s_latch_ms : active_ms()) - (double)(now % 1000);
}

static void datetime_at(int64_t ms, hal_datetime *out)
{
    int64_t s = ms >= 0 ? ms / 1000 : (ms - 999) / 1000;
    int64_t day = s >= 0 ? s / 86400 : (s - 86399) / 86400;
    int sod = (int)(s - day * 86400);
    civil_from_days(day, &out->year, &out->month, &out->day);
    out->weekday = (int)(((day % 7) + 11) % 7);   /* 1970-01-01 was a Thursday (4) */
    out->hour = sod / 3600;
    out->minute = sod / 60 % 60;
    out->second = sod % 60;
    out->millis = (int)(ms - s * 1000);
}

static hal_datetime s_latch_dt;
void hal_frame_begin(void)
{
    s_latch_ms = active_ms();
    s_latched = 0;
    datetime_at(rtc_now_ms(), &s_latch_dt);
    s_latched = 1;
}
void hal_frame_end(void) { s_latched = 0; }

void hal_get_datetime(hal_datetime *out)
{
    if (s_latched) { *out = s_latch_dt; return; }
    datetime_at(rtc_now_ms(), out);
}
void hal_set_datetime(const hal_datetime *dt)
{
    sim_rtc_set_date(dt->year, dt->month, dt->day);
    sim_rtc_set_time(dt->hour, dt->minute, dt->second);
}
uint64_t hal_epoch_seconds(void)
{
    int64_t s = rtc_now_ms() / 1000 - (int64_t)sim_tz_min * 60;
    return s > 0 ? (uint64_t)s : 0;
}

/* ---- power ---------------------------------------------------------------------- */
int  hal_battery_percent(void) { return sim_batt_pct; }
int  hal_battery_mv(void) { return 3300 + sim_batt_pct * 9 + (sim_charging ? 250 : 0); }
void hal_power_get(hal_power *out)
{
    *out = (hal_power){ hal_battery_mv(), sim_batt_pct, !sim_charging, sim_charging ? 1 : 0, 1, 0 };
}
void hal_batt_log_tick(void) {}

/* ---- key/value store -------------------------------------------------------------
 * RAM first; the interactive window also mirrors to sim_store_dir/store_KEY.dat. */
#define STORE_MAX 32
static struct { char key[32]; void *data; int size; } s_store[STORE_MAX];
static int s_nstore;

static int store_find(const char *key)
{
    for (int i = 0; i < s_nstore; i++) if (!strcmp(s_store[i].key, key)) return i;
    return -1;
}
static void store_path(char *out, int n, const char *key)
{
    snprintf(out, (size_t)n, "%s/store_%s.dat", sim_store_dir, key);
}
int hal_store_save_checked(const char *key, const void *data, int size)
{
    int i = store_find(key);
    if (i < 0) {
        if (s_nstore >= STORE_MAX) return 0;
        i = s_nstore++;
        snprintf(s_store[i].key, sizeof s_store[i].key, "%s", key);
        s_store[i].data = 0;
    }
    free(s_store[i].data);
    s_store[i].data = malloc((size_t)size);
    if (!s_store[i].data) return 0;
    memcpy(s_store[i].data, data, (size_t)size);
    s_store[i].size = size;
    if (sim_store_disabled) return 1;
    char path[300];
    CreateDirectoryA(sim_store_dir, NULL);
    store_path(path, sizeof path, key);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, (size_t)size, f) == (size_t)size;
    if (fclose(f) != 0) ok = 0;
    return ok;
}
void hal_store_save(const char *key, const void *data, int size) { (void)hal_store_save_checked(key, data, size); }
int hal_store_load(const char *key, void *data, int size)
{
    int i = store_find(key);
    if (i >= 0) {
        if (s_store[i].size != size) return 0;
        memcpy(data, s_store[i].data, (size_t)size);
        return 1;
    }
    if (sim_store_disabled) return 0;
    char path[300];
    store_path(path, sizeof path, key);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int n = (int)fread(data, 1, (size_t)size, f);
    fclose(f);
    return n == size;
}

/* ---- display / log ------------------------------------------------------------- */
int sim_backlight = 200;
void hal_set_backlight(uint8_t level) { sim_backlight = level; }

#define LOG_KEEP 32
static char s_log[LOG_KEEP][96];
static int s_nlog;
int sim_log_quiet;
void hal_log(const char *msg)
{
    snprintf(s_log[s_nlog % LOG_KEEP], sizeof s_log[0], "%s", msg ? msg : "");
    s_nlog++;
    if (!sim_log_quiet) printf("[watch] %s\n", msg ? msg : "");
}
int hal_log_count(void) { return s_nlog < LOG_KEEP ? s_nlog : LOG_KEEP; }
const char *hal_log_get(int i)
{
    int n = hal_log_count();
    if (i < 0 || i >= n) return "";
    return s_log[(s_nlog - n + i) % LOG_KEEP];
}
void hal_log_clear(void) { s_nlog = 0; }

/* ---- sound (old board path; NOS uses hal_sfx in hal_ext) ----------------------- */
void hal_beep(int f, int ms) { (void)f; (void)ms; }
void hal_play(int snd) { (void)snd; }
void hal_find_phone(void) { hal_log("find my phone -> ring companion"); }
int  hal_locate_active(void) { return 0; }

/* ---- radios ---------------------------------------------------------------------- */
int hal_wifi_scan(hal_wifi_ap *out, int max)
{
    static const hal_wifi_ap fake[] = {
        { "HomeNet-5G", -48, 36 }, { "TP-Link_A2F0", -61, 6 }, { "makerspace", -67, 11 },
        { "SKY9821", -72, 1 }, { "AndroidAP", -79, 6 }, { "eduroam", -83, 44 }, { "Xfinity", -88, 11 },
    };
    int n = (int)(sizeof fake / sizeof fake[0]);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = fake[i];
    return n;
}
void hal_wifi_connect(const char *ssid, const char *pass) { (void)ssid; (void)pass; }
void hal_ble_status(char *buf, int n) { snprintf(buf, (size_t)n, "sim: no radio"); }
int hal_ble_scan(hal_ble_dev *out, int max)
{
    static const hal_ble_dev fake[] = {
        { "Galaxy Buds", "5C:3A:45:1B:2E:0F", -52 }, { "Mi Band 7", "D8:1F:12:9A:44:70", -61 },
        { "JBL Flip 6", "10:94:BB:5C:0A:22", -68 }, { "AirPods Pro", "A4:83:E7:2D:66:91", -74 },
        { "(unknown)", "F1:22:8C:03:5D:AE", -83 }, { "TV-Living", "00:1A:7D:DA:71:13", -88 },
    };
    int n = (int)(sizeof fake / sizeof fake[0]);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = fake[i];
    return n;
}

static int s_air_run;
int  hal_wifi_air_running(void) { return s_air_run; }
int  hal_wifi_air_channel(void) { return 1 + (int)(hal_millis() / 120 % HAL_AIR_NCH); }
int  hal_wifi_air_start(void) { s_air_run = 1; return 1; }
void hal_wifi_air_stop(void) { s_air_run = 0; }
static uint8_t air_fake(int ch, double t)
{
    static const int peaks[3] = { 1, 6, 11 };
    static const double amp[3] = { 210, 165, 120 };
    double base = 0;
    for (int p = 0; p < 3; p++) { double d = ch - peaks[p]; base += amp[p] * exp(-(d * d) / 2.2); }
    base = base * (0.75 + 0.25 * sin(t * 0.8 + ch)) + 6;
    return (uint8_t)(base < 0 ? 0 : base > 255 ? 255 : base);
}
void hal_wifi_air_get(hal_air_chan *out)
{
    double t = hal_millis() / 1000.0;
    for (int i = 0; i < HAL_AIR_NCH; i++) {
        uint8_t u = air_fake(i + 1, t);
        out[i].util = u; out[i].pkts = (uint16_t)(u * 1.7);
        out[i].rssi_max = u ? (int8_t)(-92 + u / 6) : 0;
        out[i].mgmt = u / 5; out[i].data = u; out[i].ctrl = u / 8;
    }
}
void hal_wifi_air_hist(uint8_t *out)
{
    double t = hal_millis() / 1000.0;
    for (int c = 0; c < HAL_AIR_NCH; c++)
        for (int k = 0; k < HAL_AIR_HIST; k++)
            out[c * HAL_AIR_HIST + k] = air_fake(c + 1, t - (HAL_AIR_HIST - 1 - k) * 0.3);
}

hal_conn hal_status(int feature)
{
    switch (feature) {
    case HAL_FEAT_BLE: return HAL_CONN_UP;
    case HAL_FEAT_WIFI: return HAL_CONN_UP;
    case HAL_FEAT_NET: return HAL_CONN_UP;
    case HAL_FEAT_SYNC: return HAL_CONN_UP;
    case HAL_FEAT_AI: return HAL_CONN_OFF;
    case HAL_FEAT_GPS: return HAL_CONN_DOWN;
    }
    return HAL_CONN_OFF;
}
int hal_location(char *city, int n, float *lat, float *lon)
{
    if (city && n > 0) snprintf(city, (size_t)n, "London");
    if (lat) *lat = 51.5072f;
    if (lon) *lon = -0.1276f;
    return 1;
}
void hal_set_aod(int on) { (void)on; }
void hal_time_resync(void) {}

/* ---- media ----------------------------------------------------------------------- */
int hal_media_get(char *title, int tn, char *artist, int an, int *playing)
{ (void)title; (void)tn; (void)artist; (void)an; (void)playing; return 0; }
void hal_media_cmd(int cmd) { (void)cmd; }
int hal_media_time(int *pos, int *dur) { (void)pos; (void)dur; return 0; }
void hal_media_seek(int pos) { (void)pos; }
void hal_media_vol(int pct) { (void)pct; }
int hal_media_colors(uint16_t *ring, uint16_t *ab, uint16_t *sb, uint16_t *at, uint16_t *st)
{ (void)ring; (void)ab; (void)sb; (void)at; (void)st; return 0; }
const uint16_t *hal_media_cover(int *px, unsigned *gen) { (void)px; (void)gen; return 0; }

/* ---- sensors --------------------------------------------------------------------- */
int   sim_imu_override;
float sim_imu_a[3] = { 0, 0, -1 };
int hal_imu_read(hal_imu *o)
{
    if (sim_imu_override) {
        *o = (hal_imu){ sim_imu_a[0], sim_imu_a[1], sim_imu_a[2], 0, 0, 0 };
        return 1;
    }
    float t = hal_millis() / 1000.0f;        /* gentle simulated sway */
    o->ax = 0.05f * sinf(t * 1.3f);
    o->ay = 0.05f * sinf(t * 0.9f + 1.0f);
    o->az = -1.0f;
    o->gx = o->gy = o->gz = 0;
    return 1;
}
int hal_imu_sample(hal_imu *o, uint32_t *sample_ms) { *sample_ms = hal_millis(); return hal_imu_read(o); }

void hal_storage_get(hal_storage *out)
{
    memset(out, 0, sizeof *out);
    out->mounted = 1;
    out->total_bytes = 32ull << 30;
    out->free_bytes = 29ull << 30;
    snprintf(out->message, sizeof out->message, "Simulator SD");
}
int hal_audio_available(void) { return 1; }
int hal_audio_bars(uint8_t *lv, int n)
{
    float t = hal_millis() / 1000.0f;
    for (int i = 0; i < n; i++) {
        float v = 0.5f + 0.5f * sinf(t * 6.0f + i * 0.7f) * sinf(t * 2.3f + i);
        lv[i] = (uint8_t)((v < 0 ? 0 : v > 1 ? 1 : v) * 255);
    }
    return n;
}
int hal_mic_level(void)
{
    float t = hal_millis() / 1000.0f;
    float music = fmodf(t, 20.0f) < 8.0f ? 70 + 30 * sinf(t * 5.0f) : 0;
    float clap = fmodf(t, 13.0f) < 0.25f ? 220 : 0;
    float v = 18 + music + clap;
    return v > 255 ? 255 : (int)v;
}
int hal_mic_pitch(void)
{
    float t = hal_millis() / 1000.0f;
    if (fmodf(t, 20.0f) >= 8.0f) return 0;
    return 150 + (int)(375 * (1 + sinf(t * 0.9f)));
}

/* ---- activity ---------------------------------------------------------------------- */
int hal_step_count(void) { return sim_steps + (int)(hal_millis() / 1500); }
void hal_activity_get(hal_activity *out)
{
    *out = (hal_activity){ (uint32_t)hal_step_count(), 41 * 60 + hal_millis() / 4000, 0, 1 };
}

/* ---- notifications (companion -> watch queue) -------------------------------------- */
#define NQ 16
static struct { char app[20]; char title[40]; } s_nq[NQ];
static int s_nq_head, s_nq_n;
int sim_notify_push(const char *app, const char *title)
{
    if (!app || !title || !*app || !*title || strlen(app) > 19 || strlen(title) > 39) return 0;
    if (s_nq_n >= NQ) return 0;
    int i = (s_nq_head + s_nq_n++) % NQ;
    snprintf(s_nq[i].app, sizeof s_nq[i].app, "%s", app);
    snprintf(s_nq[i].title, sizeof s_nq[i].title, "%s", title);
    return 1;
}
int hal_notification_post(const char *app, const char *title) { return sim_notify_push(app, title); }
int hal_notification_poll(char *app, int an, char *title, int tn)
{
    if (!s_nq_n) return 0;
    snprintf(app, (size_t)an, "%s", s_nq[s_nq_head].app);
    snprintf(title, (size_t)tn, "%s", s_nq[s_nq_head].title);
    s_nq_head = (s_nq_head + 1) % NQ;
    s_nq_n--;
    return 1;
}

/* ---- weather (core/weather.h, pulled in by hal.h) ----------------------------------- */
void hal_weather_get(hal_weather *out)
{
    memset(out, 0, sizeof *out);
    out->valid = 1; out->generation = 1; out->temp = 17; out->feels = 15; out->code = 2;
    out->utc_offset_seconds = sim_tz_min * 60;
    out->observed_epoch = (int64_t)hal_epoch_seconds();
    snprintf(out->city, sizeof out->city, "London");
    snprintf(out->status, sizeof out->status, "Simulator data");
    static const int hc[WEATHER_HOURS] = { 2, 3, 61, 61, 2 };
    for (int i = 0; i < WEATHER_HOURS; i++)
        out->hours[i] = (weather_hour){ out->observed_epoch + i * 3600, 17 + (i < 2 ? i : 3 - i), hc[i] };
    static const int dc[WEATHER_DAYS] = { 2, 61, 0, 3 };
    for (int i = 0; i < WEATHER_DAYS; i++)
        out->days[i] = (weather_day){ out->observed_epoch + i * 86400, 19 + i % 2, 11 + i % 3, dc[i] };
}
void hal_weather_refresh(void) {}
int hal_weather_set_location(const char *city, float lat, float lon) { (void)city; (void)lat; (void)lon; return 0; }
int hal_weather_location(char *city, int n, float *lat, float *lon) { return hal_location(city, n, lat, lon); }

/* ---- memory ----------------------------------------------------------------------- */
/* Zeroed so a screen that forgets to clear a sprite renders the same every run. */
void *hal_big_alloc(uint32_t bytes) { return calloc(1, bytes); }
