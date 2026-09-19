/* app_recorder.c — Voice-memo recorder UI (watchOS Voice Memos look).
 *
 * NOTE: the plain Waveshare 1.43 board has NO microphone, so this is a UI
 * mockup. The live waveform is driven by hal_audio_bars() as a demo signal;
 * when that returns 0 (no mic) we draw a flat baseline and label it "NO MIC".
 *
 * ---------------------------------------------------------------------------
 * PLAIN-ENGLISH ALGORITHM
 *
 *   Two-state machine, driven by TAP and TICK events:
 *
 *   STATE IDLE (the library screen)
 *     - Draw a big red round RECORD button in the upper-center.
 *     - Below it, a divider then a scrolling-free list of previous "takes",
 *       newest first, each row = "Memo N        M:SS".
 *       The list is a fixed static array seeded with two example takes;
 *       no malloc, ever.
 *     - TAP inside the record button  ->  zero the elapsed timer, clear the
 *       waveform smoothing, and switch to RECORDING.
 *
 *   STATE RECORDING
 *     - Every WOS_EV_TICK carries ms-since-last-frame in ev->x; while
 *       recording we add that to s_elapsed_ms. That's the ticking clock.
 *     - Draw MM:SS from s_elapsed_ms, next to a red dot that PULSES
 *       (radius + brightness follow a sine of hal_millis) like watchOS.
 *     - Draw a LIVE MIRRORED WAVEFORM across the middle: ask hal_audio_bars
 *       for NBARS levels (0..255); each level is low-pass smoothed toward its
 *       target, then drawn as a vertical bar mirrored about the centre line.
 *       If the HAL returns 0 there is no mic -> flat line + "NO MIC".
 *     - Draw a round STOP button near the bottom (red disc + white square).
 *     - TAP inside the stop button  ->  append a new take
 *       {index = running counter, duration = elapsed seconds} to the list,
 *       then return to IDLE.
 *
 *   mm:ss formatting and the append step are the only "logic" worth testing,
 *   so they live in small pure helpers guarded by a SELFTEST main() below.
 * ---------------------------------------------------------------------------
 */
#include "apps.h"
#include "../font.h"
#include <stdio.h>
#ifndef SELFTEST
#include <math.h>
#endif

#define PI 3.14159265f

/* ---- takes list (static, no malloc) ------------------------------------ */
#define TAKES_CAP   16
#define TAKES_SHOWN 3      /* rows visible on the round face at once */

typedef struct { unsigned index; unsigned dur; } take_t;  /* dur in seconds */

static take_t   s_takes[TAKES_CAP];
static int      s_ntakes;
static unsigned s_next_index;   /* running "Memo N" counter */

/* ---- state machine ----------------------------------------------------- */
enum { ST_IDLE = 0, ST_REC };
static int      s_state;
static unsigned s_elapsed_ms;   /* accumulated from WOS_EV_TICK while recording */

/* ---- waveform ---------------------------------------------------------- */
#define NBARS 27
static float s_wave[NBARS];     /* smoothed 0..1 bar levels */

/* ---- geometry (design-grid coords, scaled by GFX_S) -------------------- */
#define REC_CY    GFX_S(82)     /* record button centre (IDLE) */
#define REC_R     GFX_S(34)
#define STOP_CY   GFX_S(198)    /* stop button centre (RECORDING) */
#define STOP_R    GFX_S(30)

/* ===================== pure helpers (self-tested) ======================= */

/* Format `secs` as minutes:seconds. pad_min -> "MM:SS" (recording clock),
 * else "M:SS" (compact list entry, watchOS style). */
static void fmt_mmss(char *buf, int cap, unsigned secs, int pad_min)
{
    unsigned m = (secs / 60u) % 100u;
    unsigned s = secs % 60u;
    snprintf(buf, (size_t)cap, pad_min ? "%02u:%02u" : "%u:%02u", m, s);
}

/* Append a finished take. Returns 1 if stored, 0 if the list was full. */
static int take_append(unsigned dur_sec)
{
    if (s_ntakes >= TAKES_CAP)
        return 0;
    s_takes[s_ntakes].index = s_next_index++;
    s_takes[s_ntakes].dur   = dur_sec;
    s_ntakes++;
    return 1;
}

/* Seed the library with a couple of example takes. */
static void takes_seed(void)
{
    s_ntakes = 0;
    s_next_index = 1;
    take_append(12);   /* Memo 1  0:12 */
    take_append(47);   /* Memo 2  0:47 */
}

#ifndef SELFTEST
/* ===================== app implementation =============================== */

static void on_start(void)
{
    s_state = ST_IDLE;
    s_elapsed_ms = 0;
    for (int i = 0; i < NBARS; i++) s_wave[i] = 0.0f;
    if (s_next_index == 0)          /* first ever launch: seed once */
        takes_seed();
}

/* draw a red round record button (white ring + red disc) */
static void draw_record_button(gfx_fb *fb, float cy)
{
    gfx_fill_circle(fb, GFX_CX, cy, REC_R + GFX_S(4), GFX_DGREY);
    gfx_ring(fb, GFX_CX, cy, REC_R + GFX_S(2), GFX_S(2), GFX_WHITE);
    gfx_fill_circle(fb, GFX_CX, cy, REC_R, GFX_RED);
}

/* draw a round stop button (red disc + white rounded square) */
static void draw_stop_button(gfx_fb *fb, float cy)
{
    gfx_ring(fb, GFX_CX, cy, STOP_R + GFX_S(2), GFX_S(2), GFX_WHITE);
    gfx_fill_circle(fb, GFX_CX, cy, STOP_R, GFX_RED);
    float q = GFX_S(13);
    gfx_fill_rect(fb, (int)(GFX_CX - q / 2), (int)(cy - q / 2),
                  (int)q, (int)q, GFX_WHITE);
}

static void draw_take_row(gfx_fb *fb, const take_t *t, float cy)
{
    char label[16], dur[12];
    snprintf(label, sizeof label, "Memo %u", t->index);
    fmt_mmss(dur, sizeof dur, t->dur, 0);
    int base = (int)(cy + font_small.ascent * 0.34f);
    font_draw(fb, &font_small, (int)GFX_S(58), base, label, GFX_WHITE);
    int w = font_text_width(&font_small, dur);
    font_draw(fb, &font_small, (int)(GFX_W - GFX_S(58) - w), base, dur, GFX_GREY);
}

static void draw_idle(gfx_fb *fb)
{
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(30),
                       "VOICE MEMOS", GFX_GREY);
    draw_record_button(fb, REC_CY);

    /* divider */
    gfx_fill_rect(fb, (int)GFX_S(56), (int)GFX_S(128),
                  (int)GFX_S(128), (int)GFX_S(1), GFX_DGREY);

    /* newest-first list, up to TAKES_SHOWN rows */
    float y0 = GFX_S(144), rh = GFX_S(30);
    int shown = s_ntakes < TAKES_SHOWN ? s_ntakes : TAKES_SHOWN;
    for (int k = 0; k < shown; k++) {
        const take_t *t = &s_takes[s_ntakes - 1 - k];  /* newest first */
        draw_take_row(fb, t, y0 + k * rh);
    }
    if (s_ntakes == 0)
        font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(160),
                           "No memos yet", GFX_DGREY);

    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(222),
                       "UI MOCKUP", GFX_DGREY);
}

static void draw_waveform(gfx_fb *fb, int have_mic, const uint8_t *lv, int na)
{
    float mid = GFX_CY;
    float margin = GFX_S(38);
    float span = GFX_W - 2 * margin;
    float slot = span / NBARS;
    float bw = slot * 0.5f;
    float maxh = GFX_S(46);

    for (int i = 0; i < NBARS; i++) {
        float target = (have_mic && i < na) ? lv[i] / 255.0f : 0.0f;
        s_wave[i] += (target - s_wave[i]) * 0.4f;
        float h = GFX_S(2) + s_wave[i] * maxh;
        float x = margin + i * slot + (slot - bw) * 0.5f;
        uint16_t c = have_mic ? GFX_RED : GFX_DGREY;
        gfx_fill_rect(fb, (int)x, (int)(mid - h), (int)bw, (int)(h * 2), c);
    }
}

static void draw_recording(gfx_fb *fb)
{
    /* pulsing record dot + elapsed clock */
    float pulse = 0.5f + 0.5f * sinf(hal_millis() / 340.0f);
    gfx_fill_circle(fb, GFX_S(70), GFX_S(50),
                    GFX_S(5) + pulse * GFX_S(3), GFX_RED);

    char buf[12];
    fmt_mmss(buf, sizeof buf, s_elapsed_ms / 1000u, 1);
    font_draw_centered(fb, &font_big, (int)GFX_CX, (int)GFX_S(64),
                       buf, GFX_WHITE);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(96),
                       "RECORDING", GFX_RED);

    uint8_t lv[NBARS];
    int na = hal_audio_bars(lv, NBARS);
    draw_waveform(fb, na > 0, lv, na);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(160),
                       na > 0 ? "" : "NO MIC", GFX_GREY);

    draw_stop_button(fb, STOP_CY);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(228),
                       "TAP TO STOP", GFX_DGREY);
}

static void on_draw(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);
    if (!hal_audio_available()) {
        font_draw_centered(fb, &font_medium, (int)GFX_CX, (int)GFX_CY, "Microphone needed", GFX_WHITE);
        font_draw_centered(fb, &font_small, (int)GFX_CX, (int)(GFX_CY + GFX_S(25)), "External audio not configured", GFX_GREY);
        return;
    }
    if (s_state == ST_REC) draw_recording(fb);
    else                   draw_idle(fb);
}

/* squared distance from (x,y) to screen centre-x at height cy */
static int hit_button(int x, int y, float cy, float r)
{
    float dx = x - GFX_CX, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

static int on_event(const wos_event *ev)
{
    if (!hal_audio_available()) return 0;
    switch (ev->type) {
    case WOS_EV_TICK:
        if (s_state == ST_REC) {
            s_elapsed_ms += (unsigned)ev->x;
            return 1;
        }
        return 0;

    case WOS_EV_TAP:
        if (s_state == ST_IDLE) {
            if (hit_button(ev->x, ev->y, REC_CY, REC_R)) {
                s_state = ST_REC;
                s_elapsed_ms = 0;
                for (int i = 0; i < NBARS; i++) s_wave[i] = 0.0f;
                hal_haptic(20);
                return 1;
            }
        } else { /* ST_REC */
            if (hit_button(ev->x, ev->y, STOP_CY, STOP_R)) {
                take_append((s_elapsed_ms + 500u) / 1000u);  /* round to s */
                s_state = ST_IDLE;
                hal_haptic(20);
                return 1;
            }
        }
        return 0;

    default:
        return 0;
    }
}

const wos_app app_recorder = {
    "Recorder", on_start, on_event, on_draw, GFX_RED, ICON_MIC
};

#else  /* ===================== SELFTEST ================================== */
#include <assert.h>
#include <string.h>

int main(void)
{
    char b[12];

    /* mm:ss formatting */
    fmt_mmss(b, sizeof b, 0,   1); assert(!strcmp(b, "00:00"));
    fmt_mmss(b, sizeof b, 12,  0); assert(!strcmp(b, "0:12"));
    fmt_mmss(b, sizeof b, 47,  0); assert(!strcmp(b, "0:47"));
    fmt_mmss(b, sizeof b, 75,  0); assert(!strcmp(b, "1:15"));
    fmt_mmss(b, sizeof b, 75,  1); assert(!strcmp(b, "01:15"));
    fmt_mmss(b, sizeof b, 3599,1); assert(!strcmp(b, "59:59"));

    /* seed + append logic */
    takes_seed();
    assert(s_ntakes == 2);
    assert(s_takes[0].index == 1 && s_takes[0].dur == 12);
    assert(s_takes[1].index == 2 && s_takes[1].dur == 47);

    assert(take_append(75) == 1);
    assert(s_ntakes == 3);
    assert(s_takes[2].index == 3 && s_takes[2].dur == 75);
    fmt_mmss(b, sizeof b, s_takes[2].dur, 0); assert(!strcmp(b, "1:15"));

    /* capacity guard: fill to TAKES_CAP, further appends refused */
    while (s_ntakes < TAKES_CAP) assert(take_append(5) == 1);
    assert(s_ntakes == TAKES_CAP);
    assert(take_append(9) == 0);
    assert(s_ntakes == TAKES_CAP);

    printf("SELFTEST OK: mm:ss format + takes-append verified\n");
    return 0;
}
#endif
