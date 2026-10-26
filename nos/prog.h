/* prog.h — progress indicators beyond dots: rounded squares, segments, slanted dashes,
 * stacking triangles, chevrons (>>>>) and dots, as a straight bar or around a ring.
 *
 * Every indicator is n elements; element i is lit by k = clamp(frac * n - i, 0, 1): whole
 * elements on, the leading one stepping in (its ink in 8 steps, a segment growing along
 * its length), the rest in `off` (0 = not drawn). Each element is one primitive, so the
 * damage recorder repaints only the elements that changed: a fill that advances one
 * element a frame costs one element a frame.
 *
 *   prog_bar(fb, PROG_CHEVRONS, x, y, w, h, 16, 0.4f, C_WHITE, C_LINE);
 *   prog_ring(fb, PROG_SEGMENTS, UI_CX, UI_CY, 200, 14, 60, f, C_WHITE, C_LINE);
 *
 * Rings start at 12 o'clock and run clockwise; their elements are laid along the circle
 * (a chevron points the way the ring fills). */
#ifndef NOS_PROG_H
#define NOS_PROG_H
#include "gfx.h"

enum { PROG_SQUARES = 0, PROG_SEGMENTS, PROG_SLANT, PROG_TRIANGLES, PROG_CHEVRONS, PROG_DOTS, PROG_NSTYLES };

const char *prog_name(int style);
/* Optional exact ring-layout acceleration. Two64-segment slots share one2088B
 * PSRAM allocation; tracked admission leaves512KiB for other UI owners. Cache
 * only immutable angle/bounds metadata, so translation, ink, fill and dim remain
 * live. Unsupported sizes or unavailable storage use the original renderer.
 * Select mode and release on the renderer thread, outside draw calls. Disabling
 * retains storage for A/B; release frees it and permits retry after failure. */
extern int prog_geometry_enabled; /* default1;0 uses the original uncached path */
unsigned prog_geometry_bytes(void);
void prog_geometry_release(void);
/* x, y: the bar's top-left; elements spread evenly over w, each h tall */
void prog_bar(gfx_fb *fb, int style, float x, float y, float w, float h, int n, float frac,
              uint16_t on, uint16_t off);
/* r: the ring's mid radius; t: its thickness (radial) */
void prog_ring(gfx_fb *fb, int style, float cx, float cy, float r, float t, int n, float frac,
               uint16_t on, uint16_t off);
/* A ring over part of the circle only: n elements from angle a0 to a1 (radians, 0 = 12 o'clock,
 * clockwise; a1 < a0 runs anticlockwise), filling from a0. */
void prog_arc(gfx_fb *fb, int style, float cx, float cy, float r, float t, float a0, float a1, int n,
              float frac, uint16_t on, uint16_t off);
/* An anti-aliased convex polygon (either winding), n <= 16 vertices as x, y pairs. */
void prog_poly(gfx_fb *fb, const float *xy, int n, uint16_t color);

/* Retained draw-list entry point: same pixels as element i of prog_arc. */
void prog_arc_segment(gfx_fb *fb, int style, float cx, float cy, float r, float t,
                      float a0, float a1, int n, float frac, uint16_t on, uint16_t off, int i);
#endif
