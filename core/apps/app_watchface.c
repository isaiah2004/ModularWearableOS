/* app_watchface.c — the home app, hosting a gallery of watch faces.
 * Swipe left/right to change face; the face name flashes briefly on switch.
 * Every face is drawn on the 240 design grid via GFX_S so it scales to any
 * panel. font_big is digits-only (0-9 : .), so faces use it for time and
 * font_medium/small for words. */
#include "apps.h"
#include "../font.h"
#include "../icons.h"
#include "../layer.h"
#include "../battery.h"
#include "../settings.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265f

/* Static-background cache tags (one per face that has a static layer).
 * Only one face is on screen at a time, so LAYER_MAX slots cover these. */
enum { LAY_CLASSIC = 0, LAY_DOTS, LAY_NEON, LAY_MINIMAL,
       LAY_CLASSIC_BASE = 5, LAY_NEON_BASE = 6 };

static const char *WDAY[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
static const char *MON[12] = { "JAN","FEB","MAR","APR","MAY","JUN",
                               "JUL","AUG","SEP","OCT","NOV","DEC" };

/* ---- shared helpers ---- */
static void hand(gfx_fb *fb, float ang, float r0, float r1, float w, uint16_t c)
{
    float s = sinf(ang), co = -cosf(ang);
    gfx_line(fb, GFX_CX - s * r0, GFX_CY - co * r0,
                 GFX_CX + s * r1, GFX_CY + co * r1, w, c);
}

static void digital(gfx_fb *fb, int hh, int mm, uint16_t col, int cy)
{
    char b[8];
    snprintf(b, sizeof b, "%02d:%02d", hh, mm);
    font_draw_centered(fb, &font_big, (int)GFX_CX,
                       (int)(cy + font_big.ascent * 0.34f), b, col);
}

/* Big time with an independently-coloured colon, centred on cx at baseline cy.
 * Draws "HH", the colon, then "MM" so the separator can carry the accent. */
static void digital_split(gfx_fb *fb, int hh, int mm, int cx, int cy,
                          uint16_t digit_col, uint16_t colon_col)
{
    char hb[4], mb[4];
    snprintf(hb, sizeof hb, "%02d", hh);
    snprintf(mb, sizeof mb, "%02d", mm);
    int wh = font_text_width(&font_big, hb);
    int wc = font_text_width(&font_big, ":");
    int wm = font_text_width(&font_big, mb);
    int total = wh + wc + wm;
    int x = cx - total / 2;
    x = font_draw(fb, &font_big, x, cy, hb, digit_col);
    x = font_draw(fb, &font_big, x, cy, ":", colon_col);
    font_draw(fb, &font_big, x, cy, mb, digit_col);
}

/* Letter-spaced small caps centred on cx (technical-label look). */
static void sc_caps(gfx_fb *fb, const font_t *f, int cx, int y,
                    const char *s, uint16_t col, float gap)
{
    int g = (int)GFX_S(gap);
    int total = 0;
    for (const char *c = s; *c; c++)
        total += font_text_width(f, (char[]){ *c, 0 }) + (c[1] ? g : 0);
    int x = cx - total / 2;
    for (const char *c = s; *c; c++) {
        char one[2] = { *c, 0 };
        x = font_draw(fb, f, x, y, one, col);
        if (c[1]) x += g;
    }
}

/* Filled rounded box centred at (cx,cy) with half-extents hw,hh, radius r. */
static void fill_round(gfx_fb *fb, float cx, float cy, float hw, float hh,
                       float r, uint16_t col)
{
    gfx_fill_rect(fb, (int)(cx - hw), (int)(cy - hh + r),
                  (int)(2 * hw), (int)(2 * (hh - r)), col);
    gfx_fill_rect(fb, (int)(cx - hw + r), (int)(cy - hh),
                  (int)(2 * (hw - r)), (int)(2 * hh), col);
    gfx_fill_circle(fb, cx - hw + r, cy - hh + r, r, col);
    gfx_fill_circle(fb, cx + hw - r, cy - hh + r, r, col);
    gfx_fill_circle(fb, cx - hw + r, cy + hh - r, r, col);
    gfx_fill_circle(fb, cx + hw - r, cy + hh - r, r, col);
}

/* Tapered hand from a tail counterweight to a pointed tip, AA via disc sweep.
 * Radii/widths are in grid units; angle 0 = 12 o'clock, clockwise. */
static void hand_taper(gfx_fb *fb, float ang, float r_tail, float r_tip,
                       float w_base, float w_tip, uint16_t col)
{
    float s = sinf(ang), co = -cosf(ang);
    float rt = GFX_S(r_tip), rb = GFX_S(r_tail);
    float hb = GFX_S(w_base) * 0.5f, ht = GFX_S(w_tip) * 0.5f;
    float htail = GFX_S(w_base) * 0.45f * 0.5f;
    float cx = GFX_CX, cy = GFX_CY;
    float tailx = cx - s * rb, taily = cy - co * rb;
    float tipx  = cx + s * rt, tipy  = cy + co * rt;
    /* two tapered segments (counterweight->hub, hub->tip) instead of a sweep of
     * ~100 fill_circles, so a hand is 2 damage commands not ~100. */
    gfx_taper(fb, tailx, taily, cx, cy, htail, hb, col);
    gfx_taper(fb, cx, cy, tipx, tipy, hb, ht, col);
}

/* Tapered hand with a dark edge underlay for a machined, premium read. */
static void hand_lux(gfx_fb *fb, float ang, float r_tail, float r_tip,
                     float w_base, float w_tip, uint16_t body, uint16_t edge)
{
    hand_taper(fb, ang, r_tail + 1.4f, r_tip + 1.0f, w_base + 2.0f, w_tip + 1.5f, edge);
    hand_taper(fb, ang, r_tail, r_tip, w_base, w_tip, body);
}

/* ---- faces ---- */

/* 0: Classic analog — ticks, battery arc, date, sweeping second hand */
static void face_classic(gfx_fb *fb, const hal_datetime *t)
{
    float span = 2 * PI / 6.0f;
    /* Static: black bg + graded tick ring + the battery-arc track + a fine
     * inner rail. None of it depends on time or battery, so cache it. */
    gfx_fb *bg = layer_need_render(LAY_CLASSIC);
    if (bg) {
        gfx_clear(bg, GFX_BLACK);
        gfx_ring(bg, GFX_CX, GFX_CY, GFX_S(101), GFX_S(1.0f), GFX_RGB(30, 30, 34));
        for (int i = 0; i < 60; i++) {
            float a = i * (2 * PI / 60.0f);
            int hr = (i % 5) == 0, q = (i % 15) == 0;
            float r0 = GFX_S(q ? 101 : (hr ? 104 : 110)), r1 = GFX_S(117);
            uint16_t tc = q ? GFX_WHITE : (hr ? GFX_RGB(150, 152, 158) : GFX_RGB(52, 52, 56));
            gfx_line(bg, GFX_CX + sinf(a) * r0, GFX_CY - cosf(a) * r0,
                         GFX_CX + sinf(a) * r1, GFX_CY - cosf(a) * r1,
                         GFX_S(q ? 3.4f : (hr ? 2.4f : 1.3f)), tc);
        }
        gfx_arc(bg, GFX_CX, GFX_CY, GFX_S(90), GFX_S(3.5f), -span, span, GFX_RGB(38, 38, 42));
    }
    /* Base = dial + battery + date + hour/minute hands, composited into its own
     * cached layer and re-rendered ONLY when the second ticks (the hands move
     * ~0.1deg/s — invisible). Each frame just blits this base, so the sweeping
     * second hand's damage band no longer re-rasterizes ~700 tapered discs. */
    int batt = hal_battery_percent();
    float sec = t->second + t->millis / 1000.0f;   /* smooth: second hand only */
    float min = t->minute + t->second / 60.0f;
    float hr = (t->hour % 12) + min / 60.0f;
    static int s_last = -1;
    if (t->second != s_last) { s_last = t->second; layer_invalidate(LAY_CLASSIC_BASE); }
    gfx_fb *base = layer_need_render(LAY_CLASSIC_BASE);
    if (base) {
        layer_blit(base, LAY_CLASSIC);             /* dial -> base */
        uint16_t bcol = batt <= 20 ? GFX_RED : GFX_ACCENT;
        gfx_arc(base, GFX_CX, GFX_CY, GFX_S(90), GFX_S(3.5f), -span,
                -span + 2 * span * (batt / 100.0f), bcol);
        char d[16];
        snprintf(d, sizeof d, "%d%%", batt);
        font_draw_centered(base, &font_small, (int)GFX_CX, (int)GFX_S(56), d, GFX_GREY);
        snprintf(d, sizeof d, "%s %d", WDAY[t->weekday % 7], t->day);
        sc_caps(base, &font_medium, (int)GFX_CX, (int)GFX_S(182), d, GFX_GREY, 1.5f);
        uint16_t edge = GFX_RGB(24, 24, 26);
        hand_lux(base, hr  * (2 * PI / 12.0f), 15, 56, 7.0f, 2.0f, GFX_WHITE, edge);
        hand_lux(base, min * (2 * PI / 60.0f), 17, 92, 5.2f, 1.6f, GFX_WHITE, edge);
    }
    layer_blit(fb, LAY_CLASSIC_BASE);

    /* slim sweeping second hand with a tail counterweight (live, every frame) */
    float sa = sec * (2 * PI / 60.0f);
    hand(fb, sa, GFX_S(24), GFX_S(101), GFX_S(1.8f), GFX_RED);
    gfx_fill_circle(fb, GFX_CX - sinf(sa) * GFX_S(24),
                    GFX_CY + cosf(sa) * GFX_S(24), GFX_S(4.5f), GFX_RED);
    gfx_fill_circle(fb, GFX_CX, GFX_CY, GFX_S(7.0f), GFX_RGB(40, 40, 44));
    gfx_fill_circle(fb, GFX_CX, GFX_CY, GFX_S(5.0f), GFX_RED);
    gfx_fill_circle(fb, GFX_CX, GFX_CY, GFX_S(2.2f), GFX_BLACK);
}

/* 1: Big bold digital */
static void face_bigdigital(gfx_fb *fb, const hal_datetime *t)
{
    gfx_clear(fb, GFX_BLACK);
    /* top: letter-spaced date, framed by two short accent rules */
    char d[20];
    snprintf(d, sizeof d, "%s %d %s", WDAY[t->weekday % 7], t->day, MON[(t->month - 1) % 12]);
    sc_caps(fb, &font_medium, (int)GFX_CX, (int)GFX_S(66), d, GFX_RGB(200, 202, 208), 1.5f);
    int dw = font_text_width(&font_medium, d) + (int)GFX_S(1.5f) * ((int)strlen(d) - 1);
    float ry = GFX_S(74), rx = dw / 2.0f + GFX_S(12);
    gfx_line(fb, GFX_CX - rx - GFX_S(14), ry, GFX_CX - rx, ry, GFX_S(2), GFX_ACCENT);
    gfx_line(fb, GFX_CX + rx, ry, GFX_CX + rx + GFX_S(14), ry, GFX_S(2), GFX_ACCENT);

    /* centre: big time with an accent colon */
    digital_split(fb, t->hour, t->minute, (int)GFX_CX,
                  (int)(GFX_S(122) + font_big.ascent * 0.34f), GFX_WHITE, GFX_ACCENT);

    /* bottom: slim seconds progress bar + tabular seconds readout */
    float sec = t->second + t->millis / 1000.0f;
    float bw = GFX_S(120), bx = GFX_CX - bw / 2.0f, by = GFX_S(180);
    fill_round(fb, GFX_CX, by, bw / 2.0f, GFX_S(1.6f), GFX_S(1.6f), GFX_RGB(34, 34, 38));
    float fw = bw * (sec / 60.0f);
    if (fw > GFX_S(3.2f))
        fill_round(fb, bx + fw / 2.0f, by, fw / 2.0f, GFX_S(1.6f), GFX_S(1.6f), GFX_ACCENT);
    char s[6];
    snprintf(s, sizeof s, "%02d", t->second);
    sc_caps(fb, &font_small, (int)GFX_CX, (int)GFX_S(202), s, GFX_GREY, 2.0f);
}

/* 2: Neon — a bright second-sweep ring around a digital core */
static void face_neon(gfx_fb *fb, const hal_datetime *t)
{
    /* Static: black bg + the recessed track ring with a faint inner rail. */
    gfx_fb *bg = layer_need_render(LAY_NEON);
    if (bg) {
        gfx_clear(bg, GFX_BLACK);
        gfx_ring(bg, GFX_CX, GFX_CY, GFX_S(112), GFX_S(7), GFX_RGB(26, 30, 28));
        gfx_ring(bg, GFX_CX, GFX_CY, GFX_S(112), GFX_S(1.0f), GFX_RGB(40, 46, 42));
    }
    layer_blit(fb, LAY_NEON);

    float sec = t->second + t->millis / 1000.0f;
    float a1 = sec / 60.0f * 2 * PI, snap = 2 * PI / 60.0f;
    /* bloom: 3 stacked halos -> bright filament, drawn live each frame via
     * gfx_sweep (settled + tip) so nothing ghosts. Trimmed from 5 to 3 layers
     * so the tip band re-rasterizes fewer thick arcs. */
    gfx_sweep(fb, GFX_CX, GFX_CY, GFX_S(112), GFX_S(13), a1, snap, GFX_RGB(20, 78, 50));
    gfx_sweep(fb, GFX_CX, GFX_CY, GFX_S(112), GFX_S(7),  a1, snap, GFX_RGB(40, 150, 92));
    gfx_sweep(fb, GFX_CX, GFX_CY, GFX_S(112), GFX_S(2),  a1, snap, GFX_RGB(170, 255, 205));
    /* glowing leading head */
    float hx = GFX_CX + sinf(a1) * GFX_S(112), hy = GFX_CY - cosf(a1) * GFX_S(112);
    gfx_fill_circle(fb, hx, hy, GFX_S(11), GFX_RGB(24, 90, 58));
    gfx_fill_circle(fb, hx, hy, GFX_S(6), GFX_RGB(150, 255, 190));
    gfx_fill_circle(fb, hx, hy, GFX_S(2.4f), GFX_WHITE);

    digital_split(fb, t->hour, t->minute, (int)GFX_CX,
                  (int)(GFX_S(112) + font_big.ascent * 0.34f), GFX_WHITE,
                  GFX_RGB(120, 255, 170));
    char d[12];
    snprintf(d, sizeof d, "%s %d", WDAY[t->weekday % 7], t->day);
    sc_caps(fb, &font_small, (int)GFX_CX, (int)GFX_S(158), d, GFX_GREEN, 2.5f);
}

/* 3: Sixty-dot ring, live second highlighted */
static void face_dots(gfx_fb *fb, const hal_datetime *t)
{
    /* Static: the 60 dot markers (white on 5s, grey otherwise). The live
     * orange "second" dot is drawn on top and fully covers whatever marker
     * sits beneath it, so the cached ring stays pixel-identical. */
    gfx_fb *bg = layer_need_render(LAY_DOTS);
    if (bg) {
        gfx_clear(bg, GFX_BLACK);
        for (int i = 0; i < 60; i++) {
            float a = i * (2 * PI / 60.0f);
            float x = GFX_CX + sinf(a) * GFX_S(108);
            float y = GFX_CY - cosf(a) * GFX_S(108);
            if (i % 15 == 0)
                gfx_fill_circle(bg, x, y, GFX_S(3.6f), GFX_WHITE);
            else if (i % 5 == 0)
                gfx_fill_circle(bg, x, y, GFX_S(2.6f), GFX_RGB(120, 120, 126));
            else
                gfx_fill_circle(bg, x, y, GFX_S(1.6f), GFX_RGB(48, 48, 52));
        }
    }
    layer_blit(fb, LAY_DOTS);

    /* live seconds dot with a soft halo — smooth sub-second motion */
    float sec = t->second + t->millis / 1000.0f;
    float a = sec * (2 * PI / 60.0f);
    float x = GFX_CX + sinf(a) * GFX_S(108), y = GFX_CY - cosf(a) * GFX_S(108);
    gfx_fill_circle(fb, x, y, GFX_S(9), GFX_RGB(70, 42, 10));
    gfx_fill_circle(fb, x, y, GFX_S(5.5f), GFX_ORANGE);
    gfx_fill_circle(fb, x, y, GFX_S(2.0f), GFX_RGB(255, 210, 150));

    digital(fb, t->hour, t->minute, GFX_WHITE, (int)(GFX_CY - GFX_S(8)));
    char d[12];
    snprintf(d, sizeof d, "%s %d", WDAY[t->weekday % 7], t->day);
    sc_caps(fb, &font_small, (int)GFX_CX, (int)(GFX_CY + GFX_S(30)), d, GFX_GREY, 2.0f);
}

/* 4: Modular — time up top, three framed gauge complications below */
static void mod_tile(gfx_fb *fb, float cx, float cy, const char *label,
                     const char *value, float frac, uint16_t accent)
{
    float hw = GFX_S(27), hh = GFX_S(29);
    fill_round(fb, cx, cy, hw, hh, GFX_S(9), GFX_RGB(19, 20, 24));
    sc_caps(fb, &font_small, (int)cx, (int)(cy - hh + GFX_S(14)), label, GFX_GREY, 1.0f);
    float ry = cy + GFX_S(7), r = GFX_S(15);
    gfx_ring(fb, cx, ry, r, GFX_S(2.4f), GFX_RGB(40, 42, 48));
    if (frac > 0.001f) gfx_arc(fb, cx, ry, r, GFX_S(2.4f), 0, frac * 2 * PI, accent);
    font_draw_centered(fb, &font_medium, (int)cx,
                       (int)(ry + font_medium.ascent * 0.34f), value, GFX_WHITE);
}

static void face_modular(gfx_fb *fb, const hal_datetime *t)
{
    gfx_clear(fb, GFX_BLACK);
    /* time header + a letter-spaced date band */
    digital(fb, t->hour, t->minute, GFX_WHITE, (int)GFX_S(56));
    char v[12];
    snprintf(v, sizeof v, "%s %d %s", WDAY[t->weekday % 7], t->day, MON[(t->month - 1) % 12]);
    sc_caps(fb, &font_small, (int)GFX_CX, (int)GFX_S(104), v, GFX_GREY, 2.0f);
    gfx_line(fb, GFX_CX - GFX_S(36), GFX_S(116), GFX_CX + GFX_S(36), GFX_S(116),
             GFX_S(1.0f), GFX_RGB(40, 42, 48));

    /* three gauge tiles */
    float off = GFX_S(58), cy = GFX_S(158);
    int batt = hal_battery_percent();
    snprintf(v, sizeof v, "%d", t->day);
    mod_tile(fb, GFX_CX - off, cy, WDAY[t->weekday % 7], v, t->day / 31.0f, GFX_ACCENT);
    snprintf(v, sizeof v, "%d", batt);
    mod_tile(fb, GFX_CX, cy, "BATT", v, batt / 100.0f, batt <= 20 ? GFX_RED : GFX_GREEN);
    snprintf(v, sizeof v, "%02d", t->second);
    mod_tile(fb, GFX_CX + off, cy, "SEC", v, t->second / 60.0f, GFX_ORANGE);
}

/* 5: Binary (BCD) clock — a maker classic, now legible + glowing */
static void bcd_col(gfx_fb *fb, float cx, int value, int bits,
                    uint16_t col, uint16_t glow, uint16_t hi)
{
    float r = GFX_S(9.5f), y0 = GFX_S(168), dy = GFX_S(33);
    for (int b = 0; b < bits; b++) {
        float y = y0 - b * dy;
        if ((value >> b) & 1) {
            gfx_fill_circle(fb, cx, y, r + GFX_S(3.0f), glow);
            gfx_fill_circle(fb, cx, y, r, col);
            gfx_fill_circle(fb, cx, y, r * 0.4f, hi);
        } else {
            gfx_fill_circle(fb, cx, y, r, GFX_RGB(22, 23, 27));
            gfx_ring(fb, cx, y, r, GFX_S(1.0f), GFX_RGB(46, 47, 53));
        }
    }
}

static void face_binary(gfx_fb *fb, const hal_datetime *t)
{
    gfx_clear(fb, GFX_BLACK);
    sc_caps(fb, &font_small, (int)GFX_CX, (int)GFX_S(32), "BINARY", GFX_GREY, 4.0f);

    /* bit-weight labels down the left edge (aligned to the 4 rows) */
    const char *wt[4] = { "1", "2", "4", "8" };
    for (int b = 0; b < 4; b++)
        font_draw_centered(fb, &font_small, (int)(GFX_CX - GFX_S(86)),
                           (int)(GFX_S(168) - b * GFX_S(33) + GFX_S(4)), wt[b],
                           GFX_RGB(70, 70, 76));

    uint16_t bglow = GFX_RGB(16, 46, 62), bhi = GFX_RGB(180, 235, 255);
    uint16_t gglow = GFX_RGB(14, 52, 32), ghi = GFX_RGB(170, 255, 205);
    bcd_col(fb, GFX_CX - GFX_S(60), t->hour / 10,   2, GFX_ACCENT, bglow, bhi);
    bcd_col(fb, GFX_CX - GFX_S(22), t->hour % 10,   4, GFX_ACCENT, bglow, bhi);
    bcd_col(fb, GFX_CX + GFX_S(22), t->minute / 10, 3, GFX_GREEN,  gglow, ghi);
    bcd_col(fb, GFX_CX + GFX_S(60), t->minute % 10, 4, GFX_GREEN,  gglow, ghi);

    /* group labels + decoded readout */
    sc_caps(fb, &font_small, (int)(GFX_CX - GFX_S(41)), (int)GFX_S(196), "HRS",
            GFX_ACCENT, 1.5f);
    sc_caps(fb, &font_small, (int)(GFX_CX + GFX_S(41)), (int)GFX_S(196), "MIN",
            GFX_GREEN, 1.5f);
    char b[8];
    snprintf(b, sizeof b, "%02d:%02d", t->hour, t->minute);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(214), b, GFX_RGB(90, 90, 96));
}

/* 6: Minimal — thin hands, four markers, nothing else */
static void face_minimal(gfx_fb *fb, const hal_datetime *t)
{
    /* Static: black bg + four slim cardinal markers (12 accented). */
    gfx_fb *bg = layer_need_render(LAY_MINIMAL);
    if (bg) {
        gfx_clear(bg, GFX_BLACK);
        for (int i = 0; i < 4; i++) {
            float a = i * (PI / 2);
            int top = (i == 0);
            float r0 = GFX_S(top ? 100 : 106), r1 = GFX_S(114);
            gfx_line(bg, GFX_CX + sinf(a) * r0, GFX_CY - cosf(a) * r0,
                         GFX_CX + sinf(a) * r1, GFX_CY - cosf(a) * r1,
                         GFX_S(top ? 3.0f : 2.0f),
                         top ? GFX_WHITE : GFX_RGB(70, 70, 76));
        }
    }
    layer_blit(fb, LAY_MINIMAL);

    float min = t->minute + t->second / 60.0f;
    float hr = (t->hour % 12) + min / 60.0f;
    uint16_t edge = GFX_RGB(20, 20, 22);
    hand_lux(fb, hr  * (2 * PI / 12.0f), 12, 54, 5.0f, 1.6f, GFX_WHITE, edge);
    hand_lux(fb, min * (2 * PI / 60.0f), 14, 92, 4.0f, 1.2f, GFX_ACCENT, edge);
    gfx_fill_circle(fb, GFX_CX, GFX_CY, GFX_S(5.5f), GFX_RGB(40, 40, 44));
    gfx_fill_circle(fb, GFX_CX, GFX_CY, GFX_S(3.2f), GFX_WHITE);
    gfx_fill_circle(fb, GFX_CX, GFX_CY, GFX_S(1.3f), GFX_BLACK);
}

/* ================= Apex: dense Apple-Infograph analog face =================
 * Everything-visual, information-rich: analog tapered hands over a fine tick
 * ring, two curved data gauges hugging the top bezel (battery + steps), a music
 * complication, three concentric activity rings, a moon-phase dial, and a date
 * pill. The static tick ring is cached; the rest is light enough to redraw. */
#define LAY_APEX 7

/* a tapered hand from the centre out to radius r (angle 0 = 12 o'clock, cw),
 * with a short counterweight tail behind the hub */
static void apex_hand(gfx_fb *fb, float a, float r, float tail,
                      float hw0, float hw1, uint16_t col)
{
    float s = sinf(a), c = -cosf(a);
    gfx_taper(fb, GFX_CX - s * tail, GFX_CY - c * tail,
                  GFX_CX + s * r,    GFX_CY + c * r, hw0, hw1, col);
}

/* a curved data gauge along the bezel: grey track + coloured fill for `frac` */
static void apex_gauge(gfx_fb *fb, float a0, float a1, float frac, uint16_t col)
{
    float r = GFX_S(109), th = GFX_S(6);
    gfx_arc(fb, GFX_CX, GFX_CY, r, th, a0, a1, GFX_RGB(34, 36, 42));
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    if (frac > 0.004f)
        gfx_arc(fb, GFX_CX, GFX_CY, r, th, a0, a0 + (a1 - a0) * frac, col);
}

/* three concentric Apple-style activity rings centred at (cx,cy) */
static void apex_rings(gfx_fb *fb, float cx, float cy, float f0, float f1, float f2)
{
    const float rr[3] = { GFX_S(19), GFX_S(13), GFX_S(7) };
    const uint16_t cc[3] = { GFX_RGB(250, 55, 95), GFX_RGB(120, 240, 90),
                             GFX_RGB(70, 200, 250) };
    const float ff[3] = { f0, f1, f2 };
    for (int i = 0; i < 3; i++) {
        gfx_ring(fb, cx, cy, rr[i], GFX_S(3.2f), GFX_RGB(28, 29, 33));
        float f = ff[i] > 1 ? 1 : ff[i];
        if (f > 0.004f)
            gfx_arc(fb, cx, cy, rr[i], GFX_S(3.2f), 0, f * 2 * PI, cc[i]);
    }
}

/* Everything except the hands, composed into the cached base layer. Redrawn
 * only when a value changes (once a second at most), so the per-frame cost is a
 * band blit of this cache plus the live hands. */
static void apex_base(gfx_fb *bg, int batt, int steps, float illum,
                      const char *dnum, const char *wd)
{
    gfx_clear(bg, GFX_BLACK);

    /* fine tick ring */
    for (int i = 0; i < 60; i++) {
        float a = i * (2 * PI / 60.0f);
        int hour = (i % 5) == 0;
        float r0 = GFX_S(hour ? 100 : 105), r1 = GFX_S(114);
        gfx_line(bg, GFX_CX + sinf(a) * r0, GFX_CY - cosf(a) * r0,
                     GFX_CX + sinf(a) * r1, GFX_CY - cosf(a) * r1,
                     GFX_S(hour ? 3.0f : 1.1f),
                     hour ? GFX_RGB(170, 175, 185) : GFX_RGB(60, 62, 70));
    }

    /* top bezel gauges: battery (left of 12), steps (right of 12) */
    apex_gauge(bg, -1.34f, -0.30f, batt / 100.0f, GFX_GREEN);
    apex_gauge(bg,  0.30f,  1.34f, steps / 10000.0f, GFX_ACCENT);

    /* date pill, upper-centre */
    float px = GFX_CX, py = GFX_CY - GFX_S(44);
    gfx_fill_circle(bg, px, py, GFX_S(20), GFX_RGB(20, 21, 25));
    font_draw_centered(bg, &font_small, (int)px, (int)(py - GFX_S(4)), wd, GFX_ORANGE);
    font_draw_centered(bg, &font_medium, (int)px, (int)(py + GFX_S(14)), dnum, GFX_WHITE);

    /* left: music complication */
    float lx = GFX_CX - GFX_S(50), ly = GFX_CY + GFX_S(4);
    gfx_ring(bg, lx, ly, GFX_S(20), GFX_S(3), GFX_RGB(250, 90, 170));
    icon_glyph(bg, ICON_MUSIC, lx, ly, GFX_S(12), GFX_WHITE);

    /* right: activity rings */
    apex_rings(bg, GFX_CX + GFX_S(50), GFX_CY + GFX_S(4),
               steps / 10000.0f, batt / 100.0f, 0.62f);

    /* bottom-centre: moon phase dial */
    float nx = GFX_CX, ny = GFX_CY + GFX_S(52);
    gfx_ring(bg, nx, ny, GFX_S(20), GFX_S(3), GFX_RGB(44, 48, 60));
    gfx_fill_circle(bg, nx, ny, GFX_S(13), GFX_RGB(210, 214, 220));
    gfx_fill_circle(bg, nx - GFX_S(13) + illum * 2 * GFX_S(13), ny,
                    GFX_S(13), GFX_RGB(18, 19, 24));
}

static void face_apex(gfx_fb *fb, const hal_datetime *t)
{
    static uint32_t s_fid = 0xFFFFFFFFu, s_vhash = 0xFFFFFFFFu;
    static int s_batt, s_steps;
    static float s_illum;
    static char s_dnum[4], s_wd[4];
    if (wos_frame_id != s_fid) {
        s_fid = wos_frame_id;
        s_batt = battery_pct();
        s_steps = hal_step_count();
        float mf = fmodf(t->day / 29.53f, 1.0f);      /* rough lunar phase */
        s_illum = 0.5f - 0.5f * cosf(mf * 2 * PI);
        snprintf(s_dnum, sizeof s_dnum, "%d", t->day);
        snprintf(s_wd, sizeof s_wd, "%s", WDAY[t->weekday % 7]);
        /* Recompose the cached base ONLY when a displayed value actually changes
         * (battery, ~50-step buckets, day, moon) — not every second. The old
         * once-a-second invalidate forced a full-screen redraw each second,
         * which is what made Apex's fps swing and flicker. Steps are quantised
         * so a step-by-step walk doesn't thrash the cache. */
        unsigned h = (unsigned)s_batt
                   + (unsigned)(s_steps / 50) * 131u
                   + (unsigned)t->day * 100003u
                   + (unsigned)(s_illum * 16.0f) * 7u;
        if (h != s_vhash) { s_vhash = h; layer_invalidate(LAY_APEX); }
    }

    gfx_fb *bg = layer_need_render(LAY_APEX);
    if (bg) apex_base(bg, s_batt, s_steps, s_illum, s_dnum, s_wd);
    layer_blit(fb, LAY_APEX);

    /* ---- analog hands over the composed base (live, ms-smooth) ---- */
    float hr = (t->hour % 12 + t->minute / 60.0f) * (2 * PI / 12.0f);
    float mn = (t->minute + t->second / 60.0f) * (2 * PI / 60.0f);
    float sc = (t->second + t->millis / 1000.0f) * (2 * PI / 60.0f);
    apex_hand(fb, hr, GFX_S(46), GFX_S(12), GFX_S(3.4f), GFX_S(1.6f), GFX_WHITE);
    apex_hand(fb, mn, GFX_S(78), GFX_S(14), GFX_S(3.0f), GFX_S(1.2f), GFX_WHITE);
    apex_hand(fb, sc, GFX_S(80), GFX_S(16), GFX_S(1.3f), GFX_S(1.0f), GFX_RGB(255, 120, 40));
    gfx_fill_circle(fb, GFX_CX, GFX_CY, GFX_S(5.5f), GFX_RGB(255, 120, 40));
    gfx_fill_circle(fb, GFX_CX, GFX_CY, GFX_S(2.6f), GFX_BLACK);
}

typedef void (*face_fn)(gfx_fb *, const hal_datetime *);
/* Interactive faces receive raw pointer and gesture events. Pip consumes
 * strokes in its eye zone; other center swipes still reach the gallery. */
static const struct {
    const char *name;
    face_fn draw;
    int (*event)(const wos_event *ev);
} FACES[] = {
    { "Apex",     face_apex, NULL },          /* the default flagship face */
    { "Pip",      face_pip_draw, face_pip_event },
    { "Infograph", face_dense_draw, NULL },
    { "Samsung",  face_samsung_draw, NULL },
    { "Ultra",    face_ultra_draw, NULL },
    { "Pixel",    face_pixel_draw, NULL },
    { "Nothing",  face_nothing_draw, NULL },
    { "Classic",  face_classic, NULL },
    { "Digital",  face_bigdigital, NULL },
    { "Neon",     face_neon, NULL },
    { "Dots",     face_dots, NULL },
    { "Modular",  face_modular, NULL },
    { "Binary",   face_binary, NULL },
    { "Minimal",  face_minimal, NULL },
};
#define NFACES ((int)(sizeof FACES / sizeof FACES[0]))

/* ================= face-switch transition =================================
 * A swipe runs a brief, latched animation instead of a hard cut:
 *   [0 .. TR_COVER)   the OUTGOING face eases to black — a fade veil ramps up.
 *   [TR_COVER .. 1)   the INCOMING face is revealed: it slides in from the swipe
 *                     direction while the veil eases back out (fade-from-black).
 * Everything is driven by hal_millis() latched to wos_frame_id so the record and
 * raster passes agree. A full-screen veil command (its hash tracks the alpha)
 * makes the OS repaint the whole face every frame the transition runs — brief
 * (~0.4s), so the full-redraw cost is fine AND every pixel the slide vacates is
 * guaranteed covered. */
#define TR_DUR_MS   400          /* total transition length */
#define TR_COVER    0.40f        /* fraction spent fading the old face to black */
#define TR_VCLR     0.45f        /* fraction of the REVEAL over which the veil clears */
#define TR_SLIDE    GFX_S(52)    /* distance the incoming face slides in, px */
#define TR_NAME_MS  1300u        /* name-flash total lifetime */

static uint32_t s_switch_ms;
static int s_from = -1;          /* face shown during the cover phase; -1 = idle */
static int s_dir;                /* +1 swipe-left (enter from right), -1 = right */

/* latched once per frame, read by both the record and the raster pass */
static int s_tr_face;            /* face index to draw this frame */
static int s_tr_ox;              /* horizontal slide offset, px */
static uint8_t s_tr_veil;        /* fade-to-black veil alpha (0 = none) */
static uint8_t s_tr_name;        /* name-flash brightness (0 = hidden) */

/* Uniform fade-to-black over the whole visible buffer. It works in screen space
 * (ignores any content slide) so it dims the incoming face and the vacated strip
 * alike. In record mode it emits one full-screen box whose hash follows alpha —
 * that is what forces the per-frame full redraw during the transition. */
static void draw_veil(gfx_fb *fb, uint8_t alpha)
{
    if (fb->rec) {
        gfx_rec_cmd(fb, 0, 0, GFX_W, GFX_H, gfx_hash_u(0xA1u, alpha));
        return;
    }
    if (alpha == 0) return;
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (c0 < 0) c0 = 0;
    if (c1 > GFX_W) c1 = GFX_W;
    for (int y = 0; y < fb->h; y++) {
        uint16_t *row = &fb->px[y * GFX_W];
        for (int x = c0; x < c1; x++)
            row[x] = gfx_blend565(row[x], GFX_BLACK, alpha);
    }
}

static void on_draw(gfx_fb *fb)
{
    /* Latch everything time-dependent ONCE per frame. on_draw runs twice per
     * frame — once to RECORD damage boxes, once to RASTERIZE — a few ms apart.
     * Re-reading the clock or the transition timer between the passes desyncs
     * them: a moving element lands where its damage box no longer covers, so a
     * residual sliver is left un-erased. One read per frame keeps them in sync. */
    static uint32_t s_fid = 0xFFFFFFFFu;
    static hal_datetime t;
    if (g_saved_face < 0 || g_saved_face >= NFACES) g_saved_face = 0;
    if (wos_frame_id != s_fid) {
        s_fid = wos_frame_id;
        hal_get_datetime(&t);

        uint32_t el = hal_millis() - s_switch_ms;
        s_tr_face = g_saved_face;
        s_tr_ox = 0;
        s_tr_veil = 0;
        if (s_from >= 0 && el < TR_DUR_MS) {
            float p = el / (float)TR_DUR_MS;
            if (p < TR_COVER) {
                /* cover: outgoing face eases to black (smoothstep ramp) */
                float c = p / TR_COVER;
                s_tr_face = s_from;
                s_tr_veil = (uint8_t)(c * c * (3.0f - 2.0f * c) * 255.0f);
            } else {
                /* reveal: the veil clears quickly (ease-out) so the incoming face
                 * is already visible while it is still gliding; the slide runs the
                 * whole reveal on a smoothstep, so it is the dominant, decelerating
                 * motion the eye follows into place from the swipe direction. */
                float r = (p - TR_COVER) / (1.0f - TR_COVER);
                float vr = r / TR_VCLR;
                if (vr > 1.0f) vr = 1.0f;
                float vinv = 1.0f - vr;
                float ve = 1.0f - vinv * vinv * vinv;      /* ease-out cubic */
                float ss = r * r * (3.0f - 2.0f * r);      /* smoothstep slide */
                s_tr_face = g_saved_face;
                s_tr_ox = (int)(s_dir * TR_SLIDE * (1.0f - ss));
                s_tr_veil = (uint8_t)((1.0f - ve) * 255.0f);
            }
        } else if (s_from >= 0) {
            s_from = -1;                               /* transition complete */
        }

        /* Name flash: fades in with the reveal, holds, fades out — one continuous
         * gesture with the transition rather than a hard 1.2s pop. */
        float nb;
        if (el < 150u)            nb = 0.0f;
        else if (el < 450u)       nb = (el - 150u) / 300.0f;
        else if (el < 1050u)      nb = 1.0f;
        else if (el < TR_NAME_MS) nb = 1.0f - (el - 1050u) / (float)(TR_NAME_MS - 1050u);
        else                      nb = 0.0f;
        s_tr_name = (uint8_t)(nb * 255.0f);
    }

    int face = s_tr_face;
    if (face < 0 || face >= NFACES) face = g_saved_face;

    if (s_tr_veil || s_tr_ox) {
        /* Lay a full-screen black base first: the strip the slide vacates isn't
         * touched by the face's own translated clear/blit, so this keeps it from
         * showing stale pixels. Then draw the face translated, then the veil. */
        gfx_fill_rect(fb, 0, 0, GFX_W, GFX_H, GFX_BLACK);
        int sox = fb->ox;
        fb->ox += s_tr_ox;
        FACES[face].draw(fb, &t);
        fb->ox = sox;
        draw_veil(fb, s_tr_veil);
    } else {
        FACES[face].draw(fb, &t);
    }

    if (s_tr_name) {
        uint16_t col = GFX_RGB(s_tr_name, s_tr_name, s_tr_name);
        font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(224),
                           FACES[g_saved_face].name, col);
    }
}

static void face_next(void) { g_saved_face = (g_saved_face + 1) % NFACES; settings_save(); }
static void face_prev(void) { g_saved_face = (g_saved_face + NFACES - 1) % NFACES; settings_save(); }

static int on_event(const wos_event *ev)
{
    /* Pip consumes strokes beginning over its eyes. Other swipes, including
     * those below the eyes, retain gallery navigation. Edges remain OS-owned. */
    if (g_saved_face < 0 || g_saved_face >= NFACES) g_saved_face = 0;
    if (s_from < 0 && FACES[g_saved_face].event &&
        FACES[g_saved_face].event(ev))
        return 1;

    /* Face swaps use the OS scan-fade crossfade (new face sweeps in from the swipe direction),
     * then flash the face name as a toast. */
    if (ev->type == WOS_EV_SWIPE_LEFT) {
        wos_app_crossfade(0, -1, face_next);    /* new face enters from the right */
        wos_callout(FACES[g_saved_face].name, 1100);
        return 1;
    }
    if (ev->type == WOS_EV_SWIPE_RIGHT) {
        wos_app_crossfade(0, +1, face_prev);    /* new face enters from the left */
        wos_callout(FACES[g_saved_face].name, 1100);
        return 1;
    }
    return 0;
}

const wos_app app_watchface = { "watchface", 0, on_event, on_draw, GFX_ACCENT, ICON_CLOCK };
