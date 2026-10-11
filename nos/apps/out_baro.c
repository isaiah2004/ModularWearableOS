/* out_baro.c — Barometer / altimeter / local climate (BME280 on the strap), outdoor team.
 *
 * Three pages in a horizontal pager (SCR_HDRAG; back swipe from the left edge):
 *   1 PRESSURE  hPa in dot-matrix, 3 h trend, 24 h dot chart whose "now" column is
 *               topped by the screen's one red dot; a storm warning (red) when it falls
 *               > 3 hPa in 3 h.
 *   2 ALTITUDE  metres, sea-level reference, CALIBRATE pushes the sea-level picker
 *               (scr_baro_cal: drag / - + with auto-repeat, Use GPS when the GPS has a
 *               3D fix, Set; back = cancel).
 *   3 LOCAL     temperature + humidity, labelled as a strap sensor.
 * Sensors are read in tick() into statics; draw() only reads them. Live values change
 * at most once a second, with a digit morph.
 *
 * Sim preview knob: NOS_STORM=1 (sim builds only) forces a -3.6 hPa / 3 h trend. */
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
#include <stdlib.h>

#define NPAGE  3
#define PAGE_W 466.0f
#define NHIST  144
#define NCOL   24
#define CROWS  7
#define IRIS_LAG 0.17f

/* ------------------------------------------------------------- pager */
typedef struct { mspring sp; int n, page, dragging; float x0; } pager;
static void pg_init(pager *p, int n, int page)
{
    memset(p, 0, sizeof *p);
    p->n = n; p->page = page;
    msp_init(&p->sp, page * PAGE_W, SPRING_PAGE);
}
static int pg_input(pager *p, const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN: if (p->sp.moving) { p->sp.v = 0; p->sp.moving = 0; } p->x0 = p->sp.x; return 0;
    case IN_DRAG_START:
        if (in->axis != AXIS_X) return 0;
        p->dragging = 1; p->x0 = p->sp.x;
        /* fall through */
    case IN_DRAG: {
        if (!p->dragging) return 0;
        float x = p->x0 - in->dx, max = (p->n - 1) * PAGE_W;
        if (x < 0) x = -rubber_band(-x, PAGE_W);
        if (x > max) x = max + rubber_band(x - max, PAGE_W);
        msp_set(&p->sp, x);
        return 1;
    }
    case IN_DRAG_END: {
        if (!p->dragging) return 0;
        p->dragging = 0;
        float v = -in->vx;
        float proj = p->sp.x + v * 0.18f;
        int t = (int)floorf(proj / PAGE_W + 0.5f);
        int cur = (int)floorf(p->x0 / PAGE_W + 0.5f);
        if (t > cur + 1) t = cur + 1;
        if (t < cur - 1) t = cur - 1;
        if (t < 0) t = 0;
        if (t > p->n - 1) t = p->n - 1;
        msp_kick(&p->sp, t * PAGE_W, v);
        return 1;
    }
    default: return 0;
    }
}
static int pg_tick(pager *p, float dt)
{
    msp_step(&p->sp, dt);
    int pg = (int)floorf(p->sp.x / PAGE_W + 0.5f);
    if (pg < 0) pg = 0;
    if (pg > p->n - 1) pg = p->n - 1;
    if (pg != p->page) { p->page = pg; nos_feedback(FB_TICK); return 1; }
    return 0;
}
static float pg_pos(const pager *p) { return floorf(p->sp.x + 0.5f); }

/* ------------------------------------------------------------- morphing value */
typedef struct { char cur[16], prev[16]; float t0, dur; } mval;
static void mv_set(mval *m, const char *s, float dur)
{
    if (!strcmp(m->cur, s)) return;
    if (strlen(m->cur) == strlen(s) && m->cur[0]) { strcpy(m->prev, m->cur); m->t0 = nos_t; }
    else { strcpy(m->prev, s); m->t0 = -10; }
    m->dur = dur;
    snprintf(m->cur, sizeof m->cur, "%s", s);
}
static void mv_draw(gfx_fb *fb, const dot_style *st, float x, float y, const mval *m, float build)
{
    if (build < 1.0f) { dot_text_anim(fb, st, x, y, m->cur, 1, build, DOT_REVEAL_RANDOM, 3u); return; }
    float p = (nos_t - m->t0) / m->dur;
    if (p < 1.0f) dot_text_morph(fb, st, x, y, m->prev, m->cur, 1, p);
    else dot_text_a(fb, st, x, y, m->cur, 1);
}

/* ------------------------------------------------------------- buttons
 * Pressed = C_SEC fill with dark ink (readable on the panel); disabled = C_SURF with
 * C_DIS ink; `k` 0..1 fades a disabled button into its enabled look (8 steps). */
static void btn(gfx_fb *fb, float cx, float cy, float w, const char *label, int primary, int pressed, float k)
{
    k = floorf(m_clamp(k, 0, 1) * 8.0f + 0.5f) / 8.0f;
    uint16_t fill, ink;
    if (pressed) { fill = C_SEC; ink = C_BG; }
    else if (primary) { fill = ui_mix(C_SURF, C_WHITE, k); ink = ui_mix(C_DIS, C_BG, k); }
    else { fill = ui_mix(C_SURF, C_SURF2, k); ink = ui_mix(C_DIS, C_PRI, k); }
    ui_rrect(fb, cx - w / 2, cy - UI_BTN_H / 2, w, UI_BTN_H, UI_BTN_H / 2, fill);
    ui_label(fb, (int)cx, (int)(cy + UI_F_LABEL->ascent * 0.5f - 2), label, ink, UI_CENTER);
}
static void round_btn(gfx_fb *fb, float cx, float cy, float r, int icon, int pressed)
{
    gfx_fill_circle(fb, cx, cy, r, pressed ? C_SEC : C_SURF2);
    dot_icon(fb, icon, cx, cy, 4.0f, 3.2f, pressed ? C_BG : C_PRI);
}

/* ------------------------------------------------------------- state */
static struct {
    int present, valid;
    hal_env env;
    float *hist; int nhist;
    float hist_t;            /* nos_t of the last history read */
    float val_t;             /* nos_t of the last displayed-value refresh (1 Hz) */
    uint8_t cols[NCOL];
    int ncols;
    float lo, hi;            /* chart range */
    float d3h; int have3h;   /* change over 3 h */
    int storm;
    float sea;               /* derived sea-level reference */
    pager pg;
    float t0;                /* entry build start */
    mval m_hpa, m_alt, m_temp;
    int pressed;             /* control id under the finger */
    float press_until;       /* pressed state shows >= 3 frames */
    float saved_t;
    int inited;
} B;

enum { P_NONE = 0, P_CAL, P_MINUS, P_PLUS, P_GPS, P_SET };

static float alt_from(float p, float p0)
{
    if (p <= 0 || p0 <= 0) return 0;
    return 44330.0f * (1.0f - powf(p / p0, 1.0f / 5.255f));
}
static float sea_from(float p, float alt)
{
    float k = 1.0f - alt / 44330.0f;
    if (k <= 0.05f) k = 0.05f;
    return p / powf(k, 5.255f);
}

static int storm_knob(void)
{
#ifndef ESP_PLATFORM
    static int k = -1;
    if (k < 0) { const char *e = getenv("NOS_STORM"); k = e && *e == '1'; }
    return k;
#else
    return 0;
#endif
}

static void read_history(void)
{
    if (!B.hist) B.hist = (float *)hal_big_alloc(NHIST * sizeof(float));
    if (!B.hist) { B.nhist = 0; return; }
    B.nhist = hal_env_history(B.hist, NHIST);
    if (B.nhist > NHIST) B.nhist = NHIST;
    /* hourly means, the newest hour last (right-aligned) */
    int nh = (B.nhist + 5) / 6;
    if (nh > NCOL) nh = NCOL;
    float mean[NCOL];
    float lo = 1e9f, hi = -1e9f;
    for (int c = 0; c < nh; c++) {
        int end = B.nhist - (nh - 1 - c) * 6, start = end - 6;
        if (start < 0) start = 0;
        float s = 0; int k = 0;
        for (int i = start; i < end; i++) if (B.hist[i] > 0) { s += B.hist[i]; k++; }
        mean[c] = k ? s / k : 0;
        if (k) { if (mean[c] < lo) lo = mean[c]; if (mean[c] > hi) hi = mean[c]; }
    }
    if (B.valid) {        /* the live reading belongs in the "now" column's range too */
        if (B.env.pressure_hpa < lo) lo = B.env.pressure_hpa;
        if (B.env.pressure_hpa > hi) hi = B.env.pressure_hpa;
    }
    if (hi - lo < 4.0f) { float m = (hi + lo) * 0.5f; lo = m - 2.0f; hi = m + 2.0f; }
    lo = floorf(lo); hi = ceilf(hi);
    B.lo = lo; B.hi = hi;
    memset(B.cols, 0, sizeof B.cols);
    B.ncols = nh;
    for (int c = 0; c < nh; c++) {
        float v = c == nh - 1 && B.valid ? B.env.pressure_hpa : mean[c];
        if (v <= 0) continue;
        float f = (v - lo) / (hi - lo);
        int cells = 1 + (int)floorf(f * (CROWS - 1) + 0.5f);   /* at least one dot */
        B.cols[NCOL - nh + c] = (uint8_t)(cells * 255 / CROWS);
    }
    /* 3 h trend: 18 samples of 10 min */
    B.have3h = B.nhist > 18;
    if (B.have3h) {
        float now = B.valid ? B.env.pressure_hpa : B.hist[B.nhist - 1];
        B.d3h = now - B.hist[B.nhist - 1 - 18];
    } else B.d3h = 0;
    if (storm_knob()) { B.have3h = 1; B.d3h = -3.6f; }
    B.storm = B.have3h && B.d3h < -3.0f;
}

static void fmt1(char *o, int n, float v) { snprintf(o, (size_t)n, "%.1f", (double)(floorf(v * 10.0f + 0.5f) / 10.0f)); }

static void refresh_values(void)
{
    char t[16];
    B.sea = sea_from(B.env.pressure_hpa, B.env.altitude_m);
    fmt1(t, sizeof t, B.env.pressure_hpa); mv_set(&B.m_hpa, t, 0.28f);
    snprintf(t, sizeof t, "%d", (int)lroundf(B.env.altitude_m)); mv_set(&B.m_alt, t, 0.28f);
    snprintf(t, sizeof t, "%.1f\xB0", (double)(floorf(B.env.temp_c * 10.0f + 0.5f) / 10.0f)); mv_set(&B.m_temp, t, 0.28f);
}

static void baro_enter(void)
{
    if (nos_revealed() && B.inited) {        /* back from the picker: no rebuild */
        B.hist_t = -100; B.val_t = -100;
        return;
    }
    int keep = B.pg.n ? B.pg.page : 0;
    float *h = B.hist;
    memset(&B, 0, sizeof B);
    B.hist = h;
    B.inited = 1;
    pg_init(&B.pg, NPAGE, keep);
    B.t0 = nos_t + IRIS_LAG;
    B.hist_t = -100; B.val_t = -100;
    B.saved_t = -100;
}

static void baro_tick(float dt)
{
    B.present = (hal_ext_present() & EXT_ENV) != 0;
    B.valid = B.present && hal_env_get(&B.env) && B.env.valid;
    if (B.valid && nos_t - B.hist_t > 5.0f) { read_history(); B.hist_t = nos_t; }
    if (B.valid && nos_t - B.val_t >= 1.0f) { refresh_values(); B.val_t = nos_t; }
    pg_tick(&B.pg, dt);
    if (B.pressed && nos_t > B.press_until && B.press_until > 0) { B.pressed = 0; B.press_until = 0; }
}

/* ------------------------------------------------------------- pages */
static float build_of(void) { return m_clamp((nos_t - B.t0) / UI_ENTRY_S, 0, 1); }

static void page_pressure(gfx_fb *fb)
{
    ui_title(fb, "BAROMETER", 0);
    float b = build_of();
    /* trend row */
    char t[40];
    int icon; const char *word;
    if (!B.have3h) { icon = 0; word = "TREND IN 3 H"; }
    else if (B.d3h > 1.0f) { icon = DOT_ICON_ARROW_UP; word = "RISING"; }
    else if (B.d3h < -1.0f) { icon = DOT_ICON_ARROW_DOWN; word = "FALLING"; }
    else { icon = DOT_ICON_ARROW_RIGHT; word = "STEADY"; }
    if (B.storm) {
        ui_state(fb, UI_CX, 118, "STORM WARNING", C_RED);          /* the screen's one red */
    } else {
        if (B.have3h) snprintf(t, sizeof t, "%s %+.1f / 3H", word, (double)B.d3h);
        else snprintf(t, sizeof t, "%s", word);
        int w = ui_text_width(UI_F_LABEL, t, 2) + (icon ? 24 : 0);
        int x = UI_CX - w / 2;
        if (icon) dot_icon(fb, icon, x + 8, 112, 1.8f, 1.5f, C_PRI);
        ui_label(fb, x + (icon ? 24 : 0), 118, t, C_SEC, UI_LEFT);
    }
    /* hero */
    dot_style st = { DOT_FONT_TALL, UI_HEROW_PITCH, UI_HEROW_PITCH * 0.8f, C_WHITE, 0, 1, 1 };
    mv_draw(fb, &st, UI_CX, 142, &B.m_hpa, b);
    ui_text(fb, UI_F_LABEL, UI_CX, 242, "hPa", C_SEC, UI_CENTER, 2);
    /* 24 h chart */
    const float pitch = 10.0f, d = 7.0f;
    float x0 = UI_CX - NCOL * pitch * 0.5f, y0 = 272;
    if (B.ncols < 2) {
        ui_state(fb, UI_CX, 320, "COLLECTING", C_DIS);
        return;
    }
    uint8_t v[NCOL];
    for (int i = 0; i < NCOL; i++) v[i] = (uint8_t)(B.cols[i] * m_clamp(b * 1.6f - i * 0.02f, 0, 1));
    dot_columns(fb, x0, y0, NCOL, CROWS, pitch, d, v, C_PRI, C_LINE);
    /* "now": only the top lit dot of the last column is red (white while a storm
     * warning holds the red) */
    int cells = (v[NCOL - 1] * CROWS + 127) / 255;
    if (cells > 0) {
        float cx = x0 + (NCOL - 1) * pitch + pitch * 0.5f, cy = y0 + (CROWS - cells) * pitch + pitch * 0.5f;
        dot_disc(fb, cx, cy, d + 1.0f, B.storm ? C_WHITE : C_RED);
    }
    ui_label(fb, (int)x0, 370, "-24H", C_DIS, UI_LEFT);
    ui_label(fb, (int)(x0 + NCOL * pitch), 370, "NOW", C_DIS, UI_RIGHT);
}

static void page_altitude(gfx_fb *fb)
{
    ui_title(fb, "ALTITUDE", 0);
    float b = build_of();
    dot_style st = { DOT_FONT_TALL, UI_HERO_PITCH, UI_HERO_PITCH * 0.8f, C_WHITE, 0, 1, 1 };
    mv_draw(fb, &st, UI_CX, 118, &B.m_alt, b);
    ui_label(fb, UI_CX, 228, "METRES", C_SEC, UI_CENTER);
    char t[40];
    if (nos_t - B.saved_t < 1.6f) ui_state(fb, UI_CX, 284, "SAVED", C_PRI);
    else {
        snprintf(t, sizeof t, "SEA LEVEL %.1f", (double)(floorf(B.sea * 10 + 0.5f) / 10));
        ui_label_unit(fb, UI_CX, 284, t, "hPa", C_DIS, UI_CENTER);
    }
    btn(fb, UI_CX, UI_BTN_Y, UI_BTN_W, "Calibrate", 0, B.pressed == P_CAL, 1);
}

static void page_local(gfx_fb *fb)
{
    ui_title(fb, "LOCAL", 0);
    float b = build_of();
    /* the number and its degree are centred as a group; the unit sits under it */
    dot_style st = { DOT_FONT_TALL, UI_HEROW_PITCH, UI_HEROW_PITCH * 0.8f, C_WHITE, 0, 1, 1 };
    mv_draw(fb, &st, UI_CX, 118, &B.m_temp, b);
    ui_label(fb, UI_CX, 222, nos_prefs.units_metric ? "TEMPERATURE C" : "TEMPERATURE F", C_SEC, UI_CENTER);
    /* humidity as 20 whole dots (5% each) */
    float h = m_clamp(B.env.humidity, 0, 100);
    const float pitch = 14.0f;
    dot_bar(fb, UI_CX - 9.5f * pitch, 284, 20, pitch, 8.0f, floorf(h / 5.0f + 0.5f) * b, C_PRI, C_LINE2);
    char t[24];
    snprintf(t, sizeof t, "HUMIDITY %d%%", (int)lroundf(h));
    ui_label(fb, UI_CX, 326, t, C_SEC, UI_CENTER);
    ui_label(fb, UI_CX, 386, "STRAP SENSOR", C_DIS, UI_CENTER);
}

/* ------------------------------------------------------------- draw / input */
static void baro_draw(gfx_fb *fb)
{
    if (!B.present) {
        ui_title(fb, "BAROMETER", 0);
        ui_absent(fb, DOT_ICON_GAUGE, "Not connected", "Add the climate sensor for pressure",
                  "CLIMATE MODULE", build_of());
        return;
    }
    if (!B.valid) {
        ui_title(fb, "BAROMETER", 0);
        ui_absent(fb, DOT_ICON_GAUGE, 0, 0, 0, build_of());
        ui_state(fb, UI_CX, 340, "READING", C_SEC);
        return;
    }
    float pos = pg_pos(&B.pg);
    for (int i = 0; i < NPAGE; i++) {
        float off = i * PAGE_W - pos;
        if (off <= -PAGE_W || off >= PAGE_W) continue;
        gfx_fb sub;
        if (!nos_subview(fb, &sub, 0, 0, GFX_W, GFX_H, (int)off, 0)) continue;
        if (i == 0) page_pressure(&sub);
        else if (i == 1) page_altitude(&sub);
        else page_local(&sub);
    }
    ui_page_dots(fb, UI_CX, UI_DOTS_Y, NPAGE, pos / PAGE_W);
}

extern const nos_screen scr_baro_cal;

static int baro_input(const nos_input *in)
{
    if (!B.valid) return 0;
    if (pg_input(&B.pg, in)) { B.pressed = 0; return 1; }
    int on_cal = B.pg.page == 1 && !B.pg.sp.moving && abs(in->x - UI_CX) < UI_BTN_W / 2 &&
                 abs(in->y - UI_BTN_Y) < UI_BTN_H / 2;
    switch (in->type) {
    case IN_DOWN: B.pressed = on_cal ? P_CAL : 0; B.press_until = 0; return 1;
    case IN_UP: if (nos_t >= B.press_until) B.pressed = 0; return 1;
    case IN_TAP:
        if (on_cal) {
            nos_feedback(FB_TAP);
            B.press_until = nos_t + 0.1f;          /* keep the press visible for 3 frames */
            nos_push(&scr_baro_cal, TX_PUSH);
            return 1;
        }
        return 0;
    default: return 0;
    }
}

const nos_screen scr_baro = { "BAROMETER", baro_enter, 0, baro_tick, baro_draw, baro_input, SCR_HDRAG };

/* ================================================================ sea-level picker */
#define CAL_MIN 870.0f
#define CAL_MAX 1085.0f
#define PM_Y    256
#define PM_DX   70

static struct {
    float v;
    mval m;
    float t0;
    int pressed; float press_t0, press_until, rep_t;
    int repeated;
    int drag; float last_dy, acc;
    int gps_ok; float gps_alt, gps_t;   /* gps_t: when the fix arrived (enable fade) */
    hal_env env;
} K;

static void cal_show(void)
{
    char t[16];
    fmt1(t, sizeof t, K.v);
    mv_set(&K.m, t, 0.12f);
}
static void cal_step(float d)
{
    float v = floorf((K.v + d) * 10.0f + 0.5f) / 10.0f;
    if (v < CAL_MIN || v > CAL_MAX) { nos_feedback(FB_LIMIT); v = m_clamp(v, CAL_MIN, CAL_MAX); }
    else nos_feedback(FB_TICK);
    K.v = v;
    cal_show();
}

static void cal_enter(void)
{
    memset(&K, 0, sizeof K);
    hal_env_get(&K.env);
    float sea = sea_from(K.env.pressure_hpa, K.env.altitude_m);
    K.v = floorf((sea > 800 ? sea : 1013.25f) * 10.0f + 0.5f) / 10.0f;
    cal_show();
    K.m.t0 = -10;
    K.t0 = nos_t;
    K.gps_t = -10;
    hal_gps_enable(1);
}
static void cal_leave(void) { hal_gps_enable(0); }   /* a recording track re-enables it each frame */

static void cal_tick(float dt)
{
    hal_env_get(&K.env);
    hal_gps g;
    int ok = hal_gps_get(&g) && g.valid && g.fix3d;
    if (ok && !K.gps_ok) K.gps_t = nos_t;
    K.gps_ok = ok;
    K.gps_alt = g.alt_m;
    /* hold on - / +: auto-repeat at 8 Hz after 400 ms, whole hPa after 2 s */
    if ((K.pressed == P_MINUS || K.pressed == P_PLUS) && !K.drag) {
        float held = nos_t - K.press_t0;
        if (held >= 0.4f && nos_t - K.rep_t >= 0.125f) {
            K.rep_t = nos_t; K.repeated = 1;
            float s = held > 2.0f ? 1.0f : 0.1f;
            cal_step(K.pressed == P_MINUS ? -s : s);
        }
    }
    if ((K.pressed == P_SET || K.pressed == P_GPS) && K.press_until > 0 && nos_t > K.press_until) K.pressed = 0;
}

static void cal_draw(gfx_fb *fb)
{
    ui_title(fb, "SEA LEVEL", 0);
    float b = m_clamp((nos_t - K.t0) / UI_ENTRY_S, 0, 1);
    dot_style st = { DOT_FONT_TALL, UI_HEROW_PITCH, UI_HEROW_PITCH * 0.8f, C_WHITE, 0, 1, 1 };
    mv_draw(fb, &st, UI_CX, 102, &K.m, b);
    ui_text(fb, UI_F_LABEL, UI_CX, 200, "hPa", C_SEC, UI_CENTER, 2);
    round_btn(fb, UI_CX - PM_DX, PM_Y, 34, DOT_ICON_MINUS, K.pressed == P_MINUS);
    round_btn(fb, UI_CX + PM_DX, PM_Y, 34, DOT_ICON_PLUS, K.pressed == P_PLUS);
    char t[24];
    snprintf(t, sizeof t, "ALTITUDE %d", (int)lroundf(alt_from(K.env.pressure_hpa, K.v)));
    ui_label_unit(fb, UI_CX, 316, t, "m", C_DIS, UI_CENTER);
    /* both buttons always: Use GPS waits disabled for a 3D fix, then fades in place */
    float gk = K.gps_ok ? m_clamp((nos_t - K.gps_t) / 0.15f, 0, 1) : 0;
    btn(fb, UI_BTN_PAIR_X0, UI_BTN_Y, UI_BTN_PAIR_W, "Use GPS", 0, K.pressed == P_GPS && K.gps_ok, gk);
    btn(fb, UI_BTN_PAIR_X1, UI_BTN_Y, UI_BTN_PAIR_W, "Set", 1, K.pressed == P_SET, 1);
}

static int cal_hit(int x, int y)
{
    if (abs(y - PM_Y) < 46 && x < UI_CX && abs(x - (UI_CX - PM_DX)) < 48) return P_MINUS;
    if (abs(y - PM_Y) < 46 && x >= UI_CX && abs(x - (UI_CX + PM_DX)) < 48) return P_PLUS;
    if (abs(y - UI_BTN_Y) < UI_BTN_H / 2) {
        if (abs(x - UI_BTN_PAIR_X0) <= UI_BTN_PAIR_W / 2 + 4) return P_GPS;
        if (abs(x - UI_BTN_PAIR_X1) <= UI_BTN_PAIR_W / 2 + 4) return P_SET;
    }
    return 0;
}

static int cal_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN:
        K.pressed = cal_hit(in->x, in->y);
        K.press_t0 = nos_t; K.rep_t = nos_t; K.repeated = 0; K.press_until = 0;
        return 1;
    case IN_UP:
        if (K.pressed == P_MINUS || K.pressed == P_PLUS || nos_t >= K.press_until) K.pressed = 0;
        return 1;
    case IN_TAP: {
        int h = cal_hit(in->x, in->y);
        if (h == P_MINUS && !K.repeated) cal_step(-0.1f);
        else if (h == P_PLUS && !K.repeated) cal_step(0.1f);
        else if (h == P_GPS) {
            if (!K.gps_ok) { nos_feedback(FB_ERROR); return 1; }
            K.v = floorf(sea_from(K.env.pressure_hpa, K.gps_alt) * 10.0f + 0.5f) / 10.0f;
            cal_show();
            nos_feedback(FB_TAP);
            K.press_until = nos_t + 0.1f;
        } else if (h == P_SET) {
            hal_env_set_sea_level(K.v);
            B.saved_t = nos_t;
            nos_feedback(FB_SUCCESS);
            K.press_until = nos_t + 0.1f;
            nos_pop();
        }
        return 1;
    }
    case IN_LONG: return 1;                   /* the hold repeats in tick() */
    case IN_DRAG_START:
        if (in->axis != AXIS_Y) return 0;
        K.drag = 1; K.pressed = 0; K.last_dy = 0; K.acc = 0;
        /* fall through */
    case IN_DRAG: {
        if (!K.drag) return 0;
        /* every 8 px of travel is one detent; its size scales with the finger's speed */
        K.acc += -(in->dy - K.last_dy);
        K.last_dy = in->dy;
        float sp = fabsf(in->vy), step = sp < 300 ? 0.1f : sp < 900 ? 0.5f : 1.0f;
        while (K.acc >= 8.0f) { K.acc -= 8.0f; cal_step(step); }
        while (K.acc <= -8.0f) { K.acc += 8.0f; cal_step(-step); }
        return 1;
    }
    case IN_DRAG_END: K.drag = 0; return 1;
    default: return 0;
    }
}

const nos_screen scr_baro_cal = { "SEA LEVEL", cal_enter, cal_leave, cal_tick, cal_draw, cal_input, 0 };
