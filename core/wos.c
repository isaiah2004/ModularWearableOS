#include "wos.h"
#include "anim.h"
#include "font.h"
#include "battery.h"
#include <stdio.h>
#include <string.h>

/* Debug HUD: FPS + last touch coords, drawn over everything. Toggle with
 * wos_debug_hud (default on). Handy for on-device frame-rate / touch tuning. */
int wos_debug_hud = 0;   /* on-screen fps pill: OFF by default (its per-frame redraw would defeat
                          * the static-screen power saving); toggle with the `hud` serial command */
static float s_hud_fps;

/* Debug "redraw boxes": outline every rectangle actually sent to the panel this frame
 * (green; red = the frame went out as one atomic band). Last frame's outlines are
 * erased by also repainting their 2px edges, unoutlined, wherever this frame's rects
 * don't already cover them - so a static screen ends up clean. Costs extra flushes,
 * and the extra rects keep frames off the atomic path while it's on. */
int wos_debug_rects = 0;
static int s_stream_hint;   /* this frame asked for streamed (non-atomic) present */
void wos_stream_hint(void) { s_stream_hint = 1; }
static int s_flat_hint;     /* this frame is Flat60 palette content */
void wos_flat_hint(void) { s_flat_hint = 1; }
int  wos_frame_flat(void) { return s_flat_hint; }
int wos_fast_display = 0;   /* 8-bit panel wire format (see wos.h) */
int wos_fps_cap48 = 0;   /* host paces frames at 48fps instead of 60 (see wos.h) */
#define DBG_EDGE 2
static int s_dbg_prev[28][4], s_dbg_prev_n;

#define SLOP          (GFX_W / 24)
#define EDGE_MARGIN   (GFX_W / 8)      /* a drag starting this close to an
                                          edge is an overlay-open gesture */
#define VSWIPE_MIN    (GFX_W / 6)
#define SWIPE_MAX_MS  400
#define TAP_MAX_MS    350
#define LONG_PRESS_MS 600
#define NAV_OMEGA     16.0f
#define OV_FLICK      (GFX_W * 2.0f)   /* px/s to commit an open/close */

static gfx_fb s_fb;
static const wos_app *const *s_apps;
static int s_napps;
static int s_cur;

int wos_dbg_n, wos_dbg_bands, wos_dbg_rows;   /* profiling: cmds, dirty bands, dirty rows */
uint32_t wos_dbg_record_us, wos_dbg_raster_us, wos_dbg_present_us;

/* Increments once per rendered frame. Faces read it to compute frame-invariant
 * data (sensor reads, formatted strings, heavy math) exactly once instead of
 * repeating the work on every stripe pass. */
uint32_t wos_frame_id;

/* banded-render config (see wos_set_stripes); s_stripe_h == 0 => full frame */
static uint16_t *s_stripe[2];
static int s_stripe_h;
static void (*s_stripe_wait)(void);
static void (*s_stripe_flush)(uint16_t *, int, int, int, int, int);   /* buf,stride,x0,y0,w,rows */
int wos_scan_order;   /* 0 = row stripes: column bands (1/2) measured faster but tore worse on the panel */
static void (*s_vsync)(void);   /* wait for panel TE before flushing (optional) */
/* Frame hooks for atomic present: begin(atomic) is called before the raster
 * pass (host switches stripe flushes to accumulate into a back-buffer when
 * atomic); end(atomic) after it (host presents the whole frame in one TE-synced
 * transfer). Lets a big full-screen redraw appear at once — no band, no tear. */
static void (*s_frame_begin)(int atomic, int x0, int y0, int x1, int y1);
static void (*s_frame_end)(int atomic);

/* This frame's final dirty rects, exported for the sim debug overlay. */
int wos_dbg_rect[16][4];
int wos_dbg_nr;

/* App-declared EXPLICIT dirty rects (wos_dirty). When an app supplies them, the
 * engine repaints exactly those, verbatim — no auto-merge into bands. The app
 * declares one rect per moving element covering its OLD->NEW sweep, so each
 * element stays its own tight box (it self-clears the vacated spot). Lets a
 * scrolling list avoid the engine chaining everything into one tall band. */
static int s_ex[16][4], s_ex_n;      /* declared this frame */

void wos_dirty(int x0, int y0, int x1, int y1)
{
    if (x1 <= x0 || y1 <= y0) return;
    if (s_ex_n < 16) {
        s_ex[s_ex_n][0] = x0; s_ex[s_ex_n][1] = y0;
        s_ex[s_ex_n][2] = x1; s_ex[s_ex_n][3] = y1;
        s_ex_n++;
    }
}

/* Forces the next app frame to redraw the whole screen (after boot, an app
 * switch, or an overlay). Partial redraws only kick in once the panel is known
 * to already hold this app's full frame. */
uint32_t wos_dbg_dirty_pixels;
static int s_force_full = 1;
void wos_invalidate(void) { s_force_full = 1; }

/* Retained-mode damage: each frame is drawn once into a recorder (bbox+hash per
 * primitive), diffed against last frame to find the changed rows. */
static gfx_rec s_rec_a, s_rec_b;
static gfx_rec *s_rec_cur = &s_rec_a, *s_rec_prev = &s_rec_b;

/* registered edge overlays */
static const wos_app *s_ov[4];
static int s_ovf[4];

/* touch phase machine */
enum { PH_IDLE, PH_APP, PH_OPEN, PH_OV, PH_CLOSE, PH_TX };
static int s_phase = PH_IDLE;
static int s_sx, s_sy, s_lx, s_ly;
static uint32_t s_t0, s_tmove;
static int s_long_fired;
static int s_ovdecide;      /* PH_OV: -1 undecided, 0 route-to-overlay */

/* active overlay slide state */
static int s_active = -1;   /* edge index, or -1 */
static float s_off;         /* px displaced from open(0) toward the edge */
static float s_off_prev;
static float s_offvel;      /* px/s */
static spring_t s_spring;
static int s_settling;
static int s_pending_close;

static uint32_t s_last_frame_t;

/* ---- system-wide scan-fade crossfade -----------------------------------------------------
 * Any screen change renders the FROM and TO screens once each into two PSRAM full-frame buffers,
 * then a soft ~TX_FADE-wide front sweeps across (memcpy solids + blend the front; same trick as
 * app_music). `s_tx_prog` 0->1 is from A to B. For EDGE OVERLAYS the sweep FOLLOWS THE FINGER
 * (`s_tx_drag`): dragging in advances prog, backing off retreats it, and release commits (prog->1)
 * or cancels (prog->0). App/face switches just auto-run prog 0->1. axis 0=horizontal, 1=vertical. */
static uint16_t *s_txa, *s_txb;
static int   s_tx, s_tx_drag, s_tx_axis, s_tx_dir;
static float s_tx_prog, s_tx_tgt;       /* prog now / animation target when not dragging */
static int   s_tx_to_active;            /* s_active to apply on COMMIT (-1 = an app/close) */
static int   s_tx_to_app;               /* app index to switch to on commit, or -1 */
static int   s_tx_edge, s_tx_open;      /* which edge the drag is on; 1=opening, 0=closing */
int wos_app_visible(int i) { return s_napps>0 && s_cur==i && s_active<0 && !s_tx; }
static char     s_callout[24];          /* transient power/action toast */
static uint32_t s_callout_until;
#define TX_FADE 34
#define TX_STEP_PROG 0.16f              /* prog/frame for the non-drag settle (~6 frames) */
/* Transition damage. Each frame only the strip the fade front swept since the
 * last presented frame changes (|dp| + TX_FADE px), so the transition repaints
 * that band instead of the whole screen. The FROM/TO snapshots also record their
 * draw commands, so when the fade ends the real screen diffs against the
 * snapshot it lands on (plus the unswept tail) instead of a forced full repaint. */
static int  s_tx_p_prev, s_tx_p_cur;    /* sweep position last presented / this frame */
static gfx_rec *s_rec_snap[2];          /* recordings of the FROM (0) / TO (1) snapshots */
static int  s_tx_tail[4], s_tx_tail_ok; /* commit frame: the strip still showing the fade */
static int  wos_crossfade_begin(int axis, int dir, int to_active, int to_app, int drag);

/* ---- geometry helpers ---- */
static int axis_is_x(int e)   { return e == WOS_EDGE_LEFT || e == WOS_EDGE_RIGHT; }
static float edge_full(int e) { return axis_is_x(e) ? GFX_W : GFX_H; }

/* how far the finger has moved inward from the origin edge */
static int inward_delta(int e, int x, int y)
{
    switch (e) {
    case WOS_EDGE_LEFT:   return x - s_sx;
    case WOS_EDGE_RIGHT:  return s_sx - x;
    case WOS_EDGE_TOP:    return y - s_sy;
    default:              return s_sy - y;   /* BOTTOM */
    }
}

static int edge_at(int x, int y)
{
    int best = -1, bestd = EDGE_MARGIN;
    if (x < bestd)             { bestd = x;             best = WOS_EDGE_LEFT; }
    if (GFX_W - 1 - x < bestd) { bestd = GFX_W - 1 - x; best = WOS_EDGE_RIGHT; }
    if (y < bestd)             { bestd = y;             best = WOS_EDGE_TOP; }
    if (GFX_H - 1 - y < bestd) { bestd = GFX_H - 1 - y; best = WOS_EDGE_BOTTOM; }
    return best;
}

static int inward_dominant(int e, int dx, int dy)
{
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    switch (e) {
    case WOS_EDGE_LEFT:   return dx > 0 && adx > ady;
    case WOS_EDGE_RIGHT:  return dx < 0 && adx > ady;
    case WOS_EDGE_TOP:    return dy > 0 && ady > adx;
    default:              return dy < 0 && ady > adx;
    }
}

static int toward_edge_dominant(int e, int dx, int dy)
{
    return inward_dominant(e, -dx, -dy);
}

/* ---- dispatch ---- */
static void app_dispatch(wos_event_type t, int x, int y)
{
    wos_event ev = { t, x, y };
    if (s_apps[s_cur]->on_event)
        s_apps[s_cur]->on_event(&ev);
}

static int ov_dispatch(wos_event_type t, int x, int y)
{
    wos_event ev = { t, x, y };
    const wos_app *o = s_active >= 0 ? s_ov[s_active] : 0;
    return (o && o->on_event) ? o->on_event(&ev) : 0;
}

/* ---- registry API ---- */
int         wos_app_count(void)    { return s_napps; }
const char *wos_app_name(int i)    { return s_apps[i]->name; }
uint16_t    wos_app_accent(int i)  { return s_apps[i]->accent; }
int         wos_app_icon(int i)    { return s_apps[i]->icon; }
int         wos_current_app(void)  { return s_cur; }

static void spring_settle(float target)
{
    s_spring.pos = s_off;
    s_spring.vel = s_offvel;
    s_spring.omega = NAV_OMEGA;
    spring_to(&s_spring, target);
    s_settling = 1;
    s_pending_close = (target >= edge_full(s_active) - 0.5f);
}

/* edge -> (sweep axis, sweep direction) for opening; closing reverses the direction. */
static int edge_axis(int e) { return axis_is_x(e) ? 0 : 1; }
static int edge_dir(int e)  { return (e == WOS_EDGE_LEFT || e == WOS_EDGE_TOP) ? +1 : -1; }

/* while dragging an edge crossfade, map the finger to prog 0..1 (opening = inward, closing = out) */
static void update_tx_prog(int x, int y)
{
    float d = (float)inward_delta(s_tx_edge, x, y);
    if (!s_tx_open) d = -d;                       /* closing: progress = movement toward the edge */
    float full = edge_full(s_tx_edge);
    float pr = full > 0 ? d / full : 0;
    if (pr < 0) pr = 0;
    if (pr > 1) pr = 1;
    s_tx_prog = pr;
}

void wos_close_overlay(void)
{
    if (s_active >= 0) {
        int e = s_active;
        s_phase = PH_IDLE;
        wos_crossfade_begin(edge_axis(e), -edge_dir(e), -1, -1, 0);   /* auto close */
    }
}

void wos_open_app(int i)
{
    if (i < 0 || i >= s_napps)
        return;
    if (i == s_cur && s_active < 0)      /* already showing it */
        return;
    wos_crossfade_begin(0, +1, -1, i, 0);      /* auto app switch */
}

void wos_set_stripes(uint16_t *buf_a, uint16_t *buf_b, int stripe_h,
                     void (*wait_free)(void),
                     void (*flush)(uint16_t *, int, int, int, int, int))
{
    s_stripe[0] = buf_a;
    s_stripe[1] = buf_b;
    s_stripe_h = stripe_h;
    s_stripe_wait = wait_free;
    s_stripe_flush = flush;
}

void wos_set_vsync(void (*wait_vsync)(void)) { s_vsync = wait_vsync; }
void wos_set_frame_hooks(void (*begin)(int, int, int, int, int), void (*end)(int))
{
    s_frame_begin = begin;
    s_frame_end = end;
}

void wos_init(uint16_t *framebuffer, const wos_app *const *apps, int napps)
{
    s_fb.px = framebuffer;
    s_fb.ox = s_fb.oy = 0;
    s_fb.y_base = 0;
    s_fb.h = GFX_H;
    s_fb.cx0 = 0; s_fb.cx1 = GFX_W;
    s_apps = apps;
    s_napps = napps;
    s_cur = 0;
    s_last_frame_t = hal_millis();
    if (s_apps[s_cur]->on_start)
        s_apps[s_cur]->on_start();
}

void wos_register_overlay(int edge, const wos_app *screen, int flags)
{
    if (edge >= 0 && edge < 4) {
        s_ov[edge] = screen;
        s_ovf[edge] = flags;
    }
}

/* ---- pointer ---- */
static void track_off(int x, int y, uint32_t now)
{
    uint32_t dt = now - s_tmove;
    if (dt == 0) dt = 1;
    float v = (s_off - s_off_prev) * 1000.0f / dt;
    s_offvel = 0.6f * v + 0.4f * s_offvel;
    s_off_prev = s_off;
    s_tmove = now;
}

void wos_pointer(int x, int y, int down)
{
    uint32_t now = hal_millis();

    if (s_tx && !s_tx_drag) return;      /* auto crossfade: ignore input; a drag one tracks below */

    if (down && s_phase == PH_IDLE) {
        s_sx = s_lx = x;
        s_sy = s_ly = y;
        s_t0 = s_tmove = now;
        s_long_fired = 0;
        s_offvel = 0;
        if (s_active >= 0 && !s_settling) {
            s_phase = PH_OV;
            s_ovdecide = -1;
        } else {
            s_phase = PH_APP;
            app_dispatch(WOS_EV_TOUCH_DOWN, x, y);   /* apps can opt into raw drags */
        }
        return;
    }

    if (down && s_phase != PH_IDLE) {
        if (x == s_lx && y == s_ly)
            return;
        s_lx = x; s_ly = y;
        int dx = x - s_sx, dy = y - s_sy;
        int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;

        switch (s_phase) {
        case PH_APP:
            if (adx > SLOP || ady > SLOP) {
                int e = edge_at(s_sx, s_sy);
                if (e >= 0 && s_ov[e] && inward_dominant(e, dx, dy)) {
                    app_dispatch(WOS_EV_TOUCH_UP, x, y); /* overlay takes ownership */
                    s_tx_edge = e; s_tx_open = 1;     /* finger-driven scan-fade to the overlay */
                    if (wos_crossfade_begin(edge_axis(e), edge_dir(e), e, -1, 1)) {
                        s_phase = PH_TX;
                        update_tx_prog(x, y);
                    } else s_phase = PH_IDLE;
                    return;
                }
            }
            app_dispatch(WOS_EV_TOUCH_MOVE, x, y);   /* not opening an overlay -> raw drag to the app */
            break;
        case PH_OPEN: {
            float in = inward_delta(s_active, x, y);
            float full = edge_full(s_active);
            if (in < 0) in = 0;
            if (in > full) in = full;
            s_off = full - in;
            track_off(x, y, now);
            break;
        }
        case PH_OV:
            if (s_ovdecide == -1 && (adx > SLOP || ady > SLOP)) {
                if (!(s_ovf[s_active] & WOS_OV_PANNABLE) &&
                    toward_edge_dominant(s_active, dx, dy)) {
                    int e = s_active;                /* swipe back to the edge -> scan-fade home */
                    s_tx_edge = e; s_tx_open = 0;
                    if (wos_crossfade_begin(edge_axis(e), -edge_dir(e), -1, -1, 1)) {
                        s_phase = PH_TX;
                        update_tx_prog(x, y);
                    } else s_phase = PH_IDLE;
                    return;
                } else {
                    s_ovdecide = 0;
                    ov_dispatch(WOS_EV_TOUCH_DOWN, s_sx, s_sy);
                    ov_dispatch(WOS_EV_TOUCH_MOVE, x, y);
                }
            } else if (s_ovdecide == 0) {
                ov_dispatch(WOS_EV_TOUCH_MOVE, x, y);
            }
            break;
        case PH_CLOSE: {
            float out = -inward_delta(s_active, x, y);
            float full = edge_full(s_active);
            if (out < 0) out = 0;
            if (out > full) out = full;
            s_off = out;
            track_off(x, y, now);
            break;
        }
        case PH_TX:                                  /* scan-fade front follows the finger */
            update_tx_prog(x, y);
            break;
        }
        return;
    }

    if (!down && s_phase != PH_IDLE) {
        int ph = s_phase;
        s_phase = PH_IDLE;
        int dx = x - s_sx, dy = y - s_sy;
        int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
        uint32_t dt = now - s_t0;

        switch (ph) {
        case PH_APP:
            app_dispatch(WOS_EV_TOUCH_UP, x, y);     /* end of any raw drag */
            if (s_long_fired) break;
            if (dt <= SWIPE_MAX_MS && ady >= VSWIPE_MIN && ady > adx)
                app_dispatch(dy < 0 ? WOS_EV_SWIPE_UP : WOS_EV_SWIPE_DOWN, x, y);
            else if (dt <= SWIPE_MAX_MS && adx >= VSWIPE_MIN && adx > ady)
                app_dispatch(dx < 0 ? WOS_EV_SWIPE_LEFT : WOS_EV_SWIPE_RIGHT, x, y);
            else if (dt <= TAP_MAX_MS && adx <= SLOP && ady <= SLOP)
                app_dispatch(WOS_EV_TAP, x, y);
            break;
        case PH_OPEN: {
            float full = edge_full(s_active);
            int open = s_off < full * 0.5f || s_offvel < -OV_FLICK;
            spring_settle(open ? 0.0f : full);
            break;
        }
        case PH_OV:
            if (s_ovdecide <= 0) {
                /* Once movement claimed the gesture, returning to its start
                 * (e.g. one dial revolution) is still a drag, never a tap. */
                if (s_ovdecide == 0) {
                    ov_dispatch(WOS_EV_TOUCH_UP, x, y);
                } else if (dt <= TAP_MAX_MS && adx <= SLOP && ady <= SLOP) {
                    if (!ov_dispatch(WOS_EV_TAP, x, y))
                        wos_close_overlay();
                }
            }
            break;
        case PH_CLOSE: {
            float full = edge_full(s_active);
            int close = s_off > full * 0.33f || s_offvel > OV_FLICK;
            spring_settle(close ? full : 0.0f);
            break;
        }
        case PH_TX:                                  /* release: commit past a third, else snap back */
            s_tx_drag = 0;
            s_tx_tgt = (s_tx_prog > 0.33f) ? 1.0f : 0.0f;
            break;
        }
    }
}

/* ---- frame ---- */
/* Draw the overlay, adding its slide displacement on top of whatever base
 * translate `fb` already carries (the stripe base oy = -y0). */
static void draw_overlay_translated(gfx_fb *fb)
{
    int off = (int)(s_off + 0.5f);
    int sox = fb->ox, soy = fb->oy;
    if (axis_is_x(s_active))
        fb->ox += (s_active == WOS_EDGE_LEFT) ? -off : off;
    else
        fb->oy += (s_active == WOS_EDGE_TOP) ? -off : off;
    s_ov[s_active]->on_draw(fb);
    fb->ox = sox;
    fb->oy = soy;
}

/* HUD text prepared once per frame in wos_run_frame, drawn per stripe here. */
static char s_hud_buf[28];

/* Draw a complete frame's worth of content into `fb`. Called once for a full
 * frame, or once per band (with fb->oy/h set) for a stripe render — every
 * primitive self-clips to fb, so a band only paints its own rows. */
static void draw_wos_transition(gfx_fb *fb);

static void wos_draw_all(gfx_fb *fb)
{
    if (s_tx) { draw_wos_transition(fb); return; }   /* scan-fade owns the whole frame */

    if (s_active < 0 || s_off > 0.5f)
        s_apps[s_cur]->on_draw(fb);
    if (s_active >= 0)
        draw_overlay_translated(fb);

    if (wos_debug_hud) {
        int cx = (int)GFX_CX, cy = (int)GFX_S(32);   /* lowered a little */
        int w = (int)GFX_S(38), h = (int)GFX_S(20), r = h / 2;
        int x0 = cx - w / 2;
        uint16_t bg = GFX_RGB(18, 18, 22);
        gfx_fill_circle(fb, x0 + r, cy, r, bg);          /* rounded pill ends */
        gfx_fill_circle(fb, x0 + w - r, cy, r, bg);
        gfx_fill_rect(fb, x0 + r, cy - r, w - 2 * r, h, bg);
        font_draw_centered(fb, &font_small, cx,
                           cy + (int)(font_small.ascent * 0.34f), s_hud_buf, GFX_GREEN);
    }

    if (s_callout[0] && hal_millis() < s_callout_until) {   /* power/action toast, lower-centre pill */
        /* small font, text baseline centred IN the pill (y here is a baseline —
         * the old -line_height/2 put the text half out the pill's top) */
        int cx = (int)GFX_CX, cy = (int)GFX_CY + (int)GFX_S(62);
        int tw = font_text_width(&font_small, s_callout);
        int h = font_small.line_height + (int)GFX_S(8), r = h / 2;
        int w = tw + (int)GFX_S(18);
        uint16_t bg = GFX_RGB(24, 24, 30);
        gfx_fill_circle(fb, cx - w / 2 + r, cy, r, bg);
        gfx_fill_circle(fb, cx + w / 2 - r, cy, r, bg);
        gfx_fill_rect(fb, cx - w / 2 + r, cy - r, w - 2 * r, h, bg);
        font_draw_centered(fb, &font_small, cx,
                           cy + (int)(font_small.ascent * 0.34f), s_callout, GFX_WHITE);
    }
}

/* Render the CURRENT screen state (app + overlay, no HUD) into a full-frame PSRAM buffer,
 * and record its draw commands into snapshot `snap` (same frame id, so the recording
 * matches the pixels) to serve as the damage baseline when the transition ends. */
static void render_state_to(uint16_t *buf, int snap)
{
    gfx_fb fb;
    fb.px = buf; fb.ox = fb.oy = 0; fb.y_base = 0; fb.h = GFX_H;
    fb.cx0 = 0; fb.cx1 = GFX_W; fb.rec = 0; fb.stride = fb.x_base = 0;
    int hud = wos_debug_hud; wos_debug_hud = 0;
    int tx = s_tx; s_tx = 0;               /* draw the real screen, not the transition */
    wos_frame_id++;                        /* force apps that cache per-frame to recompute (A vs B) */
    wos_draw_all(&fb);
    if (!s_rec_snap[snap]) s_rec_snap[snap] = hal_big_alloc(sizeof(gfx_rec));
    if (s_rec_snap[snap]) {
        int ex_n = s_ex_n;
        gfx_fb r = fb;
        r.px = 0; r.rec = s_rec_snap[snap];
        r.rec->n = 0; r.rec->overflow = 0;
        wos_draw_all(&r);
        s_ex_n = ex_n;                     /* snapshot dirty declarations are not this frame's */
    }
    s_tx = tx; wos_debug_hud = hud;
}

/* Sweep position (px along the axis) for progress `prog`. */
static int tx_pos(float prog)
{
    float span = (float)(s_tx_axis ? GFX_H : GFX_W);
    return (int)((s_tx_dir > 0) ? prog * span : span * (1.0f - prog));
}
/* Strip [lo,hi) along the axis whose pixels differ between sweep positions a and b. */
static void tx_band(int a, int b, int *lo, int *hi)
{
    int span = s_tx_axis ? GFX_H : GFX_W;
    int pa = a < b ? a : b, pb = a < b ? b : a;
    *lo = s_tx_dir > 0 ? pa : pa - TX_FADE;
    *hi = s_tx_dir > 0 ? pb + TX_FADE : pb;
    if (*lo < 0) *lo = 0;
    if (*hi > span) *hi = span;
    *lo &= ~1; *hi = (*hi + 1) & ~1; if (*hi > span) *hi = span;
}
static void tx_band_rect(int a, int b, int r[4])
{
    int lo, hi;
    tx_band(a, b, &lo, &hi);
    if (s_tx_axis == 0) { r[0] = lo; r[1] = 0; r[2] = hi; r[3] = GFX_H; }
    else                { r[0] = 0; r[1] = lo; r[2] = GFX_W; r[3] = hi; }
}

/* Composite the crossfade: solid A/B fields memcpy'd, only the ~TX_FADE front blended. The
 * recorded command is full-screen, but wos_run_frame repaints just the swept band (tx_band). */
static void draw_wos_transition(gfx_fb *fb)
{
    int p = tx_pos(s_tx_prog);
    if (fb->rec) s_tx_p_cur = p;
    if (gfx_rec_cmd(fb, 0, 0, GFX_W, GFX_H,
                    gfx_hash_u(0x7c0ffeeu, (unsigned)(p + s_tx_axis * 100000))))
        return;
    if (!s_txa || !s_txb) return;
    int c0 = fb->cx0, c1 = fb->cx1;                    /* only the columns this buffer holds */
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (c0 < 0) c0 = 0;
    if (c1 > GFX_W) c1 = GFX_W;
    if (s_tx_axis == 0) {                              /* horizontal sweep */
        int f0, f1;
        if (s_tx_dir > 0) { f0 = p; f1 = p + TX_FADE; }
        else              { f0 = p - TX_FADE; f1 = p; }
        if (f0 < 0) f0 = 0;
        if (f1 > GFX_W) f1 = GFX_W;
        if (f0 > GFX_W) f0 = GFX_W;
        if (f1 < 0) f1 = 0;
        uint8_t wt[TX_FADE + 1];
        for (int c = f0; c < f1; c++) {
            float al = (s_tx_dir > 0) ? (float)(p + TX_FADE - c) / (float)TX_FADE
                                      : (float)(c - p + TX_FADE) / (float)TX_FADE;
            if (al < 0) al = 0;
            if (al > 1) al = 1;
            wt[c - f0] = (uint8_t)(al * 255.0f);
        }
        uint16_t *lb = (s_tx_dir > 0) ? s_txb : s_txa, *rb = (s_tx_dir > 0) ? s_txa : s_txb;
        int l1 = f0 < c1 ? f0 : c1, b0 = f0 > c0 ? f0 : c0, b1 = f1 < c1 ? f1 : c1, r0 = f1 > c0 ? f1 : c0;
        for (int y = 0; y < fb->h; y++) {
            int ly = y + fb->y_base;
            uint16_t *ar = &s_txa[ly * GFX_W], *br = &s_txb[ly * GFX_W], *dr = gfx_row(fb, y);
            if (l1 > c0) memcpy(dr + c0, &lb[ly * GFX_W + c0], (size_t)(l1 - c0) * 2);
            for (int c = b0; c < b1; c++) dr[c] = gfx_blend565(ar[c], br[c], wt[c - f0]);
            if (c1 > r0) memcpy(dr + r0, &rb[ly * GFX_W + r0], (size_t)(c1 - r0) * 2);
        }
    } else {                                           /* vertical sweep */
        int g0, g1;
        if (s_tx_dir > 0) { g0 = p; g1 = p + TX_FADE; }
        else              { g0 = p - TX_FADE; g1 = p; }
        uint16_t *tb = (s_tx_dir > 0) ? s_txb : s_txa, *bb = (s_tx_dir > 0) ? s_txa : s_txb;
        for (int y = 0; y < fb->h; y++) {
            int ly = y + fb->y_base;
            uint16_t *dr = gfx_row(fb, y);
            if (ly < g0)       memcpy(dr + c0, &tb[ly * GFX_W + c0], (size_t)(c1 - c0) * 2);
            else if (ly >= g1) memcpy(dr + c0, &bb[ly * GFX_W + c0], (size_t)(c1 - c0) * 2);
            else {
                float al = (s_tx_dir > 0) ? (float)(p + TX_FADE - ly) / (float)TX_FADE
                                          : (float)(ly - p + TX_FADE) / (float)TX_FADE;
                if (al < 0) al = 0;
                if (al > 1) al = 1;
                uint8_t w = (uint8_t)(al * 255.0f);
                uint16_t *ar = &s_txa[ly * GFX_W], *br = &s_txb[ly * GFX_W];
                for (int c = c0; c < c1; c++) dr[c] = gfx_blend565(ar[c], br[c], w);
            }
        }
    }
}

/* apply the committed TO state (on_start already ran while capturing B) */
static void tx_apply_to(void)
{
    if (s_tx_to_app >= 0) { s_cur = s_tx_to_app; s_active = -1; }
    else s_active = s_tx_to_active;
    s_off = 0; s_settling = 0; s_pending_close = 0;
}

/* Snapshot the FROM screen, build + snapshot the TO screen, then run/track the crossfade. The
 * state is left as FROM during the fade; tx_apply_to() commits it when prog reaches 1 (a cancelled
 * drag just stays FROM). `drag`=1 means the caller drives prog from the finger. */
static int wos_crossfade_begin(int axis, int dir, int to_active, int to_app, int drag)
{
    if (s_tx) return 0;                                /* one at a time */
    if (!s_txa) s_txa = hal_big_alloc((uint32_t)GFX_W * GFX_H * 2);
    if (!s_txb) s_txb = hal_big_alloc((uint32_t)GFX_W * GFX_H * 2);
    s_tx_to_active = to_active; s_tx_to_app = to_app;
    if (!s_txa || !s_txb) {                            /* no PSRAM -> apply instantly */
        if (to_app >= 0 && s_apps[to_app]->on_start) s_apps[to_app]->on_start();
        else if (to_active >= 0 && s_ov[to_active]->on_start) s_ov[to_active]->on_start();
        tx_apply_to(); s_force_full = 1; return 0;
    }
    render_state_to(s_txa, 0);                         /* FROM = current state */
    int sv_active = s_active, sv_cur = s_cur; float sv_off = s_off;
    if (to_app >= 0) { s_cur = to_app; if (s_apps[to_app]->on_start) s_apps[to_app]->on_start(); s_active = -1; }
    else { s_active = to_active; if (to_active >= 0 && s_ov[to_active]->on_start) s_ov[to_active]->on_start(); }
    s_off = 0;
    render_state_to(s_txb, 1);                         /* TO */
    s_active = sv_active; s_cur = sv_cur; s_off = sv_off;   /* revert; committed by tx_apply_to() */
    s_tx_axis = axis; s_tx_dir = dir;
    s_tx_prog = 0.0f; s_tx_tgt = 1.0f; s_tx_drag = drag;
    s_tx = 1; s_tx_p_prev = tx_pos(0.0f);   /* screen == FROM: only swept bands repaint */
    return 1;
}

void wos_callout(const char *msg, int ms)
{
    int i = 0;
    for (; msg[i] && i < (int)sizeof s_callout - 1; i++) s_callout[i] = msg[i];
    s_callout[i] = 0;
    s_callout_until = hal_millis() + (uint32_t)ms;
}

/* crossfade between two states of the CURRENT app (e.g. a watch-face swap). Renders the app now,
 * lets `apply` mutate the app's own state, renders again, then auto-sweeps. No OS commit needed —
 * the app already owns the new state (s_tx_to_app = -2 tells the finish handler to skip it). */
void wos_app_crossfade(int axis, int dir, void (*apply)(void))
{
    if (s_tx) { if (apply) apply(); return; }
    if (!s_txa) s_txa = hal_big_alloc((uint32_t)GFX_W * GFX_H * 2);
    if (!s_txb) s_txb = hal_big_alloc((uint32_t)GFX_W * GFX_H * 2);
    if (!s_txa || !s_txb) { if (apply) apply(); s_force_full = 1; return; }
    render_state_to(s_txa, 0);
    if (apply) apply();
    render_state_to(s_txb, 1);
    s_tx_axis = axis; s_tx_dir = dir; s_tx_to_active = -1; s_tx_to_app = -2;
    s_tx_prog = 0.0f; s_tx_tgt = 1.0f; s_tx_drag = 0;
    s_tx = 1; s_tx_p_prev = tx_pos(0.0f);
}

/* Diff this frame's recorder against last frame's, by index. Any command whose
 * bbox or param-hash changed contributes its OLD box (erase) and NEW box (draw)
 * to the dirty set; the y-intervals are sorted and merged into a few bands.
 * Returns band count (0 = nothing changed), or -1 to mean "redraw everything"
 * (structural change / too much churn / overflow). */
/* 2D dirty rectangles. Each rect is {x0,y0,x1,y1}. A thin change (a clock hand,
 * a moving glyph) yields a thin rect, so we repaint a column-limited region
 * instead of a full-width row band — the key to 60fps on busy static faces. */
#define WOS_MAXRECTS  28   /* headroom for staggered per-element updates (hive/list) */
#define WOS_MERGE_GAP 12

static int rect_near(const int *a, const int *b, int gap)
{
    return a[0] <= b[2] + gap && b[0] <= a[2] + gap &&
           a[1] <= b[3] + gap && b[1] <= a[3] + gap;
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
    return a->hash == b->hash && a->x0 == b->x0 && a->y0 == b->y0 &&
           a->x1 == b->x1 && a->y1 == b->y1;
}

/* Merge one command box into the dirty set. 0 = ok, -1 = too fragmented. */
static int damage_add(int rects[][4], int *nr, const gfx_cmd *c)
{
    if (c->x1 <= c->x0 || c->y1 <= c->y0) return 0;
    int box[4] = { c->x0, c->y0, c->x1, c->y1 };
    for (int r = 0; r < *nr; r++)
        if (rect_near(rects[r], box, WOS_MERGE_GAP)) { rect_union(rects[r], box); return 0; }
    if (*nr >= WOS_MAXRECTS) return -1;
    rects[*nr][0] = box[0]; rects[*nr][1] = box[1];
    rects[*nr][2] = box[2]; rects[*nr][3] = box[3]; (*nr)++;
    return 0;
}

static int compute_damage(gfx_rec *cur, gfx_rec *prev, int rects[][4])
{
    if (cur->overflow || prev->overflow)
        return -1;
    int nr = 0;
    if (cur->n == prev->n) {
        for (int i = 0; i < cur->n; i++) {
            gfx_cmd *a = &cur->c[i], *b = &prev->c[i];
            if (cmd_same(a, b)) continue;
            if (damage_add(rects, &nr, b) || damage_add(rects, &nr, a))   /* erase old, draw new */
                return -1;
        }
    } else {
        /* A command appeared or vanished (text changed length, a sweep split...).
         * Commands paint in order, so a pixel covered only by an identical common
         * prefix and suffix is unchanged: repaint just the differing middle, old
         * boxes and new, instead of the whole screen. */
        int n = cur->n < prev->n ? cur->n : prev->n, pre = 0, suf = 0;
        while (pre < n && cmd_same(&cur->c[pre], &prev->c[pre])) pre++;
        while (suf < n - pre && cmd_same(&cur->c[cur->n - 1 - suf], &prev->c[prev->n - 1 - suf])) suf++;
        for (int i = pre; i < prev->n - suf; i++)
            if (damage_add(rects, &nr, &prev->c[i])) return -1;
        for (int i = pre; i < cur->n - suf; i++)
            if (damage_add(rects, &nr, &cur->c[i])) return -1;
    }
    if (nr == 0) return 0;
    /* a union may now touch another rect — coalesce to a fixed point */
    for (int pass = 0, changed = 1; changed && pass < WOS_MAXRECTS * 2; pass++) {
        changed = 0;
        for (int i = 0; i < nr; i++)
            for (int j = i + 1; j < nr; j++)
                if (rect_near(rects[i], rects[j], WOS_MERGE_GAP)) {
                    rect_union(rects[i], rects[j]);
                    rects[j][0] = rects[nr-1][0]; rects[j][1] = rects[nr-1][1];
                    rects[j][2] = rects[nr-1][2]; rects[j][3] = rects[nr-1][3];
                    nr--; changed = 1; j--;
                }
    }
    for (int i = 0; i < nr; i++) {
        if (rects[i][0] < 0) rects[i][0] = 0;
        if (rects[i][1] < 0) rects[i][1] = 0;
        if (rects[i][2] > GFX_W) rects[i][2] = GFX_W;
        if (rects[i][3] > GFX_H) rects[i][3] = GFX_H;
        /* The QSPI AMOLED needs 2-pixel (32-bit) aligned partial writes: an odd
         * x-start or width shears the DMA'd sub-rect. Snap x-bounds to even. */
        rects[i][0] &= ~1;
        rects[i][2] = (rects[i][2] + 1) & ~1;
        if (rects[i][2] > GFX_W) rects[i][2] = GFX_W;   /* GFX_W is even */
        /* Snap y-bounds to even too. A rect tracking a smooth hand alternates an
         * even/odd row-start every frame; the panel places an odd-row partial
         * write one line off, so the redrawn region jitters 1px up/down. Keeping
         * the row-start even holds it steady (sim can't show this — no DMA). */
        rects[i][1] &= ~1;
        rects[i][3] = (rects[i][3] + 1) & ~1;
        if (rects[i][3] > GFX_H) rects[i][3] = GFX_H;   /* GFX_H is even */
    }
    return nr;
}

/* Render + flush one dirty rectangle [rx0,rx1) x [ry0,ry1). On device it walks
 * the rows in stripe-sized chunks into the two DMA buffers (ping-ponged via
 * *slot) and flushes only the [rx0,rx1) sub-rectangle; in the simulator it
 * renders in place. wos_draw_all draws the whole screen, but gfx_clear and the
 * layer blit honour fb.cx0/cx1 so only this column span is actually produced. */
/* Debug: paint the DBG_EDGE-wide border of rect [rx0,rx1)x[ry0,ry1) into the rows of
 * this stripe (buffer row 0 = logical y fb->y_base). */
static void dbg_outline(gfx_fb *fb, int rx0, int ry0, int rx1, int ry1, uint16_t col)
{
    int c0 = fb->cx0 > rx0 ? fb->cx0 : rx0, c1 = fb->cx1 < rx1 ? fb->cx1 : rx1;   /* band clip */
    for (int r = 0; r < fb->h; r++) {
        int y = fb->y_base + r;
        uint16_t *row = gfx_row(fb, r);
        if (y < ry0 + DBG_EDGE || y >= ry1 - DBG_EDGE) {
            for (int x = c0; x < c1; x++) row[x] = col;
        } else {
            for (int x = c0; x < c1; x++)
                if (x < rx0 + DBG_EDGE || x >= rx1 - DBG_EDGE) row[x] = col;
        }
    }
}

/* Column band [bx0,bx1) x [ry0,ry1) of dirty rect `r` (device only): the whole
 * height in one buffer, packed bx1-bx0 pixels per row. */
static void render_band(int bx0, int bx1, int ry0, int ry1, const int r[4], int *slot,
                        uint16_t outline)
{
    gfx_fb fb;
    fb.ox = fb.oy = 0;
    fb.y_base = ry0;
    fb.h = ry1 - ry0;
    fb.cx0 = bx0;
    fb.cx1 = bx1;
    fb.rec = 0;
    fb.stride = bx1 - bx0;
    fb.x_base = bx0;
    if (s_stripe_wait) s_stripe_wait();
    fb.px = s_stripe[*slot];
    uint32_t raster_start = hal_micros();
    wos_draw_all(&fb);
    if (outline) dbg_outline(&fb, r[0], r[1], r[2], r[3], outline);
    wos_dbg_raster_us += hal_micros() - raster_start;
    s_stripe_flush(fb.px, fb.stride, bx0, ry0, bx1 - bx0, fb.h);
    *slot ^= 1;
}

/* Scan-ordered raster: cut every rect into column bands as wide as a stripe
 * buffer allows for its height, then emit bands from all rects by x so the panel
 * receives the frame in the direction its scan sweeps (dir -1 = right->left). */
static void render_bands(int (*rects)[4], int nrects, int npaint, uint16_t dbg_col,
                         int dir, int *slot)
{
    static int cur[WOS_MAXRECTS * 5], bw[WOS_MAXRECTS * 5];   /* off the main task's stack */
    int cap = s_stripe_h * GFX_W;
    for (int i = 0; i < nrects; i++) {
        int h = rects[i][3] - rects[i][1];
        bw[i] = h > 0 ? (cap / h) & ~1 : 0;
        if (bw[i] < 2) bw[i] = 2;
        cur[i] = dir < 0 ? rects[i][2] : rects[i][0];
    }
    for (;;) {
        int best = -1;
        for (int i = 0; i < nrects; i++) {
            if (rects[i][3] <= rects[i][1] || rects[i][2] <= rects[i][0]) continue;
            if (dir < 0 ? cur[i] <= rects[i][0] : cur[i] >= rects[i][2]) continue;
            if (best < 0 || (dir < 0 ? cur[i] > cur[best] : cur[i] < cur[best])) best = i;
        }
        if (best < 0) break;
        int *r = rects[best], b0, b1;
        if (dir < 0) { b1 = cur[best]; b0 = b1 - bw[best]; if (b0 < r[0]) b0 = r[0]; cur[best] = b0; }
        else         { b0 = cur[best]; b1 = b0 + bw[best]; if (b1 > r[2]) b1 = r[2]; cur[best] = b1; }
        render_band(b0, b1, r[1], r[3], r, slot, best < npaint ? dbg_col : 0);
    }
}

static void render_rect(int rx0, int ry0, int rx1, int ry1, int *slot, uint16_t outline)
{
    if (rx1 <= rx0 || ry1 <= ry0) return;
    int chunk = s_stripe_h > 0 ? s_stripe_h : (ry1 - ry0);
    if (chunk <= 0) return;
    for (int y0 = ry0; y0 < ry1; y0 += chunk) {
        int rows = ry1 - y0 < chunk ? ry1 - y0 : chunk;
        gfx_fb fb;
        fb.ox = fb.oy = 0;
        fb.y_base = y0;
        fb.h = rows;
        fb.cx0 = rx0;
        fb.cx1 = rx1;
        fb.rec = 0;   /* rasterize, don't record */
        fb.stride = fb.x_base = 0;
        if (s_stripe_h > 0) {
            if (s_stripe_wait) s_stripe_wait();      /* buffer free to reuse */
            fb.px = s_stripe[*slot];
            uint32_t raster_start=hal_micros();
            wos_draw_all(&fb);
            if (outline) dbg_outline(&fb, rx0, ry0, rx1, ry1, outline);
            wos_dbg_raster_us+=hal_micros()-raster_start;
            s_stripe_flush(fb.px + rx0, GFX_W, rx0, y0, rx1 - rx0, rows);   /* DMA sub-rect */
            *slot ^= 1;
        } else {
            fb.px = &s_fb.px[y0 * GFX_W];            /* sim: render in place */
            wos_draw_all(&fb);
            if (outline) dbg_outline(&fb, rx0, ry0, rx1, ry1, outline);
        }
    }
}

int wos_run_frame(void)
{
    wos_dbg_raster_us=0;
    uint32_t now = hal_millis();
    uint32_t dt_ms = now - s_last_frame_t;
    s_last_frame_t = now;

    if (s_tx && !s_tx_drag) {                /* not finger-driven: settle prog to commit(1)/cancel(0) */
        if (s_tx_prog < s_tx_tgt)      { s_tx_prog += TX_STEP_PROG; if (s_tx_prog > s_tx_tgt) s_tx_prog = s_tx_tgt; }
        else if (s_tx_prog > s_tx_tgt) { s_tx_prog -= TX_STEP_PROG; if (s_tx_prog < s_tx_tgt) s_tx_prog = s_tx_tgt; }
        if (s_tx_prog == s_tx_tgt) {
            int to_b = s_tx_tgt >= 0.5f;
            if (to_b && s_tx_to_app != -2) tx_apply_to();   /* committed (not app-internal) */
            s_tx = 0;
            /* The panel shows the fade at s_tx_p_prev. Repaint the strip from there to the
             * end, and diff the live screen against the snapshot it lands on. */
            gfx_rec *snap = s_rec_snap[to_b];
            if (snap && !snap->overflow) {
                memcpy(s_rec_prev, snap, sizeof *snap);
                tx_band_rect(s_tx_p_prev, tx_pos(s_tx_tgt), s_tx_tail);
                s_tx_tail_ok = 1;
            } else
                s_force_full = 1;
        }
    }

    if (s_phase == PH_APP && !s_long_fired && now - s_t0 >= LONG_PRESS_MS) {
        int adx = s_lx - s_sx, ady = s_ly - s_sy;
        if (adx < 0) adx = -adx;
        if (ady < 0) ady = -ady;
        if (adx <= SLOP && ady <= SLOP) {
            s_long_fired = 1;
            app_dispatch(WOS_EV_LONG_PRESS, s_lx, s_ly);
        }
    }

    app_dispatch(WOS_EV_TICK, (int)dt_ms, 0);
    if (s_active >= 0)
        ov_dispatch(WOS_EV_TICK, (int)dt_ms, 0);

    if (s_settling) {
        if (!spring_step(&s_spring, dt_ms / 1000.0f)) {
            s_off = s_spring.pos;
            s_settling = 0;
            if (s_pending_close) {
                s_active = -1;
                s_off = 0;
            }
        } else {
            s_off = s_spring.pos;
        }
        s_offvel = s_spring.vel;
    }

    /* HUD string: computed once per frame, painted per band in wos_draw_all */
    if (wos_debug_hud) {
        float fps = dt_ms > 0 ? 1000.0f / dt_ms : 999.0f;
        s_hud_fps = s_hud_fps * 0.9f + fps * 0.1f;
        snprintf(s_hud_buf, sizeof s_hud_buf, "%d", (int)(s_hud_fps + 0.5f));
    }

    /* Latch the clock so the record pass and the raster pass see one consistent
     * time (an animation must sit at the same spot in both, or its damage box
     * wouldn't cover where it actually gets drawn). */
    wos_frame_id++;
    battery_update();          /* smooth + debounce the battery once per frame */
    hal_frame_begin();

    /* Pass 1 — record this frame's draw commands (bbox + hash), no pixels. */
    s_rec_cur->n = 0;
    s_rec_cur->overflow = 0;
    gfx_fb rfb;
    rfb.px = 0;
    rfb.ox = rfb.oy = 0;
    rfb.y_base = 0;
    rfb.h = GFX_H;
    rfb.cx0 = 0; rfb.cx1 = GFX_W;
    rfb.stride = rfb.x_base = 0;
    rfb.rec = s_rec_cur;
    s_ex_n = 0;                /* apps re-declare explicit dirty rects each frame */
    s_stream_hint = 0;
    s_flat_hint = 0;
    uint32_t record_start=hal_micros();
    wos_draw_all(&rfb);
    wos_dbg_record_us=hal_micros()-record_start;

    /* Decide what to repaint. Overlays animate over the whole screen, and the
     * first frame after boot / an app switch can't trust the panel -> full. */
    static int rects[WOS_MAXRECTS * 5][4];   /* room for the debug outline erasers; static: 2 KB */
    int nrects;
    /* A settled, stationary overlay (e.g. the control centre sitting open) is a
     * static screen — damage-track it like any face so it hits 60fps. Only a
     * MOVING overlay (sliding open/closed or dragged) repaints the whole screen.
     * The settle frame still runs one forced full (s_force_full carries over from
     * the last moving frame) to give damage a clean baseline. */
    int ov_moving = s_active >= 0 && (s_settling || s_off > 0.5f);
    if (ov_moving)
        s_force_full = 1;
    /* Preserve this frame's decision until explicit damage has been considered.
     * Clearing s_force_full early let an overlay's wos_dirty() rectangles replace
     * the required full repaint, leaving untouched panel pixels stale. */
    int force_full = s_force_full;
    if (force_full)
        nrects = -1;
    else if (s_tx) {                     /* transition: just the band the front swept */
        tx_band_rect(s_tx_p_prev, s_tx_p_cur, rects[0]);
        nrects = rects[0][2] > rects[0][0] && rects[0][3] > rects[0][1];
    } else {
        nrects = compute_damage(s_rec_cur, s_rec_prev, rects);
        if (s_tx_tail_ok && nrects >= 0) {       /* transition just ended: add its tail */
            int merged = 0;
            for (int i = 0; i < nrects && !merged; i++)
                if (rect_near(rects[i], s_tx_tail, WOS_MERGE_GAP)) { rect_union(rects[i], s_tx_tail); merged = 1; }
            if (!merged && s_tx_tail[2] > s_tx_tail[0] && s_tx_tail[3] > s_tx_tail[1]) {
                if (nrects < WOS_MAXRECTS) { for (int k = 0; k < 4; k++) rects[nrects][k] = s_tx_tail[k]; nrects++; }
                else nrects = -1;
            }
        }
    }
    s_tx_tail_ok = 0;
    if (s_tx) s_tx_p_prev = s_tx_p_cur;
    if (!ov_moving)
        s_force_full = 0;
    if (nrects < 0) {
        nrects = 1;
        rects[0][0] = 0; rects[0][1] = 0; rects[0][2] = GFX_W; rects[0][3] = GFX_H;
    }

    /* EXPLICIT dirty rects: if the app declared its own this frame, repaint
     * exactly those (verbatim, no auto-merge). Each declared rect covers one
     * element's old->new sweep, so it stays a tight box that self-clears the
     * vacated spot -> no tall chained band -> small fast flushes that stay ahead
     * of the scan (no tear) and fit the frame. */
    if (s_ex_n > 0 && !force_full && !ov_moving && s_active >= 0) {
        nrects = 0;
        for (int i = 0; i < s_ex_n && nrects < WOS_MAXRECTS; i++) {
            int x0 = s_ex[i][0], y0 = s_ex[i][1], x1 = s_ex[i][2], y1 = s_ex[i][3];
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > GFX_W) x1 = GFX_W;
            if (y1 > GFX_H) y1 = GFX_H;
            if (x1 <= x0 || y1 <= y0) continue;        /* fully off-screen */
            x0 &= ~1; x1 = (x1 + 1) & ~1; if (x1 > GFX_W) x1 = GFX_W;
            y0 &= ~1; y1 = (y1 + 1) & ~1; if (y1 > GFX_H) y1 = GFX_H;
            rects[nrects][0] = x0; rects[nrects][1] = y0;
            rects[nrects][2] = x1; rects[nrects][3] = y1; nrects++;
        }
        /* The debug HUD's number changes every frame, but explicit rects bypass
         * compute_damage — so add the HUD's box here or it never gets reflushed
         * (stale/frozen HUD over a scrolling list). Small box at top -> tear-free. */
        if (wos_debug_hud && nrects < WOS_MAXRECTS) {
            int hw = (int)GFX_S(38), hh = (int)GFX_S(20);
            int hx0 = (int)GFX_CX - hw / 2 - 2, hy0 = (int)GFX_S(32) - hh / 2 - 2;
            int hx1 = (int)GFX_CX + hw / 2 + 2, hy1 = (int)GFX_S(32) + hh / 2 + 2;
            if (hx0 < 0) hx0 = 0;
            if (hy0 < 0) hy0 = 0;
            if (hx1 > GFX_W) hx1 = GFX_W;
            if (hy1 > GFX_H) hy1 = GFX_H;
            hx0 &= ~1; hx1 = (hx1 + 1) & ~1;
            hy0 &= ~1; hy1 = (hy1 + 1) & ~1;
            rects[nrects][0] = hx0; rects[nrects][1] = hy0;
            rects[nrects][2] = hx1; rects[nrects][3] = hy1; nrects++;
        }
    }

    /* Debug redraw boxes: rects [0, npaint) get outlined; erasers for last frame's
     * outline edges are appended after them (skipped where a painted rect covers). */
    int npaint = nrects;
    if (wos_debug_rects || s_dbg_prev_n) {
        int cap = (int)(sizeof rects / sizeof rects[0]);
        for (int i = 0; i < s_dbg_prev_n; i++) {
            int x0 = s_dbg_prev[i][0], y0 = s_dbg_prev[i][1], x1 = s_dbg_prev[i][2], y1 = s_dbg_prev[i][3];
            int e[4][4] = { { x0, y0, x1, y0 + DBG_EDGE }, { x0, y1 - DBG_EDGE, x1, y1 },
                            { x0, y0, x0 + DBG_EDGE, y1 }, { x1 - DBG_EDGE, y0, x1, y1 } };
            for (int k = 0; k < 4 && nrects < cap; k++) {
                int covered = 0;
                for (int j = 0; j < npaint && !covered; j++)
                    covered = rects[j][0] <= e[k][0] && rects[j][1] <= e[k][1] &&
                              rects[j][2] >= e[k][2] && rects[j][3] >= e[k][3];
                if (covered || e[k][2] <= e[k][0] || e[k][3] <= e[k][1]) continue;
                rects[nrects][0] = e[k][0]; rects[nrects][1] = e[k][1];
                rects[nrects][2] = e[k][2]; rects[nrects][3] = e[k][3]; nrects++;
            }
        }
        s_dbg_prev_n = 0;
        if (wos_debug_rects)
            for (int i = 0; i < npaint && s_dbg_prev_n < 28; i++, s_dbg_prev_n++)
                for (int k = 0; k < 4; k++) s_dbg_prev[s_dbg_prev_n][k] = rects[i][k];
    }

    /* Export the frame's dirty rects for the sim debug overlay. */
    extern int wos_dbg_rect[16][4], wos_dbg_nr;
    wos_dbg_nr = nrects;
    for (int i = 0; i < nrects && i < 16; i++) {
        wos_dbg_rect[i][0] = rects[i][0]; wos_dbg_rect[i][1] = rects[i][1];
        wos_dbg_rect[i][2] = rects[i][2]; wos_dbg_rect[i][3] = rects[i][3];
    }

    wos_dbg_dirty_pixels=0;
    for(int i=0;i<nrects;i++) wos_dbg_dirty_pixels+=(rects[i][2]-rects[i][0])*(rects[i][3]-rects[i][1]);
    int dirty_rows = 0;
    for (int i = 0; i < nrects; i++)
        dirty_rows += rects[i][3] - rects[i][1];

    extern int wos_dbg_n, wos_dbg_bands, wos_dbg_rows;
    wos_dbg_n = s_rec_cur->n;
    wos_dbg_bands = nrects;
    wos_dbg_rows = dirty_rows;

    /* Big full-screen repaint (e.g. a scrolling overlay): compose the whole frame
     * OFF-SCREEN into a back-buffer, then submit one coherent image instead of
     * exposing progressive stripe composition. The installed board has no
     * confirmed TE connection, so this does not itself guarantee tear-free output. */
    /* A single TALL band (full-screen redraw, OR a big scrolling band like the
     * app-list rail) renders too slowly to stay ahead of the scan -> tears mid
     * screen. Compose it off-screen and present as one coherent image instead.
     * The band may be narrow (the list rail), so pass its bounds through. */
    int atomic = !s_stream_hint && nrects == 1 && (rects[0][3] - rects[0][1]) >= GFX_H / 2;
    uint16_t dbg_col = !wos_debug_rects ? 0 : atomic ? GFX_RED : GFX_GREEN;
    if (s_frame_begin && nrects>0)
        s_frame_begin(atomic, rects[0][0], rects[0][1], rects[0][2], rects[0][3]);

    /* If the platform provides a real synchronization callback, wait for it to
     * pace the flush — but ONLY when last frame's work comfortably fit a refresh
     * (~16.7ms). That way even a fast full-redraw (a scrolling overlay) can stay
     * aligned when synchronization is available, while a genuinely heavy frame
     * presents immediately instead of quantizing to 30fps. Uses the previous frame's render time
     * (this frame's isn't known yet, but successive frames are similar). */
    static uint32_t s_last_render_us = 0;
    /* Overlays (launcher/app-list/etc.) redraw the whole screen every frame, so
     * writing bands into the live panel GRAM without synchronization can tear.
     * For an overlay always invoke the configured sync callback first — trading a
     * possible drop to 30fps for more stable pacing. Non-overlay frames keep the
     * time-gated behaviour so a genuinely heavy face presents immediately. */
    if (!atomic && nrects > 0 && s_vsync && (s_active >= 0 || s_last_render_us < 14000u))
        s_vsync();

    /* Pass 2 — rasterize + flush only the dirty rectangles. */
    uint32_t _r0 = hal_micros();
    int slot = 0;
    if (wos_scan_order && s_stripe_h > 0)
        render_bands(rects, nrects, npaint, dbg_col, wos_scan_order == 1 ? -1 : 1, &slot);
    else
        for (int i = 0; i < nrects; i++)
            render_rect(rects[i][0], rects[i][1], rects[i][2], rects[i][3], &slot,
                        i < npaint ? dbg_col : 0);
    s_last_render_us = hal_micros() - _r0;

    uint32_t present_start=hal_micros();
    if (s_frame_end) s_frame_end(atomic);
    wos_dbg_present_us=hal_micros()-present_start;

    hal_frame_end();

    /* This frame's recording is next frame's baseline. */
    gfx_rec *tmp = s_rec_prev;
    s_rec_prev = s_rec_cur;
    s_rec_cur = tmp;

    return nrects > 0 ? 1 : 0;
}
