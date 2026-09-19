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
void board_native_to_ui(int *x, int *y);
void board_pointer_update(int *x, int *y, int raw_x, int raw_y, int down);
void board_imu_decode(const uint8_t raw[12], hal_imu *out);
#endif
