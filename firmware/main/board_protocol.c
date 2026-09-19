/* Register formats from Waveshare Demo V3: FT3168, PCF85063, QMI8658.
 * Portable conversions are also exercised by tests/board_protocol_test.c. */
#include "board_protocol.h"

static int bcd(uint8_t v)
{
    return (v & 15) > 9 || (v >> 4) > 9 ? -1 : (v >> 4) * 10 + (v & 15);
}
static uint8_t enc(int v) { return (uint8_t)((v / 10) * 16 + v % 10); }
static int leap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
static int mdays(int y, int m)
{
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return days[m - 1] + (m == 2 && leap(y));
}
int board_datetime_valid(const hal_datetime *d)
{
    return d && d->year >= 2000 && d->year <= 2099 && d->month >= 1 && d->month <= 12 &&
        d->day >= 1 && d->day <= mdays(d->year, d->month) &&
        d->hour >= 0 && d->hour <= 23 && d->minute >= 0 && d->minute <= 59 &&
        d->second >= 0 && d->second <= 59;
}
int board_rtc_decode(const uint8_t r[7], int64_t *epoch)
{
    /* OS means the oscillator stopped; never use this as a trusted time. */
    if ((r[0] & 0x80) || r[4] > 6) return 0;
    int s = bcd(r[0]), min = bcd(r[1]), h = bcd(r[2]);
    int d = bcd(r[3]), m = bcd(r[5]), y = bcd(r[6]);
    if (s < 0 || s > 59 || min < 0 || min > 59 || h < 0 || h > 23 ||
        y < 0 || m < 1 || m > 12 || d < 1 || d > mdays(2000 + y, m)) return 0;
    y += 2000;
    int days = 0;
    for (int yr = 1970; yr < y; yr++) days += 365 + leap(yr);
    for (int mo = 1; mo < m; mo++) days += mdays(y, mo);
    *epoch = ((int64_t)(days + d - 1) * 24 + h) * 3600 + min * 60 + s;
    return 1;
}
int board_rtc_encode(int64_t epoch, uint8_t r[7])
{
    if (epoch < 946684800LL || epoch >= 4102444800LL) return 0;
    int days = (int)(epoch / 86400), rem = (int)(epoch % 86400);
    r[0] = enc(rem % 60); r[1] = enc(rem / 60 % 60); r[2] = enc(rem / 3600);
    r[4] = (uint8_t)((days + 4) % 7);
    int y = 1970, m = 1;
    while (days >= 365 + leap(y)) { days -= 365 + leap(y); y++; }
    while (days >= mdays(y, m)) { days -= mdays(y, m); m++; }
    r[3] = enc(days + 1); r[5] = enc(m); r[6] = enc(y - 2000);
    return 1;
}
int board_touch_decode(const uint8_t r[5], int *x, int *y)
{
    *x = ((r[1] & 15) << 8) | r[2];
    *y = ((r[3] & 15) << 8) | r[4];
    return (r[0] & 15) > 0 && (r[1] >> 6) != 1 && *x < 466 && *y < 466;
}
void board_pixels_ccw(const uint16_t *src, uint16_t *dst, int width, int height)
{
    for (int x = width - 1; x >= 0; x--)
        for (int y = 0; y < height; y++)
            *dst++ = src[y * width + x];
}
void board_native_to_ui(int *x, int *y)
{
    int native_x = *x;
    *x = 465 - *y;
    *y = native_x;
}
void board_pointer_update(int *x, int *y, int raw_x, int raw_y, int down)
{
    /* FT3168 release coordinates may be stale/zero. End the gesture at the
     * last valid UI point, never at unrotated or invalid register contents. */
    if (!down) return;
    *x = raw_x; *y = raw_y;
    board_native_to_ui(x, y);
}
static int16_t le16(const uint8_t *p) { return (int16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8); }
void board_imu_decode(const uint8_t r[12], hal_imu *o)
{
    /* CTRL2 = +/-8g (4096 LSB/g), CTRL3 = +/-512 dps (64 LSB/dps). */
    o->ax = le16(r) / 4096.0f; o->ay = le16(r + 2) / 4096.0f; o->az = le16(r + 4) / 4096.0f;
    o->gx = le16(r + 6) / 64.0f; o->gy = le16(r + 8) / 64.0f; o->gz = le16(r + 10) / 64.0f;
}
