#ifndef NOS_MBLUR_KERNEL_H
#define NOS_MBLUR_KERNEL_H
#include "gfx.h"

/* Scratch is caller-owned: no allocation, history frame, or PSRAM dependency. */
void mb_h4(uint16_t *row, int width, int log2_len, uint32_t *scratch);
typedef struct {
    uint16_t row[GFX_W];
    int next, pending;
} mb_h4_state;
void mb_h4_rows(gfx_fb *fb, int c0, int c1, int k, uint32_t *scratch, mb_h4_state *s);
typedef struct {
    uint32_t sum[(GFX_W + 3) / 4];
    uint32_t prev[(GFX_W + 3) / 4];
    int next, origin;
} mb_v4_state;
void mb_v4(gfx_fb *fb, int c0, int c1, int y0, int y1, int k, mb_v4_state *s);
/* Full-column live fallback: no horizontal pooling or row reconstruction.
 * Per-column continuity survives changing round-panel stripe clips. */
typedef struct {
    uint32_t sum[GFX_W];
    uint16_t next[GFX_W];
} mb_v_columns_state;
void mb_v_columns_reset(mb_v_columns_state *s);
void mb_v_columns(gfx_fb *fb,int c0,int c1,int y0,int y1,int k,mb_v_columns_state *s);
#endif
