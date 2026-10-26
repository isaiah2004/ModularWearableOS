/* Bounded one-frame temporal exposure fallback; not multisample motion blur. */
#ifndef NOS_RENDER_HISTORY_H
#define NOS_RENDER_HISTORY_H
#include "gfx.h"
#include <stdint.h>

typedef struct {
    uint32_t frame, pixels, seeded, unchanged, changed, blended;
    uint32_t duplicates, equal_words, rows_cleared;
    uint32_t uniform_read_bytes, uniform_write_bytes; /* logical PSRAM bytes avoided */
    uint32_t uniform_pixels; /* exact matching known solid-history shortcut */
    uint32_t copied; /* logical native updates, including virtual black stores */
} nos_history_stats;

/* Cumulative logical representation counters, not measured bus transactions.
 * Reset invalidates history but retains these diagnostic totals. */
typedef struct {
    uint64_t virtual_pixels, avoided_bytes, materialize_bytes, classify_pixels;
} nos_history_virtual_stats;
/* Experimental exact virtual-black representation, default0. The caller must
 * fence all history workers before changing mode or materializing raw storage.
 * Disabling drains all virtual blocks before publishing mode0. */
void nos_history_virtual_set(int enabled);
int nos_history_virtual_get(void);
const nos_history_virtual_stats *nos_history_virtual_get_stats(void);
/* Required before any debug/test consumer reads raw RGB565 history storage. */
void nos_history_materialize(void);

/* One allocation: RGB565 sharp history plus two row-aligned bitmaps.
 * Returns zero on allocation failure; apply then returns zero without writes.
 * Call on the renderer thread only. Reset invalidates without copying a frame. */
int nos_history_init(void);
/* Native physical-copy policy; virtual black takes precedence when enabled. */
extern int nos_history_native_copy; /* exact runtime A/B, default1 */
extern int nos_history_uniform; /* exact internal shortcut A/B; defaults to1 */
void nos_history_reset(void);
/* Legacy begin selects75/25 (Q5 previous weight8), independent of cadence mode.
 * With cadence enabled, interval begin targets1/120s mean age using rounded Q5
 * previous-SHARP weight round(32e6/(120*elapsed_us)), capped32; zero also selects8.
 * Quantization is approximate:40Hz uses11/32 (8.594ms versus8.333ms target).
 * This adjusts mean age, not the shape of the two-sample exposure. Supply the
 * same logical elapsed interval used for scene motion, including skipped ticks.
 * The first begin for a generation fixes both interval and weight. Runtime mode
 * changes take effect next generation. All deferred exposure jobs must finish
 * before a new generation begins; no per-pixel timestamp/history is added. */
void nos_history_begin(uint32_t frame);
extern int nos_history_cadence; /* Experimental runtime A/B, default0 selects75/25. */
void nos_history_begin_interval(uint32_t frame,uint32_t elapsed_us);
unsigned nos_history_previous_weight_q5(void);
uint32_t nos_history_interval_us(void);
/* Half-open SCREEN coordinates; content ox/oy are already baked into fb pixels.
 * Clips against screen, stripe, fb column clip, and packed column-band storage.
 * enabled=0 still stores current sharp history, without blending the output.
 * A pixel is processed at most once per begin(frame), even with overlap. Keep
 * its first output: freshly re-rendered overlapping buffers must be coalesced
 * before calling apply. No framebuffer contents are read outside the window.
 * Caller must redraw this frame's damage UNION previous frame's damage, using
 * SHARP scene pixels, for one extra frame to erase exposure tails. Untouched
 * pixels must still equal their last sharp history. Reset after scene/display
 * discontinuities if that contract cannot be maintained. The first observation
 * of each pixel seeds history without blur; static equal pixels are exact.
 * Returns one if storage is ready (also for an empty window), zero otherwise. */
int nos_history_apply(gfx_fb *fb, int x0, int y0, int x1, int y1, int enabled);
/* Disjoint compositor fast path. All history windows in a frame must be
 * nonoverlapping, including calls to the generic API. Cold/holes fall back to
 * generic initialization. Warm enabled=0 updates sharp history without comparison; copied
 * counts logical updates, including virtual black, so seeded+unchanged+changed+copied equals pixels.
 * Duplicates are forbidden by this API contract, not detected. */
int nos_history_apply_disjoint(gfx_fb *fb,int x0,int y0,int x1,int y1,int enabled);
const nos_history_stats *nos_history_get_stats(void);
/* Actual successful allocation size, zero when initialization failed. */
uint32_t nos_history_storage_bytes(void);
extern int nos_history_packed; /* Exact debug A/B, default1; select outside pixels. */
#endif
