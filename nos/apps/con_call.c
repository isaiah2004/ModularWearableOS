/* con_call.c — incoming / active phone call (Gadgetbridge "call" messages).
 *
 * Pushed by the services when the phone reports an incoming call. The watch has no
 * call audio: answering routes the call on the phone, so the screen says ON PHONE.
 *  - Incoming: caller name, number, a dot ring around the bezel with a bright comet
 *    lapping it, and two big round buttons: decline (a dark disc with a red X, the
 *    screen's one red) and accept (white). The services own the ringing buzz and
 *    push/pop this screen.
 *  - Active: a dot-matrix duration timer with [ ON PHONE ] under it, the ring counting
 *    seconds, and End.
 *  - Ended: End folds away into "[ CALL ENDED ]"; a call that stops ringing on the
 *    phone's side builds its buttons out in place into "[ MISSED CALL ]". Then the
 *    screen leaves by itself. */
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

#define RING_R     222.0f    /* the comet keeps clear of the name and the captions */
#define RING_N     60
#define BTN_Y      326.0f    /* the incoming pair */
#define END_Y      346.0f    /* the single End button sits lower, under [ ON PHONE ] */
#define BTN_R      44.0f     /* 88 px, the OS button size */
#define LABEL_Y    396
#define END_LABEL_Y 416
#define NAME_W     340
#define MORPH_S    0.25f     /* button rearrangement between states */
#define OUT_S      0.20f     /* dot dissolve of the buttons (missed call) */
#define BTN_DX     82.0f
#define LAP_S      1.6f      /* comet lap incl. a short rest */
#define LAP_RUN_S  1.2f
#define COMET      8
#define ENDED_S    1.4f
#define HERO_PITCH 5.0f
#define HERO_H     (9 * HERO_PITCH)
#define HOLD_S     (2.5f / NOS_FPS)   /* a quick tap shows its pressed fill >= 3 frames */

enum { CS_NONE = 0, CS_INCOMING, CS_ACTIVE, CS_ENDED };
enum { P_NONE = 0, P_DECLINE, P_ACCEPT, P_END };

static struct {
    int   st;               /* CS_* as shown */
    char  name[32], number[24];
    float t_enter, t_state;
    float build;            /* entry 0..1 */
    int   pressed;
    int   held;             /* last pressed button, shown until t_down + HOLD_S */
    float t_down;
    uint32_t t0_ms;         /* call start (hal_millis) */
    char  dur[12], dur_prev[12];
    float morph;            /* duration digit morph 0..1 */
    float t_morph;
    int   comet;            /* comet head dot index, -1 = resting */
    int   lap;              /* last lap that buzzed */
    int   popped;
    float txt;
    int   sec;              /* seconds of the current minute (active) */
    int   prev_st;          /* state before the last change (for the morph) */
    int   missed;           /* ended while still ringing (the phone side hung up) */
    int   lines;            /* caller name lines (1-2) */
    char  l1[40], l2[40];
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
    C.prev_st = C.st;
    C.st = st;
    C.t_state = nos_t;
    if (st == CS_ENDED) C.missed = C.prev_st == CS_INCOMING;
    if (st == CS_ACTIVE) {
        const svc_call_t *c = svc_call();
        C.t0_ms = c->t0_ms ? c->t0_ms : hal_millis();
        C.dur[0] = C.dur_prev[0] = 0;
    }
}

/* The caller's name, wrapped into at most two title lines of NAME_W. */
static void wrap_name(void)
{
    const char *name = C.name[0] ? C.name : C.number[0] ? C.number : "Unknown caller";
    char l1[40];
    snprintf(l1, sizeof l1, "%s", name);
    C.l2[0] = 0;
    C.lines = 1;
    if (ui_text_width(UI_F_TITLE, l1, 0) <= NAME_W) { memcpy(C.l1, l1, sizeof l1); return; }
    /* break at the last space (or after a hyphen) that fits; the rest is line 2 */
    int n = (int)strlen(l1), cut = -1;
    for (int i = 1; i < n; i++)
        if (l1[i] == ' ' || l1[i] == '-') {
            char t[40];
            int k = l1[i] == '-' ? i + 1 : i;
            memcpy(t, l1, (size_t)k); t[k] = 0;
            if (ui_text_width(UI_F_TITLE, t, 0) <= NAME_W) cut = k; else break;
        }
    if (cut < 0) { memcpy(C.l1, l1, sizeof l1); return; }
    memcpy(C.l1, l1, (size_t)cut); C.l1[cut] = 0;
    int c2 = cut;
    while (c2 > 0 && C.l1[c2 - 1] == ' ') C.l1[--c2] = 0;
    const char *r = name + cut;
    while (*r == ' ') r++;
    snprintf(C.l2, sizeof C.l2, "%s", r);
    C.lines = 2;
}

static void call_enter(void)
{
    const svc_call_t *c = svc_call();
    memset(&C, 0, sizeof C);
    /* the screen rises in; the build starts part-way so it is seen */
    C.t_enter = nos_t + 0.12f;
    C.t_state = nos_t - 10.0f;
    C.t_down = -10.0f;
    C.comet = -1;
    C.lap = -1;
    copy_caller(c);
    wrap_name();
    C.st = C.prev_st = c->state == 2 ? CS_ACTIVE : c->state == 1 ? CS_INCOMING : CS_ENDED;
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
    C.build = q((nos_t - C.t_enter) / UI_ENTRY_S, 32);
    C.txt = nos_t >= C.t_enter ? 1.0f : 0.0f;    /* words arrive with the build at full ink */
    wrap_name();

    /* follow the phone: it may answer, hang up or reject on its own */
    if (C.st == CS_INCOMING && c->state == 2) go(CS_ACTIVE);
    else if (!C.popped && (C.st == CS_INCOMING || C.st == CS_ACTIVE) && c->state == 0 && nos_t - C.t_state > 0.3f) go(CS_ENDED);

    if (C.st == CS_INCOMING) {
        float t = nos_t - C.t_enter;
        if (t < 0) t = 0;
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

/* A disc breaking into dots: a 6 px grid of dots that shrink away in a hashed order.
 * out 0 = whole, 1 = gone. One damage command over the disc. */
#define DIS_PITCH 6.0f
static void disc_dots(gfx_fb *fb, float cx, float cy, float r, uint16_t col, float out)
{
    const float dmax = DIS_PITCH * 1.1f, e = dmax * 0.5f + 1.0f;
    if (fb->rec) {
        unsigned h = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_u(0xD155u, col), cx), cy), r);
        gfx_rec_cmd(fb, (int)floorf(cx - r - e), (int)floorf(cy - r - e), (int)ceilf(cx + r + e),
                    (int)ceilf(cy + r + e), gfx_hash_f(h, out));
        return;
    }
    int lo = fb->y_base - fb->oy, hi = lo + fb->h;      /* logical rows in this stripe */
    if (cy + r + e < lo || cy - r - e > hi) return;
    int n = (int)(r / DIS_PITCH);
    float rr = (r - 1.0f) * (r - 1.0f);
    for (int j = -n; j <= n; j++) {
        float y = cy + j * DIS_PITCH;
        if (y + e < lo || y - e > hi) continue;
        for (int i = -n; i <= n; i++) {
            if ((float)(i * i + j * j) * DIS_PITCH * DIS_PITCH > rr) continue;
            float s = 0.5f * m_hash01((unsigned)(i + 16) * 37u + (unsigned)(j + 16), 0xCA11u);
            float k = q(1.0f - (out - s) / 0.5f, 4);      /* 4 sizes: the stamp cache stays small */
            if (k > 0) dot_disc(fb, cx + i * DIS_PITCH, y, dmax * k, col);
        }
    }
}

/* Accept: white disc, black glyph. Decline / End: a dark disc with the screen's one red,
 * the X. `out` > 0 builds the button out into dots (missed call). */
static void round_button(gfx_fb *fb, float cx, float cy, float r, int decline, int icon, int pressed,
                         float icon_p, float out)
{
    if (r < 1 || out >= 1) return;
    uint16_t fill = decline ? (pressed ? C_LINE2 : C_SURF2) : (pressed ? C_SEC : C_WHITE);
    uint16_t ink = decline ? C_RED : C_BG;
    if (out > 0) disc_dots(fb, cx, cy, r, fill, out);
    else ui_circle(fb, cx, cy, r, fill);
    float pitch = 4.2f * (r / BTN_R);
    float ip = out > 0 ? 1.0f - q(out * 1.6f, 8) : icon_p;
    if (r < 6) return;                     /* a folding disc: no glyph residue at the end */
    dot_icon_anim(fb, icon, cx, cy, pitch, pitch * 0.8f, ink, ip, out > 0 ? DOT_REVEAL_RANDOM : DOT_REVEAL_CENTER);
}

/* Name block: the same place in every state of the call. */
static int number_y(void) { return C.lines == 2 ? 200 : 186; }
static void draw_caller(gfx_fb *fb, uint16_t c1, uint16_t c2)
{
    int y = C.lines == 2 ? 134 : 150;
    ui_text(fb, UI_F_TITLE, UI_CX, y, C.l1, c1, UI_CENTER, 0);
    if (C.lines == 2) ui_text_fit(fb, UI_F_TITLE, UI_CX, y + 32, C.l2, c1, UI_CENTER, NAME_W);
    if (C.name[0] && C.number[0] && strcmp(C.name, C.number))
        ui_text(fb, UI_F_LABEL, UI_CX, number_y(), C.number, c2, UI_CENTER, 2);
}

static const char *title_of(int st) { return st == CS_INCOMING ? "INCOMING CALL" : "CALL"; }

static int shown_pressed(int b) { return C.pressed == b || (C.held == b && nos_t - C.t_down < HOLD_S); }

static void call_draw(gfx_fb *fb)
{
    draw_ring(fb);
    if (nos_t < C.t_enter) return;
    uint16_t c1 = ui_dim(C_PRI, C.txt), c2 = ui_dim(C_SEC, C.txt);
    /* m: 0..1 through the change from prev_st to st (rigid slides, 4-step crossfades);
     * words change in sequence: the old leave in the first half, the new arrive in the second */
    float m = bezier_ease(&EASE_STANDARD, q((nos_t - C.t_state) / MORPH_S, 16));
    float mf = q(m, 4), fout = q(1.0f - 2.0f * m, 4), fin = q(2.0f * m - 1.0f, 4);
    const char *t_new = title_of(C.st), *t_old = title_of(C.prev_st);
    if (m >= 1 || !strcmp(t_new, t_old)) ui_title(fb, t_new, 0);
    else {
        if (fout > 0) ui_label(fb, UI_CX, UI_TITLE_Y, t_old, ui_dim(C_SEC, fout), UI_CENTER);
        if (fin > 0) ui_label(fb, UI_CX, UI_TITLE_Y, t_new, ui_dim(C_SEC, fin), UI_CENTER);
    }
    int ended = C.st == CS_ENDED;
    draw_caller(fb, ended ? ui_mix(c1, C_SEC, mf) : c1, ended ? ui_mix(c2, C_DIS, mf) : c2);
    float ib = q((nos_t - C.t_enter) / UI_ENTRY_S, 32);
    int hy = number_y() + 20;                    /* the hero slot under the number */
    int sy = hy + (int)HERO_H + 22;              /* [ ON PHONE ] under the timer */
    dot_style st = { DOT_FONT_TALL, HERO_PITCH, HERO_PITCH * 0.8f, C_WHITE, 0, 1, 1 };

    switch (C.st) {
    case CS_INCOMING:
        ui_label(fb, UI_CX, hy + 22, "ANSWERS ON PHONE", ui_dim(C_DIS, C.txt), UI_CENTER);
        round_button(fb, UI_CX - BTN_DX, BTN_Y, BTN_R, 1, DOT_ICON_CROSS, shown_pressed(P_DECLINE), ib, 0);
        round_button(fb, UI_CX + BTN_DX, BTN_Y, BTN_R, 0, DOT_ICON_PHONE, shown_pressed(P_ACCEPT), ib, 0);
        ui_label(fb, (int)(UI_CX - BTN_DX), LABEL_Y, "DECLINE", c2, UI_CENTER);
        ui_label(fb, (int)(UI_CX + BTN_DX), LABEL_Y, "ACCEPT", c2, UI_CENTER);
        break;
    case CS_ACTIVE: {
        /* from incoming: decline slides to the centre and becomes End, accept folds away */
        int from_in = C.prev_st == CS_INCOMING && m < 1;
        float k = from_in ? fin : 1.0f;
        st.color = ui_dim(C_WHITE, k);
        if (C.dur_prev[0] && C.morph < 1) dot_text_morph(fb, &st, UI_CX, (float)hy, C.dur_prev, C.dur, 1, C.morph);
        else if (k > 0) dot_text_a(fb, &st, UI_CX, (float)hy, C.dur[0] ? C.dur : "00:00", 1);
        if (k > 0) ui_state(fb, UI_CX, sy, "ON PHONE", ui_dim(C_SEC, k));
        if (from_in) {
            if (fout > 0) {
                ui_label(fb, UI_CX, hy + 22, "ANSWERS ON PHONE", ui_dim(C_DIS, fout), UI_CENTER);
                ui_label(fb, (int)(UI_CX + BTN_DX), LABEL_Y, "ACCEPT", ui_dim(C_SEC, fout), UI_CENTER);
                ui_label(fb, (int)(UI_CX - BTN_DX), LABEL_Y, "DECLINE", ui_dim(C_SEC, fout), UI_CENTER);
            }
            round_button(fb, UI_CX + BTN_DX + 40 * m, BTN_Y, BTN_R * (1 - m), 0, DOT_ICON_PHONE,
                         shown_pressed(P_ACCEPT), 1, 0);
        }
        float cx = from_in ? m_lerp(UI_CX - BTN_DX, UI_CX, m) : UI_CX;
        float cy = from_in ? m_lerp(BTN_Y, END_Y, m) : END_Y;
        round_button(fb, cx, cy, BTN_R, 1, DOT_ICON_CROSS, shown_pressed(P_END), from_in ? 1 : ib, 0);
        if (k > 0) ui_label(fb, UI_CX, END_LABEL_Y, "END", ui_dim(C_SEC, k), UI_CENTER);
        break;
    }
    default:
        if (C.missed) {
            /* the phone side stopped ringing: both buttons build out where they are */
            float out = q((nos_t - C.t_state) / OUT_S, 8);
            if (fout > 0) {
                ui_label(fb, UI_CX, hy + 22, "ANSWERS ON PHONE", ui_dim(C_DIS, fout), UI_CENTER);
                ui_label(fb, (int)(UI_CX - BTN_DX), LABEL_Y, "DECLINE", ui_dim(C_SEC, fout), UI_CENTER);
                ui_label(fb, (int)(UI_CX + BTN_DX), LABEL_Y, "ACCEPT", ui_dim(C_SEC, fout), UI_CENTER);
            }
            if (fin > 0) ui_state(fb, UI_CX, hy + 22, "MISSED CALL", ui_dim(C_PRI, fin));
            round_button(fb, UI_CX - BTN_DX, BTN_Y, BTN_R, 1, DOT_ICON_CROSS, 0, 1, out > 0 ? out : 0.01f);
            round_button(fb, UI_CX + BTN_DX, BTN_Y, BTN_R, 0, DOT_ICON_PHONE, 0, 1, out > 0 ? out : 0.01f);
            break;
        }
        /* ended: the timer stays where it was, greyed; End folds away and only then does
         * the state come up in its place (the disc is drawn over it while it folds) */
        st.color = ui_mix(C_WHITE, C_SEC, mf);
        if (C.dur[0]) dot_text_a(fb, &st, UI_CX, (float)hy, C.dur, 1);
        if (fout > 0) {
            ui_state(fb, UI_CX, sy, "ON PHONE", ui_dim(C_SEC, fout));
            ui_label(fb, UI_CX, END_LABEL_Y, "END", ui_dim(C_SEC, fout), UI_CENTER);
        }
        float lab = q((nos_t - C.t_state - MORPH_S * 0.56f) / 0.15f, 4);   /* from r < 8 px */
        if (lab > 0) ui_state(fb, UI_CX, (int)END_Y + 6, "CALL ENDED", ui_dim(C_PRI, lab));
        if (m < 1) round_button(fb, UI_CX, END_Y, BTN_R * (1 - m), 1, DOT_ICON_CROSS, shown_pressed(P_END), 1, 0);
        break;
    }
}

/* ----------------------------------------------------------------- input */
static int hit(int x, int y, float cx, float cy)
{
    float dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= (BTN_R + 14) * (BTN_R + 14);
}

static int button_at(int x, int y)
{
    if (C.st == CS_INCOMING) {
        if (hit(x, y, UI_CX - BTN_DX, BTN_Y)) return P_DECLINE;
        if (hit(x, y, UI_CX + BTN_DX, BTN_Y)) return P_ACCEPT;
    } else if (C.st == CS_ACTIVE && hit(x, y, UI_CX, END_Y)) return P_END;
    return P_NONE;
}

static int call_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN:
        C.pressed = button_at(in->x, in->y);
        C.held = C.pressed;
        C.t_down = nos_t;
        return 1;
    case IN_TAP: {
        int b = button_at(in->x, in->y);
        C.pressed = P_NONE;
        if (b == P_DECLINE) {
            svc_call_reject();
            nos_feedback(FB_TAP);
            C.popped = 1;
            C.held = P_DECLINE; C.t_down = nos_t + 1.0f;   /* it stays lit while it leaves */
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
        if (in->type == IN_DRAG_START) C.held = P_NONE;
        return 1;
    default:
        return 1;
    }
}

const nos_screen scr_call = { "CALL", call_enter, call_leave, call_tick, call_draw, call_input,
                              SCR_NOBACK | SCR_KEEPAWAKE };
