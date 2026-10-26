/* con_torch.c — Torch: the whole panel as a light.
 *
 * White at full brightness; a tap switches to a dim, warm red night-vision light.
 * A vertical drag sets the level in 10% detents (a small dot gauge shows while the
 * finger is down). Entry and mode changes are flat 4-frame colour fades in perceptual
 * steps (12, 38, 62, 100 %: m_ink), never a hard cut to white and no dither. The panel
 * brightness is restored on leave. */
#include "nos.h"
#include "diag_log.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "power_policy.h"
#include "apps/nos_apps.h"
#include <math.h>

#define TORCH_RED   0x7800          /* #780000: deep warm red, kind to dark-adapted eyes */
#define TORCH_FIELD 0x9800          /* #9A0000: the field under the gauge in red mode */
#define TORCH_UNLIT 0x6000          /* #600000: unlit gauge dots in red mode (10 % reads apart from 100 %) */
#define FADE_F      4               /* the light's colour fade, whole frames */
#define HINT_S      2.6f
#define GAUGE_N     10

static struct {
    int   mode;            /* 0 white, 1 red */
    float level[2];        /* 0.1 .. 1.0 per mode */
    uint16_t from, to;     /* fade colours */
    float p;               /* fade progress 0..1 (4 steps) */
    nos_time_t t0;              /* fade start (nos_t), < 0 = waiting for the transition */
    int   started;
    float hint;            /* hint label alpha 0..1 (quantised) */
    int   hint_i;          /* 0 = tap hint, 1 = drag hint */
    nos_time_t t_enter;
    int   dragging;
    float drag_level0;
    int   last_step;
    float gauge;           /* gauge alpha 0..1 (quantised) */
    nos_time_t t_gauge;
    uint8_t bl;            /* last backlight written */
    uint32_t t_input;      /* hal_millis() of the last touch: the auto-off clock (G29) */
    int   low;             /* low-battery latch (power_low_update) */
    int   dim;             /* low battery: backlight scaled down */
} T;

static uint16_t mode_color(int m) { return m ? TORCH_RED : C_WHITE; }

static uint8_t backlight_for(int m, float level)
{
    float k = m ? level * 0.55f : level;            /* night mode stays dim */
    if (T.dim) k *= 0.4f;                           /* low battery */
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
    if (nos_revealed()) {
        T.dragging = 0;
        T.bl = 0;                 /* the covering alert released our override */
        T.t_input = hal_millis();
        nos_prefs.torch = 1;
        set_bl(backlight_for(T.mode, T.level[T.mode]));
        return;
    }
    T.mode = 0;
    T.level[0] = 1.0f;
    if (T.level[1] <= 0) T.level[1] = 0.5f;
    T.from = C_BG; T.to = C_WHITE;
    T.p = 0; T.t0 = -1; T.started = 0;
    T.hint = 0; T.t_enter = nos_t;
    T.dragging = 0; T.gauge = 0; T.t_gauge = -10;
    T.bl = 0;
    T.t_input = hal_millis();
    T.low = T.dim = 0;
    nos_prefs.torch = 1;
}

static void torch_leave(void)
{
    nos_brightness_override(-1);
    nos_prefs.torch = 0;
}

static void torch_tick(float dt)
{
    nos_keep_awake();
    /* G29: a forgotten torch closes itself (5 min without a touch); on low battery it dims and closes
     * after 1 min, and at the critical level it closes at once. Closing restores the panel level and
     * the normal screen timeout takes over. */
    hal_power pw; hal_power_get(&pw);
    T.low = power_low_update(T.low, &pw);
    int state = power_torch_state(T.low, &pw, hal_millis() - T.t_input);
    T.dim = state == NOS_TORCH_DIM;
    if (state == NOS_TORCH_CLOSE) { if (!nos_transitioning()) nos_pop(); return; }
    /* the fade starts with the opening iris: the iris edge and the fade read as one
     * soft switch-on, with no black beat and never a white flash */
    if (!T.started) {
        T.started = 1;
        T.t0 = nos_t;
        set_bl(backlight_for(T.mode, T.level[T.mode]));
    }
    if (T.started) {
        /* 4 flat perceptual steps, the first on the frame the fade starts */
        T.p = m_ink(m_clamp((float)(m_frame_k(T.t0) + 1) / FADE_F, 0, 1));
        if (T.p >= 1) T.from = T.to;
    }
    /* hints, once per visit: the tap, then the drag (in over 4 frames, out over 6,
     * perceptual steps) */
    float age = nos_t - T.t_enter;
    T.hint_i = age < HINT_S + 0.3f ? 0 : 1;
    float a0 = T.t_enter + (T.hint_i ? HINT_S + 0.45f : 0.35f);
    float h_in = m_clamp((float)(m_frame_k(a0) + 1) / 4.0f, 0, 1);
    float h_out = m_clamp((float)(m_frame_k(a0 + HINT_S - 0.35f) + 1) / 6.0f, 0, 1);
    T.hint = nos_t + 0.5f / NOS_FPS < a0 ? 0.0f : m_ink(h_in < 1.0f - h_out ? h_in : 1.0f - h_out);
    /* the gauge shows at once under the finger and fades out over 6 frames, 0.6 s after */
    T.gauge = T.dragging ? 1.0f : m_ink(1.0f - m_clamp((float)(m_frame_k(T.t_gauge + 0.6f) + 1) / 6.0f, 0, 1));
    if (T.started) set_bl(backlight_for(T.mode, T.level[T.mode]));
#ifdef GFX_COST_MODEL
    nos_motion_probe("torch.light", MC_FADE, 100.0f * T.p);
    nos_motion_probe("torch.hint", MC_FADE, 100.0f * T.hint);
    nos_motion_probe("torch.gauge", MC_FADE, 100.0f * T.gauge);
#endif
}

static void torch_draw(gfx_fb *fb)
{
    uint16_t bg;
    if (T.p >= 1 || T.from == T.to) { bg = T.to; gfx_fill_rect(fb, 0, 0, GFX_W, GFX_H, bg); }
    else { bg = ui_mix(T.from, T.to, T.p); gfx_fill_rect(fb, 0, 0, GFX_W, GFX_H, bg); }
    if (T.p < 1) return;
    /* ink a few steps off the light colour: readable, never loud */
    uint16_t ink = T.mode ? 0x3000 : 0xA514;        /* #300000 / #A0A0A0 */
    if (T.gauge > 0) {
        float lv = T.level[T.mode];
        /* red mode: the gauge sits on a brighter red field (#9A0000) with black lit dots,
         * #600000 unlit ones and black figures, so it reads without lighting the room */
        if (T.mode) ui_rrect(fb, UI_CX - 115, 336, 230, 78, 39, ui_mix(bg, TORCH_FIELD, T.gauge));
        uint16_t on = ui_mix(bg, T.mode ? C_BG : C_LINE2, T.gauge);
        uint16_t off = ui_mix(bg, T.mode ? TORCH_UNLIT : 0xD69A, T.gauge);
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
    uint32_t request = diag_action_request(DIAG_DOMAIN_CONTROL, DIAG_ACT_SET, (int32_t)diag_log_name_id("torch.mode"));
    diag_action_context(request, (int32_t)diag_log_name_id("TORCH"), T.mode);
    T.from = mode_color(T.mode);
    T.mode ^= 1;
    T.to = mode_color(T.mode);
    diag_action_result(request, DIAG_RESULT_SUCCESS, T.mode); /* selected light mode */
    T.t0 = nos_t; T.p = 0; T.started = 1;
    nos_feedback(T.mode ? FB_TOGGLE_ON : FB_TOGGLE_OFF);
}

static void torch_level_trace(int result)
{
    int old = (int)lroundf(T.drag_level0 * 10.0f), value = (int)lroundf(T.level[T.mode] * 10.0f);
    if (old == value) return;
    uint32_t request = diag_action_request(DIAG_DOMAIN_CONTROL, DIAG_ACT_SET, (int32_t)diag_log_name_id("torch.level"));
    diag_action_context(request, (int32_t)diag_log_name_id("TORCH"), old);
    diag_action_result(request, result, value); /* selected detent, no per-frame backlight events */
}
static int torch_input(const nos_input *in)
{
    T.t_input = hal_millis();
    if (in->type == IN_CANCEL) {
        if (T.dragging) torch_level_trace(DIAG_RESULT_CANCELLED);
        T.dragging = 0; T.t_gauge = nos_t; return 1;
    }

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
        torch_level_trace(DIAG_RESULT_SUCCESS);
        T.dragging = 0;
        T.t_gauge = nos_t;
        return 1;
    case IN_UP:
        if (T.dragging) { torch_level_trace(DIAG_RESULT_SUCCESS); T.dragging = 0; T.t_gauge = nos_t; }
        return 0;
    default:
        return 0;
    }
}

const nos_screen scr_torch = { "TORCH", torch_enter, torch_leave, torch_tick, torch_draw, torch_input,
                               SCR_OPAQUE_BG | SCR_KEEPAWAKE | SCR_SELFCAP };
