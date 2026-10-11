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
void time_ring(gfx_fb *fb, float cx, float cy, float r, int n, float d, float lit, uint16_t on, uint16_t off);

static float qz(float v, float n) { return floorf(v * n + 0.5f) / n; }
#define PRESS_HOLD 0.1f          /* a pressed pill shows >= 0.1 s (3 frames) even on a quick tap */
/* the label of a pill that builds by scale: in from k 0.6, 3 steps of fill -> ink */
static float ink_k(float k) { return qz(m_clamp((k - 0.3f) / 0.4f, 0, 1), 3); }   /* from the 2nd scale step */

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
/* the standard button pair (UI_BTN_PAIR_*: 140 x 88 at y 372); the bezel track the
 * second hand runs on gives way where the pills cross it */
#define BTN_Y    ((float)UI_BTN_Y)   /* the pair standard: 140 x 88 at y 372 */
#define BTN_W    ((float)UI_BTN_PAIR_W)
#define BTN_LX   ((float)UI_BTN_PAIR_X0)
#define BTN_RX   ((float)UI_BTN_PAIR_X1)
#define LIST_Y0  194.0f
#define LIST_Y1  302.0f          /* three rows; the third dims into the bottom fade */
#define LAP_X0   (UI_ROW_X0 + 8)
#define LAP_X1   (UI_ROW_X1 - 8)
#define STEP_S   0.15f
#define ROW_H    36.0f
#define FADE_TOP 12.0f           /* rows fade within 12 px of the window top (the live row rests unfaded) */
#define HAND_S   0.3f            /* RESET: the hand sweeps home over 0.3 s */
#define DIG_Y_SOLO 186.0f
#define DIG_Y_LAPS 100.0f

enum { BP_NONE = 0, BP_LEFT, BP_RIGHT };
static struct {
    float t0, build;
    int pressed, press_live;     /* press_live: the left pill was enabled at the touch */
    float press_t0, unpress_at;
    svc_stopwatch_t sw;          /* snapshot for this frame */
    dgm dig, ten;                /* ten: the tenths cell, morphs on RESET only */
    char tenths[4];
    mspring lay;                 /* 0 = no laps (digits centred), 1 = laps shown */
    mspring ins;                 /* new-row slide-in offset, px */
    mscroll sc;
    int dragging;
    int best, worst;             /* lap numbers (1-based), 0 = none */
    char cur_lbl[16], cur_time[16];
    float dim;                   /* 0 running/idle .. 1 stopped: 150 ms colour step */
    struct {                     /* the rows fading out after RESET, as they were drawn */
        svc_stopwatch_t sw;
        char cur_lbl[16], cur_time[16];
        int best, worst;
    } gh;
    float hand_from, hand_t0;    /* RESET: hand position (turns) it sweeps home from */
    char lt[8], rt[8], lt0[8], rt0[8];   /* button labels, and the ones fading out */
    float lab_t0;
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
/* a whole number of rows, so the list can rest (snap) on row boundaries at both ends */
static float list_max(void)
{
    float content = (S.sw.nlaps + 1) * ROW_H;
    float m = content - (LIST_Y1 - LIST_Y0);
    return m > 0 ? ceilf(m / ROW_H) * ROW_H : 0;
}

static void sw_snapshot(int morph)
{
    uint32_t was = S.sw.elapsed_ms;
    S.sw = *time_sw();
    uint32_t ms = S.sw.elapsed_ms, s = ms / 1000;
    char b[16];
    if (s >= 3600) snprintf(b, sizeof b, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    else snprintf(b, sizeof b, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    dgm_set(&S.dig, b, morph);
    snprintf(S.tenths, sizeof S.tenths, ".%u", (unsigned)(ms / 100 % 10));
    if (morph && was && !ms) dgm_set(&S.ten, S.tenths, 1);          /* RESET: .8 -> .0 morphs */
    else if (!morph || strcmp(S.ten.cur, S.tenths)) dgm_set(&S.ten, S.tenths, 0);
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

static void sw_labels(const char **lt, const char **rt)
{
    int idle = !S.sw.running && !S.sw.elapsed_ms;
    *lt = S.sw.running || idle ? "LAP" : "RESET";
    *rt = S.sw.running ? "STOP" : idle ? "START" : "RESUME";
}

static float s_seen = -10;
static void sw_enter(void)
{
    sw_poll();
    S.pressed = BP_NONE; S.unpress_at = 0;
    if (nos_t - s_seen < 0.2f) return;          /* revealed by a pop: already on screen */
    S.t0 = nos_enter_t(); S.build = 0;
    sw_snapshot(0);
    const char *lt, *rt;
    sw_labels(&lt, &rt);
    snprintf(S.lt, sizeof S.lt, "%s", lt); snprintf(S.rt, sizeof S.rt, "%s", rt);
    memcpy(S.lt0, S.lt, sizeof S.lt); memcpy(S.rt0, S.rt, sizeof S.rt);
    S.lab_t0 = -10;
    S.dim = (!S.sw.running && S.sw.elapsed_ms) ? 1.0f : 0.0f;
    msp_init(&S.lay, S.sw.nlaps ? 100.0f : 0.0f, SPRING_PAGE);   /* percent */
    msp_init(&S.ins, 0, SPRING_SNAPPY);
    msc_init(&S.sc, 0, list_max(), ROW_H);      /* rests on row boundaries */
    S.sc.snap_free = 1;
    S.dragging = 0;
    S.hand_t0 = -10;
}

static void sw_tick(float dt)
{
    s_seen = nos_t;
    sw_poll();
    int prev_laps = S.sw.nlaps;
    svc_stopwatch_t before = S.sw;
    char bl[16], bt[16];
    int bb = S.best, bw = S.worst;
    memcpy(bl, S.cur_lbl, sizeof bl); memcpy(bt, S.cur_time, sizeof bt);
    sw_snapshot(1);
    if (before.elapsed_ms && !S.sw.elapsed_ms) {        /* RESET */
        S.hand_from = (float)(before.elapsed_ms % 60000) / 60000.0f;
        S.hand_t0 = nos_t;
        if (prev_laps) {                                /* every row fades out as it was */
            S.gh.sw = before;
            memcpy(S.gh.cur_lbl, bl, sizeof bl); memcpy(S.gh.cur_time, bt, sizeof bt);
            S.gh.best = bb; S.gh.worst = bw;
        }
    }
    if (S.sw.nlaps != prev_laps) {
        msp_to(&S.lay, S.sw.nlaps ? 100.0f : 0.0f);
        if (S.sw.nlaps > prev_laps && prev_laps > 0) { S.ins.x = -ROW_H; S.ins.v = 0; msp_to(&S.ins, 0); }
        msc_bounds(&S.sc, 0, list_max());
        if (S.sw.nlaps > prev_laps) msc_animate_to(&S.sc, 0);
    }
    msp_step(&S.lay, dt);
    msp_step(&S.ins, dt);
    msc_step(&S.sc, dt);
    if (S.unpress_at > 0 && nos_t >= S.unpress_at) { S.pressed = BP_NONE; S.unpress_at = 0; }
    S.build = m_clamp((nos_t - S.t0) / 0.4f, 0, 1);
    float tgt = (!S.sw.running && S.sw.elapsed_ms) ? 1.0f : 0.0f, st = dt / STEP_S;
    S.dim = S.dim < tgt ? fminf(tgt, S.dim + st) : fmaxf(tgt, S.dim - st);
    const char *lt, *rt;
    sw_labels(&lt, &rt);
    if (strcmp(lt, S.lt) || strcmp(rt, S.rt)) {
        memcpy(S.lt0, S.lt, sizeof S.lt); memcpy(S.rt0, S.rt, sizeof S.rt);
        snprintf(S.lt, sizeof S.lt, "%s", lt); snprintf(S.rt, sizeof S.rt, "%s", rt);
        S.lab_t0 = nos_t;
    }
    if (S.sw.running) nos_keep_awake();
}

static float dig_top(void) { return floorf(m_lerp(DIG_Y_SOLO, DIG_Y_LAPS, S.lay.x / 100.0f) + 0.5f); }

static void draw_list(gfx_fb *fb)
{
    gfx_fb sub;
    if (!nos_subview(fb, &sub, 0, (int)LIST_Y0, GFX_W, (int)LIST_Y1, 0, 0)) return;
    /* the rows come in once the rising digits are clear of them (first lap) */
    float k = qz(m_clamp(S.lay.x / 100.0f * 2.5f - 1.5f, 0, 1), 8);
    if (k <= 0) return;
    float off = floorf(S.sc.x + 0.5f) - floorf(S.ins.x + 0.5f);
    int ghost = !S.sw.nlaps;                         /* after RESET: the old rows, as drawn */
    const svc_stopwatch_t *sw = ghost ? &S.gh.sw : &S.sw;
    int n = sw->nlaps, best = ghost ? S.gh.best : S.best, worst = ghost ? S.gh.worst : S.worst;
    for (int r = 0; r <= n; r++) {
        float y = LIST_Y0 + r * ROW_H - off;          /* row top */
        if (y + ROW_H <= LIST_Y0 || y >= LIST_Y1) continue;
        float yc = y + ROW_H * 0.5f;
        /* fade rows that are leaving the window */
        float e = fminf((yc - LIST_Y0) / FADE_TOP, (LIST_Y1 - yc) / (ROW_H * 0.7f));
        float f = qz(m_clamp(e, 0, 1), 8) * k;
        if (f <= 0) continue;
        int x0 = LAP_X0, x1 = LAP_X1;        /* one x for every row: no drift along the circle */
        int base = (int)(yc + 6);
        char lb[16], tb[16];
        uint16_t lc = C_SEC, tc = C_SEC;
        if (r == 0) {
            snprintf(lb, sizeof lb, "%s", ghost ? S.gh.cur_lbl : S.cur_lbl);
            snprintf(tb, sizeof tb, "%s", ghost ? S.gh.cur_time : S.cur_time);
            lc = C_PRI; tc = C_PRI;
        } else {
            int i = n - r + 1;                       /* lap number, newest first */
            snprintf(lb, sizeof lb, "LAP %02d", i);
            fmt_lap(tb, sizeof tb, lap_ms(sw, i));
            if (i == best) { lc = C_SEC; tc = C_WHITE; }
            else if (i == worst) { lc = C_DIS; tc = C_DIS; }
            else { lc = C_DIS; tc = C_SEC; }
        }
        ui_label(&sub, x0, base, lb, ui_dim(lc, f), UI_LEFT);
        ui_label(&sub, x1, base, tb, ui_dim(tc, f), UI_RIGHT);
        if (r == 0 && n > 0)                         /* hairline under the live row */
            gfx_fill_rect(&sub, x0, (int)(y + ROW_H) - 1, x1 - x0, 2, ui_dim(C_LINE, f));
    }
}

/* distance from (x, y) to the nearest button pill's edge (negative inside) */
static float pill_d(float x, float y)
{
    float r = UI_BTN_H * 0.5f, best = 1e9f;
    for (int p = 0; p < 2; p++) {
        float c = p ? BTN_RX : BTN_LX, ax = c - BTN_W / 2 + r, bx = c + BTN_W / 2 - r;
        float qx = x < ax ? ax : x > bx ? bx : x, dx = x - qx, dy = y - BTN_Y;
        float d = gfx_fast_sqrt(dx * dx + dy * dy) - r;
        if (d < best) best = d;
    }
    return best;
}
/* the bezel track: time_ring's 60 dots, less the ones within 6 px of a button pill (the
 * standard pair at y 372 reaches r 218, over the track); the hand gives way the same way */
static uint8_t s_trk_skip[60];
static int s_trk_ok;
static void sw_track(gfx_fb *fb, float lit)
{
    if (!s_trk_ok) {                             /* once: which dots give way */
        for (int i = 0; i < 60; i++) {
            float a = 6.2831853f * (float)i / 60.0f;
            s_trk_skip[i] = pill_d(UI_CX + RING_R * gfx_fast_sin(a), UI_CY - RING_R * gfx_fast_cos(a)) < 6.0f;
        }
        s_trk_ok = 1;
    }
    lit = floorf(lit * 32.0f + 0.5f) / 32.0f;
    for (int i = 0; i < 60; i++) {
        if (s_trk_skip[i]) continue;
        float a = 6.2831853f * (float)i / 60.0f;
        float x = UI_CX + RING_R * gfx_fast_sin(a), y = UI_CY - RING_R * gfx_fast_cos(a);
        float f = m_clamp(lit - (float)i, 0, 1);
        if (f >= 1) dot_disc(fb, x, y, 4, C_LINE2);
        else if (i == (int)lit) dot_disc(fb, x, y, f > 0 ? 4 * gfx_fast_sqrt(f) : 0.05f, C_LINE2);
    }
}

static void sw_draw(gfx_fb *fb)
{
    float b = S.build;
    /* bezel: a dim 60-dot track and the red second hand, sweeping once a minute
     * (1.2 deg = 4.4 px per frame: smooth, never strobing); it stops when stopped */
    sw_track(fb, 60.0f * m_clamp(b * 1.5f, 0, 1));
    if (b > 0.3f) {
        float turn = (float)(S.sw.elapsed_ms % 60000) / 60000.0f;
        float hp = (nos_t - S.hand_t0) / HAND_S;
        if (hp < 1 && !S.sw.running && !S.sw.elapsed_ms) {
            /* RESET: the hand sweeps home the short way (back, or on past 12) */
            float from = S.hand_from > 0.5f ? S.hand_from - 1.0f : S.hand_from;
            turn = from * (1.0f - bezier_ease(&EASE_STANDARD, m_clamp(hp, 0, 1)));
        }
        float a = 6.2831853f * turn;
        float hx = UI_CX + RING_R * gfx_fast_sin(a), hy = UI_CY - RING_R * gfx_fast_cos(a);
        /* near a pill the hand shrinks away in 4 steps and grows back past it: never under */
        float g = qz(m_clamp((pill_d(hx, hy) - 2.0f) / 8.0f, 0, 1), 4);
        float d = 10.0f * qz(m_clamp((b - 0.3f) * 3, 0, 1), 4) * g;
        dot_disc(fb, hx, hy, d > 0.05f ? d : 0.05f, C_RED);      /* always recorded */
    }
    ui_title(fb, "STOPWATCH", 0);

    /* digits: mm:ss (TALL, pitch 8) + .t (row pitch 4.4), tabular, centred as one group,
     * bottoms aligned */
    float pitch = UI_HEROW_PITCH;
    float dk = qz(S.dim, 6);
    dot_style st = { DOT_FONT_TALL, pitch, pitch * 0.8f, ui_mix(C_WHITE, C_SEC, dk), 0, 1, 1 };
    dot_style ts = { DOT_FONT_TALL, UI_LIST_PITCH, UI_LIST_PITCH * 0.8f, C_SEC, 0, 1, 1 };
    float w1 = dg_width(&st, S.dig.cur), w2 = dg_width(&ts, S.tenths), gap = 4;
    float x0 = floorf(UI_CX - (w1 + gap + w2) * 0.5f + 0.5f);
    float top = dig_top();
    /* stopped (no laps shown): [ PAUSED ] above the digits, like the timer */
    float pk = dk * (1 - m_clamp(S.lay.x / 50.0f, 0, 1));
    if (pk > 0.01f) ui_state(fb, UI_CX, 150, "PAUSED", ui_dim(C_SEC, qz(pk, 6)));
    dgm_draw(fb, &S.dig, &st, x0, top, 0.2f, b);
    float ty = floorf(top + (pitch - UI_LIST_PITCH) * 9 + 0.5f);
    dgm_draw(fb, &S.ten, &ts, x0 + w1 + gap, ty, 0.2f, b);

    draw_list(fb);

    /* buttons: full colour from the start (scale steps, no grey fade-up), labels in
     * from 60 % of the build; labels crossfade through the fill when the state changes.
     * Pressed: C_SEC with black ink. */
    float k = qz(m_clamp(b * 2.5f - 1.0f, 0, 1), 8);
    if (k <= 0) return;
    int idle = !S.sw.running && !S.sw.elapsed_ms;
    int pl = S.pressed == BP_LEFT && S.press_live, pr = S.pressed == BP_RIGHT;   /* RESET stays pressed into idle */
    uint16_t lf = pl ? C_SEC : idle ? C_SURF : C_SURF2, li = pl ? C_BG : idle ? C_DIS : C_PRI;
    uint16_t rf = pr ? C_SEC : C_WHITE;
    float sc = 0.72f + 0.28f * k, w = BTN_W * sc, h = UI_BTN_H * sc;
    ui_rrect(fb, BTN_LX - w / 2, BTN_Y - h / 2, w, h, h / 2, lf);
    ui_rrect(fb, BTN_RX - w / 2, BTN_Y - h / 2, w, h, h / 2, rf);
    float ik = ink_k(k);
    if (ik <= 0) return;
    float lp = m_clamp((nos_t - S.lab_t0) / STEP_S, 0, 1);
    float la = qz(lp < 0.5f ? 1 - lp * 2 : lp * 2 - 1, 4);
    int lch = lp < 1 && strcmp(S.lt0, S.lt) != 0, rch = lp < 1 && strcmp(S.rt0, S.rt) != 0;
    const char *lt = lch && lp < 0.5f ? S.lt0 : S.lt, *rt = rch && lp < 0.5f ? S.rt0 : S.rt;
    ui_label(fb, (int)BTN_LX, (int)BTN_Y + 6, lt, ui_mix(lf, li, (lch ? la : 1) * ik), UI_CENTER);
    ui_label(fb, (int)BTN_RX, (int)BTN_Y + 6, rt, ui_mix(rf, C_BG, (rch ? la : 1) * ik), UI_CENTER);
}

static int sw_hit(int x, int y)
{
    if (fabsf(y - BTN_Y) > 50) return BP_NONE;
    if (fabsf(x - BTN_LX) <= BTN_W / 2 + 4) return BP_LEFT;
    if (fabsf(x - BTN_RX) <= BTN_W / 2 + 4) return BP_RIGHT;
    return BP_NONE;
}

static int sw_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN:
        S.pressed = sw_hit(in->x, in->y); S.press_t0 = nos_t; S.unpress_at = 0;
        S.press_live = S.sw.running || S.sw.elapsed_ms;
        msc_grab(&S.sc);
        return 1;
    case IN_DRAG_START:
        S.pressed = BP_NONE; S.unpress_at = 0;
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
        if (S.pressed != BP_NONE) S.unpress_at = fmaxf(nos_t, S.press_t0 + PRESS_HOLD);
        return 1;
    case IN_TAP: {
        int h = sw_hit(in->x, in->y);
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
