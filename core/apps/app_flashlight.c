/* Flashlight / Torch: turn the whole AMOLED into a solid light panel.
 *
 * PLAIN-ENGLISH ALGORITHM
 *   State is a single mode index that TAP advances through a 4-step cycle:
 *       WHITE  -> WARM -> RED -> OFF -> (wraps back to WHITE)
 *   Each frame we simply flood-fill the entire screen with that mode's
 *   colour (OFF = black), so there is nothing to animate and no flicker.
 *   The panel backlight is driven per mode: full brightness (255) for the
 *   three lit modes, and a low level (20) for OFF so the black screen goes
 *   properly dim instead of glowing dark-grey.
 *   On first entry we show a subtle "TAP TO CHANGE" hint near the bottom.
 *   A per-frame TICK accumulates elapsed ms; the hint stays solid for a
 *   couple of seconds then fades out (by lerping its colour toward the
 *   background) so it never spoils the torch once you know the gesture.
 */
#include "apps.h"
#include "../font.h"

/* mode order for the TAP cycle */
enum { MODE_WHITE = 0, MODE_WARM, MODE_RED, MODE_OFF, MODE_COUNT };

/* hint timing (ms): fully visible, then fade to nothing */
#define HINT_HOLD_MS  1800
#define HINT_FADE_MS  1400
#define HINT_TOTAL_MS (HINT_HOLD_MS + HINT_FADE_MS)

static int      s_mode;
static uint32_t s_hint_ms;   /* ms since the hint started showing */

static uint16_t mode_color(int m)
{
    switch (m) {
        case MODE_WHITE: return GFX_WHITE;
        case MODE_WARM:  return GFX_RGB(255, 220, 150);
        case MODE_RED:   return GFX_RED;
        default:         return GFX_BLACK;   /* MODE_OFF */
    }
}

/* apply the backlight for a mode: bright when lit, dim when off */
static void apply_backlight(int m)
{
    hal_set_backlight(m == MODE_OFF ? 20 : 255);
}

static void on_start(void)
{
    s_mode    = MODE_WHITE;
    s_hint_ms = 0;            /* show the hint from the top */
    apply_backlight(s_mode);
}

static int on_event(const wos_event *ev)
{
    if (ev->type == WOS_EV_TAP) {
        s_mode = (s_mode + 1) % MODE_COUNT;
        apply_backlight(s_mode);
        return 1;
    }
    if (ev->type == WOS_EV_TICK) {
        /* ev->x = ms since last frame; cap the accumulator so it can't wrap */
        if (s_hint_ms < HINT_TOTAL_MS) {
            uint32_t dt = (ev->x > 0) ? (uint32_t)ev->x : 0;
            s_hint_ms += dt;
            if (s_hint_ms > HINT_TOTAL_MS) s_hint_ms = HINT_TOTAL_MS;
        }
        return 1;
    }
    return 0;
}

static void on_draw(gfx_fb *fb)
{
    uint16_t bg = mode_color(s_mode);
    gfx_clear(fb, bg);

    if (s_hint_ms >= HINT_TOTAL_MS)
        return;   /* hint has fully faded — pure torch */

    /* alpha: 255 while holding, then linearly down to 0 across the fade */
    uint8_t alpha = 255;
    if (s_hint_ms > HINT_HOLD_MS) {
        uint32_t into = s_hint_ms - HINT_HOLD_MS;   /* 0..HINT_FADE_MS */
        alpha = (uint8_t)(255 - (into * 255) / HINT_FADE_MS);
    }

    /* pick a hint ink that contrasts with the current background, then fade
     * it by blending toward the background as alpha drops */
    uint16_t ink = (s_mode == MODE_WHITE || s_mode == MODE_WARM)
                       ? GFX_RGB(60, 60, 60)     /* dark ink on bright panels */
                       : GFX_RGB(210, 210, 210); /* light ink on red / black  */
    uint16_t col = gfx_blend565(bg, ink, alpha);

    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(198),
                       "TAP TO CHANGE", col);
}

const wos_app app_flashlight = { "Torch", on_start, on_event, on_draw,
                                 GFX_YELLOW, ICON_FLASHLIGHT };
