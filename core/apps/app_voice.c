/* app_voice.c — Voice Assistant (Siri / Google-Assistant style concept UI).
 *
 * HOW THE REAL THING WOULD WORK:
 *   The plain Waveshare 1.43 board has NO microphone. In the shipping design
 *   the watch does not run speech-to-text or an LLM locally at all: it opens a
 *   BLE audio/assistant channel to the paired PHONE, streams the mic capture
 *   there, and the phone runs the wake-word + speech-to-text + the actual
 *   assistant, then sends back the recognised query and the spoken answer for
 *   us to render. So this screen is purely the on-watch PRESENTATION layer.
 *   Because there is no mic here, this build is a MOCKUP: the "listening"
 *   waveform is driven by hal_audio_bars() (a demo signal in the sim; a flat
 *   synthesized shimmer if it reports no mic), and the query/answer text is a
 *   short scripted set of canned Q/A pairs cycled on each ask.
 *
 * ---------------------------------------------------------------------------
 * PLAIN-ENGLISH ALGORITHM
 *
 *   Four-state machine, driven entirely by TAP and TICK (ev->x = ms/frame):
 *
 *   STATE IDLE
 *     - Friendly prompt "Tap to speak" under a glowing round mic button.
 *     - The mic glyph is drawn procedurally (capsule + cradle arc + stand).
 *     - The button "breathes": a soft halo whose radius/brightness follow a
 *       slow sine of hal_millis() (phase = t/900).  TAP -> LISTENING.
 *
 *   STATE LISTENING  (auto-advances after LISTEN_MS = 2500 ms)
 *     - A living Siri-style orb: N_RING concentric rings expand outward and
 *       fade.  Ring k's phase = frac(t/RING_MS + k/N_RING) in [0,1); its
 *       radius = R0 + phase*SPREAD and its brightness = (1-phase).  A pulsing
 *       core disc sits in the middle.  "Listening..." label on top.
 *     - A live MIRRORED WAVEFORM near the bottom: hal_audio_bars() levels
 *       (0..255), each low-pass smoothed toward its target; if there is no mic
 *       we synthesize a gentle travelling sine so the mockup still looks alive.
 *     - Each TICK adds ev->x to s_elapsed_ms; at >= LISTEN_MS -> THINKING.
 *
 *   STATE THINKING  (auto-advances after THINK_MS = 1200 ms)
 *     - A rotating "comet" spinner: a bright gfx_arc whose start angle sweeps
 *       with time (angle = t/SPIN_MS * 2PI), over a faint full ring, plus two
 *       dimmer trailing arcs for the tail.  "Thinking..." label.
 *     - At >= THINK_MS -> ANSWER; on entry we latch the current Q/A pair to
 *       display and advance the cycle index for next time.
 *
 *   STATE ANSWER
 *     - A clean rounded card: "YOU ASKED", the query, a divider, then the
 *       two-line answer.  A small pulsing assistant orb sits above it.
 *     - Hint "Tap to ask again".  TAP -> LISTENING (asks the next canned pair,
 *       cycling through NQA pairs on repeat).
 *
 *   TAP while LISTENING/THINKING cancels back to IDLE.
 *
 *   The only branch-y "logic" worth testing is the time-driven state advance
 *   and the Q/A cycling, so both live in pure helpers (state_after / next_qa)
 *   compiled in both the app and the SELFTEST build below.
 * ---------------------------------------------------------------------------
 */
#include "apps.h"
#include "../font.h"
#include <stdio.h>
#ifndef SELFTEST
#include <math.h>
#endif

#define PI 3.14159265f

/* ---- state machine (visible to app + selftest) ------------------------- */
enum { ST_IDLE = 0, ST_LISTEN, ST_THINK, ST_ANSWER };

#define LISTEN_MS 2500u   /* time spent "listening" before thinking */
#define THINK_MS  1200u   /* spinner duration before the answer     */

/* ---- scripted Q/A (the phone would supply these for real) -------------- */
typedef struct { const char *q; const char *a1; const char *a2; } qa_t;

static const qa_t QA[] = {
    { "What's the weather?", "24\xB0 and clear",   "in Hyderabad."   },
    { "Any reminders?",      "Standup at 10,",     "gym at 6pm."     },
    { "Play some music.",    "Starting your",      "Focus mix now."  },
};
#define NQA ((int)(sizeof QA / sizeof QA[0]))

/* ===================== pure helpers (self-tested) ======================= */

/* Time-driven advance for the two auto-advancing states. Given the current
 * state and how long we've been in it, return the state we should now be in.
 * IDLE and ANSWER never auto-advance (they wait for a TAP). */
static int state_after(int state, unsigned elapsed_ms)
{
    if (state == ST_LISTEN && elapsed_ms >= LISTEN_MS) return ST_THINK;
    if (state == ST_THINK  && elapsed_ms >= THINK_MS)  return ST_ANSWER;
    return state;
}

/* Next Q/A index, cycling 0..NQA-1. */
static int next_qa(int i) { return (i + 1) % NQA; }

#ifndef SELFTEST
/* ===================== app state ======================================= */

static int      s_state;
static unsigned s_elapsed_ms;   /* time accumulated in the current state */
static int      s_qa;           /* next pair to ask */
static int      s_shown_qa;     /* pair currently displayed in ANSWER    */

#define NBARS 24
static float    s_wave[NBARS];  /* smoothed 0..1 waveform levels */

/* geometry (design-grid coords, scaled by GFX_S) */
#define MIC_CY   GFX_CY               /* mic button centre (IDLE)   */
#define MIC_R    GFX_S(46)            /* mic button radius          */

/* ===================== small drawing helpers =========================== */

/* purple (or any colour) at brightness a in [0,1], composited on black */
static uint16_t dim(uint16_t col, float a)
{
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    return gfx_blend565(GFX_BLACK, col, (uint8_t)(a * 255.0f + 0.5f));
}

/* a filled rounded-rect card (built from rects + 4 corner discs) */
static void draw_card(gfx_fb *fb, float cx, float cy, float hw, float hh,
                      float r, uint16_t col)
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

/* procedural microphone glyph: rounded capsule + U cradle + stand */
static void draw_mic_glyph(gfx_fb *fb, float cx, float cy, float r, uint16_t col)
{
    float cap_cy = cy - r * 0.22f;      /* capsule centre                */
    float hh = r * 0.52f;               /* capsule half-height           */
    float hw = r * 0.30f;               /* capsule half-width            */
    float th = r * 0.14f;               /* stroke thickness              */

    /* capsule body (rounded top+bottom) */
    gfx_fill_rect(fb, (int)(cx - hw), (int)(cap_cy - hh),
                  (int)(2 * hw), (int)(2 * hh), col);
    gfx_fill_circle(fb, cx, cap_cy - hh, hw, col);
    gfx_fill_circle(fb, cx, cap_cy + hh, hw, col);

    /* cradle: a U-shaped arc hugging the lower half of the capsule
     * (0 = 12 o'clock, cw; PI/2..3PI/2 is the bottom half) */
    gfx_arc(fb, cx, cap_cy, hw + r * 0.24f, th, PI * 0.5f, PI * 1.5f, col);

    /* stand: stem down to a foot */
    float foot = cy + r * 0.72f;
    gfx_line(fb, cx, cap_cy + hh + r * 0.20f, cx, foot, th, col);
    gfx_line(fb, cx - r * 0.34f, foot, cx + r * 0.34f, foot, th, col);
}

/* live mirrored waveform about a horizontal mid-line */
static void draw_waveform(gfx_fb *fb, float mid_y)
{
    uint8_t lv[NBARS];
    int na = hal_audio_bars(lv, NBARS);
    float t = hal_millis() / 1000.0f;

    float margin = GFX_S(52);
    float span = GFX_W - 2 * margin;
    float slot = span / NBARS;
    float bw = slot * 0.5f;
    float maxh = GFX_S(30);

    for (int i = 0; i < NBARS; i++) {
        float target;
        if (na > 0 && i < na) {
            target = lv[i] / 255.0f;
        } else {
            /* no mic: gentle travelling shimmer so the mockup stays alive */
            float s = sinf(t * 6.0f + i * 0.7f) * sinf(t * 2.3f + i * 0.3f);
            target = 0.15f + 0.35f * (0.5f + 0.5f * s);
        }
        s_wave[i] += (target - s_wave[i]) * 0.35f;
        float h = GFX_S(2) + s_wave[i] * maxh;
        float x = margin + i * slot + (slot - bw) * 0.5f;
        uint16_t c = dim(GFX_PURPLE, 0.45f + 0.55f * s_wave[i]);
        gfx_fill_rect(fb, (int)x, (int)(mid_y - h), (int)bw, (int)(h * 2), c);
    }
}

/* ===================== per-state rendering ============================= */

static void draw_idle(gfx_fb *fb)
{
    float t = hal_millis() / 1000.0f;
    float breathe = 0.5f + 0.5f * sinf(t * (2 * PI / 0.9f)); /* 0..1, ~0.9s */

    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(40),
                       "ASSISTANT", GFX_GREY);

    /* soft breathing halo behind the button */
    gfx_fill_circle(fb, GFX_CX, MIC_CY, MIC_R + GFX_S(14) + breathe * GFX_S(8),
                    dim(GFX_PURPLE, 0.12f + 0.10f * breathe));
    gfx_ring(fb, GFX_CX, MIC_CY, MIC_R + GFX_S(8), GFX_S(2),
             dim(GFX_PURPLE, 0.35f + 0.35f * breathe));

    /* the button + procedural mic */
    gfx_fill_circle(fb, GFX_CX, MIC_CY, MIC_R, GFX_PURPLE);
    draw_mic_glyph(fb, GFX_CX, MIC_CY, MIC_R * 0.72f, GFX_WHITE);

    font_draw_centered(fb, &font_medium, (int)GFX_CX,
                       (int)(MIC_CY + GFX_S(84)), "Tap to speak", GFX_WHITE);
}

static void draw_listening(gfx_fb *fb)
{
    float t = hal_millis() / 1000.0f;
    float cx = GFX_CX, cy = GFX_CY - GFX_S(18);

#define N_RING 3
    /* Spread capped so the outermost (faintest) ring stays below the label
     * band above and clear of the waveform below. */
    float R0 = GFX_S(26), SPREAD = GFX_S(30), RING_MS = 1.4f;
    for (int k = 0; k < N_RING; k++) {
        float ph = fmodf(t / RING_MS + (float)k / N_RING, 1.0f);
        float rad = R0 + ph * SPREAD;
        gfx_ring(fb, cx, cy, rad, GFX_S(3), dim(GFX_PURPLE, 1.0f - ph));
    }
#undef N_RING

    /* pulsing core */
    float pulse = 0.5f + 0.5f * sinf(t * (2 * PI / 0.55f));
    gfx_fill_circle(fb, cx, cy, R0 - GFX_S(4) + pulse * GFX_S(6), GFX_PURPLE);
    gfx_fill_circle(fb, cx, cy, GFX_S(10), dim(GFX_WHITE, 0.6f + 0.4f * pulse));

    font_draw_centered(fb, &font_medium, (int)GFX_CX, (int)GFX_S(40),
                       "Listening", GFX_WHITE);

    draw_waveform(fb, GFX_CY + GFX_S(82));
}

static void draw_thinking(gfx_fb *fb)
{
    float t = hal_millis() / 1000.0f;
    float cx = GFX_CX, cy = GFX_CY - GFX_S(10);
    float r = GFX_S(34), th = GFX_S(6);

    /* faint full track */
    gfx_ring(fb, cx, cy, r, th, dim(GFX_PURPLE, 0.18f));

    /* rotating comet: bright head + two fading tail segments */
    float a = fmodf(t * (2 * PI / 0.85f), 2 * PI);   /* ~0.85s / rev */
    float seg = 0.9f;                                /* head arc span */
    gfx_arc(fb, cx, cy, r, th, a, a + seg, dim(GFX_PURPLE, 1.0f));
    gfx_arc(fb, cx, cy, r, th, a - seg, a, dim(GFX_PURPLE, 0.55f));
    gfx_arc(fb, cx, cy, r, th, a - 2 * seg, a - seg, dim(GFX_PURPLE, 0.28f));

    font_draw_centered(fb, &font_medium, (int)GFX_CX,
                       (int)(GFX_CY + GFX_S(66)), "Thinking", GFX_WHITE);
}

static void draw_answer(gfx_fb *fb)
{
    const qa_t *p = &QA[s_shown_qa];
    float t = hal_millis() / 1000.0f;
    float pulse = 0.5f + 0.5f * sinf(t * (2 * PI / 1.1f));

    /* small assistant orb up top */
    gfx_fill_circle(fb, GFX_CX, GFX_S(42), GFX_S(9) + pulse * GFX_S(3),
                    dim(GFX_PURPLE, 0.6f + 0.4f * pulse));

    /* the answer card */
    float cy = GFX_CY + GFX_S(4);
    draw_card(fb, GFX_CX, cy, GFX_S(96), GFX_S(62), GFX_S(16), GFX_DGREY);

    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)(cy - GFX_S(42)),
                       "YOU ASKED", GFX_GREY);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)(cy - GFX_S(24)),
                       p->q, GFX_PURPLE);

    /* divider */
    gfx_fill_rect(fb, (int)(GFX_CX - GFX_S(72)), (int)(cy - GFX_S(12)),
                  (int)GFX_S(144), (int)GFX_S(1), GFX_GREY);

    font_draw_centered(fb, &font_medium, (int)GFX_CX, (int)(cy + GFX_S(14)),
                       p->a1, GFX_WHITE);
    font_draw_centered(fb, &font_medium, (int)GFX_CX, (int)(cy + GFX_S(40)),
                       p->a2, GFX_WHITE);

    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(212),
                       "Tap to ask again", GFX_DGREY);
}

/* ===================== app callbacks =================================== */

static void on_start(void)
{
    s_state = ST_IDLE;
    s_elapsed_ms = 0;
    s_qa = 0;
    s_shown_qa = 0;
    for (int i = 0; i < NBARS; i++) s_wave[i] = 0.0f;
}

static void on_draw(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);
    if (!hal_audio_available()) {
        font_draw_centered(fb, &font_medium, (int)GFX_CX, (int)GFX_CY, "Microphone needed", GFX_WHITE);
        font_draw_centered(fb, &font_small, (int)GFX_CX, (int)(GFX_CY + GFX_S(25)), "External audio not configured", GFX_GREY);
        return;
    }
    switch (s_state) {
    case ST_LISTEN: draw_listening(fb); break;
    case ST_THINK:  draw_thinking(fb);  break;
    case ST_ANSWER: draw_answer(fb);    break;
    default:        draw_idle(fb);      break;
    }
}

/* begin a fresh "ask": clear timers + waveform smoothing, go LISTENING */
static void begin_listening(void)
{
    s_state = ST_LISTEN;
    s_elapsed_ms = 0;
    for (int i = 0; i < NBARS; i++) s_wave[i] = 0.0f;
    hal_haptic(20);
}

static int on_event(const wos_event *ev)
{
    if (!hal_audio_available()) return 0;
    switch (ev->type) {
    case WOS_EV_TICK:
        if (s_state == ST_LISTEN || s_state == ST_THINK) {
            s_elapsed_ms += (unsigned)ev->x;
            int ns = state_after(s_state, s_elapsed_ms);
            if (ns != s_state) {
                s_state = ns;
                s_elapsed_ms = 0;
                if (ns == ST_ANSWER) {
                    s_shown_qa = s_qa;         /* latch what we display */
                    s_qa = next_qa(s_qa);      /* cycle for next ask    */
                    hal_haptic(15);
                }
            }
            return 1;
        }
        return 0;

    case WOS_EV_TAP:
        if (s_state == ST_IDLE || s_state == ST_ANSWER) {
            begin_listening();                 /* start / ask again */
        } else {
            s_state = ST_IDLE;                 /* cancel listen/think */
            s_elapsed_ms = 0;
        }
        return 1;

    default:
        return 0;
    }
}

const wos_app app_voice = {
    "Assistant", on_start, on_event, on_draw, GFX_PURPLE, ICON_WAVEFORM
};

#else  /* ===================== SELFTEST ================================== */
#include <assert.h>

int main(void)
{
    /* IDLE / ANSWER never auto-advance regardless of elapsed time */
    assert(state_after(ST_IDLE,   100000) == ST_IDLE);
    assert(state_after(ST_ANSWER, 100000) == ST_ANSWER);

    /* LISTENING advances to THINKING exactly at LISTEN_MS */
    assert(state_after(ST_LISTEN, 0)              == ST_LISTEN);
    assert(state_after(ST_LISTEN, LISTEN_MS - 1)  == ST_LISTEN);
    assert(state_after(ST_LISTEN, LISTEN_MS)      == ST_THINK);
    assert(state_after(ST_LISTEN, LISTEN_MS + 1)  == ST_THINK);

    /* THINKING advances to ANSWER exactly at THINK_MS */
    assert(state_after(ST_THINK, 0)             == ST_THINK);
    assert(state_after(ST_THINK, THINK_MS - 1)  == ST_THINK);
    assert(state_after(ST_THINK, THINK_MS)      == ST_ANSWER);

    /* a full listen->think->answer accumulation, frame by frame */
    {
        int st = ST_LISTEN;
        unsigned e = 0;
        for (int frame = 0; frame < 400 && st != ST_ANSWER; frame++) {
            e += 16;                       /* ~60 fps */
            int ns = state_after(st, e);
            if (ns != st) { st = ns; e = 0; }
        }
        assert(st == ST_ANSWER);
    }

    /* Q/A index cycles 0..NQA-1 and wraps */
    {
        int i = 0;
        for (int n = 0; n < NQA; n++) i = next_qa(i);
        assert(i == 0);                    /* back to start after NQA steps */
        assert(next_qa(NQA - 1) == 0);
    }

    printf("SELFTEST OK: state_after transitions + Q/A cycling verified\n");
    return 0;
}
#endif
