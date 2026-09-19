/* app_applist.c — curved focal app list (RIGHT edge overlay), Wear-OS style.
 * The focused (vertically centered) row is large; rows scale down toward the top
 * and bottom, and each row's left edge follows the round bezel so the column of
 * icons traces the screen's curve. Inertial scroll with focal snap.
 *
 * Motion design
 * -------------
 *  - ENTRANCE: on open the rows CASCADE in top-to-bottom — each fades up from
 *    black and flies in from the right edge (easeOutBack slide) with a per-row
 *    delay. Rows are stacked vertically, so the moving set is a single contiguous
 *    vertical band: the 2D damage engine repaints just that band, not the screen.
 *    Once every row settles the draw commands are identical frame-to-frame -> 0
 *    dirty -> effectively free.
 *  - MOTION BLUR: during a fast fling each row's icon leaves a short vertical
 *    comet trail (alpha-decayed ghost discs along the scroll velocity). A fling
 *    already full-redraws, so the trail costs no extra dirty area.
 *
 * The entrance clock, scroll and velocity are latched by wos_frame_id so the
 * record and raster passes emit identical geometry (see face_dense.c).
 * Tap a row to launch; swipe right (toward the edge) or tap empty to close. */
#include "apps.h"
#include "../font.h"
#include "../icons.h"
#include <math.h>

#define PI      3.14159265f
#define SHOW_SCROLLBAR 1   /* scroll bar on (tear-free now via the atomic present) */
/* Row pitch is kept comfortably larger than the focused icon so a >12px BLACK
 * GAP sits between every row. That gap does two jobs: (1) it exceeds the damage
 * engine's merge distance, so each row stays its OWN dirty band instead of
 * merging into one full-screen rect (which would trip the atomic present = 20fps
 * + a center tear); (2) any progressive-flush tear lands in that black gap, so
 * it's invisible. */
#define ROW_H   GFX_S(72)      /* vertical spacing between row anchors. Wide enough that only
                                * 3 items are on screen at rest (centre pill + 1 neighbour each
                                * side; the 2nd neighbours are culled off-screen) and at most 4
                                * while moving (one 2nd-neighbour peeks in). Fewer on-screen rows
                                * -> fewer per-frame dirty boxes -> more budget headroom. */
#define ICON_R  GFX_S(19)      /* focused icon radius at REST (scaled per row). While
                                * scrolling it collapses to ~0.6x + the pill body is
                                * hidden, so the moving row is a small icon-only circle
                                * that fits the tear/banding budget; it expands to this
                                * full size + label pill once the list settles. */
#define SCROLL_R   (GFX_CX - GFX_S(5))  /* scroll arc radius ~= display radius, so it
                                          * hugs the round rim and lines up with the edge */
#define SCROLL_MID (PI * 0.5f)           /* arc centre angle: 3 o'clock (right rim) */
#define RING_FADE_IN  260.0f   /* ms: focal ring eases in when the list settles */
#define RING_FADE_OUT 120.0f   /* ms: ...and snaps out fast the moment it moves */
/* Top speed limit: the RENDERED scroll can move at most this many px per frame,
 * so content never travels further than the black gap in one frame — any tear
 * stays within the gap (invisible) and each row's old/new band overlap enough to
 * merge into ONE band (half the dirty area). Input scroll is uncapped; the view
 * follows it speed-limited and catches up as a fling decelerates. */
/* The left ICON BAND: one vertical strip that repaints as a SINGLE rect while
 * scrolling (no per-row horizontal bands, no per-band command-dispatch tax). Its
 * width is inside the 60fps pixel budget (~190px x 466 ~ 88k px ~ half-screen).
 * Everything that moves (icons + scroll arc) lives inside it; the name lives
 * OUTSIDE it (with a >12px gap so it never merges in). */
#define LIST_BAND_W   GFX_S(98)    /* ~190 px */

/* Drag / inertia feel. Tuned for iOS-style rubber-banding and to shrug off a
 * touch driver that drops samples (one MOVE then reporting a huge jump). */
#define OVER_RESIST GFX_S(60)      /* drag effectiveness halves at this overshoot (px) */
#define OVER_MAX    GFX_S(85)      /* hard cap so the first/last row can never leave view */
#define MOVE_DMAX   (GFX_H * 0.5f) /* max believable finger delta in one sample (px) */
#define DT_FLOOR    8u             /* ms floor: a batched sample can't imply huge speed */
#define VEL_MAX     4000.0f        /* px/s cap on both fling and inertial speed */

/* Staggered cascade entrance (ms) + the fly-in slide distance (px). A short base
 * delay lets the panel land first, so the rows visibly cascade in afterward
 * (dark glass, then it populates) rather than under the slide. */
#define ENTER_BASE    70.0f
#define ENTER_STAGGER 52.0f
#define ENTER_DUR     330.0f
#define ENTER_FADE    210.0f
#define ENTER_SLIDE   GFX_S(112)
#define LIST_WIN      9

static float scroll;           /* px; input target (logical scroll) */
static float rscroll;          /* px; rendered scroll, tracks scroll 1:1 (smooth) */
static float prev_rscroll;     /* last frame's rscroll (moved-test + thumb old pos) */
static int   prev_frow;        /* last frame's focused row — a change repaints the centre
                                * pill (the only wide element), everything else is rails */
static float icon_flush;       /* rscroll when the icon boxes were last flushed. A pill-swap
                                * frame skips the icons, so their old->new sweep next frame
                                * must start from HERE, not prev_rscroll. */
static float vel;
static int last_y, dragging;
static uint32_t last_t;
static uint32_t open_ms;

/* on_draw records once, then may raster repeatedly into clipped framebuffer
 * stripes for the same frame. Keep the exact floating-point results from the
 * first call so later calls neither repeat layout nor risk tiny drift. */
typedef struct {
    int valid, icon;
    float cy, r, icx, dim;
    const char *name;
    uint16_t accent, fg;
} list_row_geom;

typedef struct {
    uint32_t frame;
    int base;
    float bodyr;
    list_row_geom row[LIST_WIN];
} list_frame_geom;

static list_frame_geom frame_geom = { .frame = 0xFFFFFFFFu };

static float max_scroll(void) { return (wos_app_count() - 1) * ROW_H; }
static int focused_row(void);   /* defined below; used by on_draw for the ring */

static void on_start(void)
{
    scroll = 0; rscroll = 0; prev_rscroll = 0; prev_frow = -1; icon_flush = 0; vel = 0; dragging = 0;
    open_ms = hal_millis();
    frame_geom.frame = 0xFFFFFFFFu;
}

/* Focal emphasis: a smooth bell centered on the focused row. 1.0 at the vertical
 * center, easing to ~0.5 at the rim so the focused item clearly dominates while
 * neighbours stay legible (Wear-OS feel). */
static float row_scale(float dy)
{
    float a = dy / (GFX_H * 0.5f);
    return 0.5f + 0.5f * gfx_fast_exp(-1.7f * a * a);
}

/* Multiply an RGB565 colour toward black by f (0..1) — the depth/fade dim. */
static uint16_t dim565(uint16_t c, float f)
{
    if (f <= 0.0f) return 0;
    if (f > 1.0f) f = 1.0f;
    int r = (int)(((c >> 11) & 0x1F) * f);
    int g = (int)(((c >>  5) & 0x3F) * f);
    int b = (int)(( c        & 0x1F) * f);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Depth cue: 1.0 for the focused row, easing to ~0.30 at the rim so the centre
 * dominates. Driven continuously from the focal scale sc (0.5..1.0). */
static float row_dim(float sc)
{
    float t = (sc - 0.5f) * 2.0f;      /* 0 at the rim .. 1 at the centre */
    if (t < 0) t = 0; else if (t > 1) t = 1;
    return 0.30f + 0.70f * t * t;
}

/* Overshooting ease (spring-like pop that peaks just past 1.0, then settles). */
static float ease_out_back(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    float x = t - 1.0f;
    return 1.0f + c3 * x * x * x + c1 * x * x;
}

#if SHOW_SCROLLBAR
/* draw a thin scroll-position arc hugging the right bezel */
static void scroll_indicator(gfx_fb *fb, float a_in)
{
    int n = wos_app_count();
    if (n < 2) return;
    float m = max_scroll();
    float frac = m > 0 ? rscroll / m : 0;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;

    /* Scroll track as an arc along the INNER (right) edge of the icon column,
     * concentric with the screen so it follows the same curve the icons trace —
     * sits just to their right, between the rail and the name. */
    float R = SCROLL_R;                   /* hugs the display rim */
    float half = 0.72f;                   /* half-span of the track (rad) */
    float a0 = SCROLL_MID - half;         /* top of the arc (right rim) */
    float a1 = SCROLL_MID + half;         /* bottom of the arc */
    gfx_arc(fb, GFX_CX, GFX_CY, R, GFX_S(2.5f), a0, a1, dim565(GFX_DGREY, a_in));

    /* thumb: a solid WHITE DOT riding the track, larger than the track thickness so it reads
     * as an intentional handle. Position = frac along the track. */
    float th = a0 + frac * (a1 - a0);
    float kx = GFX_CX + R * gfx_fast_sin(th);
    float ky = GFX_CY - R * gfx_fast_cos(th);
    gfx_fill_circle(fb, kx, ky, GFX_S(6.0f), dim565(GFX_WHITE, a_in));
}
#endif

/* Position warp: pull the IMMEDIATE neighbours in toward the centre pill for a snug gap. The
 * pull is a GAUSSIAN — strong at ~half a row, ~0 by the 2nd neighbour (which therefore stays
 * culled off screen -> 3 items at rest, 4 moving) AND ~0 for far items, so their per-frame
 * sweep stays linear/small (no whip -> boxes stay small -> budget stays low). MUST be applied
 * identically in the draw and dirty passes or the dirty box won't match the drawn icon. */
static float warp_cy(float cy)
{
    float d = cy - GFX_CY;
    float u = d / (float)ROW_H;
    return GFX_CY + d * (1.0f - 0.8f * gfx_fast_exp(-u * u));
}

static void on_draw(gfx_fb *fb)
{
    static uint32_t s_fid = 0xFFFFFFFFu;
    static float s_elapsed, s_last_ms;
    if (wos_frame_id != s_fid) {
        s_fid = wos_frame_id;
        uint32_t now = hal_millis();
        s_elapsed = (float)(now - open_ms);
        float dt = s_last_ms > 0.0f ? ((float)now - s_last_ms) : 16.0f;
        if (dt > 100.0f) dt = 100.0f;
        s_last_ms = (float)now;
        /* No speed cap: only the narrow left ICON RAIL scrolls (the wide name is
         * decoupled), so the dirty band is small enough for 60fps at any speed —
         * the view can track the input 1:1 and stay responsive. */
        prev_rscroll = rscroll;    /* last frame's rendered position */
        rscroll = scroll;          /* render tracks input 1:1 -> smooth. Tear-freedom now
                                    * comes from the FIXED centre pill + narrow split rails
                                    * (see the draw + dirty below), not from capping scroll. */
    }

    gfx_clear(fb, GFX_BLACK);
    /* Coalesce the left band into ONE dirty rect while scrolling: a THIN,
     * full-height black line down the middle of the band. It overlaps every icon
     * in x and spans all y, so all their per-row damage merges into this single
     * vertical band (no 5-6 horizontal bands). It's a 2px line (not a full rect)
     * so it doesn't double-clear the band. Its recorded y toggles with the scroll
     * -> dirty exactly when moving; settled y is stable -> 0-dirty idle. */
    int n = wos_app_count();
    float R = GFX_W * 0.5f;               /* full bezel radius */
    int frow = (int)(rscroll / ROW_H + 0.5f);   /* visually-centered row (ring) */
    if (frow < 0) frow = 0;
    if (frow >= n) frow = n - 1;

    /* Consider a window of WIN rows around focus and draw only the ones actually
     * on screen (off-screen slots are culled, not drawn). The window is wide
     * enough (frow +-3) that every on-screen row — plus one row of margin — is
     * always inside it, so an icon keeps being redrawn until it is FULLY off
     * screen (no half-erased ghosts as a row exits). Constant command count is no
     * longer required: while scrolling we drive the repaint with explicit dirty
     * rects (wos_dirty), so compute_damage — the thing that needed a stable count
     * — is bypassed. Culling keeps the drawn/declared set to just the visible
     * rows, so widening the window costs nothing per frame. */
    /* Half-height of the centre band the fixed focus pill owns. At exactly ROW_H/2 an
     * incoming icon reaches the band edge precisely as it becomes the focus, so the
     * hand-off from sliding rail icon to centred pill has no gap. */
    float pill_r = ICON_R * 1.16f;      /* focus pill radius */
    /* The frozen centre band is EXACTLY the pill's height (not ROW_H/2). Any taller and
     * the strip between the pill edge and the band edge is neither redrawn by the rails
     * (clipped at the band) nor covered by the pill -> stale icon slivers freeze there
     * (that was the margin artifact). Band == pill => no uncovered strip. */
    float halfband = pill_r;
    int base = frow - LIST_WIN / 2;

    /* Build once per presented frame, then reuse verbatim in recorder and raster
     * passes. This is intentionally a tiny geometry cache, not a pixel cache. */
    if (frame_geom.frame != wos_frame_id) {
        frame_geom.frame = wos_frame_id;
        frame_geom.base = base;
        for (int s = 0; s < LIST_WIN; s++) {
            list_row_geom *g = &frame_geom.row[s];
            int i = base + s;
            g->valid = 0;
            if (i < 0 || i >= n) continue;
            float cy = warp_cy(GFX_CY + i * ROW_H - rscroll);
            if (cy + ICON_R < 0 || cy - ICON_R > GFX_H) continue;
            float dy = cy - GFX_CY;
            float cy0 = GFX_CY + i * ROW_H;
            int initial = (cy0 - ICON_R < GFX_H);
            float alpha, slide;
            if (!initial) {
                alpha = 1.0f;
                slide = 0.0f;
            } else {
                float u = s_elapsed - ENTER_BASE - i * ENTER_STAGGER;
                if (u <= 0.0f) {
                    alpha = 0.0f;
                    slide = ENTER_SLIDE;
                } else {
                    float t = u / ENTER_DUR;
                    if (t > 1.0f) t = 1.0f;
                    slide = ENTER_SLIDE * (1.0f - ease_out_back(t));
                    alpha = u / ENTER_FADE;
                    if (alpha > 1.0f) alpha = 1.0f;
                }
            }
            float sc = row_scale(dy);
            float r = ICON_R * (0.72f + 0.42f * sc);
            float inside = R * R - dy * dy;
            float bez = inside > 0 ? GFX_CX - gfx_fast_sqrt(inside) : GFX_CX;
            float indent = bez * 0.9f;
            if (indent > GFX_S(24)) indent = GFX_S(24);
            g->valid = 1;
            g->cy = cy;
            g->r = r;
            g->icx = GFX_S(17) + indent + r + slide;
            g->dim = row_dim(sc) * alpha;
            g->name = wos_app_name(i);
            g->accent = wos_app_accent(i);
            g->icon = wos_app_icon(i);
            g->fg = dim565(GFX_WHITE, g->dim);
        }
        const char *focus_name = wos_app_name(frow);
        float focus_icx0 = GFX_S(17) + pill_r;
        frame_geom.bodyr = focus_icx0 + pill_r + GFX_S(7)
                         + (float)font_text_width(&font_medium, focus_name) + GFX_S(9);
    }

    /* ---- ICONS: every visible row EXCEPT the focused one, sliding smoothly along the
     * arc. They pass BEHIND the centre pill (drawn on top, below): each slides up to the
     * band edge and tucks UNDER the pill, completing its travel instead of stopping short.
     * Narrow per-icon dirty boxes (see the dirty pass) -> tear-free. ---- */
    for (int s = 0; s < LIST_WIN; s++) {
        const list_row_geom *g = &frame_geom.row[s];
        if (!g->valid) continue;
        const char *nm = g->name;
        gfx_fill_circle(fb, g->icx, g->cy, g->r, dim565(g->accent, g->dim));
        int icon = g->icon;
        if (icon != ICON_NONE) {
            icon_glyph(fb, icon, g->icx, g->cy, g->r * 0.62f, g->fg);
        } else {
            char init[2];
            init[0] = nm && nm[0] ? nm[0] : '?';
            init[1] = 0;
            if (init[0] >= 'a' && init[0] <= 'z') init[0] -= 32;
            font_draw_centered(fb, &font_small, (int)g->icx,
                               (int)(g->cy + font_small.ascent * 0.34f), init, g->fg);
        }
    }

    /* ---- CENTRE PILL, drawn ON TOP so the icons pass UNDER it: colour body + focused
     * icon + label, FROZEN at the vertical centre. A FLAT-edged rectangle that fills the
     * whole band height and runs off the left edge (x<0), so an icon tucking under it meets
     * a clean straight edge — no rounded-corner peek, no gap, no early stop. No triangle.
     * The dirty pass repaints this band only on a focus change. ---- */
    float icx0 = GFX_S(17) + pill_r;                     /* fixed icon anchor (unchanged) */
    float pill_right;
    {
        const char *nm = wos_app_name(frow);
        uint16_t body = wos_app_accent(frow);
        float bodyr = frame_geom.bodyr;   /* right-cap centre */
        gfx_fill_rect(fb, 0, (int)(GFX_CY - pill_r), (int)bodyr, (int)(2.0f * pill_r), body);
        gfx_fill_circle(fb, bodyr, GFX_CY, pill_r, body);
        int icon = wos_app_icon(frow);
        if (icon != ICON_NONE) {
            icon_glyph(fb, icon, icx0, GFX_CY, pill_r * 0.62f, GFX_WHITE);
        } else {
            char init[2];
            init[0] = nm && nm[0] ? nm[0] : '?';
            init[1] = 0;
            if (init[0] >= 'a' && init[0] <= 'z') init[0] -= 32;
            font_draw_centered(fb, &font_small, (int)icx0,
                               (int)(GFX_CY + font_small.ascent * 0.34f), init, GFX_WHITE);
        }
        font_draw(fb, &font_medium, (int)(icx0 + pill_r + GFX_S(7)),
                  (int)(GFX_CY + font_medium.ascent * 0.30f), nm, GFX_WHITE);
        pill_right = bodyr + pill_r;
    }

    /* ---- DIRTY (record pass only) ----
     * Two independent kinds of damage, kept on SEPARATE frames so neither frame goes over
     * budget:
     *   - ICON boxes: one narrow old->new sweep box per sliding icon, cropped at both band
     *     lines (the frozen pill owns the middle). Cheap, tear-free.
     *   - PILL-SWAP band: on a focus change the label changes -> a wider (one-row) repaint.
     *     It gets its OWN frame (icons frozen that frame) so the wide band never shares a
     *     frame with the icon boxes. Icons resume next frame, sweeping from `icon_flush`
     *     (their last actually-flushed position, NOT prev_rscroll). A safety net still
     *     refreshes icons on a swap frame if they've fallen >half a row behind (fast scroll),
     *     so they never freeze outright. */
    if (fb->rec) {
        int focus_changed = (frow != prev_frow);
        int entering = (s_elapsed < ENTER_BASE + LIST_WIN * ENTER_STAGGER + ENTER_DUR + 80.0f);
        int moved = (rscroll != prev_rscroll);
        int bandtop = (int)(GFX_CY - halfband);
        int bandbot = (int)(GFX_CY + halfband);
        if (entering) {
            wos_dirty(0, 0, GFX_W, GFX_H);          /* one-time open: full clean redraw */
            icon_flush = rscroll;
        } else {
            int do_band  = focus_changed;
            int do_icons = (moved || dragging) && !focus_changed;
            if (focus_changed) {                    /* safety: don't let icons freeze at speed */
                float behind = rscroll - icon_flush;
                if (behind < 0) behind = -behind;
                if (behind > ROW_H * 0.5f) do_icons = 1;
            }
            if (do_icons) {
                float m = GFX_S(5);      /* margin: catch edges, keep box < 125px wide */
                for (int i = frow - LIST_WIN / 2; i <= frow + LIST_WIN / 2; i++) {
                    if (i < 0 || i >= n) continue;
                    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
                    int on = 0;
                    for (int p = 0; p < 2; p++) {   /* new + flushed(old) positions */
                        float cy, r, icx;
                        if (p == 0) {
                            const list_row_geom *g = &frame_geom.row[i - frame_geom.base];
                            if (!g->valid) continue;
                            cy = g->cy;
                            r = g->r;
                            icx = g->icx; /* entrance is over whenever this path runs */
                        } else {
                            float cy_old = warp_cy(GFX_CY + i * ROW_H - icon_flush);
                            if (cy_old + ICON_R < 0 || cy_old - ICON_R > GFX_H) continue;
                            float dy = cy_old - GFX_CY;
                            float sc = row_scale(dy);
                            r = ICON_R * (0.72f + 0.42f * sc);
                            float inside = R * R - dy * dy;
                            float bez = inside > 0 ? GFX_CX - gfx_fast_sqrt(inside) : GFX_CX;
                            float indent = bez * 0.9f;
                            if (indent > GFX_S(24)) indent = GFX_S(24);
                            cy = cy_old;
                            icx = GFX_S(17) + indent + r;
                        }
                        on = 1;
                        if (icx - r - m < minx) minx = icx - r - m;
                        if (icx + r + m > maxx) maxx = icx + r + m;
                        if (cy  - r - m < miny) miny = cy  - r - m;
                        if (cy  + r + m > maxy) maxy = cy  + r + m;
                    }
                    if (on && maxx > minx) {
                        /* crop at BOTH band lines -> the icon keeps cropping continuously as
                         * it crosses centre (no skip at frow); the frozen pill owns the middle. */
                        if (miny < bandtop) {
                            int by1 = maxy < bandtop ? (int)maxy : bandtop;
                            if (by1 > (int)miny)
                                wos_dirty((int)minx, (int)miny, (int)maxx, by1);
                        }
                        if (maxy > bandbot) {
                            int by0 = miny > bandbot ? (int)miny : bandbot;
                            if ((int)maxy > by0)
                                wos_dirty((int)minx, by0, (int)maxx, (int)maxy);
                        }
                    }
                }
                icon_flush = rscroll;
            }
            if (do_band) {
                /* pill swap on its own frame: redraw ONLY the label band, as wide as the pill
                 * actually is (new label, or the OLD one if wider so it's fully cleared). */
                float br = pill_right;
                if (prev_frow >= 0 && prev_frow < n) {
                    float ob = icx0 + pill_r + GFX_S(7)
                             + (float)font_text_width(&font_medium, wos_app_name(prev_frow))
                             + GFX_S(9) + pill_r;
                    if (ob > br) br = ob;
                }
                wos_dirty(0, bandtop, (int)(br + GFX_S(4)), bandbot);
            }
#if SHOW_SCROLLBAR
            if (moved || dragging || focus_changed) {
                /* knob: dirty just its OLD+NEW circle; the bounding box covers the swept path,
                 * so the old knob is erased -> no trail. */
                float mm = max_scroll();
                float a0 = SCROLL_MID - 0.72f, a1 = SCROLL_MID + 0.72f;
                float kr = GFX_S(6.0f) + GFX_S(2);         /* knob radius + AA */
                float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
                for (int p = 0; p < 2; p++) {
                    float sv = p ? prev_rscroll : rscroll;
                    float frac = mm > 0 ? sv / mm : 0;
                    if (frac < 0) frac = 0;
                    if (frac > 1) frac = 1;
                    float th = a0 + frac * (a1 - a0);
                    float kx = GFX_CX + SCROLL_R * gfx_fast_sin(th);
                    float ky = GFX_CY - SCROLL_R * gfx_fast_cos(th);
                    if (kx - kr < minx) minx = kx - kr;
                    if (kx + kr > maxx) maxx = kx + kr;
                    if (ky - kr < miny) miny = ky - kr;
                    if (ky + kr > maxy) maxy = ky + kr;
                }
                wos_dirty((int)minx, (int)miny, (int)maxx, (int)maxy);
            }
#endif
        }
        prev_frow = frow;
    }

    /* whole-list fade for the scroll track (constant 2 commands) */
#if SHOW_SCROLLBAR
    float ind_a = s_elapsed / 300.0f;
    if (ind_a > 1.0f) ind_a = 1.0f;
    scroll_indicator(fb, ind_a);
#endif
}

static int focused_row(void)
{
    int i = (int)(scroll / ROW_H + 0.5f);
    if (i < 0) i = 0;
    if (i >= wos_app_count()) i = wos_app_count() - 1;
    return i;
}

static int row_at(int y)
{
    /* nearest row anchor to the tapped y */
    float best = 1e9f; int hit = -1;
    int n = wos_app_count();
    for (int i = 0; i < n; i++) {
        float cy = GFX_CY + i * ROW_H - rscroll;
        float d = fabsf(cy - y);
        if (d < best && d < ROW_H * 0.5f) { best = d; hit = i; }
    }
    return hit;
}

static int on_event(const wos_event *ev)
{
    switch (ev->type) {
    case WOS_EV_TOUCH_DOWN:
        last_y = ev->y; dragging = 1; vel = 0; last_t = hal_millis();
        return 1;
    case WOS_EV_TOUCH_MOVE:
        if (dragging) {
            uint32_t now = hal_millis();
            uint32_t dt = now - last_t;
            if (dt < DT_FLOOR) dt = DT_FLOOR;   /* floor guards against batched samples */
            float d = last_y - ev->y;
            /* clamp one sample so a dropped touch report can't teleport the list */
            if (d >  MOVE_DMAX) d =  MOVE_DMAX;
            else if (d < -MOVE_DMAX) d = -MOVE_DMAX;
            /* Exponential rubber-band + hard stop (see original tuning notes). */
            float m = max_scroll();
            float over = scroll < 0 ? -scroll : scroll > m ? scroll - m : 0;
            if (over > 0) d /= 1.0f + over / OVER_RESIST;
            scroll += d;
            if (scroll < -OVER_MAX) scroll = -OVER_MAX;
            else if (scroll > m + OVER_MAX) scroll = m + OVER_MAX;
            float iv = d * 1000.0f / dt;
            vel = 0.7f * iv + 0.3f * vel;
            if (vel >  VEL_MAX) vel =  VEL_MAX;
            else if (vel < -VEL_MAX) vel = -VEL_MAX;
            last_y = ev->y; last_t = now;
        }
        return 1;
    case WOS_EV_TOUCH_UP:
        dragging = 0;   /* momentum (WOS_EV_TICK) coasts and focal-snaps to the nearest row */
        return 1;
    case WOS_EV_TAP: {
        int i = row_at(ev->y);
        if (i >= 0) { wos_open_app(i); return 1; }
        return 0;
    }
    case WOS_EV_TICK: {
        if (!dragging) {
            float dt = ev->x / 1000.0f;
            if (vel >  VEL_MAX) vel =  VEL_MAX;   /* cap total inertial speed */
            else if (vel < -VEL_MAX) vel = -VEL_MAX;
            scroll += vel * dt;
            vel *= 0.88f;
            float m = max_scroll();
            if (scroll < 0)      { scroll += (0 - scroll) * 0.30f; vel = 0; }
            else if (scroll > m) { scroll += (m - scroll) * 0.30f; vel = 0; }
            else if (fabsf(vel) < 40.0f) {
                /* settle onto the nearest row (focal snap) */
                float target = focused_row() * ROW_H;
                scroll += (target - scroll) * 0.22f;
                vel = 0;
            }
        }
        return 1;
    }
    default:
        return 0;
    }
}

const wos_app app_applist = { "applist", on_start, on_event, on_draw, GFX_GREY, ICON_NONE };
