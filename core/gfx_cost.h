/* gfx_cost.h — device cost-model hooks for the raster primitives.
 *
 * The simulator (sim/nos_sim.c) estimates how long each frame would take on the
 * ESP32-S3 by counting, per primitive call made in RASTER mode (never record
 * mode), the pixels it actually wrote inside the clipped stripe / dirty rect,
 * and pricing them with the per-pixel costs measured on the watch
 * (docs/FLAT60_DESIGN.md, core/gfx_bench.c). Every raster call is also one
 * "call" (~0.4 us of setup + clip tests per stripe it is invoked in).
 *
 * Usage in a primitive (exactly ONE GFX_COST executes per raster-mode call,
 * including calls that clip away to nothing — those still cost the call):
 *
 *     void my_prim(gfx_fb *fb, ...) {
 *         if (fb->rec) { ...record...; return; }
 *         unsigned n = 0;            // pixels written
 *         ...  n += span; ...  if (a) { write; n++; } ...
 *         GFX_COST(GFX_COST_MASK, n);
 *     }
 *
 * GFX_COST_PX(cls, px) adds pixels WITHOUT counting a call: use it from an inner
 * helper (e.g. a per-dot stamp) when the public entry point already does
 * GFX_COST(cls, 0) once.
 *
 * Unless GFX_COST_MODEL is defined (only the simulator defines it) both macros
 * compile to a void cast of the count, so the firmware's code is unchanged and
 * the optimiser drops the dead counters. They never affect rendered pixels. */
#ifndef GFX_COST_H
#define GFX_COST_H

/* Primitive classes, ns per written pixel measured on the watch (16-row stripes). */
enum {
    GFX_COST_FILL = 0,  /*   4  clear, fill_rect */
    GFX_COST_CIRCLE,    /*  25  AA disc, span filled */
    GFX_COST_PILL,      /*  32  AA capsule */
    GFX_COST_BLIT,      /*  33  sprite / sprite8 blit from PSRAM */
    GFX_COST_MASK,      /* 131  8-bit coverage mask stamp (icon cache, dot stamps) */
    GFX_COST_GLYPH,     /* 146  big baked glyphs (digits) */
    GFX_COST_BLEND,     /* 190  alpha blend */
    GFX_COST_TEXT,      /* 280  small/medium baked text */
    GFX_COST_RING,      /* 300  AA ring */
    GFX_COST_ARC,       /* 507  AA arc */
    GFX_COST_ICON,      /* 641  vector icon */
    GFX_COST_LINE,      /* 723  AA line / taper */
    GFX_COST_DIM,       /*   8  engine: transition dimming (nos.c dim_rows) */
    GFX_COST_SELECT,    /*   6  engine: iris/dissolve per-pixel layer select (composite_two) */
    GFX_COST_NCLASS
};

#ifdef GFX_COST_MODEL
void gfx_cost_add(int cls, unsigned px, int calls);   /* sim/nos_sim.c */
#define GFX_COST(cls, px)    gfx_cost_add((cls), (unsigned)(px), 1)
#define GFX_COST_PX(cls, px) gfx_cost_add((cls), (unsigned)(px), 0)
#else
#define GFX_COST(cls, px)    ((void)(px))
#define GFX_COST_PX(cls, px) ((void)(px))
#endif

#endif
