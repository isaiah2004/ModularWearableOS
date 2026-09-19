/* app_findphone.c — Find My Phone (ring-your-phone concept UI).
 *
 * HOW THE REAL THING WOULD WORK:
 *   On a shipping build this app does NOT make any sound itself — the watch
 *   is not the noisy end. Tapping "ring" sends a small BLE command over the
 *   watch<->phone link to the companion app on the paired PHONE, which then
 *   plays a loud locate-me tone at full volume (even if the phone is on
 *   silent), the same way Apple's / Google's "Find My" ping works. The phone
 *   would also ACK so we could show "Found it / phone answered". That BLE
 *   channel and companion app are NOT wired yet (see HAL wish at bottom), so
 *   this build is purely the WATCH-SIDE UI + animation: a delightful, clearly
 *   "ringing" screen that stands in for the real ping. The only local output
 *   we can actually drive here is the haptic motor, which we pulse in rhythm.
 *
 * ---------------------------------------------------------------------------
 * PLAIN-ENGLISH ALGORITHM
 *
 *   Two-state machine, driven by TAP and TICK (ev->x = ms since last frame):
 *
 *   STATE IDLE
 *     - A procedurally drawn phone glyph (rounded-rect body + a speaker slit
 *       near the top + a round home dot near the bottom) sits centred, gently
 *       breathing inside a soft accent halo.
 *     - Labels: "FIND MY PHONE" up top and "Tap to ring" below.
 *     - TAP -> start ringing (fire one haptic; would send the BLE ping).
 *
 *   STATE RINGING  (auto-stops after RING_TIMEOUT_MS = 15000 ms)
 *     - Emanating SOUND WAVES: N_RING concentric rings expand outward from the
 *       phone and fade as they grow. Ring k's phase is
 *           ph = frac( t_ms / RING_PERIOD_MS + k/N_RING )   in [0,1)
 *       so the rings are evenly staggered; its radius grows linearly
 *           radius = R0 + ph*SPREAD
 *       and its brightness = (1-ph), with a short fade-IN for small ph so a
 *       ring never "pops" into existence at the centre. Composited on black
 *       with gfx_blend565 (via dim()) for smooth AMOLED falloff.
 *     - The phone glyph in the middle BUZZES (a fast horizontal sine jitter),
 *       and a little "sound" indicator (three nested arcs opening rightward,
 *       brightness chasing each other) sits beside it — the classic ringer mark.
 *     - Labels: "Ringing your phone" up top, and a big red STOP button (pill)
 *       near the bottom.
 *     - RHYTHMIC HAPTIC: we accumulate elapsed ms; a beat index = elapsed/BEAT_MS
 *       ticks up once per BEAT_MS (700 ms). Whenever the beat index increases we
 *       fire a gentle hal_haptic() pulse, so the watch buzzes in time with the
 *       ring animation.
 *     - AUTO-STOP: when elapsed >= RING_TIMEOUT_MS we fall back to IDLE on our
 *       own (so we don't nag forever). A TAP anywhere while ringing (the STOP
 *       button is the affordance) also stops immediately and returns to IDLE.
 *
 *   The interesting/branch-y logic — the auto-stop cutoff, the ring phase/radius
 *   growth, and the beat counter — lives in pure helpers (ring_active_after,
 *   ring_phase, ring_radius, beat_index) that compile into both the app and the
 *   SELFTEST build below and are asserted there.
 * ---------------------------------------------------------------------------
 */
#include "apps.h"
#include "../font.h"
#include <stdio.h>
#ifndef SELFTEST
#include <math.h>
#endif

#define PI 3.14159265f

/* ---- state machine + timing (visible to app + selftest) ---------------- */
enum { ST_IDLE = 0, ST_RING };

#define RING_TIMEOUT_MS 15000u   /* auto-stop ringing after this long   */
#define BEAT_MS           700u   /* one rhythmic haptic pulse per beat   */
#define RING_PERIOD_MS   1300    /* ms for a wave to cross the full spread */
#define N_RING              4    /* number of staggered emanating rings   */

/* ===================== pure helpers (self-tested) ======================= */

/* fractional part of a non-negative float, without <math.h> (frac01(2.7)=0.7).
 * All our inputs (time in ms / period) are >= 0, so a truncating cast is safe. */
static float frac01(float v) { return v - (float)(int)v; }

/* Should we still be ringing? Auto-stop once we've been ringing long enough.
 * Returns the state we should now be in given the current one + elapsed time.
 * IDLE never auto-changes (it waits for a TAP). */
static int ring_active_after(int state, unsigned elapsed_ms)
{
    if (state == ST_RING && elapsed_ms >= RING_TIMEOUT_MS) return ST_IDLE;
    return state;
}

/* Phase in [0,1) of emanating ring k at absolute time t_ms. Rings are evenly
 * staggered by k/N so a fresh one leaves the centre every RING_PERIOD_MS/N. */
static float ring_phase(uint32_t t_ms, int k, int n)
{
    float v = (float)t_ms / (float)RING_PERIOD_MS + (float)k / (float)n;
    return frac01(v);
}

/* Radius of a ring at phase p: grows linearly from r0 out to r0+spread. */
static float ring_radius(float r0, float spread, float p)
{
    return r0 + p * spread;
}

/* Which rhythmic beat are we on? Increments once per BEAT_MS; used to decide
 * when to fire the next haptic pulse. */
static unsigned beat_index(unsigned elapsed_ms) { return elapsed_ms / BEAT_MS; }

#ifndef SELFTEST
/* ===================== app state ======================================= */

static int      s_state;
static unsigned s_elapsed_ms;   /* time accumulated while RINGING */
static unsigned s_last_beat;    /* last beat index we buzzed on   */

/* ===================== small drawing helpers =========================== */

/* accent (or any colour) at brightness a in [0,1], composited on black */
static uint16_t dim(uint16_t col, float a)
{
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    return gfx_blend565(GFX_BLACK, col, (uint8_t)(a * 255.0f + 0.5f));
}

/* filled rounded-rect centred at (cx,cy) with half-extents hw,hh (rects + 4
 * corner discs) — used for the phone body, its screen, and the STOP pill */
static void draw_round_rect(gfx_fb *fb, float cx, float cy,
                            float hw, float hh, float r, uint16_t col)
{
    gfx_fill_rect(fb, (int)(cx - hw), (int)(cy - hh + r),
                  (int)(2 * hw), (int)(2 * (hh - r)), col);
    gfx_fill_rect(fb, (int)(cx - hw + r), (int)(cy - hh),
                  (int)(2 * (hw - r)), (int)(2 * hh), col);
    gfx_fill_circle(fb, cx - hw + r, cy - hh + r, r, col);
    gfx_fill_circle(fb, cx + hw - r, cy - hh + r, r, col);
    gfx_fill_circle(fb, cx - hw + r, cy + hh - r, r, col);
    gfx_fill_circle(fb, cx + hw - r, cy + hh - r, r, col);
}

/* procedural phone glyph centred at (cx,cy): rounded body, inset dark screen,
 * a speaker slit in the top bezel, a round home dot in the bottom bezel. */
static void draw_phone(gfx_fb *fb, float cx, float cy, float scale)
{
    float hw = GFX_S(19) * scale;   /* body half-width  (taller than wide) */
    float hh = GFX_S(31) * scale;   /* body half-height                    */
    float r  = GFX_S(7)  * scale;   /* body corner radius                  */
    float bz = GFX_S(7)  * scale;   /* bezel thickness top & bottom        */
    float bx = GFX_S(3)  * scale;   /* bezel thickness left & right        */

    /* body */
    draw_round_rect(fb, cx, cy, hw, hh, r, GFX_WHITE);

    /* inset screen (leaves a thick top/bottom bezel for speaker + home) */
    float sw = hw - bx;
    float sh = hh - bz;
    draw_round_rect(fb, cx, cy, sw, sh, r * 0.55f, GFX_DGREY);

    /* speaker slit in the top bezel */
    float slit_w = hw * 0.5f, slit_h = GFX_S(2) * scale;
    float slit_y = cy - (hh + sh) * 0.5f;
    draw_round_rect(fb, cx, slit_y, slit_w, slit_h, slit_h, GFX_GREY);

    /* home dot in the bottom bezel */
    float home_y = cy + (hh + sh) * 0.5f;
    gfx_fill_circle(fb, cx, home_y, GFX_S(3) * scale, GFX_GREY);
}

/* little "ringer" mark: three nested arcs opening rightward whose brightness
 * chases outward, evoking sound leaving the phone. */
static void draw_sound_mark(gfx_fb *fb, float x, float y, float t)
{
    for (int i = 0; i < 3; i++) {
        float rr = GFX_S(9) + i * GFX_S(9);
        float b  = 0.30f + 0.70f * (0.5f + 0.5f * sinf(t * 6.0f - i * 1.1f));
        /* PI/2 == 3 o'clock in this coordinate system (0 = 12, clockwise) */
        gfx_arc(fb, x, y, rr, GFX_S(3),
                PI * 0.5f - 0.6f, PI * 0.5f + 0.6f, dim(GFX_ACCENT, b));
    }
}

/* ===================== per-state rendering ============================= */

static void draw_idle(gfx_fb *fb)
{
    float t = hal_millis() / 1000.0f;
    float breathe = 0.5f + 0.5f * sinf(t * (2 * PI / 1.1f)); /* 0..1, ~1.1s */
    float cx = GFX_CX, cy = GFX_CY - GFX_S(6);

    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(46),
                       "FIND MY PHONE", GFX_GREY);

    /* soft breathing halo behind the phone */
    gfx_fill_circle(fb, cx, cy, GFX_S(58) + breathe * GFX_S(6),
                    dim(GFX_ACCENT, 0.08f + 0.07f * breathe));
    gfx_ring(fb, cx, cy, GFX_S(56), GFX_S(2),
             dim(GFX_ACCENT, 0.25f + 0.30f * breathe));

    draw_phone(fb, cx, cy, 1.35f);

    font_draw_centered(fb, &font_medium, (int)GFX_CX, (int)GFX_S(198),
                       "Tap to ring", GFX_WHITE);
}

static void draw_ringing(gfx_fb *fb)
{
    float    t    = hal_millis() / 1000.0f;
    uint32_t t_ms = hal_millis();
    float    cx   = GFX_CX, cy = GFX_CY - GFX_S(18);

    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(30),
                       "RINGING", GFX_GREY);

    /* emanating sound waves: staggered rings, growing + fading. The spread is
     * capped so even the faintest outermost ring stays below the title band
     * above and clear of the STOP pill below. */
    float R0 = GFX_S(26), SPREAD = GFX_S(32);
    for (int k = 0; k < N_RING; k++) {
        float ph  = ring_phase(t_ms, k, N_RING);
        float rad = ring_radius(R0, SPREAD, ph);
        float fade_in = ph < 0.12f ? ph / 0.12f : 1.0f;  /* no pop at centre */
        float b = (1.0f - ph) * fade_in;
        gfx_ring(fb, cx, cy, rad, GFX_S(3), dim(GFX_ACCENT, b));
    }

    /* the phone, buzzing: fast horizontal jitter */
    float shake = sinf(t * 42.0f) * GFX_S(2.4f);
    draw_phone(fb, cx + shake, cy, 1.05f);

    /* ringer "sound" mark just off the phone's upper-right */
    draw_sound_mark(fb, cx + GFX_S(30), cy - GFX_S(6), t);

    /* big STOP pill near the bottom (whole-screen tap also stops), with a soft
     * pill-shaped halo pulse — kept fully inside the bezel. */
    float sy = GFX_CY + GFX_S(76);
    float pulse = 0.5f + 0.5f * sinf(t * (2 * PI / 0.7f));
    draw_round_rect(fb, cx, sy, GFX_S(46) + pulse * GFX_S(4),
                    GFX_S(21) + pulse * GFX_S(3), GFX_S(23),
                    dim(GFX_RED, 0.20f + 0.16f * pulse));
    draw_round_rect(fb, cx, sy, GFX_S(46), GFX_S(21), GFX_S(21), GFX_RED);
    font_draw_centered(fb, &font_medium, (int)cx, (int)(sy + GFX_S(7)),
                       "STOP", GFX_WHITE);
}

/* ===================== app callbacks =================================== */

static void on_start(void)
{
    s_state = ST_IDLE;
    s_elapsed_ms = 0;
    s_last_beat = 0;
}

static void on_draw(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);
    if (s_state == ST_RING) draw_ringing(fb);
    else                    draw_idle(fb);
}

/* begin ringing: reset timers, fire the first buzz, and send the real BLE ping so the paired
 * companion rings ITSELF at full volume (MEDIA_CMD code 7). */
static void start_ringing(void)
{
    s_state = ST_RING;
    s_elapsed_ms = 0;
    s_last_beat = 0;
    hal_find_phone();
    hal_haptic(20);
}

static void stop_ringing(void)
{
    s_state = ST_IDLE;
    s_elapsed_ms = 0;
    hal_haptic(12);
}

static int on_event(const wos_event *ev)
{
    switch (ev->type) {
    case WOS_EV_TICK:
        if (s_state == ST_RING) {
            s_elapsed_ms += (ev->x > 0) ? (unsigned)ev->x : 0u;

            /* rhythmic haptic: buzz once each time the beat index ticks up */
            unsigned b = beat_index(s_elapsed_ms);
            if (b != s_last_beat) {
                s_last_beat = b;
                hal_haptic(14);
            }

            /* auto-stop after the timeout */
            if (ring_active_after(s_state, s_elapsed_ms) == ST_IDLE)
                stop_ringing();
            return 1;
        }
        return 0;

    case WOS_EV_TAP:
        if (s_state == ST_RING) stop_ringing();   /* STOP */
        else                    start_ringing();  /* ring the phone */
        return 1;

    default:
        return 0;
    }
}

const wos_app app_findphone = {
    "Find Phone", on_start, on_event, on_draw, GFX_ACCENT, ICON_PHONE
};

#else  /* ===================== SELFTEST ================================== */
#include <assert.h>

int main(void)
{
    /* --- auto-stop cutoff ------------------------------------------------ */
    assert(ring_active_after(ST_IDLE, 100000)          == ST_IDLE);  /* idle stays idle */
    assert(ring_active_after(ST_RING, 0)               == ST_RING);
    assert(ring_active_after(ST_RING, RING_TIMEOUT_MS-1)== ST_RING);
    assert(ring_active_after(ST_RING, RING_TIMEOUT_MS)  == ST_IDLE); /* stops exactly at 15s */
    assert(ring_active_after(ST_RING, RING_TIMEOUT_MS+5)== ST_IDLE);

    /* frame-by-frame accumulation stops at/after the timeout */
    {
        int st = ST_RING; unsigned e = 0;
        for (int f = 0; f < 2000 && st == ST_RING; f++) {
            e += 16;                    /* ~60 fps */
            st = ring_active_after(st, e);
        }
        assert(st == ST_IDLE);
        assert(e >= RING_TIMEOUT_MS);   /* only stopped after the full timeout */
    }

    /* --- ring phase stays in [0,1) and radius grows monotonically with it - */
    for (uint32_t t = 0; t < 5000; t += 37) {
        for (int k = 0; k < N_RING; k++) {
            float p = ring_phase(t, k, N_RING);
            assert(p >= 0.0f && p < 1.0f);
        }
    }
    /* within one ring, a larger phase => strictly larger radius */
    {
        float r0 = 30.0f, spread = 74.0f;
        assert(ring_radius(r0, spread, 0.0f) == r0);
        assert(ring_radius(r0, spread, 0.25f) < ring_radius(r0, spread, 0.5f));
        assert(ring_radius(r0, spread, 0.5f)  < ring_radius(r0, spread, 0.99f));
        assert(ring_radius(r0, spread, 1.0f) == r0 + spread);
    }
    /* ring k=0 starts at the centre (phase 0 at t=0) and a fresh ring leaves
     * every RING_PERIOD_MS/N_RING ms */
    {
        assert(ring_phase(0, 0, N_RING) == 0.0f);
        float step = ring_phase(RING_PERIOD_MS / N_RING, 0, N_RING);
        assert(step > 0.2f && step < 0.3f);   /* ~1/N_RING == 0.25 */
    }

    /* --- rhythmic beat counter increments once per BEAT_MS --------------- */
    assert(beat_index(0)            == 0);
    assert(beat_index(BEAT_MS - 1)  == 0);
    assert(beat_index(BEAT_MS)      == 1);
    assert(beat_index(2 * BEAT_MS)  == 2);
    /* count distinct beats (haptic pulses) over the full ring window */
    {
        unsigned last = 0, pulses = 0;
        for (unsigned e = 16; e <= RING_TIMEOUT_MS; e += 16) {
            unsigned b = beat_index(e);
            if (b != last) { last = b; pulses++; }
        }
        assert(pulses == RING_TIMEOUT_MS / BEAT_MS); /* one buzz per beat, 21 */
    }

    printf("SELFTEST OK: auto-stop @15s, ring phase/radius growth, %u beats/ring\n",
           RING_TIMEOUT_MS / BEAT_MS);
    return 0;
}
#endif
