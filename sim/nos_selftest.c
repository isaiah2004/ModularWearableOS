/* nos_selftest.c — a debug root screen for exercising the simulator's tools when no
 * real apps are registered (nos_sim.exe --selftest, or automatically when nothing
 * defines nos_apps_boot()). Uses only gfx/font/sprite primitives so it does not
 * depend on unfinished NOS modules.
 *
 *   root:  a dot orbiting the centre (continuous damage), a seconds counter that
 *          ticks once a second, a disc that follows the finger while dragging.
 *          tap = push the list screen (TX_PUSH), long press = toast,
 *          air gesture = banner, vertical drag up = pull the list in (TX_RISE).
 *   list:  12 pills scrolled by a momentum scroller; side button / swipe back pops. */
#include <stdio.h>
#include <math.h>
#include "nos.h"
#include "motion.h"
#include "font.h"
#include "hal_ext.h"
#include "ui.h"

/* ---- root --------------------------------------------------------------------- */
static float s_ang;
static int   s_fx = -1, s_fy = -1;
static int   s_taps;
/* `set selftest_bug 1`: a deliberate damage bug for testing --damagecheck. The dot is
 * drawn in raster mode but never recorded, so damage tracking leaves stale copies. */
int nos_selftest_bug;

/* `set selftest_spin RAD_PER_S` and `set selftest_smear 0..3` (none, 1 ghost, 2 ghosts,
 * stretch): a fast small object to judge the R7 smear helpers (ui_smear_disc/stretch). */
float nos_selftest_spin = 1.2f;
int nos_selftest_smear;
static ui_trail s_trail;
static void root_tick(float dt)
{
    s_ang += dt * nos_selftest_spin;
    ui_trail_push(&s_trail, GFX_CX + 200 * sinf(s_ang), GFX_CY - 200 * cosf(s_ang));
    nos_motion_probe("selftest.dot.x", MC_DECORATIVE, s_trail.x[0]);
}

static void root_draw(gfx_fb *fb)
{
    hal_datetime dt;
    hal_get_datetime(&dt);
    gfx_ring(fb, GFX_CX, GFX_CY, 200, 3, 0x2104);
    if (nos_selftest_smear == 3) ui_smear_stretch(fb, &s_trail, 9, 0xD0C4, 16);
    else if (nos_selftest_smear) ui_smear_disc(fb, &s_trail, 9, 0xD0C4, 16, nos_selftest_smear);
    gfx_fill_circle(fb, s_trail.x[0], s_trail.y[0], 9, 0xD0C4);
    char t[16];
    snprintf(t, sizeof t, "%02d:%02d", dt.hour, dt.minute);
    font_draw_centered(fb, &font_large, (int)GFX_CX, 220, t, 0xEF5D);
    snprintf(t, sizeof t, "%02d", dt.second);
    font_draw_centered(fb, &font_medium, (int)GFX_CX, 290, t, 0x9CD3);
    snprintf(t, sizeof t, "TAPS %d", s_taps);
    font_draw_centered(fb, &font_small, (int)GFX_CX, 350, t, 0x632C);
    gfx_fill_pill(fb, GFX_CX - 60, 120, GFX_CX + 60, 14, 0x18C3);
    if (s_fx >= 0) gfx_fill_circle(fb, (float)s_fx, (float)s_fy, 28, 0xFFFF);
    if (nos_selftest_bug && !fb->rec) gfx_fill_circle(fb, 120.0f + (float)(nos_frame * 4 % 220), 405, 8, 0xFFFF);
}

static const nos_screen st_list;
static int root_input(const nos_input *in)
{
    switch (in->type) {
    case IN_TAP: s_taps++; nos_iris_x = in->x; nos_iris_y = in->y; nos_push(&st_list, TX_PUSH); return 1;
    case IN_LONG: nos_toast("LONG PRESS"); return 1;
    case IN_AIR: nos_banner("Air gesture", in->x == GEST_LEFT ? "left" : in->x == GEST_RIGHT ? "right" :
                            in->x == GEST_UP ? "up" : "down / other", 0, 0); return 1;
    case IN_DRAG_START:
        if (in->axis == AXIS_Y && in->dy < 0 && in->y0 > 330) { nos_pull(&st_list, TX_RISE, in); return 1; }
        s_fx = in->x; s_fy = in->y; return 1;
    case IN_DRAG: s_fx = in->x; s_fy = in->y; return 1;
    case IN_DRAG_END: case IN_UP: s_fx = s_fy = -1; return 1;
    default: return 0;
    }
}

const nos_screen nos_selftest_root = {
    "selftest", 0, 0, root_tick, root_draw, root_input, SCR_NOBACK
};

/* ---- list ---------------------------------------------------------------------- */
#define ROWS 12
#define ROW_H 84
static mscroll s_sc;
static int s_sc_init;

static void list_enter(void)
{
    if (!s_sc_init) { msc_init(&s_sc, 0, (float)(ROWS * ROW_H - GFX_H + 120), ROW_H); s_sc_init = 1; }
}
static void list_tick(float dt) { msc_step(&s_sc, dt); }
static void list_draw(gfx_fb *fb)
{
    static const char *names[ROWS] = { "Timer", "Stopwatch", "Alarm", "Weather", "Compass", "Heart",
                                       "Steps", "Music", "Torch", "Recorder", "Settings", "About" };
    int off = (int)lroundf(s_sc.x);
    for (int i = 0; i < ROWS; i++) {
        int cy = 120 + i * ROW_H - off;
        if (cy < -60 || cy > GFX_H + 60) continue;
        gfx_fill_pill(fb, 110, (float)cy, GFX_W - 110, 32, i == 3 ? 0xD0C4 : 0x18C3);
        font_draw_centered(fb, &font_small, (int)GFX_CX, cy + 11, names[i], 0xEF5D);
    }
}
static int list_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN: msc_grab(&s_sc); return 1;
    case IN_DRAG_START: if (in->axis != AXIS_Y) return 0; msc_drag(&s_sc, -in->dy); return 1;
    case IN_DRAG: if (in->axis == AXIS_Y) msc_drag(&s_sc, -in->dy); return 1;
    case IN_DRAG_END: if (in->axis == AXIS_Y) msc_release(&s_sc, -in->vy); return 1;
    case IN_UP: if (!s_sc.moving && !s_sc.springing) msc_release(&s_sc, 0); return 1;
    case IN_TAP: nos_toast("TAP"); return 1;
    default: return 0;
    }
}
static const nos_screen st_list = {
    "selftest list", list_enter, 0, list_tick, list_draw, list_input, 0
};
