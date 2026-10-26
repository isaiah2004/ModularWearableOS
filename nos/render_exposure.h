#ifndef NOS_RENDER_EXPOSURE_H
#define NOS_RENDER_EXPOSURE_H
#include "gfx.h"
#define NOS_EXPOSURE_ROWS 16
#define NOS_EXPOSURE_WORDS ((GFX_W + 31) / 32)
typedef struct {
    gfx_fb fb; /* pointer remains exclusively owned until worker finishes */
    int x0, y0, x1, y1, enabled, mode;
    uint32_t directional[NOS_EXPOSURE_ROWS][NOS_EXPOSURE_WORDS];
} nos_exposure_job;
/* UI thread after final scene draw. Returns0 for invalid/>16-row work: caller
 * must use synchronous original path. No allocation, job owns coverage snapshot. */
int nos_exposure_prepare(nos_exposure_job *job, const gfx_fb *fb, int x0, int y0, int x1, int y1,
                         int temporal, int aod);
/* Sole history producer; frame begin/reset/stat reads require worker fence. */
void nos_exposure_apply(const nos_exposure_job *job);
#endif
