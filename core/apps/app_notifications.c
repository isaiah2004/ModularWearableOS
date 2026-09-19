/* app_notifications.c — notification shade (TOP edge overlay). Renders the
 * notification store as cards. The store is filled over BLE (iPhone ANCS or
 * the companion app) on device; the simulator seeds a couple so the UI is
 * visible. Swipe up or tap empty to dismiss. */
#include "apps.h"
#include "../notif.h"
#include "../font.h"
#include <string.h>

static void on_draw(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);
    int n = notif_count();
    if (n == 0) {
        font_draw_centered(fb, &font_medium, (int)GFX_CX, (int)GFX_CY,
                           "No Notifications", GFX_GREY);
        return;
    }
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(34),
                       "NOTIFICATIONS", GFX_GREY);
    float y = GFX_S(52);
    float cardw = GFX_S(190), x = GFX_CX - cardw / 2;
    for (int i = 0; i < n; i++) {
        const notif_t *m = notif_get(i);
        float h = GFX_S(52);
        gfx_fill_rect(fb, (int)x, (int)y, (int)cardw, (int)h, GFX_DGREY);
        gfx_fill_rect(fb, (int)x, (int)y, (int)GFX_S(4), (int)h, m->accent);
        font_draw(fb, &font_small, (int)(x + GFX_S(14)),
                  (int)(y + GFX_S(20)), m->app, m->accent);
        font_draw(fb, &font_small, (int)(x + GFX_S(14)),
                  (int)(y + GFX_S(40)), m->title, GFX_WHITE);
        y += h + GFX_S(10);
    }
}

static int on_event(const wos_event *ev)
{
    return 0;   /* any tap dismisses */
}

const wos_app app_notifications = { "notif", 0, on_event, on_draw, GFX_RED, ICON_BELL };
