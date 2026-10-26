/* time_alarm.c — Alarms: list (next-alarm summary, per-row toggles, add), editor (hour /
 * minute drums, day chips, delete) and the ringing screen (slide-to-stop, snooze).
 * Team: time.
 *
 * State lives in svc (svc_alarm_*). While svc is the link stub (svc_alarm_add returns
 * -1 with room left) the list falls back to a local table so the app stays usable;
 * fallback alarms are not fired (that is the alarm service's job). */
#include "nos.h"
#include "diag_log.h"
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
void time_alert_dot(gfx_fb *fb);

static float qz(float v, float n) { return floorf(v * n + 0.5f) / n; }
/* Builds count whole frames (docs/MOTION30.html R3/R9): b runs 0..1 over BUILD_F frames,
 * bk() gives the frame back; pills scale in on the 6-frame table from frame k0. */
#define BUILD_F 12
static int   bk(float b) { return (int)floorf(b * BUILD_F + 0.5f); }
static float scale_k(int k, int k0) { return mf_at(&MF_MICRO6, k - k0); }
/* a list row r's build at frame k: rows one frame apart (at most 8 starts), 6 frames each */
static float row_p(int k, int r) { return m_clamp((float)m_stagger_k(k, r, 1) / 6.0f, 0, 1); }
/* A label swap: the old text fades out over frames 1-3, the new one in over 4-6, in
 * perceptual ink steps. t0 is the frame it changed (it shows its first step there). */
static float swap_old(nos_time_t t0) { return m_ink(1.0f - (float)(m_frame_k(t0) + 1) / 3.0f); }
static float swap_new(nos_time_t t0) { return m_ink((float)(m_frame_k(t0) + 1 - 3) / 3.0f); }
#define PRESS_HOLD 0.1f          /* a pressed control shows >= 0.1 s (3 frames) even on a quick tap */
/* the label of a pill that builds by scale: in from k 0.6, 3 steps of fill -> ink */
static float ink_k(float k) { return qz(m_clamp((k - 0.3f) / 0.4f, 0, 1), 3); }   /* from the 2nd scale step */
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
/* Snooze state lives in svc (svc_alarm_snoozed / svc_alarm_snooze_cancel). The local
 * fallback table has no ringer, so nothing in it is ever snoozed. */


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
static int is_snoozed(int idx)
{
    int i;
    return a_svc != 0 && svc_alarm_snoozed(&i, NULL) && i == idx;
}

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
#define L_ROW    90.0f           /* the list-row pitch (§8) */
#define L_CARD   80.0f
#define L_PAD    5.0f            /* card top inside its row */
#define L_MARGIN 30.0f
static struct {
    nos_time_t t0; float build;
    mscroll sc;
    int dragging, was_moving;
    int pressed, press_part;     /* row (n = add row), part 0 body 1 toggle */
    nos_time_t press_t0, unpress_at;  /* a pressed row shows >= 100 ms even on a quick tap */
    uint32_t press_generation, press_id;
    int press_bound;
    mtween tog[SVC_ALARM_MAX];       /* toggle knob, percent: the 4-frame table (14-11-5-2 px) */
    uint8_t tog_on[SVC_ALARM_MAX];
    int n;
    uint8_t order[SVC_ALARM_MAX];   /* display row -> alarm index, sorted by time */
    char next[32], next_old[32];    /* header; the old one fades out when it changes */
    int have_next, old_have_next;
    nos_time_t next_t0;
    int empty_pressed;
    int leaving;                     /* pushing the editor: the press holds until covered */
    uint32_t snz_at;                 /* svc's snooze deadline the cached minute is for */
    int snz_h, snz_m;                /* wall-clock minute the snooze rings in */
    char lab[SVC_ALARM_MAX][24], lab_old[SVC_ALARM_MAX][24];   /* row sub-labels */
    uint8_t lab_snz[SVC_ALARM_MAX], lab_old_snz[SVC_ALARM_MAX];
    nos_time_t lab_t0[SVC_ALARM_MAX];
    float fb_prev;                   /* fling blur: last frame's drawn offset */
    int fb_lv;                       /* fling blur level this frame (0 = sharp) */
} A;
/* label change: old fades out, then the new one in (swap_old / swap_new, 6 frames) */
enum { PART_BODY = 0, PART_TOGGLE };

static float list_max(void)
{
    float content = L_TOP + (A.n + 1) * L_ROW + 70;
    return content > GFX_H ? content - GFX_H : 0;
}

/* the row's sub-label: "SNOOZED 07:09" while snoozed, else its days; returns 1 if snoozed */
static int row_label(int idx, char *b, int n)
{
    if (is_snoozed(idx)) {
        char tt[12];
        ui_fmt_time(tt, sizeof tt, A.snz_h, A.snz_m);
        snprintf(b, (size_t)n, "SNOOZED %s", tt);
        return 1;
    }
    fmt_days(b, n, al_get(idx)->days);
    return 0;
}
/* fade: a changed header / row label crossfades (off on entry and when rows shift) */
static void list_sync(int fade)
{
    int was = A.n;
    A.n = al_count();
    if (A.n != was) fade = 0;
    uint32_t at;
    if (a_svc != 0 && svc_alarm_snoozed(NULL, &at) && at != A.snz_at) {
        /* the minute it rings in, computed once per snooze so it never flickers */
        hal_datetime dt; hal_get_datetime(&dt);
        int32_t left = (int32_t)(at - hal_millis());
        int sec = dt.hour * 3600 + dt.minute * 60 + dt.second + (left > 0 ? (int)(left / 1000) : 0);
        A.snz_h = sec / 3600 % 24; A.snz_m = sec / 60 % 60;
        A.snz_at = at;
    }
    msc_bounds(&A.sc, 0, list_max());
    /* rows in time order (svc keeps creation order; indices stay svc's) */
    for (int i = 0; i < A.n; i++) A.order[i] = (uint8_t)i;
    for (int i = 1; i < A.n; i++) {
        uint8_t v = A.order[i];
        int tv = al_get(v)->h * 60 + al_get(v)->m, j = i - 1;
        while (j >= 0 && al_get(A.order[j])->h * 60 + al_get(A.order[j])->m > tv) { A.order[j + 1] = A.order[j]; j--; }
        A.order[j + 1] = v;
    }
    int h, m, in;
    char nx[32];
    int hn = time_alarm_next(&h, &m, &in);
    if (hn) {
        char t[16];
        fmt_in(t, sizeof t, in);
        snprintf(nx, sizeof nx, "NEXT %s", t);
    } else snprintf(nx, sizeof nx, "ALL ALARMS OFF");
    if (strcmp(nx, A.next)) {
        if (fade && A.next[0]) {
            memcpy(A.next_old, A.next, sizeof A.next);
            A.old_have_next = A.have_next;
            A.next_t0 = nos_t;              /* swap_*: frame 1 is this one */
        } else A.next_t0 = -10;
        memcpy(A.next, nx, sizeof A.next);
    }
    A.have_next = hn;
    for (int i = 0; i < A.n && i < SVC_ALARM_MAX; i++) {
        char b[24];
        int sz = row_label(i, b, sizeof b);
        if (strcmp(b, A.lab[i])) {
            if (fade) {
                memcpy(A.lab_old[i], A.lab[i], sizeof A.lab[i]);
                A.lab_old_snz[i] = A.lab_snz[i];
                A.lab_t0[i] = nos_t;
            } else A.lab_t0[i] = -10;
            memcpy(A.lab[i], b, sizeof A.lab[i]);
        } else if (!fade) A.lab_t0[i] = -10;
        A.lab_snz[i] = (uint8_t)sz;
    }
}
/* crossfade levels for a label change at t0: old out over the first half, new in after */
static float xf_old(nos_time_t t0) { return swap_old(t0); }
static float xf_new(nos_time_t t0) { return swap_new(t0); }
static int row_on(int idx) { return al_get(idx)->on || is_snoozed(idx); }

static void tog_set(int i, int on)
{
    float v = on ? 100.0f : 0.0f;
    mtw_start_tab(&A.tog[i], v, v, &MF_MICRO4);
    A.tog[i].t0 = -10;
    A.tog_on[i] = (uint8_t)on;
}
static float tog_x(int i) { return mtw_value(&A.tog[i]); }

static void ed_watch(void);
static void alarm_enter(void)
{
    A.pressed = -1; A.dragging = 0; A.empty_pressed = 0; A.leaving = 0;
    A.press_bound = 0;
    int back = nos_revealed();                  /* revealed by a pop: keep scroll + no build */
    int was = A.n;
    A.n = al_count();
    if (!back) { A.t0 = nos_enter_t(); A.build = 0; msc_init(&A.sc, 0, list_max(), 0); A.next[0] = 0; }
    list_sync(back && A.n == was);             /* rows shifted: no label crossfade */
    for (int i = 0; i < A.n && i < SVC_ALARM_MAX; i++) tog_set(i, row_on(i));
    A.fb_prev = floorf(A.sc.x + 0.5f); A.fb_lv = 0;
}
static void fblur_tick(float dt);
static void alarm_tick(float dt)
{
    ed_watch();                                  /* a new alarm lands as its editor leaves */
    int was = A.n;
    list_sync(1);
    if (A.n != was) {
        /* rows shifted: every knob rests where its alarm is */
        for (int i = 0; i < A.n; i++) tog_set(i, row_on(i));
        if (!A.n || !was) { A.t0 = nos_t; A.build = 0; }    /* list <-> empty: build the new state */
    }
    for (int i = 0; i < A.n; i++) {
        int on = row_on(i);
        if (on != A.tog_on[i]) {
            /* a tap (this frame's input) or svc: the knob travels from where it is now, and
             * shows its first step on this frame */
            mtw_start_tab(&A.tog[i], tog_x(i), on ? 100.0f : 0.0f, &MF_MICRO4);
            mtw_lead(&A.tog[i]);
            A.tog_on[i] = (uint8_t)on;
        }
    }
    msc_step(&A.sc, dt);
    if (!A.leaving && A.unpress_at > 0 && nos_t >= A.unpress_at) { A.pressed = -1; A.empty_pressed = 0; A.unpress_at = 0; }
    A.build = m_clamp((nos_t - A.t0) / M_FRAMES(BUILD_F), 0, 1);
    fblur_tick(dt);
    /* motion audit (sim only) */
    {
        static const char *k_tog[4] = { "alarm.tog0", "alarm.tog1", "alarm.tog2", "alarm.tog3" };
        nos_motion_probe("alarm.list", A.dragging ? MC_FINGER : MC_MOMENTUM, floorf(A.sc.x + 0.5f));
        for (int r = 0; r < A.n && r < 4; r++)
            nos_motion_probe(k_tog[r], MC_MICRO, 32.0f * qz(tog_x(A.order[r]) / 100.0f, 16));
        nos_motion_probe("alarm.row0.ink", MC_FADE, m_ink(row_p(bk(A.build), 0) * 2) * 100.0f);
        nos_motion_probe("alarm.row3.ink", MC_FADE, m_ink(row_p(bk(A.build), 3) * 2) * 100.0f);
        nos_motion_probe("alarm.next.new", MC_FADE, xf_new(A.next_t0) * 100.0f);
        nos_motion_probe("alarm.next.old", MC_FADE, A.next_t0 > 0 ? xf_old(A.next_t0) * 100.0f : 0.0f);
        if (A.n == 0) {
            float k = scale_k(bk(A.build), 3);
            if (k > 0) nos_motion_probe("alarm.empty.btn.w", MC_MICRO, UI_BTN_W * (0.72f + 0.28f * k));
        }
    }
}

static float row_top(int r) { return L_TOP + r * L_ROW - floorf(A.sc.x + 0.5f); }
/* Fixed-width cards (the notification card: 344 wide, r 40, SURF2); content on fixed
 * anchors, so rows never drift sideways as they scroll. */
#define L_CARD_W  344.0f
#define L_TIME_X  (UI_CX - 142.0f)
#define L_TOG_X   (UI_CX + 112.0f)
static float row_fade(float y)
{
    float d = fabsf(y + L_ROW * 0.5f - UI_CY);
    return qz(1.0f - 0.7f * m_smoothstep(130.0f, 230.0f, d), 16);
}

static void alarm_row(gfx_fb *fb, int r, float y, float f, float p, int pressed, int part);
/* rows on the glass at drawn offset off, clamped to the list (n alarms + the add row) */
static void al_vis(float off, int *i0, int *i1)
{
    *i0 = (int)floorf((off - L_TOP) / L_ROW);
    *i1 = (int)floorf((off - L_TOP + GFX_H) / L_ROW);
    if (*i0 < 0) *i0 = 0;
    if (*i1 > A.n) *i1 = A.n;
}
static void alarm_draw(gfx_fb *fb)
{
    float b = A.build;
    float off = floorf(A.sc.x + 0.5f);
    ui_title(fb, "ALARMS", off);
    if (A.n == 0) {
        ui_absent(fb, DOT_ICON_ALARM, "No alarms", NULL, NULL, b);
        float k = scale_k(bk(b), 3);                 /* frames 4-9 */
        if (k <= 0) return;
        float sc = 0.72f + 0.28f * k, w = UI_BTN_W * sc, h = UI_BTN_H * sc, ik = ink_k(k);
        uint16_t fill = A.empty_pressed ? C_SEC : C_WHITE;
        ui_rrect(fb, UI_CX - w / 2, UI_BTN_Y - h / 2, w, h, h / 2, fill);
        if (ik > 0) ui_label(fb, UI_CX, UI_BTN_Y + 6, "ADD ALARM", ui_mix(fill, C_BG, ik), UI_CENTER);
        return;
    }
    float sk = m_ink(b * 2) * (1.0f - m_clamp(off / 50.0f, 0, 1));
    if (sk > 0.02f) {
        float ko = xf_old(A.next_t0), kn = xf_new(A.next_t0);
        if (ko > 0) ui_label(fb, UI_CX, (int)(102 - off), A.next_old, ui_dim(A.old_have_next ? C_PRI : C_DIS, qz(sk, 8) * ko), UI_CENTER);
        if (kn > 0) ui_label(fb, UI_CX, (int)(102 - off), A.next, ui_dim(A.have_next ? C_PRI : C_DIS, qz(sk, 8) * kn), UI_CENTER);
    }
    /* a fast free glide (ui.h fling blur) blits every visible row's pre-blurred sprite,
     * or none: a lone sharp row among smeared ones reads as a glitch */
    int i0, i1;
    al_vis(off, &i0, &i1);
    int blur = A.fb_lv;
    for (int r = 0; r <= A.n; r++) {
        float y = row_top(r);
        if (y + L_ROW < 0) continue;
        if (y > GFX_H) break;
        float p = row_p(bk(b), r);
        if (p <= 0) continue;
        float f = row_fade(y) * m_ink(p * 2);
        if (blur && ui_fblur_draw(fb, &A, r, (int)y, f)) continue;
        alarm_row(fb, r, y, f, p, A.pressed == r, A.press_part);
    }
}

/* fling blur: row r at full ink, built, not pressed, its top at y (the cache bakes it) */
static void alarm_blur_row(gfx_fb *fb, int i, int x0, int y, int x1, int h, void *ctx)
{
    (void)x0; (void)x1; (void)h; (void)ctx;
    alarm_row(fb, i, (float)y, 1.0f, 1.0f, 0, PART_BODY);
}
/* Prepare rows during entry; use the selected cache shutter only after the build
 * and while no row is pressed. The shared policy decides whether drags blur. */
static void fblur_tick(float dt)
{
    float off = floorf(A.sc.x + 0.5f), fr = dt * M_FPS;
    float st = (off - A.fb_prev) / (fr > 0.5f ? fr : 1.0f);
    A.fb_prev = off;
    A.fb_lv = 0;
    if (A.n == 0) return;
    int held = A.dragging || A.pressed >= 0;
    int i0, i1;
    al_vis(off, &i0, &i1);
    ui_fblur_tick(&A, held, st, i0, i1, A.n + 1, (int)L_ROW,
                  (int)(UI_CX - L_CARD_W / 2) - 1, (int)(UI_CX + L_CARD_W / 2) + 1, 0, alarm_blur_row, 0);
    A.fb_lv = A.build >= 1.0f && A.pressed < 0 && ui_fblur_active(&A);
}

/* one list row (r == A.n: the add pill) with its top at y, ink f, build p */
static void alarm_row(gfx_fb *fb, int r, float y, float f, float p, int pressed, int part)
{
    float cy = y + L_PAD + L_CARD * 0.5f;
    if (r == A.n) {                              /* add row: a pill, pressed C_SEC + black ink */
        if (A.n >= SVC_ALARM_MAX) return;
        float w = UI_BTN_W;
        int pr = pressed;
        ui_rrect(fb, UI_CX - w / 2, y + L_PAD, w, L_CARD, L_CARD / 2, ui_dim(pr ? C_SEC : C_SURF2, f));
        dot_icon(fb, DOT_ICON_PLUS, UI_CX - 52, cy, 2.0f, 1.6f, pr ? C_BG : ui_dim(C_PRI, f));
        ui_label(fb, UI_CX + 14, (int)cy + 6, "ADD", pr ? C_BG : ui_dim(C_PRI, f), UI_CENTER);
        return;
    }
    int idx = A.order[r];
    const svc_alarm *a = al_get(idx);
    int pressed_body = pressed && part == PART_BODY;
    uint16_t card = pressed_body ? C_LINE2 : C_SURF2;   /* a pressed card: C_LINE2 */
    ui_rrect(fb, UI_CX - L_CARD_W / 2, y + L_PAD, L_CARD_W, L_CARD, 40, ui_dim(card, f));
    char t[12];
    fmt_hm(t, sizeof t, a->h, a->m);
    /* the time dims with the toggle's own motion (not in one frame) */
    float on = qz(tog_x(idx) / 100.0f, 8);
    dot_style st = { DOT_FONT_TALL, UI_LIST_PITCH, UI_LIST_PITCH * 0.8f, ui_dim(ui_mix(C_DIS, C_WHITE, on), f), 0, 1, 1 };
    int tw = dot_text_anim(fb, &st, L_TIME_X, y + 17, t, 0, p, DOT_REVEAL_RANDOM, (unsigned)idx);
    if (!nos_prefs.hour24)
        ui_label(fb, (int)L_TIME_X + tw + 8, (int)(y + 56), a->h < 12 ? "AM" : "PM", ui_dim(C_SEC, f), UI_LEFT);
    /* sub-label: SNOOZED hh:mm in white, days dimming with the toggle; a change fades
     * out the old text into the card colour, then fades the new one in */
    {
        uint16_t cn = A.lab_snz[idx] ? C_PRI : ui_mix(C_DIS, C_SEC, on);
        uint16_t co = A.lab_old_snz[idx] ? C_PRI : ui_mix(C_DIS, C_SEC, on);
        float ko = xf_old(A.lab_t0[idx]), kn = xf_new(A.lab_t0[idx]);
        if (ko > 0) ui_label(fb, (int)L_TIME_X, (int)(y + 76), A.lab_old[idx], ui_dim(ui_mix(card, co, ko), f), UI_LEFT);
        if (kn > 0) ui_label(fb, (int)L_TIME_X, (int)(y + 76), A.lab[idx], ui_dim(ui_mix(card, cn, kn), f), UI_LEFT);
    }
    float tx = L_TOG_X, ty = cy, ton = qz(tog_x(idx) / 100.0f, 16);
    if (f >= 0.99f) ui_toggle(fb, tx, ty, ton);
    else {
        gfx_fill_pill(fb, tx - 16, ty, tx + 16, 22, ui_dim(ui_mix(C_LINE2, C_WHITE, ton), f));
        gfx_fill_circle(fb, m_lerp(tx - 16, tx + 16, ton), ty, 16, ui_dim(ui_mix(C_DIS, C_BG, ton), f));
    }
}

/* returns the display row (n = add row), -1 none */
static int list_hit(int x, int y, int *part)
{
    *part = PART_BODY;
    if (A.n == 0) return in_rect(x, y, UI_CX, UI_BTN_Y, UI_BTN_W + 8, 96) ? 0 : -1;
    float fy = y + floorf(A.sc.x + 0.5f) - L_TOP;
    if (fy < 0) return -1;
    int r = (int)(fy / L_ROW);
    if (r > A.n) return -1;
    float yt = row_top(r);
    if (y < yt + 1 || y > yt + L_PAD + L_CARD + 4) return -1;
    if (r == A.n) return (A.n < SVC_ALARM_MAX && fabsf((float)(x - UI_CX)) < 108) ? r : -1;
    if (x > L_TOG_X - 54) *part = PART_TOGGLE;
    return r;
}

extern const nos_screen scr_alarm_edit;
/* s_edit_idx < 0 with s_edit_new: a draft (s_draft) the list doesn't have yet. It is
 * added when the editor starts to leave, so the list under the push stays as it was and
 * DELETE on a new alarm leaves no stray row. */
static int s_edit_idx = -1, s_edit_new;
static uint32_t s_edit_generation, s_edit_id;
static int s_edit_conflict;
static svc_alarm s_draft;

static void open_edit(int i, int is_new)
{
    s_edit_idx = i; s_edit_new = is_new;
    s_edit_generation = svc_alarm_generation();
    s_edit_id = i >= 0 && a_svc != 0 ? svc_alarm_id(i) : 0;
    s_edit_conflict = 0;
    A.leaving = 1; A.unpress_at = 0;             /* the tapped pill/card stays pressed */
    nos_push(&scr_alarm_edit, TX_PUSH);
}
static void add_alarm(void)
{
    if (al_count() >= SVC_ALARM_MAX) { nos_feedback(FB_ERROR); nos_toast("List full"); return; }
    nos_feedback(FB_TAP);
    memset(&s_draft, 0, sizeof s_draft);
    s_draft.h = 7; s_draft.m = 0; s_draft.on = 1; s_draft.days = 0;
    open_edit(-1, 1);
}
/* Cancel a snooze from the list toggle. svc drops the snooze; the alarm also switches
 * off, so the toggle shows what the tap meant (a one-shot is already off: svc clears it
 * when it fires). The toggle then slides off and the label + header crossfade. */
static void snooze_cancel(int idx)
{
    svc_alarm_snooze_cancel();
    al_get(idx)->on = 0;
    al_save();
}

static int alarm_press_valid(int idx)
{
    return !A.press_bound || a_svc == 0 ||
           (A.press_generation == svc_alarm_generation() &&
            (!A.press_id || (idx >= 0 && svc_alarm_id(idx) == A.press_id)));
}
static int alarm_input(const nos_input *in)
{
    if (in->type == IN_CANCEL) { A.pressed = -1; A.empty_pressed = A.dragging = A.press_bound = 0; A.unpress_at = 0; msc_jump(&A.sc, A.sc.x); return 1; }

    switch (in->type) {
    case IN_DOWN:
        A.was_moving = A.sc.moving && fabsf(A.sc.v) > 60.0f;
        msc_grab(&A.sc);
        A.dragging = 0;
        A.pressed = A.was_moving ? -1 : list_hit(in->x, in->y, &A.press_part);
        A.empty_pressed = A.n == 0 && A.pressed == 0;
        A.press_t0 = nos_t; A.unpress_at = 0;
        A.press_bound = 1; A.press_generation = svc_alarm_generation(); A.press_id = 0;
        if (a_svc != 0 && A.pressed >= 0 && A.pressed < A.n) A.press_id = svc_alarm_id(A.order[A.pressed]);
        return 1;
    case IN_DRAG_START:
        A.press_bound = 0;
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
        /* still grabbed (no drag): let go. After a drag IN_DRAG_END already released it
         * with the fling's velocity, which this must not zero */
        if (!A.dragging && A.sc.dragging) msc_release(&A.sc, 0);
        if (A.pressed >= 0 || A.empty_pressed) A.unpress_at = fmax(nos_t, A.press_t0 + 0.1f);
        return 1;
    case IN_TAP: {
        msc_release(&A.sc, 0);
        int part, r = A.was_moving ? -1 : list_hit(in->x, in->y, &part);
        if (r < 0) return 1;
        int bound = alarm_press_valid(r < A.n ? A.order[r] : -1);
        A.press_bound = 0;
        if (!bound) { A.pressed = -1; nos_toast("ALARMS UPDATED"); return 1; }
        if (A.n == 0 || r == A.n) { add_alarm(); return 1; }
        int idx = A.order[r];
        if (part == PART_TOGGLE) {
            if (is_snoozed(idx)) { snooze_cancel(idx); nos_feedback(FB_TOGGLE_OFF); return 1; }
            svc_alarm *a = al_get(idx);
            uint32_t request = diag_action_request(DIAG_DOMAIN_ALARM, DIAG_ACT_SET,
                                                   a_svc == 0 ? idx + 1 : (int32_t)svc_alarm_id(idx));
            diag_action_context(request, (int32_t)diag_log_name_id("ALARMS"), a->on);
            a->on = !a->on;
            diag_action_result(request, DIAG_RESULT_SUCCESS, a->on); /* RAM edit; svc logs checked save */
            al_save();
            nos_feedback(a->on ? FB_TOGGLE_ON : FB_TOGGLE_OFF);
        } else { nos_feedback(FB_TAP); open_edit(idx, 0); }
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
#define E_INFO_Y   284.0f
#define E_CHIP1_Y  328.0f
#define E_CHIP2_Y  404.0f
#define E_CHIP_R   34.0f
#define E_SUM_Y    490.0f
#define E_DEL_Y    558.0f
#define E_MAX      190.0f
#define E_ARM_S    3.0f            /* delete stays armed 3 s (one red dot per second) */
#define E_DEL_W    ((float)UI_BTN_W)
#define E_ARM_W    320.0f          /* the arm standard (Power, Settings) */
#define E_CHIP_S   M_FRAMES(4)     /* a chip's on/off colour step */
#define E_XF_S     M_FRAMES(6)     /* label swaps: old out (3 frames), new in (3) */
static struct {
    nos_time_t t0; float build;
    mscroll sc;
    int target;              /* drum being touched, -1 none */
    int dragging, pressed;   /* pressed: 0 none, 1..7 chip (index+1, Mon first), 8 delete */
    nos_time_t press_t0, unpress_at;
    char info[32], sum[32], sum_old[32];
    nos_time_t sum_t0;
    int sum_ok, valid;
    float on_k[7];           /* chip on amount, steps to its bit over E_CHIP_S */
    float ck[7];             /* chip pressed amount: 1 while held, then steps back */
    nos_time_t armed_t;           /* delete armed at (nos_t), < 0 = not armed */
    nos_time_t arm_t0; float dis_e;     /* last arm/disarm (nos_t); armed time at the disarm */
    int arm_was;             /* armed before the last change */
    mtween dw;               /* delete width between the two labels: EASE_STEADY, 7 frames */
    int shown;               /* the push has finished (so a transition now = leaving) */
    nos_time_t del_at;            /* confirm-delete fires at (once the press has shown) */
    int gone;                /* deleted: the editor draws its last state from s_draft */
    float dw_x;              /* delete width this frame (prepared in tick) */
} E;
static svc_alarm *ed_alarm(void) { return s_edit_idx >= 0 ? al_get(s_edit_idx) : &s_draft; }
static int ed_valid(void)
{
    if (E.gone) return 1;
    if (s_edit_conflict) return 0;
    if (a_svc != 0 && (svc_alarm_generation() != s_edit_generation ||
        (s_edit_idx >= 0 && svc_alarm_id(s_edit_idx) != s_edit_id))) return 0;
    return s_edit_idx >= 0 ? s_edit_idx < al_count() : s_edit_new;
}
static int ed_reconcile(void)
{
    E.valid = ed_valid();
    if (E.valid) return 1;
    if (!s_edit_conflict) { s_edit_conflict = 1; nos_toast("ALARMS UPDATED"); }
    if (!nos_transitioning() && nos_top() == &scr_alarm_edit) nos_pop();
    return 0;
}
/* add the draft to the list (once); the editor goes on editing the stored alarm */
static void ed_commit(void)
{
    if (E.gone || !ed_valid() || s_edit_idx >= 0 || !s_edit_new) return;
    int i = al_add(s_draft.h, s_draft.m);
    if (i < 0) { s_edit_new = 0; nos_toast("List full"); return; }
    svc_alarm *a = al_get(i);
    a->days = s_draft.days; a->on = s_draft.on;
    memcpy(a->label, s_draft.label, sizeof a->label);
    al_save();
    s_edit_idx = i;
    s_edit_generation = svc_alarm_generation();
    s_edit_id = a_svc == 0 ? 0 : svc_alarm_id(i);
}
/* the editor is on top and moving after its push finished: a pop / back swipe / home */
static void ed_watch(void)
{
    if (!(E.shown && nos_top() == &scr_alarm_edit && nos_transitioning())) return;
    ed_commit();
    A.pressed = -1; A.empty_pressed = 0; A.leaving = 0; A.unpress_at = 0;   /* the list shows again */
}

static float chip_x(int k) { return k < 4 ? UI_CX + (k - 1.5f) * 80.0f : UI_CX + (k - 5) * 80.0f; }
static float chip_y(int k) { return k < 4 ? E_CHIP1_Y : E_CHIP2_Y; }
static int   chip_wd(int k) { return (k + 1) % 7; }      /* k 0 = Monday */
/* arm / disarm delete: the width springs, the label crossfades, the dots build */
static void set_armed(int on)
{
    int was = E.armed_t >= 0;
    if (on == was) return;
    E.arm_was = was; E.arm_t0 = nos_t;
    if (!on) E.dis_e = nos_t - E.armed_t;
    E.armed_t = on ? nos_t : -1;
    /* widen first (from the tap: it moves this frame); a disarm waits for the long label */
    if (on) { mtw_start(&E.dw, mtw_value(&E.dw), E_ARM_W, M_FRAMES(7), &EASE_STEADY); mtw_lead(&E.dw); }
}
/* the summary line says only what the chips can't: ONCE, EVERY DAY, WEEKDAYS, WEEKENDS */
static void sum_words(char *b, int n, int days)
{
    days &= 0x7F;
    if (!days || days == 0x7F || days == 0x3E || days == 0x41) fmt_days(b, n, days);
    else b[0] = 0;
}
/* confirm-delete: the alarm goes now; the editor keeps drawing its last state (frozen,
 * DELETE still pressed) as it slides away */
static void ed_delete(void)
{
    if (!ed_valid()) return;
    E.del_at = 0;
    s_draft = *ed_alarm();
    if (s_edit_idx >= 0) { al_remove(s_edit_idx); al_save(); }   /* a draft just goes */
    s_edit_idx = -1;
    E.gone = 1;
    nos_pop();
}
static float xf_out(nos_time_t t0) { return swap_old(t0); }
static float xf_in(nos_time_t t0)  { return swap_new(t0); }

static void edit_enter(void)
{
    if (nos_revealed()) { E.valid = ed_valid(); return; }
    E.t0 = nos_enter_t(); E.build = 0; E.target = -1; E.dragging = 0; E.pressed = 0; E.unpress_at = 0;
    E.armed_t = -1; E.arm_t0 = -10; E.arm_was = 0; E.sum_ok = 0; E.sum_t0 = -10;
    mtw_start(&E.dw, E_DEL_W, E_DEL_W, M_FRAMES(7), &EASE_STEADY); E.dw.t0 = -10; E.dw_x = E_DEL_W;
    E.shown = 0; E.del_at = 0; E.gone = 0;
    E.valid = ed_valid();
    msc_init(&E.sc, 0, E_MAX, 0);
    if (!E.valid) return;
    const svc_alarm *a = ed_alarm();
    for (int c = 0; c < 7; c++) { E.on_k[c] = (float)(a->days >> chip_wd(c) & 1); E.ck[c] = 0; }
    tdrum_setup(2, 0, 23, a->h, 62, UI_HEROW_PITCH, 3.6f);   /* HH:MM hero: pitch 8 */
    tdrum_setup(3, 0, 59, a->m, 62, UI_HEROW_PITCH, 3.6f);
}
static void edit_leave(void)
{
    if (nos_leaving_covered()) return;           /* covered (a ring): the draft waits */
    if (!ed_valid()) { s_edit_new = 0; return; }
    if (E.del_at > 0) ed_delete();               /* left before the press finished */
    ed_commit();                                 /* gone without moving (home now) */
    if (ed_valid() && !E.gone) al_save();
    E.shown = 0; E.gone = 0; s_edit_new = 0;
}
static void edit_tick(float dt)
{
    if (!ed_reconcile()) return;
    if (!nos_transitioning()) E.shown = 1;
    else ed_watch();
    E.valid = ed_valid();
    msc_step(&E.sc, dt);
    float off = floorf(E.sc.x + 0.5f);
    tdrum_place(2, UI_CX - E_DRUM_DX, E_DRUM_Y - off);
    tdrum_place(3, UI_CX + E_DRUM_DX, E_DRUM_Y - off);
    tdrum_tick(2, dt);
    tdrum_tick(3, dt);
    E.build = m_clamp((nos_t - E.t0) / M_FRAMES(BUILD_F), 0, 1);
    if (E.del_at > 0 && nos_t >= E.del_at) { ed_delete(); return; }
    if (E.gone) return;                          /* frozen as it leaves */
    if (E.armed_t >= 0 && nos_t - E.armed_t >= E_ARM_S) set_armed(0);
    if (E.armed_t < 0 && E.dw.to != E_DEL_W && m_frame_k(E.arm_t0) + 1 >= 3)
        mtw_start(&E.dw, mtw_value(&E.dw), E_DEL_W, M_FRAMES(7), &EASE_STEADY);
    E.dw_x = mtw_value(&E.dw);
    if (E.unpress_at > 0 && nos_t >= E.unpress_at) { E.pressed = 0; E.unpress_at = 0; }
    if (!E.valid) return;
    svc_alarm *a = ed_alarm();
    for (int c = 0; c < 7; c++) {
        float tg = (float)(a->days >> chip_wd(c) & 1), st = dt / E_CHIP_S;
        E.on_k[c] = E.on_k[c] < tg ? fminf(tg, E.on_k[c] + st) : fmaxf(tg, E.on_k[c] - st);
        if (E.pressed == c + 1) E.ck[c] = 1;
        else E.ck[c] = fmaxf(0, E.ck[c] - dt / PRESS_HOLD);
    }
    int h = tdrum_value(2), m = tdrum_value(3);
    if (a->h != h || a->m != m) { a->h = (uint8_t)h; a->m = (uint8_t)m; }
    /* motion audit (sim only) */
    nos_motion_probe("aledit.scroll", E.dragging ? MC_FINGER : MC_MOMENTUM, floorf(E.sc.x + 0.5f));
    nos_motion_probe("aledit.del.w", MC_COMPONENT, floorf(mtw_value(&E.dw) + 0.5f));
    nos_motion_probe("aledit.chip0.on", MC_FADE, qz(E.on_k[0], 4) * 100.0f);
    nos_motion_probe("aledit.chip0.press", MC_FADE, qz(E.ck[0], 4) * 100.0f);
    nos_motion_probe("aledit.chip6.ink", MC_FADE, m_ink(row_p(bk(E.build), 6) * 2) * 100.0f);
    nos_motion_probe("aledit.info.ink", MC_FADE, m_ink((float)(bk(E.build) - 2) / 6.0f) * 100.0f);
    nos_motion_probe("aledit.arm.new", MC_FADE, xf_in(E.arm_t0) * 100.0f);
    char in[16];
    fmt_in(in, sizeof in, al_in_min(a));
    if (!nos_prefs.hour24) snprintf(E.info, sizeof E.info, "%s  %s", h < 12 ? "AM" : "PM", in);
    else snprintf(E.info, sizeof E.info, "%s", in);
    char sb[32];
    sum_words(sb, sizeof sb, a->days);
    if (strcmp(sb, E.sum)) {
        if (E.sum_ok) { memcpy(E.sum_old, E.sum, sizeof E.sum); E.sum_t0 = nos_t; }
        memcpy(E.sum, sb, sizeof E.sum);
    }
    E.sum_ok = 1;
}
static void edit_draw(gfx_fb *fb)
{
    if (!E.valid) { ui_state(fb, UI_CX, UI_CY, "DELETED", C_SEC); return; }
    float off = floorf(E.sc.x + 0.5f), b = E.build;
    ui_title(fb, s_edit_new ? "NEW ALARM" : "EDIT ALARM", off);
    tdrum_draw(fb, 2, b);
    tdrum_draw(fb, 3, b);
    dot_style cs = { DOT_FONT_TALL, UI_HEROW_PITCH, UI_HEROW_PITCH * 0.8f, C_SEC, 0, 1, 1 };
    if (b > 0.3f) dot_text_a(fb, &cs, UI_CX, E_DRUM_Y - off - 36.0f, ":", 1);
    float k = m_ink((float)(bk(b) - 2) / 6.0f);   /* frames 3-8 */
    if (k <= 0) return;
    ui_label(fb, UI_CX, (int)(E_INFO_Y - off), E.info, ui_dim(C_SEC, k), UI_CENTER);
    for (int c = 0; c < 7; c++) {
        float p = row_p(bk(b), c);
        if (p <= 0) continue;
        float kk = m_ink(p * 2);
        float cx = chip_x(c), cy = chip_y(c) - off;
        /* on/off steps over 0.15 s; pressed = C_SEC with black ink, held >= 0.1 s, then
         * stepping back into the (new) state colour */
        float on = qz(E.on_k[c], 4), pk = qz(E.ck[c], 4);
        uint16_t fill = ui_mix(ui_mix(C_SURF2, C_WHITE, on), C_SEC, pk);
        uint16_t ink = ui_mix(ui_mix(C_PRI, C_BG, on), C_BG, pk);
        ui_circle(fb, cx, cy, E_CHIP_R, ui_dim(fill, kk));
        ui_label(fb, (int)cx, (int)cy + 6, ui_weekday3(chip_wd(c)), ui_mix(ui_dim(fill, kk), ink, kk), UI_CENTER);
    }
    {   /* the summary crossfades when it changes (and is empty when the chips say it) */
        float ko = xf_out(E.sum_t0), kn = xf_in(E.sum_t0);
        if (ko > 0 && E.sum_old[0]) ui_label(fb, UI_CX, (int)(E_SUM_Y - off), E.sum_old, ui_dim(C_PRI, k * ko), UI_CENTER);
        if (kn > 0 && E.sum[0]) ui_label(fb, UI_CX, (int)(E_SUM_Y - off), E.sum, ui_dim(C_PRI, k * kn), UI_CENTER);
    }
    /* delete: arm, then tap again (OS standard). Arming springs the outline wider, the
     * label crossfades through the fill, and three red dots build from the centre and
     * expire one per second (red -> grey in steps). Pressed: C_SURF2 fill (BTN_DANGER). */
    float dy = E_DEL_Y - off, w = floorf(E.dw_x + 0.5f);
    int armed = E.armed_t >= 0;
    uint16_t dfill = E.pressed == 8 ? C_SURF2 : C_BG;
    if (E.pressed == 8) ui_rrect(fb, UI_CX - w / 2, dy - UI_BTN_H / 2, w, UI_BTN_H, UI_BTN_H / 2, dfill);
    ui_rrect_line(fb, UI_CX - w / 2, dy - UI_BTN_H / 2, w, UI_BTN_H, UI_BTN_H / 2, 2, C_RED);
    {
        int ly = (int)(dy + UI_F_LABEL->ascent * 0.5f - 2);
        float ko = xf_out(E.arm_t0), kn = xf_in(E.arm_t0);
        if (ko > 0) ui_label(fb, UI_CX, ly, E.arm_was ? "TAP AGAIN TO DELETE" : "DELETE", ui_mix(dfill, C_RED, ko), UI_CENTER);
        if (kn > 0) ui_label(fb, UI_CX, ly, armed ? "TAP AGAIN TO DELETE" : "DELETE", ui_mix(dfill, C_RED, kn), UI_CENTER);
    }
    /* the dots build on the 3-frame snap table, the centre first and the sides 2 frames
     * later (out: sides first); each expires red -> grey in 4 one-frame steps */
    int ks = m_frame_k(E.arm_t0) + 1;
    for (int i = 0; i < 3; i++) {
        int dist = i == 1 ? 0 : 1;
        float sz;
        if (armed) sz = mf_at(&MF_SNAP3, ks - dist * 2);                    /* centre first */
        else if (E.arm_was) sz = 1.0f - mf_at(&MF_SNAP3, ks - (1 - dist) * 2);
        else sz = 0;
        if (sz <= 0) continue;
        float e = armed ? nos_t - E.armed_t : E.dis_e;
        uint16_t col = ui_mix(C_RED, C_LINE2, qz(m_clamp((e - (3 - i)) / M_FRAMES(4), 0, 1), 4));
        dot_disc(fb, UI_CX + (i - 1) * 14.0f, dy + 60, 6 * sz, col);
    }
}
static int edit_hit(int x, int y)
{
    float off = floorf(E.sc.x + 0.5f);
    for (int c = 0; c < 7; c++)
        if (in_circle(x, y, chip_x(c), chip_y(c) - off, 44)) return c + 1;
    if (in_rect(x, y, UI_CX, E_DEL_Y - off, (E.armed_t >= 0 ? E_ARM_W : E_DEL_W) + 8, 96)) return 8;
    return 0;
}
static int edit_input(const nos_input *in)
{
    if (in->type == IN_CANCEL) { E.pressed = E.dragging = 0; E.target = -1; E.unpress_at = E.del_at = 0; tdrum_input(2, in); tdrum_input(3, in); msc_jump(&E.sc, E.sc.x); return 1; }

    if (!ed_reconcile()) return 1;
    if (!E.valid) return 0;
    if (E.gone || E.del_at > 0) return 1;        /* deleting */
    if (in->type == IN_DOWN) {
        E.target = -1; E.dragging = 0;
        if (tdrum_hit(2, in->x, in->y)) E.target = 2;
        else if (tdrum_hit(3, in->x, in->y)) E.target = 3;
        if (E.target >= 0) { msc_release(&E.sc, 0); return tdrum_input(E.target, in); }
        msc_grab(&E.sc);
        E.pressed = edit_hit(in->x, in->y); E.press_t0 = nos_t; E.unpress_at = 0;
        return 1;
    }
    if (E.target >= 0) {
        int r = tdrum_input(E.target, in);
        if (in->type == IN_UP) E.target = -1;
        return r;
    }
    switch (in->type) {
    case IN_DRAG_START:
        E.pressed = 0; E.unpress_at = 0;
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
        if (!E.dragging && E.sc.dragging) msc_release(&E.sc, 0);   /* only a held list: a fling keeps its speed */
        if (E.pressed) E.unpress_at = fmax(nos_t, E.press_t0 + PRESS_HOLD);
        return 1;
    case IN_TAP: {
        msc_release(&E.sc, 0);
        int h = edit_hit(in->x, in->y);
        svc_alarm *a = ed_alarm();
        if (h != 8) set_armed(0);
        if (h >= 1 && h <= 7) {
            int bit = 1 << chip_wd(h - 1);
            a->days ^= (uint8_t)bit;
            nos_feedback(a->days & bit ? FB_TOGGLE_ON : FB_TOGGLE_OFF);
        } else if (h == 8 && E.armed_t < 0) {
            nos_feedback(FB_LIMIT);
            set_armed(1);
        } else if (h == 8) {
            nos_feedback(FB_TAP);
            /* DELETE stays pressed >= PRESS_HOLD, then the editor leaves as it was */
            E.pressed = 8; E.unpress_at = 0;
            E.del_at = fmax(nos_t, E.press_t0 + PRESS_HOLD);
            if (nos_t >= E.del_at) ed_delete();
        }
        return 1;
    }
    default:
        return 0;
    }
}
const nos_screen scr_alarm_edit = { "ALARM EDIT", edit_enter, edit_leave, edit_tick, edit_draw, edit_input, 0 };

/* ============================================================== ringing */
#define R_TIME_Y   100.0f
#define R_SNZ_Y    240.0f     /* SNOOZE 196..284 */
#define R_SL_Y     352.0f     /* track 312..392: 28 px under SNOOZE */
#define R_SL_W     260.0f     /* far cap point r 189: 15+ px inside the pulse ring (r 211) */
#define R_SL_R     40.0f
#define R_KNOB_R   32.0f      /* 8 px inset, like the pill buttons' content */
static struct {
    nos_time_t t0; float build;
    int h, m;
    char label[24];
    mspring knob;            /* 0..1 along the track */
    int grab, committed, haptic_on, snz_pressed, last_step, at_end;
    int leaving;             /* snoozed / stopped: SNOOZE stays pressed as the screen falls */
    nos_time_t snz_t0, snz_unpress, snz_fire; float grab_k;   /* snz_fire: a quick tap snoozes once the press has shown */
} R;
static float sl_x0(void) { return UI_CX - R_SL_W / 2 + R_SL_R; }
static float sl_x1(void) { return UI_CX + R_SL_W / 2 - R_SL_R; }

static void ring_enter(void)
{
    R.t0 = nos_enter_t(); R.build = 0; R.snz_unpress = 0; R.snz_fire = 0; R.leaving = 0;
    int i = svc_alarm_ringing();
    hal_datetime dt; hal_get_datetime(&dt);
    R.h = dt.hour; R.m = dt.minute;
    snprintf(R.label, sizeof R.label, "ALARM");
    if (i >= 0 && i < svc_alarm_count()) {
        const svc_alarm *a = svc_alarm_get(i);
        R.h = a->h; R.m = a->m;
        if (a->label[0]) snprintf(R.label, sizeof R.label, "%s", a->label);
    }
    if (!nos_prefs.hour24) {                     /* AM/PM rides in the title line */
        size_t l = strlen(R.label);
        snprintf(R.label + l, sizeof R.label - l, "  %s", R.h < 12 ? "AM" : "PM");
    }
    msp_init(&R.knob, 0, SPRING_SNAPPY);
    msp_snap(&R.knob, 100.0f / 180.0f);         /* percent of a 180 px track: a 1 px snap (R5) */
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
static void snooze(void);
static void ring_tick(float dt)
{
    if (svc_alarm_ringing() < 0) {
        if (!nos_transitioning() && nos_top() == &scr_alarm_ring) { ring_leave(); nos_pop(); }
        return;
    }
    nos_keep_awake();
    R.build = m_clamp((nos_t - R.t0) / M_FRAMES(BUILD_F), 0, 1);
    if (R.snz_fire > 0 && nos_t >= R.snz_fire) { R.snz_fire = 0; snooze(); return; }
    if (!R.leaving && R.snz_unpress > 0 && nos_t >= R.snz_unpress) { R.snz_pressed = 0; R.snz_unpress = 0; }
    if (!R.grab) msp_step(&R.knob, dt);
    {   /* motion audit (sim only) */
        float kx = qz(m_clamp(R.knob.x / 100.0f, 0, 1), 256);
        nos_motion_probe("aring.knob", R.grab ? MC_FINGER : MC_MOMENTUM, m_lerp(sl_x0(), sl_x1(), kx));
        float k = scale_k(bk(R.build), 3);
        if (k > 0) nos_motion_probe("aring.btn.w", MC_MICRO, UI_BTN_W * (0.72f + 0.28f * k));
    }
    if (R.committed && !R.knob.moving) {
        R.committed = 0;
        int index = svc_alarm_ringing();
        uint32_t request = diag_action_request(DIAG_DOMAIN_ALARM, DIAG_ACT_STOP, (int32_t)svc_alarm_id(index));
        diag_action_context(request, (int32_t)diag_log_name_id("ALARM_RING"), (int32_t)svc_alarm_generation());
        svc_alarm_stop();
        diag_action_result(request, svc_alarm_ringing() < 0 ? DIAG_RESULT_SUCCESS : DIAG_RESULT_REJECTED, 0);
        ring_leave();
        nos_pop();
    }
}
static void ring_draw(gfx_fb *fb)
{
    time_pulse_draw(fb, nos_t - R.t0, 1);
    time_alert_dot(fb);
    float b = R.build;
    ui_title(fb, R.label, 0);
    char t[12];
    ui_fmt_time(t, sizeof t, R.h, R.m);
    dot_style st = { DOT_FONT_TALL, UI_HEROW_PITCH, UI_HEROW_PITCH * 0.8f, C_WHITE, 0, 1, 1 };
    dot_text_anim(fb, &st, UI_CX, R_TIME_Y, t, 1, b, DOT_REVEAL_CENTER, 5);
    float k = scale_k(bk(b), 3);                 /* frames 4-9 */
    if (k <= 0) return;
    /* SNOOZE, the track and the knob build by scale steps at full colour (no fade-up
     * from black); labels come in from 60 % of the build. Pressed: C_SEC, black ink. */
    float sc = 0.72f + 0.28f * k, ik = ink_k(k);
    uint16_t sf = R.snz_pressed ? C_SEC : C_SURF2, si = R.snz_pressed ? C_BG : C_PRI;
    float sw = UI_BTN_W * sc, sh = UI_BTN_H * sc;
    ui_rrect(fb, UI_CX - sw / 2, R_SNZ_Y - sh / 2, sw, sh, sh / 2, sf);
    if (ik > 0) ui_label(fb, UI_CX, (int)R_SNZ_Y + 6, "SNOOZE", ui_mix(sf, si, ik), UI_CENTER);
    /* slide to stop */
    float kx = qz(m_clamp(R.knob.x / 100.0f, 0, 1), 256);
    float hw = (R_SL_W / 2 - R_SL_R) * sc, tr = R_SL_R * sc;
    gfx_fill_pill(fb, UI_CX - hw, R_SL_Y, UI_CX + hw, tr, C_SURF2);
    float hint = qz(m_clamp(1.0f - kx * 2.5f, 0, 1), 8) * ik;
    /* centred between the resting knob's edge and the track end: equal margins (16 px;
     * the baked +0.06 em tracking only, so it fits the 260 track) */
    if (hint > 0) ui_text(fb, UI_F_LABEL, (int)floorf((sl_x0() + R_KNOB_R + sl_x1() + R_SL_R) * 0.5f + 0.5f),
                          (int)R_SL_Y + 6, "SLIDE TO STOP", ui_mix(C_SURF2, C_SEC, hint), UI_CENTER, 0);
    float x = m_lerp(sl_x0(), sl_x1(), kx);
    uint16_t kf = R.grab ? C_SEC : C_WHITE;
    ui_circle(fb, x, R_SL_Y, R_KNOB_R * sc, kf);
    if (ik > 0) dot_icon(fb, DOT_ICON_ARROW_RIGHT, x, R_SL_Y, 2.4f, 1.9f, ui_mix(kf, C_BG, ik));
}
static void snooze(void)
{
    R.leaving = 1; R.snz_unpress = 0;
    int index = svc_alarm_ringing(), snoozed = -1;
    uint32_t request = diag_action_request(DIAG_DOMAIN_ALARM, DIAG_ACT_PAUSE, (int32_t)svc_alarm_id(index));
    diag_action_context(request, (int32_t)diag_log_name_id("ALARM_RING"), (int32_t)svc_alarm_generation());
    svc_alarm_snooze();                          /* svc remembers which alarm, and when */
    int accepted = index >= 0 && svc_alarm_snoozed(&snoozed, NULL) && snoozed == index;
    diag_action_result(request, accepted ? DIAG_RESULT_SUCCESS : DIAG_RESULT_REJECTED, accepted);
    nos_toast("Snoozed");                        /* the list row says until when */
    ring_leave();
    nos_pop();
}
static int ring_input(const nos_input *in)
{
    if (in->type == IN_CANCEL) { R.grab = R.snz_pressed = R.committed = 0; R.snz_fire = R.snz_unpress = 0; R.knob.v = 0; msp_to(&R.knob, 0); return 1; }

    if (R.snz_fire > 0 || R.leaving || R.committed) return 1;   /* snoozing / leaving / a committed stop wins (button too) */
    float span = sl_x1() - sl_x0();
    switch (in->type) {
    case IN_DOWN: {
        float x = m_lerp(sl_x0(), sl_x1(), R.knob.x / 100.0f);
        R.snz_pressed = in_rect(in->x, in->y, UI_CX, R_SNZ_Y, UI_BTN_W + 8, 96);
        R.snz_t0 = nos_t; R.snz_unpress = 0;
        if (!R.committed && in_circle(in->x, in->y, x, R_SL_Y, 52)) {
            R.grab = 1; R.grab_k = R.knob.x / 100.0f; R.knob.v = 0; R.knob.moving = 0;
            R.last_step = (int)(R.knob.x / 20.0f);
        }
        return 1;
    }
    case IN_DRAG_START:
    case IN_DRAG: {
        R.snz_pressed = 0; R.snz_unpress = 0;
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
        if (R.snz_pressed) R.snz_unpress = fmax(nos_t, R.snz_t0 + PRESS_HOLD);
        return 1;
    case IN_TAP:
        if (in_rect(in->x, in->y, UI_CX, R_SNZ_Y, UI_BTN_W + 8, 96)) {
            /* SNOOZE stays pressed until the screen is gone; a tap quicker than the press
             * hold waits out the rest of it (<= 2 frames) */
            nos_feedback(FB_TAP);
            R.snz_pressed = 1; R.snz_unpress = 0;
            if (nos_t >= R.snz_t0 + PRESS_HOLD) snooze();
            else R.snz_fire = R.snz_t0 + PRESS_HOLD;
        }
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
