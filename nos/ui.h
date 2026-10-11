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
#define C_RED     0xD0C4   /* #D71921 Nothing red */
#define C_RED_D   0xB083   /* #B01319 pressed red */
#define C_GREEN   0x4CEB   /* #4A9E5C success (sparingly) */
#define C_AMBER   0xD548   /* #D4A843 warning (sparingly) */
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
/* Value block: big dot-matrix number with a mono caps unit label beneath. */
void ui_value(gfx_fb *fb, float cx, float y, const char *value, const char *unit,
              float pitch, uint16_t color);
/* Module-absent / phone-absent / empty standard screen (keep the screen's ui_title):
 * dot icon C_DIS at y 170, `head` in UI_F_TITLE at baseline 270 ("Not connected",
 * "Phone not connected", "No alarms"), up to 2 body lines in C_SEC (what it adds /
 * what to do), and an optional caps footer in C_DIS at y 398 (e.g. "COMPASS MODULE").
 * `p` = entry build progress 0..1 (icon builds, text follows). */
void ui_absent(gfx_fb *fb, int icon, const char *head, const char *body, const char *footer, float p);
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
    float press_t;
    int dragging;
    float bar_alpha;    /* scrollbar fade */
    float bar_t;
    ui_row_fn draw_row;
    void (*on_tap)(int i);
    int  margin;        /* bezel margin for row spans */
    int  snap;          /* rows snap to row_h after a fling */
};
void ui_list_init(ui_list *l, int n, int row_h, int top, ui_row_fn draw_row, void (*on_tap)(int));
void ui_list_set_n(ui_list *l, int n);
int  ui_list_input(ui_list *l, const nos_input *in);   /* returns 1 if consumed */
void ui_list_tick(ui_list *l, float dt);
void ui_list_draw(gfx_fb *fb, ui_list *l);
float ui_list_offset(const ui_list *l);
int  ui_list_at_top(const ui_list *l);

/* ---- engine overlays (called by nos.c) ---- */
void ui_draw_banner(gfx_fb *fb, const char *title, const char *body, int icon, float y, int pressed);
void ui_draw_toast(gfx_fb *fb, const char *text, float t01);

/* ---- formatting helpers ---- */
void ui_fmt_time(char *out, int n, int h, int m);           /* honours 24 h pref */
void ui_fmt_dur(char *out, int n, uint32_t ms, int tenths); /* 1:02:03 / 02:03.4 */
const char *ui_weekday3(int wd);                            /* "MON" */
const char *ui_month3(int m);                               /* "JAN" (1-12) */
#endif
