/* sprite.h — pre-rendered RGB565 images in PSRAM, for Flat60's "draw once, blit
 * many" rule: anything anti-aliased (shapes, text, icons) is rendered into a
 * sprite once and moved afterwards as a plain row copy (~33 ns/px on the watch
 * vs 150-700 ns/px to re-rasterize). */
#ifndef WATCH_SPRITE_H
#define WATCH_SPRITE_H

#include "gfx.h"

typedef struct {
    uint16_t *px;      /* w*h pixels, row-major, PSRAM */
    int w, h;
} sprite;

/* Allocate a w x h sprite (PSRAM). Returns 0 on failure. Sprites are never freed:
 * owners allocate a fixed set once and re-render into them. */
int sprite_alloc(sprite *s, int w, int h);

/* A gfx_fb that draws into the sprite: logical (0,0) is the sprite's top-left and
 * every primitive clips to it. Clear it first; it starts as whatever was there. */
gfx_fb sprite_fb(const sprite *s);

/* Copy the sprite to logical (x, y). Opaque: its background pixels are copied
 * too, so sprites must not overlap other content (Flat60 lays them on black).
 * In record mode this records one damage command keyed by `key` and position;
 * change `key` whenever the sprite's pixels change. */
void sprite_blit(gfx_fb *fb, const sprite *s, int x, int y, unsigned key);

/* 8-bit sprites: RGB332 indices blitted through a 256-entry RGB565 colour table.
 * Half the PSRAM of a sprite and half the reads per blit, and the table makes
 * dimming, fades and tints free (rebuild 256 entries instead of touching pixels).
 * RGB332 loses nothing on Flat60: its palette is RGB332-exact and fast display
 * sends RGB332 to the panel anyway. Index 0 is black. */
typedef struct {
    uint8_t *px;       /* w*h RGB332 indices, row-major, PSRAM */
    int w, h;
} sprite8;

int sprite8_alloc(sprite8 *s, int w, int h);

/* Quantize an RGB565 sprite of the same size into s (nearest RGB332 level). */
void sprite8_from(sprite8 *s, const sprite *src);

/* RGB332 index -> 8-bit channel levels, and the table that shows indices as-is. */
void rgb332_levels(int idx, int rgb[3]);
void sprite8_lut_identity(uint16_t lut[256]);

/* Copy the sprite to logical (x, y), mapping every pixel through lut. Opaque,
 * like sprite_blit; `key` must change whenever the pixels or the lut change. */
void sprite8_blit(gfx_fb *fb, const sprite8 *s, int x, int y, const uint16_t *lut, unsigned key);

#endif
