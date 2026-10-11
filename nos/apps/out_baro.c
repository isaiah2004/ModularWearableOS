/* out_baro.c — Barometer / altimeter / local climate (BME280 on the strap), outdoor team.
 *
 * Three pages in a horizontal pager (SCR_HDRAG; back swipe from the left edge):
 *   1 PRESSURE  hPa in dot-matrix, 3 h trend arrow, 24 h dot chart with the "now"
 *               column in red; storm warning (amber) when it falls > 3 hPa in 3 h.
 *   2 ALTITUDE  metres, sea-level reference, CALIBRATE -> picker (drag / - +) or
 *               "Use GPS" when the GPS has a 3D fix.
 *   3 LOCAL     temperature + humidity, labelled as a strap sensor.
 * Sensors are read in tick() into statics; draw() only reads them. */
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
/* returns 1 when the page index changed */
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
typedef struct { char cur[16], prev[16]; float t0; } mval;
static void mv_set(mval *m, const char *s)
{
    if (!strcmp(m->cur, s)) return;
    if (strlen(m->cur) == strlen(s) && m->cur[0]) { strcpy(m->prev, m->cur); m->t0 = nos_t; }
    else { strcpy(m->prev, s); m->t0 = -10; }
    snprintf(m->cur, sizeof m->cur, "%s", s);
}
/* degree mark: a 3x3 ring of dots hung right of a centred number */
static void deg_mark(gfx_fb *fb, float x, float y, float pitch, float p, uint16_t col)
{
    const float c = pitch * 0.72f, d = pitch * 0.5f * m_clamp(p * 1.5f - 0.5f, 0, 1);
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++)
            if (r != 1 || k != 1) dot_disc(fb, x + k * c, y + r * c, d, col);
}
static void mv_draw(gfx_fb *fb, const dot_style *st, float x, float y, const mval *m, float build)
{
    if (build < 1.0f) { dot_text_anim(fb, st, x, y, m->cur, 1, build, DOT_REVEAL_RANDOM, 3u); return; }
    float p = (nos_t - m->t0) / 0.28f;
    if (p < 1.0f) dot_text_morph(fb, st, x, y, m->prev, m->cur, 1, p);
    else dot_text_a(fb, st, x, y, m->cur, 1);
}

/* ------------------------------------------------------------- state */
static struct {
    int present, valid;
    hal_env env;
    float *hist; int nhist;
    float hist_t;            /* nos_t of the last history read */
    uint8_t cols[NCOL];
    int ncols;
    float lo, hi;            /* chart range */
    float d3h; int have3h;   /* change over 3 h */
    int storm;
    float sea;               /* derived sea-level reference */
    pager pg;
    float build_t[NPAGE];
    mval m_hpa, m_alt, m_temp;
    /* calibration picker */
    int cal;
    float cal_v, cal_v0, cal_build_t;
    int cal_drag;
    int gps_ok; float gps_alt;
    int pressed;             /* control id under the finger */
    float saved_t;
} B;

enum { P_NONE = 0, P_CAL, P_MINUS, P_PLUS, P_GPS, P_SET };

static void draw_absent(gfx_fb *fb, const char *title, int icon, const char *what, const char *part)
{
    ui_title(fb, title, 0);
    dot_icon(fb, icon, UI_CX, 176, 6.0f, 4.8f, C_DIS);
    ui_text(fb, UI_F_TITLE, UI_CX, 276, "Not connected", C_PRI, UI_CENTER, 0);
    ui_text_wrap(fb, UI_F_BODY, UI_CX, 316, 330, 30, 2, what, C_SEC, UI_CENTER);
    ui_label(fb, UI_CX, 404, part, C_DIS, UI_CENTER);
}

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
    B.storm = B.have3h && B.d3h < -3.0f;
}

static void baro_enter(void)
{
    int keep = B.pg.n ? B.pg.page : 0;
    float *h = B.hist;
    memset(&B, 0, sizeof B);
    B.hist = h;
    pg_init(&B.pg, NPAGE, keep);
    for (int i = 0; i < NPAGE; i++) B.build_t[i] = nos_t;
    B.hist_t = -100;
    B.saved_t = -100;
}

static void baro_leave(void)
{
    if (B.cal) hal_gps_enable(0);
}

static void fmt1(char *o, int n, float v) { snprintf(o, (size_t)n, "%.1f", (double)(floorf(v * 10.0f + 0.5f) / 10.0f)); }

static void baro_tick(float dt)
{
    B.present = (hal_ext_present() & EXT_ENV) != 0;
    B.valid = B.present && hal_env_get(&B.env) && B.env.valid;
    if (B.valid && nos_t - B.hist_t > 5.0f) { read_history(); B.hist_t = nos_t; }
    if (B.valid) {
        B.sea = sea_from(B.env.pressure_hpa, B.env.altitude_m);
        char t[16];
        fmt1(t, sizeof t, B.env.pressure_hpa); mv_set(&B.m_hpa, t);
        snprintf(t, sizeof t, "%d", (int)lroundf(B.env.altitude_m)); mv_set(&B.m_alt, t);
        snprintf(t, sizeof t, "%.1f", (double)(floorf(B.env.temp_c * 10.0f + 0.5f) / 10.0f)); mv_set(&B.m_temp, t);
    }
    pg_tick(&B.pg, dt);
    if (B.cal) {
        hal_gps g;
        B.gps_ok = hal_gps_get(&g) && g.valid && g.fix3d;
        B.gps_alt = g.alt_m;
    }
}

/* ------------------------------------------------------------- pages */
static float build_of(int page) { return m_clamp((nos_t - B.build_t[page]) / 0.36f, 0, 1); }

static void page_pressure(gfx_fb *fb)
{
    ui_title(fb, "BAROMETER", 0);
    float b = build_of(0);
    /* trend row */
    char t[40];
    int icon; const char *word;
    if (!B.have3h) { icon = 0; word = "TREND IN 3 H"; }
    else if (B.d3h > 1.0f) { icon = DOT_ICON_ARROW_UP; word = "RISING"; }
    else if (B.d3h < -1.0f) { icon = DOT_ICON_ARROW_DOWN; word = "FALLING"; }
    else { icon = DOT_ICON_ARROW_RIGHT; word = "STEADY"; }
    if (B.storm) {
        ui_state(fb, UI_CX, 118, "STORM WARNING", C_AMBER);
    } else {
        if (B.have3h) snprintf(t, sizeof t, "%s %+.1f / 3H", word, (double)B.d3h);
        else snprintf(t, sizeof t, "%s", word);
        int w = ui_text_width(UI_F_LABEL, t, 2) + (icon ? 24 : 0);
        int x = UI_CX - w / 2;
        if (icon) dot_icon(fb, icon, x + 8, 112, 1.8f, 1.5f, C_PRI);
        ui_label(fb, x + (icon ? 24 : 0), 118, t, C_SEC, UI_LEFT);
    }
    /* hero */
    dot_style st = { DOT_FONT_TALL, 8.0f, 6.4f, C_WHITE, 0, 1, 1 };
    mv_draw(fb, &st, UI_CX, 142, &B.m_hpa, b);
    ui_label(fb, UI_CX, 242, "HPA", C_SEC, UI_CENTER);
    /* 24 h chart */
    const float pitch = 10.0f, d = 7.0f;
    float x0 = UI_CX - NCOL * pitch * 0.5f, y0 = 272;
    if (B.ncols < 2) {
        ui_state(fb, UI_CX, 320, "COLLECTING", C_DIS);
        return;
    }
    uint8_t v[NCOL];
    for (int i = 0; i < NCOL; i++) v[i] = (uint8_t)(B.cols[i] * m_clamp(b * 1.6f - i * 0.02f, 0, 1));
    dot_columns(fb, x0, y0, NCOL - 1, CROWS, pitch, d, v, C_PRI, C_LINE);
    dot_columns(fb, x0 + (NCOL - 1) * pitch, y0, 1, CROWS, pitch, d, v + NCOL - 1, C_RED, C_LINE);
    ui_text(fb, UI_F_SMALL, (int)x0, 368, "-24H", C_DIS, UI_LEFT, 1);
    ui_text(fb, UI_F_SMALL, (int)(x0 + NCOL * pitch), 368, "NOW", C_DIS, UI_RIGHT, 1);
}

static void page_altitude(gfx_fb *fb)
{
    ui_title(fb, "ALTITUDE", 0);
    float b = build_of(1);
    dot_style st = { DOT_FONT_TALL, 10.0f, 8.0f, C_WHITE, 0, 1, 1 };
    mv_draw(fb, &st, UI_CX, 118, &B.m_alt, b);
    ui_label(fb, UI_CX, 242, "METRES", C_SEC, UI_CENTER);
    char t[40];
    if (nos_t - B.saved_t < 1.6f) ui_state(fb, UI_CX, 296, "SAVED", C_PRI);
    else {
        snprintf(t, sizeof t, "SEA LEVEL %.1f HPA", (double)(floorf(B.sea * 10 + 0.5f) / 10));
        ui_label(fb, UI_CX, 296, t, C_DIS, UI_CENTER);
    }
    ui_button(fb, UI_CX, 360, 196, 60, "Calibrate", BTN_SECONDARY, B.pressed == P_CAL);
}

static void page_local(gfx_fb *fb)
{
    ui_title(fb, "LOCAL", 0);
    float b = build_of(2);
    dot_style st = { DOT_FONT_TALL, 9.0f, 7.2f, C_WHITE, 0, 1, 1 };
    mv_draw(fb, &st, UI_CX, 114, &B.m_temp, b);
    deg_mark(fb, UI_CX + dot_text_width(&st, B.m_temp.cur) * 0.5f + st.pitch * 1.3f, 114 + st.pitch * 0.4f, st.pitch, b, C_WHITE);
    ui_label(fb, UI_CX, 228, "TEMPERATURE", C_SEC, UI_CENTER);
    /* humidity as 20 dots */
    float h = m_clamp(B.env.humidity, 0, 100);
    const float pitch = 14.0f;
    dot_bar(fb, UI_CX - 9.5f * pitch, 288, 20, pitch, 8.0f, h / 5.0f * b, C_PRI, C_LINE2);
    char t[24];
    snprintf(t, sizeof t, "HUMIDITY %d%%", (int)lroundf(h));
    ui_label(fb, UI_CX, 330, t, C_SEC, UI_CENTER);
    ui_label(fb, UI_CX, 392, "STRAP SENSOR", C_DIS, UI_CENTER);
}

/* ------------------------------------------------------------- calibration picker */
#define CAL_MIN 870.0f
#define CAL_MAX 1085.0f
#define CAL_BTN_Y 374
static void cal_draw(gfx_fb *fb)
{
    ui_title(fb, "SEA LEVEL", 0);
    float b = m_clamp((nos_t - B.cal_build_t) / 0.3f, 0, 1);
    char t[24];
    fmt1(t, sizeof t, B.cal_v);
    dot_style st = { DOT_FONT_TALL, 7.0f, 5.6f, C_WHITE, 0, 1, 1 };
    dot_text_anim(fb, &st, UI_CX, 106, t, 1, b, DOT_REVEAL_RANDOM, 9u);
    ui_label(fb, UI_CX, 198, "HPA", C_SEC, UI_CENTER);
    ui_icon_button(fb, UI_CX - 66, 252, 34, DOT_ICON_MINUS, 0, B.pressed == P_MINUS);
    ui_icon_button(fb, UI_CX + 66, 252, 34, DOT_ICON_PLUS, 0, B.pressed == P_PLUS);
    snprintf(t, sizeof t, "ALTITUDE %d M", (int)lroundf(alt_from(B.env.pressure_hpa, B.cal_v)));
    ui_label(fb, UI_CX, 320, t, C_DIS, UI_CENTER);
    if (B.gps_ok) {
        ui_button(fb, UI_CX - 80, CAL_BTN_Y, 150, 60, "Use GPS", BTN_SECONDARY, B.pressed == P_GPS);
        ui_button(fb, UI_CX + 80, CAL_BTN_Y, 150, 60, "Set", BTN_PRIMARY, B.pressed == P_SET);
    } else
        ui_button(fb, UI_CX, CAL_BTN_Y, 196, 60, "Set", BTN_PRIMARY, B.pressed == P_SET);
}

static int cal_hit(int x, int y)
{
    if (abs(y - 252) < 46 && abs(x - (UI_CX - 66)) < 48 && x < UI_CX) return P_MINUS;
    if (abs(y - 252) < 46 && abs(x - (UI_CX + 66)) < 48 && x >= UI_CX) return P_PLUS;
    if (abs(y - CAL_BTN_Y) < 44) {
        if (B.gps_ok) return x < UI_CX ? (x > UI_CX - 160 ? P_GPS : 0) : (x < UI_CX + 160 ? P_SET : 0);
        return abs(x - UI_CX) < 100 ? P_SET : 0;
    }
    return 0;
}

static void cal_open(void)
{
    B.cal = 1;
    B.cal_v = floorf((B.sea > 0 ? B.sea : 1013.25f) * 10.0f + 0.5f) / 10.0f;
    B.cal_build_t = nos_t;
    hal_gps_enable(1);
    nos_invalidate();
}
static void cal_close(int save)
{
    if (save) {
        hal_env_set_sea_level(B.cal_v);
        B.saved_t = nos_t;
        nos_feedback(FB_SUCCESS);
    }
    B.cal = 0;
    B.build_t[1] = nos_t;
    hal_gps_enable(0);
    nos_invalidate();
}
static void cal_step(float d)
{
    float v = floorf((B.cal_v + d) * 10.0f + 0.5f) / 10.0f;
    if (v < CAL_MIN || v > CAL_MAX) { nos_feedback(FB_LIMIT); v = m_clamp(v, CAL_MIN, CAL_MAX); }
    else nos_feedback(FB_TICK);
    B.cal_v = v;
}

static int cal_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN: B.pressed = cal_hit(in->x, in->y); return 1;
    case IN_UP: B.pressed = 0; return 1;
    case IN_TAP: {
        int h = cal_hit(in->x, in->y);
        if (h == P_MINUS) cal_step(-0.1f);
        else if (h == P_PLUS) cal_step(0.1f);
        else if (h == P_GPS) {
            B.cal_v = floorf(sea_from(B.env.pressure_hpa, B.gps_alt) * 10.0f + 0.5f) / 10.0f;
            nos_feedback(FB_TAP);
        } else if (h == P_SET) cal_close(1);
        return 1;
    }
    case IN_LONG: {
        int h = cal_hit(in->x, in->y);
        if (h == P_MINUS) cal_step(-1.0f);
        else if (h == P_PLUS) cal_step(1.0f);
        return 1;
    }
    case IN_DRAG_START:
        if (in->axis != AXIS_Y) return 0;
        B.cal_drag = 1; B.cal_v0 = B.cal_v; B.pressed = 0;
        return 1;
    case IN_DRAG: {
        if (!B.cal_drag) return 0;
        /* 8 px per 0.1 hPa, finger up = higher */
        float v = B.cal_v0 + floorf(-in->dy / 8.0f) * 0.1f;
        v = m_clamp(floorf(v * 10.0f + 0.5f) / 10.0f, CAL_MIN, CAL_MAX);
        if (fabsf(v - B.cal_v) > 0.05f) { B.cal_v = v; nos_feedback(FB_TICK); }
        return 1;
    }
    case IN_DRAG_END: B.cal_drag = 0; return 1;
    case IN_BTN: cal_close(0); nos_feedback(FB_BACK); return 1;   /* the button cancels */
    default: return 0;
    }
}

/* ------------------------------------------------------------- draw / input */
static void baro_draw(gfx_fb *fb)
{
    if (!B.present) {
        draw_absent(fb, "BAROMETER", DOT_ICON_GAUGE, "Add the climate sensor for pressure",
                    "BME280  STRAP");
        return;
    }
    if (!B.valid) {
        ui_title(fb, "BAROMETER", 0);
        dot_icon(fb, DOT_ICON_GAUGE, UI_CX, 190, 6.0f, 4.8f, C_DIS);
        ui_state(fb, UI_CX, 292, "READING", C_SEC);
        return;
    }
    if (B.cal) { cal_draw(fb); return; }
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
    ui_page_dots(fb, UI_CX, 434, NPAGE, pos / PAGE_W);
}

static int baro_input(const nos_input *in)
{
    if (!B.valid) return 0;
    if (B.cal) return cal_input(in);
    if (pg_input(&B.pg, in)) { B.pressed = 0; return 1; }
    int on_cal = B.pg.page == 1 && !B.pg.sp.moving && abs(in->x - UI_CX) < 110 && abs(in->y - 360) < 44;
    switch (in->type) {
    case IN_DOWN: B.pressed = on_cal ? P_CAL : 0; return 1;
    case IN_UP: B.pressed = 0; return 1;
    case IN_TAP:
        if (on_cal) { nos_feedback(FB_TAP); cal_open(); return 1; }
        return 0;
    default: return 0;
    }
}

const nos_screen scr_baro = { "BAROMETER", baro_enter, baro_leave, baro_tick, baro_draw, baro_input, SCR_HDRAG };
