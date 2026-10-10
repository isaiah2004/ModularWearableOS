#ifndef WATCH_BOARD_PROTOCOL_H
#define WATCH_BOARD_PROTOCOL_H
#include <stdint.h>
#include "hal.h"
int board_rtc_decode(const uint8_t raw[7], int64_t *epoch);
int board_rtc_encode(int64_t epoch, uint8_t raw[7]);
int board_datetime_valid(const hal_datetime *dt);
int board_touch_decode(const uint8_t raw[5], int *x, int *y);
/* CCW display output and inverse physical touch. USB uses UI coordinates. */
void board_pixels_ccw(const uint16_t *src, uint16_t *dst, int width, int height);
/* Same rotation reading a width x height window of rows `stride` apart, optionally
 * byte-swapping to wire order on the way: one pass instead of swap+pack, rotate. */
void board_pixels_ccw_swap(const uint16_t *src, int stride, uint16_t *dst,
                           int width, int height, int swap);
/* 1.43C (unrotated panel): copy a width x height window of rows `stride` apart
 * into a packed buffer, optionally byte-swapping to wire order. */
void board_pixels_pack_swap(const uint16_t *src, int stride, uint16_t *dst,
                            int width, int height, int swap);
/* Fast display: convert width x height wire-order (big-endian) RGB565 pixels to the
 * panel's 8-bit wire formats, in place-safe (dst may alias src). mode 1 = RGB332
 * with a 4x4 ordered dither, 2 = 256-level luma. The dither is anchored to panel
 * coordinates (px0,py0 = the window origin) so static pixels never shimmer when a
 * region is repainted through a different stripe. */
void board_pixels_to8(uint8_t *dst, const uint16_t *src, int width, int height,
                      int px0, int py0, int mode);
/* 1.43C fused pack + narrow: read a width x height window of rows `stride` apart
 * (native RGB565 if swap, else already wire order) straight into 8-bit wire pixels.
 * Same output as board_pixels_pack_swap followed by board_pixels_to8. */
void board_pixels_pack_to8(const uint16_t *src, int stride, uint8_t *dst, int width,
                           int height, int px0, int py0, int mode, int swap);
/* Physical touch -> UI. 1.43-B: inverse of the CCW output; 1.43C: 180 degrees. */
void board_native_to_ui(int *x, int *y);
void board_pointer_update(int *x, int *y, int raw_x, int raw_y, int down);
void board_imu_decode(const uint8_t raw[12], hal_imu *out);
#endif
