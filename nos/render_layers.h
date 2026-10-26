#ifndef NOS_RENDER_LAYERS_H
#define NOS_RENDER_LAYERS_H
#include "nos.h"

/* Up to three retained sharp screen rasters, allocated on demand in PSRAM. Content is
 * invalidated in 16-row bands by ordered drawing-command signatures. A moving
 * screen changes its transform, not these cached pixels. */
typedef struct {
    uint32_t bytes, bands_rendered, bands_reused, blit_pixels, overflow_frames;
    uint32_t record_us, raster_us, allocation_failures;
} nos_layer_stats;
extern int nos_layer_cache_enabled;
void nos_layers_init(void);
/* UI-thread safe point; releases all rasters and allows allocation retries. */
void nos_layers_release(void);
void nos_layers_idle(void);
void nos_layers_invalidate(void);
void nos_layers_begin(uint32_t frame);
int nos_layers_draw(gfx_fb *fb, const nos_screen *screen);
nos_layer_stats nos_layers_stats(void);
#endif
