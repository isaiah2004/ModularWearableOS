/* sys_control.c — control centre (pulled down from the face, TX_DROP).
 *
 *   y  52   10:09 . 78%            (mono; briefly replaced by "WI-FI OFF" after a toggle)
 *   y 124   ( WIFI ) ( BT ) ( DND )
 *   y 218   ( TORCH ) ( AOD ) ( SOUND ) ( HAPTICS )
 *   y 312   ( LTE ) ( GPS ) ( RAISE ) ( SETTINGS )
 *   y 394   [=====brightness=====       ]
 *
 * Everything fits on the glass, so nothing scrolls: an upward drag always belongs to
 * the engine, which closes the sheet with the finger. Tiles: on = white disc with black
 * ink, off = C_SURF2 with white ink; the white grows out of the centre (or shrinks back)
 * on a snappy spring. Absent strap modules show a disabled tile and say why on tap.
 * The brightness slider follows the finger relative to where it was grabbed, ticks every
 * 10 % and drives the backlight live. */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { T_WIFI = 0, T_BT, T_DND, T_TORCH, T_AOD, T_SOUND, T_HAPTIC, T_LTE, T_GPS, T_RAISE, T_SETTINGS, NT };

#define TILE_R   43.0f
#define PITCH_X  96.0f
#define HEAD_Y   56
#define SL_Y     370.0f
#define SL_H     50.0f
#define SL_W     236.0f
#define SL_X     (UI_CX - SL_W / 2)

/* Dim a neutral grey without the RGB565 rounding casts (keeps G = 2R, B = R). */
static uint16_t gdim(uint16_t c, float k)
{
    if (k >= 1.0f) return c;
    if (k <= 0.0f) return 0;
    int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
    if (b != r || (g >> 1) != r) return ui_dim(c, k);       /* not a grey */
    int v = (int)(r * k + 0.5f);
    return (uint16_t)(v << 11 | (v * 2) << 5 | v);
}

static const struct { int icon; const char *name; uint32_t need; } k_tile[NT] = {
    [T_WIFI]     = { DOT_ICON_WIFI,     "Wi-Fi",     0 },
    [T_BT]       = { DOT_ICON_BT,       "Bluetooth", 0 },
    [T_DND]      = { DOT_ICON_DND,      "Do not disturb", 0 },
    [T_TORCH]    = { DOT_ICON_TORCH,    "Torch",     0 },
    [T_AOD]      = { DOT_ICON_EYE,      "Always on", 0 },
    [T_SOUND]    = { DOT_ICON_SPEAKER,  "Sound",     EXT_SPEAKER },
    [T_HAPTIC]   = { DOT_ICON_HAND,     "Haptics",   EXT_HAPTIC },
    [T_LTE]      = { DOT_ICON_SIGNAL,   "LTE",       EXT_LTE },
    [T_GPS]      = { DOT_ICON_PIN,      "GPS",       EXT_GPS },
    [T_RAISE]    = { DOT_ICON_ARROW_UP, "Raise to wake", 0 },
    [T_SETTINGS] = { DOT_ICON_SETTINGS, "Settings",  0 },
};

static struct {
    mspring on[NT];          /* 0..1 white fill */
    int pressed;             /* tile under the finger, -1 none; NT = slider */
    int sl_drag;
    float sl_v0, sl_val;     /* slider value at grab, live value 0..1 */
    int sl_step;             /* last 10 % step (ticks) */
    float t_enter;
    char note[32];           /* "WI-FI OFF" */
    float note_t;
    int bat, charging;
    char time[8];
} C;

static void tile_pos(int i, float *x, float *y)
{
    if (i < 3) { *x = UI_CX + (i - 1) * PITCH_X; *y = 124; }
    else if (i < 7) { *x = UI_CX + (i - 3 - 1.5f) * PITCH_X; *y = 218; }
    else { *x = UI_CX + (i - 7 - 1.5f) * PITCH_X; *y = 312; }
}

static int present(int i) { return !k_tile[i].need || (hal_ext_present() & k_tile[i].need); }

static int state(int i)
{
    switch (i) {
    case T_WIFI: return nos_prefs.wifi;
    case T_BT: return nos_prefs.ble;
    case T_DND: return nos_prefs.dnd;
    case T_TORCH: return nos_prefs.torch;
    case T_AOD: return nos_prefs.aod;
    case T_SOUND: return nos_prefs.sound;
    case T_HAPTIC: return nos_prefs.haptics;
    case T_LTE: return nos_prefs.lte;
    case T_GPS: return nos_prefs.gps_auto;
    case T_RAISE: return nos_prefs.raise_wake;
    default: return 0;
    }
}

static void set_note(const char *name, const char *st)
{
    snprintf(C.note, sizeof C.note, "%s%s%s", name, st ? " " : "", st ? st : "");
    C.note_t = nos_t;
}

static void toggle(int i)
{
    int on = !state(i);
    /* feedback is felt/heard before haptics/sound switch off and after they switch on */
    if (!on) nos_feedback(FB_TOGGLE_OFF);
    switch (i) {
    case T_WIFI: nos_prefs.wifi = (uint8_t)on; break;      /* the services own the radios */
    case T_BT: nos_prefs.ble = (uint8_t)on; break;
    case T_DND: nos_prefs.dnd = (uint8_t)on; break;
    case T_AOD: nos_prefs.aod = (uint8_t)on; break;
    case T_SOUND: nos_prefs.sound = (uint8_t)on; break;
    case T_HAPTIC: nos_prefs.haptics = (uint8_t)on; break;
    case T_LTE: nos_prefs.lte = (uint8_t)on; hal_lte_enable(on); break;
    case T_GPS: nos_prefs.gps_auto = (uint8_t)on; hal_gps_enable(on); break;
    case T_RAISE: nos_prefs.raise_wake = (uint8_t)on; break;
    default: return;
    }
    prefs_save();
    if (on) nos_feedback(FB_TOGGLE_ON);
    msp_to(&C.on[i], on ? 1.0f : 0.0f);
    set_note(i == T_DND ? "DND" : i == T_RAISE ? "RAISE" : i == T_AOD ? "AOD" : k_tile[i].name, on ? "ON" : "OFF");
}

static void apply_brightness(float v)
{
    int pct = (int)lroundf(v * 100.0f);
    if (pct < 5) pct = 5;
    nos_prefs.brightness = (uint8_t)pct;
    hal_set_backlight((uint8_t)(pct * 255 / 100));
}

static void c_enter(void)
{
    for (int i = 0; i < NT; i++) msp_init(&C.on[i], state(i) ? 1.0f : 0.0f, SPRING_SNAPPY);
    C.pressed = -1;
    C.sl_drag = 0;
    C.sl_val = nos_prefs.brightness / 100.0f;
    C.t_enter = nos_t;
    C.note[0] = 0;
}

static void c_leave(void)
{
    if (C.sl_drag) { C.sl_drag = 0; prefs_save(); }
    C.pressed = -1;
}

static void c_tick(float dt)
{
    for (int i = 0; i < NT; i++) {
        float tgt = state(i) ? 1.0f : 0.0f;              /* follow changes made elsewhere */
        if (C.on[i].target != tgt) msp_to(&C.on[i], tgt);
        msp_step(&C.on[i], dt);
    }
    if (!C.sl_drag) C.sl_val = nos_prefs.brightness / 100.0f;
    const svc_sys_t *s = svc_sys();
    C.bat = s->battery_pct > 0 ? s->battery_pct : hal_battery_percent();
    C.charging = s->charging;
    hal_datetime now;
    hal_get_datetime(&now);
    ui_fmt_time(C.time, sizeof C.time, now.hour, now.minute);
}

/* build-in: tiles arrive row by row (2 frames apart), growing the last few px */
static float tile_in(int i)
{
    int row = i < 3 ? 0 : i < 7 ? 1 : 2;
    float e = (nos_t - C.t_enter - row * (2.0f / NOS_FPS) - (i % 4) * (1.0f / NOS_FPS)) / 0.20f;
    e = bezier_ease(&EASE_DECEL, m_clamp(e, 0, 1));
    return floorf(e * 16.0f + 0.5f) / 16.0f;
}

static void draw_tile(gfx_fb *fb, int i)
{
    float x, y;
    tile_pos(i, &x, &y);
    float e = tile_in(i);
    if (e <= 0) return;
    float r = TILE_R * (0.86f + 0.14f * e);
    int pressed = C.pressed == i;
    float pitch = 4.2f;
    if (!present(i)) {
        ui_circle(fb, x, y, r, gdim(C_SURF, e));
        dot_icon(fb, k_tile[i].icon, x, y, pitch, pitch * 0.8f, gdim(C_DIS, e));
        return;
    }
    float k = m_clamp(C.on[i].x, 0, 1);
    k = floorf(k * 16.0f + 0.5f) / 16.0f;
    ui_circle(fb, x, y, r, gdim(pressed ? C_SURF3 : C_SURF2, e));
    if (k > 0) ui_circle(fb, x, y, r * k, gdim(pressed ? C_SEC : C_WHITE, e));
    uint16_t ink = k >= 0.5f ? C_BG : gdim(C_PRI, e);
    dot_icon(fb, k_tile[i].icon, x, y, pitch, pitch * 0.8f, ink);
}

static void draw_header(gfx_fb *fb)
{
    float age = nos_t - C.note_t;
    if (C.note[0] && age < 1.6f) {                       /* the last toggle, then back */
        float k = age < 1.3f ? 1.0f : 1.0f - (age - 1.3f) / 0.3f;
        k = floorf(m_clamp(k, 0, 1) * 8.0f + 0.5f) / 8.0f;
        if (k > 0) { ui_label(fb, UI_CX, HEAD_Y, C.note, gdim(C_PRI, k), UI_CENTER); return; }
    }
    char b[16];
    snprintf(b, sizeof b, "%d%%", C.bat);
    ui_label(fb, UI_CX - 12, HEAD_Y, C.time, C_PRI, UI_RIGHT);
    dot_disc(fb, UI_CX, HEAD_Y - 6, 4, C_DIS);
    ui_label(fb, UI_CX + 12, HEAD_Y, b, C.charging ? C_PRI : C_SEC, UI_LEFT);
}

static void c_draw(gfx_fb *fb)
{
    draw_header(fb);
    for (int i = 0; i < NT; i++) draw_tile(fb, i);
    float e = tile_in(NT - 1);
    if (e > 0) {
        float v = floorf(C.sl_val * 100.0f + 0.5f) / 100.0f;
        ui_slider(fb, SL_X, SL_Y + (1 - e) * 8, SL_W, SL_H, v, DOT_ICON_BRIGHT);
    }
}

static int tile_at(int x, int y)
{
    for (int i = 0; i < NT; i++) {
        float tx, ty;
        tile_pos(i, &tx, &ty);
        float dx = x - tx, dy = y - ty;
        if (dx * dx + dy * dy <= 48.0f * 48.0f) return i;   /* the 96 px cell */
    }
    if (y >= SL_Y - 14 && y <= SL_Y + SL_H + 14 && x >= SL_X - 10 && x <= SL_X + SL_W + 10) return NT;
    return -1;
}

static void tap_tile(int i, int x, int y)
{
    float tx, ty;
    tile_pos(i, &tx, &ty);
    if (!present(i)) {
        nos_feedback(FB_ERROR);
        set_note(i == T_SOUND ? "NO SPEAKER" : i == T_HAPTIC ? "NO MOTOR" : i == T_LTE ? "NO LTE MODULE" : "NO GPS MODULE", 0);
        return;
    }
    if (i == T_TORCH) { nos_open(&scr_torch, (int)tx, (int)ty); return; }
    if (i == T_SETTINGS) { nos_open(&scr_settings, (int)tx, (int)ty); return; }
    toggle(i);
    (void)x; (void)y;
}

static int c_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN:
        C.pressed = tile_at(in->x, in->y);
        return 1;
    case IN_DRAG_START:
        if (C.pressed == NT && in->axis == AXIS_X) {
            C.sl_drag = 1;
            C.sl_v0 = C.sl_val;
            C.sl_step = (int)floorf(C.sl_val * 10.0f + 0.5f);
            return 1;
        }
        C.pressed = -1;
        return 0;                 /* vertical: up closes the sheet (engine); nothing scrolls */
    case IN_DRAG:
        if (!C.sl_drag) return 0;
        {
            float v = m_clamp(C.sl_v0 + in->dx / (SL_W - SL_H), 0.05f, 1.0f);
            int step = (int)floorf(v * 10.0f + 0.5f);
            if (step != C.sl_step) { C.sl_step = step; nos_feedback(v <= 0.05f || v >= 1.0f ? FB_LIMIT : FB_TICK); }
            C.sl_val = v;
            apply_brightness(v);
        }
        return 1;
    case IN_DRAG_END:
        if (!C.sl_drag) return 0;
        C.sl_drag = 0;
        prefs_save();
        return 1;
    case IN_TAP: {
        int i = tile_at(in->x, in->y);
        C.pressed = -1;
        if (i == NT) {            /* tap on the track: jump there */
            float v = m_clamp((in->x - SL_X - SL_H * 0.5f) / (SL_W - SL_H), 0.05f, 1.0f);
            C.sl_val = v;
            apply_brightness(v);
            prefs_save();
            nos_feedback(FB_TICK);
        } else if (i >= 0) tap_tile(i, in->x, in->y);
        return 1;
    }
    case IN_UP:
        C.pressed = -1;
        return 1;
    default:
        return 0;
    }
}

const nos_screen scr_control = { "CONTROL", c_enter, c_leave, c_tick, c_draw, c_input, SCR_HDRAG };
