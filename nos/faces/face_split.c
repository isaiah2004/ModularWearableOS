/* face_split.c — SPLIT: typographic.
 *
 * A left-aligned stack in the grotesk hero face: the hours in white, the minutes
 * below in grey, and a mono caps column on the right (HR / STP / BAT). Red appears
 * only when the heart rate is out of range.
 *
 * Motion: digits are tabular and roll like an odometer — on a minute change only the
 * digits that changed roll up (old out, new in, clipped to the digit cell, 300 ms
 * EASE_MECH); the build-in rolls all four in with a 2-frame stagger while the data
 * column steps in line by line.
 * Cost: still frames repaint nothing; a roll repaints one ~100 x 130 px digit cell.
 * Taps: HR -> Heart, STP -> Activity. */
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
} S;

static void enter(void) { S.init = 0; }

static void tick(const face_ctx *c, float dt)
{
    fk_aod_update(c);
    char hh[3], mm[3], now[5];
    fk_hh(hh, c->dt.hour);
    fk_mm(mm, c->dt.minute);
    now[0] = hh[0]; now[1] = hh[1]; now[2] = mm[0]; now[3] = mm[1]; now[4] = 0;
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

/* one digit cell: the old glyph rolls up and out while the new one rises in */
static void digit(gfx_fb *fb, int x, int base, char from, char to, float p, uint16_t col, int cell_w)
{
    char a[2] = { from, 0 }, b[2] = { to, 0 };
    if (p >= 1.0f || from == to) { ui_text(fb, UI_F_HERO, x, base, b, col, UI_LEFT, 0); return; }
    gfx_fb sub;
    int top = base - CAP_H - 6, bot = base + 6;      /* digits have no descenders */
    if (!nos_subview(fb, &sub, x - 4 + fb->ox, top + fb->oy, x + cell_w + 4 + fb->ox, bot + fb->oy, 0, 0)) return;
    float e = fk_mech(p);
    int span = bot - top;
    int dy = (int)(e * (float)span + 0.5f);
    if (from) ui_text(&sub, UI_F_HERO, x, base - dy, a, col, UI_LEFT, 0);
    ui_text(&sub, UI_F_HERO, x, base + span - dy, b, col, UI_LEFT, 0);
}

/* baselines of the data rows: 50 px apart, centred on the digit block */
static int row_y(int k, int n) { return 246 + (int)(((float)k - (float)(n - 1) * 0.5f) * 50.0f); }

static void data_row(gfx_fb *fb, int x, int y, const char *lab, const char *val, uint16_t vcol, uint16_t lcol)
{
    ui_label(fb, x, y, lab, lcol, UI_LEFT);
    ui_label(fb, x + ui_text_width(UI_F_LABEL, "STP", 2) + 16, y, val, vcol, UI_LEFT);  /* one value column */
}

static void draw(gfx_fb *fb, const face_ctx *c)
{
    int cw = ui_text_width(UI_F_HERO, "0", 0);
    char b[16];
    if (c->aod) {
        int dx = fk_aod_dx, dy = fk_aod_dy;
        char hh[3] = { S.to[0], S.to[1], 0 }, mm[3] = { S.to[2], S.to[3], 0 };
        fk_text_outline(fb, UI_F_HERO, X_DIG + dx, Y_HH + dy, hh, C_SEC, UI_LEFT, 0, 1);
        fk_text_outline(fb, UI_F_HERO, X_DIG + dx, Y_MM + dy, mm, C_DIS, UI_LEFT, 0, 1);
        if (S.batt > 0) {
            snprintf(b, sizeof b, "%d", S.batt);
            data_row(fb, X_COL + dx, 296 + dy, "BAT", b, C_DIS, C_LINE2);
        }
        return;
    }
    float e = c->enter;
    for (int i = 0; i < 4; i++) {
        int x = X_DIG + (i & 1) * cw, base = i < 2 ? Y_HH : Y_MM;
        uint16_t col = i < 2 ? C_WHITE : C_DIS;
        float p;
        char from = S.from[i];
        if (e < 1.0f) { p = m_clamp((e - 0.07f * (float)i) / 0.6f, 0, 1); from = 0; }
        else p = m_clamp((c->t - S.t0[i]) / ROLL_S, 0, 1);
        digit(fb, x, base, from, S.to[i], p, col, cw);
    }
    /* data column, stepping in one line at a time during the build-in */
    int n = S.wx ? 4 : 3;
    int lines = e >= 1.0f ? n : (int)(m_clamp((e - 0.35f) / 0.5f, 0, 1) * ((float)n + 0.99f));
    int out_of_range = S.bpm > 0 && (S.bpm >= 120 || S.bpm < 40);
    for (int k = 0; k < lines; k++) {
        int y = row_y(k, n);
        switch (k) {
        case 0:
            if (S.bpm > 0) snprintf(b, sizeof b, "%d", S.bpm); else snprintf(b, sizeof b, "--");
            data_row(fb, X_COL, y, "HR", b, out_of_range ? C_RED : C_PRI, C_DIS);
            break;
        case 1:
            if (S.steps >= 10000) snprintf(b, sizeof b, "%d.%dK", S.steps / 1000, (S.steps % 1000) / 100);
            else snprintf(b, sizeof b, "%d", S.steps);
            data_row(fb, X_COL, y, "STP", b, C_PRI, C_DIS);
            break;
        case 2:
            if (n == 4) {
                snprintf(b, sizeof b, "%d�", (int)(S.temp + (S.temp < 0 ? -0.5f : 0.5f)));
                data_row(fb, X_COL, y, "TMP", b, C_PRI, C_DIS);
                break;
            }
            /* fall through */
        default:
            if (S.batt > 0 || S.charging) snprintf(b, sizeof b, S.charging ? "%d+" : "%d", S.batt);
            else snprintf(b, sizeof b, "--");
            data_row(fb, X_COL, y, "BAT", b, S.batt > 0 && S.batt <= 15 && !S.charging ? C_WHITE : C_PRI, C_DIS);
            break;
        }
    }
}

static const struct nos_screen *tap(int x, int y)
{
    if (x < X_COL - 24) return 0;
    int n = S.wx ? 4 : 3;
    for (int k = 0; k < n; k++) {
        int ry = row_y(k, n) - 6;                    /* row centre (label cap height 13) */
        if (y < ry - 25 || y >= ry + 25) continue;
        if (k == 0) return &scr_heart;
        if (k == 1) return &scr_activity;
        if (k == 2 && n == 4) return &scr_weather;
        return 0;
    }
    return 0;
}

const nos_face face_split = { "SPLIT", enter, tick, draw, tap };
