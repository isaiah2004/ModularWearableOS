#include "board_protocol.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint8_t raw[7]; int64_t epoch = 0;
    /* UTC codec round-trip across leap day, Y2038 and chip's whole range. */
    const int64_t epochs[] = {946684800LL, 951782400LL, 1709164800LL, 2147483648LL, 4102444799LL};
    for (unsigned i = 0; i < sizeof epochs / sizeof epochs[0]; i++) {
        assert(board_rtc_encode(epochs[i], raw));
        assert(board_rtc_decode(raw, &epoch) && epoch == epochs[i]);
    }
    assert(board_rtc_encode(1709164800LL, raw)); /* 2024-02-29 */
    assert(raw[3] == 0x29 && raw[5] == 0x02 && raw[6] == 0x24);
    raw[6] = 0x23; assert(!board_rtc_decode(raw, &epoch)); /* not a leap year */
    raw[6] = 0x24; raw[0] |= 0x80; assert(!board_rtc_decode(raw, &epoch)); /* oscillator stopped */
    raw[0] = 0x6a; assert(!board_rtc_decode(raw, &epoch)); /* malformed BCD */
    assert(!board_rtc_encode(0, raw));
    assert(!board_rtc_encode(4102444800LL, raw));
    hal_datetime dt = {.year=2024, .month=2, .day=29, .hour=23, .minute=59, .second=59};
    assert(board_datetime_valid(&dt));
    dt.year = 2023; assert(!board_datetime_valid(&dt));
    dt.month = 13; assert(!board_datetime_valid(&dt));
    dt.month = 1; dt.hour = -1; assert(!board_datetime_valid(&dt));
    /* Check every day supported by this RTC, not just hand-picked dates. */
    for (int64_t t = 946684800LL; t < 4102444800LL; t += 86400) {
        assert(board_rtc_encode(t + 45296, raw));
        assert(board_rtc_decode(raw, &epoch) && epoch == t + 45296);
    }
    int x, y;
    uint8_t touch[5] = {1, 0x81, 0xd1, 0x01, 0xd1};
    assert(board_touch_decode(touch, &x, &y) && x == 465 && y == 465);
    touch[2] = 0xd2; assert(!board_touch_decode(touch, &x, &y));
    touch[2] = 0xd1; touch[1] = 0x41; assert(!board_touch_decode(touch, &x, &y));
    touch[1] = 0x81; touch[0] = 0; assert(!board_touch_decode(touch, &x, &y));
    uint8_t imu[12] = {0x00,0x10, 0x00,0xf0, 0x00,0x20, 0x40,0, 0xc0,0xff, 0,0x80};
    hal_imu m; board_imu_decode(imu, &m);
    assert(m.ax == 1 && m.ay == -1 && m.az == 2);
    assert(m.gx == 1 && m.gy == -1 && m.gz == -512);
    const uint16_t pixels[] = {0x1234,2,3,4,5,0xabcd};
    const uint16_t rotated[] = {3,0xabcd,2,5,0x1234,4};
    uint16_t actual[8] = {0xbeef,0,0,0,0,0,0,0xcafe};
    board_pixels_ccw(pixels, actual + 1, 3, 2);
    assert(!memcmp(actual + 1, rotated, sizeof rotated));
    assert(actual[0] == 0xbeef && actual[7] == 0xcafe);
    /* All screen pixels must return to their UI position after the display's
     * CCW mapping and the touch inverse, including each extreme corner. */
    for (int uy = 0; uy < 466; uy++) for (int ux = 0; ux < 466; ux++) {
        x = uy; y = 465 - ux;
        board_native_to_ui(&x, &y);
        assert(x == ux && y == uy);
    }
    board_pixels_ccw(pixels, actual + 1, 1, 6);
    assert(!memcmp(actual + 1, pixels, sizeof pixels));
    board_pixels_ccw(pixels, actual + 1, 6, 1);
    for (int i = 0; i < 6; i++) assert(actual[1 + i] == pixels[5 - i]);
    x = 0; y = 0;
    board_pointer_update(&x, &y, 120, 365, 1);
    assert(x == 100 && y == 120);
    board_pointer_update(&x, &y, 0, 0, 0);
    assert(x == 100 && y == 120); /* release cannot turn a tap into a swipe */
    board_pointer_update(&x, &y, 130, 345, 1);
    assert(x == 120 && y == 130);
    board_pointer_update(&x, &y, 4095, 4095, 0);
    assert(x == 120 && y == 130); /* failed reads retain the last valid point */
    puts("PASS: RTC, FT3168 press/release, QMI8658, CCW pixels and touch inverse");
    return 0;
}
