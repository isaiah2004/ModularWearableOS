/* Battery estimates are available only when the HAL has credible battery-
 * supply evidence. External power is not proof of charging or full charge. */
#include "apps.h"
#include "../font.h"
#include "../icons.h"
#include "../battery.h"
#include <stdio.h>

/* big number in digit-only font_big, with a small "%" tacked on the right */
static void draw_big_pct(gfx_fb *fb, int cx, int cy, int pct, uint16_t col)
{
    char n[6];
    snprintf(n, sizeof n, "%d", pct);
    int wn = font_text_width(&font_big, n);
    int wp = font_text_width(&font_medium, "%");
    int x  = cx - (wn + wp) / 2;
    x = font_draw(fb, &font_big, x, cy, n, col);
    font_draw(fb, &font_medium, x + (int)GFX_S(2), cy - (int)GFX_S(6), "%", col);
}

static void voltage_str(char *b, int n)
{
    int mv = battery_mv();
    if(mv>0) snprintf(b, n, "Rail %d.%02d V", mv / 1000, (mv % 1000) / 10);
    else snprintf(b,n,"Voltage unavailable");
}

static void on_draw(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(38),
                       "BATTERY", GFX_GREY);

    int state = battery_state();
    int pct   = battery_pct();
    char vstr[24];
    voltage_str(vstr, sizeof vstr);

    if (state == BATT_EXTERNAL || !battery_valid()) {
        int external=state==BATT_EXTERNAL;
        icon_glyph(fb, external ? ICON_FLASH : ICON_BATTERY,
                   GFX_CX, GFX_S(91), GFX_S(28), external ? GFX_GREEN : GFX_GREY);
        font_draw_centered(fb, &font_medium, (int)GFX_CX, (int)GFX_S(140),
                           external ? "On external power" : "Power state unknown",
                           external ? GFX_GREEN : GFX_GREY);
        font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(166),
                           "Battery estimate", GFX_GREY);
        font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(184),
                           "unavailable", GFX_GREY);
        font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(210),
                           vstr, GFX_WHITE);
        return;
    }

    uint16_t lvl = pct > 50 ? GFX_GREEN : pct > 20 ? GFX_ORANGE : GFX_RED;
    draw_big_pct(fb, (int)GFX_CX, (int)GFX_S(96), pct, GFX_WHITE);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(115),
                       "Approximate", GFX_GREY);

    /* battery gauge: bordered body + terminal nub, inner bar filled by level */
    float bw = GFX_S(132), bh = GFX_S(46), bt = GFX_S(3), pad = GFX_S(3);
    float bx = GFX_CX - bw / 2, by = GFX_S(128);
    uint16_t frame = GFX_RGB(120, 124, 132);
    gfx_fill_rect(fb, (int)bx, (int)by, (int)bw, (int)bh, frame);
    gfx_fill_rect(fb, (int)(bx + bt), (int)(by + bt),
                  (int)(bw - 2 * bt), (int)(bh - 2 * bt), GFX_BLACK);
    gfx_fill_rect(fb, (int)(bx + bw), (int)(by + bh * 0.30f),
                  (int)GFX_S(7), (int)(bh * 0.40f), frame);
    float fw = (bw - 2 * (bt + pad)) * (pct / 100.0f);
    if (fw > 1)
        gfx_fill_rect(fb, (int)(bx + bt + pad), (int)(by + bt + pad),
                      (int)fw, (int)(bh - 2 * (bt + pad)), lvl);

    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(192),
                       vstr, GFX_WHITE);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(210),
                       "On battery", GFX_GREY);
}

static int on_event(const wos_event *ev) { (void)ev; return 0; }

const wos_app app_battery = { "Battery", 0, on_event, on_draw, GFX_GREEN, ICON_BATTERY };
