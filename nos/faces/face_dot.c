/* face_dot.c — DOT: the house dot-matrix clock.
 *
 * HH over MM in DOT_FONT_TALL on one lattice (pitch 18, dot 0.8 pitch), the unlit
 * cells of every digit box visible as ghost dots. The seconds are one red dot that
 * sits on the same lattice in the gap between the hours and the minutes and blinks
 * once a second. Date above, battery below in mono caps.
 *
 * Motion: build-in = the dots of HH then MM sparkle in (random order) over the grid;
 * minute change = dot_text_morph (old dots shrink, new dots grow) over 8 frames.
 * Cost: idle frames repaint only the red dot's 16 px box; a minute change repaints the
 * changed digit block for 8 frames. */
#include "faces/face_kit.h"
#include "nos.h"
#include <stdio.h>

#define P      18.0f                 /* lattice pitch */
#define D      (P * 0.8f)            /* lit dot */
#define ROWS   9
#define Y_HH   (UI_CY - P * 0.5f - ROWS * P)
#define Y_MM   (UI_CY + P * 0.5f)
#define MORPH_S 0.27f

static struct {
    fk_morph hh, mm;
    float t;
    float enter;
    int sec_ms;
} S;

static void enter(void) { S.hh.init = S.mm.init = 0; }

static void tick(const face_ctx *c, float dt)
{
    char hh[3], mm[3];
    fk_hh(hh, c->dt.hour);
    fk_mm(mm, c->dt.minute);
    S.t = c->t;
    /* during the build-in the digits are drawn by the reveal, not the morph */
    if (c->enter < 1.0f) { S.hh.init = S.mm.init = 0; }
    fk_morph_tick(&S.hh, hh, c->t);
    fk_morph_tick(&S.mm, mm, c->t);
    S.enter = c->enter;
    S.sec_ms = c->dt.millis;
    fk_aod_update(c);
}

/* the blinking seconds dot: grows in 100 ms at the top of each second, holds,
 * shrinks out from 500 ms. Size quantised to 4 steps so it costs 6 frames/s. */
static float sec_dot_d(int ms)
{
    float k;
    if (ms < 100) k = (float)(ms / 25 + 1) * 0.25f;
    else if (ms < 500) k = 1.0f;
    else if (ms < 600) k = 1.0f - (float)((ms - 500) / 25 + 1) * 0.25f;
    else k = 0.0f;
    return D * k;
}

static void draw_labels(gfx_fb *fb, const face_ctx *c, uint16_t col, int dx, int dy, int battery)
{
    char b[24];
    snprintf(b, sizeof b, "%s %d", ui_weekday3(c->dt.weekday), c->dt.day);
    ui_label(fb, UI_CX + dx, 48 + dy, b, col, UI_CENTER);
    if (!battery) return;
    if (c->battery > 0 || c->charging) {
        snprintf(b, sizeof b, c->charging ? "+%d%%" : "%d%%", c->battery);
        ui_label(fb, UI_CX + dx, 436 + dy, b, c->battery <= 15 && !c->charging ? C_PRI : col, UI_CENTER);
    }
}

static void draw_aod(gfx_fb *fb, const face_ctx *c)
{
    int dx = fk_aod_dx, dy = fk_aod_dy;
    char hh[3], mm[3];
    fk_hh(hh, c->dt.hour);
    fk_mm(mm, c->dt.minute);
    dot_style st = { DOT_FONT_TALL, P, P * 0.42f, C_SEC, 0, 1, 1 };
    dot_text_a(fb, &st, UI_CX + dx, Y_HH + dy, hh, 1);
    dot_text_a(fb, &st, UI_CX + dx, Y_MM + dy, mm, 1);
    draw_labels(fb, c, C_DIS, dx, dy, 0);
}

static void draw(gfx_fb *fb, const face_ctx *c)
{
    if (c->aod) { draw_aod(fb, c); return; }
    float e = c->enter;
    uint16_t lab = e >= 1.0f ? C_SEC : ui_dim(C_SEC, (float)(int)(fk_ease(e * 1.6f) * 4.0f) / 4.0f);
    draw_labels(fb, c, lab, 0, 0, 1);

    dot_style st = { DOT_FONT_TALL, P, D, C_WHITE, C_GHOST, 1, 1 };
    if (e < 1.0f) {
        char hh[3], mm[3];
        fk_hh(hh, c->dt.hour);
        fk_mm(mm, c->dt.minute);
        dot_text_anim(fb, &st, UI_CX, Y_HH, hh, 1, m_clamp(e / 0.65f, 0, 1), DOT_REVEAL_RANDOM, 7);
        dot_text_anim(fb, &st, UI_CX, Y_MM, mm, 1, m_clamp((e - 0.2f) / 0.65f, 0, 1), DOT_REVEAL_RANDOM, 11);
    } else {
        dot_text_morph(fb, &st, UI_CX, Y_HH, S.hh.from, S.hh.to, 1, fk_morph_p(&S.hh, S.t, MORPH_S));
        dot_text_morph(fb, &st, UI_CX, Y_MM, S.mm.from, S.mm.to, 1, fk_morph_p(&S.mm, S.t, MORPH_S));
    }
    /* seconds: one red lattice dot between HH and MM */
    float d = e < 1.0f ? D * fk_stagger(e, 0.85f, 0.15f) : sec_dot_d(c->dt.millis);
    if (d > 0.5f) dot_disc(fb, UI_CX, UI_CY, d, C_RED);
}

const nos_face face_dot = { "DOT", enter, tick, draw, 0 };
