/* face_split.c — SPLIT: typographic.
 *
 * A left-aligned stack in the grotesk hero face: the hours in white, the minutes
 * below in grey, and a mono caps column on the right (HR / STP / [TMP] / BAT) with one
 * value column. The tabular "1" is nudged to its optical edge so the stack's left
 * edge stays straight ("10", "11", "09" all start on the same line). Red appears only
 * as an alert: heart rate out of range, else a low battery.
 *
 * Motion: digits roll like an odometer — on a minute change only the changed digits
 * roll (clipped to the digit cell, 300 ms EASE_MECH); the build-in rolls all four in
 * with a 2-frame stagger and the data rows fade in (4 steps, 2 frames apart). Wake
 * from AOD: the outlined AOD digits fill in (4 steps) — the time never leaves.
 * Cost: still frames repaint nothing; a roll repaints one ~100 x 130 px digit cell.
 * Taps: HR -> Heart, STP -> Activity, TMP -> Weather (press rects: face_ext.h). */
#include "faces/face_kit.h"
#include "nos.h"
#include "svc.h"
#include "apps/nos_apps.h"
#include <stdio.h>
#include <string.h>

#define X_DIG   62                      /* left edge of the digit stack */
#define Y_HH    224                     /* baselines */
#define Y_MM    358
#define X_COL   300                     /* data column */
#define ROLL_S  0.30f
#define CAP_H   114                     /* nf_hero cap height + overshoot */

static struct {
    char from[5], to[5];                /* HHMM */
    float t0[4];
    int init;
    int bpm, steps, batt, charging;
    int wx; float temp;
    fk_wake w;
    uint16_t red;                       /* C_RED, easing to C_PRI while recording */
} S;

static void enter(void) { S.init = 0; }

static void tick(const face_ctx *c, float dt)
{
    S.red = fk_red(c, C_PRI);
    fk_aod_update(c);
    fk_wake_tick(&S.w, c);
    fk_unread_tick(c);
    char hh[3], mm[3], now[5];
    fk_hh(hh, c->dt.hour);
    fk_mm(mm, c->dt.minute);
    now[0] = hh[0]; now[1] = hh[1]; now[2] = mm[0]; now[3] = mm[1]; now[4] = 0;
    if (hh[0] == ' ') { now[0] = hh[1]; now[1] = ' '; }   /* 12 h: a single hour digit keeps the left edge */
    if (fk_wake_stale(&S.w)) S.init = 0;                  /* off the glass: settle, don't roll */
    if (!S.init) {
        memcpy(S.from, now, 5); memcpy(S.to, now, 5);
        for (int i = 0; i < 4; i++) S.t0[i] = -100.0f;
        S.init = 1;
    } else if (strcmp(now, S.to)) {
        for (int i = 0; i < 4; i++)
            if (now[i] != S.to[i]) { S.from[i] = S.to[i]; S.t0[i] = c->t; }
        memcpy(S.to, now, 5);
    }
    const svc_heart_t *h = svc_heart();
    const svc_activity_t *a = svc_activity();
    S.bpm = c->bpm > 0 ? c->bpm : h->bpm_last;
    S.steps = c->steps > 0 ? c->steps : a->steps;
    S.batt = c->battery;
    S.charging = c->charging;
    S.wx = c->weather_valid;
    S.temp = c->temp_c;
}

/* optical nudge for a digit in a cell: the tabular "1" moves to the stack's edge
 * (left cell) or half way (right cell, so "11" doesn't read "1  1") */
static int nudge(char ch, int right)
{
    if (ch != '1') return 0;
    const font_glyph *g1 = fk_glyph(UI_F_HERO, '1'), *g0 = fk_glyph(UI_F_HERO, '0');
    int d = (g1 && g0) ? g1->xoff - g0->xoff : 0;
    return right ? -d / 2 : -d;
}

static int hr_alarm(void) { return S.bpm > 0 && (S.bpm >= 120 || S.bpm < 40); }
static int bat_alarm(void) { return S.batt > 0 && S.batt <= 15 && !S.charging; }

/* baselines of the data rows: 50 px apart, centred on the digit block */
static int nrows(void) { return S.wx ? 4 : 3; }
static int row_y(int k, int n) { return 240 + (int)(((float)k - (float)(n - 1) * 0.5f) * 50.0f); }
static int val_x(void) { return X_COL + ui_text_width(UI_F_LABEL, "STP", 2) + 16; }

/* row k of n: its label, value text and value colour; returns the row kind
 * (0 HR, 1 STP, 2 TMP, 3 BAT) */
static int row_text(int k, int n, char b[16], const char **labp, uint16_t *vcolp)
{
    const char *lab = "BAT";
    uint16_t vcol = C_PRI;
    int kind = k == 0 ? 0 : k == 1 ? 1 : (k == 2 && n == 4) ? 2 : 3;
    switch (kind) {
    case 0:
        lab = "HR";
        if (S.bpm > 0) snprintf(b, 16, "%d", S.bpm); else snprintf(b, 16, "--");
        if (hr_alarm()) vcol = S.red;
        break;
    case 1:
        lab = "STP";
        if (S.steps >= 10000) snprintf(b, 16, "%d.%dK", S.steps / 1000, (S.steps % 1000) / 100);
        else snprintf(b, 16, "%d", S.steps);
        break;
    case 2:
        lab = "TMP";
        snprintf(b, 16, "%d\xB0", (int)(S.temp + (S.temp < 0 ? -0.5f : 0.5f)));
        break;
    default:
        if (S.batt > 0 || S.charging) snprintf(b, 16, "%d", S.batt);
        else snprintf(b, 16, "--");
        if (bat_alarm()) vcol = hr_alarm() ? C_WHITE : S.red;
        break;
    }
    *labp = lab; *vcolp = vcol;
    return kind;
}

static void data_row(gfx_fb *fb, int k, int n, float a, const face_ctx *c, float dx, float dy)
{
    char b[16];
    const char *lab;
    uint16_t vcol;
    int kind = row_text(k, n, b, &lab, &vcol);
    int y = row_y(k, n) + (int)dy, x = X_COL + (int)dx;
    uint16_t lc = ui_dim(kind == 3 && bat_alarm() ? C_PRI : C_DIS, a);
    ui_label(fb, x, y, lab, lc, UI_LEFT);
    int vx = val_x() + (int)dx;
    if (kind == 3 && S.charging) {                      /* bolt before the value */
        dot_icon(fb, DOT_ICON_BOLT, (float)(vx + 6), (float)(y - 6), 1.45f, 1.7f, ui_dim(C_PRI, a));
        vx += 18;
    }
    ui_label(fb, vx, y, b, ui_dim(vcol, a), UI_LEFT);
}

static void draw(gfx_fb *fb, const face_ctx *c)
{
    int cw = ui_text_width(UI_F_HERO, "0", 0);
    float dx, dy;
    fk_wake_off(&S.w, c, &dx, &dy);
    float k = fk_wake_k(&S.w, c);
    int n = nrows();
    if (c->aod || k < 1.0f) {
        /* AOD outlines; during the wake morph the cores fill in */
        for (int i = 0; i < 4; i++) {
            char s[2] = { S.to[i], 0 };
            int x = X_DIG + (i & 1) * cw + nudge(S.to[i], i & 1) + (int)dx;
            int y = (i < 2 ? Y_HH : Y_MM) + (int)dy;
            uint16_t fin = i < 2 ? C_WHITE : C_DIS;
            uint16_t line = ui_mix(i < 2 ? C_SEC : C_DIS, fin, k);
            static const signed char o[4][2] = { {-1,0},{1,0},{0,-1},{0,1} };
            for (int j = 0; j < 4; j++) ui_text(fb, UI_F_HERO, x + o[j][0], y + o[j][1], s, line, UI_LEFT, 0);
            ui_text(fb, UI_F_HERO, x, y, s, ui_mix(C_BG, fin, k), UI_LEFT, 0);
        }
        if (c->aod) {
            if (S.batt > 0) {                            /* one datum, on the BAT row */
                char b[12]; snprintf(b, sizeof b, "%d", S.batt);
                int y = row_y(n - 1, n) + (int)dy;
                ui_label(fb, X_COL + (int)dx, y, "BAT", C_DIS, UI_LEFT);
                ui_label(fb, val_x() + (int)dx, y, b, C_DIS, UI_LEFT);
            }
        } else
            for (int r = 0; r < n; r++) data_row(fb, r, n, k, c, dx, dy);
        /* grey in AOD, reaching its rest colour (red unless an alarm owns the red) */
        fk_unread(fb, c, ui_mix(C_SEC, (hr_alarm() || bat_alarm()) ? C_PRI : fk_red(c, C_PRI), k), C_BG, dx, dy);
        return;
    }
    float e = fk_build(&S.w, c);
    for (int i = 0; i < 4; i++) {
        int x = X_DIG + (i & 1) * cw, base = i < 2 ? Y_HH : Y_MM;
        uint16_t col = i < 2 ? C_WHITE : C_DIS;
        float p;
        char from = S.from[i];
        if (e < 1.0f) { p = m_clamp((e - 0.07f * (float)i) / 0.6f, 0, 1); from = 0; }
        else p = m_clamp((c->t - S.t0[i]) / ROLL_S, 0, 1);
        fk_roll2(fb, UI_F_HERO, 0, x, base, cw, CAP_H, from, S.to[i],
                 nudge(from, i & 1), nudge(S.to[i], i & 1), p, col);
    }
    /* data column: rows fade in over 4 frames each, 2 frames apart */
    for (int r = 0; r < n; r++) {
        float a = 1.0f;
        if (e < 1.0f) {
            float t = e * 12.0f - 3.0f - 2.0f * (float)r;   /* frames since this row's start */
            a = m_clamp((float)(int)(t + 1.0f) / 4.0f, 0, 1);
        }
        if (a > 0.0f) data_row(fb, r, n, a, c, 0, 0);
    }
    fk_unread(fb, c, (hr_alarm() || bat_alarm()) ? C_PRI : fk_red(c, C_PRI), C_BG, 0, 0);
}

/* press rect of the complication under (x, y); returns its screen or 0 */
const struct nos_screen *face_split_hit(int x, int y, int r[4])
{
    if (x < X_COL - 24) return 0;
    int n = nrows();
    for (int k = 0; k < n; k++) {
        int ry = row_y(k, n) - 6;                    /* row centre (label cap height 13) */
        if (y < ry - 25 || y >= ry + 25) continue;
        const struct nos_screen *s = k == 0 ? &scr_heart : k == 1 ? &scr_activity :
                                     (k == 2 && n == 4) ? &scr_weather : 0;
        if (s && r) {                                /* hugs the row's ink: label to value end */
            char b[16]; const char *lab; uint16_t vc;
            row_text(k, n, b, &lab, &vc);
            r[0] = X_COL - 16; r[1] = ry - 22;
            r[2] = val_x() + ui_text_width(UI_F_LABEL, b, 2) + 16; r[3] = ry + 22;
        }
        return s;
    }
    return 0;
}

static const struct nos_screen *tap(int x, int y) { return face_split_hit(x, y, 0); }

const nos_face face_split = { "SPLIT", enter, tick, draw, tap };
