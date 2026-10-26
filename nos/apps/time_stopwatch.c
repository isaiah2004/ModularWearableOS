/* time_stopwatch.c — Stopwatch. Team: time.
 *
 * Big tabular dot digits mm:ss with smaller tenths, a red second hand (a slanted dash)
 * running around a bezel track of 60 slanted dashes once a minute, the seconds gone
 * lit behind it (the hand is the only thing that moves every frame: its damage box is a
 * few dozen pixels), Start/Stop + Lap/Reset pills and a lap list (live current lap, best
 * lap white, worst grey).
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

/* from time_timer.c: one slanted dash (prog kit PROG_SLANT geometry) at a free angle */
void time_slant_xy(float r, float th, float slot, float a, float sc, float *xy);
void time_slant(gfx_fb *fb, float r, float th, float slot, float a, float sc, uint16_t c);

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

/* ============================================ the seconds track (also the widget's) */
/* 60 slanted dashes mark the whole seconds within the minute: even minutes light the
 * ones gone, [0, s); odd minutes clear them, [s, 60), so the minute rolls over without
 * a 59-dash cut (the lit run just changes which end the hand drags). A dash's slot spans
 * its second, so it changes as the hand leaves it: one dash a second repaints. */
void time_sw_lit(uint32_t ms, int *l0, int *l1)
{
    int s = (int)(ms / 1000 % 60);
    if ((ms / 60000) & 1) { *l0 = s; *l1 = 60; }
    else { *l0 = 0; *l1 = s; }
}
/* Dashes [l0, l1) in `on`, the rest `off`; only the first `shown` drawn (the entry sweep;
 * the leading one steps its ink in, 8 steps); skip: dashes that give way (0 = none);
 * tc != 0: dashes whose centre lies in the signed-turn span [t0, t1] take tc instead. */
void time_sw_marks(gfx_fb *fb, float r, float th, float shown, int l0, int l1, uint16_t on, uint16_t off,
                   const uint8_t *skip, float t0, float t1, uint16_t tc)
{
    const float slot = 6.2831853f * r / 60.0f;
    shown = floorf(shown * 8.0f + 0.5f) / 8.0f;
    for (int i = 0; i < 60 && (float)i < shown; i++) {
        if (skip && skip[i]) continue;
        float ti = ((float)i + 0.5f) / 60.0f;
        uint16_t c = i >= l0 && i < l1 ? on : off;
        if (tc && (ti > 0.5f ? ti - 1.0f : ti) >= t0 && (ti > 0.5f ? ti - 1.0f : ti) <= t1) c = tc;
        float f = shown - (float)i;
        time_slant(fb, r, th, slot, 6.2831853f * ti, 1.0f, f >= 1 ? c : ui_dim(c, f));
    }
}

/* ============================================ per-character digits with morphs */
#define DG_MAX 10
typedef struct { char cur[DG_MAX + 1], from[DG_MAX + 1]; nos_time_t t0[DG_MAX]; } dgm;
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
#define TRK_TH   16.0f           /* the track: 60 slanted dashes, r 203-219 (the timer's ring) */
#define TRK_SLOT (6.2831853f * RING_R / 60.0f)
#define HAND_TH  (TRK_TH + 6.0f) /* the hand: a dash 6 px longer */
#define SWEEP_F  34              /* the track sweeps in over 1.13 s (EASE_STEADY, whole frames):
                                  * 1326 px, <= 63 px a frame */
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
#define DIM_S    M_FRAMES(4)     /* running <-> stopped colour step */
#define LAB_S    M_FRAMES(6)     /* a pill label swap: out through the fill (3), in (3) */
#define ROW_H    36.0f
#define FADE_TOP 12.0f           /* rows fade within 12 px of the window top (the live row rests unfaded) */
#define DIG_Y_SOLO 186.0f
#define DIG_Y_LAPS 100.0f
/* Motion (docs/MOTION30.html):
 *  - RESET: the hand flies back to 12 as a mechanical cut (m_cut: nudge, lead-in, one jump
 *    to 4 px past, 2-1-0), the flyback of a real stopwatch; on the jump frame the bezel
 *    track dashes it passed light in dim red (45 %, then 20 % the next frame), the R7
 *    trail, so the one big step reads as a sweep, not a teleport; the lit seconds ghost
 *    out with it (45 %, 20 %, off).
 *  - The hero digits move between their solo and lap places (86 px) with the same cut: a
 *    dot-matrix numeral never slides at mid speed (R2/R4). The lap rows fade in once the
 *    digits have jumped clear (frames 3-7), and fade out before they jump back (1-3).
 *  - A new lap pushes the rows down one row on the 4-frame table (16-12-6-2 px). */

enum { BP_NONE = 0, BP_LEFT, BP_RIGHT };
static struct {
    nos_time_t t0; float build;
    int pressed, press_live;     /* press_live: the left pill was enabled at the touch */
    nos_time_t press_t0, unpress_at;
    svc_stopwatch_t sw;          /* snapshot for this frame */
    dgm dig, ten;                /* ten: the tenths cell, morphs on RESET only */
    char tenths[4];
    nos_time_t lay_t0; float lay_from, lay_to;   /* digits' top: cut between DIG_Y_SOLO and DIG_Y_LAPS */
    float dig_y;                 /* the digits' top this frame (whole px) */
    float rows_k;                /* lap rows' ink */
    mtween ins;                  /* new-row slide-in offset, px */
    float ins_x;
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
    nos_time_t hand_t0; float hand_from;    /* RESET: hand position (turns) it flies home from */
    float hand_turn, hand_k;     /* this frame (signed turns from 12) and its build scale */
    float hand_sc;               /* drawn scale: the build times the pill give-way */
    float trail_a, trail_b, trail_k;   /* RESET jump: track span lit (turns) and its ink */
    int rs_l0, rs_l1, rs_k;      /* RESET: the lit dashes it left, and the cut's frame (0 = none) */
    char lt[8], rt[8], lt0[8], rt0[8];   /* button labels, and the ones fading out */
    nos_time_t lab_t0;
    float track_shown, btn_k;    /* entry build, prepared in tick: dashes swept in, pills */
    float fb_prev;               /* fling blur: last frame's drawn list offset */
    int fb_lv;                   /* fling blur level this frame (0 = sharp) */
} S;
/* pill label swap: the old label sinks into the fill over 3 frames, the new one rises over
 * the next 3, in perceptual ink steps */
static float lab_k(void)
{
    float lp = m_clamp((nos_t - S.lab_t0) / LAB_S, 0, 1);
    return m_ink(lp < 0.5f ? 1 - lp * 2 : lp * 2 - 1);
}

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

static float dig_top(void);
static float slant_pill_d(float th, float a, float sc);
static nos_time_t s_seen = -10;
static void sw_enter(void)
{
    sw_poll();
    S.pressed = BP_NONE; S.unpress_at = 0;
    S.fb_lv = 0;
    if (nos_t - s_seen < 0.2f) return;          /* revealed by a pop: already on screen */
    S.t0 = nos_enter_t(); S.build = 0;
    sw_snapshot(0);
    const char *lt, *rt;
    sw_labels(&lt, &rt);
    snprintf(S.lt, sizeof S.lt, "%s", lt); snprintf(S.rt, sizeof S.rt, "%s", rt);
    memcpy(S.lt0, S.lt, sizeof S.lt); memcpy(S.rt0, S.rt, sizeof S.rt);
    S.lab_t0 = -10;
    S.dim = (!S.sw.running && S.sw.elapsed_ms) ? 1.0f : 0.0f;
    S.lay_from = S.lay_to = S.dig_y = S.sw.nlaps ? DIG_Y_LAPS : DIG_Y_SOLO;
    S.lay_t0 = -10; S.rows_k = S.sw.nlaps ? 1.0f : 0.0f;
    mtw_start_tab(&S.ins, 0, 0, &MF_MICRO4); S.ins.t0 = -10; S.ins_x = 0;
    msc_init(&S.sc, 0, list_max(), ROW_H);      /* rests on row boundaries */
    S.sc.snap_free = 1;
    S.dragging = 0;
    S.hand_t0 = -10;
    S.fb_prev = floorf(S.sc.x + 0.5f) - S.ins_x;
}
static void fblur_tick(float dt);

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
        time_sw_lit(before.elapsed_ms, &S.rs_l0, &S.rs_l1);
        if (prev_laps) {                                /* every row fades out as it was */
            S.gh.sw = before;
            memcpy(S.gh.cur_lbl, bl, sizeof bl); memcpy(S.gh.cur_time, bt, sizeof bt);
            S.gh.best = bb; S.gh.worst = bw;
        }
    }
    if (S.sw.nlaps != prev_laps) {
        float to = S.sw.nlaps ? DIG_Y_LAPS : DIG_Y_SOLO;
        if (to != S.lay_to) {
            /* started from input this frame: it shows its first step now (k = 1) */
            S.lay_from = S.dig_y; S.lay_to = to; S.lay_t0 = nos_t - M_FRAMES(1);
        }
        if (S.sw.nlaps > prev_laps && prev_laps > 0) { mtw_start_tab(&S.ins, -ROW_H, 0, &MF_MICRO4); mtw_lead(&S.ins); }
        msc_bounds(&S.sc, 0, list_max());
        if (S.sw.nlaps > prev_laps) msc_animate_to(&S.sc, 0);
    }
    {
        int k = m_frame_k(S.lay_t0);
        S.dig_y = floorf(m_cut(S.lay_from, S.lay_to, k) + 0.5f);
        /* rows: in after the jump (frames 3-7), out before it (frames 1-3) */
        if (S.lay_to == DIG_Y_LAPS) S.rows_k = m_ink((float)(k - 2) / 5.0f);
        else S.rows_k = m_ink(1.0f - (float)k / 3.0f);
    }
    S.ins_x = floorf(mtw_value(&S.ins) + 0.5f);
    msc_step(&S.sc, dt);
    if (S.unpress_at > 0 && nos_t >= S.unpress_at) { S.pressed = BP_NONE; S.unpress_at = 0; }
    S.build = m_clamp((nos_t - S.t0) / 0.4f, 0, 1);
    fblur_tick(dt);
    float tgt = (!S.sw.running && S.sw.elapsed_ms) ? 1.0f : 0.0f, st = dt / DIM_S;
    S.dim = S.dim < tgt ? fminf(tgt, S.dim + st) : fmaxf(tgt, S.dim - st);
    const char *lt, *rt;
    sw_labels(&lt, &rt);
    if (strcmp(lt, S.lt) || strcmp(rt, S.rt)) {
        memcpy(S.lt0, S.lt, sizeof S.lt); memcpy(S.rt0, S.rt, sizeof S.rt);
        snprintf(S.lt, sizeof S.lt, "%s", lt); snprintf(S.rt, sizeof S.rt, "%s", rt);
        S.lab_t0 = nos_t;
    }
    if (S.sw.running) nos_keep_awake();
    /* the hand: sweeping with the time (0.74 px a frame, AA at its float position), or
     * the RESET flyback cut (k = 1 on the RESET frame itself) */
    {
        const float circ = 6.2831853f * RING_R;
        int k = m_frame_k(S.hand_t0) + 1;
        int resetting = k < M_CUT_FRAMES && !S.sw.running && !S.sw.elapsed_ms;
        S.trail_k = 0;
        S.rs_k = resetting ? k : 0;
        if (resetting) {
            float from = (S.hand_from > 0.5f ? S.hand_from - 1.0f : S.hand_from) * circ;
            S.hand_turn = m_cut(from, 0, k) / circ;
            if (k == 3 || k == 4) {      /* the jump frame and the one after: the trail */
                S.trail_a = m_cut(from, 0, 2) / circ; S.trail_b = m_cut(from, 0, 3) / circ;
                S.trail_k = k == 3 ? UI_GHOST1 : UI_GHOST2;
            }
        } else S.hand_turn = (float)(S.sw.elapsed_ms % 60000) / 60000.0f;
        nos_motion_probe("sw.hand", resetting ? MC_COMPONENT : MC_TRACKED,
                         resetting ? S.hand_turn * circ : (float)S.sw.elapsed_ms / 60000.0f * circ);
    }
    /* entry build, whole frames: the track sweeps in from 12 (EASE_STEADY over SWEEP_F
     * frames, 1 s: a sweep, not a blink), the lit seconds with it; the hand grows in
     * (4 steps) as the sweep passes it; the pills scale in on the 6-frame table (5-10) */
    {
        int bk = m_frame_k(S.t0);
        float u = bk <= 0 ? 0.0f : bk >= SWEEP_F ? 1.0f : bezier_ease(&EASE_STEADY, (float)bk / SWEEP_F);
        S.track_shown = floorf(60.0f * u * 8.0f + 0.5f) / 8.0f;
        float hp = 60.0f * (S.hand_turn < 0 ? S.hand_turn + 1.0f : S.hand_turn);
        S.hand_k = S.track_shown >= 60.0f ? 1.0f : qz(m_clamp((S.track_shown - hp) / 2.0f, 0, 1), 4);
        /* near a pill the hand shrinks away in 4 steps and grows back past it: never under */
        float g = qz(m_clamp((slant_pill_d(HAND_TH, 6.2831853f * S.hand_turn, 1.0f) - 2.0f) / 8.0f, 0, 1), 4);
        S.hand_sc = S.hand_k * g;
        S.btn_k = mf_at(&MF_MICRO6, bk - 4);
    }
    /* motion audit (sim only) */
    nos_motion_probe("sw.track.arc", MC_FILL, S.track_shown * TRK_SLOT);
    nos_motion_probe("sw.digits.y", MC_COMPONENT, S.dig_y);
    nos_motion_probe("sw.list", S.dragging ? MC_FINGER : MC_MOMENTUM, floorf(S.sc.x + 0.5f));
    nos_motion_probe("sw.list.ins", MC_MICRO, S.ins_x);
    if (S.btn_k > 0) nos_motion_probe("sw.btn.w", MC_MICRO, floorf(BTN_W * (0.72f + 0.28f * S.btn_k) + 0.5f));
    nos_motion_probe("sw.dim", MC_FADE, qz(S.dim, 4) * 100.0f);
    nos_motion_probe("sw.label.ink", MC_FADE, lab_k() * 100.0f);
    nos_motion_probe("sw.rows.ink", MC_FADE, S.rows_k * 100.0f);
}

static float dig_top(void) { return S.dig_y; }

/* one lap row r (0 = the live lap) with its top at y, in ink f */
static void sw_row(gfx_fb *fb, int r, float y, float f)
{
    int ghost = !S.sw.nlaps;                         /* after RESET: the old rows, as drawn */
    const svc_stopwatch_t *sw = ghost ? &S.gh.sw : &S.sw;
    int n = sw->nlaps, best = ghost ? S.gh.best : S.best, worst = ghost ? S.gh.worst : S.worst;
    float yc = y + ROW_H * 0.5f;
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
    ui_label(fb, x0, base, lb, ui_dim(lc, f), UI_LEFT);
    ui_label(fb, x1, base, tb, ui_dim(tc, f), UI_RIGHT);
    if (r == 0 && n > 0)                         /* hairline under the live row */
        gfx_fill_rect(fb, x0, (int)(y + ROW_H) - 1, x1 - x0, 2, ui_dim(C_LINE, f));
}

/* ---- fling blur (ui.h). The completed laps (rows 1..n) are cached in 72 px bands of
 * two rows, baked at full ink with 24 px of context: 12 bands either side of the resting
 * list cover all 30 laps, where 12 single rows would run out mid-fling. Each band is
 * blitted once per row, clipped to that row's strip at the row's own window fade. The
 * live lap (row 0) is never cached: its time changes every 3 frames, so it is always drawn
 * live (sharp, like a header), never stale, and the resting cache stays valid while the
 * stopwatch runs (no re-bake every tenth). Only a fast free glide of the real laps blurs:
 * not held, not the RESET ghost rows, not while rows slide in or fade. */
#define SW_BAND (2 * (int)ROW_H)
static float band_top(int b, float off) { return LIST_Y0 + (1 + 2 * b) * ROW_H - off; }
static void sw_blur_band(gfx_fb *fb, int i, int x0, int y, int x1, int h, void *ctx)
{
    (void)x0; (void)x1; (void)ctx;
    for (int r = 1; r <= S.sw.nlaps; r++) {
        float ry = (float)y + (r - 1 - 2 * i) * ROW_H;
        if (ry + ROW_H <= y - 24 || ry >= y + h + 24) continue;
        sw_row(fb, r, ry, 1.0f);
    }
}
/* the bands of the lap rows inside the window at drawn offset off; 0 if none */
static int sw_vis(float off, int *b0, int *b1)
{
    int i0 = (int)floorf(off / ROW_H), i1 = (int)ceilf((off + (LIST_Y1 - LIST_Y0)) / ROW_H) - 1;
    if (i0 < 1) i0 = 1;
    if (i1 > S.sw.nlaps) i1 = S.sw.nlaps;
    *b0 = (i0 - 1) / 2; *b1 = (i1 - 1) / 2;
    return i1 >= i0;
}
static void fblur_tick(float dt)
{
    float off = floorf(S.sc.x + 0.5f) - S.ins_x, fr = dt * M_FPS;
    float st = (off - S.fb_prev) / (fr > 0.5f ? fr : 1.0f);
    S.fb_prev = off;
    S.fb_lv = 0;
    if (!S.sw.nlaps) return;
    int animating = S.rows_k < 1.0f || S.ins_x != 0 || S.build < 1.0f;
    int held = S.dragging || animating;
    int b0, b1;
    if (!sw_vis(off, &b0, &b1)) b0 = b1 = 0;
    ui_fblur_tick(&S, held, st, b0, b1, (S.sw.nlaps + 1) / 2, SW_BAND, LAP_X0 - 2, LAP_X1 + 2, 1, sw_blur_band, 0);
    S.fb_lv = !animating && ui_fblur_active(&S);
}

static void draw_list(gfx_fb *fb)
{
    gfx_fb sub;
    if (!nos_subview(fb, &sub, 0, (int)LIST_Y0, GFX_W, (int)LIST_Y1, 0, 0)) return;
    /* the rows come in once the digits have jumped clear of them (first lap) */
    float k = S.rows_k;
    if (k <= 0) return;
    float off = floorf(S.sc.x + 0.5f) - S.ins_x;
    int n = S.sw.nlaps ? S.sw.nlaps : S.gh.sw.nlaps;
    /* a fast free glide blits the pre-blurred bands for every visible lap row, or none (a
     * lone sharp lap among smeared ones reads as a glitch) */
    int b0, b1;
    int blur = S.fb_lv && sw_vis(off, &b0, &b1);
    /* blurring, the live row goes last: its hairline reaches 1 px into lap 1's strip */
    /* Restrict traversal without changing the live-row-last compositing order. */
    int r0 = (int)floorf(off / ROW_H), r1 = (int)ceilf((off + LIST_Y1 - LIST_Y0) / ROW_H);
    if (r0 < 0) r0 = 0;
    if (r1 > n) r1 = n;
    int first = blur && r0 == 0 ? 1 : r0;
    int live_last = blur && r0 == 0 && r1 >= 0;
    int count = r1 >= first ? r1 - first + 1 : 0;
    for (int j = 0; j < count + live_last; j++) {
        int r = j < count ? first + j : 0;
        float y = LIST_Y0 + r * ROW_H - off;          /* row top */
        if (y + ROW_H <= LIST_Y0 || y >= LIST_Y1) continue;
        float yc = y + ROW_H * 0.5f;
        /* fade rows that are leaving the window */
        float e = fminf((yc - LIST_Y0) / FADE_TOP, (LIST_Y1 - yc) / (ROW_H * 0.7f));
        float f = qz(m_clamp(e, 0, 1), 8) * k;
        if (f <= 0) continue;
        if (blur && r > 0) {                         /* lap r's strip of its band, at its fade */
            gfx_fb rs;
            int b = (r - 1) / 2;
            if (nos_subview(&sub, &rs, 0, (int)y, GFX_W, (int)(y + ROW_H), 0, 0) &&
                ui_fblur_draw(&rs, &S, b, (int)band_top(b, off), f)) continue;
        }
        sw_row(&sub, r, y, f);
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
/* the nearest a slanted dash at angle a, scale sc, comes to a button pill */
static float slant_pill_d(float th, float a, float sc)
{
    float xy[8], d = 1e9f;
    time_slant_xy(RING_R, th, TRK_SLOT, a, sc, xy);
    for (int i = 0; i < 4; i++) {
        float e = pill_d(xy[2 * i], xy[2 * i + 1]);
        if (e < d) d = e;
    }
    return d;
}
/* the bezel track: 60 slanted dashes, less the ones within 6 px of a button pill (the
 * standard pair at y 372 reaches r 218, over the track); the hand gives way the same way */
static uint8_t s_trk_skip[60];
static int s_trk_ok;
static void sw_track(gfx_fb *fb, float shown, uint16_t on)
{
    if (!s_trk_ok) {                             /* once: which dashes give way */
        for (int i = 0; i < 60; i++)
            s_trk_skip[i] = slant_pill_d(TRK_TH, 6.2831853f * ((float)i + 0.5f) / 60.0f, 1.0f) < 6.0f;
        s_trk_ok = 1;
    }
    int l0, l1;
    if (S.rs_k > 0) {
        /* RESET: the lit seconds stay through the lead-in, then ghost out with the jump */
        l0 = S.rs_l0; l1 = S.rs_l1;
        if (S.rs_k == 3) on = ui_mix(C_LINE2, on, UI_GHOST1);
        else if (S.rs_k == 4) on = ui_mix(C_LINE2, on, UI_GHOST2);
        else if (S.rs_k > 4) l0 = l1 = 0;
    } else time_sw_lit(S.sw.elapsed_ms, &l0, &l1);
    /* RESET's jump frame (and the one after): the dashes the hand flew over light in dim
     * red, 45 % then 20 % (the hand's own smear, R7: a ring trail) */
    float t0 = fminf(S.trail_a, S.trail_b), t1 = fmaxf(S.trail_a, S.trail_b);
    uint16_t tc = S.trail_k > 0 ? ui_dim(C_RED, S.trail_k) : 0;
    time_sw_marks(fb, RING_R, TRK_TH, shown, l0, l1, on, C_LINE2, s_trk_skip, t0, t1, tc);
}

static void sw_draw(gfx_fb *fb)
{
    float b = S.build;
    float dk = qz(S.dim, 4);
    /* bezel: 60 slanted dashes, the seconds gone lit (grey, dimmer when stopped), and the
     * red second hand, sweeping once a minute (0.2 deg = 0.74 px a frame at its float
     * position: smooth, never strobing); it stops when stopped, and flies home on RESET
     * (the short way: back, or on past 12) */
    sw_track(fb, S.track_shown, ui_mix(C_SEC, C_DIS, dk));
    if (S.hand_k > 0)              /* hand_sc: its build and pill give-way scale, from tick */
        time_slant(fb, RING_R, HAND_TH, TRK_SLOT, 6.2831853f * S.hand_turn,     /* always recorded: */
                   S.hand_sc > 0.02f ? S.hand_sc : 0.02f, S.hand_sc > 0.02f ? C_RED : C_BG);   /* gone = a black speck */
    ui_title(fb, "STOPWATCH", 0);

    /* digits: mm:ss (TALL, pitch 8) + .t (row pitch 4.4), tabular, centred as one group,
     * bottoms aligned */
    float pitch = UI_HEROW_PITCH;
    dot_style st = { DOT_FONT_TALL, pitch, pitch * 0.8f, ui_mix(C_WHITE, C_SEC, dk), 0, 1, 1 };
    dot_style ts = { DOT_FONT_TALL, UI_LIST_PITCH, UI_LIST_PITCH * 0.8f, C_SEC, 0, 1, 1 };
    float w1 = dg_width(&st, S.dig.cur), w2 = dg_width(&ts, S.tenths), gap = 4;
    float x0 = floorf(UI_CX - (w1 + gap + w2) * 0.5f + 0.5f);
    float top = dig_top();
    /* stopped (no laps shown): [ PAUSED ] above the digits, like the timer */
    float pk = S.sw.elapsed_ms ? dk * (1 - m_clamp((DIG_Y_SOLO - top) / (DIG_Y_SOLO - DIG_Y_LAPS) * 2.0f, 0, 1)) : 0.0f;
    if (pk > 0.01f) ui_state(fb, UI_CX, 150, "PAUSED", ui_dim(C_SEC, qz(pk, 4)));
    dgm_draw(fb, &S.dig, &st, x0, top, M_FRAMES(6), b);
    float ty = floorf(top + (pitch - UI_LIST_PITCH) * 9 + 0.5f);
    dgm_draw(fb, &S.ten, &ts, x0 + w1 + gap, ty, M_FRAMES(6), b);

    draw_list(fb);

    /* buttons: full colour from the start (scale steps, no grey fade-up), labels in
     * from 60 % of the build; labels crossfade through the fill when the state changes.
     * Pressed: C_SEC with black ink. */
    float k = S.btn_k;
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
    float lp = m_clamp((nos_t - S.lab_t0) / LAB_S, 0, 1);
    float la = lab_k();
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

/* A flick glides to a row boundary (the list rests on rows). msc's pager projection (0.5 s
 * of decay) puts the row so far ahead that its snap spring speeds up past the release
 * speed (MOTION30 momentum budget); instead the spring is critically damped and its row is
 * at most 1.9 v / w0 ahead: from speed v at distance D it only slows while w0 D <= 2 v,
 * an ease-out from the finger's speed. A rubber-band return is left as msc made it. */
static void sw_fling_snap(float v)
{
    mscroll *s = &S.sc;
    if (!s->springing || s->x < s->min || s->x > s->max) return;
    float reach = 1.9f * v * s->sp.response / 6.2831853f;
    float tgt = s->min + ROW_H * floorf((s->x + reach - s->min) / ROW_H + 0.5f);
    if ((tgt - s->x) * v > 0 && fabsf(tgt - s->x) > fabsf(reach)) {
        float back = tgt - (v > 0 ? ROW_H : -ROW_H);   /* one row short of the reach */
        if ((back - s->x) * v >= 0) tgt = back;
    }
    s->sp.damping = 1.0f;
    msp_to(&s->sp, m_clamp(tgt, s->min, s->max));
}

static int sw_input(const nos_input *in)
{
    if (in->type == IN_CANCEL) { S.pressed = BP_NONE; S.press_live = S.dragging = 0; S.unpress_at = 0; msc_jump(&S.sc, S.sc.x); return 1; }

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
        sw_fling_snap(-in->vy);
        return 1;
    case IN_UP:
        /* still grabbed (no drag): let go. After a drag IN_DRAG_END already released it
         * with the fling's velocity, which this must not zero */
        if (!S.dragging && S.sc.dragging) msc_release(&S.sc, 0);
        if (S.pressed != BP_NONE) S.unpress_at = fmax(nos_t, S.press_t0 + PRESS_HOLD);
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
