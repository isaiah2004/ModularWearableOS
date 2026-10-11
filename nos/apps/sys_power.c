/* sys_power.c — power sheet (button long press, TX_RISE).
 *
 * Battery level as the hero (dot-matrix %, with its state underneath), then three large
 * pills in the thumb zone: Sleep, Restart, Power off. Power off is the destructive one:
 * red outline; the first tap arms it (fills red, "TAP AGAIN TO POWER OFF") for 3 s and
 * the second tap commits. Restart shows [ RESTARTING ] for a moment first so the user
 * sees it was taken. The host implements the actions (firmware nos_main.c / the sim). */
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

#define BW   288.0f
#define BH   72.0f
static const float k_by[3] = { 222.0f, 306.0f, 390.0f };
static const float k_bw[3] = { BW, BW, 236.0f };

static struct {
    int pressed;            /* 1..3, 0 none */
    float armed_t;          /* power off armed at, < 0 none */
    float t_enter;
    float pending_t; int pending;   /* deferred host action */
    int bat, charging, valid;
    char pct[8];
} P;

static void p_enter(void)
{
    P.pressed = 0;
    P.armed_t = -1;
    P.t_enter = nos_t;
    P.pending = -1;
}

static void p_tick(float dt)
{
    const svc_sys_t *s = svc_sys();
    P.bat = s->battery_pct > 0 ? s->battery_pct : hal_battery_percent();
    P.charging = s->charging;
    P.valid = s->battery_valid || s->battery_pct > 0;
    snprintf(P.pct, sizeof P.pct, "%d%%", P.bat);
    if (P.armed_t >= 0 && nos_t - P.armed_t > 3.0f) P.armed_t = -1;
    if (P.pending >= 0 && nos_t >= P.pending_t) { int m = P.pending; P.pending = -1; nos_host_power(m); }
    (void)dt;
}

static float in_k(int i)
{
    float e = (nos_t - P.t_enter - 0.08f - i * (2.0f / NOS_FPS)) / 0.24f;
    return floorf(bezier_ease(&EASE_DECEL, m_clamp(e, 0, 1)) * 16.0f + 0.5f) / 16.0f;
}

static void p_draw(gfx_fb *fb)
{
    float rp = m_clamp((nos_t - P.t_enter) / 0.40f, 0, 1);
    dot_style st = { DOT_FONT_TALL, 6.0f, 4.8f, C_WHITE, 0, 1, 1 };
    dot_text_anim(fb, &st, UI_CX, 74, P.pct, 1, rp, DOT_REVEAL_RANDOM, 7u);
    ui_label(fb, UI_CX, 156, P.charging ? "CHARGING" : "BATTERY", P.charging ? C_PRI : C_SEC, UI_CENTER);
    static const char *const names[3] = { "SLEEP", "RESTART", "POWER OFF" };
    for (int i = 0; i < 3; i++) {
        float k = in_k(i);
        if (k <= 0) continue;
        float y = floorf(k_by[i] + (1 - k) * 18.0f + 0.5f);
        int pressed = P.pressed == i + 1;
        if (i == 2) {
            int armed = P.armed_t >= 0;
            ui_button(fb, UI_CX, y, k_bw[i], BH, armed ? "TAP AGAIN" : names[i], armed ? BTN_RED : BTN_DANGER, pressed);
        } else ui_button(fb, UI_CX, y, k_bw[i], BH, names[i], BTN_SECONDARY, pressed);
    }
}

static int btn_at(int x, int y)
{
    for (int i = 0; i < 3; i++)
        if (fabsf(y - k_by[i]) <= BH * 0.5f + 6 && fabsf((float)(x - UI_CX)) <= k_bw[i] * 0.5f + 6) return i + 1;
    return 0;
}

static int p_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN: P.pressed = btn_at(in->x, in->y); return 1;
    case IN_DRAG_START: P.pressed = 0; return 0;   /* a downward drag closes the sheet */
    case IN_UP: P.pressed = 0; return 1;
    case IN_TAP: {
        int b = btn_at(in->x, in->y);
        P.pressed = 0;
        if (P.pending >= 0) return 1;
        if (b == 1) {                       /* sleep: back to the face, then the panel off */
            nos_feedback(FB_TAP);
            nos_home();
            nos_host_power(0);
        } else if (b == 2) {
            nos_feedback(FB_TAP);
            nos_toast("[ Restarting ]");
            P.pending = 1; P.pending_t = nos_t + 0.8f;
        } else if (b == 3) {
            if (P.armed_t < 0) { nos_feedback(FB_LIMIT); P.armed_t = nos_t; }
            else {
                nos_feedback(FB_ERROR);
                nos_toast("[ Powering off ]");
                P.pending = 2; P.pending_t = nos_t + 0.8f;
            }
        } else P.armed_t = -1;
        return 1;
    }
    default:
        return 0;
    }
}

const nos_screen scr_power = { "POWER", p_enter, 0, p_tick, p_draw, p_input, 0 };
