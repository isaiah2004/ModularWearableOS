/* sys_control.c — control centre (pulled down from the face, TX_DROP).
 *
 *   y  56   10:09 . 78%            (mono; briefly "WI-FI OFF" / "BRIGHTNESS 60%")
 *   y 124   ( WIFI ) ( BT ) ( DND )
 *   y 220   ( TORCH ) ( AOD ) ( SOUND ) ( HAPTICS )
 *   y 316   ( LTE ) ( GPS ) ( RAISE ) ( SETTINGS )
 *   y 392   ( *  ##########--- )      brightness: ten 15 x 18 segments in a pill
 *
 * Tiles are r = 40 discs on a 96 px grid (16 px gaps). Everything fits on the glass, so
 * nothing scrolls: an upward drag always belongs to the engine, which closes the sheet.
 * Toggles: on = white disc + black ink, off = C_SURF2 + white ink; the fill morphs in
 * three snap frames (MF_SNAP3: 60/90/100 %, quantised grey steps) from wherever it is
 * (the pressed C_SEC on a tap) and the ink flips at the midpoint, where both read.
 * Torch and Settings open screens (the panel is the
 * light), so they are drawn as outlined discs with no on/off state.
 * Absent strap modules show a disabled tile and say why on tap. Brightness is a 257 x 50
 * C_SURF2 pill holding the sun (outside the measured track) and ten 15 x 18 segments with
 * 4 px gaps (prog.h PROG_SEGMENTS, 19 px a step); the leading segment grows along its
 * length, so a drag reads continuously. The bar follows the finger 1:1 relative to where
 * it was grabbed, ticks every 10 %, shows the % in the header and drives the backlight
 * live; a tap fills through the tapped segment on an EASE_STEADY tween of 4-9 frames by
 * distance (no frame over ~1.6x the average). Pulled in by the finger, the sheet shows its
 * content immediately (the sheet's own motion is the entrance); otherwise tiles build in
 * row by row, in whole frames (MOTION30 R9), and the brightness fill sweeps in from empty
 * over 13-25 frames (0.43-0.83 s) as the pill arrives. */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "prog.h"
#include "pblur.h"
#include "diag_log.h"
#include "apps/nos_apps.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T_WIFI = 0, T_BT, T_DND, T_TORCH, T_AOD, T_SOUND, T_HAPTIC, T_LTE, T_GPS, T_RAISE, T_SETTINGS, NT };

#define TILE_R   40.0f
#define PITCH    96.0f
#define HEAD_Y   56
/* brightness: a 257 x 50 pill centred on 392, the sun in its left cap, then the bar */
#define SL_W     257.0f
#define SL_H     50.0f
#define SL_X     (UI_CX - SL_W * 0.5f)
#define SL_Y     367.0f          /* pill top */
#define SUN_X    (SL_X + 28.0f)  /* the sun's centre: outside the measured track */
#define BAR_N    10              /* ten 15 x 18 segments, 4 px gaps: 19 px slots */
#define BAR_SLOT 19.0f
#define BAR_W    (BAR_SLOT * BAR_N)
#define BAR_X    (SL_X + 53.0f)  /* the first segment starts 2 px in: 16 px clear to the right cap */
#define BAR_Y    (SL_Y + 16.0f)
#define SL_RUN   BAR_W           /* the fill end's travel: a drag moves it 1:1 */
#define HIT_X0   (UI_CX - 129.5f) /* the 88 px hit row (unchanged): the pill plus 1 px */
#define HIT_X1   (UI_CX + 129.5f)
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

static struct { int id, down, shown; nos_time_t t0, until; } P = { -1, 0, 0, 0, 0 };
static void press_down(int id) { P.id = id; P.down = id >= 0; P.shown = 0; P.t0 = nos_t; P.until = 0; }
static void press_cancel(void) { P.id = -1; P.down = 0; P.until = 0; }
static void press_up(void)
{
    if (!P.down) return;
    P.down = 0;
    nos_time_t vis = P.t0 + 0.09f;                    /* when it became visible */
    P.until = nos_t < vis ? nos_t + 0.09f : vis + 0.10f;
}
static int press_on(int id)
{
    if (id < 0 || P.id != id) return 0;
    if (P.down) return P.shown && nos_touch_travel() <= 8;
    return nos_t < P.until;
}
/* a held press lights once 90 ms have passed with the finger still (<= 3 px), then
 * stays lit until release unless the finger passes 8 px (a drag starting) */
static void press_tick(void)
{
    if (P.down && !P.shown && nos_t - P.t0 >= 0.09f && nos_touch_travel() <= 3) P.shown = 1;
}

static struct {
    nos_time_t on_t0[NT];         /* fill morph start (MF_SNAP3 from lv_from to lv_to) */
    short lv_from[NT], lv_to[NT];   /* fill grey levels of the running morph */
    int page, page_drag, page_anim;
    float page_x, page_start;
    mtween page_tw;
    int sl_drag;
    float sl_v0, sl_dx0, sl_val;   /* value and finger dx at grab, live value 0..1 */
    int sl_step;             /* last 10 % step (ticks) */
    mtween sl_tw;            /* a tap: the fill moves to the nearest 10 % step */
    int sl_anim, sl_n;       /* ...over sl_n frames */
    uint32_t sl_request;
    nos_time_t fill_t0;           /* entry: the fill sweeps from empty, from this time... */
    int fill_n, fill_live;   /* ...over fill_n frames; live: shows sl_val as is */
    nos_time_t t_enter;
    char note[32];           /* "WI-FI OFF" */
    char note_name[24];      /* its subject ("WI-FI"): a new subject crossfades, a new value doesn't */
    char prev_note[32];      /* the note it replaced, going out */
    float prev_k; nos_time_t swap_t;
    nos_time_t note_t;
    int note_hold;           /* keep the note up (slider being dragged) */
    int bat, charging;
    char time[8];
    float rec_raw;           /* header: clock + battery (0) .. REC chip (1), linear */
    int rec_kind;            /* 1 track, 2 memo (kept while it fades out) */
    char rec_lbl[16];
    nos_time_t rec_open_t;        /* a tap on the chip: when its app opens (after the lit frames) */
    int rec_ox, rec_oy;
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

/* the fill level drawn now: the snap table, 8 grey steps (62.5 / 87.5 / 100 %) */
static int fill_lv(int i)
{
    float k = floorf(mf_at(&MF_SNAP3, m_frame_k(C.on_t0[i])) * 8.0f + 0.5f) / 8.0f;
    return (int)lroundf(C.lv_from[i] + (C.lv_to[i] - C.lv_from[i]) * k);
}
/* morph tile i to its state's fill, starting from level `from`; one frame in, so the tap
 * that toggled it already shows the first step */
static void morph(int i, int from)
{
    C.lv_from[i] = (short)from;
    C.lv_to[i] = (short)(state(i) ? 255 : LV_OFF);
    C.on_t0[i] = nos_t - 1.0f / NOS_FPS;
}

static void set_note(const char *name, const char *st)
{
    char b[32];
    snprintf(b, sizeof b, "%s%s%s", name, st ? " " : "", st ? st : "");
    float age = nos_t - C.note_t;
    if (!C.note[0] || age > 1.6f) C.note_t = nos_t;          /* fade in only when not showing */
    else {
        if (strcmp(C.note_name, name)) {                     /* another subject: crossfade */
            float k = age < 0.10f ? age / 0.10f : age < 1.3f ? 1.0f : 1.0f - (age - 1.3f) / 0.3f;
            snprintf(C.prev_note, sizeof C.prev_note, "%s", C.note);
            C.prev_k = m_clamp(k, 0, 1);
            C.swap_t = nos_t;
        }
        if (age > 0.10f) C.note_t = nos_t - 0.10f;           /* already up: restart the hold */
    }
    snprintf(C.note_name, sizeof C.note_name, "%s", name);
    snprintf(C.note, sizeof C.note, "%s", b);
}

static void toggle(int i)
{
    int on = !state(i);
    uint32_t request=diag_action_request(DIAG_DOMAIN_CONTROL,DIAG_ACT_SET,i);
    diag_action_context(request,(int32_t)diag_log_name_id("CONTROL"),!on);
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
    default: diag_action_result(request,DIAG_RESULT_REJECTED,i);return;
    }
    prefs_save();
    /* This confirms the local preference. Radio/panel acknowledgments belong to
     * their backend operation and must not be inferred from this toggle. */
    diag_action_result(request,state(i)==on?DIAG_RESULT_SUCCESS:DIAG_RESULT_FAILED,state(i));
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
static void brightness_finish(int result)
{
    if(C.sl_request)diag_action_result(C.sl_request,result,nos_prefs.brightness);
    C.sl_request=0;
}
static void brightness_begin(void)
{
    brightness_finish(DIAG_RESULT_CANCELLED);
    C.sl_request=diag_action_request(DIAG_DOMAIN_CONTROL,DIAG_ACT_SET,NT);
    diag_action_context(C.sl_request,(int32_t)diag_log_name_id("CONTROL"),nos_prefs.brightness);
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
        C.on_t0[i] = nos_t - 1.0f;
    }
    press_cancel();
    C.sl_drag = 0; C.sl_anim = 0;
    C.note_hold = 0;
    C.sl_val = nos_prefs.brightness / 100.0f;
    C.rec_kind = sys_rec_state(C.rec_lbl, sizeof C.rec_lbl);
    C.rec_raw = C.rec_kind ? 1.0f : 0.0f;  /* the sheet brings its header as it is */
    C.rec_open_t = 0;
    C.fill_live = 1;
    if (nos_revealed()) return;            /* back from torch/settings: as it was */
    C.page = C.page_drag = C.page_anim = 0; C.page_x = 0; pblur_invalidate(6);
    /* the finger is revealing the sheet: show everything at once */
    C.fill_live = nos_entered_by_finger();
    C.t_enter = C.fill_live ? nos_t - 2.0f : nos_enter_t();
    /* the fill sweeps in from the pill's second frame: 15 frames plus one per 8 % */
    C.fill_t0 = C.t_enter + M_FRAMES(8);
    C.fill_n = 15 + (int)lroundf(12.0f * C.sl_val);
    C.note[0] = 0; C.prev_note[0] = 0; C.note_name[0] = 0;
}

/* the brightness fill drawn now (0..1): the entry sweep, then the live value */
/* (EASE_STEADY over n frames lands on frame n - 2: its last two frames are sub-pixel slivers) */
static int fill_sweeping(void) { return !C.fill_live && m_frame_k(C.fill_t0) < C.fill_n - 2; }
static float fill_shown(void)
{
    if (!fill_sweeping()) return C.sl_val;
    int k = m_frame_k(C.fill_t0);
    return k <= 0 ? 0.0f : C.sl_val * bezier_ease(&EASE_STEADY, (float)k / (float)C.fill_n);
}
/* a finger on the bar takes the fill where it is drawn */
static void fill_take(void)
{
    if (fill_sweeping()) C.sl_val = fill_shown();
    C.fill_live = 1;
}

static void c_leave(void)
{
    brightness_finish(DIAG_RESULT_CANCELLED);
    if (C.sl_drag || C.sl_anim) { C.sl_drag = C.sl_anim = 0; prefs_save(); }
    if (P.down) press_cancel();                /* a released press stays lit through its iris */
}

static void c_probe(void);

static void c_tick(float dt)
{
    press_tick();
    if (C.page_anim) {
        C.page_x = mtw_value(&C.page_tw);
        if (mtw_done(&C.page_tw)) { C.page_anim = 0; C.page_x = (float)(C.page * GFX_W); }
    }
    if (C.prev_note[0] && nos_t - C.swap_t > 2.5f / NOS_FPS) C.prev_note[0] = 0;   /* swap done */
    for (int i = 0; i < NT; i++) {
        int to = state(i) ? 255 : LV_OFF;                /* follow changes made elsewhere */
        if (C.lv_to[i] != to) morph(i, fill_lv(i));
    }
    if (C.sl_anim) {
        C.sl_val = mtw_value(&C.sl_tw);
        if (mtw_done(&C.sl_tw)) { C.sl_anim = 0; C.sl_val = C.sl_tw.to; prefs_save(); }
        apply_brightness(C.sl_val);
        if(!C.sl_anim)brightness_finish(DIAG_RESULT_SUCCESS);
        note_brightness();
    } else if (!C.sl_drag) C.sl_val = nos_prefs.brightness / 100.0f;
    if (C.note_hold && nos_t - C.note_t > 0.10f) C.note_t = nos_t - 0.10f;   /* held up, after its fade in */
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
    if (C.rec_open_t > 0 && nos_t >= C.rec_open_t) {
        C.rec_open_t = 0;
        nos_open(sys_rec_screen(C.rec_kind), C.rec_kind == 4 ? UI_CX : C.rec_ox,
                 C.rec_kind == 4 ? UI_CY : C.rec_oy);
    }
    c_probe();
    sys_control_prewarm();
}

/* build-in: tiles arrive row by row (2 frames apart) and left to right in a row (1 frame
 * apart), each over 4 whole frames on EASE_STEADY (38/71/93/100 %), the last tile done
 * 11 frames in. The disc grows its last 14 % (AA, at float radius) while its ink comes up
 * in perceptual steps (m_ink). */
static float tile_in(int i)
{
    int row = i < 3 ? 0 : i < 7 ? 1 : 2, col = i < 3 ? i : i < 7 ? i - 3 : i - 7;
    int k = m_frame_k(C.t_enter) - (row * 2 + col);
    return k <= 0 ? 0.0f : k >= 4 ? 1.0f : bezier_ease(&EASE_STEADY, k / 4.0f);
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
    float p = tile_in(i);
    if (p <= 0) return;
    float r = TILE_R * (0.86f + 0.14f * p), e = m_ink(p);
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
    float age = nos_t - C.note_t, kn = 0;
    if (C.note[0] && age < 1.6f) {                       /* the last change, then back */
        float k = age < 0.10f ? age / 0.10f : age < 1.3f ? 1.0f : 1.0f - (age - 1.3f) / 0.3f;
        kn = m_ink(k);                                   /* perceptual ink levels */
        /* one note replacing another: the old out in 2 steps, then the new in 2 */
        int u = C.prev_note[0] ? (int)floorf((nos_t - C.swap_t) * NOS_FPS + 0.5f) : 2;
        if (u == 0) ui_label(fb, UI_CX, HEAD_Y, C.prev_note, gdim(C_PRI, 0.5f * C.prev_k), UI_CENTER);
        else if (kn > 0) ui_label(fb, UI_CX, HEAD_Y, C.note, gdim(C_PRI, u == 1 ? 0.5f * kn : kn), UI_CENTER);
        if (u < 2 && kn < C.prev_k) kn = C.prev_k;          /* the clock stays out through the swap */
    }
    /* the clock (or the REC chip) gives way to the note and comes back as it goes, in 3 steps */
    float ko = floorf((1.0f - kn) * 3.0f + 0.5f) / 3.0f;
    if (ko <= 0) return;
    /* recording: the header is the REC chip (clock and battery out, then the chip in) */
    float kc = floorf(m_clamp(1.0f - 2.0f * C.rec_raw, 0, 1) * 3.0f + 0.5f) / 3.0f * ko;
    float kr = floorf(m_clamp(2.0f * C.rec_raw - 1.0f, 0, 1) * 3.0f + 0.5f) / 3.0f * ko;
    if (kr > 0 && C.rec_kind) {
        int w = sys_rec_width(C.rec_lbl), x0 = UI_CX - w / 2;
        if (press_on(ID_REC)) ui_rrect(fb, x0 - 12, HEAD_Y - 21, w + 24, 30, 15, C_SURF2);
        sys_rec_draw(fb, x0, HEAD_Y, C.rec_lbl, ui_dim(C_RED, kr), gdim(C_PRI, kr));
    }
    if (kc <= 0) return;
    char b[16];
    snprintf(b, sizeof b, "%d%%", C.bat);
    /* the group "10:09 . 78%" centred as a whole */
    int wt = ui_label_width(C.time), wb = ui_label_width(b), x0 = UI_CX - (wt + 24 + wb) / 2;
    ui_label(fb, x0, HEAD_Y, C.time, gdim(C_PRI, kc), UI_LEFT);
    dot_disc(fb, (float)(x0 + wt + 12), HEAD_Y - 6, 4, gdim(C_DIS, kc));
    ui_label(fb, x0 + wt + 24, HEAD_Y, b, gdim(C.charging ? C_PRI : C_SEC, kc), UI_LEFT);
}

/* the pill (one step up while pressed), the sun, then the ten segments: lit C_PRI, the
 * rest C_LINE2 (a step lighter while pressed); each segment one primitive, so a drag
 * repaints only the leading one */
static void draw_slider(gfx_fb *fb, float y, float v, int pressed)
{
    float r = SL_H * 0.5f;
    gfx_fill_pill(fb, SL_X + r, y + r, SL_X + SL_W - r, r, pressed ? C_SURF3 : C_SURF2);
    dot_icon(fb, DOT_ICON_BRIGHT, SUN_X, y + r, 3.4f, 2.7f, pressed ? C_PRI : C_SEC);
    prog_bar(fb, PROG_SEGMENTS, BAR_X, y + (BAR_Y - SL_Y), BAR_W, 18.0f, BAR_N, v,
             C_PRI, pressed ? grey(72) : C_LINE2);
}

static void controls_draw(gfx_fb *fb)
{
    draw_header(fb);
    for (int i = 0; i < NT; i++) draw_tile(fb, i);
    float p = tile_in(NT - 1);
    if (p > 0) {
        float v = fill_shown();                /* the leading segment grows in 8 steps */
        float y = SL_Y + floorf((1 - p) * 8 + 0.5f), e = m_ink(p);
        int press = press_on(NT) || C.sl_drag;
        gfx_fb sub;                            /* the build fades it in by ink dim */
        if (e < 1.0f && nos_subview(fb, &sub, 0, 0, GFX_W, GFX_H, 0, 0)) {
            /* the recorder hashes colours, not the ink dim: record the level so an ink
             * step on a frame the rise did not move still repaints */
            if (fb->rec) gfx_rec_cmd(fb, (int)SL_X, (int)y, (int)(SL_X + SL_W) + 1, (int)(y + SL_H) + 1,
                                     gfx_hash_f(0x511du, e));
            nos_ink_dim(&sub, e);
            draw_slider(&sub, y, v, press);
        } else draw_slider(fb, y, v, press);
    }
}

static const char *const tool_names[] = {"FILE QUICK REPORT", "ENERGY", "BATTERY HISTORY", "LOGS / WRIST TUNING"};
static const nos_screen *const tool_screens[] = {&scr_quick_report, &scr_energy, &scr_battery_history, &scr_wrist_tuning};

static void tools_draw(gfx_fb *fb)
{
    ui_title(fb, "QUICK TOOLS", 0);
    for (int i = 0; i < 4; i++) {
        ui_rrect(fb, 83, 105 + i * 70, 300, 56, 28, C_SURF2);
        ui_label(fb, UI_CX, 140 + i * 70, tool_names[i], C_PRI, UI_CENTER);
    }
}

static void c_draw(gfx_fb *fb)
{
    gfx_fb sub;
    int x = (int)lroundf(C.page_x);
    if (x < GFX_W && nos_subview(fb, &sub, 0, 0, GFX_W - x, GFX_H, -x, 0)) controls_draw(&sub);
    if (x > 0 && nos_subview(fb, &sub, GFX_W - x, 0, GFX_W, GFX_H, GFX_W - x, 0)) tools_draw(&sub);
    ui_circle(fb, UI_CX - 9, 443, 3, C.page == 0 ? C_PRI : C_SEC);
    ui_circle(fb, UI_CX + 9, 443, 3, C.page == 1 ? C_PRI : C_SEC);
}

/* Draw the resting drawer from current services, even before its first enter.
 * Only temporary render state changes; no radio, preference, tick or input calls. */
static void control_blur_source(gfx_fb *fb)
{
    __typeof__(C) saved = C;
    __typeof__(P) press = P;
    P.id = -1;
    for (int i = 0; i < NT; i++) {
        C.lv_from[i] = C.lv_to[i] = (short)(state(i) ? 255 : LV_OFF);
        C.on_t0[i] = nos_t - 10;
    }
    C.t_enter = nos_t - 10; C.fill_live = 1;
    if (nos_depth() <= 1) C.page = 0;
    C.page_x = (float)(C.page * GFX_W);
    C.sl_drag = C.sl_anim = 0; C.sl_val = nos_prefs.brightness / 100.0f;
    C.note[0] = C.prev_note[0] = 0;
    const svc_sys_t *s = svc_sys();
    C.bat = s->battery_pct > 0 ? s->battery_pct : hal_battery_percent();
    C.charging = s->charging;
    hal_datetime now; hal_get_datetime(&now);
    ui_fmt_time(C.time, sizeof C.time, now.hour, now.minute);
    C.rec_kind = sys_rec_state(C.rec_lbl, sizeof C.rec_lbl);
    C.rec_raw = C.rec_kind ? 1 : 0;
    c_draw(fb);
    C = saved; P = press;
}
int sys_control_prewarm(void)
{
    static unsigned last;
    unsigned sig = gfx_hash_u((unsigned)nos_prefs.brightness, (unsigned)(nos_depth() <= 1 ? 0 : C.page));
    for (int i = 0; i < NT; i++) sig = gfx_hash_u(sig, (unsigned)(state(i) + 2 * present(i)));
    if (sig != last) { last = sig; pblur_invalidate(6); }
    pblur_check(6, control_blur_source);
    if (nos_transitioning() || (nos_top() == &scr_control && (P.down || C.sl_drag || C.sl_anim || C.page_drag || C.page_anim))) return 0;
    return pblur_tick(6, 1, control_blur_source);
}

/* motion audit (sim only): every animated value as drawn */
static void c_probe(void)
{
    static const char *const k_fill[NT] = {
        "control.fill.wifi", "control.fill.bt", "control.fill.dnd", 0, "control.fill.aod",
        "control.fill.sound", "control.fill.haptic", "control.fill.lte", "control.fill.gps",
        "control.fill.raise", 0 };
    for (int i = 0; i < NT; i++)
        if (k_fill[i] && present(i)) nos_motion_probe(k_fill[i], MC_FADE, fill_lv(i) * 100.0f / 255.0f);
    static const char *const k_r[3] = { "control.build.r0", "control.build.r3", "control.build.r7" };
    static const int k_ri[3] = { 0, 3, 7 };
    for (int j = 0; j < 3; j++) {
        float e = tile_in(k_ri[j]);
        nos_motion_probe(k_r[j], MC_MICRO, TILE_R * (0.86f + 0.14f * e));
    }
    nos_motion_probe("control.build.ink0", MC_FADE, m_ink(tile_in(0)) * 100.0f);
    float e = tile_in(NT - 1);
    nos_motion_probe("control.slider.y", MC_MICRO, floorf((1 - e) * 8 + 0.5f));
    nos_motion_probe("control.slider.ink", MC_FADE, m_ink(e) * 100.0f);
    /* the fill end as drawn: the entry sweep (a fill, as Activity's ring), then the finger
     * or a tap's tween */
    nos_motion_probe("control.bright", fill_sweeping() ? MC_FILL : C.sl_drag ? MC_FINGER :
                     C.sl_n <= 6 ? MC_MICRO : MC_COMPONENT, fill_shown() * SL_RUN);
    float age = nos_t - C.note_t, kn = 0;
    if (C.note[0] && age < 1.6f) {
        float k = age < 0.10f ? age / 0.10f : age < 1.3f ? 1.0f : 1.0f - (age - 1.3f) / 0.3f;
        kn = m_ink(k);
    }
    nos_motion_probe("control.note.ink", MC_FADE, kn * 100.0f);
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
    if (y >= SL_Y - 19 && y <= SL_Y + SL_H + 19 && x >= HIT_X0 && x <= HIT_X1) return NT;   /* 88 px */
    return -1;
}

static void tap_tile(int i)
{
    float tx, ty;
    tile_pos(i, &tx, &ty);
    if (!present(i)) {
        uint32_t request=diag_action_request(DIAG_DOMAIN_CONTROL,DIAG_ACT_SET,i);
        diag_action_context(request,(int32_t)diag_log_name_id("CONTROL"),state(i));
        diag_action_result(request,DIAG_RESULT_REJECTED,(int32_t)k_tile[i].need);
        nos_feedback(FB_ERROR);
        char n[32];
        snprintf(n, sizeof n, "NO %s", ui_module_short(k_tile[i].need));   /* as Settings and the toasts */
        set_note(n, 0);
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
    if(in->type==IN_CANCEL) {
        brightness_finish(DIAG_RESULT_CANCELLED);
        press_cancel();
        if(C.sl_drag)prefs_save();
        C.sl_drag=C.note_hold=0;
        if(C.page_drag) {
            C.page_drag=0;
            mtw_start(&C.page_tw,C.page_x,(float)(C.page*GFX_W),M_FRAMES(8),&EASE_STEADY);C.page_anim=1;
        }
        return 1;
    }
    if (in->type == IN_DRAG_START && in->axis == AXIS_X && !(C.page == 0 && P.down && P.id == NT)) {
        press_cancel();
        pblur_invalidate(6);
        C.page_drag = 1; C.page_anim = 0; C.page_start = C.page_x + in->dx;
        return 1;
    }
    if (C.page_drag && in->type == IN_DRAG) {
        C.page_x = m_clamp(C.page_start - in->dx, 0, GFX_W);
        return 1;
    }
    if (C.page_drag && in->type == IN_DRAG_END) {
        C.page_drag = 0;
        if (in->dx < -50) C.page = 1;
        else if (in->dx > 50) C.page = 0;
        mtw_start(&C.page_tw, C.page_x, (float)(C.page * GFX_W), M_FRAMES(8), &EASE_STEADY);
        C.page_anim = 1;
        return 1;
    }
    if (C.page_anim) return 1;
    if (C.page == 1) {
        if (in->type == IN_TAP && in->x >= 83 && in->x < 383 && in->y >= 105 && in->y < 371) {
            int row = (in->y - 105) / 70;
            if ((in->y - 105) % 70 < 56) nos_push(tool_screens[row], TX_PUSH);
        }
        return in->type != IN_DRAG_START;
    }
    switch (in->type) {
    case IN_DOWN: {
        int i = tile_at(in->x, in->y);
        press_down(i >= 0 && i < NT && !present(i) ? -1 : i);   /* disabled tiles explain instead */
        return 1;
    }
    case IN_DRAG_START:
        if (P.down && P.id == NT && in->axis == AXIS_X) {
            brightness_begin();
            C.sl_drag = 1;
            fill_take();
            if (C.sl_anim) { C.sl_anim = 0; prefs_save(); }   /* the finger takes it where it is */
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
            float v = m_clamp(C.sl_v0 + (in->dx - C.sl_dx0) / SL_RUN, 0.05f, 1.0f);
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
        brightness_finish(DIAG_RESULT_SUCCESS);
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
        if (i == ID_REC) {        /* the recording: lit 3 frames, then its app opens */
            P.id = ID_REC; P.down = 0; P.until = nos_t + 0.5f;
            C.rec_open_t = nos_t + 3.0f / NOS_FPS - 0.001f;
            C.rec_ox = in->x; C.rec_oy = in->y;
            return 1;
        }
        if (i < 0 && !C.rec_kind && in->y < 84 && abs(in->x - UI_CX) < 110) {
            nos_open(&scr_battery, in->x, in->y);   /* the header "10:09 . 78%": the battery */
            return 1;
        }
        if (i == NT) {            /* tap on the bar: jump there */
            brightness_begin();
            press_up();
            /* the fill moves through the tapped segment (the sun's side: the first):
             * EASE_STEADY over 4-9 whole frames by distance, so no frame steps more than
             * ~20 px (a short hop reads as a micro move, a long one as a component), from
             * this frame */
            fill_take();
            float v = m_clamp(floorf((in->x - BAR_X) / BAR_SLOT) + 1.0f, 1.0f, (float)BAR_N) / BAR_N;
            int n = (int)ceilf(fabsf(v - C.sl_val) * SL_RUN / 12.0f);
            C.sl_n = n < 4 ? 4 : n > 9 ? 9 : n;
            mtw_start(&C.sl_tw, C.sl_val, v, M_FRAMES(C.sl_n), &EASE_STEADY);
            mtw_lead(&C.sl_tw);
            C.sl_anim = 1;
            C.sl_step = (int)floorf(v * 10.0f + 0.5f);
            nos_feedback(FB_TICK);
            note_brightness();
        } else if (i >= 0) tap_tile(i);
        return 1;
    }
    case IN_UP:
        press_up();
        if (C.sl_drag) { C.sl_drag = 0; prefs_save(); brightness_finish(DIAG_RESULT_SUCCESS); }
        C.note_hold = 0;
        return 1;
    default:
        return 0;
    }
}

const nos_screen scr_control = { "CONTROL", c_enter, c_leave, c_tick, c_draw, c_input, SCR_HDRAG };
