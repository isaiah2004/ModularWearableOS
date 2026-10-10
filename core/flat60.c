/* flat60.c — Flat60 palette helpers and style switch (see flat60.h) */
#include "flat60.h"
#include "wos.h"
#include "apps/apps.h"

int wos_ui_style;

/* RGB565 -> 8-bit channels scaled so the brightest is 255 (hue direction only). */
static int hue_dir(uint16_t c, int out[3])
{
    int ch[3] = { (c >> 11) << 3, ((c >> 5) & 63) << 2, (c & 31) << 3 };
    int mx = ch[0] > ch[1] ? ch[0] : ch[1], mn = ch[0] < ch[1] ? ch[0] : ch[1];
    if (ch[2] > mx) mx = ch[2];
    if (ch[2] < mn) mn = ch[2];
    for (int k = 0; k < 3; k++) out[k] = mx ? ch[k] * 255 / mx : 0;
    return mx - mn;                                  /* chroma */
}

uint16_t f60_accent(uint16_t c)
{
    static const uint16_t pal[8] = { F60_RED, F60_ORANGE, F60_YELLOW, F60_GREEN,
                                     F60_TEAL, F60_BLUE, F60_PURPLE, F60_PINK };
    int h[3], p[3];
    if (hue_dir(c, h) < 40) return F60_TEXT2;        /* grey-ish: no hue to match */
    int best = 0, bestd = 1 << 30;
    for (int i = 0; i < 8; i++) {
        hue_dir(pal[i], p);
        int d = 0;
        for (int k = 0; k < 3; k++) d += (h[k] - p[k]) * (h[k] - p[k]);
        if (d < bestd) { bestd = d; best = i; }
    }
    return pal[best];
}

extern const wos_app app_stack;
extern int wos_fast_display;

void ui_style_apply(void)
{
    wos_register_overlay(WOS_EDGE_RIGHT, wos_ui_style ? &app_stack : &app_applist, 0);
    /* Flat60 is designed for the 8-bit wire format (its palette is RGB332-exact) */
    if (wos_ui_style && !wos_fast_display) wos_fast_display = 1;
}
