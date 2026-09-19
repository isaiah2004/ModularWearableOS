/* app_pomodoro.c — a focus timer on the classic Pomodoro rhythm: 25-min focus
 * sessions split by 5-min breaks, with a longer 15-min break after four. A big
 * depleting ring is the hero; the phase colours it (coral for focus, mint for
 * break), a tomato motif sits above the clock, and four dots track the cycle.
 *
 * Tap to start/pause, long-press to reset, swipe left to skip to the next phase.
 */
#include "apps.h"
#include "../font.h"
#include "../icons.h"
#include <stdio.h>

#define PI 3.14159265f
#define FOCUS_MS  (25u * 60u * 1000u)
#define SHORT_MS  ( 5u * 60u * 1000u)
#define LONG_MS   (15u * 60u * 1000u)
#define CYCLE     4                       /* focus sessions before a long break */

enum { PH_FOCUS, PH_SHORT, PH_LONG };

static int      s_phase;
static unsigned s_left;      /* ms remaining in the current phase */
static int      s_running;
static int      s_done;      /* focus sessions completed this cycle (0..CYCLE) */

static unsigned phase_ms(int p)   { return p == PH_FOCUS ? FOCUS_MS : p == PH_SHORT ? SHORT_MS : LONG_MS; }
static const char *phase_nm(int p){ return p == PH_FOCUS ? "FOCUS" : p == PH_SHORT ? "BREAK" : "LONG BREAK"; }
static uint16_t phase_col(int p)  { return p == PH_FOCUS ? GFX_RGB(255, 99, 71) : GFX_RGB(58, 205, 165); }

static void reset(void) { s_phase = PH_FOCUS; s_left = FOCUS_MS; s_running = 0; s_done = 0; }
static void on_start(void) { reset(); }

/* move to the next phase in the rhythm and buzz */
static void advance(void)
{
    if (s_phase == PH_FOCUS) {
        if (++s_done >= CYCLE) { s_phase = PH_LONG; s_done = 0; }
        else                     s_phase = PH_SHORT;
    } else {
        s_phase = PH_FOCUS;
    }
    s_left = phase_ms(s_phase);
    hal_haptic(35);
}

/* a small tomato: a red body with a couple of side lobes and a green leaf */
static void tomato(gfx_fb *fb, float cx, float cy, float r)
{
    uint16_t body = GFX_RGB(230, 66, 52), leaf = GFX_RGB(80, 180, 92);
    gfx_fill_circle(fb, cx - r * 0.34f, cy + r * 0.05f, r * 0.72f, body);
    gfx_fill_circle(fb, cx + r * 0.34f, cy + r * 0.05f, r * 0.72f, body);
    gfx_fill_circle(fb, cx, cy + r * 0.12f, r * 0.9f, body);
    gfx_fill_circle(fb, cx, cy - r * 0.62f, r * 0.32f, leaf);
    gfx_fill_circle(fb, cx - r * 0.34f, cy - r * 0.5f, r * 0.24f, leaf);
    gfx_fill_circle(fb, cx + r * 0.34f, cy - r * 0.5f, r * 0.24f, leaf);
}

static void on_draw(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);
    uint16_t col = phase_col(s_phase);

    /* hero ring — a soft wide glow under a bright depleting arc */
    float R = GFX_S(96), TH = GFX_S(9);
    float frac = (float)s_left / (float)phase_ms(s_phase);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    gfx_ring(fb, GFX_CX, GFX_CY, R, TH, GFX_RGB(32, 33, 39));
    if (frac > 0.002f) {
        gfx_arc(fb, GFX_CX, GFX_CY, R, TH + GFX_S(5), 0, frac * 2 * PI,
                gfx_blend565(GFX_BLACK, col, 70));      /* glow */
        gfx_arc(fb, GFX_CX, GFX_CY, R, TH, 0, frac * 2 * PI, col);
    }

    /* phase label + tomato + big clock */
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(70), phase_nm(s_phase), col);
    tomato(fb, GFX_CX, GFX_CY - GFX_S(36), GFX_S(11));

    char t[12];
    unsigned secs = (s_left + 999) / 1000;
    snprintf(t, sizeof t, "%u:%02u", secs / 60, secs % 60);
    font_draw_centered(fb, &font_big, (int)GFX_CX,
                       (int)(GFX_CY + font_big.ascent * 0.34f), t, GFX_WHITE);

    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(164),
                       s_running ? "tap to pause" : "tap to start", GFX_GREY);

    /* four cycle dots */
    float gap = GFX_S(18), x0 = GFX_CX - (CYCLE - 1) * gap / 2, y = GFX_S(178);
    for (int i = 0; i < CYCLE; i++)
        gfx_fill_circle(fb, x0 + i * gap, y, GFX_S(4),
                        i < s_done ? GFX_RGB(255, 99, 71) : GFX_RGB(58, 58, 66));
}

static int on_event(const wos_event *ev)
{
    switch (ev->type) {
    case WOS_EV_TICK:
        if (s_running && s_left > 0) {
            unsigned dt = (unsigned)ev->x;
            if (dt >= s_left) { s_left = 0; advance(); }
            else                s_left -= dt;
        }
        return 1;
    case WOS_EV_TAP:        s_running = !s_running; return 1;
    case WOS_EV_LONG_PRESS: reset(); hal_haptic(15); return 1;
    case WOS_EV_SWIPE_LEFT: s_left = 0; advance(); return 1;
    default:               return 0;
    }
}

const wos_app app_pomodoro = {
    "Pomodoro", on_start, on_event, on_draw, GFX_RGB(255, 99, 71), ICON_TIMER
};
