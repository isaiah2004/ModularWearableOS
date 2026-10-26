#include "cost_profile.h"
#include "gfx.h"
#include "gfx_cost.h"
#ifdef GFX_COST_MODEL
#define COST_EDGE_ACCUM(p,n) do { unsigned *edge_count=(p); if(edge_count) *edge_count+=(unsigned)(n); } while(0)
#else
#define COST_EDGE_ACCUM(p,n) ((void)(p))
#endif
#include <stdint.h>
#include <math.h>

/* ---- damage recording (retained-mode diff; see gfx.h) ---- */
unsigned gfx_hash_u(unsigned h, unsigned v) { return (h ^ v) * 16777619u; }
unsigned gfx_hash_f(unsigned h, float v)
{
    union { float f; unsigned u; } u;
    u.f = v;
    return gfx_hash_u(h, u.u);
}
int gfx_rec_cmd(gfx_fb *fb, int x0, int y0, int x1, int y1, unsigned hash)
{
    gfx_rec *r = fb->rec;
    if (!r) return 0;
    /* record in screen space: content drawn translated (pagers, slides) damages
     * where it lands, not where its logical coordinates say */
    x0 += fb->ox; x1 += fb->ox;
    y0 += fb->oy; y1 += fb->oy;
    /* Ink scaling changes pixels even when a primitive's nominal colour and
     * position stay fixed. Retained layers and damage need that dependency. */
    if (fb->dim) hash = gfx_hash_u(hash, 0xd1000000u | (unsigned)fb->dim);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > GFX_W) x1 = GFX_W;
    if (y1 > GFX_H) y1 = GFX_H;
    if (r->n < GFX_MAX_CMDS) {
        gfx_cmd *c = &r->c[r->n];
        c->x0 = x0; c->y0 = y0; c->x1 = x1; c->y1 = y1; c->hash = hash;
    } else {
        r->overflow = 1;   /* too many commands -> OS falls back to full redraw */
    }
    r->n++;
    return 1;
}

/* Fast sin/cos — libm sinf/cosf are software (no HW FPU transcendentals on the
 * Xtensa) and cost ~1000s of cycles; the arc rasterizer calls them ~9x per arc.
 * A 7th-order minimax polynomial after range-reduction is ~1e-6 accurate (well
 * under a pixel at any dial radius) and ~100x cheaper. */
static float fast_sin(float x)
{
    x -= 6.2831853f * floorf(x * 0.15915494f + 0.5f);   /* reduce to [-pi, pi] */
    /* fold into [-pi/2, pi/2] where the polynomial is accurate: unfolded it was off
     * by 0.038 at +-pi (rings drew 8 px out of place at 3 and 6 o'clock) */
    if (x > 1.5707963f) x = 3.1415927f - x;
    else if (x < -1.5707963f) x = -3.1415927f - x;
    float x2 = x * x;
    return x * (0.9999966f + x2 * (-0.16665682f + x2 * (0.00830632f - x2 * 0.00018363f)));
}
static float fast_cos(float x) { return fast_sin(x + 1.5707963f); }

/* Fast sqrt — sqrtf is soft on this build (~1000 cycles) and the scanline
 * rasterizers call it per row. Quake rsqrt seed + 2 Newton steps (mults only,
 * no divide) is ~1e-5 accurate (far under a pixel) and ~50x cheaper. */
static float fast_sqrt(float x)
{
    if (x <= 0.0f) return 0.0f;
    union { float f; unsigned i; } u = { x };
    u.i = 0x5f3759df - (u.i >> 1);
    float r = u.f;
    r *= 1.5f - 0.5f * x * r * r;
    r *= 1.5f - 0.5f * x * r * r;
    return x * r;
}

/* floorf/ceilf are also soft here; the scanline setup calls them ~4x per disc.
 * These handle the rasterizer's on-screen range fine. */
static int ifloor(float x) { int i = (int)x; return i - (x < (float)i); }
static int iceil(float x)  { int i = (int)x; return i + (x > (float)i); }

/* Clamp a logical row range [*py0,*py1] to the rows this buffer/stripe holds.
 * Returns 0 when nothing is left: every stripe of every dirty rect re-runs the
 * whole draw list, so a primitive outside it must cost a test, not a row walk. */
static int clip_rows(const gfx_fb *fb, int *py0, int *py1)
{
    int lo = fb->y_base - fb->oy, hi = lo + fb->h - 1;
    if (*py0 < lo) *py0 = lo;
    if (*py1 > hi) *py1 = hi;
    return *py0 <= *py1;
}

/* Fast exp — expf is soft too. e^x = 2^(x/ln2) via the classic float bit-hack
 * (Schraudolph): stuff the exponent field directly. ~2% accurate, plenty for
 * the cosmetic Gaussian falloffs (icon fisheye, list focal bell) that call it.
 * Guarded so a big-negative argument saturates to 0 instead of denormal noise. */
static float fast_exp(float x)
{
    if (x < -12.0f) return 0.0f;
    if (x >  12.0f) x = 12.0f;
    union { float f; int i; } u;
    u.i = (int)(12102203.16f * x) + 1064866805;
    return u.f;
}

/* Public wrappers so apps can reach these on the hot path without pulling in
 * soft-float libm. Thin by design — the compiler inlines the static bodies. */
float gfx_fast_sin(float x)  { return fast_sin(x); }
float gfx_fast_cos(float x)  { return fast_cos(x); }
float gfx_fast_sqrt(float x) { return fast_sqrt(x); }
float gfx_fast_exp(float x)  { return fast_exp(x); }

/* ---- pixel-level ---- */

uint16_t gfx_blend565(uint16_t dst, uint16_t src, uint8_t alpha)
{
    /* Split RGB565 into G | RB lanes of one 32-bit word so a single
     * multiply blends all three channels. Alpha scaled to 0..32. */
    uint32_t a = ((uint32_t)alpha + 7) >> 3;
    if (a >= 32) return src;
    if (a == 0) return dst;
    uint32_t d = (dst | ((uint32_t)dst << 16)) & 0x07E0F81F;
    uint32_t s = (src | ((uint32_t)src << 16)) & 0x07E0F81F;
    /* round each lane (+16 before >> 5): white or a neutral grey over black then lands
     * on a neutral grey (truncating tinted AA edges green; dropping green's low bit
     * tinted bright ones pink) */
    uint32_t r = ((s * a + d * (32 - a) + 0x02008010u) >> 5) & 0x07E0F81F;
    return (uint16_t)(r | (r >> 16));
}

void gfx_px_blend(gfx_fb *fb, int x, int y, uint16_t color, uint8_t alpha)
{
    if (fb->rec) {
        gfx_rec_cmd(fb, x, y, x + 1, y + 1,
                    gfx_hash_f(gfx_hash_u(7u, color), (float)alpha));
        return;
    }
    color = gfx_ink(fb, color);
    x += fb->ox;
    y += fb->oy - fb->y_base;
    if ((unsigned)x >= GFX_W || (unsigned)y >= (unsigned)fb->h || alpha == 0) {
        GFX_COST(GFX_COST_BLEND, 0);
        return;
    }
    /* 2D dirty-rect x-window: a single pixel outside the clipped columns is
     * skipped (sentinel cx1<=cx0 means full width). */
    { int c0 = fb->cx0, c1 = fb->cx1;
      if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
      if (c0 < 0) c0 = 0;
      if (c1 > GFX_W) c1 = GFX_W;
      if (x < c0 || x >= c1) { GFX_COST(GFX_COST_BLEND, 0); return; } }
    uint16_t *p = &gfx_row(fb, y)[x];
    *p = (alpha >= 255) ? color : gfx_blend565(*p, color, alpha);
    GFX_COST(GFX_COST_BLEND, 1);
}

/* Solid span fill with word stores: the stripe buffers are 4-byte aligned and
 * dirty rects are even-aligned, so nearly every span is a run of 32-bit writes. */
static void fill_span(uint16_t *p, int n, uint16_t color)
{
    if (n <= 0) return;
    if ((uintptr_t)p & 2) { *p++ = color; n--; }
    uint32_t two = ((uint32_t)color << 16) | color;
    uint32_t *q = (uint32_t *)p;
    for (int i = 0; i < n / 2; i++) q[i] = two;
    if (n & 1) p[n - 1] = color;
}

void gfx_clear(gfx_fb *fb, uint16_t color)
{
    NOS_COST_SCOPE(NOS_COST_CLEAR,!fb->rec);
    if (fb->rec) { gfx_rec_cmd(fb, 0, 0, GFX_W, GFX_H, gfx_hash_u(2u, color)); return; }
    /* Fast path when there's no content slide: the whole buffer is this app's
     * window, so fill it directly. Works for a full frame AND a render stripe
     * (y_base shifts which logical rows the buffer holds, but it's still fully
     * this app's). A mid-slide clear (oy/ox != 0) must not wipe the neighbor. */
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }        /* sentinel: no x-clip */
    if (c0 < 0) c0 = 0;
    if (c1 > GFX_W) c1 = GFX_W;
#ifdef GFX_COST_MODEL
    gfx_cost_target(fb->cost_psram);
#endif
    if (fb->ox == 0 && fb->oy == 0 && c0 == 0 && c1 == GFX_W && !fb->stride && !fb->x_base) {
        color = gfx_ink(fb, color);
        uint32_t two = ((uint32_t)color << 16) | color;
        uint32_t *p = (uint32_t *)fb->px;
        for (int i = 0; i < GFX_W * fb->h / 2; i++)
            p[i] = two;
        GFX_COST(GFX_COST_FILL, GFX_W * fb->h);
    } else if (fb->ox == 0 && fb->oy == 0) {
        /* 2D dirty-rect render: clear only the dirty columns of each row */
        color = gfx_ink(fb, color);
        for (int y = 0; y < fb->h; y++)
            fill_span(&gfx_row(fb, y)[c0], c1 - c0, color);
        GFX_COST(GFX_COST_FILL, c1 > c0 ? (c1 - c0) * fb->h : 0);
    } else {
        /* translated (mid page-slide): clear only this app's window so a
         * neighbor drawn earlier isn't wiped */
        gfx_fill_rect(fb, 0, 0, GFX_W, GFX_H, color);
    }
}

void gfx_fill_rect(gfx_fb *fb, int x, int y, int w, int h, uint16_t color)
{
    NOS_COST_SCOPE(NOS_COST_SHAPE,!fb->rec);
    if (fb->rec) {
        unsigned hh = gfx_hash_u(gfx_hash_u(gfx_hash_u(gfx_hash_u(1u, color),
                      (unsigned)x), (unsigned)y), (unsigned)(w * 65536 + h));
        gfx_rec_cmd(fb, x, y, x + w, y + h, hh);
        return;
    }
    color = gfx_ink(fb, color);
    x += fb->ox;
    y += fb->oy - fb->y_base;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > GFX_W ? GFX_W : x + w;
    int y1 = y + h > fb->h ? fb->h : y + h;
    /* 2D dirty-rect x-window: clamp the written span to the clipped columns
     * (sentinel cx1<=cx0 means full width). */
    { int c0 = fb->cx0, c1 = fb->cx1;
      if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
      if (c0 < 0) c0 = 0;
      if (c1 > GFX_W) c1 = GFX_W;
      if (x0 < c0) x0 = c0;
      if (x1 > c1) x1 = c1; }
    for (int yy = y0; yy < y1; yy++)
        fill_span(&gfx_row(fb, yy)[x0], x1 - x0, color);
    GFX_COST(GFX_COST_FILL, x1 > x0 && y1 > y0 ? (x1 - x0) * (y1 - y0) : 0);
}

/* ---- SDF helpers: coverage = how far inside the shape edge a pixel
 * center sits, clamped to one pixel of falloff -> smooth AA edge. ---- */

static uint8_t coverage(float signed_dist_inside)
{
    float c = signed_dist_inside + 0.5f;
    if (c <= 0.0f) return 0;
    if (c >= 1.0f) return 255;
    return (uint8_t)(c * 255.0f);
}

/* Iterate a clipped bounding box, calling the SDF for each pixel. */
#define BBOX_LOOP(fb, fx0, fy0, fx1, fy1, ...)                           \
    do {                                                                 \
        int _x0 = (int)floorf(fx0) + (fb)->ox, _y0 = (int)floorf(fy0) + (fb)->oy - (fb)->y_base; \
        int _x1 = (int)ceilf(fx1) + (fb)->ox, _y1 = (int)ceilf(fy1) + (fb)->oy - (fb)->y_base;   \
        if (_x0 < 0) _x0 = 0;                                            \
        if (_y0 < 0) _y0 = 0;                                            \
        if (_x1 > GFX_W - 1) _x1 = GFX_W - 1;                            \
        if (_y1 > (fb)->h - 1) _y1 = (fb)->h - 1;                        \
        /* 2D dirty-rect x-window (buffer cols; cx1<=cx0 => full width) */ \
        int _c0 = (fb)->cx0, _c1 = (fb)->cx1;                           \
        if (_c1 <= _c0) { _c0 = 0; _c1 = GFX_W; }                        \
        if (_c0 < 0) _c0 = 0;                                            \
        if (_c1 > GFX_W) _c1 = GFX_W;                                    \
        if (_x0 < _c0) _x0 = _c0;                                        \
        if (_x1 > _c1 - 1) _x1 = _c1 - 1;                                \
        for (int py = _y0; _x0 <= _x1 && py <= _y1; py++) {              \
            float fy = (float)(py - (fb)->oy + (fb)->y_base) + 0.5f;     \
            uint16_t *_row = gfx_row((fb), py);                          \
            for (int px_ = _x0; px_ <= _x1; px_++) {                     \
                float fx = (float)(px_ - (fb)->ox) + 0.5f;               \
                uint8_t _a;                                              \
                __VA_ARGS__;                                             \
                if (_a == 255)                                           \
                    _row[px_] = color;                                   \
                else if (_a)                                             \
                    _row[px_] = gfx_blend565(_row[px_], color, _a);      \
            }                                                            \
        }                                                                \
    } while (0)

/* Filled disc, scanline. sqrtf is software on the Xtensa (no HW sqrt), so the
 * solid interior is filled with NO per-pixel sqrt — only the ~1px AA rim pays
 * for it. That turns O(r^2) sqrts into O(r). Factored out so gfx_taper can
 * sweep discs without each one becoming its own recorded damage command. */
static unsigned fill_circle_raster(gfx_fb *fb, float cx, float cy, float r, uint16_t color, unsigned *blends)
{
    unsigned n = 0;   /* pixels written (cost model only) */
    float r_out = r + 1.0f, ro2 = r_out * r_out;
    float ri = r - 1.0f, ri2 = ri > 0 ? ri * ri : 0.0f;
    /* 2D dirty-rect x-window (buffer columns, sentinel cx1<=cx0 => full width);
     * px is logical so the clamp is expressed in logical x as [c0-ox, c1-1-ox]. */
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (c0 < 0) c0 = 0;
    if (c1 > GFX_W) c1 = GFX_W;
    int pxlo = c0 - fb->ox, pxhi = c1 - 1 - fb->ox;
    if (iceil(cx + r_out) < pxlo || ifloor(cx - r_out) > pxhi) return 0;
    int py0 = ifloor(cy - r_out), py1 = iceil(cy + r_out);
    if (!clip_rows(fb, &py0, &py1)) return 0;
    for (int py = py0; py <= py1; py++) {
        int by = py + fb->oy - fb->y_base;
        float dy = (py + 0.5f) - cy;
        float ao = ro2 - dy * dy;
        if (ao <= 0) continue;
        float xo = fast_sqrt(ao);
        float ai = ri2 - dy * dy;
        int solidL = 1, solidR = 0;                 /* empty by default */
        if (ai > 0) { float xi = fast_sqrt(ai); solidL = iceil(cx - xi); solidR = ifloor(cx + xi); }
        uint16_t *row = gfx_row(fb, by) + fb->ox;   /* indexed by logical px; px is
                                                    * clipped to [pxlo,pxhi], all on-buffer */
        int px0 = ifloor(cx - xo), px1 = iceil(cx + xo);
        if (px0 < pxlo) px0 = pxlo;
        if (px1 > pxhi) px1 = pxhi;
        /* interior [s0,s1] is one solid span (no sqrt, no per-pixel branch);
         * only the AA rim pixels either side take a coverage sample */
        int s0 = solidL > px0 ? solidL : px0, s1 = solidR < px1 ? solidR : px1;
        if (s0 > s1) { s0 = px1 + 1; s1 = px1; }   /* no solid part: all rim */
        for (int px = px0; px <= px1; px++) {
            if (px == s0) { fill_span(&row[s0], s1 - s0 + 1, color); n += s1 - s0 + 1; px = s1; continue; }
            float dx = (px + 0.5f) - cx;
            uint8_t a = coverage(r - fast_sqrt(dx * dx + dy * dy));
            if (a == 255) row[px] = color;
            else if (a) { row[px] = gfx_blend565(row[px], color, a); COST_EDGE_ACCUM(blends,1); }
            n += a != 0;
        }
    }
    return n;
}

void gfx_fill_circle(gfx_fb *fb, float cx, float cy, float r, uint16_t color)
{
    NOS_COST_SCOPE(NOS_COST_SHAPE,!fb->rec);
    if (fb->rec) {
        unsigned hh = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_u(3u, color), cx), cy), r);
        gfx_rec_cmd(fb, (int)(cx - r - 1), (int)(cy - r - 1),
                    (int)(cx + r + 2), (int)(cy + r + 2), hh);
        return;
    }
    unsigned nb=0, n = fill_circle_raster(fb, cx, cy, r, gfx_ink(fb, color), &nb);
    GFX_COST(GFX_COST_CIRCLE, n);
    GFX_COST_PSRAM_EDGE(nb);
}

/* One AA cap of a pill: columns [k0,k1] of `row`, distance measured to (ccx, row centre). */
static unsigned pill_cap(uint16_t *row, int k0, int k1, float ccx, float dy2, float r, float ri2,
                         uint16_t color, unsigned *blends)
{
    unsigned n = 0;
    for (int px = k0; px <= k1; px++) {
        float dx = (px + 0.5f) - ccx;
        float d2 = dx * dx + dy2;
        uint8_t a = d2 < ri2 ? 255 : coverage(r - fast_sqrt(d2));
        if (a == 255) row[px] = color;
        else if (a) { row[px] = gfx_blend565(row[px], color, a); COST_EDGE_ACCUM(blends,1); }
        n += a != 0;
    }
    return n;
}

void gfx_fill_pill(gfx_fb *fb, float xa, float cy, float xb, float r, uint16_t color)
{
    NOS_COST_SCOPE(NOS_COST_SHAPE,!fb->rec);
    if (xb < xa) xb = xa;
    if (fb->rec) {
        unsigned hh = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_u(9u, color), xa), cy), xb), r);
        gfx_rec_cmd(fb, ifloor(xa - r - 1), ifloor(cy - r - 1), iceil(xb + r + 1) + 1, iceil(cy + r + 1) + 1, hh);
        return;
    }
    color = gfx_ink(fb, color);
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (c0 < 0) c0 = 0;
    if (c1 > GFX_W) c1 = GFX_W;
    int pxlo = c0 - fb->ox, pxhi = c1 - 1 - fb->ox;     /* logical column clip */
    int lcap = ifloor(xa - r - 1.0f), rcap = iceil(xb + r + 1.0f);
    if (rcap < pxlo || lcap > pxhi) { GFX_COST(GFX_COST_PILL, 0); return; }
    /* body = pixels whose centre lies in [xa, xb]: vertical coverage only */
    int ba = iceil(xa - 0.5f), bb = ifloor(xb - 0.5f);
    int py0 = ifloor(cy - r - 1.0f), py1 = iceil(cy + r + 1.0f);
    if (!clip_rows(fb, &py0, &py1)) { GFX_COST(GFX_COST_PILL, 0); return; }
    unsigned n = 0, nb = 0;   /* pixels written / blended (cost model only) */
    float ri = r - 1.0f, ri2 = ri > 0 ? ri * ri : 0.0f;
    int l0 = lcap < pxlo ? pxlo : lcap, l1 = ba - 1 > pxhi ? pxhi : ba - 1;
    int b0 = ba < pxlo ? pxlo : ba, b1 = bb > pxhi ? pxhi : bb;
    int k0 = bb + 1 < pxlo ? pxlo : bb + 1, k1 = rcap > pxhi ? pxhi : rcap;
    for (int py = py0; py <= py1; py++) {
        uint16_t *row = gfx_row(fb, py + fb->oy - fb->y_base) + fb->ox;
        float dy = (py + 0.5f) - cy, ady = dy < 0 ? -dy : dy;
        uint8_t ay = coverage(r - ady);
        if (!ay) continue;
        float dy2 = dy * dy;
        if (ay == 255 && dy2 < ri2) {
            /* opaque row: the caps' solid cores join the body into ONE span (one sqrt
             * per row); only the few rim pixels outside it take per-pixel coverage */
            float h = fast_sqrt(ri2 - dy2) - 0.01f;
            int s0 = iceil(xa - h - 0.5f), s1 = ifloor(xb + h - 0.5f);
            if (s0 < pxlo) s0 = pxlo;
            if (s1 > pxhi) s1 = pxhi;
            n += pill_cap(row, l0, s0 - 1 < l1 ? s0 - 1 : l1, xa, dy2, r, ri2, color, &nb);
            if (s0 <= s1) { fill_span(&row[s0], s1 - s0 + 1, color); n += s1 - s0 + 1; }
            n += pill_cap(row, s1 + 1 > k0 ? s1 + 1 : k0, k1, xb, dy2, r, ri2, color, &nb);
            continue;
        }
        n += pill_cap(row, l0, l1, xa, dy2, r, ri2, color, &nb);
        if (b0 <= b1) {
            if (ay == 255) fill_span(&row[b0], b1 - b0 + 1, color);
            else { for (int px = b0; px <= b1; px++) row[px] = gfx_blend565(row[px], color, ay); COST_EDGE_ACCUM(&nb,b1-b0+1); }
            n += b1 - b0 + 1;
        }
        n += pill_cap(row, k0, k1, xb, dy2, r, ri2, color, &nb);
    }
    GFX_COST(GFX_COST_PILL, n);
    GFX_COST_PSRAM_EDGE(nb);
}

/* Tapered thick line (clock hand): a swept disc from (ax,ay) half-width ha to
 * (bx,by) half-width hb. Rasterizes as a disc sweep but records a SINGLE damage
 * command — critical, since a hand built from ~100 separate gfx_fill_circle
 * calls would overflow the recorder and force a full-screen redraw every frame. */
void gfx_taper(gfx_fb *fb, float ax, float ay, float bx, float by,
               float ha, float hb, uint16_t color)
{
    NOS_COST_SCOPE(NOS_COST_SHAPE,!fb->rec);
    float hmax = ha > hb ? ha : hb;
    if (fb->rec) {
        unsigned hh = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(
                      gfx_hash_u(8u, color), ax), ay), bx), by), ha), hb);
        gfx_rec_cmd(fb, (int)((ax < bx ? ax : bx) - hmax - 1),
                        (int)((ay < by ? ay : by) - hmax - 1),
                        (int)((ax > bx ? ax : bx) + hmax + 2),
                        (int)((ay > by ? ay : by) + hmax + 2), hh);
        return;
    }
    color = gfx_ink(fb, color);
    /* True tapered capsule SDF (half-width lerps ha->hb along the segment): one
     * stable coverage sample per pixel, no disc-sweep rim shimmer. See gfx_line. */
    float vx = bx - ax, vy = by - ay;
    float vlen2 = vx * vx + vy * vy;
    if (vlen2 < 1e-6f) { GFX_COST(GFX_COST_LINE, fill_circle_raster(fb, ax, ay, hmax, color, 0)); return; }
    float inv = 1.0f / vlen2, rmax = hmax + 1.0f, rmax2 = rmax * rmax;
    unsigned n = 0;   /* pixels written (cost model only) */
    /* 2D dirty-rect x-window (buffer columns; sentinel cx1<=cx0 => full width) */
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (c0 < 0) c0 = 0;
    if (c1 > GFX_W) c1 = GFX_W;
    int px0 = ifloor((ax < bx ? ax : bx) - rmax), px1 = iceil((ax > bx ? ax : bx) + rmax);
    int py0 = ifloor((ay < by ? ay : by) - rmax), py1 = iceil((ay > by ? ay : by) + rmax);
    for (int py = py0; py <= py1; py++) {
        int byi = py + fb->oy - fb->y_base;
        if ((unsigned)byi >= (unsigned)fb->h) continue;
        float fy = py + 0.5f;
        uint16_t *row = gfx_row(fb, byi);
        for (int px = px0; px <= px1; px++) {
            unsigned bxi = (unsigned)(px + fb->ox);
            if (bxi < (unsigned)c0 || bxi >= (unsigned)c1) continue;
            float fx = px + 0.5f;
            float t = ((fx - ax) * vx + (fy - ay) * vy) * inv;
            if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
            float ddx = fx - (ax + t * vx), ddy = fy - (ay + t * vy);
            float d2 = ddx * ddx + ddy * ddy;
            if (d2 >= rmax2) continue;
            float hw = ha + (hb - ha) * t;              /* local half-width */
            uint8_t a = coverage(hw - fast_sqrt(d2));
            if (!a) continue;
            row[bxi] = a == 255 ? color : gfx_blend565(row[bxi], color, a);
            n++;
        }
    }
    GFX_COST(GFX_COST_LINE, n);
}

/* Ring: an annular band. Scanline-rasterized — per row we solve the circle
 * equation for the 1-2 narrow x-spans the band actually occupies, instead of
 * walking the whole r_out^2 bounding box. Cost is O(r) not O(r^2), which is
 * what keeps big-radius rings (and the second-sweep arc) cheap. */
void gfx_ring(gfx_fb *fb, float cx, float cy, float r_mid, float thickness, uint16_t color)
{
    NOS_COST_SCOPE(NOS_COST_SHAPE,!fb->rec);
    if (fb->rec) {
        float ro = r_mid + thickness * 0.5f + 1.0f;
        unsigned hh = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(
                      gfx_hash_u(4u, color), cx), cy), r_mid), thickness);
        gfx_rec_cmd(fb, (int)(cx - ro), (int)(cy - ro),
                    (int)(cx + ro + 1), (int)(cy + ro + 1), hh);
        return;
    }
    color = gfx_ink(fb, color);
    float half = thickness * 0.5f;
    float r_out = r_mid + half + 1.0f;
    float r_in = r_mid - half - 1.0f;
    if (r_in < 0) r_in = 0;
    float r_out2 = r_out * r_out, r_in2 = r_in * r_in;
    /* fully-covered core of the band (no AA) — tested with squared distance so
     * interior pixels skip the (software) sqrt; only the two rims pay for it. */
    float rs_lo = r_mid - (half - 1.0f), rs_hi = r_mid + (half - 1.0f);
    float rs_lo2 = rs_lo > 0 ? rs_lo * rs_lo : 0.0f, rs_hi2 = rs_hi * rs_hi;
    /* 2D dirty-rect x-window (buffer columns, sentinel cx1<=cx0 => full width);
     * px is logical, so the span clamp is [c0-ox, c1-1-ox]. */
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (c0 < 0) c0 = 0;
    if (c1 > GFX_W) c1 = GFX_W;
    int pxlo = c0 - fb->ox, pxhi = c1 - 1 - fb->ox;
    if (iceil(cx + r_out) < pxlo || ifloor(cx - r_out) > pxhi) { GFX_COST(GFX_COST_RING, 0); return; }

    int py0 = ifloor(cy - r_out), py1 = iceil(cy + r_out);
    if (!clip_rows(fb, &py0, &py1)) { GFX_COST(GFX_COST_RING, 0); return; }
    unsigned n = 0;   /* pixels written (cost model only) */
    for (int py = py0; py <= py1; py++) {
        int by = py + fb->oy - fb->y_base;
        float dy = (py + 0.5f) - cy;
        float ao = r_out2 - dy * dy;
        if (ao <= 0) continue;
        float xo = fast_sqrt(ao);
        int hole = (dy * dy < r_in2);
        float xi = hole ? fast_sqrt(r_in2 - dy * dy) : 0.0f;
        uint16_t *row = gfx_row(fb, by);
        for (int seg = 0; seg < (hole ? 2 : 1); seg++) {
            float lx = !hole ? cx - xo : (seg == 0 ? cx - xo : cx + xi);
            float rx = !hole ? cx + xo : (seg == 0 ? cx - xi : cx + xo);
            int px0 = ifloor(lx), px1 = iceil(rx);
            if (px0 < pxlo) px0 = pxlo;
            if (px1 > pxhi) px1 = pxhi;
            for (int px = px0; px <= px1; px++) {
                int bx = px + fb->ox;
                if ((unsigned)bx >= GFX_W) continue;
                float dx = (px + 0.5f) - cx;
                float d2 = dx * dx + dy * dy;
                if (d2 >= rs_lo2 && d2 <= rs_hi2) { row[bx] = color; n++; continue; }  /* solid, no sqrt */
                uint8_t a = coverage(half - fabsf(fast_sqrt(d2) - r_mid));
                if (a == 255) row[bx] = color;
                else if (a) row[bx] = gfx_blend565(row[bx], color, a);
                n += a != 0;
            }
        }
    }
    GFX_COST(GFX_COST_RING, n);
}

/* Is there an integer n with (theta + 2*PI*n) inside [a0, a1]?
 * Callers guarantee a1 - a0 < 2*PI, so at most one n can qualify. */
static int arc_angle_in(float theta, float a0, float a1)
{
    float n = ceilf((a0 - theta) * (1.0f / 6.28318531f));
    return theta + n * 6.28318531f <= a1 + 1e-4f;
}

/* Arc: ring band limited to [a0, a1]. Angular ends get AA too by treating
 * each end as a rounded cap (distance to the end point of the midline). */
void gfx_arc(gfx_fb *fb, float cx, float cy, float r_mid, float thickness,
             float a0, float a1, uint16_t color)
{
    NOS_COST_SCOPE(NOS_COST_SHAPE,!fb->rec);
    if (a1 <= a0) return;
    if (a1 - a0 >= 6.2831f) { gfx_ring(fb, cx, cy, r_mid, thickness, color); return; }
    if (fb->rec) {
        /* Tight damage box = just the swept wedge (sin/cos extent over [a0,a1]
         * plus any cardinal inside it), not the whole ring — so a segmented
         * sweep only dirties its moving tip, not the entire dial. */
        float pad = thickness * 0.5f + 1.0f;
        float s0 = fast_sin(a0), s1 = fast_sin(a1), c0 = fast_cos(a0), c1 = fast_cos(a1);
        float smin = s0 < s1 ? s0 : s1, smax = s0 > s1 ? s0 : s1;
        float cmin = c0 < c1 ? c0 : c1, cmax = c0 > c1 ? c0 : c1;
        if (arc_angle_in( 1.57079633f, a0, a1)) smax =  1.0f;
        if (arc_angle_in(-1.57079633f, a0, a1)) smin = -1.0f;
        if (arc_angle_in( 0.0f,        a0, a1)) cmax =  1.0f;
        if (arc_angle_in( 3.14159265f, a0, a1)) cmin = -1.0f;
        unsigned hh = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(
                      gfx_hash_u(5u, color), cx), cy), r_mid), thickness), a0), a1);
        /* Raster bounds use floor(low)..ceil(high), both inclusive. Record the
         * matching half-open box; plain casts and a single +1 can miss the
         * last row/column when an endpoint crosses a cardinal direction. */
        gfx_rec_cmd(fb, ifloor(cx + smin * r_mid - pad),
                    ifloor(cy - cmax * r_mid - pad),
                    iceil(cx + smax * r_mid + pad) + 1,
                    iceil(cy - cmin * r_mid + pad) + 1, hh);
        return;
    }
    color = gfx_ink(fb, color);
    float half = thickness * 0.5f;
    /* end-cap centers on the midline */
    float e0x = cx + fast_sin(a0) * r_mid, e0y = cy - fast_cos(a0) * r_mid;
    float e1x = cx + fast_sin(a1) * r_mid, e1y = cy - fast_cos(a1) * r_mid;
    /* Tight bbox of just the swept wedge instead of the whole ring: take the
     * sin/cos extent over [a0,a1] (endpoints, plus any cardinal angle that
     * falls inside the sweep) and pad for thickness + one pixel of AA. This
     * skips the expensive per-pixel atan2f over the entire ring interior. */
    float s0 = fast_sin(a0), s1 = fast_sin(a1), c0 = fast_cos(a0), c1 = fast_cos(a1);
    float smin = s0 < s1 ? s0 : s1, smax = s0 > s1 ? s0 : s1;
    float cmin = c0 < c1 ? c0 : c1, cmax = c0 > c1 ? c0 : c1;
    if (arc_angle_in( 1.57079633f, a0, a1)) smax =  1.0f;  /* sin peaks +1 */
    if (arc_angle_in(-1.57079633f, a0, a1)) smin = -1.0f;  /* sin dips  -1 */
    if (arc_angle_in( 0.0f,        a0, a1)) cmax =  1.0f;  /* cos peaks +1 */
    if (arc_angle_in( 3.14159265f, a0, a1)) cmin = -1.0f;  /* cos dips  -1 */
    float pad = half + 1.0f;
    /* Angular sector test via dot with the sector bisector — avoids per-pixel
     * atan2f. A pixel is inside [a0,a1] iff cos(angle to bisector) >=
     * cos(halfwidth); multiply both sides by dist>=0 to drop the divide. */
    float bis = 0.5f * (a0 + a1);
    float bx = fast_sin(bis), by = -fast_cos(bis);
    float cos_hw = fast_cos(0.5f * (a1 - a0));
    float cos_hw2 = cos_hw * cos_hw;   /* for the squared (sqrt-free) angular test */
    /* Scanline the annular band inside the wedge bbox (O(band) not O(bbox)).
     * y = cy - cos*r, so the row range uses the cos extent. */
    float r_out = r_mid + pad, r_in = r_mid - pad;
    if (r_in < 0) r_in = 0;
    float r_out2 = r_out * r_out, r_in2 = r_in * r_in;
    /* solid core of the band (no AA) — squared-distance test skips the sqrt */
    float rs_lo = r_mid - (half - 1.0f), rs_hi = r_mid + (half - 1.0f);
    float rs_lo2 = rs_lo > 0 ? rs_lo * rs_lo : 0.0f, rs_hi2 = rs_hi * rs_hi;
    float wx0 = cx + smin * r_mid - pad, wx1 = cx + smax * r_mid + pad;
    /* 2D dirty-rect x-window (buffer columns, sentinel cx1<=cx0 => full width);
     * px is logical, so the span clamp is [cc0-ox, cc1-1-ox]. */
    int cc0 = fb->cx0, cc1 = fb->cx1;
    if (cc1 <= cc0) { cc0 = 0; cc1 = GFX_W; }
    if (cc0 < 0) cc0 = 0;
    if (cc1 > GFX_W) cc1 = GFX_W;
    int pxlo = cc0 - fb->ox, pxhi = cc1 - 1 - fb->ox;
    if (iceil(wx1) < pxlo || ifloor(wx0) > pxhi) { GFX_COST(GFX_COST_ARC, 0); return; }
    int py0 = ifloor(cy - cmax * r_mid - pad);
    int py1 = iceil(cy - cmin * r_mid + pad);
    if (!clip_rows(fb, &py0, &py1)) { GFX_COST(GFX_COST_ARC, 0); return; }
    unsigned n = 0;   /* pixels written (cost model only) */
    for (int py = py0; py <= py1; py++) {
        int by_ = py + fb->oy - fb->y_base;
        float dy = (py + 0.5f) - cy;
        float ao = r_out2 - dy * dy;
        if (ao <= 0) continue;
        float xo = fast_sqrt(ao);
        int hole = (dy * dy < r_in2);
        float xi = hole ? fast_sqrt(r_in2 - dy * dy) : 0.0f;
        uint16_t *row = gfx_row(fb, by_);
        for (int seg = 0; seg < (hole ? 2 : 1); seg++) {
            float lx = !hole ? cx - xo : (seg == 0 ? cx - xo : cx + xi);
            float rx = !hole ? cx + xo : (seg == 0 ? cx - xi : cx + xo);
            if (lx < wx0) lx = wx0;
            if (rx > wx1) rx = wx1;
            int px0 = ifloor(lx), px1 = iceil(rx);
            if (px0 < pxlo) px0 = pxlo;
            if (px1 > pxhi) px1 = pxhi;
            for (int px = px0; px <= px1; px++) {
                int bxc = px + fb->ox;
                if ((unsigned)bxc >= GFX_W) continue;
                float dx = (px + 0.5f) - cx;
                float d2 = dx * dx + dy * dy;
                float dot = dx * bx + dy * by;
                /* Angular sector test with squared values — no sqrt. Inside iff
                 * dot >= dist*cos_hw; square it (guarding signs) to drop sqrt. */
                int inside = cos_hw >= 0.0f
                    ? (dot >= 0.0f && dot * dot >= d2 * cos_hw2)
                    : (dot >= 0.0f || dot * dot <= d2 * cos_hw2);
                uint8_t a;
                if (inside) {
                    if (d2 >= rs_lo2 && d2 <= rs_hi2) a = 255;  /* solid core, no sqrt */
                    else a = coverage(half - fabsf(fast_sqrt(d2) - r_mid));  /* AA rim */
                } else {
                    /* outside the sweep: only the rounded end caps contribute */
                    float d0x = (px + 0.5f) - e0x, d0y = dy + cy - e0y;
                    float d1x = (px + 0.5f) - e1x, d1y = dy + cy - e1y;
                    float dcap = fast_sqrt(d0x * d0x + d0y * d0y);
                    float dcap1 = fast_sqrt(d1x * d1x + d1y * d1y);
                    if (dcap1 < dcap) dcap = dcap1;
                    a = coverage(half - dcap);
                }
                if (a == 255) row[bxc] = color;
                else if (a) row[bxc] = gfx_blend565(row[bxc], color, a);
                n += a != 0;
            }
        }
    }
    GFX_COST(GFX_COST_ARC, n);
}

/* Growing sweep arc [0,a1] split into a "settled" part (snapped down to the
 * nearest `snap` multiple, so it only changes on a snap tick) and a small
 * moving tip. Under retained-mode damage that means the tip repaints every
 * frame but the settled arc only occasionally — 2 arcs instead of a single
 * arc whose whole wedge re-dirties each frame. */
void gfx_sweep(gfx_fb *fb, float cx, float cy, float r, float th,
               float a1, float snap, uint16_t color)
{
    NOS_COST_SCOPE(NOS_COST_SHAPE,!fb->rec);
    if (a1 <= 0.0f) return;
    /* Splitting an anti-aliased arc changes its pixels at the join: both arcs
     * paint a rounded cap there, so the overlapping fringe is blended twice.
     * The split is only a retained-mode recording trick. Raster mode uses the
     * original primitive and therefore remains exactly pixel-identical to
     * gfx_arc([0,a1]), including clipped stripes and partial x windows. */
    if (!fb->rec) {
        gfx_arc(fb, cx, cy, r, th, 0.0f, a1, color);
        return;
    }
    if (snap <= 0.0f) {
        gfx_arc(fb, cx, cy, r, th, 0.0f, a1, color);
        return;
    }
    /* The squared bisector test used by gfx_arc becomes ill-conditioned as a
     * sector approaches a full turn. Above 1.8*pi, record the same unsplit arc
     * that raster mode draws so its conservative wedge box covers every AA
     * boundary pixel. This is only the final 10% of a growing sweep. */
    if (a1 >= 5.65486678f) {
        gfx_arc(fb, cx, cy, r, th, 0.0f, a1, color);
        return;
    }
    float wa = floorf(a1 / snap) * snap;
    if (wa > 0.0002f)        gfx_arc(fb, cx, cy, r, th, 0.0f, wa, color);
    if (a1 > wa + 0.0002f) {
        int tip_cmd = fb->rec->n;
        gfx_arc(fb, cx, cy, r, th, wa, a1, color);
        /* Include the adjacent AA sample on each side of the analytical box;
         * compute_damage rounds this guard outward to the panel's even pixel
         * transfer bounds. */
        if (tip_cmd < GFX_MAX_CMDS && tip_cmd < fb->rec->n) {
            gfx_cmd *tip = &fb->rec->c[tip_cmd];
            tip->x0 -= 2; tip->y0 -= 2;
            tip->x1 += 2; tip->y1 += 2;
            if (tip->x0 < 0) tip->x0 = 0;
            if (tip->y0 < 0) tip->y0 = 0;
            if (tip->x1 > GFX_W) tip->x1 = GFX_W;
            if (tip->y1 > GFX_H) tip->y1 = GFX_H;
        }
    }
}

/* Thick AA line = capsule SDF (distance to segment minus half-width). */
void gfx_line(gfx_fb *fb, float x0, float y0, float x1, float y1,
              float width, uint16_t color)
{
    NOS_COST_SCOPE(NOS_COST_SHAPE,!fb->rec);
    float half = width * 0.5f;
    if (fb->rec) {
        unsigned hh = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(
                      gfx_hash_u(6u, color), x0), y0), x1), y1), width);
        gfx_rec_cmd(fb, (int)((x0 < x1 ? x0 : x1) - half - 1),
                        (int)((y0 < y1 ? y0 : y1) - half - 1),
                        (int)((x0 > x1 ? x0 : x1) + half + 2),
                        (int)((y0 > y1 ? y0 : y1) + half + 2), hh);
        return;
    }
    color = gfx_ink(fb, color);
    /* Rasterize as a sweep of discs (radius = half) along the segment: O(len)
     * work, not the O(len^2) of scanning the whole diagonal bounding box. */
    /* True capsule SDF: ONE coverage sample per pixel (distance to the segment
     * minus half-width). A disc SWEEP double-blends where neighbouring discs'
     * AA rims overlap, and which discs overlap a pixel shifts as the hand
     * rotates -> the edge shimmers 1px up/down. A single SDF sample is stable.
     * Far pixels are rejected on squared distance before the sqrt. */
    float vx = x1 - x0, vy = y1 - y0;
    float vlen2 = vx * vx + vy * vy;
    if (vlen2 < 1e-6f) { GFX_COST(GFX_COST_LINE, fill_circle_raster(fb, x0, y0, half, color, 0)); return; }
    float rmax = half + 1.0f, rmax2 = rmax * rmax;
    /* 2D dirty-rect x-window (buffer columns; sentinel cx1<=cx0 => full width) */
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (c0 < 0) c0 = 0;
    if (c1 > GFX_W) c1 = GFX_W;
    int bx0 = ifloor((x0 < x1 ? x0 : x1) - rmax), bx1 = iceil((x0 > x1 ? x0 : x1) + rmax);
    int py0 = ifloor((y0 < y1 ? y0 : y1) - rmax), py1 = iceil((y0 > y1 ? y0 : y1) + rmax);
    if (bx1 + fb->ox < c0 || bx0 + fb->ox >= c1) { GFX_COST(GFX_COST_LINE, 0); return; }
    if (!clip_rows(fb, &py0, &py1)) { GFX_COST(GFX_COST_LINE, 0); return; }
    unsigned n = 0;   /* pixels written (cost model only) */
    if (bx0 < c0 - fb->ox) bx0 = c0 - fb->ox;
    if (bx1 > c1 - 1 - fb->ox) bx1 = c1 - 1 - fb->ox;
    float inv = 1.0f / vlen2;
    /* Any pixel with coverage lies within rmax of the segment, so within rmax of
     * its infinite line: on row fy that is |x - xc| <= rmax*len/|vy|. Visiting just
     * that strip (+1 px of float margin) turns the O(bbox) scan of a diagonal
     * hand into O(length); pixels outside it were all rejected by d2 >= rmax2. */
    float avy = vy < 0 ? -vy : vy;
    int banded = avy > 1e-3f;
    float hw = banded ? rmax * sqrtf(vlen2) / avy : 0.0f, slope = banded ? vx / vy : 0.0f;
    for (int py = py0; py <= py1; py++) {
        int by = py + fb->oy - fb->y_base;
        float fy = py + 0.5f;
        uint16_t *row = gfx_row(fb, by);
        int lo = bx0, hi = bx1;
        if (banded) {
            float xc = x0 + (fy - y0) * slope;
            int a0 = ifloor(xc - hw - 0.5f) - 1, a1 = iceil(xc + hw - 0.5f) + 1;
            if (a0 > lo) lo = a0;
            if (a1 < hi) hi = a1;
        }
        for (int px = lo; px <= hi; px++) {
            unsigned bxi = (unsigned)(px + fb->ox);
            if (bxi < (unsigned)c0 || bxi >= (unsigned)c1) continue;
            float fx = px + 0.5f;
            float t = ((fx - x0) * vx + (fy - y0) * vy) * inv;
            if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
            float ddx = fx - (x0 + t * vx), ddy = fy - (y0 + t * vy);
            float d2 = ddx * ddx + ddy * ddy;
            if (d2 >= rmax2) continue;
            uint8_t a = coverage(half - fast_sqrt(d2));
            if (!a) continue;
            row[bxi] = a == 255 ? color : gfx_blend565(row[bxi], color, a);
            n++;
        }
    }
    GFX_COST(GFX_COST_LINE, n);
}
