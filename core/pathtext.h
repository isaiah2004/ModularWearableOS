/* pathtext.h — high-quality text, straight or laid along a circular arc, using
 * stb_truetype (public domain) to rasterize glyphs from an embedded vector font
 * (static Saira Bold / Saira Condensed SemiBold). Glyphs are rendered at 3x oversample and sampled
 * with 2x2 supersampling when rotated, so arc text stays smooth at any angle —
 * unlike rotating a pre-baked low-res bitmap.
 *
 * Angle convention matches gfx: 0 = 12 o'clock, clockwise, radians. On an arc
 * each glyph is laid tangent with its baseline on radius R and its top pointing
 * radially outward (bottom-of-dial numerals read inverted — the dive-bezel look).
 *
 * Rasterization is not cheap; use it into a CACHED layer (static dial furniture)
 * rather than on a per-frame hot path. No-op in record mode. */
#ifndef WATCH_PATHTEXT_H
#define WATCH_PATHTEXT_H

#include "gfx.h"

typedef enum {
    PATHTEXT_BOLD,
    PATHTEXT_CONDENSED,
    PATHTEXT_FONT_COUNT
} pathtext_font;

/* Text along an arc, centred angularly at a_mid, baseline on radius R (px).
 * size_px is the actual capital height, independent of font vertical metrics.
 * Text APIs support printable ASCII (the embedded fonts are subsetted). */
void pathtext_arc(gfx_fb *fb, pathtext_font font, const char *s, float R, float a_mid,
                  float size_px, uint16_t col);

/* Reverse-direction arc, with glyph tops toward the hub. Lower complications
 * read upright from left to right instead of adopting the inverted bezel style. */
void pathtext_arc_inward(gfx_fb *fb, pathtext_font font, const char *s, float R,
                         float a_mid, float size_px, uint16_t col);

/* Straight text. y is the baseline. center!=0 centres the string on x, else x
 * is the left pen origin. Returns the pen x after the last glyph. */
int  pathtext_draw(gfx_fb *fb, pathtext_font font, const char *s, int x, int y,
                   float size_px, uint16_t col, int center);

/* Pixel width of a straight string at size_px. */
float pathtext_width(pathtext_font font, const char *s, float size_px);

#endif
