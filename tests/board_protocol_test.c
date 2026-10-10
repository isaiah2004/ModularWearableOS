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
#if CONFIG_WATCH_BOARD_143C
    /* 1.43C: the panel is upright (36h C0) and the CST820 reads 180 degrees off. */
    for (int uy = 0; uy < 466; uy++) for (int ux = 0; ux < 466; ux++) {
        x = 465 - ux; y = 465 - uy;
        board_native_to_ui(&x, &y);
        assert(x == ux && y == uy);
    }
#else
    /* All screen pixels must return to their UI position after the display's
     * CCW mapping and the touch inverse, including each extreme corner. */
    for (int uy = 0; uy < 466; uy++) for (int ux = 0; ux < 466; ux++) {
        x = uy; y = 465 - ux;
        board_native_to_ui(&x, &y);
        assert(x == ux && y == uy);
    }
#endif
    board_pixels_ccw(pixels, actual + 1, 1, 6);
    assert(!memcmp(actual + 1, pixels, sizeof pixels));
    board_pixels_ccw(pixels, actual + 1, 6, 1);
    for (int i = 0; i < 6; i++) assert(actual[1 + i] == pixels[5 - i]);
    x = 0; y = 0;
    {   /* fused path: a 2x2 window of a stride-4 buffer, byte-swapped */
        const uint16_t big[] = {0x0102,0x0304,0x0506,0x0708, 0x090a,0x0b0c,0x0d0e,0x0f10};
        const uint16_t want[] = {0x0807,0x100f,0x0605,0x0e0d};   /* cols 3,2 of rows 0,1 */
        uint16_t got[4];
        board_pixels_ccw_swap(big + 2, 4, got, 2, 2, 1);
        assert(!memcmp(got, want, sizeof want));
    }
    {   /* 1.43C pack path: same window, unrotated */
        const uint16_t big[] = {0x0102,0x0304,0x0506,0x0708, 0x090a,0x0b0c,0x0d0e,0x0f10};
        const uint16_t want[] = {0x0605,0x0807,0x0e0d,0x100f};
        uint16_t got[4];
        board_pixels_pack_swap(big + 2, 4, got, 2, 2, 1);
        assert(!memcmp(got, want, sizeof want));
        board_pixels_pack_swap(big + 2, 4, got, 2, 2, 0);
        assert(got[0] == 0x0506 && got[1] == 0x0708 && got[2] == 0x0d0e && got[3] == 0x0f10);
    }
    {   /* fast display: 565 wire order -> RGB332 / luma, in place */
        const uint16_t px565[6] = {0xFFFF, 0x0000, 0x00F8, 0xE007, 0x1F00, 0x10A5};  /* white black red green blue grey */
        uint16_t buf[6];
        memcpy(buf, px565, sizeof buf);
        board_pixels_to8((uint8_t *)buf, buf, 6, 1, 0, 0, 1);
        const uint8_t *o = (const uint8_t *)buf;
        assert(o[0] == 0xFF && o[1] == 0x00 && o[2] == 0xE0 && o[3] == 0x1C && o[4] == 0x03);
        memcpy(buf, px565, sizeof buf);
        board_pixels_to8((uint8_t *)buf, buf, 6, 1, 0, 0, 2);
        assert(o[0] == 255 && o[1] == 0 && o[2] == 76 && o[3] == 149 && o[4] == 28);
        /* mid grey dithers: a 4x4 tile averages to the true level, not one flat step */
        uint16_t tile[16]; uint8_t t8[16]; int sum = 0;
        for (int i = 0; i < 16; i++) tile[i] = 0x10A5;   /* wire order of 0xA510: r=20 g=40 b=16 */
        board_pixels_to8(t8, tile, 4, 4, 0, 0, 1);
        for (int i = 0; i < 16; i++) sum += t8[i] >> 5;
        assert(sum >= 16 * 20 * 7 / 31 - 1 && sum <= 16 * 20 * 7 / 31 + 2);   /* ~4.5 avg red */
        assert(t8[0] != t8[5] || t8[1] != t8[2]);    /* not a flat colour */
        /* fused pack+narrow == pack_swap then to8, for both modes, odd window offsets
         * and widths (unrolled body + tail), native and wire-order sources */
        uint16_t src[7 * 9], wire[7 * 9], packed[5 * 6];
        uint8_t a8[5 * 6], b8[5 * 6];
        for (int i = 0; i < 7 * 9; i++) { src[i] = (uint16_t)(i * 2654435761u >> 7); wire[i] = (uint16_t)((src[i] << 8) | (src[i] >> 8)); }
        for (int mode = 1; mode <= 2; mode++)
            for (int sw = 0; sw <= 1; sw++)
                for (int w = 1; w <= 6; w++) {
                    board_pixels_pack_swap(src + 10, 7, packed, w, 5, 1);
                    board_pixels_to8(a8, packed, w, 5, 3, 1, mode);
                    board_pixels_pack_to8((sw ? src : wire) + 10, 7, b8, w, 5, 3, 1, mode, sw);
                    assert(!memcmp(a8, b8, (size_t)w * 5));
                }
    }
#if CONFIG_WATCH_BOARD_143C
    board_pointer_update(&x, &y, 120, 365, 1);
    assert(x == 345 && y == 100);
    board_pointer_update(&x, &y, 0, 0, 0);
    assert(x == 345 && y == 100); /* release cannot turn a tap into a swipe */
    board_pointer_update(&x, &y, 4095, 4095, 0);
    assert(x == 345 && y == 100); /* failed reads retain the last valid point */
    puts("PASS (1.43C): RTC, touch press/release, QMI8658, pixels and 180-degree touch");
#else
    board_pointer_update(&x, &y, 120, 365, 1);
    assert(x == 100 && y == 120);
    board_pointer_update(&x, &y, 0, 0, 0);
    assert(x == 100 && y == 120); /* release cannot turn a tap into a swipe */
    board_pointer_update(&x, &y, 130, 345, 1);
    assert(x == 120 && y == 130);
    board_pointer_update(&x, &y, 4095, 4095, 0);
    assert(x == 120 && y == 130); /* failed reads retain the last valid point */
    puts("PASS: RTC, FT3168 press/release, QMI8658, CCW pixels and touch inverse");
#endif
    return 0;
}
