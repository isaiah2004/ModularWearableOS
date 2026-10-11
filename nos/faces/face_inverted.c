/* face_inverted.c — INVERTED: the white Nothing phone, as a face.
 *
 * A full white disc with a black hairline ring at r = 220, black dot-matrix time on
 * one line with light-grey ghost cells, the date above and the steps below in mono
 * caps. The seconds are one red dot riding the hairline ring (gliding with millis).
 * AOD is dark, never white: the time as grey ring-dots on black and a dim hairline.
 *
 * Motion: build-in = the white disc opens from the centre (EASE_STANDARD) and the
 * digits print in with a column scan; minute change = dot_text_morph.
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
#define C_GHOSTW 0xEF5D                 /* #E8E8E8 unlit cells on white */
#define C_LABW  0x6B4D                  /* #6A6A6A labels on white */

static struct { fk_morph tm; int steps; } S;

static void enter(void) { S.tm.init = 0; }

static void tick(const face_ctx *c, float dt)
{
    fk_aod_update(c);
    char b[8];
    char hh[3], mm[3];
    fk_hh(hh, c->dt.hour); fk_mm(mm, c->dt.minute);
    snprintf(b, sizeof b, "%s:%s", hh, mm);
    if (c->enter < 1.0f) S.tm.init = 0;
    fk_morph_tick(&S.tm, b, c->t);
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
    char b[16];
    snprintf(b, sizeof b, "%s %d", ui_weekday3(c->dt.weekday), c->dt.day);
    ui_label(fb, UI_CX + (int)dx, 140 + (int)dy, b, C_DIS, UI_CENTER);
}

static void draw(gfx_fb *fb, const face_ctx *c)
{
    if (c->aod) { draw_aod(fb, c); return; }
    float e = c->enter;
    char b[24];
    float open = e >= 1.0f ? 1.0f : fk_ease(e / 0.55f);
    if (open >= 1.0f) gfx_fill_rect(fb, 0, 0, GFX_W, GFX_H, C_WHITE);
    else {
        float r = (float)(int)(open * 236.0f);
        if (r > 0.5f) gfx_fill_circle(fb, UI_CX, UI_CY, r, C_WHITE);
    }
    if (open >= 1.0f) gfx_ring(fb, UI_CX, UI_CY, R_RING, 2.0f, C_INK);

    dot_style st = { DOT_FONT_TALL, P, P * 0.8f, C_INK, C_GHOSTW, 1, 1 };
    if (e < 1.0f) {
        if (open > 0.6f)
            dot_text_anim(fb, &st, UI_CX, Y_TIME, S.tm.to, 1, m_clamp((e - 0.35f) / 0.55f, 0, 1),
                          DOT_REVEAL_SCAN, 0);
    } else
        dot_text_morph(fb, &st, UI_CX, Y_TIME, S.tm.from, S.tm.to, 1, fk_morph_p(&S.tm, c->t, 0.27f));

    if (open > 0.8f) {
        snprintf(b, sizeof b, "%s %d", ui_weekday3(c->dt.weekday), c->dt.day);
        ui_label(fb, UI_CX, 140, b, C_LABW, UI_CENTER);
        if (S.steps >= 10000) snprintf(b, sizeof b, "%d.%dK STEPS", S.steps / 1000, (S.steps % 1000) / 100);
        else snprintf(b, sizeof b, "%d STEPS", S.steps);
        ui_label(fb, UI_CX, 344, b, C_LABW, UI_CENTER);
    }
    /* seconds: the red dot riding the hairline */
    if (open >= 1.0f) {
        float d = e < 1.0f ? 11.0f * fk_stagger(e, 0.8f, 0.2f) : 11.0f;
        float as = ((float)c->dt.second + (float)c->dt.millis * 0.001f) / 60.0f * 6.2831853f;
        if (d > 0.5f) dot_disc(fb, UI_CX + fk_sin(as) * R_RING, UI_CY - fk_cos(as) * R_RING, d, C_RED);
    }
}

static const struct nos_screen *tap(int x, int y)
{
    return (y > 310 && y < 380 && x > 120 && x < 346) ? &scr_activity : 0;
}

const nos_face face_inverted = { "INVERTED", enter, tick, draw, tap };
