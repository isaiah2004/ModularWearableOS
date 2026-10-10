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

#endif
