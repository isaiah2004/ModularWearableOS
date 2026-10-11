/* face_dot.c — DOT: the house dot-matrix clock.
 *
 * HH over MM in DOT_FONT_TALL on one lattice (pitch 17, dot 0.8 pitch), the unlit
 * cells of every digit box visible as ghost dots. The seconds are one red dot that
 * sits on the same lattice in the gap between the hours and the minutes and blinks
 * once a second. Date above; below, the battery only when it is low or charging.
 *
 * Motion: build-in = the dots of HH then MM sparkle in (random order) over the grid;
 * wake from AOD = the thin grey AOD dots grow and brighten into place (6 frames, no
 * blank frame); minute change = dot_text_morph over 8 frames; the date cross-fades at
 * midnight. 12 h: a lone hour digit is centred.
 * Cost: idle frames repaint only the red dot's 16 px box. */
#include "faces/face_kit.h"
#include "nos.h"
#include <stdio.h>

#define P      17.0f                 /* lattice pitch */
#define D      (P * 0.8f)            /* lit dot */
#define D_AOD  (P * 0.42f)
#define ROWS   9
#define Y_HH   (UI_CY - P * 0.5f - ROWS * P)
#define Y_MM   (UI_CY + P * 0.5f)
#define Y_DATE 50
#define Y_STAT 428
#define MORPH_S 0.27f
#define GHOST  0x18C3                /* #181818, neutral */

static struct {
    fk_morph hh, mm;
    fk_wake w;
    fk_xfade date;
} S;

static void enter(void) { S.hh.init = S.mm.init = 0; }

static void tick(const face_ctx *c, float dt)
{
    char hh[3], mm[3], db[16];
    fk_hh(hh, c->dt.hour);
    fk_mm(mm, c->dt.minute);
    fk_wake_tick(&S.w, c);
    float e = fk_build(&S.w, c);
    int stale = fk_wake_stale(&S.w);
    /* during the build-in the digits are drawn by the reveal, not the morph */
    if (e < 1.0f) { S.hh.init = S.mm.init = 0; }
    /* 12 h: a lone hour digit centres over the minutes (it morphs to "10" by gliding) */
    fk_morph_tick(&S.hh, hh[0] == ' ' ? hh + 1 : hh, c->t);
    fk_morph_tick(&S.mm, mm, c->t);
    if (stale) { fk_morph_settle(&S.hh); fk_morph_settle(&S.mm); }
    fk_date(db, c);
    fk_xfade_tick(&S.date, db, c->t, c->aod || stale || e < 1.0f);
    fk_aod_update(c);
    fk_unread_tick(c);
}

static void draw_date(gfx_fb *fb, const face_ctx *c, uint16_t col, float dx, float dy)
{
    fk_xfade_label(fb, &S.date, c->t, UI_CX + (int)dx, Y_DATE + (int)dy, col, UI_CENTER);
}

static void draw(gfx_fb *fb, const face_ctx *c)
{
    float dx, dy;
    fk_wake_off(&S.w, c, &dx, &dy);
    float k = fk_wake_k(&S.w, c);                       /* 0 AOD .. 1 awake */
    float e = fk_build(&S.w, c);
    if (c->aod || k < 1.0f) {
        /* AOD, and the wake morph out of it: thin grey dots grow into the awake grid */
        dot_style st = { DOT_FONT_TALL, P, D_AOD + (D - D_AOD) * k, ui_mix(C_SEC, C_WHITE, k),
                         k >= 0.5f ? ui_mix(C_BG, GHOST, (k - 0.25f) / 0.75f) : 0, 1, 1 };
        dot_text_a(fb, &st, UI_CX + dx, Y_HH + dy, S.hh.to, 1);
        dot_text_a(fb, &st, UI_CX + dx, Y_MM + dy, S.mm.to, 1);
        draw_date(fb, c, ui_mix(C_DIS, C_SEC, k), dx, dy);
        /* battery: a critical one shows in AOD too and brightens; otherwise it fades up */
        face_ctx ca = *c; ca.aod = 1;
        uint16_t sc = c->aod ? C_DIS : fk_status_shown(&ca) ? ui_mix(C_DIS, C_SEC, k) : ui_dim(C_SEC, k);
        fk_status(fb, c, UI_CX + (int)dx, Y_STAT + (int)dy, sc, UI_CENTER);
        fk_unread(fb, c, ui_mix(C_SEC, C_PRI, k), C_BG, dx, dy);
        return;
    }
    uint16_t lab = e >= 1.0f ? C_SEC : ui_dim(C_SEC, (float)(int)(fk_ease(e * 1.6f) * 4.0f) / 4.0f);
    draw_date(fb, c, lab, 0, 0);
    fk_status(fb, c, UI_CX, Y_STAT, lab, UI_CENTER);

    dot_style st = { DOT_FONT_TALL, P, D, C_WHITE, GHOST, 1, 1 };
    if (e < 1.0f) {
        dot_text_anim(fb, &st, UI_CX, Y_HH, S.hh.to, 1, m_clamp(e / 0.65f, 0, 1), DOT_REVEAL_RANDOM, 7);
        dot_text_anim(fb, &st, UI_CX, Y_MM, S.mm.to, 1, m_clamp((e - 0.2f) / 0.65f, 0, 1), DOT_REVEAL_RANDOM, 11);
    } else {
        dot_text_morph(fb, &st, UI_CX, Y_HH, S.hh.from, S.hh.to, 1, fk_morph_p(&S.hh, c->t, MORPH_S));
        dot_text_morph(fb, &st, UI_CX, Y_MM, S.mm.from, S.mm.to, 1, fk_morph_p(&S.mm, c->t, MORPH_S));
    }
    /* seconds: one red lattice dot between HH and MM */
    float d = e < 1.0f ? D * fk_stagger(e, 0.85f, 0.15f) : D * fk_blink(c->dt.millis);
    if (d > 0.5f) dot_disc(fb, UI_CX, UI_CY, d, fk_red(c, C_PRI));
    fk_unread(fb, c, C_PRI, C_BG, 0, 0);
}

const nos_face face_dot = { "DOT", enter, tick, draw, 0 };
