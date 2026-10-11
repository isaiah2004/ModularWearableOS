/* set_modules.c — Modules: diagnostics dashboard for every board part and strap module,
 * and the hardware bring-up tool for the real watch.
 *
 * List: a summary (online count + one dot per module), then BOARD and STRAP sections.
 * Each row: status dot (white = present/ok, grey = absent), name, bus/part, live value.
 * Live values are sampled in tick at 4 Hz so the text is readable and damage settles.
 *
 * Tapping a row pushes a detail page: a live readout plus a test action
 *   touch: five edge targets   IMU: bubble level   RTC: resync   SD: capacity
 *   haptic: play each pattern  speaker: play each SFX   mic: live dB + envelope
 *   GPS: satellites + SNR      LTE: power on/off   env: T/RH/P   light: lux, prox, gestures
 *   compass: dial + calibrate  HR: live PPG trace
 * Every page has a designed "not connected" state naming the expected bus and pins. */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { M_TOUCH = 0, M_IMU, M_RTC, M_SD, M_BATT, M_WIFI, M_BLE,
       M_HAPTIC, M_MIC, M_SPK, M_GPS, M_LTE, M_ENV, M_LIGHT, M_MAG, M_HR, M_COUNT };
#define M_BOARD_N 7

typedef struct {
    const char *name, *bus, *title;
    int icon;
    uint32_t ext;          /* EXT_* bit for strap modules, 0 = on the board */
    const char *adds;      /* what it adds (absent state) */
    const char *wire;      /* where it is expected (absent state) */
} mod_info;

static const mod_info MI[M_COUNT] = {
    { "Touch", "I2C 0x38  FT3168", "TOUCH", DOT_ICON_HAND, 0, "", "" },
    { "Motion", "I2C 0x6B  QMI8658", "MOTION", DOT_ICON_LEVEL, 0, "Raise to wake, steps and tilt",
      "I2C 0x6B, SDA 47 SCL 48" },
    { "Clock", "I2C 0x51  PCF85063", "CLOCK", DOT_ICON_TIMER, 0, "Keeps time while the watch sleeps",
      "I2C 0x51, SDA 47 SCL 48" },
    { "SD card", "SDMMC 1-bit", "SD CARD", DOT_ICON_SD, 0, "Voice memos and logs",
      "SDMMC CLK 41, CMD 39, D0 40" },
    { "Battery", "ADC GPIO4  3:1", "BATTERY", DOT_ICON_BATTERY, 0, "Charge estimate from the rail",
      "GPIO4, ADC1 channel 3" },
    { "Wi-Fi", "ESP32-S3  2.4 GHz", "WI-FI", DOT_ICON_WIFI, 0, "", "" },
    { "Bluetooth", "ESP32-S3  NUS", "BLUETOOTH", DOT_ICON_BT, 0, "", "" },
    { "Haptics", "GPIO16  PWM", "HAPTICS", DOT_ICON_BELL, EXT_HAPTIC, "A felt click on every detent",
      "Motor driver on GPIO16" },
    { "Microphone", "I2S IN  INMP441", "MICROPHONE", DOT_ICON_MIC, EXT_MIC, "Voice memos and the sound meter",
      "I2S BCLK 1, WS 2, SD 3" },
    { "Speaker", "I2S OUT  MAX98357A", "SPEAKER", DOT_ICON_SPEAKER, EXT_SPEAKER, "UI sounds, alarms and memo playback",
      "I2S BCLK 1, LRC 2, DIN 5, SD 6" },
    { "GPS", "UART1  M10", "GPS", DOT_ICON_SAT, EXT_GPS, "Position for navigation and routes",
      "UART1 RX 7, TX 17" },
    { "Cellular", "UART0  EG800AK", "CELLULAR", DOT_ICON_SIGNAL, EXT_LTE, "Calls and SMS without the phone",
      "UART0 RX 44, TX 43, PWRKEY 18" },
    { "Environment", "I2C 0x76  BME280", "ENVIRONMENT", DOT_ICON_THERMO, EXT_ENV, "Temperature, humidity and altitude",
      "I2C 0x76 / 0x77, SDA 47 SCL 48" },
    { "Light", "I2C 0x39  APDS-9960", "LIGHT", DOT_ICON_SUN, EXT_LIGHT, "Auto brightness and air gestures",
      "I2C 0x39, SDA 47 SCL 48" },
    { "Compass", "I2C 0x0D  QMC5883L", "COMPASS", DOT_ICON_COMPASS, EXT_MAG, "Heading for the compass and maps",
      "I2C 0x0D / 0x1E, SDA 47 SCL 48" },
    { "Heart rate", "I2C 0x57  MAX30102", "HEART RATE", DOT_ICON_HEART, EXT_HR, "Heart rate and blood oxygen",
      "I2C 0x57, SDA 47 SCL 48" },
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
static int label_w(const char *s)
{
    char b[32]; int i = 0;
    for (; s[i] && i < 31; i++) b[i] = (char)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]);
    b[i] = 0;
    return ui_text_width(UI_F_LABEL, b, 2);
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

/* ui_list_input releases the scroller again on the IN_UP that follows IN_DRAG_END,
 * with zero velocity, which kills every fling. Forward IN_UP only while the finger
 * still holds the list (a press that never dragged). */
/* ui_list_tick quantises the fading scrollbar to 1/16 with round-to-nearest, so the
 * exponential fade-out stalls at 3/16 forever. Finish the fade linearly. */
static void bar_fix(ui_list *l, float dt)
{
    if (nos_t - l->bar_t < 0.6f || l->bar_alpha <= 0) return;
    float a = l->bar_alpha - dt * 3.0f;
    l->bar_alpha = a <= 0.03f ? 0 : floorf(a * 16.0f) / 16.0f;
}

static int list_in(ui_list *l, const nos_input *in)
{
    if (in->type == IN_UP && !l->sc.dragging) { l->pressed = -1; return 1; }
    int moving = l->sc.moving && fabsf(l->sc.v) > 5.0f;
    int r = ui_list_input(l, in);
    /* a press on a resting list need not hold the scroller: holding it keeps the
     * scrollbar lit, so every tap would flash it */
    if (in->type == IN_DOWN && !moving && !l->sc.springing) l->sc.dragging = 0;
    return r;
}

/* ======================================================================= sampling
 * One snapshot of everything, refreshed at 4 Hz (list) or every frame (detail visuals). */
static struct {
    int ok[M_COUNT];
    char val[M_COUNT][24];
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

static void list_values(void)
{
    char *v;
    for (int m = 0; m < M_COUNT; m++) V.ok[m] = mod_present(m);
    v = V.val[M_TOUCH];
    if (V.touched) snprintf(v, 24, "%d, %d", V.touch_x, V.touch_y); else snprintf(v, 24, "Ready");
    V.ok[M_IMU] = V.imu_ok;
    if (V.imu_ok) {
        float g = sqrtf(V.imu.ax * V.imu.ax + V.imu.ay * V.imu.ay + V.imu.az * V.imu.az);
        snprintf(V.val[M_IMU], 24, "%.2f g", g);
    } else snprintf(V.val[M_IMU], 24, "No reply");
    ui_fmt_time(V.val[M_RTC], 24, V.dt.hour, V.dt.minute);
    V.ok[M_SD] = V.sd.mounted;
    if (V.sd.mounted) snprintf(V.val[M_SD], 24, "%.0f GB free", V.sd.free_bytes / (1024.0 * 1024.0 * 1024.0));
    else snprintf(V.val[M_SD], 24, "No card");
    int mv = V.pw.rail_mv > 0 ? V.pw.rail_mv : hal_battery_mv();
    V.ok[M_BATT] = mv > 0;
    if (mv > 0) snprintf(V.val[M_BATT], 24, "%d mV", mv); else snprintf(V.val[M_BATT], 24, "No reading");
    hal_conn w = hal_status(HAL_FEAT_WIFI);
    V.ok[M_WIFI] = w == HAL_CONN_UP;
    snprintf(V.val[M_WIFI], 24, "%s", w == HAL_CONN_UP ? "Online" : w == HAL_CONN_TRYING ? "Joining" :
             w == HAL_CONN_DOWN ? "No network" : "Off");
    int phone = hal_phone_connected() || svc_sys()->phone_connected;
    hal_conn b = hal_status(HAL_FEAT_BLE);
    V.ok[M_BLE] = b != HAL_CONN_OFF;
    snprintf(V.val[M_BLE], 24, "%s", phone ? "Phone linked" : b == HAL_CONN_OFF ? "Off" : "Advertising");
    if (V.ok[M_HAPTIC]) snprintf(V.val[M_HAPTIC], 24, nos_prefs.haptics ? "%d%%" : "Muted", nos_prefs.haptic_strength);
    if (V.ok[M_MIC]) {
        if (V.mic_db >= 0) snprintf(V.val[M_MIC], 24, "%.0f dB", V.mic_db); else snprintf(V.val[M_MIC], 24, "Ready");
    }
    if (V.ok[M_SPK]) snprintf(V.val[M_SPK], 24, "Vol %d%%", nos_prefs.volume);
    if (V.ok[M_GPS]) {
        if (V.gps.valid) snprintf(V.val[M_GPS], 24, "%d sats", V.gps.sats_used);
        else if (V.gps.sats_view) snprintf(V.val[M_GPS], 24, "%d in view", V.gps.sats_view);
        else snprintf(V.val[M_GPS], 24, "Searching");
    }
    if (V.ok[M_LTE]) {
        if (!V.lte.powered) snprintf(V.val[M_LTE], 24, "Off");
        else if (!V.lte.sim) snprintf(V.val[M_LTE], 24, "No SIM");
        else if (V.lte.registered == 1 || V.lte.registered == 2) snprintf(V.val[M_LTE], 24, "%d dBm", V.lte.rssi_dbm);
        else snprintf(V.val[M_LTE], 24, "Searching");
    }
    if (V.ok[M_ENV]) {
        if (V.env_ok) snprintf(V.val[M_ENV], 24, "%.1fC  %.0f%%", V.env.temp_c, V.env.humidity);
        else snprintf(V.val[M_ENV], 24, "Warming up");
    }
    if (V.ok[M_LIGHT]) {
        if (V.light_ok) snprintf(V.val[M_LIGHT], 24, "%.0f lux", V.light.lux); else snprintf(V.val[M_LIGHT], 24, "Ready");
    }
    if (V.ok[M_MAG]) {
        if (V.mag_ok) snprintf(V.val[M_MAG], 24, "HDG %03.0f", fmodf(V.mag.heading_deg + 360.0f, 360.0f));
        else snprintf(V.val[M_MAG], 24, "Ready");
    }
    if (V.ok[M_HR]) snprintf(V.val[M_HR], 24, "Ready");
    for (int m = M_HAPTIC; m < M_COUNT; m++) if (!V.ok[m]) snprintf(V.val[m], 24, "Absent");
}

/* ======================================================================= list */
enum { LK_SUM = 0, LK_HDR, LK_ROW };
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
};
#define NLE ((int)(sizeof LE / sizeof LE[0]))
#define MU 30
#define MTOP 96
static int lunits(int k) { return k == LK_SUM ? 6 : k == LK_HDR ? 2 : 3; }

static struct {
    ui_list L;
    int start[NLE];
    int8_t unit_e[NLE * 6];
    int nunits;
    int inited, covered, was_moving;
    float t0, next;
    mmt sum;
    int online;
} ML;

static void mods_count(void)
{
    int n = 0;
    for (int m = 0; m < M_COUNT; m++) n += V.ok[m] != 0;
    ML.online = n;
}

static void modules_enter(void)
{
    int revealed = ML.inited && ML.covered && nos_transitioning();
    ML.covered = 0;
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
        ML.L.bottom_pad = GFX_H / 2 - 50;
        ui_list_set_n(&ML.L, u);
        ML.t0 = nos_t;
        ML.inited = 1;
    }
    hal_gps_enable(1);              /* satellites in the list; auto-stops when we leave */
    V.sd_t = 0;
    sample(0, 1);
    list_values();
    mods_count();
    char b[12]; snprintf(b, sizeof b, "%d/%d", ML.online, M_COUNT);
    mmt_set(&ML.sum, b, !revealed);
    ML.next = nos_t + 0.25f;
}
static void modules_leave(void) { ML.covered = 1; hal_gps_enable(0); }

static void modules_tick(float dt)
{
    ui_list_tick(&ML.L, dt);
    bar_fix(&ML.L, dt);
    if (nos_t >= ML.next) {
        sample(0, 1);
        list_values();
        mods_count();
        char b[12]; snprintf(b, sizeof b, "%d/%d", ML.online, M_COUNT);
        mmt_set(&ML.sum, b, 0);
        ML.next = nos_t + 0.25f;
    }
}

static float mlist_off(void) { return floorf(ui_list_offset(&ML.L) + 0.5f); }

static float mbuild(float y, float *dy)
{
    int k = (int)((y - 60.0f) / 80.0f);
    if (k < 0) k = 0;
    if (k > 6) k = 6;
    float p = m_clamp((nos_t - ML.t0 - k * 0.034f) / 0.20f, 0, 1);
    float e = floorf(bezier_ease(&EASE_STANDARD, p) * 8.0f + 0.5f) / 8.0f;
    *dy = (1.0f - e) * 14.0f;
    return e;
}

static void draw_summary(gfx_fb *fb, float y, float k)
{
    float bp = m_clamp((nos_t - ML.t0) / 0.32f, 0, 1);
    dot_style hs = { DOT_FONT_TALL, 7.0f, 5.6f, ui_dim(C_WHITE, k), 0, 1, 1 };
    if (bp < 1) dot_text_anim(fb, &hs, UI_CX, y + 6, ML.sum.to, 1, bp, DOT_REVEAL_RANDOM, 21);
    else mmt_draw(fb, &ML.sum, &hs, UI_CX, y + 6, 0.2f);
    ui_label(fb, UI_CX, (int)(y + 104), "ONLINE", ui_dim(C_SEC, k), UI_CENTER);
    /* one dot per module, in list order: the whole bus at a glance */
    const float pitch = 16.0f;
    float x0 = UI_CX - (M_COUNT - 1) * pitch * 0.5f;
    for (int m = 0; m < M_COUNT; m++) {
        float p = m_clamp((nos_t - ML.t0 - 0.08f - m * 0.012f) / 0.12f, 0, 1);
        p = floorf(p * 4.0f + 0.5f) / 4.0f;
        if (p <= 0) continue;
        float gx = m >= M_BOARD_N ? 8.0f : 0.0f;     /* gap between board and strap */
        dot_disc(fb, x0 + m * pitch + gx - 4.0f, y + 140, 8.0f * p, ui_dim(V.ok[m] ? C_WHITE : C_LINE2, k));
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
    float edge = cy;
    int x0, x1;
    ui_row_span(edge, 30, &x0, &x1);
    if (x1 - x0 < 160) return;
    int lx = x0 > 62 ? x0 : 62, rx = x1 < 404 ? x1 : 404;
    if (le->kind == LK_HDR) { ui_label(fb, lx, (int)(y + h - 16), le->label, ui_dim(C_DIS, k), UI_LEFT); return; }
    int m = le->mod;
    if (pressed) ui_rrect(fb, lx - 20, y + 5, rx - lx + 40, h - 10, (h - 10) * 0.5f, C_SURF2);
    int ok = V.ok[m];
    dot_disc(fb, lx + 5, cy - 11, 10, ui_dim(ok ? C_WHITE : C_DIS, k));
    int tx = lx + 24;
    ui_text(fb, UI_F_BODY, tx, cap_base(UI_F_BODY, cy - 11), MI[m].name, ui_dim(ok ? C_PRI : C_SEC, k), UI_LEFT, 0);
    ui_text(fb, UI_F_SMALL, tx, cap_base(UI_F_SMALL, cy + 17), MI[m].bus, ui_dim(C_DIS, k), UI_LEFT, 0);
    int nend = tx + ui_text_width(UI_F_BODY, MI[m].name, 0) + 14;
    if (rx - label_w(V.val[m]) > nend)
        ui_label(fb, rx, cap_base(UI_F_LABEL, cy - 11), V.val[m], ui_dim(ok ? C_SEC : C_DIS, k), UI_RIGHT);
}

static void modules_draw(gfx_fb *fb)
{
    float off = mlist_off();
    ui_title(fb, "MODULES", off);
    int pe = ML.L.pressed >= 0 && ML.L.pressed < ML.nunits && nos_t - ML.L.press_t > 0.05f ?
             ML.unit_e[ML.L.pressed] : -1;
    for (int e = 0; e < NLE; e++) {
        float h = (float)(lunits(LE[e].kind) * MU);
        float y = MTOP + ML.start[e] * MU - off;
        if (y + h < 0) continue;
        if (y > GFX_H) break;
        draw_lrow(fb, e, y, h, e == pe && LE[e].kind == LK_ROW);
    }
    float content = (float)(MTOP + ML.nunits * MU + ML.L.bottom_pad);
    ui_scrollbar(fb, ML.L.sc.x, content, GFX_H, ML.L.bar_alpha);
}

static const nos_screen scr_mod_detail, scr_mod_touch;
static void open_detail(int m);

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
            if (u >= 0 && u < ML.nunits && LE[ML.unit_e[u]].kind == LK_ROW) {
                nos_feedback(FB_TAP);
                open_detail(LE[ML.unit_e[u]].mod);
            }
        }
        return 1;
    }
    return list_in(&ML.L, in);
}

const nos_screen scr_modules = { "MODULES", modules_enter, modules_leave, modules_tick,
                                 modules_draw, modules_input, 0 };

/* ======================================================================= detail */
static struct {
    int m;
    float t0, next;
    int pressed;                 /* action button held */
    mmt hero;
    char unit[32];
    char line[3][44];
    /* haptic / speaker play lists */
    ui_list L;
    int was_moving;
    int last_play; float play_t;
    /* touch test */
    int tgt_hit[5]; int tgt_done;
    int fx, fy, fdown;
    /* compass */
    int calibrating;
    /* LTE */
    int lte_req; float lte_req_t;
    /* light */
    int gest; float gest_t;
    /* mic */
    float peak;
    uint8_t wave[32];
} D;

static void open_detail(int m)
{
    D.m = m;
    nos_push(m == M_TOUCH ? &scr_mod_touch : &scr_mod_detail, TX_PUSH);
}

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
#define PLAY_ROW 88

static void detail_values(int init)
{
    char h[24];
    h[0] = 0;
    D.unit[0] = 0;
    for (int i = 0; i < 3; i++) D.line[i][0] = 0;
    switch (D.m) {
    case M_IMU: {
        float g = sqrtf(V.imu.ax * V.imu.ax + V.imu.ay * V.imu.ay + V.imu.az * V.imu.az);
        float tilt = g > 0.05f ? acosf(m_clamp(fabsf(V.imu.az) / g, 0, 1)) * 57.29578f : 0;
        snprintf(h, sizeof h, "%.0f\xC2\xB0", tilt);
        snprintf(D.unit, sizeof D.unit, "TILT");
        snprintf(D.line[0], 44, "X %+.2f  Y %+.2f  Z %+.2f G", V.imu.ax, V.imu.ay, V.imu.az);
        snprintf(D.line[1], 44, "GYRO %.0f %.0f %.0f DPS", V.imu.gx, V.imu.gy, V.imu.gz);
        break;
    }
    case M_RTC:
        snprintf(h, sizeof h, "%02d:%02d:%02d", V.dt.hour, V.dt.minute, V.dt.second);
        snprintf(D.unit, sizeof D.unit, "%s %d %s %d", ui_weekday3(V.dt.weekday), V.dt.day, ui_month3(V.dt.month), V.dt.year);
        snprintf(D.line[0], 44, "%s", hal_epoch_seconds() ? "UTC SYNCED" : "UTC NOT SYNCED");
        break;
    case M_SD:
        if (V.sd.mounted) {
            snprintf(h, sizeof h, "%.1f", V.sd.free_bytes / (1024.0 * 1024.0 * 1024.0));
            snprintf(D.unit, sizeof D.unit, "GB FREE OF %.0f", V.sd.total_bytes / (1024.0 * 1024.0 * 1024.0));
        }
        snprintf(D.line[0], 44, "%.40s", V.sd.message[0] ? V.sd.message : "SDMMC 1-bit, 20 MHz");
        break;
    case M_BATT: {
        int mv = V.pw.rail_mv > 0 ? V.pw.rail_mv : hal_battery_mv();
        snprintf(h, sizeof h, "%d", mv);
        snprintf(D.unit, sizeof D.unit, "MV ON THE RAIL");
        int pct = V.pw.battery_percent ? V.pw.battery_percent : hal_battery_percent();
        if (V.pw.external_power == 1) snprintf(D.line[0], 44, "USB POWER");
        else snprintf(D.line[0], 44, "%d%% ESTIMATE", pct);
        snprintf(D.line[1], 44, "GPIO4  ADC1 CH3  3:1");
        break;
    }
    case M_WIFI: {
        hal_conn w = hal_status(HAL_FEAT_WIFI);
        snprintf(D.unit, sizeof D.unit, "%s", w == HAL_CONN_UP ? "ONLINE" : w == HAL_CONN_TRYING ? "JOINING" :
                 w == HAL_CONN_DOWN ? "NO NETWORK" : "OFF");
        snprintf(D.line[0], 44, "2.4 GHZ  STATION");
        snprintf(D.line[1], 44, "%s", nos_prefs.wifi ? "ENABLED IN SETTINGS" : "DISABLED IN SETTINGS");
        break;
    }
    case M_BLE: {
        int phone = hal_phone_connected() || svc_sys()->phone_connected;
        snprintf(D.unit, sizeof D.unit, "%s", phone ? "PHONE LINKED" : "ADVERTISING AS NOS");
        char st[40]; hal_ble_status(st, sizeof st);
        snprintf(D.line[0], 44, "%.40s", st);
        snprintf(D.line[1], 44, "NORDIC UART  GADGETBRIDGE");
        break;
    }
    case M_MIC:
        if (V.mic_db >= 0) snprintf(h, sizeof h, "%.0f", V.mic_db); else snprintf(h, sizeof h, "--");
        snprintf(D.unit, sizeof D.unit, "DB SPL");
        snprintf(D.line[0], 44, "PEAK %.0f DB", D.peak);
        break;
    case M_GPS:
        snprintf(h, sizeof h, "%d", V.gps.valid ? V.gps.sats_used : 0);
        snprintf(D.unit, sizeof D.unit, "SATELLITES IN USE");
        if (V.gps.valid)
            snprintf(D.line[0], 44, "%.4f%c  %.4f%c", fabs(V.gps.lat), V.gps.lat >= 0 ? 'N' : 'S',
                     fabs(V.gps.lon), V.gps.lon >= 0 ? 'E' : 'W');
        else snprintf(D.line[0], 44, "%d IN VIEW, NO FIX YET", V.gps.sats_view);
        if (V.gps.valid) snprintf(D.line[1], 44, "%s  HDOP %.1f", V.gps.fix3d ? "3D FIX" : "2D FIX", V.gps.hdop);
        break;
    case M_LTE:
        if (V.lte_ok && V.lte.powered) {
            snprintf(h, sizeof h, "%d", V.lte.rssi_dbm);
            snprintf(D.unit, sizeof D.unit, "DBM  %d/4 BARS", V.lte.bars);
            snprintf(D.line[0], 44, "SIM %s", V.lte.sim == 1 ? "READY" : V.lte.sim == 2 ? "PIN LOCKED" : "MISSING");
            snprintf(D.line[1], 44, "%s %s", V.lte.registered == 1 ? "HOME" : V.lte.registered == 2 ? "ROAMING" :
                     V.lte.registered == 3 ? "SEARCHING" : "NOT REGISTERED", V.lte.op);
        } else {
            snprintf(h, sizeof h, "OFF");
            snprintf(D.unit, sizeof D.unit, "%s", D.lte_req ? "STARTING" : "MODEM POWERED DOWN");
            snprintf(D.line[0], 44, "PWRKEY ON IO18");
        }
        break;
    case M_ENV:
        if (V.env_ok) {
            float t = nos_prefs.units_metric ? V.env.temp_c : V.env.temp_c * 1.8f + 32.0f;
            snprintf(h, sizeof h, "%.1f\xC2\xB0", t);
            snprintf(D.unit, sizeof D.unit, "%s", nos_prefs.units_metric ? "CELSIUS" : "FAHRENHEIT");
            snprintf(D.line[0], 44, "HUMIDITY %.0f%%", V.env.humidity);
            snprintf(D.line[1], 44, "PRESSURE %.1f HPA", V.env.pressure_hpa);
            snprintf(D.line[2], 44, "ALTITUDE %.0f M", V.env.altitude_m);
        } else { snprintf(h, sizeof h, "--"); snprintf(D.unit, sizeof D.unit, "WAITING FOR A SAMPLE"); }
        break;
    case M_LIGHT:
        if (V.light_ok) snprintf(h, sizeof h, "%.0f", V.light.lux); else snprintf(h, sizeof h, "--");
        snprintf(D.unit, sizeof D.unit, "LUX");
        snprintf(D.line[0], 44, "PROXIMITY %d", V.light_ok ? V.light.prox : 0);
        {
            static const char *const g[] = { "NONE", "UP", "DOWN", "LEFT", "RIGHT", "NEAR", "FAR" };
            snprintf(D.line[1], 44, "GESTURE %s", (D.gest > 0 && D.gest <= 6 && nos_t - D.gest_t < 2.0f) ? g[D.gest] : "--");
        }
        break;
    case M_MAG:
        if (V.mag_ok) {
            float hd = fmodf(V.mag.heading_deg + 360.0f, 360.0f);
            snprintf(h, sizeof h, "%.0f\xC2\xB0", hd);
            snprintf(D.unit, sizeof D.unit, "%s", cardinal(hd));
            snprintf(D.line[0], 44, "FIELD %.0f UT", V.mag.field_ut);
            snprintf(D.line[1], 44, "%s", D.calibrating ? "MOVE IN A FIGURE 8" :
                     V.mag.calibrated ? "CALIBRATED" : "NOT CALIBRATED");
        } else { snprintf(h, sizeof h, "--"); snprintf(D.unit, sizeof D.unit, "WAITING FOR A SAMPLE"); }
        break;
    case M_HR:
        if (V.hr_ok && V.hr.bpm) snprintf(h, sizeof h, "%d", V.hr.bpm); else snprintf(h, sizeof h, "--");
        snprintf(D.unit, sizeof D.unit, "BPM");
        if (V.hr_ok && V.hr.spo2) snprintf(D.line[0], 44, "SPO2 %d%%", V.hr.spo2);
        else if (V.hr_ok && V.hr.on_wrist && !V.hr.bpm) snprintf(D.line[0], 44, "LOCKING ON");
        break;
    default: break;
    }
    mmt_set(&D.hero, h, init);
}

static void detail_enter(void)
{
    D.t0 = nos_t;
    D.pressed = 0;
    D.calibrating = 0;
    D.lte_req = 0;
    D.gest = 0;
    D.peak = 0;
    D.last_play = -1; D.play_t = -100;
    memset(D.tgt_hit, 0, sizeof D.tgt_hit); D.tgt_done = 0; D.fdown = 0;
    if (D.m == M_HR) hal_hr_enable(1);
    if (D.m == M_GPS) hal_gps_enable(1);
    if (D.m == M_LIGHT) hal_light_gesture_enable(1);
    int n = D.m == M_HAPTIC ? N_HAP : SFX_COUNT;
    ui_list_init(&D.L, n, PLAY_ROW, PLAY_TOP, 0, 0);
    D.L.bottom_pad = GFX_H / 2 - PLAY_ROW / 2;
    ui_list_set_n(&D.L, n);
    V.sd_t = 0;
    sample(D.m == M_HR, D.m == M_MIC);
    detail_values(1);
    D.next = nos_t + 0.25f;
}
static void detail_leave(void)
{
    if (D.m == M_HR) hal_hr_enable(0);
    if (D.m == M_GPS) hal_gps_enable(0);
    if (D.m == M_LIGHT) hal_light_gesture_enable(0);
    if (D.m == M_MAG && D.calibrating) { hal_mag_calibrate(0); D.calibrating = 0; }
}

static void detail_tick(float dt)
{
    if (is_list_page()) { ui_list_tick(&D.L, dt); bar_fix(&D.L, dt); }
    /* visuals every frame (bubble, dial, traces), text at 4 Hz */
    sample(D.m == M_HR, D.m == M_MIC);
    if (D.m == M_MIC) {
        if (V.mic_db > D.peak) D.peak = V.mic_db;
        if (!hal_mic_wave(D.wave)) memset(D.wave, 0, sizeof D.wave);
    }
    if (D.m == M_LIGHT) {
        int g = hal_light_gesture();
        if (g != GEST_NONE) { D.gest = g; D.gest_t = nos_t; nos_feedback(FB_TICK); }
    }
    if (D.m == M_MAG && D.calibrating && V.mag_ok && V.mag.cal_progress >= 1.0f) {
        hal_mag_calibrate(0);
        D.calibrating = 0;
        nos_feedback(FB_SUCCESS);
        nos_toast("Compass calibrated");
    }
    if (D.m == M_LTE && D.lte_req && ((V.lte_ok && V.lte.powered) == (D.lte_req > 0) || nos_t - D.lte_req_t > 20.0f))
        D.lte_req = 0;
    if (nos_t >= D.next) { detail_values(0); D.next = nos_t + 0.25f; }
}

/* ---- detail drawing ---- */
static void absent_state(gfx_fb *fb)
{
    const mod_info *mi = &MI[D.m];
    float bp = m_clamp((nos_t - D.t0) / 0.32f, 0, 1);
    dot_icon_anim(fb, mi->icon, UI_CX, 156, 4.2f, 3.4f, C_DIS, bp, DOT_REVEAL_CENTER);
    ui_text(fb, UI_F_TITLE, UI_CX, 244, "Not connected", C_PRI, UI_CENTER, 0);
    ui_text_wrap(fb, UI_F_BODY, UI_CX, 282, 300, 30, 2, mi->adds, C_SEC, UI_CENTER);
    ui_label(fb, UI_CX, 354, "EXPECTED ON", C_DIS, UI_CENTER);
    ui_text_fit(fb, UI_F_SMALL, UI_CX, 378, mi->wire, C_SEC, UI_CENTER, 300);
}

static void hero(gfx_fb *fb, float y, float pitch)
{
    float bp = m_clamp((nos_t - D.t0) / 0.32f, 0, 1);
    dot_style hs = { DOT_FONT_TALL, pitch, pitch * 0.8f, C_WHITE, 0, 1, 1 };
    if (bp < 1) dot_text_anim(fb, &hs, UI_CX, y, D.hero.to, 1, bp, DOT_REVEAL_RANDOM, 13);
    else mmt_draw(fb, &D.hero, &hs, UI_CX, y, 0.16f);
}

static void lines(gfx_fb *fb, float y)
{
    for (int i = 0; i < 3; i++)
        if (D.line[i][0]) ui_label(fb, UI_CX, (int)(y + i * 26), D.line[i], i ? C_DIS : C_SEC, UI_CENTER);
}

static void play_row(gfx_fb *fb, int i, float y, int pressed)
{
    float h = PLAY_ROW, cy = y + h * 0.5f;
    float k = qfade(cy);
    float edge = cy;
    int x0, x1;
    ui_row_span(edge, 30, &x0, &x1);
    if (x1 - x0 < 160) return;
    int lx = x0 > 70 ? x0 : 70, rx = x1 < 396 ? x1 : 396;
    if (pressed) ui_rrect(fb, lx - 20, y + 5, rx - lx + 40, h - 10, (h - 10) * 0.5f, C_SURF2);
    const char *name = D.m == M_HAPTIC ? k_hap_names[i] : k_sfx_names[i];
    ui_text(fb, UI_F_BODY, lx, cap_base(UI_F_BODY, cy), name, ui_dim(C_PRI, k), UI_LEFT, 0);
    float since = nos_t - D.play_t;
    int playing = i == D.last_play && since < 0.6f;
    float r = 22;
    gfx_fill_circle(fb, rx - r, cy, r, ui_dim(playing ? C_WHITE : C_SURF2, k));
    dot_icon(fb, DOT_ICON_PLAY, rx - r + 1, cy, 2.2f, 1.8f, ui_dim(playing ? C_BG : C_PRI, k));
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
        for (int i = 0; i < n; i++) {
            float y = PLAY_TOP + i * PLAY_ROW - off;
            if (y + PLAY_ROW < 0) continue;
            if (y > GFX_H) break;
            play_row(fb, i, y, i == D.L.pressed && nos_t - D.L.press_t > 0.05f);
        }
        ui_scrollbar(fb, D.L.sc.x, (float)(PLAY_TOP + n * PLAY_ROW + D.L.bottom_pad), GFX_H, D.L.bar_alpha);
        return;
    }
    ui_title(fb, mi->title, 0);
    if (!mod_present(D.m) || (D.m == M_SD && !V.sd.mounted)) {
        if (D.m == M_SD) {
            dot_icon(fb, DOT_ICON_SD, UI_CX, 156, 4.2f, 3.4f, C_DIS);
            ui_text(fb, UI_F_TITLE, UI_CX, 244, "No card", C_PRI, UI_CENTER, 0);
            ui_text_wrap(fb, UI_F_BODY, UI_CX, 282, 300, 30, 2, "Insert a microSD card for memos and logs", C_SEC, UI_CENTER);
            if (V.sd.message[0]) ui_text_fit(fb, UI_F_SMALL, UI_CX, 368, V.sd.message, C_DIS, UI_CENTER, 300);
        } else absent_state(fb);
        return;
    }
    float bp = m_clamp((nos_t - D.t0) / 0.32f, 0, 1);
    switch (D.m) {
    case M_WIFI: case M_BLE:
        dot_icon_anim(fb, mi->icon, UI_CX, 180, 5.0f, 4.0f, C_WHITE, bp, DOT_REVEAL_CENTER);
        ui_state(fb, UI_CX, 290, D.unit, C_PRI);
        lines(fb, 334);
        break;
    case M_IMU:
        hero(fb, 108, 6.0f);
        ui_label(fb, UI_CX, 190, D.unit, C_SEC, UI_CENTER);
        draw_bubble(fb, UI_CX, 268);
        lines(fb, 356);
        break;
    case M_RTC:
        hero(fb, 136, 6.0f);
        ui_label(fb, UI_CX, 220, D.unit, C_SEC, UI_CENTER);
        lines(fb, 262);
        break;
    case M_SD: {
        hero(fb, 128, 8.0f);
        ui_label(fb, UI_CX, 230, D.unit, C_SEC, UI_CENTER);
        float used = V.sd.total_bytes ? (float)(1.0 - (double)V.sd.free_bytes / (double)V.sd.total_bytes) : 0;
        ui_segments(fb, 93, 260, 280, 26, 20, used * 20.0f * bp, C_WHITE, C_LINE);
        lines(fb, 330);
        break;
    }
    case M_BATT: {
        hero(fb, 128, 8.0f);
        ui_label(fb, UI_CX, 230, D.unit, C_SEC, UI_CENTER);
        int pct = V.pw.battery_percent ? V.pw.battery_percent : hal_battery_percent();
        ui_segments(fb, 113, 260, 240, 26, 10, pct / 10.0f * bp, C_WHITE, C_LINE);
        lines(fb, 330);
        break;
    }
    case M_MIC: {
        hero(fb, 116, 8.0f);
        ui_label(fb, UI_CX, 216, D.unit, C_SEC, UI_CENTER);
        dot_columns(fb, UI_CX - 16 * 8.0f, 240, 32, 8, 8.0f, 5.6f, D.wave, C_WHITE, C_GHOST);
        lines(fb, 342);
        break;
    }
    case M_GPS: {
        hero(fb, 116, 8.0f);
        ui_label(fb, UI_CX, 216, D.unit, C_SEC, UI_CENTER);
        uint8_t v[16];
        int n = V.gps.nsnr < 16 ? V.gps.nsnr : 16;
        for (int i = 0; i < 16; i++) v[i] = i < n ? (uint8_t)m_clamp(V.gps.snr[i] * 255.0f / 50.0f, 0, 255) : 0;
        dot_columns(fb, UI_CX - 8 * 12.0f, 238, 16, 6, 12.0f, 8.0f, v, C_WHITE, C_GHOST);
        lines(fb, 342);
        break;
    }
    case M_LTE:
        hero(fb, 104, 8.0f);
        ui_label(fb, UI_CX, 204, D.unit, C_SEC, UI_CENTER);
        if (V.lte_ok && V.lte.powered) dot_bar(fb, UI_CX - 36, 236, 4, 24, 12, (float)V.lte.bars, C_WHITE, C_LINE2);
        lines(fb, 278);
        break;
    case M_ENV:
        hero(fb, 134, 8.0f);
        ui_label(fb, UI_CX, 234, D.unit, C_SEC, UI_CENTER);
        lines(fb, 286);
        break;
    case M_LIGHT: {
        hero(fb, 124, 8.0f);
        ui_label(fb, UI_CX, 224, D.unit, C_SEC, UI_CENTER);
        float lit = V.light_ok ? V.light.prox * 16.0f / 255.0f : 0;
        dot_bar(fb, UI_CX - 7.5f * 16.0f, 262, 16, 16, 9, floorf(lit + 0.5f), C_WHITE, C_LINE2);
        lines(fb, 306);
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
        hero(fb, 112, 8.0f);
        ui_label(fb, UI_CX, 212, D.unit, C_SEC, UI_CENTER);
        if (D.calibrating) ui_segments(fb, 133, 236, 200, 18, 10, (V.mag_ok ? V.mag.cal_progress : 0) * 10.0f, C_WHITE, C_LINE);
        lines(fb, D.calibrating ? 290 : 262);
        break;
    }
    case M_HR: {
        hero(fb, 104, 8.0f);
        ui_label(fb, UI_CX, 204, D.unit, C_SEC, UI_CENTER);
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
            ui_text_wrap(fb, UI_F_BODY, UI_CX, 320, 280, 30, 2, "Wear it snug, sensor against the skin", C_DIS, UI_CENTER);
        }
        break;
    }
    default: break;
    }
    if (detail_has_button()) {
        int kind = BTN_SECONDARY;
        if (D.m == M_MAG && D.calibrating) kind = BTN_PRIMARY;
        ui_button(fb, UI_CX, 388, 200, 64, detail_button(), kind, D.pressed);
    }
}

static int detail_btn_hit(int x, int y) { return y > 350 && y < 430 && x > UI_CX - 120 && x < UI_CX + 120; }

static void play_tap(int i)
{
    D.last_play = i; D.play_t = nos_t;
    if (D.m == M_HAPTIC) hal_haptic(i);
    else hal_sfx(i);
    if (D.m == M_HAPTIC && i == HAPTIC_ALARM) nos_toast("Tap again to stop");
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
                    if (D.m == M_HAPTIC && D.last_play == HAPTIC_ALARM && nos_t - D.play_t < 30.0f) {
                        hal_haptic_stop();
                        D.last_play = -1;
                    } else play_tap(i);
                }
            }
            return 1;
        }
        return list_in(&D.L, in);
    }
    switch (in->type) {
    case IN_DOWN: D.pressed = detail_has_button() && detail_btn_hit(in->x, in->y); return 1;
    case IN_DRAG_START: D.pressed = 0; return 0;
    case IN_TAP:
        if (D.pressed) {
            D.pressed = 0;
            nos_feedback(FB_TAP);
            if (D.m == M_RTC) { hal_time_resync(); nos_toast("Resync requested"); }
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
    float bp = m_clamp((nos_t - D.t0) / 0.3f, 0, 1);
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
        ui_label(fb, UI_CX, 170, b, C_PRI, UI_CENTER);
    } else if (V.touched) {
        char b[24]; snprintf(b, sizeof b, "LAST %d, %d", V.touch_x, V.touch_y);
        ui_label(fb, UI_CX, 170, b, C_DIS, UI_CENTER);
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
