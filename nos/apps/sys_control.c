/* sys_control.c — control centre (pulled down from the face, TX_DROP).
 *
 *   y  56   10:09 . 78%            (mono; briefly "WI-FI OFF" / "BRIGHTNESS 60%")
 *   y 124   ( WIFI ) ( BT ) ( DND )
 *   y 220   ( TORCH ) ( AOD ) ( SOUND ) ( HAPTICS )
 *   y 316   ( LTE ) ( GPS ) ( RAISE ) ( SETTINGS )
 *   y 392   * [][][][][][][][][][]     brightness in 10 segments
 *
 * Tiles are r = 40 discs on a 96 px grid (16 px gaps). Everything fits on the glass, so
 * nothing scrolls: an upward drag always belongs to the engine, which closes the sheet.
 * Toggles: on = white disc + black ink, off = C_SURF2 + white ink; the fill morphs in
 * quantised grey steps from wherever it is (the pressed C_SEC on a tap) and the ink flips
 * at the midpoint, where both read. Torch and Settings open screens (the panel is the
 * light), so they are drawn as outlined discs with no on/off state.
 * Absent strap modules show a disabled tile and say why on tap. The brightness bar follows
 * the finger relative to where it was grabbed, ticks every 10 %, shows the % in the
 * header and drives the backlight live. Pulled in by the finger, the sheet shows its
 * content immediately (the sheet's own motion is the entrance); otherwise tiles build in
 * row by row. */
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
#include <stdlib.h>
#include <string.h>

enum { T_WIFI = 0, T_BT, T_DND, T_TORCH, T_AOD, T_SOUND, T_HAPTIC, T_LTE, T_GPS, T_RAISE, T_SETTINGS, NT };

#define TILE_R   40.0f
#define PITCH    96.0f
#define HEAD_Y   56
#define SL_Y     372.0f          /* segment bar top */
#define SL_H     40.0f
#define SL_X     138.0f          /* icon + bar centred as one group */
#define SL_W     224.0f
#define SL_ICON_X 114.0f
#define LV_OFF   26              /* C_SURF2 */
#define LV_PRESS 153             /* C_SEC: the house pressed fill, with dark ink */

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
/* neutral grey of 8-bit level v */
static uint16_t grey(int v)
{
    if (v >= 255) return C_WHITE;
    if (v < 0) v = 0;
    int r = (v * 31 + 127) / 255;
    return (uint16_t)(r << 11 | (r * 2) << 5 | r);
}

#define ICON_VIBRATE (-1)        /* drawn locally: a body with buzz marks either side */

static const struct { int icon; const char *name; uint32_t need; } k_tile[NT] = {
    [T_WIFI]     = { DOT_ICON_WIFI,     "Wi-Fi",     0 },
    [T_BT]       = { DOT_ICON_BT,       "Bluetooth", 0 },
    [T_DND]      = { DOT_ICON_DND,      "DND",       0 },
    [T_TORCH]    = { DOT_ICON_TORCH,    "Torch",     0 },
    [T_AOD]      = { DOT_ICON_EYE,      "AOD",       0 },
    [T_SOUND]    = { DOT_ICON_SPEAKER,  "Sound",     EXT_SPEAKER },
    [T_HAPTIC]   = { ICON_VIBRATE,      "Haptics",   EXT_HAPTIC },
    [T_LTE]      = { DOT_ICON_SIGNAL,   "LTE",       EXT_LTE },
    [T_GPS]      = { DOT_ICON_PIN,      "GPS",       EXT_GPS },
    [T_RAISE]    = { DOT_ICON_ARROW_UP, "Raise",     0 },
    [T_SETTINGS] = { DOT_ICON_SETTINGS, "Settings",  0 },
};

/* Section 8 pressed state: hidden for the first 100 ms (a drag's slop passes first and
 * cancels it silently), then on until release; a quick tap still shows it for 3 frames. */
/* the system REC chip (sys_home.c; asked for in nos_apps.h) */
int sys_rec_state(char *label, int n);
const nos_screen *sys_rec_screen(int kind);
int sys_rec_width(const char *label);
void sys_rec_draw(gfx_fb *fb, int x0, int y, const char *label, uint16_t dot, uint16_t ink);
#define ID_REC 100           /* press id of the header REC chip */

static struct { int id, down; float t0, until; } P = { -1, 0, 0, 0 };
static void press_down(int id) { P.id = id; P.down = id >= 0; P.t0 = nos_t; P.until = 0; }
static void press_cancel(void) { P.id = -1; P.down = 0; P.until = 0; }
static void press_up(void)
{
    if (!P.down) return;
    P.down = 0;
    float vis = P.t0 + 0.09f;                    /* when it became visible */
    P.until = nos_t < vis ? nos_t + 0.09f : vis + 0.10f;
}
static int press_on(int id)
{
    if (id < 0 || P.id != id) return 0;
    return P.down ? nos_t - P.t0 >= 0.09f : nos_t < P.until;
}

static struct {
    mspring on[NT];          /* morph progress 0..1 from lv_from to lv_to */
    short lv_from[NT], lv_to[NT];   /* fill grey levels of the running morph */
    int sl_drag;
    float sl_v0, sl_dx0, sl_val;   /* value and finger dx at grab, live value 0..1 */
    int sl_step;             /* last 10 % step (ticks) */
    float t_enter;
    char note[32];           /* "WI-FI OFF" */
    float note_t;
    int note_hold;           /* keep the note up (slider being dragged) */
    int bat, charging;
    char time[8];
    float rec_raw;           /* header: clock + battery (0) .. REC chip (1), linear */
    int rec_kind;            /* 1 track, 2 memo (kept while it fades out) */
    char rec_lbl[16];
} C;

static void tile_pos(int i, float *x, float *y)
{
    if (i < 3) { *x = UI_CX + (i - 1) * PITCH; *y = 124; }
    else if (i < 7) { *x = UI_CX + (i - 3 - 1.5f) * PITCH; *y = 220; }
    else { *x = UI_CX + (i - 7 - 1.5f) * PITCH; *y = 316; }
}

static int present(int i) { return !k_tile[i].need || (hal_ext_present() & k_tile[i].need); }

static int state(int i)
{
    switch (i) {
    case T_WIFI: return nos_prefs.wifi;
    case T_BT: return nos_prefs.ble;
    case T_DND: return nos_prefs.dnd;
    case T_AOD: return nos_prefs.aod;
    case T_SOUND: return nos_prefs.sound;
    case T_HAPTIC: return nos_prefs.haptics;
    case T_LTE: return nos_prefs.lte;
    case T_GPS: return nos_prefs.gps_auto;
    case T_RAISE: return nos_prefs.raise_wake;
    default: return 0;
    }
}

/* the fill level drawn now (8 steps along the running morph) */
static int fill_lv(int i)
{
    float k = floorf(m_clamp(C.on[i].x, 0, 1) * 8.0f + 0.5f) / 8.0f;
    return (int)lroundf(C.lv_from[i] + (C.lv_to[i] - C.lv_from[i]) * k);
}
/* morph tile i to its state's fill, starting from level `from` */
static void morph(int i, int from)
{
    C.lv_from[i] = (short)from;
    C.lv_to[i] = (short)(state(i) ? 255 : LV_OFF);
    msp_init(&C.on[i], 0.0f, SPRING_SNAPPY);
    msp_to(&C.on[i], 1.0f);
}

static void set_note(const char *name, const char *st)
{
    char b[32];
    snprintf(b, sizeof b, "%s%s%s", name, st ? " " : "", st ? st : "");
    float age = nos_t - C.note_t;
    if (!C.note[0] || age > 1.6f) C.note_t = nos_t;          /* fade in only when not showing */
    else if (age > 0.10f) C.note_t = nos_t - 0.10f;          /* already up: restart the hold */
    snprintf(C.note, sizeof C.note, "%s", b);
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
    morph(i, LV_PRESS);                        /* from the pressed fill the finger just saw */
    set_note(k_tile[i].name, on ? "ON" : "OFF");
}

static void apply_brightness(float v)
{
    int pct = (int)lroundf(v * 100.0f);
    if (pct < 5) pct = 5;
    nos_prefs.brightness = (uint8_t)pct;
    hal_set_backlight((uint8_t)(pct * 255 / 100));
}

static void note_brightness(void)
{
    char b[8];
    snprintf(b, sizeof b, "%d%%", (int)lroundf(C.sl_val * 100.0f));
    set_note("BRIGHTNESS", b);
}

static void c_enter(void)
{
    for (int i = 0; i < NT; i++) {
        C.lv_from[i] = C.lv_to[i] = (short)(state(i) ? 255 : LV_OFF);
        msp_init(&C.on[i], 1.0f, SPRING_SNAPPY);
    }
    press_cancel();
    C.sl_drag = 0;
    C.note_hold = 0;
    C.sl_val = nos_prefs.brightness / 100.0f;
    C.rec_kind = sys_rec_state(C.rec_lbl, sizeof C.rec_lbl);
    C.rec_raw = C.rec_kind ? 1.0f : 0.0f;  /* the sheet brings its header as it is */
    if (nos_revealed()) return;            /* back from torch/settings: as it was */
    /* the finger is revealing the sheet: show everything at once */
    C.t_enter = nos_entered_by_finger() ? nos_t - 2.0f : nos_enter_t();
    C.note[0] = 0;
}

static void c_leave(void)
{
    if (C.sl_drag) { C.sl_drag = 0; prefs_save(); }
    if (P.down) press_cancel();                /* a released press stays lit through its iris */
}

static void c_tick(float dt)
{
    for (int i = 0; i < NT; i++) {
        int to = state(i) ? 255 : LV_OFF;                /* follow changes made elsewhere */
        if (C.lv_to[i] != to) morph(i, fill_lv(i));
        msp_step(&C.on[i], dt);
    }
    if (!C.sl_drag) C.sl_val = nos_prefs.brightness / 100.0f;
    if (C.note_hold) C.note_t = nos_t - 0.10f;
    const svc_sys_t *s = svc_sys();
    C.bat = s->battery_pct > 0 ? s->battery_pct : hal_battery_percent();
    C.charging = s->charging;
    hal_datetime now;
    hal_get_datetime(&now);
    ui_fmt_time(C.time, sizeof C.time, now.hour, now.minute);
    /* a recording starting or ending crossfades the header over 6 frames */
    char lbl[16];
    int rk = sys_rec_state(lbl, sizeof lbl);
    if (rk) { C.rec_kind = rk; memcpy(C.rec_lbl, lbl, sizeof lbl); }
    C.rec_raw = m_clamp(C.rec_raw + (rk ? 1.0f : -1.0f) * dt / (6.0f / NOS_FPS), 0, 1);
}

/* build-in: tiles arrive row by row (2 frames apart), growing the last few px */
static float tile_in(int i)
{
    int row = i < 3 ? 0 : i < 7 ? 1 : 2;
    float e = (nos_t - C.t_enter - row * (2.0f / NOS_FPS) - (i % 4) * (1.0f / NOS_FPS)) / 0.20f;
    e = bezier_ease(&EASE_DECEL, m_clamp(e, 0, 1));
    return floorf(e * 16.0f + 0.5f) / 16.0f;
}

/* the haptics glyph: a body with buzz marks either side (11 x 9 cells) */
static void draw_vibrate(gfx_fb *fb, float cx, float cy, float pitch, uint16_t ink)
{
    static const char art[9][12] = {
        "...#####...",
        ".#.#...#.#.",
        "#..#...#..#",
        ".#.#...#.#.",
        "#..#...#..#",
        ".#.#...#.#.",
        "#..#...#..#",
        ".#.#...#.#.",
        "...#####...",
    };
    float d = pitch * 0.8f, x0 = cx - 5 * pitch, y0 = cy - 4 * pitch;
    for (int r = 0; r < 9; r++)
        for (int c = 0; c < 11; c++)
            if (art[r][c] == '#') dot_disc(fb, x0 + c * pitch, y0 + r * pitch, d, ink);
}

static void tile_icon(gfx_fb *fb, int i, float x, float y, uint16_t ink)
{
    const float pitch = 4.0f;
    if (k_tile[i].icon == ICON_VIBRATE) draw_vibrate(fb, x, y, pitch, ink);
    else dot_icon(fb, k_tile[i].icon, x, y, pitch, pitch * 0.8f, ink);
}

static void draw_tile(gfx_fb *fb, int i)
{
    float x, y;
    tile_pos(i, &x, &y);
    float e = tile_in(i);
    if (e <= 0) return;
    float r = TILE_R * (0.86f + 0.14f * e);
    int pressed = press_on(i);
    if (!present(i)) {
        ui_circle(fb, x, y, r, gdim(C_SURF, e));
        tile_icon(fb, i, x, y, gdim(C_DIS, e));
        return;
    }
    if (i == T_SETTINGS || i == T_TORCH) {     /* they open screens, not states: outlined */
        ui_circle(fb, x, y, r, gdim(pressed ? C_SEC : C_SURF2, e));
        ui_circle_line(fb, x, y, r, 2.0f, gdim(pressed ? C_SEC : C_LINE2, e));
        tile_icon(fb, i, x, y, pressed ? C_BG : gdim(C_PRI, e));
        return;
    }
    /* fill morph: neutral grey steps between C_SURF2 (26) and white; the ink flips at the
     * midpoint, where the fill is mid-grey and both inks read */
    int lv = pressed ? LV_PRESS : fill_lv(i);   /* pressed: C_SEC with dark ink (house style) */
    ui_circle(fb, x, y, r, gdim(grey(lv), e));
    tile_icon(fb, i, x, y, lv >= 140 ? C_BG : gdim(C_PRI, e));
}

static void draw_header(gfx_fb *fb)
{
    float age = nos_t - C.note_t;
    if (C.note[0] && age < 1.6f) {                       /* the last change, then back */
        float k = age < 0.10f ? age / 0.10f : age < 1.3f ? 1.0f : 1.0f - (age - 1.3f) / 0.3f;
        k = floorf(m_clamp(k, 0, 1) * 8.0f + 0.5f) / 8.0f;
        if (k > 0) { ui_label(fb, UI_CX, HEAD_Y, C.note, gdim(C_PRI, k), UI_CENTER); return; }
    }
    /* recording: the header is the REC chip (clock and battery out, then the chip in) */
    float kc = floorf(m_clamp(1.0f - 2.0f * C.rec_raw, 0, 1) * 3.0f + 0.5f) / 3.0f;
    float kr = floorf(m_clamp(2.0f * C.rec_raw - 1.0f, 0, 1) * 3.0f + 0.5f) / 3.0f;
    if (kr > 0 && C.rec_kind) {
        int w = sys_rec_width(C.rec_lbl), x0 = UI_CX - w / 2;
        if (press_on(ID_REC)) ui_rrect(fb, x0 - 12, HEAD_Y - 21, w + 24, 30, 15, C_SURF2);
        sys_rec_draw(fb, x0, HEAD_Y, C.rec_lbl, ui_dim(C_RED, kr), gdim(C_PRI, kr));
    }
    if (kc <= 0) return;
    char b[16];
    snprintf(b, sizeof b, "%d%%", C.bat);
    ui_label(fb, UI_CX - 12, HEAD_Y, C.time, gdim(C_PRI, kc), UI_RIGHT);
    dot_disc(fb, UI_CX, HEAD_Y - 6, 4, gdim(C_DIS, kc));
    ui_label(fb, UI_CX + 12, HEAD_Y, b, gdim(C.charging ? C_PRI : C_SEC, kc), UI_LEFT);
}

static void c_draw(gfx_fb *fb)
{
    draw_header(fb);
    for (int i = 0; i < NT; i++) draw_tile(fb, i);
    float e = tile_in(NT - 1);
    if (e > 0) {
        float v = floorf(C.sl_val * 100.0f + 0.5f) / 100.0f;
        float y = SL_Y + floorf((1 - e) * 8 + 0.5f);
        int press = press_on(NT) || C.sl_drag;
        dot_icon(fb, DOT_ICON_BRIGHT, SL_ICON_X, y + SL_H * 0.5f, 2.4f, 1.9f, gdim(press ? C_PRI : C_SEC, e));
        ui_segments(fb, SL_X, y, SL_W, SL_H, 10, v * 10.0f, gdim(C_WHITE, e), gdim(press ? C_LINE2 : C_SURF2, e));
    }
}

static int tile_at(int x, int y)
{
    if (C.rec_kind && C.rec_raw >= 0.5f && y < 84 &&    /* the header chip, to the first row */
        abs(x - UI_CX) <= sys_rec_width(C.rec_lbl) / 2 + 24) return ID_REC;
    for (int i = 0; i < NT; i++) {
        float tx, ty;
        tile_pos(i, &tx, &ty);
        float dx = x - tx, dy = y - ty;
        if (dx * dx + dy * dy <= 48.0f * 48.0f) return i;   /* the 96 px cell */
    }
    if (y >= SL_Y - 16 && y <= SL_Y + SL_H + 16 && x >= SL_ICON_X - 20 && x <= SL_X + SL_W + 12) return NT;
    return -1;
}

static void tap_tile(int i)
{
    float tx, ty;
    tile_pos(i, &tx, &ty);
    if (!present(i)) {
        nos_feedback(FB_ERROR);
        set_note(i == T_SOUND ? "NO SPEAKER" : i == T_HAPTIC ? "NO MOTOR" : i == T_LTE ? "NO LTE MODULE" : "NO GPS MODULE", 0);
        return;
    }
    /* the screen grows out of the tile's own icon disc */
    if (i == T_TORCH || i == T_SETTINGS) {
        press_up();
        nos_open_icon(i == T_TORCH ? &scr_torch : &scr_settings, (int)tx, (int)ty, k_tile[i].icon, TILE_R);
        return;
    }
    press_cancel();                            /* the morph starts from the pressed fill */
    toggle(i);
}

static int c_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN: {
        int i = tile_at(in->x, in->y);
        press_down(i >= 0 && i < NT && !present(i) ? -1 : i);   /* disabled tiles explain instead */
        return 1;
    }
    case IN_DRAG_START:
        if (P.down && P.id == NT && in->axis == AXIS_X) {
            C.sl_drag = 1;
            C.sl_v0 = C.sl_val;
            C.sl_dx0 = in->dx;                 /* absorb the slop: no jump when it locks */
            C.sl_step = (int)floorf(C.sl_val * 10.0f + 0.5f);
            C.note_hold = 1;
            note_brightness();
            return 1;
        }
        press_cancel();
        return 0;                 /* vertical: up closes the sheet (engine); nothing scrolls */
    case IN_DRAG:
        if (!C.sl_drag) return 0;
        {
            float v = m_clamp(C.sl_v0 + (in->dx - C.sl_dx0) / SL_W, 0.05f, 1.0f);
            int step = (int)floorf(v * 10.0f + 0.5f);
            if (step != C.sl_step) { C.sl_step = step; nos_feedback(v <= 0.05f || v >= 1.0f ? FB_LIMIT : FB_TICK); }
            C.sl_val = v;
            apply_brightness(v);
            note_brightness();
        }
        return 1;
    case IN_DRAG_END:
        if (!C.sl_drag) return 0;
        C.sl_drag = 0;
        C.note_hold = 0;
        prefs_save();
        return 1;
    case IN_LONG:
        if (P.down && P.id == T_TORCH) {
            float tx, ty;
            tile_pos(T_TORCH, &tx, &ty);
            press_up();
            nos_open_icon(&scr_torch, (int)tx, (int)ty, k_tile[T_TORCH].icon, TILE_R);
            return 1;
        }
        return 0;
    case IN_TAP: {
        int i = tile_at(in->x, in->y);
        if (i != P.id) press_cancel();
        if (i == ID_REC) {        /* the recording: open its app */
            press_up();
            nos_open(sys_rec_screen(C.rec_kind), in->x, in->y);
            return 1;
        }
        if (i == NT) {            /* tap on the bar: jump there */
            press_up();
            float v = m_clamp((in->x - SL_X) / SL_W, 0.05f, 1.0f);
            v = ceilf(v * 10.0f - 0.001f) / 10.0f;   /* to the tapped segment's end */
            C.sl_val = v;
            apply_brightness(v);
            prefs_save();
            nos_feedback(FB_TICK);
            note_brightness();
        } else if (i >= 0) tap_tile(i);
        return 1;
    }
    case IN_UP:
        press_up();
        if (C.sl_drag) { C.sl_drag = 0; prefs_save(); }
        C.note_hold = 0;
        return 1;
    default:
        return 0;
    }
}

const nos_screen scr_control = { "CONTROL", c_enter, c_leave, c_tick, c_draw, c_input, SCR_HDRAG };
