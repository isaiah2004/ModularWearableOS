/* gfx_bench.h — per-primitive drawing cost (ns per covered pixel), rendered as
 * full-frame stripes like the UI. `stripe` must hold GFX_W * stripe_h pixels;
 * `yield` (optional) runs between primitives so a watchdog can be fed. */
#ifndef WATCH_GFX_BENCH_H
#define WATCH_GFX_BENCH_H
#include "gfx.h"
void gfx_bench_run(uint16_t *stripe, int stripe_h, void (*yield)(void));
#endif
