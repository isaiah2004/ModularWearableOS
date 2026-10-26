/* sys_power.c — power sheet (button long press, TX_RISE).
 *
 * Battery level as the hero (dot-matrix %, titled BATTERY or USB POWER) over a row of ten
 * 12 x 12 rounded squares (5 px gaps, prog.h) that sweeps in to the level, then the actions
 * on the section 8 grid: SLEEP and RESTART as a pair, POWER OFF on its own in the thumb
 * row. Red is the lettering only (one red element): POWER OFF's label, then the armed
 * copy; outlines stay neutral. Restart and power off are arm-then-tap-again: the first tap
 * snaps the pill to 320 with "TAP AGAIN TO ..." in red over three segments that drain
 * over the 3 s, while its row partner fades out; a second tap within 3 s commits, and the label
 * crossfades to [ RESTARTING ] / [ POWERING OFF ] before the host acts (firmware
 * nos_main.c / the sim). The pill's size changes are mechanical cuts (MOTION30 §5,
 * m_cut: a 1 px nudge, a lead-in, one jump frame, 4-2-1-0 px settle; RESTART's edge
 * travels 169 px, too far to slide within the 16 px/frame micro budget), while its
 * colours and labels crossfade over 4 frames on their own clock. */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "prog.h"
#include "apps/nos_apps.h"
#include <math.h>
#include <stdio.h>

extern void nos_host_power(int mode);      /* 0 sleep display, 1 restart, 2 power off */

#define ROW1_Y  272.0f                     /* SLEEP | RESTART */
#define ROW2_Y  ((float)UI_BTN_Y)          /* POWER OFF */
#define HERO_Y  100.0f                     /* the % (79 px tall), then the cells under it */
#define CELL_N  10                         /* battery: ten 12 x 12 rounded squares, 5 px gaps */
#define CELL_SLOT 17.0f
#define CELL_Y  205.0f
#define CELL_IN 3                          /* the cells' sweep starts 3 frames into the build */
#define ARM_W   320.0f                     /* an armed pill */
#define ARM_S   3.0f                       /* armed for 3 s, one dot per second */
#define GROW_S  M_FRAMES(4)                /* armed pill's colours turn, its row partner fades */
#define OTHER_S M_FRAMES(4)                /* the other pills grey / come back */
#define DONE_S  (4.0f / NOS_FPS)           /* commit: the label crossfades over 4 frames */
#define ACT_S   0.8f                       /* the pill says so before the host acts */

enum { B_SLEEP = 0, B_RESTART, B_OFF, NB };

/* Section 8 pressed state: hidden for the first 100 ms (a drag's slop passes first and
 * cancels it silently), then on until release; a quick tap still shows it for 3 frames. */
static struct { int id, down, shown; nos_time_t t0, until; } P = { -1, 0, 0, 0, 0 };
static void press_down(int id) { P.id = id; P.down = id >= 0; P.shown = 0; P.t0 = nos_t; P.until = 0; }
static void press_cancel(void) { P.id = -1; P.down = 0; P.until = 0; }
static void press_up(void)
{
    if (!P.down) return;
    P.down = 0;
    nos_time_t vis = P.t0 + 0.09f;                    /* when it became visible */
    P.until = nos_t < vis ? nos_t + 0.09f : vis + 0.10f;
}
static int press_on(int id)
{
    if (id < 0 || P.id != id) return 0;
    if (P.down) return P.shown && nos_touch_travel() <= 8;
    return nos_t < P.until;
}
/* a held press lights once 90 ms have passed with the finger still (<= 3 px), then
 * stays lit until release unless the finger passes 8 px (a drag starting) */
static void press_tick(void)
{
    if (P.down && !P.shown && nos_t - P.t0 >= 0.09f && nos_touch_travel() <= 3) P.shown = 1;
}

static struct {
    int arm;                 /* armed pill, -1 none */
    nos_time_t arm_t[NB];         /* when each pill armed (its dots keep counting as it shrinks) */
    float g[NB];             /* 0..1 resting -> armed colours, linear in time */
    nos_time_t cut_t0[NB];        /* the geometry's cut started (one frame in) */
    int cut_dir[NB];         /* 1 growing to armed, -1 back to rest */
    float oth[NB];           /* 0..1 another pill is armed: this one greys (one red at a time) */
    uint16_t from[NB];       /* the fill the pill grows from (its pressed fill, if showing) */
    int done;                /* committed action, -1 none */
    nos_time_t done_t;
    int fired;
    nos_time_t t_enter;
    int bat, charging;
    char pct[8];
    int cells;               /* lit cells: the level to the nearest 10 %, at least 1 */
} S;

static float q8(float x) { return floorf(m_clamp(x, 0, 1) * 8.0f + 0.5f) / 8.0f; }
static uint16_t rest_fill(int i) { return i == B_OFF ? C_BG : C_SURF2; }
/* the armed geometry drawn now (0 rest, 1 armed): a mechanical cut over the pill's
 * farthest-moving edge in px (RESTART's left edge 169, POWER OFF's 60), so the settle is
 * 4-2-1-0 px whatever the travel */
static float edge_d(int i) { return i == B_RESTART ? 169.0f : 60.0f; }
static float geo(int i)
{
    if (i == B_SLEEP) return 0.0f;
    int k = m_frame_k(S.cut_t0[i]);
    float d = edge_d(i);
    return (S.cut_dir[i] > 0 ? m_cut(0, d, k) : m_cut(d, 0, k)) / d;
}
static void cut_start(int i, int dir)
{
    if (S.cut_dir[i] == dir) return;
    S.cut_dir[i] = dir;
    S.cut_t0[i] = nos_t - 1.0f / NOS_FPS;      /* from input: this frame already moves */
}

/* the armed copy keeps the house tracking when it fits, else tightens to clear the caps */
static void fit_label(gfx_fb *fb, float cx, int y, float w, const char *s, uint16_t col)
{
    int tr = 2;
    while (tr > 0 && ui_text_width(UI_F_LABEL, s, tr) > w - 56) tr--;
    ui_text(fb, UI_F_LABEL, (int)cx, y, s, col, UI_CENTER, tr);
}

static void disarm(void)
{
    if (S.arm < 0) return;
    S.from[S.arm] = rest_fill(S.arm);          /* shrinks back to its resting look */
    cut_start(S.arm, -1);
    S.arm = -1;
}

static void arm(int b)
{
    disarm();
    int shown = press_on(b);                   /* grow from the pressed fill the finger saw */
    S.from[b] = shown ? (b == B_OFF ? C_SURF2 : C_SEC) : rest_fill(b);
    press_cancel();                            /* arming is the feedback from here */
    S.arm = b;
    S.arm_t[b] = nos_t;
    cut_start(b, 1);
    nos_feedback(FB_LIMIT);
}

static void commit(int b)
{
    press_up();
    S.done = b;
    S.done_t = nos_t;
    S.fired = 0;
    nos_feedback(b == B_OFF ? FB_ERROR : FB_TAP);
}

static void p_enter(void)
{
    press_cancel();
    S.arm = -1;
    S.done = -1;
    S.fired = 0;
    for (int i = 0; i < NB; i++) {
        S.g[i] = 0; S.oth[i] = 0; S.arm_t[i] = 0; S.from[i] = rest_fill(i);
        S.cut_dir[i] = -1; S.cut_t0[i] = nos_t - 10.0f;
    }
    S.t_enter = nos_enter_t();
}

static void p_leave(void)
{
    if (P.down) press_cancel();
}

static void p_probe(void);

static void p_tick(float dt)
{
    press_tick();
    const svc_sys_t *s = svc_sys();
    S.bat = s->battery_pct > 0 ? s->battery_pct : hal_battery_percent();
    S.charging = s->charging;
    snprintf(S.pct, sizeof S.pct, "%d%%", S.bat);
    S.cells = S.bat <= 0 ? 0 : (S.bat + 5) / 10 < 1 ? 1 : (S.bat + 5) / 10 > CELL_N ? CELL_N : (S.bat + 5) / 10;
    if (S.arm >= 0 && S.done < 0 && nos_t - S.arm_t[S.arm] >= ARM_S) disarm();   /* last dot out */
    for (int i = 0; i < NB; i++) {
        float tgt = i == S.arm || i == S.done ? 1.0f : 0.0f;
        float st = dt / GROW_S;
        S.g[i] = S.g[i] < tgt ? fminf(tgt, S.g[i] + st) : fmaxf(tgt, S.g[i] - st);
        /* the others grey while one is armed, and come back over 150 ms when it expires */
        int lead = S.done >= 0 ? S.done : S.arm;
        float ot = lead >= 0 && lead != i ? 1.0f : 0.0f, os = dt / OTHER_S;
        S.oth[i] = S.oth[i] < ot ? fminf(ot, S.oth[i] + os) : fmaxf(ot, S.oth[i] - os);
    }
    if (S.done >= 0 && !S.fired && nos_t - S.done_t >= ACT_S) {
        S.fired = 1;
        nos_host_power(S.done == B_OFF ? 2 : 1);   /* the device does not come back from this */
        nos_home();                                /* the sim does: the sheet goes away */
    }
    p_probe();
}

/* the battery cells sweep in: EASE_STEADY over 14 frames plus one per lit cell, landing 2 frames early (0.43-0.73 s),
 * the leading cell inking in over its share (prog.h), from CELL_IN frames into the build */
static int cells_n(void) { return 14 + S.cells; }
static float cells_u(void)
{
    int k = m_frame_k(S.t_enter) - CELL_IN, n = cells_n();
    /* lands on frame n - 2: EASE_STEADY's last two frames are sub-pixel slivers */
    return k <= 0 ? 0.0f : k >= n - 2 ? 1.0f : bezier_ease(&EASE_STEADY, (float)k / (float)n);
}

/* entry build per pill: rise 18 px on the 6-frame table (5 5 3 3 1 1 px) as its ink comes
 * up in perceptual steps, from 2 whole frames after the hero starts, 2 frames apart */
static float in_k(int i)
{
    return mf_at(&MF_MICRO6, m_stagger_k(m_frame_k(S.t_enter) - 2, i, 2));
}

static void pill_geo(int i, float g, float *cx, float *cy, float *w)
{
    if (i == B_SLEEP) { *cx = UI_BTN_PAIR_X0; *cy = ROW1_Y; *w = UI_BTN_PAIR_W; return; }
    if (i == B_RESTART) {
        *cx = UI_BTN_PAIR_X1 + (UI_CX - UI_BTN_PAIR_X1) * g;
        *cy = ROW1_Y;
        *w = UI_BTN_PAIR_W + (ARM_W - UI_BTN_PAIR_W) * g;
        return;
    }
    *cx = UI_CX; *cy = ROW2_Y; *w = UI_BTN_W + (ARM_W - UI_BTN_W) * g;
}

static void pill(gfx_fb *fb, float cx, float cy, float w, int solid, uint16_t fill, uint16_t line)
{
    float x = floorf(cx - w * 0.5f + 0.5f), y = cy - UI_BTN_H * 0.5f;
    w = floorf(w + 0.5f);
    if (solid) ui_rrect(fb, x, y, w, UI_BTN_H, UI_BTN_H * 0.5f, fill);
    if (line) ui_rrect_line(fb, x, y, w, UI_BTN_H, UI_BTN_H * 0.5f, 2, line);
}

static void draw_pill(gfx_fb *fb, int i, float k, float dy)
{
    static const char *const name[NB] = { "SLEEP", "RESTART", "POWER OFF" };
    static const char *const again[NB] = { "", "TAP AGAIN TO RESTART", "TAP AGAIN TO POWER OFF" };
    static const char *const doing[NB] = { "", "[ RESTARTING ]", "[ POWERING OFF ]" };
    float g = S.g[i], cx, cy, w;                        /* g: the colours' clock */
    pill_geo(i, geo(i), &cx, &cy, &w);                  /* the shape: the cut */
    cy += dy;
    if (i == B_SLEEP) k *= m_ink(1.0f - S.g[B_RESTART]);   /* its partner is armed: give way */
    if (k <= 0) return;
    int pressed = press_on(i);
    int asc = UI_F_LABEL->ascent;
    int by = (int)(cy + asc * 0.5f - 2), ay = (int)(cy + asc * 0.5f - 10);

    if (g <= 0) {                                       /* at rest: the section 8 buttons */
        uint16_t fill = C_BG, line = 0, ink;
        float ko = floorf(S.oth[i] * 4.0f + 0.5f) / 4.0f;   /* greyed while another is armed */
        if (i == B_OFF) {                               /* red lettering, neutral outline */
            fill = pressed ? C_SURF2 : C_BG;
            line = C_LINE2;
            ink = ui_mix(C_RED, C_DIS, ko);
        } else { fill = pressed ? C_SEC : C_SURF2; ink = pressed ? C_BG : ui_mix(C_PRI, C_DIS, ko); }
        pill(fb, cx, cy, w, fill != C_BG, ui_dim(fill, k), line ? ui_dim(line, k) : 0);
        ui_label(fb, (int)cx, by, name[i], ui_dim(ink, k), UI_CENTER);
        return;
    }

    /* growing, armed or committed: the fill gives way to a neutral outline (the red is the
     * lettering); over RESTART's partner the inside stays solid so the fading SLEEP never
     * shows through it */
    uint16_t fill = pressed && g >= 1 ? C_SURF2 : ui_dim(S.from[i], 1.0f - g);
    int solid = fill != C_BG || (i == B_RESTART && g < 1);
    uint16_t line = i == B_OFF ? C_LINE2 : ui_dim(C_LINE2, g);
    pill(fb, cx, cy, w, solid, ui_dim(fill, k), ui_dim(line, k));

    /* the labels swap on the cut's jump frame (the colour clock passes 5/8 there), so the
     * pill is never empty: the resting label fades until it, the armed one from it */
    float au = g < 0.625f ? q8(1.0f - g) : 0.0f, aa = g >= 0.625f ? q8(2.0f * g - 1.0f) : 0.0f;
    if (au > 0) {                                       /* the resting label goes first */
        uint16_t ink = i == B_OFF ? C_RED : S.from[i] == C_SEC ? C_BG : C_PRI;
        ui_label(fb, (int)cx, by, name[i], ui_dim(ui_mix(fill, ink, au), k), UI_CENTER);
    }
    if (aa <= 0) return;
    float ka = aa, kd = 0;
    if (S.done == i) {                                  /* commit: armed label out, state in */
        float c = (nos_t - S.done_t) / DONE_S;
        ka *= q8(1.0f - c);                             /* overlapped: never an empty pill */
        kd = q8(c);
    }
    if (ka > 0) {
        uint16_t red = ui_dim(ui_mix(fill, C_RED, ka), k);
        fit_label(fb, cx, ay, ARM_W, again[i], red);   /* tracking fixed as it grows */
        /* the 3 s as three 14 x 5 segments draining right to left (prog.h: the leading
         * one shortens in 8 steps a second), in neutral ink: the red stays the copy */
        float left = m_clamp(1.0f - (nos_t - S.arm_t[i]) / ARM_S, 0, 1);
        prog_bar(fb, PROG_SEGMENTS, cx - 27.0f, cy + 15.5f, 54.0f, 5.0f, 3, left,
                 ui_dim(ui_mix(fill, C_PRI, ka), k), ui_dim(ui_mix(fill, C_LINE2, ka), k));
    }
    if (kd > 0) ui_label(fb, (int)cx, by, doing[i], ui_dim(ui_mix(fill, C_RED, kd), k), UI_CENTER);
}

static void p_draw(gfx_fb *fb)
{
    ui_title(fb, S.charging ? "USB POWER" : "BATTERY", 0); /* external power present; the B cannot sense charge state */
    float rp = m_clamp((nos_t - S.t_enter) / 0.40f, 0, 1);
    dot_style st = { DOT_FONT_TALL, UI_HERO_PITCH, UI_HERO_PITCH * 0.8f, C_WHITE, 0, 1, 1 };
    dot_text_anim(fb, &st, UI_CX, HERO_Y, S.pct, 1, rp, DOT_REVEAL_RANDOM, 7u);
    /* the level in cells: the track inks in with the hero, the lit cells sweep over it */
    float kt = m_ink(m_clamp((nos_t - S.t_enter) / M_FRAMES(6), 0, 1));
    if (kt > 0)
        prog_bar(fb, PROG_SQUARES, UI_CX - CELL_SLOT * CELL_N * 0.5f, CELL_Y, CELL_SLOT * CELL_N, 12.0f,
                 CELL_N, (float)S.cells / CELL_N * cells_u(), C_PRI, ui_dim(C_LINE, kt));
    /* SLEEP first: the growing RESTART covers it */
    for (int i = 0; i < NB; i++) {
        float p = in_k(i);
        if (p <= 0) continue;
        draw_pill(fb, i, m_ink(p), floorf((1 - p) * 18.0f + 0.5f));
    }
}

/* motion audit (sim only): the pills' build-in and the armed pills' growing edge */
static void p_probe(void)
{
    static const char *const k_y[NB] = { "power.pill0.y", "power.pill1.y", "power.pill2.y" };
    for (int i = 0; i < NB; i++) nos_motion_probe(k_y[i], MC_MICRO, floorf((1 - in_k(i)) * 18.0f + 0.5f));
    nos_motion_probe("power.pill0.ink", MC_FADE, m_ink(in_k(0)) * 100.0f);
    nos_motion_probe("power.cells.fill", MC_FILL, (float)S.cells * CELL_SLOT * cells_u());
    static const char *const k_e[NB] = { 0, "power.restart.left", "power.off.left" };
    for (int i = B_RESTART; i < NB; i++) {
        float cx, cy, w;
        pill_geo(i, geo(i), &cx, &cy, &w);
        /* both are cuts: RESTART's 169 px edge is a component-scale change (its 19 px
         * lead-in is within the cut's 20), POWER OFF's 60 px a micro one */
        nos_motion_probe(k_e[i], edge_d(i) > 100.0f ? MC_COMPONENT : MC_MICRO, floorf(cx - w * 0.5f + 0.5f));
    }
}

static int btn_at(int x, int y)
{
    const float hh = UI_BTN_H * 0.5f + 6;               /* hit rows 100 px */
    if (fabsf(y - ROW1_Y) <= hh) {
        if (S.arm == B_RESTART || S.done == B_RESTART)
            return fabsf((float)(x - UI_CX)) <= ARM_W * 0.5f + 6 ? B_RESTART : -1;
        if (x >= UI_BTN_PAIR_X0 - UI_BTN_PAIR_W / 2 - 6 && x <= UI_BTN_PAIR_X1 + UI_BTN_PAIR_W / 2 + 6)
            return x < UI_CX ? B_SLEEP : B_RESTART;
        return -1;
    }
    if (fabsf(y - ROW2_Y) <= hh) {
        float w = S.arm == B_OFF || S.done == B_OFF ? ARM_W : UI_BTN_W;
        return fabsf((float)(x - UI_CX)) <= w * 0.5f + 6 ? B_OFF : -1;
    }
    return -1;
}

static int p_input(const nos_input *in)
{
    switch (in->type) {
    case IN_CANCEL: press_cancel();return 1;
    case IN_DOWN:
        press_down(S.done >= 0 ? -1 : btn_at(in->x, in->y));
        return 1;
    case IN_DRAG_START: press_cancel(); return 0;      /* a downward drag closes the sheet */
    case IN_UP: press_up(); return 1;
    case IN_TAP: {
        if (S.done >= 0) { press_cancel(); return 1; }  /* already on its way */
        int b = btn_at(in->x, in->y);
        if (b != P.id) press_cancel();
        if (b == B_SLEEP) {                             /* back to the face, then the panel off */
            press_up();
            disarm();
            nos_feedback(FB_TAP);
            nos_home();
            nos_host_power(0);
        } else if (b == B_RESTART || b == B_OFF) {
            if (S.arm == b) commit(b);
            else arm(b);
        } else disarm();
        return 1;
    }
    default:
        return 0;
    }
}

const nos_screen scr_power = { "POWER", p_enter, p_leave, p_tick, p_draw, p_input, 0 };
