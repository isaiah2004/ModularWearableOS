/* set_settings.c — Settings: a sectioned list plus its sub-pages.
 *
 * Main list: ui_list owns scrolling, rubber band, pressed row and the scrollbar fade;
 * this file lays out variable-height entries on the list's 30 px unit grid (headers
 * 2 units, rows 3 units = UI_ROW_H) and draws them itself. Rows keep the standard
 * column (UI_ROW_X0..X1, values at UI_VALUE_X) and only inset where the glass would
 * clip them, so nothing drifts sideways while scrolling. A value that can't keep
 * ROW_GAP to its label in the standard column sits under the label instead, and a
 * value that changes cross-fades.
 *
 * Sub-pages (all pushed with TX_PUSH):
 *   picker  — a drum: values sit PK_STEP px apart and turn 1:1 under the finger; the
 *             value on the centre line is the hero and the others shrink away from it;
 *             detents, momentum and snap; rulers at both sides turn with it.
 *   slider  — brightness / volume / haptic strength: hero percent + the OS pill slider
 *             (ui_slider 235 x 50, the control centre's control).
 *   radio   — one page per radio, shared with Modules (which adds its bus line):
 *             Wi-Fi's big round tile is the switch; Bluetooth is the phone link and
 *             stays on, so its page is status only.
 *   settime — two drums (hour, minute) and SET.
 * Storage and Cellular open the Modules pages for the SD card and the modem (one page
 * per part across the OS). Restart and Reset settings arm on the first tap and act on
 * the second (NOS_DESIGN.md §8). Every value is backed by nos_prefs + prefs_save().
 * Draw is pure: tick prepares every string and animation value. */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include "faces/face.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define SET_VERSION NOS_VERSION
void nos_host_power(int mode);   /* firmware/sim: 0 sleep, 1 restart, 2 power off */

/* set_modules.c: one module count and one page per part, shared with Settings */
int  set_modules_online(int *total);
void set_modules_open_sd(void);      /* the SD card page, titled STORAGE */
void set_modules_open_lte(void);     /* the modem page, without the wiring */
/* exported to Modules: the radio pages (Modules adds its bus line) */
void set_settings_open_radio(int bt, int from_modules);

#define PRESS_FLASH 0.10f        /* a quick tap still shows its pressed state 3 frames */
#define PRESS_HOLD  0.20f        /* a held finger shows it only once it is clearly not a scroll */
#define ARM_S       3.0f         /* arm-then-tap-again window */
#define ROW_GAP     24           /* least gap between a row's label and its value */
#define VALUE_XF    0.24f        /* a changed value: old out, new in */

/* ======================================================================= helpers */
static float qfade(float yc)
{
    float d = fabsf(yc - UI_CY);
    float f = 1.0f - 0.7f * m_smoothstep(140.0f, 230.0f, d);
    return floorf(f * 16.0f + 0.5f) / 16.0f;
}
static int cap_base(const font_t *f, float cy) { return (int)lroundf(cy + f->ascent * 0.36f); }
static float q8(float v) { return floorf(m_clamp(v, 0, 1) * 8.0f + 0.5f) / 8.0f; }

/* Standard row column; rows only inset where the glass would clip them (margin 6 keeps
 * every row inside |cy - 233| < 150 exactly on UI_ROW_X0..X1, and the inset beyond it
 * grows continuously, so nothing jumps). */
static void row_cols(float cy, int *lx, int *rx)
{
    int x0, x1;
    ui_row_span(cy, 6, &x0, &x1);
    *lx = x0 > UI_ROW_X0 ? x0 : UI_ROW_X0;
    *rx = x1 < UI_ROW_X1 ? x1 : UI_ROW_X1;
}

/* ui_toggle with a brightness factor (rows dim near the bezel). With its module absent
 * it greys like the sliders do: the value still saves, but nothing is live. */
static void toggle_k(gfx_fb *fb, float cx, float cy, float on, float k, int absent)
{
    const float w = 76, h = 44, r = 16;
    on = m_clamp(on, 0, 1);
    uint16_t track = absent ? ui_mix(C_SURF2, C_DIS, on) : ui_mix(C_LINE2, C_WHITE, on);
    uint16_t thumb = absent ? ui_mix(C_LINE2, C_SURF2, on) : ui_mix(C_DIS, C_BG, on);
    gfx_fill_pill(fb, cx - w / 2 + h / 2, cy, cx + w / 2 - h / 2, h / 2, ui_dim(track, k));
    gfx_fill_circle(fb, m_lerp(cx - w / 2 + h / 2, cx + w / 2 - h / 2, on), cy, r, ui_dim(thumb, k));
}

/* A dot-matrix string that morphs from its previous value whenever it changes. */
typedef struct { char from[24], to[24]; float t0, dur; } mtext;
static void mt_init(mtext *m, const char *s, float dur)
{
    snprintf(m->to, sizeof m->to, "%s", s);
    memcpy(m->from, m->to, sizeof m->from);
    m->t0 = -100.0f; m->dur = dur;
}
static int mt_set(mtext *m, const char *s)
{
    if (!strcmp(s, m->to)) return 0;
    memcpy(m->from, m->to, sizeof m->from);
    snprintf(m->to, sizeof m->to, "%s", s);
    m->t0 = nos_t;
    return 1;
}
static void mt_draw(gfx_fb *fb, const mtext *m, const dot_style *st, float x, float y, int align)
{
    float p = (nos_t - m->t0) / m->dur;
    if (p >= 1.0f) dot_text_a(fb, st, x, y, m->to, align);
    else dot_text_morph(fb, st, x, y, m->from, m->to, align, p);
}

/* Push affordance: a little dot ">" ending at rx. */
static void chevron(gfx_fb *fb, float rx, float cy, uint16_t col)
{
    dot_style st = { DOT_FONT_5X7, 2.0f, 1.7f, col, 0, 1, 0 };
    dot_text(fb, &st, rx - 8.0f, cy - 7.0f, ">");
}
/* Two-way affordance for rows that flip between two values: a dot "^" over a "v". */
static void flipglyph(gfx_fb *fb, float rx, float cy, uint16_t col)
{
    float x = rx - 4.0f;
    const float d = 2.2f;
    dot_disc(fb, x, cy - 8, d, col); dot_disc(fb, x - 3, cy - 5, d, col); dot_disc(fb, x + 3, cy - 5, d, col);
    dot_disc(fb, x, cy + 8, d, col); dot_disc(fb, x - 3, cy + 5, d, col); dot_disc(fb, x + 3, cy + 5, d, col);
}

/* A caps value with an optional case-kept unit ("15 s", "29 GB"). */
static int value_w(const char *v, const char *u)
{
    int w = ui_label_width(v);
    if (u && *u) w += 8 + ui_text_width(UI_F_LABEL, u, 2);
    return w;
}
static void value_draw(gfx_fb *fb, int x, int y, const char *v, const char *u, uint16_t col, int align)
{
    if (u && *u) ui_label_unit(fb, x, y, v, u, col, align);
    else ui_label(fb, x, y, v, col, align);
}
static void ident(gfx_fb *fb, int x, int y, const char *s, uint16_t col, int align)
{
    ui_text(fb, UI_F_SMALL, x, y, s, col, align, 1);
}

/* Arm-then-tap-again pill (UI_BTN_H tall). `a` 0..1 is the arm spring: the plain label
 * goes out, then "TAP AGAIN TO ..." and three red dots that expire one per second come
 * in. `red` 0..1 fades the plain outline to grey while another row is armed. */
static void arm_pill(gfx_fb *fb, float cy, float w, const char *label, const char *armed_label,
                     float armed_t, float a, int pressed, float red, float k)
{
    uint16_t line = ui_mix(C_LINE2, C_RED, m_clamp(red + a, 0, 1));
    uint16_t ink = ui_mix(C_DIS, C_RED, red);
    float x = floorf(UI_CX - w * 0.5f + 0.5f), y = cy - UI_BTN_H * 0.5f;
    w = (UI_CX - x) * 2.0f;
    if (pressed && a <= 0) ui_rrect(fb, x, y, w, UI_BTN_H, UI_BTN_H * 0.5f, ui_dim(C_SURF2, k));
    ui_rrect_line(fb, x, y, w, UI_BTN_H, UI_BTN_H * 0.5f, 2, ui_dim(line, k));
    int asc = UI_F_LABEL->ascent;
    float aout = q8(1.0f - a * 2.0f), ain = q8(a * 2.0f - 1.0f);
    if (aout > 0 && label[0])
        ui_label(fb, UI_CX, (int)(cy + asc * 0.5f - 2), label, ui_dim(ink, k * aout), UI_CENTER);
    if (ain <= 0) return;
    ui_label(fb, UI_CX, (int)(cy + asc * 0.5f - 10), armed_label, ui_dim(C_RED, k * ain), UI_CENTER);
    int left = 3 - (int)floorf(nos_t - armed_t);
    for (int i = 0; i < 3; i++)
        if (i < left) dot_disc(fb, UI_CX + (i - 1) * 14.0f, cy + 18, 6, ui_dim(C_RED, k * ain));
}

/* ======================================================================= drum
 * Values sit PK_STEP px apart on a drum that turns 1:1 under the finger. Each value is
 * sized and lit by its distance from the centre line: the hero pitch on it, the
 * secondary pitch one slot away, fading out beyond. Detents, momentum and snap come
 * from the release; the rulers at both sides carry a dot every third of a slot. */
#define PK_STEP 94.0f
#define PK_RESP 0.50f
#define PK_TAP_RESP 0.30f
typedef struct {
    int n, idx;
    float x, x0;            /* drum position, px (value i is centred at i * PK_STEP) */
    mspring sp;
    int dragging, springing, over;
    int locked;             /* idx was set by a tap; the spring only carries the drum there */
} pk;
typedef struct { int c; char s[5][12]; } pk_txt;   /* texts of values c-2..c+2 */

static float pk_max(const pk *p) { return (float)(p->n - 1) * PK_STEP; }
static void pk_init(pk *p, int n, int idx)
{
    memset(p, 0, sizeof *p);
    p->n = n < 1 ? 1 : n;
    p->idx = idx < 0 ? 0 : idx >= p->n ? p->n - 1 : idx;
    p->x = p->idx * PK_STEP;
    msp_init(&p->sp, p->x, PK_RESP, 1.0f);
}
static int pk_centre(const pk *p) { return (int)floorf(m_clamp(p->x, 0, pk_max(p)) / PK_STEP + 0.5f); }
static void pk_grab(pk *p) { p->dragging = 1; p->springing = 0; p->locked = 0; p->x0 = p->x; p->over = 0; }
static void pk_drag(pk *p, float dy)
{
    float raw = p->x0 - dy, mx = pk_max(p);
    int over = 0;
    /* past either end the drum gives under one slot, so the last value never leaves the line */
    if (raw < 0) { raw = rubber_band(raw, PK_STEP); over = 1; }
    else if (raw > mx) { raw = mx + rubber_band(raw - mx, PK_STEP); over = 1; }
    if (over && !p->over) nos_feedback(FB_LIMIT);
    p->over = over;
    p->x = raw;
}
static void pk_go_r(pk *p, float target, float v, float resp)
{
    float dist = target - p->x;
    float w = 6.2831853f / resp;
    /* a critically damped spring overshoots when it starts faster than w*dist toward
     * its target: cap the hand-off so the snap is a click, never a bounce */
    if (dist * v > 0 && fabsf(v) > w * fabsf(dist)) v = (v > 0 ? 1.0f : -1.0f) * w * fabsf(dist);
    if (dist * v < 0) v *= 0.3f;
    msp_init(&p->sp, p->x, resp, 1.0f);
    p->sp.v = v;
    msp_to(&p->sp, target);
    p->springing = 1;
}
static void pk_go(pk *p, float target, float v) { pk_go_r(p, target, v, PK_RESP); }
static void pk_release(pk *p, float vy)
{
    p->dragging = 0;
    float v = -vy, mx = pk_max(p);
    float rest = (p->x < 0 || p->x > mx) ? p->x : decay_project(p->x, v);
    float k = floorf(m_clamp(rest, 0, mx) / PK_STEP + 0.5f);
    pk_go(p, k * PK_STEP, (p->x < 0 || p->x > mx) ? 0 : v);
}
/* a tap on a neighbour: the value changes now (tick, commit); the drum carries it in */
static int pk_step(pk *p, int d)
{
    int t = p->idx + d;
    if (t < 0 || t >= p->n) { nos_feedback(FB_LIMIT); return 0; }
    p->idx = t;
    p->locked = 1;
    nos_feedback(FB_TICK);
    pk_go_r(p, t * PK_STEP, 0, PK_TAP_RESP);
    return 1;
}
/* advance; returns 1 when the detent on the centre line changed */
static int pk_tick(pk *p, float dt)
{
    if (p->springing && !p->dragging) {
        if (!msp_step(&p->sp, dt)) { p->springing = 0; p->locked = 0; p->x = p->sp.target; }
        else p->x = p->sp.x;
    }
    if (p->locked) return 0;
    int k = pk_centre(p);
    if (k != p->idx) { p->idx = k; nos_feedback(FB_TICK); return 1; }
    return 0;
}
static void pk_texts(const pk *p, pk_txt *t, void (*fmt)(int i, char *o, int n))
{
    t->c = pk_centre(p);
    for (int j = 0; j < 5; j++) {
        int i = t->c - 2 + j;
        if (i < 0 || i >= p->n) t->s[j][0] = 0;
        else fmt(i, t->s[j], sizeof t->s[j]);
    }
}
static float hero_pitch(const char *s)
{
    int n = 0;
    for (const char *c = s; *c; c++) if (*c != ':' && *c != '.' && *c != ',') n++;
    return n <= 4 ? UI_HERO_PITCH : UI_HEROW_PITCH;
}
/* one drum column centred on (cx, cyl); `hl` = value index drawn in C_PRI (pressed) */
static void pk_column(gfx_fb *fb, const pk *p, const pk_txt *t, float cx, float cyl, float bp,
                      unsigned seed, int hl)
{
    for (int j = 0; j < 5; j++) {
        int i = t->c - 2 + j;
        if (!t->s[j][0]) continue;
        float d = i * PK_STEP - p->x;
        float a = fabsf(d) / PK_STEP;
        if (a >= 1.6f) continue;
        float s = a < 1 ? 1.0f - a * a * (3.0f - 2.0f * a) : 0;
        float hp = hero_pitch(t->s[j]);
        float pitch = floorf((UI_SUB_PITCH + (hp - UI_SUB_PITCH) * s) * 8.0f + 0.5f) / 8.0f;
        uint16_t col = a < 1 ? ui_mix(C_DIS, C_WHITE, q8(s)) : ui_dim(C_DIS, q8(1.0f - (a - 1.0f) / 0.6f));
        if (i == hl && a > 0.5f) col = C_PRI;
        dot_style st = { DOT_FONT_TALL, pitch, pitch * 0.8f, col, 0, 1, 1 };
        float y = floorf((cyl + d - 4.5f * pitch) * 2.0f + 0.5f) * 0.5f;
        if (bp < 1) dot_text_anim(fb, &st, cx, y, t->s[j], 1, bp, DOT_REVEAL_RANDOM, seed + (unsigned)i);
        else dot_text_a(fb, &st, cx, y, t->s[j], 1);
    }
}
/* ruler: a dot every third of a slot, the detent dots larger, brightest at the line */
static void pk_ruler(gfx_fb *fb, const pk *p, float x, float cyl)
{
    const float st = PK_STEP / 3.0f;
    int j0 = (int)floorf((p->x - 150.0f) / st), j1 = (int)ceilf((p->x + 150.0f) / st);
    if (j0 < 0) j0 = 0;
    if (j1 > (p->n - 1) * 3) j1 = (p->n - 1) * 3;
    for (int j = j0; j <= j1; j++) {
        float y = cyl + j * st - p->x;
        float dy = fabsf(y - cyl);
        if (dy > 150.0f) continue;
        float k = 1.0f - dy / 150.0f;
        k = q8(k * k);
        int major = j % 3 == 0;
        float d = major ? 4.0f + 3.0f * q8(1.0f - m_clamp(dy / st, 0, 1)) : 3.0f;
        uint16_t c = ui_mix(C_LINE, major ? C_SEC : C_DIS, k);
        if (major && dy < st * 0.5f) c = ui_mix(C_SEC, C_WHITE, q8(1.0f - dy / (st * 0.5f)));
        dot_disc(fb, x, floorf(y * 2.0f + 0.5f) * 0.5f, d, c);
    }
}

/* ======================================================================= values */
static const int16_t k_tz[] = {
    -720, -660, -600, -570, -540, -480, -420, -360, -300, -240, -210, -180, -120, -60, 0,
    60, 120, 180, 210, 240, 270, 300, 330, 345, 360, 390, 420, 480, 525, 540, 570, 600,
    630, 660, 720, 765, 780, 825, 840 };
#define N_TZ ((int)(sizeof k_tz / sizeof k_tz[0]))
static const uint8_t k_timeout[] = { 5, 10, 15, 30, 60 };

static void fmt_tz(int min, char *o, int n, int utc)
{
    int a = min < 0 ? -min : min;
    if (utc) snprintf(o, (size_t)n, "UTC%c%d:%02d", min < 0 ? '-' : '+', a / 60, a % 60);
    else snprintf(o, (size_t)n, "%c%d:%02d", min < 0 ? '-' : '+', a / 60, a % 60);
}
/* house number format: a thousands separator only from 10,000 */
static void fmt_steps(int s, char *o, int n)
{
    if (s >= 10000) snprintf(o, (size_t)n, "%d,%03d", s / 1000, s % 1000);
    else snprintf(o, (size_t)n, "%d", s);
}
static int tz_index(int min)
{
    int best = 0;
    for (int i = 1; i < N_TZ; i++) if (abs(k_tz[i] - min) < abs(k_tz[best] - min)) best = i;
    return best;
}
static int timeout_index(int s)
{
    int best = 0;
    for (int i = 1; i < 5; i++) if (abs(k_timeout[i] - s) < abs(k_timeout[best] - s)) best = i;
    return best;
}
static int phone_linked(void) { return hal_phone_connected() || svc_sys()->phone_connected; }
/* one name per radio state, shared with Modules */
static const char *wifi_word(void)
{
    hal_conn c = hal_status(HAL_FEAT_WIFI);
    return !nos_prefs.wifi ? "Off" : c == HAL_CONN_UP ? "Connected" : c == HAL_CONN_TRYING ? "Joining" : "Not connected";
}

/* ======================================================================= main list */
enum { K_HDR = 0, K_NAV, K_TOG, K_FLIP, K_ARM, K_DANGER };
enum {
    I_BRIGHT = 0, I_AOD, I_TIMEOUT, I_RAISE, I_FACE,
    I_SOUND, I_VOLUME, I_HAPTICS, I_HSTR, I_DND,
    I_BT, I_WIFI, I_CELL, I_GPS,
    I_H24, I_TZ, I_SETTIME,
    I_GOAL, I_WRIST, I_UNITS,
    I_MODULES, I_STORAGE, I_ABOUT, I_RESTART, I_RESET,
    I_COUNT, I_NONE = 255
};
typedef struct { uint8_t kind, id; const char *label; } sentry;
static const sentry E[] = {
    { K_HDR, I_NONE, "Display" },
    { K_NAV, I_BRIGHT, "Brightness" },
    { K_TOG, I_AOD, "Always-on display" },
    { K_NAV, I_TIMEOUT, "Screen timeout" },
    { K_TOG, I_RAISE, "Raise to wake" },
    { K_NAV, I_FACE, "Watch face" },
    { K_HDR, I_NONE, "Sound & haptics" },
    { K_TOG, I_SOUND, "UI sounds" },
    { K_NAV, I_VOLUME, "Volume" },
    { K_TOG, I_HAPTICS, "Haptics" },
    { K_NAV, I_HSTR, "Haptic strength" },
    { K_TOG, I_DND, "Do not disturb" },
    { K_HDR, I_NONE, "Connections" },
    { K_NAV, I_BT, "Bluetooth" },
    { K_NAV, I_WIFI, "Wi-Fi" },
    { K_NAV, I_CELL, "Cellular" },
    { K_TOG, I_GPS, "GPS auto" },
    { K_HDR, I_NONE, "Time" },
    { K_TOG, I_H24, "24-hour clock" },
    { K_NAV, I_TZ, "Time zone" },
    { K_NAV, I_SETTIME, "Set time" },
    { K_HDR, I_NONE, "Activity" },
    { K_NAV, I_GOAL, "Step goal" },
    { K_FLIP, I_WRIST, "Wrist" },
    { K_FLIP, I_UNITS, "Units" },
    { K_HDR, I_NONE, "System" },
    { K_NAV, I_MODULES, "Modules" },
    { K_NAV, I_STORAGE, "Storage" },
    { K_NAV, I_ABOUT, "About" },
    { K_ARM, I_RESTART, "Restart" },
    { K_DANGER, I_RESET, "Reset settings" },
};
#define NE ((int)(sizeof E / sizeof E[0]))
#define UNIT 30
#define LTOP 96
static int units_of(int kind) { return kind == K_HDR ? 2 : kind == K_DANGER ? 4 : 3; }

static struct {
    ui_list L;
    int start[NE];              /* first unit of each entry */
    int8_t unit_e[NE * 4];      /* unit -> entry */
    int nunits;
    int inited;
    int was_moving;
    float build_t0;
    mspring tog[I_COUNT];
    char val[I_COUNT][24];
    char unit[I_COUNT][8];
    /* a changed value cross-fades from the old one (which may sit on the other line) */
    char oval[I_COUNT][24];
    char ounit[I_COUNT][8];
    float vt0[I_COUNT];
    int xf_ok;
    float stor_t, mod_t;
    hal_storage stor;
    int mods_on, mods_tot;
    uint32_t ext;
    int flash_e; float flash_t;  /* pressed pill held after a quick tap */
    int arm_e; float arm_t;      /* armed Restart / Reset */
    mspring arm_sp[2];           /* 0 Restart, 1 Reset: plain -> armed */
    mspring grey_sp;             /* Reset's red fades to grey while Restart is armed */
    float restart_at;            /* pending restart after the toast */
} S;

static uint8_t *tog_pref(int id)
{
    switch (id) {
    case I_AOD: return &nos_prefs.aod;
    case I_RAISE: return &nos_prefs.raise_wake;
    case I_SOUND: return &nos_prefs.sound;
    case I_HAPTICS: return &nos_prefs.haptics;
    case I_DND: return &nos_prefs.dnd;
    case I_GPS: return &nos_prefs.gps_auto;
    case I_H24: return &nos_prefs.hour24;
    default: return 0;
    }
}
/* toggles whose hardware is a strap module */
static int tog_absent(int id)
{
    uint32_t m = S.ext;
    return id == I_GPS ? !(m & EXT_GPS) : id == I_SOUND ? !(m & EXT_SPEAKER) :
           id == I_HAPTICS ? !(m & EXT_HAPTIC) : 0;
}
static const char *tog_missing(int id) { return id == I_GPS ? "NO GPS" : id == I_SOUND ? "NO SPEAKER" : "NO MOTOR"; }

static void set_val(int id, const char *v, const char *u)
{
    if (!u) u = "";
    if (!strcmp(S.val[id], v) && !strcmp(S.unit[id], u)) return;
    if (S.xf_ok) {
        memcpy(S.oval[id], S.val[id], sizeof S.oval[id]);
        memcpy(S.ounit[id], S.unit[id], sizeof S.ounit[id]);
        S.vt0[id] = nos_t;
    }
    snprintf(S.val[id], sizeof S.val[id], "%s", v);
    snprintf(S.unit[id], sizeof S.unit[id], "%s", u);
}

static void update_values(void)
{
    char b[24];
    if (nos_prefs.auto_bright && (S.ext & EXT_LIGHT)) set_val(I_BRIGHT, "Auto", 0);
    else { snprintf(b, sizeof b, "%d%%", nos_prefs.brightness); set_val(I_BRIGHT, b, 0); }
    snprintf(b, sizeof b, "%d", nos_prefs.sleep_s); set_val(I_TIMEOUT, b, "s");
    int nf = nos_face_count;
    int fi = nos_prefs.face < nf ? nos_prefs.face : 0;
    set_val(I_FACE, nf > 0 && nos_faces[fi] && nos_faces[fi]->name ? nos_faces[fi]->name : "", 0);
    snprintf(b, sizeof b, "%d%%", nos_prefs.volume); set_val(I_VOLUME, b, 0);
    snprintf(b, sizeof b, "%d%%", nos_prefs.haptic_strength); set_val(I_HSTR, b, 0);
    set_val(I_BT, phone_linked() ? "Connected" : "Not connected", 0);
    set_val(I_WIFI, wifi_word(), 0);
    hal_lte lte;
    if (!(S.ext & EXT_LTE) || !hal_lte_get(&lte)) set_val(I_CELL, "No module", 0);
    else if (lte.powered && (lte.registered == 1 || lte.registered == 2) && lte.op[0]) {
        snprintf(b, sizeof b, "%.12s", lte.op); set_val(I_CELL, b, 0);
    } else set_val(I_CELL, lte.powered ? "On" : "Off", 0);
    fmt_tz(nos_prefs.tz_min, b, sizeof b, 1); set_val(I_TZ, b, 0);
    hal_datetime dt; hal_get_datetime(&dt);
    ui_fmt_time(b, sizeof b, dt.hour, dt.minute); set_val(I_SETTIME, b, 0);
    fmt_steps(nos_prefs.step_goal_100 * 100, b, sizeof b); set_val(I_GOAL, b, 0);
    set_val(I_WRIST, nos_prefs.wrist_left ? "Left" : "Right", 0);
    set_val(I_UNITS, nos_prefs.units_metric ? "Metric" : "Imperial", 0);
    snprintf(b, sizeof b, "%d/%d", S.mods_on, S.mods_tot); set_val(I_MODULES, b, 0);
    if (S.stor.mounted) {
        double fr = (double)S.stor.free_bytes / (1024.0 * 1024.0 * 1024.0);
        snprintf(b, sizeof b, fr >= 10 ? "%.0f" : "%.1f", fr);
        set_val(I_STORAGE, b, "GB");
    } else set_val(I_STORAGE, "No card", 0);
    set_val(I_ABOUT, SET_VERSION, 0);
    set_val(I_RESTART, "", 0);
}

static void list_setup(void)
{
    int u = 0;
    for (int e = 0; e < NE; e++) {
        S.start[e] = u;
        int k = units_of(E[e].kind);
        for (int j = 0; j < k; j++) S.unit_e[u + j] = (int8_t)e;
        u += k;
    }
    S.nunits = u;
    ui_list_init(&S.L, S.nunits, UNIT, LTOP, 0, 0);
    S.L.bottom_pad = GFX_H / 2 - 60;      /* the last row can reach the centre */
    ui_list_set_n(&S.L, S.nunits);
    S.L.bar_t = -10.0f;
}

static void settings_sync_toggles(void)
{
    for (int i = 0; i < I_COUNT; i++) {
        uint8_t *p = tog_pref(i);
        msp_init(&S.tog[i], p && *p ? 1.0f : 0.0f, SPRING_SNAPPY);
    }
}

static void settings_enter(void)
{
    /* revealed by back keeps scroll and skips the build; a fresh entry builds from the
     * frame the engine says it can be seen (40 % of the launcher's iris) */
    if (!S.inited || !nos_revealed()) {
        list_setup();
        S.build_t0 = nos_enter_t();
        S.inited = 1;
    }
    settings_sync_toggles();
    S.ext = hal_ext_present();
    hal_storage_get(&S.stor);
    S.stor_t = nos_t;
    S.mods_on = set_modules_online(&S.mods_tot);
    S.mod_t = nos_t;
    S.flash_e = -1;
    S.arm_e = -1;
    for (int j = 0; j < 2; j++) msp_init(&S.arm_sp[j], 0, 0.20f, 1.0f);
    msp_init(&S.grey_sp, 0, 0.15f, 1.0f);
    S.restart_at = -1;
    S.xf_ok = 0;
    update_values();
    memcpy(S.oval, S.val, sizeof S.oval);
    memcpy(S.ounit, S.unit, sizeof S.ounit);
    for (int i = 0; i < I_COUNT; i++) S.vt0[i] = -100.0f;
    S.xf_ok = 1;
}

static void spring_to(mspring *s, float t) { if (s->target != t) msp_to(s, t); }

static void settings_tick(float dt)
{
    ui_list_tick(&S.L, dt);
    for (int i = 0; i < I_COUNT; i++) msp_step(&S.tog[i], dt);
    S.ext = hal_ext_present();
    if (nos_t - S.stor_t > 2.0f) { hal_storage_get(&S.stor); S.stor_t = nos_t; }
    if (nos_t - S.mod_t > 1.0f) { S.mods_on = set_modules_online(&S.mods_tot); S.mod_t = nos_t; }
    if (S.arm_e >= 0 && (nos_t - S.arm_t > ARM_S || S.L.dragging)) S.arm_e = -1;
    int armed_id = S.arm_e >= 0 ? E[S.arm_e].id : I_NONE;
    spring_to(&S.arm_sp[0], armed_id == I_RESTART ? 1.0f : 0.0f);
    spring_to(&S.arm_sp[1], armed_id == I_RESET ? 1.0f : 0.0f);
    spring_to(&S.grey_sp, armed_id == I_RESTART ? 1.0f : 0.0f);
    for (int j = 0; j < 2; j++) msp_step(&S.arm_sp[j], dt);
    msp_step(&S.grey_sp, dt);
    if (S.restart_at >= 0 && nos_t >= S.restart_at) { S.restart_at = -1; nos_host_power(1); }
    update_values();
}

static float list_off(void) { return floorf(ui_list_offset(&S.L) + 0.5f); }

static int entry_at(int y)
{
    float fy = y + list_off() - LTOP;
    if (fy < 0) return -1;
    int u = (int)(fy / UNIT);
    return u < S.nunits ? S.unit_e[u] : -1;
}

/* build-in: rows rise 14 px and fade in, 34 ms apart from the top down (<= 6 rows) */
static float build_k(float y, float *dy)
{
    float t = nos_t - S.build_t0;
    int k = (int)((y - 60.0f) / 80.0f);
    if (k < 0) k = 0;
    if (k > 5) k = 5;
    float p = m_clamp((t - k * 0.034f) / 0.20f, 0, 1);
    float e = bezier_ease(&EASE_STANDARD, p);
    e = floorf(e * 8.0f + 0.5f) / 8.0f;
    *dy = (1.0f - e) * 14.0f;
    return e;
}

/* a value too wide to keep ROW_GAP to its label in the standard column goes under it */
static int value_under(const char *label, const char *v, const char *u)
{
    if (!v || !v[0]) return 0;
    return UI_ROW_X0 + ui_text_width(UI_F_BODY, label, 0) + ROW_GAP + value_w(v, u) > UI_VALUE_X;
}

/* one value of a row: on the label's line (right-aligned at vx; it fades out before it
 * gets closer than 8 px to the label) or under the label (left-aligned at tx) */
static void row_value(gfx_fb *fb, const char *v, const char *u, int under, int vx, int tx, int lw,
                      float cy, float k, float a, int dy)
{
    if (!v[0] || a <= 0) return;
    if (under) {
        value_draw(fb, tx, cap_base(UI_F_LABEL, cy + 17) + dy, v, u, ui_dim(C_SEC, k * a), UI_LEFT);
        return;
    }
    int gap = vx - value_w(v, u) - (tx + lw);
    float g = gap >= ROW_GAP ? 1.0f : q8((gap - 8) / (float)(ROW_GAP - 8));
    if (g <= 0) return;
    value_draw(fb, vx, cap_base(UI_F_LABEL, cy) + dy, v, u, ui_dim(C_SEC, k * a * g), UI_RIGHT);
}

static void draw_entry(gfx_fb *fb, int e, float y, float h, int pressed)
{
    const sentry *en = &E[e];
    float bdy, bk = build_k(y, &bdy);
    if (bk <= 0) return;
    y += floorf(bdy + 0.5f);
    float cy = y + h * 0.5f;
    float k = qfade(cy) * bk;
    int lx, rx;
    row_cols(cy, &lx, &rx);
    if (rx - lx < 120) return;

    if (en->kind == K_HDR) {
        ui_label(fb, lx, (int)(y + h - 16), en->label, ui_dim(C_DIS, k), UI_LEFT);
        return;
    }
    if (en->kind == K_DANGER) {
        float a = m_clamp(S.arm_sp[1].x, 0, 1);
        float w = m_lerp(260.0f, 320.0f, a);
        arm_pill(fb, cy, w, "RESET SETTINGS", "TAP AGAIN TO RESET", S.arm_t, a, pressed,
                 1.0f - q8(S.grey_sp.x), k);
        return;
    }
    if (en->kind == K_ARM) {
        float a = m_clamp(S.arm_sp[0].x, 0, 1);
        float ain = q8(a * 2.0f - 1.0f), aout = q8(1.0f - a * 2.0f);
        if (ain > 0) { arm_pill(fb, cy, 320, "", "TAP AGAIN TO RESTART", S.arm_t, a, 0, 1, k); return; }
        if (a > 0) pressed = 0;                      /* arming is the feedback */
        k *= aout;
        if (k <= 0) return;
    }
    if (pressed) ui_rrect(fb, lx - 20, y + 5, rx - lx + 40, h - 10, (h - 10) * 0.5f, C_SURF2);
    int id = en->id;
    int lw = ui_text_width(UI_F_BODY, en->label, 0);
    const int vx = rx - (UI_ROW_X1 - UI_VALUE_X);    /* value right edge (384 in the column) */
    int has_v = en->kind == K_NAV || en->kind == K_FLIP;
    float vp = has_v ? (nos_t - S.vt0[id]) / VALUE_XF : 1.0f;
    int und = has_v && value_under(en->label, S.val[id], S.unit[id]);
    int und_o = has_v && vp < 1 && value_under(en->label, S.oval[id], S.ounit[id]);
    /* label line: centred, or raised when the value sits under it (eased across a change) */
    float tl = vp >= 1 ? (float)und : m_lerp((float)und_o, (float)und, bezier_ease(&EASE_STANDARD, m_clamp(vp, 0, 1)));
    int bl = (int)lroundf(m_lerp((float)cap_base(UI_F_BODY, cy), (float)cap_base(UI_F_BODY, cy - 12), tl));
    /* the control's left edge: the label gives way to it near the bezel, never the reverse */
    int cl = rx;
    switch (en->kind) {
    case K_TOG: cl = rx - 76; break;
    case K_NAV: case K_FLIP: cl = (S.val[id][0] && !und) ? vx - value_w(S.val[id], S.unit[id]) : vx; break;
    default: break;
    }
    int tx = lx;
    if (tx + lw + ROW_GAP > cl) {
        int g0, g1;
        ui_row_span(cy, 2, &g0, &g1);
        tx = cl - ROW_GAP - lw;
        if (tx < g0) tx = g0;                        /* never past the glass */
        if (tx > lx) tx = lx;
    }
    ui_text(fb, UI_F_BODY, tx, bl, en->label, ui_dim(C_PRI, k), UI_LEFT, 0);
    switch (en->kind) {
    case K_TOG: {
        int gap = cl - (tx + lw);
        float vk = k * (gap >= ROW_GAP ? 1.0f : q8((gap - 8) / (float)(ROW_GAP - 8)));
        if (vk > 0) toggle_k(fb, rx - 38, cy, S.tog[id].x, vk, tog_absent(id));
        break;
    }
    case K_NAV: case K_FLIP:
        if (en->kind == K_NAV) chevron(fb, rx, cy, ui_dim(C_DIS, k));
        else flipglyph(fb, rx, cy, ui_dim(C_DIS, k));
        if (vp >= 1) row_value(fb, S.val[id], S.unit[id], und, vx, tx, lw, cy, k, 1, 0);
        else if (vp < 0.5f) {
            float a = q8(1.0f - vp * 2.0f);
            row_value(fb, S.oval[id], S.ounit[id], und_o, vx, tx, lw, cy, k, a, -(int)((1 - a) * 8));
        } else {
            float a = q8(vp * 2.0f - 1.0f);
            row_value(fb, S.val[id], S.unit[id], und, vx, tx, lw, cy, k, a, (int)((1 - a) * 8));
        }
        break;
    default: break;
    }
}

static int pressed_entry(void)
{
    if (S.flash_e >= 0 && nos_t - S.flash_t < PRESS_FLASH) return S.flash_e;
    if (S.L.pressed >= 0 && S.L.pressed < S.nunits && nos_t - S.L.press_t > PRESS_HOLD) return S.unit_e[S.L.pressed];
    return -1;
}

static void settings_draw(gfx_fb *fb)
{
    float off = list_off();
    ui_title(fb, "SETTINGS", off);
    int pe = pressed_entry();
    for (int e = 0; e < NE; e++) {
        float h = (float)(units_of(E[e].kind) * UNIT);
        float y = LTOP + S.start[e] * UNIT - off;
        if (y + h < 0) continue;
        if (y > GFX_H) break;
        draw_entry(fb, e, y, h, e == pe && E[e].kind != K_HDR);
    }
    float content = (float)(LTOP + S.nunits * UNIT + S.L.bottom_pad);
    ui_scrollbar(fb, S.L.sc.x, content, GFX_H, S.L.bar_alpha);
}

/* ---- sub-page declarations ---- */
static const nos_screen scr_set_pick, scr_set_slide, scr_set_radio, scr_set_time;
static void open_picker(int id);
static void open_slider(int id);

static void flip(int id)
{
    if (id == I_WRIST) nos_prefs.wrist_left = !nos_prefs.wrist_left;
    else nos_prefs.units_metric = !nos_prefs.units_metric;
    prefs_save();
    update_values();                         /* the value cross-fades */
    nos_feedback(FB_TAP);
}

static void reset_all(void)
{
    prefs_defaults();
    prefs_save();
    hal_store_save("nosprefs", &nos_prefs, sizeof nos_prefs);
    settings_sync_toggles();
    hal_set_backlight((uint8_t)(nos_prefs.brightness * 255 / 100));
    hal_audio_volume(nos_prefs.volume);
    hal_haptic_set_strength(nos_prefs.haptic_strength);
    nos_feedback(FB_SUCCESS);
    nos_toast("RESET");
    update_values();
}

static void activate(int e)
{
    const sentry *en = &E[e];
    if (en->kind != K_ARM && en->kind != K_DANGER) S.arm_e = -1;
    switch (en->kind) {
    case K_HDR: return;
    case K_TOG: {
        uint8_t *p = tog_pref(en->id);
        if (!p) return;
        int on = !*p;
        if (!on) nos_feedback(FB_TOGGLE_OFF);   /* felt before haptics/sound turn off */
        *p = (uint8_t)on;
        if (on) nos_feedback(FB_TOGGLE_ON);
        msp_to(&S.tog[en->id], on ? 1.0f : 0.0f);
        prefs_save();
        if (tog_absent(en->id)) nos_toast(tog_missing(en->id));   /* saved, but nothing is live */
        return;
    }
    case K_FLIP: flip(en->id); return;
    case K_ARM: case K_DANGER:
        if (S.arm_e != e) { S.arm_e = e; S.arm_t = nos_t; nos_feedback(FB_LIMIT); return; }
        S.arm_e = -1;
        if (en->id == I_RESET) reset_all();
        else {
            hal_store_save("nosprefs", &nos_prefs, sizeof nos_prefs);
            nos_feedback(FB_ERROR);
            nos_toast("RESTARTING");
            S.restart_at = nos_t + 0.8f;
        }
        return;
    default: break;
    }
    nos_feedback(FB_TAP);
    switch (en->id) {
    case I_BRIGHT: case I_VOLUME: case I_HSTR: open_slider(en->id); break;
    case I_TIMEOUT: case I_TZ: case I_GOAL: open_picker(en->id); break;
    case I_BT: case I_WIFI: set_settings_open_radio(en->id == I_BT, 0); break;
    case I_FACE: nos_push(&scr_faces, TX_PUSH); break;
    case I_CELL: set_modules_open_lte(); break;
    case I_SETTIME: nos_push(&scr_set_time, TX_PUSH); break;
    case I_MODULES: nos_push(&scr_modules, TX_PUSH); break;
    case I_STORAGE: set_modules_open_sd(); break;
    case I_ABOUT: nos_push(&scr_about, TX_PUSH); break;
    }
}

static int settings_input(const nos_input *in)
{
    if (in->type == IN_DOWN) S.was_moving = S.L.sc.moving && fabsf(S.L.sc.v) > 60.0f;
    if (in->type == IN_TAP) {
        nos_input up = *in;
        up.type = IN_UP;
        ui_list_input(&S.L, &up);             /* release the hold; no list feedback */
        if (!S.was_moving) {
            int e = entry_at(in->y);
            if (e >= 0 && E[e].kind != K_HDR) { S.flash_e = e; S.flash_t = nos_t; activate(e); }
            else S.arm_e = -1;
        }
        return 1;
    }
    return ui_list_input(&S.L, in);
}

const nos_screen scr_settings = { "SETTINGS", settings_enter, 0, settings_tick,
                                  settings_draw, settings_input, 0 };

/* ======================================================================= picker page */
static struct {
    int id;
    pk p;
    pk_txt tx;
    char extra[24];
    int pressed;            /* -1 none, 0 prev, 1 next */
    int tz0;                /* tz when the page opened (clock shift on leave) */
    float t0;
} P;

static const char *pick_title(int id)
{
    return id == I_TIMEOUT ? "SCREEN TIMEOUT" : id == I_TZ ? "TIME ZONE" : "STEP GOAL";
}
static const char *pick_unit(int id)
{
    return id == I_TIMEOUT ? "SECONDS" : id == I_TZ ? "UTC OFFSET" : "STEPS A DAY";
}
static int pick_n(int id) { return id == I_TIMEOUT ? 5 : id == I_TZ ? N_TZ : 57; }
static void pick_fmt(int i, char *o, int n)
{
    if (i < 0 || i >= pick_n(P.id)) { o[0] = 0; return; }
    if (P.id == I_TIMEOUT) snprintf(o, (size_t)n, "%d", k_timeout[i]);
    else if (P.id == I_TZ) fmt_tz(k_tz[i], o, n, 0);
    else fmt_steps(2000 + i * 500, o, n);
}
static void pick_extra(int id, int i, char *o, int n)
{
    o[0] = 0;
    if (id == I_TZ) {
        hal_datetime dt; hal_get_datetime(&dt);
        int m = dt.hour * 60 + dt.minute + (k_tz[i] - P.tz0);
        m = ((m % 1440) + 1440) % 1440;
        char t[12];
        ui_fmt_time(t, sizeof t, m / 60, m % 60);
        snprintf(o, (size_t)n, "%s LOCAL", t);
    } else if (id == I_GOAL) {
        float km = (2000 + i * 500) * 0.00075f;
        snprintf(o, (size_t)n, "%.1f", nos_prefs.units_metric ? km : km * 0.6214f);
    }
}
static void pick_commit(void)
{
    int i = P.p.idx;
    if (P.id == I_TIMEOUT) nos_prefs.sleep_s = k_timeout[i];
    else if (P.id == I_TZ) nos_prefs.tz_min = k_tz[i];
    else nos_prefs.step_goal_100 = (uint16_t)((2000 + i * 500) / 100);
    prefs_save();
}
static void pick_texts(void)
{
    pk_texts(&P.p, &P.tx, pick_fmt);
    pick_extra(P.id, P.p.idx, P.extra, sizeof P.extra);
}

static void open_picker(int id) { P.id = id; nos_push(&scr_set_pick, TX_PUSH); }

static void pick_enter(void)
{
    int idx = P.id == I_TIMEOUT ? timeout_index(nos_prefs.sleep_s) :
              P.id == I_TZ ? tz_index(nos_prefs.tz_min) :
              (int)m_clamp((nos_prefs.step_goal_100 * 100 - 2000) / 500.0f + 0.5f, 0, 56);
    pk_init(&P.p, pick_n(P.id), idx);
    P.tz0 = P.id == I_TZ ? k_tz[idx] : nos_prefs.tz_min;
    P.pressed = -1;
    P.t0 = nos_enter_t();
    pick_texts();
}
static void pick_leave(void)
{
    if (P.id == I_TZ && nos_prefs.tz_min != P.tz0) {
        /* shift the wall clock by the change in offset (UTC stays put) */
        hal_datetime dt; hal_get_datetime(&dt);
        int delta = nos_prefs.tz_min - P.tz0;
        int m = dt.hour * 60 + dt.minute + delta;
        int dd = 0;
        while (m < 0) { m += 1440; dd--; }
        while (m >= 1440) { m -= 1440; dd++; }
        dt.hour = m / 60; dt.minute = m % 60;
        if (dd) {
            static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
            int ml = mdays[(dt.month - 1) % 12] + (dt.month == 2 && dt.year % 4 == 0);
            dt.day += dd; dt.weekday = (dt.weekday + dd + 7) % 7;
            if (dt.day < 1) { dt.month--; if (dt.month < 1) { dt.month = 12; dt.year--; } dt.day = mdays[(dt.month - 1) % 12] + (dt.month == 2 && dt.year % 4 == 0); }
            else if (dt.day > ml) { dt.day = 1; dt.month++; if (dt.month > 12) { dt.month = 1; dt.year++; } }
        }
        hal_set_datetime(&dt);
        P.tz0 = nos_prefs.tz_min;
    }
}
static void pick_tick(float dt)
{
    if (pk_tick(&P.p, dt)) pick_commit();
    pick_texts();
}

/* the drum window: values are clipped to it, the labels below stay clear */
#define DRUM_Y0 104
#define DRUM_Y1 352
static void pick_draw(gfx_fb *fb)
{
    ui_title(fb, pick_title(P.id), 0);
    float bp = m_clamp((nos_t - P.t0) / UI_ENTRY_S, 0, 1);
    pk_ruler(fb, &P.p, 70, UI_CY);
    pk_ruler(fb, &P.p, GFX_W - 70, UI_CY);
    gfx_fb sub;
    if (nos_subview(fb, &sub, 0, DRUM_Y0, GFX_W, DRUM_Y1, 0, 0)) {
        int hl = P.pressed == 0 ? P.p.idx - 1 : P.pressed == 1 ? P.p.idx + 1 : -9;
        pk_column(&sub, &P.p, &P.tx, UI_CX, UI_CY, bp, 7, hl);
    }
    ui_label(fb, UI_CX, 376, pick_unit(P.id), C_SEC, UI_CENTER);
    if (P.extra[0]) {
        if (P.id == I_GOAL) ui_label_unit(fb, UI_CX, 402, P.extra, nos_prefs.units_metric ? "km" : "mi", C_SEC, UI_CENTER);
        else ui_label(fb, UI_CX, 402, P.extra, C_SEC, UI_CENTER);
    }
}

static int pick_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN:
        pk_grab(&P.p);
        P.p.dragging = 0;                      /* hold still; a drag re-grabs */
        P.p.springing = 0;
        P.pressed = in->y < UI_CY - 60 ? 0 : (in->y > UI_CY + 60 && in->y < 360) ? 1 : -1;
        return 1;
    case IN_DRAG_START:
        P.pressed = -1;
        if (in->axis != AXIS_Y) { pk_go(&P.p, P.p.idx * PK_STEP, 0); return 0; }
        pk_grab(&P.p);
        pk_drag(&P.p, in->dy);
        return 1;
    case IN_DRAG:
        if (!P.p.dragging) return 0;
        pk_drag(&P.p, in->dy);
        return 1;
    case IN_DRAG_END:
        if (!P.p.dragging) return 0;
        pk_release(&P.p, in->vy);
        return 1;
    case IN_TAP:
        if (P.pressed == 0 || P.pressed == 1) {
            if (pk_step(&P.p, P.pressed ? 1 : -1)) { pick_commit(); pick_texts(); }
        } else pk_go(&P.p, P.p.idx * PK_STEP, 0);
        P.pressed = -1;
        return 1;
    case IN_UP:
        P.pressed = -1;
        if (!P.p.dragging && !P.p.springing) pk_go(&P.p, P.p.idx * PK_STEP, 0);
        return 1;
    default: return 0;
    }
}
static const nos_screen scr_set_pick = { "SET_PICK", pick_enter, pick_leave, pick_tick, pick_draw, pick_input, 0 };

/* ======================================================================= slider page
 * The control centre's control: ui_slider, 235 x 50, the icon riding the fill's end
 * cap. With no hardware behind it the fill goes grey: the value still saves. */
#define SL_W 235.0f
#define SL_H 50.0f
#define SL_X (UI_CX - SL_W * 0.5f)
static struct {
    int id;
    float v;                 /* 0-100, continuous while dragging */
    float v0;
    int dragging;
    mspring sp;              /* tap-to-position animation */
    int animating;
    int shown;               /* integer shown / committed */
    mtext hero;
    mspring tog;
    int pressed_row; float row_flash;
    float lux; int have_lux;
    float t0;
} SL;

static uint8_t *slide_pref(int id)
{
    return id == I_BRIGHT ? &nos_prefs.brightness : id == I_VOLUME ? &nos_prefs.volume : &nos_prefs.haptic_strength;
}
static float slide_min(void) { return SL.id == I_BRIGHT ? 5.0f : 0.0f; }   /* never a black screen */
static int slide_hw(void)
{
    uint32_t m = hal_ext_present();
    return SL.id == I_BRIGHT ? 1 : SL.id == I_VOLUME ? (m & EXT_SPEAKER) != 0 : (m & EXT_HAPTIC) != 0;
}
static int bright_auto_row(void) { return SL.id == I_BRIGHT && (hal_ext_present() & EXT_LIGHT); }
static float slide_y(void) { return bright_auto_row() ? 214.0f : 250.0f; }
static float slide_hero_y(void) { return bright_auto_row() ? 100.0f : 132.0f; }

static void slider_draw(gfx_fb *fb, float x, float y, float w, float h, float v, int icon, int live)
{
    if (live) { ui_slider(fb, x, y, w, h, v, icon); return; }
    float r = h * 0.5f;
    gfx_fill_pill(fb, x + r, y + r, x + w - r, r, C_SURF2);
    float fw = h + (w - h) * m_clamp(v, 0, 1);
    gfx_fill_pill(fb, x + r, y + r, x + fw - r, r, C_DIS);
    float ip = h * 0.065f + 1.0f;
    dot_icon(fb, icon, x + r, y + r, ip, ip * 0.8f, C_SURF2);
}

static void open_slider(int id) { SL.id = id; nos_push(&scr_set_slide, TX_PUSH); }

static void slide_apply(int v, int old)
{
    *slide_pref(SL.id) = (uint8_t)v;
    prefs_save();
    int s10 = v / 10, o10 = old / 10;
    if (SL.id == I_BRIGHT) hal_set_backlight((uint8_t)(v * 255 / 100));
    else if (SL.id == I_VOLUME) {
        hal_audio_volume(v);
        if (s10 != o10) { hal_sfx(SFX_TICK); if (nos_prefs.haptics) hal_haptic(HAPTIC_TICK); }
    } else {
        hal_haptic_set_strength(v);
        if (s10 != o10) hal_haptic(HAPTIC_CLICK);
    }
    if (SL.id == I_BRIGHT && s10 != o10) nos_feedback(FB_TICK);
    if ((v == (int)slide_min() || v == 100) && v != old) nos_feedback(FB_LIMIT);
}

static void slide_enter(void)
{
    SL.v = *slide_pref(SL.id);
    SL.shown = (int)SL.v;
    SL.dragging = SL.animating = 0;
    SL.pressed_row = 0;
    SL.row_flash = -100;
    char b[8]; snprintf(b, sizeof b, "%d%%", SL.shown);
    mt_init(&SL.hero, b, 0.14f);
    msp_init(&SL.tog, nos_prefs.auto_bright ? 1.0f : 0.0f, SPRING_SNAPPY);
    SL.t0 = nos_enter_t();
}
static void slide_tick(float dt)
{
    if (SL.animating) {
        if (!msp_step(&SL.sp, dt)) SL.animating = 0;
        SL.v = SL.sp.x;
    }
    msp_step(&SL.tog, dt);
    int v = (int)lroundf(m_clamp(SL.v, 0, 100));
    if (v != SL.shown) { int old = SL.shown; SL.shown = v; slide_apply(v, old); }
    char b[8]; snprintf(b, sizeof b, "%d%%", SL.shown);
    mt_set(&SL.hero, b);
    hal_light l;
    SL.have_lux = hal_light_get(&l) && l.valid;
    SL.lux = SL.have_lux ? l.lux : 0;
}
static void slide_draw(gfx_fb *fb)
{
    const char *title = SL.id == I_BRIGHT ? "BRIGHTNESS" : SL.id == I_VOLUME ? "VOLUME" : "HAPTIC STRENGTH";
    int icon = SL.id == I_BRIGHT ? DOT_ICON_BRIGHT : SL.id == I_VOLUME ? DOT_ICON_SPEAKER : DOT_ICON_HAND;
    ui_title(fb, title, 0);
    float bp = m_clamp((nos_t - SL.t0) / UI_ENTRY_S, 0, 1);
    dot_style hs = { DOT_FONT_TALL, UI_HERO_PITCH, UI_HERO_PITCH * 0.8f, C_WHITE, 0, 1, 1 };
    if (bp < 1) dot_text_anim(fb, &hs, UI_CX, slide_hero_y(), SL.hero.to, 1, bp, DOT_REVEAL_RANDOM, 3);
    else mt_draw(fb, &SL.hero, &hs, UI_CX, slide_hero_y(), 1);
    float vis = m_clamp(SL.v, 0, 100) / 100.0f;
    slider_draw(fb, SL_X, slide_y(), SL_W, SL_H, vis, icon, slide_hw());
    const char *note = 0;
    if (bright_auto_row()) {
        float cy = 352;
        if (SL.pressed_row || nos_t - SL.row_flash < PRESS_FLASH) ui_rrect(fb, 70, cy - 40, 326, 80, 40, C_SURF2);
        ui_text(fb, UI_F_BODY, 104, cap_base(UI_F_BODY, cy - 12), "Auto", C_PRI, UI_LEFT, 0);
        char b[24];
        if (SL.have_lux) {
            int lx = (int)(SL.lux + 0.5f);
            lx = lx >= 1000 ? lx / 100 * 100 : lx >= 100 ? lx / 10 * 10 : lx;   /* steady text */
            snprintf(b, sizeof b, "%d", lx);
            ui_label_unit(fb, 104, cap_base(UI_F_LABEL, cy + 16), b, "LUX NOW", C_DIS, UI_LEFT);
        } else ui_label(fb, 104, cap_base(UI_F_LABEL, cy + 16), "READING LIGHT", C_DIS, UI_LEFT);
        toggle_k(fb, 324, cy, SL.tog.x, 1.0f, 0);
    } else if (SL.id == I_BRIGHT) note = "NO LIGHT MODULE";
    else if (SL.id == I_VOLUME && !slide_hw()) note = "NO SPEAKER";
    else if (SL.id == I_VOLUME && !nos_prefs.sound) note = "UI SOUNDS OFF";
    else if (SL.id == I_HSTR && !slide_hw()) note = "NO MOTOR";
    else if (SL.id == I_HSTR && !nos_prefs.haptics) note = "HAPTICS OFF";
    if (note) ui_state(fb, UI_CX, 362, note, C_DIS);
}
static int in_slider(int x, int y)
{
    float sy = slide_y();
    return y > sy - 30 && y < sy + SL_H + 30 && x > SL_X - 20 && x < SL_X + SL_W + 20;
}
static float slide_val_at(int x) { return m_clamp((x - SL_X - SL_H * 0.5f) / (SL_W - SL_H) * 100.0f, slide_min(), 100); }

static int slide_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN:
        SL.pressed_row = bright_auto_row() && in->y > 310 && in->y < 400;
        return 1;
    case IN_DRAG_START:
        SL.pressed_row = 0;
        if (in->axis != AXIS_X || !in_slider(in->x0, in->y0)) return 0;
        SL.dragging = 1; SL.animating = 0;
        SL.v0 = m_clamp(SL.v, 0, 100);
        SL.v = m_clamp(SL.v0 + in->dx / (SL_W - SL_H) * 100.0f, slide_min(), 100);
        return 1;
    case IN_DRAG:
        if (!SL.dragging) return 0;
        SL.v = m_clamp(SL.v0 + in->dx / (SL_W - SL_H) * 100.0f, slide_min(), 100);
        return 1;
    case IN_DRAG_END:
        if (!SL.dragging) return 0;
        SL.dragging = 0;
        return 1;
    case IN_TAP:
        if (in_slider(in->x, in->y)) {
            /* taps land on the 10 % detents the haptics mark */
            msp_init(&SL.sp, SL.v, SPRING_SNAPPY);
            msp_to(&SL.sp, m_clamp(floorf(slide_val_at(in->x) / 10.0f + 0.5f) * 10.0f, slide_min(), 100));
            SL.animating = 1;
            nos_feedback(FB_TAP);
        } else if (SL.pressed_row) {
            int on = !nos_prefs.auto_bright;
            nos_prefs.auto_bright = (uint8_t)on;
            nos_feedback(on ? FB_TOGGLE_ON : FB_TOGGLE_OFF);
            msp_to(&SL.tog, on ? 1.0f : 0.0f);
            prefs_save();
            SL.row_flash = nos_t;
        }
        SL.pressed_row = 0;
        return 1;
    case IN_UP:
        SL.pressed_row = 0;
        return 1;
    default: return 0;
    }
}
static const nos_screen scr_set_slide = { "SET_SLIDE", slide_enter, 0, slide_tick, slide_draw, slide_input, SCR_HDRAG };

/* ======================================================================= radio page
 * One page per radio for the whole OS (Settings and Modules open it; Modules adds its
 * bus line in the footer slot). Wi-Fi: the big tile is the switch. Bluetooth is the
 * phone link and stays on: the bare icon when linked, the phone-absent standard when
 * not. Both keep one layout: icon at RADIO_ICON_Y, state at RADIO_STATE_Y. */
#define RADIO_ICON_Y  176
#define RADIO_STATE_Y 298
#define RADIO_BODY_Y  336
#define RADIO_BUS_Y   398
static struct {
    int id, mod;
    int pressed;
    mspring on, press;
    float t0, flash_t;
    char state[24], line[48], bus[40];
} R;

void set_settings_open_radio(int bt, int from_modules)
{
    R.id = bt ? I_BT : I_WIFI;
    R.mod = from_modules;
    nos_push(&scr_set_radio, TX_PUSH);
}
static int radio_lit(void) { return R.id == I_BT ? phone_linked() : nos_prefs.wifi; }
static void radio_enter(void)
{
    msp_init(&R.on, radio_lit() ? 1.0f : 0.0f, SPRING_SNAPPY);
    msp_init(&R.press, 0, SPRING_SNAPPY);
    R.pressed = 0;
    R.flash_t = -100;
    R.t0 = nos_enter_t();
}
static void radio_tick(float dt)
{
    if ((R.on.target > 0.5f) != radio_lit()) msp_to(&R.on, radio_lit() ? 1.0f : 0.0f);
    int held = R.pressed || nos_t - R.flash_t < PRESS_FLASH;
    if ((R.press.target > 0.5f) != held) msp_to(&R.press, held ? 1.0f : 0.0f);
    msp_step(&R.on, dt);
    msp_step(&R.press, dt);
    if (R.id == I_BT) {
        snprintf(R.state, sizeof R.state, "CONNECTED");
        snprintf(R.line, sizeof R.line, "Notifications, music and weather sync");
        char st[40]; hal_ble_status(st, sizeof st);
        snprintf(R.bus, sizeof R.bus, "NUS  %.30s", st);
    } else {
        hal_conn c = hal_status(HAL_FEAT_WIFI);
        int on = nos_prefs.wifi;
        snprintf(R.state, sizeof R.state, "%s", wifi_word());
        snprintf(R.line, sizeof R.line, "%s", !on ? "Weather via the phone" :
                 c == HAL_CONN_UP ? "Weather and time sync over Wi-Fi" :
                 c == HAL_CONN_TRYING ? "Looking for a network" : "No saved network in range");
        snprintf(R.bus, sizeof R.bus, "ESP32-S3  2.4 GHZ  STATION");
    }
    for (char *s = R.bus; *s; s++) *s = (char)toupper((unsigned char)*s);
}
static void radio_draw(gfx_fb *fb)
{
    ui_title(fb, R.id == I_BT ? "BLUETOOTH" : "WI-FI", 0);
    float bp = m_clamp((nos_t - R.t0) / UI_ENTRY_S, 0, 1);
    float tk = q8((bp - 0.3f) / 0.7f);               /* text follows the icon, as ui_absent */
    if (R.mod && tk > 0) ident(fb, UI_CX, RADIO_BUS_Y, R.bus, ui_dim(C_DIS, tk), UI_CENTER);
    if (R.id == I_BT) {
        if (!phone_linked()) {
            ui_absent(fb, DOT_ICON_BT, "Phone not connected", "Pair in Gadgetbridge as Bangle.js NOS", NULL, bp);
            return;
        }
        dot_icon_anim(fb, DOT_ICON_BT, UI_CX, RADIO_ICON_Y, 6.0f, 4.8f, C_WHITE, bp, DOT_REVEAL_CENTER);
        if (tk <= 0) return;
        ui_state(fb, UI_CX, RADIO_STATE_Y, R.state, ui_dim(C_PRI, tk));
        ui_text_wrap(fb, UI_F_BODY, UI_CX, RADIO_BODY_Y, 330, 30, 2, R.line, ui_dim(C_SEC, tk), UI_CENTER);
        return;
    }
    /* fill mixes on/off, then the pressed tint eases in and out on its own spring, so a
     * release fades from grey instead of flashing white */
    float on = m_clamp(R.on.x, 0, 1), pk = q8(R.press.x);
    /* one grey level, so the fade never picks up a 565 colour cast; it ends on true white */
    float lv = m_lerp(m_lerp(0x1A, 0xFF, on), m_lerp(0x2A, 0x99, on), pk);
    int g = (int)(lv + 0.5f);
    uint16_t fill = g >= 0xFC ? C_WHITE : GFX_RGB(g & 0xF8, g & 0xF8, g & 0xF8);
    uint16_t ink = ui_mix(C_PRI, C_BG, on);
    gfx_fill_circle(fb, UI_CX, RADIO_ICON_Y, 76, fill);
    dot_icon_anim(fb, DOT_ICON_WIFI, UI_CX, RADIO_ICON_Y, 6.0f, 4.8f, ink, bp, DOT_REVEAL_CENTER);
    if (tk <= 0) return;
    ui_state(fb, UI_CX, RADIO_STATE_Y, R.state, ui_dim(nos_prefs.wifi ? C_PRI : C_SEC, tk));
    ui_text_wrap(fb, UI_F_BODY, UI_CX, RADIO_BODY_Y, 330, 30, 2, R.line, ui_dim(C_SEC, tk), UI_CENTER);
}
static int radio_input(const nos_input *in)
{
    float dx = in->x - UI_CX, dy = in->y - (float)RADIO_ICON_Y;
    int hit = R.id == I_WIFI && dx * dx + dy * dy < 90.0f * 90.0f;
    switch (in->type) {
    case IN_DOWN: R.pressed = hit; return 1;
    case IN_DRAG_START: R.pressed = 0; return 0;
    case IN_TAP:
        if (hit) {
            int v = !nos_prefs.wifi;
            nos_prefs.wifi = (uint8_t)v;
            nos_feedback(v ? FB_TOGGLE_ON : FB_TOGGLE_OFF);
            msp_to(&R.on, v ? 1.0f : 0.0f);
            prefs_save();
            R.flash_t = nos_t;
        }
        R.pressed = 0;
        return 1;
    case IN_UP: R.pressed = 0; return 1;
    default: return 0;
    }
}
static const nos_screen scr_set_radio = { "SET_RADIO", radio_enter, 0, radio_tick, radio_draw, radio_input, 0 };

/* ======================================================================= set time */
#define TCOL 76.0f
#define TCY  196.0f              /* the drums sit high: the SET button owns the thumb row */
#define TDRUM_Y0 82
#define TDRUM_Y1 322
static struct { pk h, m; pk_txt ht, mt; int pressed; float flash_t; int active; float t0; } T;

static void two(int v, char *o, int n)
{
    if (v < 0 || v > 99 || n < 3) { o[0] = 0; return; }
    o[0] = (char)('0' + v / 10); o[1] = (char)('0' + v % 10); o[2] = 0;
}
static void time_texts(void)
{
    pk_texts(&T.h, &T.ht, two);
    pk_texts(&T.m, &T.mt, two);
}
static void time_enter(void)
{
    hal_datetime dt; hal_get_datetime(&dt);
    pk_init(&T.h, 24, dt.hour);
    pk_init(&T.m, 60, dt.minute);
    T.pressed = 0; T.active = -1; T.flash_t = -100;
    T.t0 = nos_enter_t();
    time_texts();
}
static void time_tick(float dt)
{
    pk_tick(&T.h, dt);
    pk_tick(&T.m, dt);
    time_texts();
}
static void time_draw(gfx_fb *fb)
{
    ui_title(fb, "SET TIME", 0);
    pk_ruler(fb, &T.h, 64, TCY);
    pk_ruler(fb, &T.m, GFX_W - 64, TCY);
    float bp = m_clamp((nos_t - T.t0) / UI_ENTRY_S, 0, 1);
    gfx_fb sub;
    if (nos_subview(fb, &sub, 0, TDRUM_Y0, GFX_W, TDRUM_Y1, 0, 0)) {
        pk_column(&sub, &T.h, &T.ht, UI_CX - TCOL, TCY, bp, 5, -9);
        pk_column(&sub, &T.m, &T.mt, UI_CX + TCOL, TCY, bp, 90, -9);
    }
    dot_style cs = { DOT_FONT_TALL, UI_HERO_PITCH, UI_HERO_PITCH * 0.8f, C_DIS, 0, 1, 1 };
    dot_text_a(fb, &cs, UI_CX, TCY - 4.5f * UI_HERO_PITCH, ":", 1);
    ui_button(fb, UI_CX, UI_BTN_Y, UI_BTN_W, UI_BTN_H, "SET", BTN_PRIMARY,
              T.pressed || nos_t - T.flash_t < PRESS_FLASH);
}
static int time_btn(int x, int y)
{
    return fabsf(y - (float)UI_BTN_Y) <= UI_BTN_H * 0.5f + 4 && fabsf(x - (float)UI_CX) <= UI_BTN_W * 0.5f + 10;
}
static int time_input(const nos_input *in)
{
    pk *p;
    switch (in->type) {
    case IN_DOWN:
        T.pressed = time_btn(in->x, in->y);
        return 1;
    case IN_DRAG_START:
        T.pressed = 0;
        if (in->axis != AXIS_Y || in->y0 > 320) return 0;
        T.active = in->x0 < UI_CX ? 0 : 1;
        p = T.active ? &T.m : &T.h;
        pk_grab(p);
        pk_drag(p, in->dy);
        return 1;
    case IN_DRAG:
        if (T.active < 0) return 0;
        pk_drag(T.active ? &T.m : &T.h, in->dy);
        return 1;
    case IN_DRAG_END:
        if (T.active < 0) return 0;
        pk_release(T.active ? &T.m : &T.h, in->vy);
        T.active = -1;
        return 1;
    case IN_TAP:
        if (T.pressed) {
            T.flash_t = nos_t;
            hal_datetime dt; hal_get_datetime(&dt);
            dt.hour = T.h.idx; dt.minute = T.m.idx; dt.second = 0; dt.millis = 0;
            hal_set_datetime(&dt);
            nos_feedback(FB_SUCCESS);
            nos_toast("TIME SET");
            nos_pop();
        } else if (in->y < TCY - 50 || (in->y > TCY + 50 && in->y < 320)) {
            if (pk_step(in->x < UI_CX ? &T.h : &T.m, in->y < TCY ? -1 : 1)) time_texts();
        }
        T.pressed = 0;
        return 1;
    case IN_UP: T.pressed = 0; return 1;
    default: return 0;
    }
}
static const nos_screen scr_set_time = { "SET_TIME", time_enter, 0, time_tick, time_draw, time_input, 0 };
