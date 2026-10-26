/* flat60.h — the Flat60 interface style (docs/FLAT60_DESIGN.md).
 *
 * Palette: every colour is an exact RGB332 level (R,G in {0,36,73,109,146,182,
 * 219,255}, B in {0,85,170,255}), so the 8-bit fast-display wire format shows it
 * unchanged and the bus budget doubles. Only anti-aliased edges take shades in
 * between. */
#ifndef WATCH_FLAT60_H
#define WATCH_FLAT60_H

#include "gfx.h"

#define F60_BLACK    GFX_RGB(0, 0, 0)
#define F60_WHITE    GFX_RGB(255, 255, 255)
#define F60_TEXT2    GFX_RGB(182, 182, 170)   /* secondary text */
#define F60_SURFACE  GFX_RGB(36, 36, 85)      /* chip / card fill on black */
#define F60_SURFACE2 GFX_RGB(73, 73, 85)      /* pressed / raised */

#define F60_RED      GFX_RGB(255, 36, 0)
#define F60_ORANGE   GFX_RGB(255, 146, 0)
#define F60_YELLOW   GFX_RGB(255, 219, 0)
#define F60_GREEN    GFX_RGB(0, 219, 85)
#define F60_TEAL     GFX_RGB(0, 182, 170)
#define F60_BLUE     GFX_RGB(36, 109, 255)
#define F60_PURPLE   GFX_RGB(146, 73, 255)
#define F60_PINK     GFX_RGB(255, 73, 170)

/* Nearest Flat60 accent to an arbitrary RGB565 colour (app accents predate the
 * palette); greys map to F60_TEXT2. */
uint16_t f60_accent(uint16_t c565);

/* 0 = classic interface, 1 = Flat60. Persisted; ui_style_apply() installs the
 * matching overlays (call after registering the classic ones, and on change). */
extern int wos_ui_style;
void ui_style_apply(void);

#endif
