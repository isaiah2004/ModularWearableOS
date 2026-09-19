/* Stopwatch: tap to start/stop, long-press to reset.
 * Exercises tap/long-press events, big tabular digits, and progress arc. */
#include "apps.h"
#include "../font.h"
#include <stdio.h>

#define PI 3.14159265f

static int s_running;
static uint32_t s_accum;    /* ms accumulated while stopped */
static uint32_t s_start_t;  /* hal_millis at last start */

static uint32_t elapsed(void)
{
    return s_running ? s_accum + (hal_millis() - s_start_t) : s_accum;
}

static int on_event(const wos_event *ev)
{
    if (ev->type == WOS_EV_TAP) {
        if (s_running) {
            s_accum += hal_millis() - s_start_t;
            s_running = 0;
        } else {
            s_start_t = hal_millis();
            s_running = 1;
        }
        return 1;
    }
    if (ev->type == WOS_EV_LONG_PRESS) {
        s_running = 0;
        s_accum = 0;
        return 1;
    }
    return 0;
}

static void on_draw(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);

    uint32_t ms = elapsed();
    unsigned cs = (ms / 10) % 100;
    unsigned s  = (ms / 1000) % 60;
    unsigned m  = (ms / 60000) % 100;

    /* seconds progress ring */
    gfx_ring(fb, GFX_CX, GFX_CY, GFX_S(106), GFX_S(3), GFX_DGREY);
    float frac = (ms % 60000) / 60000.0f;
    if (frac > 0.001f)
        /* gfx_sweep (settled + moving tip) instead of a single gfx_arc: the arc
         * grows every frame, and a plain arc re-dirties its whole wedge each
         * frame. Splitting keeps the dirty band to just the tip. Pixel-identical
         * (gfx_sweep is two gfx_arc calls); snap matches the neon watchface. */
        gfx_sweep(fb, GFX_CX, GFX_CY, GFX_S(106), GFX_S(3), frac * 2 * PI,
                  2 * PI / 60.0f, s_running ? GFX_ACCENT : GFX_GREY);

    char buf[16];
    snprintf(buf, sizeof buf, "%02u:%02u", m, s);
    font_draw_centered(fb, &font_big, (int)GFX_CX, (int)GFX_S(128), buf, GFX_WHITE);
    snprintf(buf, sizeof buf, ".%02u", cs);
    font_draw_centered(fb, &font_medium, (int)GFX_CX, (int)GFX_S(158), buf, GFX_GREY);

    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(66), "STOPWATCH", GFX_GREY);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(186),
                       s_running ? "TAP TO STOP" : "TAP START | HOLD RESET",
                       GFX_GREY);
}

const wos_app app_stopwatch = { "stopwatch", 0, on_event, on_draw, GFX_GREEN, ICON_STOPWATCH };
