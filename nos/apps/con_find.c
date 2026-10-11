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
#define STATE_Y   346             /* [ RINGING ] */
#define HINT_Y    382             /* the hint keeps its place in both states */
#define SWAP_S    0.24f           /* state words: old out, then new in (4 steps each) */
#define HOLD_S    (2.5f / NOS_FPS)   /* a quick tap shows its pressed fill >= 3 frames */

static struct {
    float t_enter;
    float build;           /* entry 0..1 */
    int   connected;
    int   active;          /* phone ringing (as shown) */
    float t_active;        /* when ringing started */
    mtween fill;           /* white disc growth 0..1 */
    int   pressed;
    float t_down;          /* press start (the fill holds >= 3 frames) */
    float t_swap;          /* last ringing on/off: the state words change in sequence */
    float err_t;           /* last refused tap (not connected) */
    float txt;             /* text alpha, quantised */
} F;

static void find_enter(void)
{
    F.t_enter = nos_enter_t();            /* build from 40% of the opening iris */
    F.build = 0;
    F.pressed = 0;
    F.t_down = -10;
    F.t_swap = -10;
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
    F.t_swap = nos_t;
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
                         int nrings, float period, int ndots, float d0)
{
    for (int i = 0; i < nrings; i++) {
        float ph = t / period - (float)i / (float)nrings;
        if (ph < 0) continue;                     /* not emitted yet */
        ph -= floorf(ph);
        float r = r0 + (r1 - r0) * ph;
        float d = d0 * (1.0f - 0.45f * ph);
        uint16_t col = grey_step(ph);
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
        draw_ripples(fb, UI_CX, FB_CY, t, RIP_R0, RIP_R1, RIP_N, RIP_PER, RIP_DOTS, 7.0f);
    }
    /* the button is there at full size; its glyph builds; ringing fills it white */
    int pr = F.pressed || nos_t - F.t_down < HOLD_S;
    ui_circle(fb, UI_CX, FB_CY, FB_R, pr && fillp < 0.5f ? C_LINE2 : C_SURF2);
    float wr = FB_R * q(fillp, 16);
    if (wr > 1) ui_circle(fb, UI_CX, FB_CY, wr, pr && fillp >= 0.5f ? C_SEC : C_WHITE);
    uint16_t ink = fillp > 0.5f ? C_BG : C_PRI;
    dot_icon_anim(fb, DOT_ICON_PHONE, UI_CX, FB_CY, 7.0f, 5.6f, ink, F.build, DOT_REVEAL_CENTER);
    /* the hint stays where it is; the words change in sequence (old out, then new in) */
    float m = (nos_t - F.t_swap) / SWAP_S;
    float fout = q(1.0f - 2.0f * m, 4), fin = q(2.0f * m - 1.0f, 4);
    const char *h_new = F.active ? "TAP TO STOP" : "TAP TO RING", *h_old = F.active ? "TAP TO RING" : "TAP TO STOP";
    if (m >= 1) {
        if (F.active) ui_state(fb, UI_CX, STATE_Y, "RINGING", C_PRI);
        ui_label(fb, UI_CX, HINT_Y, h_new, C_SEC, UI_CENTER);
        return;
    }
    if (fout > 0) {
        if (!F.active) ui_state(fb, UI_CX, STATE_Y, "RINGING", ui_dim(C_PRI, fout));
        ui_label(fb, UI_CX, HINT_Y, h_old, ui_dim(C_SEC, fout), UI_CENTER);
    }
    if (fin > 0) {
        if (F.active) ui_state(fb, UI_CX, STATE_Y, "RINGING", ui_dim(C_PRI, fin));
        ui_label(fb, UI_CX, HINT_Y, h_new, ui_dim(C_SEC, fin), UI_CENTER);
    }
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
        if (F.pressed) F.t_down = nos_t;
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
        F.t_down = -10;
        /* fallthrough */
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
#define FW_RINGS  4
#define FW_DOTS   56
#define FW_PER    1.2f
#define FW_R0     (FW_R + 14.0f)
#define FW_R1     224.0f
#define FW_D0     11.0f
#define FW_CHUNK  8            /* dots per damage command */
#define FW_TITLE_Y 70          /* the words the ripples keep clear of (baselines) */
#define FW_HINT_Y  402

typedef struct { float x, y, d; uint16_t c; } fw_dot;

static struct {
    float t_enter;
    float build;
    int   was_active;
    int   stopped;
    float pulse;           /* disc breathing, quantised */
    fw_dot (*dot)[FW_DOTS];         /* this frame's ripple dots, PSRAM (d 0 = skipped) */
    int   n[FW_RINGS];     /* dots in each ring (0 = not emitted yet) */
    int   tw, hw;          /* title / hint label widths */
} W;

/* A dot keeps out of a word's band: whole dots go, never a clipped half-dot. */
static int fw_clear(float x, float y, float r)
{
    const float cap = 13.0f, pad = 8.0f;      /* label cap height, air around the words */
    if (y + r > FW_TITLE_Y - cap - pad && y - r < FW_TITLE_Y + pad && fabsf(x - UI_CX) - r < W.tw * 0.5f + pad + 6)
        return 0;
    if (y + r > FW_HINT_Y - cap - pad && y - r < FW_HINT_Y + pad && fabsf(x - UI_CX) - r < W.hw * 0.5f + pad + 6)
        return 0;
    return 1;
}

/* The ripple dots for this frame (tick: draw only reads them). */
static void fw_ripples(float t)
{
    if (!W.dot) W.dot = (fw_dot (*)[FW_DOTS])hal_big_alloc(sizeof(fw_dot) * FW_RINGS * FW_DOTS);
    for (int i = 0; i < FW_RINGS; i++) {
        float ph = t / FW_PER - (float)i / (float)FW_RINGS;
        W.n[i] = 0;
        if (ph < 0 || !W.dot) continue;           /* not emitted yet */
        ph -= floorf(ph);
        float r = FW_R0 + (FW_R1 - FW_R0) * ph;
        float d = FW_D0 * (1.0f - 0.45f * ph);
        uint16_t col = ph < 0.45f ? C_WHITE : ph < 0.7f ? C_PRI : ph < 0.85f ? C_SEC : C_DIS;
        W.n[i] = FW_DOTS;
        for (int k = 0; k < FW_DOTS; k++) {
            float a = 6.2831853f * (float)k / FW_DOTS;
            fw_dot *o = &W.dot[i][k];
            o->x = UI_CX + r * gfx_fast_sin(a);
            o->y = UI_CY - r * gfx_fast_cos(a);
            o->d = fw_clear(o->x, o->y, d * 0.5f) ? d : 0;
            o->c = col;
        }
    }
}

/* Recorded per chunk of dots (box + hash), stamped only where they meet the stripe. */
static void fw_draw_ripples(gfx_fb *fb)
{
    int lo = fb->y_base - fb->oy, hi = lo + fb->h;
    for (int i = 0; i < FW_RINGS; i++)
        for (int k0 = 0; k0 < W.n[i]; k0 += FW_CHUNK) {
            const fw_dot *o = &W.dot[i][k0];
            int n = W.n[i] - k0 < FW_CHUNK ? W.n[i] - k0 : FW_CHUNK;
            if (fb->rec) {
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                unsigned h = 0xF1D0u;
                for (int j = 0; j < n; j++) {
                    if (o[j].d <= 0) continue;
                    float e = o[j].d * 0.5f + 1.0f;
                    if (o[j].x - e < x0) x0 = o[j].x - e;
                    if (o[j].y - e < y0) y0 = o[j].y - e;
                    if (o[j].x + e > x1) x1 = o[j].x + e;
                    if (o[j].y + e > y1) y1 = o[j].y + e;
                    h = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_u(h, o[j].c), o[j].x), o[j].y), o[j].d);
                }
                if (x1 > x0) gfx_rec_cmd(fb, (int)floorf(x0), (int)floorf(y0), (int)ceilf(x1), (int)ceilf(y1), h);
                continue;
            }
            for (int j = 0; j < n; j++) {
                float e = o[j].d * 0.5f + 1.0f;
                if (o[j].d > 0 && o[j].y + e >= lo && o[j].y - e <= hi) dot_disc(fb, o[j].x, o[j].y, o[j].d, o[j].c);
            }
        }
}

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
    W.tw = ui_label_width("FIND WATCH");
    W.hw = ui_label_width("TAP TO STOP");
    fw_ripples(0);
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
    fw_ripples(nos_t - W.t_enter);
}

static void fw_draw(gfx_fb *fb)
{
    fw_draw_ripples(fb);
    ui_circle(fb, UI_CX, UI_CY, FW_R + 8.0f * W.pulse, C_WHITE);
    dot_icon_anim(fb, DOT_ICON_BELL, UI_CX, UI_CY, 8.0f, 6.4f, C_BG, W.build, DOT_REVEAL_CENTER);
    /* the ripples leave whole-dot gaps around the words */
    ui_title(fb, "FIND WATCH", 0);
    ui_label(fb, UI_CX, FW_HINT_Y, "TAP TO STOP", C_PRI, UI_CENTER);
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
