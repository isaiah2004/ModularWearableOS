/* time_timer.c — Timer app (presets, custom picker, running ring), the TIME'S UP ring
 * screen and the home "Timer" glance (wg_timer). Team: time.
 *
 * Also hosts two helpers the other time_* files use (declared extern there):
 *  - tdrum_*: the vertical drum picker (1:1 drag, momentum, snap, a tick per detent,
 *    FB_LIMIT at the ends), used by the custom timer and the alarm editor.
 *  - time_pulse_draw: the outward dot-ring pulse of the ringing screens.
 *
 * State lives in svc (svc_timer_*). Until the services team lands the real service the
 * stub never changes state, so a thin local fallback (TL) runs the timer from
 * hal_millis(); the first start probes which one is alive. */
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

/* from time_stopwatch.c / time_alarm.c */
const svc_stopwatch_t *time_sw(void);
int time_alarm_next(int *h, int *m, int *in_min);

static float qz(float v, float n) { return floorf(v * n + 0.5f) / n; }
static float ease_o(float t) { t = m_clamp(t, 0, 1); float u = 1 - t; return 1 - u * u * u; }

/* A dot ring like dot_ring() (n dots from 12 o'clock clockwise, [0, lit) lit with a
 * partially grown end dot, others `off`, 0 = skip). One damage command per dot
 * (dot_ring records up to 16 arc chunks): a draining tip repaints one dot. */
void time_ring(gfx_fb *fb, float cx, float cy, float r, int n, float d, float lit,
               uint16_t on, uint16_t off)
{
    lit = floorf(lit * 32.0f + 0.5f) / 32.0f;
    for (int i = 0; i < n; i++) {
        float a = 6.2831853f * (float)i / (float)n;
        float x = cx + r * gfx_fast_sin(a), y = cy - r * gfx_fast_cos(a);
        float f = m_clamp(lit - (float)i, 0, 1);
        if (f >= 1) { dot_disc(fb, x, y, d, on); continue; }
        if (off) dot_disc(fb, x, y, d, off);
        /* the end dot always records its own command (a vanishing 0.05 px dot when the
         * end sits exactly on a boundary) so the command count never changes while the
         * ring drains: an unchanged count keeps the damage diff per-dot */
        if (i == (int)lit) dot_disc(fb, x, y, f > 0 ? d * gfx_fast_sqrt(f) : 0.05f, on);
    }
}

/* =============================================================== drum picker */
#define TDRUM_N 4
enum { DR_IDLE = 0, DR_HOLD, DR_DRAG, DR_GLIDE, DR_SNAP };
typedef struct {
    int lo, hi;
    float row, pc, pn;       /* px per value; dot pitch of the centre / neighbour values */
    float cx, cy;
    float x, v;              /* position in px (value * row) */
    int mode;
    float x0, target, tau;
    mspring sp;
    int idx, over, limit_on_land, pressed, was_moving;
} tdrum;
static tdrum s_dr[TDRUM_N];

void tdrum_setup(int id, int lo, int hi, int value, float row, float pc, float pn)
{
    tdrum *d = &s_dr[id];
    memset(d, 0, sizeof *d);
    d->lo = lo; d->hi = hi; d->row = row; d->pc = pc; d->pn = pn;
    if (value < lo) value = lo;
    if (value > hi) value = hi;
    d->x = value * row;
    d->idx = value;
    msp_init(&d->sp, d->x, SPRING_SNAPPY);
}
void tdrum_place(int id, float cx, float cy) { s_dr[id].cx = cx; s_dr[id].cy = cy; }
int tdrum_value(int id)
{
    const tdrum *d = &s_dr[id];
    float x = (d->mode == DR_GLIDE || d->mode == DR_SNAP) ? d->target : d->x;
    int v = (int)floorf(x / d->row + 0.5f);
    return v < d->lo ? d->lo : v > d->hi ? d->hi : v;
}
int tdrum_hit(int id, int x, int y)
{
    const tdrum *d = &s_dr[id];
    return fabsf(x - d->cx) < 66 && fabsf(y - d->cy) < d->row * 1.75f;
}
int tdrum_busy(int id) { return s_dr[id].mode != DR_IDLE; }

static void dr_snap_to(tdrum *d, float tgt, float v)
{
    d->target = tgt;
    d->sp.x = d->x; d->sp.v = v;
    d->sp.response = 0.26f; d->sp.damping = 1.0f;
    msp_to(&d->sp, tgt);
    d->mode = DR_SNAP;
}

static void dr_release(tdrum *d, float v)
{
    float lo = d->lo * d->row, hi = d->hi * d->row;
    if (d->x < lo || d->x > hi) { dr_snap_to(d, d->x < lo ? lo : hi, v * 0.5f); return; }
    /* a picker wheel is heavier than a list: project with a 0.25 s decay, and treat
     * slow releases as "put it here" (snap to the nearest detent, no projection) */
    float rest = fabsf(v) < 260.0f ? d->x : d->x + v * 0.25f;
    float k = floorf(rest / d->row + 0.5f);
    float tgt = m_clamp(k * d->row, lo, hi);
    d->limit_on_land = (rest < lo - d->row * 0.5f || rest > hi + d->row * 0.5f);
    float D = tgt - d->x;
    if (fabsf(v) >= 260.0f && D * v > 0 && fabsf(D) > d->row * 0.3f) {
        d->tau = m_clamp(D / v, 0.04f, 0.9f);
        d->target = tgt;
        d->mode = DR_GLIDE;           /* exponential glide that lands exactly on the detent */
        return;
    }
    dr_snap_to(d, tgt, v);
}

int tdrum_input(int id, const nos_input *in)
{
    tdrum *d = &s_dr[id];
    switch (in->type) {
    case IN_DOWN:
        d->was_moving = (d->mode == DR_GLIDE || d->mode == DR_SNAP) && fabsf(d->v) > 80.0f;
        d->mode = DR_HOLD; d->v = 0; d->x0 = d->x; d->pressed = 1; d->over = 0;
        return 1;
    case IN_DRAG_START:
    case IN_DRAG: {
        if (in->type == IN_DRAG_START && in->axis != AXIS_Y) return 0;
        if (d->mode != DR_HOLD && d->mode != DR_DRAG) return 0;
        d->mode = DR_DRAG;
        float raw = d->x0 - in->dy, lo = d->lo * d->row, hi = d->hi * d->row;
        /* pickers barely give past their ends (~25 px): the band never empties */
        if (raw < lo) raw = lo + rubber_band(raw - lo, d->row * 0.4f);
        else if (raw > hi) raw = hi + rubber_band(raw - hi, d->row * 0.4f);
        d->x = raw;
        return 1;
    }
    case IN_DRAG_END:
        if (d->mode != DR_DRAG) return 0;
        dr_release(d, -in->vy);
        return 1;
    case IN_TAP: {
        float lo = d->lo * d->row, hi = d->hi * d->row;
        float cur = m_clamp(floorf(d->x / d->row + 0.5f) * d->row, lo, hi);
        float off = (in->y - d->cy) / d->row;
        if (!d->was_moving && fabsf(off) > 0.55f) {        /* tap a neighbour: step to it */
            float tgt = cur + (off > 0 ? d->row : -d->row);
            if (tgt < lo || tgt > hi) { nos_feedback(FB_LIMIT); tgt = cur; }
            dr_snap_to(d, tgt, 0);
        } else dr_snap_to(d, cur, 0);
        return 1;
    }
    case IN_UP:
        d->pressed = 0;
        if (d->mode == DR_HOLD) {
            float lo = d->lo * d->row, hi = d->hi * d->row;
            dr_snap_to(d, m_clamp(floorf(d->x / d->row + 0.5f) * d->row, lo, hi), 0);
        } else if (d->mode == DR_DRAG) dr_release(d, 0);
        return 1;
    default:
        return 0;
    }
}

void tdrum_tick(int id, float dt)
{
    tdrum *d = &s_dr[id];
    if (d->mode == DR_GLIDE) {
        float e = expf(-dt / d->tau);
        float nx = d->target - (d->target - d->x) * e;
        d->v = (nx - d->x) / dt;
        d->x = nx;
        if (fabsf(d->target - d->x) < d->row * 0.2f) dr_snap_to(d, d->target, d->v);
    } else if (d->mode == DR_SNAP) {
        int m = msp_step(&d->sp, dt);
        d->x = d->sp.x; d->v = d->sp.v;
        if (!m) {
            d->x = d->target; d->v = 0; d->mode = DR_IDLE;
            if (d->limit_on_land) { nos_feedback(FB_LIMIT); d->limit_on_land = 0; }
        }
    }
    int idx = (int)floorf(d->x / d->row + 0.5f);
    if (idx < d->lo) idx = d->lo;
    if (idx > d->hi) idx = d->hi;
    if (idx != d->idx) { d->idx = idx; nos_feedback(FB_TICK); }
    if (d->mode == DR_DRAG) {
        int beyond = d->x < d->lo * d->row - 2.0f || d->x > d->hi * d->row + 2.0f;
        if (beyond && !d->over) nos_feedback(FB_LIMIT);
        d->over = beyond;
    }
}

/* build: 0..1 entry reveal */
void tdrum_draw(gfx_fb *fb, int id, float build)
{
    const tdrum *d = &s_dr[id];
    /* selection band: the drum's touch surface, brighter while held */
    float bw = d->pc * 11 + 34, bh = d->pc * 9 + 24;
    if (build > 0.05f)
        ui_rrect(fb, d->cx - bw / 2, d->cy - bh / 2, bw, bh, 28,
                 ui_dim(d->pressed ? C_SURF2 : C_SURF, qz(m_clamp(build * 2, 0, 1), 8)));
    float pos = d->x / d->row;
    int base = (int)floorf(pos);
    for (int val = base - 2; val <= base + 3; val++) {
        if (val < d->lo || val > d->hi) continue;
        float dd = val - pos, ad = fabsf(dd);
        if (ad > 1.62f) continue;
        float t = ad < 1 ? ad : 1;
        t = t * t * (3 - 2 * t);
        float pitch = qz(m_lerp(d->pc, d->pn, t), 4);
        uint16_t col = ui_mix(C_WHITE, C_DIS, qz(t, 16));
        if (ad > 1) col = ui_dim(C_DIS, qz(1 - (ad - 1) / 0.62f, 16));
        dot_style st = { DOT_FONT_TALL, pitch, pitch * 0.8f, col, 0, 1, 1 };
        char b[8];
        snprintf(b, sizeof b, "%02d", val);
        float y = d->cy + dd * d->row - pitch * 4.5f;
        if (build >= 1) dot_text_a(fb, &st, d->cx, y, b, 1);
        else dot_text_anim(fb, &st, d->cx, y, b, 1, build, DOT_REVEAL_RANDOM, (unsigned)(id * 7 + val));
    }
}

/* ======================================================== ringing pulse */
/* The one red element of a ringing screen: the alert marker at 12 o'clock of the
 * outer pulse ring (covers that ring's own dot). */
void time_alert_dot(gfx_fb *fb) { dot_disc(fb, UI_CX, UI_CY - 211.0f, 9, C_RED); }

/* Three concentric dot rings near the bezel; a brightness wave runs outward. phase in
 * seconds. Brightness is quantised so the ring repaints in steps. */
void time_pulse_draw(gfx_fb *fb, float phase, int rings)
{
    static const float R[3] = { 165, 188, 211 };
    static const int   N[3] = { 48, 54, 60 };
    const float period = 1.4f;
    for (int k = 3 - rings; k < 3; k++) {
        float t = fmodf(phase / period - k * 0.2f + 4.0f, 1.0f);   /* 0..1 per cycle */
        float b = t < 0.12f ? t / 0.12f : 1 - (t - 0.12f) / 0.6f;
        b = m_clamp(b, 0, 1);
        b = qz(b, 12);
        uint16_t col = ui_mix(C_LINE2, C_WHITE, b);
        float d = 6.0f + b * 2.0f;
        time_ring(fb, UI_CX, UI_CY, R[k], N[k], qz(d, 2), (float)N[k], col, 0);
    }
}

/* ===================================================== timer state + fallback */
typedef struct { int state; uint32_t total, left; } tm_t;
static struct { int state; uint32_t total, left, end_ms; } TL;   /* local fallback */
static int s_svc = -1;           /* -1 unknown, 0 stub (use TL), 1 real svc */
static int s_probe;

static void tm_get(tm_t *o)
{
    if (s_svc != 1 && TL.state) {
        o->state = TL.state; o->total = TL.total;
        if (TL.state == 1) {
            int32_t l = (int32_t)(TL.end_ms - hal_millis());
            o->left = l > 0 ? (uint32_t)l : 0;
        } else o->left = TL.state == 3 ? 0 : TL.left;
        return;
    }
    if (s_svc == 0) { memset(o, 0, sizeof *o); return; }
    const svc_timer_t *t = svc_timer();
    o->state = t->state; o->total = t->total_ms; o->left = t->left_ms;
}
static void tm_start(uint32_t ms)
{
    svc_timer_start(ms);
    if (s_svc != 1) {
        TL.state = 1; TL.total = ms; TL.left = ms; TL.end_ms = hal_millis() + ms;
        s_probe = 0;
    }
}
static void tm_pause(void)
{
    svc_timer_pause();
    if (s_svc != 1 && TL.state == 1) {
        int32_t l = (int32_t)(TL.end_ms - hal_millis());
        TL.left = l > 0 ? (uint32_t)l : 0; TL.state = 2;
    }
}
static void tm_resume(void)
{
    svc_timer_resume();
    if (s_svc != 1 && TL.state == 2) { TL.end_ms = hal_millis() + TL.left; TL.state = 1; }
}
static void tm_cancel(void)
{
    svc_timer_cancel();
    if (s_svc != 1) TL.state = 0;
}
static void tm_add(uint32_t ms)
{
    svc_timer_add(ms);
    if (s_svc != 1 && TL.state) {
        TL.total += ms;
        if (TL.state == 1) TL.end_ms += ms; else TL.left += ms;
    }
}
/* Once per frame from every timer surface: settle which backend is alive, and in
 * fallback mode fire the ring screen when the local timer reaches zero. */
static void tm_poll(void)
{
    if (s_svc < 0 && TL.state) {
        if (svc_timer()->state != 0) { s_svc = 1; TL.state = 0; }
        else if (++s_probe >= 3) s_svc = 0;
    }
    if (s_svc != 1 && TL.state == 1 && (int32_t)(TL.end_ms - hal_millis()) <= 0) {
        TL.state = 3;
        if (nos_top() != &scr_timer_ring) nos_push(&scr_timer_ring, TX_RISE);
    }
}
static uint32_t tm_secs(uint32_t ms) { return (ms + 999) / 1000; }   /* 4:59.2 shows 5:00 */
static void tm_fmt(char *b, int n, uint32_t ms)
{
    uint32_t s = tm_secs(ms);
    if (s >= 3600) snprintf(b, (size_t)n, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    else snprintf(b, (size_t)n, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}
static void tm_total_label(char *b, int n, uint32_t ms)
{
    uint32_t s = (ms + 500) / 1000;
    if (s >= 3600 && s % 60 == 0) snprintf(b, (size_t)n, "%u H %u MIN", (unsigned)(s / 3600), (unsigned)(s / 60 % 60));
    else if (s < 60) snprintf(b, (size_t)n, "%u SEC", (unsigned)s);
    else if (s % 60 == 0) snprintf(b, (size_t)n, "%u MIN", (unsigned)(s / 60));
    else snprintf(b, (size_t)n, "%u MIN %u SEC", (unsigned)(s / 60), (unsigned)(s % 60));
}

/* ======================================== per-character digits with morphs */
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
static float dg_cw(const dot_style *st, char c)
{
    char b[2] = { c, 0 };
    return (float)dot_text_width(st, b);
}
static float dg_width(const dot_style *st, const char *s)
{
    float w = 0; int n = 0;
    for (; s[n]; n++) w += dg_cw(st, s[n]);
    return n ? w + (n - 1) * st->pitch : 0;
}
/* Each glyph is its own command so a ticking second repaints one digit. `dim_n` leading
 * characters draw in dim_col. build < 1 runs the reveal instead. */
static void dgm_draw(gfx_fb *fb, const dgm *m, const dot_style *st0, float cx, float y,
                     float dur, int dim_n, uint16_t dim_col, float build)
{
    float x = cx - dg_width(st0, m->cur) * 0.5f;
    x = floorf(x + 0.5f);
    for (int i = 0; m->cur[i]; i++) {
        dot_style st = *st0;
        if (i < dim_n) st.color = dim_col;
        char a[2] = { m->from[i], 0 }, b[2] = { m->cur[i], 0 };
        float p = (nos_t - m->t0[i]) / dur;
        if (build < 1) dot_text_anim(fb, &st, x, y, b, 0, build, DOT_REVEAL_RANDOM, (unsigned)(i * 13 + 5));
        else if (p < 1 && a[0] != b[0]) dot_text_morph(fb, &st, x, y, a, b, 0, p);
        else dot_text(fb, &st, x, y, b);
        x += dg_cw(&st, m->cur[i]) + st.pitch;
    }
}

/* ====================================================== hit helpers */
static int in_circle(int x, int y, float cx, float cy, float r)
{
    float dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}
static int in_rect(int x, int y, float cx, float cy, float w, float h)
{
    return fabsf(x - cx) <= w * 0.5f && fabsf(y - cy) <= h * 0.5f;
}

/* ============================================================= timer app */
static const uint16_t k_presets[6] = { 1, 3, 5, 10, 15, 30 };
#define PILL_W 116
#define PILL_H 88
static float preset_cx(int i) { return UI_CX + (i % 3 - 1) * 128.0f; }
static float preset_cy(int i) { return 170.0f + (i / 3) * 100.0f; }
#define CUSTOM_CY ((float)UI_BTN_Y)
#define CUSTOM_W  ((float)UI_BTN_W)

/* run view geometry */
#define RING_R    211.0f
#define RING_D    7.0f
#define BTN_Y     344.0f
#define BTN_SIDE  104.0f
#define DIG_TOP   184.0f
#define OUT_S     0.15f            /* outgoing view departs over 150 ms, then the new one builds */
#define PAUSE_S   0.15f

/* A white primary surface builds in by scale steps at full colour (a dimmed white would
 * read as its pressed grey). k 0..1. */
static float build_scale(float k) { return 0.72f + 0.28f * qz(m_clamp(k, 0, 1), 8); }

enum { V_SETUP = 0, V_RUN };
enum { P_NONE = -1, P_CUSTOM = 6, P_CANCEL = 10, P_PAUSE, P_ADD };
static struct {
    int view, prev, out_on;
    float out_t0, build_t0, build, out;
    int dig_keep;           /* digits already on screen (opened from the widget): no reveal */
    int pressed;
    tm_t tm;
    dgm dig;
    float lit;              /* ring lit dots (fractional), frozen once cancelled */
    int last10;
    char total_lbl[24];
    float pz;               /* 0 running .. 1 paused, 150 ms colour/label/icon step */
} T;

static void timer_load_digits(int morph)
{
    char b[12];
    tm_fmt(b, sizeof b, T.tm.left);
    dgm_set(&T.dig, b, morph);
}
/* fresh: no outgoing phase (the screen just appeared) */
static void timer_set_view(int v, int fresh)
{
    if (!fresh && v != T.view) { T.prev = T.view; T.out_on = 1; T.out_t0 = nos_t; T.build_t0 = nos_t + OUT_S; }
    else { T.out_on = 0; T.build_t0 = nos_t; }
    T.view = v;
    T.build = 0;
    T.out = T.out_on ? 1.0f : 0.0f;
    T.pressed = P_NONE;
    if (v == V_RUN) {
        timer_load_digits(0);
        tm_total_label(T.total_lbl, sizeof T.total_lbl, T.tm.total);
        T.pz = T.tm.state == 2 ? 1.0f : 0.0f;
    }
}

static float s_timer_seen = -10;   /* nos_t of the last tick: visible under a transition */
static void timer_enter(void)
{
    tm_poll();
    tm_get(&T.tm);
    int v = T.tm.state ? V_RUN : V_SETUP;
    T.pressed = P_NONE;
    /* revealed by a pop it was already on screen (ticked under the transition): keep it */
    if (nos_t - s_timer_seen < 0.2f && T.view == v) return;
    timer_set_view(v, 1);
    T.dig_keep = v == V_RUN;       /* a running timer's digits are already known (widget) */
    T.build_t0 = nos_t + UI_ENTRY_S * 0.55f;   /* iris entries build from ~40% of the iris */
}

static void timer_tick(float dt)
{
    s_timer_seen = nos_t;
    tm_poll();
    tm_get(&T.tm);
    if (T.view == V_RUN && T.tm.state == 0) { T.dig_keep = 0; timer_set_view(V_SETUP, 0); }
    else if (T.view == V_SETUP && T.tm.state != 0) { T.dig_keep = 0; timer_set_view(V_RUN, 0); }
    T.out = T.out_on ? 1.0f - bezier_ease(&EASE_ACCEL, m_clamp((nos_t - T.out_t0) / OUT_S, 0, 1)) : 0;
    if (T.out_on && T.out <= 0) T.out_on = 0;
    T.build = m_clamp((nos_t - T.build_t0) / (T.view == V_RUN ? 0.42f : 0.36f), 0, 1);
    if (T.view == V_RUN && T.tm.state != 0) {
        timer_load_digits(1);
        uint32_t tot = T.tm.total > T.tm.left ? T.tm.total : T.tm.left;
        T.lit = tot ? 60.0f * (float)T.tm.left / (float)tot : 0;
        T.last10 = T.tm.state == 1 && T.tm.left > 0 && T.tm.left <= 10000;
        tm_total_label(T.total_lbl, sizeof T.total_lbl, T.tm.total);
        float tgt = T.tm.state == 2 ? 1.0f : 0.0f, step = dt / PAUSE_S;
        T.pz = T.pz < tgt ? fminf(tgt, T.pz + step) : fmaxf(tgt, T.pz - step);
        if (T.tm.state == 1) nos_keep_awake();
    }
}

static void timer_draw_setup(gfx_fb *fb, float b)
{
    if (b > 0) ui_title(fb, "TIMER", (1 - qz(m_clamp(b * 2, 0, 1), 6)) * 60);
    for (int i = 0; i < 6; i++) {
        float p = m_stagger(b, i, 7, 0.5f);
        if (p <= 0) continue;
        float cx = preset_cx(i), cy = preset_cy(i);
        uint16_t fill = T.pressed == i ? C_SURF3 : C_SURF2;
        ui_rrect(fb, cx - PILL_W / 2, cy - PILL_H / 2, PILL_W, PILL_H, PILL_H / 2, ui_dim(fill, qz(m_clamp(p * 2, 0, 1), 8)));
        char t[4];
        snprintf(t, sizeof t, "%d", k_presets[i]);
        dot_style st = { DOT_FONT_TALL, 4.4f, 3.5f, C_WHITE, 0, 1, 1 };
        dot_text_anim(fb, &st, cx, cy - 27, t, 1, p, DOT_REVEAL_RANDOM, (unsigned)i);
        if (p >= 0.6f) ui_label(fb, (int)cx, (int)cy + 31, "MIN", C_SEC, UI_CENTER);
    }
    float p = m_stagger(b, 6, 7, 0.5f);
    if (p > 0) {
        uint16_t fill = T.pressed == P_CUSTOM ? C_SURF3 : C_SURF2;
        ui_rrect(fb, UI_CX - CUSTOM_W / 2, CUSTOM_CY - PILL_H / 2, CUSTOM_W, PILL_H, PILL_H / 2,
                 ui_dim(fill, qz(m_clamp(p * 2, 0, 1), 8)));
        if (p >= 0.5f) ui_label(fb, UI_CX, (int)CUSTOM_CY + 6, "CUSTOM", C_PRI, UI_CENTER);
    }
}

static void timer_draw_run(gfx_fb *fb, float b, float dig_b)
{
    float lit = T.lit * ease_o(b);
    if (b > 0) ui_title(fb, "TIMER", (1 - qz(m_clamp(b * 2, 0, 1), 6)) * 60);
    /* ring: lit dots drain anticlockwise toward 12; the last 10 s tip turns red */
    if (T.last10) {
        float fl = floorf(lit);
        time_ring(fb, UI_CX, UI_CY, RING_R, 60, RING_D, fl, C_WHITE, C_LINE2);
        float f = qz(lit - fl, 32);
        float a = 6.2831853f * fl / 60.0f;     /* always recorded: a steady command count */
        dot_disc(fb, UI_CX + RING_R * gfx_fast_sin(a), UI_CY - RING_R * gfx_fast_cos(a),
                 f > 0 ? RING_D * gfx_fast_sqrt(f) + 1.0f : 0.05f, C_RED);
    } else
        time_ring(fb, UI_CX, UI_CY, RING_R, 60, RING_D, lit, C_WHITE, C_LINE2);

    /* label: the total, crossfading to [ PAUSED ] through the pause step */
    float pz = qz(T.pz, 6);
    float lp = qz(m_clamp(b * 3 - 0.6f, 0, 1), 8);
    if (lp > 0) {
        if (pz < 0.5f) ui_label(fb, UI_CX, 150, T.total_lbl, ui_dim(C_SEC, lp * (1 - pz * 2)), UI_CENTER);
        else ui_state(fb, UI_CX, 150, "PAUSED", ui_dim(C_SEC, lp * (pz * 2 - 1)));
    }
    /* hero of 5-8 glyphs (MM:SS, H:MM:SS): the house pitch 8 */
    dot_style st = { DOT_FONT_TALL, UI_HEROW_PITCH, UI_HEROW_PITCH * 0.8f, ui_mix(C_WHITE, C_SEC, pz), 0, 1, 1 };
    dgm_draw(fb, &T.dig, &st, UI_CX, DIG_TOP, 0.28f, T.last10 ? 3 : 0, C_DIS, dig_b);

    /* controls */
    float bp = m_clamp(b * 2.5f - 1.0f, 0, 1);
    if (bp <= 0) return;
    float k = qz(bp, 8);
    float lx = UI_CX - BTN_SIDE, rx = UI_CX + BTN_SIDE, sy = BTN_Y - 6;
    ui_circle(fb, lx, sy, 40, ui_dim(T.pressed == P_CANCEL ? C_SURF3 : C_SURF2, k));
    dot_icon(fb, DOT_ICON_CROSS, lx, sy, 2.6f, 2.1f, ui_dim(C_PRI, k));
    uint16_t disc = T.pressed == P_PAUSE ? C_SEC : C_WHITE;
    ui_circle(fb, UI_CX, BTN_Y, 46 * build_scale(bp), disc);
    /* pause <-> play: one icon fades into the disc, the other fades out of it */
    float ik = pz < 0.5f ? 1 - pz * 2 : pz * 2 - 1;
    if (k >= 1) dot_icon(fb, pz >= 0.5f ? DOT_ICON_PLAY : DOT_ICON_PAUSE, UI_CX + (pz >= 0.5f ? 2 : 0), BTN_Y,
                         3.0f, 2.4f, ui_mix(disc, C_BG, ik));
    ui_circle(fb, rx, sy, 40, ui_dim(T.pressed == P_ADD ? C_SURF3 : C_SURF2, k));
    dot_style ps = { DOT_FONT_5X7, 2.8f, 2.3f, ui_dim(C_PRI, k), 0, 1, 0 };
    dot_text_a(fb, &ps, rx, sy - 17, "+1", 1);
    ui_text(fb, UI_F_SMALL, (int)rx, (int)sy + 21, "MIN", ui_dim(C_SEC, k), UI_CENTER, 1);
}

static void timer_draw_view(gfx_fb *fb, int v, float b, float dig_b)
{
    if (v == V_SETUP) timer_draw_setup(fb, b);
    else timer_draw_run(fb, b, dig_b);
}
static void timer_draw(gfx_fb *fb)
{
    if (T.out_on) { timer_draw_view(fb, T.prev, T.out, T.out); return; }
    timer_draw_view(fb, T.view, T.build, T.dig_keep ? 1.0f : T.build);
}

static int timer_hit(int x, int y)
{
    if (T.out_on) return P_NONE;
    if (T.view == V_SETUP) {
        for (int i = 0; i < 6; i++)
            if (in_rect(x, y, preset_cx(i), preset_cy(i), PILL_W + 8, PILL_H + 8)) return i;
        if (in_rect(x, y, UI_CX, CUSTOM_CY, CUSTOM_W + 8, PILL_H + 8)) return P_CUSTOM;
        return P_NONE;
    }
    if (in_circle(x, y, UI_CX, BTN_Y, 50)) return P_PAUSE;
    if (in_circle(x, y, UI_CX - BTN_SIDE, BTN_Y - 6, 48)) return P_CANCEL;
    if (in_circle(x, y, UI_CX + BTN_SIDE, BTN_Y - 6, 48)) return P_ADD;
    return P_NONE;
}

extern const nos_screen scr_timer_pick;

static int timer_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN: T.pressed = timer_hit(in->x, in->y); return T.pressed != P_NONE;
    case IN_DRAG_START: T.pressed = P_NONE; return 0;
    case IN_UP: T.pressed = P_NONE; return 0;
    case IN_TAP: {
        int h = timer_hit(in->x, in->y);
        T.pressed = P_NONE;
        if (h == P_NONE) return 0;
        if (h >= 0 && h < 6) {
            nos_feedback(FB_TAP);
            tm_start(k_presets[h] * 60000u);
            tm_get(&T.tm);
            T.dig_keep = 0;
            timer_set_view(V_RUN, 0);
        } else if (h == P_CUSTOM) {
            nos_feedback(FB_TAP);
            nos_push(&scr_timer_pick, TX_PUSH);
        } else if (h == P_PAUSE) {
            if (T.tm.state == 1) { tm_pause(); nos_feedback(FB_TOGGLE_OFF); }
            else if (T.tm.state == 2) { tm_resume(); nos_feedback(FB_TOGGLE_ON); }
        } else if (h == P_CANCEL) {
            nos_feedback(FB_TAP);
            tm_cancel();
            tm_get(&T.tm);
            T.dig_keep = 0;
            timer_set_view(V_SETUP, 0);
        } else if (h == P_ADD) {
            nos_feedback(FB_TAP);
            tm_add(60000);
        }
        return 1;
    }
    default: return 0;
    }
}

const nos_screen scr_timer = { "TIMER", timer_enter, 0, timer_tick, timer_draw, timer_input, 0 };

/* ============================================================ custom picker */
#define PK_CY   192.0f
#define PK_DX   72.0f
#define PK_BTN_Y ((float)UI_BTN_Y)
static struct { float build_t0, build; int pressed; int target; int mins, secs; int init; } PK;
enum { PKP_NONE = 0, PKP_START };

static void pick_enter(void)
{
    if (!PK.init) { PK.mins = 10; PK.secs = 0; PK.init = 1; }
    tdrum_setup(0, 0, 99, PK.mins, 64, 8, 4);
    tdrum_setup(1, 0, 59, PK.secs, 64, 8, 4);
    PK.build_t0 = nos_t; PK.build = 0; PK.target = -1; PK.pressed = PKP_NONE;
}
static void pick_leave(void) { PK.mins = tdrum_value(0); PK.secs = tdrum_value(1); }
static void pick_tick(float dt)
{
    tdrum_place(0, UI_CX - PK_DX, PK_CY);
    tdrum_place(1, UI_CX + PK_DX, PK_CY);
    tdrum_tick(0, dt);
    tdrum_tick(1, dt);
    tm_poll();
    PK.build = m_clamp((nos_t - PK.build_t0) / 0.36f, 0, 1);
}
static int pick_secs(void) { return tdrum_value(0) * 60 + tdrum_value(1); }
static void pick_draw(gfx_fb *fb)
{
    ui_title(fb, "CUSTOM", 0);
    tdrum_draw(fb, 0, PK.build);
    tdrum_draw(fb, 1, PK.build);
    dot_style cs = { DOT_FONT_TALL, 8, 6.4f, C_SEC, 0, 1, 1 };
    if (PK.build > 0.3f) dot_text_a(fb, &cs, UI_CX, PK_CY - 36, ":", 1);
    float lp = qz(m_clamp(PK.build * 2 - 0.5f, 0, 1), 8);
    if (lp > 0) {
        ui_label(fb, (int)(UI_CX - PK_DX), 300, "MIN", ui_dim(C_SEC, lp), UI_CENTER);
        ui_label(fb, (int)(UI_CX + PK_DX), 300, "SEC", ui_dim(C_SEC, lp), UI_CENTER);
        int ok = pick_secs() > 0;
        float sc = build_scale(lp), w = UI_BTN_W * sc, h = UI_BTN_H * sc;
        uint16_t fill = !ok ? C_SURF : PK.pressed == PKP_START ? C_SEC : C_WHITE;
        ui_rrect(fb, UI_CX - w / 2, PK_BTN_Y - h / 2, w, h, h / 2, fill);
        if (lp >= 1) ui_label(fb, UI_CX, (int)PK_BTN_Y + 6, "START", ok ? C_BG : C_DIS, UI_CENTER);
    }
}
static int pick_input(const nos_input *in)
{
    if (in->type == IN_DOWN) {
        PK.target = -1; PK.pressed = PKP_NONE;
        if (tdrum_hit(0, in->x, in->y)) PK.target = 0;
        else if (tdrum_hit(1, in->x, in->y)) PK.target = 1;
        else if (in_rect(in->x, in->y, UI_CX, PK_BTN_Y, UI_BTN_W + 8, 96)) PK.pressed = PKP_START;
        if (PK.target >= 0) return tdrum_input(PK.target, in);
        return PK.pressed != PKP_NONE;
    }
    if (PK.target >= 0) {
        int r = tdrum_input(PK.target, in);
        if (in->type == IN_UP) PK.target = -1;
        return r;
    }
    if (in->type == IN_DRAG_START || in->type == IN_UP) { PK.pressed = PKP_NONE; return 0; }
    if (in->type == IN_TAP && in_rect(in->x, in->y, UI_CX, PK_BTN_Y, UI_BTN_W + 8, 96)) {
        PK.pressed = PKP_NONE;
        int s = pick_secs();
        if (s <= 0) { nos_feedback(FB_ERROR); return 1; }
        nos_feedback(FB_TAP);
        tm_start((uint32_t)s * 1000u);
        nos_pop();
        return 1;
    }
    return 0;
}
const nos_screen scr_timer_pick = { "TIMER SET", pick_enter, pick_leave, pick_tick, pick_draw, pick_input, 0 };

/* ================================================================ ringing */
#define RG_BTN_Y 316.0f
#define RG_BTN_W 124.0f
static struct { float t0, build; int pressed; uint32_t total; int haptic_on; } RG;
enum { RGP_NONE = 0, RGP_STOP, RGP_ADD };

static void ring_enter(void)
{
    RG.t0 = nos_t; RG.build = 0; RG.pressed = RGP_NONE;
    tm_t t; tm_get(&t); RG.total = t.total;
    /* the timer service runs the alarm haptics/sound (svc ringer); only the local
     * fallback (link-stub svc) buzzes from here */
    RG.haptic_on = 0;
    if (s_svc == 0) {
        hal_haptic(HAPTIC_ALARM);
        if (nos_prefs.sound) hal_sfx(SFX_TIMER_DONE);
        RG.haptic_on = 1;
    }
    nos_keep_awake();
}
static void ring_leave(void)
{
    if (RG.haptic_on) { hal_haptic_stop(); hal_sfx_stop(); RG.haptic_on = 0; }
}
static void ring_tick(float dt)
{
    tm_poll();
    RG.build = m_clamp((nos_t - RG.t0) / 0.4f, 0, 1);
    nos_keep_awake();
    /* stop buzzing after a minute; the screen stays until dismissed */
    if (RG.haptic_on && nos_t - RG.t0 > 60.0f) { hal_haptic_stop(); RG.haptic_on = 0; }
}
static void ring_draw(gfx_fb *fb)
{
    time_pulse_draw(fb, nos_t - RG.t0, 1);   /* one ring: the title at y 70 stays clear */
    time_alert_dot(fb);
    ui_title(fb, "TIME'S UP", 0);
    dot_style st = { DOT_FONT_TALL, UI_HEROW_PITCH, UI_HEROW_PITCH * 0.8f, C_WHITE, 0, 1, 1 };
    dot_text_anim(fb, &st, UI_CX, 168, "00:00", 1, RG.build, DOT_REVEAL_CENTER, 3);
    float k = qz(m_clamp(RG.build * 2 - 0.6f, 0, 1), 8);
    if (k <= 0) return;
    float lx = UI_CX - 6 - RG_BTN_W / 2, rx = UI_CX + 6 + RG_BTN_W / 2;
    float sc = build_scale(k), w = RG_BTN_W * sc, h = UI_BTN_H * sc;
    ui_rrect(fb, lx - w / 2, RG_BTN_Y - h / 2, w, h, h / 2, RG.pressed == RGP_STOP ? C_SEC : C_WHITE);
    if (k >= 1) ui_label(fb, (int)lx, (int)RG_BTN_Y + 6, "STOP", C_BG, UI_CENTER);
    ui_rrect(fb, rx - RG_BTN_W / 2, RG_BTN_Y - 44, RG_BTN_W, 88, 44, ui_dim(RG.pressed == RGP_ADD ? C_SURF3 : C_SURF2, k));
    ui_label(fb, (int)rx, (int)RG_BTN_Y + 6, "+1 MIN", ui_dim(C_PRI, k), UI_CENTER);
}
static int ring_hit(int x, int y)
{
    if (in_rect(x, y, UI_CX - 6 - RG_BTN_W / 2, RG_BTN_Y, RG_BTN_W + 8, 96)) return RGP_STOP;
    if (in_rect(x, y, UI_CX + 6 + RG_BTN_W / 2, RG_BTN_Y, RG_BTN_W + 8, 96)) return RGP_ADD;
    return RGP_NONE;
}
static void ring_stop(void)
{
    tm_cancel();
    ring_leave();
    nos_pop();
}
static int ring_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN: RG.pressed = ring_hit(in->x, in->y); return 1;
    case IN_UP: case IN_DRAG_START: RG.pressed = RGP_NONE; return 1;
    case IN_TAP: {
        int h = ring_hit(in->x, in->y);
        RG.pressed = RGP_NONE;
        if (h == RGP_STOP) { nos_feedback(FB_TAP); ring_stop(); }
        else if (h == RGP_ADD) {
            nos_feedback(FB_TAP);
            tm_cancel();
            tm_start(60000);
            ring_leave();
            nos_pop();
        }
        return 1;
    }
    case IN_BTN: nos_feedback(FB_TAP); ring_stop(); return 1;
    default: return 1;
    }
}
const nos_screen scr_timer_ring = { "TIME'S UP", ring_enter, ring_leave, ring_tick, ring_draw, ring_input,
                                    SCR_NOBACK | SCR_KEEPAWAKE };

/* ============================================================== widget */
enum { WG_QUICK = 0, WG_TIMER, WG_SW, WG_ALARM };
/* widget rings stay inside r 160: the home pager owns the top centre (unread dot,
 * y <= 36) and the bottom centre (page dots, y >= 424), and the title sits at y 70 */
#define WG_RING_R 158.0f
static const uint16_t k_wq[3] = { 3, 5, 10 };
#define WQ_Y  236.0f
#define WQ_DX 124.0f
#define WQ_R  54.0f
static struct {
    int mode, prev, out_on;
    float t0, build, out_t0, out;
    tm_t tm;
    dgm dig[4];              /* per mode, so an outgoing mode keeps its own digits */
    float lit;
    uint32_t sw_ms;
    int al_h, al_m, al_in;
    char lbl[24], al_lbl[24];
    int flash; float flash_t;   /* quick circle tapped: its fill flashes */
    int pressed;                /* quick circle under the finger, -1 none */
} W = { .flash = -1, .pressed = -1 };

static int wg_mode_now(void)
{
    if (W.tm.state == 1 || W.tm.state == 2 || W.tm.state == 3) return WG_TIMER;
    const svc_stopwatch_t *sw = time_sw();
    if (sw->running) return WG_SW;
    if (time_alarm_next(&W.al_h, &W.al_m, &W.al_in)) return WG_ALARM;
    return WG_QUICK;
}

static void wg_tick(float dt, int visible)
{
    tm_poll();
    tm_get(&W.tm);
    int m = wg_mode_now();
    if (m != W.mode) {          /* the old glance departs (150 ms), then the new one builds */
        W.prev = W.mode; W.mode = m;
        W.out_on = visible; W.out_t0 = nos_t;
        W.t0 = nos_t + (visible ? OUT_S : 0);
    }
    W.out = W.out_on ? 1.0f - bezier_ease(&EASE_ACCEL, m_clamp((nos_t - W.out_t0) / OUT_S, 0, 1)) : 0;
    if (W.out_on && W.out <= 0) W.out_on = 0;
    W.build = m_clamp((nos_t - W.t0) / 0.4f, 0, 1);
    char b[16];
    if (W.mode == WG_TIMER) {
        tm_fmt(b, sizeof b, W.tm.left);
        dgm_set(&W.dig[WG_TIMER], b, 1);
        uint32_t tot = W.tm.total > W.tm.left ? W.tm.total : W.tm.left;
        W.lit = tot ? 60.0f * (float)W.tm.left / (float)tot : 0;
        tm_total_label(W.lbl, sizeof W.lbl, W.tm.total);
    } else if (W.mode == WG_SW) {
        W.sw_ms = time_sw()->elapsed_ms;
        uint32_t s = W.sw_ms / 1000;
        if (s >= 3600) snprintf(b, sizeof b, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
        else snprintf(b, sizeof b, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
        dgm_set(&W.dig[WG_SW], b, 1);
    } else if (W.mode == WG_ALARM) {
        ui_fmt_time(b, sizeof b, W.al_h, W.al_m);
        dgm_set(&W.dig[WG_ALARM], b, 1);
        if (W.al_in >= 60) snprintf(W.al_lbl, sizeof W.al_lbl, "IN %dH %dM", W.al_in / 60, W.al_in % 60);
        else snprintf(W.al_lbl, sizeof W.al_lbl, "IN %d MIN", W.al_in < 1 ? 1 : W.al_in);
    }
}
static void wg_shown(void) { W.t0 = nos_t; W.build = 0; W.out_on = 0; W.pressed = -1; }

static void wg_draw_mode(gfx_fb *fb, int mode, float b)
{
    float k = qz(m_clamp(b * 2.5f - 0.5f, 0, 1), 8);
    if (b <= 0) return;
    dot_style hs = { DOT_FONT_TALL, UI_HEROW_PITCH, UI_HEROW_PITCH * 0.8f, C_WHITE, 0, 1, 1 };
    if (mode == WG_TIMER) {
        ui_title(fb, "TIMER", (1 - k) * 60);
        time_ring(fb, UI_CX, UI_CY, WG_RING_R, 60, 6, W.lit * ease_o(b), C_WHITE, C_LINE2);
        /* same places as the app's run view, so opening it changes nothing that is shown */
        if (W.tm.state == 2) ui_state(fb, UI_CX, 150, "PAUSED", ui_dim(C_SEC, k));
        else ui_label(fb, UI_CX, 150, W.lbl, ui_dim(C_SEC, k), UI_CENTER);
        hs.color = W.tm.state == 2 ? C_SEC : C_WHITE;
        dgm_draw(fb, &W.dig[WG_TIMER], &hs, UI_CX, DIG_TOP, 0.28f, 0, 0, b);
    } else if (mode == WG_SW) {
        ui_title(fb, "STOPWATCH", (1 - k) * 60);
        time_ring(fb, UI_CX, UI_CY, WG_RING_R, 60, 4, 60 * m_clamp(b * 1.5f, 0, 1), C_LINE2, 0);
        /* the second hand sweeps once a minute like the app's; white, not red: the home
         * pager owns the one red (unread dot) */
        if (k >= 1) {
            float a = 6.2831853f * (float)(W.sw_ms % 60000) / 60000.0f;
            dot_disc(fb, UI_CX + WG_RING_R * gfx_fast_sin(a), UI_CY - WG_RING_R * gfx_fast_cos(a), 9, C_WHITE);
        }
        dot_style ts = { DOT_FONT_TALL, 5, 4, C_SEC, 0, 1, 1 };
        char tb[4];
        snprintf(tb, sizeof tb, ".%u", (unsigned)(W.sw_ms / 100 % 10));
        const dgm *dg = &W.dig[WG_SW];
        float w1 = dg_width(&hs, dg->cur), w2 = dg_width(&ts, tb), gap = 4;
        float x0 = floorf(UI_CX - (w1 + gap + w2) * 0.5f + 0.5f);
        dgm_draw(fb, dg, &hs, x0 + w1 * 0.5f, 197, 0.2f, 0, 0, b);
        float ty = 197 + UI_HEROW_PITCH * 9 - 45;
        if (b < 1) dot_text_anim(fb, &ts, x0 + w1 + gap, ty, tb, 0, b, DOT_REVEAL_RANDOM, 7);
        else dot_text(fb, &ts, x0 + w1 + gap, ty, tb);
    } else if (mode == WG_ALARM) {
        ui_title(fb, "NEXT ALARM", (1 - k) * 60);
        dot_icon_anim(fb, DOT_ICON_ALARM, UI_CX, 146, 3.0f, 2.4f, C_SEC, b, DOT_REVEAL_CENTER);
        dgm_draw(fb, &W.dig[WG_ALARM], &hs, UI_CX, 190, 0.28f, 0, 0, b);
        ui_label(fb, UI_CX, 304, W.al_lbl, ui_dim(C_PRI, k), UI_CENTER);
    } else {
        ui_title(fb, "TIMER", (1 - k) * 60);
        for (int i = 0; i < 3; i++) {
            float p = m_stagger(b, i, 3, 0.6f);
            if (p <= 0) continue;
            float cx = UI_CX + (i - 1) * WQ_DX;
            int fl = W.pressed == i || (W.flash == i && nos_t - W.flash_t < 0.3f);
            ui_circle(fb, cx, WQ_Y, WQ_R, ui_dim(fl ? C_SURF3 : C_SURF2, qz(m_clamp(p * 2, 0, 1), 8)));
            char nb[4];
            snprintf(nb, sizeof nb, "%d", k_wq[i]);
            dot_style st = { DOT_FONT_TALL, UI_LIST_PITCH, UI_LIST_PITCH * 0.8f, C_WHITE, 0, 1, 1 };
            dot_text_anim(fb, &st, cx, WQ_Y - 28, nb, 1, p, DOT_REVEAL_RANDOM, (unsigned)i);
            if (p > 0.6f) ui_label(fb, (int)cx, (int)WQ_Y + 32, "MIN", C_SEC, UI_CENTER);
        }
    }
}
static void wg_draw(gfx_fb *fb)
{
    if (W.out_on) wg_draw_mode(fb, W.prev, W.out);
    else wg_draw_mode(fb, W.mode, W.build);
}

static void wg_tap(int x, int y)
{
    if (W.mode == WG_QUICK && !W.out_on) {
        for (int i = 0; i < 3; i++)
            if (in_circle(x, y, UI_CX + (i - 1) * WQ_DX, WQ_Y, WQ_R + 6)) {
                /* the tapped circle flashes, the glance then turns into the running
                 * timer: that is the confirmation (no toast over the ring) */
                nos_feedback(FB_SUCCESS);
                W.flash = i; W.flash_t = nos_t;
                tm_start(k_wq[i] * 60000u);
                return;
            }
        nos_open(&scr_timer, x, y);
        return;
    }
    if (W.mode == WG_TIMER) nos_open(&scr_timer, x, y);
    else if (W.mode == WG_SW) nos_open(&scr_stopwatch, x, y);
    else if (W.mode == WG_ALARM) nos_open(&scr_alarm, x, y);
    else nos_open(&scr_timer, x, y);
}

static void wg_press(int x, int y, int down)
{
    W.pressed = -1;
    if (!down || W.mode != WG_QUICK || W.out_on) return;
    for (int i = 0; i < 3; i++)
        if (in_circle(x, y, UI_CX + (i - 1) * WQ_DX, WQ_Y, WQ_R + 6)) W.pressed = i;
}

const nos_widget wg_timer = { "TIMER", wg_tick, wg_draw, wg_tap, wg_shown, wg_press };
