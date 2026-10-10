/* app_stack.c — Flat60 app list (RIGHT edge overlay when wos_ui_style == 1).
 *
 * Built to the Flat60 budget (docs/FLAT60_DESIGN.md): each app is one bold chip
 * (surface pill, solid accent disc, white glyph, label) rendered ONCE into a
 * PSRAM sprite. Scrolling only clears to black and blits the visible chips, so
 * a frame costs ~33 ns per moving pixel whatever the scroll speed, and the
 * dirty area is bounded by the chip column. Chips are centred and slide
 * rigidly; the round glass trims the top and bottom ones. */
#include "apps.h"
#include "../flat60.h"
#include "../font.h"
#include "../sprite.h"
#include <math.h>
#include <stdio.h>

#define CHIP_W     320
#define CHIP_H     76
#define CHIP_R     (CHIP_H / 2)
#define PITCH      90             /* row spacing: a 14 px black gap between chips */
#define DISC_R     28
#define MAX_APPS   40

/* drag / inertia feel, as the classic list */
#define OVER_RESIST 60.0f
#define OVER_MAX    90.0f
#define MOVE_DMAX   (GFX_H * 0.5f)
#define DT_FLOOR    8u
#define VEL_MAX     4000.0f

static sprite chips[MAX_APPS];
static sprite title;            /* "Apps" heading, one row above the first chip */
static int    chips_ready;       /* sprites rendered for this many apps */
static float  scroll, vel;       /* px; row i is centred at GFX_CY + i*PITCH - scroll */
static int    dragging, last_y;
static uint32_t last_t;

static float max_scroll(void) { return (wos_app_count() - 1) * (float)PITCH; }

/* Draw app i's chip into its sprite (once). */
static void render_chip(int i)
{
    gfx_fb fb = sprite_fb(&chips[i]);
    float cy = CHIP_H * 0.5f;
    gfx_clear(&fb, F60_BLACK);
    gfx_fill_pill(&fb, CHIP_R, cy, CHIP_W - CHIP_R, CHIP_R - 1, F60_SURFACE);
    float dcx = CHIP_R;                                   /* disc sits in the left cap */
    gfx_fill_circle(&fb, dcx, cy, DISC_R, f60_accent(wos_app_accent(i)));
    int icon = wos_app_icon(i);
    if (icon != ICON_NONE) icon_glyph(&fb, icon, dcx, cy, DISC_R * 0.62f, F60_WHITE);
    else {
        char init[2] = { wos_app_name(i)[0], 0 };
        if (init[0] >= 'a' && init[0] <= 'z') init[0] -= 32;
        font_draw_centered(&fb, &font_medium, (int)dcx, (int)(cy + font_medium.ascent * 0.34f), init, F60_WHITE);
    }
    char label[24];                                       /* registry names vary in case */
    snprintf(label, sizeof label, "%s", wos_app_name(i));
    if (label[0] >= 'a' && label[0] <= 'z') label[0] -= 32;
    font_draw(&fb, &font_medium, (int)(dcx + DISC_R + 16), (int)(cy + font_medium.ascent * 0.34f),
              label, F60_WHITE);
}

static void prepare_chips(void)
{
    if (!title.px && sprite_alloc(&title, CHIP_W, CHIP_H)) {
        gfx_fb fb = sprite_fb(&title);
        gfx_clear(&fb, F60_BLACK);
        font_draw_centered(&fb, &font_large, CHIP_W / 2, (int)(CHIP_H * 0.5f + font_large.ascent * 0.34f),
                           "Apps", F60_TEXT2);
    }
    int n = wos_app_count();
    if (n > MAX_APPS) n = MAX_APPS;
    for (int i = chips_ready; i < n; i++) {
        if (!chips[i].px && !sprite_alloc(&chips[i], CHIP_W, CHIP_H)) break;
        render_chip(i);
        chips_ready = i + 1;
    }
}

static void on_start(void)
{
    scroll = 0; vel = 0; dragging = 0;
    prepare_chips();
}

static void on_draw(gfx_fb *fb)
{
    if (fb->rec) { wos_stream_hint(); wos_flat_hint(); }   /* blits of palette chips */
    gfx_clear(fb, F60_BLACK);
    int n = chips_ready, focus = (int)(scroll / PITCH + 0.5f);
    /* rows within +-3 of the focus cover the whole glass */
    for (int i = focus - 3; i <= focus + 3; i++) {
        if (i < -1 || i >= n) continue;
        float cy = GFX_CY + i * (float)PITCH - scroll;
        int y = (int)floorf(cy - CHIP_H * 0.5f + 0.5f);
        if (y + CHIP_H <= 0 || y >= GFX_H) continue;
        sprite_blit(fb, i < 0 ? &title : &chips[i], (GFX_W - CHIP_W) / 2, y, (unsigned)(i + 1));
    }
}

static int row_at(int y)
{
    int i = (int)floorf((y - GFX_CY + scroll) / PITCH + 0.5f);
    float cy = GFX_CY + i * (float)PITCH - scroll;
    if (i < 0 || i >= wos_app_count() || fabsf(y - cy) > CHIP_H * 0.5f) return -1;
    return i;
}

static int on_event(const wos_event *ev)
{
    switch (ev->type) {
    case WOS_EV_TOUCH_DOWN:
        last_y = ev->y; dragging = 1; vel = 0; last_t = hal_millis();
        return 1;
    case WOS_EV_TOUCH_MOVE:
        if (dragging) {
            uint32_t now = hal_millis(), dt = now - last_t;
            if (dt < DT_FLOOR) dt = DT_FLOOR;
            float d = (float)(last_y - ev->y);
            if (d > MOVE_DMAX) d = MOVE_DMAX;
            else if (d < -MOVE_DMAX) d = -MOVE_DMAX;
            float m = max_scroll(), over = scroll < 0 ? -scroll : scroll > m ? scroll - m : 0;
            if (over > 0) d /= 1.0f + over / OVER_RESIST;     /* rubber band */
            scroll += d;
            if (scroll < -OVER_MAX) scroll = -OVER_MAX;
            else if (scroll > m + OVER_MAX) scroll = m + OVER_MAX;
            vel = 0.7f * (d * 1000.0f / dt) + 0.3f * vel;
            if (vel > VEL_MAX) vel = VEL_MAX;
            else if (vel < -VEL_MAX) vel = -VEL_MAX;
            last_y = ev->y; last_t = now;
        }
        return 1;
    case WOS_EV_TOUCH_UP:
        dragging = 0;                         /* momentum coasts, then snaps (TICK) */
        return 1;
    case WOS_EV_TAP: {
        int i = row_at(ev->y);
        if (i >= 0) { wos_open_app(i); return 1; }
        return 0;
    }
    case WOS_EV_TICK:
        if (!dragging) {
            float dt = ev->x / 1000.0f, m = max_scroll();
            scroll += vel * dt;
            vel *= 0.88f;
            if (scroll < 0)      { scroll += -scroll * 0.30f; vel = 0; }
            else if (scroll > m) { scroll += (m - scroll) * 0.30f; vel = 0; }
            else if (fabsf(vel) < 40.0f) {    /* settle the nearest chip onto the centre */
                float target = floorf(scroll / PITCH + 0.5f) * PITCH;
                scroll += (target - scroll) * 0.22f;
                if (fabsf(target - scroll) < 0.5f) scroll = target;
                vel = 0;
            }
        }
        return 1;
    default:
        return 0;
    }
}

const wos_app app_stack = { "stack", on_start, on_event, on_draw, F60_BLUE, ICON_NONE };
