/* nos.h — NOS, a Nothing-inspired watch OS: frame loop, screen stack, input, transitions.
 *
 * First principles this engine is built on (see docs/NOS_DESIGN.md):
 *  1. A fixed 30 Hz cadence. Every frame represents exactly 1/30 s of motion, so
 *     moving things advance by equal steps (judder comes from uneven steps far more
 *     than from a low rate). nos_dt is a whole number of frame periods: a late
 *     frame skips ahead in time instead of slowing motion down.
 *  2. Draw is pure. A screen's draw() runs once to record and once per display
 *     stripe; it must only read state that tick() prepared. All animation state is
 *     advanced in tick(), once per frame.
 *  3. Damage-tracked. The engine records every primitive (bbox + hash) and only
 *     repaints what changed, so a still screen costs nothing and a ticking second
 *     costs a digit.
 *  4. Everything is interruptible and velocity-continuous. Transitions are driven
 *     by a progress value the finger can grab at any moment; releasing hands the
 *     finger's velocity to the spring that finishes the motion.
 */
#ifndef NOS_H
#define NOS_H
#include <stdint.h>
#include "gfx.h"
#include "hal.h"

#define NOS_VERSION   "1.0.0"
#define NOS_FPS       30
#define NOS_PERIOD_MS 33.333f

/* ---- time ----------------------------------------------------------------- */
extern uint32_t nos_frame;   /* frames presented since boot */
extern float    nos_t;       /* animation clock, seconds; advances by nos_dt each frame */
extern float    nos_dt;      /* this frame's step: k * (1/30) s, k >= 1 */

/* ---- input ------------------------------------------------------------------
 * The recogniser turns raw touch into events. A press first delivers IN_DOWN.
 * Movement beyond the slop locks an axis and produces IN_DRAG_START, IN_DRAG...,
 * IN_DRAG_END (with release velocity). A press that never moved produces IN_TAP on
 * release, or IN_LONG after 450 ms (then no tap). IN_UP always closes a press. */
typedef enum {
    IN_DOWN = 0, IN_UP, IN_TAP, IN_LONG,
    IN_DRAG_START, IN_DRAG, IN_DRAG_END,
    IN_BTN, IN_BTN_LONG, IN_BTN_DOUBLE,   /* side button: press, 0.6 s hold, double */
    IN_AIR,                               /* APDS-9960 air gesture, x = GEST_* */
} nos_in_type;

enum { AXIS_NONE = 0, AXIS_X = 1, AXIS_Y = 2 };

typedef struct {
    nos_in_type type;
    int   x, y;          /* current finger position */
    int   x0, y0;        /* where the press started */
    float dx, dy;        /* x - x0, y - y0 */
    float vx, vy;        /* px/s, least-squares over the last ~80 ms */
    int   axis;          /* drag axis, AXIS_* */
} nos_input;

/* ---- screens ---------------------------------------------------------------- */
enum {
    SCR_HDRAG   = 1 << 0,  /* the screen consumes horizontal drags itself (pagers, sliders):
                              the back gesture then only starts within the left edge zone */
    SCR_NOBACK  = 1 << 1,  /* no swipe-back at all (the root face, modal flows) */
    SCR_OPAQUE_BG = 1 << 2,/* draw() paints every pixel itself; engine skips the black clear */
    SCR_KEEPAWAKE = 1 << 3,/* don't auto-sleep while this screen is on top (stopwatch, nav) */
};

typedef struct nos_screen {
    const char *name;
    void (*enter)(void);          /* became the top screen (pushed or revealed by a pop) */
    void (*leave)(void);          /* stopped being on top (covered or popped) */
    void (*tick)(float dt);       /* advance state; called every frame while visible */
    void (*draw)(gfx_fb *fb);     /* render; pure; may run many times per frame */
    int  (*input)(const nos_input *in);   /* return 1 if consumed */
    uint32_t flags;
} nos_screen;

/* Transition geometries. Every one is a rigid translation (no scaling: cost stays a
 * blit) plus a dim of the screen underneath. */
enum {
    TX_NONE = 0,
    TX_PUSH,     /* new screen slides in from the right; the old one parallaxes left and dims */
    TX_RISE,     /* new screen rises from the bottom over the dimmed old one (notifications) */
    TX_DROP,     /* new screen drops from the top (control centre) */
    TX_IRIS,     /* iris: the new screen appears through a hard-edged, hairline-rimmed
                    circle that grows from nos_iris_x/y (an app opening from its icon) */
    TX_DISSOLVE, /* ordered-dither dissolve in place (face changes, mode switches) */
};
extern int nos_iris_x, nos_iris_y;   /* centre for TX_IRIS; set before pushing */
/* Optional seed for an opening TX_IRIS: the icon the user tapped (DOT_ICON_*, on a
 * C_SURF2 disc of radius nos_iris_seed_r), drawn over the circle and fading out over
 * the first 40 %, so the app grows out of the icon instead of deleting it. 0 = none;
 * nos_open() clears it, nos_open_icon() sets it. */
extern int nos_iris_seed;
extern float nos_iris_seed_r;

void nos_push(const nos_screen *s, int tx);
void nos_pop(void);                         /* reverses the transition the top was pushed with */
void nos_pop_with(int tx);                  /* pop with a different transition (boot -> home) */
void nos_replace(const nos_screen *s, int tx);   /* swap the top screen */
int  nos_retarget(const nos_screen *s);           /* a push still opening shows s instead (1 = done) */
void nos_home(void);                        /* pop to the root with one transition */
void nos_home_now(void);                    /* collapse to the root instantly (entering AOD) */
/* Inside enter(): 1 when the screen is being pulled in by a finger (nos_pull): skip
 * build-ins, the finger is revealing it. */
int  nos_entered_by_finger(void);
/* From a sheet's input during IN_DRAG: its content hit the scroll limit in the closing
 * direction, so hand the finger to the engine (re-based, no jump) to move the sheet. */
void nos_sheet_take(void);
/* Dim what `fb` covers to k (0..1) of its brightness, after drawing (costs a pass
 * over every lit pixel). Prefer nos_ink_dim before drawing: free. */
void nos_dim(gfx_fb *fb, float k);
/* Everything drawn into `fb` from now on keeps k (0..1) of its brightness (edit-mode
 * backdrops, a face receding). Scales each primitive's colour once, so a dimmed
 * screen costs no more than a bright one. Composes with a dim already on `fb`. */
void nos_ink_dim(gfx_fb *fb, float k);
/* Screen the side button's long press opens when the top screen doesn't take it. */
extern const struct nos_screen *nos_power_screen;
void nos_set_root(const nos_screen *s);
const nos_screen *nos_top(void);
int  nos_depth(void);                       /* 1 = only the root */
int  nos_transitioning(void);
/* Inside enter(): 1 when the screen is being revealed by a pop (back), 0 when it was
 * just pushed. Use it to keep scroll state and skip build-ins on return. */
int  nos_revealed(void);
/* Inside leave(): 1 when the screen is being covered by a push (it stays on the stack
 * and will be revealed later: keep sensors and state), 0 when it is going away. */
int  nos_leaving_covered(void);
/* The finger's travel from its touch-down point in px (max of |dx|, |dy|; 0 when up).
 * Under the 12 px drag slop no IN_DRAG arrives: use this to drop a pressed state early. */
int  nos_touch_travel(void);
/* Inside enter(): when the screen's entry build should start. Equals nos_t, except for
 * an iris push, where it is the frame the circle passes 40 % (~2 frames in: big enough
 * to show the build; the icon seed covers the start). Use it for build clocks with no
 * extra delay of your own: `S.t_enter = nos_enter_t();`, clamping elapsed at 0. */
float nos_enter_t(void);

/* Interactive transitions: call from a screen's input handler on IN_DRAG_START to let
 * the finger pull another screen in (e.g. the face pulling notifications up). The
 * engine then owns the drag: progress follows the finger along the transition's axis,
 * and release commits or cancels by position and velocity. */
void nos_pull(const nos_screen *s, int tx, const nos_input *start);

/* ---- system services the UI calls ------------------------------------------ */
/* A Nothing-style banner over everything (incoming notification, alarm, toast).
 * `icon` is a DOT_ICON_* id (nos/dot.h), 0 = none. Tapping it calls on_tap. */
void nos_banner(const char *title, const char *body, int icon, void (*on_tap)(void));
void nos_toast(const char *text);           /* short centred pill, ~1.4 s */
/* Feedback: haptic + sound in one call (respects the user's settings). */
enum { FB_TICK = 0, FB_TAP, FB_TOGGLE_ON, FB_TOGGLE_OFF, FB_BACK, FB_OPEN,
       FB_SUCCESS, FB_ERROR, FB_LIMIT, FB_NOTIFY };
void nos_feedback(int kind);
/* Panel brightness override for screens that are light sources (torch, find-watch):
 * level 0-255, or -1 to return to the user's brightness. The host applies it every
 * frame instead of the user level, so it can't be fought by auto-brightness. */
void nos_brightness_override(int level);
extern int nos_bright_override;

/* Background hook run every frame after input, before screens tick (services). */
extern void (*nos_bg_tick)(float dt);
/* A view of `fb` restricted to screen rect [x0,x1)x[y0,y1) with content translated by
 * (ox, oy): for pagers and sub-panels. Returns 0 when nothing of it is in this stripe. */
int  nos_subview(const gfx_fb *fb, gfx_fb *out, int x0, int y0, int x1, int y1, int ox, int oy);

/* Request a full repaint (after a screen changes something the recorder can't see,
 * such as re-rendering a sprite under the same key). */
void nos_invalidate(void);
/* Ask for the next frame even if nothing seems to move (polling sensors etc.).
 * Damage tracking still sends only what changed. */
void nos_keep_awake(void);                 /* resets the screen-off timer */
int  nos_awake_ms(void);                   /* ms since the last interaction */

/* ---- host interface (firmware main / simulator) ------------------------------ */
void nos_init(uint16_t *fullframe);        /* fullframe: sim renders whole frames into it */
void nos_set_stripes(uint16_t *a, uint16_t *b, int rows, void (*wait_free)(void),
                     void (*flush)(uint16_t *buf, int stride, int x0, int y0, int w, int rows));
/* Scratch buffer of GFX_W * rows pixels for two-layer transitions (iris/dissolve). */
void nos_set_scratch(uint16_t *buf);
void nos_touch(int x, int y, int down);    /* raw touch, UI coordinates, any rate */
void nos_button(int down);                 /* raw side button state */
void nos_air(int gesture);
/* Run one frame: input, tick, record, damage, raster+flush. `elapsed_ms` is the
 * wall time since the previous call (the engine quantises it to whole periods).
 * Returns the number of pixels sent (0 = the panel needed nothing). */
uint32_t nos_frame_run(float elapsed_ms);

/* Profiling for the last frame (sim cost model and device PERF lines). */
typedef struct {
    uint32_t dirty_px, rects, cmds;
    uint32_t record_us, raster_us;
    int      full;           /* forced full repaint */
} nos_stats;
extern nos_stats nos_last;
extern int nos_debug_force_full;   /* sim --damagecheck: repaint everything every frame */
/* Always-on display: the host sets this while the panel is dimmed; the root face draws
 * its sparse AOD variant (<10% lit pixels, slow drift against burn-in). */
extern int nos_aod;

#endif
