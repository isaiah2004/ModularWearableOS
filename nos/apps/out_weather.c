/* out_weather.c — Weather app + home widget, outdoor team.
 *
 * Data: svc_weather() (phone via Gadgetbridge, else Wi-Fi). The wrist line reads the
 * strap BME280 when present. Layout (one vertical scroll, momentum + rubber band):
 *   now      animated dot icon + big dot-matrix temperature, condition, hi/lo,
 *            feels / humidity / wind, wrist sensor line
 *   hourly   next 12 h as two strips of six: hour, dot icon, temperature
 *   5 days   day, icon, low, segmented low-high bar on the week's range, high
 * Animated icons follow Nothing OS 3: built from dots of several sizes, looping
 * subtly on a 120-frame (4 s) cycle — sun rays turn, clouds drift, rain and snow
 * fall, a storm bolt flashes once per loop. Each icon is ONE damage command keyed by
 * (condition, frame, scale), so the loop repaints only the icon's box. */
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

#define PI_F  3.14159265f
#define LOOP  120                 /* frames per animation loop (4 s) */

/* ------------------------------------------------------------- helpers */
static int rows_hit(const gfx_fb *fb, float y0, float y1)
{
    if (fb->rec) return 1;
    float lo = (float)(fb->y_base - fb->oy), hi = lo + (float)fb->h;
    return y1 >= lo && y0 < hi;
}
static float lsin(int f, int period, float phase) { return gfx_fast_sin(2 * PI_F * ((float)(f % period) / period + phase)); }

/* ------------------------------------------------------------- dot weather icons
 * Local geometry at scale 1: about 124 x 112 px around (cx, cy). */
typedef struct { gfx_fb *fb; float cx, cy, s, g; } ictx;   /* g = build-in 0..1 */

static void idot(const ictx *k, float x, float y, float d, uint16_t col)
{
    float px = k->cx + x * k->s, py = k->cy + y * k->s, pd = d * k->s * k->g;
    if (pd < 0.6f || !rows_hit(k->fb, py - pd, py + pd)) return;
    dot_disc(k->fb, px, py, pd, col);
}
static void ifill(const ictx *k, float x, float y, float r, uint16_t col)
{
    float px = k->cx + x * k->s, py = k->cy + y * k->s, pr = r * k->s;
    if (!rows_hit(k->fb, py - pr, py + pr)) return;
    gfx_fill_circle(k->fb, px, py, pr, col);
}

/* cloud = three circles over a flat base; sdf < 0 inside */
static float cloud_sdf(float x, float y)
{
    float d1 = sqrtf((x + 20) * (x + 20) + (y - 6) * (y - 6)) - 18;
    float d2 = sqrtf((x - 4) * (x - 4) + (y + 8) * (y + 8)) - 24;
    float d3 = sqrtf((x - 26) * (x - 26) + (y - 8) * (y - 8)) - 16;
    float d = d1 < d2 ? d1 : d2;
    d = d < d3 ? d : d3;
    float base = y - 22;                       /* flat bottom */
    return d > base ? d : base;
}
static void cloud(const ictx *k, float ox, float oy, uint16_t col, int occlude)
{
    if (occlude) {                              /* black silhouette hides what is behind */
        ifill(k, ox - 20, oy + 6, 21, C_BG); ifill(k, ox + 4, oy - 8, 27, C_BG); ifill(k, ox + 26, oy + 8, 19, C_BG);
        float px = k->cx + (ox - 40) * k->s, py = k->cy + (oy + 2) * k->s;
        if (rows_hit(k->fb, py, py + 24 * k->s))
            gfx_fill_rect(k->fb, (int)px, (int)py, (int)(84 * k->s), (int)(24 * k->s), C_BG);
    }
    const float p = 7.0f;
    for (float y = -32; y <= 22; y += p)
        for (float x = -38; x <= 42; x += p) {
            float d = cloud_sdf(x, y);
            if (d > -1.5f) continue;
            float sz = m_clamp(2.4f - d * 0.32f, 2.4f, 6.0f);   /* bigger dots deeper inside */
            idot(k, x + ox, y + oy, sz, col);
        }
}
static void sun(const ictx *k, float ox, float oy, float sc, int f)
{
    ifill(k, ox, oy, 21 * sc * k->g, C_WHITE);
    float rot = (float)(f % LOOP) / LOOP * (PI_F / 4);         /* one ray spacing per loop */
    for (int i = 0; i < 8; i++) {
        float a = rot + i * (PI_F / 4), s = gfx_fast_sin(a), c = gfx_fast_cos(a);
        float breathe = 0.5f + 0.5f * lsin(f, LOOP, i * 0.125f);
        idot(k, ox + s * 34 * sc, oy - c * 34 * sc, 7.0f * sc, C_WHITE);
        idot(k, ox + s * 47 * sc, oy - c * 47 * sc, (3.6f + 1.6f * breathe) * sc, C_PRI);
    }
}
static void moon(const ictx *k, float ox, float oy, int f)
{
    const float p = 6.5f;
    for (float y = -28; y <= 28; y += p)
        for (float x = -28; x <= 28; x += p) {
            float d1 = sqrtf(x * x + y * y) - 27, d2 = 22 - sqrtf((x - 14) * (x - 14) + (y + 10) * (y + 10));
            float d = d1 > d2 ? d1 : d2;
            if (d > -1.0f) continue;
            idot(k, ox + x, oy + y, m_clamp(2.6f - d * 0.45f, 2.6f, 6.0f), C_WHITE);
        }
    static const signed char st[4][2] = { { 30, -30 }, { 44, -6 }, { 22, 26 }, { 46, 22 } };
    for (int i = 0; i < 4; i++) {
        float tw = 0.5f + 0.5f * lsin(f, LOOP, i * 0.31f);
        idot(k, ox + st[i][0], oy + st[i][1], 2.0f + 2.6f * tw, i & 1 ? C_SEC : C_PRI);
    }
}
static void rain(const ictx *k, int f, int heavy)
{
    static const float cols[4] = { -24, -8, 8, 24 };
    for (int c = 0; c < 4; c++)
        for (int j = 0; j < 2; j++) {
            float t = (float)((f + c * 11 + j * 15) % 30) / 30.0f;   /* 1 s fall */
            float y = 26 + t * 34, x = cols[c] - t * 6;
            float d = (heavy ? 6.0f : 5.0f) * (1.0f - t * 0.55f);
            idot(k, x, y, d, C_PRI);
        }
}
static void snow(const ictx *k, int f)
{
    static const float cols[5] = { -28, -14, 0, 14, 28 };
    for (int c = 0; c < 5; c++) {
        float t = (float)((f + c * 13) % 60) / 60.0f;              /* 2 s fall */
        float x = cols[c] + 4.0f * lsin(f + c * 9, 60, 0), y = 26 + t * 34;
        float d = 5.5f * (1.0f - t * 0.4f);
        idot(k, x, y, d, C_WHITE);
    }
}
static void bolt(const ictx *k, int f)
{
    int ph = f % LOOP;
    int flash = ph < 3 || (ph >= 6 && ph < 8);
    static const signed char b[6][2] = { { 6, 24 }, { 0, 32 }, { -6, 40 }, { 4, 42 }, { -2, 50 }, { -8, 58 } };
    for (int i = 0; i < 6; i++) idot(k, b[i][0], b[i][1], flash ? 6.5f : 5.0f, flash ? C_WHITE : C_DIS);
}
static void fog(const ictx *k, int f)
{
    for (int r = 0; r < 4; r++) {
        float y = -24 + r * 16, shift = (float)(f % LOOP) / LOOP * 9.0f * (r & 1 ? -1 : 1);
        for (int i = -6; i <= 6; i++) {
            float x = i * 9.0f + shift;
            float edge = 1.0f - m_clamp((fabsf(x) - 34) / 18, 0, 1);
            if (edge <= 0) continue;
            idot(k, x, y, 5.0f * edge, r == 1 ? C_PRI : C_SEC);
        }
    }
}
static void wind(const ictx *k, int f)
{
    static const float ys[3] = { -22, 0, 22 };
    static const float len[3] = { 70, 92, 58 };
    for (int r = 0; r < 3; r++) {
        float t = (float)((f + r * 37) % LOOP) / LOOP;
        for (int i = 0; i < 9; i++) {
            float x = -50 + i * 11.0f;
            float head = -50 + t * (len[r] + 60);                  /* a gust travels along the line */
            float dd = fabsf(x - head);
            float sz = 2.6f + 3.4f * (1.0f - m_clamp(dd / 30, 0, 1));
            if (x > -50 + len[r]) break;
            idot(k, x, ys[r], sz, C_PRI);
        }
    }
}

/* scale s: 1 = 124 px icon. g: build-in. f: loop frame. */
static void wx_icon(gfx_fb *fb, int cond, float cx, float cy, float s, float g, int f)
{
    float hw = 64 * s, hh = 66 * s;
    if (fb->rec) {
        unsigned h = gfx_hash_u(gfx_hash_u(gfx_hash_f(gfx_hash_f(0x3EA7u, s), floorf(g * 32)), (unsigned)cond), (unsigned)(f % LOOP));
        gfx_rec_cmd(fb, (int)(cx - hw), (int)(cy - hh), (int)(cx + hw + 1), (int)(cy + hh + 1), h);
        return;
    }
    if (!rows_hit(fb, cy - hh, cy + hh)) return;
    ictx k = { fb, cx, cy, s, g };
    float drift = 3.0f * lsin(f, LOOP, 0);
    switch (cond) {
    case WX_CLEAR: sun(&k, 0, 0, 1.0f, f); break;
    case WX_NIGHT: moon(&k, -6, 0, f); break;
    case WX_PARTLY: sun(&k, 16, -16, 0.72f, f); cloud(&k, -6 + drift, 14, C_PRI, 1); break;
    case WX_CLOUDY: cloud(&k, 10 + drift * 0.6f, -16, C_DIS, 0); cloud(&k, -4 + drift, 6, C_PRI, 1); break;
    case WX_RAIN: cloud(&k, drift, -14, C_PRI, 0); k.cy -= 0; rain(&k, f, 0); break;
    case WX_STORM: cloud(&k, drift, -14, C_SEC, 0); rain(&k, f, 1); bolt(&k, f); break;
    case WX_SNOW: cloud(&k, drift, -14, C_PRI, 0); snow(&k, f); break;
    case WX_FOG: fog(&k, f); break;
    case WX_WIND: wind(&k, f); break;
    default: cloud(&k, drift, 0, C_PRI, 0); break;
    }
}

static int static_icon(int cond)
{
    switch (cond) {
    case WX_CLEAR: return DOT_ICON_SUN;
    case WX_NIGHT: return DOT_ICON_MOON;
    case WX_RAIN: case WX_STORM: return DOT_ICON_RAIN;
    case WX_SNOW: return DOT_ICON_SNOW;
    case WX_WIND: return DOT_ICON_WIND;
    default: return DOT_ICON_CLOUD;
    }
}

/* ------------------------------------------------------------- data */
static svc_weather_t s_demo;
static const svc_weather_t *wx_data(void)
{
    const svc_weather_t *w = svc_weather();
#ifndef ESP_PLATFORM
    /* sim preview knob: NOS_WX_DEMO=<WX_* index> shows sample data while the phone
     * link is empty (the services team's injection replaces this). */
    if (!w->valid) {
        static int inited = -2;
        if (inited == -2) {
            const char *e = getenv("NOS_WX_DEMO");
            inited = e && *e ? atoi(e) : -1;
            if (inited >= 0) {
                static const char *txt[] = { "Clear", "Partly cloudy", "Cloudy", "Light rain", "Thunderstorm",
                                             "Snow", "Fog", "Windy", "Clear night" };
                svc_weather_t *d = &s_demo;
                d->valid = 1; d->cond = inited % 9;
                d->temp_c = d->cond == WX_SNOW ? -2 : 24; d->feels_c = d->temp_c + 2;
                d->hi_c = d->temp_c + 3; d->lo_c = d->temp_c - 6;
                snprintf(d->text, sizeof d->text, "%s", txt[d->cond]);
                d->humidity = 62; d->rain_pct = 40; d->uv = 5; d->wind_kph = 12; d->wind_dir = 225;
                snprintf(d->loc, sizeof d->loc, "Bengaluru");
                d->nhours = 12;
                for (int i = 0; i < 12; i++) {
                    d->hours_c[i] = d->temp_c + 3.0f * sinf((i - 3) * 0.45f);
                    d->hours_cond[i] = i < 3 ? d->cond : i < 6 ? WX_PARTLY : i < 9 ? WX_CLOUDY : WX_NIGHT;
                }
                d->ndays = 5;
                static const int dc[5] = { WX_PARTLY, WX_RAIN, WX_STORM, WX_CLOUDY, WX_CLEAR };
                for (int i = 0; i < 5; i++) {
                    d->days[i].hi = d->hi_c + (float)((i * 7) % 5 - 2);
                    d->days[i].lo = d->lo_c + (float)((i * 3) % 4 - 1);
                    d->days[i].cond = i == 0 ? d->cond : dc[i];
                }
            }
        }
        if (inited >= 0) return &s_demo;
    }
#endif
    return w;
}

static void fmt_deg(char *o, int n, float c)
{
    float v = nos_prefs.units_metric ? c : c * 9.0f / 5.0f + 32.0f;
    snprintf(o, (size_t)n, "%d\xC2\xB0", (int)lroundf(v));
}

/* ------------------------------------------------------------- app state */
#define SEC_HOURS 466
#define SEC_DAYS  780
#define CONTENT_H 1140

static struct {
    const svc_weather_t *w;
    int frame;
    float t0;
    mscroll sc;
    int dragging, was_moving;
    float bar_alpha, bar_t;
    int hour_now, wday;
    int wrist_ok; float wrist_c;
    int connected;
} W;

static void weather_enter(void)
{
    memset(&W, 0, sizeof W);
    W.t0 = nos_t;
    msc_init(&W.sc, 0, CONTENT_H - GFX_H, 0);
    W.w = wx_data();
}

static void weather_tick(float dt)
{
    W.w = wx_data();
    W.frame++;
    if (msc_step(&W.sc, dt)) W.bar_t = nos_t;
    float target = nos_t - W.bar_t < 0.6f ? 1.0f : 0.0f;
    W.bar_alpha += (target - W.bar_alpha) * m_clamp(dt * (target > W.bar_alpha ? 10.0f : 4.0f), 0, 1);
    if (W.bar_alpha < 0.03f) W.bar_alpha = 0;
    W.bar_alpha = floorf(W.bar_alpha * 16.0f + 0.5f) / 16.0f;
    hal_datetime dtm;
    hal_get_datetime(&dtm);
    W.hour_now = dtm.hour;
    W.wday = dtm.weekday;
    hal_env e;
    W.wrist_ok = (hal_ext_present() & EXT_ENV) && hal_env_get(&e) && e.valid;
    W.wrist_c = W.wrist_ok ? e.temp_c : 0;
    const svc_sys_t *s = svc_sys();
    W.connected = s->phone_connected || s->wifi_up;
}

static float build(float delay, float dur) { return m_clamp((nos_t - W.t0 - delay) / dur, 0, 1); }

static void draw_now(gfx_fb *fb, const svc_weather_t *w, float scroll)
{
    char t[40];
    ui_title(fb, w->loc[0] ? w->loc : "WEATHER", scroll);
    float g = build(0, 0.36f);
    wx_icon(fb, w->cond, 142, 186, 1.0f, g, W.frame);
    fmt_deg(t, sizeof t, w->temp_c);
    dot_style st = { DOT_FONT_TALL, 9.0f, 7.2f, C_WHITE, 0, 1, 0 };
    dot_text_anim(fb, &st, 228, 146, t, 0, build(0.05f, 0.34f), DOT_REVEAL_RANDOM, 21u);
    ui_text_fit(fb, UI_F_BODY, UI_CX, 290, w->text, C_PRI, UI_CENTER, 320);
    char hi[8], lo[8];
    fmt_deg(hi, sizeof hi, w->hi_c); fmt_deg(lo, sizeof lo, w->lo_c);
    snprintf(t, sizeof t, "H %s   L %s", hi, lo);
    ui_label(fb, UI_CX, 324, t, C_SEC, UI_CENTER);
    /* three facts */
    char v[3][16];
    fmt_deg(v[0], sizeof v[0], w->feels_c);
    snprintf(v[1], sizeof v[1], "%d%%", w->humidity);
    snprintf(v[2], sizeof v[2], "%d", (int)lroundf(nos_prefs.units_metric ? w->wind_kph : w->wind_kph * 0.621f));
    static const char *cap[3] = { "FEELS", "HUMID", "WIND" };
    for (int i = 0; i < 3; i++) {
        int x = UI_CX + (i - 1) * 104;
        ui_label(fb, x, 366, v[i], C_PRI, UI_CENTER);
        ui_label(fb, x, 390, i == 2 ? (nos_prefs.units_metric ? "KM/H" : "MPH") : cap[i], C_DIS, UI_CENTER);
    }
    if (W.wrist_ok) {
        fmt_deg(v[0], sizeof v[0], W.wrist_c);
        snprintf(t, sizeof t, "WRIST %s", v[0]);
        ui_label(fb, UI_CX, 428, t, C_DIS, UI_CENTER);
    }
}

static void draw_hours(gfx_fb *fb, const svc_weather_t *w)
{
    int y0 = SEC_HOURS;
    ui_label(fb, UI_CX, y0 + 58, "NEXT 12 HOURS", C_SEC, UI_CENTER);
    int n = w->nhours < 12 ? w->nhours : 12;
    if (n <= 0) { ui_state(fb, UI_CX, y0 + 140, "NO HOURLY DATA", C_DIS); return; }
    for (int i = 0; i < n; i++) {
        int row = i / 6, col = i % 6;
        int x = UI_CX + (int)((col - 2.5f) * 60), y = y0 + 96 + row * 112;
        if (!rows_hit(fb, (float)y - 20, (float)y + 92)) continue;
        char t[8];
        snprintf(t, sizeof t, "%02d", (W.hour_now + 1 + i) % 24);
        ui_label(fb, x, y, t, C_DIS, UI_CENTER);
        dot_icon(fb, static_icon(w->hours_cond[i]), (float)x, (float)y + 30, 2.6f, 2.1f, C_PRI);
        fmt_deg(t, sizeof t, w->hours_c[i]);
        ui_label(fb, x, y + 76, t, C_PRI, UI_CENTER);
    }
}

static void draw_days(gfx_fb *fb, const svc_weather_t *w)
{
    int y0 = SEC_DAYS;
    ui_label(fb, UI_CX, y0 + 30, "5 DAYS", C_SEC, UI_CENTER);
    int n = w->ndays < 5 ? w->ndays : 5;
    if (n <= 0) { ui_state(fb, UI_CX, y0 + 100, "NO FORECAST", C_DIS); return; }
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < n; i++) {
        if (w->days[i].lo < lo) lo = w->days[i].lo;
        if (w->days[i].hi > hi) hi = w->days[i].hi;
    }
    if (hi - lo < 1) hi = lo + 1;
    const int NSEG = 10;
    for (int i = 0; i < n; i++) {
        int y = y0 + 74 + i * 56;
        if (!rows_hit(fb, (float)y - 24, (float)y + 24)) continue;
        ui_label(fb, 92, y + 7, i == 0 ? "TODAY" : ui_weekday3(W.wday + i), i == 0 ? C_PRI : C_SEC, UI_LEFT);
        dot_icon(fb, static_icon(w->days[i].cond), 186, (float)y, 2.2f, 1.8f, C_PRI);
        char t[8];
        fmt_deg(t, sizeof t, w->days[i].lo);
        ui_label(fb, 246, y + 7, t, C_DIS, UI_RIGHT);
        /* segments lit between this day's low and high on the week's scale */
        float a = (w->days[i].lo - lo) / (hi - lo) * NSEG, b = (w->days[i].hi - lo) / (hi - lo) * NSEG;
        const float bx = 256, bw = 76, gap = 3, sw = (bw - gap * (NSEG - 1)) / NSEG;
        for (int sgi = 0; sgi < NSEG; sgi++) {
            int on = sgi + 0.5f >= a && sgi + 0.5f <= b + 0.01f;
            if (!on && sgi >= (int)a && sgi < (int)ceilf(b)) on = 1;
            gfx_fill_rect(fb, (int)lroundf(bx + sgi * (sw + gap)), y - 3, (int)lroundf(sw), 6, on ? C_PRI : C_LINE2);
        }
        fmt_deg(t, sizeof t, w->days[i].hi);
        ui_label(fb, 342, y + 7, t, C_PRI, UI_LEFT);
    }
}

static void draw_empty(gfx_fb *fb)
{
    ui_title(fb, "WEATHER", 0);
    float g = build(0, 0.36f);
    dot_icon_anim(fb, DOT_ICON_CLOUD, UI_CX, 184, 6.0f, 4.8f, C_DIS, g, DOT_REVEAL_RANDOM);
    ui_state(fb, UI_CX, 284, W.connected ? "UPDATING" : "NO WEATHER", C_SEC);
    if (!W.connected)
        ui_text_wrap(fb, UI_F_BODY, UI_CX, 326, 290, 30, 2, "Connect your phone or Wi-Fi", C_DIS, UI_CENTER);
    if (W.wrist_ok) {
        char v[8], t[24];
        fmt_deg(v, sizeof v, W.wrist_c);
        snprintf(t, sizeof t, "WRIST %s", v);
        ui_label(fb, UI_CX, 404, t, C_DIS, UI_CENTER);
    }
}

static void weather_draw(gfx_fb *fb)
{
    const svc_weather_t *w = W.w;
    if (!w || !w->valid) { draw_empty(fb); return; }
    float off = floorf(W.sc.x + 0.5f);
    gfx_fb sub;
    if (nos_subview(fb, &sub, 0, 0, GFX_W, GFX_H, 0, -(int)off)) {
        if (off < SEC_HOURS) draw_now(&sub, w, off);
        if (off + GFX_H > SEC_HOURS && off < SEC_DAYS) draw_hours(&sub, w);
        if (off + GFX_H > SEC_DAYS) draw_days(&sub, w);
    }
    ui_scrollbar(fb, W.sc.x, CONTENT_H, GFX_H, W.bar_alpha);
}

static int weather_input(const nos_input *in)
{
    if (!W.w || !W.w->valid) return 0;
    switch (in->type) {
    case IN_DOWN:
        W.was_moving = W.sc.moving;
        msc_grab(&W.sc);
        return 1;
    case IN_DRAG_START:
        if (in->axis != AXIS_Y) { msc_release(&W.sc, 0); return 0; }
        W.dragging = 1;
        /* fall through */
    case IN_DRAG:
        if (!W.dragging) return 0;
        msc_drag(&W.sc, in->dy);
        W.bar_t = nos_t;
        return 1;
    case IN_DRAG_END:
        if (!W.dragging) return 0;
        W.dragging = 0;
        msc_release(&W.sc, -in->vy);
        return 1;
    case IN_TAP: case IN_UP:
        if (!W.dragging) msc_release(&W.sc, 0);
        return 1;
    default: return 0;
    }
}

const nos_screen scr_weather = { "WEATHER", weather_enter, 0, weather_tick, weather_draw, weather_input, 0 };

/* ------------------------------------------------------------- widget */
static struct { int frame; float t0; int wrist_ok; } G;

static void wg_tick(float dt, int visible)
{
    if (visible) G.frame++;
}
static void wg_shown(void) { G.t0 = nos_t; }
static float wg_build(float delay) { return m_clamp((nos_t - G.t0 - delay) / 0.34f, 0, 1); }

static void wg_draw(gfx_fb *fb)
{
    const svc_weather_t *w = wx_data();
    if (!w->valid) {
        ui_label(fb, UI_CX, 96, "WEATHER", C_SEC, UI_CENTER);
        dot_icon(fb, DOT_ICON_CLOUD, UI_CX, 200, 6.0f, 4.8f, C_DIS);
        ui_state(fb, UI_CX, 300, "NO WEATHER", C_SEC);
        ui_label(fb, UI_CX, 336, "CONNECT YOUR PHONE", C_DIS, UI_CENTER);
        return;
    }
    char t[40];
    ui_label(fb, UI_CX, 96, w->loc[0] ? w->loc : "WEATHER", C_SEC, UI_CENTER);
    wx_icon(fb, w->cond, UI_CX, 178, 0.95f, wg_build(0), G.frame);
    fmt_deg(t, sizeof t, w->temp_c);
    dot_style st = { DOT_FONT_TALL, 8.0f, 6.4f, C_WHITE, 0, 1, 0 };
    dot_text_anim(fb, &st, UI_CX + 8, 254, t, 1, wg_build(0.05f), DOT_REVEAL_RANDOM, 17u);
    char hi[8], lo[8];
    fmt_deg(hi, sizeof hi, w->hi_c); fmt_deg(lo, sizeof lo, w->lo_c);
    snprintf(t, sizeof t, "H %s   L %s", hi, lo);
    ui_label(fb, UI_CX, 362, t, C_PRI, UI_CENTER);
    ui_label(fb, UI_CX, 390, w->text, C_DIS, UI_CENTER);
}

static void wg_tap(int x, int y) { nos_open(&scr_weather, x, y); }

const nos_widget wg_weather = { "WEATHER", wg_tick, wg_draw, wg_tap, wg_shown };
