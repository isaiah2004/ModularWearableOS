/* nos.h — NOS, a Nothing-inspired watch OS: frame loop, screen stack, input, transitions.
 *
 * First principles this engine is built on (see docs/NOS_DESIGN.md):
 *  1. A nominal NOS_FPS animation cadence (40 Hz by default); the device may
 *     present more frequently at NOS_PRESENT_FPS. nos_dt follows the finite,
 *     positive elapsed interval, capped at four target periods after a stall.
 *     Invalid/nonpositive intervals use one target period. Authored frame tables
 *     sample this elapsed clock; presentation does not quantize elapsed time.
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
#include "render_config.h"
#include "animation_time.h"

#define NOS_VERSION   "0.5.0-alpha"

/* ---- time ----------------------------------------------------------------- */
extern uint32_t nos_frame;   /* frames presented since boot */
extern nos_time_t nos_t;     /* animation clock, seconds; store origins as nos_time_t */
extern float    nos_dt;      /* bounded elapsed seconds for this frame's motion math */

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
    IN_CANCEL,                            /* lost touch stream: discard press/drag without action */
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
    SCR_NATIVE_CACHE = 1 << 4, /* screen owns raster caches; avoid duplicate retained storage */
    SCR_LIVE_RENDER = 1 << 5, /* simple screen: draw into stripes, skip optional PSRAM raster capture */
    SCR_SELFCAP = 1 << 6,  /* with KEEPAWAKE: the screen bounds its own awake time (torch); no shell inactivity cap */
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
 * blit) plus a dim of the screen underneath. A finger always moves the geometry 1:1;
 * programmatic opens and closes of PUSH/RISE/DROP use the variant the engine picked for
 * 30 fps (docs/MOTION30.html §5: a short shared-axis fade-through, a steady push or a
 * mechanical cut), so no frame steps more than ~64 px. */
enum {
    TX_NONE = 0,
    TX_PUSH,     /* new screen comes in from the right; the old one parallaxes left and dims */
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
extern int nos_iris_blur;             /* the iris edge's motion blur: 1 on (default), 0 the hard rim */

/* Motion blur for whole-screen moves (MOTION30 R7): pager pages, push / rise / drop
 * transitions. Applied per stripe, after drawing the moving layer, on its live pixels.
 *   nos_mblur_h: rows of [x0, x1) blurred along x, a box of ~half the step (4-32 px);
 *                all-black rows cost a scan only.
 *   nos_mblur_v: rows [y0, y1) smeared along y, an exponential tail (a 16-row stripe has
 *                no rows above or below it, so one row of state runs stripe to stripe).
 * Raw motion is pixels since the last drawn frame. nos_mblur_step normalizes it
 * to the authored 1/30-second exposure; filters begin at 8 normalized pixels.
 * nos_mblur_step(step, held): 0 when off, or held by a finger (the
 * eye tracks a drag) unless nos_mblur == 2. nos_mb_track gives the step from the drawn
 * position, once per frame, 0 after a frame that wasn't drawn. */
extern int nos_mblur;                 /* 0 off, 1 only once released, 2 also under a finger (default) */
/* 1 when a finger-held move must stay sharp (nos_mblur < 2): lists use it for their blur */
#define NOS_HELD_SHARP(held) ((held) && nos_mblur < 2)
float nos_mblur_step(float step, int held);
/* Changes only when an admitted live H/V filter records a command. Retained
 * layers compare around recording to avoid caching streaming filter output. */
uint32_t nos_mblur_record_epoch(void);
/* Live passes choose the primary or fallback kernel per region and reserve its cost
 * against CPU raster (elapsed raster minus stripe waits and previous blur).
 * nos_mblur_afford is the separate iris guard; step() only checks user policy. */
#define NOS_MBLUR_BUDGET_US 26000u   /* CPU raster excluding stripe waits; leaves pipeline headroom */
int   nos_mblur_afford(void);
float nos_mblur_ns(int kind);            /* ns/area pixel: full H,V then reduced H,V */
extern uint32_t nos_mblur_reduced[2];     /* subset of ran: quarter-width H / full-column V fallback */
/* blur diagnostics for the serial "info", once per frame per kind:
 * [kind][0 moving, 1 fast enough, 2 drawn blurred, 3 fell back sharp]; kinds: NOS_BS_* */
enum { NOS_BS_LIST, NOS_BS_SETTINGS, NOS_BS_NOTIFS, NOS_BS_LIVE, NOS_BS_N };
extern uint32_t nos_blur_stat[NOS_BS_N][4];
extern uint32_t nos_mblur_gate[2];       /* [legacy first veto (now zero), unaffordable region-frames] */
void nos_blur_note(int kind, int moving, int fast, int blurred);
extern uint32_t nos_mblur_count[2][2];   /* [horizontal, vertical][skipped, ran], for the serial "info" */
void  nos_mblur_h(gfx_fb *fb, int x0, int x1, float step);
void nos_mblur_h_reduced(gfx_fb *fb, int x0, int x1, float step);
void nos_mblur_v_reduced(gfx_fb *fb, int y0, int y1, float step);
int nos_mblur_v_fallback(gfx_fb *fb, int x0, int y0, int x1, int y1, float step);
void  nos_mblur_v(gfx_fb *fb, int y0, int y1, float step);
void  nos_mblur_v_rect(gfx_fb *fb, int x0, int y0, int x1, int y1, float step);   /* columns [x0, x1) only */
typedef struct { uint32_t frame; float last, step; } nos_mb_track;
float nos_mb_track_step(nos_mb_track *t, float pos);

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
 * screen costs no more than a bright one. Composes with a dim already on `fb`.
 * Damage: recorded primitive signatures include fb->dim, so dim-only changes on
 * stationary primitives repaint. Custom callbacks must record their current dim
 * through gfx_rec_cmd; retain their usual source-revision keys and pure draw rule. */
void nos_ink_dim(gfx_fb *fb, float k);
/* Screen the side button's long press opens when the top screen doesn't take it. */
extern const struct nos_screen *nos_power_screen;
void nos_set_root(const nos_screen *s);
const nos_screen *nos_top(void);
/* Live stack entries and outgoing transition owners, including covered screens. */
int nos_contains(const nos_screen *screen);
int  nos_depth(void);                       /* 1 = only the root */
int  nos_transitioning(void);
/* A firmware host keeps this true while awaiting checked shutdown barriers. */
int  nos_host_shutdown_pending(void);
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
nos_time_t nos_enter_t(void);

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
/* Knock to lock: while locked, new touches are ignored (they show how to unlock) and the
 * host skips raise-to-wake; the side button still works. A double knock (hal_knock_count)
 * toggles it when nos_prefs.knock_lock allows. */
extern int nos_locked;
void nos_set_locked(int on);
void nos_lock_hint(void);                  /* "KNOCK TWICE TO UNLOCK", at most every 2 s */
int  nos_knock_enabled(void);
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
uint32_t nos_user_idle_ms(void);           /* ms since the last real touch/button/gesture (apps' keep-awake calls do not count) */

/* ---- host interface (firmware main / simulator) ------------------------------ */
void nos_init(uint16_t *fullframe);        /* fullframe: sim renders whole frames into it */
void nos_set_stripes(uint16_t *a, uint16_t *b, int rows, void (*wait_free)(void),
                     void (*flush)(uint16_t *buf, int stride, int x0, int y0, int w, int rows));
/* Optional host worker for temporal exposure. prepare snapshots final stripe
 * coverage into host-owned slot storage; 1 defers exposure to the flush worker,
 * 0 uses the synchronous path. The host retains pixels and metadata until the
 * worker releases that slot. finish fences all queued work and returns worker
 * exposure microseconds since its previous call. The engine fences before a
 * synchronous fallback, statistics publication and the next history generation.
 * Install together, before rendering starts; NULL disables both hooks. */
void nos_set_stripe_exposure(int (*prepare)(const gfx_fb *, int, int, int, int, int),
                            uint32_t (*finish)(void));
/* Scratch buffer of GFX_W * rows pixels for two-layer transitions (iris/dissolve). */
void nos_set_scratch(uint16_t *buf);
/* Raw touch in UI coordinates, at the controller's rate (60-100 Hz), every sample: they
 * feed the velocity fit. nos_touch_at takes the sample's own timestamp (hal_millis()
 * clock), for hosts that sample on another task and hand a batch over right before
 * nos_frame_run (R10: latch the newest sample just before tick). nos_touch stamps "now".
 * A press and release inside one batch still produce their tap. */
void nos_touch(int x, int y, int down);
void nos_touch_at(int x, int y, int down, uint32_t ms);
/* UI-thread timeout/removal: sends IN_CANCEL, never TAP/UP/DRAG_END. down=2 in
 * nos_touch_at is the host handoff sentinel for this operation. */
void nos_touch_cancel(void);
/* Drag prediction (R10): while dragging, screens and the engine see the finger ~16 ms
 * (half a frame) ahead along its least-squares velocity, capped at half its last frame's
 * travel and 24 px, never past a release or once it stops. Release velocity is zero when
 * the finger was still for 40 ms. 1 = on (default; serial `predict 0|1` on the watch). */
extern int nos_touch_predict;
void nos_button(int down);                 /* raw side button state */
void nos_air(int gesture);
/* Run one frame: input, tick, record, damage, raster+flush. `elapsed_ms` is the
 * wall time since the previous call, used exactly when finite and positive and
 * capped to four target periods. Other values use one target period. nos_dt,
 * screen ticks and temporal exposure receive that same bounded interval.
 * Returns the number of pixels sent (0 = the panel needed nothing). */
uint32_t nos_frame_run(float elapsed_ms);
/* Host flash-writer scope: repaint cached pose with app clock/ticks/input frozen.
 * Held input is cancelled on the next ordinary frame, never replayed as a tap. */
uint32_t nos_frame_run_readonly(float elapsed_ms);

/* Profiling for the last frame (sim cost model and device PERF lines). */
typedef struct {
    uint32_t dirty_px, rects, cmds; /* rects: actual windows; may exceed12 in band mode */
    uint32_t record_us, raster_us;
    uint32_t compare_us; /* nested in raster_us; damage comparison only */
    uint32_t wait_us;        /* of raster_us: waiting for a free stripe (the bus), not CPU */
    uint32_t tick_us, history_us, history_pixels, history_blended, history_uniform_pixels;
    uint32_t history_uniform_read_bytes, history_uniform_write_bytes;
    uint32_t layer_record_us, layer_raster_us, layer_bands, layer_reused;
    uint32_t mblur_h_pixels, mblur_v_pixels; /* actual H/V raster work; recording adds zero */
    uint32_t cache_bytes, allocation_failures;
    int      full;           /* forced full repaint */
    uint32_t damage_legacy_rects; /* old <=12 bounding boxes; HUD diagnostic only */
    uint32_t damage_bands_used, damage_bands_fallback;
} nos_stats;
extern nos_stats nos_last;
extern int nos_debug_force_full;   /* sim --damagecheck: repaint everything every frame */
/* Experimental defaultOFF; UI-thread safe point only. Invalidates on change. */
void nos_damage_bands_set(int enabled);
int nos_damage_bands_get(void);
extern int nos_temporal_blur;      /* A/B diagnostic; production defaults to enabled */

/* Debug/A-B: the variant programmatic opens and closes of TX_PUSH / TX_RISE / TX_DROP
 * use (sim `set tx push open axis`, docs/NOS_SIM.md). The engine default is in nos.c. */
enum { TXV_DEFAULT = 0, TXV_LEGACY, TXV_STEADY, TXV_AXIS, TXV_CUT };
void nos_debug_tx_variant(int tx, int opening, int variant);

/* ---- motion audit (docs/MOTION30.html §14, docs/NOS_SIM.md "Motion audit") -----------
 * From tick(), report the on-screen position in px (offset, x/y, radius, extent) of each
 * animated thing, EVERY frame it is on screen, still or moving, under a stable name:
 *     nos_motion_probe("music.cover.x", MC_COMPONENT, S.cover_x);
 * The sim measures each frame's step and checks every move against its class budget
 * (step px/frame, peak/avg, first frame, creep, length) into motion.csv / motion.txt. A
 * thing that only exists while it moves can give its start: report the origin once when
 * the animation starts (in input), then the live value in tick; the first report of a
 * frame is the origin. For fades (MC_FADE) report the ink in % (0-100). Reports from
 * draw() are wrong (it runs ~30x a frame). Compiles to nothing on the watch. */
enum {
    MC_FINGER = 0,   /* 1:1 with a finger (pager drag, sheets, list drag): no step budget */
    MC_MOMENTUM,     /* after release: fling decay, snap / settle springs (<= 64 px/f unless
                        still faster than the release), ends by a 1 px snap */
    MC_TRANSITION,   /* screen transitions: <= 64 px/f, 7-12 frames, or a cut */
    MC_COMPONENT,    /* cards, banners, toasts, sheets settling: <= 64 (prefer 32), 7-9 frames */
    MC_MICRO,        /* toggles, pressed pills, chips, badges: <= 16 px/f, 3-6 frames */
    MC_TRACKED,      /* scrolls, drifting art, hands, marquees: <= 16 px/f, whole px, even */
    MC_DISCRETE,     /* digits, values, states: no spatial step (morph / flip in place) */
    MC_DECORATIVE,   /* breathing, pulses, sparkle: <= 16 px/f, may run on twos */
    MC_FADE,         /* ink level in %: 16 perceptual levels, 3-9 frames, no sub-level creep */
    MC_FILL,         /* progress sweeping in (rings, bars; arc px): <= 64 px/f, 12-45 frames,
                      * even (peak <= 1.6x avg), a gentle start allowed, crisp landing */
    MC_COUNT
};
#ifdef GFX_COST_MODEL
/* Simulator reference testing only; null keeps normal adaptive admission. */
extern int (*nos_quality_decision)(unsigned frame,int kind,unsigned key,int proposed);
void nos_motion_probe(const char *name, int cls, float px);
#else
#define nos_motion_probe(name, cls, px) ((void)(name), (void)(cls), (void)(px))
#endif
/* Always-on display: the host sets this while the panel is dimmed; the root face draws
 * its sparse AOD variant (<10% lit pixels, slow drift against burn-in). */
extern int nos_aod;

#endif
