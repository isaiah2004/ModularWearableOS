/* app_ble.c — passive BLE scanner (white-hat recon; observe-only).
 *
 * WHITE-HAT / PASSIVE: this app never advertises, connects, injects, spoofs
 * or attacks. It only READS the result of a passive scan (hal_ble_scan) and
 * lists what is already broadcasting nearby, exactly like the Wi-Fi analyzer.
 *
 * PLAIN-ENGLISH ALGORITHM (scan -> list -> detail):
 *   1. on_start: run hal_ble_scan() once into a fixed static array (<=12
 *      devices, no malloc). Remember how many came back (0 => "No devices").
 *   2. LIST state (sel < 0): draw a header ("BLUETOOTH" + "N DEVICES") and one
 *      row per device: its name on the left and 0..4 signal BARS on the right,
 *      derived from RSSI (stronger dBm => more bars). Rows that would fall
 *      outside the round screen are skipped.
 *   3. A TAP on a row switches to the DETAIL state (sel = that index).
 *   4. DETAIL state (sel >= 0): draw the device name large, its MAC address,
 *      its RSSI in dBm, and "tap to go back". A TAP anywhere returns to LIST.
 *
 * Data comes from hal_ble_scan() — a real passive scan on device (returns 0
 * for now), a fake list in the simulator. */
#include "apps.h"
#include "../font.h"
#include "../icons.h"
#include <stdio.h>
#include <string.h>

/* signal bars 0..4 from RSSI dBm (mirrors app_wifi.c's mapping) */
static int bars(int rssi)
{
    if (rssi >= -55) return 4;
    if (rssi >= -66) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

#ifndef SELFTEST

#define MAXBLE  12
#define ROW_H   GFX_S(40)
#define LIST_Y0 GFX_S(80)

typedef hal_ble_dev dev_t;   /* hal_ble_scan lives in hal.h */

static dev_t s_dev[MAXBLE];
static int   s_n;
static int   sel = -1;

static void on_start(void)
{
    sel = -1;
    s_n = hal_ble_scan(s_dev, MAXBLE);
    if (s_n < 0) s_n = 0;
}

/* Recognisable Bluetooth rune drawn from the standard 6-point path.
 * Half-height H = r, half-width W = r*5/8 (matches the classic 10x16 glyph). */
static void bt_glyph(gfx_fb *fb, float cx, float cy, float r, float wdt, uint16_t col)
{
    float H = r, W = r * 0.625f;
    float ax = cx - W, ay = cy - H * 0.5f;   /* A: left-upper   */
    float bx = cx + W, by = cy + H * 0.5f;   /* B: right-lower  */
    float ccx = cx,    ccy = cy + H;         /* C: bottom tip   */
    float dx = cx,     dy = cy - H;          /* D: top tip      */
    float ex = cx + W, ey = cy - H * 0.5f;   /* E: right-upper  */
    float fx = cx - W, fy = cy + H * 0.5f;   /* F: left-lower   */
    gfx_line(fb, ax, ay, bx, by, wdt, col);  /* A-B lower cross */
    gfx_line(fb, bx, by, ccx, ccy, wdt, col);/* B-C to bottom   */
    gfx_line(fb, ccx, ccy, dx, dy, wdt, col);/* C-D the spine   */
    gfx_line(fb, dx, dy, ex, ey, wdt, col);  /* D-E to right    */
    gfx_line(fb, ex, ey, fx, fy, wdt, col);  /* E-F upper cross */
}

static void draw_bars(gfx_fb *fb, float x, float y, int b)
{
    for (int i = 0; i < 4; i++) {
        float h = GFX_S(6 + i * 4);
        uint16_t c = i < b ? GFX_GREEN : GFX_DGREY;
        gfx_fill_rect(fb, (int)(x + i * GFX_S(8)), (int)(y - h),
                      (int)GFX_S(5), (int)h, c);
    }
}

static void on_draw(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);

    /* DETAIL view */
    if (sel >= 0 && sel < s_n) {
        dev_t *d = &s_dev[sel];
        const char *nm = d->name[0] ? d->name : "(unknown)";
        gfx_fill_circle(fb, GFX_CX, GFX_S(84), GFX_S(34), GFX_ACCENT);
        bt_glyph(fb, GFX_CX, GFX_S(84), GFX_S(20), GFX_S(4), GFX_WHITE);
        font_draw_centered(fb, &font_large, (int)GFX_CX, (int)GFX_S(146), nm, GFX_WHITE);
        font_draw_centered(fb, &font_medium, (int)GFX_CX, (int)GFX_S(176), d->addr, GFX_GREY);
        char b[24];
        snprintf(b, sizeof b, "%d dBm", d->rssi);
        font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(202), b, GFX_GREY);
        font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(224), "tap to go back", GFX_DGREY);
        return;
    }

    /* LIST view — header (small BT glyph + label) */
    int lw = font_text_width(&font_small, "BLUETOOTH");
    float gw = GFX_S(11), gap = GFX_S(7);
    float x0 = GFX_CX - (gw + gap + lw) * 0.5f;
    bt_glyph(fb, x0 + gw * 0.5f, GFX_S(34), GFX_S(7), GFX_S(2), GFX_ACCENT);
    font_draw(fb, &font_small, (int)(x0 + gw + gap),
              (int)(GFX_S(34) + font_small.ascent * 0.34f), "BLUETOOTH", GFX_GREY);

    if (s_n <= 0) {
        font_draw_centered(fb, &font_medium, (int)GFX_CX, (int)GFX_CY, "No devices", GFX_GREY);
        return;
    }

    char hdr[20];
    snprintf(hdr, sizeof hdr, "%d DEVICES", s_n);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(56), hdr, GFX_DGREY);

    for (int i = 0; i < s_n; i++) {
        float cy = LIST_Y0 + i * ROW_H;
        if (cy < GFX_S(66) || cy > GFX_H - GFX_S(34))
            continue;                       /* keep rows inside the circle */
        char nm[15];
        int k = 0;
        if (s_dev[i].name[0]) {
            for (; k < 14 && s_dev[i].name[k]; k++) nm[k] = s_dev[i].name[k];
        } else {
            const char *u = "(unknown)";
            for (; k < 14 && u[k]; k++) nm[k] = u[k];
        }
        nm[k] = 0;
        font_draw(fb, &font_small, (int)GFX_S(56),
                  (int)(cy + font_small.ascent * 0.34f), nm, GFX_WHITE);
        draw_bars(fb, GFX_W - GFX_S(82), cy + GFX_S(9), bars(s_dev[i].rssi));
    }
}

static int on_event(const wos_event *ev)
{
    if (ev->type != WOS_EV_TAP)
        return 0;

    if (sel >= 0) { sel = -1; return 1; }   /* detail -> back to list */

    for (int i = 0; i < s_n; i++) {
        float cy = LIST_Y0 + i * ROW_H;
        if (cy < GFX_S(66) || cy > GFX_H - GFX_S(34))
            continue;
        if (ev->y >= cy - ROW_H * 0.5f && ev->y < cy + ROW_H * 0.5f) {
            sel = i; return 1;
        }
    }
    return 1;
}

const wos_app app_ble = { "BLE", on_start, on_event, on_draw, GFX_ACCENT, ICON_BLUETOOTH };

#else  /* SELFTEST — verify the RSSI -> bars mapping */

int main(void)
{
    struct { int rssi, want; } t[] = {
        { -30, 4 }, { -55, 4 },          /* boundary: >= -55 => 4 */
        { -56, 3 }, { -66, 3 },          /* boundary: >= -66 => 3 */
        { -67, 2 }, { -75, 2 },          /* boundary: >= -75 => 2 */
        { -76, 1 }, { -85, 1 },          /* boundary: >= -85 => 1 */
        { -86, 0 }, { -120, 0 },         /* below => 0 */
    };
    int n = (int)(sizeof t / sizeof t[0]), fails = 0;
    for (int i = 0; i < n; i++) {
        int got = bars(t[i].rssi);
        int ok = got == t[i].want;
        if (!ok) fails++;
        printf("  bars(%4d) = %d  want %d  %s\n",
               t[i].rssi, got, t[i].want, ok ? "ok" : "FAIL");
    }
    printf("%s: %d/%d cases passed\n", fails ? "FAIL" : "PASS", n - fails, n);
    return fails ? 1 : 0;
}

#endif
