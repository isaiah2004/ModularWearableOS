/* app_timer.c — Countdown Timer.
 *
 * PLAIN-ENGLISH ALGORITHM
 * -----------------------
 * Two states, held in s_state:
 *
 *   ST_PICK  (preset picker)
 *     - Four tappable discs are laid out 2x2: 1, 3, 5, 10 minutes.
 *     - TAP inside a disc -> load that duration and switch to ST_RUN,
 *       running immediately (s_paused = 0, s_done_fired = 0).
 *     - Other events ignored.
 *
 *   ST_RUN   (counting down / paused / done)
 *     - Big MM:SS sits in the center; an orange progress RING depletes
 *       from a full circle to empty as the remaining time falls.
 *     - WOS_EV_TICK carries ev->x = ms since the last frame. While
 *       running (not paused) and time remains, subtract ev->x from
 *       s_remaining. Clamp at 0.
 *     - Reaching 0 is DONE: fire hal_haptic(400) exactly once (guarded by
 *       s_done_fired) and flash the screen. The flash toggles on a
 *       ~350 ms wall-clock cadence via hal_millis().
 *     - TAP: if time remains, toggle pause/resume; if DONE, return to the
 *       picker.
 *     - LONG_PRESS: always return to the picker.
 *
 * TIMING MATH
 * -----------
 *   remaining is milliseconds (long). Display uses CEIL seconds so the
 *   last whole second shows as 00:01 right up until it hits 00:00:
 *       total_sec = (remaining + 999) / 1000
 *       MM = total_sec / 60,  SS = total_sec % 60
 *   Ring fraction = remaining / total, clamped to [0,1]; the foreground
 *   arc spans frac * 2*PI clockwise from 12 o'clock, so a full timer is a
 *   full ring and an expired timer is empty.
 */
#include "apps.h"
#include "../font.h"
#include "../icons.h"
#include <stdio.h>

#define PI 3.14159265f

/* ---- pure logic (compiled always, exercised by SELFTEST) ---------------- */

/* Format a millisecond count as MM:SS using ceil-seconds. */
static void fmt_mmss(long ms, char *buf, int n)
{
    if (ms < 0) ms = 0;
    long total_sec = (ms + 999) / 1000;   /* ceil to the next whole second */
    long mm = total_sec / 60;
    long ss = total_sec % 60;
    snprintf(buf, n, "%02ld:%02ld", mm, ss);
}

/* Depleting ring fraction in [0,1]: 1 = full time left, 0 = expired. */
static float ring_frac(long remaining, long total)
{
    if (total <= 0) return 0.0f;
    if (remaining < 0) remaining = 0;
    if (remaining > total) remaining = total;
    return (float)remaining / (float)total;
}

#ifndef SELFTEST

/* ---- app state ---------------------------------------------------------- */

enum { ST_PICK = 0, ST_RUN };

static const int   s_preset_min[4] = { 1, 3, 5, 10 };
/* single accent per screen: every preset ring shares the timer's orange,
 * matching the running countdown ring (numbers alone distinguish presets). */
static const uint16_t s_preset_col[4] = { GFX_ORANGE, GFX_ORANGE,
                                          GFX_ORANGE, GFX_ORANGE };

static int  s_state;       /* ST_PICK / ST_RUN */
static int  s_paused;      /* in ST_RUN: 1 = held, 0 = counting */
static int  s_done_fired;  /* haptic fired for this countdown */
static long s_total;       /* ms of the chosen preset */
static long s_remaining;   /* ms left */

/* Center of preset disc i (2x2 grid), in panel pixels. */
static void preset_center(int i, float *cx, float *cy)
{
    *cx = (i % 2) ? GFX_S(152) : GFX_S(88);
    *cy = (i / 2) ? GFX_S(156) : GFX_S(100);
}

#define PRESET_R GFX_S(27)

static void start_preset(int i)
{
    s_total     = (long)s_preset_min[i] * 60000L;
    s_remaining = s_total;
    s_paused    = 0;
    s_done_fired = 0;
    s_state     = ST_RUN;
}

/* ---- events ------------------------------------------------------------- */

static int on_event(const wos_event *ev)
{
    if (ev->type == WOS_EV_TICK) {
        if (s_state == ST_RUN && !s_paused && s_remaining > 0) {
            s_remaining -= ev->x;
            if (s_remaining <= 0) {
                s_remaining = 0;
                if (!s_done_fired) {
                    hal_play(SND_TIMER);           /* chiptune "time's up" chime */
                    s_done_fired = 1;
                }
            }
        }
        return 1;
    }

    if (s_state == ST_PICK) {
        if (ev->type == WOS_EV_TAP) {
            for (int i = 0; i < 4; i++) {
                float cx, cy;
                preset_center(i, &cx, &cy);
                float dx = ev->x - cx, dy = ev->y - cy;
                if (dx * dx + dy * dy <= PRESET_R * PRESET_R) {
                    start_preset(i);
                    return 1;
                }
            }
        }
        return 0;
    }

    /* ST_RUN */
    if (ev->type == WOS_EV_LONG_PRESS) {
        s_state = ST_PICK;
        return 1;
    }
    if (ev->type == WOS_EV_TAP) {
        if (s_remaining <= 0)
            s_state = ST_PICK;      /* DONE -> back to the picker */
        else
            s_paused = !s_paused;   /* pause / resume */
        return 1;
    }
    return 0;
}

/* ---- drawing ------------------------------------------------------------ */

static void draw_pick(gfx_fb *fb)
{
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(48),
                       "TIMER", GFX_GREY);

    for (int i = 0; i < 4; i++) {
        float cx, cy;
        preset_center(i, &cx, &cy);
        gfx_fill_circle(fb, cx, cy, PRESET_R, GFX_DGREY);
        gfx_ring(fb, cx, cy, PRESET_R - GFX_S(2), GFX_S(3), s_preset_col[i]);

        char n[8];
        snprintf(n, sizeof n, "%d", s_preset_min[i]);
        font_draw_centered(fb, &font_large, (int)cx,
                           (int)(cy + GFX_S(9)), n, GFX_WHITE);
    }

    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(198),
                       "MINUTES", GFX_GREY);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(212),
                       "TAP A PRESET", GFX_GREY);
}

static void draw_run(gfx_fb *fb)
{
    int done = (s_remaining <= 0);
    float r_mid = GFX_S(104), thick = GFX_S(6);

    gfx_ring(fb, GFX_CX, GFX_CY, r_mid, thick, GFX_DGREY);

    if (done) {
        /* full ring + flashing DONE */
        int on = (hal_millis() / 350) % 2;
        uint16_t c = on ? GFX_ORANGE : GFX_WHITE;
        gfx_arc(fb, GFX_CX, GFX_CY, r_mid, thick, 0, 2 * PI, GFX_ORANGE);
        font_draw_centered(fb, &font_large, (int)GFX_CX, (int)GFX_S(132),
                           "DONE", c);
        font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(190),
                           "TAP FOR PRESETS", GFX_GREY);
        return;
    }

    float frac = ring_frac(s_remaining, s_total);
    if (frac > 0.0005f)
        /* gfx_sweep instead of gfx_arc: the depleting ring's end angle changes
         * every frame, so a single arc re-dirties its whole wedge each frame.
         * Splitting into settled + tip keeps the dirty band to just the tip.
         * Pixel-identical output (gfx_sweep == two gfx_arc calls). */
        gfx_sweep(fb, GFX_CX, GFX_CY, r_mid, thick, frac * 2 * PI,
                  2 * PI / 60.0f, GFX_ORANGE);

    char buf[16];
    fmt_mmss(s_remaining, buf, sizeof buf);
    font_draw_centered(fb, &font_big, (int)GFX_CX, (int)GFX_S(132), buf,
                       s_paused ? GFX_GREY : GFX_WHITE);

    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(66),
                       "TIMER", GFX_GREY);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(190),
                       s_paused ? "PAUSED | HOLD TO RESET"
                                : "TAP PAUSE | HOLD RESET",
                       GFX_GREY);
}

static void on_draw(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);
    if (s_state == ST_PICK) draw_pick(fb);
    else                    draw_run(fb);
}

const wos_app app_timer = { "Timer", 0, on_event, on_draw,
                            GFX_ORANGE, ICON_TIMER };

#endif /* !SELFTEST */

/* ---- self-test ---------------------------------------------------------- */
#ifdef SELFTEST
#include <assert.h>
#include <string.h>
#include <math.h>

static int close_f(float a, float b) { return fabsf(a - b) < 0.0005f; }

int main(void)
{
    char b[16];

    /* MM:SS formatting, ceil-seconds semantics */
    fmt_mmss(0, b, sizeof b);        assert(strcmp(b, "00:00") == 0);
    fmt_mmss(1, b, sizeof b);        assert(strcmp(b, "00:01") == 0);
    fmt_mmss(999, b, sizeof b);      assert(strcmp(b, "00:01") == 0);
    fmt_mmss(1000, b, sizeof b);     assert(strcmp(b, "00:01") == 0);
    fmt_mmss(1001, b, sizeof b);     assert(strcmp(b, "00:02") == 0);
    fmt_mmss(59000, b, sizeof b);    assert(strcmp(b, "00:59") == 0);
    fmt_mmss(60000, b, sizeof b);    assert(strcmp(b, "01:00") == 0);
    fmt_mmss(300000, b, sizeof b);   assert(strcmp(b, "05:00") == 0);
    fmt_mmss(299001, b, sizeof b);   assert(strcmp(b, "05:00") == 0);  /* ceil */
    fmt_mmss(299000, b, sizeof b);   assert(strcmp(b, "04:59") == 0);
    fmt_mmss(600000, b, sizeof b);   assert(strcmp(b, "10:00") == 0);
    fmt_mmss(-5, b, sizeof b);       assert(strcmp(b, "00:00") == 0);

    /* ring fraction: full -> empty, clamped */
    assert(close_f(ring_frac(600000, 600000), 1.0f));
    assert(close_f(ring_frac(300000, 600000), 0.5f));
    assert(close_f(ring_frac(0,      600000), 0.0f));
    assert(close_f(ring_frac(-100,   600000), 0.0f));   /* clamp low  */
    assert(close_f(ring_frac(999999, 600000), 1.0f));   /* clamp high */
    assert(close_f(ring_frac(100,    0),      0.0f));   /* no divzero */

    printf("app_timer self-test: PASS\n");
    return 0;
}
#endif
