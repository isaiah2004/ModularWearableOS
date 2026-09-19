/* wos.h — the watch OS: event dispatch, app lifecycle, edge-menu overlays.
 *
 * Navigation model (watchOS/Wear hybrid the user asked for):
 *   - the current app fills the screen (watchface is app 0)
 *   - each screen edge can host an overlay opened by an inward edge-swipe:
 *       LEFT   -> honeycomb app launcher
 *       RIGHT  -> vertical app list (Android-style)
 *       TOP    -> notifications
 *       BOTTOM -> control center
 *   - overlays slide in under the finger and spring-settle; swipe back
 *     toward the origin edge (or tap empty space) to dismiss.
 * Overlays are just wos_app screens registered against an edge. */
#ifndef WATCH_WOS_H
#define WATCH_WOS_H

#include "gfx.h"
#include "hal.h"

typedef enum {
    WOS_EV_TICK,        /* once per frame; x = ms since last frame */
    WOS_EV_TOUCH_DOWN,  /* raw pointer events; apps and overlays can handle drags */
    WOS_EV_TOUCH_MOVE,
    WOS_EV_TOUCH_UP,
    WOS_EV_TAP,
    WOS_EV_LONG_PRESS,
    WOS_EV_SWIPE_UP,
    WOS_EV_SWIPE_DOWN,
    /* mid-screen horizontal swipes reach the app; edge swipes open overlays */
    WOS_EV_SWIPE_LEFT,
    WOS_EV_SWIPE_RIGHT,
} wos_event_type;

typedef struct {
    wos_event_type type;
    int x, y;
} wos_event;

typedef struct wos_app {
    const char *name;
    void (*on_start)(void);
    /* Return nonzero to consume the event. For overlay TAPs, returning 0
     * tells the OS to dismiss the overlay (tap-on-empty-space to close). */
    int  (*on_event)(const wos_event *ev);
    void (*on_draw)(gfx_fb *fb);
    uint16_t accent;    /* icon color in launcher / list */
    uint8_t  icon;      /* ICON_* id (icons.h), 0 = initial-letter fallback */
} wos_app;

enum { WOS_EDGE_LEFT = 0, WOS_EDGE_RIGHT, WOS_EDGE_TOP, WOS_EDGE_BOTTOM };
enum { WOS_OV_PANNABLE = 1 };  /* overlay owns 2D drags (e.g. the launcher) */

void wos_init(uint16_t *framebuffer, const wos_app *const *apps, int napps);
void wos_register_overlay(int edge, const wos_app *screen, int flags);

/* Banded (stripe) rendering. Instead of drawing a whole full-screen frame into
 * one (slow, PSRAM) buffer, the OS renders horizontal bands of `stripe_h` rows
 * into two small internal-SRAM buffers, ping-ponged so a band's DMA overlaps
 * the next band's render. The platform supplies:
 *   wait_free() — block until a stripe buffer is free to overwrite (its prior
 *                 DMA has completed); called once before each band is drawn.
 *   flush(buf, y0, rows) — start the async transfer of a freshly drawn band.
 * If never called, wos_run_frame() falls back to a single full-frame draw into
 * the wos_init() framebuffer (the simulator path). */
void wos_set_stripes(uint16_t *buf_a, uint16_t *buf_b, int stripe_h,
                     void (*wait_free)(void),
                     void (*flush)(uint16_t *buf, int x0, int y0, int w, int rows));

/* Optional vsync gate. If set, the OS calls it once per frame that has pixels
 * to draw, right before flushing, so the platform can block until the panel's
 * TE (tearing-effect / vblank) pulse. This paces rendering to the panel's real
 * refresh and lands writes in vblank (tear-free). NULL = free-running. */
void wos_set_vsync(void (*wait_vsync)(void));

/* Optional atomic-present hooks for big full-screen repaints. begin(atomic) is
 * called before the raster pass and end(atomic) after it. When atomic!=0 the
 * host should accumulate this frame's stripe flushes into a back-buffer and, in
 * end(), TE-sync then push the whole frame in one transfer (no band, no tear).
 * When atomic==0 the host flushes per-stripe as usual. NULL = disabled. */
void wos_set_frame_hooks(void (*begin)(int atomic, int x0, int y0, int x1, int y1),
                         void (*end)(int atomic));

/* Declare an EXPLICIT dirty rect for this frame (screen coords). When an app
 * calls this during its on_draw record pass, the OS repaints exactly the rects
 * it declares — verbatim, no auto-merge into bands. Give each moving element
 * (icon, scrollbar thumb) ONE rect covering its OLD->NEW sweep this frame; that
 * box then self-clears the vacated spot, so each element stays a tight box
 * instead of the damage engine chaining them into one tall band that tears and
 * drops fps. The app is responsible for declaring every region it changes.
 * Ignored while an overlay is sliding open/closed or on a forced full repaint. */
void wos_dirty(int x0, int y0, int x1, int y1);

/* HAL feeds raw touch state (down = finger present). */
void wos_pointer(int x, int y, int down);

/* Briefly show a centred toast (e.g. "Sleep", "Power Off", "Always-On") over the whole UI for
 * `ms` milliseconds — used for power/button-action feedback. */
void wos_callout(const char *msg, int ms);

/* Scan-fade crossfade between two states of the CURRENT app: snapshots the screen, runs `apply`
 * to mutate the app's own state, snapshots again, then sweeps. axis 0=horizontal / 1=vertical,
 * dir +1 sweeps the new screen in from the left/top, -1 from the right/bottom. */
void wos_app_crossfade(int axis, int dir, void (*apply)(void));

/* Render one frame. Returns nonzero if it should be flushed. */
int  wos_run_frame(void);

/* Bumped once per frame. A face draw runs once per stripe, so guard any
 * frame-invariant work (sensor reads, string formatting, heavy math) behind
 * a change in this value and cache the result across the stripe passes. */
extern uint32_t wos_frame_id;

/* App registry — used by the launcher / app-list overlays. */
int         wos_app_count(void);
const char *wos_app_name(int i);
uint16_t    wos_app_accent(int i);
int         wos_app_icon(int i);
int         wos_current_app(void);
int         wos_app_visible(int i);  /* settled foreground app, no overlay */
void        wos_open_app(int i);     /* switch app, dismiss any overlay */
void        wos_close_overlay(void); /* dismiss the active overlay */

#endif
