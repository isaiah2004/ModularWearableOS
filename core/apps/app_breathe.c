/* app_breathe.c — Breathing / Mindfulness screen.
 *
 * A serene "breathing ring": N small dots in a fixed hexagon around the
 * centre plus a central dot. They slide outward and swell on the inhale, then
 * draw back in and shrink on the exhale, following a smooth eased curve. Each
 * dot is a single anti-aliased disc (gfx_fill_circle), so only those tiny
 * regions move and the damage engine repaints a few small boxes per frame
 * rather than a full-screen effect. Center guidance text switches with the
 * phase ("Breathe in" / "Breathe out"), a bezel ring shows session progress,
 * and a soft haptic tick marks every phase change.
 *
 * ---------------------------------------------------------------------------
 * PLAIN-ENGLISH ALGORITHM
 *
 *   TIME. Everything is driven by accumulated TICK milliseconds (ev->x), not
 *   by frame counting, so motion stays silky at any refresh rate. We keep one
 *   counter s_total_ms = milliseconds of *breathing done so far* (frozen while
 *   paused / finished). The session lasts SESSION_MS (1 minute).
 *
 *   BREATH CYCLE. One breath is CYCLE_MS = INHALE_MS + EXHALE_MS (4s + 4s,
 *   both configurable). Given elapsed ms, breath_at() reduces it modulo the
 *   cycle and reports:
 *     - phase    : position within the whole cycle, 0..1 (for the rotation etc)
 *     - inhaling : 1 for the first INHALE_MS of the cycle, else 0
 *     - scale    : an EASED breath amount in 0..1, 0 = fully contracted,
 *                  1 = fully bloomed. We use a raised cosine so velocity is
 *                  zero at both turning points (the calm "hang" at top/bottom
 *                  of a breath):
 *                    inhale, local t in 0..1:  scale = 0.5 - 0.5*cos(PI*t)  (0->1)
 *                    exhale, local t in 0..1:  scale = 0.5 + 0.5*cos(PI*t)  (1->0)
 *                  With a symmetric 4s/4s cycle this makes scale peak exactly
 *                  at the half-cycle and bottom out at the cycle boundaries.
 *
 *   GEOMETRY. Dot ring radius   ring_r = lerp(RING_MIN, RING_MAX, scale)
 *             dot disc radius   dot_r  = lerp(DOT_MIN,  DOT_MAX,  scale)
 *   Dot k sits at the fixed hexagon angle k*2PI/N, distance `ring_r` from
 *   centre, and is drawn as one gfx_fill_circle. A central dot pulses gently.
 *   Nothing rotates, so a dot only ever moves along its own radial line —
 *   keeping the per-frame dirty area to a few small discs.
 *
 *   SESSION. progress = s_total_ms / SESSION_MS drives a bezel arc. At each
 *   inhale<->exhale flip we buzz hal_haptic briefly; at SESSION_MS we stop,
 *   buzz once more, and show "Well done / Tap to restart". TAP pauses/resumes
 *   while running, or restarts once finished.
 *
 *   Only the branch-y bit (elapsed ms -> phase / label / eased scale) is pure
 *   and worth testing, so it lives in breath_at() and is exercised by the
 *   SELFTEST main() at the bottom (t = 0, quarter, half, full cycle).
 * ---------------------------------------------------------------------------
 */
#include "apps.h"
#include "../font.h"
#include <stdio.h>
#include <math.h>

#define PI      3.14159265f
#define TWO_PI  6.28318531f

/* ---- tunables ---------------------------------------------------------- */
#define N_PETALS   6
#define INHALE_MS  4000u
#define EXHALE_MS  4000u
#define CYCLE_MS   (INHALE_MS + EXHALE_MS)   /* one full breath */
#define SESSION_MS 60000u                    /* 1 minute session */

/* ===================== pure, self-tested breath math =================== */

typedef struct {
    float phase;    /* 0..1 position within the whole breath cycle          */
    float scale;    /* 0..1 eased breath amount (0 = contracted, 1 = bloom) */
    int   inhaling; /* 1 during inhale, 0 during exhale                     */
} breath_t;

/* Map elapsed ms -> where we are in the current breath. Pure & deterministic. */
static breath_t breath_at(unsigned ms)
{
    unsigned c = ms % CYCLE_MS;
    breath_t b;
    b.phase = (float)c / (float)CYCLE_MS;
    if (c < INHALE_MS) {
        float t = (float)c / (float)INHALE_MS;      /* 0..1 through inhale */
        b.scale = 0.5f - 0.5f * cosf(PI * t);       /* eased 0 -> 1        */
        b.inhaling = 1;
    } else {
        float t = (float)(c - INHALE_MS) / (float)EXHALE_MS; /* 0..1 exhale */
        b.scale = 0.5f + 0.5f * cosf(PI * t);       /* eased 1 -> 0        */
        b.inhaling = 0;
    }
    return b;
}

static const char *breath_label(int inhaling)
{
    return inhaling ? "Breathe in" : "Breathe out";
}

#ifndef SELFTEST
/* ===================== app state ======================================= */

static unsigned s_total_ms;      /* breathing time accumulated so far      */
static int      s_running;       /* 1 = breathing, 0 = paused/finished     */
static int      s_last_inhaling; /* previous phase, for change detection    */

/* ---- geometry (design-grid values, scaled by GFX_S) -------------------- */
/* Six dots sit on a ring around the centre. On the inhale the ring expands
 * (dots move outward) and each dot swells; on the exhale it contracts. Only the
 * small dot discs move, so the damage engine repaints a handful of tiny boxes
 * per frame instead of a full-screen effect. */
#define RING_MIN   GFX_S(18)     /* ring radius when fully contracted (exhale) */
#define RING_MAX   GFX_S(78)     /* ring radius when fully bloomed   (inhale)  */
#define DOT_MIN    GFX_S(7)      /* dot radius when contracted                 */
#define DOT_MAX    GFX_S(15)     /* dot radius when bloomed                    */
#define CORE_MIN   GFX_S(6)      /* central dot radius, gently pulses          */
#define CORE_MAX   GFX_S(11)
#define RING_R     GFX_S(112)    /* session progress ring, near the bezel   */

/* ---- palette: calming teal -> blue ------------------------------------- */
#define PETAL_TEAL GFX_RGB(40, 210, 170)
#define PETAL_BLUE GFX_RGB(70, 150, 230)
#define CORE_COL   GFX_RGB(150, 240, 220)
#define TRACK_COL  GFX_RGB(20, 40, 45)

/* colour `col` at brightness a in [0,1], composited on black */
static uint16_t dim(uint16_t col, float a)
{
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    return gfx_blend565(GFX_BLACK, col, (uint8_t)(a * 255.0f + 0.5f));
}

/* seconds remaining -> "M:SS" */
static void fmt_remaining(char *buf, int n, unsigned total_ms)
{
    unsigned rem = (total_ms >= SESSION_MS) ? 0 : (SESSION_MS - total_ms);
    int secs = (int)((rem + 999) / 1000);   /* round up so it reads 1:00.. */
    snprintf(buf, n, "%d:%02d", secs / 60, secs % 60);
}

/* ===================== app callbacks =================================== */

static void on_start(void)
{
    s_total_ms = 0;
    s_running = 1;
    s_last_inhaling = breath_at(0).inhaling;
}

static void on_draw(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);

    int finished = (s_total_ms >= SESSION_MS);
    breath_t b = breath_at(s_total_ms);

    float cx = GFX_CX, cy = GFX_CY;
    float scale  = b.scale;
    float ring_r = RING_MIN + scale * (RING_MAX - RING_MIN); /* dot ring radius */
    float dot_r  = DOT_MIN  + scale * (DOT_MAX  - DOT_MIN);   /* each dot's size */
    float core_r = CORE_MIN + scale * (CORE_MAX - CORE_MIN);  /* central dot     */

    /* faint session-progress ring around the bezel */
    gfx_ring(fb, cx, cy, RING_R, GFX_S(4), TRACK_COL);
    float prog = (float)s_total_ms / (float)SESSION_MS;
    if (prog > 0.0f) {
        if (prog > 1.0f) prog = 1.0f;
        gfx_arc(fb, cx, cy, RING_R, GFX_S(4), 0.0f, TWO_PI * prog,
                dim(PETAL_TEAL, 0.7f));
    }

    /* the breathing ring: N small dots in a fixed hexagon, each a single
     * anti-aliased disc. They slide out on the inhale and swell a little, then
     * draw back in on the exhale. Only these tiny discs move, so the damage
     * engine repaints a few small boxes per frame — no full-screen effect. */
    float bright = 0.55f + 0.45f * scale;     /* dots also brighten on inhale */
    for (int k = 0; k < N_PETALS; k++) {
        float theta = (float)k * (TWO_PI / N_PETALS);   /* fixed hexagon angles */
        float px = cx + gfx_fast_sin(theta) * ring_r;
        float py = cy - gfx_fast_cos(theta) * ring_r;
        uint16_t col = gfx_blend565(PETAL_TEAL, PETAL_BLUE,
                                    (uint8_t)((float)k / (N_PETALS - 1) * 255.0f));
        gfx_fill_circle(fb, px, py, dot_r, dim(col, bright));
    }
    gfx_fill_circle(fb, cx, cy, core_r, dim(CORE_COL, 0.5f + 0.5f * scale));

    /* header */
    font_draw_centered(fb, &font_small, (int)cx, (int)GFX_S(38),
                       "BREATHE", GFX_GREY);

    if (finished) {
        font_draw_centered(fb, &font_medium, (int)cx, (int)(cy - GFX_S(2)),
                           "Well done", GFX_WHITE);
        font_draw_centered(fb, &font_small, (int)cx, (int)GFX_S(206),
                           "Tap to restart", GFX_GREY);
        return;
    }

    /* center guidance, on top of the flower */
    font_draw_centered(fb, &font_medium, (int)cx, (int)(cy - GFX_S(2)),
                       breath_label(b.inhaling), GFX_WHITE);

    if (!s_running) {
        font_draw_centered(fb, &font_small, (int)cx, (int)GFX_S(206),
                           "Paused - tap to resume", GFX_GREY);
    } else {
        char t[8];
        fmt_remaining(t, sizeof t, s_total_ms);
        font_draw_centered(fb, &font_medium, (int)cx, (int)GFX_S(208),
                           t, dim(PETAL_TEAL, 0.9f));
    }
}

static int on_event(const wos_event *ev)
{
    switch (ev->type) {
    case WOS_EV_TICK:
        if (s_running) {
            s_total_ms += (unsigned)ev->x;
            if (s_total_ms >= SESSION_MS) {
                s_total_ms = SESSION_MS;
                s_running = 0;
                hal_haptic(40);            /* gentle "session over" cue */
            } else {
                breath_t b = breath_at(s_total_ms);
                if (b.inhaling != s_last_inhaling) {
                    s_last_inhaling = b.inhaling;
                    hal_haptic(15);        /* soft phase-change tick */
                }
            }
        }
        return 1;

    case WOS_EV_TAP:
        if (s_total_ms >= SESSION_MS) on_start();  /* finished -> restart */
        else s_running = !s_running;               /* pause / resume      */
        return 1;

    default:
        return 0;
    }
}

const wos_app app_breathe = {
    "Breathe", on_start, on_event, on_draw, GFX_GREEN, ICON_BREATHE
};

#else  /* ===================== SELFTEST ================================== */
#include <assert.h>

static int feq(float a, float b) { float d = a - b; return d < 0.02f && d > -0.02f; }

int main(void)
{
    /* t = 0 : very start of the inhale — contracted, "Breathe in" */
    breath_t b0 = breath_at(0);
    assert(b0.inhaling == 1);
    assert(feq(b0.phase, 0.0f));
    assert(feq(b0.scale, 0.0f));

    /* quarter cycle (2000 ms) : mid-inhale, eased scale == 0.5 */
    breath_t bq = breath_at(CYCLE_MS / 4);
    assert(bq.inhaling == 1);
    assert(feq(bq.phase, 0.25f));
    assert(feq(bq.scale, 0.5f));

    /* half cycle (4000 ms) : peak bloom, phase flips to exhale */
    breath_t bh = breath_at(CYCLE_MS / 2);
    assert(bh.inhaling == 0);
    assert(feq(bh.phase, 0.5f));
    assert(feq(bh.scale, 1.0f));

    /* three-quarter cycle : mid-exhale, back to 0.5 */
    breath_t b3 = breath_at(CYCLE_MS * 3 / 4);
    assert(b3.inhaling == 0);
    assert(feq(b3.scale, 0.5f));

    /* full cycle wraps back to the start */
    breath_t bf = breath_at(CYCLE_MS);
    assert(bf.inhaling == 1);
    assert(feq(bf.phase, 0.0f));
    assert(feq(bf.scale, 0.0f));

    /* labels track the phase */
    assert(breath_label(1)[8] == 'i');   /* "Breathe in"  */
    assert(breath_label(0)[8] == 'o');   /* "Breathe out" */

    /* scale stays within [0,1] and is symmetric across the whole cycle */
    for (unsigned t = 0; t < CYCLE_MS; t += 50) {
        breath_t b = breath_at(t);
        assert(b.scale >= -0.001f && b.scale <= 1.001f);
        breath_t mirror = breath_at(CYCLE_MS - t == CYCLE_MS ? 0 : CYCLE_MS - t);
        assert(feq(b.scale, mirror.scale));   /* inhale mirrors exhale */
    }

    printf("SELFTEST OK: breath phase/label/eased-scale verified "
           "at t=0, 1/4, 1/2, 3/4, full cycle\n");
    return 0;
}
#endif
