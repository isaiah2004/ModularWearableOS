/* Register formats from Waveshare Demo V3: FT3168, PCF85063, QMI8658.
 * Portable conversions are also exercised by tests/board_protocol_test.c. */
#include "board_protocol.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h" /* host tests pass -DCONFIG_WATCH_BOARD_143C=1 instead */
#endif

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
    board_pixels_ccw_swap(src, width, dst, width, height, 0);
}
void board_pixels_ccw_swap(const uint16_t *src, int stride, uint16_t *dst,
                           int width, int height, int swap)
{
    for (int x = width - 1; x >= 0; x--) {
        const uint16_t *col = src + x;
        if (swap)
            for (int y = 0; y < height; y++, col += stride)
                *dst++ = (uint16_t)((*col << 8) | (*col >> 8));
        else
            for (int y = 0; y < height; y++, col += stride)
                *dst++ = *col;
    }
}
void board_pixels_pack_swap(const uint16_t *src, int stride, uint16_t *dst,
                            int width, int height, int swap)
{
    for (int y = 0; y < height; y++, src += stride) {
        if (swap)
            for (int x = 0; x < width; x++)
                *dst++ = (uint16_t)((src[x] << 8) | (src[x] >> 8));
        else
            for (int x = 0; x < width; x++)
                *dst++ = src[x];
    }
}
static const uint8_t s_bayer4[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
static uint8_t s_lut_r[16][32], s_lut_g[16][64], s_lut_b[16][32];   /* pre-shifted RGB332 fields */
static uint8_t s_luma_hi[256], s_luma_lo[256];                      /* luma split by wire byte */
static int s_to8_ready;

static void to8_init(void)
{
    /* Ordered dither with thresholds confined to 0.15..0.85 of a step: a colour
     * within 0.15 of an RGB332 level (every Flat60 palette colour) maps to that
     * level cleanly, with no speckle; true in-between shades (AA edges, gradients)
     * still dither and average to the right level. */
    for (int t = 0; t < 16; t++) {
        float th = 0.15f + 0.7f * (float)(s_bayer4[t] * 2 + 1) / 32.0f;
        for (int v = 0; v < 32; v++) {
            int r3 = (int)(v * 7 / 31.0f + th), b2 = (int)(v * 3 / 31.0f + th);
            s_lut_r[t][v] = (uint8_t)((r3 > 7 ? 7 : r3) << 5);
            s_lut_b[t][v] = (uint8_t)(b2 > 3 ? 3 : b2);
        }
        for (int v = 0; v < 64; v++) {
            int g3 = (int)(v * 7 / 63.0f + th);
            s_lut_g[t][v] = (uint8_t)((g3 > 7 ? 7 : g3) << 2);
        }
    }
    /* Y = .30R + .59G + .11B scaled so white -> 255; the two halves of the 565 word
     * contribute independently, so two 256-entry tables cover all 65536 colours. */
    for (int hi = 0; hi < 256; hi++)
        s_luma_hi[hi] = (uint8_t)(((hi >> 3) * 633 + ((hi & 7) << 3) * 612) >> 8);
    for (int lo = 0; lo < 256; lo++)
        s_luma_lo[lo] = (uint8_t)(((lo >> 5) * 612 + (lo & 31) * 237) >> 8);
    s_to8_ready = 1;
}

void board_pixels_to8(uint8_t *dst, const uint16_t *src, int width, int height,
                      int px0, int py0, int mode)
{
    if (!s_to8_ready) to8_init();
    const uint8_t *s = (const uint8_t *)src;          /* wire order: hi byte first */
    for (int y = 0; y < height; y++) {
        if (mode == 3) {
            for (int x = 0; x < width; x++, s += 2)
                *dst++ = (uint8_t)((s[0] & 0xE0) | ((s[0] & 0x07) << 2) | ((s[1] >> 3) & 0x03));
            continue;
        }
        if (mode == 2) {
            for (int x = 0; x < width; x++, s += 2) {
                int v = s_luma_hi[s[0]] + s_luma_lo[s[1]];
                *dst++ = (uint8_t)(v > 255 ? 255 : v);
            }
            continue;
        }
        const uint8_t *row = &s_bayer4[((py0 + y) & 3) << 2];
        for (int x = 0; x < width; x++, s += 2) {
            int t = row[(px0 + x) & 3], hi = s[0], lo = s[1];
            *dst++ = s_lut_r[t][hi >> 3] | s_lut_g[t][((hi & 7) << 3) | (lo >> 5)] | s_lut_b[t][lo & 31];
        }
    }
}

/* Per-pixel cost here is instruction-bound (~25 cycles for a naive loop with a
 * per-pixel byte-order test). The no-dither path converts two pixels per
 * register; every path keeps the byte-order test out of its inner loop. */
#define RGB332_T(v) ((uint32_t)((((v) >> 8) & 0xE0) | (((v) >> 6) & 0x1C) | (((v) >> 3) & 0x03)))
#define LOAD(p, i)  (swap ? (p)[i] : __builtin_bswap16((p)[i]))

void board_pixels_pack_to8(const uint16_t *src, int stride, uint8_t *dst, int width,
                           int height, int px0, int py0, int mode, int swap)
{
    if (!s_to8_ready) to8_init();
    for (int y = 0; y < height; y++, src += stride) {
        const uint16_t *__restrict s = src;
        uint8_t *__restrict d = dst;
        dst += width;
        int x = 0;
        if (mode == 2) {
            for (; x < width; x++) {
                uint16_t v = LOAD(s, x);
                int l = s_luma_hi[v >> 8] + s_luma_lo[v & 0xFF];
                d[x] = (uint8_t)(l > 255 ? 255 : l);
            }
            continue;
        }
        if (mode == 3) {                       /* RGB332 truncation: R[15:13] G[10:8] B[4:3] */
            /* Two pixels per register: the same three mask/shift steps extract
             * both bytes (to bits 0-7 and 16-23), then one 16-bit store. Separate
             * loops per source byte order keep the swap test out of the loop. */
            if (x < width && ((uintptr_t)(d + x) & 1)) { d[x] = (uint8_t)RGB332_T(LOAD(s, x)); x++; }
            uint16_t *__restrict d2 = (uint16_t *)(d + x);
            if (swap) {                        /* native RGB565 */
                for (; x + 2 <= width; x += 2) {
                    uint32_t w = (uint32_t)s[x] | (uint32_t)s[x + 1] << 16;
                    uint32_t c = ((w >> 8) & 0x00E000E0u) | ((w >> 6) & 0x001C001Cu) | ((w >> 3) & 0x00030003u);
                    *d2++ = (uint16_t)((c & 0xFFu) | ((c >> 8) & 0xFF00u));
                }
            } else {                           /* wire order: hi byte in bits 0-7 */
                for (; x + 2 <= width; x += 2) {
                    uint32_t w = (uint32_t)s[x] | (uint32_t)s[x + 1] << 16;
                    uint32_t c = (w & 0x00E000E0u) | ((w << 2) & 0x001C001Cu) | ((w >> 11) & 0x00030003u);
                    *d2++ = (uint16_t)((c & 0xFFu) | ((c >> 8) & 0xFF00u));
                }
            }
            for (; x < width; x++) d[x] = (uint8_t)RGB332_T(LOAD(s, x));
            continue;
        }
        /* Dithered RGB332: the threshold only depends on (x+px0)&3, so the lane
         * tables are bound once per row. */
        const uint8_t *row = &s_bayer4[((py0 + y) & 3) << 2];
        const uint8_t *R[4], *G[4], *B[4];
        for (int k = 0; k < 4; k++) {
            int t = row[(px0 + k) & 3];
            R[k] = s_lut_r[t]; G[k] = s_lut_g[t]; B[k] = s_lut_b[t];
        }
#define TO8(k, v) (R[k][(v) >> 11] | G[k][((v) >> 5) & 63] | B[k][(v) & 31])
        /* Unrolled by four with byte stores and one loop per byte order (measured
         * faster here than assembling 32-bit words: the lookups dominate). */
        if (swap) {
            for (; x + 4 <= width; x += 4) {
                uint16_t a = s[x], b = s[x + 1], c = s[x + 2], e = s[x + 3];
                d[x] = TO8(0, a); d[x + 1] = TO8(1, b); d[x + 2] = TO8(2, c); d[x + 3] = TO8(3, e);
            }
        } else {
            for (; x + 4 <= width; x += 4) {
                uint16_t a = __builtin_bswap16(s[x]), b = __builtin_bswap16(s[x + 1]);
                uint16_t c = __builtin_bswap16(s[x + 2]), e = __builtin_bswap16(s[x + 3]);
                d[x] = TO8(0, a); d[x + 1] = TO8(1, b); d[x + 2] = TO8(2, c); d[x + 3] = TO8(3, e);
            }
        }
        for (; x < width; x++) { uint16_t v = LOAD(s, x); d[x] = TO8(x & 3, v); }
#undef TO8
    }
}
#undef LOAD
#undef RGB332_T

void board_native_to_ui(int *x, int *y)
{
#if CONFIG_WATCH_BOARD_143C
    /* CST820 under the 36h C0 (180 degree) panel: mirror both axes, as the demo. */
    *x = 465 - *x;
    *y = 465 - *y;
#else
    int native_x = *x;
    *x = 465 - *y;
    *y = native_x;
#endif
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
