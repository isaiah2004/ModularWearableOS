/* set_diag.c — Diagnostics: does every board part and strap module actually work, not
 * just answer?
 *
 * A run is a guided pass of four cards (the parts only a person can check: the side
 * button, the touch panel, the motor and the speaker) while every sensor test runs in
 * the background, then the result list:
 *
 *   summary   [ TESTING ] and a count while it runs, then PASSED as a dot-matrix hero
 *             ("13/14"), one dot per part (white pass, ring fail, grey absent) and the
 *             fail count beside the one red dot on the screen
 *   rows      status dot, name, PASS / FAIL, and a sub-line: the reading, the reason it
 *             failed, or NO MODULE; a tap pushes the raw values and the threshold
 *   RE-RUN    a pill at the end
 *
 * Each sensor test is probe -> read -> plausibility, with real thresholds (DI[].check):
 * accel |g| 0.80-1.20, RTC advancing with uptime, rail 3.00-4.40 V, BME280 -20-60 C /
 * 0-100 % / 300-1100 hPa, lux >= 0 and not frozen, |B| 20-70 uT, HR IR live + skin,
 * new NMEA data from the GPS (a fix isn't needed), LTE answering AT, a mic floor that is
 * neither silent nor railed. Tests only poll the cached HAL getters (no bus calls in
 * tick) and their clocks advance only while the app is on screen, so a cover can't time
 * them out. Streams (GPS, HR, LTE) start for their test and stop when it resolves or the
 * app goes away (not when it is only covered). Every result goes to hal_log:
 *   DIAG <NAME> PASS|FAIL|ABSENT <reason>; <values>; <values>
 */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "svc_track.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PRESS_FLASH 0.10f
#define PRESS_HOLD  0.05f
#define PRESS_TRAVEL 8
#define ROW_GAP 24
#define FADE_S 0.15f              /* view cross-fade out; the next view then builds */
#define MORPH_S 0.28f

enum { D_BUTTON = 0, D_TOUCH, D_IMU, D_RTC, D_BATT, D_SD,
       D_HAPTIC, D_MIC, D_SPK, D_GPS, D_LTE, D_ENV, D_LIGHT, D_MAG, D_HR, D_N };
#define D_BOARD_N 6

typedef struct {
    const char *name, *title, *log;
    int icon;
    uint32_t ext;             /* EXT_* bit for strap modules, 0 = on the board */
    const char *check[2];     /* the threshold, as shown on the row page */
} dinfo;

static const dinfo DI[D_N] = {
    { "Button", "BUTTON", "BUTTON", DOT_ICON_ARROW_RIGHT, 0, { "A PRESS AND RELEASE", "SEEN BY THE OS" } },
    { "Touch", "TOUCH", "TOUCH", DOT_ICON_HAND, 0, { "2 TARGETS, EACH TAP", "WITHIN 44 PX" } },
    { "Motion", "MOTION", "IMU", DOT_ICON_LEVEL, 0, { "GRAVITY 0.80 TO 1.20 g", "FRESH, NOT FROZEN" } },
    { "Clock", "CLOCK", "RTC", DOT_ICON_TIMER, 0, { "ADVANCES WITH UPTIME", "WITHIN 1 s" } },
    { "Battery", "BATTERY", "BATTERY", DOT_ICON_BATTERY, 0, { "3.00 TO 4.40 V", "UP TO 5.20 V ON USB" } },
    { "SD card", "SD CARD", "SD", DOT_ICON_SD, 0, { "MOUNTED, SIZE OVER 0", "FREE NOT OVER SIZE" } },
    { "Haptics", "HAPTICS", "HAPTIC", DOT_ICON_BELL, EXT_HAPTIC, { "YOU FELT THE BUZZ", 0 } },
    { "Microphone", "MICROPHONE", "MIC", DOT_ICON_MIC, EXT_MIC, { "FLOOR 15 TO 105 dB", "LEVEL NOT FROZEN" } },
    { "Speaker", "SPEAKER", "SPEAKER", DOT_ICON_SPEAKER, EXT_SPEAKER, { "YOU HEARD THE CHIME", 0 } },
    { "GPS", "GPS", "GPS", DOT_ICON_SAT, EXT_GPS, { "NMEA SENTENCES IN 10 s", "A FIX IS NOT NEEDED" } },
    { "Cellular", "CELLULAR", "LTE", DOT_ICON_SIGNAL, EXT_LTE, { "ANSWERS AT IN 25 s", 0 } },
    { "Climate", "CLIMATE", "CLIMATE", DOT_ICON_THERMO, EXT_ENV,
      { "-20 TO 60\xB0" "C, 0-100%", "300 TO 1100 hPa" } },
    { "Light", "LIGHT", "LIGHT", DOT_ICON_SUN, EXT_LIGHT, { "0 LUX OR MORE", "READING NOT FROZEN" } },
    { "Compass", "COMPASS", "MAG", DOT_ICON_COMPASS, EXT_MAG, { "FIELD 20 TO 70 uT", "READING NOT FROZEN" } },
    { "Heart rate", "HEART RATE", "HR", DOT_ICON_HEART, EXT_HR, { "IR SIGNAL LIVE AND", "SKIN CONTACT IN 8 s" } },
};

/* ======================================================================= results */
enum { ST_WAIT = 0, ST_RUN, ST_ASK, ST_PASS, ST_FAIL, ST_ABSENT, ST_SKIP };   /* SKIP: a card left untested */
typedef struct {
    int st;
    float el;                 /* test clock, s: advances only while the app is on screen */
    float t_done;             /* nos_t when it resolved (rows cross-fade) */
    char why[28];             /* the row's sub-line: the reading, or why it failed */
    char rd[2][40];           /* raw values for the row page */
} dres;
static dres R[D_N];

/* per-test scratch */
static struct {
    hal_imu imu_first, imu_last; uint32_t imu_ms; int imu_n, imu_moved; float imu_gsum, imu_gyro;
    int rtc_init; hal_datetime rtc0; uint32_t rtc_ms0;
    int mic_n; float mic_sum, mic_min, mic_max; int mic_rail;
    int gps_init; uint32_t gps_n0; hal_gps gps;
    int lte_we_on; hal_lte lte;
    int env_ok; hal_env env;
    int lt_n, lt_pmin, lt_pmax; float lt_min, lt_max, lt_last;
    int mg_n, mg_moved; float mg_first_f, mg_first_h, mg_sum; hal_mag mg;
    int hr_live, hr_last; hal_hr hr;
    float touch_off; int touch_miss; float mdx[3], mdy[3];   /* the last 3 near misses */
} T;

static struct {
    int inited;
    int running, done;
    float t_done;
    uint32_t step_frame;
    int queue[4], nq, qi;     /* the cards of this run */
    int view, pview;          /* -1 the list, else the D_* card on screen */
    float sw_t, build_t;      /* view switch time; the view's entry build clock */
    int npass, nfail, ntested, nskip;
    char hero[12];
    char cnt_from[16], cnt_to[16]; float cnt_t;   /* "8 OF 13" tested parts resolved */
    int pend; float pend_t;   /* a list fading out before a RE-RUN or the fails-first re-layout */
    float title_off0, title_t0;   /* the title slides back from this scroll after a reset */
    int covered;              /* a row page is on top */
} G;
enum { PEND_NONE = 0, PEND_RERUN, PEND_RELAYOUT };
#define VIEW_LIST (-1)
#define VIEW_NONE (-2)

static int sod(const hal_datetime *d) { return d->hour * 3600 + d->minute * 60 + d->second; }
static int is_card(int d) { return d == D_BUTTON || d == D_TOUCH || d == D_HAPTIC || d == D_SPK; }
static int resolved(int st) { return st >= ST_PASS; }

static void log_res(int d)
{
    static const char *const W[] = { "WAIT", "RUN", "ASK", "PASS", "FAIL", "ABSENT", "SKIP" };
    char b[160], o[160];
    int n = snprintf(b, sizeof b, "DIAG %s %s %s", DI[d].log, W[R[d].st], R[d].why);
    for (int i = 0; i < 2 && n > 0 && n < (int)sizeof b; i++)
        if (R[d].rd[i][0]) n += snprintf(b + n, sizeof b - (size_t)n, "; %s", R[d].rd[i]);
    /* the serial log is ASCII: the degree mark becomes "deg" */
    int j = 0;
    for (int i = 0; b[i] && j < (int)sizeof o - 4; i++) {
        if ((unsigned char)b[i] == 0xB0) { o[j++] = 'd'; o[j++] = 'e'; o[j++] = 'g'; }
        else o[j++] = b[i];
    }
    o[j] = 0;
    hal_log(o);
}

static void power_down(int d)
{
    if (d == D_GPS && !svc_track_active()) hal_gps_enable(0);   /* a recording track keeps it */
    if (d == D_HR) hal_hr_enable(0);
    if (d == D_LTE && T.lte_we_on) { hal_lte_enable(0); T.lte_we_on = 0; }
}

static void res(int d, int st)
{
    R[d].st = st;
    R[d].t_done = nos_t;
    log_res(d);
    power_down(d);
}
#define WHY(d, ...) snprintf(R[d].why, sizeof R[d].why, __VA_ARGS__)
#define RD(d, i, ...) snprintf(R[d].rd[i], sizeof R[d].rd[i], __VA_ARGS__)

static void test_start(int d)
{
    R[d].st = ST_RUN;
    R[d].el = 0;
    if (d == D_GPS) hal_gps_enable(1);
    if (d == D_HR) hal_hr_enable(1);
    if (d == D_LTE) {
        hal_lte_get(&T.lte);
        if (!T.lte.powered) { hal_lte_enable(1); T.lte_we_on = 1; }
    }
}

/* ---- the sensor tests: called every frame while running; resolve with res() ---- */
static void test_imu(dres *r)
{
    hal_imu im; uint32_t ms;
    if (hal_imu_sample(&im, &ms) && (T.imu_n == 0 || ms != T.imu_ms)) {
        T.imu_ms = ms;
        if (T.imu_n == 0) T.imu_first = im;
        else if (memcmp(&im, &T.imu_first, sizeof im)) T.imu_moved = 1;
        T.imu_n++;
        T.imu_gsum += sqrtf(im.ax * im.ax + im.ay * im.ay + im.az * im.az);
        float w = fmaxf(fabsf(im.gx), fmaxf(fabsf(im.gy), fabsf(im.gz)));
        if (w > T.imu_gyro) T.imu_gyro = w;
        T.imu_last = im;
    }
    if (r->el < 1.0f) return;
    const hal_imu *l = &T.imu_last;
    float g = T.imu_n ? T.imu_gsum / T.imu_n : 0;
    RD(D_IMU, 0, "X %+.2f  Y %+.2f", l->ax, l->ay);
    RD(D_IMU, 1, "Z %+.2f g  GYRO %.0f\xB0/s", l->az, T.imu_gyro);
    if (T.imu_n < 3) { WHY(D_IMU, "NO FRESH SAMPLES"); res(D_IMU, ST_FAIL); }
    else if (g < 0.8f) { WHY(D_IMU, "%.2f g, TOO LOW", g); res(D_IMU, ST_FAIL); }
    else if (g > 1.2f) { WHY(D_IMU, "%.2f g, TOO HIGH", g); res(D_IMU, ST_FAIL); }
    else if (!T.imu_moved && T.imu_n >= 8) { WHY(D_IMU, "READING FROZEN"); res(D_IMU, ST_FAIL); }
    else if (T.imu_gyro > 500.0f) { WHY(D_IMU, "GYRO %.0f \xB0/s", T.imu_gyro); res(D_IMU, ST_FAIL); }
    else { WHY(D_IMU, "%.2f g", g); res(D_IMU, ST_PASS); }
}

static void test_rtc(dres *r)
{
    if (!T.rtc_init) { hal_get_datetime(&T.rtc0); T.rtc_ms0 = hal_millis(); T.rtc_init = 1; }
    if (r->el < 2.2f) return;
    hal_datetime d1;
    hal_get_datetime(&d1);
    float up = (float)(hal_millis() - T.rtc_ms0) / 1000.0f;
    int ds = sod(&d1) - sod(&T.rtc0);
    if (ds < -43200) ds += 86400;
    const hal_datetime *a = &T.rtc0;
    RD(D_RTC, 0, "%02d:%02d:%02d TO %02d:%02d:%02d", a->hour, a->minute, a->second, d1.hour, d1.minute, d1.second);
    RD(D_RTC, 1, "IN %.1f s OF UPTIME", up);
    if (ds == 0) { WHY(D_RTC, "STOPPED"); res(D_RTC, ST_FAIL); }
    else if (fabsf(ds - up) > 1.2f) { WHY(D_RTC, "OFF BY %+.0f s", ds - up); res(D_RTC, ST_FAIL); }
    else if (d1.year < 2020) { WHY(D_RTC, "DATE NOT SET"); res(D_RTC, ST_FAIL); }
    else { WHY(D_RTC, "ON TIME"); res(D_RTC, ST_PASS); }
}

static void test_batt(dres *r)
{
    if (r->el < 0.5f) return;
    hal_power p;
    hal_power_get(&p);
    int mv = p.rail_mv > 0 ? p.rail_mv : hal_battery_mv();
    int pct = hal_battery_percent();
    float v = mv / 1000.0f, hi = p.external_power == 1 ? 5.2f : 4.4f;
    RD(D_BATT, 0, "%.2f V  %d%%", v, pct);
    RD(D_BATT, 1, "%s", p.external_power == 1 ? "USB POWER SEEN" : p.external_power == 0 ? "ON THE CELLS" : "SUPPLY UNKNOWN");
    if (mv <= 0) { WHY(D_BATT, "NO RAIL READING"); res(D_BATT, ST_FAIL); }
    else if (v < 3.0f) { WHY(D_BATT, "%.2f V, TOO LOW", v); res(D_BATT, ST_FAIL); }
    else if (v > hi) { WHY(D_BATT, "%.2f V, TOO HIGH", v); res(D_BATT, ST_FAIL); }
    else if (pct < 0 || pct > 100) { WHY(D_BATT, "%d%%, OUT OF RANGE", pct); res(D_BATT, ST_FAIL); }
    else { WHY(D_BATT, "%.2f V%s", v, p.external_power == 1 ? ", USB" : ""); res(D_BATT, ST_PASS); }
}

static void upcase(char *o, int n, const char *s)
{
    int i = 0;
    for (; s[i] && i < n - 1; i++) o[i] = (char)toupper((unsigned char)s[i]);
    o[i] = 0;
}
static void test_sd(dres *r)
{
    (void)r;
    hal_storage s;
    hal_storage_get(&s);
    char m[40];
    upcase(m, 30, s.message);
    double gb = 1024.0 * 1024.0 * 1024.0;
    if (!s.mounted) { WHY(D_SD, "NO CARD"); RD(D_SD, 0, "%.22s", m); res(D_SD, ST_ABSENT); return; }
    RD(D_SD, 0, "%.1f OF %.1f GB FREE", s.free_bytes / gb, s.total_bytes / gb);
    RD(D_SD, 1, "%.22s", m);
    if (!s.total_bytes) { WHY(D_SD, "SIZE 0"); res(D_SD, ST_FAIL); }
    else if (s.free_bytes > s.total_bytes) { WHY(D_SD, "FREE OVER SIZE"); res(D_SD, ST_FAIL); }
    else if (strstr(m, "FAIL") || strstr(m, "CANNOT")) { snprintf(R[D_SD].why, sizeof R[D_SD].why, "%.20s", m); res(D_SD, ST_FAIL); }
    else { WHY(D_SD, "%.1f GB FREE", s.free_bytes / gb); res(D_SD, ST_PASS); }
}

static void test_mic(dres *r)
{
    float db = hal_mic_db();               /* polling keeps the mic running */
    if (r->el > 0.5f && db >= 0) {
        if (!T.mic_n || db < T.mic_min) T.mic_min = db;
        if (!T.mic_n || db > T.mic_max) T.mic_max = db;
        T.mic_n++; T.mic_sum += db;
        uint8_t w[32];
        if (hal_mic_wave(w) == 32) {
            int hi = 0;
            for (int i = 0; i < 32; i++) hi += w[i] == 255;
            if (hi >= 30) T.mic_rail++;
        }
    }
    if (r->el < 2.5f) return;
    if (T.mic_n < 10) {
        if (r->el < 4.0f) return;
        RD(D_MIC, 0, "%d SAMPLES IN 4 s", T.mic_n);
        WHY(D_MIC, db < 0 ? "NO REPLY" : "NO SAMPLES"); res(D_MIC, ST_FAIL); return;
    }
    float mean = T.mic_sum / T.mic_n;
    RD(D_MIC, 0, "FLOOR %.0f  PEAK %.0f dB", T.mic_min, T.mic_max);
    RD(D_MIC, 1, "MEAN %.0f dB  %d READS", mean, T.mic_n);
    if (mean < 15.0f) { WHY(D_MIC, "%.0f dB, SILENT", mean); res(D_MIC, ST_FAIL); }
    else if (mean > 105.0f || T.mic_rail > T.mic_n / 2) { WHY(D_MIC, "%.0f dB, RAILED", mean); res(D_MIC, ST_FAIL); }
    else if (T.mic_max - T.mic_min < 0.01f) { WHY(D_MIC, "LEVEL FROZEN"); res(D_MIC, ST_FAIL); }
    else { WHY(D_MIC, "%.0f dB FLOOR", T.mic_min); res(D_MIC, ST_PASS); }
}

/* NMEA arriving = the driver's good-checksum sentence counter keeps climbing (a fix
 * isn't needed); 2 s gives the UTC time to come through for the reading */
static void test_gps(dres *r)
{
    hal_gps *g = &T.gps;
    if (!hal_gps_get(g)) {
        if (r->el > 1.0f) { WHY(D_GPS, "NO REPLY"); res(D_GPS, ST_FAIL); }
        return;
    }
    if (!T.gps_init) { T.gps_init = 1; T.gps_n0 = g->sentences; return; }
    uint32_t n = g->sentences - T.gps_n0;
    if ((!n || r->el < 2.0f) && r->el < 10.0f) return;
    RD(D_GPS, 0, "%u NMEA SENTENCES", (unsigned)n);
    if (g->utc_valid) RD(D_GPS, 1, "UTC %02d:%02d:%02d  %d SATS", g->utc_h, g->utc_m, g->utc_s, g->sats_view);
    else RD(D_GPS, 1, "NO TIME YET  %d SATS", g->sats_view);
    if (!n) { WHY(D_GPS, "NO NMEA IN 10 s"); res(D_GPS, ST_FAIL); return; }
    WHY(D_GPS, "%u NMEA IN %.0f s", (unsigned)n, r->el);
    res(D_GPS, ST_PASS);
}

static void test_lte(dres *r)
{
    hal_lte *l = &T.lte;
    if (!hal_lte_get(l)) { WHY(D_LTE, "NO REPLY"); res(D_LTE, ST_FAIL); return; }
    if (!l->powered && r->el < 25.0f) return;
    if (!l->powered) {                        /* the modem never answered: nothing else is known */
        RD(D_LTE, 0, "NO AT REPLY");
        RD(D_LTE, 1, UI_NONE);
        WHY(D_LTE, "NO AT REPLY IN 25 s"); res(D_LTE, ST_FAIL); return;
    }
    RD(D_LTE, 0, "%s", !l->sim ? "NO SIM" : l->sim == 2 ? "SIM LOCKED" : "SIM READY");
    if (l->registered == 1 || l->registered == 2) RD(D_LTE, 1, "%s  %d dBm", l->registered == 2 ? "ROAMING" : "HOME", l->rssi_dbm);
    else RD(D_LTE, 1, "%s", l->registered == 3 ? "SEARCHING" : "NOT REGISTERED");
    WHY(D_LTE, "ANSWERS AT");
    res(D_LTE, ST_PASS);
}

static void test_env(dres *r)
{
    hal_env *e = &T.env;
    if (hal_env_get(e) && e->valid) T.env_ok = 1;
    if (!T.env_ok) {
        if (r->el < 3.0f) return;
        WHY(D_ENV, "NO SAMPLE IN 3 s"); res(D_ENV, ST_FAIL); return;
    }
    if (r->el < 1.0f) return;
    RD(D_ENV, 0, "%.1f\xB0" "C  %.0f%% RH", e->temp_c, e->humidity);
    RD(D_ENV, 1, "%.1f hPa", e->pressure_hpa);
    if (!isfinite(e->temp_c) || e->temp_c < -20 || e->temp_c > 60) { WHY(D_ENV, "%.0f\xB0" "C, OUT OF RANGE", e->temp_c); res(D_ENV, ST_FAIL); }
    else if (!isfinite(e->humidity) || e->humidity < 0 || e->humidity > 100) { WHY(D_ENV, "%.0f%% RH, OUT", e->humidity); res(D_ENV, ST_FAIL); }
    else if (!isfinite(e->pressure_hpa) || e->pressure_hpa < 300 || e->pressure_hpa > 1100) { WHY(D_ENV, "%.0f hPa, OUT", e->pressure_hpa); res(D_ENV, ST_FAIL); }
    else { WHY(D_ENV, "%.1f\xB0" "C  %.0f hPa", e->temp_c, e->pressure_hpa); res(D_ENV, ST_PASS); }
}

static void test_light(dres *r)
{
    hal_light l;
    if (hal_light_get(&l) && l.valid) {
        if (!T.lt_n || l.lux < T.lt_min) T.lt_min = l.lux;
        if (!T.lt_n || l.lux > T.lt_max) T.lt_max = l.lux;
        if (!T.lt_n || l.prox < T.lt_pmin) T.lt_pmin = l.prox;
        if (!T.lt_n || l.prox > T.lt_pmax) T.lt_pmax = l.prox;
        T.lt_last = l.lux;
        T.lt_n++;
    }
    if (r->el < 4.0f) return;
    RD(D_LIGHT, 0, "%.0f TO %.0f LUX", T.lt_min, T.lt_max);
    RD(D_LIGHT, 1, "PROX %d-%d  %d READS", T.lt_pmin, T.lt_pmax, T.lt_n);
    if (!T.lt_n) { WHY(D_LIGHT, "NO SAMPLE IN 4 s"); res(D_LIGHT, ST_FAIL); }
    else if (!isfinite(T.lt_min) || T.lt_min < 0) { WHY(D_LIGHT, "%.0f LUX, INVALID", T.lt_min); res(D_LIGHT, ST_FAIL); }
    else if (T.lt_n >= 10 && T.lt_max == T.lt_min && T.lt_pmax == T.lt_pmin) { WHY(D_LIGHT, "READING FROZEN"); res(D_LIGHT, ST_FAIL); }
    else { WHY(D_LIGHT, "%.0f LUX", T.lt_last); res(D_LIGHT, ST_PASS); }
}

static void test_mag(dres *r)
{
    hal_mag *m = &T.mg;
    if (hal_mag_get(m) && m->valid) {
        if (!T.mg_n) { T.mg_first_f = m->field_ut; T.mg_first_h = m->heading_deg; }
        else if (m->field_ut != T.mg_first_f || m->heading_deg != T.mg_first_h) T.mg_moved = 1;
        T.mg_sum += m->field_ut;
        T.mg_n++;
    }
    if (r->el < 3.0f) return;
    float f = T.mg_n ? T.mg_sum / T.mg_n : 0;
    RD(D_MAG, 0, "FIELD %.1f uT  HDG %03.0f", f, T.mg_n ? fmodf(m->heading_deg + 360.0f, 360.0f) : 0.0f);
    RD(D_MAG, 1, "%d READS  %s", T.mg_n, m->calibrated ? "CALIBRATED" : "UNCALIBRATED");
    if (!T.mg_n) { WHY(D_MAG, "NO SAMPLE IN 3 s"); res(D_MAG, ST_FAIL); }
    else if (f < 20.0f) { WHY(D_MAG, "%.0f uT, TOO WEAK", f); res(D_MAG, ST_FAIL); }
    else if (f > 70.0f) { WHY(D_MAG, "%.0f uT, TOO STRONG", f); res(D_MAG, ST_FAIL); }
    else if (T.mg_n >= 10 && !T.mg_moved) { WHY(D_MAG, "READING FROZEN"); res(D_MAG, ST_FAIL); }
    else { WHY(D_MAG, "%.0f uT", f); res(D_MAG, ST_PASS); }
}

static void test_hr(dres *r)
{
    hal_hr *h = &T.hr;
    if (!hal_hr_get(h)) { WHY(D_HR, "NO REPLY"); res(D_HR, ST_FAIL); return; }
    int last = h->wave[47];
    if (r->el > 0.3f && last != T.hr_last) T.hr_live++;   /* the IR trace keeps moving */
    T.hr_last = last;
    int pass = h->on_wrist && T.hr_live >= 5 && r->el >= 2.0f;
    if (!pass && r->el < 8.0f) return;
    RD(D_HR, 0, "IR %s  QUALITY %.2f", T.hr_live >= 5 ? "LIVE" : "FLAT", h->quality);
    if (h->bpm) RD(D_HR, 1, "%d BPM", h->bpm);
    else RD(D_HR, 1, "%s", h->on_wrist ? "NO LOCK YET" : "OFF THE SKIN");
    if (pass) { WHY(D_HR, "SKIN CONTACT"); res(D_HR, ST_PASS); }
    else if (T.hr_live < 5) { WHY(D_HR, "NO IR SIGNAL"); res(D_HR, ST_FAIL); }
    else { WHY(D_HR, "NO SKIN CONTACT"); res(D_HR, ST_FAIL); }
}

/* ======================================================================= the run */
static void card_reset(void);
static void card_keep(void);
static void list_init(void);
static float dlist_off(void);
static void go_view(int v)
{
    card_keep();                     /* the outgoing card fades as it was, not reset */
    G.pview = G.view;
    G.view = v;
    G.sw_t = nos_t;
    G.build_t = nos_t + FADE_S;
    card_reset();
}

static void count_text(char *o, int n)
{
    int tot = 0, res_n = 0;
    for (int d = 0; d < D_N; d++) {
        if (R[d].st == ST_ABSENT) continue;
        tot++;
        res_n += resolved(R[d].st);
    }
    snprintf(o, (size_t)n, "%d OF %d", res_n, tot);   /* not the a/b result format */
}
static void count_set(int init)
{
    char b[16];
    count_text(b, sizeof b);
    if (init) { snprintf(G.cnt_to, sizeof G.cnt_to, "%s", b); memcpy(G.cnt_from, G.cnt_to, sizeof G.cnt_from); G.cnt_t = -100; return; }
    if (!strcmp(b, G.cnt_to)) return;
    memcpy(G.cnt_from, G.cnt_to, sizeof G.cnt_from);
    snprintf(G.cnt_to, sizeof G.cnt_to, "%s", b);
    G.cnt_t = nos_t;
}

/* mode 1: the screen's entry (build at nos_enter_t); 2: after a RE-RUN fade-out */
static void run_start(int mode)
{
    memset(R, 0, sizeof R);
    memset(&T, 0, sizeof T);
    uint32_t ext = hal_ext_present();
    G.nq = 0; G.qi = 0;
    hal_log("DIAG START");
    for (int d = 0; d < D_N; d++) {
        if (DI[d].ext && !(ext & DI[d].ext)) {
            WHY(d, "NO MODULE");                 /* the OS's row word for an absent module */
            R[d].st = ST_ABSENT; R[d].t_done = -100;
            log_res(d);
        } else if (is_card(d)) { R[d].st = ST_WAIT; G.queue[G.nq++] = d; }
        else test_start(d);
    }
    G.running = 1; G.done = 0; G.nfail = 0; G.pend = 0;
    R[G.queue[0]].st = ST_ASK;
    list_init();
    count_set(1);
    G.pview = VIEW_NONE; G.view = G.queue[0]; G.sw_t = nos_t;
    G.build_t = mode == 1 ? nos_enter_t() : nos_t;
    G.title_t0 = nos_t;
    if (mode == 1) G.title_off0 = 0;
    card_reset();
}

static void run_finish(void)
{
    G.running = 0; G.done = 1; G.t_done = nos_t;
    G.npass = G.nfail = G.ntested = G.nskip = 0;
    for (int d = 0; d < D_N; d++) {
        G.npass += R[d].st == ST_PASS;
        G.nfail += R[d].st == ST_FAIL;
        G.ntested += R[d].st == ST_PASS || R[d].st == ST_FAIL;
        G.nskip += R[d].st == ST_SKIP;
    }
    snprintf(G.hero, sizeof G.hero, "%d/%d", G.npass, G.ntested);
    char b[64];
    snprintf(b, sizeof b, "DIAG DONE %d/%d PASS, %d FAIL, %d SKIP, %d ABSENT", G.npass, G.ntested, G.nfail, G.nskip,
             D_N - G.ntested - G.nskip);
    hal_log(b);
    nos_feedback(G.nfail ? FB_ERROR : FB_SUCCESS);
    if (!G.nfail) return;
    /* fails go to the top. A list on screen fades out first, then rebuilds in that order */
    if (G.view == VIEW_LIST && !G.covered && nos_t >= G.build_t) { G.pend = PEND_RELAYOUT; G.pend_t = nos_t; }
    else list_init();
}

static void card_restore(void);
static void run_abort(void)
{
    G.pend = 0;
    if (!G.running) return;
    for (int d = 0; d < D_N; d++) if (R[d].st == ST_RUN) power_down(d);
    card_restore();
    hal_haptic_stop();
    G.running = 0;
    hal_log("DIAG ABORTED");
}

/* advance every running test; once per frame from whichever diag page is visible */
static void diag_step(float dt)
{
    if (!G.running || G.step_frame == nos_frame) return;
    G.step_frame = nos_frame;
    int open = 0, run = 0;
    for (int d = 0; d < D_N; d++) {
        dres *r = &R[d];
        if (r->st == ST_RUN) {
            run = 1;
            r->el += dt;
            switch (d) {
            case D_IMU: test_imu(r); break;
            case D_RTC: test_rtc(r); break;
            case D_BATT: test_batt(r); break;
            case D_SD: test_sd(r); break;
            case D_MIC: test_mic(r); break;
            case D_GPS: test_gps(r); break;
            case D_LTE: test_lte(r); break;
            case D_ENV: test_env(r); break;
            case D_LIGHT: test_light(r); break;
            case D_MAG: test_mag(r); break;
            case D_HR: test_hr(r); break;
            default: break;
            }
        }
        open += !resolved(r->st);
    }
    if (run) nos_keep_awake();        /* only a background test holds the screen; a card waits like any page */
    count_set(0);
    if (!open) run_finish();
}

/* ======================================================================= helpers */
static float q8(float x) { return floorf(m_clamp(x, 0, 1) * 8.0f + 0.5f) / 8.0f; }
static float qfade(float yc)
{
    float d = fabsf(yc - UI_CY);
    float f = 1.0f - 0.7f * m_smoothstep(140.0f, 230.0f, d);
    return floorf(f * 16.0f + 0.5f) / 16.0f;
}
static int cap_base(const font_t *f, float cy) { return (int)lroundf(cy + f->ascent * 0.36f); }
static void row_cols(float cy, int *lx, int *rx)
{
    int x0, x1;
    ui_row_span(cy, 6, &x0, &x1);
    *lx = x0 > UI_ROW_X0 ? x0 : UI_ROW_X0;
    *rx = x1 < UI_ROW_X1 ? x1 : UI_ROW_X1;
}
static void chevron(gfx_fb *fb, float rx, float cy, uint16_t col)
{
    dot_style st = { DOT_FONT_5X7, 2.0f, 1.7f, col, 0, 1, 0 };
    dot_text(fb, &st, rx - 8.0f, cy - 7.0f, ">");
}
/* caps label whose strings are already cased (units keep theirs: g, uT, hPa, dB) */
static int dlab(gfx_fb *fb, int x, int y, const char *s, uint16_t col, int align)
{
    return ui_text(fb, UI_F_LABEL, x, y, s, col, align, 2);
}
static int dlab_w(const char *s) { return ui_text_width(UI_F_LABEL, s, 2); }
static float hero_pitch(const char *s)
{
    int n = 0;
    for (const char *c = s; *c; c++) n += *c != ':' && *c != '.';
    return n <= 4 ? UI_HERO_PITCH : UI_HEROW_PITCH;
}
/* 2 Hz blink of a test in progress: discrete, two levels */
static uint16_t run_col(void) { return fmodf(nos_t, 1.0f) < 0.5f ? C_SEC : C_DIS; }
/* the status dot of a part: white pass, ring fail, grey absent, blinking in progress */
static void status_dot(gfx_fb *fb, float x, float y, float d, int st, float k)
{
    if (st == ST_PASS) dot_disc(fb, x, y, d, ui_dim(C_WHITE, k));
    else if (st == ST_FAIL) ui_circle_line(fb, x, y, d * 0.5f - 0.5f, 2.0f, ui_dim(C_PRI, k));
    else if (st == ST_ABSENT) dot_disc(fb, x, y, d, ui_dim(C_LINE2, k));
    else if (st == ST_SKIP) ui_circle_line(fb, x, y, d * 0.5f - 0.5f, 2.0f, ui_dim(C_DIS, k));
    else if (st == ST_WAIT) dot_disc(fb, x, y, d * 0.6f, ui_dim(C_DIS, k));
    else dot_disc(fb, x, y, d * 0.6f, ui_dim(run_col(), k));
}

/* a standard button that can also be drawn faded (views cross-fade, rows near the bezel) */
static void pill(gfx_fb *fb, float cx, float cy, float w, const char *label, int kind, int pressed, float k)
{
    if (k >= 1) { ui_button(fb, cx, cy, w, UI_BTN_H, label, kind, pressed); return; }
    uint16_t fill = kind == BTN_PRIMARY ? (pressed ? C_SEC : C_WHITE) : (pressed ? C_SEC : C_SURF2);
    uint16_t ink = kind == BTN_PRIMARY || pressed ? C_BG : C_PRI;
    ui_rrect(fb, cx - w * 0.5f, cy - UI_BTN_H * 0.5f, w, UI_BTN_H, UI_BTN_H * 0.5f, ui_dim(fill, k));
    ui_label(fb, (int)cx, (int)(cy + UI_F_LABEL->ascent * 0.5f - 2), label, ui_dim(ink, k), UI_CENTER);
}

/* ======================================================================= cards */
#define TGT_R 44                  /* a hit */
#define TGT_NEAR 120              /* a miss that counts; farther is a stray touch */
#define CARD_WAIT_S 30.0f         /* an unanswered card resolves as NO ANSWER */
static const int TGT[2][2] = { { 150, 150 }, { 316, 380 } };
typedef struct {
    int tgt;                      /* touch: target in play */
    float hit_t, miss_t;
    int miss_x, miss_y, missed;
    float play_t;                 /* haptic / speaker: last play */
    int played;
    int pressed;                  /* 1 left / single, 2 right, 3 the icon */
    float flash_t; int flash_b;
    int held;                     /* button card: the side button is down (or was, at the press) */
    float t_in;                   /* nos_t the card came up */
} cstate;
static cstate C, Cp;              /* the card on screen; the one fading out */

static void card_reset(void)
{
    memset(&C, 0, sizeof C);
    C.flash_t = -100; C.hit_t = -100; C.play_t = -100; C.miss_t = -100;
    C.t_in = nos_t;
}
static void card_keep(void) { Cp = C; }
static void card_restore(void)
{
    hal_haptic_set_strength(nos_prefs.haptic_strength);
    hal_audio_volume(nos_prefs.volume);
}
static void card_play(int d)
{
    if (d == D_HAPTIC) {
        hal_haptic_set_strength(nos_prefs.haptic_strength > 80 ? nos_prefs.haptic_strength : 80);
        hal_haptic(HAPTIC_BUZZ);
    } else {
        hal_audio_volume(nos_prefs.volume > 60 ? nos_prefs.volume : 60);
        hal_sfx(SFX_NOTIFY);
    }
    C.play_t = nos_t;
    C.played = 1;
}
/* a card resolved: a tick, then the next card or the list */
static void card_done(int d, int st)
{
    res(d, st);
    card_restore();
    nos_feedback(FB_TICK);
    G.qi++;
    if (G.qi < G.nq) { R[G.queue[G.qi]].st = ST_ASK; go_view(G.queue[G.qi]); }
    else go_view(VIEW_LIST);
}
static void card_tick(void)
{
    int d = G.view;
    C.held = d == D_BUTTON && hal_button_down();
    if ((d == D_HAPTIC || d == D_SPK) && !C.played && nos_t >= G.build_t + 0.1f) card_play(d);
    if (nos_t - C.t_in > CARD_WAIT_S) {
        WHY(d, "NO ANSWER");
        RD(d, 0, "NO ANSWER IN %.0f s", CARD_WAIT_S);
        card_done(d, ST_SKIP);
    }
}

static int pair_hit(int x, int y)
{
    if (fabsf(y - (float)UI_BTN_Y) > UI_BTN_H * 0.5f + 4) return 0;
    if (fabsf(x - (float)UI_BTN_PAIR_X0) <= UI_BTN_PAIR_W * 0.5f + 6) return 1;
    if (fabsf(x - (float)UI_BTN_PAIR_X1) <= UI_BTN_PAIR_W * 0.5f + 6) return 2;
    return 0;
}
static int single_hit(int x, int y)
{
    return fabsf(y - (float)UI_BTN_Y) <= UI_BTN_H * 0.5f + 4 && fabsf(x - (float)UI_CX) <= UI_BTN_W * 0.5f + 6;
}
static int card_btn(int d, int x, int y)
{
    if (d == D_BUTTON) return single_hit(x, y) ? 1 : 0;
    if (d == D_HAPTIC || d == D_SPK) {
        int b = pair_hit(x, y);
        if (b) return b;
        float dx = x - UI_CX, dy = y - 170.0f;
        if (dx * dx + dy * dy < 60.0f * 60.0f) return 3;
    }
    return 0;
}

static const char *dir_word(float dx, float dy)
{
    int h = dx < -12 ? -1 : dx > 12 ? 1 : 0, v = dy < -12 ? -1 : dy > 12 ? 1 : 0;
    static const char *const W[3][3] = { { "UP LEFT", "UP", "UP RIGHT" }, { "LEFT", "", "RIGHT" },
                                         { "DOWN LEFT", "DOWN", "DOWN RIGHT" } };
    return W[v + 1][h + 1];
}
/* The touch card judges the panel, not the person: the back-swipe edge and stray touches
 * far from the dot don't count; it fails only on 3 near misses with one consistent offset */
static void touch_down(int x, int y)
{
    if (x < 40) return;                                   /* the back-swipe edge */
    float dx = (float)(x - TGT[C.tgt][0]), dy = (float)(y - TGT[C.tgt][1]);
    float off = sqrtf(dx * dx + dy * dy);
    if (off > TGT_NEAR) return;                           /* a stray touch */
    if (off <= TGT_R) {
        if (off > T.touch_off) T.touch_off = off;
        C.tgt++; C.hit_t = nos_t; C.missed = 0;
        if (C.tgt >= 2) {
            RD(D_TOUCH, 0, "LAST TAP %d, %d", x, y);
            RD(D_TOUCH, 1, "%d NEAR MISS%s", T.touch_miss, T.touch_miss == 1 ? "" : "ES");
            WHY(D_TOUCH, "%.0f PX OFF", T.touch_off);
            card_done(D_TOUCH, ST_PASS);
        } else nos_feedback(FB_TICK);
        return;
    }
    C.miss_x = x; C.miss_y = y; C.missed = 1; C.miss_t = nos_t;
    int i = T.touch_miss % 3;
    T.mdx[i] = dx; T.mdy[i] = dy;
    T.touch_miss++;
    if (T.touch_miss < 3) return;
    float mx = (T.mdx[0] + T.mdx[1] + T.mdx[2]) / 3.0f, my = (T.mdy[0] + T.mdy[1] + T.mdy[2]) / 3.0f;
    float spread = 0;
    for (int j = 0; j < 3; j++) {
        float ex = T.mdx[j] - mx, ey = T.mdy[j] - my, e = sqrtf(ex * ex + ey * ey);
        if (e > spread) spread = e;
    }
    if (spread > 30.0f) return;                           /* scattered: a person, not an offset */
    RD(D_TOUCH, 0, "TAP %d,%d AT %d,%d", x, y, TGT[C.tgt][0], TGT[C.tgt][1]);
    RD(D_TOUCH, 1, "OFFSET %+.0f, %+.0f PX", mx, my);
    WHY(D_TOUCH, "OFF BY %.0f PX %s", sqrtf(mx * mx + my * my), dir_word(mx, my));
    card_done(D_TOUCH, ST_FAIL);
}

static int card_input(const nos_input *in)
{
    int d = G.view;
    if (in->type == IN_BTN || in->type == IN_BTN_LONG || in->type == IN_BTN_DOUBLE) {
        if (d == D_BUTTON) {
            if (in->type != IN_BTN_DOUBLE) {
                WHY(D_BUTTON, "PRESS SEEN");
                RD(D_BUTTON, 0, "%s", in->type == IN_BTN_LONG ? "LONG PRESS" : "PRESS AND RELEASE");
                C.held = 1;                      /* the pressed disc fades out with the card */
                card_done(D_BUTTON, ST_PASS);
            }
            return 1;
        }
        if (d == D_TOUCH) {          /* the button skips a touch panel that can't be used */
            if (in->type == IN_BTN && nos_t - C.t_in > 0.5f) {
                RD(D_TOUCH, 0, "%d NEAR MISS%s", T.touch_miss, T.touch_miss == 1 ? "" : "ES");
                WHY(D_TOUCH, "NOT TESTED");
                card_done(D_TOUCH, ST_SKIP);
            }
            return 1;
        }
        return 0;
    }
    if (nos_t < G.build_t) return 1;          /* the card is still coming in */
    if (d == D_TOUCH) {
        if (in->type == IN_DOWN) touch_down(in->x, in->y);
        return in->type != IN_DRAG_START;      /* a swipe from the edge still goes back */
    }
    switch (in->type) {
    case IN_DOWN: C.pressed = card_btn(d, in->x, in->y); return 1;
    case IN_UP: case IN_DRAG_START: case IN_LONG: C.pressed = 0; return in->type != IN_DRAG_START;
    case IN_TAP: {
        int b = card_btn(d, in->x0, in->y0);
        C.pressed = 0;
        if (!b) return 1;
        C.flash_b = b; C.flash_t = nos_t;
        if (b == 3) { nos_feedback(FB_TAP); card_play(d); return 1; }
        if (d == D_BUTTON) {
            RD(D_BUTTON, 0, "NO PRESS, SKIPPED");
            WHY(D_BUTTON, "NOT TESTED");
            card_done(D_BUTTON, ST_SKIP);
        } else {
            int yes = b == 2;
            RD(d, 0, "%s", d == D_HAPTIC ? "HAPTIC_BUZZ  300 ms" : "SFX_NOTIFY");
            RD(d, 1, "%s %d%%", d == D_HAPTIC ? "STRENGTH" : "VOLUME",
               d == D_HAPTIC ? (nos_prefs.haptic_strength > 80 ? nos_prefs.haptic_strength : 80)
                             : (nos_prefs.volume > 60 ? nos_prefs.volume : 60));
            if (d == D_HAPTIC) WHY(d, yes ? "FELT IT" : "NOT FELT");
            else WHY(d, yes ? "HEARD IT" : "NOT HEARD");
            card_done(d, yes ? ST_PASS : ST_FAIL);
        }
        return 1;
    }
    default: return 1;
    }
}

static int card_index(int d)
{
    int qi = 0;
    while (qi < G.nq - 1 && G.queue[qi] != d) qi++;
    return qi;
}
/* "1 OF 4": stays put across cards; only the digit swaps (out, then in) */
static void draw_step(gfx_fb *fb, float k)
{
    float p = nos_t - G.sw_t;
    int fading = G.pview >= 0 && G.view >= 0 && p < FADE_S;
    int cur = card_index(G.view) + 1, old = fading ? card_index(G.pview) + 1 : cur;
    char tail[12], a[4], b[4];
    snprintf(tail, sizeof tail, " OF %d", G.nq);
    snprintf(a, sizeof a, "%d", old);
    snprintf(b, sizeof b, "%d", cur);
    int wd = ui_label_width(b), wt = ui_label_width(tail);
    int x0 = UI_CX - (wd + wt) / 2;
    ui_label(fb, x0 + wd, 104, tail, ui_dim(C_DIS, k), UI_LEFT);
    if (fading) {
        float h = p / FADE_S;
        if (h < 0.5f) ui_label(fb, x0, 104, a, ui_dim(C_DIS, k * q8(1.0f - h * 2.0f)), UI_LEFT);
        else ui_label(fb, x0, 104, b, ui_dim(C_DIS, k * q8(h * 2.0f - 1.0f)), UI_LEFT);
    } else ui_label(fb, x0, 104, b, ui_dim(C_DIS, k), UI_LEFT);
}

/* `out`: the card is fading out, drawn as it was left (held flash, held button) */
static void draw_card(gfx_fb *fb, int d, float bp, float k, const cstate *c, int out)
{
    float tk = q8((bp - 0.3f) / 0.7f) * k;     /* text follows the icon */
    const char *head = "", *hint = "";
    switch (d) {
    case D_BUTTON: head = "Press the button"; hint = "THE SIDE BUTTON, ONCE"; break;
    case D_TOUCH: head = "Tap the dots"; hint = "PRESS BUTTON TO SKIP"; break;
    case D_HAPTIC: head = "Felt it?"; hint = "TAP THE BELL TO REPLAY"; break;
    case D_SPK: head = "Heard it?"; hint = "TAP SPEAKER TO REPLAY"; break;
    }
    if (d == D_TOUCH) {
        for (int i = 0; i < 2; i++) {
            float x = (float)TGT[i][0], y = (float)TGT[i][1];
            float p = q8((bp - i * 0.15f) / 0.5f);
            if (p <= 0) continue;
            if (i < c->tgt) dot_disc(fb, x, y, 18.0f * p, ui_dim(C_DIS, k));
            else if (i == c->tgt) {
                dot_disc(fb, x, y, 30.0f * p, ui_dim(C_WHITE, k));
                uint16_t rc = nos_t - c->miss_t < 0.2f ? C_PRI : fmodf(nos_t - c->t_in, 1.0f) < 0.5f ? C_LINE2 : C_DIS;
                ui_circle_line(fb, x, y, 30.0f, 2.0f, ui_dim(rc, k * p));
            } else ui_circle_line(fb, x, y, 15.0f, 2.0f, ui_dim(C_LINE2, k * p));
        }
        if (c->missed) dot_disc(fb, (float)c->miss_x, (float)c->miss_y, 8.0f, ui_dim(C_DIS, k));
        float ms = nos_t - c->miss_t;
        if (ms < 0.8f) ui_label(fb, UI_CX, 330, "MISSED", ui_dim(C_PRI, k * q8((0.8f - ms) / 0.2f)), UI_CENTER);
    } else {
        int icon = DI[d].icon;
        float since = nos_t - c->play_t;
        int press = !out && c->pressed == 3 && nos_touch_travel() <= PRESS_TRAVEL;
        /* the lit disc behind a playing icon steps off over 2 levels */
        float lk = press || (d == D_BUTTON && c->held) || since < 0.6f ? 1.0f : since < 0.7f ? 0.5f : 0.0f;
        if (lk > 0 && !(d == D_BUTTON && c->held)) dot_disc(fb, UI_CX, 170, 96.0f, ui_dim(C_SURF2, k * lk));
        /* playing: one ring stepping outward (discrete, 3 steps) */
        if ((d == D_HAPTIC || d == D_SPK) && since < 0.6f) {
            int s = (int)(since / 0.2f);
            ui_circle_line(fb, UI_CX, 170, 50.0f + s * 4.0f, 2.0f, ui_dim(s == 2 ? C_LINE2 : C_DIS, k));
        }
        if (d == D_BUTTON && c->held) dot_disc(fb, UI_CX, 170, 96.0f, ui_dim(C_WHITE, k));
        dot_icon_anim(fb, icon, UI_CX, 170, 4.2f, 3.4f, ui_dim(d == D_BUTTON && c->held ? C_BG : C_WHITE, k),
                      m_clamp(bp / 0.7f, 0, 1), DOT_REVEAL_CENTER);
    }
    if (tk <= 0) return;
    ui_text(fb, UI_F_TITLE, UI_CX, 270, head, ui_dim(C_PRI, tk), UI_CENTER, 0);
    ui_label(fb, UI_CX, 302, hint, ui_dim(C_SEC, tk), UI_CENTER);
    /* buttons arrive with the text, at full colour; a finger that moves drops the press;
     * a card fading out keeps the button that answered it lit */
    int fl = out ? c->flash_b : nos_t - c->flash_t < PRESS_FLASH ? c->flash_b : 0;
    int pr = !out && nos_touch_travel() <= PRESS_TRAVEL ? c->pressed : 0;
    if (d == D_BUTTON)
        pill(fb, UI_CX, UI_BTN_Y, UI_BTN_W, "SKIP", BTN_SECONDARY, pr == 1 || fl == 1, k);
    else if (d == D_HAPTIC || d == D_SPK) {
        pill(fb, UI_BTN_PAIR_X0, UI_BTN_Y, UI_BTN_PAIR_W, "NO", BTN_SECONDARY, pr == 1 || fl == 1, k);
        pill(fb, UI_BTN_PAIR_X1, UI_BTN_Y, UI_BTN_PAIR_W, "YES", BTN_PRIMARY, pr == 2 || fl == 2, k);
    }
}

/* ======================================================================= list */
enum { LK_SUM = 0, LK_HDR, LK_ROW, LK_BTN };
typedef struct { uint8_t kind, d; const char *label; } lentry;
#define NLE_MAX (D_N + 6)
#define MU 30
#define MTOP 96
static int lunits(int k) { return k == LK_SUM ? 6 : k == LK_HDR ? 2 : k == LK_BTN ? 4 : 3; }

static struct {
    ui_list L;
    lentry e[NLE_MAX];
    int n;
    int start[NLE_MAX];
    int8_t unit_e[NLE_MAX * 6];
    int nunits;
    int was_moving;
    int flash_e; float flash_t;
} DL;

static void add_e(int kind, int d, const char *label) { DL.e[DL.n++] = (lentry){ (uint8_t)kind, (uint8_t)d, label }; }
/* While testing (or with nothing failed): Board, then Strap modules. With fails: FAILED
 * first, straight under the summary (the first fail row at y 336), then the passes, then
 * what isn't connected. */
static void list_layout(void)
{
    DL.n = 0;
    add_e(LK_SUM, 0, 0);
    if (G.done && G.nfail) {
        add_e(LK_HDR, 0, "Failed");
        for (int d = 0; d < D_N; d++) if (R[d].st == ST_FAIL) add_e(LK_ROW, d, 0);
        int h = 0;
        for (int d = 0; d < D_N; d++) if (R[d].st == ST_PASS) { if (!h++) add_e(LK_HDR, 0, "Passed"); add_e(LK_ROW, d, 0); }
        h = 0;
        for (int d = 0; d < D_N; d++) if (R[d].st == ST_SKIP) { if (!h++) add_e(LK_HDR, 0, "Skipped"); add_e(LK_ROW, d, 0); }
        h = 0;
        for (int d = 0; d < D_N; d++) if (R[d].st == ST_ABSENT) { if (!h++) add_e(LK_HDR, 0, "Not connected"); add_e(LK_ROW, d, 0); }
    } else {
        add_e(LK_HDR, 0, "Board");
        for (int d = 0; d < D_BOARD_N; d++) add_e(LK_ROW, d, 0);
        add_e(LK_HDR, 0, "Strap modules");
        for (int d = D_BOARD_N; d < D_N; d++) add_e(LK_ROW, d, 0);
    }
    add_e(LK_BTN, 0, 0);
}
static void list_init(void)
{
    list_layout();
    int u = 0;
    for (int e = 0; e < DL.n; e++) {
        DL.start[e] = u;
        int k = lunits(DL.e[e].kind);
        for (int j = 0; j < k; j++) DL.unit_e[u + j] = (int8_t)e;
        u += k;
    }
    DL.nunits = u;
    ui_list_init(&DL.L, u, MU, MTOP, 0, 0);
    DL.L.bottom_pad = 90;
    ui_list_set_n(&DL.L, u);
    DL.L.bar_t = -10.0f;
    DL.flash_e = -1;
}
static float dlist_off(void) { return floorf(ui_list_offset(&DL.L) + 0.5f); }

static float lbuild(float y, float *dy)
{
    int k = (int)((y - 60.0f) / 80.0f);
    if (k < 0) k = 0;
    if (k > 5) k = 5;
    float p = m_clamp((nos_t - G.build_t - k * 0.034f) / 0.20f, 0, 1);
    float e = floorf(bezier_ease(&EASE_STANDARD, p) * 8.0f + 0.5f) / 8.0f;
    *dy = (1.0f - e) * 14.0f;
    return e;
}

/* `built`: draw as fully built (a list fading out) */
static void draw_summary(gfx_fb *fb, float y, float k, int built)
{
    char b[40];
    /* the hero builds straight away when the run was already done as the list came in */
    int direct = G.done && G.t_done <= G.build_t;
    float hero_t0 = direct ? G.build_t : G.t_done + FADE_S;
    float p = G.done ? nos_t - G.t_done : -1;
    /* in progress: the state, then it fades for the hero; a list fading out for the
     * fails-first re-layout still shows the state, so the hero builds once, in the new list */
    if (!G.done || G.pend == PEND_RELAYOUT || (!direct && p < FADE_S && !built)) {
        float sk = G.done && G.pend != PEND_RELAYOUT ? q8(1.0f - p / FADE_S) : 1.0f;
        ui_state(fb, UI_CX, (int)(y + 44), G.running || G.done ? "TESTING" : "STOPPED", ui_dim(C_SEC, k * sk));
        /* "12 OF 13" tested parts resolved: steps out, then in, as each resolves */
        float mp = (nos_t - G.cnt_t) / 0.12f;
        if (mp >= 1) ui_label(fb, UI_CX, (int)(y + 78), G.cnt_to, ui_dim(C_PRI, k * sk), UI_CENTER);
        else if (mp < 0.5f) ui_label(fb, UI_CX, (int)(y + 78), G.cnt_from, ui_dim(C_PRI, k * sk * q8(1.0f - 2.0f * mp)), UI_CENTER);
        else ui_label(fb, UI_CX, (int)(y + 78), G.cnt_to, ui_dim(C_PRI, k * sk * q8(2.0f * mp - 1.0f)), UI_CENTER);
        /* the long waits say what they wait for */
        int nrun = 0, first = -1, cards = 0;
        for (int d = 0; d < D_N; d++) {
            if (R[d].st == ST_RUN) { if (first < 0) first = d; nrun++; }
            cards += R[d].st == ST_ASK || R[d].st == ST_WAIT;
        }
        if (!cards && first >= 0) {
            if (nrun == 1) snprintf(b, sizeof b, "%s", DI[first].title);
            else snprintf(b, sizeof b, "%s +%d", DI[first].title, nrun - 1);
            ui_label(fb, UI_CX, (int)(y + 108), b, ui_dim(C_DIS, k * sk), UI_CENTER);
        }
    } else {
        float bp = built ? 1.0f : m_clamp((nos_t - hero_t0) / UI_ENTRY_S, 0, 1);
        float pitch = hero_pitch(G.hero);
        dot_style hs = { DOT_FONT_TALL, pitch, pitch * 0.8f, ui_dim(C_WHITE, k), 0, 1, 1 };
        float yy = y + 2 + (UI_HEROW_PITCH - pitch) * 9.0f * 0.5f - (pitch > UI_HEROW_PITCH ? 4 : 0);
        if (bp < 1) dot_text_anim(fb, &hs, UI_CX, yy, G.hero, 1, bp, DOT_REVEAL_RANDOM, 29);
        else dot_text_a(fb, &hs, UI_CX, yy, G.hero, 1);
        float tk = k * q8((bp - 0.3f) / 0.7f);
        int nabs = 0;
        for (int d = 0; d < D_N; d++) nabs += R[d].st == ST_ABSENT;
        if (tk > 0) {
            if (G.nfail) {                 /* the screen's one red: the dot by the fail count */
                snprintf(b, sizeof b, "%d FAILED", G.nfail);
                int w1 = ui_label_width("PASSED"), w2 = ui_label_width(b);
                int x0 = UI_CX - (w1 + 28 + 16 + w2) / 2;
                ui_label(fb, x0, (int)(y + 108), "PASSED", ui_dim(C_SEC, tk), UI_LEFT);
                dot_disc(fb, x0 + w1 + 28 + 4, y + 102, 8.0f, ui_dim(C_RED, tk));
                ui_label(fb, x0 + w1 + 28 + 16, (int)(y + 108), b, ui_dim(C_PRI, tk), UI_LEFT);
            } else if (nabs || G.nskip) {  /* what wasn't tested, said quietly */
                if (G.nskip) snprintf(b, sizeof b, "%d SKIPPED", G.nskip);
                else snprintf(b, sizeof b, "%d NOT CONNECTED", nabs);
                int w1 = ui_label_width("PASSED"), w2 = ui_label_width(b);
                int x0 = UI_CX - (w1 + 24 + 6 + w2) / 2;
                ui_label(fb, x0, (int)(y + 108), "PASSED", ui_dim(C_SEC, tk), UI_LEFT);
                dot_disc(fb, x0 + w1 + 12 + 3, y + 102, 5.0f, ui_dim(C_DIS, tk));
                ui_label(fb, x0 + w1 + 24 + 6, (int)(y + 108), b, ui_dim(C_DIS, tk), UI_LEFT);
            } else ui_label(fb, UI_CX, (int)(y + 108), "PASSED", ui_dim(C_SEC, tk), UI_CENTER);
        }
    }
    /* one dot per part, in list order */
    const float pd = 16.0f;
    float x0 = UI_CX - (D_N - 1) * pd * 0.5f - 4.0f;
    for (int d = 0; d < D_N; d++) {
        float q = built ? 1.0f : q8((nos_t - G.build_t - 0.08f - d * 0.012f) / 0.12f);
        if (q <= 0) continue;
        float gx = d >= D_BOARD_N ? 8.0f : 0.0f;
        status_dot(fb, x0 + d * pd + gx, y + 142, 8.0f * q, R[d].st, k);
    }
}

static void draw_row(gfx_fb *fb, int d, float y, float h, int pressed, float k)
{
    float cy = y + h * 0.5f;
    int lx, rx;
    row_cols(cy, &lx, &rx);
    if (rx - lx < 140) return;
    if (pressed) ui_rrect(fb, lx - 20, y + 5, rx - lx + 40, h - 10, (h - 10) * 0.5f, C_SURF2);
    const dres *r = &R[d];
    int tx = lx + 24;
    const char *val = r->st == ST_PASS ? "PASS" : r->st == ST_FAIL ? "FAIL" : r->st == ST_SKIP ? "SKIPPED" : 0;
    const int vx = rx - (UI_ROW_X1 - UI_VALUE_X);
    int nw = ui_text_width(UI_F_BODY, DI[d].name, 0), vw = val ? dlab_w(val) : 0;
    int cl = vx - vw;
    if (tx + nw + ROW_GAP > cl) {                 /* the name gives way at the bezel */
        int g0, g1;
        ui_row_span(cy - 11, 2, &g0, &g1);
        tx = cl - ROW_GAP - nw;
        if (tx < g0 + 20) tx = g0 + 20;
    }
    int gap = cl - (tx + nw);                      /* the value fades out before 8 px */
    float vk = gap >= ROW_GAP ? 1.0f : q8((gap - 8) / (float)(ROW_GAP - 8));
    float kc = resolved(r->st) ? q8((nos_t - r->t_done) / 0.2f) : 0;   /* result cross-fade */
    if (r->st == ST_ABSENT && r->t_done < 0) kc = 1;
    if (G.done && G.nfail && r->t_done < G.t_done) kc = 1;            /* re-laid out: settled */
    status_dot(fb, tx - 19, cy - 11, r->st == ST_FAIL ? 12 : 10, r->st, k);
    int absent = r->st == ST_ABSENT;
    ui_text(fb, UI_F_BODY, tx, cap_base(UI_F_BODY, cy - 11), DI[d].name, ui_dim(absent ? C_SEC : C_PRI, k), UI_LEFT, 0);
    int sb = cap_base(UI_F_LABEL, cy + 17);
    if (kc < 1) {
        const char *s = r->st == ST_ASK ? "WAITING FOR YOU" : r->st == ST_WAIT ? "WAITING" : "TESTING";
        dlab(fb, tx, sb, s, ui_dim(r->st == ST_RUN ? C_SEC : C_DIS, k * (1.0f - kc)), UI_LEFT);
    }
    if (kc > 0) {
        uint16_t sc = r->st == ST_FAIL ? C_PRI : absent || r->st == ST_SKIP ? C_DIS : C_SEC;
        dlab(fb, tx, sb, r->why, ui_dim(sc, k * kc), UI_LEFT);
        if (val && vk > 0) dlab(fb, vx, cap_base(UI_F_LABEL, cy - 11), val,
                                ui_dim(r->st == ST_FAIL ? C_PRI : C_DIS, k * kc * vk), UI_RIGHT);
    }
    chevron(fb, rx, cy - 11, ui_dim(C_DIS, k));
}

static int list_pressed(void)
{
    if (DL.flash_e >= 0 && nos_t - DL.flash_t < PRESS_FLASH) return DL.flash_e;
    if (DL.L.pressed >= 0 && DL.L.pressed < DL.nunits && nos_t - DL.L.press_t > PRESS_HOLD &&
        nos_touch_travel() <= PRESS_TRAVEL) return DL.unit_e[DL.L.pressed];
    return -1;
}

static void draw_list(gfx_fb *fb, float k, int built)
{
    float off = dlist_off();
    int pe = k >= 1 ? list_pressed() : -1;
    for (int e = 0; e < DL.n; e++) {
        float h = (float)(lunits(DL.e[e].kind) * MU);
        float y = MTOP + DL.start[e] * MU - off;
        if (y + h < 0) continue;
        if (y > GFX_H) break;
        float bdy = 0, bk = built ? 1.0f : lbuild(y, &bdy);
        if (bk <= 0) continue;
        y += floorf(bdy + 0.5f);
        float cy = y + h * 0.5f;
        float rk = qfade(cy) * bk * k;
        const lentry *le = &DL.e[e];
        if (le->kind == LK_SUM) draw_summary(fb, y, rk, built);
        else if (le->kind == LK_HDR) {
            int lx, rx;
            row_cols(cy, &lx, &rx);
            ui_label(fb, lx, (int)(y + h - 16), le->label, ui_dim(C_DIS, rk), UI_LEFT);
        } else if (le->kind == LK_ROW) draw_row(fb, le->d, y, h, e == pe, rk);
        else if (G.done && rk > 0) {
            float hero_t0 = G.t_done <= G.build_t ? G.build_t : G.t_done + FADE_S;
            float bk2 = built ? 1.0f : q8((nos_t - hero_t0) / UI_ENTRY_S);
            if (bk2 > 0) pill(fb, UI_CX, y + 60, UI_BTN_W, "RE-RUN", BTN_SECONDARY, e == pe, rk * bk2);
        }
    }
    float content = (float)(MTOP + DL.nunits * MU + DL.L.bottom_pad);
    if (k >= 1) ui_scrollbar(fb, DL.L.sc.x, content, GFX_H, DL.L.bar_alpha);
}

static const nos_screen scr_diag_row;
static int row_open;
static int list_input(const nos_input *in)
{
    if (in->type == IN_DOWN) DL.was_moving = DL.L.sc.moving && fabsf(DL.L.sc.v) > 60.0f;
    if (in->type == IN_TAP) {
        nos_input up = *in; up.type = IN_UP;
        ui_list_input(&DL.L, &up);
        if (DL.was_moving) return 1;
        float fy = in->y + dlist_off() - MTOP;
        int u = fy >= 0 ? (int)(fy / MU) : -1;
        if (u < 0 || u >= DL.nunits) return 1;
        int e = DL.unit_e[u];
        if (DL.e[e].kind == LK_ROW) {
            DL.flash_e = e; DL.flash_t = nos_t;
            nos_feedback(FB_TAP);
            row_open = DL.e[e].d;
            nos_push(&scr_diag_row, TX_PUSH);
        } else if (DL.e[e].kind == LK_BTN && G.done) {
            float by = MTOP + DL.start[e] * MU - dlist_off() + 60;
            if (fabsf(in->y - by) <= UI_BTN_H * 0.5f + 4 && fabsf(in->x - (float)UI_CX) <= UI_BTN_W * 0.5f + 10) {
                DL.flash_e = e; DL.flash_t = nos_t;
                nos_feedback(FB_TAP);
                G.pend = PEND_RERUN; G.pend_t = nos_t;   /* the list fades out as it is, then the run restarts */
                G.title_off0 = dlist_off();
            }
        }
        return 1;
    }
    return ui_list_input(&DL.L, in);
}

/* ======================================================================= screen */
static void diag_enter(void)
{
    if (G.inited && nos_revealed()) {          /* back from a row page: keep it all */
        G.covered = 0;
        if (G.running) {
            if (R[D_GPS].st == ST_RUN) hal_gps_enable(1);
            if (R[D_HR].st == ST_RUN) hal_hr_enable(1);
            if (R[D_LTE].st == ST_RUN && T.lte_we_on) hal_lte_enable(1);
        }
        return;
    }
    G.inited = 1;
    G.covered = 0;
    G.step_frame = 0;
    run_start(1);
}
/* leaving (back, home, or the screen going to sleep / AOD): the run stops cleanly */
static void diag_leave(void)
{
    if (nos_leaving_covered()) { G.covered = 1; return; }   /* a row page: the tests carry on */
    run_abort();
    G.inited = 0;
}
static void diag_tick(float dt)
{
    ui_list_tick(&DL.L, dt);
    diag_step(dt);
    if (G.pend && nos_t - G.pend_t >= FADE_S) {
        if (G.pend == PEND_RERUN) run_start(2);
        else {                                   /* fails to the top: rebuild in the new order */
            G.title_off0 = dlist_off(); G.title_t0 = nos_t;
            list_init();
            G.build_t = nos_t;
            G.pend = 0;
        }
    }
    if (!G.pend && G.view != VIEW_LIST && G.view >= 0) card_tick();
}
static void diag_draw(gfx_fb *fb)
{
    /* the title rides the list scroll; after a reset it slides in from where it was */
    float ts = G.view == VIEW_LIST || G.pend ? dlist_off() : 0;
    float tp = (nos_t - G.title_t0) / UI_ENTRY_S;
    if (tp < 1 && !G.pend) ts = fmaxf(ts, fminf(G.title_off0, 48.0f) * (1.0f - q8(tp)));
    ui_title(fb, "DIAGNOSTICS", ts);
    if (G.pend) { draw_list(fb, q8(1.0f - (nos_t - G.pend_t) / FADE_S), 1); return; }
    float p = nos_t - G.sw_t;
    int fading = G.pview != VIEW_NONE && p < FADE_S;
    if (G.view >= 0 || (fading && G.pview >= 0)) {
        float sk = 1;
        if (fading && G.view < 0) sk = q8(1.0f - p / FADE_S);          /* the last card: the step goes too */
        else if (!fading && G.pview == VIEW_NONE) sk = q8((m_clamp((nos_t - G.build_t) / UI_ENTRY_S, 0, 1) - 0.3f) / 0.7f);
        if (sk > 0) draw_step(fb, sk);
    }
    if (fading) {
        if (G.pview == VIEW_LIST) draw_list(fb, q8(1.0f - p / FADE_S), 1);
        else draw_card(fb, G.pview, 1, q8(1.0f - p / FADE_S), &Cp, 1);
        return;
    }
    float bp = m_clamp((nos_t - G.build_t) / UI_ENTRY_S, 0, 1);
    if (G.view == VIEW_LIST) draw_list(fb, 1, 0);
    else draw_card(fb, G.view, bp, 1, &C, 0);
}
static int diag_input(const nos_input *in)
{
    int btn = in->type == IN_BTN || in->type == IN_BTN_LONG || in->type == IN_BTN_DOUBLE;
    if (G.pend) return !btn;
    /* touches wait out a view cross-fade; the side button never does (the button card
     * takes a press from its first frame) */
    if (!btn && nos_t - G.sw_t < FADE_S && G.pview != VIEW_NONE) return 1;
    if (G.view != VIEW_LIST) return card_input(in);
    if (btn) return 0;
    return list_input(in);
}

const nos_screen scr_diag = { "DIAG", diag_enter, diag_leave, diag_tick, diag_draw, diag_input, 0 };

/* ======================================================================= row page */
int set_modules_absent_copy(uint32_t ext, const char **body, const char **foot);   /* set_modules.c */
static struct { int d; float t0; } RV;

static void row_enter(void)
{
    if (nos_revealed()) return;
    RV.d = row_open;
    RV.t0 = nos_enter_t();
}
static void row_tick(float dt) { diag_step(dt); }
static void row_draw(gfx_fb *fb)
{
    int d = RV.d;
    const dres *r = &R[d];
    float bp = m_clamp((nos_t - RV.t0) / UI_ENTRY_S, 0, 1);
    ui_title(fb, DI[d].title, 0);
    if (r->st == ST_ABSENT) {                  /* the OS's standard absent state, as Modules */
        const char *body = "", *foot = 0;
        set_modules_absent_copy(DI[d].ext, &body, &foot);
        if (d == D_SD) ui_absent(fb, DOT_ICON_SD, "No card", body, 0, bp);
        else ui_absent(fb, DI[d].icon, "Not connected", body, foot, bp);
        return;
    }
    dot_icon_anim(fb, DI[d].icon, UI_CX, 126, 3.6f, 2.9f, C_WHITE, m_clamp(bp / 0.7f, 0, 1), DOT_REVEAL_CENTER);
    float tk = q8((bp - 0.3f) / 0.7f);
    if (tk <= 0) return;
    float kc = resolved(r->st) ? q8((nos_t - r->t_done) / 0.2f) : 0;
    if (kc < 1) ui_state(fb, UI_CX, 214, r->st == ST_ASK || r->st == ST_WAIT ? "WAITING" : "TESTING",
                         ui_dim(C_SEC, tk * (1.0f - kc)));
    if (kc > 0) {
        /* the verdict in words, the one size up; the reason only when it failed */
        const char *s = r->st == ST_PASS ? "Passed" : r->st == ST_SKIP ? "Skipped" : "Failed";
        ui_text(fb, UI_F_TITLE, UI_CX, 220, s, ui_dim(C_PRI, tk * kc), UI_CENTER, 0);
        if (r->st == ST_FAIL) {                 /* the one red on the page */
            int w = ui_text_width(UI_F_TITLE, s, 0);
            dot_disc(fb, UI_CX - w * 0.5f - 18, 210, 10, ui_dim(C_RED, tk * kc));
            dlab(fb, UI_CX, 250, r->why, ui_dim(C_SEC, tk * kc), UI_CENTER);
        }
    }
    ui_label(fb, UI_CX, 286, "READING", ui_dim(C_DIS, tk), UI_CENTER);
    if (kc > 0 && (r->rd[0][0] || r->rd[1][0])) {
        for (int i = 0; i < 2; i++)
            if (r->rd[i][0]) dlab(fb, UI_CX, 310 + i * 24, r->rd[i], ui_dim(C_PRI, tk * kc), UI_CENTER);
    } else dlab(fb, UI_CX, 310, UI_NONE, ui_dim(C_DIS, tk), UI_CENTER);
    ui_label(fb, UI_CX, 370, "CHECK", ui_dim(C_DIS, tk), UI_CENTER);
    for (int i = 0; i < 2; i++)
        if (DI[d].check[i]) dlab(fb, UI_CX, 394 + i * 22, DI[d].check[i], ui_dim(C_SEC, tk), UI_CENTER);
}
static int row_input(const nos_input *in) { (void)in; return 0; }
static const nos_screen scr_diag_row = { "DIAG_ROW", row_enter, 0, row_tick, row_draw, row_input, 0 };
