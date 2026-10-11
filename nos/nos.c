/* nos.c — NOS engine: frame loop, screen stack, gestures, transitions, damage. See nos.h. */
#include "nos.h"
#include "motion.h"
#include "prefs.h"
#include "hal_ext.h"
#include "ui.h"
#include "gfx_cost.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

uint32_t nos_frame;
float    nos_t;
float    nos_dt = 1.0f / NOS_FPS;
nos_stats nos_last;
int nos_iris_x = GFX_W / 2, nos_iris_y = GFX_H / 2;
int nos_debug_force_full;
void (*nos_bg_tick)(float dt);
int nos_aod;

/* ------------------------------------------------------------------ stack */
#define MAX_DEPTH 8
typedef struct { const nos_screen *s; int tx; } entry;
static entry s_stack[MAX_DEPTH];
static int s_depth;

/* ------------------------------------------------------------- transition
 * Geometry is always (under, over, p): p = 1 shows `over` fully. Pushing animates
 * p 0 -> 1; popping animates 1 -> 0, then drops the over screen. */
enum { TXS_IDLE = 0, TXS_AUTO, TXS_FINGER };
static struct {
    int state;
    int type;
    const nos_screen *under, *over;
    int popping;            /* commit at p = 0 removes `over` */
    int replacing;          /* push replaces the screen below once complete */
    mspring sp;             /* p in px units: q = p * span */
    mtween tw;              /* curve-driven types (iris, dissolve) */
    int use_tween;
    float span;
    float p;
    /* finger-driven */
    int axis, sign;         /* finger travel that increases p: sign * d(axis) */
    float p0;
    int cancel_pops;        /* finger pull-in: cancelling pops the pulled screen */
    float lin_v;            /* > 0: leaving at constant speed (p per second) toward p = 0 */
} T;
static int s_by_finger;                     /* inside enter(): pulled in by a finger */
int nos_entered_by_finger(void) { return s_by_finger; }
const nos_screen *nos_power_screen;         /* set by the app registry: long-press target */

static int s_force_full = 1;
void nos_invalidate(void) { s_force_full = 1; }

static const nos_screen *top_screen(void) { return s_depth ? s_stack[s_depth - 1].s : 0; }
const nos_screen *nos_top(void) { return top_screen(); }
int nos_depth(void) { return s_depth; }
int nos_transitioning(void) { return T.state != TXS_IDLE; }

static int s_revealing;
int nos_revealed(void) { return s_revealing; }
static void call_enter(const nos_screen *s) { if (s && s->enter) s->enter(); }
static void call_reveal(const nos_screen *s) { s_revealing = 1; call_enter(s); s_revealing = 0; }
static void call_leave(const nos_screen *s) { if (s && s->leave) s->leave(); }

static float tx_span(int type)
{
    return (type == TX_RISE || type == TX_DROP) ? (float)GFX_H : (float)GFX_W;
}

static void tx_begin(int type, const nos_screen *under, const nos_screen *over, float p_from, float p_to)
{
    T.type = type;
    T.under = under;
    T.over = over;
    T.span = tx_span(type);
    T.p = p_from;
    T.use_tween = (type == TX_IRIS || type == TX_DISSOLVE);
    if (T.use_tween) {
        float dur = type == TX_IRIS ? 0.34f : 0.30f;
        mtw_start(&T.tw, p_from, p_to, dur, type == TX_IRIS ? &EASE_STANDARD : &EASE_INOUT);
    } else {
        msp_init(&T.sp, p_from * T.span, SPRING_PAGE);
        msp_to(&T.sp, p_to * T.span);
    }
    /* which finger travel grows p, so a moving transition can be grabbed */
    T.axis = (type == TX_RISE || type == TX_DROP) ? AXIS_Y : AXIS_X;
    T.sign = type == TX_DROP ? 1 : -1;
    T.lin_v = 0;
    T.state = TXS_AUTO;
    s_force_full = 1;
}

void nos_set_root(const nos_screen *s)
{
    while (s_depth > 0) { call_leave(s_stack[s_depth - 1].s); s_depth--; }
    s_stack[0].s = s; s_stack[0].tx = TX_NONE; s_depth = 1;
    T.state = TXS_IDLE;
    call_enter(s);
    s_force_full = 1;
}

void nos_push(const nos_screen *s, int tx)
{
    if (!s || s_depth >= MAX_DEPTH) return;
    if (T.state != TXS_IDLE) {               /* finish the running one instantly */
        if (T.popping && s_depth > 1) { call_leave(s_stack[s_depth - 1].s); s_depth--; call_reveal(top_screen()); }
        T.state = TXS_IDLE;
    }
    const nos_screen *under = top_screen();
    call_leave(under);
    s_stack[s_depth].s = s; s_stack[s_depth].tx = tx; s_depth++;
    call_enter(s);
    T.popping = 0; T.replacing = 0; T.cancel_pops = 0;
    if (tx == TX_NONE) { s_force_full = 1; return; }
    tx_begin(tx, under, s, 0.0f, 1.0f);
}

void nos_replace(const nos_screen *s, int tx)
{
    if (s_depth <= 1) { nos_set_root(s); return; }
    T.state = TXS_IDLE;
    const nos_screen *old = top_screen();
    call_leave(old);
    s_stack[s_depth - 1].s = s;
    s_stack[s_depth - 1].tx = tx;
    call_enter(s);
    T.popping = 0; T.replacing = 0; T.cancel_pops = 0;
    if (tx == TX_NONE) { s_force_full = 1; return; }
    tx_begin(tx == TX_PUSH ? TX_DISSOLVE : tx, old, s, 0.0f, 1.0f);
}

void nos_pop(void)
{
    if (s_depth <= 1) return;
    if (T.state != TXS_IDLE && T.popping) return;     /* already leaving */
    const nos_screen *over = top_screen();
    const nos_screen *under = s_stack[s_depth - 2].s;
    int tx = s_stack[s_depth - 1].tx;
    T.popping = 1; T.replacing = 0; T.cancel_pops = 0;
    if (tx == TX_NONE) {
        call_leave(over); s_depth--; call_reveal(under); s_force_full = 1; return;
    }
    float from = T.state != TXS_IDLE ? T.p : 1.0f;
    tx_begin(tx, under, over, from, 0.0f);
}

void nos_pop_with(int tx)
{
    if (s_depth > 1) s_stack[s_depth - 1].tx = tx;
    nos_pop();
}

void nos_home(void)
{
    if (s_depth <= 1) return;
    /* Collapse the middle of the stack silently, then animate the top away. */
    const nos_screen *top = top_screen();
    int tx = s_stack[s_depth - 1].tx;
    T.state = TXS_IDLE;
    call_leave(top);
    while (s_depth > 1) s_depth--;
    s_stack[1].s = top; s_stack[1].tx = tx == TX_NONE ? TX_DISSOLVE : tx; s_depth = 2;
    if (tx == TX_IRIS) { nos_iris_x = GFX_W; nos_iris_y = GFX_H / 2; }   /* close toward the button */
    nos_pop();
}

void nos_home_now(void)
{
    T.state = TXS_IDLE; T.popping = 0;
    while (s_depth > 1) { call_leave(s_stack[s_depth - 1].s); s_depth--; }
    call_reveal(top_screen());
    s_force_full = 1;
}

static void P_rebase(void);
/* Finger-driven transition. axis/sign say which finger travel grows p. */
static void finger_begin(int type, const nos_screen *under, const nos_screen *over,
                         float p0, int popping, int axis, int sign)
{
    T.type = type == TX_IRIS || type == TX_DISSOLVE ? TX_PUSH : type;   /* fingers move rigid geometry */
    T.under = under; T.over = over;
    T.span = tx_span(T.type);
    T.p = T.p0 = p0;
    T.use_tween = 0;
    msp_init(&T.sp, p0 * T.span, SPRING_SHEET);
    T.popping = popping;
    T.axis = axis; T.sign = sign;
    T.lin_v = 0;
    T.state = TXS_FINGER;
    P_rebase();                     /* measure from here: the slop must not jump the screen */
    s_force_full = 1;
}

void nos_pull(const nos_screen *s, int tx, const nos_input *start)
{
    if (!s || s_depth >= MAX_DEPTH || T.state != TXS_IDLE) return;
    const nos_screen *under = top_screen();
    call_leave(under);
    s_stack[s_depth].s = s; s_stack[s_depth].tx = tx; s_depth++;
    s_by_finger = 1; call_enter(s); s_by_finger = 0;
    int axis = (tx == TX_RISE || tx == TX_DROP) ? AXIS_Y : AXIS_X;
    int sign = tx == TX_RISE ? -1 : tx == TX_DROP ? 1 : -1;  /* rise: finger up; push: finger left */
    finger_begin(tx, under, s, 0.0f, 0, axis, sign);
    T.cancel_pops = 1;
    (void)start;
}

/* ------------------------------------------------------------- overlays */
static struct {
    int active;
    char title[40], body[96];
    int icon;
    void (*on_tap)(void);
    mspring y;            /* 0 = hidden above, 1 = shown (px units of its height) */
    float shown_t;
    int dragging;
} B;
#define BANNER_H 190.0f      /* travel: card top 52 + height 136 + margin */
#define BANNER_SHOW_S 4.0f

void nos_banner(const char *title, const char *body, int icon, void (*on_tap)(void))
{
    snprintf(B.title, sizeof B.title, "%s", title ? title : "");
    snprintf(B.body, sizeof B.body, "%s", body ? body : "");
    B.icon = icon;
    B.on_tap = on_tap;
    if (!B.active) msp_init(&B.y, 0, SPRING_SHEET);
    B.active = 1;
    msp_to(&B.y, BANNER_H);
    B.shown_t = nos_t;
    if (!nos_prefs.dnd) nos_feedback(FB_NOTIFY);
    nos_keep_awake();
}

static struct { char text[40]; float t0; int active; } TO;
void nos_toast(const char *text)
{
    snprintf(TO.text, sizeof TO.text, "%s", text ? text : "");
    TO.t0 = nos_t;
    TO.active = 1;
}
#define TOAST_S 1.4f

/* ------------------------------------------------------------- feedback */
void nos_feedback(int kind)
{
    static const signed char hap[] = {
        [FB_TICK] = HAPTIC_TICK, [FB_TAP] = HAPTIC_CLICK, [FB_TOGGLE_ON] = HAPTIC_DOUBLE,
        [FB_TOGGLE_OFF] = HAPTIC_CLICK, [FB_BACK] = HAPTIC_TICK, [FB_OPEN] = HAPTIC_CLICK,
        [FB_SUCCESS] = HAPTIC_DOUBLE, [FB_ERROR] = HAPTIC_HEAVY, [FB_LIMIT] = HAPTIC_TICK,
        [FB_NOTIFY] = HAPTIC_BUZZ };
    static const signed char sfx[] = {
        [FB_TICK] = SFX_TICK, [FB_TAP] = SFX_TAP, [FB_TOGGLE_ON] = SFX_TOGGLE_ON,
        [FB_TOGGLE_OFF] = SFX_TOGGLE_OFF, [FB_BACK] = SFX_BACK, [FB_OPEN] = SFX_OPEN,
        [FB_SUCCESS] = SFX_SUCCESS, [FB_ERROR] = SFX_ERROR, [FB_LIMIT] = -1,
        [FB_NOTIFY] = SFX_NOTIFY };
    if (kind < 0 || kind > FB_NOTIFY) return;
    if (nos_prefs.haptics) hal_haptic(hap[kind]);
    /* ticks are felt, not heard: a sound per detent is noise */
    if (nos_prefs.sound && sfx[kind] >= 0 && kind != FB_TICK && kind != FB_LIMIT) hal_sfx(sfx[kind]);
}

int nos_bright_override = -1;
void nos_brightness_override(int level)
{
    nos_bright_override = level < 0 ? -1 : level > 255 ? 255 : level;
    /* the sim has no per-frame brightness host: apply immediately there too */
    hal_set_backlight((uint8_t)(level >= 0 ? nos_bright_override : nos_prefs.brightness * 255 / 100));
}

/* --------------------------------------------------------------- awake */
static uint32_t s_last_input_ms;
void nos_keep_awake(void) { s_last_input_ms = hal_millis(); }
int nos_awake_ms(void) { return (int)(hal_millis() - s_last_input_ms); }

/* --------------------------------------------------------------- input */
#define SLOP        12
#define LONG_S      0.45f
#define DOUBLE_MS   320
#define BTN_LONG_MS 600
#define EDGE_ZONE   44
#define VSAMPLES    8

static struct {
    int down, x, y, x0, y0;
    int dragging, axis;
    int long_fired;
    float t_down;
    int owner;               /* 0 screen, 1 engine back-swipe/pull, 2 banner */
    int sx[VSAMPLES], sy[VSAMPLES];
    uint32_t st[VSAMPLES];
    int ns;
} P;

static int s_raw_x, s_raw_y, s_raw_down, s_raw_dirty;
void nos_touch(int x, int y, int down)
{
    s_raw_x = x; s_raw_y = y; s_raw_down = down; s_raw_dirty = 1;
    /* sample velocity at the source rate, not the frame rate */
    if (down) {
        int i = P.ns % VSAMPLES;
        P.sx[i] = x; P.sy[i] = y; P.st[i] = hal_millis(); P.ns++;
    }
}

static void velocity(float *vx, float *vy)
{
    /* least-squares slope over samples within 100 ms of the newest */
    *vx = *vy = 0;
    int n = P.ns < VSAMPLES ? P.ns : VSAMPLES;
    if (n < 2) return;
    int newest = (P.ns - 1) % VSAMPLES;
    uint32_t tn = P.st[newest];
    float st = 0, sx = 0, sy = 0, stt = 0, stx = 0, sty = 0; int k = 0;
    for (int j = 0; j < n; j++) {
        int i = (P.ns - 1 - j) % VSAMPLES;
        float t = -(float)(tn - P.st[i]) / 1000.0f;
        if (t < -0.10f) break;
        st += t; sx += P.sx[i]; sy += P.sy[i]; stt += t * t; stx += t * P.sx[i]; sty += t * P.sy[i]; k++;
    }
    if (k < 2) return;
    float den = k * stt - st * st;
    if (den < 1e-9f) return;
    *vx = (k * stx - st * sx) / den;
    *vy = (k * sty - st * sy) / den;
    const float VMAX = 6000.0f;
    *vx = m_clamp(*vx, -VMAX, VMAX);
    *vy = m_clamp(*vy, -VMAX, VMAX);
}

static int send(nos_in_type type)
{
    const nos_screen *s = top_screen();
    nos_input in;
    in.type = type; in.x = P.x; in.y = P.y; in.x0 = P.x0; in.y0 = P.y0;
    in.dx = (float)(P.x - P.x0); in.dy = (float)(P.y - P.y0);
    velocity(&in.vx, &in.vy);
    in.axis = P.axis;
    return s && s->input ? s->input(&in) : 0;
}

static int near_left_edge(int x, int y)
{
    float dx = x - GFX_CX, dy = y - GFX_CY;
    float r = sqrtf(dx * dx + dy * dy);
    return x < GFX_CX - 60 && r > GFX_W / 2.0f - EDGE_ZONE;
}

static int in_banner(int x, int y)
{
    float top = UI_BANNER_TOP + (B.y.x - BANNER_H);
    return B.active && B.y.x > 40 && y >= top && y < top + 136 && x > 83 && x < GFX_W - 83;
}

static void finger_update(void)
{
    float d = T.axis == AXIS_X ? (float)(P.x - P.x0) : (float)(P.y - P.y0);
    float p = T.p0 + T.sign * d / T.span;
    if (p > 1) p = 1 + (p - 1) * 0.15f;            /* resist past the end */
    if (p < 0) p = 0;
    T.p = p;
    T.sp.x = p * T.span;
}

static void finger_release(void)
{
    float vx, vy;
    velocity(&vx, &vy);
    float v = T.sign * (T.axis == AXIS_X ? vx : vy) / T.span;   /* p per second */
    float proj = T.p + v * 0.18f;                               /* intent: where it was heading */
    int commit = proj > 0.5f;                                   /* commit = end at p 1 */
    T.state = TXS_AUTO;
    T.use_tween = 0;
    msp_init(&T.sp, T.p * T.span, SPRING_PAGE);
    T.sp.v = v * T.span;
    msp_to(&T.sp, commit ? T.span : 0.0f);
    T.lin_v = 0;
    if (!commit) {
        /* leaving the glass: keep the finger's speed (never brake an exit), at least
         * a full span in 0.22 s */
        float sp = -v;
        if (sp < 1.0f / 0.22f) sp = 1.0f / 0.22f;
        T.lin_v = sp;
    }
    if (T.popping) {
        /* back-swipe: p 1 = stay; reaching 0 pops */
        if (!commit) nos_feedback(FB_BACK);
    } else if (commit) nos_feedback(FB_OPEN);
    else T.popping = 1;               /* a pull or grabbed push let go: the over screen leaves */
}


static void tx_finish(void);
static void P_rebase(void) { P.x0 = P.x; P.y0 = P.y; }

static void press_begin(int x, int y)
{
    P.down = 1; P.x = P.x0 = x; P.y = P.y0 = y;
    P.dragging = 0; P.axis = AXIS_NONE; P.long_fired = 0; P.owner = 0;
    P.t_down = nos_t;
    P.sx[0] = x; P.sy[0] = y; P.st[0] = hal_millis(); P.ns = 1;
    nos_keep_awake();
    if (in_banner(x, y)) { P.owner = 2; B.dragging = 1; return; }
    if (T.state == TXS_AUTO && !T.use_tween && !T.lin_v &&
        fabsf(T.sp.v) > 150.0f && fabsf(T.sp.x - T.sp.target) > 6.0f) {
        /* grab a moving transition mid-flight: the finger takes over from the spring */
        T.state = TXS_FINGER;
        T.p0 = T.p;
        P.owner = 1;
        return;
    }
    if (T.state == TXS_AUTO) tx_finish();          /* nearly settled / tween: land it, tap through */
    send(IN_DOWN);
}

static void drag_decide(void)
{
    int adx = abs(P.x - P.x0), ady = abs(P.y - P.y0);
    P.axis = adx >= ady ? AXIS_X : AXIS_Y;
    P.dragging = 1;
    const nos_screen *s = top_screen();
    uint32_t fl = s ? s->flags : 0;
    /* system back gesture: rightward drag on any pushed screen */
    int top_tx = s_depth > 1 ? s_stack[s_depth - 1].tx : TX_NONE;
    int sheet = top_tx == TX_RISE || top_tx == TX_DROP;   /* sheets close along their own axis */
    if (P.axis == AXIS_X && P.x > P.x0 && s_depth > 1 && !(fl & SCR_NOBACK) && !sheet &&
        (!(fl & SCR_HDRAG) || near_left_edge(P.x0, P.y0))) {
        finger_begin(TX_PUSH, s_stack[s_depth - 2].s, s, 1.0f, 1, AXIS_X, -1);
        P.owner = 1;
        send(IN_UP);              /* the screen's press is cancelled */
        return;
    }
    /* sheets close with the drag that opened them, reversed, once the sheet's own
     * content declines the drag (e.g. its list is scrolled to the end) */
    if (s_depth > 1 && !(fl & SCR_NOBACK)) {
        int tx = s_stack[s_depth - 1].tx;
        int closing = (tx == TX_RISE && P.axis == AXIS_Y && P.y > P.y0) ||
                      (tx == TX_DROP && P.axis == AXIS_Y && P.y < P.y0);
        if (closing) {
            if (send(IN_DRAG_START)) return;
            finger_begin(tx, s_stack[s_depth - 2].s, s, 1.0f, 1, AXIS_Y, tx == TX_RISE ? -1 : 1);
            P.owner = 1;
            return;
        }
    }
    send(IN_DRAG_START);
    if (T.state == TXS_FINGER) P.owner = 1;   /* the screen called nos_pull() */
}

void nos_sheet_take(void)
{
    if (!P.down || P.owner != 0 || s_depth <= 1 || T.state != TXS_IDLE) return;
    int tx = s_stack[s_depth - 1].tx;
    if (tx != TX_RISE && tx != TX_DROP) return;
    finger_begin(tx, s_stack[s_depth - 2].s, top_screen(), 1.0f, 1, AXIS_Y, tx == TX_RISE ? -1 : 1);
    P.owner = 1;
}

static void process_touch(void)
{
    if (!s_raw_dirty) return;
    s_raw_dirty = 0;
    int x = s_raw_x, y = s_raw_y;
    if (s_raw_down && !P.down) { press_begin(x, y); return; }
    if (!P.down) return;
    if (s_raw_down) {
        P.x = x; P.y = y;
        nos_keep_awake();
        if (P.owner == 2 || P.owner == 3) return;
        if (P.owner == 1) { finger_update(); return; }
        if (!P.dragging) {
            if (!P.long_fired && (abs(x - P.x0) > SLOP || abs(y - P.y0) > SLOP)) drag_decide();
            return;
        }
        send(IN_DRAG);
        return;
    }
    /* release */
    P.down = 0;
    if (P.owner == 2) {
        B.dragging = 0;
        float vx, vy; velocity(&vx, &vy);
        if (P.y - P.y0 < -24 || vy < -500) msp_kick(&B.y, 0, vy);           /* flick away */
        else if (P.y - P.y0 > 60 || (abs(P.x - P.x0) < SLOP && abs(P.y - P.y0) < SLOP)) {
            /* a tap, or pulling the banner down, opens it */
            msp_to(&B.y, 0);
            nos_feedback(FB_TAP);
            if (B.on_tap) B.on_tap();
        } else msp_to(&B.y, BANNER_H);
        return;
    }
    if (P.owner == 1) { finger_release(); return; }
    if (P.owner == 3) return;
    if (P.dragging) send(IN_DRAG_END);
    else if (!P.long_fired) send(IN_TAP);
    send(IN_UP);
}

static void process_long(void)
{
    if (P.down && !P.dragging && !P.long_fired && P.owner == 0 && nos_t - P.t_down >= LONG_S) {
        P.long_fired = 1;
        if (send(IN_LONG)) nos_feedback(FB_TAP);
    }
}

/* Side button: the top screen sees IN_BTN* first. Unhandled presses go home. */
static int s_btn_raw, s_btn_down;
static uint32_t s_btn_t0, s_btn_last_up;
static int s_btn_long_sent, s_btn_pending;
void nos_button(int down) { s_btn_raw = down; }

static void dispatch_btn(nos_in_type t)
{
    nos_keep_awake();
    nos_input in; memset(&in, 0, sizeof in); in.type = t;
    if (P.down && P.owner == 1) P.owner = 3;     /* a finger mid-sheet goes inert until release */
    const nos_screen *s = top_screen();
    if (s && s->input && s->input(&in)) return;
    if (t == IN_BTN && s_depth > 1) { nos_feedback(FB_BACK); nos_home(); }
    else if (t == IN_BTN_LONG && nos_power_screen && s != nos_power_screen) {
        nos_feedback(FB_OPEN);
        nos_push(nos_power_screen, TX_RISE);
    }
}

static void process_button(void)
{
    uint32_t now = hal_millis();
    if (s_btn_raw && !s_btn_down) { s_btn_down = 1; s_btn_t0 = now; s_btn_long_sent = 0; }
    else if (!s_btn_raw && s_btn_down) {
        s_btn_down = 0;
        if (!s_btn_long_sent) {
            /* act at once on every press; a quick second press adds IN_BTN_DOUBLE to
             * whatever is on top by then (the launcher opens the last app) */
            if (s_btn_pending && now - s_btn_last_up < DOUBLE_MS) { s_btn_pending = 0; dispatch_btn(IN_BTN_DOUBLE); }
            else { s_btn_pending = 1; s_btn_last_up = now; dispatch_btn(IN_BTN); }
        }
    }
    if (s_btn_down && !s_btn_long_sent && now - s_btn_t0 >= BTN_LONG_MS) {
        s_btn_long_sent = 1; s_btn_pending = 0;
        dispatch_btn(IN_BTN_LONG);
    }
    if (s_btn_pending && !s_btn_down && now - s_btn_last_up >= DOUBLE_MS) s_btn_pending = 0;
}

static int s_air;
void nos_air(int g) { s_air = g; }

/* --------------------------------------------------------------- render
 * The scene: (under, over) during a transition, else the top screen; then the
 * engine overlays. Each screen draws into a sub-buffer clipped to the part of the
 * display it owns, translated by its offset. */
static uint16_t *s_full;                 /* sim full frame */
static uint16_t *s_stripe[2];
static int s_stripe_h;
static void (*s_wait_free)(void);
static void (*s_flush)(uint16_t *, int, int, int, int, int);
static uint16_t *s_scratch;

void nos_set_stripes(uint16_t *a, uint16_t *b, int rows, void (*wait_free)(void),
                     void (*flush)(uint16_t *, int, int, int, int, int))
{
    s_stripe[0] = a; s_stripe[1] = b; s_stripe_h = rows; s_wait_free = wait_free; s_flush = flush;
}
void nos_set_scratch(uint16_t *buf) { s_scratch = buf; }

/* Dim: keep k/32 of every channel. One multiply per lit pixel. Truncating, then
 * dropping green's low bit, keeps neutral greys neutral (g6 = 2 * r5 stays exact)
 * without a per-pixel branch; the lost bit is invisible on a dimmed backdrop. */
static void dim_rows(gfx_fb *fb, int c0, int c1, int k)
{
    if (fb->rec || k >= 32 || c1 <= c0) return;
    for (int r = 0; r < fb->h; r++) {
        uint16_t *p = gfx_row(fb, r);
        if (k <= 0) { memset(p + c0, 0, (size_t)(c1 - c0) * 2); continue; }
        for (int x = c0; x < c1; x++) {
            uint32_t c = p[x];
            if (!c) continue;
            uint32_t d = (c | (c << 16)) & 0x07E0F81Fu;
            d = ((d * (uint32_t)k) >> 5) & 0x07C0F81Fu;
            p[x] = (uint16_t)(d | (d >> 16));
        }
    }
    GFX_COST_PX(GFX_COST_DIM, fb->h * (c1 - c0));   /* sim cost model only */
}

/* Sub-buffer of `fb` covering screen rows [y0, y1) and columns [x0, x1), with the
 * screen's content translated by (ox, oy). Returns 0 if empty. */
static int sub_fb(const gfx_fb *fb, gfx_fb *out, int x0, int y0, int x1, int y1, int ox, int oy)
{
    *out = *fb;
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (x0 > c0) c0 = x0;
    if (x1 < c1) c1 = x1;
    if (c1 <= c0) return 0;
    out->cx0 = c0; out->cx1 = c1;
    out->ox = fb->ox + ox; out->oy = fb->oy + oy;
    if (fb->rec) return 1;
    int r0 = y0 - fb->y_base, r1 = y1 - fb->y_base;
    if (r0 < 0) r0 = 0;
    if (r1 > fb->h) r1 = fb->h;
    if (r1 <= r0) return 0;
    out->px = gfx_row(fb, r0) + fb->x_base;    /* gfx_row subtracts x_base */
    out->y_base = fb->y_base + r0;
    out->h = r1 - r0;
    return 1;
}

void nos_dim(gfx_fb *fb, float k)
{
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    dim_rows(fb, c0, c1, (int)lroundf(m_clamp(k, 0, 1) * 32.0f));
}

int nos_subview(const gfx_fb *fb, gfx_fb *out, int x0, int y0, int x1, int y1, int ox, int oy)
{
    return sub_fb(fb, out, x0, y0, x1, y1, ox, oy);
}

static void clear_sub(gfx_fb *fb)
{
    if (fb->rec) return;
    for (int r = 0; r < fb->h; r++)
        memset(gfx_row(fb, r) + fb->cx0, 0, (size_t)(fb->cx1 - fb->cx0) * 2);
    GFX_COST_PX(GFX_COST_FILL, fb->h * (fb->cx1 - fb->cx0));   /* sim cost model only */
}

/* sim check: dim by post-processing pixels (the old path) instead of ink, to prove
 * every writer honours gfx_fb.dim (`set postdim 1`, compare frames) */
int nos_debug_post_dim;
static void draw_screen(const gfx_fb *fb, const nos_screen *s, int x0, int y0, int x1, int y1,
                        int ox, int oy, int dimk)
{
    gfx_fb sub;
    if (!s || !sub_fb(fb, &sub, x0, y0, x1, y1, ox, oy)) return;
    if (!(s->flags & SCR_OPAQUE_BG)) clear_sub(&sub);
    /* a dimmed backdrop scales its inks as it draws: no per-pixel pass */
    if (dimk < 32 && !nos_debug_post_dim) sub.dim = 32 - (dimk < 0 ? 0 : dimk);
    if (s->draw) s->draw(&sub);
    if (dimk < 32 && nos_debug_post_dim) dim_rows(&sub, sub.cx0, sub.cx1, dimk);
}

/* 8x8 Bayer thresholds 0..63 */
static const uint8_t BAYER8[8][8] = {
    { 0,32, 8,40, 2,34,10,42}, {48,16,56,24,50,18,58,26}, {12,44, 4,36,14,46, 6,38},
    {60,28,52,20,62,30,54,22}, { 3,35,11,43, 1,33, 9,41}, {51,19,59,27,49,17,57,25},
    {15,47, 7,39,13,45, 5,37}, {63,31,55,23,61,29,53,21} };

uint32_t nos_dbg_us[4];   /* host trace: under draw, over draw, select, scratch clear */

/* Scratch fb over the same rows as `fb`, clipped to columns [x0, x1) and cleared. */
static void scratch_fb(const gfx_fb *fb, gfx_fb *o, int x0, int x1)
{
    *o = *fb;
    o->px = s_scratch;
    o->stride = 0; o->x_base = 0;
    o->cx0 = x0; o->cx1 = x1;
    uint32_t q = hal_micros();
    for (int r = 0; r < o->h; r++) memset(o->px + (size_t)r * GFX_W + x0, 0, (size_t)(x1 - x0) * 2);
    GFX_COST_PX(GFX_COST_FILL, o->h * (x1 - x0));   /* sim cost model only */
    nos_dbg_us[3] += hal_micros() - q;
}

/* Dissolve: `under` is already in fb; draw `over` into the scratch rows and take its
 * pixels where the ordered-dither mask says so. */
static void dissolve_over(const gfx_fb *fb, const nos_screen *over, float p)
{
    if (!over) return;
    if (fb->rec) { if (over->draw) { gfx_fb f = *fb; over->draw(&f); } return; }
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    gfx_fb o;
    scratch_fb(fb, &o, c0, c1);
    uint32_t q = hal_micros();
    if (over->draw) over->draw(&o);
    uint32_t q2 = hal_micros();
    nos_dbg_us[1] += q2 - q;
    int thr = (int)(p * 64.0f + 0.5f);
    for (int r = 0; r < fb->h; r++) {
        int y = fb->y_base + r;
        uint16_t *d = gfx_row(fb, r), *s = o.px + (size_t)r * GFX_W;
        const uint8_t *b = BAYER8[y & 7];
        for (int x = c0; x < c1; x++) if (b[x & 7] < thr) d[x] = s[x];
    }
    GFX_COST_PX(GFX_COST_SELECT, fb->h * (c1 - c0));   /* sim cost model only */
    nos_dbg_us[2] += hal_micros() - q2;
}

/* Dot-matrix iris. The new screen shows through a circle growing from the tapped
 * icon; its edge is a 34 px ring where an ordered dither turns it into dots. Inside
 * the ring only `over` can be seen and outside it only `under`, so each screen is
 * drawn just where it is visible (over straight into the stripe), and only the ring
 * pays for two layers. The dither test compares squared distance against one
 * threshold per Bayer level: no square root per pixel. */
#define IRIS_ROWS 16
static void iris_draw(const gfx_fb *fb, const nos_screen *under, const nos_screen *over,
                      float p, int dimk)
{
    if (fb->rec) {
        draw_screen(fb, under, 0, 0, GFX_W, GFX_H, 0, 0, dimk);
        if (over && over->draw) { gfx_fb f = *fb; over->draw(&f); }
        return;
    }
    if (fb->h > IRIS_ROWS) {                         /* sim full frames: 16-row bands */
        for (int r0 = 0; r0 < fb->h; r0 += IRIS_ROWS) {
            gfx_fb sub;
            int y0 = fb->y_base + r0, y1 = y0 + IRIS_ROWS;
            if (y1 > fb->y_base + fb->h) y1 = fb->y_base + fb->h;
            if (sub_fb(fb, &sub, 0, y0, GFX_W, y1, 0, 0)) iris_draw(&sub, under, over, p, dimk);
        }
        return;
    }
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    float fx = (float)nos_iris_x, fy = (float)nos_iris_y;
    /* the glass is a circle: the farthest visible pixel is centre distance + radius */
    float far = gfx_fast_sqrt((fx - GFX_CX) * (fx - GFX_CX) + (fy - GFX_CY) * (fy - GFX_CY)) + GFX_W * 0.5f;
    const float band = 34.0f, R0 = 36.0f;      /* the tapped icon is the seed, not a hole */
    float R = R0 + p * (far + band - R0) - band * 0.5f;
    float rin = R - band * 0.5f, rout = R + band * 0.5f;
    float rin2 = rin > 0 ? rin * rin : 0, rout2 = rout > 0 ? rout * rout : 0;

    /* per-row spans: [a0, a1) inside the outer edge, [i0, i1) solidly inside */
    int16_t a0[IRIS_ROWS], a1[IRIS_ROWS], i0[IRIS_ROWS], i1[IRIS_ROWS];
    int A0 = c1, A1 = c0, I0 = c0, I1 = c1;          /* union of spans, common interior */
    for (int r = 0; r < fb->h; r++) {
        float dy = (fb->y_base + r + 0.5f) - fy, dy2 = dy * dy;
        if (rout <= 0 || dy2 >= rout2) { a0[r] = a1[r] = i0[r] = i1[r] = 0; I1 = I0; continue; }
        float hw_out = gfx_fast_sqrt(rout2 - dy2);   /* libm sqrtf is ~1000 cycles here */
        int x0 = (int)(fx - hw_out), x1 = (int)(fx + hw_out) + 1;
        if (x0 < c0) x0 = c0;
        if (x1 > c1) x1 = c1;
        int n0 = x1, n1 = x1;
        if (rin > 0 && rin2 > dy2) {
            float hw_in = gfx_fast_sqrt(rin2 - dy2);
            n0 = (int)(fx - hw_in) + 1; n1 = (int)(fx + hw_in);
        }
        if (n0 < x0) n0 = x0;
        if (n1 > x1) n1 = x1;
        if (n1 < n0) n1 = n0;
        a0[r] = (int16_t)x0; a1[r] = (int16_t)x1; i0[r] = (int16_t)n0; i1[r] = (int16_t)n1;
        if (x1 > x0) { if (x0 < A0) A0 = x0; if (x1 > A1) A1 = x1; }
        if (n0 > I0) I0 = n0;
        if (n1 < I1) I1 = n1;
    }
    if (I1 <= I0) I0 = I1 = c0;                      /* no column is inside on every row */

    uint32_t q = hal_micros();
    if (I1 > I0) {
        draw_screen(fb, under, c0, 0, I0, GFX_H, 0, 0, dimk);
        draw_screen(fb, under, I1, 0, c1, GFX_H, 0, 0, dimk);
    } else draw_screen(fb, under, c0, 0, c1, GFX_H, 0, 0, dimk);
    uint32_t q1 = hal_micros();
    nos_dbg_us[0] += q1 - q;
    if (!over || A1 <= A0) return;
    if (I1 > I0) draw_screen(fb, over, I0, 0, I1, GFX_H, 0, 0, 32);
    nos_dbg_us[1] += hal_micros() - q1;
    if (!s_scratch) return;

    /* the ring: over into the scratch rows, then dither it in */
    float thr2[64];
    for (int b = 0; b < 64; b++) {
        float t = rout - (float)b * band * (1.0f / 64.0f);   /* take if distance < t */
        thr2[b] = t > 0 ? t * t : -1.0f;
    }
    int span[2][2] = { { A0, I1 > I0 ? I0 : A1 }, { I1 > I0 ? I1 : A1, A1 } };
    for (int k = 0; k < 2; k++) {
        int x0 = span[k][0], x1 = span[k][1];
        if (x1 <= x0) continue;
        gfx_fb o;
        scratch_fb(fb, &o, x0, x1);
        uint32_t q2 = hal_micros();
        if (over->draw) over->draw(&o);
        uint32_t q3 = hal_micros();
        nos_dbg_us[1] += q3 - q2;
        unsigned sel = 0;                            /* sim cost model only */
        for (int r = 0; r < fb->h; r++) {
            int lo = a0[r] > x0 ? a0[r] : x0, hi = a1[r] < x1 ? a1[r] : x1;
            if (hi <= lo) continue;
            int y = fb->y_base + r;
            float dy = (y + 0.5f) - fy, dy2 = dy * dy;
            uint16_t *d = gfx_row(fb, r), *src = o.px + (size_t)r * GFX_W;
            const uint8_t *b = BAYER8[y & 7];
            int n0 = i0[r], n1 = i1[r];
            sel += (unsigned)(hi - lo);
            for (int x = lo; x < hi; x++) {
                if (x >= n0 && x < n1) {             /* solid on this row: copy the run */
                    int e = n1 < hi ? n1 : hi;
                    memcpy(d + x, src + x, (size_t)(e - x) * 2);
                    x = e - 1;
                    continue;
                }
                float dx = (x + 0.5f) - fx;
                if (dx * dx + dy2 < thr2[b[x & 7]]) d[x] = src[x];
            }
        }
        GFX_COST_PX(GFX_COST_SELECT, sel);
        nos_dbg_us[2] += hal_micros() - q3;
    }
}

static void draw_overlays(gfx_fb *fb)
{
    if (B.active && B.y.x > 0.5f) ui_draw_banner(fb, B.title, B.body, B.icon, B.y.x - BANNER_H);
    if (TO.active) ui_draw_toast(fb, TO.text, (nos_t - TO.t0) / TOAST_S);
}

static void scene_draw(gfx_fb *fb)
{
    if (T.state == TXS_IDLE) {
        draw_screen(fb, top_screen(), 0, 0, GFX_W, GFX_H, 0, 0, 32);
        draw_overlays(fb);
        return;
    }
    float p = m_clamp(T.p, 0, 1);
    int W = GFX_W, H = GFX_H;
    switch (T.type) {
    case TX_PUSH: {
        int edge = (int)lroundf(W * (1.0f - p));          /* over's left edge */
        int uox = (int)lroundf(-W * 0.30f * p);           /* parallax */
        int dimk = 32 - (int)lroundf(16.0f * p);
        draw_screen(fb, T.under, 0, 0, edge, H, uox, 0, dimk);
        draw_screen(fb, T.over, edge, 0, W, H, edge, 0, 32);
        break;
    }
    case TX_RISE: case TX_DROP: {
        int rise = T.type == TX_RISE;
        int off = (int)lroundf(H * (1.0f - p));
        int top = rise ? off : 0, bot = rise ? H : H - off;
        int uoy = (int)lroundf((rise ? -1 : 1) * H * 0.10f * p);
        int dimk = 32 - (int)lroundf(26.0f * p);
        if (rise) draw_screen(fb, T.under, 0, 0, W, top, 0, uoy, dimk);
        else      draw_screen(fb, T.under, 0, bot, W, H, 0, uoy, dimk);
        draw_screen(fb, T.over, 0, top, W, bot, 0, rise ? off : -off, 32);
        /* the sheet's leading edge: a hairline across the glass and a grabber pill, so
         * the face reads as covered, not sliced */
        if (off > 0 && off < H) {
            int ey = rise ? top : bot - 2;
            gfx_fb e;
            if (nos_subview(fb, &e, 0, ey, W, ey + 2, 0, 0)) gfx_fill_rect(&e, 0, ey, W, 2, GFX_RGB(51, 51, 51));
            float gy = rise ? top + 14.0f : bot - 16.0f;
            gfx_fill_pill(fb, GFX_CX - 15, gy, GFX_CX + 15, 2.5f, GFX_RGB(102, 102, 102));
        }
        break;
    }
    case TX_IRIS: case TX_DISSOLVE: {
        if (!s_scratch && T.type == TX_DISSOLVE) {   /* no second layer: cut at the midpoint */
            draw_screen(fb, p < 0.5f ? T.under : T.over, 0, 0, W, H, 0, 0, 32);
            break;
        }
        if (T.type == TX_IRIS) {
            iris_draw(fb, T.under, p > 0.001f ? T.over : 0, p, 32 - (int)lroundf(18.0f * p));
            break;
        }
        uint32_t q = hal_micros();
        draw_screen(fb, T.under, 0, 0, W, H, 0, 0, 32);
        nos_dbg_us[0] += hal_micros() - q;
        if (p > 0.001f) dissolve_over(fb, T.over, p);
        break;
    }
    default:
        draw_screen(fb, T.over, 0, 0, W, H, 0, 0, 32);
    }
    draw_overlays(fb);
}

/* ------------------------------------------------------------- damage */
#define MAXRECTS 12
#define MERGE_GAP 12
static gfx_rec s_rec_a, s_rec_b;
static gfx_rec *s_rec_cur = &s_rec_a, *s_rec_prev = &s_rec_b;
int nos_dbg_rect[MAXRECTS][4], nos_dbg_nr;

static int rect_near(const int *a, const int *b, int gap)
{
    return a[0] <= b[2] + gap && b[0] <= a[2] + gap && a[1] <= b[3] + gap && b[1] <= a[3] + gap;
}
static void rect_union(int *a, const int *b)
{
    if (b[0] < a[0]) a[0] = b[0];
    if (b[1] < a[1]) a[1] = b[1];
    if (b[2] > a[2]) a[2] = b[2];
    if (b[3] > a[3]) a[3] = b[3];
}
static int cmd_same(const gfx_cmd *a, const gfx_cmd *b)
{
    return a->hash == b->hash && a->x0 == b->x0 && a->y0 == b->y0 && a->x1 == b->x1 && a->y1 == b->y1;
}
static int damage_add(int rects[][4], int *nr, const gfx_cmd *c)
{
    if (c->x1 <= c->x0 || c->y1 <= c->y0) return 0;
    int box[4] = { c->x0, c->y0, c->x1, c->y1 };
    for (int r = 0; r < *nr; r++)
        if (rect_near(rects[r], box, MERGE_GAP)) { rect_union(rects[r], box); return 0; }
    if (*nr >= MAXRECTS) {
        /* out of rects: merge into the one that grows least, rather than giving up and
         * repainting the whole screen (busy lists hit this ~14% of fling frames) */
        int best = 0; long best_cost = -1;
        for (int r = 0; r < *nr; r++) {
            int u[4] = { rects[r][0], rects[r][1], rects[r][2], rects[r][3] };
            rect_union(u, box);
            long grow = (long)(u[2] - u[0]) * (u[3] - u[1]) - (long)(rects[r][2] - rects[r][0]) * (rects[r][3] - rects[r][1]);
            if (best_cost < 0 || grow < best_cost) { best_cost = grow; best = r; }
        }
        rect_union(rects[best], box);
        return 0;
    }
    memcpy(rects[*nr], box, sizeof box);
    (*nr)++;
    return 0;
}
static int compute_damage(gfx_rec *cur, gfx_rec *prev, int rects[][4])
{
    if (cur->overflow || prev->overflow) return -1;
    int nr = 0;
    if (cur->n == prev->n) {
        for (int i = 0; i < cur->n; i++) {
            if (cmd_same(&cur->c[i], &prev->c[i])) continue;
            if (damage_add(rects, &nr, &prev->c[i]) || damage_add(rects, &nr, &cur->c[i])) return -1;
        }
    } else {
        /* A command appeared or vanished. Draw order is code order, so a pixel whose
         * covering commands are the same set as last frame is unchanged: damage only
         * commands present in one frame and not the other (matched by bbox + hash),
         * so two small changes far apart stay two small rects. */
        static uint16_t tab[1024];            /* open-addressed: index+1 into prev, 0 empty */
        static uint8_t used[GFX_MAX_CMDS];
        memset(tab, 0, sizeof tab);
        memset(used, 0, sizeof used);
        for (int i = 0; i < prev->n && i < GFX_MAX_CMDS; i++) {
            unsigned h = (prev->c[i].hash ^ (unsigned)(prev->c[i].x0 * 31 + prev->c[i].y0)) & 1023u;
            while (tab[h]) h = (h + 1) & 1023u;
            tab[h] = (uint16_t)(i + 1);
        }
        for (int i = 0; i < cur->n && i < GFX_MAX_CMDS; i++) {
            const gfx_cmd *c = &cur->c[i];
            unsigned h = (c->hash ^ (unsigned)(c->x0 * 31 + c->y0)) & 1023u;
            int found = 0;
            while (tab[h]) {
                int j = tab[h] - 1;
                if (!used[j] && cmd_same(c, &prev->c[j])) { used[j] = 1; found = 1; break; }
                h = (h + 1) & 1023u;
            }
            if (!found && damage_add(rects, &nr, c)) return -1;
        }
        for (int j = 0; j < prev->n && j < GFX_MAX_CMDS; j++)
            if (!used[j] && damage_add(rects, &nr, &prev->c[j])) return -1;
    }
    for (int pass = 0, changed = 1; changed && pass < MAXRECTS * 2; pass++) {
        changed = 0;
        for (int i = 0; i < nr; i++)
            for (int j = i + 1; j < nr; j++)
                if (rect_near(rects[i], rects[j], MERGE_GAP)) {
                    rect_union(rects[i], rects[j]);
                    memcpy(rects[j], rects[nr - 1], sizeof rects[j]);
                    nr--; changed = 1; j--;
                }
    }
    for (int i = 0; i < nr; i++) {
        if (rects[i][0] < 0) rects[i][0] = 0;
        if (rects[i][1] < 0) rects[i][1] = 0;
        if (rects[i][2] > GFX_W) rects[i][2] = GFX_W;
        if (rects[i][3] > GFX_H) rects[i][3] = GFX_H;
        /* the QSPI panel needs even-aligned windows (odd starts shear or jitter) */
        rects[i][0] &= ~1; rects[i][2] = (rects[i][2] + 1) & ~1;
        rects[i][1] &= ~1; rects[i][3] = (rects[i][3] + 1) & ~1;
        if (rects[i][2] > GFX_W) rects[i][2] = GFX_W;
        if (rects[i][3] > GFX_H) rects[i][3] = GFX_H;
    }
    return nr;
}

static void render_rect(int rx0, int ry0, int rx1, int ry1, int *slot)
{
    if (rx1 <= rx0 || ry1 <= ry0) return;
    int chunk = s_stripe_h > 0 ? s_stripe_h : (ry1 - ry0);
    for (int y0 = ry0; y0 < ry1; y0 += chunk) {
        int rows = ry1 - y0 < chunk ? ry1 - y0 : chunk;
        gfx_fb fb;
        memset(&fb, 0, sizeof fb);
        fb.y_base = y0;
        fb.h = rows;
        fb.cx0 = rx0; fb.cx1 = rx1;
        if (s_stripe_h > 0) {
            if (s_wait_free) s_wait_free();
            fb.px = s_stripe[*slot];
            scene_draw(&fb);
            s_flush(fb.px + rx0, GFX_W, rx0, y0, rx1 - rx0, rows);
            *slot ^= 1;
        } else {
            fb.px = &s_full[y0 * GFX_W];
            scene_draw(&fb);
        }
    }
}

/* ---------------------------------------------------------------- frame */
void nos_init(uint16_t *fullframe)
{
    s_full = fullframe;
    if (!s_scratch && !s_stripe_h && fullframe)
        s_scratch = (uint16_t *)hal_big_alloc(GFX_W * GFX_H * 2);
    s_last_input_ms = hal_millis();
}

static void tick_screens(float dt)
{
    if (T.state != TXS_IDLE) {
        if (T.under && T.under->tick) T.under->tick(dt);
        if (T.over && T.over != T.under && T.over->tick) T.over->tick(dt);
    } else {
        const nos_screen *s = top_screen();
        if (s && s->tick) s->tick(dt);
    }
}

static void tx_complete(void)
{
    if (T.p <= 0.001f && T.popping && s_depth > 1) {      /* the over screen leaves */
        call_leave(top_screen());
        s_depth--;
        call_reveal(top_screen());
    }
    T.state = TXS_IDLE;
    T.popping = 0; T.cancel_pops = 0; T.lin_v = 0;
    s_force_full = 1;
}

static void tick_transition(float dt)
{
    if (T.state != TXS_AUTO) return;
    int moving;
    if (T.lin_v > 0) {
        T.p -= T.lin_v * dt;
        moving = T.p > 0;
        if (!moving) T.p = 0;
        T.sp.x = T.p * T.span;
    } else if (T.use_tween) { T.p = mtw_value(&T.tw); moving = !mtw_done(&T.tw); if (!moving) T.p = T.tw.to; }
    else {
        moving = msp_step(&T.sp, dt);
        /* end as soon as the picture stops changing: within 1.5 px of the target
         * and slower than 3 px per frame (a critically damped tail is otherwise
         * ~10 frames of full repaints that look identical) */
        if (moving && fabsf(T.sp.x - T.sp.target) < 1.5f && fabsf(T.sp.v) < 90.0f) {
            msp_set(&T.sp, T.sp.target);
            moving = 0;
        }
        T.p = T.sp.x / T.span;
    }
    if (moving) return;
    tx_complete();
}

/* Land a running transition at its destination now (a tap during its last frames). */
static void tx_finish(void)
{
    if (T.state != TXS_AUTO) return;
    if (T.lin_v > 0) T.p = 0;
    else if (T.use_tween) T.p = T.tw.to;
    else { msp_set(&T.sp, T.sp.target); T.p = T.sp.x / T.span; }
    tx_complete();
}

static void tick_overlays(float dt)
{
    if (B.active) {
        if (!B.dragging && B.y.target > 0 && nos_t - B.shown_t > BANNER_SHOW_S) msp_to(&B.y, 0);
        if (B.dragging && P.down && P.owner == 2) {
            float d = (float)(P.y - P.y0);
            B.y.x = BANNER_H + (d < 0 ? d : rubber_band(d, 200.0f));
        } else if (!msp_step(&B.y, dt) && B.y.target <= 0) B.active = 0;
    }
    if (TO.active && nos_t - TO.t0 > TOAST_S) TO.active = 0;
}

uint32_t nos_frame_run(float elapsed_ms)
{
    /* quantise time to whole periods so motion steps stay equal */
    int k = (int)(elapsed_ms / NOS_PERIOD_MS + 0.5f);
    if (k < 1) k = 1;
    if (k > 4) k = 4;                                /* after a stall, don't teleport */
    nos_dt = (float)k * (1.0f / NOS_FPS);
    nos_t += nos_dt;
    nos_frame++;

    hal_frame_begin();
    process_touch();
    process_long();
    process_button();
    if (s_air) {
        nos_input in; memset(&in, 0, sizeof in); in.type = IN_AIR; in.x = s_air; s_air = 0;
        const nos_screen *s = top_screen();
        if (s && s->input) s->input(&in);
        nos_keep_awake();
    }
    if (nos_bg_tick) nos_bg_tick(nos_dt);
    tick_transition(nos_dt);
    tick_screens(nos_dt);
    tick_overlays(nos_dt);
    prefs_tick();

    static int rects[MAXRECTS][4];
    int nr;
    int moving_tx = T.state != TXS_IDLE;
    uint32_t t0 = hal_micros();
    s_rec_cur->n = 0; s_rec_cur->overflow = 0;
    if (!moving_tx) {
        gfx_fb rfb; memset(&rfb, 0, sizeof rfb);
        rfb.h = GFX_H; rfb.cx0 = 0; rfb.cx1 = GFX_W; rfb.rec = s_rec_cur;
        scene_draw(&rfb);
    } else s_rec_cur->overflow = 1;                  /* a moving transition repaints all */
    uint32_t t1 = hal_micros();
    int full = s_force_full || moving_tx || nos_debug_force_full;
    nr = full ? -1 : compute_damage(s_rec_cur, s_rec_prev, rects);
    if (nr < 0) {
        nr = 1; rects[0][0] = 0; rects[0][1] = 0; rects[0][2] = GFX_W; rects[0][3] = GFX_H;
        full = 1;
    }
    if (!moving_tx) s_force_full = 0;

    int slot = 0;
    uint32_t px = 0;
    for (int i = 0; i < nr; i++) {
        render_rect(rects[i][0], rects[i][1], rects[i][2], rects[i][3], &slot);
        px += (uint32_t)(rects[i][2] - rects[i][0]) * (uint32_t)(rects[i][3] - rects[i][1]);
    }
    uint32_t t2 = hal_micros();
    hal_frame_end();

    nos_last.dirty_px = px;
    nos_last.rects = (uint32_t)nr;
    nos_last.cmds = (uint32_t)s_rec_cur->n;
    nos_last.record_us = t1 - t0;
    nos_last.raster_us = t2 - t1;
    nos_last.full = full;
    nos_dbg_nr = nr;
    memcpy(nos_dbg_rect, rects, sizeof rects);

    gfx_rec *tmp = s_rec_prev; s_rec_prev = s_rec_cur; s_rec_cur = tmp;
    return px;
}
