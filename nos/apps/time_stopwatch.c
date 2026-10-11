/* time_stopwatch.c — Stopwatch. Team: time.
 *
 * Big tabular dot digits mm:ss with smaller tenths, a red second-hand dot running
 * around the bezel once per second (the only thing that moves every frame: its damage
 * box is a few dozen pixels), Start/Stop + Lap/Reset pills and a lap list (live
 * current lap, best lap white, worst grey).
 *
 * State lives in svc (svc_sw_*). While svc is still the link stub, a local fallback
 * keeps time from hal_millis(); time_sw()/time_sw_cmd() hide which one runs (the home
 * widget in time_timer.c reads time_sw() too). */
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

/* from time_timer.c (exact-position trig and dot ring) */
float time_sin(float x);
float time_cos(float x);
void time_ring(gfx_fb *fb, float cx, float cy, float r, int n, float d, float lit, uint16_t on, uint16_t off);

static float qz(float v, float n) { return floorf(v * n + 0.5f) / n; }

/* ============================================================ state */
static svc_stopwatch_t SL;          /* local fallback */
static uint32_t SL_base;            /* hal_millis() at elapsed 0 while running */
static int s_svc = -1, s_probe;     /* -1 unknown, 0 stub, 1 real */

const svc_stopwatch_t *time_sw(void)
{
    if (s_svc == 1) return svc_stopwatch();
    if (s_svc < 0 && !SL.running && !SL.elapsed_ms) return svc_stopwatch();
    if (SL.running) SL.elapsed_ms = hal_millis() - SL_base;
    return &SL;
}
enum { SW_START = 0, SW_STOP, SW_LAP, SW_RESET };
void time_sw_cmd(int c)
{
    switch (c) {
    case SW_START: svc_sw_start(); break;
    case SW_STOP:  svc_sw_stop();  break;
    case SW_LAP:   svc_sw_lap();   break;
    default:       svc_sw_reset(); break;
    }
    if (s_svc == 1) return;
    uint32_t now = hal_millis();
    if (SL.running) SL.elapsed_ms = now - SL_base;
    switch (c) {
    case SW_START: if (!SL.running) { SL_base = now - SL.elapsed_ms; SL.running = 1; } s_probe = 0; break;
    case SW_STOP:  SL.running = 0; break;
    case SW_LAP:   if (SL.nlaps < SVC_LAPS) SL.laps[SL.nlaps++] = SL.elapsed_ms; break;
    default:       memset(&SL, 0, sizeof SL); break;
    }
}
static void sw_poll(void)
{
    if (s_svc < 0 && SL.running) {
        if (svc_stopwatch()->running) { s_svc = 1; memset(&SL, 0, sizeof SL); }
        else if (++s_probe >= 3) s_svc = 0;
    }
}

/* ============================================ per-character digits with morphs */
#define DG_MAX 10
typedef struct { char cur[DG_MAX + 1], from[DG_MAX + 1]; float t0[DG_MAX]; } dgm;
static void dgm_set(dgm *m, const char *s, int morph)
{
    if (!morph || strlen(s) != strlen(m->cur)) {
        size_t n = strlen(s);
        if (n > DG_MAX) n = DG_MAX;
        memcpy(m->cur, s, n); m->cur[n] = 0;
        memcpy(m->from, s, n); m->from[n] = 0;
        for (int i = 0; i < DG_MAX; i++) m->t0[i] = -10;
        return;
    }
    for (int i = 0; s[i] && i < DG_MAX; i++)
        if (s[i] != m->cur[i]) { m->from[i] = m->cur[i]; m->cur[i] = s[i]; m->t0[i] = nos_t; }
}
static float dg_cw(const dot_style *st, char c) { char b[2] = { c, 0 }; return (float)dot_text_width(st, b); }
static float dg_width(const dot_style *st, const char *s)
{
    float w = 0; int n = 0;
    for (; s[n]; n++) w += dg_cw(st, s[n]);
    return n ? w + (n - 1) * st->pitch : 0;
}
static void dgm_draw(gfx_fb *fb, const dgm *m, const dot_style *st, float x, float y, float dur, float build)
{
    for (int i = 0; m->cur[i]; i++) {
        char a[2] = { m->from[i], 0 }, b[2] = { m->cur[i], 0 };
        float p = (nos_t - m->t0[i]) / dur;
        if (build < 1) dot_text_anim(fb, st, x, y, b, 0, build, DOT_REVEAL_RANDOM, (unsigned)(i * 11 + 3));
        else if (p < 1 && a[0] != b[0]) dot_text_morph(fb, st, x, y, a, b, 0, p);
        else dot_text(fb, st, x, y, b);
        x += dg_cw(st, m->cur[i]) + st->pitch;
    }
}

/* ============================================================ screen */
#define RING_R   211.0f
#define BTN_Y    350.0f
#define BTN_W    140.0f
#define BTN_DX   78.0f
#define LIST_Y0  194.0f
#define LIST_Y1  300.0f
#define ROW_H    36.0f
#define DIG_Y_SOLO 186.0f
#define DIG_Y_LAPS 100.0f

enum { BP_NONE = 0, BP_LEFT, BP_RIGHT };
static struct {
    float t0, build;
    int pressed;
    svc_stopwatch_t sw;          /* snapshot for this frame */
    dgm dig;
    char tenths[4];
    mspring lay;                 /* 0 = no laps (digits centred), 1 = laps shown */
    mspring ins;                 /* new-row slide-in offset, px */
    mscroll sc;
    int dragging;
    int best, worst;             /* lap numbers (1-based), 0 = none */
    char cur_lbl[16], cur_time[16];
} S;

static void fmt_lap(char *b, int n, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s >= 3600) snprintf(b, (size_t)n, "%u:%02u:%02u.%u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60), (unsigned)(ms / 100 % 10));
    else snprintf(b, (size_t)n, "%02u:%02u.%u", (unsigned)(s / 60), (unsigned)(s % 60), (unsigned)(ms / 100 % 10));
}
static uint32_t lap_ms(const svc_stopwatch_t *sw, int i)   /* i 1-based completed lap */
{
    uint32_t a = i >= 2 ? sw->laps[i - 2] : 0;
    return sw->laps[i - 1] - a;
}
static float list_max(void)
{
    float content = (S.sw.nlaps + 1) * ROW_H;
    float m = content - (LIST_Y1 - LIST_Y0);
    return m > 0 ? m : 0;
}

static void sw_snapshot(int morph)
{
    S.sw = *time_sw();
    uint32_t ms = S.sw.elapsed_ms, s = ms / 1000;
    char b[16];
    if (s >= 3600) snprintf(b, sizeof b, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    else snprintf(b, sizeof b, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    dgm_set(&S.dig, b, morph);
    snprintf(S.tenths, sizeof S.tenths, ".%u", (unsigned)(ms / 100 % 10));
    int n = S.sw.nlaps;
    snprintf(S.cur_lbl, sizeof S.cur_lbl, "LAP %02d", n + 1);
    fmt_lap(S.cur_time, sizeof S.cur_time, ms - (n ? S.sw.laps[n - 1] : 0));
    S.best = S.worst = 0;
    if (n >= 2) {
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (int i = 1; i <= n; i++) {
            uint32_t l = lap_ms(&S.sw, i);
            if (l < lo) { lo = l; S.best = i; }
            if (l >= hi) { hi = l; S.worst = i; }
        }
    }
}

static float s_seen = -10;
static void sw_enter(void)
{
    sw_poll();
    S.pressed = BP_NONE;
    if (nos_t - s_seen < 0.2f) return;          /* revealed by a pop: already on screen */
    S.t0 = nos_t; S.build = 0;
    sw_snapshot(0);
    msp_init(&S.lay, S.sw.nlaps ? 100.0f : 0.0f, SPRING_PAGE);   /* percent */
    msp_init(&S.ins, 0, SPRING_SNAPPY);
    msc_init(&S.sc, 0, list_max(), 0);
    S.dragging = 0;
}

static void sw_tick(float dt)
{
    s_seen = nos_t;
    sw_poll();
    int prev_laps = S.sw.nlaps;
    sw_snapshot(1);
    if (S.sw.nlaps != prev_laps) {
        msp_to(&S.lay, S.sw.nlaps ? 100.0f : 0.0f);
        if (S.sw.nlaps > prev_laps && prev_laps > 0) { S.ins.x = -ROW_H; S.ins.v = 0; msp_to(&S.ins, 0); }
        msc_bounds(&S.sc, 0, list_max());
        if (S.sw.nlaps > prev_laps) msc_animate_to(&S.sc, 0);
    }
    msp_step(&S.lay, dt);
    msp_step(&S.ins, dt);
    msc_step(&S.sc, dt);
    S.build = m_clamp((nos_t - S.t0) / 0.4f, 0, 1);
    if (S.sw.running) nos_keep_awake();
}

static float dig_top(void) { return floorf(m_lerp(DIG_Y_SOLO, DIG_Y_LAPS, S.lay.x / 100.0f) + 0.5f); }

static void draw_list(gfx_fb *fb)
{
    gfx_fb sub;
    if (!nos_subview(fb, &sub, 0, (int)LIST_Y0, GFX_W, (int)LIST_Y1, 0, 0)) return;
    float k = qz(m_clamp(S.lay.x / 100.0f * 1.6f - 0.6f, 0, 1), 8);
    if (k <= 0) return;
    float off = floorf(S.sc.x + 0.5f) - floorf(S.ins.x + 0.5f);
    int n = S.sw.nlaps;
    for (int r = 0; r <= n; r++) {
        float y = LIST_Y0 + r * ROW_H - off;          /* row top */
        if (y + ROW_H <= LIST_Y0 || y >= LIST_Y1) continue;
        float yc = y + ROW_H * 0.5f;
        /* fade rows that are leaving the window */
        float e = fminf(yc - LIST_Y0, LIST_Y1 - yc) / (ROW_H * 0.7f);
        float f = qz(m_clamp(e, 0, 1), 8) * k;
        if (f <= 0) continue;
        int x0, x1;
        ui_row_span(yc, 64, &x0, &x1);
        int base = (int)(yc + 6);
        char lb[16], tb[16];
        uint16_t lc = C_SEC, tc = C_SEC;
        if (r == 0) {
            snprintf(lb, sizeof lb, "%s", S.cur_lbl);
            snprintf(tb, sizeof tb, "%s", S.cur_time);
            lc = C_PRI; tc = C_PRI;
        } else {
            int i = n - r + 1;                       /* lap number, newest first */
            snprintf(lb, sizeof lb, "LAP %02d", i);
            fmt_lap(tb, sizeof tb, lap_ms(&S.sw, i));
            if (i == S.best) { lc = C_SEC; tc = C_WHITE; }
            else if (i == S.worst) { lc = C_DIS; tc = C_DIS; }
            else { lc = C_DIS; tc = C_SEC; }
        }
        ui_label(&sub, x0, base, lb, ui_dim(lc, f), UI_LEFT);
        ui_label(&sub, x1, base, tb, ui_dim(tc, f), UI_RIGHT);
        if (r == 0 && n > 0) {                      /* hairline under the live row */
            int hx0, hx1;
            ui_row_span(y + ROW_H, 70, &hx0, &hx1);
            gfx_fill_rect(&sub, hx0, (int)(y + ROW_H) - 1, hx1 - hx0, 2, ui_dim(C_LINE, f));
        }
    }
}

static void sw_draw(gfx_fb *fb)
{
    float b = S.build;
    /* bezel: a dim 60-dot track and the red second-hand dot (smooth, once per second) */
    time_ring(fb, UI_CX, UI_CY, RING_R, 60, 4, 60.0f * m_clamp(b * 1.5f, 0, 1), C_LINE2, 0);
    if (b > 0.3f) {
        float ph = (float)(S.sw.elapsed_ms % 1000) / 1000.0f;
        for (int t = 2; t >= 0; t--) {
            if (t && !S.sw.running) continue;
            float a = 6.2831853f * (ph - t * 0.012f);
            float d = t == 0 ? 10.0f : t == 1 ? 7.0f : 5.0f;
            uint16_t c = t == 0 ? C_RED : ui_dim(C_RED, t == 1 ? 0.55f : 0.3f);
            dot_disc(fb, UI_CX + RING_R * time_sin(a), UI_CY - RING_R * time_cos(a), d, c);
        }
    }
    ui_title(fb, "STOPWATCH", 0);

    /* digits: mm:ss (TALL, pitch 8) + .t (pitch 5), tabular, centred as one group */
    int hours = strlen(S.dig.cur) > 5;
    float pitch = hours ? 6.0f : 8.0f;
    uint16_t dc = (!S.sw.running && S.sw.elapsed_ms) ? C_PRI : C_WHITE;
    dot_style st = { DOT_FONT_TALL, pitch, pitch * 0.8f, dc, 0, 1, 1 };
    dot_style ts = { DOT_FONT_TALL, 5, 4, C_SEC, 0, 1, 1 };
    float w1 = dg_width(&st, S.dig.cur), w2 = dg_width(&ts, S.tenths), gap = 4;
    float x0 = floorf(UI_CX - (w1 + gap + w2) * 0.5f + 0.5f);
    float top = dig_top() + (hours ? 9 : 0);
    dgm_draw(fb, &S.dig, &st, x0, top, 0.2f, b);
    float ty = top + pitch * 9 - 45;
    if (b < 1) dot_text_anim(fb, &ts, x0 + w1 + gap, ty, S.tenths, 0, b, DOT_REVEAL_RANDOM, 9);
    else dot_text(fb, &ts, x0 + w1 + gap, ty, S.tenths);

    draw_list(fb);

    /* buttons */
    float k = qz(m_clamp(b * 2.5f - 1.0f, 0, 1), 8);
    if (k <= 0) return;
    int idle = !S.sw.running && !S.sw.elapsed_ms;
    const char *lt = S.sw.running || idle ? "LAP" : "RESET";
    const char *rt = S.sw.running ? "STOP" : idle ? "START" : "RESUME";
    float lx = UI_CX - BTN_DX, rx = UI_CX + BTN_DX;
    ui_rrect(fb, lx - BTN_W / 2, BTN_Y - 44, BTN_W, 88, 44,
             ui_dim(idle ? C_SURF : S.pressed == BP_LEFT ? C_SURF3 : C_SURF2, k));
    ui_label(fb, (int)lx, (int)BTN_Y + 6, lt, ui_dim(idle ? C_DIS : C_PRI, k), UI_CENTER);
    ui_rrect(fb, rx - BTN_W / 2, BTN_Y - 44, BTN_W, 88, 44, ui_dim(S.pressed == BP_RIGHT ? C_SEC : C_WHITE, k));
    ui_label(fb, (int)rx, (int)BTN_Y + 6, rt, C_BG, UI_CENTER);
}

static int sw_hit(int x, int y)
{
    if (fabsf(y - BTN_Y) > 50) return BP_NONE;
    if (fabsf(x - (UI_CX - BTN_DX)) <= BTN_W / 2 + 4) return BP_LEFT;
    if (fabsf(x - (UI_CX + BTN_DX)) <= BTN_W / 2 + 4) return BP_RIGHT;
    return BP_NONE;
}

static int sw_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN:
        S.pressed = sw_hit(in->x, in->y);
        msc_grab(&S.sc);
        return 1;
    case IN_DRAG_START:
        S.pressed = BP_NONE;
        if (in->axis != AXIS_Y || !S.sw.nlaps) { msc_release(&S.sc, 0); return 0; }
        S.dragging = 1;
        msc_drag(&S.sc, in->dy);
        return 1;
    case IN_DRAG:
        if (!S.dragging) return 0;
        msc_drag(&S.sc, in->dy);
        return 1;
    case IN_DRAG_END:
        if (!S.dragging) return 0;
        S.dragging = 0;
        msc_release(&S.sc, -in->vy);
        return 1;
    case IN_UP:
        if (!S.dragging) msc_release(&S.sc, 0);
        S.pressed = BP_NONE;
        return 1;
    case IN_TAP: {
        int h = sw_hit(in->x, in->y);
        S.pressed = BP_NONE;
        msc_release(&S.sc, 0);
        int idle = !S.sw.running && !S.sw.elapsed_ms;
        if (h == BP_RIGHT) {
            if (S.sw.running) { time_sw_cmd(SW_STOP); nos_feedback(FB_TOGGLE_OFF); }
            else { time_sw_cmd(SW_START); nos_feedback(FB_TOGGLE_ON); }
        } else if (h == BP_LEFT) {
            if (idle) nos_feedback(FB_ERROR);
            else if (S.sw.running) {
                if (S.sw.nlaps >= SVC_LAPS) nos_feedback(FB_LIMIT);
                else { time_sw_cmd(SW_LAP); nos_feedback(FB_TAP); }
            } else { time_sw_cmd(SW_RESET); nos_feedback(FB_TAP); }
        }
        return 1;
    }
    default:
        return 0;
    }
}

const nos_screen scr_stopwatch = { "STOPWATCH", sw_enter, 0, sw_tick, sw_draw, sw_input, 0 };
