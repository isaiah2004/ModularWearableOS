/* gfx.h — software rendering engine for a round RGB565 framebuffer.
 * All shape edges are anti-aliased (coverage from signed distance),
 * which matters a lot on a small round display.
 *
 * Target: Waveshare ESP32-S3-Touch-AMOLED-1.43, 466x466 round AMOLED.
 * Layouts are authored against a 240x240 design grid and scaled with
 * GFX_S(), so the core stays resolution-agnostic. */
#ifndef WATCH_GFX_H
#define WATCH_GFX_H

#include <stdint.h>

#define GFX_W 466
#define GFX_H 466

#define GFX_CX (GFX_W / 2.0f)
#define GFX_CY (GFX_H / 2.0f)

/* scale a coordinate authored on the original 240px design grid */
#define GFX_S(v) ((float)(v) * (GFX_W / 240.0f))

/* Retained-mode damage recorder. When a gfx_fb carries a non-NULL `rec`, the
 * drawing calls don't rasterize — they append one (bounding-box, param-hash)
 * entry per primitive. Diffing this frame's list against last frame's (by
 * index) yields exactly which rows changed, so the OS re-renders only those. */
#define GFX_MAX_CMDS 320
typedef struct { int x0, y0, x1, y1; unsigned hash; } gfx_cmd;
typedef struct { gfx_cmd c[GFX_MAX_CMDS]; int n; int overflow; } gfx_rec;

typedef struct {
    uint16_t *px;   /* row-major, stride GFX_W, `h` rows tall */
    int ox, oy;     /* content translate for slide transitions */
    int y_base;     /* logical y shown at buffer row 0 (a stripe's top); 0 for
                       a full frame. Kept separate from oy so a full-screen
                       clear (oy==0) is distinguishable from a mid-slide clear. */
    int h;          /* rows in this buffer: GFX_H for a full frame, else the
                       stripe height. Logical y -> buffer row (y+oy-y_base),
                       clipped to [0,h). */
    int cx0, cx1;   /* logical x-clip window [cx0,cx1) for 2D dirty-rect render:
                       gfx_clear and layer_blit only touch these columns so a
                       thin change repaints a thin column, not a full row-band.
                       0..GFX_W means "no clip" (whole width). */
    gfx_rec *rec;   /* non-NULL => record commands instead of rasterizing */
    /* Column-band buffers (device scan-order flush): the buffer holds only buffer
     * columns [x_base, x_base+stride) of each row, packed `stride` apart. Both 0
     * (the default for every positional initializer) = full GFX_W-wide rows. All
     * column clipping stays in screen columns (cx0/cx1), so only row addressing
     * changes - always go through gfx_row(). */
    int stride, x_base;
    /* Ink dim for transition backdrops: 0 = none, else every colour drawn keeps
     * (32 - dim)/32 of each channel. Applied once per primitive (dimming is
     * linear, so AA blends against an already-dimmed backdrop agree), which makes
     * a dimmed screen cost the same as a bright one. Writers call gfx_ink(). */
    int dim;
} gfx_fb;

/* Pointer such that gfx_row(fb, r)[bx] is buffer row r, buffer column bx (screen
 * column + ox). With a column band this points before the band's first pixel
 * (bx < x_base is never written: every writer clips to cx0/cx1 inside the band). */
static inline uint16_t *gfx_row(const gfx_fb *fb, int r)
{
    return fb->px + (r * (fb->stride ? fb->stride : GFX_W) - fb->x_base);
}

/* The colour a primitive actually writes into `fb` (see gfx_fb.dim). Truncating
 * and dropping green's low bit keeps neutral greys neutral (g6 = 2 * r5). */
static inline uint16_t gfx_ink(const gfx_fb *fb, uint16_t c)
{
    if (!fb->dim) return c;
    uint32_t d = (c | ((uint32_t)c << 16)) & 0x07E0F81Fu;
    d = ((d * (uint32_t)(32 - fb->dim)) >> 5) & 0x07C0F81Fu;
    return (uint16_t)(d | (d >> 16));
}

/* Record a primitive's screen bbox + a hash of its visual parameters. Returns
 * 1 if the fb is in record mode (caller must then return without drawing),
 * 0 to rasterize normally. Defined in gfx.c; used by gfx/font/layer/sprites. */
int gfx_rec_cmd(gfx_fb *fb, int x0, int y0, int x1, int y1, unsigned hash);
unsigned gfx_hash_f(unsigned h, float v);
unsigned gfx_hash_u(unsigned h, unsigned v);

/* Fast approximations of libm calls that are software (no HW transcendentals /
 * sqrt on the ESP32-S3 — ~1000 cycles each). Accurate to well under a pixel
 * (sin/cos/sqrt) or a couple percent (exp), and ~50-100x cheaper. Use these on
 * per-frame hot paths (icon glyphs, focal-scale bells) instead of sinf/etc. */
float gfx_fast_sin(float x);
float gfx_fast_cos(float x);
float gfx_fast_sqrt(float x);
float gfx_fast_exp(float x);

/* RGB565 from 8-bit channels */
#define GFX_RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

#define GFX_BLACK  GFX_RGB(0, 0, 0)
#define GFX_WHITE  GFX_RGB(255, 255, 255)
#define GFX_RED    GFX_RGB(235, 60, 50)
#define GFX_GREY   GFX_RGB(128, 128, 128)
#define GFX_DGREY  GFX_RGB(45, 45, 48)
#define GFX_ACCENT GFX_RGB(90, 200, 250)
#define GFX_GREEN  GFX_RGB(60, 200, 120)
#define GFX_ORANGE GFX_RGB(255, 150, 40)
#define GFX_PURPLE GFX_RGB(170, 120, 250)
#define GFX_YELLOW GFX_RGB(250, 205, 70)
#define GFX_PINK   GFX_RGB(250, 100, 160)

uint16_t gfx_blend565(uint16_t dst, uint16_t src, uint8_t alpha);

void gfx_clear(gfx_fb *fb, uint16_t color);
void gfx_fill_rect(gfx_fb *fb, int x, int y, int w, int h, uint16_t color);
void gfx_px_blend(gfx_fb *fb, int x, int y, uint16_t color, uint8_t alpha);

/* Anti-aliased primitives. Coordinates are float so hands/ticks can sit
 * at exact angles; angles are radians, 0 = 12 o'clock, clockwise. */
void gfx_fill_circle(gfx_fb *fb, float cx, float cy, float r, uint16_t color);
/* Horizontal capsule of radius r: round caps centred (xa, cy) and (xb, cy), fully
 * anti-aliased. Solid spans are word-filled; only the cap rims and the two
 * fractional edge rows pay per-pixel coverage math. */
void gfx_fill_pill(gfx_fb *fb, float xa, float cy, float xb, float r, uint16_t color);
void gfx_ring(gfx_fb *fb, float cx, float cy, float r_mid, float thickness, uint16_t color);
void gfx_arc(gfx_fb *fb, float cx, float cy, float r_mid, float thickness,
             float a0, float a1, uint16_t color);
void gfx_line(gfx_fb *fb, float x0, float y0, float x1, float y1,
              float width, uint16_t color);
/* Tapered thick line for clock hands: half-width ha at (ax,ay) -> hb at (bx,by).
 * One damage command (unlike sweeping fill_circles), so partial redraw works. */
void gfx_taper(gfx_fb *fb, float ax, float ay, float bx, float by,
               float ha, float hb, uint16_t color);
/* Growing sweep arc [0,a1]. Recording exposes a settled part (snapped to
 * `snap`) plus a moving tip so partial redraw usually repaints only the tip;
 * raster mode draws one gfx_arc and preserves its rounded caps exactly. */
void gfx_sweep(gfx_fb *fb, float cx, float cy, float r, float th,
               float a1, float snap, uint16_t color);

#endif
