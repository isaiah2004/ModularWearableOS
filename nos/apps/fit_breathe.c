/* fit_breathe.c — guided breathing (health team).
 *
 * Three states on one screen:
 *   START    a static halo of dot rings around a white START disc, duration pills
 *   SESSION  the dot field breathes: inhale 4 s, hold 2 s, exhale 6 s (5 breaths/min)
 *   SUMMARY  minutes, breaths and (if the HR module locked) the average heart rate
 *
 * The field is NR concentric dot rings. Every ring follows the same breath curve with
 * its own lag (inner rings lead, a wave travels outward) and twists slightly as it
 * opens, so it reads as organic while staying perfectly periodic. Holds are truly
 * still (zero repaint). Positions are computed once per frame in tick() into a PSRAM
 * table; draw() only stamps them, rejecting by stripe, and records ONE damage command
 * (bbox + hash of every dot) for the whole field. Dot diameters are prewarmed (mask
 * bake) on the start screen so the first breath never stalls. */
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
#define RING_LAG 0.07f          /* s between neighbouring rings */
#define T_INTRO  1.00f          /* start screen -> first inhale */
#define T_OUTRO  0.70f          /* last exhale -> summary */
#define T_BUILD0 0.17f          /* entry build starts at 40% of the iris ... */
#define T_SUMOUT 0.18f          /* AGAIN: the summary builds out before the field returns */

/* ---- field geometry ---- */
#define NR       8
#define NDOT_MAX 240
#define START_CY 204.0f
#define START_B  0.50f
#define DISC_R   44.0f
#define PILL_Y   370
#define PROG_N   60
#define PROG_R   222.0f

typedef struct { float x, y, d; uint16_t c; } fdot;
typedef struct {
    float   ca[NDOT_MAX], sa[NDOT_MAX];   /* base unit vectors */
    uint8_t ring[NDOT_MAX];
    fdot    dot[NDOT_MAX];                /* this frame */
    int     n;                            /* dots drawn this frame */
    int     total;
} field_mem;

static field_mem *F;
static int s_ring0[NR + 1];               /* first dot index of each ring */
static const uint8_t k_ring_n[NR] = { 6, 12, 17, 23, 28, 34, 39, 45 };

/* field bbox + content hash for the frame (prepared in tick) */
static int      f_x0, f_y0, f_x1, f_y1;
static unsigned f_hash;

/* ---- state ---- */
enum { ST_START = 0, ST_INTRO, ST_SESSION, ST_OUTRO, ST_SUMMARY, ST_SUMOUT };
static struct {
    int   st;
    float t;            /* time in the current state */
    float build;        /* entry build progress 0..1 */
    float et;           /* since enter */
    float sel[3];       /* duration pills: selected-ness, crossfaded */
    int   pressed;      /* control under the finger, -1 none */
    int   cycles;       /* breaths in this session */
    int   phase;        /* 0 in, 1 hold, 2 out (last phase announced) */
    /* heart rate measured during the session */
    int   hr_on;
    float hr_sum;
    int   hr_n;
    int   hr_avg;
    /* prewarm progress (mask bake) */
    int   warm_q;
    /* prepared for draw */
    float ui_k;         /* title + pills fade (quantised) */
    int   from_summary; /* intro starts from an empty screen (AGAIN) */
    float disc_r;
    float sum_p;        /* summary build-in */
    float prog_lit;
    float prog_k;       /* progress ring fade (quantised) */
    uint16_t txt_col;
    int   txt_id;       /* 0 in, 1 hold, 2 out */
} B;

static const int k_minutes[3] = { 1, 3, 5 };
static int s_min_i = 1;         /* selected duration (kept across visits) */
enum { CTL_NONE = -1, CTL_DISC = 0, CTL_PILL0, CTL_PILL1, CTL_PILL2, CTL_AGAIN, CTL_DONE };

/* ------------------------------------------------------------------ helpers */
static float ease_sine(float x) { x = m_clamp(x, 0, 1); return 0.5f - 0.5f * cosf(3.14159265f * x); }
static float qz(float v, float steps) { return floorf(v * steps + 0.5f) / steps; }

static void haptic(int pattern) { if (nos_prefs.haptics) hal_haptic(pattern); }

/* Breath curve at session time t (0 = start of the first inhale), 0 contracted .. 1 open. */
static float breath(float t)
{
    if (t <= 0) return 0;
    float p = fmodf(t, T_CYCLE);
    if (p < T_IN) return ease_sine(p / T_IN);
    if (p < T_IN + T_HOLD) return 1;
    return 1.0f - ease_sine((p - T_IN - T_HOLD) / T_OUT);
}

static int field_alloc(void)
{
    if (F) return 1;
    F = (field_mem *)hal_big_alloc(sizeof(field_mem));
    if (!F) return 0;
    int k = 0;
    for (int i = 0; i < NR; i++) {
        s_ring0[i] = k;
        for (int j = 0; j < k_ring_n[i] && k < NDOT_MAX; j++, k++) {
            float a = 6.2831853f * (float)j / (float)k_ring_n[i];
            F->ca[k] = cosf(a);
            F->sa[k] = sinf(a);
            F->ring[k] = (uint8_t)i;
        }
    }
    s_ring0[NR] = k;
    F->total = k;
    F->n = 0;
    return 1;
}

/* Lay the field out: per ring openness b[i] (0..1), size multiplier grow[i] (0..1),
 * brightness scale kc. Rings whose radius is below rcut are skipped (under the disc). */
static void field_layout(float cx, float cy, const float *b, const float *grow, float kc, float rcut)
{
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    unsigned h = 0xB4EA7u;
    int n = 0;
    for (int i = 0; i < NR; i++) {
        float u = (float)i / (float)(NR - 1);
        float bi = m_clamp(b[i], 0, 1);
        float rc = 10.0f + 48.0f * u, re = 30.0f + 135.0f * u;
        float r = m_lerp(rc, re, bi);
        float de = 10.0f - 3.0f * u;
        float d = m_lerp(4.0f, de, bi) * m_clamp(grow[i], 0, 1);
        d = qz(d, 4.0f);                       /* the stamp's own quantum */
        if (d < 0.5f || r < rcut) continue;
        float k = (0.42f + 0.58f * bi) * (1.0f - 0.5f * u) * kc;
        k = qz(m_clamp(k, 0, 1), 16.0f);
        if (k <= 0) continue;
        uint16_t col = ui_dim(C_PRI, k);
        /* twist as the ring opens: alternate directions, fixed per-ring offset */
        float tw = (float)i * 0.41f + 0.22f * bi * ((i & 1) ? 1.0f : -1.0f);
        float ct = cosf(tw), st = sinf(tw);
        for (int j = s_ring0[i]; j < s_ring0[i + 1]; j++) {
            float ux = F->sa[j] * ct + F->ca[j] * st;      /* sin(a + tw) */
            float uy = F->ca[j] * ct - F->sa[j] * st;      /* cos(a + tw) */
            /* positions on the stamp's 1/4 px grid so equal hashes mean equal pixels */
            float x = qz(cx + r * ux, 4.0f), y = qz(cy - r * uy, 4.0f);
            fdot *o = &F->dot[n++];
            o->x = x; o->y = y; o->d = d; o->c = col;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
            h = gfx_hash_u(h, (unsigned)(int)(x * 4) * 2654435761u ^ (unsigned)(int)(y * 4));
            h = gfx_hash_u(h, (unsigned)(int)(d * 4) << 16 | col);
        }
    }
    F->n = n;
    if (!n) { f_x0 = f_y0 = f_x1 = f_y1 = 0; f_hash = 0; return; }
    float e = 7.0f;                           /* max dot radius + AA */
    f_x0 = (int)floorf(x0 - e); f_y0 = (int)floorf(y0 - e);
    f_x1 = (int)ceilf(x1 + e);  f_y1 = (int)ceilf(y1 + e);
    f_hash = h;
}

static void field_draw(gfx_fb *fb)
{
    if (!F || !F->n) return;
    if (fb->rec) { gfx_rec_cmd(fb, f_x0, f_y0, f_x1, f_y1, f_hash); return; }
    float ylo = (float)(fb->y_base - fb->oy), yhi = ylo + (float)fb->h;
    if ((float)f_y1 <= ylo || (float)f_y0 >= yhi) return;
    for (int i = 0; i < F->n; i++) {
        const fdot *o = &F->dot[i];
        float e = o->d * 0.5f + 1.0f;
        if (o->y + e <= ylo || o->y - e >= yhi) continue;
        dot_disc(fb, o->x, o->y, o->d, o->c);
    }
}

/* Bake the dot masks this screen uses (0.25 px steps up to 10.5 px) a few per frame,
 * by stamping into a one-row scratch buffer, so the first breath never stalls. */
static uint16_t *s_warm_row;
static void prewarm_step(int per_frame)
{
    if (B.warm_q > 42) return;
    if (!s_warm_row) s_warm_row = (uint16_t *)hal_big_alloc(GFX_W * 2);
    if (!s_warm_row) { B.warm_q = 99; return; }
    gfx_fb fb;
    memset(&fb, 0, sizeof fb);
    fb.px = s_warm_row; fb.h = 1; fb.cx0 = 0; fb.cx1 = GFX_W;
    for (int i = 0; i < per_frame && B.warm_q <= 42; i++, B.warm_q++)
        dot_disc(&fb, 20.0f, 0.0f, (float)B.warm_q * 0.25f, C_WHITE);
}

/* -------------------------------------------------------------- session HR */
static void hr_start(void)
{
    B.hr_sum = 0; B.hr_n = 0; B.hr_avg = 0; B.hr_on = 0;
    if (hal_ext_present() & EXT_HR) { hal_hr_enable(1); B.hr_on = 1; }
}
static void hr_stop(void)
{
    if (B.hr_on) hal_hr_enable(0);
    B.hr_on = 0;
}
static void hr_sample(void)
{
    if (!B.hr_on) return;
    hal_hr r;
    if (!hal_hr_get(&r)) return;
    if (r.on_wrist && r.bpm > 30 && r.bpm < 220 && r.quality >= 0.5f) { B.hr_sum += (float)r.bpm; B.hr_n++; }
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
    field_alloc();
    B.st = ST_START; B.t = 0; B.build = 0;
    B.et = nos_revealed() ? 10.0f : 0.0f;          /* back from a cover: no rebuild */
    B.pressed = CTL_NONE;
    B.hr_on = 0;
    B.warm_q = 1;
    for (int i = 0; i < 3; i++) B.sel[i] = i == s_min_i;
}

static void breathe_leave(void)
{
    hr_stop();
    B.pressed = CTL_NONE;
}

static void prepare_start(float build)
{
    float b[NR], g[NR];
    for (int i = 0; i < NR; i++) {
        b[i] = START_B;
        g[i] = bezier_ease(&EASE_STANDARD, m_stagger(build, i, NR, 0.55f));
    }
    field_layout(UI_CX, START_CY, b, g, 0.85f, DISC_R + 12.0f);
    B.ui_k = 1.0f;
    /* the disc and pills arrive at full size and colour with the dot build (no scale-up) */
    B.disc_r = build > 0 ? DISC_R : 0;
}

static void prepare_session(float t)
{
    /* t = session time (0 = first inhale) */
    float end = session_len();
    float b[NR], g[NR];
    for (int i = 0; i < NR; i++) {
        float ti = t - RING_LAG * (float)i;
        if (ti > end) ti = end;
        b[i] = breath(ti);
        g[i] = 1;
    }
    field_layout(UI_CX, UI_CY, b, g, 1.0f, 0);
}

static void breathe_tick(float dt)
{
    B.t += dt;
    B.et += dt;
    B.build = m_clamp((B.et - T_BUILD0) / UI_ENTRY_S, 0, 1);
    for (int i = 0; i < 3; i++) {             /* pill selection crossfades over 0.15 s */
        float tg = i == s_min_i ? 1.0f : 0.0f, st = dt / 0.15f;
        B.sel[i] = B.sel[i] < tg ? m_clamp(B.sel[i] + st, 0, tg) : m_clamp(B.sel[i] - st, tg, 1);
    }
    if (!F) return;
    switch (B.st) {
    case ST_START:
        prewarm_step(3);
        prepare_start(B.build);
        B.txt_col = 0; B.prog_lit = 0; B.prog_k = 0;
        break;
    case ST_INTRO: {
        nos_keep_awake();
        hr_sample();
        float p = m_clamp(B.t / (T_INTRO - 0.2f), 0, 1);
        float e = ease_sine(p);
        float b[NR], g[NR];
        float disc = m_clamp(B.t / 0.35f, 0, 1);
        if (B.from_summary) {
            /* AGAIN: the contracted field grows back in from its core */
            for (int i = 0; i < NR; i++) {
                b[i] = 0;
                g[i] = bezier_ease(&EASE_STANDARD, m_stagger(p, i, NR, 0.5f));
            }
            field_layout(UI_CX, UI_CY, b, g, 1.0f, 0);
            B.ui_k = 0;
            B.disc_r = 0;
        } else {
            for (int i = 0; i < NR; i++) {
                b[i] = START_B * (1.0f - e);
                /* rings hidden under the disc grow in as it shrinks */
                g[i] = i < 3 ? bezier_ease(&EASE_STANDARD, m_clamp((disc - 0.3f * (2 - i) / 2.0f) * 1.6f, 0, 1)) : 1.0f;
            }
            field_layout(UI_CX, m_lerp(START_CY, UI_CY, e), b, g, m_lerp(0.85f, 1.0f, e), 0);
            B.ui_k = qz(1.0f - m_clamp(B.t / 0.3f, 0, 1), 6.0f);
            B.disc_r = qz(DISC_R * (1.0f - bezier_ease(&EASE_ACCEL, disc)), 4.0f);
        }
        B.prog_k = qz(m_clamp((B.t - 0.4f) / 0.5f, 0, 1), 6.0f);
        B.prog_lit = 0;
        /* "Breathe in" leads the first inhale by 0.2 s (no dead frames before it) */
        B.txt_id = 0;
        B.txt_col = ui_dim(C_PRI, qz(m_clamp((B.t - (T_INTRO - 0.2f)) / 0.33f, 0, 1), 6.0f));
        if (B.t >= T_INTRO) { go(ST_SESSION); }
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
        /* text: fade in over 0.33 s after a phase starts, out over 0.2 s before it ends */
        float tin = ph == 0 ? p : ph == 1 ? p - T_IN : p - T_IN - T_HOLD;
        float plen = ph == 0 ? T_IN : ph == 1 ? T_HOLD : T_OUT;
        float tout = plen - tin;
        if (t >= end - 0.2f) tout = end - t;               /* last exhale fades for the outro */
        float k = m_clamp((tin + (t < T_IN ? 0.2f : 0.0f)) / 0.33f, 0, 1);   /* first inhale: continues the intro's fade */
        float k2 = m_clamp(tout / 0.2f, 0, 1);
        if (k2 < k) k = k2;
        B.txt_col = ui_dim(C_PRI, qz(k, 6.0f));
        B.txt_id = ph;
        B.prog_k = 1;
        B.prog_lit = floorf((float)PROG_N * m_clamp(t / end, 0, 1));
        if (t >= end) {
            hr_stop();
            B.hr_avg = B.hr_n > 0 ? (int)(B.hr_sum / (float)B.hr_n + 0.5f) : 0;
            if (B.hr_avg) svc_heart_record(B.hr_avg, 0);
            haptic(HAPTIC_DOUBLE);
            go(ST_OUTRO);
        }
        break;
    }
    case ST_OUTRO: {
        float end = session_len();
        float p = m_clamp(B.t / T_OUTRO, 0, 1);
        float b[NR], g[NR];
        for (int i = 0; i < NR; i++) {
            float ti = end + B.t - RING_LAG * (float)i;
            if (ti > end) ti = end;
            b[i] = breath(ti);
            /* outer rings vanish first, the core last: the field closes to a point */
            g[i] = 1.0f - bezier_ease(&EASE_ACCEL, m_stagger(p, NR - 1 - i, NR, 0.5f));
        }
        field_layout(UI_CX, UI_CY, b, g, 1.0f, 0);
        B.txt_col = 0;
        B.prog_lit = (float)PROG_N;
        B.prog_k = 1;
        {
            float k = 1.0f - m_clamp((B.t - 0.2f) / 0.5f, 0, 1);
            B.prog_k = qz(k, 6.0f);
        }
        B.sum_p = 0;
        if (B.t >= T_OUTRO) go(ST_SUMMARY);
        break;
    }
    case ST_SUMMARY:
        if (F) F->n = 0;
        B.sum_p = m_clamp(B.t / UI_ENTRY_S, 0, 1);
        break;
    case ST_SUMOUT:
        /* AGAIN: the summary builds out, then the field grows back in */
        if (F) F->n = 0;
        B.sum_p = 1.0f - m_clamp(B.t / T_SUMOUT, 0, 1);
        if (B.t >= T_SUMOUT) session_begin(1);
        break;
    }
}

/* ------------------------------------------------------------------ draw */
static void pill(gfx_fb *fb, float cx, float cy, float w, float h, const char *label,
                 float on, int pressed, float k)
{
    /* on: 0 off .. 1 on (crossfaded, quantised here); k: fade level 0..1, quantised */
    if (k <= 0.01f) return;
    on = qz(m_clamp(on, 0, 1), 8.0f);
    uint16_t fill = ui_mix(pressed ? C_SURF3 : C_SURF2, pressed ? C_SEC : C_WHITE, on);
    uint16_t ink = ui_mix(C_PRI, C_BG, on);
    fill = ui_dim(fill, k);
    if (on < 0.5f) ink = ui_dim(ink, k);
    ui_rrect(fb, cx - w / 2, cy - h / 2, w, h, h / 2, fill);
    ui_label(fb, (int)cx, (int)(cy + UI_F_LABEL->ascent * 0.5f - 2), label, ink, UI_CENTER);
}

static void draw_start(gfx_fb *fb)
{
    float k = B.ui_k;
    if (k > 0.01f) ui_label(fb, UI_CX, 70, "BREATHE", ui_dim(C_SEC, k), UI_CENTER);
    field_draw(fb);
    if (B.disc_r > 0.5f) {
        uint16_t fill = B.pressed == CTL_DISC ? C_SEC : C_WHITE;
        gfx_fill_circle(fb, UI_CX, START_CY, B.disc_r, fill);
        /* START: at the tap its ink fades into the disc (0.1 s) before the disc shrinks */
        float lk = B.st == ST_START ? 0.0f : qz(m_clamp(B.t / 0.1f, 0, 1), 8.0f);
        if (lk < 1 && B.disc_r > DISC_R * 0.7f)
            ui_label(fb, UI_CX, (int)(START_CY + UI_F_LABEL->ascent * 0.5f - 2), "START", ui_mix(C_BG, fill, lk), UI_CENTER);
    }
    /* duration pills: with the build at full colour; fade with the intro */
    for (int i = 0; i < 3; i++) {
        float kk = B.build > 0 ? k : 0;
        if (kk <= 0.01f) continue;
        char b[8];
        snprintf(b, sizeof b, "%d MIN", k_minutes[i]);
        pill(fb, (float)(UI_CX + (i - 1) * 108), (float)PILL_Y, 96, 56, b,
             B.sel[i], B.pressed == CTL_PILL0 + i, kk);
    }
}

static void draw_session(gfx_fb *fb)
{
    if (B.prog_k > 0.01f) dot_ring(fb, UI_CX, UI_CY, PROG_R, PROG_N, 3.5f, 0, B.prog_lit, ui_dim(C_SEC, B.prog_k), ui_dim(C_LINE2, B.prog_k));
    field_draw(fb);
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
    float kt = qz(out ? p : m_clamp(p / 0.6f, 0, 1), 8.0f);
    ui_label(fb, UI_CX, UI_TITLE_Y, "COMPLETE", ui_dim(C_SEC, kt), UI_CENTER);
    char b[16];
    snprintf(b, sizeof b, "%d", k_minutes[s_min_i]);
    dot_style st = { DOT_FONT_TALL, UI_HERO_PITCH, UI_HERO_PITCH * 0.8f, C_WHITE, 0, 1, 1 };
    dot_text_anim(fb, &st, UI_CX, 132, b, 1, p, DOT_REVEAL_RANDOM, 7);
    float pl = qz(out ? p : m_clamp((p - 0.4f) / 0.5f, 0, 1), 8.0f);
    ui_label(fb, UI_CX, 246, "MIN", ui_dim(C_SEC, pl), UI_CENTER);
    char line[40];
    snprintf(line, sizeof line, "%d BREATHS", B.cycles);
    ui_label(fb, UI_CX, 286, line, ui_dim(C_DIS, pl), UI_CENTER);
    if (B.hr_avg) {
        snprintf(line, sizeof line, "AVG %d BPM", B.hr_avg);
        ui_label(fb, UI_CX, 314, line, ui_dim(C_DIS, pl), UI_CENTER);
    }
    /* the action pair arrives with the hero at full colour; it fades on the way out */
    float pb = out ? qz(p, 8.0f) : (p > 0 ? 1.0f : 0.0f);
    if (pb > 0.01f) {
        pill(fb, UI_BTN_PAIR_X0, UI_BTN_Y, UI_BTN_PAIR_W, UI_BTN_H, "AGAIN", 0, B.pressed == CTL_AGAIN, pb);
        pill(fb, UI_BTN_PAIR_X1, UI_BTN_Y, UI_BTN_PAIR_W, UI_BTN_H, "DONE", 1, B.pressed == CTL_DONE, pb);
    }
}

static void breathe_draw(gfx_fb *fb)
{
    switch (B.st) {
    case ST_START: case ST_INTRO:
        if (B.st == ST_INTRO) draw_session(fb);
        draw_start(fb);
        break;
    case ST_SESSION: case ST_OUTRO:
        draw_session(fb);
        break;
    case ST_SUMMARY: case ST_SUMOUT:
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
    switch (in->type) {
    case IN_DOWN:
        B.pressed = hit(in->x, in->y);
        return B.pressed != CTL_NONE;
    case IN_UP:
    case IN_DRAG_START:
        B.pressed = CTL_NONE;
        return 0;
    case IN_TAP: {
        int c = hit(in->x, in->y);
        B.pressed = CTL_NONE;
        if (c == CTL_NONE) return 0;
        if (c == CTL_DISC) { nos_feedback(FB_TAP); session_begin(0); }
        else if (c >= CTL_PILL0 && c <= CTL_PILL2) {
            if (s_min_i != c - CTL_PILL0) { s_min_i = c - CTL_PILL0; nos_feedback(FB_TICK); }
        } else if (c == CTL_AGAIN) { nos_feedback(FB_TAP); go(ST_SUMOUT); }
        else if (c == CTL_DONE) { nos_feedback(FB_TAP); nos_pop(); }
        return 1;
    }
    default:
        return 0;
    }
}

const nos_screen scr_breathe = { "BREATHE", breathe_enter, breathe_leave, breathe_tick,
                                 breathe_draw, breathe_input, 0 };
