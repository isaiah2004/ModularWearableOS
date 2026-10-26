#ifndef NOS_SETTINGS_SHUTTER_H
#define NOS_SETTINGS_SHUTTER_H
#include "gfx.h"
#include <stdint.h>
/* Experimental, defaultOFF. Setter is UI-thread only, outside painting. */
void set_settings_shutter_set(int enabled);
int set_settings_shutter_get(void);
typedef struct {
  unsigned ticks, moving_ticks, painted_pixels, filtered_pixels, fallback_calls;
  int enabled, exposure_q8, pose_q8;
} settings_shutter_stats;
settings_shutter_stats set_settings_shutter_stats(void);
typedef struct {
  int full0, full1, n;
  int offset[4];
  uint32_t weight[4], den, reciprocal;
  int zero, pose;
} settings_shutter_kernel;
typedef struct {
  int x0, y0, x1, y1;
  unsigned pixels;
} settings_shutter_coverage;
/* Bounds: exposure +/-12288Q8(48px), pose +/-4096Q8. Invalidinput returns0.
 * Kernel is frozen onceperframe; invalidinput never negates INT_MIN. */
int settings_shutter_kernel_make(settings_shutter_kernel *, int exposure_q8,
                                 int pose_q8);
void settings_shutter_prefix_rows(const uint8_t *, uint16_t *, int w, int h,
                                  int first, int end);
settings_shutter_coverage
settings_shutter_blit(gfx_fb *, const uint8_t *, const uint16_t *, int w, int h,
                      int x, int y, const uint16_t *,
                      const settings_shutter_kernel *, int exposure, int pose,
                      int owner0, int owner1, unsigned key);
#endif
