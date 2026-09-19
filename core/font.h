/* font.h — baked-atlas font rendering. Atlases are generated offline by
 * tools/bake_font.py from system TTFs into 8-bit alpha bitmaps in flash. */
#ifndef WATCH_FONT_H
#define WATCH_FONT_H

#include "gfx.h"

typedef struct {
    uint16_t code;    /* unicode codepoint (ASCII range for now) */
    uint8_t  w, h;    /* bitmap size */
    int8_t   xoff, yoff; /* bitmap offset from pen position (yoff from baseline) */
    uint8_t  xadv;    /* pen advance */
    uint32_t bitmap_off; /* byte offset into font->bitmap */
} font_glyph;

typedef struct {
    const font_glyph *glyphs; /* sorted by code */
    int nglyphs;
    const uint8_t *bitmap;    /* 8-bit alpha, glyph rows packed */
    uint8_t line_height;
    uint8_t ascent;
} font_t;

/* Draw with pen at (x, baseline_y). Returns final pen x. */
int  font_draw(gfx_fb *fb, const font_t *f, int x, int y, const char *text, uint16_t color);
int  font_text_width(const font_t *f, const char *text);
/* Convenience: horizontally centered around cx. */
void font_draw_centered(gfx_fb *fb, const font_t *f, int cx, int y, const char *text, uint16_t color);

/* Baked fonts (see core/fonts/) */
extern const font_t font_big;    /* large tabular digits + ':' for time */
extern const font_t font_large;  /* large alphanumeric (focused list item) */
extern const font_t font_medium; /* medium UI text */
extern const font_t font_small;  /* labels */
extern const font_t font_clock;  /* Infograph hero digits (~15% > big) */
extern const font_t font_dow;    /* Infograph day-of-week (uppercase + digits) */
extern const font_t font_date;   /* Infograph dd/MMM (uppercase + digits) */

#endif
