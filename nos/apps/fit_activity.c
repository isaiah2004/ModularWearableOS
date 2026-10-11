/* fit_activity.c — Activity app and home widget (health team).
 *
 * Pages (vertical, one screen each, finger-driven spring pager):
 *   0  steps hero: dot-matrix count inside a segmented glyph ring toward the goal
 *      (48 radial three-dot segments; past the goal the ring continues in red)
 *   1  today: distance / kcal / active minutes blocks + hourly dot-column chart
 *   2  this week: daily average + 7 day bars with day letters, today emphasised
 * Entry: the ring lights segment by segment while the number counts up through dot
 * morphs; one haptic tick when the last segment lands. Pages 1-2 grow their bars the
 * first time they come into view. Data: svc_activity(), copied in tick(). */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#define PAGE_H   466.0f
#define NPAGES   3
#define SEG_N    48
#define SEG_D    5.5f
#define FILL_S   0.62f          /* ring fill + count-up duration */
#define COUNT_K  8              /* count-up steps (each a dot morph) */
#define ENTRY_DELAY 0.22f       /* app: let the iris open before the ring fills */

static const float k_seg_r[3] = { 182.0f, 193.0f, 204.0f };

/* ------------------------------------------------------------ shared data */
static svc_activity_t s_act;      /* copy taken in tick */
static int s_hour, s_wday;        /* local time, read in tick */

static void read_data(void)
{
    const svc_activity_t *a = svc_activity();
    if (a) s_act = *a;
    if (s_act.goal <= 0) s_act.goal = nos_prefs.step_goal_100 ? nos_prefs.step_goal_100 * 100 : 8000;
    hal_datetime dt;
    hal_get_datetime(&dt);
    s_hour = dt.hour; s_wday = dt.weekday;
}

static float qz(float v, float steps) { return floorf(v * steps + 0.5f) / steps; }

/* Neutral grey at a fraction of a grey token (ui_dim rounds the 5/6-bit channels
 * separately, which tints dark greys magenta). */
static uint16_t gdim(uint16_t c, float k)
{
    float lvl = (float)((c >> 5) & 63) / 63.0f * m_clamp(k, 0, 1);
    int r = (int)lroundf(lvl * 31.0f), g = (int)lroundf(lvl * 63.0f);
    return (uint16_t)(r << 11 | g << 5 | r);
}

/* Segments lit (white) and over-goal (red) for a step count, in whole segments. */
static void seg_counts(int steps, int goal, int *lit, int *over)
{
    float f = goal > 0 ? (float)steps / (float)goal : 0;
    int l = (int)floorf(m_clamp(f, 0, 1) * SEG_N);
    if (steps > 0 && l == 0) l = 1;                    /* the first steps always show */
    int o = f > 1 ? (int)floorf(m_clamp(f - 1, 0, 1) * SEG_N) : 0;
    if (f > 1 && o == 0) o = 1;
    *lit = l; *over = o;
}

/* The glyph ring: `show` segments of the lit+over total are shown so far.
 * widget: a smaller ring (the home pager owns the top/bottom centre of widget pages)
 * and no red (the pager's unread dot is the only red there): the overflow lap is white
 * over a grey first lap instead. */
static void seg_ring(gfx_fb *fb, float cx, float cy, int lit, int over, int show, uint16_t off, int widget)
{
    int w = show < lit ? show : lit;
    int r = show > lit ? show - lit : 0;
    if (r > over) r = over;
    for (int k = 0; k < 3; k++) {
        float rad = k_seg_r[k] - (widget ? 22.0f : 0.0f);
        if (r > 0) {
            /* over goal: the second lap paints over the start of the first */
            dot_ring(fb, cx, cy, rad, SEG_N, SEG_D, (float)r, (float)SEG_N, widget ? C_SEC : C_PRI, 0);
            dot_ring(fb, cx, cy, rad, SEG_N, SEG_D, 0, (float)r, widget ? C_WHITE : C_RED, 0);
        } else
            dot_ring(fb, cx, cy, rad, SEG_N, SEG_D, 0, (float)w, C_PRI, off);
    }
}
/* ---- count-up through dot morphs ----
 * mode 0: counting up from 0 in COUNT_K eased steps, each step a dot morph;
 * mode 1: one morph from the previous value to a new one (live change);
 * mode 2: settled. All state advances in tick; draw only reads it. */
typedef struct {
    char from[12], to[12];
    float t, p;
    int mode, target;
} counter;

static int count_value(int target, int k)
{
    if (k >= COUNT_K) return target;
    if (k <= 0) return 0;
    return (int)lroundf((float)target * bezier_ease(&EASE_STANDARD, (float)k / COUNT_K));
}
static void counter_hold(counter *c, int target)       /* pre-build: shows 0 */
{
    c->t = 0; c->p = 0; c->target = target; c->mode = target ? 0 : 2;
    snprintf(c->from, sizeof c->from, "0");
    snprintf(c->to, sizeof c->to, "%d", target ? count_value(target, 1) : 0);
}
static void counter_set(counter *c, int target)
{
    if (c->target == target) return;
    snprintf(c->from, sizeof c->from, "%d", c->target);
    snprintf(c->to, sizeof c->to, "%d", target);
    c->target = target; c->mode = 1; c->t = 0; c->p = 0;
}
static void counter_tick(counter *c, float dt)
{
    if (c->mode == 2) return;
    c->t += dt;
    if (c->mode == 1) {
        c->p = qz(m_clamp(c->t / 0.3f, 0, 1), 16.0f);
        if (c->p >= 1) { c->mode = 2; memcpy(c->from, c->to, sizeof c->to); }
        return;
    }
    float step = FILL_S / COUNT_K;
    int k = (int)(c->t / step);
    if (k >= COUNT_K) {
        c->mode = 2; c->p = 1;
        snprintf(c->to, sizeof c->to, "%d", c->target);
        memcpy(c->from, c->to, sizeof c->to);
        return;
    }
    snprintf(c->from, sizeof c->from, "%d", count_value(c->target, k));
    snprintf(c->to, sizeof c->to, "%d", count_value(c->target, k + 1));
    c->p = qz(m_clamp(c->t / step - (float)k, 0, 1), 16.0f);
}
static void counter_draw(gfx_fb *fb, const counter *c, const dot_style *st, float cx, float y)
{
    if (c->mode == 2) dot_text_a(fb, st, cx, y, c->to, 1);
    else dot_text_morph(fb, st, cx, y, c->from, c->to, 1, c->p);
}

/* -------------------------------------------------------- vertical pager */
typedef struct {
    mspring sp;
    int page, n, drag;
    float grab, dy0;
    float bar_k, bar_t;   /* scroll indicator fade */
} vpager;

static void vp_init(vpager *v, int n)
{
    msp_init(&v->sp, 0, SPRING_PAGE);
    v->page = 0; v->n = n; v->drag = 0; v->bar_k = 0; v->bar_t = -10;
}
static int vp_input(vpager *v, const nos_input *in)
{
    float max = (float)(v->n - 1) * PAGE_H;
    switch (in->type) {
    case IN_DRAG_START:
        if (in->axis != AXIS_Y) return 0;
        v->drag = 1; v->grab = v->sp.x; v->dy0 = in->dy;
        v->bar_t = nos_t;
        /* fallthrough */
    case IN_DRAG: {
        if (!v->drag) return 0;
        float x = v->grab - (in->dy - v->dy0);
        if (x < 0) x = -rubber_band(-x, PAGE_H);
        if (x > max) x = max + rubber_band(x - max, PAGE_H);
        msp_set(&v->sp, x);
        v->bar_t = nos_t;
        return 1;
    }
    case IN_DRAG_END: {
        if (!v->drag) return 0;
        v->drag = 0;
        float vel = -in->vy, x = v->sp.x;
        int t;
        if (vel > 350) t = (int)floorf(x / PAGE_H) + 1;
        else if (vel < -350) t = (int)ceilf(x / PAGE_H) - 1;
        else t = (int)floorf(x / PAGE_H + 0.5f);
        if (t > v->page + 1) t = v->page + 1;
        if (t < v->page - 1) t = v->page - 1;
        if (t < 0) t = 0;
        if (t > v->n - 1) t = v->n - 1;
        if (t != v->page) nos_feedback(FB_TICK);
        v->page = t;
        msp_kick(&v->sp, (float)t * PAGE_H, vel);
        v->bar_t = nos_t;
        return 1;
    }
    default:
        return 0;
    }
}
static void vp_tick(vpager *v, float dt)
{
    if (!v->drag && msp_step(&v->sp, dt)) v->bar_t = nos_t;
    float target = nos_t - v->bar_t < 0.7f ? 1.0f : 0.0f;
    float k = dt * (target > v->bar_k ? 10.0f : 3.5f);
    v->bar_k += (target - v->bar_k) * (k > 1 ? 1 : k);
    if (v->bar_k < 0.04f) v->bar_k = 0;
    v->bar_k = qz(v->bar_k, 16.0f);
}
static float vp_off(const vpager *v) { return floorf(v->sp.x + 0.5f); }
static void vp_draw_bar(gfx_fb *fb, const vpager *v)
{
    ui_scrollbar(fb, v->sp.x, PAGE_H * v->n, PAGE_H, v->bar_k);
}

/* ================================================================= APP */
static struct {
    vpager vp;
    float t;               /* since enter */
    counter cnt;
    int lit, over, shown;  /* ring fill */
    int ticked;
    float pg_build[NPAGES];
    int   pg_started[NPAGES];
} A;

static void act_enter(void)
{
    read_data();
    vp_init(&A.vp, NPAGES);
    A.vp.bar_t = -10;                 /* the bar would crowd the ring: shown only while paging */
    A.t = 0;
    seg_counts(s_act.steps, s_act.goal, &A.lit, &A.over);
    A.shown = 0;
    A.ticked = 0;
    counter_hold(&A.cnt, s_act.steps);
    memset(A.pg_build, 0, sizeof A.pg_build);
    memset(A.pg_started, 0, sizeof A.pg_started);
    A.pg_started[0] = 1;
}

static void act_tick(float dt)
{
    A.t += dt;
    read_data();
    vp_tick(&A.vp, dt);
    /* the ring: segment by segment; the timeline waits for the iris to open */
    float te = A.t - ENTRY_DELAY;
    int lit, over;
    seg_counts(s_act.steps, s_act.goal, &lit, &over);
    if (te > FILL_S + 0.1f) {
        A.lit = lit; A.over = over;
        A.shown = A.lit + A.over;
        if (A.cnt.mode == 2) counter_set(&A.cnt, s_act.steps);
    } else {
        float e = bezier_ease(&EASE_STANDARD, m_clamp(te / FILL_S, 0, 1));
        A.shown = (int)floorf(e * (float)(A.lit + A.over) + 0.0001f);
    }
    if (!A.ticked && A.shown >= A.lit + A.over && te > 0.05f) {
        A.ticked = 1;
        if (A.lit + A.over > 0) nos_feedback(FB_TICK);
    }
    if (te > 0) counter_tick(&A.cnt, dt);
    /* pages 1-2 build the first time they come half into view */
    float pos = A.vp.sp.x / PAGE_H;
    for (int i = 1; i < NPAGES; i++) {
        if (!A.pg_started[i] && fabsf(pos - (float)i) < 0.55f) A.pg_started[i] = 1;
        if (A.pg_started[i] && A.pg_build[i] < 1) A.pg_build[i] = m_clamp(A.pg_build[i] + dt / 0.5f, 0, 1);
    }
}

/* ---- page 0: hero ---- */
static void draw_hero(gfx_fb *fb, float oy, const counter *c, int lit, int over, int shown, int title)
{
    seg_ring(fb, UI_CX, UI_CY + oy, lit, over, shown, C_LINE, !title);
    dot_icon(fb, DOT_ICON_STEPS, UI_CX, 140 + oy, 3.0f, 2.4f, C_SEC);
    dot_style st = { DOT_FONT_TALL, 8.0f, 6.4f, s_act.steps ? C_WHITE : C_DIS, C_GHOST, 1, 1 };
    counter_draw(fb, c, &st, UI_CX, 190 + oy);
    ui_label(fb, UI_CX, (int)(298 + oy), "STEPS", C_SEC, UI_CENTER);
    char b[32];
    int pct = s_act.goal > 0 ? (int)((long)s_act.steps * 100 / s_act.goal) : 0;
    if (s_act.steps > 0) snprintf(b, sizeof b, "%d%% OF %d", pct, s_act.goal);
    else snprintf(b, sizeof b, "GOAL %d", s_act.goal);
    ui_label(fb, UI_CX, (int)(330 + oy), b, C_DIS, UI_CENTER);
}

/* ---- page 1: today ---- */
static void value_block(gfx_fb *fb, float cx, float oy, int icon, const char *v, const char *unit, float p)
{
    float k = qz(m_clamp(p * 1.4f, 0, 1), 8.0f);
    dot_icon(fb, icon, cx, 108 + oy, 2.6f, 2.1f, gdim(C_SEC, k));
    dot_style st = { DOT_FONT_TALL, 5.0f, 4.0f, C_WHITE, 0, 1, 1 };
    dot_text_anim(fb, &st, cx, 132 + oy, v, 1, p, DOT_REVEAL_RAIN, 3);
    ui_label(fb, (int)cx, (int)(204 + oy), unit, gdim(C_SEC, k), UI_CENTER);
}

static void draw_today(gfx_fb *fb, float oy)
{
    float p = A.pg_build[1];
    char b[16];
    int metric = nos_prefs.units_metric != 0;
    float dist = metric ? s_act.km : s_act.km * 0.621371f;
    if (dist < 10) snprintf(b, sizeof b, "%.1f", (double)dist);
    else snprintf(b, sizeof b, "%d", (int)(dist + 0.5f));
    value_block(fb, UI_CX - 124, oy, DOT_ICON_ROUTE, b, metric ? "KM" : "MI", m_stagger(p, 0, 3, 0.6f));
    snprintf(b, sizeof b, "%d", (int)(s_act.kcal + 0.5f));
    value_block(fb, UI_CX, oy, DOT_ICON_FLAME, b, "KCAL", m_stagger(p, 1, 3, 0.6f));
    snprintf(b, sizeof b, "%d", s_act.active_min);
    value_block(fb, UI_CX + 124, oy, DOT_ICON_TIMER, b, "ACTIVE", m_stagger(p, 2, 3, 0.6f));

    /* hourly chart: 24 dot columns; past = grey, this hour = white, future = grid only */
    const int rows = 7;
    const float pitch = 13.0f, d = 9.0f;
    float x0 = UI_CX - 12 * pitch, y0 = 238 + oy;
    int maxv = 400;
    for (int i = 0; i < 24; i++) if (s_act.hourly[i] > maxv) maxv = s_act.hourly[i];
    uint8_t past[24], now[1];
    float pb = bezier_ease(&EASE_STANDARD, m_clamp((p - 0.15f) / 0.85f, 0, 1));
    for (int i = 0; i < 24; i++) {
        float g = bezier_ease(&EASE_STANDARD, m_stagger(pb, i, 24, 0.5f));
        float v = (float)s_act.hourly[i] / (float)maxv;
        if (s_act.hourly[i] > 0 && v < 0.5f / rows) v = 0.5f / rows;   /* any activity shows */
        v = floorf(v * rows + 0.5f) / rows;                           /* whole dots at rest */
        if (s_act.hourly[i] > 0 && v < 1.0f / rows) v = 1.0f / rows;
        int q = (int)lroundf(m_clamp(v * g, 0, 1) * 255.0f);
        past[i] = i < s_hour ? (uint8_t)(q > 255 ? 255 : q) : 0;
        if (i == s_hour) now[0] = (uint8_t)(q > 255 ? 255 : q);
    }
    dot_columns(fb, x0, y0, 24, rows, pitch, d, past, C_SEC, C_GHOST);
    if (s_hour >= 0 && s_hour < 24)
        dot_columns(fb, x0 + s_hour * pitch, y0, 1, rows, pitch, d, now, C_WHITE, C_GHOST);
    /* now marker (the screen's one red element unless the goal ring overflows) */
    float nx = x0 + (s_hour + 0.5f) * pitch, ny = y0 + rows * pitch + 10;
    if (pb > 0.5f) dot_disc(fb, nx, ny, 6.0f, A.over ? C_WHITE : C_RED);
    static const char *k_ax[4] = { "00", "06", "12", "18" };
    for (int i = 0; i < 4; i++) {
        float ax = x0 + (i * 6 + 0.5f) * pitch;
        if (fabsf(ax - nx) < 46) continue;
        ui_label(fb, (int)ax, (int)(y0 + rows * pitch + 40), k_ax[i], C_DIS, UI_CENTER);
    }
    ui_label(fb, (int)nx, (int)(y0 + rows * pitch + 40), "NOW", C_SEC, UI_CENTER);
}

/* ---- page 2: week ---- */
static void draw_week(gfx_fb *fb, float oy)
{
    float p = A.pg_build[2];
    uint32_t sum = 0, maxv = (uint32_t)s_act.goal;
    int ndays = 0;
    for (int i = 0; i < 7; i++) {
        uint32_t v = i == 6 ? (uint32_t)s_act.steps : s_act.week[i];
        if (i == 6 && s_act.week[6] > v) v = s_act.week[6];
        if (v) { sum += v; ndays++; }
        if (v > maxv) maxv = v;
    }
    ui_label(fb, UI_CX, (int)(96 + oy), "THIS WEEK", C_SEC, UI_CENTER);
    char b[16];
    snprintf(b, sizeof b, "%u", (unsigned)(ndays ? sum / (uint32_t)ndays : 0));
    dot_style st = { DOT_FONT_TALL, 5.0f, 4.0f, ndays ? C_WHITE : C_DIS, 0, 1, 1 };
    dot_text_anim(fb, &st, UI_CX, 118 + oy, b, 1, m_clamp(p * 1.5f, 0, 1), DOT_REVEAL_RAIN, 5);
    ui_label(fb, UI_CX, (int)(190 + oy), "DAILY AVG", C_DIS, UI_CENTER);

    const int rows = 9;
    const float pitch = 10.0f, d = 7.6f, slot = 42.0f;
    float y0 = 222 + oy, chart_h = rows * pitch;
    float scale = (float)maxv * 1.08f;
    float pb = bezier_ease(&EASE_STANDARD, m_clamp((p - 0.1f) / 0.9f, 0, 1));
    for (int i = 0; i < 7; i++) {
        uint32_t v = i == 6 ? (uint32_t)s_act.steps : s_act.week[i];
        if (i == 6 && s_act.week[6] > v) v = s_act.week[6];
        float g = bezier_ease(&EASE_STANDARD, m_stagger(pb, i, 7, 0.5f));
        float f = scale > 0 ? (float)v / scale : 0;
        if (v > 0 && f < 0.5f / rows) f = 0.5f / rows;
        f = floorf(f * rows + 0.5f) / rows;
        if (v > 0 && f < 1.0f / rows) f = 1.0f / rows;
        uint8_t q = (uint8_t)lroundf(m_clamp(f * g, 0, 1) * 255.0f);
        uint8_t vals[2] = { q, q };
        float cx = UI_CX + (i - 3) * slot;
        dot_columns(fb, cx - pitch, y0, 2, rows, pitch, d, vals, i == 6 ? C_WHITE : C_SEC, C_GHOST);
        static const char *k_d = "SMTWTFS";
        char l[2] = { k_d[((s_wday - (6 - i)) % 7 + 7) % 7], 0 };
        ui_label(fb, (int)cx, (int)(y0 + chart_h + 32), l, i == 6 ? C_WHITE : C_DIS, UI_CENTER);
    }
    /* goal line: a fine dotted rule across the chart */
    float gy = y0 + chart_h - chart_h * ((float)s_act.goal / scale);
    if (pb > 0.3f && gy > y0) {
        float gx0 = UI_CX - 3 * slot - pitch, gx1 = UI_CX + 3 * slot + pitch;
        dot_bar(fb, gx0, gy, 41, (gx1 - gx0) / 40.0f, 2.2f, 41, C_DIS, 0);
        ui_label(fb, (int)(gx1 + 12), (int)(gy + 6), "GOAL", C_DIS, UI_LEFT);
    }
}

static void act_draw(gfx_fb *fb)
{
    float off = vp_off(&A.vp);
    for (int pg = 0; pg < NPAGES; pg++) {
        float oy = pg * PAGE_H - off;
        if (oy <= -PAGE_H || oy >= PAGE_H) continue;
        if (pg == 0) draw_hero(fb, oy, &A.cnt, A.lit, A.over, A.shown, 1);
        else if (pg == 1) draw_today(fb, oy);
        else draw_week(fb, oy);
    }
    vp_draw_bar(fb, &A.vp);
}

static int act_input(const nos_input *in)
{
    return vp_input(&A.vp, in);
}

const nos_screen scr_activity = { "ACTIVITY", act_enter, 0, act_tick, act_draw, act_input, 0 };

/* ============================================================== WIDGET
 * While the page is off screen it rests in its pre-build state (ring unlit, "0"), so
 * when the pager settles on it shown() plays the same fill + count-up as the app. */
static struct {
    float t;
    int started;          /* shown() ran since the page last became visible */
    counter cnt;
    int lit, over, shown;
} W;

static void wg_reset(void)
{
    W.t = 0; W.started = 0; W.shown = 0;
    seg_counts(s_act.steps, s_act.goal, &W.lit, &W.over);
    counter_hold(&W.cnt, s_act.steps);
}

static void wg_shown(void)
{
    if (W.started) return;              /* already building since the page slid in */
    read_data();
    wg_reset();
    W.started = 1;
}

static void wg_tick(float dt, int visible)
{
    if (!visible) { if (W.started || W.cnt.target != s_act.steps) { read_data(); wg_reset(); } return; }
    read_data();
    if (!W.started) { wg_reset(); W.started = 1; }   /* build while the page slides in */
    W.t += dt;
    int lit, over;
    seg_counts(s_act.steps, s_act.goal, &lit, &over);
    if (W.t > FILL_S + 0.1f) {
        W.lit = lit; W.over = over; W.shown = lit + over;
        if (W.cnt.mode == 2) counter_set(&W.cnt, s_act.steps);
    } else {
        float e = bezier_ease(&EASE_STANDARD, m_clamp(W.t / FILL_S, 0, 1));
        W.shown = (int)floorf(e * (float)(W.lit + W.over) + 0.0001f);
    }
    counter_tick(&W.cnt, dt);
}

static void wg_draw(gfx_fb *fb)
{
    draw_hero(fb, 0, &W.cnt, W.lit, W.over, W.shown, 0);
}

static void wg_tap(int x, int y) { nos_open(&scr_activity, x, y); }

const nos_widget wg_activity = { "ACTIVITY", wg_tick, wg_draw, wg_tap, wg_shown };
