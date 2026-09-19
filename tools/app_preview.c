/* app_preview.c — render one app (its on_draw) to preview0..3.bmp without the
 * full sim. Compile with the app's wos_app symbol in PREVIEW_APP:
 *
 *   gcc tools/app_preview.c core/gfx.c core/font.c core/icons.c core/layer.c \
 *       core/fonts/*.c core/apps/app_weather.c -DPREVIEW_APP=app_weather \
 *       -I core -o prev.exe -lm
 *   ./prev.exe     # writes preview0..3.bmp (4 frames across time for animation)
 */
#include "preview_stubs.h"

extern const wos_app PREVIEW_APP;

int main(void) {
    static uint16_t buf[GFX_W * GFX_H];
    gfx_fb fb = { .px = buf, .h = GFX_H };
    if (PREVIEW_APP.on_start) PREVIEW_APP.on_start();
    uint32_t times[4] = { 0, 3000, 12000, 40000 };
    uint32_t prev = 0;
    for (int k = 0; k < 4; k++) {
        g_ms = times[k];
        if (PREVIEW_APP.on_event) {
            wos_event e = { WOS_EV_TICK, (int)(times[k] - prev), 0 };
            PREVIEW_APP.on_event(&e);
        }
        prev = times[k];
        PREVIEW_APP.on_draw(&fb);
        char nm[24]; snprintf(nm, sizeof nm, "preview%d.bmp", k);
        pv_save(nm, buf);
    }
    return 0;
}
