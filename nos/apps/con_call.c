/* con_call.c — incoming / active phone call (Gadgetbridge "call" messages).
 *
 * Pushed by the services when the phone reports an incoming call. The watch has no
 * call audio: answering routes the call on the phone, so the screen says ON PHONE.
 *  - Incoming: caller name, number, a dot ring around the bezel with a bright comet
 *    lapping it, and two big round buttons: decline (red, the screen's one red) and
 *    accept (white). The services own the ringing buzz and push/pop this screen.
 *  - Active: a dot-matrix duration timer, the ring counting seconds, and End.
 *  - Ended: "[ CALL ENDED ]" for a moment, then the screen leaves by itself. */
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

#define RING_R     214.0f
#define RING_N     60
#define BTN_Y      330.0f
#define BTN_R      50.0f
#define BTN_DX     82.0f
#define LAP_S      1.6f      /* comet lap incl. a short rest */
#define LAP_RUN_S  1.2f
#define COMET      8
#define ENDED_S    1.4f

enum { CS_NONE = 0, CS_INCOMING, CS_ACTIVE, CS_ENDED };
enum { P_NONE = 0, P_DECLINE, P_ACCEPT, P_END };

static struct {
    int   st;               /* CS_* as shown */
    char  name[32], number[24];
    float t_enter, t_state;
    float build;            /* entry 0..1 */
    int   pressed;
    uint32_t t0_ms;         /* call start (hal_millis) */
    char  dur[12], dur_prev[12];
    float morph;            /* duration digit morph 0..1 */
    float t_morph;
    int   comet;            /* comet head dot index, -1 = resting */
    int   lap;              /* last lap that buzzed */
    int   popped;
    float txt;
    int   sec;              /* seconds of the current minute (active) */
} C;

static float q(float x, float n) { return floorf(m_clamp(x, 0, 1) * n + 0.5f) / n; }

static void copy_caller(const svc_call_t *c)
{
    if (c->name[0]) snprintf(C.name, sizeof C.name, "%s", c->name);
    if (c->number[0]) snprintf(C.number, sizeof C.number, "%s", c->number);
}

static void go(int st)
{
    if (C.st == st) return;
    C.st = st;
    C.t_state = nos_t;
    if (st == CS_ACTIVE) {
        const svc_call_t *c = svc_call();
        C.t0_ms = c->t0_ms ? c->t0_ms : hal_millis();
        C.dur[0] = C.dur_prev[0] = 0;
    }
}

static void call_enter(void)
{
    const svc_call_t *c = svc_call();
    memset(&C, 0, sizeof C);
    C.t_enter = C.t_state = nos_t;
    C.comet = -1;
    C.lap = -1;
    copy_caller(c);
    C.st = c->state == 2 ? CS_ACTIVE : c->state == 1 ? CS_INCOMING : CS_ENDED;
    if (C.st == CS_ACTIVE) { C.t0_ms = c->t0_ms ? c->t0_ms : hal_millis(); }
}

static void call_leave(void) {}

static void fmt_dur(char *out, int n, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s >= 3600) snprintf(out, (size_t)n, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    else snprintf(out, (size_t)n, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

static void call_tick(float dt)
{
    nos_keep_awake();
    const svc_call_t *c = svc_call();
    copy_caller(c);
    C.build = q((nos_t - C.t_enter) / 0.36f, 32);
    /* words fade in on entry and again when the call ends (the layout changes) */
    float tin = (nos_t - C.t_enter - 0.1f) / 0.2f, tend = C.st == CS_ENDED ? (nos_t - C.t_state) / 0.2f : 1.0f;
    C.txt = q(tin < tend ? tin : tend, 4);

    /* follow the phone: it may answer, hang up or reject on its own */
    if (C.st == CS_INCOMING && c->state == 2) go(CS_ACTIVE);
    else if ((C.st == CS_INCOMING || C.st == CS_ACTIVE) && c->state == 0 && nos_t - C.t_state > 0.3f) go(CS_ENDED);

    if (C.st == CS_INCOMING) {
        float t = nos_t - C.t_enter;
        int lap = (int)(t / LAP_S);
        float ph = t - lap * LAP_S;
        C.comet = ph < LAP_RUN_S ? (int)(ph / LAP_RUN_S * RING_N) : -1;
        C.lap = lap;
    } else C.comet = -1;

    if (C.st == CS_ACTIVE) {
        char d[12];
        uint32_t el = hal_millis() - C.t0_ms;
        C.sec = (int)(el / 1000 % 60);
        fmt_dur(d, sizeof d, el);
        if (strcmp(d, C.dur)) {
            if (C.dur[0] && strlen(C.dur) == strlen(d)) { memcpy(C.dur_prev, C.dur, sizeof C.dur); C.t_morph = nos_t; }
            else C.dur_prev[0] = 0;
            memcpy(C.dur, d, sizeof d);
        }
        C.morph = C.dur_prev[0] ? q((nos_t - C.t_morph) / 0.22f, 16) : 1.0f;
    }
    if (C.st == CS_ENDED && !C.popped && nos_t - C.t_state > ENDED_S && !nos_transitioning()) {
        C.popped = 1;
        nos_pop();
    }
}

/* ------------------------------------------------------------------ draw */
static void draw_ring(gfx_fb *fb)
{
    float b = bezier_ease(&EASE_STANDARD, C.build);
    if (C.st == CS_ACTIVE) {
        /* seconds of the current minute: past dim, now white */
        int s = C.sec;
        dot_ring(fb, UI_CX, UI_CY, RING_R, RING_N, 4.0f, 0, (float)s * b, C_DIS, C_LINE2);
        float a = 6.2831853f * (float)s / RING_N;
        dot_disc(fb, UI_CX + RING_R * gfx_fast_sin(a), UI_CY - RING_R * gfx_fast_cos(a), 9.0f, C_WHITE);
        return;
    }
    dot_ring(fb, UI_CX, UI_CY, RING_R, RING_N, 4.0f, 0, RING_N * b, C_LINE2, 0);
    if (C.comet < 0) return;
    static const float sz[COMET] = { 11, 9.5f, 8.5f, 7.5f, 6.5f, 5.5f, 5, 4.5f };
    static const uint16_t col[COMET] = { C_WHITE, C_PRI, C_PRI, C_SEC, C_SEC, C_DIS, C_DIS, C_LINE2 };
    for (int k = 0; k < COMET; k++) {
        int i = C.comet - k;
        if (i < 0) break;                       /* the tail grows in from 12 o'clock */
        float a = 6.2831853f * (float)i / RING_N;
        dot_disc(fb, UI_CX + RING_R * gfx_fast_sin(a), UI_CY - RING_R * gfx_fast_cos(a), sz[k], col[k]);
    }
}

static void round_button(gfx_fb *fb, float cx, float r, int red, int icon, int pressed, const char *label, uint16_t lc)
{
    if (r < 1) return;
    uint16_t fill = red ? (pressed ? C_RED_D : C_RED) : (pressed ? C_SEC : C_WHITE);
    ui_circle(fb, cx, BTN_Y, r, fill);
    float pitch = 4.6f * (r / BTN_R);
    dot_icon(fb, icon, cx, BTN_Y, pitch, pitch * 0.8f, red ? C_WHITE : C_BG);
    ui_label(fb, (int)cx, 410, label, lc, UI_CENTER);
}

static void draw_caller(gfx_fb *fb, int y_name, uint16_t c1, uint16_t c2)
{
    const char *name = C.name[0] ? C.name : C.number[0] ? C.number : "Unknown caller";
    int x0, x1;
    ui_row_span((float)y_name, 40, &x0, &x1);
    ui_text_fit(fb, UI_F_TITLE, UI_CX, y_name, name, c1, UI_CENTER, x1 - x0);
    if (C.name[0] && C.number[0])
        ui_text(fb, UI_F_LABEL, UI_CX, y_name + 36, C.number, c2, UI_CENTER, 2);
}

static void call_draw(gfx_fb *fb)
{
    draw_ring(fb);
    uint16_t c1 = ui_dim(C_PRI, C.txt), c2 = ui_dim(C_SEC, C.txt), c3 = ui_dim(C_DIS, C.txt);
    float b = bezier_ease(&EASE_STANDARD, C.build);
    switch (C.st) {
    case CS_INCOMING: {
        ui_title(fb, "INCOMING CALL", 0);
        draw_caller(fb, 180, c1, c2);
        ui_label(fb, UI_CX, 258, "ON PHONE", c3, UI_CENTER);
        /* buttons grow in, decline two frames ahead of accept */
        float bd = bezier_ease(&EASE_STANDARD, q((nos_t - C.t_enter) / 0.26f, 32));
        float ba = bezier_ease(&EASE_STANDARD, q((nos_t - C.t_enter - 0.066f) / 0.26f, 32));
        round_button(fb, UI_CX - BTN_DX, BTN_R * bd, 1, DOT_ICON_CROSS, C.pressed == P_DECLINE, "DECLINE", c2);
        round_button(fb, UI_CX + BTN_DX, BTN_R * ba, 0, DOT_ICON_PHONE, C.pressed == P_ACCEPT, "ACCEPT", c2);
        break;
    }
    case CS_ACTIVE: {
        ui_title(fb, "ON PHONE", 0);
        draw_caller(fb, 128, c1, c2);
        dot_style st = { DOT_FONT_TALL, 7.0f, 5.6f, C_WHITE, 0, 1, 1 };
        if (C.dur_prev[0] && C.morph < 1) dot_text_morph(fb, &st, UI_CX, 194, C.dur_prev, C.dur, 1, C.morph);
        else dot_text_a(fb, &st, UI_CX, 194, C.dur, 1);
        round_button(fb, UI_CX, BTN_R * b, 1, DOT_ICON_CROSS, C.pressed == P_END, "END", c2);
        break;
    }
    default: {
        ui_title(fb, "CALL", 0);
        draw_caller(fb, 190, ui_dim(C_SEC, C.txt), ui_dim(C_DIS, C.txt));
        ui_state(fb, UI_CX, 290, "CALL ENDED", c1);
        if (C.dur[0]) ui_text(fb, UI_F_LABEL, UI_CX, 326, C.dur, c2, UI_CENTER, 2);
        break;
    }
    }
}

/* ----------------------------------------------------------------- input */
static int hit(int x, int y, float cx)
{
    float dx = x - cx, dy = y - BTN_Y;
    return dx * dx + dy * dy <= (BTN_R + 14) * (BTN_R + 14);
}

static int button_at(int x, int y)
{
    if (C.st == CS_INCOMING) {
        if (hit(x, y, UI_CX - BTN_DX)) return P_DECLINE;
        if (hit(x, y, UI_CX + BTN_DX)) return P_ACCEPT;
    } else if (C.st == CS_ACTIVE && hit(x, y, UI_CX)) return P_END;
    return P_NONE;
}

static int call_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN:
        C.pressed = button_at(in->x, in->y);
        return 1;
    case IN_TAP: {
        int b = button_at(in->x, in->y);
        C.pressed = P_NONE;
        if (b == P_DECLINE) {
            svc_call_reject();
            nos_feedback(FB_TAP);
            C.popped = 1;
            nos_pop();
        } else if (b == P_ACCEPT) {
            svc_call_accept();
            nos_feedback(FB_SUCCESS);
            go(CS_ACTIVE);
        } else if (b == P_END) {
            svc_call_hangup();
            nos_feedback(FB_TAP);
            go(CS_ENDED);
        } else if (C.st == CS_ENDED && !C.popped) {
            C.popped = 1;
            nos_pop();
        }
        return 1;
    }
    case IN_BTN:
        if (C.st == CS_INCOMING) { svc_ring_silence(); nos_feedback(FB_TAP); return 1; }   /* like a phone: button silences */
        return 0;
    case IN_UP:
    case IN_DRAG_START:
        C.pressed = P_NONE;
        return 1;
    default:
        return 1;
    }
}

const nos_screen scr_call = { "CALL", call_enter, call_leave, call_tick, call_draw, call_input,
                              SCR_NOBACK | SCR_KEEPAWAKE };
