/* set_modules.c — Modules: diagnostics dashboard for every board part and strap module,
 * and the hardware bring-up tool for the real watch.
 *
 * List: a summary (online count + one dot per module), then BOARD and STRAP sections.
 * Each row: status dot (white = present/ok, grey = absent), name, bus/part, live value,
 * chevron. Live values refresh at 1 Hz so the text is readable and damage settles.
 * The list ends with RUN DIAGNOSTICS, which pushes the pass/fail test run (set_diag.c).
 *
 * Tapping a row pushes a detail page: a live readout plus a test action
 *   touch: five edge targets   IMU: bubble level   RTC: resync   SD: capacity
 *   haptic: play each pattern  speaker: play each SFX   mic: live dB + envelope
 *   GPS: satellites + SNR      LTE: power on/off   env: T/RH/P   light: lux, prox, gestures
 *   compass: dial + calibrate  HR: live PPG trace
 * Every page has the standard "not connected" state (ui_absent, the same words as the
 * module's app) plus the part and the exact wiring it expects, because this page is the
 * bring-up tool.
 *
 * Shared with Settings: set_modules_online() is the one module count in the OS;
 * Settings > Storage / Cellular open the SD card / Cellular pages here (as user pages:
 * titled STORAGE, no wiring), and Wi-Fi / Bluetooth here open the Settings radio pages
 * with the bus line added. One word per radio state everywhere. */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "svc_track.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void set_settings_open_radio(int bt, int from_modules);   /* set_settings.c */

enum { M_TOUCH = 0, M_IMU, M_RTC, M_SD, M_BATT, M_WIFI, M_BLE,
       M_HAPTIC, M_MIC, M_SPK, M_GPS, M_LTE, M_ENV, M_LIGHT, M_MAG, M_HR, M_COUNT };
#define M_BOARD_N 7
#define PRESS_FLASH 0.10f        /* a quick tap still shows its pressed state 3 frames */
#define PRESS_HOLD  0.05f        /* a held row shows its pill after 50 ms...   */
#define PRESS_TRAVEL 8           /* ...and drops it once the finger has moved 8 px (a scroll) */
#define ROW_GAP     24           /* least gap between a row's name and its value */
#define LIVE_S 1.0f              /* live text readings refresh at 1 Hz */
#define MORPH_S 0.28f

typedef struct {
    const char *name, *bus, *title;
    int icon;
    uint32_t ext;          /* EXT_* bit for strap modules, 0 = on the board */
    const char *body;      /* absent state: what it adds (the module's app says the same) */
    const char *foot;      /* absent footer: the module's user name */
    const char *wire;      /* bring-up: the part and exactly where it is expected */
} mod_info;

static const mod_info MI[M_COUNT] = {
    { "Touch", "I2C 0x38  FT3168", "TOUCH", DOT_ICON_HAND, 0, "", "", "" },
    { "Motion", "I2C 0x6B  QMI8658", "MOTION", DOT_ICON_LEVEL, 0, "", "", "" },
    { "Clock", "I2C 0x51  PCF85063", "CLOCK", DOT_ICON_TIMER, 0, "", "", "" },
    { "SD card", "SDMMC 1-BIT", "SD CARD", DOT_ICON_SD, 0, "Insert a microSD card for voice memos and logs", "", "" },
    { "Battery", "ADC GPIO4  3:1", "BATTERY", DOT_ICON_BATTERY, 0, "", "", "" },
    { "Wi-Fi", "ESP32-S3  2.4 GHZ", "WI-FI", DOT_ICON_WIFI, 0, "", "", "" },
    { "Bluetooth", "ESP32-S3  NUS", "BLUETOOTH", DOT_ICON_BT, 0, "", "", "" },
    { "Haptics", "GPIO16  PWM", "HAPTICS", DOT_ICON_BELL, EXT_HAPTIC, "Add the motor to feel every tap",
      "HAPTIC MODULE", "PWM DRIVER  GPIO16" },
    { "Microphone", "I2S IN  INMP441", "MICROPHONE", DOT_ICON_MIC, EXT_MIC, "Add the mic strap to record voice memos",
      "MIC MODULE", "INMP441  BCLK 1 WS 2 SD 3" },
    { "Speaker", "I2S OUT  MAX98357A", "SPEAKER", DOT_ICON_SPEAKER, EXT_SPEAKER, "Add the speaker for sounds and alarms",
      "SPEAKER MODULE", "MAX98357A  BCLK1 LRC2 DIN5 SD6" },
    { "GPS", "UART1  M10", "GPS", DOT_ICON_SAT, EXT_GPS, "Add the GPS for speed and tracks",
      "GPS MODULE", "M10  UART1  RX 7 TX 17" },
    { "Cellular", "UART0  EG800AK", "CELLULAR", DOT_ICON_SIGNAL, EXT_LTE, "Add the LTE strap to text without your phone",
      "LTE MODULE", "EG800AK  RX 44 TX 43 PWRKEY 18" },
    { "Environment", "I2C 0x76  BME280", "ENVIRONMENT", DOT_ICON_THERMO, EXT_ENV, "Add the climate sensor for pressure",
      "CLIMATE MODULE", "BME280  0x76/77  SDA 47 SCL 48" },
    { "Light", "I2C 0x39  APDS-9960", "LIGHT", DOT_ICON_SUN, EXT_LIGHT, "Add the light sensor for auto brightness",
      "LIGHT MODULE", "APDS-9960  0x39  SDA 47 SCL 48" },
    { "Compass", "I2C 0x0D  QMC5883L", "COMPASS", DOT_ICON_COMPASS, EXT_MAG, "Add the compass to see your heading",
      "COMPASS MODULE", "QMC5883L  0x0D  SDA 47 SCL 48" },
    { "Heart rate", "I2C 0x57  MAX30102", "HEART RATE", DOT_ICON_HEART, EXT_HR, "Add the strap to measure BPM and SpO2",
      "HR MODULE", "MAX30102  0x57  SDA 47 SCL 48" },
};

static const char *const k_hap_names[] = { "Tick", "Click", "Double", "Heavy", "Buzz", "Alarm", "Heartbeat" };
#define N_HAP 7
static const char *const k_sfx_names[SFX_COUNT] = {
    "Tap", "Tick", "Back", "Open", "Toggle on", "Toggle off", "Notify", "Alarm", "Timer done",
    "Success", "Error", "Charge", "Boot", "Shutter" };

/* ======================================================================= helpers */
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
/* technical identifiers: small mono caps, tracking 1 */
static void ident(gfx_fb *fb, int x, int y, const char *s, uint16_t col, int align)
{
    ui_text(fb, UI_F_SMALL, x, y, s, col, align, 1);
}
/* caps label + case-kept unit; an empty value draws the unit alone */
static int lab_u(gfx_fb *fb, int x, int y, const char *v, const char *u, uint16_t col, int align)
{
    if (u && *u && (!v || !*v)) return ui_text(fb, UI_F_LABEL, x, y, u, col, align, 2);
    if (u && *u) return ui_label_unit(fb, x, y, v, u, col, align);
    return ui_label(fb, x, y, v, col, align);
}
static int lab_u_w(const char *v, const char *u)
{
    int w = v && *v ? ui_label_width(v) : 0;
    if (u && *u) w += (w ? 8 : 0) + ui_text_width(UI_F_LABEL, u, 2);
    return w;
}

typedef struct { char from[24], to[24]; float t0; } mmt;
static void mmt_set(mmt *m, const char *s, int init)
{
    if (init) { snprintf(m->to, sizeof m->to, "%s", s); memcpy(m->from, m->to, sizeof m->from); m->t0 = -100; return; }
    if (!strcmp(m->to, s)) return;
    memcpy(m->from, m->to, sizeof m->from);
    snprintf(m->to, sizeof m->to, "%s", s);
    m->t0 = nos_t;
}
static void mmt_draw(gfx_fb *fb, const mmt *m, const dot_style *st, float x, float y, float dur)
{
    float p = (nos_t - m->t0) / dur;
    if (p >= 1) dot_text_a(fb, st, x, y, m->to, 1);
    else dot_text_morph(fb, st, x, y, m->from, m->to, 1, p);
}
static float hero_pitch(const char *s)
{
    int n = 0;
    for (const unsigned char *c = (const unsigned char *)s; *c; c++)
        if (*c != ':' && *c != '.' && *c != ',' && *c != 0xC2 && *c != 0xB0) n++;
    return n <= 4 ? UI_HERO_PITCH : UI_HEROW_PITCH;
}

static int mod_present(int m)
{
    if (MI[m].ext) return (hal_ext_present() & MI[m].ext) != 0;
    return 1;
}

static const char *cardinal(float h)
{
    static const char *const c[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    int i = (int)floorf(fmodf(h + 22.5f + 360.0f, 360.0f) / 45.0f);
    return c[i & 7];
}
static void fmt_gb(double gb, char *o, int n) { snprintf(o, (size_t)n, gb >= 10 ? "%.0f" : "%.1f", gb); }
static double gib(uint64_t b) { return (double)b / (1024.0 * 1024.0 * 1024.0); }
static int rail_mv(const hal_power *pw) { return pw->rail_mv > 0 ? pw->rail_mv : hal_battery_mv(); }

/* ======================================================================= sampling */
static struct {
    int ok[M_COUNT];
    char val[M_COUNT][24];
    char vu[M_COUNT][8];         /* case-kept unit of val */
    hal_imu imu; int imu_ok;
    hal_storage sd;
    hal_power pw;
    hal_gps gps; int gps_ok;
    hal_lte lte; int lte_ok;
    hal_env env; int env_ok;
    hal_light light; int light_ok;
    hal_mag mag; int mag_ok;
    hal_hr hr; int hr_ok;
    float mic_db;
    hal_datetime dt;
    int touch_x, touch_y, touched;
    float sd_t;
} V;

static void sample(int poll_hr, int poll_mic)
{
    uint32_t ext = hal_ext_present();
    V.imu_ok = hal_imu_read(&V.imu);
    hal_get_datetime(&V.dt);
    if (nos_t - V.sd_t > 1.0f || V.sd_t == 0) { hal_storage_get(&V.sd); V.sd_t = nos_t; }
    hal_power_get(&V.pw);
    V.gps_ok = (ext & EXT_GPS) && hal_gps_get(&V.gps);
    V.lte_ok = (ext & EXT_LTE) && hal_lte_get(&V.lte);
    V.env_ok = (ext & EXT_ENV) && hal_env_get(&V.env) && V.env.valid;
    V.light_ok = (ext & EXT_LIGHT) && hal_light_get(&V.light) && V.light.valid;
    V.mag_ok = (ext & EXT_MAG) && hal_mag_get(&V.mag) && V.mag.valid;
    V.hr_ok = poll_hr && (ext & EXT_HR) && hal_hr_get(&V.hr);
    V.mic_db = (poll_mic && (ext & EXT_MIC)) ? hal_mic_db() : -1.0f;
}

/* presence / health of every part, the same rule everywhere */
static void presence(int ok[M_COUNT], int imu_ok, const hal_storage *sd, const hal_power *pw)
{
    for (int m = 0; m < M_COUNT; m++) ok[m] = mod_present(m);
    ok[M_IMU] = imu_ok;
    ok[M_SD] = sd->mounted;
    ok[M_BATT] = rail_mv(pw) > 0;
    ok[M_WIFI] = hal_status(HAL_FEAT_WIFI) == HAL_CONN_UP;
    ok[M_BLE] = hal_status(HAL_FEAT_BLE) != HAL_CONN_OFF;
}

int set_modules_online(int *total)
{
    int ok[M_COUNT];
    hal_imu imu; hal_storage sd; hal_power pw;
    int iok = hal_imu_read(&imu);
    hal_storage_get(&sd);
    hal_power_get(&pw);
    presence(ok, iok, &sd, &pw);
    int n = 0;
    for (int m = 0; m < M_COUNT; m++) n += ok[m] != 0;
    if (total) *total = M_COUNT;
    return n;
}

static void setv(int m, const char *v, const char *u)
{
    snprintf(V.val[m], sizeof V.val[m], "%s", v);
    snprintf(V.vu[m], sizeof V.vu[m], "%s", u ? u : "");
}

static void list_values(void)
{
    char b[24];
    presence(V.ok, V.imu_ok, &V.sd, &V.pw);
    if (V.touched) { snprintf(b, sizeof b, "%d, %d", V.touch_x, V.touch_y); setv(M_TOUCH, b, 0); }
    else setv(M_TOUCH, "Ready", 0);
    if (V.imu_ok) {
        float g = sqrtf(V.imu.ax * V.imu.ax + V.imu.ay * V.imu.ay + V.imu.az * V.imu.az);
        snprintf(b, sizeof b, "%.2f", g); setv(M_IMU, b, "g");
    } else setv(M_IMU, "No reply", 0);
    ui_fmt_time(b, sizeof b, V.dt.hour, V.dt.minute); setv(M_RTC, b, 0);
    if (V.sd.mounted) { fmt_gb(gib(V.sd.free_bytes), b, sizeof b); setv(M_SD, b, "GB"); }
    else setv(M_SD, "No card", 0);
    int mv = rail_mv(&V.pw);
    if (mv > 0) { snprintf(b, sizeof b, "%.2f", mv / 1000.0f); setv(M_BATT, b, "V"); }
    else setv(M_BATT, UI_NONE, 0);
    /* one word per radio state, the same as Settings */
    hal_conn w = hal_status(HAL_FEAT_WIFI);
    setv(M_WIFI, !nos_prefs.wifi ? "Off" : w == HAL_CONN_UP ? "Connected" : w == HAL_CONN_TRYING ? "Joining" :
         "Not connected", 0);
    int phone = hal_phone_connected() || svc_sys()->phone_connected;
    setv(M_BLE, phone ? "Connected" : "Not connected", 0);
    if (V.ok[M_HAPTIC]) {
        if (nos_prefs.haptics) { snprintf(b, sizeof b, "%d%%", nos_prefs.haptic_strength); setv(M_HAPTIC, b, 0); }
        else setv(M_HAPTIC, "Muted", 0);
    }
    if (V.ok[M_MIC]) {
        if (V.mic_db >= 0) { snprintf(b, sizeof b, "%.0f", V.mic_db); setv(M_MIC, b, "dB"); }
        else setv(M_MIC, "Ready", 0);
    }
    if (V.ok[M_SPK]) { snprintf(b, sizeof b, "Vol %d%%", nos_prefs.volume); setv(M_SPK, b, 0); }
    if (V.ok[M_GPS]) {
        if (V.gps.valid) { snprintf(b, sizeof b, "%d sats", V.gps.sats_used); setv(M_GPS, b, 0); }
        else if (V.gps.sats_view) { snprintf(b, sizeof b, "%d in view", V.gps.sats_view); setv(M_GPS, b, 0); }
        else setv(M_GPS, "Searching", 0);
    }
    if (V.ok[M_LTE]) {
        if (!V.lte_ok || !V.lte.powered) setv(M_LTE, "Off", 0);
        else if (!V.lte.sim) setv(M_LTE, "No SIM", 0);
        else if ((V.lte.registered == 1 || V.lte.registered == 2) && V.lte.rssi_dbm) {
            snprintf(b, sizeof b, "%d", V.lte.rssi_dbm); setv(M_LTE, b, "dBm");
        } else setv(M_LTE, "Searching", 0);
    }
    if (V.ok[M_ENV]) {
        if (V.env_ok) {
            float t = nos_prefs.units_metric ? V.env.temp_c : V.env.temp_c * 1.8f + 32.0f;
            snprintf(b, sizeof b, "%.0f", t); setv(M_ENV, b, nos_prefs.units_metric ? "\xB0" "C" : "\xB0" "F");
        }
        else setv(M_ENV, "Warming up", 0);
    }
    if (V.ok[M_LIGHT]) {
        if (V.light_ok) { snprintf(b, sizeof b, "%.0f", V.light.lux); setv(M_LIGHT, b, "LUX"); }
        else setv(M_LIGHT, "Ready", 0);
    }
    if (V.ok[M_MAG]) {
        if (V.mag_ok) { snprintf(b, sizeof b, "HDG %03.0f", fmodf(V.mag.heading_deg + 360.0f, 360.0f)); setv(M_MAG, b, 0); }
        else setv(M_MAG, "Ready", 0);
    }
    if (V.ok[M_HR]) setv(M_HR, "Ready", 0);
    for (int m = M_HAPTIC; m < M_COUNT; m++) if (!V.ok[m]) setv(m, "Absent", 0);
}

/* ======================================================================= list */
enum { LK_SUM = 0, LK_HDR, LK_ROW, LK_BTN };
typedef struct { uint8_t kind, mod; const char *label; } lentry;
static const lentry LE[] = {
    { LK_SUM, 0, 0 },
    { LK_HDR, 0, "Board" },
    { LK_ROW, M_TOUCH, 0 }, { LK_ROW, M_IMU, 0 }, { LK_ROW, M_RTC, 0 }, { LK_ROW, M_SD, 0 },
    { LK_ROW, M_BATT, 0 }, { LK_ROW, M_WIFI, 0 }, { LK_ROW, M_BLE, 0 },
    { LK_HDR, 0, "Strap modules" },
    { LK_ROW, M_HAPTIC, 0 }, { LK_ROW, M_MIC, 0 }, { LK_ROW, M_SPK, 0 }, { LK_ROW, M_GPS, 0 },
    { LK_ROW, M_LTE, 0 }, { LK_ROW, M_ENV, 0 }, { LK_ROW, M_LIGHT, 0 }, { LK_ROW, M_MAG, 0 },
    { LK_ROW, M_HR, 0 },
    { LK_BTN, 0, 0 },          /* RUN DIAGNOSTICS: tests that every part works (set_diag.c) */
};
#define NLE ((int)(sizeof LE / sizeof LE[0]))
#define MU 30
#define MTOP 96
static int lunits(int k) { return k == LK_SUM ? 6 : k == LK_HDR ? 2 : k == LK_BTN ? 4 : 3; }
#define DIAG_W 300

static struct {
    ui_list L;
    int start[NLE];
    int8_t unit_e[NLE * 6];
    int nunits;
    int inited, was_moving;
    float t0, next;
    mmt sum;
    int online;
    int flash_e; float flash_t;
} ML;

static void mods_count(void)
{
    int n = 0;
    for (int m = 0; m < M_COUNT; m++) n += V.ok[m] != 0;
    ML.online = n;
}

static void modules_enter(void)
{
    int revealed = ML.inited && nos_revealed();
    if (!revealed) {
        int u = 0;
        for (int e = 0; e < NLE; e++) {
            ML.start[e] = u;
            int k = lunits(LE[e].kind);
            for (int j = 0; j < k; j++) ML.unit_e[u + j] = (int8_t)e;
            u += k;
        }
        ML.nunits = u;
        ui_list_init(&ML.L, u, MU, MTOP, 0, 0);
        ML.L.bottom_pad = 120;
        ui_list_set_n(&ML.L, u);
        ML.L.bar_t = -10.0f;
        ML.t0 = nos_enter_t();
        ML.inited = 1;
    }
    hal_gps_enable(1);              /* satellites in the list, also under its own detail pages */
    V.sd_t = 0;
    sample(0, 1);
    list_values();
    mods_count();
    char b[12]; snprintf(b, sizeof b, "%d/%d", ML.online, M_COUNT);
    mmt_set(&ML.sum, b, !revealed);
    ML.next = nos_t + LIVE_S;
    ML.flash_e = -1;
}
/* covered by a detail page (or anything else): the GPS keeps its fix */
static void modules_leave(void)
{
    if (!nos_leaving_covered() && !svc_track_active()) hal_gps_enable(0);
}

static void modules_tick(float dt)
{
    ui_list_tick(&ML.L, dt);
    if (nos_t >= ML.next) {
        sample(0, 1);
        list_values();
        mods_count();
        char b[12]; snprintf(b, sizeof b, "%d/%d", ML.online, M_COUNT);
        mmt_set(&ML.sum, b, 0);
        ML.next = nos_t + LIVE_S;
    }
}

static float mlist_off(void) { return floorf(ui_list_offset(&ML.L) + 0.5f); }

static float mbuild(float y, float *dy)
{
    int k = (int)((y - 60.0f) / 80.0f);
    if (k < 0) k = 0;
    if (k > 5) k = 5;
    float p = m_clamp((nos_t - ML.t0 - k * 0.034f) / 0.20f, 0, 1);
    float e = floorf(bezier_ease(&EASE_STANDARD, p) * 8.0f + 0.5f) / 8.0f;
    *dy = (1.0f - e) * 14.0f;
    return e;
}

static void draw_summary(gfx_fb *fb, float y, float k)
{
    float bp = m_clamp((nos_t - ML.t0) / UI_ENTRY_S, 0, 1);
    float pitch = hero_pitch(ML.sum.to);
    dot_style hs = { DOT_FONT_TALL, pitch, pitch * 0.8f, ui_dim(C_WHITE, k), 0, 1, 1 };
    if (bp < 1) dot_text_anim(fb, &hs, UI_CX, y + 2, ML.sum.to, 1, bp, DOT_REVEAL_RANDOM, 21);
    else mmt_draw(fb, &ML.sum, &hs, UI_CX, y + 2, MORPH_S);
    ui_label(fb, UI_CX, (int)(y + 108), "ONLINE", ui_dim(C_SEC, k), UI_CENTER);
    /* one dot per module, in list order: the whole bus at a glance */
    const float pitch_d = 16.0f;
    float x0 = UI_CX - (M_COUNT - 1) * pitch_d * 0.5f;
    for (int m = 0; m < M_COUNT; m++) {
        float p = m_clamp((nos_t - ML.t0 - 0.08f - m * 0.012f) / 0.12f, 0, 1);
        p = floorf(p * 4.0f + 0.5f) / 4.0f;
        if (p <= 0) continue;
        float gx = m >= M_BOARD_N ? 8.0f : 0.0f;     /* gap between board and strap */
        dot_disc(fb, x0 + m * pitch_d + gx - 4.0f, y + 142, 8.0f * p, ui_dim(V.ok[m] ? C_WHITE : C_LINE2, k));
    }
}

static void draw_lrow(gfx_fb *fb, int e, float y, float h, int pressed)
{
    const lentry *le = &LE[e];
    float bdy, bk = mbuild(y, &bdy);
    if (bk <= 0) return;
    y += floorf(bdy + 0.5f);
    float cy = y + h * 0.5f;
    float k = qfade(cy) * bk;
    if (le->kind == LK_SUM) { draw_summary(fb, y, k); return; }
    int lx, rx;
    row_cols(cy, &lx, &rx);
    if (rx - lx < 140) return;
    if (le->kind == LK_HDR) { ui_label(fb, lx, (int)(y + h - 16), le->label, ui_dim(C_DIS, k), UI_LEFT); return; }
    if (le->kind == LK_BTN) {
        if (k >= 1) ui_button(fb, UI_CX, y + 60, DIAG_W, UI_BTN_H, "RUN DIAGNOSTICS", BTN_SECONDARY, pressed);
        else {                                       /* faded near the bezel: the same pill, dimmed */
            ui_rrect(fb, UI_CX - DIAG_W * 0.5f, y + 60 - UI_BTN_H * 0.5f, DIAG_W, UI_BTN_H, UI_BTN_H * 0.5f,
                     ui_dim(pressed ? C_SEC : C_SURF2, k));
            ui_label(fb, UI_CX, (int)(y + 60 + UI_F_LABEL->ascent * 0.5f - 2), "RUN DIAGNOSTICS",
                     ui_dim(pressed ? C_BG : C_PRI, k), UI_CENTER);
        }
        return;
    }
    int m = le->mod;
    if (pressed) ui_rrect(fb, lx - 20, y + 5, rx - lx + 40, h - 10, (h - 10) * 0.5f, C_SURF2);
    int ok = V.ok[m];
    const int vx = rx - (UI_ROW_X1 - UI_VALUE_X);
    int vw = lab_u_w(V.val[m], V.vu[m]);
    int nw = ui_text_width(UI_F_BODY, MI[m].name, 0);
    /* a value that can't keep ROW_GAP to the name in the standard column takes the
     * sub-line (the bus is on the detail page), so names never move in the middle band */
    int under = UI_ROW_X0 + 24 + nw + ROW_GAP + vw > UI_VALUE_X;
    int cl = under ? vx : vx - vw;
    int tx = lx + 24;
    if (tx + nw + ROW_GAP > cl) {                         /* the name gives way at the bezel */
        int g0, g1;
        ui_row_span(cy - 11, 2, &g0, &g1);
        tx = cl - ROW_GAP - nw;
        if (tx < g0 + 20) tx = g0 + 20;
    }
    int gap = cl - (tx + nw);                              /* fades out before 8 px */
    float vk = k * (gap >= ROW_GAP ? 1.0f : floorf(m_clamp((gap - 8) / (float)(ROW_GAP - 8), 0, 1) * 8.0f + 0.5f) / 8.0f);
    dot_disc(fb, tx - 19, cy - 11, 10, ui_dim(ok ? C_WHITE : C_DIS, k));
    ui_text(fb, UI_F_BODY, tx, cap_base(UI_F_BODY, cy - 11), MI[m].name, ui_dim(ok ? C_PRI : C_SEC, k), UI_LEFT, 0);
    uint16_t vc = ok ? C_SEC : C_DIS;
    if (under) lab_u(fb, tx, cap_base(UI_F_LABEL, cy + 17), V.val[m], V.vu[m], ui_dim(vc, k), UI_LEFT);
    else {
        ident(fb, tx, cap_base(UI_F_SMALL, cy + 17), MI[m].bus, ui_dim(C_DIS, k), UI_LEFT);
        if (vk > 0) lab_u(fb, vx, cap_base(UI_F_LABEL, cy - 11), V.val[m], V.vu[m], ui_dim(vc, vk), UI_RIGHT);
    }
    chevron(fb, rx, cy - 11, ui_dim(C_DIS, k));
}

static void modules_draw(gfx_fb *fb)
{
    float off = mlist_off();
    ui_title(fb, "MODULES", off);
    int pe = -1;
    if (ML.flash_e >= 0 && nos_t - ML.flash_t < PRESS_FLASH) pe = ML.flash_e;
    else if (ML.L.pressed >= 0 && ML.L.pressed < ML.nunits && nos_t - ML.L.press_t > PRESS_HOLD && nos_touch_travel() <= PRESS_TRAVEL) pe = ML.unit_e[ML.L.pressed];
    for (int e = 0; e < NLE; e++) {
        float h = (float)(lunits(LE[e].kind) * MU);
        float y = MTOP + ML.start[e] * MU - off;
        if (y + h < 0) continue;
        if (y > GFX_H) break;
        draw_lrow(fb, e, y, h, e == pe && LE[e].kind >= LK_ROW);
    }
    float content = (float)(MTOP + ML.nunits * MU + ML.L.bottom_pad);
    ui_scrollbar(fb, ML.L.sc.x, content, GFX_H, ML.L.bar_alpha);
}

static const nos_screen scr_mod_detail, scr_mod_touch;
static void open_detail(int m, int from_settings);
/* Settings' Storage and Cellular rows: the same pages, as user pages */
void set_modules_open_sd(void) { open_detail(M_SD, 1); }
void set_modules_open_lte(void) { open_detail(M_LTE, 1); }

static int modules_input(const nos_input *in)
{
    if (in->type == IN_DOWN) {
        V.touch_x = in->x; V.touch_y = in->y; V.touched = 1;
        ML.was_moving = ML.L.sc.moving && fabsf(ML.L.sc.v) > 60.0f;
    }
    if (in->type == IN_TAP) {
        nos_input up = *in; up.type = IN_UP;
        ui_list_input(&ML.L, &up);
        if (!ML.was_moving) {
            float fy = in->y + mlist_off() - MTOP;
            int u = fy >= 0 ? (int)(fy / MU) : -1;
            int e = u >= 0 && u < ML.nunits ? ML.unit_e[u] : -1;
            if (e >= 0 && LE[e].kind == LK_BTN) {
                float by = MTOP + ML.start[e] * MU - mlist_off() + 60;
                if (fabsf(in->y - by) <= UI_BTN_H * 0.5f + 4 && fabsf(in->x - (float)UI_CX) <= DIAG_W * 0.5f + 10) {
                    ML.flash_e = e; ML.flash_t = nos_t;
                    nos_feedback(FB_TAP);
                    nos_push(&scr_diag, TX_PUSH);
                }
            } else if (u >= 0 && u < ML.nunits && LE[ML.unit_e[u]].kind == LK_ROW) {
                ML.flash_e = ML.unit_e[u]; ML.flash_t = nos_t;
                nos_feedback(FB_TAP);
                open_detail(LE[ML.unit_e[u]].mod, 0);
            }
        }
        return 1;
    }
    return ui_list_input(&ML.L, in);
}

const nos_screen scr_modules = { "MODULES", modules_enter, modules_leave, modules_tick,
                                 modules_draw, modules_input, 0 };

/* ======================================================================= detail */
static struct {
    int m;
    float t0, next;
    int pressed; float flash_t;  /* action button */
    mmt hero;
    char unit[32], unit_u[12];   /* label under the hero, + case-kept unit */
    int ustate;                  /* the label is a state: "[ OFF ]" */
    char line[3][44], lu[3][12]; /* info lines + case-kept units */
    /* haptic / speaker play lists */
    ui_list L;
    int was_moving;
    int last_play; float play_t;
    int flash_row; float flash_rt;
    /* touch test */
    int tgt_hit[5]; int tgt_done;
    int fx, fy, fdown;
    /* compass */
    int calibrating;
    /* LTE */
    int lte_req; float lte_req_t;
    /* light */
    int gest; float gest_t;
    int sig;                     /* last LTE / SD state signature */
    /* mic */
    float peak;
    uint8_t wave[32];
    /* haptic: the Alarm pattern repeats until stopped */
    int alarm_on;
    /* opened from Settings: a user page (row title, no bus or wiring lines) */
    int from_set;
} D;

static void open_detail(int m, int from_settings)
{
    /* the radios have one page each in the OS, in Settings; Modules adds its bus line */
    if (m == M_WIFI || m == M_BLE) { set_settings_open_radio(m == M_BLE, 1); return; }
    D.m = m;
    D.from_set = from_settings;
    nos_push(m == M_TOUCH ? &scr_mod_touch : &scr_mod_detail, TX_PUSH);
}
static const char *detail_title(void) { return D.m == M_SD && D.from_set ? "STORAGE" : MI[D.m].title; }

static int detail_has_button(void)
{
    if (!mod_present(D.m)) return 0;
    return D.m == M_RTC || D.m == M_LTE || D.m == M_MAG;
}
static const char *detail_button(void)
{
    switch (D.m) {
    case M_RTC: return "RESYNC";
    case M_LTE: return (V.lte_ok && V.lte.powered) ? "POWER OFF" : "POWER ON";
    case M_MAG: return D.calibrating ? "DONE" : "CALIBRATE";
    default: return "";
    }
}
static int is_list_page(void) { return mod_present(D.m) && (D.m == M_HAPTIC || D.m == M_SPK); }
#define PLAY_TOP 96
#define PLAY_ROW UI_ROW_H

static void dline(int i, const char *s, const char *u)
{
    snprintf(D.line[i], sizeof D.line[i], "%s", s);
    snprintf(D.lu[i], sizeof D.lu[i], "%s", u ? u : "");
}

static void detail_values(int init)
{
    char h[24], b[44];
    h[0] = 0;
    D.unit[0] = 0; D.unit_u[0] = 0; D.ustate = 0;
    for (int i = 0; i < 3; i++) { D.line[i][0] = 0; D.lu[i][0] = 0; }
    switch (D.m) {
    case M_IMU: {
        float g = sqrtf(V.imu.ax * V.imu.ax + V.imu.ay * V.imu.ay + V.imu.az * V.imu.az);
        float tilt = g > 0.05f ? acosf(m_clamp(fabsf(V.imu.az) / g, 0, 1)) * 57.29578f : 0;
        snprintf(h, sizeof h, "%.0f\xC2\xB0", tilt);
        snprintf(D.unit, sizeof D.unit, "TILT");
        snprintf(b, sizeof b, "X %+.2f  Y %+.2f", V.imu.ax, V.imu.ay); dline(0, b, 0);
        snprintf(b, sizeof b, "Z %+.2f", V.imu.az); dline(1, b, "g");
        snprintf(b, sizeof b, "GYRO %.0f %.0f %.0f", V.imu.gx, V.imu.gy, V.imu.gz); dline(2, b, "\xB0/s");
        break;
    }
    case M_RTC:
        snprintf(h, sizeof h, "%02d:%02d:%02d", V.dt.hour, V.dt.minute, V.dt.second);
        snprintf(D.unit, sizeof D.unit, "%s %d %s %d", ui_weekday3(V.dt.weekday), V.dt.day, ui_month3(V.dt.month), V.dt.year);
        dline(0, hal_epoch_seconds() ? "UTC SYNCED" : "UTC NOT SYNCED", 0);
        break;
    case M_SD:
        if (V.sd.mounted) {
            fmt_gb(gib(V.sd.free_bytes), h, sizeof h);
            char t[12]; fmt_gb(gib(V.sd.total_bytes), t, sizeof t);
            snprintf(D.unit, sizeof D.unit, "GB FREE OF %s", t);
            int pct = V.sd.total_bytes ? (int)((1.0 - (double)V.sd.free_bytes / (double)V.sd.total_bytes) * 100 + 0.5) : 0;
            snprintf(b, sizeof b, "%d%% USED", pct); dline(0, b, 0);
        }
        if (!D.from_set) {                    /* bring-up detail, not for the Storage page */
            snprintf(b, sizeof b, "%.40s", V.sd.message[0] ? V.sd.message : "SDMMC 1-BIT, 20 MHZ"); dline(1, b, 0);
        }
        break;
    case M_BATT: {
        int mv = rail_mv(&V.pw);
        if (mv > 0) snprintf(h, sizeof h, "%.2f", mv / 1000.0f); else snprintf(h, sizeof h, UI_NONE);
        snprintf(D.unit, sizeof D.unit, "VOLTS ON THE RAIL");
        int pct = V.pw.battery_percent ? V.pw.battery_percent : hal_battery_percent();
        if (V.pw.external_power == 1) dline(0, "USB POWER", 0);
        else { snprintf(b, sizeof b, "%d%% ESTIMATE", pct); dline(0, b, 0); }
        dline(1, "GPIO4  ADC1 CH3  3:1", 0);
        break;
    }
    case M_MIC:
        if (V.mic_db >= 0) snprintf(h, sizeof h, "%.0f", V.mic_db); else snprintf(h, sizeof h, UI_NONE);
        snprintf(D.unit_u, sizeof D.unit_u, "dB SPL");
        snprintf(b, sizeof b, "PEAK %.0f", D.peak); dline(0, b, "dB");
        break;
    case M_GPS:
        if (V.gps.valid) snprintf(h, sizeof h, "%d", V.gps.sats_used); else snprintf(h, sizeof h, UI_NONE);
        snprintf(D.unit, sizeof D.unit, "SATELLITES IN USE");
        if (V.gps.valid) {
            snprintf(b, sizeof b, "%.4f%c  %.4f%c", fabs(V.gps.lat), V.gps.lat >= 0 ? 'N' : 'S',
                     fabs(V.gps.lon), V.gps.lon >= 0 ? 'E' : 'W');
            dline(0, b, 0);
            snprintf(b, sizeof b, "%s  HDOP %.1f", V.gps.fix3d ? "3D FIX" : "2D FIX", V.gps.hdop); dline(1, b, 0);
        } else { snprintf(b, sizeof b, "%d IN VIEW, NO FIX YET", V.gps.sats_view); dline(0, b, 0); }
        break;
    case M_LTE: {
        int reg = V.lte_ok && V.lte.powered && (V.lte.registered == 1 || V.lte.registered == 2) && V.lte.rssi_dbm;
        if (reg) {
            snprintf(h, sizeof h, "%d", V.lte.rssi_dbm);
            snprintf(D.unit_u, sizeof D.unit_u, "dBm");
        } else if (V.lte_ok && V.lte.powered) {
            snprintf(h, sizeof h, UI_NONE);
            snprintf(D.unit, sizeof D.unit, "%s", !V.lte.sim ? "NO SIM" : V.lte.sim == 2 ? "SIM LOCKED" : "SEARCHING");
        } else {                              /* words are never dot-matrix heroes */
            snprintf(h, sizeof h, UI_NONE);
            snprintf(D.unit, sizeof D.unit, "%s", D.lte_req > 0 ? "STARTING" : "OFF");
            D.ustate = 1;
        }
        if (V.lte_ok && V.lte.powered) {
            snprintf(b, sizeof b, "SIM %s", V.lte.sim == 1 ? "READY" : V.lte.sim == 2 ? "PIN LOCKED" : "MISSING");
            dline(0, b, 0);
            const char *r = V.lte.registered == 1 ? "HOME" : V.lte.registered == 2 ? "ROAMING" :
                            V.lte.registered == 3 ? "SEARCHING" : "NOT REGISTERED";
            if (V.lte.op[0]) snprintf(b, sizeof b, "%s  %.18s", r, V.lte.op);
            else snprintf(b, sizeof b, "%s", r);
            if (strcmp(b, D.unit)) dline(1, b, 0);        /* never repeat the state above */
        } else if (!D.from_set) dline(0, "PWRKEY ON IO18", 0);   /* bring-up only */
        break;
    }
    case M_ENV:
        if (V.env_ok) {
            float t = nos_prefs.units_metric ? V.env.temp_c : V.env.temp_c * 1.8f + 32.0f;
            snprintf(h, sizeof h, "%.1f\xC2\xB0", t);
            snprintf(D.unit, sizeof D.unit, "%s", nos_prefs.units_metric ? "CELSIUS" : "FAHRENHEIT");
            snprintf(b, sizeof b, "HUMIDITY %.0f%%", V.env.humidity); dline(0, b, 0);
            snprintf(b, sizeof b, "PRESSURE %.1f", V.env.pressure_hpa); dline(1, b, "hPa");
            snprintf(b, sizeof b, "ALTITUDE %.0f", V.env.altitude_m); dline(2, b, "m");
        } else { snprintf(h, sizeof h, UI_NONE); snprintf(D.unit, sizeof D.unit, "WAITING FOR A SAMPLE"); }
        break;
    case M_LIGHT: {
        if (V.light_ok) snprintf(h, sizeof h, "%.0f", V.light.lux); else snprintf(h, sizeof h, UI_NONE);
        snprintf(D.unit, sizeof D.unit, "LUX");
        snprintf(b, sizeof b, "PROXIMITY %d", V.light_ok ? V.light.prox : 0); dline(0, b, 0);
        static const char *const g[] = { "NONE", "UP", "DOWN", "LEFT", "RIGHT", "NEAR", "FAR" };
        snprintf(b, sizeof b, "GESTURE %s", (D.gest > 0 && D.gest <= 6 && nos_t - D.gest_t < 2.0f) ? g[D.gest] : UI_NONE);
        dline(1, b, 0);
        break;
    }
    case M_MAG:
        if (V.mag_ok) {
            float hd = fmodf(V.mag.heading_deg + 360.0f, 360.0f);
            snprintf(h, sizeof h, "%.0f\xC2\xB0", hd);
            snprintf(D.unit, sizeof D.unit, "%s", cardinal(hd));
            snprintf(b, sizeof b, "FIELD %.0f", V.mag.field_ut); dline(0, b, "uT");
            dline(1, D.calibrating ? "MOVE IN A FIGURE 8" : V.mag.calibrated ? "CALIBRATED" : "NOT CALIBRATED", 0);
        } else { snprintf(h, sizeof h, UI_NONE); snprintf(D.unit, sizeof D.unit, "WAITING FOR A SAMPLE"); }
        break;
    case M_HR:
        if (V.hr_ok && V.hr.bpm) snprintf(h, sizeof h, "%d", V.hr.bpm); else snprintf(h, sizeof h, UI_NONE);
        snprintf(D.unit, sizeof D.unit, "BPM");
        if (V.hr_ok && V.hr.spo2) { snprintf(b, sizeof b, "SPO2 %d%%", V.hr.spo2); dline(0, b, 0); }
        else if (V.hr_ok && V.hr.on_wrist && !V.hr.bpm) dline(0, "LOCKING ON", 0);
        break;
    default: break;
    }
    mmt_set(&D.hero, h, init);
}

static void detail_sensors(int on)
{
    if (D.m == M_HR) hal_hr_enable(on);
    if (D.m == M_GPS && on) hal_gps_enable(1);   /* off belongs to the Modules list under it */
    if (D.m == M_LIGHT) hal_light_gesture_enable(on);
}
static void detail_enter(void)
{
    if (nos_revealed()) { detail_sensors(1); return; }   /* back from a cover: keep everything */
    D.t0 = nos_enter_t();
    D.pressed = 0; D.flash_t = -100;
    D.calibrating = 0;
    D.lte_req = 0;
    D.gest = 0;
    D.peak = 0;
    D.last_play = -1; D.play_t = -100;
    D.flash_row = -1; D.flash_rt = -100;
    memset(D.tgt_hit, 0, sizeof D.tgt_hit); D.tgt_done = 0; D.fdown = 0;
    D.alarm_on = 0;
    if (D.m == M_TOUCH) V.touched = 0;           /* the tap that opened the page is not data */
    detail_sensors(1);
    int n = D.m == M_HAPTIC ? N_HAP : SFX_COUNT;
    ui_list_init(&D.L, n, PLAY_ROW, PLAY_TOP, 0, 0);
    D.L.bottom_pad = GFX_H / 2 - PLAY_ROW / 2;
    ui_list_set_n(&D.L, n);
    D.L.bar_t = -10.0f;
    V.sd_t = 0;
    sample(D.m == M_HR, D.m == M_MIC);
    if (D.m == M_MIC && V.mic_db > 0) D.peak = V.mic_db;
    detail_values(1);
    D.next = nos_t + LIVE_S;
}
static void detail_leave(void)
{
    if (nos_leaving_covered()) return;           /* covered: sensors and tests keep running */
    detail_sensors(0);
    if (D.m == M_MAG && D.calibrating) { hal_mag_calibrate(0); D.calibrating = 0; }
    if (D.m == M_HAPTIC && D.alarm_on) { hal_haptic_stop(); D.alarm_on = 0; }
}

static void detail_tick(float dt)
{
    if (is_list_page()) ui_list_tick(&D.L, dt);
    /* visuals every frame (bubble, dial, traces), text at 1 Hz */
    sample(D.m == M_HR, D.m == M_MIC);
    if (D.m == M_MIC) {
        if (V.mic_db > D.peak) D.peak = V.mic_db;
        if (!hal_mic_wave(D.wave)) memset(D.wave, 0, sizeof D.wave);
    }
    if (D.m == M_LIGHT) {
        int g = hal_light_gesture();
        if (g != GEST_NONE) { D.gest = g; D.gest_t = nos_t; nos_feedback(FB_TICK); detail_values(0); }
    }
    if (D.m == M_MAG && D.calibrating && V.mag_ok && V.mag.cal_progress >= 1.0f) {
        hal_mag_calibrate(0);
        D.calibrating = 0;
        nos_feedback(FB_SUCCESS);
        nos_toast("CALIBRATED");
        detail_values(0);
    }
    if (D.m == M_LTE && D.lte_req && ((V.lte_ok && V.lte.powered) == (D.lte_req > 0) || nos_t - D.lte_req_t > 20.0f))
        D.lte_req = 0;
    /* state changes (not readings) show at once, so the text never contradicts the button */
    int sig = D.m == M_LTE ? (V.lte_ok ? 1 + V.lte.powered * 2 + V.lte.sim * 4 + V.lte.registered * 16 : 0) :
              D.m == M_SD ? V.sd.mounted : 0;
    if (sig != D.sig) { D.sig = sig; detail_values(0); }
    if (nos_t >= D.next) { detail_values(0); D.next = nos_t + LIVE_S; }
}

/* ---- detail drawing ---- */
static void absent_page(gfx_fb *fb)
{
    const mod_info *mi = &MI[D.m];
    float bp = m_clamp((nos_t - D.t0) / UI_ENTRY_S, 0, 1);
    /* the same body and footer as the module's own app; Modules adds the part and its pins */
    ui_absent(fb, mi->icon, "Not connected", mi->body, mi->foot, bp);
    float k = floorf(m_clamp((bp - 0.3f) / 0.7f, 0, 1) * 8.0f + 0.5f) / 8.0f;
    if (k > 0 && mi->wire[0] && !D.from_set) ident(fb, UI_CX, 372, mi->wire, ui_dim(C_SEC, k), UI_CENTER);
}

static void hero(gfx_fb *fb, float y)
{
    float bp = m_clamp((nos_t - D.t0) / UI_ENTRY_S, 0, 1);
    float pitch = hero_pitch(D.hero.to);
    dot_style hs = { DOT_FONT_TALL, pitch, pitch * 0.8f, C_WHITE, 0, 1, 1 };
    float yy = y + (UI_HEROW_PITCH - pitch) * 9.0f * 0.5f - (pitch > UI_HEROW_PITCH ? 4 : 0);
    if (bp < 1) dot_text_anim(fb, &hs, UI_CX, yy, D.hero.to, 1, bp, DOT_REVEAL_RANDOM, 13);
    else mmt_draw(fb, &D.hero, &hs, UI_CX, yy, MORPH_S);
}
static void unit_line(gfx_fb *fb, int y)
{
    if (D.ustate) ui_state(fb, UI_CX, y, D.unit, C_SEC);
    else if (D.unit_u[0]) lab_u(fb, UI_CX, y, D.unit, D.unit_u, C_SEC, UI_CENTER);
    else if (D.unit[0]) ui_label(fb, UI_CX, y, D.unit, C_SEC, UI_CENTER);
}
static void lines(gfx_fb *fb, float y)
{
    for (int i = 0; i < 3; i++)
        if (D.line[i][0]) lab_u(fb, UI_CX, (int)(y + i * 26), D.line[i], D.lu[i], i ? C_DIS : C_SEC, UI_CENTER);
}

static void play_row(gfx_fb *fb, int i, float y, int pressed)
{
    float h = PLAY_ROW, cy = y + h * 0.5f;
    float k = qfade(cy);
    int lx, rx;
    row_cols(cy, &lx, &rx);
    if (rx - lx < 140) return;
    if (pressed) ui_rrect(fb, lx - 20, y + 5, rx - lx + 40, h - 10, (h - 10) * 0.5f, C_SURF2);
    const char *name = D.m == M_HAPTIC ? k_hap_names[i] : k_sfx_names[i];
    ui_text(fb, UI_F_BODY, lx, cap_base(UI_F_BODY, cy), name, ui_dim(C_PRI, k), UI_LEFT, 0);
    float since = nos_t - D.play_t;
    int looping = D.m == M_HAPTIC && D.alarm_on && i == HAPTIC_ALARM;
    int playing = looping || (i == D.last_play && since < 0.6f);
    float r = 22;
    gfx_fill_circle(fb, rx - r, cy, r, ui_dim(playing ? C_WHITE : C_SURF2, k));
    if (looping) {                               /* stop: a 3 x 3 dot square */
        for (int yy = -1; yy <= 1; yy++)
            for (int xx = -1; xx <= 1; xx++) dot_disc(fb, rx - r + xx * 4.4f, cy + yy * 4.4f, 3.6f, ui_dim(C_BG, k));
    } else dot_icon(fb, DOT_ICON_PLAY, rx - r + 1, cy, 2.2f, 1.8f, ui_dim(playing ? C_BG : C_PRI, k));
}

static void draw_bubble(gfx_fb *fb, float cx, float cy)
{
    const float R = 54;
    ui_circle_line(fb, cx, cy, R, 2, C_LINE2);
    dot_disc(fb, cx, cy, 4, C_DIS);
    float bx = m_clamp(V.imu.ax, -1, 1) * R, by = m_clamp(V.imu.ay, -1, 1) * R;
    float d = sqrtf(bx * bx + by * by);
    if (d > R - 10) { bx *= (R - 10) / d; by *= (R - 10) / d; }
    /* half-pixel steps keep the bubble smooth without endless sub-pixel repaints */
    bx = floorf(bx * 2 + 0.5f) * 0.5f; by = floorf(by * 2 + 0.5f) * 0.5f;
    dot_disc(fb, cx + bx, cy + by, 18, C_WHITE);
}

static void detail_draw(gfx_fb *fb)
{
    const mod_info *mi = &MI[D.m];
    if (is_list_page()) {
        float off = floorf(ui_list_offset(&D.L) + 0.5f);
        ui_title(fb, mi->title, off);
        int n = D.L.n;
        int pr = D.flash_row >= 0 && nos_t - D.flash_rt < PRESS_FLASH ? D.flash_row :
                 (D.L.pressed >= 0 && nos_t - D.L.press_t > PRESS_HOLD && nos_touch_travel() <= PRESS_TRAVEL ? D.L.pressed : -1);
        for (int i = 0; i < n; i++) {
            float y = PLAY_TOP + i * PLAY_ROW - off;
            if (y + PLAY_ROW < 0) continue;
            if (y > GFX_H) break;
            play_row(fb, i, y, i == pr);
        }
        ui_scrollbar(fb, D.L.sc.x, (float)(PLAY_TOP + n * PLAY_ROW + D.L.bottom_pad), GFX_H, D.L.bar_alpha);
        return;
    }
    ui_title(fb, detail_title(), 0);
    float bp = m_clamp((nos_t - D.t0) / UI_ENTRY_S, 0, 1);
    if (!mod_present(D.m)) { absent_page(fb); return; }
    if (D.m == M_SD && !V.sd.mounted) {
        ui_absent(fb, DOT_ICON_SD, "No card", mi->body, D.from_set ? NULL : "SDMMC 1-BIT", bp);
        if (V.sd.message[0] && !D.from_set) {
            char b[44]; snprintf(b, sizeof b, "%.40s", V.sd.message);
            ident(fb, UI_CX, 372, b, C_SEC, UI_CENTER);
        }
        return;
    }
    switch (D.m) {
    /* every hero puts its label baseline 100 px under the hero top: caps 20 px clear */
    case M_IMU:
        hero(fb, 100);
        unit_line(fb, 200);
        draw_bubble(fb, UI_CX, 274);
        lines(fb, 352);
        break;
    case M_RTC:
        hero(fb, 112);
        unit_line(fb, 212);
        lines(fb, 252);
        break;
    case M_SD: {
        hero(fb, 112);
        unit_line(fb, 212);
        float used = V.sd.total_bytes ? (float)(1.0 - (double)V.sd.free_bytes / (double)V.sd.total_bytes) : 0;
        ui_segments(fb, 93, 242, 280, 26, 20, used * 20.0f * bp, C_WHITE, C_LINE);
        lines(fb, 312);
        break;
    }
    case M_BATT: {
        hero(fb, 112);
        unit_line(fb, 212);
        int pct = V.pw.battery_percent ? V.pw.battery_percent : hal_battery_percent();
        ui_segments(fb, 113, 242, 240, 26, 10, pct / 10.0f * bp, C_WHITE, C_LINE);
        lines(fb, 312);
        break;
    }
    case M_MIC:
        hero(fb, 112);
        unit_line(fb, 212);
        dot_columns(fb, UI_CX - 16 * 8.0f, 236, 32, 8, 8.0f, 5.6f, D.wave, C_WHITE, C_GHOST);
        lines(fb, 338);
        break;
    case M_GPS: {
        hero(fb, 112);
        unit_line(fb, 212);
        uint8_t v[16];
        int n = V.gps.nsnr < 16 ? V.gps.nsnr : 16;
        for (int i = 0; i < 16; i++) v[i] = i < n ? (uint8_t)m_clamp(V.gps.snr[i] * 255.0f / 50.0f, 0, 255) : 0;
        dot_columns(fb, UI_CX - 8 * 12.0f, 234, 16, 6, 12.0f, 8.0f, v, C_WHITE, C_GHOST);
        lines(fb, 340);
        break;
    }
    case M_LTE: {
        hero(fb, 100);
        unit_line(fb, 200);
        int reg = V.lte_ok && V.lte.powered && (V.lte.registered == 1 || V.lte.registered == 2) && V.lte.rssi_dbm;
        dot_bar(fb, UI_CX - 36, 226, 4, 24, 12, reg ? (float)V.lte.bars : 0.0f, C_WHITE, C_LINE2);
        lines(fb, 262);
        break;
    }
    case M_ENV:
        hero(fb, 128);
        unit_line(fb, 228);
        lines(fb, 280);
        break;
    case M_LIGHT: {
        hero(fb, 120);
        unit_line(fb, 220);
        float lit = V.light_ok ? V.light.prox * 16.0f / 255.0f : 0;
        dot_bar(fb, UI_CX - 7.5f * 16.0f, 258, 16, 16, 9, floorf(lit + 0.5f), C_WHITE, C_LINE2);
        lines(fb, 302);
        break;
    }
    case M_MAG: {
        /* dial: 60 dim dots on the bezel, the north dot travels around it */
        dot_ring(fb, UI_CX, UI_CY, 206, 60, 4, 0, 0, C_WHITE, C_LINE2);
        if (V.mag_ok) {
            float a = -V.mag.heading_deg * 0.0174533f;
            float nx = UI_CX + sinf(a) * 206, ny = UI_CY - cosf(a) * 206;
            nx = floorf(nx * 2 + 0.5f) * 0.5f; ny = floorf(ny * 2 + 0.5f) * 0.5f;
            dot_disc(fb, nx, ny, 14, C_RED);
        }
        hero(fb, 96);
        unit_line(fb, 196);
        if (D.calibrating) ui_segments(fb, 133, 214, 200, 16, 10, (V.mag_ok ? V.mag.cal_progress : 0) * 10.0f, C_WHITE, C_LINE);
        lines(fb, D.calibrating ? 262 : 244);
        break;
    }
    case M_HR: {
        hero(fb, 104);
        unit_line(fb, 204);
        if (V.hr_ok && V.hr.on_wrist) {
            const float x0 = UI_CX - 23.5f * 6.0f;
            for (int i = 0; i < 48; i++) {
                float v = V.hr.wave[i] / 255.0f;
                float y = 300 - v * 64;
                y = floorf(y * 2 + 0.5f) * 0.5f;
                dot_disc(fb, x0 + i * 6.0f, y, i == 47 ? 8.0f : 4.0f, i == 47 ? C_RED : ui_mix(C_LINE2, C_WHITE, (i + 8) / 56.0f));
            }
            ui_segments(fb, 163, 336, 140, 10, 5, V.hr.quality * 5.0f, C_WHITE, C_LINE);
            lines(fb, 380);
        } else {
            ui_state(fb, UI_CX, 276, V.hr_ok ? "PLACE ON WRIST" : "STARTING", C_SEC);
            ui_label(fb, UI_CX, 316, "WEAR IT SNUG, SENSOR ON SKIN", C_DIS, UI_CENTER);
        }
        break;
    }
    default: break;
    }
    if (detail_has_button()) {
        int kind = BTN_SECONDARY;
        if (D.m == M_MAG && D.calibrating) kind = BTN_PRIMARY;
        ui_button(fb, UI_CX, UI_BTN_Y, UI_BTN_W, UI_BTN_H, detail_button(), kind,
                  D.pressed || nos_t - D.flash_t < PRESS_FLASH);
    }
}

static int detail_btn_hit(int x, int y)
{
    return fabsf(y - (float)UI_BTN_Y) <= UI_BTN_H * 0.5f + 4 && fabsf(x - (float)UI_CX) <= UI_BTN_W * 0.5f + 10;
}

static void play_tap(int i)
{
    D.last_play = i; D.play_t = nos_t;
    if (D.m == M_HAPTIC) hal_haptic(i);
    else hal_sfx(i);
    if (D.m == M_HAPTIC) D.alarm_on = i == HAPTIC_ALARM;   /* its row shows stop until tapped */
}

static int detail_input(const nos_input *in)
{
    if (is_list_page()) {
        if (in->type == IN_DOWN) D.was_moving = D.L.sc.moving && fabsf(D.L.sc.v) > 60.0f;
        if (in->type == IN_TAP) {
            nos_input up = *in; up.type = IN_UP;
            ui_list_input(&D.L, &up);
            if (!D.was_moving) {
                float fy = in->y + floorf(ui_list_offset(&D.L) + 0.5f) - PLAY_TOP;
                int i = fy >= 0 ? (int)(fy / PLAY_ROW) : -1;
                if (i >= 0 && i < D.L.n) {
                    D.flash_row = i; D.flash_rt = nos_t;
                    if (D.m == M_HAPTIC && D.alarm_on) {   /* any tap stops the alarm train */
                        hal_haptic_stop();
                        D.alarm_on = 0;
                        D.last_play = -1;
                    } else play_tap(i);
                }
            }
            return 1;
        }
        return ui_list_input(&D.L, in);
    }
    switch (in->type) {
    case IN_DOWN: D.pressed = detail_has_button() && detail_btn_hit(in->x, in->y); return 1;
    case IN_DRAG_START: D.pressed = 0; return 0;
    case IN_TAP:
        if (D.pressed) {
            D.pressed = 0;
            D.flash_t = nos_t;
            nos_feedback(FB_TAP);
            if (D.m == M_RTC) { hal_time_resync(); nos_toast("RESYNCING"); }
            else if (D.m == M_LTE) {
                int on = !(V.lte_ok && V.lte.powered);
                hal_lte_enable(on);
                D.lte_req = on ? 1 : -1; D.lte_req_t = nos_t;
                detail_values(0);
            } else if (D.m == M_MAG) {
                if (!D.calibrating) { hal_mag_calibrate(1); D.calibrating = 1; }
                else { hal_mag_calibrate(0); D.calibrating = 0; nos_feedback(FB_SUCCESS); }
                detail_values(0);
            }
        }
        return 1;
    case IN_UP: D.pressed = 0; return 1;
    default: return 0;
    }
}
static const nos_screen scr_mod_detail = { "MOD_DETAIL", detail_enter, detail_leave, detail_tick,
                                           detail_draw, detail_input, 0 };

/* ======================================================================= touch test
 * Five targets (centre and the four edges, where the controller's axes are easiest to
 * get wrong) turn white when touched; a dot follows the finger with its coordinates. */
static const int16_t k_tgt[5][2] = { { 233, 233 }, { 233, 52 }, { 414, 233 }, { 233, 414 }, { 52, 233 } };

static void touch_tick(float dt) { (void)dt; }
static void touch_draw(gfx_fb *fb)
{
    /* the top ring owns the title band, so the title sits under it */
    ui_label(fb, UI_CX, 120, "TOUCH", C_SEC, UI_CENTER);
    float bp = m_clamp((nos_t - D.t0) / UI_ENTRY_S, 0, 1);
    for (int i = 0; i < 5; i++) {
        float p = m_clamp(bp * 5 - i, 0, 1);
        if (p <= 0) continue;
        float r = 26 * p;
        if (D.tgt_hit[i]) gfx_fill_circle(fb, k_tgt[i][0], k_tgt[i][1], r, C_WHITE);
        else ui_circle_line(fb, k_tgt[i][0], k_tgt[i][1], r, 2, C_LINE2);
    }
    if (D.tgt_done) ui_state(fb, UI_CX, 310, "PASS", C_PRI);
    else ui_label(fb, UI_CX, 310, "TOUCH EVERY RING", C_SEC, UI_CENTER);
    if (D.fdown) {
        dot_disc(fb, D.fx, D.fy, 30, C_SEC);
        char b[16]; snprintf(b, sizeof b, "%d, %d", D.fx, D.fy);
        ui_label(fb, UI_CX, 176, b, C_PRI, UI_CENTER);
    } else if (V.touched) {
        char b[24]; snprintf(b, sizeof b, "LAST %d, %d", V.touch_x, V.touch_y);
        ui_label(fb, UI_CX, 176, b, C_DIS, UI_CENTER);
    }
}
static void touch_at(int x, int y)
{
    D.fx = x; D.fy = y; D.fdown = 1;
    V.touch_x = x; V.touch_y = y; V.touched = 1;
    for (int i = 0; i < 5; i++) {
        int dx = x - k_tgt[i][0], dy = y - k_tgt[i][1];
        if (!D.tgt_hit[i] && dx * dx + dy * dy < 40 * 40) {
            D.tgt_hit[i] = 1;
            int all = 1;
            for (int j = 0; j < 5; j++) all &= D.tgt_hit[j];
            if (all && !D.tgt_done) { D.tgt_done = 1; nos_feedback(FB_SUCCESS); }
            else nos_feedback(FB_TICK);
        }
    }
}
static int touch_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN: case IN_DRAG_START: case IN_DRAG: touch_at(in->x, in->y); return 1;
    case IN_UP: D.fdown = 0; return 1;
    case IN_TAP: case IN_DRAG_END: return 1;
    default: return 0;
    }
}
static const nos_screen scr_mod_touch = { "MOD_TOUCH", detail_enter, detail_leave, touch_tick,
                                          touch_draw, touch_input, SCR_HDRAG };
