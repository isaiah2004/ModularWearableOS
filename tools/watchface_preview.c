/* watchface_preview.c — render every face in the watchface gallery to
 * face0..10.bmp, by cycling with SWIPE_LEFT. No full sim needed.
 *
 *   gcc tools/watchface_preview.c core/apps/app_watchface.c \
 *       core/apps/face_dense.c core/apps/face_samsung.c core/apps/face_pixel.c \
 *       core/apps/face_nothing.c core/earth_sprite.c \
 *       core/gfx.c core/font.c core/icons.c core/layer.c core/fonts/*.c \
 *       -I core -o wf.exe -lm
 *   ./wf.exe        # writes face0.bmp .. face10.bmp (gallery order)
 */
#include "preview_stubs.h"

extern const wos_app app_watchface;

int main(void) {
    static uint16_t buf[GFX_W * GFX_H];
    gfx_fb fb = { .px = buf, .h = GFX_H };
    if (app_watchface.on_start) app_watchface.on_start();
    g_ms = 9000;   /* 10:09:09 showcase time */
    for (int f = 0; f < 11; f++) {
        app_watchface.on_draw(&fb);
        char nm[24]; snprintf(nm, sizeof nm, "face%d.bmp", f);
        pv_save(nm, buf);
        if (app_watchface.on_event) {
            wos_event e = { WOS_EV_SWIPE_LEFT, 0, 0 };
            app_watchface.on_event(&e);
        }
        g_ms += 2000;   /* advance so the face-name toast has faded before draw */
    }
    return 0;
}
