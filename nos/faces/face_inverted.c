/* face_inverted.c — INVERTED: the white Nothing phone, as a face.
 *
 * A full white disc with a black hairline ring at r = 220, black dot-matrix time on
 * one line with light-grey ghost cells, the date above (the battery under it while low
 * or charging) and the steps below in mono caps. The seconds are one red dot riding
 * the hairline (full float precision, gliding with millis). AOD is dark, never white:
 * the time as grey ring-dots on black and a dim hairline.
 *
 * Opening (build-in and wake alike, 0.5 s): the white disc grows linearly from the
 * centre over 0.36 s with the hairline riding its edge, so the panel never flashes
 * white; the digits are mid-grey dots meanwhile (readable on black and on white — on a
 * wake they are the AOD digits from frame 0, on a build-in they print in by column),
 * then darken to black in 3 steps while the ghost cells and labels fade up in 4. No
 * ghost cell is ever drawn on black; a minute turning mid-opening morphs as usual, and
 * the unread dot rides the opening as a mid-grey dot. Minute change: dot_text_morph;
 * the date cross-fades at midnight.
 * Cost: per frame only the red dot's box; the white fill is a 4 ns/px rect fill on a
 * full redraw. Tap the steps line: Activity. */
#include "faces/face_kit.h"
#include "nos.h"
#include "svc.h"
#include "apps/nos_apps.h"
#include <stdio.h>

#define P       14.0f
#define Y_TIME  (UI_CY - 4.5f * P)
#define R_RING  220.0f
#define C_INK   0x0000
#define C_GHOSTW 0xEF7D                 /* #EFEFEF unlit cells on white, neutral */
#define C_LABW  0x6B4D                  /* #6A6A6A labels on white */
#define C_MID   0x8C51                  /* #8A8A8E digits while the disc opens */
#define OPEN_S  0.36f
#define DONE_S  0.50f

static struct { fk_morph tm; int steps; fk_wake w; int restart; float t0; int wake; fk_xfade date; } S = { .t0 = -100.0f };

static void enter(void) { S.tm.init = 0; S.restart = 1; }

static void tick(const face_ctx *c, float dt)
{
    fk_aod_update(c);
    int was = S.w.active;
    fk_wake_tick(&S.w, c);
    fk_unread_tick(c);
    if (S.w.active && !was) { S.t0 = c->t; S.wake = 1; S.restart = 0; }
    else if (S.restart && !c->aod) {
        if (!fk_wake_skip(&S.w)) { S.t0 = c->t; S.wake = 0; }
        S.restart = 0;
    }
    char b[8], db[16];
    char hh[3], mm[3];
    fk_hh(hh, c->dt.hour); fk_mm(mm, c->dt.minute);
    snprintf(b, sizeof b, "%s:%s", hh, mm);
    fk_morph_tick(&S.tm, b, c->t);
    int stale = fk_wake_stale(&S.w);
    if (stale) fk_morph_settle(&S.tm);
    fk_date(db, c);
    fk_xfade_tick(&S.date, db, c->t, c->aod || stale || c->t - S.t0 < DONE_S);
    S.steps = c->steps > 0 ? c->steps : svc_activity()->steps;
}

static void draw_aod(gfx_fb *fb, const face_ctx *c)
{
    float dx = (float)fk_aod_dx, dy = (float)fk_aod_dy;
    gfx_ring(fb, UI_CX + dx, UI_CY + dy, R_RING, 1.5f, C_LINE2);
    dot_style st = { DOT_FONT_TALL, P, P * 0.8f, C_DIS, 0, 1, 1 };
    dot_text_a(fb, &st, UI_CX + dx, Y_TIME + dy, S.tm.to, 1);
    st.dot = P * 0.8f - 3.5f; st.color = C_BG;          /* hollow the dots */
    dot_text_a(fb, &st, UI_CX + dx, Y_TIME + dy, S.tm.to, 1);
    ui_label(fb, UI_CX + (int)dx, 140 + (int)dy, S.date.to, C_DIS, UI_CENTER);
    fk_status(fb, c, UI_CX + (int)dx, 166 + (int)dy, C_DIS, UI_CENTER);   /* critical only */
    fk_unread(fb, c, C_SEC, C_BG, dx, dy + 24.0f);
}

static float steps4(float x) { return (float)(int)(m_clamp(x, 0, 1) * 4.0f) / 4.0f; }

static void draw(gfx_fb *fb, const face_ctx *c)
{
    if (c->aod) { draw_aod(fb, c); return; }
    char b[24];
    float u = c->t - S.t0;                               /* time into the opening */
    int opening = u < DONE_S;
    float open = m_clamp(u / OPEN_S, 0, 1);
    float r = opening ? (float)(int)(open * 236.0f) : 236.0f;
    if (r >= 236.0f) gfx_fill_rect(fb, 0, 0, GFX_W, GFX_H, C_WHITE);
    else if (r > 0.5f) gfx_fill_circle(fb, UI_CX, UI_CY, r, C_WHITE);
    float rr = r < R_RING ? r : R_RING;
    if (rr > 2.0f) gfx_ring(fb, UI_CX, UI_CY, rr, 2.0f, C_INK);

    /* after the disc is open: digits darken (3 steps), ghost + labels fade up (4) */
    float after = opening ? (u - OPEN_S) / (DONE_S - OPEN_S) : 1.0f;
    float ink = open < 1.0f ? 0.0f : (float)(int)(m_clamp(after, 0, 1) * 3.0f + 1.0f) / 3.0f;
    if (ink > 1.0f) ink = 1.0f;
    float fade = open < 1.0f ? 0.0f : steps4(after + 0.25f);
    uint16_t dcol = ui_mix(C_MID, C_INK, ink);
    uint16_t ghost = fade > 0.0f ? ui_mix(C_WHITE, C_GHOSTW, fade) : 0;
    dot_style st = { DOT_FONT_TALL, P, P * 0.8f, dcol, ghost, 1, 1 };
    /* a wake keeps the AOD digits (and their minute morph, if the minute turns now) */
    if (opening && !S.wake)
        dot_text_anim(fb, &st, UI_CX, Y_TIME, S.tm.to, 1, m_clamp(u / OPEN_S, 0, 1), DOT_REVEAL_SCAN, 0);
    else
        dot_text_morph(fb, &st, UI_CX, Y_TIME, S.tm.from, S.tm.to, 1, fk_morph_p(&S.tm, c->t, 0.27f));

    float la = opening ? steps4(after + 0.25f) * (open >= 1.0f) : 1.0f;
    if (la > 0.0f) {
        uint16_t lc = ui_mix(C_WHITE, C_LABW, la);
        fk_xfade_label(fb, &S.date, c->t, UI_CX, 140, lc, UI_CENTER);
        fk_status(fb, c, UI_CX, 166, lc, UI_CENTER);
        if (S.steps >= 10000) snprintf(b, sizeof b, "%d.%dK STEPS", S.steps / 1000, (S.steps % 1000) / 100);
        else snprintf(b, sizeof b, "%d STEPS", S.steps);
        ui_label(fb, UI_CX, 344, b, lc, UI_CENTER);
    }
    /* seconds: the red dot riding the hairline */
    float rad = opening ? 5.5f * steps4(after) : 5.5f;
    if (rad > 0.25f) {
        float as = ((float)c->dt.second + (float)c->dt.millis * 0.001f) / 60.0f * 6.2831853f;
        gfx_fill_circle(fb, UI_CX + fk_sin(as) * R_RING, UI_CY - fk_cos(as) * R_RING, rad, C_RED);
    }
    /* unread, y 48 (clear of the second dot): through the opening it is a mid-grey dot
     * like the digits (on black until the disc covers it), darkening with them */
    if (!opening) fk_unread(fb, c, C_INK, C_WHITE, 0, 24.0f);
    else {
        float lv = fk_unread_level(c);
        if (lv > 0.0f) {
            int on_white = r >= 191.0f;
            uint16_t bg = on_white ? C_WHITE : C_BG;
            uint16_t dc = on_white ? ui_mix(C_MID, C_INK, ink) : C_MID;
            dot_disc(fb, UI_CX, 48.0f, 8.0f, ui_mix(bg, dc, lv));
        }
    }
}

const struct nos_screen *face_inverted_hit(int x, int y, int r[4])
{
    if (y > 310 && y < 380 && x > 120 && x < 346) {
        if (r) { r[0] = 120; r[1] = 318; r[2] = 346; r[3] = 356; }
        return &scr_activity;
    }
    return 0;
}

static const struct nos_screen *tap(int x, int y) { return face_inverted_hit(x, y, 0); }

const nos_face face_inverted = { "INVERTED", enter, tick, draw, tap };
