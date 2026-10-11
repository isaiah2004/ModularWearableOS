/* motion.h — NOS animation primitives.
 *
 * Two families, chosen per motion:
 *  - Springs for anything a finger can touch (pages, sheets, scroll snaps, toggles):
 *    they take the release velocity and are interruptible at any instant. Configured
 *    like SwiftUI: `response` = period of the undamped oscillation in seconds (how
 *    fast), `damping` = damping fraction (1 = no overshoot, 0.8 = a hint of settle).
 *  - Curves for motion that nothing touches (reveals, staggered builds, fades):
 *    exact cubic-bezier easing over a fixed duration.
 * All functions are deterministic in dt, so the sim and the watch move identically. */
#ifndef NOS_MOTION_H
#define NOS_MOTION_H

/* ---- springs ---- */
typedef struct {
    float x, v;          /* position, velocity (units/s) */
    float target;
    float response;      /* s */
    float damping;       /* fraction */
    float eps;           /* settle threshold; scaled to the move on msp_to/kick */
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
void  msp_to(mspring *s, float target);
void  msp_set(mspring *s, float x);            /* jump, zero velocity */
void  msp_kick(mspring *s, float target, float v);   /* retarget with a velocity (release) */
/* Advance; returns 1 while moving. Settles exactly onto target when within `eps`. */
int   msp_step(mspring *s, float dt);
#define MSP_EPS 0.25f    /* max settle threshold; each move uses min(0.25, 1% of its distance) so 0..1 springs settle precisely */

/* ---- momentum scrolling (iOS model) ----
 * Velocity decays exponentially with a 0.998/ms rate (time constant ~0.5 s). The
 * projected resting point lets snapping pick its target at release time. */
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
extern const bezier EASE_STANDARD;    /* (0.2, 0, 0, 1)    Material 3 "emphasized" feel */
extern const bezier EASE_DECEL;       /* (0.05, 0.7, 0.1, 1) things arriving */
extern const bezier EASE_ACCEL;       /* (0.3, 0, 0.8, 0.15) things leaving */
extern const bezier EASE_INOUT;       /* (0.65, 0, 0.35, 1) symmetric moves */
extern const bezier EASE_MECH;        /* (0.7, 0, 0.2, 1)  Nothing's precise, mechanical snap */

/* A one-shot tween on the animation clock. */
typedef struct { float from, to, t0, dur; const bezier *curve; } mtween;
void  mtw_start(mtween *tw, float from, float to, float dur, const bezier *curve);
float mtw_value(const mtween *tw);             /* uses nos_t */
int   mtw_done(const mtween *tw);
float mtw_progress(const mtween *tw);          /* raw 0-1 */

/* helpers */
float m_clamp(float x, float lo, float hi);
float m_lerp(float a, float b, float t);
float m_unlerp(float a, float b, float x);     /* inverse lerp, unclamped */
float m_smoothstep(float e0, float e1, float x);
/* Stagger: progress of item i of n when a sequence runs at global progress p, each
 * item taking `span` of the timeline (0-1) and starting evenly spread. */
float m_stagger(float p, int i, int n, float span);
/* Deterministic hash noise in [0,1) for per-dot randomised timings. */
float m_hash01(unsigned a, unsigned b);
#endif
