/* face_kit.h — small helpers shared by the NOS faces (implemented in faces.c).
 * Private to nos/faces: nothing outside this directory includes it. */
#ifndef NOS_FACE_KIT_H
#define NOS_FACE_KIT_H
#include "faces/face.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"

/* Eased build-in progress of item i of n (stagger), from raw enter 0..1. */
float fk_ease(float p);                              /* EASE_STANDARD, clamped */
float fk_mech(float p);                              /* EASE_MECH, clamped */
float fk_stagger(float enter, float start, float span); /* eased (enter-start)/span */

/* "HH" (24 h or 12 h per prefs, always two digits) and "MM". */
void  fk_hh(char out[3], int hour);
void  fk_mm(char out[3], int minute);
int   fk_hour_disp(int hour);                        /* 0-23 or 1-12 per prefs */

/* Logical rows/columns the raster target holds: a primitive whose box misses
 * these can skip its work (draw() runs once per stripe). */
typedef struct { float x0, y0, x1, y1; } fk_view;
void  fk_view_of(const gfx_fb *fb, fk_view *v);
static inline int fk_hit(const fk_view *v, float x0, float y0, float x1, float y1)
{ return x1 > v->x0 && x0 < v->x1 && y1 > v->y0 && y0 < v->y1; }

/* A minute-change morph tracker: call fk_morph_tick each frame with the current
 * string; it reports the previous string and progress (1 = settled). */
typedef struct { char from[12], to[12]; float t0; int init; } fk_morph;
void  fk_morph_tick(fk_morph *m, const char *now, float t);
float fk_morph_p(const fk_morph *m, float t, float dur);

/* Outline a text (AOD): grey ring of offsets, then black core. One call per offset. */
void  fk_text_outline(gfx_fb *fb, const font_t *f, int x, int y, const char *s,
                      uint16_t color, int align, int tracking, int w);

/* Shared shift for the AOD (set in tick from face_aod_shift). */
extern int fk_aod_dx, fk_aod_dy;
void  fk_aod_update(const face_ctx *c);

/* Accurate fast sine/cosine (gfx_fast_sin is off by up to 0.04 near +-pi, which
 * puts dots 8 px out of place on a bezel ring, so the faces use these). */
float fk_sin(float x);
float fk_cos(float x);

/* A set of n anti-aliased dots produced by fn(i): recorded as one damage command per
 * `chunk` consecutive dots (bbox + hash of what they draw), rasterised with
 * dot_disc only where they touch the stripe. d <= 0 skips a dot. */
typedef void (*fk_dot_fn)(int i, const void *ctx, float *x, float *y, float *d, uint16_t *col);
void  fk_dots(gfx_fb *fb, int n, int chunk, fk_dot_fn fn, const void *ctx);

/* dot_ring replacement with accurate positions: n dots on radius r from 12 o'clock
 * clockwise, dots [0, lit) in `on`, the rest in `off` (0 = skip). 8 dots per command. */
void  fk_ring(gfx_fb *fb, float cx, float cy, float r, int n, float d, int lit,
              uint16_t on, uint16_t off);

/* A tap at (x, y) inside the circle of radius r around (cx, cy)? */
static inline int fk_in(int x, int y, float cx, float cy, float r)
{ float dx = (float)x - cx, dy = (float)y - cy; return dx * dx + dy * dy <= r * r; }
#endif
