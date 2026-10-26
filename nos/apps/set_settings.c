/* set_settings.c — Settings: a sectioned list plus its sub-pages.
 *
 * Main list: ui_list owns scrolling, rubber band, pressed row and the scrollbar fade;
 * this file lays out variable-height entries on the list's 30 px unit grid (headers
 * 2 units, rows 3 units = UI_ROW_H) and draws them itself. Rows keep the standard
 * fixed column (UI_ROW_X0..X1, values at UI_VALUE_X); the glass clips it without
 * changing row geometry. A value that can't keep ROW_GAP to its label in the
 * standard column sits under the label instead, and a
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
 * Draw is pure: tick prepares every string and animation value.
 *
 * Motion (docs/MOTION30.html): everything counts whole frames. Toggles, arm pills and
 * value swaps are micro moves on EASE_STEADY frame tables (<= 13 px a frame, no creep);
 * the drums follow the finger 1:1, glide to a stop at constant deceleration after a
 * fling, click into a detent in 5 frames after a slow release, and a tap on a neighbour
 * steps the drum with a mechanical cut (m_cut); the slider's tap glides its fill on
 * EASE_STEADY while the hero morphs once to the new value. */
#include "nos.h"
#include "diag_log.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include "faces/face.h"
#include "wake_service.h"
#include "sprite.h"
#include "psram.h"
#include "settings_shutter.h"
#include "render_layers.h"
#include "apps/native_tile.h"
#include "apps/scroll_diag.h"
#include "native_menu_assets.h"
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
/* exported to Modules: the radio pages (Modules adds its bus line) */
void set_settings_open_radio(int bt, int from_modules);

#define PRESS_FLASH 0.10f        /* a quick tap still shows its pressed state 3 frames */
#define PRESS_HOLD  0.05f        /* a held row shows its pill after 50 ms...   */
#define PRESS_TRAVEL 8           /* ...and drops it once the finger has moved 8 px (a scroll) */
#define ARM_S       3.0f         /* arm-then-tap-again window */
#define ROW_GAP     24           /* least gap between a row's label and its value */
#define VAL_F       6            /* a changed value: frames for the whole swap...      */
#define VAL_OUT_F   2            /* ...the old one lifts and fades over the first 2 (35 %),
                                    the new one rises in over the other 4 */
#define VAL_LIFT    8.0f         /* px the values travel */

/* ======================================================================= helpers */
/* EASE_STEADY as frame tables (R3): peak step <= 1.56x the average, no creeping tail */
static const mframes MF_STEADY4 = { 4, { 38, 70, 92, 100 } };
static const mframes MF_STEADY5 = { 5, { 31, 58, 80, 95, 100 } };
static const mframes MF_STEADY6 = { 6, { 26, 50, 70, 86, 97, 100 } };
static const mframes MF_POP2 = { 2, { 60, 100 } };         /* pressed states: 2 frames */
/* retarget a frame-table tween from where it is now (a toggle tapped mid-move) */
static void tw_to(mtween *tw, float to, const mframes *tab, int lead)
{
    if (tw->to == to) return;
    mtw_start_tab(tw, mtw_value(tw), to, tab);
    if (lead) mtw_lead(tw);                  /* from input: this frame already moves */
}
static void tw_set(mtween *tw, float v) { mtw_start_tab(tw, v, v, &MF_POP2); tw->t0 = -100.0f; }
static float qfade(float yc)
{
    float d = fabsf(yc - UI_CY);
    float f = 1.0f - 0.7f * m_smoothstep(140.0f, 230.0f, d);
    return floorf(f * 64.0f + 0.5f) / 64.0f;
}
static int cap_base(const font_t *f, float cy) { return (int)lroundf(cy + f->ascent * 0.36f); }
static float q8(float v) { return floorf(m_clamp(v, 0, 1) * 8.0f + 0.5f) / 8.0f; }

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
typedef struct { char from[24], to[24]; nos_time_t t0; float dur; } mtext;
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
    float p = m_frame_k(m->t0) / (m->dur * M_FPS);
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

/* The two labels of an arm tween: the outgoing one fades over its first frame (100, 38 %),
 * the incoming one over the next 3 (19, 56, 100 %), perceptual ink steps (R4, R9). */
static void arm_inks(const mtween *tw, float *plain, float *armed)
{
    int k = m_frame_k(tw->t0);
    float out = k >= 2 ? 0.0f : m_ink(1.0f - k * 0.5f), in = k < 2 ? 0.0f : m_ink((k - 1) / 3.0f);
    if (tw->to > tw->from) { *plain = out; *armed = in; }
    else if (tw->to < tw->from) { *plain = in; *armed = out; }
    else { *plain = tw->to > 0.5f ? 0.0f : 1.0f; *armed = 1.0f - *plain; }
}
/* Arm-then-tap-again pill (UI_BTN_H tall), 260 px wide plain, 320 armed. `tw` is the arm
 * tween (0 plain, 1 armed): the plain label goes out, then "TAP AGAIN TO ..." and three
 * red dots that expire one per second come in. `red` 0..1 fades the plain outline to
 * grey while another row is armed. `surf`: the plain state is a filled secondary pill
 * (Restart, as on the power sheet) whose fill fades out as the red outline comes in;
 * else a red-outlined danger pill (Reset). */
static void arm_pill(gfx_fb *fb, float cy, const mtween *tw, const char *label, const char *armed_label,
                     nos_time_t armed_t, int pressed, float red, float k, int surf)
{
    float a = m_clamp(mtw_value(tw), 0, 1), w = m_lerp(260.0f, 320.0f, a);
    uint16_t line = surf ? ui_mix(C_SURF2, C_RED, q8(a)) : ui_mix(C_LINE2, C_RED, m_clamp(red + a, 0, 1));
    uint16_t ink = surf ? (pressed ? C_BG : C_PRI) : ui_mix(C_DIS, C_RED, red);
    float x = floorf(UI_CX - w * 0.5f + 0.5f), y = cy - UI_BTN_H * 0.5f;
    w = (UI_CX - x) * 2.0f;
    if (surf && a < 1) ui_rrect(fb, x, y, w, UI_BTN_H, UI_BTN_H * 0.5f,
                                ui_dim(pressed && a <= 0 ? C_SEC : C_SURF2, k * q8(1.0f - a)));
    else if (pressed && a <= 0) ui_rrect(fb, x, y, w, UI_BTN_H, UI_BTN_H * 0.5f, ui_dim(C_SURF2, k));
    if (!surf || a > 0) ui_rrect_line(fb, x, y, w, UI_BTN_H, UI_BTN_H * 0.5f, 2, ui_dim(line, k));
    int asc = UI_F_LABEL->ascent;
    float aout, ain;
    arm_inks(tw, &aout, &ain);
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
 * secondary pitch one slot away, fading out beyond. The rulers at both sides carry a
 * dot every third of a slot.
 * Motion (MOTION30): a fling glides at constant deceleration onto the detent its speed
 * reaches (a wheel with friction: frame 1 keeps the finger's speed, the drum stops dead
 * with a last step of ~1.4 px, no creeping tail); a slow release or a touch clicks into
 * the nearest detent in 5 frames (EASE_STEADY); a tap on a neighbour steps the drum one
 * value with a mechanical cut (m_cut: nudge, lead-in, one jump, 4-2-1-0 settle), so no
 * frame shows the big dot-matrix numerals sliding at a mid speed (R4, R8). */
#define PK_STEP 94.0f
#define PK_SNAP_F 5             /* a slow release / a touch: frames to the nearest detent */
#define PK_GLIDE_MAX_F 45       /* the longest glide: 1.5 s */
typedef struct {
    int n, idx;
    float x, x0;            /* drum position, px (value i is centred at i * PK_STEP) */
    mtween tw;              /* the release glide / detent click; for a cut: from, to, t0 */
    int dragging, moving, over;
    int cut;                /* the move is a tap's m_cut step */
    int locked;             /* idx was set by a tap; the move only carries the drum there */
    const char *probe;      /* motion audit name */
} pk;
/* constant deceleration: 1 - (1 - u)^2 as a cubic bezier (its first frame keeps the
 * release speed when the glide lasts 2 * distance / speed) */
static const bezier EASE_GLIDE = { 1.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f, 1.0f };
typedef struct { int c; char s[5][12]; } pk_txt;   /* texts of values c-2..c+2 */

static float pk_max(const pk *p) { return (float)(p->n - 1) * PK_STEP; }
static void pk_init(pk *p, int n, int idx)
{
    memset(p, 0, sizeof *p);
    p->n = n < 1 ? 1 : n;
    p->idx = idx < 0 ? 0 : idx >= p->n ? p->n - 1 : idx;
    p->x = p->idx * PK_STEP;
}
static int pk_centre(const pk *p) { return (int)floorf(m_clamp(p->x, 0, pk_max(p)) / PK_STEP + 0.5f); }
/* a touch catches a gliding drum where it is; a tap's cut is a click, so it lands first
 * (a second quick tap then steps on from that value) */
static void pk_grab(pk *p)
{
    int landed = p->moving && p->cut;           /* still the tap's step until the finger acts */
    if (landed) p->x = p->tw.to;
    p->dragging = 1; p->moving = 0; p->cut = 0; p->locked = landed; p->x0 = p->x; p->over = 0;
}
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
/* Carry the drum to `target` (px) from a release at v px/s (0: a touch or a slow
 * release). Started from input, so the move shows on this frame already. */
static void pk_go(pk *p, float target, float v)
{
    float d = target - p->x;
    p->cut = 0;
    if (fabsf(d) < 0.5f) { p->x = target; p->moving = 0; p->locked = 0; return; }
    float f = 0;
    if (d * v > 0) f = 2.0f * fabsf(d) / fabsf(v) * M_FPS;   /* glide frames at that speed */
    if (f > PK_SNAP_F + 0.5f) {
        if (f > PK_GLIDE_MAX_F) f = PK_GLIDE_MAX_F;
        mtw_start(&p->tw, p->x, target, M_FRAMES(floorf(f + 0.5f)), &EASE_GLIDE);
    } else mtw_start(&p->tw, p->x, target, M_FRAMES(PK_SNAP_F), &EASE_STEADY);
    mtw_lead(&p->tw);
    p->moving = 1;
}
static void pk_release(pk *p, float vy)
{
    p->dragging = 0;
    float v = -vy, mx = pk_max(p);
    float rest = (p->x < 0 || p->x > mx) ? p->x : decay_project(p->x, v);
    float k = floorf(m_clamp(rest, 0, mx) / PK_STEP + 0.5f);
    pk_go(p, k * PK_STEP, (p->x < 0 || p->x > mx) ? 0 : v);
}
/* a tap on a neighbour: the value changes now (tick, commit); the drum steps there with
 * a cut that starts on this frame */
static int pk_step(pk *p, int d)
{
    int t = p->idx + d;
    if (t < 0 || t >= p->n) { nos_feedback(FB_LIMIT); return 0; }
    p->idx = t;
    p->locked = 1;
    nos_feedback(FB_TICK);
    if (p->moving && p->cut) p->x = p->tw.to;     /* a step still running lands first */
    mtw_start(&p->tw, p->x, t * PK_STEP, M_FRAMES(M_CUT_FRAMES), &EASE_LINEAR);
    mtw_lead(&p->tw);
    p->cut = 1;
    p->moving = 1;
    p->x = m_cut(p->tw.from, p->tw.to, m_frame_k(p->tw.t0));
    nos_motion_probe(p->probe, MC_COMPONENT, -floorf(p->tw.from * 2.0f + 0.5f) * 0.5f);   /* the audit: a new move */
    return 1;
}
/* advance; returns 1 when the detent on the centre line changed */
static int pk_tick(pk *p, float dt)
{
    (void)dt;
    if (p->moving && !p->dragging) {
        if (p->cut) {
            int k = m_frame_k(p->tw.t0);
            p->x = m_cut(p->tw.from, p->tw.to, k);
            if (k >= M_CUT_FRAMES) { p->moving = 0; p->cut = 0; p->locked = 0; p->x = p->tw.to; }
        } else if (mtw_done(&p->tw)) { p->moving = 0; p->locked = 0; p->x = p->tw.to; }
        else p->x = mtw_value(&p->tw);
    }
    if (p->locked) return 0;
    int k = pk_centre(p);
    if (k != p->idx) { p->idx = k; nos_feedback(FB_TICK); return 1; }
    return 0;
}
#ifdef GFX_COST_MODEL
/* motion audit: the drum as drawn (half px); a finger, its release, or a tap's step */
static void pk_probe(const char *name, const pk *p)
{
    nos_motion_probe(name, p->dragging ? MC_FINGER : p->locked ? MC_COMPONENT : MC_MOMENTUM,
                     -floorf(p->x * 2.0f + 0.5f) * 0.5f);
}
#endif
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
                      unsigned seed, int hl, float wy0, float wy1)
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
        /* gone by the time a glyph reaches the window edge: never sliced */
        float ke = q8(fminf(y - wy0, wy1 - (y + 9.0f * pitch)) / 14.0f);
        if (ke <= 0) continue;
        if (ke < 1) { col = ui_dim(col, ke); st.color = col; }
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
    I_BRIGHT = 0, I_THEME, I_AOD, I_AODB, I_TIMEOUT, I_RAISE, I_LEARN, I_KNOCK, I_FACE,
    I_SOUND, I_VOLUME, I_HAPTICS, I_HSTR, I_DND,
    I_BT, I_WIFI, I_CELL, I_GPS,
    I_H24, I_TZ, I_SETTIME,
    I_GOAL, I_WRIST, I_UNITS,
    I_MODULES, I_BATTERY, I_STORAGE, I_ABOUT, I_RESTART, I_RESET,
    I_COUNT, I_NONE = 255
};
/* Diagnostic targets 0x5000 + I_* are fixed controls, never display text. A SET
 * success confirms the RAM setting; prefs owns checked persistence separately. */
static uint32_t settings_action(int id, uint16_t action, int old)
{
    uint32_t request = diag_action_request(DIAG_DOMAIN_CONTROL, action, 0x5000 + id);
    diag_action_context(request, (int32_t)diag_log_name_id("SETTINGS"), old);
    return request;
}
static int settings_value(int id)
{
    return id == I_WRIST ? nos_prefs.wrist_left : id == I_THEME ? nos_prefs.theme :
           id == I_UNITS ? nos_prefs.units_metric : id == I_TIMEOUT ? nos_prefs.sleep_s :
           id == I_TZ ? nos_prefs.tz_min : nos_prefs.step_goal_100;
}
typedef struct { uint8_t kind, id; const char *label; } sentry;
static const sentry E[] = {
    { K_HDR, I_NONE, "Display" },
    { K_NAV, I_BRIGHT, "Brightness" },
    { K_FLIP, I_THEME, "Theme" },
    { K_TOG, I_AOD, "Always-on display" },
    { K_NAV, I_AODB, "Always-on brightness" },
    { K_NAV, I_TIMEOUT, "Screen timeout" },
    { K_TOG, I_RAISE, "Raise to wake" },
    { K_NAV, I_LEARN, "Learn reading angle" },
    { K_TOG, I_KNOCK, "Knock to lock" },
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
    { K_NAV, I_BATTERY, "Battery" },
    { K_NAV, I_STORAGE, "Storage" },
    { K_NAV, I_ABOUT, "About" },
    { K_ARM, I_RESTART, "Restart" },
    { K_DANGER, I_RESET, "Reset settings" },
};
#define NE ((int)(sizeof E / sizeof E[0]))
#define UNIT 30
#define LTOP 96
static int units_of(int kind) { return kind == K_HDR ? 2 : kind == K_DANGER || kind == K_ARM ? 4 : 3; }

static struct {
    ui_list L;
    int start[NE];              /* first unit of each entry */
    int8_t unit_e[NE * 4];      /* unit -> entry */
    int nunits;
    int inited;
    int was_moving;
    nos_time_t build_t0;
    mtween tog[I_COUNT];         /* toggle thumbs: 4 frames on MF_STEADY4 */
    char val[I_COUNT][24];
    char unit[I_COUNT][8];
    /* a changed value cross-fades from the old one (which may sit on the other line) */
    char oval[I_COUNT][24];
    char ounit[I_COUNT][8];
    nos_time_t vt0[I_COUNT];
    int xf_ok;
    nos_time_t stor_t; nos_time_t mod_t;
    hal_storage stor;
    int mods_on, mods_tot;
    uint32_t ext;
    int flash_e; nos_time_t flash_t;  /* pressed pill held after a quick tap */
    int arm_e; nos_time_t arm_t;      /* armed Restart / Reset */
    mtween arm_tw[2];            /* 0 Restart, 1 Reset: plain (0) -> armed (1), 5 frames */
    mtween grey_tw;              /* Reset's red fades to grey while Restart is armed */
    nos_time_t restart_at;            /* pending restart after the toast */
    int ptog, pval;              /* motion audit: the last toggled / changed row */
    int learn_wait;              /* Learn reading angle: waiting for its result */
} S;

static uint8_t *tog_pref(int id)
{
    switch (id) {
    case I_AOD: return &nos_prefs.aod;
    case I_RAISE: return &nos_prefs.raise_wake;
    case I_KNOCK: return &nos_prefs.knock_lock;
    case I_SOUND: return &nos_prefs.sound;
    case I_HAPTICS: return &nos_prefs.haptics;
    case I_DND: return &nos_prefs.dnd;
    /* Automatic GPS demand is not implemented by the device host. Apps own
     * receiver leases; never present the legacy preference as a live switch. */
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
/* "NO <NAME>" from the OS's one short-name table (ui_module_short) */
static const char *no_module(uint32_t ext)
{
    static char b[24];
    snprintf(b, sizeof b, "NO %s", ui_module_short(ext));
    return b;
}
static const char *tog_missing(int id) { return no_module(id == I_GPS ? EXT_GPS : id == I_SOUND ? EXT_SPEAKER : EXT_HAPTIC); }

static void set_val(int id, const char *v, const char *u)
{
    if (!u) u = "";
    if (!strcmp(S.val[id], v) && !strcmp(S.unit[id], u)) return;
    if (S.xf_ok) {
        memcpy(S.oval[id], S.val[id], sizeof S.oval[id]);
        memcpy(S.ounit[id], S.unit[id], sizeof S.ounit[id]);
        S.vt0[id] = nos_t;
        S.pval = id;
    }
    snprintf(S.val[id], sizeof S.val[id], "%s", v);
    snprintf(S.unit[id], sizeof S.unit[id], "%s", u);
}

static void update_values(void)
{
    char b[24];
    if (nos_prefs.auto_bright && (S.ext & EXT_LIGHT)) { snprintf(b, sizeof b, "Auto %d%%", nos_prefs.brightness); set_val(I_BRIGHT, b, 0); }
    else { snprintf(b, sizeof b, "%d%%", nos_prefs.brightness); set_val(I_BRIGHT, b, 0); }
    snprintf(b, sizeof b, "%d", nos_prefs.sleep_s); set_val(I_TIMEOUT, b, "s");
    int nf = nos_face_count;
    int fi = nos_prefs.face < nf ? nos_prefs.face : 0;
    set_val(I_FACE, nf > 0 && nos_faces[fi] && nos_faces[fi]->name ? nos_faces[fi]->name : "", 0);
    snprintf(b, sizeof b, "%d%%", nos_prefs.aod_bright); set_val(I_AODB, b, 0);
    {
        wake_info w; wake_service_get(&w);
        snprintf(b, sizeof b, "%s %s", w.wrist ? "Right" : "Left",
                 w.learning ? "holding" : w.learned ? "learned" : "default");
        set_val(I_LEARN, b, 0);
    }
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
    set_val(I_THEME, nos_prefs.theme ? "Light" : "Dark", 0);
    set_val(I_WRIST, nos_prefs.wrist_left ? "Left" : "Right", 0);
    set_val(I_UNITS, nos_prefs.units_metric ? "Metric" : "Imperial", 0);
    snprintf(b, sizeof b, "%d/%d", S.mods_on, S.mods_tot); set_val(I_MODULES, b, 0);
    {
        const svc_sys_t *sy = svc_sys();
        int bp = sy->battery_pct > 0 ? sy->battery_pct : hal_battery_percent();
        if (sy->external) set_val(I_BATTERY, "USB power", 0);
        else { snprintf(b, sizeof b, "%d", bp); set_val(I_BATTERY, b, "%"); }
    }
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
        tw_set(&S.tog[i], p && *p ? 1.0f : 0.0f);
    }
}

static void rows_open(void);
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
    for (int j = 0; j < 2; j++) tw_set(&S.arm_tw[j], 0);
    tw_set(&S.grey_tw, 0);
    S.restart_at = -1;
    S.pval = -1;
    S.xf_ok = 0;
    update_values();
    memcpy(S.oval, S.val, sizeof S.oval);
    memcpy(S.ounit, S.unit, sizeof S.ounit);
    for (int i = 0; i < I_COUNT; i++) S.vt0[i] = -100.0f;
    S.xf_ok = 1;
    rows_open();
}

static void settings_probe(void);
static void rows_tick(float dt);

static void settings_tick(float dt)
{
    ui_list_tick(&S.L, dt);
    S.ext = hal_ext_present();
    if (nos_t - S.stor_t > 2.0f) { hal_storage_get(&S.stor); S.stor_t = nos_t; }
    if (nos_t - S.mod_t > 1.0f) { S.mods_on = set_modules_online(&S.mods_tot); S.mod_t = nos_t; }
    if (S.arm_e >= 0 && (nos_t - S.arm_t > ARM_S || S.L.dragging)) S.arm_e = -1;
    int armed_id = S.arm_e >= 0 ? E[S.arm_e].id : I_NONE;
    /* arming happens on a tap (input already led the tween); expiry and scroll here */
    tw_to(&S.arm_tw[0], armed_id == I_RESTART ? 1.0f : 0.0f, &MF_STEADY5, 0);
    tw_to(&S.arm_tw[1], armed_id == I_RESET ? 1.0f : 0.0f, &MF_STEADY5, 0);
    tw_to(&S.grey_tw, armed_id == I_RESTART ? 1.0f : 0.0f, &MF_STEADY5, 0);
    if (S.restart_at >= 0 && nos_t >= S.restart_at) { S.restart_at = -1; nos_host_power(1); }
    update_values();
    if (S.learn_wait) {                      /* the reading angle's result */
        wake_info w; wake_service_get(&w);
        if (!w.learning) {
            S.learn_wait = 0;
            if (w.learn_result == 1) { nos_toast(w.wrist ? "RIGHT ANGLE SAVED" : "LEFT ANGLE SAVED"); nos_feedback(FB_SUCCESS); }
            else { nos_toast("TRY AGAIN: HOLD STILL"); nos_feedback(FB_ERROR); }
        }
    }
    rows_tick(dt);
    settings_probe();
}

static float list_off(void) { return floorf(ui_list_offset(&S.L) + 0.5f); }

/* ---- per-entry fling sprites: fixed columns, pitch-cropped tails ----------------
 * The complete list is resident while open. Home warms only the first screen;
 * hidden rows are released at a UI safe point, before other caches grow again.
 * Each allocation is bounded by one entry, never a screen-wide band. */
#define ROW_W (UI_ROW_X1 - UI_ROW_X0 + 1)
#define ROW_MAX_H (4 * UNIT)
#define ROW_HALO 0
#define ROW_CACHE_LIMIT (3u * 1024u * 1024u)
static const int row_blur_len[4] = { 15, 30, 60, 90 };
typedef struct { sprite8 s; int x, y; } entry_image;
typedef struct { entry_image image[9]; unsigned sig; int ready,rom; uint16_t *prefix; unsigned prefix_bytes; } entry_cache;
#define ROW_SLICE 4
unsigned set_settings_bake_budget_us = 750;
unsigned set_settings_bake_max_steps = 16;
typedef struct {
    entry_cache staged;
    native_trail_kernel trail;
    int active, e, phase, row, level, h, shift, mask, peak;
    int x0, x1, y0, y1, copy;
    unsigned sig;
    int sum[ROW_W];
} row_job;
static struct {
    native_carry_pool carry_pool; /* PSRAM owned and freed with this cache. */
    entry_cache entry[NE];
    uint16_t lut[2][65][256];             /* 64-step neutral and Reset ink */
    struct {
        uint8_t pixels[ROW_W * ROW_MAX_H];
        sprite8 s;
        int entry, x, y, combined;
        unsigned sig,touched;
    } controls[8];
    uint16_t control_canvas[ROW_W * ROW_MAX_H];
    uint16_t canvas[ROW_W * ROW_MAX_H];
    uint8_t grey[ROW_W * ROW_MAX_H];
    uint8_t blur[ROW_W * (ROW_MAX_H + 2 * ROW_HALO)];
    uint16_t red;
    int lut_pos, lut_ready,rom_palette,rom_bound;
    unsigned rom_sync_publication;
    const font_t *font_body,*font_label,*font_title;
    row_job job;
} *RC;
static int settings_carry_enabled=1;
static native_carry_stats settings_carry_stats;
static int shutter_enabled;
static float shutter_previous;
static int shutter_exposure, shutter_pose;
static settings_shutter_kernel shutter_kernel;
static settings_shutter_stats shutter_stats;
static nos_time_t row_retry_at[NE]; nos_time_t rows_retry_at;
static uint32_t row_retry_room[NE], rows_retry_room;
static int row_deferred;
static void row_cancel(void);
static unsigned row_bytes, row_bakes, control_rasters, control_work_pixels, control_prepare_us;
static float row_prev, row_step;
static int row_level, row_baking, row_overlay, row_neutral;
static int native_pose, native_exposure, native_direction;
static unsigned control_current_mask,control_tick_frame;
static float native_previous;
static void controls_tick(void);
static int value_under(const char *label, const char *v, const char *u);
static struct { unsigned sig; int settled,under,old_under,combined; } entry_draw_revision[NE];
static unsigned entry_draw_frame,entry_signature_walks,entry_draw_hits;
static int entry_draw_valid;
static unsigned entry_draw_publication;
static void entry_draw_prepare(void);
static int settings_rom_active(void);
static void rows_rom_sync(void);
static const uint16_t *rows_ink(int red,int level);
typedef struct { float y,h,dy,bk,k; int lev,valid; } entry_geometry;
static entry_geometry entry_draw_geometry[NE];
static unsigned entry_geometry_evaluations,entry_geometry_hits;
static float build_k(float y,float *dy);


/* These tiles own labels only. Controls remain live and retain compositor blur. */
static int entry_split(int e)
{
    return E[e].kind == K_TOG || E[e].kind == K_NAV || E[e].kind == K_FLIP;
}
void ui_fblur_release(void);
void pblur_release(void);
static void draw_entry(gfx_fb *fb, int e, float y, float h, int pressed);

static void entry_free(entry_cache *r)
{
    if(RC)RC->rom_sync_publication=0;
    if(!r->rom){
        for (int k = 0; k < 9; k++) {
            row_bytes -= (unsigned)(r->image[k].s.w * r->image[k].s.h);
            hal_big_free(r->image[k].s.px);
        }
        row_bytes -= r->prefix_bytes; hal_big_free(r->prefix);
    }
    memset(r, 0, sizeof *r);
}
void set_settings_carry_set(int enabled)
{
    enabled=!!enabled;
    if(enabled==settings_carry_enabled)return;
    settings_carry_enabled=enabled;
    if(RC)native_carry_reset(&RC->carry_pool,nos_frame);
    memset(&settings_carry_stats,0,sizeof settings_carry_stats);
}
int set_settings_carry_get(void) { return settings_carry_enabled; }
void set_settings_carry_report(void)
{
    unsigned bytes=RC?(unsigned)sizeof RC->carry_pool:0;
    printf("NOSCARRY v=1 enabled=%d bytes=%u allocated=%u capacity_bytes=%u location=psram-stage calls=%llu hits=%llu saved_reads=%llu copy_bytes=%llu full_misses=%llu\n",
           settings_carry_enabled,bytes,bytes,(unsigned)sizeof(native_carry_pool),
           (unsigned long long)settings_carry_stats.calls,(unsigned long long)settings_carry_stats.hits,
           (unsigned long long)settings_carry_stats.saved_reads,(unsigned long long)settings_carry_stats.copy_bytes,
           (unsigned long long)settings_carry_stats.full_misses);
}
void set_settings_cache_release(void)
{
    memset(&settings_carry_stats,0,sizeof settings_carry_stats);
    if (!RC) return;
    row_cancel();
    for (int e = 0; e < NE; e++) entry_free(&RC->entry[e]);
    hal_big_free(RC); RC = 0; row_bytes = 0;
}
void set_settings_shutter_set(int enabled)
{
    enabled = !NOS_RENDER_SHARP && !!enabled;
    if (enabled == shutter_enabled) return;
    set_settings_cache_release();
    shutter_enabled = enabled;
    memset(&shutter_stats, 0, sizeof shutter_stats);
    shutter_previous = ui_list_offset(&S.L);
    shutter_exposure = shutter_pose = 0;
    settings_shutter_kernel_make(&shutter_kernel, 0, 0);
    memset(row_retry_at, 0, sizeof row_retry_at);
    memset(row_retry_room, 0, sizeof row_retry_room);
    rows_retry_at = 0; rows_retry_room = 0;
    nos_layers_invalidate();
    nos_invalidate();
}
int set_settings_shutter_get(void) { return shutter_enabled; }
float set_settings_scroll_offset(void) { return ui_list_offset(&S.L); }
settings_shutter_stats set_settings_shutter_stats(void)
{
    settings_shutter_stats out = shutter_stats;
    out.enabled = shutter_enabled; out.exposure_q8 = shutter_exposure; out.pose_q8 = shutter_pose;
    return out;
}
void set_settings_cache_mem(void)
{
    int ready = 0;
    if (RC) for (int e = 0; e < NE; e++) ready += RC->entry[e].ready;
    printf("SETTINGS_CACHE entries=%d/%d bytes=%u limit=%u bakes=%u\n",
           ready, NE, row_bytes, ROW_CACHE_LIMIT, row_bakes);
    printf("SETTINGS_CONTROLS rasters=%u pixels=%u prep_us=%u pose_q8=%d exposure_q8=%d\n",
           control_rasters,control_work_pixels,control_prepare_us,native_pose,native_exposure);
    printf("SETTINGS_REVISIONS walks=%u draw_hits=%u geometry=%u geometry_hits=%u\n",
           entry_signature_walks,entry_draw_hits,entry_geometry_evaluations,entry_geometry_hits);
}
/* Called before home prewarming, never during a transition using outgoing sprites. */
void set_settings_cache_idle(void)
{
    if (nos_transitioning() || nos_top() == &scr_settings) return;
    /* Home now warms entry sprites instead of sharing the generic list arena. */
    if (nos_top() == &scr_home) ui_fblur_release();
    if (!RC) return;
    if (nos_top() != &scr_home) { set_settings_cache_release(); return; }
    if (RC->job.active && LTOP + S.start[RC->job.e] * UNIT >= GFX_H) row_cancel();
    for (int e = 0; e < NE; e++)
        if (!RC->entry[e].rom && LTOP + S.start[e] * UNIT >= GFX_H) entry_free(&RC->entry[e]);
}
static void *row_alloc(unsigned bytes)
{
    uint32_t room = nos_mem_available();
    if (row_bytes > ROW_CACHE_LIMIT || bytes > ROW_CACHE_LIMIT - row_bytes ||
        room < 16384u || bytes > room - 16384u) { row_deferred = 1; return 0; }
    void *p = hal_big_alloc(bytes);
    if (p) row_bytes += bytes;
    return p;
}
static void row_cancel(void)
{
    if (!RC || !RC->job.active) return;
    entry_free(&RC->job.staged);
    memset(&RC->job, 0, sizeof RC->job);
}
static int rows_init(void)
{
    if (RC) return 1;
    uint32_t room = nos_mem_available();
    if (nos_t < rows_retry_at && room <= rows_retry_room) return 0;
    row_deferred = 0;
    RC = row_alloc(sizeof *RC);
    if (!RC) {
        if (!row_deferred) { rows_retry_at = nos_t + 1; rows_retry_room = room; }
        return 0;
    }
    /* Pixel workspaces are overwritten in bounded slices before use. */
    native_carry_reset(&RC->carry_pool,nos_frame);
    memset(RC->entry, 0, sizeof RC->entry);
    for (unsigned i=0;i<8;i++) { RC->controls[i].entry=-1; RC->controls[i].s.px=RC->controls[i].pixels; }
    memset(&RC->job, 0, sizeof RC->job);
    RC->rom_sync_publication=0;
    RC->red = C_RED; RC->lut_pos = 0;
    RC->rom_palette=settings_rom_active();RC->lut_ready=RC->rom_palette;RC->rom_bound=0;
    RC->font_body=UI_F_BODY;RC->font_label=UI_F_LABEL;RC->font_title=UI_F_TITLE;
    return 1;
}
static int settings_rom_active(void)
{
    return !shutter_enabled && native_menu_settings_palette(0,64) && native_menu_settings_palette(1,64);
}
static const uint16_t *rows_ink(int red,int level)
{
    if(RC->rom_palette){
        const uint16_t *ink=native_menu_settings_palette(red,level);
        if(ink)return ink;
    }
    return RC->lut[red][level];
}
static void rows_palette(void)
{
    if(!RC)return;
    int rom=settings_rom_active();
    int fonts=RC->font_body==UI_F_BODY && RC->font_label==UI_F_LABEL && RC->font_title==UI_F_TITLE;
    if(RC->red==C_RED && RC->rom_palette==rom && fonts)return;
    RC->rom_sync_publication=0;
    native_carry_reset(&RC->carry_pool,nos_frame);
    row_cancel();
    for(int e=0;e<NE;e++)if(!fonts || (!rom && RC->entry[e].rom))entry_free(&RC->entry[e]);
    if(!fonts){
        for(int i=0;i<8;i++)RC->controls[i].entry=-1;
        control_current_mask=0;
        RC->font_body=UI_F_BODY;RC->font_label=UI_F_LABEL;RC->font_title=UI_F_TITLE;
    }
    RC->red=C_RED;RC->rom_palette=rom;RC->rom_bound=0;
    RC->lut_pos=0;RC->lut_ready=rom;
}
static void rows_palette_step(void)
{
    if(RC->rom_palette){RC->lut_ready=1;return;}
    for (int n = 0; n < 256 && RC->lut_pos < 2 * 65 * 256; n++, RC->lut_pos++) {
        int at = RC->lut_pos, which = at / (65 * 256), level = (at / 256) % 65, i = at % 256;
        RC->lut[which][level][i] = ui_dim(which ? C_RED : C_WHITE, i * level / (255.0f * 64.0f));
    }
    RC->lut_ready = RC->lut_pos == 2 * 65 * 256;
}
static unsigned source_font_key(unsigned h,const font_t *font)
{
    uintptr_t pointer=(uintptr_t)font;
    h=gfx_hash_u(h,(unsigned)pointer);
#if UINTPTR_MAX > UINT32_MAX
    h=gfx_hash_u(h,(unsigned)(pointer>>32));
#endif
    return h;
}
static unsigned entry_sig(int e)
{
    entry_signature_walks++;
    unsigned h = source_font_key(source_font_key(source_font_key(2166136261u,UI_F_BODY),UI_F_LABEL),UI_F_TITLE);
    int id = E[e].id;
    if (entry_split(e)) {
        /* Only the under-label layout changes static ink. Values and toggle
         * state must not cancel an unfinished label bake or replace its assets. */
        if (E[e].kind != K_TOG)
            h = (h ^ (unsigned)value_under(E[e].label, S.val[id], S.unit[id])) * 16777619u;
    }
    if (E[e].kind == K_DANGER) h ^= C_RED;
    return h;
}
static int entry_settled(int e)
{
    int id = E[e].id;
    if (id == I_NONE) return 1;
    if (E[e].kind == K_TOG) return 1;
    if (entry_split(e)) {
        if (m_frame_k(S.vt0[id]) >= VAL_F) return 1;
        return value_under(E[e].label, S.val[id], S.unit[id]) ==
               value_under(E[e].label, S.oval[id], S.ounit[id]);
    }
    if (E[e].kind == K_ARM || E[e].kind == K_DANGER)
        return S.arm_e < 0 && mtw_done(&S.arm_tw[0]) && mtw_done(&S.arm_tw[1]) && mtw_done(&S.grey_tw);
    return m_frame_k(S.vt0[id]) >= VAL_F;
}
/* Only draw consumers use this snapshot. Bake jobs retain their raw source
 * checks so mutations while unpublished still cancel before publication. */
static void entry_draw_prepare(void)
{
    /* Every tick is a new immutable publication, even within one frame. */
    if(!++entry_draw_publication){entry_draw_publication=1;if(RC)RC->rom_sync_publication=0;}
    entry_draw_valid=0;
    for(int e=0;e<NE;e++){
        entry_draw_geometry[e].valid=0;
        int id=E[e].id,has_value=E[e].kind==K_NAV || E[e].kind==K_FLIP;
        int under=has_value && value_under(E[e].label,S.val[id],S.unit[id]);
        int old_under=has_value && m_frame_k(S.vt0[id])<VAL_F &&
            value_under(E[e].label,S.oval[id],S.ounit[id]);
        entry_draw_revision[e].under=under;
        entry_draw_revision[e].old_under=old_under;
        entry_draw_revision[e].combined=has_value && (under || old_under);
        entry_draw_revision[e].sig=entry_sig(e);
        entry_draw_revision[e].settled=entry_settled(e);
    }
    entry_draw_frame=nos_frame;entry_draw_valid=1;
}
static int entry_draw_current(void)
{
    return entry_draw_valid && entry_draw_frame==nos_frame;
}
/* ROM pixels are borrowed, immutable and never enter staged bake ownership.
 * Publish all four validated descriptors together, with the current layout key. */
static void rows_rom_sync(void)
{
    if(!RC || !RC->rom_palette)return;
    /* Acknowledgement belongs to this cache and exact prepared publication.
     * Raw callers, partial assets and active jobs retain the original walk. */
    if(entry_draw_current() && RC->rom_sync_publication==entry_draw_publication && !RC->job.active)return;
    int complete=1;
    row_cancel();
    for(int e=0;e<NE;e++){
        unsigned sig=entry_draw_current()?entry_draw_revision[e].sig:entry_sig(e);
        entry_cache *r=&RC->entry[e];
        if(r->rom && r->ready && r->sig==sig)continue;
        int layout=E[e].kind==K_DANGER?(C_RED==0x2F3B):
            (E[e].kind==K_NAV || E[e].kind==K_FLIP)?
            (entry_draw_current()?entry_draw_revision[e].under:value_under(E[e].label,S.val[E[e].id],S.unit[E[e].id])):0;
        entry_cache next={0};int valid=1;
        for(int k=0;k<(NOS_RENDER_SHARP?1:9);k++){ /* SHARP only ever samples level 0 */
            const native_asset_desc *asset=native_menu_asset(0,(unsigned)e,(unsigned)layout,(unsigned)k);
            if(!asset){valid=0;break;}
            next.image[k].s=native_menu_sprite(asset);
            if(!next.image[k].s.px){valid=0;break;}
            next.image[k].x=asset->x;next.image[k].y=asset->y;
        }
        if(!valid){complete=0;if(r->rom)entry_free(r);continue;}
        next.sig=sig;next.ready=next.rom=1;
        entry_free(r);*r=next;
    }
    RC->rom_bound=1;
    RC->rom_sync_publication=complete && entry_draw_current()?entry_draw_publication:0;
}
/* Geometry and ink are immutable between tick and all record/raster stripes.
 * Unusual callback poses and offscreen bake/capture retain the raw calculation. */
static const entry_geometry *entry_geometry_get(int e,float y,float h,entry_geometry *local)
{
    int cached=!row_baking && !row_neutral && entry_draw_current();
    entry_geometry *g=cached?&entry_draw_geometry[e]:local;
    if(cached && g->valid && g->y==y && g->h==h){entry_geometry_hits++;return g;}
    g->y=y;g->h=h;g->bk=build_k(y,&g->dy);
    g->k=row_baking || row_neutral?1.0f:qfade(y+g->dy+h*.5f)*g->bk;
    g->lev=(int)(g->k*64.0f+.5f);g->valid=1;
    entry_geometry_evaluations++;
    return g;
}
/* One job owns the shared workspace. Published sprites survive staging and are
 * replaced atomically after all seven images and a final signature check. */
static void row_bounds(row_job *j, const uint8_t *p, int y, int rows)
{
    for (int sy = y; sy < y + rows; sy++) for (int x = 0; x < ROW_W; x++) if (p[sy * ROW_W + x]) {
        if (x < j->x0) j->x0 = x;
        if (x > j->x1) j->x1 = x;
        if (sy < j->y0) j->y0 = sy;
        if (sy > j->y1) j->y1 = sy;
    }
}
static void row_bounds_reset(row_job *j)
{
    j->x0 = ROW_W; j->x1 = -1; j->y0 = j->h + 2 * ROW_HALO; j->y1 = -1;
}
static void row_failed(void)
{
    int e = RC->job.e;
    row_cancel();
    /* Measure after freeing our staging data: our own rollback is not a recovery. */
    row_retry_at[e] = nos_t + 1.0f; row_retry_room[e] = nos_mem_available();
}
static void row_start(int e)
{
    row_job *j = &RC->job;
    memset(j, 0, sizeof *j); j->active = 1; j->e = e;
    j->sig = entry_sig(e); j->h = units_of(E[e].kind) * UNIT;
    j->shift = 5; j->mask = j->peak = 63;
    if (E[e].kind == K_DANGER) {
        int rr = C_RED >> 11, gg = (C_RED >> 5) & 63, bb = C_RED & 31;
        if (rr * 2 >= gg && rr >= bb) { j->shift = 11; j->mask = 31; j->peak = rr; }
        else if (bb * 2 >= gg) { j->shift = 0; j->mask = 31; j->peak = bb; }
        else j->peak = gg;
    }
    row_bounds_reset(j);
}
static void row_step_job(void)
{
    row_job *j = &RC->job;
    if (j->sig != entry_sig(j->e) || !entry_settled(j->e)) { row_cancel(); return; }
    int height = j->h + (j->level ? 2 * ROW_HALO : 0);
    int rows = height - j->row; if (rows > ROW_SLICE) rows = ROW_SLICE;
    if (j->phase == 0) {
        memset(RC->canvas + j->row * ROW_W, 0, (size_t)rows * ROW_W * 2);
        gfx_fb fb = {0}; fb.px = RC->canvas + j->row * ROW_W;
        fb.stride = ROW_W; fb.y_base = j->row; fb.h = rows;
        fb.ox = -UI_ROW_X0; fb.cx1 = ROW_W;
#ifdef GFX_COST_MODEL
        fb.cost_psram = 1;
#endif
        row_baking = 1; draw_entry(&fb, j->e, 0, (float)j->h, 0); row_baking = 0;
        j->row += rows;
        if (j->row == j->h) { j->phase = 1; j->row = 0; }
        return;
    }
    if (j->phase == 1) {
        for (int i = j->row * ROW_W; i < (j->row + rows) * ROW_W; i++)
            RC->grey[i] = (uint8_t)((((RC->canvas[i] >> j->shift) & j->mask) * 255 + j->peak / 2) / j->peak);
        row_bounds(j, RC->grey, j->row, rows);
        j->row += rows;
        if (j->row == j->h) { j->phase = 2; j->row = 0; }
        return;
    }
    if (j->phase == 2) {
        if (j->x1 < j->x0) { j->phase = 4; return; }
        entry_image *im = &j->staged.image[j->level];
        int w = j->x1 - j->x0 + 1, h = j->y1 - j->y0 + 1;
        row_deferred = 0;
        uint8_t *px = row_alloc((unsigned)(w * h));
        if (!px) { if (!row_deferred) row_failed(); return; }
        im->s = (sprite8){px,w,h}; im->x = j->x0; im->y = j->y0 - (j->level ? ROW_HALO : 0);
        j->copy = 0; j->phase = 3; return;
    }
    if (j->phase == 3) {
        entry_image *im = &j->staged.image[j->level];
        int n = im->s.h - j->copy; if (n > ROW_SLICE) n = ROW_SLICE;
        const uint8_t *src = j->level ? RC->blur : RC->grey;
        for (int y = j->copy; y < j->copy + n; y++)
            memcpy(im->s.px + y * im->s.w, src + (y + im->y + (j->level ? ROW_HALO : 0)) * ROW_W + im->x, (size_t)im->s.w);
        j->copy += n; if (j->copy == im->s.h) j->phase = shutter_enabled ? 7 : 4;
        return;
    }
    if (j->phase == 7) {
        entry_image *im = &j->staged.image[0];
        unsigned bytes = (unsigned)im->s.w * (unsigned)(im->s.h + 1) * 2u;
        j->staged.prefix = row_alloc(bytes);
        if (!j->staged.prefix) { if (!row_deferred) row_failed(); return; }
        j->staged.prefix_bytes = bytes; j->copy = 0; j->phase = 8; return;
    }
    if (j->phase == 8) {
        entry_image *im = &j->staged.image[0];
        int end = j->copy + ROW_SLICE; if (end > im->s.h) end = im->s.h;
        settings_shutter_prefix_rows(im->s.px, j->staged.prefix, im->s.w, im->s.h, j->copy, end);
        j->copy = end;
        if (end == im->s.h) { j->level = 8; j->phase = 4; }
        return;
    }
    if (j->phase == 4) {
        if (shutter_enabled && !j->staged.image[0].s.px) j->level = 8;
        if (++j->level == 9) {
            entry_cache *r = &RC->entry[j->e];
            entry_free(r); j->staged.sig = j->sig; j->staged.ready = 1;
            *r = j->staged; memset(&j->staged, 0, sizeof j->staged);
            memset(j, 0, sizeof *j); row_bakes++; return;
        }
        memset(j->sum, 0, sizeof j->sum); j->row = 0;
        row_bounds_reset(j); j->phase = 5; return;
    }
    int len = row_blur_len[(j->level - 1)%4];
    if(j->phase==5){native_trail_kernel_make(&j->trail,len);j->row=0;j->phase=6;return;}
    for(int y=j->row;y<j->row+rows;y++)for(int x=0;x<ROW_W;x++)
        RC->blur[y*ROW_W+x]=(uint8_t)native_trail_sample(&j->trail,RC->grey,ROW_W,j->h,x,y-ROW_HALO,j->level<=4?1:-1);
    row_bounds(j, RC->blur, j->row, rows); j->row += rows;
    if (j->row == height) { j->phase = 2; j->row = 0; }
}
static int rows_choose(int home)
{
    int best = -1, best_score = 0;
    float off = home ? 0 : list_off();
    uint32_t room = nos_mem_available();
    for (int e = 0; e < NE; e++) {
        float y = LTOP + S.start[e] * UNIT - off;
        int h = units_of(E[e].kind) * UNIT;
        if (home && y >= GFX_H) continue;
        entry_cache *r = &RC->entry[e];
        if ((r->ready && r->sig == entry_sig(e)) || !entry_settled(e)) continue;
        if (nos_t < row_retry_at[e] && room <= row_retry_room[e]) continue;
        int visible = y < GFX_H && y + h > 0;
        float projected = home ? y : y - S.L.sc.v * 0.15f;
        int predicted = projected < GFX_H && projected + h > 0;
        int score = (visible ? 0 : predicted ? 50000 : 100000) +
                    (int)fminf(fabsf(y + h * 0.5f - UI_CY), fabsf(projected + h * 0.5f - UI_CY) + 30);
        if (best < 0 || score < best_score) { best = e; best_score = score; }
    }
    return best;
}
/* Desktop pixel references replay wall-clock cache work, including distinct
 * invocations in the same frame. Device budget decisions remain unchanged. */
#ifdef GFX_COST_MODEL
static unsigned bake_tape_frame=~0u,bake_tape_call;
#endif
/* SHARP samples level 0 only: once it is baked, skip the eight blur levels and publish. */
static void row_sharp_trim(void)
{
    if (NOS_RENDER_SHARP && RC->job.active && RC->job.phase == 4 && !RC->job.level) RC->job.level = 8;
}
static void rows_work(int home)
{
    if(settings_rom_active()){
        if(!rows_init())return;
        rows_palette();
        if(!home || !RC->rom_bound)rows_rom_sync();
        return;
    }
    if (!set_settings_bake_budget_us || !set_settings_bake_max_steps) return;
    uint32_t start = hal_micros();
#ifdef GFX_COST_MODEL
    if (bake_tape_frame != nos_frame) { bake_tape_frame=nos_frame; bake_tape_call=0; }
    unsigned tape_call=++bake_tape_call;
#endif
    for (unsigned n = 0; n < set_settings_bake_max_steps; n++) {
        row_deferred = 0;
        if (!rows_init()) return;
        rows_palette();
        if (!RC->lut_ready) rows_palette_step();
        else {
            if (!RC->job.active) { int e = rows_choose(home); if (e < 0) return; row_start(e); }
            row_step_job();
            row_sharp_trim();
        }
        if (row_deferred) return;
        int stop=hal_micros() - start >= set_settings_bake_budget_us;
#ifdef GFX_COST_MODEL
        if (nos_quality_decision) {
            /* Home plus15-bit invocation and16-bit step identify this decision. */
            if (tape_call>32767u || n>65535u) abort();
            stop=nos_quality_decision(nos_frame,3,((unsigned)!!home<<31)|(tape_call<<16)|n,stop);
        }
#endif
        if (stop) return;
    }
}
static void rows_open(void)
{
    /* Settings and these inactive blur arenas never need to be resident together.
     * The launcher retains its sharp and visible outgoing rows during the iris. */
    extern void sys_notifs_blur_release(void), sys_launcher_trim(void), orrery_cache_trim(void);
    ui_fblur_release(); pblur_release(); sys_notifs_blur_release();
    sys_launcher_trim(); orrery_cache_trim();
    rows_palette();
    entry_draw_valid=0;
    row_prev = floorf(ui_list_offset(&S.L) + 0.5f); row_step = 0; row_level = 0;
    native_previous=ui_list_offset(&S.L); native_pose=native_exposure=0;
    shutter_previous = ui_list_offset(&S.L); shutter_exposure = shutter_pose = 0;
    settings_shutter_kernel_make(&shutter_kernel, 0, 0);
    if (RC && RC->job.active) row_cancel();
    if(settings_rom_active() && rows_init()){rows_palette();controls_tick();}
}
static void rows_tick(float dt)
{
    float off = list_off(), fr = dt * M_FPS;
    float real=ui_list_offset(&S.L);
    native_pose=NOS_RENDER_SHARP?0:(int)lroundf((off-real)*256.0f);
    float travel=fabsf(real-native_previous)/(fr>0.5f?fr:1.0f);
    native_exposure=nos_mblur && !NOS_HELD_SHARP(S.L.dragging) ?
        native_smear_exposure(travel) : 0;
    native_direction=real>native_previous?-1:real<native_previous?1:native_direction;
    native_previous=real;
    controls_tick();
    if (shutter_enabled) {
        float real = ui_list_offset(&S.L), delta = shutter_previous - real;
        float exposure = isfinite(delta) && isfinite(dt) && dt > 0 ? delta / (dt * M_FPS * 2.0f) : 0;
        if (!isfinite(exposure)) exposure = 0;
        if (exposure > 48) exposure = 48;
        if (exposure < -48) exposure = -48;
        float phase = isfinite(real) ? off - real : 0;
        if (phase > 1) phase = 1;
        if (phase < -1) phase = -1;
        shutter_exposure = nos_mblur && !NOS_HELD_SHARP(S.L.dragging) ? (int)roundf(exposure * 256) : 0;
        shutter_pose = (int)roundf(phase * 256);
        settings_shutter_kernel_make(&shutter_kernel, shutter_exposure, shutter_pose);
        shutter_previous = real;
        shutter_stats.ticks++;
        shutter_stats.moving_ticks += shutter_exposure != 0;
    }

    row_step = fabsf(off - row_prev) / (fr > 0.5f ? fr : 1.0f); row_prev = off;
    row_level = !nos_mblur || NOS_HELD_SHARP(S.L.dragging) ? 0 : ui_fblur_level(row_step);
    rows_work(0);
    if (!row_level) nos_blur_note(NOS_BS_SETTINGS, S.L.dragging || row_step != 0, 0, 0);
}
void set_settings_prewarm(void)
{
    if (nos_top() != &scr_home || nos_transitioning() || !nos_mblur) return;
    if (!S.inited) { list_setup(); S.inited = 1; }
    S.ext = hal_ext_present();
    settings_sync_toggles();
    S.xf_ok = 0; update_values(); S.xf_ok = 1;
    rows_work(1);
}

static int entry_at(int y)
{
    float fy = y + list_off() - LTOP;
    if (fy < 0) return -1;
    int u = (int)(fy / UNIT);
    return u < S.nunits ? S.unit_e[u] : -1;
}

/* build-in: rows rise 14 px and fade in, one 80 px band a frame from the top down
 * (6 bands, R9), each over 5 frames: the rise on MF_STEADY5 at whole pixels, the ink in
 * perceptual steps. Returns the ink (0 = not yet drawn). */
#define BUILD_RISE 14.0f
static float build_k(float y, float *dy)
{
    if (row_baking || row_neutral) { *dy = 0; return 1.0f; }
    int band = (int)((y - 60.0f) / 80.0f);
    if (band < 0) band = 0;
    if (band > 5) band = 5;
    int k = m_stagger_k(m_frame_k(S.build_t0), band, 1);
    *dy = floorf((1.0f - mf_at(&MF_STEADY5, k)) * BUILD_RISE + 0.5f);
    return m_ink(k / 5.0f);
}

/* a value too wide to keep ROW_GAP to its label in the standard column goes under it */
static int value_under(const char *label, const char *v, const char *u)
{
    if (!v || !v[0]) return 0;
    return UI_ROW_X0 + ui_text_width(UI_F_BODY, label, 0) + ROW_GAP + value_w(v, u) > UI_VALUE_X;
}

/* A value swap `k` whole frames in (R4 at micro scale, "move less, fade more"): the old
 * value lifts VAL_LIFT px at constant speed and fades over VAL_OUT_F frames, then the new
 * one rises the same 8 px into place on MF_STEADY4 while its ink comes up perceptually.
 * Positions are whole pixels from the cumulative share (R6). */
static void val_swap(int k, float *ko, int *dyo, float *ki, int *dyi)
{
    float u = m_clamp(k / (float)(VAL_OUT_F + 1), 0, 1);
    *ko = k > VAL_OUT_F ? 0.0f : m_ink(1.0f - u);
    *dyo = -(int)floorf(VAL_LIFT * u + 0.5f);
    int j = k - VAL_OUT_F;
    *ki = j <= 0 ? 0.0f : m_ink(j / (float)(VAL_F - VAL_OUT_F));
    *dyi = (int)floorf(VAL_LIFT * (1.0f - mf_at(&MF_STEADY4, j)) + 0.5f);
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
    entry_geometry local;
    const entry_geometry *pose=entry_geometry_get(e,y,h,&local);
    float bk=pose->bk;
    if (bk <= 0) return;
    y += pose->dy;
    /* Authored 60/90/120px rows contain the header, value lifts, toggle,
     * and 88px arm pill. Unknown/custom heights retain the full renderer.
     * Build-in translation has already been applied to y. */
    if(!fb->rec && h==units_of(en->kind)*(UI_ROW_H/3.0f) &&
       ((int)ceilf(y+h)+4+fb->oy<=fb->y_base ||
        (int)floorf(y)-4+fb->oy>=fb->y_base+fb->h)) return;

    float cy = y + h * 0.5f;
    float k = pose->k;
    int lx = UI_ROW_X0, rx = UI_ROW_X1;

    if (en->kind == K_HDR) {
        ui_label(fb, lx, (int)(y + h - 16), en->label, ui_dim(C_DIS, k), UI_LEFT);
        return;
    }
    mtween plain;
    if (row_baking) tw_set(&plain, 0);
    if (en->kind == K_DANGER) {
        arm_pill(fb, cy, row_baking ? &plain : &S.arm_tw[1], "RESET SETTINGS", "TAP AGAIN TO RESET", S.arm_t, pressed,
                 row_baking ? 1.0f : 1.0f - q8(mtw_value(&S.grey_tw)), k, 0);
        return;
    }
    if (en->kind == K_ARM) {                         /* Restart: the power sheet's pill and morph */
        arm_pill(fb, cy, row_baking ? &plain : &S.arm_tw[0], "RESTART", "TAP AGAIN TO RESTART", S.arm_t,
                 pressed && mtw_value(&S.arm_tw[0]) <= 0, 1, k, 1);
        return;
    }
    if (pressed) ui_rrect(fb, lx - 20, y + 5, rx - lx + 40, h - 10, (h - 10) * 0.5f, C_SURF2);
    int id = en->id;
    int lw = ui_text_width(UI_F_BODY, en->label, 0);
    const int vx = rx - (UI_ROW_X1 - UI_VALUE_X);    /* value right edge (384 in the column) */
    int has_v = en->kind == K_NAV || en->kind == K_FLIP;
    int vk = has_v && !row_baking ? m_frame_k(S.vt0[id]) : VAL_F;      /* frames into a value swap */
    int prepared=!row_baking && entry_draw_current();
    int und = prepared ? entry_draw_revision[e].under :
        has_v && value_under(en->label, S.val[id], S.unit[id]);
    int und_o = prepared ? entry_draw_revision[e].old_under :
        has_v && vk < VAL_F && value_under(en->label, S.oval[id], S.ounit[id]);
    /* label line: centred, or raised 12 px when the value sits under it (across a swap) */
    float tl = vk >= VAL_F ? (float)und : m_lerp((float)und_o, (float)und, mf_at(&MF_STEADY6, vk));
    int bl = cap_base(UI_F_BODY, cy) - (int)floorf(tl * 12.0f + 0.5f);
    /* Fixed control and label columns, independent of the row's screen position. */
    int cl = rx;
    switch (en->kind) {
    case K_TOG: cl = rx - 76; break;
    case K_NAV: case K_FLIP: cl = (S.val[id][0] && !und) ? vx - value_w(S.val[id], S.unit[id]) : vx; break;
    default: break;
    }
    int tx = lx;
    if (!row_overlay)
        ui_text(fb, UI_F_BODY, tx, bl, en->label, ui_dim(C_PRI, k), UI_LEFT, 0);
    if (row_baking && entry_split(e)) return;
    switch (en->kind) {
    case K_TOG: {
        if (id == I_GPS) {
            ui_label(fb, rx, cap_base(UI_F_LABEL, cy), "N/A", ui_dim(C_DIS, k), UI_RIGHT);
            break;
        }
        int gap = cl - (tx + lw);
        float vk2 = k * (gap >= ROW_GAP ? 1.0f : q8((gap - 8) / (float)(ROW_GAP - 8)));
        if (vk2 > 0) toggle_k(fb, rx - 38, cy, (row_baking ? S.tog[id].to : mtw_value(&S.tog[id])), vk2, tog_absent(id));
        break;
    }
    case K_NAV: case K_FLIP:
        if (en->kind == K_NAV) chevron(fb, rx, cy, ui_dim(C_DIS, k));
        else flipglyph(fb, rx, cy, ui_dim(C_DIS, k));
        if (vk >= VAL_F) row_value(fb, S.val[id], S.unit[id], und, vx, tx, lw, cy, k, 1, 0);
        else {
            float ko, ki; int dyo, dyi;
            val_swap(vk, &ko, &dyo, &ki, &dyi);
            if (ko > 0) row_value(fb, S.oval[id], S.ounit[id], und_o, vx, tx, lw, cy, k, ko, dyo);
            if (ki > 0) row_value(fb, S.val[id], S.unit[id], und, vx, tx, lw, cy, k, ki, dyi);
        }
        break;
    default: break;
    }
}

/* Reusable compact control atlas. Each changed visible control is rasterized once
 * in tick; compositor stripes only sample its published bytes. Slots are pinned
 * for the entire visible set before an outgoing slot can be reused. */
static int control_combined(int e)
{
    if(entry_draw_current())return entry_draw_revision[e].combined;
    int id=E[e].id;
    return E[e].kind!=K_TOG &&
        (value_under(E[e].label,S.val[id],S.unit[id]) ||
         (m_frame_k(S.vt0[id])<VAL_F && value_under(E[e].label,S.oval[id],S.ounit[id])));
}
static unsigned control_sig(int e)
{
    int id=E[e].id;
    unsigned h=source_font_key(source_font_key(gfx_hash_u(0xc017u,(unsigned)e),UI_F_BODY),UI_F_LABEL);
    for(const char *p=S.val[id];*p;p++)h=gfx_hash_u(h,(unsigned char)*p);
    for(const char *p=S.unit[id];*p;p++)h=gfx_hash_u(h,(unsigned char)*p);
    int phase=m_frame_k(S.vt0[id]);if(phase>VAL_F)phase=VAL_F;
    h=gfx_hash_u(h,(unsigned)phase);
    if(phase<VAL_F){
        for(const char *p=S.oval[id];*p;p++)h=gfx_hash_u(h,(unsigned char)*p);
        for(const char *p=S.ounit[id];*p;p++)h=gfx_hash_u(h,(unsigned char)*p);
    }
    h=gfx_hash_u(h,(unsigned)lroundf(mtw_value(&S.tog[id])*65536.0f));
    return gfx_hash_u(h,(unsigned)tog_absent(id));
}
static void controls_tick(void)
{
    /* All source publications precede raster. Also covers repeated ticks in
     * one frame, without relying on a content hash as a mutation counter. */
    if(RC)native_carry_reset(&RC->carry_pool,nos_frame);
    entry_draw_prepare();
    control_current_mask=0;control_tick_frame=nos_frame;
    if(settings_rom_active() && !RC)rows_init();
    rows_palette();
    rows_rom_sync();
    if(!RC || !RC->lut_ready)return;
    float off=list_off();int wanted[NE]={0},pinned[8]={0};
    for(int e=0;e<NE;e++){
        float y=LTOP+S.start[e]*UNIT-off;
        int h=units_of(E[e].kind)*UNIT;
        wanted[e]=entry_split(e) && y<GFX_H && y+h>0;
        if(wanted[e])for(int i=0;i<8;i++)if(RC->controls[i].entry==e){pinned[i]=1;RC->controls[i].touched=nos_frame;}
    }
    /* Keep the nearest outgoing source on the reverse side while admitting
     * incoming controls. Visible slots always win; empty slots are used first. */
    int reverse=-1;float reverse_distance=1e9f;
    for(int i=0;i<8;i++)if(!pinned[i] && RC->controls[i].entry>=0){
        int e=RC->controls[i].entry;float y=LTOP+S.start[e]*UNIT-off;
        float end=y+units_of(E[e].kind)*UNIT;
        int side=S.L.sc.v>1?end<=0:S.L.sc.v< -1?y>=GFX_H:(end<=0 || y>=GFX_H);
        float distance=end<=0?-end:y-GFX_H;
        if(side && distance<reverse_distance){reverse=i;reverse_distance=distance;}
    }
    if(reverse>=0)pinned[reverse]=1;
    for(int e=0;e<NE;e++)if(wanted[e]){
        int slot=-1;
        for(int i=0;i<8;i++)if(RC->controls[i].entry==e){slot=i;break;}
        if(slot<0)for(int i=0;i<8;i++)if(!pinned[i] && RC->controls[i].entry<0){slot=i;break;}
        if(slot<0){
            unsigned oldest=0;float farthest=-1;
            for(int i=0;i<8;i++)if(!pinned[i]){
                int old=RC->controls[i].entry;unsigned age=nos_frame-RC->controls[i].touched;
                float distance=fabsf(LTOP+S.start[old]*UNIT-off-UI_CY);
                if(slot<0 || age>oldest || (age==oldest && distance>farthest)){slot=i;oldest=age;farthest=distance;}
            }
        }
        if(slot<0)continue; /* unusual custom geometry retains the live fallback */
        pinned[slot]=1;RC->controls[slot].touched=nos_frame;
        unsigned sig=control_sig(e);
        if(RC->controls[slot].entry==e && RC->controls[slot].sig==sig){
            control_current_mask|=1u<<slot;
            continue;
        }
        uint32_t started=hal_micros();
        int h=units_of(E[e].kind)*UNIT,combined=control_combined(e),id=E[e].id;
        int left=UI_ROW_X0;
        if(!combined){
            if(E[e].kind==K_TOG)left=UI_ROW_X1-78;
            else {
                int w=value_w(S.val[id],S.unit[id]);
                if(m_frame_k(S.vt0[id])<VAL_F){int old=value_w(S.oval[id],S.ounit[id]);if(old>w)w=old;}
                left=UI_VALUE_X-w-2;
                if(left>UI_ROW_X1-14)left=UI_ROW_X1-14;
                if(left<UI_ROW_X0)left=UI_ROW_X0;
            }
        }
        int origin=left-UI_ROW_X0,capture_w=ROW_W-origin;
        memset(RC->control_canvas,0,(size_t)capture_w*h*sizeof(uint16_t));
        gfx_fb fb={0};fb.px=RC->control_canvas;fb.stride=capture_w;fb.x_base=origin;fb.h=h;
        fb.ox=-UI_ROW_X0;fb.cx0=origin;fb.cx1=ROW_W;
#ifdef GFX_COST_MODEL
        fb.cost_psram=1;
#endif
        row_neutral=1;row_overlay=!combined;
        draw_entry(&fb,e,0,(float)h,0);
        row_neutral=row_overlay=0;
        int x0=capture_w,x1=-1,y0=h,y1=-1;
        for(int y=0;y<h;y++)for(int x=0;x<capture_w;x++)if(RC->control_canvas[y*capture_w+x]){
            if(x<x0)x0=x;
            if(x>x1)x1=x;
            if(y<y0)y0=y;
            if(y>y1)y1=y;
        }
        int w=x1-x0+1,rows=y1-y0+1;
        if(w<0)w=0;
        if(rows<0)rows=0;
        for(int y=0;y<rows;y++)for(int x=0;x<w;x++){
            unsigned green=(RC->control_canvas[(y+y0)*capture_w+x+x0]>>5)&63;
            RC->controls[slot].pixels[y*w+x]=(uint8_t)((green*255+31)/63);
        }
        RC->controls[slot].s.w=w;RC->controls[slot].s.h=rows;
        RC->controls[slot].x=x0+origin;RC->controls[slot].y=y0;
        RC->controls[slot].combined=combined;
        RC->controls[slot].sig=sig;RC->controls[slot].entry=e;
        control_current_mask|=1u<<slot;
        control_rasters++;control_work_pixels+=(unsigned)(capture_w*h);
        control_prepare_us+=hal_micros()-started;
    }
}
static int control_draw(gfx_fb *fb,int e,float y,float h)
{
    if(!RC || !RC->lut_ready || !entry_split(e) || control_tick_frame!=nos_frame)return 0;
    /* tick validated the immutable source once; recording and all output stripes
     * consume that publication without rehashing strings/tweens per draw call. */
    for(int i=0;i<8;i++)if((control_current_mask&(1u<<i)) && RC->controls[i].entry==e){
        entry_geometry local;
        const entry_geometry *pose=entry_geometry_get(e,y,h,&local);
        float dy=pose->dy;
        int lev=pose->lev;
        int x=UI_ROW_X0+RC->controls[i].x,iy=(int)(y+dy)+RC->controls[i].y;
        native_tile_blit_carry(fb,&RC->controls[i].s,NULL,0,x,iy,rows_ink(0,lev),gfx_hash_u(RC->controls[i].sig,(unsigned)lev),
                         native_pose,native_exposure,(int)(y+dy),(int)(y+dy+h),
                         settings_carry_enabled?&RC->carry_pool:NULL,nos_frame,RC->controls[i].sig,
                         NATIVE_CARRY_EXTERNAL,&settings_carry_stats);
        return 1;
    }
    return 0;
}

#ifdef GFX_COST_MODEL
/* motion audit: a build-in value is on the glass only once it shows (`vis`); its first
 * visible frame reports where it starts (`from`) as the move's origin */
static void probe_vis(const char *name, int cls, int vis, float px, float from, int *seen)
{
    if (!vis) { *seen = 0; return; }
    if (!*seen) nos_motion_probe(name, cls, from);
    *seen = 1;
    nos_motion_probe(name, cls, px);
}
#endif
/* motion audit (docs/NOS_SIM.md): the drawn offsets of what animates; sim only */
static void settings_probe(void)
{
#ifdef GFX_COST_MODEL
    static int seen[3];
    if (m_frame_k(S.build_t0) <= 1) seen[0] = seen[1] = 0;   /* a fresh build */
    float bdy, e0 = build_k(60.0f, &bdy);
    probe_vis("set.build0", MC_MICRO, e0 > 0, bdy, BUILD_RISE, &seen[0]);
    probe_vis("set.build0.ink", MC_FADE, e0 > 0, e0 * 100.0f, 0, &seen[1]);
    float e5 = build_k(460.0f, &bdy);
    probe_vis("set.build5", MC_MICRO, e5 > 0, bdy, BUILD_RISE, &seen[2]);
    if (S.ptog) nos_motion_probe("set.tog", MC_MICRO, 32.0f * m_clamp(mtw_value(&S.tog[S.ptog]), 0, 1));
    if (S.pval >= 0) {                       /* the last value swap: old out, new in */
        int k = m_frame_k(S.vt0[S.pval]);
        float ko, ki; int dyo, dyi;
        val_swap(k, &ko, &dyo, &ki, &dyi);
        if (k >= 1 && k <= VAL_OUT_F) {
            if (k == 1) { nos_motion_probe("set.value.out", MC_MICRO, 0); nos_motion_probe("set.value.out.ink", MC_FADE, 100); }
            nos_motion_probe("set.value.out", MC_MICRO, (float)dyo);
            nos_motion_probe("set.value.out.ink", MC_FADE, ko * 100.0f);
        }
        int j = k - VAL_OUT_F;
        if (j >= 1 && j <= VAL_F - VAL_OUT_F + 1) {
            if (j == 1) { nos_motion_probe("set.value.in", MC_MICRO, VAL_LIFT); nos_motion_probe("set.value.in.ink", MC_FADE, 0); }
            nos_motion_probe("set.value.in", MC_MICRO, (float)dyi);
            nos_motion_probe("set.value.in.ink", MC_FADE, (k >= VAL_F ? 1.0f : ki) * 100.0f);
        }
    }
    for (int j = 0; j < 2; j++) {
        float w = m_lerp(260.0f, 320.0f, m_clamp(mtw_value(&S.arm_tw[j]), 0, 1));
        nos_motion_probe(j ? "set.arm1.w" : "set.arm0.w", MC_MICRO, UI_CX - floorf(UI_CX - w * 0.5f + 0.5f));
    }
    nos_motion_probe("set.grey.ink", MC_FADE, q8(mtw_value(&S.grey_tw)) * 100.0f);
#endif
}

static int pressed_entry(void)
{
    if (S.flash_e >= 0 && nos_t - S.flash_t < PRESS_FLASH) return S.flash_e;
    if (S.L.pressed >= 0 && S.L.pressed < S.nunits && nos_t - S.L.press_t > PRESS_HOLD && nos_touch_travel() <= PRESS_TRAVEL) return S.unit_e[S.L.pressed];
    return -1;
}

/* The scene clears this opaque list column to C_BG before drawing entries.
 * Once every foreground contributor in an authored row is native, that native
 * row also owns its black backing. Protecting only the tight current ink tiles
 * lets history resurrect the previous pose in the black gaps between them. */
static void settings_backing_claim(gfx_fb *fb,int e,float y,float h)
{
    if(shutter_enabled || !native_exposure)return;
    entry_geometry local;
    const entry_geometry *pose=entry_geometry_get(e,y,h,&local);
    if(pose->bk>=1.0f && pose->dy==0)
        ui_fblur_coverage_claim(fb,UI_ROW_X0,(int)y,UI_ROW_X1+1,(int)(y+h));
}

static void settings_draw(gfx_fb *fb)
{
    float off = list_off();
    float content = (float)(LTOP + S.nunits * UNIT + S.L.bottom_pad);
    int all_blurred = 1;
    int pe = pressed_entry();
    for (int e = 0; e < NE; e++) {
        float h = (float)(units_of(E[e].kind) * UNIT);
        float y = LTOP + S.start[e] * UNIT - off;
        if (y + h + ROW_HALO < 0) continue;
        if (y - ROW_HALO > GFX_H) break;
        entry_cache *r = RC ? &RC->entry[e] : 0;
        /* Armed/pressed pills and active value/toggle tweens still draw live: their
         * settled sprite must never replace the state the user is interacting with. */
        /* Keep the same published raster at rest and on rounded0/1px scroll
         * steps. Its grayscale palette differs from live AA by up to1--2
         * RGB565 channel codes; switching every held frame made edges flicker.
         * A newly published row may make that small change once. */
        int combined=entry_split(e) && control_combined(e);
        if(e!=pe && combined && control_draw(fb,e,y,h)){
            settings_backing_claim(fb,e,y,h);
            continue;
        }
        int prepared=entry_draw_current();
        int settled=prepared?entry_draw_revision[e].settled:entry_settled(e);
        unsigned signature=prepared?entry_draw_revision[e].sig:entry_sig(e);
        entry_draw_hits+=prepared;
        int cached = e != pe && settled && !combined;
        if (shutter_enabled) {
            float dy;
            if (build_k(y, &dy) < 1.0f || dy != 0) cached = 0;
            if (r && r->image[0].s.px && !r->prefix) cached = 0;
        }
        if (cached && r && RC->lut_ready && r->ready && r->sig == signature) {
            const entry_image *im = &r->image[0];
            entry_geometry local;
            const entry_geometry *pose=entry_geometry_get(e,y,h,&local);
            float dy=pose->dy;
            int lev=pose->lev;
            /* Native static ink is clipped to this authored row. Controls,
             * backing ownership and fallback/blur diagnostics still execute
             * below, including carry invalidation on off-stripe calls. */
            if ((fb->rec || shutter_enabled ||
                 ((int)(y+dy+h)+ROW_HALO+fb->oy>fb->y_base &&
                  (int)(y+dy)-ROW_HALO+fb->oy<fb->y_base+fb->h)) && im->s.px && lev) {
                unsigned key = r->sig ^ (unsigned)(e * 64 + lev) ^ ((unsigned)row_level << 20);
                int ix = UI_ROW_X0 + im->x, iy = (int)(y + dy) + im->y;
                if (shutter_enabled && r->prefix) {
                    const uint16_t *palette = rows_ink(E[e].kind == K_DANGER,lev);
                    settings_shutter_coverage coverage = settings_shutter_blit(fb, im->s.px, r->prefix,
                        im->s.w, im->s.h, ix, iy, palette, &shutter_kernel,
                        shutter_exposure, shutter_pose, (int)(y + dy), (int)(y + dy) + (int)h, key);
                    shutter_stats.painted_pixels += coverage.pixels;
                    if (shutter_exposure && coverage.pixels) {
                        shutter_stats.filtered_pixels += coverage.pixels;
                        ui_fblur_coverage_claim(fb, coverage.x0, coverage.y0, coverage.x1, coverage.y1);
                    }
                } else {
                    int low,high;unsigned mix;
                    native_prepared_levels(native_exposure,&low,&high,&mix);
                    native_direction_levels(native_direction,&low,&high);
                    key=gfx_hash_u(gfx_hash_u(key,(unsigned)low),(unsigned)high);
                    const entry_image *a=&r->image[low],*b=&r->image[high];
                    native_prepared_blit(fb,&a->s,UI_ROW_X0+a->x,(int)(y+dy)+a->y,
                                         &b->s,UI_ROW_X0+b->x,(int)(y+dy)+b->y,mix,
                                         rows_ink(E[e].kind==K_DANGER,lev),key,native_pose,
                                         (int)(y+dy)-ROW_HALO,(int)(y+dy+h)+ROW_HALO,native_exposure!=0,NULL,0);
                }
            }
            int control_ready=!entry_split(e) || control_draw(fb,e,y,h);
            if (entry_split(e) && !control_ready) {
                row_overlay = 1;
                draw_entry(fb, e, y, h, 0);
                row_overlay = 0;
                if (row_level) all_blurred = 0;
            }
            if(control_ready)settings_backing_claim(fb,e,y,h);
        } else {
            if (shutter_enabled && !fb->rec) shutter_stats.fallback_calls++;
            if (row_level) all_blurred = 0;
            draw_entry(fb, e, y, h, e == pe && E[e].kind != K_HDR);
        }
    }
    if(!shutter_enabled && native_exposure){
        int first=(int)(LTOP-off),last=(int)(LTOP+S.nunits*UNIT-off);
        if(first>0)ui_fblur_coverage_claim(fb,UI_ROW_X0,0,UI_ROW_X1+1,first);
        if(last<GFX_H)ui_fblur_coverage_claim(fb,UI_ROW_X0,last,UI_ROW_X1+1,GFX_H);
    }
    if (row_level) nos_blur_note(NOS_BS_SETTINGS, 1, 1, all_blurred);
    ui_title(fb, "SETTINGS", off);
    ui_scrollbar(fb, S.L.sc.x, content, GFX_H, S.L.bar_alpha);
}

/* ---- sub-page declarations ---- */
static const nos_screen scr_set_pick, scr_set_slide, scr_set_time;
static void open_picker(int id);
static void open_slider(int id);

static void flip(int id)
{
    uint32_t request = settings_action(id, DIAG_ACT_SET, settings_value(id));
    if (id == I_WRIST) prefs_set_wrist(!nos_prefs.wrist_left, PREFS_WRIST_SOURCE_MANUAL);
    else if (id == I_THEME) nos_prefs.theme = !nos_prefs.theme;   /* the host inverts the panel next frame */
    else nos_prefs.units_metric = !nos_prefs.units_metric;
    if (id == I_WRIST) diag_action_context(request, PREFS_WRIST_SOURCE_MANUAL, (int32_t)prefs_wrist_revision());
    diag_action_result(request, DIAG_RESULT_SUCCESS, settings_value(id));
    prefs_save();
    update_values();                         /* the value swaps... */
    S.vt0[id] -= 1.0f / M_FPS;               /* ...from this very frame (input moves now) */
    nos_feedback(FB_TAP);
}

static void reset_all(void)
{
    uint32_t request = settings_action(I_RESET, DIAG_ACT_SET, 0);
    if (prefs_loading()) {
        diag_action_result(request, DIAG_RESULT_REJECTED, 0);
        nos_feedback(FB_ERROR);
        nos_toast("SETTINGS NOT READY");
        return;
    }
    prefs_defaults();
    prefs_save();
    int saved = prefs_flush();
    settings_sync_toggles();
    hal_set_backlight((uint8_t)(nos_prefs.brightness * 255 / 100));
    hal_audio_volume(nos_prefs.volume);
    hal_haptic_set_strength(nos_prefs.haptic_strength);
    diag_action_result(request, saved ? DIAG_RESULT_SUCCESS : DIAG_RESULT_PENDING, saved);
    nos_feedback(saved ? FB_SUCCESS : FB_ERROR);
    nos_toast(saved ? "RESET" : "RESET SAVE PENDING");
    update_values();
}

static void activate(int e)
{
    const sentry *en = &E[e];
    if (en->kind != K_ARM && en->kind != K_DANGER) S.arm_e = -1;
    switch (en->kind) {
    case K_HDR: return;
    case K_TOG: {
        if (en->id == I_GPS) {
            uint32_t request = settings_action(en->id, DIAG_ACT_SET, nos_prefs.gps_auto);
            diag_action_result(request, DIAG_RESULT_REJECTED, 0);
            nos_feedback(FB_LIMIT);
            nos_toast("GPS AUTO UNAVAILABLE");
            return;
        }
        uint8_t *p = tog_pref(en->id);
        if (!p) return;
        int on = !*p;
        uint32_t request = settings_action(en->id, DIAG_ACT_SET, *p);
        if (!on) nos_feedback(FB_TOGGLE_OFF);   /* felt before haptics/sound turn off */
        *p = (uint8_t)on;
        diag_action_result(request, DIAG_RESULT_SUCCESS, on);
        if (on) nos_feedback(FB_TOGGLE_ON);
        tw_to(&S.tog[en->id], on ? 1.0f : 0.0f, &MF_STEADY4, 1);
        S.ptog = en->id;
        nos_motion_probe("set.tog", MC_MICRO, 32.0f * S.tog[en->id].from);   /* the audit's origin */
        prefs_save();
        if (tog_absent(en->id)) nos_toast(tog_missing(en->id));   /* saved, but nothing is live */
        return;
    }
    case K_FLIP: flip(en->id); return;
    case K_ARM: case K_DANGER:
        if (S.arm_e != e) {
            S.arm_e = e; S.arm_t = nos_t; nos_feedback(FB_LIMIT);
            int r = en->id == I_RESET;
            tw_to(&S.arm_tw[r], 1.0f, &MF_STEADY5, 1);
            tw_to(&S.arm_tw[!r], 0.0f, &MF_STEADY5, 1);
            tw_to(&S.grey_tw, r ? 0.0f : 1.0f, &MF_STEADY5, 1);
            return;
        }
        S.arm_e = -1;
        if (en->id == I_RESET) reset_all();
        else {
            prefs_save();
            if (!prefs_flush()) {
                nos_feedback(FB_ERROR);
                nos_toast("PREFERENCES SAVE FAILED");
                return;
            }
            nos_feedback(FB_ERROR);
            nos_toast("RESTARTING");
            S.restart_at = nos_t + 0.8f;
        }
        return;
    default: break;
    }
    nos_feedback(FB_TAP);
    switch (en->id) {
    case I_BRIGHT: case I_AODB: case I_VOLUME: case I_HSTR: open_slider(en->id); break;
    case I_LEARN: {                          /* hold the selected wrist still, ~1 s */
        wake_info w; wake_service_get(&w);
        int wrist = nos_prefs.wrist_left ? 0 : 1;
        /* Also synchronize at the learn boundary: two inputs can arrive before
         * the host's next preference pass. A failed save must not learn the
         * other hand's pose. */
        if (w.wrist != wrist && !wake_service_set("wrist", wrist)) {
            nos_feedback(FB_ERROR);
            nos_toast("WRIST NOT SAVED: RETRY");
            break;
        }
        wake_service_learn();
        S.learn_wait = 1;
        nos_toast(wrist ? "HOLD RIGHT WRIST STILL" : "HOLD LEFT WRIST STILL");
        break;
    }
    case I_TIMEOUT: case I_TZ: case I_GOAL: open_picker(en->id); break;
    case I_BT: case I_WIFI: set_settings_open_radio(en->id == I_BT, 0); break;
    case I_FACE: nos_push(&scr_faces, TX_PUSH); break;
    case I_CELL: nos_push(&scr_cell, TX_PUSH); break;   /* the one modem page: the Cellular app */
    case I_SETTIME: nos_push(&scr_set_time, TX_PUSH); break;
    case I_MODULES: nos_push(&scr_modules, TX_PUSH); break;
    case I_BATTERY: nos_push(&scr_battery, TX_PUSH); break;
    case I_STORAGE: set_modules_open_sd(); break;
    case I_ABOUT: nos_push(&scr_about, TX_PUSH); break;
    }
}

static int settings_input(const nos_input *in)
{
    if(in->type==IN_CANCEL) {S.was_moving=0;S.arm_e=-1;return ui_list_input(&S.L,in);}
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
                                  settings_draw, settings_input, SCR_NATIVE_CACHE };

/* ======================================================================= picker page */
static struct {
    int id;
    pk p;
    pk_txt tx;
    char extra[24];
    int pressed;            /* -1 none, 0 prev, 1 next */
    nos_time_t t0;
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
        /* Firmware applies tz_min to its local calendar every loop. Preview
         * from UTC so it is correct both before and after that host pass. */
        uint64_t utc = hal_epoch_seconds();
        if (!utc) { snprintf(o, (size_t)n, "TIME NOT SYNCED"); return; }
        int m = (int)((utc / 60) % 1440) + k_tz[i];
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
    int old = settings_value(P.id);
    int next = P.id == I_TIMEOUT ? k_timeout[i] : P.id == I_TZ ? k_tz[i] : (2000 + i * 500) / 100;
    if (old == next) return;
    uint32_t request = settings_action(P.id, DIAG_ACT_SET, old);
    if (P.id == I_TIMEOUT) nos_prefs.sleep_s = k_timeout[i];
    else if (P.id == I_TZ) nos_prefs.tz_min = k_tz[i];
    else nos_prefs.step_goal_100 = (uint16_t)((2000 + i * 500) / 100);
    diag_action_result(request, DIAG_RESULT_SUCCESS, settings_value(P.id));
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
    P.p.probe = "pick.drum";
    P.pressed = -1;
    P.t0 = nos_enter_t();
    pick_texts();
}
static void pick_leave(void)
{
    /* Offset selection changes display preferences only. The host owns TZ;
     * neither an exit nor a covering screen may write the UTC/RTC clock. */
    P.pressed = -1;
}
static void pick_tick(float dt)
{
    if (pk_tick(&P.p, dt)) pick_commit();
    pick_texts();
#ifdef GFX_COST_MODEL
    pk_probe(P.p.probe, &P.p);
#endif
}

/* the drum window: values are clipped to it, the labels below stay clear */
#define DRUM_Y0 96
#define DRUM_Y1 362
static void pick_draw(gfx_fb *fb)
{
    ui_title(fb, pick_title(P.id), 0);
    float bp = m_clamp((nos_t - P.t0) / UI_ENTRY_S, 0, 1);
    pk_ruler(fb, &P.p, 70, UI_CY);
    pk_ruler(fb, &P.p, GFX_W - 70, UI_CY);
    gfx_fb sub;
    if (nos_subview(fb, &sub, 0, DRUM_Y0, GFX_W, DRUM_Y1, 0, 0)) {
        int hl = P.pressed == 0 ? P.p.idx - 1 : P.pressed == 1 ? P.p.idx + 1 : -9;
        pk_column(&sub, &P.p, &P.tx, UI_CX, UI_CY, bp, 7, hl, DRUM_Y0, DRUM_Y1);
    }
    ui_label(fb, UI_CX, 376, pick_unit(P.id), C_SEC, UI_CENTER);
    if (P.extra[0]) {
        if (P.id == I_GOAL) ui_label_unit(fb, UI_CX, 402, P.extra, nos_prefs.units_metric ? "km" : "mi", C_SEC, UI_CENTER);
        else ui_label(fb, UI_CX, 402, P.extra, C_SEC, UI_CENTER);
    }
}

static int pick_input(const nos_input *in)
{
    if(in->type==IN_CANCEL) {P.pressed=-1;P.p.dragging=0;pk_go(&P.p,P.p.idx*PK_STEP,0);return 1;}
    switch (in->type) {
    case IN_DOWN:
        pk_grab(&P.p);
        P.p.dragging = 0;                      /* hold still; a drag re-grabs */
        P.p.moving = 0;
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
        if (!P.p.dragging && !P.p.moving) pk_go(&P.p, P.p.idx * PK_STEP, 0);
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
    int touch;               /* a finger is on the track: it shows pressed, as in the control centre */
    mtween tw;               /* a tap glides the fill to its detent (EASE_STEADY, 5-9 frames) */
    int animating;
    int shown;               /* integer shown / committed */
    int trace_old;           /* one semantic result per completed gesture */
    mtext hero;
    mtween tog;              /* the Auto toggle: 4 frames on MF_STEADY4 */
    int pressed_row; nos_time_t row_flash;
    float lux; int have_lux;
    nos_time_t t0;
} SL;

static uint8_t *slide_pref(int id)
{
    return id == I_BRIGHT ? &nos_prefs.brightness : id == I_AODB ? &nos_prefs.aod_bright :
           id == I_VOLUME ? &nos_prefs.volume : &nos_prefs.haptic_strength;
}
static float slide_min(void) { return SL.id == I_BRIGHT ? 5.0f : SL.id == I_AODB ? 1.0f : 0.0f; }   /* never a black screen */
static int slide_hw(void)
{
    uint32_t m = hal_ext_present();
    return SL.id == I_BRIGHT || SL.id == I_AODB ? 1 : SL.id == I_VOLUME ? (m & EXT_SPEAKER) != 0 : (m & EXT_HAPTIC) != 0;
}
static int bright_auto_row(void) { return SL.id == I_BRIGHT && (hal_ext_present() & EXT_LIGHT); }
static float slide_y(void) { return bright_auto_row() ? 214.0f : 250.0f; }
static float slide_hero_y(void) { return bright_auto_row() ? 100.0f : 132.0f; }

static void slider_draw(gfx_fb *fb, float x, float y, float w, float h, float v, int icon, int live)
{
    if (live) { ui_slider_p(fb, x, y, w, h, v, icon, SL.touch || SL.dragging); return; }
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
    else if (SL.id == I_AODB) nos_brightness_override(prefs_pct_level(v));   /* the preview */
    else if (SL.id == I_VOLUME) {
        hal_audio_volume(v);
        if (s10 != o10) { hal_sfx(SFX_TICK); if (nos_prefs.haptics) hal_haptic(HAPTIC_TICK); }
    } else {
        hal_haptic_set_strength(v);
        if (s10 != o10) hal_haptic(HAPTIC_CLICK);
    }
    if ((SL.id == I_BRIGHT || SL.id == I_AODB) && s10 != o10) nos_feedback(FB_TICK);
    if ((v == (int)slide_min() || v == 100) && v != old) nos_feedback(FB_LIMIT);
}

static void slide_trace(int outcome)
{
    int value = *slide_pref(SL.id);
    if (value == SL.trace_old) return;
    uint32_t request = settings_action(SL.id, DIAG_ACT_SET, SL.trace_old);
    diag_action_result(request, outcome, value);
    SL.trace_old = value;
}
static void slide_enter(void)
{
    SL.v = *slide_pref(SL.id);
    SL.trace_old = (int)SL.v;
    SL.shown = (int)SL.v;
    SL.dragging = SL.animating = SL.touch = 0;
    SL.pressed_row = 0;
    SL.row_flash = -100;
    char b[8]; snprintf(b, sizeof b, "%d%%", SL.shown);
    mt_init(&SL.hero, b, M_FRAMES(4));
    tw_set(&SL.tog, nos_prefs.auto_bright ? 1.0f : 0.0f);
    SL.t0 = nos_enter_t();
    /* always-on brightness: the whole page shows at that level while it is open, so it is
     * set by eye in the room it will be used in */
    if (SL.id == I_AODB) nos_brightness_override(prefs_pct_level(nos_prefs.aod_bright));
}
static void slide_leave(void)
{
    slide_trace(SL.animating || SL.dragging || SL.touch ? DIAG_RESULT_CANCELLED : DIAG_RESULT_SUCCESS);
    SL.animating = SL.dragging = SL.touch = 0;
    SL.v = *slide_pref(SL.id);
    if (SL.id == I_AODB) nos_brightness_override(-1);   /* back to the user's level */
}
static void slide_tick(float dt)
{
    (void)dt;
    if (SL.animating) {
        SL.v = mtw_value(&SL.tw);
        if (mtw_done(&SL.tw)) SL.animating = 0;
    }
    int v = (int)lroundf(m_clamp(SL.v, 0, 100));
    if (v != SL.shown) { int old = SL.shown; SL.shown = v; slide_apply(v, old); }
    if (!SL.touch && !SL.dragging && !SL.animating) slide_trace(DIAG_RESULT_SUCCESS);
    /* the hero follows the finger value by value; a tap's glide shows its target at once
     * (one morph, R8) while the fill and the hardware level travel */
    char b[8]; snprintf(b, sizeof b, "%d%%", SL.animating ? (int)lroundf(SL.tw.to) : SL.shown);
    mt_set(&SL.hero, b);
    hal_light l;
    SL.have_lux = hal_light_get(&l) && l.valid;
    SL.lux = SL.have_lux ? l.lux : 0;
#ifdef GFX_COST_MODEL
    {   /* a tap's glide: a micro move up to 48 px (5-6 frames), a component beyond */
        int short_hop = fabsf(SL.tw.to - SL.tw.from) * (SL_W - SL_H) / 100.0f <= 48.0f;
        nos_motion_probe("slide.fill", SL.dragging ? MC_FINGER : short_hop ? MC_MICRO : MC_COMPONENT,
                         (SL_W - SL_H) * m_clamp(SL.v, 0, 100) / 100.0f);
    }
    if (bright_auto_row()) nos_motion_probe("slide.tog", MC_MICRO, 32.0f * m_clamp(mtw_value(&SL.tog), 0, 1));
#endif
}
static void slide_draw(gfx_fb *fb)
{
    const char *title = SL.id == I_BRIGHT ? "BRIGHTNESS" : SL.id == I_AODB ? "ALWAYS-ON" : SL.id == I_VOLUME ? "VOLUME" : "HAPTIC STRENGTH";
    int icon = SL.id == I_BRIGHT || SL.id == I_AODB ? DOT_ICON_BRIGHT : SL.id == I_VOLUME ? DOT_ICON_SPEAKER : DOT_ICON_HAND;
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
        toggle_k(fb, 324, cy, mtw_value(&SL.tog), 1.0f, 0);
    } else if (SL.id == I_BRIGHT) note = no_module(EXT_LIGHT);
    else if (SL.id == I_AODB) note = "SHOWN AT THIS LEVEL";
    else if (SL.id == I_VOLUME && !slide_hw()) note = no_module(EXT_SPEAKER);
    else if (SL.id == I_VOLUME && !nos_prefs.sound) note = "UI SOUNDS OFF";
    else if (SL.id == I_HSTR && !slide_hw()) note = no_module(EXT_HAPTIC);
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
    if(in->type==IN_CANCEL) {
        SL.pressed_row=SL.touch=SL.dragging=SL.animating=0;
        SL.v=*slide_pref(SL.id); /* cancel cannot advance a deferred glide */
        slide_trace(DIAG_RESULT_CANCELLED);
        return 1;
    }
    switch (in->type) {
    case IN_DOWN:
        SL.pressed_row = bright_auto_row() && in->y > 310 && in->y < 400;
        SL.touch = in_slider(in->x, in->y);
        return 1;
    case IN_DRAG_START:
        SL.pressed_row = 0;
        if (in->axis != AXIS_X || !in_slider(in->x0, in->y0)) { SL.touch = 0; return 0; }
        SL.dragging = 1; SL.animating = 0;
        SL.v0 = m_clamp(SL.v, 0, 100);
        SL.v = m_clamp(SL.v0 + in->dx / (SL_W - SL_H) * 100.0f, slide_min(), 100);
        return 1;
    case IN_DRAG:
        if (!SL.dragging) return 0;
        SL.v = m_clamp(SL.v0 + in->dx / (SL_W - SL_H) * 100.0f, slide_min(), 100);
        return 1;
    case IN_DRAG_END:
        SL.touch = 0;
        if (!SL.dragging) return 0;
        SL.dragging = 0;
        return 1;
    case IN_TAP:
        if (in_slider(in->x, in->y)) {
            /* taps land on the 10 % detents the haptics mark; the fill glides there on
             * EASE_STEADY, 5 frames for a short hop up to 9 across the track (<= 33 px a frame) */
            float to = m_clamp(floorf(slide_val_at(in->x) / 10.0f + 0.5f) * 10.0f, slide_min(), 100);
            float px = fabsf(to - SL.v) * (SL_W - SL_H) / 100.0f;
            if (px >= 0.5f) {
                int n = 5 + (int)(px / 46.0f + 0.5f);
                mtw_start(&SL.tw, SL.v, to, M_FRAMES(n > 9 ? 9 : n), &EASE_STEADY);
                mtw_lead(&SL.tw);
                SL.animating = 1;
                SL.v = mtw_value(&SL.tw);
            }
            nos_feedback(FB_TAP);
        } else if (SL.pressed_row) {
            int on = !nos_prefs.auto_bright;
            uint32_t request = settings_action(I_BRIGHT, DIAG_ACT_TRANSITION, nos_prefs.auto_bright);
            nos_prefs.auto_bright = (uint8_t)on;
            nos_feedback(on ? FB_TOGGLE_ON : FB_TOGGLE_OFF);
            tw_to(&SL.tog, on ? 1.0f : 0.0f, &MF_STEADY4, 1);
            diag_action_result(request, DIAG_RESULT_SUCCESS, on);
            prefs_save();
            SL.row_flash = nos_t;
        }
        SL.pressed_row = 0;
        SL.touch = 0;
        return 1;
    case IN_UP:
        SL.pressed_row = 0;
        SL.touch = 0;
        return 1;
    default: return 0;
    }
}
static const nos_screen scr_set_slide = { "SET_SLIDE", slide_enter, slide_leave, slide_tick, slide_draw, slide_input, SCR_HDRAG };

/* Connectivity pages are shared with Modules. */
void set_connectivity_open(int bt);
void set_settings_open_radio(int bt, int from_modules)
{
    (void)from_modules;
    set_connectivity_open(bt);
}

/* ======================================================================= set time */
#define TCOL 76.0f
#define TCY  196.0f              /* the drums sit high: the SET button owns the thumb row */
#define TDRUM_Y0 82
#define TDRUM_Y1 322
static struct { pk h, m; pk_txt ht, mt; int pressed; nos_time_t flash_t; int active; nos_time_t t0; } T;

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
    T.h.probe = "time.h";
    T.m.probe = "time.m";
    T.pressed = 0; T.active = -1; T.flash_t = -100;
    T.t0 = nos_enter_t();
    time_texts();
}
static void time_tick(float dt)
{
    pk_tick(&T.h, dt);
    pk_tick(&T.m, dt);
    time_texts();
#ifdef GFX_COST_MODEL
    pk_probe(T.h.probe, &T.h);
    pk_probe(T.m.probe, &T.m);
#endif
}
static void time_draw(gfx_fb *fb)
{
    ui_title(fb, "SET TIME", 0);
    pk_ruler(fb, &T.h, 64, TCY);
    pk_ruler(fb, &T.m, GFX_W - 64, TCY);
    float bp = m_clamp((nos_t - T.t0) / UI_ENTRY_S, 0, 1);
    gfx_fb sub;
    if (nos_subview(fb, &sub, 0, TDRUM_Y0, GFX_W, TDRUM_Y1, 0, 0)) {
        pk_column(&sub, &T.h, &T.ht, UI_CX - TCOL, TCY, bp, 5, -9, TDRUM_Y0, TDRUM_Y1);
        pk_column(&sub, &T.m, &T.mt, UI_CX + TCOL, TCY, bp, 90, -9, TDRUM_Y0, TDRUM_Y1);
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
    if(in->type==IN_CANCEL) {T.pressed=0;T.active=-1;T.h.dragging=T.m.dragging=0;pk_go(&T.h,T.h.idx*PK_STEP,0);pk_go(&T.m,T.m.idx*PK_STEP,0);return 1;}
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
            uint32_t request = settings_action(I_SETTIME, DIAG_ACT_SET, 0);
            int accepted = hal_set_datetime_checked(&dt);
            diag_action_result(request, accepted ? DIAG_RESULT_SUCCESS : DIAG_RESULT_REJECTED, accepted);
            if (accepted) {
                nos_feedback(FB_SUCCESS);
                nos_toast("TIME SET");
                nos_pop();
            } else {
                nos_feedback(FB_ERROR);
                nos_toast(dt.year < 2000 || dt.year > 2099 ?
                          "DATE NOT SET: SYNC PHONE" : "TIME NOT SET: TRY AGAIN");
            }
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

void set_settings_scroll_snapshot(nos_scroll_snapshot *s)
{
    *s=(nos_scroll_snapshot){8,2,0,1,S.L.dragging || S.L.sc.dragging,ui_list_offset(&S.L),S.L.sc.max};
}
