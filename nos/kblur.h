/* kblur.h — blurred copies of items that keep their look, made once while idle.
 *
 * The list cache in ui.c (ui_fblur_*) belongs to whichever list moved last and re-bakes
 * its bands when the list or its owner changes. Items like notification cards don't
 * change once they arrive, so this cache keeps one blurred copy per item, keyed by the
 * caller (a notification id) with a signature of its look, in its own PSRAM arena that
 * no other list can take: a card is baked once, while the watch is idle, right after it
 * arrives, and only again if its look changes.
 *
 * An item is a band w x h (h <= max_h) drawn by fn(fb, i, x0, y, x1, h, ctx) with its top
 * at y; the bake renders it with ctx rows of black around it, so the blur's tails fade
 * into the gap and are cropped at the band (items abut, opaque blits never overlap).
 * Levels: vertical box blurs of 8 / 16 / 32 / 48 px (KB_NLEV), kept at half height.
 *
 *   static kblur *K;  if (!K) K = kblur_new(w, max_h, 24, 1600 * 1024);
 *   idle:   kblur_bake(K, key, sig, h, fn, i, ctx, 4000);       // a few ms of work
 *   glide:  if (kblur_ready(K, key, sig, lv)) kblur_draw(fb, K, key, lv, x, y, fade);
 */
#ifndef NOS_KBLUR_H
#define NOS_KBLUR_H
#include "gfx.h"
#include "ui.h"

#define KB_NLEV 4
typedef struct kblur kblur;
/* w x max_h items, ctx rows of context each side, an arena of `bytes` (NULL if no PSRAM) */
kblur *kblur_new(int w, int max_h, int ctx, unsigned bytes);
/* Bake (or keep) item `key`: returns 1 while work remains for it. Spends about budget_us
 * per call (a fixed step count in the sim, which must replay identically). */
int  kblur_bake(kblur *c, unsigned key, unsigned sig, int h, ui_blur_row_fn fn, int i, void *ctx, unsigned budget_us);
/* 1 if item key with this look has level lv (1..KB_NLEV) ready */
int  kblur_ready(const kblur *c, unsigned key, unsigned sig, int lv);
/* Blit level lv of item key with its top-left at (x, y), inked by fade (0..1). */
int  kblur_draw(gfx_fb *fb, const kblur *c, unsigned key, int lv, int x, int y, float fade);
/* Forget items whose keys are not in keys[0..n) (dismissed notifications). */
void kblur_keep_only(kblur *c, const unsigned *keys, int n);
int  kblur_slots(const kblur *c);   /* items it can hold */

#endif
