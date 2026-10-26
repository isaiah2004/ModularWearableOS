/* motion.h — NOS animation primitives.
 *
 * Two families, chosen per motion:
 *  - Springs for anything a finger can touch (pages, sheets, scroll snaps, toggles):
 *    they take the release velocity and are interruptible at any instant. Configured
 *    like SwiftUI: `response` = period of the undamped oscillation in seconds (how
 *    fast), `damping` = damping fraction (1 = no overshoot, 0.8 = a hint of settle).
 *  - Curves for motion that nothing touches (reveals, staggered builds, fades):
 *    exact cubic-bezier easing or a frame table over a whole number of frames.
 * All functions are deterministic in dt, so the sim and the watch move identically.
 *
 * Motion at 30 fps follows docs/MOTION30.html ("Smooth at 30"); the budget per class is
 * its §14. In short: count in whole frames (M_FRAMES), judge a curve by its worst frame
 * (EASE_STEADY or a frame table for arrivals), end crisply (pixel springs snap within
 * 1 px), stagger in whole frames, fade in perceptual ink steps (m_ink). */
#ifndef NOS_MOTION_H
#define NOS_MOTION_H
#include "render_config.h"
#include "animation_time.h"

/* ---- whole frames (R3, R9) ----
 * NOS authors motion at NOS_FPS (40 by default), independently of the device's
 * presentation target. Author durations as frames, not milliseconds:
 * 2 pop, 3 snap, 4-6 micro, 7-9 component, 10-12 screen. */
#define M_FPS          NOS_FPS
#define M_FRAMES(n)    ((float)(n) * (1.0f / M_FPS))   /* n whole frames in seconds: mtw_start(.., M_FRAMES(9), ..) */
int   m_frame_k(nos_time_t t0); /* whole authored frames since precise origin (rounded; saturating) */

/* ---- springs ---- */
typedef struct {
    float x, v;          /* position, velocity (units/s) */
    float target;
    float response;      /* s */
    float damping;       /* fraction */
    float eps;           /* unitless springs: settle threshold, min(0.25, 1% of each move) */
    float snap;          /* > 0: settle within `snap` units (pixel springs: 1 px), see msp_snap */
    int   moving;
} mspring;

/* Presets (response, damping), tuned on the 30 Hz cadence: */
/* Nothing's motion is mechanical: "click, not swoosh". Everything a finger drives is
 * critically damped (no overshoot); only rare playful accents may bounce. */
#define SPRING_SNAPPY   0.26f, 1.00f   /* toggles, small UI parts */
#define SPRING_PAGE     0.38f, 1.00f   /* page / screen moves */
#define SPRING_SHEET    0.42f, 1.00f   /* sheets pulled by the finger */
#define SPRING_BOUNCY   0.50f, 0.78f   /* rare playful accents (success) */
#define SPRING_SOFT     0.70f, 1.00f   /* slow ambient moves */

void  msp_init(mspring *s, float x, float response, float damping);
/* A spring whose value is on-screen pixels (offsets, positions, sizes): msp_init plus a
 * 1 px snap (R5). It ends the frame it is within 1 px of the target (or within 1.5 px and
 * already moving < 1.5 px a frame) and slower than 3 px a frame, so a page-sized move
 * creeps at most 2 frames instead of ~6, and never pops more than ~1 px. */
void  msp_init_px(mspring *s, float x, float response, float damping);
/* Settle threshold in the spring's own units: `snap` 1 = pixels; 0.5 for a 0..100 %
 * spring over a ~200 px span; 0 = the unitless default (1 % of each move, at most 0.25:
 * 0..1 springs and page indices settle precisely). Call after msp_init. */
void  msp_snap(mspring *s, float snap);
void  msp_to(mspring *s, float target);
void  msp_set(mspring *s, float x);            /* jump, zero velocity */
void  msp_kick(mspring *s, float target, float v);   /* retarget with a velocity (release) */
/* Advance; returns 1 while moving. Settles exactly onto target (see msp_snap). */
int   msp_step(mspring *s, float dt);
#define MSP_EPS 0.25f    /* unitless springs: max settle threshold; each move uses min(0.25, 1% of its distance) */
#define MSP_SNAP_PX 1.0f /* pixel springs: settle within 1 px */

/* ---- momentum scrolling (iOS model) ----
 * Velocity decays exponentially with a 0.998/ms rate (time constant ~0.5 s). The
 * projected resting point lets snapping pick its target at release time. A free fling
 * ends crisply: below 3.5 px a frame it steps whole pixels (3, then 2 px a frame, R6)
 * and stops once slower than 1.5 px a frame (no creeping tail, R5). Its springs (snap,
 * rubber-band return, edge bounce) are pixel springs. */
#define DECAY_RATE 0.998f
float decay_project(float x, float v);         /* where a fling released at x with v rests */

typedef struct {
    float x, v;           /* offset (content px), velocity px/s */
    float min, max;       /* scroll bounds */
    float snap;           /* > 0: rest on multiples of snap */
    int   snap_free;      /* 1: a fling may travel many snaps (lists); 0: one page per flick (pagers) */
    int   dragging, moving;
    float drag_x0, drag_off0;
    mspring sp;           /* used for snapping and rubber-band return */
    int   springing;
} mscroll;

void  msc_init(mscroll *s, float min, float max, float snap);
void  msc_bounds(mscroll *s, float min, float max);
void  msc_grab(mscroll *s);                    /* finger down: stop and hold */
void  msc_drag(mscroll *s, float finger_delta);/* total finger travel since grab */
void  msc_release(mscroll *s, float v);        /* finger up with velocity (content px/s) */
void  msc_jump(mscroll *s, float x);
void  msc_animate_to(mscroll *s, float x);
int   msc_step(mscroll *s, float dt);          /* returns 1 while moving */
/* iOS rubber band: resistance past the edge. `over` = overshoot, `dim` = viewport. */
float rubber_band(float over, float dim);

/* ---- curves ---- */
typedef struct { float x1, y1, x2, y2; } bezier;
float bezier_ease(const bezier *b, float t);   /* t in [0,1] -> eased [0,1] */
/* Arrivals at 30 fps: EASE_STEADY. Its biggest frame is <= 1.6x the average step, frame 1
 * already shows ~16 % of a 10-frame move, and it lands with a ~1 % last step (§4). */
extern const bezier EASE_STEADY;      /* (0.61, 1, 0.88, 1) ~ease-out sine: arrivals, screen moves */
extern const bezier EASE_LINEAR;      /* (0, 0, 1, 1)       constant-speed exits */
/* Older curves: their peak frame is 2.4-3.7x the average (EASE_STANDARD jumps 130-165 px
 * on a 466 px move); keep them for small moves and fades only. */
extern const bezier EASE_STANDARD;    /* (0.2, 0, 0, 1)    Material 3 "emphasized" feel */
extern const bezier EASE_DECEL;       /* (0.05, 0.7, 0.1, 1) things arriving (frame 1 = 68 %!) */
extern const bezier EASE_ACCEL;       /* (0.3, 0, 0.8, 0.15) things leaving */
extern const bezier EASE_INOUT;       /* (0.65, 0, 0.35, 1) symmetric moves */
extern const bezier EASE_MECH;        /* (0.7, 0, 0.2, 1)  Nothing's precise, mechanical snap */

/* ---- frame tables (R3) ----
 * Short moves written as the cumulative share shown on each frame after the start
 * (percent, last = 100): animators' "thirds". mf_at(tab, k) is the fraction for k whole
 * frames after the start (k <= 0 -> 0, k >= n -> 1). */
typedef struct { unsigned char n; unsigned char pct[12]; } mframes;
extern const mframes MF_SNAP3;        /* 60 90 100                 3 frames: toggles, chips */
extern const mframes MF_MICRO4;       /* 45 78 95 100              4 frames: pills, badges */
extern const mframes MF_MICRO6;       /* 28 53 73 87 96 100        6 frames: small reveals */
float mf_at(const mframes *tab, int k);

/* A mechanical cut (R4b, Pebble's "moook"): frame 1 a 1 px nudge, frame 2 a lead-in
 * (20 px, at most 20 % of the move), frame 3 the jump to 4 px past the target, then
 * 2, 1, 0 px. Nothing sits at a mid-speed frame for the eye to track: use it for far,
 * decisive programmatic moves (a page snapping, a tile flipping). Smear frame 3 if the
 * object is small (ui_smear_*). Returns the position k whole frames after the start. */
#define M_CUT_FRAMES 6
float m_cut(float from, float to, int k);

/* A one-shot tween on the animation clock (nos_t). Durations are rounded to whole
 * frames, so the last frame lands exactly on `to` (0.34 s = 10.2 frames used to leave a
 * sliver step). The value on the frame it starts is `from`: call mtw_lead() right after
 * starting it from input to show the first step on this very frame. */
typedef struct { float from, to; nos_time_t t0; float dur; const bezier *curve; const mframes *tab; } mtween;
void  mtw_start(mtween *tw, float from, float to, float dur, const bezier *curve);
void  mtw_start_tab(mtween *tw, float from, float to, const mframes *tab);   /* frame-table tween */
void  mtw_lead(mtween *tw);                    /* start one frame in: input -> this frame moves */
float mtw_value(const mtween *tw);             /* uses nos_t */
int   mtw_done(const mtween *tw);
float mtw_progress(const mtween *tw);          /* raw 0-1 */

/* helpers */
float m_clamp(float x, float lo, float hi);
float m_lerp(float a, float b, float t);
float m_unlerp(float a, float b, float x);     /* inverse lerp, unclamped */
float m_smoothstep(float e0, float e1, float x);
/* Stagger: progress of item i of n when a sequence runs at global progress p, each
 * item taking `span` of the timeline (0-1) and starting evenly spread. Fractional-frame
 * starts clump at 30 fps (0, 0, 1, 1, 2, 3, 3...): prefer m_stagger_k. */
float m_stagger(float p, int i, int n, float span);
/* Whole-frame stagger (R9): the local frame count of item i in a cascade that is k whole
 * frames old, each item starting `step` frames (1 or 2) after the previous one, item 0
 * at frame 0. Items past the 8th start with the 8th. Feed it to mf_at() or a curve:
 *   int k = m_frame_k(S.t0);  float p = mf_at(&MF_MICRO6, m_stagger_k(k, i, 1)); */
int   m_stagger_k(int k, int i, int step);
/* Perceptually even fade (R9): ink fraction (0..1) for linear progress t (0..1),
 * ~t^1.5 (100/70/45/25/10/0 % when fading out), quantised to 16 levels so a fade is at
 * most 16 distinct repaints. Use with nos_ink_dim / ui_dim. */
float m_ink(float t);
/* Deterministic hash noise in [0,1) for per-dot randomised timings. */
float m_hash01(unsigned a, unsigned b);
#endif
