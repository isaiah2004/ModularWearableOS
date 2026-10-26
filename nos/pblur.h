#ifndef NOS_PBLUR_H
#define NOS_PBLUR_H
#include "gfx.h"

/* Persistent, indexed-colour half-height page copies. IDs 0..5 home; 6 drawer.
 * The callback draws settled content at (0,0), without ticking or side effects.
 * tick is idle-only, at most once per frame, and returns 1 while work is pending.
 * A source change during a bake discards the unpublished staging copy. */
typedef void (*pb_draw_fn)(gfx_fb *fb);
int pblur_tick(int id, int vertical, pb_draw_fn draw);
int pblur_busy(int id);
void pblur_invalidate(int id);
void pblur_check(int id, pb_draw_fn draw); /* signature only; safe during motion */
int pblur_draw(gfx_fb *fb, int id, int x, int y, float step);
unsigned pblur_ready_mask(void);
unsigned pblur_baked_mask(void); /* slots with at least one completely published copy */
unsigned pblur_bytes(void);
extern unsigned pblur_hits[2], pblur_misses[2], pblur_published, pblur_discarded;
extern unsigned pblur_why[5];   /* discards: [switch, timeout, sig]; [3] frames skipped busy, [4] slices baked */
extern int pblur_baking; /* settled, side-effect-free draw mode for widget callbacks */
#endif
