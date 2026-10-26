/* ui.h â€” NOS design system: tokens, text, surfaces, controls.
 *
 * Visual rules (docs/research/NOTHING_DESIGN_REFERENCE.md, docs/NOS_DESIGN.md):
 *  - True black canvas, white ink, greys for hierarchy. Red (#D71921) is an interrupt:
 *    at most one red element per screen (the "now" dot, recording, alerts, over-limit).
 *  - Big numbers in dot-matrix (nos/dot.h). Words in a grotesk. Micro labels in ALL-CAPS
 *    mono with wide tracking. At most 3 sizes per screen.
 *  - Flat surfaces: circles, pills and big-radius cards; no shadows, blur or gradients.
 *    On = white fill with black ink; off = dark grey with white ink.
 *  - Discrete progress: segments and dots, never smooth gradient arcs.
 * Everything here records one damage command per call where it can, and draws only with
 * fills, mask blits and per-edge AA, so it stays inside the 30 fps budget. */
#ifndef NOS_UI_H
#define NOS_UI_H
#include <stdint.h>
#include "gfx.h"
#include "font.h"
#include "dot.h"
#include "motion.h"
#include "nos.h"

/* ---- colour tokens (RGB565) ---- */
#define C_BG      0x0000   /* #000000 */
#define C_SURF    0x1082   /* #111111 cards */
#define C_SURF2   0x18C3   /* #1A1A1A raised / pressed card */
#define C_SURF3   0x2945   /* #2A2A2A pressed on raised */
#define C_LINE    0x2104   /* #222222 hairlines, empty segments */
#define C_LINE2   0x3186   /* #333333 visible borders, off tracks */
#define C_DIS     0x630C   /* #636363 disabled, tertiary */
#define C_SEC     0x9CF3   /* #9C9C9C secondary text */
#define C_PRI     0xEF7D   /* #EEEEEE primary text (neutral: 0xEF5D read pink) */
#define C_WHITE   0xFFFF
/* The accents are variables: the light theme inverts the whole panel in hardware (DCS
 * INVON, no pixel work), so while it is on they hold their own inverse and still show
 * as themselves. Greys need nothing: inverted, they are the light theme. */
extern uint16_t nos_c_red, nos_c_red_d, nos_c_green, nos_c_amber;
#define C_RED     nos_c_red      /* #D71921 Nothing red (0xD0C4) */
#define C_RED_D   nos_c_red_d    /* #B01319 pressed red (0xB083) */
#define C_GREEN   nos_c_green    /* #4A9E5C success, sparingly (0x4CEB) */
#define C_AMBER   nos_c_amber    /* #D4A843 warning, sparingly (0xD548) */
/* 1 while the panel is inverted (the host's call, on every change; it also invalidates). */
void ui_theme_inverted(int on);
#define C_GHOST   0x18C3   /* unlit dot-matrix cells (neutral #181818) */

/* ---- geometry ---- */
#define UI_CX 233
#define UI_CY 233
#define UI_R  233
/* Half-width of the display at row y (0 outside the circle). */
int   ui_half_width(float y);
/* Inset x range usable at row y with a margin from the bezel. */
void  ui_row_span(float y, float margin, int *x0, int *x1);

/* ---- layout standards (docs/NOS_DESIGN.md §8) — use these, not local numbers ---- */
#define UI_TITLE_Y     70     /* ui_title baseline */
#define UI_BANNER_TOP  52     /* banner card top when shown */
#define UI_BTN_H       88     /* every action button: height 88, radius 44 */
#define UI_BTN_W       200    /* single action */
#define UI_BTN_PAIR_W  140    /* two actions side by side at UI_BTN_PAIR_X0 / X1 */
#define UI_BTN_PAIR_X0 154
#define UI_BTN_PAIR_X1 312
#define UI_BTN_Y       372    /* centre of the thumb-zone action row */
#define UI_DOTS_Y      440    /* page dots centre */
#define UI_HERO_PITCH  9.0f   /* the one big dot-matrix number, <= 4 glyphs */
#define UI_HEROW_PITCH 8.0f   /* hero with 5-8 glyphs (times, 914.6, +8:45) */
#define UI_LIST_PITCH  4.4f   /* numbers in rows and tiles */
#define UI_SUB_PITCH   3.0f   /* secondary numbers (5x7 font) */
#define UI_ENTRY_S     0.30f  /* entry build duration; iris entries start at 40% of the iris */
#define UI_ROW_H       90     /* list rows; section headers 60 */
#define UI_ROW_X0      62     /* list row content inside |y-233| < 150 */
#define UI_ROW_X1      404
#define UI_VALUE_X     384    /* right edge of row values / chevrons */

/* ---- type ---- */
extern const font_t *UI_F_HERO;     /* grotesk digits, full-screen clock */
extern const font_t *UI_F_DISPLAY;  /* grotesk ~64 px */
extern const font_t *UI_F_TITLE;    /* grotesk ~30 px */
extern const font_t *UI_F_BODY;     /* grotesk ~24 px */
extern const font_t *UI_F_LABEL;    /* mono ~17 px, caps + tracking */
extern const font_t *UI_F_SMALL;    /* mono ~14 px */

enum { UI_LEFT = 0, UI_CENTER = 1, UI_RIGHT = 2 };
/* Text at baseline y. tracking = extra px between glyphs. Returns width. One damage cmd. */
int  ui_text(gfx_fb *fb, const font_t *f, int x, int y, const char *s, uint16_t color,
             int align, int tracking);
int  ui_text_width(const font_t *f, const char *s, int tracking);
/* Circular baseline, clockwise from noon, glyph tops outward all round.
 * Prepare only in tick/layout; begin replaces ALL live runs (single owner).
 * A dense internal index pins exact 2-degree PSRAM masks until the next begin.
 * Motion never builds masks: misses draw from the upright atlas with fixed-point
 * nearest mapping. At rest, at most 24 exact masks build per begin. While
 * pending(), capture again next tick and include run hashes in retained damage.
 * Allocation failure uses direct preview without an idle retry loop. Invalid
 * inputs or bounds above 32x32 return 0 and an empty run. Byte glyph codes.
 * Drop succeeds only without pins; draw never allocates or calls libm. */
#define UI_ARC_GLYPHS 16
typedef struct {
    int16_t x, y;
    uint16_t slot, glyph;
    uint8_t angle, w, h;
    int8_t left, top;
} ui_arc_glyph;
typedef struct {
    ui_arc_glyph glyph[UI_ARC_GLYPHS];
    const font_t *font;
    int16_t x0, y0, x1, y1;
    uint16_t n;
    unsigned hash, epoch;
} ui_arc_text;
void ui_text_arc_begin(void);
void ui_text_arc_motion(int on);
int ui_text_arc_retain(ui_arc_text *run);
int ui_text_arc_pending(void);
int ui_text_arc_prepare(ui_arc_text *run, const font_t *f, float cx, float cy,
                        float radius, float angle, const char *s, int align, int tracking);
void ui_text_arc(gfx_fb *fb, const ui_arc_text *run, uint16_t color);
int ui_text_arc_drop(void);
void ui_text_arc_stats(unsigned *bytes, unsigned *builds, unsigned *hits);

/* ALL-CAPS mono label with the house tracking (converts to upper case). */
int  ui_label(gfx_fb *fb, int x, int y, const char *s, uint16_t color, int align);
int  ui_label_width(const char *s);       /* width of ui_label(s) incl. tracking */
/* Text clipped to max_w with a trailing "..." if needed. */
int  ui_text_fit(gfx_fb *fb, const font_t *f, int x, int y, const char *s, uint16_t color,
                 int align, int max_w);
/* Word-wrap into at most max_lines lines of max_w; returns lines drawn. */
int  ui_text_wrap(gfx_fb *fb, const font_t *f, int x, int y, int max_w, int line_h,
                  int max_lines, const char *s, uint16_t color, int align);
int  ui_ascent(const font_t *f);

/* ---- surfaces ---- */
void ui_rrect(gfx_fb *fb, float x, float y, float w, float h, float r, uint16_t color);
void ui_rrect_line(gfx_fb *fb, float x, float y, float w, float h, float r, float t, uint16_t color);
void ui_circle(gfx_fb *fb, float cx, float cy, float r, uint16_t color);
void ui_circle_line(gfx_fb *fb, float cx, float cy, float r, float t, uint16_t color);
uint16_t ui_mix(uint16_t a, uint16_t b, float t);   /* colour lerp, computed once per call */
uint16_t ui_dim(uint16_t c, float k);               /* k 0..1 brightness */

/* ---- screen chrome ---- */
/* Title at the top of an app: ALL-CAPS mono label centred at y = 64. `scroll` fades and
 * lifts it as content scrolls under (pass 0 for static screens). */
void ui_title(gfx_fb *fb, const char *s, float scroll);
/* Small time readout under the bezel top (for app screens), e.g. "10:42". */
void ui_clock_small(gfx_fb *fb, int y);
/* Scroll position indicator: a short arc hugging the right bezel. alpha 0..1. */
void ui_scrollbar(gfx_fb *fb, float offset, float content, float view, float alpha);
/* Page dots centred at (cx, y); pos is fractional (animates between pages). */
void ui_page_dots(gfx_fb *fb, int cx, int y, int n, float pos);
void ui_page_dots_a(gfx_fb *fb, int cx, int y, int n, float pos, float alpha);  /* alpha 0..1, quantise it */

/* ---- controls ---- */
/* Toggle centred at (cx, cy); on = 0..1 animated (use an mspring). */
void ui_toggle(gfx_fb *fb, float cx, float cy, float on);
enum { BTN_PRIMARY = 0, BTN_SECONDARY, BTN_DANGER, BTN_GHOST, BTN_RED };
void ui_button(gfx_fb *fb, float cx, float cy, float w, float h, const char *label,
               int kind, int pressed);
/* Circular icon button (quick settings tile, media controls). on: white fill. */
void ui_icon_button(gfx_fb *fb, float cx, float cy, float r, int icon, int on, int pressed);
/* Horizontal segmented bar: n segments, value 0..n (fractional fills the last one
 * partially as a narrower segment). Segments beyond `limit` (if > 0) draw red. */
void ui_segments(gfx_fb *fb, float x, float y, float w, float h, int n, float value,
                 uint16_t on, uint16_t off);
/* Segmented ring of n ticks (radial dashes) around (cx, cy) between r0..r1, lit
 * fraction 0..1 (more than 1 continues in red as overflow). */
void ui_tick_ring(gfx_fb *fb, float cx, float cy, float r0, float r1, int n, float w,
                  float lit, uint16_t on, uint16_t off);
/* Thick pill slider (brightness/volume): value 0..1, with an icon at the left end. */
void ui_slider(gfx_fb *fb, float x, float y, float w, float h, float value, int icon);
/* the same, with the finger on it: the track lightens a step (C_LINE2) */
void ui_slider_p(gfx_fb *fb, float x, float y, float w, float h, float value, int icon, int pressed);
/* Value block: big dot-matrix number with a mono caps unit label beneath. */
void ui_value(gfx_fb *fb, float cx, float y, const char *value, const char *unit,
              float pitch, uint16_t color);
/* Module-absent / phone-absent / empty standard screen (keep the screen's ui_title):
 * dot icon C_DIS at y 170, `head` in UI_F_TITLE at baseline 270 ("Not connected",
 * "Phone not connected", "No alarms"), up to 2 body lines in C_SEC (what it adds /
 * what to do), and an optional caps footer in C_DIS at y 398 (e.g. "COMPASS MODULE").
 * `p` = entry build progress 0..1 (icon builds, text follows). */
void ui_absent(gfx_fb *fb, int icon, const char *head, const char *body, const char *footer, float p);
/* Short module name for "NO <NAME>" notes and toasts (§8): "HAPTICS", "HR SENSOR"...
 * `ext` is one EXT_* bit (hal_ext.h). */
const char *ui_module_short(uint32_t ext);
/* Caps label that keeps unit case: ui_label_unit(fb, x, y, "4.00", "V") draws "4.00 V";
 * units like mV, dB, hPa, uT keep their case. */
int  ui_label_unit(gfx_fb *fb, int x, int y, const char *value, const char *unit, uint16_t color, int align);
/* Placeholder for a missing value: always "--". */
#define UI_NONE "--"
/* "[ LOADING ]"-style bracketed state text (Nothing shows states as text). */
void ui_state(gfx_fb *fb, int cx, int y, const char *s, uint16_t color);

/* ---- list ----
 * A vertical list with momentum scrolling, rubber-banding and a round-screen layout:
 * each row is inset to the circle and rows dim as they approach the top/bottom bezel. */
typedef struct ui_list ui_list;
typedef void (*ui_row_fn)(gfx_fb *fb, int i, int x0, int y, int x1, int h, float fade, int pressed);
struct ui_list {
    int n;
    int row_h;          /* px per row */
    int top;            /* y of the first row when scrolled to 0 (header space) */
    int bottom_pad;
    mscroll sc;
    int pressed;        /* row under the finger, -1 none */
    nos_time_t press_t;
    int dragging;
    float bar_alpha;    /* scrollbar fade */
    nos_time_t bar_t;
    ui_row_fn draw_row;
    void (*on_tap)(int i);
    int  margin;        /* bezel margin for row spans */
    int  snap;          /* rows snap to row_h after a fling */
    float fb_prev;      /* last frame's drawn offset (fling blur) */
};
void ui_list_init(ui_list *l, int n, int row_h, int top, ui_row_fn draw_row, void (*on_tap)(int));
void ui_list_set_n(ui_list *l, int n);
int  ui_list_input(ui_list *l, const nos_input *in);   /* returns 1 if consumed */
void ui_list_tick(ui_list *l, float dt);
void ui_list_draw(gfx_fb *fb, ui_list *l);
float ui_list_offset(const ui_list *l);
int  ui_list_at_top(const ui_list *l);

/* ---- retained RGB565 directional motion cache ----
 * Items survive owner changes in a bounded 2.6 MiB LRU pool. The cache stores
 * full-color sharp content plus published/staging directional blur per item.
 * A completed blur stays visible while a replacement shutter is generated.
 * Work is sliced; visible items precede the next viewport. Blur includes drags.
 * Level 0 can reuse the sharp cache. Invalidate synchronously when content changes;
 * legacy callbacks also receive visible command-signature validation.
 * bands=1 includes 24 context rows per side; bands=2 is a horizontal page.
 * Cache misses return 0 for live rendering; callers must retain that fallback. */
typedef void (*ui_blur_live_fn)(gfx_fb *fb,void *ctx);
typedef void (*ui_blur_row_fn)(gfx_fb *fb, int i, int x0, int y, int x1, int h, void *ctx);
/* All step arguments below are signed pixels per nominal NOS_FPS frame:
 * offset_delta / (elapsed_seconds * M_FPS), including skipped tick intervals.
 * These APIs recover physical speed with fabs(step) * M_FPS; do not convert
 * callers to reference-30Hz steps or normalize a second time with nos_dt.
 * Public levels 0..3 cross at 360/840/1680 px/s; early levels 0..4 cross at
 * 150/300/960/1920 px/s, independently of the configured cadence. */
int  ui_fblur_level(float step_px);
#define UI_FBLUR_EARLY 0x10
#define UI_FBLUR_PREWARM 0x20
int ui_fblur_level_early(float step_px);
void ui_fblur_idle(void); /* reclaim generic pixels on settled specialized-cache screens */
void ui_fblur_release(void); /* UI-thread safe trim; later use can allocate again */
void ui_fblur_mem(void);
/* Read immediately after this owner's tick. This is its selected shutter,
 * including held/disabled policy, not publication readiness. Generic callers
 * must not gate it with the specialized 0..3 image ladder above. */
int  ui_fblur_active(const void *id);
int  ui_fblur_any_active(void); /* any cache owner requested directional blur this frame */
/* Actual raster coverage in screen coordinates, clipped to completed stripes.
 * Save/restore enable state around offscreen layer generation. Spans are merged,
 * sorted [x0,x1); omitted spans on capacity overflow conservatively blur again. */
int ui_fblur_coverage_enable(int enabled); /* returns previous state */
int ui_fblur_row_spans(int y,int spans[][2],int capacity);
/* Same conservative spans through exclusive until; no claims change while applying. */
int ui_fblur_row_spans_run(int y,int spans[][2],int capacity,int *until);
/* Revoke directional claims under sharp foreground chrome. Logical half-open
 * bounds are clipped to this raster stripe; offscreen/record passes do nothing. */
void ui_fblur_coverage_claim(const gfx_fb *fb,int x0,int y0,int x1,int y1);
void ui_fblur_coverage_exclude(const gfx_fb *fb,int x0,int y0,int x1,int y1);
/* Generic retained shutters are 2/8/16/40 px at 15/150/720/1800 px/s.
 * UI_FBLUR_EARLY selects the early ladder above and its existing +1 level
 * promotion (capped at 4). Held/disabled blur policy is applied after selection. */
void ui_fblur_tick(const void *id, int held, float step, int i0, int i1, int n, int row_h,
                   int x0, int x1, int bands, ui_blur_row_fn fn, void *ctx);
int  ui_fblur_ready(const void *id, int i0, int i1);
typedef struct { unsigned bytes,hits,misses,evictions,work_pixels,allocation_failures; } ui_blur_stats;
void ui_fblur_stats(ui_blur_stats *out);
void ui_fblur_invalidate(const void *id,int first,int last);
/* Whole pages use bands=2 and their translated left coordinate when drawing. */
int  ui_fblur_draw_at(gfx_fb *fb, const void *id, int i, int x, int y, float fade);
int  ui_fblur_draw(gfx_fb *fb, const void *id, int i, int y, float fade);
/* Mixed bands: published items blit; each cold band clips the original live body
 * callback to its exact pixels. top is band0's actual logical y. Headers/bars
 * belong after this call. Returns count of cached band draws in this stripe. */
int ui_fblur_draw_bands(gfx_fb *fb,const void *id,int first,int last,int top,int row_h,
                       float (*fade)(float),ui_blur_live_fn live,void *ctx);

/* ---- smear for fast things the eye does not track (docs/MOTION30.html R7) ----
 * A small object stepping more than ~16 px a frame (a seconds dot, a spinner, a burst
 * dot, the jump frame of a cut) strobes into separate copies at 30 fps. Give it a ghost
 * at the previous frame's position (45 % ink, an optional second at 20 %; ghosts last
 * <= 2 frames) or stretch it along its velocity by half the step (film's 180 deg shutter).
 * Cost: one extra primitive in dimmed ink, no per-pixel pass. Never smear a drag or
 * anything the eye follows (it adds to the AMOLED hold blur).
 *   tick():  ui_trail_push(&S.trail, x, y);          // every frame, also when still
 *   draw():  ui_smear_disc(fb, &S.trail, r, C_PRI, 16, 1); gfx_fill_circle(fb, x, y, r, C_PRI);
 * For hands, sprites or icons keep the angle/position in a trail and draw the thing a
 * second time through `gfx_fb g = *fb; nos_ink_dim(&g, UI_GHOST1);` while
 * ui_trail_step(&t, 0) is over 16 px. */
typedef struct { float x[3], y[3]; int n; } ui_trail;   /* [0] this frame, [1] the last, [2] before */
#define UI_GHOST1 0.45f      /* first ghost's ink */
#define UI_GHOST2 0.20f      /* second ghost's ink (never fainter: AMOLED smears near-black) */
void  ui_trail_push(ui_trail *t, float x, float y);     /* tick(): this frame's position */
void  ui_trail_reset(ui_trail *t, float x, float y);    /* a jump that must not smear */
float ui_trail_step(const ui_trail *t, int i);          /* px from position i+1 to i (0 if unknown) */
/* Ghost discs at the previous 1-2 positions where the step that left them exceeds
 * min_step px (16 = the tracking budget), drawn before the disc itself. ghosts = 1 or 2. */
void  ui_smear_disc(gfx_fb *fb, const ui_trail *t, float r, uint16_t color, float min_step, int ghosts);
/* Velocity stretch: a capsule from half a step behind to the disc (tapering to 70 %),
 * in 45 % ink, when the last step exceeds min_step; draw the disc after it. */
void  ui_smear_stretch(gfx_fb *fb, const ui_trail *t, float r, uint16_t color, float min_step);

/* Ripple motion blur: the find screens' dot ripples drag tails of dimmer dots, the
 * Matrix face's build-in wave gets a soft front. 0 off, 1 tails (default), 2 tails and
 * burst (rings launch fast and glide out, the tail shrinking with their speed). The sim
 * sets it with `set ripple_blur N` and the M key. */
extern int ui_ripple_blur;

/* ---- engine overlays (called by nos.c) ---- */
/* y: the card's offset from its resting place (whole px). */
void ui_draw_banner(gfx_fb *fb, const char *title, const char *body, int icon, float y, int pressed);
/* off: px above its resting place (0 = shown; ui_toast_travel(text) = fully hidden). */
void ui_draw_toast(gfx_fb *fb, const char *text, float off);
float ui_toast_travel(const char *text);

/* ---- formatting helpers ---- */
void ui_fmt_time(char *out, int n, int h, int m);           /* honours 24 h pref */
void ui_fmt_dur(char *out, int n, uint32_t ms, int tenths); /* 1:02:03 / 02:03.4 */
const char *ui_weekday3(int wd);                            /* "MON" */
const char *ui_month3(int m);                               /* "JAN" (1-12) */
#endif
