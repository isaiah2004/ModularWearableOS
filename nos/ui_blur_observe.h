#ifndef UI_BLUR_OBSERVE_H
#define UI_BLUR_OBSERVE_H
#include <stdint.h>
#include "gfx.h"
typedef struct {
    uint64_t requested_processed_area,sharp_processed_area,blurred_processed_area;
    /* Actual prepared native raster calls, including overlap/black/overdraw.
     * These counters never imply unique or final-visible pixel coverage. */
    uint64_t native_processed_area,native_blurred_processed_area;
    uint32_t unknown_requests,frame;
} ui_fblur_observation;
/* Default-off exact native menu diagnostic: bit0 stages source rows, bit1
 * stages the selected palette. Shared bytes belong only to the UI rasterizer;
 * recording and flush/history workers must not use this nonreentrant owner. */
extern int nos_native_row_stage_mode;
extern uint8_t nos_native_row_stage_scratch[512];
void ui_fblur_observe_enable(int enabled);
void ui_fblur_observe_begin(unsigned frame);
void ui_fblur_observe_get(ui_fblur_observation *out);
void ui_fblur_observe_native(const gfx_fb *fb,int x0,int y0,int x1,int y1,int blurred);
#endif
