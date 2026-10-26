#ifndef NOS_BAND_DAMAGE_H
#define NOS_BAND_DAMAGE_H
#include <stdint.h>
#define BD_WIDTH 466
#define BD_HEIGHT 466
#define BD_ROWS 16
#define BD_BANDS 30
#define BD_WORDS 8
#define BD_MAX_SPANS 2
typedef struct {
  uint32_t bits[BD_BANDS][BD_WORDS];
} band_damage;
void band_damage_clear(band_damage *);
/* Captures raw changed command bounds before legacy rectangle coalescing. */
void band_damage_add(band_damage *, int x0, int y0, int x1, int y1);
/* Current + previous SHARP damage only. No recursive temporal tail
 * accumulation. Outputs even-x, canonical16-row-band spans; at most two
 * disjoint intervals. If more than two remain after gap merging, collapse that
 * band to one bounding interval. */
int band_damage_spans(const band_damage *, const band_damage *previous,
                      int include_previous, int band, int gap_px,
                      int spans[BD_MAX_SPANS][2]);
#endif
