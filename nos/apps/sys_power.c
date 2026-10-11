/* sys_power.c — power sheet (button long press, TX_RISE).
 *
 * Battery level as the hero (dot-matrix %, titled BATTERY or CHARGING), then the actions
 * on the section 8 grid: SLEEP and RESTART as a pair, POWER OFF on its own in the thumb
 * row. Restart and power off are arm-then-tap-again: the first tap grows the pill to 320
 * with a red outline, "TAP AGAIN TO ..." and three red dots inside that go out one per
 * second, while its row partner fades out; a second tap within 3 s commits, and the label
 * crossfades to [ RESTARTING ] / [ POWERING OFF ] before the host acts (firmware
 * nos_main.c / the sim). */
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

extern void nos_host_power(int mode);      /* 0 sleep display, 1 restart, 2 power off */

#define ROW1_Y  272.0f                     /* SLEEP | RESTART */
#define ROW2_Y  ((float)UI_BTN_Y)          /* POWER OFF */
#define HERO_Y  118.0f
#define ARM_W   320.0f                     /* an armed pill */
#define ARM_S   3.0f                       /* armed for 3 s, one dot per second */
#define GROW_S  0.15f                      /* armed pill grows, its row partner fades */
#define OTHER_S 0.15f                      /* the other pills grey / come back */
#define DONE_S  (4.0f / NOS_FPS)           /* commit: the label crossfades over 4 frames */
#define ACT_S   0.8f                       /* the pill says so before the host acts */

enum { B_SLEEP = 0, B_RESTART, B_OFF, NB };

/* Section 8 pressed state: hidden for the first 100 ms (a drag's slop passes first and
 * cancels it silently), then on until release; a quick tap still shows it for 3 frames. */
static struct { int id, down; float t0, until; } P = { -1, 0, 0, 0 };
static void press_down(int id) { P.id = id; P.down = id >= 0; P.t0 = nos_t; P.until = 0; }
static void press_cancel(void) { P.id = -1; P.down = 0; P.until = 0; }
static void press_up(void)
{
    if (!P.down) return;
    P.down = 0;
    float vis = P.t0 + 0.09f;                    /* when it became visible */
    P.until = nos_t < vis ? nos_t + 0.09f : vis + 0.10f;
}
static int press_on(int id)
{
    if (id < 0 || P.id != id) return 0;
    return P.down ? nos_t - P.t0 >= 0.09f : nos_t < P.until;
}

static struct {
    int arm;                 /* armed pill, -1 none */
    float arm_t[NB];         /* when each pill armed (its dots keep counting as it shrinks) */
    float g[NB];             /* 0..1 resting -> armed geometry, linear in time */
    float oth[NB];           /* 0..1 another pill is armed: this one greys (one red at a time) */
    uint16_t from[NB];       /* the fill the pill grows from (its pressed fill, if showing) */
    int done;                /* committed action, -1 none */
    float done_t;
    int fired;
    float t_enter;
    int bat, charging;
    char pct[8];
} S;

static float q8(float x) { return floorf(m_clamp(x, 0, 1) * 8.0f + 0.5f) / 8.0f; }
static uint16_t rest_fill(int i) { return i == B_OFF ? C_BG : C_SURF2; }
/* the armed geometry drawn now: ease-out, 8 steps (4 frames of growth at 30 Hz) */
static float geo(int i) { float t = 1.0f - m_clamp(S.g[i], 0, 1); return q8(1.0f - t * t); }

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
    for (int i = 0; i < NB; i++) { S.g[i] = 0; S.oth[i] = 0; S.arm_t[i] = 0; S.from[i] = rest_fill(i); }
    S.t_enter = nos_enter_t();
}

static void p_leave(void)
{
    if (P.down) press_cancel();
}

static void p_tick(float dt)
{
    const svc_sys_t *s = svc_sys();
    S.bat = s->battery_pct > 0 ? s->battery_pct : hal_battery_percent();
    S.charging = s->charging;
    snprintf(S.pct, sizeof S.pct, "%d%%", S.bat);
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
}

/* entry build per pill: rise 18 px and fade in, staggered by 2 frames */
static float in_k(int i)
{
    float e = (nos_t - S.t_enter - 0.08f - i * (2.0f / NOS_FPS)) / 0.24f;
    return floorf(bezier_ease(&EASE_DECEL, m_clamp(e, 0, 1)) * 16.0f + 0.5f) / 16.0f;
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
    float g = geo(i), cx, cy, w;
    pill_geo(i, g, &cx, &cy, &w);
    cy += dy;
    if (i == B_SLEEP) k *= 1.0f - geo(B_RESTART);       /* its partner is armed: give way */
    if (k <= 0) return;
    int pressed = press_on(i);
    int asc = UI_F_LABEL->ascent;
    int by = (int)(cy + asc * 0.5f - 2), ay = (int)(cy + asc * 0.5f - 10);

    if (g <= 0) {                                       /* at rest: the section 8 buttons */
        uint16_t fill = C_BG, line = 0, ink;
        float ko = floorf(S.oth[i] * 4.0f + 0.5f) / 4.0f;   /* greyed while another is armed */
        if (i == B_OFF) {
            fill = pressed ? C_SURF2 : C_BG;
            line = ui_mix(C_RED, C_LINE2, ko);
            ink = ui_mix(C_RED, C_DIS, ko);
        } else { fill = pressed ? C_SEC : C_SURF2; ink = pressed ? C_BG : ui_mix(C_PRI, C_DIS, ko); }
        pill(fb, cx, cy, w, fill != C_BG, ui_dim(fill, k), line ? ui_dim(line, k) : 0);
        ui_label(fb, (int)cx, by, name[i], ui_dim(ink, k), UI_CENTER);
        return;
    }

    /* growing, armed or committed: the fill gives way to a red outline; over RESTART's
     * partner the inside stays solid so the fading SLEEP never shows through it */
    uint16_t fill = pressed && g >= 1 ? C_SURF2 : ui_dim(S.from[i], 1.0f - g);
    int solid = fill != C_BG || (i == B_RESTART && g < 1);
    uint16_t line = i == B_OFF ? C_RED : ui_dim(C_RED, g);
    pill(fb, cx, cy, w, solid, ui_dim(fill, k), ui_dim(line, k));

    float au = q8(1.0f - 2.0f * g), aa = q8(2.0f * g - 1.0f);
    if (au > 0) {                                       /* the resting label goes first */
        uint16_t ink = i == B_OFF ? C_RED : S.from[i] == C_SEC ? C_BG : C_PRI;
        ui_label(fb, (int)cx, by, name[i], ui_dim(ui_mix(fill, ink, au), k), UI_CENTER);
    }
    if (aa <= 0) return;
    float ka = aa, kd = 0;
    if (S.done == i) {                                  /* commit: armed label out, state in */
        float c = (nos_t - S.done_t) / DONE_S;
        ka *= q8(1.0f - 2.0f * c);
        kd = q8(2.0f * c - 1.0f);
    }
    if (ka > 0) {
        uint16_t red = ui_dim(ui_mix(fill, C_RED, ka), k);
        fit_label(fb, cx, ay, ARM_W, again[i], red);   /* tracking fixed as it grows */
        int left = 3 - (int)floorf(nos_t - S.arm_t[i]);
        for (int d = 0; d < 3; d++)
            if (d < left) dot_disc(fb, cx + (d - 1) * 14.0f, cy + 18, 6, red);
    }
    if (kd > 0) ui_label(fb, (int)cx, by, doing[i], ui_dim(ui_mix(fill, C_RED, kd), k), UI_CENTER);
}

static void p_draw(gfx_fb *fb)
{
    ui_title(fb, S.charging ? "CHARGING" : "BATTERY", 0);
    float rp = m_clamp((nos_t - S.t_enter) / 0.40f, 0, 1);
    dot_style st = { DOT_FONT_TALL, UI_HERO_PITCH, UI_HERO_PITCH * 0.8f, C_WHITE, 0, 1, 1 };
    dot_text_anim(fb, &st, UI_CX, HERO_Y, S.pct, 1, rp, DOT_REVEAL_RANDOM, 7u);
    /* SLEEP first: the growing RESTART covers it */
    for (int i = 0; i < NB; i++) {
        float k = in_k(i);
        if (k <= 0) continue;
        draw_pill(fb, i, k, floorf((1 - k) * 18.0f + 0.5f));
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
