/* fit_breathe.c — guided breathing (health team).
 *
 * Three states on one screen:
 *   START    a white START disc inside the empty session dial, duration pills
 *   SESSION  the disc breathes: inhale 4 s, hold 2 s, exhale 6 s (5 breaths/min)
 *   SUMMARY  minutes, breaths and (if the HR module locked) the average heart rate
 *
 * The breath is the START disc itself, a solid disc that opens from r 44 to r 74 and
 * lightens from grey to white as it does (restrained: one shape, no texture). Around it
 * the dial: twelve radial capsules (6 x 18, radius 105, 12 o'clock first) that light
 * clockwise by elapsed fraction, the leading one growing outward in 8 steps. Holds are
 * truly still (zero repaint). Everything is prepared in tick(); draw() only reads it:
 * one AA circle and twelve capsules (gfx_line), each its own damage command, so a
 * frame repaints the disc and, now and then, one capsule. */
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

/* ---- timing ---- */
#define T_IN     4.0f
#define T_HOLD   2.0f
#define T_OUT    6.0f
#define T_CYCLE  (T_IN + T_HOLD + T_OUT)
#define T_INTRO  1.00f          /* start screen -> first inhale */
#define T_OUTRO  M_FRAMES(11)   /* last exhale -> summary: the disc closes (1-8), the dial fades (5-10) */
#define PRESS_MIN 0.10f         /* a pressed control shows >= 3 frames, even on a quick tap */
#define T_SUMOUT M_FRAMES(6)    /* AGAIN: the summary builds out ... */
#define T_OVL    M_FRAMES(3)    /* ... while the field already grows back for its last 3 frames */
/* Fades and builds, in whole frames (MOTION30 R9; ink levels by m_ink, perceptual): */
#define TXT_IN   M_FRAMES(8)    /* phase word fades in after its phase starts ... */
#define TXT_OUT  M_FRAMES(6)    /* ... and out before it ends */

/* ---- geometry ---- */
#define START_CY 204.0f
#define DISC_R   44.0f          /* the START disc = the breath fully out */
#define CORE_MAX 74.0f          /* the breath fully in: 22 px clear of the dial */
#define PILL_Y   370
#define DIAL_N   12             /* session progress: twelve radial capsules ... */
#define DIAL_R   105.0f         /* ... centred on radius 105 ... */
#define DIAL_W   6.0f           /* ... 6 px wide ... */
#define DIAL_L   12.0f          /* ... 18 long (12 between the cap centres) */

/* the capsules' directions, 12 o'clock first, clockwise, 30 degrees apart (no trig) */
static const float k_dir[DIAL_N][2] = {
    { 0.0f, -1.0f }, { 0.5f, -0.8660254f }, { 0.8660254f, -0.5f }, { 1.0f, 0.0f },
    { 0.8660254f, 0.5f }, { 0.5f, 0.8660254f }, { 0.0f, 1.0f }, { -0.5f, 0.8660254f },
    { -0.8660254f, 0.5f }, { -1.0f, 0.0f }, { -0.8660254f, -0.5f }, { -0.5f, -0.8660254f } };

/* ---- state ---- */
enum { ST_START = 0, ST_INTRO, ST_SESSION, ST_OUTRO, ST_SUMMARY, ST_SUMOUT };
static struct {
    int   st;
    float t;            /* time in the current state */
    float build;        /* entry build progress 0..1 */
    float et;           /* since enter */
    float sel[3];       /* duration pills: selected-ness, crossfaded */
    int   pressed;      /* control under the finger, -1 none */
    nos_time_t press_t0; nos_time_t unpress_at;   /* pressed shows >= PRESS_MIN even on a quick tap */
    int   press_hold;   /* the tapped control stays pressed while the screen departs */
    int   cycles;       /* breaths in this session */
    int   phase;        /* 0 in, 1 hold, 2 out (last phase announced) */
    /* heart rate measured during the session */
    int   hr_on;
    uint64_t hr_integral;
    uint32_t hr_start_ms, hr_last_ms, hr_valid_ms, hr_epoch;
    int hr_previous, hr_have;
    int   hr_avg;
    /* prepared for draw */
    float ui_k;         /* title fade (quantised) */
    float pill_k;       /* duration pills: 1 shown, < 1 building out by scale steps */
    int   from_summary; /* intro starts from an empty screen (AGAIN) */
    float cy;           /* the disc's and the dial's centre */
    float disc_r;       /* the disc (START, then the breath); 0 = none */
    uint16_t disc_c;    /* its fill outside START (the START disc is white / pressed) */
    float core_b;       /* breath openness shown (audit) */
    float sum_p;        /* summary build-in */
    float prog_lit;     /* capsules lit; the fraction (eighths) grows the leading one */
    float dial_k[DIAL_N];   /* each capsule's own ink (builds in / out) */
    uint16_t txt_col;
    int   txt_id;       /* 0 in, 1 hold, 2 out */
} B;

static const int k_minutes[3] = { 1, 3, 5 };
static int s_min_i = 1;         /* selected duration (kept across visits) */
enum { CTL_NONE = -1, CTL_DISC = 0, CTL_PILL0, CTL_PILL1, CTL_PILL2, CTL_AGAIN, CTL_DONE };

/* ------------------------------------------------------------------ helpers */
static float ease_sine(float x) { x = m_clamp(x, 0, 1); return 0.5f - 0.5f * gfx_fast_cos(3.14159265f * x); }
/* The breath: a sine in-out with a fifth of linear in it. The pure sine spends its first
 * and last ~7 frames (16 on the exhale) under a quarter-pixel step: a field that has
 * visibly stopped but still repaints. With the linear share it arrives crisply (R5) and a
 * hold reads as a hold, while staying as soft. Peak step 1.3 px a frame (outer ring). */
static float ease_breath(float x) { x = m_clamp(x, 0, 1); return 0.8f * ease_sine(x) + 0.2f * x; }
static float qz(float v, float steps) { return floorf(v * steps + 0.5f) / steps; }
static int   fk(float t) { return (int)floorf(t * M_FPS + 0.5f); }    /* whole frames in t s */

static void haptic(int pattern) { if (nos_prefs.haptics) hal_haptic(pattern); }

/* Breath curve at session time t (0 = start of the first inhale), 0 contracted .. 1 open. */
static float breath(float t)
{
    if (t <= 0) return 0;
    float p = fmodf(t, T_CYCLE);
    if (p < T_IN) return ease_breath(p / T_IN);
    if (p < T_IN + T_HOLD) return 1;
    return 1.0f - ease_breath((p - T_IN - T_HOLD) / T_OUT);
}

/* The breath disc for openness b: radius on the 1/4 px grid, C_SEC grey (out) to C_PRI
 * (in) through the 11 neutral RGB565 greys between them (red 19..29, green 2r+1: a
 * channel-wise mix tints green), so a hold is truly still and a slow exhale repaints
 * only real changes. */
static float core_r(float b) { return qz(m_lerp(DISC_R, CORE_MAX, m_clamp(b, 0, 1)), 4.0f); }
static uint16_t core_c(float b)
{
    unsigned r = 19u + (unsigned)floorf(m_clamp(b, 0, 1) * 10.0f + 0.5f);
    return (uint16_t)(r << 11 | (2u * r + 1u) << 5 | r);
}

static void dial_ink(float k) { for (int i = 0; i < DIAL_N; i++) B.dial_k[i] = k; }
/* the dial builds in clockwise from 12 o'clock, one capsule a frame, each over 4 frames
 * in perceptual steps (15 frames in all); k = whole frames since the build started */
static void dial_build(int k)
{
    for (int i = 0; i < DIAL_N; i++) B.dial_k[i] = m_ink((float)(k - i) / 4.0f);
}

/* The dial: lit capsules white, the rest a dark track; the leading one grows outward
 * from its inner cap over its track. One gfx_line (a true capsule) per element. */
static void capsule(gfx_fb *fb, int i, float len, uint16_t c)
{
    float ux = k_dir[i][0], uy = k_dir[i][1], r0 = DIAL_R - DIAL_L * 0.5f;
    float ax = UI_CX + ux * r0, ay = B.cy + uy * r0;
    gfx_line(fb, ax, ay, ax + ux * len, ay + uy * len, DIAL_W, c);
}
static void draw_dial(gfx_fb *fb)
{
    int whole = (int)floorf(B.prog_lit);
    float part = B.prog_lit - (float)whole;
    for (int i = 0; i < DIAL_N; i++) {
        float k = B.dial_k[i];
        if (k <= 0.01f) continue;
        if (i < whole) { capsule(fb, i, DIAL_L, ui_dim(C_PRI, k)); continue; }
        capsule(fb, i, DIAL_L, ui_dim(C_LINE2, k));
        if (i == whole && part > 0.01f) capsule(fb, i, DIAL_L * part, ui_dim(C_PRI, k));
    }
}

/* -------------------------------------------------------------- session HR */
static void hr_start(void)
{
    B.hr_integral=0;B.hr_valid_ms=0;B.hr_have=0;B.hr_epoch=0;
    B.hr_start_ms=hal_millis();B.hr_avg=0;B.hr_on=0;
    if (hal_ext_present() & EXT_HR) { hal_hr_request(HAL_HR_OWNER_BREATHE,1); B.hr_on = 1; }
}
static void hr_stop(void)
{
    if (B.hr_on) hal_hr_request(HAL_HR_OWNER_BREATHE,0);
    B.hr_on = 0;
}
static void hr_sample(void)
{
    if (!B.hr_on) return;
    hal_hr r;uint32_t now=hal_millis();
    if (!hal_hr_get(&r) || !r.sample_ms || now-r.sample_ms>3000u ||
        now-r.sample_ms>now-B.hr_start_ms || !r.on_wrist ||
        r.bpm<35 || r.bpm>230 || !isfinite(r.quality) || r.quality<0.65f || r.quality>1.0f) {
        B.hr_have=0;return;
    }
    if(B.hr_have && (int32_t)(r.sample_ms-B.hr_last_ms)<=0)return;
    uint32_t gap=r.sample_ms-B.hr_last_ms;
    if(B.hr_have && gap<=3000u) {
        B.hr_integral+=(uint64_t)(r.bpm+B.hr_previous)*gap/2u;
        B.hr_valid_ms+=gap;
    }
    B.hr_have=1;B.hr_last_ms=r.sample_ms;B.hr_previous=r.bpm;
    B.hr_epoch=svc_heart_sample_epoch(r.sample_ms);
}
static int hr_average(uint32_t session_ms)
{
    return B.hr_valid_ms>=10000u && B.hr_valid_ms>=session_ms/2u?
        (int)((B.hr_integral+B.hr_valid_ms/2u)/B.hr_valid_ms):0;
}

/* ---------------------------------------------------------------- states */
static float session_len(void) { return (float)B.cycles * T_CYCLE; }

static void go(int st) { B.st = st; B.t = 0; }

static void session_begin(int from_summary)
{
    B.from_summary = from_summary;
    B.cycles = k_minutes[s_min_i] * 60 / (int)T_CYCLE;   /* 5 per minute */
    B.phase = -1;
    hr_start();
    go(ST_INTRO);
}

static void breathe_enter(void)
{
    B.pressed = CTL_NONE; B.press_hold = 0; B.unpress_at = 0;
    if (nos_revealed()) {
        /* back from a cover: keep the state; a running session keeps its HR stream (re-
         * enabled in case the HAL idle-stopped it under a long cover) and its average */
        if ((B.st == ST_INTRO || B.st == ST_SESSION) && (hal_ext_present() & EXT_HR)) { hal_hr_request(HAL_HR_OWNER_BREATHE,1); B.hr_on = 1; }
        return;
    }
    B.st = ST_START; B.t = 0; B.build = 0;
    /* the entry build runs from nos_enter_t() (40% of the iris); a finger pull skips it */
    B.et = nos_entered_by_finger() ? 10.0f : nos_t - nos_enter_t();
    B.hr_on = 0;
    B.pill_k = 0; B.disc_r = 0; B.prog_lit = 0; dial_ink(0);
    for (int i = 0; i < 3; i++) B.sel[i] = i == s_min_i;
}

static void breathe_leave(void)
{
    B.pressed = CTL_NONE; B.press_hold = 0; B.unpress_at = 0;
    /* covered by a push: a running session keeps its heart-rate stream */
    if (nos_leaving_covered()) return;
    hr_stop();
}

/* entry: the START disc and the pills arrive at full size and colour on the first frame
 * of the build (no scale-up), the empty dial builds in clockwise around the disc */
static void prepare_start(int k)
{
    B.cy = START_CY;
    dial_build(k);
    B.prog_lit = 0;
    B.ui_k = 1.0f;
    B.disc_r = k > 0 ? DISC_R : 0;
    B.disc_c = C_SEC;
    B.pill_k = k > 0 ? 1.0f : 0.0f;
}

/* t = session time (0 = first inhale): the disc follows the breath, the dial lights by
 * elapsed fraction (the leading capsule in whole eighths: one small repaint at a time) */
static void prepare_session(float t)
{
    float end = session_len();
    float b = breath(t > end ? end : t);
    B.cy = UI_CY;
    B.core_b = b;
    B.disc_r = core_r(b);
    B.disc_c = core_c(b);
    dial_ink(1.0f);
    B.prog_lit = floorf((float)DIAL_N * 8.0f * m_clamp(t / end, 0, 1)) / 8.0f;
}

/* AGAIN: the disc grows back in at the centre, t s after it starts (it starts T_OVL
 * before the summary has gone, so no frame is empty), EASE_STEADY over 10 frames from
 * 30 % on its first frame; the empty dial builds in clockwise with it */
static void regrow_field(float t)
{
    int k = fk(t);
    float g = k <= 0 ? 0.0f : k >= 10 ? 1.0f : bezier_ease(&EASE_STEADY, (float)k / 10.0f);
    if (t > 0 && g < 0.3f) g = 0.3f;
    B.cy = UI_CY;
    B.core_b = 0;
    B.disc_r = qz(DISC_R * g, 4.0f);
    B.disc_c = C_SEC;
    B.prog_lit = 0;
    dial_build(k);
}

/* summary labels: in from 40 % of the build over 5 frames; out with the build-out */
static float sum_label_k(void)
{
    return B.st == ST_SUMOUT ? m_ink(B.sum_p) : m_ink((B.sum_p - 0.4f) / 0.5f);
}

static void breathe_tick(float dt)
{
    B.t += dt;
    B.et += dt;
    B.build = m_clamp(B.et / UI_ENTRY_S, 0, 1);
    if (B.unpress_at > 0 && nos_t >= B.unpress_at && !B.press_hold) { B.pressed = CTL_NONE; B.unpress_at = 0; }
    for (int i = 0; i < 3; i++) {             /* pill selection crossfades over 4 frames */
        float tg = i == s_min_i ? 1.0f : 0.0f, st = dt / M_FRAMES(4);
        B.sel[i] = B.sel[i] < tg ? m_clamp(B.sel[i] + st, 0, tg) : m_clamp(B.sel[i] - st, tg, 1);
    }
    switch (B.st) {
    case ST_START:
        prepare_start(fk(B.et));
        B.txt_col = 0;
        break;
    case ST_INTRO: {
        /* whole frames from the tap (k = 1 on the tap's own frame): START sinks into the
         * (pressed, grey) disc over frames 1-3, the pills scale out (1-5) and go, the
         * title fades (1-6); the disc, now the breath fully out, glides with its dial into
         * the centre on the breath curve over 24 frames */
        nos_keep_awake();
        int k = fk(B.t);
        float p = m_clamp(B.t / (T_INTRO - 0.2f), 0, 1);
        float e = ease_breath(p);
        if (B.from_summary) {
            regrow_field(B.t);
            B.ui_k = 0;
            B.pill_k = 0;
        } else {
            B.cy = qz(m_lerp(START_CY, UI_CY, e), 4.0f);
            B.core_b = 0;
            B.disc_r = DISC_R;
            B.disc_c = C_SEC;
            B.prog_lit = 0;
            dial_ink(1.0f);
            B.ui_k = m_ink(1.0f - (float)k / 6.0f);
            B.pill_k = m_clamp(1.0f - (float)k / 6.0f, 0, 1);   /* the pills build out by scale steps */
        }
        /* "Breathe in" leads the first inhale by 0.2 s (no dead frames before it) */
        B.txt_id = 0;
        B.txt_col = ui_dim(C_PRI, m_ink((B.t - (T_INTRO - 0.2f)) / TXT_IN));
        if (B.t >= T_INTRO) { B.hr_start_ms=hal_millis();B.hr_have=0;go(ST_SESSION); }
        break;
    }
    case ST_SESSION: {
        nos_keep_awake();
        hr_sample();
        float t = B.t, end = session_len();
        prepare_session(t);
        /* phase announcements + haptics */
        float p = fmodf(t, T_CYCLE);
        int ph = p < T_IN ? 0 : p < T_IN + T_HOLD ? 1 : 2;
        if (t < end && ph != B.phase) {
            if (ph == 0) haptic(HAPTIC_TICK);
            else if (ph == 2) haptic(HAPTIC_DOUBLE);
            B.phase = ph;
        }
        /* text: fades in over TXT_IN after a phase starts, out over TXT_OUT before it ends */
        float tin = ph == 0 ? p : ph == 1 ? p - T_IN : p - T_IN - T_HOLD;
        float plen = ph == 0 ? T_IN : ph == 1 ? T_HOLD : T_OUT;
        float tout = plen - tin;
        if (t >= end - TXT_OUT) tout = end - t;            /* last exhale fades for the outro */
        float k = m_clamp((tin + (t < T_IN ? 0.2f : 0.0f)) / TXT_IN, 0, 1);   /* first inhale: continues the intro's fade */
        float k2 = m_clamp(tout / TXT_OUT, 0, 1);
        if (k2 < k) k = k2;
        B.txt_col = ui_dim(C_PRI, m_ink(k));
        B.txt_id = ph;
        if (t >= end) {
            hr_stop();
            /* Trapezoidal acquisition-time mean; require half of the declared
             * session, at least 10 seconds, rather than one cached reading. */
            B.hr_avg = hr_average((uint32_t)(end*1000.0f));
            if (B.hr_avg) svc_heart_record_at(B.hr_avg, 0, B.hr_epoch);
            haptic(HAPTIC_DOUBLE);
            go(ST_OUTRO);
        }
        break;
    }
    case ST_OUTRO: {
        /* the disc (breath out) closes to a point at a constant speed over frames 1-8 (an
         * exit); the full dial fades out over frames 5-10 */
        int k = fk(B.t);
        B.cy = UI_CY;
        B.core_b = 0;
        B.disc_r = qz(DISC_R * (1.0f - m_clamp((float)k / 8.0f, 0, 1)), 4.0f);
        B.disc_c = C_SEC;
        B.prog_lit = (float)DIAL_N;
        dial_ink(m_ink(1.0f - (float)(k - 4) / 6.0f));
        B.txt_col = 0;
        B.sum_p = 0;
        if (B.t >= T_OUTRO) go(ST_SUMMARY);
        break;
    }
    case ST_SUMMARY:
        B.disc_r = 0; dial_ink(0);
        B.sum_p = m_clamp(B.t / UI_ENTRY_S, 0, 1);
        break;
    case ST_SUMOUT:
        /* AGAIN: the summary builds out; the disc starts growing back T_OVL before it
         * has gone (the intro then continues from the same clock) */
        B.sum_p = 1.0f - m_clamp(B.t / T_SUMOUT, 0, 1);
        if (B.t > T_SUMOUT - T_OVL) regrow_field(B.t - (T_SUMOUT - T_OVL));
        else { B.disc_r = 0; dial_ink(0); }
        if (B.t >= T_SUMOUT) { float t = B.t - (T_SUMOUT - T_OVL); session_begin(1); B.t = t; regrow_field(B.t); }
        break;
    }
    /* motion audit (sim only): the centre, the disc (as a component outside the session,
     * as the breath inside it), the dial and every fade */
    if (B.disc_r > 0 || B.dial_k[0] > 0) {
        /* the centre glides 29 px into the session on the breath curve itself (<= 1.8 px
         * a frame over 24 frames): slow decorative motion, not a component move */
        nos_motion_probe("breathe.cy", MC_DECORATIVE, B.cy);
    }
    if (B.st == ST_SESSION) nos_motion_probe("breathe.core.r", MC_DECORATIVE, B.disc_r);
    else nos_motion_probe("breathe.disc.r", MC_COMPONENT, B.disc_r);
    nos_motion_probe("breathe.title.ink", MC_FADE, B.ui_k * 100.0f);
    nos_motion_probe("breathe.dial.ink0", MC_FADE, B.dial_k[0] * 100.0f);
    nos_motion_probe("breathe.dial.ink11", MC_FADE, B.dial_k[DIAL_N - 1] * 100.0f);
    nos_motion_probe("breathe.dial.lit", MC_DISCRETE, B.prog_lit);
    nos_motion_probe("breathe.txt.ink", MC_FADE, B.txt_col ? (float)(B.txt_col & 0x1F) / 31.0f * 100.0f : 0.0f);
    nos_motion_probe("breathe.pill.w", MC_MICRO, B.pill_k > 0.05f ? floorf(96.0f * (B.pill_k < 1 ? 0.4f + 0.6f * B.pill_k : 1.0f) + 0.5f) : 0.0f);
    nos_motion_probe("breathe.sel.ink", MC_FADE, qz(m_clamp(B.sel[s_min_i], 0, 1), 8.0f) * 100.0f);
    nos_motion_probe("breathe.sum.ink", MC_FADE, B.st >= ST_SUMMARY ? sum_label_k() * 100.0f : 0.0f);
}

/* ------------------------------------------------------------------ draw */
/* A pill at full colour. on: 0 off .. 1 on (crossfaded, quantised here). pressed: C_SEC
 * with black ink (the ui_button rule). k: 1 shown; below 1 it builds out by scale steps
 * at its own fill, its label sinking into the fill first (never dimmed through grey,
 * which would read as pressed). */
static void pill(gfx_fb *fb, float cx, float cy, float w, float h, const char *label,
                 float on, int pressed, float k)
{
    if (k <= 0.05f) return;
    on = qz(m_clamp(on, 0, 1), 8.0f);
    uint16_t fill = pressed ? C_SEC : ui_mix(C_SURF2, C_WHITE, on);
    /* the ink flips at the fill's midpoint, so the label never sinks into a mid grey
     * while the selection crossfades */
    uint16_t ink = pressed || on >= 0.5f ? C_BG : C_PRI;
    float s = 1.0f;
    if (k < 1) {
        s = 0.4f + 0.6f * k;
        ink = ui_mix(fill, ink, qz(m_clamp((k - 0.5f) / 0.5f, 0, 1), 3.0f));
    }
    float pw = floorf(w * s + 0.5f), ph = floorf(h * s + 0.5f);
    ui_rrect(fb, cx - pw / 2, cy - ph / 2, pw, ph, ph / 2, fill);
    if (ink != fill)
        ui_label(fb, (int)cx, (int)(cy + UI_F_LABEL->ascent * 0.5f - 2), label, ink, UI_CENTER);
}

/* the disc: on START white (pressed: grey), from the tap on the breath's own grey..white */
static void draw_disc(gfx_fb *fb)
{
    if (B.disc_r <= 0.5f) return;
    int start = B.st == ST_START || (B.st == ST_INTRO && !B.from_summary);
    uint16_t fill = !start ? B.disc_c : B.pressed == CTL_DISC || B.st == ST_INTRO ? C_SEC : C_WHITE;
    gfx_fill_circle(fb, UI_CX, B.cy, B.disc_r, fill);
    if (!start) return;
    /* START: at the tap its ink fades into the disc (0.1 s) as it starts to glide */
    float lk = B.st == ST_START ? 0.0f : qz(m_clamp(B.t / 0.1f, 0, 1), 8.0f);
    if (lk < 1)
        ui_label(fb, UI_CX, (int)(B.cy + UI_F_LABEL->ascent * 0.5f - 2), "START", ui_mix(C_BG, fill, lk), UI_CENTER);
}

static void draw_start(gfx_fb *fb)
{
    float k = B.ui_k;
    if (k > 0.01f) ui_label(fb, UI_CX, 70, "BREATHE", ui_dim(C_SEC, k), UI_CENTER);
    /* duration pills: with the build at full colour; scale out with the intro */
    for (int i = 0; i < 3; i++) {
        if (B.pill_k <= 0.05f) break;
        char b[8];
        snprintf(b, sizeof b, "%d MIN", k_minutes[i]);
        pill(fb, (float)(UI_CX + (i - 1) * 108), (float)PILL_Y, 96, 56, b,
             B.sel[i], B.pressed == CTL_PILL0 + i, B.pill_k);
    }
}

static void draw_session(gfx_fb *fb)
{
    if (B.txt_col) {
        static const char *k_txt[3] = { "Breathe in", "Hold", "Breathe out" };
        ui_text(fb, UI_F_TITLE, UI_CX, 432, k_txt[B.txt_id], B.txt_col, UI_CENTER, 0);
    }
}

static void draw_summary(gfx_fb *fb)
{
    /* p: build progress (SUMMARY) or 1 -> 0 while it builds out (SUMOUT) */
    float p = B.sum_p;
    int out = B.st == ST_SUMOUT;
    float kt = m_ink(out ? p : p / 0.6f);
    ui_label(fb, UI_CX, UI_TITLE_Y, "COMPLETE", ui_dim(C_SEC, kt), UI_CENTER);
    char b[16];
    snprintf(b, sizeof b, "%d", k_minutes[s_min_i]);
    dot_style st = { DOT_FONT_TALL, UI_HERO_PITCH, UI_HERO_PITCH * 0.8f, C_WHITE, 0, 1, 1 };
    /* the block sits 30 px above the button pair (it read as falling onto them) */
    dot_text_anim(fb, &st, UI_CX, 116, b, 1, p, DOT_REVEAL_RANDOM, 7);
    float pl = sum_label_k();
    ui_label(fb, UI_CX, 230, "MIN", ui_dim(C_SEC, pl), UI_CENTER);
    char line[40];
    snprintf(line, sizeof line, "%d BREATHS", B.cycles);
    ui_label(fb, UI_CX, 270, line, ui_dim(C_DIS, pl), UI_CENTER);
    if (B.hr_avg) {
        snprintf(line, sizeof line, "AVG %d BPM", B.hr_avg);
        ui_label(fb, UI_CX, 298, line, ui_dim(C_DIS, pl), UI_CENTER);
    } else ui_label(fb,UI_CX,298,"HEART RATE UNAVAILABLE",ui_dim(C_DIS,pl),UI_CENTER);
    snprintf(line,sizeof line,"HR COVERAGE %.0f%%",session_len()>0?B.hr_valid_ms/(session_len()*10.0f):0);
    ui_label(fb,UI_CX,324,line,ui_dim(C_DIS,pl),UI_CENTER);
    /* the action pair arrives with the hero at full colour; on AGAIN both build out by
     * scale steps at their own fills, the tapped AGAIN held pressed */
    float pb = out ? p : (p > 0 ? 1.0f : 0.0f);
    if (pb > 0.05f) {
        pill(fb, UI_BTN_PAIR_X0, UI_BTN_Y, UI_BTN_PAIR_W, UI_BTN_H, "AGAIN", 0, out || B.pressed == CTL_AGAIN, pb);
        pill(fb, UI_BTN_PAIR_X1, UI_BTN_Y, UI_BTN_PAIR_W, UI_BTN_H, "DONE", 1, B.pressed == CTL_DONE, pb);
    }
}

static void breathe_draw(gfx_fb *fb)
{
    switch (B.st) {
    case ST_START: case ST_INTRO:
        draw_dial(fb);
        draw_disc(fb);
        if (B.st == ST_INTRO) draw_session(fb);
        draw_start(fb);
        break;
    case ST_SESSION: case ST_OUTRO:
        draw_dial(fb);
        draw_disc(fb);
        draw_session(fb);
        break;
    case ST_SUMMARY: case ST_SUMOUT:
        if (B.st == ST_SUMOUT) { draw_dial(fb); draw_disc(fb); }
        draw_summary(fb);
        break;
    }
}

/* ----------------------------------------------------------------- input */
static int hit(int x, int y)
{
    if (B.st == ST_START) {
        float dx = (float)x - UI_CX, dy = (float)y - START_CY;
        if (dx * dx + dy * dy <= 66.0f * 66.0f) return CTL_DISC;
        if (y >= PILL_Y - 44 && y <= PILL_Y + 44)
            for (int i = 0; i < 3; i++)
                if (x >= UI_CX + (i - 1) * 108 - 54 && x < UI_CX + (i - 1) * 108 + 54) return CTL_PILL0 + i;
    } else if (B.st == ST_SUMMARY && B.sum_p > 0) {
        if (y >= UI_BTN_Y - UI_BTN_H / 2 && y <= UI_BTN_Y + UI_BTN_H / 2) {
            if (x >= UI_BTN_PAIR_X0 - UI_BTN_PAIR_W / 2 && x < UI_CX) return CTL_AGAIN;
            if (x >= UI_CX && x < UI_BTN_PAIR_X1 + UI_BTN_PAIR_W / 2) return CTL_DONE;
        }
    }
    return CTL_NONE;
}

static int breathe_input(const nos_input *in)
{
    if(in->type==IN_CANCEL) {B.pressed=CTL_NONE;B.press_hold=0;B.unpress_at=0;return 1;}
    switch (in->type) {
    case IN_DOWN:
        if (B.press_hold) return 1;              /* departing: the tapped control holds */
        B.pressed = hit(in->x, in->y);
        B.press_t0 = nos_t; B.unpress_at = 0;
        return B.pressed != CTL_NONE;
    case IN_DRAG_START:
        if (!B.press_hold) { B.pressed = CTL_NONE; B.unpress_at = 0; }
        return 0;
    case IN_UP:
        /* a quick tap still shows the pressed state for >= 3 frames */
        if (B.pressed != CTL_NONE && !B.press_hold) B.unpress_at = fmax(nos_t, B.press_t0 + PRESS_MIN);
        return 0;
    case IN_TAP: {
        if (B.press_hold) return 1;
        int c = hit(in->x, in->y);
        if (c == CTL_NONE) return 0;
        if (c == CTL_DISC) { nos_feedback(FB_TAP); session_begin(0); }
        else if (c >= CTL_PILL0 && c <= CTL_PILL2) {
            if (s_min_i != c - CTL_PILL0) { s_min_i = c - CTL_PILL0; nos_feedback(FB_TICK); }
        } else if (c == CTL_AGAIN) { nos_feedback(FB_TAP); go(ST_SUMOUT); }
        else if (c == CTL_DONE) {
            /* DONE stays pressed while the screen closes (leave() clears it) */
            B.pressed = CTL_DONE; B.press_hold = 1;
            nos_feedback(FB_TAP); nos_pop();
        }
        return 1;
    }
    default:
        return 0;
    }
}

const nos_screen scr_breathe = { "BREATHE", breathe_enter, breathe_leave, breathe_tick,
                                 breathe_draw, breathe_input, 0 };
