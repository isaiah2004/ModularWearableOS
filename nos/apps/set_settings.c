/* set_settings.c — Settings: a sectioned list plus its sub-pages.
 *
 * Main list: ui_list owns scrolling, rubber band, pressed row and the scrollbar fade;
 * this file lays out variable-height entries on the list's 30 px unit grid (headers
 * 2 units, rows 3 units = 90 px touch targets) and draws them itself.
 *
 * Sub-pages (all pushed with TX_PUSH):
 *   picker  — one hero value in dot matrix, dragged vertically with detents,
 *             momentum and snap; values morph dot by dot; rulers track the finger.
 *   slider  — brightness / volume / haptic strength: hero percent + thick pill.
 *   radio   — Bluetooth / Wi-Fi: one big round tile toggles, state as text.
 *   settime — two pickers (hour, minute) and SET.
 *   storage — SD capacity bar.
 *   confirm — restart / reset settings.
 * Every value is backed by nos_prefs + prefs_save(). Draw is pure: tick prepares
 * every string and animation value. */
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
#include <stdlib.h>

#define SET_VERSION "0.1.0"

/* ======================================================================= helpers */
static float qfade(float yc)
{
    float d = fabsf(yc - UI_CY);
    float f = 1.0f - 0.7f * m_smoothstep(140.0f, 230.0f, d);
    return floorf(f * 16.0f + 0.5f) / 16.0f;
}
static int cap_base(const font_t *f, float cy) { return (int)lroundf(cy + f->ascent * 0.36f); }
static float q8(float v) { return floorf(m_clamp(v, 0, 1) * 8.0f + 0.5f) / 8.0f; }

/* ui_toggle with a brightness factor (rows dim near the bezel). */
static void toggle_k(gfx_fb *fb, float cx, float cy, float on, float k)
{
    const float w = 76, h = 44, r = 16;
    on = m_clamp(on, 0, 1);
    uint16_t track = ui_dim(ui_mix(C_LINE2, C_WHITE, on), k);
    uint16_t thumb = ui_dim(ui_mix(C_DIS, C_BG, on), k);
    gfx_fill_pill(fb, cx - w / 2 + h / 2, cy, cx + w / 2 - h / 2, h / 2, track);
    gfx_fill_circle(fb, m_lerp(cx - w / 2 + h / 2, cx + w / 2 - h / 2, on), cy, r, thumb);
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

/* Little dot chevron ">" for rows that open a page. */
static void chevron(gfx_fb *fb, float rx, float cy, uint16_t col)
{
    dot_style st = { DOT_FONT_5X7, 2.0f, 1.7f, col, 0, 1, 0 };
    dot_text(fb, &st, rx - 8.0f, cy - 7.0f, ">");
}

/* Body text block with a dot icon above (empty / absent states). */
static void empty_state(gfx_fb *fb, int icon, const char *title, const char *line, float cy)
{
    dot_icon(fb, icon, UI_CX, cy - 70, 4.0f, 3.2f, C_DIS);
    ui_text(fb, UI_F_TITLE, UI_CX, (int)(cy + 4), title, C_PRI, UI_CENTER, 0);
    ui_text_wrap(fb, UI_F_BODY, UI_CX, (int)(cy + 44), 300, 30, 2, line, C_SEC, UI_CENTER);
}

/* ======================================================================= picker
 * Hero value fixed at the centre; the finger drags an invisible drum whose detents
 * are PK_STEP px apart. Rulers at both sides move 1:1 with the finger so the motion
 * is visible; the hero and its neighbours morph at every detent with a haptic tick. */
#define PK_STEP 30.0f
#define PK_RESP 0.50f
typedef struct {
    int n, idx;
    float x, x0;
    mspring sp;
    int dragging, springing, over;
} pk;

static float pk_max(const pk *p) { return (float)(p->n - 1) * PK_STEP; }
static void pk_init(pk *p, int n, int idx)
{
    memset(p, 0, sizeof *p);
    p->n = n < 1 ? 1 : n;
    p->idx = idx < 0 ? 0 : idx >= p->n ? p->n - 1 : idx;
    p->x = p->idx * PK_STEP;
    msp_init(&p->sp, p->x, PK_RESP, 1.0f);
}
static void pk_grab(pk *p) { p->dragging = 1; p->springing = 0; p->x0 = p->x; p->over = 0; }
static void pk_drag(pk *p, float dy)
{
    float raw = p->x0 - dy, mx = pk_max(p);
    int over = 0;
    if (raw < 0) { raw = rubber_band(raw, 466.0f); over = 1; }
    else if (raw > mx) { raw = mx + rubber_band(raw - mx, 466.0f); over = 1; }
    if (over && !p->over) nos_feedback(FB_LIMIT);
    p->over = over;
    p->x = raw;
}
static void pk_go(pk *p, float target, float v)
{
    float dist = target - p->x;
    float w = 6.2831853f / PK_RESP;
    /* a critically damped spring overshoots when it starts faster than w*dist toward
     * its target: cap the hand-off so the snap is a click, never a bounce */
    if (dist * v > 0 && fabsf(v) > w * fabsf(dist)) v = (v > 0 ? 1.0f : -1.0f) * w * fabsf(dist);
    if (dist * v < 0) v *= 0.3f;
    msp_init(&p->sp, p->x, PK_RESP, 1.0f);
    p->sp.v = v;
    msp_to(&p->sp, target);
    p->springing = 1;
}
static void pk_release(pk *p, float vy)
{
    p->dragging = 0;
    float v = -vy, mx = pk_max(p);
    float rest = (p->x < 0 || p->x > mx) ? p->x : decay_project(p->x, v);
    float k = floorf(m_clamp(rest, 0, mx) / PK_STEP + 0.5f);
    pk_go(p, k * PK_STEP, (p->x < 0 || p->x > mx) ? 0 : v);
}
static void pk_step(pk *p, int d)
{
    int t = p->idx + d;
    if (t < 0 || t >= p->n) { nos_feedback(FB_LIMIT); return; }
    pk_go(p, t * PK_STEP, 0);
}
/* advance; returns 1 when the detent under the hero changed */
static int pk_tick(pk *p, float dt)
{
    if (p->springing && !p->dragging) {
        if (!msp_step(&p->sp, dt)) p->springing = 0;
        p->x = p->sp.x;
    }
    int k = (int)floorf(m_clamp(p->x, 0, pk_max(p)) / PK_STEP + 0.5f);
    if (k != p->idx) { p->idx = k; nos_feedback(FB_TICK); return 1; }
    return 0;
}
/* ruler: dots for every detent, brightest at the centre line */
static void pk_ruler(gfx_fb *fb, const pk *p, float x)
{
    int i0 = (int)floorf((p->x - 150.0f) / PK_STEP), i1 = (int)ceilf((p->x + 150.0f) / PK_STEP);
    if (i0 < 0) i0 = 0;
    if (i1 > p->n - 1) i1 = p->n - 1;
    for (int i = i0; i <= i1; i++) {
        float y = UI_CY + i * PK_STEP - p->x;
        float dy = fabsf(y - UI_CY);
        if (dy > 150.0f) continue;
        float k = 1.0f - dy / 150.0f;
        k = q8(k * k);
        float d = 4.0f + 3.0f * q8(1.0f - m_clamp(dy / PK_STEP, 0, 1));
        uint16_t c = ui_mix(C_LINE, C_SEC, k);
        if (dy < PK_STEP * 0.5f) c = ui_mix(C_SEC, C_WHITE, q8(1.0f - dy / (PK_STEP * 0.5f)));
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
static void fmt_steps(int s, char *o, int n)
{
    if (s >= 1000) snprintf(o, (size_t)n, "%d,%03d", s / 1000, s % 1000);
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

/* ui_list_input releases the scroller again on the IN_UP that follows IN_DRAG_END,
 * with zero velocity, which kills every fling. Forward IN_UP only while the finger
 * still holds the list (a press that never dragged). */
static int list_in(ui_list *l, const nos_input *in)
{
    if (in->type == IN_UP && !l->sc.dragging) { l->pressed = -1; return 1; }
    return ui_list_input(l, in);
}

/* ======================================================================= main list */
enum { K_HDR = 0, K_NAV, K_TOG, K_FLIP, K_DANGER };
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
    { K_NAV, I_RESTART, "Restart" },
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
    int covered;
    float build_t0;
    mspring tog[I_COUNT];
    char val[I_COUNT][24];
    /* flip rows: previous text fades out, new one in */
    char flip_old[I_COUNT][16];
    float flip_t0[I_COUNT];
    float stor_t;
    hal_storage stor;
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

static int modules_present(int *total)
{
    uint32_t m = hal_ext_present();
    int n = 0;
    for (int i = 0; i < 9; i++) n += (m >> i) & 1;
    if (total) *total = 9;
    return n;
}

static void update_values(void)
{
    char b[24];
    int sp = nos_prefs.brightness;
    if (nos_prefs.auto_bright && (hal_ext_present() & EXT_LIGHT)) snprintf(S.val[I_BRIGHT], 24, "Auto");
    else snprintf(S.val[I_BRIGHT], 24, "%d%%", sp);
    snprintf(S.val[I_TIMEOUT], 24, "%d s", nos_prefs.sleep_s);
    snprintf(S.val[I_VOLUME], 24, "%d%%", nos_prefs.volume);
    snprintf(S.val[I_HSTR], 24, "%d%%", nos_prefs.haptic_strength);
    int phone = hal_phone_connected() || svc_sys()->phone_connected;
    snprintf(S.val[I_BT], 24, "%s", !nos_prefs.ble ? "Off" : phone ? "Connected" : "On");
    hal_conn wc = hal_status(HAL_FEAT_WIFI);
    snprintf(S.val[I_WIFI], 24, "%s", !nos_prefs.wifi ? "Off" : wc == HAL_CONN_UP ? "Connected" :
             wc == HAL_CONN_TRYING ? "Joining" : "On");
    hal_lte lte;
    if (!hal_lte_get(&lte)) snprintf(S.val[I_CELL], 24, "No module");
    else if (lte.powered && lte.registered && lte.op[0]) snprintf(S.val[I_CELL], 24, "%.12s", lte.op);
    else snprintf(S.val[I_CELL], 24, "%s", lte.powered ? "On" : "Off");
    fmt_tz(nos_prefs.tz_min, S.val[I_TZ], 24, 1);
    hal_datetime dt; hal_get_datetime(&dt);
    ui_fmt_time(S.val[I_SETTIME], 24, dt.hour, dt.minute);
    fmt_steps(nos_prefs.step_goal_100 * 100, S.val[I_GOAL], 24);
    snprintf(b, sizeof b, "%s", nos_prefs.wrist_left ? "Left" : "Right");
    memcpy(S.val[I_WRIST], b, sizeof b);
    snprintf(S.val[I_UNITS], 24, "%s", nos_prefs.units_metric ? "Metric" : "Imperial");
    int tot, n = modules_present(&tot);
    snprintf(S.val[I_MODULES], 24, "%d/%d", n, tot);
    if (S.stor.mounted) {
        double fr = (double)S.stor.free_bytes / (1024.0 * 1024.0 * 1024.0);
        if (fr >= 10) snprintf(S.val[I_STORAGE], 24, "%.0f GB free", fr);
        else snprintf(S.val[I_STORAGE], 24, "%.1f GB free", fr);
    } else snprintf(S.val[I_STORAGE], 24, "No card");
    snprintf(S.val[I_ABOUT], 24, "%s", SET_VERSION);
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

static int s_sub_frame = -1;   /* frame a settings sub-page was popped (return detection) */

static void settings_enter(void)
{
    /* Revealed by a pop (a sub-page closing) keeps scroll and skips the build-in. The
     * engine calls enter() mid-transition on a pop and before tx_begin on a push. */
    int revealed = S.inited && S.covered && nos_transitioning();
    S.covered = 0;
    if (!S.inited || !revealed) {
        list_setup();
        S.build_t0 = nos_t;
        S.inited = 1;
    }
    settings_sync_toggles();
    hal_storage_get(&S.stor);
    S.stor_t = nos_t;
    for (int i = 0; i < I_COUNT; i++) S.flip_t0[i] = -100.0f;
    update_values();
    (void)s_sub_frame;
}
static void settings_leave(void) { S.covered = 1; }

static void settings_tick(float dt)
{
    ui_list_tick(&S.L, dt);
    for (int i = 0; i < I_COUNT; i++) msp_step(&S.tog[i], dt);
    if (nos_t - S.stor_t > 2.0f) { hal_storage_get(&S.stor); S.stor_t = nos_t; }
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

/* build-in: rows rise 14 px and fade in, one frame apart from the top down */
static float build_k(float y, float *dy)
{
    float t = nos_t - S.build_t0;
    int k = (int)((y - 60.0f) / 80.0f);
    if (k < 0) k = 0;
    if (k > 6) k = 6;
    float p = m_clamp((t - k * 0.034f) / 0.20f, 0, 1);
    float e = bezier_ease(&EASE_STANDARD, p);
    e = floorf(e * 8.0f + 0.5f) / 8.0f;
    *dy = (1.0f - e) * 14.0f;
    return e;
}

static int label_w(const char *s)
{
    char b[32]; int i = 0;
    for (; s[i] && i < 31; i++) b[i] = (char)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]);
    b[i] = 0;
    return ui_text_width(UI_F_LABEL, b, 2);
}

static void draw_entry(gfx_fb *fb, int e, float y, float h, int pressed)
{
    const sentry *en = &E[e];
    float bdy, bk = build_k(y, &bdy);
    if (bk <= 0) return;
    y += floorf(bdy + 0.5f);
    float cy = y + h * 0.5f;
    float k = qfade(cy) * bk;
    /* inset to the circle at the text line nearest the bezel */
    float edge = cy < UI_CY ? cy - 14 : cy + 14;
    int x0, x1;
    ui_row_span(edge, 30, &x0, &x1);
    if (x1 - x0 < 160) return;
    int lx = x0 > 62 ? x0 : 62, rx = x1 < 404 ? x1 : 404;

    if (en->kind == K_HDR) {
        ui_label(fb, lx, (int)(y + h - 16), en->label, ui_dim(C_DIS, k), UI_LEFT);
        return;
    }
    if (en->kind == K_DANGER) {
        ui_button(fb, UI_CX, cy, 260, 68, en->label, BTN_DANGER, pressed);
        return;
    }
    if (pressed) ui_rrect(fb, lx - 20, y + 5, rx - lx + 40, h - 10, (h - 10) * 0.5f, C_SURF2);
    int bl = cap_base(UI_F_BODY, cy);
    ui_text(fb, UI_F_BODY, lx, bl, en->label, ui_dim(C_PRI, k), UI_LEFT, 0);
    int lb = cap_base(UI_F_LABEL, cy);
    int lend = lx + ui_text_width(UI_F_BODY, en->label, 0) + 16;   /* values never touch the label */
    switch (en->kind) {
    case K_TOG:
        if (rx - 76 > lend - 8) toggle_k(fb, rx - 38, cy, S.tog[en->id].x, k);
        break;
    case K_NAV:
        chevron(fb, rx, cy, ui_dim(C_DIS, k));
        if (S.val[en->id][0] && rx - 20 - label_w(S.val[en->id]) > lend)
            ui_label(fb, rx - 20, lb, S.val[en->id], ui_dim(C_SEC, k), UI_RIGHT);
        break;
    case K_FLIP: {
        if (rx - label_w(S.val[en->id]) < lend) break;
        float p = (nos_t - S.flip_t0[en->id]) / 0.24f;
        if (p >= 1) ui_label(fb, rx, lb, S.val[en->id], ui_dim(C_SEC, k), UI_RIGHT);
        else if (p < 0.5f) {
            float a = q8(1.0f - p * 2.0f);
            ui_label(fb, rx, lb - (int)((1 - a) * 8), S.flip_old[en->id], ui_dim(C_SEC, k * a), UI_RIGHT);
        } else {
            float a = q8(p * 2.0f - 1.0f);
            ui_label(fb, rx, lb + (int)((1 - a) * 8), S.val[en->id], ui_dim(C_SEC, k * a), UI_RIGHT);
        }
        break;
    }
    }
}

static void settings_draw(gfx_fb *fb)
{
    float off = list_off();
    ui_title(fb, "SETTINGS", off);
    /* the highlight waits two frames so a press that turns into a scroll never flashes */
    int pe = S.L.pressed >= 0 && S.L.pressed < S.nunits && nos_t - S.L.press_t > 0.05f ?
             S.unit_e[S.L.pressed] : -1;
    for (int e = 0; e < NE; e++) {
        float h = (float)(units_of(E[e].kind) * UNIT);
        float y = LTOP + S.start[e] * UNIT - off;
        if (y + h < 0) continue;
        if (y > GFX_H) break;
        draw_entry(fb, e, y, h, e == pe);
    }
    float content = (float)(LTOP + S.nunits * UNIT + S.L.bottom_pad);
    ui_scrollbar(fb, S.L.sc.x, content, GFX_H, S.L.bar_alpha);
}

/* ---- sub-page declarations ---- */
static const nos_screen scr_set_pick, scr_set_slide, scr_set_radio, scr_set_time,
                        scr_set_storage, scr_set_confirm;
static void open_picker(int id);
static void open_slider(int id);
static void open_radio(int id);
static void open_confirm(int id);

static void flip(int id)
{
    memcpy(S.flip_old[id], S.val[id], sizeof S.flip_old[id] - 1);
    S.flip_old[id][sizeof S.flip_old[id] - 1] = 0;
    if (id == I_WRIST) nos_prefs.wrist_left = !nos_prefs.wrist_left;
    else nos_prefs.units_metric = !nos_prefs.units_metric;
    prefs_save();
    S.flip_t0[id] = nos_t;
    update_values();
    nos_feedback(FB_TAP);
}

static void activate(int e)
{
    const sentry *en = &E[e];
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
        return;
    }
    case K_FLIP: flip(en->id); return;
    case K_DANGER: nos_feedback(FB_TAP); open_confirm(en->id); return;
    default: break;
    }
    nos_feedback(FB_TAP);
    switch (en->id) {
    case I_BRIGHT: case I_VOLUME: case I_HSTR: open_slider(en->id); break;
    case I_TIMEOUT: case I_TZ: case I_GOAL: open_picker(en->id); break;
    case I_BT: case I_WIFI: open_radio(en->id); break;
    case I_FACE: nos_push(&scr_faces, TX_PUSH); break;
    case I_CELL: nos_push(&scr_cell, TX_PUSH); break;
    case I_SETTIME: nos_push(&scr_set_time, TX_PUSH); break;
    case I_MODULES: nos_push(&scr_modules, TX_PUSH); break;
    case I_STORAGE: nos_push(&scr_set_storage, TX_PUSH); break;
    case I_ABOUT: nos_push(&scr_about, TX_PUSH); break;
    case I_RESTART: open_confirm(I_RESTART); break;
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
            if (e >= 0) activate(e);
        }
        return 1;
    }
    return list_in(&S.L, in);
}

const nos_screen scr_settings = { "SETTINGS", settings_enter, settings_leave, settings_tick,
                                  settings_draw, settings_input, 0 };

/* ======================================================================= picker page */
static struct {
    int id;
    pk p;
    mtext hero, prev, next, extra;
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
static void pick_fmt(int id, int i, char *o, int n)
{
    if (i < 0 || i >= pick_n(id)) { o[0] = 0; return; }
    if (id == I_TIMEOUT) snprintf(o, (size_t)n, "%d", k_timeout[i]);
    else if (id == I_TZ) fmt_tz(k_tz[i], o, n, 0);
    else snprintf(o, (size_t)n, "%d", 2000 + i * 500);
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
        if (nos_prefs.units_metric) snprintf(o, (size_t)n, "%.1f KM", km);
        else snprintf(o, (size_t)n, "%.1f MI", km * 0.6214f);
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
static void pick_texts(int init)
{
    char a[24], b[24], c[24], d[24];
    pick_fmt(P.id, P.p.idx, a, sizeof a);
    pick_fmt(P.id, P.p.idx - 1, b, sizeof b);
    pick_fmt(P.id, P.p.idx + 1, c, sizeof c);
    pick_extra(P.id, P.p.idx, d, sizeof d);
    if (init) { mt_init(&P.hero, a, 0.18f); mt_init(&P.prev, b, 0.18f); mt_init(&P.next, c, 0.18f); mt_init(&P.extra, d, 0.22f); }
    else { mt_set(&P.hero, a); mt_set(&P.prev, b); mt_set(&P.next, c); mt_set(&P.extra, d); }
}

static void open_picker(int id) { P.id = id; nos_push(&scr_set_pick, TX_PUSH); }

static void pick_enter(void)
{
    int idx = P.id == I_TIMEOUT ? timeout_index(nos_prefs.sleep_s) :
              P.id == I_TZ ? tz_index(nos_prefs.tz_min) :
              (int)m_clamp((nos_prefs.step_goal_100 * 100 - 2000) / 500.0f + 0.5f, 0, 56);
    pk_init(&P.p, pick_n(P.id), idx);
    P.tz0 = nos_prefs.tz_min;
    if (P.id == I_TZ) P.tz0 = k_tz[idx];
    P.pressed = -1;
    P.t0 = nos_t;
    pick_texts(1);
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
    pick_texts(0);
}

static float hero_pitch(const char *s) { return strlen(s) <= 3 ? 9.0f : 8.0f; }

static void pick_draw(gfx_fb *fb)
{
    ui_title(fb, pick_title(P.id), 0);
    float bp = m_clamp((nos_t - P.t0) / 0.30f, 0, 1);
    pk_ruler(fb, &P.p, 70);
    pk_ruler(fb, &P.p, GFX_W - 70);
    float pitch = hero_pitch(P.hero.to);
    dot_style hs = { DOT_FONT_TALL, pitch, pitch * 0.8f, C_WHITE, 0, 1, 1 };
    float hy = UI_CY - 4.5f * pitch;
    if (bp < 1) dot_text_anim(fb, &hs, UI_CX, hy, P.hero.to, 1, bp, DOT_REVEAL_RANDOM, 7);
    else mt_draw(fb, &P.hero, &hs, UI_CX, hy, 1);
    dot_style ns = { DOT_FONT_5X7, 3.0f, 2.4f, C_DIS, 0, 1, 1 };
    dot_style nsp = ns; nsp.color = C_PRI;
    mt_draw(fb, &P.prev, P.pressed == 0 ? &nsp : &ns, UI_CX, 128, 1);
    mt_draw(fb, &P.next, P.pressed == 1 ? &nsp : &ns, UI_CX, 318, 1);
    ui_label(fb, UI_CX, 376, pick_unit(P.id), C_SEC, UI_CENTER);
    if (P.extra.to[0]) {
        dot_style es = { DOT_FONT_5X7, 2.6f, 2.1f, C_PRI, 0, 1, 1 };
        mt_draw(fb, &P.extra, &es, UI_CX, 394, 1);
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
        if (P.pressed == 0) pk_step(&P.p, -1);
        else if (P.pressed == 1) pk_step(&P.p, 1);
        else pk_go(&P.p, P.p.idx * PK_STEP, 0);
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

/* ======================================================================= slider page */
#define SL_X 73.0f
#define SL_W 320.0f
#define SL_H 72.0f
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
    int pressed_row;
    float lux; int have_lux;
    float t0;
} SL;

static uint8_t *slide_pref(int id)
{
    return id == I_BRIGHT ? &nos_prefs.brightness : id == I_VOLUME ? &nos_prefs.volume : &nos_prefs.haptic_strength;
}
static float slide_y(void) { return SL.id == I_BRIGHT ? 205.0f : 244.0f; }
static float slide_hero_y(void) { return SL.id == I_BRIGHT ? 102.0f : 136.0f; }

/* Thick pill with the icon riding the white fill and a dot at every 10 % detent. */
static void slider_draw(gfx_fb *fb, float x, float y, float w, float h, float v, int icon)
{
    float r = h * 0.5f, cy = y + r;
    gfx_fill_pill(fb, x + r, cy, x + w - r, r, C_SURF2);
    float fx = x + h + (w - h) * v;              /* fill's right end */
    for (int i = 1; i < 10; i++) {
        float dx = x + r + (w - h) * i / 10.0f;
        if (dx > fx - r + 6) dot_disc(fb, dx, cy, 5, C_LINE2);
    }
    gfx_fill_pill(fb, x + r, cy, fx - r, r, C_WHITE);
    dot_icon(fb, icon, x + r + 2, cy, 2.6f, 2.1f, C_BG);
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
    if ((v == 0 || v == 100) && v != old) nos_feedback(FB_LIMIT);
}

static void slide_enter(void)
{
    SL.v = *slide_pref(SL.id);
    SL.shown = (int)SL.v;
    SL.dragging = SL.animating = 0;
    SL.pressed_row = 0;
    char b[8]; snprintf(b, sizeof b, "%d%%", SL.shown);
    mt_init(&SL.hero, b, 0.14f);
    msp_init(&SL.tog, nos_prefs.auto_bright ? 1.0f : 0.0f, SPRING_SNAPPY);
    SL.t0 = nos_t;
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
    float bp = m_clamp((nos_t - SL.t0) / 0.30f, 0, 1);
    dot_style hs = { DOT_FONT_TALL, 8.0f, 6.4f, C_WHITE, 0, 1, 1 };
    if (bp < 1) dot_text_anim(fb, &hs, UI_CX, slide_hero_y(), SL.hero.to, 1, bp, DOT_REVEAL_RANDOM, 3);
    else mt_draw(fb, &SL.hero, &hs, UI_CX, slide_hero_y(), 1);
    float vis = m_clamp(SL.v, 0, 100) / 100.0f;
    slider_draw(fb, SL_X, slide_y(), SL_W, SL_H, vis, icon);
    if (SL.id == I_BRIGHT) {
        float cy = 352;
        if (SL.pressed_row) ui_rrect(fb, 70, cy - 40, 326, 80, 40, C_SURF2);
        int present = (hal_ext_present() & EXT_LIGHT) != 0;
        ui_text(fb, UI_F_BODY, 104, cap_base(UI_F_BODY, cy - 12), "Auto", present ? C_PRI : C_DIS, UI_LEFT, 0);
        char b[24];
        if (!present) snprintf(b, sizeof b, "No light sensor");
        else if (SL.have_lux) snprintf(b, sizeof b, "%d lux now", (int)(SL.lux + 0.5f));
        else snprintf(b, sizeof b, "Reading light");
        ui_label(fb, 104, cap_base(UI_F_LABEL, cy + 16), b, C_DIS, UI_LEFT);
        toggle_k(fb, 324, cy, present ? SL.tog.x : 0, present ? 1.0f : 0.5f);
    } else {
        const char *note = 0;
        if (SL.id == I_VOLUME && !(hal_ext_present() & EXT_SPEAKER)) note = "NO SPEAKER";
        else if (SL.id == I_VOLUME && !nos_prefs.sound) note = "UI SOUNDS OFF";
        else if (SL.id == I_HSTR && !(hal_ext_present() & EXT_HAPTIC)) note = "NO MOTOR";
        else if (SL.id == I_HSTR && !nos_prefs.haptics) note = "HAPTICS OFF";
        if (note) ui_state(fb, UI_CX, 368, note, C_DIS);
    }
}
static int in_slider(int x, int y)
{
    float sy = slide_y();
    return y > sy - 30 && y < sy + SL_H + 30 && x > SL_X - 20 && x < SL_X + SL_W + 20;
}
static float slide_val_at(int x) { return m_clamp((x - SL_X - SL_H * 0.5f) / (SL_W - SL_H) * 100.0f, 0, 100); }

static int slide_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN:
        SL.pressed_row = SL.id == I_BRIGHT && in->y > 310 && in->y < 400;
        return 1;
    case IN_DRAG_START:
        SL.pressed_row = 0;
        if (in->axis != AXIS_X || !in_slider(in->x0, in->y0)) return 0;
        SL.dragging = 1; SL.animating = 0;
        SL.v0 = m_clamp(SL.v, 0, 100);
        SL.v = m_clamp(SL.v0 + in->dx / (SL_W - SL_H) * 100.0f, 0, 100);
        return 1;
    case IN_DRAG:
        if (!SL.dragging) return 0;
        SL.v = m_clamp(SL.v0 + in->dx / (SL_W - SL_H) * 100.0f, 0, 100);
        return 1;
    case IN_DRAG_END:
        if (!SL.dragging) return 0;
        SL.dragging = 0;
        return 1;
    case IN_TAP:
        if (in_slider(in->x, in->y)) {
            msp_init(&SL.sp, SL.v, SPRING_SNAPPY);
            msp_to(&SL.sp, floorf(slide_val_at(in->x) / 5.0f + 0.5f) * 5.0f);
            SL.animating = 1;
            nos_feedback(FB_TAP);
        } else if (SL.pressed_row) {
            if (!(hal_ext_present() & EXT_LIGHT)) { nos_feedback(FB_ERROR); nos_toast("No light sensor"); }
            else {
                int on = !nos_prefs.auto_bright;
                nos_prefs.auto_bright = (uint8_t)on;
                nos_feedback(on ? FB_TOGGLE_ON : FB_TOGGLE_OFF);
                msp_to(&SL.tog, on ? 1.0f : 0.0f);
                prefs_save();
            }
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

/* ======================================================================= radio page */
static struct { int id; int pressed; mspring on; float t0; char state[24]; char line[48]; } R;

static void open_radio(int id) { R.id = id; nos_push(&scr_set_radio, TX_PUSH); }
static uint8_t *radio_pref(void) { return R.id == I_BT ? &nos_prefs.ble : &nos_prefs.wifi; }
static void radio_enter(void)
{
    msp_init(&R.on, *radio_pref() ? 1.0f : 0.0f, SPRING_SNAPPY);
    R.pressed = 0;
    R.t0 = nos_t;
}
static void radio_tick(float dt)
{
    msp_step(&R.on, dt);
    int on = *radio_pref();
    if (R.id == I_BT) {
        int phone = hal_phone_connected() || svc_sys()->phone_connected;
        snprintf(R.state, sizeof R.state, "%s", !on ? "OFF" : phone ? "CONNECTED" : "WAITING FOR PHONE");
        snprintf(R.line, sizeof R.line, "%s", !on ? "Phone sync is paused" :
                 phone ? "Notifications and music sync" : "Pair in Gadgetbridge as NOS");
    } else {
        hal_conn c = hal_status(HAL_FEAT_WIFI);
        snprintf(R.state, sizeof R.state, "%s", !on ? "OFF" : c == HAL_CONN_UP ? "CONNECTED" :
                 c == HAL_CONN_TRYING ? "JOINING" : "NO NETWORK");
        snprintf(R.line, sizeof R.line, "%s", !on ? "Weather falls back to the phone" :
                 c == HAL_CONN_UP ? "Weather and time sync over Wi-Fi" : "Searching for a saved network");
    }
}
static void radio_draw(gfx_fb *fb)
{
    ui_title(fb, R.id == I_BT ? "BLUETOOTH" : "WI-FI", 0);
    float on = R.on.x;
    float bp = m_clamp((nos_t - R.t0) / 0.30f, 0, 1);
    uint16_t fill = ui_mix(R.pressed ? C_SURF3 : C_SURF2, R.pressed ? C_SEC : C_WHITE, on);
    uint16_t ink = ui_mix(C_PRI, C_BG, on);
    float cy = 180;
    gfx_fill_circle(fb, UI_CX, cy, 76, fill);
    dot_icon_anim(fb, R.id == I_BT ? DOT_ICON_BT : DOT_ICON_WIFI, UI_CX, cy, 6.0f, 4.8f, ink, bp, DOT_REVEAL_CENTER);
    ui_state(fb, UI_CX, 300, R.state, *radio_pref() ? C_PRI : C_DIS);
    ui_text_wrap(fb, UI_F_BODY, UI_CX, 344, 300, 30, 2, R.line, C_SEC, UI_CENTER);
}
static int radio_input(const nos_input *in)
{
    float dx = in->x - UI_CX, dy = in->y - 180.0f;
    int hit = dx * dx + dy * dy < 90.0f * 90.0f;
    switch (in->type) {
    case IN_DOWN: R.pressed = hit; return 1;
    case IN_DRAG_START: R.pressed = 0; return 0;
    case IN_TAP:
        if (hit) {
            uint8_t *p = radio_pref();
            int v = !*p;
            *p = (uint8_t)v;
            nos_feedback(v ? FB_TOGGLE_ON : FB_TOGGLE_OFF);
            msp_to(&R.on, v ? 1.0f : 0.0f);
            prefs_save();
        }
        R.pressed = 0;
        return 1;
    case IN_UP: R.pressed = 0; return 1;
    default: return 0;
    }
}
static const nos_screen scr_set_radio = { "SET_RADIO", radio_enter, 0, radio_tick, radio_draw, radio_input, 0 };

/* ======================================================================= set time */
static struct { pk h, m; mtext hh, mm, hp, hn, mp, mn; int pressed; int active; float t0; } T;

static void two(int v, char *o)
{
    if (v < 0 || v > 99) { o[0] = 0; return; }
    o[0] = (char)('0' + v / 10); o[1] = (char)('0' + v % 10); o[2] = 0;
}
static void time_texts(int init)
{
    char a[4], b[4], c[4], d[4], e[4], f[4];
    two(T.h.idx, a); two(T.m.idx, b);
    two(T.h.idx > 0 ? T.h.idx - 1 : -1, c); two(T.h.idx < 23 ? T.h.idx + 1 : -1, d);
    two(T.m.idx > 0 ? T.m.idx - 1 : -1, e); two(T.m.idx < 59 ? T.m.idx + 1 : -1, f);
    if (init) {
        mt_init(&T.hh, a, 0.18f); mt_init(&T.mm, b, 0.18f); mt_init(&T.hp, c, 0.18f);
        mt_init(&T.hn, d, 0.18f); mt_init(&T.mp, e, 0.18f); mt_init(&T.mn, f, 0.18f);
    } else {
        mt_set(&T.hh, a); mt_set(&T.mm, b); mt_set(&T.hp, c); mt_set(&T.hn, d); mt_set(&T.mp, e); mt_set(&T.mn, f);
    }
}
static void time_enter(void)
{
    hal_datetime dt; hal_get_datetime(&dt);
    pk_init(&T.h, 24, dt.hour);
    pk_init(&T.m, 60, dt.minute);
    T.pressed = 0; T.active = -1;
    T.t0 = nos_t;
    time_texts(1);
}
static void time_tick(float dt)
{
    pk_tick(&T.h, dt);
    pk_tick(&T.m, dt);
    time_texts(0);
}
#define TCOL 76.0f
static void time_draw(gfx_fb *fb)
{
    ui_title(fb, "SET TIME", 0);
    pk_ruler(fb, &T.h, 64);
    pk_ruler(fb, &T.m, GFX_W - 64);
    float bp = m_clamp((nos_t - T.t0) / 0.30f, 0, 1);
    dot_style hs = { DOT_FONT_TALL, 8.0f, 6.4f, C_WHITE, 0, 1, 1 };
    float hy = UI_CY - 36;
    if (bp < 1) {
        dot_text_anim(fb, &hs, UI_CX - TCOL, hy, T.hh.to, 1, bp, DOT_REVEAL_RANDOM, 5);
        dot_text_anim(fb, &hs, UI_CX + TCOL, hy, T.mm.to, 1, bp, DOT_REVEAL_RANDOM, 9);
    } else {
        mt_draw(fb, &T.hh, &hs, UI_CX - TCOL, hy, 1);
        mt_draw(fb, &T.mm, &hs, UI_CX + TCOL, hy, 1);
    }
    dot_style cs = hs; cs.color = C_DIS;
    dot_text_a(fb, &cs, UI_CX, hy, ":", 1);
    dot_style ns = { DOT_FONT_5X7, 3.0f, 2.4f, C_DIS, 0, 1, 1 };
    mt_draw(fb, &T.hp, &ns, UI_CX - TCOL, 132, 1);
    mt_draw(fb, &T.hn, &ns, UI_CX - TCOL, 314, 1);
    mt_draw(fb, &T.mp, &ns, UI_CX + TCOL, 132, 1);
    mt_draw(fb, &T.mn, &ns, UI_CX + TCOL, 314, 1);
    ui_button(fb, UI_CX, 386, 156, 64, "SET", BTN_PRIMARY, T.pressed);
}
static int time_btn(int x, int y) { return y > 350 && y < 430 && x > UI_CX - 100 && x < UI_CX + 100; }
static int time_input(const nos_input *in)
{
    pk *p;
    switch (in->type) {
    case IN_DOWN:
        T.pressed = time_btn(in->x, in->y);
        return 1;
    case IN_DRAG_START:
        T.pressed = 0;
        if (in->axis != AXIS_Y) return 0;
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
            hal_datetime dt; hal_get_datetime(&dt);
            dt.hour = T.h.idx; dt.minute = T.m.idx; dt.second = 0; dt.millis = 0;
            hal_set_datetime(&dt);
            nos_feedback(FB_SUCCESS);
            nos_toast("Time set");
            nos_pop();
        } else if (in->y < UI_CY - 50 || (in->y > UI_CY + 50 && in->y < 350)) {
            pk_step(in->x < UI_CX ? &T.h : &T.m, in->y < UI_CY ? -1 : 1);
        }
        T.pressed = 0;
        return 1;
    case IN_UP: T.pressed = 0; return 1;
    default: return 0;
    }
}
static const nos_screen scr_set_time = { "SET_TIME", time_enter, 0, time_tick, time_draw, time_input, 0 };

/* ======================================================================= storage */
static struct { hal_storage s; float t; float t0; mtext hero; } ST;
static void storage_texts(int init)
{
    char b[16];
    double fr = ST.s.mounted ? (double)ST.s.free_bytes / (1024.0 * 1024.0 * 1024.0) : 0;
    snprintf(b, sizeof b, fr >= 100 ? "%.0f" : "%.1f", fr);
    if (init) mt_init(&ST.hero, b, 0.2f); else mt_set(&ST.hero, b);
}
static void storage_enter(void) { hal_storage_get(&ST.s); ST.t = ST.t0 = nos_t; storage_texts(1); }
static void storage_tick(float dt)
{
    (void)dt;
    if (nos_t - ST.t > 1.0f) { hal_storage_get(&ST.s); ST.t = nos_t; storage_texts(0); }
}
static void storage_draw(gfx_fb *fb)
{
    ui_title(fb, "STORAGE", 0);
    if (!ST.s.mounted) {
        empty_state(fb, DOT_ICON_SD, "No card", "Insert a microSD card for voice memos and logs", UI_CY);
        return;
    }
    float bp = m_clamp((nos_t - ST.t0) / 0.30f, 0, 1);
    dot_style hs = { DOT_FONT_TALL, 8.0f, 6.4f, C_WHITE, 0, 1, 1 };
    if (bp < 1) dot_text_anim(fb, &hs, UI_CX, 120, ST.hero.to, 1, bp, DOT_REVEAL_RANDOM, 11);
    else mt_draw(fb, &ST.hero, &hs, UI_CX, 120, 1);
    double tot = (double)ST.s.total_bytes / (1024.0 * 1024.0 * 1024.0);
    char b[40];
    snprintf(b, sizeof b, "GB FREE OF %.0f", tot);
    ui_label(fb, UI_CX, 230, b, C_SEC, UI_CENTER);
    float used = ST.s.total_bytes ? (float)(1.0 - (double)ST.s.free_bytes / (double)ST.s.total_bytes) : 0;
    ui_segments(fb, 93, 262, 280, 28, 20, used * 20.0f * bp, C_WHITE, C_LINE);
    int pct = (int)(used * 100 + 0.5f);
    snprintf(b, sizeof b, "%d%% USED", pct);
    ui_label(fb, 93, 324, b, C_DIS, UI_LEFT);
    ui_label(fb, 373, 324, "MICROSD", C_DIS, UI_RIGHT);
    if (ST.s.message[0]) ui_text_fit(fb, UI_F_BODY, UI_CX, 372, ST.s.message, C_SEC, UI_CENTER, 280);
}
static const nos_screen scr_set_storage = { "SET_STORAGE", storage_enter, 0, storage_tick, storage_draw, 0, 0 };

/* ======================================================================= confirm */
static struct { int id; int pressed; float t0; } CF;
static void open_confirm(int id) { CF.id = id; nos_push(&scr_set_confirm, TX_PUSH); }
static void confirm_enter(void) { CF.pressed = 0; CF.t0 = nos_t; }
/* power symbol (restart) or a counter-clockwise arrow (reset), built from dots that
 * light one by one around the ring */
static void confirm_icon(gfx_fb *fb, int reset, float cx, float cy, float p)
{
    const int n = 16;
    const float r = 26, d = 6;
    float lit = p * n;
    if (!reset) {
        dot_ring(fb, cx, cy, r, n, d, 2, 2 + m_clamp(lit, 0, 13), C_PRI, 0);
        for (int i = 0; i < 3; i++) {
            float q = m_clamp(lit - 4 - i * 2, 0, 1);
            if (q > 0) dot_disc(fb, cx, cy - r - 2 + i * 10.0f, d * q, C_PRI);
        }
    } else {
        /* arc clockwise from 12 o'clock to 9 o'clock; a "<" head at 12 points back
         * along it: the counter-clockwise "undo" arrow */
        dot_ring(fb, cx, cy, r, n, d, 1, 1 + m_clamp(lit, 0, 12), C_PRI, 0);
        float q = m_clamp(lit - 10, 0, 1);
        if (q > 0) {
            dot_disc(fb, cx - 9, cy - r, d * q, C_PRI);
            dot_disc(fb, cx - 1, cy - r - 8, d * q, C_PRI);
            dot_disc(fb, cx - 1, cy - r + 8, d * q, C_PRI);
        }
    }
}
static void confirm_draw(gfx_fb *fb)
{
    int reset = CF.id == I_RESET;
    ui_title(fb, reset ? "RESET" : "RESTART", 0);
    float bp = m_clamp((nos_t - CF.t0) / 0.30f, 0, 1);
    confirm_icon(fb, reset, UI_CX, 146, bp);
    ui_text(fb, UI_F_TITLE, UI_CX, 236, reset ? "Reset settings?" : "Restart NOS?", C_PRI, UI_CENTER, 0);
    ui_text_wrap(fb, UI_F_BODY, UI_CX, 276, 300, 30, 2,
                 reset ? "Every preference returns to its default." :
                         "Alarms and timers keep running.",
                 C_SEC, UI_CENTER);
    ui_button(fb, UI_CX, 372, 200, 68, reset ? "RESET" : "RESTART", reset ? BTN_RED : BTN_PRIMARY, CF.pressed);
}
static int confirm_btn(int x, int y) { return y > 330 && y < 420 && x > UI_CX - 120 && x < UI_CX + 120; }
static int confirm_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN: CF.pressed = confirm_btn(in->x, in->y); return 1;
    case IN_DRAG_START: CF.pressed = 0; return 0;
    case IN_TAP:
        if (CF.pressed) {
            CF.pressed = 0;
            if (CF.id == I_RESET) {
                prefs_defaults();
                prefs_save();
                hal_store_save("nosprefs", &nos_prefs, sizeof nos_prefs);
                settings_sync_toggles();
                hal_set_backlight((uint8_t)(nos_prefs.brightness * 255 / 100));
                hal_audio_volume(nos_prefs.volume);
                hal_haptic_set_strength(nos_prefs.haptic_strength);
                nos_feedback(FB_SUCCESS);
                nos_toast("Settings reset");
                nos_pop();
            } else {
                hal_store_save("nosprefs", &nos_prefs, sizeof nos_prefs);
                nos_feedback(FB_OPEN);
                S.inited = 0;
                /* soft restart: the UI stack boots again (services keep running) */
                nos_set_root(&scr_home);
                nos_push(&scr_boot, TX_NONE);
            }
            return 1;
        }
        return 1;
    case IN_UP: CF.pressed = 0; return 1;
    default: return 0;
    }
}
static const nos_screen scr_set_confirm = { "SET_CONFIRM", confirm_enter, 0, 0, confirm_draw, confirm_input, 0 };
