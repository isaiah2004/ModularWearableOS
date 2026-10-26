/* out_baro.c — Barometer / altimeter / local climate (BME280 on the strap), outdoor team.
 *
 * Three pages in a horizontal pager (SCR_HDRAG; back swipe from the left edge):
 *   1 PRESSURE  hPa in dot-matrix, 3 h trend, 24 h column chart (solid columns on
 *               one baseline) whose "now" column wears the screen's one red cap; when
 *               it falls > 3 hPa in 3 h a red dot + [ STORM ] line fades in under the
 *               chart (the trend line is kept) and takes the red.
 *   2 ALTITUDE  metres, sea-level reference, CALIBRATE pushes the sea-level picker
 *               (scr_baro_cal: drag / - + with auto-repeat, Use GPS when the GPS has a
 *               3D fix, Set; back = cancel).
 *   3 LOCAL     temperature + humidity, labelled ON WRIST.
 * Sensors are read in tick() into statics; draw() only reads them. Live values change
 * at most once a second, with a digit morph.
 *
 * Motion (docs/MOTION30.html): the pager is a pixel spring (1 px snap, no creep); the
 * chart's columns grow up from the baseline left to right (two a frame, 10 frames each,
 * whole pixels, the red cap riding the newest), the humidity bar fills whole dots over
 * 7 frames; values morph over whole frames and flip like an odometer while they count;
 * fades use the 16 perceptual ink levels.
 *
 * Sim preview knob: NOS_STORM=1 (sim builds only) forces a -3.6 hPa / 3 h trend. */
#include "nos.h"
#include "diag_log.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include "scroll_diag.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define NPAGE  3
#define PAGE_W 466.0f
#define NHIST  144
#define NCOL   24
/* the 24 h column chart: 24 solid columns 6 px wide, 4 px apart, on one baseline */
#define CW     6
#define CGAP   4
#define CPITCH 10.0f
#define CBASE  342              /* baseline row (the columns stand on it) */
#define CMINH  6                /* a reading always shows (the lowest hour) */
#define CMAXH  66
#define CGROW  10               /* entry: each column grows over 10 frames (EASE_STEADY) ... */
#define CSTAG  2                /* ... two columns a frame, left to right: 20 frames in all */

/* ------------------------------------------------------------- pager */
typedef struct { mspring sp; int n, page, dragging; float x0; } pager;
static void pg_init(pager *p, int n, int page)
{
    memset(p, 0, sizeof *p);
    p->n = n; p->page = page;
    msp_init_px(&p->sp, page * PAGE_W, SPRING_PAGE);   /* pixels: ends with a 1 px snap */
}
static int pg_input(pager *p, const nos_input *in)
{
    if (in->type == IN_CANCEL) { p->dragging = 0; p->sp.v = 0; msp_to(&p->sp, p->page * PAGE_W); return 1; }

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

/* ------------------------------------------------------------- morphing value
 * A lone change morphs (whole frames, starting one step in so the frame of the change
 * already shows it); a value that changes again before its morph ends is counting:
 * it flips each time like an odometer (R8) instead of restarting half-done morphs. */
typedef struct { char cur[16], prev[16]; nos_time_t t0, tc; float dur; } mval;
static void mv_set(mval *m, const char *s, float dur)
{
    if (!strcmp(m->cur, s)) return;
    int counting = nos_t - m->tc < dur;
    if (strlen(m->cur) == strlen(s) && m->cur[0] && !counting) { strcpy(m->prev, m->cur); m->t0 = nos_t - M_FRAMES(1); }
    else { strcpy(m->prev, s); m->t0 = -10; }
    m->dur = dur; m->tc = nos_t;
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
    dot_icon(fb, icon, cx, cy, 5.0f, 3.0f, pressed ? C_BG : C_PRI);   /* 2 px gaps: dots, not a line */
}

/* ------------------------------------------------------------- state */
typedef struct { int shown, init, pend; nos_time_t t; } plug_t;   /* see plug_tick() */

static struct {
    int present, valid;      /* shown: flip after the unplug fade */
    plug_t pl;
    hal_env env;
    float *hist; int nhist;
    nos_time_t hist_t;            /* nos_t of the last history read */
    nos_time_t val_t;             /* nos_t of the last displayed-value refresh (1 Hz) */
    uint8_t cols[NCOL];      /* column heights 0..255 of the chart (0 = no reading) */
    int ncols;
    int colh[NCOL];          /* column heights shown, px (prepared in tick) */
    float lo, hi;            /* chart range */
    float d3h; int have3h;   /* change over 3 h */
    int storm; nos_time_t storm_t; int hist_read;
    float sea;               /* sea-level reference shown (stable, see s_sea) */
    pager pg;
    nos_time_t t0;                /* entry build start */
    mval m_hpa, m_alt, m_temp;
    int pressed;             /* control id under the finger */
    nos_time_t press_until;       /* pressed state shows >= 3 frames */
    nos_time_t saved_t;
    int inited;
} B;

enum { P_NONE = 0, P_CAL, P_MINUS, P_PLUS, P_GPS, P_SET };

/* The reference the altimeter uses. It is derived from pressure + altitude, which
 * wobbles in the last digit, so it only follows a real change (> 0.35 hPa) or a Set. */
static float s_sea;

#define MORPH_S M_FRAMES(8)      /* live value digit morph */
#define FILL_N  7                /* humidity entry fill: whole dots over 7 frames */
/* rows of an entry fill shown k whole frames after the build began (frame 0 shows 1) */
static int fill_rows(nos_time_t t0, int n)
{
    int k = m_frame_k(t0);
    if (k < 0) return 0;
    return k + 1 >= FILL_N ? n : (n * (k + 1) + FILL_N - 1) / FILL_N;
}
static uint16_t fadec(uint16_t c, float k) { return k >= 1.0f ? c : ui_mix(C_BG, c, k); }
/* a label whose text already has the right case (units keep theirs) */
static int lbl(gfx_fb *fb, int x, int y, const char *s, uint16_t col, int align)
{
    return ui_text(fb, UI_F_LABEL, x, y, s, col, align, 2);
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
        float f = m_clamp((v - lo) / (hi - lo), 0, 1);
        B.cols[NCOL - nh + c] = (uint8_t)(1 + (int)floorf(f * 254.0f + 0.5f));   /* >= 1: shown */
    }
    /* 3 h trend: 18 samples of 10 min */
    B.have3h = B.nhist > 18;
    if (B.have3h) {
        float now = B.valid ? B.env.pressure_hpa : B.hist[B.nhist - 1];
        B.d3h = now - B.hist[B.nhist - 1 - 18];
    } else B.d3h = 0;
    if (storm_knob()) { B.have3h = 1; B.d3h = -3.6f; }
    int storm = B.have3h && B.d3h < -3.0f;
    if (storm != B.storm) { B.storm = storm; B.storm_t = B.hist_read ? nos_t - M_FRAMES(1) : -10; }   /* fades unless on entry; one step in */
    B.hist_read = 1;
}

static void fmt1(char *o, int n, float v) { snprintf(o, (size_t)n, "%.1f", (double)(floorf(v * 10.0f + 0.5f) / 10.0f)); }

static void refresh_values(void)
{
    char t[16];
    float d = sea_from(B.env.pressure_hpa, B.env.altitude_m);
    if (s_sea <= 0 || fabsf(d - s_sea) > 0.35f) s_sea = d;
    B.sea = s_sea;
    fmt1(t, sizeof t, B.env.pressure_hpa); mv_set(&B.m_hpa, t, MORPH_S);
    snprintf(t, sizeof t, "%d", (int)lroundf(B.env.altitude_m)); mv_set(&B.m_alt, t, MORPH_S);
    float tc = nos_prefs.units_metric ? B.env.temp_c : B.env.temp_c * 9.0f / 5.0f + 32.0f;
    snprintf(t, sizeof t, "%.1f\xB0", (double)(floorf(tc * 10.0f + 0.5f) / 10.0f)); mv_set(&B.m_temp, t, MORPH_S);
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
    B.t0 = nos_enter_t();
    B.hist_t = -100; B.val_t = -100;
    B.saved_t = -100;
    B.storm_t = -10;
}

/* A module unplugged (or plugged back) while the app is open, the rule shared with the
 * connect team: the old content steps to half, then off (2 frames at 30 fps), then the
 * new page builds in. The shown state only flips after the fade. */
#define PLUG_A 0.025f            /* frame 1 of the fade: old content at half */
#define PLUG_B 0.058f            /* frame 2: off; the flip happens on the next tick */
#define PLUG_STEP (1.0f / 30.0f) /* the new page's build starts one step in */
/* returns 1 on the tick the shown state flips */
static int plug_tick(plug_t *p, int real)
{
    if (!p->init) { p->init = 1; p->shown = real; p->pend = 0; p->t = -10; return 0; }
    if (!p->pend) { if (real != p->shown) { p->pend = 1; p->t = nos_t; } return 0; }
    if (nos_t - p->t < PLUG_B) return 0;
    p->pend = 0;
    if (real == p->shown) return 0;   /* back before the fade ended */
    p->shown = real;
    return 1;
}
/* 0 draw normally, 1 draw at half ink, 2 draw nothing; records the fade's damage */
static int plug_phase(gfx_fb *fb, const plug_t *p)
{
    if (!p->pend) return 0;
    int ph = nos_t - p->t < PLUG_A ? 1 : 2;
    if (fb->rec) gfx_rec_cmd(fb, 0, 0, GFX_W, GFX_H, gfx_hash_u(0x9106u, (unsigned)ph));
    return ph;
}

/* The chart's shown column heights. Entry (and a re-plug): the columns grow up from the
 * baseline left to right, two a frame, each over CGROW frames on EASE_STEADY (frame 0
 * already shows the first step): a 0.67 s sweep in whole frames, whole pixels. After it,
 * a new reading moves its column at most 2 px a frame (never a jump). */
static void chart_tick(void)
{
    int k = m_frame_k(B.t0);
    for (int i = 0; i < NCOL; i++) {
        int tgt = B.cols[i] ? CMINH + (int)floorf((float)(B.cols[i] - 1) / 254.0f * (float)(CMAXH - CMINH) + 0.5f) : 0;
        int j = i - (NCOL - B.ncols);                /* from the oldest hour shown */
        int ki = k + 1 - (j > 0 ? j : 0) / CSTAG;
        if (ki < CGROW) {
            float g = ki <= 0 ? 0.0f : bezier_ease(&EASE_STEADY, (float)ki / (float)CGROW);
            B.colh[i] = (int)floorf((float)tgt * g + 0.5f);
        } else if (B.colh[i] < tgt) B.colh[i] = B.colh[i] + 2 < tgt ? B.colh[i] + 2 : tgt;
        else if (B.colh[i] > tgt) B.colh[i] = B.colh[i] - 2 > tgt ? B.colh[i] - 2 : tgt;
    }
}

static void baro_tick(float dt)
{
    if (plug_tick(&B.pl, (hal_ext_present() & EXT_ENV) != 0)) B.t0 = nos_t - PLUG_STEP;
    B.present = B.pl.shown;
    if (B.pl.pend) return;                         /* the old content is fading */
    B.valid = B.present && hal_env_get(&B.env) && B.env.valid && isfinite(B.env.pressure_hpa) &&
              B.env.pressure_hpa > 0 && isfinite(B.env.temp_c) && isfinite(B.env.altitude_m);
    if (B.valid && nos_t - B.hist_t > 5.0f) { read_history(); B.hist_t = nos_t; }
    if (B.valid && nos_t - B.val_t >= 1.0f) { refresh_values(); B.val_t = nos_t; }
    pg_tick(&B.pg, dt);
    if (B.pressed && nos_t > B.press_until && B.press_until > 0) { B.pressed = 0; B.press_until = 0; }
    chart_tick();
#ifdef GFX_COST_MODEL
    /* motion audit: the pager, the chart's first / newest / tallest column (px), the
     * storm line's ink */
    if (B.valid) {
        nos_motion_probe("baro.pager", B.pg.dragging ? MC_FINGER : MC_MOMENTUM, pg_pos(&B.pg));
        int cmax = 0;
        for (int i = 0; i < NCOL; i++) if (B.colh[i] > cmax) cmax = B.colh[i];
        if (B.pg.page == 0) {
            nos_motion_probe("baro.chart", MC_COMPONENT, (float)cmax);
            nos_motion_probe("baro.chart.col0", MC_COMPONENT, (float)B.colh[NCOL - B.ncols < NCOL ? NCOL - B.ncols : 0]);
            nos_motion_probe("baro.chart.now", MC_COMPONENT, (float)B.colh[NCOL - 1]);
        }
        float su = (nos_t - B.storm_t) / M_FRAMES(6);
        nos_motion_probe("baro.storm.ink", MC_FADE, m_ink(B.storm ? su : 1.0f - su) * 100.0f);
    }
#endif
}

/* ------------------------------------------------------------- pages */
static int S_NOTITLE;      /* draw: the steady title is drawn outside the fading body */
static void btitle(gfx_fb *fb) { if (!S_NOTITLE) ui_title(fb, "BAROMETER", 0); }
static float build_of(void) { return m_clamp((nos_t - B.t0) / UI_ENTRY_S, 0, 1); }

static void page_pressure(gfx_fb *fb)
{
    btitle(fb);
    float b = build_of();
    /* trend row */
    char t[40];
    int icon; const char *word;
    if (!B.have3h) { icon = 0; word = "TREND IN 3 h"; }
    else if (B.d3h > 1.0f) { icon = DOT_ICON_ARROW_UP; word = "RISING"; }
    else if (B.d3h < -1.0f) { icon = DOT_ICON_ARROW_DOWN; word = "FALLING"; }
    else { icon = DOT_ICON_ARROW_RIGHT; word = "STEADY"; }
    /* storm: a red dot + [ STORM ] under the chart (the screen's one red), 16 px
     * below the axis labels, fading in / out over 0.2 s; the trend that triggered
     * it stays readable up top */
    float su = (nos_t - B.storm_t) / M_FRAMES(6);
    float sk = m_ink(B.storm ? su : 1.0f - su);
    if (sk > 0) {
        int w = ui_text_width(UI_F_LABEL, "[ STORM ]", 2) + 20;
        dot_disc(fb, UI_CX - w / 2 + 4, 402 - 6, 9, fadec(C_RED, sk));
        ui_label(fb, UI_CX - w / 2 + 20, 402, "[ STORM ]", fadec(C_PRI, sk), UI_LEFT);
    }
    if (B.have3h) snprintf(t, sizeof t, "%s %+.1f hPa / 3 h", word, (double)B.d3h);
    else snprintf(t, sizeof t, "%s", word);
    int tw = ui_text_width(UI_F_LABEL, t, 2) + (icon ? 24 : 0);
    int tx = UI_CX - tw / 2;
    /* the trend sits midway between the title and the hero */
    if (icon) dot_icon(fb, icon, tx + 8, 108, 1.8f, 1.5f, C_PRI);
    lbl(fb, tx + (icon ? 24 : 0), 114, t, C_SEC, UI_LEFT);
    /* hero */
    dot_style st = { DOT_FONT_TALL, UI_HEROW_PITCH, UI_HEROW_PITCH * 0.8f, C_WHITE, 0, 1, 1 };
    mv_draw(fb, &st, UI_CX, 142, &B.m_hpa, b);
    ui_text(fb, UI_F_LABEL, UI_CX, 242, "hPa", C_SEC, UI_CENTER, 2);
    /* 24 h chart */
    float x0 = UI_CX - NCOL * CPITCH * 0.5f;
    if (B.ncols < 2) {
        ui_state(fb, UI_CX, 320, "COLLECTING", C_DIS);
        return;
    }
    /* solid columns on one baseline, one rect each (only a column that changed repaints);
     * the last hour (now) in white, the rest grey; the shared baseline runs under all */
    gfx_fill_rect(fb, (int)x0, CBASE, NCOL * (int)CPITCH - CGAP, 2, C_LINE2);
    int hn = 0;
    for (int i = 0; i < NCOL; i++) {
        int h = B.colh[i];
        if (h <= 0) continue;
        int cx = (int)x0 + i * (int)CPITCH;
        gfx_fill_rect(fb, cx, CBASE - h, CW, h, i == NCOL - 1 ? C_PRI : C_SEC);
        if (i == NCOL - 1) hn = h;
    }
    /* "now": a red cap on the newest column, riding its top as it grows (white while a
     * storm warning holds the red) */
    if (hn > 0) {
        int cx = (int)x0 + (NCOL - 1) * (int)CPITCH, ch = hn < CW ? hn : CW;
        gfx_fill_rect(fb, cx, CBASE - hn, CW, ch, ui_mix(C_RED, C_WHITE, sk));
    }
    ui_label(fb, (int)x0, 370, "-24H", C_DIS, UI_LEFT);
    ui_label(fb, (int)(x0 + NCOL * CPITCH - CGAP), 370, "NOW", C_DIS, UI_RIGHT);
}

static void page_altitude(gfx_fb *fb)
{
    ui_title(fb, "ALTITUDE", 0);
    float b = build_of();
    dot_style st = { DOT_FONT_TALL, UI_HERO_PITCH, UI_HERO_PITCH * 0.8f, C_WHITE, 0, 1, 1 };
    mv_draw(fb, &st, UI_CX, 118, &B.m_alt, b);
    lbl(fb, UI_CX, 228, "m", C_SEC, UI_CENTER);
    char t[40];
    /* [ SAVED ] holds 1.5 s, then fades through to the reference: out 56-19 %, in 19-56-100 % */
    int f = m_frame_k(B.saved_t + 1.5f);
    if (f < 2) ui_state(fb, UI_CX, 284, "SAVED", fadec(C_PRI, f < 0 ? 1.0f : m_ink(1.0f - (float)(f + 1) / 3.0f)));
    else {
        snprintf(t, sizeof t, "SEA LEVEL %.1f", (double)(floorf(B.sea * 10 + 0.5f) / 10));
        ui_label_unit(fb, UI_CX, 284, t, "hPa", fadec(C_DIS, f >= 4 ? 1.0f : m_ink((float)(f - 1) / 3.0f)), UI_CENTER);
    }
    btn(fb, UI_CX, UI_BTN_Y, UI_BTN_W, "Calibrate", 0, B.pressed == P_CAL, 1);
}

static int humidity_available(void)
{
    return B.valid && B.env.humidity_valid && isfinite(B.env.humidity);
}
static void page_local(gfx_fb *fb)
{
    ui_title(fb, "LOCAL", 0);
    float b = build_of();
    /* the number, its degree ring and the unit letter hung top-right (as Weather's
     * hero) are centred as one group */
    dot_style st = { DOT_FONT_TALL, UI_HEROW_PITCH, UI_HEROW_PITCH * 0.8f, C_WHITE, 0, 1, 1 };
    const char *unit = nos_prefs.units_metric ? "C" : "F";
    int wh = dot_text_width(&st, B.m_temp.cur), wu = 6 + ui_text_width(UI_F_LABEL, unit, 2);
    float hx = UI_CX - wu * 0.5f;
    mv_draw(fb, &st, hx, 118, &B.m_temp, b);
    if (b >= 0.5f) ui_label(fb, (int)(hx + wh * 0.5f + 6), 118 + 14, unit, C_SEC, UI_LEFT);
    ui_label(fb, UI_CX, 222, "TEMPERATURE", C_SEC, UI_CENTER);
    /* humidity as 20 whole dots (5% each) */
    if (!humidity_available()) {
        ui_label(fb, UI_CX, 326, "HUMIDITY UNAVAILABLE", C_DIS, UI_CENTER);
        ui_label(fb, UI_CX, 386, "ON WRIST", C_DIS, UI_CENTER);
        return;
    }
    float h = m_clamp(B.env.humidity, 0, 100);
    const float pitch = 14.0f;
    int hl = (int)floorf(h / 5.0f + 0.5f), hr = fill_rows(B.t0, 20);      /* whole dots on the chart's beat */
    dot_bar(fb, UI_CX - 9.5f * pitch, 284, 20, pitch, 8.0f, (float)(hl < hr ? hl : hr), C_PRI, C_LINE2);
    char t[24];
    snprintf(t, sizeof t, "HUMIDITY %d%%", (int)lroundf(h));
    ui_label(fb, UI_CX, 326, t, C_SEC, UI_CENTER);
    ui_label(fb, UI_CX, 386, "ON WRIST", C_DIS, UI_CENTER);
}

/* ------------------------------------------------------------- draw / input */
static void baro_draw_body(gfx_fb *fb);
static void baro_draw(gfx_fb *fb)
{
    int ph = plug_phase(fb, &B.pl);
    /* BAROMETER on both sides of the swap (page 1, or the absent pages): it stays put */
    int steady = ph && B.pg.page == 0 && !B.pg.sp.moving;
    if (steady) ui_title(fb, "BAROMETER", 0);
    if (ph == 2) return;
    int d0 = fb->dim;
    if (ph == 1) nos_ink_dim(fb, 0.5f);
    S_NOTITLE = steady;
    baro_draw_body(fb);
    S_NOTITLE = 0;
    fb->dim = d0;
}
static void baro_draw_body(gfx_fb *fb)
{
    if (!B.present) {
        btitle(fb);
        ui_absent(fb, DOT_ICON_GAUGE, "Not connected", "Add the climate sensor for pressure",
                  "CLIMATE MODULE", build_of());
        return;
    }
    if (!B.valid) {
        btitle(fb);
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
    /* motion blur on the pages as they slide (nos.h), not on the page dots */
    static nos_mb_track mb;
    float mbs = nos_mblur_step(nos_mb_track_step(&mb, pos), B.pg.dragging);
    if (mbs > 0 && mbs < PAGE_W * 0.5f) nos_mblur_h(fb, 0, GFX_W, mbs);
    ui_page_dots(fb, UI_CX, UI_DOTS_Y, NPAGE, pos / PAGE_W);
}

extern const nos_screen scr_baro_cal;

static int baro_input(const nos_input *in)
{
    if (in->type == IN_CANCEL) { B.pressed = 0; B.press_until = 0; pg_input(&B.pg, in); return 1; }

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
    nos_time_t t0;
    int pressed; nos_time_t press_t0, press_until, rep_t;
    int repeated;
    int drag; float last_dy, acc;
    int gps_ok; nos_time_t gps_t; float gps_alt;   /* gps_t: when the fix arrived (enable fade) */
    hal_env env;
    char alt_txt[24];
} K;

static void cal_show(void)
{
    char t[16];
    fmt1(t, sizeof t, K.v);
    mv_set(&K.m, t, M_FRAMES(4));
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
    float sea = s_sea > 0 ? s_sea : sea_from(K.env.pressure_hpa, K.env.altitude_m);   /* what the page showed */
    K.v = floorf((sea > 800 ? sea : 1013.25f) * 10.0f + 0.5f) / 10.0f;
    cal_show();
    K.m.t0 = -10;
    snprintf(K.alt_txt, sizeof K.alt_txt, "ALTITUDE %d", (int)lroundf(alt_from(K.env.pressure_hpa, K.v)));
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
    if (ok && !K.gps_ok) K.gps_t = nos_t - M_FRAMES(1);   /* the fade starts one step in */
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
    snprintf(K.alt_txt, sizeof K.alt_txt, "ALTITUDE %d", (int)lroundf(alt_from(K.env.pressure_hpa, K.v)));
#ifdef GFX_COST_MODEL
    {
        float gk = K.gps_ok ? m_clamp((nos_t - K.gps_t) / M_FRAMES(4), 0, 1) : 0;
        nos_motion_probe("baro.cal.gps.ink", MC_FADE, floorf(gk * 8.0f + 0.5f) / 8.0f * 100.0f);
    }
#endif
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
    ui_label_unit(fb, UI_CX, 316, K.alt_txt, "m", C_DIS, UI_CENTER);
    /* both buttons always: Use GPS waits disabled for a 3D fix, then fades in place */
    float gk = K.gps_ok ? m_clamp((nos_t - K.gps_t) / M_FRAMES(4), 0, 1) : 0;
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
    if (in->type == IN_CANCEL) { K.pressed = K.drag = 0; K.press_until = 0; return 1; }

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
            uint32_t request = diag_action_request(DIAG_DOMAIN_SENSOR, DIAG_ACT_SET, EXT_ENV);
            diag_action_context(request, (int32_t)diag_log_name_id("SEA LEVEL"), 0);
            hal_env_set_sea_level(K.v);
            diag_action_result(request, DIAG_RESULT_PENDING, 1); /* void HAL setter has no save ACK */
            s_sea = K.v; B.sea = K.v;
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

/* UI-task diagnostic: return the actual pager state without ticking/drawing. */
void out_baro_scroll_snapshot(nos_scroll_snapshot *s)
{
    *s = (nos_scroll_snapshot){ 4, 1, B.pg.page, NPAGE, B.pg.dragging, B.pg.sp.x, (NPAGE - 1) * PAGE_W };
}
