/* layer.h — static-background cache for watch faces.
 *
 * Many faces redraw an expensive but *static* background every frame (the
 * Classic tick ring, the Dots marker ring, ...). Those pixels never change,
 * so we render them ONCE into a full-screen RGB565 buffer and fast-blit the
 * cache each frame, then draw only the moving parts (hands, sweeping arcs,
 * live text) on top.
 *
 * Each cached layer is a full framebuffer: GFX_W*GFX_H*2 = 466*466*2
 * = 434,312 bytes (~424 KB). We keep LAYER_MAX slots, so the cache costs
 * LAYER_MAX * ~424 KB of RAM. On the ESP32-S3 target these buffers live in
 * PSRAM (the board has multiple MB), not internal SRAM.
 *
 * Slots are keyed by an arbitrary integer `tag` (each face uses its own tag).
 * Only one face is ever on screen at a time, so LAYER_MAX slots comfortably
 * cover the handful of faces that cache; if more distinct tags than slots are
 * requested, the least-recently-assigned slot is recycled (that face simply
 * re-renders its background once the next time it is shown). */
#ifndef WATCH_LAYER_H
#define WATCH_LAYER_H

#include "gfx.h"

/* Number of cached full-screen backgrounds held at once. Analog faces use two
 * (a static dial + a per-second "dial+hands" base), so keep a little headroom.
 * RAM cost = LAYER_MAX * GFX_W * GFX_H * 2 bytes = 4 * ~424 KB ~= 1.7 MB PSRAM. */
#define LAYER_MAX 4

/* If layer `tag` is not cached yet (first sight, or after layer_invalidate),
 * claim a slot and return a zeroed-translate gfx_fb* to draw the static
 * background into ONCE; the caller must then draw the background and it stays
 * cached. Returns NULL when the layer is already cached — draw nothing and
 * just call layer_blit. */
gfx_fb *layer_need_render(int tag);

/* Fast-copy the cached pixels for `tag` into dst (honors dst->ox/oy and
 * clips, so it is safe even mid page-slide). No-op if `tag` is not cached. */
void layer_blit(gfx_fb *dst, int tag);

/* Force re-render of layer `tag` on its next layer_need_render call.
 * Pass tag < 0 to invalidate every cached layer. */
void layer_invalidate(int tag);

#endif
