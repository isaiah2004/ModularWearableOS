/* app_wifi.c — Wi-Fi dashboard. OBSERVE-ONLY: passive scan + monitor-mode listening.
 * There are no attack features here (no deauth, no injection, no forged frames) and there
 * never should be — this is recon you run on your own air.
 *
 * Three pages, swipe LEFT/RIGHT to cycle:
 *   NETWORKS  — nearby APs from hal_wifi_scan() (beacons only), tap a row for detail.
 *   CHANNELS  — live per-channel airtime bars from promiscuous mode. A scan can only ever
 *               show you beacons; monitor mode hears EVERY frame, so this is the page that
 *               tells you which channel is actually congested.
 *   HEAT MAP  — the same utilisation as a scrolling waterfall (channel x time).
 *
 * Monitor mode owns the radio (it hops channels), so it only runs while one of the two
 * analyzer pages is open; leaving the page — or the app — hands the station link back.
 */
#include "apps.h"
#include "../font.h"
#include "../icons.h"
#include <stdio.h>
#include <string.h>

#define MAXAP 12
#define ROW_H   GFX_S(40)
#define LIST_Y0 GFX_S(80)

typedef hal_wifi_ap ap_t;   /* hal_wifi_scan lives in hal.h */

enum { PG_APS = 0, PG_CHAN, PG_HEAT, PG_N };
static int s_page;

static ap_t s_ap[MAXAP];
static int s_n;
static float scroll;
static int last_y, dragging;
static int sel = -1;

/* air-analyzer snapshot, refreshed once per FRAME (on_draw runs once per stripe) */
static hal_air_chan s_air[HAL_AIR_NCH];
static uint8_t      s_hist[HAL_AIR_NCH * HAL_AIR_HIST];
static uint32_t     s_air_frame = 0xffffffffu;

static void air_refresh(void)
{
    if (s_air_frame == wos_frame_id) return;
    s_air_frame = wos_frame_id;
    hal_wifi_air_get(s_air);          /* also the keep-alive for the HAL auto-release */
    hal_wifi_air_hist(s_hist);
}

static void rescan(void)
{
    s_n = hal_wifi_scan(s_ap, MAXAP);
    if (s_n < 0) s_n = 0;
}

static void on_start(void)
{
    scroll = 0; sel = -1; dragging = 0; s_page = PG_APS;
    rescan();
}

/* entering/leaving the analyzer pages arms/releases monitor mode */
static void set_page(int p)
{
    int was_air = (s_page == PG_CHAN || s_page == PG_HEAT);
    int now_air = (p == PG_CHAN || p == PG_HEAT);
    s_page = p;
    if (now_air && !was_air)      hal_wifi_air_start();
    else if (was_air && !now_air) { hal_wifi_air_stop(); rescan(); }
}

/* signal bars 0..4 from RSSI dBm */
static int bars(int rssi)
{
    if (rssi >= -55) return 4;
    if (rssi >= -66) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
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

/* utilisation -> heat colour: black -> blue -> green -> yellow -> red */
static uint16_t heat(uint8_t v)
{
    if (v < 8)   return GFX_RGB(12, 12, 16);
    if (v < 64)  return GFX_RGB(0, 0, 60 + v * 3);
    if (v < 128) return GFX_RGB(0, (v - 64) * 4, 255 - (v - 64) * 3);
    if (v < 192) return GFX_RGB((v - 128) * 4, 255, 0);
    return GFX_RGB(255, 255 - (v - 192) * 4, 0);
}

/* shared plot box for the two analyzer pages */
#define PLOT_X0 GFX_S(26)
#define PLOT_X1 GFX_S(214)
#define CH_W    ((PLOT_X1 - PLOT_X0) / (float)HAL_AIR_NCH)

static void draw_ch_labels(gfx_fb *fb, float y)
{
    for (int c = 0; c < HAL_AIR_NCH; c++) {
        char b[4];
        snprintf(b, sizeof b, "%d", c + 1);
        int hot = (c + 1 == 1 || c + 1 == 6 || c + 1 == 11);   /* non-overlapping trio */
        font_draw_centered(fb, &font_small, (int)(PLOT_X0 + (c + 0.5f) * CH_W),
                           (int)y, b, hot ? GFX_GREY : GFX_DGREY);
    }
}

static void draw_page_dots(gfx_fb *fb)
{
    for (int i = 0; i < PG_N; i++)
        gfx_fill_circle(fb, GFX_CX + (i - (PG_N - 1) / 2.0f) * GFX_S(14),
                        GFX_H - GFX_S(14), GFX_S(3),
                        i == s_page ? GFX_WHITE : GFX_DGREY);
}

static void draw_channels(gfx_fb *fb)
{
    air_refresh();
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(30), "CHANNELS", GFX_GREY);

    int cur = hal_wifi_air_channel(), busiest = 0, quietest = 0;
    long tot = 0;
    for (int c = 0; c < HAL_AIR_NCH; c++) {
        tot += s_air[c].pkts;
        if (s_air[c].util > s_air[busiest].util)  busiest = c;
        if (s_air[c].util < s_air[quietest].util) quietest = c;
    }
    char hdr[36];
    if (!hal_wifi_air_running()) snprintf(hdr, sizeof hdr, "starting monitor...");
    else snprintf(hdr, sizeof hdr, "listening ch %d", cur);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(52), hdr, GFX_ACCENT);

    float base = GFX_S(172), maxh = GFX_S(100);
    gfx_line(fb, PLOT_X0, base, PLOT_X1, base, GFX_S(1), GFX_DGREY);
    for (int c = 0; c < HAL_AIR_NCH; c++) {
        float h = maxh * (s_air[c].util / 255.0f);
        if (h < GFX_S(2)) h = GFX_S(2);
        float x = PLOT_X0 + c * CH_W + CH_W * 0.18f;
        float w = CH_W * 0.64f;
        gfx_fill_rect(fb, (int)x, (int)(base - h), (int)w, (int)h, heat(s_air[c].util));
        if (c + 1 == cur)   /* the channel we're sniffing right now */
            gfx_fill_rect(fb, (int)x, (int)(base + GFX_S(3)), (int)w, (int)GFX_S(2), GFX_WHITE);
        /* how many APs the last beacon scan put on this channel */
        int aps = 0;
        for (int i = 0; i < s_n; i++) if (s_ap[i].chan == c + 1) aps++;
        if (aps) {
            char ab[8];
            snprintf(ab, sizeof ab, "%d", aps > 99 ? 99 : aps);
            font_draw_centered(fb, &font_small, (int)(x + w / 2),
                               (int)(base - h - GFX_S(4)), ab, GFX_ACCENT);
        }
    }
    draw_ch_labels(fb, base + GFX_S(18));

    char ft[64];
    snprintf(ft, sizeof ft, "clearest ch %d   busiest ch %d", quietest + 1, busiest + 1);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(208), ft, GFX_GREY);
    draw_page_dots(fb);
}

static void draw_heat(gfx_fb *fb)
{
    air_refresh();
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(30), "HEAT MAP", GFX_GREY);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(50),
                       "airtime  ch x time", GFX_DGREY);

    float y0 = GFX_S(58), rowh = GFX_S(9.5f);
    for (int c = 0; c < HAL_AIR_NCH; c++) {
        float x = PLOT_X0 + c * CH_W;
        for (int r = 0; r < HAL_AIR_HIST; r++) {
            uint8_t v = s_hist[c * HAL_AIR_HIST + r];
            gfx_fill_rect(fb, (int)(x + GFX_S(1)), (int)(y0 + r * rowh),
                          (int)(CH_W - GFX_S(1.5f)), (int)(rowh - GFX_S(1)), heat(v));
        }
    }
    draw_ch_labels(fb, y0 + HAL_AIR_HIST * rowh + GFX_S(14));

    /* legend: quiet -> busy. Kept narrow + centred so nothing falls off the round edge. */
    float lw = GFX_S(100), lx = GFX_CX - lw / 2, ly = GFX_S(196);
    for (int i = 0; i < 20; i++)
        gfx_fill_rect(fb, (int)(lx + i * (lw / 20)), (int)ly,
                      (int)(lw / 20 + 1), (int)GFX_S(6), heat((uint8_t)(i * 255 / 19)));
    font_draw_centered(fb, &font_small, (int)(GFX_CX - GFX_S(66)),
                       (int)(ly + GFX_S(6)), "quiet", GFX_DGREY);
    font_draw_centered(fb, &font_small, (int)(GFX_CX + GFX_S(66)),
                       (int)(ly + GFX_S(6)), "busy", GFX_DGREY);
    draw_page_dots(fb);
}

static void draw_aps(gfx_fb *fb)
{
    if (sel >= 0 && sel < s_n) {
        ap_t *a = &s_ap[sel];
        gfx_fill_circle(fb, GFX_CX, GFX_S(88), GFX_S(34), GFX_ACCENT);
        icon_glyph(fb, ICON_WIFI, GFX_CX, GFX_S(88), GFX_S(21), GFX_WHITE);
        font_draw_centered(fb, &font_large, (int)GFX_CX, (int)GFX_S(148), a->ssid, GFX_WHITE);
        char b[32];
        snprintf(b, sizeof b, "CH %d   %d dBm", a->chan, a->rssi);
        font_draw_centered(fb, &font_medium, (int)GFX_CX, (int)GFX_S(182), b, GFX_GREY);
        font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(210), "tap to go back", GFX_DGREY);
        return;
    }

    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(34), "WI-FI", GFX_GREY);
    char hdr[24];
    snprintf(hdr, sizeof hdr, "%d NETWORKS", s_n);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(56), hdr, GFX_DGREY);
    if (!s_n)
        font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(120),
                           "scanning... (2.4GHz only)", GFX_DGREY);

    for (int i = 0; i < s_n; i++) {
        float cy = LIST_Y0 + i * ROW_H - scroll;
        if (cy < GFX_S(66) || cy > GFX_H - GFX_S(40))
            continue;
        char ssid[15];
        int k = 0;
        for (; k < 14 && s_ap[i].ssid[k]; k++) ssid[k] = s_ap[i].ssid[k];
        ssid[k] = 0;
        font_draw(fb, &font_small, (int)GFX_S(56),
                  (int)(cy + font_small.ascent * 0.34f), ssid, GFX_WHITE);
        draw_bars(fb, GFX_W - GFX_S(82), cy + GFX_S(9), bars(s_ap[i].rssi));
    }
    draw_page_dots(fb);
}

static void on_draw(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);
    if      (s_page == PG_CHAN) draw_channels(fb);
    else if (s_page == PG_HEAT) draw_heat(fb);
    else                        draw_aps(fb);
}

static int on_event(const wos_event *ev)
{
    switch (ev->type) {
    case WOS_EV_SWIPE_LEFT:  set_page((s_page + 1) % PG_N); return 1;
    case WOS_EV_SWIPE_RIGHT: set_page((s_page + PG_N - 1) % PG_N); return 1;
    case WOS_EV_TOUCH_DOWN:
        last_y = ev->y; dragging = 1;
        return 1;
    case WOS_EV_TOUCH_MOVE:
        if (dragging && sel < 0 && s_page == PG_APS) {
            scroll += last_y - ev->y;
            last_y = ev->y;
            float m = s_n * ROW_H - GFX_H * 0.5f;
            if (m < 0) m = 0;
            if (scroll < 0) scroll = 0;
            if (scroll > m) scroll = m;
        }
        return 1;
    case WOS_EV_TOUCH_UP:
        dragging = 0;
        return 1;
    case WOS_EV_TAP:
        if (s_page != PG_APS) return 1;
        if (sel >= 0) { sel = -1; return 1; }         /* back to list */
        for (int i = 0; i < s_n; i++) {
            float cy = LIST_Y0 + i * ROW_H - scroll;
            if (ev->y >= cy - ROW_H * 0.5f && ev->y < cy + ROW_H * 0.5f) {
                sel = i; return 1;
            }
        }
        return 1;
    default:
        return 0;
    }
}

const wos_app app_wifi = { "Wi-Fi", on_start, on_event, on_draw, GFX_ACCENT, ICON_WIFI };
