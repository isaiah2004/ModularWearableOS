/* face_kit.h — small helpers shared by the NOS faces (implemented in faces.c).
 * Private to nos/faces: nothing outside this directory includes it. */
#ifndef NOS_FACE_KIT_H
#define NOS_FACE_KIT_H
#include "faces/face.h"
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"

/* ---- motion audit (docs/MOTION30.html §14, sim only: nothing on the watch) ----
 * fk_probe("dot", "sec", MC_DECORATIVE, d) reports "dot.sec" from tick();
 * fk_probe_common() the wake (ink %), the AOD drift and the unread dot every face shares. */
#ifdef GFX_COST_MODEL
void  fk_probe(const char *face, const char *what, int cls, float px);
/* fresh = this frame's value is the origin of a new move (a jump that is not motion) */
void  fk_probe_at(const char *face, const char *what, int cls, float px, int fresh);
/* 1 when the clock moved by anything but 0 or +1 s since the last call (sim `time`) */
int   fk_probe_jump(const face_ctx *c, int *last);
#else
#define fk_probe(face, what, cls, px) ((void)0)
#define fk_probe_at(face, what, cls, px, fresh) ((void)0)
#endif

/* frame tables (mf_at): FK_SNAP3 50/83/100 (a dot snapping in), FK_TICK4 40/72/92/100
 * (a hand clicking on, a hub stepping): peak frame <= 1.6x the average step */
extern const mframes FK_SNAP3, FK_TICK4;
/* whole frames from t0 to t (rounded): every motion is counted in frames (R3, R9) */
int   fk_frames(nos_time_t t, nos_time_t t0);
/* the home build-in (c->enter 0..1) lasts ENTER_S = 0.40 s = 12 frames: its frame index */
#define FK_BUILD_N 12
static inline int fk_build_k(float e) { return (int)(e * (float)FK_BUILD_N + 0.5f); }

/* "HH" (24 h zero-padded; 12 h with a blank leading digit, " 9") and "MM". */
void  fk_hh(char out[3], int hour);
void  fk_mm(char out[3], int minute);
int   fk_hour_disp(int hour);                        /* 0-23 or 1-12 per prefs */
/* "HH:MM" (24 h) or "H:MM" (12 h, the blank leading digit trimmed), for centred times */
void  fk_time(char out[8], int hour, int minute);
/* ROM-font clock for incomplete/denied face resources; requires no allocation. */
void  fk_clock_fallback(gfx_fb *fb, const face_ctx *c, int x, int baseline);
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
typedef struct { char from[12], to[12]; nos_time_t t0; int init; } fk_morph;
void  fk_morph_tick(fk_morph *m, const char *now, nos_time_t t);
/* progress of an n-frame morph in whole frames, already 1/n on the change frame */
float fk_morph_pk(const fk_morph *m, nos_time_t t, int n);
void  fk_morph_settle(fk_morph *m);                  /* jump to the current string */

/* A label cross-fade (the date at midnight): out in 2 steps, then in over 3 (5 frames),
 * in perceptual ink levels (9/16, 3/16 out; 3/16, 9/16, 1 in). snap = no fade (AOD,
 * build-in, a stale face). */
typedef struct fk_xfade_s { char from[16], to[16]; nos_time_t t0; int init; } fk_xfade;
void  fk_xfade_tick(fk_xfade *x, const char *now, nos_time_t t, int snap);
void  fk_xfade_k(const fk_xfade *x, nos_time_t t, float *a_from, float *a_to);
/* draws both sides as ui_label at (px, py) in col; returns the width of the new text */
int   fk_xfade_label(gfx_fb *fb, const fk_xfade *x, nos_time_t t, int px, int py, uint16_t col, int align);

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
/* gfx_taper's pixels, walking only the capsule's span per row (hands) */
void  fk_taper(gfx_fb *fb, float ax, float ay, float bx, float by, float ha, float hb, uint16_t color);
typedef void (*fk_dot_fn)(int i, const void *ctx, float *x, float *y, float *d, uint16_t *col);
void  fk_dots(gfx_fb *fb, int n, int chunk, fk_dot_fn fn, const void *ctx);

/* dot_ring replacement with accurate positions: n dots on radius r from 12 o'clock
 * clockwise, dots [0, lit) in `on`, the rest in `off` (0 = skip). 8 dots per command. */
void  fk_ring(gfx_fb *fb, float cx, float cy, float r, int n, float d, int lit,
              uint16_t on, uint16_t off);

/* ---- wake from AOD: the time stays on screen and morphs into the awake style ----
 * Call fk_wake_tick() in tick. While a wake morph runs (about 0.2 s), fk_build()
 * reports 1 so the face skips its build-in, fk_wake_k() steps 1/6 -> 1 (one step per
 * frame, counted, so the rhythm is even; 0 in AOD), and fk_wake_off() walks the AOD
 * drift (<= 4 px) home at 1 px a frame (a whole-pixel speed: text and dots move together). */
typedef struct { int last_aod; nos_time_t t0; int active; int dx, dy;
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
#ifdef GFX_COST_MODEL
void  fk_probe_common(const char *face, const fk_wake *w, const face_ctx *c);
void  fk_probe_xfade(const char *face, const char *what, const struct fk_xfade_s *x, nos_time_t t);
#else
#define fk_probe_common(face, w, c) ((void)0)
#define fk_probe_xfade(face, what, x, t) ((void)0)
#endif

/* ---- unread notifications: one dot at (233, 24) ----
 * fk_unread_tick() in tick (fades in/out in 4 perceptual ink steps); fk_unread() last in draw.
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
/* ---- lite draw (the picker, dimmed or sliding): the ghost layer (unlit cells, empty
 * track, future dots) fades with k = 1 - lite in quarter steps and is skipped below 1/2,
 * where under the dim it is near black anyway. Returns 1 (as designed) .. 0 (skip). */
static inline float fk_ghost(const face_ctx *c)
{
    float l = c->lite <= 0.0f ? 0.0f : c->lite >= 1.0f ? 1.0f : c->lite;
    float k = (float)(int)((1.0f - l) * 4.0f + 0.5f) / 4.0f;
    return k > 0.25f ? k : 0.0f;
}
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
typedef struct { fk_sglyph g[12]; int ascent, cap; int ok, next, failed; } fk_sfont;    /* '0'-'9', ':', ' ' */
int   fk_sfont_build(fk_sfont *sf, const font_t *src, float scale);
/* one glyph per call (background warm-up: no long frame); 1 once the font is complete */
int   fk_sfont_build_step(fk_sfont *sf, const font_t *src, float scale);

/* SEGMENT's background warm-up step (font, sprites, track); fk_aod_update runs it */
void  face_segment_warm(void);
int   fk_stext_width(const fk_sfont *sf, const char *s);
int   fk_stext(gfx_fb *fb, const fk_sfont *sf, int x, int y, const char *s, uint16_t col, int align);

/* ---- mechanical roll of one digit cell: `from` rolls up and out, `to` rises in, both
 * clipped to [x - 4, x + cell_w + 4) x [base - cap - 6, base + 6). Exactly one of f / sf
 * is used. k = whole frames since the roll began (0 = the change frame, which already
 * shows the first step; < 0 = not started: `from` at rest). The wheel turns in three
 * held steps on twos and is home after fk_roll_frames() frames (MOTION30 R8). ---- */
void  fk_roll(gfx_fb *fb, const font_t *f, const fk_sfont *sf, int x, int base, int cell_w,
              int cap, char from, char to, int k, uint16_t col);
/* same, with a per-glyph x nudge for the outgoing / incoming glyph (optical bearings) */
void  fk_roll2(gfx_fb *fb, const font_t *f, const fk_sfont *sf, int x, int base, int cell_w,
               int cap, char from, char to, int nx_from, int nx_to, int k, uint16_t col);
int   fk_roll_frames(void);
/* the roll's offset: the outgoing glyph is `fk_roll_dy` px up k frames in (span =
 * fk_roll_span(cap), the clip window's height); probe it from tick with fk_probe_roll */
int   fk_roll_span(int cap);
int   fk_roll_dy(int k, int span);
#ifdef GFX_COST_MODEL
void  fk_probe_roll(const char *face, int i, int k, int cap);
#else
#define fk_probe_roll(face, i, k, cap) ((void)0)
#endif

/* once-a-second blink envelope (0..1) from the millisecond clock, counted in whole
 * frames into the second: grows over 4 frames, holds to 500 ms, shrinks over 3; even
 * steps whatever the frame grid's phase against the clock */
float fk_blink(const face_ctx *c);
/* whole frames since the clock's second turned (0 = the frame it turned on), counted
 * from that frame (fk_aod_update tracks it), so per-second steps are even at any phase */
int   fk_sec_k(const face_ctx *c);

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
