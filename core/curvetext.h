/* curvetext.h — text laid along a circular arc (rotated per-glyph), for bezel
 * numerals and labels that follow the dial curve. Extracted from the proven
 * curved-text renderer in app_music.c (bilinear-sampled rotated glyphs).
 *
 * Angle convention matches gfx: 0 = 12 o'clock, clockwise, radians. Each glyph
 * is laid tangent to the arc with its baseline on radius R and its top pointing
 * radially outward — so numerals at the bottom read inverted, the classic
 * dive-bezel look. */
#ifndef WATCH_CURVETEXT_H
#define WATCH_CURVETEXT_H

#include "gfx.h"
#include "font.h"

/* Draw `s` centred angularly at a_mid, along radius R (grid-scaled px already).
 * No-op in record mode (intended for cached layers / static raster passes). */
void curve_text(gfx_fb *fb, const font_t *f, const char *s, float R,
                float a_mid, uint16_t col);

/* Angular span (radians) the string would occupy at radius R. */
float curve_text_span(const font_t *f, const char *s, float R);

#endif
