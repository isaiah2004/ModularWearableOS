/* dot.h — dot-matrix type, icons and glyph-ring primitives (the Nothing signature).
 *
 * Glyphs and icons are bitmaps on a cell grid; every lit cell is drawn as a round,
 * anti-aliased dot stamped from a cached coverage mask, so a dot costs a small mask
 * blit, never per-pixel distance maths. Each call records ONE damage command
 * (bbox + hash of text, style and animation progress), so a still label costs
 * nothing per frame and a changing digit repaints only its own box.
 *
 * Background assumption: NOS draws on true black or on a flat surface colour. The
 * stamp blends dot edges against whatever is already in the buffer, so dots on cards
 * are fine. */
#ifndef NOS_DOT_H
#define NOS_DOT_H
#include <stdint.h>
#include "gfx.h"

enum {
    DOT_FONT_5X7 = 0,   /* labels, widget text; full ASCII 32-126 */
    DOT_FONT_TALL,      /* 5x9 display face for numerals, time, big values; digits, : . - % / and A-Z */
    DOT_FONT_3X5,       /* tiny status text; digits, A-Z, a few symbols */
    DOT_FONT_COUNT
};

typedef struct {
    uint8_t  font;      /* DOT_FONT_* */
    float    pitch;     /* px between dot centres */
    float    dot;       /* dot diameter, px (pitch * 0.78 is the house ratio) */
    uint16_t color;     /* lit dots */
    uint16_t ghost;     /* unlit cells inside each glyph's box; 0 = not drawn */
    uint8_t  gap;       /* empty cells between glyphs (default 1) */
    uint8_t  mono;      /* 1 = every glyph advances the same width (tabular digits) */
} dot_style;

/* Reveal / build animations, `p` 0..1 (1 = fully shown). Dots grow from nothing and
 * reach full size along the chosen order. */
enum {
    DOT_REVEAL_NONE = 0,
    DOT_REVEAL_SCAN,      /* column sweep left -> right, like a matrix printer */
    DOT_REVEAL_RANDOM,    /* each dot at its own hashed moment (the Nothing "sparkle") */
    DOT_REVEAL_RAIN,      /* row by row from the top */
    DOT_REVEAL_CENTER,    /* outward from the text's centre */
};

/* Text with its top-left at (x, y). Returns the advance width in px. */
int  dot_text(gfx_fb *fb, const dot_style *st, float x, float y, const char *s);
int  dot_text_width(const dot_style *st, const char *s);
int  dot_text_height(const dot_style *st);
/* align: 0 left, 1 centre, 2 right, around x */
int  dot_text_a(gfx_fb *fb, const dot_style *st, float x, float y, const char *s, int align);
/* Animated: p = reveal progress (mode DOT_REVEAL_*). seed varies the RANDOM order. */
int  dot_text_anim(gfx_fb *fb, const dot_style *st, float x, float y, const char *s,
                   int align, float p, int mode, unsigned seed);
/* Morph between two strings of equal length cell by cell (a digit rolling over):
 * dots only in `from` shrink while dots only in `to` grow, shared dots stay. p 0..1. */
int  dot_text_morph(gfx_fb *fb, const dot_style *st, float x, float y,
                    const char *from, const char *to, int align, float p);

/* A single anti-aliased dot. */
void dot_disc(gfx_fb *fb, float cx, float cy, float d, uint16_t color);

/* Icons on an 11x11 cell grid, drawn centred at (cx, cy). */
enum {
    DOT_ICON_NONE = 0,
    DOT_ICON_BELL, DOT_ICON_HEART, DOT_ICON_STEPS, DOT_ICON_FLAME, DOT_ICON_SUN, DOT_ICON_MOON,
    DOT_ICON_CLOUD, DOT_ICON_RAIN, DOT_ICON_SNOW, DOT_ICON_WIND, DOT_ICON_THERMO, DOT_ICON_DROP,
    DOT_ICON_COMPASS, DOT_ICON_PIN, DOT_ICON_ROUTE, DOT_ICON_SAT, DOT_ICON_SIGNAL, DOT_ICON_MSG,
    DOT_ICON_MUSIC, DOT_ICON_PLAY, DOT_ICON_PAUSE, DOT_ICON_NEXT, DOT_ICON_PREV,
    DOT_ICON_TIMER, DOT_ICON_STOPWATCH, DOT_ICON_ALARM, DOT_ICON_MIC, DOT_ICON_SPEAKER,
    DOT_ICON_TORCH, DOT_ICON_SETTINGS, DOT_ICON_BATTERY, DOT_ICON_BOLT, DOT_ICON_BT, DOT_ICON_WIFI,
    DOT_ICON_PHONE, DOT_ICON_FIND, DOT_ICON_DND, DOT_ICON_BRIGHT, DOT_ICON_LOCK, DOT_ICON_CHECK,
    DOT_ICON_CROSS, DOT_ICON_PLUS, DOT_ICON_MINUS, DOT_ICON_ARROW_UP, DOT_ICON_ARROW_DOWN,
    DOT_ICON_ARROW_LEFT, DOT_ICON_ARROW_RIGHT, DOT_ICON_GAUGE, DOT_ICON_MOUNTAIN, DOT_ICON_LEAF,
    DOT_ICON_BREATHE, DOT_ICON_CALENDAR, DOT_ICON_GRID, DOT_ICON_SD, DOT_ICON_INFO, DOT_ICON_LEVEL,
    DOT_ICON_HAND, DOT_ICON_EYE, DOT_ICON_GLOBE, DOT_ICON_CAMERA, DOT_ICON_FACE, DOT_ICON_SMILE,
    DOT_ICON_COUNT
};
void dot_icon(gfx_fb *fb, int icon, float cx, float cy, float pitch, float dot, uint16_t color);
/* Same, with a build-in animation (p 0..1, DOT_REVEAL_*). */
void dot_icon_anim(gfx_fb *fb, int icon, float cx, float cy, float pitch, float dot,
                   uint16_t color, float p, int mode);

/* Glyph ring: n dots evenly around a circle of radius r (0 rad = 12 o'clock, clockwise).
 * Dots whose index lies in [lit0, lit1) use `on`, others `off` (0 = skip). Fractional
 * ends render a partially grown dot, so progress moves smoothly between dots. */
void dot_ring(gfx_fb *fb, float cx, float cy, float r, int n, float d,
              float lit0, float lit1, uint16_t on, uint16_t off);
/* Straight row of n dots from (x, y) with pitch; `lit` may be fractional. */
void dot_bar(gfx_fb *fb, float x, float y, int n, float pitch, float d, float lit,
             uint16_t on, uint16_t off);
/* A dot-grid area fill (rows x cols cells), values[] 0-255 per column = height in
 * cells * 255 / rows (bar charts, waveforms, equaliser). */
void dot_columns(gfx_fb *fb, float x, float y, int cols, int rows, float pitch, float d,
                 const uint8_t *values, uint16_t on, uint16_t off);
#endif
