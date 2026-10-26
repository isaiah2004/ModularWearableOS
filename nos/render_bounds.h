#ifndef NOS_RENDER_BOUNDS_H
#define NOS_RENDER_BOUNDS_H
#include "gfx.h"
/* Conservative physical glass bounds for a stripe. Includes the AA fringe and
 * an extra pixel of guard, rounded OUT to the panel's even window alignment.
 * Corners outside this circle cannot be seen on the round 466px panel. */
static inline void nos_glass_span(int y0, int y1, int *x0, int *x1)
{
    float c = GFX_W * 0.5f, r = c + 1.0f;
    float a = y0 + 0.5f - c, b = y1 - 0.5f - c;
    float d = a > 0 ? a : b < 0 ? -b : 0;
    float square = r * r - d * d;
    float half = square > 0 ? gfx_fast_sqrt(square) + 1.0f : 1.0f;
    int lo = (int)(c - half), hi = (int)(c + half + 1.0f);
    lo &= ~1; hi = (hi + 1) & ~1;
    if (lo < 0) lo = 0;
    if (hi > GFX_W) hi = GFX_W;
    if (*x0 < lo) *x0 = lo;
    if (*x1 > hi) *x1 = hi;
}
#endif
