/* con_find.c — Find my phone, and the screen shown when the phone looks for the watch.
 *
 * scr_find: one big round button. Tapping it rings the phone (svc_find_phone) and
 * sends dot ripples outward from the button: rings of dots at growing radii whose
 * colour steps down the grey ladder as they travel. Tap again (anywhere) to stop.
 *
 * scr_find_watch: pushed by the services when the phone rings the watch. Bright
 * white dot ripples across the whole face at full panel brightness until a tap (the
 * services' ringer loops the buzz and chime meanwhile). */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include <math.h>

static float q(float x, float n) { return floorf(m_clamp(x, 0, 1) * n + 0.5f) / n; }

static uint16_t grey_step(float k)        /* k 0 (bright) .. 1 (gone): 5 discrete steps */
{
    static const uint16_t ladder[] = { C_PRI, C_SEC, C_DIS, C_LINE2, C_LINE };
    int i = (int)(k * 5.0f);
    if (i < 0) i = 0;
    if (i > 4) i = 4;
    return ladder[i];
}

/* ======================================================= find my phone ===== */
#define FB_CY     206
#define FB_R      72.0f
#define RIP_R0    (FB_R + 10.0f)
#define RIP_R1    120.0f          /* outer ring stays clear of the title */
#define RIP_N     3
#define RIP_PER   1.8f
#define RIP_DOTS  40

static struct {
    float t_enter;
    float build;           /* entry 0..1 */
    int   connected;
    int   active;          /* phone ringing (as shown) */
    float t_active;        /* when ringing started */
    mtween fill;           /* white disc growth 0..1 */
    int   pressed;
    float err_t;           /* last refused tap (not connected) */
    float txt;             /* text alpha, quantised */
} F;

static void find_enter(void)
{
    F.t_enter = nos_t + 0.4f * 0.42f;     /* build from 40% of the opening iris */
    F.build = 0;
    F.pressed = 0;
    F.err_t = -10;
    F.active = svc_find_phone_active();
    F.t_active = nos_t;
    F.connected = svc_sys()->phone_connected || hal_phone_connected();
    mtw_start(&F.fill, F.active ? 1 : 0, F.active ? 1 : 0, 0.01f, &EASE_MECH);
}

static void find_leave(void)
{
    if (F.active) { svc_find_phone(0); F.active = 0; }
}

static void set_active(int on)
{
    if (on == F.active) return;
    F.active = on;
    if (on) F.t_active = nos_t;
    float cur = mtw_value(&F.fill);
    mtw_start(&F.fill, cur, on ? 1.0f : 0.0f, 0.20f, &EASE_MECH);
}

static void find_tick(float dt)
{
    F.build = q((nos_t - F.t_enter) / UI_ENTRY_S, 32);
    F.connected = svc_sys()->phone_connected || hal_phone_connected();
    if (F.active && !F.connected) { svc_find_phone(0); set_active(0); }
    /* the phone may stop ringing on its own (found, or its timeout) */
    if (F.active && !svc_find_phone_active() && nos_t - F.t_active > 1.0f) set_active(0);
    F.txt = nos_t >= F.t_enter ? 1.0f : 0.0f;
    if (F.active) nos_keep_awake();
}

static void draw_ripples(gfx_fb *fb, float cx, float cy, float t, float r0, float r1,
                         int nrings, float period, int ndots, float d0, int bright)
{
    for (int i = 0; i < nrings; i++) {
        float ph = t / period - (float)i / (float)nrings;
        if (ph < 0) continue;                     /* not emitted yet */
        ph -= floorf(ph);
        float r = r0 + (r1 - r0) * ph;
        float d = d0 * (1.0f - 0.45f * ph);
        uint16_t col = bright ? (ph < 0.45f ? C_WHITE : ph < 0.7f ? C_PRI : ph < 0.85f ? C_SEC : C_DIS)
                              : grey_step(ph);
        dot_ring(fb, cx, cy, r, ndots, d, 0, (float)ndots, col, 0);
    }
}

static void find_draw(gfx_fb *fb)
{
    ui_title(fb, "FIND PHONE", 0);
    if (!F.connected) {
        ui_absent(fb, DOT_ICON_BT, "Phone not connected", "Pair in Gadgetbridge as Bangle.js NOS", NULL, F.build);
        return;
    }
    if (nos_t < F.t_enter) return;
    float fillp = mtw_value(&F.fill);
    if (F.active || fillp > 0.01f) {
        float t = nos_t - F.t_active;
        draw_ripples(fb, UI_CX, FB_CY, t, RIP_R0, RIP_R1, RIP_N, RIP_PER, RIP_DOTS, 7.0f, 0);
    }
    /* the button is there at full size; its glyph builds; ringing fills it white */
    ui_circle(fb, UI_CX, FB_CY, FB_R, F.pressed && !F.active ? C_LINE2 : C_SURF2);
    float wr = FB_R * q(fillp, 16);
    if (wr > 1) ui_circle(fb, UI_CX, FB_CY, wr, F.pressed && F.active ? C_SEC : C_WHITE);
    uint16_t ink = fillp > 0.5f ? C_BG : C_PRI;
    dot_icon_anim(fb, DOT_ICON_PHONE, UI_CX, FB_CY, 7.0f, 5.6f, ink, F.build, DOT_REVEAL_CENTER);
    if (F.active) {
        ui_state(fb, UI_CX, 346, "RINGING", C_PRI);
        ui_label(fb, UI_CX, 382, "TAP TO STOP", C_SEC, UI_CENTER);
    } else ui_label(fb, UI_CX, 362, "TAP TO RING", C_SEC, UI_CENTER);
}

static int in_button(int x, int y)
{
    float dx = x - UI_CX, dy = y - FB_CY;
    return dx * dx + dy * dy <= (FB_R + 20) * (FB_R + 20);
}

static int find_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN:
        F.pressed = F.connected && (in_button(in->x, in->y) || F.active);
        return 1;
    case IN_TAP:
        F.pressed = 0;
        if (F.active) {
            svc_find_phone(0);
            set_active(0);
            nos_feedback(FB_TOGGLE_OFF);
        } else if (in_button(in->x, in->y)) {
            if (!F.connected) { nos_feedback(FB_ERROR); return 1; }
            svc_find_phone(1);
            set_active(1);
            nos_feedback(FB_TOGGLE_ON);
        }
        return 1;
    case IN_DRAG_START:
    case IN_UP:
        F.pressed = 0;
        return 0;
    default:
        return 0;
    }
}

const nos_screen scr_find = { "FIND PHONE", find_enter, find_leave, find_tick, find_draw, find_input, 0 };

/* ====================================================== find my watch ===== */
#define FW_R      84.0f

static struct {
    float t_enter;
    float build;
    int   was_active;
    int   stopped;
    float pulse;           /* disc breathing, quantised */
} W;

static void fw_restore(void)
{
    hal_haptic_stop();
    hal_sfx_stop();
    nos_brightness_override(-1);
}

static void fw_enter(void)
{
    W.t_enter = nos_t;
    W.build = 0;
    W.was_active = svc_find_watch_active();
    W.stopped = 0;
    nos_brightness_override(255);
}

static void fw_leave(void)
{
    fw_restore();
}

static void fw_stop(void)
{
    if (W.stopped) return;
    W.stopped = 1;
    svc_find_watch_stop();
    fw_restore();
    nos_feedback(FB_SUCCESS);
    nos_pop();
}

static void fw_tick(float dt)
{
    nos_keep_awake();
    W.build = q((nos_t - W.t_enter) / UI_ENTRY_S, 32);
    if (W.stopped) return;
    if (svc_find_watch_active()) W.was_active = 1;
    else if (W.was_active) { W.stopped = 1; fw_restore(); nos_pop(); return; }   /* the phone gave up */
    /* the disc swells as each ripple leaves it */
    float ph = (nos_t - W.t_enter) / 1.2f;
    ph -= floorf(ph);
    float s = ph < 0.15f ? ph / 0.15f : ph < 0.5f ? 1.0f - (ph - 0.15f) / 0.35f : 0.0f;
    W.pulse = q(s, 8);
}

static void fw_draw(gfx_fb *fb)
{
    float t = nos_t - W.t_enter;
    draw_ripples(fb, UI_CX, UI_CY, t, FW_R + 14, 224, 4, 1.2f, 56, 11.0f, 1);
    ui_circle(fb, UI_CX, UI_CY, FW_R + 8.0f * W.pulse, C_WHITE);
    dot_icon_anim(fb, DOT_ICON_BELL, UI_CX, UI_CY, 8.0f, 6.4f, C_BG, W.build, DOT_REVEAL_CENTER);
    /* black pills keep the words legible over the ripples */
    int tw = ui_label_width("FIND WATCH");
    ui_rrect(fb, UI_CX - tw / 2 - 20, 48, tw + 40, 32, 16, C_BG);
    ui_title(fb, "FIND WATCH", 0);
    int bw = ui_label_width("TAP TO STOP");
    ui_rrect(fb, UI_CX - bw / 2 - 22, 374, bw + 44, 44, 22, C_BG);
    ui_label(fb, UI_CX, 402, "TAP TO STOP", C_PRI, UI_CENTER);
}

static int fw_input(const nos_input *in)
{
    switch (in->type) {
    case IN_TAP:
    case IN_BTN:
    case IN_LONG:
        fw_stop();
        return 1;
    case IN_DOWN: case IN_UP: case IN_DRAG_START: case IN_DRAG: case IN_DRAG_END:
        return 1;              /* nothing else to do here: swallow everything */
    default:
        return 0;
    }
}

const nos_screen scr_find_watch = { "FIND WATCH", fw_enter, fw_leave, fw_tick, fw_draw, fw_input,
                                    SCR_NOBACK | SCR_KEEPAWAKE };
