/* nos.c — NOS engine: frame loop, screen stack, gestures, transitions, damage. See nos.h. */
#include "nos.h"
#include "developer.h"
#include "motion.h"
#include "prefs.h"
#include "hal_ext.h"
#include "ui.h"
#include "ui_blur_observe.h"
#include "apps/nos_apps.h"
#include "gfx_cost.h"
#include "cost_profile.h"
#include "render_layers.h"
#include "render_history.h"
#include "render_bounds.h"
#include "band_damage.h"
static int s_band_enabled, s_band_stream_current, s_band_stream_previous;
#include "sports.h"
#include "diag_log.h"
#include "native_menu_assets.h"
#include "apps/nos_apps.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

uint32_t nos_frame;
nos_time_t nos_t;
float    nos_dt = 1.0f / NOS_FPS;
nos_stats nos_last;
int nos_iris_x = GFX_W / 2, nos_iris_y = GFX_H / 2;
int nos_iris_seed;
float nos_iris_seed_r = 40.0f;
int nos_iris_blur = !NOS_RENDER_SHARP;
int nos_mblur = NOS_RENDER_SHARP ? 0 : 2;
int nos_debug_force_full;
int nos_temporal_blur = !NOS_RENDER_SHARP;
#ifdef GFX_COST_MODEL
int (*nos_quality_decision)(unsigned,int,unsigned,int);
static int quality_decide(int kind,unsigned key,int proposed) {
    return nos_quality_decision?nos_quality_decision(nos_frame,kind,key,proposed):proposed;
}
#else
#define quality_decide(kind,key,proposed) (proposed)
#endif

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
    float p_prev;           /* last frame's p: the iris blurs over the band its edge swept */
    /* finger-driven */
    int axis, sign;         /* finger travel that increases p: sign * d(axis) */
    float p0;
    int cancel_pops;        /* finger pull-in: cancelling pops the pulled screen */
    float lin_v;            /* > 0: leaving at constant speed (p per second) toward p = 0 */
    int variant;            /* TXV_*: LEGACY/STEADY rigid geometry, AXIS fade-through, CUT */
    int frames;             /* AXIS / CUT: length in whole frames */
    int released;           /* driven by a finger's release (motion audit: momentum) */
    float p_from;           /* where it began (motion audit origin) */
    int fresh;              /* 2 on the begin frame, 1 on the next (first presented frame) */
    /* AXIS plan for this frame, made in tick: one screen, offset, ink (dim k of 32) */
    const nos_screen *ax_s;
    int ax_ox, ax_oy, ax_k;
} T;
static int s_by_finger;                     /* inside enter(): pulled in by a finger */
int nos_entered_by_finger(void) { return s_by_finger; }
const nos_screen *nos_power_screen;         /* set by the app registry: long-press target */

static int s_force_full = 1;
void nos_invalidate(void) { s_force_full = 1; nos_layers_invalidate(); }

static const nos_screen *top_screen(void) { return s_depth ? s_stack[s_depth - 1].s : 0; }
const nos_screen *nos_top(void) { return top_screen(); }
int nos_depth(void) { return s_depth; }
int nos_transitioning(void) { return T.state != TXS_IDLE; }
__attribute__((weak)) int nos_host_shutdown_pending(void) {return 0;}

static uint32_t s_navigation_request;
static const nos_screen *s_navigation_target;
static int32_t screen_id(const nos_screen *s) { return s?(int32_t)diag_log_name_id(s->name):0; }
static void navigation_result(uint32_t request,int result)
{
    if(!request || request!=s_navigation_request)return;
    diag_action_result(request,result,screen_id(top_screen()));
    s_navigation_request=0;s_navigation_target=NULL;
}
static uint32_t navigation_begin(const nos_screen *target,const nos_screen *source,int action)
{
    navigation_result(s_navigation_request,DIAG_RESULT_CANCELLED);
    s_navigation_target=target;
    s_navigation_request=diag_action_request(DIAG_DOMAIN_NAVIGATION,(uint16_t)action,screen_id(target));
    diag_action_context(s_navigation_request,screen_id(source),s_depth);
    return s_navigation_request;
}
static void navigation_arrive(uint32_t request)
{
    navigation_result(request,top_screen()==s_navigation_target?DIAG_RESULT_SUCCESS:DIAG_RESULT_CANCELLED);
}

static int s_revealing;
int nos_revealed(void) { return s_revealing; }
#define IRIS_BUILD 0.40f                     /* iris entries start their build here (§8) */
/* Per-frame step budgets (docs/MOTION30.html §14): screen transitions <= 64 px a frame. */
#define TX_PEAK_PX 62.0f                     /* biggest step a programmatic transition takes */
#define AXIS_IN_PX 56.0f                     /* shared axis: the incoming screen's travel */
#define AXIS_OUT_PX 28.0f                    /* ... and the outgoing one's (half: parallax) */
#define AXIS_FRAMES 10
static float s_enter_delay;                  /* set around enter() of an iris push */
nos_time_t nos_enter_t(void) { return nos_t + s_enter_delay; }
/* The iris radius grows from the tapped icon to the farthest visible pixel. */
#define IRIS_R0 36.0f
static float iris_far(void)
{
    float fx = (float)nos_iris_x, fy = (float)nos_iris_y;
    return gfx_fast_sqrt((fx - GFX_CX) * (fx - GFX_CX) + (fy - GFX_CY) * (fy - GFX_CY)) + GFX_W * 0.5f + 2.0f;
}
/* Whole frames for an EASE_STEADY move of `px` so its peak step (1.6x the average) stays
 * under TX_PEAK_PX, within the screen-transition range of 8-12 frames. */
static int steady_frames(float px, int lo, int hi)
{
    int n = (int)ceilf(1.6f * px / TX_PEAK_PX);
    return n < lo ? lo : n > hi ? hi : n;
}
/* The iris takes its time (12-16 frames, ~28 px of radius a frame on average): on
 * EASE_STEADY its radius steps then shrink evenly, frame to frame by at most ~6 px
 * (51 49 47 ... 8 2 for a launcher tap), so the circle swells smoothly and its area grows
 * at a near-even rate; at 8-12 frames the steps were up to ~70 px and read as jumps. */
static int iris_frames(void)
{
    int n = (int)lroundf((iris_far() - IRIS_R0) / 28.0f);
    return n < 12 ? 12 : n > 16 ? 16 : n;
}
/* A shared-axis or cut open shows the new screen only from its 4th / 3rd frame: its
 * entry build starts there (nos_enter_t), not under the fade-out. */
static int s_in_frame;
static float appear_delay(int variant)
{
    int k = variant == TXV_AXIS ? AXIS_FRAMES * 35 / 100 : variant == TXV_CUT ? 2 : 0;
    return k ? M_FRAMES(k + (s_in_frame ? 0 : 1)) : 0.0f;
}
/* When the iris reaches IRIS_BUILD, in seconds from its first frame. */
static float iris_build_delay(void)
{
    int n = iris_frames();
    float lo = 0, hi = 1;
    for (int i = 0; i < 20; i++) {
        float m = 0.5f * (lo + hi);
        if (bezier_ease(&EASE_STEADY, m) < IRIS_BUILD) lo = m; else hi = m;
    }
    float d = hi * n / NOS_FPS - nos_dt;          /* the first frame is already one step in */
    return d < 0 ? 0 : d;
}

/* The variant a programmatic open / close of each geometry uses (TXV_*), chosen on film
 * against docs/MOTION30.html §5 (build/nos/smooth30/engine/). Fingers always move the
 * geometry 1:1. */
static const uint8_t TXV_DEF[2][TX_DISSOLVE + 1] = {
    /* close */ { 0, TXV_AXIS, TXV_STEADY, TXV_STEADY, 0, 0 },
    /* open  */ { 0, TXV_AXIS, TXV_STEADY, TXV_STEADY, 0, 0 },
};
static uint8_t s_txv[2][TX_DISSOLVE + 1] = {
    { 0, TXV_AXIS, TXV_STEADY, TXV_STEADY, 0, 0 },
    { 0, TXV_AXIS, TXV_STEADY, TXV_STEADY, 0, 0 },
};
void nos_debug_tx_variant(int tx, int opening, int variant)
{
    int o = opening ? 1 : 0;
    if (tx >= TX_PUSH && tx <= TX_DROP && variant >= TXV_DEFAULT && variant <= TXV_CUT)
        s_txv[o][tx] = (uint8_t)(variant ? variant : TXV_DEF[o][tx]);
}
static void banner_yield(int type);
static void tx_complete(void);
static void tick_transition(float dt);
static int s_tx_ticked;                      /* inside nos_frame_run, after tick_transition */
static int s_tx_exit;                        /* nos_home: rigid exits leave at constant speed */
/* s_in_frame: inside nos_frame_run (nos_t already advanced to the frame being built), an
 * animation started now leads by one frame so this frame moves. Started between frames
 * (a host command), the next frame's clock step does that. */
static void call_enter(const nos_screen *s) {
    if (s) diag_log_event(DIAG_NAVIGATION, (int32_t)diag_log_name_id(s->name), s_depth, s_revealing);
    if (s && s->enter) s->enter();
}
static void call_reveal(const nos_screen *s) { s_revealing = 1; call_enter(s); s_revealing = 0; }
static void call_leave(const nos_screen *s) { if (s && s->leave) s->leave(); }
static int s_covering;
int nos_leaving_covered(void) { return s_covering; }
static void call_cover(const nos_screen *s) { s_covering = 1; call_leave(s); s_covering = 0; }

static float tx_span(int type)
{
    return (type == TX_RISE || type == TX_DROP) ? (float)GFX_H : (float)GFX_W;
}

/* A rigid exit at constant speed, the most a transition may step a frame (TX_PEAK_PX):
 * any ease would read as a hesitation mid-exit, and a spring's tail is creep. */
static float exit_speed(void)
{
    float px = T.type == TX_IRIS ? iris_far() - IRIS_R0 : T.span;
    return TX_PEAK_PX * NOS_FPS / (px > 1 ? px : 1);          /* p per second */
}

/* Where a running transition is, for a close that starts from it: rigid geometry (a
 * finger, a spring, a steady push) leaves from where it is; a fade-through or a cut
 * lands first (its in-between frames are not positions a close can continue from). */
static float tx_where(void)
{
    if (T.state == TXS_IDLE) return 1.0f;
    if (T.state == TXS_AUTO && (T.variant == TXV_AXIS || T.variant == TXV_CUT)) return 1.0f;
    return T.p;
}

static void tx_begin(int type, const nos_screen *under, const nos_screen *over, float p_from, float p_to)
{
    T.type = type;
    T.under = under;
    T.over = over;
    T.span = tx_span(type);
    T.p = T.p_prev = T.p_from = p_from;
    T.released = 0;
    T.fresh = 2;
    T.ax_s = 0;
    /* Nothing here follows a finger (finger releases set their own spring), so no
     * spring either: a spring's settle is ~6 frames of 1-7 px creep, all full repaints.
     * Programmatic moves are planned in whole frames inside the 30 fps step budget
     * (docs/MOTION30.html §5): PUSH/RISE/DROP use their variant (shared-axis fade-through,
     * steady push, cut), the iris and dissolve EASE_STEADY. Closes of a moving screen
     * leave rigidly at constant speed. A tap during an open lands it; one during a close
     * is spent (press_begin): neither can be grabbed back mid-flight. */
    int opening = p_to > p_from;
    int rigid = type == TX_PUSH || type == TX_RISE || type == TX_DROP;
    int v = rigid ? s_txv[opening][type] : TXV_STEADY;
    if (!opening && p_from < 0.999f && v != TXV_LEGACY) v = TXV_STEADY;   /* moving: leave from where it is */
    T.variant = v;
    T.frames = 0;
    T.lin_v = 0;
    T.use_tween = 1;
    msp_init_px(&T.sp, p_from * T.span, SPRING_PAGE);
    float d = fabsf(p_to - p_from);
    if (v == TXV_LEGACY) {                           /* the pre-MOTION30 motion, for A/B films */
        if ((rigid || s_tx_exit) && !opening) { T.use_tween = 0; T.lin_v = 1.0f / 0.22f; }
        else mtw_start(&T.tw, p_from, p_to, 0.34f * (d > 0.45f ? d : 0.45f), &EASE_STANDARD);
    } else if (v == TXV_AXIS) {
        T.frames = AXIS_FRAMES;
        mtw_start(&T.tw, p_from, p_to, M_FRAMES(T.frames), &EASE_LINEAR);
    } else if (v == TXV_CUT) {
        T.frames = M_CUT_FRAMES;
        mtw_start(&T.tw, p_from, p_to, M_FRAMES(T.frames), &EASE_LINEAR);
    } else if (s_tx_exit && !opening) {              /* nos_home: a button has no finger speed to keep */
        T.use_tween = 0;
        T.lin_v = exit_speed();
    } else if (type == TX_DISSOLVE) {
        mtw_start(&T.tw, p_from, p_to, M_FRAMES(9), &EASE_STEADY);
    } else if (opening || type == TX_IRIS) {
        /* arrivals (and the iris shrinking back into its icon) on EASE_STEADY */
        float px = type == TX_IRIS ? iris_far() - IRIS_R0 : T.span * d;
        int n = type == TX_IRIS ? iris_frames() : steady_frames(px, 7, 12);
        mtw_start(&T.tw, p_from, p_to, M_FRAMES(n), &EASE_STEADY);
    } else {                                         /* rigid exits: constant speed */
        T.use_tween = 0;
        T.lin_v = exit_speed();
    }
    if (T.use_tween && s_in_frame) mtw_lead(&T.tw);  /* input -> this frame already moves */
    /* which finger travel grows p, so a moving transition can be grabbed */
    T.axis = (type == TX_RISE || type == TX_DROP) ? AXIS_Y : AXIS_X;
    T.sign = type == TX_DROP ? 1 : -1;
    T.state = TXS_AUTO;
    s_force_full = 1;
    if (opening) banner_yield(type);
    if (s_tx_ticked) tick_transition(0);             /* begun after this frame's tick: plan it now */
}

void nos_set_root(const nos_screen *s)
{
    uint32_t request=navigation_begin(s,top_screen(),DIAG_ACT_OPEN);
    while (s_depth > 0) { call_leave(s_stack[s_depth - 1].s); s_depth--; }
    s_stack[0].s = s; s_stack[0].tx = TX_NONE; s_depth = 1;
    T.state = TXS_IDLE;
    call_enter(s);
    navigation_arrive(request);
    s_force_full = 1;
}

void nos_push(const nos_screen *s, int tx)
{
    if (!s || s_depth >= MAX_DEPTH) {
        uint32_t rejected=diag_action_request(DIAG_DOMAIN_NAVIGATION,DIAG_ACT_OPEN,screen_id(s));
        diag_action_context(rejected,screen_id(top_screen()),s_depth);
        diag_action_result(rejected,DIAG_RESULT_REJECTED,s_depth);return;
    }
    uint32_t request=navigation_begin(s,top_screen(),DIAG_ACT_OPEN);
    if (T.state != TXS_IDLE) {               /* finish the running one instantly */
        if (T.popping && s_depth > 1) { call_leave(s_stack[s_depth - 1].s); s_depth--; call_reveal(top_screen()); }
        T.state = TXS_IDLE;
    }
    const nos_screen *under = top_screen();
    call_cover(under);
    s_stack[s_depth].s = s; s_stack[s_depth].tx = tx; s_depth++;
    if (tx == TX_IRIS) s_enter_delay = iris_build_delay();
    else if (tx >= TX_PUSH && tx <= TX_DROP) s_enter_delay = appear_delay(s_txv[1][tx]);
    call_enter(s);
    s_enter_delay = 0;
    T.popping = 0; T.replacing = 0; T.cancel_pops = 0;
    if (tx == TX_NONE) { s_force_full = 1; navigation_arrive(request); return; }
    tx_begin(tx, under, s, 0.0f, 1.0f);
}

void nos_replace(const nos_screen *s, int tx)
{
    if (s_depth <= 1) { nos_set_root(s); return; }
    uint32_t request=navigation_begin(s,top_screen(),DIAG_ACT_OPEN);
    T.state = TXS_IDLE;
    const nos_screen *old = top_screen();
    call_leave(old);
    s_stack[s_depth - 1].s = s;
    s_stack[s_depth - 1].tx = tx;
    if (tx == TX_IRIS) s_enter_delay = iris_build_delay();
    call_enter(s);
    s_enter_delay = 0;
    T.popping = 0; T.replacing = 0; T.cancel_pops = 0;
    if (tx == TX_NONE) { s_force_full = 1; navigation_arrive(request); return; }
    tx_begin(tx == TX_PUSH ? TX_DISSOLVE : tx, old, s, 0.0f, 1.0f);
}

/* A push still opening shows `s` instead (a double press caught the launcher's iris on
 * its way in): the geometry keeps going, only the screen inside changes. */
/* the screen a retarget replaced: dims out inside the iris for RT_FRAMES, then leaves */
#define RT_FRAMES 3
static const nos_screen *s_rt_old;
int nos_contains(const nos_screen *screen)
{
    if(!screen)return 0;
    for(int i=0;i<s_depth;i++)if(s_stack[i].s==screen)return 1;
    return (T.state!=TXS_IDLE && (T.under==screen || T.over==screen)) || s_rt_old==screen;
}
static nos_time_t s_rt_t0;
static int s_iris_overk = 32;
static int s_rt_k = 32;                      /* this frame's ink (of 32) for the replaced screen */
static int rt_frame(void) { return (int)((nos_t - s_rt_t0) * NOS_FPS + 0.5f); }
static void rt_flush(void) { if (s_rt_old) { const nos_screen *o = s_rt_old; s_rt_old = 0; call_leave(o); } }

int nos_retarget(const nos_screen *s)
{
    if (s_depth <= 1 || T.state != TXS_AUTO || T.popping || T.over != top_screen() || !s) return 0;
    const nos_screen *old = top_screen();
    if (old == s) return 1;
    navigation_begin(s,old,DIAG_ACT_OPEN);
    rt_flush();
    s_rt_old = old; s_rt_t0 = nos_t;              /* leaves once its dim-out is done */
    s_stack[s_depth - 1].s = s;
    /* its build starts where a fresh iris would reach IRIS_BUILD, or now if past it */
    if (T.type == TX_IRIS && T.use_tween) {
        float d = T.tw.t0 + iris_build_delay() + nos_dt - nos_t;
        s_enter_delay = d > 0 ? d : 0;
    }
    call_enter(s);
    s_enter_delay = 0;
    T.over = s;
    nos_iris_seed = 0;
    return 1;
}

void nos_pop(void)
{
    if (s_depth <= 1) return;
    if (T.state != TXS_IDLE && T.popping) return;     /* already leaving */
    const nos_screen *over = top_screen();
    const nos_screen *under = s_stack[s_depth - 2].s;
    uint32_t request=navigation_begin(under,over,DIAG_ACT_DISMISS);
    int tx = s_stack[s_depth - 1].tx;
    T.popping = 1; T.replacing = 0; T.cancel_pops = 0;
    if (tx == TX_NONE) {
        call_leave(over); s_depth--; call_reveal(under); s_force_full = 1; navigation_arrive(request); return;
    }
    tx_begin(tx, under, over, tx_where(), 0.0f);
}

void nos_pop_with(int tx)
{
    if (s_depth > 1) s_stack[s_depth - 1].tx = tx;
    nos_pop();
}

void nos_home(void)
{
    if (s_depth <= 1) return;
    navigation_begin(s_stack[0].s,top_screen(),DIAG_ACT_OPEN);
    /* Collapse the middle of the stack silently, then animate the top away: from where
     * it is if it is moving (a finger dragging a sheet), along the geometry it is
     * moving with, and accelerating over 0.22 s. A button has no finger speed to keep,
     * and a spring's settle would be ~13 frames of full repaints. The top's leave()
     * runs when it is gone (tx_complete). */
    const nos_screen *top = top_screen();
    int tx = s_stack[s_depth - 1].tx;
    int moving = T.state != TXS_IDLE && T.over == top;
    float from = moving ? tx_where() : 1.0f;
    int geo = moving ? T.type : (tx == TX_NONE ? TX_DISSOLVE : tx);
    T.state = TXS_IDLE;
    for (int i = s_depth - 2; i >= 1; i--) call_leave(s_stack[i].s);   /* covered ones: gone now */
    while (s_depth > 1) s_depth--;
    s_stack[1].s = top; s_stack[1].tx = tx == TX_NONE ? TX_DISSOLVE : tx; s_depth = 2;
    if (geo == TX_IRIS) { nos_iris_x = GFX_W; nos_iris_y = GFX_H / 2; }   /* close toward the button */
    if (from <= 0.001f) { T.popping = 1; T.p = 0; tx_complete(); return; }
    T.popping = 1; T.replacing = 0; T.cancel_pops = 0;
    s_tx_exit = 1;                  /* the iris and dissolve leave at constant speed too */
    tx_begin(geo, s_stack[0].s, top, from, 0.0f);
    s_tx_exit = 0;
}

void nos_home_now(void)
{
    /* home already alone on the glass: nothing was revealed (it keeps its own state) */
    int collapsed = s_depth > 1 || T.state != TXS_IDLE;
    uint32_t request=collapsed?navigation_begin(s_depth?s_stack[0].s:NULL,top_screen(),DIAG_ACT_OPEN):0;
    T.state = TXS_IDLE; T.popping = 0;
    while (s_depth > 1) { call_leave(s_stack[s_depth - 1].s); s_depth--; }
    if (collapsed) call_reveal(top_screen());
    navigation_arrive(request);
    s_force_full = 1;
}

static void P_rebase(void);
/* Finger-driven transition. axis/sign say which finger travel grows p. */
static void finger_begin(int type, const nos_screen *under, const nos_screen *over,
                         float p0, int popping, int axis, int sign)
{
    navigation_begin(popping?under:over,popping?over:under,popping?DIAG_ACT_DISMISS:DIAG_ACT_OPEN);
    T.type = type == TX_IRIS || type == TX_DISSOLVE ? TX_PUSH : type;   /* fingers move rigid geometry */
    T.under = under; T.over = over;
    T.span = tx_span(T.type);
    T.p = T.p0 = T.p_from = p0;
    T.use_tween = 0;
    T.variant = TXV_STEADY;         /* rigid geometry, 1:1 with the finger */
    T.released = 0; T.fresh = 0; T.ax_s = 0;
    msp_init_px(&T.sp, p0 * T.span, SPRING_SHEET);
    T.popping = popping;
    T.axis = axis; T.sign = sign;
    T.lin_v = 0;
    T.state = TXS_FINGER;
    P_rebase();                     /* measure from here: the slop must not jump the screen */
    s_force_full = 1;
    if (!popping) banner_yield(type);
}

void nos_pull(const nos_screen *s, int tx, const nos_input *start)
{
    if (!s || s_depth >= MAX_DEPTH || T.state != TXS_IDLE) return;
    const nos_screen *under = top_screen();
    call_cover(under);
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
    nos_time_t shown_t;
    int dragging;
    int fresh;            /* motion audit: report the hidden origin on its first frame */
    mtween tw;            /* timed arrival / exit; the spring only for the finger */
    int tweening;
    int tap_hold;         /* tapped: the card stays pressed while it leaves */
} B;
#define BANNER_H 190.0f      /* travel: card top 52 + height 136 + margin */
#define BANNER_SHOW_S 4.0f
#define BANNER_IN_F 9        /* component class (MOTION30 §14): EASE_STEADY in, peak ~34 px */
#define BANNER_OUT_F 6       /* constant-speed exit, ~32 px a frame */
/* timed moves are whole-frame tweens: a spring brakes into the bezel, crawling its last
 * px; partial moves (from a drag) take proportionally fewer frames, at least 3 */
static void banner_tween(float to, int frames, const bezier *curve)
{
    float span = to > B.y.x ? to - B.y.x : B.y.x - to;
    if (span < 0.5f) return;
    int n = (int)ceilf(frames * (0.4f + 0.6f * span / BANNER_H));
    mtw_start(&B.tw, B.y.x, to, M_FRAMES(n < 3 ? 3 : n), curve);
    if (s_in_frame) mtw_lead(&B.tw);                /* move on the first frame */
    B.tweening = 1;
    B.y.target = to; B.y.v = 0;
}

void nos_banner(const char *title, const char *body, int icon, void (*on_tap)(void))
{
    snprintf(B.title, sizeof B.title, "%s", title ? title : "");
    snprintf(B.body, sizeof B.body, "%s", body ? body : "");
    B.icon = icon;
    B.on_tap = on_tap;
    if (!B.active) { msp_init_px(&B.y, 0, SPRING_SHEET); B.fresh = 1; }
    B.active = 1;
    if (!B.dragging) banner_tween(BANNER_H, BANNER_IN_F, &EASE_STEADY);
    B.shown_t = nos_t;
    if (!nos_prefs.dnd) nos_feedback(FB_NOTIFY);
    nos_keep_awake();
}

/* A system sheet (notifications, control centre) coming in pushes the banner away. */
static void banner_yield(int type)
{
    if ((type == TX_RISE || type == TX_DROP) && B.active && !B.dragging) banner_tween(0, BANNER_OUT_F, &EASE_LINEAR);
}

/* Toast: drops from behind the top bezel on EASE_STEADY, holds, leaves at constant speed.
 * Planned in whole frames in tick (TO.off = whole px above its resting place). */
#define TOAST_S 1.4f
#define TOAST_IN_F 7
#define TOAST_OUT_F 5
static struct { char text[40]; nos_time_t t0; int active, fresh; float travel, off; } TO;
void nos_toast(const char *text)
{
    snprintf(TO.text, sizeof TO.text, "%s", text ? text : "");
    TO.t0 = nos_t - (s_in_frame ? 1.0f / NOS_FPS : 0.0f);   /* the first frame already moves */
    TO.active = 1;
    TO.travel = ui_toast_travel(TO.text);
    TO.off = TO.travel;
    TO.fresh = 1;
}
static void toast_tick(void)
{
    if (!TO.active) return;
    int k = m_frame_k(TO.t0), n = (int)(TOAST_S * NOS_FPS + 0.5f);
    float shown;                                    /* 0 hidden .. 1 down */
    if (k >= n) { TO.active = 0; return; }
    if (k < TOAST_IN_F) shown = bezier_ease(&EASE_STEADY, (float)k / TOAST_IN_F);
    else if (k > n - TOAST_OUT_F) shown = (float)(n - k) / TOAST_OUT_F;
    else shown = 1.0f;
    TO.off = floorf((1.0f - shown) * TO.travel + 0.5f);   /* whole px: text and pill move together */
}

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
static uint32_t s_last_input_ms, s_last_user_ms;
void nos_keep_awake(void) { s_last_input_ms = hal_millis(); }
static void user_input(void) { s_last_user_ms = hal_millis(); nos_keep_awake(); }
uint32_t nos_user_idle_ms(void) { return hal_millis() - s_last_user_ms; }
int nos_awake_ms(void) { return (int)(hal_millis() - s_last_input_ms); }

/* --------------------------------------------------------------- input */
#define SLOP        12
#define LONG_S      0.45f
#define DOUBLE_MS   320
#define BTN_LONG_MS 600
#define EDGE_ZONE   44
#define VSAMPLES    12       /* 100 ms at the controller's 60-100 Hz */
#define STILL_MS    40       /* a finger still this long before release flings nothing */
#define PRED_S      0.016f   /* drag prediction: half a frame ahead (R10) */
#define PRED_MAX    24.0f

static struct {
    int down, x, y, x0, y0;  /* x, y: what drags see (predicted while dragging) */
    int rx, ry;              /* the latched raw finger */
    int dragging, axis;
    int long_fired;
    nos_time_t t_down;
    int owner;               /* 0 screen, 1 engine back-swipe/pull, 2 banner */
    int sx[VSAMPLES], sy[VSAMPLES];
    uint32_t st[VSAMPLES];
    int ns;
    int mx, my;              /* where the finger last moved (>= 2 px, jitter-proof) */
    uint32_t t_moved, t_up;
} P;

int nos_touch_predict = 1;
static int s_raw_x, s_raw_y, s_raw_down, s_raw_dirty, s_smp_down;
static void process_touch(void);
/* how far the finger has moved since it went down (0 when up): lets a screen drop a
 * pressed state before the drag slop turns the touch into a drag */
int nos_touch_travel(void)
{
    if (!P.down) return 0;
    int dx = abs(P.rx - P.x0), dy = abs(P.ry - P.y0);
    return dx > dy ? dx : dy;
}
/* ---- knock to lock ---- */
int nos_locked;
static uint32_t s_knock_seen;
static int s_knock_init;
static nos_time_t s_lock_hint_t = -100.0;
int nos_knock_enabled(void) { return nos_prefs.knock_lock; }
void nos_lock_hint(void)
{
    if (nos_t - s_lock_hint_t < 2.0f) return;
    s_lock_hint_t = nos_t;
    nos_toast("KNOCK TWICE TO UNLOCK");
}
void nos_set_locked(int on)
{
    if (nos_locked == on) return;
    nos_locked = on;
    float state[]={(float)on};sports_emit(SP_LOCK,state,1);
    nos_feedback(on ? FB_TOGGLE_ON : FB_TOGGLE_OFF);
    nos_toast(on ? "LOCKED" : "UNLOCKED");
    s_lock_hint_t = nos_t;                 /* the toast just said it */
    nos_keep_awake();
}
static void knock_tick(void)
{
    uint32_t k = hal_knock_count();
    if (!s_knock_init) { s_knock_seen = k; s_knock_init = 1; return; }
    if (k == s_knock_seen) return;
    s_knock_seen = k;
    if (nos_knock_enabled()) nos_set_locked(!nos_locked);
}

void nos_touch(int x, int y, int down) { nos_touch_at(x, y, down, hal_millis()); }
void nos_touch_at(int x, int y, int down, uint32_t ms)
{
    if(down==2) {nos_touch_cancel();return;}
    nos_dev_touch(x,y,down);
    static int tx=-1,ty=-1,td=-1;
    if(down!=td || (down && (x!=tx || y!=ty))) {
        float sample[]={(float)x,(float)y,(float)down,(float)ms};sports_emit(SP_TOUCH,sample,4);
        tx=x;ty=y;td=down;
    }
    /* locked: a new press only shows how to unlock (a press already down finishes) */
    if (nos_locked && down && !P.down && !s_raw_down) { nos_lock_hint(); return; }
    /* a batch from a sampling task may hold a press and its release (or a release and a
     * new press): land the pending edge first so no tap is lost */
    if (s_raw_dirty && down != s_raw_down && s_raw_down != P.down) process_touch();
    s_raw_x = x; s_raw_y = y; s_raw_down = down; s_raw_dirty = 1;
    /* velocity samples at the source rate with their own timestamps, not the frame's */
    if (down) {
        if (!s_smp_down) { P.ns = 0; P.mx = x; P.my = y; P.t_moved = ms; }   /* a new press */
        int i = P.ns % VSAMPLES;
        P.sx[i] = x; P.sy[i] = y; P.st[i] = ms; P.ns++;
        if (abs(x - P.mx) >= 2 || abs(y - P.my) >= 2) { P.mx = x; P.my = y; P.t_moved = ms; }
    } else if (s_smp_down) P.t_up = ms;
    s_smp_down = down;
}

/* Least-squares slope over the samples within 100 ms of the newest (at least 3), zero
 * when the finger has been still for STILL_MS (before the release, or now). */
static void velocity(float *vx, float *vy)
{
    *vx = *vy = 0;
    int n = P.ns < VSAMPLES ? P.ns : VSAMPLES;
    if (n < 2) return;
    int newest = (P.ns - 1) % VSAMPLES;
    uint32_t tn = P.st[newest];
    uint32_t now = s_smp_down ? tn : P.t_up;
    if ((int32_t)(now - P.t_moved) >= STILL_MS) return;
    float st = 0, sx = 0, sy = 0, stt = 0, stx = 0, sty = 0; int k = 0;
    for (int j = 0; j < n; j++) {
        int i = (P.ns - 1 - j) % VSAMPLES;
        float t = -(float)(tn - P.st[i]) / 1000.0f;
        if (t < -0.10f) break;
        st += t; sx += P.sx[i]; sy += P.sy[i]; stt += t * t; stx += t * P.sx[i]; sty += t * P.sy[i]; k++;
    }
    if (k < 3) return;
    float den = k * stt - st * st;
    if (den < 1e-9f) return;
    *vx = (k * stx - st * sx) / den;
    *vy = (k * sty - st * sy) / den;
    const float VMAX = 6000.0f;
    *vx = m_clamp(*vx, -VMAX, VMAX);
    *vy = m_clamp(*vy, -VMAX, VMAX);
}

/* Drag prediction (R10): lead the latched finger by ~PRED_S of its velocity, never more
 * than half of this frame's travel (so a sudden stop overshoots at most that, for one
 * frame), PRED_MAX, or against its direction; none once it has stopped moving. */
static float pred_lead(float o, int fd)
{
    if (!fd || (o > 0) != (fd > 0)) return 0;
    float cap = 0.5f * (float)abs(fd);
    if (cap > PRED_MAX) cap = PRED_MAX;
    return o > cap ? cap : o < -cap ? -cap : o;
}
static void predict(int fdx, int fdy)
{
    P.x = P.rx; P.y = P.ry;
    if (!nos_touch_predict || P.ns < 3) return;
    if ((int32_t)(P.st[(P.ns - 1) % VSAMPLES] - P.t_moved) > 20) return;   /* stopping */
    float vx, vy;
    velocity(&vx, &vy);
    P.x = P.rx + (int)lroundf(pred_lead(vx * PRED_S, fdx));
    P.y = P.ry + (int)lroundf(pred_lead(vy * PRED_S, fdy));
}

static int send(nos_in_type type)
{
    const nos_screen *s = top_screen();
    if (s && (type == IN_TAP || type == IN_LONG || type == IN_DRAG_END))
        diag_log_event(DIAG_ACTION, (int32_t)diag_log_name_id(s->name), type, P.axis);
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
    T.released = 1;
    msp_init_px(&T.sp, T.p * T.span, SPRING_PAGE);              /* ends with a 1 px snap */
    T.sp.v = v * T.span;
    msp_to(&T.sp, commit ? T.span : 0.0f);
    T.lin_v = 0;
    if (!commit) {
        /* leaving the glass: keep the finger's speed (never brake an exit), at least
         * the exit speed of a programmatic close (TX_PEAK_PX a frame) */
        float sp = -v, lo = exit_speed();
        if (sp < lo) sp = lo;
        T.lin_v = sp;
    }
    if (T.popping) {
        /* back-swipe: p 1 = stay; reaching 0 pops */
        if (!commit) nos_feedback(FB_BACK);
    } else if (commit) nos_feedback(FB_OPEN);
    else T.popping = 1;               /* a pull or grabbed push let go: the over screen leaves */
}

void nos_touch_cancel(void)
{
    if(P.down && P.owner==0)send(IN_CANCEL);
    if(P.owner==1 && T.state==TXS_FINGER) {
        /* A pulled-in screen goes away; a closing/grabbed existing screen stays.
         * This is cancellation, so no release projection or feedback is used. */
        float target=T.cancel_pops?0.0f:T.span;
        if(T.cancel_pops)T.popping=1;
        T.state=TXS_AUTO;T.use_tween=0;T.released=0;T.lin_v=0;
        msp_init_px(&T.sp,T.p*T.span,SPRING_PAGE);msp_to(&T.sp,target);
    }
    if(P.owner==2) {B.dragging=0;B.tap_hold=0;B.tweening=0;msp_to(&B.y,BANNER_H);}
    P.down=P.dragging=P.long_fired=P.owner=P.ns=0;P.axis=AXIS_NONE;
    s_raw_down=s_raw_dirty=s_smp_down=0;
}


static void tx_finish(void);
static void P_rebase(void) { P.x0 = P.x; P.y0 = P.y; }

static void press_begin(int x, int y)
{
    P.down = 1; P.x = P.x0 = P.rx = x; P.y = P.y0 = P.ry = y;
    P.dragging = 0; P.axis = AXIS_NONE; P.long_fired = 0; P.owner = 0;
    P.t_down = nos_t;
    if (!P.ns) { P.sx[0] = x; P.sy[0] = y; P.st[0] = hal_millis(); P.ns = 1; P.t_moved = P.st[0]; }
    user_input();
    if (in_banner(x, y)) { P.owner = 2; B.dragging = 1; B.tweening = 0; return; }
    if (T.state == TXS_AUTO && !T.use_tween && !T.lin_v &&
        fabsf(T.sp.v) > 150.0f && fabsf(T.sp.x - T.sp.target) > 6.0f) {
        /* grab a moving transition mid-flight: the finger takes over from the spring */
        T.state = TXS_FINGER;
        T.p0 = T.p;
        T.released = 0;
        P.owner = 1;
        return;
    }
    /* a screen on its way out keeps going: the touch is spent (landing it would be a
     * cut, grabbing it would bring back a screen that already acted) */
    if (T.state == TXS_AUTO && T.popping && (T.use_tween || T.lin_v > 0)) { P.owner = 3; return; }
    /* nor does one on its way in: landing it and passing the tap through would act on a
     * screen nobody has seen (a double-tapped EDIT CITIES removed a city) */
    if (T.state == TXS_AUTO && T.use_tween) { P.owner = 3; return; }
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
    if (top_tx == TX_DISSOLVE) sheet = 1;     /* in-place screens (face picker) don't slide away */
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
        int fdx = x - P.rx, fdy = y - P.ry;          /* the finger's travel since the last latch */
        P.rx = x; P.ry = y;
        P.x = x; P.y = y;
        user_input();
        if (P.owner == 3) return;
        if (P.dragging || P.owner == 1 || P.owner == 2) predict(fdx, fdy);
        if (P.owner == 2) return;
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
            banner_tween(0, BANNER_OUT_F, &EASE_LINEAR);
            B.tap_hold = 1;
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
    user_input();
    nos_input in; memset(&in, 0, sizeof in); in.type = t;
    if (P.down && P.owner == 1) P.owner = 3;     /* a finger mid-sheet goes inert until release */
    const nos_screen *s = top_screen();
    diag_log_event(DIAG_ACTION, s ? (int32_t)diag_log_name_id(s->name) : 0, t, 0);
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
    if (s_btn_raw && !s_btn_down) {
        s_btn_down = 1; s_btn_t0 = now; s_btn_long_sent = 0;
        /* a quick second press acts on its down (the launcher opens the last app): its
         * release is then spent */
        if (s_btn_pending && now - s_btn_last_up < DOUBLE_MS) {
            s_btn_pending = 0; s_btn_long_sent = 1;
            dispatch_btn(IN_BTN_DOUBLE);
        }
    }
    else if (!s_btn_raw && s_btn_down) {
        s_btn_down = 0;
        /* act at once on every press; a second press within DOUBLE_MS adds IN_BTN_DOUBLE
         * to whatever is on top by then */
        if (!s_btn_long_sent) { s_btn_pending = 1; s_btn_last_up = now; dispatch_btn(IN_BTN); }
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
static uint32_t s_wait_acc;     /* this frame's stripe waits: the bus, not the CPU */
static void (*s_flush)(uint16_t *, int, int, int, int, int);
static uint16_t *s_scratch;
static int (*s_exposure_prepare)(const gfx_fb *, int, int, int, int, int);
static uint32_t (*s_exposure_finish)(void);
static int s_exposure_pending;

void nos_set_stripes(uint16_t *a, uint16_t *b, int rows, void (*wait_free)(void),
                     void (*flush)(uint16_t *, int, int, int, int, int))
{
    s_stripe[0] = a; s_stripe[1] = b; s_stripe_h = rows; s_wait_free = wait_free; s_flush = flush;
}
void nos_set_scratch(uint16_t *buf) { s_scratch = buf; }
void nos_set_stripe_exposure(int (*prepare)(const gfx_fb *, int, int, int, int, int),
                            uint32_t (*finish)(void))
{
    s_exposure_prepare = prepare && finish ? prepare : NULL;
    s_exposure_finish = prepare && finish ? finish : NULL;
}

/* Dim: keep k/32 of every channel, rounded per lane (as gfx_ink): neutral greys stay
 * neutral without a per-pixel branch. One multiply per lit pixel. */
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
            d = ((d * (uint32_t)k + 0x02008010u) >> 5) & 0x07E0F81Fu;   /* as gfx_ink */
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

void nos_ink_dim(gfx_fb *fb, float k)
{
    int keep = (int)lroundf((32 - fb->dim) * m_clamp(k, 0, 1));
    fb->dim = 32 - keep;
}

int nos_subview(const gfx_fb *fb, gfx_fb *out, int x0, int y0, int x1, int y1, int ox, int oy)
{
    return sub_fb(fb, out, x0, y0, x1, y1, ox, oy);
}

static void clear_sub(gfx_fb *fb)
{
    if (fb->rec) return;
    NOS_COST_SCOPE(NOS_COST_CLEAR,1);
    for (int r = 0; r < fb->h; r++)
        memset(gfx_row(fb, r) + fb->cx0, 0, (size_t)(fb->cx1 - fb->cx0) * 2);
    GFX_COST_PX(GFX_COST_FILL, fb->h * (fb->cx1 - fb->cx0));   /* sim cost model only */
}

/* Screen backdrop dim is a FINAL composited RGB565 operation. Local ink
 * dim inside draw callbacks remains part of the sharp screen raster. Keeping
 * the same final operation on cache hits and live/failure paths makes AA and
 * overlapping primitives independent of cache availability. Debug postdim
 * forces the live reference path for pixel comparisons with the cache path. */
int nos_debug_post_dim;
static void dim_composite(gfx_fb *fb, int keep)
{
    if (fb->rec || keep >= 32) return;
    gfx_fb ink = {0}; ink.dim = 32 - keep;
    for (int row = 0; row < fb->h; row++) {
        uint16_t *p = gfx_row(fb, row);
        for (int x = fb->cx0; x < fb->cx1; x++)
            if (p[x]) p[x] = gfx_ink(&ink, p[x]);
    }
    GFX_COST_PX(GFX_COST_DIM, fb->h * (fb->cx1 - fb->cx0));
}
static void draw_screen(const gfx_fb *fb, const nos_screen *s, int x0, int y0, int x1, int y1,
                        int ox, int oy, int dimk)
{
    gfx_fb sub;
    if (!s || !sub_fb(fb, &sub, x0, y0, x1, y1, ox, oy)) return;
    int keep = dimk < 0 ? 0 : dimk > 32 ? 32 : dimk;
    if (sub.dim) keep = ((32 - sub.dim) * keep + 16) >> 5;
    if ((s->flags & SCR_NATIVE_CACHE) && !nos_debug_post_dim) {
        /* These renderers already replay their own caches with ink dim. Keep
         * that path: a second full-pixel dim pass defeats the Orrery's replay
         * optimization and duplicates work on specialized list sprites. */
        sub.dim = 32 - keep;
        if (!(s->flags & SCR_OPAQUE_BG)) clear_sub(&sub);
        if (s->draw) s->draw(&sub);
        return;
    }
    sub.dim = 0; /* sharp callbacks retain their own local ink scaling */
    /* A translated source may cover only part of the destination clip.
     * Clear those views before the cache copy; skip the clear only when the
     * retained raster is guaranteed to replace the whole destination. */
    int covers = sub.cx0 >= sub.ox && sub.cx1 <= sub.ox + GFX_W &&
                 sub.y_base >= sub.oy && sub.y_base + sub.h <= sub.oy + GFX_H;
    int precleared = !covers && !(s->flags & SCR_OPAQUE_BG);
    if (precleared) clear_sub(&sub);
    gfx_fb cached = sub; cached.dim = 32 - keep;
    if (s->draw && !nos_debug_post_dim && !(s->flags & SCR_LIVE_RENDER) &&
        nos_layers_draw(&cached, s)) return;
    if (!precleared && !(s->flags & SCR_OPAQUE_BG)) clear_sub(&sub);
    if (s->draw) s->draw(&sub);
    if (sub.rec) {
        /* Record screen-space final dim even when a callback's nominal ink
         * stays unchanged. Translation is already represented by the clip. */
        gfx_fb marker = sub; marker.ox = marker.oy = marker.dim = 0;
        int top = y0 > fb->y_base ? y0 : fb->y_base;
        int bottom = y1 < fb->y_base + fb->h ? y1 : fb->y_base + fb->h;
        gfx_rec_cmd(&marker, sub.cx0, top, sub.cx1, bottom,
                    gfx_hash_u(0xc09d0517u, (unsigned)keep));
    } else {
        gfx_fb final;
        /* Scale only pixels owned by this translated source. An opaque view
         * can intentionally leave destination pixels outside its source alone. */
        if (sub_fb(&sub, &final, sub.ox, sub.oy, sub.ox + GFX_W, sub.oy + GFX_H, 0, 0))
            dim_composite(&final, keep);
    }
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

/* Dissolve: an 8x8 ordered dither hands pixels from `under` to `over` as p grows. The
 * layer that owns most pixels this frame draws straight into the stripe; the other one
 * goes to the scratch rows and only its pixels are copied in, at most half the screen,
 * from a per-row list of dither phases (no per-pixel compare). */
static void dissolve_draw(const gfx_fb *fb, const nos_screen *under, const nos_screen *over, float p)
{
    int thr = (int)(p * 64.0f + 0.5f);
    if (!over || thr <= 0) { draw_screen(fb, under, 0, 0, GFX_W, GFX_H, 0, 0, 32); return; }
    if (thr >= 64 || !s_scratch) { draw_screen(fb, thr >= 32 ? over : under, 0, 0, GFX_W, GFX_H, 0, 0, 32); return; }
    if (fb->rec) {
        draw_screen(fb, under, 0, 0, GFX_W, GFX_H, 0, 0, 32);
        if (over->draw) { gfx_fb f = *fb; over->draw(&f); }
        return;
    }
    int maj_over = thr > 32;
    const nos_screen *base = maj_over ? over : under, *top = maj_over ? under : over;
    uint32_t q = hal_micros();
    draw_screen(fb, base, 0, 0, GFX_W, GFX_H, 0, 0, 32);
    uint32_t q1 = hal_micros();
    nos_dbg_us[0] += q1 - q;
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    gfx_fb o;
    scratch_fb(fb, &o, c0, c1);
    uint32_t q2 = hal_micros();
    if (top && top->draw) top->draw(&o);
    uint32_t q3 = hal_micros();
    nos_dbg_us[1] += q3 - q2;
    unsigned sel = 0;                                /* sim cost model only */
    for (int r = 0; r < fb->h; r++) {
        int y = fb->y_base + r;
        const uint8_t *b = BAYER8[y & 7];
        int js[8], nj = 0;
        for (int j = 0; j < 8; j++) if ((b[j] < thr) != maj_over) js[nj++] = j;
        if (!nj) continue;
        uint16_t *d = gfx_row(fb, r), *src = o.px + (size_t)r * GFX_W;
        /* whole 8-blocks: one strided pass per phase, no bounds tests (~18 -> ~4 cycles
         * a pixel on the device); the partial blocks at the clip edges test each x */
        int xa = (c0 + 7) & ~7, xb = c1 & ~7;
        if (xa > xb) xa = xb = c1;
        for (int k = 0; k < nj; k++) {
            int j = js[k];
            for (int x = xa + j; x < xb; x += 8) d[x] = src[x];
            for (int x = (c0 & ~7) + j; x < xa; x += 8) if (x >= c0) d[x] = src[x];
            for (int x = xb + j; x < c1; x += 8) d[x] = src[x];
        }
        sel += (unsigned)((c1 - c0) * nj / 8);
    }
    GFX_COST_PX(GFX_COST_SELECT, sel);
    nos_dbg_us[2] += hal_micros() - q3;
}

/* Iris. The new screen shows through a circle growing from the tapped icon, its edge
 * the same 2 px hairline that leads a sheet: flat, no dither (§1.4). Inside the circle
 * only `over` can be seen and outside only `under`. Under draws disjoint visible
 * column bounds into the internal stripe, skipping the common inner chord.
 * Save its edge spans to scratch (PSRAM on the board), then
 * draw over across the circle's columns and restore/blend the saved pixels.
 * Fully covered stripes skip under; stripes outside the circle draw it directly.
 * Motion blur (nos_iris_blur): the edge swept the band between last frame's radius and
 * this one; a pixel there showed `over` for the part of the frame the edge was past it,
 * so the two screens blend by (Rout - d) / (Rout - Rin), in rings of ~2.5 px (one square
 * root per ring and row, a 16-bit blend per pixel). The hairline rim is smeared with
 * it: a grey glow brightest at the leading edge, fading out across the band (on black
 * screens the blend alone would leave no edge to see). Opening and closing alike: the
 * band is wide while the edge is fast and narrows as it lands. */
#define IRIS_ROWS 16
#define IRIS_RIM GFX_RGB(51, 51, 51)
/* The tapped icon over the opening circle, fading out by 40 % (nos_iris_seed). */
static void iris_seed(const gfx_fb *fb, float p)
{
    if (!nos_iris_seed || T.popping || p >= IRIS_BUILD) return;
    gfx_fb e = *fb;
    nos_ink_dim(&e, 1.0f - p / IRIS_BUILD);
    float r = nos_iris_seed_r, cx = (float)nos_iris_x, cy = (float)nos_iris_y;
    ui_circle(&e, cx, cy, r, C_SURF2);
    dot_icon(&e, nos_iris_seed, cx, cy, r * 0.118f, r * 0.094f, C_WHITE);
}
/* ---- motion blur for whole-screen moves (nos.h) ----
 * RGB565 expanded to 0x07E0F81F form: each field has 5-6 spare bits, so up to 32 pixels
 * sum in one 32-bit word and a power-of-two box divides with one shift. */
#include "mblur.h"
#include "pblur.h"
#define MB_MASK 0x07E0F81Fu
static inline uint32_t mb_x(uint16_t c) { return ((uint32_t)c | (uint32_t)c << 16) & MB_MASK; }
static inline uint16_t mb_p(uint32_t m) { m &= MB_MASK; return (uint16_t)(m | m >> 16); }
/* Bulk scratch must never consume BLE's internal heap. hal_big_alloc is strictly
 * MALLOC_CAP_SPIRAM on the board; failure disables live blur, not the UI. */
static struct mb_work {
    uint32_t line[GFX_W + 40];
    struct { unsigned key; uint32_t frame; mb_h4_state state; } h4[2];
    struct { unsigned key; uint32_t frame; mb_v_columns_state state; } vertical[2];
} *s_mb_work;
static int mb_work_ready(void)
{
    if (!s_mb_work) {
        s_mb_work = hal_big_alloc(sizeof *s_mb_work);
        if (s_mb_work) memset(s_mb_work, 0, sizeof *s_mb_work);
    }
    return s_mb_work != 0;
}
#define s_mb_line (s_mb_work->line)
#define s_mb_h4   (s_mb_work->h4)
#define s_mb_vertical (s_mb_work->vertical)
/* the budget guard: blur time spent this frame and the last, and the last cost seen */
static uint32_t s_mb_us, s_mb_us_last, s_mb_acc_frame, s_mb_est = 9000u;
static uint32_t s_mb_dec_frame = 0xFFFFFFFFu;
static int s_mb_dec;
static void mb_account(uint32_t us)
{
    if (s_mb_acc_frame != nos_frame) {         /* a new frame: last frame's total moves over */
        s_mb_us_last = s_mb_acc_frame + 1 == nos_frame ? s_mb_us : 0;
        if (s_mb_us_last) s_mb_est = s_mb_us_last;
        s_mb_us = 0;
        s_mb_acc_frame = nos_frame;
    }
    s_mb_us += us;
}
static uint32_t mb_elapsed(uint32_t start, unsigned ns)
{
    GFX_COST_PX(GFX_COST_DIM, (ns + 7u) / 8u);
#ifdef GFX_COST_MODEL
    (void)start;
    return (ns + 999u) / 1000u;
#else
    return hal_micros() - start;
#endif
}
/* last frame's CPU raster without its blur: what this frame needs before any blur. Stripe
 * waits are left out: core 0 spends them idle while core 1 and the bus send, so blur work
 * fills them (measured: a full repaint's raster is ~23 ms of which most is waiting on the
 * 23.5 ms bus; counting the waits refused every pager, drawer and app blur on the watch) */
static uint32_t mb_base(void)
{
    uint32_t cpu = nos_last.raster_us > nos_last.wait_us ? nos_last.raster_us - nos_last.wait_us : 0;
    return cpu > s_mb_us_last ? cpu - s_mb_us_last : 0;
}
int nos_mblur_afford(void)
{
    if (s_mb_dec_frame == nos_frame) return s_mb_dec;
    mb_account(0);                             /* rolls the totals over on a new frame */
    uint32_t base = mb_base();
    s_mb_dec = base + s_mb_est <= NOS_MBLUR_BUDGET_US;
    s_mb_dec_frame = nos_frame;
    return s_mb_dec;
}

float nos_mblur_step(float step, int held)
{
    if (!nos_mblur || (held && nos_mblur < 2)) return 0.0f;
    /* Eligibility only. Admission needs the region area and fidelity, which are
     * not known here. In particular, don't veto the first moving frame. */
    /* MOTION30 kernels describe an exposure in seconds. Preserve that shutter
     * when the presentation cadence changes or a frame spans several ticks. */
    return nos_dt > 0.0f ? fabsf(step) / (30.0f * nos_dt) : 0.0f;
}
float nos_mb_track_step(nos_mb_track *t, float pos)
{
    if (t->frame != nos_frame) {
        t->step = t->frame + 1 == nos_frame ? pos - t->last : 0.0f;
        t->last = pos;
        t->frame = nos_frame;
    }
    return t->step;
}
static int mb_len_log2(float step)              /* box length 1 << k, ~half the step */
{
    return step >= 64.0f ? 5 : step >= 32.0f ? 4 : step >= 16.0f ? 3 : step >= 8.0f ? 2 : 0;
}
static void mb_clip(const gfx_fb *fb, int x0, int x1, int *c0, int *c1)
{
    int a = fb->cx0, b = fb->cx1;
    if (b <= a) { a = 0; b = GFX_W; }
    if (a < 0) a = 0;
    if (b > GFX_W) b = GFX_W;
    if (x0 + fb->ox > a) a = x0 + fb->ox;
    if (x1 + fb->ox < b) b = x1 + fb->ox;
    *c0 = a; *c1 = b;
}
static int mb_region_ok(int kind, int x0, int y0, int x1, int y1, int reduced_only);
static void mb_learn(int kind, uint32_t us, int area);
static uint32_t s_mb_record_epoch;
static uint32_t s_mb_pixels[2];
uint32_t nos_mblur_record_epoch(void) { return s_mb_record_epoch; }
static void mb_h(gfx_fb *fb, int x0, int x1, float step, int reduced_only)
{
    int k = mb_len_log2(step);
    if (s_band_enabled && fb->rec && k && x1 > x0) s_band_stream_current = 1;
    if (!k || x1 <= x0 || !mb_work_ready()) return;
    int mode = mb_region_ok(0, x0, 0, x1, GFX_H, reduced_only);
    if (!mode) return;
    if (mode == 2 && k > 4) k = 4; /* shorter fallback, matching the baked 16 px box */
    if (fb->rec) {                              /* the smear spreads into the black around */
        s_mb_record_epoch++;
        /* new every frame: the smear reaches past the commands that moved, so the whole
         * band repaints while it blurs (and once more as the blur ends) */
        gfx_rec_cmd(fb, x0, 0, x1, GFX_H, gfx_hash_u(gfx_hash_u(0x3B1Eu, (unsigned)k), nos_frame));
        return;
    }
    int c0, c1;
    mb_clip(fb, x0, x1, &c0, &c1);
    if (c1 <= c0) return;
    int L = 1 << k, h = L / 2;
    uint32_t t_mb = hal_micros();
    unsigned n = 0, scan = 0;                   /* sim cost model only */
    if (mode == 2) {
        unsigned key = gfx_hash_u((unsigned)c0, (unsigned)c1);
        int slot = -1;
        for (int i = 0; i < 2; i++)
            if (s_mb_h4[i].frame == nos_frame && s_mb_h4[i].key == key) slot = i;
        if (slot < 0) {
            slot = s_mb_h4[0].frame != nos_frame ? 0 : 1;
            s_mb_h4[slot].frame = nos_frame; s_mb_h4[slot].key = key;
            s_mb_h4[slot].state.next = -1;
        }
        mb_h4_rows(fb, c0, c1, k, s_mb_line, &s_mb_h4[slot].state);
        int area = fb->h * (c1 - c0);
        uint32_t us = mb_elapsed(t_mb, 35u * (unsigned)area);
        mb_account(us); mb_learn(2, us, area);
        s_mb_pixels[0]+=(uint32_t)area;
        ui_fblur_coverage_claim(fb, x0, 0, x1, GFX_H);
        return;
    }
    for (int r = 0; r < fb->h; r++) {
        uint16_t *d = gfx_row(fb, r);
        int a = c0, b = c1 - 1;
        while (a < c1 && !d[a]) a++;
        scan += (unsigned)(a - c0);
        if (a == c1) continue;                  /* a black row stays black */
        while (!d[b]) b--;
        int o0 = a - h < c0 ? c0 : a - h, o1 = b + h + 1 > c1 ? c1 : b + h + 1;
        int base = o0 - h;                      /* s_mb_line[i] = pixel base + i */
        for (int i = 0, x = base; x < o1 + h; i++, x++) s_mb_line[i] = x >= c0 && x < c1 ? mb_x(d[x]) : 0;
        uint32_t sum = 0, rnd = (uint32_t)h * 0x00200801u;   /* + L/2 in every field: rounds */
        for (int i = 0; i < L; i++) sum += s_mb_line[i];   /* the window [x - h, x + h) */
        const uint32_t *e = s_mb_line - base;
        for (int x = o0; x < o1; x++) {
            d[x] = mb_p((sum + rnd) >> k);
            sum -= e[x - h];                    /* out first: a field never holds L + 1 */
            sum += e[x + h];
        }
        n += (unsigned)(o1 - o0);
    }
    uint32_t us = mb_elapsed(t_mb, 95u * n + 4u * scan);
    mb_account(us);
    mb_learn(0, us, fb->h * (c1 - c0));
    s_mb_pixels[0]+=(uint32_t)(fb->h*(c1-c0));
    ui_fblur_coverage_claim(fb, x0, 0, x1, GFX_H);
}
void nos_mblur_h(gfx_fb *fb, int x0, int x1, float step) { mb_h(fb, x0, x1, step, 0); }
void nos_mblur_h_reduced(gfx_fb *fb, int x0, int x1, float step) { mb_h(fb, x0, x1, step, 1); }
void nos_mblur_v(gfx_fb *fb, int y0, int y1, float step) { nos_mblur_v_rect(fb, 0, y0, GFX_W, y1, step); }
/* Per-region budget decision, once per frame: the region's area times the measured cost
 * per pixel of its kind, on top of the frame's estimated raster without blur and what
 * this frame has already committed to. kind 0 horizontal, 1 vertical. */
static struct { uint32_t frame; unsigned key; int ok; } s_mb_dec_tab[8];
static uint32_t s_mb_committed, s_mb_commit_frame;
static uint32_t s_mb_region_last = 0xFFFFFFF0u, s_mb_base_frame, s_mb_region_base;
static uint32_t s_mb_full_base = 18000u;
static int s_mb_full_measured;
static uint32_t s_mb_live_frame;
static const nos_screen *s_mb_live_owner;
uint32_t nos_blur_stat[NOS_BS_N][4], nos_mblur_gate[2];
void nos_blur_note(int kind, int moving, int fast, int blurred)
{
    static uint32_t s_seen[NOS_BS_N] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    if (kind < 0 || kind >= NOS_BS_N || !moving || s_seen[kind] == nos_frame) return;
    s_seen[kind] = nos_frame;
    nos_blur_stat[kind][0]++;
    if (fast) nos_blur_stat[kind][1]++;
    if (fast) nos_blur_stat[kind][blurred ? 2 : 3]++;
}
uint32_t nos_mblur_count[2][2];                  /* [h, v][skipped, ran] region-frames */
uint32_t nos_mblur_reduced[2];
/* H seeds measured on 1.43-B. Both V modes now use full columns; their
 * provisional 150/100 ns/pixel estimates are UNMEASURED for this new kernel.
 * Separate conservative learners remain until target calibration. */
static float s_mb_ns[4] = { 100.0f, 150.0f, 35.0f, 100.0f };
static int mb_region_ok(int kind, int x0, int y0, int x1, int y1, int reduced_only)
{
    unsigned key = gfx_hash_u(gfx_hash_u(gfx_hash_u(gfx_hash_u((unsigned)(kind + 2 * reduced_only), (unsigned)x0), (unsigned)y0), (unsigned)x1), (unsigned)y1);
    for (int i = 0; i < 8; i++)
        if (s_mb_dec_tab[i].frame == nos_frame && s_mb_dec_tab[i].key == key) return s_mb_dec_tab[i].ok;
    if (s_mb_commit_frame != nos_frame) { s_mb_commit_frame = nos_frame; s_mb_committed = 0; }
    mb_account(0);
    if (s_mb_base_frame != nos_frame) {
        uint32_t base = mb_base();
        /* Only the immediately preceding substantial LIVE repaint of this
         * screen measures its sharp cost. Partial/cached/transition frames and
         * a different owner invalidate that calibration. The conservative
         * seed otherwise makes full-screen V permanently unaffordable:
         * Even the former 50 ns/pixel fallback could not fit beside 19 ms. */
        s_mb_full_measured = s_mb_live_frame + 1 == nos_frame &&
            s_mb_live_owner && s_mb_live_owner == top_screen() &&
            T.state == TXS_IDLE &&
            nos_last.dirty_px >= GFX_W * GFX_H * 3u / 4u &&
            !nos_last.layer_bands && !nos_last.layer_reused;
        s_mb_full_base = s_mb_full_measured ? base : 18000u;
        if (s_mb_region_last + 1 != nos_frame && base < s_mb_full_base) base = s_mb_full_base;
        s_mb_region_base = base; s_mb_base_frame = nos_frame;
    }
    s_mb_region_last = nos_frame;
    uint32_t base = s_mb_region_base + nos_last.record_us + 1000u;
    /* A cached previous frame understates the sharp render on a cache miss. */
    if ((x1 - x0) * (y1 - y0) > GFX_W * GFX_H / 2 && base < 19000u && !s_mb_full_measured) base = 19000u;
    uint32_t cost = (uint32_t)(s_mb_ns[kind] * (float)((x1 - x0) * (y1 - y0)) / 1000.0f);
    int ok = !reduced_only && base + s_mb_committed + cost <= NOS_MBLUR_BUDGET_US ? 1 : 0;
    if (!ok) {
        cost = (uint32_t)(s_mb_ns[kind + 2] * (float)((x1 - x0) * (y1 - y0)) / 1000.0f);
        if (base + s_mb_committed + cost <= NOS_MBLUR_BUDGET_US) ok = 2;
    }
    ok=quality_decide(kind,key,ok);
    if(ok) {
        cost=(uint32_t)(s_mb_ns[kind+(ok==2?2:0)] *
                        (float)((x1-x0)*(y1-y0))/1000.0f);
        s_mb_committed+=cost;
    }
    nos_mblur_count[kind][ok != 0]++;
    if (ok == 2) nos_mblur_reduced[kind]++;
    if (!ok) nos_mblur_gate[1]++;             /* region-frames, never stripe invocations */
    int slot = 0;
    for (int i = 0; i < 8; i++) if (s_mb_dec_tab[i].frame != nos_frame) { slot = i; break; }
    s_mb_dec_tab[slot].frame = nos_frame; s_mb_dec_tab[slot].key = key; s_mb_dec_tab[slot].ok = ok;
    return ok;
}
/* learn the cost per area pixel from what a pass really took (watch only: the sim's
 * PC timings mean nothing and would make runs differ) */
float nos_mblur_ns(int kind) { return s_mb_ns[kind & 3]; }
static void mb_learn(int kind, uint32_t us, int area)
{
#ifndef GFX_COST_MODEL
    if (area > 2000) {
        float ns = (float)us * 1000.0f / (float)area;
        s_mb_ns[kind] = s_mb_ns[kind] * 0.7f + ns * 0.3f;
        if (ns > s_mb_ns[kind]) s_mb_ns[kind] = ns;    /* a slow one counts at once */
    }
#else
    (void)kind; (void)us; (void)area;
#endif
}

/* Both admitted vertical modes use the same full-column spatial EMA. Separate
 * admission/cost learners remain conservative until target calibration, but a
 * more generous budget cannot reduce image quality. Two logical regions retain
 * independent per-column continuity; additional regions reset safely. */
static void mb_v(gfx_fb *fb, int x0, int y0, int x1, int y1, float step, int reduced_only)
{
    if (s_band_enabled && fb->rec && step >= 8.0f && y1 > y0 && x1 > x0)
        s_band_stream_current = 1;
    if (step < 8.0f || y1 <= y0 || x1 <= x0 || !mb_work_ready()) return;
    int mode = mb_region_ok(1, x0, y0, x1, y1, reduced_only);
    if (!mode) return;
    if (fb->rec) {
        s_mb_record_epoch++;
        gfx_rec_cmd(fb, x0, y0, x1, y1, gfx_hash_u(gfx_hash_f(0x3B1Fu, step), nos_frame));
        return;
    }
    int c0, c1;
    mb_clip(fb, x0, x1, &c0, &c1);
    if (c1 <= c0) return;
    float tau = step * 0.25f;                   /* the tail's length scale, rows */
    int k = tau >= 11.0f ? 4 : tau >= 5.5f ? 3 : tau >= 2.8f ? 2 : 1;
    uint32_t t_mb = hal_micros();
    /* Physical round-panel clips change between stripes; the logical region
     * owns state. Sharing it across modes also preserves an in-frame tail. */
    unsigned key = gfx_hash_u(gfx_hash_u(gfx_hash_u(gfx_hash_u(1u, (unsigned)x0), (unsigned)x1), (unsigned)y0), (unsigned)y1);
    int slot = -1;
    for (int i = 0; i < 2; i++)
        if (s_mb_vertical[i].frame == nos_frame && s_mb_vertical[i].key == key) slot = i;
    if (slot < 0) {
        slot = s_mb_vertical[0].frame != nos_frame ? 0 : 1;
        s_mb_vertical[slot].frame = nos_frame; s_mb_vertical[slot].key = key;
        mb_v_columns_reset(&s_mb_vertical[slot].state);
    }
    mb_v_columns(fb, c0, c1, y0, y1, k, &s_mb_vertical[slot].state);
    int a = fb->y_base - fb->oy, b = a + fb->h;
    if (a < y0) a = y0;
    if (b > y1) b = y1;
    unsigned area = b > a ? (unsigned)((b - a) * (c1 - c0)) : 0;
    uint32_t us = mb_elapsed(t_mb, (mode == 2 ? 100u : 150u) * area);
    mb_account(us);
    mb_learn(mode == 2 ? 3 : 1, us, (int)area);
    s_mb_pixels[1]+=area;
    ui_fblur_coverage_claim(fb, x0, y0, x1, y1);
}

void nos_mblur_v_rect(gfx_fb *fb, int x0, int y0, int x1, int y1, float step)
{ mb_v(fb, x0, y0, x1, y1, step, 0); }
void nos_mblur_v_reduced(gfx_fb *fb, int y0, int y1, float step)
{ mb_v(fb, 0, y0, GFX_W, y1, step, 1); }
int nos_mblur_v_fallback(gfx_fb *fb, int x0, int y0, int x1, int y1, float step)
{
    if (step < 8 || !mb_work_ready() || !mb_region_ok(1, x0, y0, x1, y1, 1)) return 0;
    mb_v(fb, x0, y0, x1, y1, step, 1);
    return 1;
}

/* row r's chord of the circle of radius rad around (fx, fy), clipped to [c0, c1);
 * empty (x0 = x1 = c1) if the row misses it */
static void iris_chord(float dy2, float fx, float rad, int c0, int c1, int *x0, int *x1)
{
    *x0 = *x1 = c1;
    if (dy2 >= rad * rad) return;
    float hw = gfx_fast_sqrt(rad * rad - dy2);       /* libm sqrtf is ~1000 cycles here */
    int a = (int)(fx - hw + 0.5f), b = (int)(fx + hw + 0.5f);
    if (a < c0) a = c0;
    if (b > c1) b = c1;
    if (b < a) b = a;
    *x0 = a; *x1 = b;
}
/* d = (d * w + s * (32 - w)) / 32 over n RGB565 pixels, all three channels at once */
static void iris_blend(uint16_t *d, const uint16_t *s, int n, uint32_t w)
{
    for (int i = 0; i < n; i++) {
        uint32_t a = d[i], b = s[i];
        a = (a | a << 16) & 0x07E0F81Fu;
        b = (b | b << 16) & 0x07E0F81Fu;
        uint32_t m = ((a * w + b * (32u - w)) >> 5) & 0x07E0F81Fu;
        d[i] = (uint16_t)(m | m >> 16);
    }
}
/* d = mix(d, c, w / 32): the smeared rim's glow over a run */
static void iris_tint(uint16_t *d, int n, uint16_t c, uint32_t w)
{
    uint32_t b = ((uint32_t)c | (uint32_t)c << 16) & 0x07E0F81Fu;
    b *= w;
    for (int i = 0; i < n; i++) {
        uint32_t a = d[i];
        a = (a | a << 16) & 0x07E0F81Fu;
        uint32_t m = ((a * (32u - w) + b) >> 5) & 0x07E0F81Fu;
        d[i] = (uint16_t)(m | m >> 16);
    }
}
#define IRIS_BLUR_MAXL 20                            /* blend rings at most */
#define IRIS_GLOW GFX_RGB(160, 160, 160)             /* the smeared rim, at its leading edge */
#define IRIS_GLOW_W 13                               /* its weight there (of 32); 0 at the tail */
static void iris_draw(const gfx_fb *fb, const nos_screen *under, const nos_screen *over,
                      float p, float pp, int dimk)
{
    float fx = (float)nos_iris_x, fy = (float)nos_iris_y;
    /* the glass is a circle: the farthest visible pixel is centre distance + radius */
    float far = gfx_fast_sqrt((fx - GFX_CX) * (fx - GFX_CX) + (fy - GFX_CY) * (fy - GFX_CY)) + GFX_W * 0.5f + 2.0f;
    const float R0 = 36.0f;                          /* the tapped icon is the seed, not a hole */
    float R = R0 + p * (far - R0), Rp = R0 + pp * (far - R0);
    /* the band the edge swept this frame; under 3 px it is just the edge, with its rim */
    float Ro = R > Rp ? R : Rp, Ri = R > Rp ? Rp : R;
    int blur = nos_iris_blur && Ro - Ri >= 3.0f && nos_mblur_afford();
    blur=quality_decide(2,0,blur);
    if (!blur) Ro = Ri = R;
    if (fb->rec) {
        draw_screen(fb, under, 0, 0, GFX_W, GFX_H, 0, 0, dimk);
        if (over && over->draw) {
            gfx_fb f = *fb;
            over->draw(&f);
            if (!blur) gfx_ring(&f, fx, fy, R, 2.0f, IRIS_RIM);
            else gfx_rec_cmd(&f, (int)(fx - Ro - 1), (int)(fy - Ro - 1), (int)(fx + Ro + 2), (int)(fy + Ro + 2),
                             gfx_hash_f(gfx_hash_f(0x1B1Bu, Ro), Ri));   /* the band changes every frame */
        }
        return;
    }
    if (fb->h > IRIS_ROWS) {                         /* sim full frames: 16-row bands */
        for (int r0 = 0; r0 < fb->h; r0 += IRIS_ROWS) {
            gfx_fb sub;
            int y0 = fb->y_base + r0, y1 = y0 + IRIS_ROWS;
            if (y1 > fb->y_base + fb->h) y1 = fb->y_base + fb->h;
            if (sub_fb(fb, &sub, 0, y0, GFX_W, y1, 0, 0)) iris_draw(&sub, under, over, p, pp, dimk);
        }
        return;
    }
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }

    /* per-row spans [a0, a1) inside the outer circle (over is drawn there) and [b0, b1)
     * inside the inner one (over alone); the union of the first, the common interior of
     * the second (no under needed there) */
    int16_t a0[IRIS_ROWS], a1[IRIS_ROWS], b0[IRIS_ROWS], b1[IRIS_ROWS];
    int A0 = c1, A1 = c0, I0 = c0, I1 = c1;
    for (int r = 0; r < fb->h; r++) {
        float dy = (fb->y_base + r + 0.5f) - fy, dy2 = dy * dy;
        int x0 = c1, x1 = c1, y0 = c1, y1 = c1;
        if (over) {
            iris_chord(dy2, fx, Ro, c0, c1, &x0, &x1);
            if (blur) iris_chord(dy2, fx, Ri, c0, c1, &y0, &y1);
            else { y0 = x0; y1 = x1; }
        }
        a0[r] = (int16_t)x0; a1[r] = (int16_t)x1;
        b0[r] = (int16_t)y0; b1[r] = (int16_t)y1;
        if (x1 > x0) { if (x0 < A0) A0 = x0; if (x1 > A1) A1 = x1; }
        if (y0 > I0) I0 = y0;
        if (y1 < I1) I1 = y1;
    }
    if (I1 <= I0) I0 = I1 = c0;                      /* no column is inside on every row */

    uint32_t q = hal_micros();
    if (!over || A1 <= A0) {
        draw_screen(fb, under, c0, 0, c1, GFX_H, 0, 0, dimk);
        nos_dbg_us[0] += hal_micros() - q;
        if (!over) return;
    } else {
        /* Disjoint clips: each visible pixel is rasterised at most once,
         * internally, never into PSRAM scratch. The common inner chord needs
         * no under pixels. Keep conservative 16-row bounds at the curved edge
         * so primitive/index setup is not repeated for every single row. */
        int span[2][2] = { { A0, I1 > I0 ? I0 : A1 }, { I1 > I0 ? I1 : A1, A1 } };
        gfx_fb o = *fb;
        o.px = s_scratch; o.stride = 0; o.x_base = 0;
        unsigned saved = 0;
        if (s_scratch) {
            if (I1 > I0) {
                draw_screen(fb, under, c0, 0, I0, GFX_H, 0, 0, dimk);
                draw_screen(fb, under, I1, 0, c1, GFX_H, 0, 0, dimk);
            } else draw_screen(fb, under, c0, 0, c1, GFX_H, 0, 0, dimk);
            for (int k = 0; k < 2; k++) {
                int x0 = span[k][0], n = span[k][1] - x0;
                if (n <= 0) continue;
                for (int r = 0; r < fb->h; r++)
                    memcpy(o.px + (size_t)r * GFX_W + x0, gfx_row(fb, r) + x0, (size_t)n * 2);
                saved += (unsigned)(n * fb->h);
            }
        } else {
            draw_screen(fb, under, c0, 0, A0, GFX_H, 0, 0, dimk);
            draw_screen(fb, under, A1, 0, c1, GFX_H, 0, 0, dimk);
        }
        /* Provisional 60 ns/pixel for sequential PSRAM scratch writes. Unlike
         * preparation, this copy runs inside the stripe/transfer pipeline. */
        GFX_COST_PX(GFX_COST_SELECT, 10 * saved);
        uint32_t q1 = hal_micros();
        nos_dbg_us[0] += q1 - q;
        draw_screen(fb, over, A0, 0, A1, GFX_H, 0, 0, s_iris_overk);   /* once, straight in */
        uint32_t q2 = hal_micros();
        nos_dbg_us[1] += q2 - q1;
        unsigned sel = 0;                            /* sim cost model only */
        for (int k = 0; s_scratch && k < 2; k++) {
            int x0 = span[k][0], x1 = span[k][1];
            if (x1 <= x0) continue;
            for (int r = 0; r < fb->h; r++) {
                uint16_t *d = gfx_row(fb, r), *src = o.px + (size_t)r * GFX_W;
                int l1 = a0[r] < x1 ? a0[r] : x1, r0 = a1[r] > x0 ? a1[r] : x0;
                if (l1 > x0) { memcpy(d + x0, src + x0, (size_t)(l1 - x0) * 2); sel += (unsigned)(l1 - x0); }
                if (x1 > r0 && r0 >= l1) { memcpy(d + r0, src + r0, (size_t)(x1 - r0) * 2); sel += (unsigned)(x1 - r0); }
            }
        }
        GFX_COST_PX(GFX_COST_SELECT, sel);
        if (blur && s_scratch) {
            /* the band: ring k (radii Ro - k s .. Ro - (k+1) s) shows over at (k + 0.5) / n,
             * as the runs of its chord outside the next ring's chord */
            int n = (int)lroundf((Ro - Ri) / 2.5f);
            n = n < 2 ? 2 : n > IRIS_BLUR_MAXL ? IRIS_BLUR_MAXL : n;
            float st = (Ro - Ri) / (float)n;
            int lead_out = R >= Rp;                  /* opening: the leading edge is the outer one */
            unsigned bl = 0;                         /* sim cost model only */
            for (int r = 0; r < fb->h; r++) {
                if (a1[r] <= a0[r]) continue;
                float dy = (fb->y_base + r + 0.5f) - fy, dy2 = dy * dy;
                uint16_t *d = gfx_row(fb, r), *src = o.px + (size_t)r * GFX_W;
                int l0 = a0[r], r1 = a1[r];          /* ring k's chord */
                for (int k = 0; k < n; k++) {
                    int l1, r0;                      /* the next ring's chord (the inner one last) */
                    if (k == n - 1) { l1 = b0[r]; r0 = b1[r]; }
                    else iris_chord(dy2, fx, Ro - (float)(k + 1) * st, c0, c1, &l1, &r0);
                    uint32_t w = (uint32_t)((2 * k + 1) * 32 / (2 * n));
                    int kl = lead_out ? k : n - 1 - k;   /* rings from the leading edge */
                    uint32_t g = (uint32_t)(IRIS_GLOW_W * (n - kl) * (n - kl) / (n * n));
                    if (r0 <= l1) {                  /* the row misses the next ring: all of it */
                        if (r1 > l0) {
                            iris_blend(d + l0, src + l0, r1 - l0, w);
                            if (g) iris_tint(d + l0, r1 - l0, IRIS_GLOW, g);
                            bl += (unsigned)(r1 - l0);
                        }
                        break;
                    }
                    if (l1 > l0) {
                        iris_blend(d + l0, src + l0, l1 - l0, w);
                        if (g) iris_tint(d + l0, l1 - l0, IRIS_GLOW, g);
                        bl += (unsigned)(l1 - l0);
                    }
                    if (r1 > r0) {
                        iris_blend(d + r0, src + r0, r1 - r0, w);
                        if (g) iris_tint(d + r0, r1 - r0, IRIS_GLOW, g);
                        bl += (unsigned)(r1 - r0);
                    }
                    l0 = l1; r1 = r0;
                }
            }
            GFX_COST_PX(GFX_COST_DIM, 3 * bl);       /* blend + glow: ~3 dims a pixel */
            mb_account(hal_micros() - q2);           /* (with the copy-back: a little over) */
        }
        nos_dbg_us[2] += hal_micros() - q2;
    }
    if (!blur) {
        gfx_fb e = *fb;                              /* the rim hides the stair-step */
        gfx_ring(&e, fx, fy, R, 2.0f, IRIS_RIM);
    }
    iris_seed(fb, p);
}

static void draw_overlays(gfx_fb *fb)
{
    if (nos_locked && !nos_aod) dot_icon(fb, DOT_ICON_LOCK, UI_CX, 16, 1.7f, 1.4f, C_PRI);   /* locked */
    /* whole px: the card and its text move together (R6) */
    if (B.active && B.y.x > 0.5f) {
        float by = floorf(B.y.x - BANNER_H + 0.5f);
        ui_draw_banner(fb, B.title, B.body, B.icon, by,
                       B.tap_hold || (P.down && P.owner == 2 && nos_touch_travel() <= 8));
        /* the card's motion blur: its columns, down to a tail's length below it */
        static nos_mb_track mb;
        float s = nos_mblur_step(nos_mb_track_step(&mb, by), B.dragging);
        if (s > 0 && s < BANNER_H) {
            int top = (int)(UI_BANNER_TOP + by);
            nos_mblur_v_rect(fb, UI_CX - 152, top, UI_CX + 152, top + 136 + (int)(s * 0.6f), s);
        }
    }
    if (TO.active && !B.active && TO.off < TO.travel) ui_draw_toast(fb, TO.text, TO.off);
    nos_dev_draw(fb);
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
    if (T.variant == TXV_AXIS && T.state == TXS_AUTO && T.ax_s) {
        /* shared axis: one screen, shifted, in dimmed ink; the strip it uncovers is black */
        int ox = T.ax_ox, oy = T.ax_oy;
        gfx_fb c;
        if (ox && sub_fb(fb, &c, ox > 0 ? 0 : W + ox, 0, ox > 0 ? ox : W, H, 0, 0)) clear_sub(&c);
        if (oy && sub_fb(fb, &c, 0, oy > 0 ? 0 : H + oy, W, oy > 0 ? oy : H, 0, 0)) clear_sub(&c);
        draw_screen(fb, T.ax_s, ox > 0 ? ox : 0, oy > 0 ? oy : 0, ox < 0 ? W + ox : W, oy < 0 ? H + oy : H,
                    ox, oy, T.ax_k);
        draw_overlays(fb);
        return;
    }
    /* motion blur: how far the layers moved since the last drawn frame (p in screens) */
    static nos_mb_track mb_p;
    float mbs = nos_mblur_step(nos_mb_track_step(&mb_p, p), T.state == TXS_FINGER);
    if (mbs > 0.5f) mbs = 0;                         /* a new transition began: no move */
    switch (T.type) {
    case TX_PUSH: {
        int edge = (int)lroundf(W * (1.0f - p));          /* over's left edge */
        int uox = (int)lroundf(-W * 0.30f * p);           /* parallax */
        int dimk = 32 - (int)lroundf(16.0f * p);
        draw_screen(fb, T.under, 0, 0, edge, H, uox, 0, dimk);
        draw_screen(fb, T.over, edge, 0, W, H, edge, 0, 32);
        if (mbs > 0) {
            nos_mblur_h(fb, 0, edge, W * 0.30f * mbs);
            nos_mblur_h(fb, edge, W, W * mbs);
        }
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
        int cached = 0;
        gfx_fb sheet;
        if (T.over == &scr_control && H * mbs >= 8 &&
            nos_subview(fb, &sheet, 0, top, W, bot, 0, rise ? off : -off))
            cached = pblur_draw(&sheet, 6, 0, 0, H * mbs);
        if (!cached) {
            draw_screen(fb, T.over, 0, top, W, bot, 0, rise ? off : -off, 32);
            if (mbs > 0) {
                if (T.over == &scr_control) nos_mblur_v_reduced(fb, top, bot, H * mbs);
                else nos_mblur_v(fb, top, bot, H * mbs);
            }
        }
        /* the sheet's leading edge: a hairline across the glass and a grabber pill, so
         * the face reads as covered, not sliced */
        if (off > 0 && off < H) {
            int ey = rise ? top : bot - 2;
            gfx_fb e;
            if (nos_subview(fb, &e, 0, ey, W, ey + 2, 0, 0)) gfx_fill_rect(&e, 0, ey, W, 2, GFX_RGB(51, 51, 51));
            /* the grabber says "drag me back": not on screens that refuse it, and it
             * leaves with the last 30 % of the move instead of vanishing on arrival */
            float gk = m_clamp((1.0f - p) / 0.3f, 0, 1);
            gk = floorf(gk * 8.0f + 0.5f) / 8.0f;
            float gy = rise ? top + 14.0f : bot - 16.0f;
            if (gk > 0 && !(T.over && (T.over->flags & SCR_NOBACK)))
                gfx_fill_pill(fb, GFX_CX - 15, gy, GFX_CX + 15, 2.5f, ui_dim(GFX_RGB(102, 102, 102), gk));
        }
        break;
    }
    case TX_IRIS: case TX_DISSOLVE: {
        if (!s_scratch && T.type == TX_DISSOLVE) {   /* no second layer: cut at the midpoint */
            draw_screen(fb, p < 0.5f ? T.under : T.over, 0, 0, W, H, 0, 0, 32);
            break;
        }
        if (T.type == TX_IRIS) {
            /* a retargeted iris shows the replaced screen dimming out for RT_FRAMES first */
            const nos_screen *over = s_rt_k < 32 && s_rt_old ? s_rt_old : T.over;
            s_iris_overk = over == T.over ? 32 : s_rt_k;
            iris_draw(fb, T.under, p > 0.001f ? over : 0, p, m_clamp(T.p_prev, 0, 1), 32 - (int)lroundf(18.0f * p));
            s_iris_overk = 32;
            break;
        }
        dissolve_draw(fb, T.under, T.over, p);
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
/* Legacy bounding rectangles only: bounded12-slot HUD diagnostic, even when
 * actual band rendering emits more windows (reported separately in nos_last). */
int nos_dbg_rect[MAXRECTS][4], nos_dbg_nr;
static band_damage s_band_current, s_band_previous;
static int s_band_capture;
typedef char band_geometry_must_match[(GFX_W == BD_WIDTH && GFX_H == BD_HEIGHT) ? 1 : -1];
void nos_damage_bands_set(int enabled)
{
    enabled = !!enabled;
    if (enabled == s_band_enabled) return;
    s_band_enabled = enabled; s_band_capture = 0;
    band_damage_clear(&s_band_current); band_damage_clear(&s_band_previous);
    s_band_stream_current = s_band_stream_previous = 0;
    nos_invalidate(); /* mode boundary paints fully and retires previous exposure */
}
int nos_damage_bands_get(void) { return s_band_enabled; }
static int s_history_rects[MAXRECTS][4], s_history_nr;
static uint32_t s_history_us;

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
    if (s_band_capture) band_damage_add(&s_band_current, c->x0, c->y0, c->x1, c->y1);
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
        static struct { uint16_t tab[1024]; uint8_t used[GFX_MAX_CMDS]; } *match;
        if (!match) match = hal_big_alloc(sizeof *match);
        if (!match) return -1;               /* allocation failure: full damage */
        uint16_t *tab = match->tab;          /* index+1 into prev, 0 empty */
        uint8_t *used = match->used;
        memset(match, 0, sizeof *match);
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

static void apply_exposure(gfx_fb *fb, int x0, int y0, int x1, int y1)
{
    if(NOS_RENDER_SHARP)return;
    uint32_t start = hal_micros();
    if (!ui_fblur_any_active()) {
        nos_history_apply_disjoint(fb, x0, y0, x1, y1, nos_temporal_blur && !nos_aod);
        s_history_us += hal_micros() - start;
        return;
    }
    /* Baked directional blur already integrated its exposure. Seed history there
     * without another temporal filter; use the fallback everywhere else. */
    /* Each row keeps its left-to-right history order; disjoint rows may be
     * grouped without changing sharp history or exposure eligibility. */
    for (int y = y0; y < y1;) {
        int until=y+1;
        int spans[32][2], n = ui_fblur_row_spans_run(y, spans, 32, &until), x = x0;
        if(until>y1)until=y1;
        for (int i = 0; i < n && x < x1; i++) {
            int a = spans[i][0], b = spans[i][1];
            if (b <= x || a >= x1) continue;
            if (a > x) nos_history_apply_disjoint(fb, x, y, a < x1 ? a : x1, until, nos_temporal_blur && !nos_aod);
            if (a < x) a = x;
            if (b > x1) b = x1;
            nos_history_apply_disjoint(fb, a, y, b, until, 0);
            x = b;
        }
        if (x < x1) nos_history_apply_disjoint(fb, x, y, x1, until, nos_temporal_blur && !nos_aod);
        y=until;
    }
    s_history_us += hal_micros() - start;
}

static void finish_exposure(void)
{
    if (!s_exposure_pending) return;
    s_history_us += s_exposure_finish();
    s_exposure_pending = 0;
}

static uint32_t render_rect(int rx0, int ry0, int rx1, int ry1, int *slot)
{
    if (rx1 <= rx0 || ry1 <= ry0) return 0;
    uint32_t pixels = 0;
    int chunk = s_stripe_h > 0 ? s_stripe_h : (ry1 - ry0);
    for (int y0 = ry0; y0 < ry1; y0 += chunk) {
        int rows = ry1 - y0 < chunk ? ry1 - y0 : chunk;
        int x0 = rx0, x1 = rx1;
        nos_glass_span(y0, y0 + rows, &x0, &x1);
        if (x1 <= x0) continue;
        gfx_fb fb;
        memset(&fb, 0, sizeof fb);
        fb.y_base = y0;
        fb.h = rows;
        fb.cx0 = x0; fb.cx1 = x1;
        fb.glass_only = 1;
        if (s_stripe_h > 0) {
            if (s_wait_free) { uint32_t w0 = hal_micros(); s_wait_free(); s_wait_acc += hal_micros() - w0; }
            fb.px = s_stripe[*slot];
            { NOS_COST_SCOPE(NOS_COST_SCENE,1); scene_draw(&fb); }
            int deferred = 0;
            if (!NOS_RENDER_SHARP && s_exposure_prepare) {
                uint32_t start = hal_micros();
                deferred = s_exposure_prepare(&fb, x0, y0, x1, y0 + rows, *slot);
                s_history_us += hal_micros() - start;
            }
            if (deferred) s_exposure_pending = 1;
            else if(!NOS_RENDER_SHARP) {
                /* History has one writer, including an unusual host fallback
                 * after earlier stripes were submitted asynchronously. */
                finish_exposure();
                apply_exposure(&fb, x0, y0, x1, y0 + rows);
            }
            s_flush(fb.px + x0, GFX_W, x0, y0, x1 - x0, rows);
            *slot ^= 1;
        } else {
            fb.px = &s_full[y0 * GFX_W];
            { NOS_COST_SCOPE(NOS_COST_SCENE,1); scene_draw(&fb); }
            apply_exposure(&fb, x0, y0, x1, y0 + rows);
        }
        pixels += (uint32_t)(x1 - x0) * rows;
    }
    return pixels;
}

/* ---------------------------------------------------------------- frame */
void nos_init(uint16_t *fullframe)
{
    s_full = fullframe;
    native_menu_assets_init();
    nos_history_init();
    nos_layers_init();
    if (!s_scratch && !s_stripe_h && fullframe)
        s_scratch = (uint16_t *)hal_big_alloc(GFX_W * GFX_H * 2);
    s_last_input_ms = s_last_user_ms = hal_millis();
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

/* motion audit: the frame a transition lands is drawn idle; report its last step */
static int s_done_type = -1, s_done_cls, s_done_axis;
static float s_done_p;
static void tx_complete(void)
{
    uint32_t request=s_navigation_request;
    s_done_type = T.type; s_done_p = m_clamp(T.p, 0, 1);
    s_done_axis = T.variant == TXV_AXIS && T.state == TXS_AUTO;
    s_done_cls = T.state == TXS_FINGER ? MC_FINGER : T.released ? MC_MOMENTUM :
                 T.type == TX_DISSOLVE ? MC_FADE : MC_TRANSITION;
    rt_flush();
    if (T.p <= 0.001f && T.popping && s_depth > 1) {      /* the over screen leaves */
        call_leave(top_screen());
        s_depth--;
        call_reveal(top_screen());
    }
    T.state = TXS_IDLE;
    T.popping = 0; T.cancel_pops = 0; T.lin_v = 0;
    nos_iris_seed = 0;
    s_force_full = 1;
    navigation_arrive(request);
}

/* Shared axis (MOTION30 §5, Material's fade-through on a 56 px axis): the leaving screen
 * slides AXIS_OUT_PX away while its ink falls over the first ~35 % (3 frames of 10), then
 * the arriving one slides the last part of its AXIS_IN_PX in while its ink rises in
 * perceptual steps. One screen per frame, dimmed by ink: never a two-layer composite. */
static void axis_plan(void)
{
    int n = T.frames, k = m_frame_k(T.tw.t0);
    if (k < 1) k = 1;
    if (k > n) k = n;
    int no = n * 35 / 100;                       /* frames showing the leaving screen */
    int dx = T.type == TX_PUSH ? 1 : 0, dy = T.type == TX_RISE ? 1 : T.type == TX_DROP ? -1 : 0;
    float e = bezier_ease(&EASE_STEADY, (float)k / n);
    const nos_screen *leaving = T.popping ? T.over : T.under, *arriving = T.popping ? T.under : T.over;
    /* opening: the new screen comes from +dir and the old one parallaxes to -dir;
     * closing reverses both */
    float sgn = T.popping ? -1.0f : 1.0f;
    float ink, off;
    if (k <= no) {
        T.ax_s = leaving;
        ink = m_ink(1.0f - (float)k / (no + 1));
        off = -sgn * (T.popping ? AXIS_IN_PX : AXIS_OUT_PX) * e;
    } else {
        /* rising fast at first (ease-out), so the new screen is readable early */
        T.ax_s = arriving;
        ink = m_ink(bezier_ease(&EASE_STEADY, (float)(k - no) / (n - no)));
        off = sgn * (T.popping ? AXIS_OUT_PX : AXIS_IN_PX) * (1.0f - e);
    }
    int o = (int)lroundf(off);
    T.ax_ox = o * dx; T.ax_oy = o * dy;
    T.ax_k = (int)lroundf(ink * 32.0f);
}

static void tick_transition(float dt)
{
    if (s_rt_old && rt_frame() >= RT_FRAMES) rt_flush();
    /* the replaced screen fades in perceptual steps (62, 38, 12 %), then the new one */
    s_rt_k = s_rt_old ? (int)lroundf(32.0f * m_ink(1.0f - (float)(rt_frame() + 1) / (RT_FRAMES + 1))) : 32;
    if (T.state != TXS_AUTO) return;
    T.p_prev = T.p;
    /* Start the clock at the first presented frame (R1): the begin frame pays for the new
     * screen's enter() and a full repaint; if that made it late, the next frame must not
     * skip ahead by the delay. */
    if (T.fresh == 1 && dt > 1.5f / NOS_FPS) {
        float late = dt - 1.0f / NOS_FPS;
        if (T.use_tween) T.tw.t0 += late;
        dt -= late;
    }
    if (T.fresh) T.fresh--;
    int moving;
    if (T.lin_v > 0) {
        T.p -= T.lin_v * dt;
        moving = T.p > 0;
        if (!moving) T.p = 0;
        T.sp.x = T.p * T.span;
    } else if (T.use_tween) {
        moving = !mtw_done(&T.tw);
        if (T.variant == TXV_CUT) {
            /* positions in px of the leading edge, from m_cut: nudge, lead-in, the jump
             * to 4 px past, 2, 1, 0. A close is over at its jump: the rest is off-glass. */
            float e0 = (1.0f - T.tw.from) * T.span, e1 = (1.0f - T.tw.to) * T.span;
            int k = m_frame_k(T.tw.t0);
            T.p = 1.0f - m_cut(e0, e1, k) / T.span;
            if (T.popping && k >= 3) moving = 0;
        } else T.p = mtw_value(&T.tw);
        if (!moving) T.p = T.tw.to;
        if (T.variant == TXV_AXIS) axis_plan();
    } else {
        moving = msp_step(&T.sp, dt);            /* a pixel spring: ends with a 1 px snap */
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
        if (!B.dragging && B.y.target > 0 && nos_t - B.shown_t > BANNER_SHOW_S) banner_tween(0, BANNER_OUT_F, &EASE_LINEAR);
        if (B.dragging && P.down && P.owner == 2) {
            float d = (float)(P.y - P.y0);
            B.y.x = BANNER_H + (d < 0 ? d : rubber_band(d, 200.0f));
        } else if (B.tweening) {
            B.y.x = mtw_value(&B.tw);
            if (mtw_done(&B.tw)) {
                B.tweening = 0; B.y.x = B.tw.to; B.y.v = 0; B.y.moving = 0;
                if (B.tw.to <= 0) B.active = 0, B.tap_hold = 0;
            }
        } else if (!msp_step(&B.y, dt) && B.y.target <= 0) B.active = 0, B.tap_hold = 0;
    }
    if (TO.active && B.active) TO.t0 = nos_t - 1.0f / NOS_FPS;   /* queued: shown once the banner leaves */
    toast_tick();
}

/* Motion audit: the engine's own moving things, every frame they are on the glass
 * (docs/NOS_SIM.md "Motion audit"). Values are what is drawn: whole px for rigid
 * screens, the radius for the iris, ink % for fades. */
#ifdef GFX_COST_MODEL
static float tx_shown(float p)
{
    switch (T.type) {
    case TX_PUSH: return (float)lroundf(GFX_W * (1.0f - p));
    case TX_RISE: case TX_DROP: return (float)lroundf(GFX_H * (1.0f - p));
    case TX_IRIS: return IRIS_R0 + p * (iris_far() - IRIS_R0);
    default: return floorf(p * 64.0f + 0.5f) * 100.0f / 64.0f;   /* dissolve: dither level % */
    }
}
static void motion_probes(void)
{
    static const char *nm[] = { "tx.none", "tx.push", "tx.rise", "tx.drop", "tx.iris", "tx.dissolve" };
    if (s_done_type >= 0 && s_done_type <= TX_DISSOLVE && T.state == TXS_IDLE) {
        char n[32];
        int t = T.type; T.type = s_done_type;
        if (s_done_axis) {                         /* the arriving screen, at rest, full ink */
            snprintf(n, sizeof n, "%s.in", nm[s_done_type]);
            nos_motion_probe(n, MC_TRANSITION, 0);
            snprintf(n, sizeof n, "%s.ink", nm[s_done_type]);
            nos_motion_probe(n, MC_FADE, 100.0f);
        } else nos_motion_probe(nm[s_done_type], s_done_cls, tx_shown(s_done_p));
        T.type = t;
    }
    s_done_type = -1;
    if (T.state != TXS_IDLE && T.type >= 0 && T.type <= TX_DISSOLVE) {
        int cls = T.state == TXS_FINGER ? MC_FINGER : T.released ? MC_MOMENTUM :
                  T.type == TX_DISSOLVE ? MC_FADE : MC_TRANSITION;
        if (T.variant == TXV_AXIS && T.state == TXS_AUTO && T.ax_s) {
            char n[32];
            int in = T.ax_s == T.over ? !T.popping : T.popping;    /* the arriving screen */
            snprintf(n, sizeof n, "%s.%s", nm[T.type], in ? "in" : "out");
            if (T.fresh && !in) nos_motion_probe(n, MC_TRANSITION, 0);       /* origin: at rest */
            nos_motion_probe(n, MC_TRANSITION, (float)(T.ax_ox + T.ax_oy));
            snprintf(n, sizeof n, "%s.ink", nm[T.type]);
            if (T.fresh) nos_motion_probe(n, MC_FADE, 100.0f);               /* origin: full ink */
            nos_motion_probe(n, MC_FADE, T.ax_k * 100.0f / 32.0f);
        } else {
            if (T.fresh) nos_motion_probe(nm[T.type], cls, tx_shown(T.p_from));   /* origin */
            nos_motion_probe(nm[T.type], cls, tx_shown(m_clamp(T.p, 0, 1.15f)));
        }
    }
    if (B.active) {
        int cls = B.dragging ? MC_FINGER : B.tweening ? MC_COMPONENT : MC_MOMENTUM;
        if (B.fresh) nos_motion_probe("banner", cls, -BANNER_H);           /* origin: hidden */
        nos_motion_probe("banner", cls, floorf(B.y.x - BANNER_H + 0.5f));
    }
    B.fresh = 0;
    if (TO.active && !B.active) {
        if (TO.fresh) nos_motion_probe("toast", MC_COMPONENT, -TO.travel);
        TO.fresh = 0;
        nos_motion_probe("toast", MC_COMPONENT, -TO.off);
    }
    if (s_rt_old) nos_motion_probe("tx.retarget.ink", MC_FADE, s_rt_k * 100.0f / 32.0f);
}
#endif

static int s_readonly_cancel;
static uint32_t frame_run(float elapsed_ms,int readonly)
{
    if(NOS_RENDER_SHARP)nos_mblur=nos_temporal_blur=nos_iris_blur=0;
    if(!readonly)nos_dev_frame(elapsed_ms);
    /* Animation follows elapsed time even when a cheap frame starts early or
     * an expensive one finishes late. Authored frame tables still sample this
     * clock at NOS_FPS; presentation frequency must not speed up that clock. */
    if (!isfinite(elapsed_ms) || elapsed_ms <= 0) elapsed_ms = NOS_PERIOD_MS;
    if (elapsed_ms > 4.0f * NOS_PERIOD_MS) elapsed_ms = 4.0f * NOS_PERIOD_MS;
    nos_dt = elapsed_ms / 1000.0f;
    if(!readonly)nos_t+=nos_dt;
    nos_frame++;
    ui_fblur_observe_begin(nos_frame);
    s_mb_pixels[0]=s_mb_pixels[1]=0;
    nos_layers_begin(nos_frame);
    /* Match exposure weighting to the same elapsed interval as motion.
     * The bounded logical clock is not an optical shutter after a long stall. */
    if(!NOS_RENDER_SHARP)nos_history_begin_interval(nos_frame, (uint32_t)lroundf(elapsed_ms * 1000.0f));
    s_history_us = 0;
    s_in_frame = 1;

    uint32_t tick_start = hal_micros();
    hal_frame_begin();
    if(readonly) {
        /* Keep the already-presented pose and draw caches available to read-only
         * diagnostics while a flash writer owns mutation admission. Cancellation
         * is delivered only after the host owns UI mutation again. */
        s_readonly_cancel=1;s_raw_down=s_raw_dirty=s_smp_down=0;s_air=0;
        s_btn_raw=s_btn_down=s_btn_pending=s_btn_long_sent=0;
    } else {
    if(s_readonly_cancel) {s_readonly_cancel=0;nos_touch_cancel();}
    knock_tick();
    process_touch();
    process_long();
    process_button();
    if (s_air) {
        float event[]={(float)s_air};sports_emit(SP_GESTURE,event,1);
        nos_input in; memset(&in, 0, sizeof in); in.type = IN_AIR; in.x = s_air; s_air = 0;
        const nos_screen *s = top_screen();
        if (s && s->input) s->input(&in);
        user_input();
    }
    if (nos_bg_tick) nos_bg_tick(nos_dt);
    tick_transition(nos_dt);
#ifdef NOS_PSRAM_TRACKING
    nos_mem_idle(); /* release outgoing buffers before home prewarm allocates again */
#endif
    s_tx_ticked = 1;
    tick_screens(nos_dt);
    tick_overlays(nos_dt);
    prefs_tick();
#ifdef GFX_COST_MODEL
    motion_probes();
#endif
    }

    static int rects[MAXRECTS][4];
    int nr;
    int moving_tx = T.state != TXS_IDLE;
    uint32_t t0 = hal_micros();
    nos_last.tick_us = t0 - tick_start;
    if (s_band_enabled) {
        band_damage_clear(&s_band_current); s_band_stream_current = 0;
    }
    s_rec_cur->n = 0; s_rec_cur->overflow = 0;
    if (!moving_tx) {
        gfx_fb rfb; memset(&rfb, 0, sizeof rfb);
        rfb.h = GFX_H; rfb.cx0 = 0; rfb.cx1 = GFX_W; rfb.rec = s_rec_cur;
        scene_draw(&rfb);
    } else s_rec_cur->overflow = 1;                  /* a moving transition repaints all */
    uint32_t t1 = hal_micros();
    nos_cost_begin();
    s_wait_acc = 0;
    int full = s_force_full || moving_tx || nos_debug_force_full;
    s_band_capture = s_band_enabled;
    nr = full ? -1 : compute_damage(s_rec_cur, s_rec_prev, rects);
    s_band_capture = 0;
    if (nr < 0) {
        nr = 1; rects[0][0] = 0; rects[0][1] = 0; rects[0][2] = GFX_W; rects[0][3] = GFX_H;
        full = 1;
    }
    if (s_band_enabled && full)
        band_damage_add(&s_band_current, 0, 0, GFX_W, GFX_H);
    /* Native streaming blur carries state between raster stripes. Give a dirty
     * repaint the same stripe boundaries and glass clip as a full repaint;
     * otherwise moving rounded edges can inherit another column's filter state. */
    if(s_stripe_h>0) {
        for(int i=0;i<nr;i++) {
            rects[i][1]=(rects[i][1]/s_stripe_h)*s_stripe_h;
            rects[i][3]=((rects[i][3]+s_stripe_h-1)/s_stripe_h)*s_stripe_h;
            if(rects[i][3]>GFX_H) rects[i][3]=GFX_H;
        }
        for(int changed=1;changed;) {
            changed=0;
            for(int i=0;i<nr;i++) for(int j=i+1;j<nr;j++)
                if(rect_near(rects[i],rects[j],0)) {
                    rect_union(rects[i],rects[j]);
                    memcpy(rects[j],rects[--nr],sizeof rects[j]);j--;changed=1;
                }
        }
    }
    if (!moving_tx) s_force_full = 0;
    uint32_t tcompare = hal_micros();

    /* Temporal exposure reads SHARP history, never its previous blurred output.
     * Redraw each old damage rectangle once more to retire the one-frame tail.
     * Save only new sharp damage, otherwise a tail would perpetually invalidate. */
    int new_rects[MAXRECTS][4], new_nr = nr;
    memcpy(new_rects, rects, sizeof new_rects);
    if (nos_temporal_blur && !nos_aod) {
        for (int i = 0; i < s_history_nr; i++) {
            gfx_cmd c = { s_history_rects[i][0], s_history_rects[i][1],
                          s_history_rects[i][2], s_history_rects[i][3], 0 };
            damage_add(rects, &nr, &c);
        }
    }
    /* The disjoint history path requires one visit per screen pixel. Coalesce
     * after ALL expansion, including even alignment without stripe rendering,
     * and when temporal blur is disabled/AOD still updates sharp history.
     * render_rect emits disjoint stripes; exposure splits only shrink them. */
    for (int changed = 1; changed;) {
        changed = 0;
        for (int i = 0; i < nr; i++) for (int j = i + 1; j < nr; j++)
            if (rect_near(rects[i], rects[j], 0)) {
                rect_union(rects[i], rects[j]);
                memcpy(rects[j], rects[--nr], sizeof rects[j]);
                j--; changed = 1;
            }
    }
    s_history_nr = new_nr;
    memcpy(s_history_rects, new_rects, sizeof s_history_rects);

    int slot = 0;
    uint32_t px = 0, band_windows = 0;
    /* Stateful legacy smears require their original enlarged clips. Preserve
     * that path through the first post-smear frame; full paints also gain nothing. */
    int bands_used = s_band_enabled && !full && s_stripe_h == BD_ROWS &&
                     !s_band_stream_current && !s_band_stream_previous;
    if (bands_used) {
        for (int band = 0; band < BD_BANDS; band++) {
            int spans[BD_MAX_SPANS][2];
            int count = band_damage_spans(&s_band_current, &s_band_previous,
                nos_temporal_blur && !nos_aod, band, 64, spans);
            int y0 = band * BD_ROWS, y1 = y0 + BD_ROWS;
            if (y1 > GFX_H) y1 = GFX_H;
            for (int j = 0; j < count; j++) {
                int x0 = spans[j][0], x1 = spans[j][1];
                nos_glass_span(y0, y1, &x0, &x1); /* intersection only: spans stay disjoint */
                if (x1 <= x0) continue;
                px += render_rect(x0, y0, x1, y1, &slot); band_windows++;
            }
        }
    } else {
        for (int i = 0; i < nr; i++)
            px += render_rect(rects[i][0], rects[i][1], rects[i][2], rects[i][3], &slot);
    }
    if (s_band_enabled) {
        s_band_previous = s_band_current; /* save new SHARP damage, never unioned tails */
        s_band_stream_previous = s_band_stream_current;
    }
    /* All sharp history writes and exposure statistics must be complete before
     * publishing this frame or beginning another history generation. */
    finish_exposure();
    uint32_t t2 = hal_micros();
    nos_cost_end();
    hal_frame_end();

    nos_last.dirty_px = px;
    nos_last.rects = bands_used ? band_windows : (uint32_t)nr;
    nos_last.damage_legacy_rects = (uint32_t)nr;
    nos_last.damage_bands_used = (uint32_t)bands_used;
    nos_last.damage_bands_fallback = (uint32_t)(s_band_enabled && !bands_used);
    nos_last.cmds = (uint32_t)s_rec_cur->n;
    nos_last.record_us = t1 - t0;
    nos_last.compare_us = tcompare - t1;
    nos_last.raster_us = t2 - t1;
    nos_last.wait_us = s_wait_acc;
    nos_last.full = full;
    const nos_history_stats *hs = nos_history_get_stats();
    nos_layer_stats ls = nos_layers_stats();
    nos_last.history_us = s_history_us;
    nos_last.history_pixels = hs->pixels;
    nos_last.history_blended = hs->blended;
    nos_last.history_uniform_pixels = hs->uniform_pixels;
    nos_last.history_uniform_read_bytes = hs->uniform_read_bytes;
    nos_last.history_uniform_write_bytes = hs->uniform_write_bytes;
    nos_last.layer_record_us = ls.record_us;
    nos_last.layer_raster_us = ls.raster_us;
    nos_last.layer_bands = ls.bands_rendered;
    nos_last.layer_reused = ls.bands_reused;
    nos_last.mblur_h_pixels=s_mb_pixels[0];nos_last.mblur_v_pixels=s_mb_pixels[1];
    s_mb_live_frame = nos_frame;
    s_mb_live_owner = T.state == TXS_IDLE ? top_screen() : 0;
    nos_last.cache_bytes = ls.bytes + nos_history_storage_bytes();
    nos_last.allocation_failures = ls.allocation_failures + (!NOS_RENDER_SHARP && nos_history_storage_bytes() == 0);
    ui_blur_stats bs; ui_fblur_stats(&bs);
    nos_last.cache_bytes += bs.bytes;
    nos_last.allocation_failures += bs.allocation_failures;
#ifdef NOS_PSRAM_TRACKING
    /* Include every UI owner, including Orrery/card and specialized caches.
     * Per-cache reset/release must not erase a real allocation failure. */
    nos_last.allocation_failures = nos_mem_failures();
#endif
    nos_dbg_nr = nr;
    memcpy(nos_dbg_rect, rects, sizeof rects);

    gfx_rec *tmp = s_rec_prev; s_rec_prev = s_rec_cur; s_rec_cur = tmp;
    s_tx_ticked = 0;
    s_in_frame = 0;
    return px;
}
uint32_t nos_frame_run(float elapsed_ms) {return frame_run(elapsed_ms,0);}
uint32_t nos_frame_run_readonly(float elapsed_ms) {return frame_run(elapsed_ms,1);}
