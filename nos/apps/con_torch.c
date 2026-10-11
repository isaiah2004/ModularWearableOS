/* con_torch.c — Torch: the whole panel as a light.
 *
 * White at full brightness; a tap switches to a dim, warm red night-vision light.
 * A vertical drag sets the level in 10% detents (a small dot gauge shows while the
 * finger is down). Entry and mode changes are ordered-dither dissolves of ~150 ms,
 * never a hard cut to white. The panel brightness is restored on leave. */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include "gfx_cost.h"
#include <math.h>

#define TORCH_RED   0x7800          /* #780000: deep warm red, kind to dark-adapted eyes */
#define DISSOLVE_S  0.15f
#define HINT_S      2.6f
#define GAUGE_N     10

static struct {
    int   mode;            /* 0 white, 1 red */
    float level[2];        /* 0.1 .. 1.0 per mode */
    uint16_t from, to;     /* dissolve colours */
    float p;               /* dissolve progress 0..1 (quantised) */
    float t0;              /* dissolve start (nos_t), < 0 = waiting for the transition */
    int   started;
    float hint;            /* hint label alpha 0..1 (quantised) */
    int   hint_i;          /* 0 = tap hint, 1 = drag hint */
    float t_enter;
    int   dragging;
    float drag_level0;
    int   last_step;
    float gauge;           /* gauge alpha 0..1 (quantised) */
    float t_gauge;
    uint8_t bl;            /* last backlight written */
} T;

static uint16_t mode_color(int m) { return m ? TORCH_RED : C_WHITE; }

static uint8_t backlight_for(int m, float level)
{
    float k = m ? level * 0.55f : level;            /* night mode stays dim */
    int v = (int)lroundf(255.0f * k);
    if (v < 12) v = 12;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

static void set_bl(uint8_t v)
{
    if (v != T.bl) { T.bl = v; nos_brightness_override(v); }
}

static void torch_enter(void)
{
    T.mode = 0;
    T.level[0] = 1.0f;
    if (T.level[1] <= 0) T.level[1] = 0.5f;
    T.from = C_BG; T.to = C_WHITE;
    T.p = 0; T.t0 = -1; T.started = 0;
    T.hint = 0; T.t_enter = nos_t;
    T.dragging = 0; T.gauge = 0; T.t_gauge = -10;
    T.bl = 0;
    nos_prefs.torch = 1;
}

static void torch_leave(void)
{
    nos_brightness_override(-1);
    nos_prefs.torch = 0;
}

static float q16(float x) { return floorf(m_clamp(x, 0, 1) * 16.0f + 0.5f) / 16.0f; }

static void torch_tick(float dt)
{
    nos_keep_awake();
    /* the dissolve starts with the opening iris: the iris edge and the dither read as
     * one soft switch-on, with no black beat and never a white flash */
    if (!T.started) {
        T.started = 1;
        T.t0 = nos_t;
        set_bl(backlight_for(T.mode, T.level[T.mode]));
    }
    if (T.started) {
        float p = (nos_t - T.t0) / DISSOLVE_S;
        T.p = q16(bezier_ease(&EASE_STANDARD, m_clamp(p, 0, 1)));
        if (T.p >= 1) T.from = T.to;
    }
    /* hints, once per visit: the tap, then the drag (4-step fades) */
    float age = nos_t - T.t_enter;
    T.hint_i = age < HINT_S + 0.3f ? 0 : 1;
    float a0 = T.hint_i ? HINT_S + 0.45f : 0.35f;
    float h = m_clamp((age - a0) / 0.2f, 0, 1) * (1.0f - m_clamp((age - a0 - HINT_S + 0.35f) / 0.3f, 0, 1));
    T.hint = floorf(h * 4.0f + 0.5f) / 4.0f;
    float g = T.dragging ? 1.0f : 1.0f - m_clamp((nos_t - T.t_gauge - 0.6f) / 0.25f, 0, 1);
    T.gauge = floorf(g * 4.0f + 0.5f) / 4.0f;
    if (T.started) set_bl(backlight_for(T.mode, T.level[T.mode]));
}

/* 8x8 Bayer thresholds 0..63 (same order as the engine's dissolve) */
static const uint8_t BAYER[8][8] = {
    { 0,32, 8,40, 2,34,10,42}, {48,16,56,24,50,18,58,26}, {12,44, 4,36,14,46, 6,38},
    {60,28,52,20,62,30,54,22}, { 3,35,11,43, 1,33, 9,41}, {51,19,59,27,49,17,57,25},
    {15,47, 7,39,13,45, 5,37}, {63,31,55,23,61,29,53,21} };

static void fill_dither(gfx_fb *fb, uint16_t a, uint16_t b, float p)
{
    int thr = (int)(p * 64.0f + 0.5f);
    if (fb->rec) {
        unsigned h = gfx_hash_u(gfx_hash_u(gfx_hash_u(0x70C4u, a), b), (unsigned)thr);
        gfx_rec_cmd(fb, -fb->ox, -fb->oy, GFX_W - fb->ox, GFX_H - fb->oy, h);
        return;
    }
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (c0 < 0) c0 = 0;
    if (c1 > GFX_W) c1 = GFX_W;
    for (int r = 0; r < fb->h; r++) {
        int ly = fb->y_base + r - fb->oy;
        uint16_t *row = gfx_row(fb, r);
        const uint8_t *bay = BAYER[ly & 7];
        for (int bx = c0; bx < c1; bx++) {
            int lx = bx - fb->ox;
            row[bx] = bay[lx & 7] < thr ? b : a;
        }
    }
    GFX_COST(GFX_COST_SELECT, (unsigned)(fb->h * (c1 > c0 ? c1 - c0 : 0)));
}

static void torch_draw(gfx_fb *fb)
{
    uint16_t bg;
    if (T.p >= 1 || T.from == T.to) { bg = T.to; gfx_fill_rect(fb, 0, 0, GFX_W, GFX_H, bg); }
    else { bg = T.p < 0.5f ? T.from : T.to; fill_dither(fb, T.from, T.to, T.p); }
    if (T.p < 1) return;
    /* ink a few steps off the light colour: readable, never loud */
    uint16_t ink = T.mode ? 0x3000 : 0xA514;        /* #300000 / #A0A0A0 */
    if (T.gauge > 0) {
        float lv = T.level[T.mode];
        /* red mode: black lit dots on a darker-red track, black figures */
        uint16_t on = ui_mix(bg, T.mode ? C_BG : C_LINE2, T.gauge);
        uint16_t off = ui_mix(bg, T.mode ? 0x3800 : 0xD69A, T.gauge);
        const float pitch = 16, d = 9;
        float x0 = UI_CX - pitch * (GAUGE_N - 1) * 0.5f;
        dot_bar(fb, x0, 392, GAUGE_N, pitch, d, lv * GAUGE_N + 0.001f, on, off);
        char b[8];
        int pct = (int)lroundf(lv * 100.0f);
        b[0] = 0;
        {   /* "80%" without pulling printf into the hot path */
            int i = 0;
            if (pct >= 100) b[i++] = '1';
            if (pct >= 10) b[i++] = (char)('0' + (pct / 10) % 10);
            b[i++] = (char)('0' + pct % 10);
            b[i++] = '%'; b[i] = 0;
        }
        ui_label(fb, UI_CX, 366, b, ui_mix(bg, T.mode ? C_BG : C_DIS, T.gauge), UI_CENTER);
    } else if (T.hint > 0) {
        const char *h = T.hint_i ? "DRAG TO DIM" : T.mode ? "TAP FOR WHITE" : "TAP FOR RED LIGHT";
        ui_label(fb, UI_CX, 404, h, ui_mix(bg, T.mode ? C_BG : ink, T.hint), UI_CENTER);
    }
}

static void switch_mode(void)
{
    T.from = mode_color(T.mode);
    T.mode ^= 1;
    T.to = mode_color(T.mode);
    T.t0 = nos_t; T.p = 0; T.started = 1;
    nos_feedback(T.mode ? FB_TOGGLE_ON : FB_TOGGLE_OFF);
}

static int torch_input(const nos_input *in)
{
    switch (in->type) {
    case IN_TAP:
        switch_mode();
        return 1;
    case IN_DRAG_START:
        if (in->axis != AXIS_Y) return 0;
        T.dragging = 1;
        T.drag_level0 = T.level[T.mode];
        T.last_step = (int)lroundf(T.level[T.mode] * 10.0f);
        /* fallthrough */
    case IN_DRAG: {
        if (!T.dragging) return 0;
        /* 260 px of travel = the full range; up = brighter */
        float lv = m_clamp(T.drag_level0 - in->dy / 260.0f, 0.1f, 1.0f);
        int step = (int)lroundf(lv * 10.0f);
        if (step != T.last_step) {
            nos_feedback((step == 1 || step == 10) ? FB_LIMIT : FB_TICK);
            T.last_step = step;
        }
        T.level[T.mode] = (float)step / 10.0f;    /* detented: the gauge and panel agree */
        return 1;
    }
    case IN_DRAG_END:
        if (!T.dragging) return 0;
        T.dragging = 0;
        T.t_gauge = nos_t;
        return 1;
    case IN_UP:
        if (T.dragging) { T.dragging = 0; T.t_gauge = nos_t; }
        return 0;
    default:
        return 0;
    }
}

const nos_screen scr_torch = { "TORCH", torch_enter, torch_leave, torch_tick, torch_draw, torch_input,
                               SCR_OPAQUE_BG | SCR_KEEPAWAKE };
