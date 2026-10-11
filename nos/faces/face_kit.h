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

/* "HH" (24 h zero-padded; 12 h with a blank leading digit, " 9") and "MM". */
void  fk_hh(char out[3], int hour);
void  fk_mm(char out[3], int minute);
int   fk_hour_disp(int hour);                        /* 0-23 or 1-12 per prefs */
/* "HH:MM" (24 h) or "H:MM" (12 h, the blank leading digit trimmed), for centred times */
void  fk_time(char out[8], int hour, int minute);
/* the date label, "SUN 11" */
void  fk_date(char out[16], const face_ctx *c);

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
void  fk_morph_settle(fk_morph *m);                  /* jump to the current string */

/* A label cross-fade (the date at midnight): out in 2 steps, then in over 3 (~0.33 s).
 * snap = no fade (AOD, build-in, a stale face). */
typedef struct { char from[16], to[16]; float t0; int init; } fk_xfade;
void  fk_xfade_tick(fk_xfade *x, const char *now, float t, int snap);
void  fk_xfade_k(const fk_xfade *x, float t, float *a_from, float *a_to);
/* draws both sides as ui_label at (px, py) in col; returns the width of the new text */
int   fk_xfade_label(gfx_fb *fb, const fk_xfade *x, float t, int px, int py, uint16_t col, int align);

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

/* ---- wake from AOD: the time stays on screen and morphs into the awake style ----
 * Call fk_wake_tick() in tick. While a wake morph runs (about 0.2 s), fk_build()
 * reports 1 so the face skips its build-in, fk_wake_k() steps 1/6 -> 1 (one step per
 * frame, counted, so the rhythm is even; 0 in AOD), and fk_wake_off() slides the AOD drift back to 0. */
typedef struct { int last_aod; float t0; int active; int dx, dy;
                 uint32_t last_ms; float last_enter; int skip; int frames; int stale; } fk_wake;
/* fk_wake also suppresses a build-in restarted while the face never left the glass
 * (ticked within the last 200 ms of wall time, e.g. returning from the face picker):
 * fk_build() then reports 1 and fk_wake_skip() is true. */
int   fk_wake_skip(const fk_wake *w);
/* 1 in the tick that follows a gap of more than 0.5 s without ticks (the face was off
 * the glass: picker neighbours, an app on top): faces settle their morphs instead of
 * replaying a change nobody saw. Valid after fk_wake_tick() in the same tick. */
int   fk_wake_stale(const fk_wake *w);
void  fk_wake_tick(fk_wake *w, const face_ctx *c);
float fk_build(const fk_wake *w, const face_ctx *c);   /* effective build-in progress */
float fk_wake_k(const fk_wake *w, const face_ctx *c);
void  fk_wake_off(const fk_wake *w, const face_ctx *c, float *dx, float *dy);

/* ---- unread notifications: one dot at (233, 24) ----
 * fk_unread_tick() in tick (fades in/out in 4 colour steps); fk_unread() last in draw.
 * 8 px, like the widget pages' dot. col = C_RED unless the face shows another red at the
 * same time (at most one red), then C_PRI (C_INK on white faces): DOT, GLYPH, INVERTED,
 * LIFE and TAPE always; SEGMENT and SPLIT only while their red state is up; every face
 * while recording (pass fk_red(c, C_PRI) as the red: it rests white under the REC chip).
 * In AOD it is a 6 px C_SEC dot. backing = colour of the disc behind it. */
void  fk_unread_tick(const face_ctx *c);
void  fk_unread(gfx_fb *fb, const face_ctx *c, uint16_t col, uint16_t backing, float dx, float dy);
/* same with an extra opacity a (0..1) for faces fading it with their own motion */
void  fk_unread_a(gfx_fb *fb, const face_ctx *c, uint16_t col, uint16_t backing, float dx, float dy, float a);
float fk_unread_level(const face_ctx *c);            /* current fade level 0..1 */

/* ---- one red per screen: while the REC chip is up (c->rec 0..1, stepped by the shell)
 * a face's own red accents ease to alt (C_PRI; C_INK on white). TAPE keeps its lamp red:
 * there the lamp is the recording light and the chip drops its dot. */
static inline float fk_rec(const face_ctx *c)
{
    return c->rec <= 0.0f ? 0.0f : c->rec >= 1.0f ? 1.0f : c->rec;
}
static inline uint16_t fk_red(const face_ctx *c, uint16_t alt)
{
    float r = fk_rec(c);
    return r > 0.0f ? ui_mix(C_RED, alt, r) : C_RED;
}

/* ---- battery status: nothing above 20 % unless charging; else a bolt (charging)
 * and the percentage as a mono label at baseline y. Returns the drawn width.
 * In AOD only a critical battery (< 10 %, not charging) shows, as the bare "4%". ---- */
int   fk_status_shown(const face_ctx *c);
int   fk_status(gfx_fb *fb, const face_ctx *c, int x, int y, uint16_t col, int align);

/* ---- a font scaled once into PSRAM coverage masks (digits and ':' of nf_hero at a
 * size the baked fonts do not have). Drawn like ui_text: baseline y, one damage cmd. */
typedef struct { uint8_t w, h; int8_t xoff, yoff; uint8_t adv; uint8_t *px; } fk_sglyph;
typedef struct { fk_sglyph g[12]; int ascent, cap; int ok; } fk_sfont;    /* '0'-'9', ':', ' ' */
int   fk_sfont_build(fk_sfont *sf, const font_t *src, float scale);
int   fk_stext_width(const fk_sfont *sf, const char *s);
int   fk_stext(gfx_fb *fb, const fk_sfont *sf, int x, int y, const char *s, uint16_t col, int align);

/* ---- odometer roll of one digit cell: `from` rolls up and out, `to` rises in, both
 * clipped to [x - 4, x + cell_w + 4) x [base - cap - 6, base + 6). Exactly one of
 * f / sf is used. p 0..1 (EASE_MECH applied inside). ---- */
void  fk_roll(gfx_fb *fb, const font_t *f, const fk_sfont *sf, int x, int base, int cell_w,
              int cap, char from, char to, float p, uint16_t col);
/* same, with a per-glyph x nudge for the outgoing / incoming glyph (optical bearings) */
void  fk_roll2(gfx_fb *fb, const font_t *f, const fk_sfont *sf, int x, int base, int cell_w,
               int cap, char from, char to, int nx_from, int nx_to, float p, uint16_t col);

/* once-a-second blink envelope (0..1) from the millisecond clock: grows in 100 ms,
 * holds to 500 ms, shrinks by 600 ms; 4 steps each way */
float fk_blink(int ms);

/* glyph metrics of a baked font (0 if missing) */
const font_glyph *fk_glyph(const font_t *f, unsigned code);

/* complication hit tests with press rects (face_ext.h) */
const struct nos_screen *face_split_hit(int x, int y, int r[4]);
const struct nos_screen *face_segment_hit(int x, int y, int r[4]);
const struct nos_screen *face_inverted_hit(int x, int y, int r[4]);

/* A tap at (x, y) inside the circle of radius r around (cx, cy)? */
static inline int fk_in(int x, int y, float cx, float cy, float r)
{ float dx = (float)x - cx, dy = (float)y - cy; return dx * dx + dy * dy <= r * r; }
#endif
