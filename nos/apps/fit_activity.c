/* fit_activity.c — Activity app and home widget (health team).
 *
 * Pages (vertical, one screen each, finger-driven spring pager):
 *   0  steps hero: dot-matrix count inside a segmented glyph ring toward the goal
 *      (48 radial three-dot segments; past the goal the ring continues in red)
 *   1  today: distance / kcal / active minutes blocks + hourly dot-column chart
 *   2  this week: daily average + 7 day bars with day letters, today emphasised
 * Entry (NOS_DESIGN §8): starts at nos_enter_t() (40% of the iris); the number builds
 * with a dot sparkle while the ring lights segment by segment, both in 0.30 s, one haptic
 * tick when the last segment lands; the labels follow, and the scrollbar shows once for
 * 0.7 s to say there are pages below. Live changes morph (at most 1 Hz). Pages 1-2 grow
 * their bars from the first revealed pixel. Revealed by back: everything kept as it was.
 * Data: svc_activity(), copied in tick(). */
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
#define MORPH_S  0.28f          /* live value change */
#define MORPH_GAP 1.0f          /* live readings change at most ~1 Hz (§8) */

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

/* Thousands separator only from 10,000 (4-digit counts stay compact). */
static void fmt_steps(char *b, int n, int v)
{
    if (v < 0) v = 0;
    if (v >= 10000) snprintf(b, (size_t)n, "%d,%03d", v / 1000, v % 1000);
    else snprintf(b, (size_t)n, "%d", v);
}

/* A dot whole-row value for dot_columns: at rest rounded down so no sub-pixel speck
 * grows above the column; while growing (g < 1) continuous. */
static uint8_t col_value(float f, int rows, float g, int nonzero)
{
    f = floorf(m_clamp(f, 0, 1) * (float)rows + 0.5f) / (float)rows;
    if (nonzero && f < 1.0f / (float)rows) f = 1.0f / (float)rows;
    if (g >= 1.0f) return (uint8_t)floorf(f * 255.0f);
    return (uint8_t)lroundf(m_clamp(f * g, 0, 1) * 255.0f);
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
 * and no red (the pager's unread dot is the only red there): over the goal the first
 * lap stays full white and the overflow runs as a second ring of white dots inside it,
 * so 135 % reads as a full lap plus a third, not as 35 %. */
static void seg_ring(gfx_fb *fb, float cx, float cy, int lit, int over, int show, uint16_t off, int widget)
{
    int w = show < lit ? show : lit;
    int r = show > lit ? show - lit : 0;
    if (r > over) r = over;
    for (int k = 0; k < 3; k++) {
        float rad = k_seg_r[k] - (widget ? 22.0f : 0.0f);
        if (r > 0 && !widget) {
            /* over goal: the second lap paints over the start of the first */
            dot_ring(fb, cx, cy, rad, SEG_N, SEG_D, (float)r, (float)SEG_N, C_PRI, 0);
            dot_ring(fb, cx, cy, rad, SEG_N, SEG_D, 0, (float)r, C_RED, 0);
        } else
            dot_ring(fb, cx, cy, rad, SEG_N, SEG_D, 0, (float)w, C_PRI, r > 0 ? 0 : off);
    }
    if (r > 0 && widget)
        dot_ring(fb, cx, cy, k_seg_r[0] - 22.0f - 14.0f, SEG_N, SEG_D, 0, (float)r, C_PRI, 0);
}

/* ---- the hero: build (dot sparkle) then live morphs ----
 * All state advances in tick; draw only reads it. */
typedef struct {
    float t;            /* since the build started (< 0: waiting) */
    int value, prev;    /* shown value, previous (morph source) */
    float morph;        /* 0..1, 1 = settled */
    float since;        /* since the last morph started */
    int lit, over, shown, ticked;
} hero;

static void hero_start(hero *h, float delay)
{
    h->t = -delay; h->value = h->prev = s_act.steps; h->morph = 1; h->since = 10;
    seg_counts(s_act.steps, s_act.goal, &h->lit, &h->over);
    h->shown = 0; h->ticked = 0;
}
static void hero_finish(hero *h)
{
    if (h->t < UI_ENTRY_S) h->t = UI_ENTRY_S + 1.0f;
    h->morph = 1;
    if (!h->ticked) { h->value = h->prev = s_act.steps; h->since = 10; }
    seg_counts(s_act.steps, s_act.goal, &h->lit, &h->over);
    if (!h->ticked) h->shown = h->lit + h->over;
    h->ticked = 1;
}
static float hero_build(const hero *h) { return m_clamp(h->t / UI_ENTRY_S, 0, 1); }
/* Returns 1 on the frame the entry build completes. */
static int hero_tick(hero *h, float dt, int haptic)
{
    h->t += dt;
    h->since += dt;
    if (h->morph < 1) h->morph = m_clamp(h->morph + dt / MORPH_S, 0, 1);
    float b = hero_build(h);
    if (b < 1) {
        /* ring segments light in step with the number's build */
        h->shown = (int)floorf(bezier_ease(&EASE_STANDARD, b) * (float)(h->lit + h->over) + 0.0001f);
        return 0;
    }
    int done = 0;
    if (!h->ticked) { h->ticked = 1; done = 1; if (haptic && h->shown > 0) nos_feedback(FB_TICK); }
    /* live changes: the ring follows one segment per frame (never a jump) */
    seg_counts(s_act.steps, s_act.goal, &h->lit, &h->over);
    int tgt = h->lit + h->over;
    if (h->shown < tgt) h->shown++;
    else if (h->shown > tgt) h->shown--;
    /* the number morphs at most once a second while walking */
    if (s_act.steps != h->value && h->morph >= 1 && h->since >= MORPH_GAP) {
        h->prev = h->value; h->value = s_act.steps; h->morph = 0; h->since = 0;
    }
    return done;
}

static void draw_hero(gfx_fb *fb, float oy, const hero *h, int widget)
{
    float b = hero_build(h);
    seg_ring(fb, UI_CX, UI_CY + oy, h->lit, h->over, h->shown, C_LINE, widget);
    dot_icon_anim(fb, DOT_ICON_STEPS, UI_CX, 140 + oy, 3.0f, 2.4f, C_SEC, b, DOT_REVEAL_CENTER);
    char s[16], a[16];
    fmt_steps(s, sizeof s, h->value);
    float pitch = strlen(s) <= 4 ? UI_HERO_PITCH : UI_HEROW_PITCH;
    dot_style st = { DOT_FONT_TALL, pitch, pitch * 0.8f, h->value ? C_WHITE : C_DIS, 0, 1, 1 };
    float y = 190 + oy + (UI_HERO_PITCH - pitch) * 4.5f;   /* keep the number's centre */
    if (b < 1) { if (b > 0) dot_text_anim(fb, &st, UI_CX, y, s, 1, b, DOT_REVEAL_RANDOM, 3); }
    else if (h->morph < 1) {
        fmt_steps(a, sizeof a, h->prev);
        dot_text_morph(fb, &st, UI_CX, y, a, s, 1, qz(h->morph, 16.0f));
    } else dot_text_a(fb, &st, UI_CX, y, s, 1);
    /* labels follow the build (they would give the number away before it lands) */
    float k = qz(m_clamp((h->t - UI_ENTRY_S * 0.6f) / 0.15f, 0, 1), 8.0f);
    if (k <= 0) return;
    ui_label(fb, UI_CX, (int)(300 + oy), "STEPS", ui_dim(C_SEC, k), UI_CENTER);
    char g[32];
    int pct = s_act.goal > 0 ? (int)((long)h->value * 100 / s_act.goal) : 0;
    if (h->value > 0) snprintf(g, sizeof g, "%d%% OF %d", pct, s_act.goal);
    else snprintf(g, sizeof g, "GOAL %d", s_act.goal);
    ui_label(fb, UI_CX, (int)(332 + oy), g, ui_dim(C_DIS, k), UI_CENTER);
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
    /* linear ramp at a fixed rate (in 4 frames, out 8); quantised only when drawn,
     * so it always reaches 0 instead of rounding back up */
    float target = nos_t - v->bar_t < 0.7f ? 1.0f : 0.0f;
    float step = dt * (target > v->bar_k ? 7.5f : 3.75f);
    if (v->bar_k < target) { v->bar_k += step; if (v->bar_k > target) v->bar_k = target; }
    else { v->bar_k -= step; if (v->bar_k < target) v->bar_k = target; }
}
/* Show the scrollbar once (0.7 s, then the normal fade): there are pages below. */
static void vp_flash(vpager *v) { v->bar_t = nos_t; }
static float vp_off(const vpager *v) { return floorf(v->sp.x + 0.5f); }
static void vp_draw_bar(gfx_fb *fb, const vpager *v)
{
    /* 8 equal steps, drawn down to the last one (no pop at the end) */
    float a = qz(v->bar_k, 8.0f);
    if (a > 0) ui_scrollbar(fb, v->sp.x, PAGE_H * v->n, PAGE_H, a);
}

/* ================================================================= APP */
static struct {
    vpager vp;
    hero hero;
    float pg_build[NPAGES];
    int   pg_started[NPAGES];
} A;

static void act_enter(void)
{
    read_data();
    if (nos_revealed()) {
        /* back from a cover: keep the page, the built hero and the built pages */
        hero_finish(&A.hero);
        return;
    }
    vp_init(&A.vp, NPAGES);
    memset(A.pg_build, 0, sizeof A.pg_build);
    memset(A.pg_started, 0, sizeof A.pg_started);
    A.pg_started[0] = 1;
    if (nos_entered_by_finger()) { hero_start(&A.hero, 0); hero_finish(&A.hero); vp_flash(&A.vp); }
    else hero_start(&A.hero, nos_enter_t() - nos_t);
}

static void act_tick(float dt)
{
    read_data();
    vp_tick(&A.vp, dt);
    if (hero_tick(&A.hero, dt, 1)) vp_flash(&A.vp);
    /* pages 1-2 build from their first revealed pixel, so the stagger ends as they land */
    float pos = A.vp.sp.x / PAGE_H;
    for (int i = 1; i < NPAGES; i++) {
        if (!A.pg_started[i] && fabsf(pos - (float)i) < 0.95f) A.pg_started[i] = 1;
        if (A.pg_started[i] && A.pg_build[i] < 1) A.pg_build[i] = m_clamp(A.pg_build[i] + dt / 0.45f, 0, 1);
    }
}

/* ---- page 1: today ---- */
static void value_block(gfx_fb *fb, float cx, float oy, int icon, const char *v, int zero, const char *unit, float p)
{
    float k = qz(m_clamp(p * 1.4f, 0, 1), 8.0f);
    dot_icon_anim(fb, icon, cx, 124 + oy, 2.6f, 2.1f, C_SEC, p, DOT_REVEAL_CENTER);
    dot_style st = { DOT_FONT_TALL, UI_LIST_PITCH, UI_LIST_PITCH * 0.8f, zero ? C_DIS : C_WHITE, 0, 1, 1 };
    dot_text_anim(fb, &st, cx, 148 + oy, v, 1, p, DOT_REVEAL_RANDOM, 3);
    ui_label(fb, (int)cx, (int)(214 + oy), unit, ui_dim(C_SEC, k), UI_CENTER);
}

static void draw_today(gfx_fb *fb, float oy)
{
    float p = A.pg_build[1];
    ui_label(fb, UI_CX, (int)(UI_TITLE_Y + oy), "TODAY", C_SEC, UI_CENTER);
    char b[16];
    int metric = nos_prefs.units_metric != 0;
    float dist = metric ? s_act.km : s_act.km * 0.621371f;
    if (dist < 10) snprintf(b, sizeof b, "%.1f", (double)dist);
    else snprintf(b, sizeof b, "%d", (int)(dist + 0.5f));
    value_block(fb, UI_CX - 124, oy, DOT_ICON_ROUTE, b, dist < 0.05f, metric ? "KM" : "MI", m_stagger(p, 0, 3, 0.6f));
    snprintf(b, sizeof b, "%d", (int)(s_act.kcal + 0.5f));
    value_block(fb, UI_CX, oy, DOT_ICON_FLAME, b, s_act.kcal < 0.5f, "KCAL", m_stagger(p, 1, 3, 0.6f));
    snprintf(b, sizeof b, "%d", s_act.active_min);
    value_block(fb, UI_CX + 124, oy, DOT_ICON_TIMER, b, s_act.active_min == 0, "ACTIVE", m_stagger(p, 2, 3, 0.6f));

    /* hourly chart: 24 dot columns; past = grey, this hour = white, future = grid only */
    const int rows = 7;
    const float pitch = 13.0f, d = 9.0f;
    float x0 = UI_CX - 12 * pitch, y0 = 252 + oy;
    int maxv = 400;
    for (int i = 0; i < 24; i++) if (s_act.hourly[i] > maxv) maxv = s_act.hourly[i];
    uint8_t past[24], now[1] = { 0 };
    float pb = bezier_ease(&EASE_STANDARD, m_clamp((p - 0.15f) / 0.85f, 0, 1));
    for (int i = 0; i < 24; i++) {
        float g = bezier_ease(&EASE_STANDARD, m_stagger(pb, i, 24, 0.5f));
        uint8_t q = col_value((float)s_act.hourly[i] / (float)maxv, rows, g, s_act.hourly[i] > 0);
        past[i] = i < s_hour ? q : 0;
        if (i == s_hour) now[0] = q;
    }
    dot_columns(fb, x0, y0, 24, rows, pitch, d, past, C_SEC, C_GHOST);
    if (s_hour >= 0 && s_hour < 24)
        dot_columns(fb, x0 + s_hour * pitch, y0, 1, rows, pitch, d, now, C_WHITE, C_GHOST);
    /* now marker (the screen's one red element unless the goal ring overflows) */
    float nx = x0 + (s_hour + 0.5f) * pitch, ny = y0 + rows * pitch + 10;
    if (pb > 0.5f) dot_disc(fb, nx, ny, 6.0f, A.hero.over ? C_WHITE : C_RED);
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
        /* DAILY AVG over the six completed days: today's partial count would pull it
         * down every morning; today stays visible as its own (white) column */
        if (v && i < 6) { sum += v; ndays++; }
        if (v > maxv) maxv = v;
    }
    ui_label(fb, UI_CX, (int)(UI_TITLE_Y + oy), "THIS WEEK", C_SEC, UI_CENTER);
    char b[16];
    fmt_steps(b, sizeof b, ndays ? (int)(sum / (uint32_t)ndays) : 0);
    dot_style st = { DOT_FONT_TALL, UI_LIST_PITCH, UI_LIST_PITCH * 0.8f, ndays ? C_WHITE : C_DIS, 0, 1, 1 };
    dot_text_anim(fb, &st, UI_CX, 112 + oy, b, 1, m_clamp(p * 1.5f, 0, 1), DOT_REVEAL_RANDOM, 5);
    ui_label(fb, UI_CX, (int)(180 + oy), "DAILY AVG", C_DIS, UI_CENTER);

    const int rows = 9;
    const float pitch = 10.0f, d = 7.6f, slot = 42.0f;
    float y0 = 222 + oy, chart_h = rows * pitch;
    float scale = (float)maxv * 1.08f;
    float pb = bezier_ease(&EASE_STANDARD, m_clamp((p - 0.1f) / 0.9f, 0, 1));
    /* goal line first (bars paint over it), on a gap between dot rows */
    float gy = y0 + chart_h - chart_h * ((float)s_act.goal / scale);
    gy = y0 + floorf((gy - y0) / pitch + 0.5f) * pitch;
    if (pb > 0.3f && gy > y0 && gy < y0 + chart_h) {
        float gx0 = UI_CX - 3 * slot - pitch, gx1 = UI_CX + 3 * slot + pitch;
        dot_bar(fb, gx0, gy, 41, (gx1 - gx0) / 40.0f, 2.2f, 41, C_DIS, 0);
        ui_label(fb, (int)(gx1 + 12), (int)(gy + 6), "GOAL", C_DIS, UI_LEFT);
    }
    for (int i = 0; i < 7; i++) {
        uint32_t v = i == 6 ? (uint32_t)s_act.steps : s_act.week[i];
        if (i == 6 && s_act.week[6] > v) v = s_act.week[6];
        float g = bezier_ease(&EASE_STANDARD, m_stagger(pb, i, 7, 0.5f));
        uint8_t q = col_value(scale > 0 ? (float)v / scale : 0, rows, g, v > 0);
        uint8_t vals[2] = { q, q };
        float cx = UI_CX + (i - 3) * slot;
        dot_columns(fb, cx - pitch, y0, 2, rows, pitch, d, vals, i == 6 ? C_WHITE : C_SEC, C_GHOST);
        static const char *k_d = "SMTWTFS";
        char l[2] = { k_d[((s_wday - (6 - i)) % 7 + 7) % 7], 0 };
        ui_label(fb, (int)cx, (int)(y0 + chart_h + 32), l, i == 6 ? C_WHITE : C_DIS, UI_CENTER);
    }
}

static void act_draw(gfx_fb *fb)
{
    float off = vp_off(&A.vp);
    for (int pg = 0; pg < NPAGES; pg++) {
        float oy = pg * PAGE_H - off;
        if (oy <= -PAGE_H || oy >= PAGE_H) continue;
        if (pg == 0) draw_hero(fb, oy, &A.hero, 0);
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
 * Builds (0.30 s, the same as the app) as soon as the page slides into view; off screen
 * it rests unbuilt so the next arrival builds again. */
static struct {
    int started;
    hero hero;
} W;

static void wg_shown(void)
{
    if (W.started) return;              /* already building since the page slid in */
    read_data();
    hero_start(&W.hero, 0);
    W.started = 1;
}

static void wg_tick(float dt, int visible)
{
    read_data();
    if (!visible) { W.started = 0; hero_start(&W.hero, 0); return; }
    if (!W.started) { hero_start(&W.hero, 0); W.started = 1; }
    hero_tick(&W.hero, dt, 0);
}

static void wg_draw(gfx_fb *fb)
{
    draw_hero(fb, 0, &W.hero, 1);
}

static void wg_tap(int x, int y) { nos_open(&scr_activity, x, y); }

const nos_widget wg_activity = { "ACTIVITY", wg_tick, wg_draw, wg_tap, wg_shown, 0 };
