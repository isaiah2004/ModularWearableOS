/* icons.h — procedural (vector) app icons drawn from gfx primitives, so they
 * scale cleanly to any size. Used by the launcher, curved list, and control
 * center. icon_badge() is the standard rounded app tile. */
#ifndef WATCH_ICONS_H
#define WATCH_ICONS_H

#include "gfx.h"

/* Design language: Lucide (https://lucide.dev, ISC License) — a monochrome,
 * even-stroke line set that maps cleanly onto our line/arc/circle primitives.
 * Every glyph is centered in radius r and drawn at the shared stroke weight w. */
enum {
    ICON_NONE = 0,
    ICON_CLOCK, ICON_STOPWATCH, ICON_TIMER, ICON_WEATHER, ICON_ACTIVITY,
    ICON_SETTINGS, ICON_MUSIC, ICON_WIFI, ICON_FLASH, ICON_BELL,
    ICON_PLUS, ICON_MINUS,
    /* added for full app coverage (see icons.c) */
    ICON_ALARM, ICON_GLOBE, ICON_CALENDAR, ICON_MIC, ICON_WAVEFORM,
    ICON_PHONE, ICON_BLUETOOTH, ICON_SIGNAL, ICON_GAUGE, ICON_LEVEL,
    ICON_CALC, ICON_GRID, ICON_BATTERY, ICON_FLASHLIGHT, ICON_BREATHE,
    ICON_SYNC,   /* clock ringed by a circular retry arrow (re-sync time) */
    ICON_TRASH,  /* trash can (clear/delete) */
};

/* draw a glyph centered at (cx,cy), fitting within radius r, in `col` */
void icon_glyph(gfx_fb *fb, int id, float cx, float cy, float r, uint16_t col);

/* app tile: filled accent disc + white glyph (or initial letter fallback) */
void icon_badge(gfx_fb *fb, float cx, float cy, float r,
                uint16_t accent, int icon_id, const char *label);

#endif
