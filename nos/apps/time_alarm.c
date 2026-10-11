/* time_alarm.c — Alarms: list (next-alarm summary, per-row toggles, add), editor (hour /
 * minute drums, day chips, delete) and the ringing screen (slide-to-stop, snooze).
 * Team: time.
 *
 * State lives in svc (svc_alarm_*). While svc is the link stub (svc_alarm_add returns
 * -1 with room left) the list falls back to a local table so the app stays usable;
 * fallback alarms are not fired (that is the alarm service's job). */
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

/* from time_timer.c (exact-position trig and dot ring) */
float time_sin(float x);
float time_cos(float x);
void time_ring(gfx_fb *fb, float cx, float cy, float r, int n, float d, float lit, uint16_t on, uint16_t off);

/* from time_timer.c */
void tdrum_setup(int id, int lo, int hi, int value, float row, float pc, float pn);
void tdrum_place(int id, float cx, float cy);
int  tdrum_value(int id);
int  tdrum_hit(int id, int x, int y);
int  tdrum_input(int id, const nos_input *in);
void tdrum_tick(int id, float dt);
void tdrum_draw(gfx_fb *fb, int id, float build);
void time_pulse_draw(gfx_fb *fb, float phase, int rings);

static float qz(float v, float n) { return floorf(v * n + 0.5f) / n; }
static int in_rect(int x, int y, float cx, float cy, float w, float h)
{
    return fabsf(x - cx) <= w * 0.5f && fabsf(y - cy) <= h * 0.5f;
}
static int in_circle(int x, int y, float cx, float cy, float r)
{
    float dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

/* ===================================================== state + fallback */
static svc_alarm AL[SVC_ALARM_MAX];
static int AL_n;
static int a_svc = -1;           /* -1 unknown, 0 stub (local table), 1 real */

static int al_count(void)
{
    if (a_svc < 0 && svc_alarm_count() > 0) a_svc = 1;
    return a_svc == 0 ? AL_n : svc_alarm_count();
}
static svc_alarm *al_get(int i) { return a_svc == 0 ? &AL[i] : svc_alarm_get(i); }
static int al_add(int h, int m)
{
    int r = svc_alarm_add(h, m);
    if (r >= 0) { a_svc = 1; }
    else {
        if (a_svc == 1 || svc_alarm_count() >= SVC_ALARM_MAX) return -1;
        a_svc = 0;
        if (AL_n >= SVC_ALARM_MAX) return -1;
        memset(&AL[AL_n], 0, sizeof AL[0]);
        AL[AL_n].h = (uint8_t)h; AL[AL_n].m = (uint8_t)m;
        r = AL_n++;
    }
    svc_alarm *a = al_get(r);
    a->on = 1; a->days = 0;
    return r;
}
static void al_remove(int i)
{
    if (a_svc == 0) {
        for (int k = i; k < AL_n - 1; k++) AL[k] = AL[k + 1];
        if (AL_n > 0) AL_n--;
    } else svc_alarm_remove(i);
}
static void al_save(void) { if (a_svc != 0) svc_alarm_save(); }

/* minutes from now to the alarm's next occurrence (ignores on/off) */
static int al_in_min(const svc_alarm *a)
{
    hal_datetime dt;
    hal_get_datetime(&dt);
    int now = dt.hour * 60 + dt.minute, t = a->h * 60 + a->m;
    for (int d = 0; d <= 7; d++) {
        int mins = d * 1440 + t - now;
        if (mins <= 0) continue;
        int wd = (dt.weekday + d) % 7;
        if (!a->days || (a->days >> wd & 1)) return mins;
    }
    return 7 * 1440;
}
int time_alarm_next(int *h, int *m, int *in_min)
{
    if (a_svc != 0) return svc_alarm_next(h, m, in_min);
    int best = -1, bm = 1 << 30;
    for (int i = 0; i < AL_n; i++) {
        if (!AL[i].on) continue;
        int mm = al_in_min(&AL[i]);
        if (mm < bm) { bm = mm; best = i; }
    }
    if (best < 0) return 0;
    if (h) *h = AL[best].h;
    if (m) *m = AL[best].m;
    if (in_min) *in_min = bm;
    return 1;
}

static void fmt_in(char *b, int n, int mins)
{
    if (mins >= 1440) snprintf(b, (size_t)n, "IN %dD %dH", mins / 1440, mins % 1440 / 60);
    else if (mins >= 60) snprintf(b, (size_t)n, "IN %dH %dM", mins / 60, mins % 60);
    else snprintf(b, (size_t)n, "IN %d MIN", mins < 1 ? 1 : mins);
}
static void fmt_days(char *b, int n, int days)
{
    static const char *two[7] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };
    days &= 0x7F;
    if (!days) { snprintf(b, (size_t)n, "ONCE"); return; }
    if (days == 0x7F) { snprintf(b, (size_t)n, "EVERY DAY"); return; }
    if (days == 0x3E) { snprintf(b, (size_t)n, "WEEKDAYS"); return; }
    if (days == 0x41) { snprintf(b, (size_t)n, "WEEKENDS"); return; }
    int cnt = 0;
    for (int i = 0; i < 7; i++) cnt += days >> i & 1;
    b[0] = 0;
    for (int k = 1; k <= 7; k++) {               /* Monday first */
        int i = k % 7;
        if (!(days >> i & 1)) continue;
        size_t l = strlen(b);
        snprintf(b + l, (size_t)n - l, "%s%s", l ? " " : "", cnt <= 3 ? ui_weekday3(i) : two[i]);
    }
}
/* hour display for the list (12 h pref aware) */
static void fmt_hm(char *b, int n, int h, int m) { ui_fmt_time(b, n, h, m); }

/* ============================================================== list */
#define L_TOP    124.0f
#define L_ROW    100.0f
#define L_CARD   88.0f
#define L_MARGIN 30.0f
static struct {
    float t0, build;
    mscroll sc;
    int dragging, was_moving;
    int pressed, press_part;     /* row (n = add row), part 0 body 1 toggle */
    mspring tog[SVC_ALARM_MAX];
    int n;
    char next[32];
    int have_next;
    int empty_pressed;
} A;
enum { PART_BODY = 0, PART_TOGGLE };

static float list_max(void)
{
    float content = L_TOP + (A.n + 1) * L_ROW + 70;
    return content > GFX_H ? content - GFX_H : 0;
}
static void list_sync(void)
{
    A.n = al_count();
    msc_bounds(&A.sc, 0, list_max());
    int h, m, in;
    A.have_next = time_alarm_next(&h, &m, &in);
    if (A.have_next) {
        char t[16];
        fmt_in(t, sizeof t, in);
        snprintf(A.next, sizeof A.next, "NEXT %s", t);
    } else snprintf(A.next, sizeof A.next, "ALL ALARMS OFF");
}

static float s_list_seen = -10;
static void alarm_enter(void)
{
    A.pressed = -1; A.dragging = 0; A.empty_pressed = 0;
    int back = nos_t - s_list_seen < 0.2f;      /* revealed by a pop: keep scroll + no build */
    A.n = al_count();
    if (!back) { A.t0 = nos_t; A.build = 0; msc_init(&A.sc, 0, list_max(), 0); }
    for (int i = 0; i < A.n && i < SVC_ALARM_MAX; i++) msp_init(&A.tog[i], al_get(i)->on ? 100.0f : 0.0f, SPRING_SNAPPY);
    list_sync();
}
static void alarm_tick(float dt)
{
    s_list_seen = nos_t;
    int was = A.n;
    list_sync();
    if (A.n != was)
        for (int i = 0; i < A.n; i++) msp_set(&A.tog[i], al_get(i)->on ? 100.0f : 0.0f);
    for (int i = 0; i < A.n; i++) {
        msp_to(&A.tog[i], al_get(i)->on ? 100.0f : 0.0f);   /* percent: springs settle at 0.25 units */
        msp_step(&A.tog[i], dt);
    }
    msc_step(&A.sc, dt);
    A.build = m_clamp((nos_t - A.t0) / 0.4f, 0, 1);
}

static float row_top(int i) { return L_TOP + i * L_ROW - floorf(A.sc.x + 0.5f); }
/* Cards follow the circle at their centre line (never narrower than the content);
 * content sits on fixed anchors so rows near the bezel are clipped by the glass
 * instead of squeezing together. */
#define L_TIME_X  (UI_CX - 142.0f)
#define L_TOG_X   (UI_CX + 112.0f)
static void row_span(float y, int *x0, int *x1)
{
    ui_row_span(y + 6 + L_CARD * 0.5f, L_MARGIN, x0, x1);
    if (*x0 > UI_CX - 172) *x0 = UI_CX - 172;
    if (*x1 < UI_CX + 172) *x1 = UI_CX + 172;
}
static float row_fade(float y)
{
    float d = fabsf(y + L_ROW * 0.5f - UI_CY);
    return qz(1.0f - 0.7f * m_smoothstep(130.0f, 230.0f, d), 16);
}

#define EMPTY_BTN_Y 334.0f
static void alarm_draw(gfx_fb *fb)
{
    float b = A.build;
    float off = floorf(A.sc.x + 0.5f);
    ui_title(fb, "ALARMS", off);
    if (A.n == 0) {
        dot_icon_anim(fb, DOT_ICON_ALARM, UI_CX, 176, 5.0f, 4.0f, C_SEC, b, DOT_REVEAL_CENTER);
        float k = qz(m_clamp(b * 2 - 0.5f, 0, 1), 8);
        if (k <= 0) return;
        ui_text(fb, UI_F_BODY, UI_CX, 266, "No alarms", ui_dim(C_PRI, k), UI_CENTER, 0);
        ui_rrect(fb, UI_CX - 100, EMPTY_BTN_Y - 44, 200, 88, 44, ui_dim(A.empty_pressed ? C_SEC : C_WHITE, k));
        ui_label(fb, UI_CX, (int)EMPTY_BTN_Y + 6, "ADD ALARM", C_BG, UI_CENTER);
        return;
    }
    float sk = qz(m_clamp(b * 2, 0, 1), 8) * (1.0f - m_clamp(off / 50.0f, 0, 1));
    if (sk > 0.02f) ui_label(fb, UI_CX, (int)(102 - off), A.next, ui_dim(A.have_next ? C_PRI : C_DIS, qz(sk, 8)), UI_CENTER);
    for (int i = 0; i <= A.n; i++) {
        float y = row_top(i);
        if (y + L_ROW < 0) continue;
        if (y > GFX_H) break;
        float p = m_stagger(b, i < 7 ? i : 7, 8, 0.5f);
        if (p <= 0) continue;
        float f = row_fade(y) * qz(m_clamp(p * 2, 0, 1), 8);
        int x0, x1;
        row_span(y, &x0, &x1);
        float cy = y + 6 + L_CARD * 0.5f;
        if (i == A.n) {                              /* add row */
            if (A.n >= SVC_ALARM_MAX) continue;
            float w = 200;
            ui_rrect(fb, UI_CX - w / 2, y + 6, w, L_CARD, L_CARD / 2,
                     ui_dim(A.pressed == i ? C_SURF3 : C_SURF2, f));
            dot_icon(fb, DOT_ICON_PLUS, UI_CX - 52, cy, 2.0f, 1.6f, ui_dim(C_PRI, f));
            ui_label(fb, UI_CX + 14, (int)cy + 6, "ADD", ui_dim(C_PRI, f), UI_CENTER);
            continue;
        }
        const svc_alarm *a = al_get(i);
        int pressed_body = A.pressed == i && A.press_part == PART_BODY;
        ui_rrect(fb, (float)x0, y + 6, (float)(x1 - x0), L_CARD, 36, ui_dim(pressed_body ? C_SURF2 : C_SURF, f));
        char t[12], d[32];
        fmt_hm(t, sizeof t, a->h, a->m);
        fmt_days(d, sizeof d, a->days);
        uint16_t tc = a->on ? C_WHITE : C_DIS;
        dot_style st = { DOT_FONT_TALL, 4.4f, 3.5f, ui_dim(tc, f), 0, 1, 1 };
        int tw = dot_text_anim(fb, &st, L_TIME_X, y + 20, t, 0, p, DOT_REVEAL_RANDOM, (unsigned)i);
        if (!nos_prefs.hour24)
            ui_label(fb, (int)L_TIME_X + tw + 8, (int)(y + 59), a->h < 12 ? "AM" : "PM", ui_dim(C_SEC, f), UI_LEFT);
        ui_label(fb, (int)L_TIME_X, (int)(y + 84), d, ui_dim(a->on ? C_SEC : C_DIS, f), UI_LEFT);
        /* toggle */
        float tx = L_TOG_X, ty = cy;
        float on = qz(A.tog[i].x / 100.0f, 16);
        if (f >= 0.99f) ui_toggle(fb, tx, ty, on);
        else {
            gfx_fill_pill(fb, tx - 16, ty, tx + 16, 22, ui_dim(ui_mix(C_LINE2, C_WHITE, on), f));
            gfx_fill_circle(fb, m_lerp(tx - 16, tx + 16, on), ty, 16, ui_dim(ui_mix(C_DIS, C_BG, on), f));
        }
    }
}

static int list_hit(int x, int y, int *part)
{
    *part = PART_BODY;
    if (A.n == 0) return in_rect(x, y, UI_CX, EMPTY_BTN_Y, 208, 96) ? 0 : -1;
    float fy = y + floorf(A.sc.x + 0.5f) - L_TOP;
    if (fy < 0) return -1;
    int i = (int)(fy / L_ROW);
    if (i > A.n) return -1;
    float yt = row_top(i);
    if (y < yt + 2 || y > yt + 6 + L_CARD + 4) return -1;
    if (i == A.n) return (A.n < SVC_ALARM_MAX && fabsf((float)(x - UI_CX)) < 108) ? i : -1;
    if (x > L_TOG_X - 54) *part = PART_TOGGLE;
    return i;
}

extern const nos_screen scr_alarm_edit;
static int s_edit_idx = -1, s_edit_new;

static void open_edit(int i, int is_new)
{
    s_edit_idx = i; s_edit_new = is_new;
    nos_push(&scr_alarm_edit, TX_PUSH);
}
static void add_alarm(void)
{
    int i = al_add(7, 0);
    if (i < 0) { nos_feedback(FB_ERROR); nos_toast("Alarm list is full"); return; }
    nos_feedback(FB_TAP);
    open_edit(i, 1);
}

static int alarm_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN:
        A.was_moving = A.sc.moving && fabsf(A.sc.v) > 60.0f;
        msc_grab(&A.sc);
        A.dragging = 0;
        A.pressed = A.was_moving ? -1 : list_hit(in->x, in->y, &A.press_part);
        A.empty_pressed = A.n == 0 && A.pressed == 0;
        return 1;
    case IN_DRAG_START:
        A.pressed = -1; A.empty_pressed = 0;
        if (in->axis != AXIS_Y || A.n == 0) { msc_release(&A.sc, 0); return 0; }
        A.dragging = 1;
        msc_drag(&A.sc, in->dy);
        return 1;
    case IN_DRAG:
        if (!A.dragging) return 0;
        msc_drag(&A.sc, in->dy);
        return 1;
    case IN_DRAG_END:
        if (!A.dragging) return 0;
        A.dragging = 0;
        msc_release(&A.sc, -in->vy);
        return 1;
    case IN_UP:
        if (!A.dragging) msc_release(&A.sc, 0);
        A.pressed = -1; A.empty_pressed = 0;
        return 1;
    case IN_TAP: {
        msc_release(&A.sc, 0);
        int part, i = A.was_moving ? -1 : list_hit(in->x, in->y, &part);
        A.pressed = -1; A.empty_pressed = 0;
        if (i < 0) return 1;
        if (A.n == 0 || i == A.n) { add_alarm(); return 1; }
        if (part == PART_TOGGLE) {
            svc_alarm *a = al_get(i);
            a->on = !a->on;
            al_save();
            nos_feedback(a->on ? FB_TOGGLE_ON : FB_TOGGLE_OFF);
        } else { nos_feedback(FB_TAP); open_edit(i, 0); }
        return 1;
    }
    default:
        return 0;
    }
}

const nos_screen scr_alarm = { "ALARMS", alarm_enter, 0, alarm_tick, alarm_draw, alarm_input, 0 };

/* ============================================================== editor */
#define E_DRUM_Y   180.0f
#define E_DRUM_DX  72.0f
#define E_INFO_Y   276.0f
#define E_CHIP1_Y  332.0f
#define E_CHIP2_Y  420.0f
#define E_CHIP_R   38.0f
#define E_SUM_Y    490.0f
#define E_DEL_Y    560.0f
#define E_MAX      178.0f
static struct {
    float t0, build;
    mscroll sc;
    int target;              /* drum being touched, -1 none */
    int dragging, pressed;   /* pressed: 0 none, 1..7 chip (index+1, Mon first), 8 delete */
    char info[32], sum[32];
    int valid;
} E;

static float chip_x(int k) { return k < 4 ? UI_CX + (k - 1.5f) * 88.0f : UI_CX + (k - 5) * 88.0f; }
static float chip_y(int k) { return k < 4 ? E_CHIP1_Y : E_CHIP2_Y; }
static int   chip_wd(int k) { return (k + 1) % 7; }      /* k 0 = Monday */

static void edit_enter(void)
{
    E.t0 = nos_t; E.build = 0; E.target = -1; E.dragging = 0; E.pressed = 0;
    E.valid = s_edit_idx >= 0 && s_edit_idx < al_count();
    msc_init(&E.sc, 0, E_MAX, 0);
    if (!E.valid) return;
    const svc_alarm *a = al_get(s_edit_idx);
    tdrum_setup(2, 0, 23, a->h, 62, 7, 3.6f);
    tdrum_setup(3, 0, 59, a->m, 62, 7, 3.6f);
}
static void edit_leave(void) { if (E.valid) al_save(); }
static void edit_tick(float dt)
{
    E.valid = s_edit_idx >= 0 && s_edit_idx < al_count();
    msc_step(&E.sc, dt);
    float off = floorf(E.sc.x + 0.5f);
    tdrum_place(2, UI_CX - E_DRUM_DX, E_DRUM_Y - off);
    tdrum_place(3, UI_CX + E_DRUM_DX, E_DRUM_Y - off);
    tdrum_tick(2, dt);
    tdrum_tick(3, dt);
    E.build = m_clamp((nos_t - E.t0) / 0.4f, 0, 1);
    if (!E.valid) return;
    svc_alarm *a = al_get(s_edit_idx);
    int h = tdrum_value(2), m = tdrum_value(3);
    if (a->h != h || a->m != m) { a->h = (uint8_t)h; a->m = (uint8_t)m; }
    char in[16];
    fmt_in(in, sizeof in, al_in_min(a));
    if (!nos_prefs.hour24) snprintf(E.info, sizeof E.info, "%s  %s", h < 12 ? "AM" : "PM", in);
    else snprintf(E.info, sizeof E.info, "%s", in);
    fmt_days(E.sum, sizeof E.sum, a->days);
}
static void edit_draw(gfx_fb *fb)
{
    if (!E.valid) { ui_state(fb, UI_CX, UI_CY, "DELETED", C_SEC); return; }
    float off = floorf(E.sc.x + 0.5f), b = E.build;
    ui_title(fb, s_edit_new ? "NEW ALARM" : "EDIT ALARM", off);
    tdrum_draw(fb, 2, b);
    tdrum_draw(fb, 3, b);
    dot_style cs = { DOT_FONT_TALL, 7, 5.6f, C_SEC, 0, 1, 1 };
    if (b > 0.3f) dot_text_a(fb, &cs, UI_CX, E_DRUM_Y - off - 31.5f, ":", 1);
    float k = qz(m_clamp(b * 2 - 0.4f, 0, 1), 8);
    if (k <= 0) return;
    ui_label(fb, UI_CX, (int)(E_INFO_Y - off), E.info, ui_dim(C_SEC, k), UI_CENTER);
    const svc_alarm *a = al_get(s_edit_idx);
    for (int c = 0; c < 7; c++) {
        float p = m_stagger(b, c, 7, 0.55f);
        if (p <= 0) continue;
        float kk = qz(m_clamp(p * 2, 0, 1), 8);
        int on = a->days >> chip_wd(c) & 1;
        float cx = chip_x(c), cy = chip_y(c) - off;
        uint16_t fill = on ? (E.pressed == c + 1 ? C_SEC : C_WHITE) : (E.pressed == c + 1 ? C_SURF3 : C_SURF2);
        ui_circle(fb, cx, cy, E_CHIP_R, ui_dim(fill, kk));
        ui_label(fb, (int)cx, (int)cy + 6, ui_weekday3(chip_wd(c)), on ? C_BG : ui_dim(C_PRI, kk), UI_CENTER);
    }
    ui_label(fb, UI_CX, (int)(E_SUM_Y - off), E.sum, ui_dim(C_PRI, k), UI_CENTER);
    ui_button(fb, UI_CX, E_DEL_Y - off, 196, 88, "DELETE", BTN_DANGER, E.pressed == 8);
}
static int edit_hit(int x, int y)
{
    float off = floorf(E.sc.x + 0.5f);
    for (int c = 0; c < 7; c++)
        if (in_circle(x, y, chip_x(c), chip_y(c) - off, 44)) return c + 1;
    if (in_rect(x, y, UI_CX, E_DEL_Y - off, 204, 96)) return 8;
    return 0;
}
static int edit_input(const nos_input *in)
{
    if (!E.valid) return 0;
    if (in->type == IN_DOWN) {
        E.target = -1; E.dragging = 0;
        if (tdrum_hit(2, in->x, in->y)) E.target = 2;
        else if (tdrum_hit(3, in->x, in->y)) E.target = 3;
        if (E.target >= 0) { msc_release(&E.sc, 0); return tdrum_input(E.target, in); }
        msc_grab(&E.sc);
        E.pressed = edit_hit(in->x, in->y);
        return 1;
    }
    if (E.target >= 0) {
        int r = tdrum_input(E.target, in);
        if (in->type == IN_UP) E.target = -1;
        return r;
    }
    switch (in->type) {
    case IN_DRAG_START:
        E.pressed = 0;
        if (in->axis != AXIS_Y) { msc_release(&E.sc, 0); return 0; }
        E.dragging = 1;
        msc_drag(&E.sc, in->dy);
        return 1;
    case IN_DRAG:
        if (!E.dragging) return 0;
        msc_drag(&E.sc, in->dy);
        return 1;
    case IN_DRAG_END:
        if (!E.dragging) return 0;
        E.dragging = 0;
        msc_release(&E.sc, -in->vy);
        return 1;
    case IN_UP:
        if (!E.dragging) msc_release(&E.sc, 0);
        E.pressed = 0;
        return 1;
    case IN_TAP: {
        msc_release(&E.sc, 0);
        int h = edit_hit(in->x, in->y);
        E.pressed = 0;
        svc_alarm *a = al_get(s_edit_idx);
        if (h >= 1 && h <= 7) {
            int bit = 1 << chip_wd(h - 1);
            a->days ^= (uint8_t)bit;
            nos_feedback(a->days & bit ? FB_TOGGLE_ON : FB_TOGGLE_OFF);
        } else if (h == 8) {
            nos_feedback(FB_TAP);
            al_remove(s_edit_idx);
            al_save();
            E.valid = 0;
            s_edit_idx = -1;
            nos_pop();
        }
        return 1;
    }
    default:
        return 0;
    }
}
const nos_screen scr_alarm_edit = { "ALARM EDIT", edit_enter, edit_leave, edit_tick, edit_draw, edit_input, 0 };

/* ============================================================== ringing */
#define R_TIME_Y   120.0f
#define R_SNZ_Y    262.0f
#define R_SL_Y     362.0f
#define R_SL_W     248.0f
#define R_KNOB_R   36.0f
static struct {
    float t0, build;
    int h, m;
    char label[20];
    mspring knob;            /* 0..1 along the track */
    int grab, committed, haptic_on, snz_pressed, last_step, at_end;
    float grab_k;
} R;
static float sl_x0(void) { return UI_CX - R_SL_W / 2 + 44; }
static float sl_x1(void) { return UI_CX + R_SL_W / 2 - 44; }

static void ring_enter(void)
{
    R.t0 = nos_t; R.build = 0;
    int i = svc_alarm_ringing();
    hal_datetime dt; hal_get_datetime(&dt);
    R.h = dt.hour; R.m = dt.minute;
    snprintf(R.label, sizeof R.label, "ALARM");
    if (i >= 0 && i < svc_alarm_count()) {
        const svc_alarm *a = svc_alarm_get(i);
        R.h = a->h; R.m = a->m;
        if (a->label[0]) snprintf(R.label, sizeof R.label, "%s", a->label);
    }
    msp_init(&R.knob, 0, SPRING_SNAPPY);
    R.grab = R.committed = R.snz_pressed = R.at_end = 0;
    R.last_step = 0;
    /* the alarm service runs haptics + sound while svc_alarm_ringing() >= 0; this
     * screen only silences them when it is dismissed */
    R.haptic_on = 1;
    nos_keep_awake();
}
static void ring_leave(void)
{
    if (R.haptic_on) { hal_haptic_stop(); hal_sfx_stop(); R.haptic_on = 0; }
}
static void ring_tick(float dt)
{
    nos_keep_awake();
    R.build = m_clamp((nos_t - R.t0) / 0.4f, 0, 1);
    if (!R.grab) msp_step(&R.knob, dt);
    if (R.committed && !R.knob.moving) {
        R.committed = 0;
        svc_alarm_stop();
        ring_leave();
        nos_pop();
    }
}
static void ring_draw(gfx_fb *fb)
{
    time_pulse_draw(fb, nos_t - R.t0, 1);
    float b = R.build;
    ui_label(fb, UI_CX, 98, R.label, C_SEC, UI_CENTER);
    char t[12];
    ui_fmt_time(t, sizeof t, R.h, R.m);
    dot_style st = { DOT_FONT_TALL, 9, 7.2f, C_WHITE, 0, 1, 1 };
    dot_text_anim(fb, &st, UI_CX, R_TIME_Y, t, 1, b, DOT_REVEAL_CENTER, 5);
    float k = qz(m_clamp(b * 2 - 0.6f, 0, 1), 8);
    if (k <= 0) return;
    if (!nos_prefs.hour24) ui_label(fb, UI_CX, (int)(R_TIME_Y + 81 + 28), R.h < 12 ? "AM" : "PM", ui_dim(C_SEC, k), UI_CENTER);
    /* snooze */
    ui_rrect(fb, UI_CX - 85, R_SNZ_Y - 44, 170, 88, 44, ui_dim(R.snz_pressed ? C_SURF3 : C_SURF2, k));
    ui_label(fb, UI_CX, (int)R_SNZ_Y + 6, "SNOOZE", ui_dim(C_PRI, k), UI_CENTER);
    /* slide to stop */
    float kx = qz(m_clamp(R.knob.x / 100.0f, 0, 1), 256);
    gfx_fill_pill(fb, sl_x0(), R_SL_Y, sl_x1(), 44, ui_dim(C_SURF2, k));
    float hint = qz(m_clamp(1.0f - kx * 2.5f, 0, 1), 8) * k;
    if (hint > 0) ui_text(fb, UI_F_SMALL, (int)(sl_x0() + R_KNOB_R + (sl_x1() + 44 - sl_x0() - R_KNOB_R) * 0.5f),
                          (int)R_SL_Y + 5, "SLIDE TO STOP", ui_dim(C_SEC, hint), UI_CENTER, 1);
    float x = m_lerp(sl_x0(), sl_x1(), kx);
    ui_circle(fb, x, R_SL_Y, R_KNOB_R, ui_dim(R.grab ? C_PRI : C_WHITE, k));
    dot_icon(fb, DOT_ICON_ARROW_RIGHT, x, R_SL_Y, 2.4f, 1.9f, C_BG);
}
static void snooze(void)
{
    svc_alarm_snooze();
    nos_toast("Snoozed for 9 min");
    ring_leave();
    nos_pop();
}
static int ring_input(const nos_input *in)
{
    float span = sl_x1() - sl_x0();
    switch (in->type) {
    case IN_DOWN: {
        float x = m_lerp(sl_x0(), sl_x1(), R.knob.x / 100.0f);
        R.snz_pressed = in_rect(in->x, in->y, UI_CX, R_SNZ_Y, 178, 96);
        if (!R.committed && in_circle(in->x, in->y, x, R_SL_Y, 52)) {
            R.grab = 1; R.grab_k = R.knob.x / 100.0f; R.knob.v = 0; R.knob.moving = 0;
            R.last_step = (int)(R.knob.x / 20.0f);
        }
        return 1;
    }
    case IN_DRAG_START:
    case IN_DRAG: {
        R.snz_pressed = 0;
        if (!R.grab) return 1;
        float k = R.grab_k + in->dx / span;
        int end = k >= 1.0f;
        if (end && !R.at_end) nos_feedback(FB_LIMIT);
        R.at_end = end;
        k = m_clamp(k, 0, 1);
        R.knob.x = R.knob.target = k * 100.0f;          /* knob spring in percent of the track */
        int step = (int)(k * 5);
        if (step != R.last_step) { if (!end) nos_feedback(FB_TICK); R.last_step = step; }
        return 1;
    }
    case IN_DRAG_END:
        if (!R.grab) return 1;
        R.grab = 0;
        {
            float v = in->vx / span;                  /* track lengths per second */
            if (R.knob.x > 85.0f || (v > 2.5f && R.knob.x > 35.0f)) {
                msp_kick(&R.knob, 100.0f, v > 0 ? v * 100.0f : 0);
                R.committed = 1;
                nos_feedback(FB_SUCCESS);
            } else msp_kick(&R.knob, 0.0f, v * 100.0f);
        }
        return 1;
    case IN_UP:
        if (R.grab) { R.grab = 0; if (!R.committed) msp_kick(&R.knob, 0, 0); }
        R.snz_pressed = 0;
        return 1;
    case IN_TAP:
        R.snz_pressed = 0;
        if (in_rect(in->x, in->y, UI_CX, R_SNZ_Y, 178, 96)) { nos_feedback(FB_TAP); snooze(); }
        return 1;
    case IN_BTN:
        nos_feedback(FB_TAP);
        snooze();
        return 1;
    default:
        return 1;
    }
}
const nos_screen scr_alarm_ring = { "ALARM", ring_enter, ring_leave, ring_tick, ring_draw, ring_input,
                                    SCR_HDRAG | SCR_NOBACK | SCR_KEEPAWAKE };
